#include "stdComm_WSRelayHitCheck.h"

#include <math.h>
#include <string.h>

void stdComm_WSRelayHit_ClearRecord(WSRelayHitRecord* pRecord)
{
    memset(pRecord, 0, sizeof(*pRecord));
}

void stdComm_WSRelayHit_Record(WSRelayHitRecord* pRecord, double t, WSRelayHitVec pos, uint32_t life, int alive)
{
    if (pRecord->count)
    {
        WSRelayHitSpot* pLast = &pRecord->aSpots[(pRecord->next + WSRELAYHIT_RECORD_LEN - 1) % WSRELAYHIT_RECORD_LEN];
        if (t < pLast->t)
            return;

        // While the last spot is within 4 ms of the one before it, a frame
        // moves it instead of adding one, so a fast game keeps as long a
        // record as a slow one.
        if (pRecord->count > 1)
        {
            WSRelayHitSpot* pPrev = &pRecord->aSpots[(pRecord->next + WSRELAYHIT_RECORD_LEN - 2) % WSRELAYHIT_RECORD_LEN];
            if (pLast->t - pPrev->t < 4.0
                && pLast->life == life && pLast->alive == !!alive && pPrev->life == life && pPrev->alive == !!alive)
            {
                pLast->t = t;
                pLast->pos = pos;
                return;
            }
        }
    }

    WSRelayHitSpot* pSpot = &pRecord->aSpots[pRecord->next];
    pSpot->t = t;
    pSpot->pos = pos;
    pSpot->life = life;
    pSpot->alive = !!alive;
    pRecord->next = (pRecord->next + 1) % WSRELAYHIT_RECORD_LEN;
    if (pRecord->count < WSRELAYHIT_RECORD_LEN)
        pRecord->count++;
}

static float wsrelayhit_Dist(WSRelayHitVec a, WSRelayHitVec b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static WSRelayHitVec wsrelayhit_Lerp(WSRelayHitVec a, WSRelayHitVec b, double f)
{
    WSRelayHitVec v;
    v.x = (float)(a.x + (b.x - a.x) * f);
    v.y = (float)(a.y + (b.y - a.y) * f);
    v.z = (float)(a.z + (b.z - a.z) * f);
    return v;
}

static int wsrelayhit_IsFinite(WSRelayHitVec v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

// Where along a to b (0 to 1) is closest to p.
static double wsrelayhit_ClosestAlong(WSRelayHitVec a, WSRelayHitVec b, WSRelayHitVec p)
{
    double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    double len2 = ux*ux + uy*uy + uz*uz;
    if (len2 <= 0.0)
        return 0.0;
    double f = ((p.x - a.x)*ux + (p.y - a.y)*uy + (p.z - a.z)*uz) / len2;
    return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
}

float stdComm_WSRelayHit_Closest(const WSRelayHitRecord* pRecord, double tFrom, double tTo, uint32_t life, WSRelayHitVec pos, double* pAt)
{
    float best = -1.0f;
    double bestAt = 0.0;
    int n = pRecord->count;
    int first = (pRecord->next + WSRELAYHIT_RECORD_LEN - n) % WSRELAYHIT_RECORD_LEN;

    for (int i = 0; i < n; i++)
    {
        const WSRelayHitSpot* pA = &pRecord->aSpots[(first + i) % WSRELAYHIT_RECORD_LEN];
        if (!pA->alive || pA->life != life || pA->t > tTo)
            continue;

        const WSRelayHitSpot* pB = NULL;
        if (i + 1 < n)
        {
            pB = &pRecord->aSpots[(first + i + 1) % WSRELAYHIT_RECORD_LEN];
            if (!pB->alive || pB->life != life || pB->t <= pA->t)
                pB = NULL;
        }

        WSRelayHitVec a = pA->pos, b = pA->pos;
        double ta = pA->t, tb = pA->t;
        if (pB)
        {
            // The part of the way to the next spot that's in the window.
            if (pB->t < tFrom)
                continue;
            double span = pB->t - pA->t;
            if (ta < tFrom)
            {
                a = wsrelayhit_Lerp(pA->pos, pB->pos, (tFrom - pA->t) / span);
                ta = tFrom;
            }
            b = pB->pos;
            tb = pB->t;
            if (tb > tTo)
            {
                b = wsrelayhit_Lerp(pA->pos, pB->pos, (tTo - pA->t) / span);
                tb = tTo;
            }
        }
        else if (pA->t < tFrom)
        {
            // The last spot of a life, or the newest one: the player stayed
            // there as far as this record knows, which only counts for the
            // newest.
            if (i + 1 < n)
                continue;
            ta = tb = tFrom;
        }

        double f = wsrelayhit_ClosestAlong(a, b, pos);
        WSRelayHitVec p = wsrelayhit_Lerp(a, b, f);
        float d = wsrelayhit_Dist(p, pos);
        if (best < 0.0f || d < best)
        {
            best = d;
            bestAt = ta + (tb - ta) * f;
        }
    }

    if (pAt)
        *pAt = bestAt;
    return best;
}

double stdComm_WSRelayHit_WindowFrom(double now, double reachMs, double tSent)
{
    double from = now - reachMs;
    if (tSent >= 0.0 && tSent - WSRELAYHIT_UPDATE_SLACK_MS > from)
        from = tSent - WSRELAYHIT_UPDATE_SLACK_MS;
    return from > now ? now : from;
}

WSRelayHitVerdict stdComm_WSRelayHit_Check(const WSRelayHitRecord* pRecord, uint32_t life, int aliveNow,
                                           const WSRelayHitClaim* pClaim, const WSRelayHitAttack* pAttack,
                                           double now, double windowFrom)
{
    return stdComm_WSRelayHit_CheckIn(pRecord, life, aliveNow, pClaim, pAttack, now, windowFrom, now);
}

WSRelayHitVerdict stdComm_WSRelayHit_CheckIn(const WSRelayHitRecord* pRecord, uint32_t life, int aliveNow,
                                             const WSRelayHitClaim* pClaim, const WSRelayHitAttack* pAttack,
                                             double now, double windowFrom, double windowTo)
{
    WSRelayHitVerdict v;
    v.result = WSRELAYHIT_OK;
    v.damage = pClaim->damage;
    v.distance = -1.0f;
    v.at = 0.0;

    if (!isfinite(pClaim->damage) || pClaim->damage <= 0.0f || pClaim->damage > 100000.0f
        || !wsrelayhit_IsFinite(pClaim->target) || !wsrelayhit_IsFinite(pClaim->attacker) || !wsrelayhit_IsFinite(pClaim->blast)
        || (pClaim->source != WSRELAYHIT_SABER && pClaim->source != WSRELAYHIT_SHOT && pClaim->source != WSRELAYHIT_SPLASH))
    {
        v.result = WSRELAYHIT_BAD;
        return v;
    }
    if (!aliveNow)
    {
        v.result = WSRELAYHIT_DEAD;
        return v;
    }
    if (!pAttack->known)
    {
        v.result = WSRELAYHIT_UNKNOWN;
        return v;
    }

    // Could the attacker have made that hit?
    if (pClaim->source == WSRELAYHIT_SABER)
    {
        if (wsrelayhit_Dist(pClaim->attacker, pClaim->target) > pAttack->bladeLength + WSRELAYHIT_SABER_SLACK
            || wsrelayhit_Dist(pClaim->attacker, pAttack->from) > WSRELAYHIT_ATTACKER_SLACK)
        {
            v.result = WSRELAYHIT_OUT_OF_REACH;
            return v;
        }
    }
    else
    {
        // A shot gets no farther from where it was fired than its speed and
        // the level's gravity take it in the time since.
        double secs = (now - pAttack->at + WSRELAYHIT_FLIGHT_SLACK_MS) / 1000.0;
        if (secs < 0.0)
            secs = 0.0;
        double reach = pAttack->range + pAttack->speed * secs + 0.5 * pAttack->gravity * secs * secs + WSRELAYHIT_SHOT_SLACK;
        WSRelayHitVec at = pClaim->source == WSRELAYHIT_SPLASH ? pClaim->blast : pClaim->target;
        if (wsrelayhit_Dist(pAttack->from, at) > reach)
        {
            v.result = WSRELAYHIT_OUT_OF_REACH;
            return v;
        }
        if (pClaim->source == WSRELAYHIT_SPLASH
            && wsrelayhit_Dist(pClaim->blast, pClaim->target) > pAttack->blastRange + WSRELAYHIT_TOLERANCE)
        {
            v.result = WSRELAYHIT_OUT_OF_REACH;
            return v;
        }
    }

    // Was the player there, as the attacker's game showed them?
    WSRelayHitVerdict there = stdComm_WSRelayHit_CheckThere(pRecord, life, pClaim->target, windowFrom, windowTo);
    v.result = there.result;
    v.distance = there.distance;
    v.at = there.at;
    if (v.result != WSRELAYHIT_OK)
        return v;

    if (pAttack->maxDamage > 0.0f && v.damage > pAttack->maxDamage)
        v.damage = pAttack->maxDamage;
    return v;
}

WSRelayHitVerdict stdComm_WSRelayHit_CheckThere(const WSRelayHitRecord* pRecord, uint32_t life, WSRelayHitVec target,
                                                double windowFrom, double windowTo)
{
    WSRelayHitVerdict v;
    v.result = WSRELAYHIT_OK;
    v.damage = 0.0f;
    v.distance = -1.0f;
    v.at = 0.0;

    if (!wsrelayhit_IsFinite(target) || !isfinite(windowFrom) || !isfinite(windowTo))
    {
        v.result = WSRELAYHIT_BAD;
        return v;
    }
    v.distance = stdComm_WSRelayHit_Closest(pRecord, windowFrom, windowTo, life, target, &v.at);
    if (v.distance < 0.0f)
        v.result = WSRELAYHIT_LATE;
    else if (v.distance > WSRELAYHIT_TOLERANCE)
        v.result = WSRELAYHIT_NOT_THERE;
    return v;
}

int stdComm_WSRelayHit_MomentWindow(double now, double reachMs, double r, double beforeMs, double afterMs,
                                    double* pFrom, double* pTo)
{
    if (!isfinite(r) || !isfinite(beforeMs) || !isfinite(afterMs) || beforeMs < 0.0 || r > now + WSRELAYHIT_MOMENT_AHEAD_MS)
        return WSRELAYHIT_BAD;
    if (now - r > reachMs)
        return WSRELAYHIT_LATE;
    *pFrom = r - beforeMs - WSRELAYHIT_MOMENT_SLACK_MS;
    *pTo = (afterMs < 0.0 ? r : r + afterMs) + WSRELAYHIT_MOMENT_SLACK_MS;
    return WSRELAYHIT_OK;
}

void stdComm_WSRelayHit_ClearSeen(WSRelayHitSeen* pSeen)
{
    memset(pSeen, 0, sizeof(*pSeen));
}

int stdComm_WSRelayHit_Seen(const WSRelayHitSeen* pSeen, double now, int attacker, int source, uint32_t attack)
{
    int times = 0;
    for (int i = 0; i < WSRELAYHIT_SEEN_LEN; i++)
    {
        if (pSeen->aSeen[i].attack == attack && pSeen->aSeen[i].attacker == attacker && pSeen->aSeen[i].source == source
            && pSeen->aSeen[i].t > 0.0 && now - pSeen->aSeen[i].t <= WSRELAYHIT_SEEN_MS)
            times++;
    }
    return times;
}

void stdComm_WSRelayHit_AddSeen(WSRelayHitSeen* pSeen, double now, int attacker, int source, uint32_t attack)
{
    pSeen->aSeen[pSeen->next].t = now > 0.0 ? now : 1e-3;
    pSeen->aSeen[pSeen->next].attacker = attacker;
    pSeen->aSeen[pSeen->next].source = source;
    pSeen->aSeen[pSeen->next].attack = attack;
    pSeen->next = (pSeen->next + 1) % WSRELAYHIT_SEEN_LEN;
}
