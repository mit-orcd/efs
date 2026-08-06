#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define TEST_DIR "/tmp/efs_list_exports_test"
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
        children[i].port = 18502 + i;
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
            close(fd);
            return -1;
        }
        free(reply);
        close(fd);
        usleep(200 * 1000);
    }
    return 0;
}

static int create_export(const char *name)
{
    int fd = efs_connect_tcp(children[0].host, children[0].port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_create_export req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, name, EFS_MAX_NAME - 1);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_CREATE_EXPORT, &req, sizeof(req)) != 0 ||
        efs_recv_msg(fd, &type, &reply, &reply_len) != 0 ||
        type != EFS_MSG_CREATE_EXPORT_REPLY || reply_len < 1 ||
        ((uint8_t *)reply)[0] != EFS_CREATE_EXPORT_OK) {
        close(fd);
        return -1;
    }
    free(reply);
    close(fd);
    return 0;
}

static int list_exports_and_check(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "./efs-mgmt list-exports %s:%u", children[0].host, children[0].port);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char line[256];
    int found_alpha = 0;
    int found_beta = 0;
    int count = -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned int id;
        char name[EFS_MAX_NAME];
        if (sscanf(line, "Exports (%u):", &count) == 1)
            continue;
        if (sscanf(line, " id=%u name=%s", &id, name) == 2) {
            if (strcmp(name, "alpha") == 0)
                found_alpha = 1;
            else if (strcmp(name, "beta") == 0)
                found_beta = 1;
        }
    }
    int rc = pclose(fp);
    if (rc != 0) {
        fprintf(stderr, "efs-mgmt list-exports exited with status %d\n", rc);
        return -1;
    }

    if (count != 2) {
        fprintf(stderr, "Unexpected export count: %d\n", count);
        return -1;
    }
    if (!found_alpha || !found_beta) {
        fprintf(stderr, "Missing exports: alpha=%d beta=%d\n", found_alpha, found_beta);
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

    if (create_export("alpha") != 0) {
        fprintf(stderr, "Failed to create export alpha\n");
        failures++;
        goto cleanup;
    }
    if (create_export("beta") != 0) {
        fprintf(stderr, "Failed to create export beta\n");
        failures++;
        goto cleanup;
    }
    if (create_export("alpha") == 0) {
        fprintf(stderr, "Duplicate export alpha should have been rejected\n");
        failures++;
        goto cleanup;
    }
    printf("Exports created\n");

    if (list_exports_and_check() != 0) {
        fprintf(stderr, "FAIL: list-exports check failed\n");
        failures++;
    } else {
        printf("List exports OK\n");
    }

cleanup:
    stop_servers();
    if (failures == 0)
        printf("test_list_exports: OK\n");
    else
        printf("test_list_exports: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
