/* Session + lease Raft cmds inside efs_sim (§10 step 8 / I23 / I19). */
#include "sim_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/lock.h"
#include <string.h>

#define CMD_MAX        64

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

static int pack_hdr(uint8_t *out, uint8_t sub, const uint8_t *uuid)
{
    out[0] = EFS_MD_CMD_SESSION;
    out[1] = sub;
    memcpy(out + 2, uuid, EFS_OPID_UUID_LEN);
    return 18;
}

static int propose_sess(struct efs_sim *sim, uint32_t shard, const uint8_t *cmd,
                        uint32_t clen)
{
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, clen);
}

static int read_sess(struct efs_sim *sim, uint32_t shard, struct efs_kv **kv)
{
    int rc = sim_raft_read_group(sim, sim_shard_group(shard));

    if (rc != EFS_OK)
        return rc;
    *kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    return *kv ? EFS_OK : EFS_ERR_BUSY;
}

int sim_sess_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                   uint32_t clen, uint64_t index)
{
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch = 0, shard = 0;
    efs_ino_t ino = 0;
    uint64_t gen = 0;
    int rc = EFS_ERR_PROTO;

    if (!s || !s->disk || !cmd || clen < 18 || cmd[0] != EFS_MD_CMD_SESSION)
        goto done;
    memcpy(uuid, cmd + 2, EFS_OPID_UUID_LEN);
    switch (cmd[1]) {
    case EFS_MD_SESS_CREATE:
        if (clen < 22)
            break;
        epoch = rd32(cmd + 18);
        rc = efs_session_create(s->disk, uuid, epoch);
        break;
    case EFS_MD_SESS_REGISTER:
        if (clen < 26)
            break;
        epoch = rd32(cmd + 18);
        shard = rd32(cmd + 22);
        rc = efs_session_register(s->disk, uuid, epoch, shard);
        break;
    case EFS_MD_SESS_BEGIN:
        rc = efs_session_begin_fence(s->disk, uuid);
        break;
    case EFS_MD_SESS_FENCE_LOC:
        if (clen < 26)
            break;
        epoch = rd32(cmd + 18);
        shard = rd32(cmd + 22);
        rc = efs_session_fence_local(s->disk, shard, uuid, epoch);
        break;
    case EFS_MD_SESS_ACK:
        if (clen < 22)
            break;
        shard = rd32(cmd + 18);
        rc = efs_session_ack_fence(s->disk, uuid, shard);
        break;
    case EFS_MD_SESS_FINISH:
        rc = efs_session_finish_fence(s->disk, uuid);
        break;
    case EFS_MD_SESS_ESTABLISH:
        if (clen < 26)
            break;
        epoch = rd32(cmd + 18);
        shard = rd32(cmd + 22);
        rc = efs_session_establish(s->disk, shard, uuid, epoch);
        break;
    case EFS_MD_SESS_LEASE_OPEN:
        if (clen < 38)
            break;
        epoch = rd32(cmd + 18);
        ino = rd64(cmd + 22);
        gen = rd64(cmd + 30);
        rc = efs_lease_open(s->disk, ino, gen, uuid, epoch);
        break;
    case EFS_MD_SESS_LEASE_CLOSE:
        if (clen < 38)
            break;
        epoch = rd32(cmd + 18);
        ino = rd64(cmd + 22);
        gen = rd64(cmd + 30);
        rc = efs_lease_close(s->disk, ino, gen, uuid, epoch);
        /* Mirror of raft_host apply: last close drops the inode's locks
         * (the kernel never relays a flock UNLOCK on close) and drains
         * orphaned append reservations as holes (§7.3 ABORTED_HOLE). */
        if (rc == EFS_OK && efs_lease_any(s->disk, ino, gen) == 0) {
            int lr = efs_lock_drop_file(s->disk, ino, gen);
            int dr = efs_meta_apply_append_drain_file(s->disk, ino);

            if (lr != EFS_OK && lr != EFS_ERR_NOT_FOUND)
                rc = lr;
            if (dr != EFS_OK && dr != EFS_ERR_NOT_FOUND && rc == EFS_OK)
                rc = dr;
        }
        break;
    case EFS_MD_SESS_LEASE_DROP:
        if (clen < 26)
            break;
        epoch = rd32(cmd + 18);
        shard = rd32(cmd + 22);
        rc = efs_lease_drop_session(s->disk, shard, uuid, epoch);
        if (rc == EFS_OK || rc == EFS_ERR_NOT_FOUND) {
            int r2 = efs_lock_drop_session(s->disk, shard, uuid, epoch);
            int r3 = efs_meta_apply_append_drop_session(s->disk, shard, uuid,
                                                        epoch);

            if (r2 != EFS_OK && r2 != EFS_ERR_NOT_FOUND)
                rc = r2;
            else if (r3 != EFS_OK && r3 != EFS_ERR_NOT_FOUND)
                rc = r3;
            else if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
        }
        if (rc == EFS_OK && s->sim)
            sim_lock_fence(s->sim, uuid, epoch);
        break;
    case EFS_MD_SESS_RECLAIM:
        if (clen < 26)
            break;
        ino = rd64(cmd + 18);
        rc = efs_meta_apply_reclaim(s->disk, ino);
        break;
    default:
        rc = EFS_ERR_PROTO;
        break;
    }
done:
    sim_note_apply(s, group, index, rc, ino);
    return EFS_OK;
}

int sim_sess_boot(struct efs_sim *sim)
{
    int i, c, rc;

    if (!sim)
        return EFS_ERR_INVAL;
    /* Bootstrap like the root inode: same committed record on every replica
     * KV. Later mutations go through Raft. Avoids depending on a live leader
     * (the always-drop fault config has none). */
    for (c = 0; c < sim->nclients; c++) {
        const uint8_t *uuid = sim->cli[c].win.client_uuid;
        uint32_t epoch = sim->cli[c].win.session_epoch;

        for (i = 0; i < sim->nservers; i++) {
            if (!sim->srv[i].disk)
                continue;
            rc = efs_session_create(sim->srv[i].disk, uuid, epoch);
            if (rc != EFS_OK)
                return rc;
        }
    }
    return EFS_OK;
}

int sim_sess_ensure_id(struct efs_sim *sim, const uint8_t *uuid, uint32_t epoch,
                       uint32_t shard)
{
    struct efs_kv *kv;
    uint8_t cmd[CMD_MAX];
    uint32_t n, ssh;
    int rc;

    if (!sim || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    rc = read_sess(sim, shard, &kv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_session_accept(kv, shard, uuid, epoch);
    if (rc == EFS_OK || rc == EFS_ERR_STALE)
        return rc;
    ssh = efs_kv_session_shard(uuid);
    n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_REGISTER, uuid);
    wr32(cmd + n, epoch);
    wr32(cmd + n + 4, shard);
    n += 8;
    rc = propose_sess(sim, ssh, cmd, n);
    if (rc != EFS_OK)
        return rc;
    n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_ESTABLISH, uuid);
    wr32(cmd + n, epoch);
    wr32(cmd + n + 4, shard);
    n += 8;
    return propose_sess(sim, shard, cmd, n);
}

int sim_sess_ensure(struct efs_sim *sim, int client, uint32_t shard)
{
    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    return sim_sess_ensure_id(sim, sim->cli[client].win.client_uuid,
                              sim->cli[client].win.session_epoch, shard);
}

static int cli_ids(struct efs_sim *sim, int client, const uint8_t **uuid,
                   uint32_t *epoch)
{
    if (!sim || client < 0 || client >= sim->nclients)
        return EFS_ERR_INVAL;
    *uuid = sim->cli[client].win.client_uuid;
    *epoch = sim->cli[client].win.session_epoch;
    return EFS_OK;
}

int efs_sim_session_fence_until(struct efs_sim *sim, int client, int until)
{
    const uint8_t *uuid;
    uint32_t epoch, ssh, old_epoch, i;
    struct efs_kv *kv;
    struct efs_session_rec rec;
    uint8_t cmd[CMD_MAX];
    uint32_t n;
    int rc;

    rc = cli_ids(sim, client, &uuid, &epoch);
    if (rc != EFS_OK)
        return rc;
    (void)epoch;
    ssh = efs_kv_session_shard(uuid);
    rc = read_sess(sim, ssh, &kv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_session_get(kv, uuid, &rec);
    if (rc != EFS_OK)
        return rc;
    if (rec.state == EFS_SESSION_ACTIVE) {
        n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_BEGIN, uuid);
        rc = propose_sess(sim, ssh, cmd, n);
        if (rc != EFS_OK)
            return rc;
        rc = read_sess(sim, ssh, &kv);
        if (rc != EFS_OK)
            return rc;
        rc = efs_session_get(kv, uuid, &rec);
        if (rc != EFS_OK)
            return rc;
    }
    if (rec.state != EFS_SESSION_FENCING)
        return EFS_ERR_INVAL;
    if (until < EFS_SIM_FENCE_LOCAL)
        return EFS_OK;
    for (i = 0; i < EFS_SESSION_BITS; i++) {
        if (!efs_session_bit_get(rec.touched, i))
            continue;
        n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_FENCE_LOC, uuid);
        wr32(cmd + n, rec.epoch);
        wr32(cmd + n + 4, i);
        n += 8;
        rc = propose_sess(sim, i, cmd, n);
        if (rc != EFS_OK)
            return rc;
    }
    if (until < EFS_SIM_FENCE_ACK)
        return EFS_OK;
    for (i = 0; i < EFS_SESSION_BITS; i++) {
        if (!efs_session_bit_get(rec.touched, i))
            continue;
        n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_ACK, uuid);
        wr32(cmd + n, i);
        n += 4;
        rc = propose_sess(sim, ssh, cmd, n);
        if (rc != EFS_OK)
            return rc;
    }
    if (until < EFS_SIM_FENCE_ACTIVE)
        return EFS_OK;
    n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_FINISH, uuid);
    rc = propose_sess(sim, ssh, cmd, n);
    if (rc != EFS_OK)
        return rc;
    old_epoch = rec.epoch > 0 ? rec.epoch - 1 : 0;
    rc = read_sess(sim, ssh, &kv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_session_barrier_done(kv, uuid, old_epoch);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < EFS_SESSION_BITS; i++) {
        if (!efs_session_bit_get(rec.touched, i))
            continue;
        n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_LEASE_DROP, uuid);
        wr32(cmd + n, old_epoch);
        wr32(cmd + n + 4, i);
        n += 8;
        rc = propose_sess(sim, i, cmd, n);
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_session_get(kv, uuid, &rec);
    if (rc != EFS_OK)
        return rc;
    {
        uint8_t keep[EFS_OPID_UUID_LEN];

        memcpy(keep, uuid, EFS_OPID_UUID_LEN);
        efs_opid_window_init(&sim->cli[client].win, keep, rec.epoch);
    }
    return EFS_OK;
}

int efs_sim_session_fence(struct efs_sim *sim, int client)
{
    return efs_sim_session_fence_until(sim, client, EFS_SIM_FENCE_ACTIVE);
}

static int live_gen(struct efs_sim *sim, efs_ino_t ino, uint64_t *gen,
                    uint32_t *nlink)
{
    struct efs_kv *kv;
    struct efs_meta_row row;
    uint32_t sh = efs_kv_inode_shard(ino);
    int rc;

    rc = read_sess(sim, sh, &kv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (gen)
        *gen = row.generation;
    if (nlink)
        *nlink = row.nlink;
    return EFS_OK;
}

int efs_sim_inode_nlink(struct efs_sim *sim, efs_ino_t ino, uint32_t *nlink,
                        uint64_t *gen)
{
    if (!sim || ino == 0)
        return EFS_ERR_INVAL;
    return live_gen(sim, ino, gen, nlink);
}

int efs_sim_open(struct efs_sim *sim, int client, efs_ino_t ino)
{
    const uint8_t *uuid;
    uint32_t epoch, n, sh;
    uint64_t gen = 0;
    uint8_t cmd[CMD_MAX];
    int rc;

    rc = cli_ids(sim, client, &uuid, &epoch);
    if (rc != EFS_OK)
        return rc;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure_id(sim, uuid, epoch, sh);
    if (rc != EFS_OK)
        return rc;
    rc = live_gen(sim, ino, &gen, NULL);
    if (rc != EFS_OK)
        return rc;
    n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_LEASE_OPEN, uuid);
    wr32(cmd + n, epoch);
    wr64(cmd + n + 4, ino);
    wr64(cmd + n + 12, gen);
    n += 20;
    return propose_sess(sim, sh, cmd, n);
}

int efs_sim_close(struct efs_sim *sim, int client, efs_ino_t ino)
{
    const uint8_t *uuid;
    uint32_t epoch, n, sh;
    uint64_t gen = 0;
    uint8_t cmd[CMD_MAX];
    int rc;

    rc = cli_ids(sim, client, &uuid, &epoch);
    if (rc != EFS_OK)
        return rc;
    sh = efs_kv_inode_shard(ino);
    rc = sim_sess_ensure_id(sim, uuid, epoch, sh);
    if (rc != EFS_OK)
        return rc;
    rc = live_gen(sim, ino, &gen, NULL);
    if (rc != EFS_OK)
        return rc;
    n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_LEASE_CLOSE, uuid);
    wr32(cmd + n, epoch);
    wr64(cmd + n + 4, ino);
    wr64(cmd + n + 12, gen);
    n += 20;
    return propose_sess(sim, sh, cmd, n);
}

int efs_sim_reclaim(struct efs_sim *sim, efs_ino_t ino)
{
    uint8_t cmd[CMD_MAX], uuid[EFS_OPID_UUID_LEN];
    uint32_t n, sh;

    if (!sim || ino == 0)
        return EFS_ERR_INVAL;
    memset(uuid, 0, sizeof(uuid));
    n = (uint32_t)pack_hdr(cmd, EFS_MD_SESS_RECLAIM, uuid);
    wr64(cmd + n, ino);
    n += 8;
    sh = efs_kv_inode_shard(ino);
    return propose_sess(sim, sh, cmd, n);
}
