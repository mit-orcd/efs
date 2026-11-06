#ifndef EFS_META_APPLY_XATTR_H
#define EFS_META_APPLY_XATTR_H

#include "apply_internal.h"

/* xattr.c: extended attributes (one blob per inode).
 *
 * Owns no state.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   xattr_del_item     fill one DEL when the inode has a blob; a miss adds
 *                      nothing (*added 0), so an unlink never plants a
 *                      tombstone for a key the file never had. Used by
 *                      unlink, rmdir, reclaim and the reaper. */

int xattr_del_item(struct efs_kv *kv, efs_ino_t ino, uint8_t *k,
                   uint32_t *kl, struct efs_kv_item *it, int *added);

#endif
