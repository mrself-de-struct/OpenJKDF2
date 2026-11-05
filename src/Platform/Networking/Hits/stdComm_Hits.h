#ifndef _PLATFORM_NETWORKING_HITS_STDCOMM_HITS_H
#define _PLATFORM_NETWORKING_HITS_STDCOMM_HITS_H

// Hit what you see on the desktop (stdComm_Hits.c): the browser build's hit
// rules (stdComm_WSRelayHits.c, API in stdComm_WSRelay.h) with what its
// transport gave them.

#include "stdComm_WSRelay.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(PLATFORM_DPLAY)

// The game's messages use OpenJKDF2's layouts (stdComm_dplay.c): the only games
// the rules play in, and the only ones the extra fields they add go to.
extern int stdComm_bOpenJkdf2Wire;

// A game whose host plays Hit what you see, in its description's flags (the
// Imperial Network Protocol's list). Older games ignore it.
#define STDCOMM_HITS_SESSION_HWYS 0x10000000u

// sv_hitRules (0 Classic, 1 Hit what you see) and sv_hitReach (ms): what this
// game plays by when it hosts.
void stdComm_Hits_StartupCvars(void);
int stdComm_Hits_HostRules(int* pReachMs);

// Each frame's end (sithMain).
void stdComm_Hits_Frame(void);

#endif // PLATFORM_DPLAY

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_HITS_STDCOMM_HITS_H
