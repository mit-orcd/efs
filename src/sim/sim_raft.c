/* Single-shard Raft group inside efs_sim (architecture.md §10 step 5).
 * RF=3. Apply is meta_apply. Reads are leader + ReadIndex. */
#include "sim_internal.h"
#include "efs/opid.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CMD_CREATE  1
#define CMD_UNLINK  2
#define CMD_PUBLISH 3
#define CMD_EPOCH   SIM_CMD_EPOCH
#define CMD_SETATTR SIM_CMD_SETATTR
#define CMD_UTIMENS SIM_CMD_UTIMENS
#define CMD_TRUNCATE SIM_CMD_TRUNCATE
#define CMD_APPEND_RSV SIM_CMD_APPEND_RSV
#define CMD_APPEND_RES SIM_CMD_APPEND_RES
#define CMD_MKFS    SIM_CMD_MKFS
#define CMD_MAX     512
#define TRUNC_HDR   54
#define TRUNC_TAIL  (4 + 8 + 8 + 4 + (uint32_t)EFS_NUM_FRAGMENTS * (4 + EFS_HASH_SIZE))
#define WAIT_TICKS  80
#define RAFT_HDR    86 /* fixed part through nentries; entries follow (12+cmd each) */

static void wr64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int reachable(const struct efs_sim *sim, int i)
{
    if (!sim || i < 0 || i >= sim->nraft)
        return 0;
    return sim->srv[i].alive && !sim->srv[i].partitioned &&
           sim->srv[i].raft != NULL;
}

struct efs_raft *sim_raft_of(struct efs_sim *sim, int server, uint8_t group)
{
    if (!sim || server < 0 || server >= sim->nservers)
        return NULL;
    if (group == EFS_RAFT_GROUP_CTRL)
        return sim->srv[server].ctrl;
    if (group == EFS_RAFT_GROUP_SHARD2)
        return sim->srv[server].raft2;
    return sim->srv[server].raft;
}

static int reachable_g(const struct efs_sim *sim, int i, uint8_t group)
{
    struct efs_raft *r;

    if (!sim || i < 0 || i >= sim->nservers)
        return 0;
    if (!sim->srv[i].alive || sim->srv[i].partitioned)
        return 0;
    r = sim_raft_of((struct efs_sim *)sim, i, group);
    return r != NULL;
}

static int leader_id_g(const struct efs_sim *sim, uint8_t group)
{
    int i, lid = -1, n = 0, lim;
    struct efs_raft *r;

    lim = sim->nraft;
    if (group == EFS_RAFT_GROUP_CTRL)
        lim = EFS_SIM_RAFT_N;
    for (i = 0; i < lim; i++) {
        if (!reachable_g(sim, i, group))
            continue;
        r = sim_raft_of((struct efs_sim *)sim, i, group);
        if (r && efs_raft_role(r) == EFS_RAFT_LEADER) {
            lid = i;
            n++;
        }
    }
    return n == 1 ? lid : -1;
}

static int leader_id(const struct efs_sim *sim)
{
    return leader_id_g(sim, EFS_RAFT_GROUP_SHARD);
}

/* The proposing leader stamps uid/gid/now INTO the command, because apply
 * has to be a pure function of it: a replica that read its own clock would
 * derive a different inode row from the same committed entry. */
#define CREATE_NAME_OFF 31

static int pack_create(uint8_t *out, uint32_t *len, int has_op,
                       const struct efs_opid *op, const uint8_t *uuid,
                       uint32_t epoch, efs_ino_t parent, uint32_t mode,
                       const char *name, const struct efs_meta_attrs *at)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = CREATE_NAME_OFF + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (has_op)
        n += 8;
    if (n > CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = CMD_CREATE;
    out[1] = has_op ? 1 : 0;
    wr64(out + 2, parent);
    wr32(out + 10, mode);
    wr32(out + 14, at->uid);
    wr32(out + 18, at->gid);
    wr64(out + 22, at->now);
    out[30] = (uint8_t)nl;
    memcpy(out + CREATE_NAME_OFF, name, nl);
    p = out + CREATE_NAME_OFF + nl;
    memcpy(p, uuid, EFS_OPID_UUID_LEN);
    wr32(p + EFS_OPID_UUID_LEN, epoch);
    if (has_op)
        wr64(p + EFS_OPID_UUID_LEN + 4, op->seq);
    *len = n;
    return EFS_OK;
}

static int pack_unlink(uint8_t *out, uint32_t *len, const uint8_t *uuid,
                       uint32_t epoch, efs_ino_t parent, uint64_t now,
                       const char *name)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = 1 + 8 + 8 + 1 + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (n > CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = CMD_UNLINK;
    wr64(out + 1, parent);
    wr64(out + 9, now);
    out[17] = (uint8_t)nl;
    memcpy(out + 18, name, nl);
    p = out + 18 + nl;
    memcpy(p, uuid, EFS_OPID_UUID_LEN);
    wr32(p + EFS_OPID_UUID_LEN, epoch);
    *len = n;
    return EFS_OK;
}

static int pack_publish(uint8_t *out, uint32_t *len, const uint8_t *uuid,
                        uint32_t epoch, const struct efs_meta_pub *p)
{
    uint32_t n = 1 + 8 + 4 + 8 + 8 +
                 (uint32_t)EFS_NUM_FRAGMENTS * (4 + EFS_HASH_SIZE) +
                 EFS_OPID_UUID_LEN + 4 + 8 + 8 + 8 + 4;
    int i;
    uint8_t *q;

    if (!p || n > CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = CMD_PUBLISH;
    wr64(out + 1, p->ino);
    wr32(out + 9, p->chunk_index);
    wr64(out + 13, p->new_size);
    wr64(out + 21, p->now);
    q = out + 29;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        wr32(q, p->ch.nodes[i]);
        q += 4;
        memcpy(q, p->ch.checksums[i], EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    memcpy(q, uuid, EFS_OPID_UUID_LEN);
    wr32(q + EFS_OPID_UUID_LEN, epoch);
    q += EFS_OPID_UUID_LEN + 4;
    wr64(q, p->candidate_gen);
    wr64(q + 8, p->expected_gen);
    wr64(q + 16, p->content_epoch);
    wr32(q + 24, p->coding_profile_id);
    *len = n;
    return EFS_OK;
}

static int apply_create_cmd(struct sim_server *s, const uint8_t *cmd,
                            uint32_t clen, uint64_t index, uint8_t group)
{
    efs_ino_t parent, ino = 0;
    uint32_t mode, epoch, dsh;
    char name[EFS_MAX_NAME];
    uint8_t nl, has_op, uuid[EFS_OPID_UUID_LEN];
    int rc;
    struct efs_opid op;
    struct efs_meta_attrs at;
    struct efs_meta_dentry dent;
    struct efs_meta_row prow;
    const uint8_t *p;

    if (clen < CREATE_NAME_OFF)
        return EFS_ERR_PROTO;
    has_op = cmd[1];
    parent = rd64(cmd + 2);
    mode = rd32(cmd + 10);
    memset(&at, 0, sizeof(at));
    at.uid = rd32(cmd + 14);
    at.gid = rd32(cmd + 18);
    at.now = rd64(cmd + 22);
    nl = cmd[30];
    if ((uint32_t)CREATE_NAME_OFF + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_ERR_PROTO;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + CREATE_NAME_OFF, nl);
    p = cmd + CREATE_NAME_OFF + nl;
    memcpy(uuid, p, EFS_OPID_UUID_LEN);
    epoch = rd32(p + EFS_OPID_UUID_LEN);
    memset(&op, 0, sizeof(op));
    dsh = efs_kv_inode_shard(parent);
    if (efs_meta_apply_get_inode(s->disk, parent, &prow) == EFS_OK)
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    rc = efs_session_accept(s->disk, dsh, uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, 0);
        return EFS_OK;
    }
    if (has_op) {
        if ((uint32_t)(p - cmd) + EFS_OPID_UUID_LEN + 4 + 8 > clen)
            return EFS_ERR_PROTO;
        memcpy(op.client_uuid, uuid, EFS_OPID_UUID_LEN);
        op.session_epoch = epoch;
        op.seq = rd64(p + EFS_OPID_UUID_LEN + 4);
        rc = efs_meta_apply_create_file_op(s->disk, &op, &at, parent, mode,
                                           name, &ino);
    } else {
        rc = efs_meta_apply_create_file_log(s->disk, &at, parent, mode, name, &ino);
    }
    if (rc == EFS_ERR_EXIST &&
        efs_meta_apply_lookup(s->disk, parent, name, &dent) == EFS_OK)
        ino = dent.ino;
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

static int apply_unlink_cmd(struct sim_server *s, const uint8_t *cmd,
                            uint32_t clen, uint64_t index, uint8_t group)
{
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t nl, uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch, dsh;
    uint64_t now;
    const uint8_t *p;
    struct efs_meta_row prow;
    int rc;

    if (clen < 18)
        return EFS_ERR_PROTO;
    parent = rd64(cmd + 1);
    now = rd64(cmd + 9);
    nl = cmd[17];
    if ((uint32_t)18 + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_ERR_PROTO;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + 18, nl);
    p = cmd + 18 + nl;
    memcpy(uuid, p, EFS_OPID_UUID_LEN);
    epoch = rd32(p + EFS_OPID_UUID_LEN);
    dsh = efs_kv_inode_shard(parent);
    if (efs_meta_apply_get_inode(s->disk, parent, &prow) == EFS_OK)
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    rc = efs_session_accept(s->disk, dsh, uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, 0);
        return EFS_OK;
    }
    rc = efs_meta_apply_unlink(s->disk, parent, name, now);
    sim_note_apply(s, group, index, rc, 0);
    return EFS_OK;
}

static int apply_publish_cmd(struct sim_server *s, const uint8_t *cmd,
                             uint32_t clen, uint64_t index, uint8_t group)
{
    struct efs_meta_pub p;
    uint32_t need, epoch;
    const uint8_t *q;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint8_t lane;
    int i, rc;

    need = 29 + (uint32_t)EFS_NUM_FRAGMENTS * (4 + EFS_HASH_SIZE) +
           EFS_OPID_UUID_LEN + 4 + 8 + 8 + 8 + 4;
    if (clen < need)
        return EFS_ERR_PROTO;
    memset(&p, 0, sizeof(p));
    p.ino = rd64(cmd + 1);
    p.chunk_index = rd32(cmd + 9);
    p.new_size = rd64(cmd + 13);
    p.now = rd64(cmd + 21);
    q = cmd + 29;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = rd32(q);
        q += 4;
        memcpy(p.ch.checksums[i], q, EFS_HASH_SIZE);
        q += EFS_HASH_SIZE;
    }
    memcpy(uuid, q, EFS_OPID_UUID_LEN);
    epoch = rd32(q + EFS_OPID_UUID_LEN);
    q += EFS_OPID_UUID_LEN + 4;
    p.candidate_gen = rd64(q);
    p.expected_gen = rd64(q + 8);
    p.content_epoch = rd64(q + 16);
    p.coding_profile_id = rd32(q + 24);
    lane = (uint8_t)(p.chunk_index % EFS_META_LANES);
    rc = efs_session_accept(s->disk, efs_kv_lane_shard(p.ino, lane), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, p.ino);
        return EFS_OK;
    }
    rc = efs_meta_apply_publish(s->disk, &p);
    sim_note_apply(s, group, index, rc, p.ino);
    return EFS_OK;
}

static int apply_mkfs_cmd(struct sim_server *s, const uint8_t *cmd,
                          uint32_t clen, uint64_t index, uint8_t group)
{
    uint64_t now, salt = 0;
    int rc;

    if (clen < 9)
        return EFS_ERR_PROTO;
    now = rd64(cmd + 1);
    if (clen >= 17)
        salt = rd64(cmd + 9);
    rc = efs_meta_apply_mkfs(s->disk, now, salt);
    sim_note_apply(s, group, index, rc, EFS_ROOT_INO);
    return EFS_OK;
}

/* EFS_MD_CMD_SALT parity with the production host. The sim's shared disk
 * already makes the mkfs salt visible everywhere (the documented blind
 * spot), so this only keeps the command namespace behavior identical. */
static int apply_salt_record_cmd(struct sim_server *s, const uint8_t *cmd,
                                 uint32_t clen, uint64_t index, uint8_t group)
{
    int rc;

    if (clen < 13)
        return EFS_ERR_PROTO;
    rc = efs_meta_apply_salt_record(s->disk, rd32(cmd + 1), rd64(cmd + 5));
    sim_note_apply(s, group, index, rc, EFS_ROOT_INO);
    return EFS_OK;
}

static int apply_epoch_cmd(struct sim_server *s, const uint8_t *cmd,
                           uint32_t clen, uint64_t index, uint8_t group)
{
    efs_ino_t ino;
    int rc;

    if (clen < 9)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    rc = efs_meta_apply_epoch_fence(s->disk, ino);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

static int apply_setattr_cmd(struct sim_server *s, const uint8_t *cmd,
                             uint32_t clen, uint64_t index, uint8_t group)
{
    struct efs_meta_setattr sa;
    efs_ino_t ino;
    uint64_t now;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    int rc;

    if (clen < 61)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    now = rd64(cmd + 9);
    memset(&sa, 0, sizeof(sa));
    sa.expect_gen = rd64(cmd + 17);
    sa.mask = rd32(cmd + 25);
    sa.mode = rd32(cmd + 29);
    sa.uid = rd32(cmd + 33);
    sa.gid = rd32(cmd + 37);
    memcpy(uuid, cmd + 41, EFS_OPID_UUID_LEN);
    epoch = rd32(cmd + 41 + EFS_OPID_UUID_LEN);
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(ino), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, ino);
        return EFS_OK;
    }
    rc = efs_meta_apply_setattr(s->disk, ino, now, &sa);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

static int apply_utimens_cmd(struct sim_server *s, const uint8_t *cmd,
                             uint32_t clen, uint64_t index, uint8_t group)
{
    struct efs_meta_utimens u;
    efs_ino_t ino;
    uint64_t now;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    int rc;

    if (clen < 73)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    now = rd64(cmd + 9);
    memset(&u, 0, sizeof(u));
    u.expect_gen = rd64(cmd + 17);
    u.mask = rd32(cmd + 25);
    u.mtime = rd64(cmd + 29);
    u.atime = rd64(cmd + 37);
    u.mtime_gen = rd64(cmd + 45);
    memcpy(uuid, cmd + 53, EFS_OPID_UUID_LEN);
    epoch = rd32(cmd + 53 + EFS_OPID_UUID_LEN);
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(ino), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, ino);
        return EFS_OK;
    }
    rc = efs_meta_apply_utimens(s->disk, ino, now, &u);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

static int apply_truncate_cmd(struct sim_server *s, const uint8_t *cmd,
                              uint32_t clen, uint64_t index, uint8_t group)
{
    struct efs_meta_truncate t;
    struct efs_meta_pub tail;
    efs_ino_t ino;
    uint64_t now, expect_gen, size;
    uint8_t has_tail, uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    const uint8_t *q;
    int i, rc;

    if (clen < TRUNC_HDR)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    now = rd64(cmd + 9);
    expect_gen = rd64(cmd + 17);
    size = rd64(cmd + 25);
    has_tail = cmd[33];
    memcpy(uuid, cmd + 34, EFS_OPID_UUID_LEN);
    epoch = rd32(cmd + 34 + EFS_OPID_UUID_LEN);
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(ino), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, ino);
        return EFS_OK;
    }
    memset(&t, 0, sizeof(t));
    t.expect_gen = expect_gen;
    t.size = size;
    /* The sim's servers all vote in all groups over one shared disk, so a
     * truncate entry fences every lane locally (no cross-group split is
     * modelled — see sim_raft_truncate). */
    t.lane_mask = ~0ULL;
    if (has_tail) {
        if (clen < TRUNC_HDR + TRUNC_TAIL)
            return EFS_ERR_PROTO;
        q = cmd + TRUNC_HDR;
        memset(&tail, 0, sizeof(tail));
        tail.ino = ino;
        tail.chunk_index = rd32(q);
        tail.new_size = size;
        tail.candidate_gen = rd64(q + 4);
        tail.expected_gen = rd64(q + 12);
        tail.coding_profile_id = rd32(q + 20);
        q += 24;
        for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            tail.ch.nodes[i] = rd32(q);
            q += 4;
            memcpy(tail.ch.checksums[i], q, EFS_HASH_SIZE);
            q += EFS_HASH_SIZE;
        }
        tail.now = now;
        t.tail = &tail;
    }
    rc = efs_meta_apply_truncate(s->disk, ino, now, &t);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

static int apply_append_rsv_cmd(struct sim_server *s, const uint8_t *cmd,
                                uint32_t clen, uint64_t index, uint8_t group)
{
    struct efs_opid op;
    efs_ino_t ino;
    uint64_t len, off = 0;
    int rc;

    if (clen < 45)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    len = rd64(cmd + 9);
    memset(&op, 0, sizeof(op));
    memcpy(op.client_uuid, cmd + 17, EFS_OPID_UUID_LEN);
    op.session_epoch = rd32(cmd + 17 + EFS_OPID_UUID_LEN);
    op.seq = rd64(cmd + 37);
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(ino), op.client_uuid,
                            op.session_epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, ino);
        return EFS_OK;
    }
    rc = efs_meta_apply_append_reserve(s->disk, ino, len, &op, sim_txn_coord,
                                       s->sim, &off);
    sim_note_apply(s, group, index, rc, ino);
    if (rc == EFS_OK) {
        s->applied_extra = off;
        if (group <= 2)
            s->applied_extra_g[group] = off;
    }
    return EFS_OK;
}

static int apply_append_res_cmd(struct sim_server *s, const uint8_t *cmd,
                                uint32_t clen, uint64_t index, uint8_t group)
{
    efs_ino_t ino;
    uint64_t off;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    int outcome, rc;

    if (clen < 38)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    off = rd64(cmd + 9);
    outcome = (int)cmd[17];
    memcpy(uuid, cmd + 18, EFS_OPID_UUID_LEN);
    epoch = rd32(cmd + 18 + EFS_OPID_UUID_LEN);
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(ino), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, group, index, rc, ino);
        return EFS_OK;
    }
    rc = efs_meta_apply_append_resolve(s->disk, ino, off, outcome);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

/* Host-generated lane maintenance commands (EFS_MD_CMD_ACTIVATE_LANE /
 * EFS_MD_CMD_LANE_FENCE). The sim's coordinator never emits these — its
 * servers all vote in all groups over one shared disk, so the production
 * cross-group split never occurs — but the namespace is shared with the
 * production host, so the applies are kept here to keep them convergent. */
static int apply_activate_lane_cmd(struct sim_server *s, const uint8_t *cmd,
                                   uint32_t clen, uint64_t index,
                                   uint8_t group)
{
    efs_ino_t ino;
    int rc;

    if (clen < 10)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    if (clen >= 17)
        rc = efs_meta_apply_activate_lanes(s->disk, ino, rd64(cmd + 9));
    else
        rc = efs_meta_apply_activate_lane(s->disk, ino, cmd[9]);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

static int apply_lane_fence_cmd(struct sim_server *s, const uint8_t *cmd,
                                uint32_t clen, uint64_t index, uint8_t group)
{
    efs_ino_t ino;
    uint64_t gen, epoch, size;
    uint32_t tail_ci;
    int rc;

    if (clen < 39)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    gen = rd64(cmd + 9);
    epoch = rd64(cmd + 18);
    size = rd64(cmd + 26);
    tail_ci = rd32(cmd + 34);
    rc = efs_meta_apply_lane_fence(s->disk, ino, gen, cmd[17], epoch, size,
                                   tail_ci, cmd[38]);
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

int sim_ns_try(struct sim_server *s, uint8_t group, const uint8_t *cmd,
               uint32_t clen, uint64_t index)
{
    if (!cmd || clen == 0)
        return 0;
    switch (cmd[0]) {
    case CMD_MKFS:
        apply_mkfs_cmd(s, cmd, clen, index, group);
        return 1;
    case EFS_MD_CMD_SALT:
        apply_salt_record_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_CREATE:
        apply_create_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_UNLINK:
        apply_unlink_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_PUBLISH:
        apply_publish_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_EPOCH:
        apply_epoch_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_SETATTR:
        apply_setattr_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_UTIMENS:
        apply_utimens_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_TRUNCATE:
        apply_truncate_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_APPEND_RSV:
        apply_append_rsv_cmd(s, cmd, clen, index, group);
        return 1;
    case CMD_APPEND_RES:
        apply_append_res_cmd(s, cmd, clen, index, group);
        return 1;
    case SIM_CMD_DIR:
        sim_dir_apply(s, group, cmd, clen, index);
        return 1;
    case SIM_CMD_LOCK:
        sim_lock_apply(s, group, cmd, clen, index);
        return 1;
    case EFS_MD_CMD_ACTIVATE_LANE:
        apply_activate_lane_cmd(s, cmd, clen, index, group);
        return 1;
    case EFS_MD_CMD_LANE_FENCE:
        apply_lane_fence_cmd(s, cmd, clen, index, group);
        return 1;
    default:
        return 0;
    }
}

static int raft_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct sim_server *s = app;

    (void)term;
    if (!s || !s->disk || !cmd || clen == 0)
        return EFS_OK;
    if (sim_ns_try(s, EFS_RAFT_GROUP_SHARD, cmd, clen, index))
        return EFS_OK;
    if (cmd[0] == SIM_CMD_SESSION)
        return sim_sess_apply(s, EFS_RAFT_GROUP_SHARD, cmd, clen, index);
    return sim_txn_apply(s, EFS_RAFT_GROUP_SHARD, cmd, clen, index);
}

static int unpack_raft_ev(const uint8_t *p, uint32_t n, struct efs_raft_msg *m,
                          uint8_t **cmd_out)
{
    if (n < RAFT_HDR)
        return EFS_ERR_PROTO;
    memset(m, 0, sizeof(*m));
    m->type = p[0];
    m->group = p[1];
    m->boot_id = rd64(p + 2);
    m->from = (int)rd32(p + 10);
    m->to = (int)rd32(p + 14);
    m->term = rd64(p + 18);
    m->last_log_index = rd64(p + 26);
    m->last_log_term = rd64(p + 34);
    m->vote_granted = (int)rd32(p + 42);
    m->prev_index = rd64(p + 46);
    m->prev_term = rd64(p + 54);
    m->leader_commit = rd64(p + 62);
    m->match_index = rd64(p + 70);
    m->success = (int)rd32(p + 78);
    m->nentries = rd32(p + 82);
    if (m->nentries > EFS_RAFT_AE_MAX)
        return EFS_ERR_PROTO;
    {
        const uint8_t *q = p + RAFT_HDR;
        uint32_t i;
        for (i = 0; i < m->nentries; i++) {
            uint32_t ec;
            if ((uint32_t)(q - p) + 12u > n)
                return EFS_ERR_PROTO;
            m->entries[i].term = rd64(q); q += 8;
            ec = rd32(q); q += 4;
            m->entries[i].clen = ec;
            if ((uint32_t)(q - p) + ec > n)
                return EFS_ERR_PROTO;
            /* Entry cmd points into the event payload (freed by the
             * caller after deliver); nothing separately owned. */
            m->entries[i].cmd = ec ? q : NULL;
            q += ec;
        }
    }
    *cmd_out = NULL;
    return EFS_OK;
}

static int pack_raft_ev(const struct efs_raft_msg *msg, uint8_t **out,
                        uint32_t *plen)
{
    uint32_t n = RAFT_HDR;
    uint32_t i;
    uint8_t *p, *q;

    for (i = 0; i < msg->nentries; i++)
        n += 12u + msg->entries[i].clen;
    p = malloc(n);
    if (!p)
        return EFS_ERR_NOMEM;
    memset(p, 0, n);
    p[0] = msg->type;
    p[1] = msg->group;
    wr64(p + 2, msg->boot_id);
    wr32(p + 10, (uint32_t)msg->from);
    wr32(p + 14, (uint32_t)msg->to);
    wr64(p + 18, msg->term);
    wr64(p + 26, msg->last_log_index);
    wr64(p + 34, msg->last_log_term);
    wr32(p + 42, (uint32_t)msg->vote_granted);
    wr64(p + 46, msg->prev_index);
    wr64(p + 54, msg->prev_term);
    wr64(p + 62, msg->leader_commit);
    wr64(p + 70, msg->match_index);
    wr32(p + 78, (uint32_t)msg->success);
    wr32(p + 82, msg->nentries);
    q = p + RAFT_HDR;
    for (i = 0; i < msg->nentries; i++) {
        wr64(q, msg->entries[i].term); q += 8;
        wr32(q, msg->entries[i].clen); q += 4;
        if (msg->entries[i].clen) {
            memcpy(q, msg->entries[i].cmd, msg->entries[i].clen);
            q += msg->entries[i].clen;
        }
    }
    *out = p;
    *plen = n;
    return EFS_OK;
}

int sim_raft_send(void *net, const struct efs_raft_msg *msg)
{
    struct efs_sim *sim = net;
    struct sim_ev e;
    uint32_t plen = 0;
    int rc;

    if (!sim || !msg)
        return EFS_ERR_INVAL;
    if (msg->to < 0 || msg->to >= sim->nservers ||
        msg->from < 0 || msg->from >= sim->nservers)
        return EFS_ERR_INVAL;
    if (!sim->srv[msg->from].alive || sim->srv[msg->from].partitioned)
        return EFS_OK;
    if (!sim->srv[msg->to].alive || sim->srv[msg->to].partitioned)
        return EFS_OK;
    if (sim->drop_per_mille) {
        if (efs_sim_rng(sim) % 1000 < sim->drop_per_mille)
            return EFS_OK;
    }
    if (sim->delay_max == 0 && !sim->hold) {
        /* Synchronous delivery: the sender's entry buffers stay valid for
         * the whole recv (the caller frees them only after send returns)
         * and recv copies each entry into the log store, so no detach copy
         * is needed. */
        struct efs_raft *dst = sim_raft_of(sim, msg->to, msg->group);
        return dst ? efs_raft_recv(dst, msg) : EFS_OK;
    }
    memset(&e, 0, sizeof(e));
    e.kind = EV_RAFT;
    rc = pack_raft_ev(msg, &e.payload, &plen); /* deep-copies all entries */
    if (rc != EFS_OK)
        return rc;
    e.plen = plen;
    rc = sim_enqueue(sim, &e);
    if (rc != EFS_OK)
        free(e.payload);
    return rc;
}

int sim_raft_deliver(struct efs_sim *sim, struct sim_ev *e)
{
    struct efs_raft_msg m;
    uint8_t *cmd = NULL;
    int rc;

    if (!sim || !e || !e->payload)
        return EFS_ERR_INVAL;
    rc = unpack_raft_ev(e->payload, e->plen, &m, &cmd);
    if (rc != EFS_OK)
        return rc;
    if (m.to < 0 || m.to >= sim->nservers)
        return EFS_OK;
    if (!sim->srv[m.to].alive || sim->srv[m.to].partitioned)
        return EFS_OK;
    {
        struct efs_raft *dst = sim_raft_of(sim, m.to, m.group);
        if (!dst)
            return EFS_OK;
        return efs_raft_recv(dst, &m);
    }
}

static int drain_raft_due(struct efs_sim *sim)
{
    uint32_t i = 0;

    while (i < sim->nev) {
        struct sim_ev e;
        uint32_t j;

        if (sim->ev[i].kind != EV_RAFT || sim->ev[i].tick > sim->now) {
            i++;
            continue;
        }
        e = sim->ev[i];
        for (j = i + 1; j < sim->nev; j++)
            sim->ev[j - 1] = sim->ev[j];
        sim->nev--;
        sim_raft_deliver(sim, &e);
        free(e.payload);
        i = 0;
    }
    return EFS_OK;
}

static int tick_one(struct efs_sim *sim, int i)
{
    int rc;

    if (!reachable(sim, i))
        return EFS_OK;
    rc = efs_raft_tick(sim->srv[i].raft);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_tick(sim, i);
    if (rc != EFS_OK)
        return rc;
    rc = sim_ctrl_on_tick(sim, i);
    if (rc != EFS_OK)
        return rc;
    return drain_raft_due(sim);
}

int sim_raft_tick_reachable(struct efs_sim *sim)
{
    int i, rc;

    if (!sim)
        return EFS_ERR_INVAL;
    for (i = 0; i < sim->nraft; i++) {
        rc = tick_one(sim, i);
        if (rc != EFS_OK)
            return rc;
    }
    return drain_raft_due(sim);
}

static int wait_leader_g(struct efs_sim *sim, uint8_t group)
{
    int t, lid;

    for (t = 0; t < WAIT_TICKS; t++) {
        lid = leader_id_g(sim, group);
        if (lid >= 0)
            return lid;
        if (sim_raft_tick_reachable(sim) != EFS_OK)
            return -1;
    }
    return -1;
}

static int wait_leader(struct efs_sim *sim)
{
    return wait_leader_g(sim, EFS_RAFT_GROUP_SHARD);
}

static int mutate_g(struct efs_sim *sim, uint8_t group, const uint8_t *cmd,
                    uint32_t clen)
{
    int lid, t, rc;
    uint64_t idx = 0;
    struct efs_raft *r;

    lid = wait_leader_g(sim, group);
    if (lid < 0)
        return EFS_ERR_BUSY;
    r = sim_raft_of(sim, lid, group);
    if (!r)
        return EFS_ERR_BUSY;
    rc = efs_raft_propose(r, cmd, clen, &idx);
    if (rc != EFS_OK)
        return rc;
    for (t = 0; t < WAIT_TICKS; t++) {
        if (efs_raft_role(r) != EFS_RAFT_LEADER)
            return EFS_ERR_BUSY;
        if (efs_raft_commit(r) >= idx && efs_raft_applied(r) >= idx)
            break;
        rc = tick_one(sim, lid);
        if (rc != EFS_OK)
            return rc;
        rc = sim_raft_tick_reachable(sim);
        if (rc != EFS_OK)
            return rc;
    }
    if (efs_raft_applied(r) < idx)
        return EFS_ERR_BUSY;
    if (group > 2 || sim->srv[lid].applied_idx_g[group] != idx)
        return EFS_ERR_BUSY;
    if (sim->srv[lid].applied_ino_g[group])
        sim->last_ino = sim->srv[lid].applied_ino_g[group];
    sim->last_extra = sim->srv[lid].applied_extra_g[group];
    return sim->srv[lid].applied_rc_g[group];
}

int sim_raft_propose_group(struct efs_sim *sim, uint8_t group,
                           const uint8_t *cmd, uint32_t clen)
{
    return mutate_g(sim, group, cmd, clen);
}

static int read_begin_g(struct efs_sim *sim, uint8_t group)
{
    int lid, t, rc;
    struct efs_raft *r;

    lid = wait_leader_g(sim, group);
    if (lid < 0)
        return EFS_ERR_BUSY;
    r = sim_raft_of(sim, lid, group);
    if (!r)
        return EFS_ERR_BUSY;
    rc = efs_raft_read_begin(r);
    if (rc != EFS_OK)
        return rc;
    for (t = 0; t < WAIT_TICKS; t++) {
        if (efs_raft_read_ready(r))
            return EFS_OK;
        if (efs_raft_role(r) != EFS_RAFT_LEADER)
            return EFS_ERR_BUSY;
        rc = sim_raft_tick_reachable(sim);
        if (rc != EFS_OK)
            return rc;
    }
    return efs_raft_read_ready(r) ? EFS_OK : EFS_ERR_BUSY;
}

int sim_raft_read_group(struct efs_sim *sim, uint8_t group)
{
    return read_begin_g(sim, group);
}

struct efs_kv *sim_raft_kv_group(struct efs_sim *sim, uint8_t group)
{
    int lid = leader_id_g(sim, group);

    if (lid < 0)
        return NULL;
    return sim->srv[lid].disk;
}

static int attach(struct efs_sim *sim, int i)
{
    struct efs_raft_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = i;
    cfg.n = EFS_SIM_RAFT_N;
    cfg.voters = (1u << EFS_SIM_RAFT_N) - 1;
    cfg.boot_id = sim->srv[i].boot_id ? sim->srv[i].boot_id : 1;
    cfg.group = EFS_RAFT_GROUP_SHARD;
    /* Per-node staggered base; raft.c adds a seeded randomized deadline within
     * each band. The stagger keeps node 0 the preferred leader of EVERY group,
     * which the cross-shard txn tests currently rely on — a txn whose groups
     * elect DIFFERENT leaders exposes a separate consistency gap (uniform
     * timeouts revealed it; parked, see design-history). */
    cfg.election_ticks = (uint32_t)(4 + i * 4);
    cfg.heartbeat_ticks = 1;
    cfg.store = sim->srv[i].raft_store;
    cfg.store_ctx = sim->srv[i].raft_store;
    cfg.send = sim_raft_send;
    cfg.net = sim;
    cfg.apply = raft_apply;
    cfg.app = &sim->srv[i];
    sim->srv[i].sim = sim;
    sim->srv[i].raft = efs_raft_new(&cfg);
    return sim->srv[i].raft ? EFS_OK : EFS_ERR_NOMEM;
}

int sim_raft_boot(struct efs_sim *sim)
{
    int i, rc;

    sim->nraft = sim->nservers;
    if (sim->nservers < EFS_SIM_RAFT_N)
        return EFS_ERR_INVAL;
    sim->desired_voters = (1u << EFS_SIM_RAFT_N) - 1;
    for (i = 0; i < sim->nraft; i++) {
        sim->srv[i].boot_id = 1;
        sim->srv[i].raft_store =
            sim_raft_store_new(sim, i, EFS_RAFT_GROUP_SHARD);
        if (!sim->srv[i].raft_store)
            return EFS_ERR_NOMEM;
        rc = attach(sim, i);
        if (rc != EFS_OK)
            return rc;
    }
    rc = sim_ctrl_boot(sim);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_boot(sim);
    if (rc != EFS_OK)
        return rc;
    if (wait_leader(sim) < 0)
        return EFS_ERR_BUSY;
    return sim_raft_mkfs(sim);
}

void sim_raft_halt(struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft)
        return;
    efs_raft_free(sim->srv[server].raft);
    sim->srv[server].raft = NULL;
    sim_txn_halt(sim, server);
    sim_ctrl_halt(sim, server);
}

int sim_raft_restart(struct efs_sim *sim, int server)
{
    int rc;

    if (!sim || server < 0 || server >= sim->nraft)
        return EFS_ERR_INVAL;
    if (sim->srv[server].raft)
        return EFS_OK;
    if (!sim->srv[server].raft_store)
        return EFS_ERR_INVAL;
    /* On the durable store, a restart really reopens the log, so every crash
     * the sim injects becomes a replay of what was on disk. halt() already
     * freed all three groups' raft instances, so nothing points at the old
     * store objects. */
    if (sim->raft_dir) {
        sim_raft_disk_close(sim, server);
        sim->srv[server].raft_store =
            sim_raft_store_new(sim, server, EFS_RAFT_GROUP_SHARD);
        sim->srv[server].raft2_store =
            sim_raft_store_new(sim, server, EFS_RAFT_GROUP_SHARD2);
        sim->srv[server].ctrl_store =
            sim_raft_store_new(sim, server, EFS_RAFT_GROUP_CTRL);
        if (!sim->srv[server].raft_store || !sim->srv[server].raft2_store ||
            !sim->srv[server].ctrl_store)
            return EFS_ERR_IO;
    }
    sim->srv[server].boot_id++;
    rc = attach(sim, server);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_restart(sim, server);
    if (rc != EFS_OK)
        return rc;
    return sim_ctrl_restart(sim, server);
}

void sim_raft_free_all(struct efs_sim *sim)
{
    int i;

    if (!sim)
        return;
    for (i = 0; i < EFS_SIM_MAX_SERVERS; i++) {
        efs_raft_free(sim->srv[i].raft);
        sim->srv[i].raft = NULL;
        sim_raft_store_del(sim, sim->srv[i].raft_store);
        sim->srv[i].raft_store = NULL;
    }
    sim_txn_free_all(sim);
    sim_ctrl_free_all(sim);
    /* Last: every group's store lives in this log, so it closes only after
     * all three groups have let go of theirs. */
    for (i = 0; i < EFS_SIM_MAX_SERVERS; i++)
        sim_raft_disk_close(sim, i);
}

static int parent_dsh(struct efs_sim *sim, efs_ino_t parent, const char *name,
                      uint32_t *dsh)
{
    struct efs_meta_row row;
    struct efs_kv *kv;
    uint32_t psh;
    int rc;

    psh = efs_kv_inode_shard(parent);
    rc = sim_raft_read_group(sim, sim_shard_group(psh));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(psh));
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_meta_apply_get_inode(kv, parent, &row);
    if (rc != EFS_OK)
        return rc;
    *dsh = efs_kv_dentry_shard(parent, name, row.layout);
    return EFS_OK;
}

int sim_raft_mkfs(struct efs_sim *sim)
{
    uint8_t cmd[17];
    uint32_t sh = efs_kv_inode_shard(EFS_ROOT_INO);

    if (!sim)
        return EFS_ERR_INVAL;
    cmd[0] = CMD_MKFS;
    wr64(cmd + 1, sim->now);
    wr64(cmd + 9, sim->export_salt);
    return sim_raft_propose_group(sim, sim_shard_group(sh), cmd, 17);
}

int sim_raft_export_salt(struct efs_sim *sim, uint64_t *out)
{
    uint32_t sh = efs_kv_inode_shard(EFS_ROOT_INO);
    struct efs_kv *kv;
    int rc;

    if (!sim || !out)
        return EFS_ERR_INVAL;
    rc = sim_raft_read_group(sim, sim_shard_group(sh));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(sh));
    if (!kv)
        return EFS_ERR_BUSY;
    return efs_meta_apply_export_salt(kv, out);
}

int sim_raft_create(struct efs_sim *sim, int client, int has_op,
                    const struct efs_opid *op, efs_ino_t parent, uint32_t mode,
                    const char *name, efs_ino_t *out)
{
    uint8_t cmd[CMD_MAX];
    uint32_t clen = 0, epoch, dsh;
    struct efs_meta_attrs at;
    const uint8_t *uuid;
    int rc;

    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    if (has_op && op) {
        uuid = op->client_uuid;
        epoch = op->session_epoch;
    } else {
        uuid = sim->cli[client].win.client_uuid;
        epoch = sim->cli[client].win.session_epoch;
    }
    rc = parent_dsh(sim, parent, name, &dsh);
    if (rc != EFS_OK)
        return rc;
    rc = sim_sess_ensure_id(sim, uuid, epoch, dsh);
    if (rc != EFS_OK)
        return rc;
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = sim->now; /* simulated clock: the leader stamps, apply obeys */
    rc = pack_create(cmd, &clen, has_op, op, uuid, epoch, parent, mode, name,
                     &at);
    if (rc != EFS_OK)
        return rc;
    rc = sim_raft_propose_group(sim, sim_shard_group(dsh), cmd, clen);
    if (out)
        *out = (rc == EFS_OK) ? sim->last_ino : 0;
    return rc;
}

int sim_raft_unlink(struct efs_sim *sim, int client, efs_ino_t parent,
                    const char *name)
{
    uint8_t cmd[CMD_MAX];
    uint32_t clen = 0, dsh;
    const uint8_t *uuid;
    uint32_t epoch;
    int rc;

    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    rc = parent_dsh(sim, parent, name, &dsh);
    if (rc != EFS_OK)
        return rc;
    rc = sim_sess_ensure_id(sim, uuid, epoch, dsh);
    if (rc != EFS_OK)
        return rc;
    rc = pack_unlink(cmd, &clen, uuid, epoch, parent, sim->now, name);
    if (rc != EFS_OK)
        return rc;
    return sim_raft_propose_group(sim, sim_shard_group(dsh), cmd, clen);
}

int sim_raft_publish(struct efs_sim *sim, int client, const struct efs_meta_pub *p)
{
    uint8_t cmd[CMD_MAX];
    uint32_t clen = 0, epoch, lsh;
    const uint8_t *uuid;
    uint8_t lane;
    int rc;

    if (!sim || !p || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    lane = (uint8_t)(p->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(p->ino, lane);
    rc = sim_sess_ensure_id(sim, uuid, epoch, lsh);
    if (rc != EFS_OK)
        return rc;
    {
        struct efs_meta_pub stamped = *p;

        /* Leader-stamped: apply is a function of the entry, not of whoever
         * happens to be applying it. Same contract as CREATE/SETATTR. */
        stamped.now = sim->now;
        rc = pack_publish(cmd, &clen, uuid, epoch, &stamped);
    }
    if (rc != EFS_OK)
        return rc;
    return sim_raft_propose_group(sim, sim_shard_group(lsh), cmd, clen);
}

int sim_raft_epoch_fence(struct efs_sim *sim, efs_ino_t ino)
{
    uint8_t cmd[9];
    uint32_t shard;

    if (!sim || ino == 0)
        return EFS_ERR_INVAL;
    cmd[0] = CMD_EPOCH;
    wr64(cmd + 1, ino);
    shard = efs_kv_inode_shard(ino);
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, 9);
}

int sim_raft_lookup(struct efs_sim *sim, efs_ino_t parent, const char *name,
                    struct efs_meta_dentry *out)
{
    return sim_txn_lookup(sim, parent, name, out);
}

static int read_inode(struct efs_sim *sim, efs_ino_t ino, struct efs_meta_row *row)
{
    uint32_t sh = efs_kv_inode_shard(ino);
    struct efs_kv *kv;
    int rc;

    rc = sim_raft_read_group(sim, sim_shard_group(sh));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(sh));
    if (!kv)
        return EFS_ERR_BUSY;
    return efs_meta_apply_get_inode(kv, ino, row);
}

int sim_raft_setattr(struct efs_sim *sim, int client, efs_ino_t ino,
                     const struct efs_meta_setattr *sa)
{
    uint8_t cmd[61];
    uint32_t sh, epoch;
    const uint8_t *uuid;
    int rc;

    if (!sim || !sa || ino == 0 || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure(sim, client, sh);
    if (rc != EFS_OK)
        return rc;
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    cmd[0] = CMD_SETATTR;
    wr64(cmd + 1, ino);
    wr64(cmd + 9, sim->now);
    wr64(cmd + 17, sa->expect_gen);
    wr32(cmd + 25, sa->mask);
    wr32(cmd + 29, sa->mode);
    wr32(cmd + 33, sa->uid);
    wr32(cmd + 37, sa->gid);
    memcpy(cmd + 41, uuid, EFS_OPID_UUID_LEN);
    wr32(cmd + 41 + EFS_OPID_UUID_LEN, epoch);
    return sim_raft_propose_group(sim, sim_shard_group(sh), cmd, 61);
}

int sim_raft_utimens(struct efs_sim *sim, int client, efs_ino_t ino,
                     const struct efs_meta_utimens *u)
{
    struct efs_meta_row row;
    struct efs_meta_utimens cmd_u;
    uint8_t cmd[73];
    uint32_t sh, epoch;
    const uint8_t *uuid;
    int rc;

    if (!sim || !u || ino == 0 || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure(sim, client, sh);
    if (rc != EFS_OK)
        return rc;
    rc = read_inode(sim, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (u->expect_gen != 0 && u->expect_gen != row.generation)
        return EFS_ERR_STALE;
    cmd_u = *u;
    if (cmd_u.mask & EFS_META_SET_MTIME) {
        if (cmd_u.mtime_gen == 0)
            cmd_u.mtime_gen = row.mtime_gen + 1;
    } else {
        cmd_u.mtime_gen = 0;
    }
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    cmd[0] = CMD_UTIMENS;
    wr64(cmd + 1, ino);
    wr64(cmd + 9, sim->now);
    wr64(cmd + 17, cmd_u.expect_gen);
    wr32(cmd + 25, cmd_u.mask);
    wr64(cmd + 29, cmd_u.mtime);
    wr64(cmd + 37, cmd_u.atime);
    wr64(cmd + 45, cmd_u.mtime_gen);
    memcpy(cmd + 53, uuid, EFS_OPID_UUID_LEN);
    wr32(cmd + 53 + EFS_OPID_UUID_LEN, epoch);
    return sim_raft_propose_group(sim, sim_shard_group(sh), cmd, 73);
}

int sim_raft_truncate(struct efs_sim *sim, int client, efs_ino_t ino,
                      uint64_t size, const struct efs_meta_pub *tail)
{
    struct efs_meta_row row;
    uint8_t cmd[TRUNC_HDR + TRUNC_TAIL];
    uint32_t sh, epoch, n = TRUNC_HDR;
    const uint8_t *uuid;
    int i, rc;

    if (!sim || ino == 0 || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    if (size > 0 && (size % EFS_MIN_CHUNK_SIZE) != 0 && !tail)
        return EFS_ERR_INVAL;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure(sim, client, sh);
    if (rc != EFS_OK)
        return rc;
    rc = read_inode(sim, ino, &row);
    if (rc != EFS_OK)
        return rc;
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    cmd[0] = CMD_TRUNCATE;
    wr64(cmd + 1, ino);
    wr64(cmd + 9, sim->now);
    wr64(cmd + 17, row.generation);
    wr64(cmd + 25, size);
    if (tail) {
        if (tail->chunk_index != (uint32_t)(size / EFS_MIN_CHUNK_SIZE))
            return EFS_ERR_INVAL;
        cmd[33] = 1;
        memcpy(cmd + 34, uuid, EFS_OPID_UUID_LEN);
        wr32(cmd + 34 + EFS_OPID_UUID_LEN, epoch);
        wr32(cmd + TRUNC_HDR, tail->chunk_index);
        wr64(cmd + TRUNC_HDR + 4, tail->candidate_gen);
        wr64(cmd + TRUNC_HDR + 12, tail->expected_gen);
        wr32(cmd + TRUNC_HDR + 20, tail->coding_profile_id);
        n = TRUNC_HDR + 24;
        for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            wr32(cmd + n, tail->ch.nodes[i]);
            n += 4;
            memcpy(cmd + n, tail->ch.checksums[i], EFS_HASH_SIZE);
            n += EFS_HASH_SIZE;
        }
    } else {
        cmd[33] = 0;
        memcpy(cmd + 34, uuid, EFS_OPID_UUID_LEN);
        wr32(cmd + 34 + EFS_OPID_UUID_LEN, epoch);
    }
    return sim_raft_propose_group(sim, sim_shard_group(sh), cmd, n);
}

int sim_raft_append_reserve(struct efs_sim *sim, int client,
                            const struct efs_opid *op, efs_ino_t ino,
                            uint64_t len, uint64_t *off_out)
{
    uint8_t cmd[45];
    uint32_t sh;
    int rc;

    if (!sim || !op || ino == 0 || len == 0 || client < 0 ||
        client >= sim->nclients)
        return EFS_ERR_INVAL;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure_id(sim, op->client_uuid, op->session_epoch, sh);
    if (rc != EFS_OK)
        return rc;
    cmd[0] = CMD_APPEND_RSV;
    wr64(cmd + 1, ino);
    wr64(cmd + 9, len);
    memcpy(cmd + 17, op->client_uuid, EFS_OPID_UUID_LEN);
    wr32(cmd + 17 + EFS_OPID_UUID_LEN, op->session_epoch);
    wr64(cmd + 37, op->seq);
    rc = sim_raft_propose_group(sim, sim_shard_group(sh), cmd, 45);
    if (off_out)
        *off_out = (rc == EFS_OK) ? sim->last_extra : 0;
    return rc;
}

int sim_raft_append_resolve(struct efs_sim *sim, int client, efs_ino_t ino,
                            uint64_t off, int outcome)
{
    uint8_t cmd[38];
    uint32_t sh, epoch;
    const uint8_t *uuid;
    int rc;

    if (!sim || ino == 0 || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure(sim, client, sh);
    if (rc != EFS_OK)
        return rc;
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    cmd[0] = CMD_APPEND_RES;
    wr64(cmd + 1, ino);
    wr64(cmd + 9, off);
    cmd[17] = (uint8_t)outcome;
    memcpy(cmd + 18, uuid, EFS_OPID_UUID_LEN);
    wr32(cmd + 18 + EFS_OPID_UUID_LEN, epoch);
    return sim_raft_propose_group(sim, sim_shard_group(sh), cmd, 38);
}

int sim_raft_getattr(struct efs_sim *sim, efs_ino_t ino, struct efs_meta_stat *out)
{
    struct efs_meta_row row;
    struct efs_kv *kv;
    uint32_t ish, i;
    uint64_t bits;
    int rc;

    if (!sim || !out || ino == 0)
        return EFS_ERR_INVAL;
    rc = read_inode(sim, ino, &row);
    if (rc != EFS_OK)
        return rc;
    ish = efs_kv_inode_shard(ino);
    bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
               ? row.used_shards
               : row.active_lanes;
    for (i = 0; i < EFS_META_LANES; i++) {
        uint32_t lsh;

        if ((bits & (1ULL << i)) == 0)
            continue;
        lsh = efs_kv_lane_shard(ino, (uint8_t)i);
        if (lsh == ish)
            continue;
        rc = sim_raft_read_group(sim, sim_shard_group(lsh));
        if (rc != EFS_OK)
            return rc;
    }
    kv = sim_raft_kv_group(sim, sim_shard_group(ish));
    if (!kv)
        return EFS_ERR_BUSY;
    return efs_meta_apply_getattr(kv, ino, sim_txn_coord, sim, out);
}

int sim_raft_readdir(struct efs_sim *sim, efs_ino_t dir,
                     struct efs_meta_dir_cursor *cur, struct efs_meta_dir_ent *out,
                     uint32_t max, uint32_t *n)
{
    struct efs_meta_row row;
    struct efs_kv *kv;
    uint32_t ish, i;
    int rc;

    if (!sim || !cur || !out || !n || dir == 0)
        return EFS_ERR_INVAL;
    rc = read_inode(sim, dir, &row);
    if (rc != EFS_OK)
        return rc;
    ish = efs_kv_inode_shard(dir);
    if (row.layout != EFS_META_LAYOUT_LOCAL) {
        for (i = 0; i < 64; i++) {
            uint32_t dsh;

            if ((row.used_shards & (1ULL << i)) == 0)
                continue;
            dsh = efs_kv_lane_shard(dir, (uint8_t)i);
            rc = sim_raft_read_group(sim, sim_shard_group(dsh));
            if (rc != EFS_OK)
                return rc;
        }
    }
    kv = sim_raft_kv_group(sim, sim_shard_group(ish));
    if (!kv)
        return EFS_ERR_BUSY;
    return efs_meta_apply_readdir(kv, dir, cur, out, max, n);
}

int sim_raft_lookup_path(struct efs_sim *sim, efs_ino_t start, const char *path,
                         struct efs_meta_path_hop *hops, uint32_t cap, uint32_t *n)
{
    efs_ino_t cur;
    const char *p;
    uint32_t nh = 0;
    int rc;

    if (!sim || !path || !hops || !n || cap == 0)
        return EFS_ERR_INVAL;
    if (cap > EFS_META_PATH_MAX)
        cap = EFS_META_PATH_MAX;
    cur = start ? start : EFS_ROOT_INO;
    p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        struct efs_meta_row row;

        rc = read_inode(sim, cur, &row);
        if (rc != EFS_OK)
            return rc;
        hops[0].ino = row.ino;
        hops[0].generation = row.generation;
        hops[0].mode = row.mode;
        hops[0].uid = row.uid;
        hops[0].gid = row.gid;
        *n = 1;
        return EFS_OK;
    }
    while (*p && nh < cap) {
        char name[EFS_MAX_NAME];
        size_t nlen;
        const char *s = p;
        struct efs_meta_dentry dent;
        struct efs_meta_row row;
        int more;

        while (*p && *p != '/')
            p++;
        nlen = (size_t)(p - s);
        if (nlen == 0)
            break;
        if (nlen >= EFS_MAX_NAME)
            return EFS_ERR_NAMETOOLONG;
        memcpy(name, s, nlen);
        name[nlen] = '\0';
        while (*p == '/')
            p++;
        rc = sim_txn_lookup(sim, cur, name, &dent);
        if (rc != EFS_OK)
            return rc;
        rc = read_inode(sim, dent.ino, &row);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_ERR_IO;
        if (rc != EFS_OK)
            return rc;
        more = (*p != '\0');
        if (more && !S_ISDIR(row.mode))
            return EFS_ERR_INVAL;
        hops[nh].ino = row.ino;
        hops[nh].generation = row.generation;
        hops[nh].mode = row.mode;
        hops[nh].uid = row.uid;
        hops[nh].gid = row.gid;
        nh++;
        cur = row.ino;
    }
    *n = nh;
    return nh ? EFS_OK : EFS_ERR_INVAL;
}

int sim_raft_get_chunk(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                       struct efs_meta_chunk *out)
{
    struct efs_kv *kv;
    uint32_t lsh;
    uint8_t lane;
    int rc;

    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(ino, lane);
    rc = sim_raft_read_group(sim, sim_shard_group(lsh));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(lsh));
    if (!kv)
        return EFS_ERR_BUSY;
    return efs_meta_apply_get_chunk(kv, ino, chunk_index, out);
}

int sim_raft_check(struct efs_sim *sim)
{
    int i, j;
    int lid;
    struct efs_kv *kv;

    if (!sim)
        return EFS_ERR_INVAL;
    /* I1: at most one leader per term (including partitioned replicas). */
    for (i = 0; i < sim->nraft; i++) {
        uint64_t t;
        int n = 0;

        if (!sim->srv[i].raft)
            continue;
        if (efs_raft_role(sim->srv[i].raft) != EFS_RAFT_LEADER)
            continue;
        t = efs_raft_term(sim->srv[i].raft);
        for (j = 0; j < sim->nraft; j++) {
            if (!sim->srv[j].raft)
                continue;
            if (efs_raft_role(sim->srv[j].raft) == EFS_RAFT_LEADER &&
                efs_raft_term(sim->srv[j].raft) == t)
                n++;
        }
        if (n > 1)
            return EFS_ERR_PROTO;
    }
    lid = leader_id(sim);
    if (lid < 0)
        return EFS_OK;
    kv = sim->srv[lid].disk;
    return kv ? efs_meta_apply_check(kv) : EFS_ERR_INVAL;
}

int efs_sim_meta_leader(const struct efs_sim *sim)
{
    return sim ? leader_id(sim) : -1;
}

int efs_sim_meta_role(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft)
        return -1;
    return efs_raft_role(sim->srv[server].raft);
}

uint64_t efs_sim_meta_term(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft)
        return 0;
    return efs_raft_term(sim->srv[server].raft);
}

uint64_t efs_sim_meta_commit(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft)
        return 0;
    return efs_raft_commit(sim->srv[server].raft);
}

int efs_sim_meta_tick(struct efs_sim *sim, int server)
{
    if (!sim)
        return EFS_ERR_INVAL;
    if (server < 0)
        return sim_raft_tick_reachable(sim);
    return tick_one(sim, server);
}
uint32_t efs_sim_meta_voters(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft)
        return 0;
    return efs_raft_voters(sim->srv[server].raft);
}

int efs_sim_meta_joint(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft)
        return 0;
    return efs_raft_joint(sim->srv[server].raft);
}

int efs_sim_meta2_leader(const struct efs_sim *sim)
{
    return sim ? leader_id_g(sim, EFS_RAFT_GROUP_SHARD2) : -1;
}

uint32_t efs_sim_meta2_voters(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft2)
        return 0;
    return efs_raft_voters(sim->srv[server].raft2);
}

int efs_sim_meta2_joint(const struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= sim->nraft || !sim->srv[server].raft2)
        return 0;
    return efs_raft_joint(sim->srv[server].raft2);
}
