#ifndef EFS_SIM_INTERNAL_H
#define EFS_SIM_INTERNAL_H

#include "efs/sim.h"
#include "efs/raft.h"
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/raft_disk.h"
#include "efs/transport.h"
#include "efs/opid.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/lock.h"

/* Shared sim layout. Not a public header. */

#define SIM_MAX_EV 128
#define SIM_CMD_SESSION 9 /* matches EFS_MD_CMD_SESSION */
#define SIM_CMD_EPOCH   10
#define SIM_CMD_DIR     11 /* matches EFS_MD_CMD_DIR */
#define SIM_CMD_LOCK    12 /* matches EFS_MD_CMD_LOCK */
#define SIM_CMD_SETATTR 13
#define SIM_CMD_UTIMENS 14
#define SIM_CMD_TRUNCATE 15
#define SIM_CMD_APPEND_RSV 16
#define SIM_CMD_APPEND_RES 17
#define SIM_CMD_MKFS      18
#define SIM_LOCKQ       16

enum {
    EV_CREATE = 1,
    EV_LOOKUP,
    EV_UNLINK,
    EV_PUT_FRAG,
    EV_PUBLISH,
    EV_RAFT
};

struct sim_ev {
    uint64_t tick;
    uint32_t seq;
    uint8_t kind;
    uint8_t client;
    efs_ino_t parent;
    efs_ino_t ino;
    uint32_t mode;
    uint32_t chunk_index;
    uint32_t frag_index;
    uint32_t plen;
    uint64_t new_size;
    char name[EFS_MAX_NAME];
    uint8_t *payload;
    uint8_t has_op;
    struct efs_opid op;
    uint64_t inode_generation;
    uint64_t chunk_generation;
    uint64_t expected_gen;
    uint64_t content_epoch;
    uint32_t retry;
    uint8_t cas_explicit;
};

struct sim_lock_wait {
    uint8_t used;
    uint8_t client;
    uint64_t seq;
    int last_rc;
    struct efs_lock_req req;
};

struct sim_server {
    int alive;
    int partitioned;
    struct efs_store *store;
    struct efs_kv *disk;
    struct efs_transport *rx[EFS_SIM_MAX_CLIENTS];
    struct efs_raft *raft;
    struct efs_raft_store *raft_store;
    struct efs_raft *raft2;
    struct efs_raft_store *raft2_store;
    struct efs_raft *ctrl;
    struct efs_raft_store *ctrl_store;
    /* Non-NULL when the groups' persistent state is the durable on-disk log
     * (EFS_SIM_RAFT_DIR); all groups on this server share it. */
    struct efs_raft_disk *raft_disk;
    struct efs_sim *sim;
    uint64_t boot_id;
    uint64_t applied_idx;
    int applied_rc;
    efs_ino_t applied_ino;
    uint64_t applied_extra;
    uint64_t applied_idx_g[3];
    int applied_rc_g[3];
    efs_ino_t applied_ino_g[3];
    uint64_t applied_extra_g[3];
};

struct sim_client {
    struct efs_transport *tx[EFS_SIM_MAX_SERVERS];
    struct efs_opid_window win;
    uint64_t next_seq;
};

struct efs_sim {
    uint64_t rng;
    uint64_t now;
    uint64_t history;
    uint32_t seq;
    uint32_t delay_max;
    uint32_t drop_per_mille;
    int nservers;
    int nclients;
    int nraft;
    uint32_t desired_voters;
    /* Non-NULL when the servers' persistent state lives in the durable
     * implementations (EFS_SIM_KV_DIR / EFS_SIM_RAFT_DIR) rather than the
     * in-memory ones; inst keeps each sim instance on its own subtree. */
    const char *kv_dir;
    const char *raft_dir;
    uint32_t inst;
    int hold;
    int last_rc;
    efs_ino_t last_ino;
    uint64_t last_extra;
    uint32_t nev;
    struct sim_ev ev[SIM_MAX_EV];
    struct sim_server srv[EFS_SIM_MAX_SERVERS];
    struct sim_client cli[EFS_SIM_MAX_CLIENTS];
    struct efs_txid txn_id;
    struct efs_txn_parts txn_parts;
    int txn_live;
    uint64_t export_salt;
    uint64_t lockq_seq;
    struct sim_lock_wait lockq[SIM_LOCKQ];
};

int sim_enqueue(struct efs_sim *sim, struct sim_ev *in);

struct efs_raft *sim_raft_of(struct efs_sim *sim, int server, uint8_t group);
int sim_raft_propose_group(struct efs_sim *sim, uint8_t group,
                           const uint8_t *cmd, uint32_t clen);
int sim_raft_read_group(struct efs_sim *sim, uint8_t group);
struct efs_kv *sim_raft_kv_group(struct efs_sim *sim, uint8_t group);

/* sim_disk.c: picks the in-memory or the durable implementation of a
 * server's applied KV and of its Raft persistent state. */
void sim_disk_select(struct efs_sim *sim);
struct efs_kv *sim_disk_open(struct efs_sim *sim, int server);
void sim_disk_free(struct efs_sim *sim, struct efs_kv *kv);
struct efs_raft_store *sim_raft_store_new(struct efs_sim *sim, int server,
                                          uint8_t group);
void sim_raft_store_del(struct efs_sim *sim, struct efs_raft_store *st);
void sim_raft_disk_close(struct efs_sim *sim, int server);
/* Makes the applied KV durable through what has been applied, which is what
 * a Raft snapshot must not run ahead of. */
int sim_disk_checkpoint(struct efs_sim *sim, int server);

int sim_raft_mkfs(struct efs_sim *sim);
int sim_raft_export_salt(struct efs_sim *sim, uint64_t *out);
int sim_raft_boot(struct efs_sim *sim);
void sim_raft_free_all(struct efs_sim *sim);
void sim_raft_halt(struct efs_sim *sim, int server);
int sim_raft_restart(struct efs_sim *sim, int server);
int sim_raft_create(struct efs_sim *sim, int client, int has_op,
                    const struct efs_opid *op, efs_ino_t parent, uint32_t mode,
                    const char *name, efs_ino_t *out);
int sim_raft_unlink(struct efs_sim *sim, int client, efs_ino_t parent,
                    const char *name);
int sim_raft_publish(struct efs_sim *sim, int client, const struct efs_meta_pub *p);
int sim_raft_epoch_fence(struct efs_sim *sim, efs_ino_t ino);
int sim_raft_lookup(struct efs_sim *sim, efs_ino_t parent, const char *name,
                    struct efs_meta_dentry *out);
int sim_raft_setattr(struct efs_sim *sim, int client, efs_ino_t ino,
                     const struct efs_meta_setattr *sa);
int sim_raft_utimens(struct efs_sim *sim, int client, efs_ino_t ino,
                     const struct efs_meta_utimens *u);
int sim_raft_truncate(struct efs_sim *sim, int client, efs_ino_t ino,
                      uint64_t size, const struct efs_meta_pub *tail);
int sim_raft_append_reserve(struct efs_sim *sim, int client,
                            const struct efs_opid *op, efs_ino_t ino,
                            uint64_t len, uint64_t *off_out);
int sim_raft_append_resolve(struct efs_sim *sim, int client, efs_ino_t ino,
                            uint64_t off, int outcome);
int sim_raft_getattr(struct efs_sim *sim, efs_ino_t ino, struct efs_meta_stat *out);
int sim_raft_readdir(struct efs_sim *sim, efs_ino_t dir,
                     struct efs_meta_dir_cursor *cur, struct efs_meta_dir_ent *out,
                     uint32_t max, uint32_t *n);
int sim_raft_lookup_path(struct efs_sim *sim, efs_ino_t start, const char *path,
                         struct efs_meta_path_hop *hops, uint32_t cap, uint32_t *n);
int sim_raft_get_chunk(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                       struct efs_meta_chunk *out);
int sim_raft_check(struct efs_sim *sim);
int sim_raft_deliver(struct efs_sim *sim, struct sim_ev *e);
int sim_raft_tick_reachable(struct efs_sim *sim);
int sim_raft_send(void *net, const struct efs_raft_msg *msg);

int sim_ctrl_boot(struct efs_sim *sim);
void sim_ctrl_halt(struct efs_sim *sim, int server);
int sim_ctrl_restart(struct efs_sim *sim, int server);
void sim_ctrl_free_all(struct efs_sim *sim);
int sim_ctrl_on_tick(struct efs_sim *sim, int server);

int sim_txn_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                  uint32_t clen, uint64_t index);
int sim_sess_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                   uint32_t clen, uint64_t index);
int sim_sess_boot(struct efs_sim *sim);
int sim_sess_ensure_id(struct efs_sim *sim, const uint8_t *uuid, uint32_t epoch,
                       uint32_t shard);
int sim_sess_ensure(struct efs_sim *sim, int client, uint32_t shard);
int sim_txn_boot(struct efs_sim *sim);
void sim_txn_halt(struct efs_sim *sim, int server);
int sim_txn_restart(struct efs_sim *sim, int server);
void sim_txn_free_all(struct efs_sim *sim);
int sim_txn_tick(struct efs_sim *sim, int server);
int sim_txn_mkdir_until(struct efs_sim *sim, int client, efs_ino_t parent,
                        const char *name, struct efs_txid *txid, efs_ino_t *out,
                        int until);
int sim_txn_finish(struct efs_sim *sim, const struct efs_txid *t, int commit);
int sim_txn_lookup(struct efs_sim *sim, efs_ino_t parent, const char *name,
                   struct efs_meta_dentry *out);
int sim_txn_coord(void *user, const struct efs_txid *t, uint32_t coord_shard,
                  int *dec);

int sim_txn_read_kv(struct efs_sim *sim, uint32_t shard, struct efs_kv **kv);
void sim_txn_fill_txid(struct efs_sim *sim, struct efs_txid *t);
int sim_txn_parts_add(struct efs_txn_parts *p, uint32_t shard);
int sim_txn_propose_prep(struct efs_sim *sim, uint32_t shard, int kind,
                         const struct efs_txid *t, const struct efs_txn_parts *p,
                         const uint8_t *key, uint32_t klen, uint64_t expected,
                         int op, const uint8_t *val, uint32_t vlen,
                         const struct efs_txn_reduce *red);
int sim_txn_propose_decide(struct efs_sim *sim, uint32_t coord,
                           const struct efs_txid *t, int dec);
int sim_txn_propose_resolve(struct efs_sim *sim, uint32_t shard,
                            const struct efs_txid *t, int dec);
int sim_txn_propose_drop(struct efs_sim *sim, uint32_t shard,
                         const struct efs_txid *t);
int sim_txn_run_until(struct efs_sim *sim, int client, int until);
int sim_txn_link_until(struct efs_sim *sim, int client, efs_ino_t src_parent,
                       const char *src_name, efs_ino_t dst_parent,
                       const char *dst_name, struct efs_txid *txid, int until);
int sim_txn_unlink_until(struct efs_sim *sim, int client, efs_ino_t parent,
                         const char *name, struct efs_txid *txid, int until);
int sim_txn_rmdir_until(struct efs_sim *sim, int client, efs_ino_t parent,
                        const char *name, struct efs_txid *txid, int until);
int sim_txn_rename_until(struct efs_sim *sim, int client, efs_ino_t src_parent,
                         const char *src_name, efs_ino_t dst_parent,
                         const char *dst_name, struct efs_txid *txid, int until);

int sim_ns_try(struct sim_server *s, uint8_t group, const uint8_t *cmd,
               uint32_t clen, uint64_t index);
int sim_dir_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                  uint32_t clen, uint64_t index);
/* Drain leftovers + FINISH if `before` was LOCAL and dir is now SPLITTING. */
void sim_dir_maybe_drain(struct efs_sim *sim, efs_ino_t dir, uint8_t before);
int sim_lock_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                   uint32_t clen, uint64_t index);
int sim_lock_fence(struct efs_sim *sim, const uint8_t *uuid, uint32_t epoch);
int sim_lock_wake(struct efs_sim *sim);

static inline uint8_t sim_shard_group(uint32_t shard)
{
    return efs_raft_shard_group(shard);
}

static inline void sim_note_apply(struct sim_server *s, uint8_t group,
                                  uint64_t index, int rc, efs_ino_t ino)
{
    s->applied_idx = index;
    s->applied_rc = rc;
    s->applied_ino = ino;
    s->applied_extra = 0;
    if (group <= 2) {
        s->applied_idx_g[group] = index;
        s->applied_rc_g[group] = rc;
        s->applied_ino_g[group] = ino;
        s->applied_extra_g[group] = 0;
    }
}

#endif
