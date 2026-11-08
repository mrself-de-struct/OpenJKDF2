#include "sithCommand.h"

#include "stdPlatform.h" // Added: *_ALLOC/*_FREE macros

#include "General/sithStrTable.h"
#include "General/stdString.h"
#include "Devices/sithConsole.h"
#include "AI/sithAI.h"
#include "Main/jkGame.h"
#include "Cog/sithCog.h"
#include "World/sithThing.h"
#include "Gameplay/sithPlayerActions.h"
#include "Gameplay/sithEvent.h"
#include "Engine/sithIntersect.h"
#include "Dss/sithMulti.h"
#include "Win95/stdComm.h"
#include "World/sithWorld.h"
#include "World/sithMaterial.h"
#include "Gameplay/sithPlayer.h"
#include "World/sithTemplate.h"
#include "Main/jkQuakeConsole.h"
#include "General/stdJSON.h"
#include "General/stdFnames.h"
#include "General/stdStrTable.h"
#include "Gui/jkGUITitle.h"
#include "Main/jkStrings.h"
#include "jk.h"
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
#include "Main/Main.h"
#include "World/jkPlayer.h"
#include "Gui/jkGUIMain.h"
#include "Main/jkMain.h"
#include "General/stdFileUtil.h"
#include "General/util.h"
#include "Platform/wuRegistry.h"
#include "Main/jkServer.h"
#include "Gui/jkGUIPlayer.h"
#include "Platform/Common/stdLog.h"
#ifdef PLATFORM_POSIX
#include <unistd.h> // Added: isatty (clear)
#endif
#endif

typedef struct sithCommandBind sithCommandBind;
typedef struct sithCommandBind
{
    uint16_t key;
    const char* pCmd;
    sithCommandBind* pNext;
} sithCommandBind;

sithCommandBind* sithCommand_pBinds;

static char sithCommand_aIdk[4]; // TODO symbols?

#ifdef QOL_IMPROVEMENTS
int sithCommand_CmdQuit(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    // Altered: as closing the window does (settings saved; a multiplayer game is left
    // properly on the way out, stdComm_dplay.c's exit hook), not exit(0) on the spot,
    // which left the server waiting for a timeout to notice.
    g_should_exit = 1;
    return 1;
}
#endif

// Added: what kind of multiplayer game this is (connection, role, tick rate).
// Added (2026-11-08): "clear" empties the console, as Quake 3's does. On a headless
// (dedicated) server, whose console is the terminal, it clears the terminal's screen.
int sithCommand_CmdClear(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    jkQuakeConsole_Clear();
#ifdef PLATFORM_POSIX
    if ( Main_bHeadless && isatty(STDOUT_FILENO) )
        stdLog_TerminalPrintf("\033[2J\033[H");
#endif
    return 1;
}

int sithCommand_CmdGameInfo(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    if ( !sithNet_isMulti )
    {
        sithConsole_PrintString("Not in a multiplayer game.");
        return 1;
    }

#ifdef PLATFORM_DPLAY
    char links[128];
    char conn[384];
    char gameName[64];
    stdComm_DPlay_GetGameName(gameName, sizeof(gameName));
    if ( gameName[0] )
    {
        stdString_snprintf(std_g_genBuffer, 512, "Game: %s", gameName);
        sithConsole_PrintString(std_g_genBuffer);
    }
    stdComm_DPlay_DescribeConnection(conn, sizeof(conn), links, sizeof(links));
    stdString_snprintf(std_g_genBuffer, 512, "Connection: %s", conn);
    sithConsole_PrintString(std_g_genBuffer);
    if ( links[0] )
        sithConsole_PrintString(links);
    sithConsole_PrintString(stdComm_bOpenJkdf2Wire ? "Messages: OpenJKDF2 layouts (web lobby, Imperial Network Protocol)" : "Messages: JK.EXE layouts");
#endif

    // The episode and level, by title when the episode or cog strings have one.
    const char16_t* episodeTitle = NULL;
    for (uint32_t i = 0; i < jkEpisode_var2; i++)
    {
        if ( !__strcmpi(jkEpisode_aEpisodes[i].name, jkRes_episodeGobName) && jkEpisode_aEpisodes[i].unistr[0] )
        {
            episodeTitle = jkEpisode_aEpisodes[i].unistr;
            break;
        }
    }
    // (no %S: it means wchar_t on Linux, not char16_t)
    char title[128];
    if ( episodeTitle )
    {
        stdString_WcharToChar(title, episodeTitle, sizeof(title) - 1);
        title[sizeof(title) - 1] = 0;
        stdString_snprintf(std_g_genBuffer, 512, "Episode: %s (%s)", title, jkRes_episodeGobName);
    }
    else
        stdString_snprintf(std_g_genBuffer, 512, "Episode: %s", jkRes_episodeGobName);
    sithConsole_PrintString(std_g_genBuffer);

    if ( sithWorld_g_pCurrentWorld )
    {
        char key[64];
        const char16_t* levelTitle;
        stdFnames_CopyShortName(key, sizeof(key), sithWorld_g_pCurrentWorld->map_jkl_fname);
        jkGuiTitle_sub_4189A0(key);
        levelTitle = stdStrTable_GetValue(&jkStrings_tableExtOver, key);
        if ( !levelTitle )
            levelTitle = stdStrTable_GetValue(&jkCog_strings, key);
        if ( levelTitle )
        {
            stdString_WcharToChar(title, levelTitle, sizeof(title) - 1);
            title[sizeof(title) - 1] = 0;
            stdString_snprintf(std_g_genBuffer, 512, "Level: %s (%s)", title, sithWorld_g_pCurrentWorld->map_jkl_fname);
        }
        else
            stdString_snprintf(std_g_genBuffer, 512, "Level: %s", sithWorld_g_pCurrentWorld->map_jkl_fname);
        sithConsole_PrintString(std_g_genBuffer);
    }

    // Players in the game (the web lobby's transport always reports all 32 slots).
    int numPlayers = 0;
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        if ( jkPlayer_playerInfos[i].flags & 1 )
            numPlayers++;
    }
    stdString_snprintf(std_g_genBuffer, 512, "You are the %s, %d player%s, tick rate %d msec",
                       sithNet_isServer ? "host" : "joiner", numPlayers, numPlayers == 1 ? "" : "s", sithNet_tickrate);
    sithConsole_PrintString(std_g_genBuffer);
#ifdef PLATFORM_DPLAY
    // Added: sv_strafeSpeed
    if ( stdComm_DPlay_StrafeScale() >= 0.0 )
        stdString_snprintf(std_g_genBuffer, 512, "Sidestep speed: %.2f of forward (set by the server; JK.EXE's is 0.70)",
                           (double)stdComm_DPlay_StrafeScale());
    else
        stdString_snprintf(std_g_genBuffer, 512, "Sidestep speed: your own (g_bEqualStrafeSpeed %d)", jkPlayer_bEqualStrafeSpeed);
    sithConsole_PrintString(std_g_genBuffer);
#endif
    return 1;
}

#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
// Added: "characters", "character <name>" and "connect <host[:port]> [password]":
// join a server from the console, with a multiplayer character picked here
// instead of on the character screen. The pick is saved ("mpCharacter").

#define SITHCOMMAND_MAX_CHARS 32

typedef struct sithCommandChar
{
    char name[32];       // the .mpc file name without ".mpc", as the character screen lists it
    char model[32];      // e.g. "ky.3do"
    int  rank;           // force level
    int  bReadable;
} sithCommandChar;

// The current player's multiplayer characters, as the character screen finds them.
// On the player screen (no player picked yet) the last one picked. -1 (and says
// so) without any.
static int sithCommand_ListChars(sithCommandChar* pOut, int max)
{
    char player[32], path[128], fpath[192];
    stdFileSearch* pSearch;
    stdFileSearchResult result;
    int n = 0;

    if ( !jkGuiPlayer_UseLastProfile() )
    {
        sithConsole_PrintString("No player profile yet: make one on the player screen.");
        return -1;
    }

    stdString_WcharToChar(player, jkPlayer_playerShortName, 31);
    player[31] = 0;
    stdString_snprintf(path, sizeof(path), "player\\%s", player);
    pSearch = stdFileUtil_NewFind(path, 3, "mpc");
    if ( !pSearch )
        return 0;
    while ( n < max && stdFileUtil_FindNext(pSearch, &result) )
    {
        stdString_snprintf(fpath, sizeof(fpath), "%s\\%s", path, result.fpath);
        if ( !util_FileExistsLowLevel(fpath) )
            continue;
        sithCommandChar* c = &pOut[n++];
        stdString_SafeStrCopy(c->name, result.fpath, sizeof(c->name));
        stdFnames_StripExtAndDot(c->name);
        c->bReadable = jkPlayer_MPCPeek(player, c->name, c->model, sizeof(c->model), &c->rank);
    }
    stdFileUtil_DisposeFind(pSearch);
    return n;
}

// The model as the character screen names it ("Kyle Katarn", "Stormtrooper", ...).
static void sithCommand_ModelName(const char* pModel, char* pOut, int outLen)
{
    char key[32];
    const char16_t* pName;
    stdFnames_CopyShortName(key, 16, (char*)pModel);
    jkGuiTitle_sub_4189A0(key);
    pName = jkStrings_GetUniStringWithFallback(key);
    if ( pName && pName[0] )
    {
        stdString_WcharToChar(pOut, pName, outLen - 1);
        pOut[outLen - 1] = 0;
    }
    else
        stdString_SafeStrCopy(pOut, pModel, outLen);
}

// A name or list number as typed: surrounding spaces and quotes go.
static void sithCommand_TrimArg(const char* pIn, char* pOut, int outLen)
{
    int len;
    while ( pIn && (*pIn == ' ' || *pIn == '\t') ) pIn++;
    stdString_SafeStrCopy(pOut, pIn ? pIn : "", outLen);
    len = (int)strlen(pOut);
    while ( len && (pOut[len - 1] == ' ' || pOut[len - 1] == '\t') ) pOut[--len] = 0;
    if ( len >= 2 && pOut[0] == '"' && pOut[len - 1] == '"' )
    {
        memmove(pOut, pOut + 1, len - 2);
        pOut[len - 2] = 0;
    }
}

// The saved pick, if it's still one of this player's readable characters. -1 if not.
static int sithCommand_SelectedChar(const sithCommandChar* pChars, int n)
{
    char saved[32] = { 0 };
    wuRegistry_GetStr("mpCharacter", saved, sizeof(saved), "");
    for ( int i = 0; saved[0] && i < n; i++ )
        if ( pChars[i].bReadable && !__strcmpi(pChars[i].name, saved) )
            return i;
    return -1;
}

int sithCommand_CmdCharacters(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    sithCommandChar chars[SITHCOMMAND_MAX_CHARS];
    int n = sithCommand_ListChars(chars, SITHCOMMAND_MAX_CHARS);
    int sel = sithCommand_SelectedChar(chars, n);
    char model[64];

    if ( n < 0 )
        return 1;
    if ( !n )
    {
        sithConsole_PrintString("No multiplayer characters yet: make one on the character screen (Multiplayer, Join or Host).");
        return 1;
    }
    for ( int i = 0; i < n; i++ )
    {
        if ( chars[i].bReadable )
        {
            sithCommand_ModelName(chars[i].model, model, sizeof(model));
            stdString_snprintf(std_g_genBuffer, 512, "%2d  %-20s %-16s force level %d%s", i + 1, chars[i].name, model,
                               chars[i].rank, i == sel ? "  (selected)" : "");
        }
        else
            stdString_snprintf(std_g_genBuffer, 512, "%2d  %-20s (can't be read)", i + 1, chars[i].name);
        sithConsole_PrintString(std_g_genBuffer);
    }
    sithConsole_PrintString("Pick one with: character <name or number>");
    return 1;
}

int sithCommand_CmdCharacter(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    sithCommandChar chars[SITHCOMMAND_MAX_CHARS];
    int n = sithCommand_ListChars(chars, SITHCOMMAND_MAX_CHARS);
    char arg[64], model[64];
    int idx = -1, maxRank;

    if ( n < 0 )
        return 1;
    sithCommand_TrimArg(pArgStr, arg, sizeof(arg));
    if ( !arg[0] )
    {
        int sel = sithCommand_SelectedChar(chars, n);
        if ( sel < 0 )
            sithConsole_PrintString("No character picked. \"characters\" lists them; pick one with: character <name or number>");
        else
        {
            stdString_snprintf(std_g_genBuffer, 512, "Character: %s", chars[sel].name);
            sithConsole_PrintString(std_g_genBuffer);
        }
        return 1;
    }
    // The name (any case, spaces and all), else the number "characters" shows.
    for ( int i = 0; i < n && idx < 0; i++ )
        if ( !__strcmpi(chars[i].name, arg) ) idx = i;
    if ( idx < 0 && arg[0] >= '0' && arg[0] <= '9' && atoi(arg) >= 1 && atoi(arg) <= n && strspn(arg, "0123456789") == strlen(arg) )
        idx = atoi(arg) - 1;
    if ( idx < 0 )
    {
        stdString_snprintf(std_g_genBuffer, 512, "No character \"%s\". \"characters\" lists them.", arg);
        sithConsole_PrintString(std_g_genBuffer);
        return 1;
    }
    if ( !chars[idx].bReadable )
    {
        stdString_snprintf(std_g_genBuffer, 512, "%s can't be read.", chars[idx].name);
        sithConsole_PrintString(std_g_genBuffer);
        return 1;
    }
    // Not above the force level of the game we're in (connect checks the server's).
    maxRank = stdComm_DPlay_GameMaxRank();
    if ( maxRank >= 0 && chars[idx].rank > maxRank )
    {
        stdString_snprintf(std_g_genBuffer, 512, "%s is force level %d; this game allows up to %d.", chars[idx].name, chars[idx].rank, maxRank);
        sithConsole_PrintString(std_g_genBuffer);
        return 1;
    }
    wuRegistry_SaveStr("mpCharacter", chars[idx].name);
    sithCommand_ModelName(chars[idx].model, model, sizeof(model));
    stdString_snprintf(std_g_genBuffer, 512, "Character: %s (%s, force level %d)%s", chars[idx].name, model, chars[idx].rank,
                       sithNet_isMulti ? ", from your next join" : "");
    sithConsole_PrintString(std_g_genBuffer);
    return 1;
}

#define SITHCOMMAND_MAX_PLAYERS 64

// The player profiles, as the player screen finds them (player\<name>\<name>.plr).
static int sithCommand_ListPlayers(char (*pOut)[32], int max)
{
    char fpath[192];
    stdFileSearch* pSearch = stdFileUtil_NewFind("player", 2, 0);
    stdFileSearchResult result;
    int n = 0;
    if ( !pSearch )
        return 0;
    while ( n < max && stdFileUtil_FindNext(pSearch, &result) )
    {
        if ( !result.is_subdirectory || result.fpath[0] == '.' )
            continue;
        stdString_snprintf(fpath, sizeof(fpath), "player%c%s%c%s.plr", LEC_PATH_SEPARATOR_CHR, result.fpath, LEC_PATH_SEPARATOR_CHR, result.fpath);
        if ( !util_FileExistsLowLevel(fpath) )
            continue;
        stdString_SafeStrCopy(pOut[n++], result.fpath, 32);
    }
    stdFileUtil_DisposeFind(pSearch);
    return n;
}

// The profile in use, else (on the player screen) the last one picked.
static void sithCommand_CurrentPlayer(char* pOut, int outLen)
{
    stdString_WcharToChar(pOut, jkPlayer_playerShortName, outLen - 1);
    pOut[outLen - 1] = 0;
    if ( !pOut[0] )
        wuRegistry_GetStr("playerShortName", pOut, outLen, "");
}

int sithCommand_CmdProfiles(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    char players[SITHCOMMAND_MAX_PLAYERS][32], cur[32];
    int n = sithCommand_ListPlayers(players, SITHCOMMAND_MAX_PLAYERS);

    if ( !n )
    {
        sithConsole_PrintString("No player profiles yet: make one on the player screen.");
        return 1;
    }
    sithCommand_CurrentPlayer(cur, sizeof(cur));
    for ( int i = 0; i < n; i++ )
    {
        stdString_snprintf(std_g_genBuffer, 512, "%2d  %s%s", i + 1, players[i], !__strcmpi(players[i], cur) ? "  (selected)" : "");
        sithConsole_PrintString(std_g_genBuffer);
    }
    sithConsole_PrintString("Pick one with: profile <name or number>");
    return 1;
}

// profile <name|number>: switches player profiles (on the player screen: picks it and goes on).
int sithCommand_CmdProfile(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    char players[SITHCOMMAND_MAX_PLAYERS][32], arg[64], cur[32];
    char16_t wName[32];
    int n, idx = -1;

    sithCommand_TrimArg(pArgStr, arg, sizeof(arg));
    if ( !arg[0] )
    {
        sithCommand_CurrentPlayer(cur, sizeof(cur));
        stdString_snprintf(std_g_genBuffer, 512, cur[0] ? "Player profile: %s" : "No player profile picked. \"profiles\" lists them.", cur);
        sithConsole_PrintString(std_g_genBuffer);
        return 1;
    }
    if ( Main_bDedicatedServer )
    {
        sithConsole_PrintString("A dedicated server has no player profile.");
        return 1;
    }
    if ( !jkGuiMain_CanSwitchPlayer() )
    {
        sithConsole_PrintString("profile works on the main menu, the player screen or the Multiplayer menu (not in a game).");
        return 1;
    }
    n = sithCommand_ListPlayers(players, SITHCOMMAND_MAX_PLAYERS);
    for ( int i = 0; i < n && idx < 0; i++ )
        if ( !__strcmpi(players[i], arg) ) idx = i;
    if ( idx < 0 && atoi(arg) >= 1 && atoi(arg) <= n && strspn(arg, "0123456789") == strlen(arg) )
        idx = atoi(arg) - 1;
    if ( idx < 0 )
    {
        stdString_snprintf(std_g_genBuffer, 512, "No player profile \"%s\". \"profiles\" lists them.", arg);
        sithConsole_PrintString(std_g_genBuffer);
        return 1;
    }
    stdString_CharToWchar(wName, players[idx], 31);
    wName[31] = 0;
    if ( !jkPlayer_ReadConf(wName) )
    {
        jkPlayer_playerShortName[0] = 0;
        stdString_snprintf(std_g_genBuffer, 512, "%s can't be read.", players[idx]);
        sithConsole_PrintString(std_g_genBuffer);
        return 1;
    }
    wuRegistry_SaveStr("playerShortName", players[idx]);
    jkGuiPlayer_ConnectNow(1); // on the player screen: done with it
    stdString_snprintf(std_g_genBuffer, 512, "Player profile: %s", players[idx]);
    sithConsole_PrintString(std_g_genBuffer);
    return 1;
}

// Leaves the multiplayer game for the main menu, as Esc -> Abort does (which
// disconnects: sithMulti_Shutdown closes the connection).
int sithCommand_CmdDisconnect(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    if ( !sithNet_isMulti )
    {
        sithConsole_PrintString("Not in a multiplayer game.");
        return 1;
    }
    if ( Main_bDedicatedServer )
    {
        sithConsole_PrintString("A dedicated server keeps its game; \"quit\" stops it.");
        return 1;
    }
    sithConsole_PrintString(sithNet_isServer ? "Leaving; you're the host, so the game ends for everyone." : "Disconnecting.");
    jkQuakeConsole_Close();
    jkMain_MenuReturn();
    return 1;
}

// The character "connect"/"reconnect" join with: the one picked with "character" (or
// last joined with), else the only one there is. -1 (and says why) if none.
static int sithCommand_JoinChar(const sithCommandChar* pChars, int n)
{
    int sel = sithCommand_SelectedChar(pChars, n), readable = 0;
    if ( sel >= 0 )
        return sel;
    for ( int i = 0; i < n; i++ )
        if ( pChars[i].bReadable ) { readable++; sel = i; }
    if ( readable == 1 )
        return sel;
    sithConsole_PrintString(readable ? "Pick a character first: character <name or number> (\"characters\" lists them)."
                                     : "No multiplayer characters yet: make one on the character screen (Multiplayer, Join or Host).");
    return -1;
}

int sithCommand_CmdReconnect(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    sithCommandChar chars[SITHCOMMAND_MAX_CHARS];
    char msg[256];
    int n, sel;

    if ( Main_bDedicatedServer )
    {
        sithConsole_PrintString("A dedicated server can't join games.");
        return 1;
    }
    if ( !jkGuiMain_ConnectNow(0) )
    {
        sithConsole_PrintString("reconnect works in a game, on the main menu, the player screen or the Multiplayer menu.");
        return 1;
    }
    n = sithCommand_ListChars(chars, SITHCOMMAND_MAX_CHARS);
    if ( n < 0 )
        return 1;
    sel = sithCommand_JoinChar(chars, n);
    if ( sel < 0 )
        return 1;
    if ( !stdComm_DPlay_Reconnect(chars[sel].name, chars[sel].rank, 0, msg, sizeof(msg)) )
    {
        sithConsole_PrintString(msg);
        return 1;
    }
    sithConsole_PrintString(msg);
    jkQuakeConsole_Close();
    jkGuiMain_ConnectNow(1);
    return 1;
}

// The server asked everyone to join again (its admin's reconnectall, e.g. after
// lowering the force rank): back on the main menu, join the same game with the
// character last joined with, or on the character screen if it's above the limit now.
void sithCommand_RejoinAsked(void)
{
    sithCommandChar chars[SITHCOMMAND_MAX_CHARS];
    char msg[256];
    int n = sithCommand_ListChars(chars, SITHCOMMAND_MAX_CHARS);
    int sel = n > 0 ? sithCommand_SelectedChar(chars, n) : -1;
    if ( !stdComm_DPlay_Reconnect(sel >= 0 ? chars[sel].name : "", sel >= 0 ? chars[sel].rank : 0, 1, msg, sizeof(msg)) )
        stdLog_Printf("net", "reconnect: %s", msg);
    sithConsole_PrintString(msg);
}

int sithCommand_CmdConnect(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    sithCommandChar chars[SITHCOMMAND_MAX_CHARS];
    char args[300], msg[256];
    char* pAddress;
    char* pPassword;
    int n, sel;

    sithCommand_TrimArg(pArgStr, args, sizeof(args));
    pAddress = _strtok(args, " \t");
    pPassword = pAddress ? _strtok(NULL, " \t") : NULL;
    if ( !pAddress )
    {
        sithConsole_PrintString("connect <address>[:port] [password]   (port 27960 if none given)");
        return 1;
    }
    if ( Main_bDedicatedServer )
    {
        sithConsole_PrintString("A dedicated server can't join games.");
        return 1;
    }
    if ( !jkGuiMain_ConnectNow(0) )
    {
        sithConsole_PrintString("connect works in a game, on the main menu, the player screen or the Multiplayer menu.");
        return 1;
    }

    n = sithCommand_ListChars(chars, SITHCOMMAND_MAX_CHARS);
    if ( n < 0 )
        return 1;
    sel = sithCommand_JoinChar(chars, n);
    if ( sel < 0 )
        return 1;

    if ( !stdComm_DPlay_Connect(pAddress, pPassword, chars[sel].name, chars[sel].rank, msg, sizeof(msg)) )
    {
        sithConsole_PrintString(msg);
        return 1;
    }
    sithConsole_PrintString(msg);
    jkQuakeConsole_Close();
    jkGuiMain_ConnectNow(1);
    return 1;
}
#endif

// MOTS altered
void sithCommand_Startup()
{
    sithConsole_RegisterCommand(sithCommand_Players, "players", 0);
    sithConsole_RegisterCommand(sithCommand_Coords, "coords", 0);
    sithConsole_RegisterCommand(sithCommand_DebugMode, "trackshots", 3);
    sithConsole_RegisterCommand(sithCommand_PingPlayer, "ping", 0);
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
    // Added: kick is jkServer.c's (by slot too, and it sticks)
#else
    sithConsole_RegisterCommand(sithCommand_Kick, "kick", 0);
#endif
    sithConsole_RegisterCommand(sithCommand_Kick, "boot", 0); // MOTS added
    sithConsole_RegisterCommand(sithCommand_Tick, "tick", 0);
    sithConsole_RegisterCommand(sithCommand_Session, "session", 0);

    if ( (g_debugmodeFlags & DEBUGFLAG_IN_EDITOR) != 0 )
    {
        sithConsole_RegisterCommand(sithConsole_Help, "help", 0);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "disableai", 0);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "notarget", 6);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "outline", 1);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "disablepuppet", 2);
        sithConsole_RegisterCommand(sithCommand_CogTrace, "cogtrace", 0);
        sithConsole_RegisterCommand(sithCommand_CogList, "coglist", 0);
        sithConsole_RegisterCommand(sithCog_CogStatus, "cogstatus", 0);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "noaishots", 4);
        sithConsole_RegisterCommand(sithAI_AIStatus, "aistatus", 0);
        sithConsole_RegisterCommand(sithAI_AIList, "ailist", 0);
        sithConsole_RegisterCommand(sithCommand_Fly, "fly", 0);
        sithConsole_RegisterCommand(sithCommand_Memory, "mem", 0);
        sithConsole_RegisterCommand(sithCommand_DynamicMemory, "dynamicmem", 0);
        sithConsole_RegisterCommand(sithCommand_MemoryDump, "memdump", 0);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "invul", 5);
        sithConsole_RegisterCommand(sithCommand_CogPause, "cogpause", 0);
#ifndef LINUX_TMP
        sithConsole_RegisterCommand(sithCommand_MatList, "matlist", 0);
#endif
        sithConsole_RegisterCommand(sithCommand_Warp, "warp", 0);
        sithConsole_RegisterCommand(sithCommand_Activate, "activate", 0);
        sithConsole_RegisterCommand(sithCommand_DebugMode, "slowmo", 7);
        sithConsole_RegisterCommand(sithCommand_Jump, "jump", 0);
    }

#ifdef QOL_IMPROVEMENTS
    sithConsole_RegisterCommand(sithCommand_CmdQuit, "quit", 0);
    sithConsole_RegisterCommand(sithCommand_CmdQuit, "q", 0);
    sithConsole_RegisterCommand(sithCommand_CmdThingNpc, "npc", 0);
    sithConsole_RegisterCommand(sithCommand_CmdThingNpc, "thing", 0);
    sithConsole_RegisterCommand(sithCommand_CmdBind, "bind", 0);
    sithConsole_RegisterCommand(sithCommand_CmdUnbind, "unbind", 0);
    sithConsole_RegisterCommand(sithCommand_CmdGameInfo, "gameinfo", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdClear, "clear", 0); // Added
#ifdef PLATFORM_DPLAY
    sithConsole_RegisterCommand(sithCommand_CmdCharacters, "characters", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdCharacter, "character", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdProfiles, "profiles", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdProfile, "profile", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdConnect, "connect", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdDisconnect, "disconnect", 0); // Added
    sithConsole_RegisterCommand(sithCommand_CmdReconnect, "reconnect", 0); // Added
    jkServer_RegisterCommands(); // Added: status, kick, ban, map, rcon... (Main/jkServer.c)
#endif

    sithCommand_StartupBinds();
#endif
}

int sithCommand_DebugMode(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    int *v2; // esi
    int v3; // edi
    SithThing *v4; // eax
    char16_t *v5; // eax
    char16_t *v6; // eax
    int result; // eax
    char16_t *v8; // [esp-4h] [ebp-154h]
    char16_t v9[32]; // [esp+10h] [ebp-140h] BYREF
    char16_t a1[128]; // [esp+50h] [ebp-100h] BYREF

    switch ( pCmd->extra )
    {
        case 0u:
            v2 = &g_debugmodeFlags;
            v3 = 1;
            goto LABEL_13;
        case 1u:
            v2 = &sithRender_renderflags;
            v3 = 1;
            goto LABEL_13;
        case 2u:
            v2 = &g_debugmodeFlags;
            v3 = 2;
            goto LABEL_13;
        case 3u:
            v2 = &g_debugmodeFlags;
            v3 = 64;
            goto LABEL_13;
        case 4u:
            v2 = &g_debugmodeFlags;
            v3 = 128;
            goto LABEL_13;
        case 5u:
            if ( !sithWorld_g_pCurrentWorld )
                goto LABEL_24;
            v4 = sithWorld_g_pCurrentWorld->pLocalPlayer;
            if ( !v4 || v4->type != SITH_THING_PLAYER )
                goto LABEL_24;
            v2 = (int*)&v4->actorParams.flags;
            v3 = 8;
LABEL_13:
            if ( pArgStr )
            {
                if ( __strcmpi(pArgStr, "on") && __strcmpi(pArgStr, "1") )
                {
                    if ( !__strcmpi(pArgStr, "off") || !__strcmpi(pArgStr, "0") )
                        *v2 &= ~v3;
                }
                else
                {
                    *v2 |= v3;
                }
            }
            stdString_CharToWchar(v9, pCmd->cmdStr, 31);
            v9[31] = 0;
            if ( (*v2 & v3) != 0 )
                v5 = sithStrTable_GetUniStringWithFallback("ON");
            else
                v5 = sithStrTable_GetUniStringWithFallback("OFF");
            v8 = v5;
            v6 = sithStrTable_GetUniStringWithFallback("%s_IS_%s");
            jk_snwprintf(a1, 0x80u, v6, v9, v8);
            sithConsole_PrintWString(a1);
            result = 1;
            break;
        case 6u:
            v2 = &g_debugmodeFlags;
            v3 = 0x200;
            goto LABEL_13;
        case 7u:
            v2 = &g_debugmodeFlags;
            v3 = 0x400;
            goto LABEL_13;
        default:
LABEL_24:
            result = 0;
            break;
    }
    return result;
}

int sithCommand_Tick(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    signed int result; // eax
    unsigned int newTickrate; // eax

    if ( !sithNet_isMulti )
        return 0;
    if ( pArgStr && (newTickrate = _atoi(pArgStr)) != 0 )
    {
        if ( newTickrate >= TICKRATE_MIN && newTickrate <= TICKRATE_MAX) // Altered: same range as the host screen (was 100-300)
        {
            sithNet_tickrate = newTickrate;
            sithEvent_RegisterTask(2, sithMulti_CheckPlayers, newTickrate, 1);
            result = 1;
        }
        else
        {
            _sprintf(std_g_genBuffer, "New tick is out of range (%d-%d msec)", TICKRATE_MIN, TICKRATE_MAX); // Added: say the range
            sithConsole_PrintString(std_g_genBuffer);
            result = 0;
        }
    }
    else
    {
        _sprintf(std_g_genBuffer, "Current tick rate is %d msec", sithNet_tickrate);
        sithConsole_PrintString(std_g_genBuffer);
        result = 1;
    }
    return result;
}

int sithCommand_Session(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    uint32_t v3; // edi
    sithDplayPlayer* v4; // esi
    unsigned int v5; // eax

    if ( !sithNet_isMulti )
        return 0;
    DirectPlay_EnumPlayers(0);
    _sprintf(std_g_genBuffer, "%d players in session", DirectPlay_numPlayers);
    sithConsole_PrintString(std_g_genBuffer);
    v3 = 0;
    if ( DirectPlay_numPlayers )
    {
        v4 = &DirectPlay_aPlayers[0];
        do
        {
            v5 = sithMulti_GetPlayerIndexByID(v4->field_80);
            // Altered: char16_t name printed with %s (%S is wchar_t off Windows); unknown id no longer reads jkPlayer_playerInfos[-1]
            char name[32] = "?";
            if ( (int)v5 >= 0 )
            {
                stdString_WcharToChar(name, jkPlayer_playerInfos[v5].player_name, 31);
                name[31] = 0;
            }
            stdString_snprintf(std_g_genBuffer, 512, "Player %x (%s) is in the session", v4->field_80, name);
            sithConsole_PrintString(std_g_genBuffer);
            ++v3;
            ++v4;
        }
        while ( v3 < DirectPlay_numPlayers );
    }
    return 1;
}

int sithCommand_CogTrace(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    signed int result; // eax
    unsigned int v3; // eax
    sithCog *v4; // esi

    if ( sithWorld_g_pCurrentWorld )
    {
        if ( pArgStr )
        {
            v3 = _atoi(pArgStr);
            if ( v3 < sithWorld_g_pCurrentWorld->numCogs )
            {
                v4 = &sithWorld_g_pCurrentWorld->aCogs[v3];
                if ( (v4->flags & SITH_COG_DEBUG) != 0 )
                {
                    sithConsole_PrintString("Cog trace disabled.");
                    v4->flags &= ~SITH_COG_DEBUG;
                }
                else
                {
                    sithConsole_PrintString("Cog trace enabled.");
                    v4->flags |= SITH_COG_DEBUG;
                }
                return 1;
            }
            else
            {
                sithConsole_PrintString("cog id out of range.");
                result = 0;
            }
        }
        else
        {
            sithConsole_PrintString("syntax error.");
            result = 0;
        }
    }
    else
    {
        sithConsole_PrintString("no world open.");
        result = 0;
    }
    return result;
}

int sithCommand_CogPause(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    signed int result; // eax
    unsigned int v3; // eax
    sithCog *v4; // esi
    int v5; // eax

    if ( sithWorld_g_pCurrentWorld )
    {
        if ( pArgStr )
        {
            v3 = _atoi(pArgStr);
            if ( v3 < sithWorld_g_pCurrentWorld->numCogs )
            {
                v4 = &sithWorld_g_pCurrentWorld->aCogs[v3];
                if ( (v4->flags & SITH_COG_DISABLED) != 0 )
                {
                    sithConsole_PrintString("Cog enabled.");
                    v4->flags &= ~SITH_COG_DISABLED;
                }
                else
                {
                    sithConsole_PrintString("Cog disabled.");
                    v4->flags |= SITH_COG_DISABLED;
                }
                result = 1;
            }
            else
            {
                sithConsole_PrintString("cog id out of range.");
                result = 0;
            }
        }
        else
        {
            sithConsole_PrintString("syntax error.");
            result = 0;
        }
    }
    else
    {
        sithConsole_PrintString("No world.");
        result = 0;
    }
    return result;
}

int sithCommand_CogList(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    signed int result; // eax
    unsigned int v3; // ebp
    sithCog *i; // esi

    if ( sithWorld_g_pCurrentWorld )
    {
#ifdef SITH_DEBUG_STRUCT_NAMES
        _sprintf(std_g_genBuffer, "World cogs = %d.", sithWorld_g_pCurrentWorld->numCogs);
        sithConsole_PrintString(std_g_genBuffer);
        v3 = 0;
        for ( i = sithWorld_g_pCurrentWorld->aCogs; v3 < sithWorld_g_pCurrentWorld->numCogs; ++i )
        {
            _sprintf(std_g_genBuffer, "%d: %-16s %-16s ", v3, i->aName, i->pScript->aName);
            if ( (i->flags & SITH_COG_DISABLED) != 0 )
                _sprintf(&std_g_genBuffer[strlen(std_g_genBuffer)], "(paused) ");
            if ( (i->flags & SITH_COG_DEBUG) != 0 )
                _sprintf(&std_g_genBuffer[strlen(std_g_genBuffer)], "(trace)  ");
            _sprintf(&std_g_genBuffer[strlen(std_g_genBuffer)], sithCommand_aIdk);
            sithConsole_PrintString(std_g_genBuffer);
            ++v3;
        }
#endif
        result = 1;
    }
    else
    {
        sithConsole_PrintString("No world.");
        result = 0;
    }
    return result;
}

int sithCommand_Fly(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    SithThing *v0; // ecx
    char16_t *v3; // eax

    if ( sithWorld_g_pCurrentWorld && (v0 = sithWorld_g_pCurrentWorld->pLocalPlayer) != 0 )
    {
        if ( v0->moveType == SITH_MT_PHYSICS )
        {
            if (v0->physicsParams.flags & SITH_PF_FLY)
            {
                v0->physicsParams.flags &= ~SITH_PF_FLY;
                v0->physicsParams.flags |= SITH_PF_USEGRAVITY;
                v3 = sithStrTable_GetUniStringWithFallback("FLYING_OFF");
            }
            else
            {
                v0->physicsParams.flags &= ~SITH_PF_USEGRAVITY;
                v0->physicsParams.flags |= SITH_PF_FLY;
                v3 = sithStrTable_GetUniStringWithFallback("FLYING_ON");
            }
            sithConsole_PrintWString(v3);
            return 1;
        }
        else
        {
            sithConsole_PrintString("Not physics thing.");
            return 0;
        }
    }
    else
    {
        sithConsole_PrintString("No world.");
        return 0;
    }
    return 0;
}

int sithCommand_Memory(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    int worldAllocatedAmt[17];
    int worldQuantityAmt[17];

    // TODO: verify the exact indices or just rewrite this

    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (pArgStr) {
        pWorld = sithWorld_g_pStaticWorld;
    }
    if (!pWorld)
    {
        sithConsole_PrintString("No world.");
        return 0;
    }
    sithWorld_GetMemoryUsage(pWorld, worldAllocatedAmt, worldQuantityAmt);
    _sprintf(std_g_genBuffer, "%5d Materials        %8d bytes.", worldQuantityAmt[0], worldAllocatedAmt[0]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Models           %8d bytes.", worldQuantityAmt[10], worldAllocatedAmt[10]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Sounds", worldQuantityAmt[6]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Keyframes        %8d bytes.", worldQuantityAmt[11], worldAllocatedAmt[11]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d World Vertices   %8d bytes.", worldQuantityAmt[1], worldAllocatedAmt[1]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d World TexVerts   %8d bytes.", worldQuantityAmt[2], worldAllocatedAmt[2]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Surfaces         %8d bytes.", worldQuantityAmt[3], worldAllocatedAmt[3]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Sectors          %8d bytes.", worldQuantityAmt[5], worldAllocatedAmt[5]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Cog Scripts\t\t%8d bytes.", worldQuantityAmt[8], worldAllocatedAmt[8]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Cogs             %8d bytes.", worldQuantityAmt[7], worldAllocatedAmt[7]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "%5d Adjoins          %8d bytes.", worldQuantityAmt[4], worldAllocatedAmt[4]);
    sithConsole_PrintString(std_g_genBuffer);
    _sprintf(std_g_genBuffer, "Total Memory Used:   %8d bytes.", worldAllocatedAmt[4] + worldAllocatedAmt[5] + worldAllocatedAmt[3] + worldAllocatedAmt[2] + worldAllocatedAmt[1] + worldAllocatedAmt[11] + worldAllocatedAmt[10] + worldAllocatedAmt[0]);
    sithConsole_PrintString(std_g_genBuffer);
    sithConsole_PrintString("(Total does not include sounds & cogs)"); 
    
    return 1;
}

int sithCommand_DynamicMemory(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    _sprintf(
        std_g_genBuffer,
        "NumAllocs: %d TotalMemAlloc: %d bytes MaxMemAlloc: %d bytes",
        stdMemory_g_curState.totalAllocs,
        stdMemory_g_curState.totalBytes,
        stdMemory_g_curState.maxBytes);
    sithConsole_PrintString(std_g_genBuffer);
    return 1;
}

int sithCommand_MemoryDump(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    stdFile_t v3; // edi
    stdMemoryAlloc *i; // esi

    // Added: pSithHS -> pLowLevelHS
    v3 = pLowLevelHS->fileOpen("memdump.txt", "w+");
    if ( v3 )
    {
        pSithHS->filePrintf(
            v3,
            "Total Memory allocated: %d bytes   # Allocations: %d  Max Memory used: %d bytes\n\n",
            stdMemory_g_curState.totalBytes,
            stdMemory_g_curState.totalAllocs,
            stdMemory_g_curState.maxBytes);
        for ( i = stdMemory_g_curState.header.prev; i; i = i->prev )
            pSithHS->filePrintf(v3, "%25s:  line %3d   %8d bytes   #%d\n", i->filePath, i->lineNum, i->size, i->num);
        pSithHS->fileClose(v3);
        sithConsole_PrintString("Memory dump file 'memdump.txt' written.");
        return 1;
    }
    return 0;
}

// MatList

int sithCommand_Coords(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    SithThing *player; // esi
    signed int result; // eax
    rdVector3 a2; // [esp+38h] [ebp-Ch] BYREF

    if ( sithWorld_g_pCurrentWorld && (player = sithWorld_g_pCurrentWorld->pLocalPlayer) != 0 )
    {
        if ( player->sector )
        {
            rdMatrix_ExtractAngles34(&player->orient, &a2);
            _sprintf(
                std_g_genBuffer,
                "Pos: (%.2f, %.2f, %.2f) PYR: (%.2f, %.2f, %.2f) Sector: %d.",
                player->position.x,
                player->position.y,
                player->position.z,
                a2.x,
                a2.y,
                a2.z,
                player->sector->id);
            sithConsole_PrintString(std_g_genBuffer);
        }
        else
        {
            sithConsole_PrintString("Thing is not in world");
        }
        result = 1;
    }
    else
    {
        sithConsole_PrintString("No world.");
        result = 0;
    }
    return result;
}

int sithCommand_Warp(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    SithThing *v3; // ebp
    int result; // eax
    int v5; // eax
    SithSector *v6; // edi
    unsigned int i; // esi
    flex32_t fx, fy, fz, f2x, f2y, f2z;
    rdVector3 a1; // [esp+10h] [ebp-48h] BYREF
    rdVector3 a3a; // [esp+1Ch] [ebp-3Ch] BYREF
    rdMatrix34 a; // [esp+28h] [ebp-30h] BYREF

    if ( !sithWorld_g_pCurrentWorld || (v3 = sithWorld_g_pCurrentWorld->pLocalPlayer) == 0 )
    {
        sithConsole_PrintString("No world.");
        return 0;
    }
    if ( !pArgStr )
    {
        sithConsole_PrintString("Format: WARP x y z");
        return 0;
    }
    v5 = _sscanf(pArgStr, "%f %f %f %f %f %f", &fx, &fy, &fz, &f2x, &f2y, &f2z);
    if ( v5 < 3 )
        return 0;
    a1.x = fx; // FLEXTODO
    a1.y = fy; // FLEXTODO
    a1.z = fz; // FLEXTODO
    a3a.x = f2x; // FLEXTODO
    a3a.y = f2y; // FLEXTODO
    a3a.z = f2z; // FLEXTODO

    if ( v5 == 6 )
        rdMatrix_BuildRotate34(&a, &a3a);
    else
        rdMatrix_Identity34(&a);

    v6 = sithWorld_g_pCurrentWorld->aSectors;
    for ( i = 0; i < sithWorld_g_pCurrentWorld->numSectors; ++v6 )
    {
        if ( sithIntersect_IsSphereInSector(&a1, 0.0, v6) )
            break;
        ++i;
    }

    if ( i == sithWorld_g_pCurrentWorld->numSectors )
    {
        sithConsole_PrintString("Position not in world");
        result = 0;
    }
    else
    {
        sithThing_DetachThing(v3);
        sithThing_ExitSector(v3);
        sithThing_SetPositionAndOrient(v3, &a1, &a);
        sithThing_EnterSector(v3, v6, 1, 0);
        result = 1;
    }
    return result;
}

int sithCommand_Activate(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    SithThing *v2; // esi
    int tmp;

    // Added: fixed a nullptr dereference
    if (!pArgStr) return 0;

    if ( sithWorld_g_pCurrentWorld && (v2 = sithWorld_g_pCurrentWorld->pLocalPlayer) != 0 )
    {
        if ( _sscanf(pArgStr, "%d", &tmp) >= 1
          && tmp >= 0
          && sithInventory_g_aTypes[tmp].cog
          && sithInventory_IsInventoryAvailable(v2, tmp) )
        {
            sithCog_SendMessage(
                sithInventory_g_aTypes[tmp].cog,
                SITH_MESSAGE_ACTIVATE,
                SENDERTYPE_0,
                tmp,
                SENDERTYPE_THING,
                v2->idx,
                0);
            return 1;
        }
    }
    else
    {
        sithConsole_PrintString("No world");
    }
    return 0;
}

int sithCommand_Jump(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    int result;
    int idx;

    // MOTS Added: fixed a nullptr dereference
    if (!pArgStr) return 0;

    result = _sscanf(pArgStr, "%d", &idx);
    if ( result )
    {
        sithPlayerActions_MoveToPlayerPosition(sithPlayer_g_pLocalPlayerThing, idx - 1);
        result = 1;
    }
    return result;
}

int sithCommand_Players(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    uint32_t v2; // edi
    SithPlayer* v3; // esi
    int v4; // eax
    int v5; // ecx
    char v7[32]; // [esp+8h] [ebp-20h] BYREF

    _sprintf(std_g_genBuffer, "Maxplayers = %d", jkPlayer_maxPlayers);
    sithConsole_PrintString(std_g_genBuffer);
    v2 = 0;
    if ( jkPlayer_maxPlayers )
    {
        v3 = &jkPlayer_playerInfos[0];
        do
        {
            stdString_WcharToChar(v7, v3->player_name, 31);
            v4 = v3->playerNetId;
            v5 = v3->flags;
            v7[31] = 0;
            _sprintf(std_g_genBuffer, "Player %d:  Name: %s  Flags: %x  ID: %x", v2, v7, v5, v4);
            sithConsole_PrintString(std_g_genBuffer);
            ++v2;
            ++v3;
        }
        while ( v2 < jkPlayer_maxPlayers );
    }
    return 1;
}

int sithCommand_PingPlayer(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    int v2; // esi
    char16_t v4[32]; // [esp+4h] [ebp-40h] BYREF

    v2 = 0;
    if ( !sithNet_isMulti )
        return 0;
    if ( pArgStr )
    {
        stdString_CharToWchar(v4, pArgStr, 31);
        v4[31] = 0;
        v2 = sithPlayer_GetPlayerNumByName(v4);
        if ( v2 < 0 )
        {
            stdString_snprintf(std_g_genBuffer, 512, "Player %s not found", pArgStr); // Altered: printed the char16_t name as char
            sithConsole_PrintString(std_g_genBuffer);
            return 1; // Altered: went on to ping jkPlayer_playerInfos[-1]
        }
    }
    sithMulti_Ping(jkPlayer_playerInfos[v2].playerNetId);
    return 1;
}

int sithCommand_Kick(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    uint32_t v2; // edi
    SithPlayer *v3; // esi
    char16_t a1[32]; // [esp+Ch] [ebp-40h] BYREF

    v2 = 0;
    if ( !pArgStr || !sithNet_isMulti || !sithNet_isServer )
        return 0;
    stdString_CharToWchar(a1, pArgStr, 31);
    a1[31] = 0;
    if ( jkPlayer_maxPlayers )
    {
        v3 = jkPlayer_playerInfos;
        do
        {
            if ( (v3->flags & 1) != 0 && !__wcsicmp(v3->player_name, a1) )
            {
                stdString_snprintf(std_g_genBuffer, 512, "Kicked %s", pArgStr); // Altered: %S is wchar_t off Windows
                sithConsole_PrintString(std_g_genBuffer);
                sithMulti_QuitPlayer(v3->playerNetId);
            }
            ++v2;
            ++v3;
        }
        while ( v2 < jkPlayer_maxPlayers );
    }
    return 1;
}

int sithCommand_CompareMatInfos(const void *a, const void *b)
{
    return ((const int*)b)[2] - ((const int*)a)[2];
}

int sithCommand_MatList(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    SithWorld *pWorld = sithWorld_g_pCurrentWorld;
    if ( !pWorld )
    {
        sithConsole_PrintString("No world.");
        return 0;
    }

    // Allocate array: [matIdx, numFaces, totalBytes, bytesPerFace] per material
    int (*matInfo)[4] = (int(*)[4])SITH_ALLOC(pWorld->sizeMaterials * sizeof(int[4]));

    // Initialize
    for (int i = 0; i < pWorld->sizeMaterials; i++)
    {
        matInfo[i][0] = i;
        matInfo[i][1] = 0;
    }

    // Count faces per material
    for (int i = 0; i < pWorld->numSurfaces; i++)
    {
        rdMaterial *mat = pWorld->surfaces[i].surfaceInfo.face.material;
        if ( mat )
        {
            int matIdx = (int)(mat - pWorld->aMaterials);
            matInfo[matIdx][1]++;
        }
    }

    // Calculate memory per material
    for (int i = 0; i < pWorld->sizeMaterials; i++)
    {
        if ( matInfo[i][1] == 0 )
        {
            matInfo[i][2] = 0;
            matInfo[i][3] = 0;
        }
        else
        {
            uint32_t memSize = sithMaterial_GetMemorySize(&pWorld->aMaterials[i]);
            matInfo[i][3] = memSize;
            matInfo[i][2] = memSize / matInfo[i][1];
        }
    }

    // Sort by total bytes descending
    _qsort(matInfo, pWorld->sizeMaterials, sizeof(int[4]), sithCommand_CompareMatInfos);

    for (int i = 0; i < pWorld->sizeMaterials; i++)
    {
        if ( matInfo[i][2] != 0 )
        {
            _sprintf(std_g_genBuffer, "%-16s  %d faces, %d bytes, %d bytes/face",
                     pWorld->aMaterials[matInfo[i][0]].mat_fpath,
                     matInfo[i][1], matInfo[i][3], matInfo[i][2]);
            sithConsole_PrintString(std_g_genBuffer);
        }
    }

    SITH_FREE(matInfo);
    return 1;
}

// Added: npc spawn
int sithCommand_CmdThingNpc(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    if (sithNet_isMulti) return 1;

    if (!pArgStr) pArgStr = "";
    char* pArgStrMutable = (char*)malloc(strlen(pArgStr)+1);
    strcpy(pArgStrMutable, pArgStr);

    char* pArgIter = _strtok(pArgStrMutable, ", \t\n\r");
    if ( !pArgIter ){
        _sprintf(std_g_genBuffer, "Usage: %s [spawn]\n", pCmd->cmdStr);
        sithConsole_PrintString(std_g_genBuffer);
        free((void*)pArgStrMutable);
        return 1;
    }

    if (!__strcmpi(pArgIter, "spawn")) {
        pArgIter = _strtok(NULL, ", \t\n\r");
        if (!pArgIter) {
            _sprintf(std_g_genBuffer, "Usage: %s spawn <template>\n", pCmd->cmdStr);
            sithConsole_PrintString(std_g_genBuffer);
            free((void*)pArgStrMutable);
            return 1;
        }
        
        SithThing* pCreateThingTemplate = sithTemplate_GetTemplate(pArgIter);
        if (!pCreateThingTemplate) {
            sithConsole_PrintString("No template by that name.");
        }
        else if (pCreateThingTemplate && sithWorld_g_pCurrentWorld && sithPlayer_g_pLocalPlayerThing) {
            //SithThing* pSpawned = sithThing_CreateThing(pCreateThingTemplate, sithPlayer_g_pLocalPlayerThing);
            SithThing* pSpawned = sithPlayerActions_SpawnThingAtLookAt(sithPlayer_g_pLocalPlayerThing, pCreateThingTemplate);
        }
        else {
            sithConsole_PrintString("No world.");
        }
    }
    free((void*)pArgStrMutable);
    return 1;
}

// Added: binds
int sithCommand_CmdBind(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    char tmp[512];
    memset(tmp, 0, sizeof(tmp));

    if (!pArgStr) pArgStr = "";
    char* pArgStrMutable = (char*)malloc(strlen(pArgStr)+1);
    strcpy(pArgStrMutable, pArgStr);

    char* pArgIter = _strtok(pArgStrMutable, ", \t\n\r");
    if ( !pArgIter || strlen(pArgIter) > 1) {
        _sprintf(std_g_genBuffer, "Usage: %s <key> <command...args>\n", pCmd->cmdStr);
        sithConsole_PrintString(std_g_genBuffer);
        free((void*)pArgStrMutable);
        return 1;
    }

    uint16_t key = pArgIter[0];

    pArgIter = _strtok(NULL, ", \t\n\r");
    while (pArgIter)
    {
        strncat(tmp, pArgIter, sizeof(tmp)-1);
        strncat(tmp, " ", sizeof(tmp)-1);

        pArgIter = _strtok(NULL, ", \t\n\r");
    }
    tmp[strlen(tmp)] = 0;

    if (strlen(tmp) > 0) {
        sithCommand_AddBind(key, tmp);
    }
    else {
        _sprintf(std_g_genBuffer, "Usage: %s <key> <command...args>\n", pCmd->cmdStr);
        sithConsole_PrintString(std_g_genBuffer);
    }
    free((void*)pArgStrMutable);
    return 1;
}

// Added: unbinds
int sithCommand_CmdUnbind(stdDebugConsoleCmd *pCmd, const char *pArgStr)
{
    char tmp[512];
    memset(tmp, 0, sizeof(tmp));

    if (!pArgStr) pArgStr = "";
    char* pArgStrMutable = (char*)malloc(strlen(pArgStr)+1);
    strcpy(pArgStrMutable, pArgStr);

    char* pArgIter = _strtok(pArgStrMutable, ", \t\n\r");
    if ( !pArgIter || strlen(pArgIter) > 1) {
        _sprintf(std_g_genBuffer, "Usage: %s <key>\n", pCmd->cmdStr);
        sithConsole_PrintString(std_g_genBuffer);
        free((void*)pArgStrMutable);
        return 1;
    }

    uint16_t key = pArgIter[0];

    sithCommand_RemoveBind(key);
    free((void*)pArgStrMutable);
    return 1;
}

void sithCommand_StartupBinds()
{
    if (sithCommand_pBinds) {
        sithCommand_ShutdownBinds();
    }

    // Added: binds
    sithCommand_pBinds = (sithCommandBind*)malloc(sizeof(sithCommandBind));
    sithCommand_pBinds->key = 0;
    sithCommand_pBinds->pCmd = "";
    sithCommand_pBinds->pNext = NULL;

    sithCommand_LoadBinds();
}

void sithCommand_ShutdownBinds()
{
    if (!sithCommand_pBinds) {
        return;
    }

    sithCommand_SaveBinds();

    sithCommandBind* pBindIter = sithCommand_pBinds;
    while (pBindIter)
    {
        if (pBindIter->key && pBindIter->pCmd) {
            free((void*)pBindIter->pCmd);
        }
        sithCommandBind* pBindIterNext = pBindIter->pNext;
        free((void*)pBindIter);
        pBindIter = pBindIterNext;
    }
    sithCommand_pBinds = NULL;
}

void sithCommand_LoadBindCallback(const char* pKey, const char* pVal, void* pCtx)
{
    sithCommand_AddBind(pKey[0], pVal);
}

void sithCommand_SaveBinds()
{
    const char* ext_fpath = SITHBINDS_FNAME;

    stdJSON_EraseAll(ext_fpath);
    
    char tmp[3];
    sithCommandBind* pBindIter = sithCommand_pBinds;
    while (pBindIter = pBindIter->pNext)
    {
        snprintf(tmp, 3, "%c", pBindIter->key);
        stdJSON_SetString(ext_fpath, tmp, pBindIter->pCmd);
    }
}

void sithCommand_LoadBinds()
{
    const char* ext_fpath = SITHBINDS_FNAME;
    stdJSON_IterateKeys(ext_fpath, sithCommand_LoadBindCallback, NULL);
}

// Added
char sithCommand_aTmpCommandExecute[512];
void sithCommand_HandleBinds(uint16_t key)
{
    if (!sithCommand_pBinds) return; // Added: sithCommand isn't started in DroidWorks mode

    sithCommandBind* pBindIter = sithCommand_pBinds;
    while (pBindIter = pBindIter->pNext)
    {
        if (key == pBindIter->key) {
            strcpy(sithCommand_aTmpCommandExecute, pBindIter->pCmd);
            jkQuakeConsole_ExecuteCommand(sithCommand_aTmpCommandExecute);
        }
    }
}

// Added
void sithCommand_AddBind(uint16_t key, const char* pCmd)
{
    if (!pCmd || !key) return;

    sithCommandBind* pBindIter = sithCommand_pBinds;
    while (pBindIter)
    {
        // Overwrite existing bind
        if (key == pBindIter->key) {
            if (pBindIter->pCmd) {
                free((void*)pBindIter->pCmd);
            }
            pBindIter->pCmd = (char*)malloc(strlen(pCmd)+1);
            strcpy((char*)pBindIter->pCmd, pCmd);
            break;
        }

        // End of the list, add a bind
        if (!pBindIter->pNext) {
            sithCommandBind* pNewBind = (sithCommandBind*)malloc(sizeof(sithCommandBind));
            pBindIter->pNext = pNewBind;

            pNewBind->key = key;
            pNewBind->pCmd = (char*)malloc(strlen(pCmd)+1);
            strcpy((char*)pNewBind->pCmd, pCmd);
            pNewBind->pNext = NULL;
            break;
        }
        pBindIter = pBindIter->pNext;
    }

    sithCommand_SaveBinds();
}

// Added
void sithCommand_RemoveBind(uint16_t key)
{
    if (!key) return;

    sithCommandBind* pBindIter = sithCommand_pBinds;
    sithCommandBind* pBindIterLast = NULL;
    while (pBindIter)
    {
        // Overwrite existing bind
        if (key == pBindIter->key) {
            if (pBindIter->pCmd) {
                free((void*)pBindIter->pCmd);
            }

            sithCommandBind* pBindIterNext = pBindIter->pNext;
            if (pBindIterLast) {
                pBindIterLast->pNext = pBindIterNext;
            }
            free((void*)pBindIter);
            pBindIter = pBindIterNext;
            continue;
        }
        pBindIterLast = pBindIter;
        pBindIter = pBindIter->pNext;
    }

    sithCommand_SaveBinds();
}
