#ifndef _PLATFORM_NETWORKING_DPLAY_SHIM_H
#define _PLATFORM_NETWORKING_DPLAY_SHIM_H

// Thin C wrapper around the DirectPlay COM interface (IDirectPlay3, Unicode).
//
// This header deliberately uses only <stdint.h> types so it can be included by
// stdComm_dplay.c alongside OpenJKDF2's types.h, which defines its own DPID,
// DPNAME, GUID, etc. and would clash with <dplay.h>. All COM/<dplay.h> usage is
// confined to dplay_shim.c.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transports (see dplay_backend.h) define DPS_BACKEND_PREFIX before including
// this header so their implementations get their own names.
#ifdef DPS_BACKEND_PREFIX
#define DPS_CAT2(a, b) a##b
#define DPS_CAT(a, b) DPS_CAT2(a, b)
#define dps_Startup              DPS_CAT(DPS_BACKEND_PREFIX, _Startup)
#define dps_Shutdown             DPS_CAT(DPS_BACKEND_PREFIX, _Shutdown)
#define dps_Recreate             DPS_CAT(DPS_BACKEND_PREFIX, _Recreate)
#define dps_EnumConnections      DPS_CAT(DPS_BACKEND_PREFIX, _EnumConnections)
#define dps_InitializeConnection DPS_CAT(DPS_BACKEND_PREFIX, _InitializeConnection)
#define dps_EnumSessions         DPS_CAT(DPS_BACKEND_PREFIX, _EnumSessions)
#define dps_SetEnumHost          DPS_CAT(DPS_BACKEND_PREFIX, _SetEnumHost)
#define dps_StopEnumSessions     DPS_CAT(DPS_BACKEND_PREFIX, _StopEnumSessions)
#define dps_OpenCreate           DPS_CAT(DPS_BACKEND_PREFIX, _OpenCreate)
#define dps_OpenJoin             DPS_CAT(DPS_BACKEND_PREFIX, _OpenJoin)
#define dps_Close                DPS_CAT(DPS_BACKEND_PREFIX, _Close)
#define dps_GetSessionDesc       DPS_CAT(DPS_BACKEND_PREFIX, _GetSessionDesc)
#define dps_SetSessionDesc       DPS_CAT(DPS_BACKEND_PREFIX, _SetSessionDesc)
#define dps_CreatePlayer         DPS_CAT(DPS_BACKEND_PREFIX, _CreatePlayer)
#define dps_DestroyPlayer        DPS_CAT(DPS_BACKEND_PREFIX, _DestroyPlayer)
#define dps_EnumPlayers          DPS_CAT(DPS_BACKEND_PREFIX, _EnumPlayers)
#define dps_Send                 DPS_CAT(DPS_BACKEND_PREFIX, _Send)
#define dps_Receive              DPS_CAT(DPS_BACKEND_PREFIX, _Receive)
#endif

#define DPS_NAME_MAX     128
#define DPS_PASSWORD_MAX 64

// Mirrors DPSESSIONDESC2 with the string pointers replaced by inline buffers.
typedef struct dps_session
{
    uint8_t  guidInstance[16];
    uint32_t flags;          // DPSESSION_* flags
    uint32_t maxPlayers;
    uint32_t curPlayers;     // read-only, filled on enum/get
    uint16_t name[DPS_NAME_MAX];         // UTF-16, NUL-terminated
    uint16_t password[DPS_PASSWORD_MAX]; // UTF-16, NUL-terminated (may be empty)
    uint32_t user[4];        // dwUser1..dwUser4
} dps_session;

// DPSESSION_* values used by the glue layer (same values as <dplay.h>).
#define DPS_SESSION_JOINDISABLED      0x00000020
#define DPS_SESSION_KEEPALIVE         0x00000040
#define DPS_SESSION_NODATAMESSAGES    0x00000080
#define DPS_SESSION_PASSWORDREQUIRED  0x00000400

// Normalized system messages.
enum
{
    DPS_SYS_OTHER = 0,
    DPS_SYS_PLAYERCREATED,   // DPSYS_CREATEPLAYERORGROUP (players only)
    DPS_SYS_PLAYERDESTROYED, // DPSYS_DESTROYPLAYERORGROUP (players only)
    DPS_SYS_SESSIONLOST,     // DPSYS_SESSIONLOST
};

typedef struct dps_sysmsg
{
    int      kind;       // DPS_SYS_*
    uint32_t rawType;    // original DPSYS_* value, for logging
    uint32_t playerId;   // valid for PLAYERCREATED / PLAYERDESTROYED
} dps_sysmsg;

enum
{
    DPS_RECV_NONE = -1,  // queue empty (or error)
    DPS_RECV_DATA = 0,   // application data in *dataOut
    DPS_RECV_SYS  = 1,   // system message in *sysOut
};

// Return nonzero to continue enumeration.
typedef int (*dps_connection_cb)(void* ctx, const uint8_t guidSP[16], const uint16_t* name,
                                 const void* connection, uint32_t connectionSize);
typedef int (*dps_session_cb)(void* ctx, const dps_session* session);
typedef int (*dps_player_cb)(void* ctx, uint32_t id, const uint16_t* shortName, const uint16_t* longName);

// All functions returning long return an HRESULT (0 == DP_OK).
long dps_Startup(const uint8_t appGuid[16]); // CoInitialize + create IDirectPlay3
void dps_Shutdown(void);
long dps_Recreate(void);                     // Release + create a fresh object (InitializeConnection is once-only)

long dps_EnumConnections(dps_connection_cb cb, void* ctx);
long dps_InitializeConnection(const void* connection);

long dps_EnumSessions(dps_session_cb cb, void* ctx);   // async, includes passworded sessions
// Also search this host (name or IPv4) directly, for hosts that broadcasts
// can't reach (e.g. over the internet). NULL/empty clears it.
void dps_SetEnumHost(const char* host);
long dps_StopEnumSessions(void);

long dps_OpenCreate(const dps_session* desc);
long dps_OpenJoin(const uint8_t guidInstance[16], const uint16_t* password);
long dps_Close(void);
long dps_GetSessionDesc(dps_session* out);
long dps_SetSessionDesc(const dps_session* in);

long dps_CreatePlayer(const uint16_t* shortName, uint32_t* idOut);
long dps_DestroyPlayer(uint32_t id);
long dps_EnumPlayers(dps_player_cb cb, void* ctx);

long dps_Send(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size); // unguaranteed
int  dps_Receive(uint32_t* fromOut, const void** dataOut, uint32_t* sizeOut, dps_sysmsg* sysOut);

#ifndef DPS_BACKEND_PREFIX
// Dispatcher-only (dplay_dispatch.c):
// "host" or "host:port" of the Imperial Network Protocol relay server.
void dps_SetRelayServer(const char* server);
const char* dps_GetRelayServer(void);
// Nonzero while the Imperial Network Protocol relay is the selected connection.
int  dps_IsRelayActive(void);
// Ask for direct links to the host / joiners in the next relay game (net_directLinks).
void dps_SetRelayDirect(int on);
// Master servers whose games the join screen lists too (net_masterServers): "host[:port]",
// separated by commas or spaces.
void dps_SetRelayMasters(const char* list);
// Why the relay server ended our last game / refused our last join (JKR_LOST_*:
// 1 kicked, 2 banned; 0 nothing said). *pMinutes: a ban's minutes, 0 = for good.
int  dps_RelayLostReason(int* pMinutes);
int  dps_RelayJoinRefusal(int* pMinutes);
// Hit what you see: a number for the next dps_Send's position update, and the one the
// last data dps_Receive returned came with (1 if it had one). Relay only.
void dps_RelaySetSendSeq(uint32_t seq);
// Map and mod downloads: frames of up to 64 KiB between machines (system player ids),
// sent in pieces and handed whole to the handler. The host's machine (0 if not in a
// relay game), and whether it's this one.
void dps_RelaySetBigHandler(void (*fn)(uint32_t from, const uint8_t* p, size_t n));
int  dps_RelaySendBig(uint32_t idTo, const uint8_t* p, size_t n);
uint32_t dps_RelayHostSystemId(void);
int  dps_RelayIsHost(void);
// Hit what you see: another player's game clock less ours (ms, NaN if unknown), measured
// with pings while asked for (dps_RelaySyncClock), on the clock dps_RelaySetClock gives.
void dps_RelaySetClock(double (*fn)(void));
void dps_RelaySyncClock(uint32_t id, int on);
double dps_RelayClockOffset(uint32_t id);
int  dps_RelayLastRecvSeq(uint32_t* pSeq);
// A dedicated server's own Imperial Network Protocol server on UDP 'port'
// (jkrelay_listen.c), on its own thread, listed on master server 'master'
// ("host[:port]", or NULL). 0 on failure (logged).
int  dps_RelayListen(int port, int wsPort, const char* master);
void dps_RelayStopListening(void);
// Its admin side (Main/jkServer.c; jkrelay_listen.c): players' addresses, kicks
// that stick, bans ("a.b.c.d", minutes 0 = for good), rcon commands to run.
int  dps_ServerListening(void);
int  dps_ServerPlayerAddr(uint32_t id, char* out, size_t cap);
int  dps_ServerKick(uint32_t id, int banMinutes, char* ipOut, size_t cap);   // banMinutes DPS_KICK_RECONNECT: asked to join again
#define DPS_KICK_RECONNECT (-2)   // = JKR_KICK_RECONNECT
int  dps_ServerBan(const char* ip, int minutes);
int  dps_ServerUnban(const char* ip);
void dps_ServerListBans(char* out, size_t cap, int bForGoodOnly);
void dps_ServerSetRcon(const char* password);
// Rcon's protection: wrong passwords before an address is blocked, for how many
// minutes, and whether an address may send only one command a second.
void dps_ServerSetRconLimits(int maxTries, int blockMinutes, int bFloodProtect);
int  dps_ServerTakeRcon(char* cmd, size_t cap);
void dps_ServerRconDone(const char* output);
// Console "rcon": a command for a dedicated server ("host[:port]"); its output in out.
// guid: the game it's for (NULL: the server's own), so a relay server passes it to that game's host.
int  dps_Rcon(const char* server, const uint8_t* guid, const char* password, const char* cmd, char* out, size_t cap, int timeoutMs);
// A dedicated server hosting on a relay server: its rcon, kicks and bans through the relay
// (jkrelay_proto.h, "Rcon through a relay", "Hosts' admin"); its bans sent there again.
void dps_ServerTunnelStart(void);
void dps_ServerPushBans(void);
// Console "connect": asks a server ("host[:port]", a dedicated server's own or
// a relay) for its games, outside any connection. Returns how many it lists
// (up to maxOut of them in out), 0 none, -1 no answer within timeoutMs.
int  dps_RelayProbe(const char* server, const uint8_t appGuid[16], int timeoutMs, dps_session* out, int maxOut);
// The server the relay game in progress is on: a listed server's own "ip:port",
// else the relay server.
const char* dps_RelayCurrentServer(void);
// Web lobby (jkdf2mp browser build): "wss://host/relay", "ws://host:port/relay"
// or a bare host name (wss, /relay assumed).
void dps_SetWebLobby(const char* url);
const char* dps_GetWebLobby(void);
int  dps_IsWebLobbyActive(void);   // nonzero while it's the selected connection
int  dps_HasWebLobby(void);        // nonzero if this build includes it (libcurl)
// The selected connection in words, for the "gameinfo" console command, and
// how its game packets travel ("" if there's nothing to add).
void dps_DescribeConnection(char* out, size_t cap, char* links, size_t linksCap);
// Call while menus are up (the game isn't calling dps_Receive): keeps the
// relay connection alive. Cheap; doesn't block.
void dps_Idle(void);
// Mods on the web lobby (jkmod.h tails): a listed game's, NULL if the
// selected connection doesn't carry them; and ours, sent when hosting.
const char* dps_GetSessionModTail(const uint8_t guid[16]);
void dps_SetHostModTail(const char* hex);
// Web lobby: our game (MotS or JK) and player name, so the lobby counts and
// names us and lists only the games we can join.
void dps_SetWebPresence(int mots, const uint16_t* name);
#endif

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_DPLAY_SHIM_H
