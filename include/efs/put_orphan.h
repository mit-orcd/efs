#ifndef EFS_PUT_ORPHAN_H
#define EFS_PUT_ORPHAN_H
#include "efs/put_ticket.h"

/* D31 collection of abandoned ticketed uploads (K13). The group leader walks
 * its orphan ledger; for each record it proves revocation, has each member
 * holding a fragment delete it (checksum-guarded, durable), records the
 * acknowledgements in the group's log and retires the record only with every
 * fragment acknowledged. A body may be deleted only when ALL hold:
 *   1. the ticket is retired REJECTED with its orphan record: it can never
 *      be admitted, published or settled again (the replay floor);
 *   2. the client's session completed global revocation past the ticket's
 *      epoch (the session group, read fresh by the coordinator);
 *   3. the lane shard's durable rejection floor is above that epoch
 *      (rechecked by the acknowledgement's apply);
 *   4. the member holding the fragment sealed its storage session floor
 *      above that epoch, so no guarded PUT of the epoch can land again,
 *      and the fragment still carries the ticket's checksum.
 * A member that is down or refuses keeps the record; nothing is ever
 * declared collected by age, absence or a quorum. */
struct efs_put_orphan_ops {
    void *ctx;
    /* Fresh authoritative session-group read: OK only when the global
     * revocation barrier for `epoch` has completed. */
    int (*revoked)(void *ctx, const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
    /* Ask nodes[fi] to delete fragment fi. OK only when that member durably
     * holds no copy of it. */
    int (*delete_fragment)(void *ctx, const struct efs_put_ticket *, unsigned fi);
    /* Commit an acknowledgement for `frags` through the group's log and
     * wait for its local apply (EFS_MD_CMD_PUT_ORPHAN_ACK). */
    int (*ack)(void *ctx, const struct efs_put_ticket *, uint32_t frags);
};
struct efs_put_orphan_stats {
    uint64_t waiting;   /* records whose revocation is not proven yet */
    uint64_t deleted;   /* fragments a member confirmed deleted */
    uint64_t failures;  /* member refusals/unreachable and failed ACKs */
    uint64_t collected; /* records retired */
};
/* One record, as scanned (`acked` from the record). `skip` (optional) is a
 * mask of member ids (bit id-1) not to contact this pass. OK when the
 * record is now retired; BUSY when it remains; other errors as returned.
 * Idempotent under retries, lost replies and leader changes: the record's
 * acknowledged mask is the only progress state. */
int efs_put_orphan_collect(struct efs_kv *, const struct efs_put_ticket *, uint32_t acked,
                           const struct efs_put_orphan_ops *, uint64_t *skip,
                           struct efs_put_orphan_stats *);

/* D31 revoked-stream drain, the group leader's half. A client that died (or
 * was fenced) before SETTLE leaves ADMITTED, REJECTED or PUBLISHED tickets in
 * its streams. The leader walks the anchored stream index; for a stream whose
 * owner's revocation it proved, it commits DRAIN commands
 * (efs_put_ticket_drain) that retire the stream into the orphan ledger, where
 * efs_put_orphan_collect reclaims the bodies. */
struct efs_put_drain_ops {
    void *ctx;
    /* As efs_put_orphan_ops.revoked: the completed global barrier. */
    int (*revoked)(void *ctx, const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
    /* Commit EFS_MD_CMD_PUT_TICKET_DRAIN for the stream through its group's
     * log, wait for its local apply and return the apply verdict. */
    int (*drain)(void *ctx, const struct efs_put_stream *);
};
struct efs_put_drain_stats {
    uint64_t waiting;  /* streams whose revocation is not proven yet */
    uint64_t blocked;  /* DRAIN refused: K14 stage pending (or floor) */
    uint64_t commands; /* DRAIN commands committed */
    uint64_t drained;  /* streams emptied */
};
/* One indexed stream: revocation (fresh, then this replica's shard floor),
 * then at most `budget` DRAIN commands while the stream stays indexed and
 * each one makes progress. OK when the stream is empty; BUSY while it stays
 * (not revoked, a stage pending, or the budget spent); other errors as
 * returned. Progress lives in the log only: a new leader resumes from the
 * index, and a DRAIN committed twice is a no-op the second time. */
int efs_put_drain_stream(struct efs_kv *, const struct efs_put_stream *,
                         const struct efs_put_drain_ops *, uint32_t budget,
                         struct efs_put_drain_stats *);
#endif
