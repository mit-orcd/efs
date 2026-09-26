/* The Raft log file: record framing, group commit, replay, rotation.
 *
 * One file per node carries every group's records. The only thing that makes
 * that safe is that each record names its group and the file is replayed in
 * order, so a group's state is a fold over its own records. */

#include "raft_disk_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p + 4, (uint32_t)v);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_u64(const uint8_t *p)
{
    return ((uint64_t)get_u32(p) << 32) | (uint64_t)get_u32(p + 4);
}

/* Serialize one record into d->rec; returns total length, 0 if too big. */
static uint32_t rec_encode(struct efs_raft_disk *d, uint8_t type,
                           uint32_t group, uint64_t a, uint64_t b,
                           const uint8_t *cmd, uint32_t clen)
{
    uint8_t *p = d->rec;
    uint32_t off = 8;

    p[off++] = type;
    put_u32(p + off, group);
    off += 4;
    switch (type) {
    case RAFT_REC_HARD:
        put_u64(p + off, a);
        off += 8;
        put_u32(p + off, (uint32_t)b);
        off += 4;
        break;
    case RAFT_REC_ENTRY:
        if (clen > RAFT_CMD_MAX)
            return 0;
        put_u64(p + off, a);
        off += 8;
        put_u64(p + off, b);
        off += 8;
        put_u32(p + off, clen);
        off += 4;
        if (clen) {
            memcpy(p + off, cmd, clen);
            off += clen;
        }
        break;
    case RAFT_REC_TRUNC:
        put_u64(p + off, a);
        off += 8;
        break;
    case RAFT_REC_SNAP:
        put_u64(p + off, a);
        off += 8;
        put_u64(p + off, b);
        off += 8;
        break;
    case RAFT_REC_CFG:
        put_u32(p + off, (uint32_t)a);
        off += 4;
        put_u32(p + off, (uint32_t)b);
        off += 4;
        break;
    default:
        return 0;
    }
    if (off > RAFT_REC_MAX)
        return 0;
    put_u32(p, efs_crc32(p + 8, off - 8));
    put_u32(p + 4, off - 8);
    return off;
}

static int write_all(int fd, const uint8_t *p, size_t n, uint64_t off)
{
    while (n) {
        ssize_t w = pwrite(fd, p, n, (off_t)off);

        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return EFS_ERR_IO;
        }
        p += w;
        n -= (size_t)w;
        off += (uint64_t)w;
    }
    return EFS_OK;
}

/* One fsync serves every append that was already written when it started:
 * a caller needing round R waits for sync_done >= R rather than issuing its
 * own. Caller holds d->mu. honor_hold: a shared hold means "more appends
 * are coming, do not fsync yet". The AE batch end passes 0 so a follower
 * still fsyncs its batch while proposers on the other group hold. */
static int log_sync_locked(struct efs_raft_disk *d, int honor_hold)
{
    uint64_t want;

    if (d->sync_mode != EFS_RAFT_DISK_SYNC)
        return EFS_OK;
    if (honor_hold && d->sync_hold > 0)
        return EFS_OK;
    want = ++d->sync_want;
    for (;;) {
        if (d->io_failed)
            return EFS_ERR_IO;
        if (d->sync_done >= want)
            return EFS_OK;
        if (d->syncing) {
            pthread_cond_wait(&d->cv, &d->mu);
            continue;
        }
        /* Take the round that covers every append written so far.
         * `cover` is that byte offset: a write that lands during the
         * fsync is past it and still needs its own round. */
        d->syncing = 1;
        want = d->sync_want;
        {
            uint64_t cover = d->bytes;

            pthread_mutex_unlock(&d->mu);
            if (fsync(d->fd) != 0) {
                pthread_mutex_lock(&d->mu);
                d->io_failed = 1;
            } else {
                pthread_mutex_lock(&d->mu);
                if (d->sync_done < want)
                    d->sync_done = want;
                if (d->synced_bytes < cover)
                    d->synced_bytes = cover;
            }
        }
        d->syncing = 0;
        pthread_cond_broadcast(&d->cv);
        if (d->io_failed)
            return EFS_ERR_IO;
        return EFS_OK;
    }
}

/* Write without making it durable. Only rotation may use this directly: it
 * writes many records and pays one fsync at the end. */
static int log_write_locked(struct efs_raft_disk *d, uint8_t type,
                            uint32_t group, uint64_t a, uint64_t b,
                            const uint8_t *cmd, uint32_t clen)
{
    uint32_t len;
    int rc;

    if (d->io_failed)
        return EFS_ERR_IO;
    len = rec_encode(d, type, group, a, b, cmd, clen);
    if (len == 0)
        return EFS_ERR_INVAL;
    rc = write_all(d->fd, d->rec, len, d->bytes);
    if (rc != EFS_OK) {
        d->io_failed = 1;
        return rc;
    }
    d->bytes += len;
    return EFS_OK;
}

/* Per-thread. A follower's AppendEntries loop sets this so each entry
 * does not fsync under the host lock; defer_end fsyncs the batch once. */
static __thread int log_defer_depth;

int raft_log_append(struct efs_raft_disk *d, uint8_t type,
                    struct raft_disk_group *g, uint64_t a, uint64_t b,
                    const uint8_t *cmd, uint32_t clen)
{
    int rc = log_write_locked(d, type, g->group, a, b, cmd, clen);

    if (rc != EFS_OK)
        return rc;
    if (d->sync_hold > 0 || log_defer_depth > 0) {
        d->sync_need = 1;
        return EFS_OK;
    }
    return log_sync_locked(d, 1);
}

void raft_log_defer_begin(void)
{
    log_defer_depth++;
}

int raft_log_defer_end(struct efs_raft_disk *d)
{
    int rc = EFS_OK;

    if (log_defer_depth > 0)
        log_defer_depth--;
    if (log_defer_depth > 0 || !d)
        return EFS_OK;
    pthread_mutex_lock(&d->mu);
    if (d->io_failed)
        rc = EFS_ERR_IO;
    else if (d->sync_need || d->synced_bytes < d->bytes) {
        d->sync_need = 0;
        rc = log_sync_locked(d, 0);
    }
    pthread_mutex_unlock(&d->mu);
    return rc;
}

/* --- replay ---------------------------------------------------------- */

static int read_all(int fd, uint8_t *p, size_t n, uint64_t off)
{
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);

        if (r == 0)
            return EFS_ERR_NOT_FOUND; /* short: torn tail */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_IO;
        }
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return EFS_OK;
}

static int replay_one(struct efs_raft_disk *d, const uint8_t *p, uint32_t len)
{
    struct raft_disk_group *g;
    uint32_t group, off = 0;
    uint8_t type;

    if (len < 5)
        return EFS_ERR_PROTO;
    type = p[off++];
    group = get_u32(p + off);
    off += 4;
    if (group >= RAFT_DISK_MAX_GROUPS)
        return EFS_ERR_PROTO;
    g = raft_group_get(d, group);
    if (!g)
        return EFS_ERR_NOMEM;
    switch (type) {
    case RAFT_REC_HARD:
        if (len - off != 12)
            return EFS_ERR_PROTO;
        g->term = get_u64(p + off);
        g->voted_for = (int32_t)get_u32(p + off + 8);
        return EFS_OK;
    case RAFT_REC_ENTRY: {
        uint64_t index, term;
        uint32_t clen;

        if (len - off < 20)
            return EFS_ERR_PROTO;
        index = get_u64(p + off);
        term = get_u64(p + off + 8);
        clen = get_u32(p + off + 16);
        off += 20;
        if (clen > RAFT_CMD_MAX || len - off != clen)
            return EFS_ERR_PROTO;
        return raft_group_put_entry(g, index, term, clen ? p + off : NULL,
                                    clen);
    }
    case RAFT_REC_TRUNC:
        if (len - off != 8)
            return EFS_ERR_PROTO;
        return raft_group_truncate(g, get_u64(p + off));
    case RAFT_REC_SNAP:
        if (len - off != 16)
            return EFS_ERR_PROTO;
        return raft_group_snap(g, get_u64(p + off), get_u64(p + off + 8));
    case RAFT_REC_CFG:
        if (len - off != 8)
            return EFS_ERR_PROTO;
        g->cfg_old = get_u32(p + off);
        g->cfg_new = get_u32(p + off + 4);
        g->have_cfg = 1;
        return EFS_OK;
    default:
        return EFS_ERR_PROTO;
    }
}

/* Truncating the torn tail is what makes the next append land at a clean
 * boundary; leaving it would put a good record behind a bad one. */
static int finish_replay(struct efs_raft_disk *d, uint64_t good)
{
    uint32_t i;

    if (good < d->bytes) {
        if (ftruncate(d->fd, (off_t)good) != 0)
            return EFS_ERR_IO;
        d->bytes = good;
    }
    d->live_bytes = 0;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++)
        if (d->g[i])
            d->live_bytes += raft_group_live_bytes(d->g[i]);
    return EFS_OK;
}

int raft_log_replay(struct efs_raft_disk *d)
{
    uint8_t hdr[RAFT_DISK_HDR_LEN];
    uint8_t *buf = NULL;
    uint64_t off = RAFT_DISK_HDR_LEN;
    uint32_t cap = 0;
    int rc = EFS_OK;

    if (d->bytes == 0) {
        put_u32(hdr, RAFT_DISK_MAGIC);
        put_u32(hdr + 4, RAFT_DISK_VERSION);
        rc = write_all(d->fd, hdr, sizeof(hdr), 0);
        if (rc != EFS_OK)
            return rc;
        d->bytes = RAFT_DISK_HDR_LEN;
        return fsync(d->fd) == 0 ? EFS_OK : EFS_ERR_IO;
    }
    if (d->bytes < RAFT_DISK_HDR_LEN)
        return EFS_ERR_PROTO;
    if (read_all(d->fd, hdr, sizeof(hdr), 0) != EFS_OK)
        return EFS_ERR_PROTO;
    if (get_u32(hdr) != RAFT_DISK_MAGIC || get_u32(hdr + 4) != RAFT_DISK_VERSION)
        return EFS_ERR_PROTO;
    while (off + 8 <= d->bytes) {
        uint8_t fr[8];
        uint32_t crc, paylen;

        if (read_all(d->fd, fr, sizeof(fr), off) != EFS_OK)
            break;
        crc = get_u32(fr);
        paylen = get_u32(fr + 4);
        if (paylen == 0 || paylen > RAFT_REC_MAX)
            break; /* garbage length: treat as the crash point */
        if (off + 8 + paylen > d->bytes)
            break; /* record does not fit: torn tail */
        if (paylen > cap) {
            uint8_t *nb = realloc(buf, paylen);

            if (!nb) {
                free(buf);
                return EFS_ERR_NOMEM;
            }
            buf = nb;
            cap = paylen;
        }
        if (read_all(d->fd, buf, paylen, off + 8) != EFS_OK)
            break;
        if (efs_crc32(buf, paylen) != crc)
            break;
        rc = replay_one(d, buf, paylen);
        if (rc != EFS_OK) {
            /* CRC passed, so the bytes are what we wrote — a record that
             * cannot apply means the log is inconsistent, not torn. */
            free(buf);
            return rc;
        }
        off += 8 + paylen;
    }
    free(buf);
    return finish_replay(d, off);
}

/* --- rotation -------------------------------------------------------- */

/* Writes every group's live state to a fresh file and renames it over the
 * old one, so a crash mid-rotation leaves the previous log intact. */
int raft_log_rotate_locked(struct efs_raft_disk *d)
{
    char tmp[RAFT_DISK_PATH_MAX + 32];
    char final[RAFT_DISK_PATH_MAX + 32];
    uint8_t hdr[RAFT_DISK_HDR_LEN];
    int fd, dfd, old_fd;
    uint64_t old_bytes;
    uint32_t i;
    int rc = EFS_OK;

    if (d->io_failed)
        return EFS_ERR_IO;
    snprintf(tmp, sizeof(tmp), "%s/raft.log.new", d->dir);
    snprintf(final, sizeof(final), "%s/raft.log", d->dir);
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return EFS_ERR_IO;
    put_u32(hdr, RAFT_DISK_MAGIC);
    put_u32(hdr + 4, RAFT_DISK_VERSION);
    rc = write_all(fd, hdr, sizeof(hdr), 0);
    /* Swap the fd so raft_log_append writes into the new file. */
    old_fd = d->fd;
    old_bytes = d->bytes;
    d->fd = fd;
    d->bytes = RAFT_DISK_HDR_LEN;
    for (i = 0; rc == EFS_OK && i < RAFT_DISK_MAX_GROUPS; i++) {
        struct raft_disk_group *g = d->g[i];
        uint32_t k;

        if (!g)
            continue;
        if (g->term || g->voted_for != -1)
            rc = log_write_locked(d, RAFT_REC_HARD, g->group, g->term,
                                  (uint32_t)g->voted_for, NULL, 0);
        if (rc == EFS_OK && g->snap_idx)
            rc = log_write_locked(d, RAFT_REC_SNAP, g->group, g->snap_idx,
                                  g->snap_term, NULL, 0);
        if (rc == EFS_OK && g->have_cfg)
            rc = log_write_locked(d, RAFT_REC_CFG, g->group, g->cfg_old,
                                  g->cfg_new, NULL, 0);
        for (k = 0; rc == EFS_OK && k < g->n; k++)
            rc = log_write_locked(d, RAFT_REC_ENTRY, g->group,
                                  g->snap_idx + 1 + k, g->log[k].term,
                                  g->log[k].cmd, g->log[k].clen);
    }
    if (rc == EFS_OK && fsync(fd) != 0)
        rc = EFS_ERR_IO;
    if (rc == EFS_OK && rename(tmp, final) != 0)
        rc = EFS_ERR_IO;
    if (rc != EFS_OK) {
        d->fd = old_fd;
        d->bytes = old_bytes;
        close(fd);
        unlink(tmp);
        d->io_failed = 1;
        return rc;
    }
    close(old_fd);
    dfd = open(d->dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    d->live_bytes = d->bytes;
    d->synced_bytes = d->bytes;
    return EFS_OK;
}

/* Recomputing the live size is O(live entries), so only do it once per
 * doubling of the file: the check itself must not become the cost. */
void raft_log_maybe_rotate_locked(struct efs_raft_disk *d)
{
    uint32_t i;

    if (d->bytes < d->check_at)
        return;
    d->live_bytes = 0;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++)
        if (d->g[i])
            d->live_bytes += raft_group_live_bytes(d->g[i]);
    if (d->bytes >= d->live_bytes * RAFT_ROTATE_RATIO)
        (void)raft_log_rotate_locked(d);
    d->check_at = d->bytes * 2;
    if (d->check_at < RAFT_ROTATE_MIN_BYTES)
        d->check_at = RAFT_ROTATE_MIN_BYTES;
}

int efs_raft_disk_sync_hold(struct efs_raft_disk *d)
{
    if (!d)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&d->mu);
    d->sync_hold++;
    pthread_mutex_unlock(&d->mu);
    return EFS_OK;
}

int efs_raft_disk_sync_release(struct efs_raft_disk *d)
{
    int rc = EFS_OK;

    if (!d)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&d->mu);
    if (d->sync_hold > 0)
        d->sync_hold--;
    /* A hold/release that appended nothing (a propose that forwarded)
     * must not fsync. Nested holds fsync once, on the release that
     * drops the count to zero, covering every append in between. */
    if (d->sync_hold == 0 && d->sync_need) {
        d->sync_need = 0;
        rc = log_sync_locked(d, 1);
    }
    pthread_mutex_unlock(&d->mu);
    return rc;
}

int efs_raft_disk_sync_depth(struct efs_raft_disk *d)
{
    int n;

    if (!d)
        return 0;
    pthread_mutex_lock(&d->mu);
    n = d->sync_hold;
    pthread_mutex_unlock(&d->mu);
    return n;
}

/* Caller dropped one hold. Returns only once `bytes` as of this call are
 * durable, so a later broadcast cannot publish an unsynced append. Other
 * holders may fsync the batch; this waits for that fsync. */
int efs_raft_disk_sync_release_wait(struct efs_raft_disk *d)
{
    int rc = EFS_OK;
    uint64_t mine;

    if (!d)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&d->mu);
    if (d->sync_hold > 0)
        d->sync_hold--;
    mine = d->bytes;
    if (d->sync_mode != EFS_RAFT_DISK_SYNC) {
        pthread_mutex_unlock(&d->mu);
        return EFS_OK;
    }
    for (;;) {
        if (d->io_failed) {
            rc = EFS_ERR_IO;
            break;
        }
        if (d->sync_hold == 0 && d->sync_need) {
            d->sync_need = 0;
            rc = log_sync_locked(d, 1);
            break;
        }
        if (d->synced_bytes >= mine)
            break;
        if (d->sync_hold == 0)
            break;
        pthread_cond_wait(&d->cv, &d->mu);
    }
    pthread_mutex_unlock(&d->mu);
    return rc;
}
