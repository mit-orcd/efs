#include "efs_fuse/fuse_internal.h"
#include "commands/source.h"
#include "reply_buffers.h"
#include "stop_control.h"

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

static int efs_fuse_fsync_ino(fuse_ino_t ino, int isdatasync,
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

static int efs_fuse_write(const char *path, const char *buf, size_t size,
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

static int efs_fuse_write_buf(const char *path, struct fuse_bufvec *buf,
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

static int efs_fuse_flush_ino(fuse_ino_t ino, struct fuse_file_info *fi)
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

static int efs_fuse_create_at(fuse_ino_t parent_ino, const char *name,
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

static int efs_fuse_mkdir_at(fuse_ino_t parent_ino, const char *name, mode_t mode,
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

static int efs_fuse_unlink_at(fuse_ino_t parent_ino, const char *name)
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

static int efs_fuse_rmdir_at(fuse_ino_t parent_ino, const char *name)
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

static int efs_fuse_statfs(const char *path, struct statvfs *stbuf)
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
    rc = efs_client_stat_refresh((efs_ino_t)ino, &row);
    if (rc != EFS_OK)
        return fuse_stat_errno(rc);
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


static int efs_fuse_rename_at(fuse_ino_t parent, const char *name,
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
        return 0;
    }
}

static int efs_fuse_flock_ino(fuse_ino_t ino, struct fuse_file_info *fi, int op)
{
    efs_ino_t fh = efs_file_ino(fi, (efs_ino_t)ino);
    if (!fi) return -EBADF;
    uint64_t owner = g_client.flock_token ^ fi->lock_owner;
    uint32_t rpc_op = (uint32_t)op;
    if (!(op & (LOCK_UN | LOCK_NB))) rpc_op |= EFS_FLOCK_WAIT;
    int rc;
    if (getenv("EFS_FLOCK_DBG"))
        fprintf(stderr, "flock: ino=%llu op=%u owner=%llu\n",
                (unsigned long long)fh, op, (unsigned long long)owner);
    rc = efs_client_rpc_flock(g_client.export_id, fh, rpc_op, owner);
    if (rc == EFS_ERR_BUSY || rc == EFS_ERR_AGAIN)
        return -EAGAIN;
    return efs_rc_to_errno(rc);
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
        ssize_t written;
        do { written = write(fd, &c, 1); }
        while (written < 0 && errno == EINTR);
        if (written != 1)
            fprintf(stderr, "fuse readiness notification failed: %s\n",
                    written < 0 ? strerror(errno) : "short write");
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
    efs_open_retry_start();
    (void)efs_client_view_start();
    /* Timeouts are per lookup/getattr reply (always 0). There is no
     * fuse_config on the low-level API. */
    if (conn) {
        /* libfuse requires this to match the max_read mount option. */
        conn->max_read = 512u << 10;
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
        /* Off, with no switch (D42). W15.5 asked for splice reads (Sep 29)
         * and the first profile with it (fstor007 ecopy) showed no gain:
         * libfuse splices /dev/fuse into a pipe and the write handler's
         * fuse_buf_copy then read()s the pipe into the dcache buffer, the
         * same kernel->user copy read(/dev/fuse) does, plus one splice per
         * request (24 668 in 67 s, 17 072 of them header-sized). */
        conn->want &= ~FUSE_CAP_SPLICE_READ;
#endif
#ifdef FUSE_CAP_EXPORT_SUPPORT
        /* No NFS re-export (D42, W152 out of alpha): the kernel never
         * builds file handles for an efs mount. */
        conn->want &= ~FUSE_CAP_EXPORT_SUPPORT;
#endif
        if (conn->congestion_threshold < 96)
            conn->congestion_threshold = 96;
    }
    efs_client_stage_set_pin_hooks(efs_client_ino_is_open,
                                   efs_client_ino_has_plock);
    efs_client_enable_meta_batch(4096);
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
 * no open fh refreshes strictly; a transient follow-up GETATTR failure must
 * not turn that known committed mutation into a failed creation reply.
 * Its local row is a reply fallback, never authority for a new operation. */
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

/* Explicit bounded deep-path diagnostics. Normal callbacks do not read a
 * clock or write a line. This records callback service, not kernel queue time;
 * join caller PID/monotonic time to the transparent syscall observer. */
static int deep_callback_name(const char *name)
{
    return name && name[0] == 'd' && name[1] >= '0' && name[1] <= '9' &&
           name[2] >= '0' && name[2] <= '9' && name[3] == '\0';
}


static struct deep_callback_sample deep_callback_begin(fuse_req_t req, const char *name)
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
static void deep_callback_reply_begin(struct deep_callback_sample *sample)
{
    if (!sample->begin)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sample->service_end = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void deep_callback_end(struct deep_callback_sample sample, const char *op,
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

static void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
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
    else if (fuse_reply_open(req, fi) != 0)
        (void)efs_fuse_release_ino(ino, fi);
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

static void ll_create_run(fuse_req_t req, fuse_ino_t parent, const char *name,
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
    if (n == 5)
        return -EINVAL;
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

/* Nonzero once a clean unmount failed to flush, report, settle or revoke:
 * efs-fuse then exits 1, never reporting a clean completion (D42). The
 * first reason goes into the stop record. */
static int g_unmount_unclean;
static char g_unclean_reason[192];

static void unmount_unclean(const char *reason)
{
    if (!g_unmount_unclean && reason && *reason)
        snprintf(g_unclean_reason, sizeof(g_unclean_reason), "%s", reason);
    g_unmount_unclean = 1;
}

static void efs_fuse_destroy(void *userdata)
{
    (void)userdata;
    efs_client_set_stop_observer(efs_stop_phase);
    efs_stop_phase("flushing", 0, 0);
    fuse_pressure_stop();
    int wrc = efs_wb_sync();
    int drc = efs_dcache_flush_all();
    if (wrc != EFS_OK || drc != EFS_OK) {
        char why[96];
        fprintf(stderr, "efs-fuse: unmount: data flush incomplete "
                        "(writeback=%d dcache=%d)\n", wrc, drc);
        snprintf(why, sizeof(why), "data flush writeback=%d dcache=%d", wrc, drc);
        unmount_unclean(why);
    }
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
        for (uint64_t attempt = 1;; attempt++) {
            efs_stop_phase("draining", attempt, 0);
            mrc = efs_client_note_meta_change(1);
            if (mrc == EFS_OK)
                break;
            /* A fenced session can never publish again (K5/I23): waiting
             * would only delay the unmount. Report the loss once. */
            if (mrc == EFS_ERR_FENCED || efs_client_session_fenced()) {
                fprintf(stderr, "efs-fuse: mount session fenced; writes that "
                                "were never reported are discarded\n");
                mrc = EFS_OK;
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= drain_s)
                break;
            usleep(200000);
        }
        if (mrc != EFS_OK) {
            char why[96];
            fprintf(stderr, "efs-fuse: UNMOUNT DATA LOSS: metadata flush "
                            "still failing (rc=%d) after %lds; writes that "
                            "were never reported are gone\n", mrc, drain_s);
            snprintf(why, sizeof(why), "metadata report rc=%d after %lds", mrc, drain_s);
            unmount_unclean(why);
        }
    }
    /* Free cached .find query results. */
    pthread_mutex_lock(&g_find_res_mu);
    for (int i = 0; i < EFS_FIND_RES_N; i++) {
        free(g_find_res[i].text);
        g_find_res[i].text = NULL;
        g_find_res[i].valid = 0;
    }
    pthread_mutex_unlock(&g_find_res_mu);
    efs_open_retry_stop();
    efs_client_view_stop();
    if (efs_client_shutdown() != EFS_OK)
        unmount_unclean(efs_client_shutdown_reason());
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
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    ll_symlink_run(req, link, parent, name);
    efs_client_rpc_set_deadline_ms(previous);
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
    efs_client_map_opened((efs_ino_t)ino);
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
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    ll_create_run(req, parent, name, mode, fi);
    efs_client_rpc_set_deadline_ms(previous);
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
    return efs_parse_host_port(str, host, host_len, port);
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
    if (node >= 0)
        efs_setting_u64("EFS_NUMA_NODE", (unsigned)node);
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

static pid_t g_perf_pid = -1;
static int g_perf_want;           /* --perf given; start after the fork */
static char g_perf_path[512];

/* --strace: `strace -f -tt -T -o <path> -p <pid>` on the daemon. ptrace
 * stops every syscall of every thread, so this is for one client and a
 * wall question (where did an fsync's 8 s go), never for a bandwidth
 * number. Needs kernel.yama.ptrace_scope=0 (runtime on the test hosts).
 * EFS_STRACE_EXPR narrows it, e.g. "trace=fsync,writev,recvfrom,futex"
 * (passed as -e). */
static pid_t g_strace_pid = -1;
static int g_strace_want;         /* --strace given; start after the fork */
static char g_strace_path[512];

/* SIGTERM makes perf finalize perf.data and strace detach; wait up to
 * ~5 s, then SIGKILL. */
static void stop_recorder(pid_t *pidp)
{
    efs_stop_recorder(pidp);
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
/* W125: refuse to mount a cluster whose daemons do not accept this
 * client's protocol declaration, instead of mounting and then failing
 * every read. A node that refuses is incompatible; one that does not
 * answer within 3 s is counted as down. At least one node must accept
 * and none may refuse. */
static int protocol_check(void)
{
    uint32_t accepted = 0, refused = 0;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        int fd = efs_connect_tcp(g_client.nodes[i].addr, g_client.nodes[i].port);
        if (fd < 0)
            continue;
        efs_set_recv_timeout(fd, 3000);
        efs_set_send_timeout(fd, 3000);
        int rc = efs_fd_declare(fd);
        close(fd);
        if (rc == EFS_OK) {
            accepted++;
        } else if (rc != EFS_ERR_NET) {
            refused++;
            fprintf(stderr, "node %s:%u refused protocol %u (rc=%d)\n", g_client.nodes[i].addr,
                    g_client.nodes[i].port, EFS_PROTOCOL_VERSION, rc);
        } else {
            fprintf(stderr, "node %s:%u did not answer the protocol %u declaration "
                    "(older daemon or down)\n", g_client.nodes[i].addr,
                    g_client.nodes[i].port, EFS_PROTOCOL_VERSION);
        }
    }
    if (!accepted || refused) {
        fprintf(stderr, "Cluster does not support client protocol %u: upgrade the daemons "
                "before this client\n", EFS_PROTOCOL_VERSION);
        return EFS_ERR_PROTO;
    }
    return EFS_OK;
}

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
            if (efs_fd_declare(fd) != EFS_OK) { /* W133: declared first */
                close(fd);
                continue;
            }
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

/* A6/F1: libfuse and RPC workers share four malloc arenas, independently
 * of CPU count and whether the launcher was used. Per-core glibc defaults
 * retained tens of MiB of free holes across 74+ arenas under fixed-set
 * rewrites on large hosts. This is process policy, never a mode switch.
 * Keep glibc's initial large-allocation threshold fixed as well: otherwise
 * freeing a large transfer can raise it to MiB, putting later transient
 * frames into retained heap holes despite the bounded arena count. This
 * preserves malloc/free ownership while letting large buffers return their
 * pages on free. Other allocators do not expose these glibc policies:
 * an ASan build replaces malloc and its mallopt always fails, so the policy
 * is skipped there (K56) and stays fatal in every glibc build.
 * shortcut: relies on gcc's __SANITIZE_ADDRESS__ (the sanitizer gate's
 * compiler); a clang ASan build needs a __has_feature test. */
static int fuse_allocator_init(void)
{
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
    if (!mallopt(M_ARENA_MAX, 4) || !mallopt(M_MMAP_THRESHOLD, 128 * 1024))
        return -1;
#endif
    return 0;
}

static size_t client_settings_text(char *buf, size_t size)
{
    int rows, cut;
    return efs_settings_render(buf, size, &rows, &cut);
}

int main(int argc, char **argv)
{
    /* Before transport, client pools or FUSE start any worker threads. */
    if (fuse_allocator_init()) {
        fprintf(stderr, "efs-fuse: cannot establish allocator policy\n");
        return 1;
    }
    if (argc==2 && !strcmp(argv[1],"--help-fuse")) {
        puts("efs-fuse - FUSE option reference\n");
        fuse_cmdline_help();fuse_lowlevel_help();return 0;
    }
    if (efs_help_requested(argc,argv)) { efs_cli_help(stdout,argv[0],NULL);return 0; }
    if (efs_env_removed_refuse("efs-fuse"))
        return 1;
    /* F5: the in-process client benchmark left the product binary; refuse
     * its flags rather than pass them to FUSE as mount options. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench") || !strcmp(argv[i], "--bench-time") ||
            !strcmp(argv[i], "--bench-mib")) {
            fprintf(stderr, "efs-fuse: %s was removed; measure the client with efs-bench "
                    "or through a mount\n", argv[i]);
            return 2;
        }
    }
#if !EFS_FAULTS
    efs_faults_env_ignored("efs-fuse");
#endif
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
    efs_settings_init(EFS_SETTINGS_CLIENT);
    if (efs_transport_startup("efs-fuse") != EFS_OK)
        return 1;

    int perf = 0, strace_opt = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--perf") == 0)
            perf = 1;
        else if (strcmp(argv[i], "--strace") == 0)
            strace_opt = 1;
    }

    if (argc < 3) {
        efs_cli_help(stderr,argv[0],NULL);
        return 1;
    }

    /* Server addresses are the first arguments (they contain a colon);
     * the first argument without a colon is the mountpoint. There is one
     * export and no export label argument. */
    int arg_idx = 1;
    const char *nodes[EFS_MAX_NODES];
    uint32_t node_count = 0;
    while (arg_idx < argc && strchr(argv[arg_idx], ':') != NULL &&
           node_count < EFS_MAX_NODES) {
        nodes[node_count++] = argv[arg_idx++];
    }

    if (node_count == 0 || arg_idx >= argc) {
        fprintf(stderr, "Missing mountpoint\n");
        return 1;
    }
    const char *mountpoint = argv[arg_idx++];
    /* Options follow the mountpoint. A second positional is the removed
     * export-label form (NODE LABEL MOUNTPOINT): refuse it, never guess. */
    if (arg_idx < argc && argv[arg_idx][0] != '-') {
        fprintf(stderr, "efs-fuse: unexpected argument '%s' after mountpoint '%s'.\n"
                "Usage: efs-fuse NODE:PORT [NODE:PORT ...] MOUNTPOINT [options]\n"
                "The export label argument was removed; there is one export.\n",
                argv[arg_idx], mountpoint);
        return 1;
    }
    const char *export_name = "default";

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
    efs_client_setup_mount_token();

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
    if (protocol_check() != EFS_OK)
        return 1;
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
    /* K33: the committed record this mount starts from; the serving process
     * installs later revisions without a remount (efs_client_view_start). */
    if (efs_client_view_mount() != EFS_OK)
        return 1;
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
    /* K5/I23: the mount session is established after the daemon fork, in
     * the process that serves the mount (K31): over RDMA the session RPC
     * starts the device's completion poller thread, and fork() would leave
     * the serving daemon without it. */
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

    int ret = efs_fuse_main_mt(fuse_argc, fuse_argv);
    stop_perf_recorder();
    /* destroy() already shut down on clean unmount; a repeat returns its
     * result. A failed flush, settle or revoke is not a clean exit. */
    efs_client_set_stop_observer(efs_stop_phase);
    if (efs_client_shutdown() != EFS_OK)
        unmount_unclean(efs_client_shutdown_reason());
    if (!ret && g_unmount_unclean) {
        fprintf(stderr, "efs-fuse: unmount did not complete cleanly; exit 1\n");
        ret = 1;
    }
    /* The stop record's last word: the stop tooling reports this verdict. */
    if (ret) {
        char why[sizeof(g_unclean_reason) + 32];
        snprintf(why, sizeof(why), "%s", g_unclean_reason[0] ? g_unclean_reason : "");
        if (!why[0])
            snprintf(why, sizeof(why), "efs-fuse exit %d", ret);
        efs_stop_status("failed", 0, 0, ret, why);
    } else
        efs_stop_status("done", 0, 0, 0, NULL);
    return ret;
}

/* libfuse supplies max_threads=10 and an unset idle sentinel itself,
 * so zero-value fallbacks cannot establish our bounded defaults. Insert them
 * first; later explicit user options retain ordinary last-option precedence. */
static int efs_fuse_parse_cmdline(struct fuse_args *args,
                                  struct fuse_cmdline_opts *opts)
{
    if(!args || args->argc<1 || !args->argv)return -1;
    struct fuse_args configured=FUSE_ARGS_INIT(0,NULL);
    for(int i=0;i<args->argc;i++) {
        if(fuse_opt_add_arg(&configured,args->argv[i])!=0)goto failed;
        /* Linux's default 256-page limit includes the user's first-page
         * offset. A 1 MiB malloc buffer at offset 16 otherwise becomes
         * 1 MiB-16 plus 16 bytes. Half-MiB requests fit with page headroom
         * and divide ordinary 1 MiB reads into chunk-aligned requests.
         * max_read is a mount option and must match conn->max_read in init. */
        if(i==0 && fuse_opt_add_arg(&configured,
            "-omax_read=524288,max_threads=32,max_idle_threads=8")!=0)goto failed;
    }
    fuse_opt_free_args(args);
    *args=configured;
    return fuse_parse_cmdline(args,opts);
failed:
    fuse_opt_free_args(&configured);
    return -1;
}

static int efs_fuse_main_mt(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_cmdline_opts opts;
    struct fuse_loop_config *config = NULL;
    int ret = -1;

    if (efs_fuse_parse_cmdline(&args, &opts) != 0)
        return 1;

    if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        ret = 0;
        goto out;
    }

    if (opts.show_help) {
        efs_cli_help(stdout,"efs-fuse",NULL);
        ret = 0;
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
    /* K5/I23: one session per mount, committed on its owner shard before
     * the first mutation. Without it every mutation would be refused, so
     * fail the mount instead of serving a read-only surprise. K31: here,
     * after the daemon fork, so the RDMA poller it may start belongs to the
     * serving process. A failure exits the child before FUSE_INIT; the
     * waiting parent reports it. */
    int session_rc = efs_client_session_start();
    if (session_rc != EFS_OK) {
        fprintf(stderr, "Could not establish the mount session (%s).\n",
                efs_strerror(session_rc));
        fflush(stderr);
        ret = 1;
        goto out;
    }
    {
        char hex[2 * EFS_OPID_UUID_LEN + 1];
        for (unsigned i = 0; i < EFS_OPID_UUID_LEN; i++)
            snprintf(hex + 2 * i, 3, "%02x", g_client.opid_uuid[i]);
        printf("session uuid=%s epoch=%u\n", hex, g_client.opid_epoch);
    }
    fflush(stdout);

    /* This is the process that serves the mount (the child, or the only
     * process under -f). Attach the recorder to it; the parent above has
     * already been excluded. */
    if (g_perf_want) {
        g_perf_pid = efs_start_perf_recorder(getpid(), g_perf_path);
        if (g_perf_pid < 0)
            fprintf(stderr, "Warning: could not start perf recorder; continuing without profiling\n");
        else
            fprintf(stderr, "efs-fuse perf recorder pid=%d -> %s\n", (int)g_perf_pid, g_perf_path);
        fflush(stderr);
    }
    if (g_strace_want) {
        g_strace_pid = efs_start_strace_recorder(getpid(), g_strace_path);
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

    efs_stop_settings_text = client_settings_text;
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
