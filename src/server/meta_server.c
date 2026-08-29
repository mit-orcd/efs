#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Snapshot peer by id under lock (do not return a live s->nodes pointer). */
static int copy_node_by_id(struct efsd_server *s, efs_node_id_t id,
                           struct efs_node *out)
{
    if (!s || !out)
        return -1;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (s->nodes[i].id == id) {
            *out = s->nodes[i];
            pthread_mutex_unlock(&s->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return -1;
}

static uint64_t heal_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void heal_prog_begin(struct efsd_server *s, const char *name,
                            uint32_t shard, uint32_t pages, uint64_t gen)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->heal_active = 1;
    memset(s->heal_export, 0, sizeof(s->heal_export));
    if (name)
        strncpy(s->heal_export, name, sizeof(s->heal_export) - 1);
    s->heal_shard = shard;
    s->heal_pages_done = 0;
    s->heal_pages_total = pages;
    s->heal_gen = gen;
    s->heal_started_us = heal_mono_us();
    s->heal_last_us = s->heal_started_us;
    pthread_mutex_unlock(&s->lock);
}

static void heal_prog_page(struct efsd_server *s, uint32_t done)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->heal_pages_done = done;
    s->heal_last_us = heal_mono_us();
    pthread_mutex_unlock(&s->lock);
}

static void heal_prog_end(struct efsd_server *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->heal_active = 0;
    pthread_mutex_unlock(&s->lock);
}

static uint32_t server_live_ids_locked(struct efsd_server *s,
                                       efs_node_id_t *live);

void server_fill_heal_status(struct efsd_server *s,
                             struct efs_msg_heal_status_reply *r)
{
    memset(r, 0, sizeof(*r));
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    uint32_t n = s->export_count;
    if (n > EFS_MAX_EXPORTS)
        n = EFS_MAX_EXPORTS;
    r->export_count = n;
    uint64_t now = heal_mono_us();
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = server_live_ids_locked(s, live);
    for (uint32_t e = 0; e < n; e++) {
        struct efs_export *ex = &s->exports[e];
        struct efs_msg_heal_status_export *o = &r->exports[e];
        strncpy(o->name, ex->name, EFS_MAX_NAME - 1);
        o->gen = ex->root.generation;
        /* Only tables this node owns. Counting every extra-shard pointer
         * on the primary made status stick at "healing tables 6/8 dirty"
         * while gen advanced — those extras live on other owners. */
        uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
        uint32_t total = 0, need = 0;
        int own0 = (sc <= 1 ||
                    efs_shard_owner_of(0, sc, live, nlive) == s->id);
        if (own0) {
            total++;
            if (ex->meta_fragmented && ex->root.page_count > 0 &&
                ex->meta_needs_rebuild)
                need++;
        }
        if (ex->shard_tabs) {
            for (uint32_t i = 1; i < ex->shard_tab_cap &&
                 i < EFS_META_MAX_SHARDS; i++) {
                struct efs_export *tab = ex->shard_tabs[i];
                if (!tab)
                    continue;
                if (sc > 1 &&
                    efs_shard_owner_of(i, sc, live, nlive) != s->id)
                    continue;
                total++;
                if (tab->meta_needs_rebuild && tab->root.page_count > 0)
                    need++;
            }
        }
        o->tables_total = total;
        o->tables_need = need;
        if (need)
            o->flags |= EFS_HEAL_F_TABLES;
        if (s->heal_active &&
            (s->heal_export[0] == '\0' ||
             strcmp(s->heal_export, ex->name) == 0)) {
            o->flags |= EFS_HEAL_F_REBUILD;
            o->cur_shard = s->heal_shard;
            o->pages_done = s->heal_pages_done;
            o->pages_total = s->heal_pages_total;
            if (s->heal_started_us && now >= s->heal_started_us)
                o->elapsed_us = now - s->heal_started_us;
        }
        if (o->flags)
            r->healing = 1;
    }
    pthread_mutex_unlock(&s->lock);
}

static int server_get_fragment_on_fd(int fd, efs_export_id_t export_id, efs_ino_t ino,
                                    uint32_t chunk_index, uint32_t fragment_index,
                                    uint8_t *data, uint8_t *checksum)
{
    struct efs_msg_get_chunk req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.chunk_index = chunk_index;
    req.fragment_index = fragment_index;

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = EFS_ERR_NET;

    if (efs_send_msg(fd, EFS_MSG_GET_CHUNK, &req, sizeof(req)) == 0 &&
        efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
        type == EFS_MSG_GET_CHUNK_REPLY && reply_len >= 1) {
        uint8_t *r = reply;
        if (r[0] == EFS_GET_CHUNK_OK &&
            reply_len == 1 + EFS_HASH_SIZE + EFS_META_FRAGMENT_SIZE) {
            memcpy(checksum, r + 1, EFS_HASH_SIZE);
            memcpy(data, r + 1 + EFS_HASH_SIZE, EFS_META_FRAGMENT_SIZE);
            rc = EFS_OK;
        } else if (r[0] == EFS_GET_CHUNK_NOT_FOUND) {
            rc = EFS_ERR_NOT_FOUND;
        } else {
            rc = EFS_ERR_IO;
        }
    }

    free(reply);
    return rc;
}

static int server_get_fragment_from_peer(const char *host, uint16_t port,
                                         efs_export_id_t export_id, efs_ino_t ino,
                                         uint32_t chunk_index, uint32_t fragment_index,
                                         uint8_t *data, uint8_t *checksum)
{
    int fd = server_peer_conn_get(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    int rc = server_get_fragment_on_fd(fd, export_id, ino, chunk_index,
                                       fragment_index, data, checksum);
    if (rc == EFS_ERR_NET)
        server_peer_conn_drop(host, port, fd);
    else
        server_peer_conn_release(host, port, fd);
    return rc;
}

static int server_put_fragment_to_peer(const char *host, uint16_t port,
                                       efs_export_id_t export_id, efs_ino_t ino,
                                       uint32_t chunk_index, uint32_t fragment_index,
                                       const uint8_t *data, const uint8_t *checksum)
{
    int fd = server_peer_conn_get(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    size_t msg_len = sizeof(struct efs_msg_put_chunk) + EFS_META_FRAGMENT_SIZE;
    uint8_t *msg = malloc(msg_len);
    if (!msg) {
        server_peer_conn_release(host, port, fd);
        return EFS_ERR_NOMEM;
    }
    struct efs_msg_put_chunk *req = (struct efs_msg_put_chunk *)msg;
    memset(req, 0, sizeof(*req));
    req->export_id = export_id;
    req->ino = ino;
    req->chunk_index = chunk_index;
    req->fragment_index = fragment_index;
    memcpy(req->checksum, checksum, EFS_HASH_SIZE);
    req->data_len = EFS_META_FRAGMENT_SIZE;
    memcpy(msg + sizeof(*req), data, EFS_META_FRAGMENT_SIZE);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = EFS_ERR_NET;

    if (efs_send_msg(fd, EFS_MSG_PUT_CHUNK, msg, (uint32_t)msg_len) == 0 &&
        efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
        type == EFS_MSG_PUT_CHUNK_REPLY && reply_len == 1) {
        uint8_t *r = reply;
        if (r[0] == EFS_PUT_CHUNK_OK)
            rc = EFS_OK;
        else if (r[0] == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
            rc = EFS_ERR_QUOTA;
        else
            rc = EFS_ERR_IO;
    }

    free(msg);
    free(reply);
    if (rc == EFS_ERR_NET)
        server_peer_conn_drop(host, port, fd);
    else
        server_peer_conn_release(host, port, fd);
    return rc;
}

static int fetch_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                               efs_ino_t table_ino, efs_node_id_t node_id,
                               uint32_t chunk_index, uint32_t fragment_index,
                               uint8_t *data)
{
    uint8_t checksum[EFS_HASH_SIZE];
    if (node_id == s->id) {
        uint32_t len = 0;
        if (server_read_fragment(s, ex, table_ino, chunk_index,
                                 fragment_index, data, &len) == EFS_OK)
            return EFS_OK;
    } else {
        struct efs_node n;
        if (copy_node_by_id(s, node_id, &n) == 0 &&
            server_get_fragment_from_peer(n.addr, n.port, ex->id,
                                          table_ino, chunk_index,
                                          fragment_index, data, checksum) == EFS_OK)
            return EFS_OK;
    }

    /* Placement can be stale: try every other member (and local if the
     * hint was a peer). */
    struct efs_node snap[EFS_MAX_NODES];
    uint32_t nc = 0;
    efs_node_id_t self = 0;
    pthread_mutex_lock(&s->lock);
    nc = s->node_count;
    if (nc > EFS_MAX_NODES)
        nc = EFS_MAX_NODES;
    memcpy(snap, s->nodes, sizeof(struct efs_node) * nc);
    self = s->id;
    pthread_mutex_unlock(&s->lock);

    for (uint32_t i = 0; i < nc; i++) {
        if (snap[i].id == 0 || snap[i].id == node_id)
            continue;
        if (snap[i].id == self) {
            uint32_t len = 0;
            if (server_read_fragment(s, ex, table_ino, chunk_index,
                                     fragment_index, data, &len) == EFS_OK)
                return EFS_OK;
            continue;
        }
        if (server_get_fragment_from_peer(snap[i].addr, snap[i].port, ex->id,
                                          table_ino, chunk_index,
                                          fragment_index, data, checksum) == EFS_OK)
            return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

static void gc_region_pages(struct efsd_server *s, struct efs_export *ex,
                            efs_ino_t table_ino, uint64_t dead_generation,
                            int region, uint32_t old_pc, uint32_t live_pc,
                            uint32_t layout_ver)
{
    uint32_t cap = (region == EFS_META_REGION_CHUNK)
                       ? (layout_ver >= 5 ? EFS_META_CHUNK_PAGE_MAX
                                          : EFS_META_V4_CHUNK_MAX)
                       : (layout_ver >= 5 ? EFS_META_INO_PAGE_MAX
                                          : EFS_META_V4_INO_MAX);
    if (old_pc > cap)
        old_pc = cap;
    if (live_pc > old_pc)
        live_pc = old_pc;
    for (uint32_t pi = live_pc; pi < old_pc; pi++) {
        uint32_t ci = efs_meta_region_page_chunk_index_ver(dead_generation, region,
                                                          pi, layout_ver);
        if (ci == UINT32_MAX)
            continue;
        efs_node_id_t placed[EFS_NUM_FRAGMENTS];
        pthread_mutex_lock(&s->lock);
        efs_place_fragments(s->nodes, s->node_count, table_ino, ci,
                            placed);
        efs_node_id_t self = s->id;
        pthread_mutex_unlock(&s->lock);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            if (placed[fi] != self)
                continue;
            server_unlink_fragment_files(s, ex, table_ino, ci,
                                         (uint32_t)fi);
        }
    }
}

void server_gc_meta_slot_pages(struct efsd_server *s, struct efs_export *ex,
                               uint64_t dead_generation,
                               uint32_t old_ino_pages, uint32_t old_chunk_pages,
                               uint32_t live_ino_pages, uint32_t live_chunk_pages)
{
    if (!s || !ex)
        return;
    /* Legacy single-space: chunk counts are 0; inode count is the old
     * packed page_count. */
    int old_v5 = old_ino_pages > EFS_META_V4_INO_MAX ||
                 old_chunk_pages > EFS_META_V4_CHUNK_MAX;
    int live_v5 = live_ino_pages > EFS_META_V4_INO_MAX ||
                  live_chunk_pages > EFS_META_V4_CHUNK_MAX;
    if (old_v5 != live_v5)
        return; /* v4↔v5 slot windows differ; leave the retired layout */
    uint32_t ver = old_v5 ? 5 : 4;
    if (old_chunk_pages == 0 && live_chunk_pages == 0) {
        gc_region_pages(s, ex, EFS_META_TABLE_INO, dead_generation,
                        EFS_META_REGION_INO, old_ino_pages, live_ino_pages, ver);
        return;
    }
    gc_region_pages(s, ex, EFS_META_TABLE_INO, dead_generation,
                    EFS_META_REGION_INO, old_ino_pages, live_ino_pages, ver);
    gc_region_pages(s, ex, EFS_META_TABLE_INO, dead_generation,
                    EFS_META_REGION_CHUNK, old_chunk_pages, live_chunk_pages, ver);
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* CoW (EFSR v7) GC: reclaim the chunk_indices the previous committed root
 * referenced but the new root no longer does. The flush writes each dirty
 * page to a fresh ci, so the dead cis are exactly old_cis[] - new page_cis[].
 * Best-effort: a missed reclaim leaks a page until a later GC, never
 * corrupts (a live ci is never unlinked — it is always in new page_cis[]). */
void server_gc_meta_cow_pages(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t table_ino,
                              const uint32_t *old_cis, uint32_t old_count,
                              const uint32_t *new_cis, uint32_t new_count)
{
    if (!s || !ex || !old_cis || !new_cis)
        return;
    uint32_t n = new_count;
    uint32_t *live = malloc((size_t)n * sizeof(uint32_t));
    if (!live)
        return;
    memcpy(live, new_cis, (size_t)n * sizeof(uint32_t));
    qsort(live, n, sizeof(uint32_t), u32_cmp);
    for (uint32_t i = 0; i < old_count; i++) {
        uint32_t ci = old_cis[i];
        if (ci < EFS_META_COW_BASE)
            continue; /* legacy dual-slot / never-written slot */
        if (bsearch(&ci, live, n, sizeof(uint32_t), u32_cmp))
            continue; /* still referenced by the new root */
        efs_node_id_t placed[EFS_NUM_FRAGMENTS];
        pthread_mutex_lock(&s->lock);
        /* Re-check against the LIVE committed root, not just the new_cis
         * snapshot the caller took: a concurrent PUT_META can re-adopt a
         * root that still references this ci (e.g. a stale gen re-adopted
         * after a newer gen's GC already ran), or the root can advance and
         * carry the ci forward via page reuse. Reclaiming a ci the committed
         * root still references makes that page unrecoverable on the next
         * rebuild (gen raced with GC, permanent shard wedge). A missed
         * reclaim only leaks a page until a later GC — never corrupts. */
        int still_live = 0;
        if (ex->root.page_cis) {
            for (uint32_t p = 0; p < ex->root.page_count; p++) {
                if (ex->root.page_cis[p] == ci) {
                    still_live = 1;
                    break;
                }
            }
        }
        if (!still_live)
            efs_place_fragments(s->nodes, s->node_count, table_ino, ci, placed);
        efs_node_id_t self = s->id;
        uint64_t live_gen = ex->root.generation;
        pthread_mutex_unlock(&s->lock);
        if (still_live) {
            fprintf(stderr,
                    "meta-gc: SKIP live ci=%u table_ino=%llu (still in "
                    "committed root gen=%llu)\n",
                    ci, (unsigned long long)table_ino,
                    (unsigned long long)live_gen);
            continue;
        }
        fprintf(stderr,
                "meta-gc: reclaim ci=%u table_ino=%llu (committed gen=%llu)\n",
                ci, (unsigned long long)table_ino, (unsigned long long)live_gen);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            if (placed[fi] != self)
                continue;
            server_unlink_fragment_files(s, ex, table_ino, ci, (uint32_t)fi);
        }
    }
    free(live);
}

static int put_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                             efs_ino_t table_ino, efs_node_id_t node_id,
                             uint32_t chunk_index, uint32_t fragment_index,
                             const uint8_t *data, const uint8_t *checksum);
static int server_rebuild_export_from_pages_ino(struct efsd_server *s,
                                                struct efs_export *ex,
                                                efs_ino_t table_ino);

/* Meta heal coordinator: the lowest-numbered live node pushes reconstructed
 * fragments to peer-owned slots. Every server rebuilds concurrently, so
 * without a single coordinator all nodes would duplicate the same heal PUTs. */
static int server_is_meta_heal_coordinator(struct efsd_server *s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ull +
                      (uint64_t)ts.tv_nsec / 1000000ull;
    int coord = 1;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < s->node_count; i++) {
        struct efs_node *n = &s->nodes[i];
        if (n->id == s->id)
            continue;
        if (n->id < s->id &&
            (n->down_until_ms == 0 || n->down_until_ms <= now_ms)) {
            coord = 0;
            break;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return coord;
}

static uint32_t server_live_ids_locked(struct efsd_server *s, efs_node_id_t *live)
{
    uint32_t n = 0;
    if (!s || !live)
        return 0;
    live[n++] = s->id;
    for (uint32_t i = 0; i < s->node_count && n < EFS_MAX_NODES; i++) {
        efs_node_id_t id = s->nodes[i].id;
        if (id == 0 || id == s->id)
            continue;
        if (server_node_is_down_locked(s, id))
            continue;
        live[n++] = id;
    }
    return n;
}

/* Materialize extra-shard tables only for shards this node owns. Peers keep
 * the v8 descriptors and the pages; they do not assemble those tables. */
static void server_rebuild_owned_extras(struct efsd_server *s,
                                        struct efs_export *ex)
{
    if (!s || !ex || !ex->root.shard_bits)
        return;
    uint32_t n = 0;
    uint32_t ids[EFS_META_MAX_SHARDS];
    pthread_mutex_lock(&s->lock);
    n = ex->root.extra_shard_count;
    if (n > EFS_META_MAX_SHARDS)
        n = EFS_META_MAX_SHARDS;
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = server_live_ids_locked(s, live);
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    uint32_t want = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sh = ex->root.extra_shard_ids ? ex->root.extra_shard_ids[i] : 0;
        if (!sh)
            continue;
        if (efs_shard_owner_of(sh, sc, live, nlive) != s->id)
            continue;
        ids[want++] = sh;
    }
    pthread_mutex_unlock(&s->lock);
    for (uint32_t i = 0; i < want; i++) {
        uint32_t sh = ids[i];
        pthread_mutex_lock(&s->lock);
        struct efs_export *tab = efs_export_table(ex, sh);
        /* Never rebuild a dirty shard table: this server owns it (single
         * writer), so the dirty table is ahead of the committed root and a
         * rebuild would drop the ACKED-but-uncommitted ops (DIRTY-REBUILD). */
        int need = tab && tab->root.page_count > 0 && !tab->shard_dirty &&
                   (tab->meta_needs_rebuild || tab->inode_count == 0);
        pthread_mutex_unlock(&s->lock);
        if (!need)
            continue;
        int rc = server_rebuild_export_from_pages_ino(
            s, tab, efs_meta_shard_table_ino(sh));
        if (rc == EFS_OK)
            fprintf(stderr, "meta-catchup: rebuilt export=%s shard=%u (owned)\n",
                    ex->name, sh);
    }
}

int server_ensure_shard_ready(struct efsd_server *s, struct efs_export *ex,
                              uint32_t shard)
{
    if (!s || !ex)
        return -1;
    /* Caller holds s->lock. */
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (ex->root.shard_bits == 0 || sc <= 1 || shard == 0) {
        /* Same DIRTY-REBUILD rule the extra-shard path below has always had,
         * which the main table was missing. A dirty main table holds ops this
         * server has already ACKed but not yet committed, so it is AHEAD of
         * the committed root; rebuilding from that root drops them.
         *
         * That is the post-mkfs data loss: while joiners churn through
         * rebuilds, shard 0 got rebuilt out from under acknowledged work, so
         * creates vanished (a directory came back with 3 of 500 files), chunk
         * mappings vanished (correct size, all-zero contents) -- and because
         * the rebuild also restores next_ino from the older root, the
         * allocator handed out inos that were already live
         * (`cwi-fail: ino_dup ... shard=0`), which surfaced to applications as
         * EEXIST on a brand-new name. fsync did not help: the report had
         * already succeeded before the rebuild threw the row away. */
        int need = ex->meta_fragmented && ex->root.page_count > 0 &&
                   !ex->shard_dirty &&
                   (ex->meta_needs_rebuild ||
                    (ex->inode_count <= 1 && ex->root.blob_len > (1u << 20)));
        if (!need)
            return 0;
        pthread_mutex_unlock(&s->lock);
        int rc = server_rebuild_export_from_pages(s, ex);
        pthread_mutex_lock(&s->lock);
        return (rc == EFS_OK && !ex->meta_needs_rebuild) ? 0 : -1;
    }
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = server_live_ids_locked(s, live);
    if (efs_shard_owner_of(shard, sc, live, nlive) != s->id)
        return -1;
    struct efs_export *tab = efs_export_table(ex, shard);
    if (!tab)
        return -1;
    /* Never flushed: an empty extra is the truth, first create is fine. */
    if (tab->root.page_count == 0)
        return 0;
    /* Dirty: the table holds ACKED-but-uncommitted ops. This server owns the
     * shard (single writer), so the dirty table is AHEAD of the committed
     * root, never behind it — rebuilding from the committed root would only
     * drop the uncommitted ops (DIRTY-REBUILD). Keep it; the flush commits. */
    if (tab->shard_dirty)
        return 0;
    if (!tab->meta_needs_rebuild && tab->inode_count > 0)
        return 0;
    pthread_mutex_unlock(&s->lock);
    int rc = server_rebuild_export_from_pages_ino(
        s, tab, efs_meta_shard_table_ino(shard));
    pthread_mutex_lock(&s->lock);
    if (rc == EFS_OK)
        fprintf(stderr, "meta-ensure: rebuilt export=%s shard=%u (on-demand)\n",
                ex->name, shard);
    return (rc == EFS_OK && tab && !tab->meta_needs_rebuild) ? 0 : -1;
}

int server_rebuild_export_from_pages(struct efsd_server *s, struct efs_export *ex)
{
    return server_rebuild_export_from_pages_ino(s, ex, EFS_META_TABLE_INO);
}

/* Per-shard partition (blocker 1): the rebuild's table swap must be
 * serialized with handlers that will hold per-shard locks. Take the export's
 * shard locks in global->shard order (caller holds s->lock). A main-table
 * rebuild frees/reinstalls EVERY shard table (efs_export_free drops
 * ex->shard_tabs), so it locks all sc shards; an extra-shard rebuild touches
 * only its own table, so it locks just that shard. The page fetch above runs
 * with NO shard lock held (it is network I/O). Returns the export index whose
 * locks were taken (>=0), or -1 to fall back to s->lock only. */
static int rebuild_shards_lock(struct efsd_server *s, struct efs_export *ex,
                               efs_ino_t table_ino, uint32_t *sc_out)
{
    int meidx;
    if (table_ino == EFS_META_TABLE_INO) {
        meidx = server_export_index_locked(s, ex);
    } else {
        /* The shard table is heap-allocated (not in s->exports[]); find the
         * main export by id (efs_export_init_empty_table copies it). */
        meidx = -1;
        for (uint32_t i = 0; i < s->export_count; i++)
            if (s->exports[i].id == ex->id) {
                meidx = (int)i;
                break;
            }
    }
    if (meidx < 0)
        return -1;
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (table_ino == EFS_META_TABLE_INO)
        server_shard_lock_all(s, (uint32_t)meidx, sc);
    else
        server_shard_lock(s, (uint32_t)meidx, ex->shard_id);
    *sc_out = sc;
    return meidx;
}

static void rebuild_shards_unlock(struct efsd_server *s, int meidx,
                                  struct efs_export *ex, efs_ino_t table_ino,
                                  uint32_t sc)
{
    if (meidx < 0)
        return;
    if (table_ino == EFS_META_TABLE_INO)
        server_shard_unlock_all(s, (uint32_t)meidx, sc);
    else
        server_shard_unlock(s, (uint32_t)meidx, ex->shard_id);
}

static int server_rebuild_export_from_pages_ino(struct efsd_server *s,
                                                struct efs_export *ex,
                                                efs_ino_t table_ino)
{
    if (!s || !ex || !ex->meta_fragmented)
        return EFS_ERR_INVAL;

    /* Deep-copy the EFSR under lock so concurrent PUT_META cannot free
     * page_checksums under this thread (UAF → bogus decode / SIGSEGV). */
    struct efs_export_root snap;
    memset(&snap, 0, sizeof(snap));
    pthread_mutex_lock(&s->lock);
    if (!ex->meta_fragmented || ex->root.page_count == 0 ||
        ex->root.blob_len == 0 || !ex->root.page_checksums ||
        ex->root.page_count > EFS_META_MAX_PAGES) {
        pthread_mutex_unlock(&s->lock);
        return EFS_ERR_INVAL;
    }
    int ei = server_export_index_locked(s, ex);
    int crc = efs_export_root_copy(&snap, &ex->root);
    /* Extra-shard tables get their own incremental cache slot (the main
     * meta_blob_cache is per-export and would be clobbered by shard rebuilds). */
    uint32_t shard_slot = 0;
    if (table_ino != EFS_META_TABLE_INO && ex->shard_id > 0 &&
        ex->shard_id < EFS_META_MAX_SHARDS)
        shard_slot = ex->shard_id;
    /* DIAG: rebuilding a DIRTY shard table from committed pages drops the
     * uncommitted ops (the live table is newer than the pages). Log loudly
     * so we can confirm/deny this as the source of the parallel
     * dir_many_files "one unlink lost" flake. */
    if (table_ino != EFS_META_TABLE_INO && ex->shard_dirty)
        fprintf(stderr,
                "DIRTY-REBUILD: export=%s table_ino=%llu shard=%u gen=%llu "
                "inodes=%llu — rebuilding a dirty shard table drops "
                "uncommitted ops\n",
                ex->name, (unsigned long long)table_ino, ex->shard_id,
                (unsigned long long)ex->root.generation,
                (unsigned long long)ex->inode_count);
    pthread_mutex_unlock(&s->lock);
    if (crc != EFS_OK)
        return crc;

    fprintf(stderr,
            "meta-catchup: rebuilding export=%s table_ino=%llu gen=%llu "
            "pages=%u inodes=%llu\n",
            ex->name, (unsigned long long)table_ino,
            (unsigned long long)snap.generation, snap.page_count,
            (unsigned long long)ex->inode_count);

    const struct efs_export_root *root = &snap;
    const uint64_t start_gen = root->generation;
    const uint32_t page_count = root->page_count;
    int saw_v4_ci = 0, saw_v5_ci = 0;

    uint8_t (*pages)[EFS_META_PAGE_SIZE] =
        calloc(page_count, EFS_META_PAGE_SIZE);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    if (!pages || !frag_buf) {
        free(pages);
        free(frag_buf);
        efs_export_root_free(&snap);
        return EFS_ERR_NOMEM;
    }
    uint8_t *fragments[EFS_NUM_FRAGMENTS] = {
        frag_buf, frag_buf + EFS_META_FRAGMENT_SIZE,
        frag_buf + 2 * EFS_META_FRAGMENT_SIZE
    };
    uint32_t holed = 0;
    heal_prog_begin(s, ex->name, shard_slot, page_count, start_gen);

    for (uint32_t pi = 0; pi < page_count; pi++) {
        /* Early-abort on a stale root: if a concurrent flush advanced the
         * generation, our snapshot's checksums no longer match the same-slot
         * fragments being written, producing a mismatch storm. Bail out as
         * PROTO so the catch-up loop re-polls the newest root and retries,
         * instead of fetching every remaining page against a dead gen. */
        pthread_mutex_lock(&s->lock);
        uint64_t cur_gen = ex->root.generation;
        /* Cache hit: the page's fragment checksums are unchanged from the
         * last rebuild's generation, so the assembled-blob cache already
         * holds exactly this page's content — memcpy instead of peer fetch.
         * Shard tables use their per-shard slot; the main table uses the
         * per-export slot. */
        int cached = 0;
        uint8_t *cache = NULL;
        uint32_t cache_len = 0;
        const uint8_t *sums = NULL;
        uint32_t cache_pages = 0;
        if (ei >= 0) {
            if (shard_slot) {
                cache = s->shard_blob_cache[ei][shard_slot];
                cache_len = s->shard_blob_cache_len[ei][shard_slot];
                sums = s->shard_blob_sums[ei][shard_slot];
                cache_pages = s->shard_blob_pages[ei][shard_slot];
            } else {
                cache = s->meta_blob_cache[ei];
                cache_len = s->meta_blob_cache_len[ei];
                sums = s->meta_blob_sums[ei];
                cache_pages = s->meta_blob_pages[ei];
            }
        }
        if (cur_gen == start_gen && cache && sums &&
            pi < cache_pages &&
            memcmp(sums + (size_t)pi * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                   efs_export_root_checksum_const(root, pi, 0),
                   EFS_NUM_FRAGMENTS * EFS_HASH_SIZE) == 0) {
            size_t off = (size_t)pi * EFS_META_PAGE_SIZE;
            size_t avail = (off < cache_len) ? cache_len - off : 0;
            if (avail > 0) {
                size_t n = avail > EFS_META_PAGE_SIZE ? EFS_META_PAGE_SIZE
                                                      : avail;
                memcpy(pages[pi], cache + off, n);
                if (n < EFS_META_PAGE_SIZE)
                    memset(pages[pi] + n, 0, EFS_META_PAGE_SIZE - n);
                cached = 1;
            }
        }
        pthread_mutex_unlock(&s->lock);
        if (cur_gen != start_gen) {
            free(frag_buf);
            free(pages);
            efs_export_root_free(&snap);
            heal_prog_end(s);
            return EFS_ERR_PROTO;
        }
        heal_prog_page(s, pi + 1);
        if (cached)
            continue;

        /* Advertised layout, then the other v4↔v5 window, then legacy pi.
         * A save used to rewrite v4 roots as v5 without moving pages.
         * Also try the previous generation's dual-slot: the flush PUTs only
         * dirty pages, so pages clean at start_gen still live at gen-1 (or
         * older, same parity) slots while the flipped EFSR matches their
         * content. The client read path already does this; the rebuild
         * wedged on restarts without it. */
        uint32_t try_ci[6];
        int ntry;
        if (efs_export_root_is_cow(root)) {
            /* CoW (EFSR v7): the root carries each page's exact chunk_index.
             * Read it directly — no dual-slot / v4-window candidate search.
             * An interrupted flush never overwrote this ci (the flush writes
             * dirty pages to fresh cis and only then flips the root), so the
             * fragments here always match the committed checksums. */
            try_ci[0] = root->page_cis[pi];
            ntry = 1;
        } else {
            ntry = efs_meta_page_ci_candidates(start_gen, root->version,
                                               root->ino_page_count,
                                               root->chunk_page_count, pi,
                                               try_ci);
            if (start_gen > 0) {
                uint32_t alt[3];
                int na = efs_meta_page_ci_candidates(start_gen - 1,
                                                     root->version,
                                                     root->ino_page_count,
                                                     root->chunk_page_count,
                                                     pi, alt);
                for (int i = 0; i < na; i++) {
                    int seen = 0;
                    for (int j = 0; j < ntry; j++)
                        if (try_ci[j] == alt[i])
                            seen = 1;
                    if (!seen && ntry < 6)
                        try_ci[ntry++] = alt[i];
                }
            }
        }

        int decoded = 0;
        for (int ti = 0; ti < ntry && !decoded; ti++) {
            uint32_t ci = try_ci[ti];
            uint32_t ci_layout = efs_meta_page_ci_layout(
                start_gen, root->ino_page_count, root->chunk_page_count, pi,
                ci);
            const char *ci_scheme = ci_layout == 5
                                        ? "v5"
                                        : (ci_layout == 4 ? "v4" : "legacy");
            efs_node_id_t placed[EFS_NUM_FRAGMENTS];
            pthread_mutex_lock(&s->lock);
            efs_place_fragments(s->nodes, s->node_count, table_ino, ci,
                                placed);
            efs_node_id_t self = s->id;
            pthread_mutex_unlock(&s->lock);

            int have[EFS_NUM_FRAGMENTS] = {0};

            for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
                int frc = fetch_meta_fragment(s, ex, table_ino,
                                              placed[fi], ci, (uint32_t)fi,
                                              fragments[fi]);
                if (frc == EFS_OK) {
                    uint8_t sum[EFS_HASH_SIZE];
                    efs_hash(fragments[fi], EFS_META_FRAGMENT_SIZE, sum);
                    if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                               EFS_HASH_SIZE) == 0)
                        have[fi] = 1;
                    else
                        fprintf(stderr,
                                "meta-rebuild: page %u frag %u checksum mismatch "
                                "(node %u ci=%u scheme=%s)\n",
                                pi, fi, placed[fi], ci, ci_scheme);
                } else {
                    fprintf(stderr,
                            "meta-rebuild: page %u frag %u fetch failed rc=%d "
                            "(node %u export=%u ci=%u)\n",
                            pi, fi, frc, placed[fi], ex->id, ci);
                }
            }

            int missing = -1, a = -1, b = -1;
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!have[i]) {
                    if (missing < 0)
                        missing = i;
                } else if (a < 0) {
                    a = i;
                } else if (b < 0) {
                    b = i;
                }
            }
            if (missing < 0) {
                for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                    if (i != a && i != b) {
                        missing = i;
                        break;
                    }
                }
            }
            if (a >= 0 && b >= 0 && missing >= 0 &&
                efs_decode_chunk(fragments, EFS_META_PAGE_SIZE, a, b, missing,
                                 pages[pi], EFS_META_PAGE_SIZE) == 0) {
                decoded = 1;
                if (ci_layout == 4)
                    saw_v4_ci = 1;
                else if (ci_layout == 5)
                    saw_v5_ci = 1;
                if (ci_layout != 0 &&
                    ci_layout != (root->version >= 5 ? 5u : 4u))
                    fprintf(stderr,
                            "meta-rebuild: page %u loaded via v%u "
                            "chunk_index (root claimed v%u gen=%llu)\n",
                            pi, ci_layout, root->version,
                            (unsigned long long)start_gen);

                /* Server-side heal, not client-triggered: rewrite every
                 * missing/corrupt fragment of this page. Self-owned frags are
                 * written locally; peer-owned holes are pushed by the single
                 * heal coordinator (lowest live node id) so concurrent
                 * rebuilds on all servers don't duplicate the same PUTs. */
                int coord = -1; /* computed lazily */
                int need_heal = 0;
                for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
                    if (!have[fi]) {
                        need_heal = 1;
                        break;
                    }
                }
                if (need_heal &&
                    efs_encode_chunk(pages[pi], EFS_META_PAGE_SIZE,
                                     EFS_META_PAGE_SIZE, fragments) == 0) {
                    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
                        if (have[fi])
                            continue;
                        if (placed[fi] != self) {
                            if (coord < 0)
                                coord = server_is_meta_heal_coordinator(s);
                            if (!coord)
                                continue;
                        }
                        const uint8_t *csum =
                            efs_export_root_checksum_const(root, pi, fi);
                        int hrc = put_meta_fragment(s, ex, table_ino,
                                                    placed[fi], ci, (uint32_t)fi,
                                                    fragments[fi], csum);
                        if (hrc == EFS_OK)
                            fprintf(stderr,
                                    "meta-heal: export=%s page=%u fi=%u ci=%u "
                                    "node=%u\n",
                                    ex->name, pi, fi, ci, placed[fi]);
                        else
                            fprintf(stderr,
                                    "meta-heal: failed export=%s page=%u fi=%u "
                                    "ci=%u node=%u rc=%d\n",
                                    ex->name, pi, fi, ci, placed[fi], hrc);
                    }
                }
            } else if (ti + 1 >= ntry) {
                fprintf(stderr,
                        "meta-rebuild: decode failed page %u have=%d%d%d "
                        "nodes=%u,%u,%u node_count=%u ci=%u\n",
                        pi, have[0], have[1], have[2], placed[0], placed[1],
                        placed[2], s->node_count, ci);
            }
        }
        if (!decoded) {
            /* CoW (EFSR v7): an unrecoverable page is a catchup-vs-GC race,
             * not genuine loss — with the cluster up, all of a CoW page's
             * fragments exist unless a peer committed a newer root and its GC
             * reclaimed this generation's dead cis (2+1 EC tolerates one
             * fragment loss, so genuine loss needs 2+ nodes down). Zero-fill
             * would corrupt the table (garbage next_ino, rebuild loop). Bail
             * out as PROTO so the catchup re-polls the newest root (whose cis
             * are live) and retries. */
            if (efs_export_root_is_cow(root)) {
                fprintf(stderr,
                        "meta-rebuild: page %u unrecoverable (CoW ci=%u); "
                        "gen %llu raced with GC, re-polling (not zero-fill)\n",
                        pi, try_ci[0], (unsigned long long)start_gen);
                free(frag_buf);
                free(pages);
                efs_export_root_free(&snap);
                heal_prog_end(s);
                return EFS_ERR_PROTO;
            }
            /* Hopeless page (no candidate CI yields 2 checksum-matching
             * fragments): zero-fill and keep rebuilding the rest. Aborting
             * the whole rebuild here fenced the export's tables forever and
             * re-ran the same doomed O(pages) pass on every trigger — the
             * meta-rebuild livelock. The hole is bounded to this page; a
             * later client flush that still has the real content re-publishes
             * the page and overwrites the zeros. */
            memset(pages[pi], 0, EFS_META_PAGE_SIZE);
            holed++;
            fprintf(stderr,
                    "meta-rebuild: page %u unrecoverable across all nodes; "
                    "zero-filled (gen=%llu)\n",
                    pi, (unsigned long long)start_gen);
        }
    }
    free(frag_buf);
    if (holed)
        fprintf(stderr,
                "meta-rebuild: export=%s gen=%llu completed with %u "
                "zero-filled page(s)\n",
                ex->name, (unsigned long long)start_gen, holed);

    char *blob = NULL;
    size_t blob_len = 0;
    int rc = efs_meta_assemble_blob(root, pages, &blob, &blob_len);
    free(pages);
    if (rc != EFS_OK) {
        efs_export_root_free(&snap);
        heal_prog_end(s);
        return rc;
    }

    /* Deserialize into a STAGING export without s->lock. At GiB scale this
     * parses millions of inode/chunk rows and reindexes — seconds of work
     * that, done in place under s->lock, stalled every handler (the
     * post-restart rebuild storm). The assembled blob is self-contained, so
     * staging needs no shared state; the lock is taken only to swap the
     * finished table in (pointer moves, not a reparse). */
    struct efs_export staging;
    memset(&staging, 0, sizeof(staging));
    rc = efs_export_deserialize(&staging, blob, blob_len);

    pthread_mutex_lock(&s->lock);
    /* Hold the export's shard locks across the table swap below so it is
     * serialized with (future) shard-lock-holding handlers. global->shard. */
    uint32_t shard_sc = 0;
    int shard_eidx = rebuild_shards_lock(s, ex, table_ino, &shard_sc);
    if (ex->root.generation != start_gen) {
        rebuild_shards_unlock(s, shard_eidx, ex, table_ino, shard_sc);
        pthread_mutex_unlock(&s->lock);
        efs_export_free(&staging);
        free(blob);
        efs_export_root_free(&snap);
        heal_prog_end(s);
        return EFS_ERR_PROTO;
    }
    if (rc != EFS_OK) {
        /* Staging failed; the live export keeps its previous table (better
         * than the old in-place path, which left ex half-freed). Restore the
         * EFSR from snap is unnecessary — ex->root was never touched. */
        efs_export_free(&staging);
        free(blob);
        ex->meta_fragmented = 1;
        ex->meta_needs_rebuild = 1;
        fprintf(stderr, "FENCE-SITE rebuild-deserialize-fail rc=%d\n", rc);
        rebuild_shards_unlock(s, shard_eidx, ex, table_ino, shard_sc);
        pthread_mutex_unlock(&s->lock);
        heal_prog_end(s);
        return rc;
    }

    /* Commit: free the old table's contents and move the staged pointers in.
     * efs_export_free zeroes ex (including root.page_checksums); the staged
     * root is empty (EFSM carries no EFSR), then root_move reinstalls snap.
     * Preserve shard_id: for an extra-shard table the deserialize staging
     * zeroed it, and alloc_ino keys the ino congruence class off it. */
    uint32_t saved_shard_id = ex->shard_id;
    /* The allocator watermark of the table we are about to replace. The swap
     * below discards it, and the committed root can be older, so without this
     * the rebuild reissues inos that are still live (see the next_ino floor
     * where the root is installed). */
    efs_ino_t saved_next_ino = ex->next_ino;

    /* A MAIN-table rebuild must not drop dirty shard tables. efs_export_free
     * recursively frees ex->shard_tabs; but a shard owner holds ACKED-yet-
     * unflushed CREATE_SHARD rows in its dirty shard tables (flush window
     * ~2s). Wiping one lets its alloc_ino re-hand-out those inos, and the
     * next create's parent-dentry write then collides with the orphaned
     * dentry on the dentry shard -> spurious EEXIST under parallel creates.
     * Detach dirty shard tables before the free, reattach them after the
     * staging swap. Clean shard tables are dropped and rebuilt from the new
     * root's pages by server_rebuild_owned_extras (they match committed
     * state, so rebuilding is both safe and refreshes them). */
    struct efs_export **dirty_tabs = NULL;
    uint32_t dirty_cap = 0;
    if (table_ino == EFS_META_TABLE_INO && ex->shard_tabs) {
        dirty_cap = ex->shard_tab_cap;
        dirty_tabs = calloc(dirty_cap, sizeof(*dirty_tabs));
        if (dirty_tabs) {
            for (uint32_t i = 0; i < dirty_cap; i++) {
                struct efs_export *t = ex->shard_tabs[i];
                if (t && t->shard_dirty) {
                    dirty_tabs[i] = t;        /* keep */
                    ex->shard_tabs[i] = NULL; /* efs_export_free skips NULL */
                }
            }
        }
    }
    efs_export_free(ex);
    *ex = staging;
    ex->shard_id = saved_shard_id;
    memset(&staging, 0, sizeof(staging));
    if (dirty_tabs) {
        uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
        uint32_t cap = dirty_cap > sc ? dirty_cap : sc;
        struct efs_export **arr = calloc(cap, sizeof(*arr));
        if (arr) {
            ex->shard_tabs = arr;
            ex->shard_tab_cap = cap;
            for (uint32_t i = 0; i < dirty_cap && i < cap; i++)
                if (dirty_tabs[i])
                    arr[i] = dirty_tabs[i];
        } else {
            for (uint32_t i = 0; i < dirty_cap; i++)
                if (dirty_tabs[i]) {
                    efs_export_free(dirty_tabs[i]);
                    free(dirty_tabs[i]);
                }
        }
        free(dirty_tabs);
    }

    /* Blocker 2: a main-table rebuild freed every clean shard table above.
     * Pre-create them all so later op/read paths find an existing table and
     * never do the per-export lazy-create mutation under a single shard lock.
     * Holds s->lock + all the export's shard locks (blocker 1). */
    if (table_ino == EFS_META_TABLE_INO)
        (void)efs_export_precreate_shards(ex);

    /* Success: keep the assembled blob (plus this generation's page
     * checksums) as the incremental-rebuild cache so the next rebuild only
     * fetches pages whose checksums changed. Shard tables use their
     * per-shard slot so a shard rebuild never clobbers the main table's
     * cache (and vice versa). */
    if (ei >= 0) {
        uint8_t **cachep;
        uint32_t *cache_lenp;
        uint8_t **sumsp;
        uint32_t *cache_pagesp;
        if (shard_slot) {
            cachep = &s->shard_blob_cache[ei][shard_slot];
            cache_lenp = &s->shard_blob_cache_len[ei][shard_slot];
            sumsp = &s->shard_blob_sums[ei][shard_slot];
            cache_pagesp = &s->shard_blob_pages[ei][shard_slot];
        } else {
            cachep = &s->meta_blob_cache[ei];
            cache_lenp = &s->meta_blob_cache_len[ei];
            sumsp = &s->meta_blob_sums[ei];
            cache_pagesp = &s->meta_blob_pages[ei];
            s->meta_blob_cache_gen[ei] = start_gen;
        }
        free(*cachep);
        free(*sumsp);
        *cachep = (uint8_t *)blob;
        *cache_lenp = (uint32_t)blob_len;
        *cache_pagesp = page_count;
        size_t sum_n = (size_t)page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        *sumsp = malloc(sum_n);
        if (*sumsp)
            memcpy(*sumsp, root->page_checksums, sum_n);
        else
            *cache_pagesp = 0; /* no sums: cache unusable, freed next */
        blob = NULL; /* owned by the cache */
    }
    free(blob);

    ex->meta_fragmented = 1;
    ex->meta_needs_rebuild = 0;
    efs_export_root_move(&ex->root, &snap);
    /* A poisoned v5 label on v4-resident pages must not stay v5: the next
     * GET_META / incremental flush would address the empty 32k window. */
    if (saw_v4_ci && !saw_v5_ci && ex->root.version >= 5)
        ex->root.version = 4;
    else if (saw_v5_ci && !saw_v4_ci && ex->root.version < 5)
        ex->root.version = 5;
    /* next_ino only ever moves FORWARD, across the rebuild too.
     *
     * Plain assignment let a rebuild from an older root hand back ino numbers
     * that were already allocated and live; the dentry write then rejected
     * them as duplicates (`cwi-fail: ino_dup`) and the caller saw EEXIST on a
     * name that did not exist. The DIRTY-REBUILD guards should keep us off a
     * table whose allocations are still uncommitted, but reusing a live ino is
     * severe enough to refuse outright: skipping ino numbers costs nothing,
     * reissuing one corrupts a file. Same reasoning as the dirty shard tables
     * preserved across the swap above. */
    ex->next_ino = ex->root.next_ino;
    if (saved_next_ino > ex->next_ino)
        ex->next_ino = saved_next_ino;
    if (efs_chunk_size_valid(ex->root.chunk_size))
        ex->chunk_size = ex->root.chunk_size;
    /* Features are root-owned; the EFSM blob does not carry them, so restore
     * the export's working copy from the (server-preserved) root. */
    ex->features = ex->root.features;
    rebuild_shards_unlock(s, shard_eidx, ex, table_ino, shard_sc);
    pthread_mutex_unlock(&s->lock);
    heal_prog_end(s);
    return EFS_OK;
}

static int put_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                             efs_ino_t table_ino, efs_node_id_t node_id,
                             uint32_t chunk_index, uint32_t fragment_index,
                             const uint8_t *data, const uint8_t *checksum)
{
    if (node_id == s->id) {
        /* Use sync writers to avoid pool re-entrancy during meta flush. */
        int rc = server_write_fragment_sync(s, ex, table_ino, chunk_index,
                                            fragment_index, data,
                                            EFS_META_FRAGMENT_SIZE);
        if (rc != EFS_OK)
            return rc;
        return server_write_fragment_sum_sync(s, ex, table_ino, chunk_index,
                                              fragment_index, checksum);
    }
    struct efs_node n;
    if (copy_node_by_id(s, node_id, &n) != 0)
        return EFS_ERR_NOT_FOUND;
    return server_put_fragment_to_peer(n.addr, n.port, ex->id, table_ino,
                                       chunk_index, fragment_index, data, checksum);
}

/* One fragment PUT for the parallel meta flush. */
struct meta_put_job {
    struct efsd_server *s;
    struct efs_export *ex;
    efs_ino_t table_ino;
    efs_node_id_t node;
    uint32_t ci;
    int fi;
    const uint8_t *frag;
    const uint8_t *checksum;
    int rc;
};

static void *meta_put_thread(void *arg)
{
    struct meta_put_job *j = arg;
    j->rc = put_meta_fragment(j->s, j->ex, j->table_ino, j->node, j->ci,
                              (uint32_t)j->fi, j->frag, j->checksum);
    return NULL;
}

/* Ensure peers have an export row before we PUT meta-page fragments.
 * Send a legacy empty EFSM shell (not EFSR) so clients never treat a
 * page-less bootstrap root as authoritative. The shell is a fresh empty
 * export carrying only id+name: serializing the LIVE table here ran unlocked
 * against concurrent REPORT_CHUNKS array growth (SIGSEGV in
 * efs_export_serialize_ex) and fanned a full-table blob out per flush that
 * every peer then deserialized+merged under s->lock. */
static void bootstrap_export_on_peers(struct efsd_server *s, struct efs_export *ex)
{
    /* Publish the live EFSR (id + shard_bits). A bits=0 empty-shell EFSM
     * left joiners at shard_bits=0; CREATE_SHARD then returned NOT_PRIMARY
     * and file create was EIO until a later gen>0 flush — which a fresh
     * mkfs never produced. */
    char *buf = NULL;
    size_t len = 0;
    pthread_mutex_lock(&s->lock);
    int src = efs_export_root_serialize(&ex->root, &buf, &len);
    pthread_mutex_unlock(&s->lock);
    if (src != EFS_OK || !buf)
        return;

    struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    if (!nodes) {
        free(buf);
        return;
    }
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
    efs_node_id_t self = s->id;
    pthread_mutex_unlock(&s->lock);

    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == self)
            continue;
        const char *host = nodes[i].addr;
        uint16_t port = nodes[i].port;
        int fd = server_peer_conn_get(host, port);
        if (fd < 0)
            continue;
        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        int ok = (efs_send_msg(fd, EFS_MSG_PUT_META, buf, (uint32_t)len) == 0 &&
                  efs_recv_msg(fd, &type, &reply, &reply_len) == 0);
        free(reply);
        if (ok)
            server_peer_conn_release(host, port, fd);
        else
            server_peer_conn_drop(host, port, fd);
    }
    free(nodes);
    free(buf);
}

static int server_flush_fragmented_meta_locked(struct efsd_server *s,
                                               struct efs_export *ex,
                                               efs_ino_t table_ino,
                                               int commit_cluster_root,
                                               uint32_t shard_idx,
                                               uint32_t shard_count);
static int server_commit_cluster_extras(struct efsd_server *s,
                                        struct efs_export *ex);

/* PUT_META the current cluster root with refreshed extra-shard descriptors
 * (no shard-0 page rewrite). Used by a non-primary extra-shard owner. */
static int server_commit_cluster_extras(struct efsd_server *s,
                                        struct efs_export *ex)
{
    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    pthread_mutex_lock(&s->lock);
    if (efs_export_root_copy(&root, &ex->root) != EFS_OK) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    /* Do NOT bump the main generation: the primary is the sole shard-0
     * writer and owns the gen sequence. An extras commit that bumped the gen
     * made receivers (including the primary) install our STALE copy of the
     * shard-0 page_cis and fence their live tables — unflushed RPC ops were
     * dropped and rebuilds fetched long-GC'd pages (mc_stress data loss).
     * Same-gen + identical shard-0 checksums is recognized as an extras
     * refresh and merged without fencing. */
    root.generation = ex->root.generation;
    root.version = EFS_META_ROOT_VERSION_V8;
    int crc = efs_export_root_capture_extras(&root, ex);
    /* Ownership remap fence: only push descriptors for shards we CURRENTLY
     * own. After a membership change a shard we used to own must come from
     * its new owner — pushing our (now stale) descriptor could collide with
     * the new owner's same-gen commit. */
    if (crc == EFS_OK && root.extra_shard_count > 0) {
        efs_node_id_t live[EFS_MAX_NODES];
        uint32_t nlive = 0;
        live[nlive++] = s->id;
        for (uint32_t ni = 0; ni < s->node_count && nlive < EFS_MAX_NODES;
             ni++) {
            efs_node_id_t id = s->nodes[ni].id;
            if (id == 0 || id == s->id)
                continue;
            if (server_node_is_down_locked(s, id))
                continue;
            live[nlive++] = id;
        }
        uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
        uint32_t out = 0;
        for (uint32_t i = 0; i < root.extra_shard_count; i++) {
            uint32_t sh = root.extra_shard_ids ? root.extra_shard_ids[i] : 0;
            if (sh == 0)
                continue;
            if (sc > 1 && efs_shard_owner_of(sh, sc, live, nlive) != s->id) {
                efs_export_root_free(&root.extra_roots[i]);
                continue;
            }
            if (out != i) {
                root.extra_roots[out] = root.extra_roots[i];
                if (root.extra_shard_ids)
                    root.extra_shard_ids[out] = sh;
            }
            out++;
        }
        root.extra_shard_count = out;
    }
    pthread_mutex_unlock(&s->lock);
    if (crc != EFS_OK) {
        efs_export_root_free(&root);
        return -1;
    }
    char *root_buf = NULL;
    size_t root_len = 0;
    if (efs_export_root_serialize(&root, &root_buf, &root_len) != EFS_OK) {
        efs_export_root_free(&root);
        return -1;
    }
    int acks = 0;
    struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    if (!nodes) {
        free(root_buf);
        efs_export_root_free(&root);
        return -1;
    }
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
    efs_node_id_t self = s->id;
    pthread_mutex_unlock(&s->lock);
    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == self)
            continue;
        int fd = server_peer_conn_get(nodes[i].addr, nodes[i].port);
        if (fd < 0)
            continue;
        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        int net_ok = (efs_send_msg(fd, EFS_MSG_PUT_META, root_buf,
                                   (uint32_t)root_len) == 0 &&
                      efs_recv_msg(fd, &type, &reply, &reply_len) == 0);
        if (net_ok && type == EFS_MSG_PUT_META_REPLY && reply_len >= 1 &&
            ((uint8_t *)reply)[0] == EFS_PUT_META_OK)
            acks++;
        free(reply);
        if (net_ok)
            server_peer_conn_release(nodes[i].addr, nodes[i].port, fd);
        else
            server_peer_conn_drop(nodes[i].addr, nodes[i].port, fd);
    }
    free(nodes);
    free(root_buf);
    uint32_t need = (node_count >= EFS_NUM_FRAGMENTS) ? 2
                    : (node_count > 1) ? 1 : 0;
    if (acks < (int)need) {
        efs_export_root_free(&root);
        return -1;
    }
    pthread_mutex_lock(&s->lock);
    /* Adopt only the descriptors this commit refreshed (owned shards); carry
     * every other descriptor forward from the existing root. A full
     * root_move would persist the ownership-FILTERED root and wipe the
     * restart path for shards this node doesn't own — the wire filter exists
     * to avoid pushing stale same-gen descriptors, not to shrink the local
     * recovery record. root's non-extras fields are a same-gen copy of
     * ex->root's (copied at entry, gen unchanged), so no root_move. */
    for (uint32_t i = 0; i < root.extra_shard_count; i++) {
        uint32_t sh = root.extra_shard_ids ? root.extra_shard_ids[i] : 0;
        if (sh == 0)
            continue;
        uint32_t j = 0;
        while (j < ex->root.extra_shard_count &&
               (!ex->root.extra_shard_ids || ex->root.extra_shard_ids[j] != sh))
            j++;
        if (j < ex->root.extra_shard_count) {
            if (ex->root.extra_roots[j].generation >=
                root.extra_roots[i].generation)
                continue;
            efs_export_root_free(&ex->root.extra_roots[j]);
            if (efs_export_root_copy(&ex->root.extra_roots[j],
                                     &root.extra_roots[i]) != EFS_OK)
                break;
        } else {
            uint32_t n = ex->root.extra_shard_count;
            uint32_t *ids = realloc(ex->root.extra_shard_ids,
                                    (size_t)(n + 1) * sizeof(uint32_t));
            struct efs_export_root *er =
                realloc(ex->root.extra_roots,
                        (size_t)(n + 1) * sizeof(struct efs_export_root));
            if (!ids || !er) {
                if (ids)
                    ex->root.extra_shard_ids = ids;
                if (er)
                    ex->root.extra_roots = er;
                break;
            }
            ex->root.extra_shard_ids = ids;
            ex->root.extra_roots = er;
            memset(&ex->root.extra_roots[n], 0,
                   sizeof(ex->root.extra_roots[n]));
            if (efs_export_root_copy(&ex->root.extra_roots[n],
                                     &root.extra_roots[i]) != EFS_OK)
                break;
            ex->root.extra_shard_ids[n] = sh;
            ex->root.extra_shard_count = n + 1;
        }
    }
    server_save_export(s, ex);
    pthread_mutex_unlock(&s->lock);
    efs_export_root_free(&root);
    return 0;
}

/* Write all meta pages (2+1) and persist/replicate the EFSR root.
 * Serialized on s->meta_flush_mu: the meta-flush thread and a synchronous
 * REPORT_CHUNKS (fsync barrier) flush both call this, and without the mutex
 * both compute the same new_gen (= root.generation+1) and race to the peers —
 * the loser's root is rejected STALE (gen <= peer's), so the sync fsync sees
 * 0 peer acks and returns EIO even though the data commits on the retry. */
/* Group commit for the synchronous fsync flush.
 *
 * The caller's mutation is already applied to the live table before it gets
 * here, so ANY flush that *starts* after this point snapshots it (the snapshot
 * is taken under s->lock inside the flush). That makes coalescing safe: a
 * caller that finds a flush already running cannot rely on it — that flush may
 * have snapshotted first — but it can wait for the NEXT one.
 *
 * The leader keeps flushing while callers are still queued, so a waiter can
 * never block forever waiting for a flush that nobody starts.
 *
 * Returns the rc of the flush that covered the caller. If several completed,
 * the last rc is used; a failure only costs the client a BUSY retry.
 */
int server_flush_meta_grouped(struct efsd_server *s, struct efs_export *ex)
{
    uint32_t eidx = (uint32_t)(ex - s->exports);
    if (eidx >= EFS_MAX_EXPORTS)
        return server_flush_fragmented_meta(s, ex);

    pthread_mutex_lock(&s->flush_grp_mu);
    if (s->flush_running[eidx]) {
        /* The in-flight flush may have snapshotted before this caller's
         * mutation, so it proves nothing; the next one is guaranteed to
         * include it. */
        uint64_t target = s->flush_started[eidx] + 1;
        if (s->flush_target[eidx] < target)
            s->flush_target[eidx] = target;
        while (s->flush_done[eidx] < target)
            pthread_cond_wait(&s->flush_grp_cv, &s->flush_grp_mu);
        int rc = s->flush_last_rc[eidx];
        pthread_mutex_unlock(&s->flush_grp_mu);
        return rc;
    }
    s->flush_running[eidx] = 1;

    int rc = 0;
    for (;;) {
        s->flush_started[eidx]++;
        pthread_mutex_unlock(&s->flush_grp_mu);

        rc = server_flush_fragmented_meta(s, ex);

        pthread_mutex_lock(&s->flush_grp_mu);
        s->flush_done[eidx] = s->flush_started[eidx];
        s->flush_last_rc[eidx] = rc;
        pthread_cond_broadcast(&s->flush_grp_cv);
        /* Callers that queued during this flush need one that starts after
         * their mutation — run it rather than leaving them stranded. */
        if (s->flush_done[eidx] >= s->flush_target[eidx])
            break;
    }
    s->flush_running[eidx] = 0;
    pthread_mutex_unlock(&s->flush_grp_mu);
    return rc;
}

int server_flush_fragmented_meta(struct efsd_server *s, struct efs_export *ex)
{
    uint64_t t_win0 = heal_mono_us();
    uint32_t tabs_seen = 0, tabs_flushed = 0;
    pthread_mutex_lock(&s->meta_flush_mu);
    /* Time spent queued behind another flush (the flush thread and every sync
     * fsync REPORT_CHUNKS serialize on meta_flush_mu). Kept separate from the
     * extras loop so a queueing problem is never misread as per-shard cost. */
    uint64_t t_wait = heal_mono_us() - t_win0;
    int rc = 0;
    int flushed_extra = 0;
    pthread_mutex_lock(&s->lock);
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = 0;
    live[nlive++] = s->id;
    for (uint32_t i = 0; i < s->node_count && nlive < EFS_MAX_NODES; i++) {
        efs_node_id_t id = s->nodes[i].id;
        if (id == 0 || id == s->id)
            continue;
        if (server_node_is_down_locked(s, id))
            continue;
        live[nlive++] = id;
    }
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    uint32_t bits = ex->root.shard_bits;
    int primary = server_is_meta_primary_locked(s);
    pthread_mutex_unlock(&s->lock);
    /* Extra-shard pages must land before the cluster root references them.
     * Only flush shards that are actually dirty: an export-level dirty mark
     * covers every shard, and flushing clean shards on every window is what
     * fanned the extras-commit catchup storm (every clean flush bumped that
     * shard's descriptor gen and made every peer rebuild it). */
    if (bits && ex->shard_tabs) {
        for (uint32_t i = 1; i < ex->shard_tab_cap; i++) {
            struct efs_export *tab = ex->shard_tabs[i];
            if (!tab || tab->inode_count == 0)
                continue;
            tabs_seen++;
            if (sc > 1 &&
                efs_shard_owner_of(i, sc, live, nlive) != s->id) {
                /* Lost ownership (membership remap): the unflushed ops in
                 * this tab are lost (crash-like window — the new owner
                 * rebuilds from the last committed descriptor). Clear dirty
                 * so the descriptor merge can adopt the new owner's tables;
                 * a dirty tab is never merged and would stay stale forever. */
                tab->shard_dirty = 0;
                continue;
            }
            if (!tab->shard_dirty)
                continue;
            /* A hollow extra (still catching up) cannot be serialized —
             * flush_locked bails needs_rebuild and used to fail the
             * caller's fsync (9-way first-finisher EIO). Leave it dirty
             * for catchup; the sync REPORT retries as BUSY. */
            if (tab->meta_needs_rebuild && tab->root.page_count > 0) {
                rc = -1;
                continue;
            }
            int src = server_flush_fragmented_meta_locked(
                s, tab, efs_meta_shard_table_ino(i), 0, i, sc);
            tabs_flushed++;
            if (src != 0)
                rc = src;
            else
                flushed_extra = 1;
            /* shard_dirty is now cleared inside the flush, only when no op
             * landed during the unlocked window (flush-race fix). */
        }
    }
    int main_dirty = ex->shard_dirty;
    uint64_t t_extras = heal_mono_us() - t_win0 - t_wait;
    uint64_t t_main0 = heal_mono_us();
    uint64_t t_commit = 0;
    if (primary) {
        /* Shard 0 used to flush on every rpc_dirty window even when only
         * extras changed — a 71 MB serialize + PUT of every CoW page.
         * Only rewrite it when the main table itself is dirty. */
        if (ex->shard_dirty) {
            int rc0 = server_flush_fragmented_meta_locked(
                s, ex, EFS_META_TABLE_INO, 1, 0, sc);
            if (rc0 != 0)
                rc = rc0;
            /* shard_dirty is now cleared inside the flush, only when no op
             * landed during the unlocked window (flush-race fix). */
        } else if (flushed_extra && rc == 0) {
            uint64_t c0 = heal_mono_us();
            rc = server_commit_cluster_extras(s, ex);
            t_commit = heal_mono_us() - c0;
        }
    } else if (flushed_extra && rc == 0) {
        uint64_t c0 = heal_mono_us();
        rc = server_commit_cluster_extras(s, ex);
        t_commit = heal_mono_us() - c0;
    }
    pthread_mutex_unlock(&s->meta_flush_mu);
    if (efs_lock_prof_on)
        fprintf(stderr,
                "LOCK-PROF all_calls=%llu all_wait_us=%llu all_hold_us=%llu "
                "all_shards=%llu n_calls=%llu n_wait_us=%llu\n",
                __atomic_load_n(&efs_lock_all_calls, __ATOMIC_RELAXED),
                __atomic_load_n(&efs_lock_all_wait_us, __ATOMIC_RELAXED),
                __atomic_load_n(&efs_lock_all_hold_us, __ATOMIC_RELAXED),
                __atomic_load_n(&efs_lock_all_shards, __ATOMIC_RELAXED),
                __atomic_load_n(&efs_lockn_calls, __ATOMIC_RELAXED),
                __atomic_load_n(&efs_lockn_wait_us, __ATOMIC_RELAXED));
    if (efs_lock_prof_on) {
        unsigned long long tot = 0;
        for (int i = 0; i < 256; i++)
            tot += __atomic_load_n(&efs_rpc_count[i], __ATOMIC_RELAXED);
        fprintf(stderr,
                "RPC-PROF total=%llu report=%llu getattr=%llu lookup=%llu "
                "create=%llu create_shard=%llu getchunks=%llu lookup_path=%llu "
                "readdir=%llu\n",
                tot,
                __atomic_load_n(&efs_rpc_count[EFS_MSG_REPORT_CHUNKS],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_GETATTR],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_LOOKUP],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_CREATE],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_CREATE_SHARD],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_GETCHUNKS],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_LOOKUP_PATH],
                                __ATOMIC_RELAXED),
                __atomic_load_n(&efs_rpc_count[EFS_MSG_INODE_READDIR],
                                __ATOMIC_RELAXED));
    }
    if (getenv("EFS_FLUSH_PROF"))
        fprintf(stderr,
                "FLUSH-WINDOW export=%s shards=%u tabs_nonempty=%u "
                "tabs_flushed=%u main_dirty=%d wait=%lluus extras=%lluus "
                "main=%lluus commit=%lluus total=%lluus\n",
                ex->name, sc, tabs_seen, tabs_flushed, main_dirty,
                (unsigned long long)t_wait,
                (unsigned long long)t_extras,
                (unsigned long long)(heal_mono_us() - t_main0),
                (unsigned long long)t_commit,
                (unsigned long long)(heal_mono_us() - t_win0));
    return rc;
}

static int server_flush_fragmented_meta_locked(struct efsd_server *s,
                                               struct efs_export *ex,
                                               efs_ino_t table_ino,
                                               int commit_cluster_root,
                                               uint32_t shard_idx,
                                               uint32_t shard_count)
{
    /* Peers must know the export before accepting meta-page PUT_CHUNKs. */
    bootstrap_export_on_peers(s, ex);

    /* Snapshot tables under the lock, then pack unlocked. A live serialize
     * of a large table is seconds of strnlen/memcpy under s->lock — every
     * PUT_CHUNK takes that lock for export_acquire, so a post-warmup flush
     * wedged 9-client sw-1m at ~140 MiB/s. The memcpy snapshot is tens of
     * ms; handlers keep mutating the live table and those ops stay dirty
     * for the next flush. */
    char *blob = NULL;
    size_t blob_len = 0;
    uint64_t new_gen = 1;
    uint64_t old_gen = 0;
    uint32_t old_ino_pc = 0, old_ch_pc = 0;
    uint32_t ino_blob_len = 0, chunk_blob_len = 0;
    uint32_t *old_cis = NULL;
    uint32_t old_cis_count = 0;
    uint32_t *new_cis = NULL;
    uint32_t new_cis_count = 0;
    struct efs_export snap;
    memset(&snap, 0, sizeof(snap));
    /* EFS_FLUSH_PROF: per-stage timing. The flush pays a FIXED cost per shard
     * table, so raising shard_bits multiplies it (bits=5 measured 3x worse on
     * posixstress). Breaks the cost into lock-wait / snapshot / serialize /
     * page-PUT / commit so the per-table overhead is attributable. */
    uint64_t t_begin = heal_mono_us();
    uint64_t t_lockwait = 0, t_snap = 0, t_ser = 0, t_pages = 0;
    pthread_mutex_lock(&s->lock);
    t_lockwait = heal_mono_us() - t_begin;
    if (ex->meta_needs_rebuild) {
        /* Fenced by a concurrent client-driven PUT_META (counts zeroed): the
         * in-memory table is stale and the catchup rebuild will overwrite it,
         * so serializing now would flush an empty/torn table and lose the
         * client's data. Refuse; the caller retries after the rebuild. */
        fprintf(stderr,
                "meta-flush: export=%s bail needs_rebuild gen=%llu "
                "pages=%u inodes=%llu\n",
                ex->name, (unsigned long long)ex->root.generation,
                ex->root.page_count, (unsigned long long)ex->inode_count);
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    /* Take the shard locks for the snapshot, in global->shard order.
     *
     * The snapshot memcpy's ex->inodes[], and since the per-op CREATE path
     * drops the global lock and reallocs that array holding only a shard
     * lock, the global lock alone no longer excludes a writer: the flush
     * thread read a freed pointer and took a SIGSEGV in
     * efs_export_table_snapshot_ex under a 9-client 10M-inode create load.
     *
     * This is the same global+all-shards discipline the rebuild already
     * follows (rebuild_shards_lock, blocker 1); the flush is the other
     * whole-table reader and was simply never converted when the per-op
     * writers landed. A main-table flush needs every shard lock because a
     * create may land on any shard; a single-shard flush needs only its own. */
    uint32_t fsc = 0;
    int fidx = rebuild_shards_lock(s, ex, table_ino, &fsc);

    efs_export_ensure_rollups(ex);
    int omit_chunks = (ex->chunk_epoch == ex->flushed_chunk_epoch &&
                       ex->root.chunk_blob_len > 0);
    uint64_t snap_chunk_epoch = ex->chunk_epoch;
    uint32_t keep_ch_len = ex->root.chunk_blob_len;
    /* DIAG flush-race: capture the live table's mutation signals so the commit
     * path can detect an op that landed during the unlocked PUT window (such
     * an op is NOT in this snapshot; if the caller then clears shard_dirty it
     * is lost on a later rebuild). inode_count catches create/unlink;
     * layout_epoch catches unlink (and a concurrent table adopt). */
    uint64_t snap_icount = ex->inode_count;
    uint64_t snap_lepoch = ex->layout_epoch;
    int snap_rc = efs_export_table_snapshot_ex(ex, &snap, omit_chunks);
    new_gen = ex->root.generation + 1;
    if (new_gen == 0)
        new_gen = 1;
    if (ex->meta_fragmented) {
        old_gen = ex->root.generation;
        old_ino_pc = ex->root.ino_page_count ? ex->root.ino_page_count
                                             : ex->root.page_count;
        old_ch_pc = ex->root.chunk_page_count;
    }
    rebuild_shards_unlock(s, fidx, ex, table_ino, fsc);
    pthread_mutex_unlock(&s->lock);
    t_snap = heal_mono_us() - t_begin - t_lockwait;
    if (snap_rc != EFS_OK)
        return -1;
    uint64_t t_ser0 = heal_mono_us();
    if (efs_export_serialize_ex(&snap, &blob, &blob_len, &ino_blob_len,
                                &chunk_blob_len) != EFS_OK) {
        efs_export_table_snapshot_free(&snap);
        return -1;
    }
    efs_export_table_snapshot_free(&snap);
    t_ser = heal_mono_us() - t_ser0;
    if (omit_chunks) {
        chunk_blob_len = keep_ch_len;
        blob_len = (size_t)ino_blob_len;
    }

    if (blob_len > (size_t)EFS_META_MAX_PAGES * EFS_META_PAGE_SIZE) {
        free(blob);
        return -1;
    }

    /* Durability is gated by the two-phase peer-ack quorum below, not by a
     * hard node-count check here: during cluster formation a node's membership
     * view can lag the real cluster (gossip convergence), so refusing on a
     * stale count would fail mkfs/create on a healthy-but-converging ring. */
    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    pthread_mutex_lock(&s->lock);
    int prc = efs_export_root_prepare(&root, ex, new_gen, ino_blob_len,
                                      chunk_blob_len);
    pthread_mutex_unlock(&s->lock);
    if (prc != EFS_OK) {
        free(blob);
        return -1;
    }

    /* Server-owned metadata (Phase 2): the primary's flush thread is the sole
     * metadata writer — there is no client flush election to carry forward.
     * Clear the lease so peers skip the writer gate; a stale pre-2a client
     * lease would otherwise be rejected as not-currently-held (the peers'
     * post-restart election state is inconsistent, so some accept and some
     * reject, and the root never reaches quorum). The gen CAS still fences a
     * lagging writer. */
    root.write_lease_id = 0;
    root.write_lease_until_ms = 0;

    /* CoW (EFSR v7): allocate each page a fresh chunk_index above the legacy
     * dual-slot window. Carry the allocator forward from the committed root
     * so a re-flush never reuses a ci the committed root still references. */
    pthread_mutex_lock(&s->lock);
    int cow_ok = efs_export_root_is_cow(&ex->root) &&
                 ex->root.next_ci >= EFS_META_COW_BASE;
    uint32_t committed_next = ex->root.next_ci;
    pthread_mutex_unlock(&s->lock);
    root.next_ci = cow_ok ? committed_next : EFS_META_COW_BASE;

    uint8_t *page = malloc(EFS_META_PAGE_SIZE);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    if (!page || !frag_buf) {
        free(page);
        free(frag_buf);
        free(blob);
        efs_export_root_free(&root);
        return -1;
    }
    uint8_t *fragments[EFS_NUM_FRAGMENTS] = {
        frag_buf, frag_buf + EFS_META_FRAGMENT_SIZE,
        frag_buf + 2 * EFS_META_FRAGMENT_SIZE
    };

    /* Reuse committed CoW pages whose fragment checksums match. Create and
     * unlink only touch a couple of inode pages; rewriting every chunk page
     * of a 71 MB table was the remaining create/unlink tax. */
    uint8_t *skip_sums = NULL;
    uint32_t *skip_cis = NULL;
    uint32_t skip_pc = 0, skip_ino_pc = 0, skip_ch_len = 0;
    pthread_mutex_lock(&s->lock);
    if (cow_ok && ex->root.page_checksums && ex->root.page_cis &&
        ex->root.page_count > 0) {
        skip_pc = ex->root.page_count;
        skip_ino_pc = ex->root.ino_page_count;
        skip_ch_len = ex->root.chunk_blob_len;
        size_t sn = (size_t)skip_pc * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        skip_sums = malloc(sn);
        skip_cis = malloc((size_t)skip_pc * sizeof(uint32_t));
        if (skip_sums && skip_cis) {
            memcpy(skip_sums, ex->root.page_checksums, sn);
            memcpy(skip_cis, ex->root.page_cis,
                   (size_t)skip_pc * sizeof(uint32_t));
        } else {
            free(skip_sums);
            free(skip_cis);
            skip_sums = NULL;
            skip_cis = NULL;
            skip_pc = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
    uint32_t pages_reused = 0;
    uint64_t t_pages0 = heal_mono_us();

    for (uint32_t packed = 0; packed < root.page_count; packed++) {
        int region = (packed < root.ino_page_count) ? EFS_META_REGION_INO
                                                    : EFS_META_REGION_CHUNK;
        uint32_t pi = (region == EFS_META_REGION_INO)
                          ? packed
                          : packed - root.ino_page_count;
        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        int reuse = 0;
        if (skip_sums && skip_cis && packed < skip_pc) {
            int in_ino = (region == EFS_META_REGION_INO);
            int layout_ok = (root.ino_page_count == skip_ino_pc);
            if (!in_ino && layout_ok && chunk_blob_len == skip_ch_len) {
                reuse = 1;
            } else if (in_ino && packed < skip_ino_pc) {
                /* Hash after encode; compared below. */
            } else if (!in_ino && !layout_ok) {
                reuse = 0;
            }
        }
        if (reuse) {
            memcpy(checksums,
                   skip_sums + (size_t)packed * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                   sizeof(checksums));
            root.page_cis[packed] = skip_cis[packed];
            for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
                memcpy(efs_export_root_checksum(&root, packed, fi),
                       checksums[fi], EFS_HASH_SIZE);
            pages_reused++;
            continue;
        }
        const char *rbase = (region == EFS_META_REGION_INO)
                                ? blob
                                : blob + ino_blob_len;
        uint32_t rlen = (region == EFS_META_REGION_INO) ? ino_blob_len
                                                       : chunk_blob_len;
        if (efs_meta_extract_page(rbase, rlen, pi, page) != EFS_OK) {
            free(page);
            free(frag_buf);
            free(blob);
            free(skip_sums);
            free(skip_cis);
            efs_export_root_free(&root);
            return -1;
        }
        efs_encode_chunk(page, EFS_META_PAGE_SIZE, EFS_META_PAGE_SIZE, fragments);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            efs_hash(fragments[fi], EFS_META_FRAGMENT_SIZE, checksums[fi]);
        if (skip_sums && skip_cis && packed < skip_pc &&
            ((region == EFS_META_REGION_INO && packed < skip_ino_pc) ||
             (region == EFS_META_REGION_CHUNK &&
              root.ino_page_count == skip_ino_pc))) {
            const uint8_t *old = skip_sums +
                (size_t)packed * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
            if (memcmp(checksums[0], old, EFS_HASH_SIZE) == 0 &&
                memcmp(checksums[1], old + EFS_HASH_SIZE, EFS_HASH_SIZE) == 0 &&
                memcmp(checksums[2], old + 2 * EFS_HASH_SIZE,
                       EFS_HASH_SIZE) == 0) {
                root.page_cis[packed] = skip_cis[packed];
                for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
                    memcpy(efs_export_root_checksum(&root, packed, fi),
                           checksums[fi], EFS_HASH_SIZE);
                pages_reused++;
                continue;
            }
        }

        /* CoW (EFSR v7): write the page to a fresh, never-referenced
         * chunk_index and record it in the new root's page_cis[]. */
        if (root.next_ci == UINT32_MAX) {
            free(page);
            free(frag_buf);
            free(blob);
            free(skip_sums);
            free(skip_cis);
            efs_export_root_free(&root);
            return -1;
        }
        uint32_t ci = root.next_ci++;
        root.page_cis[packed] = ci;

        efs_node_id_t placed[EFS_NUM_FRAGMENTS];
        pthread_mutex_lock(&s->lock);
        efs_place_fragments(s->nodes, s->node_count, table_ino, ci,
                            placed);
        /* Re-route fragments off heartbeat-marked-down nodes so a dead peer
         * doesn't consume a full PUT timeout per page. */
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            if (!server_node_is_down_locked(s, placed[fi]))
                continue;
            for (uint32_t ni = 0; ni < s->node_count; ni++) {
                efs_node_id_t cand = s->nodes[ni].id;
                if (cand == placed[0] || cand == placed[1] || cand == placed[2])
                    continue;
                if (server_node_is_down_locked(s, cand))
                    continue;
                placed[fi] = cand;
                break;
            }
        }
        pthread_mutex_unlock(&s->lock);

        /* Fan the 3 fragment PUTs out in parallel: a sequential page flush
         * pays 3 RTTs (plus a fixed 50ms retry sleep) per page, which is a
         * meta RTT storm under mkfs/migrate. Decode only needs 2 acks. */
        struct meta_put_job jobs[EFS_NUM_FRAGMENTS];
        pthread_t tids[EFS_NUM_FRAGMENTS];
        int spawned[EFS_NUM_FRAGMENTS] = {0, 0, 0};
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            jobs[fi].s = s;
            jobs[fi].ex = ex;
            jobs[fi].table_ino = table_ino;
            jobs[fi].node = placed[fi];
            jobs[fi].ci = ci;
            jobs[fi].fi = fi;
            jobs[fi].frag = fragments[fi];
            jobs[fi].checksum = checksums[fi];
            jobs[fi].rc = EFS_ERR_NET;
            if (pthread_create(&tids[fi], NULL, meta_put_thread, &jobs[fi]) == 0)
                spawned[fi] = 1;
            else
                meta_put_thread(&jobs[fi]);
        }
        int acks = 0;
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            if (spawned[fi])
                pthread_join(tids[fi], NULL);
            int prc = jobs[fi].rc;
            if (prc != EFS_OK) {
                /* Peer may not have finished bootstrap yet — retry once. */
                prc = put_meta_fragment(s, ex, table_ino, placed[fi], ci,
                                        (uint32_t)fi, fragments[fi],
                                        checksums[fi]);
            }
            if (prc == EFS_OK) {
                acks++;
            } else {
                fprintf(stderr,
                        "meta-flush: page %u frag %u put failed rc=%d "
                        "(node %u export=%u ci=%u)\n",
                        packed, fi, prc, placed[fi], ex->id, ci);
            }
        }
        if (acks < 2) {
            free(page);
            free(frag_buf);
            free(blob);
            free(skip_sums);
            free(skip_cis);
            efs_export_root_free(&root);
            return -1;
        }
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            memcpy(efs_export_root_checksum(&root, packed, fi), checksums[fi],
                   EFS_HASH_SIZE);
    }
    free(page);
    free(frag_buf);
    free(blob);
    free(skip_sums);
    free(skip_cis);
    t_pages = heal_mono_us() - t_pages0;
    if (getenv("EFS_FLUSH_PROF"))
        fprintf(stderr,
                "FLUSH-PROF shard=%u pages=%u written=%u reused=%u "
                "lockwait=%lluus snap=%lluus ser=%lluus pages=%lluus "
                "total=%lluus\n",
                shard_idx, root.page_count, root.page_count - pages_reused,
                pages_reused, (unsigned long long)t_lockwait,
                (unsigned long long)t_snap, (unsigned long long)t_ser,
                (unsigned long long)t_pages,
                (unsigned long long)(heal_mono_us() - t_begin));
    if (pages_reused >= 16)
        fprintf(stderr, "meta-flush: reused %u/%u unchanged CoW pages\n",
                pages_reused, root.page_count);

    /* Extra shards (Phase 3): pages live under table_ino. Do not PUT_META
     * this root — that would replace the export-level shard-0 EFSR. v8 will
     * fold per-shard gens into the cluster root. */
    if (!commit_cluster_root) {
        pthread_mutex_lock(&s->lock);
        if (ex->inode_count != snap_icount || ex->layout_epoch != snap_lepoch)
            fprintf(stderr,
                    "FLUSH-RACE: export=%s table_ino=%llu shard=%u changed "
                    "during flush (inodes %llu->%llu layout %llu->%llu)\n",
                    ex->name, (unsigned long long)table_ino,
                    ex->shard_id,
                    (unsigned long long)snap_icount,
                    (unsigned long long)ex->inode_count,
                    (unsigned long long)snap_lepoch,
                    (unsigned long long)ex->layout_epoch);
        /* Ownership remap fence: re-verify we still own this shard AT COMMIT
         * TIME (membership is updated under this same lock, so check+commit
         * are atomic w.r.t. membership changes). A flush that started while
         * we were owner must not commit after a remap — its same-gen
         * descriptor would silently collide with the new owner's. */
        if (shard_count > 1 && shard_idx > 0) {
            efs_node_id_t live[EFS_MAX_NODES];
            uint32_t nlive = 0;
            live[nlive++] = s->id;
            for (uint32_t ni = 0; ni < s->node_count && nlive < EFS_MAX_NODES;
                 ni++) {
                efs_node_id_t id = s->nodes[ni].id;
                if (id == 0 || id == s->id)
                    continue;
                if (server_node_is_down_locked(s, id))
                    continue;
                live[nlive++] = id;
            }
            if (efs_shard_owner_of(shard_idx, shard_count, live, nlive) !=
                s->id) {
                pthread_mutex_unlock(&s->lock);
                efs_export_root_free(&root);
                return -1;
            }
        }
        ex->meta_fragmented = 1;
        /* Snapshot outgoing/incoming page_cis for the GC below — same CoW
         * reclaim as the cluster-root path, or every shard flush leaks its
         * superseded pages forever. */
        if (ex->root.page_cis && ex->root.page_count > 0) {
            old_cis_count = ex->root.page_count;
            old_cis = malloc((size_t)old_cis_count * sizeof(uint32_t));
            if (old_cis)
                memcpy(old_cis, ex->root.page_cis,
                       (size_t)old_cis_count * sizeof(uint32_t));
            else
                old_cis_count = 0;
        }
        if (root.page_cis && root.page_count > 0) {
            new_cis_count = root.page_count;
            new_cis = malloc((size_t)new_cis_count * sizeof(uint32_t));
            if (new_cis)
                memcpy(new_cis, root.page_cis,
                       (size_t)new_cis_count * sizeof(uint32_t));
            else
                new_cis_count = 0;
        }
        efs_export_root_move(&ex->root, &root);
        ex->flushed_chunk_epoch = snap_chunk_epoch;
        /* Flush-race fix: the snapshot was taken under the lock, then the lock
         * was dropped for serialize/PUT. An op that landed in that window is
         * NOT in the committed root. The caller used to clear shard_dirty
         * unconditionally, losing such an op on a later rebuild. Clear dirty
         * only if the table is unchanged since the snapshot; otherwise keep
         * it dirty so the next flush commits the straggler. */
        if (ex->inode_count == snap_icount && ex->layout_epoch == snap_lepoch)
            ex->shard_dirty = 0;
        pthread_mutex_unlock(&s->lock);
        if (old_cis && new_cis)
            server_gc_meta_cow_pages(s, ex, table_ino, old_cis, old_cis_count,
                                     new_cis, new_cis_count);
        free(old_cis);
        free(new_cis);
        return 0;
    }

    /* Fold extra-shard descriptors into the cluster root (v8).
     * server_commit_cluster_extras only runs on NON-primary owners — so for
     * shards owned by the primary itself, the shard flush above updated
     * tab->root in memory but nothing ever published it: a restart rebuilt
     * those shards from their upgrade-time descriptors and every later
     * create/write on them was lost (post-bounce size=0 on primary-owned
     * shards). Capture the local tables, then MAX-MERGE the previous root's
     * descriptors: the persisted root is the recovery record for EVERY shard,
     * so a descriptor must never be dropped or regressed — an ownership
     * filter here garbage-collected shard 4's descriptor during a heartbeat
     * flap and wiped its recovery path. Receivers skip descriptors with
     * gen <= theirs, so carrying an unowned shard's older descriptor is
     * harmless. */
    if (shard_count > 1) {
        pthread_mutex_lock(&s->lock);
        int crc = efs_export_root_capture_extras(&root, ex);
        if (crc == EFS_OK)
            crc = efs_export_root_maxmerge_extras(&root, &ex->root);
        pthread_mutex_unlock(&s->lock);
        if (crc != EFS_OK) {
            free(old_cis);
            free(new_cis);
            efs_export_root_free(&root);
            return -1;
        }
    }

    /* Two-phase commit: the new root is NOT installed locally until a quorum
     * of peers has acknowledged it. Serialize the candidate root for fan-out
     * first, push to peers, and only on quorum move it into ex->root and
     * persist. On failure the local node keeps the prior authoritative gen. */
    char *root_buf = NULL;
    size_t root_len = 0;
    if (efs_export_root_serialize(&root, &root_buf, &root_len) != EFS_OK) {
        efs_export_root_free(&root);
        return -1;
    }
    /* Fingerprint the exact root bytes the prepares carry, computed NOW —
     * root_buf is freed after the prepare fan-out but the COMMIT broadcast
     * below needs the hash. Peers promote only a pending root whose stashed
     * prepare bytes hash to this same value. */
    uint8_t root_sum[EFS_HASH_SIZE];
    efs_hash(root_buf, root_len, root_sum);

    int acks = 0;
    struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    if (!nodes) {
        free(root_buf);
        efs_export_root_free(&root);
        return -1;
    }
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
    efs_node_id_t self = s->id;
    pthread_mutex_unlock(&s->lock);

    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == self)
            continue;
        const char *host = nodes[i].addr;
        uint16_t port = nodes[i].port;
        int fd = server_peer_conn_get(host, port);
        if (fd < 0)
            continue;
        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        int net_ok = (efs_send_msg(fd, EFS_MSG_PUT_META, root_buf,
                                   (uint32_t)root_len) == 0 &&
                      efs_recv_msg(fd, &type, &reply, &reply_len) == 0);
        if (net_ok && type == EFS_MSG_PUT_META_REPLY && reply_len >= 1 &&
            ((uint8_t *)reply)[0] == EFS_PUT_META_OK) {
            acks++;
        }
        free(reply);
        if (net_ok)
            server_peer_conn_release(host, port, fd);
        else
            server_peer_conn_drop(host, port, fd);
    }
    free(nodes);
    free(root_buf);

    /* Durability: match the client's ≥2-ack rule so a single surviving copy
     * cannot be lost with one node failure. node_count already includes self,
     * so require min(2, node_count) total roots persisted (self counts once
     * committed below). For a 3-node cluster that is self + 1 peer; to be
     * safe under one concurrent failure require 2 peer acks when 3+ nodes. */
    uint32_t need_peer_acks = (node_count >= EFS_NUM_FRAGMENTS) ? 2
                              : (node_count > 1) ? 1 : 0;
    if (acks < (int)need_peer_acks) {
        fprintf(stderr,
                "meta-flush: export %u root replicate got %d/%u peer acks — "
                "gen %llu NOT committed locally\n",
                root.id, acks, need_peer_acks, (unsigned long long)new_gen);
        efs_export_root_free(&root);
        return -1;
    }

    /* Quorum reached: install + persist locally. Persist under lock so a
     * concurrent peer PUT_META cannot free page_checksums mid-fwrite. */
    uint32_t new_ino_pc = root.ino_page_count ? root.ino_page_count
                                              : root.page_count;
    uint32_t new_ch_pc = root.chunk_page_count;
    pthread_mutex_lock(&s->lock);
    if (ex->inode_count != snap_icount || ex->layout_epoch != snap_lepoch)
        fprintf(stderr,
                "FLUSH-RACE: export=%s table_ino=%llu (cluster-root) changed "
                "during flush (inodes %llu->%llu layout %llu->%llu)\n",
                ex->name, (unsigned long long)table_ino,
                (unsigned long long)snap_icount,
                (unsigned long long)ex->inode_count,
                (unsigned long long)snap_lepoch,
                (unsigned long long)ex->layout_epoch);
    ex->meta_fragmented = 1;
    /* CoW (EFSR v7): snapshot the outgoing root's page_cis[] and the new
     * root's page_cis[] so the GC runs lock-free on stable copies. */
    if (ex->root.page_cis && ex->root.page_count > 0) {
        old_cis_count = ex->root.page_count;
        old_cis = malloc((size_t)old_cis_count * sizeof(uint32_t));
        if (old_cis)
            memcpy(old_cis, ex->root.page_cis,
                   (size_t)old_cis_count * sizeof(uint32_t));
        else
            old_cis_count = 0;
    }
    if (root.page_cis && root.page_count > 0) {
        new_cis_count = root.page_count;
        new_cis = malloc((size_t)new_cis_count * sizeof(uint32_t));
        if (new_cis)
            memcpy(new_cis, root.page_cis,
                   (size_t)new_cis_count * sizeof(uint32_t));
        else
            new_cis_count = 0;
    }
    efs_export_root_move(&ex->root, &root);
    ex->flushed_chunk_epoch = snap_chunk_epoch;
    /* Flush-race fix (see shard path): clear shard_dirty only if no op landed
     * during the unlocked serialize/PUT window; otherwise keep it dirty so the
     * next flush commits the straggler instead of losing it on a rebuild. */
    if (ex->inode_count == snap_icount && ex->layout_epoch == snap_lepoch)
        ex->shard_dirty = 0;
    s->export_meta_dirty = 1;
    server_save_export(s, ex);
    s->export_meta_dirty = 0;
    uint32_t commit_pc0 = (ex->root.page_cis && ex->root.page_count > 0)
                              ? ex->root.page_cis[0]
                              : 0;
    uint32_t commit_nextci = ex->root.next_ci;
    efs_export_id_t commit_export_id = ex->root.id;
    pthread_mutex_unlock(&s->lock);
    fprintf(stderr,
            "meta-flush: committed export=%s gen=%llu pages=%u page0_ci=%u "
            "next_ci=%u (old_gen=%llu)\n",
            ex->name, (unsigned long long)new_gen, new_ino_pc, commit_pc0,
            commit_nextci, (unsigned long long)old_gen);

    /* 2PC phase 2: the root is committed locally and peers hold it as a
     * pending prepare. Broadcast the commit so they promote + persist + GC
     * it. Best-effort: a peer that misses this converges via GET_META_ROOT
     * catchup (which only ever serves committed roots). The reply must be
     * drained so the pooled peer conn stays in sync. */
    {
        struct efs_msg_meta_commit cm;
        memset(&cm, 0, sizeof(cm));
        cm.export_id = commit_export_id;
        cm.gen = new_gen;
        /* Fingerprint computed at serialize time (root_buf is freed above). */
        memcpy(cm.root_sum, root_sum, EFS_HASH_SIZE);
        struct efs_node *cn = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
        if (cn) {
            pthread_mutex_lock(&s->lock);
            uint32_t cn_count = s->node_count;
            if (cn_count > EFS_MAX_NODES)
                cn_count = EFS_MAX_NODES;
            memcpy(cn, s->nodes, sizeof(struct efs_node) * cn_count);
            efs_node_id_t cn_self = s->id;
            pthread_mutex_unlock(&s->lock);
            for (uint32_t i = 0; i < cn_count; i++) {
                if (cn[i].id == cn_self)
                    continue;
                int fd = server_peer_conn_get(cn[i].addr, cn[i].port);
                if (fd < 0)
                    continue;
                uint8_t type;
                void *reply = NULL;
                uint32_t reply_len = 0;
                int net_ok =
                    (efs_send_msg(fd, EFS_MSG_META_COMMIT, &cm,
                                  (uint32_t)sizeof(cm)) == 0 &&
                     efs_recv_msg(fd, &type, &reply, &reply_len) == 0);
                free(reply);
                if (net_ok)
                    server_peer_conn_release(cn[i].addr, cn[i].port, fd);
                else
                    server_peer_conn_drop(cn[i].addr, cn[i].port, fd);
            }
            free(cn);
        }
    }

    /* Retire the previous generation's metadata pages (best-effort). */
    if (old_gen != new_gen) {
        if (old_cis && new_cis) {
            /* Old root was CoW (EFSR v7): reclaim the cis it referenced but
             * the new root no longer does. */
            server_gc_meta_cow_pages(s, ex, table_ino, old_cis, old_cis_count,
                                     new_cis, new_cis_count);
        } else if (old_ino_pc > 0 || old_ch_pc > 0) {
            /* Old root was dual-slot (v6 and earlier): only pages beyond the
             * new generation's page count are dead; in-range fragments stay
             * for dirty-page skip reuse by future same-parity gens. */
            server_gc_meta_slot_pages(s, ex, old_gen, old_ino_pc, old_ch_pc,
                                      new_ino_pc, new_ch_pc);
        }
    }
    free(old_cis);
    free(new_cis);
    return 0;
}

int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex)
{
    /* Prefer fragmented persist so peers store EFSR + pages, not N full copies. */
    return server_flush_fragmented_meta(s, ex);
}

/* Join-time bootstrap: fetch EVERY export the peer serves (LIST_EXPORTS,
 * then a named GET_META per export). The old single unnamed GET_META only
 * ever carried the peer's exports[0], so a joining node never learned the
 * other exports until their owners happened to flush. */
int server_fetch_metadata_from(struct efsd_server *s, const char *host, uint16_t port)
{
    int fd = server_peer_conn_get(host, port);
    if (fd < 0)
        return -1;

    if (efs_send_msg(fd, EFS_MSG_LIST_EXPORTS, NULL, 0) != 0) {
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_LIST_EXPORTS_REPLY ||
        payload_len < sizeof(struct efs_msg_list_exports_reply)) {
        free(payload);
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    struct efs_msg_list_exports_reply list;
    memcpy(&list, payload, sizeof(list));
    free(payload);
    if (list.export_count > EFS_MAX_EXPORTS)
        list.export_count = EFS_MAX_EXPORTS;
    if (list.export_count == 0) {
        server_peer_conn_release(host, port, fd);
        return -1;
    }

    int rc = -1;
    for (uint32_t i = 0; i < list.export_count; i++) {
        list.exports[i].name[EFS_MAX_NAME - 1] = '\0';
        const char *ename = list.exports[i].name;
        if (!ename[0])
            continue;
        if (efs_send_msg(fd, EFS_MSG_GET_META, ename,
                         (uint32_t)strlen(ename) + 1) != 0)
            break;
        payload = NULL;
        payload_len = 0;
        if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
            type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
            free(payload);
            break;
        }

        pthread_mutex_lock(&s->lock);
        int one = -1;
        if (efs_meta_blob_is_root(payload, payload_len)) {
            struct efs_export_root root;
            memset(&root, 0, sizeof(root));
            if (efs_export_root_deserialize(&root, payload, payload_len) == 0) {
                struct efs_export *ex =
                    server_get_export_create(s, root.id, root.name);
                if (ex) {
                    ex->meta_fragmented = 1;
                    efs_export_root_move(&ex->root, &root);
                    ex->id = ex->root.id;
                    strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
                    ex->next_ino = ex->root.next_ino;
                    if (efs_chunk_size_valid(ex->root.chunk_size))
                        ex->chunk_size = ex->root.chunk_size;
                    /* Never rebuild here: this runs on the startup rejoin
                     * path (main thread, before accept()) and on the JOIN
                     * handler. A synchronous 2+1 page rebuild blocks accept()
                     * for the whole sweep, and when every node restarts
                     * together each peer fetch waits out the full I/O timeout
                     * because the peer's accept loop isn't up yet — the
                     * cluster deadlocks for hours. Install the root, flag the
                     * rebuild, and let the meta catch-up thread do it once we
                     * can serve. */
                    ex->meta_needs_rebuild = (ex->root.page_count > 0);
                    if (ex->meta_needs_rebuild)
                        fprintf(stderr,
                                "FENCE-SITE fetch-from-peer %s:%u gen=%llu\n",
                                host, port,
                                (unsigned long long)ex->root.generation);
                    one = 0;
                } else {
                    efs_export_root_free(&root);
                }
            }
        } else if (efs_meta_blob_is_export(payload, payload_len)) {
            struct efs_export tmp;
            efs_export_init(&tmp, 1, "pending");
            if (efs_export_deserialize(&tmp, payload, payload_len) == 0) {
                struct efs_export *ex =
                    server_get_export_create(s, tmp.id ? tmp.id : 1,
                                             tmp.name[0] ? tmp.name
                                                         : "pending");
                if (ex) {
                    efs_export_free(ex);
                    *ex = tmp;
                    memset(&tmp, 0, sizeof(tmp));
                    ex->meta_fragmented = 0;
                    server_save_export(s, ex);
                    one = 0;
                }
            }
            efs_export_free(&tmp);
        }
        pthread_mutex_unlock(&s->lock);
        free(payload);
        if (one == 0)
            rc = 0;
    }
    server_peer_conn_release(host, port, fd);
    return rc;
}

/* Install a newer EFSR root from a peer without rebuilding (catch-up does that).
 * Returns 1 if a newer root was installed, 0 if unchanged/skipped, -1 on error. */
static int catchup_install_newer_root(struct efsd_server *s, const void *payload,
                                      uint32_t payload_len)
{
    if (!s || !payload || payload_len == 0)
        return -1;
    if (!efs_meta_blob_is_root(payload, payload_len) &&
        !efs_meta_blob_is_export(payload, payload_len))
        return 0;

    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    size_t used = 0;
    const char *efsm = NULL;
    uint32_t efsm_len = 0;
    if (efs_meta_blob_is_root(payload, payload_len)) {
        if (efs_export_root_deserialize_used(&root, payload, payload_len,
                                             &used) != 0)
            return -1;
        if (used > 0 && used < payload_len &&
            efs_meta_blob_is_export((const char *)payload + used,
                                    payload_len - used)) {
            efsm = (const char *)payload + used;
            efsm_len = payload_len - (uint32_t)used;
        }
    } else {
        return 0;
    }

    pthread_mutex_lock(&s->lock);
    /* Multi-export: the root lands in ITS OWN export's slot (find-or-create
     * by id). Keying off exports[0] morphed slot 0 into whatever root
     * arrived last and diverged multi-export clusters. */
    struct efs_export *ex = server_get_export_create(s, root.id, root.name);
    if (!ex) {
        pthread_mutex_unlock(&s->lock);
        efs_export_root_free(&root);
        return 0;
    }
    int ei = server_export_index_locked(s, ex);
    /* Unflushed RPC mutations mean our in-memory table is ahead of ANY root
     * a peer can serve: roots are made by flushing a snapshot that predates
     * those ops. Installing this root (and especially rebuilding/adopting
     * from it) would wipe the dirty state — the primary's own in-flight
     * flush commits the root to peers before root_move updates it locally,
     * so its catchup can see that newer root and rebuild away newer
     * in-memory reports (mc_stress: cdir files back to size 0). Defer; the
     * flush advances our local root past this gen and the next poll is a
     * no-op. */
    if (ei >= 0 &&
        __atomic_load_n(&s->rpc_dirty_ops[ei], __ATOMIC_RELAXED) > 0) {
        pthread_mutex_unlock(&s->lock);
        efs_export_root_free(&root);
        return 0;
    }
    /* Installing a committed root supersedes any pending 2PC prepare at or
     * below its gen (the writer's COMMIT may never arrive here; this root
     * came from a peer that already committed it). */
    if (ei >= 0 && s->pending_valid[ei] &&
        s->pending_root[ei].generation <= root.generation) {
        efs_export_root_free(&s->pending_root[ei]);
        memset(&s->pending_root[ei], 0, sizeof(s->pending_root[ei]));
        s->pending_valid[ei] = 0;
        free(s->pending_blob[ei]);
        s->pending_blob[ei] = NULL;
        s->pending_blob_len[ei] = 0;
    }
    /* Extras-only refresh (same shard-0 pages, newer gen): adopt the gen and
     * merge the extra-shard descriptors without touching the live tables. */
    if (ex->meta_fragmented &&
        efs_export_root_same_pages(&root, &ex->root)) {
        ex->root.generation = root.generation;
        efs_export_merge_extra_roots(ex, &root);
        s->export_meta_dirty = 1;
        server_save_export(s, ex);
        pthread_mutex_unlock(&s->lock);
        efs_export_root_free(&root);
        return 1;
    }
    /* A joiner can load a leftover empty bits=0 efs-s3 (destroy missed this
     * node). That must not win against a peer's sharded root or extra-shard
     * restart never rebuilds extras (GETATTR size 0 on shards 2/6). */
    int local_empty = !ex->meta_fragmented ||
                      (ex->root.page_count == 0 && ex->inode_count <= 1 &&
                       ex->root.shard_bits == 0);
    int incoming_real = root.shard_bits || root.generation > 0 ||
                        root.page_count > 0;
    if (local_empty && incoming_real) {
        fprintf(stderr,
                "meta-catchup: adopt peer export=%s gen=%llu bits=%u "
                "(local was empty bits=0)\n",
                root.name[0] ? root.name : ex->name,
                (unsigned long long)root.generation, root.shard_bits);
    } else if (ex->meta_fragmented && root.generation <= ex->root.generation) {
        int adopt_same = efsm && efsm_len &&
                         root.generation == ex->root.generation &&
                         ex->meta_needs_rebuild;
        if (!adopt_same) {
            pthread_mutex_unlock(&s->lock);
            efs_export_root_free(&root);
            return 0;
        }
    }
    /* Fresh mkfs is gen=0 + shard_bits>0 (no pages yet). That is a real
     * export: skip only a worthless empty root. Bailing on every gen=0
     * left joiners at bits=0 after the "adopt peer" log, so CREATE_SHARD
     * for shards 1–7 came back NOT_PRIMARY and file create was EIO. */
    if (!ex->meta_fragmented && root.generation == 0 && !incoming_real) {
        pthread_mutex_unlock(&s->lock);
        efs_export_root_free(&root);
        return 0;
    }

    uint64_t new_gen = root.generation;
    ex->meta_fragmented = 1;
    /* Never orphan a shard: carry forward extra-shard descriptors the
     * incoming root lacks (per-shard higher gen wins). The rebuild
     * reinstalls shard tables from these descriptors, so dropping one here
     * loses that shard permanently (the efs-s3 clobber). */
    (void)efs_export_root_maxmerge_extras(&root, &ex->root);
    efs_export_root_move(&ex->root, &root);
    ex->id = ex->root.id;
    strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
    ex->next_ino = ex->root.next_ino;
    if (efs_chunk_size_valid(ex->root.chunk_size))
        ex->chunk_size = ex->root.chunk_size;
    /* Keep the live tables serving PUTs. Deserialize the 97MB+ blob
     * unlocked, then adopt. The old path parsed 800k chunks under s->lock
     * and stalled every PUT_CHUNK (sw-1m collapsed to ~140 MiB/s). */
    int have_blob = (efsm && efsm_len);
    if (!have_blob && ex->root.page_count > 0) {
        ex->meta_needs_rebuild = 1;
        fprintf(stderr, "FENCE-SITE catchup-root-no-blob gen=%llu\n",
                (unsigned long long)ex->root.generation);
    }
    s->export_meta_dirty = 1;
    server_save_export(s, ex);
    pthread_mutex_unlock(&s->lock);

    if (have_blob) {
        struct efs_export tmp;
        memset(&tmp, 0, sizeof(tmp));
        int drc = efs_export_deserialize(&tmp, efsm, efsm_len);
        pthread_mutex_lock(&s->lock);
        if (drc == 0 && ex->root.generation == new_gen) {
            efs_export_adopt_tables(ex, &tmp);
            ex->meta_fragmented = 1;
            ex->meta_needs_rebuild = 0;
            free(ex->gm_blob);
            ex->gm_blob = NULL;
        } else if (drc != 0 && ex->root.generation == new_gen &&
                   ex->root.page_count > 0) {
            ex->meta_needs_rebuild = 1;
            fprintf(stderr, "FENCE-SITE catchup-blob-deserialize-fail\n");
        }
        s->export_meta_dirty = 1;
        server_save_export(s, ex);
        pthread_mutex_unlock(&s->lock);
        efs_export_free(&tmp);
    }

    fprintf(stderr,
            "meta-catchup: installed newer root gen=%llu extras=%u bits=%u\n",
            (unsigned long long)new_gen, ex->root.extra_shard_count,
            ex->root.shard_bits);
    if (ex->root.shard_bits)
        server_rebuild_owned_extras(s, ex);
    return 1;
}

/* Same-gen extras refresh from a peer ROOT. Extra-shard owners publish
 * descriptors without bumping the main gen, so a joiner that adopted a
 * primary root missing extras (extra_count=0 at gen N) would otherwise
 * skip GET_META forever (peer_gen==our_gen && !stuck) and GETATTR its
 * own shards as size 0. GET_META_ROOT already carries the nested EFSRs. */
static int catchup_merge_peer_extras(struct efsd_server *s, const char *name,
                                     const struct efs_export_root *incoming)
{
    if (!s || !incoming || incoming->extra_shard_count == 0)
        return 0;

    pthread_mutex_lock(&s->lock);
    struct efs_export *ex = incoming->id ? server_get_export(s, incoming->id)
                                         : NULL;
    if (!ex && name && name[0])
        ex = server_find_export_no_create(s, name);
    if (!ex || !ex->root.shard_bits) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    uint32_t before_n = ex->root.extra_shard_count;
    uint64_t before_g = 0;
    for (uint32_t i = 0; i < before_n; i++)
        if (ex->root.extra_roots)
            before_g += ex->root.extra_roots[i].generation;

    efs_export_merge_extra_roots(ex, incoming);

    uint32_t after_n = ex->root.extra_shard_count;
    uint64_t after_g = 0;
    for (uint32_t i = 0; i < after_n; i++)
        if (ex->root.extra_roots)
            after_g += ex->root.extra_roots[i].generation;
    if (after_n == before_n && after_g == before_g) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    s->export_meta_dirty = 1;
    server_save_export(s, ex);
    fprintf(stderr,
            "meta-catchup: extras-merge export=%s %u->%u "
            "(peer extras=%u gen=%llu)\n",
            ex->name, before_n, after_n, incoming->extra_shard_count,
            (unsigned long long)incoming->generation);
    pthread_mutex_unlock(&s->lock);
    server_rebuild_owned_extras(s, ex);
    return 1;
}

/* Best-effort GET_META from ALL peers, PER EXPORT (named requests — the
 * unnamed variant serves only the peer's exports[0], which diverged
 * multi-export clusters). Install the highest-generation root found per
 * export (install function still rejects anything not strictly newer).
 * Polling only the first peer could strand a lagging node when that peer
 * is behind. */
static int catchup_poll_peer_root(struct efsd_server *s)
{
    struct efs_node nodes[EFS_MAX_NODES];
    char names[EFS_MAX_EXPORTS][EFS_MAX_NAME];
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
    efs_node_id_t self = s->id;
    uint32_t ec = s->export_count;
    if (ec > EFS_MAX_EXPORTS)
        ec = EFS_MAX_EXPORTS;
    for (uint32_t e = 0; e < ec; e++) {
        memcpy(names[e], s->exports[e].name, EFS_MAX_NAME);
        names[e][EFS_MAX_NAME - 1] = '\0';
    }
    pthread_mutex_unlock(&s->lock);

    int best = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == self)
            continue;
        const char *host = nodes[i].addr;
        uint16_t port = nodes[i].port;
        if (!host[0] || port == 0)
            continue;

        int fd = server_peer_conn_get(host, port);
        if (fd < 0)
            continue;
        int conn_ok = 1;
        for (uint32_t e = 0; e < ec && conn_ok; e++) {
            if (!names[e][0])
                continue;
            /* Phase 1: root-only poll. A full GET_META here cost every
             * server 3 x ~1 GiB of memcpy + network every 2 s even at rest
             * — the memmove storm pinned idle efsd at 60%+ CPU and starved
             * client IO. */
            if (efs_send_msg(fd, EFS_MSG_GET_META_ROOT, names[e],
                             (uint32_t)strlen(names[e]) + 1) != 0) {
                conn_ok = 0;
                break;
            }
            uint8_t type;
            void *payload = NULL;
            uint32_t payload_len = 0;
            if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
                type != EFS_MSG_GET_META_ROOT_REPLY) {
                free(payload);
                conn_ok = 0;
                break;
            }
            if (payload_len == 0) {
                free(payload);
                /* ROOT is omitted when the peer has not marked the export
                 * fragmented. A leftover empty bits=0 table still has to
                 * pull GET_META or extra-shard restart never sees extras. */
                pthread_mutex_lock(&s->lock);
                struct efs_export *lex =
                    server_find_export_no_create(s, names[e]);
                int pull_empty = lex && !lex->root.shard_bits &&
                                 lex->root.page_count == 0 &&
                                 lex->inode_count <= 1;
                pthread_mutex_unlock(&s->lock);
                if (!pull_empty)
                    continue;
                fprintf(stderr,
                        "meta-catchup: poll %s peer=%u empty-local, "
                        "GET_META despite empty ROOT\n",
                        names[e], (unsigned)nodes[i].id);
                goto pull_full;
            }

            uint64_t peer_gen = 0;
            efs_export_id_t peer_id = 0;
            uint32_t peer_extras = 0;
            uint32_t peer_bits = 0;
            if (efs_meta_blob_is_root(payload, payload_len)) {
                struct efs_export_root r;
                memset(&r, 0, sizeof(r));
                if (efs_export_root_deserialize_used(&r, payload,
                                                     payload_len, NULL) == 0) {
                    peer_gen = r.generation;
                    peer_id = r.id;
                    peer_extras = r.extra_shard_count;
                    peer_bits = r.shard_bits;
                    int m = catchup_merge_peer_extras(s, names[e], &r);
                    if (m > best)
                        best = m;
                }
                efs_export_root_free(&r);
            }
            free(payload);

            pthread_mutex_lock(&s->lock);
            struct efs_export *ex = peer_id ? server_get_export(s, peer_id)
                                            : NULL;
            if (!ex)
                ex = server_find_export_no_create(s, names[e]);
            uint64_t our_gen = ex ? ex->root.generation : 0;
            int stuck = ex ? ex->meta_needs_rebuild : 0;
            uint32_t our_extras = ex ? ex->root.extra_shard_count : 0;
            uint32_t our_bits = ex ? ex->root.shard_bits : 0;
            pthread_mutex_unlock(&s->lock);

            /* Phase 2: full blob only when the peer is strictly newer, or
             * same generation while our rebuild is stuck (peer may hold
             * live tables that fragment rebuild cannot recover).
             * Also pull when we have an empty leftover and the peer has
             * any real export — gen>0 OR shard_bits (fresh mkfs is gen=0
             * + bits=3, no pages). peer_gen>0 alone left joiners at
             * bits=0; CREATE_SHARD then returned NOT_PRIMARY and file
             * create was EIO (basic_empty_file on a clean cluster). */
            int local_empty = !ex ||
                              (!ex->root.shard_bits &&
                               ex->root.page_count == 0 &&
                               ex->inode_count <= 1);
            int want = (peer_gen > our_gen) ||
                       (peer_gen > 0 && peer_gen == our_gen && stuck) ||
                       (local_empty && (peer_gen > 0 || peer_bits > 0)) ||
                       (our_bits == 0 && peer_bits > 0) ||
                       (ex && ex->root.shard_bits &&
                        peer_extras > our_extras);
            if (!want)
                continue;
        pull_full:
            if (efs_send_msg(fd, EFS_MSG_GET_META, names[e],
                             (uint32_t)strlen(names[e]) + 1) != 0) {
                conn_ok = 0;
                break;
            }
            payload = NULL;
            payload_len = 0;
            if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
                type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
                free(payload);
                conn_ok = 0;
                break;
            }

            int rc = catchup_install_newer_root(s, payload, payload_len);
            free(payload);
            if (rc > best)
                best = rc;
        }
        if (conn_ok)
            server_peer_conn_release(host, port, fd);
        else
            server_peer_conn_drop(host, port, fd);
    }
    return best;
}

static void *meta_catchup_thread(void *arg)
{
    struct efsd_server *s = arg;

    while (s->running) {
        int did_work = 0;
        int need[EFS_MAX_EXPORTS];
        uint32_t ec = 0;

        memset(need, 0, sizeof(need));
        pthread_mutex_lock(&s->lock);
        ec = s->export_count;
        if (ec > EFS_MAX_EXPORTS)
            ec = EFS_MAX_EXPORTS;
        for (uint32_t e = 0; e < ec; e++) {
            struct efs_export *ex = &s->exports[e];
            /* Rebuild when fenced, or when RAM is a hollow 1-inode table
             * but the root advertises a real page blob (missed rebuild /
             * stale .efsm). Do NOT key off inode_count<=1 alone: an empty
             * export stays at 1 inode. A fresh mkfs is 2 pages (~128 KiB);
             * the old 64 KiB cutoff spun rebuild forever and wedged
             * CREATE_SHARD (BUSY / FUSE hang on empty-file create). */
            /* DIRTY-REBUILD: never rebuild a main table with unflushed ops --
             * it is ahead of the committed root, so a rebuild only loses the
             * ops (and rolls next_ino back). The flush clears shard_dirty and
             * this picks the table up on the next pass. */
            need[e] = (ex->meta_fragmented && ex->root.page_count > 0 &&
                       !ex->shard_dirty &&
                       (ex->meta_needs_rebuild ||
                        (ex->inode_count <= 1 &&
                         ex->root.blob_len > (1u << 20))));
        }
        pthread_mutex_unlock(&s->lock);

        int rebuild_failed = 0;
        for (uint32_t e = 0; e < ec; e++) {
            if (!need[e] || !s->running)
                continue;
            struct efs_export *ex = &s->exports[e];
            int rc = server_rebuild_export_from_pages(s, ex);
            if (rc == EFS_OK) {
                pthread_mutex_lock(&s->lock);
                /* Repair torn-era counters on every rebuild: pages holed by
                 * pre-election dual-slot tears could reset next_ino below
                 * live inos (create collisions) and leave stale rollups.
                 * next_ino only ever moves forward; rollups are derived
                 * state and always safe to recompute. */
                uint64_t maxino = 0;
                for (uint64_t ri = 0; ri < ex->inode_count; ri++)
                    if (ex->inodes[ri].ino > maxino)
                        maxino = ex->inodes[ri].ino;
                if (ex->next_ino <= maxino) {
                    fprintf(stderr,
                            "meta-repair: export=%s next_ino %llu -> %llu\n",
                            ex->name, (unsigned long long)ex->next_ino,
                            (unsigned long long)(maxino + 1));
                    ex->next_ino = maxino + 1;
                }
                if (ex->root.next_ino <= maxino)
                    ex->root.next_ino = maxino + 1;
                efs_export_recompute_rollups(ex);
                /* Fresh table for the same generation: the GET_META cache
                 * (possibly a root-only reply from mid-rebuild) is stale. */
                free(ex->gm_blob);
                ex->gm_blob = NULL;
                s->epoch++;
                /* The main rebuild's efs_export_free wiped shard_tabs.
                 * Reinstall the extra-shard roots (marks each table
                 * meta_needs_rebuild) and rebuild them below — otherwise a
                 * restarted server lazily recreates its shard tables EMPTY
                 * and every non-zero-shard file reads back as size 0. */
                server_save_export(s, ex);
                pthread_mutex_unlock(&s->lock);
                fprintf(stderr, "meta-catchup: rebuilt export=%s\n", ex->name);
                server_rebuild_owned_extras(s, ex);
                did_work = 1;
            } else if (rc == EFS_ERR_PROTO) {
                /* Gen raced with PUT_META, or a CoW rebuild hit the
                 * catchup-vs-GC race (a peer reclaimed this gen's dead cis).
                 * Retry promptly AND re-poll peers for the newest root (whose
                 * cis are live) — without the poll, a stuck-at-old-gen server
                 * would rebuild the same reclaimed gen forever. */
                did_work = 1;
                rebuild_failed = 1;
            } else {
                fprintf(stderr, "meta-catchup: rebuild export=%s rc=%d\n",
                        ex->name, rc);
                /* Decode/IO failure: back off; leave meta_needs_rebuild set. */
                rebuild_failed = 1;
            }
        }

        /* Owned extra shards only. Peers keep descriptors and do not
         * assemble those tables (that was the extras catchup storm). */
        for (uint32_t e = 0; e < ec; e++) {
            if (!s->running)
                break;
            struct efs_export *ex = &s->exports[e];
            if (!ex->root.shard_bits || ex->root.extra_shard_count == 0)
                continue;
            uint32_t before = 0, after = 0;
            pthread_mutex_lock(&s->lock);
            for (uint32_t i = 1; ex->shard_tabs && i < ex->shard_tab_cap; i++)
                if (ex->shard_tabs[i] && !ex->shard_tabs[i]->meta_needs_rebuild &&
                    ex->shard_tabs[i]->inode_count > 0)
                    before++;
            pthread_mutex_unlock(&s->lock);
            server_rebuild_owned_extras(s, ex);
            pthread_mutex_lock(&s->lock);
            for (uint32_t i = 1; ex->shard_tabs && i < ex->shard_tab_cap; i++)
                if (ex->shard_tabs[i] && !ex->shard_tabs[i]->meta_needs_rebuild &&
                    ex->shard_tabs[i]->inode_count > 0)
                    after++;
            pthread_mutex_unlock(&s->lock);
            if (after > before)
                did_work = 1;
        }

        int any_dirty = 0;
        pthread_mutex_lock(&s->lock);
        uint32_t node_count = s->node_count;
        for (uint32_t e = 0; e < s->export_count && e < EFS_MAX_EXPORTS; e++) {
            struct efs_export *ex = &s->exports[e];
            if (ex->meta_fragmented && ex->root.page_count > 0 &&
                ex->meta_needs_rebuild) {
                any_dirty = 1;
                break;
            }
        }
        pthread_mutex_unlock(&s->lock);

        /* Poll peers when clean (normal catch-up) OR when a rebuild just
         * failed: a peer holding live tables at the same generation can
         * ship its blob, which fragment rebuild may be unable to recover. */
        if ((!any_dirty || rebuild_failed) && node_count > 1 && s->running) {
            /* catchup_poll only walks local names. A joiner up before
             * mkfs has export_count==0 and would never learn efs-test. */
            uint32_t local_ec = 0;
            struct efs_node seed;
            memset(&seed, 0, sizeof(seed));
            pthread_mutex_lock(&s->lock);
            local_ec = s->export_count;
            for (uint32_t i = 0; i < s->node_count; i++) {
                if (s->nodes[i].id != s->id && s->nodes[i].addr[0] &&
                    s->nodes[i].port) {
                    seed = s->nodes[i];
                    break;
                }
            }
            pthread_mutex_unlock(&s->lock);
            if (local_ec == 0 && seed.port)
                (void)server_fetch_metadata_from(s, seed.addr, seed.port);
            int prc = catchup_poll_peer_root(s);
            if (prc > 0)
                did_work = 1;
        }

        if (!s->running)
            break;
        /* Debounce rebuild storms: continuous PUT_META under load used to
         * rebuild hundreds of times/min. Peer TCP is pooled, but pacing still
         * keeps catch-up from thrashing. Hot-path clients are unaffected. */
        /* Failed rebuilds used to retry every 2s and leak peer fds until
         * accept() hit EMFILE — clients then sat in CLOSE-WAIT with no quorum. */
        if (!did_work && any_dirty)
            sleep(15);
        else if (did_work)
            sleep(1);
        else
            sleep(2);
    }
    return NULL;
}

void server_start_meta_catchup(struct efsd_server *s)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&s->meta_catchup_tid, &attr, meta_catchup_thread, s);
    pthread_attr_destroy(&attr);
}

/* Phase 2a: the metadata primary is the lowest-id live node. This server is
 * live (it is running), so start from s->id and look for a lower-id live
 * peer. Funneling all RPC mutations to one writer keeps replicas convergent
 * (two concurrent flushes would collide on CoW cis from the same root). */
efs_node_id_t server_meta_primary_id_locked(struct efsd_server *s)
{
    efs_node_id_t best = s->id;
    for (uint32_t i = 0; i < s->node_count; i++) {
        efs_node_id_t id = s->nodes[i].id;
        if (id == s->id)
            continue;
        if (server_node_is_down_locked(s, id))
            continue;
        if (id < best)
            best = id;
    }
    return best;
}

int server_is_meta_primary_locked(struct efsd_server *s)
{
    return server_meta_primary_id_locked(s) == s->id;
}

void server_meta_mark_rpc_dirty_locked(struct efsd_server *s, uint32_t eidx)
{
    if (eidx >= EFS_MAX_EXPORTS)
        return;
    /* Blocker 3: under the per-shard partition this is called holding a shard
     * lock, NOT s->lock, so the bump and the threshold sum are atomic (the
     * flush thread reads/resets with atomics too). The condvar signal is not
     * sent under s->lock here, so it can race — but the flush thread's timed
     * wait covers a missed wake; the signal is only an early-flush hint. */
    (void)__atomic_add_fetch(&s->rpc_dirty_ops[eidx], 1, __ATOMIC_RELAXED);
    uint64_t total = 0;
    for (uint32_t e = 0; e < s->export_count && e < EFS_MAX_EXPORTS; e++)
        total += __atomic_load_n(&s->rpc_dirty_ops[e], __ATOMIC_RELAXED);
    if (total >= EFS_META_FLUSH_OPS)
        pthread_cond_signal(&s->rpc_dirty_cv);
}

/* Phase 2a: batched server-side flush of RPC-driven dirty exports. Wakes on
 * the dirty signal (EFS_META_FLUSH_OPS) or the EFS_META_FLUSH_MS batch window,
 * and flushes each dirty export via server_flush_fragmented_meta — but only
 * when this server is the metadata primary (a non-primary must not flush, or
 * two writers could tear a generation). */
static void *meta_flush_thread(void *arg)
{
    struct efsd_server *s = arg;
    while (1) {
        pthread_mutex_lock(&s->lock);
        if (!s->running) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + EFS_META_FLUSH_MS * 1000000ull;
        ts.tv_sec += (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(&s->rpc_dirty_cv, &s->lock, &ts);
        if (!s->running) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        uint32_t dirty[EFS_MAX_EXPORTS];
        uint32_t ndirty = 0;
        int can_flush_primary = server_is_meta_primary_locked(s);
        for (uint32_t e = 0; e < s->export_count && e < EFS_MAX_EXPORTS;
             e++) {
            /* Per-export flush eligibility: the primary flushes everything;
             * a non-primary flushes only sharded exports (as an extra-shard
             * owner). Keying this off exports[0] stranded the shard tables
             * of every other export on non-primary nodes. */
            int can_flush = can_flush_primary ||
                            (s->exports[e].root.shard_bits &&
                             s->exports[e].root.shard_count > 1);
            if (!can_flush)
                continue;
            if (__atomic_load_n(&s->rpc_dirty_ops[e], __ATOMIC_RELAXED) > 0) {
                int extra_hold = 0;
                if (s->exports[e].root.shard_bits &&
                    s->exports[e].shard_tabs) {
                    efs_node_id_t xlive[EFS_MAX_NODES];
                    uint32_t xnlive = server_live_ids_locked(s, xlive);
                    uint32_t xsc = s->exports[e].root.shard_count
                                       ? s->exports[e].root.shard_count
                                       : 1;
                    for (uint32_t i = 1; i < s->exports[e].shard_tab_cap &&
                         i < EFS_META_MAX_SHARDS; i++) {
                        struct efs_export *tab = s->exports[e].shard_tabs[i];
                        if (!tab || !tab->meta_needs_rebuild ||
                            tab->root.page_count == 0)
                            continue;
                        if (efs_shard_owner_of(i, xsc, xlive, xnlive) ==
                            s->id) {
                            extra_hold = 1;
                            break;
                        }
                    }
                }
                if ((s->exports[e].meta_needs_rebuild && can_flush_primary) ||
                    extra_hold) {
                    /* Keep the dirty mark: dropping it made creates look
                     * successful while flush never ran (RAM-only, gen
                     * stuck). Retry after catchup clears needs_rebuild. */
                    fprintf(stderr,
                            "meta-flush: export=%s hold %llu op(s) until "
                            "rebuild gen=%llu extra_hold=%d\n",
                            s->exports[e].name,
                            (unsigned long long)__atomic_load_n(
                                &s->rpc_dirty_ops[e], __ATOMIC_RELAXED),
                            (unsigned long long)
                                s->exports[e].root.generation,
                            extra_hold);
                    continue;
                }
                dirty[ndirty++] = e;
                __atomic_store_n(&s->rpc_dirty_ops[e], 0, __ATOMIC_RELAXED);
            }
        }
        pthread_mutex_unlock(&s->lock);

        /* Flush outside the lock: server_flush_fragmented_meta does network
         * I/O (page PUTs + root PUT_META) and re-takes s->lock internally. */
        for (uint32_t i = 0; i < ndirty; i++) {
            if (!s->running)
                break;
            if (server_flush_fragmented_meta(s, &s->exports[dirty[i]]) != 0) {
                /* Flush failed (no quorum / net): re-mark dirty so the next
                 * window retries instead of losing the in-memory mutations.
                 * CAS so a concurrent op's mark (now lock-free) isn't lost. */
                pthread_mutex_lock(&s->lock);
                uint64_t zero = 0;
                (void)__atomic_compare_exchange_n(&s->rpc_dirty_ops[dirty[i]],
                                                  &zero, 1, 0,
                                                  __ATOMIC_RELAXED,
                                                  __ATOMIC_RELAXED);
                pthread_mutex_unlock(&s->lock);
            }
        }
    }
    return NULL;
}

void server_start_meta_flush(struct efsd_server *s)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&s->meta_flush_tid, &attr, meta_flush_thread, s) == 0)
        s->meta_flush_started = 1;
    pthread_attr_destroy(&attr);
}
