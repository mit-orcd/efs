#include "efs/request_receipt.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* W155 request receipts: see include/efs/request_receipt.h. */

#define RECEIPT_PROBE 8u

static uint64_t receipt_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int efs_receipt_table_init(struct efs_receipt_table *t, uint32_t cap)
{
    uint32_t n = 16;

    if (!t)
        return EFS_ERR_INVAL;
    memset(t, 0, sizeof(*t));
    while (n < cap && n < (1u << 20))
        n <<= 1;
    t->slot = calloc(n, sizeof(*t->slot));
    if (!t->slot)
        return EFS_ERR_NOMEM;
    t->mask = n - 1;
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    return EFS_OK;
}

void efs_receipt_table_free(struct efs_receipt_table *t)
{
    if (!t || !t->slot)
        return;
    pthread_cond_destroy(&t->cv);
    pthread_mutex_destroy(&t->mu);
    free(t->slot);
    t->slot = NULL;
}

uint32_t efs_receipt_outcome(int rc, int proposed)
{
    if (rc == EFS_OK)
        return EFS_RECEIPT_OUT_APPLIED;
    if (!proposed)
        return EFS_RECEIPT_OUT_NONE;
    if (rc == EFS_ERR_STALE)
        return EFS_RECEIPT_OUT_PARTIAL;
    return EFS_RECEIPT_OUT_UNKNOWN;
}

uint32_t efs_receipt_merge(uint32_t prior, uint32_t next)
{
    return next > prior ? next : prior;
}

const char *efs_receipt_outcome_name(uint32_t outcome)
{
    switch (outcome) {
    case EFS_RECEIPT_OUT_NONE: return "none";
    case EFS_RECEIPT_OUT_UNKNOWN: return "unknown";
    case EFS_RECEIPT_OUT_PARTIAL: return "partial";
    case EFS_RECEIPT_OUT_APPLIED: return "applied";
    default: return "?";
    }
}

void efs_receipt_key_format(const struct efs_receipt_key *k, char *buf, size_t cap)
{
    if (!buf || !cap)
        return;
    if (!k) {
        snprintf(buf, cap, "-");
        return;
    }
    snprintf(buf, cap, "%02x%02x%02x%02x/%u/%llx:%llu", k->uuid[0], k->uuid[1],
             k->uuid[2], k->uuid[3], k->epoch, (unsigned long long)k->mint_ns,
             (unsigned long long)k->seq);
}

static uint32_t receipt_hash(const struct efs_receipt_key *k)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = k->uuid;
    uint8_t tail[20];

    for (unsigned i = 0; i < 16; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    memcpy(tail, &k->epoch, 4);
    memcpy(tail + 4, &k->mint_ns, 8);
    memcpy(tail + 12, &k->seq, 8);
    for (unsigned i = 0; i < sizeof(tail); i++) {
        h ^= tail[i];
        h *= 1099511628211ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static int receipt_same(const struct efs_receipt_key *a, const struct efs_receipt_key *b)
{
    return a->epoch == b->epoch && a->mint_ns == b->mint_ns && a->seq == b->seq &&
           !memcmp(a->uuid, b->uuid, sizeof(a->uuid));
}

/* Caller holds t->mu. */
static struct efs_receipt *receipt_find(struct efs_receipt_table *t,
                                        const struct efs_receipt_key *k)
{
    uint32_t h = receipt_hash(k);

    for (uint32_t i = 0; i < RECEIPT_PROBE; i++) {
        struct efs_receipt *r = &t->slot[(h + i) & t->mask];
        if (r->state != EFS_RECEIPT_STATE_ABSENT && receipt_same(&r->key, k))
            return r;
    }
    return NULL;
}

/* Caller holds t->mu. A pending slot is the running attempt's reservation:
 * never evict it or a duplicate can start another execution. Return NULL
 * when the bounded probe window has no empty or completed slot. */
static struct efs_receipt *receipt_insert(struct efs_receipt_table *t,
                                          const struct efs_receipt_key *k)
{
    uint32_t h = receipt_hash(k);
    struct efs_receipt *pick = NULL, *done = NULL;

    for (uint32_t i = 0; i < RECEIPT_PROBE; i++) {
        struct efs_receipt *r = &t->slot[(h + i) & t->mask];
        if (r->state == EFS_RECEIPT_STATE_ABSENT) {
            pick = r;
            break;
        }
        if (r->state == EFS_RECEIPT_STATE_DONE && (!done || r->end_ms < done->end_ms))
            done = r;
    }
    if (!pick) {
        if (!done)
            return NULL;
        pick = done;
        t->evictions++;
    }
    memset(pick, 0, sizeof(*pick));
    pick->key = *k;
    pick->state = EFS_RECEIPT_STATE_DONE;
    pick->outcome = EFS_RECEIPT_OUT_NONE;
    snprintf(pick->phase, sizeof(pick->phase), "-");
    t->stores++;
    return pick;
}

int efs_receipt_lookup(struct efs_receipt_table *t, const struct efs_receipt_key *k,
                       struct efs_receipt *out)
{
    struct efs_receipt *r;

    if (!t || !t->slot || !k)
        return 0;
    pthread_mutex_lock(&t->mu);
    r = receipt_find(t, k);
    if (r && out)
        *out = *r;
    pthread_mutex_unlock(&t->mu);
    return r != NULL;
}

static void receipt_deadline(struct timespec *until, uint32_t ms)
{
    clock_gettime(CLOCK_REALTIME, until);
    until->tv_sec += ms / 1000u;
    until->tv_nsec += (long)(ms % 1000u) * 1000000l;
    if (until->tv_nsec >= 1000000000l) {
        until->tv_sec++;
        until->tv_nsec -= 1000000000l;
    }
}

int efs_receipt_run(struct efs_receipt_table *t, const struct efs_receipt_key *k,
                    efs_ino_t ino, uint32_t nrec, uint32_t join_ms,
                    efs_receipt_exec_fn exec, void *ctx, int *how,
                    struct efs_receipt *out)
{
    struct efs_receipt *r;
    struct timespec until;
    int waited = 0, proposed = 0, rc;
    const char *phase = NULL;
    struct efs_receipt_stale stale;

    if (how)
        *how = EFS_RECEIPT_RAN;
    if (!t || !t->slot || !k || !exec)
        return EFS_ERR_INVAL;
    receipt_deadline(&until, join_ms);
    pthread_mutex_lock(&t->mu);
    for (;;) {
        r = receipt_find(t, k);
        if (r && r->state == EFS_RECEIPT_STATE_DONE &&
            r->outcome == EFS_RECEIPT_OUT_APPLIED) {
            if (waited)
                r->joined++;
            else
                r->replayed++;
            if (how)
                *how = waited ? EFS_RECEIPT_JOINED : EFS_RECEIPT_REPLAY;
            if (out)
                *out = *r;
            pthread_mutex_unlock(&t->mu);
            return EFS_OK;
        }
        if (r && waited && r->state == EFS_RECEIPT_STATE_DONE &&
            r->outcome == EFS_RECEIPT_OUT_PARTIAL && r->rc == EFS_ERR_STALE) {
            /* The attempt this duplicate joined committed all but its CAS
             * losers: the same answer, so the client replays only those. */
            r->joined++;
            if (how)
                *how = EFS_RECEIPT_JOINED;
            if (out)
                *out = *r;
            pthread_mutex_unlock(&t->mu);
            return EFS_ERR_STALE;
        }
        if (!r || r->state != EFS_RECEIPT_STATE_PENDING)
            break;
        waited = 1;
        if (pthread_cond_timedwait(&t->cv, &t->mu, &until) == ETIMEDOUT) {
            r = receipt_find(t, k);
            if (r && r->state == EFS_RECEIPT_STATE_PENDING) {
                r->joined++;
                if (how)
                    *how = EFS_RECEIPT_WAITING;
                if (out)
                    *out = *r;
                pthread_mutex_unlock(&t->mu);
                return EFS_ERR_BUSY;
            }
        }
    }
    if (!r) {
        r = receipt_insert(t, k);
        if (!r) {
            if (how) *how = EFS_RECEIPT_REFUSED;
            if (out) memset(out, 0, sizeof(*out));
            pthread_mutex_unlock(&t->mu);
            return EFS_ERR_BUSY;
        }
    }
    r->state = EFS_RECEIPT_STATE_PENDING;
    r->attempts++;
    r->ino = ino;
    r->nrec = nrec;
    r->begin_ms = receipt_now_ms();
    r->end_ms = 0;
    pthread_mutex_unlock(&t->mu);

    memset(&stale, 0, sizeof(stale));
    rc = exec(ctx, &proposed, &phase, &stale);

    pthread_mutex_lock(&t->mu);
    /* The pending reservation cannot be evicted while exec runs. */
    r->state = EFS_RECEIPT_STATE_DONE;
    r->rc = rc;
    r->outcome = efs_receipt_merge(r->outcome, efs_receipt_outcome(rc, proposed));
    r->end_ms = receipt_now_ms();
    snprintf(r->phase, sizeof(r->phase), "%s", phase && *phase ? phase : "-");
    if (rc == EFS_ERR_STALE)
        r->stale = stale;
    if (out)
        *out = *r;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return rc;
}
