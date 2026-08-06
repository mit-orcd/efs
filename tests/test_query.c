#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "src/client/client_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define TEST_DIR "/tmp/efs_query_test"
#define NODE_COUNT 3

struct child_proc {
    pid_t pid;
    char storage[256];
    char log[256];
    char host[64];
    uint16_t port;
};

static struct child_proc children[NODE_COUNT];

static int wait_for_server(const char *host, uint16_t port, int timeout_ms)
{
    int fd;
    for (int t = 0; t < timeout_ms; t += 100) {
        fd = efs_connect_tcp(host, port);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        usleep(100 * 1000);
    }
    return -1;
}

static int start_servers(void)
{
    system("rm -rf " TEST_DIR "; mkdir -p " TEST_DIR);

    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(children[i].storage, sizeof(children[i].storage), TEST_DIR "/s%d", i + 1);
        snprintf(children[i].log, sizeof(children[i].log), TEST_DIR "/s%d.log", i + 1);
        snprintf(children[i].host, sizeof(children[i].host), "127.0.0.1");
        children[i].port = 18492 + i;
        mkdir(children[i].storage, 0755);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            int logfd = open(children[i].log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (logfd >= 0) {
                dup2(logfd, STDOUT_FILENO);
                dup2(logfd, STDERR_FILENO);
                close(logfd);
            }
            char node_id[8], port_str[8];
            snprintf(node_id, sizeof(node_id), "%d", i + 1);
            snprintf(port_str, sizeof(port_str), "%u", children[i].port);
            execl("./efsd", "efsd",
                  "--node-id", node_id,
                  "--addr", children[i].host,
                  "--port", port_str,
                  "--storage", children[i].storage,
                  NULL);
            perror("execl");
            _exit(1);
        }
        children[i].pid = pid;
    }

    for (int i = 0; i < NODE_COUNT; i++) {
        if (wait_for_server(children[i].host, children[i].port, 3000) != 0) {
            fprintf(stderr, "Server %d did not start\n", i + 1);
            return -1;
        }
        if (kill(children[i].pid, 0) != 0) {
            fprintf(stderr, "Server %d process died unexpectedly\n", i + 1);
            return -1;
        }
    }
    return 0;
}

static void stop_servers(void)
{
    for (int i = 0; i < NODE_COUNT; i++) {
        if (children[i].pid > 0)
            kill(children[i].pid, SIGTERM);
    }
    for (int i = 0; i < NODE_COUNT; i++) {
        if (children[i].pid > 0) {
            int status;
            for (int t = 0; t < 50; t++) {
                if (waitpid(children[i].pid, &status, WNOHANG) != 0)
                    break;
                usleep(100 * 1000);
            }
            kill(children[i].pid, SIGKILL);
            waitpid(children[i].pid, NULL, 0);
            children[i].pid = 0;
        }
    }
}

static int join_cluster(void)
{
    for (int i = 1; i < NODE_COUNT; i++) {
        int fd = efs_connect_tcp(children[0].host, children[0].port);
        if (fd < 0)
            return -1;
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

        struct efs_msg_join req;
        memset(&req, 0, sizeof(req));
        strncpy(req.peer_host, children[i].host, sizeof(req.peer_host) - 1);
        req.peer_port = children[i].port;

        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_send_msg(fd, EFS_MSG_JOIN, &req, sizeof(req)) != 0 ||
            efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
            type != EFS_MSG_JOIN_REPLY || reply_len < 1 ||
            ((uint8_t *)reply)[0] != EFS_JOIN_OK) {
            free(reply);
            close(fd);
            return -1;
        }
        free(reply);
        close(fd);
        usleep(200 * 1000);
    }
    return 0;
}

static int create_export(void)
{
    int fd = efs_connect_tcp(children[0].host, children[0].port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_create_export req;
    memset(&req, 0, sizeof(req));
    strcpy(req.name, "fs");

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_CREATE_EXPORT, &req, sizeof(req)) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_CREATE_EXPORT_REPLY || reply_len < 1 ||
        ((uint8_t *)reply)[0] != EFS_CREATE_EXPORT_OK) {
        free(reply);
        close(fd);
        return -1;
    }
    free(reply);
    close(fd);
    return 0;
}

static int setup_client(void)
{
    g_client.export_id = 1;
    strcpy(g_client.export_name, "fs");
    g_client.node_count = NODE_COUNT;
    for (int i = 0; i < NODE_COUNT; i++) {
        g_client.nodes[i].id = i + 1;
        strncpy(g_client.nodes[i].addr, children[i].host, sizeof(g_client.nodes[i].addr) - 1);
        g_client.nodes[i].port = children[i].port;
    }
    efs_export_init(&g_client.export, 1, "fs");
    return efs_client_fetch_metadata(children[0].host, children[0].port);
}

static int query_and_check(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "./efs-query --raw %s:%u", children[0].host, children[0].port);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[256];
    int total_files = -1;
    long long total_bytes = -1;
    int users = -1;
    int uid1000_files = -1;
    long long uid1000_bytes = -1;
    int uid2000_files = -1;
    long long uid2000_bytes = -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned int uid;
        unsigned long long files, bytes;
        if (sscanf(line, "total_files %llu", &files) == 1)
            total_files = (int)files;
        else if (sscanf(line, "total_bytes %llu", &bytes) == 1)
            total_bytes = (long long)bytes;
        else if (sscanf(line, "users %u", &uid) == 1)
            users = (int)uid;
        else if (sscanf(line, "user %u %llu %llu", &uid, &files, &bytes) == 3) {
            if (uid == 1000) {
                uid1000_files = (int)files;
                uid1000_bytes = (long long)bytes;
            } else if (uid == 2000) {
                uid2000_files = (int)files;
                uid2000_bytes = (long long)bytes;
            }
        }
    }
    int rc = pclose(fp);
    if (rc != 0) {
        fprintf(stderr, "efs-query exited with status %d\n", rc);
        return -1;
    }

    if (total_files != 2 || users != 2) {
        fprintf(stderr, "Unexpected totals: files=%d users=%d\n", total_files, users);
        return -1;
    }
    if (total_bytes != 4096 + 8192) {
        fprintf(stderr, "Unexpected total bytes: %lld\n", total_bytes);
        return -1;
    }
    if (uid1000_files != 1 || uid1000_bytes != 4096) {
        fprintf(stderr, "Unexpected uid 1000 stats: files=%d bytes=%lld\n", uid1000_files, uid1000_bytes);
        return -1;
    }
    if (uid2000_files != 1 || uid2000_bytes != 8192) {
        fprintf(stderr, "Unexpected uid 2000 stats: files=%d bytes=%lld\n", uid2000_files, uid2000_bytes);
        return -1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;

    if (start_servers() != 0) {
        fprintf(stderr, "Failed to start servers\n");
        stop_servers();
        return 1;
    }
    printf("Servers started\n");

    if (join_cluster() != 0) {
        fprintf(stderr, "Failed to form cluster\n");
        failures++;
        goto cleanup;
    }
    printf("Cluster formed\n");

    if (create_export() != 0) {
        fprintf(stderr, "Failed to create export\n");
        failures++;
        goto cleanup;
    }
    printf("Export created\n");

    if (setup_client() != 0) {
        fprintf(stderr, "Failed to fetch metadata\n");
        failures++;
        goto cleanup;
    }
    printf("Metadata fetched\n");

    efs_ino_t f1 = efs_client_create(EFS_ROOT_INO, "a.bin", S_IFREG | 0644, 1000, 1000);
    efs_ino_t f2 = efs_client_create(EFS_ROOT_INO, "b.bin", S_IFREG | 0644, 2000, 2000);
    if (f1 == 0 || f2 == 0) {
        fprintf(stderr, "Failed to create files\n");
        failures++;
        goto cleanup;
    }

    char *buf1 = calloc(4096, 1);
    char *buf2 = calloc(8192, 1);
    if (efs_client_write(f1, 0, 4096, buf1) != 0 ||
        efs_client_write(f2, 0, 8192, buf2) != 0) {
        fprintf(stderr, "Failed to write files\n");
        failures++;
        free(buf1);
        free(buf2);
        goto cleanup;
    }
    free(buf1);
    free(buf2);
    printf("Files written\n");

    if (query_and_check() != 0) {
        fprintf(stderr, "FAIL: query check failed\n");
        failures++;
    } else {
        printf("Query result OK\n");
    }

cleanup:
    stop_servers();
    if (failures == 0)
        printf("test_query: OK\n");
    else
        printf("test_query: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
