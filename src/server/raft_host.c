#include "server_internal.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/dir_layout.h"
#include "efs/opid.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/lock.h"
#include "efs/metadata.h"
#include "efs/wire.h"
#include "efs/network.h"
#include <errno.h>
#include <fcntl.h>
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
/* Pump holds h->mu across cfg.send. A dead peer must not sit on the
 * 30s pool SO_RCVTIMEO or election cannot tick. Restore the pool
 * timeout before release so bounce RPCs keep the long budget. */
#define HOST_SEND_IO_MS    250
#define HOST_INBOX_MAX     256
#define HOST_ENCODE_STACK  (64 * 1024)
#define HOST_NGROUPS       2
#define HOST_READ_TRIES    80 /* 80 × 5 ms = 400 ms; heartbeat is 50 ms */
#define HOST_CREATE_NAME_OFF 31
#define HOST_CMD_MAX       512
#define HOST_SETATTR_LEN   61
#define HOST_UTIMENS_LEN   73
#define HOST_TRUNC_LEN     54
#define HOST_APPEND_RSV_LEN 45
#define HOST_APPEND_RES_LEN 38
#define HOST_TRUNC_TAIL    (4u + 8u + 8u + 4u + \
                            (uint32_t)EFS_NUM_FRAGMENTS * (4u + EFS_HASH_SIZE))
#define HOST_PUBLISH_LEN   (29u + (uint32_t)EFS_NUM_FRAGMENTS * (4u + EFS_HASH_SIZE) + \
                            EFS_OPID_UUID_LEN + 4u + 8u + 8u + 8u + 4u)
#define HOST_DIR_LEN       10
#define HOST_SESS_LEASE_LEN 38 /* tag+sub+uuid+epoch+ino+gen */
#define HOST_LOCK_LEN 65 /* tag+kind+ino+gen+dom+type+range+owner; sim pack_lock */

struct host_inbox_item {
    uint8_t *buf;
    uint32_t len;
};

struct host_group {
    uint8_t group;
    uint8_t hosted;
    uint32_t voters;
    uint64_t applied_saved;
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
    if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
        efs_set_recv_timeout(pc->fd, HOST_SEND_IO_MS);
        efs_set_send_timeout(pc->fd, HOST_SEND_IO_MS);
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
    if (pc->kind == EFS_CONN_TCP && pc->fd >= 0) {
        efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
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

/* Same encoding as sim apply_setattr_cmd. Session fencing is not hosted.
 * Apply never stalls the log. */
static int apply_setattr_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_setattr sa;
    efs_ino_t ino;
    uint64_t now;
    int rc;

    if (clen < HOST_SETATTR_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    memset(&sa, 0, sizeof(sa));
    sa.expect_gen = rd64be(cmd + 17);
    sa.mask = rd32be(cmd + 25);
    sa.mode = rd32be(cmd + 29);
    sa.uid = rd32be(cmd + 33);
    sa.gid = rd32be(cmd + 37);
    rc = efs_meta_apply_setattr(h->kv, ino, now, &sa);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK; /* replay / stale handle after a later unlink */
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply setattr rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied setattr index=%llu ino=%llu mask=%u\n",
            (unsigned long long)index, (unsigned long long)ino, sa.mask);
    return EFS_OK;
}

/* Same encoding as sim apply_utimens_cmd. Session fencing is not hosted. */
static int apply_utimens_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_utimens u;
    efs_ino_t ino;
    uint64_t now;
    int rc;

    if (clen < HOST_UTIMENS_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    memset(&u, 0, sizeof(u));
    u.expect_gen = rd64be(cmd + 17);
    u.mask = rd32be(cmd + 25);
    u.mtime = rd64be(cmd + 29);
    u.atime = rd64be(cmd + 37);
    u.mtime_gen = rd64be(cmd + 45);
    rc = efs_meta_apply_utimens(h->kv, ino, now, &u);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply utimens rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied utimens index=%llu ino=%llu mask=%u\n",
            (unsigned long long)index, (unsigned long long)ino, u.mask);
    return EFS_OK;
}

/* Same encoding as sim apply_truncate_cmd. Session fencing is not hosted. */
static int apply_truncate_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                              uint32_t clen, uint64_t index)
{
    struct efs_meta_truncate t;
    struct efs_meta_pub tail;
    efs_ino_t ino;
    uint64_t now;
    const uint8_t *q;
    int i, rc;

    if (clen < HOST_TRUNC_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    now = rd64be(cmd + 9);
    memset(&t, 0, sizeof(t));
    t.expect_gen = rd64be(cmd + 17);
    t.size = rd64be(cmd + 25);
    if (cmd[33]) {
        if (clen < HOST_TRUNC_LEN + HOST_TRUNC_TAIL)
            return EFS_OK;
        q = cmd + HOST_TRUNC_LEN;
        memset(&tail, 0, sizeof(tail));
        tail.ino = ino;
        tail.chunk_index = rd32be(q);
        tail.new_size = t.size;
        tail.candidate_gen = rd64be(q + 4);
        tail.expected_gen = rd64be(q + 12);
        tail.coding_profile_id = rd32be(q + 20);
        q += 24;
        for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            tail.ch.nodes[i] = rd32be(q);
            q += 4;
            memcpy(tail.ch.checksums[i], q, EFS_HASH_SIZE);
            q += EFS_HASH_SIZE;
        }
        tail.now = now;
        t.tail = &tail;
    }
    rc = efs_meta_apply_truncate(h->kv, ino, now, &t);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply truncate rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied truncate index=%llu ino=%llu size=%llu\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned long long)t.size);
    return EFS_OK;
}

/* Pump-safe: apply already holds the committed KV. Must not ReadIndex. */
static int host_apply_coord(void *user, const struct efs_txid *t,
                            uint32_t coord_shard, int *dec)
{
    struct efs_raft_host *h = user;

    if (!h || !h->kv || !t || !dec)
        return EFS_ERR_IO;
    return efs_txn_decision_get(h->kv, coord_shard, t, dec);
}

/* Same layout as sim apply_append_rsv_cmd, big-endian. Session / op-id
 * are not hosted: UUID and seq are zero. Apply never stalls the log. */
static int apply_append_rsv_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                uint32_t clen, uint64_t index)
{
    struct efs_opid op;
    efs_ino_t ino;
    uint64_t len, off = 0;
    int rc;

    if (clen < HOST_APPEND_RSV_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    len = rd64be(cmd + 9);
    memset(&op, 0, sizeof(op));
    memcpy(op.client_uuid, cmd + 17, EFS_OPID_UUID_LEN);
    op.session_epoch = rd32be(cmd + 33);
    op.seq = rd64be(cmd + 37);
    rc = efs_meta_apply_append_reserve(h->kv, ino, len, &op, host_apply_coord,
                                       h, &off);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply append-rsv rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied append-rsv index=%llu ino=%llu\n",
            (unsigned long long)index, (unsigned long long)ino);
    return EFS_OK;
}

static int apply_append_res_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                                uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    uint64_t off;
    int outcome, rc;

    if (clen < HOST_APPEND_RES_LEN)
        return EFS_OK;
    ino = rd64be(cmd + 1);
    off = rd64be(cmd + 9);
    outcome = (int)cmd[17];
    rc = efs_meta_apply_append_resolve(h->kv, ino, off, outcome);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply append-res rc=%d index=%llu ino=%llu\n",
                rc, (unsigned long long)index, (unsigned long long)ino);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied append-res index=%llu ino=%llu off=%llu\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned long long)off);
    return EFS_OK;
}

/* Same encoding as sim_dir_apply. Empty LOCAL → HASHED for the smoke;
 * migrate of a non-empty dir that writes a hashed dentry on another
 * group is not hosted (SPLITTING dest CREATE stays BUSY). */
static int apply_dir_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                         uint32_t clen, uint64_t index)
{
    efs_ino_t dir;
    int rc = EFS_ERR_PROTO;

    if (clen < HOST_DIR_LEN)
        return EFS_OK;
    dir = rd64be(cmd + 2);
    switch (cmd[1]) {
    case EFS_MD_DIR_BEGIN:
        rc = efs_meta_dir_begin_split(h->kv, dir);
        break;
    case EFS_MD_DIR_MIGRATE:
        rc = efs_meta_dir_migrate_one(h->kv, dir);
        break;
    case EFS_MD_DIR_FINISH:
        rc = efs_meta_dir_finish_hashed(h->kv, dir);
        break;
    default:
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply dir rc=%d index=%llu ino=%llu kind=%u\n",
                rc, (unsigned long long)index, (unsigned long long)dir,
                (unsigned)cmd[1]);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied dir index=%llu ino=%llu kind=%u\n",
            (unsigned long long)index, (unsigned long long)dir,
            (unsigned)cmd[1]);
    return EFS_OK;
}

/* Same encoding as sim_sess_apply (LEASE_OPEN/CLOSE/RECLAIM). Session
 * create/fence is not hosted; apply never stalls the log. Last close
 * reclaims a nlink=0 inode (I19). */
static int apply_session_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch = 0;
    efs_ino_t ino = 0;
    uint64_t gen = 0;
    int rc = EFS_ERR_PROTO;

    if (clen < 18)
        return EFS_OK;
    memcpy(uuid, cmd + 2, EFS_OPID_UUID_LEN);
    switch (cmd[1]) {
    case EFS_MD_SESS_LEASE_OPEN:
        if (clen < HOST_SESS_LEASE_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        ino = rd64be(cmd + 22);
        gen = rd64be(cmd + 30);
        rc = efs_lease_open(h->kv, ino, gen, uuid, epoch);
        break;
    case EFS_MD_SESS_LEASE_CLOSE:
        if (clen < HOST_SESS_LEASE_LEN)
            return EFS_OK;
        epoch = rd32be(cmd + 18);
        ino = rd64be(cmd + 22);
        gen = rd64be(cmd + 30);
        rc = efs_lease_close(h->kv, ino, gen, uuid, epoch);
        if (rc == EFS_OK) {
            int r2 = efs_meta_apply_reclaim(h->kv, ino);

            if (r2 != EFS_OK && r2 != EFS_ERR_BUSY && r2 != EFS_ERR_NOT_FOUND)
                rc = r2;
        }
        break;
    case EFS_MD_SESS_RECLAIM:
        if (clen < 26)
            return EFS_OK;
        ino = rd64be(cmd + 18);
        rc = efs_meta_apply_reclaim(h->kv, ino);
        break;
    default:
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY ||
        rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr,
                "raft-host: apply session rc=%d index=%llu ino=%llu kind=%u\n",
                rc, (unsigned long long)index, (unsigned long long)ino,
                (unsigned)cmd[1]);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied session index=%llu ino=%llu kind=%u\n",
            (unsigned long long)index, (unsigned long long)ino,
            (unsigned)cmd[1]);
    return EFS_OK;
}

/* Same encoding as sim_lock_apply. Conflict/cap/stale never stall the
 * log; the propose path returns BUSY to the client. Blocking wait
 * queues stay leader memory (not hosted). */
static int apply_lock_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                          uint32_t clen, uint64_t index)
{
    struct efs_lock_req r;
    const uint8_t *p;
    int rc = EFS_ERR_PROTO;

    if (clen < HOST_LOCK_LEN)
        return EFS_OK;
    memset(&r, 0, sizeof(r));
    r.ino = rd64be(cmd + 2);
    r.generation = rd64be(cmd + 10);
    r.domain = cmd[18];
    r.type = cmd[19];
    r.start = rd64be(cmd + 20);
    r.end = rd64be(cmd + 28);
    r.owner.kind = cmd[36];
    r.owner.id = rd64be(cmd + 37);
    p = cmd + 45;
    memcpy(r.owner.uuid, p, EFS_OPID_UUID_LEN);
    r.owner.epoch = rd32be(p + EFS_OPID_UUID_LEN);
    if (cmd[1] == EFS_MD_LOCK_GRANT)
        rc = efs_lock_grant(h->kv, &r);
    else if (cmd[1] == EFS_MD_LOCK_RELEASE)
        rc = efs_lock_release(h->kv, &r);
    else
        return EFS_OK;
    if (rc == EFS_ERR_AGAIN || rc == EFS_ERR_NOLCK || rc == EFS_ERR_NOT_FOUND ||
        rc == EFS_ERR_INVAL || rc == EFS_ERR_BUSY || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr,
                "raft-host: apply lock rc=%d index=%llu ino=%llu kind=%u\n",
                rc, (unsigned long long)index, (unsigned long long)r.ino,
                (unsigned)cmd[1]);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied lock index=%llu ino=%llu kind=%u\n",
            (unsigned long long)index, (unsigned long long)r.ino,
            (unsigned)cmd[1]);
    return EFS_OK;
}

/* Same encoding as sim apply_publish_cmd. Session fencing is not hosted. */
static int apply_publish_cmd(struct efs_raft_host *h, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_pub p;
    const uint8_t *q;
    int i, rc;

    if (clen < HOST_PUBLISH_LEN)
        return EFS_OK;
    memset(&p, 0, sizeof(p));
    p.ino = rd64be(cmd + 1);
    p.chunk_index = rd32be(cmd + 9);
    p.new_size = rd64be(cmd + 13);
    p.now = rd64be(cmd + 21);
    q = cmd + 29;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = rd32be(q);
        q += 4;
        memcpy(p.ch.checksums[i], q, EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    q += EFS_OPID_UUID_LEN + 4;
    p.candidate_gen = rd64be(q);
    p.expected_gen = rd64be(q + 8);
    p.content_epoch = rd64be(q + 16);
    p.coding_profile_id = rd32be(q + 24);
    rc = efs_meta_apply_publish(h->kv, &p);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
        rc = EFS_OK;
    if (rc != EFS_OK) {
        fprintf(stderr, "raft-host: apply publish rc=%d index=%llu ino=%llu ci=%u\n",
                rc, (unsigned long long)index, (unsigned long long)p.ino,
                p.chunk_index);
        return EFS_OK;
    }
    fprintf(stderr, "raft-host: applied publish index=%llu ino=%llu ci=%u\n",
            (unsigned long long)index, (unsigned long long)p.ino, p.chunk_index);
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
    if (cmd[0] == EFS_MD_CMD_PUBLISH)
        return apply_publish_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_SETATTR)
        return apply_setattr_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_UTIMENS)
        return apply_utimens_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_TRUNCATE)
        return apply_truncate_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_APPEND_RSV)
        return apply_append_rsv_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_APPEND_RES)
        return apply_append_res_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_DIR)
        return apply_dir_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_SESSION)
        return apply_session_cmd(h, cmd, clen, index);
    if (cmd[0] == EFS_MD_CMD_LOCK)
        return apply_lock_cmd(h, cmd, clen, index);
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

static struct host_group *group_slot(struct efs_raft_host *h, uint8_t group)
{
    int i;
    for (i = 0; i < HOST_NGROUPS; i++) {
        if (h->g[i].group == group)
            return &h->g[i];
    }
    return NULL;
}

static int host_hosts(struct efs_raft_host *h, uint8_t group)
{
    return group_raft(h, group) != NULL;
}

/* First peer other than self (and skip) that votes in every listed group.
 * Dual-hosts of {0,2} are raft ids 1 and 2 at n=4. Never returns self. */
static int host_pick_peer(struct efs_raft_host *h, const uint8_t *groups, int ng,
                          int skip)
{
    int rid, i;

    for (rid = 0; rid < h->n; rid++) {
        int ok = 1;
        if (rid == h->raft_id || rid == skip)
            continue;
        for (i = 0; i < ng; i++) {
            struct host_group *s = group_slot(h, groups[i]);
            uint32_t voters = s ? s->voters : group_voters(groups[i], h->n);
            if (!hosts_group(rid, voters)) {
                ok = 0;
                break;
            }
        }
        if (ok)
            return rid;
    }
    return -1;
}

/* Both scratch groups. HASHED used_shards / file lanes can sit on the
 * group this replica does not host; bounce to a dual-host instead of
 * returning NOT_PRIMARY to the client. */
static void host_need_both(uint8_t *need)
{
    need[0] = EFS_RAFT_GROUP_SHARD;
    need[1] = EFS_RAFT_GROUP_SHARD2;
}

static int host_rpc_submit(struct efs_raft_host *h, int rid, uint8_t group,
                           const uint8_t *cmd, uint32_t clen,
                           struct efs_msg_raft_mkfs_reply *rep)
{
    uint8_t payload[1 + HOST_CMD_MAX];
    uint32_t plen;
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;

    memset(rep, 0, sizeof(*rep));
    if (rid < 0 || rid == h->raft_id || clen > HOST_CMD_MAX)
        return EFS_ERR_INVAL;
    payload[0] = group;
    if (clen)
        memcpy(payload + 1, cmd, clen);
    plen = 1u + clen;
    if (peer_addr(h, rid, host, sizeof(host), &port) != 0)
        return EFS_ERR_NOT_PRIMARY;
    pc = server_peer_conn_get(host, port);
    if (!pc)
        return EFS_ERR_BUSY;
    if (efs_conn_send_msg(pc, EFS_MSG_RAFT_MKFS, payload, plen) != 0) {
        server_peer_conn_drop(host, port, pc);
        return EFS_ERR_IO;
    }
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||
        rtype != EFS_MSG_RAFT_MKFS_REPLY || rlen < sizeof(*rep)) {
        free(reply);
        server_peer_conn_drop(host, port, pc);
        return EFS_ERR_IO;
    }
    memcpy(rep, reply, sizeof(*rep));
    free(reply);
    server_peer_conn_release(host, port, pc);
    return EFS_OK;
}

/* Submit cmd (or ReadIndex if clen==0) to the group's leader. Never targets
 * self. prefer_rid is a hint; NOT_PRIMARY replies retry leader_hint. */
static int host_remote_cmd(struct efs_raft_host *h, uint8_t group,
                           const uint8_t *cmd, uint32_t clen,
                           struct efs_msg_raft_mkfs_reply *rep, int prefer_rid)
{
    int attempt, rid, skip = -1, rc;

    for (attempt = 0; attempt < h->n + 2; attempt++) {
        rid = prefer_rid;
        if (rid < 0 || rid == h->raft_id || rid == skip)
            rid = host_pick_peer(h, &group, 1, skip);
        if (rid < 0 || rid == h->raft_id)
            return EFS_ERR_NOT_PRIMARY;
        rc = host_rpc_submit(h, rid, group, cmd, clen, rep);
        if (rc != EFS_OK) {
            skip = rid;
            prefer_rid = -1;
            continue;
        }
        if (rep->rc == EFS_OK)
            return EFS_OK;
        if (rep->rc == EFS_ERR_NOT_PRIMARY && rep->leader_hint >= 0 &&
            rep->leader_hint != h->raft_id && rep->leader_hint != rid) {
            prefer_rid = rep->leader_hint;
            skip = rid;
            continue;
        }
        return rep->rc != 0 ? rep->rc : EFS_ERR_NOT_PRIMARY;
    }
    return EFS_ERR_NOT_PRIMARY;
}

static int host_wait_applied(struct efs_raft_host *h, uint8_t group,
                             uint64_t idx, int *leader_hint);

/* Drop h->mu while waiting: the pump must tick for heartbeats to land.
 * Caller serializes with read_mu. A follower ReadIndexes the leader
 * (RAFT_MKFS group-only) then waits until the local replica has applied
 * that index. Unhosted groups stay NOT_PRIMARY — the KV is not here. */
static int host_read_index(struct efs_raft_host *h, uint8_t group,
                           int *leader_hint)
{
    int begun = 0;
    int t;
    int lid = -1;

    if (leader_hint)
        *leader_hint = -1;
    pthread_mutex_lock(&h->mu);
    {
        struct efs_raft *r = group_raft(h, group);
        if (!r) {
            pthread_mutex_unlock(&h->mu);
            return EFS_ERR_NOT_PRIMARY;
        }
        lid = efs_raft_leader(r);
        if (leader_hint)
            *leader_hint = lid;
        if (efs_raft_role(r) != EFS_RAFT_LEADER) {
            pthread_mutex_unlock(&h->mu);
            {
                struct efs_msg_raft_mkfs_reply rep;
                int rc = host_remote_cmd(h, group, NULL, 0, &rep, lid);
                if (rc != EFS_OK)
                    return rc;
                if (leader_hint && rep.leader_hint >= 0)
                    *leader_hint = rep.leader_hint;
                return host_wait_applied(h, group, rep.index, leader_hint);
            }
        }
    }
    pthread_mutex_unlock(&h->mu);

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
            lid = efs_raft_leader(r);
            pthread_mutex_unlock(&h->mu);
            if (lid == h->raft_id)
                return EFS_ERR_NOT_PRIMARY;
            {
                struct efs_msg_raft_mkfs_reply rep;
                int rc2 = host_remote_cmd(h, group, NULL, 0, &rep, lid);
                if (rc2 != EFS_OK)
                    return rc2;
                if (leader_hint && rep.leader_hint >= 0)
                    *leader_hint = rep.leader_hint;
                return host_wait_applied(h, group, rep.index, leader_hint);
            }
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
            /* No local replica: the leader already waited in submit. */
            pthread_mutex_unlock(&h->mu);
            return EFS_OK;
        }
        if (leader_hint)
            *leader_hint = efs_raft_leader(r);
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
    int lid = -1;
    struct efs_msg_raft_mkfs_reply rep;

    pthread_mutex_lock(&h->mu);
    r = group_raft(h, group);
    if (r) {
        lid = efs_raft_leader(r);
        if (leader_hint)
            *leader_hint = lid;
        if (efs_raft_role(r) == EFS_RAFT_LEADER) {
            rc = efs_raft_propose(r, cmd, clen, idx);
            pthread_mutex_unlock(&h->mu);
            return rc;
        }
    }
    pthread_mutex_unlock(&h->mu);
    rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);
    if (rc != EFS_OK)
        return rc;
    if (idx)
        *idx = rep.index;
    if (leader_hint && rep.leader_hint >= 0)
        *leader_hint = rep.leader_hint;
    return EFS_OK;
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

static int host_inode_rpc_peer(struct efs_raft_host *h, int rid, uint8_t req_type,
                               const void *req, uint32_t reqlen,
                               uint8_t reply_type, void *out, uint32_t outlen)
{
    char host[64];
    uint16_t port = 0;
    struct efs_conn *pc;
    uint8_t rtype = 0;
    void *reply = NULL;
    uint32_t rlen = 0;

    if (rid < 0 || rid == h->raft_id || !req || !out || outlen == 0)
        return -1;
    if (peer_addr(h, rid, host, sizeof(host), &port) != 0)
        return -1;
    pc = server_peer_conn_get(host, port);
    if (!pc)
        return -1;
    if (efs_conn_send_msg(pc, req_type, req, reqlen) != 0) {
        server_peer_conn_drop(host, port, pc);
        return -1;
    }
    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0) {
        server_peer_conn_drop(host, port, pc);
        return -1;
    }
    if (rtype != reply_type || rlen < outlen) {
        free(reply);
        server_peer_conn_drop(host, port, pc);
        return -1;
    }
    memcpy(out, reply, outlen);
    free(reply);
    server_peer_conn_release(host, port, pc);
    return 0;
}

/* Bounce an inode RPC to a peer that hosts every listed group. Never
 * targets self, so a dual-host that hosts all groups never forwards
 * (no 1↔2 loop). Caller must not hold read_mu. */
static void host_inode_forward(struct efs_raft_host *h, uint8_t req_type,
                               const void *req, uint32_t reqlen,
                               uint8_t reply_type,
                               struct efs_msg_inode_reply *out,
                               const uint8_t *groups, int ng)
{
    int tries, rid, skip = -1;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, req_type, req, reqlen, reply_type,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

static void host_fwd_create(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, uint32_t mode, uint32_t uid,
                            uint32_t gid, struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_create req;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    strncpy(req.name, name, EFS_MAX_NAME - 1);
    req.mode = mode;
    req.uid = uid;
    req.gid = gid;
    host_inode_forward(h, EFS_MSG_INODE_CREATE, &req, sizeof(req),
                       EFS_MSG_INODE_CREATE_REPLY, out, groups, ng);
}

static void host_fwd_lookup(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_lookup req;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    strncpy(req.name, name, EFS_MAX_NAME - 1);
    host_inode_forward(h, EFS_MSG_INODE_LOOKUP, &req, sizeof(req),
                       EFS_MSG_INODE_LOOKUP_REPLY, out, groups, ng);
}

static void host_fwd_getattr(struct efs_raft_host *h, efs_ino_t ino,
                             struct efs_msg_inode_reply *out,
                             const uint8_t *groups, int ng)
{
    struct efs_msg_inode_getattr req;

    memset(&req, 0, sizeof(req));
    req.ino = ino;
    host_inode_forward(h, EFS_MSG_INODE_GETATTR, &req, sizeof(req),
                       EFS_MSG_INODE_GETATTR_REPLY, out, groups, ng);
}

static void host_fwd_hold(struct efs_raft_host *h, efs_ino_t ino, uint32_t flags,
                          uint64_t owner, struct efs_msg_inode_reply *out,
                          const uint8_t *groups, int ng)
{
    struct efs_msg_inode_hold req;

    memset(&req, 0, sizeof(req));
    req.ino = ino;
    req.flags = flags;
    req.owner = owner;
    host_inode_forward(h, EFS_MSG_INODE_HOLD, &req, sizeof(req),
                       EFS_MSG_INODE_HOLD_REPLY, out, groups, ng);
}

static void host_fwd_flock(struct efs_raft_host *h, efs_ino_t ino, uint32_t op,
                           uint64_t owner, uint64_t start, uint64_t end,
                           struct efs_msg_inode_reply *out,
                           const uint8_t *groups, int ng)
{
    uint8_t buf[sizeof(struct efs_msg_inode_flock) + EFS_FLOCK_RANGE_LEN];
    struct efs_msg_inode_flock *req = (struct efs_msg_inode_flock *)buf;

    memset(buf, 0, sizeof(buf));
    req->ino = ino;
    req->op = op;
    req->owner = owner;
    memcpy(buf + sizeof(*req), &start, 8);
    memcpy(buf + sizeof(*req) + 8, &end, 8);
    host_inode_forward(h, EFS_MSG_INODE_FLOCK, buf, sizeof(buf),
                       EFS_MSG_INODE_FLOCK_REPLY, out, groups, ng);
}

static void host_fwd_append(struct efs_raft_host *h, efs_ino_t ino, uint64_t len,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_append req;

    memset(&req, 0, sizeof(req));
    req.ino = ino;
    req.len = len;
    host_inode_forward(h, EFS_MSG_INODE_APPEND, &req, sizeof(req),
                       EFS_MSG_INODE_APPEND_REPLY, out, groups, ng);
}

static void host_fwd_unlink(struct efs_raft_host *h, efs_ino_t parent,
                            const char *name, int is_dir,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_unlink req;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    strncpy(req.name, name, EFS_MAX_NAME - 1);
    req.is_dir = is_dir ? 1 : 0;
    host_inode_forward(h, EFS_MSG_INODE_UNLINK, &req, sizeof(req),
                       EFS_MSG_INODE_UNLINK_REPLY, out, groups, ng);
}

static void host_fwd_link(struct efs_raft_host *h, efs_ino_t src_ino,
                          efs_ino_t new_parent, const char *new_name,
                          struct efs_msg_inode_reply *out,
                          const uint8_t *groups, int ng)
{
    struct efs_msg_inode_link req;

    memset(&req, 0, sizeof(req));
    req.src_ino = src_ino;
    req.new_parent = new_parent;
    strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    host_inode_forward(h, EFS_MSG_INODE_LINK, &req, sizeof(req),
                       EFS_MSG_INODE_LINK_REPLY, out, groups, ng);
}

static void host_fwd_rename(struct efs_raft_host *h, efs_ino_t old_parent,
                            const char *old_name, efs_ino_t new_parent,
                            const char *new_name,
                            struct efs_msg_inode_reply *out,
                            const uint8_t *groups, int ng)
{
    struct efs_msg_inode_rename_at req;

    memset(&req, 0, sizeof(req));
    req.old_parent = old_parent;
    strncpy(req.old_name, old_name, EFS_MAX_NAME - 1);
    req.new_parent = new_parent;
    strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    host_inode_forward(h, EFS_MSG_INODE_RENAME_AT, &req, sizeof(req),
                       EFS_MSG_INODE_RENAME_AT_REPLY, out, groups, ng);
}

static void host_fwd_lookup_path(struct efs_raft_host *h, efs_ino_t start,
                                 const char *path,
                                 struct efs_msg_inode_lookup_path_reply *out,
                                 const uint8_t *groups, int ng)
{
    struct efs_msg_inode_lookup_path req;
    int tries, rid, skip = -1;

    memset(&req, 0, sizeof(req));
    req.start = start;
    if (path)
        strncpy(req.path, path, sizeof(req.path) - 1);
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, EFS_MSG_INODE_LOOKUP_PATH, &req,
                                sizeof(req), EFS_MSG_INODE_LOOKUP_PATH_REPLY,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
}

static void host_fwd_readdir(struct efs_raft_host *h, efs_ino_t parent,
                             uint32_t max_ents, uint64_t after_ino,
                             struct efs_msg_inode_readdir_reply *out,
                             const uint8_t *groups, int ng)
{
    struct efs_msg_inode_readdir req;
    int tries, rid, skip = -1;

    memset(&req, 0, sizeof(req));
    req.parent = parent;
    req.max_ents = max_ents;
    req.after_ino = after_ino;
    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_NOT_PRIMARY;
    for (tries = 0; tries < h->n; tries++) {
        rid = host_pick_peer(h, groups, ng, skip);
        if (rid < 0)
            return;
        if (host_inode_rpc_peer(h, rid, EFS_MSG_INODE_READDIR, &req,
                                sizeof(req), EFS_MSG_INODE_READDIR_REPLY,
                                out, sizeof(*out)) != 0) {
            skip = rid;
            out->status = EFS_INODE_RPC_NOT_PRIMARY;
            continue;
        }
        if (out->status != EFS_INODE_RPC_NOT_PRIMARY)
            return;
        skip = rid;
    }
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

/* Same encoding as sim_raft_setattr. Session bytes are zero (not hosted). */
static int pack_setattr_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                            uint64_t now, const struct efs_meta_setattr *sa)
{
    if (!sa)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_SETATTR;
    wr64be(out + 1, ino);
    wr64be(out + 9, now);
    wr64be(out + 17, sa->expect_gen);
    wr32be(out + 25, sa->mask);
    wr32be(out + 29, sa->mode);
    wr32be(out + 33, sa->uid);
    wr32be(out + 37, sa->gid);
    memset(out + 41, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_SETATTR_LEN;
    return EFS_OK;
}

/* Same encoding as sim_raft_utimens. Session bytes are zero (not hosted). */
static int pack_utimens_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                            uint64_t now, const struct efs_meta_utimens *u)
{
    if (!u)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_UTIMENS;
    wr64be(out + 1, ino);
    wr64be(out + 9, now);
    wr64be(out + 17, u->expect_gen);
    wr32be(out + 25, u->mask);
    wr64be(out + 29, u->mtime);
    wr64be(out + 37, u->atime);
    wr64be(out + 45, u->mtime_gen);
    memset(out + 53, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_UTIMENS_LEN;
    return EFS_OK;
}

/* Same encoding as sim_raft_truncate. Unaligned sizes carry a tail CAS. */
static int pack_truncate_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                             uint64_t now, uint64_t expect_gen, uint64_t size,
                             const struct efs_meta_pub *tail)
{
    uint8_t *q;
    int i;

    out[0] = EFS_MD_CMD_TRUNCATE;
    wr64be(out + 1, ino);
    wr64be(out + 9, now);
    wr64be(out + 17, expect_gen);
    wr64be(out + 25, size);
    out[33] = tail ? 1 : 0;
    memset(out + 34, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_TRUNC_LEN;
    if (!tail)
        return EFS_OK;
    q = out + HOST_TRUNC_LEN;
    wr32be(q, tail->chunk_index);
    wr64be(q + 4, tail->candidate_gen);
    wr64be(q + 12, tail->expected_gen);
    wr32be(q + 20, tail->coding_profile_id);
    q += 24;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        wr32be(q, tail->ch.nodes[i]);
        q += 4;
        memcpy(q, tail->ch.checksums[i], EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    *len = HOST_TRUNC_LEN + HOST_TRUNC_TAIL;
    return EFS_OK;
}

static int pack_append_rsv_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                               uint64_t alen)
{
    out[0] = EFS_MD_CMD_APPEND_RSV;
    wr64be(out + 1, ino);
    wr64be(out + 9, alen);
    memset(out + 17, 0, EFS_OPID_UUID_LEN + 4 + 8);
    *len = HOST_APPEND_RSV_LEN;
    return EFS_OK;
}

static int pack_append_res_cmd(uint8_t *out, uint32_t *len, efs_ino_t ino,
                               uint64_t off, int outcome)
{
    out[0] = EFS_MD_CMD_APPEND_RES;
    wr64be(out + 1, ino);
    wr64be(out + 9, off);
    out[17] = (uint8_t)outcome;
    memset(out + 18, 0, EFS_OPID_UUID_LEN + 4);
    *len = HOST_APPEND_RES_LEN;
    return EFS_OK;
}

/* Same encoding as sim_sess LEASE_OPEN/CLOSE. owner is the session
 * identity until sessions are hosted (zero UUID + epoch 1). */
static void host_hold_uuid(uint64_t owner, uint8_t uuid[EFS_OPID_UUID_LEN])
{
    memset(uuid, 0, EFS_OPID_UUID_LEN);
    wr64be(uuid, owner);
}

static int pack_lease_cmd(uint8_t *out, uint32_t *len, int open, efs_ino_t ino,
                          uint64_t gen, uint64_t owner)
{
    uint8_t uuid[EFS_OPID_UUID_LEN];

    host_hold_uuid(owner, uuid);
    out[0] = EFS_MD_CMD_SESSION;
    out[1] = open ? EFS_MD_SESS_LEASE_OPEN : EFS_MD_SESS_LEASE_CLOSE;
    memcpy(out + 2, uuid, EFS_OPID_UUID_LEN);
    wr32be(out + 18, 1); /* epoch; sessions not hosted */
    wr64be(out + 22, ino);
    wr64be(out + 30, gen);
    *len = HOST_SESS_LEASE_LEN;
    return EFS_OK;
}

/* Same encoding as sim pack_lock. Owner is the session stand-in until
 * sessions are hosted (zero UUID + epoch 1). */
static int pack_lock_cmd(uint8_t *out, uint32_t *len, uint8_t kind,
                         const struct efs_lock_req *r)
{
    uint8_t *p;

    if (!r)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_LOCK;
    out[1] = kind;
    wr64be(out + 2, r->ino);
    wr64be(out + 10, r->generation);
    out[18] = r->domain;
    out[19] = r->type;
    wr64be(out + 20, r->start);
    wr64be(out + 28, r->end);
    out[36] = r->owner.kind;
    wr64be(out + 37, r->owner.id);
    p = out + 45;
    memcpy(p, r->owner.uuid, EFS_OPID_UUID_LEN);
    wr32be(p + EFS_OPID_UUID_LEN, r->owner.epoch);
    *len = HOST_LOCK_LEN;
    return EFS_OK;
}

static void fill_flock_req(struct efs_lock_req *r, efs_ino_t ino, uint64_t gen,
                           uint8_t type, uint64_t owner, uint8_t domain,
                           uint64_t start, uint64_t end)
{
    memset(r, 0, sizeof(*r));
    r->ino = ino;
    r->generation = gen;
    r->domain = domain;
    r->type = type;
    r->start = start;
    r->end = end;
    /* Classic fcntl = process token; flock (and OFD fcntl) = OFD. */
    r->owner.kind = (domain == EFS_LOCK_FCNTL) ? EFS_LOCK_PROC : EFS_LOCK_OFD;
    r->owner.id = owner;
    host_hold_uuid(owner, r->owner.uuid);
    r->owner.epoch = 1;
}

/* Same encoding as sim pack_publish. Session bytes are zero (not hosted). */
static int pack_publish_cmd(uint8_t *out, uint32_t *len, const struct efs_meta_pub *p)
{
    uint8_t *q;
    int i;

    if (!p)
        return EFS_ERR_INVAL;
    out[0] = EFS_MD_CMD_PUBLISH;
    wr64be(out + 1, p->ino);
    wr32be(out + 9, p->chunk_index);
    wr64be(out + 13, p->new_size);
    wr64be(out + 21, p->now);
    q = out + 29;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        wr32be(q, p->ch.nodes[i]);
        q += 4;
        memcpy(q, p->ch.checksums[i], EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    memset(q, 0, EFS_OPID_UUID_LEN + 4);
    q += EFS_OPID_UUID_LEN + 4;
    wr64be(q, p->candidate_gen);
    wr64be(q + 8, p->expected_gen);
    wr64be(q + 16, p->content_epoch);
    wr32be(q + 24, p->coding_profile_id);
    *len = HOST_PUBLISH_LEN;
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
    if (rc == EFS_ERR_AGAIN)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_NOLCK)
        return EFS_INODE_RPC_BUSY;
    if (rc == EFS_ERR_INVAL)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_EXIST)
        return EFS_INODE_RPC_EXIST;
    if (rc == EFS_ERR_NAMETOOLONG)
        return EFS_INODE_RPC_INVAL;
    if (rc == EFS_ERR_NOT_EMPTY)
        return EFS_INODE_RPC_NOT_EMPTY;
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

/* Persist last_applied without compacting the log. Restart restores it so
 * CREATE is not re-applied onto a KV that already ran a later rename. */
static void applied_path(struct efs_raft_host *h, uint8_t group,
                         char *path, size_t cap)
{
    snprintf(path, cap, "%s/mdraft/applied.%u", h->s->storage_path, group);
}

static int persist_applied(struct efs_raft_host *h, int gi)
{
    uint64_t idx;
    char path[EFS_MAX_PATH], tmp[EFS_MAX_PATH];
    uint8_t buf[8];
    int fd, n, rc;

    if (!h->g[gi].hosted || !h->g[gi].r)
        return EFS_OK;
    idx = efs_raft_applied(h->g[gi].r);
    if (idx == 0 || idx == h->g[gi].applied_saved)
        return EFS_OK;
    applied_path(h, h->g[gi].group, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    wr64be(buf, idx);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    n = (int)write(fd, buf, 8);
    rc = fsync(fd);
    close(fd);
    if (n != 8 || rc != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    h->g[gi].applied_saved = idx;
    return EFS_OK;
}

static int load_applied(struct efs_raft_host *h, int gi, uint64_t *idx)
{
    char path[EFS_MAX_PATH];
    uint8_t buf[8];
    int fd, n;

    *idx = 0;
    applied_path(h, h->g[gi].group, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return EFS_OK;
    n = (int)read(fd, buf, 8);
    close(fd);
    if (n != 8)
        return EFS_OK;
    *idx = rd64be(buf);
    return EFS_OK;
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
        for (i = 0; i < HOST_NGROUPS; i++)
            (void)persist_applied(h, i);
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
    {
        uint64_t applied = 0;
        if (load_applied(h, gi, &applied) == EFS_OK && applied > 0) {
            (void)efs_raft_restore_applied(h->g[gi].r, applied);
            h->g[gi].applied_saved = efs_raft_applied(h->g[gi].r);
        }
    }
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

/* Payload: group byte, then command bytes (empty command = ReadIndex).
 * A hosted replica proposes (or ReadIndexes) even when it is not the
 * leader; an unhosted node forwards a non-empty command to a voter.
 * Holds read_mu so a submit cannot interleave with a local inode
 * handler on this node. */
void server_raft_host_submit(const uint8_t *payload, uint32_t plen,
                             struct efs_msg_raft_mkfs_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_raft *r;
    uint8_t group;
    const uint8_t *cmd;
    uint32_t clen;
    uint64_t idx = 0;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->leader_hint = -1;
    if (!h || !h->running || !payload || plen < 1) {
        out->rc = EFS_ERR_INVAL;
        return;
    }
    group = payload[0];
    cmd = payload + 1;
    clen = plen - 1;
    if (!host_hosts(h, group)) {
        /* Unhosted: bounce to a voter. ReadIndex still requires a local
         * replica (inode handlers bounce the inode RPC instead). */
        if (clen == 0) {
            out->rc = EFS_ERR_NOT_PRIMARY;
            return;
        }
        rc = host_remote_cmd(h, group, cmd, clen, out, -1);
        if (rc != EFS_OK)
            out->rc = rc;
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    if (clen == 0) {
        rc = host_read_index(h, group, &hint);
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (r)
            idx = efs_raft_applied(r);
        pthread_mutex_unlock(&h->mu);
    } else {
        rc = host_propose_wait(h, group, cmd, clen, &hint);
        pthread_mutex_lock(&h->mu);
        r = group_raft(h, group);
        if (r)
            idx = efs_raft_applied(r);
        pthread_mutex_unlock(&h->mu);
    }
    pthread_mutex_unlock(&h->read_mu);
    out->rc = rc;
    out->index = idx;
    if (hint >= 0)
        out->leader_hint = hint;
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
    {
        uint8_t ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
        if (!host_hosts(h, ig)) {
            host_fwd_getattr(h, ino, out, &ig, 1);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        uint8_t need[2];

        pthread_mutex_unlock(&h->read_mu);
        host_need_both(need);
        host_fwd_getattr(h, ino, out, need, 2);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

/* Open lease (I19): one Raft entry on the inode shard. flags=1 open,
 * flags=0 close. owner is the session stand-in. Directories INVAL.
 * Last close reclaims a nlink=0 inode. Sessions/fencing are not hosted. */
void server_raft_host_hold(efs_ino_t ino, uint32_t flags, uint64_t owner,
                           struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_stat st;
    uint8_t cmd[HOST_SESS_LEASE_LEN];
    uint32_t clen = 0;
    uint8_t ig;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (ino == 0 || (flags != 0 && flags != 1)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        host_fwd_hold(h, ino, flags, owner, out, &ig, 1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        pthread_mutex_unlock(&h->read_mu);
        host_fwd_hold(h, ino, flags, owner, out, &ig, 1);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = pack_lease_cmd(cmd, &clen, flags ? 1 : 0, ino, row.generation,
                            owner);
    if (rc == EFS_OK)
        rc = host_propose_wait(h, ig, cmd, clen, &hint);
    if (rc == EFS_OK) {
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
        if (flags == 0 && rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK && flags == 1)
        stat_to_inode(&st, &out->inode);
}

/* Non-blocking flock/fcntl on the inode shard (§7.6). EFS_FLOCK_FCNTL
 * selects the record-lock domain (byte ranges allowed); otherwise
 * FLOCK (whole-file only). Owner is the session stand-in. Conflict →
 * BUSY. EFS_FLOCK_GETLK is a ReadIndex (no Raft entry). Blocking wait
 * queues are not hosted. */
void server_raft_host_flock(efs_ino_t ino, uint32_t op, uint64_t owner,
                            uint64_t start, uint64_t end,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_lock_req req;
    struct efs_lock_req hit;
    uint8_t cmd[HOST_LOCK_LEN];
    uint32_t clen = 0;
    uint8_t ig;
    uint8_t kind;
    uint8_t ltype;
    uint8_t domain;
    int hint = -1;
    int rc;
    int blk;
    int is_getlk = (op & EFS_FLOCK_GETLK) ? 1 : 0;

    memset(out, 0, sizeof(*out));
    memset(&hit, 0, sizeof(hit));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (start >= end) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (is_getlk) {
        if (op & EFS_FLOCK_UN) {
            out->status = EFS_INODE_RPC_INVAL;
            return;
        }
        kind = 0;
        if (op & EFS_FLOCK_EX)
            ltype = EFS_LOCK_EX;
        else if (op & EFS_FLOCK_SH)
            ltype = EFS_LOCK_SH;
        else {
            out->status = EFS_INODE_RPC_INVAL;
            return;
        }
    } else if (op & EFS_FLOCK_UN) {
        kind = EFS_MD_LOCK_RELEASE;
        ltype = EFS_LOCK_EX;
    } else if (op & EFS_FLOCK_EX) {
        kind = EFS_MD_LOCK_GRANT;
        ltype = EFS_LOCK_EX;
    } else if (op & EFS_FLOCK_SH) {
        kind = EFS_MD_LOCK_GRANT;
        ltype = EFS_LOCK_SH;
    } else {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    domain = (op & EFS_FLOCK_FCNTL) ? EFS_LOCK_FCNTL : EFS_LOCK_FLOCK;
    if (domain == EFS_LOCK_FLOCK &&
        (start != 0 || end != ~(uint64_t)0)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        host_fwd_flock(h, ino, op, owner, start, end, out, &ig, 1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        pthread_mutex_unlock(&h->read_mu);
        host_fwd_flock(h, ino, op, owner, start, end, out, &ig, 1);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK) {
        fill_flock_req(&req, ino, row.generation, ltype, owner, domain,
                       start, end);
        if (is_getlk) {
            memset(&hit, 0, sizeof(hit));
            rc = efs_lock_getlk(h->kv, &req, &hit);
        } else if (kind == EFS_MD_LOCK_GRANT) {
            blk = efs_lock_blocked(h->kv, &req, NULL);
            if (blk < 0)
                rc = blk;
            else if (blk)
                rc = EFS_ERR_AGAIN;
        }
    }
    if (!is_getlk) {
        if (rc == EFS_OK)
            rc = pack_lock_cmd(cmd, &clen, kind, &req);
        if (rc == EFS_OK)
            rc = host_propose_wait(h, ig, cmd, clen, &hint);
        if (rc == EFS_OK && kind == EFS_MD_LOCK_GRANT) {
            blk = efs_lock_blocked(h->kv, &req, NULL);
            if (blk < 0)
                rc = blk;
            else if (blk)
                rc = EFS_ERR_AGAIN;
        }
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (is_getlk && rc == EFS_OK) {
        out->inode.nlink = hit.type;
        out->inode.size = hit.start;
        out->inode.ctime = hit.end;
        out->inode.ino = (efs_ino_t)hit.owner.id;
    }
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
    if (!host_hosts(h, pg)) {
        uint8_t need[2];
        need[0] = pg;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_lookup(h, parent, name, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, pg, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL) {
        uint32_t hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        uint8_t hg = efs_raft_shard_group(hsh);
        int hh = -1;
        if (hg != pg) {
            if (!host_hosts(h, hg)) {
                uint8_t need[2];
                need[0] = pg;
                need[1] = hg;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_lookup(h, parent, name, out, need, 2);
                return;
            }
            rc = host_read_index(h, hg, &hh);
            if (rc != EFS_OK)
                hint = hh;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK) {
        uint8_t cg = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
        if (!host_hosts(h, cg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (cg != pg)
                    need[nn++] = cg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_lookup(h, parent, name, out, need, nn);
            return;
        }
        rc = host_read_inode_lanes(h, dent.ino, &hint);
        if (rc == EFS_ERR_NOT_PRIMARY) {
            uint8_t need[2];

            pthread_mutex_unlock(&h->read_mu);
            host_need_both(need);
            host_fwd_lookup(h, parent, name, out, need, 2);
            return;
        }
    }
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

/* File CREATE: one Raft entry on the dentry shard when that shard's
 * group already has everything the apply writes (LOCAL parent, or a
 * HASHED dest whose used_shards bit is set, or first-use on the same
 * Raft group as the parent). First use of a HASHED dir lane on another
 * group is a 2-shard txn (parent used_shards + dest dentry/inode).
 * SPLITTING dest is BUSY. MKDIR is a 2-shard txn of its own. */
static int host_hashed_create_txn(struct efs_raft_host *h, efs_ino_t parent,
                                  const char *name, uint32_t mode,
                                  const struct efs_meta_attrs *at,
                                  struct efs_meta_row *prow, uint32_t dsh,
                                  int *hint);

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
    pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
    if (!host_hosts(h, pg)) {
        uint8_t need[2];
        need[0] = pg;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_create(h, parent, name, mode, uid, gid, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, pg, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        dg = efs_raft_shard_group(dsh);
        if (!host_hosts(h, dg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (dg != pg)
                need[nn++] = dg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_create(h, parent, name, mode, uid, gid, out, need, nn);
            return;
        }
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
    if (rc == EFS_OK && prow.layout == EFS_META_LAYOUT_HASHED &&
        (prow.used_shards & (1ull << efs_kv_dir_lane(name))) == 0 &&
        pg != dg)
        rc = host_hashed_create_txn(h, parent, name, mode, &at, &prow, dsh,
                                    &hint);
    else if (rc == EFS_OK) {
        rc = host_propose(h, dg, cmd, clen, &idx, &hint);
        if (rc == EFS_OK)
            rc = host_wait_applied(h, dg, idx, &hint);
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

static int host_parts_add(struct efs_txn_parts *p, uint32_t shard)
{
    uint8_t i;

    if (!p)
        return EFS_ERR_INVAL;
    for (i = 0; i < p->n; i++) {
        if (p->shard[i] == shard)
            return EFS_OK;
    }
    if (p->n >= EFS_TXN_MAX_PART)
        return EFS_ERR_BUSY;
    p->shard[p->n++] = shard;
    return EFS_OK;
}

struct host_pver_guard {
    uint32_t shard;
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen;
    uint64_t ver;
};

/* Ancestry of dst_parent as shared pver GUARDs (not exclusive on the
 * inode row). src in the chain is INVAL. Too many distinct shards is
 * BUSY (EFS_TXN_MAX_PART). read_mu held. */
static int host_pver_guard_chain(struct efs_raft_host *h, efs_ino_t dst_parent,
                                 efs_ino_t src, struct efs_txn_parts *parts,
                                 struct host_pver_guard *g, int *ng, int *hint)
{
    efs_ino_t cur = dst_parent;
    int hops, rc;

    *ng = 0;
    for (hops = 0; hops < 64; hops++) {
        struct efs_meta_row r;
        uint32_t sh;

        if (cur == src)
            return EFS_ERR_INVAL;
        sh = efs_kv_inode_shard(cur);
        rc = host_read_index(h, efs_raft_shard_group(sh), hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_get_inode(h->kv, cur, &r);
        if (rc != EFS_OK)
            return rc;
        if (!S_ISDIR(r.mode))
            return EFS_ERR_INVAL;
        if (*ng >= EFS_TXN_MAX_PART)
            return EFS_ERR_BUSY;
        rc = efs_kv_key_pver(sh, cur, g[*ng].key, &g[*ng].klen);
        if (rc == EFS_OK)
            rc = efs_txn_ver_get(h->kv, g[*ng].key, g[*ng].klen, &g[*ng].ver);
        if (rc == EFS_OK)
            rc = host_parts_add(parts, sh);
        if (rc != EFS_OK)
            return rc;
        g[*ng].shard = sh;
        (*ng)++;
        if (cur == EFS_ROOT_INO || cur == r.parent)
            return EFS_OK;
        cur = r.parent;
    }
    return EFS_ERR_INVAL;
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

/* First use of a HASHED dir lane whose dentry shard is on a different
 * Raft group than the parent inode. Parent used_shards is exclusive;
 * dest dentry, child inode, alloc, dseq, and dir-lane ride the dest
 * shard. Parent mtime/ctime stay on the dir-lane (not the home row). */
static int host_hashed_create_txn(struct efs_raft_host *h, efs_ino_t parent,
                                  const char *name, uint32_t mode,
                                  const struct efs_meta_attrs *at,
                                  struct efs_meta_row *prow, uint32_t dsh,
                                  int *hint)
{
    struct efs_meta_row crow;
    struct efs_meta_dentry dent;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_pino[EFS_META_INO_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint8_t v_dseq[8], v_ln[EFS_META_LANE_BYTES], sb[8];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0, kln = 0, sn = 8;
    uint32_t psh, coord;
    uint64_t pver = 0, aver = 0, sver = 0, dver = 0, lver = 0, seq = 0;
    uint64_t bit;
    uint8_t lane, cmd[22];
    efs_ino_t next = 0, ino = 0;
    int rc, i, gr;

    psh = efs_kv_inode_shard(parent);
    lane = efs_kv_dir_lane(name);
    bit = 1ull << lane;
    prow->used_shards |= bit;
    rc = efs_meta_apply_peek_alloc(h->kv, dsh, &next);
    if (rc == EFS_OK) {
        ino = next;
        if (efs_kv_inode_shard(ino) != dsh)
            rc = EFS_ERR_PROTO;
        next = ino + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
    }
    if (rc == EFS_OK) {
        memset(&crow, 0, sizeof(crow));
        crow.ino = ino;
        crow.generation = 1;
        crow.mode = mode;
        crow.nlink = 1;
        crow.parent = parent;
        crow.uid = at->uid;
        crow.gid = at->gid;
        crow.base_mtime = at->now;
        crow.base_atime = at->now;
        crow.base_ctime = at->now;
        memset(&dent, 0, sizeof(dent));
        dent.ino = ino;
        dent.generation = 1;
        dent.type = mode & S_IFMT;
        rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(prow, v_pino, sizeof(v_pino));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&crow, v_cino, sizeof(v_cino));
        wr64be(v_alloc, next);
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(dsh, ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(dsh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, lane, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_meta_stamp_dir_lane(h->kv, prow, name, at->now, k_ln, &kln,
                                     v_ln, sizeof(v_ln));
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_alloc, ka, &aver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ln, kln, &lver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        parts.n = 2;
        if (psh < dsh) {
            parts.shard[0] = psh;
            parts.shard[1] = dsh;
        } else {
            parts.shard[0] = dsh;
            parts.shard[1] = psh;
        }
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == psh)
                rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pino, kpi,
                               pver, EFS_TXN_PUT, v_pino, sizeof(v_pino),
                               hint);
            if (rc == EFS_OK && sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_cino,
                                   kci, 0, EFS_TXN_PUT, v_cino, sizeof(v_cino),
                                   hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_alloc,
                                   ka, aver, EFS_TXN_PUT, v_alloc,
                                   sizeof(v_alloc), hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq,
                                   ks, sver, EFS_TXN_PUT, v_dseq, 8, hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_ln, kln,
                                   lver, EFS_TXN_PUT, v_ln, sizeof(v_ln),
                                   hint);
            }
        }
        if (rc != EFS_OK)
            (void)host_drop_parts(h, &t, &parts, hint);
        else {
            coord = efs_txn_coordinator(&t, &parts);
            pack_decide(cmd, &t, coord, EFS_TXN_COMMIT);
            rc = host_propose_wait(h, efs_raft_shard_group(coord), cmd, 22,
                                   hint);
            for (i = 0; i < parts.n && rc == EFS_OK; i++) {
                pack_resolve(cmd, &t, parts.shard[i], EFS_TXN_COMMIT);
                rc = host_propose_wait(h, efs_raft_shard_group(parts.shard[i]),
                                       cmd, 22, hint);
            }
        }
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
    psh = efs_kv_inode_shard(parent);
    if (!host_hosts(h, efs_raft_shard_group(psh))) {
        uint8_t need[2];
        need[0] = EFS_RAFT_GROUP_SHARD;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_create(h, parent, name, mode, uid, gid, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(EFS_ROOT_INO)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_export_salt(h->kv, &salt);
    csh = (rc == EFS_OK) ? efs_kv_mkdir_shard(parent, name, salt) : 0;
    if (rc == EFS_OK && !host_hosts(h, efs_raft_shard_group(csh))) {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(psh);
        if (efs_raft_shard_group(csh) != need[0])
            need[nn++] = efs_raft_shard_group(csh);
        pthread_mutex_unlock(&h->read_mu);
        host_fwd_create(h, parent, name, mode, uid, gid, out, need, nn);
        return;
    }
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

/* LOCAL empty RMDIR: 2-shard txn (parent dentry+row+dseq, child inode).
 * A node that does not host a participant group bounces the inode RPC
 * to a dual-host; a dual-host submits to a group it does not lead.
 * HASHED/SPLITTING dirs are INVAL/BUSY here (dseq-on-used-lanes is a
 * later slice). */
void server_raft_host_rmdir(efs_ino_t parent, const char *name,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent one;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_pdseq[EFS_KV_KEY_MAX], k_cdseq[EFS_KV_KEY_MAX];
    uint8_t v_pino[EFS_META_INO_BYTES], v_pdseq[8], sb[8];
    uint32_t kd = 0, kpi = 0, kci = 0, kps = 0, kcs = 0, sn = 8, nent = 0;
    uint32_t psh, csh, coord;
    uint64_t pver = 0, dver = 0, sver = 0, cver = 0, gver = 0, seq = 0, now;
    uint8_t cmd[22];
    int hint = -1;
    int rc, i, gr, held;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
    psh = efs_kv_inode_shard(parent);
    if (!host_hosts(h, efs_raft_shard_group(psh))) {
        uint8_t need[2];
        need[0] = EFS_RAFT_GROUP_SHARD;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_unlink(h, parent, name, 1, out, need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, parent, name, &dent);
    if (rc == EFS_OK && dent.ino == EFS_ROOT_INO)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && (dent.type & S_IFMT) != S_IFDIR)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        csh = efs_kv_inode_shard(dent.ino);
        if (!host_hosts(h, efs_raft_shard_group(csh))) {
            uint8_t need[2];
            int nn = 1;
            need[0] = efs_raft_shard_group(psh);
            if (efs_raft_shard_group(csh) != need[0])
                need[nn++] = efs_raft_shard_group(csh);
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_unlink(h, parent, name, 1, out, need, nn);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve(h->kv, parent, name, &dent, &row);
    if (rc == EFS_OK && (!S_ISDIR(row.mode) || row.ino == EFS_ROOT_INO))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && row.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    if (rc == EFS_OK && row.layout != EFS_META_LAYOUT_LOCAL)
        rc = EFS_ERR_INVAL;
    csh = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && efs_raft_shard_group(csh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(csh), &hint);
    if (rc == EFS_OK && prow.nlink < 3)
        rc = EFS_ERR_PROTO;
    if (rc == EFS_OK && row.nlink > 2)
        rc = EFS_ERR_NOT_EMPTY;
    if (rc == EFS_OK) {
        memset(&cur, 0, sizeof(cur));
        memset(&one, 0, sizeof(one));
        nent = 0;
        rc = efs_meta_apply_readdir(h->kv, row.ino, &cur, &one, 1, &nent);
        if (rc == EFS_OK && nent > 0)
            rc = EFS_ERR_NOT_EMPTY;
    }
    if (rc == EFS_OK) {
        held = efs_lease_any(h->kv, row.ino, row.generation);
        if (held < 0)
            rc = held;
        else if (held)
            rc = EFS_ERR_BUSY;
    }
    if (rc == EFS_OK) {
        prow.nlink--;
        if (prow.base_mtime < now)
            prow.base_mtime = now;
        if (prow.base_ctime < now)
            prow.base_ctime = now;
        rc = efs_meta_pack_inode(&prow, v_pino, sizeof(v_pino));
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(psh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, row.ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(psh, parent, 0, k_pdseq, &kps);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(csh, row.ino, 0, k_cdseq, &kcs);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_pdseq, kps, &sver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_cino, kci, &cver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_cdseq, kcs, &gver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_pdseq, kps, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_pdseq, seq + 1);
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
                               dver, EFS_TXN_DEL, NULL, 0, &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pino,
                                   kpi, pver, EFS_TXN_PUT, v_pino,
                                   sizeof(v_pino), &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, psh, EFS_TXN_EXCL, &t, &parts, k_pdseq,
                                   kps, sver, EFS_TXN_PUT, v_pdseq, 8, &hint);
            }
            if (rc == EFS_OK && sh == csh) {
                rc = host_prep(h, csh, EFS_TXN_EXCL, &t, &parts, k_cino, kci,
                               cver, EFS_TXN_DEL, NULL, 0, &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, csh, EFS_TXN_GUARD, &t, &parts, k_cdseq,
                                   kcs, gver, 0, NULL, 0, &hint);
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
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        out->inode.ino = row.ino;
        out->inode.mode = row.mode;
        out->inode.nlink = 2;
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* nlink>1 file UNLINK, or last-link when dentry shard ≠ inode shard:
 * dest dentry DEL + inode nlink-- (or inode DEL) as a txn (same
 * PREPARE/DECIDE/RESOLVE as LINK). LOCAL parent only; HASHED/SPLITTING
 * are INVAL/BUSY. Last-link on one shard stays on EFS_MD_CMD_UNLINK.
 * The receiving node must lead every participant group. */
static void host_unlink_txn(efs_ino_t parent, const char *name,
                            struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_ino[EFS_META_INO_BYTES], v_par[EFS_META_INO_BYTES], v_dseq[8];
    uint8_t sb[8], cmd[22];
    uint32_t kd = 0, ki = 0, kp = 0, ks = 0, sn = 8;
    uint32_t dsh, ish, psh, coord;
    uint64_t dver = 0, iver = 0, pver = 0, sver = 0, seq = 0, now;
    uint32_t nlink_out = 0;
    int hint = -1;
    int rc, i, gr, held = 0, last = 0, put_ino = 0;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !name || parent == 0 || name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
    pthread_mutex_lock(&h->read_mu);
    psh = efs_kv_inode_shard(parent);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        if (efs_raft_shard_group(dsh) != efs_raft_shard_group(psh))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    } else
        dsh = 0;
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve(h->kv, parent, name, &dent, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    ish = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(dsh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    if (rc == EFS_OK) {
        nlink_out = row.nlink;
        last = row.nlink <= 1;
        if (last) {
            held = efs_lease_any(h->kv, row.ino, row.generation);
            if (held < 0)
                rc = held;
            else if (held) {
                row.nlink = 0;
                if (row.base_ctime < now)
                    row.base_ctime = now;
                put_ino = 1;
            }
        } else {
            row.nlink--;
            if (row.base_ctime < now)
                row.base_ctime = now;
            put_ino = 1;
        }
    }
    if (rc == EFS_OK) {
        if (prow.base_mtime < now)
            prow.base_mtime = now;
        if (prow.base_ctime < now)
            prow.base_ctime = now;
        rc = efs_meta_pack_inode(&prow, v_par, sizeof(v_par));
        if (rc == EFS_OK && put_ino)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, parent, 0, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        if (dsh == ish) {
            parts.n = 1;
            parts.shard[0] = dsh;
        } else {
            parts.n = 2;
            if (dsh < ish) {
                parts.shard[0] = dsh;
                parts.shard[1] = ish;
            } else {
                parts.shard[0] = ish;
                parts.shard[1] = dsh;
            }
        }
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_DEL, NULL, 0, &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_par, kp,
                                   pver, EFS_TXN_PUT, v_par, sizeof(v_par),
                                   &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq, ks,
                                   sver, EFS_TXN_PUT, v_dseq, 8, &hint);
            }
            if (rc == EFS_OK && sh == ish) {
                if (put_ino)
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                                   iver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                                   &hint);
                else
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                                   iver, EFS_TXN_DEL, NULL, 0, &hint);
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
    if (rc == EFS_OK && !last) {
        rc = host_read_inode_lanes(h, row.ino, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, row.ino, host_txn_coord, h, &st);
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        if (!last)
            stat_to_inode(&st, &out->inode);
        else {
            out->inode.ino = row.ino;
            out->inode.mode = row.mode;
            out->inode.nlink = nlink_out;
        }
        out->inode.parent = parent;
        strncpy(out->inode.name, name, EFS_MAX_NAME - 1);
    }
}

/* Last-link file UNLINK on one shard: one Raft entry on the dentry
 * shard. nlink>1, or last-link with dsh ≠ ish, is the txn above.
 * Directories go through RMDIR. */
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
        server_raft_host_rmdir(parent, name, out);
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
        if (rc == EFS_OK && S_ISDIR(row.mode)) {
            pthread_mutex_unlock(&h->read_mu);
            server_raft_host_rmdir(parent, name, out);
            return;
        }
        if (rc == EFS_OK) {
            ig = efs_raft_shard_group(efs_kv_inode_shard(row.ino));
            if (row.nlink > 1 || ig != dg) {
                pthread_mutex_unlock(&h->read_mu);
                host_unlink_txn(parent, name, out);
                return;
            }
        }
    }
    if (rc == EFS_OK)
        rc = pack_unlink_cmd(cmd, &clen, parent, now_ns(), name);
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

/* Mode/owner SETATTR: one Raft entry on the inode shard. SIZE is INVAL
 * (truncate later). MTIME/ATIME is the utimens fence (below). Mixed
 * mode+time classes are INVAL — the §6 matrix splits them. */
static void host_utimens(efs_ino_t ino, uint32_t mask, uint64_t mtime,
                         uint32_t mtime_nsec, uint64_t atime,
                         struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_utimens u;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    uint8_t cmd[HOST_UTIMENS_LEN];
    uint32_t clen = 0;
    uint64_t idx = 0, bits;
    uint8_t g;
    int hint = -1;
    int rc;
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if ((mask & (EFS_SETATTR_MTIME | EFS_SETATTR_ATIME)) == 0 ||
        (mask & ~(EFS_SETATTR_MTIME | EFS_SETATTR_ATIME)) != 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    memset(&u, 0, sizeof(u));
    if (mask & EFS_SETATTR_MTIME) {
        u.mask |= EFS_META_SET_MTIME;
        u.mtime = mtime * 1000000000ull + (uint64_t)mtime_nsec;
    }
    if (mask & EFS_SETATTR_ATIME) {
        u.mask |= EFS_META_SET_ATIME;
        u.atime = atime * 1000000000ull;
    }
    u.expect_gen = 0;
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && (u.mask & EFS_META_SET_MTIME)) {
        u.mtime_gen = row.mtime_gen + 1;
        bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
                   ? row.used_shards
                   : row.active_lanes;
        for (i = 0; i < EFS_META_LANES && rc == EFS_OK; i++) {
            uint32_t lsh;
            uint8_t lg;

            if ((bits & (1ULL << i)) == 0)
                continue;
            lsh = efs_kv_lane_shard(ino, (uint8_t)i);
            lg = efs_raft_shard_group(lsh);
            if (lg != g)
                rc = EFS_ERR_INVAL; /* cross-group lane fence later */
        }
    }
    if (rc == EFS_OK)
        rc = pack_utimens_cmd(cmd, &clen, ino, now_ns(), &u);
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, g, idx, &hint);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

/* SETATTR SIZE: content_epoch fence + base_size. Unaligned sizes mint a
 * same-group tail candidate (CAS inside the truncate entry). */
static void host_truncate(efs_ino_t ino, uint64_t size,
                          struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    struct efs_meta_pub tail;
    struct efs_meta_chunk got;
    const struct efs_meta_pub *tp = NULL;
    uint8_t cmd[HOST_CMD_MAX];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t clen = 0, tci = 0, lsh = 0;
    uint64_t idx = 0, bits = 0, now;
    uint8_t g, lane = 0, lg = 0;
    int hint = -1;
    int rc;
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !S_ISREG(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        bits = row.active_lanes;
        for (i = 0; i < EFS_META_LANES && rc == EFS_OK; i++) {
            if ((bits & (1ULL << i)) == 0)
                continue;
            lsh = efs_kv_lane_shard(ino, (uint8_t)i);
            lg = efs_raft_shard_group(lsh);
            if (lg != g)
                rc = EFS_ERR_INVAL; /* cross-group lane fence later */
        }
    }
    if (rc == EFS_OK && size > 0 && (size % EFS_MIN_CHUNK_SIZE) != 0) {
        tci = (uint32_t)(size / EFS_MIN_CHUNK_SIZE);
        lane = (uint8_t)(tci % EFS_META_LANES);
        lsh = efs_kv_lane_shard(ino, lane);
        lg = efs_raft_shard_group(lsh);
        if (lg != g)
            rc = EFS_ERR_INVAL; /* tail CAS is same-group this slice */
        memset(&got, 0, sizeof(got));
        if (rc == EFS_OK)
            rc = efs_meta_apply_get_chunk(h->kv, ino, tci, &got);
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
        else if (rc != EFS_OK)
            ;
        if (rc == EFS_OK) {
            memset(&tail, 0, sizeof(tail));
            memset(uuid, 0, sizeof(uuid));
            tail.ino = ino;
            tail.chunk_index = tci;
            tail.new_size = size;
            tail.expected_gen = got.generation;
            tail.candidate_gen = efs_meta_candidate_gen(uuid, 0, 2, tci, 0);
            if (tail.candidate_gen == 0 ||
                tail.candidate_gen == tail.expected_gen)
                tail.candidate_gen = tail.expected_gen + 1;
            if (tail.candidate_gen == 0)
                tail.candidate_gen = 1;
            tail.coding_profile_id = EFS_META_PROFILE_K2F1;
            for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                tail.ch.nodes[i] = (efs_node_id_t)(i + 1);
                memset(tail.ch.checksums[i], (uint8_t)(0xa5 + i),
                       EFS_HASH_SIZE);
            }
            tp = &tail;
        }
    }
    now = now_ns();
    if (rc == EFS_OK)
        rc = pack_truncate_cmd(cmd, &clen, ino, now, 0, size, tp);
    if (rc == EFS_OK && clen > HOST_CMD_MAX)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, g, idx, &hint);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

void server_raft_host_setattr(efs_ino_t ino, uint32_t mask, uint32_t mode,
                              uint32_t uid, uint32_t gid, uint64_t size,
                              uint64_t mtime, uint32_t mtime_nsec,
                              uint64_t atime, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_setattr sa;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    uint8_t cmd[HOST_SETATTR_LEN];
    uint32_t clen = 0;
    uint64_t idx = 0;
    uint8_t g;
    int hint = -1;
    int rc;
    uint32_t own, times, sz;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    own = mask & (EFS_SETATTR_MODE | EFS_SETATTR_UID | EFS_SETATTR_GID);
    times = mask & (EFS_SETATTR_MTIME | EFS_SETATTR_ATIME);
    sz = mask & EFS_SETATTR_SIZE;
    if ((own && times) || (own && sz) || (times && sz)) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (sz) {
        if (mask != EFS_SETATTR_SIZE) {
            out->status = EFS_INODE_RPC_INVAL;
            return;
        }
        host_truncate(ino, size, out);
        return;
    }
    if (times) {
        host_utimens(ino, times, mtime, mtime_nsec, atime, out);
        return;
    }
    if (own == 0 || (mask & ~(EFS_SETATTR_MODE | EFS_SETATTR_UID |
                              EFS_SETATTR_GID)) != 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.mask = mask & (EFS_META_SET_MODE | EFS_META_SET_UID | EFS_META_SET_GID);
    sa.mode = mode;
    sa.uid = uid;
    sa.gid = gid;
    sa.expect_gen = 0;
    rc = pack_setattr_cmd(cmd, &clen, ino, now_ns(), &sa);
    if (rc != EFS_OK) {
        set_inode_rc(out, rc, -1);
        return;
    }
    g = efs_raft_shard_group(efs_kv_inode_shard(ino));
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, g, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK)
        rc = host_propose(h, g, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, g, idx, &hint);
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK)
        stat_to_inode(&st, &out->inode);
}

/* O_APPEND reserve. Reply size is the watermark (off+len). Visible
 * getattr size stays the frontier until REPORT resolves the rsv. */
void server_raft_host_append(efs_ino_t ino, uint64_t len,
                             struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_stat st;
    uint8_t cmd[HOST_APPEND_RSV_LEN];
    uint32_t clen = 0, nopen = 0;
    uint64_t idx = 0, wm = 0;
    uint8_t ig;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0 || len == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    if (!host_hosts(h, ig)) {
        host_fwd_append(h, ino, len, out, &ig, 1);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !S_ISREG(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = pack_append_rsv_cmd(cmd, &clen, ino, len);
    if (rc == EFS_OK)
        rc = host_propose(h, ig, cmd, clen, &idx, &hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, ig, idx, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_append_state(h->kv, ino, &wm, NULL, &nopen);
    if (rc == EFS_OK && nopen == 0)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.size = wm;
    }
}

/* Hard link: dest dentry + inode nlink++ as a txn (same PREPARE/DECIDE/
 * RESOLVE as MKDIR). LOCAL dest only; HASHED/SPLITTING are INVAL/BUSY.
 * The receiving node must lead every participant group. Directories are
 * INVAL. LINK_SHARD is not this path. */
void server_raft_host_link(efs_ino_t src_ino, efs_ino_t new_parent,
                           const char *new_name, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row dprow, row;
    struct efs_meta_dentry dent, ndent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES];
    uint8_t v_par[EFS_META_INO_BYTES], v_dseq[8], sb[8];
    uint32_t kd = 0, ki = 0, kp = 0, ks = 0, sn = 8;
    uint32_t dsh, ish, coord;
    uint64_t dver = 0, iver = 0, pver = 0, sver = 0, seq = 0, now;
    uint8_t cmd[22];
    int hint = -1;
    int rc, i, gr;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !new_name || src_ino == 0 || new_parent == 0 ||
        new_name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    now = now_ns();
    dsh = efs_kv_inode_shard(new_parent);
    ish = efs_kv_inode_shard(src_ino);
    if (!host_hosts(h, efs_raft_shard_group(dsh)) ||
        !host_hosts(h, efs_raft_shard_group(ish))) {
        uint8_t need[2];
        int nn = 1;
        need[0] = efs_raft_shard_group(dsh);
        if (efs_raft_shard_group(ish) != need[0])
            need[nn++] = efs_raft_shard_group(ish);
        host_fwd_link(h, src_ino, new_parent, new_name, out, need, nn);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(dsh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, new_parent, &dprow);
    if (rc == EFS_OK && !S_ISDIR(dprow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && dprow.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    if (rc == EFS_OK && dprow.layout != EFS_META_LAYOUT_LOCAL)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, src_ino, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        dsh = efs_kv_dentry_shard(new_parent, new_name, dprow.layout);
        if (efs_raft_shard_group(dsh) !=
            efs_raft_shard_group(efs_kv_inode_shard(new_parent)))
            rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    }
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup(h->kv, new_parent, new_name, &dent);
        if (rc == EFS_OK) {
            pthread_mutex_unlock(&h->read_mu);
            set_inode_rc(out, EFS_ERR_EXIST, hint);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    if (rc == EFS_OK) {
        memset(&ndent, 0, sizeof(ndent));
        ndent.ino = row.ino;
        ndent.generation = row.generation;
        ndent.type = row.mode & S_IFMT;
        row.nlink++;
        if (row.base_ctime < now)
            row.base_ctime = now;
        if (dprow.base_mtime < now)
            dprow.base_mtime = now;
        if (dprow.base_ctime < now)
            dprow.base_ctime = now;
        rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&dprow, v_par, sizeof(v_par));
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, new_parent, new_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, src_ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(new_parent), new_parent, k_par,
                              &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(dsh, new_parent, 0, k_dseq, &ks);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dent, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, ks, &sver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, ks, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        memset(&parts, 0, sizeof(parts));
        if (dsh == ish) {
            parts.n = 1;
            parts.shard[0] = dsh;
        } else {
            parts.n = 2;
            if (dsh < ish) {
                parts.shard[0] = dsh;
                parts.shard[1] = ish;
            } else {
                parts.shard[0] = ish;
                parts.shard[1] = dsh;
            }
        }
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == dsh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dent, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_par, kp,
                                   pver, EFS_TXN_PUT, v_par, sizeof(v_par),
                                   &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dseq, ks,
                                   sver, EFS_TXN_PUT, v_dseq, 8, &hint);
            }
            if (rc == EFS_OK && sh == ish) {
                rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                               iver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                               &hint);
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
        rc = host_read_inode_lanes(h, src_ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, src_ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = new_parent;
        strncpy(out->inode.name, new_name, EFS_MAX_NAME - 1);
    }
}

/* Same-dir LOCAL RENAME: src dentry DEL + dest dentry PUT + inode
 * parent/ctime as a txn (same PREPARE/DECIDE/RESOLVE as LINK). A
 * directory also GUARDs dst_parent ancestry pver sidecars and exclusive-
 * PUTs its own pver (cycle prevention). HASHED/SPLITTING and cross-dir
 * stay INVAL this slice. Replacing an existing dest is EXIST. Directory
 * inodes scatter, so a node that does not host both groups bounces. */
void server_raft_host_rename_at(efs_ino_t old_parent, const char *old_name,
                                efs_ino_t new_parent, const char *new_name,
                                struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent, ndent;
    struct efs_meta_stat st;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct host_pver_guard gv[EFS_TXN_MAX_PART];
    uint8_t k_src[EFS_KV_KEY_MAX], k_dst[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_par[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX], k_pver[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES];
    uint8_t v_par[EFS_META_INO_BYTES], v_dseq[8], v_pver[8], sb[8], cmd[22];
    uint32_t ks = 0, kd = 0, ki = 0, kp = 0, kq = 0, kpv = 0, sn = 8;
    uint32_t ssh, dsh, ish, psh, coord;
    uint64_t sver = 0, dver = 0, iver = 0, pver = 0, qver = 0, ever = 0;
    uint64_t seq = 0, now;
    int hint = -1;
    int rc, i, gr, is_dir = 0, ngv = 0;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || !old_name || !new_name || old_parent == 0 ||
        new_parent == 0 || old_name[0] == '\0' || new_name[0] == '\0') {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (old_parent != new_parent) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (strcmp(old_name, new_name) == 0) {
        out->status = EFS_INODE_RPC_OK;
        return;
    }
    now = now_ns();
    psh = efs_kv_inode_shard(old_parent);
    if (!host_hosts(h, efs_raft_shard_group(psh))) {
        uint8_t need[2];
        need[0] = EFS_RAFT_GROUP_SHARD;
        need[1] = EFS_RAFT_GROUP_SHARD2;
        host_fwd_rename(h, old_parent, old_name, new_parent, new_name, out,
                        need, 2);
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(psh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, old_parent, &prow);
    if (rc == EFS_OK && !S_ISDIR(prow.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && prow.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    if (rc == EFS_OK && prow.layout != EFS_META_LAYOUT_LOCAL)
        rc = EFS_ERR_INVAL;
    ssh = (rc == EFS_OK) ? efs_kv_dentry_shard(old_parent, old_name, prow.layout)
                         : 0;
    dsh = (rc == EFS_OK) ? efs_kv_dentry_shard(new_parent, new_name, prow.layout)
                         : 0;
    if (rc == EFS_OK && efs_raft_shard_group(ssh) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(ssh), &hint);
    if (rc == EFS_OK && efs_raft_shard_group(dsh) != efs_raft_shard_group(psh) &&
        efs_raft_shard_group(dsh) != efs_raft_shard_group(ssh))
        rc = host_read_index(h, efs_raft_shard_group(dsh), &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_lookup(h->kv, old_parent, old_name, &dent);
    if (rc == EFS_OK && (dent.type & S_IFMT) == S_IFDIR)
        is_dir = 1;
    if (rc == EFS_OK && is_dir && dent.ino == EFS_ROOT_INO)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        rc = efs_meta_apply_lookup(h->kv, new_parent, new_name, &ndent);
        if (rc == EFS_OK) {
            pthread_mutex_unlock(&h->read_mu);
            set_inode_rc(out, EFS_ERR_EXIST, hint);
            return;
        }
        if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
    }
    /* Bounce before resolve: a scattered dir inode lives on another
     * group, and resolve maps a missing row to I9 (EFS_ERR_IO). Dest
     * EXIST is parent-local and does not need the child row. */
    if (rc == EFS_OK && is_dir) {
        uint32_t csh = efs_kv_inode_shard(dent.ino);
        if (!host_hosts(h, efs_raft_shard_group(csh))) {
            uint8_t need[2];
            int nn = 1;
            need[0] = efs_raft_shard_group(psh);
            if (efs_raft_shard_group(csh) != need[0])
                need[nn++] = efs_raft_shard_group(csh);
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_rename(h, old_parent, old_name, new_parent, new_name, out,
                            need, nn);
            return;
        }
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_resolve(h->kv, old_parent, old_name, &dent, &row);
    if (rc == EFS_OK && S_ISDIR(row.mode))
        is_dir = 1;
    if (rc == EFS_OK && is_dir && row.ino == EFS_ROOT_INO)
        rc = EFS_ERR_INVAL;
    ish = (rc == EFS_OK) ? efs_kv_inode_shard(row.ino) : 0;
    if (rc == EFS_OK && efs_raft_shard_group(ish) != efs_raft_shard_group(psh))
        rc = host_read_index(h, efs_raft_shard_group(ish), &hint);
    memset(&parts, 0, sizeof(parts));
    if (rc == EFS_OK)
        rc = host_parts_add(&parts, ssh);
    if (rc == EFS_OK && dsh != ssh)
        rc = host_parts_add(&parts, dsh);
    if (rc == EFS_OK)
        rc = host_parts_add(&parts, ish);
    if (rc == EFS_OK && is_dir) {
        rc = host_pver_guard_chain(h, new_parent, row.ino, &parts, gv, &ngv,
                                   &hint);
        if (rc == EFS_OK)
            row.parent_version++;
    }
    if (rc == EFS_OK) {
        memset(&ndent, 0, sizeof(ndent));
        ndent.ino = row.ino;
        ndent.generation = row.generation;
        ndent.type = row.mode & S_IFMT;
        row.parent = new_parent;
        if (row.base_ctime < now)
            row.base_ctime = now;
        if (prow.base_mtime < now)
            prow.base_mtime = now;
        if (prow.base_ctime < now)
            prow.base_ctime = now;
        rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&prow, v_par, sizeof(v_par));
    }
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(ssh, old_parent, old_name, k_src, &ks);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(dsh, new_parent, new_name, k_dst, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, old_parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(ssh, old_parent, 0, k_dseq, &kq);
    if (rc == EFS_OK && is_dir) {
        rc = efs_kv_key_pver(ish, row.ino, k_pver, &kpv);
        if (rc == EFS_OK)
            rc = efs_txn_ver_get(h->kv, k_pver, kpv, &ever);
        if (rc == EFS_OK)
            wr64be(v_pver, row.parent_version);
    }
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_src, ks, &sver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dst, kd, &dver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_ino, ki, &iver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(h->kv, k_dseq, kq, &qver);
    if (rc == EFS_OK) {
        sn = 8;
        gr = efs_kv_get(h->kv, k_dseq, kq, sb, &sn);
        seq = (gr == EFS_OK && sn >= 8) ? rd64be(sb) : 0;
        wr64be(v_dseq, seq + 1);
        fill_txid(h, &t);
        for (i = 0; i < parts.n && rc == EFS_OK; i++) {
            uint32_t sh = parts.shard[i];
            if (sh == ssh) {
                rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_src, ks,
                               sver, EFS_TXN_DEL, NULL, 0, &hint);
                if (rc == EFS_OK && dsh == ssh)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_dst, kd,
                                   dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                                   &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_par, kp,
                                   pver, EFS_TXN_PUT, v_par, sizeof(v_par),
                                   &hint);
                if (rc == EFS_OK)
                    rc = host_prep(h, ssh, EFS_TXN_EXCL, &t, &parts, k_dseq, kq,
                                   qver, EFS_TXN_PUT, v_dseq, 8, &hint);
            }
            if (rc == EFS_OK && sh == dsh && dsh != ssh) {
                rc = host_prep(h, dsh, EFS_TXN_EXCL, &t, &parts, k_dst, kd,
                               dver, EFS_TXN_PUT, v_dent, sizeof(v_dent),
                               &hint);
            }
            if (rc == EFS_OK && sh == ish) {
                rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_ino, ki,
                               iver, EFS_TXN_PUT, v_ino, sizeof(v_ino),
                               &hint);
                if (rc == EFS_OK && is_dir)
                    rc = host_prep(h, ish, EFS_TXN_EXCL, &t, &parts, k_pver,
                                   kpv, ever, EFS_TXN_PUT, v_pver, 8, &hint);
            }
        }
        for (i = 0; i < ngv && rc == EFS_OK; i++)
            rc = host_prep(h, gv[i].shard, EFS_TXN_GUARD, &t, &parts, gv[i].key,
                           gv[i].klen, gv[i].ver, 0, NULL, 0, &hint);
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
        rc = host_read_inode_lanes(h, row.ino, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_getattr(h->kv, row.ino, host_txn_coord, h, &st);
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = new_parent;
        strncpy(out->inode.name, new_name, EFS_MAX_NAME - 1);
    }
}

/* OPEN reservations whose range is at or below the published size become
 * COMPLETED. No new opcode: REPORT already committed the data. Catch-up
 * with nopen==0 is a no-op (existing raft-smoke-p). read_mu held. */
static int host_resolve_caught_up(struct efs_raft_host *h, efs_ino_t ino,
                                  uint64_t sz, int *hint)
{
    uint64_t offs[64], lens[64];
    uint32_t n = 64, i, clen = 0;
    uint8_t cmd[HOST_APPEND_RES_LEN];
    uint8_t ig;
    int rc;

    rc = efs_meta_apply_append_open(h->kv, ino, offs, lens, &n);
    if (rc != EFS_OK)
        return rc;
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    for (i = 0; i < n; i++) {
        if (offs[i] + lens[i] > sz)
            continue;
        rc = pack_append_res_cmd(cmd, &clen, ino, offs[i],
                                 EFS_META_APPEND_COMPLETED);
        if (rc != EFS_OK)
            return rc;
        rc = host_propose_wait(h, ig, cmd, clen, hint);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* Files and symlinks carry a chunk map; FUSE stores a symlink target as
 * ordinary published bytes. Directories do not. */
static int host_holds_chunks(uint32_t mode)
{
    return S_ISREG(mode) || S_ISLNK(mode);
}

/* One chunk CAS + lane MAX. read_mu held. First-use of a lane whose
 * group is not the inode's is INVAL this slice (that is a 2-shard txn).
 * Lane 0 is the inode shard, so the smoke's first chunk is one group. */
static int host_pub_locked(struct efs_raft_host *h, const struct efs_chunk_rec *rec,
                           uint64_t new_size, int *hint)
{
    struct efs_meta_pub p;
    struct efs_meta_row row;
    struct efs_meta_chunk got;
    uint8_t cmd[HOST_PUBLISH_LEN];
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t clen = 0, lsh, ish;
    uint64_t idx = 0;
    uint8_t lane, lg, ig;
    int rc, i;

    if (!rec || rec->ino == 0)
        return EFS_ERR_INVAL;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (rec->nodes[i] == 0)
            return EFS_ERR_INVAL;
    }
    ish = efs_kv_inode_shard(rec->ino);
    ig = efs_raft_shard_group(ish);
    rc = host_read_index(h, ig, hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, rec->ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (!host_holds_chunks(row.mode))
        return EFS_ERR_INVAL;
    lane = (uint8_t)(rec->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(rec->ino, lane);
    lg = efs_raft_shard_group(lsh);
    if ((row.active_lanes & (1ULL << lane)) == 0 && lg != ig)
        return EFS_ERR_INVAL; /* first-use cross-group later */
    if (lg != ig)
        rc = host_read_index(h, lg, hint);
    if (rc != EFS_OK)
        return rc;
    memset(&got, 0, sizeof(got));
    rc = efs_meta_apply_get_chunk(h->kv, rec->ino, rec->chunk_index, &got);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    else if (rc != EFS_OK)
        return rc;
    memset(&p, 0, sizeof(p));
    p.ino = rec->ino;
    p.chunk_index = rec->chunk_index;
    p.new_size = new_size;
    p.now = now_ns();
    p.expected_gen = got.generation;
    memset(uuid, 0, sizeof(uuid));
    p.candidate_gen = efs_meta_candidate_gen(uuid, 0, 1, rec->chunk_index, 0);
    if (p.candidate_gen == 0)
        p.candidate_gen = 1;
    p.content_epoch = row.content_epoch;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    memcpy(p.ch.nodes, rec->nodes, sizeof(p.ch.nodes));
    memcpy(p.ch.checksums, rec->checksums, sizeof(p.ch.checksums));
    rc = pack_publish_cmd(cmd, &clen, &p);
    if (rc == EFS_OK)
        rc = host_propose(h, lg, cmd, clen, &idx, hint);
    if (rc == EFS_OK)
        rc = host_wait_applied(h, lg, idx, hint);
    return rc;
}

void server_raft_host_report(const struct efs_chunk_rec *recs, uint32_t count,
                             const struct efs_ino_size_rec *irecs,
                             uint32_t ino_count, struct efs_msg_inode_reply *out)
{
    struct efs_raft_host *h = g_host;
    uint32_t i, j;
    uint64_t sz;
    int hint = -1;
    int rc = EFS_OK;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (count == 0) {
        out->status = EFS_INODE_RPC_OK;
        return;
    }
    if (!recs) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    pthread_mutex_lock(&h->read_mu);
    for (i = 0; i < count && rc == EFS_OK; i++) {
        sz = 0;
        for (j = 0; j < ino_count && irecs; j++) {
            if (irecs[j].ino == recs[i].ino) {
                sz = irecs[j].size;
                break;
            }
        }
        if (sz == 0)
            sz = ((uint64_t)recs[i].chunk_index + 1) * EFS_MIN_CHUNK_SIZE;
        rc = host_pub_locked(h, &recs[i], sz, &hint);
        if (rc == EFS_OK)
            rc = host_resolve_caught_up(h, recs[i].ino, sz, &hint);
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc(out, rc, hint);
}

void server_raft_host_getchunks(efs_ino_t ino, uint32_t start, uint32_t max,
                                struct efs_msg_inode_getchunks_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_chunk ch;
    uint32_t ci, group_end, lsh, seen = 0;
    uint8_t ig, lg;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || ino == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (max == 0 || max > EFS_GETCHUNKS_MAX)
        max = EFS_GETCHUNKS_MAX;
    group_end = (start | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1u;
    ig = efs_raft_shard_group(efs_kv_inode_shard(ino));
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, ig, &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, ino, &row);
    if (rc == EFS_OK && !host_holds_chunks(row.mode))
        rc = EFS_ERR_INVAL;
    seen = (rc == EFS_OK) ? (1u << ig) : 0;
    for (ci = start; rc == EFS_OK && ci < group_end && out->count < max; ci++) {
        uint8_t lane = (uint8_t)(ci % EFS_META_LANES);

        /* A chunk cannot exist on a lane the inode never registered.
         * ReadIndexing unused lanes would send this RPC to groups this
         * node does not host (fresh smoke: group-0 leader, lane 1+). */
        if ((row.active_lanes & (1ULL << lane)) == 0)
            continue;
        lsh = efs_kv_lane_shard(ino, lane);
        lg = efs_raft_shard_group(lsh);
        if ((seen & (1u << lg)) == 0) {
            int hh = -1;
            rc = host_read_index(h, lg, &hh);
            if (rc != EFS_OK)
                hint = hh;
            else
                seen |= 1u << lg;
        }
        if (rc != EFS_OK)
            break;
        rc = efs_meta_apply_get_chunk(h->kv, ino, ci, &ch);
        if (rc == EFS_ERR_NOT_FOUND) {
            rc = EFS_OK;
            continue;
        }
        if (rc != EFS_OK)
            break;
        out->recs[out->count].ino = ino;
        out->recs[out->count].chunk_index = ci;
        memcpy(out->recs[out->count].nodes, ch.nodes,
               sizeof(out->recs[out->count].nodes));
        memcpy(out->recs[out->count].checksums, ch.checksums,
               sizeof(out->recs[out->count].checksums));
        out->count++;
    }
    pthread_mutex_unlock(&h->read_mu);
    out->status = rc_to_inode_status(rc);
    out->primary_id = (hint >= 0) ? (efs_node_id_t)(hint + 1) : 0;
}

/* READDIR: ReadIndex the dir inode (and used dir-lane groups if HASHED),
 * then scan. after_ino skips already-returned inos so the old wire cursor
 * still works. SPLITTING is BUSY. */
void server_raft_host_readdir(efs_ino_t parent, uint32_t max_ents,
                              uint64_t after_ino,
                              struct efs_msg_inode_readdir_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent page[EFS_READDIR_MAX];
    struct efs_meta_stat st;
    uint32_t got = 0, i;
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    if (!h || !h->running || parent == 0) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    if (max_ents == 0 || max_ents > EFS_READDIR_MAX)
        max_ents = EFS_READDIR_MAX;
    {
        uint8_t pg = efs_raft_shard_group(efs_kv_inode_shard(parent));
        if (!host_hosts(h, pg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            if (pg != EFS_RAFT_GROUP_SHARD2)
                need[nn++] = EFS_RAFT_GROUP_SHARD2;
            else
                need[nn++] = EFS_RAFT_GROUP_SHARD;
            host_fwd_readdir(h, parent, max_ents, after_ino, out, need, nn);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_index(h, efs_raft_shard_group(efs_kv_inode_shard(parent)),
                         &hint);
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, parent, &row);
    if (rc == EFS_OK && !S_ISDIR(row.mode))
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && row.layout == EFS_META_LAYOUT_SPLITTING)
        rc = EFS_ERR_BUSY;
    if (rc == EFS_OK && row.layout != EFS_META_LAYOUT_LOCAL) {
        uint32_t li;
        for (li = 0; li < EFS_META_LANES; li++) {
            uint8_t lg;
            if ((row.used_shards & (1ull << li)) == 0)
                continue;
            lg = efs_raft_shard_group(efs_kv_lane_shard(parent, (uint8_t)li));
            if (!host_hosts(h, lg)) {
                uint8_t need[2];
                int nn = 1;
                need[0] = efs_raft_shard_group(efs_kv_inode_shard(parent));
                if (lg != need[0])
                    need[nn++] = lg;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_readdir(h, parent, max_ents, after_ino, out, need, nn);
                return;
            }
        }
    }
    if (rc == EFS_OK)
        rc = host_read_inode_lanes(h, parent, &hint);
    memset(&cur, 0, sizeof(cur));
    while (rc == EFS_OK && !cur.done && out->count < max_ents) {
        got = 0;
        rc = efs_meta_apply_readdir(h->kv, parent, &cur, page, EFS_READDIR_MAX,
                                    &got);
        for (i = 0; i < got && rc == EFS_OK && out->count < max_ents; i++) {
            uint8_t cg;

            if (page[i].d.ino <= after_ino)
                continue;
            /* HASHED child's used_shards may be unhosted; stub like an
             * unhosted inode group instead of failing the whole listing. */
            cg = efs_raft_shard_group(efs_kv_inode_shard(page[i].d.ino));
            if (!host_hosts(h, cg)) {
            stub_ent:
                memset(&out->ents[out->count], 0, sizeof(out->ents[0]));
                out->ents[out->count].ino = page[i].d.ino;
                out->ents[out->count].mode = page[i].d.type;
                out->ents[out->count].parent = parent;
                strncpy(out->ents[out->count].name, page[i].name,
                        EFS_MAX_NAME - 1);
                out->count++;
                rc = EFS_OK;
                continue;
            }
            rc = host_read_inode_lanes(h, page[i].d.ino, &hint);
            if (rc == EFS_ERR_NOT_PRIMARY)
                goto stub_ent;
            if (rc != EFS_OK)
                break;
            rc = efs_meta_apply_getattr(h->kv, page[i].d.ino, host_txn_coord, h,
                                        &st);
            if (rc == EFS_ERR_NOT_PRIMARY)
                goto stub_ent;
            if (rc != EFS_OK)
                break;
            stat_to_inode(&st, &out->ents[out->count]);
            out->ents[out->count].parent = parent;
            strncpy(out->ents[out->count].name, page[i].name, EFS_MAX_NAME - 1);
            out->count++;
        }
    }
    pthread_mutex_unlock(&h->read_mu);
    if (rc == EFS_OK)
        out->status = EFS_INODE_RPC_OK;
    else {
        out->count = 0;
        out->status = rc_to_inode_status(rc);
    }
}

/* LOOKUP_PATH: hop-by-hop ReadIndex + lookup, same as the in-sim walk.
 * Empty path is the start inode. Intermediate not-a-directory is INVAL.
 * A hop whose dentry or child inode lives on a group this node does not
 * host bounces, same as LOOKUP (scattered MKDIR dests). Intermediate
 * hops only need the inode row (layout/mode); do not ReadIndex a HASHED
 * ancestor's used_shards or a group-0-only replica fails the walk
 * before the hashed-dentry bounce. Leaf getattr bounces if lanes are
 * unhosted. */
void server_raft_host_lookup_path(efs_ino_t start, const char *path,
                                  struct efs_msg_inode_lookup_path_reply *out)
{
    struct efs_raft_host *h = g_host;
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct efs_meta_stat st;
    efs_ino_t cur, last_parent = 0;
    const char *p;
    uint32_t nh = 0;
    char last_name[EFS_MAX_NAME];
    int hint = -1;
    int rc;

    memset(out, 0, sizeof(*out));
    out->status = EFS_INODE_RPC_ERROR;
    memset(last_name, 0, sizeof(last_name));
    if (!h || !h->running) {
        out->status = EFS_INODE_RPC_INVAL;
        return;
    }
    cur = start ? start : EFS_ROOT_INO;
    p = path ? path : "";
    while (*p == '/')
        p++;
    {
        uint8_t sg = efs_raft_shard_group(efs_kv_inode_shard(cur));
        if (!host_hosts(h, sg)) {
            uint8_t need[2];
            need[0] = EFS_RAFT_GROUP_SHARD;
            need[1] = EFS_RAFT_GROUP_SHARD2;
            host_fwd_lookup_path(h, start, path, out, need, 2);
            return;
        }
    }
    pthread_mutex_lock(&h->read_mu);
    rc = host_read_inode_lanes(h, cur, &hint);
    if (rc == EFS_ERR_NOT_PRIMARY) {
        uint8_t need[2];

        pthread_mutex_unlock(&h->read_mu);
        host_need_both(need);
        host_fwd_lookup_path(h, start, path, out, need, 2);
        return;
    }
    if (rc == EFS_OK)
        rc = efs_meta_apply_get_inode(h->kv, cur, &row);
    if (rc == EFS_OK && *p == '\0') {
        rc = efs_meta_apply_getattr(h->kv, cur, host_txn_coord, h, &st);
        pthread_mutex_unlock(&h->read_mu);
        set_inode_rc((struct efs_msg_inode_reply *)out, rc, hint);
        if (rc == EFS_OK)
            stat_to_inode(&st, &out->inode);
        return;
    }
    while (rc == EFS_OK && *p && nh < EFS_LOOKUP_PATH_MAX_DEPTH) {
        char name[EFS_MAX_NAME];
        size_t nlen;
        const char *s = p;
        uint32_t dsh;
        uint8_t dg, pg;

        while (*p && *p != '/')
            p++;
        nlen = (size_t)(p - s);
        while (*p == '/')
            p++;
        if (nlen == 0)
            break;
        if (nlen >= EFS_MAX_NAME) {
            rc = EFS_ERR_NAMETOOLONG;
            break;
        }
        memset(name, 0, sizeof(name));
        memcpy(name, s, nlen);
        if (!S_ISDIR(row.mode)) {
            rc = EFS_ERR_INVAL;
            break;
        }
        pg = efs_raft_shard_group(efs_kv_inode_shard(cur));
        dsh = efs_kv_dentry_shard(cur, name, row.layout);
        dg = efs_raft_shard_group(dsh);
        if (dg != pg && !host_hosts(h, dg)) {
            uint8_t need[2];
            int nn = 1;
            need[0] = pg;
            need[nn++] = dg;
            pthread_mutex_unlock(&h->read_mu);
            host_fwd_lookup_path(h, start, path, out, need, nn);
            return;
        }
        if (dg != pg)
            rc = host_read_index(h, dg, &hint);
        if (rc == EFS_OK)
            rc = efs_meta_apply_lookup(h->kv, cur, name, &dent);
        if (rc == EFS_OK) {
            uint8_t cg = efs_raft_shard_group(efs_kv_inode_shard(dent.ino));
            if (!host_hosts(h, cg)) {
                uint8_t need[2];
                int nn = 1;
                need[0] = pg;
                if (cg != pg)
                    need[nn++] = cg;
                pthread_mutex_unlock(&h->read_mu);
                host_fwd_lookup_path(h, start, path, out, need, nn);
                return;
            }
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_get_inode(h->kv, dent.ino, &row);
        if (rc == EFS_OK) {
            last_parent = cur;
            memcpy(last_name, name, EFS_MAX_NAME);
            out->ancestors[nh].ino = row.ino;
            out->ancestors[nh].mode = row.mode;
            out->ancestors[nh].uid = row.uid;
            out->ancestors[nh].gid = row.gid;
            cur = row.ino;
            nh++;
        }
    }
    if (rc == EFS_OK && *p)
        rc = EFS_ERR_INVAL;
    if (rc == EFS_OK && nh > 0) {
        out->ancestor_count = nh > 1 ? nh - 1 : 0;
        rc = host_read_inode_lanes(h, cur, &hint);
        if (rc == EFS_ERR_NOT_PRIMARY) {
            uint8_t need[2];

            pthread_mutex_unlock(&h->read_mu);
            host_need_both(need);
            host_fwd_lookup_path(h, start, path, out, need, 2);
            return;
        }
        if (rc == EFS_OK)
            rc = efs_meta_apply_getattr(h->kv, cur, host_txn_coord, h, &st);
    }
    pthread_mutex_unlock(&h->read_mu);
    set_inode_rc((struct efs_msg_inode_reply *)out, rc, hint);
    if (rc == EFS_OK) {
        stat_to_inode(&st, &out->inode);
        out->inode.parent = last_parent;
        strncpy(out->inode.name, last_name, EFS_MAX_NAME - 1);
    }
}

