/* Pure session + open-lease SM. One KV, no I/O. */
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/opid.h"
#include <string.h>

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void bit_set(uint8_t *map, uint32_t shard)
{
    map[shard / 8u] |= (uint8_t)(1u << (shard % 8u));
}

int efs_session_bit_get(const uint8_t *map, uint32_t shard)
{
    if (!map || shard >= EFS_SESSION_BITS)
        return 0;
    return (map[shard / 8u] & (uint8_t)(1u << (shard % 8u))) != 0;
}

static int bits_covered(const uint8_t *need, const uint8_t *have)
{
    uint32_t i;

    for (i = 0; i < EFS_SESSION_BITMAP; i++) {
        if ((need[i] & have[i]) != need[i])
            return 0;
    }
    return 1;
}

static void pack_rec(uint8_t *p, const struct efs_session_rec *r)
{
    be32(p, r->epoch);
    be32(p + 4, r->state);
    memcpy(p + 8, r->touched, EFS_SESSION_BITMAP);
    memcpy(p + 8 + EFS_SESSION_BITMAP, r->acked, EFS_SESSION_BITMAP);
}

static int unpack_rec(const uint8_t *p, uint32_t n, struct efs_session_rec *r)
{
    if (!p || !r || n < EFS_SESSION_VAL)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->epoch = rd32(p);
    r->state = (uint8_t)rd32(p + 4);
    memcpy(r->touched, p + 8, EFS_SESSION_BITMAP);
    memcpy(r->acked, p + 8 + EFS_SESSION_BITMAP, EFS_SESSION_BITMAP);
    return EFS_OK;
}

static int sess_key(const uint8_t *uuid, uint8_t *key, uint32_t *klen)
{
    return efs_kv_key_session(efs_kv_session_shard(uuid), uuid, key, klen);
}

static int load_rec(struct efs_kv *kv, const uint8_t *uuid,
                    struct efs_session_rec *r)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_SESSION_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = sess_key(uuid, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_kv_get(kv, key, klen, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    return unpack_rec(val, vlen, r);
}

static int store_rec(struct efs_kv *kv, const uint8_t *uuid,
                     const struct efs_session_rec *r)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_SESSION_VAL];
    uint32_t klen = 0;
    int rc;

    rc = sess_key(uuid, key, &klen);
    if (rc != EFS_OK)
        return rc;
    pack_rec(val, r);
    return efs_kv_put(kv, key, klen, val, EFS_SESSION_VAL);
}

static int load_local(struct efs_kv *kv, uint32_t shard, const uint8_t *uuid,
                      struct efs_sess_local *loc)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_SESS_LOCAL_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = efs_kv_key_sess_local(shard, uuid, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_kv_get(kv, key, klen, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    if (vlen < EFS_SESS_LOCAL_VAL)
        return EFS_ERR_PROTO;
    loc->established = rd32(val);
    loc->reject_below = rd32(val + 4);
    return EFS_OK;
}

static int store_local(struct efs_kv *kv, uint32_t shard, const uint8_t *uuid,
                       const struct efs_sess_local *loc)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_SESS_LOCAL_VAL];
    uint32_t klen = 0;
    int rc;

    rc = efs_kv_key_sess_local(shard, uuid, key, &klen);
    if (rc != EFS_OK)
        return rc;
    be32(val, loc->established);
    be32(val + 4, loc->reject_below);
    return efs_kv_put(kv, key, klen, val, EFS_SESS_LOCAL_VAL);
}

int efs_session_create(struct efs_kv *kv, const uint8_t uuid[EFS_OPID_UUID_LEN],
                       uint32_t epoch)
{
    struct efs_session_rec r;
    int rc;

    if (!kv || !uuid)
        return EFS_ERR_INVAL;
    rc = load_rec(kv, uuid, &r);
    if (rc == EFS_OK)
        return EFS_OK;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    memset(&r, 0, sizeof(r));
    r.epoch = epoch ? epoch : 1;
    r.state = EFS_SESSION_ACTIVE;
    return store_rec(kv, uuid, &r);
}

int efs_session_get(struct efs_kv *kv, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    struct efs_session_rec *out)
{
    if (!kv || !uuid || !out)
        return EFS_ERR_INVAL;
    return load_rec(kv, uuid, out);
}

int efs_session_register(struct efs_kv *kv, const uint8_t uuid[EFS_OPID_UUID_LEN],
                         uint32_t epoch, uint32_t shard)
{
    struct efs_session_rec r;
    int rc;

    if (!kv || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    rc = load_rec(kv, uuid, &r);
    if (rc != EFS_OK)
        return rc;
    if (r.state == EFS_SESSION_FENCING) {
        uint32_t old = r.epoch > 0 ? r.epoch - 1 : 0;

        if (epoch == r.epoch)
            return EFS_ERR_BUSY; /* new session waits on the barrier */
        if (epoch == old && efs_session_bit_get(r.touched, shard))
            return EFS_OK; /* already frozen in the set */
        return EFS_ERR_STALE; /* no new registrations for E */
    }
    if (r.state != EFS_SESSION_ACTIVE)
        return EFS_ERR_INVAL;
    if (epoch != r.epoch)
        return EFS_ERR_STALE;
    if (efs_session_bit_get(r.touched, shard))
        return EFS_OK;
    bit_set(r.touched, shard);
    return store_rec(kv, uuid, &r);
}

int efs_session_begin_fence(struct efs_kv *kv,
                            const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    struct efs_session_rec r;
    int rc;

    if (!kv || !uuid)
        return EFS_ERR_INVAL;
    rc = load_rec(kv, uuid, &r);
    if (rc != EFS_OK)
        return rc;
    if (r.state == EFS_SESSION_FENCING)
        return EFS_OK;
    if (r.state != EFS_SESSION_ACTIVE)
        return EFS_ERR_INVAL;
    r.epoch++;
    r.state = EFS_SESSION_FENCING;
    memset(r.acked, 0, sizeof(r.acked));
    return store_rec(kv, uuid, &r);
}

int efs_session_ack_fence(struct efs_kv *kv,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t shard)
{
    struct efs_session_rec r;
    int rc;

    if (!kv || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    rc = load_rec(kv, uuid, &r);
    if (rc != EFS_OK)
        return rc;
    if (r.state != EFS_SESSION_FENCING)
        return EFS_ERR_INVAL;
    if (!efs_session_bit_get(r.touched, shard))
        return EFS_OK;
    if (efs_session_bit_get(r.acked, shard))
        return EFS_OK;
    bit_set(r.acked, shard);
    return store_rec(kv, uuid, &r);
}

int efs_session_finish_fence(struct efs_kv *kv,
                             const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    struct efs_session_rec r;
    uint8_t okey[EFS_KV_KEY_MAX];
    uint32_t olen = 0, old_epoch;
    int rc;

    if (!kv || !uuid)
        return EFS_ERR_INVAL;
    rc = load_rec(kv, uuid, &r);
    if (rc != EFS_OK)
        return rc;
    if (r.state == EFS_SESSION_ACTIVE)
        return EFS_OK;
    if (r.state != EFS_SESSION_FENCING)
        return EFS_ERR_INVAL;
    if (!bits_covered(r.touched, r.acked))
        return EFS_ERR_BUSY;
    old_epoch = r.epoch > 0 ? r.epoch - 1 : 0;
    r.state = EFS_SESSION_ACTIVE;
    memset(r.acked, 0, sizeof(r.acked));
    rc = store_rec(kv, uuid, &r);
    if (rc != EFS_OK)
        return rc;
    /* Fenced epoch's dedup window is dropped wholesale (I16 / §7.5). */
    if (old_epoch) {
        rc = efs_kv_key_opid(efs_kv_session_shard(uuid), uuid, old_epoch,
                             okey, &olen);
        if (rc == EFS_OK) {
            rc = efs_kv_del(kv, okey, olen);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
        }
    }
    return rc;
}

int efs_session_barrier_done(struct efs_kv *kv,
                             const uint8_t uuid[EFS_OPID_UUID_LEN],
                             uint32_t old_epoch)
{
    struct efs_session_rec r;
    int rc;

    if (!kv || !uuid)
        return EFS_ERR_INVAL;
    rc = load_rec(kv, uuid, &r);
    if (rc != EFS_OK)
        return rc;
    if (r.state != EFS_SESSION_ACTIVE || r.epoch <= old_epoch)
        return EFS_ERR_BUSY;
    return EFS_OK;
}

int efs_session_reclaimable(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t old_epoch)
{
    if (!kv || !uuid || shard >= EFS_SESSION_BITS || !old_epoch)
        return EFS_ERR_INVAL;
    int rc = efs_session_barrier_done(kv, uuid, old_epoch);
    if (rc != EFS_OK) return rc;
    struct efs_sess_local loc;
    rc = load_local(kv, shard, uuid, &loc);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_BUSY;
    if (rc != EFS_OK) return rc;
    return loc.reject_below > old_epoch ? EFS_OK : EFS_ERR_BUSY;
}

int efs_session_establish(struct efs_kv *kv, uint32_t shard,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    struct efs_sess_local loc;
    int rc;

    if (!kv || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    memset(&loc, 0, sizeof(loc));
    rc = load_local(kv, shard, uuid, &loc);
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (epoch < loc.reject_below)
        return EFS_ERR_STALE;
    loc.established = epoch;
    return store_local(kv, shard, uuid, &loc);
}

int efs_session_fence_local(struct efs_kv *kv, uint32_t shard,
                            const uint8_t uuid[EFS_OPID_UUID_LEN],
                            uint32_t fence_epoch)
{
    struct efs_sess_local loc;
    int rc;

    if (!kv || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    memset(&loc, 0, sizeof(loc));
    rc = load_local(kv, shard, uuid, &loc);
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (fence_epoch > loc.reject_below)
        loc.reject_below = fence_epoch;
    rc = store_local(kv, shard, uuid, &loc);
    if (rc != EFS_OK)
        return rc;
    /* This shard's dir-op window for the epoch just fenced. Windows live
     * on the dentry shard (§7.9), so the drop has to happen here: finish
     * runs on the session shard and cannot see the other group's keys.
     * Old-epoch requests are rejected by reject_below from this point, so
     * the recorded verdict is no longer the answer. A shard the session
     * never registered is not visited — its window stays until it is. */
    if (fence_epoch > 0) {
        uint8_t okey[EFS_KV_KEY_MAX];
        uint32_t olen = 0;

        rc = efs_kv_key_opid(shard, uuid, fence_epoch - 1, okey, &olen);
        if (rc == EFS_OK) {
            rc = efs_kv_del(kv, okey, olen);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
        }
    }
    return rc;
}

int efs_session_accept(struct efs_kv *kv, uint32_t shard,
                       const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    struct efs_sess_local loc;
    int rc;

    if (!kv || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    rc = load_local(kv, shard, uuid, &loc);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_BUSY;
    if (rc != EFS_OK)
        return rc;
    if (epoch < loc.reject_below)
        return EFS_ERR_STALE;
    if (loc.established != epoch)
        return EFS_ERR_BUSY;
    return EFS_OK;
}

int efs_lease_open(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                   const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], one = 1;
    uint32_t klen = 0, shard;
    int rc;

    if (!kv || !uuid || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.generation != gen)
        return EFS_ERR_STALE;
    shard = efs_kv_inode_shard(ino);
    rc = efs_kv_key_lease(shard, ino, gen, uuid, epoch, key, &klen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_put(kv, key, klen, &one, 1);
}

int efs_lease_close(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0;
    int rc;

    if (!kv || !uuid || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lease(efs_kv_inode_shard(ino), ino, gen, uuid, epoch, key,
                          &klen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_del(kv, key, klen);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    return rc;
}

static int any_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    (void)user;
    (void)key;
    (void)klen;
    (void)val;
    (void)vlen;
    return 1;
}

int efs_lease_any(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lease_prefix(efs_kv_inode_shard(ino), ino, gen, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan_prefix(kv, pref, plen, any_cb, NULL);
    if (rc == 1)
        return 1;
    return rc;
}

#define DROP_MAX 32

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
    const uint8_t *u;
    uint32_t ep;

    (void)val;
    (void)vlen;
    /* shard(2)+kind(1)+ino(8)+gen(8)+uuid(16)+epoch(4) */
    if (klen < 3 + 8 + 8 + EFS_OPID_UUID_LEN + 4)
        return 0;
    u = key + 3 + 8 + 8;
    ep = ((uint32_t)u[EFS_OPID_UUID_LEN] << 24) |
         ((uint32_t)u[EFS_OPID_UUID_LEN + 1] << 16) |
         ((uint32_t)u[EFS_OPID_UUID_LEN + 2] << 8) |
         (uint32_t)u[EFS_OPID_UUID_LEN + 3];
    if (memcmp(u, a->uuid, EFS_OPID_UUID_LEN) != 0 || ep != a->epoch)
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

int efs_lease_drop_session(struct efs_kv *kv, uint32_t shard,
                           const uint8_t uuid[EFS_OPID_UUID_LEN],
                           uint32_t epoch)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    if (!kv || !uuid || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lease_shard_prefix(shard, pref, &plen);
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
