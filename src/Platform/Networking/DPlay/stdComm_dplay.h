#ifndef _PLATFORM_NETWORKING_DPLAY_STDCOMM_DPLAY_H
#define _PLATFORM_NETWORKING_DPLAY_STDCOMM_DPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PLATFORM_DPLAY

// Vanilla-compatible networking backend: real Microsoft DirectPlay, so
// OpenJKDF2 and unmodified JK.EXE 1.0 can play together.
void stdComm_DPlay_Startup();

// Nonzero while the selected connection talks to OpenJKDF2 builds that use
// OpenJKDF2's own message layouts (the jkdf2mp web lobby) instead of JK.EXE's.
extern int stdComm_bOpenJkdf2Wire;
extern int stdComm_DPlay_downloadRateKB; // Added: sv_downloadRate (KB/s), see stdComm_dplay.c
extern float stdComm_DPlay_strafeSpeed;  // Added: sv_strafeSpeed (a flex_t cvar), see stdComm_dplay.c
void stdComm_DPlay_StrafeTick(void);     // Added: a host's sv_strafeSpeed into its game's description
float stdComm_DPlay_StrafeScale(void);   // Added: the game's sidestep speed (part of forward), -1 none

// The server the selected connection uses: 0 none (DirectPlay), 1 OpenJKDF2
// Online relay, 2 web lobby. Get/Set read and save its address
// (registry.json: relayServer / webLobbyUrl).
int  stdComm_DPlay_ServerKind(void);
const char* stdComm_DPlay_GetServer(void);
void stdComm_DPlay_SetServer(const char* address);

// Added: opens a connection by name ("relay", "web", "tcpip", "ipx") for a
// dedicated server, with an optional relay server / lobby URL for this run.
// 0 on success, else an HRESULT (the reason is logged).
int stdComm_DPlay_OpenConnectionNamed(const char* pKind, const char* pServer);

// stdComm_Open() results besides DirectPlay's HRESULTs:
//  RESTART: the host runs another mod (or none); the engine restarts into it
//           (g_should_exit is set) and the game is selected again afterwards.
//  HANDLED: the join failed and the player has already been told why.
#define STDCOMM_DPLAY_RESTART ((int)0x4A4B0001)
#define STDCOMM_DPLAY_HANDLED ((int)0x4A4B0002)

// One line for the join screen about a listed game's mod (web lobby), or "".
void stdComm_DPlay_DescribeMod(int idx, uint16_t* pOut, int maxChars);

// For the "gameinfo" console command: the connection in use, and how its
// game packets travel ("" if there's nothing to add).
void stdComm_DPlay_DescribeConnection(char* pOut, int outLen, char* pLinks, int linksLen);

// Called every frame while a menu is up (jkGuiRend): keeps the connection
// alive on the stats screen and in dialogs, where the game doesn't receive.
void stdComm_DPlay_Idle(void);

// Added: registers net_askBeforeDownload (sithCvar_Startup)
void stdComm_DPlay_StartupCvars(void);

// Added: after the engine restarted into a web lobby host's mod, join that game again with
// the connection, password and character picked before (jkGuiMain, jkGuiMultiplayer).
int stdComm_DPlay_AutoJoinPending(void);
int stdComm_DPlay_AutoJoinConnection(void);
const uint16_t* stdComm_DPlay_AutoJoinPassword(void);
const uint16_t* stdComm_DPlay_AutoJoinCharacter(void);
int stdComm_DPlay_AutoJoinFind(void);       // its index in the game list, -1 if not listed
void stdComm_DPlay_AutoJoinDone(int bJoin); // bJoin 1: joining it now, 0: given up
int stdComm_DPlay_AutoJoinPicks(void);      // waiting for one game (else the player picks)

// Added: console "connect <host[:port]> [password]" (sithCommand.c): asks an Imperial Network
// Protocol server (a dedicated server's own, or a relay) for its games. 1 if one or more are
// listed and the menus should go on to join (auto-join above) with character pCharacter
// (force level charRank); pMsg says what happened.
int stdComm_DPlay_Connect(const char* pAddress, const char* pPassword, const char* pCharacter, int charRank,
                          char* pMsg, int msgLen);

// Added: console "reconnect": the last Imperial Network Protocol game joined, again
// (same game, or the server's only one or the one with the same name), with pCharacter.
int stdComm_DPlay_Reconnect(const char* pCharacter, int charRank, int bAsked, char* pMsg, int msgLen);

// Added: the map in the game's description (a dedicated host's map rotation, jkMain.c).
void stdComm_DPlay_SetSessionMap(const char* pGob, const char* pJklFname);
int stdComm_DPlay_GetSessionNextMap(char* pGob, int gobLen, char* pOut, int outLen);

// Added: the force level limit of the game we're in (console "character"), -1 if none.
int stdComm_DPlay_GameMaxRank(void);
int stdComm_DPlay_SetMaxRank(int rank); // Added: server "rank"
int stdComm_DPlay_TakeLeaveMessage(uint16_t* pOut, int outLen);
int stdComm_DPlay_RejoinAsked(void);     // Added: the server asked everyone to reconnect
int stdComm_DPlay_TakeRejoinAsked(void); // Added: the same, once // Added: "You were kicked/banned..." once
int stdComm_DPlay_JoinRefusalText(uint16_t* pOut, int outLen); // Added: "You are banned..."
// Added: console "password" and "rcon" (Main/jkServer.c).
int stdComm_DPlay_SetPassword(const char* pPassword);
void stdComm_DPlay_RconServer(char* pOut, int outLen);
int stdComm_DPlay_RconGuid(uint8_t guid[16]);
void stdComm_DPlay_StartTunnel(void);
// Added: the game's name as hosted ("gameinfo"), "" if not in a game.
void stdComm_DPlay_GetGameName(char* pOut, int outLen);

// Added: a dedicated server's own server for "connect" (Main_StartupDedicated), listed on
// master server pMaster ("host[:port]", or NULL). 0 on failure.
int stdComm_DPlay_Listen(int port, int wsPort, const char* pMaster);
// A joiner following its host into another episode: gets the host's copy if needed (1 ok).
int stdComm_DPlay_FetchHostMap(const char* pEpisode);

// Hit what you see (Platform/Networking/Hits/stdComm_Hits.c): a clock in ms, when this
// player's position update numbered seq went to player toId (-1 if unknown), the number
// the packet being handled came with (1 if it had one), and whether the rules' messages
// need sithComm's resends.
double stdComm_DPlay_Now(void);
double stdComm_DPlay_SeqSentAt(int toId, uint32_t seq);
int stdComm_DPlay_RecvSeq(uint32_t* pSeq);
int stdComm_DPlay_HitsResend(void);
void stdComm_DPlay_SyncClock(int netId, int bOn);
double stdComm_DPlay_ClockOffset(int netId);

#endif // PLATFORM_DPLAY

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_DPLAY_STDCOMM_DPLAY_H
