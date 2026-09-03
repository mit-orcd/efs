#ifndef EFS_DIR_LAYOUT_H
#define EFS_DIR_LAYOUT_H

#include "efs/common.h"
#include "efs/kv.h"

/* Directory layout-epoch SM (architecture.md §7.4 / §10 step 10).
 * LOCAL → SPLITTING(e) → HASHED(e). The sim driver calls these; there is
 * no pressure heuristic here. */

int efs_meta_dir_begin_split(struct efs_kv *kv, efs_ino_t dir);
int efs_meta_dir_migrate_one(struct efs_kv *kv, efs_ino_t dir);
int efs_meta_dir_finish_hashed(struct efs_kv *kv, efs_ino_t dir);

#endif
