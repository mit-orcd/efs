#ifndef EFS_FUSE_OPEN_H
#define EFS_FUSE_OPEN_H

#include "fuse_internal.h"

/* open.c: the open-reference table, lease acquire and retry, OPEN and READ.
 *
 * Owner: open.c alone. The table (one struct efs_open_ref row per inode,
 *   a hash and a list, the row counts and flags, the retry keys and the
 *   append burst fields) and its mutex are private to it; callers use only
 *   the operations below and never lock the table or read a row.
 * Lock: one private table mutex, never held across an RPC and never held
 *   when an operation below returns. Each operation takes it once. The
 *   inode edge stripe (open_edge_mu, OPEN_EDGE_SHARDS) is public: it
 *   serializes one inode's lease edge (first OPEN, last RELEASE, the retry
 *   pass) and may be held across the lease RPC; a caller that holds both
 *   takes the stripe first. Operations that prove an edge
 *   (efs_open_acquire_*, efs_open_release_pending, efs_open_close_unadmitted)
 *   are called with the inode's stripe held; the table operations
 *   (efs_close_note, efs_client_ino_is_open, efs_open_retained, the
 *   efs_open_append_* three) need no lock of the caller.
 * Lifetime: a row is created by the first open of an inode and freed once
 *   its last close is confirmed. A last close whose CLOSE edge could not be
 *   confirmed leaves the row retained (count 0, close pending) until the
 *   retry thread (efs_open_retry_start at FUSE_INIT, efs_open_retry_stop at
 *   destroy, one bounded pass per 100 ms), the next RELEASE or the next
 *   OPEN of that inode proves the edge (the OPEN waits for an unanswered
 *   CLOSE within its deadline and fails with the edge's error otherwise,
 *   then opens afresh). A retained row counts as open: the
 *   inode stays pinned in the cache evictor and GETATTR keeps its open-fd
 *   path until the CLOSE is proven. An abandoned CREATE whose cleanup is
 *   still owed is a struct efs_pending_create, queued by namespace.c and
 *   drained by the retry pass oldest first.
 *
 * Operations and their answers
 *   efs_open_acquire_fileid(ino, gen)   take this descriptor's open: the
 *       first open of the inode takes the HOLD lease and fetches the
 *       FileID when gen is 0. EFS_ERR_BUSY: the identity window or the
 *       owner's admission is full, or the caller's deadline passed; the
 *       row is retained and the caller may retry on its own budget.
 *       Any other EFS_ERR_* is the lease RPC's error (the open fails with
 *       efs_rc_to_errno of it) or EFS_ERR_NOMEM when no row could be made.
 *   efs_open_acquire_created(ino, gen, token, *handed_off)
 *       the CREATE variant: adopts the pending token into the lease edge;
 *       handed_off says the token was consumed. EFS_ERR_INVAL without
 *       gen or token; otherwise as above.
 *   efs_close_note(ino, *leased)   drop one open; 1 when it was the last
 *       (then *leased says the HOLD must be released and the row is kept
 *       pending that CLOSE), 0 otherwise and for an unknown inode.
 *   efs_open_release_pending(ino)   prove the retained CLOSE (or the
 *       adoption before it) and free the row: EFS_OK when nothing is
 *       pending or the row is gone; EFS_ERR_BUSY while the edge has no
 *       answer yet (the row stays retained, its retry key is published);
 *       any other EFS_ERR_* is the lease RPC's.
 *   efs_open_close_unadmitted(ino)   1 while the retained CLOSE has no
 *       identity position yet (a full window, worth a short wait), else 0.
 *   efs_open_retry_pass(deadline)   one bounded background pass: retained
 *       releases oldest first, then pending CREATEs, within the caller's
 *       deadline. EFS_OK when nothing is retained or pending, EFS_ERR_BUSY
 *       when something is (also when the deadline is already past), any
 *       other EFS_ERR_* from the last edge proven.
 *   efs_client_ino_is_open(ino)   1 while a row exists for ino, retained
 *       rows included (client_internal.h; the evictor's pin test).
 *   efs_open_retained()   rows waiting for a confirmed last close (count
 *       0, close pending); 0 when none. Walks the table under its mutex:
 *       for the stop path and diagnostics, not per operation.
 *   efs_open_append_handoff(ino, *publishing)   append.c's publication
 *       gate: the row's handoff time (0 when none or no row) and whether
 *       a publication is running.
 *   efs_open_append_burst_note(ino, now)   count one append write on
 *       the row: 1 when this write must publish first (32 writes or one
 *       second of unpublished ownership; the row is marked publishing and
 *       its burst restarted), 0 otherwise, also when a publication is
 *       already running or the inode has no row.
 *   efs_open_append_burst_published(ino, rc, done_ms)   end that
 *       publication: on EFS_OK the burst age restarts at done_ms and a
 *       100 ms handoff window opens for a competing client; on failure
 *       the burst is left full so the next write publishes again.
 *   wb_lock_budget(mu)   lock mu within the caller's RPC deadline:
 *       EFS_ERR_BUSY past it, EFS_ERR_IO on a lock error.
 *   efs_fuse_open_ino and efs_fuse_read_ino return a negative errno:
 *   EAGAIN when a virtual result was evicted between ensure and pin,
 *   ESTALE when it no longer matches the opened version, EIO for a failed
 *   read. */

struct efs_pending_create {
    struct efs_client_create_request request;
    struct efs_client_lease_request cleanup;
    struct efs_pending_create *next;
};

pthread_mutex_t *open_edge_mu(efs_ino_t ino);
int efs_close_note(efs_ino_t ino, int *leased);
int efs_open_release_pending(efs_ino_t ino);
int efs_open_close_unadmitted(efs_ino_t ino);
unsigned efs_open_retained(void);
uint64_t efs_open_append_handoff(efs_ino_t ino, int *publishing);
int efs_open_append_burst_note(efs_ino_t ino, uint64_t now);
void efs_open_append_burst_published(efs_ino_t ino, int rc, uint64_t done_ms);
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

#endif
