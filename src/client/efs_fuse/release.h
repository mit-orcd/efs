#ifndef EFS_FUSE_RELEASE_H
#define EFS_FUSE_RELEASE_H

#include "fuse_internal.h"

/* release.c: RELEASE, the control drain, RENAME and LSEEK.
 *
 * Owns no state. RELEASE drops the open description (support.h), unlocks
 * a flock owner the kernel reports at final close, and proves the
 * last-close lease edge through open.h under the inode's stripe: when the
 * identity window is momentarily full it waits CLOSE_ADMIT_SPINS steps of
 * 1 ms for a position, then leaves the row retained for the retry thread.
 * efs_control_drain is the stop path: it waits for the write-back pool
 * to go idle (efs_wb_pending, writeback.h), runs the open retry pass, drains the
 * data cache and reports dirty metadata until all are clean or the
 * deadline passes.
 *
 * Failure behaviour: efs_fuse_release_ino replies the flock unlock's errno
 * (0 when there was none, logged when it failed) even when the lease
 * release stays BUSY: the row is retained and the retry thread or the
 * drain proves it later. efs_control_drain returns 0 when
 * everything is published and -1 when unresolved writes or releases
 * remain, in which case the mount is retained. RENAME and LSEEK reply a
 * positive errno through efs_rc_to_errno; RENAME never silently drops a
 * flag it does not implement (EINVAL). */

/* Bounded wait (1 ms steps) for a CLOSE identity position, and the most
 * retained releases one background pass may drain. A full window is
 * transient (milliseconds); keep the bound short so an unreachable owner
 * cannot hold FUSE workers in RELEASE (the drain retries the rest). */
#define CLOSE_ADMIT_SPINS 250u

int efs_fuse_release_ino(fuse_ino_t ino, struct fuse_file_info *fi);
int efs_control_drain(uint64_t deadline, int force);
int efs_fuse_rename_at(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags, efs_ino_t *out_src);
off_t efs_fuse_lseek_ino(fuse_ino_t ino, off_t off, int whence,
                                struct fuse_file_info *fi);

#endif
