// Hit what you see: hit rules a host can pick on its host screen
// (jkGuiNetHost) instead of Classic, the default. The game that made an
// attack decides what it hit, by what its player saw, and the target's game
// checks each hit before it counts (stdComm_WSRelayHitCheck.h). Classic
// keeps the original rules, where each game decides the hits on its own
// player by where it has the attacker and the attack: about a round trip of
// the network ago, so a swing or a shot that hit on the attacker's screen
// often missed there.
//
// In a match played by Hit what you see:
//   - When this game's attack (its player's saber swing, a shot it fired, or
//     that shot's blast) hits another player, it sends that player's game a
//     claim: what hit, how hard, where it showed the target, and which of the
//     target's position updates it had on screen. Here the hit only shows,
//     as before: other players never take damage in this game.
//   - That game checks the claim against where its player was and applies
//     it through the usual code (shields, Force powers, death and kill
//     credit), or turns it down, and tells the attacker's hit ledger
//     (packaging/wasm/netstats.js) which, with a receipt.
//   - Another game's attack on this game's player only shows here, since
//     that game sends the claim. Blocks the same: the attacker's game
//     decides them by what it shows.
//   - A bolt this game fired stays this game's to decide after another
//     player's saber turns it away: it tells the other games the bolt's new
//     path (TURNED), and their copies follow it.
//   - A charge set off early (a mine touched, or a charge hit) is set off by
//     the game that fired it, or by the game whose player set it off, which
//     tells the others (DETONATE).
//   - A game's own player can't turn a bolt away until the fire wait after
//     their swing is over (sithWeapon_fireWait), so each swing tells the
//     other games how long that is (WAIT), for the bolts they decide. A
//     game that draws them from their timed updates has them in it while
//     the moment it draws them at is.
//   - A targeted Force power (Grip, Pull, Blinding, Deadly Sight) is still
//     the attacker's game's to aim: it picks a player it shows in reach and
//     sends their game the power's cog message (SkillTarget). Between two
//     games that draw each other from their timed updates, the message also
//     says the moment the player was drawn at and where it showed them
//     (stdComm_WSRelay_ForceMoment), and their game plays it only if they
//     were there then, as it checks a claim's last step.
// Everything else stays as it was: the host's damage messages, falls, world
// traps, and a player's own attacks on itself.
//
// Two games play by claims with each other only once each has said hello
// (HELLO, with the rules it plays by) and heard the other's. An older build
// never says hello, so with it hits stay as they were, both ways. Hello
// version 2 is a game that draws other players from buffers of their timed
// updates (stdComm_WSRelayMotion.c): two such games send each other times
// on their updates, and claims that name the moment the target was drawn
// at, which its game checks its record at. Version 3 also claims the
// explosions a blast makes in turn (a MotS flash bomb's): an older game
// decides those for its own player and never claims them, so with one they
// stay as they were, both ways. Version 1 gets Phase 2 claims. Jedi Knight
// and MotS games say the same version.
//
// All of it is one engine message, past DSS_MAX so older engines ignore it
// (STDCOMM_WSRELAY_HITS_MSG), sent without a resend id: the links under it
// already resend and keep order, and a claim that came a resend later would
// be past its reach anyway. First byte: type.
//   HELLO     version u8, flags u8 (1 from the host, 2 heard yours), rules
//             u8, reach ms u16, 0 u16
//   CLAIM     source u8, ledger kind u8, flags u8 (1 update number known,
//             2 moment known, 4 carried on past the target's newest
//             update), damage f32, damage type s32, attack u32 (the swing's
//             number or the shot's guid), target's guid s32, credited
//             player's guid s32, target as shown vec3, attacker's position
//             vec3, blast vec3, the target's update number u32, how long it
//             had been applied in ms u16, 0 u16; with a moment: the moment
//             f64 (the target's clock), ms from its update before u16, ms to
//             its update after u16 (0xFFFF: none)
//   TURNED    0 u8, template s16, shot guid s32, blocker's guid s32, sector
//             s16, 0 s16, position vec3, velocity vec3
//   DETONATE  0 u8, fuse ms u16, charge guid s32
//   WAIT      0 u8, fire wait ms u16; from version 2, when the swing
//             started f64 (its game's motion clock)

#include "stdComm_WSRelay.h"
#include "stdComm_WSRelayHitCheck.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Devices/sithComm.h"
#include "Gameplay/sithPlayer.h"
#include "World/sithThing.h"
#include "World/sithSector.h"
#include "World/sithSoundClass.h"
#include "World/sithTemplate.h"
#include "World/sithWorld.h"
#include "Engine/sithPuppet.h"
#include "Cog/sithCog.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "stdPlatform.h"
#include "jk.h"

#include <math.h>
#include <string.h>

#define HITS_HELLO (1)
#define HITS_CLAIM (2)
#define HITS_TURNED (3)
#define HITS_DETONATE (4)
#define HITS_WAIT (5)

#define HITS_VERSION (3)
// The first version that claims explosions made in turn (hits_ClaimedWith).
#define HITS_VERSION_PARTS (3)

#define HITS_HELLO_HOST (1)
#define HITS_HELLO_HEARD (2)

#define HITS_CLAIM_SEQ (1)
#define HITS_CLAIM_MOMENT (2)
#define HITS_CLAIM_CARRIED (4)

// How often each game says hello to each other player, and how soon after
// its last one it answers a hello that needs an answer.
#define HITS_HELLO_MS (5000.0)
#define HITS_ANSWER_MS (250.0)

// What this game keeps of each other player's attacks, for checking their
// claims: swings, shots, and charges, which can go off long after they're
// fired. Shots any saber turned away, by whom.
#define HITS_SWINGS (8)
#define HITS_SHOTS (48)
#define HITS_CHARGES (16)
#define HITS_TURNS (16)
// Explosions, by the shot they came from.
#define HITS_BLASTS (64)
// How far down a blast's templates (creatething) to look for more of its
// explosions.
#define HITS_BLAST_CHAIN (8)

#define HITS_OUTBOX (64)
#define HITS_MAX_BODY (96)

typedef struct WSRelayHitsSwing
{
    uint32_t swing;
    double at;
    float damage;
    float bladeLength;
} WSRelayHitsSwing;

typedef struct WSRelayHitsShot
{
    int guid;
    double at;              // when it was fired, as its fire message tells
    WSRelayHitVec from;     // where it was fired from
    float speed;            // JKU/s
    float range;            // a shot that hits at once reaches this far
    float gravity;          // JKU/s/s, if it falls
    float damage;
    int damageType;
    float blastRange;       // its blast's, or 0
    float blastDamage;
    int blastDamageType;
    int blasts;             // the explosions in its blast, which can each
                            // hit a player once
} WSRelayHitsShot;

// Another player's game, by their player slot.
typedef struct WSRelayHitsPeer
{
    int netId;              // the player this is about; all of it starts
                            // over when another player gets the slot
    int bHello;             // their game said hello
    int version;            // with this version
    int bHeardMe;           // and its last hello said it had heard this one's
    int rules;              // the rules their game said it plays by
    double helloAt;         // when this game last said hello to them
    int bAnswer;            // their hello needs an answer
    uint32_t posSeq;        // their last position update applied here
    int bPosSeq;            // it had a number (the fast lane)
    double posAt;           // when it was applied
    double waitUntil;       // the end of their fire wait, as this game has it
    double waitFrom;        // and on their clock, if their game said
    double waitTo;
    int bWaitMoment;
    WSRelayHitsSwing aSwings[HITS_SWINGS];
    int nextSwing;
    WSRelayHitsShot aShots[HITS_SHOTS];
    int nextShot;
    WSRelayHitsShot aCharges[HITS_CHARGES];
    int nextCharge;
    struct
    {
        int guid;
        int blockerGuid;
    } aTurns[HITS_TURNS];
    int nextTurn;
} WSRelayHitsPeer;

// What made a hit, as both games know it.
typedef struct WSRelayHitsAttack
{
    int source;             // WSRELAYHIT_SABER, _SHOT or _SPLASH
    int kind;               // as the hit ledger names it (STDCOMM_WSRELAY_KIND_*)
    int maker;              // the player slot whose game made it
    uint32_t attack;        // the swing's number, or the shot's guid
    SithThing* pCredit;     // the player it counts for
    WSRelayHitVec blast;
    int bPart;              // an explosion its blast made in turn
} WSRelayHitsAttack;

int stdComm_WSRelay_bClassicHits = 1;
int stdComm_WSRelay_hitReachMs = STDCOMM_WSRELAY_REACH_DEFAULT;

// This match's rules: the host's choice.
static int hits_rules = STDCOMM_WSRELAY_HITS_CLASSIC;
static int hits_reachMs = STDCOMM_WSRELAY_REACH_DEFAULT;

static WSRelayHitsPeer hits_aPeers[JKPLAYER_NUM_INFOS];

// Where this game's player has been, and each life it's had.
static WSRelayHitRecord hits_record;
static uint32_t hits_life = 1;
static int hits_bWasAlive = 0;
static void* hits_pWorld = NULL;
static double hits_recordAt = -1.0;
// Its last swing, whose fire wait the other games have been told.
static uint32_t hits_ownSwing = 0;

// Attacks that already counted on this game's player, and the ones of them
// a claim counted.
static WSRelayHitSeen hits_seen;
static WSRelayHitSeen hits_claimed;

static struct
{
    int idx;
    uint32_t signature;
    int weaponGuid;
    int bPart;              // made in turn (stdComm_WSRelay_NoteBlastPart)
} hits_aBlasts[HITS_BLASTS];
static int hits_nextBlast = 0;

// Messages wait here for the end of the frame, or the next fire message,
// so a claim never gets to the target's game before the swing or shot it's
// about.
static struct
{
    int to;
    int len;
    uint8_t body[HITS_MAX_BODY];
} hits_aOutbox[HITS_OUTBOX];
static int hits_numOutbox = 0;
static SithMessage hits_msg;

// While a claim's damage is applied (hits_TakeClaim): what the hit ledger
// says about it.
static int hits_bApplying = 0;
static int hits_applyKind = 0;
static uint32_t hits_applyAttack = 0;
static float hits_applyDistance = -1.0f;
static int hits_applyMoment = 0;
static float hits_applyAgeMs = -1.0f;

// While a targeted Force power this game checked takes effect
// (stdComm_WSRelay_ForceTakes), which the hit ledger has already.
static int hits_bForce = 0;

static double hits_Now()
{
    return stdComm_WSRelay_Now();
}

static WSRelayHitVec hits_Vec(const rdVector3* pVec)
{
    WSRelayHitVec v = { pVec->x, pVec->y, pVec->z };
    return v;
}

static int hits_SlotOf(SithThing* pPlayer)
{
    if (!pPlayer || pPlayer->type != SITH_THING_PLAYER || !pPlayer->actorParams.pPlayer)
        return -1;
    intptr_t slot = pPlayer->actorParams.pPlayer - jkPlayer_playerInfos;
    return (slot >= 0 && slot < JKPLAYER_NUM_INFOS) ? (int)slot : -1;
}

// The player slot of the game that made a thing (sithThing_Create): its
// guid's high half is that slot plus one. -1 for the level's own things.
static int hits_MakerOf(int guid)
{
    int slot = (int)((uint32_t)guid >> 16) - 1;
    return (slot >= 0 && slot < JKPLAYER_NUM_INFOS) ? slot : -1;
}

static int hits_IsAlive(SithThing* pPlayer)
{
    return pPlayer && pPlayer->type == SITH_THING_PLAYER
        && !(pPlayer->flags & (SITH_TF_DISABLED|SITH_TF_DEAD|SITH_TF_DESTROYED))
        && pPlayer->actorParams.health > 0.0;
}

// Another player as this game shows them. Their health here isn't theirs:
// it's 0 from their first death on (sithActor_KillActor), since only their
// own game sets it again when they come back (sithPlayer). So only the flags
// say whether they're dead here; their game checks the rest.
static int hits_IsShownAlive(SithThing* pPlayer)
{
    return pPlayer && pPlayer->type == SITH_THING_PLAYER
        && !(pPlayer->flags & (SITH_TF_DISABLED|SITH_TF_DEAD|SITH_TF_DESTROYED));
}

static int hits_Live()
{
    return hits_rules == STDCOMM_WSRELAY_HITS_HWYS && stdComm_WSRelay_InMatch();
}

static WSRelayHitsPeer* hits_Peer(int slot)
{
    if (slot < 0 || slot >= JKPLAYER_NUM_INFOS || slot == (int)playerThingIdx)
        return NULL;
    SithPlayer* pInfo = &jkPlayer_playerInfos[slot];
    int netId = (pInfo->flags & 1) ? (int)pInfo->playerNetId : 0;
    WSRelayHitsPeer* p = &hits_aPeers[slot];
    if (p->netId != netId)
    {
        memset(p, 0, sizeof(*p));
        p->netId = netId;
    }
    return netId ? p : NULL;
}

// This game sends claims for its attacks on that player.
static int hits_ClaimsTo(WSRelayHitsPeer* p)
{
    return p && p->bHello && p->rules == STDCOMM_WSRELAY_HITS_HWYS && hits_Live();
}

// That player's game sends claims for its attacks on this game's player.
static int hits_DecidedBy(WSRelayHitsPeer* p)
{
    return hits_ClaimsTo(p) && p->bHeardMe;
}

// ---- Messages.

typedef struct WSRelayHitsOut
{
    uint8_t* p;
    int n;
} WSRelayHitsOut;

static void hits_PutU8(WSRelayHitsOut* o, uint32_t v)
{
    o->p[o->n++] = (uint8_t)v;
}

static void hits_PutU16(WSRelayHitsOut* o, uint32_t v)
{
    uint16_t x = (uint16_t)v;
    memcpy(o->p + o->n, &x, 2);
    o->n += 2;
}

static void hits_Put32(WSRelayHitsOut* o, uint32_t v)
{
    memcpy(o->p + o->n, &v, 4);
    o->n += 4;
}

static void hits_PutF32(WSRelayHitsOut* o, float v)
{
    memcpy(o->p + o->n, &v, 4);
    o->n += 4;
}

static void hits_PutF64(WSRelayHitsOut* o, double v)
{
    memcpy(o->p + o->n, &v, 8);
    o->n += 8;
}

static void hits_PutVec(WSRelayHitsOut* o, WSRelayHitVec v)
{
    hits_PutF32(o, v.x);
    hits_PutF32(o, v.y);
    hits_PutF32(o, v.z);
}

typedef struct WSRelayHitsIn
{
    const uint8_t* p;
    int n;
    int len;
    int bad;
} WSRelayHitsIn;

static int hits_Has(WSRelayHitsIn* in, int bytes)
{
    if (in->n + bytes > in->len)
    {
        in->bad = 1;
        return 0;
    }
    return 1;
}

static uint32_t hits_GetU8(WSRelayHitsIn* in)
{
    return hits_Has(in, 1) ? in->p[in->n++] : 0;
}

static uint32_t hits_GetU16(WSRelayHitsIn* in)
{
    uint16_t x = 0;
    if (hits_Has(in, 2))
    {
        memcpy(&x, in->p + in->n, 2);
        in->n += 2;
    }
    return x;
}

static int16_t hits_GetS16(WSRelayHitsIn* in)
{
    return (int16_t)hits_GetU16(in);
}

static uint32_t hits_Get32(WSRelayHitsIn* in)
{
    uint32_t x = 0;
    if (hits_Has(in, 4))
    {
        memcpy(&x, in->p + in->n, 4);
        in->n += 4;
    }
    return x;
}

static float hits_GetF32(WSRelayHitsIn* in)
{
    float x = 0.0f;
    if (hits_Has(in, 4))
    {
        memcpy(&x, in->p + in->n, 4);
        in->n += 4;
    }
    return x;
}

static double hits_GetF64(WSRelayHitsIn* in)
{
    double x = 0.0;
    if (hits_Has(in, 8))
    {
        memcpy(&x, in->p + in->n, 8);
        in->n += 8;
    }
    return x;
}

static WSRelayHitVec hits_GetVec(WSRelayHitsIn* in)
{
    WSRelayHitVec v;
    v.x = hits_GetF32(in);
    v.y = hits_GetF32(in);
    v.z = hits_GetF32(in);
    return v;
}

// The moment another player was drawn at (stdComm_WSRelay_MotionMoment), as
// a claim or a Force power names it: the moment f64, ms from their update
// before it u16, ms to the one after u16 (0xFFFF: none).
static void hits_PutMoment(WSRelayHitsOut* o, double r, double tBefore, double tAfter)
{
    double before = ceil(r - tBefore), after = tAfter < 0.0 ? 65535.0 : ceil(tAfter - r);
    hits_PutF64(o, r);
    hits_PutU16(o, before < 0.0 ? 0 : before > 65534.0 ? 65534 : (uint32_t)before);
    hits_PutU16(o, after < 0.0 ? 0 : after > 65535.0 ? 65535 : (uint32_t)after);
}

void stdComm_WSRelay_FlushHits()
{
    for (int i = 0; i < hits_numOutbox; i++)
    {
        memset(&hits_msg.netMsg, 0, sizeof(hits_msg.netMsg));
        hits_msg.netMsg.cogMsgId = STDCOMM_WSRELAY_HITS_MSG;
        hits_msg.netMsg.msg_size = hits_aOutbox[i].len;
        memcpy(hits_msg.pktData, hits_aOutbox[i].body, hits_aOutbox[i].len);
        // Changed: with a resend id where the transport doesn't resend
        // (stdComm_WSRelay_HitsResend).
        sithComm_SendMsgToPlayer(&hits_msg, hits_aOutbox[i].to, 1, stdComm_WSRelay_HitsResend());
    }
    hits_numOutbox = 0;
}

// to: a player's net id, or -1 for everyone.
static void hits_Queue(int to, const uint8_t* pBody, int len)
{
    if (len > HITS_MAX_BODY)
        return;
    if (hits_numOutbox >= HITS_OUTBOX)
        stdComm_WSRelay_FlushHits();
    hits_aOutbox[hits_numOutbox].to = to;
    hits_aOutbox[hits_numOutbox].len = len;
    memcpy(hits_aOutbox[hits_numOutbox].body, pBody, len);
    hits_numOutbox++;
}

// ---- Rules.

void stdComm_WSRelay_ResetHits()
{
    hits_rules = STDCOMM_WSRELAY_HITS_CLASSIC;
    hits_reachMs = STDCOMM_WSRELAY_REACH_DEFAULT;
    memset(hits_aPeers, 0, sizeof(hits_aPeers));
    stdComm_WSRelayHit_ClearRecord(&hits_record);
    stdComm_WSRelayHit_ClearSeen(&hits_seen);
    stdComm_WSRelayHit_ClearSeen(&hits_claimed);
    hits_life++;
    hits_bWasAlive = 0;
    hits_pWorld = NULL;
    hits_recordAt = -1.0;
    hits_ownSwing = 0;
    memset(hits_aBlasts, 0, sizeof(hits_aBlasts));
    hits_nextBlast = 0;
    hits_numOutbox = 0;
    hits_bApplying = 0;
    hits_bForce = 0;
}

int stdComm_WSRelay_ClampReach(int reachMs)
{
    if (reachMs < STDCOMM_WSRELAY_REACH_MIN)
        return STDCOMM_WSRELAY_REACH_MIN;
    if (reachMs > STDCOMM_WSRELAY_REACH_MAX)
        return STDCOMM_WSRELAY_REACH_MAX;
    return reachMs;
}

void stdComm_WSRelay_SetHitRules(int rules, int reachMs)
{
    hits_rules = (rules == STDCOMM_WSRELAY_HITS_HWYS) ? STDCOMM_WSRELAY_HITS_HWYS : STDCOMM_WSRELAY_HITS_CLASSIC;
    hits_reachMs = stdComm_WSRelay_ClampReach(reachMs);
}

int stdComm_WSRelay_GetHitRules(int* pReachMs)
{
    if (pReachMs)
        *pReachMs = hits_reachMs;
    return hits_rules;
}

// ---- What made a hit.

static int hits_BlastWeapon(SithThing* pExplosion, int* pbPart)
{
    for (int i = 0; i < HITS_BLASTS; i++)
    {
        if (hits_aBlasts[i].weaponGuid && hits_aBlasts[i].idx == (int)pExplosion->idx && hits_aBlasts[i].signature == pExplosion->signature)
        {
            if (pbPart)
                *pbPart = hits_aBlasts[i].bPart;
            return hits_aBlasts[i].weaponGuid;
        }
    }
    return 0;
}

static void hits_NoteBlast(SithThing* pExplosion, SithThing* pWeapon, int bPart)
{
    if (!pExplosion || !pWeapon)
        return;
    hits_aBlasts[hits_nextBlast].idx = pExplosion->idx;
    hits_aBlasts[hits_nextBlast].signature = pExplosion->signature;
    hits_aBlasts[hits_nextBlast].weaponGuid = pWeapon->guid;
    hits_aBlasts[hits_nextBlast].bPart = bPart;
    hits_nextBlast = (hits_nextBlast + 1) % HITS_BLASTS;
}

void stdComm_WSRelay_NoteBlast(SithThing* pExplosion, SithThing* pWeapon)
{
    hits_NoteBlast(pExplosion, pWeapon, 0);
}

// The shot whose blast sithWeapon_CreateWeaponExplosion is making, while it
// makes it.
static SithThing* hits_pBlastShot = NULL;

SithThing* stdComm_WSRelay_MakingBlast(SithThing* pWeapon)
{
    SithThing* pWas = hits_pBlastShot;
    hits_pBlastShot = pWeapon;
    return pWas;
}

void stdComm_WSRelay_NoteBlastPart(SithThing* pThing)
{
    if (hits_pBlastShot && pThing && pThing->type == SITH_THING_EXPLOSION)
        hits_NoteBlast(pThing, hits_pBlastShot, 1);
}

int stdComm_WSRelay_BlastShot(SithThing* pExplosion)
{
    return pExplosion ? hits_BlastWeapon(pExplosion, NULL) : 0;
}

// Whose attack pSource is, if it's a player's swing, shot or blast.
static int hits_Classify(SithThing* pSource, int damageType, WSRelayHitsAttack* a)
{
    memset(a, 0, sizeof(*a));
    a->maker = -1;
    if (pSource->type == SITH_THING_PLAYER && (damageType & SITH_DAMAGE_SABER))
    {
        a->source = WSRELAYHIT_SABER;
        a->maker = hits_SlotOf(pSource);
        a->attack = stdComm_WSRelay_GetSwing(pSource);
        a->pCredit = pSource;
    }
    else if (pSource->type == SITH_THING_WEAPON)
    {
        a->source = WSRELAYHIT_SHOT;
        a->maker = hits_MakerOf(pSource->guid);
        a->attack = (uint32_t)pSource->guid;
        a->pCredit = sithThing_GetThingParent(pSource);
    }
    else if (pSource->type == SITH_THING_EXPLOSION)
    {
        int guid = hits_BlastWeapon(pSource, &a->bPart);
        if (!guid)
            return 0;
        a->source = WSRELAYHIT_SPLASH;
        a->maker = hits_MakerOf(guid);
        a->attack = (uint32_t)guid;
        a->pCredit = sithThing_GetThingParent(pSource);
        a->blast = hits_Vec(&pSource->position);
    }
    else
    {
        return 0;
    }

    if (a->maker < 0 || !a->attack || !a->pCredit || a->pCredit->type != SITH_THING_PLAYER)
        return 0;
    a->kind = stdComm_WSRelay_HitKind(pSource, damageType);
    return 1;
}

// The attack number the hit ledger knows a hit by: the swing's, or the
// shot's, as stdComm_WSRelay_NoteHitBy has it.
static uint32_t hits_LedgerAttack(int kind, uint32_t attack)
{
    return (kind == STDCOMM_WSRELAY_KIND_SABER || kind == STDCOMM_WSRELAY_KIND_SHOT) ? attack : 0;
}

// Whether this game and that player's play attack a by claims: an explosion
// a blast made in turn only when both say version 3. An older game decides
// those for its own player and never claims them (see the top).
static int hits_ClaimedWith(WSRelayHitsPeer* p, const WSRelayHitsAttack* a)
{
    return !a->bPart || (p && p->version >= HITS_VERSION_PARTS);
}

// ---- Other players' attacks, as their messages tell this game.

void stdComm_WSRelay_NoteSwing(SithThing* pPlayer, uint32_t swing, flex_t damage, flex_t bladeLength)
{
    if (!swing || !pPlayer || !pPlayer->playerInfo)
        return;
    WSRelayHitsPeer* p = hits_Peer(hits_SlotOf(pPlayer));
    if (!p)
        return;
    // Its message is noted when it comes, and again when it plays on the
    // timeline (stdComm_WSRelay_HoldForTimeline).
    for (int i = 0; i < HITS_SWINGS; i++)
    {
        if (p->aSwings[i].swing == swing)
            return;
    }
    WSRelayHitsSwing* s = &p->aSwings[p->nextSwing];
    s->swing = swing;
    s->at = hits_Now();
    s->damage = (float)damage;
    s->bladeLength = (float)bladeLength;
    p->nextSwing = (p->nextSwing + 1) % HITS_SWINGS;
}

void stdComm_WSRelay_NoteShot(SithThing* pTemplate, const rdVector3* pFirePos, int guid, flex_t scale, int scaleFlags, flex_t secsAhead)
{
    if (!pTemplate || !pFirePos || pTemplate->type != SITH_THING_WEAPON)
        return;
    WSRelayHitsPeer* p = hits_Peer(hits_MakerOf(guid));
    if (!p)
        return;

    // A charge can go off long after it's fired, so it's kept apart from the
    // shots that keep coming.
    WSRelayHitsShot* s;
    if (pTemplate->weaponParams.flags & (SITH_WF_EXPLODE|SITH_WF_PROXIMITY|SITH_WF_ATTACHFACE|SITH_WF_ATTACHTHING))
    {
        s = &p->aCharges[p->nextCharge];
        p->nextCharge = (p->nextCharge + 1) % HITS_CHARGES;
    }
    else
    {
        s = &p->aShots[p->nextShot];
        p->nextShot = (p->nextShot + 1) % HITS_SHOTS;
    }
    memset(s, 0, sizeof(*s));

    float absScale = fabsf((float)scale);
    s->guid = guid;
    // It starts as far along its path as it was fired that much earlier.
    s->at = hits_Now() - (secsAhead > 0.0 ? (double)secsAhead * 1000.0 : 0.0);
    s->from = hits_Vec(pFirePos);
    if (pTemplate->moveType == SITH_MT_PHYSICS)
    {
        s->speed = rdVector_Len3(&pTemplate->physicsParams.vel);
        if (scaleFlags & 1)
            s->speed *= absScale;
        if ((pTemplate->physicsParams.flags & SITH_PF_USEGRAVITY) && sithWorld_g_pCurrentWorld)
            s->gravity = fabsf(sithWorld_g_pCurrentWorld->gravity);
    }
    s->range = pTemplate->weaponParams.range;
    s->damage = pTemplate->weaponParams.damage;
    if (scaleFlags & 2)
        s->damage *= absScale;
    if (scaleFlags & 4)
        s->damage *= absScale;
    s->damageType = pTemplate->weaponParams.damageType;

    // Its blast: the explosion its template makes where it hits, and any
    // more that explosion's template makes in turn (creatething), as MotS's
    // flash bomb's does: a thing with a cog, which makes a narrow explosion
    // and a wide one. Each of them can hit a player once, and a claim is
    // bounded by the widest and the hardest.
    SithThing* aBlasts[2] = { pTemplate->weaponParams.pExplosionTemplate, pTemplate->weaponParams.fleshHitTemplate };
    for (int i = 0; i < 2; i++)
    {
        int blasts = 0;
        SithThing* pBlast = aBlasts[i];
        for (int depth = 0; pBlast && depth < HITS_BLAST_CHAIN; depth++, pBlast = pBlast->pCreateThingTemplate)
        {
            if (pBlast->type != SITH_THING_EXPLOSION || (depth && pBlast->explosionParams.damage <= 0.0))
                continue;
            if (pBlast->explosionParams.damage > 0.0)
                blasts++;
            if (pBlast->explosionParams.range > s->blastRange)
                s->blastRange = pBlast->explosionParams.range;
            if (pBlast->explosionParams.damage > s->blastDamage)
            {
                s->blastDamage = pBlast->explosionParams.damage;
                s->blastDamageType = pBlast->explosionParams.damageType;
            }
        }
        if (blasts > s->blasts)
            s->blasts = blasts;
    }
}

static WSRelayHitsShot* hits_FindShot(WSRelayHitsPeer* p, int guid)
{
    for (int i = 0; i < HITS_SHOTS; i++)
    {
        if (p->aShots[i].guid == guid && p->aShots[i].at > 0.0)
            return &p->aShots[i];
    }
    for (int i = 0; i < HITS_CHARGES; i++)
    {
        if (p->aCharges[i].guid == guid && p->aCharges[i].at > 0.0)
            return &p->aCharges[i];
    }
    return NULL;
}

// How many times an attack can hit this game's player: once, or a blast as
// many times as it has explosions (stdComm_WSRelay_NoteShot).
static int hits_TimesHits(WSRelayHitsPeer* p, int slot, int source, uint32_t attack)
{
    WSRelayHitsShot* s = NULL;
    if (source == WSRELAYHIT_SPLASH && hits_MakerOf((int)attack) == slot && p->version >= HITS_VERSION_PARTS)
        s = hits_FindShot(p, (int)attack);
    return s && s->blasts > 1 ? s->blasts : 1;
}

static int hits_TurnedBy(WSRelayHitsPeer* p, int guid, int blockerGuid)
{
    for (int i = 0; i < HITS_TURNS; i++)
    {
        if (p->aTurns[i].guid == guid && p->aTurns[i].blockerGuid == blockerGuid && guid)
            return 1;
    }
    return 0;
}

void stdComm_WSRelay_NotePos(SithThing* pThing, int fromNetId)
{
    WSRelayHitsPeer* p = hits_Peer(hits_SlotOf(pThing));
    if (!p || p->netId != fromNetId)
        return;
    uint32_t seq = 0;
    p->bPosSeq = stdComm_WSRelay_RecvSeq(&seq);
    p->posSeq = seq;
    p->posAt = hits_Now();
}

// ---- Hits.

int stdComm_WSRelay_ClaimHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType)
{
    if (hits_bApplying || !pVictim || !pSource || pVictim->type != SITH_THING_PLAYER)
        return 0;
    // Targeted Force powers, and the host's damage messages: their messages
    // decide, as before (a Force power's once stdComm_WSRelay_ForceTakes has
    // let it play).
    if (sithComm_msgInProgress == DSS_SENDTRIGGER || sithComm_msgInProgress == DSS_DAMAGE)
        return 0;
    if (!hits_Live())
        return 0;

    WSRelayHitsAttack a;
    if (!hits_Classify(pSource, damageType, &a))
        return 0;

    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    double now = hits_Now();
    if (pVictim == pLocal)
    {
        if (a.maker == (int)playerThingIdx)
            return 0;
        // An explosion made in turn, from a game older than version 3: this
        // one decides it, as before, since that game never claims those.
        WSRelayHitsPeer* pm = hits_Peer(a.maker);
        if (pm && pm->bHello && !hits_ClaimedWith(pm, &a))
            return 0;
        // That game decides, or its claim for this attack counted already:
        // it can start sending claims a moment before this game hears so.
        if (hits_DecidedBy(pm) || stdComm_WSRelayHit_Seen(&hits_claimed, now, a.maker, a.source, a.attack))
            return 1;
        // That game doesn't send claims, or not yet: this one decides, as
        // before, and a claim for the same attack would count it twice.
        if (hits_IsAlive(pLocal))
            stdComm_WSRelayHit_AddSeen(&hits_seen, now, a.maker, a.source, a.attack);
        return 0;
    }

    if (a.maker != (int)playerThingIdx || !hits_IsShownAlive(pVictim))
        return 0;
    WSRelayHitsPeer* pv = hits_Peer(hits_SlotOf(pVictim));
    if (!hits_ClaimsTo(pv) || !hits_ClaimedWith(pv, &a))
        return 0;

    // The moment this game drew them at, if it drew them from their timed
    // updates.
    double r = 0.0, tBefore = 0.0, tAfter = -1.0;
    int carried = 0;
    int bMoment = pv->version >= 2 && stdComm_WSRelay_MotionMoment(pVictim, &r, &tBefore, &tAfter, &carried);

    uint8_t body[HITS_MAX_BODY];
    WSRelayHitsOut o = { body, 0 };
    double age = pv->bPosSeq ? now - pv->posAt : 0.0;
    hits_PutU8(&o, HITS_CLAIM);
    hits_PutU8(&o, a.source);
    hits_PutU8(&o, a.kind);
    hits_PutU8(&o, (pv->bPosSeq ? HITS_CLAIM_SEQ : 0) | (bMoment ? HITS_CLAIM_MOMENT : 0) | (bMoment && carried ? HITS_CLAIM_CARRIED : 0));
    hits_PutF32(&o, (float)damage);
    hits_Put32(&o, (uint32_t)damageType);
    hits_Put32(&o, a.attack);
    hits_Put32(&o, pVictim->guid);
    hits_Put32(&o, a.pCredit->guid);
    hits_PutVec(&o, hits_Vec(&pVictim->position));
    hits_PutVec(&o, hits_Vec(pLocal ? &pLocal->position : &pVictim->position));
    hits_PutVec(&o, a.blast);
    hits_Put32(&o, pv->posSeq);
    hits_PutU16(&o, age < 0.0 ? 0 : age > 65535.0 ? 65535 : (uint32_t)age);
    hits_PutU16(&o, 0);
    if (bMoment)
        hits_PutMoment(&o, r, tBefore, tAfter);
    hits_Queue(pv->netId, body, o.n);

    // As before, the hit shows here; other players never take damage here.
    return 0;
}

static void hits_TakeClaim(int fromNetId, int slot, WSRelayHitsIn* in, double now)
{
    WSRelayHitClaim claim;
    claim.source = hits_GetU8(in);
    int kind = hits_GetU8(in);
    int flags = hits_GetU8(in);
    claim.damage = hits_GetF32(in);
    int damageType = (int)hits_Get32(in);
    uint32_t attack = hits_Get32(in);
    int victimGuid = (int)hits_Get32(in);
    int creditGuid = (int)hits_Get32(in);
    claim.target = hits_GetVec(in);
    claim.attacker = hits_GetVec(in);
    claim.blast = hits_GetVec(in);
    uint32_t posSeq = hits_Get32(in);
    hits_GetU16(in);
    hits_GetU16(in);
    double r = 0.0;
    int before = 0, after = 0;
    if (flags & HITS_CLAIM_MOMENT)
    {
        r = hits_GetF64(in);
        before = (int)hits_GetU16(in);
        after = (int)hits_GetU16(in);
    }
    if (in->bad)
        return;

    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    WSRelayHitsPeer* p = hits_Peer(slot);
    if (!p || !pLocal || victimGuid != (int)pLocal->guid)
        return;
    if (kind < STDCOMM_WSRELAY_KIND_SABER || kind > STDCOMM_WSRELAY_KIND_OTHER)
        kind = STDCOMM_WSRELAY_KIND_OTHER;
    uint32_t ledgerAttack = hits_LedgerAttack(kind, attack);

    WSRelayHitVerdict v;
    memset(&v, 0, sizeof(v));
    v.result = WSRELAYHIT_OK;
    v.distance = -1.0f;

    WSRelayHitAttack at;
    memset(&at, 0, sizeof(at));
    SithThing* pCredit = sithThing_GetGuidThing(creditGuid);
    SithThing* pSender = jkPlayer_playerInfos[slot].pLocalPlayer;
    int applyType = damageType;

    if (!hits_Live() || !p->bHello || p->rules != STDCOMM_WSRELAY_HITS_HWYS)
    {
        v.result = STDCOMM_WSRELAY_HITS_NOT_PLAYING;
    }
    else if (stdComm_WSRelayHit_Seen(&hits_seen, now, slot, claim.source, attack) >= hits_TimesHits(p, slot, claim.source, attack))
    {
        v.result = WSRELAYHIT_REPEAT;
    }
    else if (!pCredit || pCredit->type != SITH_THING_PLAYER || !pSender)
    {
        v.result = WSRELAYHIT_BAD;
    }
    else if (claim.source == WSRELAYHIT_SABER)
    {
        // Only the swinging player's own game claims for a swing.
        if (pCredit != pSender)
        {
            v.result = WSRELAYHIT_BAD;
        }
        else
        {
            for (int i = 0; i < HITS_SWINGS; i++)
            {
                WSRelayHitsSwing* s = &p->aSwings[i];
                if (s->swing != attack || s->at <= 0.0)
                    continue;
                at.known = 1;
                at.at = s->at;
                at.maxDamage = s->damage;
                at.bladeLength = s->bladeLength;
                at.from = hits_Vec(&pSender->position);
                break;
            }
            applyType = SITH_DAMAGE_SABER;
        }
    }
    else if (claim.source == WSRELAYHIT_SHOT || claim.source == WSRELAYHIT_SPLASH)
    {
        // A shot is the shooter's game's to claim, and counts for the
        // shooter, or for the player whose saber turned it away.
        WSRelayHitsShot* s = hits_MakerOf((int)attack) == slot ? hits_FindShot(p, (int)attack) : NULL;
        if (pCredit != pSender && !hits_TurnedBy(p, (int)attack, creditGuid))
        {
            v.result = WSRELAYHIT_BAD;
        }
        else if (s)
        {
            at.known = 1;
            at.at = s->at;
            at.from = s->from;
            at.speed = s->speed;
            at.range = s->range;
            at.gravity = s->gravity;
            if (claim.source == WSRELAYHIT_SHOT)
            {
                at.maxDamage = s->damage;
                applyType = s->damageType;
            }
            else
            {
                at.maxDamage = s->blastDamage;
                at.blastRange = s->blastRange;
                applyType = s->blastDamageType;
            }
        }
    }

    // The record is on the motion clock (stdComm_WSRelayMotion.c), which
    // runs up to a frame behind stdComm_WSRelay_Now's, as now and the times
    // updates were sent are.
    double clock = stdComm_WSRelay_MotionNow();
    int moment = (flags & HITS_CLAIM_MOMENT) ? STDCOMM_WSRELAY_MOMENT_AT : STDCOMM_WSRELAY_MOMENT_NONE;
    // How far back that moment is, which the reach limits (-1: none named).
    float ageMs = moment ? (float)(clock - r) : -1.0f;
    if (v.result == WSRELAYHIT_OK)
    {
        // Where this game's player was at that moment: between the updates
        // the attacker's game drew it from, or from the one before to the
        // moment, when it was carried on past the newest.
        double from = 0.0, to = clock;
        int window = WSRELAYHIT_LATE;
        if (moment)
            window = stdComm_WSRelayHit_MomentWindow(clock, hits_reachMs, r, before, after == 0xFFFF ? -1.0 : after, &from, &to);

        // No moment: anywhere since the target's update the attacker had on
        // screen was sent, as Phase 2 checks. A moment further back than the
        // reach: anywhere in the reach. Never further back.
        if (window == WSRELAYHIT_LATE)
        {
            double tSent = -1.0;
            if (moment)
                moment = STDCOMM_WSRELAY_MOMENT_PAST_REACH;
            else if (flags & HITS_CLAIM_SEQ)
                tSent = stdComm_WSRelay_SeqSentAt(fromNetId, posSeq);
            from = stdComm_WSRelayHit_WindowFrom(clock, hits_reachMs, tSent < 0.0 ? -1.0 : tSent - (now - clock));
            to = clock;
            window = WSRELAYHIT_OK;
        }

        if (window == WSRELAYHIT_OK)
            v = stdComm_WSRelayHit_CheckIn(&hits_record, hits_life, hits_IsAlive(pLocal), &claim, &at, now, from, to);
        else
            v.result = window;
    }

    if (v.result == WSRELAYHIT_OK)
    {
        stdComm_WSRelayHit_AddSeen(&hits_seen, now, slot, claim.source, attack);
        stdComm_WSRelayHit_AddSeen(&hits_claimed, now, slot, claim.source, attack);
        hits_bApplying = 1;
        hits_applyKind = kind;
        hits_applyAttack = ledgerAttack;
        hits_applyDistance = v.distance;
        hits_applyMoment = moment;
        hits_applyAgeMs = ageMs;
        sithThing_DamageThing(pLocal, pCredit, v.damage, applyType);
        hits_bApplying = 0;
        return;
    }

    stdComm_WSRelay_NoteTurnedDown(fromNetId, kind, v.result == WSRELAYHIT_DEAD ? STDCOMM_WSRELAY_DEAD : STDCOMM_WSRELAY_TURNED_DOWN,
                                   claim.damage, ledgerAttack, v.result, v.distance, moment, ageMs);
}

int stdComm_WSRelay_ApplyingClaim(int* pKind, uint32_t* pAttack, float* pDistance, int* pMoment, float* pAgeMs)
{
    if (!hits_bApplying)
        return 0;
    *pKind = hits_applyKind;
    *pAttack = hits_applyAttack;
    *pDistance = hits_applyDistance;
    *pMoment = hits_applyMoment;
    *pAgeMs = hits_applyAgeMs;
    return 1;
}

int stdComm_WSRelay_ClaimsTo(SithThing* pPlayer)
{
    return hits_ClaimsTo(hits_Peer(hits_SlotOf(pPlayer)));
}

int stdComm_WSRelay_DecidedElsewhere(SithThing* pSource, int damageType)
{
    WSRelayHitsAttack a;
    if (!pSource || !hits_Live() || sithComm_msgInProgress == DSS_SENDTRIGGER || sithComm_msgInProgress == DSS_DAMAGE)
        return 0;
    if (!hits_Classify(pSource, damageType, &a) || a.maker == (int)playerThingIdx)
        return 0;
    WSRelayHitsPeer* pm = hits_Peer(a.maker);
    return hits_ClaimedWith(pm, &a)
           && (hits_DecidedBy(pm) || stdComm_WSRelayHit_Seen(&hits_claimed, hits_Now(), a.maker, a.source, a.attack));
}

// ---- Targeted Force powers.

int stdComm_WSRelay_ForceMoment(SithThing* pVictim, uint8_t* pOut)
{
    if (!pVictim || pVictim == sithPlayer_g_pLocalPlayerThing || !hits_IsShownAlive(pVictim))
        return 0;
    WSRelayHitsPeer* pv = hits_Peer(hits_SlotOf(pVictim));
    double r = 0.0, tBefore = 0.0, tAfter = -1.0;
    int carried = 0;
    if (!hits_ClaimsTo(pv) || pv->version < 2 || !stdComm_WSRelay_MotionMoment(pVictim, &r, &tBefore, &tAfter, &carried))
        return 0;

    WSRelayHitsOut o = { pOut, 0 };
    hits_PutMoment(&o, r, tBefore, tAfter);
    hits_PutVec(&o, hits_Vec(&pVictim->position));
    return o.n;
}

int stdComm_WSRelay_ForceTakes(SithThing* pVictim, SithThing* pSource, const uint8_t* pData)
{
    hits_bForce = 0;

    // Not on this game's player, or from a game it doesn't play Hit what you
    // see with: as before. Its player dead: its class cog does nothing, as
    // before.
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    WSRelayHitsPeer* p = hits_Peer(hits_SlotOf(pSource));
    if (!pLocal || pVictim != pLocal || !p || !hits_Live() || !p->bHello || p->rules != STDCOMM_WSRELAY_HITS_HWYS
        || !hits_IsAlive(pLocal))
        return 1;

    WSRelayHitsIn in = { pData, 0, STDCOMM_WSRELAY_FORCE_BYTES, 0 };
    double r = hits_GetF64(&in);
    int before = (int)hits_GetU16(&in);
    int after = (int)hits_GetU16(&in);
    WSRelayHitVec target = hits_GetVec(&in);

    // Was the player where that game showed them, at the moment it drew them
    // at, or, for one further back than the reach, anywhere in the reach: as
    // a claim (hits_TakeClaim), on the motion clock.
    double clock = stdComm_WSRelay_MotionNow();
    double from = 0.0, to = clock;
    int moment = STDCOMM_WSRELAY_MOMENT_AT;
    WSRelayHitVerdict v;
    memset(&v, 0, sizeof(v));
    v.distance = -1.0f;
    v.result = stdComm_WSRelayHit_MomentWindow(clock, hits_reachMs, r, before, after == 0xFFFF ? -1.0 : after, &from, &to);
    if (v.result == WSRELAYHIT_LATE)
    {
        moment = STDCOMM_WSRELAY_MOMENT_PAST_REACH;
        from = stdComm_WSRelayHit_WindowFrom(clock, hits_reachMs, -1.0);
        to = clock;
        v.result = WSRELAYHIT_OK;
    }
    if (v.result == WSRELAYHIT_OK)
        v = stdComm_WSRelayHit_CheckThere(&hits_record, hits_life, target, from, to);

    stdComm_WSRelay_NoteTargeted(p->netId, v.result == WSRELAYHIT_OK ? STDCOMM_WSRELAY_HIT : STDCOMM_WSRELAY_TURNED_DOWN,
                                 v.result, v.distance, moment, (float)(clock - r));
    hits_bForce = v.result == WSRELAYHIT_OK;
    return hits_bForce;
}

void stdComm_WSRelay_ForceDone()
{
    hits_bForce = 0;
}

int stdComm_WSRelay_ApplyingForce()
{
    return hits_bForce;
}

// ---- Bolts turned away, and charges.

int stdComm_WSRelay_BlockDecidedElsewhere(SithThing* pWeapon, SithThing* pBlocker)
{
    WSRelayHitsAttack a;
    if (!pWeapon || !pBlocker || pBlocker->type != SITH_THING_PLAYER || !hits_Live())
        return 0;
    if (!hits_Classify(pWeapon, pWeapon->weaponParams.damageType, &a) || a.maker == (int)playerThingIdx)
        return 0;
    WSRelayHitsPeer* p = hits_Peer(a.maker);
    if (pBlocker == sithPlayer_g_pLocalPlayerThing)
        return hits_DecidedBy(p);
    // Between two other players: the game that fired it says where it goes.
    return p && p->bHello && p->rules == STDCOMM_WSRELAY_HITS_HWYS;
}

int stdComm_WSRelay_InFireWait(SithThing* pPlayer)
{
    if (!pPlayer || pPlayer == sithPlayer_g_pLocalPlayerThing || !hits_Live())
        return 0;
    WSRelayHitsPeer* p = hits_Peer(hits_SlotOf(pPlayer));
    if (!p)
        return 0;
    // Drawn from their timed updates: whether they were in it at the moment
    // they're drawn at.
    double r, tBefore, tAfter;
    int carried;
    if (p->bWaitMoment && stdComm_WSRelay_MotionMoment(pPlayer, &r, &tBefore, &tAfter, &carried))
        return r >= p->waitFrom && r < p->waitTo;
    return p->waitUntil > hits_Now();
}

static void hits_TakeWait(int slot, WSRelayHitsIn* in)
{
    hits_GetU8(in);
    int waitMs = (int)hits_GetU16(in);
    double from = in->len - in->n >= 8 ? hits_GetF64(in) : NAN;
    WSRelayHitsPeer* p = hits_Peer(slot);
    if (in->bad || !p)
        return;
    p->waitUntil = hits_Now() + waitMs;
    p->bWaitMoment = isfinite(from);
    p->waitFrom = p->bWaitMoment ? from : 0.0;
    p->waitTo = p->bWaitMoment ? from + waitMs : 0.0;
}

// This game's copy of a shot, if it's still flying here.
static SithThing* hits_FindLiveShot(int guid)
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (!pWorld || !pWorld->aThings || guid <= 0)
        return NULL;
    for (int i = 0; i <= pWorld->numThings; i++)
    {
        SithThing* pThing = &pWorld->aThings[i];
        if (pThing->guid == guid && pThing->type == SITH_THING_WEAPON && !(pThing->flags & SITH_TF_DESTROYED))
            return pThing;
    }
    return NULL;
}

void stdComm_WSRelay_BoltTurned(SithThing* pWeapon, SithThing* pBlocker)
{
    if (!pWeapon || !pBlocker || !pWeapon->sector || !hits_Live() || hits_MakerOf(pWeapon->guid) != (int)playerThingIdx)
        return;

    uint8_t body[HITS_MAX_BODY];
    WSRelayHitsOut o = { body, 0 };
    hits_PutU8(&o, HITS_TURNED);
    hits_PutU8(&o, 0);
    hits_PutU16(&o, pWeapon->pTemplate ? (uint32_t)(uint16_t)pWeapon->pTemplate->idx : 0xFFFF);
    hits_Put32(&o, pWeapon->guid);
    hits_Put32(&o, pBlocker->guid);
    hits_PutU16(&o, (uint32_t)(uint16_t)pWeapon->sector->id);
    hits_PutU16(&o, 0);
    hits_PutVec(&o, hits_Vec(&pWeapon->position));
    hits_PutVec(&o, hits_Vec(&pWeapon->physicsParams.vel));
    hits_Queue(-1, body, o.n);
}

static void hits_TakeTurned(int slot, WSRelayHitsIn* in)
{
    hits_GetU8(in);
    int templateIdx = hits_GetS16(in);
    int guid = (int)hits_Get32(in);
    int blockerGuid = (int)hits_Get32(in);
    int sectorId = hits_GetS16(in);
    hits_GetU16(in);
    WSRelayHitVec pos = hits_GetVec(in);
    WSRelayHitVec vel = hits_GetVec(in);
    if (in->bad || hits_MakerOf(guid) != slot)
        return;
    WSRelayHitsPeer* p = hits_Peer(slot);
    if (!p)
        return;
    p->aTurns[p->nextTurn].guid = guid;
    p->aTurns[p->nextTurn].blockerGuid = blockerGuid;
    p->nextTurn = (p->nextTurn + 1) % HITS_TURNS;
    if (!hits_Live())
        return;

    // This game's copy takes the bolt's new path. If it burst on the blocker
    // here, it's made again.
    SithSector* pSector = sithSector_GetPtrFromIdx(sectorId);
    rdVector3 position = { pos.x, pos.y, pos.z };
    rdVector3 velocity = { vel.x, vel.y, vel.z };
    rdVector3 dir;
    if (!pSector || !isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) || rdVector_Normalize3(&dir, &velocity) <= 0.0)
        return;
    rdMatrix34 orient;
    rdMatrix_BuildFromLook34(&orient, &dir);

    SithThing* pBlocker = sithThing_GetGuidThing(blockerGuid);
    SithThing* pShot = hits_FindLiveShot(guid);
    if (pShot)
    {
        pShot->position = position;
        sithThing_SetSector(pShot, pSector, 0);
        pShot->orient.rvec = orient.rvec;
        pShot->orient.lvec = orient.lvec;
        pShot->orient.uvec = orient.uvec;
    }
    else
    {
        SithThing* pTemplate = sithTemplate_GetTemplateByIndex(templateIdx);
        if (!pTemplate || pTemplate->type != SITH_THING_WEAPON)
            return;
        pShot = sithThing_CreateThingAtPos(pTemplate, &position, &orient, pSector, pBlocker);
        if (!pShot)
            return;
        pShot->guid = guid;
        pShot->flags |= SITH_TF_INVULN;
    }
    if (pShot->moveType == SITH_MT_PHYSICS)
        pShot->physicsParams.vel = velocity;
    if (pBlocker)
    {
        pShot->pParent = pBlocker;
        pShot->parentSignature = pBlocker->signature;
    }
    // As sithActor_thing_anim_blocked leaves it: it can hit whoever fired it.
    pShot->weaponParams.flags &= ~SITH_WF_NOSHOOTERDAMAGE;

    // And what it shows.
    sithSoundClass_PlayModeRandom(pShot, SITH_SC_DEFLECTED);
    if (pBlocker && (pBlocker->type == SITH_THING_PLAYER || pBlocker->type == SITH_THING_ACTOR))
    {
        sithPuppet_PlayMode(pBlocker, SITH_ANIM_BLOCK, 0);
        if (pBlocker == sithPlayer_g_pLocalPlayerThing)
            sithCog_ThingSendMessage(pBlocker, 0, SITH_MESSAGE_BLOCKED);
    }
}

static void hits_Arm(SithThing* pWeapon, int fuseMs)
{
    // As sithWeapon_ThingCollisionHandler arms a mine, and
    // sithWeapon_DamageWeapon a charge.
    if (pWeapon->weaponParams.flags & SITH_WF_PROXIMITY)
    {
        pWeapon->weaponParams.flags &= ~SITH_WF_PROXIMITY;
        pWeapon->weaponParams.flags |= SITH_WF_EXPLODE;
        pWeapon->collideSize = 0.0;
        sithSoundClass_PlayModeFirst(pWeapon, SITH_SC_ACTIVATE);
    }
    if (!pWeapon->msecLifeLeft || pWeapon->msecLifeLeft > fuseMs)
        pWeapon->msecLifeLeft = fuseMs;
}

int stdComm_WSRelay_ArmCharge(SithThing* pWeapon, SithThing* pBy, int fuseMs)
{
    if (!pWeapon || !hits_Live())
        return 1;
    int maker = hits_MakerOf(pWeapon->guid);
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    int bMine = maker == (int)playerThingIdx
             || (pBy && pLocal && (pBy == pLocal || sithThing_GetThingParent(pBy) == pLocal));
    if (!bMine)
    {
        // Set off by something else here: the game that fired it says when,
        // if it plays by these rules. If not, each game does, as before.
        WSRelayHitsPeer* p = hits_Peer(maker);
        return !(p && p->bHello && p->rules == STDCOMM_WSRELAY_HITS_HWYS);
    }

    // Every game sets it off now. Once is enough.
    if ((pWeapon->weaponParams.flags & SITH_WF_PROXIMITY) || !pWeapon->msecLifeLeft || pWeapon->msecLifeLeft > fuseMs)
    {
        uint8_t body[8];
        WSRelayHitsOut o = { body, 0 };
        hits_PutU8(&o, HITS_DETONATE);
        hits_PutU8(&o, 0);
        hits_PutU16(&o, (uint32_t)fuseMs);
        hits_Put32(&o, pWeapon->guid);
        hits_Queue(-1, body, o.n);
    }
    return 1;
}

static void hits_TakeDetonate(WSRelayHitsIn* in)
{
    hits_GetU8(in);
    int fuseMs = (int)hits_GetU16(in);
    int guid = (int)hits_Get32(in);
    if (in->bad || !hits_Live())
        return;
    SithThing* pWeapon = sithThing_GetGuidThing(guid);
    if (!pWeapon || pWeapon->type != SITH_THING_WEAPON || (pWeapon->flags & SITH_TF_DESTROYED))
        return;
    hits_Arm(pWeapon, fuseMs);
}

// ---- Shots start where their shooter was.

int stdComm_WSRelay_ShotStart(SithThing* pShooter, int guid, const rdVector3* pShooterPos, rdVector3* pOutPos, SithSector** ppOutSector)
{
    if (!pShooter || !pShooterPos || !hits_Live() || !sithWorld_g_pCurrentWorld)
        return 0;
    WSRelayHitsPeer* p = hits_Peer(hits_MakerOf(guid));
    if (!p || !p->bHello || p->rules != STDCOMM_WSRELAY_HITS_HWYS)
        return 0;
    if (!isfinite(pShooterPos->x) || !isfinite(pShooterPos->y) || !isfinite(pShooterPos->z))
        return 0;
    if (rdVector_Dist3(&pShooter->position, pShooterPos) <= STDCOMM_WSRELAY_SHOT_START_SLACK)
        return 0;
    rdVector3 pos = *pShooterPos;
    SithSector* pSector = sithSector_FindSectorAtPos(sithWorld_g_pCurrentWorld, &pos);
    if (!pSector)
        return 0;
    *pOutPos = pos;
    *ppOutSector = pSector;
    return 1;
}

// ---- The record, and motion.

void stdComm_WSRelay_RecordOwn(double t)
{
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    if (!pLocal || !stdComm_WSRelay_InMatch())
        return;

    // A new level: nothing from the last one applies.
    if (hits_pWorld != sithWorld_g_pCurrentWorld)
    {
        hits_pWorld = sithWorld_g_pCurrentWorld;
        stdComm_WSRelayHit_ClearRecord(&hits_record);
        stdComm_WSRelayHit_ClearSeen(&hits_seen);
        stdComm_WSRelayHit_ClearSeen(&hits_claimed);
        memset(hits_aBlasts, 0, sizeof(hits_aBlasts));
        hits_life++;
        hits_bWasAlive = 0;
        hits_recordAt = -1.0;
    }

    // To the ms, as this game's timed updates name it.
    t = floor(t + 0.5);
    if (t <= hits_recordAt)
        return;
    hits_recordAt = t;

    // Each death starts a new life.
    int bAlive = hits_IsAlive(pLocal);
    if (hits_bWasAlive && !bAlive)
        hits_life++;
    hits_bWasAlive = bAlive;
    stdComm_WSRelayHit_Record(&hits_record, t, hits_Vec(&pLocal->position), hits_life, bAlive);
}

int stdComm_WSRelay_MotionWith(int slot)
{
    WSRelayHitsPeer* p = hits_Peer(slot);
    return p && p->version >= 2 && hits_ClaimsTo(p);
}

int stdComm_WSRelay_HelloWith(int slot)
{
    WSRelayHitsPeer* p = hits_Peer(slot);
    return p && p->bHello ? p->version : 0;
}

// ---- Each frame, and each message.

static void hits_SendHello(WSRelayHitsPeer* p, double now)
{
    uint8_t body[8];
    WSRelayHitsOut o = { body, 0 };
    hits_PutU8(&o, HITS_HELLO);
    hits_PutU8(&o, HITS_VERSION);
    hits_PutU8(&o, (stdComm_bIsServer ? HITS_HELLO_HOST : 0) | (p->bHello ? HITS_HELLO_HEARD : 0));
    hits_PutU8(&o, hits_rules);
    hits_PutU16(&o, hits_reachMs);
    hits_PutU16(&o, 0);
    hits_Queue(p->netId, body, o.n);
    p->helloAt = now;
    p->bAnswer = 0;
}

static void hits_TakeHello(int fromNetId, int slot, WSRelayHitsIn* in)
{
    int version = hits_GetU8(in);
    int flags = hits_GetU8(in);
    int rules = hits_GetU8(in);
    int reachMs = hits_GetU16(in);
    if (in->bad || version < 1)
        return;
    rules = (rules == STDCOMM_WSRELAY_HITS_HWYS) ? STDCOMM_WSRELAY_HITS_HWYS : STDCOMM_WSRELAY_HITS_CLASSIC;

    // The host's rules are the match's. Fixed: also a dedicated host's, whose
    // own slot is never in the game, so it isn't a peer (hits_Peer).
    if (!stdComm_bIsServer && (flags & HITS_HELLO_HOST) && fromNetId == (int)sithNet_serverNetId)
        stdComm_WSRelay_SetHitRules(rules, reachMs);

    WSRelayHitsPeer* p = hits_Peer(slot);
    if (!p)
        return;

    int bFirst = !p->bHello;
    p->bHello = 1;
    // The lower of the two: both games have to read what the other sends.
    p->version = version < HITS_VERSION ? version : HITS_VERSION;
    p->bHeardMe = !!(flags & HITS_HELLO_HEARD);
    p->rules = rules;

    // So that game knows this one heard it.
    if (bFirst || !p->bHeardMe)
        p->bAnswer = 1;
}

void stdComm_WSRelay_ProcessHits(SithMessage* pMsg)
{
    WSRelayHitsIn in = { (const uint8_t*)pMsg->pktData, 0, (int)pMsg->netMsg.msg_size, 0 };
    if (in.len <= 0 || in.len > (int)sizeof(pMsg->pktData))
        return;
    int fromNetId = (int)pMsg->netMsg.idx;
    int slot = (int)sithPlayer_GetPlayerNum(fromNetId);
    if (slot < 0 || slot >= JKPLAYER_NUM_INFOS || slot == (int)playerThingIdx)
        return;

    switch (hits_GetU8(&in))
    {
        case HITS_HELLO:
            hits_TakeHello(fromNetId, slot, &in);
            break;
        case HITS_CLAIM:
            hits_TakeClaim(fromNetId, slot, &in, hits_Now());
            break;
        case HITS_TURNED:
            hits_TakeTurned(slot, &in);
            break;
        case HITS_DETONATE:
            hits_TakeDetonate(&in);
            break;
        case HITS_WAIT:
            hits_TakeWait(slot, &in);
            break;
        default:
            break;
    }
}

void stdComm_WSRelay_HitsFrame()
{
    if (!stdComm_WSRelay_InMatch() || !sithPlayer_g_pLocalPlayerThing)
    {
        hits_numOutbox = 0;
        return;
    }
    double now = hits_Now();
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    stdComm_WSRelay_RecordOwn(stdComm_WSRelay_MotionNow());

    // A swing this frame: its fire wait, which its cog has set by now.
    uint32_t swing = stdComm_WSRelay_GetSwing(pLocal);
    if (swing != hits_ownSwing)
    {
        hits_ownSwing = swing;
        if (hits_Live())
        {
            double waitMs = ((double)sithWeapon_fireWait - (double)sithTime_g_secGameTime) * 1000.0;
            uint8_t body[12];
            WSRelayHitsOut o = { body, 0 };
            hits_PutU8(&o, HITS_WAIT);
            hits_PutU8(&o, 0);
            hits_PutU16(&o, waitMs <= 0.0 ? 0 : waitMs > 65535.0 ? 65535 : (uint32_t)waitMs);
            hits_PutF64(&o, stdComm_WSRelay_OwnSwingAt());
            hits_Queue(-1, body, o.n);
        }
    }

    // Hello to each player, now and then, and at once to a new one.
    for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
    {
        WSRelayHitsPeer* p = hits_Peer(i);
        if (!p)
            continue;
        if (!p->helloAt || now - p->helloAt >= HITS_HELLO_MS || (p->bAnswer && now - p->helloAt >= HITS_ANSWER_MS))
            hits_SendHello(p, now);
    }

    stdComm_WSRelay_FlushHits();
}
