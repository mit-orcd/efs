/* efs-fuse: release, control drain, destroy, rename, lseek. */
#include "fuse_internal.h"
#include "support.h"
#include "open.h"

int efs_fuse_release_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    efs_ino_t fh = efs_file_ino(fi, (efs_ino_t)ino);
    if (virt_kind(fh)) { efs_file_close(fi); return 0; }
    int leased = 0;
    int lock_rc = EFS_OK;
    efs_client_note_meta_change(0);
    if (!fh) { efs_file_close(fi); return 0; }
    pthread_mutex_t *edge = open_edge_mu(fh);
    pthread_mutex_lock(edge);
    /* The kernel supplies this open description's valid owner at final
     * RELEASE. Other opens of the inode must neither delay its unlock nor
     * select a different owner's lock. Serialize with the existing OPEN edge. */
    if (fi && fi->flock_release) {
        uint64_t owner = g_client.flock_token ^ fi->lock_owner;
        lock_rc = efs_client_rpc_flock(g_client.export_id, fh,
                                       EFS_FLOCK_UN, owner);
        if (lock_rc != EFS_OK)
            efs_fuse_log_err("release-flock", lock_rc, fh, 0, 0, NULL);
    }
    efs_file_close(fi);
    if (efs_close_note(fh, &leased)) {
        if (leased) {
            int rc = efs_open_release_pending(fh);
            /* Identity admission is a 16-position window shared by every
             * thread; a momentarily full window is not a refusal. Wait for a
             * position like OPEN does, outside the stripe (an older owner on
             * a colliding stripe may need it to finish). Giving up left the
             * release to the background drain, and under a busy open/close
             * stream retained rows grew without bound (Oct 16 xorinox: +40/s,
             * 23,455 rows). A reopen meanwhile clears close_pending. */
            for (unsigned spin = 0; rc == EFS_ERR_BUSY && spin < CLOSE_ADMIT_SPINS &&
                 !efs_client_rpc_past_deadline() && efs_open_close_unadmitted(fh); spin++) {
                pthread_mutex_unlock(edge);
                efs_client_opid_wait(1);
                pthread_mutex_lock(edge);
                rc = efs_open_release_pending(fh);
            }
        }
        efs_client_stage_evict_ino(fh);
    }
    pthread_mutex_unlock(edge);
    return efs_rc_to_errno(lock_rc);
}

/* Runs before unmount with mutations quiesced. Timeout of the control
 * request does not cancel or free an accepted background job. */
int efs_control_drain(uint64_t deadline, int force)
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
        uint64_t ownership_deadline = stats_now_ms() + 1000;
        if (deadline < ownership_deadline)
            ownership_deadline = deadline;
        int ownership_rc = efs_open_retry_pass(ownership_deadline);
        int data_rc = efs_dcache_drain_pending();
        int report_rc = efs_client_report_dirty(1);
        efs_buf_drain_leave();
        struct efs_report_pressure pressure;
        efs_client_report_pressure_stats(&pressure);
        if (data_rc == EFS_OK && report_rc == EFS_OK && ownership_rc == EFS_OK &&
            !pressure.pending_records && !pressure.reserved_records &&
            !pressure.inflight_records &&
            !efs_dcache_pending_records(NULL)) {
            result = 0;
            break;
        }
        usleep(10000);
    }
    if (result)
        fprintf(stderr, "stop-refused: unresolved writes or lease releases remain; mount retained\n");
    efs_client_rpc_set_deadline_ms(previous);
    return result;
}


int efs_fuse_rename_at(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags, efs_ino_t *out_src)
{
    struct efs_inode src, src_parent, dst_parent;
    int rc;
    /* Legacy rename has no atomic flags contract. Never silently turn an
     * exchange, no-replace or whiteout request into a destructive rename. */
    if (flags != 0)
        return -EOPNOTSUPP;
    if (virt_kind(parent) || virt_kind(newparent))
        return -EACCES;
    rc = efs_client_stat_refresh((efs_ino_t)parent, &src_parent);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    int wx = check_dir_wx(&src_parent);
    if (wx != 0)
        return wx;
    rc = efs_client_rpc_lookup(g_client.export_id, (efs_ino_t)parent, name, &src);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
    rc = efs_client_stat_refresh((efs_ino_t)newparent, &dst_parent);
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

static off_t inode_seek_data_hole(const struct efs_inode *ino, off_t off, int whence)
{
    if (off < 0)
        return -EINVAL;
    if ((uint64_t)off >= ino->size) {
        if (whence == SEEK_HOLE)
            return (off_t)ino->size;
        return -ENXIO;
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

off_t efs_fuse_lseek_ino(fuse_ino_t ino, off_t off, int whence,
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
