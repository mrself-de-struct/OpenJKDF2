// DirectPlay COM shim for OpenJKDF2's vanilla-compatible networking backend.
//
// Everything that touches <dplay.h> lives in this file. The rest of the
// backend (stdComm_dplay.c) talks to it through dplay_shim.h using plain
// stdint types, so OpenJKDF2's own DPID/DPNAME/GUID typedefs never collide
// with the real Windows ones.
//
// Interface choice: JK.EXE 1.0 uses IDirectPlay3 (Unicode) -- player names in
// its enum callbacks are wide strings. DirectPlay's wire protocol is the same
// for IDirectPlay3/3A/4/4A, so this choice only affects string handling.

#if defined(PLATFORM_DPLAY) && !defined(DPLAY_NATIVE)

#include <windows.h>
#include <objbase.h>
#include <initguid.h>   // instantiate CLSID_DirectPlay / IID_IDirectPlay3 in this TU
#include <dplay.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DPS_BACKEND_PREFIX dpcom
#include "dplay_backend.h"

static LPDIRECTPLAY3 dps_pDP = NULL;
static GUID dps_appGuid;
static int dps_bComInitted = 0;

static uint8_t* dps_pRecvBuf = NULL;
static DWORD dps_recvBufSize = 0;

#define DPS_LOG(...) do { fprintf(stderr, "[dplay] " __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void dps_CopyW(uint16_t* dst, size_t dstLen, const WCHAR* src)
{
    size_t i = 0;
    if (!dstLen) return;
    if (src)
    {
        for (; i + 1 < dstLen && src[i]; i++)
            dst[i] = (uint16_t)src[i];
    }
    dst[i] = 0;
}

static void dps_DescToNeutral(const DPSESSIONDESC2* sd, dps_session* out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->guidInstance, &sd->guidInstance, 16);
    out->flags      = sd->dwFlags;
    out->maxPlayers = sd->dwMaxPlayers;
    out->curPlayers = sd->dwCurrentPlayers;
    dps_CopyW(out->name, DPS_NAME_MAX, sd->lpszSessionName);
    dps_CopyW(out->password, DPS_PASSWORD_MAX, sd->lpszPassword);
    out->user[0] = sd->dwUser1;
    out->user[1] = sd->dwUser2;
    out->user[2] = sd->dwUser3;
    out->user[3] = sd->dwUser4;
}

// Fills *sd from *in. String pointers reference the caller's dps_session, so
// *in must outlive the DirectPlay call.
static void dps_NeutralToDesc(const dps_session* in, DPSESSIONDESC2* sd)
{
    memset(sd, 0, sizeof(*sd));
    sd->dwSize = sizeof(*sd);
    sd->dwFlags = in->flags;
    memcpy(&sd->guidInstance, in->guidInstance, 16);
    sd->guidApplication = dps_appGuid;
    sd->dwMaxPlayers = in->maxPlayers;
    sd->lpszSessionName = (LPWSTR)in->name;
    sd->lpszPassword = in->password[0] ? (LPWSTR)in->password : NULL;
    sd->dwUser1 = in->user[0];
    sd->dwUser2 = in->user[1];
    sd->dwUser3 = in->user[2];
    sd->dwUser4 = in->user[3];
}

static long dps_Create(void)
{
    HRESULT hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IDirectPlay3, (LPVOID*)&dps_pDP);
    if (FAILED(hr))
    {
        dps_pDP = NULL;
        DPS_LOG("CoCreateInstance(CLSID_DirectPlay) failed: 0x%08lx. "
                "Is DirectPlay installed (Windows Features > Legacy Components, "
                "or Wine/winetricks directplay)?\n", (unsigned long)hr);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

long dps_Startup(const uint8_t appGuid[16])
{
    HRESULT hr;

    memcpy(&dps_appGuid, appGuid, 16);

    if (!dps_bComInitted)
    {
        hr = CoInitialize(NULL);
        // S_FALSE means COM was already initialized on this thread; still balanced by CoUninitialize.
        if (SUCCEEDED(hr))
            dps_bComInitted = 1;
        else if (hr != RPC_E_CHANGED_MODE)
            return hr;
    }

    if (dps_pDP)
        return DP_OK;
    return dps_Create();
}

void dps_Shutdown(void)
{
    if (dps_pDP)
    {
        IDirectPlay3_Close(dps_pDP);
        IDirectPlay3_Release(dps_pDP);
        dps_pDP = NULL;
    }
    free(dps_pRecvBuf);
    dps_pRecvBuf = NULL;
    dps_recvBufSize = 0;

    if (dps_bComInitted)
    {
        CoUninitialize();
        dps_bComInitted = 0;
    }
}

long dps_Recreate(void)
{
    if (dps_pDP)
    {
        IDirectPlay3_Close(dps_pDP);
        IDirectPlay3_Release(dps_pDP);
        dps_pDP = NULL;
    }
    return dps_Create();
}

// ---------------------------------------------------------------------------
// Connections (service providers)
// ---------------------------------------------------------------------------

typedef struct
{
    dps_connection_cb cb;
    void* ctx;
} dps_ConnCtx;

static BOOL CALLBACK dps_EnumConnectionsCallback(LPCGUID lpguidSP, LPVOID lpConnection, DWORD dwConnectionSize,
                                                 LPCDPNAME lpName, DWORD dwFlags, LPVOID lpContext)
{
    dps_ConnCtx* c = (dps_ConnCtx*)lpContext;
    uint16_t name[128];
    (void)dwFlags;

    dps_CopyW(name, 128, lpName ? lpName->lpszShortName : NULL);
    return c->cb(c->ctx, (const uint8_t*)lpguidSP, name, lpConnection, dwConnectionSize) ? TRUE : FALSE;
}

long dps_EnumConnections(dps_connection_cb cb, void* ctx)
{
    dps_ConnCtx c = { cb, ctx };
    if (!dps_pDP) return DPERR_UNINITIALIZED;
    return IDirectPlay3_EnumConnections(dps_pDP, &dps_appGuid, dps_EnumConnectionsCallback, &c, 0);
}

long dps_InitializeConnection(const void* connection)
{
    if (!dps_pDP) return DPERR_UNINITIALIZED;
    return IDirectPlay3_InitializeConnection(dps_pDP, (LPVOID)connection, 0);
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

typedef struct
{
    dps_session_cb cb;
    void* ctx;
} dps_SessCtx;

static BOOL CALLBACK dps_EnumSessionsCallback(LPCDPSESSIONDESC2 lpThisSD, LPDWORD lpdwTimeOut, DWORD dwFlags, LPVOID lpContext)
{
    dps_SessCtx* c = (dps_SessCtx*)lpContext;
    dps_session s;
    (void)lpdwTimeOut;

    // Final call of an enumeration pass; returning FALSE ends it.
    if (dwFlags & DPESC_TIMEDOUT)
        return FALSE;

    dps_DescToNeutral(lpThisSD, &s);
    return c->cb(c->ctx, &s) ? TRUE : FALSE;
}

long dps_EnumSessions(dps_session_cb cb, void* ctx)
{
    DPSESSIONDESC2 sd;
    dps_SessCtx c = { cb, ctx };

    if (!dps_pDP) return DPERR_UNINITIALIZED;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = dps_appGuid;

    // ASYNC: the first call starts a background enumeration (and, for TCP/IP
    // without an address, shows DirectPlay's own "enter host address" dialog,
    // exactly like vanilla JK). Later calls return immediately with the cache.
    return IDirectPlay3_EnumSessions(dps_pDP, &sd, 0, dps_EnumSessionsCallback, &c,
                                     DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_PASSWORDREQUIRED | DPENUMSESSIONS_ASYNC);
}

static BOOL CALLBACK dps_NullSessionsCallback(LPCDPSESSIONDESC2 a, LPDWORD b, DWORD c, LPVOID d)
{
    (void)a; (void)b; (void)c; (void)d;
    return FALSE;
}

void dps_SetEnumHost(const char* host)
{
    // Microsoft DirectPlay's TCP/IP provider asks for the address itself.
    (void)host;
}

long dps_StopEnumSessions(void)
{
    DPSESSIONDESC2 sd;
    if (!dps_pDP) return DPERR_UNINITIALIZED;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = dps_appGuid;
    return IDirectPlay3_EnumSessions(dps_pDP, &sd, 0, dps_NullSessionsCallback, NULL, DPENUMSESSIONS_STOPASYNC);
}

long dps_OpenCreate(const dps_session* desc)
{
    DPSESSIONDESC2 sd;
    if (!dps_pDP) return DPERR_UNINITIALIZED;

    dps_NeutralToDesc(desc, &sd);
    return IDirectPlay3_Open(dps_pDP, &sd, DPOPEN_CREATE);
}

long dps_OpenJoin(const uint8_t guidInstance[16], const uint16_t* password)
{
    DPSESSIONDESC2 sd;
    if (!dps_pDP) return DPERR_UNINITIALIZED;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    memcpy(&sd.guidInstance, guidInstance, 16);
    sd.guidApplication = dps_appGuid;
    sd.lpszPassword = (password && password[0]) ? (LPWSTR)password : NULL;
    return IDirectPlay3_Open(dps_pDP, &sd, DPOPEN_JOIN);
}

long dps_Close(void)
{
    if (!dps_pDP) return DPERR_UNINITIALIZED;
    return IDirectPlay3_Close(dps_pDP);
}

long dps_GetSessionDesc(dps_session* out)
{
    DWORD size = 0;
    HRESULT hr;
    void* buf;

    if (!dps_pDP) return DPERR_UNINITIALIZED;

    hr = IDirectPlay3_GetSessionDesc(dps_pDP, NULL, &size);
    if (hr != DPERR_BUFFERTOOSMALL)
        return hr;

    buf = malloc(size);
    if (!buf) return DPERR_OUTOFMEMORY;

    hr = IDirectPlay3_GetSessionDesc(dps_pDP, buf, &size);
    if (SUCCEEDED(hr))
        dps_DescToNeutral((const DPSESSIONDESC2*)buf, out);
    free(buf);
    return hr;
}

long dps_SetSessionDesc(const dps_session* in)
{
    DPSESSIONDESC2 sd;
    if (!dps_pDP) return DPERR_UNINITIALIZED;

    dps_NeutralToDesc(in, &sd);
    return IDirectPlay3_SetSessionDesc(dps_pDP, &sd, 0);
}

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

long dps_CreatePlayer(const uint16_t* shortName, uint32_t* idOut)
{
    DPNAME name;
    DPID id = 0;
    HRESULT hr;

    if (!dps_pDP) return DPERR_UNINITIALIZED;

    memset(&name, 0, sizeof(name));
    name.dwSize = sizeof(name);
    name.lpszShortName = (LPWSTR)shortName; // vanilla sends only a short name
    name.lpszLongName = NULL;

    hr = IDirectPlay3_CreatePlayer(dps_pDP, &id, &name, NULL, NULL, 0, 0);
    if (SUCCEEDED(hr) && idOut)
        *idOut = (uint32_t)id;
    return hr;
}

long dps_DestroyPlayer(uint32_t id)
{
    if (!dps_pDP) return DPERR_UNINITIALIZED;
    return IDirectPlay3_DestroyPlayer(dps_pDP, (DPID)id);
}

typedef struct
{
    dps_player_cb cb;
    void* ctx;
} dps_PlayerCtx;

static BOOL CALLBACK dps_EnumPlayersCallback(DPID dpId, DWORD dwPlayerType, LPCDPNAME lpName, DWORD dwFlags, LPVOID lpContext)
{
    dps_PlayerCtx* c = (dps_PlayerCtx*)lpContext;
    uint16_t shortName[64];
    uint16_t longName[64];
    (void)dwFlags;

    if (dwPlayerType != DPPLAYERTYPE_PLAYER)
        return TRUE;

    dps_CopyW(shortName, 64, lpName ? lpName->lpszShortName : NULL);
    dps_CopyW(longName, 64, lpName ? lpName->lpszLongName : NULL);
    return c->cb(c->ctx, (uint32_t)dpId,
                 (lpName && lpName->lpszShortName) ? shortName : NULL,
                 (lpName && lpName->lpszLongName) ? longName : NULL) ? TRUE : FALSE;
}

long dps_EnumPlayers(dps_player_cb cb, void* ctx)
{
    dps_PlayerCtx c = { cb, ctx };
    if (!dps_pDP) return DPERR_UNINITIALIZED;
    return IDirectPlay3_EnumPlayers(dps_pDP, NULL, dps_EnumPlayersCallback, &c, DPENUMPLAYERS_ALL);
}

// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

long dps_Send(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size)
{
    if (!dps_pDP) return DPERR_UNINITIALIZED;

    // Flags 0 == unguaranteed. Vanilla JK does its own reliability (sithComm
    // msgId + DSS_RESET acks + 1.7 s resends); in captures of vanilla play all
    // game traffic goes over DirectPlay's UDP path, none over its TCP path.
    return IDirectPlay3_Send(dps_pDP, (DPID)idFrom, (DPID)idTo, 0, (LPVOID)data, size);
}

int dps_Receive(uint32_t* fromOut, const void** dataOut, uint32_t* sizeOut, dps_sysmsg* sysOut)
{
    DPID idFrom = 0, idTo = 0;
    DWORD size;
    HRESULT hr;

    if (!dps_pDP) return DPS_RECV_NONE;

    if (!dps_pRecvBuf)
    {
        dps_recvBufSize = 4096;
        dps_pRecvBuf = (uint8_t*)malloc(dps_recvBufSize);
        if (!dps_pRecvBuf) { dps_recvBufSize = 0; return DPS_RECV_NONE; }
    }

    for (;;)
    {
        size = dps_recvBufSize;
        hr = IDirectPlay3_Receive(dps_pDP, &idFrom, &idTo, DPRECEIVE_ALL, dps_pRecvBuf, &size);
        if (hr == DPERR_BUFFERTOOSMALL)
        {
            uint8_t* grown = (uint8_t*)realloc(dps_pRecvBuf, size);
            if (!grown) return DPS_RECV_NONE;
            dps_pRecvBuf = grown;
            dps_recvBufSize = size;
            continue;
        }
        break;
    }

    if (hr == DPERR_NOMESSAGES)
        return DPS_RECV_NONE;
    if (FAILED(hr))
    {
        DPS_LOG("Receive failed: 0x%08lx\n", (unsigned long)hr);
        return DPS_RECV_NONE;
    }

    if (idFrom != DPID_SYSMSG)
    {
        *fromOut = (uint32_t)idFrom;
        *dataOut = dps_pRecvBuf;
        *sizeOut = (uint32_t)size;
        return DPS_RECV_DATA;
    }

    memset(sysOut, 0, sizeof(*sysOut));
    sysOut->rawType = ((DPMSG_GENERIC*)dps_pRecvBuf)->dwType;
    switch (sysOut->rawType)
    {
        case DPSYS_CREATEPLAYERORGROUP:
        {
            DPMSG_CREATEPLAYERORGROUP* m = (DPMSG_CREATEPLAYERORGROUP*)dps_pRecvBuf;
            if (m->dwPlayerType == DPPLAYERTYPE_PLAYER)
            {
                sysOut->kind = DPS_SYS_PLAYERCREATED;
                sysOut->playerId = (uint32_t)m->dpId;
            }
            break;
        }
        case DPSYS_DESTROYPLAYERORGROUP:
        {
            DPMSG_DESTROYPLAYERORGROUP* m = (DPMSG_DESTROYPLAYERORGROUP*)dps_pRecvBuf;
            if (m->dwPlayerType == DPPLAYERTYPE_PLAYER)
            {
                sysOut->kind = DPS_SYS_PLAYERDESTROYED;
                sysOut->playerId = (uint32_t)m->dpId;
            }
            break;
        }
        case DPSYS_SESSIONLOST:
            sysOut->kind = DPS_SYS_SESSIONLOST;
            break;
        default:
            break;
    }
    return DPS_RECV_SYS;
}

const dps_backend dpcom_backend = DPS_BACKEND_TABLE(dpcom);

#endif // PLATFORM_DPLAY && !DPLAY_NATIVE
