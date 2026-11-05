#ifndef EFS_META_APPLY_LOOKUP_H
#define EFS_META_APPLY_LOOKUP_H

#include "apply_internal.h"

/* lookup.c: inode and dentry reads, LOOKUP, RESOLVE and path walks.
 *
 * Owns no state.
 * Locks: none of its own. Every mutation runs inside the participant's
 *   serialized Raft apply (or a coordinator-checked transaction step), and
 *   the KV is the only shared state.
 *
 * Exports inside the module
 *   inode_read_tx      the inode row as a handler sees it: a COMMITted
 *                      replacement is visible through `coord`; NULL coord is
 *                      a plain read (apply readers never contact a
 *                      coordinator).
 *   dent_get, dent_get_tx
 *                      one dentry on one shard; _tx sees a COMMITted EXCL
 *                      intent as the value (I17).
 *
 * Failure behaviour: EFS_ERR_NOT_FOUND is only an absent row or name (I9);
 * a coordinator or KV failure keeps its own code. */

int inode_read_tx(struct efs_kv *kv, efs_ino_t ino,
                   efs_txn_coord_fn coord, void *ctx,
                   struct efs_txn_pending *pend,
                   struct efs_meta_row *out);
int dent_get(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
             const char *name, struct efs_meta_dentry *out);
int dent_get_tx(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
                const char *name, efs_txn_coord_fn coord, void *ctx,
                struct efs_meta_dentry *out);

#endif
