#include "efs/lock.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include <string.h>

#define DROP_MAX 32
#define SCAN_MAX 64
#define LOCK_KLEN 65

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

static int owner_eq(const struct efs_lock_owner *a, const struct efs_lock_owner *b)
{
    return a && b && a->kind == b->kind && a->id == b->id && a->epoch == b->epoch &&
           memcmp(a->uuid, b->uuid, EFS_OPID_UUID_LEN) == 0;
}

static int overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

static int parse_key(const uint8_t *key, uint32_t klen, struct efs_lock_req *r)
{
    if (!key || !r || klen < LOCK_KLEN || key[2] != EFS_KV_KIND_LOCK)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->ino = rd64(key + 3);
    r->generation = rd64(key + 11);
    r->domain = key[19];
    r->start = rd64(key + 20);
    r->end = rd64(key + 28);
    r->owner.kind = key[36];
    r->owner.id = rd64(key + 37);
    memcpy(r->owner.uuid, key + 45, EFS_OPID_UUID_LEN);
    r->owner.epoch = rd32(key + 45 + EFS_OPID_UUID_LEN);
    return EFS_OK;
}

static int conflicts(const struct efs_lock_req *a, const struct efs_lock_req *b)
{
    if (a->domain != b->domain)
        return 0;
    if (!overlap(a->start, a->end, b->start, b->end))
        return 0;
    if (owner_eq(&a->owner, &b->owner))
        return 0;
    return a->type == EFS_LOCK_EX || b->type == EFS_LOCK_EX;
}

struct scan_acc {
    const struct efs_lock_req *req;
    struct efs_lock_req recs[SCAN_MAX];
    uint8_t keys[SCAN_MAX][EFS_KV_KEY_MAX];
    uint32_t klen[SCAN_MAX];
    int n;
    int full;
    struct efs_lock_owner by;
    int blocked;
    int owner_hit;
};

static int scan_cb(void *user, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct scan_acc *a = user;
    struct efs_lock_req r;

    if (parse_key(key, klen, &r) != EFS_OK)
        return 0;
    if (vlen >= 1)
        r.type = val[0];
    if (a->n >= SCAN_MAX) {
        a->full = 1;
        return 1;
    }
    a->recs[a->n] = r;
    memcpy(a->keys[a->n], key, klen);
    a->klen[a->n] = klen;
    a->n++;
    return 0;
}

static int load_domain(struct efs_kv *kv, const struct efs_lock_req *req,
                       struct scan_acc *a)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    memset(a, 0, sizeof(*a));
    a->req = req;
    rc = efs_kv_key_lock_prefix(efs_kv_inode_shard(req->ino), req->ino,
                                req->generation, req->domain, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan_prefix(kv, pref, plen, scan_cb, a);
    if (rc != EFS_OK && rc != 1)
        return rc;
    if (a->full)
        return EFS_ERR_NOLCK;
    return EFS_OK;
}

static int check_gen(struct efs_kv *kv, const struct efs_lock_req *req)
{
    struct efs_meta_row row;
    int rc;

    rc = efs_meta_apply_get_inode(kv, req->ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.generation != req->generation)
        return EFS_ERR_STALE;
    return EFS_OK;
}

int efs_lock_blocked(struct efs_kv *kv, const struct efs_lock_req *req,
                     struct efs_lock_owner *by)
{
    struct scan_acc a;
    int i, rc;

    if (!kv || !req)
        return EFS_ERR_INVAL;
    rc = check_gen(kv, req);
    if (rc != EFS_OK)
        return rc;
    rc = load_domain(kv, req, &a);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < a.n; i++) {
        if (conflicts(req, &a.recs[i])) {
            if (by)
                *by = a.recs[i].owner;
            return 1;
        }
    }
    return 0;
}

int efs_lock_getlk(struct efs_kv *kv, const struct efs_lock_req *req,
                   struct efs_lock_req *out)
{
    struct scan_acc a;
    int i, rc;

    if (!kv || !req || !out)
        return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    rc = check_gen(kv, req);
    if (rc != EFS_OK)
        return rc;
    rc = load_domain(kv, req, &a);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < a.n; i++) {
        if (conflicts(req, &a.recs[i])) {
            *out = a.recs[i];
            return EFS_OK;
        }
    }
    return EFS_OK;
}

int efs_lock_owner_blocks(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          uint8_t domain, const struct efs_lock_owner *owner,
                          uint64_t start, uint64_t end, uint8_t type)
{
    struct efs_lock_req probe;
    struct scan_acc a;
    int i, rc;

    if (!kv || !owner || ino == 0)
        return EFS_ERR_INVAL;
    memset(&probe, 0, sizeof(probe));
    probe.ino = ino;
    probe.generation = gen;
    probe.domain = domain;
    probe.type = type;
    probe.start = start;
    probe.end = end;
    probe.owner = *owner;
    rc = load_domain(kv, &probe, &a);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < a.n; i++) {
        if (owner_eq(&a.recs[i].owner, owner) &&
            overlap(start, end, a.recs[i].start, a.recs[i].end) &&
            (type == EFS_LOCK_EX || a.recs[i].type == EFS_LOCK_EX))
            return 1;
    }
    return 0;
}

int efs_lock_grant(struct efs_kv *kv, const struct efs_lock_req *req)
{
    struct scan_acc a;
    struct efs_kv_item it[SCAN_MAX + 1];
    uint8_t key[EFS_KV_KEY_MAX], one;
    uint32_t klen = 0, n = 0;
    int i, ndel = 0, rc;

    if (!kv || !req || req->ino == 0 ||
        (req->type != EFS_LOCK_SH && req->type != EFS_LOCK_EX))
        return EFS_ERR_INVAL;
    if (req->domain != EFS_LOCK_FCNTL && req->domain != EFS_LOCK_FLOCK)
        return EFS_ERR_INVAL;
    rc = check_gen(kv, req);
    if (rc != EFS_OK)
        return rc;
    rc = load_domain(kv, req, &a);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < a.n; i++) {
        if (conflicts(req, &a.recs[i]))
            return EFS_ERR_AGAIN;
        if (owner_eq(&req->owner, &a.recs[i].owner) &&
            overlap(req->start, req->end, a.recs[i].start, a.recs[i].end)) {
            if (a.recs[i].start == req->start && a.recs[i].end == req->end &&
                a.recs[i].type == req->type)
                return EFS_OK;
            ndel++;
        }
    }
    if ((uint32_t)(a.n - ndel + 1) > EFS_LOCK_CAP)
        return EFS_ERR_NOLCK;
    rc = efs_kv_key_lock(efs_kv_inode_shard(req->ino), req->ino, req->generation,
                         req->domain, req->start, req->end, req->owner.kind,
                         req->owner.id, req->owner.uuid, req->owner.epoch, key,
                         &klen);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    for (i = 0; i < a.n; i++) {
        if (owner_eq(&req->owner, &a.recs[i].owner) &&
            overlap(req->start, req->end, a.recs[i].start, a.recs[i].end) &&
            !(a.recs[i].start == req->start && a.recs[i].end == req->end &&
              a.recs[i].type == req->type)) {
            it[n].op = EFS_KV_DEL;
            it[n].key = a.keys[i];
            it[n].klen = a.klen[i];
            n++;
        }
    }
    one = req->type;
    it[n].op = EFS_KV_PUT;
    it[n].key = key;
    it[n].klen = klen;
    it[n].val = &one;
    it[n].vlen = 1;
    n++;
    return efs_kv_batch(kv, it, n);
}

int efs_lock_release(struct efs_kv *kv, const struct efs_lock_req *req)
{
    struct scan_acc a;
    struct efs_kv_item it[SCAN_MAX];
    uint32_t n = 0;
    int i, rc;

    if (!kv || !req || req->ino == 0)
        return EFS_ERR_INVAL;
    rc = check_gen(kv, req);
    if (rc != EFS_OK)
        return rc;
    rc = load_domain(kv, req, &a);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    for (i = 0; i < a.n; i++) {
        if (!owner_eq(&req->owner, &a.recs[i].owner))
            continue;
        if (req->start == 0 && req->end == ~(uint64_t)0) {
            it[n].op = EFS_KV_DEL;
            it[n].key = a.keys[i];
            it[n].klen = a.klen[i];
            n++;
            continue;
        }
        if (a.recs[i].start == req->start && a.recs[i].end == req->end) {
            it[n].op = EFS_KV_DEL;
            it[n].key = a.keys[i];
            it[n].klen = a.klen[i];
            n++;
        }
    }
    if (n == 0)
        return EFS_OK;
    return efs_kv_batch(kv, it, n);
}

int efs_lock_count(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    struct scan_acc a;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    memset(&a, 0, sizeof(a));
    rc = efs_kv_key_lock_file_prefix(efs_kv_inode_shard(ino), ino, gen, pref,
                                     &plen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan_prefix(kv, pref, plen, scan_cb, &a);
    if (rc != EFS_OK && rc != 1)
        return rc;
    return a.n;
}

struct drop_acc {
    const uint8_t *uuid;
    uint32_t epoch;
    uint8_t keys[DROP_MAX][EFS_KV_KEY_MAX];
    uint32_t klen[DROP_MAX];
    int n;
    int full;
};

static int drop_cb(void *user, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct drop_acc *a = user;
    uint32_t ep;

    (void)val;
    (void)vlen;
    if (klen < LOCK_KLEN)
        return 0;
    if (memcmp(key + 45, a->uuid, EFS_OPID_UUID_LEN) != 0)
        return 0;
    ep = rd32(key + 45 + EFS_OPID_UUID_LEN);
    if (ep != a->epoch)
        return 0;
    if (a->n >= DROP_MAX) {
        a->full = 1;
        return 1;
    }
    memcpy(a->keys[a->n], key, klen);
    a->klen[a->n] = klen;
    a->n++;
    return 0;
}

int efs_lock_drop_session(struct efs_kv *kv, uint32_t shard,
                          const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint32_t epoch)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    if (!kv || !uuid)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lock_shard_prefix(shard, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    for (;;) {
        struct drop_acc acc;
        struct efs_kv_item it[DROP_MAX];
        int i;

        memset(&acc, 0, sizeof(acc));
        acc.uuid = uuid;
        acc.epoch = epoch;
        rc = efs_kv_scan_prefix(kv, pref, plen, drop_cb, &acc);
        if (rc != EFS_OK && rc != 1)
            return rc;
        if (acc.n == 0)
            return EFS_OK;
        memset(it, 0, sizeof(it));
        for (i = 0; i < acc.n; i++) {
            it[i].op = EFS_KV_DEL;
            it[i].key = acc.keys[i];
            it[i].klen = acc.klen[i];
        }
        rc = efs_kv_batch(kv, it, (uint32_t)acc.n);
        if (rc != EFS_OK)
            return rc;
        if (!acc.full)
            return EFS_OK;
    }
}
