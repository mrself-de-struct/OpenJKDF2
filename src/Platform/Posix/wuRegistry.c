#include "Platform/wuRegistry.h"

#include "General/stdJSON.h"

#include "jk.h"
#include <stdio.h>
#include <string.h>

// Added: the keys a dedicated server reads from server.json (wuRegistry_UseServerFile).
static const char* wuRegistry_aServerKeys[] = {
    // the host screen's (jkGuiNetHost_SaveSettings) and the game being hosted
    "gameName", "serverPassword", "serverEpisodeGob", "serverMapJkl", "maxPlayers", "maxRank",
    "sessionFlags", "gameFlags", "timeLimit", "scoreLimit", "tickRate", "portNum", "bIsDedicated",
    "bIsCoop", "bIsEpisodeCoop", "bUseScoreLimit", "bUseTimeLimit", "bIsSingleLevel", "bIsTeams",
    // where it hosts (Main_StartupDedicated; the command line wins)
    "connection", "relayServer", "webLobbyUrl", "port", "wsPort", "master", "maps", "shuffleMaps",
    // admin (Main/jkServer.c)
    "rconPassword", "bans",
};
static int wuRegistry_bServerFile = 0;

static const char* wuRegistry_File(LPCSTR lpValueName)
{
    if (wuRegistry_bServerFile && lpValueName)
        for (size_t i = 0; i < sizeof(wuRegistry_aServerKeys) / sizeof(wuRegistry_aServerKeys[0]); i++)
            if (!strcmp(lpValueName, wuRegistry_aServerKeys[i]))
                return SERVER_FNAME;
    return REGISTRY_FNAME;
}

void wuRegistry_UseServerFile(void)
{
    FILE* f = fopen(SERVER_FNAME, "r");   // (before the game's file system is up)
    int bNew = !f;
    if (f) fclose(f);
    wuRegistry_bServerFile = 1;
    // First run: move what this folder's registry.json has.
    for (size_t i = 0; bNew && i < sizeof(wuRegistry_aServerKeys) / sizeof(wuRegistry_aServerKeys[0]); i++)
        stdJSON_MoveKey(REGISTRY_FNAME, SERVER_FNAME, wuRegistry_aServerKeys[i]);
    // Every run: show the keys added since, with their defaults.
    if (!stdJSON_HasKey(SERVER_FNAME, "maps")) { char tmp[1]; stdJSON_GetStringList(SERVER_FNAME, "maps", tmp, 0, 1); }
    if (!stdJSON_HasKey(SERVER_FNAME, "shuffleMaps")) stdJSON_SaveBool(SERVER_FNAME, "shuffleMaps", 0);
    if (!stdJSON_HasKey(SERVER_FNAME, "rconPassword")) stdJSON_SetString(SERVER_FNAME, "rconPassword", "");
    if (!stdJSON_HasKey(SERVER_FNAME, "bans")) stdJSON_SetStringList(SERVER_FNAME, "bans", "");
    if (!stdJSON_HasKey(SERVER_FNAME, "connection")) stdJSON_SetString(SERVER_FNAME, "connection", "");
    if (!stdJSON_HasKey(SERVER_FNAME, "relayServer")) stdJSON_SetString(SERVER_FNAME, "relayServer", "");
    if (!stdJSON_HasKey(SERVER_FNAME, "port")) stdJSON_SaveInt(SERVER_FNAME, "port", 27960); // JKR_DEFAULT_SERVER_PORT
    if (!stdJSON_HasKey(SERVER_FNAME, "master")) stdJSON_SetString(SERVER_FNAME, "master", "");
    if (!stdJSON_HasKey(SERVER_FNAME, "wsPort")) stdJSON_SaveInt(SERVER_FNAME, "wsPort", 0); // browsers (0 = none)
}

LSTATUS wuRegistry_Startup(HKEY hKey, LPCSTR lpSubKey, BYTE *lpData)
{
    wuRegistry_bStarted = 1;
    return 0;
}

void wuRegistry_Shutdown()
{
    wuRegistry_bStarted = 0;
}

int wuRegistry_SaveInt(LPCSTR lpValueName, int val)
{
    stdJSON_SaveInt(wuRegistry_File(lpValueName), lpValueName, val);
    return 1;
}

int wuRegistry_SaveFloat(LPCSTR lpValueName, flex_t val)
{
    return stdJSON_SaveFloat(wuRegistry_File(lpValueName), lpValueName, val);
}

int wuRegistry_GetInt(LPCSTR lpValueName, int defaultVal)
{
    return stdJSON_GetInt(wuRegistry_File(lpValueName), lpValueName, defaultVal);
}

flex_t wuRegistry_GetFloat(LPCSTR lpValueName, flex_t defaultVal)
{
    return stdJSON_GetFloat(wuRegistry_File(lpValueName), lpValueName, defaultVal);
}

int wuRegistry_SaveBool(LPCSTR lpValueName, int bVal)
{
    return stdJSON_SaveBool(wuRegistry_File(lpValueName), lpValueName, bVal);
}

int wuRegistry_GetBool(LPCSTR lpValueName, int bDefaultVal)
{
    return stdJSON_GetBool(wuRegistry_File(lpValueName), lpValueName, bDefaultVal);
}

int wuRegistry_SaveBytes(LPCSTR lpValueName, BYTE *lpData, DWORD cbData)
{
    return stdJSON_SaveBytes(wuRegistry_File(lpValueName), lpValueName, lpData, cbData);
}

int wuRegistry_GetBytes(LPCSTR lpValueName, BYTE *lpData, DWORD cbData)
{
    return stdJSON_GetBytes(wuRegistry_File(lpValueName), lpValueName, lpData, cbData);
}

int wuRegistry_SaveStr(LPCSTR lpValueName, const char *lpData)
{
    return stdJSON_SetString(wuRegistry_File(lpValueName), lpValueName, lpData);
}

int wuRegistry_GetStr(LPCSTR lpValueName, char* lpData, int outSize, const char *outDefault)
{
    return stdJSON_GetString(wuRegistry_File(lpValueName), lpValueName, lpData, outSize, outDefault);
}

int wuRegistry_SetWString(LPCSTR lpValueName, const char16_t *lpData)
{
    return stdJSON_SetWString(wuRegistry_File(lpValueName), (const char*)lpValueName, lpData);
}

int wuRegistry_GetWString(LPCSTR lpValueName, char16_t* lpData, int outSize, const char16_t *outDefault)
{
    return stdJSON_GetWString(wuRegistry_File(lpValueName), (const char*)lpValueName, lpData, outSize, outDefault);
}