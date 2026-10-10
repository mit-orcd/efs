#ifndef EFS_BATCH_OBS_H
#define EFS_BATCH_OBS_H
#include <stdint.h>

/* Opt-in diagnostics only: never used for admission, ACK or persistence. */
enum efs_batch_counter {
    BO_SYNC, BO_SYNC_ERROR, BO_SYNC_DURABLE_SKIP, BO_SYNC_HOLD_SKIP,
    BO_HOLD_OPEN, BO_HOLD_CLOSE, BO_ROTATE,
    BO_ELIGIBLE, BO_NO_STORE, BO_ROLE, BO_TYPE, BO_ROUTE, BO_TERM,
    BO_EMPTY, BO_ENTRY_LIMIT, BO_LOG_READ, BO_TAIL, BO_CONFIG, BO_ADMIT, BO_BYTES,
    BO_FOLLOW_END, BO_FOLLOW_INELIGIBLE, BO_FOLLOW_BYTES, BO_FOLLOW_ERROR,
    BO_HOST_END, BO_HOST_GROUP, BO_HOST_COUNT, BO_HOST_BYTES, BO_INBOX_EMPTY,
    BO_SPIN_CAP, BO_TAIL_BEHIND, BO_TAIL_AHEAD, BO_TAIL_TERM, BO_INBOX_DROP,
    BO_AE_INITIAL, BO_AE_EXTEND, BO_AE_TIMEOUT, BO_AE_OTHER,
    BO_REPLY_PARTIAL, BO_REPLY_COVERED, BO_DROP_DATA, BO_DROP_EMPTY,
    BO_DROP_OTHER, BO_ACCEPT_DATA, BO_ACCEPT_EMPTY,
    BO_WIRE_SENT, BO_FLOW_BUSY, BO_FLOW_ADMITTED, BO_FLOW_LEGACY,
    BO_FLOW_WAIT, BO_FLOW_CONTROL, BO_FLOW_PROBE, BO_FLOW_ENQUEUE_RETRY,
    BO_FLOW_PREBUILD_RETRY, BO_BATCH_NOOP,
    BO_OVERLAP_DUP, BO_OVERLAP_SUFFIX, BO_OVERLAP_CONFLICT, BO_OVERLAP_LOG,
    BO_OVERLAP_CFG, BO_OVERLAP_IDLE, BO_SUFFIX_ACTIVE, BO_SUFFIX_READ,
    BO_SUFFIX_READY, BO_END_DUP, BO_END_SUFFIX, BO_END_CONFLICT,
    BO_END_LOG, BO_END_CFG, BO_BATCH_OVERLAP, BO_OVERLAP_ADMIT,
    BO_OVERLAP_BYTES, BO_OVERLAP_READ_BOUNDARY, BO_FLOW_RETIRE_EARLY, BO_COUNTER_N
};
enum efs_batch_hist {
    BH_WAL_ENTRIES, BH_WAL_BYTES, BH_SYNC_US, BH_SYNC_WAIT_US,
    BH_SYNC_WAITERS, BH_HOLD_US, BH_HOLD_DEPTH, BH_PROPOSERS,
    BH_INBOX, BH_HOST_MESSAGES, BH_FOLLOW_MESSAGES, BH_FOLLOW_ENTRIES,
    BH_FOLLOW_ASSEMBLY_US, BH_FOLLOW_TOTAL_US, BH_SPINS,
    BH_INBOX_AGE_US, BH_PUMP_LOCK_US, BH_PUMP_DRAIN_US,
    BH_KV_RELEASE_US, BH_APPLIED_PERSIST_US, BH_HIST_N
};
extern int efs_batch_obs_on;
void efs_batch_obs_init(void);
uint64_t efs_batch_obs_now_us(void);
void efs_batch_obs_count(enum efs_batch_counter key);
void efs_batch_obs_reject(enum efs_batch_counter key);
void efs_batch_obs_command(uint8_t opcode);
void efs_batch_obs_hist(enum efs_batch_hist key, uint64_t value);
void efs_batch_obs_dump(uint64_t now_us);
#define BO_ON() __atomic_load_n(&efs_batch_obs_on, __ATOMIC_RELAXED)
#define BO_COUNT(k) do { if (BO_ON()) efs_batch_obs_count(k); } while (0)
#define BO_HIST(k,v) do { if (BO_ON()) efs_batch_obs_hist(k,(v)); } while (0)
#endif
