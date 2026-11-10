// dplay_probe: exercise the native DirectPlay client against a real JK.EXE
// host without building the whole game.
//
//   cc -std=gnu11 -O2 -DPLATFORM_DPLAY -DDPLAY_NATIVE -Isrc/Platform/Networking/DPlay
//      src/Tests/dplay_probe.c src/Platform/Networking/DPlay/dplay_native.c
//      src/Platform/Networking/DPlay/dplay_dispatch.c
//      src/Platform/Networking/DPlay/jkrelay_client.c
//      src/Platform/Networking/DPlay/jkrelay_listen.c
//      src/Platform/Networking/DPlay/jkmod.c -lpthread -o dplay_probe
//
//   ./dplay_probe list                 search for JK sessions for ~6 s
//   ./dplay_probe join <n> [name]      join session n as a DirectPlay player,
//                                      print traffic, Ctrl-C to leave cleanly
//   ./dplay_probe host [name] [pw]     host a session (DirectPlay level only),
//                                      print joins/leaves and traffic
//   ./dplay_probe rcon <host[:port]> <password> <command...>
//                                      a dedicated server's rcon (prints its output);
//                                      PROBE_RCON_GAME=1: through a relay server, to the
//                                      host of the first game it lists
//   PROBE_RELAY=server[:port] ...      use Imperial Network Protocol (relay) instead of DirectPlay
//   PROBE_DIRECT=1 ...                 with the relay: ask for direct links (as net_directLinks 1)
//   PROBE_MASTERS=host[,host...] ...   with the relay: list the master servers' games too (as net_masterServers)
//   PROBE_WEB=ws(s)://host/relay ...   use the jkdf2mp web lobby (link with -lcurl -DPLATFORM_CURL -DLINUX)
//   PROBE_IPX=1 ...                    use DirectPlay IPX (IPXWrapper's IPX over UDP 54792)
//   PROBE_DOWNLOAD=<episode> join ...  with the relay: download the host's map <episode> (without
//                                      .gob) into ./episode as the game would, print the time and
//                                      speed, then leave. Build with -DPROBE_XFER -DJKX_TESTS and
//                                      jkweb_xfer.c (+ -lz); retail GOBs it refers to come from ./resource
//
// The probe joins at the DirectPlay level only. It never sends JK's own join
// request, so it never spawns in the game; the host will just see a DirectPlay
// player arrive and leave. Environment: OPENJKDF2_DPLAY_LOG=1 for protocol
// logging, OPENJKDF2_DPLAY_HOSTS=ip,... to search hosts directly.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dplay_shim.h"

#ifdef PROBE_XFER
// The joiner's side of map downloads (jkweb_xfer.c) over the relay, as stdComm_dplay.c
// gives it to the game: frames to the host's machine, and the host's frames back.
#include "jkweb_xfer.h"
uint64_t jkx_TestNowMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
int jkweb_XferDirectUp(void) { return 1; }
int jkweb_XferDirectPossible(void) { return 1; }
int jkweb_XferHostState(void) { return dps_RelayHostSystemId() ? 1 : 0; }
int jkweb_XferCanSend(uint32_t to, int allowDirect) { (void)to; (void)allowDirect; return 1; }
void jkweb_XferNoteLoss(uint32_t to) { (void)to; }
uint32_t jkweb_XferPartMax(void) { return 0; }
int jkweb_XferSend(uint32_t to, const uint8_t* frame, size_t n, int allowDirect)
{
    (void)allowDirect;
    // PROBE_TRACE=1: each block asked for (type 5: block number, count)
    if (getenv("PROBE_TRACE") && n >= 16 + 16 && frame[12] == 5)
        printf("  %6llu ms  ask block %u (x%u)\n", (unsigned long long)jkx_TestNowMs() % 1000000,
               frame[24] | frame[25] << 8 | frame[26] << 16, frame[28] | frame[29] << 8);
    return to == 1 && dps_RelaySendBig(dps_RelayHostSystemId(), frame, n) ? JKX_VIA_DIRECT : 0;
}
void jkweb_XferPump(void) { dps_Idle(); }
static void probe_BigFrame(uint32_t from, const uint8_t* p, size_t n)
{
    // PROBE_TRACE=1: each block part that arrives whole (type 6: block, its size, offset, length)
    if (getenv("PROBE_TRACE") && n >= 16 + 24 && p[12] == 6)
        printf("  %6llu ms  got block %u part at %u len %u of %u\n", (unsigned long long)jkx_TestNowMs() % 1000000,
               p[24] | p[25] << 8 | p[26] << 16, (unsigned)(p[32] | p[33] << 8 | p[34] << 16), (unsigned)(n - 16 - 24),
               (unsigned)(p[28] | p[29] << 8 | p[30] << 16));
    if (n >= 16 && !memcmp(p + 8, "JKMT", 4) && from == dps_RelayHostSystemId()) jkx_OnFrame(1, p, n, JKX_VIA_DIRECT);
}
#endif

static const uint8_t kJk[16] = { 0xC0,0x13,0x06,0xBF,0x79,0xDE,0xD0,0x11,0x99,0xC9,0x00,0xA0,0x24,0x76,0xAD,0x4B };

static const char* kDss[] = {
    [1]="THINGPOS",[2]="CHAT",[3]="SECTORFLAGS",[4]="FIREPROJECTILE",[5]="DEATH",[6]="DAMAGE",
    [7]="SETTHINGMODEL",[8]="SENDTRIGGER",[9]="PLAYKEY",[10]="PLAYSOUND",[11]="SYNCTHING",
    [12]="THINGFULLDESC",[13]="SYNCCOG",[14]="SURFACESTATUS",[15]="AISTATUS",[16]="INVENTORY",
    [17]="SURFACE",[18]="SECTORSTATUS",[19]="PLAYKEYMODE",[20]="PATHMOVE",[21]="SYNCPUPPET",
    [22]="SYNCTHINGATTACHMENT",[23]="SYNCEVENTS",[24]="SYNCCAMERAS",[25]="TAKEITEM1",[26]="TAKEITEM2",
    [27]="STOPKEY",[28]="STOPSOUND",[29]="CREATETHING",[30]="SYNCPALEFFECTS",[32]="LEAVEJOIN",
    [33]="WELCOME",[34]="JOINREQUEST",[35]="DESTROYTHING",[36]="JOINING",[37]="PLAYSOUNDMODE",
    [38]="PING",[39]="PINGREPLY",[40]="RESET",[41]="ENUMPLAYERS",[42]="QUIT",[48]="JKENABLESABER",
    [49]="SABERINFO3",[56]="ENDLEVEL",[57]="SABERINFO1",[58]="SABERINFO2",[59]="JKSETWEAPONMESH",[60]="SETTEAM",
};

static volatile sig_atomic_t g_stop = 0;
static int g_wantRelay, g_haveConn;
static uint8_t g_conn[64];

static int onConnection(void* ctx, const uint8_t guidSP[16], const uint16_t* name, const void* conn, uint32_t size)
{
    int isRelay = (size == 4 && !memcmp(conn, "JKR1", 4)) ? 1 : (size == 4 && !memcmp(conn, "JKW1", 4)) ? 2
                : (size == 4 && !memcmp(conn, "DPNX", 4)) ? 3 : 0;
    (void)ctx; (void)guidSP; (void)name;
    if (g_haveConn || isRelay != g_wantRelay || size > sizeof(g_conn)) return 1;
    memcpy(g_conn, conn, size);
    g_haveConn = 1;
    return 1;
}
static void onSig(int s) { (void)s; g_stop = 1; }

static dps_session g_sessions[32];
static int g_count;

static void str8(const uint16_t* s, char* out, size_t n)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) out[i] = s[i] < 0x80 ? (char)s[i] : '?';
    out[i] = 0;
}

static int onSession(void* ctx, const dps_session* s)
{
    (void)ctx;
    for (int i = 0; i < g_count; i++)
        if (!memcmp(g_sessions[i].guidInstance, s->guidInstance, 16)) { g_sessions[i] = *s; return 1; }
    if (g_count < 32) g_sessions[g_count++] = *s;
    return 1;
}

static int onPlayer(void* ctx, uint32_t id, const uint16_t* shortName, const uint16_t* longName)
{
    char n[64];
    (void)ctx; (void)longName;
    str8(shortName, n, sizeof(n));
    printf("    player %08x %s\n", id, n);
    return 1;
}

static void search(void)
{
    g_count = 0;
    for (int i = 0; i < 4; i++)
    {
        dps_EnumSessions(onSession, NULL);
        usleep(1500 * 1000);
    }
    dps_EnumSessions(onSession, NULL);
    for (int i = 0; i < g_count; i++)
    {
        char n[256];
        str8(g_sessions[i].name, n, sizeof(n));
        printf("[%d] %-40s %u/%u players  flags=0x%x  version=%u  tick=%u ms%s\n", i, n,
               g_sessions[i].curPlayers, g_sessions[i].maxPlayers, g_sessions[i].flags,
               g_sessions[i].user[2] & 0xFFFF, g_sessions[i].user[3],
               (g_sessions[i].flags & DPS_SESSION_PASSWORDREQUIRED) ? "  (password)" : "");
        {
            const char* t = dps_GetSessionModTail(g_sessions[i].guidInstance);
            if (t && *t) printf("    mod tail: %s\n", t);
        }
    }
    if (!g_count) printf("no sessions found (try OPENJKDF2_DPLAY_HOSTS=<host ip>)\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s list | join <n> [name] | host [name] [pw] | rcon <host[:port]> <password> <command>\n", argv[0]); return 2; }
    if (!strcmp(argv[1], "rcon"))
    {
        static char out[16384];
        char cmd[512] = { 0 };
        if (argc < 5) { fprintf(stderr, "usage: %s rcon <host[:port]> <password> <command...>\n", argv[0]); return 2; }
        for (int i = 4; i < argc; i++)
        {
            if (i > 4) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        // PROBE_RCON_GAME=1: for the first game the server lists (a dedicated server hosting
        // on a relay server, whose rcon the relay passes on).
        uint8_t guid[16];
        int bGuid = 0;
        if (getenv("PROBE_RCON_GAME"))
        {
            dps_session games[8];
            if (dps_RelayProbe(argv[2], kJk, 3000, games, 8) <= 0) { fprintf(stderr, "no games on %s\n", argv[2]); return 1; }
            memcpy(guid, games[0].guidInstance, 16);
            bGuid = 1;
        }
        if (!dps_Rcon(argv[2], bGuid ? guid : NULL, argv[3], cmd, out, sizeof(out), 15000))
        {
            fprintf(stderr, "no answer from %s\n", argv[2]);
            return 1;
        }
        fputs(out, stdout);
        if (out[0] && out[strlen(out) - 1] != '\n') putchar('\n');
        return 0;
    }
    if (dps_Startup(kJk)) { fprintf(stderr, "startup failed\n"); return 1; }

    // Pick the connection: Imperial Network Protocol relay if PROBE_RELAY=host[:port],
    // otherwise DirectPlay.
    {
        const char* relay = getenv("PROBE_RELAY");
        const char* web = getenv("PROBE_WEB");
        if (relay && *relay) dps_SetRelayServer(relay);
        dps_SetRelayDirect(getenv("PROBE_DIRECT") != NULL);   // relay: direct links to the host / joiners
        if (getenv("PROBE_MASTERS")) dps_SetRelayMasters(getenv("PROBE_MASTERS"));   // relay: master servers' games too
        if (web && *web) dps_SetWebLobby(web);
        g_wantRelay = (relay && *relay) ? 1 : ((web && *web) ? 2 : (getenv("PROBE_IPX") ? 3 : 0));
        dps_EnumConnections(onConnection, NULL);
        if (!g_haveConn || dps_InitializeConnection(g_conn)) { fprintf(stderr, "no usable connection\n"); return 1; }
        printf("connection: %s\n", g_wantRelay == 1 ? "Imperial Network Protocol relay" : g_wantRelay == 2 ? "web lobby"
               : g_wantRelay == 3 ? "DirectPlay IPX" : "DirectPlay");
    }

    uint32_t myId = 0;
    long hr;
    const char* nm;
    uint16_t name[32] = { 0 };

    if (!strcmp(argv[1], "host"))
    {
        dps_session s;
        const char* sn = "probe:JK1MP:m10.jkl";
        memset(&s, 0, sizeof(s));
        for (int i = 0; i < DPS_NAME_MAX - 1 && sn[i]; i++) s.name[i] = (uint8_t)sn[i];
        s.flags = DPS_SESSION_KEEPALIVE | DPS_SESSION_NODATAMESSAGES;
        if (argc > 3) { s.flags |= DPS_SESSION_PASSWORDREQUIRED; for (int i = 0; i < DPS_PASSWORD_MAX - 1 && argv[3][i]; i++) s.password[i] = (uint8_t)argv[3][i]; }
        s.maxPlayers = 16;
        s.user[0] = 0x12345678; s.user[1] = 0x90; s.user[2] = 10 | (8u << 16); s.user[3] = 180;
        if (getenv("PROBE_MOD_TAIL")) dps_SetHostModTail(getenv("PROBE_MOD_TAIL"));
        hr = dps_OpenCreate(&s);
        if (hr) { fprintf(stderr, "host failed: 0x%08lx\n", (unsigned long)hr); dps_Shutdown(); return 1; }
        printf("hosting '%s'\n", sn);
        nm = argc > 2 ? argv[2] : "hostbot";
        goto have_session;
    }

    search();
    if (strcmp(argv[1], "join") || argc < 3) { dps_Shutdown(); return 0; }

    int idx = atoi(argv[2]);
    if (idx < 0 || idx >= g_count) { fprintf(stderr, "no session %d\n", idx); dps_Shutdown(); return 1; }

    nm = argc > 3 ? argv[3] : "dpprobe";
    {
        uint16_t pw[DPS_PASSWORD_MAX] = { 0 };
        const char* pws = getenv("PROBE_PASSWORD");
        for (int i = 0; pws && i < DPS_PASSWORD_MAX - 1 && pws[i]; i++) pw[i] = (uint8_t)pws[i];
        hr = dps_OpenJoin(g_sessions[idx].guidInstance, pw);
    }
    if (hr) { fprintf(stderr, "join failed: 0x%08lx\n", (unsigned long)hr); dps_Shutdown(); return 1; }
    printf("joined session\n");

have_session:
    for (int i = 0; i < 31 && nm[i]; i++) name[i] = (uint8_t)nm[i];
    hr = dps_CreatePlayer(name, &myId);
    if (hr) { fprintf(stderr, "create player failed: 0x%08lx\n", (unsigned long)hr); dps_Close(); dps_Shutdown(); return 1; }
    printf("created player %08x '%s'\n", myId, nm);
    dps_EnumPlayers(onPlayer, NULL);

#ifdef PROBE_XFER
    if (getenv("PROBE_DOWNLOAD"))
    {
        uint64_t t0 = jkx_TestNowMs(), lastShow = 0;
        int st;
        dps_RelaySetBigHandler(probe_BigFrame);
        // PROBE_DECLINE=1: as a player who answers No to the download, leaves, and joins
        // the same game again (the join screen does dps_Close, then dps_OpenJoin)
        if (getenv("PROBE_DECLINE") && !strcmp(argv[1], "join"))
        {
            int st0;
            jkx_SetAskFirst(1);
            jkx_StartMap(getenv("PROBE_DOWNLOAD"), "gob");
            while ((st0 = jkx_Tick()) == JKX_BUSY && !g_stop) { usleep(2000); }
            if (st0 == JKX_ASK) { jkx_Answer(0); st0 = jkx_Tick(); }
            printf("first try: %s (%s)\n", st0 == JKX_FAILED ? "failed" : st0 == JKX_READY ? "ready" : "?", jkx_Reason() ? jkx_Reason() : "");
            dps_Close();
            {
                uint16_t pw[DPS_PASSWORD_MAX] = { 0 };
                long hr2 = dps_OpenJoin(g_sessions[idx].guidInstance, pw);
                printf("joined again: hr %08lx, host system id %08x\n", (unsigned long)hr2, dps_RelayHostSystemId());
            }
            dps_RelaySetBigHandler(probe_BigFrame);
        }
        jkx_SetAskFirst(0);
        jkx_StartMap(getenv("PROBE_DOWNLOAD"), "gob");
        while ((st = jkx_Tick()) == JKX_BUSY && !g_stop)
        {
            uint32_t from = 0, size = 0;
            const void* data = NULL;
            dps_sysmsg sys;
            while (dps_Receive(&from, &data, &size, &sys) != DPS_RECV_NONE) {}
            if (jkx_TestNowMs() - lastShow >= 2000) { lastShow = jkx_TestNowMs(); printf("  %s\n", jkx_Status()); fflush(stdout); }
            // PROBE_SLEEP_MS: between reads, as the game's frames (16 at 60 fps); default 2
            usleep(getenv("PROBE_SLEEP_MS") ? (useconds_t)atoi(getenv("PROBE_SLEEP_MS")) * 1000 : 2000);
        }
        {
            double secs = (double)(jkx_TestNowMs() - t0) / 1000.0;
            printf("download %s after %.1f s: %s %s\n", st == JKX_READY ? "done" : "FAILED", secs, jkx_Reason() ? jkx_Reason() : "",
                   jkx_ResultPath() ? jkx_ResultPath() : "");
        }
        dps_Close();
        dps_Shutdown();
        return st == JKX_READY ? 0 : 1;
    }
#endif

    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);
    time_t lastReport = time(NULL);
    unsigned long counts[64] = { 0 }, total = 0, bytes = 0;

    time_t lastSend = 0;
    while (!g_stop)
    {
        if (getenv("PROBE_SEND") && time(NULL) != lastSend)
        {
            // A harmless DSS_PING (id 38) to everyone, once a second, to test the data path.
            uint8_t ping[8] = { 38, 0, 0, 0, 0, 0, 0, 0 };
            lastSend = time(NULL);
            // PROBE_SEND_TO=id sends to one player instead of everyone (the web
            // lobby's hosts can't broadcast; JK always addresses players).
            dps_Send(myId, getenv("PROBE_SEND_TO") ? (uint32_t)strtoul(getenv("PROBE_SEND_TO"), NULL, 0) : 0, ping, sizeof(ping));
        }
        uint32_t from = 0, size = 0;
        const void* data = NULL;
        dps_sysmsg sys;
        int r = dps_Receive(&from, &data, &size, &sys);
        if (r == DPS_RECV_NONE) { usleep(5000); }
        else if (r == DPS_RECV_DATA)
        {
            total++; bytes += size;
            if (size >= 2)
            {
                unsigned id = ((const uint8_t*)data)[0] | (((const uint8_t*)data)[1] << 8);
                if (id < 64) counts[id]++;
                if (id < 64 && id != 1 && kDss[id] && (id == 32 || id == 33 || id == 36 || id == 38 || id == 41 || id == 42))
                    printf("  from %08x: DSS_%s (%u bytes)\n", from, kDss[id], size);
            }
        }
        else if (r == DPS_RECV_SYS)
        {
            const char* k = sys.kind == DPS_SYS_PLAYERCREATED ? "player created" :
                            sys.kind == DPS_SYS_PLAYERDESTROYED ? "player left" :
                            sys.kind == DPS_SYS_SESSIONLOST ? "SESSION LOST" : "other";
            printf("  system: %s %08x\n", k, sys.playerId);
            if (sys.kind == DPS_SYS_SESSIONLOST) break;
        }

        if (time(NULL) - lastReport >= 10)
        {
            lastReport = time(NULL);
            printf("  [%lu msgs, %lu bytes]", total, bytes);
            for (int i = 0; i < 64; i++)
                if (counts[i] && kDss[i]) printf(" %s=%lu", kDss[i], counts[i]);
            printf("\n");
            fflush(stdout);
        }
    }

    printf("leaving\n");
    dps_Close();
    dps_Shutdown();
    return 0;
}
