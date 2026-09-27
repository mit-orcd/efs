#include "kv_lsm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

struct blk_ref {
    uint8_t *key; /* first key in the block */
    uint32_t klen;
    uint64_t off;
    uint32_t len;
};

/* Direct-mapped. A mkdir's negative dentry lookup used to pread one
 * block per segment and free it, so the next name in the same directory
 * paid the disk again. Slots keep those blocks resident. Each block is
 * about 8 KiB, so this is a couple of megabytes per segment. */
#define KV_SEG_CACHE_SLOTS 256

struct seg_cslot {
    uint32_t bi;
    uint32_t len;
    uint64_t off;
    uint8_t *blk;
    int valid;
};

struct kv_seg {
    int fd;
    struct blk_ref *idx;
    uint32_t nblocks;
    uint8_t *last_key;
    uint32_t last_klen;
    /* Owner holds one ref. A get pins a second ref before it drops the
     * LSM lock to pread, so compaction can unlink the segment without
     * closing the fd under that read. */
    uint32_t refs;
    char *path; /* open path; a private reopen for export */
    char *doom; /* unlink this path when refs hits 0 */
    struct seg_cslot cslot[KV_SEG_CACHE_SLOTS];
};

/* --- writer ---------------------------------------------------------- */

struct kv_seg_w {
    char *path;
    char *tmp;
    FILE *f;
    uint64_t off;
    uint8_t *blk;
    uint32_t blk_len;
    uint32_t blk_cap;
    uint64_t blk_off;
    struct blk_ref *idx;
    uint32_t nblocks;
    uint32_t idx_cap;
    uint8_t *prev_key;
    uint32_t prev_klen;
    uint32_t prev_cap;
    uint64_t nentries;
};

static int w_reserve_idx(struct kv_seg_w *w)
{
    struct blk_ref *p;
    uint32_t cap;

    if (w->nblocks < w->idx_cap)
        return EFS_OK;
    cap = w->idx_cap ? w->idx_cap * 2 : 32;
    p = realloc(w->idx, cap * sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    w->idx = p;
    w->idx_cap = cap;
    return EFS_OK;
}

static int w_flush_block(struct kv_seg_w *w)
{
    struct blk_ref *r;
    int rc;

    if (w->blk_len == 0)
        return EFS_OK;
    rc = w_reserve_idx(w);
    if (rc != EFS_OK)
        return rc;
    if (fwrite(w->blk, 1, w->blk_len, w->f) != w->blk_len)
        return EFS_ERR_IO;
    r = &w->idx[w->nblocks];
    /* The block's first key is at a known offset inside the buffer. */
    r->klen = get_u32(w->blk + 1);
    r->key = malloc(r->klen ? r->klen : 1);
    if (!r->key)
        return EFS_ERR_NOMEM;
    memcpy(r->key, w->blk + 9, r->klen);
    r->off = w->blk_off;
    r->len = w->blk_len;
    w->nblocks++;
    w->off += w->blk_len;
    w->blk_off = w->off;
    w->blk_len = 0;
    return EFS_OK;
}

int kv_seg_w_open(const char *path, struct kv_seg_w **out)
{
    struct kv_seg_w *w;
    size_t n;

    if (!path || !out)
        return EFS_ERR_INVAL;
    w = calloc(1, sizeof(*w));
    if (!w)
        return EFS_ERR_NOMEM;
    n = strlen(path);
    w->path = malloc(n + 1);
    w->tmp = malloc(n + 5);
    w->blk_cap = KV_LSM_BLOCK_TARGET + KV_LSM_KLEN_MAX + 64;
    w->blk = malloc(w->blk_cap);
    if (!w->path || !w->tmp || !w->blk) {
        free(w->path);
        free(w->tmp);
        free(w->blk);
        free(w);
        return EFS_ERR_NOMEM;
    }
    memcpy(w->path, path, n + 1);
    snprintf(w->tmp, n + 5, "%s.tmp", path);
    w->f = fopen(w->tmp, "wb");
    if (!w->f) {
        free(w->path);
        free(w->tmp);
        free(w->blk);
        free(w);
        return EFS_ERR_IO;
    }
    *out = w;
    return EFS_OK;
}

int kv_seg_w_add(struct kv_seg_w *w, uint8_t op, const uint8_t *key,
                 uint32_t klen, const uint8_t *val, uint32_t vlen)
{
    uint32_t need;
    int rc;

    if (!w || !key || klen == 0 || klen > KV_LSM_KLEN_MAX)
        return EFS_ERR_INVAL;
    if (op != KV_OP_PUT && op != KV_OP_DEL)
        return EFS_ERR_INVAL;
    if (op == KV_OP_DEL)
        vlen = 0;
    if (vlen > KV_LSM_VLEN_MAX || (vlen && !val))
        return EFS_ERR_INVAL;
    if (w->nentries && kv_key_cmp(key, klen, w->prev_key, w->prev_klen) <= 0)
        return EFS_ERR_INVAL;
    need = 9 + klen + vlen;
    if (w->blk_len && w->blk_len + need > KV_LSM_BLOCK_TARGET) {
        rc = w_flush_block(w);
        if (rc != EFS_OK)
            return rc;
    }
    if (w->blk_len + need > w->blk_cap) {
        uint8_t *p = realloc(w->blk, w->blk_len + need);
        if (!p)
            return EFS_ERR_NOMEM;
        w->blk = p;
        w->blk_cap = w->blk_len + need;
    }
    w->blk[w->blk_len] = op;
    put_u32(w->blk + w->blk_len + 1, klen);
    put_u32(w->blk + w->blk_len + 5, vlen);
    memcpy(w->blk + w->blk_len + 9, key, klen);
    if (vlen)
        memcpy(w->blk + w->blk_len + 9 + klen, val, vlen);
    w->blk_len += need;
    if (klen > w->prev_cap) {
        uint8_t *p = realloc(w->prev_key, klen);
        if (!p)
            return EFS_ERR_NOMEM;
        w->prev_key = p;
        w->prev_cap = klen;
    }
    memcpy(w->prev_key, key, klen);
    w->prev_klen = klen;
    w->nentries++;
    return EFS_OK;
}

uint64_t kv_seg_w_bytes(const struct kv_seg_w *w)
{
    return w ? w->off + w->blk_len : 0;
}

static void w_free(struct kv_seg_w *w)
{
    uint32_t i;

    for (i = 0; i < w->nblocks; i++)
        free(w->idx[i].key);
    free(w->idx);
    free(w->blk);
    free(w->prev_key);
    free(w->path);
    free(w->tmp);
    free(w);
}

int kv_seg_w_finish(struct kv_seg_w *w)
{
    uint8_t foot[KV_LSM_FOOTER_LEN];
    uint64_t idx_off;
    uint32_t idx_len = 0;
    uint32_t i;
    int rc;

    if (!w)
        return EFS_ERR_INVAL;
    if (w->nentries == 0) {
        rc = EFS_ERR_NOT_FOUND;
        goto out_abort;
    }
    rc = w_flush_block(w);
    if (rc != EFS_OK)
        goto out_abort;
    idx_off = w->off;
    for (i = 0; i < w->nblocks; i++) {
        uint8_t hdr[4];
        uint8_t tail[12];

        put_u32(hdr, w->idx[i].klen);
        if (fwrite(hdr, 1, 4, w->f) != 4 ||
            fwrite(w->idx[i].key, 1, w->idx[i].klen, w->f) != w->idx[i].klen) {
            rc = EFS_ERR_IO;
            goto out_abort;
        }
        put_u64(tail, w->idx[i].off);
        put_u32(tail + 8, w->idx[i].len);
        if (fwrite(tail, 1, 12, w->f) != 12) {
            rc = EFS_ERR_IO;
            goto out_abort;
        }
        idx_len += 16 + w->idx[i].klen;
    }
    memset(foot, 0, sizeof(foot));
    put_u64(foot, idx_off);
    put_u32(foot + 8, idx_len);
    put_u32(foot + 12, w->nblocks);
    put_u32(foot + 16, KV_LSM_SEG_MAGIC);
    put_u32(foot + 20, KV_LSM_SEG_VERSION);
    put_u32(foot + 24, efs_crc32(foot, 24));
    if (fwrite(foot, 1, sizeof(foot), w->f) != sizeof(foot)) {
        rc = EFS_ERR_IO;
        goto out_abort;
    }
    if (fflush(w->f) != 0 || fsync(fileno(w->f)) != 0) {
        rc = EFS_ERR_IO;
        goto out_abort;
    }
    fclose(w->f);
    w->f = NULL;
    /* Rename last: a crash before this leaves only a .tmp, which recovery
     * ignores, so a half-written segment is never in the live set. */
    if (rename(w->tmp, w->path) != 0) {
        unlink(w->tmp);
        w_free(w);
        return EFS_ERR_IO;
    }
    w_free(w);
    return EFS_OK;

out_abort:
    if (w->f)
        fclose(w->f);
    unlink(w->tmp);
    w_free(w);
    return rc;
}

void kv_seg_w_abort(struct kv_seg_w *w)
{
    if (!w)
        return;
    if (w->f)
        fclose(w->f);
    unlink(w->tmp);
    w_free(w);
}

/* --- reader ---------------------------------------------------------- */

static int pread_all(int fd, uint8_t *p, uint32_t n, uint64_t off)
{
    while (n) {
        ssize_t k = pread(fd, p, n, (off_t)off);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_IO;
        }
        if (k == 0)
            return EFS_ERR_PROTO;
        p += (size_t)k;
        off += (uint64_t)k;
        n -= (uint32_t)k;
    }
    return EFS_OK;
}

static void seg_free_idx(struct kv_seg *s)
{
    uint32_t i;

    for (i = 0; i < s->nblocks; i++)
        free(s->idx[i].key);
    free(s->idx);
    s->idx = NULL;
    s->nblocks = 0;
}

int kv_seg_open(const char *path, struct kv_seg **out)
{
    struct kv_seg *s;
    uint8_t foot[KV_LSM_FOOTER_LEN];
    uint8_t *ibuf = NULL;
    uint64_t idx_off;
    uint32_t idx_len, nblocks, off = 0, i;
    off_t fsz;
    int rc = EFS_OK;

    if (!path || !out)
        return EFS_ERR_INVAL;
    s = calloc(1, sizeof(*s));
    if (!s)
        return EFS_ERR_NOMEM;
    s->refs = 1;
    s->path = strdup(path);
    if (!s->path) {
        free(s);
        return EFS_ERR_NOMEM;
    }
    s->fd = open(path, O_RDONLY);
    if (s->fd < 0) {
        free(s->path);
        free(s);
        return EFS_ERR_NOT_FOUND;
    }
    fsz = lseek(s->fd, 0, SEEK_END);
    if (fsz < (off_t)KV_LSM_FOOTER_LEN) {
        rc = EFS_ERR_PROTO;
        goto fail;
    }
    rc = pread_all(s->fd, foot, KV_LSM_FOOTER_LEN,
                   (uint64_t)fsz - KV_LSM_FOOTER_LEN);
    if (rc != EFS_OK)
        goto fail;
    if (get_u32(foot + 16) != KV_LSM_SEG_MAGIC ||
        get_u32(foot + 20) != KV_LSM_SEG_VERSION ||
        get_u32(foot + 24) != efs_crc32(foot, 24)) {
        rc = EFS_ERR_PROTO;
        goto fail;
    }
    idx_off = get_u64(foot);
    idx_len = get_u32(foot + 8);
    nblocks = get_u32(foot + 12);
    if (nblocks == 0 || nblocks > (1u << 24) || idx_len == 0 ||
        idx_off + idx_len + KV_LSM_FOOTER_LEN > (uint64_t)fsz) {
        rc = EFS_ERR_PROTO;
        goto fail;
    }
    ibuf = malloc(idx_len);
    s->idx = calloc(nblocks, sizeof(*s->idx));
    if (!ibuf || !s->idx) {
        rc = EFS_ERR_NOMEM;
        goto fail;
    }
    rc = pread_all(s->fd, ibuf, idx_len, idx_off);
    if (rc != EFS_OK)
        goto fail;
    for (i = 0; i < nblocks; i++) {
        uint32_t klen;

        if (off + 4 > idx_len) {
            rc = EFS_ERR_PROTO;
            goto fail;
        }
        klen = get_u32(ibuf + off);
        off += 4;
        if (klen == 0 || klen > KV_LSM_KLEN_MAX || off + klen + 12 > idx_len) {
            rc = EFS_ERR_PROTO;
            goto fail;
        }
        s->idx[i].key = malloc(klen);
        if (!s->idx[i].key) {
            rc = EFS_ERR_NOMEM;
            goto fail;
        }
        memcpy(s->idx[i].key, ibuf + off, klen);
        s->idx[i].klen = klen;
        off += klen;
        s->idx[i].off = get_u64(ibuf + off);
        s->idx[i].len = get_u32(ibuf + off + 8);
        off += 12;
        s->nblocks = i + 1;
        if (s->idx[i].off + s->idx[i].len > idx_off) {
            rc = EFS_ERR_PROTO;
            goto fail;
        }
    }
    free(ibuf);
    *out = s;
    return EFS_OK;

fail:
    free(ibuf);
    seg_free_idx(s);
    close(s->fd);
    free(s->path);
    free(s);
    return rc;
}

static void seg_free(struct kv_seg *s)
{
    uint32_t i;

    if (!s)
        return;
    seg_free_idx(s);
    free(s->last_key);
    for (i = 0; i < KV_SEG_CACHE_SLOTS; i++)
        free(s->cslot[i].blk);
    if (s->fd >= 0)
        close(s->fd);
    free(s->path);
    free(s->doom);
    free(s);
}

const char *kv_seg_filepath(const struct kv_seg *s)
{
    return s ? s->path : NULL;
}

void kv_seg_doom(struct kv_seg *s, const char *path)
{
    if (!s || !path)
        return;
    free(s->doom);
    s->doom = strdup(path);
    if (!s->doom)
        unlink(path);
}

void kv_seg_pin(struct kv_seg *s)
{
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_ACQ_REL);
}

void kv_seg_unpin(struct kv_seg *s)
{
    if (!s)
        return;
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        if (s->doom)
            unlink(s->doom);
        seg_free(s);
    }
}

void kv_seg_close(struct kv_seg *s)
{
    if (s)
        kv_seg_unpin(s);
}

int kv_seg_first_key(struct kv_seg *s, const uint8_t **key, uint32_t *klen)
{
    if (!s || !s->nblocks)
        return EFS_ERR_NOT_FOUND;
    *key = s->idx[0].key;
    *klen = s->idx[0].klen;
    return EFS_OK;
}

/* First learner publishes. A pinned view can read the segment while
 * compaction learns the same last key; the loser frees its copy. */
static void seg_note_last(struct kv_seg *s, const uint8_t *tail, uint32_t tail_kl)
{
    uint8_t *p, *cur;

    if (!tail || !tail_kl)
        return;
    cur = __atomic_load_n(&s->last_key, __ATOMIC_ACQUIRE);
    if (cur)
        return;
    p = malloc(tail_kl);
    if (!p)
        return;
    memcpy(p, tail, tail_kl);
    __atomic_store_n(&s->last_klen, tail_kl, __ATOMIC_RELAXED);
    if (!__atomic_compare_exchange_n(&s->last_key, &cur, p, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        free(p);
}

/* Blocks are ordered, so the last key is the last entry of the last block. */
int kv_seg_last_key(struct kv_seg *s, const uint8_t **key, uint32_t *klen)
{
    struct blk_ref *r;
    uint8_t *blk, *have;
    uint32_t off = 0;
    int rc;

    if (!s || !s->nblocks)
        return EFS_ERR_NOT_FOUND;
    have = __atomic_load_n(&s->last_key, __ATOMIC_ACQUIRE);
    if (have) {
        *key = have;
        *klen = __atomic_load_n(&s->last_klen, __ATOMIC_RELAXED);
        return EFS_OK;
    }
    r = &s->idx[s->nblocks - 1];
    blk = malloc(r->len);
    if (!blk)
        return EFS_ERR_NOMEM;
    rc = pread_all(s->fd, blk, r->len, r->off);
    if (rc != EFS_OK) {
        free(blk);
        return rc;
    }
    {
        const uint8_t *tail = NULL;
        uint32_t tail_kl = 0;

        while (off + 9 <= r->len) {
            uint32_t kl = get_u32(blk + off + 1);
            uint32_t vl = get_u32(blk + off + 5);

            if (off + 9 + kl + vl > r->len)
                break;
            tail = blk + off + 9;
            tail_kl = kl;
            off += 9 + kl + vl;
        }
        seg_note_last(s, tail, tail_kl);
    }
    free(blk);
    have = __atomic_load_n(&s->last_key, __ATOMIC_ACQUIRE);
    if (!have)
        return EFS_ERR_PROTO;
    *key = have;
    *klen = __atomic_load_n(&s->last_klen, __ATOMIC_RELAXED);
    return EFS_OK;
}

/* Last block whose first key is <= key; -1 when key precedes the segment. */
static int32_t block_for(struct kv_seg *s, const uint8_t *key, uint32_t klen)
{
    int32_t lo = 0, hi = (int32_t)s->nblocks - 1, ans = -1;

    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (kv_key_cmp(s->idx[mid].key, s->idx[mid].klen, key, klen) <= 0) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

/* Search one immutable block. A miss is cached by the caller too: the
 * next lookup of a nearby key (the next name in a directory) must not
 * pread this block again. */
static int search_block(struct kv_seg *s, const uint8_t *blk, uint32_t len,
                        uint32_t bi, const uint8_t *key, uint32_t klen,
                        struct kv_buf *val, uint8_t *op)
{
    uint32_t off = 0;
    const uint8_t *tail = NULL;
    uint32_t tail_kl = 0;
    int saw_end = 1;
    int rc = EFS_ERR_NOT_FOUND;

    while (off + 9 <= len) {
        uint8_t o = blk[off];
        uint32_t kl = get_u32(blk + off + 1);
        uint32_t vl = get_u32(blk + off + 5);
        int c;

        if (off + 9 + kl + vl > len)
            break;
        c = kv_key_cmp(blk + off + 9, kl, key, klen);
        if (c == 0) {
            *op = o;
            rc = kv_buf_set(val, blk + off + 9 + kl, vl);
            saw_end = 0;
            break;
        }
        if (c > 0) {
            saw_end = 0;
            break;
        }
        tail = blk + off + 9;
        tail_kl = kl;
        off += 9 + kl + vl;
    }
    if (saw_end && bi + 1 == s->nblocks)
        seg_note_last(s, tail, tail_kl);
    return rc;
}

int kv_seg_probe(struct kv_seg *s, const uint8_t *key, uint32_t klen,
                 struct kv_buf *val, uint8_t *op, int *need_io,
                 struct kv_seg_io *io)
{
    struct blk_ref *r;
    struct seg_cslot *c;
    int32_t bi;

    if (need_io)
        *need_io = 0;
    if (!s || !key || klen == 0 || !val || !op || !need_io || !io)
        return EFS_ERR_INVAL;
    /* Key past this segment. Learned the first time the last block is
     * scanned to the end; later misses must not pread or evict. */
    {
        uint8_t *lk = __atomic_load_n(&s->last_key, __ATOMIC_ACQUIRE);

        if (lk && kv_key_cmp(key, klen, lk,
                             __atomic_load_n(&s->last_klen, __ATOMIC_RELAXED)) > 0)
            return EFS_ERR_NOT_FOUND;
    }
    bi = block_for(s, key, klen);
    if (bi < 0)
        return EFS_ERR_NOT_FOUND;
    r = &s->idx[bi];
    c = &s->cslot[(uint32_t)bi % KV_SEG_CACHE_SLOTS];
    if (c->valid && c->bi == (uint32_t)bi && c->len == r->len &&
        c->off == r->off && c->blk)
        return search_block(s, c->blk, c->len, (uint32_t)bi, key, klen, val,
                            op);
    io->fd = s->fd;
    io->off = r->off;
    io->len = r->len;
    io->bi = (uint32_t)bi;
    *need_io = 1;
    return EFS_OK;
}

int kv_seg_read(const struct kv_seg_io *io, uint8_t **blk)
{
    uint8_t *p;

    if (!io || !blk || !io->len || io->fd < 0)
        return EFS_ERR_INVAL;
    p = malloc(io->len);
    if (!p)
        return EFS_ERR_NOMEM;
    if (pread_all(io->fd, p, io->len, io->off) != EFS_OK) {
        free(p);
        return EFS_ERR_IO;
    }
    *blk = p;
    return EFS_OK;
}

int kv_seg_install(struct kv_seg *s, const struct kv_seg_io *io, uint8_t *blk,
                   const uint8_t *key, uint32_t klen, struct kv_buf *val,
                   uint8_t *op)
{
    struct seg_cslot *c;

    if (!s || !io || !blk || !key || !val || !op)
        return EFS_ERR_INVAL;
    if (io->bi >= s->nblocks || s->idx[io->bi].off != io->off ||
        s->idx[io->bi].len != io->len) {
        free(blk);
        return EFS_ERR_AGAIN;
    }
    c = &s->cslot[io->bi % KV_SEG_CACHE_SLOTS];
    free(c->blk);
    c->blk = blk;
    c->bi = io->bi;
    c->len = io->len;
    c->off = io->off;
    c->valid = 1;
    return search_block(s, blk, io->len, io->bi, key, klen, val, op);
}

int kv_seg_get(struct kv_seg *s, const uint8_t *key, uint32_t klen,
               struct kv_buf *val, uint8_t *op)
{
    struct kv_seg_io io;
    uint8_t *blk = NULL;
    int need_io = 0;
    int rc = kv_seg_probe(s, key, klen, val, op, &need_io, &io);

    if (rc != EFS_OK || !need_io)
        return rc;
    rc = kv_seg_read(&io, &blk);
    if (rc != EFS_OK)
        return rc;
    return kv_seg_install(s, &io, blk, key, klen, val, op);
}

/* True when no key of this segment can be >= seek and carry prefix.
 * The range test is the one compaction uses (first key from the block
 * index, last key learned once per segment). Unknown = not excluded. */
int kv_seg_excludes(struct kv_seg *s, const uint8_t *seek, uint32_t slen,
                    const uint8_t *prefix, uint32_t plen)
{
    const uint8_t *fk = NULL, *lk = NULL;
    uint32_t fl = 0, ll = 0;

    if (!s || !s->nblocks)
        return 1;
    if (plen && prefix) {
        if (kv_seg_first_key(s, &fk, &fl) == EFS_OK &&
            !kv_has_prefix(fk, fl, prefix, plen) &&
            kv_key_cmp(fk, fl, prefix, plen) > 0)
            return 1;
    }
    if (slen && seek) {
        if (kv_seg_last_key(s, &lk, &ll) == EFS_OK &&
            kv_key_cmp(lk, ll, seek, slen) < 0)
            return 1;
    }
    return 0;
}

/* --- iterator -------------------------------------------------------- */

struct kv_seg_iter {
    struct kv_seg *s;
    uint32_t bi;
    uint32_t off;
    uint8_t *blk;
    uint32_t blk_len;
    int loaded;
};

int kv_seg_iter_open(struct kv_seg *s, struct kv_seg_iter **out)
{
    struct kv_seg_iter *it;

    if (!s || !out)
        return EFS_ERR_INVAL;
    it = calloc(1, sizeof(*it));
    if (!it)
        return EFS_ERR_NOMEM;
    it->s = s;
    *out = it;
    return EFS_OK;
}

void kv_seg_iter_close(struct kv_seg_iter *it)
{
    if (!it)
        return;
    free(it->blk);
    free(it);
}

static int iter_load(struct kv_seg_iter *it)
{
    struct blk_ref *r;
    uint8_t *p;

    if (it->bi >= it->s->nblocks)
        return EFS_ERR_NOT_FOUND;
    r = &it->s->idx[it->bi];
    p = realloc(it->blk, r->len);
    if (!p)
        return EFS_ERR_NOMEM;
    it->blk = p;
    /* A block a point get just read is in the segment's slots; iterators
     * run under the LSM lock like the install that filled them. */
    {
        struct seg_cslot *c = &it->s->cslot[it->bi % KV_SEG_CACHE_SLOTS];

        if (c->valid && c->bi == it->bi && c->len == r->len &&
            c->off == r->off && c->blk) {
            memcpy(it->blk, c->blk, r->len);
            it->blk_len = r->len;
            it->off = 0;
            it->loaded = 1;
            return EFS_OK;
        }
    }
    if (pread_all(it->s->fd, it->blk, r->len, r->off) != EFS_OK)
        return EFS_ERR_IO;
    it->blk_len = r->len;
    it->off = 0;
    it->loaded = 1;
    return EFS_OK;
}

int kv_seg_iter_seek(struct kv_seg_iter *it, const uint8_t *seek, uint32_t plen)
{
    int32_t bi;

    if (!it)
        return EFS_ERR_INVAL;
    it->loaded = 0;
    it->off = 0;
    if (plen == 0 || !seek) {
        it->bi = 0;
        return EFS_OK;
    }
    bi = block_for(it->s, seek, plen);
    it->bi = bi < 0 ? 0 : (uint32_t)bi;
    return EFS_OK;
}

int kv_seg_iter_next(struct kv_seg_iter *it, const uint8_t **key, uint32_t *klen,
                     const uint8_t **val, uint32_t *vlen, uint8_t *op)
{
    if (!it || !key || !klen || !val || !vlen || !op)
        return EFS_ERR_INVAL;
    for (;;) {
        uint32_t kl, vl;
        int rc;

        if (!it->loaded) {
            rc = iter_load(it);
            if (rc != EFS_OK)
                return rc;
        }
        if (it->off + 9 > it->blk_len) {
            it->bi++;
            it->loaded = 0;
            continue;
        }
        kl = get_u32(it->blk + it->off + 1);
        vl = get_u32(it->blk + it->off + 5);
        if (it->off + 9 + kl + vl > it->blk_len)
            return EFS_ERR_PROTO;
        *op = it->blk[it->off];
        *key = it->blk + it->off + 9;
        *klen = kl;
        *val = it->blk + it->off + 9 + kl;
        *vlen = vl;
        it->off += 9 + kl + vl;
        return EFS_OK;
    }
}
