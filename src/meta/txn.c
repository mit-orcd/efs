#include "efs/txn.h"
#include "efs/kv_key.h"
#include "efs/dir_spread.h"
#include "efs/meta_apply.h"
#include <string.h>

#define VAL_MAX EFS_KV_KEY_MAX
#define KEY_MAX EFS_KV_KEY_MAX

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void be64(uint8_t *p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

uint32_t efs_txn_hash(const struct efs_txid *t)
{
    uint32_t h = 2166136261u;
    int i;

    if (!t)
        return 0;
    for (i = 0; i < EFS_TXN_ID_LEN; i++) {
        h ^= t->bytes[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t efs_txn_coordinator(const struct efs_txid *t,
                             const struct efs_txn_parts *p)
{
    if (!t || !p || p->n == 0)
        return 0;
    return p->shard[efs_txn_hash(t) % p->n];
}

static int parts_ok(const struct efs_txn_parts *p)
{
    return p && p->n > 0 && p->n <= EFS_TXN_MAX_PART;
}

static int pack_parts(uint8_t *out, const struct efs_txn_parts *p)
{
    uint8_t i;

    out[0] = p->n;
    for (i = 0; i < p->n; i++)
        be32(out + 1 + (uint32_t)i * 4u, p->shard[i]);
    return 1 + (int)p->n * 4;
}

static int unpack_parts(const uint8_t *in, uint32_t n, struct efs_txn_parts *p,
                        uint32_t *used)
{
    uint8_t i, np;
    uint32_t need;

    if (!in || n < 1)
        return EFS_ERR_PROTO;
    np = in[0];
    if (np == 0 || np > EFS_TXN_MAX_PART)
        return EFS_ERR_PROTO;
    need = 1u + (uint32_t)np * 4u;
    if (n < need)
        return EFS_ERR_PROTO;
    if (p) {
        memset(p, 0, sizeof(*p));
        p->n = np;
        for (i = 0; i < np; i++)
            p->shard[i] = rd32(in + 1 + (uint32_t)i * 4u);
    }
    if (used)
        *used = need;
    return EFS_OK;
}

static int same_txid(const uint8_t *a, const struct efs_txid *t)
{
    return memcmp(a, t->bytes, EFS_TXN_ID_LEN) == 0;
}

static uint32_t key_shard(const uint8_t *key)
{
    return ((uint32_t)key[0] << 8) | key[1];
}

int efs_txn_ver_get(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                    uint64_t *ver)
{
    uint8_t vk[KEY_MAX], buf[8];
    uint32_t vl = 0, n = 8;
    int rc;

    if (!kv || !key || !ver)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_ver(key, klen, vk, &vl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, vk, vl, buf, &n);
    if (rc == EFS_ERR_NOT_FOUND) {
        *ver = 0;
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (n < 8)
        return EFS_ERR_PROTO;
    *ver = rd64(buf);
    return EFS_OK;
}

struct scan_hit {
    int other;
    struct efs_txid self;
};

static int guard_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct scan_hit *h = user;

    (void)val;
    (void)vlen;
    if (klen < 16)
        return 0;
    if (memcmp(key + klen - 16, h->self.bytes, 16) != 0)
        h->other = 1;
    return 0;
}

static int guards_conflict(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                           const struct efs_txid *t)
{
    uint8_t pref[KEY_MAX];
    uint32_t pl = 0;
    struct scan_hit h;
    int rc;

    rc = efs_kv_key_guard_prefix(key, klen, pref, &pl);
    if (rc != EFS_OK)
        return rc;
    memset(&h, 0, sizeof(h));
    h.self = *t;
    rc = efs_kv_scan_prefix(kv, pref, pl, guard_cb, &h);
    if (rc != EFS_OK)
        return rc;
    return h.other ? EFS_ERR_BUSY : EFS_OK;
}

static int pack_excl(uint8_t *out, uint32_t cap, uint32_t *n,
                     const struct efs_txid *t, const struct efs_txn_parts *p,
                     uint64_t expected, int op, const uint8_t *val, uint32_t vlen)
{
    uint32_t off;
    int pn;

    if (!parts_ok(p) || (op == EFS_TXN_PUT && vlen && !val))
        return EFS_ERR_INVAL;
    if (op == EFS_TXN_DEL)
        vlen = 0;
    pn = pack_parts(out + 16, p);
    off = 16u + (uint32_t)pn + 1u + 8u + 1u + 4u + vlen;
    if (off > cap)
        return EFS_ERR_INVAL;
    memcpy(out, t->bytes, 16);
    out[16 + (uint32_t)pn] = EFS_TXN_EXCL;
    be64(out + 16 + (uint32_t)pn + 1, expected);
    out[16 + (uint32_t)pn + 1 + 8] = (uint8_t)op;
    be32(out + 16 + (uint32_t)pn + 1 + 8 + 1, vlen);
    if (vlen)
        memcpy(out + 16 + (uint32_t)pn + 1 + 8 + 1 + 4, val, vlen);
    *n = off;
    return EFS_OK;
}

static int parse_excl(const uint8_t *in, uint32_t n, struct efs_txid *t,
                      struct efs_txn_parts *p, int *op, const uint8_t **val,
                      uint32_t *vlen)
{
    uint32_t used = 0, off, vl;
    int rc;

    if (n < 16 + 1)
        return EFS_ERR_PROTO;
    memcpy(t->bytes, in, 16);
    rc = unpack_parts(in + 16, n - 16, p, &used);
    if (rc != EFS_OK)
        return rc;
    off = 16 + used;
    if (off + 1 + 8 + 1 + 4 > n || in[off] != EFS_TXN_EXCL)
        return EFS_ERR_PROTO;
    if (op)
        *op = in[off + 1 + 8];
    vl = rd32(in + off + 1 + 8 + 1);
    if (off + 1 + 8 + 1 + 4 + vl > n)
        return EFS_ERR_PROTO;
    if (vlen)
        *vlen = vl;
    if (val)
        *val = vl ? in + off + 1 + 8 + 1 + 4 : NULL;
    return EFS_OK;
}

static int parse_parts_txid(const uint8_t *in, uint32_t n, struct efs_txid *t,
                            struct efs_txn_parts *p, uint32_t *used)
{
    if (n < 16 + 1)
        return EFS_ERR_PROTO;
    memcpy(t->bytes, in, 16);
    return unpack_parts(in + 16, n - 16, p, used);
}

int efs_txn_prepare_excl(struct efs_kv *kv, const struct efs_txid *t,
                         const struct efs_txn_parts *p, const uint8_t *key,
                         uint32_t klen, uint64_t expected_ver, int op,
                         const uint8_t *new_val, uint32_t nlen)
{
    uint8_t ik[KEY_MAX], buf[VAL_MAX], packed[VAL_MAX];
    uint32_t il = 0, bl = VAL_MAX, pn = 0;
    uint64_t ver = 0;
    struct efs_txid have;
    int rc;

    if (!kv || !t || !key || klen < 3)
        return EFS_ERR_INVAL;
    rc = efs_txn_ver_get(kv, key, klen, &ver);
    if (rc != EFS_OK)
        return rc;
    if (ver != expected_ver)
        return EFS_ERR_STALE;
    /* ver 0 is "no txn has resolved this key". A live row at ver 0 is an
     * unversioned apply write (root inode, file CREATE) — CAS(0) updates it.
     * Insert uniqueness is intent conflict, then STALE once resolve bumps ver. */
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &bl);
    if (rc == EFS_OK) {
        rc = parse_excl(buf, bl, &have, NULL, NULL, NULL, NULL);
        if (rc != EFS_OK)
            return rc;
        if (same_txid(have.bytes, t))
            return EFS_OK;
        return EFS_ERR_BUSY;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    rc = guards_conflict(kv, key, klen, t);
    if (rc != EFS_OK)
        return rc;
    rc = pack_excl(packed, sizeof(packed), &pn, t, p, expected_ver, op, new_val,
                   nlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_put(kv, ik, il, packed, pn);
}

int efs_txn_prepare_guard(struct efs_kv *kv, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t observed_ver)
{
    uint8_t ik[KEY_MAX], gk[KEY_MAX], buf[VAL_MAX], packed[80];
    uint32_t il = 0, gl = 0, bl = VAL_MAX;
    uint64_t ver = 0;
    struct efs_txid have;
    int pn, rc;

    if (!kv || !t || !key || !parts_ok(p))
        return EFS_ERR_INVAL;
    rc = efs_txn_ver_get(kv, key, klen, &ver);
    if (rc != EFS_OK)
        return rc;
    if (ver != observed_ver)
        return EFS_ERR_STALE;
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &bl);
    if (rc == EFS_OK) {
        rc = parse_excl(buf, bl, &have, NULL, NULL, NULL, NULL);
        if (rc != EFS_OK)
            return rc;
        if (!same_txid(have.bytes, t))
            return EFS_ERR_BUSY;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = efs_kv_key_guard(key, klen, t->bytes, gk, &gl);
    if (rc != EFS_OK)
        return rc;
    memcpy(packed, t->bytes, 16);
    pn = pack_parts(packed + 16, p);
    be64(packed + 16 + pn, observed_ver);
    return efs_kv_put(kv, gk, gl, packed, (uint32_t)(16 + pn + 8));
}

int efs_txn_prepare_reduce(struct efs_kv *kv, const struct efs_txid *t,
                           const struct efs_txn_parts *p, const uint8_t *key,
                           uint32_t klen, const struct efs_txn_reduce *red)
{
    uint8_t ik[KEY_MAX], rk[KEY_MAX], buf[VAL_MAX], packed[80];
    uint32_t il = 0, rl = 0, bl = VAL_MAX;
    struct efs_txid have;
    int pn, rc;

    if (!kv || !t || !key || !red || !parts_ok(p))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &bl);
    if (rc == EFS_OK) {
        rc = parse_excl(buf, bl, &have, NULL, NULL, NULL, NULL);
        if (rc != EFS_OK)
            return rc;
        if (!same_txid(have.bytes, t))
            return EFS_ERR_BUSY;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = efs_kv_key_reduce(key, klen, t->bytes, rk, &rl);
    if (rc != EFS_OK)
        return rc;
    memcpy(packed, t->bytes, 16);
    pn = pack_parts(packed + 16, p);
    be64(packed + 16 + pn, red->max_end);
    be64(packed + 16 + pn + 8, red->max_mtime);
    be64(packed + 16 + pn + 16, red->max_ctime);
    return efs_kv_put(kv, rk, rl, packed, (uint32_t)(16 + pn + 24));
}

struct drop_acc {
    struct efs_txid t;
    uint32_t shard;
    struct efs_kv_item it[32];
    uint8_t keys[32][KEY_MAX];
    uint32_t n;
    int rc;
};

static int drop_add(struct drop_acc *a, const uint8_t *key, uint32_t klen)
{
    if (a->n >= 32) {
        a->rc = EFS_ERR_NOMEM;
        return 1;
    }
    memcpy(a->keys[a->n], key, klen);
    a->it[a->n].op = EFS_KV_DEL;
    a->it[a->n].key = a->keys[a->n];
    a->it[a->n].klen = klen;
    a->n++;
    return 0;
}

static int drop_cb(void *user, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct drop_acc *a = user;
    struct efs_txid have;

    if (klen < 3 || key_shard(key) != a->shard)
        return 0;
    if (key[2] == EFS_KV_KIND_INTENT) {
        if (parse_excl(val, vlen, &have, NULL, NULL, NULL, NULL) != EFS_OK)
            return 0;
        if (!same_txid(have.bytes, &a->t))
            return 0;
        return drop_add(a, key, klen);
    }
    if (key[2] == EFS_KV_KIND_GUARD || key[2] == EFS_KV_KIND_REDUCE) {
        if (klen < 16 || memcmp(key + klen - 16, a->t.bytes, 16) != 0)
            return 0;
        return drop_add(a, key, klen);
    }
    return 0;
}

int efs_txn_drop(struct efs_kv *kv, const struct efs_txid *t, uint32_t shard)
{
    struct drop_acc a;
    uint8_t pref[2];
    int rc;

    if (!kv || !t)
        return EFS_ERR_INVAL;
    memset(&a, 0, sizeof(a));
    a.t = *t;
    a.shard = shard;
    pref[0] = (uint8_t)(shard >> 8);
    pref[1] = (uint8_t)shard;
    rc = efs_kv_scan_prefix(kv, pref, 2, drop_cb, &a);
    if (a.rc != EFS_OK)
        return a.rc;
    if (rc != EFS_OK)
        return rc;
    if (a.n == 0)
        return EFS_OK;
    return efs_kv_batch(kv, a.it, a.n);
}

int efs_txn_decide(struct efs_kv *kv, uint32_t coord_shard,
                   const struct efs_txid *t, int decision)
{
    uint8_t key[KEY_MAX], val[1], have[1];
    uint32_t klen = 0, n = 1;
    int rc;

    if (!kv || !t || (decision != EFS_TXN_COMMIT && decision != EFS_TXN_ABORT))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_decision(coord_shard, t->bytes, key, &klen);
    if (rc != EFS_OK)
        return rc;
    val[0] = (uint8_t)decision;
    rc = efs_kv_get(kv, key, klen, have, &n);
    if (rc == EFS_OK) {
        if (n < 1 || have[0] != val[0])
            return EFS_ERR_PROTO;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    return efs_kv_put(kv, key, klen, val, 1);
}

int efs_txn_decision_get(struct efs_kv *kv, uint32_t coord_shard,
                         const struct efs_txid *t, int *decision)
{
    uint8_t key[KEY_MAX], have[1];
    uint32_t klen = 0, n = 1;
    int rc;

    if (!kv || !t || !decision)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_decision(coord_shard, t->bytes, key, &klen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, klen, have, &n);
    if (rc != EFS_OK)
        return rc;
    if (n < 1)
        return EFS_ERR_PROTO;
    *decision = have[0];
    return EFS_OK;
}

static int ask_coord(efs_txn_coord_fn coord, void *ctx, const struct efs_txid *t,
                     const struct efs_txn_parts *p, int *dec)
{
    uint32_t sh;
    int rc;

    sh = efs_txn_coordinator(t, p);
    rc = coord(ctx, t, sh, dec);
    if (rc == EFS_ERR_NOT_FOUND) {
        *dec = EFS_TXN_UNDECIDED;
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return EFS_ERR_IO;
    return EFS_OK;
}

static int decide_visible(efs_txn_coord_fn coord, void *ctx,
                          const struct efs_txid *t, const struct efs_txn_parts *p,
                          int *dec)
{
    int rc;

    rc = ask_coord(coord, ctx, t, p, dec);
    if (rc != EFS_OK)
        return rc;
    if (*dec != EFS_TXN_UNDECIDED)
        return EFS_OK;
    /* Re-check before returning the old value (I17 spanning a decision). */
    return ask_coord(coord, ctx, t, p, dec);
}

int efs_txn_read(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                 efs_txn_coord_fn coord, void *ctx, uint8_t *val, uint32_t *vlen)
{
    uint8_t ik[KEY_MAX], buf[VAL_MAX];
    uint32_t il = 0, bl = VAL_MAX, nv = 0;
    struct efs_txid t;
    struct efs_txn_parts p;
    const uint8_t *nvp = NULL;
    int op = 0, dec = 0, rc;

    if (!kv || !key || !coord || !vlen)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &bl);
    if (rc == EFS_ERR_NOT_FOUND)
        return efs_kv_get(kv, key, klen, val, vlen);
    if (rc != EFS_OK)
        return rc;
    rc = parse_excl(buf, bl, &t, &p, &op, &nvp, &nv);
    if (rc != EFS_OK)
        return rc;
    rc = decide_visible(coord, ctx, &t, &p, &dec);
    if (rc != EFS_OK)
        return rc;
    if (dec == EFS_TXN_COMMIT) {
        if (op == EFS_TXN_DEL)
            return EFS_ERR_NOT_FOUND;
        if (nv > *vlen) {
            *vlen = nv;
            return EFS_ERR_INVAL;
        }
        if (nv && val)
            memcpy(val, nvp, nv);
        *vlen = nv;
        return EFS_OK;
    }
    return efs_kv_get(kv, key, klen, val, vlen);
}

struct red_acc {
    efs_txn_coord_fn coord;
    void *ctx;
    struct efs_txn_reduce *out;
    struct efs_txn_pending *pend;
    int rc;
};

static void pend_add(struct efs_txn_pending *p, const struct efs_txid *t,
                     const struct efs_txn_parts *parts)
{
    uint8_t i;

    if (!p)
        return;
    for (i = 0; i < p->n; i++)
        if (memcmp(p->txid[i].bytes, t->bytes, EFS_TXN_ID_LEN) == 0)
            return; /* one transaction can hold intents on several lanes */
    if (p->n >= EFS_TXN_MAX_PENDING) {
        p->overflow = 1;
        return;
    }
    p->txid[p->n] = *t;
    p->parts[p->n] = *parts;
    p->n++;
}

int efs_txn_pending_recheck(const struct efs_txn_pending *pend,
                            efs_txn_coord_fn coord, void *ctx, int *moved)
{
    uint8_t i;
    int dec, rc;

    if (!coord || !moved)
        return EFS_ERR_INVAL;
    *moved = 0;
    if (!pend)
        return EFS_OK;
    if (pend->overflow) {
        *moved = 1;
        return EFS_OK;
    }
    for (i = 0; i < pend->n; i++) {
        rc = ask_coord(coord, ctx, &pend->txid[i], &pend->parts[i], &dec);
        if (rc != EFS_OK)
            return rc; /* unreachable authority is never "absent" (I9) */
        if (dec != EFS_TXN_UNDECIDED) {
            *moved = 1;
            return EFS_OK;
        }
    }
    return EFS_OK;
}

static int red_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    struct red_acc *a = user;
    struct efs_txid t;
    struct efs_txn_parts p;
    uint32_t used = 0;
    int dec = 0, rc;
    const uint8_t *pay;

    (void)key;
    (void)klen;
    rc = parse_parts_txid(val, vlen, &t, &p, &used);
    if (rc != EFS_OK)
        return 0;
    if (16 + used + 24 > vlen)
        return 0;
    rc = decide_visible(a->coord, a->ctx, &t, &p, &dec);
    if (rc != EFS_OK) {
        a->rc = rc;
        return 1;
    }
    if (dec == EFS_TXN_UNDECIDED)
        pend_add(a->pend, &t, &p);
    if (dec != EFS_TXN_COMMIT)
        return 0;
    pay = val + 16 + used;
    if (rd64(pay) > a->out->max_end)
        a->out->max_end = rd64(pay);
    if (rd64(pay + 8) > a->out->max_mtime)
        a->out->max_mtime = rd64(pay + 8);
    if (rd64(pay + 16) > a->out->max_ctime)
        a->out->max_ctime = rd64(pay + 16);
    return 0;
}

int efs_txn_reduce_read(struct efs_kv *kv, const uint8_t *lane_key, uint32_t klen,
                        efs_txn_coord_fn coord, void *ctx,
                        struct efs_txn_reduce *out)
{
    return efs_txn_reduce_read_ex(kv, lane_key, klen, coord, ctx, out, NULL);
}

int efs_txn_reduce_read_ex(struct efs_kv *kv, const uint8_t *lane_key,
                           uint32_t klen, efs_txn_coord_fn coord, void *ctx,
                           struct efs_txn_reduce *out,
                           struct efs_txn_pending *pend)
{
    uint8_t pref[KEY_MAX], buf[VAL_MAX];
    uint32_t pl = 0, n = sizeof(buf);
    struct red_acc a;
    int rc;

    if (!kv || !lane_key || !coord || !out)
        return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    /* Only the leading reduce triple is this layer's business; the lane's
     * owner keeps its own fields after it. The buffer has to fit the WHOLE
     * record even so, because a short buffer is a hard error rather than a
     * truncated read — sizing it to the triple would make every materialized
     * lane unreadable. */
    rc = efs_kv_get(kv, lane_key, klen, buf, &n);
    if (rc == EFS_OK && n >= 24) {
        out->max_end = rd64(buf);
        out->max_mtime = rd64(buf + 8);
        out->max_ctime = rd64(buf + 16);
    } else if (rc != EFS_ERR_NOT_FOUND && rc != EFS_OK) {
        return rc;
    }
    rc = efs_kv_key_reduce_prefix(lane_key, klen, pref, &pl);
    if (rc != EFS_OK)
        return rc;
    memset(&a, 0, sizeof(a));
    a.coord = coord;
    a.ctx = ctx;
    a.out = out;
    a.pend = pend;
    rc = efs_kv_scan_prefix(kv, pref, pl, red_cb, &a);
    if (a.rc != EFS_OK)
        return a.rc;
    return rc;
}

struct res_acc {
    struct efs_kv *kv;
    struct efs_txid t;
    uint32_t shard;
    int decision;
    struct efs_kv_item it[48];
    uint8_t keys[48][KEY_MAX];
    uint8_t vals[48][VAL_MAX];
    uint32_t n;
    int rc;
};

static int res_add_del(struct res_acc *a, const uint8_t *key, uint32_t klen)
{
    if (a->n >= 48) {
        a->rc = EFS_ERR_NOMEM;
        return 1;
    }
    memcpy(a->keys[a->n], key, klen);
    a->it[a->n].op = EFS_KV_DEL;
    a->it[a->n].key = a->keys[a->n];
    a->it[a->n].klen = klen;
    a->n++;
    return 0;
}

static int res_add_put(struct res_acc *a, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    if (a->n >= 48 || vlen > VAL_MAX) {
        a->rc = EFS_ERR_NOMEM;
        return 1;
    }
    memcpy(a->keys[a->n], key, klen);
    memcpy(a->vals[a->n], val, vlen);
    if (klen >= 3 && key[2] == EFS_KV_KIND_INODE &&
        vlen >= EFS_META_INO_BYTES && val[56] == EFS_META_LAYOUT_SPLITTING)
        efs_dir_spread_note(rd64(val));
    a->it[a->n].op = EFS_KV_PUT;
    a->it[a->n].key = a->keys[a->n];
    a->it[a->n].klen = klen;
    a->it[a->n].val = a->vals[a->n];
    a->it[a->n].vlen = vlen;
    a->n++;
    return 0;
}

static int res_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    struct res_acc *a = user;
    struct efs_txid have;
    struct efs_txn_parts p;
    uint8_t orig[KEY_MAX], vk[KEY_MAX], vv[8];
    uint32_t olen = 0, vl = 0;
    const uint8_t *nv = NULL;
    uint32_t nvl = 0;
    int op = 0;

    if (klen < 3 || key_shard(key) != a->shard)
        return 0;
    if (key[2] == EFS_KV_KIND_INTENT) {
        if (parse_excl(val, vlen, &have, &p, &op, &nv, &nvl) != EFS_OK)
            return 0;
        if (!same_txid(have.bytes, &a->t))
            return 0;
        if (efs_kv_key_unwrap(key, klen, orig, &olen) != EFS_OK)
            return 0;
        if (a->decision == EFS_TXN_COMMIT) {
            if (op == EFS_TXN_DEL) {
                if (res_add_del(a, orig, olen))
                    return 1;
            } else if (res_add_put(a, orig, olen, nv, nvl)) {
                return 1;
            }
            be64(vv, 1);
            if (efs_kv_key_ver(orig, olen, vk, &vl) == EFS_OK)
                (void)res_add_put(a, vk, vl, vv, 8);
        }
        return res_add_del(a, key, klen);
    }
    if (key[2] == EFS_KV_KIND_GUARD || key[2] == EFS_KV_KIND_REDUCE) {
        if (klen < 16 || memcmp(key + klen - 16, a->t.bytes, 16) != 0)
            return 0;
        if (key[2] == EFS_KV_KIND_REDUCE && a->decision == EFS_TXN_COMMIT) {
            uint32_t used = 0;
            uint8_t lane[KEY_MAX];
            uint32_t ln = 0;
            uint8_t folded[32];

            if (parse_parts_txid(val, vlen, &have, &p, &used) != EFS_OK)
                return 0;
            if (efs_kv_key_unwrap(key, klen - 16, lane, &ln) != EFS_OK)
                return 0;
            /* Materialize MAX into the lane key; missing lane starts at 0. */
            memset(folded, 0, sizeof(folded));
            {
                uint8_t cur[32];
                uint32_t cn = 32;
                uint64_t e = 0, mt = 0, ct = 0, seq = 1;

                if (a->kv && efs_kv_get(a->kv, lane, ln, cur, &cn) == EFS_OK &&
                    cn >= 24) {
                    e = rd64(cur);
                    mt = rd64(cur + 8);
                    ct = rd64(cur + 16);
                    seq = (cn >= 32) ? rd64(cur + 24) + 1 : 1;
                }
                if (16 + used + 24 <= vlen) {
                    uint64_t pe = rd64(val + 16 + used);
                    uint64_t pmt = rd64(val + 16 + used + 8);
                    uint64_t pct = rd64(val + 16 + used + 16);

                    if (pe > e)
                        e = pe;
                    if (pmt > mt)
                        mt = pmt;
                    if (pct > ct)
                        ct = pct;
                }
                be64(folded, e);
                be64(folded + 8, mt);
                be64(folded + 16, ct);
                be64(folded + 24, seq);
            }
            if (res_add_put(a, lane, ln, folded, 32))
                return 1;
        }
        return res_add_del(a, key, klen);
    }
    return 0;
}

int efs_txn_resolve(struct efs_kv *kv, const struct efs_txid *t, uint32_t shard,
                    int decision)
{
    struct res_acc a;
    uint8_t pref[2];
    int rc, i;

    if (!kv || !t)
        return EFS_ERR_INVAL;
    if (decision != EFS_TXN_COMMIT && decision != EFS_TXN_ABORT)
        return EFS_ERR_INVAL;
    memset(&a, 0, sizeof(a));
    a.kv = kv;
    a.t = *t;
    a.shard = shard;
    a.decision = decision;
    pref[0] = (uint8_t)(shard >> 8);
    pref[1] = (uint8_t)shard;
    rc = efs_kv_scan_prefix(kv, pref, 2, res_cb, &a);
    if (a.rc != EFS_OK)
        return a.rc;
    if (rc != EFS_OK)
        return rc;
    /* Bump versions from current, not hardcoded 1. */
    for (i = 0; i < (int)a.n; i++) {
        uint8_t orig[KEY_MAX];
        uint32_t olen = 0;
        uint64_t ver = 0;

        if (a.it[i].op != EFS_KV_PUT || a.it[i].klen < 3)
            continue;
        if (a.keys[i][2] != EFS_KV_KIND_VER)
            continue;
        if (efs_kv_key_unwrap(a.keys[i], a.it[i].klen, orig, &olen) != EFS_OK)
            continue;
        if (efs_txn_ver_get(kv, orig, olen, &ver) != EFS_OK)
            ver = 0;
        be64(a.vals[i], ver + 1);
        a.it[i].vlen = 8;
    }
    if (a.n == 0)
        return EFS_OK;
    return efs_kv_batch(kv, a.it, a.n);
}
