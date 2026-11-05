// A dedicated server's own Imperial Network Protocol server: the relay server
// (tools/jkrelay/jkrelay_server.c) built into the game, on its own thread.
// Players connect to the dedicated server's address directly (console
// "connect host[:port]", or the join screen's address box) instead of a relay
// somewhere else; the dedicated server hosts on it over 127.0.0.1 like on any
// relay. Only this machine can host on it, and it introduces nobody for
// direct links: everyone reaches it already. With a master server set, it
// lists itself there (jkrelay_proto.h, "Master server"). 2026-10-27.

#if defined(PLATFORM_DPLAY)

#define JKRELAY_EMBEDDED
#include "../../../../tools/jkrelay/jkrelay_server.c"

#include "dplay_shim.h"

#ifdef _WIN32
static HANDLE jkl_thread;
static CRITICAL_SECTION jkl_lock;
static void jkl_Lock(void) { EnterCriticalSection(&jkl_lock); }
static void jkl_Unlock(void) { LeaveCriticalSection(&jkl_lock); }
#else
#include <pthread.h>
static pthread_t jkl_thread;
static pthread_mutex_t jkl_lock = PTHREAD_MUTEX_INITIALIZER;
static void jkl_Lock(void) { pthread_mutex_lock(&jkl_lock); }
static void jkl_Unlock(void) { pthread_mutex_unlock(&jkl_lock); }
#endif
static volatile int jkl_bRunning = 0;
static int jkl_bTunnel = 0;   // admin only, through a relay server (jkrelay_ListenTunnelStart)
static volatile int jkl_bStop = 0;

#ifdef _WIN32
static DWORD WINAPI jkl_Thread(LPVOID arg)
#else
static void* jkl_Thread(void* arg)
#endif
{
    (void)arg;
    while (!jkl_bStop)
        server_service(20);
    server_close();
    return 0;
}

// 'master': "host[:port]" of a relay server to list this server on, or NULL.
// 'wsPort': a TCP port for browsers' WebSockets as well (0 = none).
int jkrelay_Listen(int port, int wsPort, const char* master)
{
    if (jkl_bRunning) return 1;
#ifdef _WIN32
    {
        static int bInit = 0;
        if (!bInit) InitializeCriticalSection(&jkl_lock);
        bInit = 1;
    }
#endif
    g_embedded = 1;
    g_verbose = getenv("OPENJKDF2_DPLAY_LOG") != NULL;
    if (!server_open(port)) return 0;
    if (wsPort && !ws_listen(wsPort)) { server_close(); return 0; }
    server_set_master(master, port);
    jkl_bStop = 0;
#ifdef _WIN32
    jkl_thread = CreateThread(NULL, 0, jkl_Thread, NULL, 0, NULL);
    if (!jkl_thread)
#else
    if (pthread_create(&jkl_thread, NULL, jkl_Thread, NULL) != 0)
#endif
    {
        server_close();
        return 0;
    }
    jkl_bRunning = 1;
    return 1;
}

void jkrelay_StopListening(void)
{
    if (!jkl_bRunning) return;
    jkl_bStop = 1;
#ifdef _WIN32
    WaitForSingleObject(jkl_thread, INFINITE);
    CloseHandle(jkl_thread);
#else
    pthread_join(jkl_thread, NULL);
#endif
    jkl_bRunning = 0;
}

int jkrelay_Listening(void)
{
    return jkl_bRunning;
}

// ---------------------------------------------------------------------------
// For the server's admin commands (Main/jkServer.c), from the game's thread
// ---------------------------------------------------------------------------

// The address the game player 'id' plays from ("ip:port"), 0 if unknown.
int jkrelay_ListenPlayerAddr(uint32_t id, char* out, size_t cap)
{
    const char* a;
    if (!jkl_bRunning || !cap) return 0;
    jkl_Lock();
    a = player_addr(id);
    if (a) snprintf(out, cap, "%s", a);
    jkl_Unlock();
    return a != NULL;
}

// Takes the game player 'id' out of the game (its machine hears the game is
// over), and with banMinutes >= 0 bans its IP address (0 = for good). Its IP
// ("a.b.c.d") goes to ipOut. 0 if there's no such player.
int jkrelay_ListenKick(uint32_t id, int banMinutes, char* ipOut, size_t cap)
{
    uint32_t ip;
    if (!jkl_bRunning) return 0;
    jkl_Lock();
    ip = kick_player(id, banMinutes);
    if (ip && banMinutes >= 0) ban_add(ip, banMinutes);
    jkl_Unlock();
    if (ip && cap)
    {
        struct in_addr a;
        a.s_addr = ip;
        snprintf(ipOut, cap, "%s", inet_ntoa(a));
    }
    return ip != 0;
}

// "a.b.c.d"; minutes 0 = for good. 0 if it isn't an IPv4 address.
int jkrelay_ListenBan(const char* ip, int minutes)
{
    struct in_addr a;
    if (!inet_pton(AF_INET, ip, &a) || !a.s_addr) return 0;
    jkl_Lock();
    ban_add(a.s_addr, minutes);
    jkl_Unlock();
    return 1;
}

int jkrelay_ListenUnban(const char* ip)
{
    struct in_addr a;
    int n;
    if (!inet_pton(AF_INET, ip, &a)) return 0;
    jkl_Lock();
    n = ban_remove(a.s_addr);
    jkl_Unlock();
    return n;
}

// The bans, one "ip" or "ip (n min left)" a line. bForGoodOnly: just the IPs of
// the bans for good, comma separated (what server.json keeps).
void jkrelay_ListenListBans(char* out, size_t cap, int bForGoodOnly)
{
    size_t n = 0;
    uint64_t now = now_ms();
    if (!cap) return;
    out[0] = 0;
    jkl_Lock();
    for (int i = 0; i < MAX_BANS && n + 40 < cap; i++)
    {
        struct in_addr a;
        if (!g_bans[i].ip || (g_bans[i].until && now > g_bans[i].until)) continue;
        if (bForGoodOnly && g_bans[i].until) continue;
        a.s_addr = g_bans[i].ip;
        if (bForGoodOnly)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? "," : "", inet_ntoa(a));
        else if (g_bans[i].until)
            n += (size_t)snprintf(out + n, cap - n, "%s (%u min left)\n", inet_ntoa(a), (unsigned)((g_bans[i].until - now) / 60000 + 1));
        else
            n += (size_t)snprintf(out + n, cap - n, "%s\n", inet_ntoa(a));
    }
    jkl_Unlock();
}

// sv_rconMaxTries, sv_rconBlockMinutes, sv_floodProtect (Main/jkServer.c).
void jkrelay_ListenSetRconLimits(int maxTries, int blockMinutes, int bFloodProtect)
{
    jkl_Lock();
    g_rconMaxBad = maxTries < 1 ? 1 : maxTries;
    g_rconBlockMs = (uint64_t)(blockMinutes < 1 ? 1 : blockMinutes) * 60000;
    g_rconFloodMs = bFloodProtect ? 1000 : 0;
    jkl_Unlock();
}

// "" turns rcon off.
void jkrelay_ListenSetRcon(const char* password)
{
    jkl_Lock();
    snprintf(g_rconPassword, sizeof(g_rconPassword), "%s", password ? password : "");
    jkl_Unlock();
}

// An rcon command waiting to be run (then jkrelay_ListenRconDone with its output).
int jkrelay_ListenTakeRcon(char* cmd, size_t cap)
{
    int bGot = 0;
    if (!jkl_bRunning && !jkl_bTunnel) return 0;
    jkl_Lock();
    if (g_rconJob.state == 1)
    {
        snprintf(cmd, cap, "%s", g_rconJob.cmd);
        g_rconJob.state = 2;
        bGot = 1;
    }
    jkl_Unlock();
    return bGot;
}

void jkrelay_ListenRconDone(const char* output)
{
    jkl_Lock();
    if (g_rconJob.state == 2)
    {
        rcon_send(&g_rconJob.addr, g_rconJob.seq, output && output[0] ? output : "(no output)");
        g_rconJob.state = 0;
    }
    jkl_Unlock();
}

// ---------------------------------------------------------------------------
// Tunnel mode (2026-10-29): a dedicated server hosting on a relay server has no
// port of its own; its rcon comes through the relay (jkrelay_proto.h, "Rcon
// through a relay") and is checked here as on its own server: the same password,
// flood protection and blocking. Its bans are kept here too, which the relay
// server enforces for it (JKR_SETBANS).
// ---------------------------------------------------------------------------

static void (*jkl_pfnTunnelOut)(uint32_t ip, uint16_t port, const void* p, size_t n) = NULL;

static void jkl_TunnelSend(const struct sockaddr_in* to, const void* p, size_t len)
{
    if (jkl_pfnTunnelOut) jkl_pfnTunnelOut(to->sin_addr.s_addr, to->sin_port, p, len);
}

// send: how an answer goes back (ip and port in network order, as they came).
void jkrelay_ListenTunnelStart(void (*send)(uint32_t ip, uint16_t port, const void* p, size_t n))
{
    if (jkl_bRunning) return;
    jkl_Lock();
    g_embedded = 1;
    jkl_pfnTunnelOut = send;
    g_pfnTunnelSend = jkl_TunnelSend;
    jkl_bTunnel = 1;
    jkl_Unlock();
}

int jkrelay_ListenTunnelActive(void)
{
    return jkl_bTunnel && !jkl_bRunning;
}

// An rcon packet that came through the relay server from ip:port (network order).
void jkrelay_ListenTunnelPacket(uint32_t ip, uint16_t port, const uint8_t* p, size_t len)
{
    struct sockaddr_in from;
    if (!jkl_bTunnel || len < JKR_HDR_SIZE || rd32(p) != JKR_MAGIC) return;
    if (p[4] != JKR_RCON_CHALLENGE && p[4] != JKR_RCON) return;
    memset(&from, 0, sizeof(from));
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = ip;
    from.sin_port = port;
    jkl_Lock();
    rcon_packet(&from, p[4], rd32(p + 12), p + JKR_HDR_SIZE, p + len);
    jkl_Unlock();
}

// The bans as JKR_SETBANS carries them (u32 ip, u16 minutes left; 0 for good), at most max.
int jkrelay_ListenBanList(uint8_t* out, int max)
{
    int n = 0;
    uint64_t now = now_ms();
    jkl_Lock();
    for (int i = 0; i < MAX_BANS && n < max; i++)
    {
        uint64_t left;
        if (!g_bans[i].ip || (g_bans[i].until && now > g_bans[i].until)) continue;
        left = g_bans[i].until ? (g_bans[i].until - now + 59999) / 60000 : 0;
        memcpy(out + 6 * n, &g_bans[i].ip, 4);
        wr16(out + 6 * n + 4, (uint16_t)(left > 0xFFFF ? 0xFFFF : (left ? left : 1)) * (g_bans[i].until != 0));
        n++;
    }
    jkl_Unlock();
    return n;
}

// A ban the relay server did (JKR_HOSTKICK), kept with the others.
void jkrelay_ListenAddBan(uint32_t ip, int minutes)
{
    jkl_Lock();
    ban_add(ip, minutes);
    jkl_Unlock();
}

#endif // PLATFORM_DPLAY
