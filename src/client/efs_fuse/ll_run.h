#ifndef EFS_FUSE_LL_RUN_H
#define EFS_FUSE_LL_RUN_H

#include "fuse_internal.h"

/* ll_run.c: the low-level operation bodies (ll_* and ll_*_run).
 *
 * Owns no state. Each body sets t_req (fuse_internal.h) and, for LOOKUP,
 * t_lookup_memo_serial (getattr.h) around the *_at / *_ino call, replies
 * once, then clears them; invalidations are queued after the reply
 * (ll_inval_*, support.h). main.c wraps the mutating bodies with the
 * stop gate (../stop_control.h) and installs them in the operation table.
 *
 * Failure behaviour: a body replies fuse_reply_err with the positive errno
 * its callee returned and never substitutes a default. */

void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name);
void ll_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup);
void ll_forget_multi(fuse_req_t req, size_t count,
                            struct fuse_forget_data *forgets);
void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void ll_setattr_run(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                       int to_set, struct fuse_file_info *fi);
void ll_readlink(fuse_req_t req, fuse_ino_t ino);
void ll_mkdir_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode);
void ll_unlink_run(fuse_req_t req, fuse_ino_t parent, const char *name);
void ll_rmdir_run(fuse_req_t req, fuse_ino_t parent, const char *name);
void ll_symlink_run(fuse_req_t req, const char *link, fuse_ino_t parent,
                       const char *name);
void ll_rename_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                      fuse_ino_t newparent, const char *newname,
                      unsigned int flags);
void ll_link_run(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                    const char *newname);
void ll_open_run(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                    struct fuse_file_info *fi);
void ll_write_run(fuse_req_t req, fuse_ino_t ino, const char *buf,
                     size_t size, off_t off, struct fuse_file_info *fi);
void ll_write_buf_run(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv,
                         off_t off, struct fuse_file_info *fi);
void ll_flush_run(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void ll_fsync_run(fuse_req_t req, fuse_ino_t ino, int datasync,
                     struct fuse_file_info *fi);
void ll_fallocate_run(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset,
                         off_t length, struct fuse_file_info *fi);
void ll_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                       struct fuse_file_info *fi);
void ll_releasedir(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi);
void ll_statfs(fuse_req_t req, fuse_ino_t ino);
void ll_access(fuse_req_t req, fuse_ino_t ino, int mask);
void ll_mknod_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode, dev_t rdev);
void ll_create_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, struct fuse_file_info *fi);
void ll_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     struct flock *lock);
void ll_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     struct flock *lock, int sleep);
void ll_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     int op);
void ll_lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
                     struct fuse_file_info *fi);
void ll_setxattr_run(fuse_req_t req, fuse_ino_t ino, const char *name,
                        const char *value, size_t size, int flags);
void ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                        size_t size);
void ll_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size);
void ll_removexattr_run(fuse_req_t req, fuse_ino_t ino, const char *name);

#endif
