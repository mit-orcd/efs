#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/erasure.h"
#include "efs/network.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERIFY_Q 256

struct verify_job {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint32_t data_len;
    uint8_t checksum[EFS_HASH_SIZE];
};

struct verify_pool {
    int running;
    pthread_t tid;
    pthread_t scrub_tid;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    struct verify_job q[VERIFY_Q];
    int head;
    int tail;
    int count;
    uint64_t dropped;
    uint64_t mismatch;
    uint64_t healed;
    uint64_t skipped; /* jobs obsoleted by a newer write to the same fragment */
};

static struct verify_pool g_vfy;
static struct efsd_server *g_vfy_s;

static int peer_get_fragment(const char *host, uint16_t port,
                             efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t chunk_index, uint32_t fragment_index,
                             uint32_t frag_len, uint8_t *data, uint8_t *checksum)
{
    int fd = server_peer_conn_get(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

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
            reply_len == 1 + EFS_HASH_SIZE + frag_len) {
            memcpy(checksum, r + 1, EFS_HASH_SIZE);
            memcpy(data, r + 1 + EFS_HASH_SIZE, frag_len);
            rc = EFS_OK;
        } else if (r[0] == EFS_GET_CHUNK_NOT_FOUND) {
            rc = EFS_ERR_NOT_FOUND;
        } else {
            rc = EFS_ERR_IO;
        }
    }

    free(reply);
    if (rc == EFS_ERR_NET)
        server_peer_conn_drop(host, port, fd);
    else
        server_peer_conn_release(host, port, fd);
    return rc;
}

/* Rebuild this replica from the other two fragments. Does not change the
 * client-visible sidecar checksum. */
static int heal_from_peers(struct efsd_server *s, struct efs_export *ex,
                           const struct verify_job *job)
{
    uint32_t frag_len = job->data_len;
    uint32_t chunk_size = (efs_ino_is_meta_table(job->ino))
                              ? EFS_META_PAGE_SIZE
                              : (frag_len * 2u);
    if (chunk_size == 0)
        return EFS_ERR_IO;

    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count;
    efs_node_id_t local_id = 0;
    pthread_mutex_lock(&s->lock);
    node_count = s->node_count;
    memcpy(nodes, s->nodes, sizeof(nodes));
    struct efs_node *local = server_local_node(s);
    if (local)
        local_id = local->id;
    pthread_mutex_unlock(&s->lock);

    efs_node_id_t placed[EFS_NUM_FRAGMENTS];
    efs_place_fragments(nodes, node_count, job->ino, job->chunk_index, placed);

    int other[2];
    int n_other = 0;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if ((uint32_t)i != job->fragment_index)
            other[n_other++] = i;
    }
    if (n_other != 2)
        return EFS_ERR_IO;

    uint8_t *bufs[EFS_NUM_FRAGMENTS] = {NULL};
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    uint8_t *chunk = NULL;
    int rc = EFS_ERR_NOMEM;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        bufs[i] = malloc(frag_len);
        if (!bufs[i])
            goto out;
        frags[i] = bufs[i];
    }
    chunk = malloc(chunk_size);
    if (!chunk)
        goto out;

    for (int i = 0; i < 2; i++) {
        int oi = other[i];
        struct efs_node *n = NULL;
        for (uint32_t j = 0; j < node_count; j++) {
            if (nodes[j].id == placed[oi]) {
                n = &nodes[j];
                break;
            }
        }
        if (!n) {
            rc = EFS_ERR_NET;
            goto out;
        }
        if (n->id == local_id) {
            uint32_t got = 0;
            rc = server_read_fragment(s, ex, job->ino, job->chunk_index,
                                      (uint32_t)oi, frags[oi], &got);
            if (rc != 0 || got != frag_len)
                goto out;
        } else {
            uint8_t peer_ck[EFS_HASH_SIZE];
            rc = peer_get_fragment(n->addr, n->port, job->export_id, job->ino,
                                   job->chunk_index, (uint32_t)oi, frag_len,
                                   frags[oi], peer_ck);
            if (rc != 0)
                goto out;
        }
    }

    rc = efs_decode_chunk(frags, chunk_size, other[0], other[1],
                          (int)job->fragment_index, chunk, chunk_size);
    if (rc != 0)
        goto out;

    uint8_t *healed = frags[job->fragment_index];
    if (job->fragment_index == 0)
        memcpy(healed, chunk, frag_len);
    else if (job->fragment_index == 1)
        memcpy(healed, chunk + frag_len, frag_len);
    else {
        for (uint32_t i = 0; i < frag_len; i++)
            healed[i] = (uint8_t)(chunk[i] ^ chunk[frag_len + i]);
    }

    uint8_t got[EFS_HASH_SIZE];
    efs_hash(healed, frag_len, got);
    if (memcmp(got, job->checksum, EFS_HASH_SIZE) != 0) {
        rc = EFS_ERR_CHECKSUM;
        goto out;
    }

    /* A newer write may have landed while we fetched peers: re-check the
     * sidecar so a stale heal can't clobber a fresh fragment. */
    uint8_t cur_sum[EFS_HASH_SIZE];
    if (server_read_fragment_sum(s, ex, job->ino, job->chunk_index,
                                 job->fragment_index, cur_sum) == 0 &&
        memcmp(cur_sum, job->checksum, EFS_HASH_SIZE) != 0) {
        rc = EFS_ERR_BUSY;
        goto out;
    }

    rc = server_write_fragment_with_sum(s, ex, job->ino, job->chunk_index,
                                        job->fragment_index, healed, frag_len,
                                        job->checksum);
out:
    free(chunk);
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        free(bufs[i]);
    return rc;
}

static void verify_one(struct efsd_server *s, const struct verify_job *job)
{
    struct efs_export *ex = server_export_acquire(s, job->export_id);
    if (!ex)
        return;

    uint8_t *buf = malloc(job->data_len);
    if (!buf) {
        server_export_put(s, ex);
        return;
    }

    uint32_t got = 0;
    int rc = server_read_fragment(s, ex, job->ino, job->chunk_index,
                                  job->fragment_index, buf, &got);
    if (rc == 0 && got == job->data_len) {
        /* Obsolescence check: a newer write may have rewritten this fragment
         * since the job was enqueued (meta dual-slot pages flip every other
         * generation under concurrent flushers). If the on-disk sidecar sum
         * no longer matches the job's checksum, the job is stale — verifying
         * against the old expectation would mismatch and trigger a pointless
         * heal storm. The newer write's own verify job checks the content. */
        uint8_t cur_sum[EFS_HASH_SIZE];
        if (server_read_fragment_sum(s, ex, job->ino, job->chunk_index,
                                     job->fragment_index, cur_sum) == 0 &&
            memcmp(cur_sum, job->checksum, EFS_HASH_SIZE) != 0) {
            g_vfy.skipped++;
            free(buf);
            server_export_put(s, ex);
            return;
        }
        uint8_t hash[EFS_HASH_SIZE];
        efs_hash(buf, job->data_len, hash);
        if (memcmp(hash, job->checksum, EFS_HASH_SIZE) != 0) {
            g_vfy.mismatch++;
            int hrc = heal_from_peers(s, ex, job);
            if (hrc == 0) {
                g_vfy.healed++;
                fprintf(stderr,
                        "verify-heal: export=%llu ino=%llu ci=%u fi=%u\n",
                        (unsigned long long)job->export_id,
                        (unsigned long long)job->ino, job->chunk_index,
                        job->fragment_index);
            } else {
                fprintf(stderr,
                        "verify-heal: failed export=%llu ino=%llu ci=%u fi=%u "
                        "rc=%d\n",
                        (unsigned long long)job->export_id,
                        (unsigned long long)job->ino, job->chunk_index,
                        job->fragment_index, hrc);
            }
        }
    }
    free(buf);
    server_export_put(s, ex);
}

static void *verify_thread(void *arg)
{
    struct efsd_server *s = arg;
    for (;;) {
        pthread_mutex_lock(&g_vfy.lock);
        while (g_vfy.running && g_vfy.count == 0)
            pthread_cond_wait(&g_vfy.cv, &g_vfy.lock);
        if (!g_vfy.running && g_vfy.count == 0) {
            pthread_mutex_unlock(&g_vfy.lock);
            break;
        }
        struct verify_job job = g_vfy.q[g_vfy.head];
        g_vfy.head = (g_vfy.head + 1) % VERIFY_Q;
        g_vfy.count--;
        pthread_cond_signal(&g_vfy.cv);
        pthread_mutex_unlock(&g_vfy.lock);
        verify_one(s, &job);
    }
    return NULL;
}

/* Background scrubber: with client read-verify off by default, disk rot on
 * cold data is found only here. Walks the live chunk table in small batches
 * and enqueues a verify job for every fragment this node owns; verify_one
 * re-hashes and heals from peers on mismatch. Best-effort: the index cursor
 * is approximate across table rebuilds (rows may shift), so individual
 * chunks can be skipped or repeated within a pass — the next pass covers
 * them. At SCRUB_BATCH per SCRUB_SLEEP_MS a multi-million-chunk table takes
 * hours per sweep, which is the intended disk-rot cadence. */
#define SCRUB_BATCH 128
#define SCRUB_SLEEP_MS 1000

struct scrub_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint32_t fi;
    uint32_t len;
    uint8_t ck[EFS_HASH_SIZE];
};

static void *scrub_thread(void *arg)
{
    struct efsd_server *s = arg;
    uint64_t cursor = 0;
    static struct scrub_ent batch[SCRUB_BATCH];

    for (;;) {
        pthread_mutex_lock(&g_vfy.lock);
        int running = g_vfy.running;
        pthread_mutex_unlock(&g_vfy.lock);
        if (!running)
            break;

        uint32_t bn = 0;
        efs_export_id_t eid = 0;
        pthread_mutex_lock(&s->lock);
        if (s->export_count > 0) {
            struct efs_export *ex = &s->exports[0];
            if (ex->meta_fragmented && !ex->meta_needs_rebuild &&
                ex->chunk_count > 0) {
                eid = ex->id;
                uint64_t n = ex->chunk_count;
                if (cursor >= n)
                    cursor = 0;
                uint8_t zck[EFS_HASH_SIZE];
                uint32_t zck_len = 0;
                uint64_t i = cursor;
                for (; i < n && bn < SCRUB_BATCH; i++) {
                    const struct efs_chunk_entry *ce = &ex->chunks[i];
                    if (efs_ino_is_meta_table(ce->ino))
                        continue;
                    uint32_t fi = EFS_NUM_FRAGMENTS;
                    for (uint32_t k = 0; k < EFS_NUM_FRAGMENTS; k++)
                        if (ce->fragment_nodes[k] == s->id) {
                            fi = k;
                            break;
                        }
                    if (fi == EFS_NUM_FRAGMENTS)
                        continue;
                    uint32_t fl = server_frag_len(ex, ce->ino);
                    if (fl == 0)
                        continue;
                    if (zck_len != fl) {
                        efs_hash_zero_fragment_len(fl, zck);
                        zck_len = fl;
                    }
                    if (memcmp(ce->checksums[fi], zck, EFS_HASH_SIZE) == 0)
                        continue; /* sparse zero chunk: nothing on disk */
                    batch[bn].ino = ce->ino;
                    batch[bn].ci = ce->chunk_index;
                    batch[bn].fi = fi;
                    batch[bn].len = fl;
                    memcpy(batch[bn].ck, ce->checksums[fi], EFS_HASH_SIZE);
                    bn++;
                }
                cursor = i;
            }
        }
        pthread_mutex_unlock(&s->lock);

        for (uint32_t j = 0; j < bn; j++)
            server_verify_enqueue(s, eid, batch[j].ino, batch[j].ci,
                                  batch[j].fi, batch[j].len, batch[j].ck);

        /* Stoppable sleep: shutdown must not wait a full interval. */
        for (int w = 0; w < SCRUB_SLEEP_MS / 50; w++) {
            usleep(50000);
            pthread_mutex_lock(&g_vfy.lock);
            running = g_vfy.running;
            pthread_mutex_unlock(&g_vfy.lock);
            if (!running)
                break;
        }
    }
    return NULL;
}

int server_verify_start(struct efsd_server *s)
{
    memset(&g_vfy, 0, sizeof(g_vfy));
    pthread_mutex_init(&g_vfy.lock, NULL);
    pthread_cond_init(&g_vfy.cv, NULL);
    g_vfy.running = 1;
    g_vfy_s = s;
    if (efsd_pthread_create(&g_vfy.tid, verify_thread, s) != 0) {
        g_vfy.running = 0;
        return -1;
    }
    if (efsd_pthread_create(&g_vfy.scrub_tid, scrub_thread, s) != 0) {
        /* Scrubber is best-effort: the write/read verify pool still works. */
        g_vfy.scrub_tid = 0;
    }
    return 0;
}

void server_verify_stop(struct efsd_server *s)
{
    (void)s;
    pthread_mutex_lock(&g_vfy.lock);
    g_vfy.running = 0;
    pthread_cond_broadcast(&g_vfy.cv);
    pthread_mutex_unlock(&g_vfy.lock);
    pthread_join(g_vfy.tid, NULL);
    if (g_vfy.scrub_tid)
        pthread_join(g_vfy.scrub_tid, NULL);
    pthread_mutex_destroy(&g_vfy.lock);
    pthread_cond_destroy(&g_vfy.cv);
    g_vfy_s = NULL;
}

void server_verify_enqueue(struct efsd_server *s, efs_export_id_t export_id,
                           efs_ino_t ino, uint32_t chunk_index,
                           uint32_t fragment_index, uint32_t data_len,
                           const uint8_t checksum[EFS_HASH_SIZE])
{
    (void)s;
    if (!g_vfy.running || data_len == 0)
        return;
    struct verify_job job;
    job.export_id = export_id;
    job.ino = ino;
    job.chunk_index = chunk_index;
    job.fragment_index = fragment_index;
    job.data_len = data_len;
    memcpy(job.checksum, checksum, EFS_HASH_SIZE);

    pthread_mutex_lock(&g_vfy.lock);
    if (g_vfy.count >= VERIFY_Q) {
        g_vfy.dropped++;
        pthread_mutex_unlock(&g_vfy.lock);
        return;
    }
    g_vfy.q[g_vfy.tail] = job;
    g_vfy.tail = (g_vfy.tail + 1) % VERIFY_Q;
    g_vfy.count++;
    pthread_cond_signal(&g_vfy.cv);
    pthread_mutex_unlock(&g_vfy.lock);
}
