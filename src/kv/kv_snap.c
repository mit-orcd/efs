#include "efs/kv_snap.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
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

static int key_in_group(const uint8_t *key, uint32_t klen, uint8_t group)
{
    uint32_t shard;

    if (klen < 2)
        return 0;
    shard = ((uint32_t)key[0] << 8) | (uint32_t)key[1];
    if (shard > EFS_KV_SHARD_MASK)
        return 0;
    return efs_raft_shard_group(shard) == group;
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
    if (!key_in_group(key, klen, a->group))
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
    put_u32(a->buf + a->len, klen);
    a->len += 4;
    put_u32(a->buf + a->len, vlen);
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
    put_u32(a.buf, a.n);
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
    if (!key_in_group(key, klen, c->group))
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
    n = get_u32(data);
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
        klen = get_u32(data + off);
        off += 4;
        vlen = get_u32(data + off);
        off += 4;
        if (klen > len - off || vlen > len - off - klen ||
            !key_in_group(data + off, klen, group)) {
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

int efs_kv_group_import_file(struct efs_kv *kv, uint8_t group, const char *path)
{
    uint8_t *buf = NULL;
    off_t sz;
    int fd, rc;

    if (!kv || !path)
        return EFS_ERR_INVAL;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return EFS_ERR_IO;
    sz = lseek(fd, 0, SEEK_END);
    if (sz < 4 || (uint64_t)sz > 0xffffffffu) {
        close(fd);
        return sz < 4 ? EFS_ERR_PROTO : EFS_ERR_NOMEM;
    }
    buf = malloc((size_t)sz);
    if (!buf) {
        close(fd);
        return EFS_ERR_NOMEM;
    }
    {
        uint8_t *p = buf;
        size_t left = (size_t)sz;
        off_t at = 0;

        while (left) {
            ssize_t n = pread(fd, p, left, at);

            if (n < 0) {
                free(buf);
                close(fd);
                return EFS_ERR_IO;
            }
            if (n == 0)
                break;
            p += n;
            at += n;
            left -= (size_t)n;
        }
        if (left) {
            free(buf);
            close(fd);
            return EFS_ERR_IO;
        }
    }
    close(fd);
    rc = efs_kv_group_import(kv, group, buf, (uint32_t)sz);
    free(buf);
    return rc;
}
