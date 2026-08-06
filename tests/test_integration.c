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

#define TEST_DIR "/tmp/efs_test"
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

static int start_servers(void)
{
    system("rm -rf " TEST_DIR "; mkdir -p " TEST_DIR);

    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(children[i].storage, sizeof(children[i].storage), TEST_DIR "/s%d", i + 1);
        snprintf(children[i].log, sizeof(children[i].log), TEST_DIR "/s%d.log", i + 1);
        snprintf(children[i].host, sizeof(children[i].host), "127.0.0.1");
        children[i].port = 18432 + i;
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
        if (children[i].pid > 0) {
            kill(children[i].pid, SIGTERM);
        }
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

static int verify_file(const char *path, const char *expected, size_t expected_len)
{
    struct efs_inode ino;
    if (efs_client_lookup(path, &ino) != 0)
        return -1;

    char *buf = malloc(expected_len + 1);
    size_t got = 0;
    int rc = efs_client_read(ino.ino, 0, expected_len, buf, &got);
    if (rc != 0 || got != expected_len) {
        fprintf(stderr, "verify_file: read failed rc=%d got=%zu expected=%zu size=%llu\n",
                rc, got, expected_len, (unsigned long long)ino.size);
        free(buf);
        return -1;
    }
    buf[expected_len] = '\0';
    if (memcmp(buf, expected, expected_len) != 0) {
        fprintf(stderr, "verify_file: mismatch, got '%s', expected '%s'\n", buf, expected);
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
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

int main(void)
{
    int failures = 0;

    if (start_servers() != 0) {
        fprintf(stderr, "Failed to start servers\n");
        stop_servers();
        print_logs();
        return 1;
    }
    printf("Servers started\n");

    if (join_cluster() != 0) {
        fprintf(stderr, "Failed to form cluster\n");
        failures++;
        goto cleanup;
    }
    printf("Cluster formed\n");

    if (create_export("test") != 0) {
        fprintf(stderr, "Failed to create export\n");
        failures++;
        goto cleanup;
    }
    printf("Export created\n");

    /* Initialize client. */
    const char *node_addrs[EFS_MAX_NODES] = {
        "127.0.0.1:18432",
        "127.0.0.1:18433",
        "127.0.0.1:18434"
    };
    efs_client_init_nodes(&g_client, node_addrs, NODE_COUNT);
    g_client.export_id = 1;
    strncpy(g_client.export_name, "test", EFS_MAX_NAME - 1);
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);

    if (efs_client_fetch_metadata(children[0].host, children[0].port) != 0) {
        fprintf(stderr, "Failed to fetch metadata\n");
        failures++;
        goto cleanup;
    }
    printf("Metadata fetched\n");

    /* Create a file and write data. */
    efs_ino_t root = EFS_ROOT_INO;
    efs_ino_t fno = efs_client_create(root, "hello.txt", S_IFREG | 0644, 0, 0);
    if (fno == 0) {
        fprintf(stderr, "Failed to create file\n");
        failures++;
        goto cleanup;
    }
    printf("File created, ino=%llu\n", (unsigned long long)fno);

    const char data[] = "Hello, distributed world! This is a test of efs.";
    size_t len = sizeof(data) - 1;
    if (efs_client_write(fno, 0, len, data) != 0) {
        fprintf(stderr, "Failed to write file\n");
        failures++;
        goto cleanup;
    }
    printf("File written (%zu bytes)\n", len);

    if (verify_file("/hello.txt", data, len) != 0) {
        fprintf(stderr, "FAIL: read back did not match\n");
        failures++;
    } else {
        printf("Read back OK\n");
    }

    /* Stop one server and verify we can still read. */
    printf("Stopping server 2...\n");
    kill(children[1].pid, SIGKILL);
    waitpid(children[1].pid, NULL, 0);
    children[1].pid = 0;
    sleep(1);

    if (verify_file("/hello.txt", data, len) != 0) {
        fprintf(stderr, "FAIL: read after node failure did not match\n");
        failures++;
    } else {
        printf("Read after node failure OK\n");
    }

cleanup:
    efs_export_free(&g_client.export);
    stop_servers();
    print_logs();
    system("rm -rf " TEST_DIR);

    if (failures == 0) {
        printf("test_integration: OK\n");
        return 0;
    }
    printf("test_integration: %d failures\n", failures);
    return 1;
}
