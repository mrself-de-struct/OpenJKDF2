// "Imperial Network Protocol" transport: multiplayer through a relay server.
//
// Implements the same API as the DirectPlay transports (see dplay_backend.h),
// so everything above it -- JK's join logic, its game messages, checksums,
// the scoreboard -- is exactly what a DirectPlay game uses. Only the path the
// bytes take differs: every player keeps one outbound UDP flow to the relay
// server, which forwards traffic within the session. Players only make
// outbound connections, so nobody needs port forwarding; only the server must
// be reachable. OpenJKDF2 players only (JK.EXE can't speak this).
//
// Direct links: when the host and a joiner both want one (net_directLinks),
// the server introduces them and they punch through their NATs from this same
// socket; game packets then go straight between them, and the host forwards
// joiners' packets to the other players. Each link falls back to the server
// when it fails or goes quiet. See "Direct links" in jkrelay_proto.h.
//
// Resuming: when the server restarts (or forgets us), the host puts its game
// back on it with the same player ids and the joiners rejoin it, so the game
// goes on. See "Resuming" in jkrelay_proto.h.
//
// Protocol: jkrelay_proto.h. Server: tools/jkrelay/jkrelay_server.c.
// Server address: dps_SetRelayServer(), OPENJKDF2_RELAY="host[:port]".
// OPENJKDF2_DPLAY_LOG=1 logs every relay packet type.

// JKRELAY_STANDALONE_CLIENT: built on its own, without the DirectPlay glue (the browser
// build's Imperial Network Protocol connection).
#if defined(PLATFORM_DPLAY) || defined(JKRELAY_STANDALONE_CLIENT)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET jkr_sock;
#define JKR_BADSOCK INVALID_SOCKET
#define jkr_closesock closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int jkr_sock;
#define JKR_BADSOCK (-1)
#define jkr_closesock close
#endif

#ifdef __EMSCRIPTEN__
// Browsers (2026-10-29): there's no UDP, so each socket here is a WebSocket to an
// Imperial Network Protocol server's websocket port (jkrelay_server -w, behind
// wss://), one packet per binary message. A server is named by a ws:// or wss:// URL,
// a bare host (wss://<host>/imperial) or host:port (ws://host:port/, for testing);
// jkr_ResolveAddr gives each a made-up address that stands for it. Waits yield to the
// page (emscripten_sleep, -sASYNCIFY) so messages can arrive. No direct links (no hole
// punching), and dedicated servers reachable only at their own address aren't listed.
#include <emscripten.h>

#define JKR_WS_MAX_NAMES 16
#define JKR_WS_MAX_SOCKS 8
static char jkr_wsUrls[JKR_WS_MAX_NAMES][256];
static int jkr_wsNumUrls = 0;
static char jkr_wsGateway[256];   // the relay server's URL: other addresses go through its gateway

// The URL for a server named as the player typed it: a ws:// or wss:// URL as it is,
// host:port as ws://host:port/, a bare host as wss://<host>/imperial.
static void jkr_WsUrlFor(const char* server, char* url, size_t cap)
{
    if (!strncmp(server, "ws://", 5) || !strncmp(server, "wss://", 6))
        snprintf(url, cap, "%s", server);
    else if (strchr(server, ':'))
        snprintf(url, cap, "ws://%s/", server);
    else
        snprintf(url, cap, "wss://%s/imperial", server);
}
static struct { int used, ws; struct sockaddr_in peer; } jkr_wsSocks[JKR_WS_MAX_SOCKS];

EM_JS(int, jkr_js_open, (const char* url), {
    var u = UTF8ToString(url);
    if (!Module.jkrWs) Module.jkrWs = { next: 1, socks: {} };
    var id = Module.jkrWs.next++;
    var s = { q: [], out: [], closed: false };
    try { s.ws = new WebSocket(u); } catch (e) { return -1; }
    s.ws.binaryType = 'arraybuffer';
    s.ws.onopen = function() { for (var i = 0; i < s.out.length; i++) s.ws.send(s.out[i]); s.out = []; };
    // Altered: what isn't read yet is capped, as a UDP socket's buffer is (a server
    // flooding us drops packets instead of filling the tab's memory).
    s.ws.onmessage = function(ev) { if (ev.data instanceof ArrayBuffer && s.q.length < 4096) s.q.push(new Uint8Array(ev.data)); };
    s.ws.onclose = function() { s.closed = true; };
    s.ws.onerror = function() { s.closed = true; };
    Module.jkrWs.socks[id] = s;
    return id;
});

EM_JS(int, jkr_js_send, (int id, const void* p, int n), {
    var s = Module.jkrWs && Module.jkrWs.socks[id];
    if (!s || s.closed) return -1;
    var d = HEAPU8.slice(p, p + n);
    if (s.ws.readyState === 1) s.ws.send(d);
    else if (s.ws.readyState === 0) s.out.push(d);
    else return -1;
    return n;
});

EM_JS(int, jkr_js_recv, (int id, void* p, int cap), {
    var s = Module.jkrWs && Module.jkrWs.socks[id];
    if (!s || !s.q.length) return -1;
    var d = s.q.shift();
    var n = Math.min(d.length, cap);
    HEAPU8.set(d.subarray(0, n), p);
    return n;
});

EM_JS(void, jkr_js_close, (int id), {
    var s = Module.jkrWs && Module.jkrWs.socks[id];
    if (!s) return;
    try { s.ws.close(); } catch (e) {}
    delete Module.jkrWs.socks[id];
});

static int jkr_WsSocket(void)
{
    for (int i = 0; i < JKR_WS_MAX_SOCKS; i++)
        if (!jkr_wsSocks[i].used)
        {
            memset(&jkr_wsSocks[i], 0, sizeof(jkr_wsSocks[i]));
            jkr_wsSocks[i].used = 1;
            return i;
        }
    return -1;
}

// A made-up address's URL; any other address (a dedicated server the relay server lists)
// through the relay server's gateway: <its URL>?to=a.b.c.d:port (jkrelay_proto.h, "Gateway").
static const char* jkr_WsUrlOf(const struct sockaddr_in* a)
{
    static char gw[320];
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    if ((ip & 0xFFFFFF00u) == 0x7F010000u)
        return (int)(ip & 0xFF) < jkr_wsNumUrls ? jkr_wsUrls[ip & 0xFF] : NULL;
    if (!jkr_wsGateway[0]) return NULL;
    snprintf(gw, sizeof(gw), "%s%sto=%u.%u.%u.%u:%u", jkr_wsGateway, strchr(jkr_wsGateway, '?') ? "&" : "?",
             ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, (unsigned)ntohs(a->sin_port));
    return gw;
}

static int jkr_WsSendTo(int sk, const void* p, int n, const struct sockaddr_in* to)
{
    const char* url;
    if (sk < 0 || sk >= JKR_WS_MAX_SOCKS || !jkr_wsSocks[sk].used || !to) return -1;
    if (jkr_wsSocks[sk].ws && (jkr_wsSocks[sk].peer.sin_addr.s_addr != to->sin_addr.s_addr || jkr_wsSocks[sk].peer.sin_port != to->sin_port))
    {
        jkr_js_close(jkr_wsSocks[sk].ws);   // another server now: one connection at a time
        jkr_wsSocks[sk].ws = 0;
    }
    if (!jkr_wsSocks[sk].ws)
    {
        if (!(url = jkr_WsUrlOf(to))) return -1;   // not a server we named (a listed server's own address)
        jkr_wsSocks[sk].ws = jkr_js_open(url);
        if (jkr_wsSocks[sk].ws < 0) { jkr_wsSocks[sk].ws = 0; return -1; }
        jkr_wsSocks[sk].peer = *to;
    }
    return jkr_js_send(jkr_wsSocks[sk].ws, p, n);
}

static int jkr_WsRecvFrom(int sk, void* p, int cap, struct sockaddr_in* from)
{
    int n;
    if (sk < 0 || sk >= JKR_WS_MAX_SOCKS || !jkr_wsSocks[sk].ws) return -1;
    n = jkr_js_recv(jkr_wsSocks[sk].ws, p, cap);
    if (n >= 0 && from) *from = jkr_wsSocks[sk].peer;
    return n;
}

static int jkr_WsWait(int ms)
{
    // Altered: a wait of 0 (the game's receive on every physics step, jkr_Pump(0)) no longer
    // yields. Each yield was a timer, which browsers hold for 4 ms once timers nest, so a slow
    // frame with many steps got slower still. Messages that come in during a frame are read at
    // the next frame's first step. Every loop that waits for a message asks for 10 or 20 ms,
    // and still sleeps. (From tristamus' copy, 97d336ab.)
    if (ms > 0)
        emscripten_sleep((unsigned)ms);
    return 1;
}

static void jkr_WsClose(int sk)
{
    if (sk < 0 || sk >= JKR_WS_MAX_SOCKS) return;
    if (jkr_wsSocks[sk].ws) jkr_js_close(jkr_wsSocks[sk].ws);
    memset(&jkr_wsSocks[sk], 0, sizeof(jkr_wsSocks[sk]));
}

#undef jkr_closesock
#define jkr_closesock(sk)            jkr_WsClose(sk)
#define socket(d, t, p)              jkr_WsSocket()
#define sendto(sk, b, n, f, a, l)    jkr_WsSendTo((sk), (b), (int)(n), (const struct sockaddr_in*)(a))
#define recvfrom(sk, b, n, f, a, l)  jkr_WsRecvFrom((sk), (b), (int)(n), (struct sockaddr_in*)(a))
#define poll(pfd, n, ms)             jkr_WsWait(ms)
#define bind(sk, a, l)               0
#define connect(sk, a, l)            (-1)
#define getsockname(sk, a, l)        (-1)
#define fcntl(...)                   0
#endif

#define DPS_BACKEND_PREFIX jkrelay
#include "dplay_backend.h"
#include "jkrelay_proto.h"
#ifndef JKRELAY_NO_RCON
#include "jkrelay_hmac.h"   // rcon (needs jkmod_Sha256)
#endif

// Servers listed on a master are joined at their own address, which browsers can't reach.
#ifdef __EMSCRIPTEN__
#define JKR_LIST_FLAGS          (JKR_LF_TAILS | JKR_LF_SERVERS | JKR_LF_GATEWAY)   // listed servers through the gateway
#else
#define JKR_LIST_FLAGS          (JKR_LF_SERVERS | JKR_LF_TAILS)
#endif
#define JKR_RCVBUF_BYTES (4 * 1024 * 1024)   // Added: the game socket's receive buffer (map downloads)

#define JKR_OK_HR               0
#define JKR_ERR_GENERIC         ((long)0x80004005)
#define JKR_ERR_UNAVAILABLE     ((long)0x887700FA)
#define JKR_ERR_NOCONNECTION    ((long)0x887700AA)
#define JKR_ERR_NOSESSIONS      ((long)0x88770168)
#define JKR_ERR_INVALIDPLAYER   ((long)0x88770096)
#define JKR_ERR_ACCESSDENIED    ((long)0x88770014)

#define JKR_MAX_PLAYERS   64
#define JKR_MAX_SESSIONS  32
#define JKR_LIST_WAIT_MS  400    // the menu polls every 5 s; don't freeze it
#define JKR_MAX_MASTERS   8
#define JKR_MASTER_WAIT_MS   600     // per master server, each time the menu polls
#define JKR_MASTER_RETRY_MS  30000   // a master that didn't answer isn't asked again before this

typedef struct
{
    int      used;
    uint32_t id, sysId, flags;
    int      local;
    int      seen;            // resuming: the server told us about it again
    uint16_t name[32];
} jkr_player;

// A direct link to another machine (a joiner's to the host, the host's to each joiner).
typedef struct
{
    int      used;
    uint32_t sysId, key;              // the peer's system player id, the link key from the server
    struct sockaddr_in pub, lan;      // where the server says it is
    struct sockaddr_in addr;          // where it answers from (once weHear)
    int      hasLan;
    int      weHear, peerHears, up;
    uint64_t startMs, lastRxMs, lastTxMs, lastPunchMs;
} jkr_link;

typedef struct jkr_event
{
    struct jkr_event* next;
    int        type;          // DPS_RECV_DATA / DPS_RECV_SYS
    uint32_t   from, size;
    uint32_t   seq;           // data: the sender's update number (jkrelay_SetSendSeq), 0 if none
    dps_sysmsg sys;
    uint8_t    data[];
} jkr_event;

static struct
{
    int       started;
    int       log;
    uint8_t   appGuid[16];
    jkr_sock  sock;
    char      server[256];
    struct sockaddr_in serverAddr;
    int       serverResolved;
    uint32_t  token;
    uint32_t  nextSeq;
    uint64_t  startMs, lastRxMs, lastTxMs;

    // pending request
    uint32_t  waitSeq;
    int       gotReply;
    uint8_t   reply[JKR_MAX_PACKET];
    int       replyLen;

    // events from the server
    uint32_t  lastEvent;
    int       initialSeen;     // JOIN: existing players received so far

    // session list from the last LIST
    dps_session sessions[JKR_MAX_SESSIONS];
    struct sockaddr_in sessionAddr[JKR_MAX_SESSIONS];   // a listed server's own address (port 0: on the relay server)
    char      sessionTail[JKR_MAX_SESSIONS][JKR_MAX_TAIL + 1];   // its host's mod ("": not said)
    char      hostTail[JKR_MAX_TAIL + 1];   // ours, when we host (jkrelay_SetHostTail)
    uint8_t   hostBans[6 * JKR_MAX_HOST_BANS];   // ours (jkrelay_HostSetBans), sent again after a resume
    int       numHostBans;
    void    (*pfnTunnel)(uint32_t ip, uint16_t port, const uint8_t* p, size_t n);   // rcon through the relay
    int       numSessions;
    int       numRelaySessions;       // the first numSessions came from the relay server, the rest from master servers
    // master servers (net_masterServers), asked for their games as well: "host[:port]" each,
    // and when one that didn't answer may be asked again
    char      masters[JKR_MAX_MASTERS][128];
    uint64_t  masterRetryMs[JKR_MAX_MASTERS];
    int       numMasters;
    // joined a listed server at its own address (jkrelay_proto.h, "Master server"):
    // the relay server's address is put back when the game ends
    int       bDirect;
    uint64_t  lastListMs;

    // session
    int       inSession, isHost, lostSignalled;
    int       lostReason, lostMinutes;   // why the server ended our game (JKR_LOST_*), a ban's minutes
    int       failReason, failMinutes;   // why the last JOIN was refused (JKR_LOST_BANNED)
    dps_session desc;
    uint32_t  localSysId;
    jkr_player players[JKR_MAX_PLAYERS];
    jkr_link  links[JKR_MAX_PLAYERS];
    int       wantDirect;             // net_directLinks

    // putting the game back on a restarted server (jkrelay_proto.h, "Resuming")
    int       resuming;               // 1 = HELLO sent, 2 = REHOST / REJOIN sent
    uint32_t  resumeSeq;              // the request being retried (0 = new one next time)
    uint64_t  resumeStartMs, resumeTxMs;
    uint64_t  resumedMs;              // players not back by + JKR_REJOIN_WINDOW_MS are gone
    int       serverQuiet;            // logged that the server stopped answering

    jkr_event* evHead;
    jkr_event* evTail;
    jkr_event* evLast;     // returned by Receive; freed on the next call
    uint32_t   sendSeq;    // for the next Send's header (jkrelay_SetSendSeq)

    uint8_t   rx[2048];
} R;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t jkr_NowMs(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount();   // XP-compatible; wraps after 49 days
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void jkr_Log(int always, const char* fmt, ...)
{
    va_list ap;
    if (!always && !R.log) return;
    va_start(ap, fmt);
    fprintf(stderr, "[jkrelay %8.3f] ", (double)(jkr_NowMs() - R.startMs) / 1000.0);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static size_t jkr_Len16(const uint16_t* s) { size_t n = 0; if (s) while (s[n]) n++; return n; }

// Writes u8 length + UTF-16 string (truncated to max). Returns bytes written.
static size_t jkr_PutStr(uint8_t* p, const uint16_t* s, size_t max)
{
    size_t n = jkr_Len16(s);
    if (n > max) n = max;
    p[0] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) wr16(p + 1 + i * 2, s[i]);
    return 1 + n * 2;
}

// Reads u8 length + UTF-16 string into out (NUL-terminated). Returns bytes consumed or 0.
static size_t jkr_GetStr(const uint8_t* p, const uint8_t* end, uint16_t* out, size_t outLen)
{
    size_t n, i;
    if (p >= end) return 0;
    n = p[0];
    if (p + 1 + n * 2 > end) return 0;
    for (i = 0; i < n && i + 1 < outLen; i++) out[i] = rd16(p + 1 + i * 2);
    if (outLen) out[i] = 0;
    return 1 + n * 2;
}

static size_t jkr_PutDesc(uint8_t* p, const dps_session* d, uint32_t curPlayers)
{
    memcpy(p, d->guidInstance, 16);
    wr32(p + 16, d->flags);
    wr32(p + 20, d->maxPlayers);
    wr32(p + 24, curPlayers);
    for (int i = 0; i < 4; i++) wr32(p + 28 + 4 * i, d->user[i]);
    return 44 + jkr_PutStr(p + 44, d->name, JKR_MAX_NAME);
}

static size_t jkr_GetDesc(const uint8_t* p, const uint8_t* end, dps_session* d)
{
    size_t n;
    if (end - p < JKR_DESC_FIXED) return 0;
    memset(d, 0, sizeof(*d));
    memcpy(d->guidInstance, p, 16);
    d->flags = rd32(p + 16);
    d->maxPlayers = rd32(p + 20);
    d->curPlayers = rd32(p + 24);
    for (int i = 0; i < 4; i++) d->user[i] = rd32(p + 28 + 4 * i);
    n = jkr_GetStr(p + 44, end, d->name, DPS_NAME_MAX);
    return n ? 44 + n : 0;
}

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

// "host[:port]" -> address (port 'defPort' if none given). 0 if it can't be resolved.
#ifdef __EMSCRIPTEN__
// A server's websocket URL, and a made-up address that stands for it (see "Browsers").
static int jkr_ResolveAddr(const char* server, uint16_t defPort, struct sockaddr_in* out)
{
    char url[256];
    int i;
    if (!server || !server[0]) return 0;
    jkr_WsUrlFor(server, url, sizeof(url));
    for (i = 0; i < jkr_wsNumUrls && strcmp(jkr_wsUrls[i], url); i++) {}
    if (i == jkr_wsNumUrls)
    {
        if (jkr_wsNumUrls >= JKR_WS_MAX_NAMES) return 0;
        snprintf(jkr_wsUrls[jkr_wsNumUrls++], sizeof(jkr_wsUrls[0]), "%s", url);
    }
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(0x7F010000u | (uint32_t)i);
    out->sin_port = htons(defPort);
    return 1;
}
#else
static int jkr_ResolveAddr(const char* server, uint16_t defPort, struct sockaddr_in* out)
{
    char host[256];
    const char* colon;
    uint16_t port = defPort;
    struct addrinfo hints, *res = NULL;

    strncpy(host, server, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    colon = strrchr(host, ':');
    if (colon)
    {
        port = (uint16_t)atoi(colon + 1);
        host[colon - host] = 0;
        if (!port) port = defPort;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}
#endif

static int jkr_Resolve(void)
{
    if (R.serverResolved) return 1;
    if (!R.server[0]) return 0;
    if (!jkr_ResolveAddr(R.server, JKR_DEFAULT_PORT, &R.serverAddr))
    {
        jkr_Log(1, "can't resolve relay server '%s'\n", R.server);
        return 0;
    }
    R.serverResolved = 1;
    jkr_Log(0, "relay server %s -> %s:%u\n", R.server, inet_ntoa(R.serverAddr.sin_addr), ntohs(R.serverAddr.sin_port));
    return 1;
}

static void jkr_SendRaw(const uint8_t* p, size_t len)
{
    if (R.sock == JKR_BADSOCK || !jkr_Resolve()) return;
    sendto(R.sock, (const char*)p, (int)len, 0, (struct sockaddr*)&R.serverAddr, sizeof(R.serverAddr));
    R.lastTxMs = jkr_NowMs();
}

static void jkr_PutHdr(uint8_t* p, uint8_t type, uint32_t seq)
{
    memset(p, 0, JKR_HDR_SIZE);
    wr32(p, JKR_MAGIC);
    p[4] = type;
    p[5] = JKR_VERSION;
    wr32(p + 8, R.token);
    wr32(p + 12, seq);
}

static int jkr_SameAddr(const struct sockaddr_in* a, const struct sockaddr_in* b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

// Our LAN address, for peers behind the same NAT: the source address the system
// picks for the server, and our socket's port. 0 if unknown.
static void jkr_LanAddr(uint32_t* ipOut, uint16_t* portOut)
{
    struct sockaddr_in a;
#ifdef _WIN32
    int al = sizeof(a);
#else
    socklen_t al = sizeof(a);
#endif
    *ipOut = 0;
    *portOut = 0;
    if (R.sock == JKR_BADSOCK || !jkr_Resolve()) return;
    if (getsockname(R.sock, (struct sockaddr*)&a, &al) == 0) *portOut = ntohs(a.sin_port);
    jkr_sock t = socket(AF_INET, SOCK_DGRAM, 0);
    if (t == JKR_BADSOCK) return;
    al = sizeof(a);
    if (connect(t, (struct sockaddr*)&R.serverAddr, sizeof(R.serverAddr)) == 0
        && getsockname(t, (struct sockaddr*)&a, &al) == 0)
        *ipOut = ntohl(a.sin_addr.s_addr);
    jkr_closesock(t);
}

// The HOST / JOIN extension (jkrelay_proto.h, "Direct links").
static size_t jkr_PutJoinExt(uint8_t* p)
{
    uint32_t ip;
    uint16_t port;
    jkr_LanAddr(&ip, &port);
    wr32(p, R.wantDirect ? JKR_JF_DIRECT : 0);
    wr32(p + 4, ip);
    wr16(p + 8, port);
    return JKR_JOIN_EXT_SIZE;
}

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

static jkr_player* jkr_Find(uint32_t id)
{
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (R.players[i].used && R.players[i].id == id) return &R.players[i];
    return NULL;
}

static jkr_player* jkr_Add(uint32_t id)
{
    jkr_player* p = jkr_Find(id);
    if (p) return p;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (!R.players[i].used)
        {
            memset(&R.players[i], 0, sizeof(R.players[i]));
            R.players[i].used = 1;
            R.players[i].id = id;
            return &R.players[i];
        }
    return NULL;
}

static void jkr_Push(int type, uint32_t from, const void* data, uint32_t size, int sysKind, uint32_t sysPlayer)
{
    jkr_event* e = (jkr_event*)malloc(sizeof(jkr_event) + size);
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->from = from;
    e->size = size;
    if (size) memcpy(e->data, data, size);
    e->sys.kind = sysKind;
    e->sys.playerId = sysPlayer;
    if (R.evTail) R.evTail->next = e; else R.evHead = e;
    R.evTail = e;
}

static void jkr_FreeEvents(void)
{
    while (R.evHead) { jkr_event* e = R.evHead; R.evHead = e->next; free(e); }
    R.evTail = NULL;
}

static void jkr_ResetSession(void)
{
    R.inSession = R.isHost = R.lostSignalled = 0;
    R.localSysId = 0;
    // Altered (2026-11-09): R.lastEvent is kept. The server numbers its events per client,
    // from its first packet on, and goes on counting across games; resetting here made a
    // client that left a game (e.g. said No to a download) and joined again drop every
    // event (waiting for number 1), so it never saw the host's players ("host left").
    // It starts over only with a new client on the server (the WELCOME below).
    R.initialSeen = 0;
    memset(&R.desc, 0, sizeof(R.desc));
    memset(R.players, 0, sizeof(R.players));
    memset(R.links, 0, sizeof(R.links));
    R.resuming = 0;
    R.resumeSeq = 0;
    R.resumedMs = 0;
    R.serverQuiet = 0;
    jkr_FreeEvents();
}

static void jkr_SessionLost(const char* why)
{
    if (!R.inSession || R.lostSignalled) return;
    jkr_Log(1, "session lost: %s\n", why);
    R.lostSignalled = 1;
    jkr_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_SESSIONLOST, 0);
}

// ---------------------------------------------------------------------------
// Direct links
// ---------------------------------------------------------------------------

static jkr_link* jkr_FindLink(uint32_t sysId)
{
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (R.links[i].used && R.links[i].sysId == sysId) return &R.links[i];
    return NULL;
}

// Up and heard from lately: packets go over it. A link that went quiet (lost its
// NAT mapping, say) leaves them to the server until it answers again or times out.
static int jkr_LinkUsable(const jkr_link* l)
{
    return l->used && l->up && jkr_NowMs() - l->lastRxMs <= JKR_LINK_QUIET_MS;
}

static jkr_link* jkr_UpLink(uint32_t sysId)
{
    jkr_link* l = jkr_FindLink(sysId);
    return (l && jkr_LinkUsable(l)) ? l : NULL;
}

static jkr_link* jkr_LinkByKey(uint32_t key)
{
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (R.links[i].used && R.links[i].key == key) return &R.links[i];
    return NULL;
}

static int jkr_AnyLinkUp(void)
{
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (jkr_LinkUsable(&R.links[i])) return 1;
    return 0;
}

// For log lines: the name of the game player on that machine.
static void jkr_MachineName(uint32_t sysId, char* out, size_t outLen)
{
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* p = &R.players[i];
        if (!p->used || p->sysId != sysId || (p->flags & JKR_PF_SYSTEM)) continue;
        for (int j = 0; j < 31 && p->name[j] && k + 1 < outLen; j++)
            out[k++] = (p->name[j] >= 32 && p->name[j] < 127) ? (char)p->name[j] : '?';
        out[k] = 0;
        return;
    }
    snprintf(out, outLen, "%08x", sysId);
}

static void jkr_DescribeLink(const jkr_link* l, char* out, size_t outLen)
{
    if (R.isHost)
    {
        char nm[40];
        jkr_MachineName(l->sysId, nm, sizeof(nm));
        snprintf(out, outLen, "joiner %s", nm);
    }
    else
        snprintf(out, outLen, "the host");
}

static void jkr_SendPunch(jkr_link* l, const struct sockaddr_in* to)
{
    uint8_t pkt[JKR_HDR_SIZE + 5];
    if (!R.localSysId || R.sock == JKR_BADSOCK) return;
    jkr_PutHdr(pkt, JKR_PUNCH, 0);
    wr32(pkt + 8, l->key);
    wr32(pkt + JKR_HDR_SIZE, R.localSysId);
    pkt[JKR_HDR_SIZE + 4] = (uint8_t)l->weHear;
    sendto(R.sock, (const char*)pkt, (int)sizeof(pkt), 0, (const struct sockaddr*)to, sizeof(*to));
    l->lastTxMs = jkr_NowMs();
}

static void jkr_LinkDown(jkr_link* l, const char* why)
{
    if (l->up)
    {
        char who[48];
        jkr_DescribeLink(l, who, sizeof(who));
        jkr_Log(1, "direct link to %s down (%s); its packets go through the relay server\n", who, why);
    }
    l->up = l->weHear = l->peerHears = 0;
    memset(&l->addr, 0, sizeof(l->addr));
    l->startMs = jkr_NowMs();
    l->lastPunchMs = 0;
}

static void jkr_LinkAdd(const uint8_t* b, const uint8_t* end)
{
    jkr_link* l;
    uint32_t sysId, pubIp, lanIp;
    uint16_t pubPort, lanPort;
    if (end - b < JKR_EV_PEER_SIZE) return;
    sysId = rd32(b + 1);
    if (!R.wantDirect) return;
    l = jkr_FindLink(sysId);
    if (!l)
        for (int i = 0; i < JKR_MAX_PLAYERS && !l; i++)
            if (!R.links[i].used) l = &R.links[i];
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->used = 1;
    l->sysId = sysId;
    l->key = rd32(b + 5);
    pubIp = rd32(b + 9);
    pubPort = rd16(b + 13);
    lanIp = rd32(b + 15);
    lanPort = rd16(b + 19);
    l->pub.sin_family = AF_INET;
    l->pub.sin_addr.s_addr = htonl(pubIp);
    l->pub.sin_port = htons(pubPort);
    if (b[21] & JKR_PEER_AT_SERVER)
        l->pub.sin_addr = R.serverAddr.sin_addr;   // next to the relay server: reach it where we reach the server
    if (lanIp && lanPort)
    {
        l->lan.sin_family = AF_INET;
        l->lan.sin_addr.s_addr = htonl(lanIp);
        l->lan.sin_port = htons(lanPort);
        l->hasLan = !jkr_SameAddr(&l->lan, &l->pub);
    }
    l->startMs = jkr_NowMs();
    {
        char who[48];
        jkr_DescribeLink(l, who, sizeof(who));
        if (R.log)
            jkr_Log(0, "trying a direct link to %s at %s:%u\n", who, inet_ntoa(l->pub.sin_addr), ntohs(l->pub.sin_port));
        else
            jkr_Log(1, "trying a direct link to %s\n", who);
    }
}

static void jkr_LinkRemove(uint32_t sysId)
{
    jkr_link* l = jkr_FindLink(sysId);
    if (l) memset(l, 0, sizeof(*l));
}

// Punching, keepalives and timeouts; called from jkr_Pump.
static void jkr_LinkTick(uint64_t now)
{
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_link* l = &R.links[i];
        if (!l->used) continue;
        if (l->weHear && now - l->lastRxMs > JKR_LINK_TIMEOUT_MS)
            jkr_LinkDown(l, "no answer");
        if (l->up)
        {
            if (now - l->lastTxMs >= JKR_LINK_KEEPALIVE_MS) jkr_SendPunch(l, &l->addr);
            continue;
        }
        uint64_t every = (now - l->startMs < JKR_PUNCH_FAST_MS) ? JKR_PUNCH_MS : JKR_PUNCH_SLOW_MS;
        if (now - l->lastPunchMs < every) continue;
        l->lastPunchMs = now;
        if (l->weHear) jkr_SendPunch(l, &l->addr);
        jkr_SendPunch(l, &l->pub);
        if (l->hasLan) jkr_SendPunch(l, &l->lan);
    }
}

static void jkr_HandleData(const uint8_t* b, const uint8_t* end, jkr_link* via);
static int jkr_BigPiece(uint32_t from, const uint8_t* p, size_t n);
static void jkr_SendDescNow(void);
static void jkr_SendBansNow(void);
static int jkr_ClockPacket(uint32_t from, const uint8_t* p, size_t n);
static void jkr_ClockTick(void);
static void jkr_ResumeStart(void);
static void jkr_ResumeReply(const uint8_t* p, int len);
static void jkr_ResumeTick(uint64_t now);

// A packet from somewhere other than the server: only from peers we were introduced to.
static void jkr_HandlePeerPacket(const struct sockaddr_in* from, const uint8_t* p, int len)
{
    const uint8_t* b = p + JKR_HDR_SIZE;
    const uint8_t* end = p + len;
    jkr_link* l;
    uint64_t now = jkr_NowMs();

    if (!R.inSession || len < JKR_HDR_SIZE || rd32(p) != JKR_MAGIC) return;
    l = jkr_LinkByKey(rd32(p + 8));
    if (!l) return;

    if (p[4] != JKR_PUNCH && p[4] != JKR_DATA) return;
    if (p[4] == JKR_PUNCH && (end - b < 5 || rd32(b) != l->sysId)) return;
    if (l->up && !jkr_SameAddr(from, &l->addr)) return;
    l->addr = *from;
    l->weHear = 1;
    l->lastRxMs = now;
    // A "heard" punch, or game data (it only sends that once it hears us).
    if (p[4] == JKR_DATA || b[4]) l->peerHears = 1;
    else jkr_SendPunch(l, from);   // tell it right away that it gets through
    if (l->peerHears && !l->up)
    {
        char who[48];
        l->up = 1;
        jkr_DescribeLink(l, who, sizeof(who));
        jkr_Log(1, "direct link to %s up (%u ms)\n", who, (unsigned)(now - l->startMs));
    }
    if (p[4] == JKR_DATA)
        jkr_HandleData(b, end, l);
}

static void jkr_HandleEvent(const uint8_t* b, const uint8_t* end)
{
    if (b >= end) return;
    switch (b[0])
    {
        case JKR_EV_PLAYER_CREATED:
        {
            if (end - b < 14) return;
            int initial = b[1];
            uint32_t id = rd32(b + 2), sysId = rd32(b + 6), flags = rd32(b + 10);
            jkr_player* p = jkr_Find(id);
            if (p && p->local) return;                     // our own player
            p = jkr_Add(id);
            if (!p) return;
            p->sysId = sysId;
            p->flags = flags;
            p->seen = 1;
            jkr_GetStr(b + 14, end, p->name, 32);
            if (initial) R.initialSeen++;
            jkr_Log(0, "player %08x created%s (sys %08x, flags %x)\n", id, initial ? " [already in game]" : "", sysId, flags);
            if (!initial && !(flags & JKR_PF_SYSTEM))
                jkr_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_PLAYERCREATED, id);
            break;
        }
        case JKR_EV_PLAYER_DESTROYED:
        {
            if (end - b < 5) return;
            uint32_t id = rd32(b + 1);
            jkr_player* p = jkr_Find(id);
            if (!p || p->local) return;
            if (!(p->flags & JKR_PF_SYSTEM))
                jkr_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_PLAYERDESTROYED, id);
            else
                jkr_LinkRemove(id);
            p->used = 0;
            jkr_Log(0, "player %08x left\n", id);
            break;
        }
        case JKR_EV_SESSION_LOST:
            // Added: why, from a server that says (kicked or banned)
            R.lostReason = (end - b >= 2) ? b[1] : JKR_LOST_HOST_LEFT;
            R.lostMinutes = (end - b >= 4) ? rd16(b + 2) : 0;
            jkr_SessionLost(R.lostReason == JKR_LOST_KICKED ? "kicked" : R.lostReason == JKR_LOST_BANNED ? "banned" : "host left");
            break;
        case JKR_EV_DESC_CHANGED:
        {
            dps_session d;
            if (jkr_GetDesc(b + 1, end, &d))
            {
                memcpy(d.password, R.desc.password, sizeof(d.password));   // not sent; kept for JKR_REJOIN
                R.desc = d;
            }
            break;
        }
        case JKR_EV_PEER:
            jkr_LinkAdd(b, end);
            break;
    }
}

static void jkr_HandlePacket(const uint8_t* p, int len)
{
    const uint8_t* end = p + len;
    const uint8_t* b = p + JKR_HDR_SIZE;
    uint8_t type;
    uint32_t seq;

    if (len < JKR_HDR_SIZE || rd32(p) != JKR_MAGIC) return;
    type = p[4];
    seq = rd32(p + 12);
    R.lastRxMs = jkr_NowMs();

    if (type >= JKR_WELCOME && type <= JKR_FAIL)
    {
        if (R.resuming && seq == R.resumeSeq)
            jkr_ResumeReply(p, len);
        // Altered: a reply bigger than R.reply (JKR_MAX_PACKET) isn't taken; R.rx holds
        // 2048 bytes, and copying all of it ran past R.reply over the session state after
        // it, which any server could do with an oversized reply. (Found by tristamus.)
        else if (seq == R.waitSeq && !R.gotReply && len <= (int)sizeof(R.reply))
        {
            memcpy(R.reply, p, (size_t)len);
            R.replyLen = len;
            R.gotReply = 1;
        }
        return;
    }
    switch (type)
    {
        case JKR_TUNNEL:   // rcon for our game, through the relay server (jkrelay_proto.h)
            if (R.isHost && R.pfnTunnel && end - b >= 6 + JKR_HDR_SIZE)
            {
                uint32_t ip;
                uint16_t port;
                memcpy(&ip, b, 4);
                memcpy(&port, b + 4, 2);
                R.pfnTunnel(ip, port, b + 6, (size_t)(end - b - 6));
            }
            return;
        case JKR_EVENT:
        {
            uint8_t ack[JKR_HDR_SIZE];
            if (seq == R.lastEvent + 1)
            {
                R.lastEvent = seq;
                if (R.inSession)   // Altered: one from a game we've left is counted, not used
                    jkr_HandleEvent(b, end);
            }
            jkr_PutHdr(ack, JKR_ACK, R.lastEvent);   // also re-acks duplicates
            jkr_SendRaw(ack, sizeof(ack));
            break;
        }
        case JKR_DATA:
            jkr_HandleData(b, end, NULL);
            break;
        case JKR_UNKNOWN:
            // The server doesn't know our token (it restarted, or dropped us).
            if (!R.token || rd32(p + 8) != R.token) break;   // not about us
            if (R.inSession && R.localSysId && !R.lostSignalled && !R.resuming)
                jkr_ResumeStart();
            else if (!R.inSession)
                R.token = 0;   // handshake again next time
            break;
        default:
            break;
    }
}

// Services the socket, optionally waiting up to waitMs for something to arrive.
static void jkr_Pump(int waitMs)
{
    uint64_t now;
    if (R.sock == JKR_BADSOCK) return;
    jkr_ClockTick();   // pings for the clocks asked for (jkrelay_SyncClock)

#ifdef _WIN32
    {
        fd_set rs;
        struct timeval tv = { waitMs / 1000, (waitMs % 1000) * 1000 };
        FD_ZERO(&rs);
        FD_SET(R.sock, &rs);
        select(0, &rs, NULL, NULL, &tv);
    }
#else
    {
        struct pollfd pfd = { R.sock, POLLIN, 0 };
        poll(&pfd, 1, waitMs);
    }
#endif
    for (;;)
    {
        struct sockaddr_in from;
#ifdef _WIN32
        int fl = sizeof(from);
#else
        socklen_t fl = sizeof(from);
#endif
        int n = (int)recvfrom(R.sock, (char*)R.rx, sizeof(R.rx), 0, (struct sockaddr*)&from, &fl);
        if (n <= 0) break;
        if (R.serverResolved && !jkr_SameAddr(&from, &R.serverAddr))
            jkr_HandlePeerPacket(&from, R.rx, n);   // only peers we were introduced to
        else
            jkr_HandlePacket(R.rx, n);
    }

    now = jkr_NowMs();
    if (R.inSession) jkr_LinkTick(now);
    if (R.token && now - R.lastTxMs > JKR_KEEPALIVE_MS)
    {
        // Also how we find out that a restarted server forgot us (JKR_UNKNOWN).
        uint8_t k[JKR_HDR_SIZE];
        jkr_PutHdr(k, JKR_KEEPALIVE, 0);
        jkr_SendRaw(k, sizeof(k));
    }
    if (R.inSession && !R.lostSignalled)
    {
        if (R.resuming || R.resumedMs)
            jkr_ResumeTick(now);
        else if (now - R.lastRxMs <= JKR_TIMEOUT_MS)
            R.serverQuiet = 0;
        else if (!R.isHost && (!R.localSysId || !jkr_AnyLinkUp()))
            jkr_SessionLost("relay server stopped answering");
        else if (!R.serverQuiet)
        {
            // The host's game goes on (as does a joiner's over its link to the
            // host); keepalives find the server when it's back.
            R.serverQuiet = 1;
            jkr_Log(1, "relay server stopped answering; the game goes on%s and is put back on it when it answers\n",
                    R.isHost ? "" : " over the direct link");
        }
    }
}

// Sends a request and waits for its reply (same seq). Returns the reply
// type (JKR_FAIL included), or 0 on timeout.
static int jkr_Request(uint8_t type, const uint8_t* body, size_t bodyLen, int timeoutMs)
{
    uint8_t pkt[JKR_MAX_PACKET];
    uint64_t start = jkr_NowMs(), lastSend = 0;
    uint32_t seq = ++R.nextSeq;

    if (JKR_HDR_SIZE + bodyLen > sizeof(pkt)) return 0;
    R.waitSeq = seq;
    R.gotReply = 0;
    while (!R.gotReply && jkr_NowMs() - start < (uint64_t)timeoutMs)
    {
        if (jkr_NowMs() - lastSend >= JKR_RETRY_MS)
        {
            jkr_PutHdr(pkt, type, seq);     // token may have changed (HELLO)
            if (bodyLen) memcpy(pkt + JKR_HDR_SIZE, body, bodyLen);
            jkr_SendRaw(pkt, JKR_HDR_SIZE + bodyLen);
            lastSend = jkr_NowMs();
        }
        jkr_Pump(20);
    }
    R.waitSeq = 0;
    if (!R.gotReply)
    {
        // The server forgets clients it hasn't heard from in JKR_TIMEOUT_MS (or it
        // restarted) and then ignores our token; handshake again next time.
        if (type != JKR_HELLO && !R.inSession) R.token = 0;
        return 0;
    }
    return R.reply[4];
}

static long jkr_Fail(int replyType)
{
    if (replyType == JKR_FAIL && R.replyLen >= JKR_HDR_SIZE + 4)
    {
        // Added: why, from a server that says (banned)
        R.failReason = (R.replyLen >= JKR_HDR_SIZE + 5) ? R.reply[JKR_HDR_SIZE + 4] : 0;
        R.failMinutes = (R.replyLen >= JKR_HDR_SIZE + 7) ? rd16(R.reply + JKR_HDR_SIZE + 5) : 0;
        return (long)(int32_t)rd32(R.reply + JKR_HDR_SIZE);
    }
    return replyType ? JKR_ERR_GENERIC : JKR_ERR_UNAVAILABLE;   // timeout: "not available"
}

// Gets a token from the server (once).
static int jkr_Hello(int timeoutMs)
{
    uint8_t body[4];
    int t;
    // Quiet for a while outside a game (main menu): the server may have
    // dropped us already, so don't wait for a request to time out first.
    if (R.token && !R.inSession && jkr_NowMs() - R.lastTxMs > JKR_TIMEOUT_MS / 2) R.token = 0;
    if (R.token) return 1;
    if (!jkr_Resolve()) return 0;
    wr32(body, JKR_VERSION);
    t = jkr_Request(JKR_HELLO, body, sizeof(body), timeoutMs);
    if (t != JKR_WELCOME || R.replyLen < JKR_HDR_SIZE + 4) return 0;
    R.token = rd32(R.reply + JKR_HDR_SIZE);
    R.lastEvent = 0;   // Added: a new client to the server, whose events start over (see jkr_ResetSession)
    R.lastRxMs = jkr_NowMs();
    jkr_Log(0, "connected to relay server %s\n", R.server);
    return 1;
}

// Console "connect" (dps_RelayProbe): HELLO and LIST on a socket of its own,
// so it works from the menus without opening the connection. The server
// forgets this client after JKR_TIMEOUT_MS.
// With addrs, each game's address to join it at: a server listed there at its own
// address, else 'server' itself (a game hosted on it).
static int jkr_ProbeList(const char* server, const uint8_t appGuid[16], int timeoutMs, dps_session* out,
                         struct sockaddr_in* addrs, char (*tails)[JKR_MAX_TAIL + 1], int maxOut);

// A list reply's mod tail at p (jkrelay_proto.h, "Mods") into out; returns its size, 0 if cut short.
static size_t jkr_GetTail(const uint8_t* p, const uint8_t* end, char* out)
{
    if (out) out[0] = 0;
    if (p >= end || p + 1 + p[0] > end) return 0;
    if (out)
    {
        for (int i = 0; i < p[0]; i++) out[i] = (p[1 + i] >= 32 && p[1 + i] < 127) ? (char)p[1 + i] : '?';
        out[p[0]] = 0;
    }
    return 1 + (size_t)p[0];
}

int jkrelay_Probe(const char* server, const uint8_t appGuid[16], int timeoutMs, dps_session* out, int maxOut)
{
    return jkr_ProbeList(server, appGuid, timeoutMs, out, NULL, NULL, maxOut);
}

static int jkr_ProbeList(const char* server, const uint8_t appGuid[16], int timeoutMs, dps_session* out,
                         struct sockaddr_in* addrs, char (*tails)[JKR_MAX_TAIL + 1], int maxOut)
{
    struct sockaddr_in to;
    jkr_sock sk;
    uint8_t pkt[JKR_HDR_SIZE + 24], rx[JKR_MAX_PACKET];
    uint32_t token = 0, seq = 1;
    uint64_t start, lastSend = 0;
    int result = -1;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
    if (!server || !jkr_ResolveAddr(server, JKR_DEFAULT_PORT, &to)) goto done;
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk == JKR_BADSOCK) goto done;
    start = jkr_NowMs();
    while (result < 0 && jkr_NowMs() - start < (uint64_t)timeoutMs)
    {
        if (jkr_NowMs() - lastSend >= JKR_RETRY_MS)
        {
            size_t n = JKR_HDR_SIZE;
            memset(pkt, 0, JKR_HDR_SIZE);
            wr32(pkt, JKR_MAGIC);
            pkt[4] = token ? JKR_LIST : JKR_HELLO;
            pkt[5] = JKR_VERSION;
            wr32(pkt + 8, token);
            wr32(pkt + 12, seq);
            if (!token) { wr32(pkt + n, JKR_VERSION); n += 4; }
            else { memcpy(pkt + n, appGuid, 16); wr32(pkt + n + 16, 0); wr32(pkt + n + 20, JKR_LIST_FLAGS); n += 24; }
            sendto(sk, (const char*)pkt, (int)n, 0, (struct sockaddr*)&to, sizeof(to));
            lastSend = jkr_NowMs();
        }
#ifdef _WIN32
        {
            fd_set rs;
            struct timeval tv = { 0, 20000 };
            FD_ZERO(&rs);
            FD_SET(sk, &rs);
            if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        }
#else
        {
            struct pollfd pfd = { sk, POLLIN, 0 };
            if (poll(&pfd, 1, 20) <= 0) continue;
        }
#endif
        int len = (int)recvfrom(sk, (char*)rx, sizeof(rx), 0, NULL, NULL);
        if (len < JKR_HDR_SIZE || rd32(rx) != JKR_MAGIC || rd32(rx + 12) != seq) continue;
        if (!token)
        {
            if (rx[4] != JKR_WELCOME || len < JKR_HDR_SIZE + 4) continue;
            token = rd32(rx + JKR_HDR_SIZE);
            seq = 2;
            lastSend = 0;   // LIST right away
        }
        else if (rx[4] == JKR_LISTREPLY && len >= JKR_HDR_SIZE + 4)
        {
            int count = rd16(rx + JKR_HDR_SIZE + 2);
            int bAddrs = (rd16(rx + 6) & JKR_LR_ADDRS) != 0;
            int bTails = (rd16(rx + 6) & JKR_LR_TAILS) != 0;
            const uint8_t* p = rx + JKR_HDR_SIZE + 4;
            result = rd16(rx + JKR_HDR_SIZE);
            if (!count) result = 0;
            if (result > count) result = count;   // the first page is plenty for this
            for (int i = 0; i < result; i++)
            {
                dps_session tmp;
                size_t dn = jkr_GetDesc(p, rx + len, i < maxOut ? &out[i] : &tmp);
                if (!dn) { result = i; break; }
                p += dn;
                if (addrs && i < maxOut)
                {
                    addrs[i] = to;   // hosted on this server
                    if (bAddrs && rx + len - p >= JKR_LIST_ADDR_SIZE && (p[6] & JKR_LA_SERVER))
                    {
                        addrs[i].sin_family = AF_INET;
                        addrs[i].sin_addr.s_addr = htonl(rd32(p));
                        addrs[i].sin_port = htons(rd16(p + 4));
                        if (p[6] & JKR_LA_AT_SERVER)
                            addrs[i].sin_addr = to.sin_addr;
                    }
                }
                p += bAddrs ? JKR_LIST_ADDR_SIZE : 0;
                if (tails && i < maxOut) tails[i][0] = 0;
                if (bTails)
                {
                    size_t tn = jkr_GetTail(p, rx + len, tails && i < maxOut ? tails[i] : NULL);
                    if (!tn) { result = i + 1; break; }
                    p += tn;
                }
            }
        }
    }
    jkr_closesock(sk);
done:
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}

#ifndef JKRELAY_NO_RCON
// Console "rcon" (dps_Rcon): a command for a dedicated server's own server
// ("host[:port]", JKR_DEFAULT_SERVER_PORT if none), on a socket of its own. Its
// output goes to out. 1 if it answered, 0 if not within timeoutMs.
// guid: the game it's for (NULL: the server's own), so a relay server can pass it to
// that game's host (jkrelay_proto.h, "Rcon through a relay").
int jkrelay_Rcon(const char* server, const uint8_t* guid, const char* password, const char* cmd, char* out, size_t cap, int timeoutMs)
{
    struct sockaddr_in to;
    jkr_sock sk;
    uint8_t pkt[JKR_HDR_SIZE + 50 + JKR_RCON_MAX_CMD], rx[JKR_MAX_PACKET], nonce[16];
    uint32_t seq = (uint32_t)jkr_NowMs() * 2654435761u | 1;
    uint64_t start, lastSend = 0;
    size_t cmdLen = strlen(cmd), outLen = 0;
    int stage = 0, result = 0;   // 0 asking for a challenge, 1 command sent

    if (cap) out[0] = 0;
    if (cmdLen > JKR_RCON_MAX_CMD) cmdLen = JKR_RCON_MAX_CMD;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif
    if (!server || !jkr_ResolveAddr(server, JKR_DEFAULT_SERVER_PORT, &to)) goto done;
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk == JKR_BADSOCK) goto done;
    start = jkr_NowMs();
    while (!result && jkr_NowMs() - start < (uint64_t)(stage ? timeoutMs : 3000))   // no challenge in 3 s: nobody there
    {
        if (jkr_NowMs() - lastSend >= 500)
        {
            size_t n = JKR_HDR_SIZE;
            memset(pkt, 0, JKR_HDR_SIZE);
            wr32(pkt, JKR_MAGIC);
            pkt[5] = JKR_VERSION;
            wr32(pkt + 12, seq);
            if (stage == 0)
            {
                pkt[4] = JKR_RCON_CHALLENGE;
                memset(pkt + n, 0, JKR_RCON_PAD);
                if (guid) { memcpy(pkt + n, "JKRT", 4); memcpy(pkt + n + 4, guid, 16); }
                n += JKR_RCON_PAD;
            }
            else
            {
                uint8_t msg[16 + JKR_RCON_MAX_CMD];
                pkt[4] = JKR_RCON;
                memcpy(msg, nonce, 16);
                memcpy(msg + 16, cmd, cmdLen);
                memcpy(pkt + n, nonce, 16);
                jkr_HmacSha256(password, msg, 16 + cmdLen, pkt + n + 16);
                wr16(pkt + n + 48, (uint16_t)cmdLen);
                memcpy(pkt + n + 50, cmd, cmdLen);
                n += 50 + cmdLen;
                if (guid) { memcpy(pkt + n, "JKRT", 4); memcpy(pkt + n + 4, guid, 16); n += JKR_RCON_TAG_SIZE; }
            }
            sendto(sk, (const char*)pkt, (int)n, 0, (struct sockaddr*)&to, sizeof(to));
            lastSend = jkr_NowMs();
            if (stage == 1) lastSend += 100000;   // a command is sent once (it may change things)
        }
#ifdef _WIN32
        {
            fd_set rs;
            struct timeval tv = { 0, 20000 };
            FD_ZERO(&rs);
            FD_SET(sk, &rs);
            if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        }
#else
        {
            struct pollfd pfd = { sk, POLLIN, 0 };
            if (poll(&pfd, 1, 20) <= 0) continue;
        }
#endif
        int len = (int)recvfrom(sk, (char*)rx, sizeof(rx), 0, NULL, NULL);
        if (len < JKR_HDR_SIZE || rd32(rx) != JKR_MAGIC || rd32(rx + 12) != seq) continue;
        if (stage == 0 && rx[4] == JKR_RCON_NONCE && len >= JKR_HDR_SIZE + 16)
        {
            memcpy(nonce, rx + JKR_HDR_SIZE, 16);
            stage = 1;
            lastSend = 0;
            start = jkr_NowMs();
        }
        else if (rx[4] == JKR_RCON_OUTPUT && len >= JKR_HDR_SIZE + 2)
        {
            // Parts come in order on most networks; a lost one leaves a gap.
            size_t n = rd16(rx + JKR_HDR_SIZE);
            if (JKR_HDR_SIZE + 2 + (int)n > len) n = (size_t)(len - JKR_HDR_SIZE - 2);
            if (outLen + n + 1 > cap) n = cap > outLen + 1 ? cap - outLen - 1 : 0;
            memcpy(out + outLen, rx + JKR_HDR_SIZE + 2, n);
            outLen += n;
            out[outLen] = 0;
            if (rd16(rx + 6) & JKR_RCON_LAST) result = 1;
            start = jkr_NowMs();   // more parts may follow
        }
    }
    if (!result && outLen) result = 1;   // the last part got lost
    jkr_closesock(sk);
done:
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
#endif

// After a game on a listed server: back to the relay server (resolved and
// handshaken again next time).
static void jkr_LeaveDirect(void)
{
    if (!R.bDirect) return;
    R.bDirect = 0;
    R.serverResolved = 0;
    R.token = 0;
}

// ---------------------------------------------------------------------------
// Transport API
// ---------------------------------------------------------------------------

void jkrelay_SetServer(const char* server)
{
    if (!server) server = "";
#ifdef __EMSCRIPTEN__
    if (server[0]) jkr_WsUrlFor(server, jkr_wsGateway, sizeof(jkr_wsGateway));
    else jkr_wsGateway[0] = 0;
#endif
    if (!strcmp(server, R.server)) return;
    strncpy(R.server, server, sizeof(R.server) - 1);
    R.server[sizeof(R.server) - 1] = 0;
    R.serverResolved = 0;
    R.token = 0;            // new server, new handshake
    R.numSessions = 0;
}

long dps_Startup(const uint8_t appGuid[16])
{
    const char* lg = getenv("OPENJKDF2_DPLAY_LOG");
    const char* env = getenv("OPENJKDF2_RELAY");
    char keep[256];

    if (R.started) return JKR_OK_HR;
    memcpy(keep, R.server, sizeof(keep));   // the dispatcher may set it before startup
    memset(&R, 0, sizeof(R));
    memcpy(R.server, keep, sizeof(R.server));
    R.sock = JKR_BADSOCK;
    memcpy(R.appGuid, appGuid, 16);
    R.log = lg && *lg && *lg != '0';
    R.startMs = jkr_NowMs();
    if (env && *env) jkrelay_SetServer(env);

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return JKR_ERR_UNAVAILABLE;
    }
#endif
    R.sock = socket(AF_INET, SOCK_DGRAM, 0);   // ephemeral port: outbound only
    if (R.sock == JKR_BADSOCK) return JKR_ERR_UNAVAILABLE;
    {
        // Bound now so its port is known before the first send (the LAN address
        // for direct links).
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(R.sock, (struct sockaddr*)&a, sizeof(a));
    }
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(R.sock, FIONBIO, &nb); }
#else
    { int fl = fcntl(R.sock, F_GETFL, 0); fcntl(R.sock, F_SETFL, fl | O_NONBLOCK); }
#endif
    {
        // Added (2026-11-05): room for a map download's bursts. A host sends a part of a
        // block as a run of packets at once, and the game reads its socket once a frame;
        // Windows' small default buffer dropped some of every run, so the block was asked
        // for again and lost again: downloads froze on Windows.
        // OPENJKDF2_RCVBUF=<bytes> sets another size (tests: a small one acts as Windows').
        const char* env = getenv("OPENJKDF2_RCVBUF");
        int want = env && atoi(env) > 0 ? atoi(env) : JKR_RCVBUF_BYTES;
        setsockopt(R.sock, SOL_SOCKET, SO_RCVBUF, (const char*)&want, sizeof(want));
    }
    R.started = 1;
    return JKR_OK_HR;
}

void dps_Shutdown(void)
{
    if (!R.started) return;
    dps_Close();
    free(R.evLast);
    R.evLast = NULL;
    jkr_closesock(R.sock);
    R.sock = JKR_BADSOCK;
#ifdef _WIN32
    WSACleanup();
#endif
    R.started = 0;
}

long dps_Recreate(void)
{
    dps_Close();
    R.numSessions = 0;
    return JKR_OK_HR;
}

long dps_EnumConnections(dps_connection_cb cb, void* ctx)
{
    (void)cb; (void)ctx;   // the dispatcher lists the relay itself
    return JKR_OK_HR;
}

long dps_InitializeConnection(const void* connection)
{
    (void)connection;
    if (!R.started) return JKR_ERR_UNAVAILABLE;
    if (!R.server[0])
        jkr_Log(1, "no relay server set: type its address in the join screen's address box, "
                   "or set OPENJKDF2_RELAY=host[:port]\n");
    return JKR_OK_HR;
}

void dps_SetEnumHost(const char* host)
{
    (void)host;   // the dispatcher routes the address box to dps_SetRelayServer
}

static void jkr_RequestList(int waitMs)
{
    uint8_t body[24];
    int t, total, count, got = 0;
    dps_session list[JKR_MAX_SESSIONS];
    struct sockaddr_in addrs[JKR_MAX_SESSIONS];
    static char tails[JKR_MAX_SESSIONS][JKR_MAX_TAIL + 1];

    if (!jkr_Hello(waitMs)) return;
    memset(addrs, 0, sizeof(addrs));
    memset(tails, 0, sizeof(tails));
    do
    {
        memcpy(body, R.appGuid, 16);
        wr32(body + 16, (uint32_t)got);
        wr32(body + 20, JKR_LIST_FLAGS);   // and the servers listed on it, with their addresses
        t = jkr_Request(JKR_LIST, body, sizeof(body), waitMs);
        if (t != JKR_LISTREPLY || R.replyLen < JKR_HDR_SIZE + 4) return;   // keep the old list
        const uint8_t* p = R.reply + JKR_HDR_SIZE;
        const uint8_t* end = R.reply + R.replyLen;
        int bAddrs = (rd16(R.reply + 6) & JKR_LR_ADDRS) != 0;   // an older server doesn't send them
        int bTails = (rd16(R.reply + 6) & JKR_LR_TAILS) != 0;
        int bGateway = (rd16(R.reply + 6) & JKR_LR_GATEWAY) != 0;   // browsers reach listed servers through it
        total = rd16(p);
        count = rd16(p + 2);
        p += 4;
        for (int i = 0; i < count && got < JKR_MAX_SESSIONS; i++)
        {
            size_t n = jkr_GetDesc(p, end, &list[got]);
            if (!n) break;
            p += n;
            int bSkip = 0;
            if (bAddrs)
            {
                if (end - p < JKR_LIST_ADDR_SIZE) break;
                if (p[6] & JKR_LA_SERVER)
                {
                    addrs[got].sin_family = AF_INET;
                    addrs[got].sin_addr.s_addr = htonl(rd32(p));
                    addrs[got].sin_port = htons(rd16(p + 4));
#ifdef __EMSCRIPTEN__
                    bSkip = !bGateway;   // a browser can't reach it but through the gateway
#else
                    (void)bGateway;
                    if (p[6] & JKR_LA_AT_SERVER)
                        addrs[got].sin_addr = R.serverAddr.sin_addr;   // next to the relay server: where we reach it
#endif
                }
                p += JKR_LIST_ADDR_SIZE;
            }
            if (bTails)
            {
                size_t tn = jkr_GetTail(p, end, tails[got]);
                if (!tn) break;
                p += tn;
            }
            if (bSkip) { memset(&addrs[got], 0, sizeof(addrs[got])); continue; }
            got++;
        }
        if (!count) break;
    } while (got < total && got < JKR_MAX_SESSIONS);

    memcpy(R.sessions, list, sizeof(dps_session) * (size_t)got);
    memcpy(R.sessionAddr, addrs, sizeof(addrs[0]) * (size_t)got);
    memcpy(R.sessionTail, tails, sizeof(tails[0]) * (size_t)got);
    R.numSessions = got;
    R.numRelaySessions = got;
    R.lastListMs = jkr_NowMs();
}

// "host[:port]" naming the same server as R.server (the default port if none)?
static int jkr_SameServer(const char* a, const char* b)
{
    char x[160], y[160];
    snprintf(x, sizeof(x), strchr(a, ':') ? "%s" : "%s:%d", a, JKR_DEFAULT_PORT);
    snprintf(y, sizeof(y), strchr(b, ':') ? "%s" : "%s:%d", b, JKR_DEFAULT_PORT);
    for (char* c = x; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char* c = y; *c; c++) *c = (char)tolower((unsigned char)*c);
    return !strcmp(x, y);
}

// The games the master servers know (net_masterServers), after the relay server's,
// each once. Joining one goes to its own address (dps_OpenJoin).
static void jkr_AddMasterLists(void)
{
    dps_session list[JKR_MAX_SESSIONS];
    struct sockaddr_in addrs[JKR_MAX_SESSIONS];
    static char tails[JKR_MAX_SESSIONS][JKR_MAX_TAIL + 1];

    R.numSessions = R.numRelaySessions;
    for (int m = 0; m < R.numMasters; m++)
    {
        int got;
        if (R.server[0] && jkr_SameServer(R.masters[m], R.server)) continue;   // listed already
        if (R.masterRetryMs[m] && jkr_NowMs() < R.masterRetryMs[m]) continue;
        got = jkr_ProbeList(R.masters[m], R.appGuid, JKR_MASTER_WAIT_MS, list, addrs, tails, JKR_MAX_SESSIONS);
        if (got < 0)
        {
            R.masterRetryMs[m] = jkr_NowMs() + JKR_MASTER_RETRY_MS;
            jkr_Log(0, "master server %s didn't answer\n", R.masters[m]);
            continue;
        }
        R.masterRetryMs[m] = 0;
        for (int i = 0; i < got && R.numSessions < JKR_MAX_SESSIONS; i++)
        {
            int dup = 0;
            for (int j = 0; j < R.numSessions && !dup; j++)
                dup = !memcmp(R.sessions[j].guidInstance, list[i].guidInstance, 16);
            if (dup) continue;
            R.sessions[R.numSessions] = list[i];
            R.sessionAddr[R.numSessions] = addrs[i];
            memcpy(R.sessionTail[R.numSessions], tails[i], sizeof(tails[i]));
            R.numSessions++;
        }
    }
}

long dps_EnumSessions(dps_session_cb cb, void* ctx)
{
    if (!R.started) return JKR_ERR_UNAVAILABLE;
    if (!R.server[0] && !R.numMasters) return JKR_OK_HR;   // nothing to search yet
    if (R.server[0])
        jkr_RequestList(JKR_LIST_WAIT_MS);
    else
        R.numRelaySessions = 0;
    if (R.numRelaySessions > R.numSessions) R.numRelaySessions = R.numSessions;
    jkr_AddMasterLists();
    for (int i = 0; i < R.numSessions; i++)
        if (!cb(ctx, &R.sessions[i])) break;
    return JKR_OK_HR;
}

long dps_StopEnumSessions(void) { return JKR_OK_HR; }

long dps_OpenCreate(const dps_session* desc)
{
    uint8_t body[JKR_MAX_PACKET];
    size_t n;
    int t;

    if (!R.started) return JKR_ERR_UNAVAILABLE;
    if (R.inSession) return JKR_ERR_ACCESSDENIED;
    if (!R.server[0])
    {
        jkr_Log(1, "can't host: no relay server set (join screen address box, or OPENJKDF2_RELAY)\n");
        return JKR_ERR_UNAVAILABLE;
    }
    if (!jkr_Hello(JKR_REQUEST_TIMEOUT_MS))
    {
        jkr_Log(1, "can't host: relay server %s not answering\n", R.server);
        return JKR_ERR_UNAVAILABLE;
    }
    memcpy(body, R.appGuid, 16);
    n = 16 + jkr_PutDesc(body + 16, desc, 0);
    n += jkr_PutStr(body + n, desc->password, JKR_MAX_PASSWORD);
    n += jkr_PutJoinExt(body + n);
    t = jkr_Request(JKR_HOST, body, n, JKR_REQUEST_TIMEOUT_MS);
    if (t != JKR_HOSTED) return jkr_Fail(t);

    jkr_ResetSession();
    {
        const uint8_t* p = R.reply + JKR_HDR_SIZE;
        const uint8_t* end = R.reply + R.replyLen;
        size_t dn = jkr_GetDesc(p, end, &R.desc);
        if (!dn || p + dn + 4 > end) return JKR_ERR_GENERIC;
        R.localSysId = rd32(p + dn);
    }
    memcpy(R.desc.password, desc->password, sizeof(R.desc.password));
    jkr_player* me = jkr_Add(R.localSysId);
    me->local = 1;
    me->flags = JKR_PF_SYSTEM | JKR_PF_HOST;
    me->sysId = R.localSysId;
    R.inSession = 1;
    R.isHost = 1;
    R.lastRxMs = jkr_NowMs();
    jkr_Log(1, "hosting on relay server %s\n", R.server);
    if (R.hostTail[0]) jkr_SendDescNow();   // our mod (jkrelay_SetHostTail), which HOST doesn't carry
    if (R.numHostBans) jkr_SendBansNow();   // and our bans (jkrelay_HostSetBans)
    return JKR_OK_HR;
}

long dps_OpenJoin(const uint8_t guidInstance[16], const uint16_t* password)
{
    uint8_t body[64 + 2 * JKR_MAX_PASSWORD + JKR_JOIN_EXT_SIZE];
    size_t n;
    int t, expected = 0;
    uint64_t start;

    if (!R.started) return JKR_ERR_UNAVAILABLE;
    if (R.inSession) return JKR_ERR_ACCESSDENIED;
    R.failReason = R.failMinutes = 0;
    R.lostReason = R.lostMinutes = 0;
    // A server listed on the relay server, or a game a master server listed: join it at its own address.
    for (int i = 0; i < R.numSessions; i++)
        if (!memcmp(R.sessions[i].guidInstance, guidInstance, 16) && R.sessionAddr[i].sin_port)
        {
            R.serverAddr = R.sessionAddr[i];
            R.serverResolved = 1;
            R.token = 0;
            R.bDirect = 1;
            jkr_Log(1, "joining a listed game at %s:%u\n", inet_ntoa(R.serverAddr.sin_addr), ntohs(R.serverAddr.sin_port));
            break;
        }
    if (!jkr_Hello(JKR_REQUEST_TIMEOUT_MS)) { jkr_LeaveDirect(); return JKR_ERR_UNAVAILABLE; }
    memcpy(body, guidInstance, 16);
    n = 16 + jkr_PutStr(body + 16, password, JKR_MAX_PASSWORD);
    n += jkr_PutJoinExt(body + n);

    jkr_ResetSession();
    R.inSession = 1;                  // so the initial events are accepted
    t = jkr_Request(JKR_JOIN, body, n, JKR_REQUEST_TIMEOUT_MS);
    if (t != JKR_JOINED) { long hr = jkr_Fail(t); jkr_ResetSession(); jkr_LeaveDirect(); return hr; }
    {
        const uint8_t* p = R.reply + JKR_HDR_SIZE;
        const uint8_t* end = R.reply + R.replyLen;
        size_t dn = jkr_GetDesc(p, end, &R.desc);
        if (!dn || p + dn + 8 > end) { jkr_ResetSession(); jkr_LeaveDirect(); return JKR_ERR_GENERIC; }
        R.localSysId = rd32(p + dn);
        expected = (int)rd32(p + dn + 4);
    }
    // Kept for JKR_REJOIN.
    for (size_t i = 0; password && i + 1 < sizeof(R.desc.password) / sizeof(R.desc.password[0]) && password[i]; i++)
        R.desc.password[i] = password[i];
    jkr_player* me = jkr_Add(R.localSysId);
    me->local = 1;
    me->flags = JKR_PF_SYSTEM;
    me->sysId = R.localSysId;

    // Wait for everyone already in the game (they arrive as events).
    start = jkr_NowMs();
    while (R.initialSeen < expected && jkr_NowMs() - start < 5000) jkr_Pump(20);
    R.lastRxMs = jkr_NowMs();
    if (R.bDirect)
        jkr_Log(1, "joined the server at %s:%u\n", inet_ntoa(R.serverAddr.sin_addr), ntohs(R.serverAddr.sin_port));
    else
        jkr_Log(1, "joined via relay server %s\n", R.server);
    return JKR_OK_HR;
}

// Called while menus are up (stats screen, dialogs), when the game doesn't
// call Receive: keeps the server from dropping us after JKR_TIMEOUT_MS of
// silence and acknowledges its events. Packets are queued for Receive.
void jkrelay_Idle(void)
{
    if (R.started && R.inSession) jkr_Pump(0);
}

// Why the server ended our last game (JKR_LOST_*; 0: the host left or nothing said),
// and why it refused our last join. *pMinutes: a ban's minutes (0 = for good).
int jkrelay_LostReason(int* pMinutes)
{
    if (pMinutes) *pMinutes = R.lostMinutes;
    return R.lostReason;
}

int jkrelay_JoinRefusal(int* pMinutes)
{
    if (pMinutes) *pMinutes = R.failMinutes;
    return R.failReason;
}

// net_masterServers: "host[:port]" master servers, separated by commas or spaces, asked
// for their games too (jkr_AddMasterLists).
void jkrelay_SetMasters(const char* list)
{
    char tmp[1024];
    int n = 0;
    snprintf(tmp, sizeof(tmp), "%s", list ? list : "");
    for (char* tok = strtok(tmp, ", \t;"); tok && n < JKR_MAX_MASTERS; tok = strtok(NULL, ", \t;"))
    {
        if (n < R.numMasters && !strcmp(R.masters[n], tok))
        {
            n++;   // unchanged: keep its retry time
            continue;
        }
        snprintf(R.masters[n], sizeof(R.masters[n]), "%s", tok);
        R.masterRetryMs[n] = 0;
        n++;
    }
    R.numMasters = n;
}

// net_directLinks: ask for direct links in the next game hosted or joined.
void jkrelay_SetDirect(int on)
{
#ifdef __EMSCRIPTEN__
    on = 0;   // a browser can't punch through to anyone
#endif
    R.wantDirect = on ? 1 : 0;
}

// For "gameinfo": a listed server joined at its own address ("ip:port"), 0 if not.
int jkrelay_DescribeDirect(char* out, size_t cap)
{
    if (!R.bDirect || !R.inSession || !cap) return 0;
    snprintf(out, cap, "%s:%u", inet_ntoa(R.serverAddr.sin_addr), ntohs(R.serverAddr.sin_port));
    return 1;
}

// For "gameinfo": whether game packets use direct links or the relay server.
void jkrelay_DescribeLinks(char* out, size_t cap)
{
    int peers = 0, direct = 0;
    if (!cap) return;
    out[0] = 0;
    if (!R.inSession) return;
    if (!R.wantDirect)
    {
        snprintf(out, cap, "Packets go through the relay server (net_directLinks is 0)");
        return;
    }
    if (!R.isHost)
    {
        jkr_link* l = NULL;
        for (int i = 0; i < JKR_MAX_PLAYERS && !l; i++) if (R.links[i].used) l = &R.links[i];
        if (!l)
            snprintf(out, cap, "Packets go through the relay server (no direct link offered)");
        else
            snprintf(out, cap, jkr_LinkUsable(l) ? "Packets go over a direct link to the host"
                                                 : "Packets go through the relay server (no direct link to the host yet)");
        return;
    }
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* m = &R.players[i];
        if (!m->used || m->local || !(m->flags & JKR_PF_SYSTEM)) continue;
        peers++;
        if (jkr_UpLink(m->id)) direct++;
    }
    if (!peers)
        snprintf(out, cap, "No one has joined yet");
    else if (direct == peers)
        snprintf(out, cap, "All %d joiners on direct links", peers);
    else
        snprintf(out, cap, "%d of %d joiners on direct links, the rest through the relay server", direct, peers);
}

long dps_Close(void)
{
    if (R.inSession && !R.lostSignalled && R.token)
        jkr_Request(JKR_LEAVE, NULL, 0, 1000);   // best effort; the server times us out otherwise
    jkr_ResetSession();
    jkr_LeaveDirect();
    return JKR_OK_HR;
}

static uint32_t jkr_CountPlayers(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (R.players[i].used && !(R.players[i].flags & JKR_PF_SYSTEM)) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Resuming (jkrelay_proto.h, "Resuming")
// ---------------------------------------------------------------------------

static void jkr_ResumeStart(void)
{
    jkr_Log(1, "the relay server lost this game (restarted?); putting it back\n");
    R.resuming = 1;
    R.resumeSeq = 0;
    R.resumeStartMs = jkr_NowMs();
    R.resumeTxMs = 0;
    R.resumedMs = 0;
    R.token = 0;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (R.players[i].used && !R.players[i].local) R.players[i].seen = 0;
}

// Our own players, the system player first: u8 count, (u32 id, u32 flags, name) each.
static size_t jkr_PutOwnPlayers(uint8_t* p)
{
    size_t n = 1;
    int count = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        {
            jkr_player* m = &R.players[i];
            if (!m->used || !m->local || ((m->flags & JKR_PF_SYSTEM) != 0) != (pass == 0)) continue;
            wr32(p + n, m->id);
            wr32(p + n + 4, m->flags);
            n += 8 + jkr_PutStr(p + n + 8, m->name, 31);
            count++;
        }
    p[0] = (uint8_t)count;
    return n;
}

static void jkr_ResumeSend(uint64_t now)
{
    uint8_t pkt[JKR_MAX_PACKET];
    size_t n = JKR_HDR_SIZE;
    uint8_t type;

    // Retries of one request keep its seq (the server answers repeats from
    // its cache); a new attempt after a FAIL gets a new one.
    if (!R.resumeSeq) R.resumeSeq = ++R.nextSeq;
    if (R.resuming == 1)
    {
        type = JKR_HELLO;
        wr32(pkt + n, JKR_VERSION);
        n += 4;
    }
    else if (R.isHost)
    {
        uint32_t next = 0;
        for (int i = 0; i < JKR_MAX_PLAYERS; i++)
            if (R.players[i].used && R.players[i].id >= next) next = R.players[i].id + 1;
        type = JKR_REHOST;
        memcpy(pkt + n, R.appGuid, 16);
        n += 16;
        n += jkr_PutDesc(pkt + n, &R.desc, jkr_CountPlayers());
        n += jkr_PutStr(pkt + n, R.desc.password, JKR_MAX_PASSWORD);
        n += jkr_PutJoinExt(pkt + n);
        wr32(pkt + n, next);   // new players get ids none of the old ones have
        n += 4;
        n += jkr_PutOwnPlayers(pkt + n);
    }
    else
    {
        type = JKR_REJOIN;
        memcpy(pkt + n, R.desc.guidInstance, 16);
        n += 16;
        n += jkr_PutStr(pkt + n, R.desc.password, JKR_MAX_PASSWORD);
        n += jkr_PutJoinExt(pkt + n);
        n += jkr_PutOwnPlayers(pkt + n);
    }
    jkr_PutHdr(pkt, type, R.resumeSeq);
    jkr_SendRaw(pkt, n);
    R.resumeTxMs = now;
}

static void jkr_ResumeReply(const uint8_t* p, int len)
{
    uint8_t type = p[4];
    uint64_t now = jkr_NowMs();

    R.resumeSeq = 0;
    if (R.resuming == 1)
    {
        if (type != JKR_WELCOME || len < JKR_HDR_SIZE + 4) return;
        R.token = rd32(p + JKR_HDR_SIZE);
        R.lastEvent = 0;            // a new client to the server: its events start over
        R.resuming = 2;
        R.resumeTxMs = 0;           // REHOST / REJOIN right away
        return;
    }
    if (type == JKR_OK)
    {
        R.resuming = 0;
        R.resumedMs = now;
        R.serverQuiet = 0;
        jkr_Log(1, "game back on relay server %s (%.1f s)\n", R.server, (now - R.resumeStartMs) / 1000.0);
        if (R.isHost && R.hostTail[0]) jkr_SendDescNow();   // our mod: REHOST doesn't carry it
        if (R.isHost && R.numHostBans) jkr_SendBansNow();   // and our bans
        return;
    }
    if (type == JKR_FAIL)
    {
        uint32_t hr = len >= JKR_HDR_SIZE + 4 ? rd32(p + JKR_HDR_SIZE) : 0;
        if (!R.isHost && hr == (uint32_t)JKR_ERR_NOSESSIONS) return;   // the host isn't back yet: try again
        R.resuming = 0;
        if (R.isHost)
            jkr_Log(1, "couldn't put the game back on relay server %s (%08x); new players can't find it\n", R.server, hr);
        else
            jkr_SessionLost("couldn't rejoin the game after the relay server restarted");
    }
}

static void jkr_ResumeTick(uint64_t now)
{
    if (R.resuming)
    {
        // A host waits for the server as long as it takes; a joiner, unless
        // the game still reaches it over its link to the host.
        if (!R.isHost && now - R.resumeStartMs > JKR_RESUME_MS && !jkr_AnyLinkUp())
        {
            jkr_SessionLost("the game didn't come back on the relay server");
            return;
        }
        if (now - R.resumeTxMs >= JKR_RESUME_RETRY_MS) jkr_ResumeSend(now);
        return;
    }
    if (now - R.resumedMs <= JKR_REJOIN_WINDOW_MS) return;
    // Whoever the server hasn't told us about again by now didn't come back.
    R.resumedMs = 0;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        char who[48];
        jkr_player* m = &R.players[i];
        if (!m->used || m->local || m->seen || !(m->flags & JKR_PF_SYSTEM)) continue;
        jkr_MachineName(m->id, who, sizeof(who));
        jkr_Log(1, "%s didn't come back after the relay server restart\n", who);
    }
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* m = &R.players[i];
        if (!m->used || m->local || m->seen) continue;
        if (!(m->flags & JKR_PF_SYSTEM))
            jkr_Push(DPS_RECV_SYS, 0, NULL, 0, DPS_SYS_PLAYERDESTROYED, m->id);
        else
            jkr_LinkRemove(m->id);
        m->used = 0;
    }
}

// JKR_SETDESC's body: the desc, the password, and our mod (jkrelay_proto.h, "Mods").
static size_t jkr_PutDescBody(uint8_t* body, const dps_session* d)
{
    size_t n = jkr_PutDesc(body, d, jkr_CountPlayers());
    size_t tl = strlen(R.hostTail);
    n += jkr_PutStr(body + n, d->password, JKR_MAX_PASSWORD);
    body[n++] = (uint8_t)tl;
    memcpy(body + n, R.hostTail, tl);
    return n + tl;
}

// The same, without waiting for the answer (from inside packet handling).
static void jkr_SendDescNow(void)
{
    uint8_t pkt[JKR_MAX_PACKET];
    size_t n;
    if (!R.inSession || !R.isHost || R.resuming) return;
    n = jkr_PutDescBody(pkt + JKR_HDR_SIZE, &R.desc);
    jkr_PutHdr(pkt, JKR_SETDESC, ++R.nextSeq);
    jkr_SendRaw(pkt, JKR_HDR_SIZE + n);
}

// Our mod, as the web lobby says it (jkmod.c's tail), which the game list shows
// joiners when we host ("" for none said).
void jkrelay_SetHostTail(const char* hex)
{
    if (!hex) hex = "";
    if (!strcmp(hex, R.hostTail)) return;
    snprintf(R.hostTail, sizeof(R.hostTail), "%s", hex);
    jkr_SendDescNow();
}

// The mod a listed game's host runs, or NULL if its server or host doesn't say.
const char* jkrelay_GetSessionTail(const uint8_t guid[16])
{
    for (int i = 0; i < R.numSessions; i++)
        if (!memcmp(R.sessions[i].guidInstance, guid, 16))
            return R.sessionTail[i][0] ? R.sessionTail[i] : NULL;
    return NULL;
}

long dps_GetSessionDesc(dps_session* out)
{
    if (!R.inSession) return JKR_ERR_NOCONNECTION;
    *out = R.desc;
    out->curPlayers = jkr_CountPlayers();
    return JKR_OK_HR;
}

long dps_SetSessionDesc(const dps_session* in)
{
    uint8_t body[JKR_MAX_PACKET];
    size_t n;
    dps_session d;
    if (!R.inSession) return JKR_ERR_NOCONNECTION;
    if (!R.isHost) return JKR_ERR_ACCESSDENIED;
    d = *in;
    memcpy(d.guidInstance, R.desc.guidInstance, 16);
    if (R.resuming)
    {
        R.desc = d;   // JKR_REHOST carries it
        return JKR_OK_HR;
    }
    n = jkr_PutDescBody(body, &d);
    if (jkr_Request(JKR_SETDESC, body, n, JKR_REQUEST_TIMEOUT_MS) != JKR_OK) return JKR_ERR_GENERIC;
    R.desc = d;
    return JKR_OK_HR;
}

long dps_CreatePlayer(const uint16_t* shortName, uint32_t* idOut)
{
    uint8_t body[8 + 2 * JKR_MAX_NAME];
    size_t n;
    int t;
    uint32_t id;
    jkr_player* p;

    if (!R.inSession) return JKR_ERR_NOCONNECTION;
    wr32(body, 0);
    n = 4 + jkr_PutStr(body + 4, shortName, 31);
    t = jkr_Request(JKR_CREATEPLAYER, body, n, JKR_REQUEST_TIMEOUT_MS);
    if (t != JKR_PLAYERID || R.replyLen < JKR_HDR_SIZE + 4) return jkr_Fail(t);
    id = rd32(R.reply + JKR_HDR_SIZE);
    p = jkr_Add(id);
    if (!p) return JKR_ERR_GENERIC;
    p->local = 1;
    p->sysId = R.localSysId;
    p->flags = 0;
    for (size_t i = 0; shortName && i < 31 && shortName[i]; i++) p->name[i] = shortName[i];
    if (idOut) *idOut = id;
    return JKR_OK_HR;
}

long dps_DestroyPlayer(uint32_t id)
{
    uint8_t body[4];
    jkr_player* p = jkr_Find(id);
    if (!p || !p->local) return JKR_ERR_INVALIDPLAYER;
    wr32(body, id);
    jkr_Request(JKR_DESTROYPLAYER, body, sizeof(body), 2000);
    p->used = 0;
    return JKR_OK_HR;
}

long dps_EnumPlayers(dps_player_cb cb, void* ctx)
{
    if (!R.inSession) return JKR_ERR_NOCONNECTION;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* p = &R.players[i];
        if (!p->used || (p->flags & JKR_PF_SYSTEM)) continue;
        if (!cb(ctx, p->id, p->name[0] ? p->name : NULL, NULL)) break;
    }
    return JKR_OK_HR;
}

// One game packet, over the link if given, else through the server.
static void jkr_SendData(jkr_link* l, uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size, uint32_t seq)
{
    uint8_t pkt[JKR_MAX_PACKET];
    if (JKR_HDR_SIZE + 8 + size > sizeof(pkt)) return;
    jkr_PutHdr(pkt, JKR_DATA, seq);
    if (l) wr32(pkt + 8, l->key);
    wr32(pkt + JKR_HDR_SIZE, idFrom);
    wr32(pkt + JKR_HDR_SIZE + 4, idTo);
    memcpy(pkt + JKR_HDR_SIZE + 8, data, size);
    if (l)
    {
        sendto(R.sock, (const char*)pkt, (int)(JKR_HDR_SIZE + 8 + size), 0, (struct sockaddr*)&l->addr, sizeof(l->addr));
        l->lastTxMs = jkr_NowMs();
    }
    else if (!R.resuming)   // the server doesn't know us right now
        jkr_SendRaw(pkt, JKR_HDR_SIZE + 8 + size);
}

// Host: to one other machine, over its link or through the server. 'to' 0 =
// everyone; through the server that becomes the machine's system player.
static void jkr_SendToMachine(uint32_t sysId, uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size, uint32_t seq)
{
    jkr_link* l = jkr_UpLink(sysId);
    jkr_SendData(l, idFrom, (idTo || l) ? idTo : sysId, data, size, seq);
}

// Host: a packet for everyone ('to' 0) or one player, to every other machine
// that should get it, except 'skipSys' (where it came from).
static void jkr_HostSend(uint32_t skipSys, uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size, uint32_t seq)
{
    jkr_player* t = idTo ? jkr_Find(idTo) : NULL;
    if (idTo && !t) return;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* m = &R.players[i];
        if (!m->used || m->local || !(m->flags & JKR_PF_SYSTEM) || m->id == skipSys) continue;
        if (t && t->sysId != m->id) continue;
        jkr_SendToMachine(m->id, idFrom, idTo, data, size, seq);
    }
}

// A game packet from the server, or from a peer's link ('via').
static void jkr_HandleData(const uint8_t* b, const uint8_t* end, jkr_link* via)
{
    uint32_t from, to;
    uint32_t seq = rd32(b - JKR_HDR_SIZE + 12);   // the sender's update number (jkrelay_SetSendSeq)
    jkr_player* dst;
    if (!R.inSession || end - b < 8) return;
    from = rd32(b);
    to = rd32(b + 4);
    if (via && R.isHost)
    {
        // A joiner speaks for its own players only; the host passes it on.
        jkr_player* src = jkr_Find(from);
        if (!src || src->sysId != via->sysId) return;
        if (!to || !(dst = jkr_Find(to)) || !dst->local)
            jkr_HostSend(via->sysId, from, to, b + 8, (uint32_t)(end - b - 8), seq);
    }
    dst = to ? jkr_Find(to) : NULL;
    if (to && (!dst || !dst->local)) return;
    if (jkr_BigPiece(from, b + 8, (size_t)(end - b - 8))) return;   // a piece of a big frame, not the game's
    if (jkr_ClockPacket(from, b + 8, (size_t)(end - b - 8))) return; // a clock ping or its answer
    jkr_Push(DPS_RECV_DATA, from, b + 8, (uint32_t)(end - b - 8), 0, 0);
    if (R.evTail) R.evTail->seq = seq;
}

long dps_Send(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size)
{
    uint32_t seq = R.sendSeq;
    R.sendSeq = 0;
    if (!R.inSession) return JKR_ERR_NOCONNECTION;
    if (JKR_HDR_SIZE + 8 + size > JKR_MAX_PACKET) return JKR_ERR_GENERIC;
    if (R.isHost && jkr_AnyLinkUp())
    {
        jkr_HostSend(0, idFrom, idTo, data, size, seq);   // per machine: links, or the server
        return JKR_OK_HR;
    }
    if (!R.isHost)
    {
        // A joiner with a link sends everything to the host, which passes it on.
        for (int i = 0; i < JKR_MAX_PLAYERS; i++)
            if (jkr_LinkUsable(&R.links[i]))
            {
                jkr_SendData(&R.links[i], idFrom, idTo, data, size, seq);
                return JKR_OK_HR;
            }
    }
    jkr_SendData(NULL, idFrom, idTo, data, size, seq);
    return JKR_OK_HR;
}

int dps_Receive(uint32_t* fromOut, const void** dataOut, uint32_t* sizeOut, dps_sysmsg* sysOut)
{
    jkr_event* e;
    free(R.evLast);
    R.evLast = NULL;
    if (!R.started) return DPS_RECV_NONE;
    if (!R.evHead) jkr_Pump(0);
    e = R.evHead;
    if (!e) return DPS_RECV_NONE;
    R.evHead = e->next;
    if (!R.evHead) R.evTail = NULL;
    R.evLast = e;
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
// Big frames (2026-10-29): map and mod downloads (jkweb_xfer.c, and the browser
// build's maps.js/mods.js) send frames of up to 64 KiB, which go in pieces as
// game data: 'J' 'F' (a message id past DSS_MAX, which engines ignore) 0 0,
// u32 key, u16 index, u16 count, u32 total size, then the piece. The receiver
// puts them back together and hands the frame to jkrelay_SetBigHandler's
// function instead of the game. A frame missing a piece for 10 s is dropped
// (the transfer asks for it again).
// ---------------------------------------------------------------------------

#define JKR_BIG_HDR      16
#define JKR_BIG_PIECE    (JKR_MAX_PACKET - JKR_HDR_SIZE - 8 - JKR_BIG_HDR)
#define JKR_BIG_MAX      (64 * JKR_BIG_PIECE)
#define JKR_BIG_SLOTS    8
#define JKR_BIG_STALE_MS 10000

static struct
{
    uint32_t from, key, total;
    uint16_t count;
    uint64_t have;
    uint64_t startMs;
    uint8_t* buf;
} jkr_aBig[JKR_BIG_SLOTS];
static void (*jkr_pfnBig)(uint32_t from, const uint8_t* p, size_t n) = NULL;
static uint32_t jkr_bigKey = 0;

void jkrelay_SetBigHandler(void (*fn)(uint32_t from, const uint8_t* p, size_t n))
{
    jkr_pfnBig = fn;
}

int jkrelay_SendBig(uint32_t idFrom, uint32_t idTo, const uint8_t* p, size_t n)
{
    uint8_t piece[JKR_BIG_HDR + JKR_BIG_PIECE];
    uint16_t count = (uint16_t)((n + JKR_BIG_PIECE - 1) / JKR_BIG_PIECE);
    if (!R.inSession || !n || n > JKR_BIG_MAX) return 0;
    if (!++jkr_bigKey) jkr_bigKey = 1;
    for (uint16_t i = 0; i < count; i++)
    {
        size_t off = (size_t)i * JKR_BIG_PIECE;
        size_t len = n - off < JKR_BIG_PIECE ? n - off : JKR_BIG_PIECE;
        piece[0] = 'J'; piece[1] = 'F'; piece[2] = 0; piece[3] = 0;
        wr32(piece + 4, jkr_bigKey);
        wr16(piece + 8, i);
        wr16(piece + 10, count);
        wr32(piece + 12, (uint32_t)n);
        memcpy(piece + JKR_BIG_HDR, p + off, len);
        if (dps_Send(idFrom, idTo, piece, (uint32_t)(JKR_BIG_HDR + len)) != JKR_OK_HR) return 0;
    }
    return 1;
}

// A piece of a big frame (1, taken), or game data (0).
static int jkr_BigPiece(uint32_t from, const uint8_t* p, size_t n)
{
    uint32_t key, total;
    uint16_t idx, count;
    uint64_t now = jkr_NowMs();
    int slot = -1, oldest = 0;
    if (n < JKR_BIG_HDR || p[0] != 'J' || p[1] != 'F' || p[2] || p[3]) return 0;
    key = rd32(p + 4);
    idx = rd16(p + 8);
    count = rd16(p + 10);
    total = rd32(p + 12);
    if (!count || count > 64 || idx >= count || !total || total > JKR_BIG_MAX
        || (uint32_t)count != (total + JKR_BIG_PIECE - 1) / JKR_BIG_PIECE) return 1;
    if (n - JKR_BIG_HDR != (idx + 1 < count ? (size_t)JKR_BIG_PIECE : total - (size_t)idx * JKR_BIG_PIECE)) return 1;
    for (int i = 0; i < JKR_BIG_SLOTS; i++)
    {
        if (jkr_aBig[i].buf && now - jkr_aBig[i].startMs > JKR_BIG_STALE_MS)
        {
            free(jkr_aBig[i].buf);
            jkr_aBig[i].buf = NULL;
        }
        if (jkr_aBig[i].buf && jkr_aBig[i].from == from && jkr_aBig[i].key == key) slot = i;
        if (!jkr_aBig[i].buf || (jkr_aBig[oldest].buf && jkr_aBig[i].startMs < jkr_aBig[oldest].startMs)) oldest = i;
    }
    if (slot < 0)
    {
        slot = oldest;
        free(jkr_aBig[slot].buf);
        memset(&jkr_aBig[slot], 0, sizeof(jkr_aBig[slot]));
        if (!(jkr_aBig[slot].buf = (uint8_t*)malloc(total))) return 1;
        jkr_aBig[slot].from = from;
        jkr_aBig[slot].key = key;
        jkr_aBig[slot].total = total;
        jkr_aBig[slot].count = count;
        jkr_aBig[slot].startMs = now;
    }
    if (jkr_aBig[slot].total != total || jkr_aBig[slot].count != count) return 1;
    memcpy(jkr_aBig[slot].buf + (size_t)idx * JKR_BIG_PIECE, p + JKR_BIG_HDR, n - JKR_BIG_HDR);
    jkr_aBig[slot].have |= (uint64_t)1 << idx;
    if (jkr_aBig[slot].have == (count == 64 ? ~(uint64_t)0 : (((uint64_t)1 << count) - 1)))
    {
        uint8_t* buf = jkr_aBig[slot].buf;
        jkr_aBig[slot].buf = NULL;
        if (jkr_pfnBig) jkr_pfnBig(from, buf, total);
        free(buf);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Clocks (2026-10-29): another player's game clock less ours, which Hit what you
// see asks for (stdComm_WSRelayMotion.c), measured as the browser build's
// netstats.js does: while asked, their machine is pinged every second for 10 s,
// then every 5 s, and the answer to the ping with the shortest round trip of the
// last 16 says it, the way NTP picks. Pings go as game data the receiver takes
// before the game sees it:
//   'J' 'P' 0 0, u8 type (1 ping, 2 answer), 0 0 0, u32 seq, f64 sent, f64 got
// (the ping's time on the asking game's clock, the answering game's clock).
// The clock is the caller's (jkrelay_SetClock), in ms.
// ---------------------------------------------------------------------------

#define JKR_CLOCK_SIZE       28
#define JKR_CLOCK_FIRST_MS   10000.0
#define JKR_CLOCK_FAST_MS    1000.0
#define JKR_CLOCK_SLOW_MS    5000.0
#define JKR_CLOCK_SAMPLES    16

static double (*jkr_pfnClock)(void) = NULL;
static struct
{
    uint32_t player;           // 0: free
    double since, nextAt;      // asked for since; next ping
    uint32_t seq, sentSeq;     // the last ping
    double sentAt;
    struct { double rtt, offset; } aSample[JKR_CLOCK_SAMPLES];
    int numSamples, nextSample;
} jkr_aClock[JKR_MAX_PLAYERS];

void jkrelay_SetClock(double (*fn)(void))
{
    jkr_pfnClock = fn;
}

static void jkr_ClockSend(uint32_t toSys, int type, uint32_t seq, double sent, double got)
{
    uint8_t m[JKR_CLOCK_SIZE];
    memset(m, 0, sizeof(m));
    m[0] = 'J'; m[1] = 'P'; m[4] = (uint8_t)type;
    wr32(m + 8, seq);
    memcpy(m + 12, &sent, 8);
    memcpy(m + 20, &got, 8);
    dps_Send(R.localSysId, toSys, m, sizeof(m));
}

// Whether to measure player id's clock (on), or no longer. Each start begins afresh.
void jkrelay_SyncClock(uint32_t id, int on)
{
    int free = -1;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        if (jkr_aClock[i].player == id)
        {
            if (!on) jkr_aClock[i].player = 0;
            return;
        }
        if (free < 0 && !jkr_aClock[i].player) free = i;
    }
    if (!on || free < 0 || !jkr_pfnClock) return;
    memset(&jkr_aClock[free], 0, sizeof(jkr_aClock[free]));
    jkr_aClock[free].player = id;
    jkr_aClock[free].since = jkr_pfnClock();
}

// Their clock less ours in ms, NaN if not known (yet).
double jkrelay_ClockOffset(uint32_t id)
{
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        int best = -1;
        if (jkr_aClock[i].player != id) continue;
        for (int k = 0; k < jkr_aClock[i].numSamples; k++)
            if (best < 0 || jkr_aClock[i].aSample[k].rtt < jkr_aClock[i].aSample[best].rtt) best = k;
        return best < 0 ? NAN : jkr_aClock[i].aSample[best].offset;
    }
    return NAN;
}

static void jkr_ClockTick(void)
{
    double now;
    if (!jkr_pfnClock || !R.inSession || !R.localSysId) return;
    now = jkr_pfnClock();
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* p;
        if (!jkr_aClock[i].player || now < jkr_aClock[i].nextAt) continue;
        p = jkr_Find(jkr_aClock[i].player);
        if (!p || p->local) continue;
        jkr_aClock[i].sentSeq = ++jkr_aClock[i].seq;
        jkr_aClock[i].sentAt = now;
        jkr_aClock[i].nextAt = now + (now - jkr_aClock[i].since < JKR_CLOCK_FIRST_MS ? JKR_CLOCK_FAST_MS : JKR_CLOCK_SLOW_MS);
        jkr_ClockSend(p->sysId, 1, jkr_aClock[i].sentSeq, now, 0.0);
    }
}

// A ping or its answer (1, taken), or game data (0).
static int jkr_ClockPacket(uint32_t from, const uint8_t* p, size_t n)
{
    uint32_t seq;
    double sent, got, now;
    if (n != JKR_CLOCK_SIZE || p[0] != 'J' || p[1] != 'P' || p[2] || p[3]) return 0;
    if (!jkr_pfnClock) return 1;
    now = jkr_pfnClock();
    seq = rd32(p + 8);
    memcpy(&sent, p + 12, 8);
    memcpy(&got, p + 20, 8);
    if (p[4] == 1)
    {
        jkr_ClockSend(from, 2, seq, sent, now);   // at once, to the machine that asked
        return 1;
    }
    if (p[4] != 2) return 1;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
    {
        jkr_player* pl;
        if (!jkr_aClock[i].player || jkr_aClock[i].sentSeq != seq || jkr_aClock[i].sentAt != sent) continue;
        pl = jkr_Find(jkr_aClock[i].player);
        if (!pl || pl->sysId != from) continue;
        jkr_aClock[i].sentSeq = 0;
        jkr_aClock[i].aSample[jkr_aClock[i].nextSample].rtt = now - sent;
        jkr_aClock[i].aSample[jkr_aClock[i].nextSample].offset = got - (sent + now) / 2;
        jkr_aClock[i].nextSample = (jkr_aClock[i].nextSample + 1) % JKR_CLOCK_SAMPLES;
        if (jkr_aClock[i].numSamples < JKR_CLOCK_SAMPLES) jkr_aClock[i].numSamples++;
        jkr_Log(0, "clock of player %08x: %+.1f ms (round trip %.1f ms)\n", jkr_aClock[i].player, got - (sent + now) / 2, now - sent);
        break;
    }
    return 1;
}

// Machines, by their system players: this one's, and the host's (0 if
// unknown). Big frames go between machines, before a joiner has a player.
uint32_t jkrelay_LocalSystemId(void)
{
    return R.inSession ? R.localSysId : 0;
}

uint32_t jkrelay_HostSystemId(void)
{
    if (!R.inSession) return 0;
    if (R.isHost) return R.localSysId;
    for (int i = 0; i < JKR_MAX_PLAYERS; i++)
        if (R.players[i].used && (R.players[i].flags & (JKR_PF_SYSTEM | JKR_PF_HOST)) == (JKR_PF_SYSTEM | JKR_PF_HOST))
            return R.players[i].id;
    return 0;
}

// Whether this machine hosts the game it's in.
int jkrelay_IsHost(void)
{
    return R.inSession && R.isHost;
}

// ---------------------------------------------------------------------------
// Hosts' admin (2026-10-29, jkrelay_proto.h): a dedicated server hosting here has
// the relay server enforce its kicks and bans, and gets its rcon through it.
// ---------------------------------------------------------------------------

// The server of the game we're in, as rcon can name it: in a browser a URL (a
// dedicated server's through the gateway), else "a.b.c.d:port". 0 if none.
int jkrelay_CurrentServerName(char* out, size_t cap)
{
    if (!cap || !R.serverResolved) return 0;
#ifdef __EMSCRIPTEN__
    {
        const char* url = jkr_WsUrlOf(&R.serverAddr);
        if (!url) return 0;
        snprintf(out, cap, "%s", url);
    }
#else
    snprintf(out, cap, "%s:%u", inet_ntoa(R.serverAddr.sin_addr), (unsigned)ntohs(R.serverAddr.sin_port));
#endif
    return 1;
}

void jkrelay_SetTunnelHandler(void (*fn)(uint32_t ip, uint16_t port, const uint8_t* p, size_t n))
{
    R.pfnTunnel = fn;
}

// An answer to a tunnelled packet, to ip:port (network order) through the relay.
void jkrelay_TunnelReply(uint32_t ip, uint16_t port, const void* p, size_t n)
{
    uint8_t pkt[JKR_MAX_PACKET];
    if (!R.inSession || !R.isHost || JKR_HDR_SIZE + 6 + n > sizeof(pkt)) return;
    jkr_PutHdr(pkt, JKR_TUNNEL_REPLY, 0);
    memcpy(pkt + JKR_HDR_SIZE, &ip, 4);
    memcpy(pkt + JKR_HDR_SIZE + 4, &port, 2);
    memcpy(pkt + JKR_HDR_SIZE + 6, p, n);
    jkr_SendRaw(pkt, JKR_HDR_SIZE + 6 + n);
}

// The relay server takes player id out of our game (banMinutes < 0 a kick, else a ban
// for that long, 0 for good). 1 with its IPv4 (network order), 0 if it couldn't (an
// older relay server doesn't answer).
int jkrelay_HostKick(uint32_t id, int banMinutes, uint32_t* pIp)
{
    uint8_t body[8];
    if (!R.inSession || !R.isHost) return 0;
    wr32(body, id);
    wr32(body + 4, (uint32_t)banMinutes);
    if (jkr_Request(JKR_HOSTKICK, body, sizeof(body), 1500) != JKR_OK || R.replyLen < JKR_HDR_SIZE + 4) return 0;
    if (pIp) memcpy(pIp, R.reply + JKR_HDR_SIZE, 4);
    return 1;
}

// Where player id plays from ("a.b.c.d:port"), 0 if unknown.
int jkrelay_HostPlayerAddr(uint32_t id, char* out, size_t cap)
{
    uint8_t body[4];
    struct in_addr a;
    uint16_t port;
    if (!R.inSession || !R.isHost || !cap) return 0;
    wr32(body, id);
    if (jkr_Request(JKR_PLAYERADDR, body, sizeof(body), 1500) != JKR_OK || R.replyLen < JKR_HDR_SIZE + 6) return 0;
    memcpy(&a.s_addr, R.reply + JKR_HDR_SIZE, 4);
    memcpy(&port, R.reply + JKR_HDR_SIZE + 4, 2);
    snprintf(out, cap, "%s:%u", inet_ntoa(a), (unsigned)ntohs(port));
    return 1;
}

static void jkr_SendBansNow(void)
{
    uint8_t pkt[JKR_HDR_SIZE + 2 + sizeof(R.hostBans)];
    if (!R.inSession || !R.isHost || R.resuming) return;
    jkr_PutHdr(pkt, JKR_SETBANS, ++R.nextSeq);
    wr16(pkt + JKR_HDR_SIZE, (uint16_t)R.numHostBans);
    memcpy(pkt + JKR_HDR_SIZE + 2, R.hostBans, 6 * (size_t)R.numHostBans);
    jkr_SendRaw(pkt, JKR_HDR_SIZE + 2 + 6 * (size_t)R.numHostBans);
}

// Our game's bans (u32 IPv4 in network order, u16 minutes left, 0 for good, each),
// which the relay server refuses joins for. Sent now, and again after a resume.
void jkrelay_HostSetBans(const uint8_t* list, int n)
{
    if (n > JKR_MAX_HOST_BANS) n = JKR_MAX_HOST_BANS;
    if (n < 0) n = 0;
    memcpy(R.hostBans, list, 6 * (size_t)n);
    R.numHostBans = n;
    jkr_SendBansNow();
}

// Hit what you see (2026-10-29): a number for the next Send, which goes in its
// header's seq (0 in game data before; servers and older clients pass it on
// and ignore it). The game numbers its own position updates to each player,
// so a hit claim can name the one it had on screen (stdComm_WSRelayHits.c).
void jkrelay_SetSendSeq(uint32_t seq)
{
    R.sendSeq = seq;
}

// The number the data Receive returned last came with (1), if it had one.
int jkrelay_LastRecvSeq(uint32_t* pSeq)
{
    uint32_t seq = (R.evLast && R.evLast->type == DPS_RECV_DATA) ? R.evLast->seq : 0;
    if (pSeq) *pSeq = seq;
    return seq != 0;
}

const dps_backend jkrelay_backend = DPS_BACKEND_TABLE(jkrelay);

#endif // PLATFORM_DPLAY || JKRELAY_STANDALONE_CLIENT
