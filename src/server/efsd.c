#include "efs/common.h"
#include "efs/network.h"
#include "bench_local.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

struct efsd_server *g_server = NULL;

/* Conn/writer stacks: hello_ack is heap-allocated now, so 1 MiB is ample and
 * avoids ~8 GiB of VA when 512 conn threads are live. */
#define EFSD_THREAD_STACK (1 * 1024 * 1024)

int efsd_pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    int rc;
    if (pthread_attr_init(&attr) != 0)
        return pthread_create(tid, NULL, fn, arg);
    if (pthread_attr_setstacksize(&attr, EFSD_THREAD_STACK) != 0) {
        pthread_attr_destroy(&attr);
        return pthread_create(tid, NULL, fn, arg);
    }
    rc = pthread_create(tid, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

/* Alternate stack so SIGSEGV from stack overflow can still report. */
static char g_efsd_altstack[256 * 1024];

static void efsd_fatal_signal(int sig)
{
    const char *name = "signal";
    if (sig == SIGSEGV)
        name = "SIGSEGV";
    else if (sig == SIGBUS)
        name = "SIGBUS";
    else if (sig == SIGABRT)
        name = "SIGABRT";
    else if (sig == SIGILL)
        name = "SIGILL";
    else if (sig == SIGFPE)
        name = "SIGFPE";
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "efsd: fatal %s (%d)\n", name, sig);
    if (n > 0)
        (void)write(STDERR_FILENO, buf, (size_t)n);
    {
        void *frames[64];
        int nf = backtrace(frames, 64);
        backtrace_symbols_fd(frames, nf, STDERR_FILENO);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void efsd_install_crash_handlers(void)
{
#ifdef __SANITIZE_ADDRESS__
    /* Let ASAN own fatal signals so we get a full report. */
    (void)g_efsd_altstack;
    (void)efsd_fatal_signal;
    return;
#endif
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_efsd_altstack;
    ss.ss_size = sizeof(g_efsd_altstack);
    if (sigaltstack(&ss, NULL) != 0)
        fprintf(stderr, "Warning: sigaltstack failed: %s\n", strerror(errno));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = efsd_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --node-id <id> --addr <addr> --port <port> "
            "--storage <path>[,path...] [--storage <path> ...] "
            "[--quota <bytes>[T|G|M|K]] [--direct-io|--no-direct-io] "
            "[--writers <n>] [--join <host:port>] [--no-persist] [--perf]\n"
            "   or: %s --bench <path> --time <seconds> "
            "[--writers <n>] [--direct-io|--no-direct-io]\n"
            "  --storage        1..%d paths (comma and/or repeated).\n"
            "                   Multiple paths: least-queue write placement\n"
            "                   (chunk_index %% N) across disks for parallelism.\n"
            "  --bench <path>   local disk saturate (fragment-sized writes); no cluster\n"
            "  --time <sec>     duration for --bench (default 10)\n"
            "  --direct-io      O_DIRECT for fragment I/O\n"
            "  --no-direct-io   use the page cache for fragment I/O (default)\n"
            "  --writers <n>    writer threads per storage path "
            "(default %d, 0 = inline)\n",
            prog, prog, EFS_MAX_STORAGE_PATHS, EFS_WRITERS_PER_PATH_DEFAULT);
}

static int add_storage_path(struct efsd_server *s, const char *path)
{
    if (!path || !*path)
        return -1;
    if (s->storage_path_count >= EFS_MAX_STORAGE_PATHS) {
        fprintf(stderr, "Too many --storage paths (max %d)\n", EFS_MAX_STORAGE_PATHS);
        return -1;
    }
    strncpy(s->storage_paths[s->storage_path_count], path,
            sizeof(s->storage_paths[0]) - 1);
    s->storage_paths[s->storage_path_count][sizeof(s->storage_paths[0]) - 1] = '\0';
    s->storage_path_count++;
    return 0;
}

static int parse_storage_arg(struct efsd_server *s, const char *arg)
{
    char buf[EFS_MAX_PATH * EFS_MAX_STORAGE_PATHS];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (!*tok)
            continue;
        if (add_storage_path(s, tok) != 0)
            return -1;
    }
    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[8192];
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

static pid_t g_perf_pid = -1;

static pid_t start_perf_recorder(pid_t target, const char *perf_path)
{
    char log_dir[8192];
    strncpy(log_dir, perf_path, sizeof(log_dir) - 1);
    log_dir[sizeof(log_dir) - 1] = '\0';
    char *slash = strrchr(log_dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(log_dir);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork perf");
        return -1;
    }
    if (pid == 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)target);
        /* -g uses frame pointers (binaries are built with
         * -fno-omit-frame-pointer) so perf report can show call stacks. */
        execlp("perf", "perf", "record", "-g", "-F", "999", "-p", pid_str, "-o",
               perf_path, NULL);
        perror("exec perf");
        _exit(1);
    }
    return pid;
}

static void stop_perf_recorder(void)
{
    if (g_perf_pid > 0) {
        kill(g_perf_pid, SIGTERM);
        /* Wait (up to ~5s) for perf to finalize and write perf.data. */
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

static int parse_host_port(const char *str, char *host, size_t host_len, uint16_t *port)
{
    const char *colon = strrchr(str, ':');
    if (!colon)
        return -1;
    size_t hlen = (size_t)(colon - str);
    if (hlen >= host_len)
        return -1;
    memcpy(host, str, hlen);
    host[hlen] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return 0;
}

/* Live handler threads (accept path). Used to cap concurrent conns. */
static volatile int g_live_conns;

static void *conn_thread(void *arg)
{
    int fd = (intptr_t)arg;
    /* Long recv timeout so clients can keep a pooled connection idle between
     * requests without the server tearing it down every few seconds. */
    efs_set_recv_timeout(fd, 60000);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    server_handle_conn(fd);
    close(fd);
    __sync_fetch_and_sub(&g_live_conns, 1);
    return NULL;
}

static void sigint_handler(int sig)
{
    (void)sig;
    /* Async-signal-safe only: flip the flag. Do not close(listen_fd) here —
     * racing accept()/other threads on a closed fd has segfaulted on Engaging.
     * Without SA_RESTART, accept() returns EINTR and the loop exits. */
    if (g_server)
        g_server->running = 0;
}

int main(int argc, char **argv)
{
    /* Install before any work — imagenet load has produced silent SIGSEGVs. */
    efsd_install_crash_handlers();

    struct efsd_server server;
    memset(&server, 0, sizeof(server));
    server.persist_nodes = 1;
    server.direct_io = 0; /* default: page cache; opt in with --direct-io */
    server.nwriters = EFS_WRITERS_PER_PATH_DEFAULT;
    server.listen_fd = -1;
    g_server = &server;
    pthread_mutex_init(&server.lock, NULL);
    pthread_cond_init(&server.export_idle_cv, NULL);
    server_peer_pool_init();

    char *join_peer = NULL;
    char *bench_path = NULL;
    double bench_time = 10.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            server.id = (efs_node_id_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            strncpy(server.addr, argv[++i], sizeof(server.addr) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            server.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--storage") == 0 && i + 1 < argc) {
            if (parse_storage_arg(&server, argv[++i]) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            bench_path = argv[++i];
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            bench_time = atof(argv[++i]);
            if (bench_time <= 0.0) {
                fprintf(stderr, "Invalid --time value\n");
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc) {
            server.quota = efs_parse_quota(argv[++i]);
            if (server.quota == 0) {
                fprintf(stderr, "Invalid --quota value\n");
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--direct-io") == 0) {
            server.direct_io = 1;
        } else if (strcmp(argv[i], "--no-direct-io") == 0) {
            server.direct_io = 0;
        } else if (strcmp(argv[i], "--writers") == 0 && i + 1 < argc) {
            int n = atoi(argv[++i]);
            if (n < 0 || n > EFS_MAX_WRITERS_PER_PATH) {
                fprintf(stderr, "Invalid --writers value (0..%d per path)\n",
                        EFS_MAX_WRITERS_PER_PATH);
                usage(argv[0]);
                return 1;
            }
            server.nwriters = n;
        } else if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) {
            join_peer = argv[++i];
        } else if (strcmp(argv[i], "--no-persist") == 0) {
            server.persist_nodes = 0;
        } else if (strcmp(argv[i], "--perf") == 0) {
            server.perf = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (bench_path) {
        int writers = server.nwriters > 0 ? server.nwriters
                                         : EFS_WRITERS_PER_PATH_DEFAULT;
        return server_run_local_bench(bench_path, bench_time, writers,
                                      server.direct_io);
    }

    if (server.id == 0 || server.port == 0 || server.storage_path_count == 0) {
        fprintf(stderr, "Missing required arguments\n");
        usage(argv[0]);
        return 1;
    }
    strncpy(server.storage_path, server.storage_paths[0], sizeof(server.storage_path) - 1);
    server.storage_path[sizeof(server.storage_path) - 1] = '\0';

    /* Line-buffer early so join/listen banners show up in harness logs promptly. */
    setlinebuf(stdout);
    setlinebuf(stderr);

    char perf_path[8192];
    perf_path[0] = '\0';
    if (server.perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "%s/log/perf.data", server.storage_path);
    }

    for (uint32_t pi = 0; pi < server.storage_path_count; pi++) {
        if (mkdir_p(server.storage_paths[pi]) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir storage %s: %s\n", server.storage_paths[pi],
                    strerror(errno));
            return 1;
        }
        char subdir[8192];
        snprintf(subdir, sizeof(subdir), "%s/data", server.storage_paths[pi]);
        mkdir_p(subdir);
        snprintf(subdir, sizeof(subdir), "%s/meta", server.storage_paths[pi]);
        mkdir_p(subdir);
        snprintf(subdir, sizeof(subdir), "%s/log", server.storage_paths[pi]);
        mkdir_p(subdir);
    }

    server_migrate_old_layout(&server);

    server.nodes[0].id = server.id;
    strncpy(server.nodes[0].addr, server.addr, sizeof(server.nodes[0].addr) - 1);
    server.nodes[0].port = server.port;
    server_format_storage_paths(&server, server.nodes[0].storage_path,
                                sizeof(server.nodes[0].storage_path));
    server.nodes[0].quota = server.quota;
    server.nodes[0].used = 0;
    server.node_count = 1;

    server_load_exports(&server);
    server_load_nodes(&server);

    /* Refresh this process's row by id — never smash nodes[0], which may be a peer
     * after load (that bug duplicated one id and dropped another). */
    server_sync_local_membership(&server);
    /* Prefer meta/usage.bin; fall back to one full data/ scan. */
    server_init_local_usage(&server);

    /* Start listening first. If the configured address/port cannot be bound,
       fail immediately before contacting peers. */
    server.listen_fd = efs_listen_tcp(server.addr, server.port, EFS_LISTEN_BACKLOG);
    if (server.listen_fd < 0) {
        fprintf(stderr, "Failed to listen on %s:%u: %s\n",
                server.addr, server.port, strerror(errno));
        return 1;
    }

    if (join_peer) {
        char host[64];
        uint16_t port;
        if (parse_host_port(join_peer, host, sizeof(host), &port) != 0) {
            fprintf(stderr, "Invalid join address: %s\n", join_peer);
            close(server.listen_fd);
            return 1;
        }
        if (server_join_cluster(&server, host, port) != 0) {
            fprintf(stderr,
                    "Could not join cluster at %s; starting as standalone and retrying in background\n",
                    join_peer);
            strncpy(server.rejoin_addr, host, sizeof(server.rejoin_addr) - 1);
            server.rejoin_addr[sizeof(server.rejoin_addr) - 1] = '\0';
            server.rejoin_port = port;
            server_start_rejoin(&server);
        } else {
            server_fetch_metadata_from(&server, host, port);
            server_save_nodes(&server);
        }
    } else if (server.node_count > 1) {
        if (server_rejoin_cluster(&server) != 0) {
            fprintf(stderr,
                    "Could not rejoin cluster from persisted peers; starting as standalone and retrying in background\n");
            server_start_rejoin(&server);
        } else {
            /* Refresh export root + bulk pages from a live peer when possible. */
            for (uint32_t i = 0; i < server.node_count; i++) {
                if (server.nodes[i].id == server.id)
                    continue;
                if (server_fetch_metadata_from(&server, server.nodes[i].addr,
                                               server.nodes[i].port) == 0)
                    break;
            }
            server_save_nodes(&server);
        }
    } else {
        server_save_nodes(&server);
    }

    /* Membership is known; reconstruct bulk metadata from 2+1 pages. */
    server_rebuild_fragmented_exports(&server);

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        /* Do not set SA_RESTART: we want accept() to surface EINTR as a
         * fallback if close(listen_fd) races with the syscall entry. */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    server.running = 1;
    if (server_writer_pool_start(&server) != 0) {
        fprintf(stderr, "Failed to start writer pools (%d per path × %u paths)\n",
                server.nwriters, server.storage_path_count);
        return 1;
    }
    server_start_heartbeat(&server);
    server_start_migration(&server);
    server_start_meta_catchup(&server);

    {
        const char *stripe = (server.storage_path_count > 1) ? "leastq" : "none";
        char storage_disp[EFS_MAX_PATH];
        uint64_t used_disp = 0;
        int total_writers = server.nwriters * (int)server.storage_path_count;
        server_format_storage_paths(&server, storage_disp, sizeof(storage_disp));
        struct efs_node *local = server_local_node(&server);
        if (local)
            used_disp = local->used;
        printf("efsd node %u listening on %s:%u, storage=%s (%u paths, stripe=%s), "
               "used=%llu, quota=%llu, direct_io=%s, writers=%d/path (%d total), "
               "build=%s\n",
               server.id, server.addr, server.port, storage_disp,
               server.storage_path_count, stripe,
               (unsigned long long)used_disp,
               (unsigned long long)server.quota,
               server.direct_io ? "on" : "off",
               server.nwriters, total_writers, EFS_BUILD_ID);
    }
    fflush(stdout);

    if (server.perf) {
        g_perf_pid = start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0) {
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        }
    }

    while (server.running) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(server.listen_fd, (struct sockaddr *)&addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            /* EMFILE/ENFILE: do not tear down the accept loop — back off so
             * existing handlers can finish and free fds, then resume. */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                fprintf(stderr,
                        "accept: %s (live_conns=%d); backing off\n",
                        strerror(errno), g_live_conns);
                usleep(10 * 1000);
                continue;
            }
            if (errno != EINVAL)
                perror("accept");
            break;
        }

        int live = __sync_add_and_fetch(&g_live_conns, 1);
        if (live > EFS_SERVER_MAX_CONNS) {
            __sync_fetch_and_sub(&g_live_conns, 1);
            close(fd);
            /* Over-cap: back off like EMFILE so a conn storm doesn't spin. */
            usleep(10 * 1000);
            continue;
        }

        pthread_t tid;
        if (efsd_pthread_create(&tid, conn_thread, (void *)(intptr_t)fd) != 0) {
            __sync_fetch_and_sub(&g_live_conns, 1);
            close(fd);
        } else {
            pthread_detach(tid);
        }
    }

    if (server.listen_fd >= 0) {
        close(server.listen_fd);
        server.listen_fd = -1;
    }
    stop_perf_recorder();

    /* Drain live conn threads before tearing down server state. Detached
     * handlers use g_server (lock, exports, peer pool); destroying it while
     * they run is a UAF. Wait with a bounded timeout. */
    for (int waited_ms = 0; g_live_conns > 0 && waited_ms < 15000; ) {
        usleep(50 * 1000);
        waited_ms += 50;
    }
    if (g_live_conns > 0)
        fprintf(stderr, "shutdown: %d conn threads still live after drain "
                "timeout; forcing teardown\n", g_live_conns);

    server_writer_pool_stop(&server);
    server_usage_flush_dirty(&server);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    pthread_timedjoin_np(server.migrate_tid, NULL, &ts);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    pthread_timedjoin_np(server.meta_catchup_tid, NULL, &ts);

    pthread_mutex_lock(&server.lock);
    if (server.export_meta_dirty) {
        for (uint32_t i = 0; i < server.export_count; i++)
            server_save_export(&server, &server.exports[i]);
        server.export_meta_dirty = 0;
    }
    pthread_mutex_unlock(&server.lock);

    server_peer_pool_shutdown();
    pthread_mutex_destroy(&server.lock);
    for (uint32_t i = 0; i < server.export_count; i++)
        efs_export_free(&server.exports[i]);
    return 0;
}
