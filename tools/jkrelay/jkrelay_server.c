// jkrelay_server: relay server for OpenJKDF2's "Imperial Network Protocol" multiplayer.
//
// Players keep one outbound UDP flow to this server; it lists games, admits
// players, hands out player IDs and forwards game traffic within a session.
// Hosts and joiners that ask for it are introduced for a direct link, which
// then carries their game traffic instead (jkrelay_proto.h, "Direct links").
// Only this machine needs a reachable port (UDP 27900 by default).
//
// It never looks inside game messages, so any OpenJKDF2 game mode works.
//
//   cc -O2 -o jkrelay_server tools/jkrelay/jkrelay_server.c
//      -Isrc/Platform/Networking/DPlay
//   ./jkrelay_server [-p port] [-v] [-l logfile]
//
// Logs go to stdout, or with -l to that file (appended, one flushed line per
// event). SIGHUP reopens the file, for logrotate.
//
// The game also builds this file into itself (JKRELAY_EMBEDDED, see
// src/Platform/Networking/DPlay/jkrelay_listen.c): a dedicated server runs its
// own copy on its own port, so players connect to it directly. That copy only
// lets its own machine host, and introduces nobody for direct links (the
// server is reachable already).
//
// It's also a master server: dedicated servers running their own copy list
// themselves here (JKR_ANNOUNCE), and appear in its game list for new clients,
// which join them at their own address (jkrelay_proto.h, "Master server").
//
// Protocol: src/Platform/Networking/DPlay/jkrelay_proto.h

#ifndef JKRELAY_EMBEDDED
#define _GNU_SOURCE
#endif
#ifdef _WIN32
#define _CRT_RAND_S
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../src/Platform/Networking/DPlay/jkrelay_proto.h"
#ifdef JKRELAY_EMBEDDED
#include "../../src/Platform/Networking/DPlay/jkrelay_hmac.h"
#endif

#ifdef _WIN32
typedef SOCKET jkrs_sock;
typedef int jkrs_socklen;
#define JKRS_BADSOCK INVALID_SOCKET
#define jkrs_poll WSAPoll
#define jkrs_closesock closesocket
#else
typedef int jkrs_sock;
typedef socklen_t jkrs_socklen;
#define JKRS_BADSOCK (-1)
#define jkrs_poll poll
#define jkrs_closesock close
#endif

// A packet to a client's address: over UDP, or its WebSocket connection (see "WebSocket").
static void net_send(const struct sockaddr_in* to, const void* p, size_t len);

#define MAX_CLIENTS       256
#define MAX_SESSIONS      64
#define MAX_PLAYERS       64     // per session (system + game players)
#define MAX_EVENTS        256    // unacknowledged events per client
#define EVENT_WINDOW      32     // events resent per tick

typedef struct
{
    uint8_t  data[JKR_MAX_PACKET];
    uint16_t len;
    uint32_t seq;
} event_t;

struct session;

typedef struct client
{
    int      used;
    struct sockaddr_in addr;
    uint32_t token;
    uint64_t lastRx, lastTx;
    struct session* sess;
    uint32_t sysId;
    int      wantDirect;              // asked for a direct link (JKR_JF_DIRECT)
    uint32_t lanIp;                   // its LAN address, host order (for players behind one NAT)
    uint16_t lanPort;

    uint32_t lastReplySeq;            // duplicate requests get the cached reply
    uint8_t  lastReply[JKR_MAX_PACKET];
    uint16_t lastReplyLen;

    event_t  ev[MAX_EVENTS];          // ring of unacknowledged events
    uint32_t evNext;                  // next event number to assign (starts at 1)
    uint32_t evAcked;                 // highest acknowledged
    uint64_t lastResend;
} client_t;

typedef struct
{
    int       used;
    uint32_t  id, sysId, flags;
    client_t* owner;
    uint16_t  name[32];
    uint8_t   nameLen;
} player_t;

typedef struct session
{
    int       used;
    uint8_t   appGuid[16];
    uint8_t   guid[16];
    uint32_t  flags, maxPlayers, user[4];
    uint16_t  name[JKR_MAX_NAME];
    uint8_t   nameLen;
    uint16_t  password[JKR_MAX_PASSWORD];
    uint8_t   passwordLen;
    uint8_t   tail[JKR_MAX_TAIL];         // the host's mod (jkrelay_proto.h, "Mods"), tailLen 0: unknown
    uint8_t   tailLen;
    struct { uint32_t ip; uint64_t until; } bans[JKR_MAX_HOST_BANS];   // the host's (JKR_SETBANS); until 0 = for good
    client_t* host;
    uint64_t  rehostedMs;             // put back by JKR_REHOST: REJOINs taken until + JKR_REJOIN_WINDOW_MS
    uint32_t  nextId;
    player_t  players[MAX_PLAYERS];
} session_t;

static jkrs_sock g_sock = JKRS_BADSOCK;
static jkrs_sock g_wsListen = JKRS_BADSOCK;   // WebSocket connections (-w)
// Master server: reachability HELLOs go out from this one, not g_sock. A
// listed server's announcements to g_sock open its NAT/firewall to g_sock's
// address and port only, so an answer here means players get through too.
static jkrs_sock g_probeSock = JKRS_BADSOCK;
static int g_embedded;                // in a dedicated server (JKRELAY_EMBEDDED): only it hosts
static int g_verbose;
static const char* g_logPath;
static FILE* g_logFile;
static volatile sig_atomic_t g_reopenLog;
static client_t  g_clients[MAX_CLIENTS];
static session_t g_sessions[MAX_SESSIONS];

// Master server: dedicated servers listed here (JKR_ANNOUNCE).
#define MAX_LISTED        128
#define MAX_LISTED_PER_IP 4
typedef struct
{
    int       used, reachable;
    struct sockaddr_in addr;          // where players reach it: the announcement's IP, the announced port
    uint8_t   appGuid[16];
    session_t game;                   // desc fields only (guid, flags, maxPlayers, user, name)
    uint32_t  curPlayers;
    uint64_t  lastAnnounce, lastProbe;
    uint32_t  probeSeq;
} listed_t;
static listed_t g_listed[MAX_LISTED];

// A dedicated server's own copy: lists it on a master server (server_set_master).
static struct
{
    int       set, resolved;
    char      name[256];
    int       port;                   // ours, announced
    struct sockaddr_in addr;
    uint64_t  lastResolve, lastTx, lastAnnounce;
    uint32_t  token, helloSeq, annSeq, lastHash;
    int       listed, unlistedOks, bWarned, bRefused;
} g_master;
#ifndef JKRELAY_EMBEDDED
static volatile sig_atomic_t g_stop;
#endif

// ---------------------------------------------------------------------------

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void logf_(int verboseOnly, const char* fmt, ...)
{
    va_list ap;
    FILE* out = g_logFile ? g_logFile : stdout;
    if (verboseOnly && !g_verbose) return;
    if (g_embedded)
        fprintf(out, "[server] ");   // the game's log adds the time
    else
    {
        time_t t = time(NULL);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(out, "%s ", ts);
    }
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static uint32_t rnd32(void)
{
    uint32_t v = 0;
#ifdef _WIN32
    unsigned int r;
    if (rand_s(&r) == 0) return r;
    v = (uint32_t)now_ms() * 2654435761u ^ (uint32_t)_getpid();
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, 1, 4, f) != 4) v = (uint32_t)now_ms() * 2654435761u ^ (uint32_t)getpid();
    if (f) fclose(f);
#endif
    return v;
}

static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const char* addr_str(const struct sockaddr_in* a)
{
    static char buf[4][32];
    static int i;
    i = (i + 1) & 3;
    snprintf(buf[i], sizeof(buf[i]), "%s:%u", inet_ntoa(a->sin_addr), ntohs(a->sin_port));
    return buf[i];
}

static void name_ascii(const uint16_t* s, int n, char* out, size_t outLen)
{
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < outLen; i++) out[k++] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : '?';
    out[k] = 0;
}

// Reads u8 len + UTF-16 into out[max]. Returns bytes consumed, 0 on error.
// An optional mod tail (jkrelay_proto.h, "Mods") at p: kept in the session if it's there.
static void get_tail(const uint8_t* p, const uint8_t* end, session_t* s)
{
    if (p >= end || p + 1 + p[0] > end) return;
    s->tailLen = p[0];
    memcpy(s->tail, p + 1, s->tailLen);
}

static size_t put_tail(uint8_t* p, const session_t* s)
{
    p[0] = s->tailLen;
    memcpy(p + 1, s->tail, s->tailLen);
    return 1 + (size_t)s->tailLen;
}

static size_t get_str(const uint8_t* p, const uint8_t* end, uint16_t* out, uint8_t* lenOut, int max)
{
    int n;
    if (p >= end) return 0;
    n = p[0];
    if (p + 1 + 2 * n > end) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = rd16(p + 1 + 2 * i);
    *lenOut = (uint8_t)n;
    return 1 + 2 * (size_t)p[0];
}

static size_t put_str(uint8_t* p, const uint16_t* s, int n)
{
    p[0] = (uint8_t)n;
    for (int i = 0; i < n; i++) wr16(p + 1 + 2 * i, s[i]);
    return 1 + 2 * (size_t)n;
}

static void put_hdr(uint8_t* p, uint8_t type, uint32_t token, uint32_t seq)
{
    memset(p, 0, JKR_HDR_SIZE);
    wr32(p, JKR_MAGIC);
    p[4] = type;
    p[5] = JKR_VERSION;
    wr32(p + 8, token);
    wr32(p + 12, seq);
}

static void send_to(client_t* c, const uint8_t* p, size_t len)
{
    net_send(&c->addr, p, len);
    c->lastTx = now_ms();
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

static int count_players(session_t* s)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (s->players[i].used && !(s->players[i].flags & JKR_PF_SYSTEM)) n++;
    return n;
}

static size_t put_desc_n(uint8_t* p, session_t* s, uint32_t curPlayers)
{
    memcpy(p, s->guid, 16);
    wr32(p + 16, s->flags);
    wr32(p + 20, s->maxPlayers);
    wr32(p + 24, curPlayers);
    for (int i = 0; i < 4; i++) wr32(p + 28 + 4 * i, s->user[i]);
    return 44 + put_str(p + 44, s->name, s->nameLen);
}

static size_t put_desc(uint8_t* p, session_t* s)
{
    return put_desc_n(p, s, (uint32_t)count_players(s));
}

// Parses a desc into the session (guid and player count ignored). 0 on error.
static size_t get_desc(const uint8_t* p, const uint8_t* end, session_t* s)
{
    size_t n;
    if (end - p < JKR_DESC_FIXED) return 0;
    s->flags = rd32(p + 16);
    s->maxPlayers = rd32(p + 20);
    if (s->maxPlayers > 32) s->maxPlayers = 32;
    for (int i = 0; i < 4; i++) s->user[i] = rd32(p + 28 + 4 * i);
    n = get_str(p + 44, end, s->name, &s->nameLen, JKR_MAX_NAME);
    return n ? 44 + n : 0;
}

// 'id' 0 = hand out the next one; otherwise a resumed player's own (JKR_REHOST / JKR_REJOIN).
static player_t* add_player_id(session_t* s, client_t* owner, uint32_t flags, const uint16_t* name, uint8_t nameLen, uint32_t id)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        player_t* p = &s->players[i];
        if (p->used) continue;
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->id = id;
        if (!p->id) p->id = s->nextId++;
        if (!p->id) p->id = s->nextId++;
        p->flags = flags;
        p->owner = owner;
        p->sysId = (flags & JKR_PF_SYSTEM) ? p->id : owner->sysId;
        if (nameLen > 31) nameLen = 31;
        if (name) memcpy(p->name, name, 2u * nameLen);
        p->nameLen = nameLen;
        return p;
    }
    return NULL;
}

static player_t* add_player(session_t* s, client_t* owner, uint32_t flags, const uint16_t* name, uint8_t nameLen)
{
    return add_player_id(s, owner, flags, name, nameLen, 0);
}

static player_t* find_player(session_t* s, uint32_t id)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (s->players[i].used && s->players[i].id == id) return &s->players[i];
    return NULL;
}

static session_t* find_session(const uint8_t guid[16])
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].used && !memcmp(g_sessions[i].guid, guid, 16)) return &g_sessions[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// Reliable events
// ---------------------------------------------------------------------------

static void drop_client(client_t* c, const char* why);

static void queue_event(client_t* c, const uint8_t* body, size_t len)
{
    event_t* e;
    if (c->evNext - c->evAcked > MAX_EVENTS - 1)
    {
        drop_client(c, "too many unacknowledged events");
        return;
    }
    e = &c->ev[c->evNext % MAX_EVENTS];
    e->seq = c->evNext++;
    put_hdr(e->data, JKR_EVENT, c->token, e->seq);
    memcpy(e->data + JKR_HDR_SIZE, body, len);
    e->len = (uint16_t)(JKR_HDR_SIZE + len);
    send_to(c, e->data, e->len);
}

static void event_player_created(client_t* to, player_t* p, int initial)
{
    uint8_t b[16 + 64];
    size_t n;
    b[0] = JKR_EV_PLAYER_CREATED;
    b[1] = (uint8_t)initial;
    wr32(b + 2, p->id);
    wr32(b + 6, p->sysId);
    wr32(b + 10, p->flags);
    n = 14 + put_str(b + 14, p->name, p->nameLen);
    queue_event(to, b, n);
}

// Direct links: tells 'to' where 'peer' can be reached.
static int is_local_ip(uint32_t ip)   // host order
{
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
}

static void event_peer(client_t* to, client_t* peer, uint32_t key)
{
    uint8_t b[JKR_EV_PEER_SIZE];
    uint32_t ip = ntohl(peer->addr.sin_addr.s_addr);
    b[0] = JKR_EV_PEER;
    wr32(b + 1, peer->sysId);
    wr32(b + 5, key);
    wr32(b + 9, ip);
    wr16(b + 13, ntohs(peer->addr.sin_port));
    wr32(b + 15, peer->lanIp);
    wr16(b + 19, peer->lanPort);
    // A peer on this machine or its LAN (a dedicated server next to the relay)
    // shows up with a local address; players outside reach it at ours.
    b[21] = (is_local_ip(ip) && !is_local_ip(ntohl(to->addr.sin_addr.s_addr))) ? JKR_PEER_AT_SERVER : 0;
    queue_event(to, b, sizeof(b));
}

static void introduce(client_t* host, client_t* joiner)
{
    uint32_t key = rnd32() | 1;
    if (g_embedded) return;   // a dedicated server's own: everyone reaches it already
    event_peer(host, joiner, key);
    event_peer(joiner, host, key);
    logf_(1, "introduced %s and host %s for a direct link\n", addr_str(&joiner->addr), addr_str(&host->addr));
}

// Optional trailer of HOST / JOIN (jkrelay_proto.h, "Direct links").
static void get_join_ext(client_t* c, const uint8_t* p, const uint8_t* end)
{
    c->wantDirect = 0;
    c->lanIp = 0;
    c->lanPort = 0;
    if (end - p < JKR_JOIN_EXT_SIZE) return;
    c->wantDirect = (rd32(p) & JKR_JF_DIRECT) != 0;
    c->lanIp = rd32(p + 4);
    c->lanPort = rd16(p + 8);
}

static void event_player_destroyed(client_t* to, uint32_t id)
{
    uint8_t b[5];
    b[0] = JKR_EV_PLAYER_DESTROYED;
    wr32(b + 1, id);
    queue_event(to, b, sizeof(b));
}

// Every member of a session except 'except'.
#define FOR_MEMBERS(s, c, except) \
    for (int _i = 0; _i < MAX_CLIENTS; _i++) \
        for (client_t* c = &g_clients[_i], *_once = c; \
             _once && c->used && c->sess == (s) && c != (except); _once = NULL)

static void remove_player(session_t* s, player_t* p)
{
    uint32_t id = p->id;
    client_t* owner = p->owner;
    p->used = 0;
    FOR_MEMBERS(s, c, owner) event_player_destroyed(c, id);
}

static void leave_session(client_t* c, const char* why)
{
    session_t* s = c->sess;
    if (!s) return;

    if (s->host == c)
    {
        char nm[80];
        name_ascii(s->name, s->nameLen, nm, sizeof(nm));
        logf_(0, "session '%s' closed (%s left: %s)\n", nm, addr_str(&c->addr), why);
        uint8_t b[1] = { JKR_EV_SESSION_LOST };
        FOR_MEMBERS(s, o, c) { queue_event(o, b, 1); o->sess = NULL; }
        memset(s, 0, sizeof(*s));
    }
    else
    {
        // Game players first, then the system player, like DirectPlay.
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (s->players[i].used && s->players[i].owner == c && !(s->players[i].flags & JKR_PF_SYSTEM))
                remove_player(s, &s->players[i]);
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (s->players[i].used && s->players[i].owner == c)
                remove_player(s, &s->players[i]);
        logf_(0, "%s left (%s)\n", addr_str(&c->addr), why);
    }
    c->sess = NULL;
    c->sysId = 0;
}

static void drop_client(client_t* c, const char* why)
{
    if (!c->used) return;
    leave_session(c, why);
    logf_(1, "client %s dropped: %s\n", addr_str(&c->addr), why);
    memset(c, 0, sizeof(*c));
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

static void reply_r(client_t* c, uint8_t type, uint32_t seq, const uint8_t* body, size_t len, uint16_t reserved)
{
    put_hdr(c->lastReply, type, c->token, seq);
    wr16(c->lastReply + 6, reserved);
    if (len) memcpy(c->lastReply + JKR_HDR_SIZE, body, len);
    c->lastReplyLen = (uint16_t)(JKR_HDR_SIZE + len);
    c->lastReplySeq = seq;
    send_to(c, c->lastReply, c->lastReplyLen);
}

static void reply(client_t* c, uint8_t type, uint32_t seq, const uint8_t* body, size_t len)
{
    reply_r(c, type, seq, body, len, 0);
}

static void reply_fail(client_t* c, uint32_t seq, uint32_t hr)
{
    uint8_t b[4];
    wr32(b, hr);
    reply(c, JKR_FAIL, seq, b, 4);
}

#define HR_GENERIC          0x80004005u
#define HR_INVALIDPASSWORD  0x88770154u
#define HR_CANTCREATEPLAYER 0x8877003Cu
#define HR_NONEWPLAYERS     0x8877014Au   // JK shows "game full"
#define HR_NOSESSIONS       0x88770168u
#define HR_ACCESSDENIED     0x88770014u
#define HR_INVALIDPLAYER    0x88770096u
#define SESSION_JOINDISABLED 0x20u

// The player list of JKR_REHOST / JKR_REJOIN (jkrelay_proto.h, "Resuming"):
// checks it, then adds the players with their own ids. 0 if it's malformed or
// an id is taken (nothing added then).
static int add_resumed_players(session_t* s, client_t* c, const uint8_t* p, const uint8_t* end)
{
    int count;
    const uint8_t* q;
    if (p >= end) return 0;
    count = p[0];
    if (!count) return 0;
    q = p + 1;
    for (int i = 0; i < count; i++)
    {
        uint16_t nm[32];
        uint8_t nmLen;
        size_t n;
        if (end - q < 9) return 0;
        uint32_t id = rd32(q), flags = rd32(q + 4);
        if (!id || find_player(s, id)) return 0;
        if ((i == 0) != ((flags & JKR_PF_SYSTEM) != 0)) return 0;   // the system player first, only one
        n = get_str(q + 8, end, nm, &nmLen, 31);
        if (!n) return 0;
        q += 8 + n;
    }
    q = p + 1;
    for (int i = 0; i < count; i++)
    {
        uint16_t nm[32];
        uint8_t nmLen;
        uint32_t id = rd32(q), flags = rd32(q + 4);
        size_t n = get_str(q + 8, end, nm, &nmLen, 31);
        if (i == 0) c->sysId = id;
        flags &= JKR_PF_SYSTEM | (s->host == c ? JKR_PF_HOST : 0);
        if (!add_player_id(s, c, flags, nm, nmLen, id))
        {
            for (int j = 0; j < MAX_PLAYERS; j++)   // full: take back what was added (no events sent yet)
                if (s->players[j].owner == c) memset(&s->players[j], 0, sizeof(s->players[j]));
            return 0;
        }
        q += 8 + n;
    }
    return 1;
}

// Why a player was taken out (JKR_EV_SESSION_LOST), for kick_player and host_kick.
static uint8_t kick_reason(int banMinutes)
{
    return banMinutes == JKR_KICK_RECONNECT ? JKR_LOST_RECONNECT : banMinutes < 0 ? JKR_LOST_KICKED : JKR_LOST_BANNED;
}

static const char* kick_text(int banMinutes, int bByHost)
{
    if (banMinutes == JKR_KICK_RECONNECT) return "asked to reconnect";
    if (banMinutes < 0) return bByHost ? "kicked by the host" : "kicked";
    return bByHost ? "banned by the host" : "kicked";
}

// ---------------------------------------------------------------------------
// A dedicated server's own copy: bans, kicks, rcon (the game asks through
// jkrelay_listen.c, from its own thread, under SERVER_LOCK)
// ---------------------------------------------------------------------------

#ifdef JKRELAY_EMBEDDED
static void jkl_Lock(void);     // jkrelay_listen.c
static void jkl_Unlock(void);
#define SERVER_LOCK()   jkl_Lock()
#define SERVER_UNLOCK() jkl_Unlock()

#define MAX_BANS 128
typedef struct { uint32_t ip; uint64_t until; } ban_t;   // ip in network order; until 0 = for good
static ban_t g_bans[MAX_BANS];

// -1 if not banned, 0 for good, else the minutes left (rounded up).
static int ban_minutes_left(uint32_t ip)
{
    uint64_t now = now_ms();
    for (int i = 0; i < MAX_BANS; i++)
    {
        if (!g_bans[i].ip) continue;
        if (g_bans[i].until && now > g_bans[i].until) { g_bans[i].ip = 0; continue; }
        if (g_bans[i].ip == ip)
            return g_bans[i].until ? (int)((g_bans[i].until - now + 59999) / 60000) : 0;
    }
    return -1;
}

static int is_banned(uint32_t ip)
{
    return ban_minutes_left(ip) >= 0;
}

// A JOIN/REJOIN from a banned address: refused, saying so (JKR_LOST_BANNED).
static void reply_fail_banned(client_t* c, uint32_t seq)
{
    int minutes = ban_minutes_left(c->addr.sin_addr.s_addr);
    uint8_t b[7];
    wr32(b, HR_ACCESSDENIED);
    b[4] = JKR_LOST_BANNED;
    wr16(b + 5, (uint16_t)(minutes > 0 ? (minutes > 0xFFFF ? 0xFFFF : minutes) : 0));
    reply(c, JKR_FAIL, seq, b, sizeof(b));
}

// minutes 0 = for good.
static void ban_add(uint32_t ip, int minutes)
{
    ban_t* b = NULL;
    for (int i = 0; i < MAX_BANS && !b; i++) if (g_bans[i].ip == ip) b = &g_bans[i];
    for (int i = 0; i < MAX_BANS && !b; i++) if (!g_bans[i].ip) b = &g_bans[i];
    if (!b) b = &g_bans[0];
    b->ip = ip;
    b->until = minutes > 0 ? now_ms() + (uint64_t)minutes * 60000u : 0;
}

static int ban_remove(uint32_t ip)
{
    int n = 0;
    for (int i = 0; i < MAX_BANS; i++) if (g_bans[i].ip == ip) { g_bans[i].ip = 0; n++; }
    return n;
}

// The game player 'id' leaves the game for good: its machine is told the game is
// over (as if the host left) and taken out. Returns its IP (network order), 0 if
// there's no such player on another machine.
// banMinutes < 0: a kick (JKR_KICK_RECONNECT: asked to join again); else a ban for
// that long (0 = for good). The player's machine is told why (JKR_LOST_*).
static uint32_t kick_player(uint32_t id, int banMinutes)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        session_t* s = &g_sessions[i];
        player_t* p;
        client_t* c;
        uint8_t ev[4] = { JKR_EV_SESSION_LOST, kick_reason(banMinutes), 0, 0 };
        if (banMinutes > 0) wr16(ev + 2, (uint16_t)(banMinutes > 0xFFFF ? 0xFFFF : banMinutes));
        if (!s->used || !(p = find_player(s, id)) || !(c = p->owner) || c == s->host) continue;
        queue_event(c, ev, sizeof(ev));
        leave_session(c, kick_text(banMinutes, 0));
        return c->addr.sin_addr.s_addr;
    }
    return 0;
}

static const char* player_addr(uint32_t id)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        player_t* p;
        if (g_sessions[i].used && (p = find_player(&g_sessions[i], id)) && p->owner)
            return addr_str(&p->owner->addr);
    }
    return NULL;
}

// Rcon (jkrelay_proto.h, "Rcon")
static char g_rconPassword[128];
typedef struct { struct sockaddr_in addr; uint8_t nonce[16]; uint64_t t; } rcon_nonce_t;
typedef struct { uint32_t ip; int bad; uint64_t blockedUntil, lastCmd; } rcon_bad_t;
#define RCON_ADDRS 64
static rcon_nonce_t g_rconNonces[16];
static rcon_bad_t g_rconBad[RCON_ADDRS];   // addresses that sent commands lately
// A dedicated server's cvars (jkrelay_ListenSetRconLimits): wrong passwords before an
// address is blocked, for how long, and the least time between its commands (0: any).
static int g_rconMaxBad = JKR_RCON_MAX_BAD;
static uint64_t g_rconBlockMs = JKR_RCON_BLOCK_MS;
static uint64_t g_rconFloodMs = 1000;
static struct
{
    int state;                        // 0 free, 1 waiting for the game, 2 being run
    struct sockaddr_in addr;
    uint32_t seq;
    char cmd[JKR_RCON_MAX_CMD + 1];
} g_rconJob;

static void rcon_send(const struct sockaddr_in* to, uint32_t seq, const char* text)
{
    uint8_t pkt[JKR_HDR_SIZE + 2 + 1200];
    size_t len = strlen(text), off = 0;
    uint16_t part = 0;
    do
    {
        size_t n = len - off > 1200 ? 1200 : len - off;
        put_hdr(pkt, JKR_RCON_OUTPUT, 0, seq);
        wr16(pkt + 6, (uint16_t)(part++ | (off + n >= len ? JKR_RCON_LAST : 0)));
        wr16(pkt + JKR_HDR_SIZE, (uint16_t)n);
        memcpy(pkt + JKR_HDR_SIZE + 2, text + off, n);
        net_send(to, pkt, JKR_HDR_SIZE + 2 + n);
        off += n;
    } while (off < len);
}

// An address's entry. A new one takes the place of the one used longest ago,
// blocked ones last.
static rcon_bad_t* rcon_bad_entry(uint32_t ip, int bCreate)
{
    uint64_t now = now_ms();
    rcon_bad_t* oldest = &g_rconBad[0];
    for (int i = 0; i < RCON_ADDRS; i++)
    {
        int blocked = g_rconBad[i].blockedUntil > now, oldestBlocked = oldest->blockedUntil > now;
        if (g_rconBad[i].ip == ip) return &g_rconBad[i];
        if (blocked != oldestBlocked ? !blocked : g_rconBad[i].lastCmd < oldest->lastCmd) oldest = &g_rconBad[i];
    }
    if (!bCreate) return NULL;
    memset(oldest, 0, sizeof(*oldest));
    oldest->ip = ip;
    return oldest;
}

static void rcon_packet(const struct sockaddr_in* from, uint8_t type, uint32_t seq, const uint8_t* b, const uint8_t* end)
{
    uint64_t now = now_ms();
    rcon_bad_t* bad = rcon_bad_entry(from->sin_addr.s_addr, 0);
    if (bad && bad->blockedUntil > now) return;           // too many wrong passwords
    if (type == JKR_RCON_CHALLENGE)
    {
        rcon_nonce_t* e = &g_rconNonces[0];
        uint8_t out[JKR_HDR_SIZE + 16];
        if (end - b < JKR_RCON_PAD) return;
        for (int i = 1; i < 16; i++) if (g_rconNonces[i].t < e->t) e = &g_rconNonces[i];
        e->addr = *from;
        e->t = now;
        for (int i = 0; i < 16; i += 4) wr32(e->nonce + i, rnd32());
        put_hdr(out, JKR_RCON_NONCE, 0, seq);
        memcpy(out + JKR_HDR_SIZE, e->nonce, 16);
        net_send(from, out, sizeof(out));
        return;
    }
    // JKR_RCON: nonce[16], mac[32], u16 len, command
    {
        rcon_nonce_t* e = NULL;
        uint8_t msg[16 + JKR_RCON_MAX_CMD], mac[32];
        unsigned diff = 0;
        size_t n;
        if (end - b < 16 + 32 + 2) return;
        // Flood protection (sv_floodProtect): one command a second from an address.
        bad = rcon_bad_entry(from->sin_addr.s_addr, 1);
        if (g_rconFloodMs && bad->lastCmd && now - bad->lastCmd < g_rconFloodMs) return;
        bad->lastCmd = now;
        n = rd16(b + 48);
        if (n > JKR_RCON_MAX_CMD || (size_t)(end - b) < 50 + n) return;
        for (int i = 0; i < 16 && !e; i++)
            if (g_rconNonces[i].t && now - g_rconNonces[i].t <= JKR_RCON_NONCE_MS && !memcmp(g_rconNonces[i].nonce, b, 16)
                && g_rconNonces[i].addr.sin_addr.s_addr == from->sin_addr.s_addr && g_rconNonces[i].addr.sin_port == from->sin_port)
                e = &g_rconNonces[i];
        if (!e) { rcon_send(from, seq, "Rcon: the challenge expired, try again."); return; }
        e->t = 0;   // once
        memcpy(msg, b, 16);
        memcpy(msg + 16, b + 50, n);
        // A public command (players' "servermaps"): signed with an empty password, allowed
        // without the rcon password and never counted as a wrong one.
        {
            int bPublic = (n == 7 || (n > 7 && b[50 + 7] == ' ')) && !memcmp(b + 50, "maplist", 7);
            for (size_t i = 0; bPublic && i < n; i++)
                if (b[50 + i] < 32 || b[50 + i] > 126) bPublic = 0;
            if (bPublic)
            {
                jkr_HmacSha256("", msg, 16 + n, mac);
                for (int i = 0; i < 32; i++) diff |= mac[i] ^ b[16 + i];
                if (!diff) goto accepted;
                diff = 0;
            }
        }
        if (!g_rconPassword[0])
        {
            rcon_send(from, seq, "Rcon is off on this server.");
            return;
        }
        jkr_HmacSha256(g_rconPassword, msg, 16 + n, mac);
        for (int i = 0; i < 32; i++) diff |= mac[i] ^ b[16 + i];
        if (diff)
        {
            if (++bad->bad >= g_rconMaxBad)
            {
                bad->bad = 0;
                bad->blockedUntil = now + g_rconBlockMs;
                logf_(0, "[rcon] bad password from %s, ignoring it for %d minutes\n", addr_str(from), (int)(g_rconBlockMs / 60000));
            }
            else
                logf_(0, "[rcon] bad password from %s\n", addr_str(from));
            rcon_send(from, seq, "Bad rcon password.");
            return;
        }
        bad->bad = 0;
accepted:
        if (g_rconJob.state)
        {
            rcon_send(from, seq, "Rcon: the server is busy with another command, try again.");
            return;
        }
        g_rconJob.state = 1;
        g_rconJob.addr = *from;
        g_rconJob.seq = seq;
        memcpy(g_rconJob.cmd, b + 50, n);
        g_rconJob.cmd[n] = 0;
        for (size_t i = 0; i < n; i++)   // one line of printable ASCII
            if ((uint8_t)g_rconJob.cmd[i] < 32 || (uint8_t)g_rconJob.cmd[i] > 126) g_rconJob.cmd[i] = ' ';
        logf_(0, "[rcon] %s: %s\n", addr_str(from), g_rconJob.cmd);
    }
}
#else
#define SERVER_LOCK()
#define SERVER_UNLOCK()
static int is_banned(uint32_t ip) { (void)ip; return 0; }
static void reply_fail_banned(client_t* c, uint32_t seq) { reply_fail(c, seq, HR_ACCESSDENIED); }
#endif

// A dedicated server's own copy lists only its game: nobody else hosts on it.
// ---------------------------------------------------------------------------
// Hosts' admin and rcon through a relay (jkrelay_proto.h)
// ---------------------------------------------------------------------------

// The game's own bans (JKR_SETBANS): -1 not banned, 0 for good, else minutes left.
static int session_ban_minutes(session_t* s, uint32_t ip)
{
    uint64_t now = now_ms();
    for (int i = 0; s && i < JKR_MAX_HOST_BANS; i++)
    {
        if (!s->bans[i].ip || s->bans[i].ip != ip) continue;
        if (s->bans[i].until && now > s->bans[i].until) { s->bans[i].ip = 0; continue; }
        return s->bans[i].until ? (int)((s->bans[i].until - now + 59999) / 60000) : 0;
    }
    return -1;
}

static void session_ban_add(session_t* s, uint32_t ip, int minutes)
{
    int k = -1;
    for (int i = 0; i < JKR_MAX_HOST_BANS && k < 0; i++) if (s->bans[i].ip == ip) k = i;
    for (int i = 0; i < JKR_MAX_HOST_BANS && k < 0; i++) if (!s->bans[i].ip) k = i;
    if (k < 0) k = 0;
    s->bans[k].ip = ip;
    s->bans[k].until = minutes > 0 ? now_ms() + (uint64_t)minutes * 60000u : 0;
}

// A JOIN/REJOIN refused for a ban of that many minutes left (0 for good).
static void reply_fail_ban(client_t* c, uint32_t seq, int minutes)
{
    uint8_t b[7];
    wr32(b, HR_ACCESSDENIED);
    b[4] = JKR_LOST_BANNED;
    wr16(b + 5, (uint16_t)(minutes > 0 ? (minutes > 0xFFFF ? 0xFFFF : minutes) : 0));
    reply(c, JKR_FAIL, seq, b, sizeof(b));
}

// The host's kick: player id (not the host's) out of the host's game, told why;
// banMinutes >= 0 also bans its address from the game (0 for good). Its IP, 0 if none.
static uint32_t host_kick(session_t* s, client_t* host, uint32_t id, int banMinutes)
{
    player_t* p = find_player(s, id);
    client_t* c;
    uint32_t ip;
    uint8_t ev[4] = { JKR_EV_SESSION_LOST, kick_reason(banMinutes), 0, 0 };
    if (!p || !(c = p->owner) || c == host) return 0;
    if (banMinutes > 0) wr16(ev + 2, (uint16_t)(banMinutes > 0xFFFF ? 0xFFFF : banMinutes));
    ip = c->addr.sin_addr.s_addr;
    if (banMinutes >= 0) session_ban_add(s, ip, banMinutes);
    queue_event(c, ev, sizeof(ev));
    leave_session(c, kick_text(banMinutes, 1));
    return ip;
}

#define MAX_TUNNELS 64
static struct
{
    struct sockaddr_in addr;   // who asked
    client_t* host;            // the game's host it went to
    uint64_t until;
} g_tunnels[MAX_TUNNELS];
static struct { uint32_t ip; uint64_t windowMs; int count; } g_tunnelRate[MAX_TUNNELS];

// An rcon packet for a game hosted here (its tag names it): to the game's host.
static void tunnel_forward(const struct sockaddr_in* from, const uint8_t* p, size_t len)
{
    const uint8_t* b = p + JKR_HDR_SIZE;
    const uint8_t* end = p + len;
    const uint8_t* tag = NULL;
    session_t* s;
    uint64_t now = now_ms();
    uint8_t out[JKR_MAX_PACKET];
    int k = -1, rate = -1;

    if (p[4] == JKR_RCON_CHALLENGE && end - b >= JKR_RCON_TAG_SIZE) tag = b;
    else if (p[4] == JKR_RCON && end - b >= 50)
    {
        size_t n = rd16(b + 48);
        if ((size_t)(end - b) >= 50 + n + JKR_RCON_TAG_SIZE) tag = b + 50 + n;
    }
    if (!tag || memcmp(tag, "JKRT", 4)) return;
    if (!(s = find_session(tag + 4)) || !s->host) return;
    if (JKR_HDR_SIZE + 6 + len > sizeof(out)) return;

    // At most 8 a second from an address: the host isn't to be flooded through us.
    for (int i = 0; i < MAX_TUNNELS && rate < 0; i++) if (g_tunnelRate[i].ip == from->sin_addr.s_addr) rate = i;
    if (rate < 0)
    {
        rate = 0;
        for (int i = 1; i < MAX_TUNNELS; i++) if (g_tunnelRate[i].windowMs < g_tunnelRate[rate].windowMs) rate = i;
        g_tunnelRate[rate].ip = from->sin_addr.s_addr;
        g_tunnelRate[rate].windowMs = 0;
    }
    if (now - g_tunnelRate[rate].windowMs >= 1000) { g_tunnelRate[rate].windowMs = now; g_tunnelRate[rate].count = 0; }
    if (++g_tunnelRate[rate].count > 8) return;

    for (int i = 0; i < MAX_TUNNELS && k < 0; i++)
        if (g_tunnels[i].host == s->host && g_tunnels[i].addr.sin_addr.s_addr == from->sin_addr.s_addr
            && g_tunnels[i].addr.sin_port == from->sin_port) k = i;
    for (int i = 0; i < MAX_TUNNELS && k < 0; i++) if (!g_tunnels[i].host || g_tunnels[i].until < now) k = i;
    if (k < 0) k = 0;
    g_tunnels[k].addr = *from;
    g_tunnels[k].host = s->host;
    g_tunnels[k].until = now + JKR_TUNNEL_MS;

    put_hdr(out, JKR_TUNNEL, s->host->token, 0);
    memcpy(out + JKR_HDR_SIZE, &from->sin_addr.s_addr, 4);
    memcpy(out + JKR_HDR_SIZE + 4, &from->sin_port, 2);
    memcpy(out + JKR_HDR_SIZE + 6, p, len);
    send_to(s->host, out, JKR_HDR_SIZE + 6 + len);
    if (p[4] == JKR_RCON)
    {
        char name[96];
        name_ascii(s->name, s->nameLen, name, sizeof(name));
        logf_(1, "[rcon] %s -> the host of '%s'\n", addr_str(from), name);
    }
}

// The host's answer to a tunnelled packet: on to whoever asked, if they did lately.
static void tunnel_reply(client_t* c, const uint8_t* b, const uint8_t* end)
{
    struct sockaddr_in to;
    uint64_t now = now_ms();
    if (end - b < 6 + JKR_HDR_SIZE || rd32(b + 6) != JKR_MAGIC) return;
    if (b[6 + 4] != JKR_RCON_NONCE && b[6 + 4] != JKR_RCON_OUTPUT) return;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    memcpy(&to.sin_addr.s_addr, b, 4);
    memcpy(&to.sin_port, b + 4, 2);
    for (int i = 0; i < MAX_TUNNELS; i++)
    {
        if (g_tunnels[i].host != c || g_tunnels[i].until < now || g_tunnels[i].addr.sin_addr.s_addr != to.sin_addr.s_addr
            || g_tunnels[i].addr.sin_port != to.sin_port) continue;
        net_send(&to, b + 6, (size_t)(end - b - 6));
        return;
    }
}

static int may_host(const client_t* c)
{
    return !g_embedded || (ntohl(c->addr.sin_addr.s_addr) >> 24) == 127;
}

static void handle_request(client_t* c, uint8_t type, uint32_t seq, const uint8_t* b, const uint8_t* end)
{
    uint8_t out[JKR_MAX_PACKET];

    if (seq == c->lastReplySeq && c->lastReplyLen)
    {
        send_to(c, c->lastReply, c->lastReplyLen);   // retry of an answered request
        return;
    }

    switch (type)
    {
        case JKR_HOST:
        {
            session_t* s = NULL;
            size_t n;
            if (end - b < 16) return;
            if (!may_host(c)) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            if (c->sess) leave_session(c, "hosting a new game");
            for (int i = 0; i < MAX_SESSIONS; i++) if (!g_sessions[i].used) { s = &g_sessions[i]; break; }
            if (!s) { reply_fail(c, seq, HR_GENERIC); return; }
            memset(s, 0, sizeof(*s));
            memcpy(s->appGuid, b, 16);
            n = get_desc(b + 16, end, s);
            if (!n) { memset(s, 0, sizeof(*s)); return; }
            {
                size_t pn = get_str(b + 16 + n, end, s->password, &s->passwordLen, JKR_MAX_PASSWORD);
                get_join_ext(c, pn ? b + 16 + n + pn : end, end);
            }
            for (int i = 0; i < 16; i += 4) wr32(s->guid + i, rnd32());
            s->nextId = rnd32() | 0x01000000u;
            s->used = 1;
            s->host = c;
            c->sess = s;
            player_t* sys = add_player(s, c, JKR_PF_SYSTEM | JKR_PF_HOST, NULL, 0);
            c->sysId = sys->id;
            n = put_desc(out, s);
            wr32(out + n, c->sysId);
            reply(c, JKR_HOSTED, seq, out, n + 4);
            {
                char nm[80];
                name_ascii(s->name, s->nameLen, nm, sizeof(nm));
                logf_(0, "%s hosts '%s' (max %u)%s\n", addr_str(&c->addr), nm, s->maxPlayers, s->passwordLen ? " [password]" : "");
            }
            break;
        }

        case JKR_LIST:
        {
            uint32_t start, total = 0, count = 0, idx = 0;
            size_t n = 4;
            int bAddrs, bTails, bFull = 0;
            if (end - b < 20) return;
            start = rd32(b + 16);
            // New clients also get the listed servers, each with its address.
            bAddrs = !g_embedded && end - b >= 24 && (rd32(b + 20) & JKR_LF_SERVERS);
            bTails = end - b >= 24 && (rd32(b + 20) & JKR_LF_TAILS);   // and each game's mod
            for (int i = 0; i < MAX_SESSIONS; i++)
                if (g_sessions[i].used && !memcmp(g_sessions[i].appGuid, b, 16)) total++;
            for (int i = 0; bAddrs && i < MAX_LISTED; i++)
                if (g_listed[i].used && g_listed[i].reachable && !memcmp(g_listed[i].appGuid, b, 16)) total++;
            for (int i = 0; i < MAX_SESSIONS && !bFull; i++)
            {
                session_t* s = &g_sessions[i];
                if (!s->used || memcmp(s->appGuid, b, 16)) continue;
                if (idx++ < start) continue;
                if (n + 44 + 1 + 2 * s->nameLen + (bAddrs ? JKR_LIST_ADDR_SIZE : 0) + (bTails ? 1 + s->tailLen : 0) > JKR_MAX_PACKET - JKR_HDR_SIZE) { bFull = 1; break; }
                n += put_desc(out + n, s);
                if (bAddrs) { memset(out + n, 0, JKR_LIST_ADDR_SIZE); n += JKR_LIST_ADDR_SIZE; }
                if (bTails) n += put_tail(out + n, s);
                count++;
            }
            for (int i = 0; bAddrs && i < MAX_LISTED && !bFull; i++)
            {
                listed_t* l = &g_listed[i];
                uint32_t ip;
                if (!l->used || !l->reachable || memcmp(l->appGuid, b, 16)) continue;
                if (idx++ < start) continue;
                if (n + 44 + 1 + 2 * l->game.nameLen + JKR_LIST_ADDR_SIZE + (bTails ? 1 + l->game.tailLen : 0) > JKR_MAX_PACKET - JKR_HDR_SIZE) { bFull = 1; break; }
                n += put_desc_n(out + n, &l->game, l->curPlayers);
                ip = ntohl(l->addr.sin_addr.s_addr);
                wr32(out + n, ip);
                wr16(out + n + 4, ntohs(l->addr.sin_port));
                // On this machine or its LAN: players outside reach it at our address.
                out[n + 6] = JKR_LA_SERVER | ((is_local_ip(ip) && !is_local_ip(ntohl(c->addr.sin_addr.s_addr))) ? JKR_LA_AT_SERVER : 0);
                n += JKR_LIST_ADDR_SIZE;
                if (bTails) n += put_tail(out + n, &l->game);
                count++;
            }
            wr16(out, (uint16_t)total);
            wr16(out + 2, (uint16_t)count);
            // Browsers reach the listed servers through our gateway, when we take WebSockets.
            reply_r(c, JKR_LISTREPLY, seq, out, n, (bAddrs ? JKR_LR_ADDRS : 0) | (bTails ? JKR_LR_TAILS : 0)
                                                   | ((bAddrs && end - b >= 24 && (rd32(b + 20) & JKR_LF_GATEWAY) && g_wsListen != JKRS_BADSOCK)
                                                      ? JKR_LR_GATEWAY : 0));
            break;
        }

        case JKR_HOSTKICK:
        {
            session_t* s = c->sess;
            uint32_t ip;
            if (!s || s->host != c) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            if (end - b < 8) return;
            ip = host_kick(s, c, rd32(b), (int32_t)rd32(b + 4));
            if (!ip) { reply_fail(c, seq, HR_INVALIDPLAYER); return; }
            memcpy(out, &ip, 4);
            reply(c, JKR_OK, seq, out, 4);
            break;
        }

        case JKR_SETBANS:
        {
            session_t* s = c->sess;
            int n;
            uint64_t now = now_ms();
            if (!s || s->host != c) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            if (end - b < 2) return;
            n = rd16(b);
            if (n > JKR_MAX_HOST_BANS || end - b < 2 + 6 * n) return;
            memset(s->bans, 0, sizeof(s->bans));
            for (int i = 0; i < n; i++)
            {
                int minutes = rd16(b + 2 + 6 * i + 4);
                memcpy(&s->bans[i].ip, b + 2 + 6 * i, 4);
                s->bans[i].until = minutes ? now + (uint64_t)minutes * 60000u : 0;
            }
            reply(c, JKR_OK, seq, NULL, 0);
            break;
        }

        case JKR_PLAYERADDR:
        {
            session_t* s = c->sess;
            player_t* p;
            if (!s || s->host != c) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            if (end - b < 4) return;
            if (!(p = find_player(s, rd32(b))) || !p->owner) { reply_fail(c, seq, HR_INVALIDPLAYER); return; }
            memcpy(out, &p->owner->addr.sin_addr.s_addr, 4);
            memcpy(out + 4, &p->owner->addr.sin_port, 2);
            reply(c, JKR_OK, seq, out, 6);
            break;
        }

        case JKR_JOIN:
        {
            session_t* s = NULL;
            uint16_t pw[JKR_MAX_PASSWORD];
            uint8_t pwLen = 0;
            int existing = 0;
            size_t n;
            if (end - b < 17) return;
            if (is_banned(c->addr.sin_addr.s_addr))
            {
                logf_(0, "%s: banned, not let in\n", addr_str(&c->addr));
                reply_fail_banned(c, seq);
                return;
            }
            s = find_session(b);
            if (!s) { reply_fail(c, seq, HR_NOSESSIONS); return; }
            {
                int bm = session_ban_minutes(s, c->addr.sin_addr.s_addr);   // the host's own bans
                if (bm >= 0)
                {
                    logf_(0, "%s: banned from that game, not let in\n", addr_str(&c->addr));
                    reply_fail_ban(c, seq, bm);
                    return;
                }
            }
            size_t pn = get_str(b + 16, end, pw, &pwLen, JKR_MAX_PASSWORD);
            if (!pn) return;
            if (s->passwordLen && (pwLen != s->passwordLen || memcmp(pw, s->password, 2u * pwLen)))
            {
                logf_(0, "%s: wrong password\n", addr_str(&c->addr));
                reply_fail(c, seq, HR_INVALIDPASSWORD);
                return;
            }
            if ((s->flags & SESSION_JOINDISABLED) || (uint32_t)count_players(s) >= s->maxPlayers)
            {
                reply_fail(c, seq, HR_NONEWPLAYERS);
                return;
            }
            if (c->sess) leave_session(c, "joining another game");
            get_join_ext(c, b + 16 + pn, end);
            player_t* sys = add_player(s, c, JKR_PF_SYSTEM, NULL, 0);
            if (!sys) { reply_fail(c, seq, HR_CANTCREATEPLAYER); return; }
            c->sess = s;
            c->sysId = sys->id;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (s->players[i].used && s->players[i].owner != c) existing++;
            n = put_desc(out, s);
            wr32(out + n, c->sysId);
            wr32(out + n + 4, (uint32_t)existing);
            reply(c, JKR_JOINED, seq, out, n + 8);
            // Everyone already in the game, then tell them about the newcomer's machine.
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (s->players[i].used && s->players[i].owner != c) event_player_created(c, &s->players[i], 1);
            FOR_MEMBERS(s, o, c) event_player_created(o, sys, 0);
            if (c->wantDirect && s->host && s->host->wantDirect) introduce(s->host, c);
            {
                char nm[80];
                name_ascii(s->name, s->nameLen, nm, sizeof(nm));
                logf_(0, "%s joined '%s'\n", addr_str(&c->addr), nm);
            }
            break;
        }

        case JKR_CREATEPLAYER:
        {
            uint16_t nm[32];
            uint8_t nmLen = 0;
            player_t* p;
            if (!c->sess || end - b < 5) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            if (!get_str(b + 4, end, nm, &nmLen, 31)) return;
            p = add_player(c->sess, c, 0, nm, nmLen);
            if (!p) { reply_fail(c, seq, HR_CANTCREATEPLAYER); return; }
            wr32(out, p->id);
            reply(c, JKR_PLAYERID, seq, out, 4);
            FOR_MEMBERS(c->sess, o, c) event_player_created(o, p, 0);
            {
                char a[40];
                name_ascii(nm, nmLen, a, sizeof(a));
                logf_(1, "%s created player %08x '%s'\n", addr_str(&c->addr), p->id, a);
            }
            break;
        }

        case JKR_DESTROYPLAYER:
        {
            player_t* p;
            if (!c->sess || end - b < 4) { reply(c, JKR_OK, seq, NULL, 0); return; }
            p = find_player(c->sess, rd32(b));
            if (p && p->owner == c) remove_player(c->sess, p);
            reply(c, JKR_OK, seq, NULL, 0);
            break;
        }

        case JKR_SETDESC:
        {
            session_t* s = c->sess;
            uint8_t ev[1 + JKR_MAX_PACKET];
            size_t n;
            if (!s || s->host != c) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            n = get_desc(b, end, s);
            if (!n) return;
            {
                size_t pn = get_str(b + n, end, s->password, &s->passwordLen, JKR_MAX_PASSWORD);
                if (pn) get_tail(b + n + pn, end, s);   // Added: the host's mod
            }
            reply(c, JKR_OK, seq, NULL, 0);
            ev[0] = JKR_EV_DESC_CHANGED;
            n = 1 + put_desc(ev + 1, s);
            FOR_MEMBERS(s, o, c) queue_event(o, ev, n);
            break;
        }

        case JKR_REHOST:
        {
            session_t* s;
            size_t n, pn;
            if (end - b < 16 + JKR_DESC_FIXED) return;
            if (!may_host(c)) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
            if (find_session(b + 16))
            {
                reply_fail(c, seq, HR_ACCESSDENIED);   // still here: someone else's
                return;
            }
            if (c->sess) leave_session(c, "hosting a new game");
            s = NULL;
            for (int i = 0; i < MAX_SESSIONS; i++) if (!g_sessions[i].used) { s = &g_sessions[i]; break; }
            if (!s) { reply_fail(c, seq, HR_GENERIC); return; }
            memset(s, 0, sizeof(*s));
            memcpy(s->appGuid, b, 16);
            memcpy(s->guid, b + 16, 16);
            n = get_desc(b + 16, end, s);
            pn = n ? get_str(b + 16 + n, end, s->password, &s->passwordLen, JKR_MAX_PASSWORD) : 0;
            if (!pn || end - (b + 16 + n + pn) < JKR_JOIN_EXT_SIZE + 4) { memset(s, 0, sizeof(*s)); return; }
            {
                const uint8_t* q = b + 16 + n + pn;
                get_join_ext(c, q, end);
                s->nextId = rd32(q + JKR_JOIN_EXT_SIZE);
                s->used = 1;
                s->host = c;
                c->sess = s;
                if (!add_resumed_players(s, c, q + JKR_JOIN_EXT_SIZE + 4, end))
                {
                    c->sess = NULL;
                    c->sysId = 0;
                    memset(s, 0, sizeof(*s));
                    reply_fail(c, seq, HR_GENERIC);
                    return;
                }
            }
            s->rehostedMs = now_ms();
            reply(c, JKR_OK, seq, NULL, 0);
            {
                char nm[80];
                name_ascii(s->name, s->nameLen, nm, sizeof(nm));
                logf_(0, "%s put back '%s' (after a server restart or a lost connection)\n", addr_str(&c->addr), nm);
            }
            break;
        }

        case JKR_REJOIN:
        {
            session_t* s;
            uint16_t pw[JKR_MAX_PASSWORD];
            uint8_t pwLen = 0;
            size_t pn;
            if (end - b < 17) return;
            if (is_banned(c->addr.sin_addr.s_addr)) { reply_fail_banned(c, seq); return; }
            s = find_session(b);
            if (!s) { reply_fail(c, seq, HR_NOSESSIONS); return; }   // its host isn't back yet
            {
                int bm = session_ban_minutes(s, c->addr.sin_addr.s_addr);
                if (bm >= 0) { reply_fail_ban(c, seq, bm); return; }
            }
            if (!s->rehostedMs || now_ms() - s->rehostedMs > JKR_REJOIN_WINDOW_MS)
            {
                reply_fail(c, seq, HR_ACCESSDENIED);
                return;
            }
            pn = get_str(b + 16, end, pw, &pwLen, JKR_MAX_PASSWORD);
            if (!pn || end - (b + 16 + pn) < JKR_JOIN_EXT_SIZE) return;
            if (s->passwordLen && (pwLen != s->passwordLen || memcmp(pw, s->password, 2u * pwLen)))
            {
                reply_fail(c, seq, HR_INVALIDPASSWORD);
                return;
            }
            if (c->sess) leave_session(c, "joining another game");
            get_join_ext(c, b + 16 + pn, end);
            c->sess = s;
            if (!add_resumed_players(s, c, b + 16 + pn + JKR_JOIN_EXT_SIZE, end))
            {
                c->sess = NULL;
                c->sysId = 0;
                reply_fail(c, seq, HR_ACCESSDENIED);
                return;
            }
            reply(c, JKR_OK, seq, NULL, 0);
            // Everyone already back, and the returning players to them; all
            // "initial": they knew each other before.
            for (int i = 0; i < MAX_PLAYERS; i++)
            {
                player_t* p = &s->players[i];
                if (!p->used) continue;
                if (p->owner != c) event_player_created(c, p, 1);
                else FOR_MEMBERS(s, o, c) event_player_created(o, p, 1);
            }
            if (c->wantDirect && s->host && s->host->wantDirect) introduce(s->host, c);
            {
                char nm[80];
                name_ascii(s->name, s->nameLen, nm, sizeof(nm));
                logf_(0, "%s back in '%s'\n", addr_str(&c->addr), nm);
            }
            break;
        }

        case JKR_ANNOUNCE:
        {
            session_t tmp;
            listed_t* l = NULL;
            size_t n;
            uint16_t port;
            int perIp = 0;
            uint64_t now = now_ms();
            if (g_embedded) { reply_fail(c, seq, HR_ACCESSDENIED); return; }   // not a master server
            if (end - b < 16 + JKR_DESC_FIXED) return;
            memset(&tmp, 0, sizeof(tmp));
            n = get_desc(b + 16, end, &tmp);
            if (!n || end - (b + 16 + n) < 2) return;
            port = rd16(b + 16 + n);
            if (!port) return;
            get_tail(b + 16 + n + 2, end, &tmp);   // Added: its mod, if it says
            for (int i = 0; i < MAX_LISTED && !l; i++)
            {
                listed_t* e = &g_listed[i];
                if (!e->used || e->addr.sin_addr.s_addr != c->addr.sin_addr.s_addr) continue;
                if (ntohs(e->addr.sin_port) == port) l = e;
                else perIp++;
            }
            if (!l)
            {
                if (perIp >= MAX_LISTED_PER_IP) { reply_fail(c, seq, HR_ACCESSDENIED); return; }
                for (int i = 0; i < MAX_LISTED && !l; i++) if (!g_listed[i].used) l = &g_listed[i];
                if (!l) { reply_fail(c, seq, HR_GENERIC); return; }
                memset(l, 0, sizeof(*l));
                l->used = 1;
                l->addr.sin_family = AF_INET;
                l->addr.sin_addr = c->addr.sin_addr;
                l->addr.sin_port = htons(port);
            }
            memcpy(l->appGuid, b, 16);
            memcpy(tmp.guid, b + 16, 16);
            l->game = tmp;
            l->curPlayers = rd32(b + 16 + 24);
            l->lastAnnounce = now;
            if (!l->reachable && now - l->lastProbe >= 3000)
            {
                // Listed once it answers where players will send.
                uint8_t h[JKR_HDR_SIZE + 4];
                l->probeSeq = rnd32() | 1;
                put_hdr(h, JKR_HELLO, 0, l->probeSeq);
                wr32(h + JKR_HDR_SIZE, JKR_VERSION);
                sendto(g_probeSock, (const char*)h, sizeof(h), 0, (const struct sockaddr*)&l->addr, sizeof(l->addr));
                l->lastProbe = now;
            }
            out[0] = (uint8_t)l->reachable;
            reply(c, JKR_OK, seq, out, 1);
            break;
        }

        case JKR_LEAVE:
            leave_session(c, "left");
            reply(c, JKR_OK, seq, NULL, 0);
            break;
    }
}

// ---------------------------------------------------------------------------
// Packet loop
// ---------------------------------------------------------------------------

static client_t* find_client(const struct sockaddr_in* a)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].addr.sin_addr.s_addr == a->sin_addr.s_addr &&
            g_clients[i].addr.sin_port == a->sin_port)
            return &g_clients[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// Handshake cookies (2026-10-31). A HELLO used to take a client slot for
// JKR_TIMEOUT_MS, so HELLOs from forged addresses (~13 a second) could keep all
// MAX_CLIENTS slots busy and lock real players out. Now WELCOME's token is a
// keyed hash of the address and a time window, nothing is kept, and the slot is
// taken by the first packet that brings the token back. Clients are unchanged.
// ---------------------------------------------------------------------------

#define HELLO_COOKIE_MS     30000   // a token is good for this window and the next
#define MAX_CLIENTS_PER_IP  32      // one machine (or a LAN behind one address) can't fill the server

static void ws_sha1(const uint8_t* msg, size_t len, uint8_t out[20]);
static uint8_t g_cookieKey[16];
static int g_bCookieKey;

static uint32_t hello_cookie(const struct sockaddr_in* a, uint64_t window)
{
    uint8_t m[16 + 4 + 2 + 8], h[20];
    if (!g_bCookieKey)
    {
        for (int i = 0; i < 4; i++) wr32(g_cookieKey + 4 * i, rnd32());
        g_bCookieKey = 1;
    }
    memcpy(m, g_cookieKey, 16);
    memcpy(m + 16, &a->sin_addr.s_addr, 4);
    memcpy(m + 20, &a->sin_port, 2);
    for (int i = 0; i < 8; i++) m[22 + i] = (uint8_t)(window >> (8 * i));
    ws_sha1(m, sizeof(m), h);
    return rd32(h) | 1;
}

// A client for 'from' if 'token' is a WELCOME it was given lately (this window or
// the last), and there's room for it. NULL otherwise.
static client_t* hello_take(const struct sockaddr_in* from, uint32_t token)
{
    uint64_t window = now_ms() / HELLO_COOKIE_MS;
    client_t* c = NULL;
    int sameIp = 0;
    if (token != hello_cookie(from, window) && token != hello_cookie(from, window - 1))
        return NULL;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].addr.sin_addr.s_addr == from->sin_addr.s_addr) sameIp++;
    if (sameIp >= MAX_CLIENTS_PER_IP)
    {
        logf_(1, "client %s refused: %d already from that address\n", addr_str(from), sameIp);
        return NULL;
    }
    for (int i = 0; i < MAX_CLIENTS && !c; i++) if (!g_clients[i].used) c = &g_clients[i];
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->addr = *from;
    c->token = token;
    c->evNext = 1;
    c->lastRx = now_ms();
    logf_(1, "client %s connected\n", addr_str(from));
    return c;
}


// ---------------------------------------------------------------------------
// Listing a dedicated server's own copy on a master server
// ---------------------------------------------------------------------------

#ifdef JKRELAY_EMBEDDED
// "host[:port]" of the master server (port JKR_DEFAULT_PORT if none), and our
// own port to announce. Before the server's thread starts.
static void server_set_master(const char* name, int port)
{
    memset(&g_master, 0, sizeof(g_master));
    if (!name || !name[0]) return;
    strncpy(g_master.name, name, sizeof(g_master.name) - 1);
    g_master.port = port;
    g_master.set = 1;
}
#endif

static int master_resolve(void)
{
    char host[256];
    char* colon;
    uint16_t port = JKR_DEFAULT_PORT;
    struct addrinfo hints, *res = NULL;
    strncpy(host, g_master.name, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    colon = strrchr(host, ':');
    if (colon)
    {
        port = (uint16_t)atoi(colon + 1);
        *colon = 0;
        if (!port) port = JKR_DEFAULT_PORT;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    memset(&g_master.addr, 0, sizeof(g_master.addr));
    g_master.addr.sin_family = AF_INET;
    g_master.addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    g_master.addr.sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}

static void master_send(uint8_t type, uint32_t seq, const uint8_t* body, size_t len)
{
    uint8_t pkt[JKR_MAX_PACKET];
    put_hdr(pkt, type, type == JKR_HELLO ? 0 : g_master.token, seq);
    if (len) memcpy(pkt + JKR_HDR_SIZE, body, len);
    sendto(g_sock, (const char*)pkt, (int)(JKR_HDR_SIZE + len), 0, (const struct sockaddr*)&g_master.addr, sizeof(g_master.addr));
    g_master.lastTx = now_ms();
}

// Handshakes with the master server, then announces our game every
// JKR_ANNOUNCE_MS and soon after it changes (players joining or leaving).
static void master_tick(uint64_t now)
{
    session_t* s = NULL;
    uint8_t body[JKR_MAX_PACKET];
    uint32_t hash;
    size_t n;

    if (!g_master.set) return;
    if (!g_master.resolved)
    {
        if (g_master.lastResolve && now - g_master.lastResolve < 30000) return;
        g_master.lastResolve = now;
        if (!master_resolve())
        {
            if (!g_master.bWarned) logf_(0, "can't find master server %s; trying again every 30 s\n", g_master.name);
            g_master.bWarned = 1;
            return;
        }
        g_master.resolved = 1;
        g_master.bWarned = 0;
    }
    for (int i = 0; i < MAX_SESSIONS && !s; i++) if (g_sessions[i].used && g_sessions[i].host) s = &g_sessions[i];
    if (!s) return;   // nothing hosted (yet)
    if (!g_master.token)
    {
        if (now - g_master.lastTx < 1000) return;
        wr32(body, JKR_VERSION);
        g_master.helloSeq = rnd32() | 1;
        master_send(JKR_HELLO, g_master.helloSeq, body, 4);
        return;
    }
    hash = (uint32_t)count_players(s) * 2654435761u ^ s->flags ^ s->maxPlayers * 40503u ^ s->user[0] ^ s->user[1] ^ s->user[2] ^ s->user[3];
    for (int i = 0; i < s->nameLen; i++) hash = hash * 31u + s->name[i];
    for (int i = 0; i < s->tailLen; i++) hash = hash * 31u + s->tail[i];
    if (now - g_master.lastAnnounce < JKR_ANNOUNCE_MS && (hash == g_master.lastHash || now - g_master.lastAnnounce < 2000))
        return;
    memcpy(body, s->appGuid, 16);
    n = 16 + put_desc(body + 16, s);
    wr16(body + n, (uint16_t)g_master.port);
    n += 2;
    n += put_tail(body + n, s);   // Added: our mod (older master servers ignore it)
    g_master.annSeq = rnd32() | 1;   // each one new: the master server answers repeats from its cache
    master_send(JKR_ANNOUNCE, g_master.annSeq, body, n);
    g_master.lastAnnounce = now;
    g_master.lastHash = hash;
}

// Everything from the master server but its reachability HELLO.
static void master_packet(uint8_t type, uint32_t token, uint32_t seq, const uint8_t* b, const uint8_t* end)
{
    switch (type)
    {
        case JKR_WELCOME:
            if (seq != g_master.helloSeq || end - b < 4) return;
            g_master.token = rd32(b);
            g_master.lastAnnounce = 0;   // announce right away
            break;
        case JKR_OK:
            if (seq != g_master.annSeq || end - b < 1) return;
            if (b[0] && !g_master.listed)
                logf_(0, "listed on master server %s\n", g_master.name);
            if (!b[0] && g_master.listed)
                logf_(0, "master server %s doesn't list this server any more\n", g_master.name);
            if (!b[0] && ++g_master.unlistedOks == 3)
                logf_(0, "master server %s can't reach this server: let UDP %d in (firewall, port forwarding)\n",
                      g_master.name, g_master.port);
            if (b[0]) g_master.unlistedOks = 0;
            g_master.listed = b[0];
            break;
        case JKR_FAIL:
            if (seq != g_master.annSeq) return;
            if (!g_master.bRefused)
                logf_(0, "master server %s won't list this server (error %08x)\n", g_master.name, end - b >= 4 ? rd32(b) : 0);
            g_master.bRefused = 1;
            break;
        case JKR_UNKNOWN:
            // It restarted or forgot us: handshake again.
            if (token != g_master.token) return;
            g_master.token = 0;
            g_master.listed = 0;
            break;
    }
}

// Master server: a listed server answering our reachability HELLO (g_probeSock).
static void handle_probe_reply(const struct sockaddr_in* from, const uint8_t* p, size_t len)
{
    if (len < JKR_HDR_SIZE || rd32(p) != JKR_MAGIC || p[4] != JKR_WELCOME) return;
    for (int i = 0; i < MAX_LISTED; i++)
    {
        listed_t* l = &g_listed[i];
        char nm[80];
        if (!l->used || l->reachable || rd32(p + 12) != l->probeSeq || l->addr.sin_addr.s_addr != from->sin_addr.s_addr
            || l->addr.sin_port != from->sin_port)
            continue;
        l->reachable = 1;
        name_ascii(l->game.name, l->game.nameLen, nm, sizeof(nm));
        logf_(0, "listing '%s' at %s\n", nm, addr_str(&l->addr));
    }
}

#ifndef JKRELAY_EMBEDDED
static void gateway_down(const struct sockaddr_in* from, const uint8_t* b, const uint8_t* end);
#else
// Players reached through our master server's gateway (jkrelay_proto.h, "Gateway"):
// their own addresses, which our answers go to through the master server.
#define MAX_VIA 128
static struct { struct sockaddr_in addr; uint64_t lastMs; } g_via[MAX_VIA];

static int via_find(const struct sockaddr_in* a)
{
    for (int i = 0; i < MAX_VIA; i++)
        if (g_via[i].lastMs && g_via[i].addr.sin_addr.s_addr == a->sin_addr.s_addr && g_via[i].addr.sin_port == a->sin_port)
            return i;
    return -1;
}

static void via_note(const struct sockaddr_in* a)
{
    int k = via_find(a), oldest = 0;
    if (k < 0)
    {
        for (int i = 0; i < MAX_VIA; i++)
        {
            if (!g_via[i].lastMs) { k = i; break; }
            if (g_via[i].lastMs < g_via[oldest].lastMs) oldest = i;
        }
        if (k < 0) k = oldest;
        g_via[k].addr = *a;
    }
    g_via[k].lastMs = now_ms();
}
#endif

static void handle_packet(const struct sockaddr_in* from, const uint8_t* p, size_t len)
{
    const uint8_t* end = p + len;
    const uint8_t* b = p + JKR_HDR_SIZE;
    client_t* c;
    uint8_t type;
    uint32_t token, seq;

    if (len < JKR_HDR_SIZE || rd32(p) != JKR_MAGIC) return;
    type = p[4];
    token = rd32(p + 8);
    seq = rd32(p + 12);
#ifdef JKRELAY_EMBEDDED
    // A browser player's packet through our master server's gateway: as from its address.
    if (type == JKR_VIA)
    {
        struct sockaddr_in va;
        if (!g_master.resolved || from->sin_addr.s_addr != g_master.addr.sin_addr.s_addr || from->sin_port != g_master.addr.sin_port)
            return;
        if (end - b < JKR_VIA_SIZE + JKR_HDR_SIZE || b[JKR_VIA_SIZE + 4] == JKR_VIA) return;
        memset(&va, 0, sizeof(va));
        va.sin_family = AF_INET;
        memcpy(&va.sin_addr.s_addr, b, 4);
        memcpy(&va.sin_port, b + 4, 2);
        via_note(&va);
        handle_packet(&va, b + JKR_VIA_SIZE, (size_t)(end - b - JKR_VIA_SIZE));
        return;
    }
#else
    if (type == JKR_VIA)
    {
        gateway_down(from, b, end);   // a listed dedicated server's answer to a browser
        return;
    }
#endif
    if (g_master.resolved && type != JKR_HELLO && from->sin_addr.s_addr == g_master.addr.sin_addr.s_addr
        && from->sin_port == g_master.addr.sin_port)
    {
        master_packet(type, token, seq, b, end);
        return;
    }
#ifdef JKRELAY_EMBEDDED
    if (type == JKR_RCON_CHALLENGE || type == JKR_RCON)
    {
        rcon_packet(from, type, seq, b, end);
        return;
    }
#else
    if (type == JKR_RCON_CHALLENGE || type == JKR_RCON)
    {
        tunnel_forward(from, p, len);   // to the game's host (jkrelay_proto.h, "Rcon through a relay")
        return;
    }
#endif
    c = find_client(from);

    if (type == JKR_HELLO)
    {
        uint8_t out[JKR_HDR_SIZE + 4];
        uint32_t cookie;
        if (end - b < 4 || rd32(b) != JKR_VERSION)
            return;   // incompatible client; stay silent (no amplification)
        if (c && c->lastReplySeq == seq && c->lastReplyLen)
        {
            send_to(c, c->lastReply, c->lastReplyLen);
            return;
        }
        if (c) drop_client(c, "reconnected");
        // No slot yet (2026-10-31): the token is made from the address, so a HELLO
        // from a forged address costs nothing. The slot is taken when the token
        // comes back (hello_take), which only the address's real owner can do.
        cookie = hello_cookie(from, now_ms() / HELLO_COOKIE_MS);
        put_hdr(out, JKR_WELCOME, cookie, seq);
        wr32(out + JKR_HDR_SIZE, cookie);
        net_send(from, out, sizeof(out));
        return;
    }

    // The first packet with a WELCOME's token: now it's a client.
    if (!c && token)
        c = hello_take(from, token);

    // Everything else needs the token handed out in WELCOME (stops spoofed
    // sources from getting replies, so the server can't be used to amplify).
    // A client we don't know (we restarted, or dropped it) is told so, so it
    // can handshake again and put its game back: the answer is no bigger than
    // the packet and carries the token it sent, so only it believes it.
    if (!c || token != c->token)
    {
        // (Never in answer to replies or JKR_UNKNOWN itself: two servers would
        // answer each other forever.)
        if (token && (type < JKR_WELCOME || type == JKR_ACK || type == JKR_DATA || type == JKR_KEEPALIVE))
        {
            uint8_t out[JKR_HDR_SIZE];
            put_hdr(out, JKR_UNKNOWN, token, seq);
            net_send(from, out, sizeof(out));
        }
        return;
    }
    c->lastRx = now_ms();

    switch (type)
    {
        case JKR_KEEPALIVE:
            break;
        case JKR_TUNNEL_REPLY:
            if (c->sess && c->sess->host == c) tunnel_reply(c, b, end);
            break;
        case JKR_ACK:
            if (seq > c->evAcked && seq < c->evNext) c->evAcked = seq;
            break;
        case JKR_DATA:
        {
            session_t* s = c->sess;
            player_t* src;
            uint32_t to;
            uint8_t fwd[JKR_MAX_PACKET];
            if (!s || end - b < 8) return;
            src = find_player(s, rd32(b));
            // Players can't speak for others, except the host forwarding what
            // joiners sent it over direct links.
            if (!src || (src->owner != c && s->host != c)) return;
            to = rd32(b + 4);
            memcpy(fwd, p, len);
            if (to == 0)
            {
                FOR_MEMBERS(s, o, c)
                {
                    wr32(fwd + 8, o->token);
                    send_to(o, fwd, len);
                }
            }
            else
            {
                player_t* dst = find_player(s, to);
                if (!dst || dst->owner == c) return;
                wr32(fwd + 8, dst->owner->token);
                send_to(dst->owner, fwd, len);
            }
            break;
        }
        default:
            if (type < JKR_WELCOME) handle_request(c, type, seq, b, end);
            break;
    }
}

static void tick(void)
{
    uint64_t now = now_ms();
    // Master server: listings not renewed are dropped.
    for (int i = 0; i < MAX_LISTED; i++)
    {
        listed_t* l = &g_listed[i];
        if (!l->used || now - l->lastAnnounce <= JKR_ANNOUNCE_TTL_MS) continue;
        if (l->reachable)
        {
            char nm[80];
            name_ascii(l->game.name, l->game.nameLen, nm, sizeof(nm));
            logf_(0, "listing '%s' at %s dropped (not renewed)\n", nm, addr_str(&l->addr));
        }
        memset(l, 0, sizeof(*l));
    }
    master_tick(now);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        client_t* c = &g_clients[i];
        if (!c->used) continue;
        if (now - c->lastRx > JKR_TIMEOUT_MS) { drop_client(c, "timed out"); continue; }

        // Resend unacknowledged events.
        if (c->evAcked + 1 < c->evNext && now - c->lastResend >= JKR_RETRY_MS)
        {
            uint32_t last = c->evNext - 1;
            if (last > c->evAcked + EVENT_WINDOW) last = c->evAcked + EVENT_WINDOW;
            for (uint32_t s = c->evAcked + 1; s <= last; s++)
            {
                event_t* e = &c->ev[s % MAX_EVENTS];
                send_to(c, e->data, e->len);
            }
            c->lastResend = now;
        }
        if (now - c->lastTx > JKR_KEEPALIVE_MS)
        {
            uint8_t k[JKR_HDR_SIZE];
            put_hdr(k, JKR_KEEPALIVE, c->token, 0);
            send_to(c, k, sizeof(k));
        }
    }
}

// ---------------------------------------------------------------------------
// WebSocket (2026-10-29): browsers can't send UDP, so the server also takes the
// same packets over WebSocket connections (RFC 6455), one JKR packet per binary
// message, on TCP port -w (ws://; for wss://, a TLS reverse proxy on this machine in
// front of it). A connection stands for a client at its IP and TCP port (through a
// proxy on this machine, the X-Forwarded-For IP), so everything above it (sessions,
// bans, logs) treats it as a UDP client, and what the server sends to that address
// goes back over the connection. Browsers can't punch direct links: they don't ask.
// ---------------------------------------------------------------------------

#define MAX_WS          128
#define WS_IN_MAX       4096           // the HTTP request, then frames (packets are <= JKR_MAX_PACKET)
#define WS_OUT_MAX      65536          // waiting to go out; a connection that falls this far behind is closed
#define WS_HANDSHAKE_MS 10000

typedef struct
{
    int       used, open;
    jkrs_sock sock;
    struct sockaddr_in addr;
    uint8_t   in[WS_IN_MAX];
    size_t    inLen;
    uint8_t*  out;
    size_t    outLen;
    uint64_t  since;
    int       gw;                     // a gateway to a dedicated server we list (jkrelay_proto.h, "Gateway")
    struct sockaddr_in gwTo;
    int       closing;                // a plain HTTP answer: closed once it's all sent
} ws_conn_t;

static ws_conn_t g_ws[MAX_WS];

static int ws_would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static void ws_nonblocking(jkrs_sock sk)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(sk, FIONBIO, &nb);
#else
    int fl = fcntl(sk, F_GETFL, 0);
    fcntl(sk, F_SETFL, fl | O_NONBLOCK);
#endif
}

// SHA-1 (FIPS 180-1), only for the handshake's Sec-WebSocket-Accept.
static void ws_sha1(const uint8_t* msg, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t blk[64];
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64)
    {
        uint32_t w[80], a, b, c, d, e;
        for (int i = 0; i < 64; i++)
        {
            size_t at = off + i;
            blk[i] = at < len ? msg[at] : at == len ? 0x80 : 0;
        }
        if (off + 64 == total)
            for (int i = 0; i < 8; i++) blk[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 80; i++)
        {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = x << 1 | x >> 31;
        }
        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k, t;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++)
    {
        out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i];
    }
}

static void ws_base64(const uint8_t* in, size_t len, char* out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static ws_conn_t* ws_find(const struct sockaddr_in* a)
{
    for (int i = 0; i < MAX_WS; i++)
        if (g_ws[i].used && g_ws[i].addr.sin_addr.s_addr == a->sin_addr.s_addr && g_ws[i].addr.sin_port == a->sin_port)
            return &g_ws[i];
    return NULL;
}

static void ws_close(ws_conn_t* w, const char* why)
{
    client_t* c;
    if (!w->used) return;
    jkrs_closesock(w->sock);
    if (w->open)
    {
        logf_(1, "%s: websocket closed (%s)\n", addr_str(&w->addr), why);
        if ((c = find_client(&w->addr)) != NULL)
            drop_client(c, "websocket closed");
    }
    free(w->out);
    memset(w, 0, sizeof(*w));
}

// Raw bytes out, the rest kept for when the socket can take them.
static void ws_write(ws_conn_t* w, const uint8_t* p, size_t n)
{
    if (!w->outLen)
    {
        int sent = (int)send(w->sock, (const char*)p, (int)n, 0);
        if (sent < 0 && !ws_would_block()) { ws_close(w, "send failed"); return; }
        if (sent > 0) { p += sent; n -= (size_t)sent; }
        if (!n) return;
    }
    if (!w->out && !(w->out = (uint8_t*)malloc(WS_OUT_MAX))) { ws_close(w, "out of memory"); return; }
    if (w->outLen + n > WS_OUT_MAX) { ws_close(w, "too far behind"); return; }
    memcpy(w->out + w->outLen, p, n);
    w->outLen += n;
}

static void ws_flush(ws_conn_t* w)
{
    int sent;
    if (!w->outLen) return;
    sent = (int)send(w->sock, (const char*)w->out, (int)w->outLen, 0);
    if (sent < 0 && !ws_would_block()) { ws_close(w, "send failed"); return; }
    if (sent > 0)
    {
        memmove(w->out, w->out + sent, w->outLen - (size_t)sent);
        w->outLen -= (size_t)sent;
    }
    if (!w->outLen && w->closing) ws_close(w, "answered");
}

static void ws_frame(ws_conn_t* w, uint8_t opcode, const void* p, size_t n)
{
    uint8_t hdr[4];
    size_t hn = 2;
    hdr[0] = (uint8_t)(0x80 | opcode);
    if (n < 126) hdr[1] = (uint8_t)n;
    else { hdr[1] = 126; hdr[2] = (uint8_t)(n >> 8); hdr[3] = (uint8_t)n; hn = 4; }
    ws_write(w, hdr, hn);
    if (w->used && n) ws_write(w, (const uint8_t*)p, n);
}

#ifdef JKRELAY_EMBEDDED
// A dedicated server hosting on a relay server has no socket of its own: what its
// rcon answers goes back through the relay (jkrelay_listen.c, tunnel mode).
static void (*g_pfnTunnelSend)(const struct sockaddr_in* to, const void* p, size_t len) = NULL;
#endif

static void net_send(const struct sockaddr_in* to, const void* p, size_t len)
{
    ws_conn_t* w;
#ifdef JKRELAY_EMBEDDED
    if (g_sock == JKRS_BADSOCK)
    {
        if (g_pfnTunnelSend) g_pfnTunnelSend(to, p, len);
        return;
    }
#endif
#ifdef JKRELAY_EMBEDDED
    if (g_master.resolved && via_find(to) >= 0)
    {
        // A player reached through our master server's gateway: back through it.
        uint8_t out[JKR_HDR_SIZE + JKR_VIA_SIZE + JKR_MAX_PACKET + 64];
        if (len > JKR_MAX_PACKET + 64) return;
        put_hdr(out, JKR_VIA, 0, 0);
        memcpy(out + JKR_HDR_SIZE, &to->sin_addr.s_addr, 4);
        memcpy(out + JKR_HDR_SIZE + 4, &to->sin_port, 2);
        memcpy(out + JKR_HDR_SIZE + JKR_VIA_SIZE, p, len);
        sendto(g_sock, (const char*)out, (int)(JKR_HDR_SIZE + JKR_VIA_SIZE + len), 0, (const struct sockaddr*)&g_master.addr,
               sizeof(g_master.addr));
        return;
    }
#endif
    w = g_wsListen != JKRS_BADSOCK ? ws_find(to) : NULL;
    if (w)
    {
        if (w->open) ws_frame(w, 0x2, p, len);
        return;
    }
    sendto(g_sock, (const char*)p, (int)len, 0, (const struct sockaddr*)to, sizeof(*to));
}

// Gateway (jkrelay_proto.h): a browser's packet on to the dedicated server, with its address.
static void gateway_up(ws_conn_t* w, const uint8_t* p, size_t len)
{
    uint8_t out[JKR_HDR_SIZE + JKR_VIA_SIZE + JKR_MAX_PACKET + 64];
    if (len < JKR_HDR_SIZE || len > JKR_MAX_PACKET + 64 || rd32(p) != JKR_MAGIC) return;
    put_hdr(out, JKR_VIA, 0, 0);
    memcpy(out + JKR_HDR_SIZE, &w->addr.sin_addr.s_addr, 4);
    memcpy(out + JKR_HDR_SIZE + 4, &w->addr.sin_port, 2);
    memcpy(out + JKR_HDR_SIZE + JKR_VIA_SIZE, p, len);
    sendto(g_sock, (const char*)out, (int)(JKR_HDR_SIZE + JKR_VIA_SIZE + len), 0, (const struct sockaddr*)&w->gwTo, sizeof(w->gwTo));
}

// ...and its answer back to that browser.
static void gateway_down(const struct sockaddr_in* from, const uint8_t* b, const uint8_t* end)
{
    struct sockaddr_in to;
    if (end - b < JKR_VIA_SIZE + JKR_HDR_SIZE || rd32(b + JKR_VIA_SIZE) != JKR_MAGIC) return;
    memcpy(&to.sin_addr.s_addr, b, 4);
    memcpy(&to.sin_port, b + 4, 2);
    for (int i = 0; i < MAX_WS; i++)
    {
        ws_conn_t* w = &g_ws[i];
        if (!w->used || !w->open || !w->gw || w->gwTo.sin_addr.s_addr != from->sin_addr.s_addr || w->gwTo.sin_port != from->sin_port
            || w->addr.sin_addr.s_addr != to.sin_addr.s_addr || w->addr.sin_port != to.sin_port) continue;
        ws_frame(w, 0x2, b + JKR_VIA_SIZE, (size_t)(end - b - JKR_VIA_SIZE));
        return;
    }
}

// A header's value in the HTTP request ("Name: value"), case-insensitive name. 0 if absent.
static int ws_header(const char* req, const char* name, char* out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char* p = req; (p = strstr(p, "\r\n")) != NULL; )
    {
        p += 2;
        int match = 1;
        for (size_t i = 0; i < nl && match; i++)
            match = p[i] && (p[i] | 0x20) == (name[i] | 0x20);
        if (!match || p[nl] != ':') continue;
        p += nl + 1;
        while (*p == ' ' || *p == '\t') p++;
        size_t n = 0;
        while (p[n] && p[n] != '\r' && n + 1 < cap) { out[n] = p[n]; n++; }
        out[n] = 0;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The game list as JSON (2026-10-31): GET <any path ending in /list> on the
// WebSocket port, for web pages and sites that list the games (e.g. a lobby
// page showing them next to its own). Read-only, the same games LIST gives a
// Join screen: hosted here and the dedicated servers listed here that answered.
// ---------------------------------------------------------------------------

#define JSON_LIST_MAX (WS_OUT_MAX - 1024)

typedef struct { char* p; size_t n, cap; int full; } jbuf_t;

static void jb_put(jbuf_t* b, const char* fmt, ...)
{
    va_list ap;
    int k;
    if (b->full) return;
    va_start(ap, fmt);
    k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= b->cap - b->n) { b->full = 1; return; }
    b->n += (size_t)k;
}

// A JSON string from UTF-16 (the names as JK sends them).
static void jb_str16(jbuf_t* b, const uint16_t* s, int n)
{
    jb_put(b, "\"");
    for (int i = 0; i < n && !b->full; i++)
    {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c == '"' || c == '\\') jb_put(b, "\\%c", (char)c);
        else if (c < 0x20) jb_put(b, "\\u%04x", (unsigned)c);
        else if (c < 0x80) jb_put(b, "%c", (char)c);
        else if (c < 0x800) jb_put(b, "%c%c", (char)(0xC0 | c >> 6), (char)(0x80 | (c & 0x3F)));
        else if (c < 0x10000) jb_put(b, "%c%c%c", (char)(0xE0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F)));
        else jb_put(b, "%c%c%c%c", (char)(0xF0 | c >> 18), (char)(0x80 | (c >> 12 & 0x3F)), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F)));
    }
    jb_put(b, "\"");
}

static void jb_guid(jbuf_t* b, const char* key, const uint8_t g[16])
{
    jb_put(b, "\"%s\":\"{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}\"", key,
           g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int jb_nib(uint8_t c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The host's mod from its tail (jkrelay_proto.h, "Mods": mods.js's hex "JKMD"
// tail): "mod" (its name, or null) and "remasterPack"; "tail" as it came.
static void jb_tail(jbuf_t* b, const session_t* s)
{
    uint8_t raw[160];
    size_t n = s->tailLen / 2, at = 0, nl = 0;
    int flags = 0, ok = s->tailLen && !(s->tailLen % 2) && n <= sizeof(raw);
    for (size_t i = 0; ok && i < n; i++)
    {
        int hi = jb_nib(s->tail[2 * i]), lo = jb_nib(s->tail[2 * i + 1]);
        if (hi < 0 || lo < 0) ok = 0;
        else raw[i] = (uint8_t)(hi << 4 | lo);
    }
    if (ok && n >= 7 && !memcmp(raw, "JKMD", 4) && raw[4] == 2)
    {
        flags = raw[5];
        nl = raw[6];
        at = 7 + ((flags & 1) ? 32 : 0) + ((flags & 2) ? 16 : 0);
        if (at + nl != n) ok = 0;
    }
    else if (ok && n >= 6 && !memcmp(raw, "JKMD", 4) && raw[4] == 1 && n == 38u + raw[5])
    {
        flags = 1;
        nl = raw[5];
        at = 38;
    }
    else
        ok = 0;
    jb_put(b, ",\"mod\":");
    if (ok && (flags & 1) && nl)
    {
        uint16_t name16[64];
        for (size_t i = 0; i < nl && i < 64; i++) name16[i] = raw[at + i] >= 32 && raw[at + i] < 127 ? raw[at + i] : '?';
        jb_str16(b, name16, (int)(nl < 64 ? nl : 64));
    }
    else
        jb_put(b, "null");
    jb_put(b, ",\"remasterPack\":%s,\"tail\":\"", ok && (flags & 2) ? "true" : "false");
    for (int i = 0; i < s->tailLen; i++)
        if (jb_nib(s->tail[i]) >= 0) jb_put(b, "%c", (char)s->tail[i]);
    jb_put(b, "\"");
}

// One game. Its name is JK's "<game>:<episode GOB>:<map>".
static void jb_game(jbuf_t* b, const session_t* s, uint32_t players, const listed_t* l)
{
    int colon2 = -1, colon1 = -1;
    for (int i = s->nameLen - 1; i >= 0 && colon1 < 0; i--)
        if (s->name[i] == ':') { if (colon2 < 0) colon2 = i; else colon1 = i; }
    jb_put(b, "{");
    jb_guid(b, "guid", s->guid);
    jb_put(b, ",");
    jb_guid(b, "app", s->appGuid);
    jb_put(b, ",\"name\":");
    if (colon1 >= 0)
    {
        jb_str16(b, s->name, colon1);
        jb_put(b, ",\"episode\":");
        jb_str16(b, s->name + colon1 + 1, colon2 - colon1 - 1);
        jb_put(b, ",\"map\":");
        jb_str16(b, s->name + colon2 + 1, s->nameLen - colon2 - 1);
    }
    else
    {
        jb_str16(b, s->name, s->nameLen);
        jb_put(b, ",\"episode\":null,\"map\":null");
    }
    jb_put(b, ",\"players\":%u,\"maxPlayers\":%u,\"password\":%s,\"maxRank\":%u,\"tickRateMs\":%u,\"modeFlags\":%u",
           players, s->maxPlayers, ((s->flags & 0x400) || s->passwordLen) ? "true" : "false",
           s->user[2] >> 16, s->user[3], s->user[1]);
    jb_put(b, ",\"hitRules\":\"%s\",\"sharesFiles\":%s,\"openjkdf2Layouts\":%s",
           (s->flags & 0x10000000u) ? "hwys" : "classic", (s->flags & 0x08000000u) ? "true" : "false",
           (s->flags & 0x20000000u) ? "true" : "false");
    // A dedicated server running its own server (listed here). "players" counts its
    // own slot as JK does, so do "maxPlayers": 31 can join a 32. Hosted here: the
    // listing can't tell whether its host is a dedicated server.
    if (l)
    {
        uint32_t ip = ntohl(l->addr.sin_addr.s_addr);
        if (is_local_ip(ip)) jb_put(b, ",\"dedicatedServer\":true,\"address\":null,\"nextToRelay\":true");
        else jb_put(b, ",\"dedicatedServer\":true,\"address\":\"%s\"", addr_str(&l->addr));
    }
    else
        jb_put(b, ",\"dedicatedServer\":null,\"address\":null");
    jb_tail(b, s);
    jb_put(b, "}");
}

// Answers a plain GET .../list. 1 if it did (the connection then closes when sent).
static int ws_json_list(ws_conn_t* w, const char* req)
{
    char path[256], head[256];
    char* q;
    jbuf_t b;
    int first = 1, count = 0, total = 0;
    if (sscanf(req, "GET %255s HTTP/", path) != 1) return 0;
    if ((q = strchr(path, '?'))) *q = 0;
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
    if (!(pl >= 5 && !strcmp(path + pl - 5, "/list")) && !(pl >= 10 && !strcmp(path + pl - 10, "/list.json")))
        return 0;
    b.cap = JSON_LIST_MAX;
    b.n = 0;
    b.full = 0;
    if (!(b.p = (char*)malloc(b.cap))) return 0;
    for (int i = 0; i < MAX_SESSIONS; i++) if (g_sessions[i].used) total++;
    for (int i = 0; !g_embedded && i < MAX_LISTED; i++) if (g_listed[i].used && g_listed[i].reachable) total++;
    jb_put(&b, "{\"version\":1,\"games\":[");
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        size_t mark = b.n;
        if (!g_sessions[i].used) continue;
        jb_put(&b, first ? "" : ",");
        jb_game(&b, &g_sessions[i], (uint32_t)count_players(&g_sessions[i]), NULL);
        if (b.full) { b.n = mark; break; }
        first = 0; count++;
    }
    for (int i = 0; !g_embedded && !b.full && i < MAX_LISTED; i++)
    {
        size_t mark = b.n;
        listed_t* l = &g_listed[i];
        if (!l->used || !l->reachable) continue;
        memcpy(l->game.appGuid, l->appGuid, 16);
        jb_put(&b, first ? "" : ",");
        jb_game(&b, &l->game, l->curPlayers, l);
        if (b.full) { b.n = mark; break; }
        first = 0; count++;
    }
    b.full = 0;
    jb_put(&b, "],\"total\":%d,\"shown\":%d}\n", total, count);
    snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %u\r\n"
             "Access-Control-Allow-Origin: *\r\nCache-Control: max-age=5\r\nConnection: close\r\n\r\n", (unsigned)b.n);
    w->closing = 1;
    ws_write(w, (const uint8_t*)head, strlen(head));
    if (w->used) ws_write(w, (const uint8_t*)b.p, b.n);
    free(b.p);
    logf_(1, "%s: game list (json, %d games)\n", addr_str(&w->addr), count);
    if (w->used && !w->outLen) ws_close(w, "answered");
    return 1;
}

static void ws_handshake(ws_conn_t* w)
{
    char req[WS_IN_MAX + 1], key[128], acc[64], resp[256], fwd[64];
    uint8_t digest[20];
    char* end;
    memcpy(req, w->in, w->inLen);
    req[w->inLen] = 0;
    if (!(end = strstr(req, "\r\n\r\n")))
    {
        if (w->inLen >= WS_IN_MAX) ws_close(w, "request too big");
        return;
    }
    end[2] = 0;
    if (!ws_header(req, "Upgrade", acc, sizeof(acc)) && ws_json_list(w, req))
        return;
    if (!ws_header(req, "Sec-WebSocket-Key", key, sizeof(key)) || !ws_header(req, "Upgrade", acc, sizeof(acc))
        || (acc[0] | 0x20) != 'w')
    {
        static const char nope[] = "HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain\r\nContent-Length: 50\r\n"
                                   "Connection: close\r\n\r\nImperial Network Protocol relay: WebSocket only.\r\n";
        send(w->sock, nope, (int)strlen(nope), 0);
        ws_close(w, "not a websocket");
        return;
    }
    // A gateway (jkrelay_proto.h): "?to=a.b.c.d:port" on the request line, a dedicated
    // server listed here that answered.
#ifndef JKRELAY_EMBEDDED
    {
        char* line = strstr(req, "\r\n");
        char* to = strstr(req, "to=");
        if (to && line && to < line)
        {
            unsigned a, b, c, d, port;
            int ok = 0;
            if (sscanf(to + 3, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5 && a < 256 && b < 256 && c < 256 && d < 256
                && port && port < 65536)
            {
                uint32_t ip = htonl(a << 24 | b << 16 | c << 8 | d);
                for (int i = 0; i < MAX_LISTED && !ok; i++)
                    ok = g_listed[i].used && g_listed[i].reachable && g_listed[i].addr.sin_addr.s_addr == ip
                         && g_listed[i].addr.sin_port == htons((uint16_t)port);
                if (ok)
                {
                    memset(&w->gwTo, 0, sizeof(w->gwTo));
                    w->gwTo.sin_family = AF_INET;
                    w->gwTo.sin_addr.s_addr = ip;
                    w->gwTo.sin_port = htons((uint16_t)port);
                    w->gw = 1;
                }
            }
            if (!ok)
            {
                static const char nope[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 41\r\n"
                                           "Connection: close\r\n\r\nNo such server listed here, or it's down\n";
                send(w->sock, nope, (int)strlen(nope), 0);
                ws_close(w, "gateway to a server not listed here");
                return;
            }
        }
    }
#endif
    // Through a TLS proxy on this machine: the player's own address.
    if (w->addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ws_header(req, "X-Forwarded-For", fwd, sizeof(fwd)))
    {
        struct in_addr a;
        char* comma = strchr(fwd, ',');
        if (comma) *comma = 0;
        if (inet_pton(AF_INET, fwd, &a) == 1) w->addr.sin_addr = a;
    }
    {
        char tmp[200];
        snprintf(tmp, sizeof(tmp), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        ws_sha1((const uint8_t*)tmp, strlen(tmp), digest);
        ws_base64(digest, 20, acc);
    }
    snprintf(resp, sizeof(resp), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
    w->open = 1;
    ws_write(w, (const uint8_t*)resp, strlen(resp));
    {
        size_t used = (size_t)(end + 4 - req);
        memmove(w->in, w->in + used, w->inLen - used);
        w->inLen -= used;
    }
    if (w->gw)
    {
        char to[32];
        snprintf(to, sizeof(to), "%s", addr_str(&w->gwTo));
        logf_(1, "%s: websocket open, gateway to %s\n", addr_str(&w->addr), to);
    }
    else
        logf_(1, "%s: websocket open\n", addr_str(&w->addr));
}

// Frames in: binary ones are packets.
static void ws_frames(ws_conn_t* w)
{
    while (w->used && w->open && w->inLen >= 2)
    {
        uint8_t* f = w->in;
        size_t len = f[1] & 0x7F, hn = 2;
        uint8_t opcode = f[0] & 0x0F;
        if (!(f[0] & 0x80) || !(f[1] & 0x80)) { ws_close(w, "fragmented or unmasked frame"); return; }
        if (len == 126)
        {
            if (w->inLen < 4) return;
            len = (size_t)f[2] << 8 | f[3];
            hn = 4;
        }
        else if (len == 127) { ws_close(w, "frame too big"); return; }
        if (len > JKR_MAX_PACKET + 64) { ws_close(w, "frame too big"); return; }
        if (w->inLen < hn + 4 + len) return;
        uint8_t* mask = f + hn;
        uint8_t* pay = f + hn + 4;
        for (size_t i = 0; i < len; i++) pay[i] ^= mask[i & 3];
        if (opcode == 0x2 && w->gw)
            gateway_up(w, pay, len);
        else if (opcode == 0x2)
            handle_packet(&w->addr, pay, len);
        else if (opcode == 0x8) { ws_frame(w, 0x8, pay, len < 2 ? len : 2); ws_close(w, "closed by the client"); return; }
        else if (opcode == 0x9) ws_frame(w, 0xA, pay, len);
        if (!w->used) return;
        memmove(w->in, w->in + hn + 4 + len, w->inLen - (hn + 4 + len));
        w->inLen -= hn + 4 + len;
    }
}

static void ws_read(ws_conn_t* w)
{
    for (;;)
    {
        int n;
        if (w->inLen >= WS_IN_MAX) { ws_close(w, "too much at once"); return; }
        n = (int)recv(w->sock, (char*)w->in + w->inLen, (int)(WS_IN_MAX - w->inLen), 0);
        if (n == 0) { ws_close(w, "disconnected"); return; }
        if (n < 0) { if (!ws_would_block()) ws_close(w, "receive failed"); return; }
        w->inLen += (size_t)n;
        if (!w->open) ws_handshake(w);
        if (!w->used) return;
        ws_frames(w);
        if (!w->used) return;
    }
}

static void ws_accept(void)
{
    for (;;)
    {
        struct sockaddr_in from;
        jkrs_socklen fl = sizeof(from);
        jkrs_sock sk = accept(g_wsListen, (struct sockaddr*)&from, &fl);
        ws_conn_t* w = NULL;
        if (sk == JKRS_BADSOCK) return;
        for (int i = 0; i < MAX_WS && !w; i++) if (!g_ws[i].used) w = &g_ws[i];
        if (!w) { jkrs_closesock(sk); continue; }
        ws_nonblocking(sk);
        memset(w, 0, sizeof(*w));
        w->used = 1;
        w->sock = sk;
        w->addr = from;
        w->since = now_ms();
    }
}

// TCP port for WebSocket connections. 0 on failure (logged).
static int ws_listen(int port)
{
    struct sockaddr_in sa;
    int one = 1;
    g_wsListen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_wsListen == JKRS_BADSOCK) { logf_(0, "websocket socket: %s\n", strerror(errno)); return 0; }
    setsockopt(g_wsListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(g_wsListen, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(g_wsListen, 16) < 0)
    {
        logf_(0, "can't use TCP port %d for websockets\n", port);
        jkrs_closesock(g_wsListen);
        g_wsListen = JKRS_BADSOCK;
        return 0;
    }
    ws_nonblocking(g_wsListen);
    logf_(0, "websockets on TCP %d\n", port);
    return 1;
}

static void ws_close_all(void)
{
    for (int i = 0; i < MAX_WS; i++)
        if (g_ws[i].used) ws_close(&g_ws[i], "server stopping");
    if (g_wsListen != JKRS_BADSOCK) jkrs_closesock(g_wsListen);
    g_wsListen = JKRS_BADSOCK;
}

static int open_log(void);

// Binds the server's socket. 0 on failure (logged).
static int server_open(int port)
{
    struct sockaddr_in sa;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logf_(0, "can't start Winsock\n"); return 0; }
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == JKRS_BADSOCK) { logf_(0, "socket: %s\n", strerror(errno)); return 0; }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(g_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0)
    {
#ifdef _WIN32
        logf_(0, "can't use UDP port %d (error %d; another program on it?)\n", port, WSAGetLastError());
#else
        logf_(0, "can't use UDP port %d: %s\n", port, strerror(errno));
#endif
        jkrs_closesock(g_sock);
        g_sock = JKRS_BADSOCK;
        return 0;
    }
    if (!g_embedded)
    {
        g_probeSock = socket(AF_INET, SOCK_DGRAM, 0);   // any port
        if (g_probeSock != JKRS_BADSOCK)
        {
            sa.sin_port = 0;
            bind(g_probeSock, (struct sockaddr*)&sa, sizeof(sa));
        }
    }
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb); }
    if (g_probeSock != JKRS_BADSOCK) { u_long nb = 1; ioctlsocket(g_probeSock, FIONBIO, &nb); }
#endif
    logf_(0, "%s on UDP %d\n", g_embedded ? "players connect" : "jkrelay listening", port);
    return 1;
}

// Waits up to waitMs for packets, handles them, then resends and times out.
static void server_service(int waitMs)
{
    uint8_t buf[2048];
#ifdef _WIN32
    WSAPOLLFD pfd[3 + MAX_WS] = { { g_sock, POLLRDNORM, 0 }, { g_probeSock, POLLRDNORM, 0 } };
    const short evIn = POLLRDNORM, evOut = POLLWRNORM;
#else
    struct pollfd pfd[3 + MAX_WS] = { { g_sock, POLLIN, 0 }, { g_probeSock, POLLIN, 0 } };
    const short evIn = POLLIN, evOut = POLLOUT;
#endif
    int nfd = g_probeSock != JKRS_BADSOCK ? 2 : 1;
    int wsAt = 0, wsIdx[MAX_WS];
    // Added: websocket connections (see "WebSocket")
    if (g_wsListen != JKRS_BADSOCK)
    {
        pfd[nfd].fd = g_wsListen; pfd[nfd].events = evIn; pfd[nfd].revents = 0;
        wsAt = ++nfd;
        for (int i = 0; i < MAX_WS; i++)
        {
            if (!g_ws[i].used) continue;
            pfd[nfd].fd = g_ws[i].sock;
            pfd[nfd].events = (short)(evIn | (g_ws[i].outLen ? evOut : 0));
            pfd[nfd].revents = 0;
            wsIdx[nfd - wsAt] = i;
            nfd++;
        }
    }
    if (g_reopenLog)
    {
        g_reopenLog = 0;
        open_log();
        logf_(0, "log file reopened\n");
    }
    int bGot = jkrs_poll(pfd, (unsigned)nfd, waitMs) > 0;
    SERVER_LOCK();   // the game's thread asks things too (jkrelay_listen.c)
    if (bGot && wsAt)
    {
        uint64_t now = now_ms();
        if (pfd[wsAt - 1].revents) ws_accept();
        for (int k = wsAt; k < nfd; k++)
        {
            ws_conn_t* w = &g_ws[wsIdx[k - wsAt]];
            if (!w->used || w->sock != pfd[k].fd) continue;
            if (pfd[k].revents & evOut) ws_flush(w);
            if (w->used && (pfd[k].revents & ~evOut)) ws_read(w);
            if (w->used && !w->open && now - w->since > WS_HANDSHAKE_MS) ws_close(w, "no handshake");
        }
    }
    if (bGot)
    {
        for (;;)
        {
            struct sockaddr_in from;
            jkrs_socklen fl = sizeof(from);
#ifdef _WIN32
            int n = nfd > 1 ? recvfrom(g_probeSock, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl) : -1;
#else
            ssize_t n = nfd > 1 ? recvfrom(g_probeSock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &fl) : -1;
#endif
            if (n <= 0) break;
            handle_probe_reply(&from, buf, (size_t)n);
        }
        for (;;)
        {
            struct sockaddr_in from;
            jkrs_socklen fl = sizeof(from);
#ifdef _WIN32
            int n = recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
#else
            ssize_t n = recvfrom(g_sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
#endif
            if (n <= 0) break;
            handle_packet(&from, buf, (size_t)n);
        }
    }
    tick();
    SERVER_UNLOCK();
}

static void server_close(void)
{
    if (g_sock == JKRS_BADSOCK) return;
    jkrs_closesock(g_sock);
    g_sock = JKRS_BADSOCK;
    if (g_probeSock != JKRS_BADSOCK) jkrs_closesock(g_probeSock);
    g_probeSock = JKRS_BADSOCK;
    ws_close_all();
    memset(g_clients, 0, sizeof(g_clients));
    memset(g_sessions, 0, sizeof(g_sessions));
    memset(g_listed, 0, sizeof(g_listed));
#ifdef _WIN32
    WSACleanup();
#endif
}

#ifndef JKRELAY_EMBEDDED
static void on_signal(int s) { (void)s; g_stop = 1; }
#ifdef SIGHUP
static void on_hup(int s) { (void)s; g_reopenLog = 1; }
#endif
#endif

// Opens (or reopens, after logrotate moved it) the log file.
static int open_log(void)
{
    FILE* f;
    if (!g_logPath) return 1;
    f = fopen(g_logPath, "a");
    if (!f)
    {
        fprintf(stderr, "jkrelay: can't open log file %s: %s\n", g_logPath, strerror(errno));
        return 0;
    }
    if (g_logFile) fclose(g_logFile);
    g_logFile = f;
    return 1;
}

#ifndef JKRELAY_EMBEDDED
int main(int argc, char** argv)
{
    int port = JKR_DEFAULT_PORT;
    int wsPort = 0;   // -w: also take players over WebSocket on this TCP port

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) g_logPath = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wsPort = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [-p port] [-w websocket-port] [-v] [-l logfile]\n", argv[0]); return 2; }
    }

    if (!open_log()) return 1;
    if (!server_open(port)) return 1;
    if (wsPort && !ws_listen(wsPort)) return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGHUP
    signal(SIGHUP, on_hup);
#endif

    while (!g_stop)
        server_service(50);
    logf_(0, "jkrelay stopping\n");
    server_close();
    if (g_logFile) fclose(g_logFile);
    return 0;
}
#endif
