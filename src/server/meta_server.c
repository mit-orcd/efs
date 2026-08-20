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
                               efs_node_id_t node_id, uint32_t chunk_index,
                               uint32_t fragment_index, uint8_t *data)
{
    uint8_t checksum[EFS_HASH_SIZE];
    if (node_id == s->id) {
        uint32_t len = 0;
        if (server_read_fragment(s, ex, EFS_META_TABLE_INO, chunk_index,
                                 fragment_index, data, &len) == EFS_OK)
            return EFS_OK;
    } else {
        struct efs_node n;
        if (copy_node_by_id(s, node_id, &n) == 0 &&
            server_get_fragment_from_peer(n.addr, n.port, ex->id,
                                          EFS_META_TABLE_INO, chunk_index,
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
            if (server_read_fragment(s, ex, EFS_META_TABLE_INO, chunk_index,
                                     fragment_index, data, &len) == EFS_OK)
                return EFS_OK;
            continue;
        }
        if (server_get_fragment_from_peer(snap[i].addr, snap[i].port, ex->id,
                                          EFS_META_TABLE_INO, chunk_index,
                                          fragment_index, data, checksum) == EFS_OK)
            return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

static void gc_region_pages(struct efsd_server *s, struct efs_export *ex,
                            uint64_t dead_generation, int region,
                            uint32_t old_pc, uint32_t live_pc, uint32_t layout_ver)
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
        efs_place_fragments(s->nodes, s->node_count, EFS_META_TABLE_INO, ci,
                            placed);
        efs_node_id_t self = s->id;
        pthread_mutex_unlock(&s->lock);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            if (placed[fi] != self)
                continue;
            server_unlink_fragment_files(s, ex, EFS_META_TABLE_INO, ci,
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
        gc_region_pages(s, ex, dead_generation, EFS_META_REGION_INO,
                        old_ino_pages, live_ino_pages, ver);
        return;
    }
    gc_region_pages(s, ex, dead_generation, EFS_META_REGION_INO,
                    old_ino_pages, live_ino_pages, ver);
    gc_region_pages(s, ex, dead_generation, EFS_META_REGION_CHUNK,
                    old_chunk_pages, live_chunk_pages, ver);
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
        efs_place_fragments(s->nodes, s->node_count, EFS_META_TABLE_INO, ci,
                            placed);
        efs_node_id_t self = s->id;
        pthread_mutex_unlock(&s->lock);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            if (placed[fi] != self)
                continue;
            server_unlink_fragment_files(s, ex, EFS_META_TABLE_INO, ci,
                                         (uint32_t)fi);
        }
    }
    free(live);
}

static int put_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                             efs_node_id_t node_id, uint32_t chunk_index,
                             uint32_t fragment_index, const uint8_t *data,
                             const uint8_t *checksum);

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

void server_rebuild_fragmented_exports(struct efsd_server *s)
{
    for (uint32_t i = 0; i < s->export_count; i++) {
        struct efs_export *ex = &s->exports[i];
        if (!ex->meta_fragmented)
            continue;
        if (server_rebuild_export_from_pages(s, ex) == EFS_OK)
            continue;
        /* Fall back to pulling root+pages from any peer. */
        int ok = 0;
        for (uint32_t n = 0; n < s->node_count; n++) {
            if (s->nodes[n].id == s->id)
                continue;
            if (server_fetch_metadata_from(s, s->nodes[n].addr,
                                           s->nodes[n].port) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            fprintf(stderr,
                    "Export %s: root loaded; bulk meta pages not yet rebuilt\n",
                    ex->name);
        }
    }
}

int server_rebuild_export_from_pages(struct efsd_server *s, struct efs_export *ex)
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
    pthread_mutex_unlock(&s->lock);
    if (crc != EFS_OK)
        return crc;

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
         * holds exactly this page's content — memcpy instead of peer fetch. */
        int cached = 0;
        if (cur_gen == start_gen && ei >= 0 && pi < s->meta_blob_pages[ei] &&
            s->meta_blob_cache[ei] && s->meta_blob_sums[ei] &&
            memcmp(s->meta_blob_sums[ei] +
                       (size_t)pi * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE,
                   efs_export_root_checksum_const(root, pi, 0),
                   EFS_NUM_FRAGMENTS * EFS_HASH_SIZE) == 0) {
            size_t off = (size_t)pi * EFS_META_PAGE_SIZE;
            size_t avail = (off < s->meta_blob_cache_len[ei])
                               ? s->meta_blob_cache_len[ei] - off : 0;
            if (avail > 0) {
                size_t n = avail > EFS_META_PAGE_SIZE ? EFS_META_PAGE_SIZE
                                                      : avail;
                memcpy(pages[pi], s->meta_blob_cache[ei] + off, n);
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
            return EFS_ERR_PROTO;
        }
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
            efs_place_fragments(s->nodes, s->node_count, EFS_META_TABLE_INO, ci,
                                placed);
            efs_node_id_t self = s->id;
            pthread_mutex_unlock(&s->lock);

            int have[EFS_NUM_FRAGMENTS] = {0};

            for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
                int frc = fetch_meta_fragment(s, ex, placed[fi], ci, (uint32_t)fi,
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
                        int hrc = put_meta_fragment(s, ex, placed[fi], ci,
                                                    (uint32_t)fi,
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
    if (ex->root.generation != start_gen) {
        pthread_mutex_unlock(&s->lock);
        efs_export_free(&staging);
        free(blob);
        efs_export_root_free(&snap);
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
        pthread_mutex_unlock(&s->lock);
        return rc;
    }

    /* Commit: free the old table's contents and move the staged pointers in.
     * efs_export_free zeroes ex (including root.page_checksums); the staged
     * root is empty (EFSM carries no EFSR), then root_move reinstalls snap. */
    efs_export_free(ex);
    *ex = staging;
    memset(&staging, 0, sizeof(staging));

    /* Success: keep the assembled blob (plus this generation's page
     * checksums) as the incremental-rebuild cache so the next rebuild only
     * fetches pages whose checksums changed. */
    if (ei >= 0) {
        free(s->meta_blob_cache[ei]);
        free(s->meta_blob_sums[ei]);
        s->meta_blob_cache[ei] = (uint8_t *)blob;
        s->meta_blob_cache_len[ei] = (uint32_t)blob_len;
        s->meta_blob_cache_gen[ei] = start_gen;
        s->meta_blob_pages[ei] = page_count;
        size_t sum_n = (size_t)page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        s->meta_blob_sums[ei] = malloc(sum_n);
        if (s->meta_blob_sums[ei])
            memcpy(s->meta_blob_sums[ei], root->page_checksums, sum_n);
        else
            s->meta_blob_pages[ei] = 0; /* no sums: cache unusable, freed next */
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
    ex->next_ino = ex->root.next_ino;
    if (efs_chunk_size_valid(ex->root.chunk_size))
        ex->chunk_size = ex->root.chunk_size;
    /* Features are root-owned; the EFSM blob does not carry them, so restore
     * the export's working copy from the (server-preserved) root. */
    ex->features = ex->root.features;
    pthread_mutex_unlock(&s->lock);
    return EFS_OK;
}

static int put_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                             efs_node_id_t node_id, uint32_t chunk_index,
                             uint32_t fragment_index, const uint8_t *data,
                             const uint8_t *checksum)
{
    if (node_id == s->id) {
        /* Use sync writers to avoid pool re-entrancy during meta flush. */
        int rc = server_write_fragment_sync(s, ex, EFS_META_TABLE_INO, chunk_index,
                                            fragment_index, data,
                                            EFS_META_FRAGMENT_SIZE);
        if (rc != EFS_OK)
            return rc;
        return server_write_fragment_sum_sync(s, ex, EFS_META_TABLE_INO, chunk_index,
                                              fragment_index, checksum);
    }
    struct efs_node n;
    if (copy_node_by_id(s, node_id, &n) != 0)
        return EFS_ERR_NOT_FOUND;
    return server_put_fragment_to_peer(n.addr, n.port, ex->id, EFS_META_TABLE_INO,
                                       chunk_index, fragment_index, data, checksum);
}

/* One fragment PUT for the parallel meta flush. */
struct meta_put_job {
    struct efsd_server *s;
    struct efs_export *ex;
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
    j->rc = put_meta_fragment(j->s, j->ex, j->node, j->ci, (uint32_t)j->fi,
                              j->frag, j->checksum);
    return NULL;
}

/* Ensure peers have an export row before we PUT meta-page fragments.
 * Send a legacy empty EFSM shell (not EFSR) so clients never treat a
 * page-less bootstrap root as authoritative. */
static void bootstrap_export_on_peers(struct efsd_server *s, struct efs_export *ex)
{
    char *buf = NULL;
    size_t len = 0;
    if (efs_export_serialize(ex, &buf, &len) != EFS_OK)
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
                                               struct efs_export *ex);

/* Write all meta pages (2+1) and persist/replicate the EFSR root.
 * Serialized on s->meta_flush_mu: the meta-flush thread and a synchronous
 * REPORT_CHUNKS (fsync barrier) flush both call this, and without the mutex
 * both compute the same new_gen (= root.generation+1) and race to the peers —
 * the loser's root is rejected STALE (gen <= peer's), so the sync fsync sees
 * 0 peer acks and returns EIO even though the data commits on the retry. */
int server_flush_fragmented_meta(struct efsd_server *s, struct efs_export *ex)
{
    pthread_mutex_lock(&s->meta_flush_mu);
    int rc = server_flush_fragmented_meta_locked(s, ex);
    pthread_mutex_unlock(&s->meta_flush_mu);
    return rc;
}

static int server_flush_fragmented_meta_locked(struct efsd_server *s,
                                               struct efs_export *ex)
{
    /* Peers must know the export before accepting meta-page PUT_CHUNKs. */
    bootstrap_export_on_peers(s, ex);

    /* Serialize under the lock: handlers mutate ex (inode/chunk tables) and a
     * concurrent peer PUT_META can move ex->root, so an unlocked serialize is
     * a torn-blob / UAF window. */
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
    pthread_mutex_lock(&s->lock);
    if (ex->meta_needs_rebuild) {
        /* Fenced by a concurrent client-driven PUT_META (counts zeroed): the
         * in-memory table is stale and the catchup rebuild will overwrite it,
         * so serializing now would flush an empty/torn table and lose the
         * client's data. Refuse; the caller retries after the rebuild. */
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    if (efs_export_serialize_ex(ex, &blob, &blob_len, &ino_blob_len,
                                &chunk_blob_len) != EFS_OK) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    new_gen = ex->root.generation + 1;
    if (new_gen == 0)
        new_gen = 1;
    if (ex->meta_fragmented) {
        old_gen = ex->root.generation;
        old_ino_pc = ex->root.ino_page_count ? ex->root.ino_page_count
                                             : ex->root.page_count;
        old_ch_pc = ex->root.chunk_page_count;
    }
    pthread_mutex_unlock(&s->lock);

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

    for (uint32_t packed = 0; packed < root.page_count; packed++) {
        int region = (packed < root.ino_page_count) ? EFS_META_REGION_INO
                                                    : EFS_META_REGION_CHUNK;
        uint32_t pi = (region == EFS_META_REGION_INO)
                          ? packed
                          : packed - root.ino_page_count;
        const char *rbase = (region == EFS_META_REGION_INO)
                                ? blob
                                : blob + ino_blob_len;
        uint32_t rlen = (region == EFS_META_REGION_INO) ? ino_blob_len
                                                       : chunk_blob_len;
        if (efs_meta_extract_page(rbase, rlen, pi, page) != EFS_OK) {
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return -1;
        }
        efs_encode_chunk(page, EFS_META_PAGE_SIZE, EFS_META_PAGE_SIZE, fragments);
        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            efs_hash(fragments[fi], EFS_META_FRAGMENT_SIZE, checksums[fi]);

        /* CoW (EFSR v7): write the page to a fresh, never-referenced
         * chunk_index and record it in the new root's page_cis[]. */
        if (root.next_ci == UINT32_MAX) {
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return -1;
        }
        uint32_t ci = root.next_ci++;
        root.page_cis[packed] = ci;

        efs_node_id_t placed[EFS_NUM_FRAGMENTS];
        pthread_mutex_lock(&s->lock);
        efs_place_fragments(s->nodes, s->node_count, EFS_META_TABLE_INO, ci,
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
                prc = put_meta_fragment(s, ex, placed[fi], ci, (uint32_t)fi,
                                        fragments[fi], checksums[fi]);
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
    s->export_meta_dirty = 1;
    server_save_export(s, ex);
    s->export_meta_dirty = 0;
    pthread_mutex_unlock(&s->lock);

    /* Retire the previous generation's metadata pages (best-effort). */
    if (old_gen != new_gen) {
        if (old_cis && new_cis) {
            /* Old root was CoW (EFSR v7): reclaim the cis it referenced but
             * the new root no longer does. */
            server_gc_meta_cow_pages(s, ex, old_cis, old_cis_count,
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

int server_send_metadata_to(struct efsd_server *s, struct efs_export *ex,
                              const char *host, uint16_t port)
{
    char *buf = NULL;
    size_t len = 0;
    int rc;

    pthread_mutex_lock(&s->lock);
    if (ex->meta_fragmented)
        rc = efs_export_root_serialize(&ex->root, &buf, &len);
    else
        rc = efs_export_serialize(ex, &buf, &len);
    pthread_mutex_unlock(&s->lock);

    if (rc != EFS_OK || !buf)
        return -1;

    int fd = server_peer_conn_get(host, port);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    rc = efs_send_msg(fd, EFS_MSG_PUT_META, buf, (uint32_t)len);
    free(buf);
    if (rc != 0) {
        server_peer_conn_drop(host, port, fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    rc = efs_recv_msg(fd, &type, &payload, &payload_len);
    free(payload);
    if (rc != 0) {
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    server_peer_conn_release(host, port, fd);
    return (type == EFS_MSG_PUT_META_REPLY) ? 0 : -1;
}

int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex)
{
    /* Prefer fragmented persist so peers store EFSR + pages, not N full copies. */
    return server_flush_fragmented_meta(s, ex);
}

int server_fetch_metadata_from(struct efsd_server *s, const char *host, uint16_t port)
{
    int fd = server_peer_conn_get(host, port);
    if (fd < 0)
        return -1;

    if (efs_send_msg(fd, EFS_MSG_GET_META, NULL, 0) != 0) {
        server_peer_conn_drop(host, port, fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
        free(payload);
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    server_peer_conn_release(host, port, fd);

    pthread_mutex_lock(&s->lock);
    if (s->export_count == 0) {
        efs_export_init(&s->exports[0], 1, "default");
        s->export_count = 1;
    }
    struct efs_export *ex = &s->exports[0];

    int rc = -1;
    if (efs_meta_blob_is_root(payload, payload_len)) {
        struct efs_export_root root;
        memset(&root, 0, sizeof(root));
        if (efs_export_root_deserialize(&root, payload, payload_len) == 0) {
            ex->meta_fragmented = 1;
            efs_export_root_move(&ex->root, &root);
            ex->id = ex->root.id;
            strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
            ex->next_ino = ex->root.next_ino;
            if (efs_chunk_size_valid(ex->root.chunk_size))
                ex->chunk_size = ex->root.chunk_size;
            /* Never rebuild here: this runs on the startup rejoin path (main
             * thread, before accept()) and on the JOIN handler. A synchronous
             * 2+1 page rebuild blocks accept() for the whole sweep, and when
             * every node restarts together each peer fetch waits out the full
             * I/O timeout because the peer's accept loop isn't up yet — the
             * cluster deadlocks for hours. Install the root, flag the rebuild,
             * and let the meta catch-up thread do it once we can serve. */
            ex->meta_needs_rebuild = (ex->root.page_count > 0);
            if (ex->meta_needs_rebuild)
                fprintf(stderr, "FENCE-SITE fetch-from-peer %s:%u gen=%llu\n",
                        host, port, (unsigned long long)ex->root.generation);
            rc = 0;
        }
    } else if (efs_meta_blob_is_export(payload, payload_len)) {
        if (efs_export_deserialize(ex, payload, payload_len) == 0) {
            ex->meta_fragmented = 0;
            server_save_export(s, ex);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);

    free(payload);
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
    if (s->export_count == 0) {
        efs_export_init(&s->exports[0], root.id, root.name);
        s->export_count = 1;
    }
    struct efs_export *ex = &s->exports[0];
    /* Skip stale or identical generations once we already hold an EFSR root.
     * Exception: same generation, our fragment rebuild is stuck (e.g.
     * dual-slot pages vanished from every node), and the peer shipped its
     * live tables — adopt them to escape the rebuild dead-end. */
    if (ex->meta_fragmented && root.generation <= ex->root.generation) {
        int adopt_same = efsm && efsm_len &&
                         root.generation == ex->root.generation &&
                         ex->meta_needs_rebuild;
        if (!adopt_same) {
            pthread_mutex_unlock(&s->lock);
            efs_export_root_free(&root);
            return 0;
        }
    }
    if (!ex->meta_fragmented && root.generation == 0) {
        pthread_mutex_unlock(&s->lock);
        efs_export_root_free(&root);
        return 0;
    }

    uint64_t new_gen = root.generation;
    ex->meta_fragmented = 1;
    efs_export_root_move(&ex->root, &root);
    ex->id = ex->root.id;
    strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
    ex->next_ino = ex->root.next_ino;
    if (efs_chunk_size_valid(ex->root.chunk_size))
        ex->chunk_size = ex->root.chunk_size;
    if (efsm && efsm_len) {
        struct efs_export_root keep;
        memset(&keep, 0, sizeof(keep));
        if (efs_export_root_copy(&keep, &ex->root) == 0 &&
            efs_export_deserialize(ex, efsm, efsm_len) == 0) {
            efs_export_root_move(&ex->root, &keep);
            ex->meta_fragmented = 1;
            ex->meta_needs_rebuild = 0;
        } else {
            efs_export_root_move(&ex->root, &keep);
            ex->meta_fragmented = 1;
            if (ex->root.page_count > 0) {
                ex->meta_needs_rebuild = 1;
                fprintf(stderr, "FENCE-SITE catchup-blob-deserialize-fail\n");
            }
        }
    } else if (ex->root.page_count > 0) {
        ex->meta_needs_rebuild = 1;
        fprintf(stderr, "FENCE-SITE catchup-root-no-blob gen=%llu\n",
                (unsigned long long)ex->root.generation);
    }
    s->export_meta_dirty = 1;
    server_save_export(s, ex);
    pthread_mutex_unlock(&s->lock);

    fprintf(stderr, "meta-catchup: installed newer root gen=%llu\n",
            (unsigned long long)new_gen);
    return 1;
}

/* Best-effort GET_META from ALL peers; install the highest-generation root
 * found (install function still rejects anything not strictly newer). Polling
 * only the first peer could strand a lagging node when that peer is behind. */
static int catchup_poll_peer_root(struct efsd_server *s)
{
    struct efs_node nodes[EFS_MAX_NODES];
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;
    memcpy(nodes, s->nodes, sizeof(struct efs_node) * node_count);
    efs_node_id_t self = s->id;
    pthread_mutex_unlock(&s->lock);

    int best = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == self)
            continue;
        const char *host = nodes[i].addr;
        uint16_t port = nodes[i].port;
        if (!host[0] || port == 0)
            continue;

        /* Phase 1: root-only poll. A full GET_META here cost every server
         * 3 x ~1 GiB of memcpy + network every 2 s even at rest — the
         * memmove storm pinned idle efsd at 60%+ CPU and starved client IO. */
        int fd = server_peer_conn_get(host, port);
        if (fd < 0)
            continue;
        if (efs_send_msg(fd, EFS_MSG_GET_META_ROOT, NULL, 0) != 0) {
            server_peer_conn_drop(host, port, fd);
            continue;
        }
        uint8_t type;
        void *payload = NULL;
        uint32_t payload_len = 0;
        if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
            type != EFS_MSG_GET_META_ROOT_REPLY || payload_len == 0) {
            free(payload);
            server_peer_conn_drop(host, port, fd);
            continue;
        }

        uint64_t peer_gen = 0;
        if (efs_meta_blob_is_root(payload, payload_len)) {
            struct efs_export_root r;
            memset(&r, 0, sizeof(r));
            if (efs_export_root_deserialize_used(&r, payload, payload_len,
                                                 NULL) == 0)
                peer_gen = r.generation;
            efs_export_root_free(&r);
        }
        free(payload);

        pthread_mutex_lock(&s->lock);
        uint64_t our_gen = 0;
        int stuck = 0;
        if (s->export_count > 0) {
            our_gen = s->exports[0].root.generation;
            stuck = s->exports[0].meta_needs_rebuild;
        }
        pthread_mutex_unlock(&s->lock);

        /* Phase 2: full blob only when the peer is strictly newer, or same
         * generation while our rebuild is stuck (peer may hold live tables
         * that fragment rebuild cannot recover). */
        int want = (peer_gen > our_gen) || (peer_gen > 0 && peer_gen == our_gen && stuck);
        if (!want) {
            server_peer_conn_release(host, port, fd);
            continue;
        }
        if (efs_send_msg(fd, EFS_MSG_GET_META, NULL, 0) != 0) {
            server_peer_conn_drop(host, port, fd);
            continue;
        }
        payload = NULL;
        payload_len = 0;
        if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
            type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
            free(payload);
            server_peer_conn_drop(host, port, fd);
            continue;
        }
        server_peer_conn_release(host, port, fd);

        int rc = catchup_install_newer_root(s, payload, payload_len);
        free(payload);
        if (rc > best)
            best = rc;
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
            /* Only rebuild when explicitly dirty. Do NOT key off inode_count<=1:
             * an empty export stays at 1 inode forever and used to spin-rebuild
             * (hundreds of times) until a raced deserialize SIGSEGV'd. */
            need[e] = (ex->meta_fragmented && ex->root.page_count > 0 &&
                       ex->meta_needs_rebuild);
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
                server_save_export(s, ex);
                pthread_mutex_unlock(&s->lock);
                fprintf(stderr, "meta-catchup: rebuilt export=%s\n", ex->name);
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
    s->rpc_dirty_ops[eidx]++;
    uint64_t total = 0;
    for (uint32_t e = 0; e < s->export_count && e < EFS_MAX_EXPORTS; e++)
        total += s->rpc_dirty_ops[e];
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
        if (server_is_meta_primary_locked(s)) {
            for (uint32_t e = 0; e < s->export_count && e < EFS_MAX_EXPORTS;
                 e++) {
                if (s->rpc_dirty_ops[e] > 0) {
                    if (s->exports[e].meta_needs_rebuild) {
                        /* Fenced by a concurrent client-driven PUT_META: the
                         * in-memory table is stale and the catchup rebuild
                         * will overwrite it, so these RPC mutations are lost
                         * — don't flush a stale table. (Transition-only race;
                         * goes away in 2b when clients stop blob-flushing.) */
                        fprintf(stderr,
                                "meta-flush: export=%s dirty under rebuild; "
                                "dropping %llu RPC op(s) (client-flush race)\n",
                                s->exports[e].name,
                                (unsigned long long)s->rpc_dirty_ops[e]);
                        s->rpc_dirty_ops[e] = 0;
                        continue;
                    }
                    dirty[ndirty++] = e;
                    s->rpc_dirty_ops[e] = 0;
                }
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
                 * window retries instead of losing the in-memory mutations. */
                pthread_mutex_lock(&s->lock);
                if (s->rpc_dirty_ops[dirty[i]] == 0)
                    s->rpc_dirty_ops[dirty[i]] = 1;
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
