/* efs-fuse: write admission, store, buf, fsync and flush. */
#include "fuse_internal.h"
#include "support.h"
#include "open.h"
#include "writeback.h"

/* Reserve worst-case chunk capacity before any append range is accepted.
 * Capacity pressure drains bounded PUT/REPORT cohorts, never future writes
 * through the full fsync path. Cycle the same inode when its publication
 * unit needs several cohorts, within the existing sixteen-round bound.
 * Failure retains owned bytes and rejects this request before acceptance. */
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
    int early_drain = admission == 0;
    if (admission == 0 &&
        (!efs_buf_should_drain() || fuse_pressure_kick()))
        return 0;
    if (admission == 0) {
        efs_buf_unreserve();
        admission = EFS_ERR_BUSY;
    }
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
            early_drain ? "high-water" :
            meta + meta_reserved + chunks * EFS_DCACHE_ENTRY_BUDGET > meta_limit
                ? "metadata" : "body");
    efs_rdcache_trim();
    efs_dcache_trim_metadata();
    admission = efs_buf_reserve_request(bytes, chunks * EFS_DCACHE_ENTRY_BUDGET);
    if (admission == 0) {
        if (!efs_buf_should_drain() || fuse_pressure_kick() || !efs_dcache_pressure_ino(0))
            return 0;
        efs_buf_unreserve();
        admission = EFS_ERR_BUSY;
    }
    if (admission != EFS_ERR_BUSY)
        return -ENOMEM;
    /* Finite drain work, no cache/append mutex held here.
     * This is a pressure drain, not D27's whole-call recovery protocol. */
    for (int i = 0; i < 16; i++) {
        if (efs_client_rpc_past_deadline())
            return -EAGAIN;
        efs_ino_t ino = efs_dcache_pressure_ino(after);
        int rc;
        if (!ino && after) {
            after = 0;
            ino = efs_dcache_pressure_ino(0);
        }
        if (!ino)
            break;
        after = ino;
        efs_buf_drain_enter();
        rc = fuse_pressure_flush(ino);
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

static int efs_fuse_write_store(const char *path, const char *buf, size_t size,
                                off_t offset, efs_ino_t ino, int append);
static int efs_fuse_write_buf_store(struct fuse_bufvec *buf, size_t size,
                                    off_t offset, efs_ino_t ino, int append);

/* W77: one FUSE write request is one atomic publication unit. When joining
 * the units it overlaps would exceed EFS_PUB_UNIT_MAX chunks, publish this
 * file's pending units first (an O_APPEND caller already holds append_mu). */
static int fuse_unit_begin(efs_ino_t ino, uint64_t offset, size_t size, int append_held)
{
    for (unsigned round = 0; ; round++) {
        int rc = efs_client_unit_begin(ino, offset, size);
        if (rc != EFS_ERR_AGAIN)
            return rc == EFS_OK ? 0 : fuse_stat_errno(rc);
        if (round == 8)
            return -EBUSY;
        if (append_held) {
            rc = efs_wb_sync_ino(ino);
            if (rc == EFS_OK)
                rc = efs_dcache_flush_ino(ino);
            if (rc == EFS_OK)
                rc = efs_client_report_dirty_ino(ino, 1);
        } else {
            rc = efs_append_flush_report(NULL, ino);
        }
        if (rc != EFS_OK)
            return fuse_stat_errno(rc);
    }
}

static int efs_fuse_write_admitted_run(const char *path, const char *buf, size_t size,
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
        int publish_rc = append_publish_burst(ino);
        if (publish_rc != EFS_OK)return fuse_stat_errno(publish_rc);
        int lock_rc = wb_lock_budget(append_mu(ino));
        if (lock_rc != EFS_OK)
            return fuse_stat_errno(lock_rc);
        rc = append_reserve_offset(ino, (uint64_t)size, &offset);
        if (rc != 0)return rc; /* reserve failure already released stripe */
    }
    rc = fuse_unit_begin(ino, (uint64_t)offset, size, append);
    if (rc != 0) {
        if (append) {
            efs_client_stage_unpin(ino);
            pthread_mutex_unlock(append_mu(ino));
        }
        return rc;
    }
    rc = efs_fuse_write_store(path, buf, size, offset, ino, append);
    efs_client_unit_end(ino, (uint64_t)offset, size);
    return rc;
}

/* The request's bytes into the dcache or the writeback pool. With O_APPEND
 * the caller holds append_mu and the stage pin; every path releases them. */
static int efs_fuse_write_store(const char *path, const char *buf, size_t size,
                                off_t offset, efs_ino_t ino, int append)
{
    int rc;
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

static int efs_fuse_write_admitted(const char *path,const char *buf,size_t size,off_t offset,struct fuse_file_info *fi)
{
    return efs_fuse_write_admitted_run(path,buf,size,offset,fi);
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

int efs_fuse_fsync_ino(fuse_ino_t ino, int isdatasync,
                              struct fuse_file_info *fi);

/* Kernel follow-up fsync behavior is not the write callback's contract.
 * Finish publication before acknowledging an explicitly synchronous write,
 * after write admission credits are released and within its inherited budget. */
static int efs_fuse_sync_write_result(int written, struct fuse_file_info *fi)
{
    if (written <= 0 || !fi || !(fi->flags & (O_SYNC | O_DSYNC)))
        return written;
    int datasync = (fi->flags & O_SYNC) != O_SYNC;
    int rc = efs_fuse_fsync_ino(0, datasync, fi);
    if (efs_dcache_trace_on())
        fprintf(stderr, "sync-write ino=%llu bytes=%d datasync=%d rc=%d\n",
                (unsigned long long)efs_file_ino(fi, 0), written, datasync, rc);
    return rc ? rc : written;
}

int efs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_client_rpc_past_deadline() ? -EAGAIN : efs_fuse_write_run(path, buf, size, offset, fi);
    rc = efs_fuse_sync_write_result(rc, fi);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

/* write_buf: copy the FUSE receive buffer. Small writes still try an
 * in-place dcache patch; large writes go to the writeback pool. */
static int efs_fuse_write_buf_admitted_run(const char *path, struct fuse_bufvec *buf,
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
        int publish_rc = append_publish_burst(ino);
        if (publish_rc != EFS_OK)return fuse_stat_errno(publish_rc);
        int lock_rc = wb_lock_budget(append_mu(ino));
        if (lock_rc != EFS_OK)
            return fuse_stat_errno(lock_rc);
        rc = append_reserve_offset(ino, (uint64_t)size, &offset);
        if (rc != 0)return rc; /* reserve failure already released stripe */
    }
    rc = fuse_unit_begin(ino, (uint64_t)offset, size, append);
    if (rc != 0) {
        if (append) {
            efs_client_stage_unpin(ino);
            pthread_mutex_unlock(append_mu(ino));
        }
        return rc;
    }
    rc = efs_fuse_write_buf_store(buf, size, offset, ino, append);
    efs_client_unit_end(ino, (uint64_t)offset, size);
    return rc;
}

/* write_buf's bytes into the dcache or the writeback pool; O_APPEND state
 * as for efs_fuse_write_store. */
static int efs_fuse_write_buf_store(struct fuse_bufvec *buf, size_t size,
                                    off_t offset, efs_ino_t ino, int append)
{
    int rc = 0;

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

static int efs_fuse_write_buf_admitted(const char *path,struct fuse_bufvec *buf,off_t offset,struct fuse_file_info *fi)
{
    return efs_fuse_write_buf_admitted_run(path,buf,offset,fi);
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

int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
                              off_t offset, struct fuse_file_info *fi)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_client_rpc_past_deadline() ? -EAGAIN : efs_fuse_write_buf_run(path, buf, offset, fi);
    rc = efs_fuse_sync_write_result(rc, fi);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

int efs_fuse_fsync_ino(fuse_ino_t ino, int isdatasync,
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
    /* Published is not durable when servers acknowledge asynchronously.
     * K35 (D42): prove each stripe this client wrote to the file, COMMIT
     * on the nodes holding its fragments only; with one of them away the
     * stripe is marked degraded before success, with two away EIO. */
    if (rc == EFS_OK)
        rc = efs_client_data_commit(file);
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

int efs_fuse_flush_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    /* W2: last close is durable+visible. Kick-only left size/chunk-map
     * unpublished so the next O_APPEND used i_size 0 and wiped the
     * prefix (fcntl_setfl_oappend, hardlink_shared_data).
     * The inode's append stripe covers the report too; see
     * efs_append_flush_report.
     * fi->flags is not O_APPEND on this path. */
    efs_ino_t file = efs_file_ino(fi, (efs_ino_t)ino);
    if (virt_kind(file)) return 0;
    int was_stalled = efs_wb_inode_stalled(file);
    int rc = efs_append_flush_report(fi, file);
    int report_rc = rc;
    rc = efs_file_sync_error(fi, file, was_stalled, rc);
    if (rc != EFS_OK) {
        /* G5: which stage failed: the flush+REPORT (report-loop and
         * report-resolve name it) or a stalled write-back (wb). */
        fprintf(stderr, "flush-meta stage=%s ino=%llu report_rc=%d rc=%d stalled=%d\n",
                report_rc != EFS_OK ? "report" : "writeback", (unsigned long long)file,
                report_rc, rc, was_stalled);
        efs_fuse_log_err("flush-meta", rc, ino, 0, 0, NULL);
        return rc == EFS_ERR_QUOTA ? -ENOSPC : -EIO;
    }
    /* FLUSH also runs when one dup/fork copy is closed. Flock belongs to
     * the open file description; only RELEASE's flock_release flag permits
     * dropping it. Releasing here lets a child close its inherited copy and
     * silently unlock the descriptor its parent still holds. */
    return 0;
}
