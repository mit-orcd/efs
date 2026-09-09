/* Control-plane Raft group: desired placement, not shard membership.
 * architecture.md §7.8 / §10 step 6. Desired ≠ actual; the metadata
 * group converges via joint consensus (I18, L8). */
#include "sim_internal.h"
#include <stdlib.h>
#include <string.h>

#define CMD_DESIRED 4
#define CTRL_WAIT   200
#define CONV_WAIT   400

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int ctrl_apply(void *app, uint64_t index, uint64_t term,
                      const uint8_t *cmd, uint32_t clen)
{
    struct sim_server *s = app;

    (void)index;
    (void)term;
    if (!s || !s->sim || !cmd || clen < 5)
        return EFS_OK;
    if (cmd[0] == CMD_DESIRED)
        s->sim->desired_voters = rd32(cmd + 1);
    return EFS_OK;
}

static int attach_ctrl(struct efs_sim *sim, int i)
{
    struct efs_raft_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = i;
    cfg.n = EFS_SIM_RAFT_N;
    cfg.voters = (1u << EFS_SIM_RAFT_N) - 1;
    cfg.boot_id = sim->srv[i].boot_id ? sim->srv[i].boot_id : 1;
    cfg.group = EFS_RAFT_GROUP_CTRL;
    cfg.election_ticks = (uint32_t)(4 + i * 4); /* staggered; raft.c adds jitter */
    cfg.heartbeat_ticks = 1;
    cfg.store = sim->srv[i].ctrl_store;
    cfg.store_ctx = sim->srv[i].ctrl_store;
    cfg.send = sim_raft_send;
    cfg.net = sim;
    cfg.apply = ctrl_apply;
    cfg.app = &sim->srv[i];
    sim->srv[i].sim = sim;
    sim->srv[i].ctrl = efs_raft_new(&cfg);
    return sim->srv[i].ctrl ? EFS_OK : EFS_ERR_NOMEM;
}

static int ctrl_leader(const struct efs_sim *sim)
{
    int i, lid = -1, n = 0;

    for (i = 0; i < EFS_SIM_RAFT_N; i++) {
        if (!sim->srv[i].alive || sim->srv[i].partitioned || !sim->srv[i].ctrl)
            continue;
        if (efs_raft_role(sim->srv[i].ctrl) == EFS_RAFT_LEADER) {
            lid = i;
            n++;
        }
    }
    return n == 1 ? lid : -1;
}

int sim_ctrl_boot(struct efs_sim *sim)
{
    int i, t, rc;

    if (!sim)
        return EFS_ERR_INVAL;
    for (i = 0; i < EFS_SIM_RAFT_N; i++) {
        sim->srv[i].ctrl_store =
            sim_raft_store_new(sim, i, EFS_RAFT_GROUP_CTRL);
        if (!sim->srv[i].ctrl_store)
            return EFS_ERR_NOMEM;
        rc = attach_ctrl(sim, i);
        if (rc != EFS_OK)
            return rc;
    }
    for (t = 0; t < CTRL_WAIT; t++) {
        if (ctrl_leader(sim) >= 0)
            return EFS_OK;
        for (i = 0; i < EFS_SIM_RAFT_N; i++) {
            if (sim->srv[i].ctrl)
                efs_raft_tick(sim->srv[i].ctrl);
        }
    }
    return EFS_OK;
}

void sim_ctrl_halt(struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= EFS_SIM_RAFT_N)
        return;
    efs_raft_free(sim->srv[server].ctrl);
    sim->srv[server].ctrl = NULL;
}

int sim_ctrl_restart(struct efs_sim *sim, int server)
{
    if (!sim || server < 0)
        return EFS_ERR_INVAL;
    if (server >= EFS_SIM_RAFT_N)
        return EFS_OK;
    if (sim->srv[server].ctrl)
        return EFS_OK;
    if (!sim->srv[server].ctrl_store)
        return EFS_ERR_INVAL;
    return attach_ctrl(sim, server);
}

void sim_ctrl_free_all(struct efs_sim *sim)
{
    int i;

    if (!sim)
        return;
    for (i = 0; i < EFS_SIM_MAX_SERVERS; i++) {
        efs_raft_free(sim->srv[i].ctrl);
        sim->srv[i].ctrl = NULL;
        sim_raft_store_del(sim, sim->srv[i].ctrl_store);
        sim->srv[i].ctrl_store = NULL;
    }
}

int sim_ctrl_on_tick(struct efs_sim *sim, int server)
{
    int rc;

    if (!sim || server < 0 || server >= sim->nraft)
        return EFS_OK;
    if (sim->srv[server].ctrl) {
        rc = efs_raft_tick(sim->srv[server].ctrl);
        if (rc != EFS_OK)
            return rc;
    }
    if (!sim->srv[server].raft ||
        efs_raft_role(sim->srv[server].raft) != EFS_RAFT_LEADER)
        return EFS_OK;
    if (efs_raft_joint(sim->srv[server].raft))
        return EFS_OK;
    if (efs_raft_voters(sim->srv[server].raft) == sim->desired_voters)
        return EFS_OK;
    rc = efs_raft_change(sim->srv[server].raft, sim->desired_voters);
    if (rc == EFS_ERR_BUSY || rc == EFS_ERR_NOT_PRIMARY)
        return EFS_OK;
    return rc;
}

uint32_t efs_sim_ctrl_desired(const struct efs_sim *sim)
{
    return sim ? sim->desired_voters : 0;
}

int efs_sim_ctrl_set_desired(struct efs_sim *sim, uint32_t voters)
{
    uint8_t cmd[5];
    uint64_t idx = 0;
    int lid, t, rc;

    if (!sim)
        return EFS_ERR_INVAL;
    cmd[0] = CMD_DESIRED;
    wr32(cmd + 1, voters);
    lid = -1;
    for (t = 0; t < CTRL_WAIT; t++) {
        lid = ctrl_leader(sim);
        if (lid >= 0)
            break;
        rc = sim_raft_tick_reachable(sim);
        if (rc != EFS_OK)
            return rc;
    }
    if (lid < 0)
        return EFS_ERR_BUSY;
    rc = efs_raft_propose(sim->srv[lid].ctrl, cmd, 5, &idx);
    if (rc != EFS_OK)
        return rc;
    for (t = 0; t < CTRL_WAIT; t++) {
        if (efs_raft_role(sim->srv[lid].ctrl) != EFS_RAFT_LEADER)
            return EFS_ERR_BUSY;
        if (efs_raft_applied(sim->srv[lid].ctrl) >= idx)
            break;
        rc = sim_raft_tick_reachable(sim);
        if (rc != EFS_OK)
            return rc;
    }
    if (efs_raft_applied(sim->srv[lid].ctrl) < idx)
        return EFS_ERR_BUSY;
    for (t = 0; t < CONV_WAIT; t++) {
        int ml = efs_sim_meta_leader(sim);
        if (ml >= 0 && !efs_sim_meta_joint(sim, ml) &&
            efs_sim_meta_voters(sim, ml) == sim->desired_voters)
            return EFS_OK;
        rc = sim_raft_tick_reachable(sim);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_ERR_BUSY;
}
