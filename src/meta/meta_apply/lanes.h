#ifndef EFS_META_APPLY_LANES_H
#define EFS_META_APPLY_LANES_H

#include "apply_internal.h"
#include "rows.h"

/* lanes.c: lane activation, content fences and resize, lane bootstrap,
 * and the writer views a client plans against.
 *
 * Owns no state.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   resize_parts_add   add a participant shard to a transaction's part set
 *                      once; EFS_ERR_INVAL when EFS_TXN_MAX_PART are taken.
 *   resize_lane_read   read one lane record for a resize/mtime capture;
 *                      an absent lane is not an error (*len 0). */

int resize_parts_add(struct efs_txn_parts *parts, uint32_t shard);
int resize_lane_read(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                      uint8_t lane, efs_txn_coord_fn coord, void *ctx,
                      struct efs_txn_pending *pend,
                      uint8_t val[LANE_VAL], uint32_t *len,
                      struct lane_rec *stamp);

#endif
