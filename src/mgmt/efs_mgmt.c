#include "../common/parse_host_port.h"
#include <errno.h>
#include "efs/cli_help.h"
#include "efs/common.h"
#include "efs/lock.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/membership.h"
#include "efs/opid.h"
#include "efs/placement.h"
#include "efs/session.h"
#include "efs/session_revoke.h"
#include "efs/txn.h"
#include "efs/version.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int parse_host_port(const char *str, char *host, size_t host_len, uint16_t *port)
{
    return efs_parse_host_port(str, host, host_len, port);
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

/* One request/reply with a member under the short bound: connect,
 * declaration and reply, not efs_connect_declared's. reply_len is exact. */
static int probe_node_rpc(const char *nhost, uint16_t nport, uint8_t msg, uint8_t want,
                          void *out, uint32_t reply_len)
{
    int pfd = efs_connect_tcp(nhost, nport);
    if (pfd < 0)
        return 0;
    efs_set_recv_timeout(pfd, EFS_STATUS_PROBE_MS);
    efs_set_send_timeout(pfd, EFS_STATUS_PROBE_MS);
    if (efs_fd_declare(pfd) != EFS_OK) {
        close(pfd);
        return 0;
    }
    uint8_t type = 0;
    void *preply = NULL;
    uint32_t preply_len = 0;
    int ok = 0;
    if (efs_send_msg(pfd, msg, NULL, 0) == 0 &&
        efs_recv_msg(pfd, &type, &preply, &preply_len) == 0 &&
        type == want && preply_len == reply_len) {
        if (out)
            memcpy(out, preply, reply_len);
        ok = 1;
    }
    free(preply);
    close(pfd);
    return ok;
}

static int probe_node_up(const char *nhost, uint16_t nport, struct efs_msg_status_reply *status)
{
    struct efs_msg_status_reply scratch;
    return probe_node_rpc(nhost, nport, EFS_MSG_STATUS, EFS_MSG_STATUS_REPLY,
                          status ? status : &scratch, sizeof(scratch));
}

/* Build hosts and timestamps are diagnostics, not source identity. Keep the
 * commit/dirty version and branch; unknown builds cannot establish equality. */
static void status_version_identity(const char *version, char *identity, size_t size)
{
    snprintf(identity, size, "%s", version);
    char *built = strstr(identity, ", built ");
    if (built && !strstr(identity, "no git metadata")) {
        built[0] = ')'; built[1] = '\0';
    }
}

/* One member's RAFT_STATUS reply, or -1 when it cannot be read. */
static int raft_status_fetch(const char *host, uint16_t port,
                             struct efs_msg_raft_status_reply *out)
{
    if (!probe_node_rpc(host, port, EFS_MSG_RAFT_STATUS, EFS_MSG_RAFT_STATUS_REPLY,
                        out, sizeof(*out)))
        return -1;
    return out->rc == EFS_OK ? 0 : -1;
}

/* A9/D42: actual metadata leader placement, as each member reports its own
 * role (elections only; no placement policy moves leaders). A group with no
 * self-reported leader, or with two in different terms, is shown as such. */
static void status_leader_placement(const struct efs_node *nodes, const uint8_t *up, uint32_t n)
{
    struct efs_msg_raft_status_reply *r = calloc(1, sizeof(*r));
    if (!r)
        return;
    uint32_t led[EFS_RAFT_HOST_MAX_GROUPS] = {0}, ngroups = 0, reachable = 0;
    uint32_t leader_of[EFS_RAFT_HOST_MAX_GROUPS][4], group_id[EFS_RAFT_HOST_MAX_GROUPS];
    uint64_t term_of[EFS_RAFT_HOST_MAX_GROUPS][4];
    uint32_t leads[EFS_MAX_NODES] = {0};
    printf("Metadata leader placement (self-reported):\n");
    for (uint32_t i = 0; i < n; i++) {
        if (!up[i] || raft_status_fetch(nodes[i].addr, nodes[i].port, r) != 0) {
            printf("  node %u: %s\n", nodes[i].id, up[i] ? "unreachable" : "down");
            continue;
        }
        reachable++;
        char list[96] = "";
        size_t used = 0;
        for (uint32_t g = 0; g < r->ngroups && g < EFS_RAFT_HOST_MAX_GROUPS; g++) {
            const struct efs_raft_group_status *gs = &r->groups[g];
            if (g >= ngroups) ngroups = g + 1;
            group_id[g] = gs->group;
            if (!gs->hosted || gs->role != EFS_RAFT_LEADER)
                continue;
            if (led[g] < 4) {
                leader_of[g][led[g]] = nodes[i].id;
                term_of[g][led[g]] = gs->term;
            }
            led[g]++;
            leads[i]++;
            used += (size_t)snprintf(list + used, used < sizeof(list) ? sizeof(list) - used : 0,
                                     "%sgroup %u (term %llu)", used ? ", " : "", gs->group,
                                     (unsigned long long)gs->term);
        }
        printf("  node %u: leads %s\n", nodes[i].id, used ? list : "none");
    }
    uint32_t distinct = 0, have = 0;
    for (uint32_t i = 0; i < n; i++)
        distinct += leads[i] ? 1 : 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        if (led[g] == 1)
            have++;
        else if (led[g] == 0)
            printf("  group %u: no leader reported\n", group_id[g]);
        else
            for (uint32_t k = 0; k < led[g] && k < 4; k++)
                printf("  group %u: leader claim by node %u term %llu\n", group_id[g],
                       leader_of[g][k], (unsigned long long)term_of[g][k]);
    }
    printf("  leaders: %u/%u groups on %u distinct node(s); %u/%u members reported\n",
           have, ngroups, distinct, reachable, n);
    free(r);
}

static int cmd_status(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage:\n  efs-mgmt status <node:port>\n");
        return 1;
    }
    char host[64];
    uint16_t port;
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }

    int fd = efs_connect_declared(host, port);
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
    int all_have_capacity = (list->node_count > 0);
    /* W42: per-node capacity and available space of the UP nodes, fed to
     * efs_capacity_logical (same model as FUSE statfs). */
    uint64_t cap_quota[EFS_MAX_NODES], cap_room[EFS_MAX_NODES];
    uint32_t ncap = 0;
    /* W154: each group's leader reports its degraded-mark count. */
    uint64_t degraded[2] = {0, 0}, rebuilt = 0, rebuild_failed = 0, rebuild_retired = 0;
    uint32_t degraded_seen = 0;
    /* D31: each group's leader reports its abandoned-upload records. */
    uint64_t orphans[2] = {0, 0}, orphan_collected = 0, orphan_deleted = 0, orphan_failed = 0;
    uint32_t orphan_seen = 0;
    uint8_t node_up[EFS_MAX_NODES] = {0};
    for (uint32_t i = 0; i < list->node_count; i++) {
        /* Probe every advertised addr (may differ from the seed string we used). */
        struct efs_msg_status_reply current = {0};
        int up = probe_node_up(list->nodes[i].addr, list->nodes[i].port, &current);
        if (up) {
            list->nodes[i].used = current.used;
            list->nodes[i].quota = current.quota;
            for (int g = 0; g < 2; g++) {
                if (!(current.degraded_sampled & (1u << g)))
                    continue;
                if (!(degraded_seen & (1u << g)) || current.degraded_chunks[g] > degraded[g])
                    degraded[g] = current.degraded_chunks[g];
                degraded_seen |= 1u << g;
            }
            rebuilt += current.rebuilt_fragments;
            rebuild_failed += current.rebuild_failures;
            rebuild_retired += current.rebuild_retired;
            for (int g = 0; g < 2; g++) {
                if (!(current.put_orphan_sampled & (1u << g)))
                    continue;
                if (!(orphan_seen & (1u << g)) || current.put_orphan_pending[g] > orphans[g])
                    orphans[g] = current.put_orphan_pending[g];
                orphan_seen |= 1u << g;
            }
            orphan_collected += current.put_orphan_collected;
            orphan_deleted += current.put_orphan_deleted;
            orphan_failed += current.put_orphan_failures;
        }
        if (up)
            up_nodes++;
        else
            down_nodes++;
        if (i < EFS_MAX_NODES)
            node_up[i] = (uint8_t)up;

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
        } else {
            printf("  node %u: %s:%u  %s  used=%s  quota=unlimited  storage(%u):\n",
                   list->nodes[i].id, list->nodes[i].addr, list->nodes[i].port,
                   reach, used_str, npaths);
        }
        /* B5: exact per-member bytes for machine consumers (the line
         * above rounds); capacity/available are the member's own STATUS
         * values (filesystem bounded by its quota), the inputs of the
         * usable-capacity model below. */
        if (up)
            printf("      bytes: used=%llu quota=%llu capacity=%llu available=%llu valid=%d\n",
                   (unsigned long long)current.used, (unsigned long long)current.quota,
                   (unsigned long long)current.capacity_bytes,
                   (unsigned long long)current.available_bytes, current.capacity_valid ? 1 : 0);
        if (up && current.capacity_valid && ncap < EFS_MAX_NODES) {
            cap_quota[ncap] = current.capacity_bytes;
            cap_room[ncap++] = current.available_bytes;
        } else if (up && current.quota && ncap < EFS_MAX_NODES) {
            cap_quota[ncap] = current.quota;
            cap_room[ncap++] = current.quota > current.used ? current.quota - current.used : 0;
        } else {
            all_have_capacity = 0;
        }
        char paths[EFS_MAX_PATH];
        strncpy(paths, list->nodes[i].storage_path, sizeof(paths) - 1);
        paths[sizeof(paths) - 1] = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(paths, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save))
            printf("      %s\n", tok);
    }

    /* 2+1 cluster EC: a chunk's three fragments sit on three distinct
     * nodes, so usable capacity is the 3-of-N bound in
     * efs_capacity_logical (2 × min quota on three nodes, Σ × 2/3 on N
     * equal ones) — the same model FUSE df uses, over the nodes that are
     * up. Fewer than three up: nothing can be written; the state line
     * says why. */
    if (all_have_capacity && ncap >= EFS_NUM_FRAGMENTS) {
        uint64_t usable_cap = efs_capacity_logical(cap_quota, ncap);
        uint64_t usable_free = efs_capacity_logical(cap_room, ncap);
        if (usable_free > usable_cap)
            usable_free = usable_cap;
        uint64_t usable_used =
            (usable_cap > usable_free) ? (usable_cap - usable_free) : 0;
        char used_s[32], free_s[32], cap_s[32];
        format_bytes(usable_used, used_s, sizeof(used_s));
        format_bytes(usable_free, free_s, sizeof(free_s));
        format_bytes(usable_cap, cap_s, sizeof(cap_s));
        double pct = 100.0 * (double)usable_used / (double)usable_cap;
        printf("Usable (2+1 logical): used=%s  free=%s  capacity=%s (%.1f%%; filesystem capacity)\n",
               used_s, free_s, cap_s, pct);
        /* Machine consumers must not reconstruct bytes from rounded TiB. */
        printf("Usable bytes: used=%llu free=%llu capacity=%llu\n",
               (unsigned long long)usable_used, (unsigned long long)usable_free,
               (unsigned long long)usable_cap);
    } else if (down_nodes > 0) {
        printf("Usable (2+1 logical): degraded (%d/%u nodes up)\n",
               up_nodes, list->node_count);
    } else {
        printf("Usable (2+1 logical): n/a (storage capacity unavailable)\n");
    }

    const char *cluster_state = "OK";
    uint64_t degraded_total = degraded[0] + degraded[1];
    if (up_nodes < 2)
        cluster_state = "UNAVAILABLE";
    else if (down_nodes > 0)
        cluster_state = "DEGRADED";
    else if (full_nodes >= 2)
        cluster_state = "FULL";
    else if (degraded_seen == 3u && degraded_total)
        cluster_state = "DEGRADED"; /* every node up, protection not restored yet */
    printf("Cluster state: %s (%d up, %d down)\n", cluster_state, up_nodes,
           down_nodes);
    /* Stripes acknowledged with two of three fragments, awaiting rebuild
     * when the missing node is reachable (W154). Unknown until both
     * metadata groups' leaders have reported a pass. */
    if (degraded_seen == 3u)
        printf("Degraded chunks: %llu (group 0: %llu, group 2: %llu); rebuilt=%llu "
               "retired=%llu failures=%llu\n",
               (unsigned long long)(degraded[0] + degraded[1]),
               (unsigned long long)degraded[0], (unsigned long long)degraded[1],
               (unsigned long long)rebuilt, (unsigned long long)rebuild_retired,
               (unsigned long long)rebuild_failed);
    else
        printf("Degraded chunks: unknown (group leaders reported: %s%s%s); rebuilt=%llu "
               "retired=%llu failures=%llu\n",
               degraded_seen & 1u ? "0" : "", degraded_seen == 3u ? "," : "",
               degraded_seen & 2u ? "2" : (degraded_seen ? "" : "none"),
               (unsigned long long)rebuilt, (unsigned long long)rebuild_retired,
               (unsigned long long)rebuild_failed);

    /* K35 (D42): a stripe on two durable fragments survives no further
     * loss until the rebuild on return restores its third. */
    if (degraded_seen != 3u)
        printf("Protection: unknown (degraded-chunk count not reported by both groups)\n");
    else if (degraded_total)
        printf("Protection: DEGRADED: %llu chunks on two of three fragments, no fault "
               "tolerance until rebuilt (W154 rebuilds when the missing node is up)\n",
               (unsigned long long)degraded_total);
    else
        printf("Protection: full (2+1: every chunk tolerates one lost node)\n");

    /* Abandoned ticketed uploads awaiting revocation-gated collection on
     * every member that holds a fragment (D31). Unknown until both group
     * leaders finished a scan of their orphan ledger. */
    if (orphan_seen == 3u)
        printf("Abandoned uploads: pending=%llu (group 0: %llu, group 2: %llu); collected=%llu "
               "fragments_deleted=%llu failures=%llu\n",
               (unsigned long long)(orphans[0] + orphans[1]),
               (unsigned long long)orphans[0], (unsigned long long)orphans[1],
               (unsigned long long)orphan_collected, (unsigned long long)orphan_deleted,
               (unsigned long long)orphan_failed);
    else
        printf("Abandoned uploads: pending=unknown (group leaders reported: %s%s%s); "
               "collected=%llu fragments_deleted=%llu failures=%llu\n",
               orphan_seen & 1u ? "0" : "", orphan_seen == 3u ? "," : "",
               orphan_seen & 2u ? "2" : (orphan_seen ? "" : "none"),
               (unsigned long long)orphan_collected, (unsigned long long)orphan_deleted,
               (unsigned long long)orphan_failed);

    uint32_t ncopy = list->node_count;
    if (ncopy > EFS_MAX_NODES)
        ncopy = EFS_MAX_NODES;
    struct efs_node nodes_copy[EFS_MAX_NODES];
    memcpy(nodes_copy, list->nodes, ncopy * sizeof(nodes_copy[0]));

    free(reply);
    reply = NULL;

    /* Build identity across the cluster: probe VERSION on every node.
     * Identical everywhere is the norm; after a partial deploy a mismatch
     * deserves one line per node. The reply ends in "on <host>)" — the
     * answering node, not the build — so identity is compared without it. */
    {
        char vers[EFS_MAX_NODES][EFS_VERSION_REPLY_LEN];
        int have[EFS_MAX_NODES] = {0};
        uint32_t nprobed = 0, nsame = 0, first = UINT32_MAX;
        for (uint32_t i = 0; i < ncopy; i++) {
            int vfd = efs_connect_declared(nodes_copy[i].addr, nodes_copy[i].port);
            if (vfd < 0)
                continue;
            efs_set_recv_timeout(vfd, EFS_STATUS_PROBE_MS);
            efs_set_send_timeout(vfd, EFS_STATUS_PROBE_MS);
            uint8_t vt = 0;
            void *vp = NULL;
            uint32_t vl = 0;
            if (efs_send_msg(vfd, EFS_MSG_VERSION, NULL, 0) == 0 &&
                efs_recv_msg(vfd, &vt, &vp, &vl) == 0 &&
                vt == EFS_MSG_VERSION_REPLY &&
                vl == sizeof(struct efs_msg_version_reply)) {
                struct efs_msg_version_reply *vr = vp;
                vr->version[sizeof(vr->version) - 1] = '\0';
                snprintf(vers[i], sizeof(vers[i]), "%s", vr->version);
                have[i] = 1;
                nprobed++;
            }
            free(vp);
            close(vfd);
        }
        char ident[EFS_MAX_NODES][EFS_VERSION_REPLY_LEN];
        for (uint32_t i = 0; i < ncopy; i++) {
            ident[i][0] = '\0';
            if (have[i])
                status_version_identity(vers[i], ident[i], sizeof(ident[i]));
        }
        for (uint32_t i = 0; i < ncopy; i++) {
            if (!have[i])
                continue;
            if (first == UINT32_MAX)
                first = i;
            else if (strcmp(ident[i], ident[first]) == 0)
                nsame++;
        }
        if (nprobed > 0 && nsame == nprobed - 1 && nprobed == ncopy) {
            printf("Versions: %s — %u/%u identical\n", ident[first],
                   nprobed, ncopy);
        } else if (nprobed > 0 && nsame == nprobed - 1) {
            printf("Versions: %s — %u/%u identical (%u unreachable)\n",
                   ident[first], nprobed, ncopy, ncopy - nprobed);
        } else {
            printf("Versions: MISMATCH (%u/%u probed)\n", nprobed, ncopy);
            for (uint32_t i = 0; i < ncopy; i++)
                printf("  node %u: %s\n", nodes_copy[i].id,
                       have[i] ? vers[i] : "unreachable");
        }
    }

    status_leader_placement(nodes_copy, node_up, ncopy);

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

    close(fd);
    return (down_nodes > 0) ? 1 : 0;
}

static int cmd_add_node(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage:\n  efs-mgmt add-node <new-node:port> <existing-node:port>\n");
        return 1;
    }
    char new_host[64], existing_host[64];
    uint16_t new_port, existing_port;
    if (parse_host_port(argv[0], new_host, sizeof(new_host), &new_port) != 0 ||
        parse_host_port(argv[1], existing_host, sizeof(existing_host), &existing_port) != 0) {
        fprintf(stderr, "Invalid addresses\n");
        return 1;
    }

    int fd = efs_connect_declared(new_host, new_port);
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
        fprintf(stderr, "Usage:\n  efs-mgmt shrink-quota <node:port> <amount>[T|G|M|K]\n");
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

    int fd = efs_connect_declared(host, port);
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
        printf("Reduced running quota on %s:%u by %s\n",
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
        fprintf(stderr, "Usage:\n  efs-mgmt add-storage <node:port> <path>[,path...]\n");
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

    int fd = efs_connect_declared(host, port);
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

static int cmd_io_stats(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_io_stats_reply *r;
    static const char *names[EFS_IO_STATS_CLASSES] = {
        "get_chunk", "put_chunk", "disk_write", "disk_read", "gc_delete"
    };
    uint32_t i;

    if (argc < 1) {
        fprintf(stderr, "Usage:\n  efs-mgmt io-stats <node:port>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_IO_STATS, NULL, 0, &reply_type, &reply,
                  &reply_len) != 0 ||
        reply_type != EFS_MSG_IO_STATS_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to get io-stats\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("io-stats %s:%u uptime_s=%llu\n", host, port,
           (unsigned long long)(r->uptime_us / 1000000ull));
    for (i = 0; i < EFS_IO_STATS_CLASSES; i++) {
        struct efs_io_stats_class *c = &r->cls[i];
        printf("  %-10s ops=%llu bytes=%llu errors=%llu us_sum=%llu avg_us=%llu "
               "p50_us=%llu max_us=%llu\n",
               names[i],
               (unsigned long long)c->ops,
               (unsigned long long)c->bytes,
               (unsigned long long)c->errors,
               (unsigned long long)c->us_sum,
               c->ops ? (unsigned long long)(c->us_sum / c->ops) : 0ull,
               (unsigned long long)c->p50_us,
               (unsigned long long)c->us_max);
    }
    printf("  rdma       conns=%llu data_tx_frames=%llu data_tx_bytes=%llu data_rx_frames=%llu "
           "data_rx_bytes=%llu credit_stalls=%llu\n",
           (unsigned long long)r->rdma.live_conns,
           (unsigned long long)r->rdma.data_frames_tx, (unsigned long long)r->rdma.data_bytes_tx,
           (unsigned long long)r->rdma.data_frames_rx, (unsigned long long)r->rdma.data_bytes_rx,
           (unsigned long long)r->rdma.credit_stalls);
    free(reply);
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    int fd;
    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    struct efs_msg_version_reply *r;

    if (argc < 1) {
        fprintf(stderr, "Usage:\n  efs-mgmt version <node:port>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_VERSION, NULL, 0, &reply_type, &reply,
                  &reply_len) != 0 ||
        reply_type != EFS_MSG_VERSION_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to get version\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    r->version[sizeof(r->version) - 1] = '\0';
    printf("version %s:%u %s\n", host, port, r->version);
    free(reply);
    return 0;
}

#if EFS_BUILD_EXPIRY
static void expiry_duration(uint64_t ns,char *out,size_t n);
#endif

static int cmd_gc_status_mode(int argc, char **argv,int expiry_only)
{
    char host[64];
    uint16_t port;
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t len = 0;
    if (argc != 1 || parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Usage:\n  efs-mgmt %s <node:port>\n",expiry_only?"expiry-status":"gc-status");
        return 1;
    }
    int fd = efs_connect_declared(host, port);
    if (fd < 0)
        return 1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    int rc = send_recv(fd, EFS_MSG_GC_STATUS, NULL, 0, &type, &payload, &len);
    close(fd);
    /* Only the current reply layout (version 5) is read. */
    if (rc || type != EFS_MSG_GC_STATUS_REPLY ||
        len != sizeof(struct efs_msg_gc_status_reply) ||
        ((struct efs_msg_gc_status_reply *)payload)->version != 5) {
        fprintf(stderr, "GC status unavailable: no current-format reply\n");
        free(payload);
        return 1;
    }
    struct efs_msg_gc_status_reply *g = payload;
#if EFS_BUILD_EXPIRY
    if(expiry_only) {
        if(!g->expiry.available) {
            fprintf(stderr,"Expiry worker status unavailable on this node.\n");free(payload);return 1;
        }
        const struct efs_expiry_status *e=&g->expiry;char bytes[64],age[80];
        const char *state=!e->enabled?"disabled":e->uncertain?"paused: clock uncertain":!e->healthy?"waiting: clock health sample":
                          !e->leader_mask?"standby: no local leader group":"running";
        format_bytes(e->namespace_bytes,bytes,sizeof(bytes));
        printf("Expiry worker: %s\nClock: %s%s\nConfirmed namespace removals: %llu files; %s observed logical bytes\n"
               "Counters: since process start; size_unknown=%llu refused/retry=%llu errors=%llu passes=%llu\n",state,
               e->healthy?"healthy":"not healthy",e->uncertain?" (operator reset required)":"",
               (unsigned long long)e->removed_files,bytes,(unsigned long long)e->size_unknown_files,(unsigned long long)e->refused,
               (unsigned long long)e->errors,(unsigned long long)e->passes);
        if(e->last_delete_age_ms==UINT64_MAX)printf("Last successful removal: none in this process\n");
        else {expiry_duration(e->last_delete_age_ms*1000000ull,age,sizeof(age));printf("Last successful removal: %s ago\n",age);}
        for(unsigned i=0;i<2;i++) {
            if(!(e->sampled_mask&(1u<<i))) {printf("Group %u backlog: not sampled on a current local leader\n",i*2);continue;}
            expiry_duration(e->sample_age_ms[i]*1000000ull,age,sizeof(age));
            printf("Group %u overdue: %s%llu at sample, as of %s ago (bounded, non-atomic sample)\n",i*2,
                   e->due_capped[i]?"at least ":"",(unsigned long long)e->due_lower_bound[i],age);
        }
        printf("Physical reclamation: separate GC totals; no expiry-only physical byte attribution\n");
        free(payload);return 0;
    }
#else
    (void)expiry_only;
#endif
    printf("node=%u stage=%u group=%u exports=%u passes=%llu elapsed_ms=%llu "
           "last_pass_age_ms=%llu last_delete_age_ms=%llu\n",
           g->node_id, g->stage, g->group, g->exports, (unsigned long long)g->passes,
           (unsigned long long)g->pass_elapsed_ms,
           (unsigned long long)g->last_pass_age_ms,
           (unsigned long long)g->last_delete_age_ms);
    printf(
        "data_usage_bytes=%llu removed_fragments=%llu unlinked_file_bytes=%llu reclaimed_payload_bytes=%llu "
        "delete_errors=%llu mismatches=%llu sweep_errors=%llu reap_errors=%llu "
        "ack_errors=%llu scan_errors=%llu missing_exports=%llu\n",
        (unsigned long long)g->data_usage_bytes,
        (unsigned long long)g->removed_fragments,
        (unsigned long long)g->removed_file_bytes,
        (unsigned long long)g->removed_payload_bytes,
        (unsigned long long)g->delete_errors, (unsigned long long)g->mismatches,
        (unsigned long long)g->sweep_errors, (unsigned long long)g->reap_errors,
        (unsigned long long)g->ack_errors, (unsigned long long)g->scan_errors,
        (unsigned long long)g->missing_exports);
    for (unsigned i = 0; i < 2; i++)
        printf("group=%u sampled=%u pending_estimate=%llu reap_seen=%llu orphan_seen=%llu capped=%u "
               "first_reap_ino=%llu observed_age_ms=%llu\n",
               i * 2, (g->sampled_mask >> i) & 1, (unsigned long long)g->pending[i],
               (unsigned long long)g->reap_seen[i], (unsigned long long)g->orphan_seen[i], g->reap_capped[i],
               (unsigned long long)g->first_reap[i],
               (unsigned long long)g->first_seen_age_ms[i]);
    printf("trunc_pending group0=%llu group2=%llu\n",
           (unsigned long long)g->trunc_pending[0], (unsigned long long)g->trunc_pending[1]);
#if EFS_BUILD_EXPIRY
    {
        const struct efs_expiry_status *e=&g->expiry;
        const char *state=!e->available?"unavailable":!e->enabled?"disabled":
                          e->uncertain?"paused_clock":!e->healthy?"warming_clock":!e->leader_mask?"standby":"running";
        printf("expiry_state=%s expiry_available=%u expiry_enabled=%u expiry_clock_healthy=%u expiry_clock_uncertain=%u "
               "expiry_leader_mask=%u expired_files=%llu expired_namespace_bytes=%llu "
               "expired_size_unknown_files=%llu expiry_refused=%llu expiry_errors=%llu expiry_passes=%llu "
               "expiry_last_pass_age_ms=%llu expiry_last_delete_age_ms=%llu scope=since_process_start\n",state,
               e->available,e->enabled,e->healthy,e->uncertain,e->leader_mask,
               (unsigned long long)e->removed_files,(unsigned long long)e->namespace_bytes,
               (unsigned long long)e->size_unknown_files,(unsigned long long)e->refused,(unsigned long long)e->errors,(unsigned long long)e->passes,
               (unsigned long long)e->last_pass_age_ms,(unsigned long long)e->last_delete_age_ms);
        for(unsigned i=0;i<2;i++)printf("expiry_group=%u expiry_sampled=%u overdue_lower_bound=%llu "
               "overdue_capped=%u expiry_sample_age_ms=%llu\n",i*2,(e->sampled_mask>>i)&1,
               (unsigned long long)e->due_lower_bound[i],e->due_capped[i],(unsigned long long)e->sample_age_ms[i]);
    }
#endif
    {
        const struct efs_reclaim_status *c=&g->reclaim;
        printf("reclaim_state=%s reclaim_accepted=%llu reclaim_deleted=%llu reclaim_checkpointed=%llu "
               "reclaim_retired=%llu reclaim_busy=%llu journal_records=%llu journal_segments=%llu "
               "journal_epoch=%016llx reclaim_pending_bytes=%llu reclaim_credited_bytes=%llu "
               "phys_avail_bytes=%llu phys_refused=%llu reclaim_recovered=%llu\n",
               c->fail_closed?(c->recovering?"fail_closed_recovering":"fail_closed"):"ok",
               (unsigned long long)c->accepted,(unsigned long long)c->deleted,
               (unsigned long long)c->checkpointed,(unsigned long long)c->retired,(unsigned long long)c->busy,
               (unsigned long long)c->journal_records,(unsigned long long)c->journal_segments,
               (unsigned long long)c->journal_epoch,(unsigned long long)c->pending_bytes,
               (unsigned long long)c->credited_bytes,(unsigned long long)c->phys_avail_bytes,
               (unsigned long long)c->phys_refused,(unsigned long long)c->recovered);
    }
    {
        const char *names[] = {"checksum_read", "unlink", "dir_sync"};
        for (unsigned i = 0; i < EFS_GC_IO_CLASSES; i++) {
            struct efs_gc_io_stats *c = &g->io[i];
            printf("gc_io=%s ops=%llu bytes=%llu errors=%llu us_sum=%llu avg_us=%llu\n",
                   names[i], (unsigned long long)c->ops, (unsigned long long)c->bytes,
                   (unsigned long long)c->errors, (unsigned long long)c->us_sum,
                   c->ops ? (unsigned long long)(c->us_sum / c->ops) : 0ull);
        }
    }
    free(payload);
    return 0;
}

/* W125: compaction now, on every node (each applies it to the metadata
 * groups it leads); segments 0 = report the fragmentation summary only. */
static int cmd_pack_compact(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    if (argc < 1 || argc > 2 || parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Usage:\n  efs-mgmt pack-compact <node:port> [segments]   (default 64; 0 = summary only)\n");
        return 1;
    }
    struct efs_msg_pack_compact req = {.segments = argc == 2 ? (uint32_t)strtoul(argv[1], NULL, 10) : 64u};
    int fd = efs_connect_declared(host, port);
    if (fd < 0) { fprintf(stderr, "Cannot connect to %s:%u\n", host, port); return 1; }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t len = 0;
    int rc = send_recv(fd, EFS_MSG_LIST_NODES, NULL, 0, &type, &payload, &len);
    close(fd);
    if (rc || type != EFS_MSG_LIST_NODES_REPLY || len != sizeof(struct efs_msg_list_nodes_reply)) {
        fprintf(stderr, "Failed to get node list\n");
        free(payload);
        return 1;
    }
    struct efs_msg_list_nodes_reply *list = payload;
    uint32_t led = 0;
    for (uint32_t i = 0; i < list->node_count; i++) {
        const struct efs_node *n = &list->nodes[i];
        int nfd = efs_connect_declared(n->addr, n->port);
        void *r = NULL;
        uint32_t rl = 0;
        uint8_t rt = 0;
        if (nfd < 0) { printf("node %u %s:%u: unreachable\n", n->id, n->addr, n->port); continue; }
        efs_set_recv_timeout(nfd, EFS_IO_TIMEOUT_MS);
        rc = send_recv(nfd, EFS_MSG_PACK_COMPACT, &req, sizeof(req), &rt, &r, &rl);
        close(nfd);
        if (rc || rt != EFS_MSG_PACK_COMPACT_REPLY || rl != sizeof(struct efs_msg_pack_compact_reply)) {
            printf("node %u %s:%u: no current-format pack-compact reply\n", n->id, n->addr, n->port);
            free(r);
            continue;
        }
        const struct efs_msg_pack_compact_reply *x = r;
        if (x->led_groups) {
            led |= x->led_groups;
            printf("node %u %s:%u: leads group(s)%s%s, %s %u segments; segments=%llu live=%llu KiB dead=%llu KiB "
                   "holed=%llu underfilled=%llu compacted_total=%llu\n", n->id, n->addr, n->port,
                   x->led_groups & 1 ? " 0" : "", x->led_groups & 2 ? " 2" : "",
                   req.segments ? "queued" : "summary only,", req.segments,
                   (unsigned long long)x->segments, (unsigned long long)(x->live_bytes >> 10),
                   (unsigned long long)(x->dead_bytes >> 10), (unsigned long long)x->holes,
                   (unsigned long long)x->small, (unsigned long long)x->compacted_total);
        } else {
            printf("node %u %s:%u: leads no group\n", n->id, n->addr, n->port);
        }
        /* Node-wide since start, across every group it led; errors and
         * backlog waits are otherwise invisible to operators. */
        const struct efs_packer_status *p = &x->packer;
        printf("  packer: passes=%llu candidates=%llu built=%llu files_packed=%llu cancelled=%llu "
               "unindexed=%llu backfilled=%llu read_errors=%llu put_errors=%llu admit_errors=%llu "
               "abandoned=%llu busy_passes=%llu budget_waits=%llu abandon_scanned=%llu "
               "abandon_actionable=%llu abandon_refused=%llu base_segments=%llu base_files=%llu\n",
               (unsigned long long)p->passes, (unsigned long long)p->candidates,
               (unsigned long long)p->segments, (unsigned long long)p->files_packed,
               (unsigned long long)p->files_cancelled, (unsigned long long)p->unindexed,
               (unsigned long long)p->backfilled, (unsigned long long)p->read_errors,
               (unsigned long long)p->put_errors, (unsigned long long)p->admit_errors,
               (unsigned long long)p->abandoned, (unsigned long long)p->busy_passes,
               (unsigned long long)p->budget_waits, (unsigned long long)p->abandon_scanned,
               (unsigned long long)p->abandon_actionable, (unsigned long long)p->abandon_refused,
               (unsigned long long)p->base_segments, (unsigned long long)p->base_files);
        free(r);
    }
    free(payload);
    if (led != 3) fprintf(stderr, "warning: not every group's leader answered (led mask %u)\n", led);
    return led ? 0 : 1;
}

static int cmd_gc_status(int argc,char **argv) {return cmd_gc_status_mode(argc,argv,0);}
#if EFS_BUILD_EXPIRY
static int cmd_expiry_status(int argc,char **argv) {return cmd_gc_status_mode(argc,argv,1);}
#endif

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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-status <node:port>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    fd = efs_connect_declared(host, port);
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
    /* K22/W23: the node's memory/lag bound. */
    printf("  bound kv_mt_bytes=%llu kv_mem_budget=%llu kv_l0_bytes=%llu kv_l0_cap=%llu "
           "kv_apply_deferred=%llu admit_busy=%llu\n",
           (unsigned long long)r->kv_mt_bytes, (unsigned long long)r->kv_mem_budget,
           (unsigned long long)r->kv_l0_bytes, (unsigned long long)r->kv_l0_cap,
           (unsigned long long)r->kv_apply_deferred, (unsigned long long)r->admit_busy);
    /* Leader placement as this node sees it (node ids; 0 = unknown). */
    printf("  leaders");
    for (i = 0; i < r->ngroups && i < EFS_RAFT_HOST_MAX_GROUPS; i++)
        printf(" group%u=node%d", r->groups[i].group,
               r->groups[i].leader >= 0 ? r->groups[i].leader + 1 : 0);
    printf("\n");
    for (i = 0; i < r->ngroups && i < EFS_RAFT_HOST_MAX_GROUPS; i++) {
        struct efs_raft_group_status *g = &r->groups[i];
        printf("  group %u hosted=%u role=%s leader=%d term=%llu "
               "commit=%llu applied=%llu voters=0x%x joint=%u retired=%u "
               "last=%llu last_term=%llu snap=%llu rejects_sent=%llu proto_drops=%llu "
               "unapplied=%llu max_unapplied=%llu busy_sent=%llu admit_busy=%llu "
               "low_capacity=%llu campaigns_held=%llu capacity_stepdowns=%llu%s\n",
               g->group, g->hosted,
               g->hosted ? raft_role_name(g->role) : "-",
               g->leader,
               (unsigned long long)g->term,
               (unsigned long long)g->commit_index,
               (unsigned long long)g->applied_index,
               g->voters, g->joint, g->retired,
               (unsigned long long)g->last_log_index,
               (unsigned long long)g->last_log_term,
               (unsigned long long)g->snap_index,
               (unsigned long long)g->rejects_sent,
               (unsigned long long)g->proto_drops,
               (unsigned long long)g->unapplied,
               (unsigned long long)g->max_unapplied,
               (unsigned long long)g->busy_sent,
               (unsigned long long)g->admit_busy,
               (unsigned long long)g->low_capacity,
               (unsigned long long)g->campaigns_held,
               (unsigned long long)g->capacity_stepdowns,
               g->resync_pending ? " resync=1" : "");
        /* W145: replication progress per peer, as this node sees it. */
        for (uint32_t p = 0; g->hosted && g->role == EFS_RAFT_LEADER &&
                             p < g->npeers && p < EFS_RAFT_STATUS_PEERS; p++) {
            const struct efs_raft_peer_status *ps = &g->peers[p];
            char ia[24], aa[24];

            if (ps->inflight_age == UINT64_MAX) snprintf(ia, sizeof(ia), "-");
            else snprintf(ia, sizeof(ia), "%llu", (unsigned long long)ps->inflight_age);
            if (ps->ack_age == UINT64_MAX) snprintf(aa, sizeof(aa), "-");
            else snprintf(aa, sizeof(aa), "%llu", (unsigned long long)ps->ack_age);
            printf("    peer %u next=%llu match=%llu inflight=%llu..%llu inflight_age=%s "
                   "ack_age=%s sent=%llu again=%llu blocked=%llu errors=%llu "
                   "rejects=%llu reject_hint=%llu busy=%llu busy_hold=%llu\n", p,
                   (unsigned long long)ps->next_index,
                   (unsigned long long)ps->match_index,
                   (unsigned long long)ps->inflight,
                   (unsigned long long)ps->inflight_end, ia, aa,
                   (unsigned long long)ps->batches_sent,
                   (unsigned long long)ps->send_again,
                   (unsigned long long)ps->send_blocked,
                   (unsigned long long)ps->send_errors,
                   (unsigned long long)ps->rejects,
                   (unsigned long long)ps->reject_hint,
                   (unsigned long long)ps->busy,
                   (unsigned long long)ps->busy_hold);
        }
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
    fd = efs_connect_declared(host, port);
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

    fd = efs_connect_declared(host, port);
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

/* W145 repair: ask one node to resync a group it follows from the
 * leader's snapshot. Only for a follower whose committed log conflicts
 * with the leader (raft-status: proto_drops rising); refused on a leader. */
static int cmd_raft_resync(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    struct efs_msg_raft_resync req;
    struct efs_msg_raft_resync_reply *r;
    void *reply = NULL;
    uint32_t reply_len = 0;
    uint8_t reply_type = 0;
    int fd;

    if (argc != 2 || parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Usage:\n  efs-mgmt raft-resync <node:port> <group>\n"
                "  Resync GROUP on that node from its leader's snapshot (follower only).\n");
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.group = (uint8_t)strtoul(argv[1], NULL, 10);
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_RESYNC, &req, sizeof(req), &reply_type, &reply,
                  &reply_len) != 0 || reply_type != EFS_MSG_RAFT_RESYNC_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to request raft-resync\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-resync %s:%u group=%u rc=%d%s\n", host, port, req.group, r->rc,
           r->rc == EFS_ERR_NOT_PRIMARY ? " (this node leads the group; refused)" :
           r->rc == EFS_OK ? " (requested; the next conflicting AppendEntries asks for a snapshot)" : "");
    int out = r->rc == EFS_OK ? 0 : 1;
    free(reply);
    return out;
}

/* W64 failover harness: make the node stop leading GROUP. With TARGET (a
 * node-id) it is a leadership transfer; BUSY means the target is not caught
 * up or the group is frozen — retry. Refused (NOT_PRIMARY) on a non-leader. */
static int cmd_raft_step_down(int argc, char **argv)
{
    char host[64];
    uint16_t port;
    struct efs_msg_raft_step_down req;
    struct efs_msg_raft_step_down_reply *r;
    void *reply = NULL;
    uint32_t reply_len = 0;
    uint8_t reply_type = 0;
    int fd;

    if (argc < 2 || argc > 3 || parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Usage:\n  efs-mgmt raft-step-down <node:port> <group> [target-node-id]\n"
                "  Make that node stop leading GROUP now (transfer to TARGET when given).\n");
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.group = (uint8_t)strtoul(argv[1], NULL, 10);
    req.target = -1;
    if (argc == 3) {
        long node = strtol(argv[2], NULL, 10);
        if (node < 1 || node > EFS_MAX_NODES) {
            fprintf(stderr, "raft-step-down: target node-id must be 1..%d\n", EFS_MAX_NODES);
            return 1;
        }
        req.target = (int8_t)(node - 1);
    }
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_RAFT_STEP_DOWN, &req, sizeof(req), &reply_type, &reply,
                  &reply_len) != 0 || reply_type != EFS_MSG_RAFT_STEP_DOWN_REPLY ||
        reply_len != sizeof(*r)) {
        fprintf(stderr, "Failed to request raft-step-down\n");
        free(reply);
        close(fd);
        return 1;
    }
    close(fd);
    r = reply;
    printf("raft-step-down %s:%u group=%u target=%d rc=%d raft_id=%d term=%llu%s\n", host, port,
           req.group, req.target, r->rc, r->raft_id, (unsigned long long)r->term,
           r->rc == EFS_ERR_NOT_PRIMARY ? " (this node does not lead the group; refused)" :
           r->rc == EFS_ERR_BUSY ? " (target not caught up or group frozen; retry)" :
           r->rc == EFS_OK ? " (stepped down)" : "");
    int out = r->rc == EFS_OK ? 0 : 1;
    free(reply);
    return out;
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-change <node:port> <group> <voters>\n");
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
        int lfd = efs_connect_declared(host, port);
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

    if (argc != 1) {
        fprintf(stderr, "Usage:\n  efs-mgmt mkfs|raft-mkfs <node:port>\n"
                        "mkfs initializes the single cluster export and takes no label.\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    /* K33: formation is automatic; this asks the node to form now. Before
     * formation the seed proposes with the stores joined so far (at least
     * three members in all) and forms once each promised; the daemon names
     * what it waits for in its log and refuses with UNFORMED. */
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
    printf("raft-mkfs rc=%d (%s) leader_hint=%d index=%llu salt=%llu\n",
           r->rc, efs_strerror(r->rc), r->leader_hint,
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-dir <node:port> <ino> <begin|migrate|finish>\n");
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
    fd = efs_connect_declared(host, port);
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

/* Follow explicit non-applied redirects using advertised addresses, not an
 * assumed port layout. Never retry an ambiguous transport failure. */
static int sess_leader_address(const char *host, uint16_t port, int hint,
                               char target[64], uint16_t *target_port)
{
    if (hint < 0 || hint >= EFS_MAX_NODES) return EFS_ERR_NOT_PRIMARY;
    int fd=efs_connect_declared(host,port);if(fd<0)return EFS_ERR_IO;
    efs_set_recv_timeout(fd,EFS_IO_TIMEOUT_MS);efs_set_send_timeout(fd,EFS_IO_TIMEOUT_MS);
    uint8_t type;void *payload=NULL;uint32_t len=0;
    int rc=send_recv(fd,EFS_MSG_LIST_NODES,NULL,0,&type,&payload,&len);close(fd);
    if(rc || type!=EFS_MSG_LIST_NODES_REPLY || len!=sizeof(struct efs_msg_list_nodes_reply)) {
        free(payload);return EFS_ERR_IO;
    }
    const struct efs_msg_list_nodes_reply *list=payload;int found=0;
    rc=EFS_ERR_NOT_PRIMARY;
    if(list->node_count>EFS_MAX_NODES) {free(payload);return EFS_ERR_PROTO;}
    for(uint32_t i=0;i<list->node_count;i++)if(list->nodes[i].id==(uint32_t)hint+1) {
        if(found || !list->nodes[i].port || !list->nodes[i].addr[0] ||
           !memchr(list->nodes[i].addr,0,sizeof(list->nodes[i].addr))) {rc=EFS_ERR_PROTO;break;}
        snprintf(target,64,"%s",list->nodes[i].addr);*target_port=list->nodes[i].port;
        found=1;rc=EFS_OK;
    }
    free(payload);return rc;
}
static int sess_rpc(const char *host, uint16_t port, const uint8_t *payload,
                    uint32_t plen, struct efs_msg_raft_mkfs_reply *out)
{
    char target[64];snprintf(target,sizeof(target),"%s",host);uint16_t target_port=port;
    memset(out,0,sizeof(*out));out->leader_hint=-1;
    for(unsigned attempt=0;attempt<8;attempt++) {
        int fd=efs_connect_declared(target,target_port);
        if(fd<0){out->rc=EFS_ERR_IO;return out->rc;}
        efs_set_recv_timeout(fd,EFS_IO_TIMEOUT_MS);efs_set_send_timeout(fd,EFS_IO_TIMEOUT_MS);
        uint8_t type;void *reply=NULL;uint32_t len=0;
        int rc=send_recv(fd,EFS_MSG_RAFT_MKFS,payload,plen,&type,&reply,&len);close(fd);
        if(rc || type!=EFS_MSG_RAFT_MKFS_REPLY || len!=sizeof(*out)) {
            free(reply);out->rc=EFS_ERR_IO;return out->rc;
        }
        memcpy(out,reply,sizeof(*out));free(reply);
        /* Fresh session reads and exact-floor fences can be BUSY while a
         * cold participant catches up. These requests are idempotent with
         * unchanged bytes. BEGIN and cleanup selection retain their one
         * definite-result requirement, as do all transport failures. */
        if(out->rc==EFS_ERR_BUSY && efs_session_busy_retry_ok(payload,plen)) {
            usleep(50000);
            continue;
        }
        if(out->rc!=EFS_ERR_NOT_PRIMARY)return out->rc;
        if(out->leader_hint<0) {
            /* Explicit NOT_PRIMARY means this request was not proposed.
             * After cold rejoin the owner may not advertise a leader yet;
             * retry the seed within the same finite admission budget. Never
             * retry transport failures or an ambiguous submitted command. */
            snprintf(target,sizeof(target),"%s",host);target_port=port;
            usleep(50000);
            continue;
        }
        rc=sess_leader_address(host,port,out->leader_hint,target,&target_port);
        if(rc!=EFS_OK){out->rc=rc;return rc;}
    }
    return out->rc;
}

static int cmd_commands_policy(int argc,char **argv)
{
    if((argc!=2 && argc!=4) ||
       (strcmp(argv[1],"get") && strcmp(argv[1],"set")) ||
       (!strcmp(argv[1],"get") && argc!=2) ||
       (!strcmp(argv[1],"set") && argc!=4)) {
        fprintf(stderr,"Usage: efs-mgmt commands-policy node:port get | set MASK EXPECTED_VERSION\n");return 1;
    }
    char host[64];uint16_t port;
    if(parse_host_port(argv[0],host,sizeof(host),&port))return 1;
    uint8_t payload[14]={efs_raft_shard_group(1),EFS_MD_REQ_QUERY_POLICY};
    uint32_t length=2;
    if(argc==4) {
        char *end=NULL;errno=0;unsigned long mask=strtoul(argv[2],&end,10);
        if(errno || !argv[2][0] || argv[2][0]=='-' || *end || mask>3)return 1;
        errno=0;unsigned long long version=strtoull(argv[3],&end,10);
        if(errno || !argv[3][0] || argv[3][0]=='-' || *end)return 1;
        payload[1]=EFS_MD_CMD_QUERY_POLICY;wr64be_mgmt(payload+2,version);
        wr32be_mgmt(payload+10,(uint32_t)mask);length=14;
    }
    struct efs_msg_raft_mkfs_reply reply;int rc=sess_rpc(host,port,payload,length,&reply);
    printf("commands-policy rc=%d version=%llu mask=%llu leader_hint=%d\n",rc,
           (unsigned long long)reply.index,(unsigned long long)reply.salt,reply.leader_hint);
    return rc?1:0;
}

#if EFS_BUILD_EXPIRY
static int cmd_expiry_clock(int argc,char **argv)
{
    int reset=argc==2 && !strcmp(argv[1],"reset");
    if(argc!=2 || (!reset && strcmp(argv[1],"get"))) {
        fprintf(stderr,"Usage:\n  efs-mgmt expiry-clock NODE:PORT get|reset\n\n"
                       "Inspect this node's current clock health. Reset requires operator uid 0\n"
                       "after clock synchronization is restored; expiry resumes only on healthy samples.\n");
        return 1;
    }
    char host[64];uint16_t port;
    if(parse_host_port(argv[0],host,sizeof(host),&port))return 1;
    uint8_t payload[6]={0,reset?EFS_MD_REQ_EXPIRY_CLOCK_RESET:EFS_MD_REQ_EXPIRY_CLOCK};
    if(reset)wr32be_mgmt(payload+2,(uint32_t)geteuid());
    struct efs_msg_raft_mkfs_reply r;int rc=sess_rpc(host,port,payload,reset?6:2,&r);
    printf("expiry-clock rc=%d enabled=%s healthy=%s uncertain=%s wall_ns=%llu monotonic_ns=%llu\n",
        rc,(r.index&4)?"yes":"no",(r.index&1)?"yes":"no",(r.index&2)?"yes":"no",
        (unsigned long long)r.salt,(unsigned long long)r.term);
    return rc?1:0;
}

static int expiry_u64(const char *text,uint64_t *out)
{
    char *end=NULL;errno=0;unsigned long long n=strtoull(text,&end,10);
    if(errno || !text[0] || text[0]=='-' || *end)return -1;
    *out=n;return 0;
}

static int cmd_expiry_id(int argc,char **argv)
{
    if(argc!=2){fprintf(stderr,"Usage:\n  efs-mgmt expiry-id NODE:PORT INO\n\nInspect the authoritative FileID before setting a policy or deadline.\n");return 1;}
    char host[64];uint16_t port;uint64_t ino;
    if(parse_host_port(argv[0],host,sizeof(host),&port) || expiry_u64(argv[1],&ino) || !ino)return 1;
    uint8_t payload[10]={0,EFS_MD_REQ_EXPIRY_ID};payload[0]=efs_raft_shard_group(efs_kv_inode_shard(ino));
    wr64be_mgmt(payload+2,ino);struct efs_msg_raft_mkfs_reply r;int rc=sess_rpc(host,port,payload,sizeof(payload),&r);
    printf("expiry-id rc=%d ino=%llu generation=%llu uid=%u mode=0%o nlink=%llu leader_hint=%d\n",
        rc,(unsigned long long)ino,(unsigned long long)r.index,(uint32_t)r.salt,(uint32_t)(r.salt>>32),
        (unsigned long long)r.term,r.leader_hint);return rc?1:0;
}

static int cmd_expiry_enable(int argc,char **argv)
{
    if(argc!=6) {
        fprintf(stderr,"Usage:\n  efs-mgmt expiry-enable NODE:PORT INO GENERATION PARENT NAME DEADLINE_NS\n\n"
                       "Enable a deadline on an existing singly linked regular file.\n"
                       "Requires current file ownership and an enabled containing scratch policy.\n"
                       "NAME is one filename; DEADLINE_NS is an absolute Unix nanosecond value.\n");
        return 1;
    }
    char host[64];uint16_t port;uint64_t ino,gen,parent,deadline;
    size_t n=strlen(argv[4]);
    if(parse_host_port(argv[0],host,sizeof(host),&port) ||
       expiry_u64(argv[1],&ino) || expiry_u64(argv[2],&gen) ||
       expiry_u64(argv[3],&parent) || expiry_u64(argv[5],&deadline) ||
       !ino || !gen || !parent || !deadline || !n || n>=EFS_MAX_NAME || strchr(argv[4],'/'))return 1;
    uint8_t payload[55+EFS_MAX_NAME]={0};
    payload[0]=efs_raft_shard_group(efs_kv_inode_shard(ino));payload[1]=EFS_MD_REQ_EXPIRY_ENABLE;
    wr64be_mgmt(payload+2,ino);wr64be_mgmt(payload+10,gen);
    wr32be_mgmt(payload+18,(uint32_t)geteuid());wr64be_mgmt(payload+38,deadline);
    wr64be_mgmt(payload+46,parent);payload[54]=(uint8_t)n;memcpy(payload+55,argv[4],n);
    struct efs_msg_raft_mkfs_reply r;int rc=sess_rpc(host,port,payload,(uint32_t)(55+n),&r);
    printf("expiry-enable rc=%d revision=%llu deadline_ns=%llu root_ino=%llu leader_hint=%d\n",
        rc,(unsigned long long)r.index,(unsigned long long)r.salt,(unsigned long long)r.term,r.leader_hint);
    return rc?1:0;
}

static void expiry_duration(uint64_t ns,char *out,size_t n)
{
    uint64_t sec=ns/1000000000ull;
    if(sec>=86400)snprintf(out,n,"%llu days %llu hours",(unsigned long long)(sec/86400),(unsigned long long)(sec/3600%24));
    else if(sec>=3600)snprintf(out,n,"%llu hours %llu minutes",(unsigned long long)(sec/3600),(unsigned long long)(sec/60%60));
    else if(sec>=60)snprintf(out,n,"%llu minutes %llu seconds",(unsigned long long)(sec/60),(unsigned long long)(sec%60));
    else snprintf(out,n,"%llu seconds",(unsigned long long)sec);
}

static int expiry_read(const char *host,uint16_t port,uint8_t tag,uint64_t ino,uint64_t gen,
                       struct efs_msg_raft_mkfs_reply *r)
{
    uint8_t payload[18]={efs_raft_shard_group(efs_kv_inode_shard(ino)),tag};
    wr64be_mgmt(payload+2,ino);wr64be_mgmt(payload+10,gen);
    return sess_rpc(host,port,payload,(tag==EFS_MD_REQ_EXPIRY_ID || tag==EFS_MD_REQ_EXPIRY_PARENT)?10:18,r);
}

static int expiry_lookup(const char *host,uint16_t port,const char *path,struct efs_inode *inode)
{
    struct efs_msg_inode_lookup_path req={0};
    if(strlen(path)>=sizeof(req.path))return EFS_ERR_OVERFLOW;
    memcpy(req.path,path,strlen(path)+1);
    char target[64];snprintf(target,sizeof(target),"%s",host);uint16_t target_port=port;
    int rc=EFS_ERR_NOT_PRIMARY;
    /* A node that is not the namespace leader answers NOT_PRIMARY with the
     * leader as primary_id (host_namespace_leader_hint); follow it within
     * the same budget as sess_rpc. Not following it failed `expiry show`
     * whenever the seed was not leader (posix_expiry, after mkfs). A read
     * is idempotent; transport failures stay terminal. */
    for(unsigned attempt=0;attempt<8;attempt++) {
        int fd=efs_connect_declared(target,target_port);if(fd<0)return EFS_ERR_IO;
        efs_set_recv_timeout(fd,EFS_IO_TIMEOUT_MS);efs_set_send_timeout(fd,EFS_IO_TIMEOUT_MS);
        uint8_t type;void *payload=NULL;uint32_t len=0;
        rc=send_recv(fd,EFS_MSG_INODE_LOOKUP_PATH,&req,sizeof(req),&type,&payload,&len);
        close(fd);
        if(rc || type!=EFS_MSG_INODE_LOOKUP_PATH_REPLY || len!=sizeof(struct efs_msg_inode_lookup_path_reply)) {
            free(payload);return EFS_ERR_IO;
        }
        struct efs_msg_inode_lookup_path_reply *r=payload;
        rc=r->status==EFS_INODE_RPC_OK?EFS_OK:r->status==EFS_INODE_RPC_NOT_FOUND?EFS_ERR_NOT_FOUND:
           r->status==EFS_INODE_RPC_NOT_PRIMARY?EFS_ERR_NOT_PRIMARY:EFS_ERR_BUSY;
        if(!rc)*inode=r->inode;
        int leader=r->primary_id?(int)r->primary_id-1:-1;
        free(payload);
        if(rc!=EFS_ERR_NOT_PRIMARY)return rc;
        if(leader<0) {
            /* No leader known yet (election): retry the seed. */
            snprintf(target,sizeof(target),"%s",host);target_port=port;
            usleep(50000);
            continue;
        }
        rc=sess_leader_address(host,port,leader,target,&target_port);
        if(rc!=EFS_OK)return rc;
    }
    return EFS_ERR_NOT_PRIMARY; /* budget spent while still redirected */
}

static int cmd_expiry_show(int argc,char **argv)
{
    if((argc!=3 && argc!=5) || strcmp(argv[1],"show") || (argc==5 && strcmp(argv[3],"--mount"))) {
        fprintf(stderr,"Usage:\n  efs-mgmt expiry NODE:PORT show PATH [--mount MOUNT]\n\n"
          "PATH is export-relative; --mount resolves a local mounted path instead.\n"
          "Files show fixed deadlines; folders show nearest future-file policy.\n"
          "Inspection is sampled across RPCs, not an atomic namespace snapshot.\n");return 1;
    }
    char host[64];uint16_t port;if(parse_host_port(argv[0],host,sizeof(host),&port))return 1;
    const char *step="path";int why=0;
    char *local=NULL,*mount=NULL;const char *path=argv[2];struct stat st={0};
    if(argc==5) {
        local=realpath(argv[2],NULL);mount=realpath(argv[4],NULL);
        if(!local || !mount || stat(local,&st))goto bad;
        size_t n=strlen(mount);
        if(strncmp(local,mount,n) || (local[n] && local[n]!='/'))goto bad;
        path=local+n;if(!*path)path="/";
    }
    struct efs_inode inode,verify;int rc=expiry_lookup(host,port,path,&inode);
    step="lookup";why=rc;
    if(rc || (local && inode.ino!=(uint64_t)st.st_ino))goto bad;
    struct efs_msg_raft_mkfs_reply id,r,origin;
    step="file id";
    if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_ID,inode.ino,0,&id)))goto bad;
    uint64_t gen=id.index;
    printf("path: %s\nfile_id: %llu:%llu\n",argv[2],(unsigned long long)inode.ino,(unsigned long long)gen);
    if(S_ISDIR(inode.mode)) {
        uint64_t current=inode.ino,seen[EFS_TXN_NAMESPACE_MAX_PART];unsigned count=0;
        while(current && count<EFS_TXN_NAMESPACE_MAX_PART) {
            for(unsigned i=0;i<count;i++)if(seen[i]==current)goto bad;
            seen[count++]=current;
            struct efs_msg_raft_mkfs_reply parent;
            step="folder parent";
            if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_PARENT,current,0,&parent)))goto bad;
            step="folder policy";
            if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_POLICY_INFO,current,parent.salt,&r)))goto bad;
            if(r.index) {
                char ttl[80],max[80];expiry_duration(r.salt,ttl,sizeof(ttl));expiry_duration(r.term,max,sizeof(max));
                printf("folder_expiry: none (folders do not expire)\nfuture_file_policy: %s\ndefault_ttl: %s\nmaximum_retention: %s\n"
                       "owner_clear: %s\npolicy_source: inode %llu generation %llu revision %llu\n",
                       (r.leader_hint&1)?"enabled":"disabled (shadows ancestor)",ttl,max,
                       (r.leader_hint&2)?"allowed":"refused",(unsigned long long)current,
                       (unsigned long long)parent.salt,(unsigned long long)r.index);
                break;
            }
            if(current==EFS_ROOT_INO) {printf("folder_expiry: none\nfuture_file_policy: none\n");break;}
            current=parent.index;
        }
        if(!current || count==EFS_TXN_NAMESPACE_MAX_PART)goto bad;
    } else {
        step="file record";
        if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_RECORD,inode.ino,gen,&r)))goto bad;
        step="file origin";
        if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_ORIGIN,inode.ino,gen,&origin)))goto bad;
        if(!r.salt)printf("expiry: none%s\n",r.index?" (cleared)":"");
        else {
            time_t seconds=(time_t)(r.salt/1000000000ull);struct tm tm;char date[80],zone[16],relative[80];
            if(!localtime_r(&seconds,&tm) || !strftime(date,sizeof(date),"%Y-%m-%dT%H:%M:%S",&tm) ||
               !strftime(zone,sizeof(zone),"%z",&tm))goto bad;
            struct timespec now;if(clock_gettime(CLOCK_REALTIME,&now))goto bad;
            uint64_t ns=(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec;
            expiry_duration(r.salt>ns?r.salt-ns:ns-r.salt,relative,sizeof(relative));
            printf("expiry: %s.%09llu%s (%s %s)\ndeadline_ns: %llu\nrevision: %llu\n"
                   "policy_source: inode %llu generation %llu revision %llu (materialized at enable/create)\n",
                   date,(unsigned long long)(r.salt%1000000000ull),zone,
                   r.salt>ns?"expires in":"overdue by",relative,(unsigned long long)r.salt,
                   (unsigned long long)r.index,(unsigned long long)r.term,
                   (unsigned long long)origin.index,(unsigned long long)origin.salt);
        }
        struct efs_msg_raft_mkfs_reply after;
        step="record recheck";
        if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_RECORD,inode.ino,gen,&after)) ||
           after.index!=r.index || after.salt!=r.salt)goto bad;
    }
    step="path recheck";
    if((why=expiry_lookup(host,port,path,&verify)) || verify.ino!=inode.ino)goto bad;
    step="id recheck";
    if((why=expiry_read(host,port,EFS_MD_REQ_EXPIRY_ID,inode.ino,0,&id)) || id.index!=gen)goto bad;
    printf("inspection: sampled now; namespace/policy may change after this read\n");
    free(local);free(mount);return 0;
bad:
    fprintf(stderr,"Expiry inspection failed: missing/mismatched path, concurrent change, unavailable server or inspection limit (step: %s, status %d).\n",step,why);
    free(local);free(mount);return 1;
}

/* Human durations are integer quantities with explicit units, never floats. */
static int expiry_parse_duration(const char *text,uint64_t *out)
{
    uint64_t value=0,mult=0;const char *p=text;
    if(!*p)return -1;
    while(*p>='0' && *p<='9') {
        unsigned digit=(unsigned)(*p++-'0');
        if(value>(UINT64_MAX-digit)/10)return -1;
        value=value*10+digit;
    }
    if(p==text)return -1;
    if(!strcmp(p,"ns"))mult=1;
    else if(!strcmp(p,"s"))mult=1000000000ULL;
    else if(!strcmp(p,"m"))mult=60*1000000000ULL;
    else if(!strcmp(p,"h"))mult=3600*1000000000ULL;
    else if(!strcmp(p,"d"))mult=86400*1000000000ULL;
    if(!mult || !value || value>UINT64_MAX/mult)return -1;
    *out=value*mult;return 0;
}

static int cmd_expiry(int argc,char **argv,int policy)
{
    if(!policy && argc>=2 && !strcmp(argv[1],"show"))return cmd_expiry_show(argc,argv);
    int get=argc>=2 && !strcmp(argv[1],"get");
    int mutate=argc>=2 && !strcmp(argv[1],policy?"set":"renew");
    int named=policy && mutate && argc>5 && argv[5][0]=='-';
    char host[64];uint16_t port;uint64_t n[6]={0};
    /* Policy set takes only the named form; the positional TTL_NS MAX_NS
     * FLAGS form was removed. */
    if((get && argc!=4) || (mutate && (policy?!named:argc!=6)) || (!get && !mutate))goto usage;
    if(parse_host_port(argv[0],host,sizeof(host),&port))return 1;
    for(int i=2;i<(named?5:argc);i++)if(expiry_u64(argv[i],&n[i-2]))return 1;
    if(named) {
        unsigned seen=0;
        for(int i=5;i<argc;i+=2) {
            if(i+1>=argc)goto usage;
            unsigned bit=!strcmp(argv[i],"--expire-after")?1:
                         !strcmp(argv[i],"--renewal-limit")?2:
                         !strcmp(argv[i],"--flags")?4:0;
            if(!bit || (seen&bit))goto usage;
            seen|=bit;
            if(bit==4) {if(expiry_u64(argv[i+1],&n[5]))goto usage;}
            else if(expiry_parse_duration(argv[i+1],&n[bit==1?3:4]))goto usage;
        }
        if(!(seen&1) || !(seen&4))goto usage;
        if(!(seen&2))n[4]=n[3];
    }
    if(!n[0] || !n[1] || (policy && mutate && (n[5]>3 || n[3]>n[4])))goto usage;
    uint8_t payload[47]={0};payload[0]=efs_raft_shard_group(efs_kv_inode_shard(n[0]));
    payload[1]=policy?(get?EFS_MD_REQ_EXPIRY_POLICY:EFS_MD_CMD_EXPIRY_POLICY):
                      (get?EFS_MD_REQ_EXPIRY_RECORD:EFS_MD_CMD_EXPIRY_RENEW);
    wr64be_mgmt(payload+2,n[0]);wr64be_mgmt(payload+10,n[1]);uint32_t length=18;
    if(mutate) {
        wr32be_mgmt(payload+18,(uint32_t)geteuid());wr64be_mgmt(payload+22,n[2]);
        if(policy) {wr64be_mgmt(payload+30,n[3]);wr64be_mgmt(payload+38,n[4]);payload[46]=(uint8_t)n[5];length=47;}
        else {wr64be_mgmt(payload+38,n[3]);length=46;}
    }
    struct efs_msg_raft_mkfs_reply reply;int rc=sess_rpc(host,port,payload,length,&reply);
    printf("expiry%s rc=%d revision=%llu %s=%llu %s=%llu leader_hint=%d\n",
           policy?"-policy":"",rc,(unsigned long long)reply.index,
           policy?"ttl_ns":"deadline_ns",(unsigned long long)reply.salt,
           policy?"max_ns":"root_ino",(unsigned long long)reply.term,reply.leader_hint);
    return rc?1:0;
usage:
    fprintf(stderr,"Usage:\n  efs-mgmt expiry-policy NODE:PORT get ROOT_INO GENERATION\n"
                   "  efs-mgmt expiry-policy NODE:PORT set ROOT_INO GENERATION EXPECTED_REV\n"
                   "    --expire-after DURATION [--renewal-limit DURATION] --flags FLAGS\n"
                   "  efs-mgmt expiry NODE:PORT get FILE_INO GENERATION\n"
                   "  efs-mgmt expiry NODE:PORT renew FILE_INO GENERATION EXPECTED_REV DEADLINE_NS\n\n"
                   "DURATION: positive integer followed by ns, s, m, h or d (example: 24h).\n"
                   "--expire-after: new-file lifetime from creation; changes affect future files.\n"
                   "--renewal-limit: furthest deadline from renewal time; defaults to expire-after.\n"
                   "Reading/writing does not renew expiry. Repeated explicit renewals may extend lifetime.\n"
                   "Renewal limit must be at least expire-after; it is not a maximum lifetime.\n"
                   "FLAGS: bit 0 enables future-file defaults; bit 1 permits owner clearing.\n"
                   "An EFS_BUILD_EXPIRY build is required. Set requires uid 0. Renewal requires ownership.\n");
    return 1;
}

#endif

static void sess_pack_hdr(uint8_t *payload, uint8_t group, uint8_t sub,
                          const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    payload[0] = group;
    payload[1] = EFS_MD_CMD_SESSION;
    payload[2] = sub;
    memcpy(payload + 3, uuid, EFS_OPID_UUID_LEN);
}

/* The operator's session fence: the shared I23 barrier coordinator
 * (efs_session_revoke) over this tool's seed/redirect transport. */
struct sess_seed {
    const char *host;
    uint16_t port;
};
static int sess_seed_submit(void *ctx, const uint8_t *payload, uint32_t plen,
                            struct efs_msg_raft_mkfs_reply *out)
{
    const struct sess_seed *seed = ctx;
    return sess_rpc(seed->host, seed->port, payload, plen, out);
}

static int cmd_raft_session_fence(const char *host, uint16_t port,
                                  const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    struct sess_seed seed = {host, port};
    const struct efs_session_rpc rpc = {&seed, sess_seed_submit};
    struct efs_session_revoke_result res;
    int rc = efs_session_revoke(&rpc, uuid, &res), st;
    if (rc != EFS_OK && !strcmp(res.phase, "cleanup"))
        fprintf(stderr,"session cleanup incomplete: epoch=%u shard=%u rc=%d; resume cleanup-leases and cleanup-appends for that epoch/shard\n",res.old_epoch,res.shard,rc);
    if (rc != EFS_OK) fprintf(stderr,"session fence incomplete: phase=%s epoch=%u rc=%d\n",res.phase,res.epoch,rc);
    st = 0;
    if (rc == EFS_ERR_NOT_PRIMARY)
        st = 7;
    else if (rc == EFS_ERR_NOT_FOUND)
        st = 1;
    else if (rc != EFS_OK)
        st = 3;
    printf("raft-session status=%u rc=%d primary=%d op=fence ssh=%u shard=%u "
           "epoch=%u state=%u touched=%u index=%llu\n",
           st, rc, res.last.leader_hint >= 0 ? res.last.leader_hint + 1 : 0,
           efs_kv_session_shard(uuid), res.ntouch, res.epoch, res.state,
           res.ntouch > 0 ? 1u : 0u, (unsigned long long)res.last.index);
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
    uint8_t uuid[EFS_OPID_UUID_LEN], payload[1 + 26];
    uint32_t plen, ssh, shard = 0, epoch = 1;
    struct efs_msg_raft_mkfs_reply *r;
    const char *op;
    uint8_t sub = 0;
    uint32_t st, touched;

    if (argc < 3) {
        fprintf(stderr,
                "Usage:\n  efs-mgmt raft-session <node:port> <create|register|establish|get|fence|cleanup-leases|cleanup-appends> "
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
    if (!strcmp(op, "cleanup-leases") || !strcmp(op, "cleanup-appends")) {
        char *end_epoch = NULL, *end_shard = NULL;
        if (argc != 5) {
            fprintf(stderr, "raft-session cleanup-leases: old epoch and shard required\n");
            return 1;
        }
        unsigned long old = strtoul(argv[3], &end_epoch, 10);
        unsigned long target = strtoul(argv[4], &end_shard, 10);
        if (!*argv[3] || *end_epoch || !old || old > UINT32_MAX ||
            !*argv[4] || *end_shard || target >= EFS_SESSION_BITS) return 1;
        struct efs_msg_raft_mkfs_reply reply;
        memset(&reply, 0, sizeof(reply)); reply.leader_hint = -1;
        struct sess_seed seed = {host, port};
        const struct efs_session_rpc rpc = {&seed, sess_seed_submit};
        int rc = efs_session_cleanup_bounded(&rpc, uuid, (uint32_t)old,
                                      (uint32_t)target, !strcmp(op,"cleanup-appends"), &reply);
        printf("raft-session op=%s epoch=%lu shard=%lu rc=%d index=%llu salt=%llu\n",
               op, old, target, rc, (unsigned long long)reply.index,
               (unsigned long long)reply.salt);
        return rc == EFS_OK ? 0 : 1;
    }
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
        fprintf(stderr, "raft-session: op must be create|register|establish|get|fence|cleanup-leases\n");
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
    struct efs_msg_raft_mkfs_reply result;
    int call_rc = sess_rpc(host, port, payload, plen, &result);
    r = &result;
    if (call_rc != EFS_OK) r->rc = call_rc;
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
        return rc == EFS_OK ? 0 : 1;
    }
}

/* K5: raw metadata mutations name a session. An explicit uuid/epoch on
 * the command line is sent exactly as given (the caller manages it with
 * raft-session; used to probe refusals). Otherwise efs-mgmt uses
 * EFS_MGMT_SESSION=<uuid-hex>[:epoch] when set, else a fresh random uuid at
 * epoch 1, creates that session on its owner shard and admits the target
 * shard itself, like a mount does. */
struct mgmt_session {
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
};

static int mgmt_session_cmd(const char *host, uint16_t port, uint8_t group,
                            uint8_t sub, const struct mgmt_session *s,
                            uint32_t shard, int with_shard)
{
    uint8_t payload[1 + 26];
    struct efs_msg_raft_mkfs_reply reply;
    sess_pack_hdr(payload, group, sub, s->uuid);
    wr32be_mgmt(payload + 19, s->epoch);
    wr32be_mgmt(payload + 23, shard);
    int rc = sess_rpc(host, port, payload, with_shard ? 27 : 23, &reply);
    return rc != EFS_OK ? rc : reply.rc;
}

static int mgmt_session_get(const char *host, uint16_t port, struct mgmt_session *s)
{
    const char *env = getenv("EFS_MGMT_SESSION");
    memset(s, 0, sizeof(*s));
    s->epoch = 1;
    if (env && *env) {
        char hex[33];
        const char *colon = strchr(env, ':');
        size_t n = colon ? (size_t)(colon - env) : strlen(env);
        if (n != 32) { fprintf(stderr, "EFS_MGMT_SESSION: uuid must be 32 hex chars\n"); return EFS_ERR_INVAL; }
        memcpy(hex, env, 32); hex[32] = 0;
        if (parse_uuid_hex(hex, s->uuid) != 0) { fprintf(stderr, "EFS_MGMT_SESSION: bad uuid\n"); return EFS_ERR_INVAL; }
        if (colon) s->epoch = (uint32_t)strtoul(colon + 1, NULL, 0);
        if (!s->epoch) return EFS_ERR_INVAL;
    } else {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f || fread(s->uuid, 1, sizeof(s->uuid), f) != sizeof(s->uuid)) {
            if (f) fclose(f);
            return EFS_ERR_IO;
        }
        fclose(f);
        s->uuid[0] |= 1;
    }
    /* CREATE is a no-op for an existing session (any epoch); the owner's
     * epoch is then checked by REGISTER/ESTABLISH and by every apply. */
    uint32_t ssh = efs_kv_session_shard(s->uuid);
    return mgmt_session_cmd(host, port, efs_raft_shard_group(ssh), EFS_MD_SESS_CREATE,
                            s, 0, 0);
}

static int mgmt_session_admit(const char *host, uint16_t port,
                              const struct mgmt_session *s, uint32_t shard)
{
    uint8_t owner = efs_raft_shard_group(efs_kv_session_shard(s->uuid));
    uint8_t group = efs_raft_shard_group(shard);
    if (owner == group)
        return mgmt_session_cmd(host, port, group, EFS_MD_SESS_REGISTER_ESTABLISH, s, shard, 1);
    int rc = mgmt_session_cmd(host, port, owner, EFS_MD_SESS_REGISTER, s, shard, 1);
    if (rc == EFS_OK)
        rc = mgmt_session_cmd(host, port, group, EFS_MD_SESS_ESTABLISH, s, shard, 1);
    return rc;
}

/* Every invocation takes a new operation sequence so a reused session never
 * replays an earlier invocation's recorded verdict (I16). */
static void mgmt_opid(const struct mgmt_session *s, struct efs_opid_req *q)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    memset(q, 0, sizeof(*q));
    memcpy(q->id.client_uuid, s->uuid, EFS_OPID_UUID_LEN);
    q->id.session_epoch = s->epoch;
    q->id.seq = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull + 1;
    q->ack = q->id.seq - 1;
}

/* Session + op-id suffix for a namespace/SETATTR request (shard < 0: the
 * host admits the served shard itself). */
static int mgmt_identify(const char *host, uint16_t port, int64_t shard,
                         uint8_t *suffix, struct mgmt_session *out)
{
    struct mgmt_session s;
    struct efs_opid_req q;
    int rc = mgmt_session_get(host, port, &s);
    if (rc == EFS_OK && shard >= 0)
        rc = mgmt_session_admit(host, port, &s, (uint32_t)shard);
    if (rc != EFS_OK) {
        fprintf(stderr, "efs-mgmt: session setup failed: %s\n", efs_strerror(rc));
        return rc;
    }
    mgmt_opid(&s, &q);
    efs_opid_req_pack(&q, suffix);
    if (out) *out = s;
    return EFS_OK;
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-getattr <node:port> [ino]\n");
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
    fd = efs_connect_declared(host, port);
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-lookup <node:port> <parent> <name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.name, argv[2], sizeof(req.name) - 1);
    fd = efs_connect_declared(host, port);
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-create <node:port> <parent> <name> "
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
    /* K5: every raw mutation names a session (see mgmt_identify). */
    uint8_t wire[sizeof(req) + EFS_OPID_WIRE_LEN];
    memcpy(wire, &req, sizeof(req));
    if (mgmt_identify(host, port, -1, wire + sizeof(req), NULL) != EFS_OK)
        return 1;
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_CREATE, wire, sizeof(wire), &reply_type,
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
    uint8_t buf[sizeof(req) + EFS_APPEND_ACK_LEN];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    struct efs_msg_inode_reply *r;
    int sess = 0;

    if (argc < 3 || (argc != 3 && argc != 5)) {
        fprintf(stderr,
                "Usage:\n  efs-mgmt raft-append <node:port> <ino> <len> [uuid-hex epoch]\n");
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
    {
        /* K5: a reservation always carries uuid, epoch, seq and ACK. An
         * explicit uuid/epoch is sent as given; otherwise efs-mgmt creates
         * and admits its own session. */
        struct mgmt_session ms;
        struct efs_opid_req q;
        int rc = EFS_OK;
        if (sess) {
            memcpy(ms.uuid, uuid, EFS_OPID_UUID_LEN);
            ms.epoch = epoch;
        } else {
            rc = mgmt_session_get(host, port, &ms);
            if (rc == EFS_OK)
                rc = mgmt_session_admit(host, port, &ms, efs_kv_inode_shard(req.ino));
        }
        if (rc != EFS_OK) {
            fprintf(stderr, "raft-append: session setup failed: %s\n", efs_strerror(rc));
            return 1;
        }
        mgmt_opid(&ms, &q);
        efs_opid_req_pack(&q, buf + slen);
        slen += EFS_APPEND_ACK_LEN;
    }
    fd = efs_connect_declared(host, port);
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
        "Usage:\n  efs-mgmt raft-hold <node:port> <ino> <open|close> [owner] [uuid-hex epoch]\n";

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
    } else {
        /* K5: no stand-in owner; use efs-mgmt's own admitted session. */
        struct mgmt_session ms;
        int rc = mgmt_session_get(host, port, &ms);
        if (rc == EFS_OK)
            rc = mgmt_session_admit(host, port, &ms, efs_kv_inode_shard(req.ino));
        if (rc != EFS_OK) {
            fprintf(stderr, "raft-hold: session setup failed: %s\n", efs_strerror(rc));
            return 1;
        }
        memcpy(uuid, ms.uuid, EFS_OPID_UUID_LEN);
        epoch = ms.epoch;
    }
    memcpy(buf + slen, uuid, EFS_OPID_UUID_LEN);
    memcpy(buf + slen + EFS_OPID_UUID_LEN, &epoch, 4);
    slen += EFS_SESS_WIRE_LEN;
    fd = efs_connect_declared(host, port);
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
        "Usage:\n  efs-mgmt raft-%s <node:port> <ino> <ex|sh|un|gex|gsh|wex|wsh> [owner] [start end] [uuid-hex epoch]\n";
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
    if (!sess && !is_getlk) {
        /* K5: grants and releases name a session; there is no stand-in
         * owner. Use efs-mgmt's own admitted session (EFS_MGMT_SESSION
         * keeps one across invocations, so a later "un" matches). */
        struct mgmt_session ms;
        int rc = mgmt_session_get(host, port, &ms);
        if (rc == EFS_OK)
            rc = mgmt_session_admit(host, port, &ms, efs_kv_inode_shard(req.ino));
        if (rc != EFS_OK) {
            fprintf(stderr, "raft-%s: session setup failed: %s\n", name, efs_strerror(rc));
            return 1;
        }
        memcpy(uuid, ms.uuid, EFS_OPID_UUID_LEN);
        epoch = ms.epoch;
        sess = 1;
    }
    if (sess) {
        memcpy(buf + slen, uuid, EFS_OPID_UUID_LEN);
        memcpy(buf + slen + EFS_OPID_UUID_LEN, &epoch, 4);
        slen += EFS_SESS_WIRE_LEN;
    }
    fd = efs_connect_declared(host, port);
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-unlink <node:port> <parent> <name>\n");
        return 1;
    }
    if (parse_host_port(argv[0], host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.parent = (efs_ino_t)strtoull(argv[1], NULL, 0);
    strncpy(req.name, argv[2], sizeof(req.name) - 1);
    /* K5: every raw mutation names a session (see mgmt_identify). */
    uint8_t wire[sizeof(req) + EFS_OPID_WIRE_LEN];
    memcpy(wire, &req, sizeof(req));
    if (mgmt_identify(host, port, -1, wire + sizeof(req), NULL) != EFS_OK)
        return 1;
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_UNLINK, wire, sizeof(wire), &reply_type,
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-rmdir <node:port> <parent> <name>\n");
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
    /* K5: every raw mutation names a session (see mgmt_identify). */
    uint8_t wire[sizeof(req) + EFS_OPID_WIRE_LEN];
    memcpy(wire, &req, sizeof(req));
    if (mgmt_identify(host, port, -1, wire + sizeof(req), NULL) != EFS_OK)
        return 1;
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_UNLINK, wire, sizeof(wire), &reply_type,
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-link <node:port> <src_ino> <parent> <name>\n");
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
    /* K5: every raw mutation names a session (see mgmt_identify). */
    uint8_t wire[sizeof(req) + EFS_OPID_WIRE_LEN];
    memcpy(wire, &req, sizeof(req));
    if (mgmt_identify(host, port, -1, wire + sizeof(req), NULL) != EFS_OK)
        return 1;
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_LINK, wire, sizeof(wire), &reply_type,
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-setattr <node:port> <ino> <mask> "
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
    /* K5: every raw mutation names a session (see mgmt_identify). */
    uint8_t wire[sizeof(req) + EFS_OPID_WIRE_LEN];
    memcpy(wire, &req, sizeof(req));
    if (mgmt_identify(host, port, (int64_t)efs_kv_inode_shard(req.ino), wire + sizeof(req), NULL) != EFS_OK)
        return 1;
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_SETATTR, wire, sizeof(wire), &reply_type,
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-rename <node:port> <old_parent> <old_name> "
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
    /* K5: every raw mutation names a session (see mgmt_identify). */
    uint8_t wire[sizeof(req) + EFS_OPID_WIRE_LEN];
    memcpy(wire, &req, sizeof(req));
    if (mgmt_identify(host, port, -1, wire + sizeof(req), NULL) != EFS_OK)
        return 1;
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_RENAME_AT, wire, sizeof(wire), &reply_type,
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-readdir <node:port> <parent> [after_ino]\n");
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
    fd = efs_connect_declared(host, port);
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-lookup-path <node:port> <path> [start]\n");
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
    fd = efs_connect_declared(host, port);
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-publish <node:port> <ino> [chunk] [size]\n");
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
    {
        size_t rn = efs_chunk_rec_pack(buf + sizeof(req.hdr),
                                       sizeof(buf) - sizeof(req.hdr),
                                       &req.rec);
        if (rn == 0)
            return 1;
        memcpy(buf, &req.hdr, sizeof(req.hdr));
        memcpy(buf + sizeof(req.hdr) + rn, &req.sz, sizeof(req.sz));
        send_len = (uint32_t)(sizeof(req.hdr) + rn + sizeof(req.sz));
    }
    fd = efs_connect_declared(host, port);
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

/* sync-status <node:port>...: per-root durability state of each node
 * (mode, unsynced bytes/fragments, oldest age, synced %, epochs, barriers,
 * unclean-restart verification). */
static int cmd_sync_status(int argc, char **argv)
{
    static const char *modes[] = {"not attached", "?", "async"};
    int rc_all = 0;
    if (argc < 1) {
        fprintf(stderr, "Usage:\n  efs-mgmt sync-status <node:port>...\n");
        return 1;
    }
    for (int a = 0; a < argc; a++) {
        char host[64];
        uint16_t port;
        if (parse_host_port(argv[a], host, sizeof(host), &port) != 0) {
            fprintf(stderr, "Invalid address: %s\n", argv[a]);
            return 1;
        }
        int fd = efs_connect_declared(host, port);
        if (fd < 0) {
            printf("%s: unreachable\n", argv[a]);
            rc_all = 1;
            continue;
        }
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
        uint8_t type = 0;
        void *reply = NULL;
        uint32_t len = 0;
        if (send_recv(fd, EFS_MSG_SYNC_STATS, NULL, 0, &type, &reply, &len) != 0 ||
            type != EFS_MSG_SYNC_STATS_REPLY || len != sizeof(struct efs_msg_sync_stats_reply)) {
            printf("%s: sync-status unsupported or failed\n", argv[a]);
            free(reply);
            close(fd);
            rc_all = 1;
            continue;
        }
        close(fd);
        const struct efs_msg_sync_stats_reply *r = reply;
        printf("%s: window %u ms / %llu MiB\n", argv[a], r->sync_window_ms,
               (unsigned long long)(r->sync_window_bytes >> 20));
        for (uint32_t i = 0; i < r->count && i < EFS_SYNC_STATS_ROOTS; i++) {
            const struct efs_sync_root_stats *o = &r->roots[i];
            const char *m = o->mode < 3 ? modes[o->mode] : "?";
            if (o->mode != 2) {
                printf("  %s: %s\n", o->root, m);
                continue;
            }
            double pct = o->acked_bytes
                ? 100.0 * (double)(o->acked_bytes - o->unsynced_bytes) / (double)o->acked_bytes : 100.0;
            printf("  %s: async%s unsynced=%.1f MiB (%llu fragments, oldest %llu ms) synced=%.2f%% "
                   "of %.1f MiB acked\n",
                   o->root, o->error ? " ERROR(barrier failure or uncertain recovery)" : "",
                   o->unsynced_bytes / 1048576.0, (unsigned long long)o->unsynced_fragments,
                   (unsigned long long)o->oldest_unsynced_ms, pct, o->acked_bytes / 1048576.0);
            printf("    epoch boot=%u seq=%llu durable=%llu barriers=%llu failures=%llu "
                   "last=%llu ms ago (%llu us)\n",
                   o->boot, (unsigned long long)o->current_seq, (unsigned long long)o->durable_seq,
                   (unsigned long long)o->barriers, (unsigned long long)o->barrier_failures,
                   (unsigned long long)o->last_barrier_ms_ago, (unsigned long long)o->last_barrier_us);
            if (!o->prev_clean)
                printf("    previous stop UNCLEAN: recovery %s scanned=%llu bad=%llu\n",
                       o->recovery_done ? "done" : "running",
                       (unsigned long long)o->recovery_scanned, (unsigned long long)o->recovery_bad);
            if (o->error || o->recovery_bad)
                rc_all = 1;
        }
        free(reply);
    }
    return rc_all;
}

/* data-commit <node:port>...: make everything each node acknowledged durable. */
static int cmd_data_commit(int argc, char **argv)
{
    int rc_all = 0;
    if (argc < 1) {
        fprintf(stderr, "Usage:\n  efs-mgmt data-commit <node:port>...\n");
        return 1;
    }
    for (int a = 0; a < argc; a++) {
        char host[64];
        uint16_t port;
        if (parse_host_port(argv[a], host, sizeof(host), &port) != 0) {
            fprintf(stderr, "Invalid address: %s\n", argv[a]);
            return 1;
        }
        int fd = efs_connect_declared(host, port);
        if (fd < 0) { printf("%s: unreachable\n", argv[a]); rc_all = 1; continue; }
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
        uint8_t type = 0;
        void *reply = NULL;
        uint32_t len = 0;
        int ok = send_recv(fd, EFS_MSG_DATA_COMMIT, NULL, 0, &type, &reply, &len) == 0 &&
                 type == EFS_MSG_DATA_COMMIT_REPLY &&
                 len == sizeof(struct efs_msg_data_commit_reply);
        close(fd);
        const struct efs_msg_data_commit_reply *r = reply;
        printf("%s: data-commit %s roots=%u\n", argv[a],
               !ok ? "failed" : r->rc == EFS_OK ? "ok" : "EIO", ok ? r->roots : 0);
        if (!ok || r->rc != EFS_OK)
            rc_all = 1;
        free(reply);
    }
    return rc_all;
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
        fprintf(stderr, "Usage:\n  efs-mgmt raft-getchunks <node:port> <ino> [start]\n");
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
    fd = efs_connect_declared(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    if (send_recv(fd, EFS_MSG_INODE_GETCHUNKS, &req, sizeof(req), &reply_type,
                  &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_INODE_GETCHUNKS_REPLY ||
        efs_getchunks_reply_len_ok(reply, reply_len) != EFS_OK) {
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
    /* EFS_MGMT_CHUNKS=N prints the first N rows (default 4) with the
     * span trailer: which pieces of a shared chunk the server holds. */
    {
        const char *e = getenv("EFS_MGMT_CHUNKS");
        uint32_t lim = e ? (uint32_t)strtoul(e, NULL, 0) : 4u;

        for (i = 0; i < r->count && i < lim; i++) {
            uint32_t d;

            printf("  ci=%u nodes=%u,%u,%u base_gen=%llu obj_gen=%llu ck0=%02x%02x%02x%02x spans=%u seq=%llu\n",
                   r->recs[i].chunk_index,
                   r->recs[i].nodes[0], r->recs[i].nodes[1], r->recs[i].nodes[2],
                   (unsigned long long)r->recs[i].base_gen,
                   (unsigned long long)r->recs[i].chunk_generation,
                   r->recs[i].checksums[0][0], r->recs[i].checksums[0][1],
                   r->recs[i].checksums[0][2], r->recs[i].checksums[0][3],
                   r->recs[i].delta_base_n,
                   (unsigned long long)r->recs[i].delta_base_seq);
            /* W125: a packed base names its segment record. */
            if (r->recs[i].publish_flags & EFS_CHUNK_REC_F_PACKED) {
                struct efs_pack_ref ref;
                if (efs_pack_ref_decode(r->recs[i].packed_ref, sizeof(r->recs[i].packed_ref), &ref) == EFS_OK)
                    printf("    packed segment=%llu gen=%llu group=%u offset=%u len=%u\n",
                           (unsigned long long)ref.segment_id, (unsigned long long)ref.segment_generation,
                           ref.publication_group, ref.offset, ref.len);
                else
                    printf("    packed reference malformed\n");
            }
            for (d = 0; d < r->recs[i].delta_base_n && d < EFS_CHUNK_DELTA_MAX; d++)
                printf("    span off=%u len=%u gen=%llu seq=%llu nodes=%u,%u,%u\n",
                       r->recs[i].deltas[d].off, r->recs[i].deltas[d].len,
                       (unsigned long long)r->recs[i].deltas[d].generation,
                       (unsigned long long)r->recs[i].deltas[d].seq,
                       r->recs[i].deltas[d].nodes[0],
                       r->recs[i].deltas[d].nodes[1],
                       r->recs[i].deltas[d].nodes[2]);
        }
    }
    free(reply);
    return 0;
}

int main(int argc, char **argv)
{
    efs_version_check_argv("efs-mgmt", argc, argv);

    if (efs_help_requested(argc,argv)) {
        return efs_mgmt_help(stdout,argc>2 ? argv[1] : NULL);
    }
    if (argc < 2) { efs_cli_help(stderr,argv[0],NULL);return 1; }

    const char *cmd = argv[1];
    if (strcmp(cmd, "status") == 0)
        return cmd_status(argc - 2, argv + 2);
    if (strcmp(cmd, "sync-status") == 0)
        return cmd_sync_status(argc - 2, argv + 2);
    if (strcmp(cmd, "data-commit") == 0)
        return cmd_data_commit(argc - 2, argv + 2);
    if (strcmp(cmd, "add-node") == 0)
        return cmd_add_node(argc - 2, argv + 2);
    if (strcmp(cmd, "shrink-quota") == 0)
        return cmd_shrink_quota(argc - 2, argv + 2);
    if (strcmp(cmd, "add-storage") == 0)
        return cmd_add_storage(argc - 2, argv + 2);
    if (strcmp(cmd, "io-stats") == 0)
        return cmd_io_stats(argc - 2, argv + 2);
    if (strcmp(cmd, "version") == 0)
        return cmd_version(argc - 2, argv + 2);
    /* One metadata engine: mkfs is the raft mkfs. */
    if (strcmp(cmd, "mkfs") == 0)
        return cmd_raft_mkfs(argc - 2, argv + 2);
    if (strcmp(cmd, "pack-compact") == 0)
        return cmd_pack_compact(argc - 2, argv + 2);
    if (strcmp(cmd, "gc-status") == 0)
        return cmd_gc_status(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-status") == 0)
        return cmd_raft_status(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-resync") == 0)
        return cmd_raft_resync(argc - 2, argv + 2);
    if (strcmp(cmd, "raft-step-down") == 0)
        return cmd_raft_step_down(argc - 2, argv + 2);
#if EFS_BUILD_EXPIRY
    if (strcmp(cmd,"expiry-status")==0)return cmd_expiry_status(argc-2,argv+2);
    if (strcmp(cmd,"expiry-id")==0)return cmd_expiry_id(argc-2,argv+2);
    if (strcmp(cmd,"expiry-enable")==0)return cmd_expiry_enable(argc-2,argv+2);
    if (strcmp(cmd,"expiry-clock")==0)return cmd_expiry_clock(argc-2,argv+2);
    if (strcmp(cmd,"expiry-policy")==0)return cmd_expiry(argc-2,argv+2,1);
    if (strcmp(cmd,"expiry")==0)return cmd_expiry(argc-2,argv+2,0);
#endif
    if (strcmp(cmd, "commands-policy") == 0)
        return cmd_commands_policy(argc - 2, argv + 2);
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
