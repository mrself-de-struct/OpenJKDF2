// Added: a dedicated server's admin commands, typed at its own console or sent
// with "rcon" (2026-10-27): status, kick, ban, unban, bans, say, map, maplist,
// nextmap, restart, timelimit, scorelimit, password, rank, cancelvote; players'
// callvote, vote and servermaps; and the client's rcon,
// rcon_password, rcon_address. Kicks and bans stick on a server running its own
// server (Platform/Networking/DPlay/jkrelay_listen.c): the player's machine is
// taken out of the game there and a banned address isn't let in again.

#include "Main/jkServer.h"

#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)

#include "jk.h"
#include "stdPlatform.h"
#include "Devices/sithConsole.h"
#include "Dss/sithMulti.h"
#include "Dss/jkDSS.h"
#include "Gameplay/sithTime.h"
#include "General/stdJSON.h"
#include "General/stdString.h"
#include "General/stdFileUtil.h"
#include "General/stdFnames.h"
#include "General/stdStrTable.h"
#include "Gui/jkGUITitle.h"
#include "Main/jkMain.h"
#include "Main/jkStrings.h"
#include "Main/Main.h"
#include "Main/sithCvar.h"
#include "Platform/Common/stdLog.h"
#include "Platform/wuRegistry.h"
#include "Platform/Networking/DPlay/dplay_shim.h"
#include "Platform/Networking/DPlay/stdComm_dplay.h"
#include "Win95/stdComm.h"
#include "Win95/stdGob.h"
#include "World/jkPlayer.h"
#include "World/sithWorld.h"
#include "Cog/jkCog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Rcon's protection against guessed passwords (jkrelay_server.c): sv_floodProtect lets an
// address send one command a second, sv_rconMaxTries wrong passwords block it for
// sv_rconBlockMinutes. Applied by jkServer_Tick when they change.
static int jkServer_bFloodProtect = 1;
static int jkServer_rconMaxTries = 5;
static int jkServer_rconBlockMinutes = 5;

static void jkServer_Print(const char* fmt, ...)
{
    va_list ap;
    char aLine[512];
    va_start(ap, fmt);
    vsnprintf(aLine, sizeof(aLine), fmt, ap);
    va_end(ap);
    sithConsole_PrintString(aLine);
}

// Admin commands run on the host only (anywhere else: rcon).
static int jkServer_IsHost(void)
{
    if ( sithNet_isMulti && sithNet_isServer )
        return 1;
    sithConsole_PrintString("Only on the server (from elsewhere: rcon <command>).");
    return 0;
}

static void jkServer_Trim(const char* pIn, char* pOut, size_t outLen)
{
    size_t n;
    while ( pIn && (*pIn == ' ' || *pIn == '\t') ) pIn++;
    stdString_SafeStrCopy(pOut, pIn ? pIn : "", outLen);
    n = strlen(pOut);
    while ( n && (pOut[n - 1] == ' ' || pOut[n - 1] == '\t') ) pOut[--n] = 0;
    if ( n >= 2 && pOut[0] == '"' && pOut[n - 1] == '"' )
    {
        memmove(pOut, pOut + 1, n - 2);
        pOut[n - 2] = 0;
    }
}

// A player in the game by slot (as "status" shows it) or name. -1 if none.
static int jkServer_FindPlayer(const char* pWho)
{
    char16_t wName[32];
    int bNumber = pWho[0] && strspn(pWho, "0123456789") == strlen(pWho);
    for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
    {
        SithPlayer* p = &jkPlayer_playerInfos[i];
        if ( !(p->flags & 1) || p->playerNetId == stdComm_dplayIdSelf )
            continue;
        if ( bNumber && atoi(pWho) == i )
            return i;
    }
    stdString_CharToWchar(wName, pWho, 31);
    wName[31] = 0;
    for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
    {
        SithPlayer* p = &jkPlayer_playerInfos[i];
        if ( (p->flags & 1) && p->playerNetId != stdComm_dplayIdSelf && !__wcsicmp(p->player_name, wName) )
            return i;
    }
    return -1;
}

static void jkServer_PlayerName(int i, char* pOut, size_t outLen)
{
    stdString_WcharToChar(pOut, jkPlayer_playerInfos[i].player_name, (int)outLen - 1);
    pOut[outLen - 1] = 0;
}

// The bans for good, back into server.json ("bans").
static void jkServer_SaveBans(void)
{
    char aList[2048];
    dps_ServerListBans(aList, sizeof(aList), 1);
    stdJSON_SetStringList(SERVER_FNAME, "bans", aList);
}

// ---------------------------------------------------------------------------

static void jkServer_RankName(int rank, char* pOut, size_t outLen);

static int jkServer_CmdStatus(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aName[64], aAddr[64], aKey[64], aTitle[128];
    const char16_t* pTitle = NULL;
    int numPlayers = 0;

    if ( !jkServer_IsHost() )
        return 1;
    stdComm_DPlay_GetGameName(aName, sizeof(aName));
    jkServer_Print("Game: %s", aName);
    if ( sithWorld_g_pCurrentWorld )
    {
        stdFnames_CopyShortName(aKey, sizeof(aKey), sithWorld_g_pCurrentWorld->map_jkl_fname);
        jkGuiTitle_sub_4189A0(aKey);
        pTitle = stdStrTable_GetValue(&jkStrings_tableExtOver, aKey);
        if ( !pTitle ) pTitle = stdStrTable_GetValue(&jkCog_strings, aKey);
        aTitle[0] = 0;
        if ( pTitle ) { stdString_WcharToChar(aTitle, pTitle, sizeof(aTitle) - 1); aTitle[sizeof(aTitle) - 1] = 0; }
        jkServer_Print("Map: %s%s%s, %s%s", aTitle, aTitle[0] ? " (" : "", sithWorld_g_pCurrentWorld->map_jkl_fname, jkRes_episodeGobName, aTitle[0] ? ")" : "");
    }
    if ( sithNet_MultiModeFlags & MULTIMODEFLAG_TIMELIMIT )
    {
        int left = (int)((sithNet_multiplayer_timelimit - (int32_t)sithTime_g_msecGameTime) / 1000);
        if ( left < 0 ) left = 0;
        jkServer_Print("Time left: %d:%02d of %d:%02d", left / 60, left % 60,
                       sithNet_multiplayer_timelimit / 60000, (sithNet_multiplayer_timelimit / 1000) % 60);
    }
    else
        sithConsole_PrintString("Time limit: none");
    if ( sithNet_MultiModeFlags & MULTIMODEFLAG_SCORELIMIT )
        jkServer_Print("Score limit: %d", sithNet_scorelimit);
    else
        sithConsole_PrintString("Score limit: none");
    {
        char aRank[64];
        int rank = stdComm_DPlay_GameMaxRank();
        jkServer_RankName(rank, aRank, sizeof(aRank));
        jkServer_Print("Force rank: up to %d (%s)", rank, aRank);
    }
    if ( jkMain_GetForcedNextMap() )
        jkServer_Print("Next map: %s", jkMain_GetForcedNextMap());
    sithConsole_PrintString("slot  score  name             address");
    for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
    {
        SithPlayer* p = &jkPlayer_playerInfos[i];
        if ( !(p->flags & 1) || p->playerNetId == stdComm_dplayIdSelf )
            continue;
        numPlayers++;
        jkServer_PlayerName(i, aName, 20);
        if ( !dps_ServerPlayerAddr(p->playerNetId, aAddr, sizeof(aAddr)) )
            stdString_SafeStrCopy(aAddr, "-", sizeof(aAddr));
        jkServer_Print("%4d  %5d  %-16s %s", i, p->score, aName, aAddr);
    }
    jkServer_Print("%d player%s", numPlayers, numPlayers == 1 ? "" : "s");
    return 1;
}

// kick <slot|name>; ban <slot|name|ip> [minutes]
static int jkServer_Kick(const char* pArgStr, int bBan)
{
    char aArg[64], aName[32], aIp[32];
    int minutes = bBan ? 0 : -1, slot;
    char* pLast;

    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        sithConsole_PrintString(bBan ? "ban <slot, name or IP address> [minutes]   (status lists the slots; no minutes: for good)"
                                     : "kick <slot or name>   (status lists the slots)");
        return 1;
    }
    // ban ... <minutes>: a number at the end, after something else
    pLast = strrchr(aArg, ' ');
    if ( bBan && pLast && pLast[1] && strspn(pLast + 1, "0123456789") == strlen(pLast + 1) )
    {
        minutes = atoi(pLast + 1);
        *pLast = 0;
        jkServer_Trim(aArg, aArg, sizeof(aArg));
    }
    slot = jkServer_FindPlayer(aArg);
    if ( slot < 0 )
    {
        if ( bBan && strchr(aArg, '.') && dps_ServerBan(aArg, minutes) )
        {
            jkServer_Print("Banned %s%s.", aArg, minutes ? " for a while" : "");
            stdLog_Printf("admin", "banned %s (%s)", aArg, minutes ? "minutes" : "for good");
            if ( !minutes ) jkServer_SaveBans();
            // and whoever plays from there
            for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
            {
                char aAddr[64];
                SithPlayer* p = &jkPlayer_playerInfos[i];
                if ( !(p->flags & 1) || p->playerNetId == stdComm_dplayIdSelf ) continue;
                if ( dps_ServerPlayerAddr(p->playerNetId, aAddr, sizeof(aAddr)) && !strncmp(aAddr, aArg, strlen(aArg))
                     && aAddr[strlen(aArg)] == ':' )
                {
                    sithMulti_QuitPlayer(p->playerNetId);
                    dps_ServerKick(p->playerNetId, -1, aIp, sizeof(aIp));
                }
            }
            return 1;
        }
        jkServer_Print("No player \"%s\" (status lists them).", aArg);
        return 1;
    }
    jkServer_PlayerName(slot, aName, sizeof(aName));
    // JK's own kick (the player's game leaves), then out at the server, which
    // JK.EXE's kick can't do (a patched JK.EXE ignores the kick message).
    sithMulti_QuitPlayer(jkPlayer_playerInfos[slot].playerNetId);
    aIp[0] = 0;
    if ( dps_ServerKick(jkPlayer_playerInfos[slot].playerNetId, minutes, aIp, sizeof(aIp)) )
    {
        if ( bBan )
            jkServer_Print("Banned %s (%s)%s.", aName, aIp, minutes ? " for a while" : "");
        else
            jkServer_Print("Kicked %s (%s).", aName, aIp);
        if ( bBan && !minutes ) jkServer_SaveBans();
    }
    else
        jkServer_Print("Kicked %s%s.", aName, bBan ? " (not banned: bans need a server of its own)" : "");
    stdLog_Printf("admin", "%s %s %s", bBan ? "banned" : "kicked", aName, aIp);
    {
        char aMsg[96];
        stdString_snprintf(aMsg, sizeof(aMsg), "Server: %s was %s.", aName, bBan ? "banned" : "kicked");
        sithMulti_SendChat(aMsg, -1, -1);
    }
    return 1;
}

// Everyone out and straight back in (their games join again by themselves): what a
// player brings is read at joining, so after "rank" lowers the force rank, players
// already in keep their force powers until they join again. Their machines are told
// why (JKR_LOST_RECONNECT); older builds just see the game end. 2026-10-30.
static int jkServer_CmdReconnectAll(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    int n = 0, failed = 0;
    if ( !jkServer_IsHost() )
        return 1;
    for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
    {
        SithPlayer* p = &jkPlayer_playerInfos[i];
        if ( !(p->flags & 1) || p->playerNetId == stdComm_dplayIdSelf )
            continue;
        if ( dps_ServerKick(p->playerNetId, DPS_KICK_RECONNECT, NULL, 0) )
            n++;
        else
            failed++;
    }
    if ( failed && !n )
        jkServer_Print("Couldn't: reconnectall needs an Imperial Network Protocol game (a server of its own, or a relay server from 2026-10-30).");
    else if ( !n )
        jkServer_Print("Nobody to reconnect.");
    else
        jkServer_Print("Asked %d player%s to reconnect%s.", n, n == 1 ? "" : "s", failed ? " (some couldn't be)" : "");
    stdLog_Printf("admin", "reconnectall: %d player%s", n, n == 1 ? "" : "s");
    return 1;
}

static int jkServer_CmdKick(stdDebugConsoleCmd* pCmd, const char* pArgStr) { return jkServer_Kick(pArgStr, 0); }
static int jkServer_CmdBan(stdDebugConsoleCmd* pCmd, const char* pArgStr) { return jkServer_Kick(pArgStr, 1); }

static int jkServer_CmdUnban(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[64];
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        sithConsole_PrintString("unban <IP address>   (bans lists them)");
        return 1;
    }
    if ( !dps_ServerUnban(aArg) )
    {
        jkServer_Print("%s isn't banned.", aArg);
        return 1;
    }
    jkServer_SaveBans();
    jkServer_Print("Unbanned %s.", aArg);
    stdLog_Printf("admin", "unbanned %s", aArg);
    return 1;
}

static int jkServer_CmdBans(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aList[2048];
    char* pLine;
    if ( !jkServer_IsHost() )
        return 1;
    dps_ServerListBans(aList, sizeof(aList), 0);
    if ( !aList[0] )
    {
        sithConsole_PrintString(dps_ServerListening() ? "No bans." : "No bans (they need a server of its own).");
        return 1;
    }
    for ( pLine = strtok(aList, "\n"); pLine; pLine = strtok(NULL, "\n") )
        sithConsole_PrintString(pLine);
    return 1;
}

static char* jkServer_pCapture = NULL; // rcon: output of the command running (jkServer_Tick)

static int jkServer_CmdSay(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[120], aMsg[128];
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        sithConsole_PrintString("say <message>");
        return 1;
    }
    stdString_snprintf(aMsg, sizeof(aMsg), "Server: %s", aArg);
    sithMulti_SendChat(aMsg, -1, -1);
    // An rcon caller in the game gets the chat itself: the message again would show twice
    if ( jkServer_pCapture )
        sithConsole_PrintString("Sent.");
    else
        sithConsole_PrintString(aMsg);
    return 1;
}

// map <name>: that level now (after the usual 10 s end of level); nextmap [<name>]:
// after this one; restart: this level again. A name is "level" (this episode, else
// any in the episode folder) or "GOB:level" (jkMain_FindLevel).
static int jkServer_ChangeMap(const char* pArgStr, int bNow)
{
    char aArg[128];
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        if ( bNow )
            sithConsole_PrintString("map <level> or map <GOB>:<level>   (e.g. map m10, map JK1CTF:c1; maplist lists them)");
        else
            jkServer_Print("Next map: %s", jkMain_GetForcedNextMap() ? jkMain_GetForcedNextMap() : "as the rotation or episode goes");
        return 1;
    }
    if ( !jkMain_SetNextMap(aArg) )
    {
        jkServer_Print("No level %s in the episode folder (maplist lists them).", aArg);
        return 1;
    }
    stdLog_Printf("admin", "%s %s", bNow ? "map" : "next map", jkMain_GetForcedNextMap());
    if ( !bNow )
    {
        jkServer_Print("Next map: %s (when this level ends).", jkMain_GetForcedNextMap());
        return 1;
    }
    jkServer_Print("Going to %s.", jkMain_GetForcedNextMap());
    jkDSS_SendEndLevel();
    return 1;
}

// Case-insensitive strstr.
static int jkServer_Contains(const char* pHay, const char* pNeedle)
{
    size_t n = strlen(pNeedle);
    for ( ; *pHay; pHay++ )
        if ( !__strnicmp(pHay, pNeedle, n) )
            return 1;
    return !n;
}

// A level's title from its GOB's misc\cogstrings.uni, lines like
//    "M10"   0 "Canyon Oasis"
// (the key is the .jkl's name without extension, any case).
static int jkServer_GobLevelTitle(Gob* pGob, const char* pJkl, char* pOut, size_t outLen)
{
    GobFileHandle* f;
    char aKey[64], aLine[512];
    size_t keyLen;
    int bFound = 0;

    stdFnames_CopyShortName(aKey, sizeof(aKey), (char*)pJkl);
    keyLen = strlen(aKey);
    if ( !(f = stdGob_FileOpen(pGob, "misc\\cogstrings.uni")) )
        return 0;
    while ( !bFound && stdGob_FileGets(f, aLine, sizeof(aLine)) )
    {
        char* p = aLine;
        char* q;
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( *p == '"' ) p++;
        if ( __strnicmp(p, aKey, keyLen) || (p[keyLen] != '"' && p[keyLen] != ' ' && p[keyLen] != '\t') )
            continue;
        if ( !(p = strchr(p + keyLen + 1, '"')) || !(q = strchr(p + 1, '"')) )
            continue;
        *q = 0;
        stdString_SafeStrCopy(pOut, p + 1, outLen);
        bFound = pOut[0] != 0;
    }
    stdGob_FileClose(f);
    return bFound;
}

// maplist [text]: every level in the game's episode GOBs, "title | level | GOB",
// or those whose title, level or GOB has 'text' in it.
static int jkServer_CmdMapList(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aFilter[64], aPath[160], aJkl[64], aTitle[128];
    stdFileSearch* pSearch;
    stdFileSearchResult res;
    int numLevels = 0;

    jkServer_Trim(pArgStr, aFilter, sizeof(aFilter));
    if ( !(pSearch = stdFileUtil_NewFind("episode", 3, "gob")) )
        return 1;
    while ( stdFileUtil_FindNext(pSearch, &res) )
    {
        Gob* pGob;
        if ( res.is_subdirectory || res.fpath[0] == '.' )
            continue;
        stdString_snprintf(aPath, sizeof(aPath), "episode%c%s", LEC_PATH_SEPARATOR_CHR, res.fpath);
        if ( !(pGob = stdGob_Load(aPath, 2, 0)) )
            continue;
#ifndef STDGOB_COMPACT_ENTRIES
        for ( uint32_t i = 0; i < pGob->numFiles; i++ )
        {
            const char* pName = pGob->entries[i].fname;
            size_t len = strlen(pName);
            if ( __strnicmp(pName, "jkl\\", 4) || len < 8 || __strcmpi(pName + len - 4, ".jkl") )
                continue;
            stdString_SafeStrCopy(aJkl, pName + 4, sizeof(aJkl));
            if ( !jkServer_GobLevelTitle(pGob, aJkl, aTitle, sizeof(aTitle)) )
                stdString_SafeStrCopy(aTitle, "-", sizeof(aTitle));
            if ( !jkServer_Contains(aTitle, aFilter) && !jkServer_Contains(aJkl, aFilter) && !jkServer_Contains(res.fpath, aFilter) )
                continue;
            jkServer_Print("%s | %s | %s", aTitle, aJkl, res.fpath);
            numLevels++;
        }
#endif
        stdGob_Free(pGob);
    }
    stdFileUtil_DisposeFind(pSearch);
    jkServer_Print("%d level%s. map/nextmap take <level> or <GOB>:<level>", numLevels, numLevels == 1 ? "" : "s");
    return 1;
}

static int jkServer_CmdMap(stdDebugConsoleCmd* pCmd, const char* pArgStr) { return jkServer_ChangeMap(pArgStr, 1); }
static int jkServer_CmdNextMap(stdDebugConsoleCmd* pCmd, const char* pArgStr) { return jkServer_ChangeMap(pArgStr, 0); }

static int jkServer_CmdRestart(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    if ( !jkServer_IsHost() || !sithWorld_g_pCurrentWorld )
        return 1;
    {
        char aMap[128];
        stdString_snprintf(aMap, sizeof(aMap), "%s:%s", jkRes_episodeGobName, sithWorld_g_pCurrentWorld->map_jkl_fname);
        return jkServer_ChangeMap(aMap, 1);
    }
}

// timelimit <minutes>, 0 = none; scorelimit <points>, 0 = none. For this game
// (server.json keeps the ones it starts with).
static int jkServer_SetLimit(const char* pArgStr, int bTime)
{
    char aArg[32];
    int v;
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] || strspn(aArg, "0123456789") != strlen(aArg) )
    {
        sithConsole_PrintString(bTime ? "timelimit <minutes>   (0: none)" : "scorelimit <points>   (0: none)");
        return 1;
    }
    v = atoi(aArg);
    if ( bTime )
    {
        sithNet_multiplayer_timelimit = sithMulti_multiplayerTimelimit = v * 60000;
        if ( v ) { sithNet_MultiModeFlags |= MULTIMODEFLAG_TIMELIMIT; sithMulti_multiModeFlags |= MULTIMODEFLAG_TIMELIMIT; }
        else { sithNet_MultiModeFlags &= ~MULTIMODEFLAG_TIMELIMIT; sithMulti_multiModeFlags &= ~MULTIMODEFLAG_TIMELIMIT; }
        jkServer_Print(v ? "Time limit: %d minutes from the level's start." : "No time limit.", v);
    }
    else
    {
        sithNet_scorelimit = stdComm_dword_832204 = v;
        if ( v ) { sithNet_MultiModeFlags |= MULTIMODEFLAG_SCORELIMIT; sithMulti_multiModeFlags |= MULTIMODEFLAG_SCORELIMIT; }
        else { sithNet_MultiModeFlags &= ~MULTIMODEFLAG_SCORELIMIT; sithMulti_multiModeFlags &= ~MULTIMODEFLAG_SCORELIMIT; }
        jkServer_Print(v ? "Score limit: %d." : "No score limit.", v);
    }
    stdLog_Printf("admin", "%s %d", bTime ? "timelimit" : "scorelimit", v);
    return 1;
}

static int jkServer_CmdTimeLimit(stdDebugConsoleCmd* pCmd, const char* pArgStr) { return jkServer_SetLimit(pArgStr, 1); }
static int jkServer_CmdScoreLimit(stdDebugConsoleCmd* pCmd, const char* pArgStr) { return jkServer_SetLimit(pArgStr, 0); }

static int jkServer_CmdPassword(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[32];
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !stdComm_DPlay_SetPassword(aArg) )
    {
        sithConsole_PrintString("Couldn't change the password.");
        return 1;
    }
    sithConsole_PrintString(aArg[0] ? "Password set (for new players)." : "Password removed.");
    stdLog_Printf("admin", "%s", aArg[0] ? "password set" : "password removed");
    return 1;
}

// A force rank's name as the game shows it ("Jedi Lord" for 8).
static void jkServer_RankName(int rank, char* pOut, size_t outLen)
{
    char aKey[32];
    const char16_t* pName;
    stdString_snprintf(aKey, sizeof(aKey), "RANK_%d_L", rank);
    pName = jkStrings_GetUniStringWithFallback(aKey);
    pOut[0] = 0;
    if ( pName )
    {
        stdString_WcharToChar(pOut, pName, (int)outLen - 1);
        pOut[outLen - 1] = 0;
    }
}

// rank [0-8]: the highest force rank a joining character may have, for this game
// (server.json keeps the starting one). Players already in keep their characters.
static int jkServer_CmdRank(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[16], aName[64];
    int rank;
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        rank = stdComm_DPlay_GameMaxRank();
        jkServer_RankName(rank, aName, sizeof(aName));
        jkServer_Print("Force rank: up to %d (%s). rank <0-8> changes it for new players.", rank, aName);
        return 1;
    }
    if ( strspn(aArg, "0123456789") != strlen(aArg) || (rank = atoi(aArg)) > 8 )
    {
        sithConsole_PrintString("rank <0-8>   (0 Uninitiated ... 8 Jedi Lord)");
        return 1;
    }
    if ( !stdComm_DPlay_SetMaxRank(rank) )
    {
        sithConsole_PrintString("Couldn't change the force rank.");
        return 1;
    }
    jkGuiNetHost_maxRank = rank;
    jkServer_RankName(rank, aName, sizeof(aName));
    jkServer_Print("Force rank: up to %d (%s), for players who join from now on (reconnectall: everyone now).", rank, aName);
    stdLog_Printf("admin", "rank %d", rank);
    return 1;
}

// ---------------------------------------------------------------------------
// Rcon: the server's side (an rcon command runs like one typed at its console)
// ---------------------------------------------------------------------------

static size_t jkServer_captureLen = 0;
static DebugConsolePrintFunc_t jkServer_pfPrevPrint = NULL;

static int jkServer_CapturePrint(const char* pLine)
{
    size_t n = strlen(pLine);
    if ( jkServer_pCapture && jkServer_captureLen + n + 2 < JKSERVER_RCON_OUTPUT_MAX )
    {
        memcpy(jkServer_pCapture + jkServer_captureLen, pLine, n);
        jkServer_captureLen += n;
        jkServer_pCapture[jkServer_captureLen++] = '\n';
        jkServer_pCapture[jkServer_captureLen] = 0;
    }
    return jkServer_pfPrevPrint ? jkServer_pfPrevPrint(pLine) : 1;
}

static void jkServer_VoteTick(void);

void jkServer_Tick(void)
{
    static char aOutput[JKSERVER_RCON_OUTPUT_MAX];
    char aCmd[512];
    static int lastFlood = -1, lastTries = -1, lastBlock = -1;
    jkServer_VoteTick();
    if ( dps_ServerListening() && (jkServer_bFloodProtect != lastFlood || jkServer_rconMaxTries != lastTries
                                   || jkServer_rconBlockMinutes != lastBlock) )
    {
        lastFlood = jkServer_bFloodProtect;
        lastTries = jkServer_rconMaxTries;
        lastBlock = jkServer_rconBlockMinutes;
        dps_ServerSetRconLimits(jkServer_rconMaxTries, jkServer_rconBlockMinutes, jkServer_bFloodProtect);
        stdLog_Printf("server", "rcon protection: flood %s, %d wrong passwords block an address for %d minutes",
                      jkServer_bFloodProtect ? "on (one command a second)" : "off",
                      jkServer_rconMaxTries < 1 ? 1 : jkServer_rconMaxTries, jkServer_rconBlockMinutes < 1 ? 1 : jkServer_rconBlockMinutes);
    }
    if ( !dps_ServerTakeRcon(aCmd, sizeof(aCmd)) )
        return;
    aOutput[0] = 0;
    jkServer_pCapture = aOutput;
    jkServer_captureLen = 0;
    jkServer_pfPrevPrint = DebugGui_fnPrint;
    DebugGui_fnPrint = jkServer_CapturePrint;
    sithConsole_ExeCommand(aCmd);
    DebugGui_fnPrint = jkServer_pfPrevPrint;
    jkServer_pCapture = NULL;
    dps_ServerRconDone(aOutput);
}

// A dedicated server running its own server, or hosting on a relay server (its rcon then
// comes through the relay, which does its kicks and bans): rcon password and bans from
// server.json.
void jkServer_StartupDedicated(void)
{
    char aPassword[128];
    char aBans[MAIN_MAX_BANS][32];
    int n;
    if ( !dps_ServerListening() )
        return;
    wuRegistry_GetStr("rconPassword", aPassword, sizeof(aPassword), "");
    dps_ServerSetRcon(aPassword);
    if ( aPassword[0] )
        stdLog_Printf("server", "rcon on (rcon_password on the client, then rcon <command>)");
    n = stdJSON_GetStringList(SERVER_FNAME, "bans", &aBans[0][0], MAIN_MAX_BANS, 32);
    for ( int i = 0; i < n; i++ )
        if ( !dps_ServerBan(aBans[i], 0) )
            stdLog_Printf("server", "\"bans\": %s isn't an IP address, skipped", aBans[i]);
    if ( n )
        stdLog_Printf("server", "%d banned address%s", n, n == 1 ? "" : "es");
}

// ---------------------------------------------------------------------------
// Rcon: the client's side
// ---------------------------------------------------------------------------

static char jkServer_aRconPassword[128];
static char jkServer_aRconAddress[256];

static int jkServer_CmdRconPassword(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[128];
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        sithConsole_PrintString(jkServer_aRconPassword[0] ? "rcon password set (rcon_password <password> to change it)."
                                                          : "rcon_password <password>");
        return 1;
    }
    stdString_SafeStrCopy(jkServer_aRconPassword, aArg, sizeof(jkServer_aRconPassword));
    sithConsole_PrintString("rcon password set (until the game quits).");
    return 1;
}

static int jkServer_CmdRconAddress(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[256];
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( aArg[0] )
        stdString_SafeStrCopy(jkServer_aRconAddress, aArg, sizeof(jkServer_aRconAddress));
    if ( jkServer_aRconAddress[0] )
        jkServer_Print("rcon goes to %s.", jkServer_aRconAddress);
    else
    {
        char aServer[256];
        stdComm_DPlay_RconServer(aServer, sizeof(aServer));
        jkServer_Print(aServer[0] ? "rcon goes to %s (the game you're in or were last in)." : "rcon_address <host[:port]>", aServer);
    }
    return 1;
}

static int jkServer_CmdRcon(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    static char aOutput[JKSERVER_RCON_OUTPUT_MAX];
    char aCmd[512], aServer[256];
    uint8_t aGuid[16];
    char* pLine;

    jkServer_Trim(pArgStr, aCmd, sizeof(aCmd));
    if ( !aCmd[0] )
    {
        sithConsole_PrintString("rcon <command>   (rcon_password first; rcon_address picks the server)");
        return 1;
    }
    if ( !jkServer_aRconPassword[0] )
    {
        sithConsole_PrintString("Set the server's rcon password first: rcon_password <password>");
        return 1;
    }
    if ( jkServer_aRconAddress[0] )
        stdString_SafeStrCopy(aServer, jkServer_aRconAddress, sizeof(aServer));
    else
        stdComm_DPlay_RconServer(aServer, sizeof(aServer));
    if ( !aServer[0] )
    {
        sithConsole_PrintString("Which server? rcon_address <host[:port]>");
        return 1;
    }
    if ( !dps_Rcon(aServer, stdComm_DPlay_RconGuid(aGuid) ? aGuid : NULL, jkServer_aRconPassword, aCmd, aOutput, sizeof(aOutput),
                   JKSERVER_RCON_TIMEOUT_MS) )
    {
        jkServer_Print("No answer from %s (rcon reaches dedicated servers with an rconPassword: on their own server, or on a relay server that passes rcon on).", aServer);
        return 1;
    }
    for ( pLine = strtok(aOutput, "\n"); pLine; pLine = strtok(NULL, "\n") )
        sithConsole_PrintString(pLine);
    return 1;
}

// ---------------------------------------------------------------------------
// Voting, as Quake 3's callvote/vote (g_cmds.c, G_CheckVote): a vote runs 30 s; it
// passes once more than half the players in the game voted yes and fails once half
// voted no, or when its time is up. The caller votes yes. Each player calls at most
// JKSERVER_MAX_CALLVOTES votes a level, one vote at a time. Players' "callvote" and
// "vote" reach the server as chat for it alone, "!callvote ..." / "!vote ..."
// (jkServer_HandleChat, from sithMulti_ProcessChat), so a JK.EXE player can type them
// in chat too. The cvar sv_allowVote (bits, admin "togglevote") says which kinds are allowed.
// ---------------------------------------------------------------------------

enum { JKVOTE_NONE, JKVOTE_MAP, JKVOTE_KICK, JKVOTE_RANK, JKVOTE_SCORELIMIT, JKVOTE_TIMELIMIT };

// sv_allowVote: the kinds of vote players may call, one bit each (as Urban Terror's
// g_allowvote on Quake 3); 0 = no voting. Admin "togglevote" flips them.
static const struct { int bit; const char* pName; } jkServer_aVoteKinds[] = {
    { 1, "map" }, { 2, "kick" }, { 4, "rank" }, { 8, "scorelimit" }, { 16, "timelimit" },
};
#define JKSERVER_VOTE_ALL 31
static int jkServer_allowVote = JKSERVER_VOTE_ALL;

void jkServer_StartupCvars(void)
{
    sithCvar_RegisterInt("sv_allowVote", JKSERVER_VOTE_ALL, &jkServer_allowVote, CVARFLAG_GLOBAL);
    sithCvar_RegisterBool("sv_floodProtect", 1, &jkServer_bFloodProtect, CVARFLAG_GLOBAL);
    sithCvar_RegisterInt("sv_rconMaxTries", 5, &jkServer_rconMaxTries, CVARFLAG_GLOBAL);
    sithCvar_RegisterInt("sv_rconBlockMinutes", 5, &jkServer_rconBlockMinutes, CVARFLAG_GLOBAL);
    // Added: how fast a host sends maps and mods to joiners (KB/s, all of them together)
    sithCvar_RegisterInt("sv_downloadRate", DPLAY_XFER_RATE_DEFAULT_KB, &stdComm_DPlay_downloadRateKB, CVARFLAG_GLOBAL);
}

// The sv_allowVote bit for a kind of vote by name, 0 if it isn't one.
static int jkServer_VoteBit(const char* pName)
{
    for ( size_t i = 0; i < sizeof(jkServer_aVoteKinds) / sizeof(jkServer_aVoteKinds[0]); i++ )
        if ( !__strcmpi(pName, jkServer_aVoteKinds[i].pName) )
            return jkServer_aVoteKinds[i].bit;
    return 0;
}

static struct
{
    int      type;
    char     arg[128];        // what the admin command gets: "JK1MP:m10.jkl", "8", "30"
    char     desc[96];        // "map m10.jkl", "kick Name", "rank 8 (Jedi Lord)"
    uint32_t kickNetId;       // a kick's player, by net id (slots can change)
    uint32_t endMs;
    int      yes, no;
    uint32_t aVoted[JKPLAYER_NUM_INFOS + 1];
    int      numVoted;
    int      result;          // 1 passed, -1 failed: applied on the next tick
} jkServer_vote;

static struct { uint32_t netId; int count; } jkServer_aCallers[JKPLAYER_NUM_INFOS + 1];
static int jkServer_numCallers = 0;
static void* jkServer_pVoteWorld = NULL;   // the level the callvote counts are for

static int jkServer_SlotOfNetId(uint32_t netId)
{
    for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
        if ( (jkPlayer_playerInfos[i].flags & 1) && jkPlayer_playerInfos[i].playerNetId == netId )
            return i;
    return -1;
}

// Players who can vote: everyone in the game but a dedicated server's own slot.
static int jkServer_NumVoters(void)
{
    int n = 0;
    for ( int i = 0; i < jkPlayer_maxPlayers; i++ )
    {
        SithPlayer* p = &jkPlayer_playerInfos[i];
        if ( !(p->flags & 1) || !p->playerNetId ) continue;
        if ( jkGuiNetHost_bIsDedicated && p->playerNetId == (uint32_t)stdComm_dplayIdSelf ) continue;
        n++;
    }
    return n;
}

// "Server: ..." to everyone (and this machine's console).
static void jkServer_Announce(const char* fmt, ...)
{
    char aLine[160], aMsg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(aLine, sizeof(aLine), fmt, ap);
    va_end(ap);
    stdString_snprintf(aMsg, sizeof(aMsg), "Server: %s", aLine);
    sithMulti_SendChat(aMsg, -1, -1);
    sithConsole_PrintString(aMsg);
}

// "Server: ..." to one player.
static void jkServer_Tell(uint32_t netId, const char* pText)
{
    char aMsg[128];
    if ( netId == (uint32_t)stdComm_dplayIdSelf )
    {
        sithConsole_PrintString(pText);
        return;
    }
    stdString_snprintf(aMsg, sizeof(aMsg), "Server: %s", pText);
    sithMulti_SendChatTo(aMsg, -1, -1, (int)netId);
}

static void jkServer_VoteCheck(void)
{
    int n = jkServer_NumVoters();
    if ( jkServer_vote.type == JKVOTE_NONE || jkServer_vote.result )
        return;
    if ( (int32_t)(stdPlatform_GetTimeMsec() - jkServer_vote.endMs) >= 0 )
        jkServer_vote.result = -1;
    else if ( jkServer_vote.yes > n / 2 )
        jkServer_vote.result = 1;
    else if ( jkServer_vote.no >= n / 2 )
        jkServer_vote.result = -1;
}

static void jkServer_VoteApply(int type)
{
    char aArg[16];
    switch ( type )
    {
        case JKVOTE_MAP: jkServer_ChangeMap(jkServer_vote.arg, 1); break;
        case JKVOTE_RANK: jkServer_CmdRank(NULL, jkServer_vote.arg); break;
        case JKVOTE_SCORELIMIT: jkServer_SetLimit(jkServer_vote.arg, 0); break;
        case JKVOTE_TIMELIMIT: jkServer_SetLimit(jkServer_vote.arg, 1); break;
        case JKVOTE_KICK:
        {
            int slot = jkServer_SlotOfNetId(jkServer_vote.kickNetId);
            if ( slot < 0 ) break;   // gone already
            stdString_snprintf(aArg, sizeof(aArg), "%d", slot);
            jkServer_Kick(aArg, 0);
            break;
        }
    }
}

static void jkServer_VoteTick(void)
{
    if ( !sithNet_isMulti || !sithNet_isServer )
    {
        jkServer_vote.type = JKVOTE_NONE;
        return;
    }
    // A new level: everyone may call votes again (and a vote still running ends).
    if ( (void*)sithWorld_g_pCurrentWorld != jkServer_pVoteWorld )
    {
        jkServer_pVoteWorld = sithWorld_g_pCurrentWorld;
        jkServer_numCallers = 0;
        if ( jkServer_vote.type != JKVOTE_NONE && !jkServer_vote.result )
            jkServer_Announce("Vote cancelled: the level changed.");
        jkServer_vote.type = JKVOTE_NONE;
    }
    jkServer_VoteCheck();
    if ( jkServer_vote.type == JKVOTE_NONE || !jkServer_vote.result )
        return;
    if ( jkServer_vote.result > 0 )
    {
        jkServer_Announce("Vote passed: %s.", jkServer_vote.desc);
        stdLog_Printf("admin", "vote passed: %s (yes %d, no %d)", jkServer_vote.desc, jkServer_vote.yes, jkServer_vote.no);
        int type = jkServer_vote.type;
        jkServer_vote.type = JKVOTE_NONE;   // before applying: a map vote ends the level
        jkServer_VoteApply(type);
    }
    else
    {
        jkServer_Announce("Vote failed: %s.", jkServer_vote.desc);
        stdLog_Printf("admin", "vote failed: %s (yes %d, no %d)", jkServer_vote.desc, jkServer_vote.yes, jkServer_vote.no);
        jkServer_vote.type = JKVOTE_NONE;
    }
}

static void jkServer_CallVote(uint32_t netId, const char* pArgs)
{
    char aArgs[160], aWhat[16], aVal[128], aName[32], aMsg[128], aFound[128];
    char* pSpace;
    int slot = jkServer_SlotOfNetId(netId), type = JKVOTE_NONE, count = 0, c;

    if ( jkGuiNetHost_bIsDedicated && netId == (uint32_t)stdComm_dplayIdSelf )
    {
        sithConsole_PrintString("The server doesn't vote; map, kick, rank, timelimit and scorelimit do it directly.");
        return;
    }
    if ( !(jkServer_allowVote & JKSERVER_VOTE_ALL) )
    {
        jkServer_Tell(netId, "Voting is off on this server.");
        return;
    }
    if ( slot < 0 )
        return;
    jkServer_Trim(pArgs, aArgs, sizeof(aArgs));
    aWhat[0] = aVal[0] = 0;
    pSpace = strchr(aArgs, ' ');
    if ( pSpace )
    {
        *pSpace = 0;
        jkServer_Trim(pSpace + 1, aVal, sizeof(aVal));
    }
    stdString_SafeStrCopy(aWhat, aArgs, sizeof(aWhat));
    if ( jkServer_VoteBit(aWhat) && !(jkServer_allowVote & jkServer_VoteBit(aWhat)) )
    {
        stdString_snprintf(aMsg, sizeof(aMsg), "Voting on %s is off on this server.", aWhat);
        jkServer_Tell(netId, aMsg);
        return;
    }
    if ( jkServer_vote.type != JKVOTE_NONE )
    {
        jkServer_Tell(netId, "A vote is already in progress.");
        return;
    }
    for ( c = 0; c < jkServer_numCallers; c++ )
        if ( jkServer_aCallers[c].netId == netId ) { count = jkServer_aCallers[c].count; break; }
    if ( count >= JKSERVER_MAX_CALLVOTES )
    {
        jkServer_Tell(netId, "You have called the most votes allowed on this level.");
        return;
    }

    memset(&jkServer_vote, 0, sizeof(jkServer_vote));
    if ( !__strcmpi(aWhat, "map") && aVal[0] )
    {
        if ( !jkMain_FindLevel(aVal, NULL, aFound, sizeof(aFound)) )
        {
            stdString_snprintf(aMsg, sizeof(aMsg), "No level %s on this server (servermaps lists them).", aVal);
            jkServer_Tell(netId, aMsg);
            return;
        }
        type = JKVOTE_MAP;
        stdString_SafeStrCopy(jkServer_vote.arg, aFound, sizeof(jkServer_vote.arg));
        stdString_snprintf(jkServer_vote.desc, sizeof(jkServer_vote.desc), "map %s", strchr(aFound, ':') ? strchr(aFound, ':') + 1 : aFound);
    }
    else if ( !__strcmpi(aWhat, "kick") && aVal[0] )
    {
        int target = jkServer_FindPlayer(aVal);
        if ( target < 0 )
        {
            stdString_snprintf(aMsg, sizeof(aMsg), "No player %s.", aVal);
            jkServer_Tell(netId, aMsg);
            return;
        }
        type = JKVOTE_KICK;
        jkServer_vote.kickNetId = jkPlayer_playerInfos[target].playerNetId;
        jkServer_PlayerName(target, aName, sizeof(aName));
        stdString_snprintf(jkServer_vote.desc, sizeof(jkServer_vote.desc), "kick %s", aName);
    }
    else if ( !__strcmpi(aWhat, "rank") && aVal[0] && strspn(aVal, "0123456789") == strlen(aVal) && atoi(aVal) <= 8 )
    {
        type = JKVOTE_RANK;
        stdString_SafeStrCopy(jkServer_vote.arg, aVal, sizeof(jkServer_vote.arg));
        jkServer_RankName(atoi(aVal), aName, sizeof(aName));
        stdString_snprintf(jkServer_vote.desc, sizeof(jkServer_vote.desc), "rank %d (%s)", atoi(aVal), aName);
    }
    else if ( (!__strcmpi(aWhat, "scorelimit") || !__strcmpi(aWhat, "timelimit")) && aVal[0]
              && strspn(aVal, "0123456789") == strlen(aVal) && strlen(aVal) <= 6 )
    {
        int bTime = !__strcmpi(aWhat, "timelimit");
        type = bTime ? JKVOTE_TIMELIMIT : JKVOTE_SCORELIMIT;
        stdString_SafeStrCopy(jkServer_vote.arg, aVal, sizeof(jkServer_vote.arg));
        if ( !atoi(aVal) )
            stdString_snprintf(jkServer_vote.desc, sizeof(jkServer_vote.desc), bTime ? "no time limit" : "no score limit");
        else
            stdString_snprintf(jkServer_vote.desc, sizeof(jkServer_vote.desc), bTime ? "time limit %d minutes" : "score limit %d", atoi(aVal));
    }
    else
    {
        jkServer_Tell(netId, "callvote map <level> | kick <name or slot> | rank <0-8> | scorelimit <points> | timelimit <minutes>");
        return;
    }

    jkServer_vote.type = type;
    jkServer_vote.endMs = stdPlatform_GetTimeMsec() + JKSERVER_VOTE_MS;
    jkServer_vote.yes = 1;   // the caller
    jkServer_vote.aVoted[jkServer_vote.numVoted++] = netId;
    if ( c < jkServer_numCallers )
        jkServer_aCallers[c].count++;
    else if ( jkServer_numCallers < (int)(sizeof(jkServer_aCallers) / sizeof(jkServer_aCallers[0])) )
    {
        jkServer_aCallers[jkServer_numCallers].netId = netId;
        jkServer_aCallers[jkServer_numCallers++].count = 1;
    }
    jkServer_PlayerName(slot, aName, sizeof(aName));
    jkServer_Announce("%s called a vote: %s", aName, jkServer_vote.desc);
    jkServer_Announce("vote yes or vote no in the console, 30 s");
    stdLog_Printf("admin", "%s called a vote: %s", aName, jkServer_vote.desc);
    jkServer_VoteCheck();
}

static void jkServer_CastVote(uint32_t netId, const char* pArg)
{
    char aArg[32];
    int bYes;
    if ( jkGuiNetHost_bIsDedicated && netId == (uint32_t)stdComm_dplayIdSelf )
    {
        sithConsole_PrintString("The server doesn't vote; cancelvote ends a vote.");
        return;
    }
    if ( jkServer_vote.type == JKVOTE_NONE || jkServer_vote.result )
    {
        jkServer_Tell(netId, "No vote in progress.");
        return;
    }
    for ( int i = 0; i < jkServer_vote.numVoted; i++ )
        if ( jkServer_vote.aVoted[i] == netId )
        {
            jkServer_Tell(netId, "Vote already cast.");
            return;
        }
    if ( jkServer_SlotOfNetId(netId) < 0 )
        return;
    jkServer_Trim(pArg, aArg, sizeof(aArg));
    // yes/y, no/n, or a number: 1 or more yes, 0 or less no
    if ( !__strcmpi(aArg, "yes") || !__strcmpi(aArg, "y") )
        bYes = 1;
    else if ( !__strcmpi(aArg, "no") || !__strcmpi(aArg, "n") )
        bYes = 0;
    else if ( aArg[0] && strspn(aArg + (aArg[0] == '-'), "0123456789") == strlen(aArg + (aArg[0] == '-')) )
        bYes = atoi(aArg) >= 1;
    else
    {
        jkServer_Tell(netId, "vote yes | vote no   (or vote 1 / vote 0)");
        return;
    }
    if ( jkServer_vote.numVoted < (int)(sizeof(jkServer_vote.aVoted) / sizeof(jkServer_vote.aVoted[0])) )
        jkServer_vote.aVoted[jkServer_vote.numVoted++] = netId;
    if ( bYes ) jkServer_vote.yes++;
    else jkServer_vote.no++;
    jkServer_Announce("Vote: %d yes, %d no (%s)", jkServer_vote.yes, jkServer_vote.no, jkServer_vote.desc);
    jkServer_VoteCheck();
}

// On the host: "!callvote ..." / "!vote ..." chat from a player (sithMulti_ProcessChat).
// 1 if it was one (not shown as chat).
int jkServer_HandleChat(uint32_t senderNetId, const char* pText)
{
    if ( !sithNet_isMulti || !sithNet_isServer || !pText )
        return 0;
    if ( !__strnicmp(pText, "!callvote", 9) && (!pText[9] || pText[9] == ' ') )
    {
        jkServer_CallVote(senderNetId, pText + 9);
        return 1;
    }
    if ( !__strnicmp(pText, "!vote", 5) && (!pText[5] || pText[5] == ' ') )
    {
        jkServer_CastVote(senderNetId, pText + 5);
        return 1;
    }
    return 0;
}

// Console "callvote ..." / "vote ...": to the host (or, on the host, right here).
static void jkServer_SendVoteCommand(const char* pWhich, const char* pArgStr)
{
    char aArgs[128], aMsg[160];
    if ( !sithNet_isMulti )
    {
        sithConsole_PrintString("Not in a multiplayer game.");
        return;
    }
    jkServer_Trim(pArgStr, aArgs, sizeof(aArgs));
    if ( sithNet_isServer )
    {
        if ( !strcmp(pWhich, "callvote") ) jkServer_CallVote((uint32_t)stdComm_dplayIdSelf, aArgs);
        else jkServer_CastVote((uint32_t)stdComm_dplayIdSelf, aArgs);
        return;
    }
    stdString_snprintf(aMsg, sizeof(aMsg), "!%s %s", pWhich, aArgs);
    sithMulti_SendChatTo(aMsg, -1, -1, sithNet_serverNetId);
}

static int jkServer_CmdCallVote(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArgs[128];
    jkServer_Trim(pArgStr, aArgs, sizeof(aArgs));
    if ( !aArgs[0] )
    {
        sithConsole_PrintString("callvote map <level> | kick <name or slot> | rank <0-8> | scorelimit <points> | timelimit <minutes>");
        sithConsole_PrintString("servermaps [text] lists the server's levels; vote yes / vote no votes.");
        return 1;
    }
    jkServer_SendVoteCommand("callvote", aArgs);
    return 1;
}

static int jkServer_CmdVote(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    jkServer_SendVoteCommand("vote", pArgStr);
    return 1;
}

// Admin: togglevote lists the kinds of vote and whether each is allowed; togglevote
// <value> switches that one (or several, added up) on or off (sv_allowVote, saved).
static int jkServer_CmdToggleVote(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[16];
    int v;
    if ( !jkServer_IsHost() )
        return 1;
    jkServer_Trim(pArgStr, aArg, sizeof(aArg));
    if ( aArg[0] )
    {
        if ( strspn(aArg, "0123456789") != strlen(aArg) || !(v = atoi(aArg)) || (v & ~JKSERVER_VOTE_ALL) )
        {
            sithConsole_PrintString("togglevote <value>   (togglevote lists the values)");
            return 1;
        }
        sithCvar_SetRaw("sv_allowVote", (intptr_t)(jkServer_allowVote ^ v));
        sithCvar_SaveGlobals();
        stdLog_Printf("admin", "sv_allowVote %d", jkServer_allowVote);
        for ( size_t i = 0; i < sizeof(jkServer_aVoteKinds) / sizeof(jkServer_aVoteKinds[0]); i++ )
            if ( v & jkServer_aVoteKinds[i].bit )
                jkServer_Print("Voting on %s: %s", jkServer_aVoteKinds[i].pName, (jkServer_allowVote & jkServer_aVoteKinds[i].bit) ? "on" : "off");
        return 1;
    }
    jkServer_Print("Vote kinds (sv_allowVote %d):", jkServer_allowVote);
    for ( size_t i = 0; i < sizeof(jkServer_aVoteKinds) / sizeof(jkServer_aVoteKinds[0]); i++ )
        jkServer_Print("%4d  %-11s %s", jkServer_aVoteKinds[i].bit, jkServer_aVoteKinds[i].pName,
                       (jkServer_allowVote & jkServer_aVoteKinds[i].bit) ? "on" : "off");
    sithConsole_PrintString("togglevote <value> switches one on or off.");
    return 1;
}

// Admin: end the vote in progress.
static int jkServer_CmdCancelVote(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    if ( !jkServer_IsHost() )
        return 1;
    if ( jkServer_vote.type == JKVOTE_NONE )
    {
        sithConsole_PrintString("No vote in progress.");
        return 1;
    }
    jkServer_Announce("Vote cancelled: %s.", jkServer_vote.desc);
    stdLog_Printf("admin", "vote cancelled: %s", jkServer_vote.desc);
    jkServer_vote.type = JKVOTE_NONE;
    return 1;
}

// "servermaps [text]": the server's levels, from its own server ("maplist", which its
// rcon answers without the password). On the host, maplist itself.
static int jkServer_CmdServerMaps(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    static char aOutput[JKSERVER_RCON_OUTPUT_MAX];
    char aArgs[64], aCmd[96], aServer[256];
    uint8_t aGuid[16];
    jkServer_Trim(pArgStr, aArgs, sizeof(aArgs));
    stdString_snprintf(aCmd, sizeof(aCmd), aArgs[0] ? "maplist %s" : "maplist", aArgs);
    if ( !sithNet_isMulti || sithNet_isServer )
    {
        sithConsole_ExeCommand(aCmd);
        return 1;
    }
    stdComm_DPlay_RconServer(aServer, sizeof(aServer));
    if ( !aServer[0] || !dps_Rcon(aServer, stdComm_DPlay_RconGuid(aGuid) ? aGuid : NULL, "", aCmd, aOutput, sizeof(aOutput),
                                  JKSERVER_RCON_TIMEOUT_MS) )
    {
        sithConsole_PrintString("The server didn't answer (dedicated servers running their own server list their levels).");
        return 1;
    }
    for ( char* pLine = strtok(aOutput, "\n"); pLine; pLine = strtok(NULL, "\n") )
        sithConsole_PrintString(pLine);
    return 1;
}

void jkServer_RegisterCommands(void)
{
    sithConsole_RegisterCommand(jkServer_CmdStatus, "status", 0);
    sithConsole_RegisterCommand(jkServer_CmdKick, "kick", 0);   // over sithCommand_Kick: by slot too, and it sticks
    sithConsole_RegisterCommand(jkServer_CmdBan, "ban", 0);
    sithConsole_RegisterCommand(jkServer_CmdUnban, "unban", 0);
    sithConsole_RegisterCommand(jkServer_CmdBans, "bans", 0);
    sithConsole_RegisterCommand(jkServer_CmdReconnectAll, "reconnectall", 0);
    sithConsole_RegisterCommand(jkServer_CmdSay, "say", 0);
    sithConsole_RegisterCommand(jkServer_CmdMap, "map", 0);
    sithConsole_RegisterCommand(jkServer_CmdNextMap, "nextmap", 0);
    sithConsole_RegisterCommand(jkServer_CmdMapList, "maplist", 0);
    sithConsole_RegisterCommand(jkServer_CmdRestart, "restart", 0);
    sithConsole_RegisterCommand(jkServer_CmdTimeLimit, "timelimit", 0);
    sithConsole_RegisterCommand(jkServer_CmdScoreLimit, "scorelimit", 0);
    sithConsole_RegisterCommand(jkServer_CmdPassword, "password", 0);
    sithConsole_RegisterCommand(jkServer_CmdRank, "rank", 0);
    sithConsole_RegisterCommand(jkServer_CmdRank, "maxrank", 0);
    sithConsole_RegisterCommand(jkServer_CmdRcon, "rcon", 0);
    sithConsole_RegisterCommand(jkServer_CmdRconPassword, "rcon_password", 0);
    sithConsole_RegisterCommand(jkServer_CmdRconAddress, "rcon_address", 0);
    sithConsole_RegisterCommand(jkServer_CmdCallVote, "callvote", 0);
    sithConsole_RegisterCommand(jkServer_CmdVote, "vote", 0);
    sithConsole_RegisterCommand(jkServer_CmdCancelVote, "cancelvote", 0);
    sithConsole_RegisterCommand(jkServer_CmdToggleVote, "togglevote", 0);
    sithConsole_RegisterCommand(jkServer_CmdServerMaps, "servermaps", 0);
}

#endif // QOL_IMPROVEMENTS && PLATFORM_DPLAY
