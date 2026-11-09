/* efs-fuse: crash handlers, stats, features, file handles, virtual inodes, query results, invalidation, find and access checks. */
#include "fuse_internal.h"
#include "support.h"
#include "lookup.h"

/* Async-signal-safe crash breadcrumb. Uses an alternate signal stack so a
 * stack-overflow SIGSEGV can still report instead of dying silently. */
static char g_efs_fuse_altstack[256 * 1024];

/* Crash diagnostics are best effort: retry interruption, but a short/error
 * stderr write cannot be recovered here. Preserve the interrupted errno. */
static void efs_fuse_diagnostic_write(const void *data, size_t len)
{
    int saved_errno = errno;
    ssize_t written;
    do { written = write(STDERR_FILENO, data, len); }
    while (written < 0 && errno == EINTR);
    errno = saved_errno;
}

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
        efs_fuse_diagnostic_write(buf, (size_t)n);
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
        efs_fuse_diagnostic_write(hdr, (size_t)n);
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
        efs_fuse_diagnostic_write(hdr, (size_t)n);
    for (int i = 0; i < EFS_DIR_LOCKS; i++) {
        if (!g_client.dir_lock[i].__data.__lock &&
            !g_client.dir_lock[i].__data.__owner)
            continue;
        n = snprintf(hdr, sizeof(hdr),
                     "STACKDUMP dir_lock[%d] owner=%d lock=%d\n", i,
                     g_client.dir_lock[i].__data.__owner,
                     g_client.dir_lock[i].__data.__lock);
        if (n > 0)
            efs_fuse_diagnostic_write(hdr, (size_t)n);
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
    efs_fuse_diagnostic_write("STACKDUMP end\n", 14);
}

void efs_fuse_install_crash_handlers(void)
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
uint32_t fuse_chunk_size(void)
{
    return efs_client_chunk_size();
}

void stat_set_size_blocks(struct stat *stbuf, uint64_t size,
                                 uint64_t allocated)
{
    stbuf->st_size = (off_t)size;
    stbuf->st_blksize = fuse_chunk_size();
    stbuf->st_blocks = (blkcnt_t)((allocated + 511) / 512);
}

/* Virtual stats bytes come from a published server aggregate. Every new
 * snapshot validates credentials; open descriptors retain immutable bytes. */


uint64_t stats_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int efs_rc_to_errno(int rc);
/* Server-owned aggregate; the client never rebuilds rollups on this path. */
static int stats_compute(struct stats_ent *e,efs_ino_t dir_ino)
{
    struct efs_msg_query_snapshot req;
    memset(&req,0,sizeof(req));
    req.version=EFS_QUERY_VERSION;req.operation=EFS_QUERY_STATS;req.root=dir_ino;
    req.max_output=sizeof(e->text);req.visit_budget=EFS_QUERY_MAX_VISITS;
    int rc=query_credentials(&req);
    if (rc) return efs_rc_to_errno(rc);
    struct efs_msg_query_snapshot_reply reply;char *text=NULL;
    rc=efs_client_rpc_query_snapshot(&req,&reply,&text);
    if (rc) return efs_rc_to_errno(rc);
    if (reply.text_len>=sizeof(e->text)) { free(text);return -EOVERFLOW; }
    memcpy(e->text,text,reply.text_len+1);free(text);
    e->len=(int)reply.text_len;e->uid=reply.uid;e->gid=reply.gid;
    e->mtime=reply.mtime;e->atime=reply.atime;e->ctime=reply.ctime;
    e->dir_ino=dir_ino;e->computed_ms=stats_now_ms();e->valid=1;
    return 0;
}

int stats_snapshot(efs_ino_t dir_ino,struct stats_ent *out)
{
    memset(out,0,sizeof(*out));
    return stats_compute(out,dir_ino);
}

int stats_fill_stat(const struct efs_inode *parent, struct stat *stbuf)
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

#define FEATURES_TTL_MS 2000u

/* Query each seed node for the export's current feature mask. Returns 1 and
 * sets *out on a definitive reply, 0 if no node answered. */
static int features_poll_wire(uint32_t *out)
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

/* Feature discovery is advisory; the query RPC still checks durable policy.
 * A slow seed must not spend a socket's 30-second timeout just to learn an
 * optional capability. Preserve the caller's tighter budget and restore it. */
static int features_poll(uint32_t *out)
{
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t end=stats_now_ms()+1000;
    if (previous && previous<end) end=previous;
    efs_client_rpc_set_deadline_ms(end);
    int ok=features_poll_wire(out);
    efs_client_rpc_set_deadline_ms(previous);
    return ok;
}

/* Current feature mask; refreshed over the wire at most once per TTL. */
static uint32_t features_current(void)
{
    uint64_t now = stats_now_ms();
    pthread_mutex_lock(&g_feat_mu);
    if (g_feat_cache_valid && now - g_feat_cache_ms < FEATURES_TTL_MS) {
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

int feature_enabled(uint32_t bit)
{
    return (features_current() & bit) != 0;
}

/* --- Virtual per-directory .find (not a real inode) ---
 * .find is a virtual directory; reading "<dir>/.find/<term>" runs a query
 * (a command-path name, include/efs/command_path.h) over <dir>'s subtree and
 * returns the matching paths (one per line,
 * host-absolute: mountpoint + fs-root path, so they pipe/loop from any cwd).
 * Single-command, e.g. cat ".find/PATTERN". The server searches a published
 * name index and supplies canonical export-relative paths. */
static ino_t find_synthetic_ino(efs_ino_t parent_ino)
{
    return (ino_t)((1ULL << 61) | (parent_ino & ((1ULL << 60) - 1)));
}

/* Low-level FUSE: nodeid == efs ino (FUSE_ROOT_ID == EFS_ROOT_INO == 1).
 * Virtual .stats / .find names sit in unused high bits so they never collide
 * with a real ino. Each handler stashes `req` in t_req so permission helpers
 * can read uid/gid without a path-based fuse_get_context(). */
__thread fuse_req_t t_req;
struct fuse_session *g_fuse_se;

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

/* Files carry an open-description error cursor. Directories retain their
 * separate dirh representation. Untagged fh remains supported for virtual
 * files and internal calls that have no kernel open description. */
#define EFS_FILE_FH_TAG (1ULL << 63)
#define EFS_VIRTUAL_OPEN_BYTES (64u << 20)
static pthread_mutex_t g_virtual_open_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t g_virtual_open_bytes;
struct efs_file_handle *efs_file_handle(const struct fuse_file_info *fi)
{
    return fi && (fi->fh & EFS_FILE_FH_TAG) ?
        (struct efs_file_handle *)(uintptr_t)(fi->fh & ~EFS_FILE_FH_TAG) : NULL;
}
efs_ino_t efs_file_ino(const struct fuse_file_info *fi, efs_ino_t fallback)
{
    struct efs_file_handle *h = efs_file_handle(fi);
    return h ? h->ino : fi && fi->fh ? (efs_ino_t)fi->fh : fallback;
}
int efs_file_open(struct fuse_file_info *fi, efs_ino_t ino)
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
static void vq_open_size(efs_ino_t ino, size_t len, int delta);
static void vq_closed(efs_ino_t ino);
void efs_file_close(struct fuse_file_info *fi)
{
    struct efs_file_handle *h = efs_file_handle(fi);
    if (h) {
        if (h->errors) efs_wb_description_close(h->errors);
        if (h->snapshot) {
            vq_open_size(h->ino, h->snapshot_len, -1);
            vq_closed(h->ino);
            pthread_mutex_lock(&g_virtual_open_mu);
            g_virtual_open_bytes -= h->snapshot_len;
            pthread_mutex_unlock(&g_virtual_open_mu);
            free(h->snapshot);
        }
        free(h);
        fi->fh = 0;
    }
}
/* Virtual opens use the same tagged description, without writeback/leases. */
int virtual_file_open(struct fuse_file_info *fi, efs_ino_t ino,
                             const char *text, size_t len)
{
    if (!fi) return -EINVAL;
    struct efs_file_handle *h = calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    pthread_mutex_lock(&g_virtual_open_mu);
    if (len > EFS_VIRTUAL_OPEN_BYTES - g_virtual_open_bytes) {
        pthread_mutex_unlock(&g_virtual_open_mu);
        free(h);
        return -ENOMEM;
    }
    g_virtual_open_bytes += len;
    pthread_mutex_unlock(&g_virtual_open_mu);
    h->snapshot = malloc(len ? len : 1);
    if (!h->snapshot) {
        pthread_mutex_lock(&g_virtual_open_mu);
        g_virtual_open_bytes -= len;
        pthread_mutex_unlock(&g_virtual_open_mu);
        free(h);
        return -ENOMEM;
    }
    if (len) memcpy(h->snapshot, text, len);
    h->snapshot_len = len;
    h->ino = ino;
    vq_open_size(ino, len, 1);
    fi->fh = EFS_FILE_FH_TAG | (uint64_t)(uintptr_t)h;
    return 0;
}
int efs_file_sync_error(struct fuse_file_info *fi, efs_ino_t ino,
                                int was_stalled, int rc)
{
    struct efs_file_handle *h = efs_file_handle(fi);
    int remains = efs_wb_inode_stalled(ino);
    int error = h && efs_wb_description_sync(h->errors,
                        rc == EFS_OK && was_stalled && !remains);
    return error || remains ? EFS_ERR_IO : rc;
}

int virt_kind(fuse_ino_t ino)
{
    if ((ino & (EFS_VIRT_STATS | EFS_VIRT_FIND)) == (EFS_VIRT_STATS | EFS_VIRT_FIND))
        return 4; /* .efs command directory */
    if (ino & EFS_VIRT_STATS)
        return 1;
    if (ino & EFS_VIRT_FIND)
        return 2;
    if (ino & EFS_VIRT_QUERY)
        return 3;
    return 0;
}

efs_ino_t virt_parent(fuse_ino_t ino)
{
    return (efs_ino_t)(ino & EFS_VIRT_PARENT_MASK);
}

fuse_ino_t virt_find_ino(efs_ino_t parent)
{
    return (fuse_ino_t)(EFS_VIRT_FIND | (parent & EFS_VIRT_PARENT_MASK));
}

fuse_ino_t virt_commands_ino(efs_ino_t parent)
{
    return EFS_VIRT_STATS | EFS_VIRT_FIND | (parent & EFS_VIRT_PARENT_MASK);
}
/* .efs follows the cluster's versioned commands-policy as the server
 * enforces it (D42); there is no client-side switch. */
int commands_enabled(void)
{
    return (features_current() & EFS_FEATURES_DEFAULT)!=0;
}
int command_script(const char *term)
{
    return !strcmp(term,"@help") || !strcmp(term,"@ls") ||
           !strcmp(term,"@find") || !strcmp(term,"@stats");
}

#define EFS_VQ_N 64
static struct {
    int valid;
    uint32_t nlookup;
    uint32_t nreserve; /* intern -> lookup reply, including failed getattr */
    uint32_t nopen;
    size_t open_size;
    uint64_t version;
    fuse_ino_t nodeid;
    efs_ino_t dir_ino;
    char term[EFS_MAX_NAME];
} g_vq[EFS_VQ_N];
static pthread_mutex_t g_vq_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_vq_seq = 1;

uint64_t virtual_version(const char *text, size_t len)
{
    uint64_t hash=1469598103934665603ULL;
    for (size_t i=0;i<len;i++) { hash^=(unsigned char)text[i];hash*=1099511628211ULL; }
    return hash;
}
fuse_ino_t vq_intern(efs_ino_t dir_ino, const char *term,
                            uint64_t version, size_t len)
{
    fuse_ino_t id = 0;
    int slot = -1, empty = -1, stale = -1;

    pthread_mutex_lock(&g_vq_mu);
    for (int i = 0; i < EFS_VQ_N; i++) {
        if (g_vq[i].valid && g_vq[i].version == version &&
            g_vq[i].open_size == len && g_vq[i].dir_ino == dir_ino &&
            strcmp(g_vq[i].term, term) == 0) {
            if(g_vq[i].nreserve==UINT32_MAX) { pthread_mutex_unlock(&g_vq_mu);return 0; }
            g_vq[i].nreserve++;
            id = g_vq[i].nodeid;
            pthread_mutex_unlock(&g_vq_mu);
            return id;
        }
        if (!g_vq[i].valid && empty < 0)
            empty = i;
        else if (g_vq[i].valid && g_vq[i].nlookup == 0 && !g_vq[i].nopen && !g_vq[i].nreserve)
            stale = i;
    }
    slot = empty >= 0 ? empty : stale;
    if (slot < 0) { pthread_mutex_unlock(&g_vq_mu); return 0; }
    id = (fuse_ino_t)(EFS_VIRT_QUERY | (g_vq_seq++ & EFS_VIRT_PARENT_MASK));
    if (!(id & EFS_VIRT_QUERY))
        id = EFS_VIRT_QUERY | 1;
    memset(&g_vq[slot], 0, sizeof(g_vq[slot]));
    g_vq[slot].valid = 1;
    g_vq[slot].nreserve = 1;
    g_vq[slot].nodeid = id;
    g_vq[slot].dir_ino = dir_ino;
    g_vq[slot].version = version;
    g_vq[slot].open_size = len;
    strncpy(g_vq[slot].term, term, EFS_MAX_NAME - 1);
    pthread_mutex_unlock(&g_vq_mu);
    return id;
}

/* Release only the temporary intern reservation. Successful lookup_fill
 * already owns a kernel lookup reference; failed getattr must release too. */
void vq_unreserve(fuse_ino_t nodeid)
{
    pthread_mutex_lock(&g_vq_mu);
    for(int i=0;i<EFS_VQ_N;i++) if(g_vq[i].valid && g_vq[i].nodeid==nodeid) {
        if(g_vq[i].nreserve)g_vq[i].nreserve--;
        break;
    }
    pthread_mutex_unlock(&g_vq_mu);
}

int vq_lookup(fuse_ino_t nodeid, efs_ino_t *dir_out, char *term_out,
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

void vq_nlookup_add(fuse_ino_t nodeid, uint32_t n)
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

void vq_nlookup_sub(fuse_ino_t nodeid, uint64_t n)
{
    pthread_mutex_lock(&g_vq_mu);
    for (int i = 0; i < EFS_VQ_N; i++) {
        if (!g_vq[i].valid || g_vq[i].nodeid != nodeid)
            continue;
        if (n >= g_vq[i].nlookup) {
            /* NFS may decode the just-returned file handle after dentry
             * invalidation delivered FORGET. Keep the bounded identity until
             * an unreferenced slot is reused; open/looked-up slots stay pinned. */
            g_vq[i].nlookup = 0;
        } else {
            g_vq[i].nlookup -= (uint32_t)n;
        }
        break;
    }
    pthread_mutex_unlock(&g_vq_mu);
}

/* Pin the query inode's advertised size while its immutable fd is open.
 * Linux may issue GETATTR without a file handle even for fstat. A subsequent
 * path lookup shares an inode only when its result bytes are unchanged. */
static void vq_open_size(efs_ino_t ino, size_t len, int delta)
{
    pthread_mutex_lock(&g_vq_mu);
    for (int i=0; i<EFS_VQ_N; i++) if (g_vq[i].valid && g_vq[i].nodeid == ino) {
        if (delta > 0) { g_vq[i].nopen++; g_vq[i].open_size=len; }
        else if (g_vq[i].nopen) {
            g_vq[i].nopen--;
        }
        break;
    }
    pthread_mutex_unlock(&g_vq_mu);
}
int vq_pinned_size(efs_ino_t ino, size_t *len, int open_only)
{
    int found=0;
    pthread_mutex_lock(&g_vq_mu);
    for (int i=0; i<EFS_VQ_N; i++) if (g_vq[i].valid &&
        g_vq[i].nodeid==ino && (!open_only || g_vq[i].nopen)) {
        *len=g_vq[i].open_size; found=1; break;
    }
    pthread_mutex_unlock(&g_vq_mu);
    return found;
}

int vq_version_matches(efs_ino_t ino, const char *text, size_t len)
{
    uint64_t version=virtual_version(text,len);
    int match=0;
    pthread_mutex_lock(&g_vq_mu);
    for (int i=0;i<EFS_VQ_N;i++) if (g_vq[i].valid && g_vq[i].nodeid==ino) {
        match=g_vq[i].version==version && g_vq[i].open_size==len; break;
    }
    pthread_mutex_unlock(&g_vq_mu);
    return match;
}

const struct fuse_ctx *ll_ctx(void)
{
    return t_req ? fuse_req_ctx(t_req) : NULL;
}

/* Linux/POSIX group inheritance belongs to the parent directory, not the
 * creator's primary group. Only new directories inherit the setgid bit. */
void efs_create_credentials(const struct efs_inode *parent, mode_t *mode,
                                   uid_t *uid, gid_t *gid)
{
    const struct fuse_ctx *ctx = ll_ctx();
    *uid = ctx ? ctx->uid : 0;
    *gid = ctx ? ctx->gid : 0;
    if (parent->mode & S_ISGID) {
        *gid = parent->gid;
        if (S_ISDIR(*mode)) *mode |= S_ISGID;
    }
}

int query_credentials(struct efs_msg_query_snapshot *q)
{
    const struct fuse_ctx *ctx=ll_ctx();
    q->uid=ctx ? ctx->uid : 0;q->gid=ctx ? ctx->gid : 0;
    if (!t_req) return 0;
    gid_t groups[EFS_QUERY_GROUPS];
    int count=fuse_req_getgroups(t_req,EFS_QUERY_GROUPS,groups);
    if (count==-ERANGE || count>EFS_QUERY_GROUPS) return EFS_ERR_OVERFLOW;
    if (count<0) return EFS_ERR_IO;
    q->ngroups=(uint32_t)count;
    for (int i=0;i<count;i++) q->groups[i]=(uint32_t)groups[i];
    return 0;
}

static void ll_inval_push(struct ll_inval_item it)
{
    if (!g_fuse_se || (virt_kind(it.parent) &&
        !(it.kind == LL_INVAL_ENTRY && (virt_kind(it.parent) == 2 || virt_kind(it.parent) == 4))) ||
        virt_kind(it.ino))
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

void ll_inval_entry(fuse_ino_t parent, const char *name)
    __attribute__((unused));
void ll_inval_entry(fuse_ino_t parent, const char *name)
{
    struct ll_inval_item it;

    if (!name || !name[0] || (virt_kind(parent) && virt_kind(parent) != 2 && virt_kind(parent) != 4))
        return;
    memset(&it, 0, sizeof(it));
    it.kind = LL_INVAL_ENTRY;
    it.parent = parent;
    strncpy(it.name, name, EFS_MAX_NAME - 1);
    ll_inval_push(it);
}

/* Expired FUSE entries can retain lookup references until memory pressure.
 * Drop the query dentry on close so FORGET, rather than expiry alone, releases
 * the bounded identity slot for recycling. Open descriptions retain their own bytes. */
static void vq_closed(efs_ino_t ino)
{
    efs_ino_t dir;
    char term[EFS_MAX_NAME];
    if (vq_lookup(ino, &dir, term, sizeof(term)) == 0)
        ll_inval_entry(command_script(term) ? virt_commands_ino(dir) : virt_find_ino(dir),
                       command_script(term) ? term+1 : term);
}

void ll_inval_inode(fuse_ino_t ino, off_t off, off_t len)
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

int ll_inval_start(void)
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

void ll_inval_stop(void)
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

int efs_rc_to_errno(int rc);
int check_access(const struct efs_inode *ino, uid_t uid, gid_t gid, int mask);
int virtual_access(efs_ino_t ino)
{
    struct efs_inode row;
    int rc = efs_client_stat_refresh(ino, &row);
    if (rc != EFS_OK) return efs_rc_to_errno(rc);
    const struct fuse_ctx *ctx = ll_ctx();
    return ctx ? check_access(&row, ctx->uid, ctx->gid, R_OK | X_OK) : 0;
}

/* stat for the virtual .find directory itself. */
int find_dir_fill_stat(const struct efs_inode *parent, struct stat *stbuf)
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


/* POSIX permission check: does the caller (uid/gid) have the requested access
 * (mask: R_OK/W_OK/X_OK) to a file with the given mode/uid/gid? Root (uid 0)
 * bypasses DAC except execute access to a nondirectory with no execute bits.
 * We enforce this in the FUSE client (not via the default_permissions
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
    gid_t small[64];
    int capacity = (int)(sizeof(small) / sizeof(small[0]));
    int n = t_req ? fuse_req_getgroups(t_req, capacity, small) : -1;
    if (n < 0) return 0;
    gid_t *groups = small;
    /* libfuse returns the total count even when it exceeds the buffer.
     * Keep the common path on the stack, and bound the larger Linux list. */
    if (n > capacity) {
        if (n > 65536) return 0;
        capacity = n;
        groups = malloc((size_t)capacity * sizeof(*groups));
        if (!groups) return 0;
        n = fuse_req_getgroups(t_req, capacity, groups);
        /* Credentials can change between reads. Refuse an unavailable or
         * newly oversized list rather than read past initialized entries. */
        if (n < 0 || n > capacity) {
            free(groups);
            return 0;
        }
    }
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (groups[i] == gid) {
            found = 1;
            break;
        }
    }
    if (groups != small) free(groups);
    return found;
}

int check_access(const struct efs_inode *ino, uid_t uid, gid_t gid,
                        int mask)
{
    if (uid == 0) {
        if ((mask & X_OK) && !efs_mode_is_dir(ino->mode) &&
            !(ino->mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            return -EACCES;
        return 0;
    }
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

int check_dir_wx(const struct efs_inode *dir)
{
    if (!dir || dir->ino == EFS_ROOT_INO)
        return 0;
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    return check_access(dir, ctx->uid, ctx->gid, W_OK | X_OK);
}

int check_dir_x(const struct efs_inode *dir)
{
    if (!dir || dir->ino == EFS_ROOT_INO)
        return 0;
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    return check_access(dir, ctx->uid, ctx->gid, X_OK);
}

static uint64_t inode_allocated_bytes(const struct efs_inode *ino);

void fill_stat_from_inode(struct stat *stbuf, const struct efs_inode *ino)
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

int check_chown_perm(const struct efs_inode *ino, uid_t uid, gid_t gid)
{
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx || ctx->uid == 0)
        return 0;
    /* Even an unchanged-owner request requires ownership: a successful
     * chown can update ctime and is not an authorization-free no-op. */
    if (ctx->uid != ino->uid)
        return -EPERM;
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

int file_chunk_present(efs_ino_t ino, uint32_t ci)
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
    if (n == (uint64_t)n_loc) {
        efs_client_lock_dir(ino->ino);
        pthread_mutex_lock(&g_client.idx_mu);
        tail = file_chunk_present(ino->ino,
                                  (uint32_t)(ino->size / cs));
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino->ino);
    } else
        tail = n * cs >= ino->size;
    if (!tail)
        return n * cs;
    alloc = (n - 1) * cs + partial;
    return alloc;
}

/* W16.1: BUSY and NOT_PRIMARY are not a missing inode. */
int fuse_stat_errno(int rc)
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
