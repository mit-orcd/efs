#ifndef EFS_FUSE_NAMESPACE_H
#define EFS_FUSE_NAMESPACE_H

#include "fuse_internal.h"

/* namespace.c: CREATE, MKDIR, UNLINK, RMDIR, STATFS, chmod/chown, SYMLINK,
 * LINK, utimens, TRUNCATE.
 *
 * Owns no state. CREATE acquires the created inode's lease through open.h
 * (efs_open_acquire_created) and, when the reply is lost, queues a
 * struct efs_pending_create for the retry pass. TRUNCATE and utimens
 * flush the inode's unpublished bytes first (efs_utimens_flush_dirty,
 * append.h).
 *
 * Failure behaviour: every handler replies a positive errno through
 * efs_rc_to_errno (fuse_internal.h, defined here): BUSY, AGAIN and
 * NOT_PRIMARY are EBUSY, never EIO (a leader freeze must not look like
 * corruption); ENOENT only for a genuinely absent name (I9); ENOTEMPTY,
 * EACCES, EOVERFLOW, ENOMEM and EINVAL keep their meaning; anything else
 * is EIO. */

int efs_fuse_create_at(fuse_ino_t parent_ino, const char *name,
                              mode_t mode, struct fuse_file_info *fi,
                              efs_ino_t *out_ino, struct efs_inode *created);
int efs_fuse_mkdir_at(fuse_ino_t parent_ino, const char *name, mode_t mode,
                             efs_ino_t *out_ino, struct efs_inode *created);
int efs_fuse_unlink_at(fuse_ino_t parent_ino, const char *name);
int efs_fuse_rmdir_at(fuse_ino_t parent_ino, const char *name);
int efs_fuse_statfs(const char *path, struct statvfs *stbuf);
int efs_fuse_chmod_ino(fuse_ino_t ino, mode_t mode);
int efs_fuse_chown_ino(fuse_ino_t ino, uid_t uid, gid_t gid);
int efs_fuse_symlink_at(const char *link, fuse_ino_t parent_ino,
                               const char *name, efs_ino_t *out_ino);
int efs_fuse_readlink_ino(fuse_ino_t ino, char *buf, size_t size);
int efs_fuse_link_at(fuse_ino_t src_ino, fuse_ino_t newparent,
                            const char *newname);
int efs_fuse_utimens_ino(fuse_ino_t ino, const struct timespec tv[2]);
int efs_fuse_truncate_ino(fuse_ino_t ino, off_t size,
                                 struct fuse_file_info *fi);

#endif
