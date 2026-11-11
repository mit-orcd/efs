#ifndef EFS_CLIENT_PUT_WINDOW_H
#define EFS_CLIENT_PUT_WINDOW_H

#include "efs/common.h"

/* put_window.c: the PUT window, a per-inode count of PUTs that marked a
 * dcache slot clean but have not yet recorded the chunk in a dirty set.
 *
 * Owner: put_window.c alone. The table (512 slots of inode and count), the
 *   overflow count (windows that found no free slot), the all-inode total,
 *   the mutex and the condition variable are private to it; callers use
 *   only the operations below and never read the state.
 * Lock: one private mutex, put_win_mu, a leaf: under it nothing else is
 *   locked and no RPC is made, and no operation returns holding it. The
 *   operations take no other lock, so a caller may hold any lock when it
 *   calls them. Today's callers (write.c): put_win_open runs with
 *   dcache_io_mu and then dcache_mu held (that order, the same as every
 *   other pair of the two); put_win_close runs with neither held (after
 *   a PUT in flush_pipe_drain and flush_pipe_add, and on the failure
 *   returns of dcache_steal_dirty_budgeted and of the base fetch in
 *   dcache_flush_slot_inner_budgeted) or with both held (in
 *   dcache_flush_slot_inner_budgeted, the re-lock after the PUT and the
 *   retired-inode branch of the base fetch). put_win_wait is called
 *   with no cache lock held, and a waiter must hold none that a flusher
 *   needs to reach its close (dcache_io_mu, dcache_mu).
 *   The total is also read atomically without the mutex (the reader fast
 *   path).
 * Guarantees:
 *   put_win_open     counts one window for the inode (a slot, or the
 *                    overflow count when all 512 slots are in use). The
 *                    caller owes exactly one put_win_close for it, on every
 *                    exit of the flusher, after the put-record or after the
 *                    re-dirty on failure.
 *   put_win_close    ends one window and wakes every waiter. A close
 *                    without a matching open is a caller error that the
 *                    owner does not detect. If no slot holds the inode it
 *                    decrements the overflow count when that is above 0,
 *                    and in every case it decrements the all-inode total
 *                    when that is above 0, so it ends the window of some
 *                    other inode in the count: the total reaches 0 while
 *                    that window is open, and efs_dcache_put_win_wait
 *                    returns 0 at once for every inode (a reader may see
 *                    a stolen body as a hole), and an overflow window
 *                    ends early, so put_win_wait returns 0 for inodes
 *                    whose window is still open. Slot counts never go
 *                    below 0.
 *   put_win_wait     returns 0 when no window of the inode is open (the
 *                    overflow count makes every inode busy), 1 after it
 *                    waited and they closed, EFS_ERR_BUSY when one is still
 *                    open at the earlier of max_ms and the caller's RPC
 *                    deadline, EFS_ERR_IO when the wait itself fails. It
 *                    wakes at least every 50 ms to re-check, and a close
 *                    wakes it at once.
 *   efs_dcache_put_win_wait  the reader's wait: 0 at once, without the
 *                    mutex, when no window of any inode is open; otherwise
 *                    put_win_wait(ino, 8000).
 *
 * The window does not tell you what bounds the PUT itself (its deadline,
 * retries, quorum and completion): that is docs/how-it-works/put-lifecycle.md. */

void put_win_open(efs_ino_t ino);
void put_win_close(efs_ino_t ino);
int put_win_wait(efs_ino_t ino, unsigned max_ms);
/* The only declaration; read.c includes this header. client_internal.h does
 * not, so a test that stubs the name does not clash. */
int efs_dcache_put_win_wait(efs_ino_t ino);

#endif
