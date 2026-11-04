/* efs-fuse: create, mkdir, unlink, statfs, attributes, symlink, link, truncate. */
#include "fuse_internal.h"
#include "support.h"
#include "open.h"

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
    if (urc == EFS_ERR_BUSY || urc == EFS_ERR_AGAIN ||
        urc == EFS_ERR_NOT_PRIMARY)
        return -EBUSY;
    if (urc == EFS_ERR_INVAL)
        return -EINVAL;
    if (urc == EFS_ERR_OVERFLOW)
        return -EOVERFLOW;
    if (urc == EFS_ERR_ACCES)
        return -EACCES;
    if (urc == EFS_ERR_NOMEM)
        return -ENOMEM;
    return -EIO;
}

static int fuse_create_errno(int create_rc, efs_ino_t parent, const char *name)
{
    (void)parent;
    (void)name;
    if (create_rc == EFS_ERR_QUOTA)
        return -ENOSPC;
    /* The authoritative refusal belongs to this request's linearization
     * point. Later LOOKUP can race with unlink/rename or lose authority;
     * neither may replace a proven EXIST verdict with IO or BUSY. */
    if (create_rc == EFS_ERR_EXIST)
        return -EEXIST;
    return fuse_stat_errno(create_rc == EFS_OK ? EFS_ERR_IO : create_rc);
}

/* Wait only before an identity is allocated. Retained requests always resume
 * their exact identity; local receipt-window pressure must not turn a short
 * burst of creates into an application-visible EBUSY. */
static int efs_fuse_create_begin_wait(struct efs_client_create_request *request,
    efs_ino_t parent, const char *name, uint32_t mode, uid_t uid, gid_t gid)
{
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc=EFS_ERR_BUSY;
    while(!efs_client_rpc_past_deadline()) {
        rc=efs_client_create_begin(request,parent,name,mode,uid,gid);
        if(rc!=EFS_ERR_BUSY || request->active)break;
        efs_client_opid_wait(1);
    }
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

/* A transport failure can hide a committed CREATE. Resume its retained
 * request within the caller budget instead of returning EIO after one try. */
static int efs_fuse_create_resume_wait(struct efs_client_create_request *request)
{
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc=EFS_ERR_BUSY;
    while(!efs_client_rpc_past_deadline()) {
        rc=efs_client_create_resume(request,0);
        if(request->complete || !request->active ||
           (rc!=EFS_ERR_BUSY && rc!=EFS_ERR_NET))break;
        usleep(1000);
    }
    if(!request->complete && efs_client_rpc_past_deadline())rc=EFS_ERR_BUSY;
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

int efs_fuse_create_at(fuse_ino_t parent_ino, const char *name,
                              mode_t mode, struct fuse_file_info *fi,
                              efs_ino_t *out_ino, struct efs_inode *created)
{
    struct efs_inode parent;
    int rc;
    if (created)
        memset(created, 0, sizeof(*created));
    if (strlen(name) > 255)
        return -ENAMETOOLONG;
    rc = efs_client_stat_refresh((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;

    mode_t create_mode = S_IFREG | mode;
    uid_t uid;
    gid_t gid;
    efs_create_credentials(&parent, &create_mode, &uid, &gid);
    efs_ino_t ino = 0;
    struct efs_pending_create *pending_create=efs_pending_create_alloc();
    if(!pending_create)return -ENOMEM;
    int create_rc=efs_fuse_create_begin_wait(&pending_create->request,parent.ino,
        name,create_mode,uid,gid);
    if(create_rc==EFS_OK)create_rc=efs_fuse_create_resume_wait(&pending_create->request);
    if(create_rc==EFS_OK && pending_create->request.complete) {
        ino=pending_create->request.inode.ino;
        if(created)*created=pending_create->request.inode;
    } else if(create_rc==EFS_OK)create_rc=EFS_ERR_BUSY;
    if(create_rc!=EFS_OK) {
        if(pending_create->request.complete && pending_create->request.result!=EFS_OK) {
            (void)efs_client_create_reset(&pending_create->request);
            efs_pending_create_free(pending_create);
        } else if(!pending_create->request.attempted) {
            (void)efs_client_create_cancel(&pending_create->request);
            efs_pending_create_free(pending_create);
        } else efs_pending_create_queue(pending_create);
        pending_create=NULL;
    }
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
                /* This is an OPEN, not a newly created row. Reuse its
                 * staged-row pin, access checks, truncate and lease handoff;
                 * FileID acquisition alone does not stage a missing inode. */
                int open_rc = efs_fuse_open_ino((fuse_ino_t)exist.ino, fi);
                if (open_rc)
                    return open_rc;
                if (out_ino)
                    *out_ino = exist.ino;
                return 0;
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
        int frc=efs_file_open(fi,ino);
        if(frc) {
            if(pending_create)efs_pending_create_queue(pending_create);
            return frc;
        }
        fuse_fi_direct_io(fi);
        int handed_off=0;
        int hrc=pending_create ? efs_open_acquire_created(ino,
            pending_create->request.generation,pending_create->request.wire.q.id.seq,
            &handed_off) : efs_open_acquire_fileid(ino,0);
        if(hrc!=EFS_OK) {
            efs_file_close(fi);
            if(pending_create) {
                if(!handed_off)efs_pending_create_queue(pending_create);
                else {
                    (void)efs_client_create_reset(&pending_create->request);
                    efs_pending_create_free(pending_create);
                }
            }
            return fuse_stat_errno(hrc);
        }
        if(pending_create) {
            /* Stage only after the open owner pins the committed empty row.
             * Do not invent dirty work solely to prevent cache eviction. */
            int stage_rc=efs_client_create_stage_open_result(&pending_create->request);
            (void)efs_client_create_reset(&pending_create->request);
            efs_pending_create_free(pending_create);
            if(stage_rc!=EFS_OK) {
                (void)efs_fuse_release_ino(ino,fi);
                return fuse_stat_errno(stage_rc);
            }
        }
    } else if(pending_create)efs_pending_create_queue(pending_create);
    if (out_ino)
        *out_ino = ino;
    return 0;
}

int efs_fuse_mkdir_at(fuse_ino_t parent_ino, const char *name, mode_t mode,
                             efs_ino_t *out_ino, struct efs_inode *created)
{
    struct efs_inode parent;
    int rc;
    if (strlen(name) > 255)
        return -ENAMETOOLONG;
    rc = efs_client_stat_refresh((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    mode_t create_mode = S_IFDIR | mode;
    uid_t uid;
    gid_t gid;
    efs_create_credentials(&parent, &create_mode, &uid, &gid);
    efs_ino_t ino = 0;
    int mk = efs_client_create_result_inode(parent.ino, name, create_mode,
                                      uid, gid, 0, &ino, created);
    /* A directory seen after refusal may belong to a competing creator.
     * Only the original RPC's proven success can satisfy this mkdir call. */
    if (mk != EFS_OK)
        return fuse_create_errno(mk, parent.ino, name);
    if (out_ino)
        *out_ino = ino;
    return 0;
}

int efs_fuse_unlink_at(fuse_ino_t parent_ino, const char *name)
{
    struct efs_inode parent;
    int rc;
    rc = efs_client_stat_refresh((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    return fuse_unlink_errno(efs_client_unlink(parent.ino, name, false));
}

int efs_fuse_rmdir_at(fuse_ino_t parent_ino, const char *name)
{
    struct efs_inode parent;
    int rc = efs_client_stat_refresh((efs_ino_t)parent_ino, &parent);
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
static struct {
    efs_node_id_t id;
    uint64_t capacity, available, valid;
} statfs_capacity[EFS_MAX_NODES];

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
            plen == sizeof(struct efs_msg_status_reply)) {
            struct efs_msg_status_reply r;
            memcpy(&r, payload, sizeof(r));
            pthread_mutex_lock(&g_client.lock);
            for (uint32_t j = 0; j < g_client.node_count; j++)
                if (g_client.nodes[j].id == nid) {
                    g_client.nodes[j].used = r.used;
                    g_client.nodes[j].quota = r.quota;
                    statfs_capacity[j].id = nid;
                    statfs_capacity[j].capacity = r.capacity_bytes;
                    statfs_capacity[j].available = r.available_bytes;
                    statfs_capacity[j].valid = r.capacity_valid;
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

int efs_fuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));

    statfs_refresh_usage();
    pthread_mutex_lock(&g_client.lock);

    /* Use distinct-node placement over server filesystem capacity and available
     * bytes, bounded by configured quotas. A node whose capacity reading is
     * unavailable (capacity_valid == 0) falls back to its quota. */
    uint64_t quota[EFS_MAX_NODES], room[EFS_MAX_NODES];
    uint32_t nn = g_client.node_count, all_quota = 1;
    if (nn > EFS_MAX_NODES)
        nn = EFS_MAX_NODES;
    for (uint32_t i = 0; i < nn; i++) {
        uint64_t q = g_client.nodes[i].quota, u = g_client.nodes[i].used;
        quota[i] = q;
        room[i] = q > u ? q - u : 0;
        if (statfs_capacity[i].valid && statfs_capacity[i].id == g_client.nodes[i].id) {
            quota[i] = statfs_capacity[i].capacity;
            room[i] = statfs_capacity[i].available;
        } else if (q == 0) {
            all_quota = 0;
        }
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

int efs_rc_to_errno(int rc)
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
    case EFS_ERR_OVERFLOW: return -EOVERFLOW;
    case EFS_ERR_ILSEQ: return -EILSEQ;
    case EFS_ERR_UNSUPPORTED: return -EIO;
    case EFS_ERR_FRAG_FORMAT: return -EIO;
    case EFS_ERR_FRAG_IDENTITY: return -EIO;
    case EFS_ERR_FENCED:    return -EIO; /* mount session fenced (I23) */
    case EFS_ERR_UNREGISTERED: return -EIO;
    case EFS_ERR_BUSY:
        return -EBUSY;
    case EFS_ERR_AGAIN:
        return -EAGAIN;
    case EFS_ERR_NOLCK:
        return -ENOLCK;
    case EFS_ERR_NO_QUORUM:
    case EFS_ERR_NET:
    default:                return -EIO;
    }
}

int efs_fuse_chmod_ino(fuse_ino_t ino, mode_t mode)
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

int efs_fuse_chown_ino(fuse_ino_t ino, uid_t uid, gid_t gid)
{
    struct efs_inode row;
    int rc;
    if (virt_kind(ino))
        return -EACCES;
    rc = efs_client_stat_refresh((efs_ino_t)ino, &row);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    rc = check_chown_perm(&row, uid, gid);
    if (rc != 0)
        return rc;
    rc = efs_client_chown(row.ino, uid, gid);
    return efs_rc_to_errno(rc);
}

int efs_fuse_symlink_at(const char *link, fuse_ino_t parent_ino,
                               const char *name, efs_ino_t *out_ino)
{
    struct efs_inode parent;
    int rc = efs_client_stat_refresh((efs_ino_t)parent_ino, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (!efs_mode_is_dir(parent.mode))
        return -ENOTDIR;
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    mode_t create_mode = S_IFLNK | 0777;
    uid_t uid;
    gid_t gid;
    efs_create_credentials(&parent, &create_mode, &uid, &gid);
    efs_ino_t ino = 0;
    int create_rc = efs_client_create_result(parent.ino, name, create_mode,
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

int efs_fuse_readlink_ino(fuse_ino_t ino, char *buf, size_t size)
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

int efs_fuse_link_at(fuse_ino_t src_ino, fuse_ino_t newparent,
                            const char *newname)
{
    struct efs_inode src, parent;
    int rc;
    if (virt_kind(src_ino) || virt_kind(newparent))
        return -EACCES;
    rc = efs_client_stat_refresh((efs_ino_t)src_ino, &src);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    if (efs_mode_is_dir(src.mode))
        return -EPERM;
    rc = efs_client_stat_refresh((efs_ino_t)newparent, &parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&parent);
    if (wx != 0)
        return wx;
    rc = efs_client_link(src.ino, parent.ino, newname);
    return rc == EFS_ERR_NOT_PRIMARY ? -EBUSY : efs_rc_to_errno(rc);
}

int efs_fuse_utimens_ino(fuse_ino_t ino, const struct timespec tv[2])
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
    if (vk == 2 || vk == 4)
        return -EISDIR;
    if (vk)
        return -EACCES;
    if (efs_client_rpc_past_deadline())
        return -EAGAIN;
    if (!fi || !fi->fh) {
        struct efs_inode row;
        int rc = efs_client_stat_refresh((efs_ino_t)ino, &row);
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

int efs_fuse_truncate_ino(fuse_ino_t ino, off_t size,
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
