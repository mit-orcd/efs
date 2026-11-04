#ifndef EFS_FUSE_INTERNAL_H
#define EFS_FUSE_INTERNAL_H

/* Shared by every C file of src/client/efs_fuse/: the includes, the open
 * description handle, the per-request thread-local and the few helpers
 * every handler calls. Each area declares its own interface in <area>.h
 * next to its owner, with the ownership, lock, lifetime and failure
 * contract at the top; a file includes only the areas it calls. */

#include "efs/settings.h"
#include "../../common/parse_host_port.h"
#include "efs/cli_help.h"
#define FUSE_USE_VERSION 312

#include "../client_internal.h"
#include "efs/put_ticket.h"
#include "efs/session.h"
#include "efs/common.h"
#include "efs/write_extent.h"
#include <limits.h>
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/command_path.h"
#include "efs/kv_key.h"
#include "efs/log_ts.h"
#include "efs/version.h"
#include "efs/placement.h"
#include "efs/checksum.h"
#include "efs/erasure.h"
#include "efs/rdma.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <fuse_lowlevel.h>
#include <fuse_opt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../../common/recorder_stop.h"
#include "../../common/recorder_start.h"
#include <sys/statvfs.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h> /* FALLOC_FL_KEEP_SIZE (_GNU_SOURCE) */
#include <sys/file.h>
#include <limits.h>
#include <signal.h>
#include <execinfo.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sched.h>


struct efs_file_handle {
    efs_ino_t ino;
    struct efs_wb_description *errors;
    char *snapshot; /* Immutable virtual-file bytes owned by this open. */
    size_t snapshot_len;
};

/* Set by the ll_* body for the request it serves (ll_run.c); ll_ctx()
 * (support.c) reads uid/gid from it. NULL outside a handler. */
extern __thread fuse_req_t t_req;

/* Helpers every handler calls: the handle behind fi->fh (support.c),
 * the monotonic clock (support.c), the virtual-inode kind (support.c),
 * and the two errno mappings (support.c, namespace.c). */
struct efs_file_handle *efs_file_handle(const struct fuse_file_info *fi);
efs_ino_t efs_file_ino(const struct fuse_file_info *fi, efs_ino_t fallback);
uint64_t stats_now_ms(void);
int virt_kind(fuse_ino_t ino);
int fuse_stat_errno(int rc);
int efs_rc_to_errno(int rc);


/* ll_run.c */
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
