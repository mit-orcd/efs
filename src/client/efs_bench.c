#include "../common/parse_host_port.h"
#include "efs/cli_help.h"
#include "client_internal.h"
#include "../bench/bench_local.h"
#include "../bench/blake3_bench.h"
#include "../bench/io_bench.h"
#include "../bench/storage_principles.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include <math.h>
#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/network.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include "efs/version.h"
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../common/recorder_stop.h"
#include "../common/recorder_start.h"
#include <time.h>
#include <unistd.h>

enum bench_mode {
    BENCH_MODE_NONE = 0,
    BENCH_MODE_NET,   /* --time: BENCH_PUT discard */
    BENCH_MODE_WRITE, /* --size: published sequential files */
    BENCH_MODE_READ,  /* --read: published files from --id */
    BENCH_MODE_META,  /* --meta: metadata create/stat/setattr/readdir/... */
};

static void usage(const char *prog)
{
    efs_cli_help(stderr,prog,NULL);
}

static pid_t g_perf_pid = -1;

static void stop_perf_recorder(void)
{
    efs_stop_recorder(&g_perf_pid);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_host_port(const char *s, char *host, size_t host_len, uint16_t *port)
{
    return efs_parse_host_port(s, host, host_len, port);
}

/* Per-thread PUT templates: header + zero payload; only headers change.
 * Avoids copying fragment bytes every chunk. */
static __thread uint8_t *tls_msgs;
static __thread uint32_t tls_frag_len;
static __thread size_t tls_msg_size;
static __thread int tls_reqs_ready;

static int ensure_tls_reqs(uint32_t frag_len)
{
    if (tls_reqs_ready && tls_msgs && tls_frag_len == frag_len)
        return 0;
    free(tls_msgs);
    tls_msgs = NULL;
    tls_frag_len = frag_len;
    tls_msg_size = sizeof(struct efs_msg_bench_put) + frag_len;
    tls_msgs = calloc(EFS_NUM_FRAGMENTS, tls_msg_size);
    if (!tls_msgs)
        return -1;
    tls_reqs_ready = 1;
    return 0;
}

/* Map a live node id → index in g_client.nodes[]. */
static int bench_node_index(efs_node_id_t nid)
{
    if (nid == 0)
        return -1;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        if (g_client.nodes[i].id == nid)
            return (int)i;
    }
    return -1;
}

/* sticky_conns[membership_index] holds a checked-out pool conn for the
 * worker lifetime. Pass NULL to use short-lived borrows.
 * Success = ≥2 acks (same 2+1 quorum as the FUSE client PUT path). */
static int put_chunk_fanout(efs_ino_t ino, uint32_t chunk_index,
                            const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                            const uint8_t *zero_frag,
                            const uint8_t checksum[EFS_HASH_SIZE],
                            uint64_t per_node_bytes[EFS_MAX_NODES],
                            struct efs_conn **sticky_conns)
{
    (void)zero_frag; /* templates stay zero-filled */
    uint8_t msg_type = EFS_MSG_BENCH_PUT;
    uint8_t reply_type_want = EFS_MSG_BENCH_PUT_REPLY;
    uint8_t ok_status = EFS_BENCH_PUT_OK;
    uint32_t bench_frag_len = EFS_FRAGMENT_SIZE;

    if (ensure_tls_reqs(bench_frag_len) != 0)
        return EFS_ERR_NOMEM;

    struct efs_conn *conns[EFS_NUM_FRAGMENTS];
    int idxs[EFS_NUM_FRAGMENTS];
    int pending[EFS_NUM_FRAGMENTS];
    int sticky_owned[EFS_NUM_FRAGMENTS];

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        conns[i] = NULL;
        idxs[i] = -1;
        pending[i] = 0;
        sticky_owned[i] = 0;
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        efs_node_id_t nid = nodes[i];
        int idx = bench_node_index(nid);
        if (idx < 0 || nid == 0)
            continue;
        idxs[i] = idx;
        /* Skip peers in down-cooldown; drop any sticky conn so we do not
         * block on a full SO_SNDTIMEO against a dead socket. */
        if (efs_client_node_is_down(nid)) {
            if (sticky_conns && sticky_conns[idx]) {
                efs_client_conn_drop(nid, sticky_conns[idx]);
                sticky_conns[idx] = NULL;
            }
            continue;
        }
        if (sticky_conns && sticky_conns[idx]) {
            struct efs_conn *sc = sticky_conns[idx];
            if (!sc->rc) {
                struct pollfd p = { .fd = sc->fd, .events = POLLOUT };
                int pr = poll(&p, 1, 0);
                if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    efs_client_conn_drop(nid, sc);
                    sticky_conns[idx] = NULL;
                    efs_client_node_note_fail(nid);
                    continue;
                }
                /* Not writable yet: skip this peer for this chunk only. Keep
                 * sticky conn — backpressure is not a hard failure. */
                if (pr == 0 || !(p.revents & POLLOUT))
                    continue;
            }
            conns[i] = sc;
            sticky_owned[i] = 1;
        } else {
            conns[i] = efs_client_conn_get(nid);
            if (!conns[i])
                continue;
            if (!conns[i]->rc) {
                struct pollfd p = { .fd = conns[i]->fd, .events = POLLOUT };
                int pr = poll(&p, 1, 0);
                if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    efs_client_conn_drop(nid, conns[i]);
                    efs_client_node_note_fail(nid);
                    continue;
                }
                if (pr == 0 || !(p.revents & POLLOUT)) {
                    efs_client_conn_release(nid, conns[i]);
                    continue;
                }
            }
            if (sticky_conns)
                sticky_conns[idx] = conns[i];
            sticky_owned[i] = sticky_conns ? 1 : 0;
        }
        struct efs_msg_bench_put *req =
            (struct efs_msg_bench_put *)(tls_msgs + (size_t)i * tls_msg_size);
        req->export_id = 0;
        req->ino = ino;
        req->chunk_index = chunk_index;
        req->fragment_index = (uint32_t)i;
        req->data_len = bench_frag_len;
        req->path_hint = 0;
        memcpy(req->checksum, checksum, EFS_HASH_SIZE);
        if (zero_frag)
            memcpy((uint8_t *)req + sizeof(*req), zero_frag, bench_frag_len);
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!conns[i])
            continue;
        struct efs_msg_bench_put *req =
            (struct efs_msg_bench_put *)(tls_msgs + (size_t)i * tls_msg_size);
        if (efs_conn_send_msg(conns[i], msg_type, req,
                              (uint32_t)tls_msg_size) != 0) {
            efs_client_conn_drop(nodes[i], conns[i]);
            efs_client_node_note_fail(nodes[i]);
            if (sticky_conns && idxs[i] >= 0)
                sticky_conns[idxs[i]] = NULL;
            conns[i] = NULL;
            continue;
        }
        pending[i] = 1;
    }

    int acks = 0;
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        struct pollfd pfds[EFS_NUM_FRAGMENTS];
        int map[EFS_NUM_FRAGMENTS];
        int npoll = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch(conns[i]);
            if (w == EFS_CONN_REPLY_READY)
                w = -1; /* handle below via recv */
            else if (w >= 0) {
                pfds[npoll].fd = w;
                pfds[npoll].events = POLLIN;
                pfds[npoll].revents = 0;
                map[npoll] = i;
                npoll++;
                continue;
            } else {
                w = -1; /* error: recv will fail and drop */
            }
            /* READY or error: consume immediately */
            (void)w;
            efs_node_id_t nid = nodes[i];
            int idx = idxs[i];
            uint8_t reply_type = 0, status = 0;
            if (efs_conn_recv_u8_reply(conns[i], &reply_type, &status) != 0 ||
                reply_type != reply_type_want || status != ok_status) {
                efs_client_conn_drop(nid, conns[i]);
                efs_client_node_note_fail(nid);
                if (sticky_conns && idx >= 0)
                    sticky_conns[idx] = NULL;
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            }
            acks++;
            efs_client_node_note_ok(nid);
            if (idx >= 0 && (uint32_t)idx < EFS_MAX_NODES)
                per_node_bytes[idx] += bench_frag_len;
            if (!sticky_owned[i])
                efs_client_conn_release(nid, conns[i]);
            /* sticky: keep conn checked out for the next chunk */
            conns[i] = NULL;
            pending[i] = 0;
        }
        if (acks >= 2)
            break;
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0)
            break;
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
            efs_node_id_t nid = nodes[i];
            int idx = idxs[i];
            uint8_t reply_type = 0, status = 0;
            if (efs_conn_recv_u8_reply(conns[i], &reply_type, &status) != 0 ||
                reply_type != reply_type_want || status != ok_status) {
                efs_client_conn_drop(nid, conns[i]);
                efs_client_node_note_fail(nid);
                if (sticky_conns && idx >= 0)
                    sticky_conns[idx] = NULL;
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            }
            acks++;
            efs_client_node_note_ok(nid);
            if (idx >= 0 && (uint32_t)idx < EFS_MAX_NODES)
                per_node_bytes[idx] += bench_frag_len;
            if (!sticky_owned[i])
                efs_client_conn_release(nid, conns[i]);
            /* sticky: keep conn checked out for the next chunk */
            conns[i] = NULL;
            pending[i] = 0;
        }
    }

    /* Quorum met: drain ready replies so sticky conns stay usable; only drop
     * when the peer is behind (avoid reconnect storms). */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!conns[i])
            continue;
        efs_node_id_t nid = nodes[i];
        int idx = idxs[i];
        if (acks >= 2) {
            if (efs_conn_reply_watch(conns[i]) == EFS_CONN_REPLY_READY) {
                uint8_t reply_type = 0, status = 0;
                if (efs_conn_recv_u8_reply(conns[i], &reply_type, &status) == 0 &&
                    reply_type == reply_type_want) {
                    if (status == ok_status && idx >= 0 &&
                        (uint32_t)idx < EFS_MAX_NODES)
                        per_node_bytes[idx] += bench_frag_len;
                    if (!sticky_owned[i])
                        efs_client_conn_release(nid, conns[i]);
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                }
            }
            efs_client_conn_drop(nid, conns[i]);
            if (sticky_conns && idx >= 0)
                sticky_conns[idx] = NULL;
        } else {
            efs_client_conn_drop(nid, conns[i]);
            efs_client_node_note_fail(nid);
            if (sticky_conns && idx >= 0)
                sticky_conns[idx] = NULL;
        }
        conns[i] = NULL;
        pending[i] = 0;
    }

    return (acks >= 2) ? EFS_OK : EFS_ERR_NO_QUORUM;
}

struct worker_arg {
    int id;
    double deadline;      /* network ceiling */
    uint64_t bytes;       /* fragment bytes sent/received */
    uint64_t chunks_ok;
    uint64_t chunks_fail;
    uint64_t per_node_bytes[EFS_MAX_NODES]; /* indexed by membership slot */
    uint8_t *zero_frag;
    uint8_t checksum[EFS_HASH_SIZE];
};

static uint64_t g_chunk_seq;

static uint64_t next_chunk(void)
{
    return __atomic_fetch_add(&g_chunk_seq, 1, __ATOMIC_RELAXED);
}

static void *worker_main(void *arg)
{
    struct worker_arg *a = arg;
    /* Network ceiling uses synthetic identity only; the server discards it. */
    efs_ino_t ino = ((efs_ino_t)0xBEEF << 32) | 1ULL;

    /* Sticky conns filled lazily; a down peer must not abort the worker. */
    struct efs_conn *sticky[EFS_MAX_NODES];
    for (int i = 0; i < EFS_MAX_NODES; i++)
        sticky[i] = NULL;

    while (now_sec() < a->deadline) {
        uint64_t seq = next_chunk();
        uint32_t chunk_index = (uint32_t)(seq & 0xffffffffu);
        efs_ino_t net_ino = ino + (seq >> 20);
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_place_fragments(g_client.nodes, g_client.node_count, net_ino,
                            chunk_index, nodes);
        int rc = put_chunk_fanout(net_ino, chunk_index, nodes, a->zero_frag,
                                  a->checksum, a->per_node_bytes, sticky);
        if (rc == EFS_OK) {
            a->chunks_ok++;
            a->bytes += (uint64_t)EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE;
        } else {
            a->chunks_fail++;
        }
    }

    for (uint32_t i = 0; i < g_client.node_count; i++) {
        if (sticky[i]) {
            efs_client_conn_release(g_client.nodes[i].id, sticky[i]);
            sticky[i] = NULL;
        }
    }
    return NULL;
}

/* Normal client lifecycle: committed membership, a session for identified
 * mutations/tickets, and the same dcache/REPORT batch machinery as FUSE. */
static int published_client_start(const char *seed, const char *export_name)
{
    char host[64]; uint16_t port;
    if (parse_host_port(seed, host, sizeof(host), &port)) return EFS_ERR_INVAL;
    const char *nodes[EFS_MAX_NODES] = {seed};
    memset(&g_client, 0, sizeof(g_client));
    pthread_mutex_init(&g_client.lock, NULL);
    efs_client_init_nodes(&g_client, nodes, 1);
    snprintf(g_client.export_name, sizeof(g_client.export_name), "%s", export_name);
    g_client.export_id = 1;
    efs_export_init(&g_client.export, 1, export_name);
    g_client.export.root.shard_bits = EFS_KV_SHARD_BITS;
    g_client.export.root.shard_count = 1u << EFS_KV_SHARD_BITS;
    g_client.export.chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    efs_client_setup_mount_token();
    int rc = efs_client_discover_nodes(&g_client, host, port);
    if (rc == EFS_OK) rc = efs_client_view_mount();
    if (rc == EFS_OK) rc = efs_client_session_start();
    if (rc != EFS_OK) {
        fprintf(stderr, "bench client initialization: %s (%d)\n", efs_strerror(rc), rc);
        return rc;
    }
    efs_dcache_init();
    efs_client_enable_meta_batch(4096);
    return EFS_OK;
}

struct data_worker {
    unsigned id;
    uint64_t limit, bytes;
    size_t io_size;
    double deadline;
    uint64_t run_id;
    int reading, rc;
};

static void *data_worker_main(void *arg)
{
    struct data_worker *w = arg;
    char name[80]; struct efs_inode row;
    efs_ino_t ino = 0, dir = 0;
    snprintf(name, sizeof(name), "bench-data-%llu-w%u",
             (unsigned long long)w->run_id, w->id);
    if (w->reading) {
        char path[160];
        snprintf(path, sizeof(path), "/%s/payload", name);
        w->rc = efs_client_lookup(path, &row);
        if (w->rc != EFS_OK) return NULL;
        ino = row.ino;
        if (w->limit && w->limit != row.size) { w->rc = EFS_ERR_IO; return NULL; }
        w->limit = row.size;
        if (!w->limit) { w->rc = EFS_ERR_IO; return NULL; }
    } else {
        w->rc = efs_client_create_result(EFS_ROOT_INO, name, S_IFDIR | 0755,
                                         getuid(), getgid(), 0, &dir);
        if (w->rc == EFS_OK)
            w->rc = efs_client_create_result(dir, "payload", S_IFREG | 0644,
                                             getuid(), getgid(), 0, &ino);
        if (w->rc != EFS_OK) return NULL;
    }
    char *buf = malloc(w->io_size);
    if (!buf) { w->rc = EFS_ERR_NOMEM; return NULL; }
    memset(buf, 'A', w->io_size);
    do {
        uint64_t offset = 0;
        while (offset < w->limit && (!w->deadline || now_sec() < w->deadline)) {
            size_t n = w->limit - offset < w->io_size ? (size_t)(w->limit-offset) : w->io_size;
            if (w->reading) {
                size_t got = 0;
                w->rc = efs_client_read(ino, offset, n, buf, &got);
                if (w->rc == EFS_OK && got != n) w->rc = EFS_ERR_IO;
                if (w->rc == EFS_OK) {
                    for (size_t j = 0; j < n; j++)
                        if (buf[j] != 'A') { w->rc = EFS_ERR_IO; break; }
                }
            } else w->rc = efs_client_write(ino, offset, n, buf);
            if (w->rc != EFS_OK) break;
            offset += n; w->bytes += n;
        }
        if (w->rc || !w->reading || !w->deadline) break;
    } while (now_sec() < w->deadline);
    if (!w->reading && w->rc == EFS_OK)
        w->rc = efs_client_report_dirty_ino(ino, 1);
    free(buf);
    return NULL;
}

static int run_data_bench(const char *seed, const char *export_name, int reading,
                          uint64_t total, size_t io_size, unsigned nw,
                          uint64_t id, double seconds, int want_perf)
{
    double started = now_sec();
    int rc = published_client_start(seed, export_name);
    if (rc != EFS_OK) return 1;
    char name[80];
    snprintf(name, sizeof(name), "bench-data-%llu-w*/payload", (unsigned long long)id);
    if (want_perf) {
        char path[512]; const char *p = getenv("EFS_PERF_PATH");
        if (p && *p) snprintf(path, sizeof(path), "%s", p);
        else snprintf(path, sizeof(path), "/tmp/efs-bench-perf-%d/perf.data", (int)getpid());
        g_perf_pid = efs_start_perf_recorder(getpid(), path);
        if (g_perf_pid < 0) rc = EFS_ERR_IO;
    }
    pthread_t threads[64]; struct data_worker workers[64];
    double deadline = seconds ? now_sec()+seconds : 0;
    unsigned launched = 0;
    for (unsigned i = 0; rc == EFS_OK && i < nw; i++) {
        workers[i] = (struct data_worker){.id=i, .limit=total/nw+(i<total%nw),
            .io_size=io_size, .run_id=id, .reading=reading,
            .deadline=deadline};
        if (pthread_create(&threads[i], NULL, data_worker_main, &workers[i])) rc=EFS_ERR_IO;
        else launched++;
    }
    uint64_t bytes = 0;
    for (unsigned i = 0; i < launched; i++) {
        if (pthread_join(threads[i], NULL) || workers[i].rc || !workers[i].bytes) {
            fprintf(stderr, "bench worker=%u bytes=%llu rc=%d\n", i,
                    (unsigned long long)workers[i].bytes, workers[i].rc);
            rc=EFS_ERR_IO;
        }
        bytes += workers[i].bytes;
    }
    if (efs_dcache_flush_all() != EFS_OK || efs_client_report_dirty(1) != EFS_OK)
        rc = EFS_ERR_IO;
    if (efs_client_shutdown() != EFS_OK) rc = EFS_ERR_IO;
    stop_perf_recorder();
    if (!bytes || (!seconds && total && bytes != total)) rc = EFS_ERR_IO;
    double wall = now_sec()-started;
    printf("BENCH_%s kind=%s logical_bytes=%llu wall_s=%.6f logical_GiB_s=%.6f "
           "workers=%u id=%llu rc=%d path=/%s\n", rc ? "FAIL" : "OK",
           reading ? "read" : "write", (unsigned long long)bytes, wall,
           bytes/(1073741824.0*wall), nw, (unsigned long long)id, rc, name);
    return rc ? 1 : 0;
}

/* ---------- metadata bench (--meta) ---------- */

#define META_PATH_LEN  256
#define META_NAME_LEN  64
#define META_PHASE_MKDIR    (1u << 0)
#define META_PHASE_CREATE   (1u << 1)
#define META_PHASE_STAT     (1u << 2)
#define META_PHASE_GETATTR  (1u << 3)
#define META_PHASE_READDIR  (1u << 4)
#define META_PHASE_SETATTR  (1u << 5)
#define META_PHASE_RENAME   (1u << 6)
#define META_PHASE_UNLINK   (1u << 7)
#define META_PHASE_ALL      0xffu

enum meta_phase {
    MP_MKDIR = 0,
    MP_CREATE,
    MP_STAT,
    MP_GETATTR,
    MP_READDIR,
    MP_SETATTR,
    MP_RENAME,
    MP_UNLINK
};

struct meta_dir {
    efs_ino_t ino;
    char name[META_NAME_LEN];
};

struct meta_file {
    efs_ino_t ino;
    uint32_t dir_idx;
    char name[META_NAME_LEN];
    char path[META_PATH_LEN];
};

struct meta_state {
    char root_name[64];
    char root_path[128];
    uint32_t ndirs;
    uint32_t nfiles;
    uint64_t write_size;
    int keep;
    struct meta_dir *dirs;
    struct meta_file *files;
    char *write_buf;
};

struct meta_warg {
    int id;
    int nworkers;
    enum meta_phase phase;
    struct meta_state *st;
    uint64_t ops;
    uint64_t fail;
    int first_rc;
};

static int u64_dec(char *dst, size_t cap, uint64_t v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (unsigned)(v % 10));
        v /= 10;
    } while (v);
    if ((size_t)n + 1 > cap)
        return -1;
    for (int i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    dst[n] = '\0';
    return n;
}

static uint32_t parse_meta_phases(const char *s)
{
    uint32_t m = 0;
    char buf[256];
    size_t n = strlen(s);
    if (n >= sizeof(buf))
        return 0;
    memcpy(buf, s, n + 1);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, "all") == 0)
            return META_PHASE_ALL;
        else if (strcmp(tok, "mkdir") == 0)
            m |= META_PHASE_MKDIR;
        else if (strcmp(tok, "create") == 0)
            m |= META_PHASE_CREATE;
        else if (strcmp(tok, "stat") == 0)
            m |= META_PHASE_STAT;
        else if (strcmp(tok, "getattr") == 0)
            m |= META_PHASE_GETATTR;
        else if (strcmp(tok, "readdir") == 0)
            m |= META_PHASE_READDIR;
        else if (strcmp(tok, "setattr") == 0)
            m |= META_PHASE_SETATTR;
        else if (strcmp(tok, "rename") == 0)
            m |= META_PHASE_RENAME;
        else if (strcmp(tok, "unlink") == 0)
            m |= META_PHASE_UNLINK;
        else
            return 0;
    }
    return m;
}

static void *meta_worker(void *arg)
{
    struct meta_warg *a = arg;
    struct meta_state *st = a->st;
    int w = a->id, nw = a->nworkers;

    switch (a->phase) {
    case MP_MKDIR:
        for (uint32_t i = (uint32_t)w; i < st->ndirs; i += (uint32_t)nw) {
            efs_ino_t ino = efs_client_create(EFS_ROOT_INO, st->dirs[i].name,
                                              S_IFDIR | 0755, 0, 0);
            if (ino == 0) {
                a->fail++;
                continue;
            }
            st->dirs[i].ino = ino;
            a->ops++;
        }
        break;
    case MP_CREATE:
        for (uint32_t i = (uint32_t)w; i < st->nfiles; i += (uint32_t)nw) {
            uint32_t di = i % st->ndirs;
            efs_ino_t ino = efs_client_create(st->dirs[di].ino, st->files[i].name,
                                              S_IFREG | 0644, 0, 0);
            if (ino == 0) {
                a->fail++;
                continue;
            }
            __atomic_store_n(&st->files[i].ino, ino, __ATOMIC_RELEASE);
            st->files[i].dir_idx = di;
            if (st->write_size && st->write_buf) {
                if (efs_client_write(ino, 0, (size_t)st->write_size,
                                     st->write_buf) != EFS_OK ||
                    efs_client_report_dirty_ino(ino, 1) != EFS_OK)
                    a->fail++;
            }
            a->ops++;
        }
        break;
    case MP_STAT:
        for (uint32_t i = (uint32_t)w; i < st->nfiles; i += (uint32_t)nw) {
            struct efs_inode row;
            if (efs_client_lookup(st->files[i].path, &row) != 0)
                a->fail++;
            else
                a->ops++;
        }
        break;
    case MP_GETATTR:
        for (uint32_t i = (uint32_t)w; i < st->nfiles; i += (uint32_t)nw) {
            struct efs_inode row;
            efs_ino_t ino = __atomic_load_n(&st->files[i].ino, __ATOMIC_ACQUIRE);
            if (ino == 0) {
                a->fail++;
                if (!a->first_rc)
                    a->first_rc = -1;
            } else {
                int grc = efs_client_rpc_getattr(g_client.export_id, ino, &row);
                if (grc != EFS_OK) {
                    a->fail++;
                    if (!a->first_rc)
                        a->first_rc = grc;
                } else {
                    a->ops++;
                }
            }
        }
        break;
    case MP_READDIR:
        for (uint32_t i = (uint32_t)w; i < st->ndirs; i += (uint32_t)nw) {
            uint32_t src = 0, done = 0;
            char name_cur[EFS_MAX_NAME] = "";
            while (!done) {
                struct efs_inode ents[EFS_READDIR_MAX];
                uint32_t n = EFS_READDIR_MAX;
                int rc = efs_client_rpc_readdir_cur(g_client.export_id,
                                                    st->dirs[i].ino, ents, &n,
                                                    &src, name_cur, &done);
                if (rc != EFS_OK) {
                    a->fail++;
                    break;
                }
                a->ops += n;
                if (n == 0 && !done)
                    break;
            }
        }
        break;
    case MP_SETATTR: {
        uint64_t now = (uint64_t)time(NULL);
        for (uint32_t i = (uint32_t)w; i < st->nfiles; i += (uint32_t)nw) {
            efs_ino_t ino = __atomic_load_n(&st->files[i].ino, __ATOMIC_ACQUIRE);
            if (ino == 0) {
                a->fail++;
                continue;
            }
            if (efs_client_chmod(ino, 0644) != 0)
                a->fail++;
            else
                a->ops++;
            if (efs_client_utimens(ino, now, 0) != 0)
                a->fail++;
            else
                a->ops++;
        }
        break;
    }
    case MP_RENAME:
        for (uint32_t i = (uint32_t)w; i < st->nfiles; i += (uint32_t)nw) {
            efs_ino_t ino = __atomic_load_n(&st->files[i].ino, __ATOMIC_ACQUIRE);
            uint32_t di = st->files[i].dir_idx;
            char tmp[META_NAME_LEN];
            tmp[0] = 'g';
            if (ino == 0 || di >= st->ndirs ||
                u64_dec(tmp + 1, sizeof(tmp) - 1, i) < 0) {
                a->fail++;
                continue;
            }
            if (efs_client_rename_at(ino, st->dirs[di].ino,
                                     st->files[i].name, st->dirs[di].ino,
                                     tmp) != 0) {
                a->fail++;
                if (!a->first_rc)
                    a->first_rc = g_client.last_err ? g_client.last_err : -1;
                continue;
            }
            a->ops++;
            if (efs_client_rename_at(ino, st->dirs[di].ino, tmp,
                                     st->dirs[di].ino,
                                     st->files[i].name) != 0) {
                a->fail++;
                if (!a->first_rc)
                    a->first_rc = g_client.last_err ? g_client.last_err : -1;
            } else
                a->ops++;
        }
        break;
    case MP_UNLINK:
        for (uint32_t i = (uint32_t)w; i < st->nfiles; i += (uint32_t)nw) {
            uint32_t di = st->files[i].dir_idx;
            if (di >= st->ndirs ||
                efs_client_unlink(st->dirs[di].ino, st->files[i].name, false) != 0)
                a->fail++;
            else
                a->ops++;
        }
        break;
    }
    return NULL;
}

static int run_meta_phase(struct meta_state *st, enum meta_phase phase,
                          const char *name, const char *rw, int nworkers,
                          uint32_t nitems)
{
    if (nitems == 0)
        return 0;
    int nw = nworkers;
    if (nw > (int)nitems)
        nw = (int)nitems;
    if (nw < 1)
        nw = 1;
    struct meta_warg *args = calloc((size_t)nw, sizeof(*args));
    pthread_t *tids = calloc((size_t)nw, sizeof(*tids));
    if (!args || !tids) {
        free(args);
        free(tids);
        return -1;
    }
    double t0 = now_sec();
    for (int i = 0; i < nw; i++) {
        args[i].id = i;
        args[i].nworkers = nw;
        args[i].phase = phase;
        args[i].st = st;
        if (pthread_create(&tids[i], NULL, meta_worker, &args[i]) != 0) {
            args[i].fail++;
            tids[i] = 0;
        }
    }
    for (int i = 0; i < nw; i++) {
        if (tids[i])
            pthread_join(tids[i], NULL);
    }
    double wall = now_sec() - t0;
    if (wall < 1e-9)
        wall = 1e-9;
    uint64_t ops = 0, fail = 0;
    for (int i = 0; i < nw; i++) {
        ops += args[i].ops;
        fail += args[i].fail;
    }
    printf("BENCH_OK kind=meta phase=%s rw=%s ops=%llu wall_s=%.3f ops_s=%.1f "
           "workers=%d fail=%llu",
           name, rw, (unsigned long long)ops, wall, (double)ops / wall, nw,
           (unsigned long long)fail);
    for (int i = 0; i < nw; i++) {
        if (args[i].first_rc)
            printf(" first_rc_w%d=%d", i, args[i].first_rc);
    }
    printf("\n");
    fflush(stdout);
    free(args);
    free(tids);
    return fail ? 1 : 0;
}

static int meta_preformat(struct meta_state *st)
{
    for (uint32_t i = 0; i < st->ndirs; i++) {
        /* Hashed under ROOT so 64 dirs spread across shards (nested dirs
         * stay on one hashed parent and serialize create on one owner). */
        if (snprintf(st->dirs[i].name, sizeof(st->dirs[i].name), "%s-d%u",
                     st->root_name, i) >= (int)sizeof(st->dirs[i].name))
            return -1;
        st->dirs[i].ino = 0;
    }
    for (uint32_t i = 0; i < st->nfiles; i++) {
        uint32_t di = i % st->ndirs;
        st->files[i].dir_idx = di;
        st->files[i].ino = 0;
        st->files[i].name[0] = 'f';
        if (u64_dec(st->files[i].name + 1, sizeof(st->files[i].name) - 1, i) < 0)
            return -1;
        if (snprintf(st->files[i].path, sizeof(st->files[i].path), "/%s/%s",
                     st->dirs[di].name, st->files[i].name) >=
            (int)sizeof(st->files[i].path))
            return -1;
    }
    return 0;
}

static int run_meta_bench(const char *seed, const char *export_name,
                          uint32_t nfiles, uint32_t ndirs, uint64_t write_size,
                          uint32_t phases, int keep, uint64_t id, int nworkers,
                          int want_perf)
{
    char host[64];
    uint16_t port = 0;
    if (parse_host_port(seed, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid seed address: %s\n", seed);
        return 1;
    }

    if (published_client_start(seed, export_name) != EFS_OK) return 1;

    if (nworkers < 1)
        nworkers = g_client.conn_pool_size;
    if (nworkers < 1)
        nworkers = 1;
    if (nworkers > 64)
        nworkers = 64;
    if (nworkers > g_client.conn_pool_size)
        fprintf(stderr,
                "note: workers=%d > conn_pool=%d; raise EFS_CLIENT_CONNS_PER_NODE\n",
                nworkers, g_client.conn_pool_size);

    int need_files = (phases & (META_PHASE_CREATE | META_PHASE_STAT |
                                META_PHASE_GETATTR | META_PHASE_READDIR |
                                META_PHASE_SETATTR | META_PHASE_RENAME |
                                META_PHASE_UNLINK)) != 0;
    int need_tree = need_files || (phases & META_PHASE_MKDIR);
    if (ndirs == 0)
        ndirs = 1;
    if (nfiles == 0 && need_files)
        nfiles = 1;

    struct meta_state st;
    memset(&st, 0, sizeof(st));
    st.ndirs = ndirs;
    st.nfiles = nfiles;
    st.write_size = write_size;
    st.keep = keep;
    st.dirs = calloc(ndirs, sizeof(*st.dirs));
    st.files = calloc(nfiles ? nfiles : 1, sizeof(*st.files));
    if (!st.dirs || !st.files) {
        free(st.dirs);
        free(st.files);
        return 1;
    }
    if (write_size) {
        st.write_buf = malloc((size_t)write_size);
        if (!st.write_buf) {
            free(st.dirs);
            free(st.files);
            return 1;
        }
        memset(st.write_buf, 'A', (size_t)write_size);
    }

    {
        char pidbuf[16], idbuf[16];
        u64_dec(pidbuf, sizeof(pidbuf), (uint64_t)getpid());
        u64_dec(idbuf, sizeof(idbuf), id);
        snprintf(st.root_name, sizeof(st.root_name), "bench-meta-%s-%s",
                 pidbuf, idbuf);
        snprintf(st.root_path, sizeof(st.root_path), "/%s", st.root_name);
    }
    if (meta_preformat(&st) != 0) {
        fprintf(stderr, "meta: path overflow\n");
        free(st.write_buf);
        free(st.dirs);
        free(st.files);
        return 1;
    }

    char perf_path[512];
    perf_path[0] = '\0';
    if (want_perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path),
                     "/tmp/efs-bench-perf-%d/perf.data", (int)getpid());
        g_perf_pid = efs_start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf; continuing\n");
        else
            printf("perf recording -> %s\n", perf_path);
        fflush(stdout);
    }

    printf("bench meta seed=%s:%u export=%s id=%u files=%u dirs=%u workers=%d "
           "size=%llu phases=0x%x keep=%d\n",
           host, port, g_client.export_name, (unsigned)g_client.export_id,
           nfiles, ndirs, nworkers, (unsigned long long)write_size, phases,
           keep);
    fflush(stdout);

    int any_fail = 0;
    if (need_tree)
        any_fail |= run_meta_phase(&st, MP_MKDIR, "mkdir", "write", nworkers,
                                   st.ndirs);
    if (need_files)
        any_fail |= run_meta_phase(&st, MP_CREATE, "create", "write", nworkers,
                                   st.nfiles);
    if (need_files) {
        uint32_t nzero = 0;
        for (uint32_t i = 0; i < st.nfiles; i++) {
            if (__atomic_load_n(&st.files[i].ino, __ATOMIC_ACQUIRE) == 0)
                nzero++;
        }
        if (nzero)
            fprintf(stderr, "meta: %u/%u file inos still 0 after create\n",
                    nzero, st.nfiles);
    }
    if (phases & META_PHASE_STAT)
        any_fail |= run_meta_phase(&st, MP_STAT, "stat", "read", nworkers,
                                   st.nfiles);
    if (phases & META_PHASE_GETATTR)
        any_fail |= run_meta_phase(&st, MP_GETATTR, "getattr", "read", nworkers,
                                   st.nfiles);
    if (phases & META_PHASE_READDIR)
        any_fail |= run_meta_phase(&st, MP_READDIR, "readdir", "read", nworkers,
                                   st.ndirs);
    if (phases & META_PHASE_SETATTR)
        any_fail |= run_meta_phase(&st, MP_SETATTR, "setattr", "write", nworkers,
                                   st.nfiles);
    if (phases & META_PHASE_RENAME)
        any_fail |= run_meta_phase(&st, MP_RENAME, "rename", "write", nworkers,
                                   st.nfiles);
    if (write_size && need_files) {
        if (efs_dcache_flush_all() != EFS_OK || efs_client_report_dirty(1) != EFS_OK)
            any_fail = 1;
    }
    if (!keep && need_tree) {
        if (need_files)
            any_fail |= run_meta_phase(&st, MP_UNLINK, "unlink", "write",
                                       nworkers, st.nfiles);
        for (uint32_t i = 0; i < st.ndirs; i++) {
            if (st.dirs[i].ino)
                if (efs_client_unlink(EFS_ROOT_INO, st.dirs[i].name, true) != EFS_OK)
                    any_fail = 1;
        }
    }

    stop_perf_recorder();
    if (want_perf && perf_path[0]) {
        char report[600], hot[600], cmd[900];
        snprintf(report, sizeof(report), "%s.report.txt", perf_path);
        snprintf(hot, sizeof(hot), "%s.hotpath.txt", perf_path);
        snprintf(cmd, sizeof(cmd),
                 "perf report --stdio --no-children --percent-limit 0.4 -i '%s' "
                 "> '%s' 2>/dev/null",
                 perf_path, report);
        int report_status = system(cmd);
        if (report_status != 0)
            fprintf(stderr, "perf report command failed: status=%d\n", report_status);
        FILE *in = fopen(report, "r");
        FILE *out = fopen(hot, "w");
        if (out) {
            fprintf(out, "=== efs / blake3 / network hotspots ===\n");
            if (in) {
                char line[1024];
                int tops = 0;
                while (fgets(line, sizeof(line), in)) {
                    if (strstr(line, "blake3") || strstr(line, "efs_") ||
                        strstr(line, "memcpy") || strstr(line, "memmove") ||
                        strstr(line, "send") || strstr(line, "recv") ||
                        strstr(line, "hash") || strstr(line, "write") ||
                        strstr(line, "put_") || strstr(line, "lookup") ||
                        strstr(line, "strtok") || strstr(line, "snprintf") ||
                        strstr(line, "malloc") || strstr(line, "free"))
                        fputs(line, out);
                }
                rewind(in);
                fprintf(out, "--- top symbols ---\n");
                while (fgets(line, sizeof(line), in) && tops < 25) {
                    const char *s = line;
                    while (*s == ' ' || *s == '\t')
                        s++;
                    if (s[0] >= '0' && s[0] <= '9' && strstr(s, "%")) {
                        fputs(line, out);
                        tops++;
                    }
                }
                fclose(in);
            }
            fclose(out);
        }
        printf("perf report: %s\nperf hotpath: %s\n", report, hot);
    }

    free(st.write_buf);
    free(st.dirs);
    free(st.files);
    if (efs_client_shutdown() != EFS_OK) any_fail = 1;
    return any_fail ? 1 : 0;
}

/* TEMP DEBUG: in-process crash backtrace (ptrace is blocked on the nodes). */
#include <execinfo.h>
#include <signal.h>
static void bench_fatal(int sig)
{
    void *frames[48];
    int nf = backtrace(frames, 48);
    backtrace_symbols_fd(frames, nf, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void bench_fatal_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bench_fatal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

int main(int argc, char **argv)
{
    efs_version_check_argv("efs-bench", argc, argv);

    setlinebuf(stdout);
    setlinebuf(stderr);
    bench_fatal_install();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench")) {
            if (i + 1 < argc && !strcmp(argv[i + 1], "principles"))
                return efs_bench_principles_main(argc, argv);
            if (i + 1 < argc && (!strcmp(argv[i + 1], "io") || !strcmp(argv[i + 1], "io-blake3")))
                return efs_bench_io_main(argc, argv);
            if (i + 1 < argc && !strcmp(argv[i + 1], "blake3")) {
                /* Strip only the local mode selector; the shared microbench
                 * validates every remaining argument. */
                for (int j = i; j + 2 < argc; j++) argv[j] = argv[j + 2];
                argc -= 2; argv[argc] = NULL;
                return efs_bench_blake3_main(argc, argv);
            }
            return efs_bench_local_main(argc, argv);
        }
    }

    if (efs_help_requested(argc,argv)) { efs_cli_help(stdout,argv[0],NULL);return 0; }

    const char *seed = NULL;
    enum bench_mode mode = BENCH_MODE_NONE;
    double time_sec = 0.0;
    uint64_t size_bytes = 0;
    size_t io_size = 1u << 20;
    int have_id = 0;
    int want_perf = 0;
    int nworkers_arg = 0;
    int have_time = 0;
    const char *meta_export = "default";
    uint32_t meta_files = 5000;
    uint32_t meta_dirs = 64;
    uint32_t meta_phases = META_PHASE_ALL;
    int meta_keep = 0;
    uint64_t meta_id = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            char *end; errno = 0; const char *value = argv[++i];
            time_sec = strtod(value, &end);
            have_time = 1;
            if (errno || !*value || *end || !isfinite(time_sec) || time_sec <= 0.0) {
                fprintf(stderr, "Invalid --time\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size_bytes = efs_parse_quota(argv[++i]);
            if (!size_bytes) { fprintf(stderr, "Invalid --size\n"); return 1; }
        } else if (strcmp(argv[i], "--read") == 0) {
            if (mode == BENCH_MODE_META) return 1;
            mode = BENCH_MODE_READ;
        } else if (strcmp(argv[i], "--io-size") == 0 && i + 1 < argc) {
            uint64_t n = efs_parse_quota(argv[++i]);
            if (!n || n > (16u << 20)) { fprintf(stderr, "--io-size must be 1 byte to 16 MiB\n"); return 1; }
            io_size = (size_t)n;
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            char *end; errno = 0;
            const char *value = argv[++i];
            meta_id = strtoull(value, &end, 10);
            if (errno || !*value || *value == '-' || *end) return 1;
            have_id = 1;
        } else if (strcmp(argv[i], "--meta") == 0) {
            if (mode != BENCH_MODE_NONE && mode != BENCH_MODE_META) {
                fprintf(stderr, "--meta cannot combine with --read\n");
                return 1;
            }
            mode = BENCH_MODE_META;
        } else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
            meta_export = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) {
            meta_files = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (meta_files == 0) {
                fprintf(stderr, "Invalid --files\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dirs") == 0 && i + 1 < argc) {
            meta_dirs = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (meta_dirs == 0) {
                fprintf(stderr, "Invalid --dirs\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--phases") == 0 && i + 1 < argc) {
            meta_phases = parse_meta_phases(argv[++i]);
            if (meta_phases == 0) {
                fprintf(stderr, "Invalid --phases (mkdir,create,stat,getattr,"
                                "readdir,setattr,rename,unlink,all)\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--keep") == 0) {
            meta_keep = 1;
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            char *end; errno = 0; const char *value = argv[++i];
            long n = strtol(value, &end, 10);
            if (errno || !*value || *end || n < 1 || n > 64) {
                fprintf(stderr, "Invalid --workers\n");
                return 1;
            }
            nworkers_arg = (int)n;
        } else if (strcmp(argv[i], "--perf") == 0) {
            want_perf = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (!seed) {
            seed = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (mode == BENCH_MODE_NONE)
        mode = size_bytes ? BENCH_MODE_WRITE : have_time ? BENCH_MODE_NET : BENCH_MODE_NONE;
    if (!seed || mode == BENCH_MODE_NONE) {
        usage(argv[0]);
        return 1;
    }
    if (mode == BENCH_MODE_META) {
        return run_meta_bench(seed, meta_export, meta_files, meta_dirs,
                              size_bytes, meta_phases, meta_keep, meta_id,
                              nworkers_arg, want_perf);
    }
    if (mode == BENCH_MODE_WRITE || mode == BENCH_MODE_READ) {
        unsigned nw = nworkers_arg ? (unsigned)nworkers_arg : 1;
        if (nw > 64 || (size_bytes && size_bytes < nw) ||
            (mode == BENCH_MODE_READ && !have_id)) {
            fprintf(stderr, "published data needs 1..64 workers, nonempty files, and --read needs --id\n");
            return 1;
        }
        if (!have_id) meta_id = (uint64_t)getpid();
        return run_data_bench(seed, meta_export, mode == BENCH_MODE_READ,
                              size_bytes, io_size, nw, meta_id, time_sec, want_perf);
    }

    char host[64];
    uint16_t port = 0;
    if (parse_host_port(seed, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid seed address: %s\n", seed);
        return 1;
    }

    memset(&g_client, 0, sizeof(g_client));
    pthread_mutex_init(&g_client.lock, NULL);

    if (efs_client_discover_nodes(&g_client, host, port) != 0) {
        fprintf(stderr, "Failed to discover cluster from %s:%u\n", host, port);
        return 1;
    }

    printf("bench net seed=%s:%u nodes=%u conns_per_node=%d time_s=%.3f frag_bytes=%d\n",
           host, port, g_client.node_count, g_client.conn_pool_size, time_sec, EFS_FRAGMENT_SIZE);
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        printf("  node id=%u %s:%u\n", g_client.nodes[i].id, g_client.nodes[i].addr,
               g_client.nodes[i].port);
    }
    fflush(stdout);

    uint8_t *zero_frag = calloc(1, EFS_FRAGMENT_SIZE);
    if (!zero_frag)
        return 1;
    uint8_t checksum[EFS_HASH_SIZE];
    efs_hash_zero_fragment(checksum);

    char perf_path[512];
    perf_path[0] = '\0';
    if (want_perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "/tmp/efs-bench-perf-%d/perf.data",
                     (int)getpid());
        g_perf_pid = efs_start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf; continuing\n");
        else
            printf("perf recording -> %s\n", perf_path);
        fflush(stdout);
    }

    /* Sticky workers hold one conn per node; never exceed the pool. */
    int nworkers = nworkers_arg;
    if (nworkers < 1)
        nworkers = g_client.conn_pool_size;
    if (nworkers < 1)
        nworkers = 1;
    if (nworkers > 64)
        nworkers = 64;
    struct worker_arg *args = calloc((size_t)nworkers, sizeof(*args));
    pthread_t *tids = calloc((size_t)nworkers, sizeof(*tids));
    if (!args || !tids) {
        free(zero_frag);
        free(args);
        free(tids);
        return 1;
    }

    g_chunk_seq = 0;
    double t0 = now_sec();
    double deadline = have_time ? (t0 + time_sec) : 0.0;
    for (int i = 0; i < nworkers; i++) {
        args[i].id = i;
        args[i].deadline = deadline;
        args[i].zero_frag = zero_frag;
        memcpy(args[i].checksum, checksum, EFS_HASH_SIZE);
        if (pthread_create(&tids[i], NULL, worker_main, &args[i]) != 0) {
            args[i].chunks_fail++;
            tids[i] = 0;
        }
    }
    for (int i = 0; i < nworkers; i++) {
        if (tids[i])
            pthread_join(tids[i], NULL);
    }
    double wall = now_sec() - t0;
    if (wall < 1e-6)
        wall = 1e-6;

    uint64_t total = 0, ok = 0, fail = 0;
    uint64_t per_node[EFS_MAX_NODES];
    memset(per_node, 0, sizeof(per_node));
    for (int i = 0; i < nworkers; i++) {
        total += args[i].bytes;
        ok += args[i].chunks_ok;
        fail += args[i].chunks_fail;
        for (uint32_t n = 0; n < g_client.node_count && n < EFS_MAX_NODES; n++)
            per_node[n] += args[i].per_node_bytes[n];
    }

    double gib = (double)total / (1024.0 * 1024.0 * 1024.0);
    double gib_s = gib / wall;

    printf("per_server_bytes:");
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        printf(" id%u=%llu", id, (unsigned long long)per_node[i]);
    }
    printf("\n");

    printf("BENCH_OK kind=net wall_s=%.3f bytes=%llu GiB_s=%.3f workers=%d "
               "chunks_ok=%llu chunks_fail=%llu nodes=%u\n",
               wall, (unsigned long long)total, gib_s, nworkers,
               (unsigned long long)ok, (unsigned long long)fail,
               g_client.node_count);
    fflush(stdout);

    stop_perf_recorder();
    if (want_perf && perf_path[0]) {
        char report[600], hot[600], cmd[900];
        snprintf(report, sizeof(report), "%s.report.txt", perf_path);
        snprintf(hot, sizeof(hot), "%s.hotpath.txt", perf_path);
        snprintf(cmd, sizeof(cmd),
                 "perf report --stdio --no-children --percent-limit 0.4 -i '%s' "
                 "> '%s' 2>/dev/null",
                 perf_path, report);
        int report_status = system(cmd);
        if (report_status != 0)
            fprintf(stderr, "perf report command failed: status=%d\n", report_status);
        FILE *in = fopen(report, "r");
        FILE *out = fopen(hot, "w");
        if (out) {
            fprintf(out, "=== efs / blake3 / network hotspots ===\n");
            if (in) {
                char line[1024];
                int tops = 0;
                while (fgets(line, sizeof(line), in)) {
                    if (strstr(line, "blake3") || strstr(line, "efs_") ||
                        strstr(line, "memcpy") || strstr(line, "memmove") ||
                        strstr(line, "send") || strstr(line, "recv") ||
                        strstr(line, "hash") || strstr(line, "write") ||
                        strstr(line, "put_"))
                        fputs(line, out);
                }
                rewind(in);
                fprintf(out, "--- top symbols ---\n");
                while (fgets(line, sizeof(line), in) && tops < 25) {
                    const char *s = line;
                    while (*s == ' ' || *s == '\t')
                        s++;
                    if (s[0] >= '0' && s[0] <= '9' && strstr(s, "%")) {
                        fputs(line, out);
                        tops++;
                    }
                }
                fclose(in);
            }
            fclose(out);
        }
        printf("perf report: %s\nperf hotpath: %s\n", report, hot);
    }

    free(zero_frag);
    free(args);
    free(tids);
    efs_client_shutdown();
    return ok == 0 || fail != 0 ? 1 : 0;
}
