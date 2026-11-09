/* efs-fuse: the .find result cache and its operations (find_res.h). */
#include "fuse_internal.h"
#include "find_res.h"
#include "support.h"

/* Rendered bytes of one server result, kept for virtual-inode versioning;
 * immutable per-open descriptions own their copies. Every operation
 * revalidates on the server first. */
#define EFS_FIND_RES_N 16

struct efs_find_res {
    int valid;
    efs_node_id_t owner;
    efs_ino_t dir_ino;
    char term[EFS_MAX_NAME]; /* raw pattern, e.g. "*report*" */
    uint64_t ms;
    uid_t uid;
    gid_t gid;
    int len;
    size_t allocated;
    char *text;
};

static struct efs_find_res g_find_res[EFS_FIND_RES_N];
static pthread_mutex_t g_find_res_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_find_res_bytes;
static pthread_mutex_t g_find_work_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_find_work;
static void find_work_release(void)
{
    pthread_mutex_lock(&g_find_work_mu);g_find_work--;pthread_mutex_unlock(&g_find_work_mu);
}

static int find_res_caller(int i)
{
    const struct fuse_ctx *ctx = ll_ctx();
    return g_find_res[i].uid == (ctx ? ctx->uid : 0) &&
           g_find_res[i].gid == (ctx ? ctx->gid : 0);
}

/* Fetch an authorized server result and retain rendered bytes for virtual
 * inode versioning. Only the mount prefix is rendered locally. */
static int find_query_ensure(efs_ino_t dir_ino, const char *query_path,
                             const char *pattern)
{
    struct efs_msg_query_snapshot_v4 command;
    if (efs_command_path_parse_v4(pattern,&command)) return -EINVAL;
    command.base.root=dir_ino;
    (void)query_path; /* Server supplies authoritative export-relative paths. */

    /* The snapshot RPC checks fresh root/ancestor permissions and rechecks
     * them before delivery; a separate root GETATTR adds no authority. */
    const struct fuse_ctx *ctx = ll_ctx();
    uint64_t now = stats_now_ms();
    /* Query the published server index outside the local byte-cache lock. */
    pthread_mutex_lock(&g_find_work_mu);
    if (g_find_work>=2) { pthread_mutex_unlock(&g_find_work_mu);return -EBUSY; }
    g_find_work++;pthread_mutex_unlock(&g_find_work_mu);
    /* LOOKUP, GETATTR and OPEN reauthorize the same first-page request.
     * Keep its publisher stable so those callbacks cannot retain duplicate
     * sessions on two hosts and close only the last returned token. */
    efs_node_id_t owner=0;
    pthread_mutex_lock(&g_find_res_mu);
    for (int i=0;i<EFS_FIND_RES_N;i++) if (g_find_res[i].valid &&
        find_res_caller(i) && g_find_res[i].dir_ino==dir_ino &&
        !strcmp(g_find_res[i].term,pattern)) { owner=g_find_res[i].owner;break; }
    pthread_mutex_unlock(&g_find_res_mu);
    char *text = NULL;
    int len = 0;size_t allocated=0;
    struct efs_msg_query_snapshot_reply reply;
    int qrc=query_credentials(&command.base);
    uint64_t previous_deadline=efs_client_rpc_deadline_ms();
    uint64_t command_deadline=stats_now_ms()+8000;
    if (!previous_deadline || command_deadline<previous_deadline)
        efs_client_rpc_set_deadline_ms(command_deadline);
    if (!qrc) qrc=efs_client_rpc_query_snapshot_v4_at(&command,owner,&reply,&text);
    efs_client_rpc_set_deadline_ms(previous_deadline);
    if (!qrc) { len=(int)reply.text_len;allocated=(size_t)len+1; }
    if (qrc != 0) {
        find_work_release();free(text);
        if (qrc == -EOVERFLOW || qrc == -ENAMETOOLONG || qrc == -EILSEQ) return qrc;
        return efs_rc_to_errno(qrc);
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
    if (g_find_res[slot].valid) g_find_res_bytes-=g_find_res[slot].allocated;
    free(g_find_res[slot].text);g_find_res[slot].text=NULL;g_find_res[slot].valid=0;
    while (g_find_res_bytes+allocated>(64u<<20)) {
        int evict=-1;uint64_t oldest=UINT64_MAX;
        for (int i=0;i<EFS_FIND_RES_N;i++) if (g_find_res[i].valid && g_find_res[i].ms<=oldest) {
            oldest=g_find_res[i].ms;evict=i;
        }
        if (evict<0) { pthread_mutex_unlock(&g_find_res_mu);find_work_release();free(text);return -ENOMEM; }
        g_find_res_bytes-=g_find_res[evict].allocated;
        free(g_find_res[evict].text);g_find_res[evict].text=NULL;g_find_res[evict].valid=0;
    }
    g_find_res[slot].valid = 1;
    g_find_res[slot].owner=reply.primary_id;
    g_find_res[slot].dir_ino = dir_ino;
    strncpy(g_find_res[slot].term, pattern, EFS_MAX_NAME - 1);
    g_find_res[slot].term[EFS_MAX_NAME - 1] = '\0';
    g_find_res[slot].ms = now;
    g_find_res[slot].uid = ctx ? ctx->uid : 0;
    g_find_res[slot].gid = ctx ? ctx->gid : 0;
    g_find_res[slot].text = text; /* takes ownership */
    g_find_res[slot].len = len;g_find_res[slot].allocated=allocated;g_find_res_bytes+=allocated;
    pthread_mutex_unlock(&g_find_res_mu);find_work_release();
    return 0;
}

/* Result length for (dir, pattern), or -1 on invalid term / error. */
static int find_query_len(efs_ino_t dir_ino, const char *query_path,
                           const char *pattern)
{
    int rc = find_query_ensure(dir_ino, query_path, pattern);
    if (rc != 0) return rc;
    pthread_mutex_lock(&g_find_res_mu);
    int len = -1;
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (g_find_res[i].valid && find_res_caller(i) && g_find_res[i].dir_ino == dir_ino &&
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
int find_query_read(efs_ino_t dir_ino, const char *query_path,
                           const char *pattern,
                           char *buf, size_t size, off_t offset)
{
    int rc = find_query_ensure(dir_ino, query_path, pattern);
    if (rc != 0) return rc;
    pthread_mutex_lock(&g_find_res_mu);
    int n = 0;
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (!g_find_res[i].valid || !find_res_caller(i) || g_find_res[i].dir_ino != dir_ino ||
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

/* LOOKUP of .efs/find/<term>: intern the query node at the result's
 * version and length (lookup.c). */
int find_res_intern(efs_ino_t dir, const char *query, const char *name,
                    fuse_ino_t *qp)
{
    int qr=find_query_ensure(dir,query,name);
    if (qr) return qr;
    fuse_ino_t q=0;
    int present=0;
    pthread_mutex_lock(&g_find_res_mu);
    for (int i=0;i<EFS_FIND_RES_N;i++) if (g_find_res[i].valid &&
        find_res_caller(i) && g_find_res[i].dir_ino==dir &&
        strcmp(g_find_res[i].term,name)==0) {
        present=1;
        q=vq_intern(dir,name,virtual_version(g_find_res[i].text,g_find_res[i].len),
                    g_find_res[i].len);
        break;
    }
    pthread_mutex_unlock(&g_find_res_mu);
    if (!q) return present ? -ENOMEM : -EAGAIN;
    *qp=q;
    return 0;
}

/* OPEN of a query file: copy the bytes LOOKUP versioned into fi (open.c). */
int find_res_open(struct fuse_file_info *fi, fuse_ino_t ino, efs_ino_t dir_ino,
                  const char *query_path, const char *term)
{
    int rc = find_query_ensure(dir_ino, query_path, term);
    if (rc) return rc;
    rc = -EAGAIN; /* Eviction between ensure and pin must not look empty. */
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        if (g_find_res[i].valid && find_res_caller(i) && g_find_res[i].dir_ino == dir_ino &&
            strcmp(g_find_res[i].term, term) == 0) {
            rc = vq_version_matches(ino, g_find_res[i].text, g_find_res[i].len) ?
                virtual_file_open(fi, ino, g_find_res[i].text, g_find_res[i].len) : -ESTALE;
            break;
        }
    }
    pthread_mutex_unlock(&g_find_res_mu);
    return rc;
}

/* Free cached .find query results (efs_fuse_destroy). */
void find_res_clear(void)
{
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        free(g_find_res[i].text);
        g_find_res[i].text = NULL;
        g_find_res[i].valid = 0;
    }
    pthread_mutex_unlock(&g_find_res_mu);
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

/* stat for a .find/<term> query file: runs the query to report the real size
 * (so `cat` reads the full list rather than stopping at a 0 size). */
int find_query_fill_stat(const struct efs_inode *parent,
                                const char *query_path, const char *pattern,
                                struct stat *stbuf)
{
    int len = find_query_len(parent->ino, query_path, pattern);
    if (len < 0)
        return len;
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
