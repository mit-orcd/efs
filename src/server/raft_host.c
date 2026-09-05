#include "server_internal.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include "efs/metadata.h"
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
#define HOST_READ_TRIES    80 /* 80 × 5 ms = 400 ms; heartbeat is 50 ms */
#define HOST_CREATE_NAME_OFF 31
#define HOST_CMD_MAX       512

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
    pthread_mutex_t read_mu; /* serializes ReadIndex; never held by the pump */
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

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t now_ns(void);

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

static int apply_mkfs_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                          uint32_t clen, uint64_t index)
{
    uint64_t now, salt = 0;
    int rc;

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

/* Same encoding as sim pack_create / apply_create_cmd. Session fencing is
 * not hosted yet; apply is create_file only. EXIST is replay (idempotent).
 * Apply always returns OK so a name clash cannot stall the log. */
static int apply_create_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent, ino = 0;
    uint32_t mode;
    char name[EFS_MAX_NAME];
    uint8_t nl;
    struct efs_meta_attrs at;
    int rc;

    if (clen < HOST_CREATE_NAME_OFF)
        return EFS_OK;
    parent = rd64be(cmd + 2);
    mode = rd32be(cmd + 10);
    memset(&at, 0, sizeof(at));
    at.uid = rd32be(cmd + 14);
    at.gid = rd32be(cmd + 18);
    at.now = rd64be(cmd + 22);
    nl = cmd[30];
    if ((uint32_t)HOST_CREATE_NAME_OFF + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_OK;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + HOST_CREATE_NAME_OFF, nl);
    rc = efs_meta_apply_create_file(h->kv, &at, parent, mode, name, &ino);
    if (rc == EFS_ERR_EXIST) {
        struct efs_meta_dentry dent;
        if (efs_meta_apply_lookup(h->kv, parent, name, &dent) == EFS_OK)
            ino = dent.ino;
        rc = EFS_OK;
    }
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply create rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied create index=%llu parent=%llu name=%s "
            "ino=%llu\n",
            (unsigned long long)index, (unsigned long long)parent, name,
            (unsigned long long)ino);
    return EFS_OK;
}

static int apply_unlink_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t nl;
    uint64_t now;
    int rc;

    if (clen < 18)
        return EFS_OK;
    parent = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    nl = cmd[17];
    if ((uint32_t)18 + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_OK;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + 18, nl);
    rc = efs_meta_apply_unlink(h->kv, parent, name, now);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK; /* replay */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply unlink rc=%d index=%llu parent=%llu "
                "name=%s\n",
                rc, (unsigned long long)index, (unsigned long long)parent,
                name);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied unlink index=%llu parent=%llu name=%s\n",
            (unsigned long long)index, (unsigned long long)parent, name);
    return EFS_OK;
}

/* Same encoding as sim_txn_apply. Apply never stalls the log. */
static int apply_txn_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                         uint32_t clen)
{
    struct efs_txid t;
    uint32_t shard, off;
    int rc = EFS_ERR_PROTO, dec, kind, op;
    uint64_t expected;
    uint8_t klen;
    const uint8_t *key, *val;
    uint32_t vlen;
    struct efs_txn_parts p;
    struct efs_txn_reduce red;

    memset(&t, 0, sizeof(t));
    memset(&p, 0, sizeof(p));
    switch (cmd[0]) {
    case EFS_MD_CMD_PREPARE:
        if (clen < 1 + 1 + 16 + 1)
            break;
        kind = cmd[1];
        memcpy(t.bytes, cmd + 2, 16);
        p.n = cmd[18];
        if (p.n == 0 || p.n > EFS_TXN_MAX_PART)
            break;
        off = 19;
        if (clen < off + (uint32_t)p.n * 4u + 1u)
            break;
        {
            uint8_t i;
            for (i = 0; i < p.n; i++)
                p.shard[i] = rd32be(cmd + off + (uint32_t)i * 4u);
        }
        off += (uint32_t)p.n * 4u;
        klen = cmd[off++];
        if (clen < off + klen)
            break;
        key = cmd + off;
        off += klen;
        if (kind == EFS_TXN_EXCL) {
            if (clen < off + 8 + 1 + 4)
                break;
            expected = rd64be(cmd + off);
            op = cmd[off + 8];
            vlen = rd32be(cmd + off + 9);
            off += 13;
            if (clen < off + vlen)
                break;
            val = cmd + off;
            rc = efs_txn_prepare_excl(h->kv, &t, &p, key, klen, expected, op,
                                      val, vlen);
        } else if (kind == EFS_TXN_GUARD) {
            if (clen < off + 8)
                break;
            expected = rd64be(cmd + off);
            rc = efs_txn_prepare_guard(h->kv, &t, &p, key, klen, expected);
        } else if (kind == EFS_TXN_REDUCE) {
            if (clen < off + 24)
                break;
            red.max_end = rd64be(cmd + off);
            red.max_mtime = rd64be(cmd + off + 8);
            red.max_ctime = rd64be(cmd + off + 16);
            rc = efs_txn_prepare_reduce(h->kv, &t, &p, key, klen, &red);
        }
        break;
    case EFS_MD_CMD_DECIDE:
        if (clen < 1 + 16 + 4 + 1)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32be(cmd + 17);
        dec = cmd[21];
        rc = efs_txn_decide(h->kv, shard, &t, dec);
        break;
    case EFS_MD_CMD_RESOLVE:
        if (clen < 1 + 16 + 4 + 1)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32be(cmd + 17);
        dec = cmd[21];
        rc = efs_txn_resolve(h->kv, &t, shard, dec);
        break;
    case EFS_MD_CMD_DROP:
        if (clen < 1 + 16 + 4)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32be(cmd + 17);
        rc = efs_txn_drop(h->kv, &t, shard);
        break;
    default:
        return EFS_OK;
    }
    (void)rc;
    return EFS_OK;
}

static int host_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct efs_raft_host *h = app;

    (void)term;
    if (!h || !h->kv || !cmd || clen == 0)
        return EFS_OK;
    if (cmd[0] == EFS_MD_CMD_MKFS)
        return apply_mkfs_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_CREATE)
        return apply_create_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_UNLINK)
        return apply_unlink_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_PREPARE || cmd[0] == EFS_MD_CMD_DECIDE ||
        cmd[0] == EFS_MD_CMD_RESOLVE || cmd[0] == EFS_MD_CMD_DROP)
        return apply_txn_cmd(h, cmd, clen);
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

/* Drop h->mu while waiting: the pump must tick for heartbeats to land.
 * Caller serializes with read_mu. Sets *leader_hint to the raft id. */
static int host_read_index(struct efs_raft_host *h, uint8_t group,
                           int *leader_hint)
{
    int begun = 0;
    int t;

    if (leader_hint)
        *leader_hint = -1;
    for (t = 0; t < HOST_READ_TRIES; t++) {
        struct efs_raft *r;
        int rc;

        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (!r) {
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_NOT_PRIMARY;
        }
        if (leader_hint)
            *leader_hint = efs_raft_leader(r);
        if (efs_raft_role(r) != EFS_RAFT_LEADER) {
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_NOT_PRIMARY;
        }
        if (!begun) {
            rc = efs_raft_read_begin(r);
            begun = 1;
            if (rc != EFS_OK) {
                pthread_mutex_unlock(&h->mu);
                return rc;
            }
        }
        if (efs_raft_read_ready(r)) {
            pthread_mutex_unlock(&h->mu);
            return EFS_OK;
        }
        pthread_mutex_unlock(&h->mu);
        usleep(HOST_TICK_US);
    }
    return EFS_ERR_BUSY;
}

static int host_wait_applied(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, int *leader_hint)
{
    int t;

    for (t = 0; t < HOST_READ_TRIES; t++) {
        struct efs_raft *r;

        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (!r) {
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_NOT_PRIMARY;
        }
        if (leader_hint)
            *leader_hint = efs_raft_leader(r);
        if (efs_raft_role(r) != EFS_RAFT_LEADER) {
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_NOT_PRIMARY;
        }
        if (efs_raft_applied(r) >= idx) {
            pthread_mutex_unlock(&h->mu);
            return EFS_OK;
        }
        pthread_mutex_unlock(&h->mu);
        usleep(HOST_TICK_US);
    }
    return EFS_ERR_BUSY;
}

static int host_propose(struct efs_raft_host *h, uint8_t group,
                        const uint8_t *cmd, uint32_t clen, uint64_t *idx,
                        int *leader_hint)
{
    struct efs_raft *r;
    int rc;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (!r) {
        pthread_mutex_unlock(&h->mu);
        return EFS_ERR_NOT_PRIMARY;
    }
    if (leader_hint)
        *leader_hint = efs_raft_leader(r);
    if (efs_raft_role(r) != EFS_RAFT_LEADER) {
        pthread_mutex_unlock(&h->mu);
        return EFS_ERR_NOT_PRIMARY;
    }
    rc = efs_raft_propose(r, cmd, clen, idx);
    pthread_mutex_unlock(&h->mu);
    return rc;
}

static int host_propose_wait(struct efs_raft_host *h, uint8_t group,
                             const uint8_t *cmd, uint32_t clen, int *hint)
{
    uint64_t idx = 0;
    int rc;

    rc = host_propose(h, group, cmd, clen, &idx, hint);
    if (rc != EFS_OK)
        return rc;
    return host_wait_applied(h, group, idx, hint);
}

static int pack_create_cmd(uint8_t *out, uint32_t *len, efs_ino_t parent,
                           uint32_t mode, const char *name,
                           const struct efs_meta_attrs *at)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = HOST_CREATE_NAME_OFF + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_CREATE;
    out[1] = 0; /* no op-id yet */
    wr64be(out + 2, parent);
    wr32be(out + 10, mode);
    wr32be(out + 14, at->uid);
    wr32be(out + 18, at->gid);
    wr64be(out + 22, at->now);
    out[30] = (uint8_t)nl;
    memcpy(out + HOST_CREATE_NAME_OFF, name, nl);
    p = out + HOST_CREATE_NAME_OFF + nl;
    memset(p, 0, EFS_OPID_UUID_LEN + 4);
    *len = n;
    return EFS_OK;
}

/* Same encoding as sim pack_unlink. Session bytes are zero (not hosted). */
static int pack_unlink_cmd(uint8_t *out, uint32_t *len, efs_ino_t parent,
                           uint64_t now, const char *name)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = 18 + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_UNLINK;
    wr64be(out + 1, parent);
    wr64be(out + 9, now);
    out[17] = (uint8_t)nl;
    memcpy(out + 18, name, nl);
    p = out + 18 + nl;
    memset(p, 0, EFS_OPID_UUID_LEN + 4);
    *len = n;
    return EFS_OK;
}

static uint32_t pack_prep(uint8_t *out, int kind, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t expected, int op,
                          const uint8_t *val, uint32_t vlen)
{
    uint32_t n, i;

    out[0] = EFS_MD_CMD_PREPARE;
    out[1] = (uint8_t)kind;
    memcpy(out + 2, t->bytes, 16);
    out[18] = p->n;
    n = 19;
    for (i = 0; i < p->n; i++) {
        wr32be(out + n, p->shard[i]);
        n += 4;
    }
    out[n++] = (uint8_t)klen;
    memcpy(out + n, key, klen);
    n += klen;
    wr64be(out + n, expected);
    out[n + 8] = (uint8_t)op;
    wr32be(out + n + 9, vlen);
    n += 13;
    if (vlen) {
        memcpy(out + n, val, vlen);
        n += vlen;
    }
    return n;
}

static void pack_decide(uint8_t *out, const struct efs_txid *t, uint32_t coord,
                        int dec)
{
    out[0] = EFS_MD_CMD_DECIDE;
    memcpy(out + 1, t->bytes, 16);
    wr32be(out + 17, coord);
    out[21] = (uint8_t)dec;
}

static void pack_resolve(uint8_t *out, const struct efs_txid *t, uint32_t shard,
                         int dec)
{
    out[0] = EFS_MD_CMD_RESOLVE;
    memcpy(out + 1, t->bytes, 16);
    wr32be(out + 17, shard);
    out[21] = (uint8_t)dec;
}

static void pack_drop(uint8_t *out, const struct efs_txid *t, uint32_t shard)
{
    out[0] = EFS_MD_CMD_DROP;
    memcpy(out + 1, t->bytes, 16);
    wr32be(out + 17, shard);
}

static void fill_txid(struct efs_raft_host *h, struct efs_txid *t)
{
    uint64_t a = now_ns();
    uint64_t b = h->boot_id ^ h->salt ^ ((uint64_t)h->raft_id << 32);

    memcpy(t->bytes, &a, 8);
    memcpy(t->bytes + 8, &b, 8);
}

static int host_txn_coord(void *user, const struct efs_txid *t,
                          uint32_t coord_shard, int *dec)
{
    struct efs_raft_host *h = user;
    uint8_t group;
    int hint = -1;
    int rc;

    if (!h || !h->kv || !t || !dec)
        return EFS_ERR_IO;
    group = efs_raft_shard_group(coord_shard);
    rc = host_read_index(h, group, &hint);
    if (rc != EFS_OK)
        return EFS_ERR_IO; /* I9: cannot establish authority, never absence */
    return efs_txn_decision_get(h->kv, coord_shard, t, dec);
}

static void stat_to_inode(const struct efs_meta_stat *st, struct efs_inode *ino)
{
    memset(ino, 0, sizeof(*ino));
    ino->ino = st->ino;
    ino->mode = st->mode;
    ino->nlink = st->nlink;
    ino->uid = (uid_t)st->uid;
    ino->gid = (gid_t)st->gid;
    ino->size = st->size;
    ino->mtime = st->mtime / 1000000000ull;
    ino->mtime_nsec = (uint32_t)(st->mtime % 1000000000ull);
    ino->ctime = st->ctime / 1000000000ull;
    ino->atime = st->atime / 1000000000ull;
}

static uint8_t rc_to_inode_status(int rc)
{
    if (rc == EFS_OK)
        return EFS_INODE_RPC_OK;
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_INODE_RPC_NOT_FOUND;
    if (rc == EFS_ERR_NOT_PRIMARY)
        return EFS_INODE_RPC_NOT_PRIMARY;
    if (rc == EFS_ERR_BUSY)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_INVAL)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_EXIST)
        return EFS_INODE_RPC_EXIST;
    if (rc == EFS_ERR_NAMETOOLONG)
        return EFS_INODE_RPC_INVAL;
    return EFS_INODE_RPC_ERROR;
}

static void set_inode_rc(struct efs_msg_inode_reply *out, int rc, int leader_hint)
{
    out->status = rc_to_inode_status(rc);
    out->primary_id = (leader_hint >= 0) ? (efs_node_id_t)(leader_hint + 1) : 0;
}

static int host_read_inode_lanes(struct efs_raft_host *h, efs_ino_t ino,
                                 int *leader_hint)
{
    struct efs_meta_row row;
    uint32_t ish;
    uint8_t ig;
    uint64_t bits;
    uint32_t i;
    int rc;

    ish = efs_kv_inode_shard(ino);
    ig = efs_raft_shard_group(ish);
    rc = host_read_index(h, ig, leader_hint);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
               ? row.used_shards
               : row.active_lanes;
    for (i = 0; i < EFS_META_LANES; i++) {
        uint32_t lsh;
        uint8_t lg;
        int hint = -1;

        if ((bits & (1ULL << i)) == 0)
            continue;
        lsh = efs_kv_lane_shard(ino, (uint8_t)i);
        lg = efs_raft_shard_group(lsh);
        if (lg == ig)
            continue;
        rc = host_read_index(h, lg, &hint);
        if (rc != EFS_OK) {
            *leader_hint = hint;
            return rc;
        }
    }
    return EFS_OK;
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
    pthread_mutex_init(&h->read_mu, NULL);
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
    pthread_mutex_destroy(&h->read_mu);
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

int server_raft_host_active(void)
{
    struct efs_raft_host *h = g_host;
    return h && h->running;
}

void server_raft_host_getattr(efs_ino_t ino, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_stat st;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

void server_raft_host_lookup(efs_ino_t parent, const char *name,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    uint32_t psh;
    uint8_t pg;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    psh = efs_kv_inode_shard(parent);
    pg = efs_raft_shard_group(psh);
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, pg, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL) {
        uint32_t hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        uint8_t hg = efs_raft_shard_group(hsh);
        int hh = -1;
        if (hg != pg) {
            rc = host_read_index(h, hg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, dent.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* File CREATE: one Raft entry on the dentry shard (co-located with a
 * LOCAL parent). MKDIR is a 2-shard txn and is INVAL here. */
void server_raft_host_create(efs_ino_t parent, const char *name, uint32_t mode,
                             uint32_t uid, uint32_t gid,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_meta_attrs at;
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t clen = 0;
    uint64_t idx = 0;
    uint32_t dsh = 0;
    uint8_t pg, dg = 0;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mode & S_IFMT) == 0)
        mode |= S_IFREG;
    if (S_ISDIR(mode)) {
        server_raft_host_mkdir(parent, name, mode, uid, gid, out);
        return;
    }
    at.uid = uid;
    at.gid = gid;
    at.now = now_ns();
    rc = pack_create_cmd(cmd, &clen, parent, mode, name, &at);
    if (rc != EFS_OK) {
        set_inode_rc(out, rc, -1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(parent)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        dg = efs_raft_shard_group(dsh);
        pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
        if (dg != pg) {
            int hh = -1;
            rc = host_read_index(h, dg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
        if (rc == EFS_OK) {
            pthread_mutex_unlock(&h->read_mu);
            set_inode_rc(out, EFS_ERR_EXIST, hint);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    if (rc == EFS_OK)
        rc = host_propose(h, dg, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, dg, idx, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, dent.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

static int host_prep(struct efs_raft_host *h, uint32_t shard, int kind,
                     const struct efs_txid *t, const struct efs_txn_parts *p,
                     const uint8_t *key, uint32_t klen, uint64_t expected,
                     int op, const uint8_t *val, uint32_t vlen, int *hint)
{
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t n;

    n = pack_prep(cmd, kind, t, p, key, klen, expected, op, val, vlen);
    if (n > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    return host_propose_wait(h, efs_raft_shard_group(shard), cmd, n, hint);
}

static int host_drop_parts(struct efs_raft_host *h, const struct efs_txid *t,
                           const struct efs_txn_parts *p, int *hint)
{
    uint8_t cmd[21];
    int rc = EFS_OK, i, one;

    for (i = 0; i < p->n; i++) {
        pack_drop(cmd, t, p->shard[i]);
        one = host_propose_wait(h, efs_raft_shard_group(p->shard[i]), cmd, 21,
                                hint);
        if (one != EFS_OK && rc == EFS_OK)
            rc = one;
    }
    return rc;
}

void server_raft_host_mkdir(efs_ino_t parent, const char *name, uint32_t mode,
                            uint32_t uid, uint32_t gid,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, crow;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_pino[EFS_META_INO_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint8_t v_dseq[8], sb[8];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0, sn = 8;
    uint32_t psh, csh, coord;
    uint64_t pver = 0, aver = 0, sver = 0, dver = 0, salt = 0, seq = 0;
    uint64_t now;
    efs_ino_t next = 0, ino = 0;
    uint8_t cmd[22];
    int hint = -1;
    int rc, i, gr;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mode & S_IFMT) != S_IFDIR)
        mode = S_IFDIR | (mode & 07777);
    if ((mode & 07777) == 0)
        mode |= 0755;
    now = now_ns();
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(EFS_ROOT_INO)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_export_salt(h->kv, &salt);
    psh = efs_kv_inode_shard(parent);
    csh = (rc == EFS_OK) ? efs_kv_mkdir_shard(parent, name, salt) : 0;
    if (rc == EFS_OK)
        rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(csh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(csh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK) {
        pthread_mutex_unlock(&h->read_mu);
        set_inode_rc(out, EFS_ERR_EXIST, hint);
        return;
    }
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    if (rc == EFS_OK)
        rc = efs_meta_apply_peek_alloc(h->kv, csh, &next);
    if (rc == EFS_OK) {
        ino = next;
        if (efs_kv_inode_shard(ino) != csh)
            rc = EFS_ERR_PROTO;
        next = ino + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
    }
    if (rc == EFS_OK) {
        memset(&crow, 0, sizeof(crow));
        crow.ino = ino;
        crow.generation = 1;
        crow.mode = mode;
        crow.nlink = 2;
        crow.parent = parent;
        crow.uid = uid;
        crow.gid = gid;
        crow.base_mtime = now;
        crow.base_atime = now;
        crow.base_ctime = now;
        memset(&dent, 0, sizeof(dent));
        dent.ino = ino;
        dent.generation = 1;
        dent.type = S_IFDIR;
        prow.nlink++;
        if (prow.base_mtime < now)
            prow.base_mtime = now;
        if (prow.base_ctime < now)
            prow.base_ctime = now;
        rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&prow, v_pino, sizeof(v_pino));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&crow, v_cino, sizeof(v_cino));
        wr64be(v_alloc, next);
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(psh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(psh, parent, 0, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_alloc, ka, &aver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        if (psh == csh) {
            parts.n = 1;
            parts.shard[0] = psh;
        } else {
            parts.n = 2;
            if (psh < csh) {
                parts.shard[0] = psh;
                parts.shard[1] = csh;
            } else {
                parts.shard[0] = csh;
                parts.shard[1] = psh;
            }
        }
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == psh) {
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pino,
                                   kpi, pver, EFS_TXN_PUT, v_pino,
                                   sizeof(v_pino), &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_dseq,
                                   ks, sver, EFS_TXN_PUT, v_dseq, 8, &hint);
            }
            if (rc == EFS_OK && sh == csh) {
                rc = host_prep(h, csh, EFS_TXN_EXCL, &t, &parts, k_cino, kci,
                               0, EFS_TXN_PUT, v_cino, sizeof(v_cino),
                               &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, csh, EFS_TXN_EXCL, &t, &parts, k_alloc,
                                   ka, aver, EFS_TXN_PUT, v_alloc,
                                   sizeof(v_alloc), &hint);
            }
        }
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, &hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   &hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, &hint);
            }
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, dent.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, dent.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* Last-link file UNLINK: one Raft entry on the dentry shard. Directories
 * (RMDIR) and nlink>1 (2-shard) are INVAL here. */
void server_raft_host_unlink(efs_ino_t parent, const char *name, int is_dir,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    uint8_t cmd[HOST_CMD_MAX];
    uint32_t clen = 0;
    uint64_t idx = 0;
    uint32_t dsh = 0;
    uint8_t pg, dg = 0, ig;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (is_dir) {
        out->status = EFS_INODE_RPC_INVAL; /* RMDIR is a txn */
        return;
    }
    rc = pack_unlink_cmd(cmd, &clen, parent, now_ns(), name);
    if (rc != EFS_OK) {
        set_inode_rc(out, rc, -1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(parent)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        dg = efs_raft_shard_group(dsh);
        pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
        if (dg != pg) {
            int hh = -1;
            rc = host_read_index(h, dg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_resolve(h->kv, parent, name, &dent, &row);
        if (rc == EFS_OK) {
            if (S_ISDIR(row.mode) || row.nlink > 1)
                rc = EFS_ERR_INVAL;
            else {
                ig = efs_raft_shard_group(efs_kv_inode_shard(row.ino));
                if (ig != dg)
                    rc = EFS_ERR_INVAL; /* 2-shard last-link later */
            }
        }
    }
    if (rc == EFS_OK)
        rc = host_propose(h, dg, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, dg, idx, &hint);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        out->inode.ino = row.ino;
        out->inode.mode = row.mode;
        out->inode.nlink = row.nlink;
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}
