#ifndef EFS_FUSE_WRITEBACK_H
#define EFS_FUSE_WRITEBACK_H

#include "fuse_internal.h"

/* writeback.c: the write-back pool and bounce buffers.
 *
 * Owns
 *   g_wb (struct efs_wb_state)
 *                      a ring of EFS_WB_DEPTH jobs (struct efs_wb_job) and
 *                      up to EFS_WB_WORKERS_MAX workers, started on first
 *                      use (efs_wb_enqueue_owned) and joined by
 *                      efs_fuse_destroy (main.c). Workers overlap PUTs
 *                      across inodes; two jobs on one inode serialize on a
 *                      private per-inode stripe (EFS_WB_INO_STRIPES).
 *   bounce_alloc/release
 *                      a pool of 128 KiB and 1 MiB buffers, private mutex.
 *
 * Lock: g_wb.mu covers every field of g_wb; not_empty / not_full / idle
 *   are its condition variables. A job is acknowledged to FUSE only after
 *   its PUT has fragment quorum, never after a bounce copy. append.c,
 *   main.c and release.c take g_wb.mu to test ready / count / inflight and
 *   to shut the pool down; nothing else touches the fields directly.
 *
 * Failure behaviour
 *   efs_wb_enqueue_owned: EFS_ERR_NOMEM when the pool cannot start,
 *   EFS_ERR_BUSY when the ring stays full for 8 s or the caller's deadline
 *   (the buffer is released, nothing was queued), EFS_ERR_IO at shutdown;
 *   otherwise the job's PUT rc after the caller waited for it.
 *   err / err_ino retain the first failed PUT until a drain consumes it:
 *   efs_wb_sync (every inode) and efs_wb_sync_ino (that inode only) return
 *   it once and clear it; a later write to another inode is not refused.
 *   Both drains return EFS_ERR_BUSY when the lock or the idle wait runs
 *   past the caller's deadline (wb_lock_budget, open.c). A second,
 *   per-inode error channel lives in ../write.c (efs_wb_inode_stalled),
 *   read by efs_file_sync_error (support.h) at FSYNC/FLUSH. */

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

#endif
