#ifndef _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAYHITCHECK_H
#define _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAYHITCHECK_H

// The check a hit claim has to pass before its damage counts, in a match
// played by Hit what you see (stdComm_WSRelayHits.c). The game that made an
// attack says what it hit; the target's game checks that against its record
// of where its own player was. This file has no engine state, so it can be
// tested on its own (packaging/wasm/test/hitcheck-test.c) and run by anything
// that has a record and a claim. Times are in ms on one clock, distances in
// JKU.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSRelayHitVec
{
    float x, y, z;
} WSRelayHitVec;

// Where the player was at the end of a frame. Each death starts a new life.
typedef struct WSRelayHitSpot
{
    double t;
    WSRelayHitVec pos;
    uint32_t life;
    int alive;
} WSRelayHitSpot;

// About 2 s, however fast the game runs (stdComm_WSRelayHit_Record keeps
// spots at least 4 ms apart).
#define WSRELAYHIT_RECORD_LEN (512)

typedef struct WSRelayHitRecord
{
    WSRelayHitSpot aSpots[WSRELAYHIT_RECORD_LEN];
    int next;
    int count;
} WSRelayHitRecord;

void stdComm_WSRelayHit_ClearRecord(WSRelayHitRecord* pRecord);
void stdComm_WSRelayHit_Record(WSRelayHitRecord* pRecord, double t, WSRelayHitVec pos, uint32_t life, int alive);

// The closest the player came to pos between tFrom and tTo while alive in
// life, taking the path between two frames as a straight line. -1 if the
// record has no part of that life in the window. *pAt: when.
float stdComm_WSRelayHit_Closest(const WSRelayHitRecord* pRecord, double tFrom, double tTo, uint32_t life, WSRelayHitVec pos, double* pAt);

// What made the hit.
#define WSRELAYHIT_SABER (1)
#define WSRELAYHIT_SHOT (2)
#define WSRELAYHIT_SPLASH (3)

typedef struct WSRelayHitClaim
{
    int source;             // WSRELAYHIT_SABER, _SHOT or _SPLASH
    float damage;
    WSRelayHitVec target;   // where the attacker's game showed this player
    WSRelayHitVec attacker; // where the attacker was
    WSRelayHitVec blast;    // where a blast went off
} WSRelayHitClaim;

// What the target's game knows of the attack, from the attacker's swing or
// fire message.
typedef struct WSRelayHitAttack
{
    int known;              // it got that message from the claim's sender
    double at;              // when
    float maxDamage;        // the most the swing, shot or blast can do
    WSRelayHitVec from;     // a shot: where it was fired from. A swing: where
                            // this game shows the attacker now
    float bladeLength;      // a swing
    float speed;            // a shot: how fast it was fired, JKU/s
    float range;            // a shot that hits at once: how far it reaches
    float gravity;          // a shot that falls: the level's, JKU/s/s
    float blastRange;       // a blast
} WSRelayHitAttack;

// Why a claim was turned down. The receipt to the attacker says so.
#define WSRELAYHIT_OK (0)
#define WSRELAYHIT_DEAD (1)         // the player is dead
#define WSRELAYHIT_NOT_THERE (2)    // never near there in the window
#define WSRELAYHIT_LATE (3)         // nothing of this life in the window:
                                    // died since, or older than the reach
#define WSRELAYHIT_UNKNOWN (4)      // no such swing or shot from the sender
#define WSRELAYHIT_OUT_OF_REACH (5) // the attacker couldn't have reached
#define WSRELAYHIT_REPEAT (6)       // that attack already counted
#define WSRELAYHIT_BAD (7)          // nonsense numbers

// How close to the record the target has to be, and how near the attacker
// has to be to where this game shows them, for a saber.
#define WSRELAYHIT_TOLERANCE (0.2f)
#define WSRELAYHIT_SABER_SLACK (0.3f)
#define WSRELAYHIT_ATTACKER_SLACK (1.0f)
#define WSRELAYHIT_SHOT_SLACK (0.5f)
// The window starts this long before the target's update the attacker had
// on screen was sent: the frame it was made in, and the way other players'
// games steer toward each update.
#define WSRELAYHIT_UPDATE_SLACK_MS (100.0)
// A shot's flight time is taken as the time between its fire message and the
// claim arriving, which jitter can stretch.
#define WSRELAYHIT_FLIGHT_SLACK_MS (100.0)

typedef struct WSRelayHitVerdict
{
    int result;             // WSRELAYHIT_OK or why not
    float damage;           // what to apply: the claim's, at most maxDamage
    float distance;         // closest the player came to where it was shown, -1 if never
    double at;              // when
} WSRelayHitVerdict;

// The start of the window a claim is checked in: from when the target's
// update the attacker had on screen was sent (tSent, < 0 if unknown), less
// WSRELAYHIT_UPDATE_SLACK_MS, and never more than reachMs back.
double stdComm_WSRelayHit_WindowFrom(double now, double reachMs, double tSent);

// Checks a claim against the record, between windowFrom and now. The player
// is in their life `life`, alive or not (aliveNow). Whether the attack
// already counted is the caller's to say (stdComm_WSRelayHit_Seen).
WSRelayHitVerdict stdComm_WSRelayHit_Check(const WSRelayHitRecord* pRecord, uint32_t life, int aliveNow,
                                           const WSRelayHitClaim* pClaim, const WSRelayHitAttack* pAttack,
                                           double now, double windowFrom);

// As Check, between windowFrom and windowTo: a claim that names the moment
// the attacker's game drew the player at, checked between the player's
// updates it drew them from.
WSRelayHitVerdict stdComm_WSRelayHit_CheckIn(const WSRelayHitRecord* pRecord, uint32_t life, int aliveNow,
                                             const WSRelayHitClaim* pClaim, const WSRelayHitAttack* pAttack,
                                             double now, double windowFrom, double windowTo);

// Only whether the player was where another game showed them (target),
// between windowFrom and windowTo in life `life`: the last of the claim's
// checks, and all there is to check of a targeted Force power, which that
// game aimed at the player where it showed them. WSRELAYHIT_OK,
// WSRELAYHIT_NOT_THERE, WSRELAYHIT_LATE or WSRELAYHIT_BAD; no damage.
WSRelayHitVerdict stdComm_WSRelayHit_CheckThere(const WSRelayHitRecord* pRecord, uint32_t life, WSRelayHitVec target,
                                                double windowFrom, double windowTo);

// A claim that names a moment: the attacker's game drew the player at r (on
// the player's game's clock, as now is), between the player's updates
// beforeMs before it and afterMs after it (< 0: none, it carried them on
// past the newest). The window to check (CheckIn) is from the update before
// to the one after, or to r, with WSRELAYHIT_MOMENT_SLACK_MS either side for
// rounding.
//
// WSRELAYHIT_OK; WSRELAYHIT_LATE when r is further back than the reach,
// which the caller checks as a claim without a moment, never further back
// than the reach (WindowFrom); or WSRELAYHIT_BAD (r ahead of now by more
// than WSRELAYHIT_MOMENT_AHEAD_MS, or nonsense).
#define WSRELAYHIT_MOMENT_SLACK_MS (1.0)
#define WSRELAYHIT_MOMENT_AHEAD_MS (50.0)
int stdComm_WSRelayHit_MomentWindow(double now, double reachMs, double r, double beforeMs, double afterMs,
                                    double* pFrom, double* pTo);

// Attacks that already counted on this player, by attacker, kind and number,
// for 5 s. Seen: how many times one did (a blast's explosions each can).
#define WSRELAYHIT_SEEN_LEN (128)
#define WSRELAYHIT_SEEN_MS (5000.0)

typedef struct WSRelayHitSeen
{
    struct
    {
        double t;
        int attacker;
        int source;
        uint32_t attack;
    } aSeen[WSRELAYHIT_SEEN_LEN];
    int next;
} WSRelayHitSeen;

void stdComm_WSRelayHit_ClearSeen(WSRelayHitSeen* pSeen);
int stdComm_WSRelayHit_Seen(const WSRelayHitSeen* pSeen, double now, int attacker, int source, uint32_t attack);
void stdComm_WSRelayHit_AddSeen(WSRelayHitSeen* pSeen, double now, int attacker, int source, uint32_t attack);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAYHITCHECK_H
