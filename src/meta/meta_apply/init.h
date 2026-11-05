#ifndef EFS_META_APPLY_INIT_H
#define EFS_META_APPLY_INIT_H

#include "apply_internal.h"

/* init.c: inode allocation, opid records, mkfs/init, salt and membership,
 * and the public pack/unpack wrappers.
 *
 * Owns the ALLOC key protocol: the apply is the allocator. A hint is honoured
 * only while unallocated, otherwise the next free inode is taken; every
 * log-path claim bumps the key's version in the same batch so an outdated
 * PREPARE is STALE, and a claim under a live intent is EFS_ERR_BUSY.
 * Deterministic on every replica (same log, same shard state).
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   alloc_next_free, alloc_hint_or_next, alloc_key_claim
 *                      used by create.c; they fill batch items and write
 *                      nothing themselves.
 *   opid_hosted, opid_item
 *                      the operation-identity window on the dentry shard:
 *                      opid_item fills the item recording a reply (1),
 *                      returns 0 when there is nothing to record (no
 *                      identity, or the window is full: the op still
 *                      applies), negative on a KV error. */

int alloc_next_free(struct efs_kv *kv, uint32_t shard, efs_ino_t *ino,
                    efs_ino_t *next_out);
int alloc_hint_or_next(struct efs_kv *kv, uint32_t shard,
                       efs_ino_t want, efs_ino_t *ino,
                       efs_ino_t *next_out);
int alloc_key_claim(struct efs_kv *kv, const uint8_t *k_alloc,
                    uint32_t ka, uint8_t *k_ver, uint32_t *kver,
                    uint8_t v_ver[8]);
int opid_hosted(const struct efs_opid *op);
int opid_item(struct efs_kv *kv, uint32_t shard,
              const struct efs_opid_req *q, const struct efs_opid_reply *rep,
              uint8_t *key, uint8_t *val, struct efs_kv_item *it);

#endif
