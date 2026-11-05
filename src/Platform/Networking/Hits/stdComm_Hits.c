// Hit what you see on the desktop (2026-10-29): what the browser build's
// transport (tristamus' fork, stdComm_WSRelay.c) gives its hit rules
// (stdComm_WSRelayHits.c, copied here unchanged but for the dedicated host's
// hello and the resends): whether this game is in a match, each saber swing's
// number, a clock, and the numbers of position updates (stdComm_dplay.c).
// The browser's hit ledger (netstats.js) isn't here; with -verboseNetworking
// claims turned down are logged.
//
// Only in games with OpenJKDF2's message layouts (the Imperial Network
// Protocol and the web lobby), never with JK.EXE. The host picks the rules
// (sv_hitRules, sv_hitReach) and its hello tells the other games; a game that
// doesn't say hello (JK.EXE, an older OpenJKDF2) plays Classic with everyone.

#include "stdComm_Hits.h"

#if defined(PLATFORM_DPLAY)

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Devices/sithComm.h"
#include "Gameplay/sithPlayer.h"
#include "World/sithThing.h"
#include "World/sithWorld.h"
#include "Main/Main.h"
#include "Main/sithCvar.h"
#include "Platform/Networking/DPlay/stdComm_dplay.h"
#include "stdPlatform.h"
#include "jk.h"

#include <math.h>

int stdComm_WSRelay_fireGuid = 0;

static uint32_t hits_aSwing[JKPLAYER_NUM_INFOS];
static uint32_t hits_lastSwing = 0;
static double hits_ownSwingAt = 0.0;   // when this game's player last swung, on the motion clock

// The host's choice (cvars, so rcon can set them): 0 Classic, 1 Hit what you see.
static int hits_cvarRules = STDCOMM_WSRELAY_HITS_CLASSIC;
static int hits_cvarReach = STDCOMM_WSRELAY_REACH_DEFAULT;

void stdComm_Hits_StartupCvars(void)
{
    sithCvar_RegisterInt("sv_hitRules", STDCOMM_WSRELAY_HITS_CLASSIC, &hits_cvarRules, CVARFLAG_GLOBAL);
    sithCvar_RegisterInt("sv_hitReach", STDCOMM_WSRELAY_REACH_DEFAULT, &hits_cvarReach, CVARFLAG_GLOBAL);
}

// The rules this game's host plays by, from the cvars.
int stdComm_Hits_HostRules(int* pReachMs)
{
    if (pReachMs)
        *pReachMs = stdComm_WSRelay_ClampReach(hits_cvarReach);
    return hits_cvarRules == STDCOMM_WSRELAY_HITS_HWYS ? STDCOMM_WSRELAY_HITS_HWYS : STDCOMM_WSRELAY_HITS_CLASSIC;
}

// Each frame's end (sithMain): a host plays by its cvars (rcon can change
// them, and its next hello says so), then the rules' own work.
void stdComm_Hits_Frame(void)
{
    static int lastRules = -1, lastReach = -1;
    int reachMs = -1, rules;
    if (stdComm_bIsServer && stdComm_bOpenJkdf2Wire)
    {
        rules = stdComm_Hits_HostRules(&reachMs);
        stdComm_WSRelay_SetHitRules(rules, reachMs);
    }
    stdComm_WSRelay_HitsFrame();

    // Logged when they change: the host's cvars, or the host's hello here.
    rules = stdComm_WSRelay_InMatch() ? stdComm_WSRelay_GetHitRules(&reachMs) : -1;
    if (rules >= 0 && (rules != lastRules || reachMs != lastReach))
    {
        if (rules == STDCOMM_WSRELAY_HITS_HWYS)
            stdPlatform_Printf("OpenJKDF2: hit rules: Hit what you see (reach %d ms)\n", reachMs);
        else
            stdPlatform_Printf("OpenJKDF2: hit rules: Classic\n");
    }
    lastRules = rules;
    lastReach = reachMs;
}

int stdComm_WSRelay_InMatch()
{
    if (!stdComm_bOpenJkdf2Wire || !sithNet_isMulti || !stdComm_dword_8321E0 || !sithWorld_g_pCurrentWorld || !sithPlayer_g_pLocalPlayerThing)
        return 0;

    // A joiner is in once the host has welcomed it into the level
    // (sithMulti_ProcessWelcome clears this).
    return stdComm_bIsServer || !(g_submodeFlags & 8);
}

int stdComm_WSRelay_RecvSeq(uint32_t* pSeq)
{
    return stdComm_DPlay_RecvSeq(pSeq);
}

double stdComm_WSRelay_SeqSentAt(int toId, uint32_t seq)
{
    return stdComm_DPlay_SeqSentAt(toId, seq);
}

double stdComm_WSRelay_Now()
{
    return stdComm_DPlay_Now();
}

int stdComm_WSRelay_HitsResend()
{
    return stdComm_DPlay_HitsResend();
}

static int hits_PlayerSlot(SithThing* pPlayer)
{
    if (!pPlayer || pPlayer->type != SITH_THING_PLAYER || !pPlayer->actorParams.pPlayer)
        return -1;
    intptr_t slot = pPlayer->actorParams.pPlayer - jkPlayer_playerInfos;
    return (slot >= 0 && slot < JKPLAYER_NUM_INFOS) ? (int)slot : -1;
}

void stdComm_WSRelay_NewSwing(SithThing* pPlayer)
{
    int slot = hits_PlayerSlot(pPlayer);
    if (slot < 0)
        return;
    // Never 0, which stands for a swing without a number.
    if (!++hits_lastSwing)
        hits_lastSwing = 1;
    hits_aSwing[slot] = hits_lastSwing;
    if (pPlayer == sithPlayer_g_pLocalPlayerThing)
        hits_ownSwingAt = stdComm_WSRelay_MotionNow();
}

double stdComm_WSRelay_OwnSwingAt()
{
    return hits_ownSwingAt;
}

void stdComm_WSRelay_SetSwing(SithThing* pPlayer, uint32_t swing)
{
    int slot = hits_PlayerSlot(pPlayer);
    if (slot >= 0)
        hits_aSwing[slot] = swing;
}

uint32_t stdComm_WSRelay_GetSwing(SithThing* pPlayer)
{
    int slot = hits_PlayerSlot(pPlayer);
    return slot >= 0 ? hits_aSwing[slot] : 0;
}

int stdComm_WSRelay_HitKind(SithThing* pSource, int damageType)
{
    if (damageType & SITH_DAMAGE_SABER)
        return STDCOMM_WSRELAY_KIND_SABER;
    if (pSource->type == SITH_THING_EXPLOSION)
        return STDCOMM_WSRELAY_KIND_SPLASH;
    if (damageType & SITH_DAMAGE_FORCE)
        return STDCOMM_WSRELAY_KIND_FORCE;
    if (pSource->type == SITH_THING_WEAPON)
        return STDCOMM_WSRELAY_KIND_SHOT;
    return STDCOMM_WSRELAY_KIND_OTHER;
}

// The browser's hit ledger: nothing to tell here.
void stdComm_WSRelay_NoteHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType, int outcome)
{
}

void stdComm_WSRelay_NoteHitBy(SithThing* pVictim, SithThing* pAttacker, SithThing* pSource, flex_t damage, int damageType, int outcome)
{
}

void stdComm_WSRelay_NoteTurnedDown(int otherId, int kind, int outcome, flex_t damage, uint32_t attack, int reason, float distance, int moment, float ageMs)
{
    if (Main_bVerboseNetworking)
        stdPlatform_Printf("OpenJKDF2: hit claim from %x %s (kind %d, %.1f damage, reason %d, %.2f JKU off, moment %d, %.0f ms back)\n", otherId,
                           outcome == STDCOMM_WSRELAY_DEAD ? "came while dead" : "turned down", kind, (double)damage, reason, (double)distance,
                           moment, (double)ageMs);
}

void stdComm_WSRelay_NoteTargeted(int otherId, int outcome, int reason, float distance, int moment, float ageMs)
{
    if (Main_bVerboseNetworking && outcome != STDCOMM_WSRELAY_HIT)
        stdPlatform_Printf("OpenJKDF2: targeted Force from %x turned down (reason %d, %.2f JKU off, %.0f ms back)\n", otherId,
                           reason, (double)distance, (double)ageMs);
}

// The browser's ?netstats panel: nothing to show here.
void stdComm_WSRelay_NoteMotion(int otherId, double bufferMs, uint32_t steps, uint32_t carried, uint32_t held,
                                uint32_t jumps, uint32_t warps, uint32_t restarts)
{
}

// Players' clocks, for another game's shots to start as far along their path as
// they really are (stdComm_WSRelay_ShotCatchUp): pinged on the Imperial Network
// Protocol as the browser's netstats.js does (jkrelay_client.c), unknown on the web
// lobby. Claims name moments on the target's own clock, so hits don't need them.
void stdComm_WSRelay_SyncClock(int netId, int bOn)
{
    // Asked for while this game draws that player from their timed updates.
    if (Main_bVerboseNetworking)
        stdPlatform_Printf("OpenJKDF2: player %x %s timed updates\n", netId, bOn ? "drawn from" : "no longer drawn from");
    stdComm_DPlay_SyncClock(netId, bOn);
}

double stdComm_WSRelay_ClockOffset(int netId)
{
    return stdComm_DPlay_ClockOffset(netId);
}

#endif // PLATFORM_DPLAY
