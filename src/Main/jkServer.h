#ifndef _MAIN_JKSERVER_H
#define _MAIN_JKSERVER_H

// Added: a dedicated server's admin commands and rcon (jkServer.c).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JKSERVER_RCON_OUTPUT_MAX  16384
#define JKSERVER_RCON_TIMEOUT_MS  15000   // a command can wait out the scores screen
#define MAIN_MAX_BANS             128
#define JKSERVER_VOTE_MS          30000   // as Quake 3's VOTE_TIME
#define JKSERVER_MAX_CALLVOTES    3       // per player per level, as Quake 3's MAX_VOTE_COUNT

void jkServer_RegisterCommands(void);   // sithCommand_Startup
void jkServer_StartupCvars(void);       // sithCvar_Startup: sv_allowVote
void jkServer_StartupDedicated(void);   // Main_StartupDedicated, once its own server is up
void jkServer_Tick(void);               // every frame: runs a waiting rcon command, and votes
int  jkServer_HandleChat(uint32_t senderNetId, const char* pText); // host: "!callvote"/"!vote" chat

#ifdef __cplusplus
}
#endif

#endif // _MAIN_JKSERVER_H
