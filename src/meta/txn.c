#include "efs/txn.h"
#include "efs/kv_key.h"
#include "efs/dir_spread.h"
#include "efs/dir_layout.h"
#include "efs/meta_apply.h"
#include <string.h>

/* Full histories plus a 64-participant EXCL envelope, with fixed bounds.
 * Data values and durable intent envelopes have separate limits. */
#define VAL_MAX EFS_TXN_RECORD_MAX
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
    if (!t || !p || p->n == 0 || p->n > EFS_TXN_MAX_PART)
        return 0;
    return p->shard[efs_txn_hash(t) % p->n];
}

static int parts_ok(const struct efs_txn_parts *p)
{
    if (!p || !p->n || p->n > EFS_TXN_MAX_PART)
        return 0;
    for (uint32_t i = 0; i < p->n; ++i) {
        if (p->shard[i] > EFS_KV_SHARD_MASK)
            return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (p->shard[i] == p->shard[j])
                return 0;
    }
    return 1;
}

static int pack_parts(uint8_t *out, uint32_t cap,
                       const struct efs_txn_parts *p)
{
    if (!out || !p)
        return EFS_ERR_INVAL;
    uint32_t count = p->n;
    if (!count || count > EFS_TXN_MAX_PART)
        return EFS_ERR_INVAL;
    uint32_t need = 1u + count * 4u;
    if (cap < need)
        return EFS_ERR_INVAL;

    /* Validate before writing, and use the checked count throughout. */
    out[0] = (uint8_t)count;
    for (uint32_t i = 0; i < count; i++)
        be32(out + 1u + i * 4u, p->shard[i]);
    return (int)need;
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
    int any; /* count every record, not just other transactions' */
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
    if (h->any || memcmp(key + klen - 16, h->self.bytes, 16) != 0)
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

/* A pending reduce from another transaction. EXCL and GUARD must not be
 * prepared over one: EXCL would overwrite the fold (its image predates it),
 * GUARD's predicate would not survive it. Reduces among themselves commute
 * and never conflict. `t` NULL = any transaction counts. */
static int reduces_pending(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                           const struct efs_txid *t)
{
    uint8_t pref[KEY_MAX];
    uint32_t pl = 0;
    struct scan_hit h;
    int rc;

    rc = efs_kv_key_reduce_prefix(key, klen, pref, &pl);
    if (rc != EFS_OK)
        return rc;
    memset(&h, 0, sizeof(h));
    if (t)
        h.self = *t;
    else
        h.any = 1;
    rc = efs_kv_scan_prefix(kv, pref, pl, guard_cb, &h);
    if (rc != EFS_OK)
        return rc;
    return h.other ? EFS_ERR_BUSY : EFS_OK;
}

static int key_kind(const uint8_t *key, uint32_t klen)
{
    return klen >= 3 ? key[2] : -1;
}

int efs_txn_dseq_observe(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                         uint64_t *seq)
{
    uint8_t buf[8];
    uint32_t n = 8;
    int rc;

    if (!kv || !key || !seq)
        return EFS_ERR_INVAL;
    *seq = 0;
    rc = efs_kv_get(kv, key, klen, buf, &n);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    if (n < 8)
        return EFS_ERR_PROTO;
    *seq = rd64(buf);
    return EFS_OK;
}

int efs_txn_key_exclusive(struct efs_kv *kv, const uint8_t *key, uint32_t klen)
{
    uint8_t ik[KEY_MAX], byte;
    uint32_t il = 0, n = 1;
    int rc;
    if (!kv || !key || klen < 3)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, &byte, &n);
    if (rc == EFS_OK || rc == EFS_ERR_INVAL)
        return EFS_ERR_BUSY;
    return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
}

int efs_txn_key_busy(struct efs_kv *kv, const uint8_t *key, uint32_t klen)
{
    uint8_t ik[KEY_MAX], buf[VAL_MAX];
    uint32_t il = 0, bl = VAL_MAX;
    int rc;

    if (!kv || !key || klen < 3)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &bl);
    if (rc == EFS_OK || rc == EFS_ERR_INVAL) /* INVAL = present, buffer short */
        return EFS_ERR_BUSY;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    return reduces_pending(kv, key, klen, NULL);
}

static int pack_excl(uint8_t *out, uint32_t cap, uint32_t *n,
                     const struct efs_txid *t, const struct efs_txn_parts *p,
                     uint64_t expected, int op, const uint8_t *val, uint32_t vlen)
{
    uint32_t off;
    int pn;

    if (!parts_ok(p) || (op != EFS_TXN_PUT && op != EFS_TXN_DEL) ||
        vlen > EFS_TXN_VALUE_MAX || (op == EFS_TXN_PUT && vlen && !val))
        return EFS_ERR_INVAL;
    if (op == EFS_TXN_DEL)
        vlen = 0;
    pn = 1 + (int)p->n * 4;
    off = 16u + (uint32_t)pn + 1u + 8u + 1u + 4u + vlen;
    if (off > cap)
        return EFS_ERR_INVAL;
    pn = pack_parts(out + 16, cap - 16u, p);
    if (pn < 0)
        return pn;
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
    if (vl > EFS_TXN_VALUE_MAX || vl > n - (off + 1 + 8 + 1 + 4) ||
        (in[off + 1 + 8] != EFS_TXN_PUT && in[off + 1 + 8] != EFS_TXN_DEL) ||
        (in[off + 1 + 8] == EFS_TXN_DEL && vl))
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

    if (!kv || !t || !key || klen < 3 || !parts_ok(p) ||
        (op != EFS_TXN_PUT && op != EFS_TXN_DEL) || nlen > EFS_TXN_VALUE_MAX ||
        (op == EFS_TXN_PUT && nlen && !new_val))
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
    if (rc == EFS_OK)
        rc = reduces_pending(kv, key, klen, t);
    if (rc != EFS_OK)
        return rc;
    rc = pack_excl(packed, sizeof(packed), &pn, t, p, expected_ver, op, new_val,
                   nlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_put(kv, ik, il, packed, pn);
}

int efs_txn_encode_excl_value(uint8_t *out, uint32_t cap, uint32_t *len,
                              const uint8_t *expected, uint32_t en, int op,
                              const uint8_t *value, uint32_t vn)
{
    uint32_t bytes = en == EFS_TXN_ABSENT ? 0 : en;
    if (!out || !len || (op != EFS_TXN_PUT && op != EFS_TXN_DEL) ||
        bytes > EFS_TXN_VALUE_MAX || vn > EFS_TXN_VALUE_MAX ||
        (bytes && !expected) || (vn && !value) ||
        (op == EFS_TXN_DEL && vn) || cap < 9u + bytes + vn)
        return EFS_ERR_INVAL;
    out[0] = (uint8_t)op;
    be32(out + 1, en); be32(out + 5, vn);
    if (bytes) memcpy(out + 9, expected, bytes);
    if (vn) memcpy(out + 9 + bytes, value, vn);
    *len = 9u + bytes + vn;
    return EFS_OK;
}

int efs_txn_prepare_excl_value(struct efs_kv *kv, const struct efs_txid *t,
                               const struct efs_txn_parts *p, const uint8_t *key,
                               uint32_t klen, const uint8_t *expected,
                               uint32_t en, int op, const uint8_t *value,
                               uint32_t vn)
{
    uint8_t ik[KEY_MAX], buf[VAL_MAX];
    uint32_t il = 0, bn = sizeof(buf), have_len = 0;
    uint64_t ver;
    struct efs_txid have;
    struct efs_txn_parts have_parts;
    const uint8_t *have_value;
    int have_op, rc, member = 0;
    if (!kv || !t || !key || klen < 3 || !parts_ok(p) ||
        (op != EFS_TXN_PUT && op != EFS_TXN_DEL) ||
        (en != EFS_TXN_ABSENT && en > EFS_TXN_VALUE_MAX) ||
        (en != EFS_TXN_ABSENT && en && !expected) ||
        vn > EFS_TXN_VALUE_MAX || (vn && !value) ||
        (op == EFS_TXN_DEL && vn))
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < p->n; ++i)
        member |= p->shard[i] == key_shard(key);
    if (!member) return EFS_ERR_INVAL;
    rc = efs_kv_key_intent(key, klen, ik, &il);
    if (rc != EFS_OK) return rc;
    rc = efs_kv_get(kv, ik, il, buf, &bn);
    if (rc == EFS_OK) {
        rc = parse_excl(buf, bn, &have, &have_parts, &have_op,
                         &have_value, &have_len);
        if (rc != EFS_OK) return rc;
        if (!same_txid(have.bytes, t)) return EFS_ERR_BUSY;
        if (have_parts.n != p->n ||
            memcmp(have_parts.shard, p->shard, p->n * sizeof(p->shard[0])) ||
            have_op != op || have_len != vn ||
            (vn && memcmp(have_value, value, vn)))
            return EFS_ERR_STALE;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND) return rc;
    bn = sizeof(buf);
    rc = efs_kv_get(kv, key, klen, buf, &bn);
    if (rc == EFS_ERR_NOT_FOUND) {
        if (en != EFS_TXN_ABSENT) return EFS_ERR_STALE;
    } else if (rc == EFS_OK) {
        if (en == EFS_TXN_ABSENT || bn != en || (en && memcmp(buf, expected, en)))
            return EFS_ERR_STALE;
    } else {
        return rc;
    }
    rc = efs_txn_ver_get(kv, key, klen, &ver);
    if (rc != EFS_OK) return rc;
    return efs_txn_prepare_excl(kv, t, p, key, klen, ver, op, value, vn);
}

/* Stage the existing exact-value PREPARE implementation's intent PUTs.
 * No write reaches the real KV until every request passes. The participant
 * log serializes the read/validate/batch sequence, as for single PREPARE. */
struct pair_plan {
    struct efs_kv *base;
    struct efs_kv_item items[3];
    uint8_t keys[3][KEY_MAX], values[3][VAL_MAX];
    uint32_t count;
};

static int pair_get(void *ctx, const uint8_t *key, uint32_t kl,
                     uint8_t *value, uint32_t *vl)
{
    struct pair_plan *plan = ctx;
    return efs_kv_get(plan->base, key, kl, value, vl);
}

static int pair_scan(void *ctx, const uint8_t *prefix, uint32_t plen,
                      int (*cb)(void *, const uint8_t *, uint32_t,
                                const uint8_t *, uint32_t), void *user)
{
    struct pair_plan *plan = ctx;
    return efs_kv_scan_prefix(plan->base, prefix, plen, cb, user);
}

static int pair_put(void *ctx, const uint8_t *key, uint32_t kl,
                     const uint8_t *value, uint32_t vl)
{
    struct pair_plan *plan = ctx;
    uint32_t n = plan->count;
    if (n >= 3 || kl > KEY_MAX || vl > VAL_MAX)
        return EFS_ERR_INVAL;
    memcpy(plan->keys[n], key, kl);
    memcpy(plan->values[n], value, vl);
    plan->items[n] = (struct efs_kv_item){EFS_KV_PUT, plan->keys[n], kl,
                                         plan->values[n], vl};
    ++plan->count;
    return EFS_OK;
}

int efs_txn_prepare_excl_batch(struct efs_kv *kv, const struct efs_txid *t,
                              const struct efs_txn_parts *p,
                              const struct efs_txn_value_cas *requests,
                              uint32_t count)
{
    struct pair_plan plan = {.base = kv};
    const struct efs_kv_ops ops = {.get = pair_get, .put = pair_put,
                                   .scan_prefix = pair_scan};
    struct efs_kv staged = {.ops = &ops, .ctx = &plan};
    int rc;
    if (!kv || !requests || !count || count > 3)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < count; ++i) {
        const struct efs_txn_value_cas *q = &requests[i];
        if (!q->key || q->klen < 3)
            return EFS_ERR_INVAL;
        if (key_shard(q->key) != key_shard(requests[0].key))
            return EFS_ERR_INVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (q->klen == requests[j].klen &&
                !memcmp(q->key, requests[j].key, q->klen))
                return EFS_ERR_INVAL;
        rc = efs_txn_prepare_excl_value(&staged, t, p, q->key, q->klen,
                                        q->expected_value, q->expected_len,
                                        q->op, q->new_value, q->new_len);
        if (rc != EFS_OK)
            return rc;
    }
    return plan.count ? efs_kv_batch(kv, plan.items, plan.count) : EFS_OK;
}

int efs_txn_prepare_excl_pair(struct efs_kv *kv, const struct efs_txid *t,
                              const struct efs_txn_parts *p,
                              const struct efs_txn_value_cas *requests,
                              uint32_t count)
{
    if (count > 2)
        return EFS_ERR_INVAL;
    return efs_txn_prepare_excl_batch(kv, t, p, requests, count);
}

int efs_txn_prepare_guard(struct efs_kv *kv, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t observed_ver)
{
    uint8_t ik[KEY_MAX], gk[KEY_MAX], buf[VAL_MAX], packed[16u + EFS_TXN_PARTS_BYTES + 8u];
    uint32_t il = 0, gl = 0, bl = VAL_MAX;
    uint64_t ver = 0;
    struct efs_txid have;
    int pn, rc;

    if (!kv || !t || !key || !parts_ok(p))
        return EFS_ERR_INVAL;
    /* A dseq witness is guarded by VALUE: it is bumped by unversioned
     * log-path applies and by REDUCE_ADD folds, so its version sidecar
     * says nothing. The value is monotone, so "unchanged" is exact. */
    if (key_kind(key, klen) == EFS_KV_KIND_DSEQ)
        rc = efs_txn_dseq_observe(kv, key, klen, &ver);
    else
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
    rc = reduces_pending(kv, key, klen, t);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_guard(key, klen, t->bytes, gk, &gl);
    if (rc != EFS_OK)
        return rc;
    memcpy(packed, t->bytes, 16);
    pn = pack_parts(packed + 16, EFS_TXN_PARTS_BYTES, p);
    if (pn < 0)
        return pn;
    be64(packed + 16 + pn, observed_ver);
    return efs_kv_put(kv, gk, gl, packed, (uint32_t)(16 + pn + 8));
}

void efs_txn_encode_reduce(uint8_t out[EFS_TXN_REDUCE_WIRE],
                           const struct efs_txn_reduce *red)
{
    be64(out, red->max_end);
    be64(out + 8, red->max_mtime);
    be64(out + 16, red->max_ctime);
    be64(out + 24, red->mtime_gen);
}

void efs_txn_encode_ino_delta(uint8_t out[EFS_TXN_REDUCE_INO_WIRE],
                              const struct efs_txn_ino_delta *d)
{
    be32(out, (uint32_t)d->d_nlink);
    be32(out + 4, (uint32_t)d->d_nents);
    be64(out + 8, d->max_mtime);
    be64(out + 16, d->max_ctime);
    be64(out + 24, d->or_used_shards);
    be64(out + 32, d->set_parent);
    be32(out + 40, (uint32_t)d->d_pver);
    be32(out + 44, 0);
}

/* Decoder shared by apply (PREPARE) and resolve (fold). */
static void decode_ino_delta(const uint8_t *pay, struct efs_txn_ino_delta *d)
{
    d->d_nlink = (int32_t)rd32(pay);
    d->d_nents = (int32_t)rd32(pay + 4);
    d->max_mtime = rd64(pay + 8);
    d->max_ctime = rd64(pay + 16);
    d->or_used_shards = rd64(pay + 24);
    d->set_parent = rd64(pay + 32);
    d->d_pver = (int32_t)rd32(pay + 40);
}

void efs_txn_encode_add(uint8_t out[EFS_TXN_REDUCE_ADD_WIRE], uint64_t add)
{
    be64(out, add);
}

void efs_txn_encode_opid(uint8_t out[EFS_TXN_REDUCE_OPID_WIRE],
                         const struct efs_opid_req *q,
                         const struct efs_opid_reply *reply)
{
    be64(out, q->id.seq);
    be32(out + 8, (uint32_t)reply->rc);
    be64(out + 12, reply->ino);
    be64(out + 20, reply->extra);
    be64(out + 28, q->ack);
}

/* The window key carries uuid (key+3) and epoch (key+19, BE); the record
 * carries seq/verdict/ack. Rebuild the request the fold needs. */
static int decode_opid(const uint8_t *key, uint32_t klen, const uint8_t *pay,
                       uint32_t plen, struct efs_opid_req *q,
                       struct efs_opid_reply *rep)
{
    if (klen < 3 + EFS_OPID_UUID_LEN + 4 || key[2] != EFS_KV_KIND_OPID ||
        plen < EFS_TXN_REDUCE_OPID_WIRE)
        return EFS_ERR_PROTO;
    memset(q, 0, sizeof(*q));
    memset(rep, 0, sizeof(*rep));
    memcpy(q->id.client_uuid, key + 3, EFS_OPID_UUID_LEN);
    q->id.session_epoch = rd32(key + 3 + EFS_OPID_UUID_LEN);
    q->id.seq = rd64(pay);
    rep->seq = q->id.seq;
    rep->rc = (int)(int32_t)rd32(pay + 8);
    rep->ino = rd64(pay + 12);
    rep->extra = rd64(pay + 20);
    q->ack = rd64(pay + 28);
    return EFS_OK;
}

/* Record under reduce(key, txid): [txid][parts][payload]. The payload's
 * meaning follows the KIND OF THE DATA KEY (lane triple, inode delta, u64
 * add) — one record shape, so drop/resolve find every reduce of a txn by
 * its suffix regardless of kind. A pending EXCL intent or another txn's
 * GUARD on the key is BUSY; other reduces are not (they commute). */
static int prepare_reduce_rec(struct efs_kv *kv, const struct efs_txid *t,
                              const struct efs_txn_parts *p, const uint8_t *key,
                              uint32_t klen, const uint8_t *pay, uint32_t plen)
{
    uint8_t ik[KEY_MAX], rk[KEY_MAX], buf[VAL_MAX], packed[16u + EFS_TXN_PARTS_BYTES + 48u];
    uint32_t il = 0, rl = 0, bl = VAL_MAX;
    struct efs_txid have;
    int pn, rc;

    if (!kv || !t || !key || klen < 3 || !parts_ok(p) || plen > 48)
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
    rc = guards_conflict(kv, key, klen, t);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_reduce(key, klen, t->bytes, rk, &rl);
    if (rc != EFS_OK)
        return rc;
    memcpy(packed, t->bytes, 16);
    pn = pack_parts(packed + 16, EFS_TXN_PARTS_BYTES, p);
    if (pn < 0)
        return pn;
    memcpy(packed + 16 + pn, pay, plen);
    return efs_kv_put(kv, rk, rl, packed, (uint32_t)(16 + pn) + plen);
}

int efs_txn_prepare_reduce(struct efs_kv *kv, const struct efs_txid *t,
                           const struct efs_txn_parts *p, const uint8_t *key,
                           uint32_t klen, const struct efs_txn_reduce *red)
{
    uint8_t pay[EFS_TXN_REDUCE_WIRE];

    if (!red)
        return EFS_ERR_INVAL;
    efs_txn_encode_reduce(pay, red);
    return prepare_reduce_rec(kv, t, p, key, klen, pay, sizeof(pay));
}

int efs_txn_prepare_ino_delta(struct efs_kv *kv, const struct efs_txid *t,
                              const struct efs_txn_parts *p, const uint8_t *key,
                              uint32_t klen, const struct efs_txn_ino_delta *d)
{
    uint8_t pay[EFS_TXN_REDUCE_INO_WIRE];

    if (!d || key_kind(key, klen) != EFS_KV_KIND_INODE)
        return EFS_ERR_INVAL;
    efs_txn_encode_ino_delta(pay, d);
    return prepare_reduce_rec(kv, t, p, key, klen, pay, sizeof(pay));
}

int efs_txn_prepare_add(struct efs_kv *kv, const struct efs_txid *t,
                        const struct efs_txn_parts *p, const uint8_t *key,
                        uint32_t klen, uint64_t add)
{
    uint8_t pay[EFS_TXN_REDUCE_ADD_WIRE];

    if (key_kind(key, klen) != EFS_KV_KIND_DSEQ)
        return EFS_ERR_INVAL;
    efs_txn_encode_add(pay, add);
    return prepare_reduce_rec(kv, t, p, key, klen, pay, sizeof(pay));
}

int efs_txn_prepare_opid(struct efs_kv *kv, const struct efs_txid *t,
                         const struct efs_txn_parts *p, const uint8_t *key,
                         uint32_t klen, const struct efs_opid_req *q,
                         const struct efs_opid_reply *reply)
{
    uint8_t pay[EFS_TXN_REDUCE_OPID_WIRE];

    /* Exactly the window key (shard, kind, uuid, epoch): a truncated key
     * would PREPARE a record decode_opid can never fold at RESOLVE. */
    if (!q || !reply || key_kind(key, klen) != EFS_KV_KIND_OPID ||
        klen != 3 + EFS_OPID_UUID_LEN + 4)
        return EFS_ERR_INVAL;
    efs_txn_encode_opid(pay, q, reply);
    return prepare_reduce_rec(kv, t, p, key, klen, pay, sizeof(pay));
}

int efs_txn_apply_prepare(struct efs_kv *kv, int kind, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, const uint8_t *pay, uint32_t plen)
{
    struct efs_txn_reduce red;
    struct efs_txn_ino_delta d;

    if (!pay && plen)
        return EFS_ERR_PROTO;
    switch (kind) {
    case EFS_TXN_EXCL: {
        uint64_t expected;
        uint32_t vlen;
        int op;

        if (plen < 13)
            return EFS_ERR_PROTO;
        expected = rd64(pay);
        op = pay[8];
        vlen = rd32(pay + 9);
        if (vlen > EFS_TXN_VALUE_MAX || vlen > plen - 13)
            return EFS_ERR_PROTO;
        return efs_txn_prepare_excl(kv, t, p, key, klen, expected, op,
                                    pay + 13, vlen);
    }
    case EFS_TXN_EXCL_VALUE: {
        uint32_t en, vn, bytes;
        int op;
        if (plen < 9) return EFS_ERR_PROTO;
        op = pay[0]; en = rd32(pay + 1); vn = rd32(pay + 5);
        bytes = en == EFS_TXN_ABSENT ? 0 : en;
        if (bytes > EFS_TXN_VALUE_MAX || vn > EFS_TXN_VALUE_MAX ||
            plen != 9u + bytes + vn || (op != EFS_TXN_PUT && op != EFS_TXN_DEL) ||
            (op == EFS_TXN_DEL && vn))
            return EFS_ERR_PROTO;
        return efs_txn_prepare_excl_value(kv, t, p, key, klen, pay + 9,
                                          en, op, pay + 9 + bytes, vn);
    }
    case EFS_TXN_CONTENT_FENCE:
        return efs_meta_apply_fence_prepare(kv, t, p, key, klen, pay, plen);
    case EFS_TXN_LANE_BOOTSTRAP:
        return efs_meta_apply_lane_bootstrap(kv, t, p, key, klen, pay, plen);
    case EFS_TXN_CONTENT_RESIZE:
        return efs_meta_apply_resize_prepare(kv, t, p, key, klen, pay, plen);
    case EFS_TXN_GUARD:
        if (plen < EFS_TXN_GUARD_WIRE)
            return EFS_ERR_PROTO;
        return efs_txn_prepare_guard(kv, t, p, key, klen, rd64(pay));
    case EFS_TXN_REDUCE:
        /* 24-byte payload = pre-mtime_gen encoder. */
        if (plen < 24)
            return EFS_ERR_PROTO;
        memset(&red, 0, sizeof(red));
        red.max_end = rd64(pay);
        red.max_mtime = rd64(pay + 8);
        red.max_ctime = rd64(pay + 16);
        if (plen >= EFS_TXN_REDUCE_WIRE)
            red.mtime_gen = rd64(pay + 24);
        return efs_txn_prepare_reduce(kv, t, p, key, klen, &red);
    case EFS_TXN_REDUCE_INO:
        if (plen < EFS_TXN_REDUCE_INO_WIRE)
            return EFS_ERR_PROTO;
        decode_ino_delta(pay, &d);
        return efs_txn_prepare_ino_delta(kv, t, p, key, klen, &d);
    case EFS_TXN_REDUCE_ADD:
        if (plen < EFS_TXN_REDUCE_ADD_WIRE)
            return EFS_ERR_PROTO;
        return efs_txn_prepare_add(kv, t, p, key, klen, rd64(pay));
    case EFS_TXN_REDUCE_OPID: {
        struct efs_opid_req q;
        struct efs_opid_reply rep;

        if (decode_opid(key, klen, pay, plen, &q, &rep) != EFS_OK)
            return EFS_ERR_PROTO;
        return efs_txn_prepare_opid(kv, t, p, key, klen, &q, &rep);
    }
    default:
        return EFS_ERR_PROTO;
    }
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

/* The transaction records of one shard live under exactly three key kinds.
 * Visit those three prefixes, never the whole shard: a 2-byte shard scan
 * walks every inode, dentry, chunk and lane key the shard holds (and every
 * tombstone the LSM has not compacted yet) through the full segment merge,
 * under the KV lock and the apply lock. On the mount root's shard that was
 * 0.7–1.1 s per RESOLVE (Sep 21, results/measure/20260921-202253-w8-root-srv),
 * long enough for the 400 ms apply wait to return BUSY and for the client to
 * retry a committed UNLINK into ENOENT. */
static int txn_scan_kinds(struct efs_kv *kv, uint32_t shard,
                          int (*cb)(void *user, const uint8_t *key,
                                    uint32_t klen, const uint8_t *val,
                                    uint32_t vlen),
                          void *user)
{
    static const uint8_t kinds[3] = { EFS_KV_KIND_INTENT, EFS_KV_KIND_GUARD,
                                      EFS_KV_KIND_REDUCE };
    uint8_t pref[3];
    int i, rc;

    pref[0] = (uint8_t)(shard >> 8);
    pref[1] = (uint8_t)shard;
    for (i = 0; i < 3; i++) {
        pref[2] = kinds[i];
        rc = efs_kv_scan_prefix(kv, pref, 3, cb, user);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

int efs_txn_drop(struct efs_kv *kv, const struct efs_txid *t, uint32_t shard)
{
    struct drop_acc a;
    int rc;

    if (!kv || !t)
        return EFS_ERR_INVAL;
    memset(&a, 0, sizeof(a));
    a.t = *t;
    a.shard = shard;
    rc = txn_scan_kinds(kv, shard, drop_cb, &a);
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

static void pend_add(struct efs_txn_pending *p, const struct efs_txid *t,
                       const struct efs_txn_parts *parts);

int efs_txn_read_ex(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                    efs_txn_coord_fn coord, void *ctx, uint8_t *val,
                    uint32_t *vlen, struct efs_txn_pending *pend)
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
    if (dec == EFS_TXN_UNDECIDED)
        pend_add(pend, &t, &p);
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

int efs_txn_read(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                 efs_txn_coord_fn coord, void *ctx, uint8_t *val, uint32_t *vlen)
{
    return efs_txn_read_ex(kv, key, klen, coord, ctx, val, vlen, NULL);
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
    if (16 + used + 32 <= vlen && rd64(pay + 24) > a->out->mtime_gen)
        a->out->mtime_gen = rd64(pay + 24);
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
    rc = efs_txn_read_ex(kv, lane_key, klen, coord, ctx, buf, &n, pend);
    if (rc == EFS_OK && n >= 24) {
        out->max_end = rd64(buf);
        out->max_mtime = rd64(buf + 8);
        out->max_ctime = rd64(buf + 16);
        if (n >= 48)
            out->mtime_gen = rd64(buf + 40);
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

/* Queue a version bump for `key` (the post-scan loop reads the current
 * version and writes +1). An EXCL prepared against the pre-fold version
 * must land STALE, or its full image would overwrite the fold. */
static int res_add_ver_bump(struct res_acc *a, const uint8_t *key, uint32_t klen)
{
    uint8_t vk[KEY_MAX], vv[8];
    uint32_t vl = 0;

    be64(vv, 1);
    if (efs_kv_key_ver(key, klen, vk, &vl) != EFS_OK)
        return 0;
    return res_add_put(a, vk, vl, vv, 8);
}

/* COMMIT of a reduce record: fold its payload into the data key as it is
 * NOW. Dispatch on the data key's kind. Missing data key: a lane starts
 * from zero; a dseq starts from zero; an inode row that is gone means the
 * directory was removed after this reduce was prepared, which the BUSY
 * rules prevent — treat as nothing to fold rather than resurrect it. */
static int fold_reduce(struct res_acc *a, const uint8_t *key, uint32_t klen,
                       const uint8_t *pay, uint32_t plen)
{
    uint8_t cur[VAL_MAX];
    uint32_t cn = VAL_MAX;
    int rc;

    if (!a->kv || klen < 3)
        return 0;
    rc = efs_kv_get(a->kv, key, klen, cur, &cn);
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) {
        a->rc = rc;
        return 1;
    }
    if (rc == EFS_ERR_NOT_FOUND)
        cn = 0;
    switch (key[2]) {
    case EFS_KV_KIND_LANE: {
        /* MAX triple + seq++ + mtime_gen MAX in place; the owner's tail
         * (fenced_epoch, append_bar, ...) is preserved, not truncated. */
        uint8_t out[VAL_MAX];
        uint32_t on = cn > 48 ? cn : 48;
        uint64_t e = 0, mt = 0, ct = 0, seq = 0, gen = 0;

        if (plen < 24)
            return 0;
        memset(out, 0, on);
        memcpy(out, cur, cn);
        if (cn >= 24) {
            e = rd64(cur);
            mt = rd64(cur + 8);
            ct = rd64(cur + 16);
        }
        if (cn >= 32)
            seq = rd64(cur + 24);
        if (cn >= 48)
            gen = rd64(cur + 40);
        if (rd64(pay) > e)
            e = rd64(pay);
        if (rd64(pay + 8) > mt)
            mt = rd64(pay + 8);
        if (rd64(pay + 16) > ct)
            ct = rd64(pay + 16);
        if (plen >= 32 && rd64(pay + 24) > gen)
            gen = rd64(pay + 24);
        be64(out, e);
        be64(out + 8, mt);
        be64(out + 16, ct);
        be64(out + 24, seq + 1);
        be64(out + 40, gen);
        if (res_add_put(a, key, klen, out, on))
            return 1;
        return res_add_ver_bump(a, key, klen);
    }
    case EFS_KV_KIND_INODE: {
        struct efs_meta_row r;
        struct efs_txn_ino_delta d;
        uint8_t out[EFS_META_INO_BYTES];
        int64_t nl;
        int32_t i;

        if (plen < EFS_TXN_REDUCE_INO_WIRE || cn == 0)
            return 0;
        if (efs_meta_unpack_inode(cur, cn, &r) != EFS_OK)
            return 0;
        decode_ino_delta(pay, &d);
        nl = (int64_t)r.nlink + d.d_nlink;
        r.nlink = nl < 0 ? 0 : (nl > UINT32_MAX ? UINT32_MAX : (uint32_t)nl);
        for (i = 0; i < d.d_nents; i++)
            efs_meta_dir_note_entry(&r, 1);
        for (i = 0; i > d.d_nents; i--)
            efs_meta_dir_note_entry(&r, -1);
        if (d.max_mtime > r.base_mtime)
            r.base_mtime = d.max_mtime;
        if (d.max_ctime > r.base_ctime)
            r.base_ctime = d.max_ctime;
        r.used_shards |= d.or_used_shards;
        if (d.set_parent)
            r.parent = d.set_parent;
        r.parent_version = (uint64_t)((int64_t)r.parent_version + d.d_pver);
        if (efs_meta_pack_inode(&r, out, sizeof(out)) != EFS_OK)
            return 0;
        if (res_add_put(a, key, klen, out, sizeof(out)))
            return 1;
        return res_add_ver_bump(a, key, klen);
    }
    case EFS_KV_KIND_DSEQ: {
        uint8_t out[8];
        uint64_t seq = 0;

        if (plen < EFS_TXN_REDUCE_ADD_WIRE)
            return 0;
        if (cn >= 8)
            seq = rd64(cur);
        be64(out, seq + rd64(pay));
        return res_add_put(a, key, klen, out, 8);
    }
    case EFS_KV_KIND_OPID: {
        /* Ack + one verdict folded into the window as it is now; commutes
         * with the log path's read-modify-write of the same window. A
         * window with no room leaves the op unrecorded, never unapplied. */
        struct efs_opid_req q;
        struct efs_opid_reply rep;
        uint8_t out[EFS_OPID_VAL_MAX];
        uint32_t on = sizeof(out);
        int frc;

        if (decode_opid(key, klen, pay, plen, &q, &rep) != EFS_OK)
            return 0;
        frc = efs_opid_fold(cur, cn, q.id.client_uuid, q.id.session_epoch, &q,
                            &rep, out, &on);
        if (frc != EFS_OK)
            return 0;
        return res_add_put(a, key, klen, out, on);
    }
    default:
        return 0;
    }
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
            uint8_t orig_k[KEY_MAX];
            uint32_t on = 0;

            if (parse_parts_txid(val, vlen, &have, &p, &used) != EFS_OK)
                return 0;
            if (efs_kv_key_unwrap(key, klen - 16, orig_k, &on) != EFS_OK)
                return 0;
            if (fold_reduce(a, orig_k, on, val + 16 + used, vlen - 16 - used))
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
    rc = txn_scan_kinds(kv, shard, res_cb, &a);
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

struct pend_acc {
    uint32_t shard;
    struct efs_txn_pending_rec *out;
    uint32_t cap, n;
    int overflow;
};

static int pend_cb(void *user, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct pend_acc *a = user;
    struct efs_txid t;
    struct efs_txn_parts p;
    uint32_t i, used = 0;

    if (klen < 3 || key_shard(key) != a->shard)
        return 0;
    if (key[2] != EFS_KV_KIND_INTENT && key[2] != EFS_KV_KIND_GUARD &&
        key[2] != EFS_KV_KIND_REDUCE)
        return 0;
    /* Every record kind starts its value with txid[16] + parts. */
    if (parse_parts_txid(val, vlen, &t, &p, &used) != EFS_OK)
        return 0;
    for (i = 0; i < a->n; i++)
        if (same_txid(a->out[i].t.bytes, &t))
            return 0;
    if (a->n >= a->cap) {
        a->overflow = 1;
        return 1;
    }
    a->out[a->n].t = t;
    a->out[a->n].parts = p;
    a->n++;
    return 0;
}

int efs_txn_scan_pending(struct efs_kv *kv, uint32_t shard,
                         struct efs_txn_pending_rec *out, uint32_t cap,
                         uint32_t *n)
{
    struct pend_acc a;
    int rc;

    if (!kv || !out || !n || cap == 0)
        return EFS_ERR_INVAL;
    memset(&a, 0, sizeof(a));
    a.shard = shard;
    a.out = out;
    a.cap = cap;
    rc = txn_scan_kinds(kv, shard, pend_cb, &a);
    *n = a.n;
    if (a.overflow)
        return EFS_OK; /* a full page; the caller sweeps again */
    return rc;
}
