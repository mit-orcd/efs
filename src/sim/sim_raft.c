/* Single-shard Raft group inside efs_sim (architecture.md §10 step 5).
 * RF = 2f+1 = 3 (f=1). Mutations are log commands; apply is meta_apply
 * on each replica's KV. Reads are leader + ReadIndex. Messages ride the
 * sim event queue (delay/drop/partition); AE payloads are copied. */
#include "sim_internal.h"
#include "efs/opid.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CMD_CREATE  1
#define CMD_UNLINK  2
#define CMD_PUBLISH 3
#define CMD_EPOCH   SIM_CMD_EPOCH
#define CMD_MAX     512
#define WAIT_TICKS  80
#define RAFT_HDR    98

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

    lim = (group == EFS_RAFT_GROUP_SHARD2) ? EFS_SIM_RAFT_N : sim->nraft;
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

static int pack_create(uint8_t *out, uint32_t *len, int has_op,
                       const struct efs_opid *op, const uint8_t *uuid,
                       uint32_t epoch, efs_ino_t parent, uint32_t mode,
                       const char *name)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = 1 + 1 + 8 + 4 + 1 + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (has_op)
        n += 8;
    if (n > CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = CMD_CREATE;
    out[1] = has_op ? 1 : 0;
    wr64(out + 2, parent);
    wr32(out + 10, mode);
    out[14] = (uint8_t)nl;
    memcpy(out + 15, name, nl);
    p = out + 15 + nl;
    memcpy(p, uuid, EFS_OPID_UUID_LEN);
    wr32(p + EFS_OPID_UUID_LEN, epoch);
    if (has_op)
        wr64(p + EFS_OPID_UUID_LEN + 4, op->seq);
    *len = n;
    return EFS_OK;
}

static int pack_unlink(uint8_t *out, uint32_t *len, const uint8_t *uuid,
                       uint32_t epoch, efs_ino_t parent, const char *name)
{
    size_t nl = strlen(name);
    uint32_t n;
    uint8_t *p;

    if (nl >= EFS_MAX_NAME)
        return EFS_ERR_NAMETOOLONG;
    n = 1 + 8 + 1 + (uint32_t)nl + EFS_OPID_UUID_LEN + 4;
    if (n > CMD_MAX)
        return EFS_ERR_INVAL;
    out[0] = CMD_UNLINK;
    wr64(out + 1, parent);
    out[9] = (uint8_t)nl;
    memcpy(out + 10, name, nl);
    p = out + 10 + nl;
    memcpy(p, uuid, EFS_OPID_UUID_LEN);
    wr32(p + EFS_OPID_UUID_LEN, epoch);
    *len = n;
    return EFS_OK;
}

static int pack_publish(uint8_t *out, uint32_t *len, const uint8_t *uuid,
                        uint32_t epoch, const struct efs_meta_pub *p)
{
    uint32_t n = 1 + 8 + 4 + 8 +
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
    q = out + 21;
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
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent, ino = 0;
    uint32_t mode, epoch;
    char name[EFS_MAX_NAME];
    uint8_t nl, has_op, uuid[EFS_OPID_UUID_LEN];
    int rc;
    struct efs_opid op;
    struct efs_meta_dentry dent;
    const uint8_t *p;

    if (clen < 15)
        return EFS_ERR_PROTO;
    has_op = cmd[1];
    parent = rd64(cmd + 2);
    mode = rd32(cmd + 10);
    nl = cmd[14];
    if ((uint32_t)15 + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_ERR_PROTO;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + 15, nl);
    p = cmd + 15 + nl;
    memcpy(uuid, p, EFS_OPID_UUID_LEN);
    epoch = rd32(p + EFS_OPID_UUID_LEN);
    memset(&op, 0, sizeof(op));
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(parent), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, 0);
        return EFS_OK;
    }
    if (has_op) {
        if ((uint32_t)(p - cmd) + EFS_OPID_UUID_LEN + 4 + 8 > clen)
            return EFS_ERR_PROTO;
        memcpy(op.client_uuid, uuid, EFS_OPID_UUID_LEN);
        op.session_epoch = epoch;
        op.seq = rd64(p + EFS_OPID_UUID_LEN + 4);
        rc = efs_meta_apply_create_file_op(s->disk, &op, parent, mode, name,
                                           &ino);
    } else {
        rc = efs_meta_apply_create_file(s->disk, parent, mode, name, &ino);
    }
    if (rc == EFS_ERR_EXIST &&
        efs_meta_apply_lookup(s->disk, parent, name, &dent) == EFS_OK)
        ino = dent.ino;
    sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, ino);
    return EFS_OK;
}

static int apply_unlink_cmd(struct sim_server *s, const uint8_t *cmd,
                            uint32_t clen, uint64_t index)
{
    efs_ino_t parent;
    char name[EFS_MAX_NAME];
    uint8_t nl, uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    const uint8_t *p;
    int rc;

    if (clen < 10)
        return EFS_ERR_PROTO;
    parent = rd64(cmd + 1);
    nl = cmd[9];
    if ((uint32_t)10 + nl + EFS_OPID_UUID_LEN + 4 > clen)
        return EFS_ERR_PROTO;
    memset(name, 0, sizeof(name));
    memcpy(name, cmd + 10, nl);
    p = cmd + 10 + nl;
    memcpy(uuid, p, EFS_OPID_UUID_LEN);
    epoch = rd32(p + EFS_OPID_UUID_LEN);
    rc = efs_session_accept(s->disk, efs_kv_inode_shard(parent), uuid, epoch);
    if (rc != EFS_OK) {
        sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, 0);
        return EFS_OK;
    }
    rc = efs_meta_apply_unlink(s->disk, parent, name);
    sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, 0);
    return EFS_OK;
}

static int apply_publish_cmd(struct sim_server *s, const uint8_t *cmd,
                             uint32_t clen, uint64_t index)
{
    struct efs_meta_pub p;
    uint32_t need, epoch;
    const uint8_t *q;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint8_t lane;
    int i, rc;

    need = 21 + (uint32_t)EFS_NUM_FRAGMENTS * (4 + EFS_HASH_SIZE) +
           EFS_OPID_UUID_LEN + 4 + 8 + 8 + 8 + 4;
    if (clen < need)
        return EFS_ERR_PROTO;
    memset(&p, 0, sizeof(p));
    p.ino = rd64(cmd + 1);
    p.chunk_index = rd32(cmd + 9);
    p.new_size = rd64(cmd + 13);
    q = cmd + 21;
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
        sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, p.ino);
        return EFS_OK;
    }
    rc = efs_meta_apply_publish(s->disk, &p);
    sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, p.ino);
    return EFS_OK;
}

static int apply_epoch_cmd(struct sim_server *s, const uint8_t *cmd,
                           uint32_t clen, uint64_t index)
{
    efs_ino_t ino;
    int rc;

    if (clen < 9)
        return EFS_ERR_PROTO;
    ino = rd64(cmd + 1);
    rc = efs_meta_apply_epoch_fence(s->disk, ino);
    sim_note_apply(s, EFS_RAFT_GROUP_SHARD, index, rc, ino);
    return EFS_OK;
}

static int raft_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct sim_server *s = app;

    (void)term;
    if (!s || !s->disk || !cmd || clen == 0)
        return EFS_OK;
    switch (cmd[0]) {
    case CMD_CREATE:
        return apply_create_cmd(s, cmd, clen, index);
    case CMD_UNLINK:
        return apply_unlink_cmd(s, cmd, clen, index);
    case CMD_PUBLISH:
        return apply_publish_cmd(s, cmd, clen, index);
    case CMD_EPOCH:
        return apply_epoch_cmd(s, cmd, clen, index);
    case SIM_CMD_SESSION:
        return sim_sess_apply(s, EFS_RAFT_GROUP_SHARD, cmd, clen, index);
    default:
        return sim_txn_apply(s, EFS_RAFT_GROUP_SHARD, cmd, clen, index);
    }
}

static int unpack_raft_ev(const uint8_t *p, uint32_t n, struct efs_raft_msg *m,
                          uint8_t **cmd_out)
{
    uint32_t clen;

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
    m->entries[0].term = rd64(p + 86);
    clen = rd32(p + 94);
    m->entries[0].clen = clen;
    if (m->nentries && clen) {
        if (n < RAFT_HDR + clen)
            return EFS_ERR_PROTO;
        *cmd_out = (uint8_t *)p + RAFT_HDR;
        m->entries[0].cmd = *cmd_out;
    } else {
        *cmd_out = NULL;
        m->entries[0].cmd = NULL;
    }
    return EFS_OK;
}

static int pack_raft_ev(const struct efs_raft_msg *msg, uint8_t **out,
                        uint32_t *plen)
{
    uint32_t clen = (msg->nentries && msg->entries[0].clen) ?
                    msg->entries[0].clen : 0;
    uint32_t n = RAFT_HDR + clen;
    uint8_t *p = malloc(n);

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
    wr64(p + 86, msg->entries[0].term);
    wr32(p + 94, clen);
    if (clen)
        memcpy(p + RAFT_HDR, msg->entries[0].cmd, clen);
    *out = p;
    *plen = n;
    return EFS_OK;
}

int sim_raft_send(void *net, const struct efs_raft_msg *msg)
{
    struct efs_sim *sim = net;
    uint8_t *copy = NULL;
    struct efs_raft_msg m;
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
    m = *msg;
    if (m.nentries && m.entries[0].clen) {
        copy = malloc(m.entries[0].clen);
        if (!copy)
            return EFS_ERR_NOMEM;
        memcpy(copy, m.entries[0].cmd, m.entries[0].clen);
        m.entries[0].cmd = copy;
    }
    if (sim->delay_max == 0 && !sim->hold) {
        struct efs_raft *dst = sim_raft_of(sim, msg->to, m.group);
        rc = dst ? efs_raft_recv(dst, &m) : EFS_OK;
        free(copy);
        return rc;
    }
    memset(&e, 0, sizeof(e));
    e.kind = EV_RAFT;
    rc = pack_raft_ev(&m, &e.payload, &plen);
    free(copy);
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
    return sim->srv[lid].applied_rc_g[group];
}

static int mutate(struct efs_sim *sim, const uint8_t *cmd, uint32_t clen)
{
    return mutate_g(sim, EFS_RAFT_GROUP_SHARD, cmd, clen);
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

static int read_begin(struct efs_sim *sim)
{
    return read_begin_g(sim, EFS_RAFT_GROUP_SHARD);
}

int sim_raft_read_group(struct efs_sim *sim, uint8_t group)
{
    return read_begin_g(sim, group);
}

static struct efs_kv *leader_kv_g(struct efs_sim *sim, uint8_t group)
{
    int lid = leader_id_g(sim, group);

    if (lid < 0)
        return NULL;
    return sim->srv[lid].disk;
}

static struct efs_kv *leader_kv(struct efs_sim *sim)
{
    return leader_kv_g(sim, EFS_RAFT_GROUP_SHARD);
}

struct efs_kv *sim_raft_kv_group(struct efs_sim *sim, uint8_t group)
{
    return leader_kv_g(sim, group);
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
        if (efs_meta_apply_init(sim->srv[i].disk) != EFS_OK)
            return EFS_ERR_IO;
        sim->srv[i].boot_id = 1;
        sim->srv[i].raft_store = efs_raft_mem_create();
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
    (void)wait_leader(sim);
    return EFS_OK;
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
        efs_raft_mem_free(sim->srv[i].raft_store);
        sim->srv[i].raft_store = NULL;
    }
    sim_txn_free_all(sim);
    sim_ctrl_free_all(sim);
}

int sim_raft_create(struct efs_sim *sim, int client, int has_op,
                    const struct efs_opid *op, efs_ino_t parent, uint32_t mode,
                    const char *name, efs_ino_t *out)
{
    uint8_t cmd[CMD_MAX];
    uint32_t clen = 0, epoch;
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
    rc = sim_sess_ensure_id(sim, uuid, epoch, efs_kv_inode_shard(parent));
    if (rc != EFS_OK)
        return rc;
    rc = pack_create(cmd, &clen, has_op, op, uuid, epoch, parent, mode, name);
    if (rc != EFS_OK)
        return rc;
    rc = mutate(sim, cmd, clen);
    if (out)
        *out = (rc == EFS_OK) ? sim->last_ino : 0;
    return rc;
}

int sim_raft_unlink(struct efs_sim *sim, int client, efs_ino_t parent,
                    const char *name)
{
    uint8_t cmd[CMD_MAX];
    uint32_t clen = 0;
    const uint8_t *uuid;
    uint32_t epoch;
    int rc;

    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    uuid = sim->cli[client].win.client_uuid;
    epoch = sim->cli[client].win.session_epoch;
    rc = sim_sess_ensure_id(sim, uuid, epoch, efs_kv_inode_shard(parent));
    if (rc != EFS_OK)
        return rc;
    rc = pack_unlink(cmd, &clen, uuid, epoch, parent, name);
    if (rc != EFS_OK)
        return rc;
    return mutate(sim, cmd, clen);
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
    rc = pack_publish(cmd, &clen, uuid, epoch, p);
    if (rc != EFS_OK)
        return rc;
    return mutate(sim, cmd, clen);
}

int sim_raft_epoch_fence(struct efs_sim *sim, efs_ino_t ino)
{
    uint8_t cmd[9];

    if (!sim || ino == 0)
        return EFS_ERR_INVAL;
    cmd[0] = CMD_EPOCH;
    wr64(cmd + 1, ino);
    return mutate(sim, cmd, 9);
}

int sim_raft_lookup(struct efs_sim *sim, efs_ino_t parent, const char *name,
                    struct efs_meta_dentry *out)
{
    return sim_txn_lookup(sim, parent, name, out);
}

int sim_raft_get_chunk(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                       struct efs_meta_chunk *out)
{
    struct efs_kv *kv;
    int rc;

    rc = read_begin(sim);
    if (rc != EFS_OK)
        return rc;
    kv = leader_kv(sim);
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
