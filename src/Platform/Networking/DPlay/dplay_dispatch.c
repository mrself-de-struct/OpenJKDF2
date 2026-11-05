// Implements the dps_* API (dplay_shim.h) by forwarding to the transport that
// belongs to the connection the player picked in the multiplayer menu:
//
//   DirectPlay entries      -> dpcom_backend (Windows) / dpnative_backend
//                              Plays with the original JK.EXE. Needs reachable
//                              ports (TCP 2300, UDP 2350, UDP 47624 to host).
//                              dpnative also has an IPX entry (IPX over UDP
//                              54792, for JK.EXE players using IPXWrapper).
//   "Imperial Network Protocol" -> jkrelay_backend
//                              OpenJKDF2 players only, through a relay server;
//                              nobody needs port forwarding.
//   "OpenJKDF2 Web Lobby"   -> jkweb_backend
//                              Plays with the jkdf2mp browser build through its
//                              WebSocket lobby; nobody needs port forwarding.
//
// Both carry the same game messages, so gameplay is identical.

#if defined(PLATFORM_DPLAY)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dplay_backend.h"

#if defined(DPLAY_NATIVE)
#define DPS_DIRECTPLAY (&dpnative_backend)
int dpn_IsIpxActive(void);   // dplay_native.c
void dpn_Idle(void);         // dplay_native.c
#else
#define DPS_DIRECTPLAY (&dpcom_backend)
#endif

// Connection blob that marks the relay entry in the connection list.
static const uint8_t dps_kRelayBlob[4] = { 'J', 'K', 'R', '1' };
// {6F4A4B52-4F4A-4B44-4632-4F4E4C494E45}: identifies the relay "service provider"
static const uint8_t dps_kRelayGuid[16] = { 0x52, 0x4B, 0x4A, 0x6F, 0x4A, 0x4F, 0x44, 0x4B,
                                            0x46, 0x32, 0x4F, 0x4E, 0x4C, 0x49, 0x4E, 0x45 };
// "Imperial Network Protocol": our relay (named "OpenJKDF2 Online (no port forwarding)" until 2026-10-26)
static const uint16_t dps_kRelayName[] = { 'I','m','p','e','r','i','a','l',' ','N','e','t','w','o','r','k',
                                           ' ','P','r','o','t','o','c','o','l',0 };

// Web lobby entry.
static const uint8_t dps_kWebBlob[4] = { 'J', 'K', 'W', '1' };
static const uint8_t dps_kWebGuid[16] = { 0x57, 0x4B, 0x4A, 0x6F, 0x4A, 0x4F, 0x44, 0x4B,
                                          0x46, 0x32, 0x57, 0x45, 0x42, 0x4C, 0x42, 0x59 };
static const uint16_t dps_kWebName[] = { 'O','p','e','n','J','K','D','F','2',' ','W','e','b',' ','L','o','b','b','y',' ',
                                         '(','b','r','o','w','s','e','r',' ','p','l','a','y','e','r','s',')',0 };
#if defined(PLATFORM_CURL)
static const dps_backend* const DPS_WEB = &jkweb_backend;
void jkweb_SetLobby(const char* url);
const char* jkweb_GetSessionTail(const uint8_t guid[16]);
void jkweb_SetHostTail(const char* hex);
void jkweb_SetPresence(int mots, const uint16_t* name);
void jkweb_DescribeLinks(char* out, size_t cap);
#else
static const dps_backend* const DPS_WEB = NULL;   // no libcurl: no web lobby
#endif
static int dps_webStarted = 0;
static char dps_webLobby[512] = { 0 };

static const dps_backend* dps_cur = NULL;     // selected transport
static uint8_t dps_appGuid[16];
static int dps_dplayStarted = 0;
static int dps_relayStarted = 0;
static char dps_relayServer[256] = { 0 };

void jkrelay_SetServer(const char* server);  // jkrelay_client.c
void jkrelay_Idle(void);                     // jkrelay_client.c
void jkrelay_SetDirect(int on);              // jkrelay_client.c
void jkrelay_SetMasters(const char* list);   // jkrelay_client.c
int  jkrelay_LostReason(int* pMinutes);      // jkrelay_client.c
int  jkrelay_JoinRefusal(int* pMinutes);     // jkrelay_client.c
void jkrelay_DescribeLinks(char* out, size_t cap);
int  jkrelay_DescribeDirect(char* out, size_t cap);
int  jkrelay_Probe(const char* server, const uint8_t appGuid[16], int timeoutMs, dps_session* out, int maxOut);
int  jkrelay_Listen(int port, int wsPort, const char* master); // jkrelay_listen.c
int  jkrelay_Listening(void);
int  jkrelay_ListenPlayerAddr(uint32_t id, char* out, size_t cap);
int  jkrelay_ListenKick(uint32_t id, int banMinutes, char* ipOut, size_t cap);
int  jkrelay_ListenBan(const char* ip, int minutes);
int  jkrelay_ListenUnban(const char* ip);
void jkrelay_ListenListBans(char* out, size_t cap, int bForGoodOnly);
void jkrelay_ListenSetRcon(const char* password);
void jkrelay_ListenSetRconLimits(int maxTries, int blockMinutes, int bFloodProtect);
int  jkrelay_ListenTakeRcon(char* cmd, size_t cap);
void jkrelay_ListenRconDone(const char* output);
int  jkrelay_Rcon(const char* server, const uint8_t* guid, const char* password, const char* cmd, char* out, size_t cap, int timeoutMs);
void jkrelay_ListenTunnelStart(void (*send)(uint32_t ip, uint16_t port, const void* p, size_t n));   // jkrelay_listen.c
int  jkrelay_ListenTunnelActive(void);
void jkrelay_ListenTunnelPacket(uint32_t ip, uint16_t port, const uint8_t* p, size_t len);
int  jkrelay_ListenBanList(uint8_t* out, int max);
void jkrelay_ListenAddBan(uint32_t ip, int minutes);
void jkrelay_SetTunnelHandler(void (*fn)(uint32_t ip, uint16_t port, const uint8_t* p, size_t n));   // jkrelay_client.c
void jkrelay_TunnelReply(uint32_t ip, uint16_t port, const void* p, size_t n);
int  jkrelay_HostKick(uint32_t id, int banMinutes, uint32_t* pIp);
int  jkrelay_HostPlayerAddr(uint32_t id, char* out, size_t cap);
void jkrelay_HostSetBans(const uint8_t* list, int n);
void jkrelay_StopListening(void);
void jkrelay_SetSendSeq(uint32_t seq);       // jkrelay_client.c
void jkrelay_SetBigHandler(void (*fn)(uint32_t from, const uint8_t* p, size_t n));
int  jkrelay_SendBig(uint32_t idFrom, uint32_t idTo, const uint8_t* p, size_t n);
uint32_t jkrelay_LocalSystemId(void);
uint32_t jkrelay_HostSystemId(void);
int  jkrelay_IsHost(void);
void jkrelay_SetHostTail(const char* hex);
void jkrelay_SetClock(double (*fn)(void));
void jkrelay_SyncClock(uint32_t id, int on);
double jkrelay_ClockOffset(uint32_t id);
const char* jkrelay_GetSessionTail(const uint8_t guid[16]);
int  jkrelay_LastRecvSeq(uint32_t* pSeq);

// ---------------------------------------------------------------------------

void dps_SetRelayServer(const char* server)
{
    size_t n = server ? strlen(server) : 0;
    if (n >= sizeof(dps_relayServer)) n = sizeof(dps_relayServer) - 1;
    memcpy(dps_relayServer, server ? server : "", n);
    dps_relayServer[n] = 0;
    jkrelay_SetServer(dps_relayServer);
}

const char* dps_GetRelayServer(void) { return dps_relayServer; }

int dps_IsRelayActive(void) { return dps_cur == &jkrelay_backend; }

void dps_SetRelayDirect(int on) { jkrelay_SetDirect(on); }
void dps_SetRelayMasters(const char* list) { jkrelay_SetMasters(list); }
int  dps_RelayLostReason(int* pMinutes) { return jkrelay_LostReason(pMinutes); }
int  dps_RelayJoinRefusal(int* pMinutes) { return jkrelay_JoinRefusal(pMinutes); }
void dps_RelaySetBigHandler(void (*fn)(uint32_t from, const uint8_t* p, size_t n)) { jkrelay_SetBigHandler(fn); }
int  dps_RelaySendBig(uint32_t idTo, const uint8_t* p, size_t n)
{
    return dps_IsRelayActive() && jkrelay_SendBig(jkrelay_LocalSystemId(), idTo, p, n);
}
uint32_t dps_RelayHostSystemId(void) { return dps_IsRelayActive() ? jkrelay_HostSystemId() : 0; }
int  dps_RelayIsHost(void) { return dps_IsRelayActive() && jkrelay_IsHost(); }
void dps_RelaySetClock(double (*fn)(void)) { jkrelay_SetClock(fn); }
void dps_RelaySyncClock(uint32_t id, int on) { if (dps_IsRelayActive()) jkrelay_SyncClock(id, on); }
double dps_RelayClockOffset(uint32_t id) { return dps_IsRelayActive() ? jkrelay_ClockOffset(id) : NAN; }
void dps_RelaySetSendSeq(uint32_t seq) { if (dps_IsRelayActive()) jkrelay_SetSendSeq(seq); }
int  dps_RelayLastRecvSeq(uint32_t* pSeq)
{
    if (dps_IsRelayActive()) return jkrelay_LastRecvSeq(pSeq);
    if (pSeq) *pSeq = 0;
    return 0;
}
int  dps_RelayListen(int port, int wsPort, const char* master) { return jkrelay_Listen(port, wsPort, master); }
// A dedicated server's admin (Main/jkServer.c): on its own server (jkrelay_listen.c), or
// (2026-10-29) hosting on a relay server, with its rcon coming through the relay and
// its kicks and bans done there (jkrelay_proto.h, "Hosts' admin").
static int dps_RelayAdmin(void) { return jkrelay_ListenTunnelActive() && dps_IsRelayActive() && jkrelay_IsHost(); }

static void dps_TunnelOut(uint32_t ip, uint16_t port, const void* p, size_t n) { jkrelay_TunnelReply(ip, port, p, n); }

void dps_ServerTunnelStart(void)
{
    jkrelay_ListenTunnelStart(dps_TunnelOut);
    jkrelay_SetTunnelHandler(jkrelay_ListenTunnelPacket);
}

// Our bans, to the relay server (it keeps them for the game we host there).
void dps_ServerPushBans(void)
{
    static uint8_t list[6 * 128];
    if (!jkrelay_ListenTunnelActive()) return;
    jkrelay_HostSetBans(list, jkrelay_ListenBanList(list, 128));
}

int  dps_ServerListening(void) { return jkrelay_Listening() || jkrelay_ListenTunnelActive(); }
int  dps_ServerPlayerAddr(uint32_t id, char* out, size_t cap)
{
    if (jkrelay_Listening()) return jkrelay_ListenPlayerAddr(id, out, cap);
    return dps_RelayAdmin() && jkrelay_HostPlayerAddr(id, out, cap);
}
int  dps_ServerKick(uint32_t id, int banMinutes, char* ipOut, size_t cap)
{
    uint32_t ip = 0;
    if (jkrelay_Listening()) return jkrelay_ListenKick(id, banMinutes, ipOut, cap);
    if (!dps_RelayAdmin() || !jkrelay_HostKick(id, banMinutes, &ip)) return 0;
    if (banMinutes >= 0)
    {
        jkrelay_ListenAddBan(ip, banMinutes);
        dps_ServerPushBans();
    }
    if (cap)
    {
        const uint8_t* b = (const uint8_t*)&ip;
        snprintf(ipOut, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    return 1;
}
int  dps_ServerBan(const char* ip, int minutes)
{
    int r = jkrelay_ListenBan(ip, minutes);
    if (r) dps_ServerPushBans();
    return r;
}
int  dps_ServerUnban(const char* ip)
{
    int r = jkrelay_ListenUnban(ip);
    if (r) dps_ServerPushBans();
    return r;
}
void dps_ServerListBans(char* out, size_t cap, int bForGoodOnly) { jkrelay_ListenListBans(out, cap, bForGoodOnly); }
void dps_ServerSetRcon(const char* password) { jkrelay_ListenSetRcon(password); }
void dps_ServerSetRconLimits(int maxTries, int blockMinutes, int bFloodProtect) { jkrelay_ListenSetRconLimits(maxTries, blockMinutes, bFloodProtect); }
int  dps_ServerTakeRcon(char* cmd, size_t cap) { return jkrelay_ListenTakeRcon(cmd, cap); }
void dps_ServerRconDone(const char* output) { jkrelay_ListenRconDone(output); }
int  dps_Rcon(const char* server, const uint8_t* guid, const char* password, const char* cmd, char* out, size_t cap, int timeoutMs)
{
    return jkrelay_Rcon(server, guid, password, cmd, out, cap, timeoutMs);
}
void dps_RelayStopListening(void) { jkrelay_StopListening(); }
int  dps_RelayProbe(const char* server, const uint8_t appGuid[16], int timeoutMs, dps_session* out, int maxOut)
{
    return jkrelay_Probe(server, appGuid, timeoutMs, out, maxOut);
}
const char* dps_RelayCurrentServer(void)
{
    static char direct[64];
    return jkrelay_DescribeDirect(direct, sizeof(direct)) ? direct : dps_relayServer;
}

void dps_SetWebLobby(const char* url)
{
    size_t n = url ? strlen(url) : 0;
    if (n >= sizeof(dps_webLobby)) n = sizeof(dps_webLobby) - 1;
    memcpy(dps_webLobby, url ? url : "", n);
    dps_webLobby[n] = 0;
#if defined(PLATFORM_CURL)
    jkweb_SetLobby(dps_webLobby);
#endif
}

const char* dps_GetWebLobby(void) { return dps_webLobby; }
int dps_IsWebLobbyActive(void) { return DPS_WEB && dps_cur == DPS_WEB; }

const char* dps_GetSessionModTail(const uint8_t guid[16])
{
#if defined(PLATFORM_CURL)
    if (dps_IsWebLobbyActive()) return jkweb_GetSessionTail(guid);
#endif
    if (dps_IsRelayActive()) return jkrelay_GetSessionTail(guid); // Added: relay servers that list mods
    (void)guid;
    return NULL;
}

void dps_SetHostModTail(const char* hex)
{
#if defined(PLATFORM_CURL)
    jkweb_SetHostTail(hex);
#endif
    jkrelay_SetHostTail(hex); // Added: the relay's game list says it too
    (void)hex;
}
void dps_SetWebPresence(int mots, const uint16_t* name)
{
#if defined(PLATFORM_CURL)
    jkweb_SetPresence(mots, name);
#endif
    (void)mots; (void)name;
}
int dps_HasWebLobby(void) { return DPS_WEB != NULL; }

// Menus are up and the game isn't receiving: keep transports that need it alive.
void dps_Idle(void)
{
    if (dps_relayStarted) jkrelay_Idle();
#if defined(DPLAY_NATIVE)
    if (dps_dplayStarted) dpn_Idle();
#endif
}

void dps_DescribeConnection(char* out, size_t cap, char* links, size_t linksCap)
{
    if (linksCap) links[0] = 0;
    if (!cap) return;
    if (!dps_cur)
        snprintf(out, cap, "none");
    else if (dps_cur == &jkrelay_backend)
    {
        char direct[64];
        if (jkrelay_DescribeDirect(direct, sizeof(direct)))
            snprintf(out, cap, "Imperial Network Protocol, server at %s (listed on %s)", direct, dps_relayServer[0] ? dps_relayServer : "?");
        else
            snprintf(out, cap, "Imperial Network Protocol via %s", dps_relayServer[0] ? dps_relayServer : "?");
        if (linksCap) jkrelay_DescribeLinks(links, linksCap);
    }
    else if (dps_IsWebLobbyActive())
    {
        snprintf(out, cap, "OpenJKDF2 Web Lobby via %s", dps_webLobby[0] ? dps_webLobby : "?");
#if defined(PLATFORM_CURL)
        if (linksCap) jkweb_DescribeLinks(links, linksCap);
#endif
    }
    else
#if defined(DPLAY_NATIVE)
        snprintf(out, cap, dpn_IsIpxActive() ? "DirectPlay IPX (JK.EXE + IPXWrapper compatible)"
                                             : "DirectPlay TCP/IP (JK.EXE compatible)");
#else
        snprintf(out, cap, "DirectPlay TCP/IP (JK.EXE compatible, Microsoft DirectPlay)");
#endif
}

long dps_Startup(const uint8_t appGuid[16])
{
    long hr;

    // OPENJKDF2_DPLAY_LOGFILE=<path>: append the networking log (everything
    // the transports print to stderr) to a file. Handy on Windows, where the
    // game has no terminal to redirect.
    {
        static int redirected = 0;
        const char* logPath = getenv("OPENJKDF2_DPLAY_LOGFILE");
        if (!redirected && logPath && *logPath)
        {
            if (freopen(logPath, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
            redirected = 1;
        }
    }
    memcpy(dps_appGuid, appGuid, 16);
    dps_cur = NULL;

    hr = DPS_DIRECTPLAY->Startup(appGuid);
    dps_dplayStarted = (hr >= 0);

    // The relay only needs an outbound UDP socket; it can't fail for lack of
    // DirectPlay, so multiplayer stays available either way.
    dps_relayStarted = (jkrelay_backend.Startup(appGuid) >= 0);
    dps_webStarted = DPS_WEB && DPS_WEB->Startup(appGuid) >= 0;
    return (dps_dplayStarted || dps_relayStarted || dps_webStarted) ? 0 : hr;
}

void dps_Shutdown(void)
{
    if (dps_dplayStarted) DPS_DIRECTPLAY->Shutdown();
    if (dps_relayStarted) jkrelay_backend.Shutdown();
    if (dps_webStarted) DPS_WEB->Shutdown();
    dps_dplayStarted = dps_relayStarted = dps_webStarted = 0;
    dps_cur = NULL;
}

long dps_Recreate(void)
{
    long hr = dps_cur ? dps_cur->Recreate() : 0;
    dps_cur = NULL;   // the next InitializeConnection picks again
    return hr;
}

typedef struct
{
    dps_connection_cb cb;
    void* ctx;
    int stopped;
} dps_ConnCtx;

static int dps_ConnPassthrough(void* ctx, const uint8_t guidSP[16], const uint16_t* name,
                               const void* connection, uint32_t connectionSize)
{
    dps_ConnCtx* c = (dps_ConnCtx*)ctx;
    if (!c->cb(c->ctx, guidSP, name, connection, connectionSize)) { c->stopped = 1; return 0; }
    return 1;
}

long dps_EnumConnections(dps_connection_cb cb, void* ctx)
{
    dps_ConnCtx c = { cb, ctx, 0 };
    if (dps_dplayStarted)
        DPS_DIRECTPLAY->EnumConnections(dps_ConnPassthrough, &c);
    if (dps_relayStarted && !c.stopped)
        c.stopped = !cb(ctx, dps_kRelayGuid, dps_kRelayName, dps_kRelayBlob, sizeof(dps_kRelayBlob));
    if (dps_webStarted && !c.stopped)
        cb(ctx, dps_kWebGuid, dps_kWebName, dps_kWebBlob, sizeof(dps_kWebBlob));
    return (dps_dplayStarted || dps_relayStarted || dps_webStarted) ? 0 : (long)0x887707D0; // DPERR_UNINITIALIZED
}

long dps_InitializeConnection(const void* connection)
{
    if (connection && !memcmp(connection, dps_kRelayBlob, sizeof(dps_kRelayBlob)))
    {
        if (!dps_relayStarted) return (long)0x887700FA;   // DPERR_UNAVAILABLE
        dps_cur = &jkrelay_backend;
    }
    else if (connection && !memcmp(connection, dps_kWebBlob, sizeof(dps_kWebBlob)))
    {
        if (!dps_webStarted) return (long)0x887700FA;
        dps_cur = DPS_WEB;
    }
    else
    {
        if (!dps_dplayStarted) return (long)0x887700FA;
        dps_cur = DPS_DIRECTPLAY;
    }
    return dps_cur->InitializeConnection(connection);
}

#define DPS_FWD(call, fail) do { if (!dps_cur) return fail; return dps_cur->call; } while (0)

long dps_EnumSessions(dps_session_cb cb, void* ctx)       { DPS_FWD(EnumSessions(cb, ctx), (long)0x887707D0); }
void dps_SetEnumHost(const char* host)                     { if (dps_cur) dps_cur->SetEnumHost(host); }
long dps_StopEnumSessions(void)                            { DPS_FWD(StopEnumSessions(), 0); }
long dps_OpenCreate(const dps_session* desc)               { DPS_FWD(OpenCreate(desc), (long)0x887707D0); }
long dps_OpenJoin(const uint8_t g[16], const uint16_t* pw) { DPS_FWD(OpenJoin(g, pw), (long)0x887707D0); }
long dps_Close(void)                                       { DPS_FWD(Close(), 0); }
long dps_GetSessionDesc(dps_session* out)                  { DPS_FWD(GetSessionDesc(out), (long)0x887700AA); }
long dps_SetSessionDesc(const dps_session* in)             { DPS_FWD(SetSessionDesc(in), (long)0x887700AA); }
long dps_CreatePlayer(const uint16_t* n, uint32_t* id)     { DPS_FWD(CreatePlayer(n, id), (long)0x887700AA); }
long dps_DestroyPlayer(uint32_t id)                        { DPS_FWD(DestroyPlayer(id), (long)0x887700AA); }
long dps_EnumPlayers(dps_player_cb cb, void* ctx)          { DPS_FWD(EnumPlayers(cb, ctx), (long)0x887700AA); }
long dps_Send(uint32_t f, uint32_t t, const void* d, uint32_t s) { DPS_FWD(Send(f, t, d, s), (long)0x887700AA); }
int  dps_Receive(uint32_t* f, const void** d, uint32_t* s, dps_sysmsg* m) { DPS_FWD(Receive(f, d, s, m), DPS_RECV_NONE); }

#endif // PLATFORM_DPLAY
