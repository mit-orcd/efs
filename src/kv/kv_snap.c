#include "efs/kv_snap.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <stdlib.h>
#include <string.h>

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

    (void)val;
    (void)vlen;
    if (c->rc != EFS_OK)
        return c->rc;
    if (!key_in_group(key, klen, c->group))
        return EFS_OK;
    if (c->n == c->cap && col_grow_k(c) != EFS_OK) {
        c->rc = EFS_ERR_NOMEM;
        return EFS_ERR_NOMEM;
    }
    if (col_grow_a(c, klen) != EFS_OK) {
        c->rc = EFS_ERR_NOMEM;
        return EFS_ERR_NOMEM;
    }
    memcpy(c->arena + c->alen, key, klen);
    c->k[c->n].off = c->alen;
    c->k[c->n].klen = klen;
    c->alen += klen;
    c->n++;
    return EFS_OK;
}

static int incoming_has(const struct efs_kv_item *items, uint32_t n,
                        const uint8_t *key, uint32_t klen)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (items[i].klen == klen && memcmp(items[i].key, key, klen) == 0)
            return 1;
    }
    return 0;
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

    total = n;
    for (i = 0; i < c.n; i++) {
        if (!incoming_has(items, n, c.arena + c.k[i].off, c.k[i].klen))
            total++;
    }
    if (total > n) {
        struct efs_kv_item *p = realloc(items, (size_t)total * sizeof(*p));
        if (!p) {
            free(c.k);
            free(c.arena);
            free(items);
            return EFS_ERR_NOMEM;
        }
        items = p;
    }
    ni = n;
    for (i = 0; i < c.n; i++) {
        if (incoming_has(items, n, c.arena + c.k[i].off, c.k[i].klen))
            continue;
        items[ni].op = EFS_KV_DEL;
        items[ni].key = c.arena + c.k[i].off;
        items[ni].klen = c.k[i].klen;
        items[ni].val = NULL;
        items[ni].vlen = 0;
        ni++;
    }
    rc = ni ? efs_kv_batch(kv, items, ni) : EFS_OK;
    free(c.k);
    free(c.arena);
    free(items);
    return rc;
}
