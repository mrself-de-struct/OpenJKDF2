#ifndef _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H
#define _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Desktop build: a copy of the browser build's header (tristamus' fork,
// src/Platform/Networking/WSRelay), for its Hit what you see
// (stdComm_WSRelayHits.c, stdComm_WSRelayMotion.c). Only the hit rules' part
// is implemented here (stdComm_Hits.c): the browser's transport, its hit
// ledger and its test player aren't.
#if defined(PLATFORM_WSRELAY) || defined(PLATFORM_DPLAY)

// What stdComm_Open returns when the engine has to restart into the host's
// mod before joining (g_should_exit is already set).
#define STDCOMM_WSRELAY_RESTART (0x4A4B5752)

// The netcode this build speaks, which it tells the lobby when it connects
// (/relay?net=<n>): the lobby only lists games, and lets it join them, whose
// host speaks the same. Raise it only with a change older builds can't play
// a match with. Anything they can is agreed on through the hellos
// (stdComm_WSRelayHits.c), so every build so far is 0. NETCODE in
// packaging/wasm/index.html has to be the same.
#define STDCOMM_WSRELAY_NETCODE (0)

// What stdComm_Open returns when the lobby turned the join down because the
// game's host speaks a newer netcode, or an older one.
#define STDCOMM_WSRELAY_NEWER (0x4A4B564E)
#define STDCOMM_WSRELAY_OLDER (0x4A4B564F)

void stdComm_WSRelay_Startup();
void stdComm_WSRelay_Shutdown();

// Writes a line about the mod of listed game idx for the join screen, empty
// if there's nothing to say.
void stdComm_WSRelay_DescribeMod(int idx, char16_t* pOut, int maxChars);

// What happened to a hit between players, for the hit ledger
// (packaging/wasm/netstats.js). Under the Classic rules another player's
// game decides whether a hit on them counts, and this one only shows it.
// Under Hit what you see, this game decides its own player's hits on others
// and claims them (stdComm_WSRelayHits.c), and the other game can turn a
// claim down.
#define STDCOMM_WSRELAY_HIT (1)
#define STDCOMM_WSRELAY_BLOCK (2)
#define STDCOMM_WSRELAY_DEAD (3)
#define STDCOMM_WSRELAY_TURNED_DOWN (4)

// Kinds of hit, as the hit ledger names them.
#define STDCOMM_WSRELAY_KIND_SABER (1)
#define STDCOMM_WSRELAY_KIND_SHOT (2)
#define STDCOMM_WSRELAY_KIND_SPLASH (3)
#define STDCOMM_WSRELAY_KIND_FORCE (4)
#define STDCOMM_WSRELAY_KIND_OTHER (5)

// The kind of a hit by pSource with damageType.
int stdComm_WSRelay_HitKind(SithThing* pSource, int damageType);

// A hit on player pVictim from pSource (a player, or their shot, explosion or
// Force power), before the victim's game applies its damage or a block.
void stdComm_WSRelay_NoteHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType, int outcome);

// As NoteHit, for a hit whose source no longer leads back to its attacker: a
// bolt a ready saber turns away is the blocker's from then on.
void stdComm_WSRelay_NoteHitBy(SithThing* pVictim, SithThing* pAttacker, SithThing* pSource, flex_t damage, int damageType, int outcome);

// So the ledger can tell which attack a hit in either game came from, each
// saber swing a player starts (jkEnableSaber) gets a number, which the
// message that turns their blade on in the other games carries
// (DSS_JKENABLESABER). Set: the number from that message, 0 if it had none.
void stdComm_WSRelay_NewSwing(SithThing* pPlayer);
void stdComm_WSRelay_SetSwing(SithThing* pPlayer, uint32_t swing);
uint32_t stdComm_WSRelay_GetSwing(SithThing* pPlayer);
// When this game's own player last swung, on the motion clock
// (stdComm_WSRelay_MotionNow).
double stdComm_WSRelay_OwnSwingAt();

// A shot's id in every game (its guid) while another game's shot is made here
// (sithDSSThing_ProcessFire), which sithWeapon_WeaponFireProjectile gives it
// as it's made, since it can hit at once. 0 otherwise.
extern int stdComm_WSRelay_fireGuid;

// The number the game packet being handled came with on the fast lane, if
// it did (returns 1).
int stdComm_WSRelay_RecvSeq(uint32_t* pSeq);

// When this game sent player toId its position update numbered seq, by
// stdComm_WSRelay_Now's clock (ms), or -1 if it doesn't know.
double stdComm_WSRelay_SeqSentAt(int toId, uint32_t seq);
double stdComm_WSRelay_Now();

// Whether the rules' messages need sithComm's resends (1): the links under
// the lobby's games resend and keep order themselves, but the Imperial
// Network Protocol's (UDP between desktops) don't.
int stdComm_WSRelay_HitsResend();

// The hit ledger: a claim from player otherId this game turned down, or
// found its player dead for (outcome STDCOMM_WSRELAY_DEAD), and why
// (stdComm_WSRelayHitCheck.h's WSRELAYHIT_*, or
// STDCOMM_WSRELAY_HITS_NOT_PLAYING). distance: how close its player came to
// where the claim showed it, -1 if that wasn't checked. moment: whether the
// claim named the moment the attacker's game drew the player at, and how it
// was checked (STDCOMM_WSRELAY_MOMENT_*). ageMs: how far back that moment
// was when it was checked, on the motion clock, -1 without one.
void stdComm_WSRelay_NoteTurnedDown(int otherId, int kind, int outcome, flex_t damage, uint32_t attack, int reason, float distance, int moment, float ageMs);

// The hit ledger: a targeted Force power on this game's player from player
// otherId, sent with the moment its game drew this one's player at
// (stdComm_WSRelay_ForceTakes), which played (STDCOMM_WSRELAY_HIT) or was
// turned down (STDCOMM_WSRELAY_TURNED_DOWN) for reason. distance, moment and
// ageMs as for NoteTurnedDown.
void stdComm_WSRelay_NoteTargeted(int otherId, int outcome, int reason, float distance, int moment, float ageMs);

// ---- Hit what you see (stdComm_WSRelayHits.c).

// The rules a match plays by: the host's choice, on its host screen.
#define STDCOMM_WSRELAY_HITS_CLASSIC (0)
#define STDCOMM_WSRELAY_HITS_HWYS (1)

// How far back a claim can reach, in ms.
#define STDCOMM_WSRELAY_REACH_MIN (150)
#define STDCOMM_WSRELAY_REACH_MAX (500)
#define STDCOMM_WSRELAY_REACH_DEFAULT (300)

// A host lists its rules in its lobby entry: guidInstance.c is this, and
// guidInstance.d the rules, with the reach in ms from bit 8. The lobby and
// older games ignore both.
#define STDCOMM_WSRELAY_HITS_ENTRY_MAGIC (0x4A4B4852) // "JKHR"

// The engine message the rules' messages go in, past DSS_MAX: 'J' 'C'.
#define STDCOMM_WSRELAY_HITS_MSG (0x434A)

// Why a claim was turned down, past stdComm_WSRelayHitCheck.h's reasons:
// this game doesn't play by claims with the sender.
#define STDCOMM_WSRELAY_HITS_NOT_PLAYING (8)

// Another game's shot starts where its shooter was when this game's copy of
// the shooter is farther than this from there, in JKU.
#define STDCOMM_WSRELAY_SHOT_START_SLACK (0.3)

// The host's settings (jkGuiNetHost), which it plays by when it hosts:
// Classic unless it picks Hit what you see.
extern int stdComm_WSRelay_bClassicHits;
extern int stdComm_WSRelay_hitReachMs;

int stdComm_WSRelay_ClampReach(int reachMs);

// This match's rules, and the reach. A new session starts Classic, until the
// host's entry or hello says otherwise.
void stdComm_WSRelay_SetHitRules(int rules, int reachMs);
int stdComm_WSRelay_GetHitRules(int* pReachMs);
void stdComm_WSRelay_ResetHits();

// A hit on player pVictim from pSource, before sithThing_DamageThing applies
// it. Returns 1 if this game leaves it to the attacker's game, which sends a
// claim for it: the hit only shows here. When this game's own attack hit
// another player, it claims the hit, and the hit shows here as before (0).
int stdComm_WSRelay_ClaimHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType);

// A claim named no moment; named the moment the attacker's game drew the
// player at, and was checked there; or named one further back than the
// reach, so was checked as a claim without one.
#define STDCOMM_WSRELAY_MOMENT_NONE (0)
#define STDCOMM_WSRELAY_MOMENT_AT (1)
#define STDCOMM_WSRELAY_MOMENT_PAST_REACH (2)

// While a claim's damage is applied: the ledger's kind and attack number for
// it, how close the player came to where the claim showed them, how it named
// the moment (STDCOMM_WSRELAY_MOMENT_*), and how far back that was (ms, -1
// without one).
int stdComm_WSRelay_ApplyingClaim(int* pKind, uint32_t* pAttack, float* pDistance, int* pMoment, float* pAgeMs);

// Whether this game claims its hits on player pPlayer, and so decides its
// blocks against them too.
int stdComm_WSRelay_ClaimsTo(SithThing* pPlayer);

// Whether a hit or block on this game's player by pSource is the attacker's
// game's to decide.
int stdComm_WSRelay_DecidedElsewhere(SithThing* pSource, int damageType);

// Targeted Force powers: the cog message a power aimed at another player
// sends their game (SkillTarget, sithDSSCog_SendMessage). ForceMoment: the
// STDCOMM_WSRELAY_FORCE_BYTES that go after it, into pOut, while this game
// draws pVictim from their timed updates in a match played by Hit what you
// see: the moment it drew them at and where (0: none). ForceTakes: whether
// one on this game's player pVictim from pSource, with those bytes, plays
// (1) or is turned down (0), by where its player was then; the hit ledger
// has either. ApplyingForce: while one that was checked plays, until
// ForceDone, so the ledger doesn't count the damage it does again.
#define STDCOMM_WSRELAY_FORCE_BYTES (24)
int stdComm_WSRelay_ForceMoment(SithThing* pVictim, uint8_t* pOut);
int stdComm_WSRelay_ForceTakes(SithThing* pVictim, SithThing* pSource, const uint8_t* pData);
void stdComm_WSRelay_ForceDone();
int stdComm_WSRelay_ApplyingForce();

// What other players' games tell this one about their attacks: a swing
// (DSS_JKENABLESABER: its number, damage and blade length; noted once, so
// again is nothing), a shot (sithDSSThing_ProcessFire: where it was fired
// from, its guid, how its game scaled it, and how far along its path it
// starts, in s), and their position updates (sithDSSThing_ProcessPos), from
// player fromNetId.
void stdComm_WSRelay_NoteSwing(SithThing* pPlayer, uint32_t swing, flex_t damage, flex_t bladeLength);
void stdComm_WSRelay_NoteShot(SithThing* pTemplate, const rdVector3* pFirePos, int guid, flex_t scale, int scaleFlags, flex_t secsAhead);
void stdComm_WSRelay_NotePos(SithThing* pThing, int fromNetId);

// An explosion made by pWeapon (sithWeapon_CreateWeaponExplosion), so a hit
// by its blast is known by the shot's guid.
void stdComm_WSRelay_NoteBlast(SithThing* pExplosion, SithThing* pWeapon);

// MakingBlast: pWeapon's blast is being made (NULL: no longer), and the one
// before, which it returns, when that's done. NoteBlastPart: pThing was made
// in turn (sithThing_CreateThingAtPos's creatething): if it's an explosion,
// it's the shot's blast too. BlastShot: the guid of the shot an explosion
// came from, or 0.
SithThing* stdComm_WSRelay_MakingBlast(SithThing* pWeapon);
void stdComm_WSRelay_NoteBlastPart(SithThing* pThing);
int stdComm_WSRelay_BlastShot(SithThing* pExplosion);

// Whether player pBlocker's saber turning pWeapon away here is another
// game's to decide (the one that fired it), which then says so.
int stdComm_WSRelay_BlockDecidedElsewhere(SithThing* pWeapon, SithThing* pBlocker);

// Whether another player is in the fire wait after their last swing, as their
// game said: until it's over, their saber turns no bolt away, as a game's own
// player's doesn't (sithWeapon_fireWait).
int stdComm_WSRelay_InFireWait(SithThing* pPlayer);

// A saber turned pWeapon away (sithActor_thing_anim_blocked). If this game
// fired it, the others' copies follow its new path.
void stdComm_WSRelay_BoltTurned(SithThing* pWeapon, SithThing* pBlocker);

// pBy sets charge pWeapon off: it goes off in fuseMs. Returns 0 if another
// game says when, which this one then hears.
int stdComm_WSRelay_ArmCharge(SithThing* pWeapon, SithThing* pBy, int fuseMs);

// Where another game's shot starts here (sithDSSThing_ProcessFire), from
// where its shooter was: returns 1 with the place and its sector when this
// game's copy of the shooter is too far from there.
int stdComm_WSRelay_ShotStart(SithThing* pShooter, int guid, const rdVector3* pShooterPos, rdVector3* pOutPos, SithSector** ppOutSector);

// A message of the rules' own (sithMessage_ProcessMessages), each frame's
// end (sithMain), and after a fire message, so claims for what a shot hit as
// it was made go after it (sithDSSThing_Fire).
void stdComm_WSRelay_ProcessHits(SithMessage* pMsg);
void stdComm_WSRelay_HitsFrame();
void stdComm_WSRelay_FlushHits();

// The Join list's words for a listed game's hit rules, or fewer of them.
void stdComm_WSRelay_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars);

// Whether this game and player slot's draw each other from buffers of their
// timed updates: both play Hit what you see, and both games have it (hello
// version 2).
int stdComm_WSRelay_MotionWith(int slot);

// The hello version this game and player slot's agreed on (the lower of the
// two), or 0 if their game hasn't said hello: a build from before Hit what
// you see, or not yet. For the match summary (netstats.js).
int stdComm_WSRelay_HelloWith(int slot);

// Where this game's player is at t on the motion clock, for the record
// claims are checked against: each physics step, and each frame's end.
void stdComm_WSRelay_RecordOwn(double t);

// ---- Smooth, time-stamped motion (stdComm_WSRelayMotion.c).

// The motion clock: the time of the physics state this game is at, in ms on
// stdComm_WSRelay_Now's clock.
double stdComm_WSRelay_MotionNow();
void stdComm_WSRelay_ResetMotion();

// Each frame, before its physics steps, and each step, before things move
// (sithMain).
void stdComm_WSRelay_MotionFrame(int numSteps);
void stdComm_WSRelay_MotionStep();

// This game's own position update, built in sithComm_netMsgTmp
// (sithDSSThing_Pos): returns 1 if it sent it, with its time to the games
// that read it. 0: the caller sends it as before.
int stdComm_WSRelay_SendOwnPos(SithThing* pThing, int toID, int dpFlags);

// Another player's position update (sithDSSThing_ProcessPos): returns 1 if it
// went to the buffer they're drawn from, or isn't needed because of it.
int stdComm_WSRelay_TakeMotion(SithThing* pThing, SithMessage* pMsg);

// Whether another player is drawn from the buffer this step: their own
// physics and movement are skipped (sithThing_Update).
int stdComm_WSRelay_MotionDrawn(SithThing* pThing);

// The moment a player drawn from the buffer was drawn at this step, on their
// game's clock, with the times of their updates before and after it (-1 if
// none after), and whether they were carried on past the newest (1) or held
// still (2). 0 if they're not drawn from the buffer.
int stdComm_WSRelay_MotionMoment(SithThing* pThing, double* pR, double* pBefore, double* pAfter, int* pCarried);

// Swing, animation, sound and saber-on messages about this game's own
// player (PLAYKEY, PLAYKEYMODE, STOPKEY, PLAYSOUND following them,
// PLAYSOUNDMODE, JKENABLESABER) carry the motion clock's time (u32 ms, after
// the original's fields) while other games draw it from its timed updates,
// and those play them when the moment they draw it at gets there (at most
// WSRELAYPLAYOUT_HOLD_MS later); a shot's fire message carries it too
// (stdComm_WSRelay_ShotCatchUp). TimelineTime: whether to add the time for
// pThing, and the time.
int stdComm_WSRelay_TimelineTime(SithThing* pThing, uint32_t* pTime);

// A message just in (sithMessage_ProcessMessages): 1 if it waits to play on
// the timeline the player it's about is drawn on, and isn't to be handled
// now. What a saber-on message means for hits is noted at once. A sound
// stop from their game waits behind what does, and their death plays it
// all first.
int stdComm_WSRelay_HoldForTimeline(SithMessage* pMsg);

// Another game's shot (sithDSSThing_ProcessFire), fired at fire32 (u32 ms on
// its game's motion clock): how far along its path it really is by now on
// this game's clock, from the two clocks' offset, in s, at most 0.25. 0 if
// unknown.
flex_t stdComm_WSRelay_ShotCatchUp(SithThing* pShooter, uint32_t fire32);

// Moves a shot on along its path for secs, a physics step at a time, as its
// game moved it.
void stdComm_WSRelay_MoveShotOn(SithThing* pShot, flex_t secs);

// Player netId's clock less this game's (stdComm_WSRelay_Now), in ms, NaN if
// unknown: netstats.js measures it, the way NTP does, while this game asks
// for it (SyncClock).
void stdComm_WSRelay_SyncClock(int netId, int bOn);
double stdComm_WSRelay_ClockOffset(int netId);

// ?netstats: how player otherId was drawn over the last second: the buffer's
// own part of the delay (ms), the steps drawn, how many carried on past the
// newest update, held still, jumped further than their velocity explains or
// across a warp, and how often their buffer started over.
void stdComm_WSRelay_NoteMotion(int otherId, double bufferMs, uint32_t steps, uint32_t carried, uint32_t held,
                                uint32_t jumps, uint32_t warps, uint32_t restarts);

// ?nettest's scripted player (stdComm_WSRelayTest.c). While it plays, it
// gives sithControl the input it reads.
extern int stdComm_WSRelay_bTestInput;
void stdComm_WSRelay_TestTick(SithThing* pPlayer, flex_t deltaSecs);
int stdComm_WSRelay_TestKey(int inputFunc, int* pPresses);
flex_t stdComm_WSRelay_TestAxis(int inputFunc);

// And what it counts that the hit ledger leaves out, only while it plays: a
// bolt pShooter fired that reached player pBlocker's ready saber, and whether
// it turned it away here; a hit on a player by something no player fired; and
// a targeted Force power in bin played on this game's player, or sent to
// another (bSent).
void stdComm_WSRelay_TestBolt(SithThing* pWeapon, SithThing* pShooter, SithThing* pBlocker, int bTurned);
void stdComm_WSRelay_TestHit(SithThing* pVictim, SithThing* pSource, flex_t damage);
void stdComm_WSRelay_TestSkill(SithThing* pVictim, int bin, int bSent);

// Whether this game is in a match with other players: its player has joined,
// which the host checks before taking anything from it.
int stdComm_WSRelay_InMatch();

#endif // PLATFORM_WSRELAY || PLATFORM_DPLAY

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H
