#ifndef EFS_FUSE_OPEN_H
#define EFS_FUSE_OPEN_H

#include "fuse_internal.h"

/* open.c: open references, lease acquire and retry, OPEN and READ.
 *
 * Owns
 *   struct efs_open_ref    one row per inode (hash, never a table walk):
 *                      open count, append burst state, the lease edge and
 *                      its adoption request. A row lives from the first
 *                      open until the last close is confirmed; a row whose
 *                      last close could not be confirmed stays with
 *                      close_pending set until the retry thread or the next
 *                      RELEASE proves the CLOSE edge.
 *   struct efs_pending_create
 *                      an abandoned CREATE whose cleanup is still owed;
 *                      queued by namespace.c, drained by the retry pass.
 *   efs_open_retry_*   the background retry thread: drains retained
 *                      releases and pending CREATEs oldest first, bounded
 *                      per pass; started at FUSE_INIT, joined at destroy.
 *
 * Locks (take the stripe first, then g_open_mu; never the reverse)
 *   open_edge_mu(ino)  one of OPEN_EDGE_SHARDS stripes; serializes the
 *                      lease edge of one inode (first OPEN, last RELEASE,
 *                      the retry pass) and owns the row's edge and
 *                      adoption bodies. May be held across the lease RPC.
 *   g_open_mu          the table and hash, n, close_pending, retry_order
 *                      and retry_turn of every row. Table operations only,
 *                      never across an RPC (client rule,
 *                      ../client_internal.h). append.c takes it to read and
 *                      update a row's append fields.
 *
 * Failure behaviour
 *   EFS_ERR_BUSY: the identity window or the owner's admission is full,
 *   or the caller's deadline has passed; the row is retained and the
 *   caller may retry on its own budget (OPEN waits for a position, RELEASE
 *   spins CLOSE_ADMIT_SPINS times, release.h). Any other EFS_ERR_* is the
 *   lease RPC's own error and the open fails with efs_rc_to_errno of it.
 *   efs_fuse_open_ino and efs_fuse_read_ino return a negative errno:
 *   EAGAIN when a virtual result was evicted between ensure and pin,
 *   ESTALE when it no longer matches the opened version, EIO for a failed
 *   read. */

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

struct efs_pending_create {
    struct efs_client_create_request request;
    struct efs_client_lease_request cleanup;
    struct efs_pending_create *next;
};

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

#endif
