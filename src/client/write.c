#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include "efs/meta_apply.h"
#include "efs/wb_recovery.h"
#include "efs/writer_state.h"
#include "efs/publication.h"
#include "efs/write_extent.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>

/* D27 registry. Cache shard locks may take this mutex; never the reverse.
 * Nodes survive description close while a stalled record still owns bytes. */
#define WB_ERROR_BUCKETS 256
struct wb_inode_error {
    efs_ino_t ino;
    struct efs_wb_error_state state;
    uint64_t descriptions;
    struct wb_inode_error *next;
};
struct efs_wb_description {
    struct wb_inode_error *inode;
    uint64_t cursor;
};
static pthread_mutex_t g_wb_error_mu = PTHREAD_MUTEX_INITIALIZER;
static struct wb_inode_error *g_wb_errors[WB_ERROR_BUCKETS];

static struct wb_inode_error *wb_error_get(efs_ino_t ino, int create)
{
    unsigned b = ino % WB_ERROR_BUCKETS;
    struct wb_inode_error *n = g_wb_errors[b];
    for (; n; n = n->next)
        if (n->ino == ino)
            return n;
    if (!create)
        return NULL;
    n = calloc(1, sizeof(*n));
    if (n) {
        n->ino = ino;
        n->next = g_wb_errors[b];
        g_wb_errors[b] = n;
    }
    return n;
}

static void wb_error_retire(struct wb_inode_error *n)
{
    if (n->descriptions || n->state.unresolved)
        return;
    struct wb_inode_error **p = &g_wb_errors[n->ino % WB_ERROR_BUCKETS];
    while (*p != n)
        p = &(*p)->next;
    *p = n->next;
    free(n);
}

struct efs_wb_description *efs_wb_description_open(efs_ino_t ino)
{
    struct efs_wb_description *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    pthread_mutex_lock(&g_wb_error_mu);
    d->inode = wb_error_get(ino, 1);
    if (d->inode) {
        ++d->inode->descriptions;
        d->cursor = d->inode->state.sequence;
    }
    pthread_mutex_unlock(&g_wb_error_mu);
    if (!d->inode) {
        free(d);
        return NULL;
    }
    return d;
}

void efs_wb_description_close(struct efs_wb_description *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&g_wb_error_mu);
    --d->inode->descriptions;
    wb_error_retire(d->inode);
    pthread_mutex_unlock(&g_wb_error_mu);
    free(d);
}

int efs_wb_description_sync(struct efs_wb_description *d, int recovered_here)
{
    int error = 0;
    if (!d)
        return 0;
    pthread_mutex_lock(&g_wb_error_mu);
    error = efs_wb_error_sync(&d->inode->state, &d->cursor, recovered_here);
    pthread_mutex_unlock(&g_wb_error_mu);
    return error;
}

int efs_wb_inode_stalled(efs_ino_t ino)
{
    pthread_mutex_lock(&g_wb_error_mu);
    struct wb_inode_error *n = wb_error_get(ino, 0);
    int stalled = n && n->state.unresolved;
    pthread_mutex_unlock(&g_wb_error_mu);
    return stalled;
}

static int wb_error_record_add(efs_ino_t ino)
{
    pthread_mutex_lock(&g_wb_error_mu);
    struct wb_inode_error *n = wb_error_get(ino, 1);
    if (n)
        efs_wb_error_add(&n->state);
    pthread_mutex_unlock(&g_wb_error_mu);
    return n != NULL;
}

static void wb_error_record_resolve(efs_ino_t ino)
{
    pthread_mutex_lock(&g_wb_error_mu);
    struct wb_inode_error *n = wb_error_get(ino, 0);
    if (n) {
        (void)efs_wb_error_resolve(&n->state);
        wb_error_retire(n);
    }
    pthread_mutex_unlock(&g_wb_error_mu);
}

static void dcache_cycle_sent(const struct efs_chunk_rec *r, uint64_t seq);
static int dcache_cycle_verdict(efs_ino_t ino, uint32_t ci,
                                const struct efs_chunk_entry *ce,
                                uint64_t object_seq);

static uint32_t data_chunk_size(void)
{
    uint32_t cs = g_client.export.chunk_size;
    return efs_chunk_size_valid(cs) ? cs : EFS_DEFAULT_CHUNK_SIZE;
}

static uint32_t data_frag_size(void)
{
    return efs_frag_size(data_chunk_size());
}

static void frag_ptrs(uint8_t *buf, uint32_t frag_len, uint8_t *frags[EFS_NUM_FRAGMENTS])
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        frags[i] = buf + (size_t)i * frag_len;
}

/* chunk_keys / chunks realloc under set/reserve. Lookups must hold idx_mu. */
static int export_chunk_exists(efs_ino_t ino, uint32_t ci)
{
    pthread_mutex_lock(&g_client.idx_mu);
    int ok = (efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    return ok;
}

static int export_chunk_copy(efs_ino_t ino, uint32_t ci, struct efs_chunk_entry *out)
{
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_get_chunk(&g_client.export, ino, ci, out);
    pthread_mutex_unlock(&g_client.idx_mu);
    return rc;
}

static void export_reserve_chunks_locked(uint64_t extra)
{
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    (void)efs_export_reserve_chunks(&g_client.export, extra);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
}

static uint64_t dcache_base_gen_of(efs_ino_t ino, uint32_t ci, uint64_t fallback);
static int dcache_trace_on(void);
static int dcache_object_of(efs_ino_t ino, uint32_t ci, struct efs_chunk_rec *rec,
                             uint64_t *seq);
static int dcache_note_committed(efs_ino_t ino, uint32_t ci, uint64_t gen,
                                  uint64_t object_gen, uint64_t object_seq);
static int dcache_replay_stale_ex(efs_ino_t ino, uint32_t ci, int absent);
static int report_replay_stale(efs_ino_t ino);

static int dcache_pending_of(efs_ino_t ino, uint32_t ci);

/* A pulled span-only row has no local PUT identity. Retain its report mark
 * only when local cache ownership proves there are bytes still to publish. */
static void report_retry_without_identity(efs_ino_t ino, uint32_t ci)
{
    if (dcache_pending_of(ino, ci))
        efs_client_mark_chunk_dirty(ino, ci);
}

/* Each new PUT owns a fresh object name. A content-derived name can be
 * republished after supersession while an older GC record still names it.
 * Retries of this PUT retain its name; another PUT of identical bytes does
 * not revive an object already scheduled for collection. */
static pthread_once_t object_uuid_once = PTHREAD_ONCE_INIT;
static uint8_t object_uuid[EFS_OPID_UUID_LEN];
static int object_uuid_ready;
static uint64_t object_sequence;
static void object_uuid_init(void)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    size_t n = 0;
    while (n < sizeof(object_uuid)) {
        ssize_t got = read(fd, object_uuid + n, sizeof(object_uuid) - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += (size_t)got;
    }
    close(fd);
    object_uuid_ready = n == sizeof(object_uuid);
}
static uint64_t new_chunk_object_gen(uint32_t ci)
{
    pthread_once(&object_uuid_once, object_uuid_init);
    if (!object_uuid_ready) return 0;
    uint64_t seq = __atomic_add_fetch(&object_sequence, 1, __ATOMIC_RELAXED);
    if (!seq) return 0;
    return efs_meta_candidate_gen(object_uuid, 0, seq, ci, 0);
}

/* Last successful PUT, independent of dcache_find (which requires e->data).
 * GETCHUNKS/apply_chunk_recs must not be the source of REPORT mappings —
 * that is how a ftruncate stub gen was republished and remount DECODE'd.
 *
 * This table holds every PUT this client has not yet seen committed, so
 * it is a real hash table, not a direct-mapped cache. The direct-mapped
 * version (4096, then 65536 slots) lost an entry on every collision; the
 * REPORT rec builder then fell back to the staging table's nodes, and a
 * STALE-round repull (apply_chunk_recs) had just overwritten those with
 * the SERVER's row — nodes 0,0,0 for a span-only chunk a peer had
 * published first. That rec was INVAL at the server ("skipping rec",
 * reply OK), the client marked it committed, and the bytes were gone:
 * 9x4 IOR hard, 31k chunks per client, ~100 client-boundary pieces of
 * shared chunks per run read back as zeros (Sep 30 2026,
 * results/io500/20260930-104500-rdma). An entry is dropped when the
 * REPORT that carried it returns OK (putid_drop), so the table is bounded
 * by the unpublished set. */
struct put_id {
    efs_ino_t ino; /* 0 = empty, PUTID_TOMB = deleted */
    uint32_t ci;
    uint64_t gen;
    uint64_t seq; /* dcache snapshot seq of this PUT (0 = unordered) */
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t cks[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint32_t delta_off;
    uint32_t delta_len; /* 0 = this PUT is a full-chunk image */
    uint32_t delta_base_n;
    uint64_t delta_base_seq;
    uint64_t publish_epoch;
    uint32_t publish_flags;
};
#define PUTID_TOMB ((efs_ino_t)~0ull)
#define PUTID_MIN 65536u
static struct put_id *g_putid;
static uint64_t g_putid_mask;   /* capacity - 1 (capacity is a power of 2) */
static uint64_t g_putid_live;   /* entries with a real ino */
static uint64_t g_putid_used;   /* live + tombstones */
static pthread_mutex_t g_putid_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t putid_hash(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = ((uint64_t)ino * 1315423911ull) ^ ((uint64_t)ci << 1);

    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

/* Caller holds g_putid_mu. Live entry for (ino, ci) or NULL. */
static struct put_id *putid_find(efs_ino_t ino, uint32_t ci)
{
    uint64_t i, n;

    if (!g_putid)
        return NULL;
    i = putid_hash(ino, ci) & g_putid_mask;
    for (n = 0; n <= g_putid_mask; n++) {
        struct put_id *p = &g_putid[i];

        if (p->ino == 0)
            return NULL;
        if (p->ino == ino && p->ci == ci)
            return p;
        i = (i + 1) & g_putid_mask;
    }
    return NULL;
}

/* Caller holds g_putid_mu. Rebuild at `cap` slots without tombstones. */
static int putid_rehash(uint64_t cap)
{
    struct put_id *nt = calloc(cap, sizeof(*nt));
    uint64_t i;

    if (!nt)
        return -1;
    for (i = 0; g_putid && i <= g_putid_mask; i++) {
        struct put_id *p = &g_putid[i];
        uint64_t j;

        if (p->ino == 0 || p->ino == PUTID_TOMB)
            continue;
        j = putid_hash(p->ino, p->ci) & (cap - 1);
        while (nt[j].ino)
            j = (j + 1) & (cap - 1);
        nt[j] = *p;
    }
    free(g_putid);
    g_putid = nt;
    g_putid_mask = cap - 1;
    g_putid_used = g_putid_live;
    return 0;
}

/* Caller holds g_putid_mu. Slot for a new (ino, ci): first tombstone or
 * empty on its probe path. Grows at 1/2 live, sweeps tombstones at 3/4
 * used. NULL only on NOMEM. */
static struct put_id *putid_insert_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t i, n;
    struct put_id *tomb = NULL;

    if (!g_putid && putid_rehash(PUTID_MIN) != 0)
        return NULL;
    if ((g_putid_live + 1) * 2 > g_putid_mask + 1) {
        if (putid_rehash((g_putid_mask + 1) * 2) != 0)
            return NULL;
    } else if ((g_putid_used + 1) * 4 > (g_putid_mask + 1) * 3) {
        if (putid_rehash(g_putid_mask + 1) != 0)
            return NULL;
    }
    i = putid_hash(ino, ci) & g_putid_mask;
    for (n = 0; n <= g_putid_mask; n++) {
        struct put_id *p = &g_putid[i];

        if (p->ino == 0) {
            if (tomb)
                return tomb;
            g_putid_used++;
            return p;
        }
        if (p->ino == PUTID_TOMB && !tomb)
            tomb = p;
        i = (i + 1) & g_putid_mask;
    }
    return tomb;
}

/* Record only if this PUT's snapshot is at least as new as the recorded
 * one for the same chunk (see dcache_ent.object_seq). */
static void putid_note(efs_ino_t ino, uint32_t ci, uint64_t gen, uint64_t seq,
                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      const uint8_t cks[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                      uint32_t delta_off, uint32_t delta_len,
                      uint32_t delta_base_n, uint64_t delta_base_seq,
                      const struct efs_chunk_entry *obs)
{
    struct put_id *p;

    if (!ino || !gen || ino == PUTID_TOMB)
        return;
    pthread_mutex_lock(&g_putid_mu);
    p = putid_find(ino, ci);
    if (p && seq && p->seq && seq < p->seq) {
        pthread_mutex_unlock(&g_putid_mu);
        return;
    }
    if (!p) {
        p = putid_insert_slot(ino, ci);
        if (!p) {
            static int said;

            if (!said) {
                said = 1;
                fprintf(stderr, "efs: putid table NOMEM at %llu live\n",
                        (unsigned long long)g_putid_live);
            }
            pthread_mutex_unlock(&g_putid_mu);
            return;
        }
        g_putid_live++;
    }
    p->ino = ino;
    p->ci = ci;
    p->gen = gen;
    p->seq = seq;
    memcpy(p->nodes, nodes, sizeof(p->nodes));
    memcpy(p->cks, cks, sizeof(p->cks));
    p->delta_off = delta_off;
    p->delta_len = delta_len;
    p->delta_base_n = delta_base_n;
    p->delta_base_seq = delta_base_seq;
    p->publish_epoch = obs ? obs->read_view.fence_epoch : 0;
    p->publish_flags = EFS_CHUNK_REC_F_FRESH_OBJECT |
        (obs && obs->read_view.count ? EFS_CHUNK_REC_F_CAPTURED_EPOCH : 0);
    pthread_mutex_unlock(&g_putid_mu);
}

static int putid_fill(efs_ino_t ino, uint32_t ci, struct efs_chunk_rec *rec,
                       uint64_t *seq)
{
    struct put_id *p;
    int hit = 0;

    if (!ino || !rec)
        return 0;
    pthread_mutex_lock(&g_putid_mu);
    p = putid_find(ino, ci);
    if (p && p->gen) {
        memcpy(rec->nodes, p->nodes, sizeof(rec->nodes));
        memcpy(rec->checksums, p->cks, sizeof(rec->checksums));
        rec->chunk_generation = p->gen;
        if (seq) *seq = p->seq;
        rec->delta_off = p->delta_off;
        rec->delta_len = p->delta_len;
        rec->delta_base_n = p->delta_base_n;
        rec->delta_base_seq = p->delta_base_seq;
        rec->publish_epoch = p->publish_epoch;
        rec->publish_flags = p->publish_flags;
        hit = 1;
    }
    pthread_mutex_unlock(&g_putid_mu);
    return hit;
}

/* The host committed object `gen` for (ino, ci) (gen 0: the chunk is
 * gone, e.g. truncated). A newer PUT of the same chunk keeps its entry. */
static void putid_drop(efs_ino_t ino, uint32_t ci, uint64_t gen)
{
    struct put_id *p;

    if (!ino)
        return;
    pthread_mutex_lock(&g_putid_mu);
    p = putid_find(ino, ci);
    if (p && (gen == 0 || p->gen == gen)) {
        p->ino = PUTID_TOMB;
        p->gen = 0;
        g_putid_live--;
    }
    pthread_mutex_unlock(&g_putid_mu);
}

static void putid_drop_snapshot(efs_ino_t ino, uint32_t ci, uint64_t gen,
                                 uint64_t seq)
{
    pthread_mutex_lock(&g_putid_mu);
    struct put_id *p = putid_find(ino, ci);
    if (p && p->gen == gen && p->seq == seq) {
        p->ino = PUTID_TOMB;
        p->gen = 0;
        g_putid_live--;
    }
    pthread_mutex_unlock(&g_putid_mu);
}

/* The REPORT rec builder had neither a putid nor a dcache object for a
 * dirty chunk and is about to use the staging table's mapping. That
 * mapping may be the server's row (a repull), not this client's PUT. */
static void putid_miss_note(efs_ino_t ino, uint32_t ci,
                            const efs_node_id_t nodes[EFS_NUM_FRAGMENTS])
{
    static uint64_t n, last_ms;
    uint64_t now;
    struct timespec ts;

    uint64_t count = __sync_add_and_fetch(&n, 1);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    uint64_t previous = __sync_val_compare_and_swap(&last_ms, 0, 0);
    if (now - previous < 1000ull ||
        !__sync_bool_compare_and_swap(&last_ms, previous, now))
        return;
    fprintf(stderr,
            "efs: report rec ino=%llu ci=%u rejected unowned staging observation "
            "nodes=%u,%u,%u (putid miss, n=%llu)\n",
            (unsigned long long)ino, ci, nodes[0], nodes[1], nodes[2],
            (unsigned long long)count);
}

/* Both initial and STALE-rebuilt REPORTs use the same ownership rule.
 * Nonzero nodes in a fetched table are observations, never PUT evidence. */
static int report_fill_owned(efs_ino_t ino, uint32_t ci,
                             struct efs_chunk_rec *rec, uint64_t *seq,
                             const efs_node_id_t observed[EFS_NUM_FRAGMENTS])
{
    if (putid_fill(ino, ci, rec, seq) || dcache_object_of(ino, ci, rec, seq))
        return 1;
    putid_miss_note(ino, ci, observed);
    report_retry_without_identity(ino, ci);
    return 0;
}

static void now_ns(uint64_t *sec, uint32_t *nsec)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = (uint64_t)ts.tv_sec;
    *nsec = (uint32_t)ts.tv_nsec;
}

static uint64_t pack_dirty_chunk(efs_ino_t ino, uint32_t chunk_index)
{
    uint64_t k = ino ^ ((uint64_t)chunk_index * 0x9E3779B97F4A7C15ULL);
    return k ? k : 1;
}

static int dirty_set_ensure(uint64_t **keys, uint64_t *mask, uint64_t count)
{
    if (*keys && count * 2 <= *mask + 1)
        return 0;
    uint64_t old_mask = *mask;
    uint64_t *old_keys = *keys;
    uint64_t cap = 16;
    while (cap < (count ? count * 4 : 16))
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(uint64_t));
    if (!nk)
        return -1;
    *keys = nk;
    *mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & *mask;
            while ((*keys)[j] != 0)
                j = (j + 1) & *mask;
            (*keys)[j] = k;
        }
        free(old_keys);
    }
    return 0;
}

static void dirty_set_put(uint64_t *keys, uint64_t mask, uint64_t key, uint64_t *count)
{
    if (!keys || mask == 0 || key == 0)
        return;
    uint64_t i = key & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0) {
            keys[i] = key;
            (*count)++;
            return;
        }
        if (keys[i] == key)
            return;
        i = (i + 1) & mask;
    }
}

static int dirty_set_has(const uint64_t *keys, uint64_t mask, efs_ino_t ino)
{
    if (!keys || !mask)
        return 0;
    uint64_t i = (uint64_t)ino & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0)
            return 0;
        if (keys[i] == (uint64_t)ino)
            return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

/* W41 (decided, Oct 2 2026): the live dirty state is one record per
 * inode, so a REPORT detaches one inode's marks and leaves every other
 * inode's in place. Before, one process-wide set was detached whole on
 * every REPORT (an fsync of file A re-marked every other file's chunks
 * one by one under dirty_mu) and one `report_mu` serialized every
 * REPORT: a 16-stream dd's close() waited 74.6 s behind the other
 * streams' publishes, and a file looping on STALE held every other
 * close. Records live under g_client.dirty_mu. */
struct ino_dirty {
    efs_ino_t ino;
    uint64_t *chunk_keys, chunk_mask, chunk_count; /* dedupe set */
    efs_ino_t *chunk_inos;                          /* parallel arrays */
    uint32_t *chunk_idxs;
    uint64_t chunk_cap;
    int ino_marked;   /* the row (size/mtime) needs an irec */
    uint64_t since_ms; /* oldest unpublished mark, monotonic */
    unsigned landed;  /* D24: landed PUTs since this inode's last kick */
    struct ino_dirty *next;
};
#define IDIRTY_BUCKETS 4096
static struct ino_dirty *g_idirty[IDIRTY_BUCKETS];
static uint64_t g_idirty_n; /* records */

static struct ino_dirty *idirty_find(efs_ino_t ino)
{
    struct ino_dirty *r = g_idirty[(uint64_t)ino % IDIRTY_BUCKETS];

    while (r && r->ino != ino)
        r = r->next;
    return r;
}

static struct ino_dirty *idirty_get(efs_ino_t ino)
{
    struct ino_dirty *r = idirty_find(ino);

    if (r)
        return r;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->ino = ino;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    r->since_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    r->next = g_idirty[(uint64_t)ino % IDIRTY_BUCKETS];
    g_idirty[(uint64_t)ino % IDIRTY_BUCKETS] = r;
    g_idirty_n++;
    return r;
}

/* Unlink and free a record (its arrays are the caller's if they were
 * moved out first — pass them NULL via idirty_detach). */
static void idirty_free(struct ino_dirty *r)
{
    struct ino_dirty **pp = &g_idirty[(uint64_t)r->ino % IDIRTY_BUCKETS];

    while (*pp && *pp != r)
        pp = &(*pp)->next;
    if (*pp)
        *pp = r->next;
    if (r->ino_marked && g_client.dirty_ino_count)
        g_client.dirty_ino_count--;
    if (g_client.dirty_chunk_count >= r->chunk_count)
        g_client.dirty_chunk_count -= r->chunk_count;
    else
        g_client.dirty_chunk_count = 0;
    free(r->chunk_keys);
    free(r->chunk_inos);
    free(r->chunk_idxs);
    free(r);
    if (g_idirty_n)
        g_idirty_n--;
}

static void dirty_sets_clear(void)
{
    for (size_t b = 0; b < IDIRTY_BUCKETS; b++)
        while (g_idirty[b])
            idirty_free(g_idirty[b]);
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_count = 0;
    g_idirty_n = 0;
}

void efs_client_dirty_sets_free(void)
{
    dirty_sets_clear();
}

/* The inodes a REPORT is currently publishing (W41: one slot per
 * in-flight REPORT; a whole-set REPORT parks its snapshot's ino set, a
 * per-inode REPORT parks the ino). dirty_snap_save_locked detaches the
 * live marks the moment a report starts, so without this an inode looks
 * clean from snapshot until the owner has actually applied the size --
 * and lookup_walk would then serve the owner's older size while this
 * client still holds the newer one. A second REPORT of an inode that is
 * in flight waits here (sync) or leaves it for the next kick (async), so
 * two REPORTs never carry the same chunk. Guarded by g_client.dirty_mu;
 * pub_cv is broadcast when a slot is released. */
#define PUB_SLOTS 64
struct pub_slot {
    int used;
    efs_ino_t ino;       /* single-inode REPORT */
    uint64_t *keys;      /* whole-set REPORT: alias of ds->ino_keys */
    uint64_t mask;
    uint64_t pressure_records, since_ms;
};
static struct pub_slot pub_slots[PUB_SLOTS];
static pthread_cond_t pub_cv = PTHREAD_COND_INITIALIZER;

/* Caller holds dirty_mu. */
static int pub_has_locked(efs_ino_t ino)
{
    for (int i = 0; i < PUB_SLOTS; i++) {
        if (!pub_slots[i].used)
            continue;
        if (pub_slots[i].ino == ino)
            return 1;
        if (dirty_set_has(pub_slots[i].keys, pub_slots[i].mask, ino))
            return 1;
    }
    return 0;
}

static int pub_full_locked(void)
{
    for (int i = 0; i < PUB_SLOTS; i++)
        if (!pub_slots[i].used)
            return 0;
    return 1;
}

/* Caller holds dirty_mu. Returns the slot index, or -1 when all are in
 * use (the caller waits on pub_cv). */
static int pub_take_locked(efs_ino_t ino, uint64_t *keys, uint64_t mask)
{
    for (int i = 0; i < PUB_SLOTS; i++) {
        if (pub_slots[i].used)
            continue;
        pub_slots[i].used = 1;
        pub_slots[i].ino = ino;
        pub_slots[i].keys = keys;
        pub_slots[i].mask = mask;
        return i;
    }
    return -1;
}

/* Drop the alias. Must run before the snapshot array is freed or merged
 * back on every exit path, or ino_is_dirty reads freed memory. */
static void pub_release(int slot)
{
    if (slot < 0)
        return;
    pthread_mutex_lock(&g_client.dirty_mu);
    pub_slots[slot].used = 0;
    pub_slots[slot].ino = 0;
    pub_slots[slot].keys = NULL;
    pub_slots[slot].mask = 0;
    pub_slots[slot].pressure_records = 0;
    pub_slots[slot].since_ms = 0;
    pthread_cond_broadcast(&pub_cv);
    pthread_mutex_unlock(&g_client.dirty_mu);
}

/* Admission counts live marks, detached REPORTs and concurrent requests.
 * One chunk is charged twice: its chunk record and companion inode record.
 * This is a conservative REPORT workload budget, separate from data buffers. */
#define REPORT_PRESSURE_RECORD_BYTES 2048ull
_Static_assert(sizeof(struct efs_chunk_rec) + sizeof(struct efs_ino_size_rec) +
               sizeof(uint64_t) <= REPORT_PRESSURE_RECORD_BYTES,
               "REPORT admission byte charge must cover a chunk and inode");
static uint64_t report_reserved, report_waits, report_timeouts;
static uint64_t report_limit_records, report_limit_bytes, report_admit_ms;
static pthread_once_t report_config_once = PTHREAD_ONCE_INIT;
static __thread uint64_t report_request_records;
static void meta_flush_enqueue(efs_ino_t ino);

static uint64_t report_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t report_env(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    if (!value || !*value || *value == '-')
        return fallback;
    errno = 0;
    unsigned long long n = strtoull(value, &end, 10);
    return errno || *end || !n ? fallback : n;
}

static void report_config(void)
{
    report_limit_records = report_env("EFS_REPORT_MAX_RECORDS", 8192);
    report_limit_bytes = report_env("EFS_REPORT_MAX_BYTES", 16ull << 20);
    report_admit_ms = report_env("EFS_REPORT_ADMIT_MS", 8000);
    if (report_admit_ms > 60000)
        report_admit_ms = 60000;
}

/* dirty_mu held. Detached snapshots remain charged until release. */
static void report_pressure_locked(struct efs_report_pressure *out, int with_age)
{
    memset(out, 0, sizeof(*out));
    out->pending_records = g_client.dirty_ino_count + 2 * g_client.dirty_chunk_count;
    uint64_t oldest = 0, now = report_clock_ms();
    for (size_t b = 0; with_age && b < IDIRTY_BUCKETS; ++b)
        for (struct ino_dirty *r = g_idirty[b]; r; r = r->next)
            if ((r->ino_marked || r->chunk_count) &&
                (!oldest || r->since_ms < oldest))
                oldest = r->since_ms;
    for (int i = 0; i < PUB_SLOTS; ++i) {
        if (!pub_slots[i].used || !pub_slots[i].pressure_records)
            continue;
        out->pending_records += pub_slots[i].pressure_records;
        out->inflight_records += pub_slots[i].pressure_records;
        if (!oldest || pub_slots[i].since_ms < oldest)
            oldest = pub_slots[i].since_ms;
    }
    out->reserved_records = report_reserved;
    out->pending_bytes = out->pending_records * REPORT_PRESSURE_RECORD_BYTES;
    out->oldest_ms = oldest && now > oldest ? now - oldest : 0;
    out->admission_waits = report_waits;
    out->admission_timeouts = report_timeouts;
}

void efs_client_report_pressure_stats(struct efs_report_pressure *out)
{
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    report_pressure_locked(out, 1);
    pthread_mutex_unlock(&g_client.dirty_mu);
}

int efs_client_report_admit(uint64_t records)
{
    if (!g_client.meta_batch || !records)
        return EFS_OK;
    if (report_request_records)
        return EFS_ERR_INVAL; /* caller must finish its previous reservation */
    pthread_once(&report_config_once, report_config);
    uint64_t limit = report_limit_records;
    if (limit > report_limit_bytes / REPORT_PRESSURE_RECORD_BYTES)
        limit = report_limit_bytes / REPORT_PRESSURE_RECORD_BYTES;
    if (records > limit)
        return EFS_ERR_BUSY;
    efs_client_ensure_dir_locks();
    uint64_t start = report_clock_ms();
    int waited = 0;
    for (;;) {
        struct efs_report_pressure stats;
        pthread_mutex_lock(&g_client.dirty_mu);
        report_pressure_locked(&stats, 0);
        uint64_t now = report_clock_ms();
        uint64_t request_deadline = efs_client_rpc_deadline_ms();
        if ((waited && now - start >= report_admit_ms) ||
            (request_deadline && now >= request_deadline)) {
            ++report_timeouts;
            pthread_mutex_unlock(&g_client.dirty_mu);
            return EFS_ERR_BUSY;
        }
        if (stats.pending_records <= limit - records &&
            report_reserved <= limit - records - stats.pending_records) {
            report_reserved += records;
            report_request_records = records;
            pthread_mutex_unlock(&g_client.dirty_mu);
            return EFS_OK;
        }
        if (!waited) {
            report_pressure_locked(&stats, 1);
            ++report_waits;
            waited = 1;
            fprintf(stderr, "report-pressure records=%llu inflight=%llu bytes=%llu "
                    "reserved=%llu oldest_ms=%llu waits=%llu timeouts=%llu\n",
                    (unsigned long long)stats.pending_records,
                    (unsigned long long)stats.inflight_records,
                    (unsigned long long)stats.pending_bytes,
                    (unsigned long long)stats.reserved_records,
                    (unsigned long long)stats.oldest_ms,
                    (unsigned long long)report_waits,
                    (unsigned long long)report_timeouts);
        }
        uint64_t elapsed = report_clock_ms() - start;
        uint64_t deadline = efs_client_rpc_deadline_ms();
        uint64_t current = report_clock_ms();
        uint64_t outer = !deadline ? UINT64_MAX :
                         current < deadline ? deadline - current : 0;
        if (elapsed >= report_admit_ms || outer == 0) {
            ++report_timeouts;
            pthread_mutex_unlock(&g_client.dirty_mu);
            fprintf(stderr, "report-admission timeout records=%llu waited_ms=%llu\n",
                    (unsigned long long)records, (unsigned long long)elapsed);
            return EFS_ERR_BUSY;
        }
        pthread_mutex_unlock(&g_client.dirty_mu);
        meta_flush_enqueue(0);
        pthread_mutex_lock(&g_client.dirty_mu);
        /* Drain completion may precede this lock: recheck before waiting so
         * a fast acknowledgement cannot become a lost wakeup. */
        report_pressure_locked(&stats, 0);
        if (stats.pending_records <= limit - records &&
            report_reserved <= limit - records - stats.pending_records) {
            pthread_mutex_unlock(&g_client.dirty_mu);
            continue;
        }
        /* A short realtime wait, bounded by a monotonic overall deadline.
         * Recheck even without a wake: a pool failure retains its marks. */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        uint64_t wait = report_admit_ms - elapsed;
        if (wait > 100) wait = 100;
        if (wait > outer) wait = outer;
        until.tv_nsec += (long)wait * 1000000;
        until.tv_sec += until.tv_nsec / 1000000000;
        until.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&pub_cv, &g_client.dirty_mu, &until);
        pthread_mutex_unlock(&g_client.dirty_mu);
    }
}

void efs_client_report_unreserve(void)
{
    if (!report_request_records)
        return;
    pthread_mutex_lock(&g_client.dirty_mu);
    report_reserved -= report_request_records;
    report_request_records = 0;
    pthread_cond_broadcast(&pub_cv);
    pthread_mutex_unlock(&g_client.dirty_mu);
}

int efs_client_ino_is_dirty(efs_ino_t ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&g_client.dirty_mu);
    int hit = efs_client_ino_is_dirty_locked(ino);
    pthread_mutex_unlock(&g_client.dirty_mu);
    return hit;
}

/* Caller holds g_client.dirty_mu. Used by the staging-table evictor, whose
 * eviction commit already holds dirty_mu (client-cache design Part A). */
int efs_client_ino_is_dirty_locked(efs_ino_t ino)
{
    return idirty_find(ino) != NULL || pub_has_locked(ino);
}

#define MTIME_PIN_MAX 256
static efs_ino_t mtime_pin[MTIME_PIN_MAX];
static int mtime_pin_n;

void efs_client_mtime_pin(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_client.dirty_mu);
    for (int i = 0; i < mtime_pin_n; i++) {
        if (mtime_pin[i] == ino) {
            pthread_mutex_unlock(&g_client.dirty_mu);
            return;
        }
    }
    if (mtime_pin_n < MTIME_PIN_MAX) {
        mtime_pin[mtime_pin_n++] = ino;
    } else {
        /* Only a write() unpins, so a copy of many files fills this
         * and every later pin is dropped. Say so once instead of
         * silently (Sep 30 2026). */
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "mtime-pin: table full (%d), ino=%llu not pinned\n",
                    MTIME_PIN_MAX, (unsigned long long)ino);
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
}

void efs_client_mtime_unpin(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_client.dirty_mu);
    for (int i = 0; i < mtime_pin_n; i++) {
        if (mtime_pin[i] == ino) {
            mtime_pin[i] = mtime_pin[mtime_pin_n - 1];
            mtime_pin_n--;
            break;
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
}

int efs_client_mtime_is_pinned(efs_ino_t ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&g_client.dirty_mu);
    int hit = 0;
    for (int i = 0; i < mtime_pin_n; i++) {
        if (mtime_pin[i] == ino) {
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
    return hit;
}

void efs_client_mark_ino_dirty(efs_ino_t ino)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    struct ino_dirty *r = idirty_get(ino);
    if (!r) {
        pthread_mutex_unlock(&g_client.dirty_mu);
        return;
    }
    if (!r->ino_marked) {
        r->ino_marked = 1;
        g_client.dirty_ino_count++;
    }
    int stripe = (int)((uint64_t)ino % EFS_DIR_LOCKS);
    g_client.last_dirty_stripe = stripe;
    g_client.dirty_stripe_ops[stripe]++;
    pthread_mutex_unlock(&g_client.dirty_mu);
}

void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t chunk_index)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    struct ino_dirty *r = idirty_get(ino);
    uint64_t before = 0;
    if (!r)
        goto out;
    if (r->chunk_keys && r->chunk_mask) {
        uint64_t key = pack_dirty_chunk(ino, chunk_index);
        uint64_t i = key & r->chunk_mask;
        for (uint64_t n = 0; n <= r->chunk_mask; n++) {
            if (r->chunk_keys[i] == 0)
                break;
            if (r->chunk_keys[i] == key)
                goto out;
            i = (i + 1) & r->chunk_mask;
        }
    }
    before = r->chunk_count;
    /* Grow the list before the key goes into the set, so a failed
     * realloc leaves no key without a list entry (the old process-wide
     * set could end with count > listed entries). */
    if (before + 1 > r->chunk_cap) {
        uint64_t ncap = r->chunk_cap ? r->chunk_cap * 2 : 64;
        efs_ino_t *ni = realloc(r->chunk_inos, ncap * sizeof(efs_ino_t));
        uint32_t *nx = realloc(r->chunk_idxs, ncap * sizeof(uint32_t));
        if (ni)
            r->chunk_inos = ni;
        if (nx)
            r->chunk_idxs = nx;
        if (!ni || !nx)
            goto out;
        r->chunk_cap = ncap;
    }
    if (dirty_set_ensure(&r->chunk_keys, &r->chunk_mask, before + 1) != 0)
        goto out;
    dirty_set_put(r->chunk_keys, r->chunk_mask,
                  pack_dirty_chunk(ino, chunk_index), &r->chunk_count);
    if (r->chunk_count == before)
        goto out;
    uint64_t slot = r->chunk_count - 1;
    r->chunk_inos[slot] = ino;
    r->chunk_idxs[slot] = chunk_index;
    g_client.dirty_chunk_count++;
out:
    pthread_mutex_unlock(&g_client.dirty_mu);
}

/* D24 (Oct 1 2026): publish landed chunks while the file is still being
 * written. W30 PUTs every completed chunk during the write, but nothing
 * REPORTed them until close(): a 20 GiB dd's close() was one REPORT of
 * 163 840 records (~27 MB) that the server answered in 2.3 s alone and in
 * minutes when eight such closes met (every recv timeout re-sent the
 * whole REPORT, attempts=6, 116 s; the killed writers sat in D-state
 * in the kernel's forced FLUSH and the mount was wedged for every other
 * process). Now every REPORT_LANDED_CHUNKS landed PUTs (1 GiB) kick the
 * flush thread, which REPORTs the whole dirty set without waiting for a
 * commit (the same records close would send, in the same code path).
 * close() then publishes only the tail. W41 (Oct 2): the count is per
 * inode and the kick names the inode, so each writer's REPORT carries
 * its own ≤ 1 GiB of records and a pool thread publishes it without
 * waiting for the other writers' REPORTs. A REPORT at 8192 records is
 * ~1.4 MB and ~100 ms of server time. Internal constant, not a knob. */
#define REPORT_LANDED_CHUNKS 8192u

static void meta_flush_enqueue(efs_ino_t ino);

/* Called right after efs_client_mark_chunk_dirty(ino, ci) for a landed
 * PUT. The record exists unless a REPORT detached it between the two
 * calls; then this PUT rides in that REPORT's successor. */
static void report_landed_note(efs_ino_t ino)
{
    struct ino_dirty *r;
    unsigned n = 0;

    pthread_mutex_lock(&g_client.dirty_mu);
    r = idirty_find(ino);
    if (r && ++r->landed >= REPORT_LANDED_CHUNKS) {
        n = r->landed;
        r->landed = 0;
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
    if (n) {
        fprintf(stderr, "report-landed: ino=%llu %u chunks landed, kicking REPORT\n",
                (unsigned long long)ino, n);
        meta_flush_enqueue(ino);
    }
}

/* The client-driven metadata flush (flush_snapshot / send_meta_root /
 * META_FLUSH_BEGIN election / take_write_lease) was removed: the server is
 * the sole metadata writer (Phase 2b REPORT_CHUNKS + server-side flush with
 * two-phase root commit). A client that builds and PUT_METAs a root is a
 * rogue second writer — concurrent client heal flushes collided on the same
 * gen/next_ci and tore the CoW pages (the shard-0 "gen raced with GC"
 * wedge). */

/* Dirty state swapped out of g_client at snapshot time. Ops that race the
 * unlocked serialize populate fresh sets and stay dirty for the next flush;
 * on flush failure the saved marks are merged back so nothing is lost. */
struct dirty_snap {
    uint64_t *ino_keys, ino_mask, ino_count;
    uint64_t *chunk_keys, chunk_mask, chunk_count;
    efs_ino_t *chunk_inos;
    uint32_t *chunk_idxs;
    uint64_t chunk_cap;
    uint64_t *ino_slots; /* snapshot-time inode array slots */
    uint64_t *chunk_slots;
    uint64_t layout_epoch;
    uint64_t snap_icount, snap_ccount;
    int meta_dirty;
    uint32_t dirty_ops;
    uint64_t since_ms;
    /* Packed under the table lock from the live export (no full-table copy). */
    char *packed;
    size_t packed_len;
    uint32_t packed_ino_len, packed_ch_len;
    uint8_t *ino_dirty_pg, *ch_dirty_pg;
    int packed_is_cache;
};

/* Move one record's marks into the snapshot (W41). Caller holds
 * dirty_mu; `r` is unlinked and freed. Returns -1 on NOMEM with the
 * record left in place. */
static int dirty_snap_take_record(struct dirty_snap *ds, struct ino_dirty *r)
{
    if (r->ino_marked) {
        if (dirty_set_ensure(&ds->ino_keys, &ds->ino_mask,
                             ds->ino_count + 1) != 0)
            return -1;
        dirty_set_put(ds->ino_keys, ds->ino_mask, r->ino, &ds->ino_count);
    }
    if (r->chunk_count) {
        uint64_t need = ds->chunk_count + r->chunk_count;

        if (need > ds->chunk_cap) {
            uint64_t ncap = ds->chunk_cap ? ds->chunk_cap : 64;
            efs_ino_t *ni;
            uint32_t *nx;

            while (ncap < need)
                ncap *= 2;
            ni = realloc(ds->chunk_inos, ncap * sizeof(*ni));
            if (ni)
                ds->chunk_inos = ni;
            nx = realloc(ds->chunk_idxs, ncap * sizeof(*nx));
            if (nx)
                ds->chunk_idxs = nx;
            if (!ni || !nx)
                return -1;
            ds->chunk_cap = ncap;
        }
        memcpy(ds->chunk_inos + ds->chunk_count, r->chunk_inos,
               r->chunk_count * sizeof(*ds->chunk_inos));
        memcpy(ds->chunk_idxs + ds->chunk_count, r->chunk_idxs,
               r->chunk_count * sizeof(*ds->chunk_idxs));
        ds->chunk_count += r->chunk_count;
    }
    if (!ds->since_ms || r->since_ms < ds->since_ms)
        ds->since_ms = r->since_ms;
    idirty_free(r);
    return 0;
}

/* Detach the marks a REPORT will publish. only_ino: that inode's record
 * (nothing else moves). 0: every record that is not already in flight,
 * plus the process-wide meta_dirty flag. Caller holds the table lock
 * and dirty_mu. ds->chunk_keys stays NULL — the snapshot is a list, the
 * dedupe set belongs to the live record. */
static void dirty_snap_save_locked(struct dirty_snap *ds, efs_ino_t only_ino)
{
    if (only_ino) {
        struct ino_dirty *r = idirty_find(only_ino);

        if (r)
            (void)dirty_snap_take_record(ds, r);
        return;
    }
    ds->meta_dirty = g_client.meta_dirty;
    ds->dirty_ops = g_client.meta_dirty_ops;
    g_client.meta_dirty = 0;
    g_client.meta_dirty_ops = 0;
    for (size_t b = 0; b < IDIRTY_BUCKETS; b++) {
        struct ino_dirty *r = g_idirty[b], *next;

        while (r) {
            next = r->next;
            if (!pub_has_locked(r->ino))
                (void)dirty_snap_take_record(ds, r);
            r = next;
        }
    }
}

static void dirty_snap_free(struct dirty_snap *ds)
{
    free(ds->ino_keys);
    free(ds->chunk_keys);
    free(ds->chunk_inos);
    free(ds->chunk_idxs);
    free(ds->ino_slots);
    free(ds->chunk_slots);
    if (!ds->packed_is_cache)
        free(ds->packed);
    free(ds->ino_dirty_pg);
    free(ds->ch_dirty_pg);
    memset(ds, 0, sizeof(*ds));
}

/* Flush failed: re-mark everything the snapshot covered so the next flush
 * retries it. g_client.lock must be held. The snapshot is NOT freed here
 * (W41: the caller releases the pub slot that aliases ds->ino_keys first,
 * then dirty_snap_free). */
static void dirty_snap_remark_locked(struct dirty_snap *ds)
{
    for (uint64_t i = 0; i <= ds->ino_mask; i++) {
        if (ds->ino_keys && ds->ino_keys[i])
            efs_client_mark_ino_dirty(ds->ino_keys[i]);
    }
    for (uint64_t i = 0; i < ds->chunk_count; i++)
        if (ds->chunk_inos[i])
            efs_client_mark_chunk_dirty(ds->chunk_inos[i], ds->chunk_idxs[i]);
    /* Failure restores the original age as well as the marks. */
    pthread_mutex_lock(&g_client.dirty_mu);
    for (uint64_t i = 0; ds->ino_keys && i <= ds->ino_mask; ++i) {
        struct ino_dirty *r = ds->ino_keys[i] ? idirty_find(ds->ino_keys[i]) : NULL;
        if (r && ds->since_ms && ds->since_ms < r->since_ms)
            r->since_ms = ds->since_ms;
    }
    for (uint64_t i = 0; i < ds->chunk_count; ++i) {
        struct ino_dirty *r = ds->chunk_inos[i] ? idirty_find(ds->chunk_inos[i]) : NULL;
        if (r && ds->since_ms && ds->since_ms < r->since_ms)
            r->since_ms = ds->since_ms;
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
    g_client.meta_dirty |= ds->meta_dirty;
    g_client.meta_dirty_ops += ds->dirty_ops;
}

struct stale_pair {
    efs_ino_t ino;
    uint32_t ci;
    uint32_t idx;    /* index into the dirty snapshot */
    uint8_t valid;   /* successful authoritative repull */
    uint8_t absent;  /* the repull found no row on the host */
};

static int stale_pair_cmp(const void *a, const void *b)
{
    const struct stale_pair *x = a, *y = b;

    if (x->ino != y->ino)
        return x->ino < y->ino ? -1 : 1;
    if (x->ci != y->ci)
        return x->ci < y->ci ? -1 : 1;
    return 0;
}

/* STALE recovery for one report round. The server aborts the batch at the
 * first losing CAS but commits the prefix, so most chunks in a round did
 * NOT move. The old code re-pulled and re-replayed every dirty chunk every
 * round — one GETCHUNKS plus a full 128 KiB GET+PUT re-merge per chunk —
 * which at 36 ranks on one shared file is ~1 GB of EC traffic per client
 * per round and livelocks (9x4 ior-hard: 58-443 s fsync reports, one
 * 85-min report then STALE EIO). Pull the map in bulk (64 recs/RPC over
 * gap-tolerant runs) and replay only chunks whose committed gen moved.
 *
 * The host commits every rec that is not a CAS loser and returns STALE for
 * the residual, so after a STALE round most of OUR recs are committed: the
 * committed gen is our object gen. Those chunks are done — advance the
 * slot base and drop them from the snapshot. Treating them as "moved"
 * re-merged and re-PUT all of them and resent them on the OLD base, which
 * lost to itself every round (9x4 ior-hard: 15860/15860 replayed x 64
 * rounds, 572 s, then EIO) and each round's fresh gens failed every other
 * rank's CAS too. */
static uint64_t report_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* The replays of one STALE round, run REPLAY_THREADS wide. Each replay is
 * a fetch (2 fragment GETs plus a GET per span) and a 3-fragment PUT,
 * 5-12 ms serial; the 9x4 ior-hard round replays ~2600 boundary chunks
 * per client, 10-16 s serially, past the 8 s REPORT wall before the
 * second round could send (Sep 30). Different chunks take different
 * slot locks; two chunks on one slot serialize on the slot's io lock. */
#define REPLAY_THREADS 16
struct replay_fan {
    struct stale_pair *pp;
    uint64_t n;
    uint64_t next; /* under mu */
    pthread_mutex_t mu;
    uint64_t deadline;
    int error;
};

static void *replay_fan_thread(void *arg)
{
    struct replay_fan *f = arg;
    uint64_t previous = efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(f->deadline);

    for (;;) {
        uint64_t k;

        pthread_mutex_lock(&f->mu);
        if (f->next >= f->n) {
            pthread_mutex_unlock(&f->mu);
            break;
        }
        if (efs_client_rpc_past_deadline()) {
            if (!f->error)
                f->error = EFS_ERR_BUSY;
            pthread_mutex_unlock(&f->mu);
            break;
        }
        k = f->next++;
        pthread_mutex_unlock(&f->mu);
        int rc = dcache_replay_stale_ex(f->pp[k].ino, f->pp[k].ci,
                                        f->pp[k].absent);
        if (rc != EFS_OK) {
            pthread_mutex_lock(&f->mu);
            if (!f->error)
                f->error = rc;
            pthread_mutex_unlock(&f->mu);
        }
    }
    efs_client_rpc_set_deadline_ms(previous);
    return NULL;
}

static int replay_fan_run(struct stale_pair *rp, uint64_t n)
{
    struct replay_fan f;
    pthread_t tid[REPLAY_THREADS];
    int nt = 0, i;

    if (!n)
        return EFS_OK;
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    f.deadline = efs_client_rpc_deadline_ms();
    f.error = EFS_OK;
    f.pp = rp;
    f.n = n;
    f.next = 0;
    pthread_mutex_init(&f.mu, NULL);
    for (i = 0; i < REPLAY_THREADS && (uint64_t)i + 1 < n; i++) {
        if (pthread_create(&tid[nt], NULL, replay_fan_thread, &f) != 0)
            break;
        nt++;
    }
    replay_fan_thread(&f); /* the caller is one of the workers */
    for (i = 0; i < nt; i++)
        pthread_join(tid[i], NULL);
    pthread_mutex_destroy(&f.mu);
    return f.error;
}

static int stale_repull_replay(struct dirty_snap *ds, efs_ino_t only_ino)
{
    struct stale_pair *pp, *rp = NULL;
    uint64_t n = 0, i, runs = 0, replayed = 0, committed = 0;
    uint64_t t_start = report_mono_ms();
    int error = EFS_OK;
    static int dbg = -1;

    if (dbg < 0)
        dbg = getenv("EFS_REPORT_DBG") != NULL;
    if (!ds->chunk_count)
        return EFS_OK;
    pp = malloc((size_t)ds->chunk_count * sizeof(*pp));
    if (pp)
        rp = malloc((size_t)ds->chunk_count * sizeof(*rp));
    if (!rp) {
        free(pp);
        pp = NULL;
    }
    if (!pp)
        return EFS_ERR_NOMEM; /* retain snapshot; no unverifiable replay */
    for (i = 0; i < ds->chunk_count; i++) {
        if (!ds->chunk_inos[i])
            continue;
        if (only_ino && ds->chunk_inos[i] != only_ino)
            continue;
        pp[n].ino = ds->chunk_inos[i];
        pp[n].ci = ds->chunk_idxs[i];
        pp[n].idx = (uint32_t)i;
        pp[n].absent = 0;
        pp[n].valid = 0;
        n++;
    }
    qsort(pp, (size_t)n, sizeof(*pp), stale_pair_cmp);
    /* Gap-tolerant runs, one paged pull per run (64 recs/RPC). A chunk
     * the host has no row for is marked absent: the table then still
     * holds our own PUT's identity and must not read as committed. */
    i = 0;
    while (i < n) {
        efs_ino_t ino = pp[i].ino;
        uint32_t lo = pp[i].ci, hi = pp[i].ci;
        uint64_t j = i + 1, k;
        uint8_t *bits;

        while (j < n && pp[j].ino == ino && pp[j].ci - hi <= 64) {
            if (pp[j].ci > hi)
                hi = pp[j].ci;
            j++;
        }
        bits = calloc(((size_t)(hi - lo) + 8) / 8, 1);
        if (bits) {
            int pull_rc = efs_client_pull_chunks_range_absent(ino, lo, hi + 1, bits);
            if (pull_rc != EFS_OK)
                error = pull_rc;
            for (k = i; k < j; k++) {
                uint32_t b = pp[k].ci - lo;

                pp[k].absent = (bits[b >> 3] >> (b & 7)) & 1u;
                pp[k].valid = pull_rc == EFS_OK;
            }
            free(bits);
        } else {
            error = EFS_ERR_NOMEM; /* absence proof is required */
        }
        runs++;
        i = j;
    }
    /* Classify each chunk against the freshly pulled committed gen. */
    i = 0;
    while (i < n) {
        struct efs_chunk_entry ce;
        struct efs_chunk_rec obj;
        uint64_t j = i + 1, object_seq = 0;
        int done = 0;

        while (j < n && pp[j].ino == pp[i].ino && pp[j].ci == pp[i].ci)
            j++; /* [i,j) = duplicate snapshot entries for one chunk */
        if (!pp[i].valid) {
            i = j;
            continue;
        }
        if (export_chunk_copy(pp[i].ino, pp[i].ci, &ce) != 0) {
            i = j;
            continue; /* truncated away; the rec rebuild skips it */
        }
        /* Committed gen is OUR object (same identity source as the rec
         * builder: putid, then the dcache slot): the host took this rec.
         * A span does not move the base generation; it shows up in the
         * chunk's delta list instead — and only as OUR range: the object
         * name is a content hash, so two clients that merged to the same
         * image name one object under two ranges (IOR hard, Sep 30).
         * A chunk the host has no row for is never done: the table's
         * generation there is our own unreported PUT. */
            memset(&obj, 0, sizeof(obj));
            if ((putid_fill(pp[i].ino, pp[i].ci, &obj, &object_seq) ||
                 dcache_object_of(pp[i].ino, pp[i].ci, &obj, &object_seq)) &&
                !pp[i].absent) {
                uint32_t di;

                if (!obj.delta_len && obj.chunk_generation &&
                    obj.chunk_generation == ce.generation)
                    done = 1;
                for (di = 0; obj.delta_len && di < ce.ndelta && !done;
                     di++) {
                    if (ce.deltas[di].generation != obj.chunk_generation)
                        continue;
                    if (ce.deltas[di].len == 0 ||
                        (ce.deltas[di].off == obj.delta_off &&
                         ce.deltas[di].len == obj.delta_len))
                        done = 1;
                }
            }
            if (done) {
                (void)dcache_note_committed(
                    pp[i].ino, pp[i].ci,
                    obj.delta_len ? ce.generation : obj.chunk_generation,
                    obj.chunk_generation, object_seq);
                putid_drop_snapshot(pp[i].ino, pp[i].ci, obj.chunk_generation,
                                    object_seq);
            }
            if (dcache_trace_on())
                fprintf(stderr,
                        "dcache ino=%llu ci=%u stale-class committed=%llx ours=%llx done=%d base=%llx\n",
                        (unsigned long long)pp[i].ino, pp[i].ci,
                        (unsigned long long)ce.generation,
                        (unsigned long long)obj.chunk_generation, done,
                        (unsigned long long)dcache_base_gen_of(pp[i].ino, pp[i].ci, 0));
        if (done) {
            uint64_t k;

            for (k = i; k < j; k++)
                ds->chunk_inos[pp[k].idx] = 0;
            committed++;
            i = j;
            continue;
        }
        struct efs_chunk_entry observed = ce;
        if (pp[i].absent)
            memset(&observed, 0, sizeof(observed));
        if (dcache_cycle_verdict(pp[i].ino, pp[i].ci, &observed, object_seq))
            error = EFS_ERR_IO;
        /* A verified loser must fetch/rebase before the next publication.
         * An unchanged map is not proof that an unreported image landed. */
        rp[replayed++] = pp[i];
        i = j;
    }
    {
        uint64_t t1 = report_mono_ms();

        if (error == EFS_OK)
            error = replay_fan_run(rp, replayed);
        /* Always logged when a round replayed: this is the wall the
         * REPORT budget pays for (chunks = snapshot entries pulled). */
        if (dbg || replayed)
            fprintf(stderr,
                    "report-stale: chunks=%llu runs=%llu committed=%llu "
                    "replayed=%llu pull_ms=%llu replay_ms=%llu\n",
                    (unsigned long long)n, (unsigned long long)runs,
                    (unsigned long long)committed,
                    (unsigned long long)replayed,
                    (unsigned long long)(t1 - t_start),
                    (unsigned long long)(report_mono_ms() - t1));
    }
    free(rp);
    free(pp);
    return error;
}

/* Phase 2b: report the dirty set to the metadata primary (chunk mappings +
 * inode size/mtime) instead of blob-flushing the whole table. The primary
 * applies them to its in-memory table; the server meta-flush thread persists
 * them (or, for sync=1, commits before replying — the fsync durability
 * barrier). On success the dirty set is dropped; on failure it is merged back
 * so the next flush retries. The dirty set / rebase machinery is unchanged —
 * only the flush mechanism (blob PUT -> targeted RPC) differs. */
int efs_client_report_dirty(int sync)
{
    return efs_client_report_dirty_ino(0, sync);
}

/* only_ino=0 reports the whole dirty set. A per-file fsync passes the ino
 * so 9 concurrent 2g jobs do not build one 147k-rec REPORT (that times
 * out as NET). Other inodes stay dirty. */
#if defined(EFS_FAULTS) && EFS_FAULTS
#ifndef EFS_FAULT_FILE
#define EFS_FAULT_FILE "/tmp/efs/fault"
#endif
static int report_fault_withhold(efs_ino_t ino, uint32_t ci)
{
    char line[128], pair[96];
    const char *target = getenv("EFS_FAULT_WITHHOLD");
    FILE *file = fopen(EFS_FAULT_FILE, "r");
    if (file) {
        target = NULL; /* OFF/empty file overrides the startup environment */
        if (fgets(line, sizeof(line), file) &&
            sscanf(line, "WITHHOLD %95s", pair) == 1)
            target = pair;
        fclose(file);
    }
    unsigned long long target_ino;
    unsigned target_ci;
    char trailing;
    return target && sscanf(target, "%llu:%u%c", &target_ino,
                             &target_ci, &trailing) == 2 &&
           target_ino == (unsigned long long)ino && target_ci == ci;
}
#endif

/* WITHHOLD omits the selected record BEFORE RPC serialization. Successful
 * transmitted records are reconciled through the normal authoritative
 * repull; the local batch remains incomplete while an omitted record exists. */
static int report_send_records(const struct efs_chunk_rec *records, uint32_t n,
                                const struct efs_ino_size_rec *inodes, uint32_t ni,
                                int sync)
{
#if defined(EFS_FAULTS) && EFS_FAULTS
    uint32_t withheld = 0;
    for (uint32_t i = 0; i < n; ++i)
        withheld += report_fault_withhold(records[i].ino, records[i].chunk_index);
    if (withheld) {
        struct efs_chunk_rec *send = malloc((size_t)n * sizeof(*send));
        if (!send)
            return EFS_ERR_NOMEM;
        uint32_t count = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (!report_fault_withhold(records[i].ino, records[i].chunk_index))
                send[count++] = records[i];
        int rc = count || ni ? efs_client_rpc_report_dirty_raft(
            g_client.export_id, send, count, inodes, ni, sync) : EFS_OK;
        free(send);
        return count < n && rc == EFS_OK ? EFS_ERR_STALE : rc;
    }
#endif
    return efs_client_rpc_report_dirty_raft(g_client.export_id, records,
                                            n, inodes, ni, sync);
}

static int report_retry_pause(unsigned delay_us)
{
    uint64_t deadline = efs_client_rpc_deadline_ms();
    uint64_t now = report_clock_ms();
    if (deadline && now >= deadline)
        return EFS_ERR_BUSY;
    if (deadline && deadline - now < (delay_us + 999u) / 1000u)
        delay_us = (unsigned)(deadline - now) * 1000u;
    usleep(delay_us);
    return efs_client_rpc_past_deadline() ? EFS_ERR_BUSY : EFS_OK;
}

static int report_dirty_ino_run(efs_ino_t only_ino, int sync)
{
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    struct dirty_snap ds;
    int pub = -1;
    memset(&ds, 0, sizeof(ds));

    /* W41: no process-wide report lock. An inode is published by at most
     * one REPORT at a time (pub slot). A sync caller (fsync/close/
     * setattr drain) whose inode is in flight waits for that REPORT and
     * then publishes what accumulated since; an async caller leaves it
     * for the next kick. A whole-set REPORT skips in-flight inodes. */
    efs_client_ensure_dir_locks();
    /* Fast exits, no table lock. */
    pthread_mutex_lock(&g_client.dirty_mu);
    if ((only_ino && !sync && pub_has_locked(only_ino)) ||
        (g_idirty_n == 0 && !g_client.meta_dirty && !sync) ||
        (only_ino && !sync && !idirty_find(only_ino))) {
        pthread_mutex_unlock(&g_client.dirty_mu);
        return EFS_OK;
    }
    pthread_mutex_unlock(&g_client.dirty_mu);

    /* Take the pub slot BEFORE detaching the marks, so the inode never
     * reads clean while its REPORT is being set up. Lock order is table
     * lock, then dirty_mu; the wait drops both (the in-flight REPORT
     * needs the table lock to finish). */
    for (;;) {
        efs_client_table_lock();
        pthread_mutex_lock(&g_client.dirty_mu);
        if (!(only_ino && pub_has_locked(only_ino)) &&
            (pub = pub_take_locked(only_ino, NULL, 0)) >= 0)
            break;
        if (!sync) {
            pthread_mutex_unlock(&g_client.dirty_mu);
            efs_client_table_unlock();
            return EFS_OK;
        }
        pthread_mutex_unlock(&g_client.dirty_mu);
        efs_client_table_unlock();
        pthread_mutex_lock(&g_client.dirty_mu);
        while ((only_ino && pub_has_locked(only_ino)) || pub_full_locked()) {
            if (efs_client_rpc_past_deadline()) {
                pthread_mutex_unlock(&g_client.dirty_mu);
                return EFS_ERR_BUSY;
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            uint64_t wait = 50;
            uint64_t deadline = efs_client_rpc_deadline_ms();
            uint64_t now = report_clock_ms();
            if (deadline && now >= deadline) {
                pthread_mutex_unlock(&g_client.dirty_mu);
                return EFS_ERR_BUSY;
            }
            if (deadline && deadline - now < wait)
                wait = deadline - now;
            until.tv_nsec += (long)wait * 1000000;
            until.tv_sec += until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&pub_cv, &g_client.dirty_mu, &until);
        }
        pthread_mutex_unlock(&g_client.dirty_mu);
    }
    dirty_snap_save_locked(&ds, only_ino);
    pub_slots[pub].pressure_records = ds.ino_count + 2 * ds.chunk_count;
    pub_slots[pub].since_ms = ds.since_ms;
    /* Keep these inodes reading as dirty until the owner has the size. */
    if (!only_ino) {
        pub_slots[pub].keys = ds.ino_keys;
        pub_slots[pub].mask = ds.ino_mask;
    }
    pthread_mutex_unlock(&g_client.dirty_mu);

    /* Build chunk + inode size/mtime recs from the live table (still holding
     * the table lock so the entries are consistent). */
    struct efs_chunk_rec *crecs = NULL;
    uint64_t *cseq = NULL;
    struct efs_ino_size_rec *irecs = NULL;
    uint32_t cn = 0, in = 0;
    /* A chunk rec whose ino has no irec makes the server guess the size
     * as the chunk end ((ci+1) × 128 KiB, raft_host.c "sz == 0"), and a
     * later irec cannot shrink it: ecopy's mpfr Makefile.in was 32398
     * bytes on XFS and 131072 on efs (Sep 30 03:18:27, ecopy.strace).
     * The ino mark trails the chunk mark on every write path, so a
     * snapshot taken between the two ships the chunk alone. Count the
     * chunk inos that are not in the ino set and reserve irecs for them. */
    uint64_t extra_cap = 0;
    for (uint64_t i = 0; i < ds.chunk_count; i++) {
        if (ds.chunk_inos[i] &&
            !dirty_set_has(ds.ino_keys, ds.ino_mask, ds.chunk_inos[i]))
            extra_cap++;
    }
    /* calloc: flags/padding must be zero on the wire. */
    if (ds.chunk_count) {
        crecs = calloc(ds.chunk_count, sizeof(*crecs));
        cseq = calloc(ds.chunk_count, sizeof(*cseq));
    }
    if (ds.ino_count + extra_cap)
        irecs = calloc(ds.ino_count + extra_cap, sizeof(*irecs));
    if ((ds.chunk_count && (!crecs || !cseq)) || ((ds.ino_count + extra_cap) && !irecs)) {
        free(crecs);
        free(cseq);
        free(irecs);
        /* table lock held (not dirty_mu — the marks take it internally).
         * Re-mark, then release the slot, then free (see the tail). */
        dirty_snap_remark_locked(&ds);
        pub_release(pub);
        dirty_snap_free(&ds);
        efs_client_table_unlock();
        return EFS_ERR_NOMEM;
    }
    /* W41: a per-inode snapshot holds only only_ino's marks (the old
     * process-wide snapshot re-marked every other inode here). */
    for (uint64_t i = 0; i < ds.chunk_count; i++) {
        struct efs_chunk_entry ce;
        if (!ds.chunk_inos[i])
            continue;
        if (only_ino && ds.chunk_inos[i] != only_ino)
            continue;
        if (efs_export_get_chunk(&g_client.export, ds.chunk_inos[i],
                                 ds.chunk_idxs[i], &ce) != 0) {
            /* truncated away before the report; skip */
            putid_drop(ds.chunk_inos[i], ds.chunk_idxs[i], 0);
            continue;
        }
        crecs[cn].ino = ds.chunk_inos[i];
        crecs[cn].chunk_index = ds.chunk_idxs[i];
        memcpy(crecs[cn].nodes, ce.fragment_nodes, sizeof(crecs[cn].nodes));
        memcpy(crecs[cn].checksums, ce.checksums, sizeof(crecs[cn].checksums));
        /* CAS expected is the dcache base, never the object name. */
        crecs[cn].base_gen = dcache_base_gen_of(ds.chunk_inos[i],
                                               ds.chunk_idxs[i], 0);
        crecs[cn].delta_off = 0;
        crecs[cn].delta_len = 0;
        crecs[cn].delta_base_n = 0;
        crecs[cn].delta_base_seq = 0;
        crecs[cn].publish_epoch = 0;
        crecs[cn].publish_flags = 0;
        cseq[cn] = 0;
        if (!report_fill_owned(ds.chunk_inos[i], ds.chunk_idxs[i],
                               &crecs[cn], &cseq[cn], ce.fragment_nodes)) {
            ds.chunk_inos[i] = 0;
            continue;
        }
        cn++;
    }
    for (uint64_t i = 0; i <= ds.ino_mask; i++) {
        if (!ds.ino_keys || !ds.ino_keys[i])
            continue;
        if (only_ino && ds.ino_keys[i] != only_ino)
            continue;
        struct efs_inode inode;
        if (efs_export_get_inode(&g_client.export, ds.ino_keys[i], &inode) != 0)
            continue; /* unlinked before the report; skip */
        irecs[in].ino = ds.ino_keys[i];
        irecs[in].size = inode.size;
        irecs[in].mtime = inode.mtime;
        irecs[in].mtime_nsec = inode.mtime_nsec;
        if (efs_client_mtime_is_pinned(ds.ino_keys[i]))
            irecs[in].flags = EFS_INO_REC_F_TIMES;
        irecs[in].atime = inode.atime;
        /* pack fields stay zero: there is no packing. */
        in++;
    }
    /* Every chunk rec travels with its ino's size (see extra_cap). Inos in
     * the ino set were handled above (the set test is O(1); the irec scan
     * runs only for the rare chunk-only ino). */
    for (uint32_t c = 0; c < cn && extra_cap; c++) {
        efs_ino_t cino = crecs[c].ino;
        uint32_t k;
        struct efs_inode inode;

        if (dirty_set_has(ds.ino_keys, ds.ino_mask, cino))
            continue;
        for (k = 0; k < in; k++)
            if (irecs[k].ino == cino)
                break;
        if (k < in)
            continue;
        if (efs_export_get_inode(&g_client.export, cino, &inode) != 0) {
            static int logged;
            if (logged < 10) {
                logged++;
                fprintf(stderr,
                        "efs: report chunk rec ino=%llu ci=%u has no staged "
                        "row; server will size it to the chunk end\n",
                        (unsigned long long)cino, crecs[c].chunk_index);
            }
            continue;
        }
        irecs[in].ino = cino;
        irecs[in].size = inode.size;
        irecs[in].mtime = inode.mtime;
        irecs[in].mtime_nsec = inode.mtime_nsec;
        if (efs_client_mtime_is_pinned(cino))
            irecs[in].flags = EFS_INO_REC_F_TIMES;
        irecs[in].atime = inode.atime;
        in++;
        extra_cap--;
    }
    efs_client_table_unlock();

    /* A flush of a clean fd (the rd() after wr() already published)
     * used to send a sync REPORT with no records. That is a Raft
     * commit per read-close — names_crazy_dirs pays it once per name. */
    if (cn == 0 && in == 0) {
        free(crecs);
        free(cseq);
        free(irecs);
        pub_release(pub);
        dirty_snap_free(&ds);
        return EFS_OK;
    }

    /* Send the report as ONE batch to a dual-host voter (a node that is a
     * voter of every metadata group, so it can apply the whole batch
     * locally). NOT_PRIMARY follows the hint; transient NET/NO_QUORUM/BUSY
     * retried a few times. On failure the whole snap is merged back. */
    int rc = EFS_ERR_NET;
    /* W17.1: one sync report, 8s wall. The data flush is not capped.
     * A killed writer used to sit in this loop (64 STALE rounds, each
     * REPORT up to 16 retries) until the kernel FUSE request returned. */
    struct timespec ts_budget;
    uint64_t budget0 = report_clock_ms();
    uint64_t outer = efs_client_rpc_deadline_ms();
    int hit_budget = 0, nstale = 0, nbusy = 0, rounds = 0;
    uint64_t t_rpc_ms = 0, t_replay_ms = 0, t_sleep_ms = 0;
    if (sync) {
        clock_gettime(CLOCK_MONOTONIC, &ts_budget);
        budget0 = (uint64_t)ts_budget.tv_sec * 1000ull +
                  (uint64_t)ts_budget.tv_nsec / 1000000ull;
        if (!outer || outer > budget0 + 8000ull)
            efs_client_rpc_set_deadline_ms(budget0 + 8000ull);
    }
    /* STALE needs more than the NET budget: the peer fsyncs the same
     * chunk many times (n1 = 16 pwrite+fsync per 128 KiB) and each
     * publish invalidates this client's expected gen. */
    for (int attempt = 0; attempt < 64; attempt++) {
        if (sync) {
            struct timespec ts;
            uint64_t now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (uint64_t)ts.tv_sec * 1000ull +
                  (uint64_t)ts.tv_nsec / 1000000ull;
            if (now >= budget0 + 8000ull ||
                (efs_client_rpc_deadline_ms() &&
                 now >= efs_client_rpc_deadline_ms())) {
                hit_budget = 1;
                break;
            }
        }
        rounds++;
        {
            uint64_t t0 = report_mono_ms();

            for (uint32_t k = 0; k < cn; ++k)
                dcache_cycle_sent(&crecs[k], cseq[k]);
            rc = report_send_records(crecs, cn, irecs, in, sync);
            t_rpc_ms += report_mono_ms() - t0;
        }
        if (rc == EFS_ERR_STALE)
            nstale++;
        else if (rc == EFS_ERR_BUSY)
            nbusy++;
        if (rc == EFS_OK)
            break;
        if (rc == EFS_ERR_STALE) {
            uint32_t k;
            unsigned slp;
            /* An async REPORT does not spin 64 STALE rounds (it used to
             * hold the process-wide report lock while doing so and
             * starved posix symlink/utimens for 15 s after
             * dir_many_files; W41 removed that lock, but a whole-set
             * async REPORT still parks every inode it carries). A later
             * fsync/close retries. */
            if (!sync && attempt >= 2)
                break;
            for (k = 0; k < in; k++) {
                if (irecs[k].ino)
                    (void)report_replay_stale(irecs[k].ino);
            }
            {
                uint64_t t0 = report_mono_ms();

                int replay_rc = stale_repull_replay(&ds, only_ino);
                if (replay_rc != EFS_OK) {
                    rc = replay_rc;
                    break;
                }
                t_replay_ms += report_mono_ms() - t0;
            }
            slp = 2000u * (unsigned)(attempt + 1);
            if (slp > 20000u)
                slp = 20000u;
            uint64_t sleep0 = report_clock_ms();
            int pause_rc = report_retry_pause(slp);
            t_sleep_ms += report_clock_ms() - sleep0;
            if (pause_rc != EFS_OK) {
                rc = pause_rc;
                hit_budget = 1;
                break;
            }
            efs_client_table_lock();
            cn = 0;
            for (uint64_t i = 0; i < ds.chunk_count; i++) {
                struct efs_chunk_entry ce;
                if (!ds.chunk_inos[i])
                    continue;
                if (only_ino && ds.chunk_inos[i] != only_ino)
                    continue;
                if (efs_export_get_chunk(&g_client.export, ds.chunk_inos[i],
                                         ds.chunk_idxs[i], &ce) != 0) {
                    putid_drop(ds.chunk_inos[i], ds.chunk_idxs[i], 0);
                    continue;
                }
                crecs[cn].ino = ds.chunk_inos[i];
                crecs[cn].chunk_index = ds.chunk_idxs[i];
                memcpy(crecs[cn].nodes, ce.fragment_nodes,
                       sizeof(crecs[cn].nodes));
                memcpy(crecs[cn].checksums, ce.checksums,
                       sizeof(crecs[cn].checksums));
                crecs[cn].base_gen = dcache_base_gen_of(ds.chunk_inos[i],
                                                       ds.chunk_idxs[i], 0);
                crecs[cn].delta_off = 0;
                crecs[cn].delta_len = 0;
                crecs[cn].delta_base_n = 0;
                crecs[cn].delta_base_seq = 0;
                crecs[cn].publish_epoch = 0;
                crecs[cn].publish_flags = 0;
                cseq[cn] = 0;
                if (!report_fill_owned(ds.chunk_inos[i], ds.chunk_idxs[i],
                                       &crecs[cn], &cseq[cn], ce.fragment_nodes)) {
                    ds.chunk_inos[i] = 0;
                    continue;
                }
                cn++;
            }
            efs_client_table_unlock();
            continue;
        }
        /* NOT_FOUND here is not a missing inode: the server skips a
         * deleted row and replies OK. What still arrives is a Raft
         * index a snapshot ate under the propose. Retry it like BUSY.
         * Failing the first one is the 9-client fsync EIO. */
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM &&
            rc != EFS_ERR_NOT_PRIMARY && rc != EFS_ERR_BUSY &&
            rc != EFS_ERR_NOT_FOUND)
            break;
        if (attempt >= 7)
            break;
        uint64_t sleep0 = report_clock_ms();
        int pause_rc = report_retry_pause(50000u << (attempt < 4 ? attempt : 3));
        t_sleep_ms += report_clock_ms() - sleep0;
        if (pause_rc != EFS_OK) {
            rc = pause_rc;
            hit_budget = 1;
            break;
        }
    }
    efs_client_rpc_set_deadline_ms(outer);
    if (hit_budget) {
        struct timespec ts;
        uint64_t now, ms;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000ull +
              (uint64_t)ts.tv_nsec / 1000000ull;
        ms = now > budget0 ? now - budget0 : 0;
        fprintf(stderr, "report-loop ino=%llu rounds=%d stale=%d busy=%d "
                "ms=%llu rpc_ms=%llu replay_ms=%llu sleep_ms=%llu recs=%u rc=%d\n",
                (unsigned long long)only_ino, rounds, nstale, nbusy,
                (unsigned long long)ms, (unsigned long long)t_rpc_ms,
                (unsigned long long)t_replay_ms,
                (unsigned long long)t_sleep_ms, cn, rc);
        if (rc == EFS_OK)
            rc = EFS_ERR_IO;
    }
    if (dcache_trace_on()) {
        for (uint32_t k = 0; k < cn; k++)
            fprintf(stderr, "report ino=%llu ci=%u only=%llu sync=%d rc=%d\n",
                    (unsigned long long)crecs[k].ino, crecs[k].chunk_index,
                    (unsigned long long)only_ino, sync, rc);
        for (uint32_t k = 0; k < in; k++)
            fprintf(stderr, "report-irec ino=%llu size=%llu only=%llu sync=%d rc=%d\n",
                    (unsigned long long)irecs[k].ino,
                    (unsigned long long)irecs[k].size,
                    (unsigned long long)only_ino, sync, rc);
    }
    if (rc == EFS_OK) {
        /* Every rec committed: its object gen is now the slot's CAS base.
         * Otherwise the NEXT publish of a chunk this client already owns
         * (n1: 16 pwrite+fsync per chunk) CASes on the pre-publish base,
         * loses to itself, and pays a full GET+PUT replay round. */
        for (uint32_t k = 0; k < cn; k++) {
            (void)dcache_note_committed(
                crecs[k].ino, crecs[k].chunk_index,
                crecs[k].delta_len ? crecs[k].base_gen
                                   : crecs[k].chunk_generation,
                crecs[k].chunk_generation, cseq[k]);
            putid_drop_snapshot(crecs[k].ino, crecs[k].chunk_index,
                                crecs[k].chunk_generation, cseq[k]);
        }
    }
    free(crecs);
    free(cseq);
    free(irecs);
    if (rc == EFS_OK) {
        /* The owner now has these sizes: stop reporting them as
         * in-flight. */
        pub_release(pub);
        dirty_snap_free(&ds);
    } else {
        /* Re-mark first, release the slot second, so the inodes never
         * read clean in between (the evictor would drop a row with
         * unpublished chunks). The slot's alias of ds.ino_keys is
         * dropped before the snapshot is freed. */
        efs_client_table_lock();
        dirty_snap_remark_locked(&ds);
        pub_release(pub);
        dirty_snap_free(&ds);
        efs_client_table_unlock();
        g_client.report_flush_failed = 1;
    }
    return rc;
}


int efs_client_report_dirty_ino(efs_ino_t only_ino, int sync)
{
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (sync) {
        uint64_t deadline = report_clock_ms() + 8000;
        if (!outer || deadline < outer)
            efs_client_rpc_set_deadline_ms(deadline);
    }
    int rc = report_dirty_ino_run(only_ino, sync);
    if (sync)
        efs_client_rpc_set_deadline_ms(outer);
    return rc;
}


/* W41: the flush pool. Each queued entry is an inode (D24: that writer
 * crossed REPORT_LANDED_CHUNKS landed PUTs) or 0 (the op-count
 * threshold: REPORT the whole dirty set). A pool thread pops one and
 * runs the blocking REPORT off the FUSE worker threads; an inode that
 * is already in flight is left for its next kick by report_dirty_ino.
 * The queue dedupes, so a burst of kicks for one inode is one entry. */
static void meta_flush_enqueue(efs_ino_t ino)
{
    unsigned i;

    if (!g_client.meta_flush_started) {
        /* No pool (C tests / before enable): the next forced REPORT
         * carries it. */
        efs_client_ensure_dir_locks();
        pthread_mutex_lock(&g_client.dirty_mu);
        g_client.meta_dirty = 1;
        pthread_mutex_unlock(&g_client.dirty_mu);
        return;
    }
    pthread_mutex_lock(&g_client.meta_flush_mu);
    for (i = 0; i < g_client.meta_flush_qn; i++) {
        unsigned k = (g_client.meta_flush_qh + i) % EFS_META_FLUSH_QLEN;

        if (g_client.meta_flush_q[k] == ino) {
            pthread_mutex_unlock(&g_client.meta_flush_mu);
            return;
        }
    }
    if (g_client.meta_flush_qn == EFS_META_FLUSH_QLEN) {
        /* Full: collapse into a whole-set REPORT (0 = everything). */
        g_client.meta_flush_q[g_client.meta_flush_qh] = 0;
        g_client.meta_flush_qn = 1;
    } else {
        unsigned k = (g_client.meta_flush_qh + g_client.meta_flush_qn) %
                     EFS_META_FLUSH_QLEN;

        g_client.meta_flush_q[k] = ino;
        g_client.meta_flush_qn++;
    }
    pthread_cond_signal(&g_client.meta_flush_cv);
    pthread_mutex_unlock(&g_client.meta_flush_mu);
}

static void *meta_flush_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_client.meta_flush_mu);
    for (;;) {
        efs_ino_t ino;

        while (g_client.meta_flush_qn == 0 && !g_client.meta_flush_stop)
            pthread_cond_wait(&g_client.meta_flush_cv,
                              &g_client.meta_flush_mu);
        if (g_client.meta_flush_stop)
            break;
        ino = g_client.meta_flush_q[g_client.meta_flush_qh];
        g_client.meta_flush_qh = (g_client.meta_flush_qh + 1) %
                                 EFS_META_FLUSH_QLEN;
        g_client.meta_flush_qn--;
        pthread_mutex_unlock(&g_client.meta_flush_mu);
        /* Phase 2b: background flush reports dirty state via RPC. */
        (void)efs_client_report_dirty_ino(ino, 0);
        pthread_mutex_lock(&g_client.meta_flush_mu);
    }
    pthread_mutex_unlock(&g_client.meta_flush_mu);
    return NULL;
}

void efs_client_enable_meta_batch(uint32_t every_n_ops)
{
    g_client.meta_batch = 1;
    g_client.meta_batch_ops = every_n_ops ? every_n_ops : 4096;
    g_client.meta_dirty = 0;
    g_client.meta_dirty_ops = 0;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    dirty_sets_clear();
    pthread_mutex_unlock(&g_client.dirty_mu);

    if (!g_client.meta_flush_started) {
        pthread_mutex_init(&g_client.meta_flush_mu, NULL);
        pthread_cond_init(&g_client.meta_flush_cv, NULL);
        g_client.meta_flush_qh = 0;
        g_client.meta_flush_qn = 0;
        g_client.meta_flush_stop = 0;
        g_client.meta_flush_n = 0;
        for (int i = 0; i < EFS_META_FLUSH_POOL; i++) {
            if (pthread_create(&g_client.meta_flush_tids[g_client.meta_flush_n],
                               NULL, meta_flush_main, NULL) != 0)
                break;
            g_client.meta_flush_n++;
        }
        if (g_client.meta_flush_n > 0)
            g_client.meta_flush_started = 1;
    }

    /* Bound the staging table (client-cache design Part A). */
    efs_client_stage_evict_start();
}



void efs_client_stop_meta_flush(void)
{
    if (!g_client.meta_flush_started)
        return;
    pthread_mutex_lock(&g_client.meta_flush_mu);
    g_client.meta_flush_stop = 1;
    pthread_cond_broadcast(&g_client.meta_flush_cv);
    pthread_mutex_unlock(&g_client.meta_flush_mu);
    for (int i = 0; i < g_client.meta_flush_n; i++)
        pthread_join(g_client.meta_flush_tids[i], NULL);
    g_client.meta_flush_n = 0;
    g_client.meta_flush_started = 0;
    /* W41: a REPORT that was in flight on a pool thread and failed has
     * re-marked its inodes; nothing kicks the pool after this point, so
     * publish what is left once, synchronously. */
    pthread_mutex_lock(&g_client.dirty_mu);
    int left = g_idirty_n > 0;
    pthread_mutex_unlock(&g_client.dirty_mu);
    if (left)
        (void)efs_client_report_dirty_ino(0, 1);
}

void efs_client_kick_meta_flush(void)
{
    meta_flush_enqueue(0);
}

int efs_client_note_meta_change(int force)
{
    const char *skip = getenv("EFS_SKIP_META_FLUSH");
    if (skip && *skip && strcmp(skip, "0") != 0)
        return EFS_OK;
    if (force)
        g_client.meta_flush_force = 1;
    if (!g_client.meta_batch || force)
        /* Forced flush (fsync/unmount) or non-batched C API: durable report. */
        return efs_client_report_dirty(1);

    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    g_client.meta_dirty = 1;
    g_client.meta_dirty_ops++;
    uint32_t ops = g_client.meta_dirty_ops;
    uint32_t thresh = g_client.meta_batch_ops ? g_client.meta_batch_ops : 4096;
    int stripe = g_client.last_dirty_stripe;
    if (stripe < 0 || stripe >= EFS_DIR_LOCKS)
        stripe = 0;
    uint32_t stripe_ops = g_client.dirty_stripe_ops[stripe];
    int flush = (ops >= thresh) || (stripe_ops >= thresh);
    pthread_mutex_unlock(&g_client.dirty_mu);

    /* Batched threshold flush: hand off to the dedicated flush thread so the
     * O(table) serialize/encode/PUT never runs on a FUSE worker. The thread
     * coalesces all dirty ops into one flush; forced flushes (fsync/unmount)
     * above stay blocking for durability. */
    if (flush) {
        if (g_client.meta_flush_started) {
            meta_flush_enqueue(0);
            return EFS_OK;
        }
        return efs_client_report_dirty(0);
    }
    return EFS_OK;
}

/* Consume one PUT_CHUNK reply from a conn that reply_watch marked ready
 * (or whose poll fd fired). Returns 1 on OK ack, 0 on quota, -1 when the
 * conn was dropped. */
/* Root hints are an evictable placement cache, never retry history. The
 * owning PUT attempt decides NEW; a cache miss on a retry always probes. */
#define PATH_HINT_N 4096
struct path_hint_slot {
    efs_node_id_t nid;
    efs_ino_t ino;
    uint32_t ci;
    uint8_t fi, path, valid;
};
static struct path_hint_slot g_path_hint[PATH_HINT_N];
static pthread_mutex_t g_path_hint_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t path_hint_index(efs_node_id_t nid, efs_ino_t ino, uint32_t ci,
                                uint8_t fi)
{
    uint32_t h = (uint32_t)ino * 1315423911u ^ (ci * 2654435761u) ^
                 ((uint32_t)nid << 8) ^ fi;
    return h % PATH_HINT_N;
}

static uint32_t path_hint_get(efs_node_id_t nid, efs_ino_t ino, uint32_t ci,
                              uint8_t fi)
{
    uint32_t hint = 0;
    pthread_mutex_lock(&g_path_hint_mu);
    struct path_hint_slot *e = &g_path_hint[path_hint_index(nid, ino, ci, fi)];
    if (e->valid && e->nid == nid && e->ino == ino && e->ci == ci && e->fi == fi)
        hint = (uint32_t)e->path + 1u;
    pthread_mutex_unlock(&g_path_hint_mu);
    return hint;
}

static void path_hint_put(efs_node_id_t nid, efs_ino_t ino, uint32_t ci,
                          uint8_t fi, uint8_t path)
{
    if (path == 0xff) return;
    pthread_mutex_lock(&g_path_hint_mu);
    struct path_hint_slot *e = &g_path_hint[path_hint_index(nid, ino, ci, fi)];
    e->nid = nid; e->ino = ino; e->ci = ci; e->fi = fi;
    e->path = path; e->valid = 1;
    pthread_mutex_unlock(&g_path_hint_mu);
}

static int put_recv_reply(efs_node_id_t nid, struct efs_conn *conn,
                          efs_ino_t ino, uint32_t ci, uint8_t fi)
{
    uint8_t reply_type = 0, status = 0, path = 0xff;
    if (efs_conn_recv_put_reply(conn, &reply_type, &status, &path) != 0 ||
        reply_type != EFS_MSG_PUT_CHUNK_REPLY) {
        efs_client_conn_drop(nid, conn);
        efs_client_node_note_fail(nid);
        return -1;
    }
    if (status == EFS_PUT_CHUNK_OK) {
        path_hint_put(nid, ino, ci, fi, path);
        efs_client_node_note_ok(nid);
        efs_client_conn_release(nid, conn);
        return 1;
    }
    if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED) {
        efs_client_conn_release(nid, conn);
        return 0;
    }
    efs_client_conn_drop(nid, conn);
    efs_client_node_note_fail(nid);
    return -1;
}

/* failed_out[i]=1 marks placement slots that need invalidate/retry. */
static int put_fragments_parallel_once(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                                      int failed_out[EFS_NUM_FRAGMENTS], uint64_t object_gen, int first_send)
{
    struct efs_conn *conns[EFS_NUM_FRAGMENTS];
    int pending[EFS_NUM_FRAGMENTS];
    struct efs_msg_put_chunk hdrs[EFS_NUM_FRAGMENTS];
    int acks = 0;
    int quota_errors = 0;
    int reachable = 0;

    if (failed_out) {
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            failed_out[i] = 0;
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        conns[i] = NULL;
        pending[i] = 0;
        memset(&hdrs[i], 0, sizeof(hdrs[i]));
    }

    /* Unreachable peers count as missing acks — do not abort the whole PUT. */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (nodes[i] == 0) {
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        if (efs_client_node_is_down(nodes[i])) {
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        conns[i] = efs_client_conn_get(nodes[i]);
        if (!conns[i]) {
            /* NULL is a connect failure (already counted inside
             * conn_get), a node in cooldown (counting is a no-op), or
             * the pool's 5 s checkout timeout. Counting that last one
             * marked a live node DOWN for 30 s after four full pools —
             * 400 ecopy threads on 16 conns did it twice in a minute and
             * every RPC to the node returned NULL until the cooldown
             * ran out (Sep 30 2026, the 28 s client-wide freezes in
             * results/measure/20260930-051000-review2). */
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        reachable++;
        hdrs[i].export_id = g_client.export_id;
        hdrs[i].ino = ino;
        hdrs[i].chunk_index = chunk_index;
        hdrs[i].fragment_index = (uint32_t)i;
        hdrs[i].data_len = frag_len;
        memcpy(hdrs[i].checksum, checksums[i], EFS_HASH_SIZE);
        hdrs[i].chunk_generation = object_gen;
        /* This new object identity has never been sent on attempt one.
         * Once any send is possible, every later attempt probes or uses an
         * acknowledged root, even if the evictable hint cache lost it. */
        hdrs[i].path_hint = first_send ? EFS_PATH_HINT_NEW :
            path_hint_get(nodes[i], ino, chunk_index, (uint8_t)i);
    }

    /* Send on every live conn before waiting (overlap RTTs). */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!conns[i])
            continue;
        /* Never enter a blocking writev: a slow peer fills the TCP window
         * and SO_SNDTIMEO stalls the whole chunk. Wait for POLLOUT; a miss
         * is backpressure (failed=2), not a dead peer. 8ms was too short
         * under 8-job 1M (many 64 KiB fragments in flight) and inverted
         * that case vs 1-job / 128k. RDMA sends post to the SQ without
         * touching socket buffers, so the check is TCP-only. */
        if (!conns[i]->rc) {
            struct pollfd p = { .fd = conns[i]->fd, .events = POLLOUT };
            int pr = poll(&p, 1, 100);
            if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                continue;
            }
            if (pr == 0 || !(p.revents & POLLOUT)) {
                efs_client_conn_release(nodes[i], conns[i]);
                if (failed_out)
                    failed_out[i] = 2;
                conns[i] = NULL;
                continue;
            }
        }
        if (efs_conn_send_msg_parts(conns[i], EFS_MSG_PUT_CHUNK,
                                    &hdrs[i], (uint32_t)sizeof(hdrs[i]),
                                    fragments[i], frag_len) != 0) {
            efs_client_conn_drop(nodes[i], conns[i]);
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
            conns[i] = NULL;
            continue;
        }
        pending[i] = 1;
    }

    /* Poll for replies; return as soon as we have ≥2 acks so one dead peer
     * cannot serialize a full SO_RCVTIMEO wait. */
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        /* Harvest replies that are already here (RDMA CQE / readable fd).
         * Quick (non-spinning) checks: a full-budget spin per conn per
         * iteration was the single-client CPU sink (up to 6 spins per
         * chunk across the two loops). */
        for (int i = 0; i < EFS_NUM_FRAGMENTS && acks < 2; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch_quick(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i], ino, chunk_index, (uint8_t)i);
                if (r > 0)
                    acks++;
                else if (r == 0)
                    quota_errors++;
                else if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            } else if (w < 0) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            }
        }
        if (acks >= 2)
            break;

        /* Nothing ready: a few pauses on the first pending conn, then
         * the poll below blocks on the CQ event fd. A PUT reply waits
         * on a disk write, so a clock spin here ran out on every chunk. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS && acks < 2; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch_us(conns[i], 24);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i], ino, chunk_index, (uint8_t)i);
                if (r > 0)
                    acks++;
                else if (r == 0)
                    quota_errors++;
                else if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            } else if (w < 0) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            }
            break; /* spin on one conn per iteration at most */
        }
        if (acks >= 2)
            break;

        struct pollfd pfds[EFS_NUM_FRAGMENTS];
        int map[EFS_NUM_FRAGMENTS];
        int npoll = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch_quick(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i], ino, chunk_index, (uint8_t)i);
                if (r > 0)
                    acks++;
                else if (r == 0)
                    quota_errors++;
                else if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            } else if (w < 0) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            }
            pfds[npoll].fd = w;
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = i;
            npoll++;
        }
        if (acks >= 2)
            break;
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!pending[i] || conns[i] == NULL)
                    continue;
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            }
            break;
        }
        /* Cap each poll slice so we re-check the deadline promptly. */
        int wait_ms = left > 2000 ? 2000 : (int)left;
        int pr = poll(pfds, (nfds_t)npoll, wait_ms);
        if (pr < 0)
            continue;
        if (pr == 0)
            continue;

        for (int p = 0; p < npoll; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            int i = map[p];
            if (!pending[i] || !conns[i])
                continue;
            int r = put_recv_reply(nodes[i], conns[i], ino, chunk_index, (uint8_t)i);
            if (r > 0)
                acks++;
            else if (r == 0)
                quota_errors++;
            else if (failed_out)
                failed_out[i] = 1;
            conns[i] = NULL;
            pending[i] = 0;
        }
    }

    /* Quorum met: wait for remaining replies so pooled conns stay reusable.
     * poll(0)+drop used to close the third peer on almost every chunk;
     * 8-job 1M then spent its time in efs_connect_tcp. Cap the drain so a
     * truly stuck peer cannot sit on SO_RCVTIMEO. */
    if (acks >= 2) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 +
                         (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left > 2000)
            left = 2000;
        int64_t drain_end = now_ms + (left > 0 ? left : 0);
        while (now_ms < drain_end) {
            struct pollfd pfds[EFS_NUM_FRAGMENTS];
            int map[EFS_NUM_FRAGMENTS];
            int npoll = 0;
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!conns[i])
                    continue;
                int w = efs_conn_reply_watch_quick(conns[i]);
                if (w == EFS_CONN_REPLY_READY) {
                    put_recv_reply(nodes[i], conns[i], ino, chunk_index, (uint8_t)i);
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                } else if (w < 0) {
                    efs_client_conn_drop(nodes[i], conns[i]);
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                }
                pfds[npoll].fd = w;
                pfds[npoll].events = POLLIN;
                pfds[npoll].revents = 0;
                map[npoll] = i;
                npoll++;
            }
            if (npoll == 0)
                break;
            int wait_ms = (int)(drain_end - now_ms);
            if (wait_ms < 1)
                wait_ms = 1;
            int pr = poll(pfds, (nfds_t)npoll, wait_ms);
            if (pr <= 0)
                break;
            for (int p = 0; p < npoll; p++) {
                if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                    continue;
                int i = map[p];
                if (!conns[i])
                    continue;
                put_recv_reply(nodes[i], conns[i], ino, chunk_index, (uint8_t)i);
                conns[i] = NULL;
                pending[i] = 0;
            }
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_ms = (int64_t)ts.tv_sec * 1000 +
                     (int64_t)ts.tv_nsec / 1000000;
        }
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!conns[i])
            continue;
        efs_client_conn_drop(nodes[i], conns[i]);
        if (acks < 2)
            efs_client_node_note_fail(nodes[i]);
        if (failed_out)
            failed_out[i] = 1;
        conns[i] = NULL;
        pending[i] = 0;
    }

    if (quota_errors >= 2)
        return EFS_ERR_QUOTA;
    if (acks >= 2)
        return EFS_OK;
    if (reachable == 0)
        return EFS_ERR_NET;
    return EFS_ERR_NO_QUORUM;
}

/* If a stripe member is down (or failed the last attempt), put that
 * fragment on an unused live node. Four-node cluster, one down: the
 * remaining three still take a full 2+1 stripe. */
static void reroute_down_fragments(efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                   const int *failed)
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        int bad = (nodes[i] == 0) || efs_client_node_is_down(nodes[i]) ||
                  (failed && failed[i] == 1);
        if (!bad)
            continue;
        for (uint32_t n = 0; n < g_client.node_count; n++) {
            efs_node_id_t id = g_client.nodes[n].id;
            if (id == 0 || efs_client_node_is_down(id))
                continue;
            int used = 0;
            for (int j = 0; j < EFS_NUM_FRAGMENTS; j++) {
                if (j != i && nodes[j] == id)
                    used = 1;
            }
            if (!used) {
                nodes[i] = id;
                break;
            }
        }
    }
}

int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                                      uint64_t *object_out)
{
    uint64_t object_gen = new_chunk_object_gen(chunk_index);
    if (!object_gen) return EFS_ERR_IO;
    if (object_out) *object_out = object_gen;
    /* 2+1 EC requires 3 distinct nodes. On a 1-2 node ring placement wraps and
     * two fragments land on one machine, so a "2-ack quorum" can be a single
     * disk — losing that node loses the chunk. Refuse rather than store
     * undurably. */
    if (nodes[0] == nodes[1] || nodes[1] == nodes[2] || nodes[0] == nodes[2]) {
        fprintf(stderr,
                "put_fragments ino=%llu chunk=%u: EC needs 3 distinct nodes "
                "(got %u,%u,%u) — refusing undurable write\n",
                (unsigned long long)ino, chunk_index,
                (unsigned)nodes[0], (unsigned)nodes[1], (unsigned)nodes[2]);
        fflush(stderr);
        return EFS_ERR_NO_QUORUM;
    }

    /* Retry transient blips; only invalidate peers that actually failed. */
    int rc = EFS_ERR_NET;
    int failed[EFS_NUM_FRAGMENTS] = {0, 0, 0};
    for (int attempt = 1; attempt <= 4; attempt++) {
        reroute_down_fragments(nodes, attempt == 1 ? NULL : failed);
        rc = put_fragments_parallel_once(ino, chunk_index, nodes, fragments,
                                         frag_len, checksums, failed, object_gen, attempt == 1);
        if (rc == EFS_OK || rc == EFS_ERR_QUOTA)
            return rc;
        /* Only hard I/O failures invalidate a node's idle pool. failed[] is
         * also set for POLLOUT soft-misses (window backpressure) and down-skip,
         * which must NOT trigger a reconnect storm — the node is not dead. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (failed[i] == 1 && nodes[i] != 0 &&
                !efs_client_node_is_down(nodes[i]))
                efs_client_conn_invalidate_node(nodes[i]);
        }
        /* Fail fast when fewer than 2 peers are even reachable — retrying a
         * quorum we cannot reach only stacks latency. */
        int live = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            if (nodes[i] != 0 && !efs_client_node_is_down(nodes[i]))
                live++;
        if (live < 2) {
            if (attempt == 1) {
                fprintf(stderr,
                        "put_fragments ino=%llu chunk=%u: only %d/%u peers live, "
                        "clearing down-marks and re-probing\n",
                        (unsigned long long)ino, chunk_index, live,
                        (unsigned)EFS_NUM_FRAGMENTS);
                fflush(stderr);
                efs_client_nodes_force_reprobe();
                continue;
            }
            fprintf(stderr,
                    "put_fragments ino=%llu chunk=%u: only %d/%u peers live, "
                    "cannot reach EC quorum — failing fast\n",
                    (unsigned long long)ino, chunk_index, live,
                    (unsigned)EFS_NUM_FRAGMENTS);
            fflush(stderr);
            return EFS_ERR_NO_QUORUM;
        }
        fprintf(stderr,
                "put_fragments ino=%llu chunk=%u nodes=%u,%u,%u "
                "attempt %d/4 failed: %s (efs_rc=%d)%s\n",
                (unsigned long long)ino, chunk_index,
                (unsigned)nodes[0], (unsigned)nodes[1], (unsigned)nodes[2],
                attempt, efs_strerror(rc), rc,
                attempt < 4 ? " — retrying" : " — giving up");
        fflush(stderr);
        /* Exponential backoff + jitter instead of a fixed linear sleep. */
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1); /* 100,200,400ms */
            useconds_t jitter = (useconds_t)(rand() % 50000);
            usleep(base + jitter);
        }
    }
    return rc;
}

/* Assemble one chunk buffer for a write spanning [wr_start, wr_end).
 * Sets *from_zero_out when the chunk was built from a zero base (new or full
 * overwrite), which lets callers skip Blake3 of known-zero halves.
 * Returns EFS_OK on success; a read-modify-write fetch failure is propagated
 * (never silently zero over live data). */
static int assemble_write_chunk(efs_ino_t ino, uint64_t old_size,
                                uint64_t offset, const char *buf,
                                uint64_t chunk_start, uint64_t wr_start,
                                uint64_t wr_end, uint32_t chunk_size,
                                uint8_t *chunk, int *from_zero_out)
{
    int covers_full = (wr_start == chunk_start &&
                       wr_end == chunk_start + chunk_size);
    size_t off_in_chunk = (size_t)(wr_start - chunk_start);
    size_t wr_len = (size_t)(wr_end - wr_start);
    size_t src_off = (size_t)(wr_start - offset);

    if (covers_full) {
        /* Full overwrite: copy user bytes only — no memset+memcpy. */
        memcpy(chunk, buf + src_off, chunk_size);
        *from_zero_out = efs_bytes_are_zero(chunk, chunk_size);
        efs_rdcache_invalidate(ino, (uint32_t)(chunk_start / chunk_size));
        return EFS_OK;
    }

    if (chunk_start >= old_size) {
        *from_zero_out = 1;
        /* Zero only the unwritten regions. */
        if (off_in_chunk > 0)
            memset(chunk, 0, off_in_chunk);
        if (off_in_chunk + wr_len < chunk_size)
            memset(chunk + off_in_chunk + wr_len, 0,
                   chunk_size - off_in_chunk - wr_len);
    } else {
        *from_zero_out = 0;
        size_t existing = (size_t)(old_size - chunk_start);
        if (existing > chunk_size)
            existing = chunk_size;
        uint32_t ci = (uint32_t)(chunk_start / chunk_size);
        /* Do not use rdcache as the RMW base. A prior sub-chunk read
         * caches the full 128 KiB (new 4k + zeros). The next 4k write
         * then PUTs that stale chunk and drops every later 4k in it. */
        if (efs_dcache_get(ino, ci, chunk, chunk_size) != 0) {
            int rrc = efs_client_fetch_published_chunk(ino, ci, chunk,
                                                       chunk_size);
            if (rrc != EFS_OK)
                rrc = efs_client_fetch_published_chunk(ino, ci, chunk,
                                                       chunk_size);
            if (rrc != EFS_OK) {
                struct efs_chunk_entry ce;
                if (export_chunk_copy(ino, ci, &ce) != 0) {
                    *from_zero_out = 1;
                    if (off_in_chunk > 0)
                        memset(chunk, 0, off_in_chunk);
                    if (off_in_chunk + wr_len < chunk_size)
                        memset(chunk + off_in_chunk + wr_len, 0,
                               chunk_size - off_in_chunk - wr_len);
                } else {
                    return rrc;
                }
            } else if (existing < chunk_size) {
                memset(chunk + existing, 0, chunk_size - existing);
            }
        }
    }

    memcpy(chunk + off_in_chunk, buf + src_off, wr_len);
    /* Dirty dcache is the source of truth for partials; skip a second 128 KiB
     * rdcache copy on every RMW. */
    return EFS_OK;
}

/* Hash fragments; when from_zero and a half was not written, reuse the cached
 * zero-fragment digest and skip hashing the identical parity copy. */
static void hash_write_fragments(const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                 uint32_t frag_len, uint32_t chunk_size,
                                 int from_zero, uint64_t chunk_start,
                                 uint64_t wr_start, uint64_t wr_end,
                                 uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    uint32_t half = chunk_size / 2;
    int frag0_zero = 0, frag1_zero = 0;
    if (from_zero) {
        frag0_zero = (wr_start >= chunk_start + half);
        frag1_zero = (wr_end <= chunk_start + half);
    }

    /* Full-chunk zero write: both data halves and parity are zero. */
    if (from_zero && wr_start <= chunk_start &&
        wr_end >= chunk_start + chunk_size) {
        efs_hash_zero_fragment_len(frag_len, checksums[0]);
        efs_hash_zero_fragment_len(frag_len, checksums[1]);
        efs_hash_zero_fragment_len(frag_len, checksums[2]);
        return;
    }

    if (frag0_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[0]);
    else
        efs_hash(fragments[0], frag_len, checksums[0]);

    if (frag1_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[1]);
    else
        efs_hash(fragments[1], frag_len, checksums[1]);

    if (frag0_zero && !frag1_zero)
        memcpy(checksums[2], checksums[1], EFS_HASH_SIZE);
    else if (frag1_zero && !frag0_zero)
        memcpy(checksums[2], checksums[0], EFS_HASH_SIZE);
    else if (frag0_zero && frag1_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[2]);
    else
        efs_hash(fragments[2], frag_len, checksums[2]);
}

/* Per-batch completion tracker. Lives on the put_pool_run caller's stack; the
 * caller waits until every job it enqueued has been drained, so the tracker
 * outlives all pool-thread access. This is what makes the pool reentrant —
 * concurrent writers each wait on their own batch instead of racing on one
 * shared batch slot. */
struct put_batch {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int remaining;
};

struct chunk_put_job {
    efs_ino_t ino;
    uint32_t ci;
    uint64_t old_size;
    uint64_t deadline;
    uint64_t offset;
    const char *buf;
    uint64_t end;
    int rc;
    int deferred;
    uint64_t object_gen;
    int flush_put; /* 1: dcache_put_now(flush_buf) — W3 fsync pipeline */
    const uint8_t *flush_buf;
    uint32_t flush_len;
    uint64_t flush_base_gen;
    uint64_t flush_seq;
    uint32_t flush_delta_off;
    uint32_t flush_delta_len;
    int flush_have_obs;
    struct efs_chunk_entry flush_obs; /* row the merge fetched (or unused) */
    struct put_batch *bp;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

static void *chunk_put_worker(void *arg);
static int put_pool_run(struct chunk_put_job *jobs, uint32_t batch);
static int dcache_put_now(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                          uint32_t chunk_size, uint64_t base_gen,
                          uint64_t snap_seq, uint32_t delta_off,
                          uint32_t delta_len,
                          const struct efs_chunk_entry *obs);

/* Dirty assembled-chunk cache. Partial writes (4k randwrite) used to GET+encode
 * +PUT a full 128 KiB chunk on every FUSE write. Hold the assembled chunk and
 * PUT once on flush / unmount. 65536 × 128 KiB ≈ 8 GiB if every slot is live;
 * buffers are allocated on first store. Collision does not evict (that
 * serialized PUTs on one mutex and tanked 4k IOPS) — the caller PUTs now. */
#define DCACHE_SLOTS  65536
#define DCACHE_SHARDS 64
/* Disjoint dirty ranges kept per chunk. peer_shared_pwrite puts 16
 * non-adjacent 4 KiB blocks in one 128 KiB chunk; the old cap of 8
 * collapsed the rest into one whole-chunk range and an unconditional
 * CAS, so the last fsync of the chunk published this client's image
 * over the peer's blocks (496 of 1000 half-blocks). 32 covers a full
 * chunk of 4 KiB pages. */
#define DCACHE_NR     32
struct dcache_ent {
    /* Optional D25 state; legacy entries leave this NULL. Slot lock owns it. */
    struct efs_writer_state *writer;
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
    int dirty;
    int have_base; /* 1 = data[] is a complete chunk; 0 = sparse patches */
    /* 1 = this slot holds a pin on its ino in g_dcache_pins (set at the
     * clean→dirty transition, cleared when the slot's data is published
     * or dropped). Guards against double-count on re-dirty. */
    int pin_held;
    uint8_t nrange;
    uint32_t roff[DCACHE_NR];
    uint32_t rlen[DCACHE_NR];
    /* Committed generation the slot's dirty ranges were patched onto.
     * EFS_CHUNK_BASE_UNCOND = aligned last-writer-wins (no RMW). */
    uint64_t base_gen;
    /* Object name + placement of the NEWEST-SNAPSHOT successful PUT. REPORT
     * must send these, not a GETCHUNKS-adopted stub (zero checksums + a KV
     * gen that was never written as `{ci}.{fi}.{gen}`). Two flushes of one
     * slot can be in flight (W3 pipeline / concurrent closes); each takes
     * snap_seq at its copy, and dcache_put_now records an object only if its
     * seq >= object_seq — an older image whose PUT lands last must not
     * become "the" object, or the report publishes it, the newer image's
     * STALE round reads committed==ours and drops it, and the later bytes
     * are never republished (concurrent_appends 190/200). */
    uint64_t object_gen;
    uint64_t committed_object;
    uint64_t committed_seq; /* local PUT snapshot whose REPORT was accepted */
    uint64_t snap_seq;
    uint64_t object_seq;
    /* snap_seq of the image currently installed in e->data by a completed
     * flush (dcache_install_image). Same race as object_seq, other side:
     * close A snapshots (dirty=0), a write patches + re-dirties, close B
     * snapshots (dirty=0, its copy has the write), PUT B installs, then
     * the slower PUT A installs — with dirty==0 there is no overlay, so
     * A's image (without the write) replaces B's in e->data. The write is
     * gone from the only unpublished copy and every later PUT of the slot
     * carries the hole (concurrent_appends 196–199/200, 2-byte NUL holes
     * at the same offsets warm and cold, Sep 20). An install older than
     * img_seq is skipped. */
    uint64_t img_seq;
    efs_node_id_t object_nodes[EFS_NUM_FRAGMENTS];
    uint8_t object_cks[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    /* Span identity of that PUT. putid is direct-mapped and can lose a
     * chunk; REPORT's fallback is this slot. delta_len 0 is a full image.
     * Leaving these unset made a collided span report as a full-chunk CAS
     * of a zero-padded buffer, which replaced the trailer and dropped the
     * other ranks' bytes (IOR-hard, first ~200 chunks). */
    uint32_t object_delta_off;
    uint32_t object_delta_len;
    uint32_t object_delta_base_n;
    uint64_t object_delta_base_seq;
    uint64_t mutation;
    struct efs_wb_cycle_state recovery;
    struct efs_wb_cycle_state pending_cycle;
    uint64_t pending_seq;
    int pending_sent;
    uint32_t recovery_cycles;
    uint8_t stalled;
    uint64_t object_file_generation;
    uint64_t object_publish_epoch;
    uint32_t object_publish_flags;
    struct dcache_ent *next;
    /* W18: per-shard dirty list. on_dirty is 1 while linked. Every dirty
     * entry is on its list (dirty ⟹ on_dirty): the close path reads the
     * lists as the index of an inode's dirty chunks. A reclaim thread
     * that picked an entry sets reclaim_claimed instead of unlinking it,
     * so the other threads skip it and the index stays complete; the
     * claim clears with the next link/unlink. */
    struct dcache_ent *dirty_next;
    struct dcache_ent *dirty_prev;
    int on_dirty;
    uint8_t reclaim_claimed;
    /* 1 = present_extra on the inode counts this slot (not in the table). */
    uint8_t present_extra;
};

_Static_assert(sizeof(struct dcache_ent) <= EFS_DCACHE_ENTRY_BUDGET,
               "update request metadata reservation for enlarged dcache entries");

static struct dcache_ent *dcache_find(uint32_t s, efs_ino_t ino, uint32_t ci);
static struct dcache_ent *dcache_find_meta(uint32_t s, efs_ino_t ino, uint32_t ci);

/* Snapshot sequence: ONE monotonic counter for the whole dcache, not a
 * per-entry one. snap_seq / object_seq / img_seq and the putid seq are all
 * compared per (ino,ci), but the slot for an (ino,ci) is dropped and
 * recreated (adopt shrink, truncate, reclaim) while PUTs of the old
 * incarnation are still in flight or already recorded in putid. A per-entry
 * counter restarted at 1 in the new incarnation, so putid_note saw
 * seq 1 < 58 and SKIPPED every PUT of the new slot for the next 57
 * closes: the REPORT kept publishing the old incarnation's last object and
 * the file's tail was never on the server (concurrent_appends cold read:
 * 8/40 files NUL tail, warm read fine, Sep 20). */
static uint64_t g_dcache_seq;

static uint64_t dcache_seq_next(struct dcache_ent *e)
{
    uint64_t s = __atomic_add_fetch(&g_dcache_seq, 1, __ATOMIC_RELAXED);

    e->snap_seq = s;
    return s;
}

static uint64_t dcache_seq_now(void)
{
    return __atomic_load_n(&g_dcache_seq, __ATOMIC_RELAXED);
}

/* EFS_DCACHE_TRACE=1: one stderr line per slot state transition (snapshot,
 * PUT record, replay, install, take). Off by default. */
static int dcache_trace_on(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("EFS_DCACHE_TRACE") != NULL;
    return on;
}

int efs_dcache_trace_on(void)
{
    return dcache_trace_on();
}

static void dcache_trace_ranges(const struct dcache_ent *e, char *buf, size_t n)
{
    size_t k = 0;

    buf[0] = 0;
    for (uint8_t i = 0; i < e->nrange && k + 24 < n; i++)
        k += (size_t)snprintf(buf + k, n - k, "%s[%u,%u)", i ? "," : "",
                              e->roff[i], e->roff[i] + e->rlen[i]);
}

#define DTRACE(e, fmt, ...)                                                    \
    do {                                                                       \
        if (dcache_trace_on() && (e)) {                                        \
            char rb_[256];                                                     \
            dcache_trace_ranges((e), rb_, sizeof(rb_));                        \
            fprintf(stderr,                                                    \
                    "dcache ino=%llu ci=%u " fmt                               \
                    " | dirty=%d hb=%d bg=%llx obj=%llx oseq=%llu sseq=%llu"  \
                    " r=%s\n",                                                 \
                    (unsigned long long)(e)->ino, (e)->ci, ##__VA_ARGS__,      \
                    (e)->dirty, (e)->have_base,                                \
                    (unsigned long long)(e)->base_gen,                         \
                    (unsigned long long)(e)->object_gen,                       \
                    (unsigned long long)(e)->object_seq,                       \
                    (unsigned long long)(e)->snap_seq, rb_);                   \
        }                                                                      \
    } while (0)

/* Per-ino count of dcache slots holding unreported data — the "unreported
 * data" eviction pin (client-cache design Part A, pin rule 1's dcache
 * half). A slot pins from its clean→dirty transition until its PUT lands
 * (dcache_put_now then marks the ino dirty, so the dirty-set pin takes
 * over until the REPORT) or the slot is dropped. Without this, a same-size
 * overwrite (dirty dcache data, ino NOT in the dirty set) left the staged
 * row evictable: the row's loss then lost the flush's set_chunk/size
 * update and mis-truncated efs_dcache_flush_ino to ci=0.
 * Open-addressing, power-of-two, 0 = empty, backward-shift delete.
 * ALL table accesses hold g_dcache_pin_mu; add/release are called with
 * the slot's dcache_mu already held (lock order: dcache_mu → pin_mu;
 * nothing takes them in reverse). */
static uint64_t *g_dcache_pin_keys;   /* ino */
static uint32_t *g_dcache_pin_counts; /* live slots */
static uint64_t g_dcache_pin_mask;
static uint64_t g_dcache_pin_count;
static pthread_mutex_t g_dcache_pin_mu = PTHREAD_MUTEX_INITIALIZER;

static int dcache_pin_ensure(uint64_t need)
{
    if (g_dcache_pin_keys && need * 2 <= g_dcache_pin_mask + 1)
        return 0;
    uint64_t old_mask = g_dcache_pin_mask;
    uint64_t *old_keys = g_dcache_pin_keys;
    uint32_t *old_counts = g_dcache_pin_counts;
    uint64_t cap = g_dcache_pin_mask ? g_dcache_pin_mask + 1 : 1024;
    while (cap < need * 4)
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(*nk));
    uint32_t *nc = calloc(cap, sizeof(*nc));
    if (!nk || !nc) {
        free(nk);
        free(nc);
        return -1;
    }
    g_dcache_pin_keys = nk;
    g_dcache_pin_counts = nc;
    g_dcache_pin_mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & g_dcache_pin_mask;
            while (g_dcache_pin_keys[j])
                j = (j + 1) & g_dcache_pin_mask;
            g_dcache_pin_keys[j] = k;
            g_dcache_pin_counts[j] = old_counts[i];
        }
        free(old_keys);
        free(old_counts);
    }
    return 0;
}

/* Caller holds the slot's dcache_mu; takes pin_mu internally. Best-effort:
 * a failed grow drops the pin (the row may then be evicted under a dirty
 * dcache slot — a bounded-cache miss, never corruption of data already on
 * the servers). */
static void dcache_pin_add(struct dcache_ent *e)
{
    if (e->pin_held || !e->ino)
        return;
    pthread_mutex_lock(&g_dcache_pin_mu);
    if (dcache_pin_ensure(g_dcache_pin_count + 1) == 0) {
        uint64_t i = (uint64_t)e->ino & g_dcache_pin_mask;
        while (g_dcache_pin_keys[i] && g_dcache_pin_keys[i] != (uint64_t)e->ino)
            i = (i + 1) & g_dcache_pin_mask;
        if (!g_dcache_pin_keys[i]) {
            g_dcache_pin_keys[i] = (uint64_t)e->ino;
            g_dcache_pin_count++;
        }
        g_dcache_pin_counts[i]++;
        e->pin_held = 1;
    }
    pthread_mutex_unlock(&g_dcache_pin_mu);
}

/* Caller holds the slot's dcache_mu; takes pin_mu internally. */
static void dcache_pin_release(struct dcache_ent *e)
{
    if (!e->pin_held || e->stalled)
        return;
    e->pin_held = 0;
    pthread_mutex_lock(&g_dcache_pin_mu);
    if (!g_dcache_pin_keys)
        goto out;
    uint64_t i = (uint64_t)e->ino & g_dcache_pin_mask;
    for (uint64_t n = 0; n <= g_dcache_pin_mask; n++) {
        uint64_t k = g_dcache_pin_keys[i];
        if (!k)
            goto out;
        if (k != (uint64_t)e->ino) {
            i = (i + 1) & g_dcache_pin_mask;
            continue;
        }
        if (--g_dcache_pin_counts[i] == 0) {
            /* Backward-shift delete: clear slot i, then rehome any key in
             * the following run whose ideal slot is not in (i, j]. */
            g_dcache_pin_keys[i] = 0;
            g_dcache_pin_count--;
            uint64_t j = (i + 1) & g_dcache_pin_mask;
            while (g_dcache_pin_keys[j]) {
                uint64_t h = g_dcache_pin_keys[j] & g_dcache_pin_mask;
                /* h is in the cyclic interval (i, j] iff moving j's key
                 * into i would cross its home — then it must stay. */
                int in_gap = (i < j) ? (h > i && h <= j)
                                     : (h > i || h <= j);
                if (!in_gap) {
                    g_dcache_pin_keys[i] = g_dcache_pin_keys[j];
                    g_dcache_pin_counts[i] = g_dcache_pin_counts[j];
                    g_dcache_pin_keys[j] = 0;
                    i = j;
                }
                j = (j + 1) & g_dcache_pin_mask;
            }
        }
        goto out;
    }
out:
    pthread_mutex_unlock(&g_dcache_pin_mu);
}

/* Evictor query: 1 if any dcache slot pins this ino. */
int efs_dcache_ino_pinned(efs_ino_t ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&g_dcache_pin_mu);
    int pinned = 0;
    if (g_dcache_pin_keys) {
        uint64_t i = (uint64_t)ino & g_dcache_pin_mask;
        for (uint64_t n = 0; n <= g_dcache_pin_mask; n++) {
            uint64_t k = g_dcache_pin_keys[i];
            if (!k)
                break;
            if (k == (uint64_t)ino) {
                pinned = g_dcache_pin_counts[i] > 0;
                break;
            }
            i = (i + 1) & g_dcache_pin_mask;
        }
    }
    pthread_mutex_unlock(&g_dcache_pin_mu);
    return pinned;
}
static struct {
    pthread_mutex_t shard[DCACHE_SHARDS];
    /* Serializes the network base-read + PUT phase of a flush per shard.
     * dcache_mu is dropped before that I/O (to avoid holding it over the
     * network), so two concurrent flushes of the same chunk could otherwise
     * both read a stale pre-PUT base and the last PUT would wipe the other's
     * just-written ranges (concurrent-append data loss). */
    pthread_mutex_t shard_io[DCACHE_SHARDS];
    struct dcache_ent e[DCACHE_SLOTS];
    struct dcache_ent *dirty_head[DCACHE_SHARDS];
    int inited;
    uint64_t dirty_bytes;
} g_dcache;
static pthread_once_t g_dcache_once = PTHREAD_ONCE_INIT;

static void dcache_note_dirty_bytes(int64_t delta)
{
    if (delta > 0)
        __atomic_add_fetch(&g_dcache.dirty_bytes, (uint64_t)delta,
                           __ATOMIC_RELAXED);
    else if (delta < 0) {
        uint64_t sub = (uint64_t)(-delta);
        uint64_t cur = __atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED);
        while (cur) {
            uint64_t next = cur > sub ? cur - sub : 0;
            if (__atomic_compare_exchange_n(&g_dcache.dirty_bytes, &cur, next,
                                            0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
                break;
        }
    }
}

static void dcache_init(void)
{
    for (int i = 0; i < DCACHE_SHARDS; i++) {
        pthread_mutex_init(&g_dcache.shard[i], NULL);
        pthread_mutex_init(&g_dcache.shard_io[i], NULL);
    }
    __atomic_store_n(&g_dcache.inited, 1, __ATOMIC_RELEASE);
}

static void dcache_ensure(void)
{
    if (__atomic_load_n(&g_dcache.inited, __ATOMIC_ACQUIRE))
        return;
    pthread_once(&g_dcache_once, dcache_init);
}

void efs_dcache_init(void)
{
    dcache_ensure();
}

static uint32_t dcache_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)ci * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(h & (DCACHE_SLOTS - 1));
}

static pthread_mutex_t *dcache_mu(uint32_t slot)
{
    dcache_ensure();
    return &g_dcache.shard[slot & (DCACHE_SHARDS - 1)];
}

static pthread_mutex_t *dcache_io_mu(uint32_t slot)
{
    dcache_ensure();
    return &g_dcache.shard_io[slot & (DCACHE_SHARDS - 1)];
}

/* Caller holds the shard mutex for `slot`. */
static void dcache_dirty_link(struct dcache_ent *e, uint32_t slot)
{
    uint32_t sh;

    if (!e || e->on_dirty)
        return;
    sh = slot & (DCACHE_SHARDS - 1);
    e->dirty_prev = NULL;
    e->dirty_next = g_dcache.dirty_head[sh];
    if (e->dirty_next)
        e->dirty_next->dirty_prev = e;
    g_dcache.dirty_head[sh] = e;
    e->on_dirty = 1;
    e->reclaim_claimed = 0;
}

static void dcache_dirty_unlink(struct dcache_ent *e, uint32_t slot)
{
    uint32_t sh;

    if (!e || !e->on_dirty)
        return;
    sh = slot & (DCACHE_SHARDS - 1);
    if (e->dirty_prev)
        e->dirty_prev->dirty_next = e->dirty_next;
    else
        g_dcache.dirty_head[sh] = e->dirty_next;
    if (e->dirty_next)
        e->dirty_next->dirty_prev = e->dirty_prev;
    e->dirty_next = NULL;
    e->dirty_prev = NULL;
    e->on_dirty = 0;
    e->reclaim_claimed = 0;
}

static void dcache_set_dirty(struct dcache_ent *e, uint32_t slot)
{
    e->dirty = 1;
    dcache_dirty_link(e, slot);
}

/* snap_seq: the slot's dcache_ent.snap_seq taken when `chunk` was copied
 * out (0 = not a dcache snapshot; always recorded).
 * delta_len > 0 publishes [delta_off, +len) as an immutable span. The
 * object itself is a full chunk image (fragment files are the export
 * size); readers copy only that range. The base mapping is not
 * replaced. delta_len == 0 is the full-chunk CAS object. */
/* obs: the chunk row the image was merged from (fetch_published_chunk_obs),
 * or NULL when the image was not merged (fresh chunk, full overwrite, a
 * span). A fold names obs's list; reading the table here instead named
 * whatever a neighbour's GETCHUNKS window installed since the fetch. */
static int chunk_obs_has_live_spans(const struct efs_chunk_entry *obs)
{
    if (!obs)
        return 0;
    if (obs->ndelta > EFS_CHUNK_DELTA_MAX)
        return 1;
    for (uint32_t i = 0; i < obs->ndelta; ++i)
        if (obs->deltas[i].len)
            return 1;
    return 0;
}

static int dcache_put_now_budgeted(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                          uint32_t chunk_size, uint64_t base_gen,
                          uint64_t snap_seq, uint32_t delta_off,
                          uint32_t delta_len,
                          const struct efs_chunk_entry *obs)
{
    uint32_t stripe = chunk_size;
    uint32_t frag_len;
    uint8_t *parity;
    /* The span is a distinct generation of a full chunk image. Fragment
     * files are the export size (O_DIRECT). Readers copy only
     * [delta_off, delta_len); the rest of this object is not the base. */
    int span = delta_len > 0 && (uint64_t)delta_off + delta_len <= chunk_size &&
               delta_len < chunk_size;
    /* CAS expected stays on e->base_gen; this is the object write only. */
    frag_len = stripe / 2;
    parity = efs_buf_alloc(frag_len);
    if (!parity)
        return EFS_ERR_NOMEM;

    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frags[0] = (uint8_t *)(uintptr_t)chunk;
    frags[1] = (uint8_t *)(uintptr_t)(chunk + frag_len);
    frags[2] = parity;
    efs_encode_chunk(chunk, chunk_size, stripe, frags);

    const uint8_t *cfrags[EFS_NUM_FRAGMENTS] = {frags[0], frags[1], frags[2]};
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    int from_zero = efs_bytes_are_zero(chunk, chunk_size);
    hash_write_fragments(cfrags, frag_len, stripe, from_zero, 0, 0,
                         stripe, checksums);

    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);
    uint64_t object_gen = 0;
    int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                               checksums, &object_gen);
    efs_buf_free(parity, frag_len);
    if (rc != EFS_OK)
        return rc;

    /* The servers now hold newer data than any cached copy. A have_base=0
     * flush reads the OLD published chunk as its merge base, which populates
     * the rdcache with stale bytes; without invalidating, a later read serves
     * that stale rdcache entry and loses the just-written update. */
    efs_rdcache_invalidate(ino, ci);

    if (!export_chunk_exists(ino, ci)) {
        int room = efs_client_ensure_meta_room(0, 1);
        if (room != EFS_OK)
            return room;
    }
    if (efs_export_needs_chunk_grow(&g_client.export))
        export_reserve_chunks_locked(4096);
    uint32_t obs_n = 0;
    uint64_t obs_seq = 0;
    {
        struct efs_chunk_entry ce;
        uint64_t obj = object_gen;
        int have = export_chunk_copy(ino, ci, &ce) == 0;

        if (have) {
            obs_n = ce.ndelta;
            obs_seq = ce.delta_seq;
        }
        if (!span) {
            if (obs) {
                obs_n = obs->ndelta;
                obs_seq = obs->delta_seq;
            } else if (base_gen != EFS_CHUNK_BASE_UNCOND) {
                /* W38: a metadata-only observation is not proof that
                 * this body contains those spans. Name the empty list;
                 * a live server list STALEs and replay fetches its bytes.
                 * A true whole-chunk overwrite owns every byte and may
                 * intentionally replace the current list without merging. */
                obs_n = 0;
                obs_seq = 0;
            }
        }
        efs_client_lock_dir(ino);
        pthread_mutex_lock(&g_client.idx_mu);
        if (span) {
            struct efs_chunk_delta d;
            efs_node_id_t znodes[EFS_NUM_FRAGMENTS];
            uint8_t zck[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];

            /* A span must not replace the base image. The first span
             * on an empty chunk plants a generation-0 row to hang off. */
            if (!have) {
                memset(znodes, 0, sizeof(znodes));
                memset(zck, 0, sizeof(zck));
                efs_client_set_chunk(&g_client.export, ino, ci, znodes, zck);
                (void)efs_export_set_chunk_gen(&g_client.export, ino, ci, 0);
            }
            memset(&d, 0, sizeof(d));
            d.off = delta_off;
            d.len = delta_len;
            d.generation = obj;
            d.seq = obs_seq + 1;
            memcpy(d.nodes, nodes, sizeof(d.nodes));
            memcpy(d.checksums, checksums, sizeof(d.checksums));
            if (efs_export_add_chunk_delta(&g_client.export, ino, ci, &d) !=
                EFS_OK) {
                /* Chain growth can race the span decision. Without a
                 * byte-backed observation, converting this object to a
                 * full image could tombstone bytes it never loaded. Keep
                 * the old table/PUT identity and let the caller retain
                 * its dirty body for a fresh merge on retry. */
                if (!obs) {
                    pthread_mutex_unlock(&g_client.idx_mu);
                    efs_client_unlock_dir(ino);
                    return EFS_ERR_STALE;
                }
                obs_n = obs->ndelta;
                obs_seq = obs->delta_seq;
                efs_client_set_chunk(&g_client.export, ino, ci, nodes,
                                     checksums);
                (void)efs_export_set_chunk_gen(
                    &g_client.export, ino, ci,
                    object_gen);
                span = 0;
            }
        } else {
            efs_client_set_chunk(&g_client.export, ino, ci, nodes, checksums);
            /* Table gen is the object name (candidate), not the CAS base.
             * GET reads this; report still uses dcache.base_gen.
             * set_chunk_gen drops the span list this image folds. */
            (void)efs_export_set_chunk_gen(
                &g_client.export, ino, ci,
                object_gen);
        }
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        putid_note(ino, ci, obj, snap_seq, nodes, checksums,
                   span ? delta_off : 0, span ? delta_len : 0, obs_n, obs_seq, obs);
    }
    {
        uint64_t obj = object_gen;
        uint32_t sl = dcache_slot(ino, ci);
        struct dcache_ent *de;
        pthread_mutex_lock(dcache_mu(sl));
        de = dcache_find_meta(sl, ino, ci);
        if (de && (!snap_seq || snap_seq >= de->object_seq)) {
            de->object_gen = obj;
            de->object_seq = snap_seq;
            memcpy(de->object_nodes, nodes, sizeof(de->object_nodes));
            memcpy(de->object_cks, checksums, sizeof(de->object_cks));
            de->object_delta_off = span ? delta_off : 0;
            de->object_delta_len = span ? delta_len : 0;
            de->object_delta_base_n = obs_n;
            de->object_delta_base_seq = obs_seq;
            de->object_file_generation = 0;
            de->object_publish_epoch = obs ? obs->read_view.fence_epoch : 0;
            de->object_publish_flags = EFS_CHUNK_REC_F_FRESH_OBJECT |
                (obs && obs->read_view.count ? EFS_CHUNK_REC_F_CAPTURED_EPOCH : 0);
            DTRACE(de, "put-record obj=%llx seq=%llu",
                   (unsigned long long)obj, (unsigned long long)snap_seq);
        } else {
            DTRACE(de, "put-SKIP-old obj=%llx seq=%llu",
                   (unsigned long long)obj, (unsigned long long)snap_seq);
        }
        pthread_mutex_unlock(dcache_mu(sl));
    }
    efs_client_mark_chunk_dirty(ino, ci);
    report_landed_note(ino);
    /* Mark the ino too so the next report carries its size/mtime irec. The
     * append path reflects the reserved size before the patch, so
     * dcache_note_size sees no growth and skips its mark — and the
     * server-side append barrier releases only when a REPORT grows the
     * table size past the reservation, i.e. only after this PUT landed. */
    efs_client_mark_ino_dirty(ino);
    return EFS_OK;
}

static int dcache_put_now(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                          uint32_t chunk_size, uint64_t base_gen,
                          uint64_t snap_seq, uint32_t delta_off,
                          uint32_t delta_len, const struct efs_chunk_entry *obs)
{
    int rc;
    efs_buf_drain_enter();
    rc = dcache_put_now_budgeted(ino, ci, chunk, chunk_size, base_gen,
                                  snap_seq, delta_off, delta_len, obs);
    efs_buf_drain_leave();
    return rc;
}

static struct dcache_ent *dcache_find(uint32_t s, efs_ino_t ino, uint32_t ci)
{
    for (struct dcache_ent *e = &g_dcache.e[s]; e; e = e->next) {
        if (e->data && e->ino == ino && e->ci == ci)
            return e;
    }
    return NULL;
}

/* A chained node whose chunk was reclaimed (ino and body cleared by the
 * reclaim flush, dcache_flush_slot_inner) stays linked; until Sep 30 2026
 * nothing reused or unlinked it, so every reclaimed chunk left a dead node
 * behind and dcache_find walked all of them for the rest of the mount
 * (efs_dcache_yield_extra — one find — was 6.6 % of a 62 min ecopy
 * profile, dcache_steal_dirty 36.9 % of its last slice). Return such a
 * node reset to the calloc state with its chain link kept, or NULL. A
 * body-less node may also be reused after its own snapshot's REPORT has
 * committed; pending publication identities are never reused. Caller holds
 * the slot's dcache_mu; unlocked work re-finds by identity and sequence. */
static int dcache_metadata_idle(const struct dcache_ent *e)
{
    return !efs_writer_state_owned(e->writer) && !e->stalled && !e->dirty && !e->data && !e->pin_held && !e->on_dirty &&
            !e->reclaim_claimed && !e->present_extra && (!e->ino || !e->object_gen ||
             (e->committed_object == e->object_gen &&
              e->committed_seq == e->object_seq));
}

static struct dcache_ent *dcache_chain_reuse(uint32_t s)
{
    for (struct dcache_ent *e = g_dcache.e[s].next; e; e = e->next) {
        if (dcache_metadata_idle(e)) {
            struct dcache_ent *nx = e->next;

            efs_writer_state_free(e->writer);
            memset(e, 0, sizeof(*e));
            e->next = nx;
            return e;
        }
    }
    return NULL;
}

/* Pressure-only reclamation: fixed heads cost no heap metadata. Linked
 * nodes are removable under their shard lock using the same ownership
 * predicate as reuse. Never discard a pending REPORT or recovery pin. */
void efs_dcache_trim_metadata(void)
{
    dcache_ensure();
    for (uint32_t s = 0; s < DCACHE_SLOTS; ++s) {
        pthread_mutex_t *mu = dcache_mu(s);
        pthread_mutex_lock(mu);
        struct dcache_ent *prev = &g_dcache.e[s];
        while (prev->next) {
            struct dcache_ent *e = prev->next;
            if (dcache_metadata_idle(e)) {
                prev->next = e->next;
                efs_writer_state_free(e->writer);
                efs_buf_metadata_free(e, sizeof(*e));
            } else {
                prev = e;
            }
        }
        pthread_mutex_unlock(mu);
    }
}

/* Same identity match as dcache_find, including a slot whose 128 KiB body
 * was dropped after a full-chunk PUT. Report and CAS base still live on
 * the entry. */
static struct dcache_ent *dcache_find_meta(uint32_t s, efs_ino_t ino, uint32_t ci)
{
    for (struct dcache_ent *e = &g_dcache.e[s]; e; e = e->next) {
        if (e->ino == ino && e->ci == ci)
            return e;
    }
    return NULL;
}

/* Sequential full-chunk overwrite. Append and partial writes keep ranges
 * and must retain the body so a later close does not rebuild from a short
 * published size and zero the prefix. */
static int dcache_full_overwrite(int have_base, uint64_t bg, uint8_t nrange)
{
    return have_base && nrange == 0 && bg == EFS_CHUNK_BASE_UNCOND;
}

/* Hand the slot body to the PUT. No second buffer: a sequential writer
 * fills the next chunk, not this one, while the send is in flight. */
/* PUT windows. A flusher marks a dcache entry clean before its PUT and
 * records the chunk in the dirty set only after it (dcache_put_now's
 * put-record). Between the two the chunk is in no set a REPORT snapshot
 * reads: a utimens flush for the inode found nothing dirty, its REPORT
 * carried nothing, the SETATTR went out, and the in-flight PUT's chunk
 * was published by the close under the new mtime_gen — mtime = close
 * time (ecopy write→futimens→close, 2 of 5760 files, Sep 30 burst).
 * Every window is counted per inode; efs_dcache_flush_ino waits for the
 * inode's windows to close before it returns. Bounded wait: a PUT that
 * hangs is the RPC deadline's problem, not this one. */
#define PUT_WIN_SLOTS 512
static struct {
    efs_ino_t ino;
    uint32_t n;
} put_win[PUT_WIN_SLOTS];
static uint32_t put_win_overflow;
static uint32_t put_win_total; /* open windows, all inodes: read fast path */
static pthread_mutex_t put_win_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t put_win_cv = PTHREAD_COND_INITIALIZER;

static void put_win_open(efs_ino_t ino)
{
    int free_i = -1, i;

    pthread_mutex_lock(&put_win_mu);
    __atomic_fetch_add(&put_win_total, 1, __ATOMIC_RELEASE);
    for (i = 0; i < PUT_WIN_SLOTS; i++) {
        if (put_win[i].n && put_win[i].ino == ino) {
            put_win[i].n++;
            pthread_mutex_unlock(&put_win_mu);
            return;
        }
        if (free_i < 0 && put_win[i].n == 0)
            free_i = i;
    }
    if (free_i >= 0) {
        put_win[free_i].ino = ino;
        put_win[free_i].n = 1;
    } else {
        put_win_overflow++;
    }
    pthread_mutex_unlock(&put_win_mu);
}

static void put_win_close(efs_ino_t ino)
{
    int i, found = 0;

    pthread_mutex_lock(&put_win_mu);
    for (i = 0; i < PUT_WIN_SLOTS; i++) {
        if (put_win[i].n && put_win[i].ino == ino) {
            put_win[i].n--;
            found = 1;
            break;
        }
    }
    if (!found && put_win_overflow)
        put_win_overflow--;
    if (put_win_total)
        __atomic_fetch_sub(&put_win_total, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&put_win_cv);
    pthread_mutex_unlock(&put_win_mu);
}

static int put_win_busy_locked(efs_ino_t ino)
{
    int i;

    if (put_win_overflow)
        return 1;
    for (i = 0; i < PUT_WIN_SLOTS; i++)
        if (put_win[i].n && put_win[i].ino == ino)
            return 1;
    return 0;
}

/* 0 = no window was open, 1 = waited and they closed, EFS_ERR_BUSY =
 * still open after max_ms. */
static int put_win_wait(efs_ino_t ino, unsigned max_ms)
{
    uint64_t end = report_clock_ms() + max_ms;
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (outer && outer < end)
        end = outer;
    int rc = 0;
    pthread_mutex_lock(&put_win_mu);
    while (put_win_busy_locked(ino)) {
        uint64_t now = report_clock_ms();
        if (now >= end) {
            rc = EFS_ERR_BUSY;
            break;
        }
        rc = 1;
        uint64_t wait = end - now;
        if (wait > 50) wait = 50;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)wait * 1000000;
        dl.tv_sec += dl.tv_nsec / 1000000000;
        dl.tv_nsec %= 1000000000;
        int wrc = pthread_cond_timedwait(&put_win_cv, &put_win_mu, &dl);
        if (wrc && wrc != ETIMEDOUT) {
            rc = EFS_ERR_IO;
            break;
        }
    }
    pthread_mutex_unlock(&put_win_mu);
    return rc;
}

/* Read side of the window. A full-image PUT steals the dcache body
 * (snap-steal-full) and the local chunk map names the object only after
 * the PUT lands, so until then the chunk is in no place a reader looks:
 * dcache_copy misses, the map has no row, and the read is a "hole" of
 * zeros (posix writev_readv_chunk_straddle, Oct 1 07:30Z: W30 PUTs the
 * completed chunk 0 during the writev; the readv raced it). A reader
 * waits for the inode's windows first. Lock-free when none is open. */
int efs_dcache_put_win_wait(efs_ino_t ino)
{
    if (!__atomic_load_n(&put_win_total, __ATOMIC_ACQUIRE))
        return 0;
    return put_win_wait(ino, 8000u);
}



int efs_dcache_has(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    int has = dcache_find(s, ino, ci) != NULL;
    pthread_mutex_unlock(mu);
    return has;
}

int efs_dcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    return efs_dcache_copy(ino, ci, 0, dst, len);
}

static int dcache_range_covered(const struct dcache_ent *e, uint32_t off,
                                uint32_t len)
{
    if (!e || !len)
        return 0;
    uint32_t need = off;
    uint32_t end = off + len;
    while (need < end) {
        int hit = 0;
        for (uint8_t i = 0; i < e->nrange; i++) {
            uint32_t a = e->roff[i], b = a + e->rlen[i];
            if (a <= need && need < b) {
                need = b;
                hit = 1;
                break;
            }
        }
        if (!hit)
            return 0;
    }
    return 1;
}

/* Local chunk-map generation for (ino,ci); 0 = no mapping. Read BEFORE
 * taking the slot's dcache_mu (idx_mu is never taken under dcache_mu). */
static uint64_t export_chunk_gen_of(efs_ino_t ino, uint32_t ci)
{
    struct efs_chunk_entry ce;

    return export_chunk_copy(ino, ci, &ce) == 0 ? ce.generation : 0;
}

/* Slot data this client has written but not yet published: the flush
 * clears `dirty` when it snapshots and holds the pin until the PUT lands. */
static int dcache_unpublished(const struct dcache_ent *e)
{
    return e->dirty || e->pin_held ||
           (e->object_gen && (e->committed_object != e->object_gen ||
                              e->committed_seq != e->object_seq));
}

/* 1 = e->data is byte-for-byte the chunk the local chunk map names (gen
 * tg), so the WHOLE image may be served. A have_base image is the base
 * this client fetched at RMW time plus its own ranges; on a shared file
 * peers publish after that fetch, and only the chunk map says whether
 * anyone did. Current iff the map still names our base (nothing landed
 * since; our unpublished ranges are newer than anything remote) or names
 * our own committed object (image == that object). A full-overwrite
 * (UNCOND) slot is entirely ours: current while unpublished, and after
 * publish iff the map names our object. Serving a stale image whole was
 * the IO-500 hard `-W` mismatch: 5053/36000 warm, 0 after a cold remount
 * of the same bytes. */
static int dcache_image_current(const struct dcache_ent *e, uint64_t tg)
{
    if (!e->have_base)
        return 0;
    if (e->object_gen && tg == e->object_gen)
        return 1;
    if (e->base_gen == EFS_CHUNK_BASE_UNCOND)
        return dcache_unpublished(e);
    /* tg == 0: the map names nothing (hole, or truncate range-deleted
     * it). A clean base_gen=0 image there is the pre-truncate chunk —
     * never current (posix trunc_zero_then_high_pwrite read OLDPREFIX). */
    return tg != 0 && tg == e->base_gen;
}

int efs_dcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                    uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return -1;
    uint64_t tg = export_chunk_gen_of(ino, ci);
    int spans = 0;
    {
        struct efs_chunk_entry ce;

        /* Spans hang off the base generation. The cached whole image is
         * that base (or an older fold) and does not include a peer span
         * that landed later. Unpublished ranges are still ours. */
        if (export_chunk_copy(ino, ci, &ce) == 0 &&
            (ce.ndelta > 0 || (ce.read_view.count &&
             ce.read_view.parts[0].len < ce.read_view.chunk_size)))
            spans = 1;
    }
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    /* Whole image only when it is the committed chunk (above). Otherwise
     * only this client's unpublished ranges are authoritative — same-fd
     * read-your-writes after a first 4k on a new file land here; refusing
     * them sent the reader to GET/rdcache zeros (POSIX basic_rdwr_no_reopen).
     * A clean image the map no longer names is skipped: the caller fetches
     * the published chunk (a peer moved it). */
    if (e && (uint64_t)off + len <= e->len &&
        ((dcache_image_current(e, tg) && !spans) ||
         (dcache_unpublished(e) && dcache_range_covered(e, off, len)))) {
        memcpy(dst, e->data + off, len);
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

int efs_dcache_copy_kept(efs_ino_t ino, uint32_t ci, uint32_t off,
                         uint8_t *dst, uint32_t len)
{
    uint32_t s;
    struct dcache_ent *e;

    if (!dst || !len)
        return -1;
    s = dcache_slot(ino, ci);
    pthread_mutex_lock(dcache_mu(s));
    e = dcache_find(s, ino, ci);
    if (e && e->data && e->have_base && (uint64_t)off + len <= e->len) {
        memcpy(dst, e->data + off, len);
        pthread_mutex_unlock(dcache_mu(s));
        return 0;
    }
    pthread_mutex_unlock(dcache_mu(s));
    return -1;
}

int efs_dcache_copy_unpub(efs_ino_t ino, uint32_t ci, uint32_t off,
                          uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->data && !e->have_base &&
        (uint64_t)off + len <= e->len &&
        dcache_range_covered(e, off, len)) {
        memcpy(dst, e->data + off, len);
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

/* dst holds the freshly decoded published chunk (or zeros when there is no
 * mapping). Lay this client's newer bytes over it: the whole image when it
 * is the committed chunk, else ONLY the unpublished ranges. A clean slot
 * the map no longer names contributes nothing — its bytes are either
 * already in dst or superseded by a peer. The old code memcpy'd every
 * have_base image over the fetch, so a real GET still returned the base
 * peers had long since overwritten. */
void efs_dcache_overlay(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    int spans = 0;

    if (!dst || !len)
        return;
    {
        struct efs_chunk_entry ce;

        /* Same rule as efs_dcache_copy: a whole image is the base, and a
         * span hanging off it (this client's or a peer's, folded into the
         * fetch) is not in that image. Stamping the image over the fetch
         * hid the peer span (peer_overlap_pwrite_partial). */
        if (export_chunk_copy(ino, ci, &ce) == 0 &&
            (ce.ndelta > 0 || (ce.read_view.count &&
             ce.read_view.parts[0].len < ce.read_view.chunk_size)))
            spans = 1;
    }
    uint64_t tg = export_chunk_gen_of(ino, ci);
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (!e || !e->data) {
        pthread_mutex_unlock(mu);
        return;
    }
    if (!spans && dcache_image_current(e, tg) && len <= e->len) {
        memcpy(dst, e->data, len);
        pthread_mutex_unlock(mu);
        return;
    }
    if (dcache_unpublished(e)) {
        for (uint8_t i = 0; i < e->nrange; i++) {
            uint32_t a = e->roff[i], n = e->rlen[i];
            if (a >= len || !n || a >= e->len)
                continue;
            if (a + n > len)
                n = len - a;
            if (a + n > e->len)
                n = e->len - a;
            memcpy(dst + a, e->data + a, n);
        }
    }
    pthread_mutex_unlock(mu);
}

/* "Clean" for drop_if_clean means nothing of ours is at risk: not dirty,
 * AND the last object we PUT is the committed base (its REPORT landed).
 * A slot that is dirty==0 but whose object_gen != base_gen has bytes that
 * exist only in that object and in e->data; the REPORT carrying it is
 * still in flight. Dropping it made the next append rebuild the slot from
 * the COMMITTED map gen — one PUT behind — so the rebuilt image lacked the
 * previous close's bytes and every later PUT of the slot published that
 * hole (concurrent_appends: 2-byte NUL at the offset right before each
 * adopt-shrink rebuild, cold and warm; Sep 20 trace ino 14870 off 270).
 * The adopt path hits this on every O_APPEND close: the owner's row
 * (size after the last REPORT) is smaller than the local row (size after
 * the next reservation), so "shrink" fires on our own echo. */
static int dcache_keep_on_drop(const struct dcache_ent *e)
{
    if (efs_writer_state_owned(e->writer) || e->dirty || e->stalled)
        return 1;
    return e->pin_held || (e->object_gen &&
                          (e->committed_object != e->object_gen ||
                           e->committed_seq != e->object_seq));
}

int efs_dcache_bind_writer(efs_ino_t ino, uint64_t generation, uint32_t ci,
                           const struct efs_msg_inode_writer_view_reply *view)
{
    if (!ino || !generation || !view || view->status != EFS_INODE_RPC_OK)
        return EFS_ERR_INVAL;
    struct efs_msg_inode_writer_view request = {ino, generation, ci, 0};
    int rc = efs_writer_view_reply_valid(&request, view);
    if (rc != EFS_OK)
        return rc;
    dcache_ensure();
    uint32_t slot = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(slot);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(slot, ino, ci);
    if (!e || !efs_chunk_size_valid(e->len) || dcache_keep_on_drop(e)) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_BUSY;
    }
    if (e->writer && e->writer->ranges.bytes.chunk_size != e->len) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_INVAL;
    }
    struct efs_writer_ranges next = e->writer ? e->writer->ranges :
        (struct efs_writer_ranges){.bytes.chunk_size = e->len};
    rc = efs_writer_ranges_authority(&next, view);
    if (rc == EFS_OK && !e->writer) {
        e->writer = efs_writer_state_alloc(e->len);
        if (!e->writer)
            rc = EFS_ERR_NOMEM;
    }
    if (rc == EFS_OK) {
        next.ino = ino;
        next.generation = generation;
        next.chunk_index = ci;
        next.observed_epoch = view->authority_epoch;
        e->writer->ranges = next;
    }
    pthread_mutex_unlock(mu);
    return rc;
}

/* Staged typed admission: no RPC under the slot lock and no adoption of
 * previously accepted legacy bytes. Caller obtains lane authority first. */
int efs_dcache_write_lane(efs_ino_t ino, uint64_t generation, uint32_t ci,
    const struct efs_msg_lane_writer_view_reply *view,
    uint32_t off, const uint8_t *src, uint32_t len)
{
    if (!ino || !generation || !view || !src)
        return EFS_ERR_INVAL;
    struct efs_msg_lane_writer_view req = {ino, generation, ci, view->chunk_size};
    int rc = efs_lane_writer_view_reply_valid(&req, view);
    if (rc != EFS_OK)
        return rc;
    dcache_ensure();
    uint32_t slot = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(slot);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(slot, ino, ci);
    if (!e || !e->data || e->stalled ||
        (!efs_writer_state_owned(e->writer) && dcache_keep_on_drop(e))) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_BUSY;
    }
    int was_dirty = e->dirty;
    rc = efs_writer_state_write_lane(&e->writer, view, e->data, e->len, off, src, len);
    if (rc == EFS_OK) {
        ++e->mutation;
        dcache_set_dirty(e, slot);
        dcache_pin_add(e);
        if (!was_dirty)
            dcache_note_dirty_bytes((int64_t)e->len);
    }
    pthread_mutex_unlock(mu);
    return rc;
}

/* Capture ownership and its bytes together; caller reserves output memory
 * before acquiring this lock. Typed publication is still separately gated. */
static int dcache_snapshot_lane(efs_ino_t ino, uint64_t generation, uint32_t ci,
    const struct efs_msg_lane_writer_view_reply *view,
    const struct efs_msg_inode_getchunks_reply *base,
    struct efs_writer_plan *plan, uint8_t *body, uint32_t len, int publication)
{
    if (!ino || !generation || !view || !plan || !body)
        return EFS_ERR_INVAL;
    struct efs_msg_lane_writer_view req = {ino, generation, ci, len};
    int rc = efs_lane_writer_view_reply_valid(&req, view);
    if (rc != EFS_OK)
        return rc;
    dcache_ensure();
    uint32_t slot = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(slot);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(slot, ino, ci);
    if (!e || !e->data || !e->writer || e->len != len || e->stalled)
        rc = EFS_ERR_BUSY;
    else
        rc = efs_writer_state_snapshot_lane(e->writer, view, base, e->data,
                                            len, plan, body);
    if (rc == EFS_OK && publication)
        plan->cache_sequence = dcache_seq_next(e);
    pthread_mutex_unlock(mu);
    return rc;
}
int efs_dcache_snapshot_lane(efs_ino_t ino, uint64_t generation, uint32_t ci,
    const struct efs_msg_lane_writer_view_reply *view,
    const struct efs_msg_inode_getchunks_reply *base,
    struct efs_writer_plan *plan, uint8_t *body, uint32_t len)
{
    return dcache_snapshot_lane(ino,generation,ci,view,base,plan,body,len,0);
}
int efs_dcache_snapshot_publication(efs_ino_t ino, uint64_t generation, uint32_t ci,
    const struct efs_msg_lane_writer_view_reply *view,
    const struct efs_msg_inode_getchunks_reply *base,
    struct efs_writer_plan *plan, uint8_t *body, uint32_t len)
{
    return dcache_snapshot_lane(ino,generation,ci,view,base,plan,body,len,1);
}
int efs_dcache_begin_publication(efs_ino_t ino, uint64_t generation, uint32_t ci,
    const struct efs_writer_plan *plan, uint64_t object)
{
    if (!plan || plan->ino!=ino || plan->generation!=generation || plan->chunk_index!=ci ||
        !plan->cache_sequence) return EFS_ERR_INVAL;
    dcache_ensure();uint32_t slot=dcache_slot(ino,ci);
    pthread_mutex_t *mu=dcache_mu(slot);pthread_mutex_lock(mu);
    struct dcache_ent *e=dcache_find(slot,ino,ci);
    int rc=(!e || !e->writer || e->snap_seq!=plan->cache_sequence) ? EFS_ERR_STALE :
        efs_writer_state_begin_put(e->writer,plan,object,plan->cache_sequence);
    pthread_mutex_unlock(mu);return rc;
}
int efs_dcache_finish_publication(efs_ino_t ino, uint64_t generation, uint32_t ci,
    uint64_t object, uint64_t sequence, int verdict, const struct efs_chunk_rec *put)
{
    dcache_ensure();uint32_t slot=dcache_slot(ino,ci);
    pthread_mutex_t *mu=dcache_mu(slot);pthread_mutex_lock(mu);
    struct dcache_ent *e=dcache_find_meta(slot,ino,ci);
    int rc=EFS_ERR_STALE;struct efs_chunk_rec report;
    if (!e || !e->writer || e->writer->ranges.generation!=generation ||
        !e->writer->has_publication) goto done;
    if (verdict==EFS_OK) {
        if (!put || put->chunk_generation!=object) { rc=EFS_ERR_INVAL;goto done; }
        rc=efs_writer_plan_report(&e->writer->publication.plan,put,&report);
        if (rc!=EFS_OK) goto done;
    }
    rc=efs_writer_state_finish_put(e->writer,object,sequence,verdict);
    if (rc==EFS_OK) {
        e->object_gen=object;e->object_seq=sequence;
        memcpy(e->object_nodes,report.nodes,sizeof(e->object_nodes));
        memcpy(e->object_cks,report.checksums,sizeof(e->object_cks));
        e->object_delta_off=e->object_delta_len=0;
        e->object_delta_base_n=report.delta_base_n;e->object_delta_base_seq=report.delta_base_seq;
        e->object_file_generation=report.file_generation;
        e->object_publish_epoch=report.publish_epoch;e->object_publish_flags=report.publish_flags;
    }
done:
    pthread_mutex_unlock(mu);return rc;
}
int efs_dcache_publication_report(efs_ino_t ino, uint64_t generation, uint32_t ci,
    uint64_t object, uint64_t sequence, struct efs_chunk_rec *out)
{
    if (!out) return EFS_ERR_INVAL;
    dcache_ensure();uint32_t slot=dcache_slot(ino,ci);
    pthread_mutex_t *mu=dcache_mu(slot);pthread_mutex_lock(mu);
    struct dcache_ent *e=dcache_find_meta(slot,ino,ci);
    int rc=EFS_ERR_STALE;
    if (!e || !e->writer || e->writer->ranges.generation!=generation ||
        !e->writer->has_publication || e->object_gen!=object || e->object_seq!=sequence)
        goto done;
    if (!e->writer->publication_ready) { rc=EFS_ERR_BUSY;goto done; }
    if (efs_writer_buffers_overlap(out,sizeof(*out),e,sizeof(*e)) ||
        efs_writer_buffers_overlap(out,sizeof(*out),e->writer,sizeof(*e->writer)) ||
        (e->data && efs_writer_buffers_overlap(out,sizeof(*out),e->data,e->len))) {
        rc=EFS_ERR_INVAL;goto done;
    }
    struct efs_chunk_rec put={0};put.ino=ino;put.chunk_index=ci;put.chunk_generation=object;
    memcpy(put.nodes,e->object_nodes,sizeof(put.nodes));
    memcpy(put.checksums,e->object_cks,sizeof(put.checksums));
    rc=efs_writer_plan_report(&e->writer->publication.plan,&put,out);
done:
    pthread_mutex_unlock(mu);return rc;
}
/* Only an exact successful REPORT may release accepted ownership. An
 * aggregate STALE or transport failure keeps publication/bytes pinned. */
int efs_dcache_complete_publication(efs_ino_t ino, uint64_t generation, uint32_t ci,
    uint64_t object, uint64_t sequence, int verdict)
{
    dcache_ensure();uint32_t slot=dcache_slot(ino,ci);
    pthread_mutex_t *mu=dcache_mu(slot);pthread_mutex_lock(mu);
    struct dcache_ent *e=dcache_find_meta(slot,ino,ci);int rc=EFS_ERR_STALE;
    if (!e || !e->writer || e->writer->ranges.generation!=generation ||
        !e->writer->has_publication ||
        e->writer->publication.object_generation!=object ||
        e->writer->publication.snapshot_sequence!=sequence) goto done;
    rc=efs_writer_state_report(e->writer,object,sequence,verdict);
    if (verdict==EFS_OK && !e->writer->has_publication && (rc==EFS_OK || rc==EFS_ERR_STALE)) {
        e->committed_object=object;e->committed_seq=sequence;
        if (!efs_writer_state_owned(e->writer)) {
            if (e->dirty) dcache_note_dirty_bytes(-(int64_t)e->len);
            dcache_dirty_unlink(e,slot);e->dirty=0;dcache_pin_release(e);
            /* Cached input is an owned overlay, not the newly materialized
             * peer image. Re-read committed data rather than promoting it. */
            if (e->data) efs_buf_free(e->data,e->len);
            e->data=NULL;e->len=0;e->have_base=0;e->nrange=0;
        }
    }
done:
    pthread_mutex_unlock(mu);return rc;
}

/* Bind the wire size and digest once BEFORE any send. A retry must use this
 * request even if the inode grew or shrank while the reply was missing. */
int efs_dcache_publication_request(efs_ino_t ino, uint64_t generation, uint32_t ci,
    uint64_t object, uint64_t sequence, uint64_t size, struct efs_msg_publication *out)
{
    if (!out) return EFS_ERR_INVAL;
    struct efs_chunk_rec rec;
    int rc = efs_dcache_publication_report(ino,generation,ci,object,sequence,&rec);
    if (rc != EFS_OK) return rc;
    dcache_ensure();uint32_t slot=dcache_slot(ino,ci);
    pthread_mutex_t *mu=dcache_mu(slot);pthread_mutex_lock(mu);
    struct dcache_ent *e=dcache_find_meta(slot,ino,ci);
    if (!e || !e->writer || !e->writer->has_publication ||
        e->writer->publication.object_generation!=object ||
        e->writer->publication.snapshot_sequence!=sequence) { rc=EFS_ERR_STALE;goto done; }
    if (efs_writer_buffers_overlap(out,sizeof(*out),e,sizeof(*e)) ||
        efs_writer_buffers_overlap(out,sizeof(*out),e->writer,sizeof(*e->writer)) ||
        (e->data && efs_writer_buffers_overlap(out,sizeof(*out),e->data,e->len))) {
        rc=EFS_ERR_INVAL;goto done;
    }
    struct efs_writer_publication *token=&e->writer->publication;
    struct efs_meta_pub p;uint8_t digest[EFS_HASH_SIZE];
    struct efs_opid identity;
    if (token->result_bound) { size=token->result_size;identity=token->result_id; }
    else {
        rc=efs_client_publication_id(sequence,&identity);
        if (rc!=EFS_OK) goto done;
    }
    rc=efs_publication_from_rec(&rec,size,&p);
    p.publication_id=identity;
    if (rc==EFS_OK)rc=efs_publication_digest(&p,digest);
    if (rc!=EFS_OK)goto done;
    if (token->result_bound && memcmp(token->result_digest,digest,sizeof(digest))) {
        rc=EFS_ERR_STALE;goto done;
    }
    token->result_id=identity;token->result_size=size;memcpy(token->result_digest,digest,sizeof(digest));token->result_bound=1;
    memset(out,0,sizeof(*out));out->rec=rec;out->size=size;out->id=identity;
done:
    pthread_mutex_unlock(mu);return rc;
}
/* Only durable, matching terminal evidence releases the pending identity.
 * Rejection retains every accepted byte; caller must fetch a fresh view before
 * planning another publication. Unknown/transport failure changes nothing. */
int efs_dcache_publication_result(efs_ino_t ino, uint64_t generation, uint32_t ci,
    uint64_t object, uint64_t sequence, const struct efs_msg_publication_reply *reply)
{
    if (!reply || reply->rpc.status!=EFS_INODE_RPC_OK ||
        (reply->state!=EFS_PUBLICATION_COMMITTED && reply->state!=EFS_PUBLICATION_REJECTED) ||
        (reply->state==EFS_PUBLICATION_COMMITTED && reply->verdict!=EFS_OK) ||
        (reply->state==EFS_PUBLICATION_REJECTED && reply->verdict!=EFS_ERR_STALE && reply->verdict!=EFS_ERR_INVAL))
        return EFS_ERR_BUSY;
    dcache_ensure();uint32_t slot=dcache_slot(ino,ci);
    pthread_mutex_t *mu=dcache_mu(slot);pthread_mutex_lock(mu);
    struct dcache_ent *e=dcache_find_meta(slot,ino,ci);int rc=EFS_ERR_STALE;
    if (!e || !e->writer || !e->writer->has_publication ||
        e->writer->ranges.generation!=generation || !e->writer->publication_ready)goto done;
    struct efs_writer_publication *token=&e->writer->publication;
    if (token->object_generation!=object || token->snapshot_sequence!=sequence ||
        !token->result_bound || memcmp(token->result_digest,reply->digest,EFS_HASH_SIZE))goto done;
    if (reply->state==EFS_PUBLICATION_REJECTED) {
        e->writer->has_publication=0;e->writer->publication_ready=0;
        memset(token,0,sizeof(*token));e->object_gen=0;e->object_seq=0;
        rc=reply->verdict;
    } else rc=EFS_OK;
done:
    pthread_mutex_unlock(mu);
    return rc==EFS_OK ? efs_dcache_complete_publication(ino,generation,ci,object,sequence,EFS_OK) : rc;
}


static int dcache_pending_of(efs_ino_t ino, uint32_t ci)
{
    int pending;
    dcache_ensure();
    uint32_t slot = dcache_slot(ino, ci);
    pthread_mutex_lock(dcache_mu(slot));
    struct dcache_ent *e = dcache_find_meta(slot, ino, ci);
    pending = e && dcache_keep_on_drop(e);
    pthread_mutex_unlock(dcache_mu(slot));
    return pending;
}

static void dcache_drop_locked(efs_ino_t ino, uint32_t ci, int skip_dirty)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    int extra = 0;
    pthread_mutex_lock(mu);
    struct dcache_ent *head = &g_dcache.e[s];
    if (head->ino == ino && head->ci == ci) {
        if (efs_writer_state_owned(head->writer) || head->stalled || (skip_dirty && dcache_keep_on_drop(head))) {
            DTRACE(head, "drop-KEEP unreported");
            pthread_mutex_unlock(mu);
            return;
        }
        DTRACE(head, "drop skip_dirty=%d", skip_dirty);
        if (head->dirty && head->len)
            dcache_note_dirty_bytes(-(int64_t)head->len);
        if (head->present_extra) {
            extra = 1;
            head->present_extra = 0;
        }
        dcache_pin_release(head);
        dcache_dirty_unlink(head, s);
        efs_writer_state_free(head->writer);
        efs_buf_free(head->data, head->len);
        if (head->next) {
            struct dcache_ent *n = head->next;
            /* n's pin (if any) travels with the copy into the head slot;
             * head's own pin was released above. The copy must not
             * inherit n's dirty-list links. */
            dcache_dirty_unlink(n, s);
            *head = *n;
            head->on_dirty = 0;
            head->dirty_next = NULL;
            head->dirty_prev = NULL;
            if (head->dirty)
                dcache_dirty_link(head, s);
            efs_buf_metadata_free(n, sizeof(*n));
        } else {
            memset(head, 0, sizeof(*head));
        }
        pthread_mutex_unlock(mu);
        if (extra)
            efs_export_present_add(&g_client.export, ino, 1, -1);
        return;
    }
    struct dcache_ent *prev = head;
    for (struct dcache_ent *e = head->next; e; prev = e, e = e->next) {
        if (e->ino == ino && e->ci == ci) {
            if (efs_writer_state_owned(e->writer) || e->stalled || (skip_dirty && dcache_keep_on_drop(e))) {
                DTRACE(e, "drop-KEEP unreported");
                break;
            }
            DTRACE(e, "drop skip_dirty=%d", skip_dirty);
            if (e->dirty && e->len)
                dcache_note_dirty_bytes(-(int64_t)e->len);
            if (e->present_extra) {
                extra = 1;
                e->present_extra = 0;
            }
            dcache_pin_release(e);
            dcache_dirty_unlink(e, s);
            prev->next = e->next;
            efs_writer_state_free(e->writer);
            efs_buf_free(e->data, e->len);
            efs_buf_metadata_free(e, sizeof(*e));
            break;
        }
    }
    pthread_mutex_unlock(mu);
    if (extra)
        efs_export_present_add(&g_client.export, ino, 1, -1);
}

void efs_dcache_drop(efs_ino_t ino, uint32_t ci)
{
    dcache_drop_locked(ino, ci, 0);
}

void efs_dcache_drop_if_clean(efs_ino_t ino, uint32_t ci)
{
    dcache_drop_locked(ino, ci, 1);
}

/* Slot state a store installs together with the buffer, under the same
 * lock. dcache_take marks the slot dirty; a flusher that took the slot
 * between that and a second locked section saw `dirty=1 nrange=0`, PUT
 * the buffer as a full image, and the merge overlay (which walks nrange)
 * then dropped this write's bytes (IOR hard shared chunks, Sep 30:
 * `snap-inner | dirty=1 hb=1 bg=0 obj=0 r=` on a first partial write). */
struct dcache_init {
    int have_base;
    uint64_t base_gen; /* ignored when full */
    int full;          /* whole-chunk image: nrange 0, UNCOND */
    uint32_t off, len; /* the range this write owns (partial) */
};
/* Caller holds the slot mutex through ownership lookup and installation. */
static int dcache_store_owned_locked(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                              uint32_t chunk_size,
                              const struct dcache_init *in);
static int dcache_merge_owned(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len,
                              uint8_t *chunk, uint32_t cs,
                              const struct dcache_init *in);

static void dcache_add_range(struct dcache_ent *e, uint32_t off, uint32_t len)
{
    if (!e || !len)
        return;
    ++e->mutation;
    e->recovery_cycles = 0;
    e->pending_seq = 0;
    e->pending_sent = 0;
    efs_wb_cycle_reset(&e->recovery);
    uint32_t end = off + len;
    int merged = 0;
    for (uint8_t i = 0; i < e->nrange; i++) {
        uint32_t a = e->roff[i], b = a + e->rlen[i];
        if (off <= b && a <= end) {
            uint32_t lo = a < off ? a : off;
            uint32_t hi = b > end ? b : end;
            e->roff[i] = lo;
            e->rlen[i] = hi - lo;
            merged = 1;
            break;
        }
    }
    if (!merged && e->nrange < DCACHE_NR) {
        e->roff[e->nrange] = off;
        e->rlen[e->nrange] = len;
        e->nrange++;
    } else if (!merged) {
        /* 9th disjoint range: the flush GET+merge only replays nrange
         * windows onto the published base. Dropping one would zero that
         * write. Collapse to the whole chunk so every dirty byte is kept
         * (peer bytes in true holes are the same loss as have_base=1). */
        e->roff[0] = 0;
        e->rlen[0] = e->len ? e->len : end;
        e->nrange = 1;
        /* Overflow: retry is a full overwrite (no base). */
        e->base_gen = EFS_CHUNK_BASE_UNCOND;
        return;
    }
    /* A merge into range i can now touch a neighbour that the first
     * pass did not ( [0,4) + [4,6) after filling the gap ). */
    for (uint8_t i = 0; i < e->nrange; i++) {
        uint32_t a = e->roff[i], b = a + e->rlen[i];
        for (uint8_t j = (uint8_t)(i + 1); j < e->nrange; ) {
            uint32_t c = e->roff[j], d = c + e->rlen[j];
            if (a <= d && c <= b) {
                if (c < a)
                    a = c;
                if (d > b)
                    b = d;
                e->roff[i] = a;
                e->rlen[i] = b - a;
                e->nrange--;
                e->roff[j] = e->roff[e->nrange];
                e->rlen[j] = e->rlen[e->nrange];
                continue;
            }
            j++;
        }
    }
}

/* Atomic full overwrite fast path: preserve the admission budget by using
 * the existing body instead of allocating another chunk on a cache hit. */
static int dcache_patch_full(efs_ino_t ino, uint32_t ci, const uint8_t *src,
                              uint32_t cs)
{
    uint32_t slot = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(slot);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(slot, ino, ci);
    if (!e || !e->data || e->len != cs || e->writer) {
        pthread_mutex_unlock(mu);
        return -1;
    }
    if (!e->dirty) {
        dcache_set_dirty(e, slot);
        dcache_pin_add(e);
        dcache_note_dirty_bytes((int64_t)cs);
    }
    memcpy(e->data, src, cs);
    dcache_add_range(e, 0, cs);
    e->have_base = 1;
    e->nrange = 0;
    e->base_gen = EFS_CHUNK_BASE_UNCOND;
    e->img_seq = dcache_seq_now();
    pthread_mutex_unlock(mu);
    efs_rdcache_invalidate(ino, ci);
    return 0;
}

static int dcache_patch(efs_ino_t ino, uint32_t ci, uint32_t off, const uint8_t *src,
                        uint32_t len)
{
    if (!src || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->data && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        if (!e->dirty) {
            /* The entry is mid-flush: its snapshot was taken and dirty cleared,
             * but e->data is not freed until the flush's post-PUT "!e->dirty"
             * check. Patch it and re-dirty so this write is not lost — that
             * check then leaves the entry in place and a later flush writes it
             * back. Without this, a write landing during another thread's flush
             * falls through to load_and_patch, sees the not-yet-published chunk
             * (the in-flight PUT has not set the mapping), and builds a zeroed
             * have_base=1 entry that overwrites the in-flight data. */
            dcache_set_dirty(e, s);
            dcache_pin_add(e); /* already held mid-flush; keeps dirty ⟹ pinned */
            dcache_note_dirty_bytes((int64_t)e->len);
        }
        dcache_add_range(e, off, len);
        DTRACE(e, "patch off=%u len=%u", off, len);
        pthread_mutex_unlock(mu);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

/* Bounded caller-controlled pressure walk: identities, never entry pointers.
 * Also select clean PUT-complete bodies whose REPORT has not committed. */
efs_ino_t efs_dcache_pressure_ino(efs_ino_t after)
{
    efs_ino_t found = 0;
    dcache_ensure();
    for (uint32_t sh = 0; sh < DCACHE_SHARDS; sh++) {
        pthread_mutex_lock(&g_dcache.shard[sh]);
        for (uint32_t s = sh; s < DCACHE_SLOTS; s += DCACHE_SHARDS)
            for (struct dcache_ent *e = &g_dcache.e[s]; e; e = e->next)
                if (e->ino > after && (e->data || e->object_gen) &&
                    dcache_keep_on_drop(e) &&
                    (!found || e->ino < found))
                    found = e->ino;
        pthread_mutex_unlock(&g_dcache.shard[sh]);
    }
    return found;
}

/* First 4k to a chunk: load the published base NOW (one GET), patch the 4k,
 * and cache the full chunk with have_base=1. The old path zero-filled and
 * set have_base=0 for a published chunk, so EVERY flush of that chunk did a
 * 128 KiB GET+merge+PUT (read-modify-write). With a working set over the
 * dcache limit (random 4k on >= 512 MiB) reclaim runs constantly and the
 * writer thread flushes inline (dirty > 2x cap), so pwrite blocked on the
 * GET+PUT — that is what capped fio rw-4k at ~100 MiB/s. Reading the base
 * once here turns later 4k patches into pure memcpy and the flush into a
 * PUT-only. */
static int dcache_load_and_patch(efs_ino_t ino, uint32_t ci, uint32_t off,
                                 const uint8_t *src, uint32_t len, uint32_t cs)
{
    if (dcache_patch(ino, ci, off, src, len) == 0)
        return 0;

    uint8_t *chunk = efs_buf_alloc(cs);
    if (!chunk)
        return -1;

    int have_base = 0;
    uint64_t bg = 0;
    struct efs_chunk_entry ce;
    if (export_chunk_copy(ino, ci, &ce) == 0)
        bg = ce.generation;
    if (efs_rdcache_get(ino, ci, chunk, cs) == 0) {
        have_base = 1;
    } else {
        if (bg &&
            efs_client_fetch_published_chunk(ino, ci, chunk, cs) == EFS_OK)
            have_base = 1;
        if (!have_base)
            memset(chunk, 0, cs);
    }
    memcpy(chunk + off, src, len);
    {
        struct dcache_init in = { have_base, bg, 0, off, len };
        int folded = dcache_merge_owned(ino, ci, off, src, len, chunk, cs,
                                        &in);

        if (folded < 0) {
            efs_buf_free(chunk, cs);
            return -1;
        }
    }
    return 0;
}

static void dcache_note_size(efs_ino_t ino, uint64_t end)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0) {
        if (cur.size < end) {
            /* Size grow bumps mtime itself. */
            efs_export_set_size_norollup(&g_client.export, ino, end);
            efs_client_mark_ino_dirty(ino);
        } else if (cur.size > 0) {
            /* Overwrite at the same size must still advance the local
             * mtime: reports are newer-only, so a frozen mtime leaves the
             * server at the first writer's mtime and peers never get an
             * adopt trigger to re-pull the layout (mc_stress rwfile
             * identical-content rerun). Field update only — the dirty mark
             * still happens once per flush in dcache_put_now, so rw-4k does
             * not reflood the report path (that flood capped rw-4k before). */
            uint64_t sec;
            uint32_t nsec;
            now_ns(&sec, &nsec);
            if (!efs_client_mtime_is_pinned(ino) &&
                (sec > cur.mtime ||
                 (sec == cur.mtime && nsec > cur.mtime_nsec)))
                efs_export_set_mtime_ns_norollup(&g_client.export, ino,
                                                 sec, nsec);
        }
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
}

static void dcache_kick_complete(void);

static int dcache_store_full_chunk(efs_ino_t ino, uint32_t ci,
                                   const uint8_t *src, uint32_t cs)
{
    if (dcache_patch_full(ino, ci, src, cs) != 0) {
        uint8_t *chunk = efs_buf_alloc(cs);
        if (!chunk)
            return -1;
        memcpy(chunk, src, cs);
        struct dcache_init in = { 1, 0, 1, 0, 0 };
        if (dcache_merge_owned(ino, ci, 0, src, cs, chunk, cs, &in) < 0) {
            efs_buf_free(chunk, cs);
            return -1;
        }
    }
    dcache_kick_complete();
    return 0;
}

/* Whole-chunk body. `chunk` is a pool buffer. Stolen on success.
 * libfuse's request buffer does not outlive the write reply, so the
 * caller copies into `chunk` once and hands that pointer here. */
int efs_dcache_store_full_owned(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                                uint32_t cs)
{
    if (!chunk || !cs)
        return -1;
    if (dcache_patch_full(ino, ci, chunk, cs) == 0) {
        efs_buf_free(chunk, cs);
    } else {
        struct dcache_init in = { 1, 0, 1, 0, 0 };

        if (dcache_merge_owned(ino, ci, 0, chunk, cs, chunk, cs, &in) < 0)
            return -1;
    }
    dcache_note_size(ino, ((uint64_t)ci + 1) * cs);
    dcache_kick_complete();
    return 0;
}

int efs_dcache_try_patch(efs_ino_t ino, uint64_t offset, uint32_t len,
                         const uint8_t *src)
{
    if (!src || !len)
        return -1;
    uint32_t cs = data_chunk_size();
    if (efs_write_extent_valid(offset, len, cs) != EFS_OK)
        return -1;
    uint64_t remaining = len;
    uint64_t pos = offset;
    const uint8_t *p = src;
    while (remaining) {
        uint32_t ci = (uint32_t)(pos / cs);
        uint32_t off = (uint32_t)(pos % cs);
        uint32_t n = cs - off;
        if ((uint64_t)n > remaining)
            n = (uint32_t)remaining;
        int rc;
        if (off == 0 && n == cs)
            rc = dcache_store_full_chunk(ino, ci, p, cs);
        else if (dcache_patch(ino, ci, off, p, n) == 0)
            rc = 0;
        else
            rc = dcache_load_and_patch(ino, ci, off, p, n, cs);
        if (rc != 0)
            return -1;
        pos += n;
        p += n;
        remaining -= n;
    }
    dcache_note_size(ino, offset + len);
    return 0;
}

static int dcache_patch_sparse(efs_ino_t ino, uint32_t ci, uint32_t off,
                               const uint8_t *src, uint32_t len)
{
    if (!src || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->data && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        if (!e->dirty) {
            dcache_set_dirty(e, s);
            dcache_pin_add(e);
            dcache_note_dirty_bytes((int64_t)e->len);
        }
        /* A published cached chunk stays have_base=1 so the next close
         * PUTs it without GET. Ranges stay even then — STALE retry
         * overlays them onto a fresh base. */
        dcache_add_range(e, off, len);
        DTRACE(e, "patch-sparse off=%u len=%u", off, len);
        pthread_mutex_unlock(mu);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

static int dcache_load_sparse(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len, uint32_t cs)
{
    uint8_t *chunk;
    int have_base = 0;
    uint64_t bg = 0;
    struct efs_chunk_entry ce;

    if (dcache_patch_sparse(ino, ci, off, src, len) == 0)
        return 0;

    chunk = efs_buf_alloc(cs);
    if (!chunk)
        return -1;
    /* Same as load_and_patch: if this chunk is already published, RMW
     * onto those bytes. Zeros+range was only correct for a brand-new
     * chunk. After close, adopt can drop the have_base=1 slot; the next
     * O_APPEND then missed the prefix and PUT NULs + new bytes
     * (basic_terminal_cp_cat). Skip GET when there is no mapping — a
     * peer leftover name is the W1 first-fsync DECODE. */
    if (export_chunk_copy(ino, ci, &ce) == 0) {
        bg = ce.generation;
        if (efs_client_fetch_published_chunk(ino, ci, chunk, cs) == EFS_OK ||
            efs_client_fetch_published_chunk(ino, ci, chunk, cs) == EFS_OK)
            have_base = 1;
        else {
            /* A published chunk we could not read. Zeros plus this range
             * would PUT over the prefix (concurrent_appends). */
            efs_buf_free(chunk, cs);
            return -1;
        }
    }
    if (!have_base)
        memset(chunk, 0, cs);
    memcpy(chunk + off, src, len);
    {
        struct dcache_init in = { have_base, bg, 0, off, len };
        int folded = dcache_merge_owned(ino, ci, off, src, len, chunk, cs,
                                        &in);

        if (folded < 0) {
            efs_buf_free(chunk, cs);
            return -1;
        }
        /* Folded into a slot another writer already dirtied. That slot
         * owns the bytes and the ranges. Clearing nrange here kept only
         * this write; the next flush then published the rest as zeros.
         * Otherwise the store installed have_base/base_gen/range with
         * the buffer (dcache_init). */
    }
    return 0;
}

int efs_dcache_try_patch_sparse(efs_ino_t ino, uint64_t offset, uint32_t len,
                                const uint8_t *src)
{
    if (!src || !len)
        return -1;
    uint32_t cs = data_chunk_size();
    if (efs_write_extent_valid(offset, len, cs) != EFS_OK)
        return -1;
    uint64_t remaining = len;
    uint64_t pos = offset;
    const uint8_t *p = src;
    while (remaining) {
        uint32_t ci = (uint32_t)(pos / cs);
        uint32_t off = (uint32_t)(pos % cs);
        uint32_t n = cs - off;
        if ((uint64_t)n > remaining)
            n = (uint32_t)remaining;
        if (dcache_load_sparse(ino, ci, off, p, n, cs) != 0)
            return -1;
        pos += n;
        p += n;
        remaining -= n;
    }
    dcache_note_size(ino, offset + len);
    return 0;
}

static int dcache_fill(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       const uint8_t *chunk, uint32_t chunk_size)
{
    if (efs_writer_state_owned(e->writer))
        return EFS_ERR_BUSY;
    efs_writer_state_free(e->writer);
    e->writer = NULL;
    int was_dirty = e->dirty && e->data;
    /* Pool buffers are always EFS_CHUNK_SIZE-capacity; only allocate when
     * the slot has none. (The old realloc path also mixed pool/non-pool
     * buffers, which the pool's len-keyed free can't distinguish.) */
    if (!e->data) {
        e->data = efs_buf_alloc(chunk_size);
        if (!e->data)
            return EFS_ERR_NOMEM;
    }
    e->len = chunk_size;
    memcpy(e->data, chunk, chunk_size);
    e->ino = ino;
    e->ci = ci;
    dcache_set_dirty(e, dcache_slot(ino, ci));
    e->have_base = 1;
    e->nrange = 0;
    e->object_gen = 0;
    e->object_delta_off = 0;
    e->object_delta_len = 0;
    e->object_delta_base_n = 0;
    e->object_delta_base_seq = 0;
    /* A whole-chunk image (or a recreated slot) supersedes every snapshot
     * taken so far, including a dropped incarnation's in-flight PUTs:
     * their late installs would overwrite it (nrange==0 → no overlay). */
    ++e->mutation;
    e->recovery_cycles = 0;
    e->pending_seq = 0;
    e->pending_sent = 0;
    efs_wb_cycle_reset(&e->recovery);
    e->img_seq = dcache_seq_now();
    dcache_pin_add(e);
    if (!was_dirty)
        dcache_note_dirty_bytes((int64_t)chunk_size);
    return 0;
}

/* Take ownership of a pool-allocated assemble buffer. Caller must not free
 * `chunk` after success. */
static int dcache_take(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       uint8_t *chunk, uint32_t chunk_size)
{
    if (efs_writer_state_owned(e->writer))
        return EFS_ERR_BUSY;
    efs_writer_state_free(e->writer);
    e->writer = NULL;
    int was_dirty = e->dirty && e->data;
    if (e->data)
        DTRACE(e, "take-REPLACE was_dirty=%d", was_dirty);
    if (e->data && e->data != chunk)
        efs_buf_free(e->data, e->len);
    e->data = chunk;
    e->len = chunk_size;
    e->ino = ino;
    e->ci = ci;
    dcache_set_dirty(e, dcache_slot(ino, ci));
    e->have_base = 1;
    e->nrange = 0;
    e->object_gen = 0;
    e->object_delta_off = 0;
    e->object_delta_len = 0;
    e->object_delta_base_n = 0;
    e->object_delta_base_seq = 0;
    ++e->mutation;
    e->recovery_cycles = 0;
    e->pending_seq = 0;
    e->pending_sent = 0;
    efs_wb_cycle_reset(&e->recovery);
    e->img_seq = dcache_seq_now(); /* see dcache_fill */
    dcache_pin_add(e);
    if (!was_dirty)
        dcache_note_dirty_bytes((int64_t)chunk_size);
    return 0;
}

/* Count a dirty slot that is not in the chunk table yet. Called with no
 * dcache mutex held. idx then dcache is the order the flush path uses. */
static void dcache_account_extra(efs_ino_t ino, uint32_t ci)
{
    int in_table;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    struct dcache_ent *e;

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    in_table = (efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);

    pthread_mutex_lock(mu);
    e = dcache_find(s, ino, ci);
    if (!e || e->present_extra || !e->dirty || in_table) {
        pthread_mutex_unlock(mu);
        return;
    }
    e->present_extra = 1;
    pthread_mutex_unlock(mu);
    efs_export_present_add(&g_client.export, ino, 1, 1);
}

/* The chunk is about to be inserted in the table. Drop the extra count
 * so set_chunk's +1 does not double it. */
void efs_dcache_yield_extra(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    int had = 0;
    struct dcache_ent *e;

    pthread_mutex_lock(mu);
    e = dcache_find(s, ino, ci);
    if (e && e->present_extra) {
        e->present_extra = 0;
        had = 1;
    }
    pthread_mutex_unlock(mu);
    if (had)
        efs_export_present_add(&g_client.export, ino, 1, -1);
}

/* 0 = cached, 1 = should not happen (chain grows), <0 = error. */
static int dcache_store(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                        uint32_t chunk_size)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    /* _meta: a body-less node of THIS chunk (its PUT landed, the body
     * went to the rdcache or was dropped) is refilled, never shadowed
     * by a second node. dcache_find skips body-less nodes, so the store
     * chained a new one and dcache_find_meta (put-record,
     * note_committed, putid) kept hitting the stale first node: the
     * live node never learned its object, the REPORT took its identity
     * from the staging table, and the next flush folded a full image
     * from a stale delta list (peer_shared_pwrite EIO, Oct 2 2026). */
    struct dcache_ent *e = dcache_find_meta(s, ino, ci);
    if (e) {
        int rc = dcache_fill(e, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        if (rc == 0)
            dcache_account_extra(ino, ci);
        return rc;
    }
    struct dcache_ent *head = &g_dcache.e[s];
    /* A body-less entry still names a published chunk (full overwrite
     * dropped the 128 KiB). Do not reuse it for a different chunk. */
    if (!head->ino && !head->dirty && !head->data) {
        int rc = dcache_fill(head, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        if (rc == 0)
            dcache_account_extra(ino, ci);
        return rc;
    }
    int reused = 0;
    struct dcache_ent *n = dcache_chain_reuse(s);
    if (n)
        reused = 1;
    else
        n = efs_buf_metadata_alloc(sizeof(*n));
    if (!n) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_NOMEM;
    }
    int rc = dcache_fill(n, ino, ci, chunk, chunk_size);
    if (rc != 0) {
        if (!reused)
            efs_buf_metadata_free(n, sizeof(*n));
        pthread_mutex_unlock(mu);
        return rc;
    }
    if (!reused) {
        n->next = head->next;
        head->next = n;
    }
    pthread_mutex_unlock(mu);
    dcache_account_extra(ino, ci);
    return 0;
}

/* Like dcache_store, but `chunk` is stolen on success (not copied). */
static void dcache_apply_init(struct dcache_ent *e,
                              const struct dcache_init *in)
{
    if (!in)
        return;
    if (in->full) {
        e->have_base = 1;
        e->nrange = 0;
        e->base_gen = EFS_CHUNK_BASE_UNCOND;
        return;
    }
    e->have_base = in->have_base;
    if (e->base_gen != EFS_CHUNK_BASE_UNCOND)
        e->base_gen = in->have_base ? in->base_gen : 0;
    e->nrange = 0;
    dcache_add_range(e, in->off, in->len);
}

static int dcache_store_owned_locked(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                              uint32_t chunk_size,
                              const struct dcache_init *in)
{
    if (!chunk || !chunk_size)
        return EFS_ERR_INVAL;
    uint32_t s = dcache_slot(ino, ci);
    struct dcache_ent *e = dcache_find_meta(s, ino, ci); /* see dcache_store */
    if (e) {
        int rc = dcache_take(e, ino, ci, chunk, chunk_size);
        if (rc == 0)
            dcache_apply_init(e, in);
        return rc;
    }
    struct dcache_ent *head = &g_dcache.e[s];
    if (!head->ino && !head->dirty && !head->data) {
        int rc = dcache_take(head, ino, ci, chunk, chunk_size);
        if (rc == 0)
            dcache_apply_init(head, in);
        return rc;
    }
    int reused = 0;
    struct dcache_ent *n = dcache_chain_reuse(s);
    if (n)
        reused = 1;
    else
        n = efs_buf_metadata_alloc(sizeof(*n));
    if (!n) {
        return EFS_ERR_NOMEM;
    }
    int rc = dcache_take(n, ino, ci, chunk, chunk_size);
    if (rc != 0) {
        if (!reused)
            efs_buf_metadata_free(n, sizeof(*n));
        return rc;
    }
    dcache_apply_init(n, in);
    if (!reused) {
        n->next = head->next;
        head->next = n;
    }
    return 0;
}

/* Install a freshly loaded+patched chunk, or fold just [off,len) into a
 * chunk another writer dirtied while we were fetching. 0 = new slot
 * (chunk stolen), 1 = folded (chunk freed, existing ranges kept),
 * <0 = error (caller frees chunk). */
static int dcache_merge_owned(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len,
                              uint8_t *chunk, uint32_t cs,
                              const struct dcache_init *in)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && dcache_keep_on_drop(e) && e->len >= cs && off + len <= e->len) {
        if (!e->dirty) {
            dcache_set_dirty(e, s);
            dcache_pin_add(e);
            dcache_note_dirty_bytes((int64_t)e->len);
        }
        memcpy(e->data + off, src, len);
        dcache_add_range(e, off, len);
        if (in && in->full) {
            /* Full bytes and ownership become visible in the same lock hold.
             * Older in-flight images cannot replace this accepted overwrite. */
            dcache_apply_init(e, in);
            e->img_seq = dcache_seq_now();
        }
        DTRACE(e, "merge-fold off=%u len=%u", off, len);
        pthread_mutex_unlock(mu);
        efs_buf_free(chunk, cs);
        efs_rdcache_invalidate(ino, ci);
        return 1;
    }
    /* Keep lookup and installation in one critical section. A second
     * writer may otherwise create ownership between the two lock holds. */
    int rc = dcache_store_owned_locked(ino, ci, chunk, cs, in);
    pthread_mutex_unlock(mu);
    if (rc != 0)
        return rc;
    dcache_account_extra(ino, ci);
    efs_rdcache_invalidate(ino, ci);
    return 0;
}

/* have_base=0 flush must GET a published mapping even when slot base_gen
 * is still 0 (O_APPEND rebuilds a sparse slot after adopt drops the
 * cached chunk). First write of a new chunk has no mapping — stay zeros
 * (W1 leftover GET). Full-chunk overwrite is UNCOND and never merges. */
/* Install the image this flush/replay just PUT (`img` = base + our ranges
 * as of the snapshot) as the slot's have_base image, dcache_mu held. If
 * writes landed since the snapshot (e->dirty), their bytes live only in
 * e->data and their ranges are already in e->roff; lay every current range
 * over img so the result is committed-base + ALL our bytes. Storing img
 * whole here zeroed those late bytes under their own ranges — the next
 * flush then published the zeros (concurrent_appends: 190/200 lines, 20
 * NULs, identical on a peer). Keeping e->data untouched instead left a
 * sparse zeros+ranges image flagged have_base=1. */
static void dcache_install_image(struct dcache_ent *e, uint8_t *img,
                                 uint32_t len, uint64_t seq)
{
    if (!e || !e->data || e->len < len)
        return;
    if (seq && seq <= e->img_seq) {
        /* An older snapshot's PUT finished after a newer one's install
         * (see img_seq). e->data already holds the newer image. */
        DTRACE(e, "install-SKIP-old len=%u seq=%llu img_seq=%llu", len,
               (unsigned long long)seq, (unsigned long long)e->img_seq);
        return;
    }
    DTRACE(e, "install len=%u seq=%llu", len, (unsigned long long)seq);
    if (seq)
        e->img_seq = seq;
    if (e->dirty) {
        for (uint8_t i = 0; i < e->nrange; i++) {
            uint32_t a = e->roff[i], n = e->rlen[i];

            if (a >= len)
                continue;
            if (a + n > len)
                n = len - a;
            memcpy(img + a, e->data + a, n);
        }
    }
    memcpy(e->data, img, len);
    e->have_base = 1;
}

/* D23 (decided Oct 2 2026): a clean body whose publish has COMMITTED is
 * not kept in the dcache — clean bodies have no budget and no evictor
 * there, so a long append/partial-write stream made RSS track the bytes
 * written. The committed image goes to the rdcache (budgeted, LRU)
 * instead; the slot keeps its body-less node, which names the object
 * for the next snapshot. A later partial write finds the image through
 * dcache_load_and_patch's rdcache probe, else one GET (the table names
 * the committed gen, so the fetch and the CAS base are right).
 *
 * Commit time, not PUT-landed time: between PUT and REPORT the body and
 * e->roff are the only record of which bytes are THIS client's, and a
 * STALE on the REPORT (a peer's fold landed in between) is replayed by
 * re-merging exactly those ranges over the committed base. Handing the
 * body over at PUT time left nothing to replay (peer_shared_pwrite ci=1,
 * Oct 2 2026 12:57Z: 30 identical stale-class rounds, no snap-replay).
 * A span-only row has table gen 0 and the rdcache cannot key it
 * (rdcache_map_gen == 0 → refused); a span commit does not move the
 * base (gen 0 here), so span chunks keep their body as before.
 *
 * Returns 1 when the caller must hand `*img` (now detached from the
 * slot) to the rdcache AFTER releasing the slot mutex:
 * efs_rdcache_put_owned takes g_client.idx_mu for the map generation,
 * and efs_client_set_chunk takes a slot mutex under that lock through
 * efs_dcache_yield_extra — putting under the slot lock deadlocked the
 * Oct 2 2026 06:08Z D24 wedge gate (7 FUSE requests waiting, every
 * thread in __lll_lock_wait). Caller holds the slot mutex. */
static int dcache_body_to_rdcache(struct dcache_ent *e, uint8_t **img,
                                  uint32_t *len)
{
    *img = NULL;
    *len = 0;
    if (!e || e->dirty || !e->data || !e->have_base ||
        e->base_gen == EFS_CHUNK_BASE_UNCOND)
        return 0;
    *img = e->data;
    *len = e->len;
    e->data = NULL;
    e->len = 0;
    e->nrange = 0;
    DTRACE(e, "to-rdcache len=%u", *len);
    return 1;
}

/* The unlocked half of dcache_body_to_rdcache: the rdcache takes `img`
 * or the caller frees it (a refused put — table gen 0, or a pending
 * way — leaves a body-less node; the next write of the chunk does one
 * GET). */
static void dcache_img_to_rdcache(efs_ino_t ino, uint32_t ci, uint8_t *img,
                                  uint32_t len)
{
    if (img && !efs_rdcache_put_owned(ino, ci, img, len))
        efs_buf_free(img, len);
}

/* Every partial image is rebuilt under fresh lane read authority. Object
 * generation alone cannot validate bytes: a fence can mask the same object
 * without changing its identity. Failures must preserve the local snapshot. */
static int dcache_fetch_merge_base(efs_ino_t ino, uint32_t ci, uint8_t *base,
                                    uint32_t len, struct efs_chunk_entry *obs,
                                    int *have_obs)
{
    uint8_t absent = 0;
    int rc = efs_client_pull_chunks_range_absent(ino, ci, ci + 1, &absent);
    if (rc != EFS_OK)
        return rc;
    if (absent & 1) {
        /* An absent server row can leave our own unreported PUT in the
         * staging table. It is not a committed merge base. */
        memset(base, 0, len);
        memset(obs, 0, sizeof(*obs));
        *have_obs = 0;
        return EFS_OK;
    }
    return efs_client_fetch_published_chunk_obs(ino, ci, base, len, obs,
                                                have_obs);
}

/* One disjoint owned range can append a span. Any overlap with a committed
 * span needs a full CAS fold of the freshly fetched image: trimming a covered
 * prefix drops application overwrites in that prefix. */
static void span_of(efs_ino_t ino, uint32_t ci, uint64_t bg, uint8_t nrange,
                    const uint32_t *roff, const uint32_t *rlen, uint32_t len,
                    uint32_t *off, uint32_t *sp)
{
    struct efs_chunk_entry ce;
    *off = 0;
    *sp = 0;
    if (bg == EFS_CHUNK_BASE_UNCOND || nrange != 1 || !roff || !rlen ||
        rlen[0] == 0 || (uint64_t)roff[0] + rlen[0] > len || rlen[0] >= len)
        return;
    memset(&ce, 0, sizeof(ce));
    if (export_chunk_copy(ino, ci, &ce) == 0) {
        struct efs_chunk_rec mine = {0};
        uint64_t mine_gen = 0;
        if (ce.ndelta >= EFS_CHUNK_DELTA_MAX)
            return;
        /* Our unreported local span is not committed coverage. The next
         * snapshot must still include every application byte it owns. */
        if (putid_fill(ino, ci, &mine, NULL) && mine.delta_len)
            mine_gen = mine.chunk_generation;
        for (uint32_t i = 0; i < ce.ndelta; ++i) {
            const struct efs_chunk_delta *d = &ce.deltas[i];
            if (mine_gen && d->generation == mine_gen)
                continue;
            if (d->len && d->off < roff[0] + rlen[0] &&
                roff[0] < d->off + d->len)
                return;
        }
    }
    *off = roff[0];
    *sp = rlen[0];
}

/* W3: steal one dirty (ino,ci) so the PUT can run on the write pipeline
 * (hash+EC+PUT of chunk N+1 while N is in flight). Same dirty-clear and
 * unpublished-base merge as dcache_flush_slot_inner. Returns 1 if *copy_out
 * is set, 0 if that ci is clean, <0 on error. */
static int dcache_steal_dirty_budgeted(efs_ino_t ino, uint32_t ci, uint8_t **copy_out,
                              uint32_t *len_out, uint64_t *bg_out,
                              uint64_t *seq_out, uint32_t *doff,
                              uint32_t *dlen, int *drop_body,
                              struct efs_chunk_entry *obs_out,
                              int *have_obs_out)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *io = dcache_io_mu(s);
    pthread_mutex_t *mu = dcache_mu(s);
    struct dcache_ent *e;
    uint8_t *copy;
    uint32_t len, roff[DCACHE_NR], rlen[DCACHE_NR];
    uint8_t nrange = 0;
    int have_base;
    uint64_t bg;

    *copy_out = NULL;
    if (doff)
        *doff = 0;
    if (dlen)
        *dlen = 0;
    if (drop_body)
        *drop_body = 0;
    if (have_obs_out)
        *have_obs_out = 0;
    pthread_mutex_lock(io);
    pthread_mutex_lock(mu);
    for (e = &g_dcache.e[s]; e; e = e->next) {
        if (e->dirty && e->data && e->ino == ino && e->ci == ci)
            break;
    }
    if (!e) {
        pthread_mutex_unlock(mu);
        pthread_mutex_unlock(io);
        return 0;
    }
    if (efs_writer_state_owned(e->writer)) {
        pthread_mutex_unlock(mu);
        pthread_mutex_unlock(io);
        return EFS_ERR_BUSY;
    }
    len = e->len;
    have_base = e->have_base;
    nrange = e->nrange;
    bg = e->base_gen;
    if (nrange) {
        memcpy(roff, e->roff, (size_t)nrange * sizeof(uint32_t));
        memcpy(rlen, e->rlen, (size_t)nrange * sizeof(uint32_t));
    }
    *seq_out = dcache_seq_next(e);
    /* Keep the authoritative local bytes until REPORT, including full
     * overwrites. The allocator hard bound covers these retained bodies. */
    copy = efs_buf_alloc(len);
    if (!copy) {
        pthread_mutex_unlock(mu);
        pthread_mutex_unlock(io);
        return EFS_ERR_NOMEM;
    }
    memcpy(copy, e->data, len);
    DTRACE(e, "snap-copy");
    e->dirty = 0;
    dcache_dirty_unlink(e, s);
    dcache_note_dirty_bytes(-(int64_t)len);
    /* Window open: the entry is clean, the chunk not yet in the dirty
     * set. Closed by flush_pipe_drain / flush_pipe_add after the PUT. */
    put_win_open(ino);
    pthread_mutex_unlock(mu);
    pthread_mutex_unlock(io);
    int merged = 0;
    if (bg != EFS_CHUNK_BASE_UNCOND) {
        merged = 1;
        uint8_t *base = efs_buf_alloc(len);
        uint8_t i;

        if (!base) {
            efs_buf_free(copy, len);
            pthread_mutex_lock(mu);
            e = dcache_find(s, ino, ci);
            if (e && e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                dcache_set_dirty(e, s);
                dcache_note_dirty_bytes((int64_t)len);
            }
            pthread_mutex_unlock(mu);
            put_win_close(ino);
            return EFS_ERR_NOMEM;
        }
        struct efs_chunk_entry obs;
        int have_obs = 0;

        int fetch_rc = dcache_fetch_merge_base(ino, ci, base, len, &obs,
                                                &have_obs);
        if (fetch_rc != EFS_OK) {
            /* Do not PUT a zero base over a chunk we failed to read. */
            efs_buf_free(base, len);
            efs_buf_free(copy, len);
            pthread_mutex_lock(mu);
            e = dcache_find(s, ino, ci);
            if (e && e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                dcache_set_dirty(e, s);
                dcache_note_dirty_bytes((int64_t)len);
            }
            pthread_mutex_unlock(mu);
            put_win_close(ino);
            return fetch_rc;
        }
        for (i = 0; i < nrange; i++) {
            if (roff[i] + rlen[i] <= len)
                memcpy(base + roff[i], copy + roff[i], rlen[i]);
        }
        memcpy(copy, base, len);
        efs_buf_free(base, len);
        /* The base this image extends is the row the fetch decoded. */
        bg = have_obs ? obs.generation : 0;
        /* No local row = the image was built on zeros. Publish that as
         * an EMPTY observation (n=0, seq=0), never as "no observation":
         * with NULL the PUT read the table again, a window pulled in
         * between named the peer's spans, FOLD_LIST passed and the fold
         * tombstoned a span whose bytes are not in this image (IO-500
         * hard-read, Sep 30: the client-boundary piece of a chunk read
         * as zeros, the rest intact). The server STALEs an empty
         * observation against a row with spans and the replay refetches
         * with the row. */
        if (obs_out && have_obs_out) {
            *obs_out = obs;
            *have_obs_out = 1;
        }
    }
    if (merged) {
        pthread_mutex_lock(mu);
        e = dcache_find(s, ino, ci);
        if (e && e->ino == ino && e->ci == ci &&
            e->base_gen != EFS_CHUNK_BASE_UNCOND)
            e->base_gen = bg;
        pthread_mutex_unlock(mu);
    }
    *copy_out = copy;
    *len_out = len;
    *bg_out = bg;
    /* D1: a sub-range is a span even with have_base. A full overwrite
     * (span_of returns 0, or dcache_full_overwrite) replaces the chain. */
    if (!dcache_full_overwrite(have_base, bg, nrange) && doff && dlen)
        span_of(ino, ci, bg, nrange, roff, rlen, len, doff, dlen);
    return 1;
}

static int dcache_steal_dirty(efs_ino_t ino, uint32_t ci, uint8_t **copy_out,
                              uint32_t *len_out, uint64_t *bg_out,
                              uint64_t *seq_out, uint32_t *doff,
                              uint32_t *dlen, int *drop_body,
                              struct efs_chunk_entry *obs_out,
                              int *have_obs_out)
{
    int rc;
    efs_buf_drain_enter();
    rc = dcache_steal_dirty_budgeted(ino, ci, copy_out, len_out, bg_out,
                                      seq_out, doff, dlen, drop_body,
                                      obs_out, have_obs_out);
    efs_buf_drain_leave();
    return rc;
}

/* After a pipelined dcache_put_now: keep published bytes as have_base=1
 * (same as flush_slot_inner — dropping the slot made concurrent_appends
 * read zeros). Re-dirty on PUT failure. */
static int dcache_flush_keep(efs_ino_t ino, uint32_t ci, uint8_t *copy,
                             uint32_t len, int put_ok, uint64_t seq,
                             int drop_body)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    struct dcache_ent *e;

    pthread_mutex_lock(mu);
    e = drop_body ? dcache_find_meta(s, ino, ci) : dcache_find(s, ino, ci);
    if (!put_ok) {
        if (drop_body && e && !e->data) {
            e->data = copy;
            e->len = len;
            dcache_set_dirty(e, s);
            e->have_base = 1;
            e->nrange = 0;
            e->base_gen = EFS_CHUNK_BASE_UNCOND;
            dcache_pin_add(e);
            dcache_note_dirty_bytes((int64_t)len);
            DTRACE(e, "keep-putfail-restore");
            pthread_mutex_unlock(mu);
            return 1;
        }
        if (e && e->ino == ino && e->ci == ci && e->data && !e->dirty) {
            dcache_set_dirty(e, s);
            dcache_note_dirty_bytes((int64_t)len);
            DTRACE(e, "keep-putfail");
        }
        pthread_mutex_unlock(mu);
        return 0;
    }
    /* Every current snapshot keeps its local body through REPORT. The
     * legacy drop_body path below still restores ownership on PUT failure. */
    if (!drop_body && e && e->ino == ino && e->ci == ci)
        dcache_install_image(e, copy, len, seq);
    /* The PUT landed and dcache_put_now put the chunk in the dirty set,
     * which keeps the row staged until the REPORT. A clean slot (body
     * kept as cache, or the body-less node that names the object) no
     * longer needs the row pin: with it, every file this client wrote
     * stayed unevictable and "has unpublished" for as long as the slot
     * lived (Oct 1 2026). A write that re-dirtied the entry mid-PUT
     * re-pinned it (dcache_set_dirty + dcache_pin_add); leave that. */
    if (e && e->ino == ino && e->ci == ci && !e->dirty)
        dcache_pin_release(e);
    /* Retained bytes remain charged to the hard allocator bound. A
     * matching REPORT acknowledgment releases full-overwrite bodies. */
    pthread_mutex_unlock(mu);
    return 0;
}

struct flush_pipe {
    struct chunk_put_job jobs[EFS_WRITE_PIPELINE];
    uint8_t *copies[EFS_WRITE_PIPELINE];
    uint32_t lens[EFS_WRITE_PIPELINE];
    uint8_t drop[EFS_WRITE_PIPELINE];
    uint32_t n;
    int rc;
};

static void flush_pipe_init(struct flush_pipe *p)
{
    memset(p, 0, sizeof(*p));
}

static int flush_pipe_drain(struct flush_pipe *p)
{
    uint32_t i;

    if (p->n == 0)
        return p->rc;
    put_pool_run(p->jobs, p->n);
    for (i = 0; i < p->n; i++) {
        int pok = (p->jobs[i].rc == EFS_OK);

        if (!dcache_flush_keep(p->jobs[i].ino, p->jobs[i].ci, p->copies[i],
                               p->lens[i], pok, p->jobs[i].flush_seq,
                               p->drop[i]))
            efs_buf_free(p->copies[i], p->lens[i]);
        p->copies[i] = NULL;
        put_win_close(p->jobs[i].ino);
        if (!pok && p->rc == EFS_OK)
            p->rc = p->jobs[i].rc;
    }
    p->n = 0;
    return p->rc;
}

static int flush_pipe_add(struct flush_pipe *p, efs_ino_t ino, uint32_t ci,
                          uint8_t *copy, uint32_t len, uint64_t bg,
                          uint64_t seq, uint32_t doff, uint32_t dlen,
                          int drop_body, const struct efs_chunk_entry *obs)
{
    struct chunk_put_job *j;

    if (p->n == EFS_WRITE_PIPELINE) {
        int drc = flush_pipe_drain(p);

        if (drc != EFS_OK) {
            if (drop_body &&
                dcache_flush_keep(ino, ci, copy, len, 0, seq, 1))
                copy = NULL;
            efs_buf_free(copy, len);
            put_win_close(ino);
            p->rc = drc;
            return drc;
        }
    }
    j = &p->jobs[p->n];
    memset(j, 0, sizeof(*j));
    j->ino = ino;
    j->ci = ci;
    j->flush_put = 1;
    j->flush_buf = copy;
    j->flush_len = len;
    j->flush_base_gen = bg;
    j->flush_seq = seq;
    j->flush_delta_off = doff;
    j->flush_delta_len = dlen;
    j->flush_have_obs = obs != NULL;
    if (obs)
        j->flush_obs = *obs;
    j->rc = EFS_ERR_IO;
    p->copies[p->n] = copy;
    p->lens[p->n] = len;
    p->drop[p->n] = drop_body ? 1 : 0;
    p->n++;
    return EFS_OK;
}

/* Stop diagnostics: count every unacknowledged body/object, not only
 * entries whose dirty bit survived the PUT snapshot. No I/O under locks. */
uint64_t efs_dcache_pending_records(const char *discard_cause)
{
    uint64_t n = 0;
    dcache_ensure();
    for (uint32_t s = 0; s < DCACHE_SLOTS; ++s) {
        pthread_mutex_lock(dcache_mu(s));
        for (struct dcache_ent *e = &g_dcache.e[s]; e; e = e->next) {
            if (!e->ino || !dcache_keep_on_drop(e))
                continue;
            ++n;
            if (!discard_cause)
                continue;
            uint32_t ranges = e->nrange ? e->nrange : 1;
            for (uint32_t k = 0; k < ranges; ++k) {
                uint32_t off = e->nrange ? e->roff[k] : e->object_delta_off;
                uint32_t len = e->nrange ? e->rlen[k] :
                    e->object_delta_len ? e->object_delta_len : data_chunk_size();
                fprintf(stderr, "force-discard ino=%llu ci=%u off=%u len=%u cause=%s\n",
                        (unsigned long long)e->ino, e->ci, off, len, discard_cause);
            }
        }
        pthread_mutex_unlock(dcache_mu(s));
    }
    return n;
}

/* 1 if e is still a node of slot s's chain. A drop under dcache_mu frees
 * chain nodes; a flush that released the lock for a GET or PUT must not
 * touch its saved pointer until this says so. The head is an array
 * element and is never freed. */
static int dcache_chain_has(uint32_t s, const struct dcache_ent *e)
{
    const struct dcache_ent *p;

    for (p = &g_dcache.e[s]; p; p = p->next)
        if (p == e)
            return 1;
    return 0;
}

static int dcache_flush_slot_inner_budgeted(uint32_t s, efs_ino_t only_ino, int have_only)
{
    pthread_mutex_t *io = dcache_io_mu(s);
    pthread_mutex_t *mu = dcache_mu(s);
    int rc = EFS_OK;
    /* shard_io -> dcache_mu. Both drop before a fragment GET or PUT:
     * fsync of another file on this shard must not sit out a 30 s
     * recv. The entry is clean (body stolen or dirty cleared) before
     * the drop, so a second flush does not read the same merge base. */
    pthread_mutex_lock(io);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = &g_dcache.e[s];
    while (e) {
        if (!e->dirty || !e->data || (have_only && e->ino != only_ino)) {
            e = e->next;
            continue;
        }
        /* Legacy flush cannot publish an epoch-owned snapshot. */
        if (efs_writer_state_owned(e->writer)) {
            rc = EFS_ERR_BUSY;
            break;
        }
        /* Reclaim (have_only=0) must not GET+PUT an unpublished sparse
         * append chunk. Two such flushes read the same zero base and the
         * last PUT drops the other's ranges (concurrent_appends NULs).
         * Close/fsync pass have_only=1. */
        if (!have_only && !e->have_base) {
            e = e->next;
            continue;
        }
        efs_ino_t ino = e->ino;
        uint32_t ci = e->ci;
        uint32_t len = e->len;
        int have_base = e->have_base;
        uint64_t slot_bg = e->base_gen;
        uint8_t nrange = e->nrange;
        uint32_t roff[DCACHE_NR], rlen[DCACHE_NR];
        uint32_t doff = 0, dlen = 0;
        if (nrange) {
            memcpy(roff, e->roff, (size_t)nrange * sizeof(uint32_t));
            memcpy(rlen, e->rlen, (size_t)nrange * sizeof(uint32_t));
        }
        uint64_t seq = dcache_seq_next(e);
        int full = dcache_full_overwrite(have_base, slot_bg, nrange);
        uint8_t *copy;
        copy = efs_buf_alloc(len);
        if (!copy) {
            pthread_mutex_unlock(mu);
            pthread_mutex_unlock(io);
            return EFS_ERR_NOMEM;
        }
        memcpy(copy, e->data, len);
        DTRACE(e, "snap-inner");
        e->dirty = 0;
        dcache_dirty_unlink(e, s);
        dcache_note_dirty_bytes(-(int64_t)len);
        put_win_open(ino);
        pthread_mutex_unlock(mu);
        pthread_mutex_unlock(io);
        struct efs_chunk_entry merge_obs;
        int merge_have_obs = 0;

        memset(&merge_obs, 0, sizeof(merge_obs));
        if (slot_bg != EFS_CHUNK_BASE_UNCOND) {
            uint8_t *base = efs_buf_alloc(len);
            if (!base) {
                efs_buf_free(copy, len);
                pthread_mutex_lock(io);
                pthread_mutex_lock(mu);
                e = dcache_find(s, ino, ci);
                if (e && !e->dirty) {
                    dcache_set_dirty(e, s);
                    dcache_note_dirty_bytes((int64_t)len);
                }
                pthread_mutex_unlock(mu);
                pthread_mutex_unlock(io);
                put_win_close(ino);
                return EFS_ERR_NOMEM;
            }
            int rrc = dcache_fetch_merge_base(ino, ci, base, len,
                                               &merge_obs, &merge_have_obs);
            if (rrc != EFS_OK) {
                efs_buf_free(base, len);
                efs_buf_free(copy, len);
                pthread_mutex_lock(io);
                pthread_mutex_lock(mu);
                e = dcache_find(s, ino, ci);
                if (e && !e->dirty) {
                    dcache_set_dirty(e, s);
                    dcache_note_dirty_bytes((int64_t)len);
                }
                pthread_mutex_unlock(mu);
                pthread_mutex_unlock(io);
                put_win_close(ino);
                return rrc;
            }
            for (uint8_t i = 0; i < nrange; i++) {
                if (roff[i] + rlen[i] <= len)
                    memcpy(base + roff[i], copy + roff[i], rlen[i]);
            }
            memcpy(copy, base, len);
            efs_buf_free(base, len);
            slot_bg = merge_have_obs ? merge_obs.generation : 0;
            /* Zero base (no local row) is an empty observation, not
             * none — see dcache_steal_dirty. */
            merge_have_obs = 1;
        }
        /* D1: a partial write is a span even when this client holds the
         * whole chunk. A full overwrite (UNCOND, no ranges) folds. */
        if (full)
            doff = dlen = 0;
        else
            span_of(ino, ci, slot_bg, nrange, roff, rlen, len, &doff, &dlen);
        int prc = dcache_put_now(ino, ci, copy, len, slot_bg, seq, doff,
                                 dlen, merge_have_obs ? &merge_obs : NULL);
        pthread_mutex_lock(io);
        pthread_mutex_lock(mu);
        /* The put-record (or the re-dirty below on failure) is what the
         * next snapshot reads; either way the window is closed once we
         * hold the locks again. A failed PUT re-dirties under these
         * locks before any snapshot can run. */
        put_win_close(ino);
        if (!dcache_chain_has(s, e)) {
            /* The node was dropped while the PUT ran. Whatever holds
             * (ino,ci) now, if anything, gets the result; the walk
             * restarts from the head, where every flushed entry is
             * already clean. */
            struct dcache_ent *cur = dcache_find_meta(s, ino, ci);

            if (prc != EFS_OK && rc == EFS_OK)
                rc = prc;
            if (prc != EFS_OK && cur && !cur->data) {
                cur->data = copy;
                cur->len = len;
                dcache_set_dirty(cur, s);
                cur->have_base = 1;
                cur->nrange = 0;
                cur->base_gen = EFS_CHUNK_BASE_UNCOND;
                dcache_pin_add(cur);
                dcache_note_dirty_bytes((int64_t)len);
                copy = NULL;
            }
            efs_buf_free(copy, len);
            if (prc != EFS_OK)
                break; /* re-dirtied; a later flush retries, not this walk */
            e = &g_dcache.e[s];
            continue;
        }
        if (prc != EFS_OK) {
            if (rc == EFS_OK)
                rc = prc;
            if (full) {
                struct dcache_ent *back = dcache_find_meta(s, ino, ci);
                if (back && !back->data) {
                    back->data = copy;
                    back->len = len;
                    dcache_set_dirty(back, s);
                    back->have_base = 1;
                    back->nrange = 0;
                    back->base_gen = EFS_CHUNK_BASE_UNCOND;
                    dcache_pin_add(back);
                    dcache_note_dirty_bytes((int64_t)len);
                    copy = NULL;
                }
            } else if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                /* PUT failed: this slot is the only copy. Keep it dirty. */
                dcache_set_dirty(e, s);
                dcache_note_dirty_bytes((int64_t)len);
            }
            efs_buf_free(copy, len);
            e = e->next;
            continue;
        }
        if (e->ino == ino && e->ci == ci && e->data && e->len >= len) {
            /* Keep every snapshot for recovery until matching REPORT.
             * Concurrent dirty ranges remain overlaid on the image. */
            if (e->base_gen != EFS_CHUNK_BASE_UNCOND)
                e->base_gen = slot_bg;
            dcache_install_image(e, copy, len, seq);
        } else if (!full && !e->dirty && e->ino == ino && e->ci == ci) {
            dcache_pin_release(e);
            efs_buf_free(e->data, e->len);
            e->data = NULL;
            e->len = 0;
            e->ino = 0;
            e->ci = 0;
        }
        /* Clean after a landed PUT: the dirty set holds the row until
         * the REPORT; the slot's pin is not needed (see dcache_flush_keep).
         * Unreported full bodies remain allocator-charged until ACK. */
        if (e->ino == ino && e->ci == ci && !e->dirty)
            dcache_pin_release(e);
        efs_buf_free(copy, len);
        e = e->next;
    }
    pthread_mutex_unlock(mu);
    pthread_mutex_unlock(io);
    return rc;
}

static int dcache_flush_slot_inner(uint32_t s, efs_ino_t only_ino, int have_only)
{
    int rc;
    efs_buf_drain_enter();
    rc = dcache_flush_slot_inner_budgeted(s, only_ino, have_only);
    efs_buf_drain_leave();
    return rc;
}

/* The shard lock is taken inside the flush and dropped before the
 * fragment GET and the PUT. Holding it across that GET wedged ll_fsync
 * of every other file on the shard for the recv timeout (12:36Z). */
static int dcache_flush_slot(uint32_t s, efs_ino_t only_ino, int have_only)
{
    return dcache_flush_slot_inner(s, only_ino, have_only);
}

/* Reclaim-all / shutdown: every slot, every inode. The per-inode flush
 * (close, fsync, truncate) is dcache_flush_ino_pass below. */
static int dcache_flush_all_slots(void)
{
    int rc = EFS_OK;
    uint32_t i;

    dcache_ensure();
    /* Reclaim (have_only=0) skips unpublished have_base=0 slots. It
     * takes shard_io only to snapshot a dirty entry, then drops it
     * before the fragment GET. */
    for (i = 0; i < DCACHE_SLOTS; i++) {
        int prc = dcache_flush_slot(i, 0, 0);

        if (prc != EFS_OK && rc == EFS_OK)
            rc = prc;
    }
    return rc;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return x < y ? -1 : x > y;
}

/* The dirty chunk indexes of one inode, read from the per-shard dirty
 * lists (W18's index). Every dirty entry is on its list (the reclaim pop
 * claims instead of unlinking), so this is exact at collection time; a
 * write landing after it is the caller's race, as before. Sorted so the
 * PUTs go out in chunk order. Returns the count, or <0 on NOMEM. */
static int dcache_dirty_cis_of(efs_ino_t ino, uint32_t **out)
{
    uint32_t *cis = NULL, n = 0, cap = 0;
    int sh;

    for (sh = 0; sh < DCACHE_SHARDS; sh++) {
        struct dcache_ent *e;

        pthread_mutex_lock(&g_dcache.shard[sh]);
        for (e = g_dcache.dirty_head[sh]; e; e = e->dirty_next) {
            if (e->ino != ino || !e->dirty || !e->data)
                continue;
            if (n == cap) {
                uint32_t ncap = cap ? cap * 2 : 64;
                uint32_t *grown = realloc(cis, (size_t)ncap * sizeof(*cis));

                if (!grown) {
                    pthread_mutex_unlock(&g_dcache.shard[sh]);
                    free(cis);
                    return EFS_ERR_NOMEM;
                }
                cis = grown;
                cap = ncap;
            }
            cis[n++] = e->ci;
        }
        pthread_mutex_unlock(&g_dcache.shard[sh]);
    }
    if (n > 1)
        qsort(cis, n, sizeof(*cis), cmp_u32);
    *out = cis;
    return (int)n;
}

/* Until Oct 1 2026 this walked ci = 0 .. size/128 KiB from the staged
 * row (one chain walk under two mutexes per chunk, dirty or not) and
 * every slot once the file was 8 GiB; it was 36.9 % of the client's
 * cycles during an ecopy --verify. Now it costs the inode's dirty
 * entries, which at close are the few the writeback did not take. */
static int dcache_flush_ino_pass(efs_ino_t ino)
{
    struct flush_pipe pipe;
    uint32_t *cis = NULL;
    int ncis, i;

    dcache_ensure();
    ncis = dcache_dirty_cis_of(ino, &cis);
    if (ncis < 0)
        return ncis;
    if (ncis == 0)
        return EFS_OK;
    flush_pipe_init(&pipe);
    for (i = 0; i < ncis && pipe.rc == EFS_OK; i++) {
        uint32_t ci = cis[i];

        if (efs_client_rpc_deadline_ms() &&
            efs_client_rpc_past_deadline()) {
            pipe.rc = EFS_ERR_BUSY;
            break;
        }
        uint8_t *copy = NULL;
        uint32_t len = 0;
        uint64_t bg = 0, seq = 0;
        uint32_t doff = 0, dlen = 0;
        int drop_body = 0;
        struct efs_chunk_entry obs;
        int have_obs = 0;
        int st = dcache_steal_dirty(ino, ci, &copy, &len, &bg, &seq, &doff,
                                    &dlen, &drop_body, &obs, &have_obs);

        if (st < 0)
            pipe.rc = st;
        else if (st > 0 &&
                 flush_pipe_add(&pipe, ino, ci, copy, len, bg, seq, doff,
                                dlen, drop_body,
                                have_obs ? &obs : NULL) != EFS_OK)
            break;
    }
    (void)flush_pipe_drain(&pipe);
    free(cis);
    return pipe.rc;
}

int efs_dcache_flush_ino(efs_ino_t ino)
{
    int rc = dcache_flush_ino_pass(ino);
    int w;

    if (rc != EFS_OK)
        return rc;
    /* Another flusher (reclaim, a second close) may hold this inode's
     * chunks in a PUT window: entry clean, chunk not yet in the dirty
     * set. Wait for those windows; a failed PUT re-dirties the entry, so
     * one more pass picks it up. Then the caller's REPORT snapshot sees
     * every chunk this inode has written. */
    w = put_win_wait(ino, 8000u);
    if (w == EFS_ERR_BUSY)
        return EFS_ERR_BUSY;
    if (w == 1)
        rc = dcache_flush_ino_pass(ino);
    return rc;
}

int efs_dcache_flush_all(void)
{
    return dcache_flush_all_slots();
}

static uint64_t dcache_base_gen_of(efs_ino_t ino, uint32_t ci, uint64_t fallback)
{
    uint32_t s;
    uint64_t g;
    struct dcache_ent *e;

    if (!ino)
        return fallback;
    dcache_ensure();
    s = dcache_slot(ino, ci);
    pthread_mutex_lock(dcache_mu(s));
    e = dcache_find_meta(s, ino, ci);
    g = e ? e->base_gen : fallback;
    pthread_mutex_unlock(dcache_mu(s));
    return g;
}

/* 1 = slot has a PUT object; fills rec nodes/checksums/chunk_generation. */
static int dcache_object_of(efs_ino_t ino, uint32_t ci, struct efs_chunk_rec *rec,
                             uint64_t *seq)
{
    uint32_t s;
    struct dcache_ent *e;
    int hit = 0;

    if (!ino || !rec)
        return 0;
    dcache_ensure();
    s = dcache_slot(ino, ci);
    pthread_mutex_lock(dcache_mu(s));
    e = dcache_find_meta(s, ino, ci);
    if (e && e->object_gen && dcache_unpublished(e)) {
        memcpy(rec->nodes, e->object_nodes, sizeof(rec->nodes));
        memcpy(rec->checksums, e->object_cks, sizeof(rec->checksums));
        rec->chunk_generation = e->object_gen;
        if (seq) *seq = e->object_seq;
        rec->delta_off = e->object_delta_off;
        rec->delta_len = e->object_delta_len;
        rec->delta_base_n = e->object_delta_base_n;
        rec->delta_base_seq = e->object_delta_base_seq;
        rec->file_generation = e->object_file_generation;
        rec->publish_epoch = e->object_publish_epoch;
        rec->publish_flags = e->object_publish_flags;
        hit = 1;
    }
    pthread_mutex_unlock(dcache_mu(s));
    return hit;
}

/* The host committed OUR object `gen` for (ino,ci): the slot's CAS base is
 * now that gen. Without this the next publish of the chunk (a later write,
 * or the same rec re-sent after a partial-commit STALE) CASes on the
 * pre-publish base and loses to itself. UNCOND slots keep UNCOND (full
 * overwrite semantics are independent of the committed gen). Returns 1
 * when the slot still holds that object.
 *
 * The base advances even when the slot's object is already a NEWER one of
 * ours (two closes of one file overlapped: A's PUT was reported and
 * committed while B's PUT — a superset image, later snap_seq — was still
 * in flight). B's publish must CAS on the committed gen A; leaving the
 * base at the pre-A gen made B STALE by construction, and a close-kicked
 * report gives up after two STALE rounds, so B's bytes stayed unpublished
 * until some later close of the SAME file — which never came for the last
 * appends of concurrent_appends (cold read: tail NUL, warm read: fine). */
static int dcache_note_committed(efs_ino_t ino, uint32_t ci, uint64_t gen,
                                  uint64_t object_gen, uint64_t object_seq)
{
    uint32_t s;
    struct dcache_ent *e;
    int hit = 0;
    uint8_t *rd_img = NULL;
    uint32_t rd_len = 0;

    if (!ino)
        return 0;
    dcache_ensure();
    s = dcache_slot(ino, ci);
    pthread_mutex_lock(dcache_mu(s));
    e = dcache_find_meta(s, ino, ci);
    if (e) {
        /* Typed ownership is acknowledged by its immutable publication,
         * including an older successful REPORT racing a newer cache PUT. */
        if (e->writer && e->writer->has_publication)
            (void)efs_writer_state_report(e->writer, object_gen, object_seq, EFS_OK);
        /* A span publish does not move the base generation (pass 0).
         * A full image does: later CAS expects that object. */
        if (gen && e->base_gen != EFS_CHUNK_BASE_UNCOND)
            e->base_gen = gen;
        hit = object_gen && e->object_gen == object_gen &&
              e->object_seq == object_seq;
        if (hit) {
            if (e->stalled) {
                e->stalled = 0;
                wb_error_record_resolve(ino);
            }
            e->pending_seq = 0;
            e->pending_sent = 0;
            e->recovery_cycles = 0;
            efs_wb_cycle_reset(&e->recovery);
            e->committed_object = object_gen;
            e->committed_seq = object_seq;
        }
        /* This object's ranges are committed. A later pwrite must record
         * only its own bytes: keeping the previous span made a STALE
         * replay paint the old image over a peer's exclusive range
         * (peer_overlap_pwrite_partial / chunk_straddle). A write that
         * re-dirtied the slot during the report still needs its ranges. */
        if (hit && !e->dirty && !efs_writer_state_owned(e->writer)) {
            e->nrange = 0;
            dcache_pin_release(e);
        }
        /* D23: the committed full image is the slot's whole content now
         * (base = this object, no ranges of ours outstanding); it moves
         * to the budgeted rdcache and the body is dropped. */
        if (hit && !e->dirty && !efs_writer_state_owned(e->writer)) {
            if (e->object_delta_len || e->base_gen == EFS_CHUNK_BASE_UNCOND) {
                /* A committed span or full overwrite's local image is not a read-cache base.
                 * It is reconstructible from the published span chain;
                 * release only after THIS object's REPORT verdict. */
                efs_buf_free(e->data, e->len);
                e->data = NULL;
                e->len = 0;
                e->have_base = 0;
            } else {
                (void)dcache_body_to_rdcache(e, &rd_img, &rd_len);
            }
        }
    }
    pthread_mutex_unlock(dcache_mu(s));
    dcache_img_to_rdcache(ino, ci, rd_img, rd_len);
    return hit;
}

static void dcache_cycle_sent(const struct efs_chunk_rec *r, uint64_t seq)
{
    uint32_t s = dcache_slot(r->ino, r->chunk_index);
    pthread_mutex_lock(dcache_mu(s));
    struct dcache_ent *e = dcache_find_meta(s, r->ino, r->chunk_index);
    if (e && seq && e->pending_seq == seq && e->object_seq == seq)
        e->pending_sent = 1;
    pthread_mutex_unlock(dcache_mu(s));
}

/* A successful rebase/PUT becomes a completed failed publish cycle only
 * after a REPORT was sent and a successful authoritative repull proves
 * that exact snapshot did not land. Failed pulls never call this function. */
static int dcache_cycle_verdict(efs_ino_t ino, uint32_t ci,
                                const struct efs_chunk_entry *ce,
                                uint64_t object_seq)
{
    uint32_t s = dcache_slot(ino, ci);
    int stalled = 0;
    pthread_mutex_lock(dcache_mu(s));
    struct dcache_ent *e = dcache_find_meta(s, ino, ci);
    if (e && e->pending_sent && e->pending_seq && e->pending_seq == object_seq &&
        e->object_seq == object_seq &&
        e->pending_cycle.operation == e->mutation) {
        struct efs_wb_cycle_state *p = &e->pending_cycle;
        if (++e->recovery_cycles % 256 == 0)
            fprintf(stderr, "publish-contention ino=%llu ci=%u cycles=%u gen=%llu epoch=%llu spans=%u\n",
                    (unsigned long long)ino, ci, e->recovery_cycles,
                    (unsigned long long)ce->generation,
                    (unsigned long long)ce->read_view.fence_epoch, ce->ndelta);
        /* Movement since the rebase is ordinary contention. Count the
         * current observation only when it is still the one we fetched. */
        if (p->generation != ce->generation ||
            p->epoch != ce->read_view.fence_epoch || p->spans != ce->ndelta) {
            efs_wb_cycle_reset(&e->recovery);
        } else if (efs_wb_cycle_complete(&e->recovery, p->operation,
                                         p->generation, p->epoch, p->spans)) {
            if (!e->stalled && wb_error_record_add(ino)) {
                e->stalled = 1;
                dcache_pin_add(e);
                fprintf(stderr, "publish-stalled ino=%llu ci=%u gen=%llu epoch=%llu spans=%u cycles=%u\n",
                        (unsigned long long)ino, ci,
                        (unsigned long long)p->generation,
                        (unsigned long long)p->epoch, p->spans,
                        e->recovery.unchanged);
            }
            stalled = 1; /* allocation failure still fails this sync */
        }
        e->pending_seq = 0;
        e->pending_sent = 0;
    }
    pthread_mutex_unlock(dcache_mu(s));
    return stalled;
}

static int report_replay_stale(efs_ino_t ino)
{
    /* Per-ci replay happens from the STALE report's dirty snapshot.
     * A whole-file GETCHUNKS here restamped ftruncate stubs into the
     * staging table and the next REPORT republished never-PUT gens. */
    (void)ino;
    return EFS_OK;
}

int efs_dcache_replay_stale(efs_ino_t ino, uint32_t ci)
{
    return dcache_replay_stale_ex(ino, ci, 0);
}

/* The host has no row for this chunk (the repull said so): the table's
 * generation and span list are this client's own unreported PUT. Drop
 * them so the fetch below starts from an empty row; the nodes stay (the
 * rec builder needs them). */
static void export_forget_unreported(efs_ino_t ino, uint32_t ci)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    (void)efs_export_set_chunk_gen(&g_client.export, ino, ci, 0);
    (void)efs_export_set_chunk_deltas(&g_client.export, ino, ci, NULL, 0, 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
}

/* absent=1: the STALE classifier's repull found no row on the host. The
 * committed image is then empty, whatever the table holds (our own PUT,
 * see stale_repull_replay). The slot's whole buffer is this client's
 * bytes: base + ranges for a partial write, the full image otherwise. */
static int dcache_replay_stale_ex_budgeted(efs_ino_t ino, uint32_t ci, int absent)
{
    uint32_t s, len, roff[DCACHE_NR], rlen[DCACHE_NR];
    uint8_t nrange = 0;
    uint64_t bg, committed, seq, mutation;
    uint8_t *copy = NULL, *base = NULL;
    struct dcache_ent *e;
    struct efs_chunk_entry ce;
    int rc;
    int have_base = 0;
    uint64_t object_gen = 0;
    pthread_mutex_t *io;
    uint32_t span_off = 0, span_len = 0;
    struct efs_chunk_entry obs;
    int have_obs = 0;

    if (!ino)
        return EFS_OK;
    memset(&obs, 0, sizeof(obs));
    dcache_ensure();
    if (absent)
        export_forget_unreported(ino, ci);
    s = dcache_slot(ino, ci);
    io = dcache_io_mu(s);
    pthread_mutex_lock(io);
    pthread_mutex_lock(dcache_mu(s));
    e = dcache_find(s, ino, ci);
    if (!e || !e->data || !e->len) {
        pthread_mutex_unlock(dcache_mu(s));
        pthread_mutex_unlock(io);
        return EFS_OK;
    }
    bg = e->base_gen;
    len = e->len;
    nrange = e->nrange;
    have_base = e->have_base;
    object_gen = e->object_gen;
    span_off = e->object_delta_off;
    span_len = e->object_delta_len;
    if (nrange)
        memcpy(roff, e->roff, (size_t)nrange * sizeof(uint32_t));
    if (nrange)
        memcpy(rlen, e->rlen, (size_t)nrange * sizeof(uint32_t));
    copy = efs_buf_alloc(len);
    if (!copy) {
        pthread_mutex_unlock(dcache_mu(s));
        pthread_mutex_unlock(io);
        return EFS_ERR_NOMEM;
    }
    memcpy(copy, e->data, len);
    mutation = e->mutation;
    seq = dcache_seq_next(e);
    DTRACE(e, "snap-replay");
    pthread_mutex_unlock(dcache_mu(s));

    /* The host said expected != committed. Local table gen matching
     * our base_gen is a stale GETCHUNKS, not "already merged". Always
     * refetch and overlay this client's ranges. */
    committed = 0;
    if (export_chunk_copy(ino, ci, &ce) == 0)
        committed = ce.generation;

    base = efs_buf_alloc(len);
    if (!base) {
        efs_buf_free(copy, len);
        pthread_mutex_unlock(io);
        return EFS_ERR_NOMEM;
    }
    if (absent) {
        /* Empty row: zeros, empty observation, expected gen 0. The
         * table's nodes/checksums are our own object; a fetch through
         * them would read that object back as "committed". */
        memset(base, 0, len);
        committed = 0;
        have_obs = 1;
        rc = EFS_OK;
    } else {
        struct efs_chunk_entry fobs;
        int have_fobs = 0;

        rc = efs_client_fetch_published_chunk_obs(ino, ci, base, len, &fobs,
                                                  &have_fobs);
        /* The row the image was built on, not the pre-fetch table read.
         * No row = zero base = empty observation (dcache_steal_dirty). */
        if (rc == EFS_OK) {
            committed = have_fobs ? fobs.generation : 0;
            obs = fobs;
            have_obs = 1;
        }
    }
    if (dcache_trace_on())
        fprintf(stderr, "dcache ino=%llu ci=%u replay committed=%llx fetch_rc=%d nrange=%u absent=%d\n",
                (unsigned long long)ino, ci, (unsigned long long)committed, rc,
                nrange, absent);
    if (rc != EFS_OK) {
        /* A zero-checksum stub returns OK above. This is a span object
         * we could not read. Zero-filling and publishing that image
         * drops every earlier span (concurrent_appends: full size,
         * NUL records). Leave the chain in place. */
        efs_buf_free(base, len);
        efs_buf_free(copy, len);
        pthread_mutex_unlock(io);
        return rc;
    }
    /* A matching base object does not make our body the complete current
     * image: newer spans can hang off that same base (W38). Preserve the
     * fetched span bytes and overlay only our owned ranges in that case.
     * With no live spans the local buffer still contains every byte this
     * client wrote, including ranges cleared after commit. A different
     * base generation is the peer's image (peer_shared_pwrite). */
    if (absent) {
        /* Nothing published: every byte we hold is ours to publish —
         * the ranges of a partial write, the span, or the whole image.
         * CAS on the empty row (not UNCOND): a peer span that lands
         * first STALEs us into a normal round instead of being erased. */
        if (nrange) {
            uint8_t i;

            for (i = 0; i < nrange; i++)
                if (roff[i] + rlen[i] <= len)
                    memcpy(base + roff[i], copy + roff[i], rlen[i]);
        } else if (span_len && (uint64_t)span_off + span_len <= len) {
            memcpy(base + span_off, copy + span_off, span_len);
        } else {
            memcpy(base, copy, len);
        }
        bg = 0;
    } else if (have_base && object_gen && committed == object_gen &&
               !chunk_obs_has_live_spans(&obs)) {
        memcpy(base, copy, len);
        bg = committed;
    } else if (!nrange) {
        /* A span publish clears nrange once it commits. The bytes
         * outside that span are not this client's; copying the whole
         * image deletes a peer's exclusive range. A full-chunk image
         * (span_len == 0) is still a full overwrite. */
        if (span_len && (uint64_t)span_off + span_len <= len) {
            memcpy(base + span_off, copy + span_off, span_len);
            bg = committed;
        } else if (!(have_base && object_gen &&
                     (committed != object_gen ||
                      chunk_obs_has_live_spans(&obs)))) {
            memcpy(base, copy, len);
            bg = EFS_CHUNK_BASE_UNCOND;
        } else {
            bg = committed;
        }
    } else {
        uint8_t i;
        for (i = 0; i < nrange; i++) {
            if (roff[i] + rlen[i] <= len)
                memcpy(base + roff[i], copy + roff[i], rlen[i]);
        }
        bg = committed;
    }
    rc = dcache_put_now(ino, ci, base, len, bg, seq, 0, 0,
                        have_obs ? &obs : NULL);
    pthread_mutex_lock(dcache_mu(s));
    e = dcache_find(s, ino, ci);
    if (e && e->data && e->len >= len) {
        if (e->base_gen != EFS_CHUNK_BASE_UNCOND)
            e->base_gen = bg;
        dcache_install_image(e, base, len, seq);
        if (rc == EFS_OK && (absent || obs.read_view.count) &&
            e->mutation == mutation && e->object_seq == seq) {
            e->pending_cycle = (struct efs_wb_cycle_state){
                .operation = mutation, .generation = obs.generation,
                .epoch = obs.read_view.fence_epoch, .spans = obs.ndelta,
                .valid = 1};
            e->pending_seq = seq;
            e->pending_sent = 0;
        }
    }
    pthread_mutex_unlock(dcache_mu(s));
    efs_buf_free(base, len);
    efs_buf_free(copy, len);
    pthread_mutex_unlock(io);
    return rc;
}

static int dcache_replay_stale_ex(efs_ino_t ino, uint32_t ci, int absent)
{
    int rc;
    efs_buf_drain_enter();
    rc = dcache_replay_stale_ex_budgeted(ino, ci, absent);
    efs_buf_drain_leave();
    return rc;
}

/* Background incremental reclaim. The old maybe_reclaim ran a full
 * efs_dcache_flush_all() on the WRITER thread once dirty_bytes crossed the
 * limit: a 65536-slot walk with a synchronous GET+PUT per dirty chunk. With
 * a working set at/over the limit (random 4k on >= 2 GiB files) that fired
 * constantly — flush_all was 41% of efs-fuse CPU and chunks were re-PUT ~4x
 * per test as writers re-dirtied slots mid-walk. Now writers just signal;
 * a small pool sweeps slots round-robin until dirty_bytes is back under the
 * low-water mark, so drain cost is spread and parallel instead of a stall. */
#define DCACHE_RECLAIM_THREADS 16
#define DCACHE_RECLAIM_SCAN    64 /* slots per worker wake */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t cursor;
    uint64_t lim;
    int shutdown;
    int active;
    /* Writer kicks (efs_dcache_maybe_reclaim) and the kick count the last
     * EMPTY sweep saw. Only have_base slots are reclaimable here; a fresh
     * file's chunks (no base) drain through the writer pipeline and close.
     * Under ecopy every dirty slot was such a chunk, dirty_bytes sat above
     * the limit, and 16 workers re-walked 64 dirty lists back to back:
     * dcache_reclaim_main was 39.8 % of efs-fuse (Sep 30 2026,
     * results/measure/20260930-051000-review2). A worker whose sweep
     * found nothing now parks until a writer kicks again, and no sooner
     * than RECLAIM_EMPTY_NAP_NS after the empty sweep. */
    uint64_t kicks;
    uint64_t empty_kicks;
    int64_t empty_until_ns;
    /* Full-chunk overwrites waiting to be PUT before close (W30). */
    uint64_t complete;
    /* Workers inside cond_wait. A kick takes `mu` only when this is
     * non-zero (Oct 1 2026, fstor007 20 GiB dd: `dcache_kick_complete`
     * ran 8× per 1 MiB write and every finished sweep broadcast to the
     * other 15 workers — 52 000 futex calls/s on `mu`, 18 % of the
     * client's cycles in the kernel spinlock, the FUSE thread queued on
     * the same mutex). Dekker order: the worker increments `waiters`
     * before it re-reads `complete`/`kicks`; the kicker bumps those
     * before it reads `waiters`. Both sides are seq_cst, so one of them
     * sees the other's write and no wakeup is lost. */
    int waiters;
} g_reclaim = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static pthread_once_t g_reclaim_once = PTHREAD_ONCE_INIT;

static uint64_t dcache_reclaim_limit(void)
{
    /* Soft reclaim target; allocation admission is enforced in bufpool.c.
     * Dirty-byte accounting alone excludes snapshots and retained bodies. */
    uint64_t lim = 128ull << 20;
    const char *env = getenv("EFS_DCACHE_BYTES");
    if (env && *env) {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && v >= (1ull << 20) && v <= (4ull << 30))
            lim = (uint64_t)v;
    }
    return lim;
}

#define RECLAIM_EMPTY_NAP_NS (10ll * 1000 * 1000)

static int64_t reclaim_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void *dcache_reclaim_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_reclaim.mu);
    for (;;) {
        uint64_t kicks_seen;
        int npopped = 0;

        for (;;) {
            int64_t now;
            uint64_t kicks;

            if (g_reclaim.shutdown)
                break;
            /* Announce the wait before the checks (see `waiters`). */
            __atomic_add_fetch(&g_reclaim.waiters, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_SEQ_CST) <=
                    g_reclaim.lim &&
                __atomic_load_n(&g_reclaim.complete, __ATOMIC_SEQ_CST) == 0) {
                pthread_cond_wait(&g_reclaim.cv, &g_reclaim.mu);
                __atomic_sub_fetch(&g_reclaim.waiters, 1, __ATOMIC_SEQ_CST);
                continue;
            }
            /* The last sweep found nothing and no writer has kicked
             * since: the dirty set is fresh-file chunks, not our work.
             * A complete-chunk kick is work even under the cap. */
            kicks = __atomic_load_n(&g_reclaim.kicks, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_reclaim.complete, __ATOMIC_SEQ_CST) == 0 &&
                kicks == g_reclaim.empty_kicks) {
                pthread_cond_wait(&g_reclaim.cv, &g_reclaim.mu);
                __atomic_sub_fetch(&g_reclaim.waiters, 1, __ATOMIC_SEQ_CST);
                continue;
            }
            now = reclaim_now_ns();
            if (__atomic_load_n(&g_reclaim.complete, __ATOMIC_SEQ_CST) == 0 &&
                now < g_reclaim.empty_until_ns) {
                struct timespec ts;

                ts.tv_sec = (time_t)(g_reclaim.empty_until_ns / 1000000000ll);
                ts.tv_nsec = (long)(g_reclaim.empty_until_ns % 1000000000ll);
                (void)pthread_cond_timedwait(&g_reclaim.cv, &g_reclaim.mu,
                                             &ts);
                __atomic_sub_fetch(&g_reclaim.waiters, 1, __ATOMIC_SEQ_CST);
                continue;
            }
            __atomic_sub_fetch(&g_reclaim.waiters, 1, __ATOMIC_SEQ_CST);
            break;
        }
        if (g_reclaim.shutdown)
            break;
        kicks_seen = __atomic_load_n(&g_reclaim.kicks, __ATOMIC_SEQ_CST);
        g_reclaim.active++;
        pthread_mutex_unlock(&g_reclaim.mu);

        uint64_t low = g_reclaim.lim - g_reclaim.lim / 8;
        /* Up to one pipeline of PUTs in flight (the close path's
         * flush_pipe), not one chunk then a wait. Full overwrites are
         * taken even under the dirty cap; partial have_base chunks only
         * when over it. Sparse appends stay for close/fsync. */
        {
            struct flush_pipe pipe;
            int i;

            flush_pipe_init(&pipe);
            for (i = 0; i < (int)EFS_WRITE_PIPELINE && pipe.rc == EFS_OK;
                 i++) {
                uint64_t dirty = __atomic_load_n(&g_dcache.dirty_bytes,
                                                 __ATOMIC_RELAXED);
                uint64_t comp = __atomic_load_n(&g_reclaim.complete,
                                                __ATOMIC_RELAXED);
                efs_ino_t pino = 0;
                uint32_t pci = 0;
                int found = 0;
                int full = 0;
                int sh;
                uint8_t *copy = NULL;
                uint32_t len = 0;
                uint64_t bg = 0, seq = 0;
                uint32_t doff = 0, dlen = 0;
                int drop_body = 0;
                struct efs_chunk_entry obs;
                int have_obs = 0;
                int st;

                if (comp == 0 && dirty <= low)
                    break;
                for (sh = 0; sh < DCACHE_SHARDS && !found; sh++) {
                    struct dcache_ent *e;

                    pthread_mutex_lock(&g_dcache.shard[sh]);
                    for (e = g_dcache.dirty_head[sh]; e; e = e->dirty_next) {
                        int is_full;

                        if (!e->dirty || !e->data || !e->have_base ||
                            e->reclaim_claimed)
                            continue;
                        is_full = dcache_full_overwrite(e->have_base,
                                                        e->base_gen,
                                                        e->nrange);
                        if (!is_full && dirty <= g_reclaim.lim)
                            continue;
                        e->reclaim_claimed = 1;
                        pino = e->ino;
                        pci = e->ci;
                        full = is_full;
                        found = 1;
                        break;
                    }
                    pthread_mutex_unlock(&g_dcache.shard[sh]);
                }
                if (!found) {
                    __atomic_store_n(&g_reclaim.complete, 0, __ATOMIC_RELAXED);
                    break;
                }
                if (full) {
                    uint64_t c = __atomic_load_n(&g_reclaim.complete,
                                                 __ATOMIC_RELAXED);
                    if (c)
                        __atomic_fetch_sub(&g_reclaim.complete, 1,
                                           __ATOMIC_RELAXED);
                }
                st = dcache_steal_dirty(pino, pci, &copy, &len, &bg, &seq,
                                        &doff, &dlen, &drop_body, &obs,
                                        &have_obs);
                if (st <= 0) {
                    uint32_t s = dcache_slot(pino, pci);
                    pthread_mutex_t *mu = dcache_mu(s);
                    struct dcache_ent *e;

                    pthread_mutex_lock(mu);
                    for (e = &g_dcache.e[s]; e; e = e->next)
                        if (e->ino == pino && e->ci == pci &&
                            e->reclaim_claimed)
                            e->reclaim_claimed = 0;
                    pthread_mutex_unlock(mu);
                    if (st < 0)
                        pipe.rc = st;
                    continue;
                }
                npopped++;
                if (flush_pipe_add(&pipe, pino, pci, copy, len, bg, seq, doff,
                                   dlen, drop_body,
                                   have_obs ? &obs : NULL) != EFS_OK)
                    break;
            }
            (void)flush_pipe_drain(&pipe);
        }
        pthread_mutex_lock(&g_reclaim.mu);
        g_reclaim.active--;
        if (npopped == 0) {
            /* Kicks that arrived during the sweep stay unconsumed:
             * kicks_seen was read before the walk. */
            if ((int64_t)(g_reclaim.empty_kicks - kicks_seen) < 0)
                g_reclaim.empty_kicks = kicks_seen;
            g_reclaim.empty_until_ns = reclaim_now_ns() + RECLAIM_EMPTY_NAP_NS;
        } else {
            /* There was work; this worker goes straight back in (the
             * loop head re-checks), and parked workers were already
             * signalled once per kick. Waking all 15 others here was
             * the herd. */
            g_reclaim.empty_kicks =
                __atomic_load_n(&g_reclaim.kicks, __ATOMIC_SEQ_CST) - 1;
        }
        if (g_reclaim.shutdown)
            pthread_cond_broadcast(&g_reclaim.cv); /* reclaim_stop waits */
    }
    pthread_mutex_unlock(&g_reclaim.mu);
    return NULL;
}

/* One kick: count it, wake one parked worker if there is one. Busy
 * workers re-read `kicks`/`complete` at their loop head and need no
 * signal; taking `mu` for them put the FUSE thread behind the pool. */
static void reclaim_kick(void)
{
    __atomic_add_fetch(&g_reclaim.kicks, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_reclaim.waiters, __ATOMIC_SEQ_CST) == 0)
        return;
    pthread_mutex_lock(&g_reclaim.mu);
    pthread_cond_signal(&g_reclaim.cv);
    pthread_mutex_unlock(&g_reclaim.mu);
}

static void dcache_reclaim_start(void)
{
    g_reclaim.lim = dcache_reclaim_limit();
    for (int i = 0; i < DCACHE_RECLAIM_THREADS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, dcache_reclaim_main, NULL) != 0) {
            g_reclaim.shutdown = 1;
            pthread_cond_broadcast(&g_reclaim.cv);
            return;
        }
        pthread_detach(t);
    }
}

static void dcache_kick_complete(void)
{
    pthread_once(&g_reclaim_once, dcache_reclaim_start);
    __atomic_add_fetch(&g_reclaim.complete, 1, __ATOMIC_SEQ_CST);
    reclaim_kick();
}

void efs_dcache_maybe_reclaim(void)
{
    /* getenv per write walks environ on the hot path; the limit is fixed
     * for the process, so read it once (idempotent benign race). */
    static uint64_t lim;
    if (!lim)
        lim = dcache_reclaim_limit();
    uint64_t dirty =
        __atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED);
    if (dirty <= lim)
        return;
    pthread_once(&g_reclaim_once, dcache_reclaim_start);
    reclaim_kick();
    /* Inline assistance at 2x the soft target: the background pool is
     * drain-limited (a partial-entry flush is a GET+PUT at disk latency).
     * Allocation admission is the hard bound; this only helps throughput.
     * A few slots inline is backpressure, not the old full-table stall. */
    if (dirty > lim * 2) {
        for (int i = 0; i < 8; i++) {
            uint32_t s = __atomic_fetch_add(&g_reclaim.cursor, 1,
                                            __ATOMIC_RELAXED) &
                         (DCACHE_SLOTS - 1);
            (void)dcache_flush_slot(s, 0, 0);
            if (__atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED) <=
                lim * 2)
                break;
        }
    }
}

/* Stop the reclaim pool before shutdown-time state (conns, export) goes
 * away; an in-flight flush would otherwise touch freed client state. Waits
 * until no worker is mid-flush. */
void efs_dcache_reclaim_stop(void)
{
    pthread_mutex_lock(&g_reclaim.mu);
    g_reclaim.shutdown = 1;
    pthread_cond_broadcast(&g_reclaim.cv);
    while (g_reclaim.active > 0)
        pthread_cond_wait(&g_reclaim.cv, &g_reclaim.mu);
    pthread_mutex_unlock(&g_reclaim.mu);
}

/* Persistent PUT workers draining a shared FIFO of chunk jobs. Reentrant:
 * each put_pool_run enqueues its jobs tagged with a per-batch tracker and
 * waits on that tracker, so concurrent writers (WB workers) all stay in flight
 * instead of racing on a single batch slot. Queue depth spans several batches
 * so enqueuers rarely block. */
#define PUT_POOL_QDEPTH 32
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct chunk_put_job *q[PUT_POOL_QDEPTH];
    int head, tail, count;
} g_put_sh[EFS_WRITE_PIPELINE];
static struct {
    pthread_t tids[EFS_WRITE_PIPELINE];
    int nworkers;
    int ready;
    int shutdown;
    int rr;
} g_put_pool;

static void *put_pool_thread(void *arg)
{
    int si = (int)(intptr_t)arg;
    for (;;) {
        struct chunk_put_job *job;
        int was_full;

        pthread_mutex_lock(&g_put_sh[si].mu);
        while (g_put_sh[si].count == 0 && !__atomic_load_n(&g_put_pool.shutdown, __ATOMIC_ACQUIRE))
            pthread_cond_wait(&g_put_sh[si].cv, &g_put_sh[si].mu);
        if (__atomic_load_n(&g_put_pool.shutdown, __ATOMIC_ACQUIRE) && g_put_sh[si].count == 0) {
            pthread_mutex_unlock(&g_put_sh[si].mu);
            return NULL;
        }
        was_full = (g_put_sh[si].count == PUT_POOL_QDEPTH);
        job = g_put_sh[si].q[g_put_sh[si].head];
        g_put_sh[si].head = (g_put_sh[si].head + 1) % PUT_POOL_QDEPTH;
        g_put_sh[si].count--;
        if (was_full)
            pthread_cond_signal(&g_put_sh[si].cv);
        pthread_mutex_unlock(&g_put_sh[si].mu);

        uint64_t previous = efs_client_rpc_deadline_ms();
        efs_client_rpc_set_deadline_ms(job->deadline);
        if (efs_client_rpc_past_deadline())
            job->rc = EFS_ERR_BUSY;
        else
            chunk_put_worker(job);
        efs_client_rpc_set_deadline_ms(previous);

        struct put_batch *bp = job->bp;
        pthread_mutex_lock(&bp->mu);
        if (--bp->remaining == 0)
            pthread_cond_signal(&bp->cv);
        pthread_mutex_unlock(&bp->mu);
    }
}

static pthread_mutex_t g_put_init = PTHREAD_MUTEX_INITIALIZER;

static int put_pool_ensure(void)
{
    uint32_t i;
    if (__atomic_load_n(&g_put_pool.ready, __ATOMIC_ACQUIRE))
        return 0;
    pthread_mutex_lock(&g_put_init);
    if (!__atomic_load_n(&g_put_pool.ready, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_put_pool.shutdown, 0, __ATOMIC_RELEASE);
        uint32_t n = EFS_WRITE_PIPELINE;
        for (i = 0; i < n; i++) {
            pthread_mutex_init(&g_put_sh[i].mu, NULL);
            pthread_cond_init(&g_put_sh[i].cv, NULL);
        }
        for (i = 0; i < n; i++) {
            if (pthread_create(&g_put_pool.tids[i], NULL, put_pool_thread,
                               (void *)(intptr_t)i) != 0) {
                uint32_t s, j;
                __atomic_store_n(&g_put_pool.shutdown, 1, __ATOMIC_RELEASE);
                for (s = 0; s < n; s++) {
                    pthread_mutex_lock(&g_put_sh[s].mu);
                    pthread_cond_broadcast(&g_put_sh[s].cv);
                    pthread_mutex_unlock(&g_put_sh[s].mu);
                }
                for (j = 0; j < i; j++)
                    pthread_join(g_put_pool.tids[j], NULL);
                g_put_pool.shutdown = 0;
                for (s = 0; s < n; s++) {
                    pthread_cond_destroy(&g_put_sh[s].cv);
                    pthread_mutex_destroy(&g_put_sh[s].mu);
                }
                pthread_mutex_unlock(&g_put_init);
                return -1;
            }
        }
        g_put_pool.nworkers = (int)n;
        __atomic_store_n(&g_put_pool.ready, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_put_init);
    return 0;
}

static int put_pool_run(struct chunk_put_job *jobs, uint32_t batch)
{
    struct put_batch bp;
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint32_t i;
    if (!batch)
        return 0;
    uint64_t deadline = previous ? previous : report_clock_ms() + 30000;
    efs_client_rpc_set_deadline_ms(deadline);
    if (batch == 1 || put_pool_ensure() != 0) {
        for (i = 0; i < batch; i++) {
            if (efs_client_rpc_past_deadline())
                jobs[i].rc = EFS_ERR_BUSY;
            else
                chunk_put_worker(&jobs[i]);
        }
        efs_client_rpc_set_deadline_ms(previous);
        return 0;
    }
    pthread_mutex_init(&bp.mu, NULL);
    pthread_cond_init(&bp.cv, NULL);
    bp.remaining = (int)batch;
    int base = (int)__sync_fetch_and_add(&g_put_pool.rr, 1);
    for (i = 0; i < batch; i++) {
        int si = (int)(((unsigned)base + i) % (unsigned)g_put_pool.nworkers);
        jobs[i].bp = &bp;
        jobs[i].deadline = deadline;
        pthread_mutex_lock(&g_put_sh[si].mu);
        while (g_put_sh[si].count == PUT_POOL_QDEPTH &&
               !__atomic_load_n(&g_put_pool.shutdown, __ATOMIC_ACQUIRE) &&
               !efs_client_rpc_past_deadline()) {
            uint64_t now = report_clock_ms();
            if (now >= deadline)
                break;
            uint64_t wait = deadline - now;
            if (wait > 50) wait = 50;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += (long)wait * 1000000;
            until.tv_sec += until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&g_put_sh[si].cv, &g_put_sh[si].mu, &until);
        }
        int shutdown = __atomic_load_n(&g_put_pool.shutdown, __ATOMIC_ACQUIRE);
        if (shutdown || efs_client_rpc_past_deadline()) {
            pthread_mutex_unlock(&g_put_sh[si].mu);
            jobs[i].rc = shutdown ? EFS_ERR_IO : EFS_ERR_BUSY;
            jobs[i].bp = NULL;
            pthread_mutex_lock(&bp.mu);
            --bp.remaining;
            pthread_mutex_unlock(&bp.mu);
            continue;
        }
        g_put_sh[si].q[g_put_sh[si].tail] = &jobs[i];
        g_put_sh[si].tail = (g_put_sh[si].tail + 1) % PUT_POOL_QDEPTH;
        g_put_sh[si].count++;
        /* Wake immediately: waiting for all dispatches can deadlock when
         * concurrent producers fill shards before any batch signals them. */
        pthread_cond_signal(&g_put_sh[si].cv);
        pthread_mutex_unlock(&g_put_sh[si].mu);
    }
    /* Accepted jobs own stack references until their exact completion. */
    pthread_mutex_lock(&bp.mu);
    while (bp.remaining > 0)
        pthread_cond_wait(&bp.cv, &bp.mu);
    pthread_mutex_unlock(&bp.mu);
    pthread_mutex_destroy(&bp.mu);
    pthread_cond_destroy(&bp.cv);
    efs_client_rpc_set_deadline_ms(previous);
    return 0;
}

static void *chunk_put_worker(void *arg)
{
    struct chunk_put_job *job = arg;
    uint32_t chunk_size, frag_len;
    uint64_t chunk_start, wr_start, wr_end;
    int covers_full;

    if (job->flush_put) {
        job->rc = dcache_put_now(job->ino, job->ci, job->flush_buf,
                                 job->flush_len, job->flush_base_gen,
                                 job->flush_seq, job->flush_delta_off,
                                 job->flush_delta_len,
                                 job->flush_have_obs ? &job->flush_obs : NULL);
        return NULL;
    }
    chunk_size = data_chunk_size();
    frag_len = data_frag_size();
    chunk_start = (uint64_t)job->ci * chunk_size;
    wr_start = (job->offset > chunk_start) ? job->offset : chunk_start;
    wr_end = (job->end < chunk_start + chunk_size)
                 ? job->end
                 : chunk_start + chunk_size;
    covers_full = (wr_start == chunk_start &&
                   wr_end == chunk_start + chunk_size);

    /* dd if=/dev/zero / full-chunk zeros: skip assemble/encode/malloc and PUT
     * shared zero pages with the cached zero digest. */
    if (covers_full) {
        size_t src_off = (size_t)(wr_start - job->offset);
        if (efs_bytes_are_zero(job->buf + src_off, chunk_size)) {
            efs_hash_zero_fragment_len(frag_len, job->checksums[0]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[1]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[2]);
            efs_place_fragments(g_client.nodes, g_client.node_count, job->ino,
                                job->ci, job->nodes);
            /* Do not PUT all-zero fragments. The chunk table stores the
             * well-known zero digest; decode synthesizes zeros on read.
             * dd if=/dev/zero was spending ~1 GB/s on 2-ack PUTs of zeros. */
            efs_dcache_drop(job->ino, job->ci);
            job->object_gen = new_chunk_object_gen(job->ci);
            job->rc = job->object_gen ? EFS_OK : EFS_ERR_IO;
            return NULL;
        }

        /* Full overwrite: encode in place from the caller's buffer — skip the
         * extra 128 KiB assemble memcpy that showed up as 8–10% of seq write.
         * Parity scratch is a per-thread static (default geometry): one less
         * malloc/free per chunk PUT. */
        static __thread uint8_t parity_tls[EFS_FRAGMENT_SIZE];
        uint8_t *parity = parity_tls;
        if (frag_len > EFS_FRAGMENT_SIZE) {
            parity = malloc(frag_len);
            if (!parity) {
                job->rc = EFS_ERR_NOMEM;
                return NULL;
            }
        }
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        const uint8_t *src = (const uint8_t *)job->buf + src_off;
        frags[0] = (uint8_t *)(uintptr_t)src;
        frags[1] = (uint8_t *)(uintptr_t)(src + frag_len);
        frags[2] = parity;
        efs_encode_chunk(src, chunk_size, chunk_size, frags);
        const uint8_t *cfrags2[EFS_NUM_FRAGMENTS] = {frags[0], frags[1], frags[2]};
        hash_write_fragments(cfrags2, frag_len, chunk_size, 0, chunk_start,
                             wr_start, wr_end, job->checksums);
        efs_place_fragments(g_client.nodes, g_client.node_count, job->ino,
                            job->ci, job->nodes);
        job->rc = efs_client_put_fragments_parallel(
            job->ino, job->ci, job->nodes, cfrags2, frag_len, job->checksums, &job->object_gen);
        if (frag_len > EFS_FRAGMENT_SIZE)
            free(parity);
        efs_rdcache_invalidate(job->ino, job->ci);
        if (job->rc == EFS_OK)
            efs_dcache_drop(job->ino, job->ci);
        else
            (void)dcache_store(job->ino, job->ci, src, chunk_size);
        return NULL;
    }

    /* Pool-allocate the assemble buffer (malloc churn here was the arena
     * bloat source); frag scratch is a per-thread static for the default
     * geometry. Heap, not stack: large chunks exceed some FUSE/pthread
     * stacks when the worker runs inline on a FUSE thread. */
    static __thread uint8_t frag_tls[EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE];
    uint8_t *chunk = efs_buf_alloc(chunk_size);
    uint8_t *frag_buf = frag_tls;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (frag_len > EFS_FRAGMENT_SIZE)
        frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
    if (!chunk || !frag_buf) {
        efs_buf_free(chunk, chunk_size);
        if (frag_buf != frag_tls)
            free(frag_buf);
        job->rc = EFS_ERR_NOMEM;
        return NULL;
    }
    frag_ptrs(frag_buf, frag_len, frags);

    int from_zero = 0;
    int arc = assemble_write_chunk(job->ino, job->old_size, job->offset,
                                   job->buf, chunk_start, wr_start, wr_end,
                                   chunk_size, chunk, &from_zero);
    if (arc != EFS_OK) {
        efs_buf_free(chunk, chunk_size);
        if (frag_buf != frag_tls)
            free(frag_buf);
        job->rc = arc;
        return NULL;
    }

    const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        cfrags[i] = frags[i];
    efs_encode_chunk(chunk, chunk_size, chunk_size, frags);
    hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, chunk_start,
                         wr_start, wr_end, job->checksums);
    efs_place_fragments(g_client.nodes, g_client.node_count, job->ino, job->ci,
                        job->nodes);
    job->rc = efs_client_put_fragments_parallel(job->ino, job->ci, job->nodes,
                                                cfrags, frag_len, job->checksums, &job->object_gen);
    efs_rdcache_invalidate(job->ino, job->ci);
    if (job->rc != EFS_OK)
        (void)dcache_store(job->ino, job->ci, chunk, chunk_size);
    else
        efs_dcache_drop(job->ino, job->ci);
    efs_buf_free(chunk, chunk_size);
    if (frag_buf != frag_tls)
        free(frag_buf);
    return NULL;
}

static int write_chunks_no_replicate(efs_ino_t ino, uint64_t offset, size_t size,
                                     const char *buf, uint64_t old_size);


static int client_write_run(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    efs_client_mtime_unpin(ino);
    if (size == 0)
        return EFS_OK;

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);

    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t frag_len = data_frag_size();
    /* Only touch chunks that overlap [offset, end). Rewriting every prior
     * chunk on each append was an O(n^2) amplification of sequential writes. */
    uint32_t first_ci = (uint32_t)(offset / chunk_size);
    uint32_t last_ci = (uint32_t)((end - 1) / chunk_size);

    for (uint32_t ci = first_ci; ci <= last_ci; ci++) {
        uint64_t chunk_start = (uint64_t)ci * chunk_size;
        uint64_t wr_start = (offset > chunk_start) ? offset : chunk_start;
        uint64_t wr_end = (end < chunk_start + chunk_size) ? end
                                                              : chunk_start + chunk_size;

        static __thread uint8_t frag_tls[EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE];
        uint8_t *chunk = efs_buf_alloc(chunk_size);
        uint8_t *frag_buf = frag_tls;
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        if (frag_len > EFS_FRAGMENT_SIZE)
            frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
        if (!chunk || !frag_buf) {
            efs_buf_free(chunk, chunk_size);
            if (frag_buf != frag_tls)
                free(frag_buf);
            return EFS_ERR_NOMEM;
        }
        frag_ptrs(frag_buf, frag_len, frags);
        int from_zero = 0;
        int arc = assemble_write_chunk(ino, old_size, offset, buf,
                                       chunk_start, wr_start, wr_end,
                                       chunk_size, chunk, &from_zero);
        if (arc != EFS_OK) {
            efs_buf_free(chunk, chunk_size);
            if (frag_buf != frag_tls)
                free(frag_buf);
            return arc;
        }

        efs_encode_chunk(chunk, chunk_size, chunk_size, frags);

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);

        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            cfrags[i] = frags[i];
        hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, chunk_start,
                             wr_start, wr_end, checksums);

        uint64_t object_gen = 0;
        int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                                   checksums, &object_gen);
        efs_buf_free(chunk, chunk_size);
        if (frag_buf != frag_tls)
            free(frag_buf);
        if (rc != EFS_OK)
            return rc;

        if (!export_chunk_exists(ino, ci)) {
            int room = efs_client_ensure_meta_room(0, 1);
            if (room != EFS_OK)
                return room;
        }
        if (efs_export_needs_chunk_grow(&g_client.export))
            export_reserve_chunks_locked(4096);
        efs_client_lock_dir(ino);
        pthread_mutex_lock(&g_client.idx_mu);
        efs_client_set_chunk(&g_client.export, ino, ci, nodes, checksums);
        (void)efs_export_set_chunk_gen(&g_client.export, ino, ci,
                                       object_gen);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        putid_note(ino,ci,object_gen,0,nodes,checksums,0,0,0,0,NULL);
        efs_client_mark_chunk_dirty(ino, ci);
    }

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Grow size (sets mtime) OR bump mtime alone — never both (each used to
     * run sync_hardlink_attrs + parent rollups). Skip rollups until flush. */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    /* Durable at flush/close (report_dirty_ino), not at write(). A
     * synchronous whole-set report here doubled every small wr() under
     * the posix suite (names_crazy_dirs, dir_deep_nesting rmtree). */
    (void)efs_client_note_meta_change(0);

    return EFS_OK;
}

int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    if (efs_wb_inode_stalled(ino))
        return EFS_ERR_IO;
    if (!size)
        return EFS_OK;
    uint64_t cs = data_chunk_size();
    if (size > UINT64_MAX - offset)
        return EFS_ERR_INVAL;
    int rc = efs_client_report_admit(2 * (size / cs + 2) + 1);
    if (rc != EFS_OK)
        return rc;
    rc = client_write_run(ino, offset, size, buf);
    efs_client_report_unreserve();
    return rc;
}

int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    if (efs_wb_inode_stalled(ino))
        return EFS_ERR_IO;
    efs_client_mtime_unpin(ino);
    if (size == 0)
        return EFS_OK;

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);

    /* No pack staging: the KV row has no pack fields and chunks cannot
     * publish under a directory ino, so small writes take the chunk path
     * like everything else. */
    return write_chunks_no_replicate(ino, offset, size, buf, old_size);
}

static int write_chunks_no_replicate(efs_ino_t ino, uint64_t offset, size_t size,
                                     const char *buf, uint64_t old_size)
{

    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t first_ci = (uint32_t)(offset / chunk_size);
    uint32_t last_ci = (uint32_t)((end - 1) / chunk_size);
    uint32_t nchunks = last_ci - first_ci + 1;

    /* Pipeline independent chunk PUTs (each already fans out 3 fragments). */
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;
    if (pipe > nchunks)
        pipe = nchunks;

    for (uint32_t base = first_ci; base <= last_ci; ) {
        uint32_t batch = last_ci - base + 1;
        if (batch > pipe)
            batch = pipe;

        struct chunk_put_job jobs[EFS_WRITE_PIPELINE];

        for (uint32_t i = 0; i < batch; i++) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].ino = ino;
            jobs[i].ci = base + i;
            jobs[i].old_size = old_size;
            jobs[i].offset = offset;
            jobs[i].buf = buf;
            jobs[i].end = end;
            jobs[i].rc = EFS_ERR_IO;
            jobs[i].deferred = 0;
        }
        put_pool_run(jobs, batch);

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK)
                return jobs[i].rc;
            if (jobs[i].deferred)
                continue;
            if (!export_chunk_exists(ino, jobs[i].ci)) {
                int room = efs_client_ensure_meta_room(0, 1);
                if (room != EFS_OK)
                    return room;
            }
            if (efs_export_needs_chunk_grow(&g_client.export))
                export_reserve_chunks_locked(4096);
            efs_client_lock_dir(ino);
            pthread_mutex_lock(&g_client.idx_mu);
            {
                struct efs_chunk_entry prev;
                int have = (efs_export_get_chunk(&g_client.export, ino,
                                                 jobs[i].ci, &prev) == 0);
                int same_nodes = have &&
                    memcmp(prev.fragment_nodes, jobs[i].nodes,
                           sizeof(prev.fragment_nodes)) == 0;
                int same_ck = have &&
                    memcmp(prev.checksums, jobs[i].checksums,
                           sizeof(prev.checksums)) == 0;
                if (!same_nodes || !same_ck)
                    efs_client_set_chunk(&g_client.export, ino, jobs[i].ci,
                                         jobs[i].nodes, jobs[i].checksums);
                (void)efs_export_set_chunk_gen(&g_client.export, ino,
                                               jobs[i].ci,
                                               jobs[i].object_gen);
                /* The row must already cover this chunk's bytes when the
                 * chunk mark below makes it reportable: a REPORT snapshot
                 * between the mark and the size update at the end of this
                 * function shipped the chunk with a size-0 irec (fresh
                 * file), and the server sized the file to the chunk end
                 * (ecopy Makefile.in 32398 → 131072, Sep 30). */
                {
                    struct efs_inode cur;
                    uint64_t cend = ((uint64_t)jobs[i].ci + 1) * chunk_size;

                    if (cend > end)
                        cend = end;
                    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 &&
                        cur.size < cend)
                        efs_export_set_size_norollup(&g_client.export, ino,
                                                     cend);
                }
                pthread_mutex_unlock(&g_client.idx_mu);
                efs_client_unlock_dir(ino);
                putid_note(ino, jobs[i].ci,
                           jobs[i].object_gen,
                           0, jobs[i].nodes, jobs[i].checksums, 0, 0,
                           have ? prev.ndelta : 0,
                           have ? prev.delta_seq : 0, NULL);
                /* Same placement + new checksums is a new immutable
                 * generation. Skipping REPORT left KV on the ftruncate
                 * stub gen while PUT wrote `{ci}.{fi}.{G}` — remount
                 * GET of the stub missed (W1 DECODE). */
                if (!same_nodes || !same_ck || !have ||
                    prev.generation != jobs[i].object_gen) {
                    efs_client_mark_chunk_dirty(ino, jobs[i].ci);
                    /* Ino mark right behind the chunk mark, as in the
                     * dcache put-record path, so the irec is in the same
                     * snapshot as the chunk (the mark at the end of the
                     * function is too late for a concurrent REPORT). */
                    efs_client_mark_ino_dirty(ino);
                }
            }
        }
        base += batch;
    }

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Grow the size based on the current value under the lock, not the stale
     * copy read before the (unlocked) write loop, so concurrent writes on
     * different FUSE worker threads cannot shrink or mis-set the size.
     * Size grow already bumps mtime — do not also call set_mtime (that was
     * a double sync_hardlink_attrs + rollup walk per write job). */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);

    return EFS_OK;
}
