#ifndef EFS_META_APPLY_APPEND_H
#define EFS_META_APPLY_APPEND_H

#include "apply_internal.h"

/* append.c: APPEND reservations, capture and execute, completion and
 * catch-up, the per-session drain, and lazy truncate.
 *
 * Owns no process state. The append cursor (struct append_cur, one per
 * inode generation) and reservations are KV rows; an inode intent freezes
 * their creation and resolution (rows.c write check).
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   struct append_cur, load_append_cur
 *                      the cursor as GETATTR and publish read it; an
 *                      inode with none reads as a zeroed cursor (EFS_OK).
 *   trunc_pending_key  the key of a lazy truncate's pending record on the
 *                      inode's anchor shard (tsweep.c retires it). Pure. */

struct append_cur {
    uint64_t watermark;
    uint64_t frontier;
    uint32_t nopen;
};

int load_append_cur(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                    struct append_cur *c);
int trunc_pending_key(efs_ino_t ino, uint8_t *key, uint32_t *kl);

#endif
