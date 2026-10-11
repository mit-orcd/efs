#ifndef EFS_PACK_COHORT_H
#define EFS_PACK_COHORT_H
#include "efs/pack_admission.h"
/* Client coalescing primitive. Caller serializes access, owns this nonmoving
 * storage, accounts sizeof(*cohort) against its memory budget BEFORE accepting
 * bytes, and pins each captured writer snapshot until terminal reconciliation.
 * No allocation, background thread, network ACK or cache ownership transfer is
 * hidden here. The mounted writer selects it only with EFS_PACKED_WRITES=1. */
#define EFS_PACK_COHORT_RECORDS 128u
struct efs_pack_cohort {
    uint32_t group, limit, count, payload_bytes;
    uint64_t delay_ns, first_ns;
    uint8_t sealed;
    struct efs_opid owner;
    struct efs_pack_record records[EFS_PACK_COHORT_RECORDS];
    uint64_t snapshots[EFS_PACK_COHORT_RECORDS];
    struct efs_pack_ref refs[EFS_PACK_COHORT_RECORDS];
    size_t geometry;
    uint8_t payload[EFS_SEGMENT_MAX_SIZE], image[EFS_SEGMENT_MAX_SIZE];
};
int efs_pack_cohort_init(struct efs_pack_cohort *, uint32_t group,
    uint32_t record_limit, uint64_t delay_ns);
/* Copies the immutable snapshot. BUSY leaves queue and accepted bytes intact;
 * the caller flushes/reconciles this cohort before offering more. A repeated
 * inode is BUSY: unrelated versions never share one cohort slot. */
int efs_pack_cohort_offer(struct efs_pack_cohort *, const struct efs_pack_record *,
    uint64_t snapshot_sequence, uint64_t monotonic_ns);
/* Capacity, deadline, explicit sync and shutdown can all trigger a partial
 * cohort. No caller must wait for the record_limit to fill. */
int efs_pack_cohort_due(const struct efs_pack_cohort *, uint64_t monotonic_ns,
    int force);
/* Retry always returns the SAME immutable body/refs for the exact owner.
 * Never reset this storage on an uncertain PUT or publication outcome. The
 * caller may reinitialize only after every accepted snapshot has reconciled
 * its durable publication or fenced cancellation and retained recovery state. */
int efs_pack_cohort_seal(struct efs_pack_cohort *, const struct efs_opid *);
#endif
