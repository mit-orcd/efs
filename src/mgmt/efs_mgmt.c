#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <stdint.h>
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

/* Short STATUS probe so a dead peer does not stall status for EFS_IO_TIMEOUT_MS. */
#define EFS_STATUS_PROBE_MS 2000

static int probe_node_up(const char *nhost, uint16_t nport)
{
    int pfd = efs_connect_tcp(nhost, nport);
    if (pfd < 0)
        return 0;
    efs_set_recv_timeout(pfd, EFS_STATUS_PROBE_MS);
    efs_set_send_timeout(pfd, EFS_STATUS_PROBE_MS);
    uint8_t type = 0;
    void *preply = NULL;
    uint32_t preply_len = 0;
    int ok = 0;
    if (efs_send_msg(pfd, EFS_MSG_STATUS, NULL, 0) == 0 &&
        efs_recv_msg(pfd, &type, &preply, &preply_len) == 0 &&
        type == EFS_MSG_STATUS_REPLY &&
        preply_len == sizeof(struct efs_msg_status_reply))
        ok = 1;
    free(preply);
    close(pfd);
    return ok;
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

    /* Surface corrupt membership (duplicate ids) — placement will skip nodes. */
    for (uint32_t i = 0; i < list->node_count; i++) {
        for (uint32_t j = i + 1; j < list->node_count; j++) {
            if (list->nodes[i].id == list->nodes[j].id) {
                fprintf(stderr,
                        "WARNING: duplicate node id %u in membership "
                        "(%s:%u and %s:%u) — restart cluster with fixed efsd\n",
                        list->nodes[i].id,
                        list->nodes[i].addr, list->nodes[i].port,
                        list->nodes[j].addr, list->nodes[j].port);
            }
        }
    }

    int full_nodes = 0;
    int up_nodes = 0;
    int down_nodes = 0;
    int all_have_quota = (list->node_count > 0);
    uint64_t min_quota = 0;
    uint64_t min_free = UINT64_MAX;
    for (uint32_t i = 0; i < list->node_count; i++) {
        /* Probe every advertised addr (may differ from the seed string we used). */
        int up = probe_node_up(list->nodes[i].addr, list->nodes[i].port);
        if (up)
            up_nodes++;
        else
            down_nodes++;

        char used_str[32], quota_str[32];
        format_bytes(list->nodes[i].used, used_str, sizeof(used_str));
        /* storage_path may be a comma-joined list of local roots. */
        unsigned npaths = 1;
        for (const char *p = list->nodes[i].storage_path; *p; p++) {
            if (*p == ',')
                npaths++;
        }
        const char *reach = up ? "up" : "DOWN";
        if (list->nodes[i].quota > 0) {
            format_bytes(list->nodes[i].quota, quota_str, sizeof(quota_str));
            double pct = 100.0 * (double)list->nodes[i].used / (double)list->nodes[i].quota;
            printf("  node %u: %s:%u  %s  used=%s  quota=%s (%.1f%%)  storage(%u):\n",
                   list->nodes[i].id, list->nodes[i].addr, list->nodes[i].port,
                   reach, used_str, quota_str, pct, npaths);
            if (up && list->nodes[i].used >= list->nodes[i].quota)
                full_nodes++;
            if (up) {
                if (min_quota == 0 || list->nodes[i].quota < min_quota)
                    min_quota = list->nodes[i].quota;
                uint64_t free_i = (list->nodes[i].used < list->nodes[i].quota)
                                      ? (list->nodes[i].quota - list->nodes[i].used)
                                      : 0;
                if (free_i < min_free)
                    min_free = free_i;
            }
        } else {
            all_have_quota = 0;
            printf("  node %u: %s:%u  %s  used=%s  quota=unlimited  storage(%u):\n",
                   list->nodes[i].id, list->nodes[i].addr, list->nodes[i].port,
                   reach, used_str, npaths);
        }
        char paths[EFS_MAX_PATH];
        strncpy(paths, list->nodes[i].storage_path, sizeof(paths) - 1);
        paths[sizeof(paths) - 1] = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(paths, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save))
            printf("      %s\n", tok);
    }

    /* 2+1 cluster EC: each logical byte needs a fragment on every node, so
     * usable capacity is 2 * min_quota (same model as FUSE df). */
    if (all_have_quota && min_quota > 0 && min_free != UINT64_MAX && up_nodes >= 2) {
        uint64_t usable_cap = min_quota * 2;
        uint64_t usable_free = min_free * 2;
        uint64_t usable_used =
            (usable_cap > usable_free) ? (usable_cap - usable_free) : 0;
        char used_s[32], free_s[32], cap_s[32];
        format_bytes(usable_used, used_s, sizeof(used_s));
        format_bytes(usable_free, free_s, sizeof(free_s));
        format_bytes(usable_cap, cap_s, sizeof(cap_s));
        double pct = 100.0 * (double)usable_used / (double)usable_cap;
        printf("Usable (2+1 logical): used=%s  free=%s  capacity=%s (%.1f%%)\n",
               used_s, free_s, cap_s, pct);
    } else if (down_nodes > 0) {
        printf("Usable (2+1 logical): degraded (%d/%u nodes up)\n",
               up_nodes, list->node_count);
    } else {
        printf("Usable (2+1 logical): n/a (set a quota on every node)\n");
    }

    const char *cluster_state = "OK";
    if (up_nodes < 2)
        cluster_state = "UNAVAILABLE";
    else if (down_nodes > 0)
        cluster_state = "DEGRADED";
    else if (full_nodes >= 2)
        cluster_state = "FULL";
    printf("Cluster state: %s (%d up, %d down)\n", cluster_state, up_nodes,
           down_nodes);

    uint32_t ncopy = list->node_count;
    if (ncopy > EFS_MAX_NODES)
        ncopy = EFS_MAX_NODES;
    struct efs_node nodes_copy[EFS_MAX_NODES];
    memcpy(nodes_copy, list->nodes, ncopy * sizeof(nodes_copy[0]));

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
    reply = NULL;

    printf("Heal:\n");
    {
        int any = 0;
        for (uint32_t i = 0; i < ncopy; i++) {
            if (!probe_node_up(nodes_copy[i].addr, nodes_copy[i].port)) {
                printf("  node %u: DOWN\n", nodes_copy[i].id);
                continue;
            }
            int hfd = efs_connect_tcp(nodes_copy[i].addr, nodes_copy[i].port);
            if (hfd < 0) {
                printf("  node %u: unreachable\n", nodes_copy[i].id);
                continue;
            }
            efs_set_recv_timeout(hfd, EFS_STATUS_PROBE_MS);
            efs_set_send_timeout(hfd, EFS_STATUS_PROBE_MS);
            uint8_t ht = 0;
            void *hp = NULL;
            uint32_t hl = 0;
            if (efs_send_msg(hfd, EFS_MSG_HEAL_STATUS, NULL, 0) != 0 ||
                efs_recv_msg(hfd, &ht, &hp, &hl) != 0 ||
                ht != EFS_MSG_HEAL_STATUS_REPLY ||
                hl != sizeof(struct efs_msg_heal_status_reply)) {
                printf("  node %u: heal-status unsupported\n",
                       nodes_copy[i].id);
                free(hp);
                close(hfd);
                continue;
            }
            struct efs_msg_heal_status_reply *hs = hp;
            if (!hs->healing && hs->export_count == 0) {
                printf("  node %u: idle\n", nodes_copy[i].id);
                any = 1;
                free(hp);
                close(hfd);
                continue;
            }
            if (!hs->healing) {
                printf("  node %u: idle", nodes_copy[i].id);
                for (uint32_t e = 0; e < hs->export_count; e++) {
                    if (hs->exports[e].name[0])
                        printf("  %s gen=%llu", hs->exports[e].name,
                               (unsigned long long)hs->exports[e].gen);
                }
                printf("\n");
                any = 1;
                free(hp);
                close(hfd);
                continue;
            }
            for (uint32_t e = 0; e < hs->export_count; e++) {
                struct efs_msg_heal_status_export *x = &hs->exports[e];
                if (!x->flags)
                    continue;
                any = 1;
                printf("  node %u: healing  %s", nodes_copy[i].id,
                       x->name[0] ? x->name : "?");
                if (x->flags & EFS_HEAL_F_REBUILD)
                    printf("  shard=%u  pages %u/%u", x->cur_shard,
                           x->pages_done, x->pages_total);
                if (x->tables_need)
                    printf("  tables %u/%u dirty", x->tables_need,
                           x->tables_total);
                printf("  gen=%llu", (unsigned long long)x->gen);
                if ((x->flags & EFS_HEAL_F_REBUILD) && x->pages_done > 0 &&
                    x->pages_total > x->pages_done && x->elapsed_us > 0) {
                    uint64_t left = (x->elapsed_us / x->pages_done) *
                                    (x->pages_total - x->pages_done);
                    if (left < 1000000ull)
                        printf("  ~<1s left");
                    else if (left < 60000000ull)
                        printf("  ~%llu s left",
                               (unsigned long long)(left / 1000000ull));
                    else
                        printf("  ~%llu min left",
                               (unsigned long long)(left / 60000000ull));
                }
                printf("\n");
            }
            free(hp);
            close(hfd);
        }
        if (!any)
            printf("  (no heal data)\n");
    }

    close(fd);
    return (down_nodes > 0) ? 1 : 0;
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
        const char *n = list->exports[i].name;
        if (!n[0])
            n = "(unnamed)";
        else if (strcmp(n, "pending") == 0)
            n = "(pending)";
        printf("  id=%u  name=%s\n", list->exports[i].id, n);
    }

    free(reply);
    close(fd);
    return 0;
}

static int cmd_mkfs(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: mkfs <node:port> <export-name> "
                "[--chunk-size <bytes|128K|1M|…>]\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    uint32_t chunk_size = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--chunk-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mkfs: --chunk-size requires a value\n");
                return 1;
            }
            chunk_size = (uint32_t)efs_parse_quota(argv[++i]);
            if (chunk_size == 0 || !efs_chunk_size_valid(chunk_size)) {
                fprintf(stderr,
                        "Invalid chunk size (power of two, %u..%u bytes)\n",
                        EFS_MIN_CHUNK_SIZE, EFS_MAX_CHUNK_SIZE);
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown mkfs argument: %s\n", argv[i]);
            return 1;
        }
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
    req.chunk_size = chunk_size;

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
    int rc = 1;
    if (status == EFS_CREATE_EXPORT_OK) {
        uint32_t effective = chunk_size ? chunk_size : EFS_DEFAULT_CHUNK_SIZE;
        char cs_str[32];
        format_bytes(effective, cs_str, sizeof(cs_str));
        printf("Export '%s' created on %s:%u (chunk_size=%s shards=%u)\n",
               argv[1], host, port, cs_str,
               1u << EFS_DEFAULT_SHARD_BITS);
        rc = 0;
    } else if (status == EFS_CREATE_EXPORT_EXISTS) {
        fprintf(stderr, "Export '%s' already exists on %s:%u\n", argv[1], host, port);
    } else if (status == EFS_CREATE_EXPORT_REPLICATE_FAILED) {
        /* Local create succeeded; fragmented meta page/root flush to peers
         * did not reach quorum. Membership can still look fine in status. */
        fprintf(stderr,
                "Export '%s' created on %s:%u, but metadata page replicate "
                "failed (see efsd log: meta-flush). Cluster membership may "
                "still look OK — this is a meta placement/write failure, "
                "not a join failure.\n",
                argv[1], host, port);
    } else {
        fprintf(stderr, "Failed to create export '%s'\n", argv[1]);
    }

    free(reply);
    close(fd);
    return rc;
}

static int destroy_on_node(const char *host, uint16_t port, const char *name,
                           uint8_t *status_out)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_destroy_export req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, name, EFS_MAX_NAME - 1);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = send_recv(fd, EFS_MSG_DESTROY_EXPORT, &req, sizeof(req),
                       &reply_type, &reply, &reply_len);
    if (rc != 0 || reply_type != EFS_MSG_DESTROY_EXPORT_REPLY || reply_len != 1) {
        free(reply);
        close(fd);
        return -1;
    }
    *status_out = ((uint8_t *)reply)[0];
    free(reply);
    close(fd);
    return 0;
}

static int cmd_destroy(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: destroy|rmfs <node:port> <export-name|--unnamed>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    /* "-" / "--unnamed" removes bootstrap leftovers with an empty/pending name. */
    const char *export_name = argv[1];
    int wipe_unnamed = (strcmp(argv[1], "-") == 0 ||
                        strcmp(argv[1], "--unnamed") == 0);
    if (!wipe_unnamed && !argv[1][0]) {
        fprintf(stderr, "Export name must be non-empty (or --unnamed)\n");
        return 1;
    }
    if (wipe_unnamed)
        export_name = "";

    /* Ask the contacted node for membership, then wipe the export everywhere. */
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
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count = 0;
    if (send_recv(fd, EFS_MSG_LIST_NODES, NULL, 0, &reply_type, &reply, &reply_len) == 0 &&
        reply_type == EFS_MSG_LIST_NODES_REPLY &&
        reply_len == sizeof(struct efs_msg_list_nodes_reply)) {
        struct efs_msg_list_nodes_reply *list = reply;
        node_count = list->node_count;
        if (node_count > EFS_MAX_NODES)
            node_count = EFS_MAX_NODES;
        memcpy(nodes, list->nodes, node_count * sizeof(nodes[0]));
    }
    free(reply);
    close(fd);

    if (node_count == 0) {
        nodes[0].port = port;
        strncpy(nodes[0].addr, host, sizeof(nodes[0].addr) - 1);
        node_count = 1;
    }

    const char *label = wipe_unnamed ? "(unnamed/pending)" : argv[1];
    int ok = 0, missing = 0, failed = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        uint8_t st = EFS_DESTROY_EXPORT_ERROR;
        if (destroy_on_node(nodes[i].addr, nodes[i].port, export_name, &st) != 0) {
            fprintf(stderr, "  %s:%u: no reply\n", nodes[i].addr, nodes[i].port);
            failed++;
            continue;
        }
        if (st == EFS_DESTROY_EXPORT_OK) {
            printf("  %s:%u: destroyed\n", nodes[i].addr, nodes[i].port);
            ok++;
        } else if (st == EFS_DESTROY_EXPORT_NOT_FOUND) {
            printf("  %s:%u: not found\n", nodes[i].addr, nodes[i].port);
            missing++;
        } else {
            fprintf(stderr, "  %s:%u: error\n", nodes[i].addr, nodes[i].port);
            failed++;
        }
    }

    if (ok > 0 && failed == 0) {
        printf("Export '%s' destroyed on %d node(s)\n", label, ok);
        return 0;
    }
    if (ok == 0 && missing == (int)node_count) {
        fprintf(stderr, "Export '%s' not found on any node\n", label);
        return 1;
    }
    fprintf(stderr, "Export '%s': destroyed=%d missing=%d failed=%d\n",
            label, ok, missing, failed);
    return failed ? 1 : 0;
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

static int cmd_add_storage(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: add-storage <node:port> <path>[,path...]\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    struct efs_msg_add_storage req;
    memset(&req, 0, sizeof(req));
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        const char *p = argv[i];
        size_t n = strlen(p);
        if (n == 0)
            continue;
        if (off + n + (off ? 1 : 0) >= sizeof(req.paths)) {
            fprintf(stderr, "Path list too long\n");
            return 1;
        }
        if (off) {
            req.paths[off++] = ',';
            req.paths[off] = '\0';
        }
        memcpy(req.paths + off, p, n + 1);
        off += n;
    }
    if (off == 0) {
        fprintf(stderr, "No storage paths given\n");
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
    if (send_recv(fd, EFS_MSG_ADD_STORAGE, &req, sizeof(req),
                  &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_ADD_STORAGE_REPLY ||
        reply_len != sizeof(struct efs_msg_add_storage_reply)) {
        fprintf(stderr, "Failed to send add-storage request\n");
        close(fd);
        return 1;
    }

    struct efs_msg_add_storage_reply *r = reply;
    int rc = 1;
    switch (r->status) {
    case EFS_ADD_STORAGE_OK:
        printf("Added storage on %s:%u; now %u local root(s)\n",
               host, port, r->path_count);
        rc = 0;
        break;
    case EFS_ADD_STORAGE_FULL:
        fprintf(stderr, "add-storage: too many roots (max %d)\n",
                EFS_MAX_STORAGE_PATHS);
        break;
    case EFS_ADD_STORAGE_INVALID:
        fprintf(stderr, "add-storage: invalid path list (absolute paths only)\n");
        break;
    default:
        fprintf(stderr, "add-storage failed\n");
        break;
    }
    free(reply);
    close(fd);
    return rc;
}

static int feature_rpc(const char *host, uint16_t port, const char *export,
                       int do_set, uint32_t set_mask, uint32_t features,
                       uint32_t *out_feat, uint8_t *out_status)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    uint8_t rtype;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc;
    if (do_set) {
        struct efs_msg_set_features req;
        memset(&req, 0, sizeof(req));
        strncpy(req.export_name, export, EFS_MAX_NAME - 1);
        req.features = features;
        req.set_mask = set_mask;
        rc = send_recv(fd, EFS_MSG_SET_FEATURES, &req, sizeof(req),
                       &rtype, &reply, &rlen);
        if (rc == 0 && rtype != EFS_MSG_SET_FEATURES_REPLY)
            rc = -1;
    } else {
        struct efs_msg_get_features req;
        memset(&req, 0, sizeof(req));
        strncpy(req.export_name, export, EFS_MAX_NAME - 1);
        rc = send_recv(fd, EFS_MSG_GET_FEATURES, &req, sizeof(req),
                       &rtype, &reply, &rlen);
        if (rc == 0 && rtype != EFS_MSG_GET_FEATURES_REPLY)
            rc = -1;
    }
    if (rc == 0 && rlen >= sizeof(struct efs_msg_features_reply)) {
        struct efs_msg_features_reply *r = reply;
        *out_feat = r->features;
        *out_status = r->status;
    } else {
        rc = -1;
    }
    free(reply);
    close(fd);
    return rc;
}

static void print_features(const char *label, uint32_t feat)
{
    printf("  %-22s .stats=%s .find=%s\n", label,
           (feat & EFS_FEATURE_STATS) ? "on" : "off",
           (feat & EFS_FEATURE_FIND) ? "on" : "off");
}

static int cmd_feature(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: feature <node:port> <export> show\n"
                "       feature <node:port> <export> <stats|find> <on|off>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    const char *export = argv[1];

    if (argc >= 3 && strcmp(argv[2], "show") == 0) {
        uint32_t feat = 0;
        uint8_t st = EFS_FEATURES_NOT_FOUND;
        if (feature_rpc(host, port, export, 0, 0, 0, &feat, &st) != 0 ||
            st != EFS_FEATURES_OK) {
            fprintf(stderr, "Export '%s' not found on %s:%u\n", export, host, port);
            return 1;
        }
        printf("Export '%s' features (%s:%u):\n", export, host, port);
        print_features("current", feat);
        return 0;
    }

    if (argc < 4) {
        fprintf(stderr,
                "usage: feature <node:port> <export> <stats|find> <on|off>\n");
        return 1;
    }
    const char *fname = argv[2];
    const char *fval = argv[3];
    uint32_t bit;
    if (strcmp(fname, "stats") == 0 || strcmp(fname, ".stats") == 0)
        bit = EFS_FEATURE_STATS;
    else if (strcmp(fname, "find") == 0 || strcmp(fname, ".find") == 0)
        bit = EFS_FEATURE_FIND;
    else {
        fprintf(stderr, "Unknown feature '%s' (want stats|find)\n", fname);
        return 1;
    }
    int on;
    if (strcmp(fval, "on") == 0)
        on = 1;
    else if (strcmp(fval, "off") == 0)
        on = 0;
    else {
        fprintf(stderr, "Unknown value '%s' (want on|off)\n", fval);
        return 1;
    }
    uint32_t features = on ? bit : 0;

    /* Discover membership via the contacted node, then apply on every node. */
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count = 0;
    int fd = efs_connect_tcp(host, port);
    if (fd >= 0) {
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
        uint8_t rtype;
        void *reply = NULL;
        uint32_t rlen = 0;
        if (send_recv(fd, EFS_MSG_LIST_NODES, NULL, 0, &rtype, &reply, &rlen) == 0 &&
            rtype == EFS_MSG_LIST_NODES_REPLY &&
            rlen == sizeof(struct efs_msg_list_nodes_reply)) {
            struct efs_msg_list_nodes_reply *list = reply;
            node_count = list->node_count;
            if (node_count > EFS_MAX_NODES)
                node_count = EFS_MAX_NODES;
            memcpy(nodes, list->nodes, node_count * sizeof(nodes[0]));
        }
        free(reply);
        close(fd);
    }
    if (node_count == 0) {
        strncpy(nodes[0].addr, host, sizeof(nodes[0].addr) - 1);
        nodes[0].port = port;
        node_count = 1;
    }

    int ok = 0, failed = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t feat = 0;
        uint8_t st = EFS_FEATURES_NOT_FOUND;
        char label[96];
        snprintf(label, sizeof(label), "%s:%u", nodes[i].addr, nodes[i].port);
        if (feature_rpc(nodes[i].addr, nodes[i].port, export, 1, bit, features,
                        &feat, &st) != 0) {
            fprintf(stderr, "  %s: no reply\n", label);
            failed++;
            continue;
        }
        if (st != EFS_FEATURES_OK) {
            fprintf(stderr, "  %s: export not found\n", label);
            failed++;
            continue;
        }
        print_features(label, feat);
        ok++;
    }
    if (ok == 0) {
        fprintf(stderr, "Failed to set feature on any node\n");
        return 1;
    }
    printf("Feature '%s' turned %s on %d/%u node(s) for export '%s'\n",
           fname, on ? "on" : "off", ok, node_count, export);
    return failed ? 1 : 0;
}

static int cmd_upgrade(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: upgrade <node:port> <export> [shard-bits]\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    struct efs_msg_upgrade_meta req;
    memset(&req, 0, sizeof(req));
    strncpy(req.export_name, argv[1], EFS_MAX_NAME - 1);
    req.shard_bits = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc = send_recv(fd, EFS_MSG_UPGRADE_META, &req, sizeof(req),
                       &rtype, &reply, &rlen);
    close(fd);
    if (rc != 0 || rtype != EFS_MSG_UPGRADE_META_REPLY ||
        rlen < sizeof(struct efs_msg_upgrade_meta_reply)) {
        fprintf(stderr, "upgrade failed\n");
        free(reply);
        return 1;
    }
    struct efs_msg_upgrade_meta_reply *r = reply;
    if (r->status != EFS_UPGRADE_OK) {
        fprintf(stderr, "upgrade status %u\n", r->status);
        free(reply);
        return 1;
    }
    printf("upgraded %s to EFSR v6 shards=%u\n", argv[1], r->shard_count);
    free(reply);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
    fprintf(stderr, "Usage: %s <command> [args]\n"
                    "Commands:\n"
                    "  status <node:port>\n"
                    "  list-exports <node:port>\n"
                    "  mkfs <node:port> <export-name> [--chunk-size <bytes|128K|1M|…>]\n"
                    "  destroy|rmfs <node:port> <export-name|--unnamed>\n"
                    "  add-node <new-node:port> <existing-node:port>\n"
                    "  drain-node <node:port>\n"
                    "  undrain-node <node:port>\n"
                    "  remove-node <node:port>\n"
                    "  shrink-quota <node:port> <amount>[T|G|M|K]\n"
                    "  add-storage <node:port> <path>[,path...]\n"
                    "  feature <node:port> <export> show|<stats|find> <on|off>\n"
                    "  upgrade <node:port> <export> [shard-bits]\n",
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
    if (strcmp(cmd, "destroy") == 0 || strcmp(cmd, "rmfs") == 0)
        return cmd_destroy(argc - 2, argv + 2);
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
    if (strcmp(cmd, "add-storage") == 0)
        return cmd_add_storage(argc - 2, argv + 2);
    if (strcmp(cmd, "feature") == 0)
        return cmd_feature(argc - 2, argv + 2);
    if (strcmp(cmd, "upgrade") == 0)
        return cmd_upgrade(argc - 2, argv + 2);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
