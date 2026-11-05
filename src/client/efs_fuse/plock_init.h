#ifndef EFS_FUSE_PLOCK_INIT_H
#define EFS_FUSE_PLOCK_INIT_H

#include "fuse_internal.h"

/* plock_init.c: POSIX byte-range locks, flock, FUSE_INIT and readiness.
 *
 * Owns
 *   the lock table     struct efs_plock rows under a private mutex and
 *                      condition variable; a row pins its inode in the
 *                      client cache until unlocked.
 *   efs_fuse_init      FUSE_INIT: negotiates the connection, starts the
 *                      open retry thread (open.h) and signals readiness.
 *   g_fuse_ready_wr    the write end of the daemonize readiness pipe:
 *                      main() sets it before fuse_session_loop_mt (-1 in
 *                      the foreground), FUSE_INIT writes one byte, clears
 *                      and closes it; main() closes it at exit if FUSE_INIT
 *                      never ran. Single-threaded phases, no lock.
 *
 * Failure behaviour: F_SETLK returns -EAGAIN on a local conflict or when
 * the server lease RPC is BUSY/AGAIN; F_SETLKW blocks in the daemon on
 * the condition variable and retries the RPC instead (returning EAGAIN
 * made the kernel fail the syscall at once). Other RPC errors map through
 * efs_rc_to_errno. efs_fuse_wait_ready returns -1 when the child did not
 * signal within timeout_ms. */

#ifndef OFF_MAX
#define OFF_MAX ((off_t)(((unsigned long long)1 << (sizeof(off_t) * 8 - 1)) - 1))
#endif

int efs_fuse_getlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
                              struct flock *lock);
int efs_fuse_setlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
                              struct flock *lock, int sleep);
int efs_fuse_flock_ino(fuse_ino_t ino, struct fuse_file_info *fi, int op);
extern int g_fuse_ready_wr;
int efs_fuse_wait_ready(int rfd, pid_t child, int timeout_ms);
void efs_fuse_init(void *userdata, struct fuse_conn_info *conn);

#endif
