#define _GNU_SOURCE
#define FUSE_USE_VERSION 26

#include "client_internal.h"
#include "efs/common.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fuse.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <execinfo.h>

/* Async-signal-safe crash breadcrumb. Uses an alternate signal stack so a
 * stack-overflow SIGSEGV can still report instead of dying silently. */
static char g_efs_fuse_altstack[256 * 1024];

static void efs_fuse_fatal_signal(int sig)
{
    const char *name = "signal";
    if (sig == SIGSEGV)
        name = "SIGSEGV";
    else if (sig == SIGBUS)
        name = "SIGBUS";
    else if (sig == SIGABRT)
        name = "SIGABRT";
    else if (sig == SIGILL)
        name = "SIGILL";
    else if (sig == SIGFPE)
        name = "SIGFPE";
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "efs-fuse: fatal %s (%d) — mount will go ENOTCONN\n", name,
                     sig);
    if (n > 0)
        (void)write(STDERR_FILENO, buf, (size_t)n);
    /* Best-effort stack for kill/fault debugging (may allocate; last resort). */
    {
        void *frames[48];
        int nf = backtrace(frames, 48);
        backtrace_symbols_fd(frames, nf, STDERR_FILENO);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void efs_fuse_install_crash_handlers(void)
{
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_efs_fuse_altstack;
    ss.ss_size = sizeof(g_efs_fuse_altstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) {
        fprintf(stderr, "Warning: sigaltstack failed: %s\n", strerror(errno));
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = efs_fuse_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/* Generate a per-mount inode namespace so concurrent clients never assign the
 * same inode number to different files. The namespace occupies the high bits
 * of the 64-bit ino; the low 40 bits are a per-client counter. */
static int split_parent_name(const char *path, char *name, size_t name_len,
                             struct efs_inode *parent);

/* Virtual per-directory .stats (not a real inode). */
static int path_is_stats(const char *path, struct efs_inode *parent_out)
{
    if (!path)
        return -ENOENT;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, EFS_STATS_NAME) != 0)
        return -ENOENT;
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (parent_out)
        *parent_out = parent;
    return 0;
}

static ino_t stats_synthetic_ino(efs_ino_t parent_ino)
{
    return (ino_t)((1ULL << 62) | (parent_ino & ((1ULL << 62) - 1)));
}

/* du(1) sums st_blocks (512-byte units), not st_size. */
static uint32_t fuse_chunk_size(void)
{
    uint32_t cs = g_client.export.chunk_size;
    return efs_chunk_size_valid(cs) ? cs : EFS_DEFAULT_CHUNK_SIZE;
}

static void stat_set_size_blocks(struct stat *stbuf, uint64_t size)
{
    stbuf->st_size = (off_t)size;
    stbuf->st_blksize = fuse_chunk_size();
    stbuf->st_blocks = (blkcnt_t)((size + 511) / 512);
}

/* Lazy .stats cache. Serving .stats used to run efs_export_ensure_rollups()
 * under g_client.lock on every read/getattr — O(pending writes) under the
 * global lock, so a monitoring loop (watch cat .stats) stalled the write hot
 * path. Stats need not be just-in-time, so we cache the rendered text per
 * directory for a short TTL; cache hits take no g_client.lock at all. The text
 * carries an as_of=<time> line recording when the numbers were computed, so
 * readers can see exactly how fresh they are. Rollups are still applied by the
 * flush/serialize path, so skipped computes here lose nothing. */
#define EFS_STATS_CACHE_N 16
#define EFS_STATS_TEXT   640

struct stats_ent {
    int valid;
    efs_ino_t dir_ino;
    uint64_t computed_ms; /* CLOCK_MONOTONIC ms when computed */
    uint64_t uid, gid;
    uint64_t mtime, atime, ctime;
    int len;
    char text[EFS_STATS_TEXT];
};

static struct stats_ent g_stats_cache[EFS_STATS_CACHE_N];
static pthread_mutex_t g_stats_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t stats_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t stats_ttl_ms(void)
{
    static int ttl = -1; /* benign idempotent race on first init */
    if (ttl < 0) {
        ttl = 1000;
        const char *e = getenv("EFS_STATS_TTL_MS");
        if (e && *e) {
            unsigned long v = strtoul(e, NULL, 10);
            if (v <= 60000)
                ttl = (int)v;
        }
    }
    return (uint32_t)ttl;
}

/* Recompute a stats entry from the export (takes g_client.lock briefly). */
static int stats_compute_locked(struct stats_ent *e, efs_ino_t dir_ino)
{
    pthread_mutex_lock(&g_client.lock);
    efs_export_ensure_rollups(&g_client.export);
    struct efs_inode fresh;
    int grc = efs_export_get_inode(&g_client.export, dir_ino, &fresh);
    pthread_mutex_unlock(&g_client.lock);
    if (grc != 0)
        return -ENOENT;

    int n = efs_export_format_stats(&fresh, e->text, EFS_STATS_TEXT - 48);
    if (n < 0)
        return -EIO;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int m = snprintf(e->text + n, EFS_STATS_TEXT - n, "as_of=%llu.%09lu\n",
                     (unsigned long long)ts.tv_sec, (unsigned long)ts.tv_nsec);
    if (m > 0)
        n += m;
    e->len = n;
    e->uid = fresh.uid;
    e->gid = fresh.gid;
    e->mtime = fresh.mtime;
    e->atime = fresh.atime;
    e->ctime = fresh.ctime;
    e->dir_ino = dir_ino;
    e->computed_ms = stats_now_ms();
    e->valid = 1;
    return 0;
}

/* Return a fresh-or-cached stats snapshot for dir_ino. Cache hits take no
 * g_client.lock; only a miss/stale recomputes (and only once per TTL). */
static int stats_snapshot(efs_ino_t dir_ino, struct stats_ent *out)
{
    uint64_t now = stats_now_ms();
    uint32_t ttl = stats_ttl_ms();

    pthread_mutex_lock(&g_stats_cache_mu);
    for (int i = 0; i < EFS_STATS_CACHE_N; i++) {
        struct stats_ent *e = &g_stats_cache[i];
        if (e->valid && e->dir_ino == dir_ino && now - e->computed_ms < ttl) {
            *out = *e;
            pthread_mutex_unlock(&g_stats_cache_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_stats_cache_mu);

    struct stats_ent tmp;
    memset(&tmp, 0, sizeof(tmp));
    int rc = stats_compute_locked(&tmp, dir_ino);
    if (rc != 0)
        return rc;

    pthread_mutex_lock(&g_stats_cache_mu);
    int slot = -1, oldest = 0;
    uint64_t oldest_ms = ~0ULL;
    for (int i = 0; i < EFS_STATS_CACHE_N; i++) {
        struct stats_ent *e = &g_stats_cache[i];
        if (!e->valid || e->dir_ino == dir_ino) {
            slot = i;
            break;
        }
        if (e->computed_ms < oldest_ms) {
            oldest_ms = e->computed_ms;
            oldest = i;
        }
    }
    if (slot < 0)
        slot = oldest;
    g_stats_cache[slot] = tmp;
    pthread_mutex_unlock(&g_stats_cache_mu);

    *out = tmp;
    return 0;
}

static int stats_fill_stat(const struct efs_inode *parent, struct stat *stbuf)
{
    struct stats_ent e;
    int rc = stats_snapshot(parent->ino, &e);
    if (rc != 0)
        return rc;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = stats_synthetic_ino(parent->ino);
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stat_set_size_blocks(stbuf, (uint64_t)e.len);
    stbuf->st_uid = (uid_t)e.uid;
    stbuf->st_gid = (gid_t)e.gid;
    stbuf->st_mtim.tv_sec = (time_t)e.mtime;
    stbuf->st_atim.tv_sec = (time_t)e.atime;
    stbuf->st_ctim.tv_sec = (time_t)e.ctime;
    return 0;
}

static void efs_client_setup_ino_namespace(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srandom((unsigned)(ts.tv_nsec ^ ts.tv_sec ^ (getpid() << 8)));
    uint64_t tag = ((uint64_t)getpid() << 32) ^
                   ((uint64_t)ts.tv_sec << 12) ^
                   (uint64_t)ts.tv_nsec ^
                   ((uint64_t)random() << 20);
    tag &= 0x7FFFFF; /* 23 bits, keeps the ino positive */
    if (tag == 0)
        tag = 1;
    g_client.ino_namespace = tag << 40;
    g_client.ino_counter = 1;
}

static int efs_fuse_getattr(const char *path, struct stat *stbuf)
{
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0)
        return stats_fill_stat(&parent, stbuf);

    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = ino.ino;
    stbuf->st_mode = ino.mode;
    stbuf->st_nlink = ino.nlink;
    stat_set_size_blocks(stbuf, ino.size);
    stbuf->st_uid = ino.uid;
    stbuf->st_gid = ino.gid;
    stbuf->st_mtim.tv_sec = (time_t)ino.mtime;
    stbuf->st_mtim.tv_nsec = (long)ino.mtime_nsec;
    stbuf->st_atim.tv_sec = (time_t)ino.atime;
    stbuf->st_atim.tv_nsec = 0;
    stbuf->st_ctim.tv_sec = (time_t)ino.ctime;
    stbuf->st_ctim.tv_nsec = 0;
    return 0;
}

struct readdir_ent {
    char name[EFS_MAX_NAME];
    struct stat st;
};

struct readdir_collect_arg {
    struct readdir_ent *ents;
    size_t count;
    size_t cap;
};

static int readdir_collect_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    struct readdir_collect_arg *a = arg;
    struct efs_inode *child = &ex->inodes[slot];
    if (a->count >= a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 16;
        struct readdir_ent *n = realloc(a->ents, ncap * sizeof(*n));
        if (!n)
            return -ENOMEM;
        a->ents = n;
        a->cap = ncap;
    }
    struct readdir_ent *e = &a->ents[a->count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, child->name, EFS_MAX_NAME - 1);
    e->st.st_ino = child->ino;
    e->st.st_mode = child->mode;
    return 0;
}

static int efs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi)
{
    (void)offset;
    (void)fi;

    struct efs_inode parent;
    int rc = efs_client_lookup(path, &parent);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;

    /* Snapshot under the lock; call filler unlocked so a same-thread
     * getattr re-entry cannot double-lock g_client.lock.
     * .stats is lookup-only (getattr/open/read by explicit path) and is
     * intentionally omitted from readdir so ls of the directory hides it. */
    struct readdir_collect_arg col = {0};
    pthread_mutex_lock(&g_client.lock);
    rc = efs_export_foreach_child(&g_client.export, parent.ino,
                                  readdir_collect_cb, &col);
    pthread_mutex_unlock(&g_client.lock);
    if (rc != 0) {
        free(col.ents);
        return rc;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (size_t i = 0; i < col.count; i++)
        filler(buf, col.ents[i].name, &col.ents[i].st, 0);
    free(col.ents);
    return 0;
}

static int efs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0) {
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        return 0;
    }
    struct efs_inode ino;
    return efs_client_lookup(path, &ino) == 0 ? 0 : -ENOENT;
}

static void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path);

static int efs_fuse_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0) {
        struct stats_ent e;
        if (stats_snapshot(parent.ino, &e) != 0)
            return -ENOENT;
        if (offset >= e.len)
            return 0;
        size_t avail = (size_t)e.len - (size_t)offset;
        if (avail > size)
            avail = size;
        memcpy(buf, e.text + offset, avail);
        return (int)avail;
    }

    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    size_t got = 0;
    rc = efs_client_read(ino.ino, (uint64_t)offset, size, buf, &got);
    if (rc != 0) {
        efs_fuse_log_err("read", rc, ino.ino, (uint64_t)offset, size, path);
        return -EIO;
    }
    return (int)got;
}

/* Userspace writeback: ACK the FUSE write after a bounce copy and let a
 * small worker pool issue overlapping write_no_replicate calls. Single-stream
 * dd+conv=fsync still waits for durability on fsync/release, but PUTs from
 * consecutive syscalls overlap — needed to approach store-bench throughput. */
#define EFS_WB_DEPTH   32
#define EFS_WB_WORKERS 4

struct efs_wb_job {
    efs_ino_t ino;
    uint64_t offset;
    size_t size;
    char *buf;
};

/* Per-ino write serialization. WB workers run jobs concurrently, but two jobs
 * touching the same inode (overlapping partial chunks) must not RMW the same
 * chunk concurrently or they lose updates. Striped locks keep parallelism
 * across inodes while serializing within one. */
#define EFS_WB_INO_STRIPES 64
static pthread_mutex_t g_wb_ino_mu[EFS_WB_INO_STRIPES];
static int g_wb_ino_mu_ready;
static pthread_mutex_t g_wb_ino_init_mu = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *efs_wb_ino_lock(efs_ino_t ino)
{
    if (!g_wb_ino_mu_ready) {
        pthread_mutex_lock(&g_wb_ino_init_mu);
        if (!g_wb_ino_mu_ready) {
            for (int i = 0; i < EFS_WB_INO_STRIPES; i++)
                pthread_mutex_init(&g_wb_ino_mu[i], NULL);
            __atomic_store_n(&g_wb_ino_mu_ready, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_wb_ino_init_mu);
    }
    return &g_wb_ino_mu[(uint64_t)ino % EFS_WB_INO_STRIPES];
}

static struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t idle;
    struct efs_wb_job q[EFS_WB_DEPTH];
    int head, tail, count, inflight;
    int err;
    int ready;
    int shutdown;
    pthread_t workers[EFS_WB_WORKERS];
} g_wb = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
};

static void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path)
{
    if (path && path[0]) {
        fprintf(stderr,
                "efs-fuse %s: %s (efs_rc=%d) path=%s ino=%llu off=%llu len=%zu\n",
                where, efs_strerror(efs_rc), efs_rc, path,
                (unsigned long long)ino, (unsigned long long)offset, size);
    } else {
        fprintf(stderr,
                "efs-fuse %s: %s (efs_rc=%d) ino=%llu off=%llu len=%zu\n",
                where, efs_strerror(efs_rc), efs_rc,
                (unsigned long long)ino, (unsigned long long)offset, size);
    }
    fflush(stderr);
}

static void *efs_wb_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_wb.mu);
        while (g_wb.count == 0 && !g_wb.shutdown)
            pthread_cond_wait(&g_wb.not_empty, &g_wb.mu);
        if (g_wb.shutdown && g_wb.count == 0) {
            pthread_mutex_unlock(&g_wb.mu);
            return NULL;
        }
        struct efs_wb_job job = g_wb.q[g_wb.head];
        g_wb.head = (g_wb.head + 1) % EFS_WB_DEPTH;
        g_wb.count--;
        g_wb.inflight++;
        pthread_cond_signal(&g_wb.not_full);
        pthread_mutex_unlock(&g_wb.mu);

        /* Serialize same-ino writebacks: partial-chunk RMW is not atomic, so
         * two overlapping WB jobs on one file would otherwise lose updates. */
        pthread_mutex_t *ilock = efs_wb_ino_lock(job.ino);
        pthread_mutex_lock(ilock);
        int rc = efs_client_write_no_replicate(job.ino, job.offset, job.size,
                                               job.buf);
        pthread_mutex_unlock(ilock);
        free(job.buf);

        pthread_mutex_lock(&g_wb.mu);
        if (rc != EFS_OK) {
            if (g_wb.err == EFS_OK) {
                g_wb.err = rc;
                /* Log outside? hold lock briefly — fprintf is fine for errors. */
                efs_fuse_log_err("writeback", rc, job.ino, job.offset, job.size,
                                 NULL);
            } else {
                /* Follow-on failures after the first sticky error. */
                fprintf(stderr,
                        "efs-fuse writeback: also failed %s (efs_rc=%d) "
                        "ino=%llu off=%llu len=%zu (sticky_err=%s)\n",
                        efs_strerror(rc), rc,
                        (unsigned long long)job.ino,
                        (unsigned long long)job.offset, job.size,
                        efs_strerror(g_wb.err));
                fflush(stderr);
            }
        }
        g_wb.inflight--;
        if (g_wb.count == 0 && g_wb.inflight == 0)
            pthread_cond_broadcast(&g_wb.idle);
        pthread_mutex_unlock(&g_wb.mu);
    }
}

static int efs_wb_ensure(void)
{
    if (g_wb.ready)
        return 0;
    pthread_mutex_lock(&g_wb.mu);
    if (!g_wb.ready) {
        for (int i = 0; i < EFS_WB_WORKERS; i++) {
            if (pthread_create(&g_wb.workers[i], NULL, efs_wb_thread, NULL) != 0) {
                g_wb.shutdown = 1;
                pthread_cond_broadcast(&g_wb.not_empty);
                pthread_mutex_unlock(&g_wb.mu);
                for (int j = 0; j < i; j++)
                    pthread_join(g_wb.workers[j], NULL);
                g_wb.shutdown = 0;
                return -1;
            }
        }
        g_wb.ready = 1;
    }
    pthread_mutex_unlock(&g_wb.mu);
    return 0;
}

/* Enqueue a WB job taking ownership of an already-allocated buffer. */
static int efs_wb_enqueue_owned(efs_ino_t ino, uint64_t offset, size_t size,
                                char *copy)
{
    if (efs_wb_ensure() != 0) {
        free(copy);
        return EFS_ERR_NOMEM;
    }
    pthread_mutex_lock(&g_wb.mu);
    while (g_wb.count == EFS_WB_DEPTH && g_wb.err == EFS_OK)
        pthread_cond_wait(&g_wb.not_full, &g_wb.mu);
    if (g_wb.err != EFS_OK) {
        int err = g_wb.err;
        pthread_mutex_unlock(&g_wb.mu);
        free(copy);
        return err;
    }
    g_wb.q[g_wb.tail].ino = ino;
    g_wb.q[g_wb.tail].offset = offset;
    g_wb.q[g_wb.tail].size = size;
    g_wb.q[g_wb.tail].buf = copy;
    g_wb.tail = (g_wb.tail + 1) % EFS_WB_DEPTH;
    g_wb.count++;
    pthread_cond_signal(&g_wb.not_empty);
    pthread_mutex_unlock(&g_wb.mu);
    return EFS_OK;
}

static int efs_wb_enqueue(efs_ino_t ino, uint64_t offset, size_t size,
                          const char *buf)
{
    if (size == 0)
        return 0;
    if (efs_wb_ensure() != 0)
        return EFS_ERR_NOMEM;
    char *copy = malloc(size);
    if (!copy)
        return EFS_ERR_NOMEM;
    memcpy(copy, buf, size);
    return efs_wb_enqueue_owned(ino, offset, size, copy);
}

static int efs_wb_sync(void)
{
    if (!g_wb.ready)
        return EFS_OK;
    pthread_mutex_lock(&g_wb.mu);
    while (g_wb.count > 0 || g_wb.inflight > 0)
        pthread_cond_wait(&g_wb.idle, &g_wb.mu);
    int err = g_wb.err;
    g_wb.err = EFS_OK;
    pthread_mutex_unlock(&g_wb.mu);
    return err;
}

static int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    rc = efs_wb_enqueue(ino.ino, (uint64_t)offset, size, buf);
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("write", rc, ino.ino, (uint64_t)offset, size, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("write", rc, ino.ino, (uint64_t)offset, size, path);
        return -EIO;
    }
    return (int)size;
}

/* write_buf: libfuse hands us the kernel's bufvec directly instead of first
 * flattening it with a full-data memmove to call .write. We copy the vec
 * straight into the writeback job buffer — one copy instead of two. That
 * libfuse flatten was the top single-stream CPU cost (~68% memmove). */
static int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    size_t size = fuse_buf_size(buf);
    if (size == 0)
        return 0;
    char *copy = malloc(size);
    if (!copy)
        return -ENOMEM;
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT(size);
    dst.buf[0].mem = copy;
    if (fuse_buf_copy(&dst, buf, FUSE_BUF_NO_SPLICE) < 0) {
        free(copy);
        return -EIO;
    }

    rc = efs_wb_enqueue_owned(ino.ino, (uint64_t)offset, size, copy);
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("write_buf", rc, ino.ino, (uint64_t)offset, size, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("write_buf", rc, ino.ino, (uint64_t)offset, size, path);
        return -EIO;
    }
    return (int)size;
}

static void efs_fuse_sync_rollups(void)
{
    pthread_mutex_lock(&g_client.lock);
    efs_export_ensure_rollups(&g_client.export);
    pthread_mutex_unlock(&g_client.lock);
}

static int efs_fuse_fsync(const char *path, int isdatasync,
                          struct fuse_file_info *fi)
{
    (void)path;
    (void)isdatasync;
    (void)fi;
    int rc = efs_wb_sync();
    efs_fuse_sync_rollups();
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("fsync", rc, 0, 0, 0, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("fsync", rc, 0, 0, 0, path);
        return -EIO;
    }
    efs_client_note_meta_change(1);
    return 0;
}

static int efs_fuse_flush(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    int rc = efs_wb_sync();
    efs_fuse_sync_rollups();
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("flush", rc, 0, 0, 0, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("flush", rc, 0, 0, 0, path);
        return -EIO;
    }
    return 0;
}

static int efs_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return -EEXIST;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFREG | mode,
                                      ctx->uid, ctx->gid);
    if (ino == 0)
        return -EEXIST;
    return 0;
}

static int efs_fuse_mkdir(const char *path, mode_t mode)
{
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return -EEXIST;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFDIR | mode,
                                      ctx->uid, ctx->gid);
    if (ino == 0)
        return -EEXIST;
    return 0;
}

static int efs_fuse_unlink(const char *path)
{
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return -EACCES;

    return efs_client_unlink(parent.ino, name, false) == 0 ? 0 : -EIO;
}

static int efs_fuse_rmdir(const char *path)
{
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;

    return efs_client_unlink(parent.ino, name, true) == 0 ? 0 : -EIO;
}

static int efs_fuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));

    pthread_mutex_lock(&g_client.lock);

    uint64_t used_logical = 0;
    for (uint64_t i = 0; i < g_client.export.inode_count; i++) {
        struct efs_inode *ino = &g_client.export.inodes[i];
        if (!efs_mode_is_dir(ino->mode))
            used_logical += ino->size;
    }

    uint64_t min_quota = 0;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        uint64_t q = g_client.nodes[i].quota;
        if (q > 0 && (min_quota == 0 || q < min_quota))
            min_quota = q;
    }

    /* 2+1 erasure coding: 3 fragments per chunk, 1.5x physical for 1x logical.
       The cluster is limited by the smallest node because every chunk places
       one fragment on each node. Logical capacity = 2 * min_quota. */
    uint64_t total_logical = 0;
    if (min_quota > 0 && g_client.node_count > 0)
        total_logical = min_quota * 2;

    uint64_t avail = (total_logical > used_logical) ? total_logical - used_logical : 0;

    stbuf->f_bsize = 512;
    stbuf->f_frsize = 512;
    stbuf->f_blocks = total_logical / 512;
    stbuf->f_bfree = avail / 512;
    stbuf->f_bavail = avail / 512;
    stbuf->f_files = g_client.export.inode_count;
    stbuf->f_ffree = 0;
    stbuf->f_namemax = EFS_MAX_NAME;

    pthread_mutex_unlock(&g_client.lock);
    return 0;
}

static int efs_rc_to_errno(int rc)
{
    switch (rc) {
    case EFS_OK:            return 0;
    case EFS_ERR_NOT_FOUND: return -ENOENT;
    case EFS_ERR_EXIST:     return -EEXIST;
    case EFS_ERR_NOMEM:     return -ENOMEM;
    case EFS_ERR_INVAL:     return -EINVAL;
    case EFS_ERR_QUOTA:     return -ENOSPC;
    case EFS_ERR_NOT_EMPTY: return -ENOTEMPTY;
    case EFS_ERR_NO_QUORUM:
    case EFS_ERR_NET:
    default:                return -EIO;
    }
}

static int efs_fuse_chmod(const char *path, mode_t mode)
{
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    /* Mode is applied in-memory first; failure here is almost always a
     * batched metadata flush (see efs_client_note_meta_change), not chmod. */
    rc = efs_client_chmod(ino.ino, mode);
    if (rc != 0) {
        fprintf(stderr, "chmod %s failed: %s\n", path, efs_strerror(rc));
        fflush(stderr);
        return efs_rc_to_errno(rc);
    }
    return 0;
}

static int efs_fuse_chown(const char *path, uid_t uid, gid_t gid)
{
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_client_chown(ino.ino, uid, gid) != 0)
        return -EIO;
    return 0;
}

static int split_parent_name(const char *path, char *name, size_t name_len,
                             struct efs_inode *parent)
{
    char *p = strdup(path);
    if (!p)
        return -ENOMEM;
    char *base = strrchr(p, '/');
    if (!base) {
        free(p);
        return -EINVAL;
    }
    *base = '\0';
    base++;
    strncpy(name, base, name_len - 1);
    name[name_len - 1] = '\0';
    int rc = efs_client_lookup(p[0] ? p : "/", parent);
    free(p);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(parent->mode))
        return -ENOTDIR;
    return 0;
}

static int efs_fuse_symlink(const char *link, const char *path)
{
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFLNK | 0777,
                                      ctx->uid, ctx->gid);
    if (ino == 0)
        return -EEXIST;

    size_t len = strlen(link);
    if (len > 0) {
        rc = efs_client_write(ino, 0, len, link);
        if (rc != 0)
            return -EIO;
    } else {
        /* Empty target: still set size 0 explicitly. */
        if (efs_client_truncate(ino, 0) != 0)
            return -EIO;
    }
    return 0;
}

static int efs_fuse_readlink(const char *path, char *buf, size_t size)
{
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (!efs_mode_is_lnk(ino.mode))
        return -EINVAL;
    if (size == 0)
        return -EINVAL;

    size_t got = 0;
    size_t want = size - 1;
    if (want > ino.size)
        want = (size_t)ino.size;
    if (want > 0) {
        rc = efs_client_read(ino.ino, 0, want, buf, &got);
        if (rc != 0)
            return -EIO;
    }
    buf[got] = '\0';
    return 0;
}

static int efs_fuse_link(const char *from, const char *to)
{
    if (path_is_stats(from, NULL) == 0 || path_is_stats(to, NULL) == 0)
        return -EACCES;
    struct efs_inode src;
    if (efs_client_lookup(from, &src) != 0)
        return -ENOENT;
    if (efs_mode_is_dir(src.mode))
        return -EPERM;

    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(to, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return -EACCES;

    rc = efs_client_link(src.ino, parent.ino, name);
    if (rc == EFS_ERR_EXIST)
        return -EEXIST;
    if (rc != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_utime(const char *path, struct utimbuf *ubuf)
{
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;

    uint64_t mtime;
    if (ubuf)
        mtime = (uint64_t)ubuf->modtime;
    else
        mtime = (uint64_t)time(NULL);

    if (efs_client_utime(ino.ino, mtime) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_utimens(const char *path, const struct timespec tv[2])
{
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;

    /* tv[0]=atime, tv[1]=mtime; honor UTIME_OMIT / UTIME_NOW. */
    if (tv && tv[0].tv_nsec != UTIME_OMIT) {
        uint64_t asec;
        if (tv[0].tv_nsec == UTIME_NOW) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            asec = (uint64_t)now.tv_sec;
        } else {
            asec = (uint64_t)tv[0].tv_sec;
        }
        if (efs_client_set_atime(ino.ino, asec) != 0)
            return -EIO;
    }

    uint64_t sec;
    uint32_t nsec;
    if (!tv || tv[1].tv_nsec == UTIME_OMIT) {
        return 0;
    } else if (tv[1].tv_nsec == UTIME_NOW) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        sec = (uint64_t)now.tv_sec;
        nsec = (uint32_t)now.tv_nsec;
    } else {
        sec = (uint64_t)tv[1].tv_sec;
        nsec = (uint32_t)tv[1].tv_nsec;
        if (nsec >= 1000000000u)
            nsec = 0;
    }

    if (efs_client_utimens(ino.ino, sec, nsec) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_truncate(const char *path, off_t size)
{
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    if (efs_client_truncate(ino.ino, (uint64_t)size) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    int rc = efs_wb_sync();
    efs_fuse_sync_rollups();
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("release", rc, 0, 0, 0, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("release", rc, 0, 0, 0, path);
        return -EIO;
    }
    /* Coalesced: only flushes every meta_batch_ops releases/creates. */
    efs_client_note_meta_change(0);
    return 0;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
    (void)efs_wb_sync();
    if (g_wb.ready) {
        pthread_mutex_lock(&g_wb.mu);
        g_wb.shutdown = 1;
        pthread_cond_broadcast(&g_wb.not_empty);
        pthread_mutex_unlock(&g_wb.mu);
        for (int i = 0; i < EFS_WB_WORKERS; i++)
            pthread_join(g_wb.workers[i], NULL);
        g_wb.ready = 0;
        g_wb.shutdown = 0;
    }
    /* Final flush so the last dirty batch is not lost on unmount. */
    efs_client_note_meta_change(1);
    efs_client_shutdown();
}

static int efs_fuse_rename(const char *from, const char *to)
{
    if (path_is_stats(from, NULL) == 0 || path_is_stats(to, NULL) == 0)
        return -EACCES;
    struct efs_inode src;
    if (efs_client_lookup(from, &src) != 0)
        return -ENOENT;

    char name[EFS_MAX_NAME];
    struct efs_inode dst_parent;
    int rc = split_parent_name(to, name, sizeof(name), &dst_parent);
    if (rc != 0)
        return rc;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return -EACCES;

    if (efs_client_rename(src.ino, dst_parent.ino, name) != 0)
        return -EIO;
    return 0;
}

static void *efs_fuse_init(struct fuse_conn_info *conn)
{
    /* Ask the kernel for large writes so we are not forced into a 4 KiB
     * RMW of every 128 KiB chunk. */
    if (conn) {
        if (conn->capable & FUSE_CAP_BIG_WRITES)
            conn->want |= FUSE_CAP_BIG_WRITES;
        /* Prefer up to pipeline × export chunk so one FUSE write saturates
         * the PUT worker pool. Kernel may clamp lower. */
        {
            uint32_t cs = fuse_chunk_size();
            uint32_t want = (uint32_t)EFS_WRITE_PIPELINE * cs;
            if (want < (1u << 20))
                want = (1u << 20);
            if (want > (16u << 20))
                want = (16u << 20);
            if (conn->max_write == 0 || conn->max_write > want)
                conn->max_write = want;
        }
#ifdef FUSE_CAP_WRITEBACK_CACHE
        if (conn->capable & FUSE_CAP_WRITEBACK_CACHE)
            conn->want |= FUSE_CAP_WRITEBACK_CACHE;
#endif
    }
    /* Coalesce metadata PUTs so bulk creates are not O(n^2) full-metadata
     * syncs. Override with EFS_META_BATCH_OPS for heavy profiling loads. */
    uint32_t batch = 4096;
    const char *env = getenv("EFS_META_BATCH_OPS");
    if (env && *env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 1000000)
            batch = (uint32_t)v;
    }
    efs_client_enable_meta_batch(batch);
    return NULL;
}

static struct fuse_operations efs_ops = {
    .getattr = efs_fuse_getattr,
    .statfs  = efs_fuse_statfs,
    .readdir = efs_fuse_readdir,
    .open    = efs_fuse_open,
    .read    = efs_fuse_read,
    .write   = efs_fuse_write,
    .write_buf = efs_fuse_write_buf,
    .flush   = efs_fuse_flush,
    .fsync   = efs_fuse_fsync,
    .create  = efs_fuse_create,
    .mkdir   = efs_fuse_mkdir,
    .unlink   = efs_fuse_unlink,
    .rmdir    = efs_fuse_rmdir,
    .chmod    = efs_fuse_chmod,
    .chown    = efs_fuse_chown,
    .utime    = efs_fuse_utime,
    .utimens  = efs_fuse_utimens,
    .truncate = efs_fuse_truncate,
    .rename   = efs_fuse_rename,
    .symlink  = efs_fuse_symlink,
    .readlink = efs_fuse_readlink,
    .link     = efs_fuse_link,
    .release  = efs_fuse_release,
    .init     = efs_fuse_init,
    .destroy  = efs_fuse_destroy,
    .flag_utime_omit_ok = 1,
};

static int parse_addr(const char *str, char *host, size_t host_len, uint16_t *port)
{
    const char *colon = strrchr(str, ':');
    if (!colon)
        return -1;
    size_t hlen = (size_t)(colon - str);
    if (hlen >= host_len)
        return -1;
    memcpy(host, str, hlen);
    host[hlen] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[8192];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static pid_t g_perf_pid = -1;

static pid_t start_perf_recorder(pid_t target, const char *perf_path)
{
    char perf_dir[8192];
    strncpy(perf_dir, perf_path, sizeof(perf_dir) - 1);
    perf_dir[sizeof(perf_dir) - 1] = '\0';
    char *slash = strrchr(perf_dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(perf_dir);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork perf");
        return -1;
    }
    if (pid == 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)target);
        /* -g uses frame pointers (binaries are built with
         * -fno-omit-frame-pointer) so perf report can show call stacks. */
        execlp("perf", "perf", "record", "-g", "-F", "999", "-p", pid_str, "-o",
               perf_path, NULL);
        perror("exec perf");
        _exit(1);
    }
    return pid;
}

static void stop_perf_recorder(void)
{
    if (g_perf_pid > 0) {
        kill(g_perf_pid, SIGTERM);
        /* Wait (up to ~5s) for perf to finalize and write perf.data. */
        for (int i = 0; i < 50; i++) {
            if (waitpid(g_perf_pid, NULL, WNOHANG) == g_perf_pid)
                break;
            usleep(100000);
        }
        kill(g_perf_pid, SIGKILL);
        waitpid(g_perf_pid, NULL, 0);
        g_perf_pid = -1;
    }
}

int main(int argc, char **argv)
{
    /* Line-buffer logs even when stdout is a pipe (client.sh | tee). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    efs_fuse_install_crash_handlers();

    int perf = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--perf") == 0) {
            perf = 1;
            break;
        }
    }

    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <node1:port> [<node2:port> ...] <export-name> <mountpoint> [fuse options] [--perf]\n"
                "       A single server address is enough; the client will discover the rest.\n",
                argv[0]);
        return 1;
    }

    /* Server addresses are the first arguments (they contain a colon).
       The first argument without a colon is the export name. */
    int arg_idx = 1;
    const char *nodes[EFS_MAX_NODES];
    uint32_t node_count = 0;
    while (arg_idx < argc && strchr(argv[arg_idx], ':') != NULL &&
           node_count < EFS_MAX_NODES) {
        nodes[node_count++] = argv[arg_idx++];
    }

    if (node_count == 0 || arg_idx >= argc) {
        fprintf(stderr, "Missing export name and mountpoint\n");
        return 1;
    }

    const char *export_name = argv[arg_idx++];
    if (arg_idx >= argc) {
        fprintf(stderr, "Missing mountpoint\n");
        return 1;
    }
    const char *mountpoint = argv[arg_idx++];

    efs_client_init_nodes(&g_client, nodes, node_count);
    strncpy(g_client.export_name, export_name, EFS_MAX_NAME - 1);
    g_client.export_id = 1;
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    efs_client_setup_ino_namespace();

    if (node_count == 1) {
        char host[64];
        uint16_t port = 0;
        if (parse_addr(nodes[0], host, sizeof(host), &port) != 0 ||
            efs_client_discover_nodes(&g_client, host, port) != 0) {
            fprintf(stderr, "Could not discover cluster from %s\n", nodes[0]);
            return 1;
        }
        printf("Discovered %u cluster nodes from %s\n", g_client.node_count, nodes[0]);
    }

    /* Fetch initial metadata from one of the nodes. */
    printf("fetching metadata...\n");
    fflush(stdout);
    int rc = -1;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        printf("  try %s:%u\n", g_client.nodes[i].addr, g_client.nodes[i].port);
        fflush(stdout);
        rc = efs_client_fetch_metadata(g_client.nodes[i].addr, g_client.nodes[i].port);
        if (rc == 0)
            break;
        printf("  fetch failed rc=%d (%s)\n", rc, efs_strerror(rc));
        fflush(stdout);
    }
    if (rc != 0) {
        fprintf(stderr,
                "Could not fetch metadata from any node (%s).\n"
                "status/list-exports can still work while mount fails if the\n"
                "export root exists but its 2+1 meta table pages cannot be\n"
                "reconstructed (missing/corrupt fragments on peers).\n",
                efs_strerror(rc));
        return 1;
    }
    /* PUTs carry export_id; do not leave the hardcoded 1 if meta says otherwise. */
    g_client.export_id = g_client.export.id ? g_client.export.id : 1;
    if (g_client.export.name[0] &&
        strcmp(g_client.export.name, g_client.export_name) != 0) {
        fprintf(stderr,
                "Warning: mounted as '%s' but server export[0] is '%s' (id=%u)\n",
                g_client.export_name, g_client.export.name, g_client.export_id);
    }
    printf("export id=%u name=%s\n", g_client.export_id,
           g_client.export.name[0] ? g_client.export.name : g_client.export_name);
    fflush(stdout);

    char *fuse_argv[64];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    /* Prefer large writes so FUSE does not chop every write into 4 KiB and
     * force a chunk RMW per call. writeback_cache is requested via
     * FUSE_CAP_WRITEBACK_CACHE in efs_fuse_init when the kernel supports it
     * (this cluster's libfuse rejects -o writeback_cache). */
    fuse_argv[fuse_argc++] = "-o";
    /* Cache attr/entry for 1s in the kernel: with timeout=0 every access
     * round-trips FUSE getattr/lookup into userspace (and our lookup path
     * takes g_client.lock + strdup). efs serves attrs from the in-memory meta
     * cache anyway, so a short TTL is safe for single-mount workloads and a
     * large win for metadata-heavy trees. */
    fuse_argv[fuse_argc++] =
        "big_writes,max_write=16777216,max_readahead=16777216,use_ino,attr_timeout=1,entry_timeout=1,ac_attr_timeout=1";
    while (arg_idx < argc && fuse_argc < 63) {
        if (strcmp(argv[arg_idx], "--perf") == 0) {
            arg_idx++;
            continue;
        }
        fuse_argv[fuse_argc++] = argv[arg_idx++];
    }
    fuse_argv[fuse_argc] = NULL;

    char perf_path[512];
    if (perf) {
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(perf_path, sizeof(perf_path), "%s", pp);
        else
            snprintf(perf_path, sizeof(perf_path), "/tmp/efs-fuse-perf-%d/perf.data", (int)getpid());
        g_perf_pid = start_perf_recorder(getpid(), perf_path);
        if (g_perf_pid < 0) {
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        }
    }

    int ret = fuse_main(fuse_argc, fuse_argv, &efs_ops, NULL);
    stop_perf_recorder();
    /* destroy() already shut down on clean unmount; call again is a no-op. */
    efs_client_shutdown();
    return ret;
}
