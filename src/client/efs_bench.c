#include "client_internal.h"
#include "efs/checksum.h"
#include "efs/common.h"
#include "efs/network.h"
#include "efs/placement.h"
#include "efs/protocol.h"
#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Dedicated export id for store-bench PUTs (auto-created on each server). */
#define EFS_BENCH_EXPORT_ID  ((efs_export_id_t)0xB7)

enum bench_mode {
    BENCH_MODE_NONE = 0,
    BENCH_MODE_NET,   /* --time: BENCH_PUT discard */
    BENCH_MODE_STORE, /* --size: real PUT_CHUNK store */
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s <seed_host:port> --time <seconds>\n"
            "      Discover cluster; hammer all servers with mount-like parallel\n"
            "      BENCH_PUT fanout (servers ACK and discard). Measures net ceiling.\n"
            "  %s <seed_host:port> --size <bytes|[K|M|G|T]>\n"
            "      Discover cluster; write that many logical bytes (chunk-aligned)\n"
            "      with real PUT_CHUNK so servers store fragments (2+1 EC fanout).\n"
            "  Optional: --perf  (perf record -g on this process; EFS_PERF_PATH)\n",
            prog, prog);
}

static pid_t g_perf_pid = -1;

static int mkdir_p(const char *path)
{
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static pid_t start_perf_recorder(pid_t target, const char *perf_path)
{
    char perf_dir[4096];
    strncpy(perf_dir, perf_path, sizeof(perf_dir) - 1);
    perf_dir[sizeof(perf_dir) - 1] = '\0';
    char *slash = strrchr(perf_dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(perf_dir);
    }
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)target);
        execlp("perf", "perf", "record", "-g", "-F", "999", "-p", pid_str, "-o",
               perf_path, NULL);
        _exit(1);
    }
    return pid;
}

static void stop_perf_recorder(void)
{
    if (g_perf_pid > 0) {
        kill(g_perf_pid, SIGTERM);
        for (int i = 0; i < 50; i++) {
            if (waitpid(g_perf_pid, NULL, WNOHANG) == g_perf_pid)
                break;
            usleep(100000);
        }
        kill(g_perf_pid, SIGKILL);
        waitpid(g_perf_pid, NULL, 0);
        g_perf_pid = -1;
    }
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_host_port(const char *s, char *host, size_t host_len, uint16_t *port)
{
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s)
        return -1;
    size_t n = (size_t)(colon - s);
    if (n >= host_len)
        return -1;
    memcpy(host, s, n);
    host[n] = '\0';
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535)
        return -1;
    *port = (uint16_t)p;
    return 0;
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
    tls_msg_size = sizeof(struct efs_msg_put_chunk) + frag_len;
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

/* sticky_fds[membership_index] holds a checked-out pool fd for the worker
 * lifetime. Pass NULL to use short-lived borrows.
 * Success = ≥2 acks (same 2+1 quorum as the FUSE client PUT path). */
static int put_chunk_fanout(efs_ino_t ino, uint32_t chunk_index,
                            const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                            const uint8_t *zero_frag,
                            const uint8_t checksum[EFS_HASH_SIZE],
                            int store,
                            uint64_t per_node_bytes[EFS_MAX_NODES],
                            int *sticky_fds)
{
    (void)zero_frag; /* templates stay zero-filled */
    uint8_t msg_type = store ? EFS_MSG_PUT_CHUNK : EFS_MSG_BENCH_PUT;
    uint8_t reply_type_want =
        store ? EFS_MSG_PUT_CHUNK_REPLY : EFS_MSG_BENCH_PUT_REPLY;
    uint8_t ok_status = store ? EFS_PUT_CHUNK_OK : EFS_BENCH_PUT_OK;
    uint32_t bench_frag_len = EFS_FRAGMENT_SIZE;

    if (ensure_tls_reqs(bench_frag_len) != 0)
        return EFS_ERR_NOMEM;

    int fds[EFS_NUM_FRAGMENTS];
    int idxs[EFS_NUM_FRAGMENTS];
    int pending[EFS_NUM_FRAGMENTS];
    int sticky_owned[EFS_NUM_FRAGMENTS];

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        fds[i] = -1;
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
        /* Skip peers in down-cooldown; drop any sticky fd so we do not
         * block on a full SO_SNDTIMEO against a dead socket. */
        if (efs_client_node_is_down(nid)) {
            if (sticky_fds && sticky_fds[idx] >= 0) {
                efs_client_conn_drop(nid, sticky_fds[idx]);
                sticky_fds[idx] = -1;
            }
            continue;
        }
        if (sticky_fds && sticky_fds[idx] >= 0) {
            struct pollfd p = { .fd = sticky_fds[idx], .events = POLLOUT };
            int pr = poll(&p, 1, 0);
            if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                efs_client_conn_drop(nid, sticky_fds[idx]);
                sticky_fds[idx] = -1;
                efs_client_node_note_fail(nid);
                continue;
            }
            /* Not writable yet: skip this peer for this chunk only. Keep
             * sticky fd — backpressure is not a hard failure. */
            if (pr == 0 || !(p.revents & POLLOUT))
                continue;
            fds[i] = sticky_fds[idx];
            sticky_owned[i] = 1;
        } else {
            fds[i] = efs_client_conn_get(nid);
            if (fds[i] < 0)
                continue;
            struct pollfd p = { .fd = fds[i], .events = POLLOUT };
            int pr = poll(&p, 1, 0);
            if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                efs_client_conn_drop(nid, fds[i]);
                efs_client_node_note_fail(nid);
                continue;
            }
            if (pr == 0 || !(p.revents & POLLOUT)) {
                efs_client_conn_release(nid, fds[i]);
                continue;
            }
            if (sticky_fds)
                sticky_fds[idx] = fds[i];
            sticky_owned[i] = sticky_fds ? 1 : 0;
        }
        struct efs_msg_put_chunk *req =
            (struct efs_msg_put_chunk *)(tls_msgs + (size_t)i * tls_msg_size);
        req->export_id = store ? EFS_BENCH_EXPORT_ID : 0;
        req->ino = ino;
        req->chunk_index = chunk_index;
        req->fragment_index = (uint32_t)i;
        req->data_len = bench_frag_len;
        memcpy(req->checksum, checksum, EFS_HASH_SIZE);
        if (zero_frag)
            memcpy((uint8_t *)req + sizeof(*req), zero_frag, bench_frag_len);
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] < 0)
            continue;
        struct efs_msg_put_chunk *req =
            (struct efs_msg_put_chunk *)(tls_msgs + (size_t)i * tls_msg_size);
        if (efs_send_msg(fds[i], msg_type, req, (uint32_t)tls_msg_size) != 0) {
            efs_client_conn_drop(nodes[i], fds[i]);
            efs_client_node_note_fail(nodes[i]);
            if (sticky_fds && idxs[i] >= 0)
                sticky_fds[idxs[i]] = -1;
            fds[i] = -1;
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
            efs_node_id_t nid = nodes[i];
            int idx = idxs[i];
            uint8_t reply_type = 0, status = 0;
            if ((pfds[p].revents & (POLLERR | POLLHUP)) ||
                efs_recv_u8_reply(fds[i], &reply_type, &status) != 0 ||
                reply_type != reply_type_want || status != ok_status) {
                efs_client_conn_drop(nid, fds[i]);
                efs_client_node_note_fail(nid);
                if (sticky_fds && idx >= 0)
                    sticky_fds[idx] = -1;
                fds[i] = -1;
                pending[i] = 0;
                continue;
            }
            acks++;
            efs_client_node_note_ok(nid);
            if (idx >= 0 && (uint32_t)idx < EFS_MAX_NODES)
                per_node_bytes[idx] += bench_frag_len;
            if (!sticky_owned[i])
                efs_client_conn_release(nid, fds[i]);
            /* sticky: keep fd checked out for the next chunk */
            fds[i] = -1;
            pending[i] = 0;
        }
    }

    /* Quorum met: drain ready replies so sticky fds stay usable; only drop
     * when the peer is behind (avoid reconnect storms). */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] < 0)
            continue;
        efs_node_id_t nid = nodes[i];
        int idx = idxs[i];
        if (acks >= 2) {
            struct pollfd p = { .fd = fds[i], .events = POLLIN };
            int pr = poll(&p, 1, 0);
            if (pr > 0 && (p.revents & POLLIN) &&
                !(p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                uint8_t reply_type = 0, status = 0;
                if (efs_recv_u8_reply(fds[i], &reply_type, &status) == 0 &&
                    reply_type == reply_type_want) {
                    if (status == ok_status && idx >= 0 &&
                        (uint32_t)idx < EFS_MAX_NODES)
                        per_node_bytes[idx] += bench_frag_len;
                    if (!sticky_owned[i])
                        efs_client_conn_release(nid, fds[i]);
                    fds[i] = -1;
                    pending[i] = 0;
                    continue;
                }
            }
            efs_client_conn_drop(nid, fds[i]);
            if (sticky_fds && idx >= 0)
                sticky_fds[idx] = -1;
        } else {
            efs_client_conn_drop(nid, fds[i]);
            efs_client_node_note_fail(nid);
            if (sticky_fds && idx >= 0)
                sticky_fds[idx] = -1;
        }
        fds[i] = -1;
        pending[i] = 0;
    }

    return (acks >= 2) ? EFS_OK : EFS_ERR_NO_QUORUM;
}

struct worker_arg {
    int id;
    int store;
    double deadline;      /* net mode */
    uint64_t target_chunks; /* store mode; 0 = unlimited/time */
    uint64_t bytes;       /* fragment bytes sent */
    uint64_t logical_bytes;
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
    /* One synthetic file inode for store mode so chunks are contiguous;
     * net mode still spreads via seq for placement churn. */
    efs_ino_t ino = ((efs_ino_t)0xBEEF << 32) | 1ULL;

    /* Sticky fds filled lazily; a down peer must not abort the whole worker. */
    int sticky[EFS_MAX_NODES];
    for (int i = 0; i < EFS_MAX_NODES; i++)
        sticky[i] = -1;

    for (;;) {
        if (a->store) {
            uint64_t seq = next_chunk();
            if (seq >= a->target_chunks)
                break;
            uint32_t chunk_index = (uint32_t)seq;
            efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
            efs_place_fragments(g_client.nodes, g_client.node_count, ino,
                                chunk_index, nodes);
            int rc = put_chunk_fanout(ino, chunk_index, nodes, a->zero_frag,
                                      a->checksum, 1, a->per_node_bytes, sticky);
            if (rc == EFS_OK) {
                a->chunks_ok++;
                a->bytes += (uint64_t)EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE;
                a->logical_bytes += EFS_CHUNK_SIZE;
            } else {
                a->chunks_fail++;
            }
        } else {
            if (now_sec() >= a->deadline)
                break;
            uint64_t seq = next_chunk();
            uint32_t chunk_index = (uint32_t)(seq & 0xffffffffu);
            efs_ino_t net_ino = ino + (seq >> 20);
            efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
            efs_place_fragments(g_client.nodes, g_client.node_count, net_ino,
                                chunk_index, nodes);
            int rc = put_chunk_fanout(net_ino, chunk_index, nodes, a->zero_frag,
                                      a->checksum, 0, a->per_node_bytes, sticky);
            if (rc == EFS_OK) {
                a->chunks_ok++;
                a->bytes += (uint64_t)EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE;
                a->logical_bytes += EFS_CHUNK_SIZE;
            } else {
                a->chunks_fail++;
            }
        }
    }

    for (uint32_t i = 0; i < g_client.node_count; i++) {
        if (sticky[i] >= 0) {
            efs_client_conn_release(g_client.nodes[i].id, sticky[i]);
            sticky[i] = -1;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    setlinebuf(stdout);
    setlinebuf(stderr);

    const char *seed = NULL;
    enum bench_mode mode = BENCH_MODE_NONE;
    double time_sec = 10.0;
    uint64_t size_bytes = 0;
    int want_perf = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            if (mode == BENCH_MODE_STORE) {
                fprintf(stderr, "Use either --time or --size, not both\n");
                return 1;
            }
            mode = BENCH_MODE_NET;
            time_sec = atof(argv[++i]);
            if (time_sec <= 0.0) {
                fprintf(stderr, "Invalid --time\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (mode == BENCH_MODE_NET) {
                fprintf(stderr, "Use either --time or --size, not both\n");
                return 1;
            }
            mode = BENCH_MODE_STORE;
            size_bytes = efs_parse_quota(argv[++i]);
            if (size_bytes == 0) {
                fprintf(stderr, "Invalid --size (examples: 1G, 512M, 1048576)\n");
                return 1;
            }
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

    if (!seed || mode == BENCH_MODE_NONE) {
        usage(argv[0]);
        return 1;
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

    uint64_t target_chunks = 0;
    if (mode == BENCH_MODE_STORE) {
        target_chunks = size_bytes / EFS_CHUNK_SIZE;
        if (target_chunks == 0)
            target_chunks = 1;
        size_bytes = target_chunks * EFS_CHUNK_SIZE;
        printf("bench store seed=%s:%u nodes=%u conns_per_node=%d "
               "logical_bytes=%llu chunks=%llu export_id=%u\n",
               host, port, g_client.node_count, g_client.conn_pool_size,
               (unsigned long long)size_bytes, (unsigned long long)target_chunks,
               (unsigned)EFS_BENCH_EXPORT_ID);
    } else {
        printf("bench net seed=%s:%u nodes=%u conns_per_node=%d time_s=%.3f "
               "frag_bytes=%d\n",
               host, port, g_client.node_count, g_client.conn_pool_size, time_sec,
               EFS_FRAGMENT_SIZE);
    }
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
        g_perf_pid = start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf; continuing\n");
        else
            printf("perf recording -> %s\n", perf_path);
        fflush(stdout);
    }

    /* Sticky workers hold one conn per node; never exceed the pool. */
    int nworkers = g_client.conn_pool_size;
    if (nworkers < 1)
        nworkers = 1;
    if (nworkers > 64)
        nworkers = 64;
    if (mode == BENCH_MODE_STORE && target_chunks < (uint64_t)nworkers &&
        target_chunks > 0)
        nworkers = (int)target_chunks;

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
    double deadline = t0 + time_sec;
    for (int i = 0; i < nworkers; i++) {
        args[i].id = i;
        args[i].store = (mode == BENCH_MODE_STORE);
        args[i].deadline = deadline;
        args[i].target_chunks = target_chunks;
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

    uint64_t total = 0, logical = 0, ok = 0, fail = 0;
    uint64_t per_node[EFS_MAX_NODES];
    memset(per_node, 0, sizeof(per_node));
    for (int i = 0; i < nworkers; i++) {
        total += args[i].bytes;
        logical += args[i].logical_bytes;
        ok += args[i].chunks_ok;
        fail += args[i].chunks_fail;
        for (uint32_t n = 0; n < g_client.node_count && n < EFS_MAX_NODES; n++)
            per_node[n] += args[i].per_node_bytes[n];
    }

    double gib = (double)total / (1024.0 * 1024.0 * 1024.0);
    double gib_s = gib / wall;
    double logical_gib = (double)logical / (1024.0 * 1024.0 * 1024.0);
    double logical_gib_s = logical_gib / wall;

    printf("per_server_bytes:");
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        printf(" id%u=%llu", id, (unsigned long long)per_node[i]);
    }
    printf("\n");

    if (mode == BENCH_MODE_STORE) {
        printf("BENCH_OK kind=store wall_s=%.3f logical_bytes=%llu "
               "logical_GiB_s=%.3f stored_bytes=%llu stored_GiB_s=%.3f "
               "workers=%d chunks_ok=%llu chunks_fail=%llu nodes=%u\n",
               wall, (unsigned long long)logical, logical_gib_s,
               (unsigned long long)total, gib_s, nworkers,
               (unsigned long long)ok, (unsigned long long)fail,
               g_client.node_count);
    } else {
        printf("BENCH_OK kind=net wall_s=%.3f bytes=%llu GiB_s=%.3f workers=%d "
               "chunks_ok=%llu chunks_fail=%llu nodes=%u\n",
               wall, (unsigned long long)total, gib_s, nworkers,
               (unsigned long long)ok, (unsigned long long)fail,
               g_client.node_count);
    }
    fflush(stdout);

    stop_perf_recorder();
    if (want_perf && perf_path[0]) {
        char report[600], hot[600], cmd[900];
        snprintf(report, sizeof(report), "%s.report.txt", perf_path);
        snprintf(hot, sizeof(hot), "%s.hotpath.txt", perf_path);
        snprintf(cmd, sizeof(cmd),
                 "perf report --stdio --no-children --percent-limit 0.4 -i '%s' "
                 "> '%s' 2>/dev/null || true",
                 perf_path, report);
        (void)system(cmd);
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
    return (ok == 0 || (mode == BENCH_MODE_STORE && fail != 0)) ? 1 : 0;
}
