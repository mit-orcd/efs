/* Pure session + open-lease SM. One KV, no I/O. */
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/opid.h"
#include "efs/checksum.h"
#include "efs/txn.h"
#include "efs/lock.h"
#include <string.h>
#include <sys/stat.h>

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
    /* A prepared owner may still publish its pending lease. Do not advance
     * the rejection floor/established epoch past it and later let RESOLVE
     * overwrite the fence with its older snapshot. The host must drain it. */
    rc = efs_txn_key_exclusive(kv, key, klen);
    if(rc==EFS_OK)rc=efs_txn_key_guarded(kv,key,klen);
    if (rc != EFS_OK) return rc;
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

int efs_session_register_establish(struct efs_kv *kv,
    const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,uint32_t shard)
{
    if(!kv || !uuid || !epoch || shard>=EFS_SESSION_BITS)return EFS_ERR_INVAL;
    struct efs_session_rec r;struct efs_sess_local loc={0};
    int rc=load_rec(kv,uuid,&r);
    if(rc)return rc;
    /* Unlike REGISTER's old-epoch idempotence during fencing, admitting a
     * previously absent floor always requires current ACTIVE authority. */
    if(r.state==EFS_SESSION_FENCING)return epoch==r.epoch?EFS_ERR_BUSY:EFS_ERR_STALE;
    if(r.state!=EFS_SESSION_ACTIVE)return EFS_ERR_PROTO;
    if(epoch!=r.epoch)return EFS_ERR_STALE;
    rc=load_local(kv,shard,uuid,&loc);
    if(rc!=EFS_OK && rc!=EFS_ERR_NOT_FOUND)return rc;
    if(epoch<loc.reject_below || epoch<loc.established)return EFS_ERR_STALE;
    uint8_t keys[2][EFS_KV_KEY_MAX],global[EFS_SESSION_VAL],local[EFS_SESS_LOCAL_VAL];
    uint32_t lengths[2];
    rc=sess_key(uuid,keys[0],&lengths[0]);
    if(!rc)rc=efs_kv_key_sess_local(shard,uuid,keys[1],&lengths[1]);
    for(unsigned i=0;i<2 && !rc;i++) {
        rc=efs_txn_key_exclusive(kv,keys[i],lengths[i]);
        if(!rc)rc=efs_txn_key_guarded(kv,keys[i],lengths[i]);
    }
    if(rc)return rc;
    bit_set(r.touched,shard);pack_rec(global,&r);
    be32(local,epoch);be32(local+4,loc.reject_below);
    struct efs_kv_item items[2]={
        {EFS_KV_PUT,keys[0],lengths[0],global,sizeof(global)},
        {EFS_KV_PUT,keys[1],lengths[1],local,sizeof(local)}
    };
    return efs_kv_batch(kv,items,2);
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

static int finish_fence(struct efs_kv *kv,
                         const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t expected_epoch)
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
    if (expected_epoch && r.epoch!=expected_epoch)return EFS_ERR_STALE;
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

int efs_session_finish_fence(struct efs_kv *kv,const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    return finish_fence(kv,uuid,0); /* legacy persisted commands */
}
int efs_session_finish_fence_epoch(struct efs_kv *kv,const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch)
{
    return epoch<2?EFS_ERR_INVAL:finish_fence(kv,uuid,epoch);
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

    if (!kv || !uuid || !fence_epoch || shard >= EFS_SESSION_BITS)
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
        if (rc == EFS_OK) {
            /* The durable local floor supersedes both replay windows.
             * Ownership still waits for the completed global barrier/drop;
             * no session-owner row is assumed present on this inode lane. */
            okey[2] = EFS_KV_KIND_LEASE_EDGE;
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

int efs_session_accept_legacy(struct efs_kv *kv,uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch)
{
    if(!kv || !uuid || shard>=EFS_SESSION_BITS)return EFS_ERR_INVAL;
    struct efs_sess_local local;
    int rc=load_local(kv,shard,uuid,&local);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    if(rc!=EFS_OK)return rc;
    if(epoch<local.reject_below)return EFS_ERR_STALE;
    return local.established==epoch?EFS_OK:EFS_ERR_BUSY;
}

int efs_session_prepare_namespace(struct efs_kv *kv,uint32_t shard,
    const struct efs_opid *id,const struct efs_txid *t,const struct efs_txn_parts *parts)
{
    if(!kv || !id || !t || !parts || !parts->n || parts->n>EFS_TXN_MAX_PART ||
       shard>=EFS_SESSION_BITS)return EFS_ERR_INVAL;
    unsigned member=0;for(unsigned i=0;i<parts->n;i++)member+=parts->shard[i]==shard;
    if(member!=1)return EFS_ERR_INVAL;
    struct efs_sess_local local;int rc=load_local(kv,shard,id->client_uuid,&local);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK; /* historical unregistered log */
    if(rc!=EFS_OK)return rc;
    if(id->session_epoch<local.reject_below)return EFS_ERR_STALE;
    if(local.established!=id->session_epoch)return EFS_ERR_BUSY;
    uint8_t key[EFS_KV_KEY_MAX];uint32_t bytes=0;uint64_t version=0;
    rc=efs_kv_key_sess_local(shard,id->client_uuid,key,&bytes);
    if(rc==EFS_OK)rc=efs_txn_ver_get(kv,key,bytes,&version);
    if(rc==EFS_OK)rc=efs_txn_prepare_guard(kv,t,parts,key,bytes,version);
    return rc;
}

int efs_session_prepare_owner(struct efs_kv *kv, uint32_t shard,
                              const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
                              const struct efs_txid *txid, const struct efs_txn_parts *parts)
{
    if (!kv || !uuid || !epoch || !txid || !parts || !parts->n || parts->n > EFS_TXN_MAX_PART ||
        shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    unsigned found = 0;
    for (unsigned i = 0; i < parts->n; i++)
        if (parts->shard[i] == shard)
            found++;
    if (found != 1)
        return EFS_ERR_INVAL;
    int rc = efs_session_accept(kv, shard, uuid, epoch);
    if (rc != EFS_OK)
        return rc;
    struct efs_sess_local local;
    rc = load_local(kv, shard, uuid, &local);
    if (rc != EFS_OK)
        return rc;
    uint8_t key[EFS_KV_KEY_MAX], value[EFS_SESS_LOCAL_VAL];
    uint32_t len = 0;
    rc = efs_kv_key_sess_local(shard, uuid, key, &len);
    if (rc != EFS_OK)
        return rc;
    be32(value, local.established);
    be32(value + 4, local.reject_below);
    return efs_txn_prepare_excl_value(kv, txid, parts, key, len, value, sizeof(value), EFS_TXN_PUT,
                                      value, sizeof(value));
}

/* Legacy version 2: normal-open bit + pending-CREATE bit + exact token. There are
 * no closed tombstones and no extra claim keys; bounded retirement of the
 * surrounding session/operation identities is an activation prerequisite. */
#define LEASE_NORMAL 1u
#define LEASE_PENDING 2u
int efs_lease_pending_value(uint64_t token, uint8_t out[EFS_LEASE_PENDING_VALUE_LEN])
{
    if(!token || !out)return EFS_ERR_INVAL;
    out[0]=2;out[1]=LEASE_PENDING;
    for(unsigned i=0;i<8;i++)out[2+i]=(uint8_t)(token>>(56-8*i));
    return EFS_OK;
}
/* A legacy caller may observe an existing sequenced normal owner, but must
 * never change its sequence/tombstone or release it without an exact edge. */
#define LEASE_READ_VALUE_LEN 18u
static int lease_value_read(struct efs_kv *kv,const uint8_t *key,uint32_t klen,
                             uint8_t value[LEASE_READ_VALUE_LEN],
                             uint8_t *flags,uint64_t *token,int *sequenced)
{
    uint32_t n=LEASE_READ_VALUE_LEN;*flags=0;*token=0;*sequenced=0;
    int rc=efs_kv_get(kv,key,klen,value,&n);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    if((rc==EFS_ERR_INVAL || rc==EFS_ERR_NOMEM) && n>LEASE_READ_VALUE_LEN)return EFS_ERR_PROTO;
    if(rc!=EFS_OK)return rc;
    if(n==1 && value[0]==1){*flags=LEASE_NORMAL;return EFS_OK;}
    if(n<2 || (value[1]&~(LEASE_NORMAL|LEASE_PENDING)))return EFS_ERR_PROTO;
    unsigned offset;
    if(n==EFS_LEASE_PENDING_VALUE_LEN && value[0]==2 &&
       (value[1]&LEASE_PENDING))offset=2;
    else if(n==LEASE_READ_VALUE_LEN && value[0]==3) {
        uint64_t seq=0;
        for(unsigned i=0;i<8;i++)seq=(seq<<8)|value[2+i];
        if(!seq)return EFS_ERR_PROTO;
        *sequenced=1;offset=10;
    } else return EFS_ERR_PROTO;
    for(unsigned i=0;i<8;i++)*token=(*token<<8)|value[offset+i];
    if(!!(value[1]&LEASE_PENDING)!=!!*token)return EFS_ERR_PROTO;
    *flags=value[1];return EFS_OK;
}
int efs_lease_pending_close(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                            const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,
                            uint64_t token,int release_normal)
{
    uint8_t key[EFS_KV_KEY_MAX],value[LEASE_READ_VALUE_LEN],flags;
    uint32_t klen=0;uint64_t current;int sequenced;
    if(!kv || !uuid || !ino || !token || (release_normal!=0 && release_normal!=1))return EFS_ERR_INVAL;
    int rc=efs_kv_key_lease(efs_kv_inode_shard(ino),ino,gen,uuid,epoch,key,&klen);
    if(rc==EFS_OK)rc=lease_value_read(kv,key,klen,value,&flags,&current,&sequenced);
    if(rc!=EFS_OK)return rc;
    if(sequenced)return EFS_ERR_STALE; /* exact sequenced cleanup required */
    if(!flags)return EFS_OK; /* repeated completed cleanup */
    if(!(flags&LEASE_PENDING))return release_normal?EFS_ERR_STALE:EFS_OK;
    if(current!=token)return EFS_ERR_STALE;
    if(!release_normal && (flags&LEASE_NORMAL)) {
        uint8_t one=1;return efs_kv_put(kv,key,klen,&one,1);
    }
    rc=efs_kv_del(kv,key,klen);
    return rc==EFS_ERR_NOT_FOUND?EFS_OK:rc;
}

/* Every OPEN protocol must respect the same prepared close and inode guards.
 * Legacy peers cannot bypass a cleanup transaction merely by omitting identity. */
static int lease_open_guard(struct efs_kv *kv,efs_ino_t ino)
{
    uint8_t key[EFS_KV_KEY_MAX];uint32_t len;
    int rc=efs_kv_key_inode(efs_kv_inode_shard(ino),ino,key,&len);
    if(rc==EFS_OK)rc=efs_txn_key_exclusive(kv,key,len);
    if(rc!=EFS_OK)return rc;
    rc=efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),ino,key,&len);
    if(rc==EFS_OK) {key[2]=EFS_KV_KIND_CLOSE_STEP;rc=efs_txn_key_exclusive(kv,key,len);}
    return rc;
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
    rc=efs_session_accept_legacy(kv,shard,uuid,epoch);
    if(rc!=EFS_OK)return rc;
    rc=lease_open_guard(kv,ino);
    if(rc!=EFS_OK)return rc;
    rc = efs_kv_key_lease(shard, ino, gen, uuid, epoch, key, &klen);
    if (rc != EFS_OK)
        return rc;
    rc=efs_txn_key_exclusive(kv,key,klen);
    if(rc!=EFS_OK)return rc;
    uint8_t value[LEASE_READ_VALUE_LEN],flags;uint64_t token;int sequenced;
    rc=lease_value_read(kv,key,klen,value,&flags,&token,&sequenced);
    if(rc!=EFS_OK)return rc;
    if(flags&LEASE_NORMAL)return EFS_OK; /* observation, no ownership mutation */
    if(sequenced)return EFS_ERR_STALE;
    if(flags&LEASE_PENDING) {
        value[1]=flags|LEASE_NORMAL;
        return efs_kv_put(kv,key,klen,value,EFS_LEASE_PENDING_VALUE_LEN);
    }
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
    rc=efs_txn_key_exclusive(kv,key,klen);
    if(rc!=EFS_OK)return rc;
    uint8_t value[LEASE_READ_VALUE_LEN],flags;uint64_t token;int sequenced;
    rc=lease_value_read(kv,key,klen,value,&flags,&token,&sequenced);
    if(rc!=EFS_OK)return rc;
    if(sequenced)return EFS_ERR_STALE;
    if(flags&LEASE_PENDING) {
        if(!(flags&LEASE_NORMAL))return EFS_OK;
        value[1]=LEASE_PENDING;
        return efs_kv_put(kv,key,klen,value,EFS_LEASE_PENDING_VALUE_LEN);
    }
    if(!flags)return EFS_OK;
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
    /* A validated guarded closed record is a replay tombstone, not ownership.
     * Malformed/unknown values conservatively protect the inode. */
    if (vlen == 18 && val[0] == 3 && val[1] == 0) {
        uint64_t seq = 0, token = 0;
        for (unsigned i = 0; i < 8; i++) {
            seq = (seq << 8) | val[2+i];
            token = (token << 8) | val[10+i];
        }
        if (seq && !token) return 0;
    }
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

/* Exact-key cleanup never selects again during replay. The local rejection
 * floor cannot decrease, so an old owner cannot recreate a deleted key. */
int efs_session_cleanup_floor(struct efs_kv *kv, uint32_t shard,
                         const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    struct efs_sess_local local;
    if (!kv || !uuid || !epoch || shard >= EFS_SESSION_BITS)
        return EFS_ERR_INVAL;
    int rc=load_local(kv,shard,uuid,&local);
    if (rc==EFS_ERR_NOT_FOUND) return EFS_ERR_BUSY;
    if (rc!=EFS_OK) return rc;
    return local.reject_below>epoch ? EFS_OK : EFS_ERR_BUSY;
}

static uint64_t cleanup_rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p)<<32 | rd32(p+4);
}

static int cleanup_key(const uint8_t *key, uint32_t len, uint32_t shard,
                        const uint8_t *uuid, uint32_t epoch)
{
    uint32_t owner;
    if (len==39 && key[2]==EFS_KV_KIND_LEASE) owner=19;
    else if (len==65 && key[2]==EFS_KV_KIND_LOCK) owner=45;
    else return EFS_ERR_INVAL;
    if (((uint32_t)key[0]<<8 | key[1])!=shard ||
        memcmp(key+owner,uuid,EFS_OPID_UUID_LEN) || rd32(key+owner+16)!=epoch ||
        !cleanup_rd64(key+3) || !cleanup_rd64(key+11) ||
        efs_kv_inode_shard(cleanup_rd64(key+3))!=shard)
        return EFS_ERR_INVAL;
    return EFS_OK;
}

struct cleanup_scan {
    uint32_t shard,epoch,n;
    int rc;
    const uint8_t *uuid;
    struct efs_session_cleanup_key *keys;
};
static int cleanup_cb(void *user,const uint8_t *key,uint32_t len,
                       const uint8_t *val,uint32_t vlen)
{
    struct cleanup_scan *scan=user;
    (void)val;(void)vlen;
    uint32_t expected=key[2]==EFS_KV_KIND_LEASE ? 39 : 65;
    if (len!=expected) {scan->rc=EFS_ERR_PROTO;return 1;}
    uint32_t owner=expected==39 ? 19 : 45;
    if (memcmp(key+owner,scan->uuid,16) || rd32(key+owner+16)!=scan->epoch) return 0;
    if (cleanup_key(key,len,scan->shard,scan->uuid,scan->epoch)!=EFS_OK) {
        scan->rc=EFS_ERR_PROTO;return 1;
    }
    scan->keys[scan->n].len=(uint8_t)len;
    memcpy(scan->keys[scan->n++].key,key,len);
    return scan->n==EFS_SESSION_CLEANUP_MAX;
}

int efs_session_cleanup_select(struct efs_kv *kv,uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,
    struct efs_session_cleanup_key keys[EFS_SESSION_CLEANUP_MAX],uint32_t *n)
{
    if (!keys || !n) return EFS_ERR_INVAL;
    *n=0;
    int rc=efs_session_cleanup_floor(kv,shard,uuid,epoch);
    if (rc!=EFS_OK) return rc;
    struct cleanup_scan scan={.shard=shard,.epoch=epoch,.uuid=uuid,.keys=keys};
    uint8_t prefix[EFS_KV_KEY_MAX];uint32_t len;
    rc=efs_kv_key_lease_shard_prefix(shard,prefix,&len);
    if (rc==EFS_OK) rc=efs_kv_scan_prefix(kv,prefix,len,cleanup_cb,&scan);
    if (rc!=EFS_OK && rc!=1) return rc;
    if (scan.rc!=EFS_OK) return scan.rc;
    if (scan.n<EFS_SESSION_CLEANUP_MAX) {
        rc=efs_kv_key_lock_shard_prefix(shard,prefix,&len);
        if (rc==EFS_OK) rc=efs_kv_scan_prefix(kv,prefix,len,cleanup_cb,&scan);
        if (rc!=EFS_OK && rc!=1) return rc;
    }
    if (scan.rc!=EFS_OK) return scan.rc;
    *n=scan.n;
    return EFS_OK;
}

int efs_session_cleanup_apply(struct efs_kv *kv,uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,
    const struct efs_session_cleanup_key *keys,uint32_t n)
{
    if (!keys || !n || n>EFS_SESSION_CLEANUP_MAX) return EFS_ERR_INVAL;
    int rc=efs_session_cleanup_floor(kv,shard,uuid,epoch);
    if (rc!=EFS_OK) return rc;
    /* Validate ALL keys before any effects, including duplicate/foreign keys. */
    for (uint32_t i=0;i<n;i++) {
        rc=cleanup_key(keys[i].key,keys[i].len,shard,uuid,epoch);
        if (rc!=EFS_OK) return rc;
        for (uint32_t j=0;j<i;j++)
            if (keys[j].len==keys[i].len && !memcmp(keys[j].key,keys[i].key,keys[i].len))
                return EFS_ERR_INVAL;
    }
    struct efs_kv_item items[2*EFS_SESSION_CLEANUP_MAX];
    uint8_t markers[EFS_SESSION_CLEANUP_MAX][EFS_KV_KEY_MAX];
    uint8_t identities[EFS_SESSION_CLEANUP_MAX][EFS_CLOSE_STEP_VALUE];
    uint32_t count=0,marked=0;
    for (uint32_t i=0;i<n;i++) {
        const uint8_t *key=keys[i].key;
        rc=efs_txn_key_exclusive(kv,key,keys[i].len);
        if (rc!=EFS_OK) return rc;
        items[count++]=(struct efs_kv_item){EFS_KV_DEL,key,keys[i].len,NULL,0};
        if (key[2]!=EFS_KV_KIND_LEASE) continue;
        /* Exact replay of a deleted key must not recreate or replace a newer
         * close obligation. Only an actually retained lease creates one. */
        uint8_t old[18];uint32_t old_len=sizeof(old);
        rc=efs_kv_get(kv,key,keys[i].len,old,&old_len);
        if(rc==EFS_ERR_NOT_FOUND)continue;
        if(rc!=EFS_OK)return rc;
        efs_ino_t ino=cleanup_rd64(key+3);uint64_t gen=cleanup_rd64(key+11);
        struct efs_meta_row row;
        rc=efs_meta_apply_get_inode(kv,ino,&row);
        if (rc==EFS_ERR_NOT_FOUND || (rc==EFS_OK && row.generation!=gen)) continue;
        if (rc!=EFS_OK) return rc;
        uint32_t len=0;
        rc=efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,markers[marked],&len);
        if (rc!=EFS_OK) return rc;
        markers[marked][2]=EFS_KV_KIND_CLOSE_STEP;
        int duplicate=0;
        for (uint32_t j=0;j<marked;j++)
            if (!memcmp(markers[j],markers[marked],len)) duplicate=1;
        if (duplicate) continue;
        rc=efs_txn_key_exclusive(kv,markers[marked],len);
        if (rc!=EFS_OK) return rc;
        memcpy(identities[marked],key+11,8);memcpy(identities[marked]+8,uuid,16);
        be32(identities[marked]+24,epoch);
        for(unsigned b=0;b<8;b++)identities[marked][28+b]=(uint8_t)(EFS_CLOSE_SESSION_SEQ>>(56-8*b));
        items[count++]=(struct efs_kv_item){EFS_KV_PUT,markers[marked],len,identities[marked],EFS_CLOSE_STEP_VALUE};
        marked++;
    }
    return efs_kv_batch(kv,items,count);
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

/* Inactive W94 edge protocol. A separate bounded window is necessary: ordinary
 * namespace acknowledgements must not drop a CLOSE receipt before its lease
 * tombstone is retired. The client ack is contiguous across all its requests;
 * it is never inferred from time or from the largest completed request. */
#define EDGE_SLOTS EFS_OPID_REPLY_CACHE
#define EDGE_VALUE 18u
#define EDGE_WINDOW_MAX (13u + EDGE_SLOTS * 60u)
struct edge_reply {
    uint64_t seq, ino, gen;
    int rc;
    uint8_t digest[32];
};
struct edge_window {
    uint64_t floor;
    unsigned count;
    struct edge_reply replies[EDGE_SLOTS];
};
struct edge_lease {
    uint8_t flags;
    uint64_t seq, token;
};
static uint64_t edge_rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p+4);
}
static void edge_be64(uint8_t *p, uint64_t n)
{
    be32(p, (uint32_t)(n >> 32)); be32(p+4, (uint32_t)n);
}
static int edge_window_key(uint32_t shard, const uint8_t *uuid, uint32_t epoch,
                           uint8_t *key, uint32_t *len)
{
    int rc = efs_kv_key_opid(shard, uuid, epoch, key, len);
    if (rc == EFS_OK) key[2] = EFS_KV_KIND_LEASE_EDGE;
    return rc;
}
static int edge_window_load(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                            struct edge_window *w)
{
    uint8_t value[EDGE_WINDOW_MAX]; uint32_t len = sizeof(value);
    memset(w, 0, sizeof(*w));
    int rc = efs_kv_get(kv, key, klen, value, &len);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc != EFS_OK) return rc;
    if (len < 13 || value[0] != 1) return EFS_ERR_PROTO;
    w->floor = edge_rd64(value+1); w->count = rd32(value+9);
    if (w->count > EDGE_SLOTS || len != 13u + w->count*60u) return EFS_ERR_PROTO;
    for (unsigned i = 0; i < w->count; i++) {
        const uint8_t *p = value+13+i*60; struct edge_reply *r = &w->replies[i];
        r->seq=edge_rd64(p); r->rc=(int)rd32(p+8);
        r->ino=edge_rd64(p+12); r->gen=edge_rd64(p+20);
        memcpy(r->digest,p+28,32);
        if (r->seq <= w->floor || r->seq-w->floor > EFS_OPID_BITMAP_BITS ||
            !r->ino || !r->gen || (r->rc!=EFS_OK && r->rc!=EFS_ERR_STALE &&
                                   r->rc!=EFS_ERR_NOT_FOUND && r->rc!=EFS_ERR_INVAL)) return EFS_ERR_PROTO;
        for (unsigned j=0;j<i;j++) if(w->replies[j].seq==r->seq) return EFS_ERR_PROTO;
    }
    return EFS_OK;
}
static uint32_t edge_window_pack(const struct edge_window *w, uint8_t *value)
{
    value[0]=1; edge_be64(value+1,w->floor); be32(value+9,w->count);
    for(unsigned i=0;i<w->count;i++) {
        uint8_t *p=value+13+i*60; const struct edge_reply *r=&w->replies[i];
        edge_be64(p,r->seq); be32(p+8,(uint32_t)r->rc);
        edge_be64(p+12,r->ino); edge_be64(p+20,r->gen); memcpy(p+28,r->digest,32);
    }
    return 13+w->count*60;
}
static int edge_lease_load(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                           struct edge_lease *lease)
{
    uint8_t v[EDGE_VALUE]; uint32_t n=sizeof(v);
    memset(lease,0,sizeof(*lease));
    int rc=efs_kv_get(kv,key,klen,v,&n);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    if(rc!=EFS_OK)return rc;
    if(n==1 && v[0]==1) {lease->flags=LEASE_NORMAL;return EFS_OK;}
    if(n==EFS_LEASE_PENDING_VALUE_LEN && v[0]==2) {
        lease->flags=v[1];lease->token=edge_rd64(v+2);
        if(!(lease->flags&LEASE_PENDING) || !lease->token)return EFS_ERR_PROTO;
    } else if(n==EDGE_VALUE && v[0]==3) {
        lease->flags=v[1];lease->seq=edge_rd64(v+2);lease->token=edge_rd64(v+10);
        if(!lease->seq || !!(lease->flags&LEASE_PENDING)!=!!lease->token)return EFS_ERR_PROTO;
    } else return EFS_ERR_PROTO;
    return (lease->flags&~(LEASE_NORMAL|LEASE_PENDING))?EFS_ERR_PROTO:EFS_OK;
}
static void edge_lease_pack(const struct edge_lease *lease,uint8_t v[EDGE_VALUE])
{
    v[0]=3;v[1]=lease->flags;edge_be64(v+2,lease->seq);edge_be64(v+10,lease->token);
}
/* A last close with no remaining work need not enqueue an empty GC round.
 * This proof runs in the same serialized state-machine entry as the lease
 * transition. Only the exact lease being closed is ignored; malformed or
 * foreign ownership, locks, append state and transaction ownership all keep
 * the durable deferred obligation. Existing markers are never erased here. */
struct close_empty_scan { const uint8_t *key; uint32_t len,seen; };
static int close_other_lease(void *ctx,const uint8_t *key,uint32_t len,
    const uint8_t *value,uint32_t length)
{
    struct close_empty_scan *own=ctx;
    if(++own->seen>64)return 1; /* bounded proof; defer a longer prefix */
    if(len==own->len && !memcmp(key,own->key,len))return 0;
    return any_cb(NULL,key,len,value,length);
}
static int close_step_present(void *,const uint8_t *,uint32_t,
    const uint8_t *,uint32_t);
static int close_empty_proof(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
    const uint8_t *own_key,uint32_t own_len,const uint8_t *marker,uint32_t marker_len)
{
    uint32_t shard=efs_kv_inode_shard(ino),len;uint8_t key[EFS_KV_KEY_MAX];
    uint8_t value[EFS_CLOSE_STEP_VALUE];uint32_t vl=sizeof(value);
    int rc=efs_kv_get(kv,marker,marker_len,value,&vl);
    if(rc==EFS_OK)return 0;
    if(rc!=EFS_ERR_NOT_FOUND)return rc;
    rc=efs_kv_key_inode(shard,ino,key,&len);
    if(rc==EFS_OK)rc=efs_txn_key_exclusive(kv,key,len);
    if(rc==EFS_ERR_BUSY)return 0;
    if(rc!=EFS_OK)return rc;
    struct efs_meta_row row;
    rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc==EFS_ERR_NOT_FOUND)return 0;
    if(rc!=EFS_OK)return rc;
    if(row.generation!=gen || !row.nlink)return 0;
    rc=efs_kv_key_lease_prefix(shard,ino,gen,key,&len);
    struct close_empty_scan own={.key=own_key,.len=own_len};
    if(rc==EFS_OK)rc=efs_kv_scan_prefix(kv,key,len,close_other_lease,&own);
    if(rc==1)return 0;
    if(rc!=EFS_OK)return rc;
    rc=efs_lock_count(kv,ino,gen);
    if(rc>0)return 0;
    if(rc<0)return rc;
    rc=efs_kv_key_append_rsv_prefix(shard,ino,gen,key,&len);
    if(rc==EFS_OK)rc=efs_kv_scan_prefix(kv,key,len,close_step_present,NULL);
    if(rc==1)return 0;
    if(rc!=EFS_OK)return rc;
    rc=efs_kv_key_append_cur(shard,ino,gen,key,&len);
    if(rc==EFS_OK) {vl=sizeof(value);rc=efs_kv_get(kv,key,len,value,&vl);}
    if(rc==EFS_OK)return 0;
    return rc==EFS_ERR_NOT_FOUND?1:rc;
}

static int lease_guarded_edge(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                          const struct efs_opid_req *q,unsigned flags,uint64_t token,int deferred)
{
    uint32_t shard=efs_kv_inode_shard(ino),wklen=0,lklen=0;
    uint8_t wkey[EFS_KV_KEY_MAX],lkey[EFS_KV_KEY_MAX],digest[32],identity[32]={0};
    struct edge_window window;
    if(!kv || !ino || !gen || !q || !efs_opid_req_valid(q) || !q->id.session_epoch ||
       q->ack>=q->id.seq || !flags || (flags&~7u) ||
       (flags&(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLOSE))==3u ||
       !!(flags&EFS_LEASE_EDGE_CLEAR_PENDING)!=!!token)return EFS_ERR_INVAL;
    int rc=efs_session_accept(kv,shard,q->id.client_uuid,q->id.session_epoch);
    if(rc!=EFS_OK)return rc;
    rc=edge_window_key(shard,q->id.client_uuid,q->id.session_epoch,wkey,&wklen);
    if(rc==EFS_OK)rc=edge_window_load(kv,wkey,wklen,&window);
    if(rc!=EFS_OK)return rc;
    for (unsigned i=0;i<window.count;i++)
        if (efs_kv_inode_shard(window.replies[i].ino)!=shard) return EFS_ERR_PROTO;
    edge_be64(identity,ino);edge_be64(identity+8,gen);be32(identity+16,flags);
    edge_be64(identity+20,token);be32(identity+28,deferred?UINT32_C(0x434c4f53):UINT32_C(0x4c454153));
    efs_hash(identity,sizeof(identity),digest);
    if(q->id.seq<=window.floor)return EFS_ERR_STALE;
    for(unsigned i=0;i<window.count;i++)if(window.replies[i].seq==q->id.seq)
        return memcmp(window.replies[i].digest,digest,32)?EFS_ERR_PROTO:window.replies[i].rc;

    rc=efs_txn_key_exclusive(kv,wkey,wklen);
    if(rc!=EFS_OK)return rc;

    /* At most 16 old closed rows are considered, and all retirement is atomic
     * with the new floor/effect. A later OPEN on the same key is preserved. */
    struct efs_kv_item items[EDGE_SLOTS+3];uint32_t count=0;
    uint8_t retired[EDGE_SLOTS][EFS_KV_KEY_MAX];uint32_t rlen[EDGE_SLOTS];
    uint64_t floor=q->ack>window.floor?q->ack:window.floor;
    if(q->id.seq-floor>EFS_OPID_BITMAP_BITS)return EFS_ERR_BUSY;
    unsigned keep=0;
    for(unsigned i=0;i<window.count;i++) {
        struct edge_reply reply=window.replies[i];
        if(reply.seq>floor){window.replies[keep++]=reply;continue;}
        rc=efs_kv_key_lease(shard,reply.ino,reply.gen,q->id.client_uuid,
                            q->id.session_epoch,retired[count],&rlen[count]);
        struct edge_lease old;
        if(rc==EFS_OK)rc=edge_lease_load(kv,retired[count],rlen[count],&old);
        if(rc!=EFS_OK)return rc;
        if(!old.flags && old.seq==reply.seq) {
            rc=efs_txn_key_exclusive(kv,retired[count],rlen[count]);
            if(rc!=EFS_OK)return rc;
            items[count]=(struct efs_kv_item){EFS_KV_DEL,retired[count],rlen[count],NULL,0};
            count++;
        }
    }
    window.count=keep;window.floor=floor;
    if(window.count==EDGE_SLOTS)return EFS_ERR_BUSY;
    rc=efs_kv_key_lease(shard,ino,gen,q->id.client_uuid,q->id.session_epoch,lkey,&lklen);
    struct edge_lease lease;
    if(rc==EFS_OK)rc=efs_txn_key_exclusive(kv,lkey,lklen);
    if(rc==EFS_OK)rc=edge_lease_load(kv,lkey,lklen,&lease);
    if(rc!=EFS_OK)return rc;
    int verdict=EFS_OK;
    if(q->id.seq<=lease.seq || (lease.token && q->id.seq<=lease.token))verdict=EFS_ERR_STALE;
    if(verdict==EFS_OK && (flags&EFS_LEASE_EDGE_CLEAR_PENDING) &&
       (!(lease.flags&LEASE_PENDING) || lease.token!=token))verdict=EFS_ERR_STALE;
    if(verdict==EFS_OK && (flags&EFS_LEASE_EDGE_OPEN)) {
        rc=lease_open_guard(kv,ino);
        if(rc!=EFS_OK)return rc;
        struct efs_meta_row row;
        verdict=efs_meta_apply_get_inode(kv,ino,&row);
        if(verdict==EFS_OK && row.generation!=gen)verdict=EFS_ERR_STALE;
        if(verdict==EFS_OK && S_ISDIR(row.mode))verdict=EFS_ERR_INVAL;
        if(verdict!=EFS_OK && verdict!=EFS_ERR_NOT_FOUND && verdict!=EFS_ERR_STALE &&
           verdict!=EFS_ERR_INVAL)return verdict;
    }
    uint8_t close_key[EFS_KV_KEY_MAX],close_value[EFS_CLOSE_STEP_VALUE];
    uint32_t close_len=0;
    if(deferred && verdict==EFS_OK) {
        rc=efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,close_key,&close_len);
        if(rc!=EFS_OK)return rc;
        close_key[2]=EFS_KV_KIND_CLOSE_STEP;
        rc=efs_txn_key_exclusive(kv,close_key,close_len);
        if(rc!=EFS_OK)return rc; /* BUSY is retryable, never record it as success */
        /* OPEN+CLEAR_PENDING transfers the hold to a normal lease; it is
         * not a last-close event. A later CLOSE or fenced session cleanup
         * durably creates the obligation. Preserve any older marker, but
         * do not make GC repeatedly probe a newly adopted, still-open file. */
        if(!(flags&EFS_LEASE_EDGE_OPEN) &&
           (flags&(EFS_LEASE_EDGE_CLOSE|EFS_LEASE_EDGE_CLEAR_PENDING))) {
            int empty=0;
            if(flags==EFS_LEASE_EDGE_CLOSE && lease.flags==LEASE_NORMAL) {
                empty=close_empty_proof(kv,ino,gen,lkey,lklen,close_key,close_len);
                if(empty<0)return empty;
            }
            if(!empty) {
                edge_be64(close_value,gen);memcpy(close_value+8,q->id.client_uuid,16);
                be32(close_value+24,q->id.session_epoch);edge_be64(close_value+28,q->id.seq);
                items[count++]=(struct efs_kv_item){EFS_KV_PUT,close_key,close_len,
                                                 close_value,sizeof(close_value)};
            }
        }
    }
    uint8_t lvalue[EDGE_VALUE],wvalue[EDGE_WINDOW_MAX];
    if(verdict==EFS_OK) {
        if(flags&EFS_LEASE_EDGE_OPEN)lease.flags|=LEASE_NORMAL;
        if(flags&EFS_LEASE_EDGE_CLOSE)lease.flags&=~LEASE_NORMAL;
        if(flags&EFS_LEASE_EDGE_CLEAR_PENDING){lease.flags&=~LEASE_PENDING;lease.token=0;}
        lease.seq=q->id.seq;edge_lease_pack(&lease,lvalue);
        items[count++]=(struct efs_kv_item){EFS_KV_PUT,lkey,lklen,lvalue,sizeof(lvalue)};
    }
    struct edge_reply *reply=&window.replies[window.count++];
    reply->seq=q->id.seq;reply->ino=ino;reply->gen=gen;reply->rc=verdict;
    memcpy(reply->digest,digest,32);
    uint32_t wn=edge_window_pack(&window,wvalue);
    items[count++]=(struct efs_kv_item){EFS_KV_PUT,wkey,wklen,wvalue,wn};
    rc=efs_kv_batch(kv,items,count);
    return rc==EFS_OK?verdict:rc;
}
int efs_lease_guarded_edge(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                            const struct efs_opid_req *q,unsigned flags,uint64_t token)
{
    return lease_guarded_edge(kv,ino,gen,q,flags,token,0);
}
int efs_lease_guarded_edge_deferred(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                            const struct efs_opid_req *q,unsigned flags,uint64_t token)
{
    return lease_guarded_edge(kv,ino,gen,q,flags,token,1);
}

int efs_lease_guarded_retire(struct efs_kv *kv,uint32_t shard,
                            const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch)
{
    int rc=efs_session_reclaimable(kv,shard,uuid,epoch);
    if(rc!=EFS_OK)return rc; /* missing authority is not successful retirement */
    /* Retirement never expands into whole-session ownership deletion. The
     * exact-key cleanup coordinator must first drain the old lease/lock phase.
     * Its durable floor prevents matching rows from being recreated. */
    struct efs_session_cleanup_key keys[EFS_SESSION_CLEANUP_MAX];uint32_t n=0;
    rc=efs_session_cleanup_select(kv,shard,uuid,epoch,keys,&n);
    if(rc!=EFS_OK)return rc;
    if(n)return EFS_ERR_BUSY;
    uint8_t key[EFS_KV_KEY_MAX];uint32_t len=0;
    rc=edge_window_key(shard,uuid,epoch,key,&len);
    if(rc==EFS_OK)rc=efs_txn_key_exclusive(kv,key,len);
    if(rc!=EFS_OK)return rc;
    rc=efs_kv_del(kv,key,len);
    return rc==EFS_ERR_NOT_FOUND?EFS_OK:rc;
}

static int lease_guarded_probe(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                           const struct efs_opid_req *q,unsigned flags,uint64_t token,
                           int *verdict,int deferred)
{
    if(!kv || !verdict || !ino || !gen || !q || !efs_opid_req_valid(q) ||
       !q->id.session_epoch || q->ack>=q->id.seq || !flags || (flags&~7u) ||
       (flags&(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLOSE))==3u ||
       !!(flags&EFS_LEASE_EDGE_CLEAR_PENDING)!=!!token)return EFS_ERR_INVAL;
    uint32_t shard=efs_kv_inode_shard(ino),klen=0;
    int rc=efs_session_accept(kv,shard,q->id.client_uuid,q->id.session_epoch);
    uint8_t key[EFS_KV_KEY_MAX],identity[32]={0},digest[32];
    struct edge_window window;
    if(rc==EFS_OK)rc=edge_window_key(shard,q->id.client_uuid,q->id.session_epoch,key,&klen);
    if(rc==EFS_OK)rc=edge_window_load(kv,key,klen,&window);
    if(rc!=EFS_OK)return rc;
    for(unsigned i=0;i<window.count;i++)
        if(efs_kv_inode_shard(window.replies[i].ino)!=shard)return EFS_ERR_PROTO;
    if(q->id.seq<=window.floor)return EFS_ERR_STALE;
    edge_be64(identity,ino);edge_be64(identity+8,gen);be32(identity+16,flags);
    edge_be64(identity+20,token);be32(identity+28,deferred?UINT32_C(0x434c4f53):UINT32_C(0x4c454153));
    efs_hash(identity,sizeof(identity),digest);
    for(unsigned i=0;i<window.count;i++) {
        const struct edge_reply *reply=&window.replies[i];
        if(reply->seq!=q->id.seq)continue;
        if(memcmp(reply->digest,digest,32))return EFS_ERR_PROTO;
        *verdict=reply->rc;return 1;
    }
    return 0;
}

int efs_lease_guarded_probe(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
    const struct efs_opid_req *q,unsigned flags,uint64_t token,int *verdict)
{
    return lease_guarded_probe(kv,ino,gen,q,flags,token,verdict,0);
}
int efs_lease_guarded_probe_deferred(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
    const struct efs_opid_req *q,unsigned flags,uint64_t token,int *verdict)
{
    return lease_guarded_probe(kv,ino,gen,q,flags,token,verdict,1);
}

static int close_step_present(void *ctx,const uint8_t *key,uint32_t len,
    const uint8_t *value,uint32_t length)
{
    (void)ctx;(void)key;(void)len;(void)value;(void)length;return 1;
}
int efs_close_step_apply(struct efs_kv *kv,efs_ino_t ino,
    const uint8_t identity[EFS_CLOSE_STEP_VALUE],unsigned phase)
{
    if(!kv || !ino || !identity ||
       (phase!=EFS_CLOSE_STEP_LOCKS && phase!=EFS_CLOSE_STEP_RETIRE))return EFS_ERR_INVAL;
    uint64_t gen=cleanup_rd64(identity);
    if(!gen || !rd32(identity+24) || !cleanup_rd64(identity+28))return EFS_ERR_INVAL;
    uint32_t shard=efs_kv_inode_shard(ino),len=0,vl=EFS_CLOSE_STEP_VALUE;
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_CLOSE_STEP_VALUE];
    int rc=efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,key,&len);
    if(rc!=EFS_OK)return rc;
    key[2]=EFS_KV_KIND_CLOSE_STEP;
    rc=efs_txn_key_exclusive(kv,key,len);
    if(rc!=EFS_OK)return rc;
    rc=efs_kv_get(kv,key,len,value,&vl);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;
    if(rc!=EFS_OK)return rc;
    if(vl!=sizeof(value))return EFS_ERR_PROTO;
    if(memcmp(value,identity,sizeof(value)))return EFS_ERR_STALE;
    int held=efs_lease_any(kv,ino,gen);
    if(held<0)return held;
    if(held)return EFS_ERR_BUSY;
    if(phase==EFS_CLOSE_STEP_LOCKS)return efs_lock_drop_file(kv,ino,gen);
    uint8_t inode_key[EFS_KV_KEY_MAX];uint32_t inode_len;
    rc=efs_kv_key_inode(shard,ino,inode_key,&inode_len);
    if(rc==EFS_OK)rc=efs_txn_key_exclusive(kv,inode_key,inode_len);
    if(rc!=EFS_OK)return rc;
    int locks=efs_lock_count(kv,ino,gen);
    if(locks<0)return locks;
    if(locks)return EFS_ERR_BUSY;
    uint8_t prefix[EFS_KV_KEY_MAX];uint32_t pl;
    rc=efs_kv_key_append_rsv_prefix(shard,ino,gen,prefix,&pl);
    if(rc==EFS_OK)rc=efs_kv_scan_prefix(kv,prefix,pl,close_step_present,NULL);
    if(rc==1)return EFS_ERR_BUSY;
    if(rc!=EFS_OK)return rc;
    rc=efs_kv_key_append_cur(shard,ino,gen,prefix,&pl);
    if(rc==EFS_OK) {
        uint8_t cursor[24];uint32_t cl=sizeof(cursor);
        rc=efs_kv_get(kv,prefix,pl,cursor,&cl);
        if(rc==EFS_OK)return EFS_ERR_BUSY;
        if(rc!=EFS_ERR_NOT_FOUND)return rc;
    } else return rc;
    return efs_kv_del(kv,key,len);
}
