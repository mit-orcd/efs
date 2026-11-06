#ifndef EFS_META_APPLY_NAMESPACE_H
#define EFS_META_APPLY_NAMESPACE_H

#include "apply_internal.h"

/* namespace.c: UNLINK, expiry, LINK, RENAME and SETATTR.
 *
 * Owns no state.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   dentry_drop_items  drop one name into a caller batch by unlink's LOCAL /
 *                      SPLITTING / HASHED rules (used by rmdir in
 *                      readdir.c); `k_loc`, `k_hash` and `v_tomb` are caller
 *                      storage that must outlive the batch. Writes nothing. */

int dentry_drop_items(struct efs_kv *kv, const struct efs_meta_row *prow,
                      efs_ino_t parent, const char *name,
                      uint8_t *k_loc, uint32_t *kl, uint8_t *k_hash,
                      uint32_t *kh, uint8_t *v_tomb,
                      struct efs_kv_item *it, uint32_t *n);

#endif
