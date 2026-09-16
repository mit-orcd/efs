#ifndef EFS_DIR_SPREAD_H
#define EFS_DIR_SPREAD_H

#include "efs/common.h"
#include "efs/meta_apply.h"
#include <sys/stat.h>

/* In-memory SPLITTING-ino queue (architecture.md §7.4). Lost on crash;
 * re-note on DIR_BEGIN apply, on a LOCAL→SPLITTING nents flip, and when
 * a mutation writes a SPLITTING dir row. The host drains from the GC
 * thread; the sim drains on the size-trigger flip. */

void efs_dir_spread_note(efs_ino_t ino);
int efs_dir_spread_pop(efs_ino_t *ino); /* 1 if popped */

static inline void efs_dir_spread_seen(const struct efs_meta_row *row)
{
    if (row && (row->mode & S_IFMT) == S_IFDIR &&
        row->layout == EFS_META_LAYOUT_SPLITTING)
        efs_dir_spread_note(row->ino);
}

#endif
