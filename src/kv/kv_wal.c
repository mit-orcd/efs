#include "kv_lsm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
    pthread_mutex_t mu;
    pthread_cond_t cv;
    /* Group commit: appended counts records written, synced counts records
     * proven durable. A waiter needs synced >= its own append number; the
     * leader publishes the value it captured before its fsync, so it can
     * never claim a record that arrived after the fsync started. */
    uint64_t appended;
    uint64_t synced;
    int syncing;
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

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_IO;
        }
        if (k == 0)
            return EFS_ERR_IO;
        p += (size_t)k;
        n -= (size_t)k;
    }
    return EFS_OK;
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
    w->fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (w->fd < 0) {
        free(w);
        return EFS_ERR_IO;
    }
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
    put_u32(w->rec + off, n);
    off += 4;
    for (i = 0; i < n; i++) {
        uint32_t vlen = items[i].op == EFS_KV_PUT ? items[i].vlen : 0;
        w->rec[off++] = (uint8_t)(items[i].op == EFS_KV_PUT ? KV_OP_PUT : KV_OP_DEL);
        put_u32(w->rec + off, items[i].klen);
        off += 4;
        put_u32(w->rec + off, vlen);
        off += 4;
        memcpy(w->rec + off, items[i].key, items[i].klen);
        off += items[i].klen;
        if (vlen) {
            memcpy(w->rec + off, items[i].val, vlen);
            off += vlen;
        }
    }
    put_u32(w->rec, efs_crc32(w->rec + 8, off - 8));
    put_u32(w->rec + 4, off - 8);
    *out_len = off;
    return EFS_OK;
}

/* Caller holds w->mu. */
static int kv_wal_fsync_to_locked(struct kv_wal *w, uint64_t mine)
{
    for (;;) {
        uint64_t target;
        int rc;

        if (w->synced >= mine)
            return EFS_OK;
        if (w->syncing) {
            pthread_cond_wait(&w->cv, &w->mu);
            continue;
        }
        w->syncing = 1;
        target = w->appended;
        pthread_mutex_unlock(&w->mu);
        rc = fsync(w->fd) == 0 ? EFS_OK : EFS_ERR_IO;
        pthread_mutex_lock(&w->mu);
        w->syncing = 0;
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
    rc = wal_encode(w, items, n, &len);
    if (rc != EFS_OK) {
        pthread_mutex_unlock(&w->mu);
        return rc;
    }
    rc = write_all(w->fd, w->rec, len);
    if (rc != EFS_OK) {
        pthread_mutex_unlock(&w->mu);
        return rc;
    }
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

int kv_wal_reset(struct kv_wal *w, uint64_t applied)
{
    int rc = EFS_OK;

    if (!w)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&w->mu);
    while (w->syncing)
        pthread_cond_wait(&w->cv, &w->mu);
    if (w->appended > applied) {
        pthread_mutex_unlock(&w->mu);
        return EFS_OK;
    }
    if (ftruncate(w->fd, 0) != 0)
        rc = EFS_ERR_IO;
    else if (w->sync_mode == EFS_KV_LSM_SYNC && fsync(w->fd) != 0)
        rc = EFS_ERR_IO;
    if (rc == EFS_OK)
        w->synced = w->appended;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

static int read_exact(FILE *f, uint8_t *p, uint32_t n)
{
    return fread(p, 1, n, f) == n ? EFS_OK : EFS_ERR_NOT_FOUND;
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
    int rc = EFS_OK;

    if (!path || !cb)
        return EFS_ERR_INVAL;
    f = fopen(path, "rb");
    if (!f)
        return EFS_OK;
    for (;;) {
        uint32_t crc, paylen, n, off, i;

        if (read_exact(f, hdr, 8) != EFS_OK)
            break;
        crc = get_u32(hdr);
        paylen = get_u32(hdr + 4);
        if (paylen < 4 || paylen > KV_LSM_REC_MAX)
            break;
        if (paylen > cap) {
            uint8_t *p = realloc(buf, paylen);
            if (!p) {
                rc = EFS_ERR_NOMEM;
                break;
            }
            buf = p;
            cap = paylen;
        }
        if (read_exact(f, buf, paylen) != EFS_OK)
            break;
        if (efs_crc32(buf, paylen) != crc)
            break;
        n = get_u32(buf);
        off = 4;
        for (i = 0; i < n && rc == EFS_OK; i++) {
            uint8_t op;
            uint32_t klen, vlen;

            if (off + 9 > paylen) {
                rc = EFS_ERR_PROTO;
                break;
            }
            op = buf[off++];
            klen = get_u32(buf + off);
            off += 4;
            vlen = get_u32(buf + off);
            off += 4;
            if ((uint64_t)off + klen + vlen > paylen) {
                rc = EFS_ERR_PROTO;
                break;
            }
            rc = cb(user, op, buf + off, klen, buf + off + klen, vlen);
            off += klen + vlen;
        }
        if (rc != EFS_OK)
            break;
    }
    free(buf);
    fclose(f);
    return rc;
}
