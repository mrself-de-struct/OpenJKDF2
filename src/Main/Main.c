#include "Main.h"

#include "../jk.h"
#include "stdPlatform.h"
#include "Cog/jkCog.h"
#include "Gui/jkGUINetHost.h"
#include "Gui/jkGUISound.h"
#include "Gui/jkGUIMultiplayer.h"
#include "Gui/jkGUISetup.h"
#include "Gui/jkGUIDisplay.h"
#include "Gui/jkGUIForce.h"
#include "Gui/jkGUIMain.h"
#include "Gui/jkGUIPlayer.h"
#include "Gui/jkGUIEsc.h"
#include "Gui/jkGUIMap.h"
#include "Gui/jkGUIKeyboard.h"
#include "Gui/jkGUIJoystick.h"
#include "Gui/jkGUIDialog.h"
#include "Gui/jkGUIMouse.h"
#include "Gui/jkGUIControlOptions.h"
#include "Gui/jkGUIObjectives.h"
#include "Gui/jkGUISingleTally.h"
#include "Gui/jkGUIMultiTally.h"
#include "Gui/jkGUIBuildMulti.h"
#include "Gui/jkGUITitle.h"
#include "Gui/jkGUIGeneral.h"
#include "Gui/jkGUIGameplay.h"
#include "Gui/jkGUIDecision.h"
#include "Gui/jkGUISingleplayer.h"
#include "Gui/jkGUIControlSaveLoad.h"
#include "Gui/jkGUISaveLoad.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUIMods.h"
#include "World/jkPlayer.h"
#include "Dw/dwMain.h" // Added: DroidWorks app layer
#include "Gameplay/jkSaber.h"
#include "Win95/std.h"
#include "Win95/stdDisplay.h"
#include "Win95/stdConsole.h"
#include "Platform/wuRegistry.h"
#include "Platform/std3D.h"
#include "Win95/Video.h"
#include "Win95/Window.h"
#include "Win95/Windows.h"
#include "Main/jkGob.h"
#include "Main/jkRes.h"
#include "Main/jkStrings.h"
#include "Main/jkAI.h"
#include "Main/jkEpisode.h"
#include "Main/jkDev.h"
#include "Main/jkGame.h"
#include "Main/jkHudInv.h"
#include "Main/jkCutscene.h"
#include "Main/jkCredits.h"
#include "Main/jkControl.h"
#include "Main/jkSmack.h"
#include "Main/smack.h"
#include "Main/jkMain.h"
#include "Main/jkQuakeConsole.h"
#include "Main/jkTouchControls.h"
#include "Engine/rdroid.h"
#include "Main/sithMain.h"
#include "Dss/sithMulti.h"
#include "General/stdString.h"
#include "General/stdFnames.h"
#include "General/util.h"
#include "General/stdFileUtil.h"
#include "General/stdJSON.h"
#include "Dss/jkDSS.h"
#include "Main/InstallHelper.h"
#include "sithCvar.h"

#include "Platform/Common/stdHttp.h"
#include "Platform/Common/stdLog.h" // Added
#ifdef PLATFORM_DPLAY
#include "Platform/Networking/DPlay/stdComm_dplay.h" // Added
#include "Platform/Networking/DPlay/jkrelay_proto.h" // Added: JKR_DEFAULT_SERVER_PORT
#include "Main/jkServer.h" // Added
#endif
#include "Platform/Common/stdUpdater.h"

#if defined(PLATFORM_POSIX)
#include <locale.h>
#endif

#if defined(SDL2_RENDER)
#include "SDL2_helper.h"
#ifndef _WIN32
#include <unistd.h>
#endif
#include <sys/types.h>
#include <stdbool.h>
#if defined(LINUX) || defined(MACOS)
#include <pwd.h>
#endif
#include "nfd.h"
#endif

#ifdef LINUX
#include "external/fcaseopen/fcaseopen.h"
#endif

static HostServices hs;

char Main_strServerConfig[256] = SERVER_FNAME_DEFAULT; // Added: -serverConfig (SERVER_FNAME)

#ifdef QOL_IMPROVEMENTS
int32_t Main_bDedicatedServer = 0;
int32_t Main_bAutostart = 0;
int32_t Main_bAutostartSp = 0;
int32_t Main_bHeadless = 0;
int32_t Main_bVerboseNetworking = 0;
int32_t Main_bMotsCompat = 0;
int32_t Main_bDwCompat = 0;
int32_t Main_bDroidWorks = 0; // full DroidWorks game mode (implies Main_bDwCompat)
int32_t Main_bDwCogVerbs = 0; // import DW COG verbs into JK/MOTS
int32_t Main_bEnhancedCogVerbs = 0;
char Main_strEpisode[129];
char Main_strMap[128+4];
static char Main_strAutostartCharacter[32];
// Added: dedicated server connection (-connection, -relayServer, -webLobby)
static char Main_strConnection[32];
static char Main_strNetServer[256];
static int Main_listenPort = 0;   // Added: -port, a dedicated server's own server (see Main_StartupDedicated)
static int Main_wsPort = 0;       // Added: -wsPort, its WebSocket port for browser players (0 = none)
static char Main_strMaster[256];  // Added: -master, the relay server a dedicated server lists itself on
#endif

#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
// Added: a dedicated server's map rotation: "maps" in server.json (in that order,
// or shuffled with "shuffleMaps"), each "level" or "GOB:level" from any episode in
// the episode folder (a bare level: the server's first episode first; jkMain_FindLevel).
// The first one is the first level unless the command line names one.
#define MAIN_MAX_MAPS 32
#define MAIN_MAP_LEN  64   // "level" or "GOB:level"
static char Main_aMaps[MAIN_MAX_MAPS][MAIN_MAP_LEN];
static int Main_numMaps = 0;
static int Main_mapIdx = 0;
static int Main_bShuffleMaps = 0;

static void Main_LoadRotation(void)
{
    char aList[512];
    int n = 0;
    Main_numMaps = stdJSON_GetStringList(SERVER_FNAME, "maps", &Main_aMaps[0][0], MAIN_MAX_MAPS, MAIN_MAP_LEN);
    Main_bShuffleMaps = wuRegistry_GetBool("shuffleMaps", 0);
    for (int i = 0; i < Main_numMaps; i++)
        n += stdString_snprintf(aList + n, sizeof(aList) - n, "%s%s", i ? ", " : "", Main_aMaps[i]);
    Main_mapIdx = (Main_bShuffleMaps && Main_numMaps) ? (int)(_frand() * Main_numMaps) % Main_numMaps : 0;
    if (Main_numMaps > 1)
        stdLog_Printf("server", "maps: %s (%s)", aList, Main_bShuffleMaps ? "shuffled" : "in this order");
}

// Moves the rotation on and returns the next map pResolve finds ("GOB:level.jkl",
// any episode), NULL without a rotation (fewer than 2 maps) or if none is found.
const char* Main_NextRotationMap(int (*pResolve)(const char* pSpec, char* pOut, size_t outLen))
{
    static char aFound[128];
    if (Main_numMaps < 2)
        return NULL;
    for (int tries = 0; tries < Main_numMaps; tries++)
    {
        if (Main_bShuffleMaps)
            Main_mapIdx = (Main_mapIdx + 1 + (int)(_frand() * (Main_numMaps - 1)) % (Main_numMaps - 1)) % Main_numMaps;
        else
            Main_mapIdx = (Main_mapIdx + 1) % Main_numMaps;
        if (pResolve(Main_aMaps[Main_mapIdx], aFound, sizeof(aFound)))
            return aFound;
        stdLog_Printf("server", "map %s not found (episode folder), skipped", Main_aMaps[Main_mapIdx]);
    }
    return NULL;
}
#endif

#if defined(QOL_IMPROVEMENTS)
static inline int Autostart_CharacterNameValid(const char* name)
{
    if (!name || !name[0]) return 0;
    size_t length = 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
        if (++length > 31 || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
            || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    }
    return 1;
}

int Main_StartupDedicated(int bFullyDedicated)
{
    char* pRemoveExt;
    int bExplicitMap = 1;
    char aTmpPlayerShortName[32];
    // Altered: MotS's first deathmatch level (was none, so a MotS server without -map didn't start)
    const char* defaultEpisode = Main_bMotsCompat ? "JKM_MP" : "JK1MP";
    const char* defaultMap = Main_bMotsCompat ? "mdm02_freezer.jkl" : "m2.jkl";

    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_TITLE;
    Window_SetDrawHandlers(stdDisplay_DrawAndFlipGdi, stdDisplay_SetCooperativeLevel);

    if (!strlen(Main_strEpisode)) {
        strcpy(Main_strEpisode, defaultEpisode);
        bExplicitMap = 0;
    }
    if (!strlen(Main_strMap)) {
        strcpy(Main_strEpisode, defaultEpisode); // TODO
        strcpy(Main_strMap, defaultMap);
        bExplicitMap = 0;
    }
    if (pRemoveExt = strchr(Main_strEpisode, '.')) {
        *pRemoveExt = 0;
    }
    if (pRemoveExt = strchr(Main_strMap, '.')) {
        *pRemoveExt = 0;
    }
    strcat(Main_strMap, ".jkl");
    stdPlatform_Printf("Autostarting level: `%s`\n", Main_strMap);
    stdLog_Printf("server", "%s starting", bFullyDedicated ? "dedicated server" : "autostart"); // Added
    if (bFullyDedicated)
        stdLog_Printf("server", "settings: %s", SERVER_FNAME); // Added

    if (bFullyDedicated) {
        strcpy(aTmpPlayerShortName, "ServerDed");
        stdString_CharToWchar(jkPlayer_playerShortName, aTmpPlayerShortName, 31);
        jkPlayer_playerShortName[31] = 0;
        jkPlayer_CreateConf(u"ServerDed");
    }
    else {
        wuRegistry_GetStr("playerShortName", aTmpPlayerShortName, 32, "ServerDed");
        // An existing empty registry value is also an unset profile.
        if (!aTmpPlayerShortName[0]) {
            stdString_SafeStrCopy(aTmpPlayerShortName, "ServerDed", sizeof(aTmpPlayerShortName));
        }
        stdString_CharToWchar(jkPlayer_playerShortName, aTmpPlayerShortName, 31);
        jkPlayer_playerShortName[31] = 0;
        jkPlayer_CreateConf(jkPlayer_playerShortName);
    }
    

    // Dedicated player has no control at all
    if (bFullyDedicated) {
        jkGuiNetHost_bIsDedicated = 1;
    }

//#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
    jkGuiNetHost_SaveSettings();
    jkGuiNetHost_LoadSettings();
//#endif // !defined(TARGET_NO_MULTIPLAYER_MENUS)

    // Fake player
    stdString_SafeWStrCopy(jkGuiMultiplayer_mpcInfo.name, u"", 32);
    stdString_SafeStrCopy(jkGuiMultiplayer_mpcInfo.model, "ky.3do", 32);
    stdString_SafeStrCopy(jkGuiMultiplayer_mpcInfo.soundClass, "ky.snd", 32);
#ifndef OPTIMIZE_OUT_UNUSED_FIELDS
    //stdString_SafeStrCopy(jkGuiMultiplayer_mpcInfo.gap80, "", 32);
#endif
    stdString_SafeStrCopy(jkGuiMultiplayer_mpcInfo.sideMat, "sabergreen1.mat", 32);
    stdString_SafeStrCopy(jkGuiMultiplayer_mpcInfo.tipMat, "sabergreen0.mat", 32);
    jkGuiMultiplayer_mpcInfo.jediRank = 0;

    // Set up minimal render settings
    //jkPlayer_fov = 90;
    //jkPlayer_fovIsVertical = 1;
    //jkPlayer_enableTextureFilter = 0;
    //jkPlayer_enableOrigAspect = 0;
    //jkPlayer_enableBloom = 0;
    //jkPlayer_fpslimit = 150;
    //jkPlayer_enableVsync = 0;
    //jkPlayer_ssaaMultiple = 1.0;
    //jkPlayer_enableSSAO = 0;
    //jkPlayer_gamma = 1.0;
    
    jkMultiEntry3 v34;
    memset(&v34, 0, sizeof(v34));

    if (bExplicitMap) {
        wuRegistry_SaveStr("serverEpisodeGob", Main_strEpisode);
        wuRegistry_SaveStr("serverMapJkl", Main_strMap);
    }

    wuRegistry_GetWString("gameName", v34.serverName, 32, u"OpenJKDF2 Dedicated Server");
    // Altered: jkGuiNetHost_SaveSettings above saves an empty "gameName" on a new profile
    if (!v34.serverName[0])
        stdString_SafeWStrCopy(v34.serverName, u"OpenJKDF2 Dedicated Server", 32);
    wuRegistry_GetWString("serverPassword", v34.wPassword, 32, u"");
#ifndef TARGET_DREAMCAST // TODO writable config
    wuRegistry_GetStr("serverEpisodeGob", v34.episodeGobName, 32, Main_strEpisode);
    wuRegistry_GetStr("serverMapJkl", v34.mapJklFname, 32, Main_strMap);
#else
    stdString_SafeStrCopy(v34.episodeGobName, Main_strEpisode, 32);
    stdString_SafeStrCopy(v34.mapJklFname, Main_strMap, 32);
#endif
#ifdef PLATFORM_DPLAY
    // Added: the rotation's first map, unless the command line named one
    if (bFullyDedicated)
    {
        Main_LoadRotation();
        if (Main_numMaps && !bExplicitMap)
        {
            // "level" (looked for in serverEpisodeGob first) or "GOB:level"
            char aFound[128];
            char* pColon;
            if (jkMain_FindLevel(Main_aMaps[Main_mapIdx], v34.episodeGobName, aFound, sizeof(aFound)) && (pColon = strchr(aFound, ':')))
            {
                *pColon = 0;
                stdString_SafeStrCopy(v34.episodeGobName, aFound, 32);
                stdString_SafeStrCopy(v34.mapJklFname, pColon + 1, 32);
            }
            else
                stdLog_Printf("server", "map %s not found (episode folder), starting with %s", Main_aMaps[Main_mapIdx], v34.mapJklFname);
        }
    }
#endif

    if (_wcslen(v34.wPassword)) {
        jkGuiNetHost_sessionFlags |= SESSIONFLAG_PASSWORD;
    }

    jkGuiNetHost_sessionFlags |= SESSIONFLAG_ISDEDICATED;

    v34.tickRateMs = jkGuiNetHost_tickRate;
    v34.maxPlayers = jkGuiNetHost_maxPlayers;
    v34.sessionFlags = jkGuiNetHost_sessionFlags;
    v34.multiModeFlags = jkGuiNetHost_gameFlags;
    v34.maxRank = jkGuiNetHost_maxRank;
    v34.scoreLimit = jkGuiNetHost_scoreLimit;
    v34.timeLimit = jkGuiNetHost_timeLimit;

    sithNet_scorelimit = v34.scoreLimit;
    sithNet_multiplayer_timelimit = v34.timeLimit;

    // Explicit selection must fail before creating a host session. Initialize
    // inventory first so MPC personality ammo maxima survive items.dat loading.
    if (Main_strAutostartCharacter[0]) {
        std3D_StartScene();
        std3D_EndScene();
        sithOpenStatic("static.jkl");
        jkHudInv_InitItems();
        char16_t characterName[32];
        stdString_CharToWchar(characterName, Main_strAutostartCharacter, 31);
        characterName[31] = 0;
        // Match the character menu: load bins before checking the selected
        // character's eligibility for the host rank, then let ordinary player
        // startup reload those bins after the world inventory is initialized.
        if (!jkPlayer_MPCParse(&jkGuiMultiplayer_mpcInfo, NULL,
                              jkPlayer_playerShortName, characterName, 1)) {
            fprintf(stderr, "autostart: selected multiplayer character could not be read\n");
            exit(6);
        }
        if (jkGuiMultiplayer_mpcInfo.jediRank < 0
            || jkGuiMultiplayer_mpcInfo.jediRank > jkGuiNetHost_maxRank) {
            fprintf(stderr, "autostart: selected character rank %d is outside host range 0..%d\n",
                    jkGuiMultiplayer_mpcInfo.jediRank, jkGuiNetHost_maxRank);
            exit(6);
        }
    }


    // Added: say what the limits are; "timeLimit" is in milliseconds (the host
    // screen saves minutes * 60000), so a small number ends every level at once.
    if (bFullyDedicated)
    {
        char aLimits[160];
        int n = 0;
        if (v34.multiModeFlags & MULTIMODEFLAG_TIMELIMIT)
            n += stdString_snprintf(aLimits + n, sizeof(aLimits) - n, "time limit %d s", v34.timeLimit / 1000);
        if (v34.multiModeFlags & MULTIMODEFLAG_SCORELIMIT)
            n += stdString_snprintf(aLimits + n, sizeof(aLimits) - n, "%sscore limit %d", n ? ", " : "", v34.scoreLimit);
        stdString_snprintf(aLimits + n, sizeof(aLimits) - n, "%s%s", n ? ", " : "",
                           (v34.multiModeFlags & MULTIMODEFLAG_SINGLE_LEVEL) ? "this level over and over" : "the episode's levels in turn");
        stdLog_Printf("server", "%s", aLimits);
        if (Main_bMotsCompat)
        {
            if (v34.multiModeFlags & MULTIMODEFLAG_1000)
                stdLog_Printf("server", "MotS game: Jedi only, force rank up to %d (server.json \"bJediOnly\")", v34.maxRank);
            else
                stdLog_Printf("server", "MotS game: personalities (server.json \"bJediOnly\": true for Jedi only)");
        }
        if ((v34.multiModeFlags & MULTIMODEFLAG_TIMELIMIT) && v34.timeLimit < 60000)
            stdLog_Printf("server", "\"timeLimit\" is in milliseconds: %d is %.1f s (30 minutes is 1800000)",
                          v34.timeLimit, v34.timeLimit / 1000.0);
    }

    if (!Main_bAutostartSp) {
#ifdef PLATFORM_DPLAY
        // Added: open the connection the menu would have (a dedicated server
        // defaults to the Imperial Network Protocol relay). Without one the game ran on its own, unjoinable.
        // A dedicated server's settings come from server.json (wuRegistry_UseServerFile);
        // the command line wins over each.
        int bPortArg = Main_listenPort != 0;
        if (bFullyDedicated)
        {
            if (!Main_strConnection[0])
                wuRegistry_GetStr("connection", Main_strConnection, sizeof(Main_strConnection), "");
            if (!Main_strConnection[0])
                stdString_SafeStrCopy(Main_strConnection, "relay", sizeof(Main_strConnection));
            if (!Main_strNetServer[0])
                wuRegistry_GetStr(__strcmpi(Main_strConnection, "web") && __strcmpi(Main_strConnection, "weblobby") ? "relayServer" : "webLobbyUrl",
                                  Main_strNetServer, sizeof(Main_strNetServer), "");
            if (!Main_listenPort)
                Main_listenPort = wuRegistry_GetInt("port", 0);
            if (Main_listenPort <= 0 || Main_listenPort > 65535)
                Main_listenPort = 0;
            if (!Main_wsPort)
                Main_wsPort = wuRegistry_GetInt("wsPort", 0);
            if (Main_wsPort < 0 || Main_wsPort > 65535)
                Main_wsPort = 0;
            if (!Main_strMaster[0])
                wuRegistry_GetStr("master", Main_strMaster, sizeof(Main_strMaster), "");
        }
        // Added: without a relay server (or with -port) it's its own Imperial Network
        // Protocol server: players connect to this machine ("connect host[:port]").
        if (bFullyDedicated && (!__strcmpi(Main_strConnection, "relay") || !__strcmpi(Main_strConnection, "imperial")
                                || !__strcmpi(Main_strConnection, "online"))
            && (!Main_strNetServer[0] || bPortArg))
        {
            int port = Main_listenPort ? Main_listenPort : JKR_DEFAULT_SERVER_PORT;
            if (!stdComm_DPlay_Listen(port, Main_wsPort, Main_strMaster[0] ? Main_strMaster : NULL))
            {
                if (Main_wsPort)
                    stdLog_Printf("server", "can't take UDP port %d or TCP port %d (wsPort), stopping", port, Main_wsPort);
                else
                    stdLog_Printf("server", "can't take UDP port %d, stopping", port);
                exit(1);
            }
            if (Main_wsPort)
                stdLog_Printf("server", "browser players: WebSockets on TCP %d (behind a wss:// proxy for https pages)", Main_wsPort);
            stdString_snprintf(Main_strNetServer, sizeof(Main_strNetServer), "127.0.0.1:%d", port);
            stdLog_Printf("server", "players join with: connect <this machine's address>%s", port == JKR_DEFAULT_SERVER_PORT ? "" : ":<port>");
            if (Main_strMaster[0])
                stdLog_Printf("server", "listing on master server %s", Main_strMaster);
            jkServer_StartupDedicated(); // Added: rcon password, bans
        }
        else if (bFullyDedicated)
        {
            if (Main_strMaster[0])
                stdLog_Printf("server", "\"master\" is only for a server of its own (no relay server set); ignored");
            // Added: hosting on a relay server: rcon through it, and it does the kicks and bans
            // (relay servers from 2026-10-29 on; older ones leave kicks to the kicked player's game)
            if (!__strcmpi(Main_strConnection, "relay") || !__strcmpi(Main_strConnection, "imperial")
                || !__strcmpi(Main_strConnection, "online"))
            {
                stdComm_DPlay_StartTunnel();
                jkServer_StartupDedicated(); // rcon password, bans
                stdLog_Printf("server", "rcon, kicks and bans go through the relay server %s", Main_strNetServer);
            }
        }
        if (Main_strConnection[0]
            && stdComm_DPlay_OpenConnectionNamed(Main_strConnection, Main_strNetServer))
        {
            stdLog_Printf("server", "no connection, stopping");
            if (bFullyDedicated)
                exit(1);
        }
#endif
        int v21;
        for (int attempt = 1; ; attempt++)
        {
            v21 = sithMulti_CreatePlayer(
                  v34.serverName,
                  v34.wPassword,
                  v34.episodeGobName,
                  v34.mapJklFname,
                  v34.maxPlayers,
                  v34.sessionFlags,
                  v34.multiModeFlags,
                  v34.tickRateMs,
                  v34.maxRank);
            // Added: the outcome, for the log
            if (v21)
                stdLog_Printf("server", "couldn't start the game (error %08x)", (unsigned)v21);
            else
                stdLog_Printf("server", "game started%s", bFullyDedicated ? " (dedicated)" : "");
#if defined(PLATFORM_DPLAY) && defined(SDL2_RENDER)
            // Added: a dedicated server keeps trying (the relay server may be down
            // or restarting) instead of waiting on an error dialog nobody sees.
            if (v21 && bFullyDedicated && stdComm_dword_8321DC)
            {
                int waitSecs = attempt < 4 ? 5 : 30;
                stdLog_Printf("server", "trying again in %d s (attempt %d)", waitSecs, attempt);
                SDL_Delay(waitSecs * 1000);
                continue;
            }
#endif
            break;
        }
        if (v21 && bFullyDedicated)
        {
            stdLog_Printf("server", "stopping");
            exit(1);
        }
        if ( v21 == 0x88770118 )
        {
            jkGuiDialog_ErrorDialog(jkStrings_GetUniStringWithFallback("GUINET_HOSTERROR"), jkStrings_GetUniStringWithFallback("GUINET_USERCANCEL"));
        }
        else if ( v21 )
        {
            jkGuiDialog_ErrorDialog(jkStrings_GetUniStringWithFallback("GUINET_HOSTERROR"), jkStrings_GetUniStringWithFallback("GUINET_NOCONNECT"));
        }
    }
    
    
    // Preserve ordinary autostart ordering and initialize exactly once.
    if (!Main_strAutostartCharacter[0]) {
        std3D_StartScene();
        std3D_EndScene();
        sithOpenStatic("static.jkl");
        jkHudInv_InitItems();
    }


    if (!Main_bAutostartSp) {
        if (jkMain_loadFile2(v34.episodeGobName, v34.mapJklFname))
        {
            return 1;
        }
    }
    else {
        stdPlatform_Printf("Loading SP: episode `%s`, map `%s`", v34.episodeGobName, v34.mapJklFname);
        if (jkMain_LoadLevelSingleplayer(v34.episodeGobName, v34.mapJklFname))
        {
            return 1;
        }
    }
    

    return 0;
}
#endif // QOL_IMPROVEMENTS

int Main_Startup(const char *cmdline)
{
    int result; // eax

#if defined(PLATFORM_POSIX)
    // Make sure floating point stuff is using . and not ,
    setlocale(LC_ALL, "C");
#endif

    stdInitServices(&hs);
    jkGuiNetHost_maxRank = 4;
#ifdef QOL_IMPROVEMENTS
    jkGuiNetHost_maxPlayers = JKPLAYER_NUM_INFOS; // Added: every slot (32) by default, was 4 (as the browser build)
#else
    jkGuiNetHost_maxPlayers = 4;
#endif
    Video_modeStruct.geoMode = 4;
    pHS = &hs;
    jkPlayer_setFullSubtitles = 1; // Added: Set subtitles as default for opening cutscene
    jkPlayer_setDisableCutscenes = 0;
    jkPlayer_setRotateOverlayMap = 1;
    jkPlayer_setDrawStatus = 1;
    jkPlayer_setCrosshair = 0;
    jkPlayer_setSaberCam = 0;
    jkGuiNetHost_gameFlags = 144;
    jkGuiNetHost_scoreLimit = 100;
    jkGuiNetHost_timeLimit = 30 * 60000; // Altered: milliseconds (was 30, which ended a dedicated server's level at once)
    jkGuiNetHost_sessionFlags = 0;
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
    // Added: 50 ms default (vanilla 180). Below 100 ms the MP physics step is 150 Hz instead of 50 Hz
    // (engine_config.h) and movement looks smoother; JK.EXE joiners clamp their own rate to 100-300.
    jkGuiNetHost_tickRate = 50;
#else
    jkGuiNetHost_tickRate = 180;
#endif
    Video_modeStruct.modeIdx = 0;
    Video_modeStruct.descIdx = 0;
    Video_modeStruct.Video_8605C8 = 0;
    Video_modeStruct.b3DAccel = 0;
    Video_modeStruct.viewSizeIdx = 5;
    Video_modeStruct.Video_8606A4 = 0;
    Video_modeStruct.Video_8606A8 = 1;
    Video_modeStruct.lightMode = 3;
    Video_modeStruct.texMode = 1;
    Video_modeStruct.Video_8606B8 = 0;
    Video_modeStruct.Video_8606BC = 0;
    Video_modeStruct.Video_8606C0 = 0;
    //Main_path[0] = 0; // Added: We reset this elsewhere
    Main_bWindowGUI = 1;
    Main_bDisplayConfig = 0;
    Main_dword_86078C = 0;
    Main_bFrameRate = 0;
    Main_bDispStats = 0;
    Main_bNoHUD = 0;
    Main_logLevel = 0;
    Main_verboseLevel = 0;
    Main_bDevMode = 0;
    jkGuiSound_musicVolume = 1.0;
    stdPlatform_Printf("%s\n", Main_path);
    Main_ParseCmdLine((char *)cmdline);
#ifdef PLATFORM_DPLAY
    // Added: a dedicated server's hosting settings live in server.json
    if (Main_bDedicatedServer)
        wuRegistry_UseServerFile();
#endif
#ifdef TARGET_TWL
    Main_bNoHUD = 1;
#endif

    if ( Main_logLevel == 1 )
    {
        if ( Main_verboseLevel )
        {
            if ( Main_verboseLevel == 1 )
            {
                hs.messagePrint = stdConsolePrintf;
                hs.errorPrint = stdConsolePrintf;
            }
            else if ( Main_verboseLevel == 2 )
            {
                hs.debugPrint = stdConsolePrintf;
                hs.messagePrint = stdConsolePrintf;
                hs.errorPrint = stdConsolePrintf;
            }
        }
        else
        {
            hs.errorPrint = stdConsolePrintf;
        }
        stdConsole_Startup("Debug", 7u, Main_verboseLevel == 0);
    }
    else if ( Main_logLevel == 2 )
    {
        debug_log_fp = (stdFile_t)fopen("debug.log", "w+");
        if ( Main_verboseLevel )
        {
            if ( Main_verboseLevel == 1 )
            {
                hs.messagePrint = Main_FPrintf;
                hs.errorPrint = Main_FPrintf;
            }
            else if ( Main_verboseLevel == 2 )
            {
                hs.debugPrint = Main_FPrintf;
                hs.messagePrint = Main_FPrintf;
                hs.errorPrint = Main_FPrintf;
            }
        }
        else
        {
            hs.errorPrint = Main_FPrintf;
        }
    }
    stdStartup(&hs); // Added
    InstallHelper_SetCwd(); // Added

    wuRegistry_Startup(HKEY_LOCAL_MACHINE, "Software\\LucasArts Entertainment Company\\JediKnight\\v1.0", (BYTE*)"0.1");
    //stdStartup(&hs); // Moved

    stdHttp_Startup();

    // Added: DroidWorks boots a reduced engine. 
    // It has its own VFS (dwGob/inits), its own GUI, and its own app flow.
    // The sith engine itself is brought up later by dwSith_Startup from dw_Startup
    if (Main_bDroidWorks) {
        Windows_Startup();
        sithCvar_Startup();
        if (!Windows_InitWindow())
            return 0;
        rdStartup(&hs);
        Video_Startup();
        std3D_Startup();
#ifdef QUAKE_CONSOLE
        jkQuakeConsole_Startup(); // Added
#endif
        jkTouchControls_Startup(); // Added
#ifdef RDRASTER_SOFTWARE_RENDERER
        // Force sw renderer for Droidworks, for now
        rdroid_bSoftwareRenderer = 1;
#endif
        if (!dwMain_Startup())
            return 0;
        Window_SetDrawHandlers(stdDisplay_DrawAndFlipGdi, stdDisplay_SetCooperativeLevel);
        return 1;
    }

    jkGob_Startup();
    jkRes_Startup(pHS);
    Windows_Startup();
    jkStrings_Startup();
    sithCvar_Startup(); // Added

    if (Windows_InitWindow())
    {
        rdStartup(&hs);
        jkGuiRend_Startup(); // B15E8
        jkGui_Startup(); // 15C6D1
#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
        jkGuiMultiplayer_Startup();
        jkGuiNetHost_Startup();
#endif
        jkGuiSetup_Startup();
        jkGuiDisplay_Startup();
        jkGuiForce_Startup();
        jkGuiMain_Startup();
        jkGuiPlayer_Startup();
        jkGuiSound_Startup();
        jkGuiEsc_Startup();
        jkGuiMap_Startup();
        jkGuiKeyboard_Startup();
        jkGuiJoystick_Startup();
        jkGuiDialog_Startup();
        jkGuiMouse_Startup();
        jkGuiControlOptions_Startup();
        jkGuiObjectives_Startup();
        jkGuiSingleTally_Startup();
#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
        jkGuiMultiTally_Startup();
        jkGuiBuildMulti_StartupEditCharacter();
#endif
        jkGuiTitle_Startup();
        jkGuiGeneral_Startup();
        jkGuiGameplay_Startup();
        jkGuiDecision_Startup();
        jkGuiSingleplayer_Startup();
#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
        jkGuiBuildMulti_Startup();
#endif
        jkGuiSaveLoad_Startup();
        jkGuiControlSaveLoad_Startup();
#ifdef QOL_IMPROVEMENTS
        jkGuiMods_Startup();
#endif
#ifndef LINUX_TMP
        smack_Startup(); // TODO
#endif
        sithMain_Startup(&hs); // ~TODO
        jkAI_Startup();
        jkCog_Startup();
        sithCog_StartupEnhanced();
        jkEpisode_Startup();
        jkDev_Startup();
        jkGame_Startup();
        Video_Startup();
        jkControl_Startup(); // ~TODO
        jkHudInv_Startup();
        jkDSS_Startup();
        jkCutscene_Startup("ui\\cutStrings.uni");
        jkCredits_Startup("ui\\credits.uni");
        jkSmack_Startup();

        std3D_Startup(); // Added
#ifdef QUAKE_CONSOLE
        jkQuakeConsole_Startup(); // Added
#endif
        jkTouchControls_Startup(); // Added

        if (jkRes_LoadCD(0))
        {
#if defined(QOL_IMPROVEMENTS)
            if (Main_bDedicatedServer || Main_bAutostart) {
                if (Main_StartupDedicated(Main_bDedicatedServer))
                {
                    return 1;
                }
            }
#endif
            if (!Main_bMotsCompat) {
                jkSmack_SmackPlay("01-02a.smk");
            }
            else {
                jkSmack_SmackPlay("jkmintro.san");
            }

            Window_SetDrawHandlers(stdDisplay_DrawAndFlipGdi, stdDisplay_SetCooperativeLevel);
            return 1;
        }
        return 0;
    }

    return 0;
}

void Main_Shutdown()
{
    stdPlatform_Printf("OpenJKDF2: %s\n", __func__);

    dwMain_Shutdown(); // Added: DroidWorks app layer (no-op if not started)
    std3D_Shutdown(); // Added
#ifdef QUAKE_CONSOLE
    jkQuakeConsole_Shutdown();
#endif

    jkSmack_Shutdown();
    jkGuiControlSaveLoad_Shutdown();
    jkGuiSaveLoad_Shutdown();
#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
    jkGuiBuildMulti_Shutdown();
#endif
    jkGuiSingleplayer_Shutdown();
    jkGuiDecision_Shutdown();
    jkGuiGameplay_Shutdown();
    jkGuiGeneral_Shutdown();
    jkGuiTitle_Shutdown();
    jkGuiControlOptions_Shutdown();
    jkGuiMouse_Shutdown();
    jkGuiDialog_Shutdown();
    jkGuiJoystick_Shutdown();
    jkGuiKeyboard_Shutdown();
    jkGuiMap_Shutdown();
    jkGuiEsc_Shutdown();
    jkGuiForce_Shutdown();
    jkGuiDisplay_Shutdown();
    jkGuiSetup_Shutdown();
#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
    jkGuiNetHost_Shutdown();
    jkGuiMultiplayer_Shutdown();
#endif
    jkGuiMain_Shutdown();
    jkGuiPlayer_Shutdown();
    jkGuiSound_Shutdown();
    jkGuiObjectives_Shutdown();
    jkGuiSingleTally_Shutdown();
#ifdef QOL_IMPROVEMENTS
    jkGuiMods_Shutdown();
#endif
    jkGuiRend_Shutdown();
    jkCog_Shutdown();
    sithCloseStatic();
    jkCredits_Shutdown();
    jkCutscene_Shutdown();
    jkDSS_Shutdown();
    jkTouchControls_Shutdown(); // Added
    jkControl_Shutdown(); // Added
    jkHudInv_Shutdown();
    if ( jkCutscene_isRendering )
        jkCutscene_sub_421410();
    Video_Shutdown();
    jkGame_Shutdown();
    jkDev_Shutdown();
    sithShutdown();
#ifndef LINUX_TMP
    smack_Shutdown();
#endif
    jkGui_Shutdown();
    rdShutdown();
    sithCvar_Shutdown(); // Added
    jkStrings_Shutdown();
    Windows_Shutdown();
    jkRes_Shutdown();
    jkGob_Shutdown();
    stdShutdown();
    if ( Main_logLevel == 1 )
    {
        stdConsole_Shutdown();
    }
    else if ( Main_logLevel == 2 )
    {
        fclose((FILE*)debug_log_fp);
    }
    
    jkPlayer_ResetVars(); // Added

    stdHttp_Shutdown(); // Added

    // Added
    Main_bDedicatedServer = 0;
    Main_bAutostart = 0;
    Main_bHeadless = 0;
    Main_bVerboseNetworking = 0;
    Main_bDwCompat = 0;
    Main_bDroidWorks = 0;
    Main_bDwCogVerbs = 0;
    Main_bEnhancedCogVerbs = 0;
    memset(Main_strEpisode, 0, sizeof(Main_strEpisode));
    memset(Main_strMap, 0, sizeof(Main_strMap));
    memset(Main_strAutostartCharacter, 0, sizeof(Main_strAutostartCharacter));
    memset(Main_strConnection, 0, sizeof(Main_strConnection)); // Added
    memset(Main_strNetServer, 0, sizeof(Main_strNetServer)); // Added
    Main_listenPort = 0; // Added
    memset(Main_strMaster, 0, sizeof(Main_strMaster)); // Added

    stdPlatform_Printf("OpenJKDF2: %s done\n", __func__);

#ifndef QOL_IMPROVEMENTS
    exit(0);
#endif
}

// Inlined?
void Main_ShowHelp()
{
    pHS->messagePrint("\n", 0, 0, 0, 0);
    pHS->messagePrint(
        "Dark Forces II: Jedi Knight v%d.%02d%c\n",
        jkGuiTitle_verMajor,
        jkGuiTitle_verMinor,
        jkGuiTitle_verRevision,
        0);
    pHS->messagePrint("(c) 1997 Lucasfilm Ltd. and LucasArts Entertainment Company. All Rights Reserved.");
    pHS->messagePrint("Built %s %s\n", "Sep 10 1997", "09:39:21");
    pHS->messagePrint("\n");
    pHS->messagePrint("\n");
    jk_exit(3);
}

void Main_ParseCmdLine(char *cmdline)
{
    for (char* pArgTok = _strtok(cmdline, " \t"); pArgTok; pArgTok = _strtok(0, " \t"))
    {
        if ( !__strcmpi(pArgTok, "-path") || !__strcmpi(pArgTok, "/path") )
        {
            char* pArgNext = _strtok(0, " \t");
            stdString_SafeStrCopy(Main_path, pArgNext, 0x80);
        }
        else if ( !__strcmpi(pArgTok, "-fail") || !__strcmpi(pArgTok, "/fail") ) // MOTS added
        {
            //Main_failLogFp = fopen("fail.log", "w");
        }
        else if ( !__strcmpi(pArgTok, "-devMode") || !__strcmpi(pArgTok, "devMode") )
        {
            Main_bDevMode = 1;
            Main_bDisplayConfig = 1;
        }
        else if (!__strcmpi(pArgTok, "-dispStats") || !__strcmpi(pArgTok, "/dispStats") )
        {
            Main_bDispStats = 1;
        }
        else if (!__strcmpi(pArgTok, "-frameRate") || !__strcmpi(pArgTok, "/frameRate") )
        {
            Main_bFrameRate = 1;
        }
        else if (!__strcmpi(pArgTok, "-windowGUI") || !__strcmpi(pArgTok, "/windowGUI") )
        {
            Main_bWindowGUI = 1;
        }
        else if ( !__strcmpi(pArgTok, "-displayConfig") || !__strcmpi(pArgTok, "/displayConfig") )
        {
            Main_bDisplayConfig = 1;
        }
        else if ( !__strcmpi(pArgTok, "-?") || !__strcmpi(pArgTok, "/?") )
        {
            Main_ShowHelp();
        }
        else if (!__strcmpi(pArgTok, "-debug") || !__strcmpi(pArgTok, "/debug") )
        {
            char* pArgNext = _strtok(0, " \t");
            if (!__strcmpi(pArgNext, "con") )
            {
                Main_logLevel = 1;
            }
            else if (!__strcmpi(pArgNext, "log") )
            {
                Main_logLevel = 2;
            }
            else if (!__strcmpi(pArgNext, "none") )
            {
                Main_logLevel = 0;
            }
            else
            {
                Main_ShowHelp();
            }
        }
        else if (!__strcmpi(pArgTok, "-verbose") || !__strcmpi(pArgTok, "/verbose") )
        {
            char* pArgNext = _strtok(0, " \t");
            if ( _atoi(pArgNext) < 0 )
            {
                Main_verboseLevel = 0;
            }
            else if ( _atoi(pArgNext) > 2 )
            {
                Main_verboseLevel = 2;
            }
            else
            {
                Main_verboseLevel = _atoi(pArgNext);
            }
        }
        else if (!__strcmpi(pArgTok, "-noHUD") || !__strcmpi(pArgTok, "/noHUD") )
        {
            Main_bNoHUD = 1;
        }
        else if ( !__strcmpi(pArgTok, "-record") || !__strcmpi(pArgTok, "/record") ) // MOTS added
        {
            //sithTime_idk_record(0x53,0x53);
            //Main_bRecord = 1;
        }
        else if ( !__strcmpi(pArgTok, "-fixed") || !__strcmpi(pArgTok, "/fixed") ) // MOTS added
        {
            //sithTime_idk_record(0x53,0x53);
        }
        else if ( !__strcmpi(pArgTok, "-coglog") || !__strcmpi(pArgTok, "/coglog") ) // MOTS added
        {
            //Main_cogLogFp = fopen("cog.log", "wc");
        }
#ifdef QOL_IMPROVEMENTS
        else if (!__strcmpi(pArgTok, "-dedicatedServer") || !__strcmpi(pArgTok, "/dedicatedServer") )
        {
            Main_bDedicatedServer = 1;
        }
        else if (!__strcmpi(pArgTok, "-autostart") || !__strcmpi(pArgTok, "/autostart") )
        {
            Main_bAutostart = 1;
        }
        else if (!__strcmpi(pArgTok, "-sp") || !__strcmpi(pArgTok, "/sp") || !__strcmpi(pArgTok, "-singleplayer") || !__strcmpi(pArgTok, "/singleplayer"))
        {
            Main_bAutostartSp = 1;
        }
        else if (!__strcmpi(pArgTok, "-mp") || !__strcmpi(pArgTok, "/mp") || !__strcmpi(pArgTok, "-multiplayer") || !__strcmpi(pArgTok, "/multiplayer"))
        {
            Main_bAutostartSp = 0;
        }
        else if (!__strcmpi(pArgTok, "-episode") || !__strcmpi(pArgTok, "/episode") )
        {
            char* pArgNext = _strtok(0, " \t");
            stdString_SafeStrCopy(Main_strEpisode, pArgNext, 0x80);
        }
        else if (!__strcmpi(pArgTok, "-map") || !__strcmpi(pArgTok, "/map") )
        {
            char* pArgNext = _strtok(0, " \t");
            stdString_SafeStrCopy(Main_strMap, pArgNext, 0x80);
        }
        else if (!__strcmpi(pArgTok, "-character"))
        {
            char* name = _strtok(0, " \t");
            if (Main_strAutostartCharacter[0] || !Autostart_CharacterNameValid(name)) {
                fprintf(stderr, "autostart: invalid or repeated multiplayer character name\n");
                exit(6);
            }
            stdString_SafeStrCopy(Main_strAutostartCharacter, name, sizeof(Main_strAutostartCharacter));
        }
        // Added: the log file, started by stdLog_StartFromArgs in main() before this runs
        else if (!__strcmpi(pArgTok, "-log") || !__strcmpi(pArgTok, "/log")
                 || !__strcmpi(pArgTok, "-noLog") || !__strcmpi(pArgTok, "/noLog"))
        {
        }
        else if (!__strcmpi(pArgTok, "-logFile") || !__strcmpi(pArgTok, "/logFile"))
        {
            _strtok(0, " \t");
        }
        // Added: a dedicated server's settings file instead of server.json, so several
        // servers can run from one folder
        else if (!__strcmpi(pArgTok, "-serverConfig") || !__strcmpi(pArgTok, "/serverConfig"))
        {
            char* pArgNext = _strtok(0, " \t");
            if (pArgNext) stdString_SafeStrCopy(Main_strServerConfig, pArgNext, sizeof(Main_strServerConfig));
        }
        // Added: the connection a dedicated server hosts on, and its server
        else if (!__strcmpi(pArgTok, "-connection") || !__strcmpi(pArgTok, "/connection"))
        {
            char* pArgNext = _strtok(0, " \t");
            if (pArgNext) stdString_SafeStrCopy(Main_strConnection, pArgNext, sizeof(Main_strConnection));
        }
        else if (!__strcmpi(pArgTok, "-relayServer") || !__strcmpi(pArgTok, "/relayServer")
                 || !__strcmpi(pArgTok, "-webLobby") || !__strcmpi(pArgTok, "/webLobby"))
        {
            char* pArgNext = _strtok(0, " \t");
            if (pArgNext) stdString_SafeStrCopy(Main_strNetServer, pArgNext, sizeof(Main_strNetServer));
            if (!Main_strConnection[0])
                stdString_SafeStrCopy(Main_strConnection, __strcmpi(pArgTok + 1, "webLobby") ? "relay" : "web", sizeof(Main_strConnection));
        }
        else if (!__strcmpi(pArgTok, "-master") || !__strcmpi(pArgTok, "/master"))
        {
            char* pArgNext = _strtok(0, " \t");
            if (pArgNext) stdString_SafeStrCopy(Main_strMaster, pArgNext, sizeof(Main_strMaster));
        }
        else if (!__strcmpi(pArgTok, "-wsPort") || !__strcmpi(pArgTok, "/wsPort"))
        {
            char* pArgNext = _strtok(0, " \t");
            if (pArgNext) Main_wsPort = atoi(pArgNext);
            if (Main_wsPort <= 0 || Main_wsPort > 65535) Main_wsPort = 0;
        }
        else if (!__strcmpi(pArgTok, "-port") || !__strcmpi(pArgTok, "/port"))
        {
            char* pArgNext = _strtok(0, " \t");
            if (pArgNext) Main_listenPort = atoi(pArgNext);
            if (Main_listenPort <= 0 || Main_listenPort > 65535) Main_listenPort = 0;
        }
        else if (!__strcmpi(pArgTok, "-headless") || !__strcmpi(pArgTok, "/headless") )
        {
            Main_bHeadless = 1;
        }
        else if (!__strcmpi(pArgTok, "-verboseNetworking") || !__strcmpi(pArgTok, "/verboseNetworking") )
        {
            Main_bVerboseNetworking = 1;
        }
        else if (!__strcmpi(pArgTok, "-motsCompat") || !__strcmpi(pArgTok, "/motsCompat"))
        {
            Main_bMotsCompat = 1;
        }
        else if (!__strcmpi(pArgTok, "-enhancedCogVerbs") || !__strcmpi(pArgTok, "/enhancedCogVerbs"))
        {
            Main_bEnhancedCogVerbs = 1;
        }
        else if (!__strcmpi(pArgTok, "-dwCompat")
                 || !__strcmpi(pArgTok, "/dwCompat")
                 || !__strcmpi(pArgTok, "-droidworksCompat")
                 || !__strcmpi(pArgTok, "/droidworksCompat"))
        {
            Main_bDwCompat = 1;
        }
#ifdef PLATFORM_DROIDWORKS
        else if (!__strcmpi(pArgTok, "-droidworks") || !__strcmpi(pArgTok, "/droidworks"))
        {
            // Full DroidWorks game mode: DW app layer replaces the jk GUI flow.
            Main_bDroidWorks = 1;
            Main_bDwCompat = 1;
        }
#endif
        else if (!__strcmpi(pArgTok, "-dwCogVerbs") || !__strcmpi(pArgTok, "/dwCogVerbs"))
        {
            // DW COG verb import into JK/MOTS games (analog of -enhancedCogVerbs).
            Main_bDwCogVerbs = 1;
        }
#endif
        else
        {
            pHS->errorPrint("Error in arguments.\n", 0, 0, 0, 0);
            Main_ShowHelp();
        }
    }
#ifdef QOL_IMPROVEMENTS
    if (Main_strAutostartCharacter[0] && (!Main_bAutostart || Main_bAutostartSp)) {
        fprintf(stderr, "autostart: -character requires -autostart multiplayer mode\n");
        exit(6);
    }
#endif
}

int Main_FPrintf(const char* fmt, ...) {
    va_list aArgs;
    va_start (aArgs, fmt);
    int ret = __vsnprintf(std_g_genBuffer, 0x400, fmt, aArgs);
    va_end (aArgs);

    fputs(std_g_genBuffer, (FILE*)debug_log_fp);
    fflush((FILE*)debug_log_fp);

    return ret;
}
