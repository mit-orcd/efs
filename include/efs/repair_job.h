#ifndef EFS_REPAIR_JOB_H
#define EFS_REPAIR_JOB_H
#include "efs/meta_apply.h"

/* Inactive prerequisite: no RPC, worker or data write is enabled by this
 * journal. Call mutations ONLY from serialized, committed Raft apply on the
 * owning lane group. Capture needs current lane read authority; inode/export
 * authority and actual durable target receipts are the coordinator's job.
 * Floor records are retained: no unsafe receipt pruning is provided here. */
#define EFS_REPAIR_JOB_BYTES 260u
#define EFS_REPAIR_COMMAND_BYTES (2u + EFS_REPAIR_JOB_BYTES + 4u)
#define EFS_REPAIR_OP_ADMIT 1u
#define EFS_REPAIR_OP_DATA 2u
#define EFS_REPAIR_OP_CANCEL 3u
#define EFS_REPAIR_OP_SEAL 4u
#define EFS_REPAIR_OP_RETIRE 5u
int efs_repair_apply_command_term(struct efs_kv *,uint32_t group,uint64_t term,const uint8_t *,uint32_t);
int efs_repair_apply_command(struct efs_kv *, uint32_t group, const uint8_t *, uint32_t);
#define EFS_REPAIR_PENDING_MAX 128u
#define EFS_REPAIR_ADMITTED 1u
#define EFS_REPAIR_CANCELLING 2u
#define EFS_REPAIR_SEALED 3u
#define EFS_REPAIR_DATA_DURABLE 1u
#define EFS_REPAIR_CANCELLED 2u
struct efs_repair_job {
    uint8_t uuid[16], target_store[16];
    efs_ino_t ino;
    uint64_t inode_gen, object;
    uint32_t ci, fragment_len, span_off, span_len, epoch, owner;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE], source[EFS_HASH_SIZE];
    uint8_t fi, state, flags;
};
int efs_repair_job_encode(const struct efs_repair_job *, uint8_t out[EFS_REPAIR_JOB_BYTES]);
int efs_repair_job_decode(const uint8_t *, uint32_t, struct efs_repair_job *);
int efs_repair_capture(struct efs_kv *, const struct efs_repair_job *, struct efs_repair_job *);
int efs_repair_admit(struct efs_kv *, const struct efs_repair_job *);
int efs_repair_get(struct efs_kv *, const struct efs_repair_job *, struct efs_repair_job *);
/* Durable apply records of coordinator-verified receipts. These functions
 * cannot establish target durability or revocation by themselves. */
int efs_repair_write_authority(struct efs_kv *, const struct efs_repair_job *);
int efs_repair_record_data(struct efs_kv *, const struct efs_repair_job *);
int efs_repair_cancel(struct efs_kv *, const struct efs_repair_job *);
int efs_repair_record_seal(struct efs_kv *, const struct efs_repair_job *, uint32_t target_floor);
int efs_repair_retire(struct efs_kv *, const struct efs_repair_job *, int cancelled);
/* Mandatory future GC integration point. BUSY until EVERY matching active
 * job has a committed terminal target seal; absence never proves a target
 * receipt. The object predicate also fences historical FileID generations. */
int efs_repair_gc_ready(struct efs_kv *, efs_ino_t, uint64_t, uint32_t, uint64_t);
int efs_repair_gc_object_ready(struct efs_kv *, efs_ino_t, uint32_t, uint64_t);
#define EFS_REPAIR_RECEIPT_BYTES 132u
#define EFS_REPAIR_RECEIPT_DATA 1u
#define EFS_REPAIR_RECEIPT_SEAL 2u
int efs_repair_receipt_encode(const struct efs_repair_job *,unsigned kind,uint8_t out[EFS_REPAIR_RECEIPT_BYTES]);
int efs_repair_receipt_verify(const struct efs_repair_job *,unsigned kind,const uint8_t *,uint32_t);
int efs_repair_seal_authority(struct efs_kv *,const struct efs_repair_job *);
#endif
