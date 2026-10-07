#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>

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


static int overlay_chunk_deltas_ce(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                                   uint32_t chunk_len,
                                   const struct efs_chunk_entry *cep);

struct frag_get_job {
    efs_node_id_t node;
    efs_ino_t ino;
    uint32_t chunk_index;
    int fi;
    uint32_t frag_len;
    uint8_t *out;
    uint32_t len;
    int rc;
    uint64_t chunk_generation;
};

static void get_two_parallel(struct frag_get_job *jobs);

static void *frag_get_thread(void *arg)
{
    struct frag_get_job *j = arg;
    uint8_t sum[EFS_HASH_SIZE];
    j->rc = efs_client_get_fragment(j->node, j->ino, j->chunk_index, j->fi,
                                    j->frag_len, j->out, &j->len, sum,
                                    j->chunk_generation);
    return NULL;
}

/* The two preferred fragment GETs run on the chunk worker
 * (get_two_parallel): one send of both, one poll. A 128-thread frag
 * pool was a condvar hop per fragment (Oct 1, ~400 futex per MiB). */

/* Reuse the fragment scratch across decodes on the same worker thread.
 * The buffer is heap + thread-local, so without a destructor it leaks
 * (~3 fragments per decoding thread) when the thread exits. A pthread
 * key frees it at thread exit for pool workers and libfuse threads
 * alike; pool workers only exit via efs_client_read_pools_stop(). */
#include "reply_buffers.h"
static uint8_t *decode_frag_scratch(uint32_t need)
{
    return (uint8_t *)reply_buffer_get(0, need);
}

/* Issue two fragment GETs and wait for both on this thread. */
static int get_one_reply(struct efs_conn *conn, struct frag_get_job *j)
{
    uint8_t reply_type = 0;
    uint8_t status = 0;
    uint8_t sum[EFS_HASH_SIZE];

    if (efs_conn_recv_msg_into(conn, &reply_type, &status, sum, EFS_HASH_SIZE,
                               j->out, j->frag_len) != 0 ||
        reply_type != EFS_MSG_GET_CHUNK_REPLY) {
        j->rc = EFS_ERR_NET;
        return -1;
    }
    if (status != EFS_GET_CHUNK_OK) {
        j->rc = EFS_ERR_NOT_FOUND;
        return 0;
    }
    j->len = j->frag_len;
    j->rc = EFS_OK;
    return 0;
}

static void get_two_parallel(struct frag_get_job *jobs)
{
    struct efs_conn *conns[2] = {NULL, NULL};
    int pending[2] = {0, 0};
    int i;

    for (i = 0; i < 2; i++) {
        struct efs_msg_get_chunk req;
        jobs[i].rc = EFS_ERR_NET;
        jobs[i].len = 0;
        if (jobs[i].node == 0 || efs_client_node_is_down(jobs[i].node))
            continue;
        conns[i] = efs_client_conn_get(jobs[i].node);
        if (!conns[i])
            continue;
        memset(&req, 0, sizeof(req));
        req.export_id = g_client.export_id;
        req.ino = jobs[i].ino;
        req.chunk_index = jobs[i].chunk_index;
        req.fragment_index = (uint32_t)jobs[i].fi;
        if (jobs[i].chunk_generation &&
            jobs[i].chunk_generation != EFS_CHUNK_BASE_UNCOND)
            req.chunk_generation = jobs[i].chunk_generation;
        if (efs_conn_send_msg(conns[i], EFS_MSG_GET_CHUNK, &req,
                              sizeof(req)) != 0) {
            efs_client_conn_drop(jobs[i].node, conns[i]);
            efs_client_node_note_fail(jobs[i].node);
            conns[i] = NULL;
            continue;
        }
        pending[i] = 1;
    }

    {
        struct timespec ts;
        int64_t deadline;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000 +
                   5000;
        while (pending[0] || pending[1]) {
            struct pollfd pfds[2];
            int map[2];
            int np = 0;
            int64_t now, left;
            int pr;

            for (i = 0; i < 2; i++) {
                int w;
                if (!pending[i] || !conns[i])
                    continue;
                w = efs_conn_reply_watch_quick(conns[i]);
                if (w == EFS_CONN_REPLY_READY) {
                    if (get_one_reply(conns[i], &jobs[i]) != 0) {
                        efs_client_conn_drop(jobs[i].node, conns[i]);
                        efs_client_node_note_fail(jobs[i].node);
                    } else {
                        efs_client_conn_release(jobs[i].node, conns[i]);
                        if (jobs[i].rc == EFS_OK)
                            efs_client_node_note_ok(jobs[i].node);
                    }
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                }
                if (w < 0) {
                    efs_client_conn_drop(jobs[i].node, conns[i]);
                    efs_client_node_note_fail(jobs[i].node);
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                }
                pfds[np].fd = w;
                pfds[np].events = POLLIN;
                pfds[np].revents = 0;
                map[np] = i;
                np++;
            }
            if (!pending[0] && !pending[1])
                break;
            if (np == 0)
                break;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
            left = deadline - now;
            if (left <= 0) {
                for (i = 0; i < 2; i++) {
                    if (!pending[i] || !conns[i])
                        continue;
                    efs_client_conn_drop(jobs[i].node, conns[i]);
                    efs_client_node_note_fail(jobs[i].node);
                    conns[i] = NULL;
                    pending[i] = 0;
                }
                break;
            }
            pr = poll(pfds, (nfds_t)np, left > 2000 ? 2000 : (int)left);
            if (pr <= 0)
                continue;
            for (i = 0; i < np; i++) {
                int k;
                if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP)))
                    continue;
                k = map[i];
                if (!pending[k] || !conns[k])
                    continue;
                if (get_one_reply(conns[k], &jobs[k]) != 0) {
                    efs_client_conn_drop(jobs[k].node, conns[k]);
                    efs_client_node_note_fail(jobs[k].node);
                } else {
                    efs_client_conn_release(jobs[k].node, conns[k]);
                    if (jobs[k].rc == EFS_OK)
                        efs_client_node_note_ok(jobs[k].node);
                }
                conns[k] = NULL;
                pending[k] = 0;
            }
        }
    }
}

/* Reconstruct one logical chunk from any 2 of 3 fragments. The three GETs run
 * concurrently so one slow/dead peer overlaps the others instead of adding a
 * full RTT; decode only needs any two, so we don't serialize on a straggler. */
static int efs_client_decode_placed_chunk_attempts(efs_ino_t ino, uint32_t chunk_index,
                                                   uint8_t *chunk_out, uint32_t chunk_size,
                                                   uint32_t frag_len, int max_attempts,
                                                   int treat_zero_cksum_as_hole,
                                                   const struct efs_chunk_entry *cep)
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t *frag_buf;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    struct efs_chunk_entry ce;
    int have_ce;

    efs_place_fragments(g_client.nodes, g_client.node_count, ino, chunk_index,
                        nodes);

    if (max_attempts < 1)
        max_attempts = 1;

    /* Heap / TLS: three fragments on a FUSE/main stack overflows easily and
     * SIGSEGV handlers without an alt stack cannot even log. */
    frag_buf = decode_frag_scratch(EFS_NUM_FRAGMENTS * frag_len);
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    frag_ptrs(frag_buf, frag_len, frags);
    /* The two data fragments are received straight into the chunk's
     * halves; only parity needs scratch. efs_decode_chunk skips the
     * copy of a fragment that is already in place (Oct 1 read review:
     * the assemble memcpy was 21 % of client cycles on a cold read). */
    if ((size_t)frag_len * 2 == chunk_size) {
        frags[0] = chunk_out;
        frags[1] = chunk_out + frag_len;
    }

    memset(&ce, 0, sizeof(ce));
    if (cep) {
        /* The caller's row: the same one its span overlay and its
         * publish observation use (fetch_published_once). */
        ce = *cep;
        have_ce = 1;
    } else {
        pthread_mutex_lock(&g_client.idx_mu);
        have_ce = (efs_export_get_chunk(&g_client.export, ino, chunk_index,
                                        &ce) == 0);
        pthread_mutex_unlock(&g_client.idx_mu);
    }
    if (have_ce) {
        uint8_t zck[EFS_HASH_SIZE];
        efs_hash_zero_fragment_len(frag_len, zck);
        if (treat_zero_cksum_as_hole &&
            memcmp(ce.checksums[0], zck, EFS_HASH_SIZE) == 0 &&
            memcmp(ce.checksums[1], zck, EFS_HASH_SIZE) == 0 &&
            memcmp(ce.checksums[2], zck, EFS_HASH_SIZE) == 0) {
            memset(chunk_out, 0, chunk_size);
            return EFS_OK;
        }
        /* Writes may have steered a fragment onto a spare when a stripe
         * member was down. The chunk table is the source of truth. */
        if (ce.fragment_nodes[0] || ce.fragment_nodes[1] ||
            ce.fragment_nodes[2])
            memcpy(nodes, ce.fragment_nodes, sizeof(nodes));
    }

    int order[EFS_NUM_FRAGMENTS] = {0, 1, 2};
    if (g_client.local_node_id != 0) {
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (nodes[i] == g_client.local_node_id) {
                int tmp = order[0];
                order[0] = order[i];
                order[i] = tmp;
                break;
            }
        }
    }

    int rc = EFS_ERR_DECODE;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int have[EFS_NUM_FRAGMENTS] = {0, 0, 0};
        int good = 0;

        /* Fetch the two preferred fragments in parallel; pull the third
         * (parity) only if one of the first two fails. 2+1 reconstruction
         * needs any two fragments, and the all-up common case is served by the
         * two data fragments — saving a third GET, a third verify-hash, and a
         * third thread per chunk. A down node is skipped inside
         * efs_client_get_fragment (down-mark) and fails fast into the parity
         * fallback. */
        struct frag_get_job jobs[EFS_NUM_FRAGMENTS];

        /* Stage 1: the two preferred fragments in parallel. */
        for (int i = 0; i < 2; i++) {
            int fi = order[i];
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].node = nodes[fi];
            jobs[i].ino = ino;
            jobs[i].chunk_index = chunk_index;
            jobs[i].fi = fi;
            jobs[i].frag_len = frag_len;
            jobs[i].out = frags[fi];
            jobs[i].len = 0;
            jobs[i].rc = EFS_ERR_NET;
            jobs[i].chunk_generation = have_ce ? ce.generation : 0;
        }
        /* Both GETs from this worker: send, one poll, recv. The frag
         * pool's condvar hop was ~16 futex per chunk (Oct 1). */
        get_two_parallel(jobs);
        for (int i = 0; i < 2; i++) {
            if (jobs[i].rc == 0 && jobs[i].len == frag_len && !have[jobs[i].fi]) {
                have[jobs[i].fi] = 1;
                good++;
            }
        }

        /* Stage 2: parity fallback only when a preferred fragment failed. */
        if (good < 2) {
            int fi = order[2];
            jobs[2].node = nodes[fi];
            jobs[2].ino = ino;
            jobs[2].chunk_index = chunk_index;
            jobs[2].fi = fi;
            jobs[2].frag_len = frag_len;
            jobs[2].out = frags[fi];
            jobs[2].len = 0;
            jobs[2].rc = EFS_ERR_NET;
            jobs[2].chunk_generation = have_ce ? ce.generation : 0;
            frag_get_thread(&jobs[2]); /* single fetch: run inline */
            if (jobs[2].rc == 0 && jobs[2].len == frag_len && !have[fi]) {
                have[fi] = 1;
                good++;
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
        if (a < 0 || b < 0 || missing < 0) {
            /* Fewer than 2 good fragments — brief backoff before refetch. */
            if (attempt + 1 < max_attempts)
                usleep(100000u * (unsigned)(attempt + 1));
            continue;
        }
        if (efs_decode_chunk(frags, chunk_size, a, b, missing, chunk_out,
                             chunk_size) == 0) {
            rc = EFS_OK;
            break;
        }
    }
    return rc;
}

static int fetch_published_once(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                                uint32_t len, struct efs_chunk_entry *obs,
                                int *have_obs);

/* A fold or a CAS supersedes the chunk's base and every span it observed,
 * and the reaper deletes those objects within a second of the apply (L7,
 * no grace). A fetch against the map this client pulled before that fold
 * then gets NOT_FOUND on a fragment that is legitimately gone. That is a
 * stale map, not a lost chunk: pull the chunk's current row once and read
 * against it. A second failure is the caller's error (never zero-fill). */
int efs_client_fetch_published_chunk(efs_ino_t ino, uint32_t ci,
                                     uint8_t *buf, uint32_t len)
{
    return efs_client_fetch_published_chunk_obs(ino, ci, buf, len, NULL,
                                                NULL);
}

/* obs/have_obs: the chunk row (base generation, span list, newest seq)
 * this image was built from. A fold's publish must name THAT row, not a
 * later read of the table — a GETCHUNKS window pulled for a neighbouring
 * chunk (D2 lanes) can refresh this row between the fetch and the PUT,
 * and a fold that names the refreshed list while holding the old image
 * passes the server's FOLD_LIST check and drops the newer peer span
 * (IOR hard 36-rank cold verify: 7 of 288000 records wrong, Sep 30). */
int efs_client_fetch_published_chunk_obs(efs_ino_t ino, uint32_t ci,
                                         uint8_t *buf, uint32_t len,
                                         struct efs_chunk_entry *obs,
                                         int *have_obs)
{
    int rc = fetch_published_once(ino, ci, buf, len, obs, have_obs);

    if (rc == EFS_OK || rc == EFS_ERR_INVAL || rc == EFS_ERR_NOMEM)
        return rc;
    {
        struct efs_chunk_entry b, a;
        int hb, ha, prc, rc2;
        static uint64_t last_log_us;
        struct timespec ts;
        uint64_t now;

        memset(&b, 0, sizeof(b));
        memset(&a, 0, sizeof(a));
        pthread_mutex_lock(&g_client.idx_mu);
        hb = (efs_export_get_chunk(&g_client.export, ino, ci, &b) == 0);
        pthread_mutex_unlock(&g_client.idx_mu);
        prc = efs_client_pull_chunks_range(ino, ci, ci + 1);
        if (prc != EFS_OK)
            return prc;
        rc2 = fetch_published_once(ino, ci, buf, len, obs, have_obs);
        if (rc2 == EFS_OK)
            return EFS_OK;
        pthread_mutex_lock(&g_client.idx_mu);
        ha = (efs_export_get_chunk(&g_client.export, ino, ci, &a) == 0);
        pthread_mutex_unlock(&g_client.idx_mu);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
        if (now - last_log_us > 1000000ull) {
            last_log_us = now;
            fprintf(stderr,
                    "efs: fetch published ino=%llu ci=%u rc=%d then pull rc=%d "
                    "rc=%d map before gen=%llx nd=%u seq=%llx (%d) after "
                    "gen=%llx nd=%u seq=%llx (%d)\n",
                    (unsigned long long)ino, ci, rc, prc, rc2,
                    (unsigned long long)b.generation, b.ndelta,
                    (unsigned long long)b.delta_seq, hb,
                    (unsigned long long)a.generation, a.ndelta,
                    (unsigned long long)a.delta_seq, ha);
        }
        return rc2;
    }
}

static int fetch_published_once(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                                uint32_t len, struct efs_chunk_entry *obs,
                                int *have_obs)
{
    uint32_t cs = data_chunk_size();
    if (!buf || !cs || len != cs)
        return EFS_ERR_INVAL;
    int published = 0;
    int stub = 0;
    struct efs_chunk_entry ce;
    uint32_t frag_len = data_frag_size();
    uint8_t zck[EFS_HASH_SIZE];

    memset(&ce, 0, sizeof(ce));
    pthread_mutex_lock(&g_client.idx_mu);
    published = (efs_export_get_chunk(&g_client.export, ino, ci, &ce) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    /* One read of the row serves the base decode, the span overlay and
     * the caller's observation. */
    if (obs)
        *obs = ce;
    if (have_obs)
        *have_obs = published;
    /* Hole, not a GET. Reasons a GET here is DECODE:
     *  - truncate stubs (zero digest / all-zero checksums, no objects)
     *  - size grown with no chunk map (ftruncate)
     *  - recycled ino leftover `{ci}.{fi}` (store keys ignore inode gen)
     * A published non-stub mapping is a real object; fail loud. */
    if (published) {
        efs_hash_zero_fragment_len(frag_len, zck);
        if ((memcmp(ce.checksums[0], zck, EFS_HASH_SIZE) == 0 &&
             memcmp(ce.checksums[1], zck, EFS_HASH_SIZE) == 0 &&
             memcmp(ce.checksums[2], zck, EFS_HASH_SIZE) == 0) ||
            (efs_bytes_are_zero(ce.checksums[0], EFS_HASH_SIZE) &&
             efs_bytes_are_zero(ce.checksums[1], EFS_HASH_SIZE) &&
             efs_bytes_are_zero(ce.checksums[2], EFS_HASH_SIZE)))
            stub = 1;
        else {
            efs_hash_zero_fragment(zck);
            if (memcmp(ce.checksums[0], zck, EFS_HASH_SIZE) == 0 &&
                memcmp(ce.checksums[1], zck, EFS_HASH_SIZE) == 0 &&
                memcmp(ce.checksums[2], zck, EFS_HASH_SIZE) == 0)
                stub = 1;
        }
    }
    if (obs && !published) {
        /* A flush merging onto "no row" builds its image on zeros. With
         * the local table a cache (D2 windows, the evictor) this can be
         * a dropped row, not a first write. Count it. */
        static uint64_t n, last_us;
        struct timespec ts;
        uint64_t now;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
        __sync_fetch_and_add(&n, 1);
        if (now - last_us > 1000000ull) {
            last_us = now;
            fprintf(stderr, "efs: merge base norow ino=%llu ci=%u n=%llu\n",
                    (unsigned long long)ino, ci, (unsigned long long)n);
        }
    }
    if (stub || !published ||
        (ce.read_view.count && ce.read_view.parts[0].len == 0))
        memset(buf, 0, len);
    else {
        int rc = efs_client_decode_placed_chunk_attempts(ino, ci, buf, cs,
                                                         frag_len, 2, 0, &ce);
        if (rc != EFS_OK)
            return rc;
    }
    if (!published)
        return EFS_OK;
    if (ce.read_view.count) {
        if (ce.read_view.chunk_size != len ||
            ce.read_view.count != ce.ndelta + 1 ||
            ce.read_view.parts[0].off || ce.read_view.parts[0].len > len)
            return EFS_ERR_PROTO;
        memset(buf + ce.read_view.parts[0].len, 0,
               len - ce.read_view.parts[0].len);
    }
    return overlay_chunk_deltas_ce(ino, ci, buf, len, &ce);
}


int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, uint32_t expected_frag_len,
                            uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE],
                            uint64_t chunk_generation)
{
    if (node_id == 0 || expected_frag_len == 0)
        return EFS_ERR_INVAL;

    /* Skip nodes already marked down: a dead pooled fd would otherwise cost a
     * full SO_RCVTIMEO (30s) per attempt before we even try the next peer. */
    if (efs_client_node_is_down(node_id))
        return EFS_ERR_NET;

    for (int attempt = 1; attempt <= 3; attempt++) {
        struct efs_conn *conn = efs_client_conn_get(node_id);
        if (!conn) {
            /* Not a node failure: conn_get counts connect failures
             * itself, and a pool checkout timeout is load, not death
             * (see put_fragments). */
            if (attempt < 3) {
                usleep(50000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        struct efs_msg_get_chunk req;
        memset(&req, 0, sizeof(req));
        req.export_id = g_client.export_id;
        req.ino = ino;
        req.chunk_index = chunk_index;
        req.fragment_index = fragment_index;
        if (chunk_generation && chunk_generation != EFS_CHUNK_BASE_UNCOND)
            req.chunk_generation = chunk_generation;
        else {
            struct efs_chunk_entry ce;
            pthread_mutex_lock(&g_client.idx_mu);
            if (efs_export_get_chunk(&g_client.export, ino, chunk_index,
                                     &ce) == 0 &&
                ce.generation != EFS_CHUNK_BASE_UNCOND)
                req.chunk_generation = ce.generation;
            pthread_mutex_unlock(&g_client.idx_mu);
        }

        /* Zero-copy receive: status + checksum + fragment land directly in the
         * caller's buffers — no malloc + 64 KiB memcpy per GET. */
        uint8_t reply_type = 0;
        uint8_t status = 0;
        if (efs_conn_send_msg(conn, EFS_MSG_GET_CHUNK, &req, sizeof(req)) != 0 ||
            efs_conn_recv_msg_into(conn, &reply_type, &status,
                                   checksum, EFS_HASH_SIZE,
                                   data, expected_frag_len) != 0 ||
            reply_type != EFS_MSG_GET_CHUNK_REPLY) {
            efs_client_conn_drop(node_id, conn);
            efs_client_node_note_fail(node_id);
            if (attempt < 3) {
                usleep(50000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        if (status != EFS_GET_CHUNK_OK) {
            efs_client_conn_release(node_id, conn);
            return EFS_ERR_NOT_FOUND;
        }
        *data_len = expected_frag_len;

        /* Verify payload. Known-zero digests are trusted (writer/store already
         * short-circuit zeros); avoid a 64KiB memcmp on every GET.
         * Read-verify is OFF by default: at GB/s per client the inline
         * blake3 re-hash costs several cores and caps the frag pool, and
         * integrity is already covered server-side (write-time verify plus
         * the background scrubber, which also heals). EFS_READ_VERIFY=1
         * re-enables the end-to-end check for RDMA-CRC/ECC paranoia. */
        static int read_verify = -1;
        if (read_verify < 0) {
            const char *v = getenv("EFS_READ_VERIFY");
            read_verify = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
        }
        int sum_ok;
        if (!read_verify) {
            sum_ok = 1;
        } else {
            uint8_t zero_ck[EFS_HASH_SIZE];
            if (expected_frag_len == EFS_META_FRAGMENT_SIZE)
                efs_hash_zero_fragment(zero_ck);
            else
                efs_hash_zero_fragment_len(expected_frag_len, zero_ck);
            if (memcmp(checksum, zero_ck, EFS_HASH_SIZE) == 0) {
                sum_ok = 1;
            } else {
                uint8_t verify[EFS_HASH_SIZE];
                efs_hash(data, expected_frag_len, verify);
                sum_ok = (memcmp(verify, checksum, EFS_HASH_SIZE) == 0);
            }
        }
        if (!sum_ok) {
            efs_client_conn_release(node_id, conn);
            return EFS_ERR_CHECKSUM;
        }

        efs_client_conn_release(node_id, conn);
        efs_client_node_note_ok(node_id);
        return EFS_OK;
    }
    return EFS_ERR_NET;
}

struct get_batch {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int remaining;
};

/* 4096 x 2 x 128 KiB = 1 GiB. Part D bypasses the kernel page cache
 * (FOPEN_DIRECT_IO); this is the client read cache for demand + prefetch.
 * Writes invalidate per-ci after PUT. */
#define RDCACHE_SLOTS  4096
#define RDCACHE_WAYS   2
#define RDCACHE_STRIPES 64
struct rdcache_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
    uint32_t tick;
    uint64_t gen; /* chunk-map generation these bytes were decoded from */
    int pending;  /* a fetch of this key is in flight; data may be stale */
    int pins;     /* readers replying straight out of data (efs_client_read_refs):
                   * never a victim, never overwritten while > 0 */
};
static struct {
    pthread_mutex_t mu[RDCACHE_STRIPES];
    pthread_cond_t cv[RDCACHE_STRIPES];
    int mu_ready;
    struct rdcache_ent e[RDCACHE_SLOTS][RDCACHE_WAYS];
    uint32_t tick;
} g_rdcache;

static pthread_mutex_t g_rdcache_init_mu = PTHREAD_MUTEX_INITIALIZER;

static void rdcache_ensure(void)
{
    if (__atomic_load_n(&g_rdcache.mu_ready, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&g_rdcache_init_mu);
    if (!g_rdcache.mu_ready) {
        for (int i = 0; i < RDCACHE_STRIPES; i++) {
            pthread_mutex_init(&g_rdcache.mu[i], NULL);
            pthread_cond_init(&g_rdcache.cv[i], NULL);
        }
        __atomic_store_n(&g_rdcache.mu_ready, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_rdcache_init_mu);
}

static uint32_t rdcache_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)ci * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(h & (RDCACHE_SLOTS - 1));
}

static pthread_mutex_t *rdcache_mu(uint32_t slot)
{
    rdcache_ensure();
    return &g_rdcache.mu[slot & (RDCACHE_STRIPES - 1)];
}

/* Pressure can discard only reproducible, unpinned clean read bodies.
 * Pending fetches and zero-copy reply pins retain their owner. */
void efs_rdcache_trim(void)
{
    rdcache_ensure();
    for (uint32_t sh = 0; sh < RDCACHE_STRIPES; sh++) {
        pthread_mutex_lock(&g_rdcache.mu[sh]);
        for (uint32_t s = sh; s < RDCACHE_SLOTS; s += RDCACHE_STRIPES)
            for (uint32_t w = 0; w < RDCACHE_WAYS; w++) {
                struct rdcache_ent *e = &g_rdcache.e[s][w];
                if (e->data && !e->pending && !e->pins) {
                    efs_buf_free(e->data, e->len);
                    memset(e, 0, sizeof(*e));
                }
            }
        pthread_mutex_unlock(&g_rdcache.mu[sh]);
    }
}

/* Called only while constructing demand jobs, before taking index/cache
 * locks or submitting the batch. Reclaim reproducible bodies first, then
 * allow in-flight owners a bounded opportunity to release capacity. */
static void *demand_read_alloc(uint32_t len)
{
    if (efs_client_rpc_past_deadline())
        return NULL;
    void *p = efs_buf_alloc(len);
    if (p) return p;
    efs_rdcache_trim();
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        if (efs_client_rpc_past_deadline())
            return NULL;
        p = efs_buf_alloc(len);
        if (p) return p;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&delay, NULL);
    }
    return NULL;
}

static struct rdcache_ent *rdcache_find(uint32_t s, efs_ino_t ino, uint32_t ci)
{
    for (int w = 0; w < RDCACHE_WAYS; w++) {
        struct rdcache_ent *e = &g_rdcache.e[s][w];
        if (e->data && e->ino == ino && e->ci == ci)
            return e;
    }
    return NULL;
}

/* Generation the local chunk map names. 0 = no row. Taken without the
 * rdcache lock: idx_mu must not be acquired under an rdcache stripe. */
static uint64_t rdcache_map_gen(efs_ino_t ino, uint32_t ci)
{
    struct efs_chunk_entry ce;
    int ok;
    uint64_t g;

    pthread_mutex_lock(&g_client.idx_mu);
    ok = efs_export_get_chunk(&g_client.export, ino, ci, &ce) == 0;
    pthread_mutex_unlock(&g_client.idx_mu);
    if (!ok)
        return 0;
    g = efs_chunk_read_key(&ce);
    return g;
}

/* Fetch one span object and paint [off, len) onto the base image.
 * The object is a full chunk generation (export fragment size, so
 * O_DIRECT stores it). Only the published range is copied; the rest
 * of that generation is not the base. A failure fails the read:
 * dropping the span would return a hole where a writer committed. */
static int overlay_one_delta(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                             uint32_t chunk_len, const struct efs_chunk_delta *d)
{
    uint32_t frag_len, i, ngood;
    uint8_t *decoded, *fbuf;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    int have[EFS_NUM_FRAGMENTS];
    int a = -1, b = -1, missing = -1;

    if (!d || d->len == 0 || (uint64_t)d->off + d->len > chunk_len ||
        (chunk_len % 2u) != 0)
        return EFS_ERR_INVAL;
    frag_len = chunk_len / 2;
    decoded = malloc(chunk_len);
    fbuf = malloc((size_t)EFS_NUM_FRAGMENTS * frag_len);
    if (!decoded || !fbuf) {
        free(decoded);
        free(fbuf);
        return EFS_ERR_NOMEM;
    }
    memset(decoded, 0, chunk_len);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        frags[i] = fbuf + (size_t)i * frag_len;
    memset(have, 0, sizeof(have));
    ngood = 0;
    for (i = 0; i < EFS_NUM_FRAGMENTS && ngood < 2; i++) {
        uint32_t got = 0;
        uint8_t sum[EFS_HASH_SIZE];
        int grc = efs_client_get_fragment(d->nodes[i], ino, ci, i, frag_len,
                                          frags[i], &got, sum, d->generation);

        if (grc == EFS_OK && got == frag_len) {
            have[i] = 1;
            ngood++;
        }
    }
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!have[i]) {
            if (missing < 0)
                missing = (int)i;
        } else if (a < 0) {
            a = (int)i;
        } else if (b < 0) {
            b = (int)i;
        }
    }
    if (a < 0 || b < 0 || missing < 0 ||
        efs_decode_chunk(frags, chunk_len, a, b, missing, decoded,
                         chunk_len) != 0) {
        free(decoded);
        free(fbuf);
        return EFS_ERR_DECODE;
    }
    memcpy(buf + d->off, decoded + d->off, d->len);
    free(decoded);
    free(fbuf);
    return EFS_OK;
}

static int overlay_chunk_deltas_ce(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                                   uint32_t chunk_len,
                                   const struct efs_chunk_entry *cep)
{
    struct efs_chunk_entry ce = *cep;
    uint32_t i;
    int rc = EFS_OK;

    for (i = 0; i < ce.ndelta && rc == EFS_OK; i++) {
        /* A fold leaves a len-0 tombstone in the trailer (the apply keeps
         * and skips it, meta_apply.c). It carries no bytes; painting it
         * was EFS_ERR_INVAL, and every read of a once-folded chunk failed
         * with EIO (posix basic_overwrite_middle, Sep 29). */
        if (ce.deltas[i].len == 0)
            continue;
        struct efs_chunk_delta d = ce.deltas[i];
        if (ce.read_view.count) {
            const struct efs_fence_part *p;
            if (ce.read_view.chunk_size != chunk_len ||
                ce.read_view.count != ce.ndelta + 1)
                return EFS_ERR_PROTO;
            p = &ce.read_view.parts[i + 1];
            if (p->off != d.off || p->len > d.len)
                return EFS_ERR_PROTO;
            d.len = p->len;
            if (!d.len)
                continue;
        }
        rc = overlay_one_delta(ino, ci, buf, chunk_len, &d);
    }
    return rc;
}



int efs_rdcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    uint64_t tg;

    /* Dirty write-combined chunks are newer than anything on the servers. */
    if (efs_dcache_get(ino, ci, dst, len) == 0)
        return 0;
    if (!dst || !len)
        return -1;
    tg = rdcache_map_gen(ino, ci);
    if (!tg)
        return -1;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e && e->gen == tg && e->len >= len) {
        memcpy(dst, e->data, len);
        e->tick = ++g_rdcache.tick;
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

int efs_rdcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                     uint8_t *dst, uint32_t len)
{
    uint64_t tg;

    if (!dst || !len)
        return -1;
    tg = rdcache_map_gen(ino, ci);
    if (!tg)
        return -1;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e && e->gen == tg && (uint64_t)off + len <= e->len) {
        memcpy(dst, e->data + off, len);
        e->tick = ++g_rdcache.tick;
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

/* owned: src is a pool buffer the caller gives up; it becomes the way's
 * body (no memcpy) and the way's old body goes back to the pool. Returns
 * 1 when ownership was taken, 0 when the caller still owns src (a copy
 * was made, or nothing was stored). */
static int rdcache_put_inner(efs_ino_t ino, uint32_t ci, const uint8_t *src,
                             uint32_t len, int owned, uint64_t observed)
{
    uint64_t tg;

    if (!src || !len)
        return 0;
    tg = rdcache_map_gen(ino, ci);
    if (observed)
        tg = observed; /* never label captured bytes with a later map */
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    /* The way rdcache_acquire marked pending for this key first: it may
     * have no data yet (a fresh way), and rdcache_find skips those. A put
     * that lands in another way leaves that mark behind and the next
     * reader of the chunk waits on it forever (Oct 1 07:25Z, posix
     * basic_pread_pwrite D-state on a fresh mount). */
    struct rdcache_ent *e = NULL;
    for (int w = 0; w < RDCACHE_WAYS; w++) {
        struct rdcache_ent *c = &g_rdcache.e[s][w];
        if (c->ino == ino && c->ci == ci && (c->pending || c->data)) {
            e = c;
            if (c->pending)
                break;
        }
    }
    if (e && e->pins > 0 && e->data && !e->pending) {
        /* A reader is replying out of these bytes: leave them. The
         * entry is current for its gen; a newer gen of the same chunk
         * is fetched again by the next reader (the gen check fails). */
        pthread_mutex_unlock(mu);
        return 0;
    }
    if (!e) {
        /* Victim: a free way, else the LRU way; never a way pending for
         * another key (its waiters would be stranded) or pinned. */
        for (int w = 0; w < RDCACHE_WAYS; w++) {
            struct rdcache_ent *c = &g_rdcache.e[s][w];
            if (c->pending || c->pins > 0)
                continue;
            if (!c->data) {
                e = c;
                break;
            }
            if (!e || c->tick < e->tick)
                e = c;
        }
        if (!e) {
            pthread_mutex_unlock(mu);
            return 0;
        }
    }
    int took = 0;
    if (owned) {
        /* Prefetch hand-off (Oct 1 read review): the worker's buffer
         * becomes the way; the 128 KiB memcpy per prefetched chunk is
         * gone. The old body goes back to the pool. */
        if (e->data)
            efs_buf_free(e->data, e->len);
        e->data = (uint8_t *)(uintptr_t)src;
        took = 1;
    } else {
        if (e->data && e->len < len) {
            /* Oversized-chunk export: the pooled buffer can't grow. */
            efs_buf_free(e->data, e->len);
            e->data = NULL;
        }
        if (!e->data) {
            /* Pool buffers are EFS_CHUNK_SIZE-capacity; allocate once per way. */
            e->data = efs_buf_alloc_prefetch(len);
            if (!e->data) {
                /* Release the mark or its waiters never wake. */
                if (e->pending) {
                    e->pending = 0;
                    e->ino = 0;
                    pthread_cond_broadcast(
                        &g_rdcache.cv[s & (RDCACHE_STRIPES - 1)]);
                }
                pthread_mutex_unlock(mu);
                return 0;
            }
        }
        memcpy(e->data, src, len);
    }
    e->len = len;
    e->ino = ino;
    e->ci = ci;
    e->gen = tg;
    e->pending = 0;
    e->tick = ++g_rdcache.tick;
    pthread_cond_broadcast(&g_rdcache.cv[s & (RDCACHE_STRIPES - 1)]);
    pthread_mutex_unlock(mu);
    return took;
}

void efs_rdcache_put(efs_ino_t ino, uint32_t ci, const uint8_t *src, uint32_t len)
{
    (void)rdcache_put_inner(ino, ci, src, len, 0, 0);
}

/* Store a pool buffer by ownership transfer. 1 = the cache owns buf now;
 * 0 = the caller still does. */
int efs_rdcache_put_owned(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                          uint32_t len)
{
    return rdcache_put_inner(ino, ci, buf, len, 1, 0);
}

/* Pin the current image of (ino, ci) for a zero-copy reply. Returns the
 * entry handle (opaque) and its bytes, or NULL when there is no current,
 * complete, non-pending image. The entry is neither replaced nor
 * overwritten until efs_rdcache_unpin. */
void *efs_rdcache_pin(efs_ino_t ino, uint32_t ci, uint32_t len,
                      const uint8_t **data)
{
    uint64_t tg = rdcache_map_gen(ino, ci);
    if (!tg || !len)
        return NULL;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e && !e->pending && e->data && e->gen == tg && e->len >= len) {
        e->pins++;
        e->tick = ++g_rdcache.tick;
        *data = e->data;
        pthread_mutex_unlock(mu);
        return e;
    }
    pthread_mutex_unlock(mu);
    return NULL;
}

void efs_rdcache_unpin(void *handle)
{
    struct rdcache_ent *e = handle;
    if (!e)
        return;
    uint32_t s = (uint32_t)((e - &g_rdcache.e[0][0]) / RDCACHE_WAYS);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    if (e->pins > 0)
        e->pins--;
    pthread_mutex_unlock(mu);
}

void efs_rdcache_invalidate(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e) {
        e->ino = 0;
        e->pending = 0;
        pthread_cond_broadcast(&g_rdcache.cv[s & (RDCACHE_STRIPES - 1)]);
    }
    pthread_mutex_unlock(mu);
}

struct chunk_get_job {
    efs_ino_t ino;
    uint32_t ci;
    int have_ce;
    int cacheable;
    int owned; /* prefetch: get_pool_thread frees chunk + this struct */
    int ext;   /* chunk points into the caller's read buffer: no pool
                * buffer, no copy-out, never freed here */
    int rc;
    uint8_t *chunk; /* data_chunk_size() bytes, owned by caller unless owned */
    struct get_batch *bp;
};

static int rdcache_acquire(efs_ino_t ino, uint32_t ci, uint8_t *dst,
                           uint32_t len);
static void rdcache_cancel(efs_ino_t ino, uint32_t ci);

static void *chunk_get_worker(void *arg)
{
    struct chunk_get_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    int acq;
    /* Same-mount read of a chunk this client still holds. efs_dcache_copy
     * serves a current image and refuses one the map does not name. */
    if (efs_dcache_copy(job->ino, job->ci, 0, job->chunk, chunk_size) == 0) {
        job->rc = EFS_OK;
        return NULL;
    }
    struct efs_chunk_entry cached_view = {0};
    pthread_mutex_lock(&g_client.idx_mu);
    int have_cached_view = efs_export_get_chunk(&g_client.export, job->ino,
                                               job->ci, &cached_view) == EFS_OK;
    pthread_mutex_unlock(&g_client.idx_mu);
    uint64_t cached_key = have_cached_view ? efs_chunk_read_key(&cached_view) : 0;
    acq = rdcache_acquire(job->ino, job->ci, job->chunk, chunk_size);
    if (acq == 0) {
        efs_dcache_overlay(job->ino, job->ci, job->chunk, chunk_size);
        /* The cached image is the base. Spans published after it was
         * stored are not in it; paint them or the read hides a peer
         * (peer_overlap_pwrite_chunk_straddle). */
        job->rc = cached_key && cached_key == rdcache_map_gen(job->ino, job->ci)
            ? overlay_chunk_deltas_ce(job->ino, job->ci, job->chunk,
                                       chunk_size, &cached_view)
            : EFS_ERR_STALE;
        if (job->rc == EFS_OK)
            return NULL;
        /* A span this map names is gone (folded and reaped): the cached
         * base is stale too. Drop it and fetch against a fresh row. */
        efs_rdcache_invalidate(job->ino, job->ci);
        job->have_ce = 1;
    }
    if (!job->have_ce) {
        /* have_ce was snapshotted by the reader thread before this worker
         * ran. A concurrent flush commits the chunk (dcache_put_now sets the
         * local chunk mapping) and the reclaimer can then drop the dcache
         * entry, so the overlay below would find nothing and zero-fill a
         * chunk that really has data (basic_chunk_boundary read XY\0). Re-
         * check the local mapping now; if it landed, fall through to fetch. */
        pthread_mutex_lock(&g_client.idx_mu);
        if (efs_export_get_chunk(&g_client.export, job->ino, job->ci,
                                 NULL) == EFS_OK)
            job->have_ce = 1;
        pthread_mutex_unlock(&g_client.idx_mu);
    }
    if (!job->have_ce) {
        /* A hole inside the file: no row after the batch's layout pull.
         * Legit for sparse files; on an IOR file it is a lost row.
         * Count it, one line per second (Sep 30 IO-500 easy-read 1869
         * wrong 1 MiB reads with no client error line at all). */
        static uint64_t holes, last_us;
        struct timespec ts;
        uint64_t now;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
        __sync_fetch_and_add(&holes, 1);
        if (now - last_us > 1000000ull) {
            last_us = now;
            fprintf(stderr, "efs: read hole ino=%llu ci=%u holes=%llu\n",
                    (unsigned long long)job->ino, job->ci,
                    (unsigned long long)holes);
        }
        memset(job->chunk, 0, chunk_size);
        efs_dcache_overlay(job->ino, job->ci, job->chunk, chunk_size);
        job->rc = EFS_OK;
        if (acq == 1)
            rdcache_cancel(job->ino, job->ci);
        return NULL;
    }
    /* fetch covers a generation-0 base (first spans hang off an empty
     * row) and paints every span. decode_placed_chunk would fail that
     * row: its nodes are zero. */
    struct efs_chunk_entry observed;
    int have_observed = 0;
    job->rc = efs_client_fetch_published_chunk_obs(
        job->ino, job->ci, job->chunk, chunk_size, &observed, &have_observed);
    uint64_t observed_key = have_observed ? efs_chunk_read_key(&observed) : 0;
    if (job->rc == EFS_OK) {
        efs_dcache_overlay(job->ino, job->ci, job->chunk, chunk_size);
        if (job->cacheable && have_observed && job->owned) {
            /* Prefetch: the buffer becomes the cache way, no copy. */
            if (rdcache_put_inner(job->ino, job->ci, job->chunk,
                                      chunk_size, 1, observed_key))
                job->chunk = NULL;
        } else if (job->cacheable && have_observed) {
            (void)rdcache_put_inner(job->ino, job->ci, job->chunk, chunk_size,
                                    0, observed_key);
        } else if (acq == 1) {
            rdcache_cancel(job->ino, job->ci);
        }
    } else if (acq == 1) {
        rdcache_cancel(job->ino, job->ci);
    }
    if (acq == 1)
        rdcache_cancel(job->ino, job->ci);
    return NULL;
}

#define GET_POOL_QDEPTH 32
/* One synchronous chunk fetch per worker, so this is the client's
 * in-flight read cap: 32 × 128 KiB at ~0.9 ms per chunk was the 4.6 GB/s
 * four-reader ceiling on fcstor007 (Oct 1). 64 workers, 8 MiB in flight. */
#define GET_POOL_N 64
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct chunk_get_job *q[GET_POOL_QDEPTH];
    int head, tail, count;
} g_get_sh[GET_POOL_N];
static struct {
    pthread_t tids[GET_POOL_N];
    int nworkers;
    int ready;
    int shutdown;
    int rr;
} g_get_pool;
static pthread_mutex_t g_get_init = PTHREAD_MUTEX_INITIALIZER;

static void *get_pool_thread(void *arg)
{
    int si = (int)(intptr_t)arg;
    for (;;) {
        struct chunk_get_job *job;
        int was_full;

        pthread_mutex_lock(&g_get_sh[si].mu);
        while (g_get_sh[si].count == 0 && !g_get_pool.shutdown)
            pthread_cond_wait(&g_get_sh[si].cv, &g_get_sh[si].mu);
        if (g_get_pool.shutdown && g_get_sh[si].count == 0) {
            pthread_mutex_unlock(&g_get_sh[si].mu);
            return NULL;
        }
        was_full = (g_get_sh[si].count == GET_POOL_QDEPTH);
        job = g_get_sh[si].q[g_get_sh[si].head];
        g_get_sh[si].head = (g_get_sh[si].head + 1) % GET_POOL_QDEPTH;
        g_get_sh[si].count--;
        if (was_full)
            pthread_cond_signal(&g_get_sh[si].cv);
        pthread_mutex_unlock(&g_get_sh[si].mu);

        chunk_get_worker(job);

        if (job->owned) {
            uint32_t cs = data_chunk_size();
            if (job->chunk)
                efs_buf_free(job->chunk, cs);
            free(job);
            continue;
        }

        struct get_batch *bp = job->bp;
        if (bp) {
            pthread_mutex_lock(&bp->mu);
            if (--bp->remaining == 0)
                pthread_cond_signal(&bp->cv);
            pthread_mutex_unlock(&bp->mu);
        }
    }
}

static int get_pool_ensure(void)
{
    uint32_t i;
    if (g_get_pool.ready)
        return 0;
    pthread_mutex_lock(&g_get_init);
    if (!g_get_pool.ready) {
        uint32_t n = GET_POOL_N;
        for (i = 0; i < n; i++) {
            pthread_mutex_init(&g_get_sh[i].mu, NULL);
            pthread_cond_init(&g_get_sh[i].cv, NULL);
        }
        for (i = 0; i < n; i++) {
            if (pthread_create(&g_get_pool.tids[i], NULL, get_pool_thread,
                               (void *)(intptr_t)i) != 0) {
                uint32_t s, j;
                g_get_pool.shutdown = 1;
                for (s = 0; s < n; s++) {
                    pthread_mutex_lock(&g_get_sh[s].mu);
                    pthread_cond_broadcast(&g_get_sh[s].cv);
                    pthread_mutex_unlock(&g_get_sh[s].mu);
                }
                for (j = 0; j < i; j++)
                    pthread_join(g_get_pool.tids[j], NULL);
                g_get_pool.shutdown = 0;
                pthread_mutex_unlock(&g_get_init);
                return -1;
            }
        }
        g_get_pool.nworkers = (int)n;
        g_get_pool.ready = 1;
    }
    pthread_mutex_unlock(&g_get_init);
    return 0;
}

/* Stop the read pool and join its workers. Thread exit runs the TLS
 * destructor that frees each worker's decode scratch, so skipping the
 * join leaks ~192 KiB per worker. Call only when no reads are in flight. */
void efs_client_read_pools_stop(void)
{
    uint32_t i;
    if (!g_get_pool.ready)
        return;
    g_get_pool.shutdown = 1;
    for (i = 0; i < (uint32_t)g_get_pool.nworkers; i++) {
        pthread_mutex_lock(&g_get_sh[i].mu);
        pthread_cond_broadcast(&g_get_sh[i].cv);
        pthread_mutex_unlock(&g_get_sh[i].mu);
    }
    for (i = 0; i < (uint32_t)g_get_pool.nworkers; i++)
        pthread_join(g_get_pool.tids[i], NULL);
    g_get_pool.ready = 0;
    g_get_pool.shutdown = 0;
}

static int get_pool_run(struct chunk_get_job *jobs, uint32_t batch)
{
    struct get_batch bp;
    uint8_t used[GET_POOL_N];
    uint32_t i;
    int base;

    if (batch == 0)
        return 0;
    if (batch == 1 || get_pool_ensure() != 0) {
        for (i = 0; i < batch; i++)
            chunk_get_worker(&jobs[i]);
        return 0;
    }
    pthread_mutex_init(&bp.mu, NULL);
    pthread_cond_init(&bp.cv, NULL);
    bp.remaining = (int)batch;
    memset(used, 0, sizeof(used));
    base = (int)__sync_fetch_and_add(&g_get_pool.rr, 1);
    for (i = 0; i < batch; i++) {
        int si = (int)(((unsigned)base + i) % (unsigned)g_get_pool.nworkers);
        jobs[i].bp = &bp;
        pthread_mutex_lock(&g_get_sh[si].mu);
        while (g_get_sh[si].count == GET_POOL_QDEPTH && !g_get_pool.shutdown)
            pthread_cond_wait(&g_get_sh[si].cv, &g_get_sh[si].mu);
        g_get_sh[si].q[g_get_sh[si].tail] = &jobs[i];
        g_get_sh[si].tail = (g_get_sh[si].tail + 1) % GET_POOL_QDEPTH;
        g_get_sh[si].count++;
        used[si] = 1;
        pthread_mutex_unlock(&g_get_sh[si].mu);
    }
    for (i = 0; i < (uint32_t)g_get_pool.nworkers; i++) {
        if (!used[i])
            continue;
        pthread_mutex_lock(&g_get_sh[i].mu);
        pthread_cond_signal(&g_get_sh[i].cv);
        pthread_mutex_unlock(&g_get_sh[i].mu);
    }
    pthread_mutex_lock(&bp.mu);
    while (bp.remaining > 0)
        pthread_cond_wait(&bp.cv, &bp.mu);
    pthread_mutex_unlock(&bp.mu);
    pthread_mutex_destroy(&bp.mu);
    pthread_cond_destroy(&bp.cv);
    return 0;
}

/* Non-blocking submit for prefetch. Skip when that worker's queue is
 * already half full so ahead-GET cannot stall a FUSE worker. */
static int get_pool_try_submit(struct chunk_get_job *job)
{
    int si;
    if (get_pool_ensure() != 0)
        return -1;
    si = (int)((unsigned)__sync_fetch_and_add(&g_get_pool.rr, 1) %
               (unsigned)g_get_pool.nworkers);
    pthread_mutex_lock(&g_get_sh[si].mu);
    if (g_get_pool.shutdown || g_get_sh[si].count >= GET_POOL_QDEPTH / 2) {
        pthread_mutex_unlock(&g_get_sh[si].mu);
        return -1;
    }
    g_get_sh[si].q[g_get_sh[si].tail] = job;
    g_get_sh[si].tail = (g_get_sh[si].tail + 1) % GET_POOL_QDEPTH;
    g_get_sh[si].count++;
    pthread_cond_signal(&g_get_sh[si].cv);
    pthread_mutex_unlock(&g_get_sh[si].mu);
    return 0;
}

static int rdcache_hit(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    int hit = 0;
    int w;
    pthread_mutex_lock(mu);
    for (w = 0; w < RDCACHE_WAYS; w++) {
        struct rdcache_ent *e = &g_rdcache.e[s][w];
        if (e->ino == ino && e->ci == ci && (e->data || e->pending)) {
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(mu);
    return hit;
}

/* 0 = dst filled from a finished entry. 1 = this caller owns the fetch
 * and must efs_rdcache_put or efs_rdcache_cancel. A second caller for a
 * key that is already pending waits and then copies. */
static int rdcache_acquire(efs_ino_t ino, uint32_t ci, uint8_t *dst,
                           uint32_t len)
{
    uint64_t tg = rdcache_map_gen(ino, ci);
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    int stripe = (int)(s & (RDCACHE_STRIPES - 1));

    if (!dst || !len)
        return 1;
    for (;;) {
        struct rdcache_ent *e = NULL;
        struct rdcache_ent *pend = NULL;
        int w;

        pthread_mutex_lock(mu);
        for (w = 0; w < RDCACHE_WAYS; w++) {
            struct rdcache_ent *c = &g_rdcache.e[s][w];
            if (c->ino == ino && c->ci == ci) {
                if (c->pending)
                    pend = c;
                else if (c->data)
                    e = c;
            }
        }
        if (e && e->gen == tg && e->len >= len && tg) {
            memcpy(dst, e->data, len);
            e->tick = ++g_rdcache.tick;
            pthread_mutex_unlock(mu);
            return 0;
        }
        if (pend) {
            pthread_cond_wait(&g_rdcache.cv[stripe], mu);
            pthread_mutex_unlock(mu);
            continue;
        }
        /* Victim: a free way, else the LRU way that is not pending for
         * another key. Taking a pending way would strand that key's
         * waiters into a second fetch. No way at all: fetch unmarked;
         * efs_rdcache_put picks its own slot. */
        e = NULL;
        for (w = 0; w < RDCACHE_WAYS; w++) {
            struct rdcache_ent *c = &g_rdcache.e[s][w];
            if (c->pending || c->pins > 0)
                continue;
            if (!c->data) {
                e = c;
                break;
            }
            if (!e || c->tick < e->tick)
                e = c;
        }
        if (e) {
            e->ino = ino;
            e->ci = ci;
            e->pending = 1;
            e->gen = 0;
        }
        pthread_mutex_unlock(mu);
        return 1;
    }
}

static void rdcache_cancel(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    int w;

    pthread_mutex_lock(mu);
    for (w = 0; w < RDCACHE_WAYS; w++) {
        struct rdcache_ent *e = &g_rdcache.e[s][w];
        if (e->ino == ino && e->ci == ci && e->pending) {
            e->pending = 0;
            if (!e->data)
                e->ino = 0;
        }
    }
    pthread_cond_broadcast(&g_rdcache.cv[s & (RDCACHE_STRIPES - 1)]);
    pthread_mutex_unlock(mu);
}

/* Chunks ahead of the demand window. 0 disables. libfuse 3.10.2 does not
 * negotiate FUSE_MAX_PAGES, so DIO sequential reads arrive as 128 KiB
 * FUSE requests; this is what keeps queue depth off the kernel. */
#define EFS_READ_PREFETCH_DEFAULT 16
static uint32_t prefetch_depth(void)
{
    static uint32_t n;
    static int once;
    if (!once) {
        const char *e = getenv("EFS_READ_PREFETCH");
        unsigned long v;
        n = EFS_READ_PREFETCH_DEFAULT;
        if (e && *e) {
            v = strtoul(e, NULL, 10);
            if (v <= (unsigned long)EFS_WRITE_PIPELINE)
                n = (uint32_t)v;
            else
                n = EFS_WRITE_PIPELINE;
        }
        once = 1;
    }
    return n;
}

static void prefetch_ahead(efs_ino_t ino, uint32_t from_ci, uint64_t file_size)
{
    uint32_t cs = data_chunk_size();
    uint32_t depth = prefetch_depth();
    uint32_t max_ci;
    uint32_t i;

    if (!cs || !depth || file_size == 0)
        return;
    max_ci = (uint32_t)((file_size + cs - 1) / cs);
    for (i = 0; i < depth; i++) {
        uint32_t ci = from_ci + i;
        struct chunk_get_job *job;
        int have_ce;

        if (ci >= max_ci)
            break;
        if (rdcache_hit(ino, ci) || efs_dcache_has(ino, ci))
            continue;
        pthread_mutex_lock(&g_client.idx_mu);
        have_ce = (efs_export_get_chunk(&g_client.export, ino, ci,
                                        NULL) == EFS_OK);
        pthread_mutex_unlock(&g_client.idx_mu);
        if (!have_ce)
            continue;
        job = calloc(1, sizeof(*job));
        if (!job)
            return;
        job->chunk = efs_buf_alloc_prefetch(cs);
        if (!job->chunk) {
            free(job);
            return;
        }
        job->ino = ino;
        job->ci = ci;
        job->have_ce = 1;
        job->cacheable = 1;
        job->owned = 1;
        job->rc = EFS_ERR_IO;
        if (get_pool_try_submit(job) != 0) {
            efs_buf_free(job->chunk, cs);
            free(job);
            return;
        }
    }
}

static void maybe_prefetch(int want, efs_ino_t ino, uint64_t end_off,
                           uint64_t file_size)
{
    uint32_t cs = data_chunk_size();
    uint32_t next_ci;

    if (!want || !cs)
        return;
    next_ci = (uint32_t)((end_off + cs - 1) / cs);
    /* Map window one data window ahead of this prefetch. A miss inside
     * the file still fails the read; this only warms the map. */
    if (file_size > end_off) {
        uint32_t max_ci = (uint32_t)((file_size + cs - 1) / cs);
        uint32_t depth = prefetch_depth();
        uint32_t a0 = next_ci + depth;
        int have = 1;

        /* Only when that chunk's map is not here yet. The miss path
         * re-pulls its window every 200 ms; a sequential read with
         * the maps already local must not pay a GETCHUNKS for that. */
        if (a0 < max_ci) {
            pthread_mutex_lock(&g_client.idx_mu);
            have = efs_export_get_chunk(&g_client.export, ino, a0, NULL) ==
                   EFS_OK;
            pthread_mutex_unlock(&g_client.idx_mu);
        }
        if (a0 < max_ci && !have)
            (void)efs_client_pull_layout_miss(ino, a0, a0 + 1);
    }
    prefetch_ahead(ino, next_ci, file_size);
}

/* Zero-copy read (Oct 1 read review): a chunk-aligned read whose every
 * chunk has a current rdcache image, no dirty dcache entry and no spans is
 * answered with pinned references to those images; libfuse writev's them
 * to the kernel and the 128 KiB copy per chunk into the FUSE buffer is
 * gone. Returns the number of refs (> 0), 0 when the caller must take
 * efs_client_read (any chunk missing: the slow path fetches, waits on the
 * prefetch, or copies), or < 0 on error. The caller unpins every ref with
 * efs_rdcache_unpin after the reply. Prefetch and the sequential-run
 * bookkeeping run here too so the pipeline stays ahead. */
static __thread efs_ino_t t_seq_ino;
static __thread uint64_t t_seq_next;
static __thread int t_seq_run;

int efs_client_read_refs(efs_ino_t ino, uint64_t offset, size_t size,
                         struct efs_read_ref *refs, int max_refs)
{
    uint32_t cs = data_chunk_size();

    if (!cs || size == 0 || (offset % cs) != 0 || (size % cs) != 0 ||
        size / cs > (size_t)max_refs || efs_ino_is_meta_table(ino))
        return 0;
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    int have_row = (efs_export_get_inode(&g_client.export, ino, &inode) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (!have_row || offset + size > inode.size)
        return 0;
    if (efs_client_pull_chunks_range(ino, (uint32_t)(offset / cs),
            (uint32_t)((offset + size - 1) / cs) + 1) != EFS_OK)
        return 0;
    int window_rc = efs_dcache_put_win_wait(ino);
    if (window_rc < 0)
        return window_rc;
    int n = (int)(size / cs);
    uint32_t ci0 = (uint32_t)(offset / cs);
    int got = 0;
    for (int i = 0; i < n; i++) {
        uint32_t ci = ci0 + (uint32_t)i;
        struct efs_chunk_entry sce;
        int spans;

        if (efs_dcache_has(ino, ci))
            break;
        pthread_mutex_lock(&g_client.idx_mu);
        spans = efs_export_get_chunk(&g_client.export, ino, ci, &sce) != 0 ||
                sce.ndelta > 0;
        pthread_mutex_unlock(&g_client.idx_mu);
        if (spans)
            break;
        refs[i].pin = efs_rdcache_pin(ino, ci, cs, &refs[i].data);
        if (!refs[i].pin)
            break;
        refs[i].len = cs;
        got++;
    }
    if (got < n) {
        for (int i = 0; i < got; i++)
            efs_rdcache_unpin(refs[i].pin);
        return 0;
    }
    efs_client_stage_touch(ino);
    if (t_seq_ino == ino && offset == t_seq_next)
        t_seq_run++;
    else
        t_seq_run = 1;
    t_seq_ino = ino;
    t_seq_next = offset + size;
    maybe_prefetch((t_seq_run >= 2) || (size >= cs), ino, offset + size,
                   inode.size);
    return got;
}

int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len)
{
    if (!out_len || (!buf && size))
        return EFS_ERR_INVAL;
    *out_len = 0;
    if (size && efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    int want_pf;
    uint32_t chunk_size;

    if (size == 0) {
        *out_len = 0;
        return EFS_OK;
    }

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    int have_row = (efs_export_get_inode(&g_client.export, ino, &inode) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    if (!have_row) {
        /* Row miss on an open fd: the staging-table evictor (client-cache
         * design Part A) may have dropped a clean row — refetch from the
         * owner before concluding anything (stat_ino does the getattr RPC
         * + adopt + copy-out). A genuine unlink-open ghost fails the RPC
         * and falls through: unpublished dirty ranges only. have_base=1
         * past EOF is a shrink leftover; FOPEN_DIRECT_IO ignores i_size. */
        int stat_rc = efs_client_stat_ino(ino, &inode);
        if (stat_rc != EFS_OK) {
            if (stat_rc != EFS_ERR_NOT_FOUND)
                return stat_rc;
            uint32_t cs = data_chunk_size();
            if (cs && size > 0 &&
                (offset / cs) == ((offset + size - 1) / cs)) {
                uint32_t ci = (uint32_t)(offset / cs);
                uint32_t off = (uint32_t)(offset % cs);
            if (efs_dcache_copy_unpub(ino, ci, off, (uint8_t *)buf,
                                      (uint32_t)size) == 0 ||
                efs_dcache_copy_kept(ino, ci, off, (uint8_t *)buf,
                                     (uint32_t)size) == 0) {
                    *out_len = size;
                    return EFS_OK;
                }
            }
            *out_len = 0;
            return EFS_OK;
        }
    }
    efs_client_stage_touch(ino);
    /* A full-image PUT in flight for this inode has the chunk's body and
     * no map row yet; read it after the window closes (efs_dcache_put_win_wait).
     * One check per read; lock-free when no window is open anywhere. */
    int window_rc = efs_dcache_put_win_wait(ino);
    if (window_rc < 0)
        return window_rc;
    uint64_t file_size = inode.size;

    if (offset >= file_size) {
        /* Size not yet reflected (dentry stub / unlink-open ghost): still
         * serve unpublished dirty ranges from this same fd. */
        uint32_t cs = data_chunk_size();
        if (cs && size > 0 &&
            (offset / cs) == ((offset + size - 1) / cs)) {
            uint32_t ci = (uint32_t)(offset / cs);
            uint32_t off = (uint32_t)(offset % cs);
            if (efs_dcache_copy_unpub(ino, ci, off, (uint8_t *)buf,
                                      (uint32_t)size) == 0) {
                *out_len = size;
                return EFS_OK;
            }
        }
        *out_len = 0;
        return EFS_OK;
    } else if (offset + size > file_size) {
        size = (size_t)(file_size - offset);
    }

    if (t_seq_ino == ino && offset == t_seq_next)
        t_seq_run++;
    else
        t_seq_run = 1;
    t_seq_ino = ino;
    t_seq_next = offset + size;
    chunk_size = data_chunk_size();
    want_pf = (t_seq_run >= 2) || (size >= chunk_size);

    size_t total = 0;
    uint64_t end = offset + size;
    /* Sub-chunk read: dirty dcache first (newer than rdcache), then
     * decoded cache — no malloc(128k) on the 4k path. */
    if (chunk_size && size < chunk_size &&
        (offset / chunk_size) == ((end - 1) / chunk_size)) {
        uint32_t ci = (uint32_t)(offset / chunk_size);
        uint32_t off = (uint32_t)(offset % chunk_size);
        /* Peer spans do not move the base generation, so a cached image
         * from our own fold stays "current" and hides them. The batch
         * path paints spans onto a decoded-cache hit; this 2 KiB path
         * must not return that hit raw (peer_overlap_pwrite_chunk_straddle). */
        int prc = efs_client_pull_chunks_range(ino, ci, ci + 1);
        if (prc != EFS_OK && prc != EFS_ERR_NOT_FOUND)
            return prc;
        if (efs_dcache_copy(ino, ci, off, (uint8_t *)buf, (uint32_t)size) == 0) {
            *out_len = size;
            maybe_prefetch(want_pf, ino, end, file_size);
            return EFS_OK;
        }
        {
            struct efs_chunk_entry sce;
            int spans;

            pthread_mutex_lock(&g_client.idx_mu);
            spans = efs_export_get_chunk(&g_client.export, ino, ci, &sce) == 0
                    && sce.ndelta > 0;
            pthread_mutex_unlock(&g_client.idx_mu);
            if (!spans &&
                efs_rdcache_copy(ino, ci, off, (uint8_t *)buf,
                                 (uint32_t)size) == 0) {
                *out_len = size;
                maybe_prefetch(want_pf, ino, end, file_size);
                return EFS_OK;
            }
        }
    }
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;

    for (uint64_t pos = offset; pos < end; ) {
        /* Build a batch of whole chunks covering [pos, end). */
        struct chunk_get_job jobs[EFS_WRITE_PIPELINE];
        uint32_t batch = 0;
        uint64_t batch_pos = pos;

        while (batch < pipe && batch_pos < end) {
            uint32_t ci = (uint32_t)(batch_pos / chunk_size);
            uint64_t cstart = (uint64_t)ci * chunk_size;
            memset(&jobs[batch], 0, sizeof(jobs[batch]));
            jobs[batch].ino = ino;
            jobs[batch].ci = ci;
            /* A whole chunk inside [offset, end) is fetched (or copied
             * out of the rdcache) straight into the caller's buffer: no
             * pool buffer and no 128 KiB copy-out per chunk (Oct 1 read
             * review: a prefetched chunk was copied four times between
             * the wire and the FUSE reply). */
            if (batch_pos == cstart && cstart + chunk_size <= end &&
                cstart >= offset) {
                jobs[batch].chunk = (uint8_t *)buf + (cstart - offset);
                jobs[batch].ext = 1;
            } else {
                jobs[batch].chunk = demand_read_alloc(chunk_size);
                if (!jobs[batch].chunk) {
                    for (uint32_t j = 0; j < batch; j++)
                        if (!jobs[j].ext)
                            efs_buf_free(jobs[j].chunk, chunk_size);
                    return efs_client_rpc_past_deadline() ? EFS_ERR_BUSY : EFS_ERR_NOMEM;
                }
            }
            jobs[batch].rc = EFS_ERR_IO;
            jobs[batch].cacheable = 1;
            batch_pos = ((uint64_t)ci + 1) * chunk_size;
            batch++;
        }

        /* Index lookup is idx_mu, not the table lock — g_client.lock is held
         * across serialize/snapshot and would stall every multi-job read. */
        pthread_mutex_lock(&g_client.idx_mu);
        for (uint32_t i = 0; i < batch; i++) {
            struct efs_chunk_entry ce;
            int got = efs_export_get_chunk(&g_client.export, jobs[i].ino,
                                           jobs[i].ci, &ce) == EFS_OK;
            (void)got;
            (void)ce;
            /* A peer span does not move the base generation. A local
             * row with ndelta==0 is not proof the map is complete.
             * Re-pull before painting. */
            jobs[i].have_ce = 0;
        }
        pthread_mutex_unlock(&g_client.idx_mu);

        /* A missing row inside the file size is a cache miss until the
         * owner says otherwise: the row (and its chunk array) is evicted on
         * last close / by the Part A evictor, and refilled by GETCHUNKS at
         * adopt. Pull the missing range for EVERY batch and only then treat
         * what is still absent as a hole. A failed pull fails the read —
         * zero-filling here is how IO-500 ior-easy-read returned 38054
         * wrong 1 MiB reads in 1.36 s (see efs_client_pull_layout_miss). */
        {
            uint32_t miss0 = UINT32_MAX, miss1 = 0;
            for (uint32_t i = 0; i < batch; i++) {
                if (!jobs[i].have_ce) {
                    if (jobs[i].ci < miss0)
                        miss0 = jobs[i].ci;
                    if (jobs[i].ci > miss1)
                        miss1 = jobs[i].ci;
                }
            }
            if (miss0 != UINT32_MAX) {
                int prc = efs_client_pull_chunks_range(ino, miss0, miss1 + 1);
                pthread_mutex_lock(&g_client.idx_mu);
                if (prc != EFS_OK) {
                    int cached = 1;

                    /* Unlink drops the inode row, so GETCHUNKS fails,
                     * but an open fd still has the map from adopt.
                     * Serving that map is the last layout; zero-filling
                     * a miss that has no row is still a failed read. */
                    for (uint32_t i = 0; i < batch; i++) {
                        if (jobs[i].have_ce)
                            continue;
                        if (efs_export_get_chunk(&g_client.export,
                                                 jobs[i].ino, jobs[i].ci,
                                                 NULL) == EFS_OK)
                            jobs[i].have_ce = 1;
                        else
                            cached = 0;
                    }
                    pthread_mutex_unlock(&g_client.idx_mu);
                    if (!cached) {
                        fprintf(stderr,
                                "efs: read ino=%llu ci=%u..%u layout pull rc=%d, "
                                "failing read instead of zero-filling\n",
                                (unsigned long long)ino, miss0, miss1, prc);
                        for (uint32_t j = 0; j < batch; j++)
                            if (!jobs[j].ext)
                                efs_buf_free(jobs[j].chunk, chunk_size);
                        return prc;
                    }
                } else {
                    for (uint32_t i = 0; i < batch; i++)
                        jobs[i].have_ce =
                            (efs_export_get_chunk(&g_client.export,
                                                  jobs[i].ino, jobs[i].ci,
                                                  NULL) == EFS_OK);
                    pthread_mutex_unlock(&g_client.idx_mu);
                }
            }
        }

        /* Prefetch the next window before this batch's wait so those
         * GETs overlap the copy-out. Demand joins an in-flight prefetch
         * through rdcache_acquire. */
        maybe_prefetch(want_pf, ino, batch_pos, file_size);
        get_pool_run(jobs, batch);

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK) {
                for (uint32_t j = i; j < batch; j++)
                    if (!jobs[j].ext)
                        efs_buf_free(jobs[j].chunk, chunk_size);
                return jobs[i].rc;
            }
            uint64_t chunk_start = (uint64_t)jobs[i].ci * chunk_size;
            uint32_t chunk_off = (pos > chunk_start) ? (uint32_t)(pos - chunk_start) : 0;
            size_t to_copy = end - pos;
            if (to_copy > chunk_size - chunk_off)
                to_copy = chunk_size - chunk_off;
            if (jobs[i].ext) {
                /* Already in place (chunk == buf + total, chunk_off 0). */
                total += to_copy;
                pos += to_copy;
                continue;
            }
            memcpy(buf + total, jobs[i].chunk + chunk_off, to_copy);
            total += to_copy;
            pos += to_copy;
            efs_buf_free(jobs[i].chunk, chunk_size);
            jobs[i].chunk = NULL;
        }
    }

    *out_len = total;
    return EFS_OK;
}
