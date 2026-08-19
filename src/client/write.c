#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
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

static void dirty_sets_clear(void)
{
    if (g_client.dirty_ino_keys && g_client.dirty_ino_mask)
        memset(g_client.dirty_ino_keys, 0,
               (g_client.dirty_ino_mask + 1) * sizeof(uint64_t));
    if (g_client.dirty_chunk_keys && g_client.dirty_chunk_mask)
        memset(g_client.dirty_chunk_keys, 0,
               (g_client.dirty_chunk_mask + 1) * sizeof(uint64_t));
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_count = 0;
}

void efs_client_mark_ino_dirty(efs_ino_t ino)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    if (dirty_set_ensure(&g_client.dirty_ino_keys, &g_client.dirty_ino_mask,
                         g_client.dirty_ino_count + 1) != 0) {
        pthread_mutex_unlock(&g_client.dirty_mu);
        return;
    }
    dirty_set_put(g_client.dirty_ino_keys, g_client.dirty_ino_mask, ino,
                  &g_client.dirty_ino_count);
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
    uint64_t before = 0;
    if (g_client.dirty_chunk_keys && g_client.dirty_chunk_mask) {
        uint64_t key = pack_dirty_chunk(ino, chunk_index);
        uint64_t i = key & g_client.dirty_chunk_mask;
        for (uint64_t n = 0; n <= g_client.dirty_chunk_mask; n++) {
            if (g_client.dirty_chunk_keys[i] == 0)
                break;
            if (g_client.dirty_chunk_keys[i] == key)
                goto out;
            i = (i + 1) & g_client.dirty_chunk_mask;
        }
    }
    before = g_client.dirty_chunk_count;
    if (dirty_set_ensure(&g_client.dirty_chunk_keys, &g_client.dirty_chunk_mask,
                         before + 1) != 0)
        goto out;
    dirty_set_put(g_client.dirty_chunk_keys, g_client.dirty_chunk_mask,
                  pack_dirty_chunk(ino, chunk_index), &g_client.dirty_chunk_count);
    if (g_client.dirty_chunk_count == before)
        goto out;
    if (g_client.dirty_chunk_count > g_client.dirty_chunk_cap) {
        uint64_t ncap = g_client.dirty_chunk_cap ? g_client.dirty_chunk_cap * 2 : 64;
        efs_ino_t *ni = realloc(g_client.dirty_chunk_inos, ncap * sizeof(efs_ino_t));
        uint32_t *nx = realloc(g_client.dirty_chunk_idxs, ncap * sizeof(uint32_t));
        if (!ni || !nx) {
            free(ni);
            free(nx);
            goto out;
        }
        g_client.dirty_chunk_inos = ni;
        g_client.dirty_chunk_idxs = nx;
        g_client.dirty_chunk_cap = ncap;
    }
    uint64_t slot = g_client.dirty_chunk_count - 1;
    g_client.dirty_chunk_inos[slot] = ino;
    g_client.dirty_chunk_idxs[slot] = chunk_index;
out:
    pthread_mutex_unlock(&g_client.dirty_mu);
}

/* One PUT_META reply from a conn that reply_watch marked ready (or whose
 * poll fd fired). Consumes the reply; releases on OK, drops otherwise.
 * STALE means the node is healthy but already holds a newer generation —
 * count it separately and do NOT mark the node failed: with N concurrent
 * meta writers every race has a loser, and treating STALE as a node failure
 * wedged every loser's flush until remount. */
static void meta_root_recv_reply(efs_node_id_t nid, struct efs_conn *conn,
                                 int *acks, int *stales)
{
    uint8_t type = 0;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_conn_recv_msg(conn, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_PUT_META_REPLY || reply_len < 1) {
        free(reply);
        efs_client_conn_drop(nid, conn);
        efs_client_node_note_fail(nid);
        return;
    }
    uint8_t status = ((uint8_t *)reply)[0];
    free(reply);
    if (status == EFS_PUT_META_OK) {
        (*acks)++;
        efs_client_node_note_ok(nid);
        efs_client_conn_release(nid, conn);
    } else if (status == EFS_PUT_META_STALE) {
        (*stales)++;
        efs_client_conn_release(nid, conn);
    } else {
        efs_client_conn_drop(nid, conn);
        efs_client_node_note_fail(nid);
    }
}

/* Consume one META_FLUSH_BEGIN reply. BUSY/STALE mark healthy nodes — only
 * transport/parse failures drop the conn and note the node down. */
static void meta_begin_recv_reply(efs_node_id_t nid, struct efs_conn *conn,
                                  uint64_t gen, int *oks, int *busy,
                                  int *stale, int *answered)
{
    uint8_t type = 0;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_conn_recv_msg(conn, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_META_FLUSH_BEGIN_REPLY ||
        reply_len < sizeof(struct efs_msg_meta_flush_begin_reply)) {
        free(reply);
        efs_client_conn_drop(nid, conn);
        efs_client_node_note_fail(nid);
        (*answered)++;
        return;
    }
    struct efs_msg_meta_flush_begin_reply r;
    memcpy(&r, reply, sizeof(r));
    free(reply);
    (*answered)++;
    if (r.status == EFS_PUT_META_OK) {
        (*oks)++;
        efs_client_node_note_ok(nid);
    } else if (r.status == EFS_PUT_META_BUSY) {
        (*busy)++;
    }
    if (r.status == EFS_PUT_META_STALE || r.committed_gen >= gen)
        *stale = 1;
    efs_client_conn_release(nid, conn);
}

/* Meta flush election: pin (gen, writer_id) on a majority of nodes before
 * PUTting any meta page. Concurrent writers used to PUT the same dual-slot
 * page CIs with different content, tearing every overlapped page — the
 * election serializes flushes cluster-wide. Returns EFS_OK when the
 * election is won, EFS_ERR_STALE when a node already holds a committed gen
 * >= ours (caller must resync), EFS_ERR_BUSY when another live writer holds
 * the election (retry with backoff; our gen may still be next), and
 * EFS_ERR_NO_QUORUM when too few nodes answered. */
static int send_meta_begin(uint64_t gen, uint64_t writer_id)
{
    uint32_t n = g_client.node_count;
    if (n > EFS_MAX_NODES)
        n = EFS_MAX_NODES;
    if (n == 0)
        return EFS_ERR_NET;
    const int mq = (int)(n / 2 + 1);

    struct efs_msg_meta_flush_begin b;
    memset(&b, 0, sizeof(b));
    b.export_id = g_client.export_id;
    b.gen = gen;
    b.writer_id = writer_id;

    struct efs_conn *conns[EFS_MAX_NODES];
    efs_node_id_t nids[EFS_MAX_NODES];
    int pending[EFS_MAX_NODES];
    int oks = 0, busy = 0, stale = 0, answered = 0, sent = 0;

    for (uint32_t i = 0; i < n; i++) {
        conns[i] = NULL;
        pending[i] = 0;
        nids[i] = g_client.nodes[i].id;
        if (nids[i] == 0 || efs_client_node_is_down(nids[i]))
            continue;
        conns[i] = efs_client_conn_get(nids[i]);
        if (!conns[i]) {
            efs_client_node_note_fail(nids[i]);
            continue;
        }
        if (efs_conn_send_msg(conns[i], EFS_MSG_META_FLUSH_BEGIN, &b,
                              sizeof(b)) != 0) {
            efs_client_conn_drop(nids[i], conns[i]);
            efs_client_node_note_fail(nids[i]);
            conns[i] = NULL;
            continue;
        }
        pending[i] = 1;
        sent++;
    }
    if (sent < mq) {
        for (uint32_t i = 0; i < n; i++)
            if (conns[i])
                efs_client_conn_drop(nids[i], conns[i]);
        return EFS_ERR_NO_QUORUM;
    }

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (oks < mq && !stale) {
        /* Harvest replies that are already here (RDMA CQE / readable fd). */
        for (uint32_t i = 0; i < n && oks < mq && !stale; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                meta_begin_recv_reply(nids[i], conns[i], gen, &oks, &busy,
                                      &stale, &answered);
                conns[i] = NULL;
                pending[i] = 0;
            } else if (w < 0) {
                efs_client_conn_drop(nids[i], conns[i]);
                efs_client_node_note_fail(nids[i]);
                conns[i] = NULL;
                pending[i] = 0;
                answered++;
            }
        }
        if (oks >= mq || stale)
            break;

        struct pollfd pfds[EFS_MAX_NODES];
        int map[EFS_MAX_NODES];
        int npoll = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                meta_begin_recv_reply(nids[i], conns[i], gen, &oks, &busy,
                                      &stale, &answered);
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            } else if (w < 0) {
                efs_client_conn_drop(nids[i], conns[i]);
                efs_client_node_note_fail(nids[i]);
                conns[i] = NULL;
                pending[i] = 0;
                answered++;
                continue;
            }
            pfds[npoll].fd = w;
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = (int)i;
            npoll++;
        }
        if (oks >= mq || stale)
            break;
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (!pending[i] || !conns[i])
                    continue;
                efs_client_conn_drop(nids[i], conns[i]);
                efs_client_node_note_fail(nids[i]);
                conns[i] = NULL;
                pending[i] = 0;
            }
            break;
        }
        int wait_ms = left > 2000 ? 2000 : (int)left;
        int pr = poll(pfds, (nfds_t)npoll, wait_ms);
        if (pr <= 0)
            continue;

        for (int p = 0; p < npoll; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            int i = map[p];
            if (!pending[i] || !conns[i])
                continue;
            meta_begin_recv_reply(nids[i], conns[i], gen, &oks, &busy,
                                  &stale, &answered);
            conns[i] = NULL;
            pending[i] = 0;
        }
    }

    /* Election decided: drop stragglers without blocking on RCVTIMEO. */
    for (uint32_t i = 0; i < n; i++) {
        if (!pending[i] || !conns[i])
            continue;
        efs_client_conn_drop(nids[i], conns[i]);
        if (oks < mq && !stale)
            efs_client_node_note_fail(nids[i]);
        conns[i] = NULL;
        pending[i] = 0;
    }

    if (stale)
        return EFS_ERR_STALE;
    if (oks >= mq)
        return EFS_OK;
    if (answered >= mq && busy > 0)
        return EFS_ERR_BUSY;
    if (answered < mq)
        return EFS_ERR_NO_QUORUM;
    return EFS_ERR_BUSY;
}

/* Replicate the tiny export root (EFSR) to all nodes; need ≥2 acks.
 * Fan-out + poll (like fragment PUT): skip down peers, return as soon as
 * quorum is met so one dead/blackholed node cannot serialize SO_RCVTIMEO. */
static int send_meta_root(const char *buf, size_t len)
{
    uint32_t n = g_client.node_count;
    int stales = 0;
    if (n > EFS_MAX_NODES)
        n = EFS_MAX_NODES;

    struct efs_conn *conns[EFS_MAX_NODES];
    efs_node_id_t nids[EFS_MAX_NODES];
    int pending[EFS_MAX_NODES];
    int acks = 0;

    for (int pass = 0; pass < 2; pass++) {
        int got = 0;
        for (uint32_t i = 0; i < n; i++) {
            conns[i] = NULL;
            pending[i] = 0;
            nids[i] = g_client.nodes[i].id;
            if (nids[i] == 0 || efs_client_node_is_down(nids[i]))
                continue;
            conns[i] = efs_client_conn_get(nids[i]);
            if (!conns[i]) {
                efs_client_node_note_fail(nids[i]);
                continue;
            }
            got++;
        }
        if (got >= 2 || pass == 1)
            break;
        efs_client_nodes_force_reprobe();
    }

    for (uint32_t i = 0; i < n; i++) {
        if (!conns[i])
            continue;
        if (efs_conn_send_msg(conns[i], EFS_MSG_PUT_META, buf,
                              (uint32_t)len) != 0) {
            efs_client_conn_drop(nids[i], conns[i]);
            efs_client_node_note_fail(nids[i]);
            conns[i] = NULL;
            continue;
        }
        pending[i] = 1;
    }

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        /* Harvest replies that are already here (RDMA CQE / readable fd). */
        for (uint32_t i = 0; i < n && acks < 2; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                meta_root_recv_reply(nids[i], conns[i], &acks, &stales);
                conns[i] = NULL;
                pending[i] = 0;
            } else if (w < 0) {
                efs_client_conn_drop(nids[i], conns[i]);
                efs_client_node_note_fail(nids[i]);
                conns[i] = NULL;
                pending[i] = 0;
            }
        }
        if (acks >= 2)
            break;

        struct pollfd pfds[EFS_MAX_NODES];
        int map[EFS_MAX_NODES];
        int npoll = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                meta_root_recv_reply(nids[i], conns[i], &acks, &stales);
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            } else if (w < 0) {
                efs_client_conn_drop(nids[i], conns[i]);
                efs_client_node_note_fail(nids[i]);
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            }
            pfds[npoll].fd = w;
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = (int)i;
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
            for (uint32_t i = 0; i < n; i++) {
                if (!pending[i] || !conns[i])
                    continue;
                efs_client_conn_drop(nids[i], conns[i]);
                efs_client_node_note_fail(nids[i]);
                conns[i] = NULL;
                pending[i] = 0;
            }
            break;
        }
        int wait_ms = left > 2000 ? 2000 : (int)left;
        int pr = poll(pfds, (nfds_t)npoll, wait_ms);
        if (pr <= 0)
            continue;

        for (int p = 0; p < npoll; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            int i = map[p];
            if (!pending[i] || !conns[i])
                continue;
            meta_root_recv_reply(nids[i], conns[i], &acks, &stales);
            conns[i] = NULL;
            pending[i] = 0;
        }
    }

    /* Quorum met: drop stragglers without blocking on their RCVTIMEO. */
    for (uint32_t i = 0; i < n; i++) {
        if (!conns[i])
            continue;
        if (acks >= 2 && efs_conn_reply_watch(conns[i]) == EFS_CONN_REPLY_READY)
            meta_root_recv_reply(nids[i], conns[i], &acks, &stales);
        else if (acks < 2) {
            efs_client_conn_drop(nids[i], conns[i]);
            efs_client_node_note_fail(nids[i]);
        } else {
            efs_client_conn_drop(nids[i], conns[i]);
        }
        conns[i] = NULL;
        pending[i] = 0;
    }

    if (acks >= 2)
        return EFS_OK;
    /* A quorum of STALE replies means our generation is behind a concurrent
     * writer's committed root — the nodes are fine, we must resync. */
    if (stales >= 2)
        return EFS_ERR_STALE;
    return EFS_ERR_NO_QUORUM;
}

/* Serialize concurrent replicators so two flushes don't interleave page PUTs /
 * gen bumps. This replaces holding g_client.lock across serialize+network. */
static pthread_mutex_t g_repl_mu = PTHREAD_MUTEX_INITIALIZER;
/* Waiters must not block on g_repl_mu: replicate_metadata() takes it, so
 * locking it here serialized 8 fsyncs into 8 flushes. */
static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sync_meta_cv = PTHREAD_COND_INITIALIZER;
static int g_sync_meta_active;
static int g_sync_meta_rc;

/* Pack the full export into 2+1 pages, then push the EFSR root (≥2 acks). */
/* One flush attempt: serialize the full export and PUT all meta pages.
 * Caller must hold g_repl_mu. This never locks/unlocks g_repl_mu itself, so
 * every return path leaves the lock for the wrapper to release — previously
 * the page-PUT / no-quorum error paths returned holding g_repl_mu, leaking the
 * non-recursive mutex and deadlocking the next flush. */
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
    /* Packed under the table lock from the live export (no full-table copy). */
    char *packed;
    size_t packed_len;
    uint32_t packed_ino_len, packed_ch_len;
    uint8_t *ino_dirty_pg, *ch_dirty_pg;
    int packed_is_cache;
};

static void dirty_snap_save_locked(struct dirty_snap *ds)
{
    ds->ino_keys = g_client.dirty_ino_keys;
    ds->ino_mask = g_client.dirty_ino_mask;
    ds->ino_count = g_client.dirty_ino_count;
    ds->chunk_keys = g_client.dirty_chunk_keys;
    ds->chunk_mask = g_client.dirty_chunk_mask;
    ds->chunk_count = g_client.dirty_chunk_count;
    ds->chunk_inos = g_client.dirty_chunk_inos;
    ds->chunk_idxs = g_client.dirty_chunk_idxs;
    ds->chunk_cap = g_client.dirty_chunk_cap;
    ds->meta_dirty = g_client.meta_dirty;
    ds->dirty_ops = g_client.meta_dirty_ops;
    g_client.dirty_ino_keys = NULL;
    g_client.dirty_ino_mask = 0;
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_keys = NULL;
    g_client.dirty_chunk_mask = 0;
    g_client.dirty_chunk_count = 0;
    g_client.dirty_chunk_inos = NULL;
    g_client.dirty_chunk_idxs = NULL;
    g_client.dirty_chunk_cap = 0;
    g_client.meta_dirty = 0;
    g_client.meta_dirty_ops = 0;
    ds->ino_slots = NULL;
    ds->chunk_slots = NULL;
    ds->layout_epoch = g_client.export.layout_epoch;
    ds->snap_icount = g_client.export.inode_count;
    ds->snap_ccount = g_client.export.chunk_count;
    if (ds->ino_count) {
        ds->ino_slots = malloc(ds->ino_count * sizeof(uint64_t));
        uint64_t n = 0;
        if (ds->ino_slots && ds->ino_keys) {
            for (uint64_t i = 0; i <= ds->ino_mask && n < ds->ino_count; i++) {
                if (!ds->ino_keys[i])
                    continue;
                uint64_t slot = UINT64_MAX;
                (void)efs_export_inode_slot(&g_client.export, ds->ino_keys[i],
                                            &slot);
                ds->ino_slots[n++] = slot;
            }
        }
    }
    if (ds->chunk_count) {
        ds->chunk_slots = malloc(ds->chunk_count * sizeof(uint64_t));
        if (ds->chunk_slots) {
            for (uint64_t i = 0; i < ds->chunk_count; i++) {
                uint64_t slot = UINT64_MAX;
                (void)efs_export_chunk_slot(&g_client.export, ds->chunk_inos[i],
                                            ds->chunk_idxs[i], &slot);
                ds->chunk_slots[i] = slot;
            }
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
 * retries it. g_client.lock must be held. */
static void dirty_snap_merge_back_locked(struct dirty_snap *ds)
{
    for (uint64_t i = 0; i <= ds->ino_mask; i++) {
        if (ds->ino_keys && ds->ino_keys[i])
            efs_client_mark_ino_dirty(ds->ino_keys[i]);
    }
    for (uint64_t i = 0; i < ds->chunk_count; i++)
        efs_client_mark_chunk_dirty(ds->chunk_inos[i], ds->chunk_idxs[i]);
    g_client.meta_dirty |= ds->meta_dirty;
    g_client.meta_dirty_ops += ds->dirty_ops;
    dirty_snap_free(ds);
}

/* Serialize the snapshot and publish a new metadata generation: EC-encode
 * each 128 KiB page, skip PUTs for pages identical to the last committed
 * same-parity flush, PUT the rest, then flip the root. Does NOT hold
 * g_client.lock and does NOT touch dirty state. On success, *out_hashes /
 * *out_sums / *out_pages hand the caller (still holding nothing) the parity
 * slot state to commit under the lock; on failure all three are NULL/0. */
static const uint8_t *region_page_ptr(const char *base, uint32_t blen,
                                      uint32_t pi, uint8_t *pad)
{
    size_t off = (size_t)pi * EFS_META_PAGE_SIZE;
    if (off >= blen) {
        memset(pad, 0, EFS_META_PAGE_SIZE);
        return pad;
    }
    if (off + EFS_META_PAGE_SIZE <= blen)
        return (const uint8_t *)base + off;
    size_t tail = blen - off;
    memcpy(pad, base + off, tail);
    memset(pad + tail, 0, EFS_META_PAGE_SIZE - tail);
    return pad;
}

static uint32_t logical_page(int region, uint32_t pi)
{
    return (region == EFS_META_REGION_CHUNK) ? (EFS_META_CHUNK_PAGE_BASE + pi)
                                             : pi;
}

static int cache_grow(char **p, size_t *cap, size_t need)
{
    if (*cap >= need)
        return 0;
    size_t ncap = *cap ? *cap * 2 : need;
    if (ncap < need)
        ncap = need;
    if (ncap < need + (4u << 20))
        ncap = need + (4u << 20);
    char *n = realloc(*p, ncap);
    if (!n)
        return -1;
    *p = n;
    *cap = ncap;
    return 0;
}

/* Split a leftover combined [ino|chunk] blob (full serialize / adopt). */
static int split_combined_cache(void)
{
    if (g_client.meta_cache_ch || !g_client.meta_cache_ch_len)
        return 0;
    if (g_client.meta_cache_len < (size_t)g_client.meta_cache_ino_len +
                                      g_client.meta_cache_ch_len)
        return -1;
    size_t old_ch = g_client.meta_cache_ch_len;
    if (cache_grow(&g_client.meta_cache_ch, &g_client.meta_cache_ch_cap,
                   old_ch) != 0)
        return -1;
    memcpy(g_client.meta_cache_ch,
           g_client.meta_cache_blob + g_client.meta_cache_ino_len, old_ch);
    return 0;
}

/* A record that straddles a 128 KiB page must dirty both pages. */
#define MARK_SPAN(bm, npc, off0, rec) do {                          \
    uint32_t _a = (uint32_t)(off0) / EFS_META_PAGE_SIZE;            \
    uint32_t _b = (uint32_t)((off0) + (rec) - 1) / EFS_META_PAGE_SIZE; \
    if (_a < (npc)) (bm)[_a] = 1;                                   \
    if (_b < (npc)) (bm)[_b] = 1;                                   \
} while (0)

/* EFSM v6 incremental: patch dirty compact slots + append new dentries.
 * Overwrite/mtime (inode_count unchanged) does not walk names or rewrite
 * the dentry tail. Create memmoves the tail by the compact growth. */
static int incremental_serialize_v6(struct efs_export *snap, struct dirty_snap *ds,
                                    char **blob, size_t *blob_len,
                                    uint32_t *ino_blob_len, uint32_t *chunk_blob_len,
                                    uint8_t **ino_dirty_pages, uint8_t **ch_dirty_pages,
                                    uint32_t *ino_pc_out, uint32_t *ch_pc_out)
{
    size_t dent = (size_t)snap->dentry_bytes;
    size_t compact = (size_t)snap->inode_count * EFS_INODE_COMPACT_SIZE;
    uint64_t total64 = (uint64_t)EFS_META_HDR_SIZE + compact + dent;
    if (total64 > (uint64_t)UINT32_MAX)
        return -1;
    uint32_t ino_len = (uint32_t)total64;
    size_t ch_bytes = (size_t)snap->chunk_count * EFS_CHUNK_WIRE_SIZE;
    size_t total = (size_t)ino_len + ch_bytes;

    uint64_t old_ic = g_client.meta_cache_icount;
    uint64_t old_cc = g_client.meta_cache_ccount;
    size_t old_compact = (size_t)old_ic * EFS_INODE_COMPACT_SIZE;
    size_t old_dent_off = EFS_META_HDR_SIZE + old_compact;
    size_t old_ino_len = g_client.meta_cache_ino_len;
    size_t old_ch_len = g_client.meta_cache_ch_len;
    if (old_ino_len < old_dent_off)
        return -1;
    size_t existing_dent = old_ino_len - old_dent_off;
    if (snap->inode_count < old_ic || snap->chunk_count < old_cc)
        return -1;
    if (snap->inode_count == old_ic && existing_dent != dent)
        return -1;
    if (snap->inode_count > old_ic && existing_dent > dent)
        return -1;

    if (split_combined_cache() != 0)
        return -1;
    if (cache_grow(&g_client.meta_cache_blob, &g_client.meta_cache_cap,
                   ino_len) != 0)
        return -1;
    if (ch_bytes &&
        cache_grow(&g_client.meta_cache_ch, &g_client.meta_cache_ch_cap,
                   ch_bytes) != 0)
        return -1;

    uint32_t ino_pc = efs_meta_page_count_for_blob(ino_len);
    uint32_t ch_pc = efs_meta_page_count_for_blob((uint32_t)ch_bytes);
    uint8_t *idirty = calloc(ino_pc ? ino_pc : 1, 1);
    uint8_t *cdirty = calloc(ch_pc ? ch_pc : 1, 1);
    if (!idirty || !cdirty) {
        free(idirty);
        free(cdirty);
        return -1;
    }

    char *b = g_client.meta_cache_blob;
    char *cb = g_client.meta_cache_ch;
    size_t new_dent_off = EFS_META_HDR_SIZE + compact;
    if (snap->inode_count > old_ic && existing_dent) {
        memmove(b + new_dent_off, b + old_dent_off, existing_dent);
        uint32_t p0 = (uint32_t)(old_dent_off < new_dent_off ? old_dent_off
                                                             : new_dent_off) /
                      EFS_META_PAGE_SIZE;
        uint32_t p1 = (ino_len - 1) / EFS_META_PAGE_SIZE;
        for (uint32_t p = p0; p <= p1 && p < ino_pc; p++)
            idirty[p] = 1;
    }

    efs_export_pack_header(snap, (uint8_t *)b);
    if (ino_pc)
        idirty[0] = 1;

    for (uint64_t i = old_ic; i < snap->inode_count; i++) {
        uint32_t off = (uint32_t)(EFS_META_HDR_SIZE + i * EFS_INODE_COMPACT_SIZE);
        efs_export_pack_inode_compact(&snap->inodes[i], (uint8_t *)b + off);
        MARK_SPAN(idirty, ino_pc, off, EFS_INODE_COMPACT_SIZE);
    }
    if (ds->ino_slots) {
        for (uint64_t i = 0; i < ds->ino_count; i++) {
            uint64_t slot = ds->ino_slots[i];
            if (slot >= snap->inode_count)
                continue;
            uint32_t off = (uint32_t)(EFS_META_HDR_SIZE +
                                      slot * EFS_INODE_COMPACT_SIZE);
            efs_export_pack_inode_compact(&snap->inodes[slot],
                                          (uint8_t *)b + off);
            MARK_SPAN(idirty, ino_pc, off, EFS_INODE_COMPACT_SIZE);
        }
    }

    uint8_t *dp = (uint8_t *)b + new_dent_off + existing_dent;
    for (uint64_t i = old_ic; i < snap->inode_count; i++) {
        uint16_t ln = (uint16_t)strnlen(snap->inodes[i].name, EFS_MAX_NAME - 1);
        uint32_t off = (uint32_t)(dp - (uint8_t *)b);
        memcpy(dp, &ln, 2);
        dp += 2;
        if (ln) {
            memcpy(dp, snap->inodes[i].name, ln);
            dp += ln;
        }
        MARK_SPAN(idirty, ino_pc, off, (uint32_t)(2 + ln));
    }
    if ((size_t)(dp - (uint8_t *)(b + new_dent_off)) != dent) {
        free(idirty);
        free(cdirty);
        return -1;
    }
    if (ino_len > old_ino_len && ino_pc) {
        uint32_t p0 = (uint32_t)old_ino_len / EFS_META_PAGE_SIZE;
        uint32_t p1 = (ino_len - 1) / EFS_META_PAGE_SIZE;
        for (uint32_t p = p0; p <= p1 && p < ino_pc; p++)
            idirty[p] = 1;
    }

    if (ch_bytes > old_ch_len && ch_pc) {
        uint32_t p0 = (uint32_t)old_ch_len / EFS_META_PAGE_SIZE;
        uint32_t p1 = (uint32_t)(ch_bytes - 1) / EFS_META_PAGE_SIZE;
        for (uint32_t p = p0; p <= p1 && p < ch_pc; p++)
            cdirty[p] = 1;
    }
    for (uint64_t i = old_cc; i < snap->chunk_count; i++) {
        uint32_t off = (uint32_t)(i * EFS_CHUNK_WIRE_SIZE);
        efs_export_pack_chunk(&snap->chunks[i], (uint8_t *)cb + off);
        MARK_SPAN(cdirty, ch_pc, off, EFS_CHUNK_WIRE_SIZE);
    }
    if (ds->chunk_slots) {
        for (uint64_t i = 0; i < ds->chunk_count; i++) {
            uint64_t slot = ds->chunk_slots[i];
            if (slot >= snap->chunk_count)
                continue;
            uint32_t off = (uint32_t)(slot * EFS_CHUNK_WIRE_SIZE);
            efs_export_pack_chunk(&snap->chunks[slot], (uint8_t *)cb + off);
            MARK_SPAN(cdirty, ch_pc, off, EFS_CHUNK_WIRE_SIZE);
        }
    }

    g_client.meta_cache_ino_len = ino_len;
    g_client.meta_cache_ch_len = (uint32_t)ch_bytes;
    g_client.meta_cache_len = total;
    g_client.meta_cache_icount = snap->inode_count;
    g_client.meta_cache_ccount = snap->chunk_count;
    *blob = b;
    *blob_len = total;
    *ino_blob_len = ino_len;
    *chunk_blob_len = (uint32_t)ch_bytes;
    *ino_dirty_pages = idirty;
    *ch_dirty_pages = cdirty;
    if (ino_pc_out)
        *ino_pc_out = ino_pc;
    if (ch_pc_out)
        *ch_pc_out = ch_pc;
    return 0;
}

/* Patch dirty/appended rows into the cached EFSM regions in place.
 * Caller holds the table lock and passes the live export. Returns 0 and
 * fills *blob (the ino-region cache pointer — do not free). -1 = full serialize. */
static int incremental_serialize(struct efs_export *snap, struct dirty_snap *ds,
                                 char **blob, size_t *blob_len,
                                 uint32_t *ino_blob_len, uint32_t *chunk_blob_len,
                                 uint8_t **ino_dirty_pages, uint8_t **ch_dirty_pages,
                                 uint32_t *ino_pc_out, uint32_t *ch_pc_out)
{
    if (!g_client.meta_cache_blob || !ds || !snap || !snap->inodes)
        return -1;
    if (ds->layout_epoch != g_client.meta_cache_epoch)
        return -1;
    if (snap->inode_count < g_client.meta_cache_icount ||
        snap->chunk_count < g_client.meta_cache_ccount)
        return -1;
    if (!ds->ino_slots && ds->ino_count)
        return -1;

    uint32_t cache_ver = EFS_META_EFSM_V5;
    if (g_client.meta_cache_blob && g_client.meta_cache_len >= 8)
        memcpy(&cache_ver, g_client.meta_cache_blob + 4, 4);
    if (cache_ver >= EFS_META_EFSM_V6)
        return incremental_serialize_v6(snap, ds, blob, blob_len, ino_blob_len,
                                        chunk_blob_len, ino_dirty_pages,
                                        ch_dirty_pages, ino_pc_out, ch_pc_out);

    size_t ino_bytes = (size_t)snap->inode_count * EFS_INODE_WIRE_SIZE;
    size_t ch_bytes = (size_t)snap->chunk_count * EFS_CHUNK_WIRE_SIZE;
    uint32_t ino_len = (uint32_t)(EFS_META_HDR_SIZE + ino_bytes);
    size_t total = (size_t)ino_len + ch_bytes;

    if (split_combined_cache() != 0)
        return -1;

    size_t old_ino_len = g_client.meta_cache_ino_len
                             ? g_client.meta_cache_ino_len
                             : g_client.meta_cache_len;
    size_t old_ch_len = g_client.meta_cache_ch_len;
    if (cache_grow(&g_client.meta_cache_blob, &g_client.meta_cache_cap,
                   ino_len) != 0)
        return -1;
    if (ino_len > old_ino_len)
        memset(g_client.meta_cache_blob + old_ino_len, 0, ino_len - old_ino_len);
    if (ch_bytes) {
        size_t old_ch = g_client.meta_cache_ch_len;
        if (cache_grow(&g_client.meta_cache_ch, &g_client.meta_cache_ch_cap,
                       ch_bytes) != 0)
            return -1;
        if (ch_bytes > old_ch)
            memset(g_client.meta_cache_ch + old_ch, 0, ch_bytes - old_ch);
    }

    char *b = g_client.meta_cache_blob;
    char *cb = g_client.meta_cache_ch;
    efs_export_pack_header_ver(snap, (uint8_t *)b, EFS_META_EFSM_V5);
    uint32_t ino_pc = efs_meta_page_count_for_blob(ino_len);
    uint32_t ch_pc = efs_meta_page_count_for_blob((uint32_t)ch_bytes);
    uint8_t *idirty = calloc(ino_pc ? ino_pc : 1, 1);
    uint8_t *cdirty = calloc(ch_pc ? ch_pc : 1, 1);
    if (!idirty || !cdirty) {
        free(idirty);
        free(cdirty);
        return -1;
    }
    if (ino_pc)
        idirty[0] = 1; /* header */

    /* New bytes at the end of a region (including a newly created last page
     * that only holds the tail of a spanning record) must be flushed. */
    if (ino_len > old_ino_len && ino_pc) {
        uint32_t p0 = (uint32_t)old_ino_len / EFS_META_PAGE_SIZE;
        uint32_t p1 = (ino_len - 1) / EFS_META_PAGE_SIZE;
        for (uint32_t p = p0; p <= p1 && p < ino_pc; p++)
            idirty[p] = 1;
    }
    if (ch_bytes > old_ch_len && ch_pc) {
        uint32_t p0 = (uint32_t)old_ch_len / EFS_META_PAGE_SIZE;
        uint32_t p1 = (uint32_t)(ch_bytes - 1) / EFS_META_PAGE_SIZE;
        for (uint32_t p = p0; p <= p1 && p < ch_pc; p++)
            cdirty[p] = 1;
    }

    uint64_t old_ic = g_client.meta_cache_icount;
    for (uint64_t i = old_ic; i < snap->inode_count; i++) {
        efs_export_pack_inode(&snap->inodes[i],
                              (uint8_t *)b + EFS_META_HDR_SIZE +
                                  i * EFS_INODE_WIRE_SIZE);
        uint32_t off = (uint32_t)(EFS_META_HDR_SIZE + i * EFS_INODE_WIRE_SIZE);
        MARK_SPAN(idirty, ino_pc, off, EFS_INODE_WIRE_SIZE);
    }
    if (ds->ino_slots) {
        for (uint64_t i = 0; i < ds->ino_count; i++) {
            uint64_t slot = ds->ino_slots[i];
            if (slot >= snap->inode_count)
                continue;
            efs_export_pack_inode(&snap->inodes[slot],
                                  (uint8_t *)b + EFS_META_HDR_SIZE +
                                      slot * EFS_INODE_WIRE_SIZE);
            uint32_t off = (uint32_t)(EFS_META_HDR_SIZE +
                                      slot * EFS_INODE_WIRE_SIZE);
            MARK_SPAN(idirty, ino_pc, off, EFS_INODE_WIRE_SIZE);
        }
    }
    uint64_t old_cc = g_client.meta_cache_ccount;
    for (uint64_t i = old_cc; i < snap->chunk_count; i++) {
        efs_export_pack_chunk(&snap->chunks[i],
                              (uint8_t *)cb + i * EFS_CHUNK_WIRE_SIZE);
        uint32_t off = (uint32_t)(i * EFS_CHUNK_WIRE_SIZE);
        MARK_SPAN(cdirty, ch_pc, off, EFS_CHUNK_WIRE_SIZE);
    }
    if (ds->chunk_slots) {
        for (uint64_t i = 0; i < ds->chunk_count; i++) {
            uint64_t slot = ds->chunk_slots[i];
            if (slot >= snap->chunk_count)
                continue;
            efs_export_pack_chunk(&snap->chunks[slot],
                                  (uint8_t *)cb + slot * EFS_CHUNK_WIRE_SIZE);
            uint32_t off = (uint32_t)(slot * EFS_CHUNK_WIRE_SIZE);
            MARK_SPAN(cdirty, ch_pc, off, EFS_CHUNK_WIRE_SIZE);
        }
    }
    #undef MARK_SPAN

    *blob = b;
    *blob_len = total;
    *ino_blob_len = ino_len;
    *chunk_blob_len = (uint32_t)ch_bytes;
    *ino_dirty_pages = idirty;
    *ch_dirty_pages = cdirty;
    if (ino_pc_out)
        *ino_pc_out = ino_pc;
    if (ch_pc_out)
        *ch_pc_out = ch_pc;
    return 0;
}

static int flush_snapshot(struct efs_export *snap, uint64_t new_gen,
                          uint32_t committed_ino_pc, uint32_t committed_ch_pc,
                          uint8_t **out_hashes, uint8_t **out_sums,
                          uint32_t *out_pages, struct efs_export_root *out_root,
                          struct dirty_snap *ds, int full, int heal)
{
    char *blob = NULL;
    size_t blob_len = 0;
    uint32_t ino_blob_len = 0, chunk_blob_len = 0;
    uint8_t *new_hashes = NULL; /* packed [ino pages | chunk pages] */
    uint8_t *new_sums = NULL;
    uint32_t page_put = 0, page_skip = 0;
    struct efs_export_root root;

    *out_hashes = NULL;
    *out_sums = NULL;
    *out_pages = 0;
    memset(out_root, 0, sizeof(*out_root));
    memset(&root, 0, sizeof(root));

    uint8_t *ino_dirty_pg = NULL, *ch_dirty_pg = NULL;
    int incremental = 0;
    int blob_is_cache = 0;
    if (ds && ds->packed) {
        blob = ds->packed;
        blob_len = ds->packed_len;
        ino_blob_len = ds->packed_ino_len;
        chunk_blob_len = ds->packed_ch_len;
        ino_dirty_pg = ds->ino_dirty_pg;
        ch_dirty_pg = ds->ch_dirty_pg;
        blob_is_cache = ds->packed_is_cache;
        ds->packed = NULL;
        ds->ino_dirty_pg = NULL;
        ds->ch_dirty_pg = NULL;
        incremental = 1;
    } else if (!full && ds && (ds->ino_count || ds->chunk_count) &&
        incremental_serialize(snap, ds, &blob, &blob_len, &ino_blob_len,
                              &chunk_blob_len, &ino_dirty_pg, &ch_dirty_pg,
                              NULL, NULL) == 0) {
        incremental = 1;
        blob_is_cache = 1;
    } else if (efs_export_serialize_ex(snap, &blob, &blob_len, &ino_blob_len,
                                       &chunk_blob_len) != EFS_OK) {
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        return EFS_ERR_NOMEM;
    }
    if (!blob)
        return EFS_ERR_NOMEM;
    if (efs_meta_page_count_for_blob(ino_blob_len) > EFS_META_INO_PAGE_MAX ||
        efs_meta_page_count_for_blob(chunk_blob_len) > EFS_META_CHUNK_PAGE_MAX) {
        if (!g_client.meta_cap_blocked) {
            fprintf(stderr,
                    "meta replicate: region pages exceed cap (ino %u/%u chunk %u/%u)\n",
                    efs_meta_page_count_for_blob(ino_blob_len),
                    (unsigned)EFS_META_INO_PAGE_MAX,
                    efs_meta_page_count_for_blob(chunk_blob_len),
                    (unsigned)EFS_META_CHUNK_PAGE_MAX);
            fflush(stderr);
            g_client.meta_cap_blocked = 1;
        }
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
        return EFS_ERR_QUOTA;
    }

    /* 2+1 EC needs 3 distinct nodes; on a 1-2 node ring placement wraps and
     * two fragments land on one machine, so a "quorum" of 2 acks can be a
     * single disk. Refuse to publish data metadata we cannot durably store. */
    pthread_mutex_lock(&g_client.lock);
    uint32_t live_nodes = g_client.node_count;
    pthread_mutex_unlock(&g_client.lock);
    if (live_nodes < EFS_NUM_FRAGMENTS) {
        fprintf(stderr,
                "meta replicate: only %u live node(s); 2+1 EC needs %u distinct "
                "nodes — refusing to commit gen %llu\n",
                live_nodes, (unsigned)EFS_NUM_FRAGMENTS,
                (unsigned long long)new_gen);
        fflush(stderr);
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
        return EFS_ERR_NO_QUORUM;
    }

    int prc = efs_export_root_prepare(&root, snap, new_gen, ino_blob_len,
                                      chunk_blob_len);
    if (prc != EFS_OK) {
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
        return prc;
    }

    uint8_t *pad = malloc(EFS_META_PAGE_SIZE);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    new_hashes = malloc((size_t)root.page_count * EFS_HASH_SIZE);
    new_sums = malloc((size_t)root.page_count * EFS_NUM_FRAGMENTS *
                      EFS_HASH_SIZE);
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (!pad || !frag_buf || !new_hashes || !new_sums) {
        free(pad);
        free(frag_buf);
        free(new_hashes);
        free(new_sums);
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
        efs_export_root_free(&root);
        return EFS_ERR_NOMEM;
    }
    frag_ptrs(frag_buf, EFS_META_FRAGMENT_SIZE, frags);
    uint32_t abort_after = UINT32_MAX;
    {
        const char *env = getenv("EFS_META_FLUSH_ABORT_AFTER_PAGES");
        if (env && *env) {
            char *end = NULL;
            unsigned long v = strtoul(env, &end, 10);
            if (end != env && v < UINT32_MAX)
                abort_after = (uint32_t)v;
        }
    }
    uint32_t parity = (uint32_t)(new_gen & 1ULL);

    struct meta_dirty {
        uint32_t packed;
        uint32_t ci;
        int region;
        uint32_t pi;
        const char *base;
        uint32_t blen;
    };
    struct meta_dirty *dirty = calloc(root.page_count ? root.page_count : 1,
                                      sizeof(*dirty));
    if (!dirty) {
        free(pad);
        free(frag_buf);
        free(new_hashes);
        free(new_sums);
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
        efs_export_root_free(&root);
        return EFS_ERR_NOMEM;
    }
    uint32_t ndirty = 0;

    /* Pass 1: hash every region page; skip those already on disk. */
    for (int region = 0; region < 2; region++) {
        uint32_t npc = (region == EFS_META_REGION_INO) ? root.ino_page_count
                                                       : root.chunk_page_count;
        uint32_t blen = (region == EFS_META_REGION_INO) ? ino_blob_len
                                                        : chunk_blob_len;
        const char *base = (region == EFS_META_REGION_INO)
                               ? blob
                               : ((incremental && g_client.meta_cache_ch)
                                      ? g_client.meta_cache_ch
                                      : blob + ino_blob_len);
        uint32_t packed_base = (region == EFS_META_REGION_INO)
                                   ? 0
                                   : root.ino_page_count;
        uint32_t last_npc = (region == EFS_META_REGION_INO)
                                ? g_client.meta_slot_ino_pages[parity]
                                : g_client.meta_slot_chunk_pages[parity];
        uint32_t committed_npc = (region == EFS_META_REGION_INO)
                                     ? committed_ino_pc
                                     : committed_ch_pc;
        for (uint32_t pi = 0; pi < npc; pi++) {
            uint32_t packed = packed_base + pi;
            uint32_t logi = logical_page(region, pi);
            int row_dirty = 1;
            if (incremental) {
                uint8_t *bm = (region == EFS_META_REGION_INO) ? ino_dirty_pg
                                                              : ch_dirty_pg;
                row_dirty = bm && bm[pi];
            }
            if (heal && region == EFS_META_REGION_CHUNK)
                row_dirty = 1;
            /* Clean incremental page: do not hash or PUT. The other dual
             * slot already holds this content from the last same-parity
             * gen; reuse the committed root checksums. After remount the
             * skip tables are empty, and hashing/PUTing all 11k pages
             * wedged the first fsync for minutes. */
            if (incremental && !row_dirty &&
                g_client.export.root.page_checksums &&
                packed < g_client.export.root.page_count) {
                memcpy(new_sums + (size_t)packed * EFS_NUM_FRAGMENTS *
                           EFS_HASH_SIZE,
                       efs_export_root_checksum_const(&g_client.export.root,
                                                      packed, 0),
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
                memcpy(efs_export_root_checksum(&root, packed, 0),
                       new_sums + (size_t)packed * EFS_NUM_FRAGMENTS *
                           EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
                page_skip++;
                continue;
            }
            if (!row_dirty && pi < last_npc && pi < committed_npc &&
                g_client.meta_slot_hashes[parity] &&
                logi < EFS_META_MAX_PAGES) {
                memcpy(new_hashes + (size_t)packed * EFS_HASH_SIZE,
                       g_client.meta_slot_hashes[parity] +
                           (size_t)logi * EFS_HASH_SIZE,
                       EFS_HASH_SIZE);
                memcpy(new_sums + (size_t)packed * EFS_NUM_FRAGMENTS *
                           EFS_HASH_SIZE,
                       g_client.meta_slot_sums[parity] +
                           (size_t)logi * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
                memcpy(efs_export_root_checksum(&root, packed, 0),
                       new_sums + (size_t)packed * EFS_NUM_FRAGMENTS *
                           EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
                page_skip++;
                continue;
            }
            const uint8_t *pg = region_page_ptr(base, blen, pi, pad);
            efs_hash(pg, EFS_META_PAGE_SIZE,
                     new_hashes + (size_t)packed * EFS_HASH_SIZE);
            int can_skip = 0;
            if (pi < last_npc && pi < committed_npc &&
                g_client.meta_slot_hashes[parity] &&
                logi < EFS_META_MAX_PAGES &&
                memcmp(new_hashes + (size_t)packed * EFS_HASH_SIZE,
                       g_client.meta_slot_hashes[parity] +
                           (size_t)logi * EFS_HASH_SIZE,
                       EFS_HASH_SIZE) == 0) {
                memcpy(new_sums + (size_t)packed * EFS_NUM_FRAGMENTS *
                           EFS_HASH_SIZE,
                       g_client.meta_slot_sums[parity] +
                           (size_t)logi * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
                memcpy(efs_export_root_checksum(&root, packed, 0),
                       new_sums + (size_t)packed * EFS_NUM_FRAGMENTS *
                           EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
                page_skip++;
                can_skip = 1;
            }
            if (can_skip)
                continue;
            uint32_t ci = efs_meta_region_page_chunk_index(new_gen, region, pi);
            if (ci == UINT32_MAX) {
                free(dirty);
                free(pad);
                free(frag_buf);
                free(new_hashes);
                free(new_sums);
                free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
                efs_export_root_free(&root);
                return EFS_ERR_INVAL;
            }
            dirty[ndirty].packed = packed;
            dirty[ndirty].ci = ci;
            dirty[ndirty].region = region;
            dirty[ndirty].pi = pi;
            dirty[ndirty].base = base;
            dirty[ndirty].blen = blen;
            ndirty++;
        }
    }

    /* Win the cluster-wide flush election before PUTting a single page:
     * concurrent writers' page PUTs share dual-slot CIs and tear each
     * other's pages. STALE = our gen is already behind (caller resyncs);
     * BUSY = another writer is mid-flush (caller retries with backoff). */
    int brc = send_meta_begin(new_gen, g_client.write_lease_id);
    if (brc != EFS_OK) {
        free(dirty);
        free(pad);
        free(frag_buf);
        free(new_hashes);
        free(new_sums);
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache)
            free(blob);
        efs_export_root_free(&root);
        return brc;
    }
    /* The election lease (EFS_META_WRITER_EXPIRY_MS, 60 s) is shorter than
     * a big flush: thousands of dirty pages under load take minutes. If the
     * lease lapsed mid-flush, another writer won the election and PUT the
     * same-parity pages concurrently — the very tear the election exists
     * to prevent — and our root commit was rejected STALE, wedging us into
     * the resync loop. Renew every 15 s; a failed renewal aborts the flush
     * before our PUTs can overlap another holder's. */
    struct timespec rnw;
    clock_gettime(CLOCK_MONOTONIC, &rnw);
    int64_t last_renew_ms = (int64_t)rnw.tv_sec * 1000 +
                            (int64_t)rnw.tv_nsec / 1000000;

    /* Pass 2: encode + PUT dirty pages, pipelined. */
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;
    for (uint32_t base = 0; base < ndirty; ) {
        clock_gettime(CLOCK_MONOTONIC, &rnw);
        int64_t now_ms = (int64_t)rnw.tv_sec * 1000 +
                         (int64_t)rnw.tv_nsec / 1000000;
        if (now_ms - last_renew_ms > 15000) {
            brc = send_meta_begin(new_gen, g_client.write_lease_id);
            if (brc != EFS_OK) {
                free(dirty);
                free(pad);
                free(frag_buf);
                free(new_hashes);
                free(new_sums);
                free(ino_dirty_pg);
                free(ch_dirty_pg);
                if (!blob_is_cache)
                    free(blob);
                efs_export_root_free(&root);
                return brc;
            }
            last_renew_ms = now_ms;
        }
        uint32_t batch = ndirty - base;
        if (batch > pipe)
            batch = pipe;
        if (abort_after != UINT32_MAX && page_put + batch > abort_after)
            batch = abort_after - page_put;
        for (uint32_t i = 0; i < batch; i++) {
            struct meta_dirty *d = &dirty[base + i];
            const uint8_t *pg = region_page_ptr(d->base, d->blen, d->pi, pad);
            efs_encode_chunk(pg, EFS_META_PAGE_SIZE, EFS_META_PAGE_SIZE,
                             frags);
            uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
            for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
                efs_hash(frags[fi], EFS_META_FRAGMENT_SIZE, checksums[fi]);
            efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
            efs_place_fragments(g_client.nodes, g_client.node_count,
                                EFS_META_TABLE_INO, d->ci, nodes);
            const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
            for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
                cfrags[fi] = frags[fi];
            int rc = efs_client_put_fragments_parallel(EFS_META_TABLE_INO, d->ci,
                                                       nodes, cfrags,
                                                       EFS_META_FRAGMENT_SIZE,
                                                       checksums);
            if (rc != EFS_OK) {
                free(dirty);
                free(pad);
                free(frag_buf);
                free(new_hashes);
                free(new_sums);
                free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
                efs_export_root_free(&root);
                return rc;
            }
            page_put++;
            if (heal && (page_put % 500u) == 0) {
                fprintf(stderr, "meta: heal PUT %u/%u chunk pages\n",
                        page_put, ndirty);
                fflush(stderr);
            }
            for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
                memcpy(efs_export_root_checksum(&root, d->packed, fi),
                       checksums[fi], EFS_HASH_SIZE);
            memcpy(new_sums + (size_t)d->packed * EFS_NUM_FRAGMENTS *
                       EFS_HASH_SIZE,
                   checksums, EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
        }
        if (abort_after != UINT32_MAX && page_put >= abort_after) {
            fprintf(stderr,
                    "meta replicate: abort after %u page PUT(s) "
                    "(EFS_META_FLUSH_ABORT_AFTER_PAGES); root not flipped\n",
                    abort_after);
            fflush(stderr);
            free(dirty);
            free(pad);
            free(frag_buf);
            free(new_hashes);
            free(new_sums);
            free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
            efs_export_root_free(&root);
            return EFS_ERR_IO;
        }
        base += batch;
    }
    free(dirty);
    free(pad);
    free(frag_buf);
    free(ino_dirty_pg);
    free(ch_dirty_pg);
    ino_dirty_pg = NULL;
    ch_dirty_pg = NULL;
    /* Keep this generation's blob as the incremental cache. */
    if (!blob_is_cache) {
        free(g_client.meta_cache_blob);
        free(g_client.meta_cache_ch);
        g_client.meta_cache_ch = NULL;
        g_client.meta_cache_ch_cap = 0;
        g_client.meta_cache_blob = blob;
        g_client.meta_cache_cap = blob_len;
    }
    g_client.meta_cache_len = blob_len;
    g_client.meta_cache_ino_len = ino_blob_len;
    g_client.meta_cache_ch_len = chunk_blob_len;
    g_client.meta_cache_icount = snap->inode_count;
    g_client.meta_cache_ccount = snap->chunk_count;
    g_client.meta_cache_epoch = snap->layout_epoch;
    blob = NULL;

    char *root_buf = NULL;
    size_t root_len = 0;
    if (efs_export_root_serialize(&root, &root_buf, &root_len) != EFS_OK) {
        free(new_hashes);
        free(new_sums);
        efs_export_root_free(&root);
        return EFS_ERR_NOMEM;
    }

    int rc = send_meta_root(root_buf, root_len);
    free(root_buf);
    if (rc != EFS_OK) {
        free(new_hashes);
        free(new_sums);
        efs_export_root_free(&root);
        return rc;
    }
    if (page_skip) {
        fprintf(stderr,
                "meta flush gen %llu: %u pages PUT, %u unchanged (skipped)\n",
                (unsigned long long)new_gen, page_put, page_skip);
        fflush(stderr);
    }
    *out_hashes = new_hashes;
    *out_sums = new_sums;
    *out_pages = root.page_count;
    *out_root = root; /* ownership moves to caller */
    return EFS_OK;
}

static int efs_client_replicate_metadata_locked(void)
{
    /* Point-in-time snapshot of the mutable table, so the O(table) serialize
     * runs WITHOUT holding g_client.lock (previously every FUSE op stalled
     * for the whole serialize of a large table). Arrays are flat (inline
     * names), so a memcpy copy is a consistent snapshot. */
    struct efs_export snap;
    struct dirty_snap ds;
    struct efs_export_root new_root;
    uint64_t new_gen = 1;
    /* Page count of the currently committed root (gen N): a page beyond it
     * may have been reclaimed by the server GC, so it must be re-PUT. */
    uint32_t committed_ino_pc = 0, committed_ch_pc = 0;
    uint8_t *hashes = NULL, *sums = NULL;
    uint32_t pages = 0;

    memset(&snap, 0, sizeof(snap));
    memset(&ds, 0, sizeof(ds));
    memset(&new_root, 0, sizeof(new_root));

    efs_client_table_lock();
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    int heal = g_client.meta_heal;
    if (g_client.meta_batch &&
        g_client.dirty_ino_count == 0 && g_client.dirty_chunk_count == 0 &&
        !g_client.meta_dirty && !heal) {
        g_client.meta_dirty_ops = 0;
        pthread_mutex_unlock(&g_client.dirty_mu);
        efs_client_table_unlock();
        return EFS_OK;
    }
    struct efs_export *ex = &g_client.export;
    efs_export_ensure_rollups(ex);
    snap.id = ex->id;
    strncpy(snap.name, ex->name, EFS_MAX_NAME - 1);
    snap.chunk_size = ex->chunk_size;
    snap.features = ex->features;
    snap.next_ino = ex->next_ino;
    snap.inode_count = ex->inode_count;
    snap.chunk_count = ex->chunk_count;
    snap.layout_epoch = ex->layout_epoch;
    snap.dentry_bytes = ex->dentry_bytes;
    snap.efsm_version = ex->efsm_version;
    new_gen = ex->root.generation + (heal ? 2ULL : 1ULL);
    if (new_gen == 0)
        new_gen = 1;
    if (heal) {
        fprintf(stderr,
                "meta: heal rewriting chunk-table pages to canonical CIs "
                "(gen %llu -> %llu; %u skipped page(s) published as zeros)\n",
                (unsigned long long)ex->root.generation,
                (unsigned long long)new_gen, g_client.meta_heal_skipped);
        fflush(stderr);
    }
    committed_ino_pc = ex->root.ino_page_count ? ex->root.ino_page_count
                                               : ex->root.page_count;
    committed_ch_pc = ex->root.chunk_page_count;
    if (committed_ino_pc > EFS_META_INO_PAGE_MAX)
        committed_ino_pc = EFS_META_INO_PAGE_MAX;
    if (committed_ch_pc > EFS_META_CHUNK_PAGE_MAX)
        committed_ch_pc = EFS_META_CHUNK_PAGE_MAX;
    /* Swap the dirty state out: ops that race the unlocked serialize in
     * flush_snapshot() populate fresh sets and stay dirty for the next
     * flush instead of having their marks wiped by a stale clear. */
    dirty_snap_save_locked(&ds);
    int full = g_client.meta_flush_force;
    g_client.meta_flush_force = 0;
    /* EFSR v4 → v5 moves the dual-slot window; skip tables are stale. */
    if (ex->root.version < 5)
        full = 1;

    /* Incremental: pack dirty rows into the cached blob under the lock.
     * Avoids memcpy of the whole inode table (~0.5–1 GiB) every batch. */
    int packed = 0;
    if (!full && g_client.meta_cache_blob &&
        ds.layout_epoch == g_client.meta_cache_epoch &&
        ex->inode_count >= g_client.meta_cache_icount &&
        ex->chunk_count >= g_client.meta_cache_ccount) {
        if (ds.ino_count || ds.chunk_count) {
            char *pb = NULL;
            size_t plen = 0;
            uint32_t pino = 0, pch = 0;
            uint8_t *pid = NULL, *pcd = NULL;
            if (incremental_serialize(ex, &ds, &pb, &plen, &pino, &pch,
                                      &pid, &pcd, NULL, NULL) == 0) {
                ds.packed = pb;
                ds.packed_len = plen;
                ds.packed_ino_len = pino;
                ds.packed_ch_len = pch;
                ds.ino_dirty_pg = pid;
                ds.ch_dirty_pg = pcd;
                ds.packed_is_cache = 1;
                packed = 1;
            }
        } else if (ds.meta_dirty && !heal) {
            /* Nothing row-dirty: do not bump a generation. */
            dirty_snap_free(&ds);
            pthread_mutex_unlock(&g_client.dirty_mu);
            efs_client_table_unlock();
            return EFS_OK;
        }
    }
    if (heal && packed && ds.ch_dirty_pg) {
        uint32_t ch_pc = efs_meta_page_count_for_blob(ds.packed_ch_len);
        if (ch_pc)
            memset(ds.ch_dirty_pg, 1, ch_pc);
    } else if (heal && !packed && g_client.meta_cache_blob &&
               ds.layout_epoch == g_client.meta_cache_epoch) {
        uint32_t ino_len = g_client.meta_cache_ino_len;
        uint32_t ch_len = g_client.meta_cache_ch_len;
        uint32_t ino_pc = efs_meta_page_count_for_blob(ino_len);
        uint32_t ch_pc = efs_meta_page_count_for_blob(ch_len);
        uint8_t *idirty = calloc(ino_pc ? ino_pc : 1, 1);
        uint8_t *cdirty = calloc(ch_pc ? ch_pc : 1, 1);
        if (idirty && cdirty) {
            if (ch_pc)
                memset(cdirty, 1, ch_pc);
            ds.packed = g_client.meta_cache_blob;
            ds.packed_len = g_client.meta_cache_len;
            ds.packed_ino_len = ino_len;
            ds.packed_ch_len = ch_len;
            ds.ino_dirty_pg = idirty;
            ds.ch_dirty_pg = cdirty;
            ds.packed_is_cache = 1;
            packed = 1;
        } else {
            free(idirty);
            free(cdirty);
        }
    }
    if (!packed) {
        /* Incremental pack can miss (adopt split, epoch, dentry drift).
         * Fall back to a full snapshot instead of failing fsync with INVAL. */
        if (!full && !heal)
            full = 1;
        if (snap.inode_count) {
            snap.inodes = malloc(snap.inode_count * sizeof(*snap.inodes));
            if (snap.inodes)
                memcpy(snap.inodes, ex->inodes,
                       snap.inode_count * sizeof(*snap.inodes));
        }
        if (snap.chunk_count) {
            snap.chunks = malloc(snap.chunk_count * sizeof(*snap.chunks));
            if (snap.chunks)
                memcpy(snap.chunks, ex->chunks,
                       snap.chunk_count * sizeof(*snap.chunks));
        }
        if ((snap.inode_count && !snap.inodes) ||
            (snap.chunk_count && !snap.chunks)) {
            free(snap.inodes);
            free(snap.chunks);
            snap.inodes = NULL;
            snap.chunks = NULL;
            pthread_mutex_unlock(&g_client.dirty_mu);
            efs_client_table_unlock();
            pthread_mutex_lock(&g_client.lock);
            dirty_snap_merge_back_locked(&ds);
            pthread_mutex_unlock(&g_client.lock);
            return EFS_ERR_NOMEM;
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
    efs_client_table_unlock();

    int rc = flush_snapshot(&snap, new_gen, committed_ino_pc, committed_ch_pc,
                            &hashes, &sums, &pages, &new_root, &ds, full,
                            heal);
    free(snap.inodes);
    free(snap.chunks);

    pthread_mutex_lock(&g_client.lock);
    if (rc == EFS_OK) {
        if (heal)
            g_client.meta_heal = 0;
        g_client.export.meta_fragmented = 1;
        efs_export_root_move(&g_client.export.root, &new_root);
        {
            uint32_t cs = g_client.export.root.chunk_size;
            g_client.export.chunk_size = efs_chunk_size_valid(cs) ? cs
                                                                  : EFS_DEFAULT_CHUNK_SIZE;
        }
        /* Commit this parity's page hashes + fragment checksums so the next
         * flush of the same parity can skip unchanged pages. Only on success:
         * a failed flush leaves the slot state describing the last committed
         * generation. */
        uint32_t parity = (uint32_t)(new_gen & 1ULL);
        size_t hsz = (size_t)EFS_META_MAX_PAGES * EFS_HASH_SIZE;
        size_t ssz = (size_t)EFS_META_MAX_PAGES * EFS_NUM_FRAGMENTS *
                     EFS_HASH_SIZE;
        if (!g_client.meta_slot_hashes[parity]) {
            g_client.meta_slot_hashes[parity] = calloc(1, hsz);
            g_client.meta_slot_sums[parity] = calloc(1, ssz);
        }
        if (g_client.meta_slot_hashes[parity] &&
            g_client.meta_slot_sums[parity] && hashes && sums) {
            uint32_t ino_pc = g_client.export.root.ino_page_count;
            uint32_t ch_pc = g_client.export.root.chunk_page_count;
            for (uint32_t i = 0; i < ino_pc; i++) {
                memcpy(g_client.meta_slot_hashes[parity] +
                           (size_t)i * EFS_HASH_SIZE,
                       hashes + (size_t)i * EFS_HASH_SIZE, EFS_HASH_SIZE);
                memcpy(g_client.meta_slot_sums[parity] +
                           (size_t)i * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                       sums + (size_t)i * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
            }
            for (uint32_t j = 0; j < ch_pc; j++) {
                uint32_t logi = EFS_META_CHUNK_PAGE_BASE + j;
                uint32_t packed = ino_pc + j;
                memcpy(g_client.meta_slot_hashes[parity] +
                           (size_t)logi * EFS_HASH_SIZE,
                       hashes + (size_t)packed * EFS_HASH_SIZE, EFS_HASH_SIZE);
                memcpy(g_client.meta_slot_sums[parity] +
                           (size_t)logi * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                       sums + (size_t)packed * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                       EFS_NUM_FRAGMENTS * EFS_HASH_SIZE);
            }
            g_client.meta_slot_ino_pages[parity] = ino_pc;
            g_client.meta_slot_chunk_pages[parity] = ch_pc;
            g_client.meta_slot_pages[parity] = pages;
        }
        free(hashes);
        free(sums);
        /* Marks that raced the unlocked serialize stay dirty; the swapped-out
         * snapshot state is published now and can be dropped. */
        dirty_snap_free(&ds);
    } else {
        /* Flush failed: re-mark everything the snapshot covered so a later
         * flush retries it instead of silently losing the updates. */
        dirty_snap_merge_back_locked(&ds);
    }
    pthread_mutex_unlock(&g_client.lock);
    return rc;
}

/* Blocking single flush; g_repl_mu serializes concurrent flushes. */
static int efs_client_replicate_metadata_once(void)
{
    pthread_mutex_lock(&g_repl_mu);
    int rc = efs_client_replicate_metadata_locked();
    pthread_mutex_unlock(&g_repl_mu);
    return rc;
}

/* A STALE quorum means a concurrent writer committed a newer generation.
 * Re-fetch the newest root+tables so the next attempt republishes our
 * still-dirty ops on top of it. The fetch REPLACES the local table, so the
 * uncommitted rows the dirty sets point at would be freed out from under
 * them (writes then failed NOT_FOUND and creates vanished). Rebase instead:
 * snapshot every dirty inode/chunk row from the current table, swap tables,
 * then replay the snapshot onto the fetched one — dirty inos absent locally
 * are unlinks and are removed from the fetched table. The whole
 * snapshot+swap+replay runs under table_lock+idx_mu+dirty_mu so op-path
 * workers can't land changes in the doomed old table mid-resync. */
static int efs_client_meta_resync(void)
{
    if (g_client.node_count == 0)
        return EFS_ERR_NET;
    fprintf(stderr, "meta: resync after STALE quorum (local gen %llu)\n",
            (unsigned long long)g_client.export.root.generation);
    fflush(stderr);

    /* Serialize against flushes: an in-flight flush holds the dirty sets
     * swapped out (dirty_snap), so snapshotting without g_repl_mu could
     * capture incomplete dirty state — the later merge-back would then
     * reference rows our table swap already freed (lost creates → ENOENT).
     * Callers invoke resync AFTER releasing g_repl_mu, so no self-deadlock. */
    pthread_mutex_lock(&g_repl_mu);

    /* Network phase without the table locks: ops keep running and land in
     * the current table, which the locked phase below snapshots before
     * swapping. g_repl_mu keeps the dirty sets complete meanwhile. */
    struct efs_meta_fetch f;
    int frc = efs_client_fetch_meta_best(g_client.nodes[0].addr,
                                         g_client.nodes[0].port, &f);
    if (frc != EFS_OK || !f.efsm) {
        fprintf(stderr,
                "meta: resync fetch unusable frc=%d have_root=%d efsm=%p "
                "root_gen=%llu\n",
                frc, f.have_root, (void *)f.efsm,
                f.have_root ? (unsigned long long)f.root.generation : 0ull);
        fflush(stderr);
        pthread_mutex_unlock(&g_repl_mu);
        free(f.efsm);
        efs_export_root_free(&f.root);
        return frc != EFS_OK ? frc : EFS_ERR_NET;
    }

    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    pthread_mutex_lock(&g_client.dirty_mu);

    uint64_t cap_ino = g_client.dirty_ino_count;
    uint64_t cap_ch = g_client.dirty_chunk_count;
    struct efs_inode *ino_recs = malloc((cap_ino ? cap_ino : 1) *
                                        sizeof(*ino_recs));
    uint64_t *del_inos = malloc((cap_ino ? cap_ino : 1) * sizeof(*del_inos));
    struct efs_chunk_entry *ch_recs = malloc((cap_ch ? cap_ch : 1) *
                                             sizeof(*ch_recs));
    if (!ino_recs || !del_inos || !ch_recs) {
        free(ino_recs);
        free(del_inos);
        free(ch_recs);
        pthread_mutex_unlock(&g_client.dirty_mu);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_table_unlock();
        pthread_mutex_unlock(&g_repl_mu);
        free(f.efsm);
        efs_export_root_free(&f.root);
        return EFS_ERR_NOMEM;
    }

    uint64_t nrec = 0, ndel = 0, nch = 0;
    if (g_client.dirty_ino_keys) {
        for (uint64_t i = 0; i <= g_client.dirty_ino_mask; i++) {
            uint64_t ino = g_client.dirty_ino_keys[i];
            if (!ino)
                continue;
            struct efs_inode rec;
            if (efs_export_get_inode(&g_client.export, ino, &rec) == 0)
                ino_recs[nrec++] = rec;
            else
                del_inos[ndel++] = ino; /* dirty but gone: unlinked */
        }
    }
    for (uint64_t i = 0; i < g_client.dirty_chunk_count; i++) {
        struct efs_chunk_entry ce;
        if (efs_export_get_chunk(&g_client.export, g_client.dirty_chunk_inos[i],
                                 g_client.dirty_chunk_idxs[i], &ce) == 0)
            ch_recs[nch++] = ce;
        /* Absent locally = truncated away; the inode upsert below drops the
         * fetched table's beyond-size suffix for every dirty regular file. */
    }
    uint64_t old_next_ino = g_client.export.next_ino;

    int rc = efs_export_deserialize(&g_client.export, f.efsm, f.efsm_len);
    if (rc != EFS_OK) {
        fprintf(stderr, "meta: resync deserialize failed rc=%d len=%zu\n",
                rc, f.efsm_len);
        fflush(stderr);
    }
    if (rc == EFS_OK && f.have_root &&
        efs_export_root_copy(&g_client.export.root, &f.root) == EFS_OK) {
        g_client.export.meta_fragmented = 1;
        if (!g_client.export.next_ino)
            g_client.export.next_ino = f.root.next_ino;
        uint32_t cs = f.root.chunk_size;
        g_client.export.chunk_size = efs_chunk_size_valid(cs)
                                         ? cs : EFS_DEFAULT_CHUNK_SIZE;
        g_client.export.features = f.root.features;
    }
    if (rc == EFS_OK) {
        /* Republish a clean generation over the adopted one. */
        g_client.meta_dirty = 1;
        if (g_client.export.next_ino < old_next_ino)
            g_client.export.next_ino = old_next_ino;

        for (uint64_t i = 0; i < ndel; i++) {
            struct efs_inode tmp;
            if (efs_export_get_inode(&g_client.export, del_inos[i], &tmp) == 0)
                efs_export_unlink(&g_client.export, del_inos[i]);
        }

        /* Concurrent mkdir of the same path by two clients produces duplicate
         * (parent,name) rows with different inos; the name index can only
         * bind one, orphaning the other subtree. Dedup dirty DIR records
         * against the fetched table: adopt the committed row's ino and remap
         * our ino away (children + chunks rewritten below). Fixpoint because
         * nested dirty dirs ("/a" and "/a/b" in one batch) chain remaps. */
        uint64_t *map_old = malloc((nrec ? nrec : 1) * sizeof(*map_old));
        uint64_t *map_new = malloc((nrec ? nrec : 1) * sizeof(*map_new));
        uint8_t *done = calloc(nrec ? nrec : 1, 1);
        uint64_t nmap = 0;
        if (map_old && map_new && done) {
            int progress = 1;
            for (uint64_t iter = 0; progress && iter <= nrec; iter++) {
                progress = 0;
                for (uint64_t i = 0; i < nrec; i++) {
                    if (done[i] || (ino_recs[i].mode & S_IFMT) != S_IFDIR)
                        continue;
                    for (uint64_t m = 0; m < nmap; m++)
                        if (ino_recs[i].parent == map_old[m]) {
                            ino_recs[i].parent = map_new[m];
                            break;
                        }
                    struct efs_inode exst;
                    int lrc = efs_export_lookup(&g_client.export,
                                                ino_recs[i].parent,
                                                ino_recs[i].name, &exst);
                    if (lrc == 0 && exst.ino == ino_recs[i].ino) {
                        done[i] = 1; /* already committed row, refresh below */
                        efs_export_upsert_inode(&g_client.export, &ino_recs[i]);
                        progress = 1;
                    } else if (lrc == 0 && (exst.mode & S_IFMT) == S_IFDIR) {
                        map_old[nmap] = ino_recs[i].ino;
                        map_new[nmap] = exst.ino;
                        nmap++;
                        done[i] = 1;
                        progress = 1;
                    } else if (lrc == 0) {
                        /* file/dir type clash: dir wins the name */
                        efs_export_unlink(&g_client.export, exst.ino);
                        efs_export_upsert_inode(&g_client.export, &ino_recs[i]);
                        done[i] = 1;
                        progress = 1;
                    } else {
                        struct efs_inode pr;
                        if (efs_export_get_inode(&g_client.export,
                                                 ino_recs[i].parent,
                                                 &pr) == 0 ||
                            ino_recs[i].parent == EFS_ROOT_INO) {
                            efs_export_upsert_inode(&g_client.export,
                                                    &ino_recs[i]);
                            done[i] = 1;
                            progress = 1;
                        }
                        /* else: parent is a dirty dir not yet placed — defer */
                    }
                }
            }
        }
        /* Any dirs left unplaced after the fixpoint (shouldn't happen): drop
         * them rather than insert unreachable rows. */

        uint32_t cs = g_client.export.chunk_size
                          ? g_client.export.chunk_size : EFS_DEFAULT_CHUNK_SIZE;
        for (uint64_t i = 0; i < nrec; i++) {
            if ((ino_recs[i].mode & S_IFMT) == S_IFDIR)
                continue; /* handled in pass 1 */
            for (uint64_t m = 0; m < nmap; m++)
                if (ino_recs[i].parent == map_old[m]) {
                    ino_recs[i].parent = map_new[m];
                    break;
                }
            struct efs_inode exst;
            if (efs_export_lookup(&g_client.export, ino_recs[i].parent,
                                  ino_recs[i].name, &exst) == 0 &&
                exst.ino != ino_recs[i].ino) {
                /* Same-path collision with another writer's row. Last writer
                 * wins for files; never destroy a committed DIR for a file. */
                if ((exst.mode & S_IFMT) == S_IFDIR)
                    goto skip_file;
                efs_export_unlink(&g_client.export, exst.ino);
            }
            efs_export_upsert_inode(&g_client.export, &ino_recs[i]);
            if ((ino_recs[i].mode & S_IFMT) == S_IFREG && !ino_recs[i].pack_ino)
                efs_export_drop_chunks_from(&g_client.export, ino_recs[i].ino,
                                            (uint32_t)((ino_recs[i].size + cs - 1) / cs));
        skip_file:;
        }
        for (uint64_t i = 0; i < nch; i++) {
            for (uint64_t m = 0; m < nmap; m++)
                if (ch_recs[i].ino == map_old[m]) {
                    ch_recs[i].ino = map_new[m];
                    break;
                }
            efs_export_set_chunk(&g_client.export, ch_recs[i].ino,
                                 ch_recs[i].chunk_index,
                                 ch_recs[i].fragment_nodes, ch_recs[i].checksums);
        }
        free(map_old);
        free(map_new);
        free(done);
        if (nmap)
            fprintf(stderr, "meta: resync deduped %llu duplicate dir(s)\n",
                    (unsigned long long)nmap);
        efs_export_recompute_rollups(&g_client.export);
        if (efs_client_meta_cache_adopt(f.efsm, f.efsm_len) == 0)
            f.efsm = NULL;
        fprintf(stderr,
                "meta: resync rebased %llu inode(s) (%llu deleted) + %llu "
                "chunk(s) onto gen %llu\n",
                (unsigned long long)nrec, (unsigned long long)ndel,
                (unsigned long long)nch,
                (unsigned long long)g_client.export.root.generation);
        fflush(stderr);
    }

    free(ino_recs);
    free(del_inos);
    free(ch_recs);
    pthread_mutex_unlock(&g_client.dirty_mu);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    pthread_mutex_unlock(&g_repl_mu);
    free(f.efsm);
    efs_export_root_free(&f.root);
    return rc;
}

/* Non-blocking flush for the batched hot path. If another flush is already in
 * flight it serializes the full table, which coalesces our dirty ops — so
 * return instead of stalling every worker behind g_repl_mu for the whole
 * O(table) flush. Retries transient failures (releasing the lock between
 * attempts so a concurrent flush can coalesce). */
static int efs_client_replicate_metadata_nb(void)
{
    int rc = EFS_ERR_NET;
    for (int attempt = 1; attempt <= 4; attempt++) {
        if (pthread_mutex_trylock(&g_repl_mu) != 0)
            return EFS_OK; /* in-flight flush coalesces our ops */
        rc = efs_client_replicate_metadata_locked();
        pthread_mutex_unlock(&g_repl_mu);
        if (rc == EFS_OK)
            return EFS_OK;
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM &&
            rc != EFS_ERR_STALE && rc != EFS_ERR_BUSY)
            break;
        /* STALE resyncs (we're behind); BUSY just backs off — another
         * writer is mid-flush and our gen may still be next. */
        if (rc == EFS_ERR_STALE)
            efs_client_meta_resync();
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1);
            usleep(base + (useconds_t)(rand() % 50000));
        }
    }
    return rc;
}

/* fsync: one incremental publish shared by all waiters. 8× end_fsync used
 * to each run a full/incremental flush (5–18s) because every caller took
 * g_repl_mu and found new dirty from the others still writing. */
int efs_client_sync_meta(void)
{
    const char *skip = getenv("EFS_SKIP_META_FLUSH");
    if (skip && *skip && strcmp(skip, "0") != 0)
        return EFS_OK;
    pthread_mutex_lock(&g_sync_mu);
    int waited = 0;
    for (;;) {
        if (!g_sync_meta_active) {
            if (waited) {
                pthread_mutex_lock(&g_client.dirty_mu);
                int clean = (g_client.dirty_ino_count == 0 &&
                             g_client.dirty_chunk_count == 0 &&
                             !g_client.meta_dirty);
                pthread_mutex_unlock(&g_client.dirty_mu);
                if (clean) {
                    int rc = g_sync_meta_rc;
                    pthread_mutex_unlock(&g_sync_mu);
                    return rc;
                }
            }
            g_sync_meta_active = 1;
            pthread_mutex_unlock(&g_sync_mu);
            int rc = efs_client_replicate_metadata();
            pthread_mutex_lock(&g_sync_mu);
            g_sync_meta_rc = rc;
            g_sync_meta_active = 0;
            pthread_cond_broadcast(&g_sync_meta_cv);
            pthread_mutex_unlock(&g_sync_mu);
            return rc;
        }
        waited = 1;
        pthread_cond_wait(&g_sync_meta_cv, &g_sync_mu);
    }
}

int efs_client_meta_cache_adopt(char *blob, size_t blob_len)
{
    if (!blob || blob_len < EFS_META_HDR_SIZE)
        return -1;
    uint64_t ic = g_client.export.inode_count;
    uint64_t cc = g_client.export.chunk_count;
    uint32_t ver = EFS_META_EFSM_V5;
    memcpy(&ver, blob + 4, 4);
    uint32_t ino_len;
    uint32_t ch_len = (uint32_t)(cc * EFS_CHUNK_WIRE_SIZE);
    if (ver >= EFS_META_EFSM_V6) {
        size_t dent = (size_t)g_client.export.dentry_bytes;
        ino_len = (uint32_t)(EFS_META_HDR_SIZE +
                             ic * EFS_INODE_COMPACT_SIZE + dent);
    } else {
        ino_len = (uint32_t)(EFS_META_HDR_SIZE + ic * EFS_INODE_WIRE_SIZE);
    }
    if (blob_len < (size_t)ino_len + (size_t)ch_len)
        return -1;
    free(g_client.meta_cache_blob);
    free(g_client.meta_cache_ch);
    g_client.meta_cache_ch = NULL;
    g_client.meta_cache_ch_cap = 0;
    g_client.meta_cache_blob = blob;
    g_client.meta_cache_cap = blob_len;
    g_client.meta_cache_len = blob_len;
    g_client.meta_cache_ino_len = ino_len;
    g_client.meta_cache_ch_len = ch_len;
    g_client.meta_cache_icount = ic;
    g_client.meta_cache_ccount = cc;
    g_client.meta_cache_epoch = g_client.export.layout_epoch;
    fprintf(stderr,
            "meta: adopted cache blob %zuB ino_len=%u ch_len=%u "
            "inodes=%llu chunks=%llu\n",
            blob_len, ino_len, ch_len,
            (unsigned long long)ic, (unsigned long long)cc);
    fflush(stderr);
    return 0;
}

int efs_client_replicate_metadata(void)
{
    if (g_client.meta_cap_blocked) {
        if (efs_export_fits_page_cap(&g_client.export, 0, 0))
            g_client.meta_cap_blocked = 0;
        else
            return EFS_ERR_QUOTA;
    }
    /* Bulk copies (ecopy/rsync) hit a full meta flush every meta_batch_ops
     * creates/chmods. Transient net/quorum blips show up as fchmod EIO —
     * retry with exponential backoff + jitter before surfacing failure. */
    int rc = EFS_ERR_NET;
    int hard_fails = 0;
    /* STALE/BUSY are election races, not outages: the server FIFO grants the
     * queue head the next free election, so a writer that keeps BEGINing is
     * guaranteed a turn within one grant cycle. A cycle is N_writers x
     * flush-time — with 9+ writers flushing a multi-hundred-MB blob that is
     * minutes, far beyond the old 64-attempt (~58 s) budget, which is why
     * fsyncs returned EIO under copy storms. Race for up to 4 minutes
     * (queue entries live 300 s and refresh on every BEGIN). NET/NO_QUORUM
     * stay short-fused: those mean the cluster is genuinely down. */
    struct timespec rts;
    clock_gettime(CLOCK_MONOTONIC, &rts);
    int64_t race_deadline_ms = (int64_t)rts.tv_sec * 1000 +
                               (int64_t)rts.tv_nsec / 1000000 + 240000;
    for (int attempt = 1; attempt <= 512; attempt++) {
        rc = efs_client_replicate_metadata_once();
        if (rc == EFS_OK) {
            g_client.meta_cap_blocked = 0;
            return EFS_OK;
        }
        if (rc == EFS_ERR_QUOTA || rc == EFS_ERR_INVAL)
            break;
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM &&
            rc != EFS_ERR_STALE && rc != EFS_ERR_BUSY)
            break;
        if (rc == EFS_ERR_NET || rc == EFS_ERR_NO_QUORUM) {
            if (++hard_fails >= 4)
                break;
            fprintf(stderr, "meta replicate attempt %d/4 failed: %s\n",
                    hard_fails, efs_strerror(rc));
            fflush(stderr);
        } else {
            clock_gettime(CLOCK_MONOTONIC, &rts);
            int64_t now_ms = (int64_t)rts.tv_sec * 1000 +
                             (int64_t)rts.tv_nsec / 1000000;
            if (now_ms >= race_deadline_ms)
                break;
            if (rc == EFS_ERR_STALE)
                efs_client_meta_resync();
            if (attempt <= 3 || attempt % 16 == 0) {
                fprintf(stderr, "meta replicate attempt %d: %s (racing concurrent writers)\n",
                        attempt, efs_strerror(rc));
                fflush(stderr);
            }
        }
        useconds_t base = 100000u << (attempt < 4 ? (attempt - 1) : 3);
        usleep(base + (useconds_t)(rand() % 100000));
    }
    fprintf(stderr, "meta replicate giving up: %s\n", efs_strerror(rc));
    fflush(stderr);
    return rc;
}

/* Flush-thread main: waits for threshold hits and runs blocking flushes off
 * the FUSE worker threads. g_repl_mu still serializes against forced flushes
 * (fsync/unmount) from the op path. */
static void *meta_flush_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_client.meta_flush_mu);
    for (;;) {
        while (!g_client.meta_flush_req && !g_client.meta_flush_stop)
            pthread_cond_wait(&g_client.meta_flush_cv,
                              &g_client.meta_flush_mu);
        if (g_client.meta_flush_stop)
            break;
        g_client.meta_flush_req = 0;
        pthread_mutex_unlock(&g_client.meta_flush_mu);
        /* Heal waits until the dirty set is idle so ingest PUTs win. */
        if (g_client.meta_heal) {
            pthread_mutex_lock(&g_client.dirty_mu);
            int busy = (g_client.dirty_ino_count || g_client.dirty_chunk_count ||
                        g_client.meta_dirty_ops);
            pthread_mutex_unlock(&g_client.dirty_mu);
            if (busy) {
                g_client.meta_heal_pending = 1;
                pthread_mutex_lock(&g_client.meta_flush_mu);
                continue;
            }
        }
        (void)efs_client_replicate_metadata();
        if (g_client.meta_heal_pending && !g_client.meta_heal) {
            g_client.meta_heal_pending = 0;
        } else if (g_client.meta_heal_pending) {
            pthread_mutex_lock(&g_client.dirty_mu);
            int idle = (g_client.dirty_ino_count == 0 &&
                        g_client.dirty_chunk_count == 0 &&
                        g_client.meta_dirty_ops == 0);
            pthread_mutex_unlock(&g_client.dirty_mu);
            if (idle)
                efs_client_schedule_meta_heal();
        }
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
    dirty_sets_clear();

    if (!g_client.meta_flush_started) {
        pthread_mutex_init(&g_client.meta_flush_mu, NULL);
        pthread_cond_init(&g_client.meta_flush_cv, NULL);
        g_client.meta_flush_req = 0;
        g_client.meta_flush_stop = 0;
        if (pthread_create(&g_client.meta_flush_tid, NULL, meta_flush_main,
                           NULL) == 0)
            g_client.meta_flush_started = 1;
    }
    /* Heal only when idle; ingest dirties the table immediately after mount. */
    if (g_client.meta_heal)
        efs_client_schedule_meta_heal();
}

void efs_client_schedule_meta_heal(void)
{
    if (!g_client.meta_heal)
        return;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    int busy = (g_client.dirty_ino_count || g_client.dirty_chunk_count ||
                g_client.meta_dirty_ops);
    if (busy) {
        g_client.meta_heal_pending = 1;
        pthread_mutex_unlock(&g_client.dirty_mu);
        return;
    }
    g_client.meta_dirty = 1;
    pthread_mutex_unlock(&g_client.dirty_mu);
    fprintf(stderr,
            "meta: scheduling background heal flush (%u skipped chunk page(s))\n",
            g_client.meta_heal_skipped);
    fflush(stderr);
    if (g_client.meta_flush_started) {
        pthread_mutex_lock(&g_client.meta_flush_mu);
        g_client.meta_flush_req = 1;
        pthread_cond_signal(&g_client.meta_flush_cv);
        pthread_mutex_unlock(&g_client.meta_flush_mu);
        return;
    }
    (void)efs_client_replicate_metadata_nb();
}

int efs_client_take_write_lease(void)
{
    /* Multi-client fio (fcstor007–015) must all write. An exclusive lease
     * would mount the rest read-only. */
    const char *share = getenv("EFS_SHARE_WRITE_LEASE");
    if (share && *share && strcmp(share, "0") != 0) {
        g_client.write_readonly = 0;
        return EFS_OK;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ull +
                      (uint64_t)ts.tv_nsec / 1000000ull;
    struct efs_export_root *r = &g_client.export.root;
    if (r->write_lease_id && r->write_lease_until_ms > now_ms &&
        r->write_lease_id != g_client.write_lease_id) {
        g_client.write_readonly = 1;
        fprintf(stderr, "meta: write lease held by %llu until %llu; read-only\n",
                (unsigned long long)r->write_lease_id,
                (unsigned long long)r->write_lease_until_ms);
        fflush(stderr);
        return EFS_ERR_BUSY;
    }
    uint64_t id = ((uint64_t)getpid() << 32) ^ now_ms ^
                  (uint64_t)(uintptr_t)&g_client;
    if (id == 0)
        id = 1;
    g_client.write_lease_id = id;
    g_client.write_readonly = 0;
    r->write_lease_id = id;
    r->write_lease_until_ms = now_ms + 3600ull * 1000ull;
    if (r->version < 6)
        r->version = 6;
    if (r->shard_count == 0)
        r->shard_count = 1;
    return EFS_OK;
}

void efs_client_stop_meta_flush(void)
{
    if (!g_client.meta_flush_started)
        return;
    pthread_mutex_lock(&g_client.meta_flush_mu);
    g_client.meta_flush_stop = 1;
    pthread_cond_signal(&g_client.meta_flush_cv);
    pthread_mutex_unlock(&g_client.meta_flush_mu);
    pthread_join(g_client.meta_flush_tid, NULL);
    g_client.meta_flush_started = 0;
}

int efs_client_note_meta_change(int force)
{
    const char *skip = getenv("EFS_SKIP_META_FLUSH");
    if (skip && *skip && strcmp(skip, "0") != 0)
        return EFS_OK;
    if (force)
        g_client.meta_flush_force = 1;
    if (!g_client.meta_batch || force)
        return efs_client_replicate_metadata();

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
            pthread_mutex_lock(&g_client.meta_flush_mu);
            g_client.meta_flush_req = 1;
            pthread_cond_signal(&g_client.meta_flush_cv);
            pthread_mutex_unlock(&g_client.meta_flush_mu);
            return EFS_OK;
        }
        return efs_client_replicate_metadata_nb();
    }
    return EFS_OK;
}

int efs_client_put_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, const uint8_t *data, uint32_t frag_len,
                            const uint8_t checksum[EFS_HASH_SIZE])
{
    if (node_id == 0 || frag_len == 0)
        return EFS_ERR_INVAL;

    /* Retry after efsd bounce: pooled fds die; drop invalidates the idle
     * pool and the next attempt reconnects. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        struct efs_conn *conn = efs_client_conn_get(node_id);
        if (!conn) {
            if (attempt < 3) {
                usleep(100000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        struct efs_msg_put_chunk hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.export_id = g_client.export_id;
        hdr.ino = ino;
        hdr.chunk_index = chunk_index;
        hdr.fragment_index = fragment_index;
        hdr.data_len = frag_len;
        memcpy(hdr.checksum, checksum, EFS_HASH_SIZE);

        uint8_t reply_type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_conn_send_msg_parts(conn, EFS_MSG_PUT_CHUNK, &hdr, sizeof(hdr),
                                    data, frag_len) != 0 ||
            efs_conn_recv_msg(conn, &reply_type, &reply, &reply_len) != 0 ||
            reply_type != EFS_MSG_PUT_CHUNK_REPLY || reply_len != 1) {
            free(reply);
            efs_client_conn_drop(node_id, conn);
            if (attempt < 3) {
                usleep(100000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }
        uint8_t status = ((uint8_t *)reply)[0];
        free(reply);
        efs_client_conn_release(node_id, conn);
        if (status == EFS_PUT_CHUNK_OK)
            return EFS_OK;
        if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
            return EFS_ERR_QUOTA;
        return EFS_ERR_IO;
    }
    return EFS_ERR_NET;
}

/* Consume one PUT_CHUNK reply from a conn that reply_watch marked ready
 * (or whose poll fd fired). Returns 1 on OK ack, 0 on quota, -1 when the
 * conn was dropped. */
static int put_recv_reply(efs_node_id_t nid, struct efs_conn *conn)
{
    uint8_t reply_type = 0, status = 0;
    if (efs_conn_recv_u8_reply(conn, &reply_type, &status) != 0 ||
        reply_type != EFS_MSG_PUT_CHUNK_REPLY) {
        efs_client_conn_drop(nid, conn);
        efs_client_node_note_fail(nid);
        return -1;
    }
    if (status == EFS_PUT_CHUNK_OK) {
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
                                      int failed_out[EFS_NUM_FRAGMENTS])
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
            efs_client_node_note_fail(nodes[i]);
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
        /* Harvest replies that are already here (RDMA CQE / readable fd). */
        for (int i = 0; i < EFS_NUM_FRAGMENTS && acks < 2; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i]);
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

        struct pollfd pfds[EFS_NUM_FRAGMENTS];
        int map[EFS_NUM_FRAGMENTS];
        int npoll = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i]);
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
            int r = put_recv_reply(nodes[i], conns[i]);
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
                int w = efs_conn_reply_watch(conns[i]);
                if (w == EFS_CONN_REPLY_READY) {
                    put_recv_reply(nodes[i], conns[i]);
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
                put_recv_reply(nodes[i], conns[i]);
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
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
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
                                         frag_len, checksums, failed);
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
            size_t got = 0;
            int rrc = efs_client_read(ino, chunk_start, existing, (char *)chunk, &got);
            if (rrc != EFS_OK || got != existing) {
                got = 0;
                rrc = efs_client_read(ino, chunk_start, existing,
                                      (char *)chunk, &got);
            }
            if (rrc != EFS_OK || got != existing) {
                struct efs_chunk_entry ce;
                if (export_chunk_copy(ino, ci, &ce) != 0) {
                    /* Size can be ahead of the store (async WB / dcache
                     * patch). No published chunk yet — base is zeros. */
                    *from_zero_out = 1;
                    if (off_in_chunk > 0)
                        memset(chunk, 0, off_in_chunk);
                    if (off_in_chunk + wr_len < chunk_size)
                        memset(chunk + off_in_chunk + wr_len, 0,
                               chunk_size - off_in_chunk - wr_len);
                } else {
                    /* Chunk is in the table; a failed GET is real IO. */
                    return (rrc != EFS_OK) ? rrc : EFS_ERR_IO;
                }
            }
            if (existing < chunk_size)
                memset(chunk + existing, 0, chunk_size - existing);
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
    uint64_t offset;
    const char *buf;
    uint64_t end;
    int rc;
    int deferred;
    struct put_batch *bp;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

static void *chunk_put_worker(void *arg);

/* Dirty assembled-chunk cache. Partial writes (4k randwrite) used to GET+encode
 * +PUT a full 128 KiB chunk on every FUSE write. Hold the assembled chunk and
 * PUT once on flush / unmount. 65536 × 128 KiB ≈ 8 GiB if every slot is live;
 * buffers are allocated on first store. Collision does not evict (that
 * serialized PUTs on one mutex and tanked 4k IOPS) — the caller PUTs now. */
#define DCACHE_SLOTS  65536
#define DCACHE_SHARDS 64
#define DCACHE_NR     8
struct dcache_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
    int dirty;
    int have_base; /* 1 = data[] is a complete chunk; 0 = sparse patches */
    uint8_t nrange;
    uint32_t roff[DCACHE_NR];
    uint32_t rlen[DCACHE_NR];
    struct dcache_ent *next;
};
static struct {
    pthread_mutex_t shard[DCACHE_SHARDS];
    struct dcache_ent e[DCACHE_SLOTS];
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
    for (int i = 0; i < DCACHE_SHARDS; i++)
        pthread_mutex_init(&g_dcache.shard[i], NULL);
    g_dcache.inited = 1;
}

static uint32_t dcache_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)ci * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(h & (DCACHE_SLOTS - 1));
}

static pthread_mutex_t *dcache_mu(uint32_t slot)
{
    pthread_once(&g_dcache_once, dcache_init);
    return &g_dcache.shard[slot & (DCACHE_SHARDS - 1)];
}

static int dcache_put_now(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                          uint32_t chunk_size)
{
    uint32_t frag_len = chunk_size / 2;
    uint8_t *parity = efs_buf_alloc(frag_len);
    if (!parity)
        return EFS_ERR_NOMEM;

    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frags[0] = (uint8_t *)(uintptr_t)chunk;
    frags[1] = (uint8_t *)(uintptr_t)(chunk + frag_len);
    frags[2] = parity;
    efs_encode_chunk(chunk, chunk_size, chunk_size, frags);

    const uint8_t *cfrags[EFS_NUM_FRAGMENTS] = {frags[0], frags[1], frags[2]};
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    int from_zero = efs_bytes_are_zero(chunk, chunk_size);
    hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, 0, 0,
                         chunk_size, checksums);

    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);
    int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                               checksums);
    efs_buf_free(parity, frag_len);
    if (rc != EFS_OK)
        return rc;

    if (!export_chunk_exists(ino, ci)) {
        int room = efs_client_ensure_meta_room(0, 1);
        if (room != EFS_OK)
            return room;
    }
    if (efs_export_needs_chunk_grow(&g_client.export))
        export_reserve_chunks_locked(64);
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    efs_export_set_chunk(&g_client.export, ino, ci, nodes, checksums);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    efs_client_mark_chunk_dirty(ino, ci);
    return EFS_OK;
}

static struct dcache_ent *dcache_find(uint32_t s, efs_ino_t ino, uint32_t ci)
{
    for (struct dcache_ent *e = &g_dcache.e[s]; e; e = e->next) {
        if (e->data && e->ino == ino && e->ci == ci)
            return e;
    }
    return NULL;
}

int efs_dcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    return efs_dcache_copy(ino, ci, 0, dst, len);
}

int efs_dcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                    uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    /* have_base=0 entries are sparse patches over an unfetched published
     * chunk: unpatched bytes are zeros, not data. Serving them here would
     * return those zeros to readers / RMW bases — only complete chunks may
     * be copied out. */
    if (e && e->have_base && (uint64_t)off + len <= e->len) {
        memcpy(dst, e->data + off, len);
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

void efs_dcache_drop(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *head = &g_dcache.e[s];
    if (head->ino == ino && head->ci == ci) {
        if (head->dirty && head->len)
            dcache_note_dirty_bytes(-(int64_t)head->len);
        efs_buf_free(head->data, head->len);
        if (head->next) {
            struct dcache_ent *n = head->next;
            *head = *n;
            free(n);
        } else {
            memset(head, 0, sizeof(*head));
        }
        pthread_mutex_unlock(mu);
        return;
    }
    struct dcache_ent *prev = head;
    for (struct dcache_ent *e = head->next; e; prev = e, e = e->next) {
        if (e->ino == ino && e->ci == ci) {
            if (e->dirty && e->len)
                dcache_note_dirty_bytes(-(int64_t)e->len);
            prev->next = e->next;
            efs_buf_free(e->data, e->len);
            free(e);
            break;
        }
    }
    pthread_mutex_unlock(mu);
}

static int dcache_store_owned(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                              uint32_t chunk_size);
static int dcache_merge_owned(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len,
                              uint8_t *chunk, uint32_t cs);

static void dcache_add_range(struct dcache_ent *e, uint32_t off, uint32_t len)
{
    if (!e || e->have_base || !len)
        return;
    uint32_t end = off + len;
    for (uint8_t i = 0; i < e->nrange; i++) {
        uint32_t a = e->roff[i], b = a + e->rlen[i];
        if (off <= b && a <= end) {
            uint32_t lo = a < off ? a : off;
            uint32_t hi = b > end ? b : end;
            e->roff[i] = lo;
            e->rlen[i] = hi - lo;
            return;
        }
    }
    if (e->nrange < DCACHE_NR) {
        e->roff[e->nrange] = off;
        e->rlen[e->nrange] = len;
        e->nrange++;
        return;
    }
    e->roff[0] = 0;
    e->rlen[0] = e->len ? e->len : end;
    e->nrange = 1;
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
    if (e && e->dirty && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        dcache_add_range(e, off, len);
        pthread_mutex_unlock(mu);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

/* First 4k to a chunk: cache the patch without a 128 KiB GET. Published
 * chunks stay have_base=0 until flush, which fetches once and overlays. */
static int dcache_load_and_patch(efs_ino_t ino, uint32_t ci, uint32_t off,
                                 const uint8_t *src, uint32_t len, uint32_t cs)
{
    if (dcache_patch(ino, ci, off, src, len) == 0)
        return 0;

    uint8_t *chunk = efs_buf_alloc(cs);
    if (!chunk)
        return -1;

    int have_base = 0;
    if (efs_rdcache_get(ino, ci, chunk, cs) == 0) {
        have_base = 1;
    } else {
        int published = 0;
        pthread_mutex_lock(&g_client.idx_mu);
        published = (efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0);
        pthread_mutex_unlock(&g_client.idx_mu);
        memset(chunk, 0, cs);
        have_base = !published;
    }
    memcpy(chunk + off, src, len);
    if (dcache_merge_owned(ino, ci, off, src, len, chunk, cs) != 0) {
        efs_buf_free(chunk, cs);
        return -1;
    }
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        e->have_base = have_base;
        e->nrange = 0;
        if (!have_base)
            dcache_add_range(e, off, len);
    }
    pthread_mutex_unlock(mu);
    return 0;
}

static void dcache_note_size(efs_ino_t ino, uint64_t end)
{
    efs_client_lock_dir(ino);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 &&
        cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
        efs_client_mark_ino_dirty(ino);
    }
    /* Overwrite of an already-cached chunk: skip mtime + dirty-ino.
     * Random 4k used to take idx/dir locks and mark the inode dirty
     * on every patch (~250k/s), which capped rw-4k and flooded meta. */
    efs_client_unlock_dir(ino);
}

int efs_dcache_try_patch(efs_ino_t ino, uint64_t offset, uint32_t len,
                         const uint8_t *src)
{
    if (!src || !len)
        return -1;
    uint32_t cs = data_chunk_size();
    if (cs == 0)
        return -1;
    uint32_t ci = (uint32_t)(offset / cs);
    uint32_t off = (uint32_t)(offset % cs);
    if ((uint64_t)off + len > cs)
        return -1;
    if (dcache_patch(ino, ci, off, src, len) != 0 &&
        dcache_load_and_patch(ino, ci, off, src, len, cs) != 0)
        return -1;
    dcache_note_size(ino, offset + len);
    return 0;
}

static int dcache_fill(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       const uint8_t *chunk, uint32_t chunk_size)
{
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
    e->dirty = 1;
    e->have_base = 1;
    e->nrange = 0;
    if (!was_dirty)
        dcache_note_dirty_bytes((int64_t)chunk_size);
    return 0;
}

/* Take ownership of a pool-allocated assemble buffer. Caller must not free
 * `chunk` after success. */
static int dcache_take(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       uint8_t *chunk, uint32_t chunk_size)
{
    int was_dirty = e->dirty && e->data;
    if (e->data && e->data != chunk)
        efs_buf_free(e->data, e->len);
    e->data = chunk;
    e->len = chunk_size;
    e->ino = ino;
    e->ci = ci;
    e->dirty = 1;
    e->have_base = 1;
    e->nrange = 0;
    if (!was_dirty)
        dcache_note_dirty_bytes((int64_t)chunk_size);
    return 0;
}

/* 0 = cached, 1 = should not happen (chain grows), <0 = error. */
static int dcache_store(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                        uint32_t chunk_size)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        int rc = dcache_fill(e, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *head = &g_dcache.e[s];
    if (!head->dirty && !head->data) {
        int rc = dcache_fill(head, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *n = calloc(1, sizeof(*n));
    if (!n) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_NOMEM;
    }
    int rc = dcache_fill(n, ino, ci, chunk, chunk_size);
    if (rc != 0) {
        free(n);
        pthread_mutex_unlock(mu);
        return rc;
    }
    n->next = head->next;
    head->next = n;
    pthread_mutex_unlock(mu);
    return 0;
}

/* Like dcache_store, but `chunk` is stolen on success (not copied). */
static int dcache_store_owned(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                              uint32_t chunk_size)
{
    if (!chunk || !chunk_size)
        return EFS_ERR_INVAL;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        int rc = dcache_take(e, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *head = &g_dcache.e[s];
    if (!head->dirty && !head->data) {
        int rc = dcache_take(head, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *n = calloc(1, sizeof(*n));
    if (!n) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_NOMEM;
    }
    int rc = dcache_take(n, ino, ci, chunk, chunk_size);
    if (rc != 0) {
        free(n);
        pthread_mutex_unlock(mu);
        return rc;
    }
    n->next = head->next;
    head->next = n;
    pthread_mutex_unlock(mu);
    return 0;
}

/* Install a freshly loaded+patched chunk, or fold just [off,len) into a
 * chunk another writer dirtied while we were fetching. Steals `chunk` on
 * success (including the fold path, which frees it). */
static int dcache_merge_owned(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len,
                              uint8_t *chunk, uint32_t cs)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->dirty && e->len >= cs && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        dcache_add_range(e, off, len);
        pthread_mutex_unlock(mu);
        efs_buf_free(chunk, cs);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    if (dcache_store_owned(ino, ci, chunk, cs) != 0)
        return -1;
    efs_rdcache_invalidate(ino, ci);
    return 0;
}

static int dcache_flush_slot(uint32_t s, efs_ino_t only_ino, int have_only)
{
    pthread_mutex_t *mu = dcache_mu(s);
    int rc = EFS_OK;
    pthread_mutex_lock(mu);
    struct dcache_ent *e = &g_dcache.e[s];
    while (e) {
        if (!e->dirty || !e->data || (have_only && e->ino != only_ino)) {
            e = e->next;
            continue;
        }
        efs_ino_t ino = e->ino;
        uint32_t ci = e->ci;
        uint32_t len = e->len;
        int have_base = e->have_base;
        uint8_t nrange = e->nrange;
        uint32_t roff[DCACHE_NR], rlen[DCACHE_NR];
        if (!have_base && nrange) {
            memcpy(roff, e->roff, (size_t)nrange * sizeof(uint32_t));
            memcpy(rlen, e->rlen, (size_t)nrange * sizeof(uint32_t));
        }
        uint8_t *copy = efs_buf_alloc(len);
        if (!copy) {
            pthread_mutex_unlock(mu);
            return EFS_ERR_NOMEM;
        }
        memcpy(copy, e->data, len);
        e->dirty = 0;
        /* Un-count now, while the state transition is atomic. The old code
         * decremented only if the entry was still clean after the network
         * PUT; a re-dirty during the PUT re-added a full chunk, so every
         * flush/redirty race leaked 128 KiB into dirty_bytes and pinned it
         * above the reclaim limit (perpetual flush storms). */
        dcache_note_dirty_bytes(-(int64_t)len);
        pthread_mutex_unlock(mu);
        if (!have_base) {
            uint8_t *base = efs_buf_alloc(len);
            if (!base) {
                efs_buf_free(copy, len);
                pthread_mutex_lock(mu);
                if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                    e->dirty = 1;
                    dcache_note_dirty_bytes((int64_t)len);
                }
                pthread_mutex_unlock(mu);
                return EFS_ERR_NOMEM;
            }
            size_t got = 0;
            int rrc = efs_client_read(ino, (uint64_t)ci * len, len,
                                      (char *)base, &got);
            if (rrc != EFS_OK) {
                got = 0;
                rrc = efs_client_read(ino, (uint64_t)ci * len, len,
                                      (char *)base, &got);
            }
            if (rrc != EFS_OK) {
                efs_buf_free(base, len);
                efs_buf_free(copy, len);
                pthread_mutex_lock(mu);
                if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                    e->dirty = 1;
                    dcache_note_dirty_bytes((int64_t)len);
                }
                if (rc == EFS_OK)
                    rc = rrc;
                e = e->next;
                continue;
            }
            if (got < len)
                memset(base + got, 0, len - got);
            for (uint8_t i = 0; i < nrange; i++) {
                if (roff[i] + rlen[i] <= len)
                    memcpy(base + roff[i], copy + roff[i], rlen[i]);
            }
            memcpy(copy, base, len);
            efs_buf_free(base, len);
        }
        int prc = dcache_put_now(ino, ci, copy, len);
        efs_buf_free(copy, len);
        pthread_mutex_lock(mu);
        if (prc != EFS_OK) {
            if (rc == EFS_OK)
                rc = prc;
            /* PUT failed: this slot is the only copy. Keep it dirty. */
            if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                e->dirty = 1;
                dcache_note_dirty_bytes((int64_t)len);
            }
            e = e->next;
            continue;
        }
        if (!e->dirty && e->ino == ino && e->ci == ci) {
            efs_buf_free(e->data, e->len);
            e->data = NULL;
            e->len = 0;
            e->ino = 0;
            e->ci = 0;
        }
        e = e->next;
    }
    pthread_mutex_unlock(mu);
    return rc;
}

/* Close/truncate used to walk all 65536 slots (and 64 shard locks) per
 * file. ecopy hit that on flush+release+ftruncate (~200k locks/file).
 * Chunks are dense from 0, so only those hashed slots can hold this ino. */
static int dcache_flush_all_slots(efs_ino_t only_ino, int have_only)
{
    int rc = EFS_OK;
    pthread_once(&g_dcache_once, dcache_init);
    for (uint32_t i = 0; i < DCACHE_SLOTS; i++) {
        int prc = dcache_flush_slot(i, only_ino, have_only);
        if (prc != EFS_OK && rc == EFS_OK)
            rc = prc;
    }
    return rc;
}

int efs_dcache_flush_ino(efs_ino_t ino)
{
    uint32_t cs = data_chunk_size();
    uint32_t nci = 1;
    struct efs_inode inode;
    if (cs && efs_export_get_inode(&g_client.export, ino, &inode) == 0 &&
        inode.size > 0) {
        uint64_t n = (inode.size + (uint64_t)cs - 1) / (uint64_t)cs;
        if (n > UINT32_MAX)
            return dcache_flush_all_slots(ino, 1);
        nci = (uint32_t)n;
        if (nci == 0)
            nci = 1;
    }
    /* Hashed slots are exact for dense ci 0..nci-1. Walking all 65536
     * slots was the old fallback once nci > 4096 (a 512 MiB file) and
     * made 1G fsync take tens of seconds. Only scan the table when
     * hashing every ci would touch more slots than exist. */
    if (nci > DCACHE_SLOTS)
        return dcache_flush_all_slots(ino, 1);

    int rc = EFS_OK;
    pthread_once(&g_dcache_once, dcache_init);
    for (uint32_t ci = 0; ci < nci; ci++) {
        int prc = dcache_flush_slot(dcache_slot(ino, ci), ino, 1);
        if (prc != EFS_OK && rc == EFS_OK)
            rc = prc;
    }
    return rc;
}

int efs_dcache_flush_all(void)
{
    return dcache_flush_all_slots(0, 0);
}

/* Background incremental reclaim. The old maybe_reclaim ran a full
 * efs_dcache_flush_all() on the WRITER thread once dirty_bytes crossed the
 * limit: a 65536-slot walk with a synchronous GET+PUT per dirty chunk. With
 * a working set at/over the limit (random 4k on >= 2 GiB files) that fired
 * constantly — flush_all was 41% of efs-fuse CPU and chunks were re-PUT ~4x
 * per test as writers re-dirtied slots mid-walk. Now writers just signal;
 * a small pool sweeps slots round-robin until dirty_bytes is back under the
 * low-water mark, so drain cost is spread and parallel instead of a stall. */
#define DCACHE_RECLAIM_THREADS 4
#define DCACHE_RECLAIM_SCAN    64 /* slots per worker wake */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t cursor;
    uint64_t lim;
    int shutdown;
    int active;
} g_reclaim = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static pthread_once_t g_reclaim_once = PTHREAD_ONCE_INIT;

static uint64_t dcache_reclaim_limit(void)
{
    /* 512 MiB default (was 2 GiB): push data down the pipe sooner. The
     * write-behind window only needs to absorb burst jitter, not hold
     * minutes of throughput. */
    uint64_t lim = 512ull << 20;
    const char *env = getenv("EFS_DCACHE_BYTES");
    if (env && *env) {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && v >= (1ull << 20) && v <= (4ull << 30))
            lim = (uint64_t)v;
    }
    return lim;
}

static void *dcache_reclaim_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_reclaim.mu);
    for (;;) {
        while (!g_reclaim.shutdown &&
               __atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED) <=
                   g_reclaim.lim)
            pthread_cond_wait(&g_reclaim.cv, &g_reclaim.mu);
        if (g_reclaim.shutdown)
            break;
        g_reclaim.active++;
        pthread_mutex_unlock(&g_reclaim.mu);

        uint64_t low = g_reclaim.lim - g_reclaim.lim / 8;
        for (int i = 0; i < DCACHE_RECLAIM_SCAN; i++) {
            if (__atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED) <=
                low)
                break;
            uint32_t s = __atomic_fetch_add(&g_reclaim.cursor, 1,
                                            __ATOMIC_RELAXED) &
                         (DCACHE_SLOTS - 1);
            (void)dcache_flush_slot(s, 0, 0);
        }
        pthread_mutex_lock(&g_reclaim.mu);
        g_reclaim.active--;
        pthread_cond_broadcast(&g_reclaim.cv);
    }
    pthread_mutex_unlock(&g_reclaim.mu);
    return NULL;
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
    pthread_mutex_lock(&g_reclaim.mu);
    pthread_cond_signal(&g_reclaim.cv);
    pthread_mutex_unlock(&g_reclaim.mu);
    /* Hard cap at 2x: the background pool is drain-limited (a sparse-entry
     * flush is a GET+PUT at disk latency), so writers that outpace it must
     * help — otherwise a >>limit working set grows the dcache without bound.
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
#define PUT_POOL_QDEPTH (8 * EFS_WRITE_PIPELINE)
static struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct chunk_put_job *q[PUT_POOL_QDEPTH];
    int head, tail, count;
    pthread_t tids[EFS_WRITE_PIPELINE];
    int nworkers;
    int ready;
    int shutdown;
} g_put_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
};

static void *put_pool_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_put_pool.mu);
        while (g_put_pool.count == 0 && !g_put_pool.shutdown)
            pthread_cond_wait(&g_put_pool.not_empty, &g_put_pool.mu);
        if (g_put_pool.shutdown && g_put_pool.count == 0) {
            pthread_mutex_unlock(&g_put_pool.mu);
            return NULL;
        }
        struct chunk_put_job *job = g_put_pool.q[g_put_pool.head];
        g_put_pool.head = (g_put_pool.head + 1) % PUT_POOL_QDEPTH;
        g_put_pool.count--;
        pthread_cond_signal(&g_put_pool.not_full);
        pthread_mutex_unlock(&g_put_pool.mu);

        chunk_put_worker(job);

        struct put_batch *bp = job->bp;
        pthread_mutex_lock(&bp->mu);
        if (--bp->remaining == 0)
            pthread_cond_signal(&bp->cv);
        pthread_mutex_unlock(&bp->mu);
    }
}

static int put_pool_ensure(void)
{
    if (g_put_pool.ready)
        return 0;
    pthread_mutex_lock(&g_put_pool.mu);
    if (!g_put_pool.ready) {
        uint32_t n = EFS_WRITE_PIPELINE;
        for (uint32_t i = 0; i < n; i++) {
            if (pthread_create(&g_put_pool.tids[i], NULL, put_pool_thread,
                               NULL) != 0) {
                g_put_pool.shutdown = 1;
                pthread_cond_broadcast(&g_put_pool.not_empty);
                pthread_mutex_unlock(&g_put_pool.mu);
                for (uint32_t j = 0; j < i; j++)
                    pthread_join(g_put_pool.tids[j], NULL);
                g_put_pool.shutdown = 0;
                return -1;
            }
        }
        g_put_pool.nworkers = (int)n;
        g_put_pool.ready = 1;
    }
    pthread_mutex_unlock(&g_put_pool.mu);
    return 0;
}

static int put_pool_run(struct chunk_put_job *jobs, uint32_t batch)
{
    if (batch == 0)
        return 0;
    if (batch == 1 || put_pool_ensure() != 0) {
        for (uint32_t i = 0; i < batch; i++)
            chunk_put_worker(&jobs[i]);
        return 0;
    }
    struct put_batch bp;
    pthread_mutex_init(&bp.mu, NULL);
    pthread_cond_init(&bp.cv, NULL);
    bp.remaining = (int)batch;

    pthread_mutex_lock(&g_put_pool.mu);
    for (uint32_t i = 0; i < batch; i++) {
        jobs[i].bp = &bp;
        while (g_put_pool.count == PUT_POOL_QDEPTH && !g_put_pool.shutdown)
            pthread_cond_wait(&g_put_pool.not_full, &g_put_pool.mu);
        g_put_pool.q[g_put_pool.tail] = &jobs[i];
        g_put_pool.tail = (g_put_pool.tail + 1) % PUT_POOL_QDEPTH;
        g_put_pool.count++;
        pthread_cond_signal(&g_put_pool.not_empty);
    }
    pthread_mutex_unlock(&g_put_pool.mu);

    pthread_mutex_lock(&bp.mu);
    while (bp.remaining > 0)
        pthread_cond_wait(&bp.cv, &bp.mu);
    pthread_mutex_unlock(&bp.mu);
    pthread_mutex_destroy(&bp.mu);
    pthread_cond_destroy(&bp.cv);
    return 0;
}

static void *chunk_put_worker(void *arg)
{
    struct chunk_put_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    uint32_t frag_len = data_frag_size();
    uint64_t chunk_start = (uint64_t)job->ci * chunk_size;
    uint64_t wr_start = (job->offset > chunk_start) ? job->offset : chunk_start;
    uint64_t wr_end = (job->end < chunk_start + chunk_size)
                          ? job->end
                          : chunk_start + chunk_size;
    int covers_full = (wr_start == chunk_start &&
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
            job->rc = EFS_OK;
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
            job->ino, job->ci, job->nodes, cfrags2, frag_len, job->checksums);
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
                                                cfrags, frag_len, job->checksums);
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

/* ---- small-file packing (point 4) ---- */

#define PACK_STAGE_SLOTS 256
#define PACK_DIR_MAX     128

struct pack_stage {
    efs_ino_t ino;
    efs_ino_t parent;
    uint32_t len;
    uint32_t cap;
    uint8_t *buf;
    struct pack_stage *next;
};

struct dir_pack {
    efs_ino_t dir_ino;
    uint32_t ci;
    uint32_t used;
    uint8_t *buf;
    int live;
};

static struct pack_stage *g_stage[PACK_STAGE_SLOTS];
static pthread_mutex_t g_stage_mu = PTHREAD_MUTEX_INITIALIZER;
static struct dir_pack g_dir_pack[PACK_DIR_MAX];
static pthread_mutex_t g_pack_mu = PTHREAD_MUTEX_INITIALIZER;

static struct pack_stage *stage_find_locked(efs_ino_t ino)
{
    unsigned h = (unsigned)((uint64_t)ino % PACK_STAGE_SLOTS);
    struct pack_stage *s = g_stage[h];
    while (s && s->ino != ino)
        s = s->next;
    return s;
}

static int pack_put_chunk(efs_ino_t dir_ino, uint32_t ci, const uint8_t *chunk,
                          uint32_t chunk_size)
{
    uint32_t frag_len = chunk_size / 2;
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    frag_ptrs(frag_buf, frag_len, frags);
    efs_encode_chunk(chunk, chunk_size, chunk_size, frags);
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, dir_ino, ci, nodes);
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        cfrags[i] = frags[i];
        efs_hash(frags[i], frag_len, checksums[i]);
    }
    int rc = efs_client_put_fragments_parallel(dir_ino, ci, nodes, cfrags,
                                               frag_len, checksums);
    if (rc == EFS_OK) {
        if (!export_chunk_exists(dir_ino, ci)) {
            int room = efs_client_ensure_meta_room(0, 1);
            if (room != EFS_OK) {
                free(frag_buf);
                return room;
            }
        }
        if (efs_export_needs_chunk_grow(&g_client.export))
            export_reserve_chunks_locked(64);
        efs_client_lock_dir(dir_ino);
        pthread_mutex_lock(&g_client.idx_mu);
        efs_export_set_chunk(&g_client.export, dir_ino, ci, nodes, checksums);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(dir_ino);
        efs_client_mark_chunk_dirty(dir_ino, ci);
    }
    free(frag_buf);
    return rc;
}

static struct dir_pack *dir_pack_get(efs_ino_t dir_ino, uint32_t chunk_size)
{
    int free_i = -1;
    for (int i = 0; i < PACK_DIR_MAX; i++) {
        if (g_dir_pack[i].live && g_dir_pack[i].dir_ino == dir_ino)
            return &g_dir_pack[i];
        if (!g_dir_pack[i].live && free_i < 0)
            free_i = i;
    }
    if (free_i < 0)
        return NULL;
    struct dir_pack *p = &g_dir_pack[free_i];
    memset(p, 0, sizeof(*p));
    p->buf = calloc(1, chunk_size);
    if (!p->buf)
        return NULL;
    p->dir_ino = dir_ino;
    p->live = 1;
    /* Next pack chunk index: one past the highest existing chunk of dir.
     * Chunk indexes are dense from 0 — do not scan the whole chunk table. */
    uint32_t max_ci = 0;
    int any = 0;
    struct efs_chunk_entry ce;
    pthread_mutex_lock(&g_client.idx_mu);
    for (uint32_t ci = 0;
         efs_export_get_chunk(&g_client.export, dir_ino, ci, &ce) == 0;
         ci++) {
        any = 1;
        max_ci = ci + 1;
        if (ci == UINT32_MAX)
            break;
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    p->ci = any ? max_ci : 0;
    return p;
}

static int dir_pack_flush(struct dir_pack *p, uint32_t chunk_size)
{
    if (!p || !p->live || p->used == 0)
        return EFS_OK;
    int rc = pack_put_chunk(p->dir_ino, p->ci, p->buf, chunk_size);
    if (rc != EFS_OK)
        return rc;
    memset(p->buf, 0, chunk_size);
    p->ci++;
    p->used = 0;
    return EFS_OK;
}

static int pack_stage_append(efs_ino_t ino, efs_ino_t parent, uint64_t offset,
                             size_t size, const char *buf, uint32_t threshold)
{
    pthread_mutex_lock(&g_stage_mu);
    struct pack_stage *s = stage_find_locked(ino);
    if (!s) {
        if (offset != 0) {
            pthread_mutex_unlock(&g_stage_mu);
            return -1;
        }
        s = calloc(1, sizeof(*s));
        if (!s) {
            pthread_mutex_unlock(&g_stage_mu);
            return EFS_ERR_NOMEM;
        }
        s->cap = threshold;
        s->buf = malloc(s->cap);
        if (!s->buf) {
            free(s);
            pthread_mutex_unlock(&g_stage_mu);
            return EFS_ERR_NOMEM;
        }
        s->ino = ino;
        s->parent = parent;
        unsigned h = (unsigned)((uint64_t)ino % PACK_STAGE_SLOTS);
        s->next = g_stage[h];
        g_stage[h] = s;
    }
    if (offset != s->len || offset + size > threshold) {
        pthread_mutex_unlock(&g_stage_mu);
        return -1;
    }
    memcpy(s->buf + s->len, buf, size);
    s->len += (uint32_t)size;
    pthread_mutex_unlock(&g_stage_mu);
    return EFS_OK;
}

static struct pack_stage *stage_take(efs_ino_t ino)
{
    pthread_mutex_lock(&g_stage_mu);
    unsigned h = (unsigned)((uint64_t)ino % PACK_STAGE_SLOTS);
    struct pack_stage **pp = &g_stage[h];
    while (*pp && (*pp)->ino != ino)
        pp = &(*pp)->next;
    struct pack_stage *s = *pp;
    if (s)
        *pp = s->next;
    pthread_mutex_unlock(&g_stage_mu);
    return s;
}

int efs_client_dir_pack_read(efs_ino_t pack_ino, uint64_t offset, size_t size,
                             char *buf, size_t *out_len)
{
    if (!buf || !out_len)
        return -1;
    uint32_t cs = data_chunk_size();
    if (cs == 0)
        return -1;
    pthread_mutex_lock(&g_pack_mu);
    struct dir_pack *p = NULL;
    for (int i = 0; i < PACK_DIR_MAX; i++) {
        if (g_dir_pack[i].live && g_dir_pack[i].dir_ino == pack_ino) {
            p = &g_dir_pack[i];
            break;
        }
    }
    if (!p || !p->buf || p->used == 0) {
        pthread_mutex_unlock(&g_pack_mu);
        return -1;
    }
    uint64_t chunk_start = (uint64_t)p->ci * cs;
    if (offset < chunk_start || offset >= chunk_start + p->used) {
        pthread_mutex_unlock(&g_pack_mu);
        return -1;
    }
    uint32_t local = (uint32_t)(offset - chunk_start);
    size_t n = (size_t)p->used - local;
    if (n > size)
        n = size;
    memcpy(buf, p->buf + local, n);
    *out_len = n;
    pthread_mutex_unlock(&g_pack_mu);
    return 0;
}

int efs_client_pack_stage_read(efs_ino_t ino, uint64_t offset, size_t size,
                               char *buf, size_t *out_len)
{
    if (!buf || !out_len)
        return -1;
    pthread_mutex_lock(&g_stage_mu);
    struct pack_stage *s = stage_find_locked(ino);
    if (!s) {
        pthread_mutex_unlock(&g_stage_mu);
        return -1;
    }
    if (offset >= s->len) {
        *out_len = 0;
        pthread_mutex_unlock(&g_stage_mu);
        return 0;
    }
    size_t n = (size_t)s->len - (size_t)offset;
    if (n > size)
        n = size;
    memcpy(buf, s->buf + offset, n);
    *out_len = n;
    pthread_mutex_unlock(&g_stage_mu);
    return 0;
}

int efs_client_pack_seal(efs_ino_t ino)
{
    if (!g_client.meta_batch)
        return EFS_OK;
    struct pack_stage *s = stage_take(ino);
    if (!s)
        return EFS_OK;
    uint32_t chunk_size = data_chunk_size();
    if (s->len == 0 || s->len >= chunk_size) {
        /* Too big or empty: fall back to a normal write of the staged bytes. */
        int rc = EFS_OK;
        if (s->len)
            rc = write_chunks_no_replicate(ino, 0, s->len, (char *)s->buf, 0);
        free(s->buf);
        free(s);
        return rc;
    }

    pthread_mutex_lock(&g_pack_mu);
    struct dir_pack *p = dir_pack_get(s->parent, chunk_size);
    if (!p) {
        pthread_mutex_unlock(&g_pack_mu);
        int rc = write_chunks_no_replicate(ino, 0, s->len, (char *)s->buf, 0);
        free(s->buf);
        free(s);
        return rc;
    }
    if (p->used + s->len > chunk_size) {
        int frc = dir_pack_flush(p, chunk_size);
        if (frc != EFS_OK) {
            pthread_mutex_unlock(&g_pack_mu);
            free(s->buf);
            free(s);
            return frc;
        }
    }
    uint32_t off = p->ci * chunk_size + p->used;
    memcpy(p->buf + p->used, s->buf, s->len);
    p->used += s->len;
    efs_ino_t pack_ino = s->parent;
    uint32_t pack_len = s->len;
    /* PUT only when the pack chunk is full. Partial tails wait for
     * pack_flush_all / fsync — sealing every ImageNet file used to
     * encode+PUT a 128 KiB chunk on close. */
    int prc = EFS_OK;
    if (p->used == chunk_size) {
        prc = pack_put_chunk(p->dir_ino, p->ci, p->buf, chunk_size);
        if (prc != EFS_OK) {
            pthread_mutex_unlock(&g_pack_mu);
            free(s->buf);
            free(s);
            return prc;
        }
        memset(p->buf, 0, chunk_size);
        p->ci++;
        p->used = 0;
    }
    pthread_mutex_unlock(&g_pack_mu);

    efs_client_lock_dir(ino);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0) {
        struct efs_inode *ip = NULL;
        uint64_t slot = 0;
        if (efs_export_inode_slot(&g_client.export, ino, &slot) == 0)
            ip = &g_client.export.inodes[slot];
        if (ip) {
            ip->pack_ino = pack_ino;
            ip->pack_off = off;
            ip->pack_len = pack_len;
            if (ip->size < pack_len)
                ip->size = pack_len;
        }
        efs_client_mark_ino_dirty(ino);
    }
    efs_client_unlock_dir(ino);
    free(s->buf);
    free(s);
    return EFS_OK;
}

void efs_client_pack_flush_all(void)
{
    uint32_t chunk_size = data_chunk_size();
    pthread_mutex_lock(&g_pack_mu);
    for (int i = 0; i < PACK_DIR_MAX; i++) {
        if (g_dir_pack[i].live)
            (void)dir_pack_flush(&g_dir_pack[i], chunk_size);
    }
    pthread_mutex_unlock(&g_pack_mu);
}

int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    if (size == 0)
        return EFS_OK;

    efs_client_lock_dir(ino);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
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

        int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                                   checksums);
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
            export_reserve_chunks_locked(64);
        efs_client_lock_dir(ino);
        pthread_mutex_lock(&g_client.idx_mu);
        efs_export_set_chunk(&g_client.export, ino, ci, nodes, checksums);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        efs_client_mark_chunk_dirty(ino, ci);
    }

    efs_client_lock_dir(ino);
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
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    efs_client_replicate_metadata();

    return EFS_OK;
}

int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    if (size == 0)
        return EFS_OK;

    efs_client_lock_dir(ino);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    int packed = (inode.pack_ino != 0);
    efs_client_unlock_dir(ino);

    /* Default: stage ImageNet-sized writes into the parent dir pack so
     * most files add zero chunk-table rows. Seal on FUSE release. */
    if (g_client.meta_batch && !packed && offset == 0) {
        uint32_t cs = data_chunk_size();
        if (size < cs) {
            efs_ino_t parent = inode.parent;
            if (pack_stage_append(ino, parent, offset, size, buf, cs) == 0)
                return EFS_OK;
        }
    }

    if (packed) {
        efs_client_lock_dir(ino);
        uint64_t slot = 0;
        if (efs_export_inode_slot(&g_client.export, ino, &slot) == 0) {
            g_client.export.inodes[slot].pack_ino = 0;
            g_client.export.inodes[slot].pack_off = 0;
            g_client.export.inodes[slot].pack_len = 0;
            efs_client_mark_ino_dirty(ino);
        }
        efs_client_unlock_dir(ino);
    }
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
                export_reserve_chunks_locked(64);
            efs_client_lock_dir(ino);
            pthread_mutex_lock(&g_client.idx_mu);
            {
                struct efs_chunk_entry prev;
                int same = (efs_export_get_chunk(&g_client.export, ino,
                                                 jobs[i].ci, &prev) == 0 &&
                            memcmp(prev.fragment_nodes, jobs[i].nodes,
                                   sizeof(prev.fragment_nodes)) == 0 &&
                            memcmp(prev.checksums, jobs[i].checksums,
                                   sizeof(prev.checksums)) == 0);
                if (!same)
                    efs_export_set_chunk(&g_client.export, ino, jobs[i].ci,
                                         jobs[i].nodes, jobs[i].checksums);
                pthread_mutex_unlock(&g_client.idx_mu);
                efs_client_unlock_dir(ino);
                if (!same)
                    efs_client_mark_chunk_dirty(ino, jobs[i].ci);
            }
        }
        base += batch;
    }

    efs_client_lock_dir(ino);
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
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);

    return EFS_OK;
}
