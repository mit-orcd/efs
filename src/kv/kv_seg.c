#include "kv_lsm_internal.h"

#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

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
/* Divides KV_SEG_CACHE_SLOTS, so a slot always maps to one stripe. */
#define KV_SEG_CMU_STRIPES 32
/* Optional indexes are bounded independently of block/value size. */
#define KV_BLOCK_HASH_BYTES_MAX (16u * 1024u)
#define KV_SEG_HASH_BYTES_MAX (128u * 1024u)

struct seg_cslot {
    uint32_t bi;
    uint32_t len;
    uint64_t off;
    uint8_t *blk;
    uint32_t *entries; /* offsets of validated sorted records in blk */
    uint32_t nentries;
    uint32_t *hash; /* record offset + 1; zero is an empty bucket */
    uint32_t hash_slots;
    int valid;
};

/* Point-get filter over every record key (PUTs and tombstones) of an
 * immutable segment. A getattr is several point gets, half of them for
 * keys that exist in no segment (intent keys), and each miss used to
 * binary-search a block in every overlapping segment under the LSM lock
 * (Oct 15 Intel profile: getattr was a third of efsd CPU). Built off the
 * request path from the segment file itself; until it is published the
 * probe behaves exactly as before. A false positive only costs the probe. */
struct seg_bloom {
    uint64_t mask; /* bits - 1, bits a power of two */
    uint64_t w[];
};
#define SEG_BLOOM_K 6

struct kv_seg {
    int fd;
    struct blk_ref *idx;
    uint32_t nblocks;
    uint8_t *last_key;
    uint32_t last_klen;
    /* kv_key_range of idx[0] and of last_key, fixed at open. */
    int rlo;
    int rhi;
    /* Sum of block lengths, fixed at open. The compactor asks for it
     * per range per pass; a walk of 230K index entries under the LSM
     * lock is not what a byte count should cost. */
    uint64_t data_bytes;
    /* Owner holds one ref. A get pins a second ref before it drops the
     * LSM lock to pread, so compaction can unlink the segment without
     * closing the fd under that read. */
    uint32_t refs;
    char *path; /* open path; a private reopen for export */
    char *doom; /* unlink this path when refs hits 0 */
    uint32_t hash_bytes; /* soft budget across slots; atomic */
    struct seg_cslot cslot[KV_SEG_CACHE_SLOTS];
    /* Shared LSM readers probe, install and copy cached blocks in parallel.
     * cmu[bi % KV_SEG_CMU_STRIPES] guards the slot(s) of block bi; one lock
     * per segment serialized every reader of a hot segment (Oct 16 Intel:
     * ~14k waits per 10 s in scan seeks on the metadata leader). */
    pthread_mutex_t cmu[KV_SEG_CMU_STRIPES];
    struct seg_bloom *bloom; /* published once, read lock-free */
};

static pthread_mutex_t *seg_cmu(struct kv_seg *s, uint32_t bi)
{
    return &s->cmu[bi % KV_SEG_CMU_STRIPES];
}

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
    /* stdio's default buffer is st_blksize (4 KiB), so a 250 MiB
     * compaction output was 64K write() calls (5.25M of them in one
     * 20-minute trace, Sep 29). One syscall per MiB instead. */
    setvbuf(w->f, NULL, _IOFBF, 1u << 20);
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
    for (unsigned i = 0; i < KV_SEG_CMU_STRIPES; i++)
        pthread_mutex_init(&s->cmu[i], NULL);
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
        s->data_bytes += s->idx[i].len;
    }
    free(ibuf);
    /* Cache the last key now. A point get rejects this segment from the
     * in-memory span; learning it on the first miss would pread under the
     * lookup. A failure leaves the key unknown and the get falls back to
     * the block search. */
    {
        const uint8_t *lk = NULL;
        uint32_t ll = 0;

        s->rlo = kv_key_range(s->idx[0].key, s->idx[0].klen);
        s->rhi = KV_RANGE_N - 1;
        if (kv_seg_last_key(s, &lk, &ll) == EFS_OK)
            s->rhi = kv_key_range(lk, ll);
        if (s->rhi < s->rlo)
            s->rhi = s->rlo;
    }
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
    for (i = 0; i < KV_SEG_CACHE_SLOTS; i++) {
        free(s->cslot[i].blk);
        free(s->cslot[i].entries);
        free(s->cslot[i].hash);
    }
    if (s->fd >= 0)
        close(s->fd);
    free(s->bloom);
    for (unsigned i = 0; i < KV_SEG_CMU_STRIPES; i++)
        pthread_mutex_destroy(&s->cmu[i]);
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

uint64_t kv_seg_data_bytes(const struct kv_seg *s)
{
    return s ? s->data_bytes : 0;
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

/* Index the variable-length records once when a block enters the bounded
 * cache. Repeated point gets use binary search instead of comparing every
 * preceding record. No format change or unbounded per-segment allocation.
 * Validate the entire block before publishing it: malformed lengths/order are
 * corruption, never a negative lookup or a partially usable cache entry. */
static int index_block(const uint8_t *blk, uint32_t len,
                       uint32_t **entries, uint32_t *count)
{
    uint32_t off = 0, n = 0;
    const uint8_t *prev = NULL; uint32_t prev_len = 0;
    while (off < len) {
        if (len - off < 9) return EFS_ERR_PROTO;
        uint32_t kl = get_u32(blk + off + 1), vl = get_u32(blk + off + 5);
        if (!kl || kl > KV_LSM_KLEN_MAX || vl > KV_LSM_VLEN_MAX ||
            kl > len - off - 9 || vl > len - off - 9 - kl ||
            (blk[off] != KV_OP_PUT && blk[off] != KV_OP_DEL) ||
            (blk[off] == KV_OP_DEL && vl) ||
            (prev && kv_key_cmp(prev, prev_len, blk + off + 9, kl) >= 0))
            return EFS_ERR_PROTO;
        prev = blk + off + 9; prev_len = kl;
        off += 9 + kl + vl; ++n;
    }
    if (!n) return EFS_ERR_PROTO;
    uint32_t *idx = malloc((size_t)n * sizeof(*idx));
    if (!idx) return EFS_ERR_NOMEM;
    off = 0;
    for (uint32_t i = 0; i < n; ++i) {
        idx[i] = off;
        off += 9 + get_u32(blk + off + 1) + get_u32(blk + off + 5);
    }
    *entries = idx; *count = n;
    return EFS_OK;
}

static int search_block_binary(const struct seg_cslot *c, const uint8_t *key,
                         uint32_t klen, struct kv_buf *val, uint8_t *op)
{
    uint32_t lo = 0, hi = c->nentries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t off = c->entries[mid];
        uint32_t kl = get_u32(c->blk + off + 1);
        int cmp = kv_key_cmp(c->blk + off + 9, kl, key, klen);
        if (cmp < 0) lo = mid + 1;
        else if (cmp > 0) hi = mid;
        else {
            *op = c->blk[off];
            return kv_buf_set(val, c->blk + off + 9 + kl, get_u32(c->blk + off + 5));
        }
    }
    return EFS_ERR_NOT_FOUND;
}

/* Full keys include embedded zeros and shared inode/lane prefixes. This is
 * only an accelerator: every occupied-bucket hit verifies length and bytes. */
static uint64_t block_key_hash(const uint8_t *key, uint32_t klen)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    /* Native builds may use CRC instructions; this is an in-memory routing
     * hash, never a persisted checksum or an equality decision. */
    uint64_t h = klen;
    while (klen >= 8) {
        uint64_t word; memcpy(&word, key, sizeof(word));
        h = _mm_crc32_u64(h, word); key += 8; klen -= 8;
    }
    if (klen >= 4) {
        uint32_t word; memcpy(&word, key, sizeof(word));
        h = _mm_crc32_u32((uint32_t)h, word); key += 4; klen -= 4;
    }
    if (klen >= 2) {
        uint16_t word; memcpy(&word, key, sizeof(word));
        h = _mm_crc32_u16((uint32_t)h, word); key += 2; klen -= 2;
    }
    if (klen) h = _mm_crc32_u8((uint32_t)h, *key);
    return h;
#else
    const uint64_t m = UINT64_C(0xc6a4a7935bd1e995);
    uint64_t h = UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)klen * m);
    /* memcpy makes unaligned full-word loads defined on every architecture.
     * Unlike byte-at-a-time hashing this also keeps long lane keys cheap. */
    while (klen >= 8) {
        uint64_t word;
        memcpy(&word, key, sizeof(word));
        word *= m; word ^= word >> 47; word *= m;
        h ^= word; h *= m;
        key += 8; klen -= 8;
    }
    uint64_t tail = 0;
    for (uint32_t i = 0; i < klen; ++i)
        tail |= (uint64_t)key[i] << (8 * i);
    h ^= tail; h *= m;
    h ^= h >> 47; h *= m; h ^= h >> 47;
    return h;
#endif
}

/* Called only after complete block and inter-block ordering validation.
 * Keep binary search for small/oversized blocks, budget or allocation refusal.
 * The table has at least twice as many buckets as records, so probing ends. */
static uint32_t block_hash_build(struct seg_cslot *c, uint32_t available)
{
    if (c->nentries < 8 || c->nentries > KV_BLOCK_HASH_BYTES_MAX / 8)
        return 0;
    uint32_t slots = 16;
    while (slots < 2 * c->nentries)
        slots *= 2;
    uint32_t bytes = slots * sizeof(uint32_t);
    if (bytes > available)
        return 0;
    uint32_t *table = calloc(slots, sizeof(*table));
    if (!table)
        return 0;
    for (uint32_t i = 0; i < c->nentries; ++i) {
        uint32_t off = c->entries[i], kl = get_u32(c->blk + off + 1);
        uint32_t at = (uint32_t)block_key_hash(c->blk + off + 9, kl) & (slots - 1);
        while (table[at])
            at = (at + 1) & (slots - 1);
        table[at] = off + 1;
    }
    c->hash = table;
    c->hash_slots = slots;
    return bytes;
}

static int search_block(const struct seg_cslot *c, const uint8_t *key,
                        uint32_t klen, struct kv_buf *val, uint8_t *op)
{
    if (!c->hash)
        return search_block_binary(c, key, klen, val, op);
    uint32_t at = (uint32_t)block_key_hash(key, klen) & (c->hash_slots - 1);
    while (c->hash[at]) {
        uint32_t off = c->hash[at] - 1;
        if (get_u32(c->blk + off + 1) == klen &&
            !memcmp(c->blk + off + 9, key, klen)) {
            *op = c->blk[off];
            return kv_buf_set(val, c->blk + off + 9 + klen,
                              get_u32(c->blk + off + 5));
        }
        at = (at + 1) & (c->hash_slots - 1);
    }
    return EFS_ERR_NOT_FOUND;
}

static uint64_t seg_bloom_hash(const uint8_t *k, uint32_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ n;
    uint32_t i = 0;

    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, k + i, 8);
        h = (h ^ v) * 0xff51afd7ed558ccdULL;
        h ^= h >> 32;
    }
    for (; i < n; i++)
        h = (h ^ k[i]) * 0x100000001b3ULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static int seg_bloom_may(const struct seg_bloom *b, uint64_t h)
{
    uint64_t h1 = h, h2 = (h >> 32) | 1;

    for (uint32_t i = 0; i < SEG_BLOOM_K; i++) {
        uint64_t bit = (h1 + i * h2) & b->mask;
        if (!(b->w[bit >> 6] & (1ULL << (bit & 63))))
            return 0;
    }
    return 1;
}

/* Reads every block of a pinned segment with its own preads (the block
 * index and fd are immutable while pinned; the shared block cache is not
 * touched). Any failure leaves the segment unfiltered. */
static void seg_bloom_build(struct kv_seg *s)
{
    uint64_t *hs = NULL, n = 0, cap = 0;
    uint8_t *blk = NULL;
    uint32_t blk_cap = 0;

    for (uint32_t bi = 0; bi < s->nblocks; bi++) {
        const struct blk_ref *r = &s->idx[bi];
        uint32_t *entries = NULL, count = 0;

        if (r->len > blk_cap) {
            uint8_t *p = realloc(blk, r->len);
            if (!p)
                goto out;
            blk = p;
            blk_cap = r->len;
        }
        if (pread_all(s->fd, blk, r->len, r->off) != EFS_OK ||
            index_block(blk, r->len, &entries, &count) != EFS_OK)
            goto out;
        if (n + count > cap) {
            uint64_t nc = cap ? cap * 2 : 4096;
            while (nc < n + count)
                nc *= 2;
            uint64_t *p = realloc(hs, nc * sizeof(*hs));
            if (!p) {
                free(entries);
                goto out;
            }
            hs = p;
            cap = nc;
        }
        for (uint32_t i = 0; i < count; i++)
            hs[n++] = seg_bloom_hash(blk + entries[i] + 9,
                                     get_u32(blk + entries[i] + 1));
        free(entries);
    }
    {
        uint64_t bits = 1024;
        while (bits < n * 10)
            bits <<= 1;
        struct seg_bloom *b = calloc(1, sizeof(*b) + bits / 8);
        if (!b)
            goto out;
        b->mask = bits - 1;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t h1 = hs[i], h2 = (hs[i] >> 32) | 1;
            for (uint32_t k = 0; k < SEG_BLOOM_K; k++) {
                uint64_t bit = (h1 + k * h2) & b->mask;
                b->w[bit >> 6] |= 1ULL << (bit & 63);
            }
        }
        struct seg_bloom *none = NULL;
        if (!__atomic_compare_exchange_n(&s->bloom, &none, b, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            free(b);
    }
out:
    free(hs);
    free(blk);
}

/* One process-wide builder, newest request first (a fresh flush or
 * compaction output is the segment lookups probe first). */
static pthread_mutex_t g_bloom_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_bloom_cv = PTHREAD_COND_INITIALIZER;
static struct kv_seg **g_bloom_q;
static uint32_t g_bloom_n, g_bloom_cap;
static int g_bloom_on, g_bloom_started;

static void *seg_bloom_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_bloom_mu);
        while (!g_bloom_n)
            pthread_cond_wait(&g_bloom_cv, &g_bloom_mu);
        struct kv_seg *s = g_bloom_q[--g_bloom_n];
        pthread_mutex_unlock(&g_bloom_mu);
        if (!__atomic_load_n(&s->bloom, __ATOMIC_ACQUIRE))
            seg_bloom_build(s);
        kv_seg_unpin(s);
    }
    return NULL;
}

void kv_seg_bloom_enable(int on)
{
    pthread_mutex_lock(&g_bloom_mu);
    g_bloom_on = on;
    pthread_mutex_unlock(&g_bloom_mu);
}

void kv_seg_bloom_request(struct kv_seg *s)
{
    if (!s || __atomic_load_n(&s->bloom, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&g_bloom_mu);
    if (!g_bloom_on)
        goto out;
    if (!g_bloom_started) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, seg_bloom_main, NULL) != 0) {
            pthread_attr_destroy(&a);
            goto out;
        }
        pthread_attr_destroy(&a);
        g_bloom_started = 1;
    }
    if (g_bloom_n == g_bloom_cap) {
        uint32_t nc = g_bloom_cap ? g_bloom_cap * 2 : 64;
        struct kv_seg **q = realloc(g_bloom_q, nc * sizeof(*q));
        if (!q)
            goto out;
        g_bloom_q = q;
        g_bloom_cap = nc;
    }
    kv_seg_pin(s);
    g_bloom_q[g_bloom_n++] = s;
    pthread_cond_signal(&g_bloom_cv);
out:
    pthread_mutex_unlock(&g_bloom_mu);
}

/* Test hook: build synchronously. */
void kv_seg_bloom_build_now(struct kv_seg *s)
{
    if (s && !__atomic_load_n(&s->bloom, __ATOMIC_ACQUIRE))
        seg_bloom_build(s);
}

int kv_seg_has_bloom(const struct kv_seg *s)
{
    return s && __atomic_load_n(&s->bloom, __ATOMIC_ACQUIRE) != NULL;
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
    /* Key precedes this segment. One compare, not a binary search of a
     * file that cannot hold it. */
    if (s->nblocks > 0 &&
        kv_key_cmp(key, klen, s->idx[0].key, s->idx[0].klen) < 0)
        return EFS_ERR_NOT_FOUND;
    {
        const struct seg_bloom *b = __atomic_load_n(&s->bloom, __ATOMIC_ACQUIRE);

        if (b && !seg_bloom_may(b, seg_bloom_hash(key, klen)))
            return EFS_ERR_NOT_FOUND;
    }
    bi = block_for(s, key, klen);
    if (bi < 0)
        return EFS_ERR_NOT_FOUND;
    r = &s->idx[bi];
    c = &s->cslot[(uint32_t)bi % KV_SEG_CACHE_SLOTS];
    pthread_mutex_t *mu = seg_cmu(s, (uint32_t)bi);
    pthread_mutex_lock(mu);
    if (c->valid && c->bi == (uint32_t)bi && c->len == r->len &&
        c->off == r->off && c->blk) {
        int rc = search_block(c, key, klen, val, op);
        pthread_mutex_unlock(mu);
        return rc;
    }
    pthread_mutex_unlock(mu);
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

/* Validate a freshly read block against the segment index (records sorted,
 * first key = index key, last key < next block's first key). Frees blk and
 * returns an error on corruption. */
static int seg_block_index(struct kv_seg *s, uint32_t bi, uint8_t *blk, uint32_t len,
                           uint32_t **entries, uint32_t *count)
{
    int rc = index_block(blk, len, entries, count);
    if (rc == EFS_OK && kv_key_cmp(blk + 9, get_u32(blk + 1),
                                   s->idx[bi].key, s->idx[bi].klen))
        rc = EFS_ERR_PROTO;
    if (rc == EFS_OK && bi + 1 < s->nblocks) {
        uint32_t tail = (*entries)[*count - 1];
        if (kv_key_cmp(blk + tail + 9, get_u32(blk + tail + 1),
                        s->idx[bi + 1].key, s->idx[bi + 1].klen) >= 0)
            rc = EFS_ERR_PROTO;
    }
    if (rc != EFS_OK) { free(*entries); *entries = NULL; free(blk); }
    return rc;
}

/* Caller holds seg_cmu(s, bi). Takes ownership of blk and entries. */
static struct seg_cslot *seg_slot_install_locked(struct kv_seg *s, uint32_t bi,
                                                 uint8_t *blk, uint32_t *entries,
                                                 uint32_t count)
{
    const struct blk_ref *r = &s->idx[bi];
    struct seg_cslot *c = &s->cslot[bi % KV_SEG_CACHE_SLOTS];
    __atomic_sub_fetch(&s->hash_bytes, c->hash_slots * (uint32_t)sizeof(uint32_t),
                       __ATOMIC_RELAXED);
    free(c->blk); free(c->entries); free(c->hash);
    c->hash = NULL; c->hash_slots = 0;
    c->entries = entries; c->nentries = count;
    c->blk = blk;
    c->bi = bi;
    c->len = r->len;
    c->off = r->off;
    c->valid = 1;
    {
        uint32_t used = __atomic_load_n(&s->hash_bytes, __ATOMIC_RELAXED);
        uint32_t avail = used < KV_SEG_HASH_BYTES_MAX ? KV_SEG_HASH_BYTES_MAX - used : 0;
        __atomic_add_fetch(&s->hash_bytes, block_hash_build(c, avail), __ATOMIC_RELAXED);
    }
    if (bi + 1 == s->nblocks) {
        uint32_t tail = entries[count - 1];
        seg_note_last(s, blk + tail + 9, get_u32(blk + tail + 1));
    }
    return c;
}

int kv_seg_install(struct kv_seg *s, const struct kv_seg_io *io, uint8_t *blk,
                   const uint8_t *key, uint32_t klen, struct kv_buf *val,
                   uint8_t *op)
{
    if (!s || !io || !blk || !key || !val || !op)
        return EFS_ERR_INVAL;
    if (io->bi >= s->nblocks || s->idx[io->bi].off != io->off ||
        s->idx[io->bi].len != io->len) {
        free(blk);
        return EFS_ERR_AGAIN;
    }
    uint32_t *entries = NULL, count = 0;
    int rc = seg_block_index(s, io->bi, blk, io->len, &entries, &count);
    if (rc != EFS_OK) return rc;
    pthread_mutex_t *mu = seg_cmu(s, io->bi);
    pthread_mutex_lock(mu);
    struct seg_cslot *c = seg_slot_install_locked(s, io->bi, blk, entries, count);
    rc = search_block(c, key, klen, val, op);
    pthread_mutex_unlock(mu);
    return rc;
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
void kv_seg_range(const struct kv_seg *s, int *rlo, int *rhi)
{
    *rlo = s ? s->rlo : 0;
    *rhi = s ? s->rhi : KV_RANGE_N - 1;
}

int kv_seg_range_excludes(const struct kv_seg *s, int rlo, int rhi)
{
    if (!s)
        return 1;
    return rhi < s->rlo || rlo > s->rhi;
}

int kv_seg_excludes(struct kv_seg *s, const uint8_t *seek, uint32_t slen,
                    const uint8_t *prefix, uint32_t plen)
{
    const uint8_t *fk = NULL, *lk = NULL;
    uint32_t fl = 0, ll = 0;

    if (!s || !s->nblocks)
        return 1;
    if (plen && prefix) {
        int prlo, prhi;

        kv_prefix_ranges(prefix, plen, &prlo, &prhi);
        if (kv_seg_range_excludes(s, prlo, prhi))
            return 1;
    }
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

int kv_seg_snapshot_excludes(struct kv_seg *s,const uint8_t *seek,uint32_t slen,
                            const uint8_t *prefix,uint32_t plen,int *excluded)
{
    *excluded=0;
    if(!s || !s->nblocks){*excluded=1;return EFS_OK;}
    if(plen && prefix){
        int lo,hi;kv_prefix_ranges(prefix,plen,&lo,&hi);
        if(kv_seg_range_excludes(s,lo,hi)){*excluded=1;return EFS_OK;}
        const uint8_t *first;uint32_t length;
        if(!kv_seg_first_key(s,&first,&length) && !kv_has_prefix(first,length,prefix,plen) &&
           kv_key_cmp(first,length,prefix,plen)>0){*excluded=1;return EFS_OK;}
    }
    if(!slen || !seek)return EFS_OK;
    uint8_t *known=__atomic_load_n(&s->last_key,__ATOMIC_ACQUIRE);
    if(known){*excluded=kv_key_cmp(known,__atomic_load_n(&s->last_klen,__ATOMIC_RELAXED),seek,slen)<0;return EFS_OK;}
    struct blk_ref *r=&s->idx[s->nblocks-1];
    /* The last block's first key is a safe lower bound for its last key. */
    if(kv_key_cmp(r->key,r->klen,seek,slen)>=0)return EFS_OK;
    if(r->len>KV_LSM_SNAPSHOT_BLOCK_MAX)return EFS_ERR_BUSY;
    uint8_t block[KV_LSM_SNAPSHOT_BLOCK_MAX];
    int rc=pread_all(s->fd,block,r->len,r->off);if(rc)return rc;
    uint32_t offset=0,length=0;const uint8_t *last=NULL;
    while(offset<r->len){
        if(r->len-offset<9)return EFS_ERR_PROTO;
        uint32_t kl=get_u32(block+offset+1),vl=get_u32(block+offset+5);
        if(!kl || kl>r->len-offset-9 || vl>r->len-offset-9-kl)return EFS_ERR_PROTO;
        last=block+offset+9;length=kl;offset+=9+kl+vl;
    }
    if(!last)return EFS_ERR_PROTO;
    *excluded=kv_key_cmp(last,length,seek,slen)<0;return EFS_OK;
}

/* --- iterator -------------------------------------------------------- */

struct kv_seg_iter {
    struct kv_seg *s;
    uint32_t bi;
    uint32_t off;
    uint8_t *blk;
    uint32_t blk_len;
    int loaded;
    /* Sequential readahead for compaction and snapshot export, enabled
     * by kv_seg_iter_set_seq. A prefix scan under the LSM lock reads
     * one block: with the 1 MiB window on every iterator, one stat of a
     * 64-lane file pulled 28 MiB through pread (Sep 30, fcstor004
     * strace: 1048576-byte preads, 150 µs each, ~4 ms per stat). */
    int seq;
    int private_read;
    uint8_t *ra;
    uint64_t ra_off;
    uint32_t ra_len;
};

void kv_seg_iter_set_seq(struct kv_seg_iter *it, int seq)
{
    if (it)
        it->seq = seq;
}

void kv_seg_iter_set_private(struct kv_seg_iter *it, int private_read)
{
    if (it) it->private_read=private_read;
}

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
    free(it->ra);
    free(it);
}

/* Caller holds seg_cmu(it->s, it->bi). */
static struct seg_cslot *iter_cached(struct kv_seg_iter *it,
                                     const struct blk_ref *r)
{
    struct seg_cslot *c = &it->s->cslot[it->bi % KV_SEG_CACHE_SLOTS];

    if (c->valid && c->bi == it->bi && c->len == r->len &&
        c->off == r->off && c->blk && c->entries)
        return c;
    return NULL;
}

/* Copy a cached block (and, for a seek, find the first record >= seek in
 * its index) under the segment's cache lock. 1 = loaded from the cache. */
static int iter_load_cached(struct kv_seg_iter *it, const struct blk_ref *r,
                            const uint8_t *seek, uint32_t slen)
{
    pthread_mutex_t *mu = seg_cmu(it->s, it->bi);
    pthread_mutex_lock(mu);
    struct seg_cslot *c = iter_cached(it, r);
    if (!c) {
        pthread_mutex_unlock(mu);
        return 0;
    }
    uint8_t *p = realloc(it->blk, r->len);
    if (!p) {
        pthread_mutex_unlock(mu);
        return 0;
    }
    it->blk = p;
    memcpy(it->blk, c->blk, r->len);
    it->blk_len = r->len;
    it->off = 0;
    if (seek && slen) {
        /* Start at the first record >= seek instead of stepping (and
         * comparing) every record before it; the merge skips them anyway. */
        uint32_t lo = 0, hi = c->nentries;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t eo = c->entries[mid];
            if (kv_key_cmp(c->blk + eo + 9, get_u32(c->blk + eo + 1),
                           seek, slen) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        it->off = lo < c->nentries ? c->entries[lo] : it->blk_len;
    }
    it->loaded = 1;
    pthread_mutex_unlock(mu);
    return 1;
}

static int iter_load(struct kv_seg_iter *it)
{
    struct blk_ref *r;
    uint8_t *p;

    if (it->bi >= it->s->nblocks)
        return EFS_ERR_NOT_FOUND;
    r = &it->s->idx[it->bi];
    if(it->private_read && r->len>KV_LSM_SNAPSHOT_BLOCK_MAX)return EFS_ERR_BUSY;
    /* A block a point get just read is in the segment's slots. */
    if (!it->private_read && iter_load_cached(it, r, NULL, 0))
        return EFS_OK;
    p = realloc(it->blk, r->len);
    if (!p)
        return EFS_ERR_NOMEM;
    it->blk = p;
    if (it->ra && r->off >= it->ra_off &&
        r->off + r->len <= it->ra_off + it->ra_len) {
        memcpy(it->blk, it->ra + (uint32_t)(r->off - it->ra_off), r->len);
    } else if (!it->seq || it->private_read || r->len > (1u << 20)) {
        /* One block. Entries never split, so one value up to
         * KV_LSM_VLEN_MAX is one block; a scan reads exactly what it
         * needs. The 1 MiB window is for the sequential readers only. */
        if (pread_all(it->s->fd, it->blk, r->len, r->off) != EFS_OK)
            return EFS_ERR_IO;
    } else {
        uint8_t *ra = it->ra ? it->ra : malloc(1u << 20);
        ssize_t k;

        if (!ra)
            return EFS_ERR_NOMEM;
        it->ra = ra;
        k = pread(it->s->fd, ra, 1u << 20, (off_t)r->off);
        if (k < (ssize_t)r->len)
            return EFS_ERR_IO;
        it->ra_off = r->off;
        it->ra_len = (uint32_t)k;
        memcpy(it->blk, ra, r->len);
    }
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
    if (!it->private_read && it->bi < it->s->nblocks &&
        !iter_load_cached(it, &it->s->idx[it->bi], seek, plen) && !it->seq) {
        /* A prefix scan (getattr's reduce-record probe, usually empty)
         * re-read its seek block from disk on every call: the block cache
         * was filled only by point gets (Oct 16 Intel: ~8% of the metadata
         * leader's CPU in scan preads). Cache the seek block like a point
         * get would; later blocks of a long scan stay uncached. */
        const struct blk_ref *r = &it->s->idx[it->bi];
        uint8_t *blk = malloc(r->len);
        uint32_t *entries = NULL, count = 0;
        if (blk && pread_all(it->s->fd, blk, r->len, r->off) != EFS_OK) {
            free(blk); /* the ordinary load reports the I/O error */
            blk = NULL;
        }
        /* seg_block_index frees blk itself on a malformed block; the scan
         * then reads it uncached exactly as before. */
        if (blk && seg_block_index(it->s, it->bi, blk, r->len, &entries, &count) == EFS_OK) {
            pthread_mutex_lock(seg_cmu(it->s, it->bi));
            (void)seg_slot_install_locked(it->s, it->bi, blk, entries, count);
            pthread_mutex_unlock(seg_cmu(it->s, it->bi));
            (void)iter_load_cached(it, r, seek, plen);
        }
    }
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
