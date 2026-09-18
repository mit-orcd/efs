#include "efs/common.h"
#include "efs/lock.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include "efs/session.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

    /* Operational state of the contacted node (always active today). */
    if (send_recv(fd, EFS_MSG_STATUS, NULL, 0, &reply_type, &reply, &reply_len) == 0 &&
        reply_type == EFS_MSG_STATUS_REPLY &&
        reply_len == sizeof(struct efs_msg_status_reply)) {
        struct efs_msg_status_reply *st = reply;
        printf("Node %s:%u state: %s\n", host, port,
               st->state == EFS_NODE_STATE_ACTIVE ? "active" : "unknown");
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
                    printf("  tables %u/%u need-rebuild", x->tables_need,
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

static const char *raft_role_name(uint8_t role)
{
    if (role == EFS_RAFT_LEADER)
        return "LEADER";
    if (role == EFS_RAFT_CANDIDATE)
        return "CANDIDATE";
    return "FOLLOWER";
}

static int cmd_raft_status(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_raft_status_reply *r;
    uint32_t i;

    if (argc < 1) {
        fprintf(stderr, "usage: raft-status <node:port>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_STATUS, NULL, 0, &reply_type, &reply,
                  &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_STATUS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to get raft-status\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    if (r->rc != EFS_OK) {
        fprintf(stderr, "raft-status rc=%d (host off?)\n", r->rc);
        free(reply);
        return 1;
    }
    printf("node %u root=%llu salt=%llu groups=%u\n",
           r->node_id,
           (unsigned long long)r->kv_has_root,
           (unsigned long long)r->export_salt,
           r->ngroups);
    for (i = 0; i < r->ngroups && i < EFS_RAFT_HOST_MAX_GROUPS; i++) {
        struct efs_raft_group_status *g = &r->groups[i];
        printf("  group %u hosted=%u role=%s leader=%d term=%llu "
               "commit=%llu applied=%llu voters=0x%x joint=%u\n",
               g->group, g->hosted,
               g->hosted ? raft_role_name(g->role) : "-",
               g->leader,
               (unsigned long long)g->term,
               (unsigned long long)g->commit_index,
               (unsigned long long)g->applied_index,
               g->voters, g->joint);
    }
    free(reply);
    return 0;
}

static int raft_cfg_submit(const char *host, uint16_t port, uint8_t group,
                           uint8_t sub, uint32_t voters,
                           struct efs_msg_raft_mkfs_reply *out, int io_ms)
{
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    uint8_t payload[1 + 6];

    payload[0] = group;
    payload[1] = EFS_MD_CMD_CFG;
    payload[2] = sub;
    payload[3] = (uint8_t)(voters >> 24);
    payload[4] = (uint8_t)(voters >> 16);
    payload[5] = (uint8_t)(voters >> 8);
    payload[6] = (uint8_t)voters;
    fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, io_ms);
    efs_set_send_timeout(fd, io_ms);
    if (send_recv(fd, EFS_MSG_RAFT_MKFS, payload, sizeof(payload), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_MKFS_REPLY ||
        reply_len != sizeof(*out)) {
        free(reply);
        close(fd);
        return -1;
    }
    close(fd);
    memcpy(out, reply, sizeof(*out));
    free(reply);
    return 0;
}

static int raft_change_converged(const char *host, uint16_t port, uint8_t group,
                                 uint32_t voters)
{
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_raft_status_reply *r;
    uint32_t i;
    int ok = 0;

    fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return 0;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_STATUS, NULL, 0, &reply_type, &reply,
                  &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_STATUS_REPLY ||
        reply_len != sizeof(*r)) {
        free(reply);
        close(fd);
        return 0;
    }
    close(fd);
    r = reply;
    if (r->rc == EFS_OK) {
        for (i = 0; i < r->ngroups && i < EFS_RAFT_HOST_MAX_GROUPS; i++) {
            if (r->groups[i].group == group && r->groups[i].hosted &&
                r->groups[i].voters == voters && !r->groups[i].joint)
                ok = 1;
        }
    }
    free(reply);
    return ok;
}

static int cmd_raft_change(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    uint8_t group;
    uint32_t voters;
    struct efs_msg_raft_mkfs_reply r;
    int i, rc = EFS_ERR_BUSY;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-change <node:port> <group> <voters>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    group = (uint8_t)strtoul(argv[1], NULL, 0);
    voters = (uint32_t)strtoul(argv[2], NULL, 0);
    if (group != EFS_RAFT_GROUP_SHARD && group != EFS_RAFT_GROUP_SHARD2) {
        fprintf(stderr, "raft-change: group must be 0 or 2\n");
        return 1;
    }
    memset(&r, 0, sizeof(r));
    {
        int lfd = efs_connect_tcp(host, port);
        uint8_t ltype = 0;
        void *lrep = NULL;
        uint32_t llen = 0;

        if (lfd >= 0) {
            efs_set_recv_timeout(lfd, 2000);
            efs_set_send_timeout(lfd, 2000);
            if (send_recv(lfd, EFS_MSG_LIST_NODES, NULL, 0, &ltype, &lrep,
                          &llen) == 0 &&
                ltype == EFS_MSG_LIST_NODES_REPLY &&
                llen == sizeof(struct efs_msg_list_nodes_reply)) {
                struct efs_msg_list_nodes_reply *list = lrep;
                uint32_t n;

                for (n = 0; n < list->node_count; n++)
                    (void)raft_cfg_submit(list->nodes[n].addr,
                                          list->nodes[n].port, group,
                                          EFS_MD_CFG_NOTE, voters, &r, 2000);
            }
            free(lrep);
            close(lfd);
        }
    }
    /* Learner catch-up is BUSY until match_index >= commit (I18). */
    for (i = 0; i < 80; i++) {
        if (raft_cfg_submit(host, port, group, EFS_MD_CFG_CHANGE, voters, &r,
                            EFS_IO_TIMEOUT_MS) != 0) {
            fprintf(stderr, "Failed to raft-change\n");
            return 1;
        }
        rc = r.rc;
        if (rc != EFS_ERR_BUSY)
            break;
        usleep(100000);
    }
    if (rc == EFS_OK) {
        for (i = 0; i < 80 && !raft_change_converged(host, port, group, voters);
             i++)
            usleep(100000);
    }
    printf("raft-change rc=%d group=%u voters=0x%x index=%llu hint=%d\n",
           rc, group, voters, (unsigned long long)r.index, r.leader_hint);
    return rc == EFS_OK ? 0 : 1;
}

static int cmd_raft_mkfs(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_raft_mkfs_reply *r;

    if (argc < 1) {
        fprintf(stderr, "usage: raft-mkfs <node:port>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_MKFS, NULL, 0, &reply_type, &reply,
                  &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_MKFS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-mkfs\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-mkfs rc=%d leader_hint=%d index=%llu salt=%llu\n",
           r->rc, r->leader_hint,
           (unsigned long long)r->index,
           (unsigned long long)r->salt);
    {
        int rc = r->rc;
        free(reply);
        return rc == EFS_OK ? 0 : 1;
    }
}

static void wr32be_mgmt(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64be_mgmt(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/* Layout-epoch on the dir's inode group (RAFT_MKFS submit, no new opcode).
 * status= matches inode RPCs so g0_mgmt retries NOT_PRIMARY. */
static int cmd_raft_dir(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type, kind = 0;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_raft_mkfs_reply *r;
    efs_ino_t ino;
    uint8_t payload[1 + 10];
    uint32_t st;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-dir <node:port> <ino> <begin|migrate|finish>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    if (ino == 0) {
        fprintf(stderr, "raft-dir: ino required\n");
        return 1;
    }
    if (strcmp(argv[2], "begin") == 0)
        kind = EFS_MD_DIR_BEGIN;
    else if (strcmp(argv[2], "migrate") == 0)
        kind = EFS_MD_DIR_MIGRATE;
    else if (strcmp(argv[2], "finish") == 0)
        kind = EFS_MD_DIR_FINISH;
    else {
        fprintf(stderr, "raft-dir: kind must be begin|migrate|finish\n");
        return 1;
    }
    payload[0] = efs_raft_shard_group(efs_kv_inode_shard(ino));
    payload[1] = EFS_MD_CMD_DIR;
    payload[2] = kind;
    wr64be_mgmt(payload + 3, ino);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_MKFS, payload, sizeof(payload), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_MKFS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-dir\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    st = 0;
    if (r->rc == EFS_ERR_NOT_PRIMARY)
        st = 7;
    else if (r->rc == EFS_ERR_NOT_FOUND)
        st = 1;
    else if (r->rc != EFS_OK)
        st = 3;
    printf("raft-dir status=%u rc=%d kind=%s ino=%llu index=%llu\n",
           st, r->rc, argv[2], (unsigned long long)ino,
           (unsigned long long)r->index);
    {
        int rc = r->rc;
        free(reply);
        return rc == EFS_OK ? 0 : 1;
    }
}

static int sess_rpc(const char *host, uint16_t port, const uint8_t *payload,
                    uint32_t plen, struct efs_msg_raft_mkfs_reply *out)
{
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_raft_mkfs_reply *r;

    memset(out, 0, sizeof(*out));
    fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_IO;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_MKFS, payload, plen, &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_MKFS_REPLY ||
        reply_len != sizeof(*r)) {
        free(reply);
        close(fd);
        return EFS_ERR_IO;
    }
    close(fd);
    r = reply;
    *out = *r;
    free(reply);
    return out->rc;
}

static void sess_pack_hdr(uint8_t *payload, uint8_t group, uint8_t sub,
                          const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    payload[0] = group;
    payload[1] = EFS_MD_CMD_SESSION;
    payload[2] = sub;
    memcpy(payload + 3, uuid, EFS_OPID_UUID_LEN);
}

/* Coordinator-driven I23 barrier (10.5c-35c). BEGIN freezes touched[],
 * FENCE_LOC+ACK every set bit, FINISH, then LEASE_DROP(old_epoch) on
 * each touched shard. GET shard>=4096 reads one word of the bitmap. */
static int cmd_raft_session_fence(const char *host, uint16_t port,
                                  const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    uint8_t payload[1 + 26];
    struct efs_msg_raft_mkfs_reply r;
    uint32_t ssh, epoch = 0, old_epoch = 0, state = 0, word, i, ntouch = 0;
    uint32_t shards[4096];
    int rc, st;

    ssh = efs_kv_session_shard(uuid);
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), EFS_MD_SESS_BEGIN, uuid);
    rc = sess_rpc(host, port, payload, 1 + 18, &r);
    if (rc != EFS_OK)
        goto out;
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), 0, uuid);
    wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, ssh);
    rc = sess_rpc(host, port, payload, 1 + 18 + 4, &r);
    if (rc != EFS_OK)
        goto out;
    epoch = (uint32_t)(r.salt & 0xffffffffull);
    state = (uint32_t)((r.salt >> 32) & 0xffull);
    if (state != 2 || epoch == 0) {
        rc = EFS_ERR_BUSY;
        goto out;
    }
    old_epoch = epoch - 1;
    for (word = 0; word < (EFS_SESSION_BITMAP / 8u); word++) {
        uint64_t bits;
        uint32_t b;

        memset(payload, 0, sizeof(payload));
        sess_pack_hdr(payload, efs_raft_shard_group(ssh), 0, uuid);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, EFS_SESSION_BITS + word);
        rc = sess_rpc(host, port, payload, 1 + 18 + 4, &r);
        if (rc != EFS_OK)
            goto out;
        bits = r.salt;
        for (b = 0; b < 64; b++) {
            uint32_t sh;

            if ((bits & (1ull << b)) == 0)
                continue;
            sh = word * 64u + b;
            if (ntouch >= 4096) {
                rc = EFS_ERR_INVAL;
                goto out;
            }
            shards[ntouch++] = sh;
        }
    }
    for (i = 0; i < ntouch; i++) {
        uint32_t sh = shards[i];

        memset(payload, 0, sizeof(payload));
        sess_pack_hdr(payload, efs_raft_shard_group(sh), EFS_MD_SESS_FENCE_LOC,
                      uuid);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, epoch);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN + 4, sh);
        rc = sess_rpc(host, port, payload, 1 + 26, &r);
        if (rc != EFS_OK)
            goto out;
        memset(payload, 0, sizeof(payload));
        sess_pack_hdr(payload, efs_raft_shard_group(ssh), EFS_MD_SESS_ACK, uuid);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, sh);
        rc = sess_rpc(host, port, payload, 1 + 22, &r);
        if (rc != EFS_OK)
            goto out;
    }
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), EFS_MD_SESS_FINISH, uuid);
    rc = sess_rpc(host, port, payload, 1 + 18, &r);
    if (rc != EFS_OK)
        goto out;
    for (i = 0; i < ntouch; i++) {
        uint32_t sh = shards[i];

        memset(payload, 0, sizeof(payload));
        sess_pack_hdr(payload, efs_raft_shard_group(sh), EFS_MD_SESS_LEASE_DROP,
                      uuid);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, old_epoch);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN + 4, sh);
        rc = sess_rpc(host, port, payload, 1 + 26, &r);
        if (rc != EFS_OK)
            goto out;
    }
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), 0, uuid);
    wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, ssh);
    rc = sess_rpc(host, port, payload, 1 + 18 + 4, &r);
    if (rc != EFS_OK)
        goto out;
    epoch = (uint32_t)(r.salt & 0xffffffffull);
    state = (uint32_t)((r.salt >> 32) & 0xffull);
    if (state != 1) {
        rc = EFS_ERR_BUSY;
        goto out;
    }
out:
    st = 0;
    if (rc == EFS_ERR_NOT_PRIMARY)
        st = 7;
    else if (rc == EFS_ERR_NOT_FOUND)
        st = 1;
    else if (rc != EFS_OK)
        st = 3;
    printf("raft-session status=%u rc=%d primary=%d op=fence ssh=%u shard=%u "
           "epoch=%u state=%u touched=%u index=%llu\n",
           st, rc, r.leader_hint >= 0 ? r.leader_hint + 1 : 0, ssh, ntouch,
           epoch, state, ntouch > 0 ? 1u : 0u,
           (unsigned long long)r.index);
    return rc == EFS_OK ? 0 : 1;
}

static int parse_uuid_hex(const char *s, uint8_t uuid[EFS_OPID_UUID_LEN])
{
    int i;

    if (!s || strlen(s) != 2 * EFS_OPID_UUID_LEN)
        return -1;
    for (i = 0; i < EFS_OPID_UUID_LEN; i++) {
        unsigned v = 0;
        char buf[3];

        buf[0] = s[2 * i];
        buf[1] = s[2 * i + 1];
        buf[2] = '\0';
        if (sscanf(buf, "%2x", &v) != 1)
            return -1;
        uuid[i] = (uint8_t)v;
    }
    return 0;
}

/* Session record on hash(uuid) (10.5c-35a). CREATE/REGISTER on the
 * session-authority group; ESTABLISH on the shard's group; GET is a
 * ReadIndex (sub=0, not a log command). Reuses RAFT_MKFS submit. */
static int cmd_raft_session(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type, uuid[EFS_OPID_UUID_LEN], payload[1 + 26];
    void *reply = NULL;
    uint32_t reply_len = 0, plen, ssh, shard = 0, epoch = 1;
    struct efs_msg_raft_mkfs_reply *r;
    const char *op;
    uint8_t sub = 0;
    uint32_t st, touched;

    if (argc < 3) {
        fprintf(stderr,
                "usage: raft-session <node:port> <create|register|establish|get|fence> "
                "<uuid-hex> [epoch|shard] [shard]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    op = argv[1];
    if (parse_uuid_hex(argv[2], uuid) != 0) {
        fprintf(stderr, "raft-session: uuid must be 32 hex chars\n");
        return 1;
    }
    if (strcmp(argv[1], "fence") == 0)
        return cmd_raft_session_fence(host, port, uuid);
    ssh = efs_kv_session_shard(uuid);
    if (strcmp(op, "create") == 0) {
        sub = EFS_MD_SESS_CREATE;
        if (argc >= 4)
            epoch = (uint32_t)strtoul(argv[3], NULL, 0);
        shard = ssh;
    } else if (strcmp(op, "register") == 0) {
        sub = EFS_MD_SESS_REGISTER;
        if (argc < 4) {
            fprintf(stderr, "raft-session register: shard required\n");
            return 1;
        }
        shard = (uint32_t)strtoul(argv[3], NULL, 0);
        if (argc >= 5)
            epoch = (uint32_t)strtoul(argv[4], NULL, 0);
    } else if (strcmp(op, "establish") == 0) {
        sub = EFS_MD_SESS_ESTABLISH;
        if (argc < 4) {
            fprintf(stderr, "raft-session establish: shard required\n");
            return 1;
        }
        shard = (uint32_t)strtoul(argv[3], NULL, 0);
        if (argc >= 5)
            epoch = (uint32_t)strtoul(argv[4], NULL, 0);
    } else if (strcmp(op, "get") == 0) {
        sub = 0;
        if (argc >= 4)
            shard = (uint32_t)strtoul(argv[3], NULL, 0);
        else
            shard = ssh;
    } else {
        fprintf(stderr, "raft-session: op must be create|register|establish|get|fence\n");
        return 1;
    }
    if (shard >= 4096) {
        fprintf(stderr, "raft-session: shard out of range\n");
        return 1;
    }
    memset(payload, 0, sizeof(payload));
    if (sub == EFS_MD_SESS_ESTABLISH)
        payload[0] = efs_raft_shard_group(shard);
    else
        payload[0] = efs_raft_shard_group(ssh);
    payload[1] = EFS_MD_CMD_SESSION;
    payload[2] = sub;
    memcpy(payload + 3, uuid, EFS_OPID_UUID_LEN);
    if (sub == 0) {
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, shard);
        plen = 1 + 18 + 4;
    } else if (sub == EFS_MD_SESS_CREATE) {
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, epoch);
        plen = 1 + 22;
    } else {
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN, epoch);
        wr32be_mgmt(payload + 3 + EFS_OPID_UUID_LEN + 4, shard);
        plen = 1 + 26;
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_MKFS, payload, plen, &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_RAFT_MKFS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-session\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    st = 0;
    if (r->rc == EFS_ERR_NOT_PRIMARY)
        st = 7;
    else if (r->rc == EFS_ERR_NOT_FOUND)
        st = 1;
    else if (r->rc != EFS_OK)
        st = 3;
    touched = (uint32_t)((r->salt >> 40) & 1ull);
    printf("raft-session status=%u rc=%d primary=%d op=%s ssh=%u shard=%u "
           "epoch=%u state=%u touched=%u index=%llu\n",
           st, r->rc, r->leader_hint >= 0 ? r->leader_hint + 1 : 0, op, ssh,
           shard, (uint32_t)(r->salt & 0xffffffffull),
           (uint32_t)((r->salt >> 32) & 0xffull), touched,
           (unsigned long long)r->index);
    {
        int rc = r->rc;
        free(reply);
        return rc == EFS_OK ? 0 : 1;
    }
}

static int cmd_raft_getattr(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_getattr req;
    struct efs_msg_inode_reply *r;
    efs_ino_t ino = EFS_ROOT_INO;

    if (argc < 1) {
        fprintf(stderr, "usage: raft-getattr <node:port> [ino]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    if (argc >= 2)
        ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    memset(&req, 0, sizeof(req));
    req.ino = ino;
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_GETATTR, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_GETATTR_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-getattr\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-getattr status=%u primary=%u ino=%llu mode=0%o nlink=%u "
           "size=%llu mtime=%llu\n",
           r->status, r->primary_id,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink,
           (unsigned long long)r->inode.size,
           (unsigned long long)r->inode.mtime);
    free(reply);
    return 0;
}

static int cmd_raft_lookup(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_lookup req;
    struct efs_msg_inode_reply *r;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-lookup <node:port> <parent> <name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.name, argv[2], sizeof(req.name) - 1);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_LOOKUP, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_LOOKUP_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-lookup\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-lookup status=%u primary=%u parent=%llu name=%s ino=%llu "
           "mode=0%o nlink=%u\n",
           r->status, r->primary_id,
           (unsigned long long)req.parent, req.name,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_create(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_create req;
    struct efs_msg_inode_reply *r;
    uint32_t mode = S_IFREG | 0644;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-create <node:port> <parent> <name> "
                "[mode]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.name, argv[2], sizeof(req.name) - 1);
    if (argc >= 4)
        mode = (uint32_t)strtoul(argv[3], NULL, 0);
    req.mode = mode;
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_CREATE, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_CREATE_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-create\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-create status=%u primary=%u parent=%llu name=%s ino=%llu "
           "mode=0%o nlink=%u\n",
           r->status, r->primary_id,
           (unsigned long long)req.parent, req.name,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_append(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0, slen, epoch = 1;
    struct efs_msg_inode_append req;
    uint8_t buf[sizeof(req) + EFS_SESS_WIRE_LEN];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    struct efs_msg_inode_reply *r;
    int sess = 0;

    if (argc < 3 || (argc != 3 && argc != 5)) {
        fprintf(stderr,
                "usage: raft-append <node:port> <ino> <len> [uuid-hex epoch]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    if (argc >= 5) {
        if (parse_uuid_hex(argv[3], uuid) != 0) {
            fprintf(stderr, "raft-append: uuid must be 32 hex chars\n");
            return 1;
        }
        epoch = (uint32_t)strtoul(argv[4], NULL, 0);
        sess = 1;
    }
    memset(&req, 0, sizeof(req));
    req.ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    req.len = strtoull(argv[2], NULL, 0);
    memcpy(buf, &req, sizeof(req));
    slen = sizeof(req);
    if (sess) {
        memcpy(buf + slen, uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_APPEND, buf, slen, &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_APPEND_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-append\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-append status=%u primary=%u ino=%llu size=%llu\n",
           r->status, r->primary_id,
           (unsigned long long)r->inode.ino,
           (unsigned long long)r->inode.size);
    free(reply);
    return 0;
}

static int cmd_raft_hold(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type, buf[sizeof(struct efs_msg_inode_hold) + EFS_SESS_WIRE_LEN];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    void *reply = NULL;
    uint32_t reply_len = 0, slen, epoch;
    struct efs_msg_inode_hold req;
    struct efs_msg_inode_reply *r;
    const char *op;
    const char *usage =
        "usage: raft-hold <node:port> <ino> <open|close> [owner] [uuid-hex epoch]\n";

    if (argc < 3 || argc == 5 || argc > 6) {
        fprintf(stderr, "%s", usage);
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    op = argv[2];
    if (strcmp(op, "open") != 0 && strcmp(op, "close") != 0) {
        fprintf(stderr, "%s", usage);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    req.flags = (strcmp(op, "open") == 0) ? 1u : 0u;
    req.owner = (argc >= 4) ? strtoull(argv[3], NULL, 0) : 1ull;
    memcpy(buf, &req, sizeof(req));
    slen = sizeof(req);
    if (argc >= 6) {
        if (parse_uuid_hex(argv[4], uuid) != 0) {
            fprintf(stderr, "raft-hold: uuid must be 32 hex chars\n");
            return 1;
        }
        epoch = (uint32_t)strtoul(argv[5], NULL, 0);
        memcpy(buf + slen, uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_HOLD, buf, slen, &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_HOLD_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-hold\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-hold status=%u primary=%u ino=%llu nlink=%u\n",
           r->status, r->primary_id,
           (unsigned long long)r->inode.ino, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_lock_op(int argc, char **argv, uint32_t extra, const char *name)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_flock req;
    uint8_t buf[sizeof(req) + EFS_FLOCK_RANGE_LEN + EFS_SESS_WIRE_LEN];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t slen, epoch = 1;
    struct efs_msg_inode_reply *r;
    const char *op;
    const char *usage =
        "usage: raft-%s <node:port> <ino> <ex|sh|un|gex|gsh|wex|wsh> [owner] [start end] [uuid-hex epoch]\n";
    int is_getlk = 0, n, sess = 0;

    if (argc < 3) {
        fprintf(stderr, usage, name);
        return 1;
    }
    n = argc;
    if (n >= 5 && parse_uuid_hex(argv[n - 2], uuid) == 0) {
        sess = 1;
        epoch = (uint32_t)strtoul(argv[n - 1], NULL, 0);
        n -= 2;
    }
    if (n < 3 || n == 5 || n > 6) {
        fprintf(stderr, usage, name);
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    op = argv[2];
    memset(&req, 0, sizeof(req));
    req.ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    if (strcmp(op, "ex") == 0)
        req.op = EFS_FLOCK_EX | EFS_FLOCK_NB | extra;
    else if (strcmp(op, "sh") == 0)
        req.op = EFS_FLOCK_SH | EFS_FLOCK_NB | extra;
    else if (strcmp(op, "un") == 0)
        req.op = EFS_FLOCK_UN | extra;
    else if (strcmp(op, "gex") == 0) {
        req.op = EFS_FLOCK_GETLK | EFS_FLOCK_EX | extra;
        is_getlk = 1;
    } else if (strcmp(op, "gsh") == 0) {
        req.op = EFS_FLOCK_GETLK | EFS_FLOCK_SH | extra;
        is_getlk = 1;
    } else if (strcmp(op, "wex") == 0) {
        /* Blocking wait: held at the leader until grantable. */
        req.op = EFS_FLOCK_EX | EFS_FLOCK_WAIT | extra;
    } else if (strcmp(op, "wsh") == 0) {
        req.op = EFS_FLOCK_SH | EFS_FLOCK_WAIT | extra;
    } else {
        fprintf(stderr, usage, name);
        return 1;
    }
    req.owner = (n >= 4) ? strtoull(argv[3], NULL, 0) : 1ull;
    memcpy(buf, &req, sizeof(req));
    slen = sizeof(req);
    if (n >= 6) {
        uint64_t start = strtoull(argv[4], NULL, 0);
        uint64_t end = strtoull(argv[5], NULL, 0);

        memcpy(buf + sizeof(req), &start, 8);
        memcpy(buf + sizeof(req) + 8, &end, 8);
        slen += EFS_FLOCK_RANGE_LEN;
    }
    if (sess) {
        memcpy(buf + slen, uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_FLOCK, buf, slen, &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_FLOCK_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-%s\n", name);
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    if (is_getlk) {
        const char *t = r->inode.nlink == EFS_LOCK_EX ? "ex"
                        : r->inode.nlink == EFS_LOCK_SH ? "sh" : "un";

        printf("raft-%s status=%u primary=%u type=%s owner=%llu start=%llu "
               "end=%llu\n",
               name, r->status, r->primary_id, t,
               (unsigned long long)r->inode.ino,
               (unsigned long long)r->inode.size,
               (unsigned long long)r->inode.ctime);
    } else {
        printf("raft-%s status=%u primary=%u ino=%llu\n",
               name, r->status, r->primary_id,
               (unsigned long long)r->inode.ino);
    }
    free(reply);
    return 0;
}

static int cmd_raft_flock(int argc, char **argv)
{
    return cmd_raft_lock_op(argc, argv, 0, "flock");
}

static int cmd_raft_fcntl(int argc, char **argv)
{
    return cmd_raft_lock_op(argc, argv, EFS_FLOCK_FCNTL, "fcntl");
}

static int cmd_raft_unlink(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_unlink req;
    struct efs_msg_inode_reply *r;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-unlink <node:port> <parent> <name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.name, argv[2], sizeof(req.name) - 1);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_UNLINK, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_UNLINK_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-unlink\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-unlink status=%u primary=%u parent=%llu name=%s ino=%llu "
           "mode=0%o nlink=%u\n",
           r->status, r->primary_id,
           (unsigned long long)req.parent, req.name,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_rmdir(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_unlink req;
    struct efs_msg_inode_reply *r;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-rmdir <node:port> <parent> <name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.name, argv[2], sizeof(req.name) - 1);
    req.is_dir = 1;
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_UNLINK, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_UNLINK_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-rmdir\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-rmdir status=%u primary=%u parent=%llu name=%s ino=%llu "
           "mode=0%o nlink=%u\n",
           r->status, r->primary_id,
           (unsigned long long)req.parent, req.name,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_link(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_link req;
    struct efs_msg_inode_reply *r;

    if (argc < 4) {
        fprintf(stderr, "usage: raft-link <node:port> <src_ino> <parent> <name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.src_ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    req.new_parent = (efs_ino_t)strtoull(argv[2], NULL, 0);
    strncpy(req.new_name, argv[3], sizeof(req.new_name) - 1);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_LINK, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_LINK_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-link\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-link status=%u primary=%u src=%llu parent=%llu name=%s "
           "ino=%llu mode=0%o nlink=%u\n",
           r->status, r->primary_id, (unsigned long long)req.src_ino,
           (unsigned long long)req.new_parent, req.new_name,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_setattr(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_setattr req;
    struct efs_msg_inode_reply *r;

    if (argc < 3) {
        fprintf(stderr, "usage: raft-setattr <node:port> <ino> <mask> "
                "[mode|mtime|size] [uid|atime] [gid]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    req.mask = (uint32_t)strtoul(argv[2], NULL, 0);
    if (req.mask & EFS_SETATTR_SIZE) {
        if (argc >= 4)
            req.size = strtoull(argv[3], NULL, 0);
    } else if (req.mask & (EFS_SETATTR_MTIME | EFS_SETATTR_ATIME)) {
        if (argc >= 4)
            req.mtime = strtoull(argv[3], NULL, 0);
        if (argc >= 5)
            req.atime = strtoull(argv[4], NULL, 0);
    } else {
        if (argc >= 4)
            req.mode = (uint32_t)strtoul(argv[3], NULL, 0);
        if (argc >= 5)
            req.uid = (uint32_t)strtoul(argv[4], NULL, 0);
        if (argc >= 6)
            req.gid = (uint32_t)strtoul(argv[5], NULL, 0);
    }
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_SETATTR, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_SETATTR_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-setattr\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-setattr status=%u primary=%u ino=%llu mask=%u mode=0%o "
           "nlink=%u size=%llu mtime=%llu\n",
           r->status, r->primary_id,
           (unsigned long long)r->inode.ino, req.mask, r->inode.mode,
           r->inode.nlink, (unsigned long long)r->inode.size,
           (unsigned long long)r->inode.mtime);
    free(reply);
    return 0;
}

static int cmd_raft_rename(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_rename_at req;
    struct efs_msg_inode_reply *r;

    if (argc < 5) {
        fprintf(stderr, "usage: raft-rename <node:port> <old_parent> <old_name> "
                "<new_parent> <new_name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.old_parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.old_name, argv[2], sizeof(req.old_name) - 1);
    req.new_parent = (efs_ino_t)strtoull(argv[3], NULL, 0);
    strncpy(req.new_name, argv[4], sizeof(req.new_name) - 1);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_RENAME_AT, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_RENAME_AT_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-rename\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-rename status=%u primary=%u old_parent=%llu old=%s "
           "new_parent=%llu new=%s ino=%llu mode=0%o nlink=%u\n",
           r->status, r->primary_id,
           (unsigned long long)req.old_parent, req.old_name,
           (unsigned long long)req.new_parent, req.new_name,
           (unsigned long long)r->inode.ino, r->inode.mode, r->inode.nlink);
    free(reply);
    return 0;
}

static int cmd_raft_readdir(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_readdir req;
    struct efs_msg_inode_readdir_reply *r;
    uint32_t i;

    if (argc < 2) {
        fprintf(stderr, "usage: raft-readdir <node:port> <parent> [after_ino]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    req.max_ents = EFS_READDIR_MAX;
    if (argc >= 3)
        req.after_ino = strtoull(argv[2], NULL, 0);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_READDIR, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_READDIR_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-readdir\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-readdir status=%u count=%u names=", r->status, r->count);
    for (i = 0; i < r->count; i++) {
        if (i)
            printf(",");
        printf("%s", r->ents[i].name);
    }
    printf("\n");
    free(reply);
    return 0;
}

static int cmd_raft_lookup_path(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_lookup_path req;
    struct efs_msg_inode_lookup_path_reply *r;

    if (argc < 2) {
        fprintf(stderr, "usage: raft-lookup-path <node:port> <path> [start]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    strncpy(req.path, argv[1], sizeof(req.path) - 1);
    if (argc >= 3)
        req.start = (efs_ino_t)strtoull(argv[2], NULL, 0);
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_LOOKUP_PATH, &req, sizeof(req),
                  &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_LOOKUP_PATH_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-lookup-path\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-lookup-path status=%u primary=%u path=%s start=%llu "
           "ino=%llu name=%s mode=0%o ancestors=%u\n",
           r->status, r->primary_id, req.path,
           (unsigned long long)req.start,
           (unsigned long long)r->inode.ino, r->inode.name, r->inode.mode,
           r->ancestor_count);
    free(reply);
    return 0;
}

static int cmd_raft_publish(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd, i;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct {
        struct efs_msg_report_chunks hdr;
        struct efs_chunk_rec rec;
        struct efs_ino_size_rec sz;
    } req;
    uint8_t buf[sizeof(struct efs_msg_report_chunks) + sizeof(struct efs_chunk_rec) +
                sizeof(struct efs_ino_size_rec)];
    uint32_t send_len;
    struct efs_msg_inode_reply *r;

    if (argc < 2) {
        fprintf(stderr, "usage: raft-publish <node:port> <ino> [chunk] [size]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.hdr.count = 1;
    req.hdr.ino_count = 1;
    req.rec.ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    if (argc >= 3)
        req.rec.chunk_index = (uint32_t)strtoul(argv[2], NULL, 0);
    req.sz.ino = req.rec.ino;
    req.sz.size = EFS_MIN_CHUNK_SIZE;
    if (argc >= 4)
        req.sz.size = strtoull(argv[3], NULL, 0);
    req.rec.nodes[0] = 1;
    req.rec.nodes[1] = 2;
    req.rec.nodes[2] = 3;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        memset(req.rec.checksums[i], (uint8_t)(0xa5 + i), EFS_HASH_SIZE);
    send_len = (uint32_t)(sizeof(req.hdr) + sizeof(req.rec) + sizeof(req.sz));
    memcpy(buf, &req.hdr, sizeof(req.hdr));
    memcpy(buf + sizeof(req.hdr), &req.rec, sizeof(req.rec));
    memcpy(buf + sizeof(req.hdr) + sizeof(req.rec), &req.sz, sizeof(req.sz));
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_REPORT_CHUNKS, buf, send_len, &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_REPORT_CHUNKS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-publish\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-publish status=%u primary=%u ino=%llu ci=%u size=%llu\n",
           r->status, r->primary_id, (unsigned long long)req.rec.ino,
           req.rec.chunk_index, (unsigned long long)req.sz.size);
    free(reply);
    return 0;
}

static int cmd_raft_getchunks(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint32_t i;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_inode_getchunks req;
    struct efs_msg_inode_getchunks_reply *r;

    if (argc < 2) {
        fprintf(stderr, "usage: raft-getchunks <node:port> <ino> [start]\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.ino = (efs_ino_t)strtoull(argv[1], NULL, 0);
    if (argc >= 3)
        req.start = (uint32_t)strtoul(argv[2], NULL, 0);
    req.max = EFS_GETCHUNKS_MAX;
    fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_GETCHUNKS, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_GETCHUNKS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to raft-getchunks\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-getchunks status=%u primary=%u ino=%llu count=%u cis=",
           r->status, r->primary_id, (unsigned long long)req.ino, r->count);
    for (i = 0; i < r->count; i++) {
        if (i)
            printf(",");
        printf("%u", r->recs[i].chunk_index);
    }
    printf("\n");
    free(reply);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
    fprintf(stderr, "Usage: %s <command> [args]\n"
                    "Commands:\n"
                    "  status <node:port>\n"
                    "  mkfs <node:port> <export-name>\n"
                    "  add-node <new-node:port> <existing-node:port>\n"
                    "  shrink-quota <node:port> <amount>[T|G|M|K]\n"
                    "  add-storage <node:port> <path>[,path...]\n"
                    "  raft-status <node:port>\n"
                    "  raft-mkfs <node:port>\n"
                    "  raft-change <node:port> <group> <voters>\n"
                    "  raft-dir <node:port> <ino> <begin|migrate|finish>\n"
                    "  raft-session <node:port> <create|register|establish|get|fence> <uuid-hex> [epoch|shard] [shard]\n"
                    "  raft-getattr <node:port> [ino]\n"
                    "  raft-lookup <node:port> <parent> <name>\n"
                    "  raft-create <node:port> <parent> <name> [mode]\n"
                    "  raft-unlink <node:port> <parent> <name>\n"
                    "  raft-rmdir <node:port> <parent> <name>\n"
                    "  raft-link <node:port> <src_ino> <parent> <name>\n"
                    "  raft-setattr <node:port> <ino> <mask> [mode|mtime|size] [uid|atime] [gid]\n"
                    "  raft-rename <node:port> <old_parent> <old_name> <new_parent> <new_name>\n"
                    "  raft-readdir <node:port> <parent> [after_ino]\n"
                    "  raft-lookup-path <node:port> <path> [start]\n"
                    "  raft-publish <node:port> <ino> [chunk] [size]\n"
                    "  raft-getchunks <node:port> <ino> [start]\n"
                    "  raft-append <node:port> <ino> <len> [uuid-hex epoch]\n"
                    "  raft-hold <node:port> <ino> <open|close> [owner] [uuid-hex epoch]\n"
                    "  raft-flock <node:port> <ino> <ex|sh|un|gex|gsh> [owner] [uuid-hex epoch]\n"
                    "  raft-fcntl <node:port> <ino> <ex|sh|un|gex|gsh> [owner] [start end] [uuid-hex epoch]\n",
            argv[0]);
    return 1;
}

    const char *cmd = argv[1];
    if (strcmp(cmd, "status") == 0)
        return cmd_status(argc - 2, argv + 2);
    if (strcmp(cmd, "add-node") == 0)
        return cmd_add_node(argc - 2, argv + 2);
    if (strcmp(cmd, "shrink-quota") == 0)
        return cmd_shrink_quota(argc - 2, argv + 2);
    if (strcmp(cmd, "add-storage") == 0)
        return cmd_add_storage(argc - 2, argv + 2);
    /* One metadata engine: mkfs is the raft mkfs. */
    if (strcmp(cmd, "mkfs") == 0)
        return cmd_raft_mkfs(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-status") == 0)
        return cmd_raft_status(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-mkfs") == 0)
        return cmd_raft_mkfs(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-change") == 0)
        return cmd_raft_change(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-dir") == 0)
        return cmd_raft_dir(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-session") == 0)
        return cmd_raft_session(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-getattr") == 0)
        return cmd_raft_getattr(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-lookup") == 0)
        return cmd_raft_lookup(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-create") == 0)
        return cmd_raft_create(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-unlink") == 0)
        return cmd_raft_unlink(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-rmdir") == 0)
        return cmd_raft_rmdir(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-link") == 0)
        return cmd_raft_link(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-setattr") == 0)
        return cmd_raft_setattr(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-rename") == 0)
        return cmd_raft_rename(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-readdir") == 0)
        return cmd_raft_readdir(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-lookup-path") == 0)
        return cmd_raft_lookup_path(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-publish") == 0)
        return cmd_raft_publish(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-getchunks") == 0)
        return cmd_raft_getchunks(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-append") == 0)
        return cmd_raft_append(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-hold") == 0)
        return cmd_raft_hold(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-flock") == 0)
        return cmd_raft_flock(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-fcntl") == 0)
        return cmd_raft_fcntl(argc - 2, argv + 2);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
