#ifndef EFS_META_APPLY_ATTR_H
#define EFS_META_APPLY_ATTR_H

#include "apply_internal.h"
#include "rows.h"

/* attr.c: GETATTR, the content-epoch fence and UTIMENS.
 *
 * Owns no state.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   lane_seq_get       a lane's sequence number; an absent lane is seq 0, a
 *                      real value.
 *   fence_lane_bits    push a per-lane update across a lane bitmap into a
 *                      caller batch; a lane with no record yet is skipped
 *                      (its first write stamps itself), and `upd` returning
 *                      0 leaves a record untouched (replay). */

int lane_seq_get(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                 uint8_t lane, uint64_t *seq);
int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino, uint64_t generation,
                    uint64_t bits,
                    int (*upd)(struct lane_rec *ln, void *arg),
                    void *arg, struct efs_kv_item *it, uint32_t *n,
                    uint8_t k_ln[][EFS_KV_KEY_MAX],
                    uint8_t v_ln[][LANE_VAL]);

#endif
