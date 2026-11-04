/* efs-fuse: POSIX locks and FUSE init. */
#include "fuse_internal.h"
#include "support.h"

/* In-memory POSIX byte-range locks. Overlapping exclusive ranges conflict
 * even for the same lock-owner: that matches the suite (two fds, one
 * process) and is how we tell a real lock from a FUSE no-op. */
struct efs_plock {
    efs_ino_t ino;
    uint64_t owner;
    off_t start;
    off_t end;
    int type;
    struct efs_plock *next;
};

static pthread_mutex_t g_plock_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_plock_cv = PTHREAD_COND_INITIALIZER;
static struct efs_plock *g_plocks;

/* Client-cache design Part A, pin rule 3: a row with live byte-range lock
 * records is unevictable (a lock owner's later unlock must find the row). */
int efs_client_ino_has_plock(efs_ino_t ino)
{
    int has = 0;

    pthread_mutex_lock(&g_plock_mu);
    for (struct efs_plock *p = g_plocks; p; p = p->next)
        if (p->ino == ino) {
            has = 1;
            break;
        }
    pthread_mutex_unlock(&g_plock_mu);
    return has;
}

static off_t plock_end(const struct flock *fl)
{
    if (fl->l_len == 0)
        return OFF_MAX;
    if (fl->l_start >= 0 && fl->l_len > 0 &&
        fl->l_start > OFF_MAX - fl->l_len)
        return OFF_MAX;
    return fl->l_start + fl->l_len;
}

static int plock_overlap(off_t a0, off_t a1, off_t b0, off_t b1)
{
    return a0 < b1 && b0 < a1;
}

static struct efs_plock *plock_find_conflict(efs_ino_t ino, off_t start,
                                             off_t end, int type,
                                             uint64_t owner)
{
    for (struct efs_plock *p = g_plocks; p; p = p->next) {
        if (p->ino != ino || p->owner == owner ||
            !plock_overlap(p->start, p->end, start, end))
            continue;
        if (p->type == F_WRLCK || type == F_WRLCK)
            return p;
    }
    return NULL;
}

static void plock_range_u64(const struct flock *lock, efs_ino_t ino,
                            uint64_t *start_out, uint64_t *end_out)
{
    off_t start = lock->l_start;
    off_t end;
    off_t base = 0;

    if (lock->l_whence == SEEK_END)
        base = append_end_offset(ino);
    start += base;
    {
        struct flock adj = *lock;
        adj.l_start = start;
        adj.l_whence = SEEK_SET;
        end = plock_end(&adj);
    }

    *start_out = start < 0 ? 0 : (uint64_t)start;
    *end_out = (end < 0 || end == OFF_MAX) ? ~(uint64_t)0 : (uint64_t)end;
}

static uint64_t plock_rpc_owner(struct fuse_file_info *fi)
{
    uint64_t lk = fi ? fi->lock_owner : 0;
    return g_client.flock_token ^ (lk ? lk : 1ull);
}

int efs_fuse_getlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
                              struct flock *lock)
{
    efs_ino_t inum = efs_file_ino(fi, (efs_ino_t)ino);
    off_t start = lock->l_start;
    off_t end = plock_end(lock);
    struct efs_msg_inode_reply r;
    uint64_t rs, re;
    uint32_t op;
    int rc;

    pthread_mutex_lock(&g_plock_mu);
    struct efs_plock *c = plock_find_conflict(inum, start, end, lock->l_type,
                                              fi ? fi->lock_owner : 0);
    if (c) {
        lock->l_type = c->type;
        lock->l_start = c->start;
        lock->l_len = (c->end == OFF_MAX) ? 0 : (c->end - c->start);
        lock->l_pid = 0;
        pthread_mutex_unlock(&g_plock_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_plock_mu);

    /* Peer locks live on the inode shard, not this client's g_plocks. */
    plock_range_u64(lock, inum, &rs, &re);
    op = EFS_FLOCK_FCNTL | EFS_FLOCK_GETLK;
    op |= (lock->l_type == F_RDLCK) ? EFS_FLOCK_SH : EFS_FLOCK_EX;
    memset(&r, 0, sizeof(r));
    rc = efs_client_rpc_flock_range(g_client.export_id, inum, op,
                                    plock_rpc_owner(fi), rs, re, &r);
    if (rc != EFS_OK)
        return efs_rc_to_errno(rc);
    if (r.inode.nlink == 0)
        lock->l_type = F_UNLCK;
    else {
        lock->l_type = (r.inode.nlink == EFS_FLOCK_SH) ? F_RDLCK : F_WRLCK;
        lock->l_start = (off_t)r.inode.size;
        lock->l_len = (r.inode.ctime == ~(uint64_t)0)
                          ? 0
                          : (off_t)(r.inode.ctime - r.inode.size);
        lock->l_pid = 0;
    }
    return 0;
}

int efs_fuse_setlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
                              struct flock *lock, int sleep)
{
    efs_ino_t inum = efs_file_ino(fi, (efs_ino_t)ino);
    uint64_t owner = fi ? fi->lock_owner : 0;
    uint64_t rs, re;
    off_t start, end;
    uint32_t op;
    int rc;

    plock_range_u64(lock, inum, &rs, &re);
    start = (off_t)rs;
    end = (re == ~(uint64_t)0) ? OFF_MAX : (off_t)re;
    if (lock->l_type == F_UNLCK) {
        op = EFS_FLOCK_UN | EFS_FLOCK_FCNTL;
        rc = efs_client_rpc_flock_range(g_client.export_id, inum, op,
                                        plock_rpc_owner(fi), rs, re, NULL);
        pthread_mutex_lock(&g_plock_mu);
        struct efs_plock **pp = &g_plocks;
        while (*pp) {
            struct efs_plock *p = *pp;
            if (p->ino == inum && p->owner == owner &&
                plock_overlap(p->start, p->end, start, end)) {
                *pp = p->next;
                free(p);
                continue;
            }
            pp = &(*pp)->next;
        }
        /* Blocking setlk waits on this cond instead of returning EAGAIN.
         * Wake it only after the server release has been applied, so the
         * waiter's follow-up GRANT does not see the lock we just dropped. */
        pthread_cond_broadcast(&g_plock_cv);
        pthread_mutex_unlock(&g_plock_mu);
        return efs_rc_to_errno(rc);
    }

    /* F_SETLKW must block in the daemon. Returning EAGAIN here made the
     * kernel fail the syscall at once (fcntl_byte_range_lock: the waiter
     * never acquired after unlock). A non-blocking lock still fails fast. */
    for (;;) {
        pthread_mutex_lock(&g_plock_mu);
        while (plock_find_conflict(inum, start, end, lock->l_type, owner)) {
            if (!sleep) {
                pthread_mutex_unlock(&g_plock_mu);
                return -EAGAIN;
            }
            pthread_cond_wait(&g_plock_cv, &g_plock_mu);
        }
        pthread_mutex_unlock(&g_plock_mu);

        op = EFS_FLOCK_FCNTL;
        op |= (lock->l_type == F_RDLCK) ? EFS_FLOCK_SH : EFS_FLOCK_EX;
        op |= sleep ? EFS_FLOCK_WAIT : EFS_FLOCK_NB;
        rc = efs_client_rpc_flock_range(g_client.export_id, inum, op,
                                        plock_rpc_owner(fi), rs, re, NULL);
        if (rc == EFS_ERR_BUSY || rc == EFS_ERR_AGAIN) {
            if (!sleep)
                return -EAGAIN;
            usleep(2000);
            continue;
        }
        if (rc != EFS_OK)
            return efs_rc_to_errno(rc);

        pthread_mutex_lock(&g_plock_mu);
        if (plock_find_conflict(inum, start, end, lock->l_type, owner)) {
            pthread_mutex_unlock(&g_plock_mu);
            (void)efs_client_rpc_flock_range(g_client.export_id, inum,
                                             EFS_FLOCK_UN | EFS_FLOCK_FCNTL,
                                             plock_rpc_owner(fi), rs, re,
                                             NULL);
            if (!sleep)
                return -EAGAIN;
            continue;
        }
        struct efs_plock *n = calloc(1, sizeof(*n));
        if (!n) {
            pthread_mutex_unlock(&g_plock_mu);
            (void)efs_client_rpc_flock_range(g_client.export_id, inum,
                                             EFS_FLOCK_UN | EFS_FLOCK_FCNTL,
                                             plock_rpc_owner(fi), rs, re,
                                             NULL);
            return -ENOMEM;
        }
        n->ino = inum;
        n->owner = owner;
        n->start = start;
        n->end = end;
        n->type = lock->l_type;
        n->next = g_plocks;
        g_plocks = n;
        pthread_mutex_unlock(&g_plock_mu);
        return 0;
    }
}

int efs_fuse_flock_ino(fuse_ino_t ino, struct fuse_file_info *fi, int op)
{
    efs_ino_t fh = efs_file_ino(fi, (efs_ino_t)ino);
    if (!fi) return -EBADF;
    uint64_t owner = g_client.flock_token ^ fi->lock_owner;
    uint32_t rpc_op = (uint32_t)op;
    if (!(op & (LOCK_UN | LOCK_NB))) rpc_op |= EFS_FLOCK_WAIT;
    int rc;
    if (getenv("EFS_FLOCK_DBG"))
        fprintf(stderr, "flock: ino=%llu op=%u owner=%llu\n",
                (unsigned long long)fh, op, (unsigned long long)owner);
    rc = efs_client_rpc_flock(g_client.export_id, fh, rpc_op, owner);
    if (rc == EFS_ERR_BUSY || rc == EFS_ERR_AGAIN)
        return -EAGAIN;
    return efs_rc_to_errno(rc);
}

/* Parent of a daemonized efs-fuse waits on this until FUSE_INIT has run.
 * fuse_mount() puts the path in /proc/mounts before fuse_loop_mt is reading,
 * so a parent that returned at fuse_daemonize advertised a mount that could
 * not yet serve. -1 when running foreground. */
int g_fuse_ready_wr = -1;

static void efs_fuse_note_serving(void)
{
    int fd = g_fuse_ready_wr;
    if (fd >= 0) {
        g_fuse_ready_wr = -1;
        char c = 'R';
        ssize_t written;
        do { written = write(fd, &c, 1); }
        while (written < 0 && errno == EINTR);
        if (written != 1)
            fprintf(stderr, "fuse readiness notification failed: %s\n",
                    written < 0 ? strerror(errno) : "short write");
        close(fd);
    }
    fprintf(stderr, "fuse serving\n");
    fflush(stderr);
}

int efs_fuse_wait_ready(int rfd, pid_t child, int timeout_ms)
{
    struct pollfd p = { .fd = rfd, .events = POLLIN };
    while (timeout_ms > 0) {
        int slice = timeout_ms > 200 ? 200 : timeout_ms;
        int pr = poll(&p, 1, slice);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr > 0) {
            char c = 0;
            ssize_t n = read(rfd, &c, 1);
            return (n == 1 && c == 'R') ? 0 : -1;
        }
        int st = 0;
        if (waitpid(child, &st, WNOHANG) == child)
            return -1;
        timeout_ms -= slice;
    }
    kill(child, SIGTERM);
    usleep(200000);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return -1;
}

void efs_fuse_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    efs_dcache_init();
    efs_open_retry_start();
    (void)efs_client_view_start();
    /* Timeouts are per lookup/getattr reply (always 0). There is no
     * fuse_config on the low-level API. */
    if (conn) {
        /* libfuse requires this to match the max_read mount option. */
        conn->max_read = 512u << 10;
        uint32_t cs = fuse_chunk_size();
        uint32_t want = (uint32_t)EFS_WRITE_PIPELINE * cs;
        if (want < (1u << 20))
            want = (1u << 20);
        if (want > (16u << 20))
            want = (16u << 20);
        if (conn->max_write == 0 || conn->max_write > want)
            conn->max_write = want;
        if (conn->max_readahead == 0 || conn->max_readahead > (16u << 20))
            conn->max_readahead = 16u << 20;
        if (conn->max_background < 128)
            conn->max_background = 128;
#ifdef FUSE_CAP_ASYNC_READ
        if (conn->capable & FUSE_CAP_ASYNC_READ)
            conn->want |= FUSE_CAP_ASYNC_READ;
#endif
#ifdef FUSE_CAP_PARALLEL_DIROPS
        if (conn->capable & FUSE_CAP_PARALLEL_DIROPS)
            conn->want |= FUSE_CAP_PARALLEL_DIROPS;
#endif
#ifdef FUSE_CAP_POSIX_LOCKS
        if (conn->capable & FUSE_CAP_POSIX_LOCKS)
            conn->want |= FUSE_CAP_POSIX_LOCKS;
#endif
#ifdef FUSE_CAP_FLOCK_LOCKS
        if (conn->capable & FUSE_CAP_FLOCK_LOCKS)
            conn->want |= FUSE_CAP_FLOCK_LOCKS;
#endif
#ifdef FUSE_CAP_ATOMIC_O_TRUNC
        if (conn->capable & FUSE_CAP_ATOMIC_O_TRUNC)
            conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;
#endif
#ifdef FUSE_CAP_ASYNC_DIO
        if (conn->capable & FUSE_CAP_ASYNC_DIO)
            conn->want |= FUSE_CAP_ASYNC_DIO;
#endif
#ifdef FUSE_CAP_WRITEBACK_CACHE
        /* Direct-I/O is the coherence contract; do not let libfuse turn
         * kernel writeback back on. */
        conn->want &= ~FUSE_CAP_WRITEBACK_CACHE;
#endif
#ifdef FUSE_CAP_SPLICE_READ
        /* Off, with no switch (D42). W15.5 asked for splice reads (Sep 29)
         * and the first profile with it (fstor007 ecopy) showed no gain:
         * libfuse splices /dev/fuse into a pipe and the write handler's
         * fuse_buf_copy then read()s the pipe into the dcache buffer, the
         * same kernel->user copy read(/dev/fuse) does, plus one splice per
         * request (24 668 in 67 s, 17 072 of them header-sized). */
        conn->want &= ~FUSE_CAP_SPLICE_READ;
#endif
#ifdef FUSE_CAP_EXPORT_SUPPORT
        /* No NFS re-export (D42, W152 out of alpha): the kernel never
         * builds file handles for an efs mount. */
        conn->want &= ~FUSE_CAP_EXPORT_SUPPORT;
#endif
        if (conn->congestion_threshold < 96)
            conn->congestion_threshold = 96;
    }
    efs_client_stage_set_pin_hooks(efs_client_ino_is_open,
                                   efs_client_ino_has_plock);
    efs_client_enable_meta_batch(4096);
    efs_fuse_note_serving();
}
