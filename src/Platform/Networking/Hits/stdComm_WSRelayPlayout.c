#include "stdComm_WSRelayPlayout.h"

#include <math.h>
#include <string.h>

void stdComm_WSRelayPlayout_Clear(WSRelayPlayout* p)
{
    memset(p, 0, sizeof(*p));
}

static const WSRelayPlayoutSample* playout_Sample(const WSRelayPlayout* p, int i)
{
    return &p->aSamples[(p->first + i) % WSRELAYPLAYOUT_SAMPLES];
}

const WSRelayPlayoutSample* stdComm_WSRelayPlayout_Newest(const WSRelayPlayout* p)
{
    return p->count ? playout_Sample(p, p->count - 1) : NULL;
}

double stdComm_WSRelayPlayout_Unwrap(const WSRelayPlayout* p, uint32_t t32)
{
    const WSRelayPlayoutSample* pNewest = stdComm_WSRelayPlayout_Newest(p);
    if (!pNewest)
        return (double)t32;
    uint32_t newest32 = (uint32_t)(int64_t)pNewest->t;
    return pNewest->t + (double)(int32_t)(t32 - newest32);
}

double stdComm_WSRelayPlayout_Since(double now, uint32_t t32)
{
    double whole = floor(now);
    return (now - whole) - (double)(int32_t)(t32 - (uint32_t)(int64_t)whole);
}

// The share-th smallest of n values (n <= WSRELAYPLAYOUT_SAMPLES).
static double playout_Percentile(const double* pValues, int n, double share)
{
    double a[WSRELAYPLAYOUT_SAMPLES];
    memcpy(a, pValues, n * sizeof(double));
    for (int i = 1; i < n; i++)
    {
        double v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v)
        {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
    int k = (int)ceil(share * n) - 1;
    if (k < 0)
        k = 0;
    if (k >= n)
        k = n - 1;
    return a[k];
}

static double playout_Base(const WSRelayPlayout* p)
{
    return p->baseNow < p->basePrev ? p->baseNow : p->basePrev;
}

// base + J + I: the quickest trip, how much later than that updates come, and
// how far apart.
static double playout_Target(const WSRelayPlayout* p)
{
    double base = playout_Base(p);
    double late = p->numOffsets ? playout_Percentile(p->aOffsets, p->numOffsets, WSRELAYPLAYOUT_LATE_SHARE) - base : 0.0;
    if (late < 0.0)
        late = 0.0;
    double gap = p->numGaps ? playout_Percentile(p->aGaps, p->numGaps, WSRELAYPLAYOUT_GAP_SHARE) : WSRELAYPLAYOUT_GAP_FIRST_MS;
    return base + late + gap;
}

int stdComm_WSRelayPlayout_Add(WSRelayPlayout* p, const WSRelayPlayoutSample* pSample)
{
    if (!isfinite(pSample->t) || !isfinite(pSample->taken))
        return 0;

    const WSRelayPlayoutSample* pNewest = stdComm_WSRelayPlayout_Newest(p);
    if (pNewest)
    {
        double ahead = pSample->t - pNewest->t;
        if (ahead > WSRELAYPLAYOUT_RESTART_MS || ahead < -WSRELAYPLAYOUT_RESTART_MS)
        {
            // Start over, keeping the counts.
            uint32_t poses = p->poses, carried = p->carried, held = p->held, restarts = p->restarts;
            stdComm_WSRelayPlayout_Clear(p);
            p->poses = poses;
            p->carried = carried;
            p->held = held;
            p->restarts = restarts + 1;
            pNewest = NULL;
        }
        else if (ahead <= 0.0)
        {
            return 0;
        }
    }

    if (pNewest)
    {
        double gap = pSample->t - pNewest->t;
        p->aGaps[p->nextGap] = gap < WSRELAYPLAYOUT_GAP_MAX_MS ? gap : WSRELAYPLAYOUT_GAP_MAX_MS;
        p->nextGap = (p->nextGap + 1) % WSRELAYPLAYOUT_GAPS;
        if (p->numGaps < WSRELAYPLAYOUT_GAPS)
            p->numGaps++;
    }

    if (p->count == WSRELAYPLAYOUT_SAMPLES)
    {
        p->first = (p->first + 1) % WSRELAYPLAYOUT_SAMPLES;
        p->count--;
    }
    p->aSamples[(p->first + p->count) % WSRELAYPLAYOUT_SAMPLES] = *pSample;
    p->count++;

    double offset = pSample->taken - pSample->t;
    p->aOffsets[p->nextOffset] = offset;
    p->nextOffset = (p->nextOffset + 1) % WSRELAYPLAYOUT_SAMPLES;
    if (p->numOffsets < WSRELAYPLAYOUT_SAMPLES)
        p->numOffsets++;

    // After a silence both halves are stale.
    if (!p->bBase || pSample->taken - p->baseSince >= 2.0 * WSRELAYPLAYOUT_BASE_MS)
    {
        p->baseNow = p->basePrev = offset;
        p->baseSince = pSample->taken;
        p->bBase = 1;
    }
    else if (pSample->taken - p->baseSince >= WSRELAYPLAYOUT_BASE_MS)
    {
        p->basePrev = p->baseNow;
        p->baseNow = offset;
        p->baseSince = pSample->taken;
    }
    else if (offset < p->baseNow)
    {
        p->baseNow = offset;
    }

    p->target = playout_Target(p);
    if (!p->bDelay)
    {
        p->delay = p->target;
        p->lastNow = pSample->taken;
        p->bDelay = 1;
    }
    return 1;
}

static float playout_Lerp(float a, float b, double f)
{
    return (float)(a + (b - a) * f);
}

static WSRelayPlayoutVec playout_LerpVec(WSRelayPlayoutVec a, WSRelayPlayoutVec b, double f)
{
    WSRelayPlayoutVec v;
    v.x = playout_Lerp(a.x, b.x, f);
    v.y = playout_Lerp(a.y, b.y, f);
    v.z = playout_Lerp(a.z, b.z, f);
    return v;
}

// The shorter way round from a to b.
static float playout_LerpAngle(float a, float b, double f)
{
    double d = fmod((double)b - (double)a, 360.0);
    if (d >= 180.0)
        d -= 360.0;
    else if (d < -180.0)
        d += 360.0;
    return (float)(a + d * f);
}

static void playout_Take(WSRelayPlayoutPose* pOut, const WSRelayPlayoutSample* s)
{
    pOut->pos = s->pos;
    pOut->angles = s->angles;
    pOut->vel = s->vel;
    pOut->headPitch = s->headPitch;
    pOut->sector = s->sector;
    pOut->sectorNext = -1;
    pOut->tBefore = s->t;
    pOut->tAfter = -1.0;
    pOut->physFlags = s->physFlags;
    pOut->attachFlags = s->attachFlags;
    pOut->warp = s->warp;
    pOut->carried = 0;
}

int stdComm_WSRelayPlayout_At(const WSRelayPlayout* p, double t, WSRelayPlayoutPose* pOut)
{
    if (!p->count)
        return 0;
    memset(pOut, 0, sizeof(*pOut));
    pOut->r = t;

    // The last update at or before t.
    int i = p->count - 1;
    while (i >= 0 && playout_Sample(p, i)->t > t)
        i--;

    if (i < 0)
    {
        // Before the oldest: there, as far as this game knows.
        playout_Take(pOut, playout_Sample(p, 0));
        return 1;
    }

    const WSRelayPlayoutSample* a = playout_Sample(p, i);
    playout_Take(pOut, a);
    if (i == p->count - 1)
    {
        // Past the newest: on along its velocity for a while, then still.
        double since = t - a->t;
        if (since <= 0.0)
            return 1;
        pOut->carried = since <= WSRELAYPLAYOUT_CARRY_MS ? 1 : 2;
        double secs = (since < WSRELAYPLAYOUT_CARRY_MS ? since : WSRELAYPLAYOUT_CARRY_MS) / 1000.0;
        pOut->pos.x = (float)(a->pos.x + a->vel.x * secs);
        pOut->pos.y = (float)(a->pos.y + a->vel.y * secs);
        pOut->pos.z = (float)(a->pos.z + a->vel.z * secs);
        if (pOut->carried == 2)
            pOut->vel.x = pOut->vel.y = pOut->vel.z = 0.0f;
        return 1;
    }

    const WSRelayPlayoutSample* b = playout_Sample(p, i + 1);
    pOut->sectorNext = b->sector;
    pOut->tAfter = b->t;
    // Never across a jump: there until the update after it.
    if (b->warp != a->warp)
        return 1;

    double f = (t - a->t) / (b->t - a->t);
    pOut->pos = playout_LerpVec(a->pos, b->pos, f);
    pOut->vel = playout_LerpVec(a->vel, b->vel, f);
    pOut->angles.x = playout_LerpAngle(a->angles.x, b->angles.x, f);
    pOut->angles.y = playout_LerpAngle(a->angles.y, b->angles.y, f);
    pOut->angles.z = playout_LerpAngle(a->angles.z, b->angles.z, f);
    pOut->headPitch = playout_Lerp(a->headPitch, b->headPitch, f);
    return 1;
}

int stdComm_WSRelayPlayout_Pose(WSRelayPlayout* p, double now, WSRelayPlayoutPose* pOut)
{
    if (!p->count)
        return 0;

    double elapsed = now - p->lastNow;
    if (elapsed < 0.0)
        elapsed = 0.0;
    p->lastNow = now;

    // Toward the target, unseen. One that has to shrink a lot jumps (a new,
    // quicker route).
    double off = p->target - p->delay;
    if (off > 0.0)
        p->delay += WSRELAYPLAYOUT_GROW * elapsed < off ? WSRELAYPLAYOUT_GROW * elapsed : off;
    else if (off <= -WSRELAYPLAYOUT_JUMP_MS)
        p->delay = p->target;
    else if (off < 0.0)
        p->delay -= WSRELAYPLAYOUT_SHRINK * elapsed < -off ? WSRELAYPLAYOUT_SHRINK * elapsed : -off;

    // Never further past the newest update than it's carried on: the player
    // holds still there until more come, and the delay grows by as long. So
    // the moment drawn never goes back.
    double r = now - p->delay;
    double rMax = stdComm_WSRelayPlayout_Newest(p)->t + WSRELAYPLAYOUT_CARRY_MS;
    int bHeld = 0;
    if (r > rMax)
    {
        r = rMax;
        p->delay = now - r;
        bHeld = 1;
    }

    stdComm_WSRelayPlayout_At(p, r, pOut);
    if (bHeld)
        pOut->carried = 2;
    p->poses++;
    if (pOut->carried == 1)
        p->carried++;
    else if (pOut->carried == 2)
        p->held++;
    return 1;
}

double stdComm_WSRelayPlayout_Buffer(const WSRelayPlayout* p)
{
    return p->bDelay ? p->delay - playout_Base(p) : 0.0;
}

int stdComm_WSRelayPlayout_MustHold(const WSRelayPlayoutHolds* h, double t, double r)
{
    return h->count > 0 || !(t <= r);
}

int stdComm_WSRelayPlayout_Hold(WSRelayPlayoutHolds* h, double t, double now, const void* pData, int len)
{
    if (len < 0 || len > WSRELAYPLAYOUT_HOLD_BYTES || h->count == WSRELAYPLAYOUT_HOLDS)
        return 0;
    WSRelayPlayoutHeld* pHeld = &h->aHeld[(h->first + h->count) % WSRELAYPLAYOUT_HOLDS];
    pHeld->t = t;
    pHeld->since = now;
    pHeld->len = len;
    memcpy(pHeld->data, pData, len);
    h->count++;
    return 1;
}

int stdComm_WSRelayPlayout_TakeHeld(WSRelayPlayoutHolds* h, double r, double now, WSRelayPlayoutHeld* pOut)
{
    if (!h->count)
        return 0;
    const WSRelayPlayoutHeld* pHeld = &h->aHeld[h->first];
    if (!(pHeld->t <= r) && !(now - pHeld->since >= WSRELAYPLAYOUT_HOLD_MS))
        return 0;
    *pOut = *pHeld;
    h->first = (h->first + 1) % WSRELAYPLAYOUT_HOLDS;
    h->count--;
    return 1;
}
