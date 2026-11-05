#ifndef EFS_META_APPLY_PUBLISH_H
#define EFS_META_APPLY_PUBLISH_H

#include "apply_internal.h"

/* publish.c: chunk publication. The receipt (admit, result, retire), the
 * publish CAS, the ticketed (K13) and rejected paths.
 *
 * Owns
 *   pub_stale_why      thread-local: which rule STALEd the last publish on
 *                      this thread (efs_meta_apply_publish_stale_why; the
 *                      host logs it beside the rc). No lock.
 *   struct publish_mode
 *                      the ordinary write, a staged PREPARE's dry run, or
 *                      the RESOLVE that applies its own stage (W77).
 *
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   publish_inner_as   the publish CAS: one batch with the chunk, lane,
 *                      inode, GC records, degraded marks, ticket, receipt
 *                      and pack candidate. Every refusal (STALE, BUSY,
 *                      INVAL, PROTO) returns before the batch; the degraded
 *                      count follows only a written, non-dry batch.
 *   publication_receipt_valid
 *                      a stored receipt's length and verdict are well
 *                      formed (check.c). */

/* How a publish writes: the ordinary path, a staged PREPARE's dry run, or
 * the RESOLVE that applies its own stage (W77). */
struct publish_mode {
    int dry;
    const struct efs_txid *self;
};

int publication_receipt_valid(const uint8_t *v,uint32_t n);
int publish_inner_as(struct efs_kv *kv, const struct efs_meta_pub *p,
                     const struct efs_kv_item *receipt,
                     const struct publish_mode *mode);

#endif
