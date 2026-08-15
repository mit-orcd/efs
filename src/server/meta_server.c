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
                                 fragment_index, data, &len) != EFS_OK)
            return EFS_ERR_NOT_FOUND;
        return EFS_OK;
    }
    struct efs_node n;
    if (copy_node_by_id(s, node_id, &n) != 0)
        return EFS_ERR_NOT_FOUND;
    return server_get_fragment_from_peer(n.addr, n.port, ex->id, EFS_META_TABLE_INO,
                                         chunk_index, fragment_index, data, checksum);
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

static int put_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                             efs_node_id_t node_id, uint32_t chunk_index,
                             uint32_t fragment_index, const uint8_t *data,
                             const uint8_t *checksum);

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
         * A save used to rewrite v4 roots as v5 without moving pages. */
        uint32_t try_ci[3];
        int ntry = efs_meta_page_ci_candidates(start_gen, root->version,
                                               root->ino_page_count,
                                               root->chunk_page_count, pi,
                                               try_ci);

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

                /* Best-effort: rewrite missing/corrupt frags this node owns. */
                int need_heal = 0;
                for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
                    if (placed[fi] == self && !have[fi]) {
                        need_heal = 1;
                        break;
                    }
                }
                if (need_heal &&
                    efs_encode_chunk(pages[pi], EFS_META_PAGE_SIZE,
                                     EFS_META_PAGE_SIZE, fragments) == 0) {
                    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
                        if (placed[fi] != self || have[fi])
                            continue;
                        const uint8_t *csum =
                            efs_export_root_checksum_const(root, pi, fi);
                        int hrc = put_meta_fragment(s, ex, self, ci, (uint32_t)fi,
                                                    fragments[fi], csum);
                        if (hrc == EFS_OK)
                            fprintf(stderr,
                                    "meta-heal: export=%s page=%u fi=%u ci=%u\n",
                                    ex->name, pi, fi, ci);
                        else
                            fprintf(stderr,
                                    "meta-heal: failed export=%s page=%u fi=%u "
                                    "ci=%u rc=%d\n",
                                    ex->name, pi, fi, ci, hrc);
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
            free(frag_buf);
            free(pages);
            efs_export_root_free(&snap);
            return EFS_ERR_DECODE;
        }
    }
    free(frag_buf);

    char *blob = NULL;
    size_t blob_len = 0;
    int rc = efs_meta_assemble_blob(root, pages, &blob, &blob_len);
    free(pages);
    if (rc != EFS_OK) {
        efs_export_root_free(&snap);
        return rc;
    }

    /* Deserialize mutates ex (frees tables/root). Hold the server lock so a
     * concurrent PUT_META/handler cannot observe a half-freed export. */
    pthread_mutex_lock(&s->lock);
    if (ex->root.generation != start_gen) {
        pthread_mutex_unlock(&s->lock);
        free(blob);
        efs_export_root_free(&snap);
        return EFS_ERR_PROTO;
    }

    rc = efs_export_deserialize(ex, blob, blob_len);
    if (rc != EFS_OK) {
        free(blob);
        /* Root was cleared by a failed/partial deserialize path; restore snap
         * so the node keeps a usable EFSR and can retry. */
        efs_export_root_move(&ex->root, &snap);
        ex->meta_fragmented = 1;
        ex->meta_needs_rebuild = 1;
        pthread_mutex_unlock(&s->lock);
        return rc;
    }

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

/* Write all meta pages (2+1) and persist/replicate the EFSR root. */
int server_flush_fragmented_meta(struct efsd_server *s, struct efs_export *ex)
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
    pthread_mutex_lock(&s->lock);
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

        uint32_t ci = efs_meta_region_page_chunk_index(new_gen, region, pi);
        if (ci == UINT32_MAX) {
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return -1;
        }

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
    efs_export_root_move(&ex->root, &root);
    s->export_meta_dirty = 1;
    server_save_export(s, ex);
    s->export_meta_dirty = 0;
    pthread_mutex_unlock(&s->lock);

    /* Retire the previous generation's dual-slot pages (best-effort). Only
     * pages beyond the new generation's page count are dead; in-range
     * fragments stay for dirty-page skip reuse by future same-parity gens. */
    if ((old_ino_pc > 0 || old_ch_pc > 0) && old_gen != new_gen)
        server_gc_meta_slot_pages(s, ex, old_gen, old_ino_pc, old_ch_pc,
                                  new_ino_pc, new_ch_pc);
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
            ex->meta_needs_rebuild = (ex->root.page_count > 0);
            pthread_mutex_unlock(&s->lock);
            rc = server_rebuild_export_from_pages(s, ex);
            pthread_mutex_lock(&s->lock);
            if (rc == EFS_OK)
                server_save_export(s, ex);
            rc = (rc == EFS_OK) ? 0 : -1;
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
    if (!efs_meta_blob_is_root(payload, payload_len))
        return 0;

    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    if (efs_export_root_deserialize(&root, payload, payload_len) != 0)
        return -1;

    pthread_mutex_lock(&s->lock);
    if (s->export_count == 0) {
        efs_export_init(&s->exports[0], root.id, root.name);
        s->export_count = 1;
    }
    struct efs_export *ex = &s->exports[0];
    /* Skip stale or identical generations once we already hold an EFSR root. */
    if (ex->meta_fragmented && root.generation <= ex->root.generation) {
        pthread_mutex_unlock(&s->lock);
        efs_export_root_free(&root);
        return 0;
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
    if (ex->root.page_count > 0)
        ex->meta_needs_rebuild = 1;
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

        int fd = server_peer_conn_get(host, port);
        if (fd < 0)
            continue;
        if (efs_send_msg(fd, EFS_MSG_GET_META, NULL, 0) != 0) {
            server_peer_conn_drop(host, port, fd);
            continue;
        }
        uint8_t type;
        void *payload = NULL;
        uint32_t payload_len = 0;
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

        for (uint32_t e = 0; e < ec; e++) {
            if (!need[e] || !s->running)
                continue;
            struct efs_export *ex = &s->exports[e];
            int rc = server_rebuild_export_from_pages(s, ex);
            if (rc == EFS_OK) {
                pthread_mutex_lock(&s->lock);
                server_save_export(s, ex);
                pthread_mutex_unlock(&s->lock);
                fprintf(stderr, "meta-catchup: rebuilt export=%s\n", ex->name);
                did_work = 1;
            } else if (rc == EFS_ERR_PROTO) {
                /* Gen raced with PUT_META; retry promptly. */
                did_work = 1;
            } else {
                fprintf(stderr, "meta-catchup: rebuild export=%s rc=%d\n",
                        ex->name, rc);
                /* Decode/IO failure: back off; leave meta_needs_rebuild set. */
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

        if (!any_dirty && node_count > 1 && s->running) {
            int prc = catchup_poll_peer_root(s);
            if (prc > 0)
                did_work = 1;
        }

        if (!s->running)
            break;
        /* Debounce rebuild storms: continuous PUT_META under load used to
         * rebuild hundreds of times/min. Peer TCP is pooled, but pacing still
         * keeps catch-up from thrashing. Hot-path clients are unaffected. */
        if (did_work)
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
