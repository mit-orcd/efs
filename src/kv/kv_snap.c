#include "kv_order.h"
#include "efs/telemetry.h"
#include "efs/kv_snap.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "kv_lsm_internal.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>



/* Inspect the entire incoming image, including unchanged rows omitted from
 * the diff. Reserved KV kinds or a chunk with the reserved coding profile
 * come from a store this build cannot read: refuse the import before
 * anything lands. W125 packed rows (a packed segment, or a chunk with the
 * packed profile) need the PACK_V2 reader barrier to be durable before
 * they become visible; a failed import may conservatively keep it. */
static int import_barrier_item(const uint8_t *key, uint32_t klen,
                               const uint8_t *val, uint32_t vlen, int *packed)
{
    uint8_t kind;

    if (klen < 3)
        return EFS_OK;
    kind = key[2];
    if ((kind >= EFS_KV_KIND_RESERVED_MIN && kind <= EFS_KV_KIND_RESERVED_MAX) ||
        (kind == EFS_KV_KIND_CHUNK && vlen >= 12 &&
         !val[8] && !val[9] && !val[10] && val[11] == 2)) {
        efs_telemetry_log(0, "kv-snap: import holds reserved rows; refusing\n");
        return EFS_ERR_PROTO;
    }
    if (kind == EFS_KV_KIND_PACK_SEGMENT ||
        (kind == EFS_KV_KIND_CHUNK && vlen >= 12 && !val[8] &&
         !val[9] && !val[10] && val[11] == EFS_META_PROFILE_PACK))
        *packed = 1;
    return EFS_OK;
}

static int import_reader_barrier(struct efs_kv *kv,
    const struct efs_kv_item *items, uint32_t count)
{
    int packed = 0;
    for (uint32_t i = 0; i < count; i++) {
        int rc = import_barrier_item(items[i].key, items[i].klen,
                                     items[i].val, items[i].vlen, &packed);
        if (rc != EFS_OK)
            return rc;
    }
    return packed ? efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V2) : EFS_OK;
}

struct exp_acc {
    uint8_t group;
    uint32_t max_bytes;
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    uint32_t n;
    int rc;
};

static int exp_grow(struct exp_acc *a, uint32_t need)
{
    uint8_t *p;
    uint32_t cap;

    if (need <= a->cap)
        return EFS_OK;
    cap = a->cap ? a->cap : 4096;
    while (cap < need)
        cap *= 2;
    p = realloc(a->buf, cap);
    if (!p)
        return EFS_ERR_NOMEM;
    a->buf = p;
    a->cap = cap;
    return EFS_OK;
}

static int exp_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    struct exp_acc *a = user;
    uint32_t add, need;

    if (a->rc != EFS_OK)
        return a->rc;
    if (!kv_key_in_group(key, klen, a->group))
        return EFS_OK;
    add = 9u + klen + vlen;
    need = a->len + add;
    if (need > a->max_bytes) {
        a->rc = EFS_ERR_BUSY;
        return EFS_ERR_BUSY;
    }
    if (exp_grow(a, need) != EFS_OK) {
        a->rc = EFS_ERR_NOMEM;
        return EFS_ERR_NOMEM;
    }
    a->buf[a->len++] = 1; /* KV_OP_PUT */
    efs_store_le32(a->buf + a->len, klen);
    a->len += 4;
    efs_store_le32(a->buf + a->len, vlen);
    a->len += 4;
    memcpy(a->buf + a->len, key, klen);
    a->len += klen;
    if (vlen) {
        memcpy(a->buf + a->len, val, vlen);
        a->len += vlen;
    }
    a->n++;
    return EFS_OK;
}

int efs_kv_group_export(struct efs_kv *kv, uint8_t group, uint32_t max_bytes,
                        uint8_t **data, uint32_t *len)
{
    struct exp_acc a;
    int rc;

    if (!kv || !data || !len || max_bytes < 4)
        return EFS_ERR_INVAL;
    memset(&a, 0, sizeof(a));
    a.group = group;
    a.max_bytes = max_bytes;
    a.len = 4;
    a.rc = EFS_OK;
    rc = exp_grow(&a, 4);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan(kv, exp_cb, &a);
    if (a.rc != EFS_OK) {
        free(a.buf);
        return a.rc;
    }
    if (rc != EFS_OK && rc != EFS_ERR_BUSY) {
        free(a.buf);
        return rc;
    }
    efs_store_le32(a.buf, a.n);
    *data = a.buf;
    *len = a.len;
    return EFS_OK;
}

/* Offsets, not pointers: col_grow_a reallocs the arena. */
struct key_ref {
    uint32_t off;
    uint32_t klen;
    uint32_t voff;
    uint32_t vlen;
};

struct collect {
    uint8_t group;
    struct key_ref *k;
    uint32_t n;
    uint32_t cap;
    uint8_t *arena;
    uint32_t alen;
    uint32_t acap;
    int rc;
};

static int cmp_bytes(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl)
{
    uint32_t n = al < bl ? al : bl;
    int c = memcmp(a, b, n);

    if (c)
        return c;
    if (al < bl)
        return -1;
    if (al > bl)
        return 1;
    return 0;
}

static int col_grow_k(struct collect *c)
{
    uint32_t cap = c->cap ? c->cap * 2 : 16;
    struct key_ref *p = realloc(c->k, (size_t)cap * sizeof(*p));

    if (!p)
        return EFS_ERR_NOMEM;
    c->k = p;
    c->cap = cap;
    return EFS_OK;
}

static int col_grow_a(struct collect *c, uint32_t add)
{
    uint32_t need = c->alen + add;
    uint8_t *p;
    uint32_t cap;

    if (need <= c->acap)
        return EFS_OK;
    cap = c->acap ? c->acap : 4096;
    while (cap < need)
        cap *= 2;
    p = realloc(c->arena, cap);
    if (!p)
        return EFS_ERR_NOMEM;
    c->arena = p;
    c->acap = cap;
    return EFS_OK;
}

static int col_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    struct collect *c = user;

    if (c->rc != EFS_OK)
        return c->rc;
    if (!kv_key_in_group(key, klen, c->group))
        return EFS_OK;
    if (klen > UINT32_MAX - c->alen || vlen > UINT32_MAX - c->alen - klen) {
        c->rc = EFS_ERR_NOMEM;
        return EFS_ERR_NOMEM;
    }
    if (c->n == c->cap && col_grow_k(c) != EFS_OK) {
        c->rc = EFS_ERR_NOMEM;
        return EFS_ERR_NOMEM;
    }
    if (col_grow_a(c, klen + vlen) != EFS_OK) {
        c->rc = EFS_ERR_NOMEM;
        return EFS_ERR_NOMEM;
    }
    memcpy(c->arena + c->alen, key, klen);
    c->k[c->n].off = c->alen;
    c->k[c->n].klen = klen;
    c->alen += klen;
    c->k[c->n].voff = c->alen;
    c->k[c->n].vlen = vlen;
    if (vlen) {
        memcpy(c->arena + c->alen, val, vlen);
        c->alen += vlen;
    }
    c->n++;
    return EFS_OK;
}

static int item_cmp(const void *a, const void *b, void *arg)
{
    const struct efs_kv_item *ia = a, *ib = b;

    (void)arg;
    return cmp_bytes(ia->key, ia->klen, ib->key, ib->klen);
}

static int ref_cmp(const void *a, const void *b, void *arg)
{
    const struct key_ref *ra = a, *rb = b;
    const uint8_t *arena = arg;

    return cmp_bytes(arena + ra->off, ra->klen, arena + rb->off, rb->klen);
}

int efs_kv_group_import(struct efs_kv *kv, uint8_t group, const uint8_t *data,
                        uint32_t len)
{
    struct collect c;
    struct efs_kv_item *items = NULL;
    uint32_t n = 0, i, off, ni, total;
    int rc;

    if (!kv || (len && !data))
        return EFS_ERR_INVAL;
    if (len < 4)
        return EFS_ERR_PROTO;
    n = efs_load_le32(data);
    if (n > (len - 4) / 9)
        return EFS_ERR_PROTO;
    off = 4;
    items = calloc(n, sizeof(*items));
    if (n && !items)
        return EFS_ERR_NOMEM;
    for (i = 0; i < n; i++) {
        uint32_t klen, vlen;

        if (off + 9 > len) {
            free(items);
            return EFS_ERR_PROTO;
        }
        if (data[off] != 1) {
            free(items);
            return EFS_ERR_PROTO;
        }
        off++;
        klen = efs_load_le32(data + off);
        off += 4;
        vlen = efs_load_le32(data + off);
        off += 4;
        if (klen > len - off || vlen > len - off - klen ||
            !kv_key_in_group(data + off, klen, group)) {
            free(items);
            return EFS_ERR_PROTO;
        }
        items[i].op = EFS_KV_PUT;
        items[i].key = data + off;
        items[i].klen = klen;
        off += klen;
        items[i].val = vlen ? data + off : NULL;
        items[i].vlen = vlen;
        off += vlen;
    }
    if (off != len) {
        free(items);
        return EFS_ERR_PROTO;
    }

    rc = import_reader_barrier(kv, items, n);
    if (rc != EFS_OK) { free(items); return rc; }

    memset(&c, 0, sizeof(c));
    c.group = group;
    rc = efs_kv_scan(kv, col_cb, &c);
    if (c.rc != EFS_OK)
        rc = c.rc;
    if (rc != EFS_OK) {
        free(c.k);
        free(c.arena);
        free(items);
        return rc;
    }

    /* A full-image install used to memcmp every local key against every
     * incoming key, then PUT the whole image. On a live group that is
     * hours under the Raft lock for a one-index catch-up. Sort both
     * sides and write only the diff. */
    if (n > 1)
        qsort_r(items, n, sizeof(*items), item_cmp, NULL);
    if (c.n > 1)
        qsort_r(c.k, c.n, sizeof(*c.k), ref_cmp, c.arena);
    total = n + c.n;
    {
        struct efs_kv_item *diff = NULL;

        if (total) {
            diff = calloc(total, sizeof(*diff));
            if (!diff) {
                free(c.k);
                free(c.arena);
                free(items);
                return EFS_ERR_NOMEM;
            }
        }
        ni = 0;
        i = 0;
        off = 0; /* local cursor */
        while (i < n || off < c.n) {
            int cmpv;

            if (i + 1 < n &&
                cmp_bytes(items[i].key, items[i].klen, items[i + 1].key,
                          items[i + 1].klen) == 0) {
                i++;
                continue;
            }
            if (off + 1 < c.n &&
                cmp_bytes(c.arena + c.k[off].off, c.k[off].klen,
                          c.arena + c.k[off + 1].off, c.k[off + 1].klen) == 0) {
                off++;
                continue;
            }
            if (i < n && off < c.n)
                cmpv = cmp_bytes(items[i].key, items[i].klen,
                                 c.arena + c.k[off].off, c.k[off].klen);
            else
                cmpv = i < n ? -1 : 1;
            if (cmpv < 0) {
                diff[ni++] = items[i];
                i++;
            } else if (cmpv > 0) {
                diff[ni].op = EFS_KV_DEL;
                diff[ni].key = c.arena + c.k[off].off;
                diff[ni].klen = c.k[off].klen;
                diff[ni].val = NULL;
                diff[ni].vlen = 0;
                ni++;
                off++;
            } else {
                if (items[i].vlen != c.k[off].vlen ||
                    (items[i].vlen &&
                     memcmp(items[i].val, c.arena + c.k[off].voff,
                            items[i].vlen) != 0))
                    diff[ni++] = items[i];
                i++;
                off++;
            }
        }
        rc = ni ? efs_kv_batch(kv, diff, ni) : EFS_OK;
        free(diff);
    }
    free(c.k);
    free(c.arena);
    free(items);
    return rc;
}

/* K108: streaming snapshot import. The old prepare read both images whole
 * (heap = incoming + local + three item arrays) and held them until the last
 * slice was applied, so a rejoining member's heap scaled with the image, not
 * with its KV budget. Both images are written sorted and unique by
 * efs_kv_lsm_view_export, so the diff is a merge of two forward readers.
 * Each reader keeps one window (IMP_WIN, grown only to fit one record, at most
 * IMP_REC_MAX); a batch is copied out of the windows and bounded by items,
 * bytes and records scanned. Resident bytes: two windows + one batch, never a
 * function of the image or the diff size. */
#define IMP_WIN          (256u * 1024u)
#define IMP_BATCH_ITEMS  1024u          /* keys per efs_kv_batch */
#define IMP_BATCH_BYTES  (1u << 20)     /* copied key+value bytes per batch */
#define IMP_SCAN_RECORDS 16384u         /* records merged per step, equal or not */
#define IMP_REC_MAX      (9u + KV_LSM_KLEN_MAX + KV_LSM_VLEN_MAX)

struct imp_rd {
    int fd;
    uint64_t size, fpos;        /* file size; offset of the next byte to read */
    uint8_t *buf;
    size_t cap, have, at;       /* buf[at..have) is read and not yet parsed */
    uint32_t left;              /* records not yet parsed */
    int live;                   /* key/val describe a record not yet merged */
    const uint8_t *key, *val;   /* point into buf; valid until the next rd_next */
    uint32_t klen, vlen;
};

struct efs_kv_import {
    struct efs_kv *kv;
    uint8_t group;
    struct imp_rd in, loc;
    uint32_t n_in, n_loc;
    struct efs_kv_item *items;  /* IMP_BATCH_ITEMS */
    uint8_t *arena;             /* the batch's keys and values */
    size_t alen, acap;
    uint32_t n;                 /* built, not yet applied: retried until it lands */
    int finished;               /* both images fully merged */
    int err;                    /* sticky: a half-built batch cannot be resumed */
    uint64_t peak, n_diff;
};

static int rd_fill(struct imp_rd *r, size_t need)
{
    if (r->have - r->at >= need)
        return EFS_OK;
    if (r->at) {
        memmove(r->buf, r->buf + r->at, r->have - r->at);
        r->have -= r->at;
        r->at = 0;
    }
    if (!r->buf || need > r->cap) {
        size_t c = need > IMP_WIN ? need : IMP_WIN;
        uint8_t *p = realloc(r->buf, c);

        if (!p)
            return EFS_ERR_NOMEM;
        r->buf = p;
        r->cap = c;
    }
    while (r->have < need) {
        ssize_t n = pread(r->fd, r->buf + r->have, r->cap - r->have,
                          (off_t)r->fpos);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return EFS_ERR_IO;
        if (n == 0)
            return EFS_ERR_PROTO;   /* shorter than its own header says */
        r->have += (size_t)n;
        r->fpos += (uint64_t)n;
    }
    return EFS_OK;
}

static int rd_head(struct imp_rd *r)
{
    int rc;

    r->at = r->have = 0;
    r->fpos = 0;
    r->live = 0;
    rc = rd_fill(r, 4);
    if (rc != EFS_OK)
        return rc;
    r->left = efs_load_le32(r->buf + r->at);
    r->at += 4;
    if (r->left > (r->size - 4) / 9)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

static int rd_open(struct imp_rd *r, const char *path)
{
    off_t sz;

    r->fd = open(path, O_RDONLY);
    if (r->fd < 0)
        return EFS_ERR_IO;
    sz = lseek(r->fd, 0, SEEK_END);
    if (sz < 4)
        return sz < 0 ? EFS_ERR_IO : EFS_ERR_PROTO;
    r->size = (uint64_t)sz;
    return rd_head(r);
}

/* The next record, or live == 0 at the end. Every check parse_image made
 * stays: record tag, lengths inside the file, key in the group, and no
 * bytes after the last record. Lengths are also held to the LSM's own key
 * and value limits so a record is never larger than IMP_REC_MAX. */
static int rd_next(struct imp_rd *r, uint8_t group)
{
    const uint8_t *p;
    uint32_t klen, vlen;
    uint64_t pos;
    int rc;

    r->live = 0;
    if (!r->left)
        return r->have == r->at && r->fpos == r->size ? EFS_OK : EFS_ERR_PROTO;
    rc = rd_fill(r, 9);
    if (rc != EFS_OK)
        return rc;
    p = r->buf + r->at;
    klen = efs_load_le32(p + 1);
    vlen = efs_load_le32(p + 5);
    pos = r->fpos - (r->have - r->at);
    if (p[0] != 1 || klen > KV_LSM_KLEN_MAX || vlen > KV_LSM_VLEN_MAX ||
        9u + (uint64_t)klen + vlen > r->size - pos)
        return EFS_ERR_PROTO;
    rc = rd_fill(r, 9u + klen + vlen);
    if (rc != EFS_OK)
        return rc;
    p = r->buf + r->at;
    r->key = p + 9;
    r->klen = klen;
    r->val = vlen ? p + 9 + klen : NULL;
    r->vlen = vlen;
    if (!kv_key_in_group(r->key, klen, group))
        return EFS_ERR_PROTO;
    r->at += 9u + klen + vlen;
    r->left--;
    r->live = 1;
    return EFS_OK;
}

static void rd_close(struct imp_rd *r)
{
    if (r->fd >= 0)
        close(r->fd);
    free(r->buf);
    r->fd = -1;
    r->buf = NULL;
}

/* Pass 1, before any write: the whole image is framed, in the group and
 * strictly ascending (the order the merge relies on). */
static int imp_scan(struct imp_rd *r, uint8_t group, uint8_t *prev, int barrier,
                    int *packed)
{
    uint32_t plen = 0, i = 0;

    for (;;) {
        int rc = rd_next(r, group);

        if (rc != EFS_OK)
            return rc;
        if (!r->live)
            return EFS_OK;
        if (i && cmp_bytes(prev, plen, r->key, r->klen) >= 0)
            return EFS_ERR_PROTO;
        memcpy(prev, r->key, r->klen);
        plen = r->klen;
        i++;
        if (barrier) {
            rc = import_barrier_item(r->key, r->klen, r->val, r->vlen, packed);
            if (rc != EFS_OK)
                return rc;
        }
    }
}

static void imp_note(struct efs_kv_import *im)
{
    uint64_t b = (uint64_t)im->in.cap + im->loc.cap + im->acap +
                 IMP_BATCH_ITEMS * sizeof(*im->items) + KV_LSM_KLEN_MAX;

    if (b > im->peak)
        im->peak = b;
}

void efs_kv_import_close(struct efs_kv_import *im)
{
    if (!im)
        return;
    rd_close(&im->in);
    rd_close(&im->loc);
    free(im->items);
    free(im->arena);
    free(im);
}

int efs_kv_import_open(struct efs_kv *kv, uint8_t group, const char *incoming,
                       const char *local, struct efs_kv_import **out)
{
    struct efs_kv_import *im;
    uint8_t *prev;
    int rc, packed = 0;

    if (out)
        *out = NULL;
    if (!kv || !incoming || !local || !out)
        return EFS_ERR_INVAL;
    im = calloc(1, sizeof(*im));
    prev = malloc(KV_LSM_KLEN_MAX);
    if (!im || !prev) {
        free(im);
        free(prev);
        return EFS_ERR_NOMEM;
    }
    im->kv = kv;
    im->group = group;
    im->in.fd = im->loc.fd = -1;
    im->items = calloc(IMP_BATCH_ITEMS, sizeof(*im->items));
    rc = im->items ? rd_open(&im->in, incoming) : EFS_ERR_NOMEM;
    if (rc == EFS_OK)
        rc = rd_open(&im->loc, local);
    if (rc == EFS_OK)
        rc = imp_scan(&im->in, group, prev, 1, &packed);
    if (rc == EFS_OK)
        rc = imp_scan(&im->loc, group, prev, 0, NULL);
    imp_note(im);
    free(prev);
    if (rc == EFS_OK) {
        /* Reserved rows were refused above; the PACK_V2 barrier is taken
         * once the whole incoming image is known to be readable. */
        rc = rd_head(&im->in);
        if (rc == EFS_OK)
            rc = rd_head(&im->loc);
        im->n_in = im->in.left;
        im->n_loc = im->loc.left;
    }
    if (rc == EFS_OK && packed)
        rc = efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V2);
    if (rc != EFS_OK) {
        efs_kv_import_close(im);
        return rc;
    }
    *out = im;
    return EFS_OK;
}

static int imp_add(struct efs_kv_import *im, uint8_t op, const uint8_t *k,
                   uint32_t kl, const uint8_t *v, uint32_t vl)
{
    size_t need = (size_t)kl + vl;
    struct efs_kv_item *it = &im->items[im->n];

    if (im->alen + need > im->acap) {
        /* Sized for a full batch up front; only a record that crosses the
         * byte bound grows it, by exactly that record. */
        size_t c = IMP_BATCH_BYTES + 4096;
        uint8_t *p;

        if (c < im->alen + need)
            c = im->alen + need;
        p = realloc(im->arena, c);
        if (!p)
            return EFS_ERR_NOMEM;
        im->arena = p;
        im->acap = c;
    }
    memcpy(im->arena + im->alen, k, kl);
    if (vl)
        memcpy(im->arena + im->alen + kl, v, vl);
    im->alen += need;
    it->op = op;
    it->klen = kl;
    it->vlen = vl;
    it->key = it->val = NULL;   /* pointed at the arena just before the batch */
    im->n++;
    im->n_diff++;
    return EFS_OK;
}

/* Merge forward until the batch is full or the scan budget is spent. */
static int imp_build(struct efs_kv_import *im)
{
    struct imp_rd *in = &im->in, *loc = &im->loc;
    uint32_t scanned = 0;
    int rc;

    im->alen = 0;
    while (im->n < IMP_BATCH_ITEMS && im->alen < IMP_BATCH_BYTES &&
           scanned < IMP_SCAN_RECORDS) {
        int c;

        if (!in->live && (rc = rd_next(in, im->group)) != EFS_OK)
            return rc;
        if (!loc->live && (rc = rd_next(loc, im->group)) != EFS_OK)
            return rc;
        if (!in->live && !loc->live) {
            im->finished = 1;
            break;
        }
        scanned++;
        c = !loc->live ? -1 : !in->live ? 1 :
            cmp_bytes(in->key, in->klen, loc->key, loc->klen);
        if (c < 0) {
            rc = imp_add(im, EFS_KV_PUT, in->key, in->klen, in->val, in->vlen);
            in->live = 0;
        } else if (c > 0) {
            rc = imp_add(im, EFS_KV_DEL, loc->key, loc->klen, NULL, 0);
            loc->live = 0;
        } else {
            rc = EFS_OK;
            if (in->vlen != loc->vlen ||
                (in->vlen && memcmp(in->val, loc->val, in->vlen) != 0))
                rc = imp_add(im, EFS_KV_PUT, in->key, in->klen, in->val,
                             in->vlen);
            in->live = loc->live = 0;
        }
        if (rc != EFS_OK)
            return rc;
        imp_note(im);
    }
    return EFS_OK;
}

int efs_kv_import_step(struct efs_kv_import *im, int *done)
{
    int rc;

    if (done)
        *done = 0;
    if (!im || !done)
        return EFS_ERR_INVAL;
    if (im->err)
        return im->err;
    if (!im->n && !im->finished) {
        rc = imp_build(im);
        if (rc != EFS_OK) {
            im->err = rc;
            return rc;
        }
    }
    if (im->n) {
        size_t at = 0;

        for (uint32_t i = 0; i < im->n; i++) {
            im->items[i].key = im->arena + at;
            im->items[i].val = im->items[i].vlen ?
                im->arena + at + im->items[i].klen : NULL;
            at += (size_t)im->items[i].klen + im->items[i].vlen;
        }
        rc = efs_kv_batch(im->kv, im->items, im->n);
        if (rc != EFS_OK)
            return rc;      /* the same batch is applied again next step */
        im->n = 0;
    }
    *done = im->finished;
    return EFS_OK;
}

void efs_kv_import_stat(const struct efs_kv_import *im, uint32_t *n_incoming,
                        uint32_t *n_local, uint64_t *peak_bytes,
                        uint64_t *n_diff)
{
    if (n_diff)
        *n_diff = im ? im->n_diff : 0;
    if (n_incoming)
        *n_incoming = im ? im->n_in : 0;
    if (n_local)
        *n_local = im ? im->n_loc : 0;
    if (peak_bytes)
        *peak_bytes = im ? im->peak : 0;
}
