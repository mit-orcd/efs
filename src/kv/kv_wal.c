#include "kv_lsm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/falloc.h>
#endif

#ifndef EFS_KV_WAL_RESERVE_BYTES
#define EFS_KV_WAL_RESERVE_BYTES (1024ull * 1024ull)
#endif

int kv_buf_set(struct kv_buf *b, const uint8_t *src, uint32_t n)
{
    if (n > b->cap) {
        uint8_t *p = realloc(b->p, n);
        if (!p)
            return EFS_ERR_NOMEM;
        b->p = p;
        b->cap = n;
    }
    if (n && src)
        memcpy(b->p, src, n);
    b->len = n;
    return EFS_OK;
}

void kv_buf_free(struct kv_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

struct kv_wal {
    int fd;
    int sync_mode;
    uint64_t bytes, allocated_bytes; /* Valid EOF never includes reserved zeros. */
    int prealloc_off;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    /* Group commit: appended counts records written, synced counts records
     * proven durable. A waiter needs synced >= its own append number; the
     * leader publishes the value it captured before its fsync, so it can
     * never claim a record that arrived after the fsync started. */
    uint64_t appended;
    uint64_t synced;
    int syncing;
    int io_failed; /* A failed append/sync cannot be followed by a successful ACK. */
    /* Nested: apply of one Raft entry writes many WAL records then one
     * fsync (arch §8: amortize the persistence boundary). */
    int hold;
    uint8_t *rec;
    uint32_t rec_cap;
};

static int wal_grow(struct kv_wal *w, uint32_t need)
{
    uint8_t *p;

    if (need <= w->rec_cap)
        return EFS_OK;
    p = realloc(w->rec, need);
    if (!p)
        return EFS_ERR_NOMEM;
    w->rec = p;
    w->rec_cap = need;
    return EFS_OK;
}


static int write_all(int fd, const uint8_t *p, size_t n, uint64_t off)
{
    while (n) {
        ssize_t k = pwrite(fd, p, n, (off_t)off);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_IO;
        }
        if (k == 0)
            return EFS_ERR_IO;
        p += (size_t)k;
        n -= (size_t)k;
        off += (uint64_t)k;
    }
    return EFS_OK;
}

/* Bounded EOF reservation amortizes small durable append allocation. Recovery
 * truncates the zero tail before reopen. Unsupported/full allocation is only
 * an optimization failure: ordinary exact writes still decide admission. */
static void wal_reserve(struct kv_wal *w, uint64_t end)
{
#ifdef __linux__
    const uint64_t chunk = EFS_KV_WAL_RESERVE_BYTES;
    uint64_t reserve;
    if (!chunk || w->prealloc_off || w->sync_mode != EFS_KV_LSM_SYNC ||
        end <= w->allocated_bytes || end > INT64_MAX - chunk) return;
    reserve = (end + chunk - 1) & ~(chunk - 1);
    int rc;
    do { rc = fallocate(w->fd, 0, (off_t)w->allocated_bytes,
                         (off_t)(reserve - w->allocated_bytes)); }
    while (rc < 0 && errno == EINTR);
    if (rc == 0) w->allocated_bytes = reserve;
    else w->prealloc_off = 1;
#else
    (void)w; (void)end;
#endif
}

int kv_wal_open(const char *path, int sync_mode, struct kv_wal **out)
{
    struct kv_wal *w;

    if (!path || !out)
        return EFS_ERR_INVAL;
    w = calloc(1, sizeof(*w));
    if (!w)
        return EFS_ERR_NOMEM;
    w->sync_mode = sync_mode;
    w->fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (w->fd < 0) {
        free(w);
        return EFS_ERR_IO;
    }
    struct stat st;
    if (fstat(w->fd, &st) != 0 || st.st_size < 0) {
        close(w->fd); free(w); return EFS_ERR_IO;
    }
    w->bytes = w->allocated_bytes = (uint64_t)st.st_size;
    if (pthread_mutex_init(&w->mu, NULL) != 0) {
        close(w->fd);
        free(w);
        return EFS_ERR_IO;
    }
    if (pthread_cond_init(&w->cv, NULL) != 0) {
        pthread_mutex_destroy(&w->mu);
        close(w->fd);
        free(w);
        return EFS_ERR_IO;
    }
    *out = w;
    return EFS_OK;
}

void kv_wal_close(struct kv_wal *w)
{
    if (!w)
        return;
    if (w->fd >= 0)
        close(w->fd);
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    free(w->rec);
    free(w);
}

static int wal_encode(struct kv_wal *w, const struct efs_kv_item *items,
                      uint32_t n, uint32_t *out_len)
{
    uint64_t need = 8 + 4;
    uint32_t i, off;
    int rc;

    for (i = 0; i < n; i++) {
        if (items[i].op != EFS_KV_PUT && items[i].op != EFS_KV_DEL)
            return EFS_ERR_INVAL;
        if (items[i].klen == 0 || items[i].klen > KV_LSM_KLEN_MAX || !items[i].key)
            return EFS_ERR_INVAL;
        /* A DEL carries no value: its val/vlen are not encoded and must not
         * be validated. Callers build DEL items from scan callbacks that
         * only set op/key/klen over an uninitialised stack array, and the
         * memory KV (unit tests) ignores vlen on DEL — so a vlen check here
         * made the reaper's LANE_SWEEP / rsv purge fail INVAL in production
         * only, nondeterministically (`apply lane-sweep rc=-5` ~9/s on the
         * leader, 124 REAP markers re-proposed every 2 s, nothing reclaimed). */
        if (items[i].op == EFS_KV_PUT) {
            if (items[i].vlen > KV_LSM_VLEN_MAX)
                return EFS_ERR_INVAL;
            if (items[i].vlen && !items[i].val)
                return EFS_ERR_INVAL;
        }
        need += 9 + items[i].klen;
        if (items[i].op == EFS_KV_PUT)
            need += items[i].vlen;
    }
    if (need > KV_LSM_REC_MAX)
        return EFS_ERR_INVAL;
    rc = wal_grow(w, (uint32_t)need);
    if (rc != EFS_OK)
        return rc;
    off = 8;
    efs_store_le32(w->rec + off, n);
    off += 4;
    for (i = 0; i < n; i++) {
        uint32_t vlen = items[i].op == EFS_KV_PUT ? items[i].vlen : 0;
        w->rec[off++] = (uint8_t)(items[i].op == EFS_KV_PUT ? KV_OP_PUT : KV_OP_DEL);
        efs_store_le32(w->rec + off, items[i].klen);
        off += 4;
        efs_store_le32(w->rec + off, vlen);
        off += 4;
        memcpy(w->rec + off, items[i].key, items[i].klen);
        off += items[i].klen;
        if (vlen) {
            memcpy(w->rec + off, items[i].val, vlen);
            off += vlen;
        }
    }
    efs_store_le32(w->rec, efs_crc32(w->rec + 8, off - 8));
    efs_store_le32(w->rec + 4, off - 8);
    *out_len = off;
    return EFS_OK;
}

/* Caller holds w->mu. */
static int kv_wal_fsync_to_locked(struct kv_wal *w, uint64_t mine)
{
    for (;;) {
        uint64_t target;
        int rc;

        if (w->io_failed)
            return EFS_ERR_IO;
        if (w->synced >= mine)
            return EFS_OK;
        if (w->syncing) {
            pthread_cond_wait(&w->cv, &w->mu);
            continue;
        }
        w->syncing = 1;
        target = w->appended;
        pthread_mutex_unlock(&w->mu);
        /* WAL recovery needs bytes and file length, not timestamp metadata.
         * fdatasync retains both durability requirements. */
        rc = fdatasync(w->fd) == 0 ? EFS_OK : EFS_ERR_IO;
        pthread_mutex_lock(&w->mu);
        w->syncing = 0;
        if (rc != EFS_OK) w->io_failed = 1;
        if (rc == EFS_OK && w->synced < target)
            w->synced = target;
        pthread_cond_broadcast(&w->cv);
        if (rc != EFS_OK)
            return rc;
    }
}

int kv_wal_append(struct kv_wal *w, const struct efs_kv_item *items, uint32_t n,
                  uint64_t *out_seq)
{
    uint32_t len = 0;
    uint64_t mine;
    int rc;

    if (!w || n == 0 || !items || !out_seq)
        return EFS_ERR_INVAL;
    *out_seq = 0;
    pthread_mutex_lock(&w->mu);
    if (w->io_failed) {
        pthread_mutex_unlock(&w->mu);
        return EFS_ERR_IO;
    }
    rc = wal_encode(w, items, n, &len);
    if (rc != EFS_OK) {
        pthread_mutex_unlock(&w->mu);
        return rc;
    }
    if (w->bytes > INT64_MAX - (uint64_t)len) {
        pthread_mutex_unlock(&w->mu);
        return EFS_ERR_IO;
    }
    wal_reserve(w, w->bytes + len);
    rc = write_all(w->fd, w->rec, len, w->bytes);
    if (rc != EFS_OK) {
        w->io_failed = 1;
        pthread_cond_broadcast(&w->cv);
        pthread_mutex_unlock(&w->mu);
        return rc;
    }
    w->bytes += len;
    if (w->allocated_bytes < w->bytes) w->allocated_bytes = w->bytes;
    mine = ++w->appended;
    *out_seq = mine;
    /* Hold defers fsync to kv_wal_hold(0). Memtable apply still happens
     * in log order so a later put in the same Raft entry sees this one. */
    if (w->sync_mode == EFS_KV_LSM_NOSYNC || w->hold > 0) {
        if (w->sync_mode == EFS_KV_LSM_NOSYNC && w->synced < mine)
            w->synced = mine;
        pthread_mutex_unlock(&w->mu);
        return EFS_OK;
    }
    rc = kv_wal_fsync_to_locked(w, mine);
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int kv_wal_hold(struct kv_wal *w, int on)
{
    int rc = EFS_OK;

    if (!w)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&w->mu);
    if (w->io_failed) {
        pthread_mutex_unlock(&w->mu);
        return EFS_ERR_IO;
    }
    if (on) {
        w->hold++;
    } else if (w->hold > 0) {
        w->hold--;
        if (w->hold == 0 && w->sync_mode != EFS_KV_LSM_NOSYNC)
            rc = kv_wal_fsync_to_locked(w, w->appended);
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

/* Transfer one outer hold to a bounded persistence job. The token names
 * records already appended, never records that arrive during its barrier. */
int kv_wal_detach(struct kv_wal *w,uint64_t *prefix)
{
    if(!w || !prefix)return EFS_ERR_INVAL;
    pthread_mutex_lock(&w->mu);
    int rc=w->io_failed?EFS_ERR_IO:w->hold!=1?EFS_ERR_BUSY:EFS_OK;
    if(!rc){*prefix=w->appended;w->hold=0;}
    pthread_mutex_unlock(&w->mu);return rc;
}

int kv_wal_sync_prefix(struct kv_wal *w,uint64_t prefix)
{
    if(!w)return EFS_ERR_INVAL;
    pthread_mutex_lock(&w->mu);
    int rc=prefix>w->appended?EFS_ERR_INVAL:w->io_failed?EFS_ERR_IO:EFS_OK;
    if(!rc && w->sync_mode!=EFS_KV_LSM_NOSYNC)rc=kv_wal_fsync_to_locked(w,prefix);
    pthread_mutex_unlock(&w->mu);return rc;
}

int kv_wal_sync_covered(struct kv_wal *w,uint64_t prefix,int *covered)
{
    if(!covered)return EFS_ERR_INVAL;
    *covered=0;
    if(!w)return EFS_ERR_INVAL;
    pthread_mutex_lock(&w->mu);
    int rc=w->io_failed?EFS_ERR_IO:prefix>w->appended?EFS_ERR_INVAL:EFS_OK;
    if(!rc)*covered=prefix<=w->synced;
    pthread_mutex_unlock(&w->mu);return rc;
}

int kv_wal_reset(struct kv_wal *w, uint64_t applied)
{
    int rc = EFS_OK;

    if (!w)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&w->mu);
    while (w->syncing)
        pthread_cond_wait(&w->cv, &w->mu);
    if (w->io_failed) {
        pthread_mutex_unlock(&w->mu);
        return EFS_ERR_IO;
    }
    if (w->appended > applied) {
        pthread_mutex_unlock(&w->mu);
        return EFS_OK;
    }
    if (ftruncate(w->fd, 0) != 0)
        rc = EFS_ERR_IO;
    else if (w->sync_mode == EFS_KV_LSM_SYNC && fsync(w->fd) != 0)
        rc = EFS_ERR_IO;
    if (rc == EFS_OK) {
        w->synced = w->appended;
        w->bytes = w->allocated_bytes = 0;
    } else
        w->io_failed = 1;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

static int read_exact(FILE *f, uint8_t *p, uint32_t n)
{
    if (fread(p, 1, n, f) == n)
        return EFS_OK;
    return ferror(f) ? EFS_ERR_IO : EFS_ERR_NOT_FOUND;
}

int kv_wal_replay(const char *path,
                  int (*cb)(void *user, uint8_t op, const uint8_t *key,
                            uint32_t klen, const uint8_t *val, uint32_t vlen),
                  void *user)
{
    FILE *f;
    uint8_t hdr[8];
    uint8_t *buf = NULL;
    uint32_t cap = 0;
    off_t valid_end = 0;
    int rc = EFS_OK, repair_tail = 0;

    if (!path || !cb)
        return EFS_ERR_INVAL;
    /* Recovery owns the WAL exclusively, before the append descriptor opens.
     * A discarded tail must be removed before any subsequent append. */
    f = fopen(path, "r+b");
    if (!f)
        return errno == ENOENT ? EFS_OK : EFS_ERR_IO;
    for (;;) {
        uint32_t crc, paylen, n, off, i;

        rc = read_exact(f, hdr, 8);
        if (rc != EFS_OK) {
            if (rc == EFS_ERR_NOT_FOUND) {
                off_t end = ftello(f);
                if (end < 0) rc = EFS_ERR_IO;
                else { repair_tail = end != valid_end; rc = EFS_OK; }
            }
            break;
        }
        crc = efs_load_le32(hdr);
        paylen = efs_load_le32(hdr + 4);
        if (paylen < 4 || paylen > KV_LSM_REC_MAX) {
            repair_tail = 1;
            break;
        }
        if (paylen > cap) {
            uint8_t *p = realloc(buf, paylen);
            if (!p) {
                rc = EFS_ERR_NOMEM;
                break;
            }
            buf = p;
            cap = paylen;
        }
        rc = read_exact(f, buf, paylen);
        if (rc != EFS_OK) {
            if (rc == EFS_ERR_NOT_FOUND) { repair_tail = 1; rc = EFS_OK; }
            break;
        }
        if (efs_crc32(buf, paylen) != crc) { repair_tail = 1; break; }
        n = efs_load_le32(buf);
        if (!n || n > (paylen - 4) / 10) { rc = EFS_ERR_PROTO; break; }
        /* Validate the entire atomic batch before applying its first item.
         * CRC-valid malformed data is an error, not authority to truncate. */
        off = 4;
        for (i = 0; i < n && rc == EFS_OK; i++) {
            uint8_t op;
            uint32_t klen, vlen;

            if (off + 9 > paylen) {
                rc = EFS_ERR_PROTO;
                break;
            }
            op = buf[off++];
            klen = efs_load_le32(buf + off);
            off += 4;
            vlen = efs_load_le32(buf + off);
            off += 4;
            if ((op != KV_OP_PUT && op != KV_OP_DEL) || !klen ||
                klen > KV_LSM_KLEN_MAX || vlen > KV_LSM_VLEN_MAX ||
                (op == KV_OP_DEL && vlen) ||
                (uint64_t)off + klen + vlen > paylen) {
                rc = EFS_ERR_PROTO;
                break;
            }
            off += klen + vlen;
        }
        if (rc == EFS_OK && off != paylen) rc = EFS_ERR_PROTO;
        if (rc != EFS_OK) break;
        off = 4;
        for (i = 0; i < n; i++) {
            uint8_t op = buf[off++];
            uint32_t klen = efs_load_le32(buf + off); off += 4;
            uint32_t vlen = efs_load_le32(buf + off); off += 4;
            rc = cb(user, op, buf + off, klen, buf + off + klen, vlen);
            if (rc != EFS_OK) break;
            off += klen + vlen;
        }
        if (rc != EFS_OK) break;
        valid_end = ftello(f);
        if (valid_end < 0) { rc = EFS_ERR_IO; break; }
    }
    /* Never repair after an I/O/callback/structural error: preserve evidence
     * and refuse open. Only the existing torn/CRC-invalid tail rule permits it. */
    if (rc == EFS_OK && repair_tail) {
        /* A damaged frame followed by nonzero records is not merely an EOF
         * reservation/torn tail. In particular, older builds could append
         * acknowledged bytes behind an invalid header. Preserve that evidence
         * and refuse recovery instead of silently erasing the later writes. */
        uint8_t tail[4096];
        size_t got;
        while (rc==EFS_OK && (got=fread(tail,1,sizeof(tail),f))!=0) {
            for(size_t j=0;j<got;j++) if(tail[j]) {rc=EFS_ERR_PROTO;break;}
        }
        if(ferror(f)) rc=EFS_ERR_IO;
    }
    if (rc == EFS_OK && repair_tail) {
        if (ftruncate(fileno(f), valid_end) != 0 ||
            fdatasync(fileno(f)) != 0) rc = EFS_ERR_IO;
    }
    free(buf);
    if (fclose(f) != 0 && rc == EFS_OK) rc = EFS_ERR_IO;
    return rc;
}
