/* Directory layout-epoch Raft cmds (§10 step 10 / I8). */
#include "sim_internal.h"
#include "efs/dir_layout.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include <string.h>
#include <sys/stat.h>

#define DIR_BEGIN  1
#define DIR_MIGRATE 2
#define DIR_FINISH 3

static void wr64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

int sim_dir_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                  uint32_t clen, uint64_t index)
{
    efs_ino_t dir = 0;
    int rc = EFS_ERR_PROTO;

    if (!s || !s->disk || !cmd || clen < 10 || cmd[0] != SIM_CMD_DIR)
        goto done;
    dir = rd64(cmd + 2);
    switch (cmd[1]) {
    case DIR_BEGIN:
        rc = efs_meta_dir_begin_split(s->disk, dir);
        break;
    case DIR_MIGRATE:
        rc = efs_meta_dir_migrate_one(s->disk, dir);
        break;
    case DIR_FINISH:
        rc = efs_meta_dir_finish_hashed(s->disk, dir);
        break;
    default:
        break;
    }
done:
    sim_note_apply(s, group, index, rc, dir);
    return EFS_OK;
}

static int propose_dir(struct efs_sim *sim, uint8_t kind, efs_ino_t dir)
{
    uint8_t cmd[10];
    uint32_t shard;

    cmd[0] = SIM_CMD_DIR;
    cmd[1] = kind;
    wr64(cmd + 2, dir);
    shard = efs_kv_inode_shard(dir);
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, 10);
}

int efs_sim_dir_begin_split(struct efs_sim *sim, efs_ino_t dir)
{
    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    return propose_dir(sim, DIR_BEGIN, dir);
}

int efs_sim_dir_migrate(struct efs_sim *sim, efs_ino_t dir)
{
    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    return propose_dir(sim, DIR_MIGRATE, dir);
}

int efs_sim_dir_finish_hashed(struct efs_sim *sim, efs_ino_t dir)
{
    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    return propose_dir(sim, DIR_FINISH, dir);
}

int efs_sim_dir_layout(struct efs_sim *sim, efs_ino_t dir, uint8_t *layout,
                       uint64_t *epoch)
{
    struct efs_meta_row row;
    struct efs_kv *kv;
    uint32_t shard;
    int rc;

    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    shard = efs_kv_inode_shard(dir);
    rc = sim_raft_read_group(sim, sim_shard_group(shard));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (layout)
        *layout = row.layout;
    if (epoch)
        *epoch = row.layout_epoch;
    return EFS_OK;
}
