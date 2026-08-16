#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_DIR "/tmp/efs_add_storage_test"
#define NODE_COUNT 2

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
    for (int t = 0; t < timeout_ms; t += 100) {
        int fd = efs_connect_tcp(host, port);
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
        snprintf(children[i].storage, sizeof(children[i].storage),
                 TEST_DIR "/s%d", i + 1);
        snprintf(children[i].log, sizeof(children[i].log),
                 TEST_DIR "/s%d.log", i + 1);
        snprintf(children[i].host, sizeof(children[i].host), "127.0.0.1");
        children[i].port = (uint16_t)(18621 + i);
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
        if (wait_for_server(children[i].host, children[i].port, 4000) != 0) {
            fprintf(stderr, "Server %d did not start\n", i + 1);
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
    int fd = efs_connect_tcp(children[1].host, children[1].port);
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
    uint32_t rlen = 0;
    int rc = -1;
    if (efs_send_msg(fd, EFS_MSG_JOIN, &req, sizeof(req)) == 0 &&
        efs_recv_msg(fd, &type, &reply, &rlen) == 0 &&
        type == EFS_MSG_JOIN_REPLY && rlen == 1 &&
        ((uint8_t *)reply)[0] == EFS_JOIN_OK)
        rc = 0;
    free(reply);
    close(fd);
    return rc;
}

static int add_storage(const char *host, uint16_t port, const char *paths,
                       uint8_t *status_out, uint32_t *count_out)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    struct efs_msg_add_storage req;
    memset(&req, 0, sizeof(req));
    strncpy(req.paths, paths, sizeof(req.paths) - 1);
    uint8_t type;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc = -1;
    if (efs_send_msg(fd, EFS_MSG_ADD_STORAGE, &req, sizeof(req)) == 0 &&
        efs_recv_msg(fd, &type, &reply, &rlen) == 0 &&
        type == EFS_MSG_ADD_STORAGE_REPLY &&
        rlen == sizeof(struct efs_msg_add_storage_reply)) {
        struct efs_msg_add_storage_reply *r = reply;
        if (status_out)
            *status_out = r->status;
        if (count_out)
            *count_out = r->path_count;
        rc = 0;
    }
    free(reply);
    close(fd);
    return rc;
}

static int list_node0_paths(char *out, size_t out_len)
{
    int fd = efs_connect_tcp(children[1].host, children[1].port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    uint8_t type;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc = -1;
    if (efs_send_msg(fd, EFS_MSG_LIST_NODES, NULL, 0) == 0 &&
        efs_recv_msg(fd, &type, &reply, &rlen) == 0 &&
        type == EFS_MSG_LIST_NODES_REPLY &&
        rlen == sizeof(struct efs_msg_list_nodes_reply)) {
        struct efs_msg_list_nodes_reply *r = reply;
        for (uint32_t i = 0; i < r->node_count; i++) {
            if (r->nodes[i].id == 1) {
                strncpy(out, r->nodes[i].storage_path, out_len - 1);
                out[out_len - 1] = '\0';
                rc = 0;
                break;
            }
        }
    }
    free(reply);
    close(fd);
    return rc;
}

static int dir_ok(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void)
{
    int failures = 0;
    if (start_servers() != 0) {
        fprintf(stderr, "Failed to start servers\n");
        stop_servers();
        return 1;
    }
    if (join_cluster() != 0) {
        fprintf(stderr, "Failed to form cluster\n");
        stop_servers();
        return 1;
    }

    char extra[256], extra2[256];
    snprintf(extra, sizeof(extra), TEST_DIR "/s1b");
    snprintf(extra2, sizeof(extra2), TEST_DIR "/s1c");
    mkdir(extra, 0755);
    mkdir(extra2, 0755);

    uint8_t st = 0xff;
    uint32_t n = 0;
    char list[512];
    snprintf(list, sizeof(list), "%s,%s", extra, extra2);
    if (add_storage(children[0].host, children[0].port, list, &st, &n) != 0 ||
        st != EFS_ADD_STORAGE_OK || n != 3) {
        fprintf(stderr, "add-storage failed status=%u count=%u\n", st, n);
        failures++;
        goto cleanup;
    }
    printf("add-storage OK count=%u\n", n);

    char data[512], meta[512], logp[512];
    snprintf(data, sizeof(data), "%s/data", extra);
    snprintf(meta, sizeof(meta), "%s/meta", extra);
    snprintf(logp, sizeof(logp), "%s/log", extra);
    if (!dir_ok(data) || !dir_ok(meta) || !dir_ok(logp)) {
        fprintf(stderr, "new root missing data/meta/log\n");
        failures++;
        goto cleanup;
    }

    st = 0xff;
    n = 0;
    if (add_storage(children[0].host, children[0].port, extra, &st, &n) != 0 ||
        st != EFS_ADD_STORAGE_OK || n != 3) {
        fprintf(stderr, "idempotent add-storage failed status=%u count=%u\n",
                st, n);
        failures++;
        goto cleanup;
    }

    st = 0xff;
    if (add_storage(children[0].host, children[0].port, "relative/path",
                    &st, &n) != 0 ||
        st != EFS_ADD_STORAGE_INVALID) {
        fprintf(stderr, "relative path should be INVALID, got %u\n", st);
        failures++;
        goto cleanup;
    }

    /* Peer should see the updated comma list after gossip. */
    char seen[EFS_MAX_PATH];
    seen[0] = '\0';
    int got = -1;
    for (int t = 0; t < 20; t++) {
        if (list_node0_paths(seen, sizeof(seen)) == 0 &&
            strstr(seen, extra) && strstr(seen, extra2) &&
            strstr(seen, children[0].storage)) {
            got = 0;
            break;
        }
        usleep(100 * 1000);
    }
    if (got != 0) {
        fprintf(stderr, "peer LIST_NODES missing new roots: '%s'\n", seen);
        failures++;
        goto cleanup;
    }
    printf("peer sees paths: %s\n", seen);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./efs-mgmt add-storage %s:%u %s 2>&1",
             children[0].host, children[0].port, extra);
    if (system(cmd) != 0) {
        fprintf(stderr, "efs-mgmt add-storage (idempotent) failed\n");
        failures++;
    }

cleanup:
    stop_servers();
    if (failures == 0)
        printf("test_add_storage: OK\n");
    else
        printf("test_add_storage: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
