#ifndef EFS_SIM_H
#define EFS_SIM_H

#include "efs/common.h"
#include "efs/store.h"
#include "efs/opid.h"

/* Deterministic simulator (architecture.md §10 steps 1–5).
 * N logical servers + M clients in one process. Seeded PRNG drives
 * message order, drops, delays, crashes, and clock steps. Metadata is
 * a fixed RF=3 Raft group (servers 0..2) applying the §5 KV SM. Data
 * plane is mem store + loop transport — not sockets or NVMe.
 *
 * Logical chunk size is EFS_SIM_CHUNK (encode takes chunk_size). */

#define EFS_SIM_MAX_SERVERS 8
#define EFS_SIM_MAX_CLIENTS 4
#define EFS_SIM_CHUNK       EFS_MIN_CHUNK_SIZE /* encode floor */
#define EFS_SIM_RAFT_N      3 /* RF = 2f+1, f=1 */
#define EFS_SIM_META        0 /* replica 0 of the metadata Raft group */

struct efs_sim;

struct efs_sim_cfg {
    uint64_t seed;
    int nservers;           /* >= 3 for 2+1 placement */
    int nclients;
    uint32_t delay_max;     /* extra ticks; 0 = same-tick FIFO */
    uint32_t drop_per_mille; /* 0..1000 chance a scheduled msg is dropped */
};

struct efs_sim *efs_sim_new(const struct efs_sim_cfg *cfg);
void efs_sim_free(struct efs_sim *sim);

uint64_t efs_sim_rng(struct efs_sim *sim);
uint64_t efs_sim_now(const struct efs_sim *sim);
uint64_t efs_sim_history(const struct efs_sim *sim);

/* Hold=1 queues events without applying; hold=0 drains. */
void efs_sim_hold(struct efs_sim *sim, int hold);
int efs_sim_drain(struct efs_sim *sim);
int efs_sim_check(struct efs_sim *sim);

/* Namespace ops: propose to the metadata Raft leader, apply on each replica. */
int efs_sim_create(struct efs_sim *sim, int client, efs_ino_t parent,
                   uint32_t mode, const char *name, efs_ino_t *out);
/* I16: same op identity does not mint a second inode. */
int efs_sim_create_op(struct efs_sim *sim, int client, const struct efs_opid *op,
                      efs_ino_t parent, uint32_t mode, const char *name,
                      efs_ino_t *out);
void efs_sim_opid_for(struct efs_sim *sim, int client, uint64_t seq,
                      struct efs_opid *out);
int efs_sim_opid_ack(struct efs_sim *sim, int client, uint64_t contiguous_ack);
/* Drop the client's RAM window; durable KV is the source of truth (I16). */
int efs_sim_opid_forget(struct efs_sim *sim, int client);
int efs_sim_lookup(struct efs_sim *sim, int client, efs_ino_t parent,
                   const char *name, efs_ino_t *out);
int efs_sim_unlink(struct efs_sim *sim, int client, efs_ino_t parent,
                   const char *name);

/* Logical data protocol. skip_frag -1 = PUT all k+f; else omit that index.
 * publish is one event (chunk map + size) and refuses unless I14 holds.
 * read_chunk reconstructs only a *published* generation; skips corrupt
 * fragments (I25) and never serves unpublished orphans (I15). */
int efs_sim_put_stripe(struct efs_sim *sim, int client, efs_ino_t ino,
                       uint32_t chunk_index, const uint8_t *chunk,
                       uint32_t chunk_len, int skip_frag);
int efs_sim_publish(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                    uint64_t new_size);
int efs_sim_read_chunk(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                       uint8_t *out, uint32_t chunk_len);
int efs_sim_frag_present(struct efs_sim *sim, const struct efs_frag_id *id);

int efs_sim_crash(struct efs_sim *sim, int server); /* RAM gone, disk kept */
int efs_sim_restart(struct efs_sim *sim, int server);
int efs_sim_corrupt(struct efs_sim *sim, int server, const struct efs_frag_id *id);
int efs_sim_partition(struct efs_sim *sim, int server, int on);
int efs_sim_clock_step(struct efs_sim *sim, uint64_t delta);

/* Observability for I1–I4. Leader is -1 if none among reachable replicas.
 * tick: server >= 0 ticks that replica; -1 ticks every reachable replica. */
int efs_sim_meta_leader(const struct efs_sim *sim);
int efs_sim_meta_role(const struct efs_sim *sim, int server);
uint64_t efs_sim_meta_term(const struct efs_sim *sim, int server);
uint64_t efs_sim_meta_commit(const struct efs_sim *sim, int server);
int efs_sim_meta_tick(struct efs_sim *sim, int server);

#endif
