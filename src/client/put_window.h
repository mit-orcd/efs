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
 * Lock: one private mutex, put_win_mu, a leaf. It is taken by put_win_open
 *   and put_win_close while the caller holds dcache_io_mu and dcache_mu
 *   (dcache_steal_dirty, dcache_flush_slot_inner_budgeted), so those come
 *   first; under it nothing else is locked and no RPC is made. A waiter
 *   holds no cache lock, and no operation returns holding the mutex. The
 *   total is also read atomically without the mutex (the reader fast path).
 * Guarantees:
 *   put_win_open     counts one window for the inode (a slot, or the
 *                    overflow count when all 512 slots are in use). The
 *                    caller owes exactly one put_win_close for it, on every
 *                    exit of the flusher, after the put-record or after the
 *                    re-dirty on failure.
 *   put_win_close    ends one window and wakes every waiter. Closing with
 *                    no open window of the inode ends an overflow window
 *                    if there is one, and is otherwise harmless.
 *   put_win_wait     returns 0 when no window of the inode is open (the
 *                    overflow count makes every inode busy), 1 after it
 *                    waited and they closed, EFS_ERR_BUSY when one is still
 *                    open at the earlier of max_ms and the caller's RPC
 *                    deadline, EFS_ERR_IO when the wait itself fails. It
 *                    wakes at least every 50 ms to re-check, and a close
 *                    wakes it at once.
 *   efs_dcache_put_win_wait  the reader's wait: 0 at once, without the
 *                    mutex, when no window of any inode is open; otherwise
 *                    put_win_wait(ino, 8000). */

void put_win_open(efs_ino_t ino);
void put_win_close(efs_ino_t ino);
int put_win_wait(efs_ino_t ino, unsigned max_ms);
/* Also declared in client_internal.h, which read.c includes; the two agree. */
int efs_dcache_put_win_wait(efs_ino_t ino);

#endif
