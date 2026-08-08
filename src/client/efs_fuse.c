#define _GNU_SOURCE
#define FUSE_USE_VERSION 26

#include "client_internal.h"
#include "efs/common.h"
#include "efs/network.h"
#include "efs/numa_locality.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void stat_set_size_blocks(struct stat *stbuf, uint64_t size)
{
    stbuf->st_size = (off_t)size;
    stbuf->st_blksize = EFS_CHUNK_SIZE;
    stbuf->st_blocks = (blkcnt_t)((size + 511) / 512);
}

static int stats_fill_stat(const struct efs_inode *parent, struct stat *stbuf)
{
    char text[512];
    int n = efs_export_format_stats(parent, text, sizeof(text));
    if (n < 0)
        return -EIO;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = stats_synthetic_ino(parent->ino);
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stat_set_size_blocks(stbuf, (uint64_t)n);
    stbuf->st_uid = parent->uid;
    stbuf->st_gid = parent->gid;
    stbuf->st_mtim.tv_sec = (time_t)parent->mtime;
    stbuf->st_atim.tv_sec = (time_t)parent->atime;
    stbuf->st_ctim.tv_sec = (time_t)parent->ctime;
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
    if (path_is_stats(path, &parent) == 0) {
        pthread_mutex_lock(&g_client.lock);
        struct efs_inode fresh;
        int grc = efs_export_get_inode(&g_client.export, parent.ino, &fresh);
        pthread_mutex_unlock(&g_client.lock);
        if (grc != 0)
            return -ENOENT;
        return stats_fill_stat(&fresh, stbuf);
    }

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

static int efs_fuse_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0) {
        char text[512];
        pthread_mutex_lock(&g_client.lock);
        struct efs_inode fresh;
        int grc = efs_export_get_inode(&g_client.export, parent.ino, &fresh);
        pthread_mutex_unlock(&g_client.lock);
        if (grc != 0)
            return -ENOENT;
        int n = efs_export_format_stats(&fresh, text, sizeof(text));
        if (n < 0)
            return -EIO;
        if (offset >= n)
            return 0;
        size_t avail = (size_t)n - (size_t)offset;
        if (avail > size)
            avail = size;
        memcpy(buf, text + offset, avail);
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
    if (rc != 0)
        return -EIO;
    return (int)got;
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

    rc = efs_client_write_no_replicate(ino.ino, (uint64_t)offset, size, buf);
    if (rc == EFS_ERR_QUOTA)
        return -ENOSPC;
    if (rc != 0)
        return -EIO;
    return (int)size;
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
    (void)path;
    (void)fi;
    /* Coalesced: only flushes every meta_batch_ops releases/creates. */
    efs_client_note_meta_change(0);
    return 0;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
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
        /* Prefer up to 4 MiB so one FUSE write covers a full write pipeline
         * (16 × 128 KiB). Kernel may clamp lower (often 1 MiB). */
        if (conn->max_write == 0 || conn->max_write > (4u << 20))
            conn->max_write = (4u << 20);
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

    /* Soft NUMA is opt-in (EFS_NUMA_AFFINITY=1/on/true). Default: skip entirely
     * so FUSE workers are not pinned (pinning the main thread before fuse_main
     * makes every worker inherit that mask and can collapse Slurm throughput). */
    g_client.net_numa_node = -1;
    g_client.net_affinity_valid = 0;
    g_client.net_ifname[0] = '\0';
    CPU_ZERO(&g_client.net_cpu_set);
    if (efs_numa_affinity_enabled()) {
        int pinned = 0;
        for (uint32_t i = 0; i < node_count && !pinned; i++) {
            char host[64];
            uint16_t port = 0;
            if (parse_addr(nodes[i], host, sizeof(host), &port) != 0)
                continue;
            int numa = -1;
            cpu_set_t set;
            char ifname[IFNAMSIZ];
            if (efs_numa_for_peer(host, port, &numa, &set, ifname,
                                  sizeof(ifname)) == 0) {
                g_client.net_numa_node = numa;
                snprintf(g_client.net_ifname, sizeof(g_client.net_ifname), "%s",
                         ifname);
                printf("nic_numa=%d if=%s affinity=on\n", numa, ifname);
                fflush(stdout);
                g_client.net_cpu_set = set;
                g_client.net_affinity_valid = 1;
                efs_numa_apply_affinity(&g_client.net_cpu_set);
                pinned = 1;
            }
        }
        if (!pinned) {
            printf("nic_numa=unknown\n");
            fflush(stdout);
        }
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
     * force a 128 KiB RMW per call. big_writes raises the kernel limit;
     * max_write=4MiB matches EFS_WRITE_PIPELINE=16 (kernel may clamp).
     * max_readahead helps single-stream dd reads. use_ino exposes our
     * st_ino; attr/entry timeouts at 0 avoid stale nlink/mode. */
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] =
        "big_writes,max_write=4194304,max_readahead=4194304,use_ino,attr_timeout=0,entry_timeout=0,ac_attr_timeout=0";
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
