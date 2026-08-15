#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include "client_internal.h"
#include "efs/common.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <fuse.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
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
        int fd = efs_connect_tcp(g_client.nodes[i].addr, g_client.nodes[i].port);
        if (fd < 0)
            continue;
        efs_set_recv_timeout(fd, 2000);
        efs_set_send_timeout(fd, 2000);
        struct efs_msg_get_features req;
        memset(&req, 0, sizeof(req));
        strncpy(req.export_name, name, EFS_MAX_NAME - 1);
        uint8_t type = 0;
        void *reply = NULL;
        uint32_t rlen = 0;
        int ok = (efs_send_msg(fd, EFS_MSG_GET_FEATURES, &req, sizeof(req)) == 0 &&
                  efs_recv_msg(fd, &type, &reply, &rlen) == 0 &&
                  type == EFS_MSG_GET_FEATURES_REPLY &&
                  rlen >= sizeof(struct efs_msg_features_reply));
        uint32_t feat = 0;
        uint8_t status = EFS_FEATURES_NOT_FOUND;
        if (ok) {
            struct efs_msg_features_reply *r = reply;
            feat = r->features;
            status = r->status;
        }
        free(reply);
        close(fd);
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
 * Single-command, e.g. cat ".find/PATTERN". Backed by a lazily built name
 * index so the read/write data path is untouched. */
static int path_is_find(const char *path, struct efs_inode *parent_out)
{
    if (!path)
        return -ENOENT;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, EFS_FIND_NAME) != 0)
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

/* Lightweight shape check: does path name a .find query file
 * ("<dir>/.find/<term>")? The term must be a single non-empty component (no
 * '/'). Does not resolve anything — safe for hot-path guards. */
static int path_is_find_query_path(const char *path)
{
    if (!path)
        return 0;
    const char *m = strstr(path, "/" EFS_FIND_NAME "/");
    if (!m)
        return 0;
    const char *term = m + strlen("/" EFS_FIND_NAME "/");
    return *term != '\0' && strchr(term, '/') == NULL;
}

/* Resolve a .find query path "<dir>/.find/<term>": the directory whose subtree
 * to search into parent_out, and the raw glob pattern into term_out. Returns 0
 * on match, -ENOENT otherwise. */
static int path_find_query(const char *path, struct efs_inode *parent_out,
                           char *term_out, size_t term_cap)
{
    if (!path)
        return -ENOENT;
    const char *m = strstr(path, "/" EFS_FIND_NAME "/");
    if (!m)
        return -ENOENT;
    const char *term = m + strlen("/" EFS_FIND_NAME "/");
    if (!*term || strchr(term, '/'))
        return -ENOENT;
    char dir[EFS_MAX_PATH];
    size_t dlen = (size_t)(m - path);
    if (dlen == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (dlen >= sizeof(dir))
            return -ENOENT;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
    }
    struct efs_inode parent;
    if (efs_client_lookup(dir, &parent) != 0)
        return -ENOENT;
    if (!efs_mode_is_dir(parent.mode))
        return -ENOENT;
    if (term_out) {
        if (strlen(term) >= term_cap)
            return -ENOENT;
        strcpy(term_out, term);
    }
    if (parent_out)
        *parent_out = parent;
    return 0;
}

static ino_t find_synthetic_ino(efs_ino_t parent_ino)
{
    return (ino_t)((1ULL << 61) | (parent_ino & ((1ULL << 61) - 1)));
}

/* Reserved virtual-file names: never allow a real file/dir to shadow them. */
static int name_is_reserved(const char *name)
{
    return strcmp(name, EFS_STATS_NAME) == 0 || strcmp(name, EFS_FIND_NAME) == 0;
}

enum find_match { FIND_EXACT, FIND_PREFIX, FIND_SUFFIX, FIND_SUBSTR };

/* Lazily built snapshot of the inode table for .find searches. The table is
 * memcpy'd under a brief g_client.lock; the arena + ino hash are built after
 * releasing it, so indexing never stalls the data path. Rebuilt per query so
 * results are always fresh. */
struct find_ent {
    efs_ino_t ino;
    efs_ino_t parent;
    uint32_t name_off; /* offset into names arena */
    uint8_t is_dir;
};

static struct {
    struct find_ent *ents;
    uint64_t count;
    char *names; /* packed NUL-separated names */
    uint64_t *ino_keys; /* ino -> ents index (open addressing; key 0 = empty) */
    uint64_t *ino_vals;
    uint64_t ino_mask;
} g_find_idx;
static pthread_mutex_t g_find_idx_mu = PTHREAD_MUTEX_INITIALIZER;

/* Canonical absolute mountpoint, set once in main. .find results are rendered
 * as host-absolute paths (mountpoint + fs-root path) so they can be piped or
 * looped over from any working directory. */
static char g_mountpoint[EFS_MAX_PATH] = "/";

static void find_index_free_locked(void)
{
    free(g_find_idx.ents);
    g_find_idx.ents = NULL;
    free(g_find_idx.names);
    g_find_idx.names = NULL;
    free(g_find_idx.ino_keys);
    g_find_idx.ino_keys = NULL;
    free(g_find_idx.ino_vals);
    g_find_idx.ino_vals = NULL;
    g_find_idx.count = 0;
    g_find_idx.ino_mask = 0;
}

/* Rebuild the index from the live table. Caller holds g_find_idx_mu. */
static int find_index_build_locked(void)
{
    pthread_mutex_lock(&g_client.lock);
    uint64_t n = g_client.export.inode_count;
    struct efs_inode *snap = malloc((n ? n : 1) * sizeof(*snap));
    if (!snap) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_NOMEM;
    }
    memcpy(snap, g_client.export.inodes, n * sizeof(*snap));
    pthread_mutex_unlock(&g_client.lock);

    struct find_ent *ents = malloc((n ? n : 1) * sizeof(*ents));
    if (!ents) {
        free(snap);
        return EFS_ERR_NOMEM;
    }
    size_t arena = 0;
    for (uint64_t i = 0; i < n; i++)
        arena += strnlen(snap[i].name, EFS_MAX_NAME) + 1;
    char *names = malloc(arena ? arena : 1);
    if (!names) {
        free(ents);
        free(snap);
        return EFS_ERR_NOMEM;
    }

    uint64_t count = 0;
    size_t off = 0;
    for (uint64_t i = 0; i < n; i++) {
        size_t L = strnlen(snap[i].name, EFS_MAX_NAME);
        memcpy(names + off, snap[i].name, L);
        names[off + L] = '\0';
        ents[count].ino = snap[i].ino;
        ents[count].parent = snap[i].parent;
        ents[count].is_dir = efs_mode_is_dir(snap[i].mode) ? 1 : 0;
        ents[count].name_off = (uint32_t)off;
        count++;
        off += L + 1;
    }
    free(snap);

    uint64_t mask = 16;
    while (mask < count * 2)
        mask <<= 1;
    uint64_t *keys = calloc(mask, sizeof(uint64_t));
    uint64_t *vals = malloc(mask * sizeof(uint64_t));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        free(ents);
        free(names);
        return EFS_ERR_NOMEM;
    }
    for (uint64_t i = 0; i < count; i++) {
        uint64_t k = ents[i].ino;
        uint64_t h = (k * 0x9E3779B97F4A7C15ULL) & (mask - 1);
        while (keys[h] != 0)
            h = (h + 1) & (mask - 1);
        keys[h] = k;
        vals[h] = i;
    }

    find_index_free_locked();
    g_find_idx.ents = ents;
    g_find_idx.count = count;
    g_find_idx.names = names;
    g_find_idx.ino_keys = keys;
    g_find_idx.ino_vals = vals;
    g_find_idx.ino_mask = mask;
    return EFS_OK;
}

/* Look up an entry index by ino. Caller holds g_find_idx_mu. Returns -1 if
 * absent. */
static int64_t find_idx_lookup(efs_ino_t ino)
{
    if (g_find_idx.ino_mask == 0)
        return -1;
    uint64_t h = (ino * 0x9E3779B97F4A7C15ULL) & (g_find_idx.ino_mask - 1);
    while (g_find_idx.ino_keys[h] != 0) {
        if (g_find_idx.ino_keys[h] == ino)
            return (int64_t)g_find_idx.ino_vals[h];
        h = (h + 1) & (g_find_idx.ino_mask - 1);
    }
    return -1;
}

/* Build the filesystem-root-absolute path of `ino` ("/a/b/name"). Returns 0 if
 * ino lies under `dir_ino` (dir_ino is an ancestor, so the match is inside the
 * queried subtree), -1 otherwise. Caller holds g_find_idx_mu. */
static int find_fullpath(efs_ino_t dir_ino, efs_ino_t ino, char *out, size_t out_len)
{
    uint32_t comps[128]; /* name offsets, leaf-first; caps depth at 128 */
    int nc = 0;
    efs_ino_t cur = ino;
    int under = 0;
    while (nc < 128) {
        if (cur == dir_ino)
            under = 1;
        int64_t idx = find_idx_lookup(cur);
        if (idx < 0)
            break;
        efs_ino_t p = g_find_idx.ents[idx].parent;
        if (p == cur) /* root's parent is itself */
            break;
        comps[nc++] = g_find_idx.ents[idx].name_off;
        cur = p;
    }
    if (!under || nc == 0)
        return -1;
    size_t off = 0;
    out[off++] = '/';
    for (int i = nc - 1; i >= 0; i--) {
        const char *nm = g_find_idx.names + comps[i];
        size_t L = strlen(nm);
        if (off + L + 2 > out_len)
            return -1;
        memcpy(out + off, nm, L);
        off += L;
        if (i)
            out[off++] = '/';
    }
    out[off] = '\0';
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

/* Run a parsed query against a freshly rebuilt index; return the matching
 * host-absolute paths (mountpoint + fs-root path, newline-separated) in a
 * malloc'd buffer. */
static int find_run_query(efs_ino_t dir_ino, enum find_match type,
                          const char *term, size_t term_len,
                          char **out_text, int *out_len)
{
    pthread_mutex_lock(&g_find_idx_mu);
    if (find_index_build_locked() != EFS_OK) {
        pthread_mutex_unlock(&g_find_idx_mu);
        return EFS_ERR_NOMEM;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pthread_mutex_unlock(&g_find_idx_mu);
        return EFS_ERR_NOMEM;
    }
    /* Mountpoint prefix; a "/" mountpoint adds nothing (avoids "//"). */
    size_t mpl = strlen(g_mountpoint);
    size_t plen = (mpl > 1) ? mpl : 0;
    for (uint64_t i = 0; i < g_find_idx.count; i++) {
        const char *nm = g_find_idx.names + g_find_idx.ents[i].name_off;
        if (!find_name_match(nm, type, term, term_len))
            continue;
        efs_ino_t ino = g_find_idx.ents[i].ino;
        if (ino == dir_ino)
            continue;
        char full[EFS_MAX_PATH];
        if (find_fullpath(dir_ino, ino, full, sizeof(full)) != 0)
            continue;
        size_t L = strlen(full);
        if (len + plen + L + 2 > cap) {
            size_t ncap = cap * 2;
            while (ncap < len + plen + L + 2)
                ncap *= 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                pthread_mutex_unlock(&g_find_idx_mu);
                return EFS_ERR_NOMEM;
            }
            buf = nb;
            cap = ncap;
        }
        if (plen) {
            memcpy(buf + len, g_mountpoint, plen);
            len += plen;
        }
        memcpy(buf + len, full, L);
        len += L;
        buf[len++] = '\n';
    }
    pthread_mutex_unlock(&g_find_idx_mu);
    *out_text = buf;
    *out_len = (int)len;
    return 0;
}

/* Cached .find query results, keyed by (directory, raw pattern). A read of
 * ".find/<term>" runs the glob over the directory's subtree and caches the
 * rendered path list briefly so the getattr (size) and the read (content) that
 * make up a single `cat` share one index build. Queries are user-driven and
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
 * internal error. */
static int find_query_ensure(efs_ino_t dir_ino, const char *pattern)
{
    enum find_match type;
    char term[EFS_MAX_NAME];
    size_t term_len;
    if (find_parse_query(pattern, strlen(pattern), &type, term, &term_len) != 0)
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

    /* Miss: run the query (takes g_find_idx_mu internally) outside our lock. */
    char *text = NULL;
    int len = 0;
    if (find_run_query(dir_ino, type, term, term_len, &text, &len) != 0) {
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
static int find_query_len(efs_ino_t dir_ino, const char *pattern)
{
    if (find_query_ensure(dir_ino, pattern) != 0)
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
static int find_query_read(efs_ino_t dir_ino, const char *pattern,
                           char *buf, size_t size, off_t offset)
{
    if (find_query_ensure(dir_ino, pattern) != 0)
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
static int find_query_fill_stat(const struct efs_inode *parent, const char *pattern,
                                struct stat *stbuf)
{
    int len = find_query_len(parent->ino, pattern);
    if (len < 0)
        return -ENOENT;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_ino = find_query_ino(parent->ino, pattern);
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stat_set_size_blocks(stbuf, (uint64_t)len);
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

static int efs_fuse_getattr(const char *path, struct stat *stbuf,
                            struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0) {
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        return stats_fill_stat(&parent, stbuf);
    }
    char fqterm[EFS_MAX_NAME];
    struct efs_inode fqpar;
    if (path_find_query(path, &fqpar, fqterm, sizeof(fqterm)) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return find_query_fill_stat(&fqpar, fqterm, stbuf);
    }
    if (path_is_find(path, &parent) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return find_dir_fill_stat(&parent, stbuf);
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
                            off_t offset, struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;

    /* The virtual .find directory enumerates nothing — queries are read by
     * explicit ".find/<term>" path, not listed. */
    if (path_is_find(path, NULL) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        return 0;
    }

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

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (size_t i = 0; i < col.count; i++)
        filler(buf, col.ents[i].name, &col.ents[i].st, 0, 0);
    free(col.ents);
    return 0;
}

static int efs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0) {
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        return 0;
    }
    char fqterm[EFS_MAX_NAME];
    struct efs_inode fqpar;
    if (path_find_query(path, &fqpar, fqterm, sizeof(fqterm)) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES; /* query files are read-only */
        if (find_query_len(fqpar.ino, fqterm) < 0)
            return -ENOENT; /* invalid/short term */
        return 0;
    }
    if (path_is_find(path, &parent) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return -EISDIR; /* .find is a directory; query via .find/<term> */
    }
    struct efs_inode ino;
    return efs_client_lookup(path, &ino) == 0 ? 0 : -ENOENT;
}

static void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path);
static int efs_wb_sync(void);
static int efs_file_data_sync_ino(const char *path);
static int coal_read_sync(efs_ino_t ino);

static int efs_fuse_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi)
{
    (void)fi;
    struct efs_inode parent;
    if (path_is_stats(path, &parent) == 0) {
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
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
    char fqterm[EFS_MAX_NAME];
    struct efs_inode fqpar;
    if (path_find_query(path, &fqpar, fqterm, sizeof(fqterm)) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return find_query_read(fqpar.ino, fqterm, buf, size, offset);
    }
    if (path_is_find(path, &parent) == 0) {
        if (!feature_enabled(EFS_FEATURE_FIND))
            return -ENOENT;
        return -EISDIR; /* .find is a directory; query via .find/<term> */
    }

    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    /* Read-your-writes: commit any coalesced (not yet written-back) data for
     * this file before reading from the servers. Pure reads skip this via the
     * active-run counter, so the read path stays fast when nothing is dirty. */
    if (coal_read_sync(ino.ino) != 0) {
        efs_fuse_log_err("read-wbsync", EFS_ERR_IO, ino.ino, (uint64_t)offset,
                         size, path);
        return -EIO;
    }

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
    efs_ino_t err_ino; /* inode that set err; 0 = none / global */
    int ready;
    int shutdown;
    pthread_t workers[EFS_WB_WORKERS_MAX];
    efs_ino_t busy_ino[EFS_WB_WORKERS_MAX];
    int nworkers;
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
        g_wb.head = (g_wb.head + 1) % EFS_WB_DEPTH;
        g_wb.count--;
        g_wb.inflight++;
        g_wb.busy_ino[wid] = job.ino;
        pthread_cond_signal(&g_wb.not_full);
        pthread_mutex_unlock(&g_wb.mu);

        /* Serialize same-ino writebacks: partial-chunk RMW is not atomic, so
         * two overlapping WB jobs on one file would otherwise lose updates. */
        pthread_mutex_t *ilock = efs_wb_ino_lock(job.ino);
        pthread_mutex_lock(ilock);
        int rc = efs_client_write_no_replicate(job.ino, job.offset, job.size,
                                               job.buf);
        pthread_mutex_unlock(ilock);
        free(job.free_base ? job.free_base : job.buf);

        pthread_mutex_lock(&g_wb.mu);
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
        g_wb.busy_ino[wid] = 0;
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
 * (stolen FUSE receive buffer); otherwise `copy` is freed. */
static int efs_wb_enqueue_owned(efs_ino_t ino, uint64_t offset, size_t size,
                                char *copy, char *free_base)
{
    if (efs_wb_ensure() != 0) {
        free(free_base ? free_base : copy);
        return EFS_ERR_NOMEM;
    }
    pthread_mutex_lock(&g_wb.mu);
    /* Do not refuse new files because an earlier inode's PUT failed.
     * That sticky g_wb.err used to make every later write/close EIO
     * (including empty touch) after one quorum blip. */
    while (g_wb.count == EFS_WB_DEPTH && !g_wb.shutdown)
        pthread_cond_wait(&g_wb.not_full, &g_wb.mu);
    if (g_wb.shutdown) {
        pthread_mutex_unlock(&g_wb.mu);
        free(free_base ? free_base : copy);
        return EFS_ERR_IO;
    }
    g_wb.q[g_wb.tail].ino = ino;
    g_wb.q[g_wb.tail].offset = offset;
    g_wb.q[g_wb.tail].size = size;
    g_wb.q[g_wb.tail].buf = copy;
    g_wb.q[g_wb.tail].free_base = free_base;
    g_wb.tail = (g_wb.tail + 1) % EFS_WB_DEPTH;
    g_wb.count++;
    pthread_cond_signal(&g_wb.not_empty);
    pthread_mutex_unlock(&g_wb.mu);
    return EFS_OK;
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
    pthread_mutex_lock(&g_wb.mu);
    while (efs_wb_ino_pending_locked(ino))
        pthread_cond_wait(&g_wb.idle, &g_wb.mu);
    int err = EFS_OK;
    if (g_wb.err != EFS_OK && g_wb.err_ino == ino) {
        err = g_wb.err;
        g_wb.err = EFS_OK;
        g_wb.err_ino = 0;
    }
    pthread_mutex_unlock(&g_wb.mu);
    return err;
}

static int efs_file_data_sync(const char *path)
{
    struct efs_inode ino;
    if (path && efs_client_lookup(path, &ino) == 0 && !efs_mode_is_dir(ino.mode))
        return efs_file_data_sync_ino(path);
    return efs_wb_sync();
}

static int efs_file_data_sync_ino(const char *path)
{
    struct efs_inode ino;
    if (!path || efs_client_lookup(path, &ino) != 0 || efs_mode_is_dir(ino.mode))
        return EFS_OK;
    int rc = efs_wb_sync_ino(ino.ino);
    if (rc != EFS_OK)
        return rc;
    return efs_dcache_flush_ino(ino.ino);
}

/* ---- Write coalescing -----------------------------------------------------
 * Every FUSE write otherwise becomes its own writeback job -> its own chunk
 * read-modify-write + PUT, so N small writes to one chunk cost N full-chunk
 * PUTs (~192x amplification at bs=1024). Small sub-chunk writes are instead
 * accumulated into a per-ino contiguous run and flushed as one large write
 * (full chunks -> no RMW). Large (>= chunk) writes bypass straight to the WB
 * queue. Durability is unchanged: fsync/flush/release flush the run and then
 * drain the WB queue. Read-your-writes: a read of a file with an open run
 * flushes + drains it first; pure reads skip via the active-run counter. */
#define EFS_COALESCE_CAP      (1u << 20) /* 1 MiB per run */
#define EFS_COALESCE_MAX_RUNS 64
#define EFS_COALESCE_SLOTS    128

struct coal_run {
    efs_ino_t ino;
    uint64_t start, end;
    size_t cap;
    char *buf;
    struct coal_run *next;
};

static struct {
    pthread_mutex_t mu;
    struct coal_run *slots[EFS_COALESCE_SLOTS];
    int active;
} g_coal = { .mu = PTHREAD_MUTEX_INITIALIZER };

static int g_coalesce_enabled = 1;

static struct coal_run *coal_find_locked(efs_ino_t ino)
{
    unsigned h = (unsigned)((uint64_t)ino % EFS_COALESCE_SLOTS);
    struct coal_run *r = g_coal.slots[h];
    while (r && r->ino != ino)
        r = r->next;
    return r;
}

/* caller holds g_coal.mu */
static void coal_detach_locked(struct coal_run *r)
{
    unsigned h = (unsigned)((uint64_t)r->ino % EFS_COALESCE_SLOTS);
    struct coal_run **pp = &g_coal.slots[h];
    while (*pp && *pp != r)
        pp = &(*pp)->next;
    if (*pp)
        *pp = r->next;
    g_coal.active--;
}

/* Hand a detached run's buffer to the WB pool (takes over r->buf). */
static int coal_submit(struct coal_run *r)
{
    int rc = efs_wb_enqueue_owned(r->ino, r->start,
                                  (size_t)(r->end - r->start), r->buf, NULL);
    free(r);
    return rc;
}

/* Flush any buffered run for ino to the WB pool. 1 if flushed, 0 if none. */
static int coal_flush_ino(efs_ino_t ino)
{
    pthread_mutex_lock(&g_coal.mu);
    struct coal_run *r = coal_find_locked(ino);
    if (r)
        coal_detach_locked(r);
    pthread_mutex_unlock(&g_coal.mu);
    if (!r)
        return 0;
    return coal_submit(r) == EFS_OK ? 1 : -1;
}

static int coal_has_ino(efs_ino_t ino)
{
    if (!g_coalesce_enabled ||
        __atomic_load_n(&g_coal.active, __ATOMIC_RELAXED) == 0)
        return 0;
    pthread_mutex_lock(&g_coal.mu);
    int has = coal_find_locked(ino) != NULL;
    pthread_mutex_unlock(&g_coal.mu);
    return has;
}

static void coal_discard_ino(efs_ino_t ino)
{
    pthread_mutex_lock(&g_coal.mu);
    struct coal_run *r = coal_find_locked(ino);
    if (r)
        coal_detach_locked(r);
    pthread_mutex_unlock(&g_coal.mu);
    if (r) {
        free(r->buf);
        free(r);
    }
}

/* Read path helper: if ino has a buffered coalesced run, flush it and drain
 * the WB pool so the read observes it. Returns 0, or -EIO on a WB error. */
static int coal_read_sync(efs_ino_t ino)
{
    if (!g_coalesce_enabled ||
        __atomic_load_n(&g_coal.active, __ATOMIC_RELAXED) == 0)
        return 0;
    if (coal_flush_ino(ino) != 1)
        return 0;
    return efs_wb_sync() == EFS_OK ? 0 : -1;
}

/* Coalesce a small write into the ino's run; consumes copy on EFS_OK.
 * Falls back to a direct WB enqueue when coalescing is not possible. */
static int coal_write(efs_ino_t ino, uint64_t offset, size_t size, char *copy)
{
    struct coal_run *flush = NULL;
    int have_copy = 1;

    pthread_mutex_lock(&g_coal.mu);
    struct coal_run *r = coal_find_locked(ino);
    if (r && offset == r->end &&
        (uint64_t)(r->end - r->start) + size <= EFS_COALESCE_CAP) {
        size_t need = (size_t)(r->end - r->start) + size;
        if (need > r->cap) {
            size_t ncap = r->cap ? r->cap : (64u << 10);
            while (ncap < need)
                ncap *= 2;
            char *nb = realloc(r->buf, ncap);
            if (nb) {
                r->buf = nb;
                r->cap = ncap;
            } else {
                coal_detach_locked(r); /* cannot grow: flush, start fresh */
                flush = r;
                r = NULL;
            }
        }
        if (r) {
            memcpy(r->buf + (r->end - r->start), copy, size);
            r->end += size;
            pthread_mutex_unlock(&g_coal.mu);
            free(copy);
            return EFS_OK;
        }
    } else if (r) {
        /* non-contiguous or would exceed the cap: flush it, start new */
        coal_detach_locked(r);
        flush = r;
        r = NULL;
    }

    if (g_coal.active < EFS_COALESCE_MAX_RUNS) {
        struct coal_run *nr = malloc(sizeof(*nr));
        size_t icap = 64u << 10;
        if (icap < size)
            icap = size;
        if (icap > EFS_COALESCE_CAP)
            icap = EFS_COALESCE_CAP;
        char *nb = nr ? malloc(icap) : NULL;
        if (nr && nb) {
            memcpy(nb, copy, size);
            nr->ino = ino;
            nr->start = offset;
            nr->end = offset + size;
            nr->cap = icap;
            nr->buf = nb;
            unsigned h = (unsigned)((uint64_t)ino % EFS_COALESCE_SLOTS);
            nr->next = g_coal.slots[h];
            g_coal.slots[h] = nr;
            g_coal.active++;
            have_copy = 0;
            free(copy);
        } else {
            free(nr);
        }
    }
    pthread_mutex_unlock(&g_coal.mu);

    if (flush)
        coal_submit(flush); /* a WB error here surfaces on the next sync */

    if (have_copy) /* could not coalesce: write through directly */
        return efs_wb_enqueue_owned(ino, offset, size, copy, NULL);
    return EFS_OK;
}

/* Flush the buffered run for the file at path (best-effort; used before
 * fsync/flush/release so the file's data reaches the WB pool). */
static void coal_flush_path(const char *path)
{
    if (!g_coalesce_enabled ||
        __atomic_load_n(&g_coal.active, __ATOMIC_RELAXED) == 0)
        return;
    struct efs_inode ino;
    if (efs_client_lookup(path, &ino) == 0 && !efs_mode_is_dir(ino.mode))
        (void)coal_flush_ino(ino.ino);
}

static void coal_flush_all(void)
{
    for (;;) {
        pthread_mutex_lock(&g_coal.mu);
        struct coal_run *r = NULL;
        for (int i = 0; i < EFS_COALESCE_SLOTS; i++) {
            if (g_coal.slots[i]) {
                r = g_coal.slots[i];
                coal_detach_locked(r);
                break;
            }
        }
        pthread_mutex_unlock(&g_coal.mu);
        if (!r)
            break;
        coal_submit(r);
    }
}

static int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    if (path_is_find(path, NULL) == 0)
        return -EISDIR; /* .find is a virtual directory */
    if (path_is_find_query_path(path))
        return -EACCES; /* .find/<term> query files are read-only */
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    if (size == 0)
        return 0;
    char *copy = malloc(size);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, buf, size);
    if (g_coalesce_enabled && size < fuse_chunk_size())
        rc = coal_write(ino.ino, (uint64_t)offset, size, copy);
    else
        rc = efs_wb_enqueue_owned(ino.ino, (uint64_t)offset, size, copy, NULL);
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

/* write_buf: copy the FUSE receive buffer. Small writes still try an
 * in-place dcache patch; large writes go to the writeback pool. */
static int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    if (path_is_find(path, NULL) == 0)
        return -EISDIR; /* .find is a virtual directory */
    if (path_is_find_query_path(path))
        return -EACCES; /* .find/<term> query files are read-only */
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

    if (size <= 4096 && !coal_has_ino(ino.ino) &&
        efs_dcache_try_patch(ino.ino, (uint64_t)offset, (uint32_t)size,
                             (const uint8_t *)copy) == 0) {
        free(copy);
        return (int)size;
    }

    if (g_coalesce_enabled && size < fuse_chunk_size())
        rc = coal_write(ino.ino, (uint64_t)offset, size, copy);
    else
        rc = efs_wb_enqueue_owned(ino.ino, (uint64_t)offset, size, copy, NULL);
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

static int efs_fuse_fsync(const char *path, int isdatasync,
                          struct fuse_file_info *fi)
{
    (void)isdatasync;
    (void)fi;
    coal_flush_path(path);
    int rc = efs_file_data_sync(path);
    /* Directory rollups walk the inode table; they are not required for
     * file-data durability. Leave them for unmount / .stats. */
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("fsync", rc, 0, 0, 0, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("fsync", rc, 0, 0, 0, path);
        return -EIO;
    }
    {
        struct efs_inode ino;
        if (efs_client_lookup(path, &ino) == 0 && !efs_mode_is_dir(ino.mode))
            (void)efs_client_pack_seal(ino.ino);
    }
    /* Packed small files may still sit in a dir-pack tail; flush those
     * fragments so fsync data is on the servers. */
    efs_client_pack_flush_all();
    /* Incremental publish of dirty inode/chunk rows. force=1 used to
     * memcpy the whole table (~2M inodes) on every fsync and inverted
     * the 8×1G + end_fsync job versus plain 1M write. */
    rc = efs_client_sync_meta();
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("fsync-meta", rc, 0, 0, 0, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("fsync-meta", rc, 0, 0, 0, path);
        return -EIO;
    }
    return 0;
}

static int efs_fuse_flush(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    /* Close: drain this file only. A global WB wait serialized every ecopy
     * close behind every other in-flight write. */
    coal_flush_path(path);
    int rc = efs_file_data_sync_ino(path);
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

static int fuse_create_errno(efs_ino_t parent, const char *name)
{
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    int found = efs_export_lookup(&g_client.export, parent, name, NULL);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);
    return found == EFS_OK ? -EEXIST : -EIO;
}

static int efs_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (name_is_reserved(name))
        return -EEXIST;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFREG | mode,
                                      ctx->uid, ctx->gid);
    if (ino == 0) {
        int e = fuse_create_errno(parent.ino, name);
        if (e != -EEXIST)
            fprintf(stderr, "create %s failed (%s)\n", path,
                    e == -EIO ? "EIO" : "err");
        return e;
    }
    return 0;
}

static int efs_fuse_mkdir(const char *path, mode_t mode)
{
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (name_is_reserved(name))
        return -EEXIST;

    struct fuse_context *ctx = fuse_get_context();
    efs_ino_t ino = efs_client_create(parent.ino, name, S_IFDIR | mode,
                                      ctx->uid, ctx->gid);
    if (ino == 0)
        return fuse_create_errno(parent.ino, name);
    return 0;
}

static int efs_fuse_unlink(const char *path)
{
    char name[EFS_MAX_NAME];
    struct efs_inode parent;
    int rc = split_parent_name(path, name, sizeof(name), &parent);
    if (rc != 0)
        return rc;
    if (name_is_reserved(name))
        return -EACCES;

    /* Drop any buffered coalesced data for the file being removed. */
    if (g_coalesce_enabled &&
        __atomic_load_n(&g_coal.active, __ATOMIC_RELAXED) > 0) {
        struct efs_inode victim;
        if (efs_client_lookup(path, &victim) == 0)
            coal_discard_ino(victim.ino);
    }

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

static int efs_fuse_chmod(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0 || path_is_find(path, NULL) == 0 ||
        path_is_find_query_path(path))
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

static int efs_fuse_chown(const char *path, uid_t uid, gid_t gid,
                          struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0 || path_is_find(path, NULL) == 0 ||
        path_is_find_query_path(path))
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
        return fuse_create_errno(parent.ino, name);

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
    if (path_is_stats(from, NULL) == 0 || path_is_stats(to, NULL) == 0 ||
        path_is_find(from, NULL) == 0 || path_is_find(to, NULL) == 0 ||
        path_is_find_query_path(from) || path_is_find_query_path(to))
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
    if (name_is_reserved(name))
        return -EACCES;

    rc = efs_client_link(src.ino, parent.ino, name);
    if (rc == EFS_ERR_EXIST)
        return -EEXIST;
    if (rc != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_utimens(const char *path, const struct timespec tv[2],
                            struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0 || path_is_find(path, NULL) == 0 ||
        path_is_find_query_path(path))
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

static int efs_fuse_truncate(const char *path, off_t size,
                             struct fuse_file_info *fi)
{
    (void)fi;
    if (path_is_stats(path, NULL) == 0)
        return -EACCES;
    if (path_is_find(path, NULL) == 0)
        return -EISDIR; /* .find is a virtual directory */
    if (path_is_find_query_path(path))
        return -EACCES; /* query files are read-only */
    struct efs_inode ino;
    int rc = efs_client_lookup(path, &ino);
    if (rc != 0)
        return -ENOENT;
    if (efs_mode_is_dir(ino.mode))
        return -EISDIR;

    /* Commit any coalesced data before resizing so the truncate sees a stable
     * on-server length and drops/keeps whole chunks deterministically. */
    if (g_coalesce_enabled && coal_flush_ino(ino.ino) == 1)
        (void)efs_wb_sync();
    (void)efs_dcache_flush_ino(ino.ino);

    if (efs_client_truncate(ino.ino, (uint64_t)size) != 0)
        return -EIO;
    return 0;
}

static int efs_fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    coal_flush_path(path);
    int rc = efs_file_data_sync_ino(path);
    if (rc == EFS_ERR_QUOTA) {
        efs_fuse_log_err("release", rc, 0, 0, 0, path);
        return -ENOSPC;
    }
    if (rc != 0) {
        efs_fuse_log_err("release", rc, 0, 0, 0, path);
        return -EIO;
    }
    {
        struct efs_inode ino;
        if (efs_client_lookup(path, &ino) == 0 && !efs_mode_is_dir(ino.mode))
            (void)efs_client_pack_seal(ino.ino);
    }
    /* Coalesced: only flushes every meta_batch_ops releases/creates. */
    efs_client_note_meta_change(0);
    return 0;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
    coal_flush_all();
    (void)efs_wb_sync();
    (void)efs_dcache_flush_all();
    efs_client_pack_flush_all();
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
    /* Final flush so the last dirty batch is not lost on unmount. */
    efs_client_note_meta_change(1);
    /* Free the .find index and cached query results. */
    pthread_mutex_lock(&g_find_idx_mu);
    find_index_free_locked();
    pthread_mutex_unlock(&g_find_idx_mu);
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        free(g_find_res[i].text);
        g_find_res[i].text = NULL;
        g_find_res[i].valid = 0;
    }
    pthread_mutex_unlock(&g_find_res_mu);
    efs_client_shutdown();
}

static int efs_fuse_rename(const char *from, const char *to, unsigned int flags)
{
    (void)flags;
    if (path_is_stats(from, NULL) == 0 || path_is_stats(to, NULL) == 0 ||
        path_is_find(from, NULL) == 0 || path_is_find(to, NULL) == 0 ||
        path_is_find_query_path(from) || path_is_find_query_path(to))
        return -EACCES;
    struct efs_inode src;
    if (efs_client_lookup(from, &src) != 0)
        return -ENOENT;

    char name[EFS_MAX_NAME];
    struct efs_inode dst_parent;
    int rc = split_parent_name(to, name, sizeof(name), &dst_parent);
    if (rc != 0)
        return rc;
    if (name_is_reserved(name))
        return -EACCES;

    if (efs_client_rename(src.ino, dst_parent.ino, name) != 0)
        return -EIO;
    return 0;
}

static void *efs_fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    if (cfg) {
        cfg->use_ino = 1;
        cfg->attr_timeout = 1.0;
        cfg->entry_timeout = 1.0;
        cfg->ac_attr_timeout = 1.0;
        cfg->ac_attr_timeout_set = 1;
    }
    /* fuse3 always allows large writes; still cap max_write to the pipeline. */
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
        /* 8+ fio jobs × pipelined chunk GETs need more than the
         * libfuse default (12) outstanding FUSE requests. */
        if (conn->max_background < 128)
            conn->max_background = 128;
        /* fuse3 actually honors writeback_cache (2.9 rejected the mount
         * option). Kernel then writebacks unaligned windows; RMW reads of
         * not-yet-PUT chunks return EIO and close fails after a fast dd.
         * Keep writes syscall-shaped until RMW can see in-flight WB. */
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
    /* Write coalescing is on by default; EFS_WRITE_COALESCE=0 disables it. */
    const char *ce = getenv("EFS_WRITE_COALESCE");
    if (ce && *ce && atoi(ce) == 0)
        g_coalesce_enabled = 0;
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
    .utimens  = efs_fuse_utimens,
    .truncate = efs_fuse_truncate,
    .rename   = efs_fuse_rename,
    .symlink  = efs_fuse_symlink,
    .readlink = efs_fuse_readlink,
    .link     = efs_fuse_link,
    .release  = efs_fuse_release,
    .init     = efs_fuse_init,
    .destroy  = efs_fuse_destroy,
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
    {
        struct efs_inode root;
        int rrc = efs_export_get_inode(&g_client.export, EFS_ROOT_INO, &root);
        uint64_t z = 0, ones = 0;
        for (uint64_t i = 0; i < g_client.export.inode_count; i++) {
            efs_ino_t n = g_client.export.inodes[i].ino;
            if (n == 0)
                z++;
            else if (n == EFS_ROOT_INO)
                ones++;
        }
        printf("meta ready gen=%llu ver=%u inodes=%llu chunks=%llu "
               "root_get=%d mode=%o ino0=%llu ino1=%llu\n",
               (unsigned long long)g_client.export.root.generation,
               g_client.export.root.version,
               (unsigned long long)g_client.export.inode_count,
               (unsigned long long)g_client.export.chunk_count, rrc,
               rrc == 0 ? root.mode : 0,
               (unsigned long long)z, (unsigned long long)ones);
    }
    fflush(stdout);

    char *fuse_argv[64];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    /* High-level -o flags that 3.3–3.10 accept. max_write / max_readahead /
     * use_ino / writeback_cache are set in efs_fuse_init (cfg / conn);
     * passing them here is rejected as unknown on fuse3 3.10. */
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] =
        "attr_timeout=1,entry_timeout=1,ac_attr_timeout=1";
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
