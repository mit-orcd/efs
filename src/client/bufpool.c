/* Shared client body/scratch budget. Ownership transfers between the dirty
 * cache, read cache and PUT pipeline keep the allocation charged until free.
 * Failed publication therefore retains its charge, as required by D27. */
#include <efs/common.h>
#include <efs/rdma.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "client_internal.h"

#define BUF_SLAB_N 256
#define BUF_SLAB_MAX 64
#define BUFPOOL_MAX_FREE (BUF_SLAB_N * BUF_SLAB_MAX)
#define SLAB_BYTES ((uint64_t)BUF_SLAB_N * EFS_CHUNK_SIZE)
static void *g_bp[BUFPOOL_MAX_FREE];
static void *g_slabs[BUF_SLAB_MAX];
static int g_bp_n, g_bp_slabs;
static uint64_t g_live, g_reserved, g_heap, g_backing, g_metadata, g_meta_reserved;
static uint64_t g_hard, g_drain;
static pthread_mutex_t g_bp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_bp_once = PTHREAD_ONCE_INIT;
struct buf_credit { uint64_t body, metadata; };
static __thread struct buf_credit *t_credit_owner;
static pthread_key_t g_credit_key;
static int g_credit_key_error;
static __thread unsigned t_drain;

static uint64_t budget_env(const char *name, uint64_t fallback,
                           uint64_t min, uint64_t max)
{
    const char *s = getenv(name);
    char *end;
    unsigned long long v;
    if (!s || !*s || *s == '-')
        return fallback;
    errno = 0;
    v = strtoull(s, &end, 10);
    return !errno && *end == '\0' && v >= min && v <= max ? v : fallback;
}
static void credit_destroy(void *unused)
{
    struct buf_credit *credit = unused;
    pthread_mutex_lock(&g_bp_mu);
    g_reserved -= credit->body;
    g_meta_reserved -= credit->metadata;
    pthread_mutex_unlock(&g_bp_mu);
    free(credit);
}
static void buf_init(void)
{
    g_credit_key_error = pthread_key_create(&g_credit_key, credit_destroy);
    g_hard = budget_env("EFS_DCACHE_HARD_BYTES", 256ull << 20,
                         SLAB_BYTES, 4ull << 30);
    g_drain = budget_env("EFS_DCACHE_DRAIN_BYTES", 64ull << 20,
                          SLAB_BYTES, 1ull << 30);
    const char *trace = getenv("EFS_BUF_BUDGET_TRACE");
    if (trace && trace[0] && strcmp(trace,"0"))
        fprintf(stderr,"buf-budget hard=%llu drain=%llu slab=%llu\n",
                (unsigned long long)g_hard,(unsigned long long)g_drain,
                (unsigned long long)SLAB_BYTES);
}
static uint64_t buf_charge(uint32_t len)
{
    return len <= EFS_CHUNK_SIZE ? EFS_CHUNK_SIZE : len;
}
static int buf_in_slab(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < g_bp_slabs; i++) {
        uintptr_t b = (uintptr_t)g_slabs[i];
        if (b && a >= b && a < b + SLAB_BYTES)
            return 1;
    }
    return 0;
}

/* Request credit is reserved before append locking/server range reservation.
 * The caller must release it on every return. No waiting under cache locks. */
int efs_buf_reserve_request(uint64_t bytes, uint64_t metadata)
{
    int ok;
    pthread_once(&g_bp_once, buf_init);
    if (g_credit_key_error)
        return EFS_ERR_NOMEM;
    if (!t_credit_owner) {
        struct buf_credit *owner = calloc(1, sizeof(*owner));
        if (!owner)
            return EFS_ERR_NOMEM;
        if (pthread_setspecific(g_credit_key, owner)) {
            free(owner);
            return EFS_ERR_NOMEM;
        }
        t_credit_owner = owner;
    }
    pthread_mutex_lock(&g_bp_mu);
    ok = !t_credit_owner->body && !t_credit_owner->metadata &&
         bytes <= g_hard && g_live + g_reserved <= g_hard - bytes &&
         metadata <= (8ull << 20) &&
         g_metadata + g_meta_reserved <= (8ull << 20) - metadata;
    if (ok) {
        t_credit_owner->body = bytes;
        g_reserved += bytes;
        t_credit_owner->metadata = metadata;
        g_meta_reserved += metadata;
    }
    pthread_mutex_unlock(&g_bp_mu);
    return ok ? 0 : EFS_ERR_BUSY;
}
int efs_buf_reserve(uint64_t bytes)
{
    return efs_buf_reserve_request(bytes, 0);
}
void efs_buf_unreserve(void)
{
    pthread_mutex_lock(&g_bp_mu);
    if (t_credit_owner) {
        g_reserved -= t_credit_owner->body;
        t_credit_owner->body = 0;
        g_meta_reserved -= t_credit_owner->metadata;
        t_credit_owner->metadata = 0;
    }
    pthread_mutex_unlock(&g_bp_mu);
}
void efs_buf_drain_enter(void) { t_drain++; }
void efs_buf_drain_leave(void) { if (t_drain) t_drain--; }
void efs_buf_budget_stats(uint64_t *live, uint64_t *reserved,
                           uint64_t *backing, uint64_t *limit)
{
    pthread_once(&g_bp_once, buf_init);
    pthread_mutex_lock(&g_bp_mu);
    if (live) *live = g_live;
    if (reserved) *reserved = g_reserved;
    if (backing) *backing = g_backing + g_heap;
    if (limit) *limit = g_hard + g_drain;
    pthread_mutex_unlock(&g_bp_mu);
}

static void *buf_alloc(uint32_t len, int speculative)
{
    void *p = NULL;
    uint64_t charge = buf_charge(len), credit;
    pthread_once(&g_bp_once, buf_init);
    pthread_mutex_lock(&g_bp_mu);
    credit = speculative || t_drain || !t_credit_owner ? 0 :
             (t_credit_owner->body < charge ? t_credit_owner->body : charge);
    /* Drain scratch can temporarily put live bytes above the normal cap.
     * Fully reserved allocations already own capacity: honor that promise
     * under the combined bound, which drain allocations also respect.
     * Unreserved/partly reserved writes still cannot spend drain headroom. */
    uint64_t limit = speculative ? g_hard / 2 :
        (t_drain || credit == charge ? g_hard + g_drain : g_hard);
    if (charge - credit > limit ||
        g_live + g_reserved > limit - (charge - credit))
        goto out;
    if (len <= EFS_CHUNK_SIZE && g_bp_n) {
        p = g_bp[--g_bp_n];
    } else if (len <= EFS_CHUNK_SIZE && g_bp_slabs < BUF_SLAB_MAX &&
               g_backing + g_heap <= g_hard + g_drain - SLAB_BYTES) {
        /* Serialize creation: the registered slab and its free-list become
         * visible together. Slabs remain registered for process lifetime. */
        p = malloc((size_t)SLAB_BYTES);
        if (!p)
            goto out;
        g_slabs[g_bp_slabs++] = p;
        g_backing += SLAB_BYTES;
        (void)efs_rdma_zc_region_add(p, (size_t)SLAB_BYTES);
        for (int i = 1; i < BUF_SLAB_N; i++)
            g_bp[g_bp_n++] = (uint8_t *)p + (size_t)i * EFS_CHUNK_SIZE;
    } else if (charge <= g_hard + g_drain &&
               g_backing + g_heap <= g_hard + g_drain - charge) {
        p = malloc((size_t)charge);
        if (p)
            g_heap += charge;
    }
    if (p) {
        g_live += charge;
        if (t_credit_owner) t_credit_owner->body -= credit;
        g_reserved -= credit;
    }
out:
    pthread_mutex_unlock(&g_bp_mu);
    return p;
}

/* Speculative/cache admission is atomic with allocation. It never spends
 * request credit or drain capacity, and leaves half the normal budget for
 * demand reads and writes, including when queued jobs retain their bodies. */
void *efs_buf_alloc_prefetch(uint32_t len) { return buf_alloc(len, 1); }
void *efs_buf_alloc(uint32_t len) { return buf_alloc(len, 0); }

void efs_buf_free(void *p, uint32_t len)
{
    if (!p)
        return;
    uint64_t charge = buf_charge(len);
    pthread_mutex_lock(&g_bp_mu);
    g_live -= charge;
    if (len <= EFS_CHUNK_SIZE && buf_in_slab(p)) {
        g_bp[g_bp_n++] = p;
    } else {
        g_heap -= charge;
        free(p);
    }
    pthread_mutex_unlock(&g_bp_mu);
}

/* Bound collision metadata independently of body lifetime. Embedded bucket
 * heads are fixed storage. Heap nodes remain charged when reused. */
void *efs_buf_metadata_alloc(size_t size)
{
    void *p = NULL;
    pthread_mutex_lock(&g_bp_mu);
    uint64_t credit = !t_credit_owner ? 0 :
        (t_credit_owner->metadata < size ? t_credit_owner->metadata : size);
    if (size - credit <= (8ull << 20) &&
        g_metadata + g_meta_reserved <= (8ull << 20) - (size - credit)) {
        p = calloc(1, size);
        if (p) {
            g_metadata += size;
            if (t_credit_owner) t_credit_owner->metadata -= credit;
            g_meta_reserved -= credit;
        }
    }
    pthread_mutex_unlock(&g_bp_mu);
    return p;
}
void efs_buf_metadata_free(void *p, size_t size)
{
    if (!p) return;
    pthread_mutex_lock(&g_bp_mu);
    g_metadata -= size;
    free(p);
    pthread_mutex_unlock(&g_bp_mu);
}

void efs_buf_metadata_stats(uint64_t *live, uint64_t *reserved, uint64_t *limit)
{
    pthread_mutex_lock(&g_bp_mu);
    *live = g_metadata;
    *reserved = g_meta_reserved;
    *limit = 8ull << 20;
    pthread_mutex_unlock(&g_bp_mu);
}
