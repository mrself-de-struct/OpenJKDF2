#ifndef _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAYPLAYOUT_H
#define _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAYPLAYOUT_H

// Drawing another player from a short buffer of their position updates, in a
// match played by Hit what you see (stdComm_WSRelayMotion.c). Each update
// carries the time it was made on its sender's clock. This game draws the
// player a little behind the newest update, at the place the updates give for
// that moment, so the player moves smoothly along the path their own game had
// them on and every hit this game sees has an exact moment on their clock.
// This file has no engine state, so it can be tested on its own
// (packaging/wasm/test/motion-test.c). Times are in ms, positions in JKU,
// angles in degrees.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WSRelayPlayoutVec
{
    float x, y, z;
} WSRelayPlayoutVec;

// One update, as the sender's game had its player.
typedef struct WSRelayPlayoutSample
{
    double t;                   // when, on the sender's clock
    double taken;               // when this game took it, on the clock poses
                                // are drawn on (stdComm_WSRelayPlayout_Pose)
    WSRelayPlayoutVec pos;
    int sector;
    WSRelayPlayoutVec angles;   // pitch, yaw, roll
    WSRelayPlayoutVec vel;      // JKU/s
    float headPitch;
    uint32_t physFlags;
    uint32_t attachFlags;
    uint32_t warp;              // one more after each jump the sender made
} WSRelayPlayoutSample;

// About 1.3 s of updates at 50 a second.
#define WSRELAYPLAYOUT_SAMPLES (64)
// How many of the gaps between updates the delay looks at.
#define WSRELAYPLAYOUT_GAPS (16)

// How long the player is drawn on along their last velocity when the next
// update is late, before they're held still.
#define WSRELAYPLAYOUT_CARRY_MS (120.0)
// The quickest trip is the smallest delay over this long, in two halves.
#define WSRELAYPLAYOUT_BASE_MS (2000.0)
// The delay that covers late updates and the gaps between them takes the
// 95th percentile of how late the last 64 came, and the 90th of the last 16
// gaps, so a rare late one doesn't hold every other one back.
#define WSRELAYPLAYOUT_LATE_SHARE (0.95)
#define WSRELAYPLAYOUT_GAP_SHARE (0.90)
// The gap taken before two updates have come, and the longest one counted:
// one longer is the sender's game stalling, which says nothing of the next.
#define WSRELAYPLAYOUT_GAP_FIRST_MS (50.0)
#define WSRELAYPLAYOUT_GAP_MAX_MS (250.0)
// How much of the elapsed time the delay can grow or shrink by: playback at
// 0.9 to 1.05 times real speed, which nobody sees.
#define WSRELAYPLAYOUT_GROW (0.10)
#define WSRELAYPLAYOUT_SHRINK (0.05)
// A delay this much longer than the updates ask for jumps down (a new,
// quicker route). One that has to grow only eases up: when updates run out
// first, the player holds still where they're carried to until more come,
// which grows it by as long, so the moment drawn never goes back.
#define WSRELAYPLAYOUT_JUMP_MS (200.0)
// An update this far ahead of the newest, or behind it, starts the buffer
// over: the sender's clock was set again, or it stalled that long.
#define WSRELAYPLAYOUT_RESTART_MS (3000.0)

typedef struct WSRelayPlayout
{
    WSRelayPlayoutSample aSamples[WSRELAYPLAYOUT_SAMPLES];  // oldest first,
    int first;                                              // from first
    int count;

    // Each update's taken - t: the two clocks' offset plus its trip.
    double aOffsets[WSRELAYPLAYOUT_SAMPLES];
    int nextOffset;
    int numOffsets;
    double aGaps[WSRELAYPLAYOUT_GAPS];
    int nextGap;
    int numGaps;
    double baseNow;             // the smallest offset in this half
    double basePrev;            // and in the one before
    double baseSince;           // when this half started
    int bBase;

    double target;              // the delay the updates ask for
    double delay;               // the delay drawn with
    int bDelay;
    double lastNow;             // when the last pose was asked for

    // Counts, for ?netstats.
    uint32_t poses;             // poses drawn
    uint32_t carried;           // ...on past the newest update
    uint32_t held;              // ...still, past the time it's carried on
    uint32_t restarts;
} WSRelayPlayout;

typedef struct WSRelayPlayoutPose
{
    double r;                   // the sender's time it shows
    WSRelayPlayoutVec pos;
    WSRelayPlayoutVec angles;
    WSRelayPlayoutVec vel;
    float headPitch;
    int sector;                 // the update before r's
    int sectorNext;             // the one after's, -1 if none
    double tBefore;             // the update before r (or the oldest, if r
                                // is before it)
    double tAfter;              // the one after, -1 if none
    uint32_t physFlags;         // the update before r's
    uint32_t attachFlags;
    uint32_t warp;
    int carried;                // 0 between two updates, 1 carried on past the
                                // newest, 2 held still after that
} WSRelayPlayoutPose;

void stdComm_WSRelayPlayout_Clear(WSRelayPlayout* p);

// The sender's time from the low 32 bits it sends, near the newest update's.
double stdComm_WSRelayPlayout_Unwrap(const WSRelayPlayout* p, uint32_t t32);

// How long before now, on the sender's clock, the time it sent as the low
// 32 bits t32 was: the one nearest now. Negative if after.
double stdComm_WSRelayPlayout_Since(double now, uint32_t t32);

// Adds an update. 0 if it was dropped: no newer than the newest.
int stdComm_WSRelayPlayout_Add(WSRelayPlayout* p, const WSRelayPlayoutSample* pSample);

// The newest update, or NULL.
const WSRelayPlayoutSample* stdComm_WSRelayPlayout_Newest(const WSRelayPlayout* p);

// Where the updates have the player at the sender's time t: between the two
// around it, on past the newest for up to WSRELAYPLAYOUT_CARRY_MS, never
// across a warp. 0 if there are no updates.
int stdComm_WSRelayPlayout_At(const WSRelayPlayout* p, double t, WSRelayPlayoutPose* pOut);

// The pose to draw now (this game's clock), moving the delay on toward what
// the updates' times ask for. 0 if there are no updates.
int stdComm_WSRelayPlayout_Pose(WSRelayPlayout* p, double now, WSRelayPlayoutPose* pOut);

// How much later than the quickest trip this game draws the player, in ms:
// the buffer's own part of the delay.
double stdComm_WSRelayPlayout_Buffer(const WSRelayPlayout* p);

// Messages about the player that wait until the moment they're drawn at
// reaches the time their game sent each with: the swing, animation, sound
// and saber-on messages (stdComm_WSRelayMotion.c). Each is kept as bytes
// the caller packs. They play in the order they came, each when the moment
// drawn reaches its time, or once it has waited WSRELAYPLAYOUT_HOLD_MS.
#define WSRELAYPLAYOUT_HOLDS (16)
#define WSRELAYPLAYOUT_HOLD_BYTES (96)
#define WSRELAYPLAYOUT_HOLD_MS (300.0)

typedef struct WSRelayPlayoutHeld
{
    double t;                   // the sender's time
    double since;               // when it started waiting, on this game's clock
    int len;
    uint8_t data[WSRELAYPLAYOUT_HOLD_BYTES];
} WSRelayPlayoutHeld;

typedef struct WSRelayPlayoutHolds
{
    WSRelayPlayoutHeld aHeld[WSRELAYPLAYOUT_HOLDS];     // oldest first,
    int first;                                          // from first
    int count;
} WSRelayPlayoutHolds;

// Whether a message sent at t has to wait to play at moment r: when the
// moment hasn't reached it, or others are waiting ahead of it. One held with
// t at -infinity plays as soon as those ahead of it have.
int stdComm_WSRelayPlayout_MustHold(const WSRelayPlayoutHolds* h, double t, double r);

// Keeps a message to play later. 0 if it's too long, or none fits: the
// caller plays the oldest (stdComm_WSRelayPlayout_TakeHeld with now at
// infinity) and tries again.
int stdComm_WSRelayPlayout_Hold(WSRelayPlayoutHolds* h, double t, double now, const void* pData, int len);

// The oldest message that's due at moment r and this game's time now, taken
// out into *pOut. 0 if none is.
int stdComm_WSRelayPlayout_TakeHeld(WSRelayPlayoutHolds* h, double r, double now, WSRelayPlayoutHeld* pOut);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAYPLAYOUT_H
