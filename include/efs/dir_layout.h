#ifndef EFS_DIR_LAYOUT_H
#define EFS_DIR_LAYOUT_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/meta_apply.h"

/* Directory layout-epoch SM (architecture.md §7.4 / §10 step 10).
 * LOCAL → SPLITTING(e) → HASHED(e). Size trigger: a LOCAL dir's nents
 * crossing EFS_DIR_SPREAD_MIN (overridable via the env of the same name)
 * commits SPLITTING on that parent-row PUT. Pressure-triggered spread
 * has no numeric bound in the spec and is not invented here. */

uint32_t efs_dir_spread_min(void);
/* LOCAL only. +1 on create/mkdir/link dest; -1 on unlink/rmdir/rename
 * src. Crossing the size bound flips layout to SPLITTING. */
void efs_meta_dir_note_entry(struct efs_meta_row *row, int delta);

int efs_meta_dir_begin_split(struct efs_kv *kv, efs_ino_t dir);
/* First leftover local name that does not already live on lane 0.
 * NOT_FOUND if none remain (*saw_lane0 is set when a lane-0 name is
 * still in the local range). hsh is the HASHED dentry shard. */
int efs_meta_dir_migrate_peek(struct efs_kv *kv, efs_ino_t dir, char *name,
                              uint32_t nmax, uint32_t *hsh, int *saw_lane0);
int efs_meta_dir_migrate_one(struct efs_kv *kv, efs_ino_t dir);
int efs_meta_dir_finish_hashed(struct efs_kv *kv, efs_ino_t dir);

#endif
