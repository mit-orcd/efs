/* What a simulated server's persistent storage actually is.
 *
 * By default: in-memory KV, in-memory Raft store, so a sim run touches no
 * filesystem. Set EFS_SIM_KV_DIR and/or EFS_SIM_RAFT_DIR and the same cases
 * run against the durable implementations instead, which is how the
 * invariants get checked against the stores efsd will use rather than against
 * idealized maps. Nothing else in the simulator knows the difference.
 *
 * With EFS_SIM_RAFT_DIR, all of a server's Raft groups (shard, shard2, ctrl)
 * share ONE log — the production shape, where thousands of groups per node
 * multiplex a single fsync stream. That sharing is itself under test. */

#include "sim_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *dir_env(const char *name)
{
    const char *v = getenv(name);

    return (v && v[0]) ? v : NULL;
}

void sim_disk_select(struct efs_sim *sim)
{
    static uint32_t inst_next;

    sim->kv_dir = dir_env("EFS_SIM_KV_DIR");
    sim->raft_dir = dir_env("EFS_SIM_RAFT_DIR");
    /* Each sim gets its own subtree: the in-memory stores hand every instance
     * an empty one, so a reused directory would carry state across cases and
     * the second case would start dirty. */
    sim->inst = __atomic_add_fetch(&inst_next, 1, __ATOMIC_RELAXED);
}

/* --- applied-state KV ------------------------------------------------ */

/* A durable backend that fails to open makes a sim case fail somewhere
 * downstream, with nothing on screen saying the store never opened. Say so. */
static void report_open_failure(const char *what, const char *dir)
{
    fprintf(stderr, "sim: %s store failed to open at %s: %s\n", what, dir,
            strerror(errno));
}

struct efs_kv *sim_disk_open(struct efs_sim *sim, int server)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    char dir[512];

    if (!sim->kv_dir)
        return efs_kv_mem_create();
    /* Thresholds are small on purpose: the sim's volume is low, and one it
     * never crosses would check every invariant against a memtable and
     * never against a segment or a compaction result. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.memtable_max = 512;
    cfg.l0_max = 2;
    snprintf(dir, sizeof(dir), "%s/sim%u/srv%d", sim->kv_dir, sim->inst,
             server);
    kv = efs_kv_lsm_open(dir, &cfg);
    if (!kv)
        report_open_failure("KV", dir);
    return kv;
}

void sim_disk_free(struct efs_sim *sim, struct efs_kv *kv)
{
    if (sim->kv_dir)
        efs_kv_lsm_close(kv);
    else
        efs_kv_mem_free(kv);
}

/* --- Raft persistent state ------------------------------------------- */

struct efs_raft_store *sim_raft_store_new(struct efs_sim *sim, int server,
                                          uint8_t group)
{
    char dir[512];

    if (!sim->raft_dir)
        return efs_raft_mem_create();
    if (server < 0 || server >= EFS_SIM_MAX_SERVERS)
        return NULL;
    if (!sim->srv[server].raft_disk) {
        snprintf(dir, sizeof(dir), "%s/sim%u/srv%d", sim->raft_dir, sim->inst,
                 server);
        sim->srv[server].raft_disk =
            efs_raft_disk_open(dir, EFS_RAFT_DISK_NOSYNC);
        if (!sim->srv[server].raft_disk) {
            report_open_failure("Raft", dir);
            return NULL;
        }
    }
    return efs_raft_disk_group(sim->srv[server].raft_disk, group);
}

/* A group store is owned by the server's log, so releasing one is a no-op
 * there; the log itself is closed once, by sim_raft_disk_close. */
void sim_raft_store_del(struct efs_sim *sim, struct efs_raft_store *st)
{
    if (!sim->raft_dir)
        efs_raft_mem_free(st);
}

void sim_raft_disk_close(struct efs_sim *sim, int server)
{
    if (server < 0 || server >= EFS_SIM_MAX_SERVERS)
        return;
    efs_raft_disk_close(sim->srv[server].raft_disk);
    sim->srv[server].raft_disk = NULL;
}
