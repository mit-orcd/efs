#ifndef EFS_META_APPLY_PUB_STAGE_H
#define EFS_META_APPLY_PUB_STAGE_H

#include "apply_internal.h"

/* pub_stage.c: staged publication (W77, K14): stage and pin records,
 * their visibility, and the transaction resolve and retire that apply or
 * drop them.
 *
 * Owns no process state; a stage owns its chunk key in the KV until its
 * transaction resolves.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   pub_stage_owner    the transaction that staged a publication on this
 *                      chunk key; EFS_ERR_NOT_FOUND when none. Called by the
 *                      write check in rows.c, which answers EFS_ERR_BUSY to
 *                      any other writer of that chunk. */

int pub_stage_owner(struct efs_kv *kv, const uint8_t *chunk_key,
                    uint32_t kl, struct efs_txid *t,
                    struct efs_txn_parts *parts);

#endif
