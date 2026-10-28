/* The Raft log file: record framing, group commit, replay, rotation.
 *
 * One file per node carries every group's records. The only thing that makes
 * that safe is that each record names its group and the file is replayed in
 * order, so a group's state is a fold over its own records. */

#include "raft_disk_internal.h"
#include "efs/batch_obs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static void put_u64(uint8_t *p, uint64_t v)
{
    efs_store_be32(p, (uint32_t)(v >> 32));
    efs_store_be32(p + 4, (uint32_t)v);
}


static uint64_t get_u64(const uint8_t *p)
{
    return ((uint64_t)efs_load_be32(p) << 32) | (uint64_t)efs_load_be32(p + 4);
}

/* Read evicted commands from their checked original frame, under d->mu.
 * Payload offsets are cache locations, never durable-watermark certificates. */
int raft_log_read_command(struct raft_disk_group *g,uint32_t slot,int fd,uint8_t *out)
{
    struct raft_log_ent e=g->log[slot];
    if(!e.clen)return EFS_OK;
    uint64_t phys=raft_ent_phys(g->d,&e);
    if(!out || phys<33 || e.clen>RAFT_CMD_MAX)return EFS_ERR_PROTO;
    uint32_t size=33+e.clen,done=0;uint8_t *record=g->d->rec;
    while(done<size) {
        ssize_t n=pread(fd,record+done,size-done,(off_t)(phys-33+done));
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return EFS_ERR_IO;
        done+=(uint32_t)n;
    }
    if(efs_load_be32(record+4)!=size-8 || efs_crc32(record+8,size-8)!=efs_load_be32(record) ||
       record[8]!=RAFT_REC_ENTRY || efs_load_be32(record+9)!=g->group ||
       get_u64(record+13)!=g->log_base+slot+1 || get_u64(record+21)!=e.term ||
       efs_load_be32(record+29)!=e.clen)return EFS_ERR_PROTO;
    memcpy(out,record+33,e.clen);return EFS_OK;
}

/* Serialize one record into p (RAFT_REC_MAX bytes, or 64 for a record
 * without a command); returns total length, 0 if too big. */
static uint32_t rec_encode(uint8_t *p, uint8_t type,
                           uint32_t group, uint64_t a, uint64_t b,
                           const uint8_t *cmd, uint32_t clen,
                           const uint32_t *record_crc)
{
    uint32_t off = 8;

    p[off++] = type;
    efs_store_be32(p + off, group);
    off += 4;
    switch (type) {
    case RAFT_REC_HARD:
        put_u64(p + off, a);
        off += 8;
        efs_store_be32(p + off, (uint32_t)b);
        off += 4;
        break;
    case RAFT_REC_ENTRY:
        if (clen > RAFT_CMD_MAX)
            return 0;
        put_u64(p + off, a);
        off += 8;
        put_u64(p + off, b);
        off += 8;
        efs_store_be32(p + off, clen);
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
        /* Optional retained-window base. 8 bytes, big-endian like the
         * other fields. Absent (clen 0) is a pre-window record. */
        if (clen == 8 && cmd) {
            uint64_t base;

            memcpy(&base, cmd, 8);
            put_u64(p + off, base);
            off += 8;
        }
        break;
    case RAFT_REC_CFG:
        efs_store_be32(p + off, (uint32_t)a);
        off += 4;
        efs_store_be32(p + off, (uint32_t)b);
        off += 4;
        break;
    default:
        return 0;
    }
    if (off > RAFT_REC_MAX)
        return 0;
    efs_store_be32(p, record_crc ? *record_crc : efs_crc32(p + 8, off - 8));
    efs_store_be32(p + 4, off - 8);
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
    if (honor_hold && d->sync_hold > 0) {
        BO_COUNT(BO_SYNC_HOLD_SKIP);
        return EFS_OK;
    }
    if (d->io_failed)
        return EFS_ERR_IO;
    /* A concurrent batch may already have persisted this tail. Creating
     * a fresh round here would issue a barrier with no new log bytes. */
    if (d->synced_bytes >= d->bytes) {
        BO_COUNT(BO_SYNC_DURABLE_SKIP);
        return EFS_OK;
    }
    want = ++d->sync_want;
    for (;;) {
        if (d->io_failed)
            return EFS_ERR_IO;
        if (d->sync_done >= want)
            return EFS_OK;
        if (d->syncing) {
            uint64_t began = 0;
            if (BO_ON()) {
                began = efs_batch_obs_now_us();
                BO_HIST(BH_SYNC_WAITERS, ++d->obs_waiters);
            }
            pthread_cond_wait(&d->cv, &d->mu);
            if (BO_ON()) {
                d->obs_waiters--;
                BO_HIST(BH_SYNC_WAIT_US, efs_batch_obs_now_us()-began);
            }
            continue;
        }
        /* Take the round that covers every append written so far.
         * `cover` is that byte offset: a write that lands during the
         * fsync is past it and still needs its own round. */
        d->syncing = 1;
        want = d->sync_want;
        {
            uint64_t cover = d->bytes;
            uint64_t entries = d->obs_entries, began = 0;
            if (BO_ON()) {
                began = efs_batch_obs_now_us();
                BO_COUNT(BO_SYNC);
                BO_HIST(BH_WAL_ENTRIES, entries-d->obs_synced_entries);
                BO_HIST(BH_WAL_BYTES, cover-d->synced_bytes);
            }

            pthread_mutex_unlock(&d->mu);
            /* Recovery requires the log bytes and length; fdatasync
             * persists both without unrelated timestamp metadata. */
            if (fdatasync(d->fd) != 0) {
                pthread_mutex_lock(&d->mu);
                d->io_failed = 1;
                BO_COUNT(BO_SYNC_ERROR);
            } else {
                pthread_mutex_lock(&d->mu);
                if (d->sync_done < want)
                    d->sync_done = want;
                if (d->synced_bytes < cover)
                    d->synced_bytes = cover;
                if (BO_ON()) d->obs_synced_entries = entries;
            }
            if (BO_ON()) BO_HIST(BH_SYNC_US, efs_batch_obs_now_us()-began);
        }
        d->syncing = 0;
        pthread_cond_broadcast(&d->cv);
        if (d->io_failed)
            return EFS_ERR_IO;
        return EFS_OK;
    }
}

/* Reserve the EOF ahead of small appends. On XFS, growing EOF on every
 * durable record also forces inode/log work. Unwritten extents read as zero,
 * which replay already treats as the end of records. The valid/durable byte
 * watermarks must never include this padding. Allocation is an optimization:
 * if the filesystem cannot reserve a whole chunk, try the exact record write
 * normally (it may still fit). Its normal sync remains the ACK boundary. */
static void log_reserve_locked(struct efs_raft_disk *d, uint64_t end)
{
#ifdef __linux__
    const uint64_t chunk = 1024ull * 1024ull;
    uint64_t reserve;
    int rc;

    if (d->sync_mode != EFS_RAFT_DISK_SYNC || d->prealloc_off ||
        end <= d->allocated_bytes || end > INT64_MAX - chunk)
        return;
    reserve = (end + chunk - 1) & ~(chunk - 1);
    do {
        rc = fallocate(d->fd, 0, (off_t)d->allocated_bytes,
                       (off_t)(reserve - d->allocated_bytes));
    } while (rc < 0 && errno == EINTR);
    if (rc == 0)
        d->allocated_bytes = reserve;
    else
        d->prealloc_off = 1;
#else
    (void)d;
    (void)end;
#endif
}

/* Write without making it durable; raft_log_append decides when to sync. */
static int log_write_locked(struct efs_raft_disk *d, uint8_t type,
                            uint32_t group, uint64_t a, uint64_t b,
                            const uint8_t *cmd, uint32_t clen)
{
    uint32_t len;
    int rc;

    if (d->io_failed)
        return EFS_ERR_IO;
    len = rec_encode(d->rec, type, group, a, b, cmd, clen, NULL);
    if (len == 0)
        return EFS_ERR_INVAL;
    log_reserve_locked(d, d->bytes + len);
    rc = write_all(d->fd, d->rec, len, d->bytes);
    if (rc != EFS_OK) {
        d->io_failed = 1;
        return rc;
    }
    d->bytes += len;
    if (BO_ON() && type == RAFT_REC_ENTRY) {
        d->obs_entries++;
    }
    if (d->allocated_bytes < d->bytes)
        d->allocated_bytes = d->bytes;
    return EFS_OK;
}

/* Per-thread. A follower's AppendEntries loop sets this so each entry
 * does not fsync under the host lock; defer_end fsyncs the batch once. */
static __thread int log_defer_depth;

int raft_log_append(struct efs_raft_disk *d, uint8_t type,
                    struct raft_disk_group *g, uint64_t a, uint64_t b,
                    const uint8_t *cmd, uint32_t clen, uint32_t *record_crc,uint64_t *record_end)
{
    int rc = log_write_locked(d, type, g->group, a, b, cmd, clen);

    if (rc != EFS_OK)
        return rc;
    /* Capture before log_sync_locked can drop d->mu and another writer
     * replaces d->rec. Commands are immutable until removed from the log. */
    if (record_crc)
        *record_crc = efs_load_be32(d->rec);
    if(record_end)*record_end=d->base+d->bytes;
    /* Count new append requests, not old records copied during rotation. */
    if (BO_ON() && type == RAFT_REC_ENTRY)
        efs_batch_obs_command(clen ? cmd[0] : 0);
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

/* Replay is sequential. A bounded window avoids two tiny preads per record
 * while keeping CRC, framing and record ordering identical. Read failures
 * remain distinct from a short crash tail and must never truncate the log. */
#define RAFT_REPLAY_WINDOW (64u * 1024u)
struct replay_reader {
    int fd;
    uint64_t base;
    size_t len;
    uint8_t data[RAFT_REPLAY_WINDOW];
};

static int replay_read(struct replay_reader *r, uint8_t *p, size_t n,
                       uint64_t off)
{
    while (n) {
        if (off < r->base || off - r->base >= r->len) {
            ssize_t got;
            do { got = pread(r->fd, r->data, sizeof(r->data), (off_t)off); }
            while (got < 0 && errno == EINTR);
            if (got < 0)
                return EFS_ERR_IO;
            if (!got)
                return EFS_ERR_NOT_FOUND;
            r->base = off;
            r->len = (size_t)got;
        }
        size_t pos = (size_t)(off - r->base);
        size_t take = r->len - pos;
        if (take > n) take = n;
        memcpy(p, r->data + pos, take);
        p += take;
        n -= take;
        off += take;
    }
    return EFS_OK;
}

static int replay_one(struct efs_raft_disk *d, const uint8_t *p, uint32_t len, uint32_t record_crc,uint64_t end)
{
    struct raft_disk_group *g;
    uint32_t group, off = 0;
    uint8_t type;

    if (len < 5)
        return EFS_ERR_PROTO;
    type = p[off++];
    group = efs_load_be32(p + off);
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
        g->voted_for = (int32_t)efs_load_be32(p + off + 8);
        return EFS_OK;
    case RAFT_REC_ENTRY: {
        uint64_t index, term;
        uint32_t clen;

        if (len - off < 20)
            return EFS_ERR_PROTO;
        index = get_u64(p + off);
        term = get_u64(p + off + 8);
        clen = efs_load_be32(p + off + 16);
        off += 20;
        if (clen > RAFT_CMD_MAX || len - off != clen)
            return EFS_ERR_PROTO;
        int rc = raft_group_put_entry(g, index, term, clen ? p + off : NULL, clen);
        if (rc == EFS_OK)
            g->log[index - g->log_base - 1].record_crc = record_crc;
        if(rc==EFS_OK && clen)g->log[index-g->log_base-1].payload_off=end-clen;
        return rc;
    }
    case RAFT_REC_TRUNC:
        if (len - off != 8)
            return EFS_ERR_PROTO;
        return raft_group_truncate(g, get_u64(p + off));
    case RAFT_REC_SNAP: {
        uint64_t hint = RAFT_SNAP_NO_HINT;

        if (len - off != 16 && len - off != 24)
            return EFS_ERR_PROTO;
        if (len - off == 24)
            hint = get_u64(p + off + 16);
        return raft_group_snap(g, get_u64(p + off), get_u64(p + off + 8),
                               hint);
    }
    case RAFT_REC_CFG:
        if (len - off != 8)
            return EFS_ERR_PROTO;
        g->cfg_old = efs_load_be32(p + off);
        g->cfg_new = efs_load_be32(p + off + 4);
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
    d->allocated_bytes = good;
    d->live_bytes = 0;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++)
        if (d->g[i])
            d->live_bytes += raft_group_live_bytes(d->g[i]);
    return EFS_OK;
}

int raft_log_replay(struct efs_raft_disk *d)
{
    struct replay_reader reader = {.fd = d->fd};
    uint8_t hdr[RAFT_DISK_HDR_LEN];
    uint8_t *buf = NULL;
    uint64_t off = RAFT_DISK_HDR_LEN;
    uint32_t cap = 0;
    int rc = EFS_OK;

    if (d->bytes == 0) {
        efs_store_be32(hdr, RAFT_DISK_MAGIC);
        efs_store_be32(hdr + 4, RAFT_DISK_VERSION);
        rc = write_all(d->fd, hdr, sizeof(hdr), 0);
        if (rc != EFS_OK)
            return rc;
        d->bytes = RAFT_DISK_HDR_LEN;
        d->allocated_bytes = d->bytes;
        return fsync(d->fd) == 0 ? EFS_OK : EFS_ERR_IO;
    }
    if (d->bytes < RAFT_DISK_HDR_LEN)
        return EFS_ERR_PROTO;
    rc = replay_read(&reader, hdr, sizeof(hdr), 0);
    if (rc != EFS_OK)
        return rc == EFS_ERR_NOT_FOUND ? EFS_ERR_PROTO : rc;
    if (efs_load_be32(hdr) != RAFT_DISK_MAGIC || efs_load_be32(hdr + 4) != RAFT_DISK_VERSION)
        return EFS_ERR_PROTO;
    while (off + 8 <= d->bytes) {
        uint8_t fr[8];
        uint32_t crc, paylen;

        rc = replay_read(&reader, fr, sizeof(fr), off);
        if (rc != EFS_OK) {
            if (rc != EFS_ERR_NOT_FOUND) { free(buf); return rc; }
            break;
        }
        crc = efs_load_be32(fr);
        paylen = efs_load_be32(fr + 4);
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
        rc = replay_read(&reader, buf, paylen, off + 8);
        if (rc != EFS_OK) {
            if (rc != EFS_ERR_NOT_FOUND) { free(buf); return rc; }
            break;
        }
        if (efs_crc32(buf, paylen) != crc)
            break;
        rc = replay_one(d, buf, paylen, crc, off+8+paylen);
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

/* Rotation writes the live records to raft.log.new and renames it over
 * raft.log, so a crash mid-rotation leaves the previous log intact.
 *
 * It used to run whole under d->mu, from save_snap, while the host pump
 * held h->mu: on orcd-mini a 5.8 GB log with 1.5 GB live stalled every
 * proposal, read and apply on all three replicas for 12-15 s
 * (docs/status/raft-rotate-stall-20261017.md). Now d->mu is held only in
 * short steps and production runs it on its own thread. The new file is:
 *
 *   image   the live state as of the capture point X (old-file bytes
 *           [0, X)): per group HARD, SNAP(+base), CFG and every entry.
 *           Entry frames are copied verbatim and CRC-checked from the old
 *           file. Their metadata is read under d->mu RAFT_ROTATE_CHUNK
 *           entries at a time; frames are read and written unlocked.
 *   suffix  old-file bytes [X, end), copied raw. Replaying them on top of
 *           the image is the same fold that took the state at X to the
 *           current state, so image + suffix replays to what RAM holds.
 *           Copied unlocked until the remainder is small; the last piece,
 *           fdatasync, rename and directory fsync run under d->mu, then
 *           the fd swaps.
 *
 * If an entry not yet imaged is truncated, overwritten or compacted away,
 * the attempt ends with EFS_ERR_STALE and the old file stays authoritative.
 *
 * Offsets: suffix records keep their logical offsets (d->base becomes
 * old_base + X - image_len, modulo 2^64), so end_off/payload_off of every
 * record appended after X stay valid and durable watermarks never move
 * backwards. Imaged entries are tagged with their image position under
 * the attempt's gen; the swap makes the tags current without a walk. */
#define RAFT_ROTATE_BUFFER_BYTES (1024u * 1024u)
#define RAFT_ROTATE_CHUNK        8192u
#define RAFT_ROTATE_TAIL_LOCKED  (256u * 1024u)
#define RAFT_ROTATE_TAIL_PASSES  16
#define RAFT_ROTATE_RETRIES      3
#define RAFT_ROTATE_WRITEBACK    (8u * 1024u * 1024u)

void (*raft_log_rotate_hook)(struct efs_raft_disk *d, int step);

struct rot_group {
    struct raft_disk_group *g;
    uint64_t term, snap_idx, snap_term, log_base, last;
    int32_t voted_for;
    uint32_t cfg_old, cfg_new;
    int have_hard, have_cfg;
};

struct rot_meta {
    uint64_t term, phys;
    uint32_t clen, crc;
};

/* Write the new file back in RAFT_ROTATE_WRITEBACK steps. A rotation
 * that leaves hundreds of MB dirty until its fdatasync makes every Raft
 * log fsync on the same device queue behind that flush; this keeps at most
 * one step in flight and paces the copy at device speed. */
static void rot_writeback(int fd, uint64_t *done, uint64_t upto)
{
#ifdef __linux__
    if (upto - *done < RAFT_ROTATE_WRITEBACK)
        return;
    (void)sync_file_range(fd, (off_t)*done, (off_t)(upto - *done),
                          SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                          SYNC_FILE_RANGE_WAIT_AFTER);
    *done = upto;
#else
    (void)fd;
    (void)done;
    (void)upto;
#endif
}

/* Buffered sequential writer for the new file. */
struct rot_out {
    int fd;
    uint8_t *buf;
    size_t used;
    uint64_t off; /* file offset of buf[0] */
    uint64_t written_back;
};

static int rot_flush(struct rot_out *o)
{
    int rc;

    if (!o->used)
        return EFS_OK;
    rc = write_all(o->fd, o->buf, o->used, o->off);
    if (rc == EFS_OK) {
        o->off += o->used;
        o->used = 0;
        rot_writeback(o->fd, &o->written_back, o->off);
    }
    return rc;
}

static int rot_put(struct rot_out *o, const uint8_t *p, size_t n)
{
    int rc;

    if (n > RAFT_ROTATE_BUFFER_BYTES - o->used) {
        rc = rot_flush(o);
        if (rc != EFS_OK)
            return rc;
    }
    if (n > RAFT_ROTATE_BUFFER_BYTES) {
        rc = write_all(o->fd, p, n, o->off);
        if (rc == EFS_OK) {
            o->off += n;
            rot_writeback(o->fd, &o->written_back, o->off);
        }
        return rc;
    }
    memcpy(o->buf + o->used, p, n);
    o->used += n;
    return EFS_OK;
}

/* Window over the old file: live frames are mostly in file order. Never
 * caches at or past `limit`, the valid-record end the caller read under
 * d->mu: past it the file may be preallocated zeros that a later append
 * overwrites, while bytes below it never change. */
struct rot_in {
    int fd;
    uint8_t *buf;
    size_t cap, len;
    uint64_t base;
};

static int rot_read(struct rot_in *r, uint64_t off, size_t n, uint64_t limit,
                    const uint8_t **out)
{
    size_t got = 0, want;

    if (off >= r->base && off + n <= r->base + r->len) {
        *out = r->buf + (off - r->base);
        return EFS_OK;
    }
    if (n > r->cap) {
        uint8_t *nb = realloc(r->buf, n);

        if (!nb)
            return EFS_ERR_NOMEM;
        r->buf = nb;
        r->cap = n;
    }
    if (off + n > limit)
        return EFS_ERR_PROTO;
    want = limit - off < r->cap ? (size_t)(limit - off) : r->cap;
    r->len = 0;
    while (got < n) {
        ssize_t k = pread(r->fd, r->buf + got, want - got,
                          (off_t)(off + got));

        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return EFS_ERR_IO;
        got += (size_t)k;
    }
    r->base = off;
    r->len = got;
    *out = r->buf;
    return EFS_OK;
}

/* Raw copy of old-file bytes [from, to) to dst in the new file. */
static int rot_copy(struct rot_in *r, int fd, uint64_t from, uint64_t to,
                    uint64_t dst)
{
    uint64_t written_back = dst;

    while (from < to) {
        size_t n = to - from > RAFT_ROTATE_BUFFER_BYTES
                       ? RAFT_ROTATE_BUFFER_BYTES : (size_t)(to - from);
        const uint8_t *p;
        int rc = rot_read(r, from, n, to, &p);

        if (rc == EFS_OK)
            rc = write_all(fd, p, n, dst);
        if (rc != EFS_OK)
            return rc;
        from += n;
        dst += n;
        rot_writeback(fd, &written_back, dst);
    }
    return EFS_OK;
}

/* Lock bookkeeping. `drop` 0 keeps d->mu for the whole rotation (inline
 * NOSYNC callers that already own the store's state). */
struct rot_lock {
    struct efs_raft_disk *d;
    int drop, held;
    uint64_t since;
};

static void rot_note_hold(struct rot_lock *l)
{
    uint64_t held = efs_batch_obs_now_us() - l->since;

    if (l->drop && held > l->d->rot_hold_max_us)
        l->d->rot_hold_max_us = held;
}

static void rot_unlock(struct rot_lock *l)
{
    if (!l->drop || !l->held)
        return;
    rot_note_hold(l);
    l->held = 0;
    pthread_mutex_unlock(&l->d->mu);
}

static void rot_relock(struct rot_lock *l)
{
    if (l->held)
        return;
    pthread_mutex_lock(&l->d->mu);
    l->held = 1;
    l->since = efs_batch_obs_now_us();
}

static void rot_hook(struct rot_lock *l, int *step)
{
    if (l->drop && raft_log_rotate_hook)
        raft_log_rotate_hook(l->d, (*step)++);
}

/* Read up to RAFT_ROTATE_CHUNK entries' metadata from index `next` and
 * tag them with their image position. d->mu held. */
static int rot_chunk(struct efs_raft_disk *d, struct rot_group *r,
                     uint64_t next, uint64_t hi, uint64_t pos, uint32_t gen,
                     struct rot_meta *meta)
{
    struct raft_disk_group *g = r->g;
    uint64_t index;

    if (d->rot_stop)
        return EFS_ERR_STALE;
    if (d->io_failed)
        return EFS_ERR_IO;
    if (next <= g->log_base || g->rot_low <= hi ||
        hi > g->log_base + g->n)
        return EFS_ERR_STALE;
    for (index = next; index <= hi; index++) {
        struct raft_log_ent *e = &g->log[index - g->log_base - 1];
        struct rot_meta *m = &meta[index - next];

        m->term = e->term;
        m->clen = e->clen;
        m->crc = e->record_crc;
        m->phys = 0;
        if (e->clen) {
            m->phys = raft_ent_phys(d, e);
            if (pos + 34 + e->clen > RAFT_IMG_POS_MASK)
                return EFS_ERR_INVAL;
            /* Keep the current location readable without this tag. */
            if (e->img_off &&
                (uint32_t)(e->img_off >> RAFT_IMG_POS_BITS) == d->gen)
                e->payload_off = RAFT_OFF_IMAGE | m->phys;
            e->img_off = ((uint64_t)gen << RAFT_IMG_POS_BITS) | (pos + 34);
        }
        pos += 33 + (uint64_t)e->clen;
    }
    return EFS_OK;
}

/* Called with d->mu held; returns with it held. */
static int rotate_run(struct efs_raft_disk *d, int drop)
{
    char tmp[RAFT_DISK_PATH_MAX + 32];
    char final[RAFT_DISK_PATH_MAX + 32];
    uint8_t small[64];
    struct rot_lock l = {d, drop, 1, efs_batch_obs_now_us()};
    struct rot_group *rg = NULL;
    struct rot_meta *meta = NULL;
    struct rot_out out = {.fd = -1};
    struct rot_in in = {.fd = -1};
    uint64_t started = l.since, final_start = 0;
    uint64_t x = 0, old_base, image_len, copied, end = 0, old_bytes = 0;
    uint32_t ng = 0, i, gen;
    int old_fd, rc = EFS_OK, step = 0, renamed = 0;

    if (d->io_failed)
        return EFS_ERR_IO;
    d->rotating = 1;
    d->rot_hold_max_us = 0;
    d->rot_final_us = 0;
    gen = d->gen_seq;
    do {
        gen = (gen + 1) & RAFT_IMG_GEN_MASK;
    } while (!gen || gen == d->gen);
    d->gen_seq = gen;
    rg = calloc(RAFT_DISK_MAX_GROUPS, sizeof(*rg));
    meta = malloc(RAFT_ROTATE_CHUNK * sizeof(*meta));
    out.buf = malloc(RAFT_ROTATE_BUFFER_BYTES);
    in.buf = malloc(RAFT_ROTATE_BUFFER_BYTES);
    in.cap = RAFT_ROTATE_BUFFER_BYTES;
    if (!rg || !meta || !out.buf || !in.buf) {
        rc = EFS_ERR_NOMEM;
        goto done;
    }
    /* Capture X. Everything about the image is fixed here, and it must
     * be the fold of exactly the records in [0, X): wait out mutators that
     * logged a record but have not applied it yet (they drop d->mu for
     * fdatasync in between). Inline callers are such a mutator themselves,
     * past their own apply. */
    if (drop) {
        d->rot_capturing = 1;
        while (d->applying) {
            rot_note_hold(&l);
            pthread_cond_wait(&d->cv, &d->mu);
            l.since = efs_batch_obs_now_us();
        }
    } else if (d->applying > 1) {
        rc = EFS_ERR_STALE;
        goto done;
    }
    x = old_bytes = d->bytes;
    old_base = d->base;
    old_fd = in.fd = d->fd;
    image_len = RAFT_DISK_HDR_LEN;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++) {
        struct raft_disk_group *g = d->g[i];
        struct rot_group *r;

        if (!g)
            continue;
        r = &rg[ng++];
        r->g = g;
        r->term = g->term;
        r->voted_for = g->voted_for;
        r->have_hard = g->term || g->voted_for != -1;
        r->snap_idx = g->snap_idx;
        r->snap_term = g->snap_term;
        r->log_base = g->log_base;
        r->last = g->log_base + g->n;
        r->have_cfg = g->have_cfg;
        r->cfg_old = g->cfg_old;
        r->cfg_new = g->cfg_new;
        g->rot_low = UINT64_MAX;
        image_len += (r->have_hard ? REC_LEN_HARD : 0) +
                     (r->snap_idx ? REC_LEN_SNAP_BASE : 0) +
                     (r->have_cfg ? REC_LEN_CFG : 0) +
                     (uint64_t)g->n * 33 + g->bytes_all;
    }
    if (d->rot_capturing) {
        d->rot_capturing = 0;
        pthread_cond_broadcast(&d->cv);
    }
    rot_unlock(&l);

    snprintf(tmp, sizeof(tmp), "%s/raft.log.new", d->dir);
    snprintf(final, sizeof(final), "%s/raft.log", d->dir);
    out.fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out.fd < 0) {
        rc = EFS_ERR_IO;
        goto done;
    }
    efs_store_be32(small, RAFT_DISK_MAGIC);
    efs_store_be32(small + 4, RAFT_DISK_VERSION);
    rc = rot_put(&out, small, RAFT_DISK_HDR_LEN);
    for (i = 0; rc == EFS_OK && i < ng; i++) {
        struct rot_group *r = &rg[i];
        uint32_t group = r->g->group, len;
        uint64_t next = r->log_base + 1;

        if (r->have_hard) {
            len = rec_encode(small, RAFT_REC_HARD, group, r->term,
                             (uint32_t)r->voted_for, NULL, 0, NULL);
            rc = rot_put(&out, small, len);
        }
        if (rc == EFS_OK && r->snap_idx) {
            len = rec_encode(small, RAFT_REC_SNAP, group, r->snap_idx,
                             r->snap_term, (const uint8_t *)&r->log_base,
                             8, NULL);
            rc = rot_put(&out, small, len);
        }
        if (rc == EFS_OK && r->have_cfg) {
            len = rec_encode(small, RAFT_REC_CFG, group, r->cfg_old,
                             r->cfg_new, NULL, 0, NULL);
            rc = rot_put(&out, small, len);
        }
        while (rc == EFS_OK && next <= r->last) {
            uint64_t hi = r->last - next >= RAFT_ROTATE_CHUNK
                              ? next + RAFT_ROTATE_CHUNK - 1 : r->last;
            uint64_t index;

            rot_hook(&l, &step);
            rot_relock(&l);
            rc = rot_chunk(d, r, next, hi, out.off + out.used, gen, meta);
            rot_unlock(&l);
            for (index = next; rc == EFS_OK && index <= hi; index++) {
                const struct rot_meta *m = &meta[index - next];
                const uint8_t *f;
                uint32_t size = 33 + m->clen;

                if (!m->clen) {
                    len = rec_encode(small, RAFT_REC_ENTRY, group, index,
                                     m->term, NULL, 0, &m->crc);
                    rc = rot_put(&out, small, len);
                    continue;
                }
                if (m->phys < RAFT_DISK_HDR_LEN + 33)
                    rc = EFS_ERR_PROTO;
                else
                    rc = rot_read(&in, m->phys - 33, size, x, &f);
                if (rc == EFS_OK &&
                    (efs_load_be32(f) != m->crc || efs_load_be32(f + 4) != size - 8 ||
                     f[8] != RAFT_REC_ENTRY || efs_load_be32(f + 9) != group ||
                     get_u64(f + 13) != index || get_u64(f + 21) != m->term ||
                     efs_load_be32(f + 29) != m->clen ||
                     efs_crc32(f + 8, size - 8) != m->crc)) {
                    fprintf(stderr, "raft-log: rotate bad frame group=%u "
                            "index=%llu off=%llu\n", group,
                            (unsigned long long)index,
                            (unsigned long long)m->phys);
                    rc = EFS_ERR_PROTO;
                }
                if (rc == EFS_OK)
                    rc = rot_put(&out, f, size);
            }
            next = hi + 1;
        }
    }
    if (rc == EFS_OK)
        rc = rot_flush(&out);
    if (rc == EFS_OK && out.off != image_len) {
        fprintf(stderr, "raft-log: rotate image %llu != planned %llu\n",
                (unsigned long long)out.off, (unsigned long long)image_len);
        rc = EFS_ERR_PROTO;
    }
    if (rc == EFS_OK && fdatasync(out.fd) != 0)
        rc = EFS_ERR_IO;

    /* Suffix, unlocked while the remainder is large. */
    copied = x;
    for (int pass = 0; rc == EFS_OK; pass++) {
        rot_hook(&l, &step);
        rot_relock(&l);
        if (d->rot_stop)
            rc = EFS_ERR_STALE;
        else if (d->io_failed)
            rc = EFS_ERR_IO;
        end = d->bytes;
        if (rc != EFS_OK || !drop || end - copied <= RAFT_ROTATE_TAIL_LOCKED ||
            pass >= RAFT_ROTATE_TAIL_PASSES)
            break;
        rot_unlock(&l);
        rc = rot_copy(&in, out.fd, copied, end, image_len + copied - x);
        if (rc == EFS_OK && fdatasync(out.fd) != 0)
            rc = EFS_ERR_IO;
        copied = end;
    }
    if (rc != EFS_OK)
        goto done;

    /* Final step, under d->mu. A sync drops d->mu around fdatasync on the
     * old fd and then advances synced_bytes for it: never swap under it. */
    final_start = efs_batch_obs_now_us();
    while (d->syncing && !d->rot_stop && !d->io_failed) {
        rot_note_hold(&l);
        pthread_cond_wait(&d->cv, &d->mu);
        l.since = final_start = efs_batch_obs_now_us();
    }
    if (d->rot_stop || d->io_failed) {
        rc = d->io_failed ? EFS_ERR_IO : EFS_ERR_STALE;
        goto done;
    }
    end = d->bytes;
    rc = rot_copy(&in, out.fd, copied, end, image_len + copied - x);
    if (rc == EFS_OK && fdatasync(out.fd) != 0)
        rc = EFS_ERR_IO;
    if (rc == EFS_OK && rename(tmp, final) != 0)
        rc = EFS_ERR_IO;
    if (rc != EFS_OK)
        goto done;
    renamed = 1;
    {
        int dfd = open(d->dir, O_RDONLY | O_CLOEXEC);

        /* Appends after the swap land only in the new inode. If the rename
         * is not durable they could vanish behind the old name: fence. */
        if (dfd < 0 || fsync(dfd) != 0)
            d->io_failed = 1;
        if (dfd >= 0)
            close(dfd);
    }
    d->fd = out.fd;
    out.fd = -1;
    d->bytes = image_len + (end - x);
    d->allocated_bytes = d->bytes;
    d->synced_bytes = d->bytes;
    d->base = old_base + x - image_len;
    d->gen = gen;
    d->rotations++;
    d->live_bytes = 0;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++)
        if (d->g[i])
            d->live_bytes += raft_group_live_bytes(d->g[i]);
    d->check_at = d->bytes * 2;
    if (d->check_at < RAFT_ROTATE_MIN_BYTES)
        d->check_at = RAFT_ROTATE_MIN_BYTES;
    if (BO_ON()) d->obs_synced_entries = d->obs_entries;
    BO_COUNT(BO_ROTATE);
    d->rot_final_us = efs_batch_obs_now_us() - final_start;
    old_bytes = end;
    /* Closing the old inode frees its blocks: keep that off d->mu. */
    rot_unlock(&l);
    close(old_fd);

done:
    rot_unlock(&l);
    if (out.fd >= 0) {
        close(out.fd);
        if (!renamed)
            unlink(tmp);
    }
    free(rg);
    free(meta);
    free(out.buf);
    free(in.buf);
    rot_relock(&l);
    if (drop) {
        uint64_t held = efs_batch_obs_now_us() - l.since;

        if (held > d->rot_hold_max_us)
            d->rot_hold_max_us = held;
    } else {
        d->rot_hold_max_us = efs_batch_obs_now_us() - started;
    }
    d->rotating = 0;
    pthread_cond_broadcast(&d->cv);
    {
        uint64_t elapsed = efs_batch_obs_now_us() - started;

        if ((rc == EFS_OK && elapsed > 20000) ||
            (rc != EFS_OK && rc != EFS_ERR_STALE))
            fprintf(stderr, "raft-log: rotate old_bytes=%llu live_bytes=%llu "
                    "elapsed_us=%llu hold_max_us=%llu final_us=%llu rc=%d\n",
                    (unsigned long long)old_bytes,
                    (unsigned long long)(rc == EFS_OK ? d->bytes : 0),
                    (unsigned long long)elapsed,
                    (unsigned long long)d->rot_hold_max_us,
                    (unsigned long long)d->rot_final_us, rc);
    }
    return rc;
}

/* Explicit rotation (efs_raft_disk_rotate). d->mu held. */
int raft_log_rotate_locked(struct efs_raft_disk *d)
{
    int rc, tries = 0;

    if (d->io_failed)
        return EFS_ERR_IO;
    /* Keep the in-flight sync's file and watermark stable. */
    if (d->syncing)
        return EFS_ERR_BUSY;
    do {
        rc = rotate_run(d, 1);
    } while (rc == EFS_ERR_STALE && !d->rot_stop && ++tries < RAFT_ROTATE_RETRIES);
    return rc;
}

/* Recomputing the live size is O(groups); it still runs only once per
 * doubling of the file. */
void raft_log_maybe_rotate_locked(struct efs_raft_disk *d)
{
    uint32_t i;

    if (d->syncing || d->rotating)
        return; /* Preserve check_at so a later append retries. */
    if (d->bytes < d->check_at)
        return;
    d->live_bytes = 0;
    for (i = 0; i < RAFT_DISK_MAX_GROUPS; i++)
        if (d->g[i])
            d->live_bytes += raft_group_live_bytes(d->g[i]);
    if (d->bytes >= d->live_bytes * RAFT_ROTATE_RATIO) {
        if (d->rot_thread_on) {
            d->rot_want = 1;
            pthread_cond_signal(&d->rot_cv);
        } else {
            (void)rotate_run(d, 0);
        }
    }
    d->check_at = d->bytes * 2;
    if (d->check_at < RAFT_ROTATE_MIN_BYTES)
        d->check_at = RAFT_ROTATE_MIN_BYTES;
}

static void *rotator_main(void *arg)
{
    struct efs_raft_disk *d = arg;

    pthread_mutex_lock(&d->mu);
    while (!d->rot_stop) {
        int rc;

        if (!d->rot_want) {
            pthread_cond_wait(&d->rot_cv, &d->mu);
            continue;
        }
        d->rot_want = 0;
        if (d->rotating || d->io_failed)
            continue;
        rc = rotate_run(d, 1);
        /* The state moved under the capture (a snapshot compacted it,
         * a leader change truncated it): try again soon, not after the
         * file doubles. */
        if (rc == EFS_ERR_STALE && !d->rot_stop)
            d->check_at = d->bytes + RAFT_ROTATE_MIN_BYTES;
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

int raft_log_rotator_start(struct efs_raft_disk *d)
{
    if (pthread_create(&d->rot_thread, NULL, rotator_main, d) != 0)
        return EFS_ERR_NOMEM; /* inline rotation still works */
    d->rot_thread_on = 1;
    return EFS_OK;
}

void raft_log_rotator_stop(struct efs_raft_disk *d)
{
    if (!d->rot_thread_on)
        return;
    pthread_mutex_lock(&d->mu);
    d->rot_stop = 1;
    pthread_cond_broadcast(&d->rot_cv);
    pthread_mutex_unlock(&d->mu);
    pthread_join(d->rot_thread, NULL);
    d->rot_thread_on = 0;
}

int efs_raft_disk_sync_hold(struct efs_raft_disk *d)
{
    if (!d)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&d->mu);
    if (BO_ON() && d->sync_hold == 0) {
        d->obs_hold_started = efs_batch_obs_now_us();
        BO_COUNT(BO_HOLD_OPEN);
    }
    d->sync_hold++;
    BO_HIST(BH_HOLD_DEPTH, d->sync_hold);
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
    if (BO_ON() && !d->sync_hold && d->obs_hold_started) {
        BO_HIST(BH_HOLD_US, efs_batch_obs_now_us()-d->obs_hold_started);
        BO_COUNT(BO_HOLD_CLOSE);
        d->obs_hold_started = 0;
    }
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
    if (BO_ON() && !d->sync_hold && d->obs_hold_started) {
        BO_HIST(BH_HOLD_US, efs_batch_obs_now_us()-d->obs_hold_started);
        BO_COUNT(BO_HOLD_CLOSE);
        d->obs_hold_started = 0;
    }
    mine = d->base + d->bytes; /* logical: survives a rotation swap */
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
        if (d->base + d->synced_bytes >= mine)
            break;
        if (d->sync_hold == 0)
            break;
        pthread_cond_wait(&d->cv, &d->mu);
    }
    pthread_mutex_unlock(&d->mu);
    return rc;
}
