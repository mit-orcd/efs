#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include "src/client/client_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#define TEST_DIR "/tmp/efs_migrate_test"
#define NODE_COUNT 3

struct child_proc {
    pid_t pid;
    char storage[256];
    char log[256];
    char host[64];
    uint16_t port;
};

struct child_proc children[NODE_COUNT];

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

static int start_servers(uint64_t quota, int base_port)
{
    system("rm -rf " TEST_DIR "; mkdir -p " TEST_DIR);

    char quota_str[32];
    snprintf(quota_str, sizeof(quota_str), "%llu", (unsigned long long)quota);

    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(children[i].storage, sizeof(children[i].storage), TEST_DIR "/s%d", i + 1);
        snprintf(children[i].log, sizeof(children[i].log), TEST_DIR "/s%d.log", i + 1);
        snprintf(children[i].host, sizeof(children[i].host), "127.0.0.1");
        children[i].port = base_port + i;
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
                  "--quota", quota_str,
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
        int fd = efs_connect_tcp(children[i].host, children[i].port);
        if (fd < 0)
            return -1;
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

        struct efs_msg_join req;
        memset(&req, 0, sizeof(req));
        strncpy(req.peer_host, children[0].host, sizeof(req.peer_host) - 1);
        req.peer_port = children[0].port;

        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_send_msg(fd, EFS_MSG_JOIN, &req, sizeof(req)) != 0 ||
            efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
            type != EFS_MSG_JOIN_REPLY || reply_len != 1) {
            free(reply);
            close(fd);
            return -1;
        }
        uint8_t *status = reply;
        int ok = (*status == EFS_JOIN_OK);
        free(reply);
        close(fd);
        if (!ok)
            return -1;
    }
    return 0;
}

static int create_export(const char *name)
{
    int fd = efs_connect_tcp(children[0].host, children[0].port);
    if (fd < 0)
        return -1;

    struct efs_msg_create_export req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, name, EFS_MAX_NAME - 1);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_CREATE_EXPORT, &req, sizeof(req)) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_CREATE_EXPORT_REPLY || reply_len != 1) {
        free(reply);
        close(fd);
        return -1;
    }
    int ok = (*((uint8_t *)reply) == EFS_CREATE_EXPORT_OK);
    free(reply);
    close(fd);
    return ok ? 0 : -1;
}

static int init_client(const char *name, const char *node_addrs[EFS_MAX_NODES])
{
    efs_client_init_nodes(&g_client, node_addrs, NODE_COUNT);
    g_client.export_id = 1;
    strncpy(g_client.export_name, name, EFS_MAX_NAME - 1);
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    return efs_client_fetch_metadata(children[0].host, children[0].port);
}

static int read_file(efs_ino_t ino, size_t expected_len, char *buf)
{
    size_t got = 0;
    int rc = efs_client_read(ino, 0, expected_len, buf, &got);
    if (rc != 0 || got != expected_len)
        return -1;
    return 0;
}

static uint64_t get_node_usage(int server_idx)
{
    int fd = efs_connect_tcp(children[server_idx].host, children[server_idx].port);
    if (fd < 0)
        return UINT64_MAX;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_STATUS, NULL, 0) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_STATUS_REPLY ||
        reply_len != sizeof(struct efs_msg_status_reply)) {
        free(reply);
        close(fd);
        return UINT64_MAX;
    }
    struct efs_msg_status_reply *r = reply;
    uint64_t used = r->used;
    free(reply);
    close(fd);
    return used;
}

static int wait_for_usage(int server_idx, uint64_t max_usage, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 200) {
        uint64_t used = get_node_usage(server_idx);
        if (used != UINT64_MAX && used <= max_usage)
            return 0;
        usleep(200 * 1000);
    }
    return -1;
}

static int send_remove_node(int server_idx)
{
    int fd = efs_connect_tcp(children[server_idx].host, children[server_idx].port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_REMOVE_NODE, NULL, 0) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_REMOVE_NODE_REPLY || reply_len != 1) {
        free(reply);
        close(fd);
        return -1;
    }
    uint8_t *status = reply;
    int ok = (*status == EFS_REMOVE_NODE_IN_PROGRESS);
    free(reply);
    close(fd);
    return ok ? 0 : -1;
}

static int send_shrink_quota(int server_idx, uint64_t amount)
{
    int fd = efs_connect_tcp(children[server_idx].host, children[server_idx].port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_shrink_quota req;
    memset(&req, 0, sizeof(req));
    req.amount = amount;

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_SHRINK_QUOTA, &req, sizeof(req)) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_SHRINK_QUOTA_REPLY || reply_len != 1) {
        free(reply);
        close(fd);
        return -1;
    }
    uint8_t *status = reply;
    int ok = (*status == EFS_SHRINK_QUOTA_IN_PROGRESS);
    free(reply);
    close(fd);
    return ok ? 0 : -1;
}

static int cluster_node_count(void)
{
    int fd = efs_connect_tcp(children[0].host, children[0].port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_LIST_NODES, NULL, 0) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_LIST_NODES_REPLY ||
        reply_len != sizeof(struct efs_msg_list_nodes_reply)) {
        free(reply);
        close(fd);
        return -1;
    }
    struct efs_msg_list_nodes_reply *list = reply;
    int count = (int)list->node_count;
    free(reply);
    close(fd);
    return count;
}

static void print_logs(void)
{
    for (int i = 0; i < NODE_COUNT; i++) {
        printf("--- server %d log ---\n", i + 1);
        FILE *f = fopen(children[i].log, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f))
                printf("%s", line);
            fclose(f);
        }
    }
}

static int test_remove_node(void)
{
    int failures = 0;

    if (start_servers(256 * 1024, 18452) != 0) {
        fprintf(stderr, "Failed to start servers\n");
        stop_servers();
        return 1;
    }
    printf("[remove-node] Servers started\n");

    if (join_cluster() != 0) {
        fprintf(stderr, "[remove-node] Failed to form cluster\n");
        failures++;
        goto cleanup;
    }
    printf("[remove-node] Cluster formed\n");

    if (create_export("rem") != 0) {
        fprintf(stderr, "[remove-node] Failed to create export\n");
        failures++;
        goto cleanup;
    }

    const char *node_addrs[EFS_MAX_NODES] = {
        "127.0.0.1:18452",
        "127.0.0.1:18453",
        "127.0.0.1:18454"
    };
    if (init_client("rem", node_addrs) != 0) {
        fprintf(stderr, "[remove-node] Failed to fetch metadata\n");
        failures++;
        goto cleanup;
    }

    efs_ino_t fno = efs_client_create(EFS_ROOT_INO, "data.bin", S_IFREG | 0644, 0, 0);
    if (fno == 0) {
        fprintf(stderr, "[remove-node] Failed to create file\n");
        failures++;
        goto cleanup;
    }

    size_t len = 128 * 1024;
    char *data = malloc(len);
    for (size_t i = 0; i < len; i++)
        data[i] = (char)(i % 251);
    if (efs_client_write(fno, 0, len, data) != 0) {
        fprintf(stderr, "[remove-node] Failed to write file\n");
        failures++;
        free(data);
        goto cleanup;
    }
    printf("[remove-node] Wrote %zu bytes\n", len);
    free(data);

    char *read_buf = malloc(len);
    if (read_file(fno, len, read_buf) != 0) {
        fprintf(stderr, "[remove-node] Failed to read file before removal\n");
        failures++;
        free(read_buf);
        goto cleanup;
    }
    free(read_buf);
    printf("[remove-node] Read back OK before removal\n");

    if (send_remove_node(2) != 0) { /* remove server 3 (index 2) */
        fprintf(stderr, "[remove-node] Failed to request removal\n");
        failures++;
        goto cleanup;
    }
    printf("[remove-node] Removal requested\n");

    /* Wait for server 3 to exit. */
    int status;
    for (int t = 0; t < 200; t++) {
        if (waitpid(children[2].pid, &status, WNOHANG) != 0)
            break;
        usleep(100 * 1000);
    }
    if (kill(children[2].pid, 0) == 0) {
        fprintf(stderr, "[remove-node] Server 3 did not exit in time\n");
        failures++;
        kill(children[2].pid, SIGKILL);
        waitpid(children[2].pid, NULL, 0);
        goto cleanup;
    }
    children[2].pid = 0;
    printf("[remove-node] Server 3 exited\n");

    /* Wait for remaining nodes to update their cluster list. */
    sleep(1);

    int count = cluster_node_count();
    if (count != 2) {
        fprintf(stderr, "[remove-node] Expected cluster size 2, got %d\n", count);
        failures++;
    } else {
        printf("[remove-node] Cluster size is 2\n");
    }

    read_buf = malloc(len);
    if (read_file(fno, len, read_buf) != 0) {
        fprintf(stderr, "[remove-node] Failed to read file after removal\n");
        failures++;
    } else {
        printf("[remove-node] Read back OK after removal\n");
    }
    free(read_buf);

cleanup:
    efs_export_free(&g_client.export);
    stop_servers();
    print_logs();
    system("rm -rf " TEST_DIR);
    return failures;
}

static int test_shrink_quota(void)
{
    int failures = 0;

    if (start_servers(256 * 1024, 18462) != 0) {
        fprintf(stderr, "[shrink-quota] Failed to start servers\n");
        stop_servers();
        return 1;
    }
    printf("[shrink-quota] Servers started\n");

    if (join_cluster() != 0) {
        fprintf(stderr, "[shrink-quota] Failed to form cluster\n");
        failures++;
        goto cleanup;
    }
    printf("[shrink-quota] Cluster formed\n");

    if (create_export("shrink") != 0) {
        fprintf(stderr, "[shrink-quota] Failed to create export\n");
        failures++;
        goto cleanup;
    }

    const char *node_addrs[EFS_MAX_NODES] = {
        "127.0.0.1:18462",
        "127.0.0.1:18463",
        "127.0.0.1:18464"
    };
    if (init_client("shrink", node_addrs) != 0) {
        fprintf(stderr, "[shrink-quota] Failed to fetch metadata\n");
        failures++;
        goto cleanup;
    }

    efs_ino_t fno = efs_client_create(EFS_ROOT_INO, "big.bin", S_IFREG | 0644, 0, 0);
    if (fno == 0) {
        fprintf(stderr, "[shrink-quota] Failed to create file\n");
        failures++;
        goto cleanup;
    }

    size_t len = 384 * 1024;
    char *data = malloc(len);
    for (size_t i = 0; i < len; i++)
        data[i] = (char)(i % 257);
    if (efs_client_write(fno, 0, len, data) != 0) {
        fprintf(stderr, "[shrink-quota] Failed to write file\n");
        failures++;
        free(data);
        goto cleanup;
    }
    printf("[shrink-quota] Wrote %zu bytes\n", len);
    free(data);

    char *read_buf = malloc(len);
    if (read_file(fno, len, read_buf) != 0) {
        fprintf(stderr, "[shrink-quota] Failed to read file before shrink\n");
        failures++;
        free(read_buf);
        goto cleanup;
    }
    free(read_buf);
    printf("[shrink-quota] Read back OK before shrink\n");

    /* Shrink server 1 by 128 KiB: new quota is 128 KiB. It currently holds 192 KiB. */
    if (send_shrink_quota(0, 128 * 1024) != 0) {
        fprintf(stderr, "[shrink-quota] Failed to request shrink\n");
        failures++;
        goto cleanup;
    }
    printf("[shrink-quota] Shrink requested\n");

    if (wait_for_usage(0, 128 * 1024, 10000) != 0) {
        fprintf(stderr, "[shrink-quota] Server 1 usage did not drop to target\n");
        failures++;
        goto cleanup;
    }
    printf("[shrink-quota] Server 1 usage dropped to target\n");

    read_buf = malloc(len);
    if (read_file(fno, len, read_buf) != 0) {
        fprintf(stderr, "[shrink-quota] Failed to read file after shrink\n");
        failures++;
    } else {
        printf("[shrink-quota] Read back OK after shrink\n");
    }
    free(read_buf);

cleanup:
    efs_export_free(&g_client.export);
    stop_servers();
    print_logs();
    system("rm -rf " TEST_DIR);
    return failures;
}

int main(void)
{
    int failures = 0;

    failures += test_shrink_quota();
    failures += test_remove_node();

    if (failures == 0) {
        printf("test_migrate: OK\n");
        return 0;
    }
    printf("test_migrate: %d failures\n", failures);
    return 1;
}
