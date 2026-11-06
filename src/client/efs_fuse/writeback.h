#ifndef EFS_FUSE_WRITEBACK_H
#define EFS_FUSE_WRITEBACK_H

#include "fuse_internal.h"

/* writeback.c: the write-back pool and bounce buffers.
 *
 * Owner: writeback.c alone. The pool state (a ring of jobs, the workers,
 *   the per-worker busy ranges and the sticky PUT error) and its mutex are
 *   private to it; callers use only the operations below and never lock
 *   the pool or read its fields.
 * Lock: one private pool mutex; no operation below is called with it held
 *   and none returns with it held. Drains take it within the caller's RPC
 *   deadline (wb_lock_budget, open.h). Bounce buffers have their own lock.
 * Lifetime: the workers start on the first efs_wb_enqueue_owned and run
 *   until efs_wb_stop (efs_fuse_destroy, main.c). A worker leaves only once
 *   the ring is empty, so every accepted job finishes; an in-flight PUT
 *   finishes before efs_wb_stop returns. After the stop the pool is
 *   unstarted again: efs_wb_pending says 0 and both drains EFS_OK, and the
 *   next enqueue starts fresh workers.
 * Acknowledgement: FUSE write() waits until its job's PUT has 2-of-3
 *   fragment quorum, never after a bounce copy (that made ewrite succeed
 *   while servers never saw the data).
 *
 * Operations and their failures
 *   efs_wb_enqueue_owned  queue a PUT that owns its buffer and wait for it.
 *       EFS_ERR_NOMEM: the workers could not start. EFS_ERR_BUSY: the ring
 *       stayed full for 8 s or past the caller's deadline. EFS_ERR_IO: the
 *       pool is stopping. In those three the buffer is released and
 *       nothing was queued. Otherwise the job's PUT rc.
 *   efs_wb_pending(ino)   ino 0: jobs queued plus in flight in the whole
 *       pool; otherwise 1 when a job for ino is queued or being run, else
 *       0. 0 before the pool starts. Never blocks past the pool mutex.
 *   efs_wb_sync           wait until the pool is idle, then return the
 *       sticky error (the first failed PUT since the last drain consumed
 *       it, any inode) once and clear it.
 *   efs_wb_sync_ino(ino)  wait until no job of ino is pending, then
 *       return and clear the sticky error only if ino set it; a later
 *       write to another inode is never refused because of it.
 *       Both drains: EFS_ERR_BUSY when the lock or the idle wait runs past
 *       the caller's deadline, EFS_ERR_IO when a wait fails; on either the
 *       pending work and the sticky error stay where they were, and
 *       efs_wb_pending tells what was left.
 *   efs_wb_stop           stop and join the workers after they finish
 *       every queued job; enqueues racing it get EFS_ERR_IO. Does not
 *       consume the sticky error: drain with efs_wb_sync first.
 *   A second, per-inode error channel lives in ../write.c
 *   (efs_wb_inode_stalled), read by efs_file_sync_error (support.h) at
 *   FSYNC/FLUSH. */

int efs_wb_enqueue_owned(efs_ino_t ino, uint64_t offset, size_t size,
                                char *copy, char *free_base, size_t buf_cap);
int efs_wb_pending(efs_ino_t ino);
int efs_wb_sync(void);
int efs_wb_sync_ino(efs_ino_t ino);
void efs_wb_stop(void);

/* Bounce buffers for efs_wb_enqueue_owned: cap_out != 0 means the buffer
 * returns to a pool of 128 KiB / 1 MiB buffers; pass it back as buf_cap. */
char *bounce_alloc(size_t size, size_t *cap_out);
void bounce_release(char *p, size_t cap);
void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path);

#endif
