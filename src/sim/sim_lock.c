/* POSIX lock Raft cmds + in-memory wait queue (§10 step 10 / §7.6). */
#include "sim_internal.h"
#include "efs/lock.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include <string.h>

#define LOCK_GRANT   EFS_MD_LOCK_GRANT
#define LOCK_RELEASE EFS_MD_LOCK_RELEASE
#define CMD_MAX      80

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

static int pack_lock(uint8_t *out, uint32_t *len, uint8_t kind,
                     const struct efs_lock_req *r)
{
    uint8_t *p;

    out[0] = EFS_MD_CMD_LOCK;
    out[1] = kind;
    wr64(out + 2, r->ino);
    wr64(out + 10, r->generation);
    out[18] = r->domain;
    out[19] = r->type;
    wr64(out + 20, r->start);
    wr64(out + 28, r->end);
    out[36] = r->owner.kind;
    wr64(out + 37, r->owner.id);
    p = out + 45;
    memcpy(p, r->owner.uuid, EFS_OPID_UUID_LEN);
    wr32(p + EFS_OPID_UUID_LEN, r->owner.epoch);
    *len = 45 + EFS_OPID_UUID_LEN + 4;
    return EFS_OK;
}

static int unpack_lock(const uint8_t *cmd, uint32_t clen, struct efs_lock_req *r)
{
    const uint8_t *p;

    if (clen < 45 + EFS_OPID_UUID_LEN + 4)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->ino = rd64(cmd + 2);
    r->generation = rd64(cmd + 10);
    r->domain = cmd[18];
    r->type = cmd[19];
    r->start = rd64(cmd + 20);
    r->end = rd64(cmd + 28);
    r->owner.kind = cmd[36];
    r->owner.id = rd64(cmd + 37);
    p = cmd + 45;
    memcpy(r->owner.uuid, p, EFS_OPID_UUID_LEN);
    r->owner.epoch = rd32(p + EFS_OPID_UUID_LEN);
    return EFS_OK;
}

int sim_lock_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                   uint32_t clen, uint64_t index)
{
    struct efs_lock_req r;
    int rc = EFS_ERR_PROTO;

    memset(&r, 0, sizeof(r));
    if (!s || !s->disk || !cmd || clen < 2 || cmd[0] != EFS_MD_CMD_LOCK)
        goto done;
    rc = unpack_lock(cmd, clen, &r);
    if (rc != EFS_OK)
        goto done;
    if (cmd[1] == LOCK_GRANT)
        rc = efs_lock_grant(s->disk, &r);
    else if (cmd[1] == LOCK_RELEASE)
        rc = efs_lock_release(s->disk, &r);
    else
        rc = EFS_ERR_PROTO;
done:
    sim_note_apply(s, group, index, rc, r.ino);
    return EFS_OK;
}

static int fill_req(struct efs_sim *sim, int client, efs_ino_t ino,
                    uint8_t domain, uint8_t type, uint64_t start, uint64_t end,
                    uint8_t owner_kind, uint64_t owner_id, struct efs_lock_req *r)
{
    struct efs_meta_row row;
    struct efs_kv *kv;
    uint32_t shard;
    int rc;

    memset(r, 0, sizeof(*r));
    shard = efs_kv_inode_shard(ino);
    rc = sim_raft_read_group(sim, sim_shard_group(shard));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    r->ino = ino;
    r->generation = row.generation;
    r->domain = domain;
    r->type = type;
    r->start = start;
    r->end = end;
    memcpy(r->owner.uuid, sim->cli[client].win.client_uuid, EFS_OPID_UUID_LEN);
    r->owner.epoch = sim->cli[client].win.session_epoch;
    r->owner.kind = owner_kind;
    r->owner.id = owner_id;
    return EFS_OK;
}

static int propose_lock(struct efs_sim *sim, int client, uint8_t kind,
                        const struct efs_lock_req *r)
{
    uint8_t cmd[CMD_MAX];
    uint32_t clen = 0, shard;
    int rc;

    rc = sim_sess_ensure_id(sim, r->owner.uuid, r->owner.epoch,
                            efs_kv_inode_shard(r->ino));
    if (rc != EFS_OK)
        return rc;
    (void)client;
    rc = pack_lock(cmd, &clen, kind, r);
    if (rc != EFS_OK)
        return rc;
    shard = efs_kv_inode_shard(r->ino);
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, clen);
}

static int owner_eq(const struct efs_lock_owner *a, const struct efs_lock_owner *b)
{
    return a->kind == b->kind && a->id == b->id && a->epoch == b->epoch &&
           memcmp(a->uuid, b->uuid, EFS_OPID_UUID_LEN) == 0;
}

static int overlap(const struct efs_lock_req *a, const struct efs_lock_req *b)
{
    return a->domain == b->domain && a->start < b->end && b->start < a->end;
}

static int deadlock(struct efs_sim *sim, struct efs_kv *kv,
                    const struct efs_lock_req *req)
{
    struct efs_lock_owner by;
    int i, rc, hit;

    rc = efs_lock_blocked(kv, req, &by);
    if (rc <= 0)
        return rc;
    for (i = 0; i < SIM_LOCKQ; i++) {
        if (!sim->lockq[i].used)
            continue;
        if (!owner_eq(&sim->lockq[i].req.owner, &by))
            continue;
        hit = efs_lock_owner_blocks(kv, req->ino, req->generation, req->domain,
                                    &req->owner, sim->lockq[i].req.start,
                                    sim->lockq[i].req.end,
                                    sim->lockq[i].req.type);
        if (hit < 0)
            return hit;
        if (hit)
            return 1;
    }
    return 0;
}

static int enqueue(struct efs_sim *sim, int client, const struct efs_lock_req *r)
{
    int i;

    for (i = 0; i < SIM_LOCKQ; i++) {
        if (sim->lockq[i].used)
            continue;
        sim->lockq[i].used = 1;
        sim->lockq[i].client = (uint8_t)client;
        sim->lockq[i].seq = ++sim->lockq_seq;
        sim->lockq[i].last_rc = EFS_ERR_BUSY;
        sim->lockq[i].req = *r;
        return EFS_OK;
    }
    return EFS_ERR_NOLCK;
}

int sim_lock_fence(struct efs_sim *sim, const uint8_t *uuid, uint32_t epoch)
{
    int i;

    if (!sim || !uuid)
        return EFS_ERR_INVAL;
    for (i = 0; i < SIM_LOCKQ; i++) {
        if (!sim->lockq[i].used)
            continue;
        if (sim->lockq[i].req.owner.epoch != epoch)
            continue;
        if (memcmp(sim->lockq[i].req.owner.uuid, uuid, EFS_OPID_UUID_LEN) != 0)
            continue;
        sim->lockq[i].used = 0;
        sim->lockq[i].last_rc = EFS_ERR_STALE;
    }
    return EFS_OK;
}

int sim_lock_wake(struct efs_sim *sim)
{
    struct efs_kv *kv;
    uint32_t shard;
    int i, j, best, rc, blk;
    uint64_t minseq;
    uint8_t skip[SIM_LOCKQ];

    if (!sim)
        return EFS_ERR_INVAL;
    memset(skip, 0, sizeof(skip));
    for (;;) {
        best = -1;
        minseq = ~(uint64_t)0;
        for (i = 0; i < SIM_LOCKQ; i++) {
            if (!sim->lockq[i].used || skip[i])
                continue;
            if (sim->lockq[i].seq < minseq) {
                minseq = sim->lockq[i].seq;
                best = i;
            }
        }
        if (best < 0)
            return EFS_OK;
        shard = efs_kv_inode_shard(sim->lockq[best].req.ino);
        rc = sim_raft_read_group(sim, sim_shard_group(shard));
        if (rc != EFS_OK)
            return rc;
        kv = sim_raft_kv_group(sim, sim_shard_group(shard));
        if (!kv)
            return EFS_ERR_BUSY;
        blk = efs_lock_blocked(kv, &sim->lockq[best].req, NULL);
        if (blk < 0)
            return blk;
        if (!blk && sim->lockq[best].req.type == EFS_LOCK_SH) {
            for (j = 0; j < SIM_LOCKQ; j++) {
                if (!sim->lockq[j].used || j == best)
                    continue;
                if (sim->lockq[j].seq > sim->lockq[best].seq)
                    continue;
                if (sim->lockq[j].req.type == EFS_LOCK_EX &&
                    overlap(&sim->lockq[j].req, &sim->lockq[best].req)) {
                    blk = 1;
                    break;
                }
            }
        }
        if (blk) {
            skip[best] = 1;
            continue;
        }
        rc = propose_lock(sim, sim->lockq[best].client, LOCK_GRANT,
                          &sim->lockq[best].req);
        if (rc == EFS_OK) {
            sim->lockq[best].used = 0;
            sim->lockq[best].last_rc = EFS_OK;
            memset(skip, 0, sizeof(skip));
            continue;
        }
        if (rc == EFS_ERR_AGAIN) {
            skip[best] = 1;
            continue;
        }
        sim->lockq[best].used = 0;
        sim->lockq[best].last_rc = rc;
        memset(skip, 0, sizeof(skip));
    }
}

static int do_lock(struct efs_sim *sim, int client, efs_ino_t ino,
                   uint8_t domain, uint8_t type, uint64_t start, uint64_t end,
                   uint8_t owner_kind, uint64_t owner_id, int wait)
{
    struct efs_lock_req r;
    struct efs_kv *kv;
    uint32_t shard;
    int rc, dl;

    if (!sim || client < 0 || client >= sim->nclients || ino == 0)
        return EFS_ERR_INVAL;
    rc = fill_req(sim, client, ino, domain, type, start, end, owner_kind,
                  owner_id, &r);
    if (rc != EFS_OK)
        return rc;
    shard = efs_kv_inode_shard(ino);
    kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_lock_blocked(kv, &r, NULL);
    if (rc < 0)
        return rc;
    if (rc == 1) {
        if (!wait)
            return EFS_ERR_AGAIN;
        dl = deadlock(sim, kv, &r);
        if (dl < 0)
            return dl;
        if (dl)
            return EFS_ERR_DEADLK;
        rc = enqueue(sim, client, &r);
        if (rc != EFS_OK)
            return rc;
        return EFS_ERR_BUSY;
    }
    return propose_lock(sim, client, LOCK_GRANT, &r);
}

int efs_sim_lock(struct efs_sim *sim, int client, efs_ino_t ino, uint8_t domain,
                 uint8_t type, uint64_t start, uint64_t end, uint8_t owner_kind,
                 uint64_t owner_id)
{
    return do_lock(sim, client, ino, domain, type, start, end, owner_kind,
                   owner_id, 0);
}

int efs_sim_lockw(struct efs_sim *sim, int client, efs_ino_t ino, uint8_t domain,
                  uint8_t type, uint64_t start, uint64_t end, uint8_t owner_kind,
                  uint64_t owner_id)
{
    return do_lock(sim, client, ino, domain, type, start, end, owner_kind,
                   owner_id, 1);
}

int efs_sim_unlock(struct efs_sim *sim, int client, efs_ino_t ino, uint8_t domain,
                   uint64_t start, uint64_t end, uint8_t owner_kind,
                   uint64_t owner_id)
{
    struct efs_lock_req r;
    int rc;

    if (!sim || client < 0 || client >= sim->nclients || ino == 0)
        return EFS_ERR_INVAL;
    rc = fill_req(sim, client, ino, domain, EFS_LOCK_EX, start, end, owner_kind,
                  owner_id, &r);
    if (rc != EFS_OK)
        return rc;
    rc = propose_lock(sim, client, LOCK_RELEASE, &r);
    if (rc != EFS_OK)
        return rc;
    return sim_lock_wake(sim);
}

int efs_sim_lock_wake(struct efs_sim *sim)
{
    return sim_lock_wake(sim);
}
