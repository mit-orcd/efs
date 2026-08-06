#include "efs/common.h"
#include "efs/network.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

struct efsd_server *g_server = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --node-id <id> --addr <addr> --port <port> "
            "--storage <path> [--quota <bytes>[T|G|M|K]] [--direct-io|--no-direct-io] "
            "[--join <host:port>] [--no-persist] [--perf]\n"
            "  --direct-io      O_DIRECT for fragment I/O (default on)\n"
            "  --no-direct-io   use the page cache for fragment I/O\n",
            prog);
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

static void *conn_thread(void *arg)
{
    int fd = (intptr_t)arg;
    /* Long recv timeout so clients can keep a pooled connection idle between
     * requests without the server tearing it down every few seconds. */
    efs_set_recv_timeout(fd, 60000);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    server_handle_conn(fd);
    close(fd);
    return NULL;
}

static void sigint_handler(int sig)
{
    (void)sig;
    /* Async-signal-safe only. Closing listen_fd unblocks accept() even when
     * the libc wrapper restarts interrupted syscalls (SA_RESTART). Stopping
     * perf (waitpid/sleep) happens in main after the accept loop exits. */
    if (g_server) {
        g_server->running = 0;
        if (g_server->listen_fd >= 0) {
            close(g_server->listen_fd);
            g_server->listen_fd = -1;
        }
    }
}

int main(int argc, char **argv)
{
    struct efsd_server server;
    memset(&server, 0, sizeof(server));
    server.persist_nodes = 1;
    server.direct_io = 1; /* default: O_DIRECT on flash-backed node storage */
    server.listen_fd = -1;
    g_server = &server;
    pthread_mutex_init(&server.lock, NULL);

    char *join_peer = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            server.id = (efs_node_id_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            strncpy(server.addr, argv[++i], sizeof(server.addr) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            server.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--storage") == 0 && i + 1 < argc) {
            strncpy(server.storage_path, argv[++i], sizeof(server.storage_path) - 1);
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

    if (server.id == 0 || server.port == 0 || server.storage_path[0] == '\0') {
        fprintf(stderr, "Missing required arguments\n");
        usage(argv[0]);
        return 1;
    }

    char perf_path[8192];
    perf_path[0] = '\0';
    if (server.perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "%s/log/perf.data", server.storage_path);
    }

    if (mkdir(server.storage_path, 0755) != 0 && errno != EEXIST) {
        perror("mkdir storage");
        return 1;
    }

    char subdir[8192];
    snprintf(subdir, sizeof(subdir), "%s/data", server.storage_path);
    mkdir_p(subdir);
    snprintf(subdir, sizeof(subdir), "%s/meta", server.storage_path);
    mkdir_p(subdir);
    snprintf(subdir, sizeof(subdir), "%s/log", server.storage_path);
    mkdir_p(subdir);

    server_migrate_old_layout(&server);

    server.nodes[0].id = server.id;
    strncpy(server.nodes[0].addr, server.addr, sizeof(server.nodes[0].addr) - 1);
    server.nodes[0].port = server.port;
    strncpy(server.nodes[0].storage_path, server.storage_path,
            sizeof(server.nodes[0].storage_path) - 1);
    server.nodes[0].quota = server.quota;
    server.nodes[0].used = server_compute_usage(server.storage_path);
    server.node_count = 1;

    server_load_exports(&server);
    server_load_nodes(&server);

    /* Ensure the local node entry matches the current command-line arguments. */
    server.nodes[0].id = server.id;
    strncpy(server.nodes[0].addr, server.addr, sizeof(server.nodes[0].addr) - 1);
    server.nodes[0].port = server.port;
    strncpy(server.nodes[0].storage_path, server.storage_path,
            sizeof(server.nodes[0].storage_path) - 1);
    server.nodes[0].quota = server.quota;
    server.nodes[0].used = server_compute_usage(server.storage_path);

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
            pthread_mutex_lock(&server.lock);
            server_save_nodes(&server);
            pthread_mutex_unlock(&server.lock);
        }
    } else if (server.node_count > 1) {
        if (server_rejoin_cluster(&server) != 0) {
            fprintf(stderr,
                    "Could not rejoin cluster from persisted peers; starting as standalone and retrying in background\n");
            server_start_rejoin(&server);
        } else {
            pthread_mutex_lock(&server.lock);
            server_save_nodes(&server);
            pthread_mutex_unlock(&server.lock);
        }
    } else {
        pthread_mutex_lock(&server.lock);
        server_save_nodes(&server);
        pthread_mutex_unlock(&server.lock);
    }

    /* Membership is known; reconstruct bulk metadata from 2+1 pages. */
    server_rebuild_fragmented_exports(&server);

    setlinebuf(stdout);
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
    server_start_heartbeat(&server);
    server_start_migration(&server);

    printf("efsd node %u listening on %s:%u, storage=%s, used=%llu, quota=%llu, direct_io=%s\n",
           server.id, server.addr, server.port, server.storage_path,
           (unsigned long long)server.nodes[0].used,
           (unsigned long long)server.quota,
           server.direct_io ? "on" : "off");
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
            if (errno != EINVAL)
                perror("accept");
            break;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread, (void *)(intptr_t)fd) != 0) {
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

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    pthread_timedjoin_np(server.migrate_tid, NULL, &ts);

    pthread_mutex_lock(&server.lock);
    if (server.export_meta_dirty) {
        for (uint32_t i = 0; i < server.export_count; i++)
            server_save_export(&server, &server.exports[i]);
        server.export_meta_dirty = 0;
    }
    pthread_mutex_unlock(&server.lock);

    pthread_mutex_destroy(&server.lock);
    for (uint32_t i = 0; i < server.export_count; i++)
        efs_export_free(&server.exports[i]);
    return 0;
}
