#ifndef EFS_SIM_INTERNAL_H
#define EFS_SIM_INTERNAL_H

#include "efs/sim.h"
#include "efs/raft.h"
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/transport.h"
#include "efs/opid.h"

/* Shared sim layout. Not a public header. */

#define SIM_MAX_EV 128

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
};

struct sim_server {
    int alive;
    int partitioned;
    struct efs_store *store;
    struct efs_kv *disk;
    struct efs_transport *rx[EFS_SIM_MAX_CLIENTS];
    struct efs_raft *raft;
    struct efs_raft_store *raft_store;
    struct efs_raft *ctrl;
    struct efs_raft_store *ctrl_store;
    struct efs_sim *sim;
    uint64_t boot_id;
    uint64_t applied_idx;
    int applied_rc;
    efs_ino_t applied_ino;
};

struct sim_client {
    struct efs_transport *tx[EFS_SIM_MAX_SERVERS];
    struct efs_opid_window win;
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
    int hold;
    int last_rc;
    efs_ino_t last_ino;
    uint32_t nev;
    struct sim_ev ev[SIM_MAX_EV];
    struct sim_server srv[EFS_SIM_MAX_SERVERS];
    struct sim_client cli[EFS_SIM_MAX_CLIENTS];
};

int sim_enqueue(struct efs_sim *sim, struct sim_ev *in);

int sim_raft_boot(struct efs_sim *sim);
void sim_raft_free_all(struct efs_sim *sim);
void sim_raft_halt(struct efs_sim *sim, int server);
int sim_raft_restart(struct efs_sim *sim, int server);
int sim_raft_create(struct efs_sim *sim, int has_op, const struct efs_opid *op,
                    efs_ino_t parent, uint32_t mode, const char *name,
                    efs_ino_t *out);
int sim_raft_unlink(struct efs_sim *sim, efs_ino_t parent, const char *name);
int sim_raft_publish(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                     uint64_t new_size, const struct efs_meta_chunk *ch);
int sim_raft_lookup(struct efs_sim *sim, efs_ino_t parent, const char *name,
                    struct efs_meta_dentry *out);
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

#endif
