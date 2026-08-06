#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int send_recv(int fd, uint8_t type, const void *send_payload, uint32_t send_len,
                     uint8_t *reply_type, void **reply_payload, uint32_t *reply_len)
{
    if (efs_send_msg(fd, type, send_payload, send_len) != 0)
        return -1;
    return efs_recv_msg(fd, reply_type, reply_payload, reply_len);
}

static void format_bytes(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024ULL * 1024 * 1024 * 1024)
        snprintf(buf, len, "%.2f TiB", bytes / (1024.0 * 1024 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, len, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(buf, len, "%.2f MiB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024ULL)
        snprintf(buf, len, "%.2f KiB", bytes / 1024.0);
    else
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
}

static int cmd_status(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: status <node:port>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_LIST_NODES, NULL, 0, &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_LIST_NODES_REPLY || reply_len != sizeof(struct efs_msg_list_nodes_reply)) {
        fprintf(stderr, "Failed to get node list\n");
        close(fd);
        return 1;
    }

    struct efs_msg_list_nodes_reply *list = reply;
    printf("Cluster nodes (%u):\n", list->node_count);

    int full_nodes = 0;
    for (uint32_t i = 0; i < list->node_count; i++) {
        char used_str[32], quota_str[32];
        format_bytes(list->nodes[i].used, used_str, sizeof(used_str));
        if (list->nodes[i].quota > 0) {
            format_bytes(list->nodes[i].quota, quota_str, sizeof(quota_str));
            double pct = 100.0 * (double)list->nodes[i].used / (double)list->nodes[i].quota;
            printf("  node %u: %s:%u  storage=%s  used=%s  quota=%s (%.1f%%)\n",
                   list->nodes[i].id, list->nodes[i].addr, list->nodes[i].port,
                   list->nodes[i].storage_path, used_str, quota_str, pct);
            if (list->nodes[i].used >= list->nodes[i].quota)
                full_nodes++;
        } else {
            printf("  node %u: %s:%u  storage=%s  used=%s  quota=unlimited\n",
                   list->nodes[i].id, list->nodes[i].addr, list->nodes[i].port,
                   list->nodes[i].storage_path, used_str);
        }
    }

    printf("Cluster state: %s\n", (full_nodes >= 2) ? "FULL" : "OK");

    free(reply);
    reply = NULL;

    /* Operational state of the contacted node (active/draining/drained/...). */
    if (send_recv(fd, EFS_MSG_STATUS, NULL, 0, &reply_type, &reply, &reply_len) == 0 &&
        reply_type == EFS_MSG_STATUS_REPLY &&
        reply_len == sizeof(struct efs_msg_status_reply)) {
        struct efs_msg_status_reply *st = reply;
        const char *state_name = "unknown";
        switch (st->state) {
        case EFS_NODE_STATE_ACTIVE:    state_name = "active"; break;
        case EFS_NODE_STATE_LEAVING:   state_name = "leaving"; break;
        case EFS_NODE_STATE_SHRINKING: state_name = "shrinking"; break;
        case EFS_NODE_STATE_DRAINING:  state_name = "draining"; break;
        case EFS_NODE_STATE_DRAINED:   state_name = "drained"; break;
        }
        printf("Node %s:%u state: %s\n", host, port, state_name);
    }
    free(reply);
    close(fd);
    return 0;
}

static int cmd_list_exports(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: list-exports <node:port>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_LIST_EXPORTS, NULL, 0, &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_LIST_EXPORTS_REPLY ||
        reply_len != sizeof(struct efs_msg_list_exports_reply)) {
        fprintf(stderr, "Failed to list exports\n");
        close(fd);
        return 1;
    }

    struct efs_msg_list_exports_reply *list = reply;
    printf("Exports (%u):\n", list->export_count);
    for (uint32_t i = 0; i < list->export_count; i++) {
        printf("  id=%u  name=%s\n", list->exports[i].id, list->exports[i].name);
    }

    free(reply);
    close(fd);
    return 0;
}

static int cmd_mkfs(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: mkfs <node:port> <export-name>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_create_export req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, argv[1], EFS_MAX_NAME - 1);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_CREATE_EXPORT, &req, sizeof(req), &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_CREATE_EXPORT_REPLY || reply_len != 1) {
        fprintf(stderr, "Failed to create export\n");
        close(fd);
        return 1;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    if (status == EFS_CREATE_EXPORT_OK)
        printf("Export '%s' created on %s:%u\n", argv[1], host, port);
    else if (status == EFS_CREATE_EXPORT_EXISTS)
        fprintf(stderr, "Export '%s' already exists on %s:%u\n", argv[1], host, port);
    else
        fprintf(stderr, "Failed to create export '%s'\n", argv[1]);

    free(reply);
    close(fd);
    return (status == EFS_CREATE_EXPORT_OK) ? 0 : 1;
}

static int cmd_add_node(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: add-node <new-node:port> <existing-node:port>\n");
        return 1;
    }
    char new_host[64], existing_host[64];
    uint16_t new_port, existing_port;
    if (parse_host_port(argv[0], new_host, sizeof(new_host), &new_port) != 0 ||
        parse_host_port(argv[1], existing_host, sizeof(existing_host), &existing_port) != 0) {
        fprintf(stderr, "Invalid addresses\n");
        return 1;
    }

    int fd = efs_connect_tcp(new_host, new_port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to new node %s:%u\n", new_host, new_port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_join req;
    memset(&req, 0, sizeof(req));
    strncpy(req.peer_host, existing_host, sizeof(req.peer_host) - 1);
    req.peer_port = existing_port;

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_JOIN, &req, sizeof(req), &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_JOIN_REPLY || reply_len != 1) {
        fprintf(stderr, "Failed to send join request\n");
        close(fd);
        return 1;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    if (status == EFS_JOIN_OK)
        printf("Node %s:%u joined cluster via %s:%u\n", new_host, new_port, existing_host, existing_port);
    else
        fprintf(stderr, "Join failed\n");

    free(reply);
    close(fd);
    return (status == EFS_JOIN_OK) ? 0 : 1;
}

static int cmd_drain_node(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: drain-node <node:port>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_DRAIN_NODE, NULL, 0, &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_DRAIN_NODE_REPLY || reply_len != 1) {
        fprintf(stderr, "Failed to send drain-node request\n");
        close(fd);
        return 1;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    free(reply);
    close(fd);
    if (status == EFS_DRAIN_NODE_IN_PROGRESS) {
        printf("Node %s:%u is draining; watch with: efs-mgmt status %s:%u\n",
               host, port, host, port);
        return 0;
    }
    if (status == EFS_DRAIN_NODE_OK) {
        printf("Node %s:%u is drained (empty)\n", host, port);
        return 0;
    }
    fprintf(stderr, "Drain-node failed\n");
    return 1;
}

static int cmd_undrain_node(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: undrain-node <node:port>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_UNDRAIN_NODE, NULL, 0, &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_UNDRAIN_NODE_REPLY || reply_len != 1) {
        fprintf(stderr, "Failed to send undrain-node request\n");
        close(fd);
        return 1;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    free(reply);
    close(fd);
    if (status == EFS_UNDRAIN_NODE_OK) {
        printf("Node %s:%u is active again\n", host, port);
        return 0;
    }
    fprintf(stderr, "Undrain-node failed (must be drained, not still draining)\n");
    return 1;
}

static int cmd_remove_node(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: remove-node <node:port>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_REMOVE_NODE, NULL, 0, &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_REMOVE_NODE_REPLY || reply_len != 1) {
        fprintf(stderr, "Failed to send remove-node request\n");
        close(fd);
        return 1;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    free(reply);
    close(fd);
    if (status == EFS_REMOVE_NODE_OK) {
        printf("Node %s:%u is leaving the cluster\n", host, port);
        return 0;
    }
    if (status == EFS_REMOVE_NODE_NOT_DRAINED) {
        fprintf(stderr,
                "Remove-node refused: node still holds data. Run: efs-mgmt drain-node %s:%u\n",
                host, port);
        return 1;
    }
    fprintf(stderr, "Remove-node failed\n");
    return 1;
}

static int cmd_shrink_quota(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: shrink-quota <node:port> <amount>[T|G|M|K]\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    uint64_t amount = efs_parse_quota(argv[1]);
    if (amount == 0) {
        fprintf(stderr, "Invalid amount: %s\n", argv[1]);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_shrink_quota req;
    memset(&req, 0, sizeof(req));
    req.amount = amount;

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (send_recv(fd, EFS_MSG_SHRINK_QUOTA, &req, sizeof(req), &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_SHRINK_QUOTA_REPLY || reply_len != 1) {
        fprintf(stderr, "Failed to send shrink-quota request\n");
        close(fd);
        return 1;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    if (status == EFS_SHRINK_QUOTA_IN_PROGRESS)
        printf("Shrinking quota on %s:%u by %s; background migration started\n",
               host, port, argv[1]);
    else
        fprintf(stderr, "Shrink-quota failed\n");

    free(reply);
    close(fd);
    return (status == EFS_SHRINK_QUOTA_IN_PROGRESS) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
    fprintf(stderr, "Usage: %s <command> [args]\n"
                    "Commands:\n"
                    "  status <node:port>\n"
                    "  list-exports <node:port>\n"
                    "  mkfs <node:port> <export-name>\n"
                    "  add-node <new-node:port> <existing-node:port>\n"
                    "  drain-node <node:port>\n"
                    "  undrain-node <node:port>\n"
                    "  remove-node <node:port>\n"
                    "  shrink-quota <node:port> <amount>[T|G|M|K]\n",
            argv[0]);
    return 1;
}

    const char *cmd = argv[1];
    if (strcmp(cmd, "status") == 0)
        return cmd_status(argc - 2, argv + 2);
    if (strcmp(cmd, "list-exports") == 0)
        return cmd_list_exports(argc - 2, argv + 2);
    if (strcmp(cmd, "mkfs") == 0)
        return cmd_mkfs(argc - 2, argv + 2);
    if (strcmp(cmd, "add-node") == 0)
        return cmd_add_node(argc - 2, argv + 2);
    if (strcmp(cmd, "drain-node") == 0)
        return cmd_drain_node(argc - 2, argv + 2);
    if (strcmp(cmd, "undrain-node") == 0)
        return cmd_undrain_node(argc - 2, argv + 2);
    if (strcmp(cmd, "remove-node") == 0)
        return cmd_remove_node(argc - 2, argv + 2);
    if (strcmp(cmd, "shrink-quota") == 0)
        return cmd_shrink_quota(argc - 2, argv + 2);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
