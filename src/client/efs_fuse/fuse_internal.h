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


struct readdir_ent {
    char name[EFS_MAX_NAME];
    struct stat st;
};

struct readdir_collect_arg {
    struct readdir_ent *ents;
    size_t count;
    size_t cap;
    size_t *name_index; /* Entry index plus one; zero is an empty bucket. */
    size_t name_index_cap;
};

/* Per-opendir listing. fi->fh is this pointer (files carry an open-description handle).
 * First READDIR fills it; later READDIR / EOF getdents reuse it. */
struct efs_dirh {
    efs_ino_t ino;
    struct readdir_ent *ents;
    size_t count;
    fuse_ino_t parent_ino;
    mode_t self_mode;
    mode_t parent_mode;
    int listed;
};

struct efs_open_ref {
    efs_ino_t ino;
    uint64_t n;
    uint32_t append_burst;
    uint64_t append_burst_ms;
    uint64_t append_handoff_ms;
    int append_publishing;
    int leased; /* first open took HOLD; last close must release it */
    int close_pending; /* retain ownership until release is confirmed */
    uint64_t lease_generation;
    uint64_t retry_order; /* published under g_open_mu; edge is stripe-owned */
    uint64_t retry_turn; /* fair retry visits, protected by g_open_mu */
    struct efs_client_lease_request edge; /* immutable OPEN or CLOSE identity */
    struct efs_client_lease_request adoption; /* clear pending before CLOSE */
    struct efs_open_ref *next, *prev; /* every row, for retry scans */
    struct efs_open_ref *hnext; /* inode hash chain */
};

/* Bounded wait (1 ms steps) for a CLOSE identity position, and the most
 * retained releases one background pass may drain. A full window is
 * transient (milliseconds); keep the bound short so an unreachable owner
 * cannot hold FUSE workers in RELEASE (the drain retries the rest). */
#define CLOSE_ADMIT_SPINS 250u

struct efs_pending_create {
    struct efs_client_create_request request;
    struct efs_client_lease_request cleanup;
    struct efs_pending_create *next;
};

/* Write pool: FUSE write() waits until each job's PUT has 2-of-3 fragment
 * quorum. The pool still overlaps PUTs across concurrent FUSE requests
 * (many files / max_background). Do not ACK after a bounce copy — that
 * made ewrite succeed while servers never saw the data. */
#define EFS_WB_DEPTH        256

#define EFS_WB_WORKERS_MAX  16

#define EFS_WB_RESERVED     4

struct efs_wb_job {
    efs_ino_t ino;
    uint64_t offset;
    size_t size;
    char *buf;
    /* If set, this is the allocation that contains buf (stolen FUSE
     * receive buffer). Free this instead of buf when the job completes. */
    char *free_base;
    /* Non-zero: buf/free_base came from the bounce pool (see bounce_release). */
    size_t buf_cap;
    uint64_t queued_ms;
    uint64_t deadline;
    /* Caller waits for PUT quorum. Worker writes rc and signals done_cv
     * while holding g_wb.mu. */
    int *done;
    int *done_rc;
    pthread_cond_t *done_cv;
};

struct efs_wb_state {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t idle;
    struct efs_wb_job q[EFS_WB_DEPTH];
    int head, tail, count, inflight;
    uint64_t queued_bytes, inflight_bytes, admission_waits, admission_timeouts;
    int err;
    efs_ino_t err_ino; /* inode that set err; 0 = none / global */
    int ready;
    int shutdown;
    pthread_t workers[EFS_WB_WORKERS_MAX];
    efs_ino_t busy_ino[EFS_WB_WORKERS_MAX];
    uint64_t busy_off[EFS_WB_WORKERS_MAX];
    size_t busy_len[EFS_WB_WORKERS_MAX];
    int nworkers;
};

#ifndef OFF_MAX
#define OFF_MAX ((off_t)(((unsigned long long)1 << (sizeof(off_t) * 8 - 1)) - 1))
#endif

struct deep_callback_sample {
    uint64_t begin, service_end;
    long pid;
    char name[6];
};

/* getattr.c */
extern __thread uint64_t t_lookup_memo_serial;
extern __thread struct efs_inode t_getattr_row;
extern __thread efs_ino_t t_getattr_ino;
uint64_t fuse_now_us(void);
uint64_t lookup_memo_start(void);
void lookup_memo_mutation_begin(void);
void lookup_memo_mutation_end(void);
void lookup_memo_put(const struct efs_inode *row);
int efs_fuse_getattr_ino(fuse_ino_t ino, struct stat *stbuf,
                                struct fuse_file_info *fi);
struct efs_dirh *dirh_new(efs_ino_t ino);
void dirh_free(struct efs_dirh *dh);
int dirh_fill(struct efs_dirh *dh, fuse_ino_t ino);
int efs_fuse_access_ino(fuse_ino_t ino, int mask);

/* open.c */
extern pthread_mutex_t g_open_mu;
struct efs_open_ref *open_ref_find(efs_ino_t ino);
pthread_mutex_t *open_edge_mu(efs_ino_t ino);
int efs_close_note(efs_ino_t ino, int *leased);
int efs_open_release_pending(efs_ino_t ino);
int efs_open_close_unadmitted(efs_ino_t ino);
int wb_lock_budget(pthread_mutex_t *mu);
int efs_open_acquire_fileid(efs_ino_t ino, uint64_t create_generation);
int efs_open_acquire_created(efs_ino_t ino, uint64_t generation,
                                    uint64_t token, int *handed_off);
struct efs_pending_create *efs_pending_create_alloc(void);
void efs_pending_create_free(struct efs_pending_create *entry);
void efs_pending_create_queue(struct efs_pending_create *entry);
int efs_open_retry_pass(uint64_t deadline);
void efs_open_retry_start(void);
void efs_open_retry_stop(void);
void fuse_fi_direct_io(struct fuse_file_info *fi);
int fuse_odirect_unaligned(const struct fuse_file_info *fi,
                                  off_t offset, size_t size);
int efs_fuse_open_ino(fuse_ino_t ino, struct fuse_file_info *fi);
int efs_fuse_read_ino(fuse_ino_t ino, char *buf, size_t size, off_t offset,
                             struct fuse_file_info *fi);

/* writeback.c */
extern struct efs_wb_state g_wb;
char *bounce_alloc(size_t size, size_t *cap_out);
void bounce_release(char *p, size_t cap);
void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path);
int efs_wb_enqueue_owned(efs_ino_t ino, uint64_t offset, size_t size,
                                char *copy, char *free_base, size_t buf_cap);
int efs_wb_sync(void);
int efs_wb_ino_pending_locked(efs_ino_t ino);
int efs_wb_sync_ino(efs_ino_t ino);

/* append.c */
pthread_mutex_t *append_mu(efs_ino_t ino);
int efs_append_flush_report(struct fuse_file_info *fi, efs_ino_t ino);
int fuse_pressure_flush(efs_ino_t ino);
int fuse_pressure_kick(void);
void fuse_pressure_stop(void);
int efs_utimens_flush_dirty(efs_ino_t ino);
off_t append_end_offset(efs_ino_t ino);
int append_reserve_offset(efs_ino_t ino, uint64_t len, off_t *off_out);
int append_publish_burst(efs_ino_t ino);

/* write.c */
int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi);
int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi);
int efs_fuse_fsync_ino(fuse_ino_t ino, int isdatasync,
                              struct fuse_file_info *fi);
int efs_fuse_flush_ino(fuse_ino_t ino, struct fuse_file_info *fi);

/* namespace.c */
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

/* release.c */
int efs_fuse_release_ino(fuse_ino_t ino, struct fuse_file_info *fi);
int efs_control_drain(uint64_t deadline, int force);
int efs_fuse_rename_at(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags, efs_ino_t *out_src);
off_t efs_fuse_lseek_ino(fuse_ino_t ino, off_t off, int whence,
                                struct fuse_file_info *fi);

/* plock_init.c */
int efs_fuse_getlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
                              struct flock *lock);
int efs_fuse_setlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
                              struct flock *lock, int sleep);
int efs_fuse_flock_ino(fuse_ino_t ino, struct fuse_file_info *fi, int op);
extern int g_fuse_ready_wr;
int efs_fuse_wait_ready(int rfd, pid_t child, int timeout_ms);
void efs_fuse_init(void *userdata, struct fuse_conn_info *conn);

/* lookup.c */
void fill_entry(fuse_ino_t ino, struct fuse_entry_param *e,
                       const struct stat *st);
void fill_entry_row(fuse_ino_t ino, struct fuse_entry_param *e,
                           const struct efs_inode *row);
int lookup_fill(fuse_ino_t ino, struct fuse_entry_param *e,
                       struct fuse_file_info *fi);
int lookup_fill_committed(fuse_ino_t ino, struct fuse_entry_param *e,
                                 const char *op, fuse_ino_t parent,
                                 const char *name);
int efs_fuse_lookup_at(fuse_ino_t parent, const char *name,
                              struct fuse_entry_param *e);
int efs_fuse_setattr_ino(fuse_ino_t ino, struct stat *attr, int to_set,
                                struct fuse_file_info *fi);
int dirbuf_add(fuse_req_t req, char *buf, size_t bufsize, size_t *used,
                      const char *name, fuse_ino_t ino, mode_t mode, off_t next);
struct deep_callback_sample deep_callback_begin(fuse_req_t req, const char *name);
void deep_callback_reply_begin(struct deep_callback_sample *sample);
void deep_callback_end(struct deep_callback_sample sample, const char *op,
                              fuse_ino_t parent, int rc);

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
