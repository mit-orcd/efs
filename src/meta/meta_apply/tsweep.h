#ifndef EFS_META_APPLY_TSWEEP_H
#define EFS_META_APPLY_TSWEEP_H

#include "apply_internal.h"

/* tsweep.c: the truncate sweep, swept lanes, sweep done and retire.
 *
 * Owns no state.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   sweep_visible      the visible length of a part under a fence history
 *                      (pack.c checks a candidate with it). Pure. */

uint32_t sweep_visible(const struct efs_fence_history *s, uint64_t epoch,
                       uint64_t c, uint32_t off, uint32_t len);

#endif
