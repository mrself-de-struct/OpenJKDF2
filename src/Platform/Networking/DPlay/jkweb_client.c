// "OpenJKDF2 Web Lobby" transport: play with the browser build of jkdf2mp
// (github.com/tristamus/OpenJKDF2, lobby: github.com/tristamus/jkdf2mp-bb-server).
//
// Speaks that lobby's WebSocket protocol (ws(s)://<lobby>/relay):
//   text frames:   list <game> | host <entry> | update <entry> | join <room>
//                  welcome <id> <entry> | leave | ping
//                  hello <game> <name>                            (us -> lobby)
//                  list (<room> <players> <entry>)* | hosted <room>
//                  peer-joined <id> | peer-left <id> | welcome <id> <entry>
//                  host-left | error <code> | signal .. | ice ..  (lobby -> us)
//   binary frames: u32 idFrom, u32 idTo, game message
// <game> is jk1 or mots: list returns only that game's games (and those of
// hosts that never said), hello gives the lobby our game and player name
// (base64 UTF-8) for the landing page's live counts. The lobby ends a
// connection that sends nothing for 2 minutes; we ping every 25 s.
// <entry> is a hex-encoded jkMultiEntry (240 bytes, packed). The host is id 1,
// joiners get 2..64; a frame goes to the player named in idTo (to the host
// for 0 or 1).
//
// Like the browser build, a joiner opens a direct WebRTC data channel to the
// host (jkweb_rtc.c, when built with WebRTC) and game packets use it once it's
// up; the host forwards packets between joiners. Until then, or if a direct
// link can't be made, packets go through the lobby.
//
// The browser build uses OpenJKDF2's own message layouts, not JK.EXE's, so
// stdComm_dplay.c sets stdComm_bOpenJkdf2Wire while this transport is active.
//
// Map and mod transfers ("JKMT" frames) go to jkweb_xfer.c, over the
// "jk-map" channel when it's open, else through the lobby: a joiner
// downloads the host's custom episode and mod, a host serves its own.
//
// WebSocket is implemented here on top of libcurl's CONNECT_ONLY mode, which
// gives a plain or TLS (wss) stream with the curl OpenJKDF2 already uses.

#if defined(PLATFORM_DPLAY) && defined(PLATFORM_CURL)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#define strtok_r strtok_s
#else
#include <poll.h>
#include <sys/select.h>
#endif

#ifdef LINUX
#include <curl/curl.h>
#else
#include "curl/curl.h"
// Platform/Common/stdEmbeddedRes.h (not included: it pulls in the engine's
// Windows type stand-ins, which clash with <winsock2.h>)
char* stdEmbeddedRes_Load(const char* filepath, size_t* pOutSz);
#endif

#define DPS_BACKEND_PREFIX jkweb
#include "dplay_backend.h"
#include "jkweb_rtc.h"
#include "jkmod.h"
#include "jkweb_xfer.h"

#define JKW_HR_OK            0
#define JKW_HR_GENERIC       ((long)0x80004005)
#define JKW_HR_UNAVAILABLE   ((long)0x887700FA)
#define JKW_HR_NOCONNECTION  ((long)0x887700AA)
#define JKW_HR_NOSESSIONS    ((long)0x88770168)
#define JKW_HR_ACCESSDENIED  ((long)0x88770014)
#define JKW_HR_INVALIDPW     ((long)0x88770154)
#define JKW_HR_GAMEFULL      ((long)0x8877014A)

#define JKW_CONNECT_MS     5000
#define JKW_REPLY_MS       5000
#define JKW_LIST_MS        500
#define JKW_RETRY_MS       15000    // after a failed connect, don't retry on every menu refresh
#define JKW_PING_MS        25000    // the lobby and proxies drop idle WebSockets
#define JKW_MAX_ROOMS      32
#define JKW_MAX_FRAME      (64 * 1024)
#define JKW_FIRST_ID       2
#define JKW_LAST_ID        64

// jkMultiEntry (types.h, #pragma pack(1)), as the browser build sends it.
#define JKE_SIZE          240
#define JKE_MAXPLAYERS    16
#define JKE_NUMPLAYERS    20
#define JKE_SERVERNAME    24    // char16_t[32]
#define JKE_EPISODE       88    // char[32]
#define JKE_MAP           120   // char[32]
#define JKE_PASSWORD      152   // char16_t[32]
#define JKE_SESSIONFLAGS  216
#define JKE_CHECKSUMSEED  220
#define JKE_FIELD_E0      224
#define JKE_MULTIMODE     228
#define JKE_TICKRATE      232
#define JKE_MAXRANK       236

typedef struct jkw_event
{
    struct jkw_event* next;
    int        type;
    uint32_t   from, size;
    dps_sysmsg sys;
    uint8_t    data[];
} jkw_event;

static struct
{
    int      started, log;
    uint8_t  appGuid[16];
    char     url[512];
    uint64_t startMs;

    // connection
    CURL*    curl;
    curl_socket_t sock;
    int      open;
    uint64_t lastFailMs, lastTxMs;
    uint8_t* rx;
    size_t   rxLen, rxCap;
    uint8_t* msg;            // fragmented message being assembled
    size_t   msgLen;
    int      msgOp;
    void*    caBlob;

    // lobby list
    int      numRooms;
    int      roomIds[JKW_MAX_ROOMS];
    char     roomTails[JKW_MAX_ROOMS][JKMOD_MAX_TAIL_HEX + 1];   // host's mod ("" = none)
    char     hostTail[JKMOD_MAX_TAIL_HEX + 1];                    // ours, when hosting
    char     joinedTail[JKMOD_MAX_TAIL_HEX + 1];
    uint8_t  roomEntries[JKW_MAX_ROOMS][JKE_SIZE];
    int      roomPlayers[JKW_MAX_ROOMS];
    int      gotList;

    // session
    int      inSession, isHost, lostSignalled;
    int      hostPending, hostedRoom;
    int      joining, joined, joinError;
    uint32_t myId;
    uint8_t  entry[JKE_SIZE];     // host: what we advertise; client: the host's welcome
    dps_session desc;
    uint8_t  peers[JKW_LAST_ID + 1];

    jkw_event* evHead;
    jkw_event* evTail;
    jkw_event* evLast;
} W;

// ---------------------------------------------------------------------------

static uint64_t jkw_NowMs(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void jkw_Log(int always, const char* fmt, ...)
{
    va_list ap;
    if (!always && !W.log) return;
    va_start(ap, fmt);
    fprintf(stderr, "[jkweb %8.3f] ", (double)(jkw_NowMs() - W.startMs) / 1000.0);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t jkw_Rand(void)
{
    static uint32_t s = 0;
    if (!s) s = (uint32_t)time(NULL) ^ (uint32_t)jkw_NowMs() * 2654435761u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static void jkw_Push(int type, uint32_t from, const void* data, uint32_t size, int kind, uint32_t player)
{
    jkw_event* e = (jkw_event*)malloc(sizeof(jkw_event) + size);
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->from = from;
    e->size = size;
    if (size) memcpy(e->data, data, size);
    e->sys.kind = kind;
    e->sys.playerId = player;
    if (W.evTail) W.evTail->next = e; else W.evHead = e;
    W.evTail = e;
}

static void jkw_FreeEvents(void)
{
    while (W.evHead) { jkw_event* e = W.evHead; W.evHead = e->next; free(e); }
    W.evTail = NULL;
}

// ---------------------------------------------------------------------------
// jkMultiEntry <-> dps_session (same mapping stdComm_dplay.c uses for DirectPlay)
// ---------------------------------------------------------------------------

static void jkw_EntryToSession(const uint8_t* e, int room, int players, dps_session* s)
{
    size_t n = 0;
    memset(s, 0, sizeof(*s));
    wr32(s->guidInstance, 0x4A4B5753);           // "JKWS" + room, like the browser build
    wr32(s->guidInstance + 4, (uint32_t)room);
    s->flags = rd32(e + JKE_SESSIONFLAGS);
    s->maxPlayers = rd32(e + JKE_MAXPLAYERS);
    s->curPlayers = players >= 0 ? (uint32_t)players : rd32(e + JKE_NUMPLAYERS);

    // "<server>:<episode>:<map>", what the glue expects
    for (int i = 0; i < 31 && n < DPS_NAME_MAX - 1; i++)
    {
        uint16_t c = (uint16_t)(e[JKE_SERVERNAME + 2 * i] | (e[JKE_SERVERNAME + 2 * i + 1] << 8));
        if (!c) break;
        s->name[n++] = c;
    }
    s->name[n++] = ':';
    for (int i = 0; i < 31 && e[JKE_EPISODE + i] && n < DPS_NAME_MAX - 1; i++) s->name[n++] = e[JKE_EPISODE + i];
    s->name[n++] = ':';
    for (int i = 0; i < 31 && e[JKE_MAP + i] && n < DPS_NAME_MAX - 1; i++) s->name[n++] = e[JKE_MAP + i];
    s->name[n] = 0;

    for (int i = 0; i < 31 && i < DPS_PASSWORD_MAX - 1; i++)
    {
        uint16_t c = (uint16_t)(e[JKE_PASSWORD + 2 * i] | (e[JKE_PASSWORD + 2 * i + 1] << 8));
        if (!c) break;
        s->password[i] = c;
    }
    s->user[0] = rd32(e + JKE_CHECKSUMSEED);
    s->user[1] = rd32(e + JKE_MULTIMODE);
    s->user[2] = (rd32(e + JKE_FIELD_E0) & 0xFFFF) | (rd32(e + JKE_MAXRANK) << 16);
    s->user[3] = rd32(e + JKE_TICKRATE);
}

static void jkw_SessionToEntry(const dps_session* s, uint8_t* e)
{
    size_t len = 0, cEp, cMap;
    memset(e, 0, JKE_SIZE);
    while (len < DPS_NAME_MAX && s->name[len]) len++;
    cMap = cEp = (size_t)-1;
    for (size_t i = len; i-- > 0; )
    {
        if (s->name[i] != ':') continue;
        if (cMap == (size_t)-1) cMap = i; else { cEp = i; break; }
    }
    if (cEp == (size_t)-1) { cEp = len; cMap = len; }
    for (size_t i = 0; i < cEp && i < 31; i++) { e[JKE_SERVERNAME + 2 * i] = (uint8_t)s->name[i]; e[JKE_SERVERNAME + 2 * i + 1] = (uint8_t)(s->name[i] >> 8); }
    for (size_t i = cEp + 1, k = 0; i < cMap && k < 31; i++, k++) e[JKE_EPISODE + k] = (uint8_t)s->name[i];
    for (size_t i = cMap + 1, k = 0; i < len && k < 31; i++, k++) e[JKE_MAP + k] = (uint8_t)s->name[i];
    for (int i = 0; i < 31 && i < DPS_PASSWORD_MAX && s->password[i]; i++) { e[JKE_PASSWORD + 2 * i] = (uint8_t)s->password[i]; e[JKE_PASSWORD + 2 * i + 1] = (uint8_t)(s->password[i] >> 8); }
    wr32(e + JKE_MAXPLAYERS, s->maxPlayers);
    wr32(e + JKE_NUMPLAYERS, 1);
    wr32(e + JKE_SESSIONFLAGS, s->flags & ~0xC0u);     // DirectPlay's KEEPALIVE|NODATAMESSAGES mean nothing here
    wr32(e + JKE_CHECKSUMSEED, s->user[0]);
    wr32(e + JKE_FIELD_E0, s->user[2] & 0xFFFF);
    wr32(e + JKE_MAXRANK, s->user[2] >> 16);
    wr32(e + JKE_MULTIMODE, s->user[1]);
    wr32(e + JKE_TICKRATE, s->user[3]);
}

static void jkw_ToHex(const uint8_t* p, size_t n, char* out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[p[i] >> 4]; out[2 * i + 1] = hx[p[i] & 15]; }
    out[2 * n] = 0;
}

static int jkw_Nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int jkw_FromHex(const char* hex, uint8_t* out, size_t n)
{
    if (!hex || strlen(hex) < 2 * n) return 0;
    for (size_t i = 0; i < n; i++)
    {
        int a = jkw_Nib(hex[2 * i]), b = jkw_Nib(hex[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 1;
}

static const char jkw_kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t jkw_B64Encode(const uint8_t* in, size_t n, char* out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = jkw_kB64[(v >> 18) & 63];
        out[o++] = jkw_kB64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? jkw_kB64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? jkw_kB64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static size_t jkw_B64Decode(const char* in, uint8_t* out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in && *in != '='; in++)
    {
        const char* p = strchr(jkw_kB64, *in);
        if (!p) continue;
        acc = acc << 6 | (uint32_t)(p - jkw_kB64);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (o + 1 < cap) out[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (cap) out[o] = 0;
    return o;
}

// ---------------------------------------------------------------------------
// WebSocket over libcurl CONNECT_ONLY
// ---------------------------------------------------------------------------

static int jkw_WaitSock(int forWrite, int ms)
{
#ifdef _WIN32
    fd_set s;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    FD_ZERO(&s);
    FD_SET(W.sock, &s);
    return select(0, forWrite ? NULL : &s, forWrite ? &s : NULL, NULL, &tv) > 0;
#else
    struct pollfd p = { (int)W.sock, (short)(forWrite ? POLLOUT : POLLIN), 0 };
    return poll(&p, 1, ms) > 0;
#endif
}

static void jkw_Disconnect(const char* why)
{
    if (W.curl)
    {
        if (W.open) jkw_Log(0, "lobby connection closed (%s)\n", why);
        curl_easy_cleanup(W.curl);
    }
    W.curl = NULL;
    W.open = 0;
    W.rxLen = W.msgLen = 0;
    if (W.inSession && !W.lostSignalled)
    {
        W.lostSignalled = 1;
        jkw_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_SESSIONLOST, 0);
    }
}

static int jkw_SendRaw(const uint8_t* p, size_t n)
{
    uint64_t start = jkw_NowMs();
    while (n)
    {
        size_t sent = 0;
        CURLcode rc = curl_easy_send(W.curl, p, n, &sent);
        if (rc == CURLE_AGAIN)
        {
            if (jkw_NowMs() - start > 3000) { jkw_Disconnect("send timeout"); return -1; }
            jkw_WaitSock(1, 50);
            continue;
        }
        if (rc != CURLE_OK) { jkw_Disconnect(curl_easy_strerror(rc)); return -1; }
        p += sent;
        n -= sent;
    }
    W.lastTxMs = jkw_NowMs();
    return 0;
}

// Client frames must be masked (RFC 6455).
static int jkw_SendFrame(int opcode, const void* data, size_t len)
{
    uint8_t hdr[14];
    size_t h = 0;
    uint8_t mask[4];
    uint8_t* buf;
    int ret;

    if (!W.open) return -1;
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) hdr[h++] = (uint8_t)(0x80 | len);
    else if (len < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (uint8_t)(len >> 8); hdr[h++] = (uint8_t)len; }
    else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)len >> (8 * i)); }
    wr32(mask, jkw_Rand());
    memcpy(hdr + h, mask, 4);
    h += 4;

    buf = (uint8_t*)malloc(h + len);
    if (!buf) return -1;
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < len; i++) buf[h + i] = ((const uint8_t*)data)[i] ^ mask[i & 3];
    ret = jkw_SendRaw(buf, h + len);
    free(buf);
    return ret;
}

static int jkw_SendText(const char* text)
{
    jkw_Log(0, "<< %.100s\n", text);
    return jkw_SendFrame(0x1, text, strlen(text));
}

static void jkw_HandleText(char* text);
static void jkw_HandleBinary(const uint8_t* p, size_t n);
static void jkw_HandleDirect(int peer, uint8_t* p, size_t n);
static void jkw_LogSaberInfo(const char* dir, uint32_t from, uint32_t to, const uint8_t* m, size_t n);

static void jkw_HandleMessage(int op, uint8_t* p, size_t n)
{
    if (op == 0x1)
    {
        char* t = (char*)malloc(n + 1);
        if (!t) return;
        memcpy(t, p, n);
        t[n] = 0;
        jkw_HandleText(t);
        free(t);
    }
    else if (op == 0x2)
        jkw_HandleBinary(p, n);
}

// Parses complete frames out of W.rx.
static void jkw_ParseFrames(void)
{
    size_t off = 0;
    while (W.open)
    {
        uint8_t* p = W.rx + off;
        size_t avail = W.rxLen - off, h = 2;
        uint64_t len;
        int fin, op, masked;
        if (avail < 2) break;
        fin = p[0] & 0x80;
        op = p[0] & 0x0F;
        masked = p[1] & 0x80;
        len = p[1] & 0x7F;
        if (len == 126) { if (avail < 4) break; len = (uint64_t)(p[2] << 8 | p[3]); h = 4; }
        else if (len == 127) { if (avail < 10) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i]; h = 10; }
        if (masked) h += 4;
        if (len > JKW_MAX_FRAME) { jkw_Disconnect("frame too big"); return; }
        if (avail < h + len) break;
        if (masked)
            for (uint64_t i = 0; i < len; i++) p[h + i] ^= p[h - 4 + (i & 3)];

        uint8_t* payload = p + h;
        if (op == 0x8) { jkw_SendFrame(0x8, payload, len >= 2 ? 2 : 0); jkw_Disconnect("closed by lobby"); return; }
        else if (op == 0x9) jkw_SendFrame(0xA, payload, (size_t)len);
        else if (op == 0xA) { }
        else if (op == 0x0 || op == 0x1 || op == 0x2)
        {
            if (op != 0x0) { W.msgOp = op; W.msgLen = 0; }
            if (!fin || W.msgLen)
            {
                uint8_t* nm = (uint8_t*)realloc(W.msg, W.msgLen + len);
                if (!nm) { jkw_Disconnect("out of memory"); return; }
                W.msg = nm;
                memcpy(W.msg + W.msgLen, payload, len);
                W.msgLen += len;
                if (fin) { jkw_HandleMessage(W.msgOp, W.msg, W.msgLen); W.msgLen = 0; }
            }
            else
                jkw_HandleMessage(op, payload, (size_t)len);
        }
        off += h + len;
    }
    if (off && W.open)
    {
        memmove(W.rx, W.rx + off, W.rxLen - off);
        W.rxLen -= off;
    }
}

// Reads whatever is waiting (optionally waiting up to ms for something).
static void jkw_Pump(int ms)
{
    if (!W.open) return;
    if (ms > 0) jkw_WaitSock(0, ms);
    for (;;)
    {
        size_t got = 0;
        CURLcode rc;
        if (W.rxCap - W.rxLen < 16384)
        {
            size_t nc = W.rxCap ? W.rxCap * 2 : 65536;
            uint8_t* nb = (uint8_t*)realloc(W.rx, nc);
            if (!nb) { jkw_Disconnect("out of memory"); return; }
            W.rx = nb;
            W.rxCap = nc;
        }
        rc = curl_easy_recv(W.curl, W.rx + W.rxLen, W.rxCap - W.rxLen, &got);
        if (rc == CURLE_AGAIN) break;
        if (rc != CURLE_OK || got == 0) { jkw_ParseFrames(); jkw_Disconnect(rc == CURLE_OK ? "closed" : curl_easy_strerror(rc)); return; }
        W.rxLen += got;
        jkw_ParseFrames();
        if (!W.open) return;
    }
    if (W.open && jkw_NowMs() - W.lastTxMs > JKW_PING_MS)
        jkw_SendText("ping");

    // WebRTC: signaling to send through the lobby, packets from direct links.
    {
        static char json[16384];
        static char line[24000];
        static uint8_t pkt[8192];
        int to, from, len;
        while (W.open && (len = jkrtc_PollSignal(&to, json, sizeof(json))) >= 0)
        {
            int k = snprintf(line, sizeof(line), "signal %d ", to);
            jkw_B64Encode((const uint8_t*)json, (size_t)len, line + k);
            jkw_SendFrame(0x1, line, strlen(line));
        }
        while ((len = jkrtc_PollPacket(&from, pkt, sizeof(pkt))) >= 0)
            jkw_HandleDirect(from, pkt, (size_t)len);
    }
    {
        static uint8_t frame[JKX_MAX_FRAME];
        int from, len;
        while ((len = jkrtc_PollMapFrame(&from, frame, sizeof(frame))) != -1)
            if (len >= 0 && W.inSession) jkx_OnFrame((uint32_t)from, frame, (size_t)len, JKX_VIA_DIRECT);
        if (W.inSession && W.isHost) jkx_HostPump();
    }
}

// "wss://host[:port][/path]", "ws://...", "https://..." or a bare host
static int jkw_ParseUrl(char* scheme, char* hostport, size_t hpLen, char* path, size_t pathLen, int bareTls)
{
    const char* u = W.url;
    const char* s;
    int tls = bareTls;          // no scheme given: wss, unless that already failed
    if (!strncmp(u, "wss://", 6)) { u += 6; }
    else if (!strncmp(u, "https://", 8)) { u += 8; }
    else if (!strncmp(u, "ws://", 5)) { u += 5; tls = 0; }
    else if (!strncmp(u, "http://", 7)) { u += 7; tls = 0; }
    s = strchr(u, '/');
    if (!s) s = u + strlen(u);
    if ((size_t)(s - u) >= hpLen || s == u) return 0;
    memcpy(hostport, u, (size_t)(s - u));
    hostport[s - u] = 0;
    snprintf(path, pathLen, "%s", (*s && strcmp(s, "/")) ? s : "/relay");
    strcpy(scheme, tls ? "https" : "http");
    return 1;
}

// Who we are, for the lobby's live counts and Join lists (outside W: it
// outlives dps_Startup). game is "jk1" or "mots"; name is base64 UTF-8.
static char jkw_presGame[8];
static char jkw_presName[64];
static char jkw_helloSent[96];

// "hello <game> <name>" once per connection and whenever it changes, as the
// browser build does: the lobby counts and names us, and leaves the other
// game's games out of our list. Skipped until we know a name.
static void jkw_SayHello(void)
{
    char line[sizeof(jkw_helloSent)];
    if (!W.open || !jkw_presGame[0] || !jkw_presName[0]) return;
    snprintf(line, sizeof(line), "hello %s %s", jkw_presGame, jkw_presName);
    if (!strcmp(line, jkw_helloSent)) return;
    snprintf(jkw_helloSent, sizeof(jkw_helloSent), "%s", line);
    jkw_SendText(line);
}

static int jkw_ConnectOnce(int bareTls);

// A bare "host[:port]" means wss://; if TLS fails there, try plain ws:// (a
// relay.py on the LAN).
static int jkw_Connect(void)
{
    int hasScheme = strstr(W.url, "://") != NULL;
    if (W.open) return 1;
    if (W.lastFailMs && jkw_NowMs() - W.lastFailMs < JKW_RETRY_MS) return 0;
    if (jkw_ConnectOnce(1)) return 1;
    if (!hasScheme && W.url[0])
    {
        W.lastFailMs = 0;
        if (jkw_ConnectOnce(0)) return 1;
    }
    return 0;
}

static int jkw_ConnectOnce(int bareTls)
{
    char scheme[8], hostport[300], path[256], req[1024], key[25], url[400];
    uint8_t nonce[16];
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    CURLcode rc;
    uint64_t start;

    if (W.open) return 1;
    if (!W.url[0]) { jkw_Log(1, "no web lobby set: type its address (e.g. wss://example.com/relay) in the join screen's address box\n"); return 0; }
    if (W.lastFailMs && jkw_NowMs() - W.lastFailMs < JKW_RETRY_MS) return 0;
    if (!jkw_ParseUrl(scheme, hostport, sizeof(hostport), path, sizeof(path), bareTls)) { jkw_Log(1, "bad web lobby address '%s'\n", W.url); return 0; }

    W.curl = curl_easy_init();
    if (!W.curl) return 0;
    snprintf(url, sizeof(url), "%s://%s/", scheme, hostport);
    curl_easy_setopt(W.curl, CURLOPT_URL, url);
    curl_easy_setopt(W.curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(W.curl, CURLOPT_CONNECTTIMEOUT_MS, (long)JKW_CONNECT_MS);
    curl_easy_setopt(W.curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(W.curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(W.curl, CURLOPT_NOSIGNAL, 1L);
    // Altered: our WebSocket handshake is HTTP/1.1 sent by hand. The lobby's host (Cloudflare)
    // agrees to HTTP/2 when offered, and curl 8.23 offers it on a CONNECT_ONLY connection:
    // our bytes then went into curl's HTTP/2 handling and crashed in it (2026-10-27).
    curl_easy_setopt(W.curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
#ifndef LINUX
    if (W.caBlob)
    {
        struct curl_blob blob = { W.caBlob, strlen((char*)W.caBlob), CURL_BLOB_COPY };
        curl_easy_setopt(W.curl, CURLOPT_CAINFO_BLOB, &blob);
    }
#endif
    rc = curl_easy_perform(W.curl);
    if (rc != CURLE_OK)
    {
        jkw_Log(1, "can't reach web lobby %s (%s): %s\n", W.url, scheme, curl_easy_strerror(rc));
        curl_easy_cleanup(W.curl);
        W.curl = NULL;
        W.lastFailMs = jkw_NowMs();
        return 0;
    }
    curl_easy_getinfo(W.curl, CURLINFO_ACTIVESOCKET, &W.sock);
    W.open = 1;
    W.rxLen = 0;

    // HTTP upgrade
    for (int i = 0; i < 16; i += 4) wr32(nonce + i, jkw_Rand());
    for (int i = 0, k = 0; i < 16; i += 3)
    {
        uint32_t v = (uint32_t)nonce[i] << 16 | (i + 1 < 16 ? (uint32_t)nonce[i + 1] << 8 : 0) | (i + 2 < 16 ? nonce[i + 2] : 0);
        key[k++] = b64[(v >> 18) & 63];
        key[k++] = b64[(v >> 12) & 63];
        key[k++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        key[k++] = i + 2 < 16 ? b64[v & 63] : '=';
        key[k] = 0;
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: OpenJKDF2\r\n\r\n",
             path, hostport, key);
    if (jkw_SendRaw((const uint8_t*)req, strlen(req)) < 0) { W.lastFailMs = jkw_NowMs(); return 0; }

    start = jkw_NowMs();
    for (;;)
    {
        char* end;
        size_t got = 0;
        if (W.rxCap - W.rxLen < 4096)
        {
            size_t nc = W.rxCap ? W.rxCap * 2 : 65536;
            uint8_t* nb = (uint8_t*)realloc(W.rx, nc);
            if (!nb) { jkw_Disconnect("out of memory"); return 0; }
            W.rx = nb;
            W.rxCap = nc;
        }
        rc = curl_easy_recv(W.curl, W.rx + W.rxLen, W.rxCap - W.rxLen - 1, &got);
        if (rc == CURLE_OK && got) W.rxLen += got;
        else if (rc != CURLE_AGAIN) { jkw_Disconnect("handshake failed"); W.lastFailMs = jkw_NowMs(); return 0; }
        W.rx[W.rxLen] = 0;
        end = strstr((char*)W.rx, "\r\n\r\n");
        if (end)
        {
            size_t hdrLen = (size_t)(end - (char*)W.rx) + 4;
            if (strncmp((char*)W.rx, "HTTP/1.1 101", 12))
            {
                char* eol = strstr((char*)W.rx, "\r\n");
                if (eol) *eol = 0;
                jkw_Log(1, "web lobby %s refused the WebSocket (%s); is the address right? (usually wss://<site>/relay)\n", W.url, (char*)W.rx);
                jkw_Disconnect("upgrade refused");
                W.lastFailMs = jkw_NowMs();
                return 0;
            }
            memmove(W.rx, W.rx + hdrLen, W.rxLen - hdrLen);
            W.rxLen -= hdrLen;
            break;
        }
        if (jkw_NowMs() - start > JKW_CONNECT_MS) { jkw_Disconnect("handshake timeout"); W.lastFailMs = jkw_NowMs(); return 0; }
        jkw_WaitSock(0, 50);
    }
    W.lastFailMs = 0;
    W.lastTxMs = jkw_NowMs();
    jkw_Log(1, "connected to web lobby %s\n", W.url);
    jkw_helloSent[0] = 0;
    jkw_SayHello();
    jkw_ParseFrames();
    return 1;
}

// ---------------------------------------------------------------------------
// Lobby messages
// ---------------------------------------------------------------------------


// 'players' -1: the host and every player in the room.
static void jkw_SendEntryN(const char* op, int id, int players)
{
    char hex[2 * JKE_SIZE + 1], line[2 * JKE_SIZE + JKMOD_MAX_TAIL_HEX + 64];
    if (players < 0)
    {
        players = 1;
        for (int i = JKW_FIRST_ID; i <= JKW_LAST_ID; i++) players += W.peers[i];
    }
    wr32(W.entry + JKE_NUMPLAYERS, (uint32_t)players);
    jkw_ToHex(W.entry, JKE_SIZE, hex);
    // A host running a mod adds a tail naming it (jkmod.c), as the browser build does.
    if (id >= 0) snprintf(line, sizeof(line), "%s %d %s%s", op, id, hex, W.hostTail);
    else snprintf(line, sizeof(line), "%s %s%s", op, hex, W.hostTail);
    jkw_SendText(line);
}

static void jkw_SendEntry(const char* op, int id)
{
    jkw_SendEntryN(op, id, -1);
}

static void jkw_HandleText(char* t)
{
    char* save = NULL;
    char* op = strtok_r(t, " ", &save);
    if (!op) return;
    if (strcmp(op, "list") && strcmp(op, "signal") && strcmp(op, "ice") && strcmp(op, "pong"))
        jkw_Log(0, ">> %s %.80s\n", op, save ? save : "");

    if (!strcmp(op, "list"))
    {
        int n = 0;
        char *room, *players, *hex;
        while ((room = strtok_r(NULL, " ", &save)) && (players = strtok_r(NULL, " ", &save)) && (hex = strtok_r(NULL, " ", &save)))
        {
            if (n >= JKW_MAX_ROOMS) break;
            if (!jkw_FromHex(hex, W.roomEntries[n], JKE_SIZE)) continue;
            snprintf(W.roomTails[n], sizeof(W.roomTails[n]), "%s", strlen(hex) > 2 * JKE_SIZE ? hex + 2 * JKE_SIZE : "");
            W.roomIds[n] = atoi(room);
            W.roomPlayers[n] = atoi(players);
            n++;
        }
        W.numRooms = n;
        W.gotList = 1;
    }
    else if (!strcmp(op, "hosted"))
    {
        char* room = strtok_r(NULL, " ", &save);
        if (W.hostPending && room) { W.hostedRoom = atoi(room); W.hostPending = 0; }
    }
    else if (!strcmp(op, "peer-joined"))
    {
        char* id = strtok_r(NULL, " ", &save);
        int n = id ? atoi(id) : 0;
        if (W.isHost && n >= JKW_FIRST_ID && n <= JKW_LAST_ID)
        {
            // Added (as tristamus/OpenJKDF2 9a42643c): the welcome counts the players already
            // there, not the one arriving. With no room left it says the game is full (players
            // at the limit) and the joiner leaves before loading the level, instead of loading
            // it all for sithMulti's "That game is full" (two joining for the last place too).
            int players = 1, maxPlayers = (int)rd32(W.entry + JKE_MAXPLAYERS);
            for (int i = JKW_FIRST_ID; i <= JKW_LAST_ID; i++) players += (i != n) && W.peers[i];
            if (maxPlayers > 0 && players >= maxPlayers)
            {
                W.peers[n] = 0;
                jkw_SendEntryN("welcome", n, maxPlayers);
                jkw_Log(1, "player %d turned away: the game is full\n", n);
            }
            else
            {
                W.peers[n] = 0;
                jkw_SendEntryN("welcome", n, players);   // gives them their id and the game's settings
                W.peers[n] = 1;
                jkw_SendEntry("update", -1);
                jkw_Log(1, "player %d joined through the web lobby\n", n);
            }
        }
    }
    else if (!strcmp(op, "peer-left"))
    {
        char* id = strtok_r(NULL, " ", &save);
        int n = id ? atoi(id) : 0;
        if (W.isHost && n >= JKW_FIRST_ID && n <= JKW_LAST_ID && W.peers[n])
        {
            W.peers[n] = 0;
            jkrtc_ClosePeer(n);
            jkw_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_PLAYERDESTROYED, (uint32_t)n);
            jkw_SendEntry("update", -1);
            jkw_Log(1, "player %d left\n", n);
        }
    }
    else if (!strcmp(op, "welcome"))
    {
        char* id = strtok_r(NULL, " ", &save);
        char* hex = strtok_r(NULL, " ", &save);
        uint8_t e[JKE_SIZE];
        if (W.joining && id && jkw_FromHex(hex, e, JKE_SIZE))
        {
            W.myId = (uint32_t)atoi(id);
            memcpy(W.entry, e, JKE_SIZE);
            snprintf(W.joinedTail, sizeof(W.joinedTail), "%s", strlen(hex) > 2 * JKE_SIZE ? hex + 2 * JKE_SIZE : "");
            wr32(W.entry + JKE_FIELD_E0, 10);   // as the browser build does
            W.joining = 0;
            W.joined = 1;
        }
    }
    else if (!strcmp(op, "host-left"))
    {
        if (W.inSession && !W.isHost && !W.lostSignalled)
        {
            W.lostSignalled = 1;
            jkw_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_SESSIONLOST, 0);
        }
    }
    else if (!strcmp(op, "error"))
    {
        char* code = strtok_r(NULL, " ", &save);
        jkw_Log(1, "web lobby error: %s\n", code ? code : "?");
        if (W.joining) W.joinError = (code && !strcmp(code, "full")) ? 2 : 1;
        if (W.hostPending) W.hostPending = 0;
    }
    else if (!strcmp(op, "ice"))
    {
        char* b64 = strtok_r(NULL, " ", &save);
        static uint8_t json[16384];
        if (b64 && jkw_B64Decode(b64, json, sizeof(json))) jkrtc_SetIceServersJson((char*)json);
        jkw_Log(0, "lobby sent ICE servers%s\n", jkrtc_Available() ? "" : " (this build has no WebRTC; using the lobby)");
    }
    else if (!strcmp(op, "signal"))
    {
        char* id = strtok_r(NULL, " ", &save);
        char* b64 = strtok_r(NULL, " ", &save);
        static uint8_t json[16384];
        if (id && b64 && W.inSession && jkw_B64Decode(b64, json, sizeof(json)))
            jkrtc_OnSignal(atoi(id), (char*)json);
    }
}

static int jkw_SendPacketTo(uint32_t peer, const uint8_t* frame, size_t n);

// A packet that came over a direct link from player 'peer'. The host stamps
// who sent it and forwards anything meant for another joiner, like the
// browser build's host does.
static void jkw_HandleDirect(int peer, uint8_t* p, size_t n)
{
    uint32_t to;
    if (n < 8 || !W.inSession) return;
    if (W.isHost)
    {
        wr32(p, (uint32_t)peer);
        to = rd32(p + 4);
        if (to > 1)
        {
            if (to <= JKW_LAST_ID) jkw_SendPacketTo(to, p, n);
            return;
        }
    }
    if (n >= 16 && p[8] == 'J' && p[9] == 'K' && p[10] == 'M' && p[11] == 'T')
    {
        jkx_OnFrame((uint32_t)peer, p, n, JKX_VIA_DIRECT);
        return;
    }
    jkw_LogSaberInfo("<< (direct)", rd32(p), rd32(p + 4), p + 8, n - 8);
    jkw_Push(DPS_RECV_DATA, rd32(p), p + 8, (uint32_t)(n - 8), 0, 0);
}

// With logging on, notes the messages that carry players' model and saber
// (DSS_SABERINFO*), the usual suspects when a skin doesn't show up.
static void jkw_LogSaberInfo(const char* dir, uint32_t from, uint32_t to, const uint8_t* m, size_t n)
{
    uint16_t id;
    if (!W.log || n < 4) return;
    id = (uint16_t)(m[0] | m[1] << 8);
    if (id != 57 && id != 58 && id != 49) return;
    jkw_Log(0, "%s SABERINFO%d %u->%u %.32s %.32s\n", dir, id == 57 ? 1 : id == 58 ? 2 : 3, from, to,
            n > 8 ? (const char*)m + 8 : "", n > 72 ? (const char*)m + 72 : "");
}

static void jkw_HandleBinary(const uint8_t* p, size_t n)
{
    uint32_t from, to;
    if (n >= 12) jkw_LogSaberInfo("<<", rd32(p), rd32(p + 4), p + 8, n - 8);
    if (n < 8 || !W.inSession) return;
    // Map and mod download frames ("JKMT" after the ids), not the game's.
    if (n >= 16 && p[8] == 'J' && p[9] == 'K' && p[10] == 'M' && p[11] == 'T')
    {
        jkx_OnFrame(rd32(p), p, n, JKX_VIA_RELAY);
        return;
    }
    from = rd32(p);
    to = rd32(p + 4);
    if (to && to != (W.isHost ? 1u : W.myId)) return;
    jkw_Push(DPS_RECV_DATA, from, p + 8, (uint32_t)(n - 8), 0, 0);
}

// Pumps until *flag changes, the socket drops, or timeout.
static void jkw_WaitFor(volatile int* flag, int wantNonzero, int ms)
{
    uint64_t start = jkw_NowMs();
    while (W.open && jkw_NowMs() - start < (uint64_t)ms)
    {
        if ((*flag != 0) == wantNonzero) return;
        jkw_Pump(20);
    }
}

static void jkw_ResetSession(void)
{
    jkrtc_End();
    jkx_HostWithdraw();
    W.inSession = W.isHost = W.lostSignalled = 0;
    W.hostPending = W.joining = W.joined = W.joinError = 0;
    W.hostedRoom = -1;
    W.myId = 0;
    memset(W.peers, 0, sizeof(W.peers));
    memset(&W.desc, 0, sizeof(W.desc));
    jkw_FreeEvents();
}

// ---------------------------------------------------------------------------
// Transport API
// ---------------------------------------------------------------------------

// The mod tail of a listed game (from its guid), "" for none, NULL if unknown.
const char* jkweb_GetSessionTail(const uint8_t guid[16])
{
    if (rd32(guid) != 0x4A4B5753) return NULL;
    for (int i = 0; i < W.numRooms; i++)
        if ((uint32_t)W.roomIds[i] == rd32(guid + 4)) return W.roomTails[i];
    return NULL;
}

void jkweb_SetHostTail(const char* hex)
{
    snprintf(W.hostTail, sizeof(W.hostTail), "%s", hex ? hex : "");
}

// Our game and player name (UTF-16), for hello and list.
void jkweb_SetPresence(int mots, const uint16_t* name)
{
    char utf8[3 * 15 + 1];   // base64: 60 characters
    size_t n = 0;
    snprintf(jkw_presGame, sizeof(jkw_presGame), "%s", mots ? "mots" : "jk1");
    // The lobby keeps 15 characters, what the game sends the other players.
    for (int i = 0; name && i < 15 && name[i]; i++)
    {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;   // JK names are BMP; no pairs
        if (c < 0x80) utf8[n++] = (char)c;
        else if (c < 0x800) { utf8[n++] = (char)(0xC0 | (c >> 6)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
        else { utf8[n++] = (char)(0xE0 | (c >> 12)); utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    while (n && utf8[n - 1] == ' ') n--;
    jkw_presName[jkw_B64Encode((const uint8_t*)utf8, n, jkw_presName)] = 0;
    jkw_SayHello();
}

void jkweb_SetLobby(const char* url)
{
    if (!url) url = "";
    if (!strcmp(url, W.url)) return;
    snprintf(W.url, sizeof(W.url), "%s", url);
    W.lastFailMs = 0;
    if (W.open && !W.inSession) jkw_Disconnect("lobby address changed");
}

long dps_Startup(const uint8_t appGuid[16])
{
    const char* lg = getenv("OPENJKDF2_DPLAY_LOG");
    char keep[512];
    if (W.started) return JKW_HR_OK;
    memcpy(keep, W.url, sizeof(keep));
    memset(&W, 0, sizeof(W));
    memcpy(W.url, keep, sizeof(W.url));
    memcpy(W.appGuid, appGuid, 16);
    W.log = lg && *lg && *lg != '0';
    W.startMs = jkw_NowMs();
    W.hostedRoom = -1;
#ifndef LINUX
    W.caBlob = stdEmbeddedRes_Load("ssl/cacert.pem", NULL);
#endif
    W.started = 1;
    return JKW_HR_OK;
}

void dps_Shutdown(void)
{
    if (!W.started) return;
    dps_Close();
    jkw_Disconnect("shutdown");
    free(W.rx);
    free(W.msg);
    free(W.evLast);
#ifndef LINUX
    free(W.caBlob);
#endif
    W.rx = W.msg = NULL;
    W.evLast = NULL;
    W.started = 0;
}

long dps_Recreate(void) { dps_Close(); W.numRooms = 0; return JKW_HR_OK; }
long dps_EnumConnections(dps_connection_cb cb, void* ctx) { (void)cb; (void)ctx; return JKW_HR_OK; }

long dps_InitializeConnection(const void* connection)
{
    (void)connection;
    return W.started ? JKW_HR_OK : JKW_HR_UNAVAILABLE;
}

void dps_SetEnumHost(const char* host) { (void)host; }

long dps_EnumSessions(dps_session_cb cb, void* ctx)
{
    if (!W.started) return JKW_HR_UNAVAILABLE;
    if (!jkw_Connect()) return JKW_HR_OK;          // no list; the menu keeps polling
    W.gotList = 0;
    // Only the games we can join: JK can't join a MotS game or the other way
    // round. A lobby that doesn't know the argument lists everything.
    if (jkw_presGame[0])
    {
        char line[16];
        snprintf(line, sizeof(line), "list %s", jkw_presGame);
        jkw_SendText(line);
    }
    else
        jkw_SendText("list");
    jkw_WaitFor(&W.gotList, 1, JKW_LIST_MS);
    for (int i = 0; i < W.numRooms; i++)
    {
        dps_session s;
        jkw_EntryToSession(W.roomEntries[i], W.roomIds[i], W.roomPlayers[i], &s);
        if (!cb(ctx, &s)) break;
    }
    return JKW_HR_OK;
}

long dps_StopEnumSessions(void) { return JKW_HR_OK; }

long dps_OpenCreate(const dps_session* desc)
{
    if (!W.started) return JKW_HR_UNAVAILABLE;
    if (W.inSession) return JKW_HR_ACCESSDENIED;
    if (!jkw_Connect()) return JKW_HR_UNAVAILABLE;
    jkw_ResetSession();
    jkw_SessionToEntry(desc, W.entry);
    W.hostPending = 1;
    jkw_SendEntry("host", -1);
    jkw_WaitFor(&W.hostPending, 0, JKW_REPLY_MS);
    if (W.hostPending || W.hostedRoom < 0)
    {
        jkw_Log(1, "the web lobby didn't open a room for us\n");
        W.hostPending = 0;
        return JKW_HR_UNAVAILABLE;
    }
    W.desc = *desc;
    wr32(W.desc.guidInstance, 0x4A4B5753);
    wr32(W.desc.guidInstance + 4, (uint32_t)W.hostedRoom);
    W.isHost = 1;
    W.inSession = 1;
    W.myId = 1;
    jkrtc_Begin(1, W.log);
    jkw_Log(1, "hosting room %d on web lobby %s\n", W.hostedRoom, W.url);
    return JKW_HR_OK;
}

long dps_OpenJoin(const uint8_t guidInstance[16], const uint16_t* password)
{
    int idx = -1;
    char line[64];
    if (!W.started || !W.open) return JKW_HR_UNAVAILABLE;
    if (W.inSession) return JKW_HR_ACCESSDENIED;
    if (rd32(guidInstance) != 0x4A4B5753) return JKW_HR_NOSESSIONS;
    for (int i = 0; i < W.numRooms; i++)
        if ((uint32_t)W.roomIds[i] == rd32(guidInstance + 4)) idx = i;
    if (idx < 0) return JKW_HR_NOSESSIONS;

    // The browser build checks the password on the joining side (it's in the
    // game list), so do the same.
    {
        const uint8_t* pw = W.roomEntries[idx] + JKE_PASSWORD;
        for (int i = 0; i < 32; i++)
        {
            uint16_t a = (uint16_t)(pw[2 * i] | pw[2 * i + 1] << 8);
            uint16_t b = password ? password[i] : 0;
            if (a != b) return JKW_HR_INVALIDPW;
            if (!a) break;
        }
    }

    jkw_ResetSession();
    W.joining = 1;
    snprintf(line, sizeof(line), "join %d", W.roomIds[idx]);
    jkw_SendText(line);
    {
        uint64_t start = jkw_NowMs();
        while (W.open && !W.joined && !W.joinError && jkw_NowMs() - start < JKW_REPLY_MS) jkw_Pump(20);
    }
    if (!W.joined)
    {
        int err = W.joinError;
        jkw_SendText("leave");
        jkw_ResetSession();
        return err == 2 ? JKW_HR_GAMEFULL : JKW_HR_UNAVAILABLE;
    }
    // Added (as tristamus/OpenJKDF2 9a42643c): a host with no room left welcomes us with its
    // player count at the limit. Told now, before the level loads.
    if (rd32(W.entry + JKE_MAXPLAYERS) > 0 && rd32(W.entry + JKE_NUMPLAYERS) >= rd32(W.entry + JKE_MAXPLAYERS))
    {
        jkw_Log(1, "room %d is full\n", W.roomIds[idx]);
        jkw_SendText("leave");
        jkw_ResetSession();
        return JKW_HR_GAMEFULL;
    }
    jkw_EntryToSession(W.entry, W.roomIds[idx], -1, &W.desc);
    W.inSession = 1;
    jkrtc_Begin(0, W.log);     // offer a direct link to the host
    jkw_Log(1, "joined room %d on web lobby %s as player %u\n", W.roomIds[idx], W.url, W.myId);
    return JKW_HR_OK;
}

long dps_Close(void)
{
    if (W.inSession || W.hostPending || W.joining) jkw_SendText("leave");
    jkw_ResetSession();
    return JKW_HR_OK;
}

long dps_GetSessionDesc(dps_session* out)
{
    if (!W.inSession) return JKW_HR_NOCONNECTION;
    *out = W.desc;
    return JKW_HR_OK;
}

long dps_SetSessionDesc(const dps_session* in)
{
    uint8_t guid[16];
    if (!W.inSession) return JKW_HR_NOCONNECTION;
    if (!W.isHost) return JKW_HR_ACCESSDENIED;
    memcpy(guid, W.desc.guidInstance, 16);
    W.desc = *in;
    memcpy(W.desc.guidInstance, guid, 16);
    jkw_SessionToEntry(in, W.entry);
    jkw_SendEntry("update", -1);
    return JKW_HR_OK;
}

// One player per machine: the host is 1, a joiner gets its id from "welcome".
long dps_CreatePlayer(const uint16_t* shortName, uint32_t* idOut)
{
    (void)shortName;
    if (!W.inSession) return JKW_HR_NOCONNECTION;
    if (idOut) *idOut = W.isHost ? 1 : W.myId;
    return JKW_HR_OK;
}

long dps_DestroyPlayer(uint32_t id) { (void)id; return JKW_HR_OK; }

// Like the browser build, treat ids 1..32 as present: clients never learn
// who else is connected, and JK's join check only needs the ids to exist.
long dps_EnumPlayers(dps_player_cb cb, void* ctx)
{
    if (!W.inSession) return JKW_HR_NOCONNECTION;
    for (uint32_t id = 1; id <= 32; id++)
        if (!cb(ctx, id, NULL, NULL)) break;
    return JKW_HR_OK;
}

// Direct link if it's up, else the lobby. Joiners only link to the host,
// which forwards for them.
static int jkw_SendPacketTo(uint32_t peer, const uint8_t* frame, size_t n)
{
    if (jkrtc_Send((int)peer, frame, (int)n)) return 0;
    return jkw_SendFrame(0x2, frame, n);
}

// Every message through the lobby counts against its daily request limit, and below a
// 100 ms tick rate each player sends its position each physics step it's due. Through the
// lobby a player's position goes to each player at most every 200 ms (as at JK's 180 ms);
// the next one replaces a skipped one. Only DSS_THINGPOS (1) without resends (msgId 0),
// 52 or 64 bytes, which only a player's is. Direct links get them all. As the browser
// build does (tristamus web-mp e1f4f0a8, stdComm_WSRelay.c skipRelayedPos).
#define JKW_RELAY_POS_MS   200
#define JKW_RELAY_POS_KEYS 256
static struct { uint32_t to, guid; uint64_t ms; } jkw_relayPos[JKW_RELAY_POS_KEYS];

static int jkw_SkipRelayedPos(uint32_t idTo, const uint8_t* msg, uint32_t size)
{
    if (size != 4 + 52 && size != 4 + 64) return 0;
    if (rd16(msg) != 1 || rd16(msg + 2) != 0) return 0;
    uint32_t guid = rd32(msg + 4);
    uint64_t now = jkw_NowMs();
    int slot = -1;
    for (int i = 0; i < JKW_RELAY_POS_KEYS; i++)
    {
        if (jkw_relayPos[i].ms && jkw_relayPos[i].to == idTo && jkw_relayPos[i].guid == guid)
        {
            if (now - jkw_relayPos[i].ms < JKW_RELAY_POS_MS) return 1;
            slot = i;
            break;
        }
        if (slot < 0 && (!jkw_relayPos[i].ms || now - jkw_relayPos[i].ms > 10000)) slot = i;
    }
    if (slot >= 0)
    {
        jkw_relayPos[slot].to = idTo;
        jkw_relayPos[slot].guid = guid;
        jkw_relayPos[slot].ms = now;
    }
    return 0;
}

long dps_Send(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size)
{
    uint8_t buf[4096 + 8];
    if (!W.inSession) return JKW_HR_NOCONNECTION;
    // A host's sends to everyone include itself (slot 0, stdComm_SendToPlayer). The lobby
    // drops those but counts them against its daily limit (browser build: e1f4f0a8).
    if (W.isHost && (idTo == 0 || idTo == 1)) return JKW_HR_OK;
    if (size > sizeof(buf) - 8) return JKW_HR_GENERIC;
    wr32(buf, idFrom);
    wr32(buf + 4, idTo);
    memcpy(buf + 8, data, size);
    jkw_LogSaberInfo(">>", idFrom, idTo, buf + 8, size);
    if (W.isHost)
    {
        if (idTo >= JKW_FIRST_ID && idTo <= JKW_LAST_ID && jkrtc_Send((int)idTo, buf, (int)size + 8)) return JKW_HR_OK;
    }
    else if (jkrtc_Send(1, buf, (int)size + 8))
        return JKW_HR_OK;
    if (!W.open) return JKW_HR_NOCONNECTION;
    if (jkw_SkipRelayedPos(idTo, buf + 8, size)) return JKW_HR_OK;
    return jkw_SendFrame(0x2, buf, size + 8) == 0 ? JKW_HR_OK : JKW_HR_GENERIC;
}

int dps_Receive(uint32_t* fromOut, const void** dataOut, uint32_t* sizeOut, dps_sysmsg* sysOut)
{
    jkw_event* e;
    free(W.evLast);
    W.evLast = NULL;
    if (!W.started) return DPS_RECV_NONE;
    if (!W.evHead) jkw_Pump(0);
    e = W.evHead;
    if (!e) return DPS_RECV_NONE;
    W.evHead = e->next;
    if (!W.evHead) W.evTail = NULL;
    W.evLast = e;
    if (e->type == DPS_RECV_DATA)
    {
        *fromOut = e->from;
        *dataOut = e->data;
        *sizeOut = e->size;
        return DPS_RECV_DATA;
    }
    *sysOut = e->sys;
    return DPS_RECV_SYS;
}

// ---------------------------------------------------------------------------
// For jkweb_xfer.c (map and mod downloads, joiner side)
// ---------------------------------------------------------------------------

int jkweb_LobbyXferSend(uint32_t to, const uint8_t* frame, size_t n, int allowDirect)
{
    if (!W.inSession) return 0;
    if (allowDirect && jkrtc_SendMap((int)to, frame, (int)n)) return JKX_VIA_DIRECT;
    return jkw_SendFrame(0x2, frame, n) == 0 ? JKX_VIA_RELAY : 0;
}

// Chunks a host serves go out a few at a time, only while the link has room,
// so serving a download never holds up the game (or trips the send timeout).
int jkweb_LobbyXferCanSend(uint32_t to, int allowDirect)
{
    if (!W.inSession || !W.open) return 0;
    if (allowDirect && jkrtc_MapOpen((int)to))
    {
        int buffered = jkrtc_MapBuffered((int)to);
        return buffered >= 0 && buffered < 256 * 1024;
    }
    return jkw_WaitSock(1, 0);
}

// For "gameinfo": whether game packets use direct WebRTC links or the lobby.
void jkweb_DescribeLinks(char* out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (!W.inSession) return;
    if (!jkrtc_Available())
        snprintf(out, cap, "Packets go through the lobby (this build has no WebRTC)");
    else if (!W.isHost)
        snprintf(out, cap, jkrtc_IsOpen(1) ? "Packets go over a direct WebRTC link to the host"
                                           : "Packets go through the lobby (no direct link to the host)");
    else
    {
        int n, peers = 0, direct = 0;
        for (n = JKW_FIRST_ID; n <= JKW_LAST_ID; n++)
        {
            if (!W.peers[n]) continue;
            peers++;
            if (jkrtc_IsOpen(n)) direct++;
        }
        if (!peers)
            snprintf(out, cap, "No one has joined yet");
        else if (direct == peers)
            snprintf(out, cap, "All %d joiners on direct WebRTC links", peers);
        else
            snprintf(out, cap, "%d of %d joiners on direct WebRTC links, the rest through the lobby", direct, peers);
    }
}

int jkweb_LobbyXferDirectUp(void) { return W.inSession && jkrtc_MapOpen(1); }

// No direct link is coming in a build without WebRTC.
int jkweb_LobbyXferDirectPossible(void) { return jkrtc_Available(); }

int jkweb_LobbyXferHostState(void)
{
    if (!W.open) return -1;
    if (!W.inSession || W.lostSignalled) return 0;
    return 1;
}

void jkweb_LobbyXferPump(void) { jkw_Pump(0); }

const dps_backend jkweb_backend = DPS_BACKEND_TABLE(jkweb);

#endif // PLATFORM_DPLAY && PLATFORM_CURL
