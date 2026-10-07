#ifndef EFS_PUBLICATION_ACK_H
#define EFS_PUBLICATION_ACK_H
#include "efs/publication.h"
/* Staged per-stream retirement ownership. Caller serializes access and admits
 * EVERY intent in sequence order before submission. An older local intent may
 * not be hidden outside this queue. Never release accepted bytes using an ACK.
 * Allocate this object under the client metadata budget. Unknown outcomes and
 * unconsumed terminal results block later retirement. No network I/O here. */
struct efs_publication_ack_entry {
    struct efs_msg_publication request;
    uint8_t digest[EFS_HASH_SIZE];
    uint32_t consumed_state;
    int32_t consumed_verdict;
};
struct efs_publication_ack_queue {
    struct efs_publication_ack_entry entries[EFS_META_PUBLICATION_MAX_RECEIPTS];
    uint64_t last_sequence;
    uint32_t head, count;
    struct efs_msg_publication stream;
    int bound;
};
/* Caller-budgeted, zero-initialized storage. No reset while receipts are owned. */
int efs_publication_ack_admit(struct efs_publication_ack_queue *,
                             const struct efs_msg_publication *);
/* Call only AFTER cache ownership consumed an exact terminal result. */
int efs_publication_ack_consumed(struct efs_publication_ack_queue *,
    uint64_t sequence, const struct efs_msg_publication_reply *);
int efs_publication_ack_next(const struct efs_publication_ack_queue *,
                             struct efs_msg_publication *);
/* Only an exact RETIRED reply removes the oldest consumed intent. */
int efs_publication_ack_complete(struct efs_publication_ack_queue *,
    uint64_t sequence, const struct efs_msg_publication_reply *);
#endif
