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

static int server_get_fragment_from_peer(const char *host, uint16_t port,
                                         efs_export_id_t export_id, efs_ino_t ino,
                                         uint32_t chunk_index, uint32_t fragment_index,
                                         uint8_t *data, uint8_t *checksum)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

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
            reply_len == 1 + EFS_HASH_SIZE + EFS_FRAGMENT_SIZE) {
            memcpy(checksum, r + 1, EFS_HASH_SIZE);
            memcpy(data, r + 1 + EFS_HASH_SIZE, EFS_FRAGMENT_SIZE);
            rc = EFS_OK;
        } else if (r[0] == EFS_GET_CHUNK_NOT_FOUND) {
            rc = EFS_ERR_NOT_FOUND;
        } else {
            rc = EFS_ERR_IO;
        }
    }

    free(reply);
    close(fd);
    return rc;
}

static int server_put_fragment_to_peer(const char *host, uint16_t port,
                                       efs_export_id_t export_id, efs_ino_t ino,
                                       uint32_t chunk_index, uint32_t fragment_index,
                                       const uint8_t *data, const uint8_t *checksum)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_put_chunk req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.chunk_index = chunk_index;
    req.fragment_index = fragment_index;
    memcpy(req.checksum, checksum, EFS_HASH_SIZE);
    memcpy(req.data, data, EFS_FRAGMENT_SIZE);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = EFS_ERR_NET;

    if (efs_send_msg(fd, EFS_MSG_PUT_CHUNK, &req, sizeof(req)) == 0 &&
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

    free(reply);
    close(fd);
    return rc;
}

static int fetch_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                               efs_node_id_t node_id, uint32_t page_index,
                               uint32_t fragment_index, uint8_t *data)
{
    uint8_t checksum[EFS_HASH_SIZE];
    if (node_id == s->id) {
        uint32_t len = 0;
        if (server_read_fragment(s, ex, EFS_META_TABLE_INO, page_index,
                                 fragment_index, data, &len) != EFS_OK)
            return EFS_ERR_NOT_FOUND;
        return EFS_OK;
    }
    struct efs_node n;
    if (copy_node_by_id(s, node_id, &n) != 0)
        return EFS_ERR_NOT_FOUND;
    return server_get_fragment_from_peer(n.addr, n.port, ex->id, EFS_META_TABLE_INO,
                                         page_index, fragment_index, data, checksum);
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
    const struct efs_export_root *root = &ex->root;
    if (root->page_count == 0 || root->blob_len == 0)
        return EFS_ERR_PROTO;
    if (root->page_count > EFS_META_MAX_PAGES)
        return EFS_ERR_INVAL;
    if (!root->page_checksums)
        return EFS_ERR_INVAL;

    uint8_t (*pages)[EFS_CHUNK_SIZE] = calloc(root->page_count, EFS_CHUNK_SIZE);
    uint8_t (*fragments)[EFS_FRAGMENT_SIZE] =
        malloc(EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE);
    if (!pages || !fragments) {
        free(pages);
        free(fragments);
        return EFS_ERR_NOMEM;
    }

    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        efs_node_id_t placed[EFS_NUM_FRAGMENTS];
        pthread_mutex_lock(&s->lock);
        efs_place_fragments(s->nodes, s->node_count, EFS_META_TABLE_INO, pi,
                            placed);
        pthread_mutex_unlock(&s->lock);

        int have[EFS_NUM_FRAGMENTS] = {0};

        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            int frc = fetch_meta_fragment(s, ex, placed[fi], pi, (uint32_t)fi,
                                          fragments[fi]);
            if (frc == EFS_OK) {
                uint8_t sum[EFS_HASH_SIZE];
                efs_hash(fragments[fi], EFS_FRAGMENT_SIZE, sum);
                if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                           EFS_HASH_SIZE) == 0)
                    have[fi] = 1;
                else
                    fprintf(stderr,
                            "meta-rebuild: page %u frag %u checksum mismatch "
                            "(node %u)\n",
                            pi, fi, placed[fi]);
            } else {
                fprintf(stderr,
                        "meta-rebuild: page %u frag %u fetch failed rc=%d "
                        "(node %u export=%u)\n",
                        pi, fi, frc, placed[fi], ex->id);
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
        if (a < 0 || b < 0 || missing < 0 ||
            efs_decode_chunk(fragments, a, b, missing, pages[pi], EFS_CHUNK_SIZE) != 0) {
            fprintf(stderr,
                    "meta-rebuild: decode failed page %u have=%d%d%d "
                    "nodes=%u,%u,%u node_count=%u\n",
                    pi, have[0], have[1], have[2], placed[0], placed[1],
                    placed[2], s->node_count);
            free(fragments);
            free(pages);
            return EFS_ERR_DECODE;
        }
    }
    free(fragments);

    char *blob = NULL;
    size_t blob_len = 0;
    int rc = efs_meta_assemble_blob(root, pages, &blob, &blob_len);
    free(pages);
    if (rc != EFS_OK)
        return rc;

    struct efs_export_root saved_root;
    memset(&saved_root, 0, sizeof(saved_root));
    if (efs_export_root_copy(&saved_root, root) != EFS_OK) {
        free(blob);
        return EFS_ERR_NOMEM;
    }
    rc = efs_export_deserialize(ex, blob, blob_len);
    free(blob);
    if (rc != EFS_OK) {
        efs_export_root_free(&saved_root);
        return rc;
    }

    ex->meta_fragmented = 1;
    ex->meta_needs_rebuild = 0;
    efs_export_root_move(&ex->root, &saved_root);
    ex->next_ino = ex->root.next_ino;
    return EFS_OK;
}

static int put_meta_fragment(struct efsd_server *s, struct efs_export *ex,
                             efs_node_id_t node_id, uint32_t page_index,
                             uint32_t fragment_index, const uint8_t *data,
                             const uint8_t *checksum)
{
    if (node_id == s->id) {
        /* Use sync writers to avoid pool re-entrancy during meta flush. */
        int rc = server_write_fragment_sync(s, ex, EFS_META_TABLE_INO, page_index,
                                            fragment_index, data, EFS_FRAGMENT_SIZE);
        if (rc != EFS_OK)
            return rc;
        return server_write_fragment_sum_sync(s, ex, EFS_META_TABLE_INO, page_index,
                                              fragment_index, checksum);
    }
    struct efs_node n;
    if (copy_node_by_id(s, node_id, &n) != 0)
        return EFS_ERR_NOT_FOUND;
    return server_put_fragment_to_peer(n.addr, n.port, ex->id, EFS_META_TABLE_INO,
                                       page_index, fragment_index, data, checksum);
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
        int fd = efs_connect_tcp(nodes[i].addr, nodes[i].port);
        if (fd < 0)
            continue;
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        efs_send_msg(fd, EFS_MSG_PUT_META, buf, (uint32_t)len);
        efs_recv_msg(fd, &type, &reply, &reply_len);
        free(reply);
        close(fd);
    }
    free(nodes);
    free(buf);
}

/* Write all meta pages (2+1) and persist/replicate the EFSR root. */
int server_flush_fragmented_meta(struct efsd_server *s, struct efs_export *ex)
{
    /* Peers must know the export before accepting meta-page PUT_CHUNKs. */
    bootstrap_export_on_peers(s, ex);

    char *blob = NULL;
    size_t blob_len = 0;
    if (efs_export_serialize(ex, &blob, &blob_len) != EFS_OK)
        return -1;
    if (blob_len > (size_t)EFS_META_MAX_PAGES * EFS_CHUNK_SIZE) {
        free(blob);
        return -1;
    }

    uint64_t new_gen = ex->root.generation + 1;
    if (new_gen == 0)
        new_gen = 1;

    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    if (efs_export_root_prepare(&root, ex, new_gen, (uint32_t)blob_len) != EFS_OK) {
        free(blob);
        return -1;
    }

    uint8_t *page = malloc(EFS_CHUNK_SIZE);
    uint8_t (*fragments)[EFS_FRAGMENT_SIZE] =
        malloc(EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE);
    if (!page || !fragments) {
        free(page);
        free(fragments);
        free(blob);
        efs_export_root_free(&root);
        return -1;
    }
    for (uint32_t pi = 0; pi < root.page_count; pi++) {
        if (efs_meta_extract_page(blob, (uint32_t)blob_len, pi, page) != EFS_OK) {
            free(page);
            free(fragments);
            free(blob);
            efs_export_root_free(&root);
            return -1;
        }
        efs_encode_chunk(page, EFS_CHUNK_SIZE, fragments);
        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            efs_hash(fragments[fi], EFS_FRAGMENT_SIZE, checksums[fi]);

        efs_node_id_t placed[EFS_NUM_FRAGMENTS];
        pthread_mutex_lock(&s->lock);
        efs_place_fragments(s->nodes, s->node_count, EFS_META_TABLE_INO, pi,
                            placed);
        pthread_mutex_unlock(&s->lock);

        int acks = 0;
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            int prc = put_meta_fragment(s, ex, placed[fi], pi, (uint32_t)fi,
                                        fragments[fi], checksums[fi]);
            if (prc != EFS_OK) {
                /* Peer may not have finished bootstrap yet — retry once. */
                usleep(50000);
                prc = put_meta_fragment(s, ex, placed[fi], pi, (uint32_t)fi,
                                        fragments[fi], checksums[fi]);
            }
            if (prc == EFS_OK) {
                acks++;
            } else {
                fprintf(stderr,
                        "meta-flush: page %u frag %u put failed rc=%d "
                        "(node %u export=%u)\n",
                        pi, fi, prc, placed[fi], ex->id);
            }
        }
        if (acks < 2) {
            free(page);
            free(fragments);
            free(blob);
            efs_export_root_free(&root);
            return -1;
        }
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            memcpy(efs_export_root_checksum(&root, pi, fi), checksums[fi],
                   EFS_HASH_SIZE);
    }
    free(page);
    free(fragments);
    free(blob);

    pthread_mutex_lock(&s->lock);
    ex->meta_fragmented = 1;
    efs_export_root_move(&ex->root, &root);
    s->export_meta_dirty = 1;
    /* Persist under lock so a concurrent peer PUT_META cannot free
     * page_checksums mid-fwrite. */
    server_save_export(s, ex);
    s->export_meta_dirty = 0;

    /* Snapshot root for unlocked peer fan-out. */
    struct efs_export_root snap;
    memset(&snap, 0, sizeof(snap));
    int src = efs_export_root_copy(&snap, &ex->root);
    pthread_mutex_unlock(&s->lock);
    if (src != EFS_OK)
        return -1;

    /* Push root to peers. */
    char *root_buf = NULL;
    size_t root_len = 0;
    if (efs_export_root_serialize(&snap, &root_buf, &root_len) != EFS_OK) {
        efs_export_root_free(&snap);
        return -1;
    }

    int acks = 0;
    struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
    if (!nodes) {
        free(root_buf);
        efs_export_root_free(&snap);
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
        int fd = efs_connect_tcp(nodes[i].addr, nodes[i].port);
        if (fd < 0)
            continue;
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_send_msg(fd, EFS_MSG_PUT_META, root_buf, (uint32_t)root_len) == 0 &&
            efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
            type == EFS_MSG_PUT_META_REPLY && reply_len >= 1 &&
            ((uint8_t *)reply)[0] == EFS_PUT_META_OK) {
            acks++;
        }
        free(reply);
        close(fd);
    }
    free(nodes);
    free(root_buf);
    efs_export_id_t eid = snap.id;
    efs_export_root_free(&snap);
    /* Single-node clusters have no peers; otherwise require at least one
     * peer root ack (pages already needed quorum above). */
    if (node_count <= 1)
        return 0;
    if (acks < 1) {
        fprintf(stderr,
                "meta-flush: export %u root replicate got 0/%u peer acks\n",
                eid, node_count - 1);
        return -1;
    }
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

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    rc = efs_send_msg(fd, EFS_MSG_PUT_META, buf, (uint32_t)len);
    free(buf);
    if (rc != 0) {
        close(fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    rc = efs_recv_msg(fd, &type, &payload, &payload_len);
    free(payload);
    close(fd);
    return (rc == 0 && type == EFS_MSG_PUT_META_REPLY) ? 0 : -1;
}

int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex)
{
    /* Prefer fragmented persist so peers store EFSR + pages, not N full copies. */
    return server_flush_fragmented_meta(s, ex);
}

int server_fetch_metadata_from(struct efsd_server *s, const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    if (efs_send_msg(fd, EFS_MSG_GET_META, NULL, 0) != 0) {
        close(fd);
        return -1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
        free(payload);
        close(fd);
        return -1;
    }
    close(fd);

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
