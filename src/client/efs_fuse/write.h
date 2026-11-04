#ifndef EFS_FUSE_WRITE_H
#define EFS_FUSE_WRITE_H

#include "fuse_internal.h"

/* write.c: WRITE admission, the store and buffer paths, FSYNC and FLUSH.
 *
 * Owns no state. Admission reserves worst-case chunk capacity before any
 * byte is accepted; under capacity pressure it drains bounded cohorts
 * (fuse_pressure_flush, append.h) and retries within the caller's
 * deadline. Accepted bytes go to the write-back pool
 * (efs_wb_enqueue_owned, writeback.h) or the append path.
 *
 * Failure behaviour (negative errno to the FUSE reply)
 *   -EAGAIN   admission still BUSY at the deadline, or the request arrived
 *             past it; nothing was accepted.
 *   -ENOSPC   quota; -ENOMEM capacity; -EFBIG / -EINVAL an invalid extent.
 *   -EIO      a failed PUT, a stalled inode, or a retained write-back
 *             error: FSYNC and FLUSH drain this inode (efs_wb_sync_ino)
 *             and then consult efs_file_sync_error (support.h), which
 *             folds the per-description error and the stalled check of
 *             ../write.c. */

int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi);
int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi);
int efs_fuse_fsync_ino(fuse_ino_t ino, int isdatasync,
                              struct fuse_file_info *fi);
int efs_fuse_flush_ino(fuse_ino_t ino, struct fuse_file_info *fi);

#endif
