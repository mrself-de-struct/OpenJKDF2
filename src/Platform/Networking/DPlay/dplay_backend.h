#ifndef _PLATFORM_NETWORKING_DPLAY_BACKEND_H
#define _PLATFORM_NETWORKING_DPLAY_BACKEND_H

// The game talks to dplay_shim.h's dps_* API. dplay_dispatch.c implements it
// by forwarding to one of several transports, chosen by the connection the
// player picks in the multiplayer menu:
//   - DirectPlay (Microsoft's via COM on Windows, native sockets elsewhere):
//     compatible with the original JK.EXE; needs reachable ports.
//   - Imperial Network Protocol relay: OpenJKDF2 only, no port forwarding.
//
// A transport's source file defines DPS_BACKEND_PREFIX before including
// dplay_shim.h, which renames its dps_* functions to <prefix>_*; it then
// exports a dps_backend table.

#include "dplay_shim.h"

typedef struct dps_backend
{
    const char* name;
    long (*Startup)(const uint8_t appGuid[16]);
    void (*Shutdown)(void);
    long (*Recreate)(void);
    long (*EnumConnections)(dps_connection_cb cb, void* ctx);
    long (*InitializeConnection)(const void* connection);
    long (*EnumSessions)(dps_session_cb cb, void* ctx);
    void (*SetEnumHost)(const char* host);
    long (*StopEnumSessions)(void);
    long (*OpenCreate)(const dps_session* desc);
    long (*OpenJoin)(const uint8_t guidInstance[16], const uint16_t* password);
    long (*Close)(void);
    long (*GetSessionDesc)(dps_session* out);
    long (*SetSessionDesc)(const dps_session* in);
    long (*CreatePlayer)(const uint16_t* shortName, uint32_t* idOut);
    long (*DestroyPlayer)(uint32_t id);
    long (*EnumPlayers)(dps_player_cb cb, void* ctx);
    long (*Send)(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size);
    int  (*Receive)(uint32_t* fromOut, const void** dataOut, uint32_t* sizeOut, dps_sysmsg* sysOut);
} dps_backend;

#define DPS_BACKEND_TABLE(prefix) { #prefix, \
    prefix##_Startup, prefix##_Shutdown, prefix##_Recreate, prefix##_EnumConnections, \
    prefix##_InitializeConnection, prefix##_EnumSessions, prefix##_SetEnumHost, \
    prefix##_StopEnumSessions, prefix##_OpenCreate, prefix##_OpenJoin, prefix##_Close, \
    prefix##_GetSessionDesc, prefix##_SetSessionDesc, prefix##_CreatePlayer, \
    prefix##_DestroyPlayer, prefix##_EnumPlayers, prefix##_Send, prefix##_Receive }

extern const dps_backend dpcom_backend;     // dplay_shim.c   (Windows)
extern const dps_backend dpnative_backend;  // dplay_native.c (elsewhere)
extern const dps_backend jkrelay_backend;   // jkrelay_client.c
extern const dps_backend jkweb_backend;     // jkweb_client.c (needs libcurl)

#endif
