#ifndef EFS_SESSION_REVOKE_H
#define EFS_SESSION_REVOKE_H
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include "efs/protocol.h"

/* Coordinator of the I23 revocation barrier for one session (10.5c-35c):
 * BEGIN freezes touched[], every storage member seals its certificate,
 * FENCE_LOC + ACK every touched shard, FINISH at the captured epoch, then
 * bounded lease/append cleanup on each touched shard. Two callers drive it:
 * the operator (`efs-mgmt raft-session fence`) and a mount revoking its own
 * session at clean unmount (D31: the abandoned bodies of a departed client
 * become collectable without an operator). Only the transport differs. */
/* Upper bound on concurrent per-shard steps of one revoke. */
#define EFS_SESSION_REVOKE_PARALLEL_MAX 32u
struct efs_session_rpc {
    void *ctx;
    /* One EFS_MSG_RAFT_MKFS submit; payload[0] is the Raft group. Follows
     * NOT_PRIMARY redirects; retries BUSY only where
     * efs_session_busy_retry_ok allows. Returns out->rc. */
    int (*submit)(void *ctx, const uint8_t *payload, uint32_t plen,
                  struct efs_msg_raft_mkfs_reply *out);
    /* Concurrent per-shard steps (FENCE_LOC+ACK, then cleanup), capped by
     * EFS_SESSION_REVOKE_PARALLEL_MAX. 0 or 1 runs every step in the
     * caller's thread; above that, submit must be thread-safe. */
    unsigned parallel;
    /* Optional: called after each shard of a stage ("fence", "cleanup")
     * completes, from any worker thread. */
    void (*progress)(void *ctx, const char *stage, uint32_t done, uint32_t total);
};
/* Fresh session reads and exact-floor fences can be BUSY while a cold
 * participant catches up; these requests are idempotent with unchanged
 * bytes. BEGIN and cleanup selection keep their one definite result. */
static inline int efs_session_busy_retry_ok(const uint8_t *payload, uint32_t plen)
{
    return plen >= 3 && payload[1] == EFS_MD_CMD_SESSION &&
           ((payload[2] == 0 && plen == 23) ||
            (payload[2] == EFS_MD_SESS_FENCE_LOC && plen == 1 + EFS_MD_SESS_FENCE_LOC_LEN) ||
            (payload[2] == EFS_MD_SESS_STORAGE_FENCE && plen == 23));
}
/* A cleanup step's BUSY that the server certified as a no-op (zero
 * index/term): LEASE_LOCK_NEXT's refused selection or (K42) its batch
 * applied with BUSY and no effect, or SESSION_APPEND_NEXT's refused
 * selection and K42 waits (another session's OPEN append on the inode, the
 * frontier, an aborted PREPARE, REAP owed). Any other BUSY may follow a
 * submitted transaction and stays the step's one result. */
static inline int efs_session_cleanup_retry_ok(const struct efs_msg_raft_mkfs_reply *r,
                                               int appends)
{
    if (r->index || r->term)
        return 0;
    return appends ? r->salt >= EFS_MD_SESSION_APPEND_RETRY_SALT &&
                     r->salt <= EFS_MD_SESSION_APPEND_WAIT_GONE
                   : r->salt == EFS_MD_SESSION_CLEANUP_RETRY_SALT ||
                     r->salt == EFS_MD_SESSION_CLEANUP_WAIT_LOCKED;
}
/* The server's reason for a cleanup step that failed with rc ("" if none):
 * what a client logs with the shard where its revoke stopped. */
static inline const char *efs_session_cleanup_reason(uint64_t salt, int rc)
{
    if (rc != EFS_ERR_BUSY)
        return "";
    switch (salt) {
    case EFS_MD_SESSION_CLEANUP_RETRY_SALT: return "lease-selection-refused";
    case EFS_MD_SESSION_CLEANUP_WAIT_LOCKED: return "lease-key-locked";
    case EFS_MD_SESSION_APPEND_RETRY_SALT: return "append-selection-refused";
    case EFS_MD_SESSION_APPEND_WAIT_OPEN: return "append-open-by-other-session";
    case EFS_MD_SESSION_APPEND_WAIT_FRONTIER: return "append-frontier";
    case EFS_MD_SESSION_APPEND_WAIT_ABORTED: return "append-prepare-aborted";
    case EFS_MD_SESSION_APPEND_WAIT_GONE: return "append-reap-owed";
    default: return "uncertified-busy";
    }
}
/* Per-shard cleanup budget of efs_session_cleanup_bounded. */
#define EFS_SESSION_CLEANUP_BUDGET_MS 20000u
/* Repeat one certificate barrier (STORAGE_FENCE, FENCE_LOC, FINISH) while
 * BUSY, within 15 s and any caller deadline. */
int efs_session_storage_barrier(const struct efs_session_rpc *, const uint8_t *payload,
                                uint32_t plen, struct efs_msg_raft_mkfs_reply *out);
/* Exact lease/lock (appends=0) or append (appends=1) cleanup of one shard
 * for old_epoch, eight keys per RPC, within EFS_SESSION_CLEANUP_BUDGET_MS.
 * OK once the phase is empty; a certified BUSY is repeated every 50 ms. */
int efs_session_cleanup_bounded(const struct efs_session_rpc *,
                                const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t old_epoch,
                                uint32_t shard, int appends,
                                struct efs_msg_raft_mkfs_reply *reply);
struct efs_session_revoke_result {
    uint32_t epoch, state, ntouch; /* the owner's view at the end */
    uint32_t old_epoch, shard;     /* the revoked epoch; shard where cleanup stopped */
    const char *phase;             /* where an incomplete barrier stopped */
    struct efs_msg_raft_mkfs_reply last;
    uint64_t fence_ms, cleanup_ms, total_ms; /* wall time of the stages */
    uint32_t cleaned;      /* shards the cleanup probe left to per-shard cleanup */
    uint32_t probe_failed; /* probes that failed (their whole word was cleaned) */
    const char *reason;    /* cleanup stop: efs_session_cleanup_reason, or "" */
};
/* The whole barrier. OK only when the owner reads ACTIVE at the new epoch
 * after FINISH and every cleanup phase; any other result leaves the barrier
 * resumable by a later fence (it never reports a partial one as done). */
int efs_session_revoke(const struct efs_session_rpc *, const uint8_t uuid[EFS_OPID_UUID_LEN],
                       struct efs_session_revoke_result *);
#endif
