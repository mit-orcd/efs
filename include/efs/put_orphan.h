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
#endif
