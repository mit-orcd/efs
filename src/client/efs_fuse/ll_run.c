/* efs-fuse: low-level operation bodies (the *_run functions, read, readdir and friends). */
#include "fuse_internal.h"
#include "ll_run.h"
#include "support.h"
#include "getattr.h"
#include "open.h"
#include "append.h"
#include "write.h"
#include "namespace.h"
#include "release.h"
#include "plock_init.h"
#include "lookup.h"
#include "../reply_buffers.h"

void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse_entry_param e;
    int rc;
    struct deep_callback_sample path_sample = deep_callback_begin(req, name);
    t_lookup_memo_serial = lookup_memo_start();
    t_req = req;
    rc = efs_fuse_lookup_at(parent, name, &e);
    t_lookup_memo_serial = 0;
    t_req = NULL;
    deep_callback_reply_begin(&path_sample);
    if (rc)
        fuse_reply_err(req, -rc);
    else {
        fuse_reply_entry(req, &e);
        /* Also release lookup-only queries (stat has no RELEASE callback).
         * Queue after the reply; the notifier must not run inline while the
         * kernel holds the lookup's directory locks. */
        if (virt_kind(e.ino) == 3)
            ll_inval_entry(parent, name);
    }
    deep_callback_end(path_sample, "LOOKUP", parent, rc);
}

void ll_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
    if (virt_kind(ino) == 3)
        vq_nlookup_sub(ino, nlookup);
    fuse_reply_none(req);
}

void ll_forget_multi(fuse_req_t req, size_t count,
                            struct fuse_forget_data *forgets)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (virt_kind(forgets[i].ino) == 3)
            vq_nlookup_sub(forgets[i].ino, forgets[i].nlookup);
    }
    fuse_reply_none(req);
}

void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct stat st;
    int rc;
    struct deep_callback_sample path_sample = deep_callback_begin(req, NULL);
    t_req = req;
    rc = efs_fuse_getattr_ino(ino, &st, fi);
    t_req = NULL;
    deep_callback_reply_begin(&path_sample);
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_attr(req, &st, 0.0);
    deep_callback_end(path_sample, "GETATTR", ino, rc);
}

void ll_setattr_run(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
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

void ll_readlink(fuse_req_t req, fuse_ino_t ino)
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

void ll_mkdir_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode)
{
    efs_ino_t new_ino = 0;
    struct fuse_entry_param e;
    struct efs_inode created = {0};
    int rc;
    struct deep_callback_sample path_sample = deep_callback_begin(req, name);
    lookup_memo_mutation_begin();
    t_req = req;
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline) deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    rc = efs_fuse_mkdir_at(parent, name, mode, &new_ino, &created);
    if (rc == 0) {
        /* CREATE already returned the authoritative committed row. As for
         * regular CREATE, reply that exact identity and attributes rather
         * than starting another GETATTR after success. Zero cache timeouts
         * still require fresh authorization/attributes on later operations. */
        if (created.ino != new_ino || !efs_mode_is_dir(created.mode))
            rc = -EIO;
        else
            fill_entry_row((fuse_ino_t)new_ino, &e, &created);
    }
    efs_client_rpc_set_deadline_ms(previous);
    lookup_memo_mutation_end();
    t_req = NULL;
    deep_callback_reply_begin(&path_sample);
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_entry(req, &e);
    deep_callback_end(path_sample, "MKDIR", parent, rc);
}

void ll_unlink_run(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_unlink_at(parent, name);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

void ll_rmdir_run(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int rc;
    struct deep_callback_sample path_sample = deep_callback_begin(req, name);
    lookup_memo_mutation_begin();
    t_req = req;
    /* Same namespace callback budget as MKDIR, covering parent reads and
     * same-opid admission retries together. Never extend an inherited bound. */
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline) deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    rc = efs_fuse_rmdir_at(parent, name);
    efs_client_rpc_set_deadline_ms(previous);
    lookup_memo_mutation_end();
    t_req = NULL;
    deep_callback_reply_begin(&path_sample);
    fuse_reply_err(req, rc ? -rc : 0);
    deep_callback_end(path_sample, "RMDIR", parent, rc);
}

void ll_symlink_run(fuse_req_t req, const char *link, fuse_ino_t parent,
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

void ll_rename_run(fuse_req_t req, fuse_ino_t parent, const char *name,
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

void ll_link_run(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
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

void ll_open_run(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
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
    else if (fuse_reply_open(req, fi) != 0)
        (void)efs_fuse_release_ino(ino, fi);
}

/* Per-worker reply buffer. A malloc(1 MiB)/free per READ request sat
 * above glibc's mmap threshold on every call (fresh pages, kernel memset
 * and rmqueue in the read profile of a 20 GiB dd, Oct 1 2026); the
 * worker keeps one buffer sized to the largest request it has seen. */
#define LL_READ_BUF_MAX (16u << 20)

#define LL_READ_REFS_MAX 32
void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
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
                static int logged; /* once, from any reader thread */
                if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
                    fprintf(stderr, "efs: read reply zero-copy active (iov=%d size=%zu)\n", nr, size);
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

void ll_write_run(fuse_req_t req, fuse_ino_t ino, const char *buf,
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

void ll_write_buf_run(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv,
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

void ll_flush_run(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    rc = efs_fuse_flush_ino(ino, fi);
    lookup_memo_mutation_end();
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
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

void ll_fsync_run(fuse_req_t req, fuse_ino_t ino, int datasync,
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
void ll_fallocate_run(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset,
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
        fuse_reply_err(req, -efs_rc_to_errno(rc));
        return;
    }
    rc = efs_client_stat_refresh((efs_ino_t)ino, &row);
    if (rc != EFS_OK) {
        lookup_memo_mutation_end();
        t_req = NULL;
        fuse_reply_err(req, -fuse_stat_errno(rc));
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

void ll_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct efs_dirh *dh;
    int vk = virt_kind(ino);
    t_req = req;
    if (vk == 1 || vk == 3) {
        t_req = NULL;
        fuse_reply_err(req, ENOTDIR);
        return;
    }
    if (vk == 2 || vk == 4) {
        if (!commands_enabled()) {
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
            fuse_reply_err(req, -fuse_stat_errno(rc));
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

void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
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

void ll_releasedir(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
    (void)ino;
    if (fi && fi->fh)
        dirh_free((struct efs_dirh *)(uintptr_t)fi->fh);
    fuse_reply_err(req, 0);
}

void ll_statfs(fuse_req_t req, fuse_ino_t ino)
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

void ll_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    int rc;
    struct deep_callback_sample path_sample = deep_callback_begin(req, NULL);
    t_req = req;
    rc = efs_fuse_access_ino(ino, mask);
    t_req = NULL;
    deep_callback_reply_begin(&path_sample);
    fuse_reply_err(req, rc ? -rc : 0);
    deep_callback_end(path_sample, "ACCESS", ino, rc);
}

/* nfsd creates through vfs_create, which the FUSE kernel module turns
 * into FUSE_MKNOD, not FUSE_CREATE (only open(O_CREAT) uses the atomic
 * create). With no .mknod handler libfuse replied ENOSYS and the NFS
 * client saw EIO for every create. Regular files reuse the create path;
 * there is no open here, so no fh/open-note bookkeeping. */
void ll_mknod_run(fuse_req_t req, fuse_ino_t parent, const char *name,
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
        rc = efs_fuse_create_at(parent, name, mode, NULL, &new_ino, NULL);
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

void ll_create_run(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, struct fuse_file_info *fi)
{
    struct fuse_entry_param e;
    int rc;
    lookup_memo_mutation_begin();
    t_req = req;
    struct efs_inode created;
    rc = efs_fuse_create_at(parent, name, mode, fi, NULL, &created);
    int opened = (rc == 0);
    if (rc == 0 && created.ino) {
        /* CREATE already returned this committed row. A fresh GETATTR here
         * adds a quorum read solely to construct the same syscall's reply.
         * Keep zero attribute/entry TTLs and do not populate the lookup memo:
         * subsequent fstat and peer mutations still use their normal path. */
        struct stat st;
        fill_stat_from_inode(&st, &created);
        fill_entry((fuse_ino_t)created.ino, &e, &st);
    } else if (rc == 0) {
        /* O_CREAT without O_EXCL may have opened an existing file. */
        rc = lookup_fill((fuse_ino_t)efs_file_ino(fi, 0), &e, fi);
    }
    lookup_memo_mutation_end();
    t_req = NULL;
    if (rc) {
        /* A successful open followed by failed reply construction has no
         * kernel descriptor and therefore will never receive RELEASE. */
        if (opened)
            (void)efs_fuse_release_ino(0, fi);
        fuse_reply_err(req, -rc);
    } else if (fuse_reply_create(req, &e, fi) != 0) {
        /* Interrupted requests may reject the reply after CREATE/HOLD has
         * committed. Keep the namespace effect, release only our open ref. */
        (void)efs_fuse_release_ino(e.ino, fi);
    }
}

void ll_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
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

void ll_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     struct flock *lock, int sleep)
{
    int rc;
    t_req = req;
    rc = efs_fuse_setlk_ino(ino, fi, lock, sleep);
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

void ll_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                     int op)
{
    int rc;
    t_req = req;
    rc = efs_fuse_flock_ino(ino, fi, op);
    t_req = NULL;
    fuse_reply_err(req, rc ? -rc : 0);
}

void ll_lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
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
    if (n == 5)
        return -EINVAL;
    return 0;
}

void ll_setxattr_run(fuse_req_t req, fuse_ino_t ino, const char *name,
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

void ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
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

void ll_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
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

void ll_removexattr_run(fuse_req_t req, fuse_ino_t ino, const char *name)
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
