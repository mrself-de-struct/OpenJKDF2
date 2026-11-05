// Hit what you see, smooth and time-stamped motion: in a match played by Hit
// what you see, between games that both have it (hello version 2,
// stdComm_WSRelayHits.c), each player's own position updates carry the time
// they were made, and each game draws the other players from a short buffer
// of those updates (stdComm_WSRelayPlayout.h) instead of moving them along
// their last velocity until the next one comes.
//
//   - The motion clock (stdComm_WSRelay_MotionNow): the time of the physics
//     state this game is at, in ms on stdComm_WSRelay_Now's clock. It moves
//     on by one physics step each step, so the steps a frame runs back to
//     back get the times they stand for, and it's set to the wall clock again
//     when they're more than 2 steps apart (a stalled or hidden tab).
//   - This game's own updates (sithDSSThing_Pos, from sithMulti_CheckPlayers)
//     add, after the head pitch: time u32 (ms on the motion clock), warp u16
//     (one more each time the player comes back to life, is moved further in
//     a step than they could go, the level changes or the clock is set
//     again), 0 u16. Only to the players whose games read them; older games
//     read what they know and stop.
//   - Another player's updates go to their buffer (sithDSSThing_ProcessPos),
//     each taken at the motion clock's time when the frame reads it, which
//     is the time its first step stands for, so the delay covers the time
//     until the next frame reads more. At the start of each physics step
//     the player is drawn at the place the buffer gives for now
//     (stdComm_WSRelayPlayout_Pose), in the sector found through the
//     adjoins from where they were. Their own physics and movement are
//     skipped (sithThing_Update), so nothing steers or blocks them; their
//     walk animation follows the buffer's velocity, and they stand on the
//     floor or leave it as their updates say, landing as they would. Loose
//     things they walk into, a crate say, are pushed as their own move
//     pushes them, so they move here too (motion_Push).
//   - A claim for a hit on a player drawn from the buffer names the moment
//     they were drawn at, on their game's clock, and the updates around it
//     (stdComm_WSRelay_MotionMoment), which their game checks its record
//     against, or, for a moment further back than the reach, checks as a
//     claim without one.
//   - Their swing, animation, sound and saber-on messages carry the time
//     too, and wait until the moment they're drawn at gets there (the
//     timeline), so a swing lines up with the movement it happened with.
//   - Each game measures each such player's clock against its own
//     (netstats.js, the way NTP does), and their fire messages carry the
//     time, so that another game's shot starts as far along its path as it
//     really is (stdComm_WSRelay_ShotCatchUp).
// Classic matches, older games, and players with no timed update yet are
// drawn as before.

#include "stdComm_WSRelay.h"
#include "stdComm_WSRelayPlayout.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Devices/sithComm.h"
#include "Gameplay/sithPlayer.h"
#include "World/sithThing.h"
#include "World/sithSector.h"
#include "World/sithSoundClass.h"
#include "World/sithActor.h"
#include "World/sithWorld.h"
#include "Engine/sithCollision.h"
#include "Engine/sithIntersect.h"
#include "Engine/sithPhysics.h"
#include "Engine/sithPuppet.h"
#include "General/stdMath.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "stdPlatform.h"
#include "jk.h"

#include <math.h>
#include <string.h>

// A player's own update: 52 bytes standing on something, 64 not (with its
// angular velocity), and the time after it.
#define MOTION_POS_ATTACHED (52)
#define MOTION_POS_FREE (64)
#define MOTION_POS_TIMES (8)

// A move longer than this in one physics step is a jump, not running.
#define MOTION_WARP_JKU (1.0)

// How often each player's numbers go to ?netstats.
#define MOTION_STATS_MS (1000.0)

// The furthest along its path another game's shot starts.
#define MOTION_CATCH_UP_MS (250.0)

typedef struct WSRelayMotionPeer
{
    int netId;              // the player this is about; all of it starts
                            // over when another player gets the slot
    WSRelayPlayout playout;
    int bDrawn;             // drawn from the buffer this step
    WSRelayPlayoutPose pose;
    rdVector3 drawnPos;
    uint32_t lastWarp;
    double floorTriedAt;    // when this game last looked for the floor under
                            // them, standing in their game but not here
    WSRelayPlayoutHolds holds;  // their messages waiting for the timeline
    int bSync;              // this game asked for their clock

    // Since the last numbers went to ?netstats.
    double statsAt;
    uint32_t steps;
    uint32_t carried;
    uint32_t held;
    uint32_t jumps;         // moved more than their velocity explains
    uint32_t warps;
} WSRelayMotionPeer;

static WSRelayMotionPeer motion_aPeers[JKPLAYER_NUM_INFOS];

static double motion_now = 0.0;
static double motion_step = 0.0;        // this frame's step on the clock
static int motion_bRunning = 0;
static double motion_frameAt = 0.0;     // the wall clock at the last frame

// This game's own player.
static uint32_t motion_warp = 0;
static rdVector3 motion_lastPos;
static int motion_bLastPos = 0;
static int motion_bWasAlive = 0;
static void* motion_pWorld = NULL;

static WSRelayMotionPeer* motion_Peer(int slot)
{
    if (slot < 0 || slot >= JKPLAYER_NUM_INFOS || slot == (int)playerThingIdx)
        return NULL;
    SithPlayer* pInfo = &jkPlayer_playerInfos[slot];
    int netId = (pInfo->flags & 1) ? (int)pInfo->playerNetId : 0;
    WSRelayMotionPeer* p = &motion_aPeers[slot];
    if (p->netId != netId)
    {
        if (p->bSync)
            stdComm_WSRelay_SyncClock(p->netId, 0);
        memset(p, 0, sizeof(*p));
        p->netId = netId;
    }
    return netId ? p : NULL;
}

static int motion_SlotOf(SithThing* pPlayer)
{
    if (!pPlayer || pPlayer->type != SITH_THING_PLAYER || !pPlayer->actorParams.pPlayer)
        return -1;
    intptr_t slot = pPlayer->actorParams.pPlayer - jkPlayer_playerInfos;
    return (slot >= 0 && slot < JKPLAYER_NUM_INFOS) ? (int)slot : -1;
}

void stdComm_WSRelay_ResetMotion()
{
    for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
    {
        if (motion_aPeers[i].bSync)
            stdComm_WSRelay_SyncClock(motion_aPeers[i].netId, 0);
    }
    memset(motion_aPeers, 0, sizeof(motion_aPeers));
    motion_bRunning = 0;
    motion_warp++;
    motion_bLastPos = 0;
    motion_bWasAlive = 0;
    motion_pWorld = NULL;
}

double stdComm_WSRelay_MotionNow()
{
    return motion_bRunning ? motion_now : stdComm_WSRelay_Now();
}

// ---- The clock, and this game's own player.

void stdComm_WSRelay_MotionFrame(int numSteps)
{
    double wall = stdComm_WSRelay_Now();
    double step = DELTA_PHYSTICK_FPS * 1000.0;
    if (numSteps < 0)
        numSteps = 0;

    // Steps this frame bring the clock to wall, give or take.
    double end = motion_now + numSteps * step;
    motion_step = step;
    if (!motion_bRunning || wall - motion_frameAt > 1000.0 || end < wall - 2.0 * step)
    {
        // Behind (a stall the frame time doesn't make up), or a start: set
        // again, which is a jump.
        if (motion_bRunning)
            motion_warp++;
        motion_now = wall - numSteps * step;
        motion_bRunning = 1;
    }
    else if (end > wall + 2.0 * step)
    {
        // Ahead: half steps until it's back, so it never goes back.
        motion_step = step * 0.5;
    }
    motion_frameAt = wall;
}

// Where this game's player is now, before the step: the record claims are
// checked against, and whether it jumped.
static void motion_Own()
{
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    if (!pLocal)
        return;

    if (motion_pWorld != sithWorld_g_pCurrentWorld)
    {
        // A new level: nobody's updates from the last one apply.
        motion_pWorld = sithWorld_g_pCurrentWorld;
        for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
        {
            stdComm_WSRelayPlayout_Clear(&motion_aPeers[i].playout);
            memset(&motion_aPeers[i].holds, 0, sizeof(motion_aPeers[i].holds));
        }
        motion_warp++;
        motion_bLastPos = 0;
    }

    int bAlive = !(pLocal->flags & (SITH_TF_DEAD|SITH_TF_DESTROYED)) && pLocal->actorParams.health > 0.0;
    if (bAlive && !motion_bWasAlive)
        motion_warp++;
    motion_bWasAlive = bAlive;
    if (motion_bLastPos && rdVector_Dist3(&motion_lastPos, &pLocal->position) > MOTION_WARP_JKU)
        motion_warp++;
    motion_lastPos = pLocal->position;
    motion_bLastPos = 1;

    stdComm_WSRelay_RecordOwn(motion_now);
}

// ---- Sending.

// Whether any other player's game draws this one's from its timed updates.
static int motion_AnyWith()
{
    for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
    {
        if (motion_Peer(i) && stdComm_WSRelay_MotionWith(i))
            return 1;
    }
    return 0;
}

int stdComm_WSRelay_SendOwnPos(SithThing* pThing, int toID, int dpFlags)
{
    if (!pThing || pThing != sithPlayer_g_pLocalPlayerThing || toID != -1 || dpFlags || !motion_bRunning)
        return 0;
    SithMessage* pMsg = &sithComm_netMsgTmp;
    if (pMsg->netMsg.msg_size != MOTION_POS_ATTACHED && pMsg->netMsg.msg_size != MOTION_POS_FREE)
        return 0;

    if (!motion_AnyWith())
        return 0;

    // As stdComm_SendToPlayer sends to everyone, with the time to the games
    // that read it.
    uint32_t size = pMsg->netMsg.msg_size;
    uint8_t* d = (uint8_t*)pMsg->pktData;
    uint32_t t32 = (uint32_t)(int64_t)floor(motion_now + 0.5);
    uint16_t warp = (uint16_t)motion_warp;
    uint16_t zero = 0;
    memcpy(d + size, &t32, 4);
    memcpy(d + size + 4, &warp, 2);
    memcpy(d + size + 6, &zero, 2);
    for (int i = 0; i < (int)jkPlayer_maxPlayers && i < JKPLAYER_NUM_INFOS; i++)
    {
        SithPlayer* pInfo = &jkPlayer_playerInfos[i];
        if (i && (!(pInfo->flags & 1) || (int)pInfo->playerNetId == stdComm_dplayIdSelf))
            continue;
        pMsg->netMsg.msg_size = size + ((i != (int)playerThingIdx && stdComm_WSRelay_MotionWith(i)) ? MOTION_POS_TIMES : 0);
        sithComm_SendMsgToPlayer(pMsg, pInfo->playerNetId, 1, 0);
    }
    // And to a recording, once, as it was.
    pMsg->netMsg.msg_size = size;
    sithComm_SendMsgToPlayer(pMsg, -1, 4, 0);
    return 1;
}

// ---- Taking.

static float motion_F32(const uint8_t* p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static WSRelayPlayoutVec motion_Vec(const uint8_t* p)
{
    WSRelayPlayoutVec v = { motion_F32(p), motion_F32(p + 4), motion_F32(p + 8) };
    return v;
}

int stdComm_WSRelay_TakeMotion(SithThing* pThing, SithMessage* pMsg)
{
    if (!pThing || pThing->type != SITH_THING_PLAYER || pThing == sithPlayer_g_pLocalPlayerThing || pThing->moveType != SITH_MT_PHYSICS)
        return 0;
    int slot = motion_SlotOf(pThing);
    WSRelayMotionPeer* p = motion_Peer(slot);
    if (!p || jkPlayer_playerInfos[slot].pLocalPlayer != pThing || !stdComm_WSRelay_MotionWith(slot))
        return 0;

    const uint8_t* d = (const uint8_t*)pMsg->pktData;
    uint32_t size = pMsg->netMsg.msg_size;
    uint16_t attach = 0;
    memcpy(&attach, d + 4, 2);
    uint32_t known = attach ? MOTION_POS_ATTACHED : MOTION_POS_FREE;
    if ((int)pMsg->netMsg.idx != p->netId || size != known + MOTION_POS_TIMES)
    {
        // Without a time, while they're drawn from the buffer: the timed
        // ones say it too, at the moment it happened.
        return p->playout.count > 0;
    }

    WSRelayPlayoutSample s;
    memset(&s, 0, sizeof(s));
    int16_t sector = 0;
    memcpy(&sector, d + 6, 2);
    s.sector = sector;
    s.pos = motion_Vec(d + 8);
    s.angles = motion_Vec(d + 20);
    memcpy(&s.physFlags, d + 32, 4);
    s.vel = motion_Vec(d + 36);
    s.headPitch = motion_F32(d + known - 4);
    s.attachFlags = attach;
    uint32_t t32 = 0;
    uint16_t warp = 0;
    memcpy(&t32, d + known, 4);
    memcpy(&warp, d + known + 4, 2);
    s.warp = warp;
    s.t = stdComm_WSRelayPlayout_Unwrap(&p->playout, t32);
    s.taken = stdComm_WSRelay_MotionNow();
    if (!isfinite(s.pos.x) || !isfinite(s.pos.y) || !isfinite(s.pos.z))
        return 1;
    stdComm_WSRelayPlayout_Add(&p->playout, &s);
    // Phase 2's which update this game shows: claims without a moment.
    stdComm_WSRelay_NotePos(pThing, pMsg->netMsg.idx);
    return 1;
}

// ---- The timeline.

int stdComm_WSRelay_TimelineTime(SithThing* pThing, uint32_t* pTime)
{
    if (!pThing || pThing != sithPlayer_g_pLocalPlayerThing || !motion_bRunning || !motion_AnyWith())
        return 0;
    *pTime = (uint32_t)(int64_t)floor(motion_now + 0.5);
    return 1;
}

static SithMessage motion_heldMsg;

static void motion_Play(const WSRelayPlayoutHeld* pHeld)
{
    memcpy(&motion_heldMsg.netMsg, pHeld->data, sizeof(net_msg));
    memcpy(motion_heldMsg.pktData, pHeld->data + sizeof(net_msg), pHeld->len - sizeof(net_msg));
    int was = sithComm_msgInProgress;
    sithComm_msgInProgress = motion_heldMsg.netMsg.cogMsgId;
    sithMessage_Process(&motion_heldMsg);
    sithComm_msgInProgress = was;
}

// Plays their messages due at moment r and this game's time now, in order.
static void motion_PlayHeld(WSRelayMotionPeer* p, double r, double now)
{
    WSRelayPlayoutHeld held;
    while (stdComm_WSRelayPlayout_TakeHeld(&p->holds, r, now, &held))
    {
        motion_Play(&held);
    }
}

static int motion_Hold(WSRelayMotionPeer* p, const SithMessage* pMsg, double t)
{
    uint8_t data[WSRELAYPLAYOUT_HOLD_BYTES];
    int len = (int)sizeof(net_msg) + (int)pMsg->netMsg.msg_size;
    if (len > WSRELAYPLAYOUT_HOLD_BYTES)
        return 0;
    memcpy(data, &pMsg->netMsg, sizeof(net_msg));
    memcpy(data + sizeof(net_msg), pMsg->pktData, pMsg->netMsg.msg_size);

    // None free: the oldest plays now.
    WSRelayPlayoutHeld held;
    if (p->holds.count == WSRELAYPLAYOUT_HOLDS && stdComm_WSRelayPlayout_TakeHeld(&p->holds, INFINITY, INFINITY, &held))
        motion_Play(&held);
    return stdComm_WSRelayPlayout_Hold(&p->holds, t, motion_now, data, len);
}

// The bytes a message the timeline can hold has before the time, and where
// it names its thing: by guid, or by index (*pbIndex). 0 for the rest.
static uint32_t motion_TimedFields(const SithMessage* pMsg, int* pAt, int* pbIndex)
{
    *pAt = 0;
    *pbIndex = 0;
    switch (pMsg->netMsg.cogMsgId)
    {
        case DSS_PLAYKEY:
            return 18;
        case DSS_PLAYKEYMODE:
            return 10;
        case DSS_STOPKEY:
            return 12;
        case DSS_PLAYSOUNDMODE:
            return 14;
        case DSS_JKENABLESABER:
            *pbIndex = 1;
            return 18;
        case DSS_PLAYSOUND:
        {
            // Only a sound that follows a thing names it.
            uint32_t flags = 0;
            if (pMsg->netMsg.msg_size >= 4)
                memcpy(&flags, pMsg->pktData, 4);
            if (!(flags & SITHSOUNDFLAG_FOLLOWSTHING))
                return 0;
            *pAt = 14;
            return 22;
        }
    }
    return 0;
}

int stdComm_WSRelay_HoldForTimeline(SithMessage* pMsg)
{
    if (!motion_bRunning)
        return 0;
    const uint8_t* d = (const uint8_t*)pMsg->pktData;
    uint32_t size = pMsg->netMsg.msg_size;
    int id = pMsg->netMsg.cogMsgId;

    if (id == DSS_DEATH)
    {
        // What a player did before they died plays first.
        int32_t guid = 0;
        if (size >= 4)
            memcpy(&guid, d, 4);
        int slot = motion_SlotOf(sithThing_GetGuidThing(guid));
        WSRelayMotionPeer* p = motion_Peer(slot);
        if (p)
            motion_PlayHeld(p, INFINITY, INFINITY);
        return 0;
    }
    if (id == DSS_STOPSOUND)
    {
        // A sound their game stops may be one that waits: behind it.
        for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
        {
            WSRelayMotionPeer* p = motion_Peer(i);
            if (p && p->netId == (int)pMsg->netMsg.idx && p->holds.count)
            {
                return motion_Hold(p, pMsg, -INFINITY);
            }
        }
        return 0;
    }

    int at, bIndex;
    uint32_t fields = motion_TimedFields(pMsg, &at, &bIndex);
    if (!fields || size != fields + 4)
        return 0;
    SithThing* pThing;
    if (bIndex)
    {
        int16_t idx;
        memcpy(&idx, d + at, 2);
        pThing = sithThing_GetThingByIndex(idx);
    }
    else
    {
        int32_t guid;
        memcpy(&guid, d + at, 4);
        pThing = sithThing_GetGuidThing(guid);
    }
    // About their own player, from their game, while they're drawn from the
    // buffer.
    if (!stdComm_WSRelay_MotionDrawn(pThing))
        return 0;
    WSRelayMotionPeer* p = &motion_aPeers[motion_SlotOf(pThing)];
    if ((int)pMsg->netMsg.idx != p->netId)
        return 0;

    uint32_t t32;
    memcpy(&t32, d + fields, 4);
    double t = stdComm_WSRelayPlayout_Unwrap(&p->playout, t32);
    if (!stdComm_WSRelayPlayout_MustHold(&p->holds, t, p->pose.r))
        return 0;
    if (id == DSS_JKENABLESABER)
    {
        // Claims for what the swing hits can come before it plays.
        float damage, bladeLength;
        uint32_t swing;
        memcpy(&damage, d + 2, 4);
        memcpy(&bladeLength, d + 6, 4);
        memcpy(&swing, d + 14, 4);
        stdComm_WSRelay_NoteSwing(pThing, swing, damage, bladeLength);
    }
    return motion_Hold(p, pMsg, t);
}

// ---- Shots.

flex_t stdComm_WSRelay_ShotCatchUp(SithThing* pShooter, uint32_t fire32)
{
    int slot = motion_SlotOf(pShooter);
    WSRelayMotionPeer* p = motion_Peer(slot);
    if (!p || !motion_bRunning || jkPlayer_playerInfos[slot].pLocalPlayer != pShooter || !stdComm_WSRelay_MotionWith(slot))
        return 0.0;
    double offset = stdComm_WSRelay_ClockOffset(p->netId);
    if (!isfinite(offset))
        return 0.0;

    // How long before this game's moment, on their clock, they fired.
    double ms = stdComm_WSRelayPlayout_Since(motion_now + offset, fire32);
    if (!(ms > 0.0))
        return 0.0;
    return (flex_t)((ms < MOTION_CATCH_UP_MS ? ms : MOTION_CATCH_UP_MS) / 1000.0);
}

void stdComm_WSRelay_MoveShotOn(SithThing* pShot, flex_t secs)
{
    if (!pShot || !(secs > 0.0) || pShot->type != SITH_THING_WEAPON || pShot->moveType != SITH_MT_PHYSICS
        || (pShot->weaponParams.flags & SITH_WF_INSTANT_IMPACT))
        return;

    // As sithThing_Update moves it.
    for (flex_t left = secs; left > 0.0; left -= DELTA_PHYSTICK_FPS)
    {
        if (!pShot->type || (pShot->flags & (SITH_TF_DESTROYED|SITH_TF_DISABLED)))
            return;
        flex_t step = left < DELTA_PHYSTICK_FPS ? left : (flex_t)DELTA_PHYSTICK_FPS;
        sithPhysics_UpdateThing(pShot, step);
        sithThing_UpdateMove(pShot, step);
    }
    // And as much nearer its end, which comes in the next step at the soonest.
    if (pShot->msecLifeLeft)
    {
        int32_t ms = (int32_t)(secs * 1000.0);
        pShot->msecLifeLeft = pShot->msecLifeLeft > ms ? pShot->msecLifeLeft - ms : 1;
    }
}

// ---- Drawing.

static SithSector* motion_SectorAt(SithThing* pThing, const rdVector3* pPos, const WSRelayPlayoutPose* pPose)
{
    if (pThing->sector)
    {
        if (sithIntersect_IsSphereInSector(pPos, 0.0, pThing->sector))
            return pThing->sector;
        // Through the adjoins from where they were. It stops at a wall, and
        // says where.
        rdVector3 end = *pPos;
        SithSector* pSector = sithCollision_FindSectorInRadius(pThing->sector, &pThing->position, &end, 0.0);
        if (pSector && sithIntersect_IsSphereInSector(pPos, 0.0, pSector))
            return pSector;
    }
    int aIdx[2] = { pPose->sector, pPose->sectorNext };
    for (int i = 0; i < 2; i++)
    {
        SithSector* pSector = aIdx[i] >= 0 ? sithSector_GetPtrFromIdx(aIdx[i]) : NULL;
        if (pSector && sithIntersect_IsSphereInSector(pPos, 0.0, pSector))
            return pSector;
    }
    rdVector3 pos = *pPos;
    return sithWorld_g_pCurrentWorld ? sithSector_FindSectorAtPos(sithWorld_g_pCurrentWorld, &pos) : NULL;
}

// As sithThing_AttachThingToSurface lands a thing that falls onto a floor.
static void motion_LandSound(SithThing* pThing)
{
    if (!pThing->pSoundClass || !(pThing->attach_flags & SITH_ATTACH_SURFACE) || !pThing->attachedSurface)
        return;
    int flags = pThing->attachedSurface->flags;
    if (flags & SITH_SURFACE_METAL)
        sithSoundClass_PlayModeRandom(pThing, SITH_SC_LANDMETAL);
    else if (flags & SITH_SURFACE_WATER)
        sithSoundClass_PlayModeRandom(pThing, SITH_SC_LANDWATER);
    else if (flags & SITH_SURFACE_PUDDLE)
        sithSoundClass_PlayModeRandom(pThing, SITH_SC_LANDPUDDLE);
    else if (flags & (SITH_SURFACE_VERYDEEPWATER|SITH_SURFACE_EARTH))
        sithSoundClass_PlayModeRandom(pThing, SITH_SC_LANDEARTH);
    else
        sithSoundClass_PlayModeRandom(pThing, SITH_SC_LANDHARD);
}

// Loose things it walks into on its way, a crate say, are pushed as its own
// move pushes them (sithCollision_MoveThing, sithCollision_ThingCollisionHandler),
// by how fast its updates say it closes on them, so they move here too.
// Nothing here stops or turns it, their cogs aren't told it touched them, and
// players and actors aren't pushed, as nothing pushes it.
static void motion_Push(SithThing* pThing, const rdVector3* pMoved, const rdVector3* pVel)
{
    rdVector3 dir = *pMoved;
    flex_t dist = rdVector_Normalize3Acc(&dir);
    if (dist <= 0.0 || dist > MOTION_WARP_JKU || !pThing->sector || pThing->collide == SITH_COLLIDE_NONE
        || pThing->moveType != SITH_MT_PHYSICS || pThing->physicsParams.mass <= 0.0)
        return;

    sithCollision_SearchForCollisions(pThing->sector, pThing, &pThing->position, &dir, dist, pThing->moveSize,
                                      RAYCAST_200 | RAYCAST_800);
    for (SithCollision* pHit = sithCollision_PopStack(); pHit; pHit = sithCollision_PopStack())
    {
        SithThing* pOther = pHit->pThingCollided;
        if (!(pHit->type & SITHCOLLISION_THING) || !pOther
            || (pOther->type != SITH_THING_DEBRIS && pOther->type != SITH_THING_COG)
            || pOther->moveType != SITH_MT_PHYSICS || pOther->physicsParams.mass <= 0.0)
            continue;
        flex_t closing = stdMath_ClipNearZero(rdVector_Dot3(&pOther->physicsParams.vel, &pHit->hitNorm)
                                              - rdVector_Dot3(pVel, &pHit->hitNorm));
        if (closing <= 0.0)
            continue;
        if (!(pThing->physicsParams.flags & SITH_PF_SURFACEBOUNCE))
            closing *= 0.5;
        if (!(pOther->physicsParams.flags & SITH_PF_SURFACEBOUNCE))
            closing *= 0.5;
        flex_t mass = pThing->physicsParams.mass;
        flex_t massOther = pOther->physicsParams.mass;
        rdVector3 force;
        rdVector_Scale3(&force, &pHit->hitNorm, -closing * 2.0 * mass * massOther / (mass + massOther));
        sithPhysics_ApplyForce(pOther, &force);
    }
    sithCollision_DecreaseStackLevel();
}

static void motion_Draw(SithThing* pThing, WSRelayMotionPeer* p, double now)
{
    WSRelayPlayoutPose pose;
    if (!stdComm_WSRelayPlayout_Pose(&p->playout, now, &pose))
        return;

    rdVector3 pos = { pose.pos.x, pose.pos.y, pose.pos.z };
    SithSector* pSector = motion_SectorAt(pThing, &pos, &pose);
    if (pSector)
    {
        rdVector3 moved;
        rdVector_Sub3(&moved, &pos, &pThing->position);
        // Walking on from where it was drawn, not back to life or moved
        // further than it could go.
        if (p->bDrawn && pose.warp == p->lastWarp)
        {
            rdVector3 vel = { pose.vel.x, pose.vel.y, pose.vel.z };
            motion_Push(pThing, &moved, &vel);
        }
        pThing->position = pos;
        if (pSector != pThing->sector)
            sithThing_SetSector(pThing, pSector, 0);

        // Things it carries go with it.
        for (SithThing* pChild = pThing->pAttachedThing; pChild; pChild = pChild->pNextAttachedThing)
        {
            if (pChild->attach_flags & SITH_ATTACH_NOMOVE)
            {
                rdMatrix_TransformVector34(&pChild->position, &pChild->field_4C, &pThing->orient);
                rdVector_Add3Acc(&pChild->position, &pThing->position);
                if (pChild->sector != pThing->sector)
                    sithThing_SetSector(pChild, pThing->sector, 0);
            }
            else
            {
                rdVector3 dir = moved;
                flex_t dist = rdVector_Normalize3Acc(&dir);
                if (dist > 0.0)
                    sithCollision_MoveThing(pChild, &dir, dist, RAYCAST_40);
            }
        }
    }

    rdVector3 angles = { pose.angles.x, pose.angles.y, pose.angles.z };
    rdMatrix_BuildRotate34(&pThing->orient, &angles);
    rdVector3 head = { pose.headPitch, 0.0, 0.0 };
    sithActor_SetHeadPYR(pThing, &head);

    pThing->physicsParams.flags = pose.physFlags;
    pThing->physicsParams.vel.x = pose.vel.x;
    pThing->physicsParams.vel.y = pose.vel.y;
    pThing->physicsParams.vel.z = pose.vel.z;
    rdVector_Zero3(&pThing->physicsParams.angularVelocity);
    sithPuppet_SetNetVelocity(pThing, &pThing->physicsParams.vel);

    // On the floor or off it, as their game had them.
    if (!pose.attachFlags && pThing->attach_flags)
    {
        sithThing_DetachThing(pThing);
    }
    else if (pose.attachFlags && !pThing->attach_flags && now - p->floorTriedAt >= 100.0)
    {
        p->floorTriedAt = now;
        sithPhysics_FindFloor(pThing, 1);
        if (pThing->attach_flags)
        {
            motion_LandSound(pThing);
            if (pThing->pPuppetClass && !(pThing->physicsParams.flags & SITH_PF_CROUCHING))
                sithPuppet_PlayMode(pThing, SITH_ANIM_LAND, 0);
        }
    }

    // ?netstats: how smooth.
    p->steps++;
    if (pose.carried == 1)
        p->carried++;
    else if (pose.carried == 2)
        p->held++;
    if (p->bDrawn && pose.warp != p->lastWarp)
    {
        p->warps++;
    }
    else if (p->bDrawn)
    {
        double secs = (pose.r - p->pose.r) / 1000.0;
        double speed = rdVector_Len3(&pThing->physicsParams.vel);
        double moved = rdVector_Dist3(&p->drawnPos, &pThing->position);
        if (moved > speed * (secs > 0.0 ? secs : 0.0) * 1.5 + 0.05)
            p->jumps++;
    }
    p->lastWarp = pose.warp;
    p->pose = pose;
    p->drawnPos = pThing->position;
    p->bDrawn = 1;
}

static void motion_Stats(WSRelayMotionPeer* p, double now)
{
    if (!p->statsAt)
    {
        p->statsAt = now;
        return;
    }
    if (now - p->statsAt < MOTION_STATS_MS)
        return;
    if (p->steps)
    {
        stdComm_WSRelay_NoteMotion(p->netId, stdComm_WSRelayPlayout_Buffer(&p->playout), p->steps, p->carried, p->held,
                                   p->jumps, p->warps, p->playout.restarts);
    }
    p->statsAt = now;
    p->steps = p->carried = p->held = p->jumps = p->warps = 0;
}

void stdComm_WSRelay_MotionStep()
{
    if (!motion_bRunning || !stdComm_WSRelay_InMatch())
        return;

    motion_Own();
    motion_now += motion_step;

    for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
    {
        WSRelayMotionPeer* p = motion_Peer(i);
        if (!p)
            continue;

        // Their clock, for their shots.
        int bWith = stdComm_WSRelay_MotionWith(i);
        if (bWith != p->bSync)
        {
            stdComm_WSRelay_SyncClock(p->netId, bWith);
            p->bSync = bWith;
        }

        p->bDrawn = p->bDrawn && p->playout.count;
        if (p->playout.count && !bWith)
        {
            // No longer: drawn as before from the next update.
            stdComm_WSRelayPlayout_Clear(&p->playout);
            p->bDrawn = 0;
        }
        SithThing* pThing = jkPlayer_playerInfos[i].pLocalPlayer;
        if (!p->playout.count || !pThing || pThing->type != SITH_THING_PLAYER || !pThing->sector || (pThing->flags & SITH_TF_DESTROYED))
        {
            // Not drawn from the buffer: what waits plays now.
            motion_PlayHeld(p, INFINITY, INFINITY);
            continue;
        }
        motion_Draw(pThing, p, motion_now);
        motion_PlayHeld(p, p->pose.r, motion_now);
        motion_Stats(p, motion_now);
    }
}

int stdComm_WSRelay_MotionDrawn(SithThing* pThing)
{
    if (!pThing || pThing->type != SITH_THING_PLAYER || pThing == sithPlayer_g_pLocalPlayerThing)
        return 0;
    int slot = motion_SlotOf(pThing);
    if (slot < 0 || slot == (int)playerThingIdx)
        return 0;
    WSRelayMotionPeer* p = &motion_aPeers[slot];
    return p->netId && p->bDrawn && p->playout.count && jkPlayer_playerInfos[slot].pLocalPlayer == pThing;
}

int stdComm_WSRelay_MotionMoment(SithThing* pThing, double* pR, double* pBefore, double* pAfter, int* pCarried)
{
    if (!stdComm_WSRelay_MotionDrawn(pThing))
        return 0;
    WSRelayMotionPeer* p = &motion_aPeers[motion_SlotOf(pThing)];
    *pR = p->pose.r;
    *pBefore = p->pose.tBefore;
    *pAfter = p->pose.tAfter;
    *pCarried = p->pose.carried;
    return 1;
}
