/* efs-fuse: lookup memo, getattr and readdir collection. */
#include "fuse_internal.h"
#include "getattr.h"
#include "support.h"
#include "../commands/source.h"

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
__thread uint64_t t_lookup_memo_serial;
__thread struct efs_inode t_getattr_row;
__thread efs_ino_t t_getattr_ino;

uint64_t fuse_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint64_t lookup_memo_start(void)
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
void lookup_memo_mutation_begin(void)
{
    pthread_mutex_lock(&g_lookup_memo_mu);
    ++g_lookup_memo_serial;
    ++g_lookup_memo_active;
    for (unsigned i = 0; i < LOOKUP_MEMO_N; ++i)
        g_lookup_memo[i].ino = 0;
    pthread_mutex_unlock(&g_lookup_memo_mu);
}

void lookup_memo_mutation_end(void)
{
    pthread_mutex_lock(&g_lookup_memo_mu);
    ++g_lookup_memo_serial;
    --g_lookup_memo_active;
    for (unsigned i = 0; i < LOOKUP_MEMO_N; ++i)
        g_lookup_memo[i].ino = 0;
    pthread_mutex_unlock(&g_lookup_memo_mu);
}

static void lookup_memo_observe(unsigned reason);

void lookup_memo_put(const struct efs_inode *row)
{
    unsigned i;

    if (!row || !row->ino)
        return;
    pthread_mutex_lock(&g_lookup_memo_mu);
    if (!t_lookup_memo_serial || g_lookup_memo_active ||
        t_lookup_memo_serial != g_lookup_memo_serial) {
        unsigned reason = g_lookup_memo_active ? 5 : 6;
        pthread_mutex_unlock(&g_lookup_memo_mu);
        lookup_memo_observe(reason);
        return;
    }
    i = g_lookup_memo_i++ % LOOKUP_MEMO_N;
    g_lookup_memo[i].ino = row->ino;
    g_lookup_memo[i].row = *row;
    g_lookup_memo[i].us = fuse_now_us();
    pthread_mutex_unlock(&g_lookup_memo_mu);
    lookup_memo_observe(4);
}

/* Diagnostic counters only; one line per1024 memo attempts. They never
 * change cache lifetime, mutation fencing, RPC or quorum decisions. */
static void lookup_memo_observe(unsigned reason)
{
    static int env_rpc_prof = -1; /* W143: read once, per-op path; any thread may be first */
    if (__atomic_load_n(&env_rpc_prof, __ATOMIC_ACQUIRE) < 0)
        __atomic_store_n(&env_rpc_prof, getenv("EFS_RPC_PROF") != NULL, __ATOMIC_RELEASE);
    static uint64_t counts[7], total;
    if (!__atomic_load_n(&env_rpc_prof, __ATOMIC_RELAXED))
        return;
    __atomic_add_fetch(&counts[reason], 1, __ATOMIC_RELAXED);
    if (reason >= 4)
        return;
    uint64_t n = __atomic_add_fetch(&total, 1, __ATOMIC_RELAXED);
    if (!(n % 1024))
        fprintf(stderr, "MEMO-PROF attempts=%llu hit=%llu active=%llu expired=%llu missing=%llu put_stored=%llu put_active=%llu put_fenced=%llu\n",
                (unsigned long long)n,
                (unsigned long long)__atomic_load_n(&counts[0], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&counts[1], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&counts[2], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&counts[3], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&counts[4], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&counts[5], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&counts[6], __ATOMIC_RELAXED));
}

static int lookup_memo_take(efs_ino_t ino, struct efs_inode *out)
{
    uint64_t now;
    int i;
    unsigned reason = 3;

    if (!ino || !out)
        return 0;
    now = fuse_now_us();
    pthread_mutex_lock(&g_lookup_memo_mu);
    if (g_lookup_memo_active) {
        pthread_mutex_unlock(&g_lookup_memo_mu);
        lookup_memo_observe(1);
        return 0;
    }
    for (i = 0; i < LOOKUP_MEMO_N; i++) {
        if (g_lookup_memo[i].ino == ino)
            reason = 2;
        if (g_lookup_memo[i].ino == ino &&
            now - g_lookup_memo[i].us < LOOKUP_MEMO_US) {
            *out = g_lookup_memo[i].row;
            g_lookup_memo[i].ino = 0;
            pthread_mutex_unlock(&g_lookup_memo_mu);
            lookup_memo_observe(0);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_lookup_memo_mu);
    lookup_memo_observe(reason);
    return 0;
}

static void getattr_note_row(const struct efs_inode *row)
{
    if (!row || !row->ino)
        return;
    t_getattr_row = *row;
    t_getattr_ino = row->ino;
}

int efs_fuse_getattr_ino(fuse_ino_t ino, struct stat *stbuf,
                                struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    int rc;
    if (vk == 1 || vk == 2) {
        int access = virtual_access(virt_parent(ino));
        if (access) return access;
    }
    if (vk==4) {
        if (!commands_enabled()) return -ENOENT;
        rc=virtual_access(virt_parent(ino));if (rc) return rc;
        memset(stbuf,0,sizeof(*stbuf));stbuf->st_ino=ino;
        stbuf->st_mode=S_IFDIR|0555;stbuf->st_nlink=2;return 0;
    }
    struct efs_file_handle *vh = efs_file_handle(fi);
    if ((vk == 1 || vk == 3) && vh && vh->snapshot) {
        memset(stbuf, 0, sizeof(*stbuf));
        stbuf->st_ino = ino;
        stbuf->st_mode = S_IFREG | 0444;
        if (vk==3) {
            char term[EFS_MAX_NAME];efs_ino_t dir;
            if (!vq_lookup(ino,&dir,term,sizeof(term)) && command_script(term)) stbuf->st_mode=S_IFREG|0555;
        }
        stbuf->st_nlink = 1;
        stat_set_size_blocks(stbuf, vh->snapshot_len, vh->snapshot_len);
        return 0;
    }
    /* Linux can omit fi for fstat. A genuinely open virtual inode keeps
     * its immutable attributes even after server feature disablement. */
    if (vk==3) {
        size_t length;char term[EFS_MAX_NAME];efs_ino_t directory;
        if (vq_pinned_size(ino,&length,1) && !vq_lookup(ino,&directory,term,sizeof(term))) {
            memset(stbuf,0,sizeof(*stbuf));stbuf->st_ino=ino;
            stbuf->st_mode=S_IFREG | (command_script(term) ? 0555 : 0444);
            stbuf->st_nlink=1;stat_set_size_blocks(stbuf,length,length);return 0;
        }
    }
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
        if (!commands_enabled())
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
        char query_path[EFS_MAX_PATH];
        struct efs_inode parent;
        if (!commands_enabled())
            return -ENOENT;
        if (vq_lookup(ino, &dir_ino, term, sizeof(term)) != 0)
            return -ENOENT;
        rc=virtual_access(dir_ino);if (rc) return rc;
        if (command_script(term)) {
            memset(stbuf,0,sizeof(*stbuf));stbuf->st_ino=ino;stbuf->st_mode=S_IFREG|0555;stbuf->st_nlink=1;
            stat_set_size_blocks(stbuf,sizeof(efs_command_source)-1,sizeof(efs_command_source)-1);return 0;
        }
        if (efs_client_stat_ino(dir_ino, &parent) != EFS_OK)
            return -ENOENT;
        size_t pinned_len;
        if (vq_pinned_size(ino, &pinned_len, 0)) {
            memset(stbuf, 0, sizeof(*stbuf));
            stbuf->st_ino=ino; stbuf->st_mode=S_IFREG | 0444; stbuf->st_nlink=1;
            stbuf->st_uid=parent.uid; stbuf->st_gid=parent.gid;
            stat_set_size_blocks(stbuf,pinned_len,pinned_len);
            return 0;
        }
        snprintf(query_path,sizeof(query_path),"/%s/%s",EFS_FIND_NAME,term);
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




static int efs_fuse_readdir_ino(fuse_ino_t ino, struct readdir_collect_arg *col,
                                struct efs_inode *self_out);

struct efs_dirh *dirh_new(efs_ino_t ino)
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

void dirh_free(struct efs_dirh *dh)
{
    if (!dh)
        return;
    free(dh->ents);
    free(dh);
}

int dirh_fill(struct efs_dirh *dh, fuse_ino_t ino)
{
    struct readdir_collect_arg col;
    struct efs_inode row, prow;
    int rc;

    memset(&col, 0, sizeof(col));
    memset(&row, 0, sizeof(row));
    rc = efs_fuse_readdir_ino(ino, &col, &row);
    free(col.name_index);
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
    if (virt_kind(ino) == 2) dh->parent_ino = virt_commands_ino(virt_parent(ino));
    if (virt_kind(ino) == 4) dh->parent_ino = virt_parent(ino);
    if (efs_client_stat_ino((efs_ino_t)dh->parent_ino, &prow) == EFS_OK)
        dh->parent_mode = prow.mode;
    dh->listed = 1;
    return 0;
}

/* Keep first-seen names while merging shard pages. Indices survive ents
 * reallocations; this temporary index is released once the listing is filled. */
static size_t readdir_name_hash(const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return (size_t)hash;
}

static int readdir_name_reserve(struct readdir_collect_arg *col, size_t need)
{
    size_t cap = col->name_index_cap ? col->name_index_cap : 32;
    if (need > SIZE_MAX / 2)
        return -ENOMEM;
    while (cap / 2 < need) {
        if (cap > SIZE_MAX / 2)
            return -ENOMEM;
        cap *= 2;
    }
    if (cap == col->name_index_cap)
        return 0;
    if (cap > SIZE_MAX / sizeof(size_t))
        return -ENOMEM;
    size_t *index = calloc(cap, sizeof(*index));
    if (!index)
        return -ENOMEM;
    for (size_t i = 0; i < col->count; i++) {
        size_t slot = readdir_name_hash(col->ents[i].name) & (cap - 1);
        while (index[slot])
            slot = (slot + 1) & (cap - 1);
        index[slot] = i + 1;
    }
    free(col->name_index);
    col->name_index = index;
    col->name_index_cap = cap;
    return 0;
}

/* Append one page, skipping name dups (spread dirs merge per-shard scans).
 * Returns 0 or -ENOMEM. */
static int readdir_collect_page(struct readdir_collect_arg *col,
                                const struct efs_inode *ents, uint32_t n)
{
    if (n == 0)
        return 0;
    if ((size_t)n > SIZE_MAX - col->count)
        return -ENOMEM;
    size_t need = col->count + n;
    if (readdir_name_reserve(col, need) != 0)
        return -ENOMEM;
    if (need > col->cap) {
        size_t ncap = col->cap ? col->cap : 16;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2)
                return -ENOMEM;
            ncap *= 2;
        }
        if (ncap > SIZE_MAX / sizeof(*col->ents))
            return -ENOMEM;
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
        size_t slot = readdir_name_hash(ents[i].name) & (col->name_index_cap - 1);
        while (col->name_index[slot] &&
               strcmp(col->ents[col->name_index[slot] - 1].name, ents[i].name) != 0)
            slot = (slot + 1) & (col->name_index_cap - 1);
        if (col->name_index[slot])
            continue;
        struct readdir_ent *e = &col->ents[col->count];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, ents[i].name, EFS_MAX_NAME - 1);
        e->st.st_ino = ents[i].ino;
        e->st.st_mode = ents[i].mode;
        col->name_index[slot] = ++col->count;
    }
    return 0;
}

static int efs_fuse_readdir_ino(fuse_ino_t ino, struct readdir_collect_arg *col,
                                struct efs_inode *self_out)
{
    if (virt_kind(ino)==4) {
        if (!commands_enabled()) return -ENOENT;
        int rc=virtual_access(virt_parent(ino));if (rc) return rc;
        const char *names[]={"help","ls","find","stats"};
        col->ents=calloc(4,sizeof(*col->ents));if (!col->ents) return -ENOMEM;
        col->count=col->cap=4;
        for (unsigned i=0;i<4;i++) {
            snprintf(col->ents[i].name,sizeof(col->ents[i].name),"%s",names[i]);
            col->ents[i].st.st_mode=S_IFREG|0555;
        }
        return 0;
    }
    if (virt_kind(ino) == 2) {
        if (!commands_enabled())
            return -ENOENT;
        return 0; /* .find lists nothing; queries are explicit lookups */
    }
    if (virt_kind(ino))
        return -ENOTDIR;

    struct efs_inode parent;
    /* Local directory rows may be overwritten by an older LOOKUP reply.
     * Listing permission must come from a fresh authority read, including
     * peer chmod; the local cache cannot authorize enumeration. */
    int rc = efs_client_stat_refresh((efs_ino_t)ino, &parent);
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
            return fuse_stat_errno(rc);
        if (readdir_collect_page(col, ents, n) != 0)
            return -ENOMEM;
        /* A bounded scan may consume only tombstones/shadowed names.
         * The RPC decoder requires a progressing cursor; only done is EOF. */
    }
    return 0;
}

int efs_fuse_access_ino(fuse_ino_t ino, int mask)
{
    if (virt_kind(ino)) {
        if (!commands_enabled()) return -ENOENT;
        if (mask & W_OK) return -EACCES;
        efs_ino_t parent=virt_parent(ino);
        if (virt_kind(ino)==3) {
            char term[EFS_MAX_NAME];
            if (vq_lookup(ino,&parent,term,sizeof(term))) return -ENOENT;
            if ((mask & X_OK) && !command_script(term)) return -EACCES;
        }
        return virtual_access(parent);
    }
    struct efs_inode row;
    int lrc = efs_client_stat_refresh((efs_ino_t)ino, &row);
    if (lrc == EFS_ERR_ACCES)
        return -EACCES;
    if (lrc != EFS_OK)
        return fuse_stat_errno(lrc);
    const struct fuse_ctx *ctx = ll_ctx();
    if (!ctx)
        return 0;
    return check_access(&row, ctx->uid, ctx->gid, mask);
}

int efs_fuse_truncate_ino(fuse_ino_t ino, off_t size,
                                 struct fuse_file_info *fi);
int efs_fuse_release_ino(fuse_ino_t ino, struct fuse_file_info *fi);
