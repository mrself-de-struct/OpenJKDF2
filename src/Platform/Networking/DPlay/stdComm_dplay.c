// Vanilla-compatible DirectPlay networking backend for OpenJKDF2.
//
// Implements the same DirectPlay_* / stdComm_* API as the GNS, Basic and None
// backends, but on top of real Microsoft DirectPlay (IDirectPlay3) so that
// OpenJKDF2 and unmodified JK.EXE 1.0 can join each other's games.
//
// Provenance of each behaviour is marked:
//   [WIRE]     confirmed from a packet capture of vanilla JK.EXE sessions
//   [DECOMP]   follows OpenJKDF2's decompiled callers (stdComm.c, sithMulti.c, GUI)
//   [INFERRED] best reconstruction; verify against the original function in
//              Ghidra (addresses in Win95/stdComm.h) or with a capture
//
// All COM/<dplay.h> code is in dplay_shim.c; this file never includes <dplay.h>
// because OpenJKDF2's types.h defines its own DPID/DPNAME.

#include "stdComm_dplay.h"
#include "dplay_shim.h"

#include "Win95/stdComm.h"
#include "Win95/WinIdk.h"
#include "Platform/wuRegistry.h"
#include "Gui/jkGUIDialog.h"
#include "Main/sithCvar.h"
#include "jkmod.h"
#include "jkrelay_proto.h"
#include "Main/jkRes.h"
#ifdef PLATFORM_CURL
#include "jkweb_xfer.h"
#endif
#include "Dss/sithMulti.h"
#include "Gameplay/sithPlayer.h" // Added: sithPlayer_g_pLocalPlayerThing
#include "Platform/Networking/Hits/stdComm_Hits.h" // Added: Hit what you see
#include "Devices/sithConsole.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "Platform/Common/stdLog.h"
#include "jk.h"

#include <string.h>
#include <stdlib.h>

#ifdef PLATFORM_DPLAY

#ifndef FAILED
#define FAILED(hr)    ((int32_t)(hr) < 0)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) ((int32_t)(hr) >= 0)
#endif

// [WIRE] JK's DirectPlay application GUID {BF0613C0-DE79-11D0-99C9-00A02476AD4B},
// seen in both EnumSessions requests and replies. Matches Windows_DplayGuid in
// Win95/Windows.c. Used only if WinIdk hasn't been given one (MotS sets its own).
static const uint8_t DPlay_kJkAppGuid[16] = {
    0xC0, 0x13, 0x06, 0xBF, 0x79, 0xDE, 0xD0, 0x11,
    0x99, 0xC9, 0x00, 0xA0, 0x24, 0x76, 0xAD, 0x4B
};

// [DECOMP] The original startup clears 0x1180 bytes of jkGuiMultiplayer_aConnections,
// i.e. 16 entries of sith_dplay_connection.
#define DPLAY_MAX_CONNECTIONS (0x1180 / sizeof(sith_dplay_connection))
#define DPLAY_MAX_SESSIONS    (32)

// [DECOMP] stdComm_Recv() dispatches non-data returns from DirectPlay_Receive:
//     2                  -> sithMulti_ProcessPlayerLost(id)
//     DPSYS_DELETEPLAYER -> (server only) sithMulti_SyncPlayers(id, 1)
// [WIRE] A vanilla host sends DSS_LEAVEJOIN (what SyncPlayers emits) to a new
// client ~11 ms after DirectPlay's CREATEPLAYER, before the client's
// DSS_JOINREQUEST, so the SyncPlayers branch is the *player created* path,
// despite OpenJKDF2's constant name. We return that value for player creation.
#define DPLAY_RET_PLAYERLOST  (2)
#define DPLAY_RET_NEWPLAYER   (DPSYS_DELETEPLAYER)

// [WIRE] dwFlags of a vanilla host's session: KEEPALIVE | NODATAMESSAGES (0xC0),
// plus PASSWORDREQUIRED (0x400) when passworded. Peer-to-peer, no host migration,
// DirectPlay protocol off (game data goes over DirectPlay's raw UDP path).
#define DPLAY_HOST_BASE_FLAGS (DPS_SESSION_KEEPALIVE | DPS_SESSION_NODATAMESSAGES)

// [DECOMP] jkMultiEntry.sessionFlags *is* DPSESSIONDESC2.dwFlags: the GUI tests
// 0x20 (DPSESSION_JOINDISABLED) for "game closed" and 0x400 for password.
// OpenJKDF2's SESSIONFLAG_ISDEDICATED (0x8000) collides with
// DPSESSION_OPTIMIZELATENCY and must never reach DirectPlay.
#define DPLAY_SESSIONFLAG_MASK (~(uint32_t)SESSIONFLAG_ISDEDICATED)
// Added: see stdComm_DPlay_SetSessionMap
#define DPLAY_SESSION_NEXTMAP 0x40000000u
// Added: an Imperial Network Protocol game whose messages use OpenJKDF2's layouts, as the
// web lobby's do (2026-10-29, so browser players can join through the relay too). Set by
// the host; a joiner uses whatever the game it joins says (older hosts: JK.EXE's).
#define DPLAY_SESSION_OPENJKDF2WIRE 0x20000000u
// Added: an Imperial Network Protocol host that serves its map and mod to joiners
// (jkweb_xfer.c over the relay, 2026-10-29). Joiners ask only such hosts.
#define DPLAY_SESSION_XFER 0x08000000u
// Added (2026-11-06): sv_strafeSpeed, an Imperial Network Protocol host's sidestep speed
// for everyone in its games, as a part of forward speed: 0.7 (the default) is JK.EXE's,
// 1.0 as fast as forward (g_bEqualStrafeSpeed, each player's own choice otherwise). In
// the game's description, in hundredths (0x00FF0000, flags DirectPlay never gets), so
// joiners (new builds) use it while in that game (stdComm_DPlay_StrafeScale,
// sithControl.c); older builds keep their own.
#define DPLAY_SESSION_STRAFE_SET   0x04000000u
#define DPLAY_SESSION_STRAFE_VALUE 0x00FF0000u
#define DPLAY_SESSION_STRAFE_SHIFT 16
#define DPLAY_SESSION_STRAFE_MASK  (DPLAY_SESSION_STRAFE_SET | DPLAY_SESSION_STRAFE_VALUE)
float stdComm_DPlay_strafeSpeed = DPLAY_STRAFE_SPEED_DEFAULT;   // sv_strafeSpeed (jkServer.c)

// sv_strafeSpeed in hundredths, 0..DPLAY_STRAFE_SPEED_MAX
static uint32_t DPlay_StrafeHundredths(void)
{
    float v = stdComm_DPlay_strafeSpeed;
    if (!(v > 0.0)) v = 0.0;   // also NaN
    if (v > DPLAY_STRAFE_SPEED_MAX) v = DPLAY_STRAFE_SPEED_MAX;
    return (uint32_t)(v * 100.0 + 0.5);
}

static uint32_t DPlay_StrafeFlags(void)
{
    return DPLAY_SESSION_STRAFE_SET | (DPlay_StrafeHundredths() << DPLAY_SESSION_STRAFE_SHIFT);
}

static int DPlay_bStarted = 0;

// Join screen's address box (Gui/jkGUIMultiplayer.c), saved as "lastConnectedHost".
extern char16_t jkGuiMultiplayer_ipText[256];
// With DirectPlay selected it's the host to search directly; with OpenJKDF2
// Online it's the relay server ("relayServer" in registry.json); with the web
// lobby it's the lobby's URL ("webLobbyUrl").
static char16_t DPlay_savedIpText[256];
static int DPlay_ipTextMode = 0;   // 0 DirectPlay, 1 relay, 2 web lobby

int stdComm_bOpenJkdf2Wire = 0;

// After restarting into a host's mod, the game to select again on the join
// screen. Statics survive the engine's in-process restart.
static GUID_idk DPlay_pendingGuid;
static int DPlay_bPendingGuid = 0;

// Added: and go on joining it without the player clicking through the menus again:
// the connection, password and character they picked before the restart.
static int DPlay_connIdx = -1;          // the connection last opened
static int DPlay_bAutoJoin = 0;         // set at the restart, until the join screen uses it
static int DPlay_bAutoJoining = 0;      // the join in progress is that one
static int DPlay_autoJoinConnIdx = -1;
static char16_t DPlay_autoJoinPassword[32];
static char16_t DPlay_autoJoinCharacter[32];
// Added: console "connect" uses the same path: its server, and whether a game is picked
// (one listed) or the player picks one on the join screen (several).
static char DPlay_autoJoinServer[256];  // relay server for the auto-join ("" = the saved one)
static int DPlay_bAutoJoinPick = 0;     // DPlay_pendingGuid is the game to join
static int DPlay_bConnectJoin = 0;      // this auto-join came from "connect", not a restart
static int DPlay_bTempServer = 0;       // the relay server in use is DPlay_autoJoinServer, not saved

static const uint8_t* DPlay_GetAppGuid(void);

// Added: on the way out of the game (exit(), whatever called it), leave the game
// in progress the way Esc -> Abort does, so the others hear of it at once instead
// of after a timeout: the relay server is told (JKR_LEAVE), DirectPlay peers get
// the player destroyed. A hosted game closes for everyone.
static void DPlay_AtExit(void)
{
    if (DPlay_bStarted && stdComm_dword_8321DC)
    {
        stdLog_Printf("net", "leaving the game (exiting)");
        dps_Close();
    }
}

static void DPlay_Dialog(const char* text)
{
    static char16_t title[] = u"Join Error";
    char16_t msg[256];
    int i = 0;
    for (; text[i] && i < 255; i++) msg[i] = (char16_t)(uint8_t)text[i];
    msg[i] = 0;
    jkGuiDialog_ErrorDialog(title, msg);
}

// Our running mod as the browser build would see it: the -path / Expansions
// folder, or else a single non-original GOB the engine always loads from
// resource/ or mods/ (the browser counts each such GOB as a mod). 0 if none.
// A mod downloaded from a host keeps its name (its netmods/ folder is named
// by fingerprint). Statics survive the engine's in-process restart.
static char DPlay_netmodPath[256];
static char DPlay_netmodName[JKMOD_MAX_NAME + 1];

// Also gives where the mod is (a folder, one file, or the set of always-loaded GOBs: *isFile) when asked.
static int DPlay_OurModPath(uint8_t fp[32], char* name, size_t nameLen, char* modPath, size_t modPathLen, int* isFile)
{
    char path[512];
    int n;
    if (name && nameLen) name[0] = 0;
    if (modPath && modPathLen) modPath[0] = 0;
    if (Main_path[0])
    {
        if (name)
            snprintf(name, nameLen, "%s", DPlay_netmodPath[0] && !strcmp(Main_path, DPlay_netmodPath)
                     ? DPlay_netmodName : jkmod_BaseName(Main_path));
        if (modPath) snprintf(modPath, modPathLen, "%s", Main_path);
        if (isFile) *isFile = 0;
        return jkmod_FingerprintCached(Main_path, fp);
    }
    n = jkmod_FindAlwaysLoaded(Main_bMotsCompat, path, sizeof(path));
    if (n == 1)
    {
        if (modPath) snprintf(modPath, modPathLen, "%s", path);
        if (isFile) *isFile = 1;
        if (name)
        {
            char* dot;
            snprintf(name, nameLen, "%s", jkmod_BaseName(path));
            dot = strrchr(name, '.');
            if (dot) *dot = 0;
        }
        return jkmod_FingerprintFileCached(path, fp);
    }
    if (n > 1)
    {
        // Added: several make one mod, as the browser build's mods ticked together do
        // (one folder, fingerprinted as a pack of its files; jkmod_FingerprintAlwaysLoaded),
        // named "A + B + C" as the browser's page names them. Hashed once per set of files.
        static char cachedKey[1024];
        static uint8_t cachedFp[32];
        static int cachedOk;
        char key[1024] = "", one[512];
        for (int i = 0; jkmod_GetAlwaysLoaded(Main_bMotsCompat, i, one, sizeof(one)); i++)
        {
            size_t at = strlen(key);
            snprintf(key + at, sizeof(key) - at, "%s|", one);
        }
        if (strcmp(key, cachedKey))
        {
            snprintf(cachedKey, sizeof(cachedKey), "%s", key);
            cachedOk = jkmod_FingerprintAlwaysLoaded(Main_bMotsCompat, cachedFp);
            stdPlatform_Printf("OpenJKDF2: %d extra GOBs in mods/ and resource/ run as one mod%s\n", n,
                               cachedOk ? "" : ", but they couldn't be read");
        }
        if (name)
        {
            name[0] = 0;
            for (int i = 0; jkmod_GetAlwaysLoaded(Main_bMotsCompat, i, one, sizeof(one)); i++)
            {
                char base[256];
                char* dot;
                size_t at = strlen(name);
                snprintf(base, sizeof(base), "%s", jkmod_BaseName(one));
                dot = strrchr(base, '.');
                if (dot) *dot = 0;
                snprintf(name + at, nameLen - at, "%s%s", i ? " + " : "", base);
            }
        }
        if (modPath && modPathLen) snprintf(modPath, modPathLen, "%s", "mods+resource");   // a label; isFile says where
        if (isFile) *isFile = Main_bMotsCompat ? JKMOD_SRC_ALWAYS_MOTS : JKMOD_SRC_ALWAYS_JK;
        if (cachedOk) memcpy(fp, cachedFp, 32);
        return cachedOk;
    }
    return 0;
}

static int DPlay_OurMod(uint8_t fp[32], char* name, size_t nameLen)
{
    return DPlay_OurModPath(fp, name, nameLen, NULL, 0, NULL);
}

// The MotS Remastered pack, if this engine run loads it: its fingerprint as
// the browser build takes it (mods.js). The browser counts the pack as part
// of the game, not as a mod. 0 if we don't run it.
static int DPlay_OurPack(uint8_t fp[JKMOD_PACK_FP])
{
    char path[512];
    if (!Main_bMotsCompat || jkRes_bSkipRemasterPack) return 0;
    if (!jkmod_FindRemasterPack(path, sizeof(path))) return 0;
    return jkmod_PackFingerprintCached(path, fp);
}

// The pack on disk (loaded or not) and its fingerprint. 0 if there's none.
static int DPlay_LocalPack(uint8_t fp[JKMOD_PACK_FP])
{
    char path[512];
    if (!Main_bMotsCompat || !jkmod_FindRemasterPack(path, sizeof(path))) return 0;
    return jkmod_PackFingerprintCached(path, fp);
}

// Added: joining again after restarting into the host's mod (see DPlay_bAutoJoin)
int stdComm_DPlay_AutoJoinPending(void)
{
    return DPlay_bAutoJoin;
}

int stdComm_DPlay_AutoJoinConnection(void)
{
    return DPlay_autoJoinConnIdx;
}

const uint16_t* stdComm_DPlay_AutoJoinPassword(void)
{
    return (const uint16_t*)DPlay_autoJoinPassword;
}

const uint16_t* stdComm_DPlay_AutoJoinCharacter(void)
{
    return (const uint16_t*)DPlay_autoJoinCharacter;
}

// The listed game being joined, or -1 while it isn't listed (yet).
int stdComm_DPlay_AutoJoinFind(void)
{
    if (!DPlay_bAutoJoinPick) return -1;
    for (uint32_t i = 0; i < dplay_dword_55D618; i++)
        if (!memcmp(&jkGuiMultiplayer_aEntries[i].guidInstance, &DPlay_pendingGuid, sizeof(DPlay_pendingGuid)))
            return (int)i;
    return -1;
}

// bJoin: the join screen is joining it now; otherwise it gave up (cancelled, not listed).
void stdComm_DPlay_AutoJoinDone(int bJoin)
{
    DPlay_bAutoJoin = 0;
    // "connect" may still restart into the host's mod once, like a join from the list.
    DPlay_bAutoJoining = bJoin && !DPlay_bConnectJoin;
    DPlay_bConnectJoin = 0;
}

static char DPlay_lastServer[256];   // (stdComm_DPlay_Reconnect, below)

// Added: console "password" (Main/jkServer.c): the hosted game's password, "" for none.
int stdComm_DPlay_SetPassword(const char* pPassword)
{
    dps_session s;
    if (!sithNet_isMulti || !sithNet_isServer || FAILED(dps_GetSessionDesc(&s)))
        return 0;
    memset(s.password, 0, sizeof(s.password));
    for (int i = 0; pPassword && pPassword[i] && i + 1 < DPS_PASSWORD_MAX; i++) s.password[i] = (uint8_t)pPassword[i];
    if (s.password[0]) s.flags |= DPS_SESSION_PASSWORDREQUIRED;
    else s.flags &= ~(uint32_t)DPS_SESSION_PASSWORDREQUIRED;
    return SUCCEEDED(dps_SetSessionDesc(&s));
}

// Added: why the relay server ended our last game, when it said (kicked or banned):
// told in the console then, and in a dialog back on the main menu
// (stdComm_DPlay_TakeLeaveMessage).
static char16_t DPlay_aLeaveMessage[128];
static int DPlay_bRejoinAsked = 0; // Added: the server's admin asked everyone to join again (reconnectall)

static void DPlay_ReasonText(int reason, int minutes, int bRefused, char* pOut, int outLen)
{
    if (reason == JKR_LOST_KICKED)
        stdString_snprintf(pOut, outLen, "You were kicked from the server.");
    else if (bRefused)
        stdString_snprintf(pOut, outLen, minutes ? "You are banned from this server for %d more minute%s." : "You are banned from this server.",
                           minutes, minutes == 1 ? "" : "s");
    else
        stdString_snprintf(pOut, outLen, minutes ? "You were banned from this server for %d minute%s." : "You were banned from this server.",
                           minutes, minutes == 1 ? "" : "s");
}

static void DPlay_NoteLeaveReason(void)
{
    char aMsg[128];
    int minutes = 0, reason;
    if (!dps_IsRelayActive()) return;
    reason = dps_RelayLostReason(&minutes);
    if (reason == JKR_LOST_RECONNECT)
    {
        // No dialog: back on the main menu we join again (stdComm_DPlay_TakeRejoinAsked).
        DPlay_bRejoinAsked = 1;
        sithConsole_PrintString("The server asked everyone to reconnect; joining again...");
        stdLog_Printf("net", "the server asked everyone to reconnect");
        return;
    }
    if (reason != JKR_LOST_KICKED && reason != JKR_LOST_BANNED) return;
    DPlay_ReasonText(reason, minutes, 0, aMsg, sizeof(aMsg));
    stdString_CharToWchar(DPlay_aLeaveMessage, aMsg, 127);
    DPlay_aLeaveMessage[127] = 0;
    sithConsole_PrintString(aMsg);
    sithConsole_AlertSound();
    stdLog_Printf("net", "%s", aMsg);
}

// Added: whether the server asked us to join its game again (Take: once).
int stdComm_DPlay_RejoinAsked(void)
{
    return DPlay_bRejoinAsked;
}

int stdComm_DPlay_TakeRejoinAsked(void)
{
    int r = DPlay_bRejoinAsked;
    DPlay_bRejoinAsked = 0;
    return r;
}

int stdComm_DPlay_TakeLeaveMessage(uint16_t* pOut, int outLen)
{
    if (!DPlay_aLeaveMessage[0] || outLen <= 0) return 0;
    stdString_SafeWStrCopy((char16_t*)pOut, DPlay_aLeaveMessage, outLen);
    DPlay_aLeaveMessage[0] = 0;
    return 1;
}

// Added: why the relay server refused our last join, when it said (banned). 0 if it didn't.
int stdComm_DPlay_JoinRefusalText(uint16_t* pOut, int outLen)
{
    char aMsg[128];
    int minutes = 0;
    if (outLen <= 0 || !dps_IsRelayActive() || dps_RelayJoinRefusal(&minutes) != JKR_LOST_BANNED) return 0;
    DPlay_ReasonText(JKR_LOST_BANNED, minutes, 1, aMsg, sizeof(aMsg));
    stdString_CharToWchar((char16_t*)pOut, aMsg, outLen - 1);
    pOut[outLen - 1] = 0;
    return 1;
}

// Added: server "rank": the highest force rank a joining character may have, in the
// game's description (high word of dwUser3, which the join screen and "connect" check).
int stdComm_DPlay_SetMaxRank(int rank)
{
    dps_session s;
    if (!sithNet_isMulti || !sithNet_isServer || FAILED(dps_GetSessionDesc(&s)))
        return 0;
    s.user[2] = (s.user[2] & 0xFFFF) | ((uint32_t)rank << 16);
    return SUCCEEDED(dps_SetSessionDesc(&s));
}

// Added: where console "rcon" goes by default: the dedicated server of the game we're in
// (a listed server's own address), else the last one joined. "" if none.
void stdComm_DPlay_RconServer(char* pOut, int outLen)
{
    if (outLen <= 0) return;
    pOut[0] = 0;
    if (sithNet_isMulti && stdComm_DPlay_ServerKind() == 1)
        stdString_SafeStrCopy(pOut, dps_RelayCurrentServer(), outLen);
    else if (DPlay_lastServer[0])
        stdString_SafeStrCopy(pOut, DPlay_lastServer, outLen);
    else
        wuRegistry_GetStr("lastGameServer", pOut, outLen, "");
}

// Added: the force level limit of the game we're in (console "character"), -1 if none.
int stdComm_DPlay_GameMaxRank(void)
{
    dps_session s;
    if (!sithNet_isMulti || !stdComm_dword_8321DC || FAILED(dps_GetSessionDesc(&s)))
        return -1;
    return (int)(s.user[2] >> 16);
}

// Added: the game's name as hosted ("gameinfo"): the session name up to its first ':'
// ("<game name>:<episode gob>:<map jkl>"). "" if not in a game.
void stdComm_DPlay_GetGameName(char* pOut, int outLen)
{
    dps_session s;
    int k = 0;
    if (outLen <= 0) return;
    pOut[0] = 0;
    if (!sithNet_isMulti || !stdComm_dword_8321DC || FAILED(dps_GetSessionDesc(&s)))
        return;
    for (; s.name[k] && s.name[k] != ':' && k + 1 < outLen; k++)
        pOut[k] = (s.name[k] < 0x80) ? (char)s.name[k] : '?';
    pOut[k] = 0;
}

int stdComm_DPlay_Listen(int port, int wsPort, const char* pMaster)
{
    return dps_RelayListen(port, wsPort, pMaster);
}

// Whether the auto-join waits for one particular game (else the player picks).
int stdComm_DPlay_AutoJoinPicks(void)
{
    return DPlay_bAutoJoin && DPlay_bAutoJoinPick;
}

// The relay connection's index in jkGuiMultiplayer_aConnections, or -1.
static int DPlay_FindRelayConnection(void)
{
    for (int i = 0; i < (int)jkGuiMultiplayer_numConnections; i++)
    {
        const sith_dplay_connection* c = &jkGuiMultiplayer_aConnections[i];
        if (c->connection && c->connectionSize == 4 && !memcmp(c->connection, "JKR1", 4)) return i;
    }
    return -1;
}

// The game name part of a session name ("<game name>:<episode gob>:<map jkl>").
static void DPlay_GameNameOf(const uint16_t* pSessionName, char* pOut, size_t outLen)
{
    size_t k = 0;
    for (; pSessionName[k] && pSessionName[k] != ':' && k + 1 < outLen; k++)
        pOut[k] = (pSessionName[k] < 0x80) ? (char)pSessionName[k] : '?';
    pOut[k] = 0;
}

// Added: the last game joined on the Imperial Network Protocol, for console "reconnect"
// (server and game name also saved as "lastGameServer"/"lastGameName"; the password only
// in memory). Set by stdComm_Open.
static char DPlay_lastServer[256];
static char DPlay_lastGameName[64];
static GUID_idk DPlay_lastGuid;
static int DPlay_bLastGuid = 0;

// Added: the game rcon is for (the one we're in, or were last in), so a relay server
// can pass it to that game's host (jkrelay_proto.h, "Rcon through a relay"). 0 if none.
int stdComm_DPlay_RconGuid(uint8_t guid[16])
{
    dps_session s;
    if (sithNet_isMulti && stdComm_DPlay_ServerKind() == 1 && SUCCEEDED(dps_GetSessionDesc(&s)))
    {
        memcpy(guid, s.guidInstance, 16);
        return 1;
    }
    if (!DPlay_bLastGuid) return 0;
    memcpy(guid, &DPlay_lastGuid, 16);
    return 1;
}

// Added: a dedicated server hosting on a relay server gets its rcon through it, and has
// it enforce its kicks and bans (Main.c).
void stdComm_DPlay_StartTunnel(void)
{
    dps_ServerTunnelStart();
}
static char16_t DPlay_lastPassword[32];

// Sets up the auto-join of a game on 'server' (jkGuiMain_ConnectNow then goes to it):
// 'pick' to join it straight away with that character, NULL for the join screen.
static void DPlay_StartAutoJoin(int connIdx, const char* server, const char16_t* pPassword, const char* pCharacter,
                                const dps_session* pick)
{
    DPlay_bAutoJoin = 1;
    DPlay_bConnectJoin = 1;
    DPlay_autoJoinConnIdx = connIdx;
    stdString_SafeStrCopy(DPlay_autoJoinServer, server, sizeof(DPlay_autoJoinServer));
    memset(DPlay_autoJoinPassword, 0, sizeof(DPlay_autoJoinPassword));
    for (int i = 0; pPassword && i < 31 && pPassword[i]; i++) DPlay_autoJoinPassword[i] = pPassword[i];
    // A game picked: straight in with that character. Else the player picks a game
    // on the join screen, then a character on the character screen (it knows the
    // game's force level limit).
    memset(DPlay_autoJoinCharacter, 0, sizeof(DPlay_autoJoinCharacter));
    for (int i = 0; pick && pCharacter && i < 31 && pCharacter[i]; i++) DPlay_autoJoinCharacter[i] = (uint8_t)pCharacter[i];
    DPlay_bAutoJoinPick = pick != NULL;
    if (pick)
    {
        memcpy(&DPlay_pendingGuid, pick->guidInstance, sizeof(DPlay_pendingGuid));
        DPlay_bPendingGuid = 1;
    }
}

// Asks 'server' for its games (up to max in pOut). Returns how many, or 0 with pMsg set.
static int DPlay_ProbeGames(const char* server, dps_session* pOut, int max, char* pMsg, int msgLen)
{
    int n = dps_RelayProbe(server, DPlay_GetAppGuid(), 2500, pOut, max);
    if (n < 0)
        snprintf(pMsg, msgLen, "No answer from %s.", server);
    else if (n == 0)
        snprintf(pMsg, msgLen, "%s answers, but no game is running on it.", server);
    return n > 0 ? (n < max ? n : max) : 0;
}

static int DPlay_RankAllowed(const dps_session* s, const char* pCharacter, int charRank, char* pMsg, int msgLen)
{
    int maxRank = (int)(s->user[2] >> 16);
    if (charRank <= maxRank) return 1;
    snprintf(pMsg, msgLen, "%s is force level %d; that game allows up to %d. Pick another with \"character\".",
             pCharacter, charRank, maxRank);
    return 0;
}

// Added: console "connect <host[:port]> [password]": asks the server for its games and,
// if it answers, joins through the menus (jkGuiMain -> Multiplayer -> join screen) with
// the character picked with the console's "character" (pCharacter, its force level
// charRank). Port JKR_DEFAULT_SERVER_PORT (a dedicated server's own) if none given.
int stdComm_DPlay_Connect(const char* pAddress, const char* pPassword, const char* pCharacter, int charRank,
                          char* pMsg, int msgLen)
{
    char server[256], name[64];
    char16_t password[32] = { 0 };
    dps_session games[DPLAY_MAX_SESSIONS];
    int idx, n;

    pMsg[0] = 0;
    idx = DPlay_FindRelayConnection();
    if (!DPlay_bStarted || idx < 0)
    {
        snprintf(pMsg, msgLen, "This build has no Imperial Network Protocol connection.");
        return 0;
    }
    if (strchr(pAddress, ':'))
        stdString_SafeStrCopy(server, pAddress, sizeof(server));
    else
        snprintf(server, sizeof(server), "%s:%d", pAddress, JKR_DEFAULT_SERVER_PORT);

    n = DPlay_ProbeGames(server, games, DPLAY_MAX_SESSIONS, pMsg, msgLen);
    if (!n) return 0;
    if (n == 1 && !DPlay_RankAllowed(&games[0], pCharacter, charRank, pMsg, msgLen))
        return 0;
    for (int i = 0; pPassword && i < 31 && pPassword[i]; i++) password[i] = (uint8_t)pPassword[i];
    DPlay_StartAutoJoin(idx, server, password, pCharacter, n == 1 ? &games[0] : NULL);
    if (n == 1)
    {
        DPlay_GameNameOf(games[0].name, name, sizeof(name));
        snprintf(pMsg, msgLen, "Joining \"%s\" on %s as %s...", name, server, pCharacter);
    }
    else
        snprintf(pMsg, msgLen, "%d games on %s: pick one on the join screen.", n, server);
    stdLog_Printf("net", "connect %s: %d game%s listed", server, n, n == 1 ? "" : "s");
    return 1;
}

// Added: console "reconnect": joins the last game joined on the Imperial Network
// Protocol again (stdComm_Open remembers it): the same game if it's still there,
// else the server's only game or the one with the same name (a restarted server's
// game is a new one). bAsked: the server asked us to (reconnectall): a character above
// the game's force rank limit, or none, means the character screen instead of refusing.
int stdComm_DPlay_Reconnect(const char* pCharacter, int charRank, int bAsked, char* pMsg, int msgLen)
{
    dps_session games[DPLAY_MAX_SESSIONS];
    const dps_session* pick = NULL;
    char name[64];
    int idx, n;

    pMsg[0] = 0;
    idx = DPlay_FindRelayConnection();
    if (!DPlay_bStarted || idx < 0)
    {
        snprintf(pMsg, msgLen, "This build has no Imperial Network Protocol connection.");
        return 0;
    }
    if (!DPlay_lastServer[0])
    {
        wuRegistry_GetStr("lastGameServer", DPlay_lastServer, sizeof(DPlay_lastServer), "");
        wuRegistry_GetStr("lastGameName", DPlay_lastGameName, sizeof(DPlay_lastGameName), "");
    }
    if (!DPlay_lastServer[0])
    {
        snprintf(pMsg, msgLen, "No game to reconnect to yet (only Imperial Network Protocol games count).");
        return 0;
    }

    n = DPlay_ProbeGames(DPlay_lastServer, games, DPLAY_MAX_SESSIONS, pMsg, msgLen);
    if (!n) return 0;
    for (int i = 0; i < n && !pick; i++)
        if (DPlay_bLastGuid && !memcmp(games[i].guidInstance, &DPlay_lastGuid, sizeof(DPlay_lastGuid))) pick = &games[i];
    if (!pick && n == 1)
        pick = &games[0];
    for (int i = 0; i < n && !pick; i++)
    {
        DPlay_GameNameOf(games[i].name, name, sizeof(name));
        if (DPlay_lastGameName[0] && !strcmp(name, DPlay_lastGameName)) pick = &games[i];
    }
    if (!pick)
    {
        snprintf(pMsg, msgLen, "\"%s\" isn't on %s any more.", DPlay_lastGameName, DPlay_lastServer);
        return 0;
    }
    DPlay_GameNameOf(pick->name, name, sizeof(name));
    if (bAsked && (!pCharacter || !pCharacter[0] || charRank > (int)(pick->user[2] >> 16)))
    {
        DPlay_StartAutoJoin(idx, DPlay_lastServer, DPlay_lastPassword, NULL, pick); // the character screen
        snprintf(pMsg, msgLen, "Joining \"%s\" on %s again: pick a character (force rank up to %d).", name,
                 DPlay_lastServer, (int)(pick->user[2] >> 16));
        stdLog_Printf("net", "reconnect %s (asked)", DPlay_lastServer);
        return 1;
    }
    if (!DPlay_RankAllowed(pick, pCharacter, charRank, pMsg, msgLen))
        return 0;
    DPlay_StartAutoJoin(idx, DPlay_lastServer, DPlay_lastPassword, pCharacter, pick);
    snprintf(pMsg, msgLen, "Joining \"%s\" on %s as %s again...", name, DPlay_lastServer, pCharacter);
    stdLog_Printf("net", "reconnect %s", DPlay_lastServer);
    return 1;
}

// Added: net_askBeforeDownload, ask before getting a web lobby host's map or mod (game folder cvar)
static int DPlay_bAskBeforeDownload = 1;

// Added: net_directLinks, Imperial Network Protocol games go straight between the
// host and each joiner when they can (0: always through the relay server, so
// nobody sees anyone else's IP address)
static int DPlay_bDirectLinks = 1;

// Added: net_masterServers, the master servers whose Imperial Network Protocol games the join
// screen lists along with the relay server's: "host[:port]", separated by commas or spaces
// (jkrelay_client.c, jkr_AddMasterLists). Players who never changed it get new defaults.
#define DPLAY_DEFAULT_MASTER_SERVERS "master.jkjunkie.com"
static char DPlay_aMasterServers[SITHCVAR_MAX_STRLEN] = DPLAY_DEFAULT_MASTER_SERVERS; // the cvar copies its value in

void stdComm_DPlay_StartupCvars(void)
{
    sithCvar_RegisterBool("net_askBeforeDownload", 1, &DPlay_bAskBeforeDownload, CVARFLAG_GLOBAL);
    sithCvar_RegisterBool("net_directLinks", 1, &DPlay_bDirectLinks, CVARFLAG_GLOBAL);
    sithCvar_RegisterStr("net_masterServers", DPLAY_DEFAULT_MASTER_SERVERS, DPlay_aMasterServers, CVARFLAG_GLOBAL | CVARFLAG_UPDATABLE_DEFAULT);
    stdComm_Hits_StartupCvars(); // Added: sv_hitRules, sv_hitReach
}

#ifdef PLATFORM_CURL
// Web lobby downloads (jkweb_xfer.c) behind a progress dialog.
static int DPlay_XferTick(void* ctx, char16_t* text, int maxChars)
{
    int r = jkx_Tick();
    const char* s = jkx_Status();
    int i = 0;
    (void)ctx;
    for (; s[i] && i < maxChars - 1; i++) text[i] = (char16_t)(uint8_t)s[i];
    text[i] = 0;
    if (r == JKX_ASK) return 2; // closes the dialog for the question
    return r == JKX_READY ? 1 : (r == JKX_FAILED ? -1 : 0);
}

// Runs the download jkx_Start*() began. 1 when it's in place; otherwise the
// player has been told why (or cancelled) and 0 is returned.
static int DPlay_RunXfer(int kind)
{
    static char16_t title[64];
    const char* t = kind == JKX_KIND_MOD ? "Getting the host's mod" : "Getting the host's map";
    int i = 0, r;
    jkx_SetAskFirst(DPlay_bAskBeforeDownload); // Added: read by the first tick that hears from the host
    for (; t[i]; i++) title[i] = (char16_t)t[i];
    title[i] = 0;
    for (;;)
    {
        r = jkGuiDialog_ProgressDialog(title, DPlay_XferTick, NULL);
        if (r != 2) break;

        // Added: the host has said what it would send, ask before downloading
        {
            static char16_t askTitle[] = u"Download?";
            char16_t msg[320];
            const char* q = jkx_AskText();
            int n = 0;
            for (; q[n] && n < 319; n++) msg[n] = (char16_t)(uint8_t)q[n];
            msg[n] = 0;
            jkx_Answer(jkGuiDialog_YesNoDialog(askTitle, msg));
        }
        if (jkx_Tick() == JKX_FAILED) return 0; // said no
    }
    if (r == 1) return 1;
    if (r == -2) jkx_Cancel();
    // Added (2026-11-08): a map or mod in the host's noshare.txt: no dialog. The join goes on
    // as with a host that sends nothing: with the player's own copy if they have one, else
    // the game's own "can't load level" when the level loads. (A mod: see DPlay_MatchHostMod.)
    if (r != -2 && !strcmp(jkx_Reason(), "not-shared"))
        return kind == JKX_KIND_MAP;
    if (jkx_Message()[0]) DPlay_Dialog(jkx_Message());
    return 0;
}
#endif

// Added: sv_downloadRate, KB/s a host sends maps and mods at, all joiners together (below)
int stdComm_DPlay_downloadRateKB = DPLAY_XFER_RATE_DEFAULT_KB;

#ifdef PLATFORM_CURL
// ---------------------------------------------------------------------------
// Added: map and mod downloads on the Imperial Network Protocol (2026-10-29): the
// web lobby's transfers (jkweb_xfer.c, as maps.js/mods.js) between machines in a
// relay game, as big frames (dps_RelaySendBig). jkweb_xfer.c knows players as the
// lobby numbers them, the host 1 and joiners 2..64, so a host gives each joiner's
// machine a number. UDP has no back-pressure, and a chunk with a lost piece is only
// asked for again after 5 s, so a host paces what it sends.
// Altered (2026-11-05): was a fixed 1 MB/s for all joiners together. Now sv_downloadRate
// (KB/s, all joiners together; stdComm_DPlay_downloadRateKB) is shared fairly between
// the joiners downloading, and each joiner has its own rate: it starts at
// DPLAY_XFER_START_KB, rises while nothing is lost and drops when a block it was
// sent is asked for again (jkweb_XferNoteLoss). Host side only: joiners
// see the same frames, just sooner.
// ---------------------------------------------------------------------------
// Altered (2026-11-05): parts of DPLAY_XFER_PART and at most DPLAY_XFER_BURST at once.
// Joiners ask for 64 KiB parts, and a host sent them back to back: 100-200 KB within a
// millisecond, of which some was regularly lost on the way (on jkjunkie.com: the last
// part of a block, the first two of the next), and each loss cost the joiner a 5 s wait
// (the "frozen" downloads). Joiners take any multiple of 16 KiB (jkweb_xfer.c, maps.js).
#define DPLAY_XFER_PART        (16 * 1024)
#define DPLAY_XFER_BURST       (32 * 1024)
#define DPLAY_XFER_RAISE_MS    500    // a joiner without losses gets faster this often...
#define DPLAY_XFER_RAISE_PARTS 16     // ...by this part of sv_downloadRate
#define DPLAY_XFER_LOSS_GAP_MS 5000   // one slowdown per joiner's wait before asking again
#define DPLAY_XFER_ACTIVE_MS   1500   // a joiner sent to this recently shares the rate

typedef struct
{
    double   tokens;      // bytes it may send now (negative: sent ahead)
    double   rate;        // its own rate, bytes a second
    uint32_t lastMs;      // tokens last refilled
    uint32_t sentMs;      // last sent to, 0 never
    uint32_t raisedMs;    // rate last raised
    uint32_t lossMs;      // rate last halved
} DPlay_xferPeer;

static uint32_t DPlay_aXferSys[65];  // [2..64]: the machine a number stands for
static DPlay_xferPeer DPlay_aXferPeer[65];
static int DPlay_nextXferAlias = 2;
static double DPlay_xferTokens = DPLAY_XFER_BURST;
static uint32_t DPlay_xferTokensMs = 0;

static double DPlay_XferTotalRate(void)
{
    int kb = stdComm_DPlay_downloadRateKB;
    if (kb < DPLAY_XFER_MIN_KB) kb = DPLAY_XFER_MIN_KB;
    if (kb > DPLAY_XFER_MAX_KB) kb = DPLAY_XFER_MAX_KB;
    return kb * 1024.0;
}

static uint32_t DPlay_XferAliasOf(uint32_t sysId)
{
    for (int i = 2; i <= 64; i++)
        if (DPlay_aXferSys[i] == sysId) return (uint32_t)i;
    int a = DPlay_nextXferAlias;
    DPlay_nextXferAlias = a >= 64 ? 2 : a + 1;
    DPlay_aXferSys[a] = sysId;
    memset(&DPlay_aXferPeer[a], 0, sizeof(DPlay_aXferPeer[a]));   // Added: a new machine starts slow
    DPlay_aXferPeer[a].rate = DPLAY_XFER_START_KB * 1024.0;
    return (uint32_t)a;
}

static void DPlay_RelayBigFrame(uint32_t from, const uint8_t* p, size_t n)
{
    if (n < 16 || memcmp(p + 8, "JKMT", 4)) return;
    if (dps_RelayIsHost())
        jkx_OnFrame(DPlay_XferAliasOf(from), p, n, JKX_VIA_DIRECT);
    else if (from == dps_RelayHostSystemId())
        jkx_OnFrame(1, p, n, JKX_VIA_DIRECT);
}

static void DPlay_XferRefill(void)
{
    uint32_t now = stdPlatform_GetTimeMsec();
    DPlay_xferTokens += (double)(now - DPlay_xferTokensMs) * (DPlay_XferTotalRate() / 1000.0); // Altered: sv_downloadRate
    if (DPlay_xferTokens > DPLAY_XFER_BURST) DPlay_xferTokens = DPLAY_XFER_BURST;
    DPlay_xferTokensMs = now;
}

// Added: a joiner's own tokens, at its rate or its share of sv_downloadRate, whichever
// is less. Its rate rises while it's downloading without losses.
static DPlay_xferPeer* DPlay_XferPeerRefill(uint32_t to)
{
    DPlay_xferPeer* pPeer;
    uint32_t now = stdPlatform_GetTimeMsec();
    double total = DPlay_XferTotalRate(), share, rate;
    int active = 0;
    if (to < 2 || to > 64 || !dps_RelayIsHost()) return NULL;
    pPeer = &DPlay_aXferPeer[to];
    for (int i = 2; i <= 64; i++)
        if (DPlay_aXferSys[i] && DPlay_aXferPeer[i].sentMs && now - DPlay_aXferPeer[i].sentMs < DPLAY_XFER_ACTIVE_MS)
            active++;
    share = total / (active > 1 ? active : 1);
    if (pPeer->rate < DPLAY_XFER_MIN_KB * 1024.0) pPeer->rate = DPLAY_XFER_START_KB * 1024.0;
    if (pPeer->sentMs && now - pPeer->sentMs < DPLAY_XFER_ACTIVE_MS && now - pPeer->raisedMs >= DPLAY_XFER_RAISE_MS
        && now - pPeer->lossMs >= DPLAY_XFER_RAISE_MS)
    {
        pPeer->rate += total / DPLAY_XFER_RAISE_PARTS;
        pPeer->raisedMs = now;
    }
    if (pPeer->rate > total) pPeer->rate = total;
    rate = pPeer->rate < share ? pPeer->rate : share;
    if (!pPeer->lastMs) pPeer->lastMs = now;
    pPeer->tokens += (double)(now - pPeer->lastMs) * rate / 1000.0;
    if (pPeer->tokens > DPLAY_XFER_BURST) pPeer->tokens = DPLAY_XFER_BURST;
    pPeer->lastMs = now;
    return pPeer;
}

// Added: jkweb_xfer.c's host saw a joiner ask again for a block it had sent, so some of
// it was lost on the way: that joiner's rate drops by a quarter (once per wait).
void jkweb_XferNoteLoss(uint32_t to)
{
    DPlay_xferPeer* pPeer;
    uint32_t now = stdPlatform_GetTimeMsec();
    if (!dps_IsRelayActive() || !(pPeer = DPlay_XferPeerRefill(to))) return;   // the lobby paces itself
    if (pPeer->lossMs && now - pPeer->lossMs < DPLAY_XFER_LOSS_GAP_MS) return;
    pPeer->rate *= 0.75;   // losses on the internet aren't all from going too fast
    if (pPeer->rate < DPLAY_XFER_MIN_KB * 1024.0) pPeer->rate = DPLAY_XFER_MIN_KB * 1024.0;
    pPeer->lossMs = now ? now : 1;
    stdPlatform_Printf("OpenJKDF2: download to player %u lost data, now %d KB/s\n", to, (int)(pPeer->rate / 1024.0));
}

// jkweb_xfer.c's transport: the web lobby's, or the relay's.
int jkweb_XferSend(uint32_t to, const uint8_t* frame, size_t n, int allowDirect)
{
    uint32_t dst;
    if (!dps_IsRelayActive()) return jkweb_LobbyXferSend(to, frame, n, allowDirect);
    dst = dps_RelayIsHost() ? (to >= 2 && to <= 64 ? DPlay_aXferSys[to] : 0) : (to == 1 ? dps_RelayHostSystemId() : 0);
    if (!dst) return 0;
    DPlay_XferRefill();
    DPlay_xferTokens -= (double)n;
    {
        // Added: what this joiner was sent, against its own rate
        DPlay_xferPeer* pPeer = DPlay_XferPeerRefill(to);
        if (pPeer)
        {
            uint32_t now = stdPlatform_GetTimeMsec();
            pPeer->tokens -= (double)n;
            pPeer->sentMs = now ? now : 1;
        }
    }
    return dps_RelaySendBig(dst, frame, n) ? JKX_VIA_DIRECT : 0;
}

// Added: the largest part of a block a host sends at once on the relay (0: as asked).
uint32_t jkweb_XferPartMax(void) { return dps_IsRelayActive() ? DPLAY_XFER_PART : 0; }

int jkweb_XferCanSend(uint32_t to, int allowDirect)
{
    DPlay_xferPeer* pPeer;
    if (!dps_IsRelayActive()) return jkweb_LobbyXferCanSend(to, allowDirect);
    DPlay_XferRefill();
    pPeer = DPlay_XferPeerRefill(to);   // Added: and the joiner's own rate
    return DPlay_xferTokens > 0 && (!pPeer || pPeer->tokens > 0);
}

// The relay's frames are as small as the lobby's direct links ask for (16 KiB chunks).
int jkweb_XferDirectUp(void) { return dps_IsRelayActive() ? 1 : jkweb_LobbyXferDirectUp(); }
int jkweb_XferDirectPossible(void) { return dps_IsRelayActive() ? 1 : jkweb_LobbyXferDirectPossible(); }

int jkweb_XferHostState(void)
{
    if (!dps_IsRelayActive()) return jkweb_LobbyXferHostState();
    return dps_RelayHostSystemId() ? 1 : 0;
}

void jkweb_XferPump(void)
{
    if (!dps_IsRelayActive()) { jkweb_LobbyXferPump(); return; }
    dps_Idle();
}
#endif

#ifdef PLATFORM_CURL
// Added: what a host offers joiners: its episode GOB (offered again whenever it
// changes, as a dedicated server's rotation goes into another episode) and its mod.
static char DPlay_aOfferedEpisode[64];

static void DPlay_HostOffer(const char* pEpisode)
{
    uint8_t fp[32];
    char name[JKMOD_MAX_NAME + 1], modPath[512];
    int isFile = 0;
    int haveMod;
    if (!pEpisode || !pEpisode[0] || !__strcmpi(pEpisode, DPlay_aOfferedEpisode))
        return;
    stdString_SafeStrCopy(DPlay_aOfferedEpisode, pEpisode, sizeof(DPlay_aOfferedEpisode));
    haveMod = DPlay_OurModPath(fp, name, sizeof(name), modPath, sizeof(modPath), &isFile);
    if (haveMod && jkx_ModNotShared(modPath, isFile))
        haveMod = 0;   // Added: a mod in noshare.txt isn't offered (nor announced, DirectPlay_OpenHost)
    jkx_HostOffer(pEpisode, JKRES_GOB_EXT, haveMod ? modPath : NULL, isFile, haveMod ? fp : NULL, name);
    stdLog_Printf("net", "offering episode %s to joiners", pEpisode);
}
#endif

// Added: a joiner following its host into another episode (jkMain_FollowHostMap): gets
// the host's copy when it lacks it or has another one. 1 when it's in place (or there's
// nothing to get), 0 if not (the player has been told why).
int stdComm_DPlay_FetchHostMap(const char* pEpisode)
{
#ifdef PLATFORM_CURL
    dps_session s;
    if (sithNet_isServer || !pEpisode || !pEpisode[0])
        return 1;
    if (dps_IsRelayActive() && !(SUCCEEDED(dps_GetSessionDesc(&s)) && (s.flags & DPLAY_SESSION_XFER)))
        return 1;
    if (!dps_IsRelayActive() && !dps_IsWebLobbyActive())
        return 1;
    jkx_StartMap(pEpisode, JKRES_GOB_EXT);
    return DPlay_RunXfer(JKX_KIND_MAP);
#else
    (void)pEpisode;
    return 1;
#endif
}

// Web lobby: make sure we run the host's mod, and the MotS Remastered pack
// if and only if the host does, before joining, like the browser build
// (mods.js runJob). Returns 0 to go ahead, or STDCOMM_DPLAY_RESTART / _HANDLED.
static int DPlay_MatchHostMod(int idx, char16_t* pwPassword)
{
    const char* tail = dps_GetSessionModTail((const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance);
    uint8_t want[32], ours[32], wantPack[JKMOD_PACK_FP], ourPack[JKMOD_PACK_FP], localPack[JKMOD_PACK_FP];
    char name[JKMOD_MAX_NAME + 1], path[256], msg[256];
    int haveOurs, havePack, flags, modOk, packOk;

    if (!tail) return 0;                     // connection without mod info
    flags = jkmod_ParseTail(tail, want, name, sizeof(name), wantPack);
    if (flags < 0)
    {
        DPlay_Dialog("This game's mod details couldn't be read. One of you may need a newer version.");
        return STDCOMM_DPLAY_HANDLED;
    }
    haveOurs = DPlay_OurMod(ours, NULL, 0);
    havePack = DPlay_OurPack(ourPack);

    if (flags & JKMOD_TAIL_PACK)
    {
        if (!DPlay_LocalPack(localPack))
        {
            DPlay_Dialog("This game uses the MotS Remastered pack, which your game folder doesn't have. "
                         "To join, put JKMRemasteredAssets.goo in your game folder's mods folder.");
            return STDCOMM_DPLAY_HANDLED;
        }
        if (memcmp(localPack, wantPack, JKMOD_PACK_FP))
        {
            DPlay_Dialog("Your MotS Remastered pack is a different version from the host's. "
                         "To join, you both need the same JKMRemasteredAssets.goo.");
            return STDCOMM_DPLAY_HANDLED;
        }
        packOk = havePack && !memcmp(ourPack, wantPack, JKMOD_PACK_FP);
    }
    else
        packOk = !havePack;

    if (flags & JKMOD_TAIL_MOD)
        modOk = haveOurs && !memcmp(ours, want, 32);
    else
        // Host runs none. A -path mod can be switched off by restarting;
        // other GOBs in resource/ and mods/ always load, so that's left as is.
        modOk = !haveOurs || !Main_path[0];

    if (modOk && packOk) return 0;

    if (modOk)
        snprintf(path, sizeof(path), "%s", Main_path);   // same mod, restart for the pack
    else if (flags & JKMOD_TAIL_MOD)
    {
        if (!jkmod_FindLocal(want, path, sizeof(path)))
        {
#ifdef PLATFORM_CURL
            // Added: an Imperial Network Protocol host that doesn't serve it
            if (dps_IsRelayActive() && !(jkGuiMultiplayer_aEntries[idx].sessionFlags & DPLAY_SESSION_XFER))
            {
                snprintf(msg, sizeof(msg), "The host runs the mod \"%s\". Put a copy of it in your "
                         "expansions folder to join.", name);
                DPlay_Dialog(msg);
                return STDCOMM_DPLAY_HANDLED;
            }
            // Get it from the host, as a browser joiner does: join its room,
            // download, leave, then restart into it.
            dps_SetRelayDirect(DPlay_bDirectLinks); // Added
            HRESULT hr = dps_OpenJoin((const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance, (const uint16_t*)pwPassword);
            int ok;
            if (FAILED(hr)) return (int)hr;
            jkx_StartMod(want, name, JKRES_GOB_EXT);
            ok = DPlay_RunXfer(JKX_KIND_MOD);
            dps_Close();
            if (!ok && !strcmp(jkx_Reason(), "not-shared"))
                return 0;   // Added: an (older) host that announces a mod it won't send: join without it
            if (!ok) return STDCOMM_DPLAY_HANDLED;
            snprintf(path, sizeof(path), "%s", jkx_ResultPath());
            snprintf(DPlay_netmodPath, sizeof(DPlay_netmodPath), "%s", path);
            snprintf(DPlay_netmodName, sizeof(DPlay_netmodName), "%s", name);
            (void)msg;
#else
            snprintf(msg, sizeof(msg), "The host runs the mod \"%s\". Put a copy of it in your "
                     "expansions folder to join.", name);
            DPlay_Dialog(msg);
            return STDCOMM_DPLAY_HANDLED;
#endif
        }
    }
    else
        path[0] = 0;

    // Added: restarted for this game already and still not matching: don't go round again
    if (DPlay_bAutoJoining)
    {
        DPlay_Dialog("The game restarted with the host's mod but still doesn't match it, so it can't join.");
        return STDCOMM_DPLAY_HANDLED;
    }

    // The pack only changes with a restart, so it goes with the mod's.
    if (!packOk)
        jkRes_bSkipRemasterPack = !(flags & JKMOD_TAIL_PACK);

    stdPlatform_Printf("OpenJKDF2: restarting with mod path \"%s\"%s to join a game in it\n", path,
                       packOk ? "" : (jkRes_bSkipRemasterPack ? " without the Remastered pack" : " with the Remastered pack"));
    stdString_SafeStrCopy(openjkdf2_aRestartPath, path, sizeof(openjkdf2_aRestartPath));
    openjkdf2_restartMode = OPENJKDF2_RESTART_PATH;
    g_should_exit = 1;
    DPlay_pendingGuid = jkGuiMultiplayer_aEntries[idx].guidInstance;
    DPlay_bPendingGuid = 1;

    // Added: join it again after the restart (jkGuiMain/jkGuiMultiplayer ask stdComm_DPlay_AutoJoin*)
    DPlay_bAutoJoin = DPlay_connIdx >= 0;
    DPlay_autoJoinConnIdx = DPlay_connIdx;
    DPlay_bAutoJoinPick = 1;
    DPlay_bConnectJoin = 0;
    stdString_SafeStrCopy(DPlay_autoJoinServer, dps_IsRelayActive() ? dps_GetRelayServer() : "", sizeof(DPlay_autoJoinServer));
    memset(DPlay_autoJoinPassword, 0, sizeof(DPlay_autoJoinPassword));
    if (pwPassword)
        for (int i = 0; i < 31 && pwPassword[i]; i++) DPlay_autoJoinPassword[i] = pwPassword[i];
    memset(DPlay_autoJoinCharacter, 0, sizeof(DPlay_autoJoinCharacter));
    for (int i = 0; i < 31 && jkGuiMultiplayer_mpcInfo.name[i]; i++)
        DPlay_autoJoinCharacter[i] = jkGuiMultiplayer_mpcInfo.name[i];
    return STDCOMM_DPLAY_RESTART;
}

void stdComm_DPlay_DescribeMod(int idx, uint16_t* pOut, int maxChars)
{
    char text[160] = "";
    if (idx >= 0 && idx < (int)dplay_dword_55D618 && maxChars > 0)
    {
        const char* tail = dps_GetSessionModTail((const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance);
        uint8_t want[32], ours[32], wantPack[JKMOD_PACK_FP], ourPack[JKMOD_PACK_FP], localPack[JKMOD_PACK_FP];
        char name[JKMOD_MAX_NAME + 1], path[256], mod[120] = "";
        int flags = tail ? jkmod_ParseTail(tail, want, name, sizeof(name), wantPack) : 0;
        int havePack = tail ? DPlay_OurPack(ourPack) : 0;   // only connections with mod info

        if (flags < 0)
            snprintf(text, sizeof(text), "Mod: unknown");
        else if ((flags & JKMOD_TAIL_PACK) && !DPlay_LocalPack(localPack))
            snprintf(text, sizeof(text), "Needs the MotS Remastered pack");
        else if ((flags & JKMOD_TAIL_PACK) && memcmp(localPack, wantPack, JKMOD_PACK_FP))
            snprintf(text, sizeof(text), "Needs the host's version of the Remastered pack");
        else
        {
            if (flags & JKMOD_TAIL_MOD)
            {
                if (DPlay_OurMod(ours, NULL, 0) && !memcmp(ours, want, 32)) snprintf(mod, sizeof(mod), "Mod: %s (running)", name);
                else if (jkmod_FindLocal(want, path, sizeof(path))) snprintf(mod, sizeof(mod), "Mod: %s (you have it)", name);
#ifdef PLATFORM_CURL
                else snprintf(mod, sizeof(mod), "Mod: %s (downloads when you join)", name);
#else
                else snprintf(mod, sizeof(mod), "Mod: %s (not found here)", name);
#endif
            }
            if (flags & JKMOD_TAIL_PACK)
                snprintf(text, sizeof(text), "%s%sRemastered pack%s", mod, mod[0] ? ", " : "",
                         havePack ? "" : " (the game restarts to load it)");
            else if (havePack && Main_bMotsCompat)
                snprintf(text, sizeof(text), "%s%sNo Remastered pack (the game restarts without it)", mod, mod[0] ? ", " : "");
            else
                snprintf(text, sizeof(text), "%s", mod);
        }
#ifdef PLATFORM_CURL
        // Added: a custom map comes from the host too (maps.js): say whether it will.
        if (dps_IsWebLobbyActive())
        {
            int map = jkx_MapState(jkGuiMultiplayer_aEntries[idx].episodeGobName, JKRES_GOB_EXT);
            if (map)
            {
                size_t at = strlen(text);
                snprintf(text + at, sizeof(text) - at, "%sMap: %s", at ? ", " : "",
                         map == 1 ? "you have it" : "downloads when you join");
            }
        }
#endif
    }
    {
        int i = 0;
        for (; text[i] && i < maxChars - 1; i++) pOut[i] = (uint16_t)(uint8_t)text[i];
        if (maxChars > 0) pOut[i] = 0;
    }
}

void stdComm_DPlay_DescribeConnection(char* pOut, int outLen, char* pLinks, int linksLen)
{
    dps_DescribeConnection(pOut, outLen > 0 ? (size_t)outLen : 0, pLinks, linksLen > 0 ? (size_t)linksLen : 0);
}

void stdComm_DPlay_Idle(void)
{
    dps_Idle();
#ifdef PLATFORM_CURL
    if (dps_RelayIsHost()) jkx_HostPump(); // Added: serving downloads on the relay
#endif
}

// Added (2026-11-06): a host's sv_strafeSpeed goes into its game's description as soon as
// it changes (rcon), not only at the next level. Called every frame (jkServer_Tick).
void stdComm_DPlay_StrafeTick(void)
{
    static uint32_t last = 0xFFFFFFFFu;
    dps_session s;
    if (!sithNet_isMulti || !dps_IsRelayActive() || !dps_RelayIsHost() || DPlay_StrafeFlags() == last)
        return;
    if (FAILED(dps_GetSessionDesc(&s)))
        return;
    last = DPlay_StrafeFlags();
    if ((s.flags & DPLAY_SESSION_STRAFE_MASK) == last)
        return;
    s.flags = (s.flags & ~DPLAY_SESSION_STRAFE_MASK) | last;
    dps_SetSessionDesc(&s);
    stdPlatform_Printf("OpenJKDF2: sidestep speed %.2f of forward for everyone\n", DPlay_StrafeHundredths() / 100.0);
}

// Added: the sidestep speed the game this machine is in sets (a part of forward speed),
// or -1: none (an older host, or not an Imperial Network Protocol game), each player's
// own g_bEqualStrafeSpeed then. Asked every physics step, so the game's description is
// read at most every 200 ms.
float stdComm_DPlay_StrafeScale(void)
{
    static uint32_t lastMs = 0;
    static float scale = -1.0f;
    dps_session s;
    uint32_t now = stdPlatform_GetTimeMsec();
    if (!sithNet_isMulti || !dps_IsRelayActive())
        return -1.0;
    if (lastMs && now - lastMs < 200)
        return scale;
    lastMs = now ? now : 1;
    scale = -1.0;
    if (SUCCEEDED(dps_GetSessionDesc(&s)) && (s.flags & DPLAY_SESSION_STRAFE_SET))
        scale = ((s.flags & DPLAY_SESSION_STRAFE_VALUE) >> DPLAY_SESSION_STRAFE_SHIFT) / 100.0;
    return scale;
}

int stdComm_DPlay_ServerKind(void)
{
    return dps_IsRelayActive() ? 1 : (dps_IsWebLobbyActive() ? 2 : 0);
}

const char* stdComm_DPlay_GetServer(void)
{
    switch (stdComm_DPlay_ServerKind())
    {
        case 1: return dps_GetRelayServer();
        case 2: return dps_GetWebLobby();
        default: return "";
    }
}

void stdComm_DPlay_SetServer(const char* address)
{
    if (!address) return;
    switch (stdComm_DPlay_ServerKind())
    {
        case 1:
            if (strcmp(address, dps_GetRelayServer())) { dps_SetRelayServer(address); wuRegistry_SaveStr("relayServer", address); }
            break;
        case 2:
            if (strcmp(address, dps_GetWebLobby())) { dps_SetWebLobby(address); wuRegistry_SaveStr("webLobbyUrl", address); }
            break;
    }
}

static void DPlay_ShowInIpText(const char* s)
{
    memset(jkGuiMultiplayer_ipText, 0, sizeof(jkGuiMultiplayer_ipText));
    for (int i = 0; s && i < 255 && s[i]; i++) jkGuiMultiplayer_ipText[i] = (char16_t)(uint8_t)s[i];
}

static void DPlay_AsciiFromW(char* out, size_t outLen, const char16_t* w)
{
    size_t i = 0;
    for (; w && w[i] && i + 1 < outLen; i++) out[i] = (w[i] < 0x80) ? (char)w[i] : '?';
    out[i] = 0;
}

// ---------------------------------------------------------------------------
// UTF-16 helpers (char16_t on our side, uint16_t on the shim side)
// ---------------------------------------------------------------------------

static size_t DPlay_StrLen16(const uint16_t* s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

// Appends src (UTF-16) to dst at *pos, truncating to dstLen-1.
static void DPlay_Append16(uint16_t* dst, size_t dstLen, size_t* pos, const uint16_t* src, size_t srcLen)
{
    for (size_t i = 0; i < srcLen && *pos + 1 < dstLen; i++)
        dst[(*pos)++] = src[i];
    dst[*pos] = 0;
}

// Appends an ASCII string, widening each byte.
static void DPlay_AppendAscii(uint16_t* dst, size_t dstLen, size_t* pos, const char* src, size_t srcMax)
{
    for (size_t i = 0; i < srcMax && src[i] && *pos + 1 < dstLen; i++)
        dst[(*pos)++] = (uint16_t)(uint8_t)src[i];
    dst[*pos] = 0;
}

static void DPlay_CopyToAscii(char* dst, size_t dstLen, const uint16_t* src, size_t srcLen)
{
    size_t i = 0;
    for (; i < srcLen && i + 1 < dstLen; i++)
        dst[i] = (src[i] < 0x80) ? (char)src[i] : '?';
    dst[i] = 0;
}

// ---------------------------------------------------------------------------
// Session description <-> jkMultiEntry
// ---------------------------------------------------------------------------

// [WIRE] lpszSessionName is "<serverName>:<episodeGob>:<mapJkl>", e.g.
// "lemonade stand:JK1MP:m_boss17.jkl".
static void DPlay_EncodeSessionName(uint16_t* out, size_t outLen, const jkMultiEntry* e)
{
    static const uint16_t colon[1] = { ':' };
    size_t pos = 0;
    out[0] = 0;
    DPlay_Append16(out, outLen, &pos, (const uint16_t*)e->serverName,
                   DPlay_StrLen16((const uint16_t*)e->serverName) < 32 ? DPlay_StrLen16((const uint16_t*)e->serverName) : 31);
    DPlay_Append16(out, outLen, &pos, colon, 1);
    DPlay_AppendAscii(out, outLen, &pos, e->episodeGobName, sizeof(e->episodeGobName));
    DPlay_Append16(out, outLen, &pos, colon, 1);
    DPlay_AppendAscii(out, outLen, &pos, e->mapJklFname, sizeof(e->mapJklFname));
}

// Parses from the right so a ':' inside the server name doesn't break it.
static void DPlay_DecodeSessionName(const uint16_t* name, jkMultiEntry* e)
{
    size_t len = DPlay_StrLen16(name);
    size_t cMap = (size_t)-1, cEp = (size_t)-1;

    for (size_t i = len; i-- > 0; )
    {
        if (name[i] != ':') continue;
        if (cMap == (size_t)-1) cMap = i;
        else { cEp = i; break; }
    }

    memset(e->serverName, 0, sizeof(e->serverName));
    memset(e->episodeGobName, 0, sizeof(e->episodeGobName));
    memset(e->mapJklFname, 0, sizeof(e->mapJklFname));

    if (cMap == (size_t)-1 || cEp == (size_t)-1)
    {
        // Not a JK-formatted name; show it verbatim.
        size_t n = len < 31 ? len : 31;
        memcpy(e->serverName, name, n * sizeof(uint16_t));
        return;
    }

    size_t nSrv = cEp < 31 ? cEp : 31;
    memcpy(e->serverName, name, nSrv * sizeof(uint16_t));
    DPlay_CopyToAscii(e->episodeGobName, sizeof(e->episodeGobName), name + cEp + 1, cMap - cEp - 1);
    DPlay_CopyToAscii(e->mapJklFname, sizeof(e->mapJklFname), name + cMap + 1, len - cMap - 1);
}

// [WIRE] dwUser fields of a vanilla session (values from a real host):
//   dwUser1 = checksumSeed                       (0x4C1A9834, random per session)
//   dwUser2 = multiModeFlags                     (0x90)
//   dwUser3 = field_E0 | (maxRank << 16)         (0x0008000A: version 10, rank 8)
//   dwUser4 = tickRateMs                         (180)
// field_E0 == 10 is the version check (GUI shows GUINET_WRONGVERSION otherwise).
// [INFERRED] the maxRank half of dwUser3 -- confirm by hosting vanilla with a
// different max rank and checking the high word changes.
static void DPlay_EntryToSession(const jkMultiEntry* e, dps_session* s)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->guidInstance, &e->guidInstance, 16);

    s->flags = DPLAY_HOST_BASE_FLAGS | ((uint32_t)e->sessionFlags & DPLAY_SESSIONFLAG_MASK);
    if (e->wPassword[0])
    {
        s->flags |= DPS_SESSION_PASSWORDREQUIRED;
        size_t n = DPlay_StrLen16((const uint16_t*)e->wPassword);
        if (n > 31) n = 31;
        if (n > DPS_PASSWORD_MAX - 1) n = DPS_PASSWORD_MAX - 1;
        memcpy(s->password, e->wPassword, n * sizeof(uint16_t));
    }
    else
    {
        s->flags &= ~(uint32_t)DPS_SESSION_PASSWORDREQUIRED;
    }

    s->maxPlayers = (uint32_t)e->maxPlayers;
    DPlay_EncodeSessionName(s->name, DPS_NAME_MAX, e);

    s->user[0] = (uint32_t)e->checksumSeed;
    s->user[1] = (uint32_t)e->multiModeFlags;
    s->user[2] = ((uint32_t)e->field_E0 & 0xFFFF) | (((uint32_t)e->maxRank & 0xFFFF) << 16);
    s->user[3] = (uint32_t)e->tickRateMs;
}

static void DPlay_SessionToEntry(const dps_session* s, jkMultiEntry* e)
{
    memset(e, 0, sizeof(*e));
    memcpy(&e->guidInstance, s->guidInstance, 16);
    e->maxPlayers     = (int32_t)s->maxPlayers;
    e->numPlayers     = (int32_t)s->curPlayers;
    DPlay_DecodeSessionName(s->name, e);
    e->sessionFlags   = (int32_t)s->flags;
    e->checksumSeed   = (int32_t)s->user[0];
    e->multiModeFlags = (int32_t)s->user[1];
    e->field_E0       = (int32_t)(s->user[2] & 0xFFFF);
    e->maxRank        = (int32_t)(s->user[2] >> 16);
    e->tickRateMs     = (int32_t)s->user[3];
}

// ---------------------------------------------------------------------------
// Startup / shutdown
// ---------------------------------------------------------------------------

static void DPlay_FreeConnections(void)
{
    for (size_t i = 0; i < DPLAY_MAX_CONNECTIONS; i++)
    {
        free(jkGuiMultiplayer_aConnections[i].connection);
        jkGuiMultiplayer_aConnections[i].connection = NULL;
    }
    memset(jkGuiMultiplayer_aConnections, 0, sizeof(sith_dplay_connection) * DPLAY_MAX_CONNECTIONS);
    jkGuiMultiplayer_numConnections = 0;
}

static int DPlay_ConnectionCallback(void* ctx, const uint8_t guidSP[16], const uint16_t* name,
                                    const void* connection, uint32_t connectionSize)
{
    (void)ctx;
    if (jkGuiMultiplayer_numConnections >= DPLAY_MAX_CONNECTIONS)
        return 0;

    sith_dplay_connection* c = &jkGuiMultiplayer_aConnections[jkGuiMultiplayer_numConnections];
    memset(c, 0, sizeof(*c));

    size_t n = DPlay_StrLen16(name);
    if (n > 127) n = 127;
    memcpy(c->name, name, n * sizeof(uint16_t));
    memcpy(&c->guid, guidSP, 16);

    // The connection blob is only valid during the callback; keep a copy for
    // InitializeConnection.
    c->connection = (DPLCONNECTION*)malloc(connectionSize);
    if (!c->connection)
        return 0;
    memcpy(c->connection, connection, connectionSize);
    c->connectionSize = (int32_t)connectionSize;

    jkGuiMultiplayer_numConnections++;
    return 1;
}

static const uint8_t* DPlay_GetAppGuid(void)
{
    const uint32_t* g = WinIdk_GetDplayGuid();
    if (g && (g[0] | g[1] | g[2] | g[3]))
        return (const uint8_t*)g;
    return DPlay_kJkAppGuid;
}

BOOL DirectPlay_Startup()
{
    DPlay_FreeConnections();

    if (FAILED(dps_Startup(DPlay_GetAppGuid())))
        return 0;

    // [DECOMP] Original: EnumConnections(WinIdk_GetDplayGuid(), DirectPlay_EnumConnectionsCallback, 0, 0)
    if (FAILED(dps_EnumConnections(DPlay_ConnectionCallback, NULL)))
        return 0;

    DPlay_bStarted = 1;
    {
        static int bAtExit = 0; // once: this survives the engine's in-process restart
        if (!bAtExit) atexit(DPlay_AtExit);
        bAtExit = 1;
    }
    return 1;
}

void stdComm_DPlay_Startup()
{
    stdComm_dword_8321E0 = 0;
    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * DPLAY_MAX_SESSIONS);
    dplay_dword_55D618 = 0;
    DirectPlay_numPlayers = 0;

    // Imperial Network Protocol relay server: OPENJKDF2_RELAY wins, else the saved one.
    {
        const char* env = getenv("OPENJKDF2_RELAY");
        char saved[256] = { 0 };
        wuRegistry_GetStr("relayServer", saved, sizeof(saved), "");
        dps_SetRelayServer((env && *env) ? env : saved);
    }
    // jkdf2mp web lobby: OPENJKDF2_WEB_LOBBY wins, else the last one used.
    {
        const char* env = getenv("OPENJKDF2_WEB_LOBBY");
        char saved[512] = { 0 };
        wuRegistry_GetStr("webLobbyUrl", saved, sizeof(saved), "");
        dps_SetWebLobby((env && *env) ? env : saved);
    }

    if (!DirectPlay_Startup() || !jkGuiMultiplayer_numConnections)
    {
        // Leave one entry so the GUI has something to show; selecting it
        // fails in stdComm_OpenConnection with DPERR_UNAVAILABLE (GUINET_NOTAVAIL).
        DPlay_FreeConnections();
        jkGuiMultiplayer_numConnections = 1;
        jk_snwprintf(jkGuiMultiplayer_aConnections[0].name, 0x80, u"DirectPlay not available");
        DPlay_bStarted = 0;
    }
}

void DirectPlay_Destroy()
{
    DPlay_FreeConnections();
    dps_Shutdown();
    DPlay_bStarted = 0;
}

// [INFERRED] Lobby (Gaming Zone) launch detection. 1 = host, 2 = join. Lobby
// launching isn't supported yet, so never auto-launch.
int DirectPlay_EarlyInit(char16_t* pwIdk, char16_t* pwPlayerName)
{
    (void)pwIdk; (void)pwPlayerName;
    return 0;
}

// [INFERRED] Lobby score reporting; no lobby, nothing to do.
int DirectPlay_SendLobbyMessage(void* pPkt, uint32_t pktLen)
{
    (void)pPkt; (void)pktLen;
    return 1;
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

// [DECOMP] The GUI passes the selected connection index cast to a pointer and
// checks for 0 (ok), 0x887700FA (DPERR_UNAVAILABLE) or any other error.
int stdComm_OpenConnection(void* a)
{
    int idx = (int)(intptr_t)a;
    HRESULT hr;

    if (!DPlay_bStarted || idx < 0 || idx >= (int)jkGuiMultiplayer_numConnections
        || !jkGuiMultiplayer_aConnections[idx].connection)
        return (int)0x887700FA; // DPERR_UNAVAILABLE

    hr = dps_InitializeConnection(jkGuiMultiplayer_aConnections[idx].connection);
    if (FAILED(hr))
    {
        // InitializeConnection only works once per object; start fresh and retry.
        if (SUCCEEDED(dps_Recreate()))
            hr = dps_InitializeConnection(jkGuiMultiplayer_aConnections[idx].connection);
    }
    if (FAILED(hr))
    {
        stdLog_Printf("net", "couldn't open connection %d (error %08x)", idx, (unsigned)hr);
        return (int)hr;
    }
    DPlay_connIdx = idx; // Added: for joining again after a restart
    dps_RelaySetClock(stdComm_DPlay_Now); // Added: Hit what you see's clock, for players' clock offsets
#ifdef PLATFORM_CURL
    if (dps_IsRelayActive())
        dps_RelaySetBigHandler(DPlay_RelayBigFrame); // Added: map and mod downloads on the relay
#endif

    // Added: auto-joining ("connect", or after a restart) a game on a server other
    // than the saved one: use it for now, without saving it.
    if (DPlay_bAutoJoin && DPlay_autoJoinServer[0] && dps_IsRelayActive())
    {
        dps_SetRelayServer(DPlay_autoJoinServer);
        DPlay_bTempServer = 1;
    }

    // The address box shows what the selected connection needs: the relay
    // server, the web lobby URL, or (DirectPlay) the last host searched.
    {
        int mode = dps_IsRelayActive() ? 1 : (dps_IsWebLobbyActive() ? 2 : 0);
        if (mode != DPlay_ipTextMode || (mode == 1 && DPlay_bTempServer))
        {
            if (DPlay_ipTextMode == 0)
                memcpy(DPlay_savedIpText, jkGuiMultiplayer_ipText, sizeof(DPlay_savedIpText));
            if (mode == 1) DPlay_ShowInIpText(dps_GetRelayServer());
            else if (mode == 2) DPlay_ShowInIpText(dps_GetWebLobby());
            else memcpy(jkGuiMultiplayer_ipText, DPlay_savedIpText, sizeof(DPlay_savedIpText));
            DPlay_ipTextMode = mode;
        }
    }

    // The web lobby's players run OpenJKDF2 with its own message layouts, and so do
    // Imperial Network Protocol games now (for browser players); a joiner then follows the
    // game it joins (stdComm_Open). DirectPlay keeps JK.EXE's, for JK.EXE players.
    stdComm_bOpenJkdf2Wire = dps_IsWebLobbyActive() || dps_IsRelayActive();

    {
        char desc[256], links[8];
        dps_DescribeConnection(desc, sizeof(desc), links, 0);
        stdLog_Printf("net", "connection: %s", desc);
    }

    stdComm_dword_8321DC = 1;
    return 0;
}

// Added: a dedicated server picks its connection from the command line
// (-connection relay|web|tcpip|ipx; "imperial" = relay). The address is only for this run.
int stdComm_DPlay_OpenConnectionNamed(const char* pKind, const char* pServer)
{
    // DPSPGUID_TCPIP / DPSPGUID_IPX, first bytes (Microsoft's and dplay_native.c's)
    static const uint8_t tcpip[4] = { 0xE0, 0x5E, 0xE9, 0x36 };
    static const uint8_t ipx[4] = { 0x00, 0xC4, 0x5B, 0x68 };
    int want = -1;   // 0 relay, 1 web lobby, 2 TCP/IP, 3 IPX

    if (!__strcmpi(pKind, "relay") || !__strcmpi(pKind, "imperial") || !__strcmpi(pKind, "online")) want = 0;
    else if (!__strcmpi(pKind, "web") || !__strcmpi(pKind, "weblobby")) want = 1;
    else if (!__strcmpi(pKind, "tcpip") || !__strcmpi(pKind, "directplay")) want = 2;
    else if (!__strcmpi(pKind, "ipx")) want = 3;
    if (want < 0)
    {
        stdLog_Printf("net", "unknown connection \"%s\" (relay, web, tcpip or ipx)", pKind);
        return (int)0x887700FA;
    }

    for (int i = 0; i < (int)jkGuiMultiplayer_numConnections; i++)
    {
        const sith_dplay_connection* c = &jkGuiMultiplayer_aConnections[i];
        const uint8_t* blob = (const uint8_t*)c->connection;
        int kind = -1;
        if (!blob) continue;
        if (c->connectionSize == 4 && !memcmp(blob, "JKR1", 4)) kind = 0;
        else if (c->connectionSize == 4 && !memcmp(blob, "JKW1", 4)) kind = 1;
        else if (!memcmp(&c->guid, tcpip, 4)) kind = 2;
        else if (!memcmp(&c->guid, ipx, 4)) kind = 3;
        if (kind != want) continue;

        if (pServer && *pServer)
        {
            if (want == 0) dps_SetRelayServer(pServer);
            else if (want == 1) dps_SetWebLobby(pServer);
        }
        int hr = stdComm_OpenConnection((void*)(intptr_t)i);
        if (hr) return hr;
        if (want <= 1 && !stdComm_DPlay_GetServer()[0])
        {
            stdLog_Printf("net", "no %s set: use -%s <address>", want ? "web lobby" : "relay server",
                          want ? "webLobby" : "relayServer");
            stdComm_CloseConnection();
            return (int)0x887700FA;
        }
        return 0;
    }
    stdLog_Printf("net", "connection \"%s\" isn't available in this build", pKind);
    return (int)0x887700FA;
}

void stdComm_CloseConnection()
{
    if (stdComm_dword_8321DC)
    {
        stdComm_Close();
        dps_StopEnumSessions();
        dps_Recreate(); // allows a different service provider to be picked next time
        stdComm_dword_8321DC = 0;
        stdComm_bOpenJkdf2Wire = 0;
    }
    // Added: back to the saved relay server after a "connect" (the address box too)
    if (DPlay_bTempServer)
    {
        const char* env = getenv("OPENJKDF2_RELAY");
        char saved[256] = { 0 };
        wuRegistry_GetStr("relayServer", saved, sizeof(saved), "");
        dps_SetRelayServer((env && *env) ? env : saved);
        DPlay_bTempServer = 0;
        if (DPlay_ipTextMode == 1) DPlay_ShowInIpText(dps_GetRelayServer());
    }
}

// ---------------------------------------------------------------------------
// Session enumeration / join / host
// ---------------------------------------------------------------------------

static int DPlay_SessionCallback(void* ctx, const dps_session* s)
{
    (void)ctx;
    if (dplay_dword_55D618 >= DPLAY_MAX_SESSIONS)
        return 0;
    DPlay_SessionToEntry(s, &jkGuiMultiplayer_aEntries[dplay_dword_55D618]);
    dplay_dword_55D618++;
    return 1;
}

// [DECOMP] Polled by the GUI every 5 s; returns 0 on success with
// dplay_dword_55D618 sessions in jkGuiMultiplayer_aEntries.
int stdComm_EnumSessions(int a, void* b)
{
    HRESULT hr;
    char host[256];
    (void)a; (void)b;   // b is the password box, not an address

    // The join screen's address box: with DirectPlay it's also searched
    // directly (games outside the LAN/VPN, by IP or host name); with
    // the Imperial Network Protocol it's the relay server (the box is hidden
    // there now; the master servers list the games too).
    DPlay_AsciiFromW(host, sizeof(host), jkGuiMultiplayer_ipText);
    if (dps_IsRelayActive())
    {
        dps_SetRelayMasters(DPlay_aMasterServers); // Added: net_masterServers
        if (host[0] && strcmp(host, "127.0.0.1") && strcmp(host, dps_GetRelayServer()))
        {
            dps_SetRelayServer(host);
            wuRegistry_SaveStr("relayServer", host);
            DPlay_bTempServer = 0; // Added: typed in, so it's the saved one now
        }
    }
    else if (dps_IsWebLobbyActive())
    {
        if (host[0] && strcmp(host, "127.0.0.1") && strcmp(host, dps_GetWebLobby()))
        {
            dps_SetWebLobby(host);
            wuRegistry_SaveStr("webLobbyUrl", host);
        }
        // Who we are: the lobby names us and lists only the games we can join.
        dps_SetWebPresence(Main_bMotsCompat, (const uint16_t*)jkPlayer_playerShortName);
    }
    else
    {
        dps_SetEnumHost(host);
    }

    dplay_dword_55D618 = 0;
    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * DPLAY_MAX_SESSIONS);

    hr = dps_EnumSessions(DPlay_SessionCallback, NULL);

    // Back from restarting into a game's mod: select that game again.
    if (DPlay_bPendingGuid)
        for (uint32_t i = 0; i < dplay_dword_55D618; i++)
            if (!memcmp(&jkGuiMultiplayer_aEntries[i].guidInstance, &DPlay_pendingGuid, sizeof(DPlay_pendingGuid)))
            {
                jkGui_guid_556040 = DPlay_pendingGuid;
                DPlay_bPendingGuid = 0;
            }
    return (int)hr;
}

int DirectPlay_EnumSessions2()
{
    dps_StopEnumSessions();
    return 0;
}

DPID DirectPlay_CreatePlayer(char16_t* pwName, int idk2)
{
    uint32_t id = 0;
    (void)idk2;
    if (FAILED(dps_CreatePlayer((const uint16_t*)pwName, &id)))
        return 0;
    return (DPID)id;
}

void DirectPlay_Close()
{
    dps_Close();
}

int stdComm_Open(int idx, char16_t* pwPassword)
{
    HRESULT hr;
    DPID id;

    DirectPlay_EnumSessions2();

    if (idx < 0 || idx >= (int)dplay_dword_55D618)
        return (int)0x80004005; // E_FAIL

    {
        int m = DPlay_MatchHostMod(idx, pwPassword);
        DPlay_bAutoJoining = 0; // Added: only the first try after the restart
        if (m) return m;
    }

    {
        char name[64];
        stdString_WcharToChar(name, jkGuiMultiplayer_aEntries[idx].serverName, 63);
        name[63] = 0;
        stdLog_Printf("net", "joining \"%s\" (%s, %s)", name, jkGuiMultiplayer_aEntries[idx].episodeGobName,
                      jkGuiMultiplayer_aEntries[idx].mapJklFname);
    }
    dps_SetRelayDirect(DPlay_bDirectLinks); // Added: direct links (net_directLinks)
    stdComm_WSRelay_ResetHits(); // Added: Classic until the host's hello says otherwise
    stdComm_WSRelay_ResetMotion();
    hr = dps_OpenJoin((const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance, (const uint16_t*)pwPassword);
    if (FAILED(hr))
    {
        stdLog_Printf("net", "join failed (error %08x)", (unsigned)hr);
        return (int)hr;
    }

    // Added: an Imperial Network Protocol game's messages use the layouts its host chose
    if (dps_IsRelayActive())
    {
        dps_session joined;
        if (SUCCEEDED(dps_GetSessionDesc(&joined)))
            stdComm_bOpenJkdf2Wire = (joined.flags & DPLAY_SESSION_OPENJKDF2WIRE) != 0;
        stdLog_Printf("net", "message layouts: %s", stdComm_bOpenJkdf2Wire ? "OpenJKDF2" : "JK.EXE (an older host)");
    }

#ifdef PLATFORM_CURL
    // Web lobby: the host's map, before the level loads (maps.js). Added: also from
    // an Imperial Network Protocol host that serves it.
    if (dps_IsRelayActive())
        dps_RelaySetBigHandler(DPlay_RelayBigFrame);
    if (dps_IsWebLobbyActive() || (dps_IsRelayActive() && (jkGuiMultiplayer_aEntries[idx].sessionFlags & DPLAY_SESSION_XFER)))
    {
        jkx_StartMap(jkGuiMultiplayer_aEntries[idx].episodeGobName, JKRES_GOB_EXT);
        if (!DPlay_RunXfer(JKX_KIND_MAP))
        {
            dps_Close();
            return STDCOMM_DPLAY_HANDLED;
        }
    }
#endif

    stdComm_dword_8321E8 = 0;
    stdComm_bIsServer = 0;

    // [WIRE] The vanilla client creates its DirectPlay player immediately on
    // join (CREATEPLAYER with the short name only), before DSS_JOINREQUEST.
    id = DirectPlay_CreatePlayer(jkPlayer_playerShortName, 0);
    if (!id)
    {
        dps_Close();
        return (int)0x80004005;
    }

    stdComm_dplayIdSelf = id;
    stdComm_dword_8321E0 = 1;
    jkGuiMultiplayer_checksumSeed = jkGuiMultiplayer_aEntries[idx].checksumSeed;

    // Added: for console "reconnect": this game, where it is, and the character (also
    // the one console "connect" uses from now on)
    if (dps_IsRelayActive())
    {
        char character[32];
        stdString_SafeStrCopy(DPlay_lastServer, dps_RelayCurrentServer(), sizeof(DPlay_lastServer));
        stdString_WcharToChar(DPlay_lastGameName, jkGuiMultiplayer_aEntries[idx].serverName, sizeof(DPlay_lastGameName) - 1);
        DPlay_lastGameName[sizeof(DPlay_lastGameName) - 1] = 0;
        memcpy(&DPlay_lastGuid, &jkGuiMultiplayer_aEntries[idx].guidInstance, sizeof(DPlay_lastGuid));
        DPlay_bLastGuid = 1;
        memset(DPlay_lastPassword, 0, sizeof(DPlay_lastPassword));
        for (int i = 0; pwPassword && i < 31 && pwPassword[i]; i++) DPlay_lastPassword[i] = pwPassword[i];
        wuRegistry_SaveStr("lastGameServer", DPlay_lastServer);
        wuRegistry_SaveStr("lastGameName", DPlay_lastGameName);
        stdString_WcharToChar(character, jkGuiMultiplayer_mpcInfo.name, 31);
        character[31] = 0;
        if (character[0])
            wuRegistry_SaveStr("mpCharacter", character);
    }
    return 0;
}

// [INFERRED] Original stdComm_Close (0x004C9820, 0x30 bytes) is not decompiled.
void stdComm_Close()
{
    if (stdComm_dword_8321E0)
    {
        stdLog_Printf("net", "game closed");
        if (stdComm_dplayIdSelf)
            dps_DestroyPlayer(stdComm_dplayIdSelf);
        dps_Close();
        stdComm_dword_8321E0 = 0;
        stdComm_bIsServer = 0;
        stdComm_dplayIdSelf = 0;
    }
}

// [DECOMP] Called from stdComm_CreatePlayer() after checksumSeed and
// field_E0 = 10 have been filled in. Returns 0 on success.
int DirectPlay_OpenHost(jkMultiEntry* pEntry)
{
    dps_session s;
    HRESULT hr;

    memset(&pEntry->guidInstance, 0, sizeof(pEntry->guidInstance)); // DirectPlay assigns it
    DPlay_EntryToSession(pEntry, &s);

    // Web lobby: name our mod (and Remastered pack) so browser joiners can switch to it.
    {
        uint8_t fp[32], pack[JKMOD_PACK_FP];
        char name[JKMOD_MAX_NAME + 1], tail[JKMOD_MAX_TAIL_HEX + 1] = "";
        int haveMod = DPlay_OurMod(fp, name, sizeof(name));
#ifdef PLATFORM_CURL
        // Added (2026-11-08): a mod listed in noshare.txt isn't announced at all: the join
        // screen shows no mod, nobody is asked to download it or restart into it, and
        // joiners without it join as they are (a model or skin pack only changes looks).
        {
            uint8_t fp2[32];
            char name2[JKMOD_MAX_NAME + 1], modPath[512];
            int isFile = 0;
            if (haveMod && DPlay_OurModPath(fp2, name2, sizeof(name2), modPath, sizeof(modPath), &isFile)
                && jkx_ModNotShared(modPath, isFile))
            {
                haveMod = 0;
                stdLog_Printf("net", "mod %s is in noshare.txt: not announced to joiners", name);
            }
        }
#endif
        // and the MotS Remastered pack, as mods.js does. Added: on the Imperial Network Protocol too
        int havePack = (dps_IsWebLobbyActive() || dps_IsRelayActive()) && DPlay_OurPack(pack);
        jkmod_MakeTail(haveMod ? fp : NULL, name, havePack ? pack : NULL, tail, sizeof(tail));
        dps_SetHostModTail(tail);
    }
    // Web lobby: tell it our game first, so JK and MotS players only see their own.
    if (dps_IsWebLobbyActive())
        dps_SetWebPresence(Main_bMotsCompat, (const uint16_t*)jkPlayer_playerShortName);

    dps_SetRelayDirect(DPlay_bDirectLinks); // Added: direct links (net_directLinks)
    if (dps_IsRelayActive())
        dps_ServerPushBans(); // Added: a dedicated server's bans, which the relay server enforces for it
    if (dps_IsRelayActive() && stdComm_bOpenJkdf2Wire)
        s.flags |= DPLAY_SESSION_OPENJKDF2WIRE; // Added: this game's layouts (joiners follow)
#ifdef PLATFORM_CURL
    if (dps_IsRelayActive())
        s.flags |= DPLAY_SESSION_XFER; // Added: we serve our map and mod (below)
#endif
    if (dps_IsRelayActive())
        s.flags |= DPlay_StrafeFlags(); // Added: sv_strafeSpeed
    // Added: Hit what you see, when this host plays it (its hellos tell the other games)
    stdComm_WSRelay_ResetHits();
    stdComm_WSRelay_ResetMotion();
    if (stdComm_bOpenJkdf2Wire)
    {
        int reachMs;
        int rules = stdComm_Hits_HostRules(&reachMs);
        stdComm_WSRelay_SetHitRules(rules, reachMs);
        if (dps_IsRelayActive() && rules == STDCOMM_WSRELAY_HITS_HWYS)
            s.flags |= STDCOMM_HITS_SESSION_HWYS;
    }
    hr = dps_OpenCreate(&s);
    {
        char name[64];
        stdString_WcharToChar(name, pEntry->serverName, 63);
        name[63] = 0;
        if (FAILED(hr))
            stdLog_Printf("net", "hosting \"%s\" failed (error %08x)", name, (unsigned)hr);
        else
            stdLog_Printf("net", "hosting \"%s\": %s, %s, up to %d players, tick %d ms%s", name, pEntry->episodeGobName,
                          pEntry->mapJklFname, pEntry->maxPlayers, pEntry->tickRateMs,
                          pEntry->wPassword[0] ? ", password" : "");
    }
    if (FAILED(hr))
        return (int)hr;

#ifdef PLATFORM_CURL
    // Web lobby: our custom episode and our mod, for joiners who lack them
    // (maps.js / mods.js host()). Added: and on the Imperial Network Protocol.
    if (dps_IsRelayActive())
    {
        dps_RelaySetBigHandler(DPlay_RelayBigFrame);
        memset(DPlay_aXferSys, 0, sizeof(DPlay_aXferSys));
    }
    if (dps_IsWebLobbyActive() || dps_IsRelayActive())
    {
        DPlay_aOfferedEpisode[0] = 0;
        DPlay_HostOffer(pEntry->episodeGobName);
    }
#endif

    // Pick up the instance GUID DirectPlay generated.
    if (SUCCEEDED(dps_GetSessionDesc(&s)))
        memcpy(&pEntry->guidInstance, s.guidInstance, 16);

    return 0;
}

// [DECOMP] Lobby-launched host path (stdComm_seed_idk): the session already
// exists, so push the entry's settings into it. Nonzero == success.
int DirectPlay_GetSession_passwordidk(jkMultiEntry* pEntry)
{
    dps_session cur, s;

    if (FAILED(dps_GetSessionDesc(&cur)))
        return 0;

    DPlay_EntryToSession(pEntry, &s);
    memcpy(s.guidInstance, cur.guidInstance, 16);
    return SUCCEEDED(dps_SetSessionDesc(&s)) ? 1 : 0;
}

// [DECOMP] Polled every 2 s by a waiting client; the GUI proceeds once
// field_E0 is nonzero and adopts checksumSeed.
int DirectPlay_IdkSessionDesc(jkMultiEntry* pEntry)
{
    dps_session s;

    if (FAILED(dps_GetSessionDesc(&s)))
        return 0;
    DPlay_SessionToEntry(&s, pEntry);
    return 1;
}

// [DECOMP] Host calls this with the new level's .jkl and player cap whenever a
// level is loaded. [INFERRED] Original rewrites the map part of the session
// name and dwMaxPlayers; everything else is preserved.
void DirectPlay_SetSessionDesc(const char* pJklFname, DWORD maxPlayers)
{
    dps_session s;
    jkMultiEntry e;

    if (FAILED(dps_GetSessionDesc(&s)))
        return;

    DPlay_SessionToEntry(&s, &e);
    // Added: loading the named next level keeps it named (stdComm_DPlay_SetSessionMap): a joiner
    // that loads a moment after the host must still see it
    if ((s.flags & DPLAY_SESSION_NEXTMAP) && (!pJklFname || __strcmpi(e.mapJklFname, pJklFname)
                                             || (jkRes_episodeGobName[0] && __strcmpi(e.episodeGobName, jkRes_episodeGobName))))
        s.flags &= ~DPLAY_SESSION_NEXTMAP;
    memset(e.mapJklFname, 0, sizeof(e.mapJklFname));
    if (pJklFname)
        stdString_SafeStrCopy(e.mapJklFname, pJklFname, sizeof(e.mapJklFname));
    // Added: and the episode, which a dedicated server's rotation can change (jkMain.c)
    if (jkRes_episodeGobName[0])
        stdString_SafeStrCopy(e.episodeGobName, jkRes_episodeGobName, sizeof(e.episodeGobName));
#ifdef PLATFORM_CURL
    if (dps_IsRelayActive() || dps_IsWebLobbyActive())
        DPlay_HostOffer(e.episodeGobName); // Added: joiners get this episode now
#endif

    // Re-encode only the name; keep the live flags/user fields/password untouched.
    DPlay_EncodeSessionName(s.name, DPS_NAME_MAX, &e);
    if (dps_IsRelayActive())
        s.flags = (s.flags & ~DPLAY_SESSION_STRAFE_MASK) | DPlay_StrafeFlags(); // Added: sv_strafeSpeed now
    // Added: the hit rules this host plays by now (sv_hitRules can change between levels)
    if (dps_IsRelayActive() && stdComm_bOpenJkdf2Wire)
    {
        if (stdComm_Hits_HostRules(NULL) == STDCOMM_WSRELAY_HITS_HWYS)
            s.flags |= STDCOMM_HITS_SESSION_HWYS;
        else
            s.flags &= ~STDCOMM_HITS_SESSION_HWYS;
    }
    s.maxPlayers = maxPlayers;
    dps_SetSessionDesc(&s);
}

// Added: a dedicated host names the next level (and its episode) in the game's description
// when it ends a level, with this flag set, and its joiners go there (jkMain.c). The flag
// stays while that level loads and is played, until the host ends a level without naming one
// (NULL clears it). Only on the Imperial Network Protocol, never to JK.EXE.
void stdComm_DPlay_SetSessionMap(const char* pGob, const char* pJklFname)
{
    dps_session s;
    jkMultiEntry e;
    if (stdComm_DPlay_ServerKind() != 1 || FAILED(dps_GetSessionDesc(&s)))
        return;
    if (!pJklFname)
    {
        if (s.flags & DPLAY_SESSION_NEXTMAP)
        {
            s.flags &= ~DPLAY_SESSION_NEXTMAP;
            dps_SetSessionDesc(&s);
        }
        return;
    }
    DPlay_SessionToEntry(&s, &e);
    memset(e.mapJklFname, 0, sizeof(e.mapJklFname));
    stdString_SafeStrCopy(e.mapJklFname, pJklFname, sizeof(e.mapJklFname));
    if (pGob && pGob[0])
        stdString_SafeStrCopy(e.episodeGobName, pGob, sizeof(e.episodeGobName));
#ifdef PLATFORM_CURL
    DPlay_HostOffer(e.episodeGobName); // Added: joiners following us there can get it
#endif
    DPlay_EncodeSessionName(s.name, DPS_NAME_MAX, &e);
    s.flags |= DPLAY_SESSION_NEXTMAP;
    dps_SetSessionDesc(&s);
}

// The next level the host named and its episode (1), or 0 if it hasn't named one.
int stdComm_DPlay_GetSessionNextMap(char* pGob, int gobLen, char* pOut, int outLen)
{
    dps_session s;
    jkMultiEntry e;
    if (outLen <= 0 || gobLen <= 0) return 0;
    pOut[0] = 0;
    pGob[0] = 0;
    if (stdComm_DPlay_ServerKind() != 1 || FAILED(dps_GetSessionDesc(&s)) || !(s.flags & DPLAY_SESSION_NEXTMAP))
        return 0;
    DPlay_SessionToEntry(&s, &e);
    stdString_SafeStrCopy(pOut, e.mapJklFname, outLen);
    stdString_SafeStrCopy(pGob, e.episodeGobName, gobLen);
    return pOut[0] != 0;
}

// [DECOMP] Called with 1 when a level has loaded and 0 when it ends (score
// screen). The GUI shows "game closed" for sessions with 0x20 set.
// [INFERRED] that this toggles DPSESSION_JOINDISABLED -- confirm by capturing a
// vanilla host's enum reply during the score screen.
BOOL DirectPlay_SetSessionFlagidk(int bJoinable)
{
    dps_session s;

    if (FAILED(dps_GetSessionDesc(&s)))
        return 0;

    if (bJoinable)
        s.flags &= ~(uint32_t)DPS_SESSION_JOINDISABLED;
    else
        s.flags |= DPS_SESSION_JOINDISABLED;

    return SUCCEEDED(dps_SetSessionDesc(&s)) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

static int DPlay_PlayerCallback(void* ctx, uint32_t id, const uint16_t* shortName, const uint16_t* longName)
{
    DPNAME n;
    (void)ctx;

    memset(&n, 0, sizeof(n));
    n.dwSize = sizeof(n);
    n.lpszShortName = (LPWSTR)shortName;
    n.lpszLongName = (LPWSTR)longName;

    // Reuse the decompiled original callback so DirectPlay_aPlayers is filled
    // exactly as JK.EXE does.
    stdComm_EnumPlayersCallback((DPID)id, 1 /* DPPLAYERTYPE_PLAYER */, &n, 0, NULL);
    return DirectPlay_numPlayers < 0x20;
}

void DirectPlay_EnumPlayers(int a)
{
    (void)a;
    DirectPlay_numPlayers = 0;
    dps_EnumPlayers(DPlay_PlayerCallback, NULL);
}

// [DECOMP] Called by a joining client when the host's DSS_ENUMPLAYERS lists a
// DPID that the client's DirectPlay doesn't know yet.
// [INFERRED] The original (0x004301A0, 0x135 bytes) isn't decompiled. Doing
// nothing is safe: stdComm_dword_8321E8 stays 0, the client re-sends
// DSS_JOINREQUEST about every 2 s, and the host answers each one with a fresh
// DSS_ENUMPLAYERS, so the check simply re-runs once DirectPlay has caught up.
int DirectPlay_StartSession(void* pIdSelf, void* pwName)
{
    (void)pIdSelf; (void)pwName;
    DirectPlay_EnumPlayers(0);
    return 1;
}

// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

// Added: Hit what you see (Platform/Networking/Hits): this player's own position
// updates to each player get a number on the Imperial Network Protocol, which goes in
// the relay's data header (dps_RelaySetSendSeq), as the browser build's fast lane
// numbers them. A hit claim names the update its game had on screen, and this game
// knows when it sent that one. The numbers start at random, as there.
typedef struct DPlaySeqTo
{
    uint32_t id;
    uint32_t next;
    uint32_t aSeqs[256];
    double aAt[256];
} DPlaySeqTo;
static DPlaySeqTo DPlay_aSeqTo[DPLAY_MAX_SESSIONS];
static uint32_t DPlay_recvSeq = 0;   // the number of the packet DirectPlay_Receive returned last, 0 if none

double stdComm_DPlay_Now(void)
{
    return (double)stdPlatform_GetTimeMsec();
}

// This player's own position update (sithDSSThing_Pos for its own thing, without a
// resend id: 52 bytes, or 64 with the head pitch), as the browser build tells them.
static int DPlay_IsOwnPosition(const void* pData, uint32_t size)
{
    const uint8_t* p = (const uint8_t*)pData;
    if (size != 4 + 52 && size != 4 + 64)
        return 0;
    if (*(const uint16_t*)&p[0] != DSS_THINGPOS || *(const uint16_t*)&p[2] != 0)
        return 0;
    return sithPlayer_g_pLocalPlayerThing && *(const int32_t*)&p[4] == sithPlayer_g_pLocalPlayerThing->guid;
}

static DPlaySeqTo* DPlay_SeqTo(uint32_t id, int bAdd)
{
    DPlaySeqTo* pFree = NULL;
    for (int i = 0; i < DPLAY_MAX_SESSIONS; i++)
    {
        if (DPlay_aSeqTo[i].id == id)
            return &DPlay_aSeqTo[i];
        if (!pFree && !DPlay_aSeqTo[i].id)
            pFree = &DPlay_aSeqTo[i];
    }
    if (!bAdd)
        return NULL;
    if (!pFree)
        pFree = &DPlay_aSeqTo[id % DPLAY_MAX_SESSIONS];
    memset(pFree, 0, sizeof(*pFree));
    pFree->id = id;
    pFree->next = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    return pFree;
}

double stdComm_DPlay_SeqSentAt(int toId, uint32_t seq)
{
    DPlaySeqTo* t = DPlay_SeqTo((uint32_t)toId, 0);
    if (!t || t->aSeqs[seq & 255] != seq || !t->aAt[seq & 255])
        return -1.0;
    return t->aAt[seq & 255];
}

int stdComm_DPlay_RecvSeq(uint32_t* pSeq)
{
    *pSeq = DPlay_recvSeq;
    return DPlay_recvSeq != 0;
}

// Another player's clock less ours (Hit what you see's shot catch-up): measured on the
// relay, unknown on the web lobby.
void stdComm_DPlay_SyncClock(int netId, int bOn)
{
    dps_RelaySyncClock((uint32_t)netId, bOn);
}

double stdComm_DPlay_ClockOffset(int netId)
{
    return dps_RelayClockOffset((uint32_t)netId);
}

// Whether the hit rules' messages need sithComm's resends: the relay is UDP, the web
// lobby's links resend and keep order themselves.
int stdComm_DPlay_HitsResend(void)
{
    return dps_IsRelayActive();
}

BOOL DirectPlay_Send(DPID idFrom, DPID idTo, void* lpData, DWORD dwDataSize)
{
    // Added: numbered position updates (above)
    if (idTo && dps_IsRelayActive() && stdComm_bOpenJkdf2Wire && DPlay_IsOwnPosition(lpData, (uint32_t)dwDataSize))
    {
        DPlaySeqTo* t = DPlay_SeqTo((uint32_t)idTo, 1);
        uint32_t seq = ++t->next;
        if (!seq)
            seq = ++t->next;
        t->aSeqs[seq & 255] = seq;
        t->aAt[seq & 255] = stdComm_DPlay_Now();
        dps_RelaySetSendSeq(seq);
    }
    return SUCCEEDED(dps_Send((uint32_t)idFrom, (uint32_t)idTo, lpData, (uint32_t)dwDataSize)) ? 1 : 0;
}

// [DECOMP] *pLenOut is the buffer capacity on entry (2052 from stdComm_Recv,
// 2048 from stdComm_DoReceive) and the message size on return. Data is written
// starting at netMsg.cogMsgId. Returns -1 when the queue is empty.
int DirectPlay_Receive(int* pIdOut, int* pMsgIdOut, int* pLenOut)
{
#ifdef PLATFORM_CURL
    if (dps_RelayIsHost()) jkx_HostPump(); // Added: serving downloads on the relay
#endif
    for (;;)
    {
        uint32_t from = 0, size = 0;
        const void* data = NULL;
        dps_sysmsg sys;

        int r = dps_Receive(&from, &data, &size, &sys);
        if (r == DPS_RECV_NONE)
            return -1;

        if (r == DPS_RECV_DATA)
        {
            uint32_t cap = (uint32_t)*pLenOut;
            if (size > cap)
            {
                stdPlatform_Printf("DirectPlay_Receive: dropping %u-byte message (buffer %u)\n", size, cap);
                continue;
            }
            memcpy(pMsgIdOut, data, size);
            *pLenOut = (int)size;
            *pIdOut = (int)from;
            dps_RelayLastRecvSeq(&DPlay_recvSeq); // Added: Hit what you see
            return 0;
        }

        switch (sys.kind)
        {
            case DPS_SYS_PLAYERCREATED:
                if (sys.playerId == (uint32_t)stdComm_dplayIdSelf)
                    continue;
                *pIdOut = (int)sys.playerId;
                return DPLAY_RET_NEWPLAYER;

            case DPS_SYS_PLAYERDESTROYED:
                if (sys.playerId == (uint32_t)stdComm_dplayIdSelf)
                    continue;
                *pIdOut = (int)sys.playerId;
                return DPLAY_RET_PLAYERLOST;

            case DPS_SYS_SESSIONLOST:
                DPlay_NoteLeaveReason(); // Added: kicked or banned, when the server says
                // Treat as the host leaving; ProcessPlayerLost handles SERVER_LEFT_GAME.
                *pIdOut = (int)sithNet_serverNetId;
                return DPLAY_RET_PLAYERLOST;

            default:
                // Ignored system message: keep draining so data isn't delayed a frame.
                continue;
        }
    }
}

#endif // PLATFORM_DPLAY
