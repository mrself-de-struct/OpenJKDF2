// WebRTC data channels for the web lobby transport, the same way the jkdf2mp
// browser build connects (src/Platform/Networking/WSRelay in its fork):
//
//  - a star: each joiner opens one RTCPeerConnection to the host (id 1); the
//    host forwards packets meant for another joiner
//  - two pre-negotiated, reliable and ordered channels: "jk" (id 0) for game
//    packets, "jk-map" (id 1) for map and mod downloads (jkweb_xfer.c), so a
//    download never holds up game packets
//  - the joiner offers; signaling goes through the lobby as
//    "signal <id> <base64 JSON>", JSON = {"d": description} or {"c": candidate}
//  - ICE servers come from the lobby's "ice <base64 JSON>" line
//
// Until a channel is open, or if it never opens, jkweb_client.c sends through
// the lobby, so nothing depends on this working. libdatachannel calls back on
// its own threads; everything crossing into the game goes through
// mutex-protected queues that jkweb_client.c drains from the game thread.
//
// Built when JKWEB_WEBRTC is defined (CMake: TARGET_USE_WEBRTC); otherwise the
// functions below are stubs and the lobby carries everything.

#if defined(PLATFORM_DPLAY) && defined(PLATFORM_CURL)

#include "jkweb_rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef JKWEB_WEBRTC

#include <pthread.h>
#include <rtc/rtc.h>

#define JKRTC_MAX_PEERS   65          // ids 1..64
#define JKRTC_MAX_ICE     8
#define JKRTC_QUEUE_MAX   4096        // queued items before we start dropping

typedef struct jkrtc_item
{
    struct jkrtc_item* next;
    int  id;
    int  size;
    char data[];
} jkrtc_item;

typedef struct
{
    int pc, dc, mc;      // libdatachannel ids, 0 = none
    volatile int open;
    volatile int mapOpen;  // the "jk-map" channel
    int localSet;        // host: answer created
} jkrtc_peer;

static pthread_mutex_t jkrtc_lock = PTHREAD_MUTEX_INITIALIZER;
static jkrtc_peer jkrtc_peers[JKRTC_MAX_PEERS];
static int jkrtc_isHost = 0;
static int jkrtc_active = 0;
static int jkrtc_log = 0;
static int jkrtc_inited = 0;
static char* jkrtc_ice[JKRTC_MAX_ICE];
static int jkrtc_numIce = 0;

static jkrtc_item *jkrtc_sigHead, *jkrtc_sigTail;   // outgoing signaling
static jkrtc_item *jkrtc_pktHead, *jkrtc_pktTail;   // incoming packets
static jkrtc_item *jkrtc_mapHead, *jkrtc_mapTail;   // incoming "jk-map" frames
static int jkrtc_queued = 0;

static void jkrtc_Enqueue(jkrtc_item** head, jkrtc_item** tail, int id, const void* data, int size, int addNul)
{
    jkrtc_item* it;
    if (size < 0) return;
    it = (jkrtc_item*)malloc(sizeof(jkrtc_item) + (size_t)size + (addNul ? 1 : 0));
    if (!it) return;
    it->next = NULL;
    it->id = id;
    it->size = size;
    memcpy(it->data, data, (size_t)size);
    if (addNul) it->data[size] = 0;
    pthread_mutex_lock(&jkrtc_lock);
    if (jkrtc_queued >= JKRTC_QUEUE_MAX) { pthread_mutex_unlock(&jkrtc_lock); free(it); return; }
    if (*tail) (*tail)->next = it; else *head = it;
    *tail = it;
    jkrtc_queued++;
    pthread_mutex_unlock(&jkrtc_lock);
}

static jkrtc_item* jkrtc_Dequeue(jkrtc_item** head, jkrtc_item** tail)
{
    jkrtc_item* it;
    pthread_mutex_lock(&jkrtc_lock);
    it = *head;
    if (it)
    {
        *head = it->next;
        if (!*head) *tail = NULL;
        jkrtc_queued--;
    }
    pthread_mutex_unlock(&jkrtc_lock);
    return it;
}

// ---------------------------------------------------------------------------
// Tiny JSON helpers (only what the signaling messages need)
// ---------------------------------------------------------------------------

// Appends s as a JSON string literal.
static size_t jkrtc_JsonStr(char* out, size_t cap, size_t pos, const char* s)
{
    if (pos < cap) out[pos++] = '"';
    for (; *s && pos + 7 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[pos++] = '\\'; out[pos++] = (char)c; }
        else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (c == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (c == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else if (c < 0x20) pos += (size_t)snprintf(out + pos, cap - pos, "\\u%04x", c);
        else out[pos++] = (char)c;
    }
    if (pos < cap) out[pos++] = '"';
    if (pos < cap) out[pos] = 0;
    return pos;
}

// Finds "key": "<value>" after p and unescapes the value into out. Returns
// pointer past the value, or NULL.
static const char* jkrtc_JsonGet(const char* p, const char* key, char* out, size_t cap)
{
    char pat[64];
    const char* s;
    size_t n = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    s = strstr(p, pat);
    if (!s) return NULL;
    s += strlen(pat);
    while (*s == ' ' || *s == ':' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != '"') return NULL;
    s++;
    while (*s && *s != '"')
    {
        char c = *s++;
        if (c == '\\' && *s)
        {
            char e = *s++;
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == 'u' && s[0] && s[1] && s[2] && s[3])
            {
                char hex[5] = { s[0], s[1], s[2], s[3], 0 };
                unsigned v = (unsigned)strtoul(hex, NULL, 16);
                s += 4;
                c = (char)(v < 0x80 ? v : '?');
            }
            else c = e;
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = 0;
    return *s == '"' ? s + 1 : NULL;
}

static void jkrtc_UrlEncode(const char* in, char* out, size_t cap)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < cap; in++)
    {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hx[c >> 4]; out[n++] = hx[c & 15]; }
    }
    out[n] = 0;
}

// ---------------------------------------------------------------------------
// libdatachannel callbacks (other threads)
// ---------------------------------------------------------------------------

static int jkrtc_PeerOf(void* ptr) { return (int)(intptr_t)ptr; }

static void RTC_API jkrtc_OnLocalDescription(int pc, const char* sdp, const char* type, void* ptr)
{
    char json[16384];
    size_t pos;
    (void)pc;
    pos = (size_t)snprintf(json, sizeof(json), "{\"d\":{\"type\":");
    pos = jkrtc_JsonStr(json, sizeof(json), pos, type);
    pos += (size_t)snprintf(json + pos, sizeof(json) - pos, ",\"sdp\":");
    pos = jkrtc_JsonStr(json, sizeof(json), pos, sdp);
    snprintf(json + pos, sizeof(json) - pos, "}}");
    jkrtc_Enqueue(&jkrtc_sigHead, &jkrtc_sigTail, jkrtc_PeerOf(ptr), json, (int)strlen(json), 1);
}

static void RTC_API jkrtc_OnLocalCandidate(int pc, const char* cand, const char* mid, void* ptr)
{
    char json[2048];
    size_t pos;
    (void)pc;
    if (!strncmp(cand, "a=", 2)) cand += 2;   // browsers want "candidate:..."
    pos = (size_t)snprintf(json, sizeof(json), "{\"c\":{\"candidate\":");
    pos = jkrtc_JsonStr(json, sizeof(json), pos, cand);
    pos += (size_t)snprintf(json + pos, sizeof(json) - pos, ",\"sdpMid\":");
    pos = jkrtc_JsonStr(json, sizeof(json), pos, mid ? mid : "0");
    snprintf(json + pos, sizeof(json) - pos, ",\"sdpMLineIndex\":0}}");
    jkrtc_Enqueue(&jkrtc_sigHead, &jkrtc_sigTail, jkrtc_PeerOf(ptr), json, (int)strlen(json), 1);
}

static void RTC_API jkrtc_OnOpen(int dc, void* ptr)
{
    int id = jkrtc_PeerOf(ptr);
    (void)dc;
    if (id > 0 && id < JKRTC_MAX_PEERS) jkrtc_peers[id].open = 1;
    if (jkrtc_log) fprintf(stderr, "[jkweb] direct WebRTC link to player %d is up\n", id);
}

static void RTC_API jkrtc_OnClosed(int dc, void* ptr)
{
    int id = jkrtc_PeerOf(ptr);
    (void)dc;
    if (id > 0 && id < JKRTC_MAX_PEERS && jkrtc_peers[id].open)
    {
        jkrtc_peers[id].open = 0;
        if (jkrtc_log) fprintf(stderr, "[jkweb] direct link to player %d is down, using the lobby\n", id);
    }
}

static void RTC_API jkrtc_OnMessage(int dc, const char* msg, int size, void* ptr)
{
    (void)dc;
    if (size < 0) return;   // text messages: not part of the protocol
    jkrtc_Enqueue(&jkrtc_pktHead, &jkrtc_pktTail, jkrtc_PeerOf(ptr), msg, size, 0);
}

static void RTC_API jkrtc_OnMapOpen(int dc, void* ptr)
{
    int id = jkrtc_PeerOf(ptr);
    (void)dc;
    if (id > 0 && id < JKRTC_MAX_PEERS) jkrtc_peers[id].mapOpen = 1;
}

static void RTC_API jkrtc_OnMapClosed(int dc, void* ptr)
{
    int id = jkrtc_PeerOf(ptr);
    (void)dc;
    if (id > 0 && id < JKRTC_MAX_PEERS) jkrtc_peers[id].mapOpen = 0;
}

static void RTC_API jkrtc_OnMapMessage(int dc, const char* msg, int size, void* ptr)
{
    (void)dc;
    if (size < 0) return;
    jkrtc_Enqueue(&jkrtc_mapHead, &jkrtc_mapTail, jkrtc_PeerOf(ptr), msg, size, 0);
}

static void RTC_API jkrtc_OnState(int pc, rtcState state, void* ptr)
{
    static const char* names[] = { "new", "connecting", "connected", "disconnected", "failed", "closed" };
    int id = jkrtc_PeerOf(ptr);
    (void)pc;
    if (jkrtc_log && (int)state >= 0 && (int)state < 6)
        fprintf(stderr, "[jkweb] WebRTC to player %d: %s\n", id, names[state]);
    if ((state == RTC_FAILED || state == RTC_DISCONNECTED || state == RTC_CLOSED) && id > 0 && id < JKRTC_MAX_PEERS)
        jkrtc_peers[id].open = jkrtc_peers[id].mapOpen = 0;
}

// ---------------------------------------------------------------------------

static void jkrtc_Close(int id)
{
    jkrtc_peer* p;
    if (id <= 0 || id >= JKRTC_MAX_PEERS) return;
    p = &jkrtc_peers[id];
    p->open = p->mapOpen = 0;
    if (p->dc) { rtcDeleteDataChannel(p->dc); p->dc = 0; }
    if (p->mc) { rtcDeleteDataChannel(p->mc); p->mc = 0; }
    if (p->pc) { rtcDeletePeerConnection(p->pc); p->pc = 0; }
    p->localSet = 0;
}

static int jkrtc_CreatePeer(int id)
{
    rtcConfiguration cfg;
    jkrtc_peer* p = &jkrtc_peers[id];
    jkrtc_Close(id);
    memset(&cfg, 0, sizeof(cfg));
    cfg.iceServers = (const char**)jkrtc_ice;
    cfg.iceServersCount = jkrtc_numIce;
    cfg.disableAutoNegotiation = true;
    p->pc = rtcCreatePeerConnection(&cfg);
    if (p->pc < 0) { p->pc = 0; return 0; }
    rtcSetUserPointer(p->pc, (void*)(intptr_t)id);
    rtcSetLocalDescriptionCallback(p->pc, jkrtc_OnLocalDescription);
    rtcSetLocalCandidateCallback(p->pc, jkrtc_OnLocalCandidate);
    rtcSetStateChangeCallback(p->pc, jkrtc_OnState);
    return 1;
}

// Pre-negotiated channels, same ids/labels/reliability as the browser build.
static void jkrtc_CreateChannels(int id)
{
    rtcDataChannelInit init;
    jkrtc_peer* p = &jkrtc_peers[id];
    memset(&init, 0, sizeof(init));
    init.negotiated = true;
    init.manualStream = true;
    init.stream = 0;
    p->dc = rtcCreateDataChannelEx(p->pc, "jk", &init);
    if (p->dc < 0) p->dc = 0;
    init.stream = 1;
    p->mc = rtcCreateDataChannelEx(p->pc, "jk-map", &init);
    if (p->mc < 0) p->mc = 0;
    if (p->dc)
    {
        rtcSetUserPointer(p->dc, (void*)(intptr_t)id);
        rtcSetOpenCallback(p->dc, jkrtc_OnOpen);
        rtcSetClosedCallback(p->dc, jkrtc_OnClosed);
        rtcSetMessageCallback(p->dc, jkrtc_OnMessage);
    }
    if (p->mc)
    {
        rtcSetUserPointer(p->mc, (void*)(intptr_t)id);
        rtcSetOpenCallback(p->mc, jkrtc_OnMapOpen);
        rtcSetClosedCallback(p->mc, jkrtc_OnMapClosed);
        rtcSetMessageCallback(p->mc, jkrtc_OnMapMessage);
    }
}

// ---------------------------------------------------------------------------
// Interface for jkweb_client.c (game thread)
// ---------------------------------------------------------------------------

int jkrtc_Available(void) { return 1; }

void jkrtc_SetIceServersJson(const char* json)
{
    const char* p = json;
    for (int i = 0; i < jkrtc_numIce; i++) free(jkrtc_ice[i]);
    jkrtc_numIce = 0;
    // [{"urls": "stun:..." | ["...", ...], "username": "...", "credential": "..."}, ...]
    while (p && (p = strchr(p, '{')) && jkrtc_numIce < JKRTC_MAX_ICE)
    {
        const char* end = strchr(p, '}');
        char obj[2048], user[256] = "", cred[256] = "", ue[600], ce[600];
        size_t len;
        if (!end) break;
        len = (size_t)(end - p) + 1;
        if (len >= sizeof(obj)) { p = end; continue; }
        memcpy(obj, p, len);
        obj[len] = 0;
        jkrtc_JsonGet(obj, "username", user, sizeof(user));
        jkrtc_JsonGet(obj, "credential", cred, sizeof(cred));
        jkrtc_UrlEncode(user, ue, sizeof(ue));
        jkrtc_UrlEncode(cred, ce, sizeof(ce));

        // urls may be one string or an array of strings
        const char* u = strstr(obj, "\"urls\"");
        if (u)
        {
            u = strchr(u + 6, ':');
            while (u && *u && jkrtc_numIce < JKRTC_MAX_ICE)
            {
                char url[512], full[1400];
                const char* q = strchr(u, '"');
                const char* qe;
                if (!q) break;
                qe = strchr(q + 1, '"');
                if (!qe || (size_t)(qe - q - 1) >= sizeof(url)) break;
                memcpy(url, q + 1, (size_t)(qe - q - 1));
                url[qe - q - 1] = 0;
                if ((!strncmp(url, "turn:", 5) || !strncmp(url, "turns:", 6)) && user[0])
                {
                    const char* rest = strchr(url, ':') + 1;
                    snprintf(full, sizeof(full), "%.*s%s:%s@%s", (int)(rest - url), url, ue, ce, rest);
                }
                else
                    snprintf(full, sizeof(full), "%s", url);
                jkrtc_ice[jkrtc_numIce++] = strdup(full);
                u = qe + 1;
                while (*u == ' ' || *u == ',') u++;
                if (*u != '"') break;   // end of the array / single string
            }
        }
        p = end + 1;
    }
    if (jkrtc_log)
        for (int i = 0; i < jkrtc_numIce; i++)
            fprintf(stderr, "[jkweb] ICE server: %.40s%s\n", jkrtc_ice[i], strlen(jkrtc_ice[i]) > 40 ? "..." : "");
}

void jkrtc_Begin(int isHost, int log)
{
    const char* env = getenv("OPENJKDF2_ICE");
    jkrtc_log = log;
    if (!jkrtc_inited)
    {
        rtcInitLogger(RTC_LOG_ERROR, NULL);
        jkrtc_inited = 1;
    }
    // No ICE servers from the lobby: a public STUN server lets us find our
    // public address (OPENJKDF2_ICE="stun:a,stun:b" overrides, "none" disables).
    if (!jkrtc_numIce)
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", (env && *env) ? env : "stun:stun.l.google.com:19302");
        if (strcmp(buf, "none"))
            for (char* t = strtok(buf, ","); t && jkrtc_numIce < JKRTC_MAX_ICE; t = strtok(NULL, ","))
                jkrtc_ice[jkrtc_numIce++] = strdup(t);
    }
    jkrtc_End();
    jkrtc_isHost = isHost;
    jkrtc_active = 1;
    if (!isHost)
    {
        // The joiner offers to the host.
        if (jkrtc_CreatePeer(1))
        {
            jkrtc_CreateChannels(1);
            rtcSetLocalDescription(jkrtc_peers[1].pc, "offer");
        }
    }
}

void jkrtc_End(void)
{
    jkrtc_item* it;
    for (int i = 1; i < JKRTC_MAX_PEERS; i++) jkrtc_Close(i);
    while ((it = jkrtc_Dequeue(&jkrtc_sigHead, &jkrtc_sigTail))) free(it);
    while ((it = jkrtc_Dequeue(&jkrtc_pktHead, &jkrtc_pktTail))) free(it);
    while ((it = jkrtc_Dequeue(&jkrtc_mapHead, &jkrtc_mapTail))) free(it);
    jkrtc_active = 0;
}

void jkrtc_OnSignal(int from, const char* json)
{
    static char sdp[16384];
    char type[32], cand[1024], mid[64];
    if (!jkrtc_active || from <= 0 || from >= JKRTC_MAX_PEERS) return;

    if (strstr(json, "\"sdp\"") && jkrtc_JsonGet(json, "sdp", sdp, sizeof(sdp)) && jkrtc_JsonGet(json, "type", type, sizeof(type)))
    {
        if (jkrtc_log) fprintf(stderr, "[jkweb] WebRTC %s from player %d\n", type, from);
        if (!strcmp(type, "offer"))
        {
            // Only the host takes offers (from its joiners).
            if (!jkrtc_isHost) return;
            if (!jkrtc_CreatePeer(from)) return;
            rtcSetRemoteDescription(jkrtc_peers[from].pc, sdp, "offer");
            rtcSetLocalDescription(jkrtc_peers[from].pc, "answer");
            jkrtc_CreateChannels(from);
        }
        else if (!strcmp(type, "answer") && jkrtc_peers[from].pc)
            rtcSetRemoteDescription(jkrtc_peers[from].pc, sdp, "answer");
    }
    else if (jkrtc_JsonGet(json, "candidate", cand, sizeof(cand)) && cand[0] && jkrtc_peers[from].pc)
    {
        if (!jkrtc_JsonGet(json, "sdpMid", mid, sizeof(mid))) strcpy(mid, "0");
        if (jkrtc_log)
        {
            // "candidate:<f> <c> <proto> <prio> <addr> <port> typ <type>": log addr and type
            char a[64] = "", t[16] = "";
            sscanf(cand, "%*s %*s %*s %*s %63s %*s typ %15s", a, t);
            fprintf(stderr, "[jkweb] WebRTC candidate from player %d: %s (%s)%s\n", from, a, t,
                    strstr(a, ".local") ? " -- mDNS name; resolvable only on the same LAN" : "");
        }
        rtcAddRemoteCandidate(jkrtc_peers[from].pc, cand, mid);
    }
}

void jkrtc_ClosePeer(int id) { jkrtc_Close(id); }

int jkrtc_IsOpen(int id)
{
    return id > 0 && id < JKRTC_MAX_PEERS && jkrtc_peers[id].open && jkrtc_peers[id].dc;
}

int jkrtc_Send(int id, const void* data, int size)
{
    if (!jkrtc_IsOpen(id)) return 0;
    return rtcSendMessage(jkrtc_peers[id].dc, (const char*)data, size) >= 0;
}

int jkrtc_MapOpen(int id)
{
    return jkrtc_IsOpen(id) && jkrtc_peers[id].mapOpen && jkrtc_peers[id].mc;
}

int jkrtc_SendMap(int id, const void* data, int size)
{
    if (!jkrtc_MapOpen(id)) return 0;
    return rtcSendMessage(jkrtc_peers[id].mc, (const char*)data, size) >= 0;
}

int jkrtc_MapBuffered(int id)
{
    int n;
    if (!jkrtc_MapOpen(id)) return -1;
    n = rtcGetBufferedAmount(jkrtc_peers[id].mc);
    return n < 0 ? -1 : n;
}

int jkrtc_PollMapFrame(int* fromId, uint8_t* out, int cap)
{
    jkrtc_item* it = jkrtc_Dequeue(&jkrtc_mapHead, &jkrtc_mapTail);
    int n;
    if (!it) return -1;
    *fromId = it->id;
    n = it->size;
    if (n > cap) n = -2;      // too big for the caller: dropped
    else memcpy(out, it->data, (size_t)n);
    free(it);
    return n;
}

int jkrtc_PollSignal(int* toId, char* out, int cap)
{
    jkrtc_item* it = jkrtc_Dequeue(&jkrtc_sigHead, &jkrtc_sigTail);
    int n;
    if (!it) return -1;
    *toId = it->id;
    n = it->size < cap - 1 ? it->size : cap - 1;
    memcpy(out, it->data, (size_t)n);
    out[n] = 0;
    free(it);
    return n;
}

int jkrtc_PollPacket(int* fromId, uint8_t* out, int cap)
{
    jkrtc_item* it = jkrtc_Dequeue(&jkrtc_pktHead, &jkrtc_pktTail);
    int n;
    if (!it) return -1;
    *fromId = it->id;
    n = it->size < cap ? it->size : cap;
    memcpy(out, it->data, (size_t)n);
    free(it);
    return n;
}

#else // !JKWEB_WEBRTC: everything goes through the lobby

int jkrtc_Available(void) { return 0; }
void jkrtc_SetIceServersJson(const char* json) { (void)json; }
void jkrtc_Begin(int isHost, int log) { (void)isHost; (void)log; }
void jkrtc_End(void) { }
void jkrtc_OnSignal(int from, const char* json) { (void)from; (void)json; }
void jkrtc_ClosePeer(int id) { (void)id; }
int  jkrtc_IsOpen(int id) { (void)id; return 0; }
int  jkrtc_Send(int id, const void* data, int size) { (void)id; (void)data; (void)size; return 0; }
int  jkrtc_PollSignal(int* toId, char* out, int cap) { (void)toId; (void)out; (void)cap; return -1; }
int  jkrtc_PollPacket(int* fromId, uint8_t* out, int cap) { (void)fromId; (void)out; (void)cap; return -1; }
int  jkrtc_MapOpen(int id) { (void)id; return 0; }
int  jkrtc_MapBuffered(int id) { (void)id; return -1; }
int  jkrtc_SendMap(int id, const void* data, int size) { (void)id; (void)data; (void)size; return 0; }
int  jkrtc_PollMapFrame(int* fromId, uint8_t* out, int cap) { (void)fromId; (void)out; (void)cap; return -1; }

#endif // JKWEB_WEBRTC

#endif // PLATFORM_DPLAY && PLATFORM_CURL
