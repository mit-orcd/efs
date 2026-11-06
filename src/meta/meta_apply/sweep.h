#ifndef EFS_META_APPLY_SWEEP_H
#define EFS_META_APPLY_SWEEP_H

#include "apply_internal.h"

/* sweep.c: lane sweep, reap selection and batches, and GC ACK.
 *
 * Owns no process state; the GC watermark it lowers is gc.c's.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   struct packed_retirement
 *                      a packed file's one base at chunk zero, captured
 *                      without side effects before the final batch.
 *   packed_retirement_commit
 *                      add the release of that base, with the mapping, to
 *                      the caller's final batch; writes nothing itself. */

/* A packed file owns one base, at chunk zero. Capture its checked image
 * without side effects; the final batch releases that reference together
 * with the mapping, fence and EOF update. */
struct packed_retirement {
    struct efs_meta_chunk before, after;
    uint8_t present, has_after;
};

int packed_retirement_commit(struct efs_kv *kv, efs_ino_t ino,
    uint64_t gen, struct packed_retirement *retirement,
    struct efs_kv_item *it, uint32_t n, uint32_t cap);

#endif
