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

static uint64_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
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

/* Replicate the tiny export root (EFSR) to all nodes; need ≥2 acks.
 * Fan-out + poll (like fragment PUT): skip down peers, return as soon as
 * quorum is met so one dead/blackholed node cannot serialize SO_RCVTIMEO. */
static int send_meta_root(const char *buf, size_t len)
{
    uint32_t n = g_client.node_count;
    if (n > EFS_MAX_NODES)
        n = EFS_MAX_NODES;

    int fds[EFS_MAX_NODES];
    efs_node_id_t nids[EFS_MAX_NODES];
    int pending[EFS_MAX_NODES];
    int acks = 0;

    for (uint32_t i = 0; i < n; i++) {
        fds[i] = -1;
        pending[i] = 0;
        nids[i] = g_client.nodes[i].id;
        if (nids[i] == 0 || efs_client_node_is_down(nids[i]))
            continue;
        fds[i] = efs_client_conn_get(nids[i]);
        if (fds[i] < 0) {
            efs_client_node_note_fail(nids[i]);
            continue;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        if (fds[i] < 0)
            continue;
        if (efs_send_msg(fds[i], EFS_MSG_PUT_META, buf, (uint32_t)len) != 0) {
            efs_client_conn_drop(nids[i], fds[i]);
            efs_client_node_note_fail(nids[i]);
            fds[i] = -1;
            continue;
        }
        pending[i] = 1;
    }

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        struct pollfd pfds[EFS_MAX_NODES];
        int map[EFS_MAX_NODES];
        int npoll = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!pending[i] || fds[i] < 0)
                continue;
            pfds[npoll].fd = fds[i];
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = (int)i;
            npoll++;
        }
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (!pending[i] || fds[i] < 0)
                    continue;
                efs_client_conn_drop(nids[i], fds[i]);
                efs_client_node_note_fail(nids[i]);
                fds[i] = -1;
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
            if (!pending[i] || fds[i] < 0)
                continue;
            uint8_t type = 0;
            void *reply = NULL;
            uint32_t reply_len = 0;
            if ((pfds[p].revents & (POLLERR | POLLHUP)) ||
                efs_recv_msg(fds[i], &type, &reply, &reply_len) != 0 ||
                type != EFS_MSG_PUT_META_REPLY || reply_len < 1) {
                free(reply);
                efs_client_conn_drop(nids[i], fds[i]);
                efs_client_node_note_fail(nids[i]);
                fds[i] = -1;
                pending[i] = 0;
                continue;
            }
            uint8_t status = ((uint8_t *)reply)[0];
            free(reply);
            if (status == EFS_PUT_META_OK) {
                acks++;
                efs_client_node_note_ok(nids[i]);
                efs_client_conn_release(nids[i], fds[i]);
            } else {
                efs_client_conn_drop(nids[i], fds[i]);
                efs_client_node_note_fail(nids[i]);
            }
            fds[i] = -1;
            pending[i] = 0;
        }
    }

    /* Quorum met: drop stragglers without blocking on their RCVTIMEO. */
    for (uint32_t i = 0; i < n; i++) {
        if (fds[i] < 0)
            continue;
        if (acks >= 2) {
            struct pollfd p = { .fd = fds[i], .events = POLLIN };
            int pr = poll(&p, 1, 0);
            if (pr > 0 && (p.revents & POLLIN) &&
                !(p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                uint8_t type = 0;
                void *reply = NULL;
                uint32_t reply_len = 0;
                if (efs_recv_msg(fds[i], &type, &reply, &reply_len) == 0) {
                    free(reply);
                    efs_client_conn_release(nids[i], fds[i]);
                    fds[i] = -1;
                    pending[i] = 0;
                    continue;
                }
                free(reply);
            }
            efs_client_conn_drop(nids[i], fds[i]);
        } else {
            efs_client_conn_drop(nids[i], fds[i]);
            efs_client_node_note_fail(nids[i]);
        }
        fds[i] = -1;
        pending[i] = 0;
    }

    return (acks >= 2) ? EFS_OK : EFS_ERR_NO_QUORUM;
}

/* Serialize concurrent replicators so two flushes don't interleave page PUTs /
 * gen bumps. This replaces holding g_client.lock across serialize+network. */
static pthread_mutex_t g_repl_mu = PTHREAD_MUTEX_INITIALIZER;

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

    size_t ino_bytes = (size_t)snap->inode_count * EFS_INODE_WIRE_SIZE;
    size_t ch_bytes = (size_t)snap->chunk_count * EFS_CHUNK_WIRE_SIZE;
    uint32_t ino_len = (uint32_t)(EFS_META_HDR_SIZE + ino_bytes);
    size_t total = (size_t)ino_len + ch_bytes;

    /* Split a leftover combined blob from a full serialize. */
    if (!g_client.meta_cache_ch && g_client.meta_cache_ch_len &&
        g_client.meta_cache_len >= (size_t)g_client.meta_cache_ino_len +
                                       g_client.meta_cache_ch_len) {
        size_t old_ch = g_client.meta_cache_ch_len;
        if (cache_grow(&g_client.meta_cache_ch, &g_client.meta_cache_ch_cap,
                       old_ch) != 0)
            return -1;
        memcpy(g_client.meta_cache_ch,
               g_client.meta_cache_blob + g_client.meta_cache_ino_len, old_ch);
    }

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
    efs_export_pack_header(snap, (uint8_t *)b);
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

    /* A record that straddles a 128 KiB page must dirty both pages.
     * Marking only the start page left the tail on the previous generation
     * (ino==0 + leftover name → readdir lists a path lookup cannot find). */
    #define MARK_SPAN(bm, npc, off0, rec) do {                          \
        uint32_t _a = (uint32_t)(off0) / EFS_META_PAGE_SIZE;            \
        uint32_t _b = (uint32_t)((off0) + (rec) - 1) / EFS_META_PAGE_SIZE; \
        if (_a < (npc)) (bm)[_a] = 1;                                   \
        if (_b < (npc)) (bm)[_b] = 1;                                   \
    } while (0)

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
                          struct dirty_snap *ds, int full)
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
        fprintf(stderr,
                "meta replicate: region pages exceed cap (ino %u/%u chunk %u/%u)\n",
                efs_meta_page_count_for_blob(ino_blob_len),
                (unsigned)EFS_META_INO_PAGE_MAX,
                efs_meta_page_count_for_blob(chunk_blob_len),
                (unsigned)EFS_META_CHUNK_PAGE_MAX);
        fflush(stderr);
        free(ino_dirty_pg);
        free(ch_dirty_pg);
        if (!blob_is_cache) free(blob);
        return EFS_ERR_INVAL;
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

    /* Pass 2: encode + PUT dirty pages, pipelined. */
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;
    for (uint32_t base = 0; base < ndirty; ) {
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
    if (g_client.meta_batch &&
        g_client.dirty_ino_count == 0 && g_client.dirty_chunk_count == 0 &&
        !g_client.meta_dirty) {
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
    new_gen = ex->root.generation + 1;
    if (new_gen == 0)
        new_gen = 1;
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
        (ds.ino_count || ds.chunk_count) &&
        ex->inode_count >= g_client.meta_cache_icount &&
        ex->chunk_count >= g_client.meta_cache_ccount) {
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
    }
    if (!packed) {
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
            pthread_mutex_unlock(&g_client.dirty_mu);
            efs_client_table_unlock();
            return EFS_ERR_NOMEM;
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
    efs_client_table_unlock();

    int rc = flush_snapshot(&snap, new_gen, committed_ino_pc, committed_ch_pc,
                            &hashes, &sums, &pages, &new_root, &ds, full);
    free(snap.inodes);
    free(snap.chunks);

    pthread_mutex_lock(&g_client.lock);
    if (rc == EFS_OK) {
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
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM)
            break;
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1);
            usleep(base + (useconds_t)(rand() % 50000));
        }
    }
    return rc;
}

int efs_client_replicate_metadata(void)
{
    /* Bulk copies (ecopy/rsync) hit a full meta flush every meta_batch_ops
     * creates/chmods. Transient net/quorum blips show up as fchmod EIO —
     * retry with exponential backoff + jitter before surfacing failure. */
    int rc = EFS_ERR_NET;
    for (int attempt = 1; attempt <= 4; attempt++) {
        rc = efs_client_replicate_metadata_once();
        if (rc == EFS_OK)
            return EFS_OK;
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM)
            break;
        fprintf(stderr, "meta replicate attempt %d/4 failed: %s\n",
                attempt, efs_strerror(rc));
        fflush(stderr);
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1); /* 100,200,400ms */
            usleep(base + (useconds_t)(rand() % 50000));
        }
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
        (void)efs_client_replicate_metadata();
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
    int flush = (ops >= thresh);
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
        int fd = efs_client_conn_get(node_id);
        if (fd < 0) {
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
        if (efs_send_msg_parts(fd, EFS_MSG_PUT_CHUNK, &hdr, sizeof(hdr),
                               data, frag_len) != 0 ||
            efs_recv_msg(fd, &reply_type, &reply, &reply_len) != 0 ||
            reply_type != EFS_MSG_PUT_CHUNK_REPLY || reply_len != 1) {
            free(reply);
            efs_client_conn_drop(node_id, fd);
            if (attempt < 3) {
                usleep(100000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }
        uint8_t status = ((uint8_t *)reply)[0];
        free(reply);
        efs_client_conn_release(node_id, fd);
        if (status == EFS_PUT_CHUNK_OK)
            return EFS_OK;
        if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
            return EFS_ERR_QUOTA;
        return EFS_ERR_IO;
    }
    return EFS_ERR_NET;
}

/* failed_out[i]=1 marks placement slots that need invalidate/retry. */
static int put_fragments_parallel_once(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                                      int failed_out[EFS_NUM_FRAGMENTS])
{
    int fds[EFS_NUM_FRAGMENTS];
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
        fds[i] = -1;
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
        fds[i] = efs_client_conn_get(nodes[i]);
        if (fds[i] < 0) {
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

    /* Send on every live fd before waiting (overlap RTTs). writev header+body
     * so we never bounce-copy the fragment into a contiguous malloc. */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] < 0)
            continue;
        /* Never enter a blocking writev: a slow peer fills the TCP window
         * and SO_SNDTIMEO stalls the whole chunk. Wait a short slice for
         * POLLOUT; a miss is backpressure (failed=2), not a dead peer. */
        {
            struct pollfd p = { .fd = fds[i], .events = POLLOUT };
            int pr = poll(&p, 1, 8);
            if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
                continue;
            }
            if (pr == 0 || !(p.revents & POLLOUT)) {
                efs_client_conn_release(nodes[i], fds[i]);
                if (failed_out)
                    failed_out[i] = 2;
                fds[i] = -1;
                continue;
            }
        }
        if (efs_send_msg_parts(fds[i], EFS_MSG_PUT_CHUNK,
                               &hdrs[i], (uint32_t)sizeof(hdrs[i]),
                               fragments[i], frag_len) != 0) {
            efs_client_conn_drop(nodes[i], fds[i]);
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
            fds[i] = -1;
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
        struct pollfd pfds[EFS_NUM_FRAGMENTS];
        int map[EFS_NUM_FRAGMENTS];
        int npoll = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!pending[i] || fds[i] < 0)
                continue;
            pfds[npoll].fd = fds[i];
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = i;
            npoll++;
        }
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!pending[i] || fds[i] < 0)
                    continue;
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
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
            if (!pending[i] || fds[i] < 0)
                continue;
            uint8_t reply_type = 0, status = 0;
            if ((pfds[p].revents & (POLLERR | POLLHUP)) ||
                efs_recv_u8_reply(fds[i], &reply_type, &status) != 0 ||
                reply_type != EFS_MSG_PUT_CHUNK_REPLY) {
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
                pending[i] = 0;
                continue;
            }
            if (status == EFS_PUT_CHUNK_OK) {
                acks++;
                efs_client_node_note_ok(nodes[i]);
                efs_client_conn_release(nodes[i], fds[i]);
            } else if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED) {
                quota_errors++;
                efs_client_conn_release(nodes[i], fds[i]);
            } else {
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
            }
            fds[i] = -1;
            pending[i] = 0;
        }
    }

    /* Quorum met: drain or drop remaining waiters. Prefer draining a ready
     * reply so pooled fds stay healthy — dropping every third peer was
     * forcing reconnect storms (efs_connect_tcp in the write hot path). */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] < 0)
            continue;
        if (acks >= 2) {
            struct pollfd p = { .fd = fds[i], .events = POLLIN };
            int pr = poll(&p, 1, 0);
            if (pr > 0 && (p.revents & POLLIN) &&
                !(p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                uint8_t reply_type = 0, status = 0;
                if (efs_recv_u8_reply(fds[i], &reply_type, &status) == 0 &&
                    reply_type == EFS_MSG_PUT_CHUNK_REPLY) {
                    efs_client_conn_release(nodes[i], fds[i]);
                    fds[i] = -1;
                    pending[i] = 0;
                    continue;
                }
            }
            /* Still in flight / desynced: drop without note_fail. */
            efs_client_conn_drop(nodes[i], fds[i]);
            if (failed_out)
                failed_out[i] = 1;
        } else {
            efs_client_conn_drop(nodes[i], fds[i]);
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
        }
        fds[i] = -1;
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

int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
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
    for (int attempt = 1; attempt <= 4; attempt++) {
        int failed[EFS_NUM_FRAGMENTS];
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
        if (efs_rdcache_get(ino, ci, chunk, chunk_size) != 0) {
            size_t got = 0;
            int rrc = efs_client_read(ino, chunk_start, existing, (char *)chunk, &got);
            if (rrc != EFS_OK || got != existing) {
                /* RMW base unreadable: refuse rather than zero over live data. */
                return (rrc != EFS_OK) ? rrc : EFS_ERR_IO;
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
struct dcache_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
    int dirty;
    struct dcache_ent *next;
};
static struct {
    pthread_mutex_t shard[DCACHE_SHARDS];
    struct dcache_ent e[DCACHE_SLOTS];
    int inited;
} g_dcache;
static pthread_once_t g_dcache_once = PTHREAD_ONCE_INIT;

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
    uint8_t *parity = malloc(frag_len);
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
    free(parity);
    if (rc != EFS_OK)
        return rc;

    if (efs_export_needs_chunk_grow(&g_client.export)) {
        efs_client_table_lock();
        (void)efs_export_reserve_chunks(&g_client.export, 64);
        efs_client_table_unlock();
    }
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
    if (!dst || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->len >= len) {
        memcpy(dst, e->data, len);
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
        free(head->data);
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
            prev->next = e->next;
            free(e->data);
            free(e);
            break;
        }
    }
    pthread_mutex_unlock(mu);
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
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
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
    if (dcache_patch(ino, ci, off, src, len) != 0)
        return -1;

    uint64_t end = offset + len;
    efs_client_lock_dir(ino);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 &&
        cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    return 0;
}

static int dcache_fill(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       const uint8_t *chunk, uint32_t chunk_size)
{
    if (e->len != chunk_size || !e->data) {
        uint8_t *nbuf = realloc(e->data, chunk_size);
        if (!nbuf)
            return EFS_ERR_NOMEM;
        e->data = nbuf;
        e->len = chunk_size;
    }
    memcpy(e->data, chunk, chunk_size);
    e->ino = ino;
    e->ci = ci;
    e->dirty = 1;
    return 0;
}

/* Take ownership of a malloc'd assemble buffer. Caller must not free
 * `chunk` after success. */
static int dcache_take(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       uint8_t *chunk, uint32_t chunk_size)
{
    if (e->data && e->data != chunk)
        free(e->data);
    e->data = chunk;
    e->len = chunk_size;
    e->ino = ino;
    e->ci = ci;
    e->dirty = 1;
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
        uint8_t *copy = malloc(len);
        if (!copy) {
            pthread_mutex_unlock(mu);
            return EFS_ERR_NOMEM;
        }
        memcpy(copy, e->data, len);
        e->dirty = 0;
        pthread_mutex_unlock(mu);
        int prc = dcache_put_now(ino, ci, copy, len);
        free(copy);
        if (prc != EFS_OK && rc == EFS_OK)
            rc = prc;
        pthread_mutex_lock(mu);
        if (!e->dirty && e->ino == ino && e->ci == ci) {
            free(e->data);
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
#define DCACHE_FLUSH_SCAN_SLOTS  4096

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
    if (nci > DCACHE_FLUSH_SCAN_SLOTS)
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

    if (covers_full)
        efs_dcache_drop(job->ino, job->ci);
    else {
        size_t off_in = (size_t)(wr_start - chunk_start);
        size_t wr_len = (size_t)(wr_end - wr_start);
        size_t src_off = (size_t)(wr_start - job->offset);
        if (dcache_patch(job->ino, job->ci, (uint32_t)off_in,
                         (const uint8_t *)job->buf + src_off,
                         (uint32_t)wr_len) == 0) {
            job->rc = EFS_OK;
            job->deferred = 1;
            return NULL;
        }
    }

    /* dd if=/dev/zero / full-chunk zeros: skip assemble/encode/malloc and PUT
     * shared zero pages with the cached zero digest. */
    if (covers_full) {
        size_t src_off = (size_t)(wr_start - job->offset);
        if (efs_bytes_are_zero(job->buf + src_off, chunk_size)) {
            const uint8_t *z = efs_zero_bytes(frag_len);
            if (!z) {
                job->rc = EFS_ERR_NOMEM;
                return NULL;
            }
            const uint8_t *cfrags[EFS_NUM_FRAGMENTS] = {z, z, z};
            efs_hash_zero_fragment_len(frag_len, job->checksums[0]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[1]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[2]);
            efs_place_fragments(g_client.nodes, g_client.node_count, job->ino,
                                job->ci, job->nodes);
            job->rc = efs_client_put_fragments_parallel(
                job->ino, job->ci, job->nodes, cfrags, frag_len, job->checksums);
            return NULL;
        }

        /* Full overwrite: encode in place from the caller's buffer — skip the
         * extra 128 KiB assemble memcpy that showed up as 8–10% of seq write. */
        uint8_t *parity = malloc(frag_len);
        if (!parity) {
            job->rc = EFS_ERR_NOMEM;
            return NULL;
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
        free(parity);
        efs_rdcache_invalidate(job->ino, job->ci);
        return NULL;
    }

    /* Heap-allocate — large chunks exceed some FUSE/pthread stacks when the
     * worker runs inline on a FUSE thread (pthread_create failed / batch=1). */
    uint8_t *chunk = malloc(chunk_size);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (!chunk || !frag_buf) {
        free(chunk);
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
        free(chunk);
        free(frag_buf);
        job->rc = arc;
        return NULL;
    }

    /* Partial chunk: hold the assembled bytes and skip encode/hash/PUT.
     * Close / fsync persist. Collision: PUT this write now (do not evict). */
    if (!covers_full) {
        int st = dcache_store_owned(job->ino, job->ci, chunk, chunk_size);
        if (st == 0) {
            job->rc = EFS_OK;
            job->deferred = 1;
            free(frag_buf);
            return NULL;
        }
        if (st < 0) {
            job->rc = st;
            free(chunk);
            free(frag_buf);
            return NULL;
        }
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
    free(chunk);
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
        if (efs_export_needs_chunk_grow(&g_client.export)) {
            efs_client_table_lock();
            (void)efs_export_reserve_chunks(&g_client.export, 64);
            efs_client_table_unlock();
        }
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

        uint8_t *chunk = malloc(chunk_size);
        uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        if (!chunk || !frag_buf) {
            free(chunk);
            free(frag_buf);
            return EFS_ERR_NOMEM;
        }
        frag_ptrs(frag_buf, frag_len, frags);
        int from_zero = 0;
        int arc = assemble_write_chunk(ino, old_size, offset, buf,
                                       chunk_start, wr_start, wr_end,
                                       chunk_size, chunk, &from_zero);
        if (arc != EFS_OK) {
            free(chunk);
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
        free(chunk);
        free(frag_buf);
        if (rc != EFS_OK)
            return rc;

        if (efs_export_needs_chunk_grow(&g_client.export)) {
            efs_client_table_lock();
            (void)efs_export_reserve_chunks(&g_client.export, 64);
            efs_client_table_unlock();
        }
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
    efs_ino_t parent = inode.parent;
    int packed = (inode.pack_ino != 0);
    struct efs_chunk_entry ce;
    int has_chunk = (efs_export_get_chunk(&g_client.export, ino, 0, &ce) == 0);
    efs_client_unlock_dir(ino);

    uint32_t chunk_size = data_chunk_size();
    uint64_t end = offset + size;
    if (g_client.meta_batch && !packed && !has_chunk &&
        end <= chunk_size &&
        pack_stage_append(ino, parent, offset, size, buf, chunk_size) == EFS_OK) {
        efs_client_lock_dir(ino);
        if (efs_export_get_inode(&g_client.export, ino, &inode) == 0 &&
            inode.size < end)
            efs_export_set_size_norollup(&g_client.export, ino, end);
        efs_client_mark_ino_dirty(ino);
        efs_client_unlock_dir(ino);
        return EFS_OK;
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
            if (efs_export_needs_chunk_grow(&g_client.export)) {
                efs_client_table_lock();
                (void)efs_export_reserve_chunks(&g_client.export, 64);
                efs_client_table_unlock();
            }
            efs_client_lock_dir(ino);
            pthread_mutex_lock(&g_client.idx_mu);
            efs_export_set_chunk(&g_client.export, ino, jobs[i].ci, jobs[i].nodes,
                                 jobs[i].checksums);
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_unlock_dir(ino);
            efs_client_mark_chunk_dirty(ino, jobs[i].ci);
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
