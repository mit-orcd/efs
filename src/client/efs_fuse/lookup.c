/* efs-fuse: lookup, setattr and deep-callback sampling. */
#include "fuse_internal.h"
#include "support.h"
#include "../commands/source.h"

void fill_entry(fuse_ino_t ino, struct fuse_entry_param *e,
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

void fill_entry_row(fuse_ino_t ino, struct fuse_entry_param *e,
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

int lookup_fill(fuse_ino_t ino, struct fuse_entry_param *e,
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
 * no open fh refreshes strictly; a transient follow-up GETATTR failure must
 * not turn that known committed mutation into a failed creation reply.
 * Its local row is a reply fallback, never authority for a new operation. */
int lookup_fill_committed(fuse_ino_t ino, struct fuse_entry_param *e,
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

int efs_fuse_lookup_at(fuse_ino_t parent, const char *name,
                              struct fuse_entry_param *e)
{
    struct efs_inode prow, row;
    int rc;

    if (!name || name[0] == '\0')
        return -ENOENT;
    if (strlen(name) > 255)
        return -ENAMETOOLONG;

    if (virt_kind(parent)==4) {
        if (!commands_enabled()) return -ENOENT;
        rc=virtual_access(virt_parent(parent));if (rc) return rc;
        if (!strcmp(name,".")) return lookup_fill(parent,e,NULL);
        if (!strcmp(name,"..")) return lookup_fill(virt_parent(parent),e,NULL);
        if (!strcmp(name,".query")) return lookup_fill(virt_find_ino(virt_parent(parent)),e,NULL);
        char term[EFS_MAX_NAME];snprintf(term,sizeof(term),"@%s",name);
        if (!command_script(term)) return -ENOENT;
        fuse_ino_t id=vq_intern(virt_parent(parent),term,virtual_version(efs_command_source,sizeof(efs_command_source)-1),sizeof(efs_command_source)-1);
        if(!id)return -ENOMEM;
        rc=lookup_fill(id,e,NULL);vq_unreserve(id);return rc;
    }
    if (virt_kind(parent) == 2) {
        /* Query children authorize through the server snapshot RPC below.
         * Dot-directory traversal still needs its independent access check. */
        if (!strcmp(name,".") || !strcmp(name,"..")) {
            int access=virtual_access(virt_parent(parent));if (access) return access;
        }
        fuse_ino_t q;
        if (!commands_enabled())
            return -ENOENT;
        if (strcmp(name, ".") == 0)
            return lookup_fill(parent, e, NULL);
        if (strcmp(name, "..") == 0)
            return lookup_fill(virt_commands_ino(virt_parent(parent)), e, NULL);
        efs_ino_t dir=virt_parent(parent);
        char query[EFS_MAX_PATH];
        int n=snprintf(query,sizeof(query),"/%s/%s",EFS_FIND_NAME,name);
        if (n<0 || (size_t)n>=sizeof(query)) return -ENAMETOOLONG;
        int qr=find_query_ensure(dir,query,name);
        if (qr) return qr;
        q=0;
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
        qr=lookup_fill(q,e,NULL);vq_unreserve(q);return qr;
    }
    if (virt_kind(parent))
        return -ENOENT;

    if (strcmp(name, ".") == 0)
        return lookup_fill(parent, e, NULL);
    if (strcmp(name, "..") == 0) {
        fuse_ino_t up = parent;
        if (parent != FUSE_ROOT_ID &&
            efs_client_stat_refresh((efs_ino_t)parent, &prow) == EFS_OK &&
            prow.parent)
            up = (fuse_ino_t)prow.parent;
        return lookup_fill(up, e, NULL);
    }
    /* Parent search permission must not come from the staging cache. */
    rc = efs_client_stat_refresh((efs_ino_t)parent, &prow);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(prow.mode))
        return -ENOTDIR;
    rc = check_dir_x(&prow);
    if (rc != 0)
        return rc;
    /* Local namespace rows are not peer coherence evidence. A peer may
     * rename a directory while its inode still exists: GETATTR cannot
     * validate the cached parent/name binding. Resolve every ordinary name
     * through authoritative LOOKUP before adopting its row. */
    rc = efs_client_rpc_lookup(g_client.export_id, (efs_ino_t)parent, name, &row);
    if (!strcmp(name,".efs") && commands_enabled()) {
        if (rc==EFS_ERR_NOT_FOUND) return lookup_fill(virt_commands_ino(parent),e,NULL);
        if (rc==EFS_OK) {
            static unsigned collisions;
            unsigned count=__atomic_add_fetch(&collisions,1,__ATOMIC_RELAXED);
            if (count<=16) fprintf(stderr,"efs commands: real .efs takes precedence in directory %llu\n",(unsigned long long)parent);
            else if (count==17) fprintf(stderr,"efs commands: further real .efs collision diagnostics suppressed\n");
        }
    }
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    /* Adopt the LOOKUP row (full resolved inode) instead of throwing it
     * away and re-statting locally — that hid peer size/nlink growth. */
    efs_client_adopt_lookup(&row, &row);
    fill_entry_row((fuse_ino_t)row.ino, e, &row);
    return 0;
}

int efs_fuse_setattr_ino(fuse_ino_t ino, struct stat *attr, int to_set,
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

int dirbuf_add(fuse_req_t req, char *buf, size_t bufsize, size_t *used,
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

/* Explicit bounded deep-path diagnostics. Normal callbacks do not read a
 * clock or write a line. This records callback service, not kernel queue time;
 * join caller PID/monotonic time to the transparent syscall observer. */
static int deep_callback_name(const char *name)
{
    return name && name[0] == 'd' && name[1] >= '0' && name[1] <= '9' &&
           name[2] >= '0' && name[2] <= '9' && name[3] == '\0';
}


struct deep_callback_sample deep_callback_begin(fuse_req_t req, const char *name)
{
    struct deep_callback_sample sample = {0};
    static int enabled;
    static unsigned samples;
    static pid_t callers[128];
    int state = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
    if (!state) {
        state = getenv("EFS_RPC_PROF") && getenv("EFS_DEEP_PATH_PROF") ? 2 : 1;
        __atomic_store_n(&enabled, state, __ATOMIC_RELEASE);
    }
    if (state != 2)
        return sample;
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    if (!ctx || ctx->pid <= 0)
        return sample;
    int selected = deep_callback_name(name), found = 0;
    for (unsigned i = 0; i < 128; ++i) {
        pid_t pid = __atomic_load_n(&callers[i], __ATOMIC_RELAXED);
        if (pid == ctx->pid) { found = 1; break; }
        if (!pid && selected) {
            if (__atomic_compare_exchange_n(&callers[i], &pid, ctx->pid, 0,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED) ||
                pid == ctx->pid) { found = 1; break; }
        }
    }
    if (!found)
        return sample;
    unsigned slot = __atomic_load_n(&samples, __ATOMIC_RELAXED);
    do {
        if (slot >= 16384)
            return sample;
    } while (!__atomic_compare_exchange_n(&samples, &slot, slot + 1, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sample.begin = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    sample.pid = (long)ctx->pid;
    memcpy(sample.name, selected ? name : "other", selected ? 4 : 6);
    return sample;
}

/* Capture before reply without logging; the after-reply record never
 * dereferences a freed fuse_req or its message/name buffer. */
void deep_callback_reply_begin(struct deep_callback_sample *sample)
{
    if (!sample->begin)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sample->service_end = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

void deep_callback_end(struct deep_callback_sample sample, const char *op,
                              fuse_ino_t parent, int rc)
{
    if (!sample.begin)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    fprintf(stderr, "PATH-PROF op=%s pid=%ld parent=%llu name=%s mono_ns=%llu elapsed_ns=%llu rc=%d reply_ns=%llu\n",
            op, sample.pid, (unsigned long long)parent, sample.name,
            (unsigned long long)sample.begin,
            (unsigned long long)(sample.service_end - sample.begin), rc,
            (unsigned long long)(end - sample.service_end));
}
