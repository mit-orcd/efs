#include "server_internal.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/wire.h"
#include "efs/network.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HOST_TICK_US       5000
#define HOST_HB_TICKS      10
#define HOST_ELECT_BASE    100
#define HOST_ELECT_SPREAD  20
#define HOST_INBOX_MAX     256
#define HOST_ENCODE_STACK  (64 * 1024)
#define HOST_NGROUPS       2

struct host_inbox_item {
    uint8_t *buf;
    uint32_t len;
};

struct host_group {
    uint8_t group;
    uint8_t hosted;
    uint32_t voters;
    struct efs_raft *r;
};

struct efs_raft_host {
    struct efsd_server *s;
    int raft_id;
    int n;
    uint64_t boot_id;
    uint64_t salt;
    struct efs_kv *kv;
    struct efs_raft_disk *disk;
    struct host_group g[HOST_NGROUPS];
    pthread_mutex_t mu;
    pthread_t tid;
    int running;
    int started;
    pthread_mutex_t inbox_mu;
    struct host_inbox_item inbox[HOST_INBOX_MAX];
    int inbox_n;
};

static struct efs_raft_host *g_host;

static void wr64be(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t rd64be(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static int env_on(const char *name)
{
    const char *v = getenv(name);
    if (!v || !v[0] || strcmp(v, "0") == 0)
        return 0;
    return 1;
}

static uint32_t group_voters(uint8_t group, int n)
{
    if (n <= 3)
        return (1u << n) - 1u;
    if (group == EFS_RAFT_GROUP_SHARD)
        return 0x7u; /* nodes 1,2,3 = raft ids 0,1,2 */
    return 0xeu;     /* nodes 2,3,4 = raft ids 1,2,3 */
}

static int hosts_group(int raft_id, uint32_t voters)
{
    if (raft_id < 0 || raft_id >= EFS_RAFT_MAX_PEERS)
        return 0;
    return (voters & (1u << raft_id)) != 0;
}

static int peer_addr(struct efs_raft_host *h, int raft_id,
                     char *host, size_t hlen, uint16_t *port)
{
    efs_node_id_t nid;
    uint32_t i;

    if (!h || !h->s || raft_id < 0)
        return -1;
    nid = (efs_node_id_t)(raft_id + 1);
    pthread_mutex_lock(&h->s->lock);
    for (i = 0; i < h->s->node_count; i++) {
        if (h->s->nodes[i].id == nid) {
            strncpy(host, h->s->nodes[i].addr, hlen - 1);
            host[hlen - 1] = '\0';
            *port = h->s->nodes[i].port;
            pthread_mutex_unlock(&h->s->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&h->s->lock);
    return -1;
}

/* Best-effort: a send failure is a dropped packet. Returning an error from
 * tick/recv aborts the remaining broadcasts and stalls elections. */
static int host_send(void *net, const struct efs_raft_msg *msg)
{
    struct efs_raft_host *h = net;
    uint8_t stack[HOST_ENCODE_STACK];
    uint8_t *buf = stack;
    uint32_t cap = sizeof(stack);
    uint32_t len = 0;
    uint8_t *heap = NULL;
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;
    int rc;

    if (!h || !msg)
        return EFS_OK;
    if (msg->to == h->raft_id)
        return EFS_OK;
    rc = efs_wire_raft_encode(msg, buf, cap, &len);
    if (rc == EFS_ERR_NOMEM) {
        cap = EFS_WIRE_RAFT_HDR_LEN + 12u +
              (msg->nentries ? msg->entries[0].clen : 0);
        heap = malloc(cap);
        if (!heap)
            return EFS_OK;
        buf = heap;
        rc = efs_wire_raft_encode(msg, buf, cap, &len);
    }
    if (rc != EFS_OK) {
        free(heap);
        return EFS_OK;
    }
    if (peer_addr(h, msg->to, host, sizeof(host), &port) != 0) {
        free(heap);
        return EFS_OK;
    }
    pc = server_peer_conn_get(host, port);
    if (!pc) {
        free(heap);
        return EFS_OK;
    }
    if (efs_conn_send_msg(pc, EFS_MSG_RAFT, buf, len) != 0) {
        server_peer_conn_drop(host, port, pc);
        free(heap);
        return EFS_OK;
    }
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||
        rtype != EFS_MSG_RAFT_REPLY) {
        free(reply);
        server_peer_conn_drop(host, port, pc);
        free(heap);
        return EFS_OK;
    }
    free(reply);
    server_peer_conn_release(host, port, pc);
    free(heap);
    return EFS_OK;
}

static int host_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct efs_raft_host *h = app;
    uint64_t now, salt = 0;
    int rc;

    (void)index;
    (void)term;
    if (!h || !h->kv || !cmd || clen == 0)
        return EFS_OK;
    if (cmd[0] != EFS_MD_CMD_MKFS)
        return EFS_OK;
    if (clen < 9)
        return EFS_OK;
    now = rd64be(cmd + 1);
    if (clen >= 17)
        salt = rd64be(cmd + 9);
    rc = efs_meta_apply_mkfs(h->kv, now, salt);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply mkfs rc=%d index=%llu\n",
                rc, (unsigned long long)index);
        return rc;
    }
    fprintf(stderr, "raft-host: applied mkfs index=%llu salt=%llu\n",
            (unsigned long long)index, (unsigned long long)salt);
    return EFS_OK;
}

static struct efs_raft *group_raft(struct efs_raft_host *h, uint8_t group)
{
    int i;
    for (i = 0; i < HOST_NGROUPS; i++) {
        if (h->g[i].hosted && h->g[i].group == group)
            return h->g[i].r;
    }
    return NULL;
}

static void drain_inbox(struct efs_raft_host *h)
{
    struct host_inbox_item local[HOST_INBOX_MAX];
    int n, i;

    pthread_mutex_lock(&h->inbox_mu);
    n = h->inbox_n;
    memcpy(local, h->inbox, (size_t)n * sizeof(local[0]));
    h->inbox_n = 0;
    pthread_mutex_unlock(&h->inbox_mu);

    for (i = 0; i < n; i++) {
        struct efs_raft_msg msg;
        uint8_t *cmd = NULL;
        uint32_t cmd_cap = 0;
        struct efs_raft *r;
        int rc;

        if (local[i].len > EFS_WIRE_RAFT_HDR_LEN + 12u)
            cmd_cap = local[i].len - EFS_WIRE_RAFT_HDR_LEN - 12u;
        if (cmd_cap) {
            cmd = malloc(cmd_cap);
            if (!cmd) {
                free(local[i].buf);
                continue;
            }
        }
        rc = efs_wire_raft_decode(local[i].buf, local[i].len, &msg, cmd,
                                  cmd_cap);
        free(local[i].buf);
        if (rc != EFS_OK) {
            free(cmd);
            continue;
        }
        r = group_raft(h, msg.group);
        if (r)
            (void)efs_raft_recv(r, &msg);
        free(cmd);
    }
}

static void *host_pump(void *arg)
{
    struct efs_raft_host *h = arg;

    while (h->running) {
        int i;
        pthread_mutex_lock(&h->mu);
        drain_inbox(h);
        for (i = 0; i < HOST_NGROUPS; i++) {
            if (h->g[i].hosted && h->g[i].r)
                (void)efs_raft_tick(h->g[i].r);
        }
        pthread_mutex_unlock(&h->mu);
        usleep(HOST_TICK_US);
    }
    return NULL;
}

static uint64_t make_boot_id(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)getpid() << 32) ^
           ((uint64_t)ts.tv_sec << 16) ^ (uint64_t)ts.tv_nsec;
}

static uint64_t make_salt(uint64_t boot)
{
    uint64_t s = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&s, 1, sizeof(s), f) != sizeof(s))
            s = 0;
        fclose(f);
    }
    if (s == 0)
        s = boot ^ 0x9e3779b97f4a7c15ULL;
    return s;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int attach_group(struct efs_raft_host *h, int gi, uint8_t group)
{
    struct efs_raft_cfg cfg;
    struct efs_raft_store *st;
    uint32_t voters = group_voters(group, h->n);

    h->g[gi].group = group;
    h->g[gi].voters = voters;
    h->g[gi].hosted = (uint8_t)hosts_group(h->raft_id, voters);
    h->g[gi].r = NULL;
    if (!h->g[gi].hosted)
        return EFS_OK;
    st = efs_raft_disk_group(h->disk, group);
    if (!st)
        return EFS_ERR_IO;
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = h->raft_id;
    cfg.n = h->n;
    cfg.voters = voters;
    cfg.election_ticks = (uint32_t)(HOST_ELECT_BASE +
                                    h->raft_id * HOST_ELECT_SPREAD);
    cfg.heartbeat_ticks = HOST_HB_TICKS;
    cfg.boot_id = h->boot_id;
    cfg.group = group;
    cfg.store = st;
    cfg.store_ctx = st;
    cfg.send = host_send;
    cfg.net = h;
    cfg.apply = host_apply;
    cfg.app = h;
    h->g[gi].r = efs_raft_new(&cfg);
    if (!h->g[gi].r)
        return EFS_ERR_NOMEM;
    return EFS_OK;
}

int server_raft_host_start(struct efsd_server *s)
{
    struct efs_raft_host *h;
    char dir[EFS_MAX_PATH];
    const char *ns;
    int n = EFS_MAX_NODES;
    int rc;

    if (!env_on("EFS_MD_RAFT"))
        return 0;
    if (!s || g_host)
        return EFS_ERR_INVAL;
    ns = getenv("EFS_MD_RAFT_N");
    if (ns && ns[0]) {
        n = atoi(ns);
        if (n < 3 || n > EFS_MAX_NODES) {
            fprintf(stderr, "raft-host: EFS_MD_RAFT_N must be 3..%d\n",
                    EFS_MAX_NODES);
            return EFS_ERR_INVAL;
        }
    }
    if (s->id < 1 || (int)s->id > n) {
        fprintf(stderr, "raft-host: node-id %u out of 1..%d\n", s->id, n);
        return EFS_ERR_INVAL;
    }

    h = calloc(1, sizeof(*h));
    if (!h)
        return EFS_ERR_NOMEM;
    h->s = s;
    h->raft_id = (int)s->id - 1;
    h->n = n;
    h->boot_id = make_boot_id();
    h->salt = make_salt(h->boot_id);
    pthread_mutex_init(&h->mu, NULL);
    pthread_mutex_init(&h->inbox_mu, NULL);

    snprintf(dir, sizeof(dir), "%s/mdraft", s->storage_path);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "raft-host: mkdir %s: %s\n", dir, strerror(errno));
        free(h);
        return EFS_ERR_IO;
    }
    {
        char kvdir[EFS_MAX_PATH], logdir[EFS_MAX_PATH];
        struct efs_kv_lsm_cfg kcfg;
        snprintf(kvdir, sizeof(kvdir), "%s/kv", dir);
        snprintf(logdir, sizeof(logdir), "%s/log", dir);
        memset(&kcfg, 0, sizeof(kcfg));
        kcfg.sync_mode = EFS_KV_LSM_SYNC;
        h->kv = efs_kv_lsm_open(kvdir, &kcfg);
        if (!h->kv) {
            fprintf(stderr, "raft-host: kv_lsm_open %s failed\n", kvdir);
            free(h);
            return EFS_ERR_IO;
        }
        h->disk = efs_raft_disk_open(logdir, EFS_RAFT_DISK_SYNC);
        if (!h->disk) {
            fprintf(stderr, "raft-host: raft_disk_open %s failed\n", logdir);
            efs_kv_lsm_close(h->kv);
            free(h);
            return EFS_ERR_IO;
        }
    }
    rc = attach_group(h, 0, EFS_RAFT_GROUP_SHARD);
    if (rc == EFS_OK)
        rc = attach_group(h, 1, EFS_RAFT_GROUP_SHARD2);
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: attach failed rc=%d\n", rc);
        efs_raft_free(h->g[0].r);
        efs_raft_free(h->g[1].r);
        efs_raft_disk_close(h->disk);
        efs_kv_lsm_close(h->kv);
        free(h);
        return rc;
    }
    h->running = 1;
    g_host = h;
    if (efsd_pthread_create(&h->tid, host_pump, h) != 0) {
        fprintf(stderr, "raft-host: pump thread failed\n");
        g_host = NULL;
        h->running = 0;
        efs_raft_free(h->g[0].r);
        efs_raft_free(h->g[1].r);
        efs_raft_disk_close(h->disk);
        efs_kv_lsm_close(h->kv);
        free(h);
        return EFS_ERR_IO;
    }
    h->started = 1;
    fprintf(stderr,
            "raft-host: up raft_id=%d n=%d boot=%llu salt=%llu "
            "g0=%s g2=%s\n",
            h->raft_id, h->n,
            (unsigned long long)h->boot_id,
            (unsigned long long)h->salt,
            h->g[0].hosted ? "hosted" : "off",
            h->g[1].hosted ? "hosted" : "off");
    return 0;
}

void server_raft_host_stop(void)
{
    struct efs_raft_host *h = g_host;
    int i;

    if (!h)
        return;
    h->running = 0;
    if (h->started)
        pthread_join(h->tid, NULL);
    pthread_mutex_lock(&h->mu);
    for (i = 0; i < HOST_NGROUPS; i++) {
        efs_raft_free(h->g[i].r);
        h->g[i].r = NULL;
    }
    pthread_mutex_unlock(&h->mu);
    pthread_mutex_lock(&h->inbox_mu);
    for (i = 0; i < h->inbox_n; i++)
        free(h->inbox[i].buf);
    h->inbox_n = 0;
    pthread_mutex_unlock(&h->inbox_mu);
    efs_raft_disk_close(h->disk);
    efs_kv_lsm_close(h->kv);
    pthread_mutex_destroy(&h->mu);
    pthread_mutex_destroy(&h->inbox_mu);
    g_host = NULL;
    free(h);
}

int server_raft_host_inbox(const uint8_t *payload, uint32_t plen)
{
    struct efs_raft_host *h = g_host;
    uint8_t *copy;

    if (!h || !h->running)
        return EFS_ERR_INVAL;
    if (!payload || plen < EFS_WIRE_RAFT_HDR_LEN)
        return EFS_ERR_PROTO;
    copy = malloc(plen);
    if (!copy)
        return EFS_ERR_NOMEM;
    memcpy(copy, payload, plen);
    pthread_mutex_lock(&h->inbox_mu);
    if (h->inbox_n >= HOST_INBOX_MAX) {
        pthread_mutex_unlock(&h->inbox_mu);
        free(copy);
        return EFS_ERR_BUSY;
    }
    h->inbox[h->inbox_n].buf = copy;
    h->inbox[h->inbox_n].len = plen;
    h->inbox_n++;
    pthread_mutex_unlock(&h->inbox_mu);
    return EFS_OK;
}

void server_raft_host_mkfs(struct efs_msg_raft_mkfs_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_raft *r;
    uint8_t cmd[17];
    uint64_t idx = 0;
    int rc;

    memset(out, 0, sizeof(*out));
    out->leader_hint = -1;
    if (!h || !h->running) {
        out->rc = EFS_ERR_INVAL;
        return;
    }
    out->salt = h->salt;
    cmd[0] = EFS_MD_CMD_MKFS;
    wr64be(cmd + 1, now_ns());
    wr64be(cmd + 9, h->salt);
    pthread_mutex_lock(&h->mu);
    r = group_raft(h, EFS_RAFT_GROUP_SHARD);
    if (!r) {
        pthread_mutex_unlock(&h->mu);
        out->rc = EFS_ERR_NOT_PRIMARY;
        return;
    }
    out->leader_hint = efs_raft_leader(r);
    rc = efs_raft_propose(r, cmd, 17, &idx);
    pthread_mutex_unlock(&h->mu);
    out->rc = rc;
    out->index = idx;
}

void server_raft_host_status(struct efs_msg_raft_status_reply *out)
{
    struct efs_raft_host *h = g_host;
    int i;
    uint64_t salt = 0;
    int src;

    memset(out, 0, sizeof(*out));
    if (!h || !h->running) {
        out->rc = EFS_ERR_INVAL;
        return;
    }
    out->rc = EFS_OK;
    out->node_id = h->s ? h->s->id : 0;
    pthread_mutex_lock(&h->mu);
    out->ngroups = HOST_NGROUPS;
    for (i = 0; i < HOST_NGROUPS; i++) {
        struct efs_raft_group_status *gs = &out->groups[i];
        gs->group = h->g[i].group;
        gs->hosted = h->g[i].hosted;
        gs->voters = h->g[i].voters;
        gs->leader = -1;
        if (h->g[i].hosted && h->g[i].r) {
            gs->role = (uint8_t)efs_raft_role(h->g[i].r);
            gs->leader = efs_raft_leader(h->g[i].r);
            gs->term = efs_raft_term(h->g[i].r);
            gs->commit_index = efs_raft_commit(h->g[i].r);
            gs->applied_index = efs_raft_applied(h->g[i].r);
        }
    }
    pthread_mutex_unlock(&h->mu);
    src = efs_meta_apply_export_salt(h->kv, &salt);
    if (src == EFS_OK) {
        struct efs_meta_row row;
        if (efs_meta_apply_get_inode(h->kv, EFS_ROOT_INO, &row) == EFS_OK) {
            out->kv_has_root = 1;
            out->export_salt = salt;
        }
    }
}
