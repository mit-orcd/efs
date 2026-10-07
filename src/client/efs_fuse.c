#define FUSE_USE_VERSION 312

#include "client_internal.h"
#include "reply_buffers.h"
#include "stop_control.h"
#include "efs/common.h"
#include "efs/write_extent.h"
#include <limits.h>
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include "efs/log_ts.h"
#include "efs/version.h"
#include "efs/placement.h"
#include "efs/checksum.h"
#include "efs/erasure.h"
#include "efs/rdma.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <fuse_lowlevel.h>
#include <fuse_opt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h> /* FALLOC_FL_KEEP_SIZE (_GNU_SOURCE) */
#include <sys/file.h>
#include <limits.h>
#include <signal.h>
#include <execinfo.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sched.h>

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

/* Live-hang debugging. ptrace is admin-only on the test nodes (yama
 * ptrace_scope=2), so gdb/gstack/gcore cannot attach to a wedged mount.
 * SIGUSR1 to the process makes the receiving thread tgkill SIGUSR2 to every
 * other thread; each dumps its own stack to stderr (fuse.log). Resolve the
 * addresses with addr2line against the binary. Debug-only: opendir/backtrace
 * in a handler are not async-signal-safe, but the process is already stuck. */
static volatile int g_efs_fuse_dump_lock;

static void efs_fuse_stack_signal(int sig)
{
    (void)sig;
    void *frames[64];
    int nf = backtrace(frames, 64);
    char hdr[80];
    int n = snprintf(hdr, sizeof(hdr), "STACKDUMP tid=%ld frames=%d\n",
                     (long)syscall(SYS_gettid), nf);
    /* Every thread writes the same fd; without this the stacks interleave
     * line by line and none of them can be read. */
    while (__atomic_test_and_set(&g_efs_fuse_dump_lock, __ATOMIC_ACQUIRE))
        sched_yield();
    if (n > 0)
        (void)write(STDERR_FILENO, hdr, (size_t)n);
    backtrace_symbols_fd(frames, nf, STDERR_FILENO);
    __atomic_clear(&g_efs_fuse_dump_lock, __ATOMIC_RELEASE);
}

static void efs_fuse_stack_broadcast(int sig)
{
    (void)sig;
    pid_t me = (pid_t)syscall(SYS_gettid);
    char hdr[128];
    int n = snprintf(hdr, sizeof(hdr),
                     "STACKDUMP begin idx_mu owner=%d lock=%d nusers=%u\n",
                     g_client.idx_mu.__data.__owner,
                     g_client.idx_mu.__data.__lock,
                     g_client.idx_mu.__data.__nusers);
    if (n > 0)
        (void)write(STDERR_FILENO, hdr, (size_t)n);
    for (int i = 0; i < EFS_DIR_LOCKS; i++) {
        if (!g_client.dir_lock[i].__data.__lock &&
            !g_client.dir_lock[i].__data.__owner)
            continue;
        n = snprintf(hdr, sizeof(hdr),
                     "STACKDUMP dir_lock[%d] owner=%d lock=%d\n", i,
                     g_client.dir_lock[i].__data.__owner,
                     g_client.dir_lock[i].__data.__lock);
        if (n > 0)
            (void)write(STDERR_FILENO, hdr, (size_t)n);
    }
    DIR *d = opendir("/proc/self/task");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            pid_t tid = (pid_t)strtol(de->d_name, NULL, 10);
            if (tid != me)
                (void)syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
        }
        closedir(d);
    }
    efs_fuse_stack_signal(SIGUSR2);
    (void)write(STDERR_FILENO, "STACKDUMP end\n", 14);
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

    struct sigaction sd;
    memset(&sd, 0, sizeof(sd));
    sd.sa_handler = efs_fuse_stack_broadcast;
    sigemptyset(&sd.sa_mask);
    sigaction(SIGUSR1, &sd, NULL);
    sd.sa_handler = efs_fuse_stack_signal;
    sigaction(SIGUSR2, &sd, NULL);
}

static ino_t stats_synthetic_ino(efs_ino_t parent_ino)
{
    return (ino_t)((1ULL << 62) | (parent_ino & ((1ULL << 60) - 1)));
}

/* du(1) sums st_blocks (512-byte units), not st_size. */
static uint32_t fuse_chunk_size(void)
{
    uint32_t cs = g_client.export.chunk_size;
    return efs_chunk_size_valid(cs) ? cs : EFS_DEFAULT_CHUNK_SIZE;
}

static void stat_set_size_blocks(struct stat *stbuf, uint64_t size,
                                 uint64_t allocated)
{
    stbuf->st_size = (off_t)size;
    stbuf->st_blksize = fuse_chunk_size();
    stbuf->st_blocks = (blkcnt_t)((allocated + 511) / 512);
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
    int n = -1;
    if (grc == 0)
        n = efs_export_format_stats_ex(&g_client.export, &fresh, e->text,
                                       EFS_STATS_TEXT - 48);
    pthread_mutex_unlock(&g_client.lock);
    if (grc != 0)
        return -ENOENT;
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
    stat_set_size_blocks(stbuf, (uint64_t)e.len, (uint64_t)e.len);
    stbuf->st_uid = (uid_t)e.uid;
    stbuf->st_gid = (gid_t)e.gid;
    stbuf->st_mtim.tv_sec = (time_t)e.mtime;
    stbuf->st_atim.tv_sec = (time_t)e.atime;
    stbuf->st_ctim.tv_sec = (time_t)e.ctime;
    return 0;
}

/* --- Per-export feature switches (.stats / .find) ---
 * The masks live in the replicated EFSR root and are server-owned (a client
 * flush preserves the server's value). The client learns them on mount and
 * re-polls a seed node at most once per TTL so an efs-mgmt toggle reaches a
 * live mount without a remount. Polling never takes g_client.lock, so the
 * data path is unaffected. */
static uint32_t g_feat_cache = EFS_FEATURES_DEFAULT;
static uint64_t g_feat_cache_ms;
static int g_feat_cache_valid;
static pthread_mutex_t g_feat_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t features_ttl_ms(void)
{
    static int ttl = -1; /* benign idempotent race on first init */
    if (ttl < 0) {
        ttl = 2000;
        const char *e = getenv("EFS_FEATURES_TTL_MS");
        if (e && *e) {
            unsigned long v = strtoul(e, NULL, 10);
            if (v <= 600000)
                ttl = (int)v;
        }
    }
    return (uint32_t)ttl;
}

/* Query each seed node for the export's current feature mask. Returns 1 and
 * sets *out on a definitive reply, 0 if no node answered. */
static int features_poll(uint32_t *out)
{
    char name[EFS_MAX_NAME];
    pthread_mutex_lock(&g_client.lock);
    strncpy(name, g_client.export.name, EFS_MAX_NAME - 1);
    name[EFS_MAX_NAME - 1] = '\0';
    pthread_mutex_unlock(&g_client.lock);

    for (uint32_t i = 0; i < g_client.node_count; i++) {
        efs_node_id_t nid = g_client.nodes[i].id;
        struct efs_conn *conn = efs_client_conn_get(nid);
        if (!conn)
            continue;
        struct efs_msg_get_features req;
        memset(&req, 0, sizeof(req));
        strncpy(req.export_name, name, EFS_MAX_NAME - 1);
        uint8_t type = 0;
        void *reply = NULL;
        uint32_t rlen = 0;
        int ok = (efs_conn_send_msg(conn, EFS_MSG_GET_FEATURES, &req,
                                    sizeof(req)) == 0 &&
                  efs_conn_recv_msg(conn, &type, &reply, &rlen) == 0 &&
                  type == EFS_MSG_GET_FEATURES_REPLY &&
                  rlen >= sizeof(struct efs_msg_features_reply));
        uint32_t feat = 0;
        uint8_t status = EFS_FEATURES_NOT_FOUND;
        if (ok) {
            struct efs_msg_features_reply *r = reply;
            feat = r->features;
            status = r->status;
            efs_client_conn_release(nid, conn);
        } else {
            efs_client_conn_drop(nid, conn);
        }
        free(reply);
        if (ok && status == EFS_FEATURES_OK) {
            *out = feat;
            return 1;
        }
    }
    return 0;
}

/* Current feature mask; refreshed over the wire at most once per TTL. */
static uint32_t features_current(void)
{
    uint64_t now = stats_now_ms();
    pthread_mutex_lock(&g_feat_mu);
    if (g_feat_cache_valid && now - g_feat_cache_ms < features_ttl_ms()) {
        uint32_t f = g_feat_cache;
        pthread_mutex_unlock(&g_feat_mu);
        return f;
    }
    pthread_mutex_unlock(&g_feat_mu);

    uint32_t f;
    if (features_poll(&f)) {
        pthread_mutex_lock(&g_feat_mu);
        g_feat_cache = f;
        g_feat_cache_ms = now;
        g_feat_cache_valid = 1;
        pthread_mutex_unlock(&g_feat_mu);
        return f;
    }
    /* Poll failed: fall back to the last cached value, else the mount-time
     * export value learned from the EFSR root. */
    pthread_mutex_lock(&g_feat_mu);
    if (g_feat_cache_valid) {
        uint32_t f2 = g_feat_cache;
        pthread_mutex_unlock(&g_feat_mu);
        return f2;
    }
    pthread_mutex_unlock(&g_feat_mu);
    pthread_mutex_lock(&g_client.lock);
    uint32_t f3 = g_client.export.features;
    pthread_mutex_unlock(&g_client.lock);
    return f3;
}

static int feature_enabled(uint32_t bit)
{
    return (features_current() & bit) != 0;
}

/* --- Virtual per-directory .find (not a real inode) ---
 * .find is a virtual directory; reading "<dir>/.find/<term>" runs a glob-ish
 * query ("term", "*term", "term*", "*term*"; literal part >= EFS_FIND_MIN_TERM
 * chars) over <dir>'s subtree and returns the matching paths (one per line,
 * host-absolute: mountpoint + fs-root path, so they pipe/loop from any cwd).
 * Single-command, e.g. cat ".find/PATTERN". The walk is READDIR from the
 * query directory (the client table is not a full snapshot in raft mode). */
static ino_t find_synthetic_ino(efs_ino_t parent_ino)
{
    return (ino_t)((1ULL << 61) | (parent_ino & ((1ULL << 60) - 1)));
}

/* Reserved virtual-file names: never allow a real file/dir to shadow them. */
static int name_is_reserved(const char *name)
{
    return strcmp(name, EFS_STATS_NAME) == 0 || strcmp(name, EFS_FIND_NAME) == 0;
}

enum find_match { FIND_EXACT, FIND_PREFIX, FIND_SUFFIX, FIND_SUBSTR };

/* Canonical absolute mountpoint, set once in main. .find results are rendered
 * as host-absolute paths (mountpoint + fs-root path) so they can be piped or
 * looped over from any working directory. */
static char g_mountpoint[EFS_MAX_PATH] = "/";

/* Low-level FUSE: nodeid == efs ino (FUSE_ROOT_ID == EFS_ROOT_INO == 1).
 * Virtual .stats / .find names sit in unused high bits so they never collide
 * with a real ino. Each handler stashes `req` in t_req so permission helpers
 * can read uid/gid without a path-based fuse_get_context(). */
static __thread fuse_req_t t_req;
static struct fuse_session *g_fuse_se;

/* Part C: notify_inval_* deadlocks if it runs while the kernel still
 * holds the parent/inode lock for the in-flight request. Queue from the
 * ll_* wrapper AFTER fuse_reply_*. Timeouts stay 0, so create/lookup
 * replies already install dentries — do not broadcast inval_entry.
 * Only a shrinking SETATTR SIZE notifies, and only pages at/after the
 * new EOF (truncate-to-zero is (0,0) = all). clone_fd stays the
 * libfuse default — it was required when every unlink queued
 * inval_entry and the notify write starved /dev/fuse reads. Drop-newest
 * if the queue is full (a missed self-inval is safe at timeout=0). */
#define LL_INVAL_Q 256
enum { LL_INVAL_ENTRY = 1, LL_INVAL_INODE = 2 };
struct ll_inval_item {
    uint8_t kind;
    fuse_ino_t parent;
    fuse_ino_t ino;
    off_t off;
    off_t len;
    char name[EFS_MAX_NAME];
};
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int run;
    int started;
    int head;
    int tail;
    int n;
    struct ll_inval_item q[LL_INVAL_Q];
} g_inval = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

#define EFS_VIRT_STATS (1ULL << 62)
#define EFS_VIRT_FIND  (1ULL << 61)
#define EFS_VIRT_QUERY (1ULL << 60)
#define EFS_VIRT_PARENT_MASK ((1ULL << 60) - 1)
#ifndef EFS_FIND_WALK_DEPTH
#define EFS_FIND_WALK_DEPTH 128
#endif

/* Files carry an open-description error cursor. Directories retain their
 * separate dirh representation. Untagged fh remains supported for virtual
 * files and internal calls that have no kernel open description. */
#define EFS_FILE_FH_TAG (1ULL << 63)
struct efs_file_handle {
    efs_ino_t ino;
    struct efs_wb_description *errors;
};
static struct efs_file_handle *efs_file_handle(const struct fuse_file_info *fi)
{
    return fi && (fi->fh & EFS_FILE_FH_TAG) ?
        (struct efs_file_handle *)(uintptr_t)(fi->fh & ~EFS_FILE_FH_TAG) : NULL;
}
static efs_ino_t efs_file_ino(const struct fuse_file_info *fi, efs_ino_t fallback)
{
    struct efs_file_handle *h = efs_file_handle(fi);
    return h ? h->ino : fi && fi->fh ? (efs_ino_t)fi->fh : fallback;
}
static int efs_file_open(struct fuse_file_info *fi, efs_ino_t ino)
{
    if (!fi)
        return 0;
    struct efs_file_handle *h = calloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;
    h->ino = ino;
    h->errors = efs_wb_description_open(ino);
    if (!h->errors) {
        free(h);
        return -ENOMEM;
    }
    fi->fh = EFS_FILE_FH_TAG | (uint64_t)(uintptr_t)h;
    return 0;
}
static void efs_file_close(struct fuse_file_info *fi)
{
    struct efs_file_handle *h = efs_file_handle(fi);
    if (h) {
        efs_wb_description_close(h->errors);
        free(h);
        fi->fh = 0;
    }
}
static int efs_file_sync_error(struct fuse_file_info *fi, efs_ino_t ino,
                                int was_stalled, int rc)
{
    struct efs_file_handle *h = efs_file_handle(fi);
    int remains = efs_wb_inode_stalled(ino);
    int error = h && efs_wb_description_sync(h->errors,
                        rc == EFS_OK && was_stalled && !remains);
    return error || remains ? EFS_ERR_IO : rc;
}

static int virt_kind(fuse_ino_t ino)
{
    if (ino & EFS_VIRT_STATS)
        return 1;
    if (ino & EFS_VIRT_FIND)
        return 2;
    if (ino & EFS_VIRT_QUERY)
        return 3;
    return 0;
}

static efs_ino_t virt_parent(fuse_ino_t ino)
{
    return (efs_ino_t)(ino & EFS_VIRT_PARENT_MASK);
}

static fuse_ino_t virt_stats_ino(efs_ino_t parent)
{
    return (fuse_ino_t)(EFS_VIRT_STATS | (parent & EFS_VIRT_PARENT_MASK));
}

static fuse_ino_t virt_find_ino(efs_ino_t parent)
{
    return (fuse_ino_t)(EFS_VIRT_FIND | (parent & EFS_VIRT_PARENT_MASK));
}

#define EFS_VQ_N 64
static struct {
    int valid;
    uint32_t nlookup;
    fuse_ino_t nodeid;
    efs_ino_t dir_ino;
    char term[EFS_MAX_NAME];
} g_vq[EFS_VQ_N];
static pthread_mutex_t g_vq_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_vq_seq = 1;

static fuse_ino_t vq_intern(efs_ino_t dir_ino, const char *term)
{
    fuse_ino_t id = 0;
    int slot = -1, empty = -1, stale = -1;

    pthread_mutex_lock(&g_vq_mu);
    for (int i = 0; i < EFS_VQ_N; i++) {
        if (g_vq[i].valid && g_vq[i].dir_ino == dir_ino &&
            strcmp(g_vq[i].term, term) == 0) {
            id = g_vq[i].nodeid;
            pthread_mutex_unlock(&g_vq_mu);
            return id;
        }
        if (!g_vq[i].valid && empty < 0)
            empty = i;
        else if (g_vq[i].valid && g_vq[i].nlookup == 0)
            stale = i;
    }
    slot = empty >= 0 ? empty : (stale >= 0 ? stale : 0);
    id = (fuse_ino_t)(EFS_VIRT_QUERY | (g_vq_seq++ & EFS_VIRT_PARENT_MASK));
    if (!(id & EFS_VIRT_QUERY))
        id = EFS_VIRT_QUERY | 1;
    memset(&g_vq[slot], 0, sizeof(g_vq[slot]));
    g_vq[slot].valid = 1;
    g_vq[slot].nodeid = id;
    g_vq[slot].dir_ino = dir_ino;
    strncpy(g_vq[slot].term, term, EFS_MAX_NAME - 1);
    pthread_mutex_unlock(&g_vq_mu);
    return id;
}

static int vq_lookup(fuse_ino_t nodeid, efs_ino_t *dir_out, char *term_out,
                     size_t term_cap)
{
    pthread_mutex_lock(&g_vq_mu);
    for (int i = 0; i < EFS_VQ_N; i++) {
        if (!g_vq[i].valid || g_vq[i].nodeid != nodeid)
            continue;
        if (dir_out)
            *dir_out = g_vq[i].dir_ino;
        if (term_out && term_cap) {
            strncpy(term_out, g_vq[i].term, term_cap - 1);
            term_out[term_cap - 1] = '\0';
        }
        pthread_mutex_unlock(&g_vq_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_vq_mu);
    return -1;
}

static void vq_nlookup_add(fuse_ino_t nodeid, uint32_t n)
{
    pthread_mutex_lock(&g_vq_mu);
    for (int i = 0; i < EFS_VQ_N; i++) {
        if (g_vq[i].valid && g_vq[i].nodeid == nodeid) {
            g_vq[i].nlookup += n;
            break;
        }
    }
    pthread_mutex_unlock(&g_vq_mu);
}

static void vq_nlookup_sub(fuse_ino_t nodeid, uint64_t n)
{
    pthread_mutex_lock(&g_vq_mu);
    for (int i = 0; i < EFS_VQ_N; i++) {
        if (!g_vq[i].valid || g_vq[i].nodeid != nodeid)
            continue;
        if (n >= g_vq[i].nlookup) {
            g_vq[i].valid = 0;
            g_vq[i].nlookup = 0;
        } else {
            g_vq[i].nlookup -= (uint32_t)n;
        }
        break;
    }
    pthread_mutex_unlock(&g_vq_mu);
}

static const struct fuse_ctx *ll_ctx(void)
{
    return t_req ? fuse_req_ctx(t_req) : NULL;
}

static void ll_inval_push(struct ll_inval_item it)
{
    if (!g_fuse_se || virt_kind(it.parent) || virt_kind(it.ino))
        return;
    pthread_mutex_lock(&g_inval.mu);
    if (!g_inval.run) {
        pthread_mutex_unlock(&g_inval.mu);
        return;
    }
    if (g_inval.n >= LL_INVAL_Q) {
        pthread_mutex_unlock(&g_inval.mu);
        return;
    }
    g_inval.q[g_inval.tail] = it;
    g_inval.tail = (g_inval.tail + 1) % LL_INVAL_Q;
    g_inval.n++;
    pthread_cond_signal(&g_inval.cv);
    pthread_mutex_unlock(&g_inval.mu);
}

static void ll_inval_entry(fuse_ino_t parent, const char *name)
    __attribute__((unused));
static void ll_inval_entry(fuse_ino_t parent, const char *name)
{
    struct ll_inval_item it;

    if (!name || !name[0] || virt_kind(parent))
        return;
    memset(&it, 0, sizeof(it));
    it.kind = LL_INVAL_ENTRY;
    it.parent = parent;
    strncpy(it.name, name, EFS_MAX_NAME - 1);
    ll_inval_push(it);
}

static void ll_inval_inode(fuse_ino_t ino, off_t off, off_t len)
{
    struct ll_inval_item it;

    if (!ino || virt_kind(ino))
        return;
    memset(&it, 0, sizeof(it));
    it.kind = LL_INVAL_INODE;
    it.ino = ino;
    it.off = off;
    it.len = len;
    ll_inval_push(it);
}

static void *ll_inval_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct ll_inval_item it;
        pthread_mutex_lock(&g_inval.mu);
        while (g_inval.run && g_inval.n == 0)
            pthread_cond_wait(&g_inval.cv, &g_inval.mu);
        if (g_inval.n == 0) {
            pthread_mutex_unlock(&g_inval.mu);
            break;
        }
        it = g_inval.q[g_inval.head];
        g_inval.head = (g_inval.head + 1) % LL_INVAL_Q;
        g_inval.n--;
        pthread_mutex_unlock(&g_inval.mu);
        if (!g_fuse_se)
            continue;
        if (it.kind == LL_INVAL_ENTRY)
            (void)fuse_lowlevel_notify_inval_entry(g_fuse_se, it.parent,
                                                   it.name, strlen(it.name));
        else if (it.kind == LL_INVAL_INODE)
            (void)fuse_lowlevel_notify_inval_inode(g_fuse_se, it.ino,
                                                   it.off, it.len);
    }
    return NULL;
}

static int ll_inval_start(void)
{
    g_inval.run = 1;
    g_inval.head = g_inval.tail = g_inval.n = 0;
    if (pthread_create(&g_inval.th, NULL, ll_inval_thread, NULL) != 0) {
        g_inval.run = 0;
        fprintf(stderr, "efs-fuse: self-inval thread failed to start\n");
        fflush(stderr);
        return -1;
    }
    g_inval.started = 1;
    return 0;
}

static void ll_inval_stop(void)
{
    if (!g_inval.started)
        return;
    pthread_mutex_lock(&g_inval.mu);
    g_inval.run = 0;
    pthread_cond_signal(&g_inval.cv);
    pthread_mutex_unlock(&g_inval.mu);
    pthread_join(g_inval.th, NULL);
    g_inval.started = 0;
}

static int ino_to_fuse_path(efs_ino_t ino, char *out, size_t cap)
{
    char names[EFS_FIND_WALK_DEPTH][EFS_MAX_NAME];
    int n = 0;
    efs_ino_t cur = ino;

    if (!out || cap < 2)
        return -1;
    while (cur && cur != EFS_ROOT_INO && n < EFS_FIND_WALK_DEPTH) {
        struct efs_inode row;
        if (efs_client_stat_ino(cur, &row) != EFS_OK)
            break;
        strncpy(names[n], row.name, EFS_MAX_NAME - 1);
        names[n][EFS_MAX_NAME - 1] = '\0';
        n++;
        if (!row.parent || row.parent == cur)
            break;
        cur = row.parent;
    }
    if (n == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    size_t len = 0;
    out[0] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        int wr = snprintf(out + len, cap - len, "/%s", names[i]);
        if (wr < 0 || (size_t)wr >= cap - len)
            return -1;
        len += (size_t)wr;
    }
    return 0;
}

#define EFS_FIND_WALK_MAX   65536u

struct find_out {
    char *buf;
    size_t cap;
    size_t len;
    uint32_t nvisit;
};

static int find_out_add(struct find_out *o, const char *prefix, const char *rel)
{
    size_t pl = strlen(prefix), rl = strlen(rel);
    int slash = (pl > 0 && prefix[pl - 1] != '/' && rel[0] != '/');
    size_t need = pl + (slash ? 1 : 0) + rl + 1; /* newline */

    if (o->len + need + 1 > o->cap) {
        size_t ncap = o->cap ? o->cap * 2 : 4096;
        while (ncap < o->len + need + 1)
            ncap *= 2;
        char *nb = realloc(o->buf, ncap);
        if (!nb)
            return EFS_ERR_NOMEM;
        o->buf = nb;
        o->cap = ncap;
    }
    memcpy(o->buf + o->len, prefix, pl);
    o->len += pl;
    if (slash)
        o->buf[o->len++] = '/';
    memcpy(o->buf + o->len, rel, rl);
    o->len += rl;
    o->buf[o->len++] = '\n';
    o->buf[o->len] = '\0';
    return 0;
}

static int find_name_match(const char *name, enum find_match type,
                           const char *term, size_t term_len)
{
    size_t nl = strlen(name);
    switch (type) {
    case FIND_EXACT:
        return nl == term_len && memcmp(name, term, term_len) == 0;
    case FIND_PREFIX:
        return nl >= term_len && memcmp(name, term, term_len) == 0;
    case FIND_SUFFIX:
        return nl >= term_len && memcmp(name + nl - term_len, term, term_len) == 0;
    case FIND_SUBSTR:
        return nl >= term_len && memmem(name, nl, term, term_len) != NULL;
    }
    return 0;
}

/* Parse a .find query. Strips trailing whitespace/newline; extracts the
 * leading/trailing '*' and the literal term. Returns 0 on success, -EINVAL if
 * the literal term is shorter than EFS_FIND_MIN_TERM or has an interior '*'. */
static int find_parse_query(const char *q, size_t qlen, enum find_match *type,
                            char *term, size_t *term_len)
{
    while (qlen > 0 &&
           (q[qlen - 1] == '\n' || q[qlen - 1] == '\r' ||
            q[qlen - 1] == ' ' || q[qlen - 1] == '\t'))
        qlen--;
    if (qlen == 0 || qlen >= EFS_MAX_NAME)
        return -EINVAL;
    int lead = (q[0] == '*');
    int trail = (qlen > 1 && q[qlen - 1] == '*');
    size_t ts = lead ? 1 : 0;
    size_t te = trail ? qlen - 1 : qlen;
    size_t tl = (te > ts) ? te - ts : 0;
    if (tl < EFS_FIND_MIN_TERM)
        return -EINVAL;
    for (size_t i = ts; i < te; i++) {
        if (q[i] == '*')
            return -EINVAL;
    }
    memcpy(term, q + ts, tl);
    term[tl] = '\0';
    *term_len = tl;
    if (lead && trail)
        *type = FIND_SUBSTR;
    else if (trail)
        *type = FIND_PREFIX;
    else if (lead)
        *type = FIND_SUFFIX;
    else
        *type = FIND_EXACT;
    return 0;
}

/* FUSE path of the directory being searched, from a ".find/<term>" path. */
static int find_query_dir_fuse(const char *query_path, char *out, size_t cap)
{
    const char *m;
    size_t dlen;

    if (!query_path || !out || cap < 2)
        return -1;
    m = strstr(query_path, "/" EFS_FIND_NAME "/");
    if (!m)
        return -1;
    dlen = (size_t)(m - query_path);
    if (dlen == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    if (dlen >= cap)
        return -1;
    memcpy(out, query_path, dlen);
    out[dlen] = '\0';
    return 0;
}

/* host-absolute prefix for results: mountpoint + FUSE dir path. A "/"
 * mountpoint is omitted so we do not emit "//...". */
static void find_host_prefix(const char *dir_fuse, char *out, size_t cap)
{
    size_t mpl = strlen(g_mountpoint);
    const char *df = (dir_fuse && dir_fuse[0]) ? dir_fuse : "/";

    if (mpl <= 1) {
        snprintf(out, cap, "%s", df);
        return;
    }
    if (strcmp(df, "/") == 0)
        snprintf(out, cap, "%s", g_mountpoint);
    else
        snprintf(out, cap, "%s%s", g_mountpoint, df);
}

/* READDIR-walk one directory (and recurse into children). Matches are
 * rendered as host-absolute paths under host_prefix. A missing/invalid
 * child is skipped; a failure of the query directory itself is fatal. */
static int find_walk_dir(efs_ino_t dir_ino, const char *rel,
                         const char *host_prefix,
                         enum find_match type, const char *term, size_t term_len,
                         int depth, struct find_out *o)
{
    uint32_t src = 0, done = 0;
    char name_cur[EFS_MAX_NAME];
    struct efs_inode *ents;

    if (depth > EFS_FIND_WALK_DEPTH)
        return 0;
    ents = malloc(sizeof(*ents) * EFS_READDIR_MAX);
    if (!ents)
        return EFS_ERR_NOMEM;
    memset(name_cur, 0, sizeof(name_cur));
    while (!done) {
        uint32_t n = EFS_READDIR_MAX;
        uint32_t i;
        int rc = efs_client_rpc_readdir_cur(g_client.export_id, dir_ino,
                                            ents, &n, &src, name_cur, &done);
        if (rc != 0) {
            free(ents);
            if (depth > 0 && (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_INVAL))
                return 0;
            return rc;
        }
        for (i = 0; i < n; i++) {
            char child_rel[EFS_MAX_PATH];
            int nwr;

            if (ents[i].ino == 0 || ents[i].name[0] == '\0')
                continue;
            if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0)
                continue;
            if (strncmp(ents[i].name, ".fuse_hidden", 12) == 0)
                continue;
            if (strncmp(ents[i].name, ".parked-", 8) == 0)
                continue;
            if (name_is_reserved(ents[i].name))
                continue;
            if (o->nvisit >= EFS_FIND_WALK_MAX) {
                free(ents);
                return 0;
            }
            o->nvisit++;
            if (rel[0] == '\0')
                nwr = snprintf(child_rel, sizeof(child_rel), "%s", ents[i].name);
            else
                nwr = snprintf(child_rel, sizeof(child_rel), "%s/%s",
                              rel, ents[i].name);
            if (nwr < 0 || (size_t)nwr >= sizeof(child_rel))
                continue;
            if (find_name_match(ents[i].name, type, term, term_len)) {
                int arc = find_out_add(o, host_prefix, child_rel);
                if (arc != 0) {
                    free(ents);
                    return arc;
                }
            }
            if (efs_mode_is_dir(ents[i].mode)) {
                int wrc = find_walk_dir(ents[i].ino, child_rel, host_prefix,
                                          type, term, term_len, depth + 1, o);
                if (wrc != 0) {
                    free(ents);
                    return wrc;
                }
            }
        }
        if (n == 0 && !done)
            break;
    }
    free(ents);
    return 0;
}

/* Walk <dir>'s subtree via READDIR and return matching host-absolute paths
 * (newline-separated) in a malloc'd buffer. dir_fuse is the FUSE path of
 * the query directory (getattr does not fill name/parent, so we cannot
 * reconstruct paths from inodes). */
static int find_run_query(efs_ino_t dir_ino, const char *dir_fuse,
                          enum find_match type, const char *term, size_t term_len,
                          char **out_text, int *out_len)
{
    struct find_out o;
    char prefix[EFS_MAX_PATH];
    int rc;

    memset(&o, 0, sizeof(o));
    find_host_prefix(dir_fuse, prefix, sizeof(prefix));
    rc = find_walk_dir(dir_ino, "", prefix, type, term, term_len, 0, &o);
    if (rc != 0) {
        free(o.buf);
        return rc;
    }
    if (!o.buf) {
        o.buf = malloc(1);
        if (!o.buf)
            return EFS_ERR_NOMEM;
        o.buf[0] = '\0';
        o.len = 0;
    }
    *out_text = o.buf;
    *out_len = (int)o.len;
    return 0;
}

/* Cached .find query results, keyed by (directory, raw pattern). A read of
 * ".find/<term>" runs the glob over the directory's subtree and caches the
 * rendered path list briefly so the getattr (size) and the read (content) that
 * make up a single `cat` share one walk. Queries are user-driven and
 * rare, so the small cache + short TTL keep results fresh without a hot-path
 * cost. */
#define EFS_FIND_RES_N 16
#define EFS_FIND_RES_TTL_MS 5000
static struct {
    int valid;
    efs_ino_t dir_ino;
    char term[EFS_MAX_NAME]; /* raw pattern, e.g. "*report*" */
    uint64_t ms;
    int len;
    char *text;
} g_find_res[EFS_FIND_RES_N];
static pthread_mutex_t g_find_res_mu = PTHREAD_MUTEX_INITIALIZER;

/* Ensure a fresh cached result for (dir_ino, pattern). Returns 0 on success
 * (result cached, possibly empty), -EINVAL for a bad/short term, -EIO on
 * internal error. query_path is the FUSE ".find/<term>" path (used to
 * render host-absolute matches). */
static int find_query_ensure(efs_ino_t dir_ino, const char *query_path,
                             const char *pattern)
{
    enum find_match type;
    char term[EFS_MAX_NAME];
    size_t term_len;
    char dir_fuse[EFS_MAX_PATH];
    if (find_parse_query(pattern, strlen(pattern), &type, term, &term_len) != 0)
        return -EINVAL;
    if (find_query_dir_fuse(query_path, dir_fuse, sizeof(dir_fuse)) != 0)
        return -EINVAL;

    uint64_t now = stats_now_ms();
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (g_find_res[i].valid && g_find_res[i].dir_ino == dir_ino &&
            strcmp(g_find_res[i].term, pattern) == 0 &&
            now - g_find_res[i].ms < EFS_FIND_RES_TTL_MS) {
            pthread_mutex_unlock(&g_find_res_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_find_res_mu);

    /* Miss: walk the live tree outside the result-cache lock. */
    char *text = NULL;
    int len = 0;
    if (find_run_query(dir_ino, dir_fuse, type, term, term_len, &text, &len) != 0) {
        free(text);
        return -EIO;
    }

    pthread_mutex_lock(&g_find_res_mu);
    int slot = -1;
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (!g_find_res[i].valid ||
            (g_find_res[i].dir_ino == dir_ino &&
             strcmp(g_find_res[i].term, pattern) == 0)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { /* evict the oldest */
        uint64_t oldest = ~0ULL;
        for (int i = 0; i < EFS_FIND_RES_N; i++) {
            if (g_find_res[i].ms < oldest) {
                oldest = g_find_res[i].ms;
                slot = i;
            }
        }
    }
    free(g_find_res[slot].text);
    g_find_res[slot].valid = 1;
    g_find_res[slot].dir_ino = dir_ino;
    strncpy(g_find_res[slot].term, pattern, EFS_MAX_NAME - 1);
    g_find_res[slot].term[EFS_MAX_NAME - 1] = '\0';
    g_find_res[slot].ms = now;
    g_find_res[slot].text = text; /* takes ownership */
    g_find_res[slot].len = len;
    pthread_mutex_unlock(&g_find_res_mu);
    return 0;
}

/* Result length for (dir, pattern), or -1 on invalid term / error. */
static int find_query_len(efs_ino_t dir_ino, const char *query_path,
                           const char *pattern)
{
    if (find_query_ensure(dir_ino, query_path, pattern) != 0)
        return -1;
    pthread_mutex_lock(&g_find_res_mu);
    int len = -1;
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (g_find_res[i].valid && g_find_res[i].dir_ino == dir_ino &&
            strcmp(g_find_res[i].term, pattern) == 0) {
            len = g_find_res[i].len;
            break;
        }
    }
    pthread_mutex_unlock(&g_find_res_mu);
    return len;
}

/* Copy a slice of the (dir, pattern) result into buf. Returns bytes copied,
 * 0 at EOF, or a negative errno for an invalid term. */
static int find_query_read(efs_ino_t dir_ino, const char *query_path,
                           const char *pattern,
                           char *buf, size_t size, off_t offset)
{
    if (find_query_ensure(dir_ino, query_path, pattern) != 0)
        return -EINVAL;
    pthread_mutex_lock(&g_find_res_mu);
    int n = 0;
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (!g_find_res[i].valid || g_find_res[i].dir_ino != dir_ino ||
            strcmp(g_find_res[i].term, pattern) != 0)
            continue;
        if (offset < g_find_res[i].len) {
            size_t avail = (size_t)g_find_res[i].len - (size_t)offset;
            if (avail > size)
                avail = size;
            memcpy(buf, g_find_res[i].text + offset, avail);
            n = (int)avail;
        }
        break;
    }
    pthread_mutex_unlock(&g_find_res_mu);
    return n;
}

/* Synthetic ino for a .find/<term> query file: distinct high bit from the
 * .find dir, folded with a hash of the pattern so different terms differ. */
static ino_t find_query_ino(efs_ino_t dir_ino, const char *pattern)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)pattern; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return (ino_t)((1ULL << 60) | ((dir_ino + (h & 0xfffff)) & ((1ULL << 60) - 1)));
}

/* stat for the virtual .find directory itself. */
static int find_dir_fill_stat(const struct efs_inode *parent, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = find_synthetic_ino(parent->ino);
    stbuf->st_mode = S_IFDIR | 0555;
    stbuf->st_nlink = 2;
    stbuf->st_uid = parent->uid;
    stbuf->st_gid = parent->gid;
    stbuf->st_mtim.tv_sec = (time_t)parent->mtime;
    stbuf->st_atim.tv_sec = (time_t)parent->atime;
    stbuf->st_ctim.tv_sec = (time_t)parent->ctime;
    return 0;
}

/* stat for a .find/<term> query file: runs the query to report the real size
 * (so `cat` reads the full list rather than stopping at a 0 size). */
static int find_query_fill_stat(const struct efs_inode *parent,
                                const char *query_path, const char *pattern,
                                struct stat *stbuf)
{
    int len = find_query_len(parent->ino, query_path, pattern);
    if (len < 0)
        return -ENOENT;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = find_query_ino(parent->ino, pattern);
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stat_set_size_blocks(stbuf, (uint64_t)len, (uint64_t)len);
    stbuf->st_uid = parent->uid;
    stbuf->st_gid = parent->gid;
    stbuf->st_mtim.tv_sec = (time_t)parent->mtime;
    stbuf->st_atim.tv_sec = (time_t)parent->atime;
    stbuf->st_ctim.tv_sec = (time_t)parent->ctime;
    return 0;
}

/* POSIX permission check: does the caller (uid/gid) have the requested access
 * (mask: R_OK/W_OK/X_OK) to a file with the given mode/uid/gid? Root (uid 0)
 * bypasses. We enforce this in the daemon (not via the default_permissions
 * mount opt) because that option would also gate the export root, which is
 * owned by root — making the whole mount read-only for unprivileged users.
 * The export root (EFS_ROOT_INO) is therefore also skipped by the dir-wx /
 * search helpers below; every other inode is checked. */
static int caller_in_group(gid_t gid)
{
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    if (ctx->gid == gid)
        return 1;
    gid_t list[64];
    int n = t_req ? fuse_req_getgroups(t_req,
                                       (int)(sizeof(list) / sizeof(list[0])),
                                       list)
                  : -1;
    for (int i = 0; i < n; i++) {
        if (list[i] == gid)
            return 1;
    }
    return 0;
}

static int check_access(const struct efs_inode *ino, uid_t uid, gid_t gid,
                        int mask)
{
    if (uid == 0)
        return 0;
    uint32_t perm;
    if (uid == ino->uid)
        perm = (ino->mode >> 6) & 7;        /* owner */
    else if (gid == ino->gid || caller_in_group(ino->gid))
        perm = (ino->mode >> 3) & 7;        /* group */
    else
        perm = ino->mode & 7;               /* other */
    if ((mask & R_OK) && !(perm & 4))
        return -EACCES;
    if ((mask & W_OK) && !(perm & 2))
        return -EACCES;
    if ((mask & X_OK) && !(perm & 1))
        return -EACCES;
    return 0;
}

static int check_dir_wx(const struct efs_inode *dir)
{
    if (!dir || dir->ino == EFS_ROOT_INO)
        return 0;
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    return check_access(dir, ctx->uid, ctx->gid, W_OK | X_OK);
}

static int check_dir_x(const struct efs_inode *dir)
{
    if (!dir || dir->ino == EFS_ROOT_INO)
        return 0;
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    return check_access(dir, ctx->uid, ctx->gid, X_OK);
}

static uint64_t inode_allocated_bytes(const struct efs_inode *ino);

static void fill_stat_from_inode(struct stat *stbuf, const struct efs_inode *ino)
{
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = ino->ino;
    stbuf->st_mode = ino->mode;
    stbuf->st_nlink = ino->nlink;
    stat_set_size_blocks(stbuf, ino->size, inode_allocated_bytes(ino));
    stbuf->st_uid = ino->uid;
    stbuf->st_gid = ino->gid;
    stbuf->st_mtim.tv_sec = (time_t)ino->mtime;
    stbuf->st_mtim.tv_nsec = (long)ino->mtime_nsec;
    stbuf->st_atim.tv_sec = (time_t)ino->atime;
    stbuf->st_atim.tv_nsec = (long)ino->atime_nsec;
    stbuf->st_ctim.tv_sec = (time_t)ino->ctime;
    stbuf->st_ctim.tv_nsec = (long)ino->ctime_nsec;
}

static int check_chown_perm(const struct efs_inode *ino, uid_t uid, gid_t gid)
{
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx || ctx->uid == 0)
        return 0;
    if (uid != (uid_t)-1 && uid != ino->uid)
        return -EPERM;
    if (gid != (gid_t)-1 && gid != ino->gid) {
        if (ctx->uid != ino->uid)
            return -EPERM;
        if (!caller_in_group(gid))
            return -EPERM;
    }
    return 0;
}

static int file_chunk_present(efs_ino_t ino, uint32_t ci)
{
    if (efs_dcache_has(ino, ci))
        return 1;
    return efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0;
}

static uint64_t inode_allocated_bytes(const struct efs_inode *ino)
{
    uint32_t cs, n_loc;
    uint64_t n, alloc, partial;
    int tail;
    if (!ino || efs_mode_is_dir(ino->mode) || efs_mode_is_lnk(ino->mode))
        return ino ? ino->size : 0;
    if (ino->pack_ino && ino->pack_len)
        return ino->pack_len;
    cs = fuse_chunk_size();
    if (cs == 0 || ino->size == 0)
        return 0;
    /* W20: one counter, not a walk of every chunk. A partial tail is
     * the only chunk that is not a full cs, so it is one extra probe.
     * D17: the row image's server-side count covers chunks this client
     * never staged; the local sum still wins while it is ahead
     * (unreported writes). */
    efs_client_lock_dir(ino->ino);
    pthread_mutex_lock(&g_client.idx_mu);
    n_loc = efs_export_present_count(&g_client.export, ino->ino);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino->ino);
    n = ino->alloc_chunks > (uint64_t)n_loc ? ino->alloc_chunks
                                            : (uint64_t)n_loc;
    partial = ino->size % cs;
    if (partial == 0 || n == 0)
        return n * cs;
    /* The partial tail counts cs unless it is provably in the count:
     * staged by this client (probe), or the server count covers every
     * chunk below size. */
    if (n == (uint64_t)n_loc)
        tail = file_chunk_present(ino->ino,
                                  (uint32_t)(ino->size / cs));
    else
        tail = n * cs >= ino->size;
    if (!tail)
        return n * cs;
    alloc = (n - 1) * cs + partial;
    return alloc;
}

/* W16.1: BUSY and NOT_PRIMARY are not a missing inode. */
static int fuse_stat_errno(int rc)
{
    if (rc == EFS_OK)
        return 0;
    if (rc == EFS_ERR_ACCES)
        return -EACCES;
    if (rc == EFS_ERR_BUSY || rc == EFS_ERR_NOT_PRIMARY)
        return -EBUSY;
    if (rc == EFS_ERR_NOT_FOUND)
        return -ENOENT;
    if (rc == EFS_ERR_NOMEM)
        return -ENOMEM;
    if (rc == EFS_ERR_EXIST)
        return -EEXIST;
    if (rc == EFS_ERR_NAMETOOLONG)
        return -ENAMETOOLONG;
    return -EIO;
}

/* LOOKUP already returns a full row; with attr_timeout=0 the kernel
 * GETATTRs the same ino on another FUSE worker immediately after.
 * One-shot, 50 ms: consume the row, then the next getattr RPCs. Local
 * mutations invalidate the small cache and suppress it while in flight.
 * A LOOKUP captures the mutation serial before fetching its row, so an
 * older RPC reply cannot reinsert a pre-mutation row afterwards (0j). */
#define LOOKUP_MEMO_N 32
#define LOOKUP_MEMO_US 50000ull

struct lookup_memo {
    efs_ino_t ino;
    struct efs_inode row;
    uint64_t us;
};

static pthread_mutex_t g_lookup_memo_mu = PTHREAD_MUTEX_INITIALIZER;
static struct lookup_memo g_lookup_memo[LOOKUP_MEMO_N];
static unsigned g_lookup_memo_i;
static uint64_t g_lookup_memo_serial = 1;
static unsigned g_lookup_memo_active;
static __thread uint64_t t_lookup_memo_serial;
static __thread struct efs_inode t_getattr_row;
static __thread efs_ino_t t_getattr_ino;

static uint64_t fuse_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint64_t lookup_memo_start(void)
{
    uint64_t serial;
    pthread_mutex_lock(&g_lookup_memo_mu);
    serial = g_lookup_memo_active ? 0 : g_lookup_memo_serial;
    pthread_mutex_unlock(&g_lookup_memo_mu);
    return serial;
}

/* Clear all 32 slots: namespace operations can mutate a replaced inode and
 * both parents, whose identities are not all available at request entry.
 * Do not hold this mutex across I/O. Overlapping/failed mutations are safe:
 * each request balances begin/end, and neither edge permits an old reply. */
static void lookup_memo_mutation_begin(void)
{
    pthread_mutex_lock(&g_lookup_memo_mu);
    ++g_lookup_memo_serial;
    ++g_lookup_memo_active;
    for (unsigned i = 0; i < LOOKUP_MEMO_N; ++i)
        g_lookup_memo[i].ino = 0;
    pthread_mutex_unlock(&g_lookup_memo_mu);
}

static void lookup_memo_mutation_end(void)
{
    pthread_mutex_lock(&g_lookup_memo_mu);
    ++g_lookup_memo_serial;
    --g_lookup_memo_active;
    for (unsigned i = 0; i < LOOKUP_MEMO_N; ++i)
        g_lookup_memo[i].ino = 0;
    pthread_mutex_unlock(&g_lookup_memo_mu);
}

static void lookup_memo_put(const struct efs_inode *row)
{
    unsigned i;

    if (!row || !row->ino)
        return;
    pthread_mutex_lock(&g_lookup_memo_mu);
    if (!t_lookup_memo_serial || g_lookup_memo_active ||
        t_lookup_memo_serial != g_lookup_memo_serial) {
        pthread_mutex_unlock(&g_lookup_memo_mu);
        return;
    }
    i = g_lookup_memo_i++ % LOOKUP_MEMO_N;
    g_lookup_memo[i].ino = row->ino;
    g_lookup_memo[i].row = *row;
    g_lookup_memo[i].us = fuse_now_us();
    pthread_mutex_unlock(&g_lookup_memo_mu);
}

static int lookup_memo_take(efs_ino_t ino, struct efs_inode *out)
{
    uint64_t now;
    int i;

    if (!ino || !out)
        return 0;
    now = fuse_now_us();
    pthread_mutex_lock(&g_lookup_memo_mu);
    if (g_lookup_memo_active) {
        pthread_mutex_unlock(&g_lookup_memo_mu);
        return 0;
    }
    for (i = 0; i < LOOKUP_MEMO_N; i++) {
        if (g_lookup_memo[i].ino == ino &&
            now - g_lookup_memo[i].us < LOOKUP_MEMO_US) {
            *out = g_lookup_memo[i].row;
            g_lookup_memo[i].ino = 0;
            pthread_mutex_unlock(&g_lookup_memo_mu);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_lookup_memo_mu);
    return 0;
}

static void getattr_note_row(const struct efs_inode *row)
{
    if (!row || !row->ino)
        return;
    t_getattr_row = *row;
    t_getattr_ino = row->ino;
}

static int efs_fuse_getattr_ino(fuse_ino_t ino, struct stat *stbuf,
                                struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    int rc;
    if (vk == 1) {
        struct efs_inode parent;
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        if (efs_client_stat_ino(virt_parent(ino), &parent) != EFS_OK)
            return -ENOENT;
        rc = stats_fill_stat(&parent, stbuf);
        if (rc == 0)
            stbuf->st_ino = ino;
        return rc;
    }
    if (vk == 2) {
        struct efs_inode parent;
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        if (efs_client_stat_ino(virt_parent(ino), &parent) != EFS_OK)
            return -ENOENT;
        rc = find_dir_fill_stat(&parent, stbuf);
        if (rc == 0)
            stbuf->st_ino = ino;
        return rc;
    }
    if (vk == 3) {
        efs_ino_t dir_ino;
        char term[EFS_MAX_NAME];
        char dir_fuse[EFS_MAX_PATH];
        char query_path[EFS_MAX_PATH];
        struct efs_inode parent;
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        if (vq_lookup(ino, &dir_ino, term, sizeof(term)) != 0)
            return -ENOENT;
        if (efs_client_stat_ino(dir_ino, &parent) != EFS_OK)
            return -ENOENT;
        if (ino_to_fuse_path(dir_ino, dir_fuse, sizeof(dir_fuse)) != 0)
            return -ENOENT;
        if (strcmp(dir_fuse, "/") == 0)
            snprintf(query_path, sizeof(query_path), "/%s/%s",
                     EFS_FIND_NAME, term);
        else
            snprintf(query_path, sizeof(query_path), "%s/%s/%s",
                     dir_fuse, EFS_FIND_NAME, term);
        rc = find_query_fill_stat(&parent, query_path, term, stbuf);
        if (rc == 0)
            stbuf->st_ino = ino;
        return rc;
    }

    /* Open-fd size stays local: a full adopt pulls a peer REPORT over
     * unflushed bytes (posix2 overlap pwrite) and a missing ghost is
     * ENOENT (nlink_after_unlink_open). nlink is not local — the peer
     * that unlinks the last name is the one that commits 0.
     * fi->fh resolves to the ino for files; opendir stores a dirh pointer, so
     * only trust fh when it names an open file. */
    {
        efs_ino_t id = (efs_ino_t)ino;
        if (fi && fi->fh && efs_client_ino_is_open(efs_file_ino(fi, 0)))
            id = efs_file_ino(fi, 0);
        if (id && efs_client_ino_is_open(id)) {
            struct efs_inode row;
            if (efs_client_stat_open(id, &row) == EFS_OK) {
                getattr_note_row(&row);
                fill_stat_from_inode(stbuf, &row);
                return 0;
            }
        }
    }

    {
        struct efs_inode row;
        if (lookup_memo_take((efs_ino_t)ino, &row)) {
            getattr_note_row(&row);
            fill_stat_from_inode(stbuf, &row);
            return 0;
        }
    }

    struct efs_inode row;
    rc = efs_client_stat_refresh((efs_ino_t)ino, &row);
    if (rc == EFS_ERR_ACCES)
        return -EACCES;
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    getattr_note_row(&row);
    fill_stat_from_inode(stbuf, &row);
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

/* Per-opendir listing. fi->fh is this pointer (files carry an open-description handle).
 * First READDIR fills it; later READDIR / EOF getdents reuse it. */
struct efs_dirh {
    efs_ino_t ino;
    struct readdir_ent *ents;
    size_t count;
    fuse_ino_t parent_ino;
    mode_t self_mode;
    mode_t parent_mode;
    int listed;
};

static int efs_fuse_readdir_ino(fuse_ino_t ino, struct readdir_collect_arg *col,
                                struct efs_inode *self_out);

static struct efs_dirh *dirh_new(efs_ino_t ino)
{
    struct efs_dirh *dh = calloc(1, sizeof(*dh));
    if (!dh)
        return NULL;
    dh->ino = ino;
    dh->parent_ino = FUSE_ROOT_ID;
    dh->self_mode = S_IFDIR | 0755;
    dh->parent_mode = S_IFDIR | 0755;
    return dh;
}

static void dirh_free(struct efs_dirh *dh)
{
    if (!dh)
        return;
    free(dh->ents);
    free(dh);
}

static int dirh_fill(struct efs_dirh *dh, fuse_ino_t ino)
{
    struct readdir_collect_arg col;
    struct efs_inode row, prow;
    int rc;

    memset(&col, 0, sizeof(col));
    memset(&row, 0, sizeof(row));
    rc = efs_fuse_readdir_ino(ino, &col, &row);
    if (rc != 0 && virt_kind(ino) != 2) {
        free(col.ents);
        return rc;
    }
    dh->ents = col.ents;
    dh->count = col.count;
    dh->self_mode = S_IFDIR | 0755;
    dh->parent_mode = S_IFDIR | 0755;
    dh->parent_ino = FUSE_ROOT_ID;
    /* The listing's own stat of this dir is the row; no second table walk. */
    if (row.ino != 0 || efs_client_stat_ino((efs_ino_t)ino, &row) == EFS_OK) {
        dh->self_mode = row.mode;
        if (row.parent)
            dh->parent_ino = (fuse_ino_t)row.parent;
    }
    if (virt_kind(ino) == 2)
        dh->parent_ino = virt_parent(ino);
    if (efs_client_stat_ino((efs_ino_t)dh->parent_ino, &prow) == EFS_OK)
        dh->parent_mode = prow.mode;
    dh->listed = 1;
    return 0;
}

/* Append one page, skipping name dups (spread dirs merge per-shard scans).
 * Returns 0 or -ENOMEM. */
static int readdir_collect_page(struct readdir_collect_arg *col,
                                const struct efs_inode *ents, uint32_t n)
{
    if (col->count + n > col->cap) {
        size_t ncap = col->cap ? col->cap : 16;
        while (ncap < col->count + n)
            ncap *= 2;
        struct readdir_ent *ne = realloc(col->ents, ncap * sizeof(*ne));
        if (!ne)
            return -ENOMEM;
        col->ents = ne;
        col->cap = ncap;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (ents[i].ino == 0 || ents[i].name[0] == '\0')
            continue;
        if (strncmp(ents[i].name, ".fuse_hidden", 12) == 0)
            continue;
        if (strncmp(ents[i].name, ".parked-", 8) == 0)
            continue;
        int dup = 0;
        for (size_t j = 0; j < col->count; j++) {
            if (strcmp(col->ents[j].name, ents[i].name) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        struct readdir_ent *e = &col->ents[col->count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, ents[i].name, EFS_MAX_NAME - 1);
        e->st.st_ino = ents[i].ino;
        e->st.st_mode = ents[i].mode;
    }
    return 0;
}

static int efs_fuse_readdir_ino(fuse_ino_t ino, struct readdir_collect_arg *col,
                                struct efs_inode *self_out)
{
    if (virt_kind(ino) == 2) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return 0; /* .find lists nothing; queries are explicit lookups */
    }
    if (virt_kind(ino))
        return -ENOTDIR;

    struct efs_inode parent;
    int rc = efs_client_stat_ino((efs_ino_t)ino, &parent);
    if (rc == EFS_ERR_ACCES)
        return -EACCES;
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    {
        const struct fuse_ctx *ctx = ll_ctx();
        if (parent.ino != EFS_ROOT_INO && ctx &&
            check_access(&parent, ctx->uid, ctx->gid, R_OK) != 0)
            return -EACCES;
    }
    if (self_out)
        *self_out = parent;

    uint32_t src = 0, done = 0;
    char name_cur[EFS_MAX_NAME] = "";
    while (!done) {
        struct efs_inode ents[EFS_READDIR_MAX];
        uint32_t n = EFS_READDIR_MAX;
        rc = efs_client_rpc_readdir_cur(g_client.export_id, parent.ino,
                                        ents, &n, &src, name_cur, &done);
        if (rc != 0)
            return (rc == EFS_ERR_NOT_FOUND) ? -ENOENT : -EIO;
        if (readdir_collect_page(col, ents, n) != 0)
            return -ENOMEM;
        if (n == 0 && !done)
            break;
    }
    return 0;
}

static int efs_fuse_access_ino(fuse_ino_t ino, int mask)
{
    if (virt_kind(ino))
        return (mask & W_OK) ? -EACCES : 0;
    struct efs_inode row;
    int lrc = efs_client_stat_ino((efs_ino_t)ino, &row);
    if (lrc == EFS_ERR_ACCES)
        return -EACCES;
    if (lrc != EFS_OK)
        return -ENOENT;
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    return check_access(&row, ctx->uid, ctx->gid, mask);
}

static int efs_fuse_truncate_ino(fuse_ino_t ino, off_t size,
                                 struct fuse_file_info *fi);

/* Per-ino open-description count. The server HOLD (open lease) is an edge
 * trigger: acquired at this client's first open of the ino, released at the
 * last close (one lease per (session, inode)). The kernel never relays a
 * flock UNLOCK on close, so the last-close lease edge is also what drops
 * the inode's server-side locks. */
static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;
struct efs_open_ref {
    efs_ino_t ino;
    uint64_t n;
    int leased; /* first open took HOLD; last close must release it */
    struct efs_open_ref *next;
};
static struct efs_open_ref *g_open_refs;

#define OPEN_EDGE_SHARDS 64
static pthread_mutex_t g_open_edge_mu[OPEN_EDGE_SHARDS];
static pthread_once_t g_open_edge_once = PTHREAD_ONCE_INIT;
static void open_edge_init(void)
{
    for (unsigned i = 0; i < OPEN_EDGE_SHARDS; ++i)
        pthread_mutex_init(&g_open_edge_mu[i], NULL);
}
static pthread_mutex_t *open_edge_mu(efs_ino_t ino)
{
    pthread_once(&g_open_edge_once, open_edge_init);
    return &g_open_edge_mu[ino % OPEN_EDGE_SHARDS];
}

/* Inodes this client has actually locked. Last close otherwise sent
 * LOCK_UN for every file (a Raft round trip) so a skipped hold could
 * not leave flock_unlock_on_close stuck. */
#define FLOCK_ARMED_MAX 64
struct flock_arm_slot {
    efs_ino_t ino;
    uint64_t owner;
};
static struct flock_arm_slot g_flock_armed[FLOCK_ARMED_MAX];
static int g_flock_armed_n;
static int g_flock_arm_all;
static pthread_mutex_t g_flock_armed_mu = PTHREAD_MUTEX_INITIALIZER;

static void flock_arm(efs_ino_t ino, uint64_t owner)
{
    int i;

    if (!ino)
        return;
    pthread_mutex_lock(&g_flock_armed_mu);
    for (i = 0; i < g_flock_armed_n; i++) {
        if (g_flock_armed[i].ino == ino) {
            g_flock_armed[i].owner = owner;
            pthread_mutex_unlock(&g_flock_armed_mu);
            return;
        }
    }
    if (g_flock_armed_n < FLOCK_ARMED_MAX) {
        g_flock_armed[g_flock_armed_n].ino = ino;
        g_flock_armed[g_flock_armed_n].owner = owner;
        g_flock_armed_n++;
    } else {
        g_flock_arm_all = 1;
    }
    pthread_mutex_unlock(&g_flock_armed_mu);
}

/* 1 when this close must LOCK_UN. Writes the owner that took the lock. */
static int flock_armed_take(efs_ino_t ino, uint64_t *owner_out)
{
    int i, hit = 0;

    pthread_mutex_lock(&g_flock_armed_mu);
    if (g_flock_arm_all)
        hit = 1;
    for (i = 0; i < g_flock_armed_n; i++) {
        if (g_flock_armed[i].ino == ino) {
            if (owner_out)
                *owner_out = g_flock_armed[i].owner;
            g_flock_armed[i] = g_flock_armed[--g_flock_armed_n];
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_flock_armed_mu);
    return hit;
}

/* Returns 1 when this is the first open (caller must acquire the HOLD). */
static int efs_open_note(efs_ino_t ino)
{
    struct efs_open_ref *r;
    int first = EFS_ERR_NOMEM;

    pthread_mutex_lock(&g_open_mu);
    for (r = g_open_refs; r; r = r->next)
        if (r->ino == ino)
            break;
    if (!r) {
        r = calloc(1, sizeof(*r));
        if (r) {
            r->ino = ino;
            r->next = g_open_refs;
            g_open_refs = r;
        }
    }
    if (r) {
        first = (r->n == 0);
        r->n++;
    }
    pthread_mutex_unlock(&g_open_mu);
    return first;
}

static void efs_open_set_leased(efs_ino_t ino)
{
    struct efs_open_ref *r;

    pthread_mutex_lock(&g_open_mu);
    for (r = g_open_refs; r; r = r->next)
        if (r->ino == ino) {
            r->leased = 1;
            break;
        }
    pthread_mutex_unlock(&g_open_mu);
}

/* Returns 1 when this was the last close. *leased is 1 when that close
 * must release the HOLD taken on the first open. */
static int efs_close_note(efs_ino_t ino, int *leased)
{
    struct efs_open_ref **pp, *r;
    int last = 0;

    if (leased)
        *leased = 0;
    pthread_mutex_lock(&g_open_mu);
    for (pp = &g_open_refs; (r = *pp); pp = &r->next) {
        if (r->ino == ino) {
            if (r->n > 0)
                r->n--;
            if (r->n == 0) {
                if (leased)
                    *leased = r->leased;
                *pp = r->next;
                free(r);
                last = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_open_mu);
    return last;
}

/* Every returned regular descriptor, including CREATE, owns a server lease.
 * The edge stripe prevents a second open returning before the first HOLD,
 * and prevents a delayed last-close HOLD release deleting a new lease. */
static int efs_open_acquire(efs_ino_t ino)
{
    pthread_mutex_t *edge = open_edge_mu(ino);
    pthread_mutex_lock(edge);
    int first = efs_open_note(ino);
    int rc = first < 0 ? first : EFS_OK;
    if (first > 0) {
        rc = efs_client_rpc_hold(g_client.export_id, ino, 1,
                                  g_client.flock_token);
        if (rc == EFS_OK)
            efs_open_set_leased(ino);
        else
            (void)efs_close_note(ino, NULL);
    }
    pthread_mutex_unlock(edge);
    return rc;
}

/* Client-cache design Part A, pin rule 2: a ghost (nlink=0) row is
 * evictable only once no fd has it open. */
int efs_client_ino_is_open(efs_ino_t ino)
{
    struct efs_open_ref *r;
    int open = 0;

    pthread_mutex_lock(&g_open_mu);
    for (r = g_open_refs; r; r = r->next)
        if (r->ino == ino) {
            open = 1;
            break;
        }
    pthread_mutex_unlock(&g_open_mu);
    return open;
}

/* Part D: kernel page cache is the coherence hole. FOPEN_DIRECT_IO on
 * every regular open (not just O_DIRECT). Application O_DIRECT still
 * requires 4 KiB alignment; FOPEN_DIRECT_IO does not — the kernel
 * sends ordinary unaligned FUSE reads/writes once cache is bypassed.
 * libfuse 3.10.2 fuse_reply_open cannot set FOPEN_PARALLEL_DIRECT_WRITES.
 * Requests ARE 1 MiB here (Oct 1 2026 strace of a 1 MiB dd: read(/dev/fuse)
 * = 1048656, reply writev = 1048592): libfuse 3.10.2 negotiates
 * FUSE_MAX_PAGES with max_write, so a 1 MiB write() is one FUSE request
 * and eight 128 KiB chunks per ll_write_buf. */
static void fuse_fi_direct_io(struct fuse_file_info *fi)
{
    if (!fi)
        return;
    fi->direct_io = 1;
    fi->keep_cache = 0;
}

static int fuse_odirect_unaligned(const struct fuse_file_info *fi,
                                  off_t offset, size_t size)
{
    return fi && (fi->flags & O_DIRECT) &&
           ((offset & 4095) || (size & 4095));
}

static int efs_fuse_open_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    if (vk == 1) {
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        if (fi) {
            fi->fh = ino;
            fuse_fi_direct_io(fi);
        }
        return 0;
    }
    if (vk == 3) {
        efs_ino_t dir_ino;
        char term[EFS_MAX_NAME];
        char dir_fuse[EFS_MAX_PATH];
        char query_path[EFS_MAX_PATH];
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        if (vq_lookup(ino, &dir_ino, term, sizeof(term)) != 0)
            return -ENOENT;
        if (ino_to_fuse_path(dir_ino, dir_fuse, sizeof(dir_fuse)) != 0)
            return -ENOENT;
        if (strcmp(dir_fuse, "/") == 0)
            snprintf(query_path, sizeof(query_path), "/%s/%s",
                     EFS_FIND_NAME, term);
        else
            snprintf(query_path, sizeof(query_path), "%s/%s/%s",
                     dir_fuse, EFS_FIND_NAME, term);
        if (find_query_len(dir_ino, query_path, term) < 0)
            return -ENOENT;
        if (fi) {
            fi->fh = ino;
            fuse_fi_direct_io(fi);
        }
        return 0;
    }
    if (vk == 2)
        return -EISDIR;

    struct efs_inode row;
    int lrc = efs_client_stat_ino((efs_ino_t)ino, &row);
    if (lrc != EFS_OK)
        return -ENOENT;
    {
        const struct fuse_ctx *ctx = ll_ctx();
        int accmode = fi ? (fi->flags & O_ACCMODE) : O_RDONLY;
        int mask = 0;
        if (accmode != O_WRONLY)
            mask |= R_OK;
        if (accmode != O_RDONLY)
            mask |= W_OK;
        if (mask && ctx && check_access(&row, ctx->uid, ctx->gid, mask) != 0)
            return -EACCES;
    }
    if (fi) {
        fi->fh = row.ino;
        fuse_fi_direct_io(fi);
    }
    if (fi && (fi->flags & O_TRUNC) && (fi->flags & O_ACCMODE) != O_RDONLY) {
        if (efs_mode_is_dir(row.mode))
            return -EISDIR;
        int trc = efs_fuse_truncate_ino(row.ino, 0, fi);
        if (trc != 0)
            return trc;
    }
    if (fi && !efs_mode_is_dir(row.mode)) {
        int frc = efs_file_open(fi, row.ino);
        if (frc)
            return frc;
    }
    if (fi && !efs_mode_is_dir(row.mode)) {
        int hrc = efs_open_acquire(row.ino);
        if (hrc != EFS_OK) {
            efs_file_close(fi);
            return fuse_stat_errno(hrc);
        }
    }
    return 0;
}

static void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path);
static int efs_wb_sync(void);

static int efs_fuse_read_ino(fuse_ino_t ino, char *buf, size_t size, off_t offset,
                             struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    if (vk == 1) {
        struct stats_ent e;
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        if (stats_snapshot(virt_parent(ino), &e) != 0)
            return -ENOENT;
        if (offset >= e.len)
            return 0;
        size_t avail = (size_t)e.len - (size_t)offset;
        if (avail > size)
            avail = size;
        memcpy(buf, e.text + offset, avail);
        return (int)avail;
    }
    if (vk == 3) {
        efs_ino_t dir_ino;
        char term[EFS_MAX_NAME];
        char dir_fuse[EFS_MAX_PATH];
        char query_path[EFS_MAX_PATH];
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        if (vq_lookup(ino, &dir_ino, term, sizeof(term)) != 0)
            return -ENOENT;
        if (ino_to_fuse_path(dir_ino, dir_fuse, sizeof(dir_fuse)) != 0)
            return -ENOENT;
        if (strcmp(dir_fuse, "/") == 0)
            snprintf(query_path, sizeof(query_path), "/%s/%s",
                     EFS_FIND_NAME, term);
        else
            snprintf(query_path, sizeof(query_path), "%s/%s/%s",
                     dir_fuse, EFS_FIND_NAME, term);
        return find_query_read(dir_ino, query_path, term, buf, size, offset);
    }
    if (vk == 2)
        return -EISDIR;

    efs_ino_t file = efs_file_ino(fi, (efs_ino_t)ino);
    if (fuse_odirect_unaligned(fi, offset, size))
        return -EINVAL;

    size_t got = 0;
    int rc = efs_client_read(file, (uint64_t)offset, size, buf, &got);
    if (rc != 0) {
        efs_fuse_log_err("read", rc, file, (uint64_t)offset, size, NULL);
        return -EIO;
    }
    return (int)got;
}

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
} g_wb = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
};

/* glibc mmaps allocations >= 128 KiB. Every 128k/1M write_buf was then
 * mmap + 256 page-faults + munmap, so a single psync stream's FUSE ACK
 * took about as long as the PUT and the WB queue never piled up. */
#define EFS_BOUNCE_POOL   256
#define EFS_BOUNCE_SMALL  (128u << 10)
#define EFS_BOUNCE_LARGE  (1u << 20)

static struct {
    pthread_mutex_t mu;
    char *p[EFS_BOUNCE_POOL];
    size_t cap[EFS_BOUNCE_POOL];
    int n;
} g_bounce = { .mu = PTHREAD_MUTEX_INITIALIZER };

static char *bounce_fresh(size_t cap)
{
    char *p = malloc(cap);
    if (!p)
        return NULL;
    /* Fault the pages once so the first write_buf memcpy is not the tax. */
    for (size_t o = 0; o < cap; o += 4096)
        p[o] = 0;
    if (cap)
        p[cap - 1] = 0;
    return p;
}

static char *bounce_alloc(size_t size, size_t *cap_out)
{
    if (size < EFS_BOUNCE_SMALL) {
        char *p = malloc(size);
        if (cap_out)
            *cap_out = 0;
        return p;
    }
    size_t want = (size <= EFS_BOUNCE_LARGE) ? EFS_BOUNCE_LARGE : size;
    if (size <= EFS_BOUNCE_SMALL)
        want = EFS_BOUNCE_SMALL;
    pthread_mutex_lock(&g_bounce.mu);
    for (int i = g_bounce.n - 1; i >= 0; i--) {
        if (g_bounce.cap[i] >= size) {
            char *p = g_bounce.p[i];
            size_t cap = g_bounce.cap[i];
            g_bounce.n--;
            g_bounce.p[i] = g_bounce.p[g_bounce.n];
            g_bounce.cap[i] = g_bounce.cap[g_bounce.n];
            pthread_mutex_unlock(&g_bounce.mu);
            if (cap_out)
                *cap_out = cap;
            return p;
        }
    }
    pthread_mutex_unlock(&g_bounce.mu);
    char *p = bounce_fresh(want);
    if (cap_out)
        *cap_out = p ? want : 0;
    return p;
}

static void bounce_release(char *p, size_t cap)
{
    if (!p)
        return;
    if (!cap) {
        free(p);
        return;
    }
    pthread_mutex_lock(&g_bounce.mu);
    if (g_bounce.n < EFS_BOUNCE_POOL) {
        g_bounce.p[g_bounce.n] = p;
        g_bounce.cap[g_bounce.n] = cap;
        g_bounce.n++;
        pthread_mutex_unlock(&g_bounce.mu);
        return;
    }
    pthread_mutex_unlock(&g_bounce.mu);
    free(p);
}

static void wb_job_release_buf(char *copy, char *free_base, size_t buf_cap)
{
    char *p = free_base ? free_base : copy;
    bounce_release(p, buf_cap);
}

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

static int wb_ranges_overlap(uint64_t a0, size_t al, uint64_t b0, size_t bl)
{
    if (!al || !bl)
        return 0;
    return a0 < b0 + (uint64_t)bl && b0 < a0 + (uint64_t)al;
}

/* Expand a partial write to whole chunks so two 4k RMWs of the same
 * 128 KiB chunk cannot GET/PUT in parallel (lost update → later EIO). */
static void wb_claim_span(uint64_t off, size_t len, uint32_t cs,
                          uint64_t *out_off, size_t *out_len)
{
    if (!cs || !len ||
        (len >= cs && (off % cs) == 0 && (len % cs) == 0)) {
        *out_off = off;
        *out_len = len;
        return;
    }
    uint64_t a = (off / cs) * cs;
    uint64_t b = (off + (uint64_t)len + cs - 1) / cs * cs;
    *out_off = a;
    *out_len = (size_t)(b - a);
}

/* Caller holds g_wb.mu. True if another worker already claimed an
 * overlapping range of this inode (aligned PUT vs RMW GET race). */
static int wb_overlap_inflight(int self, efs_ino_t ino, uint64_t off, size_t len)
{
    for (int i = 0; i < g_wb.nworkers; i++) {
        if (i == self || g_wb.busy_ino[i] != ino || !g_wb.busy_len[i])
            continue;
        if (wb_ranges_overlap(off, len, g_wb.busy_off[i], g_wb.busy_len[i]))
            return 1;
    }
    return 0;
}

/* Deadline-aware waits preserve accepted-job ownership until completion. */
static int wb_wait_idle_budget(void)
{
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (!deadline) {
        return pthread_cond_wait(&g_wb.idle, &g_wb.mu) == 0
               ? EFS_OK : EFS_ERR_IO;
    }
    uint64_t now = stats_now_ms();
    if (now >= deadline)
        return EFS_ERR_BUSY;
    uint64_t wait = deadline - now;
    if (wait > 50) wait = 50;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)wait * 1000000;
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
    int rc = pthread_cond_timedwait(&g_wb.idle, &g_wb.mu, &until);
    if (rc && rc != ETIMEDOUT)
        return EFS_ERR_IO;
    return efs_client_rpc_past_deadline() ? EFS_ERR_BUSY : EFS_OK;
}

static int wb_lock_budget(pthread_mutex_t *mu)
{
    if (!efs_client_rpc_deadline_ms())
        return pthread_mutex_lock(mu) == 0 ? EFS_OK : EFS_ERR_IO;
    for (;;) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        int rc = pthread_mutex_trylock(mu);
        if (!rc)
            return EFS_OK;
        if (rc != EBUSY)
            return EFS_ERR_IO;
        usleep(1000);
    }
}

static void *efs_wb_thread(void *arg)
{
    int wid = (int)(intptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&g_wb.mu);
        while (g_wb.count == 0 && !g_wb.shutdown)
            pthread_cond_wait(&g_wb.not_empty, &g_wb.mu);
        if (g_wb.shutdown && g_wb.count == 0) {
            pthread_mutex_unlock(&g_wb.mu);
            return NULL;
        }
        struct efs_wb_job job = g_wb.q[g_wb.head];
        uint64_t previous = efs_client_rpc_deadline_ms();
        efs_client_rpc_set_deadline_ms(job.deadline);
        int rc = EFS_OK;
        g_wb.head = (g_wb.head + 1) % EFS_WB_DEPTH;
        g_wb.count--;
        g_wb.queued_bytes -= job.size;
        g_wb.inflight_bytes += job.size;
        g_wb.inflight++;
        pthread_cond_signal(&g_wb.not_full);
        /* Own the inode from the pop on. A job that was waiting below
         * for an overlapping in-flight job was in neither the queue nor
         * busy_ino[], so efs_wb_ino_pending_locked() said "nothing
         * pending" between the first job's finish and this worker
         * re-taking the mutex: utimens flushed + SETATTR, then this
         * job's PUT re-dirtied the chunk and close published it under
         * the new mtime_gen — the ecopy write(32k) + pwrite(tail) →
         * futimens → close files whose mtime was the close time
         * (Sep 30, ecopy.strace 03:17:43, 29 of ~10k files).
         * busy_len stays 0 until the claim, so wb_overlap_inflight()
         * skips this slot and two waiters cannot deadlock on each
         * other. */
        g_wb.busy_ino[wid] = job.ino;
        g_wb.busy_off[wid] = 0;
        g_wb.busy_len[wid] = 0;
        /* Claim the range only after overlapping in-flight jobs finish.
         * writeback_cache can deliver an unaligned window while a full
         * overwrite of the same chunks is still PUTting; RMW GET then
         * misses and used to return EIO. Partial 4k jobs claim the
         * whole chunk so they do not RMW the same 128 KiB in parallel. */
        uint32_t claim_cs = fuse_chunk_size();
        uint64_t claim_off = job.offset;
        size_t claim_len = job.size;
        wb_claim_span(job.offset, job.size, claim_cs, &claim_off, &claim_len);
        while (wb_overlap_inflight(wid, job.ino, claim_off, claim_len)) {
            rc = wb_wait_idle_budget();
            if (rc != EFS_OK)
                break;
        }
        if (efs_client_rpc_past_deadline())
            rc = EFS_ERR_BUSY;
        if (rc == EFS_OK) {
            g_wb.busy_off[wid] = claim_off;
            g_wb.busy_len[wid] = claim_len;
        }
        pthread_mutex_unlock(&g_wb.mu);

        /* Partial-chunk RMW is not atomic. Full-chunk overwrites of
         * distinct ranges are independent — the per-ino lock used to
         * force 128k×8 and 1M×1 through one PUT at a time. */
        uint32_t cs = fuse_chunk_size();
        int aligned = cs && job.size >= cs &&
                      (job.offset % cs) == 0 && (job.size % cs) == 0;
        pthread_mutex_t *ilock = NULL;
        if (rc == EFS_OK && !aligned) {
            ilock = efs_wb_ino_lock(job.ino);
            rc = wb_lock_budget(ilock);
            if (rc != EFS_OK)
                ilock = NULL;
        }
        if (rc == EFS_OK) {
            lookup_memo_mutation_begin();
            rc = efs_client_write_no_replicate(job.ino, job.offset, job.size,
                                              job.buf);
            lookup_memo_mutation_end();
        }
        if (ilock)
            pthread_mutex_unlock(ilock);
        wb_job_release_buf(job.buf, job.free_base, job.buf_cap);
        efs_client_rpc_set_deadline_ms(previous);

        pthread_mutex_lock(&g_wb.mu);
        if (job.done) {
            *job.done_rc = rc;
            *job.done = 1;
            pthread_cond_signal(job.done_cv);
        }
        if (rc != EFS_OK) {
            if (g_wb.err == EFS_OK) {
                g_wb.err = rc;
                g_wb.err_ino = job.ino;
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
        g_wb.inflight_bytes -= job.size;
        g_wb.busy_ino[wid] = 0;
        g_wb.busy_off[wid] = 0;
        g_wb.busy_len[wid] = 0;
        pthread_cond_broadcast(&g_wb.idle);
        pthread_mutex_unlock(&g_wb.mu);
    }
}

static int wb_worker_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 4;
    n -= EFS_WB_RESERVED;
    if (n < 4)
        n = 4;
    if (n > EFS_WB_WORKERS_MAX)
        n = EFS_WB_WORKERS_MAX;
    return (int)n;
}

static int efs_wb_ensure(void)
{
    if (g_wb.ready)
        return 0;
    pthread_mutex_lock(&g_wb.mu);
    if (!g_wb.ready) {
        g_wb.nworkers = wb_worker_count();
        for (int i = 0; i < g_wb.nworkers; i++) {
            if (pthread_create(&g_wb.workers[i], NULL, efs_wb_thread,
                               (void *)(intptr_t)i) != 0) {
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

/* Enqueue a WB job taking ownership of an already-allocated buffer.
 * free_base, if non-NULL, is the pointer to free when the job finishes
 * (stolen FUSE receive buffer); otherwise `copy` is released.
 * buf_cap != 0 means the buffer came from bounce_alloc and goes back
 * to the pool; 0 means a plain malloc.
 * Blocks until the PUT has 2-of-3 fragment quorum (or fails). */
static int efs_wb_enqueue_owned(efs_ino_t ino, uint64_t offset, size_t size,
                                char *copy, char *free_base, size_t buf_cap)
{
    if (efs_wb_ensure() != 0) {
        wb_job_release_buf(copy, free_base, buf_cap);
        return EFS_ERR_NOMEM;
    }
    int done = 0;
    int job_rc = EFS_ERR_IO;
    pthread_cond_t done_cv;
    pthread_cond_init(&done_cv, NULL);
    pthread_mutex_lock(&g_wb.mu);
    /* Do not refuse new files because an earlier inode's PUT failed.
     * That sticky g_wb.err used to make every later write/close EIO
     * (including empty touch) after one quorum blip. */
    uint64_t wait_end = stats_now_ms() + 8000;
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (outer && outer < wait_end)
        wait_end = outer;
    int waited = 0;
    while (g_wb.count == EFS_WB_DEPTH && !g_wb.shutdown) {
        uint64_t now = stats_now_ms();
        if (!waited) {
            waited = 1;
            ++g_wb.admission_waits;
            fprintf(stderr, "write-queue pressure jobs=%d queued_bytes=%llu "
                    "inflight_bytes=%llu oldest_ms=%llu waits=%llu timeouts=%llu\n",
                    g_wb.count, (unsigned long long)g_wb.queued_bytes,
                    (unsigned long long)g_wb.inflight_bytes,
                    (unsigned long long)(now - g_wb.q[g_wb.head].queued_ms),
                    (unsigned long long)g_wb.admission_waits,
                    (unsigned long long)g_wb.admission_timeouts);
        }
        if (now >= wait_end) {
            ++g_wb.admission_timeouts;
            pthread_mutex_unlock(&g_wb.mu);
            pthread_cond_destroy(&done_cv);
            wb_job_release_buf(copy, free_base, buf_cap);
            return EFS_ERR_BUSY; /* not enqueued; no worker owns caller state */
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        uint64_t wait = wait_end - now;
        if (wait > 50) wait = 50;
        until.tv_nsec += (long)wait * 1000000;
        until.tv_sec += until.tv_nsec / 1000000000;
        until.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&g_wb.not_full, &g_wb.mu, &until);
    }
    if (g_wb.shutdown) {
        pthread_mutex_unlock(&g_wb.mu);
        pthread_cond_destroy(&done_cv);
        wb_job_release_buf(copy, free_base, buf_cap);
        return EFS_ERR_IO;
    }
    if (stats_now_ms() >= wait_end) {
        ++g_wb.admission_timeouts;
        pthread_mutex_unlock(&g_wb.mu);
        pthread_cond_destroy(&done_cv);
        wb_job_release_buf(copy, free_base, buf_cap);
        return EFS_ERR_BUSY;
    }
    g_wb.q[g_wb.tail].deadline = outer ? outer : stats_now_ms() + 30000;
    g_wb.q[g_wb.tail].ino = ino;
    g_wb.q[g_wb.tail].offset = offset;
    g_wb.q[g_wb.tail].size = size;
    g_wb.q[g_wb.tail].buf = copy;
    g_wb.q[g_wb.tail].free_base = free_base;
    g_wb.q[g_wb.tail].buf_cap = buf_cap;
    g_wb.q[g_wb.tail].queued_ms = stats_now_ms();
    g_wb.queued_bytes += size;
    g_wb.q[g_wb.tail].done = &done;
    g_wb.q[g_wb.tail].done_rc = &job_rc;
    g_wb.q[g_wb.tail].done_cv = &done_cv;
    g_wb.tail = (g_wb.tail + 1) % EFS_WB_DEPTH;
    g_wb.count++;
    pthread_cond_signal(&g_wb.not_empty);
    while (!done)
        pthread_cond_wait(&done_cv, &g_wb.mu);
    pthread_mutex_unlock(&g_wb.mu);
    pthread_cond_destroy(&done_cv);
    return job_rc;
}

static int efs_wb_sync(void)
{
    if (!g_wb.ready)
        return EFS_OK;
    int rc = wb_lock_budget(&g_wb.mu);
    if (rc != EFS_OK)
        return rc;
    while (g_wb.count > 0 || g_wb.inflight > 0) {
        rc = wb_wait_idle_budget();
        if (rc != EFS_OK) {
            pthread_mutex_unlock(&g_wb.mu);
            return rc;
        }
    }
    int err = g_wb.err;
    g_wb.err = EFS_OK;
    g_wb.err_ino = 0;
    pthread_mutex_unlock(&g_wb.mu);
    return err;
}

static int efs_wb_ino_pending_locked(efs_ino_t ino)
{
    for (int i = 0; i < g_wb.count; i++) {
        int idx = (g_wb.head + i) % EFS_WB_DEPTH;
        if (g_wb.q[idx].ino == ino)
            return 1;
    }
    for (int i = 0; i < g_wb.nworkers; i++) {
        if (g_wb.busy_ino[i] == ino)
            return 1;
    }
    return 0;
}

/* Drain only this inode's writeback jobs so close/ecopy does not wait
 * for every other file in the pool. */
static int efs_wb_sync_ino(efs_ino_t ino)
{
    if (!g_wb.ready)
        return EFS_OK;
    int rc = wb_lock_budget(&g_wb.mu);
    if (rc != EFS_OK)
        return rc;
    while (efs_wb_ino_pending_locked(ino)) {
        rc = wb_wait_idle_budget();
        if (rc != EFS_OK) {
            pthread_mutex_unlock(&g_wb.mu);
            return rc;
        }
    }
    int err = EFS_OK;
    if (g_wb.err != EFS_OK && g_wb.err_ino == ino) {
        err = g_wb.err;
        g_wb.err = EFS_OK;
        g_wb.err_ino = 0;
    }
    pthread_mutex_unlock(&g_wb.mu);
    return err;
}

/* Serialize O_APPEND reserve+patch with close/fsync flush. Four threads
 * each write+close: without this, two flushes GET the same unpublished
 * base and the last PUT zeros the other's lines (concurrent_appends
 * 200 reserved bytes, 191 lines, NULs in the middle). */
/* Same-file append writes and close/fsync share one mutex. A single
 * global mutex made every close in the process wait out every other
 * file's REPORT. 64 stripes: one inode always hits the same stripe;
 * unrelated files usually do not. */
#define APPEND_MU_N 64
static pthread_mutex_t g_append_mu[APPEND_MU_N];
static pthread_once_t g_append_mu_once = PTHREAD_ONCE_INIT;

static void append_mu_init(void)
{
    int i;

    for (i = 0; i < APPEND_MU_N; i++)
        pthread_mutex_init(&g_append_mu[i], NULL);
}

static pthread_mutex_t *append_mu(efs_ino_t ino)
{
    pthread_once(&g_append_mu_once, append_mu_init);
    return &g_append_mu[(uint32_t)ino % APPEND_MU_N];
}

/* Close: flush and the REPORT that publishes it are one critical
 * section with O_APPEND writes. FUSE_FLUSH and FUSE_FSYNC do not carry
 * the open flags (fi->flags is zero here), so this cannot key off
 * O_APPEND. A write between the PUT and the REPORT merged into the
 * span still in flight; the next image left that offset zero
 * (concurrent_appends: size 1600, 16 NUL bytes). */
/* 1 if this inode can still have bytes this client has not published:
 * a queued/in-flight wb job, a mark in the dirty set, or a dcache slot
 * that pins it (dirty, or clean-before-PUT inside a PUT window — the pin
 * is held until the slot is dropped). These are the three places
 * unpublished data lives (project-state "nothing to flush"). */
static int efs_ino_has_unpublished(efs_ino_t ino)
{
    int pending = 0;

    if (g_wb.ready) {
        pthread_mutex_lock(&g_wb.mu);
        pending = efs_wb_ino_pending_locked(ino);
        pthread_mutex_unlock(&g_wb.mu);
    }
    return pending || efs_client_ino_is_dirty(ino) ||
           efs_dcache_ino_pinned(ino);
}

static int efs_append_flush_report_run(struct fuse_file_info *fi, efs_ino_t ino)
{
    pthread_mutex_t *amu;
    int rc;

    (void)fi;
    if (!ino || virt_kind(ino))
        return EFS_OK;
    /* Every close(2) — read-only opens and dup'd fds included — reached
     * dcache_flush_ino_pass, which probes size/128 KiB dcache slots (two
     * mutex pairs and a chain walk each, all 65536 slots past 8 GiB)
     * under the append stripe. dcache_steal_dirty was 36.9 % of the
     * client's cycles during an ecopy --verify (Sep 30 2026,
     * results/measure/20260930-205300-ecopy-perf-review). A clean inode
     * has nothing to publish; the REPORT for it would carry no record. */
    if (!efs_ino_has_unpublished(ino))
        return EFS_OK;
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    rc = efs_wb_sync_ino(ino);
    if (rc != EFS_OK)
        return rc;
    amu = append_mu(ino);
    rc = wb_lock_budget(amu);
    if (rc != EFS_OK)
        return rc;
    rc = efs_dcache_flush_ino(ino);
    if (rc == EFS_OK)
        rc = efs_client_report_dirty_ino(ino, 1);
    pthread_mutex_unlock(amu);
    return rc;
}

/* One deadline begins before queued writes and append serialization.
 * Worker RPCs inherit it; accepted jobs retain their completion ownership. */
static int efs_append_flush_report(struct fuse_file_info *fi, efs_ino_t ino)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_append_flush_report_run(fi, ino);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

/* utimens(mtime) on an inode with buffered writes: publish them first
 * (the same flush+REPORT close would run) so the SETATTR that follows
 * is the later event on the server. Clean inode: nothing. */
static int efs_utimens_flush_dirty(efs_ino_t ino)
{
    if (!ino || virt_kind(ino))
        return EFS_OK;
    /* The dirty SET is not "has unpublished data": a threshold REPORT
     * (only_ino=0) snapshots the ino mark that dcache_note_size set and
     * ships an irec-only record while the bytes are still a dirty dcache
     * entry; the set is then clean, the data is not. The dcache pin is
     * held exactly while such entries exist (the evictor keys on the
     * same predicate). Without it this returned early, the SETATTR went
     * out, and the close published the chunk under the new mtime_gen
     * (1–2 of 5760 files per ecopy-shaped burst, Sep 30 trace: the
     * flush line printed with no report, the steal came after the
     * setattr). */
    if (!efs_ino_has_unpublished(ino))
        return EFS_OK;
    return efs_append_flush_report(NULL, ino);
}

/* O_APPEND writes: the kernel sets the offset from its i_size but does not
 * serialize concurrent appends to a FUSE file (no i_rwsem around the
 * read-i_size + write + update-i_size sequence), so two racing appends can
 * read the same i_size and overwrite each other. Serialize them here and
 * re-read the true end from the local table (updated synchronously by each
 * write via dcache_note_size / the writeback worker). */

static off_t append_end_offset(efs_ino_t ino)
{
    struct efs_inode cur;
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    uint64_t size = 0;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0)
        size = cur.size;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return (off_t)size;
}

/* Cross-client append: reserve the region at the metadata owner so two
 * clients never write at the same end offset. A second client's uuid is
 * BUSY while the first still has OPEN reservations (in-place fragments
 * cannot merge two GET+PUTs). Their close REPORT resolves nopen. On
 * success, drop cached copies of the tail so a later flush merges the
 * published peer. Does not fall back to an unreserved local end. */
/* Append deduplication is session-wide, while append mutexes are per inode.
 * Never reuse a sequence on concurrent files or after exhaustion. */
static uint64_t append_next_sequence(void)
{
    uint64_t old = __atomic_load_n(&g_client.append_opid_seq, __ATOMIC_RELAXED);
    for (;;) {
        if (old == UINT64_MAX)
            return 0;
        uint64_t next = old + 1;
        if (__atomic_compare_exchange_n(&g_client.append_opid_seq, &old, next,
                                        1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return next;
    }
}

static int append_reserve_offset(efs_ino_t ino, uint64_t len, off_t *off_out)
{
    uint64_t ns = 0, start = 0, seq;
    int rc = EFS_ERR_BUSY;

    seq = append_next_sequence();
    if (!seq)
        return -EOVERFLOW;
    /* Foreign uuid is BUSY until their close REPORT. 2 ms × 15000 was
     * 500 reserve RPCs/s that stalled the live appender (B's 45 s SSH
     * timeout while A was still writing). Back off; cap ~20 s. */
    {
        int delay_us = 2000;
        for (int attempt = 0; attempt < 400; attempt++) {
            rc = efs_client_rpc_append_reserve(g_client.export_id, ino, len, seq,
                                               &ns, &start);
            if (rc == EFS_OK && ns >= len)
                break;
            if (rc != EFS_ERR_BUSY) {
                fprintf(stderr,
                        "append_rsv ino=%llu len=%llu rc=%d ns=%llu start=%llu\n",
                        (unsigned long long)ino, (unsigned long long)len, rc,
                        (unsigned long long)ns, (unsigned long long)start);
                fflush(stderr);
                break;
            }
            pthread_mutex_unlock(append_mu(ino));
            usleep(delay_us);
            if (delay_us < 50000)
                delay_us *= 2;
            pthread_mutex_lock(append_mu(ino));
        }
    }
    /* Never fall back to an unreserved local end: under 9-way that
     * overlapped another process's reservation and dropped ~40 lines.
     * Do not clamp to local_end either — that overwrites a peer's
     * reserved range with this client's bytes. */
    if (rc != EFS_OK || ns < len)
        return -EIO;
    /* The size below is ahead of the PUT. Pin until the caller has the
     * bytes in dcache or the ino is marked dirty. An early dirty mark
     * would release the server's append barrier before the PUT. */
    uint64_t off = (start <= UINT64_MAX - len && start + len == ns) ? start : (ns - len);
    if (efs_dcache_trace_on())
        fprintf(stderr, "append-reserve ino=%llu seq=%llu start=%llu end=%llu len=%llu\n",
                (unsigned long long)ino, (unsigned long long)seq,
                (unsigned long long)start, (unsigned long long)ns,
                (unsigned long long)len);
    uint32_t reserve_cs = g_client.export.chunk_size ? g_client.export.chunk_size : EFS_CHUNK_SIZE;
    if (efs_write_extent_valid(off, len, reserve_cs) != EFS_OK)
        return -EFBIG;
    efs_client_stage_pin(ino);
    if (len > 0) {
        uint32_t cs = g_client.export.chunk_size ? g_client.export.chunk_size
                                                 : EFS_CHUNK_SIZE;
        for (uint64_t ci = off / cs; ci <= (off + len - 1) / cs; ci++)
            efs_rdcache_invalidate(ino, (uint32_t)ci);
    }
    /* Reflect the reservation locally so same-client readers and the next
     * append see the advanced end before our data flushes. The ino is NOT
     * marked dirty here: the append barrier must release only after the data
     * is PUT, and an early report (periodic reporter) carrying the reflected
     * size would release it prematurely. dcache_put_now marks the ino dirty
     * once the flush lands. */
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 &&
        cur.size < ns)
        efs_export_set_size(&g_client.export, ino, ns);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    if (off_out)
        *off_out = (off_t)off;
    return 0;
}

/* Reserve worst-case chunk capacity before any append range is accepted.
 * Under pressure, use exactly the fsync ordering (queued writes, append
 * stripe, data PUTs, REPORT). A failed drain retains previously owned bytes
 * and rejects this request before touching its data or append reservation. */
static int fuse_validate_write_extent(off_t offset, size_t size)
{
    if (offset < 0)
        return -EINVAL;
    if (size > INT_MAX)
        return -EFBIG;
    uint32_t cs = fuse_chunk_size();
    if (!efs_chunk_size_valid(cs))
        return -EIO;
    return efs_write_extent_valid((uint64_t)offset, size, cs) == EFS_OK ? 0 : -EFBIG;
}

static int fuse_write_admit(size_t size)
{
    uint64_t cs = fuse_chunk_size();
    uint64_t unit = cs < EFS_CHUNK_SIZE ? EFS_CHUNK_SIZE : cs;
    uint64_t chunks, bytes;
    efs_ino_t after = 0;
    int admission;
    if (efs_client_rpc_past_deadline())
        return -EAGAIN;
    if (!size)
        return 0;
    if (!cs || size > UINT32_MAX)
        return -EFBIG;
    chunks = (size + cs - 1) / cs + 2;
    if (chunks > UINT64_MAX / (4 * unit))
        return -EFBIG;
    bytes = chunks * 4 * unit;
    admission = efs_buf_reserve_request(bytes, chunks * EFS_DCACHE_ENTRY_BUDGET);
    if (admission == 0)
        return 0;
    if (admission != EFS_ERR_BUSY)
        return -ENOMEM;
    uint64_t live, reserved, backing, limit, meta, meta_reserved, meta_limit;
    efs_buf_budget_stats(&live, &reserved, &backing, &limit);
    efs_buf_metadata_stats(&meta, &meta_reserved, &meta_limit);
    fprintf(stderr, "dcache-pressure live=%llu reserved=%llu backing=%llu "
            "limit=%llu request=%llu metadata=%llu metadata_reserved=%llu "
            "metadata_limit=%llu metadata_request=%llu failed=%s\n", (unsigned long long)live,
            (unsigned long long)reserved, (unsigned long long)backing,
            (unsigned long long)limit, (unsigned long long)bytes,
            (unsigned long long)meta, (unsigned long long)meta_reserved,
            (unsigned long long)meta_limit,
            (unsigned long long)(chunks * EFS_DCACHE_ENTRY_BUDGET),
            meta + meta_reserved + chunks * EFS_DCACHE_ENTRY_BUDGET > meta_limit
                ? "metadata" : "body");
    efs_rdcache_trim();
    efs_dcache_trim_metadata();
    admission = efs_buf_reserve_request(bytes, chunks * EFS_DCACHE_ENTRY_BUDGET);
    if (admission == 0)
        return 0;
    if (admission != EFS_ERR_BUSY)
        return -ENOMEM;
    /* Finite drain work, no cache/append mutex held here.
     * This is a pressure drain, not D27's whole-call recovery protocol. */
    for (int i = 0; i < 16; i++) {
        if (efs_client_rpc_past_deadline())
            return -EAGAIN;
        efs_ino_t ino = efs_dcache_pressure_ino(after);
        int rc;
        if (!ino)
            break;
        after = ino;
        efs_buf_drain_enter();
        rc = efs_append_flush_report(NULL, ino);
        efs_buf_drain_leave();
        if (rc != EFS_OK)
            return rc == EFS_ERR_QUOTA ? -ENOSPC :
                   rc == EFS_ERR_NOMEM ? -ENOMEM :
                   rc == EFS_ERR_BUSY ? -EAGAIN : -EIO;
        efs_rdcache_trim();
        efs_dcache_trim_metadata();
        admission = efs_buf_reserve_request(bytes,
                                            chunks * EFS_DCACHE_ENTRY_BUDGET);
        if (admission == 0)
            return 0;
        if (admission != EFS_ERR_BUSY)
            return -ENOMEM;
    }
    /* A concurrent reservation or REPORT may release capacity without a
     * dirty inode left to drain. Wait briefly outside all cache locks. */
    for (int i = 0; i < 8; ++i) {
        uint64_t deadline = efs_client_rpc_deadline_ms();
        uint64_t now = stats_now_ms();
        if (deadline && now >= deadline)
            return -EAGAIN;
        unsigned wait_us = 100000;
        if (deadline && deadline - now < 100)
            wait_us = (unsigned)(deadline - now) * 1000;
        usleep(wait_us);
        efs_rdcache_trim();
        efs_dcache_trim_metadata();
        admission = efs_buf_reserve_request(bytes,
                                            chunks * EFS_DCACHE_ENTRY_BUDGET);
        if (!admission)
            return 0;
        if (admission != EFS_ERR_BUSY)
            return -ENOMEM;
    }
    return -EAGAIN;
}

static int efs_fuse_write_admitted(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    efs_ino_t ino = efs_file_ino(fi, 0);
    if (!ino)
        return -ENOENT;
    if (virt_kind(ino) == 2)
        return -EISDIR;
    if (virt_kind(ino))
        return -EACCES;

    int rc = 0;
    if (size == 0)
        return 0;
    if (fuse_odirect_unaligned(fi, offset, size))
        return -EINVAL;
    int append = fi && (fi->flags & O_APPEND);
    if (append) {
        int lock_rc = wb_lock_budget(append_mu(ino));
        if (lock_rc != EFS_OK)
            return fuse_stat_errno(lock_rc);
        rc = append_reserve_offset(ino, (uint64_t)size, &offset);
        if (rc != 0) {
            pthread_mutex_unlock(append_mu(ino));
            return rc;
        }
    }
    if ((append ? efs_dcache_try_patch_sparse
                : efs_dcache_try_patch)(ino, (uint64_t)offset, (uint32_t)size,
                                        (const uint8_t *)buf) == 0) {
        efs_dcache_maybe_reclaim();
        if (append) {
            /* Keep the range in dcache. Adjacent O_APPEND patches coalesce
             * (one range). A per-write GET+PUT raced the peer and the last
             * full-chunk PUT dropped their records. Close flushes+reports.
             * dcache now pins the ino, so the reservation pin can drop. */
            efs_client_stage_unpin(ino);
            pthread_mutex_unlock(append_mu(ino));
        }
        return (int)size;
    }
    size_t copy_cap = 0;
    char *copy = bounce_alloc(size, &copy_cap);
    if (!copy) {
        if (append) {
            efs_client_stage_unpin(ino);
            pthread_mutex_unlock(append_mu(ino));
        }
        return -ENOMEM;
    }
    memcpy(copy, buf, size);
    rc = efs_wb_enqueue_owned(ino, (uint64_t)offset, size, copy, NULL,
                              copy_cap);
    if (append) {
        if (rc == 0)
            (void)efs_wb_sync_ino(ino);
        /* Success marked the ino dirty inside the PUT. A failed PUT
         * leaves the open fd pin holding the reflected size. */
        efs_client_stage_unpin(ino);
        pthread_mutex_unlock(append_mu(ino));
    }
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("write", rc, ino, (uint64_t)offset, size, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("write", rc, ino, (uint64_t)offset, size, path);
        return fuse_stat_errno(rc);
    }
    return (int)size;
}

static int efs_fuse_write_run(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    int extent = fuse_validate_write_extent(offset, size);
    if (extent)
        return extent;
    if (!fi || !fi->fh || virt_kind(efs_file_ino(fi, 0)) || !size ||
        fuse_odirect_unaligned(fi, offset, size))
        return efs_fuse_write_admitted(path, buf, size, offset, fi);
    if (efs_wb_inode_stalled(efs_file_ino(fi, 0)))
        return -EIO;
    uint64_t cs = fuse_chunk_size();
    int admission = efs_client_report_admit(2 * (size / cs + 2) + 1);
    if (admission != EFS_OK)
        return fuse_stat_errno(admission);
    int rc = fuse_write_admit(size);
    if (rc) {
        efs_client_report_unreserve();
        return rc;
    }
    rc = efs_fuse_write_admitted(path, buf, size, offset, fi);
    efs_buf_unreserve();
    efs_client_report_unreserve();
    return rc;
}

static int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_client_rpc_past_deadline() ? -EAGAIN : efs_fuse_write_run(path, buf, size, offset, fi);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

/* write_buf: copy the FUSE receive buffer. Small writes still try an
 * in-place dcache patch; large writes go to the writeback pool. */
static int efs_fuse_write_buf_admitted(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    efs_ino_t ino = efs_file_ino(fi, 0);
    if (!ino)
        return -ENOENT;
    if (virt_kind(ino) == 2)
        return -EISDIR;
    if (virt_kind(ino))
        return -EACCES;

    int rc = 0;
    size_t size = fuse_buf_size(buf);
    if (size == 0)
        return 0;
    if (fuse_odirect_unaligned(fi, offset, size))
        return -EINVAL;

    int append = fi && (fi->flags & O_APPEND);
    if (append) {
        int lock_rc = wb_lock_budget(append_mu(ino));
        if (lock_rc != EFS_OK)
            return fuse_stat_errno(lock_rc);
        rc = append_reserve_offset(ino, (uint64_t)size, &offset);
        if (rc != 0) {
            pthread_mutex_unlock(append_mu(ino));
            return rc;
        }
    }

    /* W15.3: walk the write in chunk-sized pieces. Each whole chunk is
     * one fuse_buf_copy into a pool buffer the dcache owns (libfuse
     * 3.10.2 reclaims the request buffer at reply, so the slot cannot
     * keep that pointer). fuse_bufvec advances buf->idx/off, so the
     * next piece continues in the request. A partial head or tail is
     * a bounce of its own length. A failed whole-chunk store falls
     * back to the bounce path for the rest of the request; the caller
     * still sees the full size or an error, never a short count. */
    {
        uint32_t cs = fuse_chunk_size();
        uint64_t pos = (uint64_t)offset;
        size_t left = size;
        int failed = 0;

        if (!cs) {
            if (append)
                pthread_mutex_unlock(append_mu(ino));
            return -EIO;
        }
        while (left && !failed) {
            uint32_t into = (uint32_t)(pos % cs);
            uint32_t room = cs - into;
            uint32_t n = left < room ? (uint32_t)left : room;

            if (into == 0 && n == cs) {
                uint8_t *chunk = efs_buf_alloc(cs);
                struct fuse_bufvec dst = FUSE_BUFVEC_INIT(n);

                if (!chunk) {
                    failed = 1;
                    rc = -ENOMEM;
                    break;
                }
                dst.buf[0].mem = chunk;
                if (fuse_buf_copy(&dst, buf, FUSE_BUF_NO_SPLICE) < 0) {
                    efs_buf_free(chunk, cs);
                    failed = 1;
                    rc = -EIO;
                    break;
                }
                if (efs_dcache_store_full_owned(
                        ino, (uint32_t)(pos / cs), chunk, cs) == 0) {
                    pos += n;
                    left -= n;
                    continue;
                }
                /* Store failed; the buffer is still ours. Patch it,
                 * then bounce whatever the request still holds. */
                if ((append ? efs_dcache_try_patch_sparse
                            : efs_dcache_try_patch)(
                        ino, pos, cs, chunk) != 0) {
                    efs_buf_free(chunk, cs);
                    failed = 1;
                    rc = -EIO;
                    break;
                }
                efs_buf_free(chunk, cs);
                pos += n;
                left -= n;
                if (left) {
                    size_t cap = 0;
                    char *copy = bounce_alloc(left, &cap);
                    struct fuse_bufvec rest = FUSE_BUFVEC_INIT(left);

                    if (!copy) {
                        failed = 1;
                        rc = -ENOMEM;
                        break;
                    }
                    rest.buf[0].mem = copy;
                    if (fuse_buf_copy(&rest, buf, FUSE_BUF_NO_SPLICE) < 0) {
                        bounce_release(copy, cap);
                        failed = 1;
                        rc = -EIO;
                        break;
                    }
                    if ((append ? efs_dcache_try_patch_sparse
                                : efs_dcache_try_patch)(
                            ino, pos, (uint32_t)left,
                            (const uint8_t *)copy) == 0) {
                        bounce_release(copy, cap);
                    } else {
                        rc = efs_wb_enqueue_owned(ino, pos, left, copy, NULL,
                                                  cap);
                        if (rc != 0) {
                            failed = 1;
                            if (rc == EFS_ERR_QUOTA)
                                rc = -ENOSPC;
                            else
                                rc = fuse_stat_errno(rc);
                        }
                    }
                    left = 0;
                }
                continue;
            }

            {
                size_t cap = 0;
                char *copy = bounce_alloc(n, &cap);
                struct fuse_bufvec dst = FUSE_BUFVEC_INIT(n);

                if (!copy) {
                    failed = 1;
                    rc = -ENOMEM;
                    break;
                }
                dst.buf[0].mem = copy;
                if (fuse_buf_copy(&dst, buf, FUSE_BUF_NO_SPLICE) < 0) {
                    bounce_release(copy, cap);
                    failed = 1;
                    rc = -EIO;
                    break;
                }
                if ((append ? efs_dcache_try_patch_sparse
                            : efs_dcache_try_patch)(
                        ino, pos, n, (const uint8_t *)copy) == 0) {
                    bounce_release(copy, cap);
                } else {
                    rc = efs_wb_enqueue_owned(ino, pos, n, copy, NULL, cap);
                    if (rc != 0) {
                        failed = 1;
                        if (rc == EFS_ERR_QUOTA)
                            rc = -ENOSPC;
                        else
                            rc = fuse_stat_errno(rc);
                        break;
                    }
                }
                pos += n;
                left -= n;
            }
        }
        if (failed) {
            if (append) {
                efs_client_stage_unpin(ino);
                pthread_mutex_unlock(append_mu(ino));
            }
            /* rc here is a negative POSIX errno, not an EFS verdict. */
            fprintf(stderr, "efs-fuse write_buf: %s (errno=%d) "
                    "ino=%llu off=%llu len=%zu\n", strerror(-rc), -rc,
                    (unsigned long long)ino, (unsigned long long)offset, size);
            return rc;
        }
        efs_dcache_maybe_reclaim();
        if (append) {
            efs_client_stage_unpin(ino);
            pthread_mutex_unlock(append_mu(ino));
        }
        return (int)size;
    }
}

static int efs_fuse_write_buf_run(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi)
{
    int extent = fuse_validate_write_extent(offset, fuse_buf_size(buf));
    if (extent)
        return extent;
    size_t size = fuse_buf_size(buf);
    if (!fi || !fi->fh || virt_kind(efs_file_ino(fi, 0)) || !size ||
        fuse_odirect_unaligned(fi, offset, size))
        return efs_fuse_write_buf_admitted(path, buf, offset, fi);
    if (efs_wb_inode_stalled(efs_file_ino(fi, 0)))
        return -EIO;
    uint64_t cs = fuse_chunk_size();
    int admission = efs_client_report_admit(2 * (size / cs + 2) + 1);
    if (admission != EFS_OK)
        return fuse_stat_errno(admission);
    int rc = fuse_write_admit(size);
    if (rc) {
        efs_client_report_unreserve();
        return rc;
    }
    rc = efs_fuse_write_buf_admitted(path, buf, offset, fi);
    efs_buf_unreserve();
    efs_client_report_unreserve();
    return rc;
}

static int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_client_rpc_past_deadline() ? -EAGAIN : efs_fuse_write_buf_run(path, buf, offset, fi);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

static int efs_fuse_fsync_ino(fuse_ino_t ino, int isdatasync,
                              struct fuse_file_info *fi)
{
    uint64_t t0, t1, t2, flush_ms, report_ms;
    int rc;

    (void)isdatasync;
    t0 = stats_now_ms();
    efs_ino_t file = efs_file_ino(fi, (efs_ino_t)ino);
    int was_stalled = efs_wb_inode_stalled(file);
    rc = efs_append_flush_report(fi, file);
    rc = efs_file_sync_error(fi, file, was_stalled, rc);
    t1 = stats_now_ms();
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("fsync", rc, ino, 0, 0, NULL);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("fsync", rc, ino, 0, 0, NULL);
        return -EIO;
    }
    rc = EFS_OK;
    t2 = stats_now_ms();
    flush_ms = t1 - t0;
    report_ms = t2 - t1;
    if (flush_ms + report_ms >= 100) {
        fprintf(stderr,
                "fsync-split ino=%llu flush_ms=%llu report_ms=%llu rc=%d\n",
                (unsigned long long)ino, (unsigned long long)flush_ms,
                (unsigned long long)report_ms, rc);
    }
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("fsync-meta", rc, ino, 0, 0, NULL);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("fsync-meta", rc, ino, 0, 0, NULL);
        return -EIO;
    }
    return 0;
}

static int efs_fuse_flush_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    /* W2: last close is durable+visible. Kick-only left size/chunk-map
     * unpublished so the next O_APPEND used i_size 0 and wiped the
     * prefix (fcntl_setfl_oappend, hardlink_shared_data).
     * The inode's append stripe covers the report too; see
     * efs_append_flush_report.
     * fi->flags is not O_APPEND on this path. */
    efs_ino_t file = efs_file_ino(fi, (efs_ino_t)ino);
    int was_stalled = efs_wb_inode_stalled(file);
    int rc = efs_append_flush_report(fi, file);
    rc = efs_file_sync_error(fi, file, was_stalled, rc);
    if (rc != EFS_OK) {
        efs_fuse_log_err("flush-meta", rc, ino, 0, 0, NULL);
        return rc == EFS_ERR_QUOTA ? -ENOSPC : -EIO;
    }
    /* close() flushes before the syscall returns. Drop a lock this fd
     * took here; release can run after the next open already locked. */
    {
        efs_ino_t fh = efs_file_ino(fi, (efs_ino_t)ino);
        uint64_t locked_owner = 0;
        int hit = flock_armed_take(fh, &locked_owner);

        if (!hit && ino && (efs_ino_t)ino != fh)
            hit = flock_armed_take((efs_ino_t)ino, &locked_owner);
        if (hit) {
            (void)efs_client_rpc_flock(g_client.export_id, fh, EFS_FLOCK_UN,
                                       locked_owner);
        }
    }
    return 0;
}

/* A busy directory RPC is EBUSY. Mapping it to EIO made a leader-freeze
 * rmdir look like metadata corruption (Sep 23, fcstor007). */
static int fuse_unlink_errno(int urc)
{
    if (urc == 0 || urc == EFS_OK)
        return 0;
    if (urc == EFS_ERR_NOT_FOUND)
        return -ENOENT;
    if (urc == EFS_ERR_NOT_EMPTY)
        return -ENOTEMPTY;
    if (urc == EFS_ERR_BUSY || urc == EFS_ERR_AGAIN)
        return -EBUSY;
    if (urc == EFS_ERR_INVAL)
        return -EINVAL;
    return -EIO;
}

static int fuse_create_errno(int create_rc, efs_ino_t parent, const char *name)
{
    if (create_rc == EFS_ERR_QUOTA)
        return -ENOSPC;
    if (create_rc != EFS_ERR_EXIST)
        return fuse_stat_errno(create_rc == EFS_OK ? EFS_ERR_IO : create_rc);
    /* An EXIST reply is EEXIST only when the existing name is visible.
     * Preserve lookup failures, including BUSY, rather than flattening to EIO. */
    int found = efs_client_rpc_lookup(g_client.export_id, parent, name, NULL);
    if (found == EFS_OK)
        return -EEXIST;
    return fuse_stat_errno(found == EFS_ERR_NOT_FOUND ? EFS_ERR_IO : found);
}

static int efs_fuse_create_at(fuse_ino_t parent_ino, const char *name,
                              mode_t mode, struct fuse_file_info *fi,
                              efs_ino_t *out_ino)
{
    struct efs_inode parent;
    int rc;
    if (strlen(name) > 255)
        return -ENAMETOOLONG;
    if (name_is_reserved(name))
        return -EEXIST;
    rc = efs_client_stat_ino((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;

    const struct fuse_ctx *ctx = ll_ctx();
    uid_t uid = ctx ? ctx->uid : 0;
    gid_t gid = ctx ? ctx->gid : 0;
    efs_ino_t ino = 0;
    int create_rc = efs_client_create_result(parent.ino, name, S_IFREG | mode,
                                              uid, gid, 0, &ino);
    if (create_rc != EFS_OK) {
        int e = fuse_create_errno(create_rc, parent.ino, name);
        if (e == -EEXIST && fi && !(fi->flags & O_EXCL)) {
            struct efs_inode exist;
            int found = 0;
            for (int t = 0; t < 8 && !found; t++) {
                if (efs_client_rpc_lookup(g_client.export_id, parent.ino,
                                          name, &exist) == EFS_OK)
                    found = 1;
                else if (t < 7)
                    usleep(1000u << t);
            }
            if (found) {
                ino = exist.ino;
                if (fi->flags & O_TRUNC) {
                    int trc = efs_client_truncate(ino, 0);
                    if (trc != EFS_OK)
                        return fuse_stat_errno(trc);
                }
            } else {
                if (e != -EEXIST)
                    fprintf(stderr, "create %s failed (%s efs_rc=%d)\n", name,
                            e == -EIO ? "EIO" : "err", create_rc);
                return e;
            }
        } else {
            if (e != -EEXIST)
                fprintf(stderr, "create %s failed (%s efs_rc=%d)\n", name,
                        e == -EIO ? "EIO" : "err", create_rc);
            return e;
        }
    }
    if (fi) {
        int frc = efs_file_open(fi, ino);
        if (frc)
            return frc;
        fuse_fi_direct_io(fi);
        int hrc = efs_open_acquire(ino);
        if (hrc != EFS_OK) {
            efs_file_close(fi);
            return fuse_stat_errno(hrc);
        }
    }
    if (out_ino)
        *out_ino = ino;
    return 0;
}

static int efs_fuse_mkdir_at(fuse_ino_t parent_ino, const char *name, mode_t mode,
                             efs_ino_t *out_ino)
{
    struct efs_inode parent;
    int rc;
    if (strlen(name) > 255)
        return -ENAMETOOLONG;
    if (name_is_reserved(name))
        return -EEXIST;
    rc = efs_client_stat_ino((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    const struct fuse_ctx *ctx = ll_ctx();
    uid_t uid = ctx ? ctx->uid : 0;
    gid_t gid = ctx ? ctx->gid : 0;
    efs_ino_t ino = 0;
    int mk = efs_client_create_result(parent.ino, name, S_IFDIR | mode,
                                      uid, gid, 0, &ino);
    if (mk != EFS_OK) {
        /* Kernel LOOKUP was negative, then MKDIR. The server can answer
         * EEXIST for a dentry this client's lookup has not seen yet
         * (posix makedirs FileExistsError on a unique testdir name:
         * names_near_path_max, content_random_overwrite_append). A real
         * existing dir is caught by the kernel lookup before MKDIR, so
         * finding it here means the create landed — reply success.
         * A few short lookups, not the 16-attempt RPC budget. */
        struct efs_inode row;
        int tries;

        /* BUSY after the RPC budget may be a committed mkdir. Keep the
         * original request error throughout the visibility check. */
        if (mk == EFS_ERR_EXIST || mk == EFS_ERR_BUSY) {
            for (tries = 0; tries < 8; tries++) {
                int lrc = efs_client_rpc_lookup(g_client.export_id,
                                                parent.ino, name, &row);
                if (lrc == EFS_OK) {
                    if (!efs_mode_is_dir(row.mode))
                        return -EEXIST;
                    if (out_ino)
                        *out_ino = row.ino;
                    return 0;
                }
                if (lrc != EFS_ERR_NOT_FOUND)
                    return fuse_stat_errno(lrc);
                if (tries + 1 < 8)
                    usleep(20000u * (unsigned)(tries + 1));
            }
        }
        return fuse_create_errno(mk, parent.ino, name);
    }
    if (out_ino)
        *out_ino = ino;
    return 0;
}

static int efs_fuse_unlink_at(fuse_ino_t parent_ino, const char *name)
{
    struct efs_inode parent;
    int rc;
    if (name_is_reserved(name))
        return -EACCES;
    rc = efs_client_stat_ino((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    return fuse_unlink_errno(efs_client_unlink(parent.ino, name, false));
}

static int efs_fuse_rmdir_at(fuse_ino_t parent_ino, const char *name)
{
    struct efs_inode parent;
    int rc = efs_client_stat_ino((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;

    int urc = EFS_ERR_NOT_EMPTY;
    for (int attempt = 0; attempt < 20 && urc == EFS_ERR_NOT_EMPTY;
         attempt++) {
        urc = efs_client_unlink(parent.ino, name, true);
        if (urc == EFS_ERR_NOT_EMPTY) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    return fuse_unlink_errno(urc);
}

/* Refresh node used/quota from STATUS at most every 5 s (df callers). The
 * discovery-time values otherwise never move, and a df that never moves
 * looks like a quota bug. Best-effort: failures keep the last values. */
static void statfs_refresh_usage(void)
{
    static uint64_t last_ms;
    static pthread_mutex_t ref_mu = PTHREAD_MUTEX_INITIALIZER;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    pthread_mutex_lock(&ref_mu);
    if (now_ms - last_ms < 5000) {
        pthread_mutex_unlock(&ref_mu);
        return;
    }
    last_ms = now_ms;
    pthread_mutex_unlock(&ref_mu);

    uint32_t n = g_client.node_count;
    if (n > EFS_MAX_NODES)
        n = EFS_MAX_NODES;
    for (uint32_t i = 0; i < n; i++) {
        efs_node_id_t nid;
        pthread_mutex_lock(&g_client.lock);
        nid = g_client.nodes[i].id;
        pthread_mutex_unlock(&g_client.lock);
        struct efs_conn *conn = efs_client_conn_get(nid);
        if (!conn)
            continue;
        uint8_t type;
        void *payload = NULL;
        uint32_t plen = 0;
        if (efs_conn_send_msg(conn, EFS_MSG_STATUS, NULL, 0) == 0 &&
            efs_conn_recv_msg(conn, &type, &payload, &plen) == 0 &&
            type == EFS_MSG_STATUS_REPLY &&
            plen >= sizeof(struct efs_msg_status_reply)) {
            struct efs_msg_status_reply r;
            memcpy(&r, payload, sizeof(r));
            pthread_mutex_lock(&g_client.lock);
            for (uint32_t j = 0; j < g_client.node_count; j++)
                if (g_client.nodes[j].id == nid) {
                    g_client.nodes[j].used = r.used;
                    if (r.quota)
                        g_client.nodes[j].quota = r.quota;
                    break;
                }
            pthread_mutex_unlock(&g_client.lock);
            efs_client_conn_release(nid, conn);
        } else {
            efs_client_conn_drop(nid, conn);
        }
        free(payload);
    }
}

static int efs_fuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));

    statfs_refresh_usage();
    pthread_mutex_lock(&g_client.lock);

    /* Report physical truth: sum server-reported per-node disk usage and
     * divide out the 2+1 amplification. node.used is refreshed at
     * discovery; slightly stale is still truthful. There is deliberately
     * NO fallback that sums the staging table's size fields: post
     * client-cache Part A the table is a bounded cache, so a scan is both
     * O(cache) on a statfs hot path and wrong (evicted rows are invisible).
     * No servers reported yet => used reads 0, which is truthful. */
    /* W42: capacity and free space come from ONE model,
     * efs_capacity_logical (3-of-N fragment placement): total = what the
     * quotas can hold, avail = what the remaining per-node room can
     * hold, used = total − avail. The old `2 × min quota` was exact only
     * for three nodes; on four 200 GiB nodes it reported 400 GiB where
     * 533 fit. A node with no quota contributes nothing to total (its
     * capacity is unknown), so total is 0 until every node has one —
     * same rule as `efs-mgmt status`. */
    uint64_t quota[EFS_MAX_NODES], room[EFS_MAX_NODES];
    uint32_t nn = g_client.node_count, all_quota = 1;
    if (nn > EFS_MAX_NODES)
        nn = EFS_MAX_NODES;
    for (uint32_t i = 0; i < nn; i++) {
        uint64_t q = g_client.nodes[i].quota, u = g_client.nodes[i].used;
        quota[i] = q;
        room[i] = q > u ? q - u : 0;
        if (q == 0)
            all_quota = 0;
    }
    uint64_t total_logical = all_quota ? efs_capacity_logical(quota, nn) : 0;
    uint64_t avail = all_quota ? efs_capacity_logical(room, nn) : 0;
    if (avail > total_logical)
        avail = total_logical;

    stbuf->f_bsize = 512;
    stbuf->f_frsize = 512;
    stbuf->f_blocks = total_logical / 512;
    stbuf->f_bfree = avail / 512;
    stbuf->f_bavail = avail / 512;
    /* Inode counts: the local table is a bounded cache and QUERY_STATS
     * carries no cluster file count, so report the architecture's
     * 2^32-object design target rather than a cache-derived number that
     * would only mislead. */
    stbuf->f_files = 1ULL << 32;
    stbuf->f_ffree = 1ULL << 32;
    stbuf->f_favail = 1ULL << 32;
    stbuf->f_namemax = 255; /* NAME_MAX; EFS_MAX_NAME is 256 with NUL */

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
    case EFS_ERR_ACCES:     return -EACCES;
    case EFS_ERR_NAMETOOLONG: return -ENAMETOOLONG;
    case EFS_ERR_NODATA:    return -ENODATA;
    case EFS_ERR_BUSY:
        return -EBUSY;
    case EFS_ERR_AGAIN:
        return -EAGAIN;
    case EFS_ERR_NO_QUORUM:
    case EFS_ERR_NET:
    default:                return -EIO;
    }
}

static int efs_fuse_chmod_ino(fuse_ino_t ino, mode_t mode)
{
    int rc;
    if (virt_kind(ino))
        return -EACCES;
    rc = efs_client_chmod((efs_ino_t)ino, mode);
    if (rc != 0) {
        fprintf(stderr, "chmod ino=%llu failed: %s\n",
                (unsigned long long)ino, efs_strerror(rc));
        fflush(stderr);
        return efs_rc_to_errno(rc);
    }
    return 0;
}

static int efs_fuse_chown_ino(fuse_ino_t ino, uid_t uid, gid_t gid)
{
    struct efs_inode row;
    int rc;
    if (virt_kind(ino))
        return -EACCES;
    if (efs_client_stat_ino((efs_ino_t)ino, &row) != EFS_OK)
        return -ENOENT;
    rc = check_chown_perm(&row, uid, gid);
    if (rc != 0)
        return rc;
    rc = efs_client_chown(row.ino, uid, gid);
    return efs_rc_to_errno(rc);
}

static int efs_fuse_symlink_at(const char *link, fuse_ino_t parent_ino,
                               const char *name, efs_ino_t *out_ino)
{
    struct efs_inode parent;
    int rc = efs_client_stat_ino((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    const struct fuse_ctx *ctx = ll_ctx();
    uid_t uid = ctx ? ctx->uid : 0;
    gid_t gid = ctx ? ctx->gid : 0;
    efs_ino_t ino = 0;
    int create_rc = efs_client_create_result(parent.ino, name, S_IFLNK | 0777,
                                              uid, gid, 0, &ino);
    if (create_rc != EFS_OK)
        return fuse_create_errno(create_rc, parent.ino, name);
    size_t len = strlen(link);
    if (len > 0) {
        rc = efs_client_write(ino, 0, len, link);
        if (rc != 0)
            return -EIO;
    } else if (efs_client_truncate(ino, 0) != 0)
        return -EIO;
    if (efs_client_report_dirty_ino(ino, 1) != EFS_OK)
        return -EIO;
    if (out_ino)
        *out_ino = ino;
    return 0;
}

static int efs_fuse_readlink_ino(fuse_ino_t ino, char *buf, size_t size)
{
    struct efs_inode row;
    int rc = efs_client_stat_ino((efs_ino_t)ino, &row);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_lnk(row.mode))
        return -EINVAL;
    if (size == 0)
        return -EINVAL;
    size_t got = 0;
    size_t want = size - 1;
    if (want > row.size)
        want = (size_t)row.size;
    if (want > 0) {
        rc = efs_client_read(row.ino, 0, want, buf, &got);
        if (rc != 0)
            return -EIO;
    }
    buf[got] = '\0';
    return 0;
}

static int efs_fuse_link_at(fuse_ino_t src_ino, fuse_ino_t newparent,
                            const char *newname)
{
    struct efs_inode src, parent;
    int rc;
    if (virt_kind(src_ino) || virt_kind(newparent))
        return -EACCES;
    if (name_is_reserved(newname))
        return -EACCES;
    if (efs_client_stat_ino((efs_ino_t)src_ino, &src) != EFS_OK)
        return -ENOENT;
    if (efs_mode_is_dir(src.mode))
        return -EPERM;
    rc = efs_client_stat_ino((efs_ino_t)newparent, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    rc = efs_client_link(src.ino, parent.ino, newname);
    if (rc == EFS_ERR_EXIST)
        return -EEXIST;
    if (rc != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_utimens_ino(fuse_ino_t ino, const struct timespec tv[2])
{
    if (virt_kind(ino))
        return -EACCES;

    /* tv[0]=atime, tv[1]=mtime; honor UTIME_OMIT / UTIME_NOW. */
    int set_a = tv && tv[0].tv_nsec != UTIME_OMIT;
    int set_m = tv && tv[1].tv_nsec != UTIME_OMIT;
    if (!set_a && !set_m)
        return 0;

    struct timespec now;
    int have_now = 0;
    uint64_t asec = 0, msec = 0;
    uint32_t nsec = 0, ansec = 0;
    if (set_a) {
        if (tv[0].tv_nsec == UTIME_NOW) {
            clock_gettime(CLOCK_REALTIME, &now);
            have_now = 1;
            asec = (uint64_t)now.tv_sec;
            ansec = (uint32_t)now.tv_nsec;
        } else {
            asec = (uint64_t)tv[0].tv_sec;
            ansec = (uint32_t)tv[0].tv_nsec;
            if (ansec >= 1000000000u)
                ansec = 0;
        }
    }
    if (set_m) {
        if (tv[1].tv_nsec == UTIME_NOW) {
            if (!have_now)
                clock_gettime(CLOCK_REALTIME, &now);
            msec = (uint64_t)now.tv_sec;
            nsec = (uint32_t)now.tv_nsec;
        } else {
            msec = (uint64_t)tv[1].tv_sec;
            nsec = (uint32_t)tv[1].tv_nsec;
            if (nsec >= 1000000000u)
                nsec = 0;
        }
    }
    /* Map the real status. Collapsing every rc to EIO turned a missing
     * inode row into "Input/output error" on rsync's set-times, which is
     * both wrong and undiagnosable — chmod hits the same SETATTR handler
     * and reported the ENOENT correctly. */
    int rc;
    /* Order the buffered data ahead of the time it must not disturb.
     * write() is client-buffered; its PUBLISH reaches the server in the
     * close REPORT with the publish time and the row's CURRENT mtime_gen,
     * so a utimens issued between the last write and close was overruled
     * by the stat MAX (ecopy's write → futimens → close → rename gave
     * every file its close time, results/measure/20260930-044100-*).
     * Flushing here lands the publish under the old generation; the
     * SETATTR then bumps it and fences the lanes. A clean inode skips
     * this; ecopy's close REPORT afterwards is empty and is skipped. */
    if (set_m) {
        rc = efs_utimens_flush_dirty((efs_ino_t)ino);
        if (efs_dcache_trace_on())
            fprintf(stderr, "utimens ino=%llu flush rc=%d\n",
                    (unsigned long long)ino, rc);
        if (rc != EFS_OK)
            return efs_rc_to_errno(rc);
    }
    if (set_a && set_m) {
        rc = efs_client_utimens_both(ino, msec, nsec, asec, ansec);
        if (efs_dcache_trace_on())
            fprintf(stderr, "utimens ino=%llu setattr rc=%d\n",
                    (unsigned long long)ino, rc);
        return efs_rc_to_errno(rc);
    }
    if (set_a) {
        rc = efs_client_set_atime(ino, asec, ansec);
        if (rc != EFS_OK)
            return efs_rc_to_errno(rc);
    }
    if (set_m) {
        rc = efs_client_utimens(ino, msec, nsec);
        if (rc != EFS_OK)
            return efs_rc_to_errno(rc);
    }
    return 0;
}

static int efs_fuse_truncate_run(fuse_ino_t ino, off_t size,
                                 struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    if (size < 0)
        return -EINVAL;
    if (vk == 2)
        return -EISDIR;
    if (vk)
        return -EACCES;
    if (efs_client_rpc_past_deadline())
        return -EAGAIN;
    if (!fi || !fi->fh) {
        struct efs_inode row;
        int rc = efs_client_stat_ino((efs_ino_t)ino, &row);
        if (rc != EFS_OK)
            return efs_rc_to_errno(rc);
        if (efs_mode_is_dir(row.mode))
            return -EISDIR;
        const struct fuse_ctx *ctx = ll_ctx();
        if (ctx && check_access(&row, ctx->uid, ctx->gid, W_OK) != 0)
            return -EACCES;
    }
    int rc = efs_dcache_flush_ino((efs_ino_t)ino);
    if (rc == EFS_OK)
        rc = efs_client_truncate((efs_ino_t)ino, (uint64_t)size);
    return efs_rc_to_errno(rc);
}

static int efs_fuse_truncate_ino(fuse_ino_t ino, off_t size,
                                 struct fuse_file_info *fi)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 8000;
    if (previous && previous < deadline)
        deadline = previous;
    /* Include access discovery and flushing without extending a caller budget. */
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_fuse_truncate_run(ino, size, fi);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

static int efs_fuse_release_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    efs_ino_t fh = efs_file_ino(fi, (efs_ino_t)ino);
    int leased = 0;
    efs_client_note_meta_change(0);
    efs_file_close(fi);
    if (!fh)
        return 0;
    pthread_mutex_t *edge = open_edge_mu(fh);
    pthread_mutex_lock(edge);
    if (!efs_close_note(fh, &leased)) {
        pthread_mutex_unlock(edge);
        return 0;
    }
    {
        uint64_t lk = fi ? fi->lock_owner : 0;
        uint64_t owner = g_client.flock_token ^
                         (lk ? lk : ((uint64_t)(uintptr_t)fi << 8));

        /* The kernel does not send LOCK_UN on close. Only an inode
         * this client locked pays the Raft unlock, with the owner that
         * took the lock (release's fi->lock_owner is not that one). */
        {
            uint64_t locked_owner = owner;
            int hit = flock_armed_take(fh, &locked_owner);
            if (!hit && ino && (efs_ino_t)ino != fh)
                hit = flock_armed_take((efs_ino_t)ino, &locked_owner);
            if (hit) {
                (void)efs_client_rpc_flock(g_client.export_id, fh,
                                           EFS_FLOCK_UN, locked_owner);
            }
        }
        if (leased)
            (void)efs_client_rpc_hold(g_client.export_id, fh, 0,
                                      g_client.flock_token);
        efs_client_stage_evict_ino(fh);
    }
    pthread_mutex_unlock(edge);
    return 0;
}

/* Runs before unmount with mutations quiesced. Timeout of the control
 * request does not cancel or free an accepted background job. */
static int efs_control_drain(uint64_t deadline, int force)
{
    if (force) {
        (void)efs_dcache_pending_records("explicit-stop-force-discard");
        struct efs_report_pressure pressure;
        efs_client_report_pressure_stats(&pressure);
        fprintf(stderr, "force-discard REPORT pending=%llu inflight=%llu cause=explicit-stop-force-discard\n",
                (unsigned long long)pressure.pending_records,
                (unsigned long long)pressure.inflight_records);
        fflush(stderr);
        return 0;
    }
    uint64_t previous = efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(deadline);
    int result = -1;
    while (stats_now_ms() < deadline) {
        int pending;
        if (g_wb.ready) {
            pthread_mutex_lock(&g_wb.mu);
            pending = g_wb.count || g_wb.inflight;
            pthread_mutex_unlock(&g_wb.mu);
        } else
            pending = 0;
        if (pending) {
            usleep(10000);
            continue;
        }
        efs_buf_drain_enter();
        int data_rc = efs_dcache_flush_all();
        int report_rc = efs_client_report_dirty(1);
        efs_buf_drain_leave();
        struct efs_report_pressure pressure;
        efs_client_report_pressure_stats(&pressure);
        if (data_rc == EFS_OK && report_rc == EFS_OK &&
            !pressure.pending_records && !pressure.reserved_records &&
            !efs_dcache_pending_records(NULL)) {
            result = 0;
            break;
        }
        usleep(10000);
    }
    if (result)
        fprintf(stderr, "stop-refused: unresolved writes remain; mount retained\n");
    efs_client_rpc_set_deadline_ms(previous);
    return result;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
    int wrc = efs_wb_sync();
    int drc = efs_dcache_flush_all();
    if (wrc != EFS_OK || drc != EFS_OK)
        fprintf(stderr, "efs-fuse: unmount: data flush incomplete "
                        "(writeback=%d dcache=%d)\n", wrc, drc);
    if (g_wb.ready) {
        pthread_mutex_lock(&g_wb.mu);
        g_wb.shutdown = 1;
        pthread_cond_broadcast(&g_wb.not_empty);
        pthread_mutex_unlock(&g_wb.mu);
        for (int i = 0; i < g_wb.nworkers; i++)
            pthread_join(g_wb.workers[i], NULL);
        g_wb.ready = 0;
        g_wb.shutdown = 0;
    }
    /* Drain the dirty set before efs_client_shutdown frees the table.
     *
     * One forced report is not enough. efs_client_report_dirty gives up on a
     * shard after 4 attempts (~750 ms total) while its owner answers BUSY,
     * which is exactly what an owner does while it is rebuilding a shard --
     * something every node does briefly after a fresh mkfs, and again
     * whenever a joiner catches up. On give-up the records are merged back
     * into the in-memory table, and shutdown then frees it, so the writes
     * were lost with no error and no log line: files came back after the
     * remount with the correct size and all-zero contents.
     *
     * The BUSY window is short (seconds) and self-healing, so retry until the
     * report succeeds. If it still will not drain, say so -- an unmount that
     * silently discards acknowledged writes is worse than a noisy one. */
    {
        const long drain_s = 60;
        struct timespec t0, now;
        int mrc;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            mrc = efs_client_note_meta_change(1);
            if (mrc == EFS_OK)
                break;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= drain_s)
                break;
            usleep(200000);
        }
        if (mrc != EFS_OK)
            fprintf(stderr, "efs-fuse: UNMOUNT DATA LOSS: metadata flush "
                            "still failing (rc=%d) after %lds; writes that "
                            "were never reported are gone\n", mrc, drain_s);
    }
    /* Free cached .find query results. */
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        free(g_find_res[i].text);
        g_find_res[i].text = NULL;
        g_find_res[i].valid = 0;
    }
    pthread_mutex_unlock(&g_find_res_mu);
    efs_client_shutdown();
}

static int efs_fuse_rename_at(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags, efs_ino_t *out_src)
{
    struct efs_inode src, src_parent, dst_parent;
    int rc;
    (void)flags;
    if (virt_kind(parent) || virt_kind(newparent))
        return -EACCES;
    if (name_is_reserved(name) || name_is_reserved(newname))
        return -EACCES;
    rc = efs_client_stat_ino((efs_ino_t)parent, &src_parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&src_parent);
    if (wx != 0)
        return wx;
    rc = efs_client_rpc_lookup(g_client.export_id, (efs_ino_t)parent, name, &src);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    rc = efs_client_stat_ino((efs_ino_t)newparent, &dst_parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    wx = check_dir_wx(&dst_parent);
    if (wx != 0)
        return wx;
    rc = efs_client_rename_at(src.ino, src_parent.ino, name,
                              dst_parent.ino, newname);
    if (rc != 0)
        return efs_rc_to_errno(rc);
    if (out_src)
        *out_src = src.ino;
    return 0;
}

#ifndef SEEK_DATA
#define SEEK_DATA 3
#endif
#ifndef SEEK_HOLE
#define SEEK_HOLE 4
#endif
#ifndef OFF_MAX
#define OFF_MAX ((off_t)(((unsigned long long)1 << (sizeof(off_t) * 8 - 1)) - 1))
#endif

static off_t inode_seek_data_hole(const struct efs_inode *ino, off_t off, int whence)
{
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ino->size) {
        if (whence == SEEK_HOLE)
            return (off_t)ino->size;
        return -ENXIO;
    }
    if (ino->pack_ino && ino->pack_len) {
        if (whence == SEEK_DATA)
            return (off < (off_t)ino->pack_len) ? off : -ENXIO;
        return (off < (off_t)ino->pack_len) ? (off_t)ino->pack_len
                                            : (off_t)ino->size;
    }
    uint32_t cs = fuse_chunk_size();
    if (cs == 0)
        return (whence == SEEK_HOLE) ? (off_t)ino->size : -ENXIO;
    uint32_t nci = (uint32_t)((ino->size + (uint64_t)cs - 1) / cs);
    uint32_t start_ci = (uint32_t)((uint64_t)off / cs);

    efs_client_lock_dir(ino->ino);
    pthread_mutex_lock(&g_client.idx_mu);
    if (whence == SEEK_DATA) {
        for (uint32_t ci = start_ci; ci < nci; ci++) {
            if (file_chunk_present(ino->ino, ci)) {
                off_t data = (off_t)((uint64_t)ci * cs);
                if (data < off)
                    data = off;
                pthread_mutex_unlock(&g_client.idx_mu);
                efs_client_unlock_dir(ino->ino);
                return data;
            }
        }
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino->ino);
        return -ENXIO;
    }
    for (uint32_t ci = start_ci; ci < nci; ci++) {
        if (!file_chunk_present(ino->ino, ci)) {
            off_t hole = (off_t)((uint64_t)ci * cs);
            if (hole < off)
                hole = off;
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_unlock_dir(ino->ino);
            return hole;
        }
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino->ino);
    return (off_t)ino->size;
}

static off_t efs_fuse_lseek_ino(fuse_ino_t ino, off_t off, int whence,
                                struct fuse_file_info *fi)
{
    efs_ino_t inum = efs_file_ino(fi, (efs_ino_t)ino);
    struct efs_inode row;
    int rc;
    efs_client_lock_dir(inum);
    pthread_mutex_lock(&g_client.idx_mu);
    rc = efs_export_get_inode(&g_client.export, inum, &row);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(inum);
    if (rc != 0)
        return -ENOENT;
    if (whence != SEEK_DATA && whence != SEEK_HOLE)
        return -EINVAL;
    return inode_seek_data_hole(&row, off, whence);
}

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

static int efs_fuse_getlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
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

static int efs_fuse_setlk_ino(fuse_ino_t ino, struct fuse_file_info *fi,
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
        flock_arm(inum, plock_rpc_owner(fi));
        return 0;
    }
}

static int efs_fuse_flock_ino(fuse_ino_t ino, struct fuse_file_info *fi, int op)
{
    efs_ino_t fh = efs_file_ino(fi, (efs_ino_t)ino);
    uint64_t lk = fi ? fi->lock_owner : 0;
    uint64_t owner = g_client.flock_token ^
                     (lk ? lk : ((uint64_t)(uintptr_t)fi << 8));
    int rc;
    if (getenv("EFS_FLOCK_DBG"))
        fprintf(stderr, "flock: ino=%llu op=%u owner=%llu\n",
                (unsigned long long)fh, op, (unsigned long long)owner);
    rc = efs_client_rpc_flock(g_client.export_id, fh, (uint32_t)op, owner);
    if (rc == EFS_ERR_BUSY)
        return -EAGAIN;
    if (rc != EFS_OK)
        return -EIO;
    if (!(op & LOCK_UN)) {
        flock_arm(fh, owner);
        if ((efs_ino_t)ino != fh)
            flock_arm((efs_ino_t)ino, owner);
    }
    return 0;
}

/* Parent of a daemonized efs-fuse waits on this until FUSE_INIT has run.
 * fuse_mount() puts the path in /proc/mounts before fuse_loop_mt is reading,
 * so a parent that returned at fuse_daemonize advertised a mount that could
 * not yet serve. -1 when running foreground. */
static int g_fuse_ready_wr = -1;

static void efs_fuse_note_serving(void)
{
    int fd = g_fuse_ready_wr;
    if (fd >= 0) {
        g_fuse_ready_wr = -1;
        char c = 'R';
        (void)write(fd, &c, 1);
        close(fd);
    }
    fprintf(stderr, "fuse serving\n");
    fflush(stderr);
}

static int efs_fuse_wait_ready(int rfd, pid_t child, int timeout_ms)
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

static void efs_fuse_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    efs_dcache_init();
    /* Timeouts are per lookup/getattr reply (always 0). There is no
     * fuse_config on the low-level API. */
    if (conn) {
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
        /* W15.5 asked for this (Sep 29 15:40Z) and the first profile
         * with it (fstor007 ecopy, 17:35Z) showed no gain: libfuse
         * splices /dev/fuse into a pipe and the write handler's
         * fuse_buf_copy then read()s the pipe into the dcache buffer,
         * which is the same one kernel->user copy read(/dev/fuse) does,
         * plus one splice per request (24 668 in 67 s, 17 072 of them
         * header-sized). `copyout` under `pipe_read` was 5.2% of the
         * client. Leave it off; fs.pipe-max-size no longer matters.
         * EFS_FUSE_SPLICE_READ=1 turns it back on for a measurement
         * (a 1 MiB streaming write is two copies without it). */
        {
            const char *sp = getenv("EFS_FUSE_SPLICE_READ");

            if (sp && *sp && strcmp(sp, "0") != 0)
                conn->want |= FUSE_CAP_SPLICE_READ;
            else
                conn->want &= ~FUSE_CAP_SPLICE_READ;
        }
#endif
#ifdef FUSE_CAP_EXPORT_SUPPORT
        /* NFS re-export of an efs mount (e.g. a gateway VM exporting to
         * clients that only speak NFS) needs the kernel to build file
         * handles via fs/fuse/export.c, which requires this cap. libfuse
         * keeps the nodeid->(parent,name) table, so by-handle lookups ride
         * the normal lookup/forget path. Handles do not survive an
         * efs-fuse restart. Off by default; EFS_FUSE_EXPORT=1 at mount
         * time enables it. */
        {
            const char *ex = getenv("EFS_FUSE_EXPORT");

            if (ex && *ex && strcmp(ex, "0") != 0 &&
                (conn->capable & FUSE_CAP_EXPORT_SUPPORT))
                conn->want |= FUSE_CAP_EXPORT_SUPPORT;
            else
                conn->want &= ~FUSE_CAP_EXPORT_SUPPORT;
        }
#endif
        if (conn->congestion_threshold < 96)
            conn->congestion_threshold = 96;
    }
    uint32_t batch = 4096;
    const char *env = getenv("EFS_META_BATCH_OPS");
    if (env && *env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 1000000)
            batch = (uint32_t)v;
    }
    efs_client_stage_set_pin_hooks(efs_client_ino_is_open,
                                   efs_client_ino_has_plock);
    efs_client_enable_meta_batch(batch);
    efs_fuse_note_serving();
}


static void fill_entry(fuse_ino_t ino, struct fuse_entry_param *e,
                       const struct stat *st)
{
    memset(e, 0, sizeof(*e));
    e->ino = ino;
    e->generation = 1;
    e->attr = *st;
    e->attr.st_ino = ino;
    e->attr_timeout = 0.0;
    e->entry_timeout = 0.0;
}

#if EFS_FAULTS
static void lookup_reply_barrier(const struct efs_inode *row)
{
    static int used;
    const char *directory = getenv("EFS_FAULT_LOOKUP_BARRIER");
    char armed[1024], entered[1024];
    if (!directory || !t_lookup_memo_serial || strcmp(row->name,"memo-blocked")) return;
    if (snprintf(armed,sizeof(armed),"%s/lookup-arm",directory)>=(int)sizeof(armed) ||
        snprintf(entered,sizeof(entered),"%s/lookup-entered",directory)>=(int)sizeof(entered)) return;
    if (access(armed,F_OK) || !__sync_bool_compare_and_swap(&used,0,1)) return;
    int fd=open(entered,O_CREAT|O_WRONLY|O_EXCL,0600);
    if(fd>=0)close(fd);
    uint64_t deadline=fuse_now_us()+10000000ull;
    while(!access(armed,F_OK) && fuse_now_us()<deadline)usleep(1000);
    fprintf(stderr,"lookup-reply barrier ino=%llu released\n",(unsigned long long)row->ino);
}
#endif

static void fill_entry_row(fuse_ino_t ino, struct fuse_entry_param *e,
                           const struct efs_inode *row)
{
    struct stat st;
    fill_stat_from_inode(&st, row);
    fill_entry(ino, e, &st);
#if EFS_FAULTS
    lookup_reply_barrier(row);
#endif
    lookup_memo_put(row);
}

static int lookup_fill(fuse_ino_t ino, struct fuse_entry_param *e,
                       struct fuse_file_info *fi)
{
    struct stat st;
    int rc = efs_fuse_getattr_ino(ino, &st, fi);
    if (rc != 0)
        return rc;
    fill_entry(ino, e, &st);
    if (t_getattr_ino == (efs_ino_t)ino)
        lookup_memo_put(&t_getattr_row);
    if (virt_kind(ino) == 3)
        vq_nlookup_add(ino, 1);
    return 0;
}

/* The mutation already committed and dual-applied `ino`. lookup_fill with
 * no open fh always refreshes, and stat_refresh maps every RPC failure to
 * NOT_FOUND, so the app saw ENOENT for a directory that existed
 * (same_parent_storm: mkdir ENOENT, the following rmdir succeeded).
 * Reply the local row instead. */
static int lookup_fill_committed(fuse_ino_t ino, struct fuse_entry_param *e,
                                 const char *op, fuse_ino_t parent,
                                 const char *name)
{
    struct efs_inode row;
    int rc = lookup_fill(ino, e, NULL);

    if (rc == 0)
        return 0;
    if (efs_client_stat_ino((efs_ino_t)ino, &row) != EFS_OK) {
        fprintf(stderr, "fuse: %s parent=%llu name=%s ok ino=%llu "
                "but getattr rc=%d\n", op, (unsigned long long)parent,
                name ? name : "", (unsigned long long)ino, rc);
        return rc;
    }
    fprintf(stderr, "fuse: %s parent=%llu name=%s ok ino=%llu "
            "getattr rc=%d, local reply\n", op,
            (unsigned long long)parent, name ? name : "",
            (unsigned long long)ino, rc);
    fill_entry_row(ino, e, &row);
    return 0;
}

static int efs_fuse_lookup_at(fuse_ino_t parent, const char *name,
                              struct fuse_entry_param *e)
{
    struct efs_inode prow, row;
    int rc;

    if (!name || name[0] == '\0')
        return -ENOENT;
    if (strlen(name) > 255)
        return -ENAMETOOLONG;

    if (virt_kind(parent) == 2) {
        fuse_ino_t q;
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        if (strcmp(name, ".") == 0)
            return lookup_fill(parent, e, NULL);
        if (strcmp(name, "..") == 0)
            return lookup_fill((fuse_ino_t)virt_parent(parent), e, NULL);
        q = vq_intern(virt_parent(parent), name);
        return lookup_fill(q, e, NULL);
    }
    if (virt_kind(parent))
        return -ENOENT;

    if (strcmp(name, ".") == 0)
        return lookup_fill(parent, e, NULL);
    if (strcmp(name, "..") == 0) {
        fuse_ino_t up = parent;
        if (parent != FUSE_ROOT_ID &&
            efs_client_stat_ino((efs_ino_t)parent, &prow) == EFS_OK &&
            prow.parent)
            up = (fuse_ino_t)prow.parent;
        return lookup_fill(up, e, NULL);
    }
    if (strcmp(name, EFS_STATS_NAME) == 0) {
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        return lookup_fill(virt_stats_ino((efs_ino_t)parent), e, NULL);
    }
    if (strcmp(name, EFS_FIND_NAME) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return lookup_fill(virt_find_ino((efs_ino_t)parent), e, NULL);
    }

    rc = efs_client_stat_local((efs_ino_t)parent, &prow);
    if (rc != EFS_OK)
        rc = efs_client_stat_ino((efs_ino_t)parent, &prow);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(prow.mode))
        return -ENOTDIR;
    rc = check_dir_x(&prow);
    if (rc != 0)
        return rc;
    /* A directory this client just created is in the local table.
     * Answering it here keeps a deep mkdir from re-reading every
     * ancestor over RPC (dir_deep_nesting*_64 is O(n^2) LOOKUPs with
     * entry_timeout=0). Files stay on the RPC: a peer unlink must
     * still be visible (peer_negative_after_unlink). A name we have
     * never seen is a miss and falls through. */
    if (efs_client_lookup_local((efs_ino_t)parent, name, &row) == EFS_OK &&
        efs_mode_is_dir(row.mode)) {
        fill_entry_row((fuse_ino_t)row.ino, e, &row);
        return 0;
    }
    rc = efs_client_rpc_lookup(g_client.export_id, (efs_ino_t)parent, name, &row);
    if (rc != EFS_OK)
        return (rc == EFS_ERR_ACCES) ? -EACCES : -ENOENT;
    /* Adopt the LOOKUP row (full resolved inode) instead of throwing it
     * away and re-statting locally — that hid peer size/nlink growth. */
    efs_client_adopt_lookup(&row, &row);
    fill_entry_row((fuse_ino_t)row.ino, e, &row);
    return 0;
}

static int efs_fuse_setattr_ino(fuse_ino_t ino, struct stat *attr, int to_set,
                                struct fuse_file_info *fi)
{
    int rc = 0;
    if (to_set & FUSE_SET_ATTR_MODE) {
        rc = efs_fuse_chmod_ino(ino, attr->st_mode);
        if (rc)
            return rc;
    }
    if (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
        uid_t uid = (to_set & FUSE_SET_ATTR_UID) ? attr->st_uid : (uid_t)-1;
        gid_t gid = (to_set & FUSE_SET_ATTR_GID) ? attr->st_gid : (gid_t)-1;
        rc = efs_fuse_chown_ino(ino, uid, gid);
        if (rc)
            return rc;
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
        rc = efs_fuse_truncate_ino(ino, attr->st_size, fi);
        if (rc)
            return rc;
    }
    if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME |
                  FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW)) {
        struct timespec tv[2];
        tv[0].tv_sec = 0;
        tv[1].tv_sec = 0;
        tv[0].tv_nsec = UTIME_OMIT;
        tv[1].tv_nsec = UTIME_OMIT;
        if (to_set & FUSE_SET_ATTR_ATIME_NOW)
            tv[0].tv_nsec = UTIME_NOW;
        else if (to_set & FUSE_SET_ATTR_ATIME) {
            tv[0].tv_sec = attr->st_atim.tv_sec;
            tv[0].tv_nsec = attr->st_atim.tv_nsec;
        }
        if (to_set & FUSE_SET_ATTR_MTIME_NOW)
            tv[1].tv_nsec = UTIME_NOW;
        else if (to_set & FUSE_SET_ATTR_MTIME) {
            tv[1].tv_sec = attr->st_mtim.tv_sec;
            tv[1].tv_nsec = attr->st_mtim.tv_nsec;
        }
        rc = efs_fuse_utimens_ino(ino, tv);
        if (rc)
            return rc;
    }
    return 0;
}

static int dirbuf_add(fuse_req_t req, char *buf, size_t bufsize, size_t *used,
                      const char *name, fuse_ino_t ino, mode_t mode, off_t next)
{
    struct stat st;
    size_t sz;
    memset(&st, 0, sizeof(st));
    st.st_ino = ino;
    st.st_mode = mode;
    sz = fuse_add_direntry(req, buf + *used, bufsize - *used, name, &st, next);
    if (sz > bufsize - *used)
        return 1;
    *used += sz;
    return 0;
}

static void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse_entry_param e;
    int rc;
    t_lookup_memo_serial = lookup_memo_start();
    t_req = req;
    rc = efs_fuse_lookup_at(parent, name, &e);
    t_lookup_memo_serial = 0;
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_entry(req, &e);
}

static void ll_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
    if (virt_kind(ino) == 3)
        vq_nlookup_sub(ino, nlookup);
    fuse_reply_none(req);
}

static void ll_forget_multi(fuse_req_t req, size_t count,
                            struct fuse_forget_data *forgets)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (virt_kind(forgets[i].ino) == 3)
            vq_nlookup_sub(forgets[i].ino, forgets[i].nlookup);
    }
    fuse_reply_none(req);
}

static void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct stat st;
    int rc;
    t_req = req;
    rc = efs_fuse_getattr_ino(ino, &st, fi);
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_attr(req, &st, 0.0);
}

static void ll_setattr_run(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                       int to_set, struct fuse_file_info *fi)
{
    struct stat st;
    off_t old_size = -1;
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    if (to_set & FUSE_SET_ATTR_SIZE) {
        struct efs_inode row;
        /* Size only. st_blocks is not an input to truncate. */
        if (efs_client_stat_ino((efs_ino_t)ino, &row) == EFS_OK)
            old_size = (off_t)row.size;
    }
    rc = efs_fuse_setattr_ino(ino, attr, to_set, fi);
    if (rc == 0)
        rc = efs_fuse_getattr_ino(ino, &st, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else {
        fuse_reply_attr(req, &st, 0.0);
        /* Shrink only. notify_inval_inode looks the inode up and can
         * pin pages that close would have forgotten — a grow after
         * create then races posix2 overlap. Truncate-to-zero wipes
         * all; otherwise drop pages at/after the new EOF. */
        if ((to_set & FUSE_SET_ATTR_SIZE) && old_size >= 0 &&
            attr->st_size < old_size) {
            if (attr->st_size <= 0)
                ll_inval_inode(ino, 0, 0);
            else
                ll_inval_inode(ino, attr->st_size,
                               (off_t)(((uint64_t)1 << 62) -
                                       (uint64_t)attr->st_size));
        }
    }
}

static void ll_readlink(fuse_req_t req, fuse_ino_t ino)
{
    char buf[EFS_MAX_PATH];
    int rc;
    t_req = req;
    rc = efs_fuse_readlink_ino(ino, buf, sizeof(buf));
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_readlink(req, buf);
}

static void ll_mkdir_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode)
{
    efs_ino_t new_ino = 0;
    struct fuse_entry_param e;
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_mkdir_at(parent, name, mode, &new_ino);
    if (rc == 0)
        rc = lookup_fill_committed((fuse_ino_t)new_ino, &e, "mkdir",
                                   parent, name);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_entry(req, &e);
}

static void ll_unlink_run(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_unlink_at(parent, name);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_rmdir_run(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_rmdir_at(parent, name);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_symlink_run(fuse_req_t req, const char *link, fuse_ino_t parent,
                       const char *name)
{
    efs_ino_t new_ino = 0;
    struct fuse_entry_param e;
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_symlink_at(link, parent, name, &new_ino);
    if (rc == 0)
        rc = lookup_fill_committed((fuse_ino_t)new_ino, &e, "symlink",
                                   parent, name);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_entry(req, &e);
}

static void ll_rename_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                      fuse_ino_t newparent, const char *newname,
                      unsigned int flags)
{
    int rc;
    efs_ino_t src_ino = 0;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_rename_at(parent, name, newparent, newname, flags, &src_ino);
    lookup_memo_mutation_end();
    t_req = NULL;
    (void)src_ino;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_link_run(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                    const char *newname)
{
    struct fuse_entry_param e;
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_link_at(ino, newparent, newname);
    if (rc == 0)
        rc = lookup_fill_committed(ino, &e, "link", newparent, newname);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_entry(req, &e);
}

static void ll_open_run(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int rc;
    int mutating = fi && (fi->flags & O_TRUNC);
    if (mutating)
        lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_open_ino(ino, fi);
    if (mutating)
        lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_open(req, fi);
}

/* Per-worker reply buffer. A malloc(1 MiB)/free per READ request sat
 * above glibc's mmap threshold on every call (fresh pages, kernel memset
 * and rmqueue in the read profile of a 20 GiB dd, Oct 1 2026); the
 * worker keeps one buffer sized to the largest request it has seen. */
#define LL_READ_BUF_MAX (16u << 20)

#define LL_READ_REFS_MAX 32
static void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                    struct fuse_file_info *fi)
{
    char *buf;
    int n, owned = 0;
    t_req = req;
    /* Zero-copy reply: a chunk-aligned read whose chunks are all in the
     * read cache goes to the kernel as one writev of the cached images
     * (fuse_reply_iov; fuse_reply_data would copy a multi-buffer vector
     * into a fresh allocation in libfuse 3.10). Anything else — a
     * partial chunk, a miss, a dirty overlay, a span — takes the copy
     * path below. */
    if (virt_kind(ino) == 0 && size <= LL_READ_BUF_MAX &&
        !fuse_odirect_unaligned(fi, off, size)) {
        efs_ino_t file = efs_file_ino(fi, (efs_ino_t)ino);
        struct efs_read_ref refs[LL_READ_REFS_MAX];
        int nr = efs_client_read_refs(file, (uint64_t)off, size, refs,
                                      LL_READ_REFS_MAX);
        if (nr > 0) {
            struct iovec iov[LL_READ_REFS_MAX];
            for (int i = 0; i < nr; i++) {
                iov[i].iov_base = (void *)(uintptr_t)refs[i].data;
                iov[i].iov_len = refs[i].len;
            }
            t_req = NULL;
            fuse_reply_iov(req, iov, nr);
            for (int i = 0; i < nr; i++)
                efs_rdcache_unpin(refs[i].pin);
            {
                static int logged;
                if (!logged) {
                    logged = 1;
                    fprintf(stderr, "efs: read reply zero-copy active (iov=%d size=%zu)\n", nr, size);
                }
            }
            return;
        }
    }
    buf = size <= LL_READ_BUF_MAX
              ? reply_buffer_get(0, size < 4096 ? 4096 : size) : NULL;
    if (!buf) {
        buf = malloc(size ? size : 1);
        owned = 1;
    }
    if (!buf) {
        t_req = NULL;
        fuse_reply_err(req, ENOMEM);
        return;
    }
    n = efs_fuse_read_ino(ino, buf, size, off, fi);
    t_req = NULL;
    if (n < 0)
        fuse_reply_err(req, -n);
    else
        fuse_reply_buf(req, buf, (size_t)n);
    if (owned)
        free(buf);
}

static void ll_write_run(fuse_req_t req, fuse_ino_t ino, const char *buf,
                     size_t size, off_t off, struct fuse_file_info *fi)
{
    int n;
    (void)ino;
    lookup_memo_mutation_begin();
    t_req = req;
    n = efs_fuse_write(NULL, buf, size, off, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (n < 0)
        fuse_reply_err(req, -n);
    else
        fuse_reply_write(req, (size_t)n);
}

static void ll_write_buf_run(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv,
                         off_t off, struct fuse_file_info *fi)
{
    int n;
    (void)ino;
    lookup_memo_mutation_begin();
    t_req = req;
    n = efs_fuse_write_buf(NULL, bufv, off, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (n < 0)
        fuse_reply_err(req, -n);
    else
        fuse_reply_write(req, (size_t)n);
}

static void ll_flush_run(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_flush_ino(ino, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    if (efs_dcache_trace_on())
        fprintf(stderr, "release ino=%llu begin\n", (unsigned long long)ino);
    rc = efs_fuse_release_ino(ino, fi);
    if (efs_dcache_trace_on())
        fprintf(stderr, "release ino=%llu rc=%d\n", (unsigned long long)ino, rc);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_fsync_run(fuse_req_t req, fuse_ino_t ino, int datasync,
                     struct fuse_file_info *fi)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_fsync_ino(ino, datasync, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

/* W26: fallocate. efs has no reservation (quota is charged at PUT, a
 * chunk that was never written is a hole the reader zero-fills), so the
 * only state a fallocate can change is the size:
 *   - mode 0, end past EOF  → extend like ftruncate (one size SETATTR;
 *     no zero fragments are written, no quota is reserved);
 *   - mode 0, end inside    → nothing to do, 0;
 *   - KEEP_SIZE inside EOF  → 0 (the bytes are already "allocated" in
 *     the only sense efs has);
 *   - KEEP_SIZE past EOF, PUNCH_HOLE, ZERO_RANGE, COLLAPSE, INSERT,
 *     UNSHARE → EOPNOTSUPP; the kernel then falls back where it can
 *     (glibc posix_fallocate writes, cp --sparse probes).
 * Without the handler libfuse answered ENOSYS, and the kernel's
 * fallocate() returned EOPNOTSUPP for every call including the extend.
 * The buffered writes are published before the size is read so an
 * extend to `end` never truncates bytes this client wrote past it. */
static void ll_fallocate_run(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset,
                         off_t length, struct fuse_file_info *fi)
{
    struct efs_inode row;
    uint64_t end;
    int rc, vk = virt_kind(ino);

    lookup_memo_mutation_begin();
    t_req = req;
    if (vk) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, vk == 2 ? EISDIR : EACCES);
        return;
    }
    if (offset < 0 || length <= 0 ||
        (uint64_t)offset > UINT64_MAX - (uint64_t)length) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, EINVAL);
        return;
    }
    if (mode & ~FALLOC_FL_KEEP_SIZE) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, EOPNOTSUPP);
        return;
    }
    end = (uint64_t)offset + (uint64_t)length;
    rc = efs_append_flush_report(fi, (efs_ino_t)ino);
    if (rc != EFS_OK) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, efs_rc_to_errno(rc));
        return;
    }
    if (efs_client_stat_ino((efs_ino_t)ino, &row) != EFS_OK) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (efs_mode_is_dir(row.mode)) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, EISDIR);
        return;
    }
    if (end <= row.size) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, 0);
        return;
    }
    if (mode & FALLOC_FL_KEEP_SIZE) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, EOPNOTSUPP);
        return;
    }
    rc = efs_fuse_truncate_ino(ino, (off_t)end, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct efs_dirh *dh;
    int vk = virt_kind(ino);
    t_req = req;
    if (vk == 1 || vk == 3) {
        t_req = NULL;
        fuse_reply_err(req, ENOTDIR);
        return;
    }
    if (vk == 2) {
        if (!feature_enabled(EFS_FEATURE_FIND)) {
            t_req = NULL;
            fuse_reply_err(req, ENOENT);
            return;
        }
        dh = dirh_new((efs_ino_t)ino);
        if (!dh) {
            t_req = NULL;
            fuse_reply_err(req, ENOMEM);
            return;
        }
        if (fi)
            fi->fh = (uint64_t)(uintptr_t)dh;
        else
            dirh_free(dh);
        t_req = NULL;
        fuse_reply_open(req, fi);
        return;
    }
    {
        struct efs_inode row;
        int rc = efs_client_stat_ino((efs_ino_t)ino, &row);
        t_req = NULL;
        if (rc != EFS_OK) {
            fuse_reply_err(req, ENOENT);
            return;
        }
        if (!efs_mode_is_dir(row.mode)) {
            fuse_reply_err(req, ENOTDIR);
            return;
        }
        dh = dirh_new(row.ino);
        if (!dh) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        if (fi)
            fi->fh = (uint64_t)(uintptr_t)dh;
        else
            dirh_free(dh);
        fuse_reply_open(req, fi);
    }
}

static void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                       struct fuse_file_info *fi)
{
    struct efs_dirh *dh = (fi && fi->fh)
                              ? (struct efs_dirh *)(uintptr_t)fi->fh
                              : NULL;
    struct efs_dirh local;
    char *buf;
    size_t used = 0;
    off_t cookie = 1;
    int rc;

    t_req = req;
    if (!dh) {
        memset(&local, 0, sizeof(local));
        local.ino = (efs_ino_t)ino;
        local.parent_ino = FUSE_ROOT_ID;
        local.self_mode = S_IFDIR | 0755;
        local.parent_mode = S_IFDIR | 0755;
        dh = &local;
    }
    /* Cookies: 1='.', 2='..', 3.. = entries. Last emitted is 2+count.
     * The kernel's next READDIR passes that cookie as off. */
    if (dh->listed && off >= 2 + (off_t)dh->count) {
        t_req = NULL;
        fuse_reply_buf(req, "", 0);
        return;
    }
    if (!dh->listed) {
        rc = dirh_fill(dh, ino);
        if (rc != 0) {
            t_req = NULL;
            fuse_reply_err(req, -rc);
            return;
        }
    }
    /* One reply buffer per FUSE worker; a du does ~2 READDIRs per
     * directory and malloc/free of the 128 KiB were visible (cfree 3.8 %
     * of client cycles on the Spark profile). */
    buf = reply_buffer_get(1, size ? size : 1);
    if (!buf) {
        t_req = NULL;
        if (dh == &local)
            free(local.ents);
        fuse_reply_err(req, ENOMEM);
        return;
    }
    if (off < cookie) {
        if (dirbuf_add(req, buf, size, &used, ".", ino, dh->self_mode, cookie))
            goto send;
    }
    cookie++;
    if (off < cookie) {
        if (dirbuf_add(req, buf, size, &used, "..", dh->parent_ino,
                       dh->parent_mode, cookie))
            goto send;
    }
    cookie++;
    {
        size_t i;
        for (i = 0; i < dh->count; i++, cookie++) {
            if (off >= cookie)
                continue;
            if (dirbuf_add(req, buf, size, &used, dh->ents[i].name,
                           (fuse_ino_t)dh->ents[i].st.st_ino,
                           dh->ents[i].st.st_mode, cookie))
                goto send;
        }
    }
    /* .stats / .find are lookup-only (same as the high-level path
     * intercept). Emitting them here makes shutil.rmtree / posix listdir
     * see reserved names and fail unlink with EACCES. */
send:
    t_req = NULL;
    fuse_reply_buf(req, buf, used);
    if (dh == &local)
        free(local.ents);
}

static void ll_releasedir(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
    (void)ino;
    if (fi && fi->fh)
        dirh_free((struct efs_dirh *)(uintptr_t)fi->fh);
    fuse_reply_err(req, 0);
}

static void ll_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct statvfs stbuf;
    int rc;
    (void)ino;
    t_req = req;
    rc = efs_fuse_statfs(NULL, &stbuf);
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_statfs(req, &stbuf);
}

static void ll_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    int rc;
    t_req = req;
    rc = efs_fuse_access_ino(ino, mask);
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

/* nfsd creates through vfs_create, which the FUSE kernel module turns
 * into FUSE_MKNOD, not FUSE_CREATE (only open(O_CREAT) uses the atomic
 * create). With no .mknod handler libfuse replied ENOSYS and the NFS
 * client saw EIO for every create. Regular files reuse the create path;
 * there is no open here, so no fh/open-note bookkeeping. */
static void ll_mknod_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode, dev_t rdev)
{
    efs_ino_t new_ino = 0;
    struct fuse_entry_param e;
    int rc;
    (void)rdev;
    lookup_memo_mutation_begin();
    t_req = req;
    if (!S_ISREG(mode))
        rc = -EPERM;
    else {
        rc = efs_fuse_create_at(parent, name, mode, NULL, &new_ino);
        if (rc == 0)
            rc = lookup_fill_committed((fuse_ino_t)new_ino, &e, "mknod",
                                       parent, name);
    }
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_entry(req, &e);
}

static void ll_create_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, struct fuse_file_info *fi)
{
    struct fuse_entry_param e;
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_create_at(parent, name, mode, fi, NULL);
    if (rc == 0)
        rc = lookup_fill((fuse_ino_t)efs_file_ino(fi, 0), &e, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_create(req, &e, fi);
}

static void ll_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     struct flock *lock)
{
    int rc;
    t_req = req;
    rc = efs_fuse_getlk_ino(ino, fi, lock);
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_lock(req, lock);
}

static void ll_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     struct flock *lock, int sleep)
{
    int rc;
    t_req = req;
    rc = efs_fuse_setlk_ino(ino, fi, lock, sleep);
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     int op)
{
    int rc;
    t_req = req;
    rc = efs_fuse_flock_ino(ino, fi, op);
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
                     struct fuse_file_info *fi)
{
    off_t r;
    t_req = req;
    r = efs_fuse_lseek_ino(ino, off, whence, fi);
    t_req = NULL;
    if (r < 0)
        fuse_reply_err(req, (int)-r);
    else
        fuse_reply_lseek(req, r);
}

static int xattr_user_name(const char *name)
{
    size_t n;

    if (!name || !name[0])
        return -EINVAL;
    n = strlen(name);
    if (n > EFS_XATTR_NAME_MAX)
        return -ERANGE;
    /* security.* and system.* stay unsupported. Accepting them makes every
     * create pay a Raft round trip for the LSM label. */
    if (strncmp(name, "user.", 5) != 0)
        return -EOPNOTSUPP;
    return 0;
}

static void ll_setxattr_run(fuse_req_t req, fuse_ino_t ino, const char *name,
                        const char *value, size_t size, int flags)
{
    int rc;

    lookup_memo_mutation_begin();
    t_req = req;
    rc = xattr_user_name(name);
    if (rc == 0 && virt_kind(ino))
        rc = -EOPNOTSUPP;
    if (rc == 0 && size > EFS_XATTR_VALUE_MAX)
        rc = -ERANGE;
    if (rc == 0) {
        uint32_t fl = (uint32_t)flags;

        if ((fl & ~(EFS_XATTR_CREATE | EFS_XATTR_REPLACE)) != 0 ||
            ((fl & EFS_XATTR_CREATE) && (fl & EFS_XATTR_REPLACE)))
            rc = -EINVAL;
    }
    if (rc == 0) {
        rc = efs_client_rpc_xattr(g_client.export_id, (efs_ino_t)ino,
                                  EFS_XATTR_SET, (uint32_t)flags, name,
                                  (uint16_t)strlen(name), value, (uint32_t)size,
                                  NULL, NULL);
        if (rc)
            rc = efs_rc_to_errno(rc);
    }
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

static void ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                        size_t size)
{
    uint8_t buf[EFS_XATTR_VALUE_MAX];
    uint32_t n = sizeof(buf);
    int rc;

    t_req = req;
    rc = xattr_user_name(name);
    if (rc == 0 && virt_kind(ino))
        rc = -EOPNOTSUPP;
    if (rc == 0) {
        rc = efs_client_rpc_xattr(g_client.export_id, (efs_ino_t)ino,
                                  EFS_XATTR_GET, 0, name,
                                  (uint16_t)strlen(name), NULL, 0, buf, &n);
        if (rc)
            rc = efs_rc_to_errno(rc);
    }
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else if (size == 0)
        fuse_reply_xattr(req, n);
    else if (size < n)
        fuse_reply_err(req, ERANGE);
    else
        fuse_reply_buf(req, (const char *)buf, n);
}

static void ll_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    uint8_t buf[EFS_XATTR_BLOB_MAX];
    uint32_t n = sizeof(buf);
    int rc = 0;

    t_req = req;
    if (virt_kind(ino))
        rc = -EOPNOTSUPP;
    if (rc == 0) {
        rc = efs_client_rpc_xattr(g_client.export_id, (efs_ino_t)ino,
                                  EFS_XATTR_LIST, 0, NULL, 0, NULL, 0, buf, &n);
        if (rc)
            rc = efs_rc_to_errno(rc);
    }
    t_req = NULL;
    if (rc)
        fuse_reply_err(req, -rc);
    else if (size == 0)
        fuse_reply_xattr(req, n);
    else if (size < n)
        fuse_reply_err(req, ERANGE);
    else
        fuse_reply_buf(req, (const char *)buf, n);
}

static void ll_removexattr_run(fuse_req_t req, fuse_ino_t ino, const char *name)
{
    int rc;

    lookup_memo_mutation_begin();
    t_req = req;
    rc = xattr_user_name(name);
    if (rc == 0 && virt_kind(ino))
        rc = -EOPNOTSUPP;
    if (rc == 0) {
        rc = efs_client_rpc_xattr(g_client.export_id, (efs_ino_t)ino,
                                  EFS_XATTR_REMOVE, 0, name,
                                  (uint16_t)strlen(name), NULL, 0, NULL, NULL);
        if (rc)
            rc = efs_rc_to_errno(rc);
    }
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

/* Low-level (inode-based) FUSE ops. Timeouts stay 0 (Part C later). */
static void ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                       int to_set, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_setattr_run(req, ino, attr, to_set, fi);
    efs_stop_mutation_leave();
}

static void ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_mkdir_run(req, parent, name, mode);
    efs_stop_mutation_leave();
}

static void ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_unlink_run(req, parent, name);
    efs_stop_mutation_leave();
}

static void ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_rmdir_run(req, parent, name);
    efs_stop_mutation_leave();
}

static void ll_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
                       const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_symlink_run(req, link, parent, name);
    efs_stop_mutation_leave();
}

static void ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                      fuse_ino_t newparent, const char *newname,
                      unsigned int flags)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_rename_run(req, parent, name, newparent, newname, flags);
    efs_stop_mutation_leave();
}

static void ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                    const char *newname)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_link_run(req, ino, newparent, newname);
    efs_stop_mutation_leave();
}

static void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_open_run(req, ino, fi);
    efs_stop_mutation_leave();
}

static void ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                     size_t size, off_t off, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_write_run(req, ino, buf, size, off, fi);
    efs_stop_mutation_leave();
}

static void ll_write_buf(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv,
                         off_t off, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_write_buf_run(req, ino, bufv, off, fi);
    efs_stop_mutation_leave();
}

static void ll_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_flush_run(req, ino, fi);
    efs_stop_mutation_leave();
}

static void ll_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                     struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_fsync_run(req, ino, datasync, fi);
    efs_stop_mutation_leave();
}

static void ll_fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset,
                         off_t length, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_fallocate_run(req, ino, mode, offset, length, fi);
    efs_stop_mutation_leave();
}

static void ll_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode, dev_t rdev)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_mknod_run(req, parent, name, mode, rdev);
    efs_stop_mutation_leave();
}

static void ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, struct fuse_file_info *fi)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_create_run(req, parent, name, mode, fi);
    efs_stop_mutation_leave();
}

static void ll_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                        const char *value, size_t size, int flags)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_setxattr_run(req, ino, name, value, size, flags);
    efs_stop_mutation_leave();
}

static void ll_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
    if (!efs_stop_mutation_enter()) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    ll_removexattr_run(req, ino, name);
    efs_stop_mutation_leave();
}

static const struct fuse_lowlevel_ops efs_ll_ops = {
    .init = efs_fuse_init,
    .destroy = efs_fuse_destroy,
    .lookup = ll_lookup,
    .forget = ll_forget,
    .forget_multi = ll_forget_multi,
    .getattr = ll_getattr,
    .setattr = ll_setattr,
    .readlink = ll_readlink,
    .mknod = ll_mknod,
    .mkdir = ll_mkdir,
    .unlink = ll_unlink,
    .rmdir = ll_rmdir,
    .symlink = ll_symlink,
    .rename = ll_rename,
    .link = ll_link,
    .open = ll_open,
    .read = ll_read,
    .write = ll_write,
    .write_buf = ll_write_buf,
    .flush = ll_flush,
    .release = ll_release,
    .fsync = ll_fsync,
    .fallocate = ll_fallocate,
    .opendir = ll_opendir,
    .readdir = ll_readdir,
    .releasedir = ll_releasedir,
    .statfs = ll_statfs,
    .access = ll_access,
    .create = ll_create,
    .getlk = ll_getlk,
    .setlk = ll_setlk,
    .flock = ll_flock,
    .lseek = ll_lseek,
    .setxattr = ll_setxattr,
    .getxattr = ll_getxattr,
    .listxattr = ll_listxattr,
    .removexattr = ll_removexattr,
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

/* Pin the client to the HCA's NUMA node: CPU affinity for every thread
 * created after this call (sched_setaffinity on the main thread is
 * inherited) and MPOL_PREFERRED for the heap, so the write/read paths'
 * buffer pool, rdcache and RDMA pool live beside the HCA.
 *
 * Why (Oct 1 2026, fstor007, 2 sockets / 8 nodes, HCA on node 2): a 20 GiB
 * 1 MiB dd wrote 21.2 s unpinned and 18.0 s pinned (1.0 → 1.2 GB/s; the
 * FUSE thread's fuse_buf_copy into a cold remote-node pool buffer was
 * 0.3 ms/MiB), and the cold read went 10.4 s → 6.2 s (2.1 → 3.5 GB/s). On a
 * single-node host (fcstor) sysfs reports node -1 or 0 and this is a
 * no-op. EFS_NUMA_NODE=none disables it; =N forces node N. */
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif
static void numa_pin_startup(const char *host, uint16_t port)
{
    const char *env = getenv("EFS_NUMA_NODE");
    int node = -1;
    char hca[64] = "env";
    if (env && (strcmp(env, "none") == 0 || strcmp(env, "-1") == 0))
        return;
    if (env && *env && strcmp(env, "auto") != 0)
        node = atoi(env);
    else
        node = efs_rdma_numa_node_for_host(host, port, hca, sizeof(hca));
    if (node < 0)
        return;
    char path[128], buf[4096];
    snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist",
             node);
    FILE *f = fopen(path, "r");
    if (!f || !fgets(buf, sizeof(buf), f)) {
        if (f)
            fclose(f);
        return;
    }
    fclose(f);
    cpu_set_t set;
    CPU_ZERO(&set);
    int ncpu = 0;
    for (char *p = buf; *p && *p != '\n';) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            break;
        if (*end == '-')
            b = strtol(end + 1, &end, 10);
        for (long c = a; c <= b && c < CPU_SETSIZE; c++) {
            CPU_SET((int)c, &set);
            ncpu++;
        }
        p = (*end == ',') ? end + 1 : end;
    }
    if (ncpu == 0 || sched_setaffinity(0, sizeof(set), &set) != 0)
        return;
    unsigned long mask[2] = {0, 0};
    if (node < 128)
        mask[node / 64] |= 1UL << (node % 64);
    long mrc = syscall(SYS_set_mempolicy, MPOL_PREFERRED, mask, 128);
    fprintf(stderr, "efs: numa pin node=%d cpus=%d (hca %s) mempolicy=%s\n",
            node, ncpu, hca, mrc == 0 ? "preferred" : strerror(errno));
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
static int g_perf_want;           /* --perf given; start after the fork */
static char g_perf_path[512];

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

static pid_t g_strace_pid = -1;
static int g_strace_want;         /* --strace given; start after the fork */
static char g_strace_path[512];

/* --strace: `strace -f -tt -T -o <path> -p <pid>` on the daemon. ptrace
 * stops every syscall of every thread, so this is for one client and a
 * wall question (where did an fsync's 8 s go), never for a bandwidth
 * number. Needs kernel.yama.ptrace_scope=0 (runtime on the test hosts).
 * EFS_STRACE_EXPR narrows it, e.g. "trace=fsync,writev,recvfrom,futex"
 * (passed as -e). */
static pid_t start_strace_recorder(pid_t target, const char *path)
{
    char dir[8192];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork strace");
        return -1;
    }
    if (pid == 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)target);
        const char *expr = getenv("EFS_STRACE_EXPR");
        const char *av[16];
        int n = 0;
        av[n++] = "strace";
        av[n++] = "-f";
        av[n++] = "-tt";
        av[n++] = "-T";
        av[n++] = "-qq";
        av[n++] = "-o";
        av[n++] = path;
        av[n++] = "-p";
        av[n++] = pid_str;
        if (expr && *expr) {
            av[n++] = "-e";
            av[n++] = expr;
        }
        av[n] = NULL;
        execvp("strace", (char *const *)av);
        perror("exec strace");
        _exit(1);
    }
    return pid;
}

/* SIGTERM makes perf finalize perf.data and strace detach; wait up to
 * ~5 s, then SIGKILL. */
static void stop_recorder(pid_t *pidp)
{
    if (*pidp > 0) {
        kill(*pidp, SIGTERM);
        for (int i = 0; i < 50; i++) {
            if (waitpid(*pidp, NULL, WNOHANG) == *pidp)
                break;
            usleep(100000);
        }
        kill(*pidp, SIGKILL);
        waitpid(*pidp, NULL, 0);
        *pidp = -1;
    }
}

static void stop_perf_recorder(void)
{
    stop_recorder(&g_strace_pid);
    stop_recorder(&g_perf_pid);
}

/* Custom fuse_main replacement with explicit active and idle worker limits. */
/* libfuse >= 3.12 exposes an active-worker limit; the old API only
 * bounded idle workers and could create more while callbacks blocked. */
static struct fuse_loop_config *efs_fuse_worker_config(const struct fuse_cmdline_opts *opts)
{
    struct fuse_loop_config *config = fuse_loop_cfg_create();
    if (!config)
        return NULL;
    unsigned max_threads = opts->max_threads ? opts->max_threads : 32;
    unsigned idle_threads = opts->max_idle_threads ? opts->max_idle_threads : 8;
    if (idle_threads > max_threads)
        idle_threads = max_threads;
    fuse_loop_cfg_set_clone_fd(config, opts->clone_fd);
    fuse_loop_cfg_set_max_threads(config, max_threads);
    fuse_loop_cfg_set_idle_threads(config, idle_threads);
    fprintf(stderr, "efs-fuse: workers max=%u idle=%u\n",
            max_threads, idle_threads);

    return config;
}

static int efs_fuse_main_mt(int argc, char *argv[]);

/* Mount bootstrap. The Raft+KV host does not serve GET_META (there is no
 * serialized table), so a fetch path cannot work. The client already has the shard->group->voter mapping compiled in
 * (kv_key.h / raft.h), so all it needs from the cluster is confirmation that
 * the Raft export exists (kv_has_root) plus a local shell export to route by.
 * Poll RAFT_STATUS on the discovered nodes until one reports kv_has_root. */
static int raft_bootstrap_metadata(void)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        for (uint32_t i = 0; i < g_client.node_count; i++) {
            int fd = efs_connect_tcp(g_client.nodes[i].addr,
                                     g_client.nodes[i].port);
            if (fd < 0)
                continue;
            efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
            efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
            uint8_t type = 0;
            void *payload = NULL;
            uint32_t plen = 0;
            if (efs_send_msg(fd, EFS_MSG_RAFT_STATUS, NULL, 0) == 0 &&
                efs_recv_msg(fd, &type, &payload, &plen) == 0 &&
                type == EFS_MSG_RAFT_STATUS_REPLY &&
                plen >= sizeof(struct efs_msg_raft_status_reply)) {
                struct efs_msg_raft_status_reply *r = payload;
                if (r->rc == EFS_OK && r->kv_has_root) {
                    free(payload);
                    close(fd);
                    goto ready;
                }
            }
            free(payload);
            close(fd);
        }
        usleep(100000); /* 100ms; mkfs/election may still be running */
    }
    fprintf(stderr,
            "efs-fuse: no node reports a Raft export "
            "(raft-status kv_has_root=0). Run 'efs-mgmt raft-mkfs' first.\n");
    return EFS_ERR_NOT_FOUND;

ready:
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    /* main() already ran efs_export_init on this struct; re-initialising
     * without freeing would orphan the whole first table (it memsets the
     * struct, so every init-time allocation leaks). */
    efs_export_free(&g_client.export);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    g_client.export.root.shard_bits = EFS_KV_SHARD_BITS;
    g_client.export.root.shard_count = 1u << EFS_KV_SHARD_BITS;
    g_client.export.chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    fprintf(stderr,
            "meta: raft host mounted export '%s' "
            "(bits=%u, no GET_META table)\n",
            g_client.export_name, EFS_KV_SHARD_BITS);
    return EFS_OK;
}

/* ---- --bench: the client-side ladder (P3, plan row L) ----
 * Levels, one variable apart: cpu (the client's arithmetic ceiling:
 * blake3 + XOR parity + the bounce copy, no network), put (the
 * per-fragment round trip via efs_client_put_fragments_parallel, no
 * FUSE/dcache/REPORT), write (efs_fuse_create -> efs_fuse_write 1 MiB ->
 * efs_fuse_fsync -> release in-process: the full pipeline with the flush
 * in the clock). dd through the mount is the fourth level and is measured
 * outside. Rules from the plan: production path only (no skipped hash, no
 * skipped REPORT), bytes counted after the reply/fsync, fixed queue depth
 * with p50/p99 per level, non-zero payloads, never strace. */
static const char *g_bench_kind;
static double g_bench_time = 10.0;
static uint64_t g_bench_mib = 64; /* per-file size for the write level */

static double bench_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define BENCH_LAT_MAX (1u << 20)
struct bench_lat {
    uint64_t *v;
    uint32_t n;
    uint32_t cap;
};

static void bench_lat_add(struct bench_lat *lv, uint64_t us)
{
    if (lv->n >= BENCH_LAT_MAX)
        return;
    if (lv->n == lv->cap) {
        uint32_t ncap = lv->cap ? lv->cap * 2 : 4096;
        if (ncap > BENCH_LAT_MAX)
            ncap = BENCH_LAT_MAX;
        uint64_t *nv = realloc(lv->v, (size_t)ncap * sizeof(*nv));
        if (!nv)
            return;
        lv->v = nv;
        lv->cap = ncap;
    }
    lv->v[lv->n++] = us;
}

static int bench_u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Merge every worker's samples, sort once, print the level line. */
static void bench_lat_report(const char *kind, const char *level, int qd,
                             struct bench_lat *lvs, int nlvs, uint64_t ops,
                             uint64_t errors, double wall, uint64_t op_bytes)
{
    uint64_t total = 0;
    for (int i = 0; i < nlvs; i++)
        total += lvs[i].n;
    uint64_t *all = malloc((size_t)(total ? total : 1) * sizeof(*all));
    uint64_t p50 = 0, p99 = 0, max = 0;
    if (all) {
        uint64_t k = 0;
        for (int i = 0; i < nlvs; i++) {
            memcpy(all + k, lvs[i].v, (size_t)lvs[i].n * sizeof(*all));
            k += lvs[i].n;
        }
        qsort(all, (size_t)k, sizeof(*all), bench_u64_cmp);
        if (k) {
            p50 = all[k / 2];
            p99 = all[(k * 99) / 100];
            max = all[k - 1];
        }
        free(all);
    }
    if (wall < 1e-9)
        wall = 1e-9;
    printf("BENCH_OK kind=%s level=%s qd=%d ops=%llu wall_s=%.3f ops_s=%.1f "
           "p50_us=%llu p99_us=%llu max_us=%llu GiB_s=%.3f errors=%llu\n",
           kind, level, qd, (unsigned long long)ops, wall, (double)ops / wall,
           (unsigned long long)p50, (unsigned long long)p99,
           (unsigned long long)max,
           (double)ops * op_bytes / (1 << 30) / wall,
           (unsigned long long)errors);
    fflush(stdout);
}

static void bench_fill_nonzero(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t x = seed ? seed : 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < len; i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint64_t v = x | 1ULL;
        size_t n = len - i < 8 ? len - i : 8;
        memcpy(buf + i, &v, n);
    }
}

/* Level cpu: chunk -> blake3 -> 2+1 encode -> bounce copy, then drop. */
struct bench_cpu_arg {
    int tid;
    double deadline;
    uint64_t ops;
    struct bench_lat lat;
};

static void *bench_cpu_worker(void *arg)
{
    struct bench_cpu_arg *a = arg;
    uint8_t *chunk = malloc(EFS_CHUNK_SIZE);
    uint8_t *bounce = malloc(EFS_CHUNK_SIZE + EFS_FRAGMENT_SIZE);
    uint8_t *f0 = malloc(EFS_FRAGMENT_SIZE);
    uint8_t *f1 = malloc(EFS_FRAGMENT_SIZE);
    uint8_t *f2 = malloc(EFS_FRAGMENT_SIZE);
    if (!chunk || !bounce || !f0 || !f1 || !f2) {
        free(chunk);
        free(bounce);
        free(f0);
        free(f1);
        free(f2);
        return NULL;
    }
    bench_fill_nonzero(chunk, EFS_CHUNK_SIZE, (uint64_t)a->tid + 1);
    uint8_t sum[EFS_HASH_SIZE];
    uint8_t *frags[EFS_NUM_FRAGMENTS] = { f0, f1, f2 };
    while (bench_now_sec() < a->deadline) {
        uint64_t t0 = fuse_now_us();
        efs_hash(chunk, EFS_CHUNK_SIZE, sum);
        efs_encode_chunk(chunk, EFS_CHUNK_SIZE, EFS_CHUNK_SIZE, frags);
        /* The RDMA bounce copy: fragments land in the registered buffer. */
        memcpy(bounce, f0, EFS_FRAGMENT_SIZE);
        memcpy(bounce + EFS_FRAGMENT_SIZE, f1, EFS_FRAGMENT_SIZE);
        memcpy(bounce + 2 * EFS_FRAGMENT_SIZE, f2, EFS_FRAGMENT_SIZE);
        bench_lat_add(&a->lat, fuse_now_us() - t0);
        a->ops++;
    }
    free(chunk);
    free(bounce);
    free(f0);
    free(f1);
    free(f2);
    return NULL;
}

static int efs_fuse_bench_cpu(double time_sec)
{
    static const int ladder[] = { 1, 2, 4, 8, 16 };
    printf("bench cpu time_s=%.3f chunk_bytes=%d threads=1,2,4,8,16\n",
           time_sec, EFS_CHUNK_SIZE);
    for (uint32_t li = 0; li < sizeof(ladder) / sizeof(ladder[0]); li++) {
        int nt = ladder[li];
        struct bench_cpu_arg *args = calloc((size_t)nt, sizeof(*args));
        pthread_t *tids = calloc((size_t)nt, sizeof(*tids));
        if (!args || !tids) {
            free(args);
            free(tids);
            return 1;
        }
        double deadline = bench_now_sec() + time_sec;
        double t0 = bench_now_sec();
        for (int i = 0; i < nt; i++) {
            args[i].tid = i;
            args[i].deadline = deadline;
            if (pthread_create(&tids[i], NULL, bench_cpu_worker, &args[i]) != 0)
                tids[i] = 0;
        }
        for (int i = 0; i < nt; i++) {
            if (tids[i])
                pthread_join(tids[i], NULL);
        }
        double wall = bench_now_sec() - t0;
        uint64_t ops = 0;
        struct bench_lat *lvs = calloc((size_t)nt, sizeof(*lvs));
        for (int i = 0; i < nt; i++) {
            ops += args[i].ops;
            if (lvs)
                lvs[i] = args[i].lat;
        }
        bench_lat_report("cpu", "arith", nt, lvs ? lvs : &args[0].lat, nt, ops,
                         0, wall, EFS_CHUNK_SIZE);
        free(lvs);
        for (int i = 0; i < nt; i++)
            free(args[i].lat.v);
        free(args);
        free(tids);
    }
    return 0;
}

/* Level put: efs_client_put_fragments_parallel at a fixed queue depth. */
struct bench_put_arg {
    int tid;
    double deadline;
    uint64_t ops;
    uint64_t errors;
    struct bench_lat lat;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
};

static void *bench_put_worker(void *arg)
{
    struct bench_put_arg *a = arg;
    efs_ino_t ino = ((efs_ino_t)0xBEEF << 32) | (uint64_t)(a->tid + 1);
    uint32_t ci_base = (uint32_t)a->tid * (1u << 22);
    uint32_t seq = 0;
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    while (bench_now_sec() < a->deadline) {
        uint32_t ci = ci_base + seq++;
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);
        /* Production hashes every fragment of every chunk; no skipped hash. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            efs_hash(a->frags[i], EFS_FRAGMENT_SIZE, sums[i]);
        uint64_t t0 = fuse_now_us();
        int rc = efs_client_put_fragments_parallel(
            ino, ci, nodes, (const uint8_t **)a->frags, EFS_FRAGMENT_SIZE, sums, NULL);
        bench_lat_add(&a->lat, fuse_now_us() - t0);
        if (rc == EFS_OK)
            a->ops++;
        else
            a->errors++;
    }
    return NULL;
}

static int efs_fuse_bench_put(double time_sec)
{
    static const int ladder[] = { 1, 16, 64, 256 };
    printf("bench put time_s=%.3f frag_bytes=%d qds=1,16,64,256 nodes=%u\n",
           time_sec, EFS_FRAGMENT_SIZE, g_client.node_count);
    for (uint32_t li = 0; li < sizeof(ladder) / sizeof(ladder[0]); li++) {
        int nt = ladder[li];
        struct bench_put_arg *args = calloc((size_t)nt, sizeof(*args));
        pthread_t *tids = calloc((size_t)nt, sizeof(*tids));
        if (!args || !tids) {
            free(args);
            free(tids);
            return 1;
        }
        double deadline = bench_now_sec() + time_sec;
        double t0 = bench_now_sec();
        for (int i = 0; i < nt; i++) {
            args[i].tid = i;
            args[i].deadline = deadline;
            int ok = 0;
            for (int f = 0; f < EFS_NUM_FRAGMENTS; f++) {
                args[i].frags[f] = malloc(EFS_FRAGMENT_SIZE);
                if (args[i].frags[f])
                    bench_fill_nonzero(args[i].frags[f], EFS_FRAGMENT_SIZE,
                                       (uint64_t)i * 3 + (uint64_t)f + 1);
                else
                    ok = -1;
            }
            if (ok != 0 ||
                pthread_create(&tids[i], NULL, bench_put_worker, &args[i]) != 0) {
                args[i].errors++;
                tids[i] = 0;
            }
        }
        for (int i = 0; i < nt; i++) {
            if (tids[i])
                pthread_join(tids[i], NULL);
        }
        double wall = bench_now_sec() - t0;
        uint64_t ops = 0, errors = 0;
        struct bench_lat *lvs = calloc((size_t)nt, sizeof(*lvs));
        for (int i = 0; i < nt; i++) {
            ops += args[i].ops;
            errors += args[i].errors;
            if (lvs)
                lvs[i] = args[i].lat;
        }
        /* ops are chunks (128 KiB logical, 3 x 64 KiB stored). */
        bench_lat_report("put", "roundtrip", nt, lvs ? lvs : &args[0].lat, nt,
                         ops, errors, wall, EFS_CHUNK_SIZE);
        printf("BENCH_NOTE kind=put qd=%d frags_s=%.1f stored_GiB_s=%.3f\n", nt,
               (double)ops * EFS_NUM_FRAGMENTS / (wall > 1e-9 ? wall : 1e-9),
               (double)ops * EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE / (1 << 30) /
                   (wall > 1e-9 ? wall : 1e-9));
        fflush(stdout);
        free(lvs);
        for (int i = 0; i < nt; i++) {
            for (int f = 0; f < EFS_NUM_FRAGMENTS; f++)
                free(args[i].frags[f]);
            free(args[i].lat.v);
        }
        free(args);
        free(tids);
    }
    return 0;
}

/* Level write: create -> 1 MiB efs_fuse_write -> fsync -> release, the
 * ll_* handlers minus the kernel. The fsync is inside the file's wall. */
struct bench_wr_arg {
    int tid;
    double deadline;
    uint64_t bytes;
    uint64_t wops;
    uint64_t errors;
    uint64_t fsync_us;
    double wall_s;
    struct bench_lat lat;
};

static void *bench_wr_worker(void *arg)
{
    struct bench_wr_arg *a = arg;
    double t_start = bench_now_sec();
    char name[64];
    snprintf(name, sizeof(name), "bench-%d-w%d", (int)getpid(), a->tid);
    struct fuse_file_info fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = O_WRONLY | O_CREAT | O_TRUNC;
    efs_ino_t ino = 0;
    double t0 = bench_now_sec();
    int rc = efs_fuse_create_at(EFS_ROOT_INO, name, 0644, &fi, &ino);
    if (rc != 0) {
        a->errors++;
        a->wall_s = bench_now_sec() - t_start;
        return NULL;
    }
    size_t wsz = 1u << 20;
    uint8_t *buf = malloc(wsz);
    if (!buf) {
        a->errors++;
        a->wall_s = bench_now_sec() - t_start;
        return NULL;
    }
    bench_fill_nonzero(buf, wsz, (uint64_t)a->tid + 11);
    uint64_t target = g_bench_mib << 20;
    off_t off = 0;
    while ((uint64_t)off < target && bench_now_sec() < a->deadline) {
        uint64_t t = fuse_now_us();
        int w = efs_fuse_write(NULL, (const char *)buf, wsz, off, &fi);
        bench_lat_add(&a->lat, fuse_now_us() - t);
        if (w != (int)wsz) {
            a->errors++;
            break;
        }
        a->wops++;
        a->bytes += (uint64_t)w;
        off += w;
    }
    free(buf);
    uint64_t tf = fuse_now_us();
    if (efs_fuse_fsync_ino(ino, 0, &fi) != 0)
        a->errors++;
    a->fsync_us = fuse_now_us() - tf;
    (void)efs_fuse_release_ino(ino, &fi);
    /* The level made a real file; remove it (REPORT already published). */
    (void)efs_client_unlink(EFS_ROOT_INO, name, false);
    a->wall_s = bench_now_sec() - t0;
    return NULL;
}

static int efs_fuse_bench_write(double time_sec)
{
    static const int ladder[] = { 1, 4, 16, 64 };
    printf("bench write time_s=%.3f file_mib=%llu files=1,4,16,64\n", time_sec,
           (unsigned long long)g_bench_mib);
    for (uint32_t li = 0; li < sizeof(ladder) / sizeof(ladder[0]); li++) {
        int nt = ladder[li];
        struct bench_wr_arg *args = calloc((size_t)nt, sizeof(*args));
        pthread_t *tids = calloc((size_t)nt, sizeof(*tids));
        if (!args || !tids) {
            free(args);
            free(tids);
            return 1;
        }
        double deadline = bench_now_sec() + time_sec;
        double t0 = bench_now_sec();
        for (int i = 0; i < nt; i++) {
            args[i].tid = i;
            args[i].deadline = deadline;
            if (pthread_create(&tids[i], NULL, bench_wr_worker, &args[i]) != 0) {
                args[i].errors++;
                tids[i] = 0;
            }
        }
        for (int i = 0; i < nt; i++) {
            if (tids[i])
                pthread_join(tids[i], NULL);
        }
        double wall = bench_now_sec() - t0;
        if (wall < 1e-9)
            wall = 1e-9;
        uint64_t bytes = 0, wops = 0, errors = 0, fsync_us = 0;
        struct bench_lat *lvs = calloc((size_t)nt, sizeof(*lvs));
        for (int i = 0; i < nt; i++) {
            bytes += args[i].bytes;
            wops += args[i].wops;
            errors += args[i].errors;
            fsync_us += args[i].fsync_us;
            if (lvs)
                lvs[i] = args[i].lat;
        }
        /* GiB/s counts bytes after fsync returned (the honest clock). */
        bench_lat_report("write", "pipeline", nt, lvs ? lvs : &args[0].lat, nt,
                         wops, errors, wall, 0);
        printf("BENCH_NOTE kind=write files=%d bytes=%llu wall_s=%.3f "
               "GiB_s=%.3f fsync_avg_ms=%.1f\n",
               nt, (unsigned long long)bytes, wall,
               (double)bytes / (1 << 30) / wall,
               (double)fsync_us / (nt ? nt : 1) / 1000.0);
        fflush(stdout);
        free(lvs);
        for (int i = 0; i < nt; i++)
            free(args[i].lat.v);
        free(args);
        free(tids);
    }
    return 0;
}

static int efs_fuse_bench_run(const char *kind, double time_sec)
{
    if (strcmp(kind, "cpu") != 0) {
        /* The serving-path init from efs_fuse_init, minus the conn knobs:
         * dcache, stage pin hooks, the background REPORT thread. */
        efs_dcache_init();
        efs_client_stage_set_pin_hooks(efs_client_ino_is_open,
                                       efs_client_ino_has_plock);
        efs_client_enable_meta_batch(4096);
    }
    pid_t bench_perf = -1;
    if (g_perf_want) {
        bench_perf = start_perf_recorder(getpid(), g_perf_path);
        if (bench_perf < 0)
            fprintf(stderr, "Warning: could not start perf; continuing\n");
    }
    int rc;
    if (strcmp(kind, "cpu") == 0)
        rc = efs_fuse_bench_cpu(time_sec);
    else if (strcmp(kind, "put") == 0)
        rc = efs_fuse_bench_put(time_sec);
    else if (strcmp(kind, "write") == 0)
        rc = efs_fuse_bench_write(time_sec);
    else {
        fprintf(stderr, "bench: unknown kind '%s' (cpu|put|write)\n", kind);
        rc = 1;
    }
    if (bench_perf > 0) {
        kill(bench_perf, SIGTERM);
        for (int i = 0; i < 50; i++) {
            if (waitpid(bench_perf, NULL, WNOHANG) == bench_perf)
                break;
            usleep(100000);
        }
        kill(bench_perf, SIGKILL);
        waitpid(bench_perf, NULL, 0);
        char cmd[600];
        snprintf(cmd, sizeof(cmd),
                 "perf report --stdio --no-children --percent-limit=2 -i '%s' "
                 "2>/dev/null | grep -E '^\\s+[0-9]+\\.[0-9]+%%' | head -8 | "
                 "sed 's/^/PERF_TOP/'",
                 g_perf_path);
        (void)system(cmd);
        printf("perf report: %s\n", g_perf_path);
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && (strcmp(argv[1], "--stop") == 0 ||
                      strcmp(argv[1], "--resume") == 0)) {
        int force = argc == 4 && strcmp(argv[3], "--force-discard") == 0;
        if (argc != 3 && !force)
            return 2;
        const char *command = strcmp(argv[1], "--resume") == 0 ? "RESUME\n" :
                              force ? "FORCE\n" : "DRAIN\n";
        int rc = efs_stop_client(argv[2], command);
        if (rc)
            fprintf(stderr, "efs-fuse: stop control refused or unavailable (%d); mount retained\n", rc);
        return rc;
    }
    efs_version_check_argv("efs-fuse", argc, argv);

    /* Line-buffer logs even when stdout is a pipe (client.sh | tee). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    efs_log_timestamps_install();
    efs_fuse_install_crash_handlers();

    int perf = 0, strace_opt = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--perf") == 0)
            perf = 1;
        else if (strcmp(argv[i], "--strace") == 0)
            strace_opt = 1;
        else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
            g_bench_kind = argv[i + 1];
        else if (strcmp(argv[i], "--bench-time") == 0 && i + 1 < argc)
            g_bench_time = atof(argv[i + 1]);
        else if (strcmp(argv[i], "--bench-mib") == 0 && i + 1 < argc)
            g_bench_mib = strtoull(argv[i + 1], NULL, 10);
    }
    if (g_bench_time <= 0.0)
        g_bench_time = 10.0;

    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <node1:port> [<node2:port> ...] <export-name> <mountpoint> [fuse options] [--perf] [--strace]\n"
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

    if (strcmp(export_name, "default") != 0) {
        fprintf(stderr, "efs-fuse: legacy export label '%s' is ignored; "
                "mounting the single export 'default' (id=1)\n", export_name);
        export_name = "default";
    }

    /* Level cpu needs no cluster, no export and no mount: the client's
     * arithmetic only. Run before any network or NUMA setup. */
    if (g_bench_kind && strcmp(g_bench_kind, "cpu") == 0)
        return efs_fuse_bench_run(g_bench_kind, g_bench_time);

    /* Canonical absolute mountpoint, used to render host-absolute .find
     * results. realpath(..., NULL) mallocs; fall back to the raw argument. */
    {
        char *rp = realpath(mountpoint, NULL);
        strncpy(g_mountpoint, rp ? rp : mountpoint, sizeof(g_mountpoint) - 1);
        g_mountpoint[sizeof(g_mountpoint) - 1] = '\0';
        free(rp);
        size_t mpl = strlen(g_mountpoint);
        while (mpl > 1 && g_mountpoint[mpl - 1] == '/')
            g_mountpoint[--mpl] = '\0';
    }

    {
        char ver[256];
        fprintf(stderr, "%s, build=%s: starting export=%s mount=%s\n",
                efs_version_string("efs-fuse", ver, sizeof(ver)),
                EFS_BUILD_ID, export_name, g_mountpoint);
    }

    {
        /* Before any thread or RDMA buffer exists: affinity is inherited
         * and the mempolicy applies to pages touched from here on. */
        char host[64];
        uint16_t port = 0;
        if (parse_addr(nodes[0], host, sizeof(host), &port) == 0)
            numa_pin_startup(host, port);
    }

    efs_client_init_nodes(&g_client, nodes, node_count);
    strncpy(g_client.export_name, export_name, EFS_MAX_NAME - 1);
    g_client.export_id = 1;
    pthread_mutex_init(&g_client.lock, NULL);
    efs_export_init(&g_client.export, g_client.export_id, g_client.export_name);
    efs_client_setup_ino_namespace();

    /* argv addresses are bootstrap only. init_nodes assigns 1..N, which
     * will not match a cluster started with --node-id 3,4,5. Always take
     * membership and real ids from LIST_NODES. */
    {
        char host[64];
        uint16_t port = 0;
        int discovered = 0;
        for (uint32_t i = 0; i < node_count; i++) {
            if (parse_addr(nodes[i], host, sizeof(host), &port) != 0)
                continue;
            if (efs_client_discover_nodes(&g_client, host, port) == 0) {
                printf("Discovered %u cluster nodes from %s\n",
                       g_client.node_count, nodes[i]);
                for (uint32_t n = 0; n < g_client.node_count; n++)
                    printf("  node id=%u %s:%u\n", g_client.nodes[n].id,
                           g_client.nodes[n].addr, g_client.nodes[n].port);
                discovered = 1;
                break;
            }
        }
        if (!discovered) {
            fprintf(stderr, "Could not discover cluster from any bootstrap node\n");
            return 1;
        }
    }

    /* Bootstrap metadata from the Raft+KV host: a thin RAFT_STATUS poll
     * for kv_has_root, then a local shell export — the client has the
     * shard->group->voter mapping compiled in (include/efs/kv_key.h,
     * include/efs/raft.h) and routes by it. No table is fetched. */
    printf("fetching metadata...\n");
    fflush(stdout);
    int rc = raft_bootstrap_metadata();
    if (rc != 0) {
        fprintf(stderr, "Could not establish the single Raft export root (%s). "
                "Check cluster health with efs-mgmt raft-status; initialize "
                "an unformatted cluster with efs-mgmt raft-mkfs.\n",
                efs_strerror(rc));
        return 1;
    }
    /* PUTs carry export_id; do not leave the hardcoded 1 if meta says otherwise. */
    g_client.export_id = g_client.export.id ? g_client.export.id : 1;
    if (g_client.export.name[0] &&
        strcmp(g_client.export.name, g_client.export_name) != 0) {
        /* Hard fail: writing into the wrong export used to be possible when
         * the server minted an empty export for an unknown name — the
         * client's flushes then lost every generation check against the real
         * export and looped STALE forever. Refuse instead. */
        fprintf(stderr,
                "ERROR: requested export '%s' but the cluster serves '%s' "
                "(id=%u). Refusing to mount the wrong export.\n",
                g_client.export_name, g_client.export.name, g_client.export_id);
        return 1;
    }
    printf("export id=%u name=%s\n", g_client.export_id,
           g_client.export.name[0] ? g_client.export.name : g_client.export_name);
    {
        /* Post-step-11 there is no bootstrap table fetch: the staging table
         * starts empty and fills on demand (client-cache Part A bounds it
         * to EFS_CLIENT_META_MB). Print cache occupancy, not a table scan. */
        struct efs_inode root;
        int rrc = efs_export_get_inode(&g_client.export, EFS_ROOT_INO, &root);
        printf("meta ready staged_rows=%llu staged_chunks=%llu "
               "staged_bytes=%llu root_get=%d mode=%o\n",
               (unsigned long long)g_client.export.inode_count,
               (unsigned long long)g_client.export.chunk_count,
               (unsigned long long)efs_export_staged_bytes(&g_client.export),
               rrc, rrc == 0 ? root.mode : 0);
    }
    fflush(stdout);
    /* The server is the sole metadata writer: no client write lease, no
     * client-side root flush. The background REPORT thread starts in .init
     * (efs_client_enable_meta_batch). */

    char *fuse_argv[64];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    /* Timeouts are per lookup/getattr reply (always 0). High-level
     * attr_timeout= mount options are not valid for fuse_session_new. */
    while (arg_idx < argc && fuse_argc < 63) {
        if (strcmp(argv[arg_idx], "--perf") == 0 ||
            strcmp(argv[arg_idx], "--strace") == 0) {
            arg_idx++;
            continue;
        }
        if (strcmp(argv[arg_idx], "--bench") == 0 ||
            strcmp(argv[arg_idx], "--bench-time") == 0 ||
            strcmp(argv[arg_idx], "--bench-mib") == 0) {
            arg_idx += 2; /* flag + value */
            continue;
        }
        fuse_argv[fuse_argc++] = argv[arg_idx++];
    }
    fuse_argv[fuse_argc] = NULL;

    if (perf) {
        /* The recorder is started inside efs_fuse_main_mt, AFTER the
         * daemon fork: started here it attaches to the parent, which
         * _exit()s once FUSE_INIT is answered, and perf dies with
         * "Couldn't create thread/CPU maps: No such process" (the
         * 20260929-021039-iorperf run recorded nothing on 9 clients). */
        const char *pp = getenv("EFS_PERF_PATH");
        if (pp && *pp)
            snprintf(g_perf_path, sizeof(g_perf_path), "%s", pp);
        else
            snprintf(g_perf_path, sizeof(g_perf_path), "/tmp/efs-fuse-perf-%d/perf.data", (int)getpid());
        g_perf_want = 1;
    }
    if (strace_opt) {
        const char *sp = getenv("EFS_STRACE_PATH");
        if (sp && *sp)
            snprintf(g_strace_path, sizeof(g_strace_path), "%s", sp);
        else
            snprintf(g_strace_path, sizeof(g_strace_path), "/tmp/efs-fuse-strace-%d.txt", (int)getpid());
        g_strace_want = 1;
    }

    /* P3 client ladder: put/write run here — the client is fully
     * bootstrapped (discovery + metadata), no FUSE session exists. */
    if (g_bench_kind) {
        int brc = efs_fuse_bench_run(g_bench_kind, g_bench_time);
        efs_client_shutdown();
        return brc;
    }

    int ret = efs_fuse_main_mt(fuse_argc, fuse_argv);
    stop_perf_recorder();
    /* destroy() already shut down on clean unmount; call again is a no-op. */
    efs_client_shutdown();
    return ret;
}

static int efs_fuse_main_mt(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_cmdline_opts opts;
    struct fuse_loop_config *config = NULL;
    int ret = -1;

    if (fuse_parse_cmdline(&args, &opts) != 0)
        return 1;

    if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        ret = 0;
        goto out;
    }

    if (opts.show_help) {
        printf("usage: %s [options] <mountpoint>\n\n", args.argv[0]);
        fuse_cmdline_help();
        fuse_lowlevel_help();
        ret = 1;
        goto out;
    }

    if (!opts.mountpoint) {
        fprintf(stderr, "error: no mountpoint specified\n");
        ret = 1;
        goto out;
    }

    /* Do not fuse_daemonize(): it returns the parent at fuse_mount, before
     * the session loop, and it redirects stdout/stderr to /dev/null so a
     * `>fuse.log` wrapper only captures pre-mount lines. Fork first, mount
     * only in the child, and hold the parent until FUSE_INIT. */
    if (!opts.foreground) {
        int pfd[2];
        if (pipe(pfd) != 0) {
            ret = 1;
            goto out;
        }
        pid_t child = fork();
        if (child < 0) {
            close(pfd[0]);
            close(pfd[1]);
            ret = 1;
            goto out;
        }
        if (child > 0) {
            close(pfd[1]);
            int ok = efs_fuse_wait_ready(pfd[0], child, 30000);
            close(pfd[0]);
            if (ok != 0)
                fprintf(stderr,
                        "ERROR: efs-fuse did not start serving within 30s\n");
            /* Do not return through efs_client_shutdown: that would close the
             * child's dup'd server sockets. Freeing the bootstrap metadata
             * table is heap-only and safe — and keeps the parent's exit
             * leak-clean under valgrind. */
            efs_export_free(&g_client.export);
            fflush(stderr);
            _exit(ok == 0 ? 0 : 1);
        }
        close(pfd[0]);
        g_fuse_ready_wr = pfd[1];
        (void)setsid();
        fprintf(stderr, "efs-fuse daemon pid=%d\n", (int)getpid());
        fflush(stderr);
    }

    /* This is the process that serves the mount (the child, or the only
     * process under -f). Attach the recorder to it; the parent above has
     * already been excluded. */
    if (g_perf_want) {
        g_perf_pid = start_perf_recorder(getpid(), g_perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        else
            fprintf(stderr, "efs-fuse perf recorder pid=%d -> %s\n", (int)g_perf_pid, g_perf_path);
        fflush(stderr);
    }
    if (g_strace_want) {
        g_strace_pid = start_strace_recorder(getpid(), g_strace_path);
        if (g_strace_pid < 0)
            fprintf(stderr, "Warning: could not start strace; continuing without it\n");
        else
            fprintf(stderr, "efs-fuse strace pid=%d -> %s\n", (int)g_strace_pid, g_strace_path);
        fflush(stderr);
    }

    g_fuse_se = fuse_session_new(&args, &efs_ll_ops, sizeof(efs_ll_ops), NULL);
    if (g_fuse_se == NULL) {
        ret = 1;
        goto out;
    }

    if (fuse_set_signal_handlers(g_fuse_se) != 0) {
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }

    if (fuse_session_mount(g_fuse_se, opts.mountpoint) != 0) {
        fuse_remove_signal_handlers(g_fuse_se);
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }

    /* Idle limits do not bound active request workers. Each worker owns
     * a FUSE receive buffer, stack and TLS scratch outside the cache cap. */
    config = efs_fuse_worker_config(&opts);
    if (!config) {
        fuse_remove_signal_handlers(g_fuse_se);
        fuse_session_unmount(g_fuse_se);
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }

    if (efs_stop_start(opts.mountpoint, efs_control_drain) != 0) {
        fprintf(stderr, "efs-fuse: cannot establish safe-stop control channel\n");
        fuse_loop_cfg_destroy(config);
        fuse_remove_signal_handlers(g_fuse_se);
        fuse_session_unmount(g_fuse_se);
        fuse_session_destroy(g_fuse_se);
        g_fuse_se = NULL;
        ret = 1;
        goto out;
    }
    (void)ll_inval_start();
    ret = fuse_session_loop_mt(g_fuse_se, config);
    fuse_loop_cfg_destroy(config);
    efs_stop_finish();
    ll_inval_stop();

    fuse_remove_signal_handlers(g_fuse_se);
    fuse_session_unmount(g_fuse_se);
    fuse_session_destroy(g_fuse_se);
    g_fuse_se = NULL;

out:
    if (g_fuse_ready_wr >= 0) {
        close(g_fuse_ready_wr);
        g_fuse_ready_wr = -1;
    }
    free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return ret;
}
