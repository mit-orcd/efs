#include "efs/meta_apply.h"
#include "efs/expiry.h"
#include "efs/metadata.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include "efs/membership.h"
#include "efs/session.h"
#include "efs/put_ticket.h"
#include "efs/pack_mapping.h"
#include "efs/kv_lsm.h"
#include "efs/checksum.h"
#include "efs/publication.h"
#include "efs/dir_layout.h"
#include "efs/dir_spread.h"
#include "efs/txn.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INO_VAL  EFS_META_INO_BYTES
#define DENT_VAL 20
#define ALLOC_VAL 8
#define LANE_VAL EFS_META_LANE_BYTES
#define CHUNK_HDR 24 /* gen:8 profile:4 epoch:8 base_clip:4 */
#define CHUNK_VAL (CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS + \
                   EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)
/* Trailer after CHUNK_VAL: u16 count, u16 pad, then count records.
 * Absent (vlen == CHUNK_VAL) means no deltas. A full-chunk CAS writes
 * the short value again and the list is gone. */
#define DELTA_REC 140
#define DELTA_HDR 4
#define CHUNK_VAL_MAX (CHUNK_VAL + EFS_PACK_REF_BYTES + DELTA_HDR + \
                       EFS_CHUNK_DELTA_MAX * DELTA_REC)

/* All ordinary metadata mutations honor prepared EXCL images. REDUCEs
 * remain commutative. Chunk mutations share the lane's exclusion even when
 * a sweep deletes rows before writing/deleting the lane stamp. Checks and
 * the final write are serialized by the participant's Raft apply log. */
static int pub_stage_owner(struct efs_kv *kv, const uint8_t *chunk_key,
                           uint32_t kl, struct efs_txid *t,
                           struct efs_txn_parts *parts);

/* `self` is the transaction whose own staged publication is being resolved:
 * its stage is the one exclusion that must not refuse the resolving batch. */
static int meta_write_check_as(struct efs_kv *kv, const uint8_t *key, uint32_t kl,
                               const struct efs_txid *self)
{
    uint8_t lane_key[20];
    if (!key || kl < 3)
        return EFS_ERR_INVAL;
    switch (key[2]) {
    case EFS_KV_KIND_INODE:
    case EFS_KV_KIND_LANE:
    case EFS_KV_KIND_CONTENT_FENCE:
    case EFS_KV_KIND_LANE_AUTHORITY:
    case EFS_KV_KIND_PACK_SEGMENT:
        return efs_txn_key_exclusive(kv, key, kl);
    case EFS_KV_KIND_CHUNK:
        if (kl != 24)
            return EFS_ERR_INVAL;
        memcpy(lane_key, key, sizeof(lane_key));
        lane_key[2] = EFS_KV_KIND_LANE;
        {
            int rc = efs_txn_key_exclusive(kv, lane_key, sizeof(lane_key));
            struct efs_txid owner;
            if (rc != EFS_OK)
                return rc;
            /* W77: a staged publication owns its chunk until resolved. */
            rc = pub_stage_owner(kv, key, kl, &owner, NULL);
            if (rc == EFS_ERR_NOT_FOUND)
                return EFS_OK;
            if (rc != EFS_OK)
                return rc;
            return self && !memcmp(owner.bytes, self->bytes, EFS_TXN_ID_LEN)
                       ? EFS_OK : EFS_ERR_BUSY;
        }
    case EFS_KV_KIND_APPEND_CUR:
    case EFS_KV_KIND_APPEND_RECEIPT:
    case EFS_KV_KIND_TRUNCATE_RECEIPT:
    case EFS_KV_KIND_APPEND_RSV: {
        uint8_t inode_key[11];
        if (kl < 19)
            return EFS_ERR_INVAL;
        memcpy(inode_key, key, sizeof(inode_key));
        inode_key[2] = EFS_KV_KIND_INODE;
        /* An inode intent freezes reservation creation/resolution even when
         * the closed cursor was absent at PREPARE. */
        return efs_txn_key_exclusive(kv, inode_key, sizeof(inode_key));
    }
    default:
        return EFS_OK;
    }
}

static int meta_write_check(struct efs_kv *kv, const uint8_t *key, uint32_t kl)
{
    return meta_write_check_as(kv, key, kl, NULL);
}

/* `dry` runs every check and writes nothing: a staged publication's PREPARE
 * proves the publish would apply now (W77). */
static int meta_write_batch_as(struct efs_kv *kv, const struct efs_kv_item *items,
                               uint32_t count, const struct efs_txid *self, int dry)
{
    for (uint32_t i = 0; i < count; ++i) {
        int rc = meta_write_check_as(kv, items[i].key, items[i].klen, self);
        if (rc != EFS_OK)
            return rc;
    }
    return dry ? EFS_OK : efs_kv_batch(kv, items, count);
}

static int meta_write_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                             uint32_t count)
{
    return meta_write_batch_as(kv, items, count, NULL, 0);
}

static int meta_write_put(struct efs_kv *kv, const uint8_t *key, uint32_t kl,
                           const uint8_t *value, uint32_t vl)
{
    int rc = meta_write_check(kv, key, kl);
    return rc == EFS_OK ? efs_kv_put(kv, key, kl, value, vl) : rc;
}

static int meta_write_del(struct efs_kv *kv, const uint8_t *key, uint32_t kl)
{
    int rc = meta_write_check(kv, key, kl);
    return rc == EFS_OK ? efs_kv_del(kv, key, kl) : rc;
}

/* Rows of an inode that no name references: the query catalog picks the
 * deletion up at its next rebuild instead of scheduling one (kv_lsm.h). */
static int meta_cleanup_del(struct efs_kv *kv, const uint8_t *key, uint32_t kl)
{
    efs_kv_lsm_cleanup_scope(1);
    int rc = meta_write_del(kv, key, kl);
    efs_kv_lsm_cleanup_scope(0);
    return rc;
}
static int meta_cleanup_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                              uint32_t count)
{
    efs_kv_lsm_cleanup_scope(1);
    int rc = meta_write_batch(kv, items, count);
    efs_kv_lsm_cleanup_scope(0);
    return rc;
}

static void be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}


static void be64(uint8_t *p, uint64_t v)
{
    efs_store_be32(p, (uint32_t)(v >> 32));
    efs_store_be32(p + 4, (uint32_t)v);
}


static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)efs_load_be32(p) << 32) | efs_load_be32(p + 4);
}

static uint64_t max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

/* A write lane's high-water marks — the values stat() reduces over (§7.3).
 *
 * The first 24 bytes are deliberately the same triple a transaction's
 * commutative reduction carries (`struct efs_txn_reduce`), so a pending
 * committed reduction folds into a materialized lane without either side
 * knowing the other's layout. Anything after that is the lane owner's.
 *
 * `mtime_gen` records which utimens generation `max_mtime` belongs to. Only
 * utimens can move a timestamp backwards, so it bumps the inode row's
 * generation and a stat ignores lane mtimes stamped under an older one —
 * which invalidates every stale lane mtime at once instead of rewriting 64
 * lanes. `fenced_epoch` is the content-epoch fence a truncate installs.
 * `present` counts the lane's live chunk rows (D17): a publish that creates
 * a row adds one, a truncate's range delete subtracts what it dropped, and
 * getattr sums it across lanes for st_blocks. */
struct lane_rec {
    uint64_t max_end;
    uint64_t max_mtime;
    uint64_t max_ctime;
    uint64_t seq;
    uint64_t fenced_epoch;
    uint64_t mtime_gen;
    uint64_t append_bar; /* live reservation watermark; 0 = no barrier */
    uint64_t present;
    /* D25 truncate sweep (protocol §4.2): the target fence epoch being
     * swept, the next chunk index to examine, and the highest target this
     * lane was completely swept for. */
    uint64_t sweep_target;
    uint64_t sweep_next_ci;
    uint64_t swept_epoch;
};

static void pack_lane(uint8_t *p, const struct lane_rec *l)
{
    be64(p + 0, l->max_end);
    be64(p + 8, l->max_mtime);
    be64(p + 16, l->max_ctime);
    be64(p + 24, l->seq);
    be64(p + 32, l->fenced_epoch);
    be64(p + 40, l->mtime_gen);
    be64(p + 48, l->append_bar);
    be64(p + 56, l->present);
    be64(p + 64, l->sweep_target);
    be64(p + 72, l->sweep_next_ci);
    be64(p + 80, l->swept_epoch);
}

/* Tolerates a record carrying only the reduce triple: a lane whose only
 * writes so far arrived as reductions has no owner-written tail. */
static int unpack_lane(const uint8_t *p, uint32_t n, struct lane_rec *l)
{
    memset(l, 0, sizeof(*l));
    if (n < 24)
        return EFS_ERR_PROTO;
    l->max_end = rd64(p + 0);
    l->max_mtime = rd64(p + 8);
    l->max_ctime = rd64(p + 16);
    if (n >= 32)
        l->seq = rd64(p + 24);
    if (n >= 40)
        l->fenced_epoch = rd64(p + 32);
    if (n >= 48)
        l->mtime_gen = rd64(p + 40);
    if (n >= 56)
        l->append_bar = rd64(p + 48);
    if (n >= 64)
        l->present = rd64(p + 56);
    if (n >= 88) {
        l->sweep_target = rd64(p + 64);
        l->sweep_next_ci = rd64(p + 72);
        l->swept_epoch = rd64(p + 80);
    }
    return EFS_OK;
}

/* D25 adoption: the one transition that moves a lane to a newer content
 * epoch (publish, BOOTSTRAP, fences). The lane's EOF summary belongs to the
 * epoch it was written under, so it is cleared and the validation sequence
 * advances; an equal or older epoch changes nothing. Returns 1 if it moved. */
static int lane_adopt(struct lane_rec *ln, uint64_t epoch)
{
    if (epoch <= ln->fenced_epoch)
        return 0;
    ln->fenced_epoch = epoch;
    ln->max_end = 0;
    ln->seq++;
    return 1;
}

/* Dir-lane stamp for a HASHED/SPLITTING mutation. Same record as a file
 * write lane — MAX mtime/ctime + seq — living on the dentry shard so a
 * create does not bounce back to the directory's home leader (§7.4). */
static int dir_lane_stamp(struct efs_kv *kv, const struct efs_meta_row *dir,
                          const char *name, uint64_t now,
                          uint8_t *k_ln, uint32_t *kl, uint8_t *v_ln)
{
    uint8_t lane = efs_kv_dir_lane(name);
    uint8_t old[LANE_VAL];
    uint32_t vn = sizeof(old);
    struct lane_rec ln;
    int rc;

    *kl = 0;
    rc = efs_kv_key_lane(efs_kv_lane_shard(dir->ino, lane), dir->ino,
                         dir->generation, lane, k_ln, kl);
    if (rc != EFS_OK)
        return rc;
    memset(&ln, 0, sizeof(ln));
    rc = efs_kv_get(kv, k_ln, *kl, old, &vn);
    if (rc == EFS_OK) {
        rc = unpack_lane(old, vn, &ln);
        if (rc != EFS_OK)
            return rc;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    ln.max_mtime = max_u64(ln.max_mtime, now);
    ln.max_ctime = max_u64(ln.max_ctime, now);
    ln.seq++;
    ln.mtime_gen = dir->mtime_gen;
    pack_lane(v_ln, &ln);
    return EFS_OK;
}

int efs_meta_stamp_dir_lane(struct efs_kv *kv, const struct efs_meta_row *dir,
                            const char *name, uint64_t now, uint8_t *key,
                            uint32_t *klen, uint8_t *val, uint32_t cap)
{
    if (!kv || !dir || !name || !key || !klen || !val || cap < LANE_VAL)
        return EFS_ERR_INVAL;
    return dir_lane_stamp(kv, dir, name, now, key, klen, val);
}

/* Per-(dir, dentry-shard) emptiness witness. Every insert (and every other
 * mutation of that shard's names) bumps it so an RMDIR that observed empty
 * cannot commit across a concurrent create (directory.md). */
static int dseq_bump(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                     uint8_t lane, uint8_t *k, uint32_t *kl, uint8_t *v)
{
    uint8_t buf[8];
    uint32_t n = 8;
    uint64_t seq = 0;
    int rc;

    rc = efs_kv_key_dseq(shard, dir, lane, k, kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, *kl, buf, &n);
    if (rc == EFS_OK && n >= 8)
        seq = rd64(buf);
    else if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
        return rc;
    be64(v, seq + 1);
    return EFS_OK;
}

static void pack_inode(uint8_t *p, const struct efs_meta_row *r)
{
    be64(p + 0, r->ino);
    be64(p + 8, r->generation);
    efs_store_be32(p + 16, r->mode);
    efs_store_be32(p + 20, r->nlink);
    be64(p + 24, r->parent);
    be64(p + 32, r->base_size);
    be64(p + 40, r->active_lanes);
    be64(p + 48, r->content_epoch);
    p[56] = r->layout;
    memset(p + 57, 0, 3);
    efs_store_be32(p + 60, r->nents);
    be64(p + 64, r->layout_epoch);
    be64(p + 72, r->used_shards);
    efs_store_be32(p + 80, r->uid);
    efs_store_be32(p + 84, r->gid);
    be64(p + 88, r->base_mtime);
    be64(p + 96, r->base_atime);
    be64(p + 104, r->base_ctime);
    be64(p + 112, r->mtime_gen);
    be64(p + 120, r->parent_version);
}

static int unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r)
{
    if (!p || !r || n < INO_VAL)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->ino = rd64(p + 0);
    r->generation = rd64(p + 8);
    r->mode = efs_load_be32(p + 16);
    r->nlink = efs_load_be32(p + 20);
    r->parent = rd64(p + 24);
    r->base_size = rd64(p + 32);
    r->active_lanes = rd64(p + 40);
    r->content_epoch = rd64(p + 48);
    r->layout = p[56];
    r->nents = efs_load_be32(p + 60);
    r->layout_epoch = rd64(p + 64);
    r->used_shards = rd64(p + 72);
    r->uid = efs_load_be32(p + 80);
    r->gid = efs_load_be32(p + 84);
    r->base_mtime = rd64(p + 88);
    r->base_atime = rd64(p + 96);
    r->base_ctime = rd64(p + 104);
    r->mtime_gen = rd64(p + 112);
    r->parent_version = rd64(p + 120);
    return EFS_OK;
}

/* D25 history, format 2 (BE): u32 format, u32 count, u64 revision, then
 * count*(epoch, size, done lane bitmap). Absent is the empty history at
 * revision 0. Malformed or other formats fail closed, never become an
 * empty history that could expose discarded bytes. */
int efs_meta_pack_fence_history(const struct efs_fence_history *h,
                                uint8_t *out, uint32_t cap, uint32_t *len)
{
    uint32_t need;
    if (!out || !len || efs_fence_history_valid(h) != EFS_OK)
        return EFS_ERR_INVAL;
    need = 16u + h->count * 24u;
    if (cap < need)
        return EFS_ERR_INVAL;
    efs_store_be32(out, 2);
    efs_store_be32(out + 4, h->count);
    be64(out + 8, h->revision);
    for (uint32_t i = 0; i < h->count; ++i) {
        be64(out + 16 + i * 24, h->entries[i].epoch);
        be64(out + 24 + i * 24, h->entries[i].size);
        be64(out + 32 + i * 24, h->entries[i].done);
    }
    *len = need;
    return EFS_OK;
}

int efs_meta_unpack_fence_history(const uint8_t *value, uint32_t len,
                                  struct efs_fence_history *out)
{
    struct efs_fence_history h = {0};
    if (!value || !out || len < 16 || efs_load_be32(value) != 2)
        return EFS_ERR_PROTO;
    h.count = efs_load_be32(value + 4);
    if (h.count > EFS_FENCE_HISTORY_MAX || len != 16u + h.count * 24u)
        return EFS_ERR_PROTO;
    h.revision = rd64(value + 8);
    for (uint32_t i = 0; i < h.count; ++i) {
        h.entries[i].epoch = rd64(value + 16 + i * 24);
        h.entries[i].size = rd64(value + 24 + i * 24);
        h.entries[i].done = rd64(value + 32 + i * 24);
    }
    if (efs_fence_history_valid(&h) != EFS_OK)
        return EFS_ERR_PROTO;
    *out = h;
    return EFS_OK;
}

static int content_fence_key(efs_ino_t ino, uint64_t gen, uint8_t authority,
                              uint8_t *key, uint32_t *len)
{
    uint32_t shard;
    int rc;
    if (!ino || !gen || authority > EFS_META_FENCE_INODE)
        return EFS_ERR_INVAL;
    shard = authority == EFS_META_FENCE_INODE ? efs_kv_inode_shard(ino) :
            efs_kv_lane_shard(ino, authority);
    rc = efs_kv_key_lane(shard, ino, gen, authority, key, len);
    if (rc == EFS_OK)
        key[2] = EFS_KV_KIND_CONTENT_FENCE;
    return rc;
}

static int fence_history_read(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t gen, uint8_t authority,
                                efs_txn_coord_fn coord, void *ctx,
                                struct efs_txn_pending *pend,
                                struct efs_fence_history *out)
{
    uint8_t key[EFS_KV_KEY_MAX], value[EFS_META_FENCE_BYTES];
    uint32_t kl = 0, len = sizeof(value);
    int rc;
    if (!kv || !out)
        return EFS_ERR_INVAL;
    rc = content_fence_key(ino, gen, authority, key, &kl);
    if (rc == EFS_OK)
        rc = coord ? efs_txn_read_ex(kv, key, kl, coord, ctx, value, &len, pend) :
                     efs_kv_get(kv, key, kl, value, &len);
    if (rc == EFS_ERR_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, kl, efs_meta_unpack_fence_history(value, len, out));
}

int efs_meta_get_fence_history(struct efs_kv *kv, efs_ino_t ino,
                               uint64_t gen, uint8_t authority,
                               struct efs_fence_history *out)
{
    return fence_history_read(kv, ino, gen, authority, NULL, NULL, NULL, out);
}

static uint64_t writer_history_floor(uint64_t epoch, const struct efs_fence_history *h)
{
    uint64_t floor = epoch;
    for (uint32_t i = h->count; i && h->entries[i - 1].epoch == floor; --i)
        --floor;
    return floor;
}

static int lane_authority_key(efs_ino_t ino, uint64_t gen, uint8_t lane,
                               uint8_t *key, uint32_t *kl)
{
    if (!ino || !gen || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    int rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, kl);
    if (rc == EFS_OK)
        key[2] = EFS_KV_KIND_LANE_AUTHORITY;
    return rc;
}

static void pack_lane_authority(uint8_t *v, uint32_t cs, uint64_t epoch, uint64_t floor)
{
    memset(v, 0, EFS_META_LANE_AUTHORITY_BYTES);
    efs_store_be32(v, 1); efs_store_be32(v + 4, cs); be64(v + 8, epoch); be64(v + 16, floor);
    efs_store_be32(v + 24, EFS_META_LANES);
}

static int unpack_lane_authority(const uint8_t *v, uint32_t len, uint32_t *cs,
                                  uint64_t *epoch, uint64_t *floor)
{
    if (len != EFS_META_LANE_AUTHORITY_BYTES || efs_load_be32(v) != 1 ||
        !efs_chunk_size_valid(efs_load_be32(v + 4)) || efs_load_be32(v + 24) != EFS_META_LANES ||
        efs_load_be32(v + 28) || rd64(v + 16) > rd64(v + 8))
        return EFS_ERR_PROTO;
    *cs = efs_load_be32(v + 4); *epoch = rd64(v + 8); *floor = rd64(v + 16);
    return EFS_OK;
}

static void pack_dentry(uint8_t *p, const struct efs_meta_dentry *d)
{
    be64(p + 0, d->ino);
    be64(p + 8, d->generation);
    efs_store_be32(p + 16, d->type);
}

static int unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d)
{
    if (!p || !d || n < DENT_VAL)
        return EFS_ERR_PROTO;
    memset(d, 0, sizeof(*d));
    d->ino = rd64(p + 0);
    d->generation = rd64(p + 8);
    d->type = efs_load_be32(p + 16);
    return EFS_OK;
}

static int kv_get_copy(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                       uint8_t *buf, uint32_t cap, uint32_t *out_len)
{
    uint32_t n = cap;
    int rc;

    rc = efs_kv_get(kv, key, klen, buf, &n);
    if (rc != EFS_OK)
        return rc;
    if (out_len)
        *out_len = n;
    return EFS_OK;
}

int efs_meta_apply_get_inode(struct efs_kv *kv, efs_ino_t ino,
                             struct efs_meta_row *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    if (!kv || !out || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_inode(val, vlen, out));
}

/* Handler reads must observe committed replacements even when an old row
 * exists. Apply readers use the plain getter and never contact a coordinator. */
static int inode_read_tx(struct efs_kv *kv, efs_ino_t ino,
                          efs_txn_coord_fn coord, void *ctx,
                          struct efs_txn_pending *pend,
                          struct efs_meta_row *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0, vlen = sizeof(val);
    int rc;
    if (!coord)
        return efs_meta_apply_get_inode(kv, ino, out);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc == EFS_OK)
        rc = efs_txn_read_ex(kv, key, klen, coord, ctx, val, &vlen, pend);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_inode(val, vlen, out));
}

int efs_meta_apply_get_inode_tx(struct efs_kv *kv, efs_ino_t ino,
                                efs_txn_coord_fn coord, void *ctx,
                                struct efs_meta_row *out)
{
    return inode_read_tx(kv, ino, coord, ctx, NULL, out);
}

int efs_meta_get_writer_view_tx(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t gen, efs_txn_coord_fn coord,
                                void *ctx, struct efs_meta_writer_view *out)
{
    if (!kv || !ino || !gen || !out)
        return EFS_ERR_INVAL;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_row before, after;
        struct efs_meta_writer_view view = {0};
        struct efs_fence_history again;
        struct efs_txn_pending pend = {0};
        int moved = 0;
        int rc = inode_read_tx(kv, ino, coord, ctx, &pend, &before);
        if (rc != EFS_OK)
            return rc;
        if (before.generation != gen)
            return EFS_ERR_STALE;
        if (!S_ISREG(before.mode))
            return EFS_ERR_INVAL;
        rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE,
                                  coord, ctx, &pend, &view.history);
        if (rc == EFS_OK)
            rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE,
                                      coord, ctx, &pend, &again);
        if (rc == EFS_OK)
            rc = inode_read_tx(kv, ino, coord, ctx, &pend, &after);
        if (rc == EFS_OK && coord)
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved || memcmp(&before, &after, sizeof(before)) ||
            memcmp(&view.history, &again, sizeof(again)))
            continue;
        view.ino = ino;
        view.generation = gen;
        view.authority_epoch = before.content_epoch;
        view.oldest_complete_epoch = before.content_epoch;
        /* Epochs advance by one per shrink. A gap may be a retired fence or
         * legacy physical truncate; neither supplies complete old history.
         * Only a contiguous suffix ending at the current stamp proves it. */
        for (uint32_t i = view.history.count; i > 0; --i) {
            uint64_t epoch = view.history.entries[i - 1].epoch;
            if (epoch > view.authority_epoch)
                return EFS_ERR_PROTO;
            if (epoch != view.oldest_complete_epoch)
                break;
            --view.oldest_complete_epoch;
        }
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

static int dent_get(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
                    const char *name, struct efs_meta_dentry *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[DENT_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = efs_kv_key_dentry(shard, parent, name, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_dentry(val, vlen, out));
}

int efs_meta_capture_dentry_drop(struct efs_kv *kv, const struct efs_meta_row *prow,
                               efs_ino_t parent, const char *name,
                               const struct efs_meta_dentry *expected,
                               struct efs_meta_dentry_drop *d)
{
    struct efs_meta_dentry tomb;
    int rc;

    if (!kv || !prow || !name || !expected || !d || !parent ||
        prow->ino != parent || prow->layout > EFS_META_LAYOUT_HASHED ||
        !expected->ino || expected->type == EFS_META_DENT_TOMBSTONE)
        return EFS_ERR_INVAL;
    memset(d, 0, sizeof(*d));
    d->psh = efs_kv_inode_shard(parent);
    d->hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(d->psh, parent, name, d->k_loc, &d->kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(d->hsh, parent, name, d->k_hash, &d->kh);
    if (rc != EFS_OK)
        return rc;
    d->del_loc = prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING && d->kl == d->kh &&
          memcmp(d->k_loc, d->k_hash, d->kl) == 0);
    d->put_tomb = prow->layout == EFS_META_LAYOUT_SPLITTING;
    d->del_hash = prow->layout == EFS_META_LAYOUT_HASHED;
    memset(&tomb, 0, sizeof(tomb));
    tomb.generation = prow->layout_epoch;
    tomb.type = EFS_META_DENT_TOMBSTONE;
    rc = efs_meta_pack_dentry(&tomb, d->v_tomb, sizeof(d->v_tomb));
    if (rc != EFS_OK)
        return rc;
    uint8_t wanted[EFS_META_DENT_BYTES];
    int matched = 0;
    rc = efs_meta_pack_dentry(expected, wanted, sizeof(wanted));
    if (rc != EFS_OK)
        return rc;
    for (unsigned int i = 0; i < 2; ++i) {
        uint8_t *key = i ? d->k_hash : d->k_loc;
        uint8_t *value = i ? d->hash_value : d->loc_value;
        uint32_t kl = i ? d->kh : d->kl;
        uint32_t *len = i ? &d->hash_len : &d->loc_len;
        if (i ? !(d->put_tomb || d->del_hash) : !d->del_loc)
            continue;
        *len = EFS_META_DENT_BYTES;
        rc = efs_kv_get(kv, key, kl, value, len);
        if (rc == EFS_ERR_NOT_FOUND) {
            *len = EFS_TXN_ABSENT;
        } else if (rc != EFS_OK) {
            return rc;
        } else if (*len == sizeof(wanted) && !memcmp(value, wanted, sizeof(wanted))) {
            matched = 1;
        } else {
            /* A hashed tombstone masks even a lingering local copy. */
            return EFS_ERR_STALE;
        }
    }
    /* At least one captured source must still name the originally read inode.
     * Absence or a replacement between lookup and this capture is STALE. */
    return matched ? EFS_OK : EFS_ERR_STALE;
}

/* As dent_get, and when the key is absent a COMMITted EXCL intent is the
 * value (I17: visible at the decision). A negative lookup pays one extra
 * local get of the intent key; the coordinator is asked only when an
 * intent is actually there. NULL coord = plain read. */
static int dent_get_tx(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
                       const char *name, efs_txn_coord_fn coord, void *ctx,
                       struct efs_meta_dentry *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[DENT_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = dent_get(kv, shard, parent, name, out);
    if (rc != EFS_ERR_NOT_FOUND || !coord)
        return rc;
    rc = efs_kv_key_dentry(shard, parent, name, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_txn_read(kv, key, klen, coord, ctx, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_dentry(val, vlen, out));
}

static int lookup_at(struct efs_kv *kv, efs_ino_t parent, const char *name,
                     efs_txn_coord_fn coord, void *ctx,
                     struct efs_meta_dentry *out)
{
    struct efs_meta_row prow;
    uint32_t hsh;
    int rc;

    if (!kv || !name || !out || parent == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode_tx(kv, parent, coord, ctx, &prow);
    if (rc == EFS_ERR_NOT_FOUND) {
        /* Parent row lives on another Raft group. Try hashed then local. */
        hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        rc = dent_get_tx(kv, hsh, parent, name, coord, ctx, out);
        if (rc == EFS_OK) {
            if (out->type == EFS_META_DENT_TOMBSTONE)
                return EFS_ERR_NOT_FOUND;
            return EFS_OK;
        }
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
        return dent_get_tx(kv, efs_kv_inode_shard(parent), parent, name,
                           coord, ctx, out);
    }
    if (rc != EFS_OK)
        return rc;
    return efs_meta_apply_lookup_row_tx(kv, &prow, name, coord, ctx, out);
}

int efs_meta_apply_lookup_row_tx(struct efs_kv *kv,
                                 const struct efs_meta_row *parent_row,
                                 const char *name, efs_txn_coord_fn coord,
                                 void *ctx, struct efs_meta_dentry *out)
{
    if (!kv || !parent_row || !parent_row->ino || !name || !out ||
        parent_row->layout > EFS_META_LAYOUT_HASHED)
        return EFS_ERR_INVAL;
    efs_ino_t parent = parent_row->ino;
    uint32_t hsh;
    int rc;
    if (parent_row->layout == EFS_META_LAYOUT_LOCAL)
        return dent_get_tx(kv, efs_kv_inode_shard(parent), parent, name,
                           coord, ctx, out);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = dent_get_tx(kv, hsh, parent, name, coord, ctx, out);
    if (rc == EFS_OK) {
        if (out->type == EFS_META_DENT_TOMBSTONE)
            return EFS_ERR_NOT_FOUND;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (parent_row->layout == EFS_META_LAYOUT_HASHED)
        return EFS_ERR_NOT_FOUND;
    return dent_get_tx(kv, efs_kv_inode_shard(parent), parent, name, coord,
                       ctx, out);
}

int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out)
{
    return lookup_at(kv, parent, name, NULL, NULL, out);
}

int efs_meta_apply_lookup_tx(struct efs_kv *kv, efs_ino_t parent,
                             const char *name, efs_txn_coord_fn coord,
                             void *ctx, struct efs_meta_dentry *out)
{
    return lookup_at(kv, parent, name, coord, ctx, out);
}

int efs_meta_apply_resolve(struct efs_kv *kv, efs_ino_t parent, const char *name,
                           struct efs_meta_dentry *dent, struct efs_meta_row *row)
{
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    rc = efs_meta_apply_lookup(kv, parent, name, &d);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, d.ino, &r);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_IO; /* I9: dentry without a row is not absence */
    if (rc != EFS_OK)
        return rc;
    if (dent)
        *dent = d;
    if (row)
        *row = r;
    return EFS_OK;
}

/* Handler-side resolve. A COMMITted-but-unresolved dentry or child row
 * is visible (lookup_tx / get_inode_tx). The apply path keeps
 * efs_meta_apply_resolve: it cannot ask a coordinator. Sep 23 freeze:
 * rmdir of d-fcstor011-1-22 returned ENOENT while the name existed,
 * because the creating txn's RESOLVE had not landed. */
int efs_meta_apply_resolve_tx(struct efs_kv *kv, efs_ino_t parent,
                              const char *name, efs_txn_coord_fn coord,
                              void *ctx, struct efs_meta_dentry *dent,
                              struct efs_meta_row *row)
{
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    rc = efs_meta_apply_lookup_tx(kv, parent, name, coord, ctx, &d);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode_tx(kv, d.ino, coord, ctx, &r);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_IO; /* I9: dentry without a row is not absence */
    if (rc != EFS_OK)
        return rc;
    if (dent)
        *dent = d;
    if (row)
        *row = r;
    return EFS_OK;
}

static void hop_from_row(struct efs_meta_path_hop *h, const struct efs_meta_row *r)
{
    h->ino = r->ino;
    h->generation = r->generation;
    h->mode = r->mode;
    h->uid = r->uid;
    h->gid = r->gid;
}

int efs_meta_apply_lookup_path(struct efs_kv *kv, efs_ino_t start,
                               const char *path, struct efs_meta_path_hop *hops,
                               uint32_t cap, uint32_t *n)
{
    struct efs_meta_row row;
    efs_ino_t cur;
    const char *p;
    uint32_t nh = 0;
    int rc;

    if (!kv || !path || !hops || !n || cap == 0)
        return EFS_ERR_INVAL;
    if (cap > EFS_META_PATH_MAX)
        cap = EFS_META_PATH_MAX;
    cur = start ? start : EFS_ROOT_INO;
    p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        rc = efs_meta_apply_get_inode(kv, cur, &row);
        if (rc != EFS_OK)
            return rc;
        hop_from_row(&hops[0], &row);
        *n = 1;
        return EFS_OK;
    }
    while (*p && nh < cap) {
        char name[EFS_MAX_NAME];
        size_t nlen;
        const char *s = p;
        struct efs_meta_row child;
        int more;

        while (*p && *p != '/')
            p++;
        nlen = (size_t)(p - s);
        if (nlen == 0)
            break;
        if (nlen >= EFS_MAX_NAME)
            return EFS_ERR_NAMETOOLONG;
        memcpy(name, s, nlen);
        name[nlen] = '\0';
        while (*p == '/')
            p++;
        rc = efs_meta_apply_resolve(kv, cur, name, NULL, &child);
        if (rc != EFS_OK)
            return rc;
        more = (*p != '\0');
        /* A file or symlink cannot be an intermediate: POSIX walks only
         * through directories, and following a symlink is the caller's
         * job after this returns. Stopping with the non-dir as a hop
         * would look like a successful batch the caller then resumes. */
        if (more && !S_ISDIR(child.mode))
            return EFS_ERR_INVAL;
        hop_from_row(&hops[nh], &child);
        nh++;
        cur = child.ino;
    }
    *n = nh;
    return nh ? EFS_OK : EFS_ERR_INVAL;
}

static int load_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next)
{
    uint8_t key[EFS_KV_KEY_MAX], val[ALLOC_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = efs_kv_key_alloc(shard, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc == EFS_ERR_NOT_FOUND) {
        *next = shard ? (efs_ino_t)shard : (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
        if (*next == EFS_ROOT_INO)
            *next += (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (vlen < ALLOC_VAL)
        return EFS_ERR_PROTO;
    *next = rd64(val);
    return EFS_OK;
}

/* A deleted inode can retain a transaction version even when its row is
 * absent. Reusing it as a generation-1 allocation both revives an old FileID
 * and makes cross-group MKDIR's CAS(0) fail forever. Treat that durable
 * history as occupied when a stale allocator watermark or hint reaches it. */
static int alloc_inode_unused(struct efs_kv *kv, efs_ino_t ino, int *unused)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t length = 0;
    uint64_t version = 0;
    int rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &length);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(kv, key, length, &version);
    if (rc == EFS_OK)
        *unused = version == 0;
    return rc;
}

/* Watermark can sit on a live row (SNAP lag, hollow replica, or a
 * rewind after unlink of a later sibling). Reusing that ino overwrites
 * the row and inherits leftover children — POSIX mkdir then EEXIST on
 * well-known names like p/c. Skip occupied inodes. */
#define ALLOC_SKIP_MAX 4096

static int alloc_next_free(struct efs_kv *kv, uint32_t shard, efs_ino_t *ino,
                           efs_ino_t *next_out)
{
    struct efs_meta_row row;
    efs_ino_t next;
    int rc, n;

    rc = load_alloc(kv, shard, &next);
    if (rc != EFS_OK)
        return rc;
    for (n = 0; n < ALLOC_SKIP_MAX; n++) {
        if (efs_kv_inode_shard(next) != shard) {
            next = shard ? (efs_ino_t)shard
                         : (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
            if (next == EFS_ROOT_INO)
                next += (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
        }
        rc = efs_meta_apply_get_inode(kv, next, &row);
        if (rc == EFS_ERR_NOT_FOUND) {
            int unused = 0;
            rc = alloc_inode_unused(kv, next, &unused);
            if (rc != EFS_OK)
                return rc;
            if (unused) {
                *ino = next;
                *next_out = next + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
                return EFS_OK;
            }
        }
        if (rc != EFS_OK)
            return rc;
        next += (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
    }
    return EFS_ERR_NOMEM;
}

/* Log-path allocation with a proposer-chosen HINT. The host picks `want`
 * on the proposing node from its APPLIED alloc watermark
 * (efs_meta_apply_peek_alloc) and packs it into the CREATE command; two
 * creates on one shard that peek before either applies (different voters,
 * or a forwarded create racing the leader's own) carry the SAME ino. The
 * old apply took `want` unconditionally, so the second PUT overwrote the
 * first file's inode row under the same key: two names → one row, both
 * writers publishing onto one ino (IO-500 ior-easy: 36 files, 10 distinct
 * inos, 76108 read-verify errors), and the first unlink left the other
 * name dangling NOT_FOUND. The apply is the allocator: honor the hint only
 * if it is still unallocated (>= watermark AND no row — the row check
 * covers a SNAP-lag rewound watermark), else take the next free ino.
 * Deterministic on every replica of the group (same log, same shard
 * state). Callers read the ino back from the dentry, never from the hint. */
static int alloc_hint_or_next(struct efs_kv *kv, uint32_t shard,
                              efs_ino_t want, efs_ino_t *ino,
                              efs_ino_t *next_out)
{
    struct efs_meta_row row;
    efs_ino_t next;
    int rc;

    if (efs_kv_inode_shard(want) != shard)
        return EFS_ERR_PROTO;
    rc = load_alloc(kv, shard, &next);
    if (rc != EFS_OK)
        return rc;
    if (want >= next) {
        rc = efs_meta_apply_get_inode(kv, want, &row);
        if (rc == EFS_ERR_NOT_FOUND) {
            int unused = 0;
            rc = alloc_inode_unused(kv, want, &unused);
            if (rc != EFS_OK)
                return rc;
            if (unused) {
                *ino = want;
                *next_out = want + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
                return EFS_OK;
            }
        }
        if (rc != EFS_OK)
            return rc;
    }
    return alloc_next_free(kv, shard, ino, next_out);
}

/* The per-shard ALLOC key is written by two paths: this log path (file
 * CREATE, same-group MKDIR) and the host's txn path (cross-group MKDIR,
 * hashed CREATE), which PREPAREs an EXCL intent on it at the version it
 * read and PUTs the child row + new watermark at resolve. The txn treats
 * the intent as its lock and the version as its CAS; the log path honored
 * neither, so the two allocators raced in both orders: (1) intent pending →
 * the log-path create took the same ino and the txn's resolve overwrote the
 * row; (2) log-path alloc write unversioned → a txn that peeked BEFORE it
 * still PREPAREd at the old version and won. posix names_* : file `cafeé`
 * (parent shard 650) and dir `aaaa…` (mkdir_shard 650) both got ino 4746,
 * the dir's unlink deleted the row, the file dangled (the old
 * `cwi-fail: ino_dup` family). Rule now: a log-path alloc under a pending
 * intent is BUSY (the txn rule — "PREPARE is no-wait, conflict → BUSY";
 * the client retries), and every log-path alloc write bumps the key's
 * version in the same batch so an outdated PREPARE is STALE, exactly as a
 * resolve would. Deterministic: intents and versions are shard state of
 * the same group's log. */
static int alloc_key_claim(struct efs_kv *kv, const uint8_t *k_alloc,
                           uint32_t ka, uint8_t *k_ver, uint32_t *kver,
                           uint8_t v_ver[8])
{
    uint8_t ik[EFS_KV_KEY_MAX], buf[8];
    uint32_t il = 0, n = 0;
    uint64_t ver = 0;
    int rc;

    rc = efs_kv_key_intent(k_alloc, ka, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &n); /* probe: found → INVAL */
    if (rc == EFS_OK || rc == EFS_ERR_INVAL)
        return EFS_ERR_BUSY;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    *kver = 0;
    rc = efs_kv_key_ver(k_alloc, ka, k_ver, kver);
    if (rc != EFS_OK)
        return rc;
    n = 8;
    rc = efs_kv_get(kv, k_ver, *kver, buf, &n);
    if (rc == EFS_OK && n >= 8)
        ver = rd64(buf);
    else if (rc != EFS_ERR_NOT_FOUND && rc != EFS_OK)
        return rc;
    be64(v_ver, ver + 1);
    return EFS_OK;
}

/* Op-id window (I16, §7.9) for a directory op lives on the shard that
 * served it — the dentry shard — under efs_kv_key_opid(shard, uuid,
 * epoch). A zero UUID / seq 0 means "no identity": only server-internal
 * work (expiry) proposes such entries. Client RPCs without a session are
 * refused at the host boundary (K5), and every apply that takes an identity
 * checks it with efs_session_accept. */
static int opid_hosted(const struct efs_opid *op)
{
    uint32_t i;

    if (!op || op->seq == 0)
        return 0;
    for (i = 0; i < EFS_OPID_UUID_LEN; i++) {
        if (op->client_uuid[i])
            return 1;
    }
    return 0;
}

static int opid_get(struct efs_kv *kv, uint32_t shard, const struct efs_opid *id,
                    uint8_t *key, uint32_t *klen, uint8_t *val, uint32_t *vlen)
{
    int rc;

    rc = efs_kv_key_opid(shard, id->client_uuid, id->session_epoch, key, klen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, *klen, val, vlen);
    if (rc == EFS_ERR_NOT_FOUND) {
        *vlen = 0;
        return EFS_OK;
    }
    return rc;
}

int efs_meta_apply_opid_probe(struct efs_kv *kv, uint32_t shard,
                              const struct efs_opid *id,
                              struct efs_opid_reply *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_OPID_VAL_MAX];
    uint32_t klen = 0, vlen = sizeof(val);
    int rc;

    if (!kv || !opid_hosted(id))
        return 0;
    rc=efs_session_accept(kv,shard,id->client_uuid,id->session_epoch);
    if(rc!=EFS_OK)return rc;
    rc = opid_get(kv, shard, id, key, &klen, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_opid_probe(val, vlen, id, out);
}

/* Fill the batch item that records `rep` for q in the window on `shard`.
 * 1 = item filled; 0 = nothing to record (no identity, or the window has
 * no room — the op still applies); negative = KV error. */
static int opid_item(struct efs_kv *kv, uint32_t shard,
                     const struct efs_opid_req *q, const struct efs_opid_reply *rep,
                     uint8_t *key, uint8_t *val, struct efs_kv_item *it)
{
    uint8_t cur[EFS_OPID_VAL_MAX];
    uint32_t klen = 0, cn = sizeof(cur), vlen = EFS_OPID_VAL_MAX;
    int rc;

    if (!q || !opid_hosted(&q->id))
        return 0;
    rc=efs_session_accept(kv,shard,q->id.client_uuid,q->id.session_epoch);
    if(rc!=EFS_OK)return rc;
    rc = opid_get(kv, shard, &q->id, key, &klen, cur, &cn);
    if (rc != EFS_OK)
        return rc;
    rc = efs_opid_fold(cur, cn, q->id.client_uuid, q->id.session_epoch, q, rep,
                       val, &vlen);
    if (rc == EFS_ERR_NOMEM)
        return 0;
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(*it));
    it->op = EFS_KV_PUT;
    it->key = key;
    it->klen = klen;
    it->val = val;
    it->vlen = vlen;
    return 1;
}

int efs_meta_apply_mkfs(struct efs_kv *kv, uint64_t now, uint64_t salt)
{
    struct efs_meta_row root;
    uint8_t k_ino[EFS_KV_KEY_MAX], k_alloc[EFS_KV_KEY_MAX];
    uint8_t k_ex[EFS_KV_KEY_MAX], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_ex[8];
    uint32_t lk = 0, ak = 0, ek = 0;
    struct efs_kv_item it[3];
    uint32_t shard = efs_kv_inode_shard(EFS_ROOT_INO);
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &root);
    if (rc == EFS_OK)
        return EFS_OK;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    memset(&root, 0, sizeof(root));
    root.ino = EFS_ROOT_INO;
    root.generation = 1;
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;
    root.parent = EFS_ROOT_INO;
    root.base_mtime = now;
    root.base_atime = now;
    root.base_ctime = now;
    pack_inode(v_ino, &root);
    be64(v_alloc, EFS_ROOT_INO + (efs_ino_t)(1u << EFS_KV_SHARD_BITS));
    be64(v_ex, salt);
    rc = efs_kv_key_inode(shard, EFS_ROOT_INO, k_ino, &lk);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(shard, k_alloc, &ak);
    if (rc == EFS_OK)
        rc = efs_kv_key_export(shard, k_ex, &ek);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_PUT;
    it[0].key = k_ino;
    it[0].klen = lk;
    it[0].val = v_ino;
    it[0].vlen = INO_VAL;
    it[1].op = EFS_KV_PUT;
    it[1].key = k_alloc;
    it[1].klen = ak;
    it[1].val = v_alloc;
    it[1].vlen = ALLOC_VAL;
    it[2].op = EFS_KV_PUT;
    it[2].key = k_ex;
    it[2].klen = ek;
    it[2].val = v_ex;
    it[2].vlen = 8;
    return meta_write_batch(kv, it, 3);
}

int efs_meta_apply_init(struct efs_kv *kv, uint64_t now)
{
    return efs_meta_apply_mkfs(kv, now, 0);
}

/* The salt record lives on the ROOT shard's export key (written by MKFS,
 * group 0) AND on the even group's anchor shard (written by
 * EFS_MD_CMD_SALT at mkfs time). A node hosting only the even group never
 * applies MKFS, so without the second record its salt read returned 0 and
 * every salted placement (MKDIR scatter) diverged — apply PROTO skips,
 * missing rows, later EIO. Read root anchor first, even anchor second. */
int efs_meta_apply_export_salt(struct efs_kv *kv, uint64_t *out)
{
    uint8_t k[EFS_KV_KEY_MAX], v[8];
    uint32_t kl = 0, vl = 8;
    uint32_t shard = efs_kv_inode_shard(EFS_ROOT_INO);
    int rc;

    if (!kv || !out)
        return EFS_ERR_INVAL;
    *out = 0;
    rc = efs_kv_key_export(shard, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, kl, v, &vl);
    if (rc == EFS_ERR_NOT_FOUND) {
        uint32_t kl2 = 0, vl2 = 8;

        rc = efs_kv_key_export(efs_kv_anchor_shard(2), k, &kl2);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_get(kv, k, kl2, v, &vl2);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_ERR_NOT_FOUND; /* no record on either anchor */
        if (rc != EFS_OK)
            return rc;
        vl = vl2;
    } else if (rc != EFS_OK)
        return rc;
    if (vl < 8)
        return EFS_ERR_PROTO;
    *out = rd64(v);
    return EFS_OK;
}

/* EFS_MD_CMD_SALT apply: write the export-salt record on the given anchor
 * shard. Idempotent — an existing record must MATCH (placement is derived
 * from the salt; silently overwriting would fork the namespace). */
int efs_meta_apply_salt_record(struct efs_kv *kv, uint32_t anchor,
                               uint64_t salt)
{
    uint8_t k[EFS_KV_KEY_MAX], v[8];
    uint32_t kl = 0, vl = 8;
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_export(anchor, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, kl, v, &vl);
    if (rc == EFS_OK) {
        if (vl >= 8 && rd64(v) == salt)
            return EFS_OK;
        return EFS_ERR_PROTO;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    be64(v, salt);
    return meta_write_put(kv, k, kl, v, 8);
}

int efs_meta_membership_get(struct efs_kv *kv, uint32_t anchor, uint8_t *out,
                            uint32_t cap, uint32_t *len)
{
    uint8_t k[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    int rc;

    if (!kv || !out || !len)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_membership(anchor, k, &kl);
    if (rc != EFS_OK)
        return rc;
    *len = cap;
    return efs_kv_get(kv, k, kl, out, len);
}

int efs_meta_apply_membership_record(struct efs_kv *kv, uint32_t anchor,
                                     const uint8_t *rec, uint32_t len, int mode)
{
    uint8_t k[EFS_KV_KEY_MAX], have[EFS_MEMBERSHIP_MAX_LEN];
    uint32_t kl = 0, hl = 0;
    struct efs_membership want, cur;
    int rc;

    if (!kv || !rec || !len || len > EFS_MEMBERSHIP_MAX_LEN ||
        efs_membership_decode(rec, len, &want) != EFS_OK)
        return EFS_ERR_INVAL;
    rc = efs_meta_membership_get(kv, anchor, have, sizeof(have), &hl);
    if (rc == EFS_OK) {
        if (mode == EFS_MEMBERSHIP_KEEP_FIRST)
            return EFS_OK;
        if (efs_membership_decode(have, hl, &cur) != EFS_OK)
            return EFS_ERR_PROTO;
        if (efs_membership_extends(&cur, &want))
            return EFS_OK;
        if (!efs_membership_extends(&want, &cur) ||
            (mode == EFS_MEMBERSHIP_EXTEND_ONE && want.count != cur.count + 1))
            return EFS_ERR_FENCED;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    } else if (mode == EFS_MEMBERSHIP_EXTEND_ONE) {
        return EFS_ERR_UNFORMED;
    }
    rc = efs_kv_key_membership(anchor, k, &kl);
    if (rc != EFS_OK)
        return rc;
    return meta_write_put(kv, k, kl, rec, len);
}

int efs_meta_pack_inode(const struct efs_meta_row *r, uint8_t *out, uint32_t cap)
{
    if (!r || !out || cap < INO_VAL)
        return EFS_ERR_INVAL;
    pack_inode(out, r);
    return EFS_OK;
}

int efs_meta_unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r)
{
    return unpack_inode(p, n, r);
}

int efs_meta_pack_dentry(const struct efs_meta_dentry *d, uint8_t *out,
                         uint32_t cap)
{
    if (!d || !out || cap < DENT_VAL)
        return EFS_ERR_INVAL;
    pack_dentry(out, d);
    return EFS_OK;
}

int efs_meta_unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d)
{
    return unpack_dentry(p, n, d);
}

int efs_meta_apply_peek_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next)
{
    efs_ino_t ino, nxt;
    int rc;

    if (!kv || !next)
        return EFS_ERR_INVAL;
    rc = alloc_next_free(kv, shard, &ino, &nxt);
    if (rc != EFS_OK)
        return rc;
    *next = ino;
    return EFS_OK;
}

/* Resolve nearest explicit policy under one participant's log. This staged
 * helper refuses another group's ancestors, rather than reading follower
 * state as policy authority. Cross-group inheritance needs a guarded txn. */
int efs_meta_apply_create_refusal(struct efs_kv *kv,uint32_t shard,
                                  const struct efs_opid_req *q)
{
    if(!q || !efs_opid_req_valid(q) || !opid_hosted(&q->id) ||
       shard>EFS_KV_SHARD_MASK)return EFS_ERR_INVAL;
    struct efs_opid_reply reply;int rc=efs_meta_apply_opid_probe(kv,shard,&q->id,&reply);
    if(rc<0)return rc;
    if(rc)return reply.rc;
    rc=efs_session_accept(kv,shard,q->id.client_uuid,q->id.session_epoch);
    if(rc!=EFS_OK)return rc;
    uint8_t k[EFS_KV_KEY_MAX],v[EFS_OPID_VAL_MAX];struct efs_kv_item item;
    memset(&reply,0,sizeof(reply));reply.rc=EFS_ERR_EXIST;
    rc=opid_item(kv,shard,q,&reply,k,v,&item);
    if(rc<0)return rc;
    if(rc!=1)return EFS_ERR_BUSY;
    rc=meta_write_batch(kv,&item,1);return rc==EFS_OK?EFS_ERR_EXIST:rc;
}

static int expiry_local_default(struct efs_kv *kv, efs_ino_t parent,
                                 uint8_t group, uint64_t now,
                                 struct efs_expiry_record *out)
{
    efs_ino_t seen[EFS_META_PATH_MAX];uint32_t count=0;
    memset(out,0,sizeof(*out));
    while(parent) {
        if(count==EFS_META_PATH_MAX)return EFS_ERR_OVERFLOW;
        for(uint32_t i=0;i<count;i++)if(seen[i]==parent)return EFS_ERR_IO;
        seen[count++]=parent;
        if(efs_raft_shard_group(efs_kv_inode_shard(parent))!=group)
            return EFS_ERR_INVAL;
        struct efs_meta_row row;struct efs_expiry_policy policy;
        uint8_t k[EFS_KV_KEY_MAX];uint32_t kn=0;
        int rc=efs_kv_key_inode(efs_kv_inode_shard(parent),parent,k,&kn);
        if(rc==EFS_OK)rc=efs_txn_key_busy(kv,k,kn);
        if(rc==EFS_OK)rc=efs_meta_apply_get_inode(kv,parent,&row);
        if(rc!=EFS_OK)return rc;
        if(!S_ISDIR(row.mode) || !row.nlink)return EFS_ERR_NOT_FOUND;
        efs_expiry_policy_key(parent,row.generation,k);
        rc=efs_txn_key_busy(kv,k,EFS_EXPIRY_KEY_BYTES);
        if(rc==EFS_OK)rc=efs_expiry_policy_get(kv,parent,row.generation,&policy);
        if(rc!=EFS_OK)return rc;
        if(policy.revision)
            return efs_expiry_materialize(&policy,parent,row.generation,now,out);
        if(parent==EFS_ROOT_INO)return EFS_OK;
        parent=row.parent;
    }
    return EFS_ERR_IO;
}

static int create_file_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                             efs_ino_t parent, uint32_t mode,
                             const char *name, const struct efs_opid_req *q,
                             int from_log, efs_ino_t want_ino, int want_layout,
                             const uint8_t *lease_uuid, uint32_t lease_epoch,
                             uint64_t lease_token, int expiry_enabled, efs_ino_t *out)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, parent_row;
    struct efs_opid_reply rep;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_opid[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_opid[EFS_OPID_VAL_MAX];
    uint32_t kd = 0, ki = 0, ka = 0;
    struct efs_kv_item it[14];
    struct efs_expiry_change expiry_change;
    struct efs_expiry_record expiry_record;
    int has_expiry=0;
    uint8_t k_lease[EFS_KV_KEY_MAX], lease_one = 1;
    uint8_t lease_pending[EFS_LEASE_PENDING_VALUE_LEN];
    uint32_t lease_len = 0;
    uint32_t n = 0, shard;
    efs_ino_t next = 0, ino;
    uint8_t k_aver[EFS_KV_KEY_MAX], v_aver[8];
    uint32_t kav = 0;
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t kp = 0, kln = 0, ks = 0;
    uint8_t dseq_lane;
    uint64_t bit;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;
    int remote_parent = 0;

    if (!kv || !name || !at || parent == 0)
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) == S_IFDIR)
        return EFS_ERR_INVAL; /* MKDIR is a 2-shard txn; not this helper */
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc == EFS_ERR_NOT_FOUND) {
        uint8_t pgrp = efs_raft_shard_group(efs_kv_inode_shard(parent));
        uint32_t hdsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        int hashed = (pgrp != efs_raft_shard_group(hdsh));
        int lay;

        /* A committed log entry must write the child even when this replica
         * lacks the parent row (follower lag / dual-host apply). A helper
         * call still treats same-group missing parent as a hole.
         * Do not guess HASHED from a missing parent on the log path — a
         * dual-host replica that can see a LOCAL parent would write
         * different keys. Packed layout/ino from the leader wins. */
        if (from_log)
            lay = want_layout >= 0 ? want_layout : EFS_META_LAYOUT_LOCAL;
        else if (hashed)
            lay = EFS_META_LAYOUT_HASHED;
        else
            lay = -1;
        if (lay >= 0) {
            memset(&parent_row, 0, sizeof(parent_row));
            parent_row.ino = parent;
            parent_row.generation = 1;
            parent_row.mode = S_IFDIR | 0755;
            parent_row.layout = (uint8_t)lay;
            parent_row.used_shards = (lay == EFS_META_LAYOUT_LOCAL)
                                         ? 0
                                         : ~(uint64_t)0;
            remote_parent = 1;
            rc = EFS_OK;
        }
    }
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(parent_row.mode))
        return EFS_ERR_INVAL;

    {
        uint8_t lay = want_layout >= 0 ? (uint8_t)want_layout
                                       : parent_row.layout;

        shard = efs_kv_dentry_shard(parent, name, lay);
        dseq_lane = lay == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    }
    /* Lease ownership (pending or CREATE-time open) is a session mutation,
     * including negative verdicts and replays. Check inside serialized apply,
     * before the replay fast path: an earlier local fence must prevent any
     * old-epoch CREATE claim. */
    if (lease_uuid) {
        rc = efs_session_accept(kv, shard, lease_uuid, lease_epoch);
        if (rc != EFS_OK) return rc;
    }
    /* I16: a replay of an op this window already recorded returns the
     * recorded verdict and touches nothing. */
    if (q) {
        int hit = efs_meta_apply_opid_probe(kv, shard, &q->id, &rep);

        if (hit < 0)
            return hit;
        if (hit) {
            if (lease_token && rep.rc == EFS_OK && !rep.ino)
                return EFS_ERR_STALE; /* retired reply cannot grant adoption */
            if (out)
                *out = rep.ino;
            return rep.rc;
        }
    }

    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc == EFS_OK) {
        if (q && opid_hosted(&q->id)) {
            /* A CREATE answered EEXIST must not become a successful new
             * CREATE when a delayed retry arrives after the name is removed.
             * Publish the negative verdict without namespace/lease effects. */
            memset(&rep, 0, sizeof(rep));
            rep.rc = EFS_ERR_EXIST;
            rc = opid_item(kv, shard, q, &rep, k_opid, v_opid, &it[0]);
            if (rc < 0) return rc;
            if (rc != 1) return EFS_ERR_BUSY;
            rc = meta_write_batch(kv, it, 1);
            if (rc != EFS_OK) return rc;
        }
        return EFS_ERR_EXIST;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (want_ino) {
        rc = alloc_hint_or_next(kv, shard, want_ino, &ino, &next);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = alloc_next_free(kv, shard, &ino, &next);
        if (rc != EFS_OK)
            return rc;
        if (efs_kv_inode_shard(ino) != shard)
            return EFS_ERR_PROTO;
    }

    memset(&row, 0, sizeof(row));
    row.ino = ino;
    row.generation = 1;
    row.mode = mode;
    row.nlink = 1;
    row.parent = parent;
    row.uid = at->uid;
    row.gid = at->gid;
    row.base_mtime = at->now;
    row.base_atime = at->now;
    row.base_ctime = at->now;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = row.generation;
    dent.type = mode & S_IFMT;
    if(expiry_enabled && S_ISREG(mode)) {
        rc=expiry_local_default(kv,parent,efs_raft_shard_group(shard),
                                 at->now,&expiry_record);
        if(rc!=EFS_OK)return rc;
        if(expiry_record.deadline_ns) {
            expiry_record.parent=parent;
            memcpy(expiry_record.name,name,strlen(name)+1);
            rc=efs_expiry_plan(kv,ino,row.generation,0,&expiry_record,&expiry_change);
            if(rc!=EFS_OK)return rc;
            has_expiry=1;
        }
    }
    pack_inode(v_ino, &row);
    pack_dentry(v_dent, &dent);
    be64(v_alloc, next);

    rc = efs_kv_key_dentry(shard, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(shard, ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(shard, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = alloc_key_claim(kv, k_alloc, ka, k_aver, &kav, v_aver);
    if (rc != EFS_OK)
        return rc;

    /* POSIX: adding an entry moves the containing directory's mtime and
     * ctime. For a LOCAL directory the parent row is on this very shard, so
     * it rides the same atomic batch and costs nothing. For a spread
     * directory it deliberately does NOT go here — routing every create back
     * to the parent's shard is the hotspot the spread exists to remove, so
     * those times live in a per-dentry-shard dir lane (§7.4). */
    if (remote_parent) {
        /* Parent row is on another Raft group. Write child + hashed
         * dentry only — a fabricated parent PUT would pollute this
         * namespace, and a lane stamp needs the real generation. */
    } else if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        efs_meta_dir_note_entry(&parent_row, 1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&parent_row);
        bit = 1ull << efs_kv_dir_lane(name);
        if ((parent_row.used_shards & bit) == 0) {
            parent_row.used_shards |= bit;
            touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, &parent_row, name, at->now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &parent_row);
        rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, &kp);
        if (rc != EFS_OK)
            return rc;
    }

    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_alloc;
    it[n].klen = ka;
    it[n].val = v_alloc;
    it[n].vlen = ALLOC_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_aver;
    it[n].klen = kav;
    it[n].val = v_aver;
    it[n].vlen = 8;
    n++;
    if (touch_parent) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    rc = dseq_bump(kv, shard, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;

    if (q) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = ino;
        rc = opid_item(kv, shard, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        if (opid_hosted(&q->id) && rc != 1)
            return EFS_ERR_BUSY; /* never create without its replay verdict */
        n += (uint32_t)rc;
    }

    if (lease_uuid) {
        rc = efs_kv_key_lease(shard, ino, row.generation, lease_uuid,
                               lease_epoch, k_lease, &lease_len);
        if (rc != EFS_OK) return rc;
        if (lease_token) {
            rc = efs_lease_pending_value(lease_token, lease_pending);
            if (rc != EFS_OK) return rc;
        }
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_lease, lease_len,
                         lease_token ? lease_pending : &lease_one,
                         lease_token ? EFS_LEASE_PENDING_VALUE_LEN : 1};
    }

    if(has_expiry) {
        if(n+expiry_change.count>sizeof(it)/sizeof(it[0]))return EFS_ERR_OVERFLOW;
        for(uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
    }
    rc = meta_write_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = ino;
    return EFS_OK;
}

int efs_meta_apply_create_file(struct efs_kv *kv, const struct efs_meta_attrs *at,
                               efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, 0, 0, -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  const struct efs_meta_attrs *at,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out)
{
    struct efs_opid_req q;

    if (!op)
        return EFS_ERR_INVAL;
    memset(&q, 0, sizeof(q));
    q.id = *op;
    return create_file_batch(kv, at, parent, mode, name, &q, 0, 0, -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_log(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                   efs_ino_t parent, uint32_t mode,
                                   const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, 1, 0, -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_log_op(struct efs_kv *kv,
                                      const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      int layout, const struct efs_opid_req *q,
                                      efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, q, 1, ino,
                             ino ? layout : -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_log_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      uint8_t layout, efs_ino_t *out)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, NULL, 1, ino,
                             (int)layout, NULL, 0, 0, 0, out);
}

/* Namespace, inode, replay verdict and initial lease share one KV batch.
 * Replaying an already recorded op never recreates a subsequently closed lease. */
int efs_meta_apply_create_file_log_lease(struct efs_kv *kv,
                                         const struct efs_meta_attrs *at,
                                         efs_ino_t parent, uint32_t mode,
                                         const char *name, efs_ino_t ino,
                                         int layout, const struct efs_opid_req *q,
                                         const uint8_t *uuid, uint32_t epoch,
                                         efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, q, 1, ino,
                             ino ? layout : -1, uuid, epoch, 0, 0, out);
}

/* Pending ownership: identity and token come from the same durable CREATE
 * request. The opt-in daemon form requires an established shard session;
 * FUSE adoption remains staged. Replay never reacquires ownership. */
int efs_meta_apply_create_file_log_pending(struct efs_kv *kv,
                                         const struct efs_meta_attrs *at,
                                         efs_ino_t parent, uint32_t mode,
                                         const char *name, efs_ino_t ino,
                                         int layout, const struct efs_opid_req *q,
                                         const uint8_t *uuid, uint32_t epoch,
                                         efs_ino_t *out)
{
    if (!q || !efs_opid_req_valid(q) || q->ack >= q->id.seq || !uuid || !epoch ||
        q->id.session_epoch != epoch ||
        memcmp(q->id.client_uuid, uuid, EFS_OPID_UUID_LEN) != 0)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, q, 1, ino,
                             ino ? layout : -1, uuid, epoch, q->id.seq, 0, out);
}

int efs_meta_apply_create_expiring(struct efs_kv *kv,
                                     const struct efs_meta_attrs *at,
                                     efs_ino_t parent,uint32_t mode,
                                     const char *name,efs_ino_t *out)
{
    return create_file_batch(kv,at,parent,mode,name,NULL,0,0,-1,NULL,0,0,1,out);
}

static int mkdir_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                       efs_ino_t parent, uint32_t mode, const char *name,
                       int from_log, efs_ino_t want_ino, int want_layout,
                       const struct efs_opid_req *q, efs_ino_t *out)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, parent_row;
    struct efs_opid_reply rep;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t k_opid[EFS_KV_KEY_MAX], v_opid[EFS_OPID_VAL_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_par[INO_VAL], v_ln[LANE_VAL], v_dseq[8];
    uint32_t kd = 0, ki = 0, ka = 0, kp = 0, kln = 0, ks = 0;
    uint8_t k_aver[EFS_KV_KEY_MAX], v_aver[8];
    uint32_t kav = 0;
    struct efs_kv_item it[10];
    uint32_t n = 0, psh, csh, dsh;
    efs_ino_t next = 0, ino;
    uint64_t salt = 0, bit;
    uint8_t dseq_lane, lay;
    int rc;
    int stamp_lane = 0;
    int remote_parent = 0;
    int touch_parent = 0;

    if (!kv || !name || !at || parent == 0 || name[0] == '\0')
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) != S_IFDIR)
        return EFS_ERR_INVAL;
    if ((mode & 07777) == 0)
        mode |= 0755;
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc == EFS_ERR_NOT_FOUND && from_log) {
        memset(&parent_row, 0, sizeof(parent_row));
        parent_row.ino = parent;
        parent_row.generation = 1;
        parent_row.mode = S_IFDIR | 0755;
        parent_row.nlink = 2;
        lay = want_layout >= 0 ? (uint8_t)want_layout : EFS_META_LAYOUT_LOCAL;
        parent_row.layout = lay;
        parent_row.used_shards = (lay == EFS_META_LAYOUT_LOCAL)
                                     ? 0
                                     : ~(uint64_t)0;
        remote_parent = 1;
        rc = EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(parent_row.mode))
        return EFS_ERR_INVAL;
    if (parent_row.layout != EFS_META_LAYOUT_LOCAL &&
        parent_row.layout != EFS_META_LAYOUT_HASHED &&
        parent_row.layout != EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_INVAL;
    lay = want_layout >= 0 ? (uint8_t)want_layout : parent_row.layout;
    dsh = efs_kv_dentry_shard(parent, name, lay);
    if (q) {
        int hit = efs_meta_apply_opid_probe(kv, dsh, &q->id, &rep);

        if (hit < 0)
            return hit;
        if (hit) {
            if (out)
                *out = rep.ino;
            return rep.rc;
        }
    }
    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    rc = efs_meta_apply_export_salt(kv, &salt);
    if (rc != EFS_OK)
        return rc;
    psh = efs_kv_inode_shard(parent);
    csh = efs_kv_mkdir_shard(parent, name, salt);
    /* A committed CREATE records the chosen FileID. Replay older scattered
     * allocations exactly, rather than relocating an inode or selecting a
     * different offset on a follower. Reject unrelated shard hints below. */
    if (from_log && want_ino && efs_kv_inode_shard(want_ino) != csh &&
        efs_kv_inode_shard(want_ino) ==
            efs_kv_mkdir_scattered_shard(parent, name, salt))
        csh = efs_kv_inode_shard(want_ino);
    dseq_lane = lay == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    if (want_ino) {
        rc = alloc_hint_or_next(kv, csh, want_ino, &ino, &next);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = alloc_next_free(kv, csh, &ino, &next);
        if (rc != EFS_OK)
            return rc;
        if (efs_kv_inode_shard(ino) != csh)
            return EFS_ERR_PROTO;
    }

    memset(&row, 0, sizeof(row));
    row.ino = ino;
    row.generation = 1;
    row.mode = mode;
    row.nlink = 2;
    row.parent = parent;
    row.uid = at->uid;
    row.gid = at->gid;
    row.base_mtime = at->now;
    row.base_atime = at->now;
    row.base_ctime = at->now;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = 1;
    dent.type = S_IFDIR;
    if (remote_parent) {
        /* Hollow follower: do not PUT a fabricated parent. */
    } else if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.nlink++;
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        efs_meta_dir_note_entry(&parent_row, 1);
        touch_parent = 1;
    } else {
        parent_row.nlink++;
        bit = 1ull << efs_kv_dir_lane(name);
        if ((parent_row.used_shards & bit) == 0)
            parent_row.used_shards |= bit;
        rc = dir_lane_stamp(kv, &parent_row, name, at->now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
        touch_parent = 1;
    }
    pack_inode(v_ino, &row);
    pack_dentry(v_dent, &dent);
    be64(v_alloc, next);
    rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = alloc_key_claim(kv, k_alloc, ka, k_aver, &kav, v_aver);
    if (rc == EFS_OK && touch_parent) {
        pack_inode(v_par, &parent_row);
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    }
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_alloc;
    it[n].klen = ka;
    it[n].val = v_alloc;
    it[n].vlen = ALLOC_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_aver;
    it[n].klen = kav;
    it[n].val = v_aver;
    it[n].vlen = 8;
    n++;
    if (touch_parent) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    rc = dseq_bump(kv, dsh, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    if (q) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = ino;
        rc = opid_item(kv, dsh, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    rc = meta_write_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = ino;
    return EFS_OK;
}

int efs_meta_apply_mkdir(struct efs_kv *kv, const struct efs_meta_attrs *at,
                         efs_ino_t parent, uint32_t mode, const char *name,
                         efs_ino_t *out)
{
    return mkdir_batch(kv, at, parent, mode, name, 0, 0, -1, NULL, out);
}

int efs_meta_apply_mkdir_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                            efs_ino_t parent, uint32_t mode, const char *name,
                            efs_ino_t ino, uint8_t layout, efs_ino_t *out)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return mkdir_batch(kv, at, parent, mode, name, 1, ino, (int)layout, NULL,
                       out);
}

int efs_meta_apply_mkdir_log_op(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                efs_ino_t parent, uint32_t mode, const char *name,
                                efs_ino_t ino, int layout,
                                const struct efs_opid_req *q, efs_ino_t *out)
{
    /* Without a leader-chosen ino this is the plain (non-log) mkdir shape
     * the host used before; the log-only parent stand-in needs the layout. */
    return mkdir_batch(kv, at, parent, mode, name, ino ? 1 : 0, ino,
                       ino ? layout : -1, q, out);
}

/* Blob: u32 count, then count times u16 nlen, u32 vlen, name, value.
 * One key per inode. Empty means the key is absent, not a zero count. */

static int xattr_check_blob(const uint8_t *b, uint32_t n, uint32_t *count)
{
    uint32_t c, pos, i;

    if (n == 0) {
        *count = 0;
        return EFS_OK;
    }
    if (n < 4 || n > EFS_XATTR_BLOB_MAX)
        return EFS_ERR_PROTO;
    c = efs_load_be32(b);
    pos = 4;
    for (i = 0; i < c; i++) {
        uint16_t nl;
        uint32_t vl, step;

        if (pos > n || n - pos < 6)
            return EFS_ERR_PROTO;
        nl = rd16(b + pos);
        vl = efs_load_be32(b + pos + 2);
        if (nl == 0 || nl > EFS_XATTR_NAME_MAX || vl > EFS_XATTR_VALUE_MAX)
            return EFS_ERR_PROTO;
        step = 6u + (uint32_t)nl + vl;
        if (step < 6 || n - pos < step)
            return EFS_ERR_PROTO;
        pos += step;
    }
    if (pos != n)
        return EFS_ERR_PROTO;
    *count = c;
    return EFS_OK;
}

static int xattr_load(struct efs_kv *kv, efs_ino_t ino, uint8_t *k, uint32_t *kl,
                      uint8_t *blob, uint32_t *blen)
{
    int rc;

    rc = efs_kv_key_xattr(efs_kv_inode_shard(ino), ino, k, kl);
    if (rc != EFS_OK)
        return rc;
    *blen = EFS_XATTR_BLOB_MAX;
    rc = efs_kv_get(kv, k, *kl, blob, blen);
    if (rc == EFS_ERR_NOT_FOUND) {
        *blen = 0;
        return EFS_OK;
    }
    return rc;
}

static int blob_put(uint8_t *dst, uint32_t cap, uint32_t *dp, const uint8_t *name,
                    uint16_t nlen, const uint8_t *val, uint32_t vlen)
{
    uint32_t need = 6u + (uint32_t)nlen + vlen;

    if (need < 6 || *dp > cap || cap - *dp < need)
        return EFS_ERR_INVAL;
    be16(dst + *dp, nlen);
    efs_store_be32(dst + *dp + 2, vlen);
    memcpy(dst + *dp + 6, name, nlen);
    if (vlen)
        memcpy(dst + *dp + 6 + nlen, val, vlen);
    *dp += need;
    return EFS_OK;
}

/* Fill one DEL when this inode has an xattr blob. A miss is not a
 * tombstone — every unlink would otherwise plant a key the file never
 * had. *added is 1 when it was filled. */
static int xattr_del_item(struct efs_kv *kv, efs_ino_t ino, uint8_t *k,
                          uint32_t *kl, struct efs_kv_item *it, int *added)
{
    uint32_t pn = 0;
    int rc;

    *added = 0;
    rc = efs_kv_key_xattr(efs_kv_inode_shard(ino), ino, k, kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, *kl, NULL, &pn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_ERR_INVAL && rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(*it));
    it->op = EFS_KV_DEL;
    it->key = k;
    it->klen = *kl;
    *added = 1;
    return EFS_OK;
}

static int xattr_name_ok(const uint8_t *name, uint16_t nlen, const uint8_t *val,
                         uint32_t vlen)
{
    uint16_t i;

    if (!name || nlen == 0 || nlen > EFS_XATTR_NAME_MAX || vlen > EFS_XATTR_VALUE_MAX)
        return EFS_ERR_INVAL;
    if (vlen && !val)
        return EFS_ERR_INVAL;
    for (i = 0; i < nlen; i++) {
        if (name[i] == 0)
            return EFS_ERR_INVAL;
    }
    return EFS_OK;
}

int efs_meta_apply_xattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                         uint8_t op, uint32_t flags, const uint8_t *name,
                         uint16_t nlen, const uint8_t *val, uint32_t vlen)
{
    struct efs_meta_row row;
    uint8_t blob[EFS_XATTR_BLOB_MAX], neu[EFS_XATTR_BLOB_MAX];
    uint8_t k_xa[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL];
    uint32_t kl = 0, ki = 0, blen = 0, count = 0, pos, i, dp, nc;
    struct efs_kv_item it[2];
    int rc, replaced = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (op != EFS_XATTR_SET && op != EFS_XATTR_REMOVE)
        return EFS_ERR_INVAL;
    if ((flags & ~(EFS_XATTR_CREATE | EFS_XATTR_REPLACE)) != 0)
        return EFS_ERR_INVAL;
    if ((flags & EFS_XATTR_CREATE) && (flags & EFS_XATTR_REPLACE))
        return EFS_ERR_INVAL;
    rc = xattr_name_ok(name, nlen, val, vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = xattr_load(kv, ino, k_xa, &kl, blob, &blen);
    if (rc != EFS_OK)
        return rc;
    rc = xattr_check_blob(blob, blen, &count);
    if (rc != EFS_OK)
        return rc;
    dp = 4;
    nc = 0;
    pos = blen ? 4 : 0;
    for (i = 0; i < count; i++) {
        uint16_t nl = rd16(blob + pos);
        uint32_t vl = efs_load_be32(blob + pos + 2);
        const uint8_t *nm = blob + pos + 6;
        const uint8_t *vv = nm + nl;
        uint32_t step = 6u + (uint32_t)nl + vl;
        int match = nl == nlen && memcmp(nm, name, nlen) == 0;

        if (match && op == EFS_XATTR_SET) {
            if (flags & EFS_XATTR_CREATE)
                return EFS_ERR_EXIST;
            if (vl == vlen && (vlen == 0 || memcmp(vv, val, vlen) == 0))
                return EFS_OK;
            rc = blob_put(neu, EFS_XATTR_BLOB_MAX, &dp, name, nlen, val, vlen);
            if (rc != EFS_OK)
                return rc;
            nc++;
            replaced = 1;
        } else if (match && op == EFS_XATTR_REMOVE) {
            replaced = 1;
        } else {
            rc = blob_put(neu, EFS_XATTR_BLOB_MAX, &dp, nm, nl, vv, vl);
            if (rc != EFS_OK)
                return rc;
            nc++;
        }
        pos += step;
    }
    if (op == EFS_XATTR_SET && !replaced) {
        if (flags & EFS_XATTR_REPLACE)
            return EFS_ERR_NODATA;
        rc = blob_put(neu, EFS_XATTR_BLOB_MAX, &dp, name, nlen, val, vlen);
        if (rc != EFS_OK)
            return rc;
        nc++;
    }
    if (op == EFS_XATTR_REMOVE && !replaced)
        return EFS_ERR_NODATA;
    efs_store_be32(neu, nc);
    row.base_ctime = max_u64(row.base_ctime, now);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    pack_inode(v_ino, &row);
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_PUT;
    it[0].key = k_ino;
    it[0].klen = ki;
    it[0].val = v_ino;
    it[0].vlen = INO_VAL;
    if (nc == 0) {
        it[1].op = EFS_KV_DEL;
        it[1].key = k_xa;
        it[1].klen = kl;
    } else {
        it[1].op = EFS_KV_PUT;
        it[1].key = k_xa;
        it[1].klen = kl;
        it[1].val = neu;
        it[1].vlen = dp;
    }
    return meta_write_batch(kv, it, 2);
}

int efs_meta_xattr_get(struct efs_kv *kv, efs_ino_t ino, const uint8_t *name,
                       uint16_t nlen, uint8_t *val, uint32_t *vlen)
{
    struct efs_meta_row row;
    uint8_t blob[EFS_XATTR_BLOB_MAX], k[EFS_KV_KEY_MAX];
    uint32_t kl = 0, blen = 0, count = 0, pos, i;
    int rc;

    if (!kv || !vlen || ino == 0)
        return EFS_ERR_INVAL;
    rc = xattr_name_ok(name, nlen, NULL, 0);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.ino != ino)
        return EFS_ERR_PROTO;
    rc = xattr_load(kv, ino, k, &kl, blob, &blen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, k, kl, xattr_check_blob(blob, blen, &count));
    if (rc != EFS_OK)
        return rc;
    pos = blen ? 4 : 0;
    for (i = 0; i < count; i++) {
        uint16_t nl = rd16(blob + pos);
        uint32_t vl = efs_load_be32(blob + pos + 2);
        const uint8_t *nm = blob + pos + 6;
        const uint8_t *vv = nm + nl;

        if (nl == nlen && memcmp(nm, name, nlen) == 0) {
            if (*vlen < vl) {
                *vlen = vl;
                return EFS_ERR_INVAL;
            }
            if (vl && val)
                memcpy(val, vv, vl);
            *vlen = vl;
            return EFS_OK;
        }
        pos += 6u + (uint32_t)nl + vl;
    }
    return EFS_ERR_NODATA;
}

int efs_meta_xattr_list(struct efs_kv *kv, efs_ino_t ino, uint8_t *buf,
                        uint32_t *len)
{
    struct efs_meta_row row;
    uint8_t blob[EFS_XATTR_BLOB_MAX], k[EFS_KV_KEY_MAX];
    uint32_t kl = 0, blen = 0, count = 0, pos, i, dp, cap;
    int rc;

    if (!kv || !len || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.ino != ino)
        return EFS_ERR_PROTO;
    rc = xattr_load(kv, ino, k, &kl, blob, &blen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, k, kl, xattr_check_blob(blob, blen, &count));
    if (rc != EFS_OK)
        return rc;
    cap = buf ? *len : 0;
    dp = 0;
    pos = blen ? 4 : 0;
    for (i = 0; i < count; i++) {
        uint16_t nl = rd16(blob + pos);
        uint32_t vl = efs_load_be32(blob + pos + 2);
        const uint8_t *nm = blob + pos + 6;
        uint32_t piece = (uint32_t)nl + 1;

        if (buf && dp + piece <= cap) {
            memcpy(buf + dp, nm, nl);
            buf[dp + nl] = 0;
        }
        dp += piece;
        pos += 6u + (uint32_t)nl + vl;
    }
    if (buf && dp > cap) {
        *len = dp;
        return EFS_ERR_INVAL;
    }
    *len = dp;
    return EFS_OK;
}

int efs_meta_apply_unlink(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          uint64_t now)
{
    return efs_meta_apply_unlink_op(kv, parent, name, now, NULL);
}

/* Recover a legacy dangling regular-file name only inside one authoritative
 * LOCAL shard. Never synthesize an inode, touch object/reap state, or infer
 * absence from another shard. Apply serialization plus intent guards make
 * absence and the name removal one atomic state-machine operation. */
static int apply_orphan_unlink(struct efs_kv *kv, efs_ino_t parent,
                               const char *name, uint64_t now,
                               const struct efs_opid_req *q)
{
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    uint8_t kd[EFS_KV_KEY_MAX], ki[EFS_KV_KEY_MAX], kp[EFS_KV_KEY_MAX];
    uint8_t ks[EFS_KV_KEY_MAX], ko[EFS_KV_KEY_MAX];
    uint8_t vp[INO_VAL], vs[8], vo[EFS_OPID_VAL_MAX];
    uint32_t nd, ni, np, ns, sh;
    int rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK) return rc;
    if (!S_ISDIR(prow.mode) || prow.layout != EFS_META_LAYOUT_LOCAL)
        return EFS_ERR_NOT_FOUND;
    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc != EFS_OK) return rc;
    sh = efs_kv_inode_shard(parent);
    if ((dent.type & S_IFMT) != S_IFREG || !dent.ino ||
        efs_kv_inode_shard(dent.ino) != sh)
        return EFS_ERR_NOT_FOUND;
    rc = efs_kv_key_dentry(sh,parent,name,kd,&nd);
    if (rc == EFS_OK) rc = efs_kv_key_inode(sh,dent.ino,ki,&ni);
    if (rc == EFS_OK) rc = efs_kv_key_inode(sh,parent,kp,&np);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,kd,nd);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,ki,ni);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,kp,np);
    if (rc != EFS_OK) return rc;
    rc = efs_meta_apply_get_inode(kv,dent.ino,&row);
    if (rc != EFS_ERR_NOT_FOUND)
        return rc == EFS_OK ? EFS_ERR_STALE : rc;
    rc = efs_kv_key_dseq(sh,parent,0,ks,&ns);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,ks,ns);
    if (rc == EFS_OK) rc = dseq_bump(kv,sh,parent,0,ks,&ns,vs);
    if (rc != EFS_OK) return rc;
    prow.base_mtime = max_u64(prow.base_mtime,now);
    prow.base_ctime = max_u64(prow.base_ctime,now);
    efs_meta_dir_note_entry(&prow,-1);
    pack_inode(vp,&prow);
    struct efs_kv_item it[4]; memset(it,0,sizeof(it));
    it[0]=(struct efs_kv_item){.op=EFS_KV_DEL,.key=kd,.klen=nd};
    it[1]=(struct efs_kv_item){.op=EFS_KV_PUT,.key=kp,.klen=np,.val=vp,.vlen=sizeof(vp)};
    it[2]=(struct efs_kv_item){.op=EFS_KV_PUT,.key=ks,.klen=ns,.val=vs,.vlen=sizeof(vs)};
    uint32_t n=3;
    if (q) {
        struct efs_opid_reply rep={0};rep.rc=EFS_OK;rep.ino=dent.ino;
        rc=opid_item(kv,sh,q,&rep,ko,vo,&it[n]);
        if (rc<0) return rc;
        n+=(uint32_t)rc;
    }
    return meta_write_batch(kv,it,n);
}

int efs_meta_apply_expiry_policy(struct efs_kv *kv, efs_ino_t root,
                                 uint64_t gen, uint32_t caller_uid,
                                 uint64_t expected,
                                 const struct efs_expiry_policy *policy)
{
    struct efs_meta_row row;
    int rc=efs_meta_apply_get_inode(kv,root,&row);
    if(rc!=EFS_OK)return rc;
    if(row.generation!=gen)return EFS_ERR_STALE;
    if(!S_ISDIR(row.mode))return EFS_ERR_INVAL;
    return efs_expiry_policy_set(kv,root,gen,caller_uid,expected,policy);
}

int efs_meta_apply_expiry_renew(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t gen, uint32_t caller_uid,
                                uint64_t expected, uint64_t now,
                                uint64_t deadline)
{
    struct efs_meta_row row;
    struct efs_expiry_record old,renewed;
    struct efs_expiry_policy policy;
    struct efs_expiry_change change;
    uint8_t inode_key[EFS_KV_KEY_MAX];uint32_t inode_len=0;
    int rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc!=EFS_OK)return rc;
    if(row.generation!=gen)return EFS_ERR_STALE;
    if(!row.nlink)return EFS_ERR_NOT_FOUND;
    rc=efs_kv_key_inode(efs_kv_inode_shard(ino),ino,inode_key,&inode_len);
    if(rc==EFS_OK)rc=efs_txn_key_busy(kv,inode_key,inode_len);
    if(rc!=EFS_OK)return rc;
    rc=efs_expiry_get(kv,ino,gen,&old);
    if(rc!=EFS_OK)return rc;
    if(!old.revision)return EFS_ERR_NODATA;
    if(old.revision!=expected)return EFS_ERR_STALE;
    if(efs_raft_shard_group(efs_kv_inode_shard(ino))!=
       efs_raft_shard_group(efs_kv_inode_shard(old.root)))return EFS_ERR_INVAL;
    uint8_t policy_key[EFS_EXPIRY_KEY_BYTES];
    efs_expiry_policy_key(old.root,old.root_generation,policy_key);
    rc=efs_txn_key_busy(kv,policy_key,sizeof(policy_key));
    if(rc==EFS_OK)rc=efs_expiry_policy_get(kv,old.root,old.root_generation,&policy);
    if(rc==EFS_OK)rc=efs_expiry_renew(&old,&policy,caller_uid,row.uid,
                                      row.nlink,row.mode,now,deadline,&renewed);
    if(rc==EFS_OK)rc=efs_expiry_plan(kv,ino,gen,expected,&renewed,&change);
    if(rc!=EFS_OK)return rc;
    return meta_write_batch(kv,change.items,change.count);
}

static int unlink_batch(struct efs_kv *kv, efs_ino_t parent, const char *name,
                             uint64_t now, const struct efs_opid_req *q,
                             const struct efs_expiry_due *expiry)
{
    struct efs_meta_dentry dent, tomb;
    struct efs_meta_row row, prow;
    struct efs_opid_reply rep;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL], v_tomb[DENT_VAL];
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint8_t k_opid[EFS_KV_KEY_MAX], v_opid[EFS_OPID_VAL_MAX];
    uint8_t k_xa[EFS_KV_KEY_MAX];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0, kr = 0, kxa = 0;
    struct efs_kv_item it[16];
    struct efs_expiry_change expiry_change;
    uint32_t n = 0, psh, hsh, dsh, nlink_after = 0;
    int rc, held = 0;
    int touch_parent = 0;
    int stamp_lane = 0;
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t ks = 0;
    uint8_t dseq_lane;

    /* I16 first: after a committed unlink the dentry is gone, and the
     * replay must answer OK, not ENOENT. The window shard is the dentry
     * shard, which needs the parent's layout. */
    if (q && opid_hosted(&q->id)) {
        rc = efs_meta_apply_get_inode(kv, parent, &prow);
        if (rc != EFS_OK)
            return rc;
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        rc = efs_meta_apply_opid_probe(kv, dsh, &q->id, &rep);
        if (rc < 0)
            return rc;
        if (rc)
            return rep.rc;
    }
    rc = efs_meta_apply_resolve(kv, parent, name, &dent, &row);
    if (expiry && rc != EFS_OK) return rc;
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_IO) {
        int repaired = apply_orphan_unlink(kv,parent,name,now,q);
        return repaired == EFS_ERR_NOT_FOUND ? rc : repaired;
    }
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    if (expiry) {
        struct efs_expiry_record current;
        if (!S_ISREG(row.mode) || row.nlink != 1 ||
            row.ino != expiry->ino || row.generation != expiry->generation)
            return EFS_ERR_STALE;
        rc = efs_expiry_get(kv, row.ino, row.generation, &current);
        if (rc != EFS_OK) return rc;
        if (!current.deadline_ns || current.deadline_ns > now ||
            current.deadline_ns != expiry->deadline_ns ||
            current.revision != expiry->revision || current.parent!=parent ||
            strcmp(current.name,name)) return EFS_ERR_STALE;
        /* D43: only while the source folder policy covers the deadline
         * (same group here; cross-group sources take the guarded txn). */
        {
            struct efs_meta_row root;struct efs_expiry_policy policy;
            if (efs_raft_shard_group(efs_kv_inode_shard(current.root)) !=
                efs_raft_shard_group(efs_kv_inode_shard(row.ino))) return EFS_ERR_INVAL;
            rc = efs_meta_apply_get_inode(kv, current.root, &root);
            if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
            if (rc != EFS_OK) return rc;
            if (!S_ISDIR(root.mode) || !root.nlink ||
                root.generation != current.root_generation) return EFS_ERR_STALE;
            rc = efs_expiry_policy_get(kv, current.root, current.root_generation, &policy);
            if (rc != EFS_OK) return rc;
            if (!policy.revision || !policy.enabled) return EFS_ERR_STALE;
        }
        /* Tombstone the revision in the namespace decision. Delayed renewal
         * must also verify the live inode, not resurrect a retired FileID. */
        current.deadline_ns = 0;
        rc = efs_expiry_plan(kv,row.ino,row.generation,current.revision,
                             &current,&expiry_change);
        if (rc != EFS_OK) return rc;
    }
    nlink_after = row.nlink <= 1 ? 0 : row.nlink - 1;
    rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    psh = efs_kv_inode_shard(parent);
    dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    /* This helper is one participant-log apply. Cross-group expiry must
     * take the guarded transaction path; never write another group's rows. */
    if (expiry && (efs_raft_shard_group(psh) != efs_raft_shard_group(dsh) ||
        efs_raft_shard_group(efs_kv_inode_shard(row.ino)) != efs_raft_shard_group(dsh)))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_dentry(psh, parent, name, k_loc, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, &kh);
    if (rc != EFS_OK)
        return rc;
    /* W36: the DENTRY is deleted unversioned below, so it needs the same
     * probe the inode row gets further down. A RENAME of this name holds
     * an EXCL DEL intent on the source dentry (and a PUT on the dest)
     * from PREPARE until RESOLVE; its inode-row REDUCE may not have
     * landed yet, so the k_ino probe alone let this unlink delete the
     * dentry and the row underneath the pending rename, whose RESOLVE
     * then PUT the dest dentry over a row that no longer existed
     * (`-?????????`, posix2 peer_rename_vs_unlink_src, 1 in 6). BUSY
     * here makes the client retry after the rename resolves, and the
     * retry finds the name gone → ENOENT, which is the right answer. */
    rc = efs_txn_key_busy(kv, k_loc, kl);
    if (rc == EFS_OK && !(kh == kl && memcmp(k_hash, k_loc, kl) == 0))
        rc = efs_txn_key_busy(kv, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    /* A lane-0 name during SPLITTING has k_hash == k_loc, and the tombstone
     * below is what has to survive; dropping the redundant delete keeps that
     * from depending on the order a batch applies its items in. In a LOCAL
     * directory the keys alias too, but nothing rewrites the key afterwards,
     * so there the delete is the whole operation. */
    if (prow.layout != EFS_META_LAYOUT_HASHED &&
        !(prow.layout == EFS_META_LAYOUT_SPLITTING &&
          kl == kh && memcmp(k_loc, k_hash, kl) == 0)) {
        it[n].op = EFS_KV_DEL;
        it[n].key = k_loc;
        it[n].klen = kl;
        n++;
    }
    if (prow.layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow.layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        pack_dentry(v_tomb, &tomb);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_hash;
        it[n].klen = kh;
        it[n].val = v_tomb;
        it[n].vlen = DENT_VAL;
        n++;
    } else if (prow.layout == EFS_META_LAYOUT_HASHED) {
        it[n].op = EFS_KV_DEL;
        it[n].key = k_hash;
        it[n].klen = kh;
        n++;
    }
    if (row.nlink <= 1) {
        held = efs_lease_any(kv, row.ino, row.generation);
        if (held < 0)
            return held;
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        /* A transaction in the middle of changing this row (a LINK's
         * nlink++ reduce, a RENAME's parent SET) must not lose to an
         * unversioned DEL — same no-wait rule as PREPARE: BUSY, retry. */
        rc = efs_txn_key_busy(kv, k_ino, ki);
        if (rc != EFS_OK)
            return rc;
        if (held) {
            row.nlink = 0;
            pack_inode(v_ino, &row);
            it[n].op = EFS_KV_PUT;
            it[n].key = k_ino;
            it[n].klen = ki;
            it[n].val = v_ino;
            it[n].vlen = INO_VAL;
            n++;
            rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(row.ino)),
                                 row.ino, k_reap, &kr);
            if (rc != EFS_OK) return rc;
            k_reap[2] = EFS_KV_KIND_ORPHAN;
            be64(v_reap, row.generation);
            it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_reap, kr, v_reap, 8};
        } else {
            int xa = 0;

            it[n].op = EFS_KV_DEL;
            it[n].key = k_ino;
            it[n].klen = ki;
            n++;
            rc = xattr_del_item(kv, row.ino, k_xa, &kxa, &it[n], &xa);
            if (rc != EFS_OK)
                return rc;
            n += (uint32_t)xa;
            /* The row dies here; the reap marker is what the background
             * reaper needs to sweep the dead file's lanes and GC its
             * fragments (L7), and it must be atomic with the delete. The
             * lease-held case above keeps the row, so the marker comes
             * from efs_meta_apply_reclaim when the last lease closes. */
            rc = efs_kv_key_reap(
                efs_kv_anchor_shard(efs_kv_inode_shard(row.ino)), row.ino,
                k_reap, &kr);
            if (rc != EFS_OK)
                return rc;
            efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
            it[n].op = EFS_KV_PUT;
            it[n].key = k_reap;
            it[n].klen = kr;
            it[n].val = v_reap;
            it[n].vlen = EFS_META_REAP_VAL;
            n++;
        }
    } else {
        /* Surviving links keep the inode; POSIX updates its ctime. */
        row.nlink--;
        row.base_ctime = max_u64(row.base_ctime, now);
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        pack_inode(v_ino, &row);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ino;
        it[n].klen = ki;
        it[n].val = v_ino;
        it[n].vlen = INO_VAL;
        n++;
    }
    /* Same split as create: LOCAL times ride the parent row; spread times
     * live on the dentry shard's dir lane so unlink does not re-serialize
     * on the directory's home leader. */
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        prow.base_mtime = max_u64(prow.base_mtime, now);
        prow.base_ctime = max_u64(prow.base_ctime, now);
        efs_meta_dir_note_entry(&prow, -1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&prow);
        rc = dir_lane_stamp(kv, &prow, name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &prow);
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    rc = dseq_bump(kv, dsh, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    if (q) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = row.ino;
        rep.extra = nlink_after;
        rc = opid_item(kv, dsh, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    if (expiry) {
        if (n + expiry_change.count > sizeof(it)/sizeof(it[0]))
            return EFS_ERR_OVERFLOW;
        for (uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
    }
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_unlink_op(struct efs_kv *kv, efs_ino_t parent, const char *name,
                             uint64_t now, const struct efs_opid_req *q)
{ return unlink_batch(kv,parent,name,now,q,NULL); }

int efs_meta_apply_expire(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          uint64_t committed_now, const struct efs_expiry_due *due)
{
    if (!due || !due->revision || !due->deadline_ns) return EFS_ERR_INVAL;
    return unlink_batch(kv,parent,name,committed_now,NULL,due);
}

int efs_meta_apply_link(struct efs_kv *kv, efs_ino_t src_parent, const char *src_name,
                        efs_ino_t dst_parent, const char *dst_name, uint64_t now)
{
    struct efs_meta_dentry src, ndent;
    struct efs_meta_row row, dprow;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_par[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_par[INO_VAL];
    uint8_t v_ln[LANE_VAL], v_dseq[8];
    uint32_t kd = 0, ki = 0, kp = 0, kln = 0, ks = 0;
    struct efs_kv_item it[8];
    uint32_t n = 0, dsh;
    uint64_t bit;
    uint8_t dseq_lane;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;

    if (!kv || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_resolve(kv, src_parent, src_name, &src, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    struct efs_expiry_record expiry;
    rc=efs_expiry_get(kv,row.ino,row.generation,&expiry);
    if(rc!=EFS_OK)return rc;
    if(expiry.deadline_ns)return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, dst_parent, dst_name, &ndent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    dseq_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0
                                                     : efs_kv_dir_lane(dst_name);
    memset(&ndent, 0, sizeof(ndent));
    ndent.ino = row.ino;
    ndent.generation = row.generation;
    ndent.type = row.mode & S_IFMT;
    pack_dentry(v_dent, &ndent);
    row.nlink++;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(v_ino, &row);

    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;

    if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
        dprow.base_mtime = max_u64(dprow.base_mtime, now);
        dprow.base_ctime = max_u64(dprow.base_ctime, now);
        efs_meta_dir_note_entry(&dprow, 1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&dprow);
        bit = 1ull << efs_kv_dir_lane(dst_name);
        if ((dprow.used_shards & bit) == 0) {
            dprow.used_shards |= bit;
            touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, &dprow, dst_name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &dprow);
        rc = efs_kv_key_inode(efs_kv_inode_shard(dst_parent), dst_parent, k_par,
                              &kp);
        if (rc != EFS_OK)
            return rc;
    }
    rc = dseq_bump(kv, dsh, dst_parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (touch_parent) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    return meta_write_batch(kv, it, n);
}

/* Drop one name from a directory into `it`, matching unlink's LOCAL /
 * SPLITTING / HASHED rules. `k_loc`/`k_hash`/`v_tomb` must outlive the batch. */
static int dentry_drop_items(struct efs_kv *kv, const struct efs_meta_row *prow,
                             efs_ino_t parent, const char *name,
                             uint8_t *k_loc, uint32_t *kl, uint8_t *k_hash,
                             uint32_t *kh, uint8_t *v_tomb,
                             struct efs_kv_item *it, uint32_t *n)
{
    uint32_t psh, hsh;
    struct efs_meta_dentry tomb;
    int rc;

    (void)kv;
    psh = efs_kv_inode_shard(parent);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(psh, parent, name, k_loc, kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    if (prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING &&
          *kl == *kh && memcmp(k_loc, k_hash, *kl) == 0)) {
        it[*n].op = EFS_KV_DEL;
        it[*n].key = k_loc;
        it[*n].klen = *kl;
        (*n)++;
    }
    if (prow->layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow->layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        pack_dentry(v_tomb, &tomb);
        it[*n].op = EFS_KV_PUT;
        it[*n].key = k_hash;
        it[*n].klen = *kh;
        it[*n].val = v_tomb;
        it[*n].vlen = DENT_VAL;
        (*n)++;
    } else if (prow->layout == EFS_META_LAYOUT_HASHED) {
        it[*n].op = EFS_KV_DEL;
        it[*n].key = k_hash;
        it[*n].klen = *kh;
        (*n)++;
    }
    return EFS_OK;
}

static int stamp_dir_items(struct efs_kv *kv, struct efs_meta_row *prow,
                           efs_ino_t parent, const char *name, uint64_t now,
                           uint8_t *k_par, uint32_t *kp, uint8_t *v_par,
                           uint8_t *k_ln, uint32_t *kln, uint8_t *v_ln,
                           int *touch_parent, int *stamp_lane)
{
    uint64_t bit;
    int rc;

    *touch_parent = 0;
    *stamp_lane = 0;
    if (prow->layout == EFS_META_LAYOUT_LOCAL) {
        prow->base_mtime = max_u64(prow->base_mtime, now);
        prow->base_ctime = max_u64(prow->base_ctime, now);
        *touch_parent = 1;
        /* Caller sets nents via efs_meta_dir_note_entry before stamp. */
    } else {
        bit = 1ull << efs_kv_dir_lane(name);
        if ((prow->used_shards & bit) == 0) {
            prow->used_shards |= bit;
            *touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, prow, name, now, k_ln, kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        *stamp_lane = 1;
    }
    if (*touch_parent) {
        pack_inode(v_par, prow);
        rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, kp);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* POSIX: src must not be an ancestor of dst. Walking the live tree is
 * unsound under concurrency; the sim txn guards parent_version. This
 * helper is the single-KV form of the same predicate. */
static int dir_is_under(struct efs_kv *kv, efs_ino_t ancestor, efs_ino_t start)
{
    efs_ino_t cur = start;
    struct efs_meta_row r;
    int hops, rc;

    for (hops = 0; hops < 64; hops++) {
        if (cur == ancestor)
            return EFS_ERR_INVAL;
        rc = efs_meta_apply_get_inode(kv, cur, &r);
        if (rc != EFS_OK)
            return rc;
        if (!S_ISDIR(r.mode))
            return EFS_ERR_INVAL;
        if (cur == EFS_ROOT_INO || cur == r.parent)
            return EFS_OK;
        cur = r.parent;
    }
    return EFS_ERR_INVAL;
}

int efs_meta_apply_rename(struct efs_kv *kv, efs_ino_t src_parent,
                          const char *src_name, efs_ino_t dst_parent,
                          const char *dst_name, uint64_t now)
{
    struct efs_meta_dentry src, exist;
    struct efs_meta_row row, sprow, dprow;
    uint8_t k_sloc[EFS_KV_KEY_MAX], k_shash[EFS_KV_KEY_MAX], v_stomb[DENT_VAL];
    uint8_t k_dent[EFS_KV_KEY_MAX], v_dent[DENT_VAL];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL];
    uint8_t k_spar[EFS_KV_KEY_MAX], v_spar[INO_VAL];
    uint8_t k_dpar[EFS_KV_KEY_MAX], v_dpar[INO_VAL];
    uint8_t k_sln[EFS_KV_KEY_MAX], v_sln[LANE_VAL];
    uint8_t k_dln[EFS_KV_KEY_MAX], v_dln[LANE_VAL];
    uint8_t k_sdseq[EFS_KV_KEY_MAX], v_sdseq[8];
    uint8_t k_ddseq[EFS_KV_KEY_MAX], v_ddseq[8];
    uint8_t k_pver[EFS_KV_KEY_MAX], v_pver[8];
    uint32_t ksl = 0, ksh = 0, kd = 0, ki = 0, ksp = 0, kdp = 0;
    uint32_t ksln = 0, kdln = 0, kss = 0, kds = 0, kpv = 0;
    struct efs_kv_item it[16];
    struct efs_expiry_change expiry_change;int expiry_move=0;
    uint32_t n = 0, dsh;
    int rc, same_dir, is_dir, touch_src = 0, stamp_src = 0, touch_dst = 0,
        stamp_dst = 0;
    uint8_t s_lane, d_lane;

    if (!kv || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    if (src_parent == dst_parent && strcmp(src_name, dst_name) == 0)
        return EFS_OK;
    rc = efs_meta_apply_resolve(kv, src_parent, src_name, &src, &row);
    if (rc != EFS_OK)
        return rc;
    is_dir = S_ISDIR(row.mode);
    if (is_dir) {
        if (row.ino == EFS_ROOT_INO)
            return EFS_ERR_INVAL;
        rc = dir_is_under(kv, row.ino, dst_parent);
        if (rc != EFS_OK)
            return rc;
        row.parent_version++;
    }
    rc = efs_meta_apply_get_inode(kv, src_parent, &sprow);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, dst_parent, dst_name, &exist);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST; /* replace is a wider txn; not this helper */
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    if(S_ISREG(row.mode)) {
        struct efs_expiry_record expiry;
        rc=efs_expiry_get(kv,row.ino,row.generation,&expiry);
        if(rc!=EFS_OK)return rc;
        if(expiry.revision && row.nlink==1 &&
           (expiry.deadline_ns || (expiry.parent==src_parent && !strcmp(expiry.name,src_name)))) {
            if(expiry.parent!=src_parent || strcmp(expiry.name,src_name))return EFS_ERR_STALE;
            expiry.parent=dst_parent;memset(expiry.name,0,sizeof(expiry.name));
            memcpy(expiry.name,dst_name,strlen(dst_name)+1);
            rc=efs_expiry_plan(kv,row.ino,row.generation,expiry.revision,&expiry,&expiry_change);
            if(rc!=EFS_OK)return rc;
            expiry_move=1;
        }
    }
    same_dir = src_parent == dst_parent;
    if (is_dir && !same_dir) {
        if (sprow.nlink < 3)
            return EFS_ERR_PROTO;
        sprow.nlink--;
        dprow.nlink++;
    }
    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    s_lane = sprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(src_name);
    d_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(dst_name);
    memset(&exist, 0, sizeof(exist));
    exist.ino = row.ino;
    exist.generation = row.generation;
    exist.type = row.mode & S_IFMT;
    pack_dentry(v_dent, &exist);
    row.parent = dst_parent;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(v_ino, &row);
    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    rc = dentry_drop_items(kv, &sprow, src_parent, src_name, k_sloc, &ksl,
                           k_shash, &ksh, v_stomb, it, &n);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (is_dir) {
        rc = efs_kv_key_pver(efs_kv_inode_shard(row.ino), row.ino, k_pver, &kpv);
        if (rc != EFS_OK)
            return rc;
        be64(v_pver, row.parent_version);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_pver;
        it[n].klen = kpv;
        it[n].val = v_pver;
        it[n].vlen = 8;
        n++;
    }

    /* Apply rename does not replace (EXIST). same-dir is net-zero nents. */
    if (!same_dir)
        efs_meta_dir_note_entry(&sprow, -1);
    efs_dir_spread_seen(&sprow);
    rc = stamp_dir_items(kv, &sprow, src_parent, src_name, now, k_spar, &ksp,
                         v_spar, k_sln, &ksln, v_sln, &touch_src, &stamp_src);
    if (rc != EFS_OK)
        return rc;
    if (!same_dir) {
        efs_meta_dir_note_entry(&dprow, 1);
        efs_dir_spread_seen(&dprow);
        rc = stamp_dir_items(kv, &dprow, dst_parent, dst_name, now, k_dpar, &kdp,
                             v_dpar, k_dln, &kdln, v_dln, &touch_dst,
                             &stamp_dst);
        if (rc != EFS_OK)
            return rc;
    } else {
        /* One parent row: times already in sprow. Dest name may land on
         * a dir lane this directory has never used — that is first use,
         * so the used_shards bit belongs on the same PUT as src. */
        touch_dst = 0;
        stamp_dst = 0;
        if (dprow.layout != EFS_META_LAYOUT_LOCAL) {
            uint64_t bit = 1ull << efs_kv_dir_lane(dst_name);
            if ((sprow.used_shards & bit) == 0) {
                sprow.used_shards |= bit;
                pack_inode(v_spar, &sprow);
                rc = efs_kv_key_inode(efs_kv_inode_shard(src_parent), src_parent,
                                      k_spar, &ksp);
                if (rc != EFS_OK)
                    return rc;
                touch_src = 1;
            }
            rc = dir_lane_stamp(kv, &sprow, dst_name, now, k_dln, &kdln, v_dln);
            if (rc != EFS_OK)
                return rc;
            stamp_dst = 1;
        }
    }
    if (is_dir && !same_dir) {
        if (!touch_src) {
            pack_inode(v_spar, &sprow);
            rc = efs_kv_key_inode(efs_kv_inode_shard(src_parent), src_parent,
                                  k_spar, &ksp);
            if (rc != EFS_OK)
                return rc;
            touch_src = 1;
        }
        if (!touch_dst) {
            pack_inode(v_dpar, &dprow);
            rc = efs_kv_key_inode(efs_kv_inode_shard(dst_parent), dst_parent,
                                  k_dpar, &kdp);
            if (rc != EFS_OK)
                return rc;
            touch_dst = 1;
        }
    }
    if (touch_src) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_spar;
        it[n].klen = ksp;
        it[n].val = v_spar;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_src) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_sln;
        it[n].klen = ksln;
        it[n].val = v_sln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    if (touch_dst) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_dpar;
        it[n].klen = kdp;
        it[n].val = v_dpar;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_dst) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_dln;
        it[n].klen = kdln;
        it[n].val = v_dln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    rc = dseq_bump(kv, efs_kv_dentry_shard(src_parent, src_name, sprow.layout),
                   src_parent, s_lane, k_sdseq, &kss, v_sdseq);
    if (rc == EFS_OK)
        rc = dseq_bump(kv, dsh, dst_parent, d_lane, k_ddseq, &kds, v_ddseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_sdseq;
    it[n].klen = kss;
    it[n].val = v_sdseq;
    it[n].vlen = 8;
    n++;
    if (!(same_dir && sprow.layout == EFS_META_LAYOUT_LOCAL &&
          kss == kds && memcmp(k_sdseq, k_ddseq, kss) == 0)) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ddseq;
        it[n].klen = kds;
        it[n].val = v_ddseq;
        it[n].vlen = 8;
        n++;
    }
    if(expiry_move) {
        if(n+expiry_change.count>sizeof(it)/sizeof(it[0]))return EFS_ERR_OVERFLOW;
        for(uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
    }
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_setattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_setattr *sa)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0;
    int rc;

    if (!kv || !sa || ino == 0)
        return EFS_ERR_INVAL;
    if (sa->mask & ~(EFS_META_SET_MODE | EFS_META_SET_UID | EFS_META_SET_GID))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (sa->expect_gen != 0 && sa->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if (sa->mask & EFS_META_SET_MODE)
        row.mode = (row.mode & ~07777u) | (sa->mode & 07777u);
    if (sa->mask & EFS_META_SET_UID)
        row.uid = sa->uid;
    if (sa->mask & EFS_META_SET_GID)
        row.gid = sa->gid;
    /* A metadata change stamps ctime and leaves mtime alone. MAX-clamped, so
     * a backwards CLOCK_REALTIME cannot walk ctime back, and so re-applying
     * the same committed entry is a no-op. */
    row.base_ctime = max_u64(row.base_ctime, now);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    pack_inode(val, &row);
    return meta_write_put(kv, key, klen, val, INO_VAL);
}

/* --- readdir ---------------------------------------------------------
 *
 * Ordered keys make a directory listing a range scan: dentries for one
 * parent are a contiguous key range, so a page costs the page. A spread
 * directory is up to 64 such ranges on 64 shards, and because a name hashes
 * to exactly one dir lane, visiting the lanes in order cannot produce a
 * duplicate — no cross-lane dedup set is needed.
 *
 * SPLITTING is the one case where a name can exist twice: on its hashed lane
 * and still on the pre-split local shard. The hashed side always wins (a
 * mutation during the split writes the hashed side, using a tombstone to
 * record a delete), so the local pass consults the hashed side per name and
 * skips anything found there — live (already returned) or tombstoned
 * (deleted). That is what stops the migration from resurrecting a name.
 *
 * Dir lane 0 is the directory's own inode shard (directory.md), and a dentry
 * key carries the shard but not the layout — so for the 1-in-64 names that
 * hash to lane 0, the hashed key and the pre-split local key are the same
 * key. Such a name is born in its final location and never migrates. Two
 * consequences, both easy to get wrong in opposite directions: the local
 * pass must not treat those names as claimed by the hashed side (they would
 * vanish), and during SPLITTING lane 0 must not be scanned as a lane on top
 * of the local pass (every unmigrated name would come back twice).
 *
 * So source 0 is always the inode shard's range: the whole directory when
 * LOCAL, the local pass when SPLITTING, lane 0 when HASHED. It must come
 * first. A listing is many pages and the migrator runs between them, moving
 * names local -> hashed only. Local first means a name the local pass has
 * not reached yet can only move to a lane that is still ahead of the cursor.
 * Scanning the lanes first lost every name that moved into an already
 * scanned lane before the local pass got to it: a run of names at the
 * migration frontier per listing (W125 C7, 1M-file directory). The cost is
 * the reverse window: a name listed by the local pass and then migrated is
 * listed again by its lane. Readdir is a weak listing; callers drop repeated
 * names. The same numbering keeps a cookie valid across LOCAL -> SPLITTING
 * -> HASHED, which can happen mid-listing. */

#define DENTRY_KEY_PREFIX 11 /* shard + kind + parent; name follows */

struct dir_scan {
    struct efs_kv *kv;
    struct efs_meta_dir_ent *out;
    uint32_t max;
    uint32_t n;
    char last[EFS_MAX_NAME];
    int have_last;
    int full; /* stopped because the page filled, not because the range ended */
    int rc;
};

static int dir_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct dir_scan *s = user;
    uint32_t nl;

    if (klen <= DENTRY_KEY_PREFIX)
        return 0; /* the prefix itself is not an entry */
    nl = klen - DENTRY_KEY_PREFIX;
    if (nl >= EFS_MAX_NAME) {
        s->rc = efs_kv_decode_result(s->kv, key, klen, EFS_ERR_PROTO);
        return s->rc;
    }
    if (s->n >= s->max) {
        s->full = 1; /* stop; the cursor resumes in this same source */
        return 1;
    }
    memcpy(s->out[s->n].name, key + DENTRY_KEY_PREFIX, nl);
    s->out[s->n].name[nl] = 0;
    int decoded = unpack_dentry(val, vlen, &s->out[s->n].d);
    if (decoded != EFS_OK) {
        s->rc = efs_kv_decode_result(s->kv, key, klen, decoded);
        return s->rc;
    }
    memcpy(s->last, s->out[s->n].name, nl + 1);
    s->have_last = 1;
    if (s->out[s->n].d.type == EFS_META_DENT_TOMBSTONE)
        return 0; /* recorded as progress, never returned to the caller */
    s->n++;
    return 0;
}

/* Scans one source's range, resuming strictly after cur->name when set. */
static int dir_scan_one(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                        const char *after, struct dir_scan *s)
{
    uint8_t pre[EFS_KV_KEY_MAX], start[EFS_KV_KEY_MAX];
    uint32_t pl = 0, sl = 0;
    int rc;

    s->kv = kv;

    rc = efs_kv_key_dentry_prefix(shard, dir, pre, &pl);
    if (rc != EFS_OK)
        return rc;
    if (after && after[0]) {
        rc = efs_kv_key_dentry(shard, dir, after, start, &sl);
        if (rc != EFS_OK)
            return rc;
        /* One 0 byte past the last name is its immediate successor and still
         * precedes any longer name beginning with it, so nothing is skipped
         * or repeated. */
        if (sl + 1 > EFS_KV_KEY_MAX)
            return EFS_ERR_INVAL;
        start[sl++] = 0;
    }
    rc = efs_kv_scan_from(kv, pre, pl, sl ? start : NULL, sl, dir_scan_cb, s);
    if (rc != EFS_OK && rc != 1)
        return rc;
    return s->rc;
}

/* During SPLITTING the hashed side is authoritative for a name. */
static int hashed_side_claims(struct efs_kv *kv, efs_ino_t dir, const char *name,
                              int *claimed)
{
    struct efs_meta_dentry d;
    int rc;

    *claimed = 0;
    /* A lane-0 name has no separate hashed copy to defer to — the record in
     * hand IS its hashed record. Asking the KV would find that same record
     * and drop the name from the listing. */
    if (efs_kv_dir_lane(name) == 0)
        return EFS_OK;
    rc = dent_get(kv, efs_kv_dentry_shard(dir, name, EFS_META_LAYOUT_HASHED),
                  dir, name, &d);
    if (rc == EFS_OK) {
        *claimed = 1;
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND) {
        *claimed = 0;
        return EFS_OK;
    }
    return rc;
}

int efs_meta_apply_readdir(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n)
{
    struct efs_meta_row row;
    struct dir_scan s;
    uint32_t last_src;
    int rc;

    if (!kv || !cur || !out || !n || max == 0 || dir == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (cur->done)
        return EFS_OK;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(row.mode))
        return EFS_ERR_INVAL; /* as create_file does for a non-dir parent */

    last_src = row.layout == EFS_META_LAYOUT_LOCAL ? 0 : EFS_META_LANES - 1;

    memset(&s, 0, sizeof(s));
    s.out = out;
    s.max = max;
    while (cur->src <= last_src && s.n < max) {
        uint32_t shard, base = s.n;
        int local_pass = 0;

        if (cur->src == 0) {
            /* LOCAL, the SPLITTING local pass and HASHED lane 0 are all
             * this one range. The local pass runs whatever the bitmap says. */
            shard = efs_kv_inode_shard(dir);
            local_pass = row.layout == EFS_META_LAYOUT_SPLITTING;
            if (row.layout == EFS_META_LAYOUT_HASHED &&
                (row.used_shards & 1ULL) == 0) {
                cur->src++;
                cur->name[0] = 0;
                continue;
            }
        } else {
            /* A lane this directory has never used holds no keys, and the
             * bitmap records every first use — including the migrator's. */
            if ((row.used_shards & (1ULL << cur->src)) == 0) {
                cur->src++;
                cur->name[0] = 0;
                continue;
            }
            shard = efs_kv_lane_shard(dir, (uint8_t)cur->src);
        }

        s.have_last = 0;
        s.last[0] = 0;
        s.full = 0;
        rc = dir_scan_one(kv, shard, dir, cur->name, &s);
        if (rc != EFS_OK)
            return rc;
        if (local_pass) {
            uint32_t i, keep = base;

            /* Only this source's entries; earlier sources are already final. */
            for (i = base; i < s.n; i++) {
                int claimed = 0;

                rc = hashed_side_claims(kv, dir, out[i].name, &claimed);
                if (rc != EFS_OK)
                    return rc;
                if (claimed)
                    continue;
                if (keep != i)
                    out[keep] = out[i];
                keep++;
            }
            s.n = keep;
        }
        if (s.have_last)
            memcpy(cur->name, s.last, strlen(s.last) + 1);
        if (s.full) {
            /* More remains in this source, so do not advance it. Returning
             * an empty page while entries remain would make any caller that
             * stops at *n == 0 miss them, so keep scanning until there is
             * something to deliver; the cursor advanced, so this terminates.
             * The only way to get here empty is a full page of names the
             * hashed side already owns. */
            if (s.n > 0)
                break;
            continue;
        }
        /* This source is exhausted: advance with a fresh name position. */
        cur->src++;
        cur->name[0] = 0;
    }
    if (cur->src > last_src)
        cur->done = 1;
    *n = s.n;
    return EFS_OK;
}

static int dir_has_live(struct efs_kv *kv, uint32_t shard, efs_ino_t dir)
{
    struct efs_meta_dir_ent one;
    struct dir_scan s;
    int rc;

    memset(&s, 0, sizeof(s));
    memset(&one, 0, sizeof(one));
    s.out = &one;
    s.max = 1;
    rc = dir_scan_one(kv, shard, dir, NULL, &s);
    if (rc != EFS_OK)
        return rc;
    if (s.rc != EFS_OK)
        return s.rc;
    return s.n > 0 ? EFS_ERR_NOT_EMPTY : EFS_OK;
}

static int dir_empty_now(struct efs_kv *kv, const struct efs_meta_row *row)
{
    uint8_t lane;
    int rc;

    if (row->nlink > 2)
        return EFS_ERR_NOT_EMPTY;
    if (row->layout == EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_BUSY;
    if (row->layout == EFS_META_LAYOUT_LOCAL)
        return dir_has_live(kv, efs_kv_inode_shard(row->ino), row->ino);
    if (row->used_shards == 0)
        return EFS_OK;
    for (lane = 0; lane < EFS_META_LANES; lane++) {
        if ((row->used_shards & (1ull << lane)) == 0)
            continue;
        rc = dir_has_live(kv, efs_kv_lane_shard(row->ino, lane), row->ino);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* "Empty now" is not "nobody is adding a child": a cross-group CREATE /
 * MKDIR / LINK into `row` holds only intents until it resolves — a dentry
 * EXCL (invisible to dir_has_live), a REDUCE_ADD on the dseq witness and
 * a REDUCE_INO on the row itself. The log path deletes rows unversioned,
 * so it has to honour those intents the way PREPARE does: BUSY. The
 * probed keys are all local by construction — the log path only runs
 * when every participant shard is on this group. */
static int dir_txn_busy(struct efs_kv *kv, const struct efs_meta_row *row)
{
    uint8_t k[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    uint8_t lane;
    int rc;

    rc = efs_kv_key_inode(efs_kv_inode_shard(row->ino), row->ino, k, &kl);
    if (rc == EFS_OK)
        rc = efs_txn_key_busy(kv, k, kl);
    if (rc != EFS_OK)
        return rc;
    if (row->layout == EFS_META_LAYOUT_LOCAL) {
        rc = efs_kv_key_dseq(efs_kv_inode_shard(row->ino), row->ino, 0, k, &kl);
        if (rc == EFS_OK)
            rc = efs_txn_key_busy(kv, k, kl);
        return rc;
    }
    for (lane = 0; lane < EFS_META_LANES; lane++) {
        if ((row->used_shards & (1ull << lane)) == 0)
            continue;
        rc = efs_kv_key_dseq(efs_kv_lane_shard(row->ino, lane), row->ino, lane,
                             k, &kl);
        if (rc == EFS_OK)
            rc = efs_txn_key_busy(kv, k, kl);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

int efs_meta_apply_rmdir(struct efs_kv *kv, efs_ino_t parent, const char *name,
                         uint64_t now)
{
    return efs_meta_apply_rmdir_op(kv, parent, name, now, NULL);
}

int efs_meta_apply_rmdir_op(struct efs_kv *kv, efs_ino_t parent, const char *name,
                            uint64_t now, const struct efs_opid_req *q)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, prow;
    struct efs_opid_reply rep;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], v_tomb[DENT_VAL];
    uint8_t k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint8_t k_opid[EFS_KV_KEY_MAX], v_opid[EFS_OPID_VAL_MAX];
    uint8_t k_xa[EFS_KV_KEY_MAX];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0, ks = 0, kxa = 0, dsh;
    struct efs_kv_item it[12];
    uint32_t n = 0;
    int rc, held, xa = 0, stamp_lane = 0;
    uint8_t dseq_lane;

    if (!kv || !name || parent == 0)
        return EFS_ERR_INVAL;
    if (q && opid_hosted(&q->id)) {
        rc = efs_meta_apply_get_inode(kv, parent, &prow);
        if (rc != EFS_OK)
            return rc;
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        rc = efs_meta_apply_opid_probe(kv, dsh, &q->id, &rep);
        if (rc < 0)
            return rc;
        if (rc)
            return rep.rc;
    }
    rc = efs_meta_apply_resolve(kv, parent, name, &dent, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(row.mode) || row.ino == EFS_ROOT_INO)
        return EFS_ERR_INVAL;
    rc = dir_empty_now(kv, &row);
    if (rc != EFS_OK)
        return rc;
    rc = dir_txn_busy(kv, &row);
    if (rc != EFS_OK)
        return rc;
    held = efs_lease_any(kv, row.ino, row.generation);
    if (held < 0)
        return held;
    if (held)
        return EFS_ERR_BUSY;
    rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    if (prow.nlink < 3)
        return EFS_ERR_PROTO;
    prow.nlink--;
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        prow.base_mtime = max_u64(prow.base_mtime, now);
        prow.base_ctime = max_u64(prow.base_ctime, now);
        efs_meta_dir_note_entry(&prow, -1);
    } else {
        efs_dir_spread_seen(&prow);
        rc = dir_lane_stamp(kv, &prow, name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    pack_inode(v_par, &prow);
    rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, &kp);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    rc = dentry_drop_items(kv, &prow, parent, name, k_loc, &kl, k_hash, &kh,
                           v_tomb, it, &n);
    if (rc != EFS_OK)
        return rc;
    /* W36 (same as unlink): a dir RENAME holds an EXCL intent on this
     * dentry until it resolves; dir_txn_busy above probed the child's
     * row and dseq, not the name being dropped. */
    rc = efs_txn_key_busy(kv, k_loc, kl);
    if (rc == EFS_OK && kh && !(kh == kl && memcmp(k_hash, k_loc, kl) == 0))
        rc = efs_txn_key_busy(kv, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_DEL;
    it[n].key = k_ino;
    it[n].klen = ki;
    n++;
    rc = xattr_del_item(kv, row.ino, k_xa, &kxa, &it[n], &xa);
    if (rc != EFS_OK)
        return rc;
    n += (uint32_t)xa;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_par;
    it[n].klen = kp;
    it[n].val = v_par;
    it[n].vlen = INO_VAL;
    n++;
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    rc = dseq_bump(kv, dsh, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    if (q) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = row.ino;
        rc = opid_item(kv, dsh, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_reclaim(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint8_t k_xa[EFS_KV_KEY_MAX];
    uint32_t ki = 0, kr = 0, kxa = 0;
    struct efs_kv_item it[4];
    uint8_t k_orphan[EFS_KV_KEY_MAX];
    int rc, held, xa = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.nlink != 0)
        return EFS_ERR_BUSY;
    held = efs_lease_any(kv, ino, row.generation);
    if (held < 0)
        return held;
    if (held)
        return EFS_ERR_BUSY;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_txn_key_busy(kv, k_ino, ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),
                             ino, k_reap, &kr);
    if (rc != EFS_OK)
        return rc;
    /* Row delete and reap marker are one batch: the marker is the reaper's
     * only record of the dead file's lanes (L7), so it can never be lost
     * to a crash between the two writes. Append cursor/reservation records
     * are cleaned by REAP_DONE, keeping this hot entry small. */
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_DEL;
    it[0].key = k_ino;
    it[0].klen = ki;
    efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
    it[1].op = EFS_KV_PUT;
    it[1].key = k_reap;
    it[1].klen = kr;
    it[1].val = v_reap;
    it[1].vlen = EFS_META_REAP_VAL;
    rc = xattr_del_item(kv, ino, k_xa, &kxa, &it[2], &xa);
    if (rc != EFS_OK)
        return rc;
    memcpy(k_orphan, k_reap, kr); k_orphan[2] = EFS_KV_KIND_ORPHAN;
    it[xa ? 3 : 2] = (struct efs_kv_item){EFS_KV_DEL, k_orphan, kr, NULL, 0};
    return meta_write_batch(kv, it, xa ? 4 : 3);
}

/* Unlike a REAP marker, this index never authorizes fragment deletion.
 * Closing a lease can precede RESOLVE's nlink decrement; recheck after all
 * pending namespace transactions settle, on the inode's own Raft group. */
int efs_meta_apply_orphan_reclaim(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], value[8];
    uint32_t len = 0, vl = sizeof(value);
    if (!kv || ino <= EFS_ROOT_INO || !gen) return EFS_ERR_INVAL;
    int rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_OK && row.generation == gen && !row.nlink)
        return efs_meta_apply_reclaim(kv, ino);
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)), ino, key, &len);
    if (rc != EFS_OK) return rc;
    key[2] = EFS_KV_KIND_ORPHAN;
    rc = efs_kv_get(kv, key, len, value, &vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc != EFS_OK) return rc;
    if (vl != sizeof(value)) return EFS_ERR_PROTO;
    if (rd64(value) != gen) return EFS_ERR_STALE;
    rc = meta_write_del(kv, key, len);
    return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
}


static uint32_t chunk_header_size(const uint8_t *p, uint32_t n)
{
    if (!p || n < CHUNK_VAL) return CHUNK_VAL;
    return CHUNK_VAL + (efs_load_be32(p + 8) == EFS_META_PROFILE_PACK ? EFS_PACK_REF_BYTES : 0);
}

static void pack_chunk(uint8_t *p, const struct efs_meta_chunk *ch)
{
    int i;

    be64(p + 0, ch->generation);
    efs_store_be32(p + 8, ch->coding_profile_id);
    be64(p + 12, ch->content_epoch);
    efs_store_be32(p + 20, ch->base_clip);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        efs_store_be32(p + CHUNK_HDR + (uint32_t)i * 4u, ch->nodes[i]);
    memcpy(p + CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS, ch->checksums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    if (ch->coding_profile_id == EFS_META_PROFILE_PACK)
        (void)efs_pack_ref_encode(&ch->packed, p + CHUNK_VAL);
}

static int unpack_chunk(const uint8_t *p, uint32_t n, struct efs_meta_chunk *ch)
{
    int i;

    if (!p || !ch || n < CHUNK_VAL)
        return EFS_ERR_PROTO;
    memset(ch, 0, sizeof(*ch));
    ch->generation = rd64(p + 0);
    ch->coding_profile_id = efs_load_be32(p + 8);
    ch->content_epoch = rd64(p + 12);
    ch->base_clip = efs_load_be32(p + 20);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        ch->nodes[i] = efs_load_be32(p + CHUNK_HDR + (uint32_t)i * 4u);
    memcpy(ch->checksums, p + CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    /* Rows with the reserved profile are not readable here; never
     * interpret their extension as a plain base. */
    if (ch->coding_profile_id == EFS_META_PROFILE_RESERVED)
        return EFS_ERR_PROTO;
    if (ch->coding_profile_id == EFS_META_PROFILE_PACK) {
        uint32_t hdr = CHUNK_VAL + EFS_PACK_REF_BYTES;
        if (n < hdr ||
            efs_pack_ref_decode(p + CHUNK_VAL, EFS_PACK_REF_BYTES, &ch->packed) != EFS_OK ||
            ch->generation != ch->packed.record_generation ||
            ch->content_epoch != ch->packed.content_epoch)
            return EFS_ERR_PROTO;
        /* A damaged extension or trailer must never masquerade as a
         * base-only image. */
        if (n != hdr) {
            if (n < hdr + DELTA_HDR || p[hdr + 2] || p[hdr + 3])
                return EFS_ERR_PROTO;
            uint32_t count = rd16(p + hdr);
            if (!count || count > EFS_CHUNK_DELTA_MAX ||
                n != hdr + DELTA_HDR + count * DELTA_REC)
                return EFS_ERR_PROTO;
        }
    }
    return EFS_OK;
}

/* Delta record: off:4 len:4 gen:8 epoch:8 seq:8 nodes:12 cks:96 = 140. */
static void pack_delta_rec(uint8_t *p, const struct efs_meta_delta *d)
{
    int i;

    efs_store_be32(p + 0, d->off);
    efs_store_be32(p + 4, d->len);
    be64(p + 8, d->generation);
    be64(p + 16, d->content_epoch);
    be64(p + 24, d->seq);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        efs_store_be32(p + 32 + (uint32_t)i * 4u, d->nodes[i]);
    memcpy(p + 32 + 4 * EFS_NUM_FRAGMENTS, d->checksums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

static int unpack_delta_rec(const uint8_t *p, struct efs_meta_delta *d)
{
    int i;

    memset(d, 0, sizeof(*d));
    d->off = efs_load_be32(p + 0);
    d->len = efs_load_be32(p + 4);
    d->generation = rd64(p + 8);
    d->content_epoch = rd64(p + 16);
    d->seq = rd64(p + 24);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        d->nodes[i] = efs_load_be32(p + 32 + (uint32_t)i * 4u);
    memcpy(d->checksums, p + 32 + 4 * EFS_NUM_FRAGMENTS,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    return EFS_OK;
}

/* 0 when the value is a bare chunk image or the trailer is short. */
static uint32_t trailer_n(const uint8_t *v, uint32_t n)
{
    uint32_t c, hdr = chunk_header_size(v, n);

    if (!v || n < hdr + DELTA_HDR)
        return 0;
    c = ((uint32_t)v[hdr] << 8) | v[hdr + 1];
    if (c > EFS_CHUNK_DELTA_MAX)
        return 0;
    if (n < hdr + DELTA_HDR + c * DELTA_REC)
        return 0;
    return c;
}

static int trailer_at(const uint8_t *v, uint32_t n, uint32_t i,
                      struct efs_meta_delta *d)
{
    if (i >= trailer_n(v, n))
        return EFS_ERR_INVAL;
    return unpack_delta_rec(v + chunk_header_size(v, n) + DELTA_HDR + i * DELTA_REC, d);
}

static int ranges_overlap(uint32_t a, uint32_t al, uint32_t b, uint32_t bl)
{
    if (al == 0 || bl == 0)
        return 0;
    return (uint64_t)a < (uint64_t)b + bl && (uint64_t)b < (uint64_t)a + al;
}

/* Write base image + n deltas. n == 0 keeps the historical short value
 * so a chunk with no deltas stays CHUNK_VAL bytes. */
static uint32_t pack_chunk_value(uint8_t *dst, const struct efs_meta_chunk *ch,
                                 const struct efs_meta_delta *deltas,
                                 uint32_t n)
{
    uint32_t i, hdr = CHUNK_VAL +
        (ch->coding_profile_id == EFS_META_PROFILE_PACK ? EFS_PACK_REF_BYTES : 0);

    pack_chunk(dst, ch);
    if (n == 0)
        return hdr;
    dst[hdr] = (uint8_t)(n >> 8);
    dst[hdr + 1] = (uint8_t)n;
    dst[hdr + 2] = 0;
    dst[hdr + 3] = 0;
    for (i = 0; i < n; i++)
        pack_delta_rec(dst + hdr + DELTA_HDR + i * DELTA_REC,
                       &deltas[i]);
    return hdr + DELTA_HDR + n * DELTA_REC;
}

/* True when a new publication carries the exact fragment set of the row it
 * supersedes — the raft truncate tail stub reuses the old row's placement
 * instead of writing new fragments. The fragments are shared with the live
 * row, not dead, so a CAS supersede between aliased rows must NOT queue a
 * GC record (the reaper would delete the only copy of the tail data). */
static int chunk_aliases(const struct efs_meta_chunk *a,
                         const struct efs_meta_chunk *b)
{
    if (a->coding_profile_id == EFS_META_PROFILE_PACK ||
        b->coding_profile_id == EFS_META_PROFILE_PACK) {
        uint8_t ar[EFS_PACK_REF_BYTES], br[EFS_PACK_REF_BYTES];
        if (a->coding_profile_id != b->coding_profile_id ||
            efs_pack_ref_encode(&a->packed, ar) != EFS_OK ||
            efs_pack_ref_encode(&b->packed, br) != EFS_OK ||
            memcmp(ar, br, sizeof(ar))) return 0;
    }
    return memcmp(a->nodes, b->nodes, sizeof(a->nodes)) == 0 &&
           memcmp(a->checksums, b->checksums, sizeof(a->checksums)) == 0;
}

void efs_meta_pack_gc(uint8_t *p, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      uint8_t ack_bits,
                      const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int i;

    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        efs_store_be32(p + (uint32_t)i * 4u, nodes[i]);
    p[4 * EFS_NUM_FRAGMENTS] = ack_bits;
    memset(p + 4 * EFS_NUM_FRAGMENTS + 1, 0, 3);
    memcpy(p + 4 * EFS_NUM_FRAGMENTS + 4, sums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

int efs_meta_unpack_gc(const uint8_t *p, uint32_t n,
                       efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                       uint8_t *ack_bits,
                       uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int i;

    if (!p || n < EFS_META_GC_VAL)
        return EFS_ERR_PROTO;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        nodes[i] = efs_load_be32(p + (uint32_t)i * 4u);
    *ack_bits = p[4 * EFS_NUM_FRAGMENTS];
    memcpy(sums, p + 4 * EFS_NUM_FRAGMENTS + 4,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    return EFS_OK;
}

void efs_meta_pack_reap(uint8_t *p, uint64_t generation,
                        uint64_t active_lanes)
{
    be64(p, generation);
    be64(p + 8, active_lanes);
}

int efs_meta_unpack_reap(const uint8_t *p, uint32_t n, uint64_t *generation,
                         uint64_t *active_lanes)
{
    if (!p || n < EFS_META_REAP_VAL)
        return EFS_ERR_PROTO;
    *generation = rd64(p);
    *active_lanes = rd64(p + 8);
    return EFS_OK;
}

/* D26: per-anchor pending-GC watermark (anchors are 1 = odd group,
 * 2 = even group; efs_kv_anchor_shard). Bumped in gc_queue, lowered in
 * gc_ack_apply, both inside the record's own apply. Unknown until the
 * GC thread derives it from one prefix scan (process start, snapshot
 * import); while unknown, inserts accumulate in gc_wm_pos and lowers are
 * dropped — the derive recounts live keys, so a dropped lower lands in
 * the count or not at all, never twice, and a folded insert may double
 * (upward only: an extra bounded scan, never a missed record). */
#define GC_WM_ANCHOR_MAX 2

static pthread_mutex_t gc_wm_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t gc_wm_pending[GC_WM_ANCHOR_MAX + 1];
static int64_t gc_wm_pos[GC_WM_ANCHOR_MAX + 1];
static int gc_wm_known[GC_WM_ANCHOR_MAX + 1];

void efs_meta_gc_pending_note(uint32_t anchor, int delta)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    if (gc_wm_known[anchor]) {
        gc_wm_pending[anchor] += delta;
        if (gc_wm_pending[anchor] < 0)
            gc_wm_pending[anchor] = 0;
    } else if (delta > 0) {
        gc_wm_pos[anchor] += delta;
    }
    pthread_mutex_unlock(&gc_wm_mu);
}

int64_t efs_meta_gc_pending_peek(uint32_t anchor)
{
    int64_t r;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&gc_wm_mu);
    r = gc_wm_known[anchor] ? gc_wm_pending[anchor] : -1;
    pthread_mutex_unlock(&gc_wm_mu);
    return r;
}

void efs_meta_gc_pending_derived(uint32_t anchor, uint64_t n)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    gc_wm_pending[anchor] = (int64_t)n + gc_wm_pos[anchor];
    gc_wm_pos[anchor] = 0;
    gc_wm_known[anchor] = 1;
    pthread_mutex_unlock(&gc_wm_mu);
}

void efs_meta_gc_pending_invalidate(uint32_t anchor)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    gc_wm_known[anchor] = 0;
    gc_wm_pending[anchor] = 0;
    gc_wm_pos[anchor] = 0;
    pthread_mutex_unlock(&gc_wm_mu);
}

int efs_meta_gc_pending_zero_if(uint32_t anchor, int64_t expect)
{
    int ok = 0;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&gc_wm_mu);
    if (gc_wm_known[anchor] && gc_wm_pending[anchor] == expect) {
        gc_wm_pending[anchor] = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&gc_wm_mu);
    return ok;
}

/* Queue one GC record for a dead chunk generation. The record lands on the
 * anchor shard of the chunk's lane shard, so the reaper scans one prefix
 * per group. gc_keys/gc_vals are caller storage indexed by the global item
 * number. */
static int gc_queue(struct efs_kv_item *it, uint32_t *n, uint32_t cap,
                    uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
                    uint8_t (*gc_vals)[EFS_META_GC_VAL],
                    efs_ino_t ino, uint8_t lane, uint32_t ci,
                    const struct efs_meta_chunk *dead)
{
    uint32_t kg = 0, anch;
    int rc;

    /* A packed segment is shared: its space is released through the
     * ledger (efs_pack_mapping_plan), never by per-inode GC. */
    if (dead->coding_profile_id == EFS_META_PROFILE_PACK)
        return EFS_ERR_BUSY;
    if (*n >= cap)
        return EFS_ERR_NOMEM;
    anch = efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane));
    rc = efs_kv_key_gc(anch, ino, dead->generation, lane, ci,
                       gc_keys[*n], &kg);
    if (rc != EFS_OK)
        return rc;
    efs_meta_pack_gc(gc_vals[*n], dead->nodes, 0, dead->checksums);
    it[*n].op = EFS_KV_PUT;
    it[*n].key = gc_keys[*n];
    it[*n].klen = kg;
    it[*n].val = gc_vals[*n];
    it[*n].vlen = EFS_META_GC_VAL;
    (*n)++;
    /* The bump is at queue time, not batch commit: a failed apply drifts
     * the watermark upward (an extra bounded scan), never downward. */
    efs_meta_gc_pending_note(anch, 1);
    return EFS_OK;
}


/* ---- W154 degraded marks ---- */

/* Per-anchor mark count, the D26 GC watermark's rules: unknown until the
 * rebuild pass derives it from one prefix count (process start, snapshot
 * import); while unknown, inserts accumulate and removals are dropped, so
 * the count can only overshoot, and a pass that finds the prefix empty
 * clamps it back to zero. Adjusted after a batch commits. Zero lets a
 * rewrite skip the per-superseded-object lookup. */
static pthread_mutex_t dg_wm_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t dg_wm_count[GC_WM_ANCHOR_MAX + 1];
static int64_t dg_wm_pos[GC_WM_ANCHOR_MAX + 1];
static int dg_wm_known[GC_WM_ANCHOR_MAX + 1];

static void degraded_note(uint32_t anchor, int delta)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX || !delta)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    if (dg_wm_known[anchor]) {
        dg_wm_count[anchor] += delta;
        if (dg_wm_count[anchor] < 0)
            dg_wm_count[anchor] = 0;
    } else if (delta > 0) {
        dg_wm_pos[anchor] += delta;
    }
    pthread_mutex_unlock(&dg_wm_mu);
}

int64_t efs_meta_degraded_count_peek(uint32_t anchor)
{
    int64_t r;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&dg_wm_mu);
    r = dg_wm_known[anchor] ? dg_wm_count[anchor] : -1;
    pthread_mutex_unlock(&dg_wm_mu);
    return r;
}

void efs_meta_degraded_count_derived(uint32_t anchor, uint64_t n)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    dg_wm_count[anchor] = (int64_t)n + dg_wm_pos[anchor];
    dg_wm_pos[anchor] = 0;
    dg_wm_known[anchor] = 1;
    pthread_mutex_unlock(&dg_wm_mu);
}

void efs_meta_degraded_count_invalidate(uint32_t anchor)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    dg_wm_known[anchor] = 0;
    dg_wm_count[anchor] = 0;
    dg_wm_pos[anchor] = 0;
    pthread_mutex_unlock(&dg_wm_mu);
}

int efs_meta_degraded_count_zero_if(uint32_t anchor, int64_t expect)
{
    int ok = 0;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&dg_wm_mu);
    if (dg_wm_known[anchor] && dg_wm_count[anchor] == expect) {
        dg_wm_count[anchor] = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&dg_wm_mu);
    return ok;
}

void efs_meta_pack_degraded(uint8_t *p, const struct efs_meta_degraded *d)
{
    int i;

    be64(p, d->inode_gen);
    p[8] = d->missing_mask;
    p[9] = p[10] = p[11] = 0;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        efs_store_be32(p + 12 + (uint32_t)i * 4u, d->nodes[i]);
    memcpy(p + 12 + 4 * EFS_NUM_FRAGMENTS, d->checksums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

int efs_meta_unpack_degraded(const uint8_t *key, uint32_t klen,
                             const uint8_t *val, uint32_t vlen,
                             struct efs_meta_degraded *out)
{
    uint32_t shard;
    int i;

    if (!key || !val || !out || klen != 24 || key[2] != EFS_KV_KIND_DEGRADED ||
        vlen != EFS_META_DEGRADED_VAL || val[9] || val[10] || val[11])
        return EFS_ERR_PROTO;
    memset(out, 0, sizeof(*out));
    out->ino = rd64(key + 3);
    out->object_gen = rd64(key + 11);
    out->lane = key[19];
    out->chunk_index = efs_load_be32(key + 20);
    out->inode_gen = rd64(val);
    out->missing_mask = val[8];
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        out->nodes[i] = efs_load_be32(val + 12 + (uint32_t)i * 4u);
    memcpy(out->checksums, val + 12 + 4 * EFS_NUM_FRAGMENTS,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    shard = ((uint32_t)key[0] << 8) | key[1];
    if (!out->ino || !out->inode_gen || !out->object_gen ||
        out->lane >= EFS_META_LANES ||
        out->lane != out->chunk_index % EFS_META_LANES ||
        shard != efs_kv_anchor_shard(efs_kv_lane_shard(out->ino, out->lane)) ||
        !out->missing_mask || !efs_chunk_missing_valid(out->missing_mask))
        return EFS_ERR_PROTO;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        if (!out->nodes[i] || out->nodes[i] > EFS_MAX_NODES)
            return EFS_ERR_PROTO;
    return EFS_OK;
}

static int degraded_key(efs_ino_t ino, uint64_t object_gen, uint8_t lane,
                        uint32_t ci, uint8_t *key, uint32_t *kl)
{
    return efs_kv_key_degraded(efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane)),
                               ino, object_gen, lane, ci, key, kl);
}

int efs_meta_degraded_live(struct efs_kv *kv, const struct efs_meta_degraded *d)
{
    struct efs_meta_chunk ch;
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t n = 0, i;
    int rc;

    if (!kv || !d)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_chunk_row(kv, d->ino, d->inode_gen, d->chunk_index,
                                      &ch, ds, EFS_CHUNK_DELTA_MAX, &n, NULL);
    if (rc == EFS_ERR_NOT_FOUND)
        return 0;
    if (rc != EFS_OK)
        return rc;
    /* Generations are unique object names: a match is this object, and a
     * placement that disagrees with the mark is a damaged row, not a
     * reason to rebuild anything. */
    if (ch.coding_profile_id != EFS_META_PROFILE_PACK &&
        ch.generation == d->object_gen)
        return memcmp(ch.nodes, d->nodes, sizeof(ch.nodes)) ||
               memcmp(ch.checksums, d->checksums, sizeof(ch.checksums))
                   ? EFS_ERR_PROTO : 1;
    if (n > EFS_CHUNK_DELTA_MAX)
        n = EFS_CHUNK_DELTA_MAX;
    for (i = 0; i < n; i++) {
        if (!ds[i].len || ds[i].generation != d->object_gen)
            continue;
        return memcmp(ds[i].nodes, d->nodes, sizeof(ds[i].nodes)) ||
               memcmp(ds[i].checksums, d->checksums, sizeof(ds[i].checksums))
                   ? EFS_ERR_PROTO : 1;
    }
    return 0;
}

int efs_meta_apply_degraded_clear(struct efs_kv *kv, const struct efs_meta_degraded *d,
                                  int rebuilt, int *outcome)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DEGRADED_VAL];
    uint8_t gkey[EFS_KV_KEY_MAX], gval[EFS_META_GC_VAL];
    struct efs_kv_item it[2];
    struct efs_meta_degraded cur;
    uint32_t kl = 0, gl = 0, vn, n = 0, anchor;
    int rc, found = 0, new_gc = 0, fi;

    if (outcome)
        *outcome = EFS_META_DEGRADED_ABSENT;
    if (!kv || !d || !d->ino || !d->object_gen || d->lane >= EFS_META_LANES ||
        d->lane != d->chunk_index % EFS_META_LANES ||
        !d->missing_mask || !efs_chunk_missing_valid(d->missing_mask))
        return EFS_ERR_INVAL;
    rc = degraded_key(d->ino, d->object_gen, d->lane, d->chunk_index, key, &kl);
    if (rc != EFS_OK)
        return rc;
    anchor = ((uint32_t)key[0] << 8) | key[1];
    vn = sizeof(val);
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK) {
        rc = efs_meta_unpack_degraded(key, kl, val, vn, &cur);
        if (rc != EFS_OK)
            return rc;
        /* Delete only the mark this clear was decided for. */
        if (cur.inode_gen == d->inode_gen && cur.missing_mask == d->missing_mask &&
            !memcmp(cur.nodes, d->nodes, sizeof(cur.nodes)) &&
            !memcmp(cur.checksums, d->checksums, sizeof(cur.checksums))) {
            it[n++] = (struct efs_kv_item){EFS_KV_DEL, key, kl, NULL, 0};
            found = 1;
            if (outcome)
                *outcome = EFS_META_DEGRADED_CLEARED;
        }
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    if (rebuilt) {
        int live = efs_meta_degraded_live(kv, d);
        if (live < 0)
            return live;
        if (!live) {
            uint8_t ack_bits;
            efs_node_id_t gnodes[EFS_NUM_FRAGMENTS];
            uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];

            /* A rewrite (or unlink, truncate, pack) won the race: the GC
             * record of the dead object may already have visited the
             * rebuilt node, so the fragment just written there is
             * queued again, checksum-guarded like any superseded one. */
            fi = __builtin_ctz(d->missing_mask);
            rc = efs_kv_key_gc(anchor, d->ino, d->object_gen, d->lane,
                               d->chunk_index, gkey, &gl);
            if (rc != EFS_OK)
                return rc;
            vn = sizeof(gval);
            rc = efs_kv_get(kv, gkey, gl, gval, &vn);
            if (rc == EFS_OK) {
                rc = efs_meta_unpack_gc(gval, vn, gnodes, &ack_bits, sums);
                if (rc != EFS_OK)
                    return rc;
                ack_bits &= (uint8_t)~(1u << fi);
            } else if (rc == EFS_ERR_NOT_FOUND) {
                memcpy(gnodes, d->nodes, sizeof(gnodes));
                memcpy(sums, d->checksums, sizeof(sums));
                ack_bits = (uint8_t)(((1u << EFS_NUM_FRAGMENTS) - 1u) & ~(1u << fi));
                new_gc = 1;
            } else {
                return rc;
            }
            efs_meta_pack_gc(gval, gnodes, ack_bits,
                             (const uint8_t (*)[EFS_HASH_SIZE])sums);
            it[n++] = (struct efs_kv_item){EFS_KV_PUT, gkey, gl, gval, EFS_META_GC_VAL};
            if (outcome)
                *outcome = EFS_META_DEGRADED_ORPHANED;
        }
    }
    if (!n)
        return EFS_OK;
    if (new_gc)
        efs_meta_gc_pending_note(anchor, 1);
    rc = meta_write_batch(kv, it, n);
    if (rc == EFS_OK && found)
        degraded_note(anchor, -1);
    return rc;
}

int efs_meta_apply_degraded_mark(struct efs_kv *kv, efs_ino_t ino, uint64_t inode_gen,
                                 uint64_t object_gen, uint32_t ci, uint32_t fi,
                                 efs_node_id_t node, int *added)
{
    struct efs_meta_chunk ch;
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    struct efs_meta_degraded d, cur;
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DEGRADED_VAL];
    struct efs_kv_item it;
    uint32_t n = 0, i, kl = 0, vn = sizeof(val);
    int rc, found = 0;

    if (added)
        *added = 0;
    if (!kv || !ino || !inode_gen || !object_gen || fi >= EFS_NUM_FRAGMENTS || !node ||
        node > EFS_MAX_NODES)
        return EFS_ERR_INVAL;
    memset(&d, 0, sizeof(d));
    rc = efs_meta_apply_get_chunk_row(kv, ino, inode_gen, ci, &ch, ds, EFS_CHUNK_DELTA_MAX,
                                      &n, NULL);
    if (rc != EFS_OK)
        return rc;
    /* The same object test as efs_meta_degraded_live: the base (never a
     * packed reference) or a live span with this generation. */
    if (ch.coding_profile_id != EFS_META_PROFILE_PACK && ch.generation == object_gen) {
        memcpy(d.nodes, ch.nodes, sizeof(d.nodes));
        memcpy(d.checksums, ch.checksums, sizeof(d.checksums));
        found = 1;
    }
    if (n > EFS_CHUNK_DELTA_MAX)
        n = EFS_CHUNK_DELTA_MAX;
    for (i = 0; !found && i < n; i++) {
        if (!ds[i].len || ds[i].generation != object_gen)
            continue;
        memcpy(d.nodes, ds[i].nodes, sizeof(d.nodes));
        memcpy(d.checksums, ds[i].checksums, sizeof(d.checksums));
        found = 1;
    }
    if (!found)
        return EFS_ERR_NOT_FOUND;
    if (d.nodes[fi] != node)
        return EFS_ERR_STALE;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        if (!d.nodes[i] || d.nodes[i] > EFS_MAX_NODES)
            return EFS_ERR_PROTO;
    d.ino = ino;
    d.inode_gen = inode_gen;
    d.object_gen = object_gen;
    d.chunk_index = ci;
    d.lane = (uint8_t)(ci % EFS_META_LANES);
    d.missing_mask = (uint8_t)(1u << fi);
    rc = degraded_key(ino, object_gen, d.lane, ci, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK) {
        rc = efs_meta_unpack_degraded(key, kl, val, vn, &cur);
        if (rc != EFS_OK)
            return rc;
        /* Published degraded already (or marked by an earlier fsync). */
        return cur.missing_mask == d.missing_mask ? EFS_OK : EFS_ERR_STALE;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    efs_meta_pack_degraded(val, &d);
    it = (struct efs_kv_item){EFS_KV_PUT, key, kl, val, EFS_META_DEGRADED_VAL};
    rc = meta_write_batch(kv, &it, 1);
    if (rc != EFS_OK)
        return rc;
    degraded_note(((uint32_t)key[0] << 8) | key[1], 1);
    if (added)
        *added = 1;
    return EFS_OK;
}

int efs_meta_apply_recovery_mark(struct efs_kv *kv, const struct efs_meta_degraded *d)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DEGRADED_VAL];
    struct efs_meta_degraded cur;
    uint32_t kl = 0, vn = sizeof(val), anchor;
    int rc, live;

    if (!kv || !d || !d->ino || !d->inode_gen || !d->object_gen ||
        d->lane >= EFS_META_LANES || d->lane != d->chunk_index % EFS_META_LANES ||
        !d->missing_mask || !efs_chunk_missing_valid(d->missing_mask))
        return EFS_ERR_INVAL;
    live = efs_meta_degraded_live(kv, d);
    if (live < 0)
        return live;
    if (!live)
        return EFS_ERR_NOT_FOUND;
    rc = degraded_key(d->ino, d->object_gen, d->lane, d->chunk_index, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK) {
        rc = efs_meta_unpack_degraded(key, kl, val, vn, &cur);
        if (rc != EFS_OK)
            return rc;
        /* One mark per object: a second missing fragment of a 2+1 stripe
         * cannot be rebuilt, and is reported, never overwritten. */
        return cur.inode_gen == d->inode_gen && cur.missing_mask == d->missing_mask
                   ? EFS_OK : EFS_ERR_STALE;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    anchor = ((uint32_t)key[0] << 8) | key[1];
    efs_meta_pack_degraded(val, d);
    rc = meta_write_batch(kv, &(struct efs_kv_item){EFS_KV_PUT, key, kl, val,
                                                     EFS_META_DEGRADED_VAL}, 1);
    if (rc == EFS_OK)
        degraded_note(anchor, 1);
    return rc;
}

/* One object of a chunk row placed on `node`: the mark it would carry. */
static int chunk_ref_emit(const uint8_t *key, uint64_t object_gen,
                          const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                          const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                          efs_node_id_t node, efs_meta_chunk_ref_fn cb, void *user)
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (nodes[i] != node)
            continue;
        struct efs_meta_degraded r = {
            .ino = rd64(key + 3), .inode_gen = rd64(key + 11), .object_gen = object_gen,
            .chunk_index = efs_load_be32(key + 20), .lane = key[19], .missing_mask = (uint8_t)(1u << i)};
        memcpy(r.nodes, nodes, sizeof(r.nodes));
        memcpy(r.checksums, sums, sizeof(r.checksums));
        int stop = cb(user, &r, key, EFS_KV_CHUNK_KEY_LEN);
        if (stop)
            return stop;
    }
    return 0;
}

struct chunk_refs_scan {
    const uint8_t *after;
    uint32_t alen;
    efs_node_id_t node;
    efs_meta_chunk_ref_fn cb;
    void *user;
    int rc;
};

static int chunk_refs_cb(void *user, const uint8_t *key, uint32_t klen,
                         const uint8_t *val, uint32_t vlen)
{
    struct chunk_refs_scan *c = user;
    struct efs_meta_chunk ch;
    uint32_t i, nd;
    int stop;

    if (c->alen && klen == c->alen && !memcmp(key, c->after, klen))
        return 0;
    if (klen != EFS_KV_CHUNK_KEY_LEN || key[2] != EFS_KV_KIND_CHUNK ||
        unpack_chunk(val, vlen, &ch) != EFS_OK) {
        c->rc = EFS_ERR_PROTO; /* a row recovery cannot read is not skipped */
        return 1;
    }
    if (ch.coding_profile_id != EFS_META_PROFILE_PACK && ch.generation &&
        (stop = chunk_ref_emit(key, ch.generation, ch.nodes,
                               (const uint8_t (*)[EFS_HASH_SIZE])ch.checksums,
                               c->node, c->cb, c->user)))
        return stop;
    nd = trailer_n(val, vlen);
    for (i = 0; i < nd; i++) {
        struct efs_meta_delta d;
        if (trailer_at(val, vlen, i, &d) != EFS_OK) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        if (!d.len || !d.generation)
            continue;
        stop = chunk_ref_emit(key, d.generation, d.nodes,
                              (const uint8_t (*)[EFS_HASH_SIZE])d.checksums,
                              c->node, c->cb, c->user);
        if (stop)
            return stop;
    }
    return 0;
}

int efs_meta_chunk_refs_scan(struct efs_kv *kv, uint32_t shard, const uint8_t *after,
                             uint32_t alen, efs_node_id_t node, efs_meta_chunk_ref_fn cb,
                             void *user)
{
    uint8_t prefix[3] = {(uint8_t)(shard >> 8), (uint8_t)shard, EFS_KV_KIND_CHUNK};
    struct chunk_refs_scan c = {after, alen, node, cb, user, EFS_OK};
    int rc;

    if (!kv || !cb || !node || shard > EFS_KV_SHARD_MASK || (alen && !after))
        return EFS_ERR_INVAL;
    rc = alen ? efs_kv_scan_from(kv, prefix, 3, after, alen, chunk_refs_cb, &c)
              : efs_kv_scan_prefix(kv, prefix, 3, chunk_refs_cb, &c);
    if (c.rc != EFS_OK)
        return c.rc;
    return rc;
}

/* Queue the deletion of `gen`'s degraded mark when one exists. A known-zero
 * count for the anchor skips the lookup. *removed counts queued deletes. */
static int degraded_retire(struct efs_kv *kv, struct efs_kv_item *it, uint32_t *n,
                           uint32_t cap, uint8_t (*keys)[EFS_KV_KEY_MAX], uint32_t *nk,
                           efs_ino_t ino, uint8_t lane, uint32_t ci, uint64_t gen,
                           uint32_t *removed)
{
    uint8_t val[EFS_META_DEGRADED_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    int rc;

    if (!gen)
        return EFS_OK;
    if (efs_meta_degraded_count_peek(efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane))) == 0)
        return EFS_OK;
    if (*n >= cap)
        return EFS_ERR_NOMEM;
    rc = degraded_key(ino, gen, lane, ci, keys[*nk], &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, keys[*nk], kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    it[*n] = (struct efs_kv_item){EFS_KV_DEL, keys[*nk], kl, NULL, 0};
    (*n)++;
    (*nk)++;
    (*removed)++;
    return EFS_OK;
}


static int evidence_ok(const struct efs_meta_pub *p)
{
    int i, j;

    if (!p || p->candidate_gen == 0)
        return EFS_ERR_INVAL;
    if (p->coding_profile_id != EFS_META_PROFILE_K2F1) return EFS_ERR_STALE;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (p->ch.nodes[i] == 0)
            return EFS_ERR_INVAL;
        for (j = 0; j < i; j++) {
            if (p->ch.nodes[i] == p->ch.nodes[j])
                return EFS_ERR_INVAL;
        }
    }
    return EFS_OK;
}

#define APPEND_CUR_VAL 24
#define APPEND_RSV_VAL 48
#define APPEND_OPEN 0
#define APPEND_DONE 1

struct append_cur {
    uint64_t watermark;
    uint64_t frontier;
    uint32_t nopen;
};

struct append_rsv {
    uint64_t off;
    uint64_t len;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    uint64_t seq;
    uint8_t state;
    /* K15/W64 decide-once: how a DONE row resolved (EFS_META_APPEND_*).
     * 0 while OPEN. A completion over a hole-retired row is refused. */
    uint8_t outcome;
};

static void pack_append_cur(uint8_t *p, const struct append_cur *c)
{
    be64(p + 0, c->watermark);
    be64(p + 8, c->frontier);
    efs_store_be32(p + 16, c->nopen);
    memset(p + 20, 0, 4);
}

static int unpack_append_cur(const uint8_t *p, uint32_t n, struct append_cur *c)
{
    if (!p || !c || n < APPEND_CUR_VAL)
        return EFS_ERR_PROTO;
    memset(c, 0, sizeof(*c));
    c->watermark = rd64(p + 0);
    c->frontier = rd64(p + 8);
    c->nopen = efs_load_be32(p + 16);
    return EFS_OK;
}

static void pack_append_rsv(uint8_t *p, const struct append_rsv *r)
{
    be64(p + 0, r->off);
    be64(p + 8, r->len);
    memcpy(p + 16, r->uuid, EFS_OPID_UUID_LEN);
    efs_store_be32(p + 32, r->epoch);
    be64(p + 36, r->seq);
    p[44] = r->state;
    p[45] = r->outcome;
    memset(p + 46, 0, 2);
}

static int unpack_append_rsv(const uint8_t *p, uint32_t n, struct append_rsv *r)
{
    if (!p || !r || n < 45)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->off = rd64(p + 0);
    r->len = rd64(p + 8);
    memcpy(r->uuid, p + 16, EFS_OPID_UUID_LEN);
    r->epoch = efs_load_be32(p + 32);
    r->seq = rd64(p + 36);
    r->state = p[44];
    r->outcome = n > 45 ? p[45] : 0;
    if (r->outcome > EFS_META_APPEND_FENCED_HOLE ||
        (r->state == APPEND_OPEN && r->outcome))
        return EFS_ERR_PROTO;
    return EFS_OK;
}

static int load_append_cur(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                           struct append_cur *c)
{
    uint8_t key[EFS_KV_KEY_MAX], val[APPEND_CUR_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    memset(c, 0, sizeof(*c));
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, gen, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_kv_get(kv, key, klen, val, &vlen);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_append_cur(val, vlen, c));
}

/* Which rule STALEd the last publish on this thread (raft_host logs it
 * beside the rc; the apply's verdict is the same either way). */
static __thread int pub_stale_why;

int efs_meta_apply_publish_stale_why(void)
{
    return pub_stale_why;
}

#define PUB_STALE(why)                 \
    do {                               \
        pub_stale_why = (why);         \
        return EFS_ERR_STALE;          \
    } while (0)

static void publication_key(const struct efs_meta_pub *p, uint8_t key[51])
{
    uint32_t shard=efs_kv_lane_shard(p->ino,p->chunk_index%EFS_META_LANES);
    key[0]=(uint8_t)(shard>>8);key[1]=(uint8_t)shard;key[2]=EFS_KV_KIND_PUBLICATION;
    be64(key+3,p->ino);be64(key+11,p->inode_gen);efs_store_be32(key+19,p->chunk_index);
    memcpy(key+23,p->publication_id.client_uuid,EFS_OPID_UUID_LEN);
    efs_store_be32(key+39,p->publication_id.session_epoch);be64(key+43,p->publication_id.seq);
}
/* One compact floor per FileID/chunk/mount-session. The floor survives receipt
 * deletion and inode deletion: neither expiry nor reuse may re-enable replay. */
static int publication_floor(struct efs_kv *kv, const struct efs_meta_pub *p, uint64_t *floor)
{
    uint8_t key[51], value[12]; uint32_t len=sizeof(value);
    publication_key(p,key); key[2]=EFS_KV_KIND_PUBLICATION_FLOOR;
    int rc=efs_kv_get(kv,key,43,value,&len);
    if(rc==EFS_ERR_NOT_FOUND) { *floor=0; return EFS_OK; }
    if(rc!=EFS_OK)return rc;
    if(len!=sizeof(value) || efs_load_be32(value)!=1 || !rd64(value+4))return EFS_ERR_PROTO;
    *floor=rd64(value+4); return EFS_OK;
}
#define PUBLICATION_RECEIPT_V1 (8u+EFS_HASH_SIZE)
#define PUBLICATION_RECEIPT_V2 (PUBLICATION_RECEIPT_V1+24u)
static int publication_locator_valid(const struct efs_publication_locator *p)
{
    return p->index ? (p->term && (p->group==0 || p->group==2)) : (!p->term && !p->group);
}

static int publication_receipt_valid(const uint8_t *v,uint32_t n)
{
    if (n<PUBLICATION_RECEIPT_V1 || efs_load_be32(v+4)<1 || efs_load_be32(v+4)>4) return 0;
    if (n==PUBLICATION_RECEIPT_V1) return efs_load_be32(v)==1;
    return n==PUBLICATION_RECEIPT_V2 && efs_load_be32(v)==2 &&
           (efs_load_be32(v+PUBLICATION_RECEIPT_V1)==0 || efs_load_be32(v+PUBLICATION_RECEIPT_V1)==2) &&
           !efs_load_be32(v+PUBLICATION_RECEIPT_V1+4) &&
           rd64(v+PUBLICATION_RECEIPT_V1+8) && rd64(v+PUBLICATION_RECEIPT_V1+16);
}
static uint32_t publication_receipt_pack(uint8_t *v,const struct efs_meta_pub *p,
                                        uint32_t state,const uint8_t *digest)
{
    efs_store_be32(v,p->receipt_locator.index?2:1);efs_store_be32(v+4,state);
    memcpy(v+8,digest,EFS_HASH_SIZE);
    if (!p->receipt_locator.index) return PUBLICATION_RECEIPT_V1;
    efs_store_be32(v+PUBLICATION_RECEIPT_V1,p->receipt_locator.group);
    efs_store_be32(v+PUBLICATION_RECEIPT_V1+4,0);
    be64(v+PUBLICATION_RECEIPT_V1+8,p->receipt_locator.index);
    be64(v+PUBLICATION_RECEIPT_V1+16,p->receipt_locator.term);
    return PUBLICATION_RECEIPT_V2;
}
struct publication_scan { unsigned count; uint64_t first; int rc; };
static int publication_scan_cb(void *ctx,const uint8_t *key,uint32_t klen,
                               const uint8_t *value,uint32_t vlen)
{
    struct publication_scan *s=ctx;
    if(klen!=51 || !rd64(key+43) || !publication_receipt_valid(value,vlen)) {
        s->rc=EFS_ERR_PROTO; return 1;
    }
    if(!s->count)s->first=rd64(key+43);
    return ++s->count>=EFS_META_PUBLICATION_MAX_RECEIPTS+1;
}
static int publication_scan(struct efs_kv *kv,const struct efs_meta_pub *p,
                            struct publication_scan *scan)
{
    uint8_t key[51];publication_key(p,key);memset(scan,0,sizeof(*scan));
    int rc=efs_kv_scan_prefix(kv,key,43,publication_scan_cb,scan);
    return rc==EFS_OK || rc==1 ? scan->rc : rc;
}
/* Durable publication streams are admitted on their lane shard. This check
 * runs inside serialized Raft apply, after any earlier FENCE_LOC command, so
 * an endpoint admission check cannot authorize a publish across a fence race.
 * Session rejection is not a terminal publication receipt. */
static int publication_session_accept(struct efs_kv *kv,
                                       const struct efs_meta_pub *p)
{
    return efs_session_accept(kv,
        efs_kv_lane_shard(p->ino,p->chunk_index % EFS_META_LANES),
        p->publication_id.client_uuid,p->publication_id.session_epoch);
}

/* Called only by serialized replicated apply. A durable admission commits the
 * immutable intent, not the file mapping. The caller must retain the payload
 * and submit it unchanged until a terminal result exists. */
int efs_meta_apply_publication_admit(struct efs_kv *kv,const struct efs_meta_pub *p)
{
    uint8_t digest[EFS_HASH_SIZE],key[51],value[PUBLICATION_RECEIPT_V2];int verdict;
    if(!kv || !p || !p->durable_result || !publication_locator_valid(&p->receipt_locator))return EFS_ERR_INVAL;
    int rc=efs_publication_digest(p,digest);if(rc!=EFS_OK)return rc;
    rc=publication_session_accept(kv,p);if(rc!=EFS_OK)return rc;
    rc=efs_meta_publication_result(kv,p,&verdict);
    if(rc==EFS_OK)return verdict==EFS_META_PUBLICATION_RETIRED?EFS_ERR_STALE:EFS_OK;
    if(rc!=EFS_ERR_NOT_FOUND)return rc;
    struct publication_scan scan;rc=publication_scan(kv,p,&scan);if(rc!=EFS_OK)return rc;
    if(scan.count>=EFS_META_PUBLICATION_MAX_RECEIPTS)return EFS_ERR_BUSY;
    publication_key(p,key);uint32_t value_len=publication_receipt_pack(value,p,4,digest);
    struct efs_kv_item item={EFS_KV_PUT,key,sizeof(key),value,value_len};
    return meta_write_batch(kv,&item,1);
}

int efs_meta_apply_publication_retire(struct efs_kv *kv,const struct efs_meta_pub *p)
{
    int verdict; uint64_t floor; uint8_t digest[EFS_HASH_SIZE];
    if(!kv || !p)return EFS_ERR_INVAL;
    int rc=efs_publication_digest(p,digest);if(rc!=EFS_OK)return rc;
    rc=publication_session_accept(kv,p);if(rc!=EFS_OK)return rc;
    rc=publication_floor(kv,p,&floor);if(rc!=EFS_OK)return rc;
    if(p->publication_id.seq<=floor)return EFS_OK; /* lost ACK reply */
    rc=efs_meta_publication_result(kv,p,&verdict);if(rc!=EFS_OK)return rc;
    if(verdict==EFS_META_PUBLICATION_PENDING)return EFS_ERR_BUSY;
    struct publication_scan scan;rc=publication_scan(kv,p,&scan);if(rc!=EFS_OK)return rc;
    /* Never discard another unresolved receipt while advancing the floor. */
    if(!scan.count || scan.first!=p->publication_id.seq)return EFS_ERR_BUSY;
    uint8_t key[51],fk[51],value[12];publication_key(p,key);memcpy(fk,key,sizeof(fk));
    fk[2]=EFS_KV_KIND_PUBLICATION_FLOOR;efs_store_be32(value,1);be64(value+4,p->publication_id.seq);
    struct efs_kv_item items[2]={{EFS_KV_DEL,key,sizeof(key),NULL,0},
        {EFS_KV_PUT,fk,43,value,sizeof(value)}};
    return meta_write_batch(kv,items,2);
}
int efs_meta_publication_receipt(struct efs_kv *kv, const struct efs_meta_pub *p, int *verdict,
                                  struct efs_publication_locator *locator)
{
    if(locator)memset(locator,0,sizeof(*locator));
    if(!kv || !p || !verdict)return EFS_ERR_INVAL;
    uint8_t digest[EFS_HASH_SIZE],key[51],v[PUBLICATION_RECEIPT_V2];uint32_t len=sizeof(v);
    int rc=efs_publication_digest(p,digest);if(rc!=EFS_OK)return rc;
    uint64_t floor;rc=publication_floor(kv,p,&floor);if(rc!=EFS_OK)return rc;
    if(p->publication_id.seq<=floor) { *verdict=EFS_META_PUBLICATION_RETIRED; return EFS_OK; }
    publication_key(p,key);rc=efs_kv_get(kv,key,sizeof(key),v,&len);if(rc!=EFS_OK)return rc;
    if(!publication_receipt_valid(v,len) ||
       memcmp(v+8,digest,sizeof(digest)))return EFS_ERR_PROTO;
    if(locator && len==PUBLICATION_RECEIPT_V2) {
        locator->group=efs_load_be32(v+PUBLICATION_RECEIPT_V1);
        locator->index=rd64(v+PUBLICATION_RECEIPT_V1+8);
        locator->term=rd64(v+PUBLICATION_RECEIPT_V1+16);
    }
    *verdict=efs_load_be32(v+4)==1?EFS_OK:efs_load_be32(v+4)==2?EFS_ERR_STALE:
        efs_load_be32(v+4)==3?EFS_ERR_INVAL:EFS_META_PUBLICATION_PENDING;
    return EFS_OK;
}
int efs_meta_publication_result(struct efs_kv *kv,const struct efs_meta_pub *p,int *verdict)
{
    return efs_meta_publication_receipt(kv,p,verdict,NULL);
}
/* How a publish writes: the ordinary path, a staged PREPARE's dry run, or
 * the RESOLVE that applies its own stage (W77). */
struct publish_mode {
    int dry;
    const struct efs_txid *self;
};

static int publish_inner_as(struct efs_kv *kv, const struct efs_meta_pub *p,
                            const struct efs_kv_item *receipt,
                            const struct publish_mode *mode)
{
    struct efs_meta_row row;
    struct efs_meta_chunk stored, got;
    uint8_t lane;
    uint32_t lsh, ish;
    uint8_t k_ch[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ch[CHUNK_VAL_MAX], v_ln[LANE_VAL], v_ino[INO_VAL];
    uint8_t old_ln[LANE_VAL], old_ch[CHUNK_VAL_MAX];
    /* gc_queue indexes by the global item number: chunk + lane + inode
     * + the superseded base + one record per delta. */
    uint8_t k_gc[4 + EFS_CHUNK_DELTA_MAX][EFS_KV_KEY_MAX];
    uint8_t v_gc[4 + EFS_CHUNK_DELTA_MAX][EFS_META_GC_VAL];
    uint32_t kc = 0, kl = 0, ki = 0, vn, ch_vlen = 0;
    uint64_t committed = 0;
    uint64_t inode_gen, mtime_gen, row_base_size = 0, row_active = 0;
    struct lane_rec ln;
    /* W154: plus one mark delete per superseded object and one new mark. */
    struct efs_kv_item it[11 + 2 * EFS_CHUNK_DELTA_MAX];
    uint8_t k_dg[2 + EFS_CHUNK_DELTA_MAX][EFS_KV_KEY_MAX];
    uint8_t v_dg[EFS_META_DEGRADED_VAL];
    uint32_t ndg = 0, dg_removed = 0, dg_anchor;
    int dg_added = 0;
    uint8_t k_cand[EFS_PACK_CANDIDATE_KEY_BYTES], v_cand[EFS_PACK_CANDIDATE_VAL_BYTES];
    uint32_t n = 0;
    uint32_t ch_wlen = CHUNK_VAL;
    uint32_t nd_old = 0;
    int appending = 0;
    int touch_inode = 0;
    int had_row = 0;
    int rc;
    struct efs_meta_delta dlist[EFS_CHUNK_DELTA_MAX];

    if (!kv || !p || p->ino == 0)
        return EFS_ERR_INVAL;
    rc = evidence_ok(p);
    if (rc != EFS_OK)
        return rc;
    if (!efs_chunk_missing_valid(p->missing_mask))
        return EFS_ERR_INVAL;
    if (p->durable_result && p->lane_local) {
        uint8_t ak[EFS_KV_KEY_MAX],av[EFS_META_LANE_AUTHORITY_BYTES];
        uint32_t kl=0,vl=sizeof(av),cs;uint64_t epoch,floor;
        rc=lane_authority_key(p->ino,p->inode_gen,p->chunk_index%EFS_META_LANES,ak,&kl);
        if(rc==EFS_OK)rc=efs_kv_get(kv,ak,kl,av,&vl);
        if(rc==EFS_ERR_NOT_FOUND)return EFS_ERR_BUSY; /* cold bootstrap required */
        if(rc!=EFS_OK)return rc;
        rc=unpack_lane_authority(av,vl,&cs,&epoch,&floor);if(rc!=EFS_OK)return rc;
        struct efs_meta_writer_view authority;
        rc=efs_meta_get_lane_writer_view(kv,p->ino,p->inode_gen,p->chunk_index,cs,&authority);
        if(rc!=EFS_OK)return rc;
        if(p->content_epoch!=authority.authority_epoch)return EFS_ERR_STALE;
    }
    if (p->lane_local) {
        /* Cross-group lane: this group's KV has no inode row. The host read
         * the row under ReadIndex on the inode group and carries the FileID
         * fields; the lane's own fenced_epoch is the linearizable truncate
         * fence here. An orphan publish (the inode was unlinked between the
         * host's read and this apply) is harmless: the chunk/lane keys are
         * generation-scoped garbage for GC (P3/L7), never reachable because
         * the row that would reference them is gone. */
        if (p->inode_gen == 0)
            return EFS_ERR_INVAL;
        inode_gen = p->inode_gen;
        mtime_gen = p->mtime_gen;
        memset(&row, 0, sizeof(row));
    } else {
        rc = efs_meta_apply_get_inode(kv, p->ino, &row);
        /* P3: a publish racing an unlink is stale work — make it harmless.
         * The inode row is gone, so there is nothing to attach the chunk to;
         * the fragments are orphans for GC (L7). A no-op OK, never an error:
         * the client's create for this ino committed before it could dirty the
         * chunk, so NOT_FOUND after commit ordering means deleted, not
         * not-yet-created. */
        if (rc == EFS_ERR_NOT_FOUND)
            return p->inode_gen ? EFS_ERR_STALE : EFS_OK;
        if (rc != EFS_OK)
            return rc;
        if (p->inode_gen && p->inode_gen != row.generation)
            return EFS_ERR_STALE;
        /* FUSE stores a symlink target as ordinary published bytes.
         * Directories have no chunk map. */
        if (!S_ISREG(row.mode) && !S_ISLNK(row.mode))
            return EFS_ERR_INVAL;
        if ((p->durable_result && p->content_epoch != row.content_epoch) ||
            p->content_epoch < row.content_epoch)
            PUB_STALE(EFS_PUB_STALE_ROW_EPOCH);
        {
            struct append_cur cur;

            rc = load_append_cur(kv, p->ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            if (cur.nopen && p->new_size > cur.watermark)
                return EFS_ERR_BUSY;
        }
        inode_gen = row.generation;
        mtime_gen = row.mtime_gen;
        row_base_size = row.base_size;
        row_active = row.active_lanes;
    }
    lane = (uint8_t)(p->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(p->ino, lane);
    ish = efs_kv_inode_shard(p->ino);
    rc = efs_kv_key_chunk(lsh, p->ino, inode_gen, lane, p->chunk_index,
                          k_ch, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, p->ino, inode_gen, lane, k_ln, &kl);
    if (rc != EFS_OK)
        return rc;
    memset(&got, 0, sizeof(got));
    vn = sizeof(old_ch);
    rc = efs_kv_get(kv, k_ch, kc, old_ch, &vn);
    if (rc == EFS_OK) {
        rc = unpack_chunk(old_ch, vn, &got);
        if (rc != EFS_OK)
            return rc;
        committed = got.generation;
        ch_vlen = vn;
        had_row = 1;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    /* A delta's candidate names the span object, not the base image, so
     * matching the base generation is not a replay of this publish. */
    uint64_t expect = p->unconditional && !p->delta_len ? committed : p->expected_gen;
    if (!p->delta_len && committed == p->candidate_gen) {
        if (got.coding_profile_id != p->coding_profile_id) return EFS_ERR_PROTO;
        if (!p->durable_result) return EFS_OK;
        /* A new identical write has its own identity and still updates time.
         * Missing evidence for an older CAS must not fabricate its success. */
        if (expect != committed) return EFS_ERR_STALE;
    }
    /* D1: a span attaches to whatever base is current. expected_gen is
     * the full-image CAS. Overlap and a chain of live spans still STALE. */
    if (!p->delta_len && expect != committed)
        PUB_STALE(EFS_PUB_STALE_BASE_CAS);
    vn = sizeof(old_ln);
    rc = efs_kv_get(kv, k_ln, kl, old_ln, &vn);
    if (rc == EFS_OK) {
        rc = unpack_lane(old_ln, vn, &ln);
        if (rc != EFS_OK)
            return rc;
    } else if (rc == EFS_ERR_NOT_FOUND) {
        /* D25 floor: a cross-group lane's first stamp comes only from
         * BOOTSTRAP or a fence, never from a publish, which this group
         * cannot check against the inode epoch. Same-group publishes
         * checked the row's epoch above. */
        if (p->lane_local)
            return EFS_ERR_BUSY;
        memset(&ln, 0, sizeof(ln));
        ln.fenced_epoch = p->content_epoch;
    } else {
        return rc;
    }
    if (p->durable_result && p->lane_local) mtime_gen = ln.mtime_gen;
    if (p->content_epoch < ln.fenced_epoch)
        PUB_STALE(EFS_PUB_STALE_LANE_EPOCH);
    (void)lane_adopt(&ln, p->content_epoch);
    if (ln.append_bar && p->new_size > ln.append_bar)
        return EFS_ERR_BUSY;
    nd_old = trailer_n(old_ch, ch_vlen);
    if (p->delta_len) {
        uint32_t i;

        if ((uint64_t)p->delta_off + p->delta_len > EFS_MAX_CHUNK_SIZE)
            return EFS_ERR_INVAL;
        for (i = 0; i < nd_old; i++) {
            rc = trailer_at(old_ch, ch_vlen, i, &dlist[i]);
            if (rc != EFS_OK)
                return rc;
            /* Replay of this same span record (object AND range),
             * including a len-0 tombstone left by a fold. The same
             * object under another range is another client's record
             * of the identical merged image: it is appended below like
             * any span (efs_meta_apply_chunk_holds). */
            if (dlist[i].generation == p->candidate_gen &&
                (dlist[i].len == 0 ||
                 (dlist[i].off == p->delta_off &&
                  dlist[i].len == p->delta_len))) {
                if (!p->durable_result) return EFS_OK;
                /* New identity: append even identical bytes, preserving order
                 * and epoch. Only a stored receipt proves a previous commit. */
                continue;
            }
            if (dlist[i].len == 0)
                continue;
            if (ranges_overlap(dlist[i].off, dlist[i].len, p->delta_off,
                               p->delta_len))
                PUB_STALE(EFS_PUB_STALE_OVERLAP);
        }
        {
            uint32_t live = 0, tombs, room, drop, w;

            for (i = 0; i < nd_old; i++)
                if (dlist[i].len)
                    live++;
            /* Live spans fill the chain: the client folds one image.
             * Tombstones do not count. Drop the oldest so this span fits. */
            if (live >= EFS_CHUNK_DELTA_MAX)
                PUB_STALE(EFS_PUB_STALE_CHAIN_FULL);
            tombs = nd_old - live;
            room = EFS_CHUNK_DELTA_MAX - 1u - live;
            drop = tombs > room ? tombs - room : 0;
            w = 0;
            for (i = 0; i < nd_old; i++) {
                if (dlist[i].len == 0 && drop) {
                    drop--;
                    continue;
                }
                if (w != i)
                    dlist[w] = dlist[i];
                w++;
            }
            nd_old = w;
        }
        appending = 1;
    } else {
        uint64_t newest = 0;

        if (nd_old) {
            struct efs_meta_delta last;

            rc = trailer_at(old_ch, ch_vlen, nd_old - 1, &last);
            if (rc != EFS_OK)
                return rc;
            newest = last.seq;
        }
        /* The merged image covers exactly the deltas the writer read.
         * A span that landed after that read must not be deleted. An
         * unconditional image (aligned, or a client's overflow collapse)
         * may replace spans of an older content epoch: a truncate the
         * writer saw masked them (its own previous tail, D4). A span of
         * this epoch newer than the writer's view is another writer's, and
         * still refuses it, so the writer replays and merges (K38). */
        if (p->unconditional) {
            for (uint32_t di = 0; di < nd_old; di++) {
                struct efs_meta_delta d;
                rc = trailer_at(old_ch, ch_vlen, di, &d);
                if (rc != EFS_OK)
                    return rc;
                if (d.len && d.content_epoch >= p->content_epoch && d.seq > p->delta_base_seq)
                    PUB_STALE(EFS_PUB_STALE_FOLD_LIST);
            }
        } else if (nd_old != p->delta_base_n || newest != p->delta_base_seq) {
            PUB_STALE(EFS_PUB_STALE_FOLD_LIST);
        }
    }
    ln.max_end = max_u64(ln.max_end, p->new_size);
    /* D17: the chunk row coming into existence is what st_blocks counts;
     * a CAS over a live row changes no count, and the replay returns above
     * never reach this. */
    if (!had_row)
        ln.present++;
    /* A write updates mtime AND ctime, and both live here rather than on the
     * inode row so that a million writers never touch the inode's leader.
     * MAX-clamped: CLOCK_REALTIME can step backwards, and re-applying a
     * committed entry must not move anything. */
    if (ln.mtime_gen < mtime_gen) {
        /* A utimens has invalidated this lane's mtime since it was stamped,
         * so it is not a value to take a MAX against — it is stale. */
        ln.max_mtime = p->now;
        ln.mtime_gen = mtime_gen;
    } else if (ln.mtime_gen == mtime_gen) {
        ln.max_mtime = max_u64(ln.max_mtime, p->now);
    }
    ln.max_ctime = max_u64(ln.max_ctime, p->now);
    ln.seq++;
    if (appending) {
        struct efs_meta_delta *neu = &dlist[nd_old];

        memset(neu, 0, sizeof(*neu));
        neu->off = p->delta_off;
        neu->len = p->delta_len;
        neu->generation = p->candidate_gen;
        neu->content_epoch = p->content_epoch;
        neu->seq = ln.seq;
        memcpy(neu->nodes, p->ch.nodes, sizeof(neu->nodes));
        memcpy(neu->checksums, p->ch.checksums, sizeof(neu->checksums));
        /* Keep the base image. A first span creates a generation-0 row
         * so GETCHUNKS has a key to hang the trailer on; readers treat
         * generation 0 as an empty base and overlay the spans. */
        stored = got;
        if (committed == 0) {
            memset(&stored, 0, sizeof(stored));
            stored.coding_profile_id = p->coding_profile_id;
            stored.content_epoch = p->content_epoch;
        }
        ch_wlen = pack_chunk_value(v_ch, &stored, dlist, nd_old + 1);
    } else {
        stored = p->ch;
        stored.generation = p->candidate_gen;
        stored.coding_profile_id = p->coding_profile_id;
        stored.content_epoch = p->content_epoch;
        /* D1: the folded spans' candidate_gens stay in the trailer so a
         * replay of one of them is a no-op. len 0 is not overlaid. */
        {
            uint32_t i, w = 0;

            for (i = 0; i < nd_old && w < EFS_CHUNK_DELTA_MAX; i++) {
                rc = trailer_at(old_ch, ch_vlen, i, &dlist[w]);
                if (rc != EFS_OK)
                    return rc;
                if (dlist[w].generation == 0)
                    continue;
                dlist[w].off = 0;
                dlist[w].len = 0;
                w++;
            }
            ch_wlen = pack_chunk_value(v_ch, &stored, w ? dlist : NULL, w);
        }
    }
    pack_lane(v_ln, &ln);

    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ch;
    it[n].klen = kc;
    it[n].val = v_ch;
    it[n].vlen = ch_wlen;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ln;
    it[n].klen = kl;
    it[n].val = v_ln;
    it[n].vlen = LANE_VAL;
    n++;
    /* First use of a lane has to register it in the inode row's bitmap: that
     * bitmap is the whole collect set for stat(), so a lane nobody recorded
     * is a lane stat() never reads. It happens at most 64 times in a file's
     * life, which is why it is not a rate-proportional cost. When the lane
     * is not the inode's own shard it is a genuine two-shard write: the
     * bitmap bit is set by a separate EFS_MD_CMD_ACTIVATE_LANE entry on the
     * inode group (proposed by the host BEFORE this lane-local publish), and
     * this entry — applied on the lane's group — must not touch the row. */
    if (!p->lane_local) {
        if ((row_active & (1ULL << lane)) == 0) {
            row_active |= 1ULL << lane;
            row.active_lanes = row_active;
            touch_inode = 1;
        }
        if (lsh == ish && p->new_size > row_base_size) {
            row.base_size = p->new_size;
            touch_inode = 1;
        }
    }
    if (touch_inode) {
        pack_inode(v_ino, &row);
        rc = efs_kv_key_inode(ish, p->ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ino;
        it[n].klen = ki;
        it[n].val = v_ino;
        it[n].vlen = INO_VAL;
        n++;
    }
    /* CAS supersede: the previous generation's value is overwritten in
     * place (the chunk key carries the FileID generation, not the chunk
     * generation), so its fragment set would be lost without a GC record
     * (L7). Emit one for the superseded generation — unless the new row
     * aliases the old fragment set (truncate tail stub), in which case the
     * fragments are shared with the live row and nothing is dead. */
    if (!appending && committed != 0 &&
        got.coding_profile_id != EFS_META_PROFILE_PACK &&
        got.generation != stored.generation &&
        (p->fresh_object || !chunk_aliases(&stored, &got))) {
        rc = gc_queue(it, &n, (uint32_t)(sizeof(it) / sizeof(it[0])),
                      k_gc, v_gc, p->ino, lane, p->chunk_index, &got);
        if (rc != EFS_OK)
            return rc;
    }
    /* A full image replaces the spans, including replay tombstones. Only
     * objects not named by the new base are orphans (L7/W54): a fold may
     * reuse a span's generation or alias its fragment set under a new one.
     * An append keeps the base and every earlier span. */
    if (!appending && nd_old) {
        uint32_t di;

        for (di = 0; di < nd_old; di++) {
            struct efs_meta_delta dead_d;
            struct efs_meta_chunk dead;

            rc = trailer_at(old_ch, ch_vlen, di, &dead_d);
            if (rc != EFS_OK)
                return rc;
            if (dead_d.generation == 0)
                continue;
            memset(&dead, 0, sizeof(dead));
            dead.generation = dead_d.generation;
            memcpy(dead.nodes, dead_d.nodes, sizeof(dead.nodes));
            memcpy(dead.checksums, dead_d.checksums, sizeof(dead.checksums));
            if (dead.generation == stored.generation ||
                (!p->fresh_object && chunk_aliases(&stored, &dead)))
                continue;
            rc = gc_queue(it, &n, (uint32_t)(sizeof(it) / sizeof(it[0])),
                          k_gc, v_gc, p->ino, lane, p->chunk_index, &dead);
            if (rc != EFS_OK)
                return rc;
        }
    }
    /* W154: a superseded object's degraded mark goes with it; the new
     * object records its own when a fragment was not acknowledged. */
    dg_anchor = efs_kv_anchor_shard(lsh);
    if (!appending) {
        uint32_t di;

        if (committed != 0 && got.coding_profile_id != EFS_META_PROFILE_PACK &&
            got.generation != stored.generation) {
            rc = degraded_retire(kv, it, &n, (uint32_t)(sizeof(it) / sizeof(it[0])),
                                 k_dg, &ndg, p->ino, lane, p->chunk_index,
                                 got.generation, &dg_removed);
            if (rc != EFS_OK)
                return rc;
        }
        for (di = 0; di < nd_old; di++) {
            struct efs_meta_delta dead_d;

            rc = trailer_at(old_ch, ch_vlen, di, &dead_d);
            if (rc != EFS_OK)
                return rc;
            if (dead_d.generation == 0 || dead_d.generation == stored.generation ||
                dead_d.generation == got.generation)
                continue;
            rc = degraded_retire(kv, it, &n, (uint32_t)(sizeof(it) / sizeof(it[0])),
                                 k_dg, &ndg, p->ino, lane, p->chunk_index,
                                 dead_d.generation, &dg_removed);
            if (rc != EFS_OK)
                return rc;
        }
    }
    if (p->missing_mask) {
        struct efs_meta_degraded mark;
        uint8_t have[EFS_META_DEGRADED_VAL];
        uint32_t kl_dg = 0, hn = sizeof(have);

        memset(&mark, 0, sizeof(mark));
        mark.inode_gen = inode_gen;
        mark.missing_mask = p->missing_mask;
        memcpy(mark.nodes, p->ch.nodes, sizeof(mark.nodes));
        memcpy(mark.checksums, p->ch.checksums, sizeof(mark.checksums));
        rc = degraded_key(p->ino, p->candidate_gen, lane, p->chunk_index,
                          k_dg[ndg], &kl_dg);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_get(kv, k_dg[ndg], kl_dg, have, &hn);
        if (rc == EFS_ERR_NOT_FOUND) {
            efs_meta_pack_degraded(v_dg, &mark);
            it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_dg[ndg], kl_dg, v_dg,
                                           EFS_META_DEGRADED_VAL};
            ndg++;
            dg_added = 1;
        } else if (rc != EFS_OK) {
            return rc;
        }
    }
    uint8_t ticket_key[EFS_PUT_TICKET_KEY_LEN], ticket_val[EFS_PUT_TICKET_VAL_LEN];
    if (p->ticketed) {
        struct efs_put_ticket ticket;
        rc = efs_put_ticket_from_pub(p, &ticket);
        if (rc == EFS_OK)
            rc = efs_put_ticket_publish_item(kv, &ticket, ticket_key, ticket_val, &it[n]);
        if (rc != EFS_OK) return rc;
        n++;
    }
    if (receipt) it[n++] = *receipt;
    /* W125: a small file becomes a packing candidate in the same batch;
     * so does a packed file a span publication just modified (it is
     * repacked with the span folded in). */
    if (p->chunk_index == 0 && p->new_size && p->new_size <= EFS_PACK_THRESHOLD &&
        (stored.coding_profile_id == EFS_META_PROFILE_K2F1 ||
         stored.coding_profile_id == EFS_META_PROFILE_PACK) &&
        efs_pack_candidate_key(efs_kv_anchor_shard(lsh), p->now / 1000000000ull / EFS_PACK_EPOCH_S,
                               p->ino, k_cand) == EFS_OK) {
        be64(v_cand, inode_gen);
        be64(v_cand + 8, ln.seq); /* the publish stamp */
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_cand, sizeof(k_cand), v_cand, sizeof(v_cand)};
    }
    if (got.coding_profile_id == EFS_META_PROFILE_PACK ||
        stored.coding_profile_id == EFS_META_PROFILE_PACK) {
        /* The old image is already held by this CAS: no extra lookup on the
         * ordinary path. A replaced packed reference is released in the
         * same batch, so its bytes become dead with this commit. */
        struct efs_pack_mapping_change change = {p->ino, inode_gen,
            p->chunk_index, had_row ? &got : NULL, &stored};
        struct efs_pack_mapping_plan plan = {0};
        rc = efs_pack_mapping_plan(kv, &change, 1, &plan);
        if (rc != EFS_OK) return rc;
        if (n + plan.count > sizeof(it) / sizeof(it[0])) {
            efs_pack_mapping_plan_free(&plan); return EFS_ERR_NOMEM;
        }
        memcpy(it + n, plan.items, plan.count * sizeof(it[0]));
        rc = meta_write_batch_as(kv, it, n + plan.count, mode ? mode->self : NULL,
                                 mode && mode->dry);
        efs_pack_mapping_plan_free(&plan);
    } else {
        rc = meta_write_batch_as(kv, it, n, mode ? mode->self : NULL, mode && mode->dry);
    }
    /* W154 count follows only a batch that was written: a K14 dry PREPARE
     * checks the publication without writing its marks. */
    if (rc == EFS_OK && !(mode && mode->dry))
        degraded_note(dg_anchor, dg_added - (int)dg_removed);
    return rc;
}

static int publish_inner(struct efs_kv *kv, const struct efs_meta_pub *p,
                         const struct efs_kv_item *receipt)
{
    return publish_inner_as(kv, p, receipt, NULL);
}

/* K13 REPORT publication. The ticket, not the evictable span trailer, is
 * the durable replay authority: a PUBLISHED ticket is an exact no-op (D1,
 * I16) and a settled or retired one never takes effect again (W88). A lost
 * CAS leaves the ticket ADMITTED: its owner may retry the same immutable
 * object against a fresh base, or settle (abandon) it. */
static int publish_ticketed(struct efs_kv *kv, const struct efs_meta_pub *p)
{
    struct efs_put_ticket ticket;
    uint32_t state;
    int rc = efs_put_ticket_from_pub(p, &ticket);
    if (rc != EFS_OK)
        return rc;
    rc = efs_put_ticket_state(kv, &ticket, &state);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_STALE; /* never admitted: no unticketed publication */
    if (rc != EFS_OK)
        return rc;
    if (state == EFS_PUT_TICKET_PUBLISHED)
        return EFS_OK;
    if (state != EFS_PUT_TICKET_ADMITTED)
        return EFS_ERR_STALE;
    rc = publish_inner(kv, p, NULL);
    if (rc == EFS_OK) {
        /* Every mutation carried the PUBLISHED item. A no-op leaves the
         * ticket ADMITTED; never let a later settle abandon a body a
         * mapping may name. */
        rc = efs_put_ticket_get(kv, &ticket, &state);
        if (rc == EFS_OK && state == EFS_PUT_TICKET_ADMITTED) {
            uint8_t key[EFS_PUT_TICKET_KEY_LEN], val[EFS_PUT_TICKET_VAL_LEN];
            struct efs_kv_item item;
            rc = efs_put_ticket_publish_item(kv, &ticket, key, val, &item);
            if (rc == EFS_OK)
                rc = meta_write_batch(kv, &item, 1);
        }
        return rc;
    }
    return rc;
}

/* The host found the file deleted: settle the ADMITTED ticket as REJECTED
 * (its body joins the orphan ledger at retirement). A PUBLISHED one already
 * took effect. verdict is returned after a reject (EFS_OK, P3 no-op). */
int efs_meta_apply_publish_reject(struct efs_kv *kv, const struct efs_meta_pub *p,
                                  int verdict)
{
    struct efs_put_ticket ticket;
    uint32_t state;
    if (!kv || !p || !p->ticketed || p->durable_result) return EFS_ERR_INVAL;
    int rc = efs_put_ticket_from_pub(p, &ticket);
    if (rc == EFS_OK) rc = efs_put_ticket_state(kv, &ticket, &state);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    if (state == EFS_PUT_TICKET_PUBLISHED) return EFS_OK;
    if (state == EFS_PUT_TICKET_ADMITTED) {
        rc = efs_put_ticket_reject(kv, &ticket);
        if (rc != EFS_OK) return rc;
        return verdict;
    }
    return EFS_ERR_STALE;
}

int efs_meta_apply_publish(struct efs_kv *kv, const struct efs_meta_pub *p)
{
    if (p && p->ticketed && !p->fresh_object) return EFS_ERR_INVAL;
    if (p && p->ticketed && !p->durable_result) return kv ? publish_ticketed(kv, p) : EFS_ERR_INVAL;
    if(!p || !p->durable_result) return publish_inner(kv,p,NULL);
    if(!publication_locator_valid(&p->receipt_locator))return EFS_ERR_INVAL;
    uint8_t digest[EFS_HASH_SIZE],key[51],value[PUBLICATION_RECEIPT_V2];int verdict;
    int rc=efs_publication_digest(p,digest);if(rc!=EFS_OK)return rc;
    rc=publication_session_accept(kv,p);if(rc!=EFS_OK)return rc;
    rc=efs_meta_publication_result(kv,p,&verdict);
    int pending=rc==EFS_OK && verdict==EFS_META_PUBLICATION_PENDING;
    if(rc==EFS_OK && !pending)return verdict==EFS_META_PUBLICATION_RETIRED?EFS_ERR_STALE:verdict;
    if(rc!=EFS_ERR_NOT_FOUND && !pending)return rc;
    struct publication_scan scan;rc=publication_scan(kv,p,&scan);if(rc!=EFS_OK)return rc;
    if(!pending && scan.count>=EFS_META_PUBLICATION_MAX_RECEIPTS)return EFS_ERR_BUSY;
    publication_key(p,key);
    uint32_t value_len=publication_receipt_pack(value,p,1,digest);
    struct efs_kv_item receipt={EFS_KV_PUT,key,sizeof(key),value,value_len};
    rc=publish_inner(kv,p,&receipt);
    if(rc!=EFS_OK && rc!=EFS_ERR_STALE && rc!=EFS_ERR_INVAL)return rc;
    /* A mutation included the receipt atomically. A replay/no-op or terminal
     * rejection has no mapping mutation; record its verdict before answering. */
    int found=efs_meta_publication_result(kv,p,&verdict);
    if(found==EFS_OK && verdict!=EFS_META_PUBLICATION_PENDING)return verdict;
    if(found!=EFS_ERR_NOT_FOUND && found!=EFS_OK)return found;
    efs_store_be32(value+4,rc==EFS_OK?1:rc==EFS_ERR_STALE?2:3);
    struct efs_kv_item completed[2] = {receipt}; unsigned completed_n = 1;
    uint8_t tk[EFS_PUT_TICKET_KEY_LEN], tv[EFS_PUT_TICKET_VAL_LEN];
    if (p->ticketed && rc == EFS_OK) {
        struct efs_put_ticket ticket;
        int trc = efs_put_ticket_from_pub(p, &ticket);
        if (trc == EFS_OK) trc = efs_put_ticket_publish_item(kv, &ticket, tk, tv, &completed[1]);
        if (trc != EFS_OK) return trc;
        completed_n++;
    }
    int written=meta_write_batch(kv,completed,completed_n);
    return written==EFS_OK?rc:written;
}

/* ---- W77 staged publication (meta_apply.h) ----------------------------- */

#define PUB_STAGE_VAL_MAX (16u + 1u + 4u * EFS_TXN_MAX_PART + EFS_PUB_STAGE_LEN)
#define PUB_PIN_KEY_LEN (24u + EFS_TXN_ID_LEN)
#define PUB_STAGE_F_LANE_LOCAL 1u
#define PUB_STAGE_F_FRESH_OBJECT 2u
#define PUB_STAGE_F_TICKETED 4u /* K13: mandatory */
#define PUB_STAGE_F_MISSING_SHIFT 3u /* W154 missing mask, bits 3..5 */
#define PUB_STAGE_F_MISSING_MASK (((1u << EFS_NUM_FRAGMENTS) - 1u) << PUB_STAGE_F_MISSING_SHIFT)
#define PUB_STAGE_F_UNCONDITIONAL 64u

int efs_meta_pub_stage_encode(const struct efs_meta_pub *p, const struct efs_opid *session,
                              uint8_t out[EFS_PUB_STAGE_LEN])
{
    uint8_t *q;
    int i;

    /* Only the default REPORT publication is staged: no receipt or packed
     * reference rides a staged request; its ticket (K13) and the W154 missing
     * mask do. The ticket stream is the REPORT session's. */
    if (!p || !session || !out || !p->ino || !p->inode_gen || p->durable_result ||
        !p->ticketed || p->coding_profile_id == EFS_META_PROFILE_PACK ||
        !efs_chunk_missing_valid(p->missing_mask) ||
        p->put_id.session_epoch != session->session_epoch ||
        memcmp(p->put_id.client_uuid, session->client_uuid, EFS_OPID_UUID_LEN))
        return EFS_ERR_INVAL;
    out[0] = EFS_PUB_STAGE_VERSION;
    be64(out + 1, p->ino);
    efs_store_be32(out + 9, p->chunk_index);
    be64(out + 13, p->new_size);
    be64(out + 21, p->now);
    be64(out + 29, p->expected_gen);
    be64(out + 37, p->candidate_gen);
    be64(out + 45, p->content_epoch);
    efs_store_be32(out + 53, p->coding_profile_id);
    be64(out + 57, p->inode_gen);
    be64(out + 65, p->mtime_gen);
    out[73] = (uint8_t)((p->lane_local ? PUB_STAGE_F_LANE_LOCAL : 0) |
                        (p->fresh_object ? PUB_STAGE_F_FRESH_OBJECT : 0) | PUB_STAGE_F_TICKETED |
                        (p->unconditional ? PUB_STAGE_F_UNCONDITIONAL : 0) |
                        ((uint32_t)p->missing_mask << PUB_STAGE_F_MISSING_SHIFT));
    efs_store_be32(out + 74, p->delta_off);
    efs_store_be32(out + 78, p->delta_len);
    efs_store_be32(out + 82, p->delta_base_n);
    be64(out + 86, p->delta_base_seq);
    q = out + 94;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++, q += 4)
        efs_store_be32(q, p->ch.nodes[i]);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++, q += EFS_HASH_SIZE)
        memcpy(q, p->ch.checksums[i], EFS_HASH_SIZE);
    memcpy(q, session->client_uuid, EFS_OPID_UUID_LEN);
    efs_store_be32(q + EFS_OPID_UUID_LEN, session->session_epoch);
    q += EFS_OPID_UUID_LEN + 4;
    be64(q, p->put_id.seq);
    efs_store_be32(q + 8, p->put_fragment_len);
    efs_store_be32(q + 12, p->put_member_mask);
    return EFS_OK;
}

int efs_meta_pub_stage_decode(const uint8_t *in, uint32_t n, struct efs_meta_pub *p,
                              struct efs_opid *session)
{
    const uint8_t *q;
    int i;

    if (!in || !p || n != EFS_PUB_STAGE_LEN || in[0] != EFS_PUB_STAGE_VERSION ||
        (in[73] & ~(PUB_STAGE_F_LANE_LOCAL | PUB_STAGE_F_FRESH_OBJECT | PUB_STAGE_F_TICKETED |
                    PUB_STAGE_F_MISSING_MASK | PUB_STAGE_F_UNCONDITIONAL)) ||
        !(in[73] & PUB_STAGE_F_TICKETED) ||
        !efs_chunk_missing_valid((in[73] & PUB_STAGE_F_MISSING_MASK) >> PUB_STAGE_F_MISSING_SHIFT))
        return EFS_ERR_PROTO;
    memset(p, 0, sizeof(*p));
    p->ino = rd64(in + 1);
    p->chunk_index = efs_load_be32(in + 9);
    p->new_size = rd64(in + 13);
    p->now = rd64(in + 21);
    p->expected_gen = rd64(in + 29);
    p->candidate_gen = rd64(in + 37);
    p->content_epoch = rd64(in + 45);
    p->coding_profile_id = efs_load_be32(in + 53);
    p->inode_gen = rd64(in + 57);
    p->mtime_gen = rd64(in + 65);
    p->lane_local = !!(in[73] & PUB_STAGE_F_LANE_LOCAL);
    p->fresh_object = !!(in[73] & PUB_STAGE_F_FRESH_OBJECT);
    p->unconditional = !!(in[73] & PUB_STAGE_F_UNCONDITIONAL);
    p->missing_mask = (uint8_t)((in[73] & PUB_STAGE_F_MISSING_MASK) >> PUB_STAGE_F_MISSING_SHIFT);
    p->delta_off = efs_load_be32(in + 74);
    p->delta_len = efs_load_be32(in + 78);
    p->delta_base_n = efs_load_be32(in + 82);
    p->delta_base_seq = rd64(in + 86);
    q = in + 94;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++, q += 4)
        p->ch.nodes[i] = efs_load_be32(q);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++, q += EFS_HASH_SIZE)
        memcpy(p->ch.checksums[i], q, EFS_HASH_SIZE);
    p->ticketed = 1;
    memcpy(p->put_id.client_uuid, q, EFS_OPID_UUID_LEN);
    p->put_id.session_epoch = efs_load_be32(q + EFS_OPID_UUID_LEN);
    p->put_id.seq = rd64(q + EFS_OPID_UUID_LEN + 4);
    p->put_fragment_len = efs_load_be32(q + EFS_OPID_UUID_LEN + 12);
    p->put_member_mask = efs_load_be32(q + EFS_OPID_UUID_LEN + 16);
    p->publication_id = p->put_id;
    if (session) {
        memset(session, 0, sizeof(*session));
        memcpy(session->client_uuid, q, EFS_OPID_UUID_LEN);
        session->session_epoch = efs_load_be32(q + EFS_OPID_UUID_LEN);
        session->seq = 1;
    }
    if (!p->ino || !p->inode_gen || p->coding_profile_id == EFS_META_PROFILE_PACK)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

void efs_meta_pub_pin_encode(uint64_t inode_gen, uint64_t content_epoch,
                             uint8_t out[EFS_PUB_PIN_LEN])
{
    out[0] = EFS_PUB_PIN_VERSION;
    be64(out + 1, inode_gen);
    be64(out + 9, content_epoch);
}

static int pub_stage_key(efs_ino_t ino, uint64_t gen, uint32_t ci,
                         uint8_t key[24])
{
    uint8_t lane = (uint8_t)(ci % EFS_META_LANES);
    uint32_t kl = 0;
    int rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane, ci,
                              key, &kl);
    if (rc == EFS_OK && kl != 24)
        rc = EFS_ERR_PROTO;
    key[2] = EFS_KV_KIND_PUB_STAGE;
    return rc;
}

/* Pin prefix [inode shard][PUB_STAGE][ino][gen][0xFF][0xFFFFFFFF]; each
 * publication's pin appends its txid, so writers of one file never serialize
 * on the pin (P1) while any of them keeps truncation out. */
static void pub_pin_prefix(efs_ino_t ino, uint64_t gen, uint8_t key[24])
{
    uint32_t shard = efs_kv_inode_shard(ino);
    key[0] = (uint8_t)(shard >> 8);
    key[1] = (uint8_t)shard;
    key[2] = EFS_KV_KIND_PUB_STAGE;
    be64(key + 3, ino);
    be64(key + 11, gen);
    key[19] = EFS_PUB_STAGE_PIN_LANE;
    efs_store_be32(key + 20, UINT32_MAX);
}

static int pub_stage_owner(struct efs_kv *kv, const uint8_t *chunk_key,
                           uint32_t kl, struct efs_txid *t,
                           struct efs_txn_parts *parts)
{
    uint8_t sk[24], val[PUB_STAGE_VAL_MAX];
    uint32_t vl = sizeof(val);
    int rc;

    if (kl != sizeof(sk))
        return EFS_ERR_INVAL;
    memcpy(sk, chunk_key, sizeof(sk));
    sk[2] = EFS_KV_KIND_PUB_STAGE;
    rc = efs_kv_get(kv, sk, sizeof(sk), val, &vl);
    if (rc != EFS_OK)
        return rc;
    return efs_txn_record_head_parse(val, vl, t, parts, NULL);
}

int efs_meta_pub_stage_peek(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint32_t chunk_index, struct efs_txid *t,
                            struct efs_txn_parts *parts)
{
    uint8_t key[24];
    struct efs_txid dummy;
    int rc;

    if (!kv || !ino)
        return EFS_ERR_INVAL;
    rc = pub_stage_key(ino, gen, chunk_index, key);
    if (rc != EFS_OK)
        return rc;
    key[2] = EFS_KV_KIND_CHUNK;
    return pub_stage_owner(kv, key, sizeof(key), t ? t : &dummy, parts);
}

static int pub_any_cb(void *user, const uint8_t *key, uint32_t klen,
                      const uint8_t *val, uint32_t vlen)
{
    (void)key; (void)val; (void)vlen;
    if (klen == PUB_PIN_KEY_LEN)
        *(int *)user = 1;
    return 0; /* a scan returns a nonzero callback value as its result */
}

int efs_meta_pub_pinned(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    uint8_t pref[24];
    int any = 0, rc;

    if (!kv || !ino)
        return EFS_ERR_INVAL;
    pub_pin_prefix(ino, gen, pref);
    rc = efs_kv_scan_prefix(kv, pref, sizeof(pref), pub_any_cb, &any);
    if (rc != EFS_OK)
        return rc;
    return any ? EFS_ERR_BUSY : EFS_OK;
}

int efs_meta_pub_stage_visible(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint32_t chunk_index, efs_txn_coord_fn coord,
                               void *ctx, struct efs_txn_pending *pend)
{
    struct efs_txid t;
    struct efs_txn_parts parts;
    int dec = EFS_TXN_UNDECIDED, rc;

    if (!coord)
        return EFS_ERR_INVAL;
    rc = efs_meta_pub_stage_peek(kv, ino, gen, chunk_index, &t, &parts);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    rc = coord(ctx, &t, efs_txn_coordinator(&t, &parts), &dec);
    if (rc == EFS_ERR_NOT_FOUND) {
        efs_txn_pending_note(pend, &t, &parts);
        return EFS_OK;
    }
    if (rc == EFS_ERR_STALE)
        return EFS_ERR_BUSY; /* reclaimed: unknown, never the old mapping */
    if (rc != EFS_OK)
        return rc;
    if (dec == EFS_TXN_COMMIT)
        return EFS_ERR_BUSY;
    if (dec == EFS_TXN_UNDECIDED)
        efs_txn_pending_note(pend, &t, &parts);
    return EFS_OK;
}

static int pub_parts_member(const struct efs_txn_parts *p, uint32_t shard)
{
    for (uint32_t i = 0; i < p->n; i++)
        if (p->shard[i] == shard)
            return 1;
    return 0;
}

/* A matching record of the same transaction is an idempotent re-PREPARE;
 * the same txid with another request is STALE (never a silent retry). */
/* K43: a stage row whose owner txn has a recorded ABORT is a resolve in
 * flight. The abort's resolve of such a row is the delete itself, so a
 * later unit that meets the row resolves it on contact instead of waiting
 * out the resolve — a lost coordinator's aborted unit otherwise blocked the
 * rebuild until the recovery scan's age gate (the K43 8 s window: the
 * client's resend lost its CAS and its REPORT budget ran out 0.13 s before
 * the scan). Only a locally recorded ABORT qualifies: without the decision
 * row this replica cannot tell the stage from a live one, so BUSY stands.
 * The txn record head carries the parts, so the coordinator shard is known;
 * the decision row is in this KV whenever this replica applied the DECIDE.
 */
static int pub_stage_reap_if_aborted(struct efs_kv *kv, const uint8_t *key,
                                     uint32_t klen, const uint8_t *have,
                                     uint32_t hl)
{
    struct efs_txid owner;
    struct efs_txn_parts parts;
    int dec = -1, rc;

    rc = efs_txn_record_head_parse(have, hl, &owner, &parts, NULL);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_decision_get(kv, efs_txn_coordinator(&owner, &parts),
                              &owner, &dec);
    if (rc != EFS_OK || dec != EFS_TXN_ABORT)
        return EFS_ERR_BUSY;
    rc = efs_kv_del(kv, key, klen);
    if (rc == EFS_OK)
        fprintf(stderr, "meta: reaped a publication stage on contact: owner "
                "txn was decided ABORT\n");
    return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
}

static int pub_record_same(const uint8_t *have, uint32_t hl, const struct efs_txid *t,
                           const uint8_t *pay, uint32_t plen)
{
    struct efs_txid owner;
    uint32_t used = 0;
    int rc = efs_txn_record_head_parse(have, hl, &owner, NULL, &used);
    if (rc != EFS_OK)
        return rc;
    if (memcmp(owner.bytes, t->bytes, EFS_TXN_ID_LEN))
        return EFS_ERR_BUSY;
    if (hl - used != plen)
        return EFS_ERR_STALE;
    /* The leader-stamped time (bytes 21..28) is a host timestamp, not part
     * of the request, as in efs_publication_digest. */
    if (plen == EFS_PUB_STAGE_LEN)
        return !memcmp(have + used, pay, 21) &&
               !memcmp(have + used + 29, pay + 29, plen - 29) ? EFS_OK : EFS_ERR_STALE;
    return !memcmp(have + used, pay, plen) ? EFS_OK : EFS_ERR_STALE;
}

static int pub_pin_prepare(struct efs_kv *kv, const struct efs_txid *t,
                           const struct efs_txn_parts *parts, const uint8_t *key,
                           uint32_t klen, const uint8_t *pay, uint32_t plen)
{
    struct efs_meta_row row;
    uint8_t pk[PUB_PIN_KEY_LEN], val[PUB_STAGE_VAL_MAX], have[PUB_STAGE_VAL_MAX];
    uint32_t hl = sizeof(have), shard;
    efs_ino_t ino;
    uint64_t gen, epoch;
    int head, rc;

    if (klen != 11 || plen != EFS_PUB_PIN_LEN || pay[0] != EFS_PUB_PIN_VERSION)
        return EFS_ERR_PROTO;
    ino = rd64(key + 3);
    gen = rd64(pay + 1);
    epoch = rd64(pay + 9);
    shard = ((uint32_t)key[0] << 8) | key[1];
    /* The pin is retired with the decision, so the FileID's shard must be
     * the coordinator that RETIREs it. */
    if (!ino || !gen || shard != efs_kv_inode_shard(ino) ||
        efs_txn_coordinator(t, parts) != shard)
        return EFS_ERR_INVAL;
    rc = efs_txn_key_exclusive(kv, key, klen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_STALE;
    if (rc != EFS_OK)
        return rc;
    if (row.generation != gen || row.content_epoch != epoch)
        return EFS_ERR_STALE;
    pub_pin_prefix(ino, gen, pk);
    memcpy(pk + 24, t->bytes, EFS_TXN_ID_LEN);
    rc = efs_kv_get(kv, pk, sizeof(pk), have, &hl);
    if (rc == EFS_OK)
        return pub_record_same(have, hl, t, pay, plen);
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    head = efs_txn_record_head_pack(val, sizeof(val), t, parts);
    if (head < 0)
        return head;
    memcpy(val + head, pay, plen);
    return efs_kv_put(kv, pk, sizeof(pk), val, (uint32_t)head + plen);
}

int efs_meta_apply_publication_stage(struct efs_kv *kv, const struct efs_txid *t,
                                     const struct efs_txn_parts *parts, const uint8_t *key,
                                     uint32_t klen, const uint8_t *pay, uint32_t plen)
{
    struct efs_meta_pub p;
    struct efs_opid session;
    struct publish_mode dry = {1, t};
    uint8_t ck[24], sk[24], val[PUB_STAGE_VAL_MAX], have[PUB_STAGE_VAL_MAX];
    uint32_t hl = sizeof(have), shard;
    int head, rc;

    if (!kv || !t || !key || !pay || klen < 3 || !efs_txn_parts_valid(parts))
        return EFS_ERR_PROTO;
    shard = ((uint32_t)key[0] << 8) | key[1];
    if (!pub_parts_member(parts, shard))
        return EFS_ERR_INVAL;
    if (key[2] == EFS_KV_KIND_INODE)
        return pub_pin_prepare(kv, t, parts, key, klen, pay, plen);
    if (key[2] != EFS_KV_KIND_CHUNK || klen != sizeof(ck))
        return EFS_ERR_PROTO;
    rc = efs_meta_pub_stage_decode(pay, plen, &p, &session);
    if (rc != EFS_OK)
        return rc;
    rc = pub_stage_key(p.ino, p.inode_gen, p.chunk_index, sk);
    if (rc != EFS_OK)
        return rc;
    memcpy(ck, sk, sizeof(ck));
    ck[2] = EFS_KV_KIND_CHUNK;
    if (memcmp(ck, key, sizeof(ck)))
        return EFS_ERR_INVAL; /* never stage under another chunk's key */
    rc = efs_kv_get(kv, sk, sizeof(sk), have, &hl);
    if (rc == EFS_OK) {
        rc = pub_record_same(have, hl, t, pay, plen);
        if (rc == EFS_ERR_BUSY) {
            int rrc = pub_stage_reap_if_aborted(kv, sk, sizeof(sk), have, hl);
            if (rrc == EFS_ERR_BUSY)
                return rc;   /* undecided or commit-decided: BUSY stands */
            if (rrc != EFS_OK)
                return rrc;  /* a corrupt stage row is not a retry loop */
            rc = efs_kv_get(kv, sk, sizeof(sk), have, &hl); /* re-read: reaped */
        }
        if (rc == EFS_OK || rc != EFS_ERR_NOT_FOUND)
            return rc;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    rc = efs_session_accept(kv, shard, session.client_uuid, session.session_epoch);
    if (rc != EFS_OK)
        return rc;
    /* The publish would apply now; the stage keeps every other writer of
     * this chunk out until RESOLVE, so it still applies then. */
    rc = publish_inner_as(kv, &p, NULL, &dry);
    if (rc != EFS_OK)
        return rc;
    head = efs_txn_record_head_pack(val, sizeof(val), t, parts);
    if (head < 0)
        return head;
    memcpy(val + head, pay, plen);
    return efs_kv_put(kv, sk, sizeof(sk), val, (uint32_t)head + plen);
}

struct pub_stage_scan {
    struct efs_txid t;
    int pins; /* 1: collect this txn's pins, 0: its chunk stages */
    uint32_t n;
    int overflow;
    uint8_t keys[EFS_PUB_STAGE_RESOLVE_MAX][PUB_PIN_KEY_LEN];
    uint8_t pay[EFS_PUB_STAGE_RESOLVE_MAX][EFS_PUB_STAGE_LEN];
};

static int pub_stage_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                             const uint8_t *val, uint32_t vlen)
{
    struct pub_stage_scan *a = user;
    struct efs_txid owner;
    uint32_t used = 0;

    if (klen != (a->pins ? PUB_PIN_KEY_LEN : 24u) ||
        efs_txn_record_head_parse(val, vlen, &owner, NULL, &used) != EFS_OK ||
        memcmp(owner.bytes, a->t.bytes, EFS_TXN_ID_LEN))
        return 0;
    if (a->n >= EFS_PUB_STAGE_RESOLVE_MAX) {
        a->overflow = 1;
        return 1;
    }
    memcpy(a->keys[a->n], key, klen);
    if (!a->pins) {
        if (vlen - used != EFS_PUB_STAGE_LEN)
            return 0;
        memcpy(a->pay[a->n], val + used, EFS_PUB_STAGE_LEN);
    }
    a->n++;
    return 0;
}

static int pub_stage_scan(struct efs_kv *kv, uint32_t shard, struct pub_stage_scan *a)
{
    uint8_t pref[3] = {(uint8_t)(shard >> 8), (uint8_t)shard, EFS_KV_KIND_PUB_STAGE};
    int rc = efs_kv_scan_prefix(kv, pref, sizeof(pref), pub_stage_scan_cb, a);
    return rc == 1 && a->overflow ? EFS_OK : rc; /* the callback stopped a full page */
}

/* COMMIT of one stage. A FileID that is gone (its row deleted after the
 * decision) has no reader left; every other refusal keeps the stage. */
static int pub_stage_commit(struct efs_kv *kv, const struct efs_txid *t,
                            const uint8_t *sk, const uint8_t *pay)
{
    struct efs_meta_pub p;
    struct efs_meta_row row;
    struct publish_mode self = {0, t};
    struct efs_kv_item del = {EFS_KV_DEL, sk, 24, NULL, 0};
    uint8_t probe[PUB_STAGE_VAL_MAX];
    uint32_t pl = sizeof(probe);
    int rc = efs_meta_pub_stage_decode(pay, EFS_PUB_STAGE_LEN, &p, NULL);

    if (rc != EFS_OK)
        return rc;
    rc = publish_inner_as(kv, &p, &del, &self);
    if (rc == EFS_OK) {
        /* A replay (or a P3 no-op) answers OK without writing its batch. */
        rc = efs_kv_get(kv, sk, 24, probe, &pl);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_OK;
        return rc == EFS_OK ? efs_kv_del(kv, sk, 24) : rc;
    }
    if (!p.lane_local) {
        int grc = efs_meta_apply_get_inode(kv, p.ino, &row);
        if (grc == EFS_ERR_NOT_FOUND || (grc == EFS_OK && row.generation != p.inode_gen))
            return efs_kv_del(kv, sk, 24);
    }
    return rc;
}

int efs_meta_apply_txn_resolve(struct efs_kv *kv, const struct efs_txid *t,
                               uint32_t shard, int decision)
{
    struct pub_stage_scan *a;
    int rc, first = EFS_OK;

    if (!kv || !t || (decision != EFS_TXN_COMMIT && decision != EFS_TXN_ABORT))
        return EFS_ERR_INVAL;
    a = calloc(1, sizeof(*a));
    if (!a)
        return EFS_ERR_NOMEM;
    a->t = *t;
    rc = pub_stage_scan(kv, shard & EFS_KV_SHARD_MASK, a);
    for (uint32_t i = 0; rc == EFS_OK && i < a->n; i++) {
        int one = decision == EFS_TXN_COMMIT ? pub_stage_commit(kv, t, a->keys[i], a->pay[i])
                                             : efs_kv_del(kv, a->keys[i], 24);
        if (one == EFS_ERR_IO || one == EFS_ERR_NOMEM)
            rc = one;
        else if (one != EFS_OK && first == EFS_OK)
            first = one;
    }
    if (rc == EFS_OK && a->overflow)
        first = first == EFS_OK ? EFS_ERR_BUSY : first; /* re-driven RESOLVE finishes */
    free(a);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_resolve(kv, t, shard, decision);
    return rc != EFS_OK ? rc : first;
}

int efs_meta_apply_txn_retire(struct efs_kv *kv, uint32_t coord_shard,
                              const struct efs_txid *t, uint64_t now_ns)
{
    struct pub_stage_scan *a;
    int rc = efs_txn_retire(kv, coord_shard, t, now_ns);

    if (rc != EFS_OK)
        return rc;
    a = calloc(1, sizeof(*a));
    if (!a)
        return EFS_ERR_NOMEM;
    a->t = *t;
    a->pins = 1;
    rc = pub_stage_scan(kv, coord_shard & EFS_KV_SHARD_MASK, a);
    for (uint32_t i = 0; rc == EFS_OK && i < a->n; i++)
        rc = efs_kv_del(kv, a->keys[i], PUB_PIN_KEY_LEN);
    free(a);
    return rc;
}

static int activate_lanes(struct efs_kv *kv, efs_ino_t ino, uint64_t mask,
                          int check_epoch, uint64_t expected_epoch)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (mask == 0)
        return EFS_OK;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    /* P3: the inode was unlinked between the host's read and this apply —
     * the activation is stale work, harmless to drop. */
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    if ((row.active_lanes & mask) == mask)
        return EFS_OK; /* idempotent replay */
    if (check_epoch && row.content_epoch != expected_epoch)
        return EFS_ERR_STALE;
    {
        /* D25 §3.4: a lane registered now cannot hold rows older than the
         * current epoch, so it is done for every existing fence entry. */
        struct efs_fence_history h;
        uint8_t hk[EFS_KV_KEY_MAX], hv[EFS_META_FENCE_BYTES];
        uint32_t hkl = 0, hvl = 0;
        struct efs_kv_item it[2];
        uint64_t added = mask & ~row.active_lanes;
        rc = fence_history_read(kv, ino, row.generation, EFS_META_FENCE_INODE,
                                NULL, NULL, NULL, &h);
        if (rc != EFS_OK)
            return rc;
        row.active_lanes |= mask;
        pack_inode(val, &row);
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
        if (rc != EFS_OK)
            return rc;
        it[0] = (struct efs_kv_item){EFS_KV_PUT, key, klen, val, INO_VAL};
        if (!h.count)
            return meta_write_batch(kv, it, 1);
        for (uint32_t i = 0; i < h.count; i++)
            h.entries[i].done |= added;
        rc = content_fence_key(ino, row.generation, EFS_META_FENCE_INODE, hk, &hkl);
        if (rc == EFS_OK)
            rc = efs_meta_pack_fence_history(&h, hv, sizeof(hv), &hvl);
        if (rc != EFS_OK)
            return rc;
        it[1] = (struct efs_kv_item){EFS_KV_PUT, hk, hkl, hv, hvl};
        return meta_write_batch(kv, it, 2);
    }
}

int efs_meta_apply_activate_lanes(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t mask)
{
    return activate_lanes(kv, ino, mask, 0, 0);
}

int efs_meta_apply_activate_lanes_at(struct efs_kv *kv, efs_ino_t ino,
                                     uint64_t mask, uint64_t expected_epoch)
{
    return activate_lanes(kv, ino, mask, 1, expected_epoch);
}

int efs_meta_apply_activate_lane(struct efs_kv *kv, efs_ino_t ino,
                                 uint8_t lane)
{
    if (lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    return efs_meta_apply_activate_lanes(kv, ino, 1ULL << lane);
}

int efs_meta_apply_content_fence(struct efs_kv *kv, efs_ino_t ino,
                                 uint64_t gen, uint8_t authority,
                                 uint64_t expected_epoch, uint64_t epoch,
                                 uint64_t size, uint64_t now)
{
    struct efs_fence_history history;
    struct efs_meta_row row;
    struct lane_rec lane;
    struct efs_kv_item items[3] = {0};
    uint8_t hk[EFS_KV_KEY_MAX], hv[EFS_META_FENCE_BYTES];
    uint8_t key[EFS_KV_KEY_MAX], value[INO_VAL];
    uint32_t hkl = 0, hvl = 0, kl = 0, vl = 0;
    uint8_t ak[EFS_KV_KEY_MAX], av[EFS_META_LANE_AUTHORITY_BYTES];
    uint32_t akl = 0, avl = sizeof(av), acs = 0, count = 2;
    uint64_t ae = 0, af = 0;
    uint64_t current;
    int rc;
    if (!kv || !ino || !gen || authority > EFS_META_FENCE_INODE ||
        expected_epoch == UINT64_MAX || epoch != expected_epoch + 1)
        return EFS_ERR_INVAL;
    if (authority == EFS_META_FENCE_INODE) {
        rc = efs_meta_apply_get_inode(kv, ino, &row);
        if (rc != EFS_OK)
            return rc;
        if (row.generation != gen)
            return EFS_ERR_STALE;
        if (!S_ISREG(row.mode))
            return EFS_ERR_INVAL;
        rc = efs_meta_pub_pinned(kv, ino, gen);
        if (rc != EFS_OK)
            return rc;
        current = row.content_epoch;
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &kl);
    } else {
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, authority), ino, gen,
                              authority, key, &kl);
        if (rc != EFS_OK)
            return rc;
        vl = sizeof(value);
        rc = efs_kv_get(kv, key, kl, value, &vl);
        memset(&lane, 0, sizeof(lane));
        if (rc == EFS_OK)
            rc = unpack_lane(value, vl, &lane);
        else if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
        current = lane.fenced_epoch;
    }
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_get_fence_history(kv, ino, gen, authority, &history);
    if (rc != EFS_OK)
        return rc;
    for (uint32_t i = 0; i < history.count; ++i) {
        if (history.entries[i].epoch > current)
            return EFS_ERR_PROTO;
        if (history.entries[i].epoch == epoch)
            return history.entries[i].size == size ? EFS_OK : EFS_ERR_STALE;
    }
    if (current != expected_epoch)
        return EFS_ERR_STALE;
    rc = efs_fence_history_append(&history, epoch, size);
    if (rc != EFS_OK)
        return rc; /* in particular BUSY: no stamp or size was changed */
    rc = content_fence_key(ino, gen, authority, hk, &hkl);
    if (rc == EFS_OK)
        rc = efs_meta_pack_fence_history(&history, hv, sizeof(hv), &hvl);
    if (rc != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE) {
        row.content_epoch = epoch;
        row.base_size = size;
        row.base_mtime = max_u64(row.base_mtime, now);
        row.base_ctime = max_u64(row.base_ctime, now);
        pack_inode(value, &row);
        vl = INO_VAL;
    } else {
        lane.fenced_epoch = epoch;
        lane.max_end = 0;
        lane.max_mtime = max_u64(lane.max_mtime, now);
        lane.max_ctime = max_u64(lane.max_ctime, now);
        lane.seq++;
        pack_lane(value, &lane);
        vl = LANE_VAL;
    }
    items[0] = (struct efs_kv_item){EFS_KV_PUT, hk, hkl, hv, hvl};
    items[1] = (struct efs_kv_item){EFS_KV_PUT, key, kl, value, vl};
    if (authority != EFS_META_FENCE_INODE) {
        rc = lane_authority_key(ino, gen, authority, ak, &akl);
        if (rc == EFS_OK)
            rc = efs_kv_get(kv, ak, akl, av, &avl);
        if (rc == EFS_OK) {
            rc = unpack_lane_authority(av, avl, &acs, &ae, &af);
            if (rc != EFS_OK)
                return rc;
            if (ae != expected_epoch)
                return EFS_ERR_BUSY;
            pack_lane_authority(av, acs, epoch, max_u64(af, writer_history_floor(epoch, &history)));
            items[count++] = (struct efs_kv_item){EFS_KV_PUT, ak, akl, av, sizeof(av)};
        } else if (rc != EFS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    return meta_write_batch(kv, items, count);
}

/* A lane may skip past earlier global fences only while it has no chunks.
 * Stop on the first row: this is an existence probe, never a range drain. */
static int resize_nonempty_lane(void *ctx, const uint8_t *key, uint32_t kl,
                                const uint8_t *value, uint32_t vl)
{
    (void)ctx; (void)key; (void)kl; (void)value; (void)vl;
    return EFS_ERR_PROTO;
}

int efs_meta_prepare_content_fence(struct efs_kv *kv, const struct efs_txid *t,
                                   const struct efs_txn_parts *parts,
                                   efs_ino_t ino, uint64_t gen, uint8_t authority,
                                   uint64_t expected_epoch, uint64_t expected_seq,
                                   uint64_t expected_active_lanes,
                                   uint64_t expected_base_size,
                                   uint64_t epoch, uint64_t size, uint64_t now)
{
    struct efs_fence_history history;
    struct efs_meta_row row;
    struct lane_rec lane = {0};
    uint8_t key[EFS_KV_KEY_MAX], hk[EFS_KV_KEY_MAX];
    uint8_t old[INO_VAL], value[INO_VAL];
    uint8_t old_history[EFS_META_FENCE_BYTES], new_history[EFS_META_FENCE_BYTES];
    uint32_t kl = 0, hkl = 0, old_len = sizeof(old), vl;
    uint32_t old_hlen = sizeof(old_history), hlen = 0;
    struct efs_txn_value_cas requests[3];
    uint8_t auth_key[EFS_KV_KEY_MAX], auth_old[EFS_META_LANE_AUTHORITY_BYTES],
            auth_new[EFS_META_LANE_AUTHORITY_BYTES];
    uint32_t auth_kl = 0, auth_len = sizeof(auth_old), auth_cs = 0;
    uint64_t auth_epoch = 0, auth_floor = 0;
    int has_authority = 0;
    uint32_t count = 1;
    int rc;
    if (!kv || !t || !parts || !parts->n || parts->n > EFS_TXN_MAX_PART ||
        !ino || !gen || authority > EFS_META_FENCE_INODE ||
        expected_epoch == UINT64_MAX || epoch != expected_epoch + 1)
        return EFS_ERR_INVAL;
    if (authority == EFS_META_FENCE_INODE &&
        (rc = efs_meta_pub_pinned(kv, ino, gen)) != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE)
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &kl);
    else
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, authority), ino, gen,
                              authority, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, old, &old_len);
    if (rc == EFS_ERR_NOT_FOUND && authority != EFS_META_FENCE_INODE) {
        old_len = EFS_TXN_ABSENT;
        rc = EFS_OK;
    } else if (rc == EFS_OK) {
        rc = authority == EFS_META_FENCE_INODE ?
             unpack_inode(old, old_len, &row) : unpack_lane(old, old_len, &lane);
    }
    if (rc != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE) {
        if (row.generation != gen || row.content_epoch != expected_epoch ||
            row.active_lanes != expected_active_lanes ||
            row.base_size != expected_base_size)
            return EFS_ERR_STALE;
        if (!S_ISREG(row.mode))
            return EFS_ERR_INVAL;
        for (uint32_t i = 0; i < EFS_META_LANES; ++i) {
            int member = 0;
            uint32_t shard;
            if (!(row.active_lanes & (1ull << i)))
                continue;
            shard = efs_kv_lane_shard(ino, (uint8_t)i);
            for (uint32_t j = 0; j < parts->n; ++j)
                member |= parts->shard[j] == shard;
            if (!member)
                return EFS_ERR_INVAL;
        }
    } else if (lane.fenced_epoch != expected_epoch || lane.seq != expected_seq) {
        return EFS_ERR_STALE;
    }
    if (authority != EFS_META_FENCE_INODE) {
        rc = lane_authority_key(ino, gen, authority, auth_key, &auth_kl);
        if (rc == EFS_OK)
            rc = efs_kv_get(kv, auth_key, auth_kl, auth_old, &auth_len);
        if (rc == EFS_OK) {
            rc = unpack_lane_authority(auth_old, auth_len, &auth_cs, &auth_epoch, &auth_floor);
            if (rc != EFS_OK)
                return rc;
            if (auth_epoch != expected_epoch)
                return EFS_ERR_BUSY;
            has_authority = 1;
        } else if (rc != EFS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    {
        rc = content_fence_key(ino, gen, authority, hk, &hkl);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_get(kv, hk, hkl, old_history, &old_hlen);
        if (rc == EFS_ERR_NOT_FOUND) {
            memset(&history, 0, sizeof(history));
            old_hlen = EFS_TXN_ABSENT;
        } else if (rc == EFS_OK) {
            rc = efs_meta_unpack_fence_history(old_history, old_hlen, &history);
        }
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
            return rc;
        if (history.count && history.entries[history.count - 1].epoch > expected_epoch)
            return EFS_ERR_PROTO;
        rc = efs_fence_history_append(&history, epoch, size);
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_pack_fence_history(&history, new_history, sizeof(new_history), &hlen);
        if (rc != EFS_OK)
            return rc;
    }
    if (authority == EFS_META_FENCE_INODE) {
        row.content_epoch = epoch;
        row.base_size = size;
        row.base_mtime = max_u64(row.base_mtime, now);
        row.base_ctime = max_u64(row.base_ctime, now);
        pack_inode(value, &row);
        vl = INO_VAL;
    } else {
        lane.fenced_epoch = epoch;
        lane.max_end = 0;
        lane.max_mtime = max_u64(lane.max_mtime, now);
        lane.max_ctime = max_u64(lane.max_ctime, now);
        ++lane.seq;
        pack_lane(value, &lane);
        vl = LANE_VAL;
    }
    requests[0] = (struct efs_txn_value_cas){key, kl, old, old_len,
                                            EFS_TXN_PUT, value, vl};
    requests[count++] = (struct efs_txn_value_cas){hk, hkl, old_history, old_hlen,
                                                   EFS_TXN_PUT, new_history, hlen};
    if (has_authority) {
        uint64_t floor = max_u64(writer_history_floor(epoch, &history), auth_floor);
        pack_lane_authority(auth_new, auth_cs, epoch, floor);
        requests[count++] = (struct efs_txn_value_cas){auth_key, auth_kl,
            auth_old, auth_len, EFS_TXN_PUT, auth_new, sizeof(auth_new)};
    }
    return efs_txn_prepare_excl_batch(kv, t, parts, requests, count);
}

int efs_meta_encode_fence_prepare(const struct efs_meta_fence_prepare *q,
                                  uint8_t *key, uint32_t *kl,
                                  uint8_t pay[EFS_META_FENCE_PREPARE_BYTES])
{
    int rc;
    if (!q || !key || !kl || !pay || !q->ino || !q->generation ||
        q->authority > EFS_META_FENCE_INODE || q->expected_epoch == UINT64_MAX)
        return EFS_ERR_INVAL;
    if (q->authority == EFS_META_FENCE_INODE)
        rc = efs_kv_key_inode(efs_kv_inode_shard(q->ino), q->ino, key, kl);
    else
        rc = efs_kv_key_lane(efs_kv_lane_shard(q->ino, q->authority), q->ino,
                              q->generation, q->authority, key, kl);
    if (rc != EFS_OK)
        return rc;
    be64(pay, q->ino);
    be64(pay + 8, q->generation);
    pay[16] = q->authority;
    be64(pay + 17, q->expected_epoch);
    be64(pay + 25, q->expected_seq);
    be64(pay + 33, q->expected_active_lanes);
    be64(pay + 41, q->expected_base_size);
    be64(pay + 49, q->size);
    be64(pay + 57, q->now);
    return EFS_OK;
}

int efs_meta_apply_fence_prepare(struct efs_kv *kv, const struct efs_txid *t,
                                 const struct efs_txn_parts *parts,
                                 const uint8_t *key, uint32_t kl,
                                 const uint8_t *pay, uint32_t plen)
{
    struct efs_meta_fence_prepare q;
    uint8_t expected_key[EFS_KV_KEY_MAX], encoded[EFS_META_FENCE_PREPARE_BYTES];
    uint32_t expected_kl = 0;
    int rc;
    if (!key || !pay || plen != EFS_META_FENCE_PREPARE_BYTES)
        return EFS_ERR_PROTO;
    q = (struct efs_meta_fence_prepare){rd64(pay), rd64(pay + 8), pay[16],
          rd64(pay + 17), rd64(pay + 25), rd64(pay + 33), rd64(pay + 41),
          rd64(pay + 49), rd64(pay + 57)};
    rc = efs_meta_encode_fence_prepare(&q, expected_key, &expected_kl, encoded);
    if (rc != EFS_OK || kl != expected_kl || memcmp(key, expected_key, kl))
        return EFS_ERR_PROTO; /* never mutate a different shard than the command key */
    return efs_meta_prepare_content_fence(kv, t, parts, q.ino, q.generation,
                                           q.authority, q.expected_epoch,
                                           q.expected_seq, q.expected_active_lanes,
                                           q.expected_base_size,
                                           q.expected_epoch + 1, q.size, q.now);
}

static int resize_parts_add(struct efs_txn_parts *parts, uint32_t shard)
{
    for (uint32_t i = 0; i < parts->n; ++i)
        if (parts->shard[i] == shard)
            return EFS_OK;
    if (parts->n == EFS_TXN_MAX_PART)
        return EFS_ERR_INVAL;
    parts->shard[parts->n++] = shard;
    return EFS_OK;
}

static int resize_lane_read(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                             uint8_t lane, efs_txn_coord_fn coord, void *ctx,
                             struct efs_txn_pending *pend,
                             uint8_t val[LANE_VAL], uint32_t *len,
                             struct lane_rec *stamp)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    int rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    *len = LANE_VAL;
    if (rc == EFS_OK)
        rc = efs_txn_read_ex(kv, key, kl, coord, ctx, val, len, pend);
    if (rc == EFS_ERR_NOT_FOUND) {
        *len = 0;
        memset(stamp, 0, sizeof(*stamp));
        return EFS_OK;
    }
    return rc == EFS_OK ? efs_kv_decode_result(kv, key, kl, unpack_lane(val, *len, stamp)) : rc;
}

int efs_meta_get_lane_writer_view(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t gen, uint32_t ci, uint32_t cs,
                                  struct efs_meta_writer_view *out)
{
    uint8_t lane = ci % EFS_META_LANES;
    uint8_t keys[3][EFS_KV_KEY_MAX], before[3][EFS_META_FENCE_BYTES],
            after[3][EFS_META_FENCE_BYTES];
    uint32_t kl[3], bl[3], al[3];
    if (!kv || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    int rc = lane_authority_key(ino, gen, lane, keys[0], &kl[0]);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, keys[1], &kl[1]);
    if (rc == EFS_OK)
        rc = content_fence_key(ino, gen, lane, keys[2], &kl[2]);
    if (rc != EFS_OK)
        return rc;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_writer_view view = {.ino = ino, .generation = gen};
        struct lane_rec stamp;
        uint32_t got_cs;
        int stable = 1;
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (unsigned i = 0; i < 3; ++i) {
                uint32_t *n = pass ? &al[i] : &bl[i];
                uint8_t *v = pass ? after[i] : before[i];
                rc = efs_txn_key_exclusive(kv, keys[i], kl[i]);
                if (rc != EFS_OK)
                    return rc;
                *n = EFS_META_FENCE_BYTES;
                rc = efs_kv_get(kv, keys[i], kl[i], v, n);
                /* Absence of the authority itself is a cold bootstrap request;
                 * a partially installed stamp/history is never a valid view. */
                if (rc != EFS_OK)
                    return rc == EFS_ERR_NOT_FOUND && i ? EFS_ERR_BUSY : rc;
            }
        }
        for (unsigned i = 0; i < 3; ++i) {
            rc = efs_txn_key_exclusive(kv, keys[i], kl[i]);
            if (rc != EFS_OK)
                return rc;
            stable &= bl[i] == al[i] && !memcmp(before[i], after[i], bl[i]);
        }
        if (!stable)
            continue;
        rc = efs_kv_decode_result(kv, keys[0], kl[0],
                unpack_lane_authority(before[0], bl[0], &got_cs,
                                      &view.authority_epoch, &view.oldest_complete_epoch));
        if (rc == EFS_OK)
            rc = efs_kv_decode_result(kv, keys[1], kl[1], unpack_lane(before[1], bl[1], &stamp));
        if (rc == EFS_OK)
            rc = efs_kv_decode_result(kv, keys[2], kl[2],
                    efs_meta_unpack_fence_history(before[2], bl[2], &view.history));
        if (rc != EFS_OK)
            return rc;
        if (got_cs != cs)
            return EFS_ERR_STALE;
        if (stamp.fenced_epoch != view.authority_epoch)
            return EFS_ERR_BUSY;
        if ((view.history.count && view.history.entries[view.history.count - 1].epoch >
             view.authority_epoch) || view.oldest_complete_epoch <
             writer_history_floor(view.authority_epoch, &view.history))
            return EFS_ERR_PROTO;
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_meta_capture_lane_bootstrap(struct efs_kv *kv, efs_ino_t ino,
                                    uint64_t gen, uint32_t ci, uint32_t cs,
                                    efs_txn_coord_fn coord, void *ctx,
                                    struct efs_meta_lane_bootstrap *out)
{
    if (!kv || !out || !coord || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    struct efs_meta_lane_bootstrap q = {.chunk_size = cs, .lane = ci % EFS_META_LANES};
    struct efs_meta_writer_view again;
    struct efs_meta_row row;
    struct lane_rec stamp;
    struct efs_txn_pending pending = {0};
    uint8_t value[LANE_VAL];
    uint32_t len;
    int moved = 0;
    int rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &q.view);
    if (rc == EFS_OK)
        rc = inode_read_tx(kv, ino, coord, ctx, &pending, &row);
    if (rc == EFS_OK)
        rc = resize_lane_read(kv, ino, gen, q.lane, coord, ctx, &pending, value, &len, &stamp);
    if (rc == EFS_OK)
        rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &again);
    if (rc == EFS_OK)
        rc = efs_txn_pending_recheck(&pending, coord, ctx, &moved);
    if (rc != EFS_OK)
        return rc;
    if (moved || memcmp(&q.view, &again, sizeof(again)) || row.generation != gen ||
        row.content_epoch != q.view.authority_epoch)
        return EFS_ERR_BUSY;
    if (len && stamp.fenced_epoch != q.view.authority_epoch)
        return EFS_ERR_BUSY; /* lagging populated lanes must finish coordinated fencing */
    q.active_lanes = row.active_lanes; q.base_size = row.base_size; q.mtime_gen = row.mtime_gen;
    q.lane_epoch = stamp.fenced_epoch; q.lane_seq = stamp.seq; q.lane_present = len != 0;
    *out = q;
    return EFS_OK;
}

static int lane_bootstrap_valid(const struct efs_meta_lane_bootstrap *q, uint8_t authority)
{
    if (!q || !q->view.ino || !q->view.generation || q->lane >= EFS_META_LANES ||
        (authority != q->lane && authority != EFS_META_FENCE_INODE) ||
        q->lane_present > 1 || !efs_chunk_size_valid(q->chunk_size) ||
        efs_fence_history_valid(&q->view.history) != EFS_OK ||
        q->view.oldest_complete_epoch > q->view.authority_epoch ||
        q->view.oldest_complete_epoch < writer_history_floor(q->view.authority_epoch, &q->view.history) ||
        (q->view.history.count && q->view.history.entries[q->view.history.count - 1].epoch > q->view.authority_epoch) ||
        (q->lane_present && q->lane_epoch != q->view.authority_epoch) ||
        (!q->lane_present && (q->lane_epoch || q->lane_seq)))
        return EFS_ERR_INVAL;
    return EFS_OK;
}

int efs_meta_encode_lane_bootstrap(const struct efs_meta_lane_bootstrap *q,
                                   uint8_t authority, uint8_t *key, uint32_t *kl,
                                   uint8_t pay[EFS_META_LANE_BOOTSTRAP_BYTES])
{
    if (!key || !kl || !pay || lane_bootstrap_valid(q, authority) != EFS_OK)
        return EFS_ERR_INVAL;
    int rc = authority == EFS_META_FENCE_INODE ?
        efs_kv_key_inode(efs_kv_inode_shard(q->view.ino), q->view.ino, key, kl) :
        efs_kv_key_lane(efs_kv_lane_shard(q->view.ino, q->lane), q->view.ino,
                         q->view.generation, q->lane, key, kl);
    if (rc != EFS_OK)
        return rc;
    memset(pay, 0, EFS_META_LANE_BOOTSTRAP_BYTES);
    be64(pay, q->view.ino); be64(pay + 8, q->view.generation);
    be64(pay + 16, q->view.authority_epoch); be64(pay + 24, q->view.oldest_complete_epoch);
    be64(pay + 32, q->active_lanes); be64(pay + 40, q->base_size);
    be64(pay + 48, q->lane_epoch); be64(pay + 56, q->lane_seq);
    efs_store_be32(pay + 64, q->chunk_size); pay[68] = q->lane; pay[69] = q->lane_present;
    pay[70] = authority; be64(pay + 72, q->mtime_gen);
    uint32_t len;
    return efs_meta_pack_fence_history(&q->view.history, pay + 80, EFS_META_FENCE_BYTES, &len);
}

int efs_meta_apply_lane_bootstrap(struct efs_kv *kv, const struct efs_txid *t,
                                  const struct efs_txn_parts *parts,
                                  const uint8_t *key, uint32_t kl,
                                  const uint8_t *pay, uint32_t plen)
{
    struct efs_meta_lane_bootstrap q = {0};
    uint8_t expected[EFS_KV_KEY_MAX], encoded[EFS_META_LANE_BOOTSTRAP_BYTES];
    uint8_t hk[EFS_KV_KEY_MAX], ak[EFS_KV_KEY_MAX];
    uint8_t old[INO_VAL], value[INO_VAL], hv[EFS_META_FENCE_BYTES], av[EFS_META_LANE_AUTHORITY_BYTES];
    uint32_t len, hkl, akl, old_len = sizeof(old), hlen = sizeof(hv), vl;
    struct efs_txn_value_cas requests[3];
    if (!kv || !t || !parts || !key || !pay || plen != EFS_META_LANE_BOOTSTRAP_BYTES ||
        efs_load_be32(pay + 80 + 4) > EFS_FENCE_HISTORY_MAX)
        return EFS_ERR_PROTO;
    q.view.ino = rd64(pay); q.view.generation = rd64(pay + 8);
    q.view.authority_epoch = rd64(pay + 16); q.view.oldest_complete_epoch = rd64(pay + 24);
    q.active_lanes = rd64(pay + 32); q.base_size = rd64(pay + 40);
    q.lane_epoch = rd64(pay + 48); q.lane_seq = rd64(pay + 56);
    q.chunk_size = efs_load_be32(pay + 64); q.lane = pay[68]; q.lane_present = pay[69];
    uint8_t authority = pay[70]; q.mtime_gen = rd64(pay + 72);
    int rc = efs_meta_unpack_fence_history(pay + 80, 16u + 24u * efs_load_be32(pay + 84), &q.view.history);
    if (rc != EFS_OK || efs_meta_encode_lane_bootstrap(&q, authority, expected, &len, encoded) != EFS_OK ||
        kl != len || memcmp(key, expected, kl) || memcmp(pay, encoded, plen))
        return EFS_ERR_PROTO;
    /* Freeze the bitmap before adopting the lane. Both shards must participate,
     * even when inode and lane happen to share a shard or a Raft group. */
    struct efs_txn_parts needed = {0};
    resize_parts_add(&needed, efs_kv_inode_shard(q.view.ino));
    resize_parts_add(&needed, efs_kv_lane_shard(q.view.ino, q.lane));
    if (parts->n != needed.n || memcmp(parts->shard, needed.shard, needed.n * sizeof(uint32_t)))
        return EFS_ERR_INVAL;
    rc = efs_kv_get(kv, key, kl, old, &old_len);
    if (rc == EFS_ERR_NOT_FOUND && authority != EFS_META_FENCE_INODE) {
        old_len = EFS_TXN_ABSENT; rc = EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    rc = content_fence_key(q.view.ino, q.view.generation, authority, hk, &hkl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, hk, hkl, hv, &hlen);
    if (rc == EFS_ERR_NOT_FOUND) { hlen = EFS_TXN_ABSENT; rc = EFS_OK; }
    if (rc != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE) {
        struct efs_meta_row row;
        struct efs_fence_history history = {0};
        rc = unpack_inode(old, old_len, &row);
        if (rc == EFS_OK && hlen != EFS_TXN_ABSENT)
            rc = efs_meta_unpack_fence_history(hv, hlen, &history);
        if (rc != EFS_OK)
            return rc;
        if (row.generation != q.view.generation || row.content_epoch != q.view.authority_epoch ||
            row.active_lanes != q.active_lanes || row.base_size != q.base_size || row.mtime_gen != q.mtime_gen ||
            memcmp(&history, &q.view.history, sizeof(history)) ||
            q.view.oldest_complete_epoch != writer_history_floor(row.content_epoch, &history))
            return EFS_ERR_STALE;
        if (!S_ISREG(row.mode))
            return EFS_ERR_INVAL;
        row.active_lanes |= 1ull << q.lane;
        pack_inode(value, &row); vl = INO_VAL;
    } else {
        struct lane_rec stamp = {0};
        if (old_len != EFS_TXN_ABSENT && unpack_lane(old, old_len, &stamp) != EFS_OK)
            return EFS_ERR_PROTO;
        if ((old_len != EFS_TXN_ABSENT) != q.lane_present || stamp.seq != q.lane_seq ||
            stamp.fenced_epoch != q.lane_epoch)
            return EFS_ERR_STALE;
        if (stamp.seq == UINT64_MAX || stamp.append_bar)
            return EFS_ERR_BUSY;
        /* A missing stamp with existing chunks cannot be re-aged as a new lane. */
        if (!q.lane_present) {
            uint8_t prefix[EFS_KV_KEY_MAX]; uint32_t pl;
            rc = efs_kv_key_chunk(efs_kv_lane_shard(q.view.ino, q.lane), q.view.ino,
                                  q.view.generation, q.lane, 0, prefix, &pl);
            if (rc == EFS_OK)
                rc = efs_kv_scan_prefix(kv, prefix, pl - 4, resize_nonempty_lane, NULL);
            if (rc != EFS_OK)
                return rc;
        }
        if (stamp.mtime_gen > q.mtime_gen)
            return EFS_ERR_BUSY;
        if (stamp.mtime_gen < q.mtime_gen) {
            stamp.mtime_gen = q.mtime_gen;
            stamp.max_mtime = 0;
        }
        stamp.fenced_epoch = q.view.authority_epoch; ++stamp.seq;
        pack_lane(value, &stamp); vl = LANE_VAL;
        lane_authority_key(q.view.ino, q.view.generation, q.lane, ak, &akl);
        pack_lane_authority(av, q.chunk_size, q.view.authority_epoch, q.view.oldest_complete_epoch);
    }
    requests[0] = (struct efs_txn_value_cas){key, kl, old, old_len, EFS_TXN_PUT, value, vl};
    uint32_t nh = 16u + 24u * q.view.history.count;
    requests[1] = (struct efs_txn_value_cas){hk, hkl, hv, hlen, EFS_TXN_PUT, pay + 80, nh};
    if (authority != EFS_META_FENCE_INODE)
        requests[2] = (struct efs_txn_value_cas){ak, akl, NULL, EFS_TXN_ABSENT, EFS_TXN_PUT, av, sizeof(av)};
    return efs_txn_prepare_excl_batch(kv, t, parts, requests,
                                      authority == EFS_META_FENCE_INODE ? 2 : 3);
}

int efs_meta_get_writer_chunk_view_tx(struct efs_kv *kv, efs_ino_t ino,
                                      uint64_t gen, uint32_t ci,
                                      efs_txn_coord_fn coord, void *ctx,
                                      struct efs_meta_writer_view *out)
{
    if (!kv || !ino || !gen || !coord || !out)
        return EFS_ERR_INVAL;
    uint8_t lane = (uint8_t)(ci % EFS_META_LANES);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_writer_view view, again;
        struct efs_meta_row row;
        struct lane_rec before, after;
        struct efs_txn_pending pend = {0};
        uint8_t b[LANE_VAL], a[LANE_VAL];
        uint32_t bl, al;
        int moved = 0;
        int rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &view);
        if (rc == EFS_OK)
            rc = resize_lane_read(kv, ino, gen, lane, coord, ctx, &pend, b, &bl, &before);
        if (rc == EFS_OK)
            rc = inode_read_tx(kv, ino, coord, ctx, &pend, &row);
        if (rc == EFS_OK)
            rc = resize_lane_read(kv, ino, gen, lane, coord, ctx, &pend, a, &al, &after);
        if (rc == EFS_OK)
            rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &again);
        if (rc == EFS_OK)
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved || bl != al || memcmp(b, a, bl) ||
            memcmp(&view, &again, sizeof(view)) || row.generation != gen ||
            row.content_epoch != view.authority_epoch)
            continue;
        if (!bl) {
            if (row.active_lanes & (1ull << lane))
                return EFS_ERR_BUSY;
        } else if (before.fenced_epoch != view.authority_epoch) {
            return EFS_ERR_BUSY;
        }
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_meta_capture_mtime(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           uint32_t mask, uint64_t mtime, uint64_t atime,
                           efs_txn_coord_fn coord, void *ctx,
                           struct efs_meta_mtime_plan *out)
{
    struct efs_meta_row row;
    struct efs_txn_pending pending = {0};
    int moved = 0;
    if (!kv || !ino || !coord || !out || !(mask & EFS_META_SET_MTIME) ||
        (mask & ~(EFS_META_SET_MTIME | EFS_META_SET_ATIME)))
        return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    int rc = inode_read_tx(kv, ino, coord, ctx, &pending, &row);
    if (rc != EFS_OK) return rc;
    if (!S_ISREG(row.mode) || row.mtime_gen == UINT64_MAX)
        return EFS_ERR_INVAL;
    uint64_t generation = row.generation, bits = row.active_lanes;
    uint64_t epoch = row.content_epoch, next = row.mtime_gen + 1;
    struct efs_meta_mtime_image *image = &out->images[out->count++];
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, image->key, &image->klen);
    if (rc != EFS_OK) return rc;
    image->expected_len = image->new_len = INO_VAL;
    pack_inode(image->expected, &row);
    row.mtime_gen = next; row.base_mtime = mtime;
    if (mask & EFS_META_SET_ATIME) row.base_atime = atime;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(image->value, &row);
    rc = resize_parts_add(&out->parts, efs_kv_inode_shard(ino));
    for (uint8_t lane = 0; lane < EFS_META_LANES && rc == EFS_OK; ++lane) {
        struct lane_rec stamp;
        uint32_t len;
        if (!(bits & (1ull << lane))) continue;
        image = &out->images[out->count++];
        rc = resize_lane_read(kv, ino, generation, lane, coord, ctx,
                              &pending, image->expected, &len, &stamp);
        if (rc != EFS_OK) return rc;
        /* A lane ahead of the row signals an unfinished legacy fanout. */
        if (stamp.mtime_gen >= next || stamp.seq == UINT64_MAX)
            return EFS_ERR_BUSY;
        /* D25: a lane behind the inode epoch adopts it here; ahead is a race. */
        if (len && stamp.fenced_epoch > epoch) return EFS_ERR_BUSY;
        if (len) (void)lane_adopt(&stamp, epoch);
        image->expected_len = len ? len : EFS_TXN_ABSENT;
        image->new_len = LANE_VAL;
        stamp.fenced_epoch = epoch; stamp.mtime_gen = next;
        stamp.max_mtime = 0; ++stamp.seq;
        pack_lane(image->value, &stamp);
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, generation,
                             lane, image->key, &image->klen);
        if (rc == EFS_OK) rc = resize_parts_add(&out->parts, efs_kv_lane_shard(ino, lane));
    }
    if (rc == EFS_OK) rc = efs_txn_pending_recheck(&pending, coord, ctx, &moved);
    return rc != EFS_OK ? rc : moved ? EFS_ERR_BUSY : EFS_OK;
}

int efs_meta_execute_mtime(const struct efs_meta_mtime_plan *plan,
                           const struct efs_txid *t,
                           const struct efs_meta_mtime_ops *ops, void *ctx)
{
    struct efs_txn_parts expected = {0};
    struct efs_meta_row before, after;
    uint64_t bits = 0;
    if (!plan || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve ||
        !plan->count || plan->count > EFS_META_LANES + 1)
        return EFS_ERR_INVAL;
    const struct efs_meta_mtime_image *inode = &plan->images[0];
    if (inode->expected_len != INO_VAL || inode->new_len != INO_VAL ||
        unpack_inode(inode->expected, INO_VAL, &before) != EFS_OK ||
        unpack_inode(inode->value, INO_VAL, &after) != EFS_OK ||
        !S_ISREG(before.mode) || before.mtime_gen == UINT64_MAX ||
        after.mtime_gen != before.mtime_gen + 1)
        return EFS_ERR_INVAL;
    struct efs_meta_row unchanged = after;
    unchanged.base_mtime = before.base_mtime; unchanged.base_atime = before.base_atime;
    unchanged.base_ctime = before.base_ctime; unchanged.mtime_gen = before.mtime_gen;
    uint8_t original[INO_VAL], normalized[INO_VAL];
    pack_inode(original, &before); pack_inode(normalized, &unchanged);
    if (memcmp(original, normalized, INO_VAL) || after.base_ctime < before.base_ctime)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->count; ++i) {
        const struct efs_meta_mtime_image *image = &plan->images[i];
        uint8_t key[EFS_KV_KEY_MAX]; uint32_t kl;
        int rc;
        if (!i) rc = efs_kv_key_inode(efs_kv_inode_shard(before.ino), before.ino, key, &kl);
        else {
            struct lane_rec stamp;
            if (image->klen != 20 || image->new_len != LANE_VAL ||
                (image->expected_len != LANE_VAL && image->expected_len != EFS_TXN_ABSENT) ||
                image->key[19] >= EFS_META_LANES ||
                unpack_lane(image->value, LANE_VAL, &stamp) != EFS_OK ||
                stamp.mtime_gen != after.mtime_gen || stamp.max_mtime ||
                stamp.fenced_epoch != before.content_epoch)
                return EFS_ERR_INVAL;
            struct lane_rec old = {0};
            if (image->expected_len != EFS_TXN_ABSENT &&
                unpack_lane(image->expected, LANE_VAL, &old) != EFS_OK)
                return EFS_ERR_INVAL;
            if (old.seq == UINT64_MAX || old.mtime_gen >= after.mtime_gen ||
                (image->expected_len != EFS_TXN_ABSENT && old.fenced_epoch > before.content_epoch))
                return EFS_ERR_INVAL;
            if (image->expected_len != EFS_TXN_ABSENT)
                (void)lane_adopt(&old, before.content_epoch);
            old.fenced_epoch = before.content_epoch; old.mtime_gen = after.mtime_gen;
            old.max_mtime = 0; ++old.seq;
            uint8_t encoded[LANE_VAL]; pack_lane(encoded, &old);
            if (memcmp(encoded, image->value, LANE_VAL)) return EFS_ERR_INVAL;
            uint8_t lane = image->key[19];
            if (bits & (1ull << lane)) return EFS_ERR_INVAL;
            bits |= 1ull << lane;
            rc = efs_kv_key_lane(efs_kv_lane_shard(before.ino, lane), before.ino,
                                  before.generation, lane, key, &kl);
        }
        if (rc != EFS_OK || image->klen != kl || memcmp(key, image->key, kl))
            return EFS_ERR_INVAL;
        rc = resize_parts_add(&expected, ((uint32_t)key[0] << 8) | key[1]);
        if (rc != EFS_OK) return rc;
    }
    if (bits != before.active_lanes || expected.n != plan->parts.n ||
        memcmp(expected.shard, plan->parts.shard, expected.n * sizeof(uint32_t)))
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->count; ++i) {
        int rc = ops->prepare(ctx, t, &plan->parts, &plan->images[i]);
        if (rc != EFS_OK) {
            int abort_rc = ops->decide(ctx, t, &plan->parts, EFS_TXN_ABORT);
            if (abort_rc != EFS_OK) return abort_rc;
            (void)ops->resolve(ctx, t, &plan->parts, EFS_TXN_ABORT);
            return rc;
        }
    }
    int rc = ops->decide(ctx, t, &plan->parts, EFS_TXN_COMMIT);
    if (rc != EFS_OK) return rc; /* ambiguous COMMIT never becomes ABORT */
    return ops->resolve(ctx, t, &plan->parts, EFS_TXN_COMMIT);
}

static int epoch_lane_upd(struct lane_rec *ln, void *arg);
static int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino,
                           uint64_t generation, uint64_t bits,
                           int (*upd)(struct lane_rec *ln, void *arg),
                           void *arg, struct efs_kv_item *it, uint32_t *n,
                           uint8_t k_ln[][EFS_KV_KEY_MAX],
                           uint8_t v_ln[][LANE_VAL]);
/* A packed file owns one base, at chunk zero. Capture its checked image
 * without side effects; the final batch releases that reference together
 * with the mapping, fence and EOF update. */
struct packed_retirement {
    struct efs_meta_chunk before, after;
    uint8_t present, has_after;
};
static int packed_retirement_commit(struct efs_kv *kv, efs_ino_t ino,
    uint64_t gen, struct packed_retirement *retirement,
    struct efs_kv_item *it, uint32_t n, uint32_t cap)
{
    if (!retirement->present) return meta_write_batch(kv, it, n);
    struct efs_pack_mapping_change change = {ino, gen, 0,
        &retirement->before, retirement->has_after ? &retirement->after : NULL};
    struct efs_pack_mapping_plan plan = {0};
    int rc = efs_pack_mapping_plan(kv, &change, 1, &plan);
    if (rc == EFS_OK && plan.count > cap - n) rc = EFS_ERR_NOMEM;
    if (rc == EFS_OK) {
        memcpy(it + n, plan.items, plan.count * sizeof(it[0]));
        rc = meta_write_batch(kv, it, n + plan.count);
    }
    efs_pack_mapping_plan_free(&plan);
    return rc;
}

/* Lane sweep batch: 64 chunk keys per KV batch (each a DEL + a GC record),
 * then the lane key. 64 keeps one entry's apply cost bounded while still
 * draining a 1 TB file's lane (~8192 chunks) in ~128 batches inside the
 * single LANE_SWEEP entry — tens of ms, far under the election deadline. */
#define SWEEP_CHUNKS 64

struct sweep_scan {
    efs_ino_t ino;
    uint64_t gen;
    struct efs_kv_item *it;
    uint8_t (*keys)[EFS_KV_KEY_MAX];
    uint8_t (*gc_vals)[EFS_META_GC_VAL];
    uint32_t n;
    uint32_t cap;
    uint32_t chunks;
    struct efs_meta_chunk packed_dead;
    uint32_t packed_ci;
    int has_packed;
    int full;
    int rc;
    int nogc; /* W124 reaped inode: node fences delete the whole inode dir */
};

/* A packed file has exactly one base, at chunk zero. Its reference is
 * released in the same atomic batch as the authoritative delete; that holds
 * for a W124 reaped inode too, whose directory never holds the segment. */
static int sweep_commit(struct efs_kv *kv, struct sweep_scan *ss)
{
    if (!ss->has_packed) return meta_write_batch(kv, ss->it, ss->n);
    struct efs_pack_mapping_change change = {ss->ino, ss->gen,
        ss->packed_ci, &ss->packed_dead, NULL};
    struct efs_pack_mapping_plan plan = {0};
    int rc = efs_pack_mapping_plan(kv, &change, 1, &plan);
    if (rc == EFS_OK && plan.count > ss->cap - ss->n) rc = EFS_ERR_NOMEM;
    if (rc == EFS_OK) {
        memcpy(ss->it + ss->n, plan.items, plan.count * sizeof(ss->it[0]));
        rc = meta_write_batch(kv, ss->it, ss->n + plan.count);
    }
    efs_pack_mapping_plan_free(&plan);
    return rc;
}

/* Reserve every dead identity before queuing its only reference's deletion. */
static int chunk_gc_slots(const uint8_t *val, uint32_t vlen,
                           const struct efs_meta_chunk *dead, uint32_t *slots)
{
    uint32_t nd = 0, hdr = chunk_header_size(val, vlen);
    if (vlen != hdr) {
        if (vlen < hdr + DELTA_HDR)
            return EFS_ERR_PROTO;
        nd = ((uint32_t)val[hdr] << 8) | val[hdr + 1];
        if (nd > EFS_CHUNK_DELTA_MAX ||
            vlen < hdr + DELTA_HDR + nd * DELTA_REC)
            return EFS_ERR_PROTO;
    }
    *slots = 1u + (dead->generation != 0);
    for (uint32_t i = 0; i < nd; i++) {
        struct efs_meta_delta dd;
        int rc = trailer_at(val, vlen, i, &dd);
        if (rc != EFS_OK)
            return rc;
        *slots += dd.generation != 0;
    }
    return EFS_OK;
}

static int sweep_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct sweep_scan *ss = user;
    struct efs_meta_chunk dead;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci, need;
    uint8_t lane;

    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK)
        return 0;
    ino = rd64(key + 3);
    gen = rd64(key + 11);
    lane = key[19];
    ci = efs_load_be32(key + 20);
    if (ino != ss->ino || gen != ss->gen)
        return 0;
    ss->rc = unpack_chunk(val, vlen, &dead);
    if (ss->rc == EFS_OK)
        ss->rc = chunk_gc_slots(val, vlen, &dead, &need);
    if (ss->rc != EFS_OK)
        return 1;
    if (need > ss->cap - ss->n - (ss->has_packed ? 1u : 0u)) {
        ss->full = 1;
        return 1;
    }
    memcpy(ss->keys[ss->n], key, klen);
    ss->it[ss->n].op = EFS_KV_DEL;
    ss->it[ss->n].key = ss->keys[ss->n];
    ss->it[ss->n].klen = klen;
    ss->it[ss->n].val = NULL; /* `it` is an uninitialised stack array */
    ss->it[ss->n].vlen = 0;
    ss->n++;
    if (dead.coding_profile_id == EFS_META_PROFILE_PACK) {
        if (ci || lane || ss->has_packed) { ss->rc = EFS_ERR_PROTO; return 1; }
        ss->packed_dead = dead; ss->packed_ci = ci; ss->has_packed = 1;
    } else if (dead.generation != 0 && !ss->nogc) {
        ss->rc = gc_queue(ss->it, &ss->n, ss->cap, ss->keys, ss->gc_vals,
                          ino, lane, ci, &dead);
        if (ss->rc != EFS_OK)
            return 1;
    }
    if (!ss->nogc) {
        uint32_t di, nd = trailer_n(val, vlen);

        for (di = 0; di < nd; di++) {
            struct efs_meta_delta dd;
            struct efs_meta_chunk span;

            ss->rc = trailer_at(val, vlen, di, &dd);
            if (ss->rc != EFS_OK)
                return 1;
            if (dd.generation == 0)
                continue;
            memset(&span, 0, sizeof(span));
            span.generation = dd.generation;
            memcpy(span.nodes, dd.nodes, sizeof(span.nodes));
            memcpy(span.checksums, dd.checksums, sizeof(span.checksums));
            ss->rc = gc_queue(ss->it, &ss->n, ss->cap, ss->keys, ss->gc_vals,
                              ino, lane, ci, &span);
            if (ss->rc != EFS_OK)
                return 1;
        }
    }
    ss->chunks++;
    return 0;
}

int efs_meta_apply_lane_sweep(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane)
{
    struct efs_kv_item it[SWEEP_CHUNKS * 2];
    uint8_t keys[SWEEP_CHUNKS * 2][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_CHUNKS * 2][EFS_META_GC_VAL];
    uint8_t pref[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX];
    uint32_t plen = 0, kl = 0;
    struct sweep_scan ss;
    uint32_t lsh;
    int rc;

    if (!kv || ino == 0 || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    lsh = efs_kv_lane_shard(ino, lane);
    rc = efs_kv_key_chunk(lsh, ino, gen, lane, 0, pref, &plen);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, ino, gen, lane, k_ln, &kl);
    if (rc != EFS_OK)
        return rc;
    plen = 20;
    for (;;) {
        memset(&ss, 0, sizeof(ss));
        ss.ino = ino;
        ss.gen = gen;
        ss.it = it;
        ss.keys = keys;
        ss.gc_vals = gc_vals;
        ss.cap = (uint32_t)(sizeof(it) / sizeof(it[0]));
        rc = efs_kv_scan_prefix(kv, pref, plen, sweep_cb, &ss);
        if (ss.rc != EFS_OK)
            return ss.rc;
        /* rc > 0 is sweep_cb's "batch full, stop" (merge_scan propagates
         * the callback's return), not an error. Treating it as one made
         * every lane with more than SWEEP_CHUNKS chunks unsweepable: the
         * reaper re-proposed LANE_SWEEP once a second forever and no
         * fragment of any file over ~8 MiB per lane was ever reclaimed
         * (IO-500 ior-easy 1.2 GiB files: `apply lane-sweep rc=1` every
         * 33 entries). */
        if (rc < 0)
            return rc;
        if (ss.chunks == 0)
            break;
        rc = sweep_commit(kv, &ss);
        if (rc != EFS_OK)
            return rc;
        if (!ss.full)
            break;
    }
    /* The lane key itself is the last thing to go. A replay finds no
     * chunks and re-deletes an absent lane key, which is a no-op. Only the
     * reaper sweeps lanes, after the last name of the inode is gone. */
    rc = meta_cleanup_del(kv, k_ln, kl);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    return rc;
}

struct sweep_selection {
    uint32_t *indices, n;
};

static int sweep_select_cb(void *user, const uint8_t *key, uint32_t klen,
                            const uint8_t *val, uint32_t vlen)
{
    struct sweep_selection *s = user;
    (void)val; (void)vlen;
    if (klen != 24) return EFS_ERR_PROTO;
    s->indices[s->n++] = efs_load_be32(key + 20);
    return s->n == EFS_META_SWEEP_BATCH_MAX ? 1 : 0;
}

int efs_meta_lane_sweep_select(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint8_t lane, uint32_t *indices, uint32_t *n)
{
    uint8_t pref[EFS_KV_KEY_MAX]; uint32_t plen;
    struct sweep_selection s = {.indices = indices};
    if (!kv || !ino || lane >= EFS_META_LANES || !indices || !n)
        return EFS_ERR_INVAL;
    *n = 0;
    int rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane,
                              0, pref, &plen);
    if (rc != EFS_OK) return rc;
    rc = efs_kv_scan_prefix(kv, pref, 20, sweep_select_cb, &s);
    if (rc < 0) return rc;
    *n = s.n;
    return EFS_OK;
}

/* Unlike a scan-and-delete step, the immutable command names exactly the
 * keys it may remove. A storage/apply-marker retry therefore cannot advance
 * into another batch. All source deletes and GC references commit together. */
static int lane_sweep_batch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint8_t lane, const uint32_t *indices,
                            uint32_t n, int nogc)
{
    struct efs_kv_item it[SWEEP_CHUNKS * 2];
    uint8_t keys[SWEEP_CHUNKS * 2][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_CHUNKS * 2][EFS_META_GC_VAL];
    struct sweep_scan ss = {.ino=ino, .gen=gen, .it=it, .keys=keys,
        .gc_vals=gc_vals, .cap=SWEEP_CHUNKS * 2, .nogc=nogc};
    uint8_t key[EFS_KV_KEY_MAX]; uint32_t kl;
    if (!kv || !ino || lane >= EFS_META_LANES || n > EFS_META_SWEEP_BATCH_MAX ||
        (n && !indices)) return EFS_ERR_INVAL;
    for (uint32_t i=0; i<n; i++) {
        if (indices[i] % EFS_META_LANES != lane || (i && indices[i] <= indices[i-1]))
            return EFS_ERR_INVAL;
    }
    for (uint32_t i=0; i<n; i++) {
        /* Same capacity as the canonical chunk/history decoder. */
        uint8_t value[CHUNK_VAL_MAX]; uint32_t vl=sizeof(value);
        int rc = efs_kv_key_chunk(efs_kv_lane_shard(ino,lane), ino, gen, lane,
                                  indices[i], key, &kl);
        if (rc != EFS_OK) return rc;
        rc = efs_kv_get(kv,key,kl,value,&vl);
        if (rc == EFS_ERR_NOT_FOUND) continue;
        if (rc != EFS_OK) return rc;
        (void)sweep_cb(&ss,key,kl,value,vl);
        if (ss.rc != EFS_OK) return ss.rc;
        if (ss.full) return EFS_ERR_NOMEM; /* no partial writes */
    }
    if (ss.n) {
        int rc=sweep_commit(kv, &ss);
        if (rc != EFS_OK) return rc;
    }
    uint32_t remaining[EFS_META_SWEEP_BATCH_MAX], count;
    int rc=efs_meta_lane_sweep_select(kv,ino,gen,lane,remaining,&count);
    if (rc != EFS_OK) return rc;
    if (count) return EFS_ERR_BUSY;
    rc=efs_kv_key_lane(efs_kv_lane_shard(ino,lane),ino,gen,lane,key,&kl);
    if (rc != EFS_OK) return rc;
    rc=meta_cleanup_del(kv,key,kl);
    return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
}

int efs_meta_apply_lane_sweep_batch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                    uint8_t lane, const uint32_t *indices,
                                    uint32_t n)
{
    return lane_sweep_batch(kv, ino, gen, lane, indices, n, 0);
}
/* W124: sweep a reaped inode's lane without queuing per-chunk GC records.
 * Every member's reclaim fence deletes the whole inode directory, including
 * fragments of losing or never-published PUTs, so the records would only add
 * per-fragment delete RPCs and acks for files already being removed. */
int efs_meta_apply_lane_sweep_reaped(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                     uint8_t lane, const uint32_t *indices,
                                     uint32_t n)
{
    return lane_sweep_batch(kv, ino, gen, lane, indices, n, 1);
}

struct rsv_purge {
    struct efs_kv_item *it;
    uint8_t (*keys)[EFS_KV_KEY_MAX];
    uint32_t n;
    uint32_t cap;
    uint32_t found;
    int full;
};

static int rsv_purge_cb(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    struct rsv_purge *rp = user;

    (void)val;
    (void)vlen;
    if (rp->n >= rp->cap) {
        rp->full = 1;
        return 1;
    }
    memcpy(rp->keys[rp->n], key, klen);
    rp->it[rp->n].op = EFS_KV_DEL;
    rp->it[rp->n].key = rp->keys[rp->n];
    rp->it[rp->n].klen = klen;
    rp->it[rp->n].val = NULL;
    rp->it[rp->n].vlen = 0;
    rp->n++;
    rp->found++;
    return 0;
}

struct reap_selection {
    struct efs_meta_reap_key *keys;
    uint32_t n;
};

static int reap_select_cb(void *user, const uint8_t *key, uint32_t kl,
                           const uint8_t *value, uint32_t vl)
{
    struct reap_selection *s = user;
    (void)value; (void)vl;
    if (kl > EFS_META_REAP_KEY_MAX) return EFS_ERR_PROTO;
    memset(&s->keys[s->n], 0, sizeof(s->keys[s->n]));
    s->keys[s->n].len = (uint8_t)kl;
    memcpy(s->keys[s->n++].key, key, kl);
    return s->n == EFS_META_REAP_BATCH_MAX ? 1 : 0;
}

int efs_meta_reap_select(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          struct efs_meta_reap_key *keys, uint32_t *n)
{
    uint8_t prefix[EFS_KV_KEY_MAX]; uint32_t plen;
    struct reap_selection s = {.keys = keys};
    if (!kv || !ino || !keys || !n) return EFS_ERR_INVAL;
    *n = 0;
    int rc = efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino), ino, gen,
                                         prefix, &plen);
    if (rc == EFS_OK) rc = efs_kv_scan_prefix(kv,prefix,plen,reap_select_cb,&s);
    if (rc < 0) return rc;
    if (s.n < EFS_META_REAP_BATCH_MAX) {
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino),ino,prefix,&plen);
        if (rc != EFS_OK) return rc;
        prefix[2] = EFS_KV_KIND_APPEND_RECEIPT;
        rc = efs_kv_scan_prefix(kv,prefix,plen,reap_select_cb,&s);
        if (rc < 0) return rc;
    }
    *n = s.n;
    return EFS_OK;
}

int efs_meta_apply_reap_batch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               const struct efs_meta_reap_key *keys, uint32_t n)
{
    struct efs_kv_item items[EFS_META_REAP_BATCH_MAX];
    uint8_t prefix[EFS_KV_KEY_MAX]; uint32_t plen;
    struct efs_meta_row row;
    if (!kv || !ino || n > EFS_META_REAP_BATCH_MAX || (n && !keys))
        return EFS_ERR_INVAL;
    int rc = efs_kv_key_inode(efs_kv_inode_shard(ino),ino,prefix,&plen);
    if (rc != EFS_OK) return rc;
    /* Reject the complete malformed set before touching any key. */
    for (uint32_t i=0; i<n; i++) {
        const uint8_t *k = keys[i].key;
        if ((keys[i].len != 27 || k[2] != EFS_KV_KIND_APPEND_RSV || rd64(k+11) != gen) &&
            (keys[i].len != 31 || k[2] != EFS_KV_KIND_APPEND_RECEIPT))
            return EFS_ERR_INVAL;
        if (memcmp(k,prefix,2) || memcmp(k+3,prefix+3,8)) return EFS_ERR_INVAL;
        for (uint32_t j=0; j<i; j++)
            if (keys[j].len == keys[i].len && !memcmp(keys[j].key,k,keys[i].len))
                return EFS_ERR_INVAL;
        memset(&items[i],0,sizeof(items[i]));
        items[i].op=EFS_KV_DEL;items[i].key=k;items[i].klen=keys[i].len;
    }
    rc = efs_meta_apply_get_inode(kv,ino,&row);
    if (rc == EFS_OK) {
        if (row.generation != gen || row.nlink)
            return efs_meta_apply_reap_done(kv,ino,gen); /* stale marker only */
        int held = efs_lease_any(kv,ino,gen);
        if (held < 0) return held;
        if (held) return EFS_ERR_BUSY;
    } else if (rc != EFS_ERR_NOT_FOUND) return rc;
    if (n && (rc=meta_write_batch(kv,items,n)) != EFS_OK) return rc;
    struct efs_meta_reap_key remaining[EFS_META_REAP_BATCH_MAX]; uint32_t count;
    rc=efs_meta_reap_select(kv,ino,gen,remaining,&count);
    if (rc != EFS_OK) return rc;
    if (count) return EFS_ERR_BUSY;
    /* No append rows remain: historical finalization performs only empty
     * prefix scans and fixed cursor/marker/inode cleanup. */
    return efs_meta_apply_reap_done(kv,ino,gen);
}

int efs_meta_apply_reap_done(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_row row;
    struct efs_kv_item it[6];
    struct efs_expiry_change expiry_change;
    uint8_t k_reap[EFS_KV_KEY_MAX], k_cur[EFS_KV_KEY_MAX];
    uint8_t k_ino[EFS_KV_KEY_MAX], k_xa[EFS_KV_KEY_MAX];
    uint32_t kr = 0, kc = 0, ki = 0, kxa = 0, n = 0;
    uint32_t ish;
    int rc, held, drop_row = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    ish = efs_kv_inode_shard(ino);
    /* Defensive: the marker and the row delete commit in one batch, so a
     * live row here should be impossible. If one shows up anyway (a row
     * re-created under the same ino is not a thing that exists), the
     * marker is the stale part — drop it and keep the row. */
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_OK) {
        if (row.generation != gen || row.nlink != 0) {
            rc = efs_kv_key_reap(efs_kv_anchor_shard(ish), ino, k_reap, &kr);
            if (rc != EFS_OK)
                return rc;
            rc = meta_write_del(kv, k_reap, kr);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
            return rc;
        }
        held = efs_lease_any(kv, ino, row.generation);
        if (held < 0)
            return held;
        if (!held)
            drop_row = 1;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    /* Leftover append reservations of a killed appender. Bounded batches;
     * each batch's deletes make the next scan progress. */
    for (;;) {
        struct efs_kv_item rit[32];
        uint8_t rkeys[32][EFS_KV_KEY_MAX];
        uint8_t pref[EFS_KV_KEY_MAX];
        uint32_t plen = 0;
        struct rsv_purge rp;

        rc = efs_kv_key_append_rsv_prefix(ish, ino, gen, pref, &plen);
        if (rc != EFS_OK)
            return rc;
        memset(&rp, 0, sizeof(rp));
        rp.it = rit;
        rp.keys = rkeys;
        rp.cap = 32;
        rc = efs_kv_scan_prefix(kv, pref, plen, rsv_purge_cb, &rp);
        if (rc < 0) /* rc > 0 = callback's batch-full stop, not an error */
            return rc;
        if (rp.found == 0)
            break;
        rc = meta_write_batch(kv, rit, rp.n);
        if (rc != EFS_OK)
            return rc;
        if (!rp.full)
            break;
    }
    if (drop_row || efs_meta_apply_get_inode(kv, ino, &row) == EFS_ERR_NOT_FOUND) {
        for (;;) {
            struct efs_kv_item rit[32];
            uint8_t keys[32][EFS_KV_KEY_MAX], prefix[EFS_KV_KEY_MAX];
            uint32_t plen = 0;
            struct rsv_purge rp = {.it = rit, .keys = keys, .cap = 32};
            rc = efs_kv_key_inode(ish, ino, prefix, &plen);
            if (rc != EFS_OK) return rc;
            prefix[2] = EFS_KV_KIND_APPEND_RECEIPT;
            rc = efs_kv_scan_prefix(kv, prefix, plen, rsv_purge_cb, &rp);
            if (rc < 0) return rc;
            if (!rp.found) break;
            rc = meta_write_batch(kv, rit, rp.n);
            if (rc != EFS_OK) return rc;
            if (!rp.full) break;
        }
    }
    rc = efs_kv_key_append_cur(ish, ino, gen, k_cur, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_reap(efs_kv_anchor_shard(ish), ino, k_reap, &kr);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_DEL;
    it[n].key = k_cur;
    it[n].klen = kc;
    n++;
    it[n].op = EFS_KV_DEL;
    it[n].key = k_reap;
    it[n].klen = kr;
    n++;
    if (drop_row) {
        rc = efs_kv_key_inode(ish, ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        int xa = 0;

        it[n].op = EFS_KV_DEL;
        it[n].key = k_ino;
        it[n].klen = ki;
        n++;
        rc = xattr_del_item(kv, ino, k_xa, &kxa, &it[n], &xa);
        if (rc != EFS_OK)
            return rc;
        n += (uint32_t)xa;
    }
    if(drop_row || efs_meta_apply_get_inode(kv,ino,&row)==EFS_ERR_NOT_FOUND) {
        struct efs_expiry_record expiry;
        rc=efs_expiry_get(kv,ino,gen,&expiry);
        if(rc!=EFS_OK)return rc;
        if(expiry.revision) {
            expiry.deadline_ns=0;
            rc=efs_expiry_plan(kv,ino,gen,expiry.revision,&expiry,&expiry_change);
            if(rc!=EFS_OK)return rc;
            expiry_change.items[0].op=EFS_KV_DEL;
            expiry_change.items[0].val=NULL;expiry_change.items[0].vlen=0;
            if(n+expiry_change.count>sizeof(it)/sizeof(it[0]))return EFS_ERR_OVERFLOW;
            for(uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
        }
    }
    /* The row is absent or has nlink 0 and no lease (checked above). */
    return meta_cleanup_batch(kv, it, n);
}

struct gc_ack_scratch {
    struct efs_kv_item batch[EFS_META_GC_ACK_MAX];
    uint8_t keys[EFS_META_GC_ACK_MAX][EFS_KV_KEY_MAX];
    uint8_t vals[EFS_META_GC_ACK_MAX][EFS_META_GC_VAL];
    uint8_t retired[EFS_META_GC_ACK_MAX];
};

static int gc_ack_apply(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                        uint32_t n, struct gc_ack_scratch *sc);

int efs_meta_apply_gc_ack(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                          uint32_t n)
{
    struct gc_ack_scratch *sc;
    int rc;

    if (!kv || (!it && n))
        return EFS_ERR_INVAL;
    if (n > EFS_META_GC_ACK_MAX)
        return EFS_ERR_INVAL;
    if (n == 0)
        return EFS_OK;
    /* ~60 KB: off the apply thread's stack. */
    sc = malloc(sizeof(*sc));
    if (!sc)
        return EFS_ERR_NOMEM;
    rc = gc_ack_apply(kv, it, n, sc);
    free(sc);
    return rc;
}

static int gc_ack_apply(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                        uint32_t n, struct gc_ack_scratch *sc)
{
    struct efs_kv_item *batch = sc->batch;
    uint8_t (*keys)[EFS_KV_KEY_MAX] = sc->keys;
    uint8_t (*vals)[EFS_META_GC_VAL] = sc->vals;
    uint8_t *retired = sc->retired;
    uint32_t i, j, bn = 0;
    int rc;

    memset(retired, 0, EFS_META_GC_ACK_MAX);
    for (i = 0; i < n; i++) {
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        uint8_t ack_bits;
        uint32_t kg = 0, vn;
        uint8_t key[EFS_KV_KEY_MAX];
        uint8_t old[EFS_META_GC_VAL];

        if (it[i].lane >= EFS_META_LANES || it[i].frag >= EFS_NUM_FRAGMENTS)
            return EFS_ERR_INVAL;
        rc = efs_kv_key_gc(efs_kv_anchor_shard(
                               efs_kv_lane_shard(it[i].ino, it[i].lane)),
                           it[i].ino, it[i].gen, it[i].lane, it[i].ci,
                           key, &kg);
        if (rc != EFS_OK)
            return rc;
        /* One batch usually carries every fragment of the same record;
         * fold into the pending entry — a fresh KV read cannot see the
         * bits the earlier items of this batch have not applied yet. */
        for (j = 0; j < bn; j++) {
            if (batch[j].klen == kg && memcmp(batch[j].key, key, kg) == 0)
                break;
        }
        if (j < bn) {
            if (retired[j])
                continue;
            rc = efs_meta_unpack_gc(vals[j], EFS_META_GC_VAL, nodes,
                                    &ack_bits, sums);
            if (rc != EFS_OK)
                return rc;
            ack_bits |= (uint8_t)(1u << it[i].frag);
            if (ack_bits == (uint8_t)((1u << EFS_NUM_FRAGMENTS) - 1u)) {
                batch[j].op = EFS_KV_DEL;
                batch[j].val = NULL;
                batch[j].vlen = 0;
                retired[j] = 1;
            } else {
                efs_meta_pack_gc(vals[j], nodes, ack_bits,
                                 (const uint8_t (*)[EFS_HASH_SIZE])sums);
            }
            continue;
        }
        vn = sizeof(old);
        rc = efs_kv_get(kv, key, kg, old, &vn);
        /* Already retired (a replay of this entry): skip. */
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_unpack_gc(old, vn, nodes, &ack_bits, sums);
        if (rc != EFS_OK)
            return rc;
        ack_bits |= (uint8_t)(1u << it[i].frag);
        memcpy(keys[bn], key, kg);
        batch[bn].key = keys[bn];
        batch[bn].klen = kg;
        if (ack_bits == (uint8_t)((1u << EFS_NUM_FRAGMENTS) - 1u)) {
            batch[bn].op = EFS_KV_DEL;
            batch[bn].val = NULL;
            batch[bn].vlen = 0;
            retired[bn] = 1;
        } else {
            efs_meta_pack_gc(vals[bn], nodes, ack_bits,
                             (const uint8_t (*)[EFS_HASH_SIZE])sums);
            batch[bn].op = EFS_KV_PUT;
            batch[bn].val = vals[bn];
            batch[bn].vlen = EFS_META_GC_VAL;
        }
        bn++;
    }
    if (bn == 0)
        return EFS_OK;
    rc = meta_write_batch(kv, batch, bn);
    if (rc != EFS_OK)
        return rc;
    /* A retired record lowers its anchor's pending-GC watermark (D26).
     * The anchor is the key's shard field; replays skipped above never
     * reach here, so each retirement lowers exactly once. */
    for (i = 0; i < bn; i++) {
        if (retired[i])
            efs_meta_gc_pending_note(
                ((uint32_t)batch[i].key[0] << 8) | batch[i].key[1], -1);
    }
    return EFS_OK;
}

/* Reads a lane's sequence number. Absent is seq 0, which is a real value:
 * a lane with no record has nothing to contribute and nothing to change. */
static int lane_seq_get(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                        uint8_t lane, uint64_t *seq)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;

    *seq = 0;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    vn = sizeof(val);
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
    if (rc != EFS_OK)
        return rc;
    *seq = ln.seq;
    return EFS_OK;
}

int efs_meta_apply_getattr(struct efs_kv *kv, efs_ino_t ino,
                           efs_txn_coord_fn coord, void *ctx,
                           struct efs_meta_stat *out)
{
    uint64_t seq1[EFS_META_LANES], seq2[EFS_META_LANES];
    uint8_t lanes[EFS_META_LANES];
    uint32_t tries;

    if (!kv || !coord || !out || ino == 0)
        return EFS_ERR_INVAL;
    for (tries = 1; tries <= EFS_META_STAT_TRIES; tries++) {
        struct efs_meta_row row, again;
        struct efs_txn_pending pend;
        uint64_t size, mtime, ctime, bits;
        uint64_t alloc = 0;
        uint32_t i, nl = 0;
        int moved = 0, stable = 1, rc, is_dir;

        memset(&pend, 0, sizeof(pend));
        rc = inode_read_tx(kv, ino, coord, ctx, &pend, &row);
        if (rc != EFS_OK)
            return rc;
        is_dir = S_ISDIR(row.mode) ? 1 : 0;
        size = row.base_size;
        mtime = row.base_mtime;
        ctime = row.base_ctime;
        /* Files reduce over write lanes; a spread directory reduces over
         * dir lanes (used_shards). A LOCAL directory's times already live
         * on the row, so it is a one-read collect. */
        bits = is_dir
                   ? (row.layout != EFS_META_LAYOUT_LOCAL ? row.used_shards : 0)
                   : row.active_lanes;
        for (i = 0; i < EFS_META_LANES; i++)
            if (bits & (1ULL << i))
                lanes[nl++] = (uint8_t)i;

        for (i = 0; i < nl; i++) {
            uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
            uint32_t kl = 0, vn;
            struct efs_txn_reduce red;
            struct lane_rec ln;
            int value_rc;

            rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lanes[i]), ino,
                                 row.generation, lanes[i], key, &kl);
            if (rc != EFS_OK)
                return rc;
            /* Committed-but-not-yet-materialized reductions have to count:
             * a transaction is visible at its decision, not when a reducer
             * gets around to folding it in, so a stat that ignored them
             * could report a size older than a write that already
             * returned. */
            /* Reuse only the reduction's first visible lane image. The
             * second collect below must independently read its sequence. */
            vn = sizeof(val);
            rc = efs_txn_reduce_read_value_ex(kv, key, kl, coord, ctx, &red,
                                               &pend, val, &vn, &value_rc);
            if (rc != EFS_OK)
                return rc;
            if (value_rc == EFS_OK) {
                rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
                if (rc != EFS_OK)
                    return rc;
            } else if (value_rc == EFS_ERR_NOT_FOUND) {
                memset(&ln, 0, sizeof(ln));
            } else {
                return value_rc;
            }
            seq1[i] = ln.seq;
            if (!is_dir) {
                /* D25: a lane's EOF counts only under the current epoch;
                 * a lane that has not adopted it holds a superseded claim. */
                if (ln.fenced_epoch == row.content_epoch)
                    size = max_u64(size, red.max_end);
                /* D17: present-chunk counts live only on materialized
                 * lanes (a pending reduction never makes a chunk
                 * present), so the reduce is a plain sum. */
                alloc += ln.present;
            }
            ctime = max_u64(ctime, red.max_ctime);
            /* A lane mtime stamped before the current utimens generation was
             * invalidated by it, and taking a MAX against it would let the
             * old value win — which is the whole reason utimens has a
             * generation and the other time sources do not.
             *
             * The fence zeros materialized max_mtime and bumps the lane's
             * generation. A committed-but-unmaterialized reduction from
             * before the fence still sits under the lane prefix with the
             * old mtime; folding it in would undo the utimens. After the
             * fence the lane is (gen>0, max_mtime==0); a later write puts
             * a real stamp back and pending reductions count again. */
            if (ln.mtime_gen >= row.mtime_gen) {
                if (!(ln.max_mtime == 0 && ln.mtime_gen > 0))
                    mtime = max_u64(mtime, red.max_mtime);
            }
        }

        /* Second collect: the sequence numbers only. */
        for (i = 0; i < nl && stable; i++) {
            uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
            uint32_t kl = 0, vn = sizeof(val);
            struct lane_rec ln;
            rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lanes[i]), ino,
                                   row.generation, lanes[i], key, &kl);
            if (rc == EFS_OK)
                rc = efs_txn_read_ex(kv, key, kl, coord, ctx, val, &vn, &pend);
            if (rc == EFS_ERR_NOT_FOUND) {
                seq2[i] = 0;
                rc = EFS_OK;
            } else if (rc == EFS_OK) {
                rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
                if (rc == EFS_OK)
                    seq2[i] = ln.seq;
            }
            if (rc != EFS_OK)
                return rc;
            if (seq2[i] != seq1[i])
                stable = 0;
        }
        rc = inode_read_tx(kv, ino, coord, ctx, &pend, &again);
        if (rc != EFS_OK)
            return rc;
        rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved)
            stable = 0;
        if (again.content_epoch != row.content_epoch ||
            again.base_size != row.base_size ||
            again.mtime_gen != row.mtime_gen ||
            again.active_lanes != row.active_lanes ||
            again.used_shards != row.used_shards ||
            again.generation != row.generation)
            stable = 0;
        if (!stable)
            continue;

        if (!is_dir) {
            struct append_cur cur;

            rc = load_append_cur(kv, ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            if (cur.nopen)
                size = cur.frontier;
        }

        memset(out, 0, sizeof(*out));
        out->ino = row.ino;
        out->generation = row.generation;
        out->mode = row.mode;
        out->nlink = row.nlink;
        out->uid = row.uid;
        out->gid = row.gid;
        out->size = size;
        out->mtime = mtime;
        out->ctime = ctime;
        out->alloc = alloc;
        out->atime = row.base_atime; /* noatime: reads do not move it */
        out->lanes = nl;
        out->attempts = tries;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

/* Push a per-lane update across a bitmap: the epoch fence and utimens'
 * mtime_gen. A bit with no lane record yet is
 * skipped — the first write to that lane will observe the row and stamp
 * itself. `upd` returns 0 to leave the record untouched (replay). */
static int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino, uint64_t generation,
                           uint64_t bits,
                           int (*upd)(struct lane_rec *ln, void *arg),
                           void *arg, struct efs_kv_item *it, uint32_t *n,
                           uint8_t k_ln[][EFS_KV_KEY_MAX],
                           uint8_t v_ln[][LANE_VAL])
{
    uint32_t i;
    int rc;

    for (i = 0; i < EFS_META_LANES; i++) {
        uint8_t old_ln[LANE_VAL];
        uint32_t kl = 0, vn;
        struct lane_rec ln;

        if ((bits & (1ULL << i)) == 0)
            continue;
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, (uint8_t)i), ino, generation,
                             (uint8_t)i, k_ln[i], &kl);
        if (rc != EFS_OK)
            return rc;
        vn = sizeof(old_ln);
        rc = efs_kv_get(kv, k_ln[i], kl, old_ln, &vn);
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
        rc = unpack_lane(old_ln, vn, &ln);
        if (rc != EFS_OK)
            return rc;
        if (!upd(&ln, arg))
            continue;
        pack_lane(v_ln[i], &ln);
        it[*n].op = EFS_KV_PUT;
        it[*n].key = k_ln[i];
        it[*n].klen = kl;
        it[*n].val = v_ln[i];
        it[*n].vlen = LANE_VAL;
        (*n)++;
    }
    return EFS_OK;
}

static int epoch_lane_upd(struct lane_rec *ln, void *arg)
{
    ln->fenced_epoch = *(const uint64_t *)arg;
    /* Invalidate the size claim: after a truncate the authoritative size is
     * base_size on the inode row, and a lane still reporting the pre-truncate
     * high-water mark would win the MAX and undo the truncate. Times are not
     * invalidated — a truncate is a modification, so they only move forward. */
    ln->max_end = 0;
    ln->seq++; /* a stat collect in flight must not validate across this */
    return 1;
}

static int utimens_lane_upd(struct lane_rec *ln, void *arg)
{
    uint64_t gen = *(const uint64_t *)arg;

    if (ln->mtime_gen >= gen)
        return 0;
    ln->mtime_gen = gen;
    /* Zero, not the utimens value: getattr takes base_mtime, and a later
     * write MAX(0, now) must not resurrect the pre-utimens stamp. */
    ln->max_mtime = 0;
    ln->seq++;
    return 1;
}

int efs_meta_apply_epoch_fence(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1];
    uint32_t ki = 0, n = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_pub_pinned(kv, ino, row.generation);
    if (rc != EFS_OK)
        return rc;
    row.content_epoch++;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    /* Every active lane, not just lane 0: a lane the fence skipped keeps
     * believing its own epoch and its own size. */
    rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                         epoch_lane_upd, &row.content_epoch, it, &n, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_utimens(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_utimens *u)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1];
    uint32_t ki = 0, n = 0;
    uint64_t bits;
    int rc;

    if (!kv || !u || ino == 0)
        return EFS_ERR_INVAL;
    if (u->mask == 0 ||
        (u->mask & ~(EFS_META_SET_MTIME | EFS_META_SET_ATIME)) != 0)
        return EFS_ERR_INVAL;
    if ((u->mask & EFS_META_SET_MTIME) && u->mtime_gen == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND && u->lane_bits && u->expect_gen &&
        (u->mask & EFS_META_SET_MTIME)) {
        memset(it, 0, sizeof(it));
        rc = fence_lane_bits(kv, ino, u->expect_gen, u->lane_bits,
                             utimens_lane_upd, (void *)&u->mtime_gen, it, &n,
                             k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
        if (n == 0)
            return EFS_OK;
        return meta_write_batch(kv, it, n);
    }
    if (rc != EFS_OK)
        return rc;
    if (u->expect_gen != 0 && u->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if ((u->mask & EFS_META_SET_MTIME) && u->mtime_gen < row.mtime_gen)
        return EFS_ERR_STALE;
    if (u->mask & EFS_META_SET_ATIME)
        row.base_atime = u->atime;
    if (u->mask & EFS_META_SET_MTIME)
        row.base_mtime = u->mtime;
    /* ctime always moves (POSIX), MAX-clamped like every implicit stamp. */
    row.base_ctime = max_u64(row.base_ctime, now);
    if (u->mask & EFS_META_SET_MTIME)
        row.mtime_gen = u->mtime_gen;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (u->mask & EFS_META_SET_MTIME) {
        bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
                   ? row.used_shards
                   : row.active_lanes;
        rc = fence_lane_bits(kv, ino, row.generation, bits, utimens_lane_upd,
                             &row.mtime_gen, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
    }
    return meta_write_batch(kv, it, n);
}

static int append_bar_set(struct lane_rec *ln, void *arg)
{
    uint64_t w = *(const uint64_t *)arg;

    if (ln->append_bar == w)
        return 0;
    ln->append_bar = w;
    return 1;
}

static int append_bar_clr(struct lane_rec *ln, void *arg)
{
    (void)arg;
    if (ln->append_bar == 0)
        return 0;
    ln->append_bar = 0;
    return 1;
}

static int collect_phys_eof(struct efs_kv *kv, const struct efs_meta_row *row,
                            efs_txn_coord_fn coord, void *ctx, uint64_t *eof,
                            uint64_t seqs[EFS_META_LANES])
{
    uint64_t size = row->base_size;
    uint32_t i;
    int rc;

    memset(seqs, 0, sizeof(uint64_t) * EFS_META_LANES);
    for (i = 0; i < EFS_META_LANES; i++) {
        uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
        uint32_t kl = 0, vn;
        struct efs_txn_reduce red;
        struct efs_txn_pending pend;
        struct lane_rec ln;

        if ((row->active_lanes & (1ULL << i)) == 0)
            continue;
        rc = efs_kv_key_lane(efs_kv_lane_shard(row->ino, (uint8_t)i), row->ino,
                             row->generation, (uint8_t)i, key, &kl);
        if (rc != EFS_OK)
            return rc;
        memset(&pend, 0, sizeof(pend));
        memset(&red, 0, sizeof(red));
        rc = efs_txn_reduce_read_ex(kv, key, kl, coord, ctx, &red, &pend);
        if (rc != EFS_OK)
            return rc;
        vn = sizeof(val);
        rc = efs_kv_get(kv, key, kl, val, &vn);
        if (rc == EFS_OK) {
            rc = unpack_lane(val, vn, &ln);
            if (rc != EFS_OK)
                return rc;
        } else if (rc == EFS_ERR_NOT_FOUND) {
            memset(&ln, 0, sizeof(ln));
        } else {
            return rc;
        }
        seqs[i] = ln.seq;
        if (ln.fenced_epoch == row->content_epoch) /* D25 adoption */
            size = max_u64(size, max_u64(red.max_end, ln.max_end));
    }
    *eof = size;
    return EFS_OK;
}

/* Reservation records accumulate while any reservation stays open (a killed
 * appender strands its OPEN records, and DONE records behind them are kept
 * until nopen reaches 0), so the scan grows: the old fixed 64-cap NOMEM'd
 * every resolve/report for the inode, which failed the client's whole
 * report batch and left the rec dirty — poisoning every later sync report
 * (the direct-io/symlink EIO window in the posix gate). */
struct rsv_scan {
    struct append_rsv *r;
    uint32_t n, cap;
    int rc;
};

static int rsv_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct rsv_scan *s = user;

    (void)key;
    (void)klen;
    if (s->n >= s->cap) {
        uint32_t ncap = s->cap ? s->cap * 2 : 64;
        struct append_rsv *nr = realloc(s->r, (size_t)ncap * sizeof(*nr));

        if (!nr) {
            s->rc = EFS_ERR_NOMEM;
            return 1;
        }
        s->r = nr;
        s->cap = ncap;
    }
    s->rc = unpack_append_rsv(val, vlen, &s->r[s->n]);
    if (s->rc != EFS_OK)
        return 1;
    s->n++;
    return 0;
}

static int load_rsvs(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                     struct rsv_scan *s)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    memset(s, 0, sizeof(*s));
    rc = efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino), ino, gen, pref,
                                      &plen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan_prefix(kv, pref, plen, rsv_scan_cb, s);
    if (s->rc != EFS_OK)
        return s->rc;
    return rc;
}

struct append_foreign_scan {const uint8_t *uuid;int foreign,rc;};
static int append_foreign_cb(void *user,const uint8_t *key,uint32_t kl,
                               const uint8_t *value,uint32_t length)
{
    struct append_foreign_scan *s=user;struct append_rsv r;
    (void)key;(void)kl;
    s->rc=unpack_append_rsv(value,length,&r);
    if(s->rc!=EFS_OK)return 1;
    if(r.state>APPEND_DONE){s->rc=EFS_ERR_PROTO;return 1;}
    if(r.state==APPEND_OPEN && memcmp(r.uuid,s->uuid,EFS_OPID_UUID_LEN)) {
        s->foreign=1;return 1;
    }
    return 0;
}
/* 0 = ok, 1 = other uuid has OPEN, <0 = error. No burst-sized allocation. */
static int append_foreign_open(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                                 const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    uint8_t prefix[EFS_KV_KEY_MAX];uint32_t pl;
    struct append_foreign_scan scan={.uuid=uuid};
    int rc=efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino),ino,gen,prefix,&pl);
    if(rc==EFS_OK)rc=efs_kv_scan_prefix(kv,prefix,pl,append_foreign_cb,&scan);
    if(scan.rc!=EFS_OK)return scan.rc;
    if(rc!=EFS_OK && rc!=1)return rc;
    return scan.foreign;
}

/* Unlike directory windows, append's result must bind the requested length
 * and must never turn a retired success into an invented offset zero. Keep
 * this window on the inode's group, in a separate identity namespace. */
static int append_receipt_load(struct efs_kv *kv, efs_ino_t ino,
                               const struct efs_opid_req *q, uint8_t *key,
                               uint32_t *kl, uint8_t *value, uint32_t *vl)
{
    int rc;
    if (!kv || !ino || !q || !efs_opid_req_valid(q) ||
        !q->id.session_epoch || q->ack >= q->id.seq)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, kl);
    if (rc != EFS_OK) return rc;
    key[2] = EFS_KV_KIND_APPEND_RECEIPT;
    memcpy(key + *kl, q->id.client_uuid, EFS_OPID_UUID_LEN);
    *kl += EFS_OPID_UUID_LEN;
    efs_store_be32(key + *kl, q->id.session_epoch); *kl += 4;
    rc = efs_kv_get(kv, key, *kl, value, vl);
    if (rc == EFS_ERR_NOT_FOUND) { *vl = 0; return EFS_OK; }
    return rc;
}

static int append_receipt_result(const uint8_t *value, uint32_t vl,
                                  uint64_t len, const struct efs_opid_req *q,
                                  uint64_t *off)
{
    struct efs_opid_reply rep;
    int rc = efs_opid_probe(value, vl, &q->id, &rep);
    if (rc <= 0) return rc < 0 ? rc : EFS_ERR_NOT_FOUND;
    if (!rep.ino) return EFS_ERR_STALE; /* retired, exact offset unavailable */
    if (rep.ino != len) return EFS_ERR_INVAL;
    if (rep.rc == EFS_OK) *off = rep.extra;
    return rep.rc;
}

int efs_meta_append_result(struct efs_kv *kv, efs_ino_t ino, uint64_t len,
                           const struct efs_opid_req *q, uint64_t *off)
{
    uint8_t key[EFS_KV_KEY_MAX], value[EFS_OPID_VAL_MAX];
    uint32_t kl = 0, vl = sizeof(value);
    if (!len || !off) return EFS_ERR_INVAL;
    int rc = append_receipt_load(kv, ino, q, key, &kl, value, &vl);
    return rc == EFS_OK ? append_receipt_result(value, vl, len, q, off) : rc;
}

/* D25 truncate receipts: the append window shape under its own kind. */
static int truncate_receipt_load(struct efs_kv *kv, efs_ino_t ino,
                                 const struct efs_opid_req *q, uint8_t *key,
                                 uint32_t *kl, uint8_t *value, uint32_t *vl)
{
    int rc;
    if (!kv || !ino || !q || !efs_opid_req_valid(q) ||
        !q->id.session_epoch || q->ack >= q->id.seq)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, kl);
    if (rc != EFS_OK) return rc;
    key[2] = EFS_KV_KIND_TRUNCATE_RECEIPT;
    memcpy(key + *kl, q->id.client_uuid, EFS_OPID_UUID_LEN);
    *kl += EFS_OPID_UUID_LEN;
    efs_store_be32(key + *kl, q->id.session_epoch); *kl += 4;
    rc = efs_kv_get(kv, key, *kl, value, vl);
    if (rc == EFS_ERR_NOT_FOUND) { *vl = 0; return EFS_OK; }
    return rc;
}

static int trunc_pending_key(efs_ino_t ino, uint8_t *key, uint32_t *kl)
{
    uint32_t shard = efs_kv_anchor_shard(efs_kv_inode_shard(ino));
    key[0] = (uint8_t)(shard >> 8);
    key[1] = (uint8_t)shard;
    key[2] = EFS_KV_KIND_TRUNC_PENDING;
    be64(key + 3, ino);
    *kl = 11;
    return EFS_OK;
}

int efs_meta_apply_truncate_lazy(struct efs_kv *kv,
                                 const struct efs_meta_truncate_lazy *t,
                                 uint64_t *epoch_after)
{
    struct efs_meta_row row;
    struct append_cur cur;
    struct efs_fence_history history;
    struct efs_opid_reply rep;
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL];
    uint8_t k_cur[EFS_KV_KEY_MAX], k_h[EFS_KV_KEY_MAX], v_h[EFS_META_FENCE_BYTES];
    uint8_t k_rc[EFS_KV_KEY_MAX], v_rc[EFS_OPID_VAL_MAX], v_new[EFS_OPID_VAL_MAX];
    uint8_t k_p[EFS_KV_KEY_MAX], v_p[8];
    uint32_t ki = 0, kc = 0, kh = 0, vh = 0, krc = 0, vrc = sizeof(v_rc);
    uint32_t vnew = sizeof(v_new), kp = 0, n = 0;
    struct efs_kv_item it[5];
    int shrink, rc;

    if (!kv || !t || !t->ino || !t->inode_gen || !epoch_after ||
        t->kind > EFS_META_TRUNCATE_SHRINK)
        return EFS_ERR_INVAL;
    shrink = t->kind == EFS_META_TRUNCATE_SHRINK;
    rc = efs_meta_apply_get_inode(kv, t->ino, &row);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_STALE;
    if (rc != EFS_OK)
        return rc;
    if (row.generation != t->inode_gen)
        return EFS_ERR_STALE;
    /* W77: a shrink moves the epoch staged publications were checked at. */
    if (shrink && (rc = efs_meta_pub_pinned(kv, t->ino, row.generation)) != EFS_OK)
        return rc;
    if (!S_ISREG(row.mode))
        return EFS_ERR_INVAL;
    /* Receipt first: a committed truncate retried under its identity
     * answers its result even though its preconditions no longer hold. */
    rc = truncate_receipt_load(kv, t->ino, &t->q, k_rc, &krc, v_rc, &vrc);
    if (rc != EFS_OK)
        return rc;
    rc = efs_opid_probe(v_rc, vrc, &t->q.id, &rep);
    if (rc < 0)
        return rc;
    if (rc == 1) {
        if (!rep.ino)
            return EFS_ERR_STALE; /* acknowledged and reclaimed: no result */
        if (rep.rc == EFS_OK)
            *epoch_after = rep.extra;
        return rep.rc;
    }
    if (row.content_epoch != t->expect_epoch ||
        row.base_size != t->expect_base_size)
        return EFS_ERR_STALE;
    rc = load_append_cur(kv, t->ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (cur.nopen)
        return EFS_ERR_BUSY;
    if (!shrink && t->size < row.base_size)
        return EFS_ERR_INVAL;
    if (shrink) {
        if (row.content_epoch == UINT64_MAX)
            return EFS_ERR_INVAL;
        rc = fence_history_read(kv, t->ino, row.generation, EFS_META_FENCE_INODE,
                                NULL, NULL, NULL, &history);
        if (rc != EFS_OK)
            return rc;
        if (history.revision == UINT64_MAX)
            return EFS_ERR_INVAL;
        rc = efs_fence_history_append_pruned(&history, row.content_epoch + 1,
                                             t->size);
        if (rc != EFS_OK)
            return rc;
        history.revision++;
        row.content_epoch++;
    }
    /* Receipt capacity before any effect: never apply unrecorded. */
    memset(&rep, 0, sizeof(rep));
    rep.rc = EFS_OK;
    rep.ino = t->ino;
    rep.extra = row.content_epoch;
    rc = efs_opid_fold(v_rc, vrc, t->q.id.client_uuid, t->q.id.session_epoch,
                       &t->q, &rep, v_new, &vnew);
    if (rc == EFS_ERR_NOMEM)
        return EFS_ERR_BUSY;
    if (rc != EFS_OK)
        return rc;
    row.base_size = t->size;
    row.base_mtime = max_u64(row.base_mtime, t->mtime_ns);
    row.base_ctime = max_u64(row.base_ctime, t->mtime_ns);
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(t->ino), t->ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_append_cur(efs_kv_inode_shard(t->ino), t->ino,
                                   row.generation, k_cur, &kc);
    if (rc != EFS_OK)
        return rc;
    it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_ino, ki, v_ino, INO_VAL};
    /* Closed cursor and new EOF change together (as the eager path). */
    it[n++] = (struct efs_kv_item){EFS_KV_DEL, k_cur, kc, NULL, 0};
    it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_rc, krc, v_new, vnew};
    if (shrink) {
        rc = content_fence_key(t->ino, row.generation, EFS_META_FENCE_INODE,
                               k_h, &kh);
        if (rc == EFS_OK)
            rc = efs_meta_pack_fence_history(&history, v_h, sizeof(v_h), &vh);
        if (rc == EFS_OK)
            rc = trunc_pending_key(t->ino, k_p, &kp);
        if (rc != EFS_OK)
            return rc;
        be64(v_p, row.generation);
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_h, kh, v_h, vh};
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_p, kp, v_p, sizeof(v_p)};
    }
    rc = meta_write_batch(kv, it, n);
    if (rc == EFS_OK)
        *epoch_after = row.content_epoch;
    return rc;
}

static int append_reserve_inner(struct efs_kv *kv, efs_ino_t ino, uint64_t len,
                                  const struct efs_opid *op,
                                  const struct efs_opid_req *durable,
                                  efs_txn_coord_fn coord, void *ctx,
                                  uint64_t *off_out)

{
    struct efs_meta_row row;
    struct efs_opid_reply rep;
    struct append_cur cur;
    struct append_rsv rsv;
    uint64_t seqs[EFS_META_LANES], seq2, phys, eof;
    uint8_t k_cur[EFS_KV_KEY_MAX], k_rsv[EFS_KV_KEY_MAX], k_opid[EFS_KV_KEY_MAX];
    uint8_t v_cur[APPEND_CUR_VAL], v_rsv[APPEND_RSV_VAL];
    uint8_t v_opid[EFS_OPID_VAL_MAX];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 4];
    uint32_t kc = 0, kr = 0, n = 0, i;
    uint8_t receipt_cur[EFS_OPID_VAL_MAX];
    uint32_t receipt_kl = 0, receipt_vl = sizeof(receipt_cur);
    int rc, hit;

    if (!kv || !op || !coord || !off_out || ino == 0 || len == 0)
        return EFS_ERR_INVAL;
    /* Receipt retirement never restores permission for a revoked identity.
     * Every append names an established session (K5). */
    rc=efs_session_accept(kv,efs_kv_inode_shard(ino),op->client_uuid,
                                 op->session_epoch);
    if(rc!=EFS_OK)return rc;
    /* APPEND's window (sim; production APPEND carries seq 0) lives on the
     * session shard, not a dentry shard. */
    if (durable) {
        rc = efs_meta_append_result(kv, ino, len, durable, off_out);
        if (rc != EFS_ERR_NOT_FOUND) return rc;
    }
    hit = durable ? 0 : efs_meta_apply_opid_probe(kv,
                    efs_kv_session_shard(op->client_uuid), op, &rep);
    if (hit < 0)
        return hit;
    if (hit) {
        *off_out = rep.extra;
        return rep.rc;
    }
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISREG(row.mode))
        return EFS_ERR_INVAL;
    rc = collect_phys_eof(kv, &row, coord, ctx, &phys, seqs);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < EFS_META_LANES; i++) {
        if ((row.active_lanes & (1ULL << i)) == 0)
            continue;
        rc = lane_seq_get(kv, ino, row.generation, (uint8_t)i, &seq2);
        if (rc != EFS_OK)
            return rc;
        if (seq2 != seqs[i])
            return EFS_ERR_BUSY;
    }
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (cur.nopen) {
        rc = append_foreign_open(kv, ino, row.generation, op->client_uuid);
        if (rc > 0)
            return EFS_ERR_BUSY;
        if (rc < 0)
            return rc;
    }
    eof = max_u64(phys, cur.watermark);
    if (len > UINT64_MAX - eof || cur.nopen == UINT32_MAX)
        return EFS_ERR_INVAL;
    if (cur.nopen == 0)
        cur.frontier = eof;
    cur.watermark = eof + len;
    cur.nopen++;
    memset(&rsv, 0, sizeof(rsv));
    rsv.off = eof;
    rsv.len = len;
    memcpy(rsv.uuid, op->client_uuid, EFS_OPID_UUID_LEN);
    rsv.epoch = op->session_epoch;
    rsv.seq = op->seq;
    rsv.state = APPEND_OPEN;
    pack_append_cur(v_cur, &cur);
    pack_append_rsv(v_rsv, &rsv);
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, row.generation,
                               k_cur, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino, row.generation,
                                   eof, k_rsv, &kr);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_cur;
    it[n].klen = kc;
    it[n].val = v_cur;
    it[n].vlen = APPEND_CUR_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_rsv;
    it[n].klen = kr;
    it[n].val = v_rsv;
    it[n].vlen = APPEND_RSV_VAL;
    n++;
    rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                         append_bar_set, &cur.watermark, it, &n, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    if (durable) {
        uint32_t packed = sizeof(v_opid);
        rc = append_receipt_load(kv, ino, durable, k_opid, &receipt_kl,
                                 receipt_cur, &receipt_vl);
        if (rc != EFS_OK) return rc;
        /* A completion may have materialized while EOF was collected. Never
         * fold a different offset over an already completed/retired ticket. */
        rc = append_receipt_result(receipt_cur, receipt_vl, len, durable, off_out);
        if (rc != EFS_ERR_NOT_FOUND) return rc;
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK; rep.ino = len; rep.extra = eof;
        rc = efs_opid_fold(receipt_cur, receipt_vl, op->client_uuid,
                           op->session_epoch, durable, &rep, v_opid, &packed);
        if (rc == EFS_ERR_NOMEM) return EFS_ERR_BUSY;
        if (rc != EFS_OK) return rc;
        it[n].op = EFS_KV_PUT; it[n].key = k_opid;
        it[n].klen = receipt_kl; it[n].val = v_opid; it[n].vlen = packed; n++;
    } else if (opid_hosted(op)) {
        struct efs_opid_req q;

        memset(&q, 0, sizeof(q));
        q.id = *op;
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = ino;
        rep.extra = eof;
        rc = opid_item(kv, efs_kv_session_shard(op->client_uuid), &q, &rep,
                       k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    rc = meta_write_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    *off_out = eof;
    return EFS_OK;
}

int efs_meta_apply_append_reserve(struct efs_kv *kv, efs_ino_t ino, uint64_t len,
                                  const struct efs_opid *op,
                                  efs_txn_coord_fn coord, void *ctx,
                                  uint64_t *off_out)
{
    return append_reserve_inner(kv, ino, len, op, NULL, coord, ctx, off_out);
}

int efs_meta_apply_append_reserve_durable(struct efs_kv *kv, efs_ino_t ino,
                                         uint64_t len,
                                         const struct efs_opid_req *q,
                                         efs_txn_coord_fn coord, void *ctx,
                                         uint64_t *off_out)
{
    if (!q) return EFS_ERR_INVAL;
    return append_reserve_inner(kv, ino, len, &q->id, q, coord, ctx, off_out);
}

/* Select an offset once from complete authoritative views. Prepared exact
 * images close every extension/resize race before the decision; followers
 * install these images instead of recomputing EOF from their hosted subset. */
int efs_meta_capture_append(struct efs_kv *kv, efs_ino_t ino, uint64_t len,
                             const struct efs_opid_req *q,
                             efs_txn_coord_fn coord, void *ctx,
                             struct efs_meta_append_plan *out)
{
    struct efs_meta_row row;
    struct append_cur cur;
    struct append_rsv rsv = {0};
    struct efs_txn_pending pending = {0};
    struct efs_meta_append_image *image;
    struct efs_opid_reply reply = {0};
    uint64_t phys;
    int rc, moved = 0;
    if (!kv || !ino || !len || !q || !coord || !out)
        return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    rc=efs_session_accept(kv,efs_kv_inode_shard(ino),q->id.client_uuid,
                                 q->id.session_epoch);
    if(rc!=EFS_OK)return rc;
    rc = efs_meta_append_result(kv, ino, len, q, &out->offset);
    if (rc != EFS_ERR_NOT_FOUND) return rc;
    rc = inode_read_tx(kv, ino, coord, ctx, &pending, &row);
    if (rc != EFS_OK) return rc;
    if (!S_ISREG(row.mode)) return EFS_ERR_INVAL;
    image = &out->images[out->count++];
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, image->key, &image->klen);
    if (rc != EFS_OK) return rc;
    image->op = EFS_TXN_PUT;
    image->expected_len = image->new_len = INO_VAL;
    pack_inode(image->expected, &row); pack_inode(image->value, &row);
    rc = resize_parts_add(&out->parts, efs_kv_inode_shard(ino));
    if (rc != EFS_OK) return rc;
    /* Cursor, receipt and reservation have fixed image positions. Lane
     * capture below establishes EOF before those replacements are built. */
    out->count = 4;
    phys = row.base_size;
    for (uint8_t lane = 0; lane < EFS_META_LANES; ++lane) {
        struct lane_rec stamp;
        struct efs_txn_reduce red = {0};
        uint32_t n;
        if (!(row.active_lanes & (1ull << lane))) continue;
        image = &out->images[out->count++];
        rc = resize_lane_read(kv, ino, row.generation, lane, coord, ctx,
                              &pending, image->expected, &n, &stamp);
        if (rc != EFS_OK) return rc;
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, row.generation,
                             lane, image->key, &image->klen);
        if (rc == EFS_OK)
            rc = efs_txn_reduce_read_ex(kv, image->key, image->klen, coord, ctx,
                                        &red, &pending);
        if (rc != EFS_OK) return rc;
        image->op = EFS_TXN_PUT;
        image->expected_len = n ? n : EFS_TXN_ABSENT;
        image->new_len = LANE_VAL;
        if (!n) stamp.fenced_epoch = row.content_epoch;
        /* D25: a lane behind the inode epoch adopts it in this image (its
         * old EOF claim is superseded); a lane ahead of it cannot occur
         * without a newer inode epoch, so it is a race: retry. */
        if (stamp.fenced_epoch > row.content_epoch) return EFS_ERR_BUSY;
        (void)lane_adopt(&stamp, row.content_epoch);
        /* red.max_end is the materialized lane's own claim (reductions carry
         * no EOF); after adoption only the stamp's current-epoch EOF counts. */
        phys = max_u64(phys, stamp.max_end);
        pack_lane(image->value, &stamp);
        rc = resize_parts_add(&out->parts, efs_kv_lane_shard(ino, lane));
        if (rc != EFS_OK) return rc;
    }
    image = &out->images[1];
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, row.generation,
                               image->key, &image->klen);
    if (rc != EFS_OK) return rc;
    uint32_t n = sizeof(image->expected);
    /* Cursor and reservation scans use materialized state together. Reading
     * a committed-but-unresolved cursor overlaid on an old reservation scan
     * could miss its foreign owner. The inode/cursor PREPARE checks reject
     * outstanding intents or any subsequent resolution/mutation. */
    rc = efs_kv_get(kv, image->key, image->klen, image->expected, &n);
    memset(&cur, 0, sizeof(cur));
    if (rc == EFS_ERR_NOT_FOUND) image->expected_len = EFS_TXN_ABSENT;
    else if (rc == EFS_OK) {
        image->expected_len = n;
        rc = unpack_append_cur(image->expected, n, &cur);
    }
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    if (cur.nopen) {
        rc = append_foreign_open(kv, ino, row.generation, q->id.client_uuid);
        if (rc) return rc > 0 ? EFS_ERR_BUSY : rc;
    }
    if (!cur.nopen) {
        /* A bounded completed page below inode base_size can survive a new
         * burst. Shrink/reset ranges beyond that floor still force cleanup. */
        rc = efs_meta_append_prune_due(kv, ino);
        if (rc) return rc > 0 ? EFS_ERR_BUSY : rc;
    }
    out->offset = max_u64(phys, cur.watermark);
    if (len > UINT64_MAX - out->offset || cur.nopen == UINT32_MAX)
        return EFS_ERR_INVAL;
    if (!cur.nopen) cur.frontier = out->offset;
    cur.watermark = out->offset + len; ++cur.nopen;
    image->op = EFS_TXN_PUT; image->new_len = APPEND_CUR_VAL;
    pack_append_cur(image->value, &cur);
    image = &out->images[2]; n = sizeof(image->expected);
    rc = append_receipt_load(kv, ino, q, image->key, &image->klen,
                             image->expected, &n);
    if (rc != EFS_OK) return rc;
    /* The first receipt probe precedes authoritative EOF collection. Another
     * dual-host handler can complete this ticket in between. Recheck the exact
     * receipt image guarded by PREPARE; if complete, discard the new plan and
     * return its canonical result. A later race is rejected by that image CAS. */
    uint64_t completed = 0;
    rc = append_receipt_result(image->expected, n, len, q, &completed);
    if (rc == EFS_OK) {
        memset(out, 0, sizeof(*out)); out->offset = completed;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND) return rc;
    image->expected_len = n ? n : EFS_TXN_ABSENT;
    reply.rc = EFS_OK; reply.ino = len; reply.extra = out->offset;
    image->new_len = sizeof(image->value); image->op = EFS_TXN_PUT;
    rc = efs_opid_fold(image->expected, n, q->id.client_uuid, q->id.session_epoch,
                       q, &reply, image->value, &image->new_len);
    if (rc != EFS_OK) return rc == EFS_ERR_NOMEM ? EFS_ERR_BUSY : rc;
    image = &out->images[3];
    rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino, row.generation,
                               out->offset, image->key, &image->klen);
    if (rc != EFS_OK) return rc;
    n = sizeof(image->expected);
    rc = efs_kv_get(kv, image->key, image->klen, image->expected, &n);
    if (rc != EFS_ERR_NOT_FOUND) return rc == EFS_OK ? EFS_ERR_BUSY : rc;
    image->expected_len = EFS_TXN_ABSENT; image->new_len = APPEND_RSV_VAL;
    image->op = EFS_TXN_PUT;
    rsv.off = out->offset; rsv.len = len;
    memcpy(rsv.uuid, q->id.client_uuid, EFS_OPID_UUID_LEN);
    rsv.epoch = q->id.session_epoch; rsv.seq = q->id.seq; rsv.state = APPEND_OPEN;
    pack_append_rsv(image->value, &rsv);
    for (uint32_t i = 4; i < out->count; ++i) {
        struct lane_rec stamp;
        image = &out->images[i];
        rc = unpack_lane(image->value, image->new_len, &stamp);
        if (rc != EFS_OK) return rc;
        stamp.append_bar = cur.watermark; pack_lane(image->value, &stamp);
    }
    rc = efs_txn_pending_recheck(&pending, coord, ctx, &moved);
    return rc != EFS_OK ? rc : moved ? EFS_ERR_BUSY : EFS_OK;
}

int efs_meta_execute_append(const struct efs_meta_append_plan *plan,
                             const struct efs_txid *t,
                             const struct efs_meta_append_ops *ops, void *ctx)
{
    struct efs_txn_parts expected = {0};
    if (!plan || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve)
        return EFS_ERR_INVAL;
    if (!plan->count) return EFS_OK; /* exact replay: no new reservation */
    if (plan->count < 4 || plan->count > EFS_META_LANES + 4 ||
        plan->images[0].klen != 11 || plan->images[0].key[2] != EFS_KV_KIND_INODE ||
        plan->images[0].expected_len != INO_VAL || plan->images[0].new_len != INO_VAL ||
        memcmp(plan->images[0].expected, plan->images[0].value, INO_VAL))
        return EFS_ERR_INVAL;
    struct efs_meta_row row;
    struct append_cur cursor, previous = {0};
    struct append_rsv reservation;
    uint64_t bits = 0;
    const struct efs_meta_append_image *c = &plan->images[1], *r = &plan->images[3];
    if (unpack_inode(plan->images[0].value, INO_VAL, &row) != EFS_OK ||
        !S_ISREG(row.mode) || c->new_len != APPEND_CUR_VAL ||
        r->new_len != APPEND_RSV_VAL ||
        unpack_append_cur(c->value, c->new_len, &cursor) != EFS_OK ||
        unpack_append_rsv(r->value, r->new_len, &reservation) != EFS_OK ||
        (c->expected_len != EFS_TXN_ABSENT &&
         unpack_append_cur(c->expected, c->expected_len, &previous) != EFS_OK) ||
        r->expected_len != EFS_TXN_ABSENT || reservation.state != APPEND_OPEN ||
        !reservation.len || reservation.off != plan->offset ||
        reservation.len > UINT64_MAX - reservation.off || previous.nopen == UINT32_MAX ||
        cursor.nopen != previous.nopen + 1 ||
        cursor.watermark != reservation.off + reservation.len ||
        cursor.frontier != (previous.nopen ? previous.frontier : reservation.off) ||
        reservation.off < max_u64(row.base_size, previous.watermark))
        return EFS_ERR_INVAL;
    for (uint32_t i = 1; i < plan->count; ++i) {
        const struct efs_meta_append_image *image = &plan->images[i];
        uint8_t key[EFS_KV_KEY_MAX]; uint32_t kl = 0;
        int rc;
        if (i == 1) rc = efs_kv_key_append_cur(efs_kv_inode_shard(row.ino), row.ino,
                                               row.generation, key, &kl);
        else if (i == 2) {
            rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, key, &kl);
            key[2] = EFS_KV_KIND_APPEND_RECEIPT;
            memcpy(key + kl, reservation.uuid, EFS_OPID_UUID_LEN); kl += EFS_OPID_UUID_LEN;
            efs_store_be32(key + kl, reservation.epoch); kl += 4;
            struct efs_opid id = {.session_epoch = reservation.epoch, .seq = reservation.seq};
            struct efs_opid_reply result;
            memcpy(id.client_uuid, reservation.uuid, EFS_OPID_UUID_LEN);
            if (image->new_len > EFS_OPID_VAL_MAX ||
                efs_opid_probe(image->value, image->new_len, &id, &result) != 1 ||
                result.rc != EFS_OK || result.ino != reservation.len || result.extra != reservation.off)
                return EFS_ERR_INVAL;
        } else if (i == 3) rc = efs_kv_key_append_rsv(efs_kv_inode_shard(row.ino), row.ino,
                                                     row.generation, reservation.off, key, &kl);
        else {
            struct lane_rec before = {0}, after;
            if (image->klen != 20 || image->key[19] >= EFS_META_LANES ||
                image->new_len != LANE_VAL ||
                (image->expected_len != EFS_TXN_ABSENT &&
                 unpack_lane(image->expected, image->expected_len, &before) != EFS_OK) ||
                unpack_lane(image->value, image->new_len, &after) != EFS_OK)
                return EFS_ERR_INVAL;
            uint8_t lane = image->key[19];
            if (bits & (1ull << lane)) return EFS_ERR_INVAL;
            bits |= 1ull << lane;
            if (image->expected_len == EFS_TXN_ABSENT) before.fenced_epoch = row.content_epoch;
            /* Capture adopts a lane behind the inode epoch (D25); the plan
             * must carry exactly that adoption. */
            if (before.fenced_epoch > row.content_epoch)
                return EFS_ERR_INVAL;
            (void)lane_adopt(&before, row.content_epoch);
            if (reservation.off < before.max_end)
                return EFS_ERR_INVAL;
            before.append_bar = cursor.watermark;
            uint8_t value[LANE_VAL]; pack_lane(value, &before);
            if (memcmp(value, image->value, LANE_VAL)) return EFS_ERR_INVAL;
            rc = efs_kv_key_lane(efs_kv_lane_shard(row.ino, lane), row.ino,
                                 row.generation, lane, key, &kl);
        }
        if (rc != EFS_OK || image->klen != kl || memcmp(image->key, key, kl))
            return EFS_ERR_INVAL;
    }
    if (bits != row.active_lanes) return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->count; ++i) {
        const struct efs_meta_append_image *image = &plan->images[i];
        if (image->klen < 11 || image->klen > EFS_KV_KEY_MAX ||
            memcmp(image->key + 3, plan->images[0].key + 3, 8) ||
            image->op != EFS_TXN_PUT || !image->new_len ||
            image->new_len > EFS_OPID_VAL_MAX ||
            (image->expected_len != EFS_TXN_ABSENT && image->expected_len > EFS_OPID_VAL_MAX))
            return EFS_ERR_INVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (image->klen == plan->images[j].klen &&
                !memcmp(image->key, plan->images[j].key, image->klen))
                return EFS_ERR_INVAL;
        int rc = resize_parts_add(&expected, ((uint32_t)image->key[0] << 8) | image->key[1]);
        if (rc != EFS_OK) return rc;
    }
    if (expected.n != plan->parts.n ||
        memcmp(expected.shard, plan->parts.shard, expected.n * sizeof(uint32_t)))
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->count; ++i) {
        int rc = ops->prepare(ctx, t, &plan->parts, &plan->images[i]);
        if (rc != EFS_OK) {
            int aborted = ops->decide(ctx, t, &plan->parts, EFS_TXN_ABORT);
            if (aborted != EFS_OK) return aborted;
            (void)ops->resolve(ctx, t, &plan->parts, EFS_TXN_ABORT);
            return rc;
        }
    }
    int rc = ops->decide(ctx, t, &plan->parts, EFS_TXN_COMMIT);
    if (rc != EFS_OK) return rc; /* never convert an ambiguous COMMIT to ABORT */
    return ops->resolve(ctx, t, &plan->parts, EFS_TXN_COMMIT);
}

/* Ordered reservations let the frontier advance in one streaming pass.
 * Stop at the first unresolved range; no full-burst allocation is needed. */
struct append_frontier_scan {uint64_t frontier,completed;int rc;};
static int append_frontier_cb(void *user,const uint8_t *key,uint32_t kl,
                                const uint8_t *value,uint32_t length)
{
    struct append_frontier_scan *s=user;struct append_rsv r;
    (void)key;(void)kl;
    s->rc=unpack_append_rsv(value,length,&r);
    if(s->rc!=EFS_OK)return 1;
    if(!r.len || r.len>UINT64_MAX-r.off || r.state>APPEND_DONE) {
        s->rc=EFS_ERR_PROTO;return 1;
    }
    if(r.off<s->frontier)return 0;
    if(r.off!=s->frontier || (r.state!=APPEND_DONE && r.off!=s->completed))return 1;
    s->frontier=r.off+r.len;return 0;
}
static int append_frontier_collect(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
                                     uint64_t completed,uint64_t *frontier)
{
    uint8_t prefix[EFS_KV_KEY_MAX],start[EFS_KV_KEY_MAX];uint32_t pl,sl;
    struct append_frontier_scan scan={.frontier=*frontier,.completed=completed};
    int rc=efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino),ino,gen,prefix,&pl);
    if(rc==EFS_OK)rc=efs_kv_key_append_rsv(efs_kv_inode_shard(ino),ino,gen,*frontier,start,&sl);
    if(rc==EFS_OK)rc=efs_kv_scan_from(kv,prefix,pl,start,sl,append_frontier_cb,&scan);
    if(scan.rc!=EFS_OK)return scan.rc;
    if(rc!=EFS_OK && rc!=1)return rc;
    *frontier=scan.frontier;return EFS_OK;
}

static int meta_append_resolve(struct efs_kv *kv, efs_ino_t ino, uint64_t off,
                                  int outcome,
                                  int (*emit)(void *,const struct efs_kv_item *,uint32_t),void *ctx)
{
    struct efs_meta_row row;
    struct append_cur cur;
    struct append_rsv rsv;
    struct rsv_scan scan = {0};
    uint8_t k_cur[EFS_KV_KEY_MAX], k_rsv[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t v_cur[APPEND_CUR_VAL], v_rsv[APPEND_RSV_VAL], v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    uint8_t (*del_keys)[EFS_KV_KEY_MAX] = NULL;
    uint32_t *del_kl = NULL;
    struct efs_kv_item *it = NULL;
    uint32_t kc = 0, kr = 0, ki = 0, n = 0, i;
    int rc, hole, progressed = 1;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (outcome != EFS_META_APPEND_COMPLETED &&
        outcome != EFS_META_APPEND_ABORTED_HOLE &&
        outcome != EFS_META_APPEND_FENCED_HOLE)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if(emit) {
        rc=efs_kv_key_inode(efs_kv_inode_shard(ino),ino,k_ino,&ki);
        if(rc==EFS_OK)rc=meta_write_check(kv,k_ino,ki);
        if(rc!=EFS_OK)return rc;
    }
    rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino, row.generation, off,
                               k_rsv, &kr);
    if (rc != EFS_OK)
        return rc;
    {
        uint8_t old[APPEND_RSV_VAL];
        uint32_t vn = sizeof(old);

        rc = efs_kv_get(kv, k_rsv, kr, old, &vn);
        if (rc == EFS_ERR_NOT_FOUND) {
            rc = load_append_cur(kv, ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            /* Drain deletes every rsv. A retry after that is a no-op;
             * a bogus offset while the burst is still live is not. */
            return cur.nopen ? EFS_ERR_NOT_FOUND : EFS_OK;
        }
        if (rc != EFS_OK)
            return rc;
        rc = unpack_append_rsv(old, vn, &rsv);
        if (rc != EFS_OK)
            return rc;
    }
    if(emit && (rsv.off!=off || !rsv.len || rsv.len>UINT64_MAX-rsv.off ||
                 rsv.state>APPEND_DONE))return EFS_ERR_PROTO;
    if (rsv.state == APPEND_DONE) {
        /* Decide-once (K15/W64). The first resolution in the log wins:
         * a retirement never overtakes a completion (no-op), and a late
         * completion of a retired range is refused, never applied. */
        if (outcome == EFS_META_APPEND_COMPLETED &&
            (rsv.outcome == EFS_META_APPEND_ABORTED_HOLE ||
             rsv.outcome == EFS_META_APPEND_FENCED_HOLE))
            return EFS_ERR_STALE;
        return EFS_OK;
    }
    hole = (outcome != EFS_META_APPEND_COMPLETED);
    rsv.state = APPEND_DONE;
    rsv.outcome = (uint8_t)outcome;
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (cur.nopen == 0)
        return EFS_ERR_PROTO;
    cur.nopen--;
    if (hole)
        row.base_size = max_u64(row.base_size, rsv.off + rsv.len);
    if(emit) {
        rc=append_frontier_collect(kv,ino,row.generation,off,&cur.frontier);
        if(rc!=EFS_OK)return rc;
    } else {
        rc = load_rsvs(kv, ino, row.generation, &scan);
        if (rc != EFS_OK) {
            free(scan.r);
            return rc;
        }
        for (i = 0; i < scan.n; i++) {
            if (scan.r[i].off == off)
                scan.r[i].state = APPEND_DONE;
        }
        while (progressed) {
            progressed = 0;
            for (i = 0; i < scan.n; i++) {
                if (scan.r[i].state == APPEND_DONE &&
                    scan.r[i].off == cur.frontier) {
                    cur.frontier = scan.r[i].off + scan.r[i].len;
                    progressed = 1;
                    scan.r[i].state = 0xff;
                }
            }
        }
    }
    it = malloc(((size_t)scan.n + EFS_META_LANES + 4) * sizeof(*it));
    if(!emit) {
        del_keys = malloc((size_t)(scan.n ? scan.n : 1) * sizeof(*del_keys));
        del_kl = malloc((size_t)(scan.n ? scan.n : 1) * sizeof(*del_kl));
    }
    if (!it || (!emit && (!del_keys || !del_kl))) {
        rc = EFS_ERR_NOMEM;
        goto out;
    }
    memset(it, 0, ((size_t)scan.n + EFS_META_LANES + 4) * sizeof(*it));
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, row.generation,
                               k_cur, &kc);
    if (rc != EFS_OK)
        goto out;
    if (cur.nopen == 0) {
        row.base_size = max_u64(row.base_size, cur.frontier);
        it[n].op = EFS_KV_DEL;
        it[n].key = k_cur;
        it[n].klen = kc;
        n++;
        if(emit) {
            pack_append_rsv(v_rsv,&rsv);
            it[n].op=EFS_KV_PUT;it[n].key=k_rsv;it[n].klen=kr;
            it[n].val=v_rsv;it[n++].vlen=APPEND_RSV_VAL;
        } else if (scan.n == 0) {
            it[n].op = EFS_KV_DEL;
            it[n].key = k_rsv;
            it[n].klen = kr;
            n++;
        }
        for (i = 0; i < scan.n; i++) {
            rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino,
                                       row.generation, scan.r[i].off,
                                       del_keys[i], &del_kl[i]);
            if (rc != EFS_OK)
                goto out;
            it[n].op = EFS_KV_DEL;
            it[n].key = del_keys[i];
            it[n].klen = del_kl[i];
            n++;
        }
        rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                             append_bar_clr, NULL, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            goto out;
    } else {
        pack_append_rsv(v_rsv, &rsv);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_rsv;
        it[n].klen = kr;
        it[n].val = v_rsv;
        it[n].vlen = APPEND_RSV_VAL;
        n++;
        pack_append_cur(v_cur, &cur);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_cur;
        it[n].klen = kc;
        it[n].val = v_cur;
        it[n].vlen = APPEND_CUR_VAL;
        n++;
    }
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        goto out;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if(emit) {
        for(uint32_t j=0;j<n;++j) {
            rc=meta_write_check(kv,it[j].key,it[j].klen);
            if(rc!=EFS_OK)goto out;
        }
        rc=emit(ctx,it,n);
    } else rc=meta_write_batch(kv,it,n);
out:
    free(scan.r);
    free(del_keys);
    free(del_kl);
    free(it);
    return rc;
}

int efs_meta_apply_append_resolve(struct efs_kv *kv,efs_ino_t ino,uint64_t off,int outcome)
{
    return meta_append_resolve(kv,ino,off,outcome,NULL,NULL);
}
int efs_meta_collect_append_completion(struct efs_kv *kv,efs_ino_t ino,uint64_t off,
                                        int outcome,
                                        int (*emit)(void *,const struct efs_kv_item *,uint32_t),void *ctx)
{
    if(!emit)return EFS_ERR_INVAL;
    return meta_append_resolve(kv,ino,off,outcome,emit,ctx);
}

struct catchup_change {uint8_t key[27], value[APPEND_RSV_VAL];};
struct catchup_scan {
    struct efs_kv *kv;
    struct catchup_change *changes;
    uint32_t count, capacity, seen, open;
    uint64_t published, frontier, watermark, generation;
    efs_ino_t ino;
    int rc;
};
static int catchup_row(void *user,const uint8_t *key,uint32_t kl,
                         const uint8_t *value,uint32_t vl)
{
    struct catchup_scan *s=user;struct append_rsv r;
    if(++s->seen>EFS_META_APPEND_CATCHUP_MAX) {
        s->rc=EFS_ERR_OVERFLOW;return 1;
    }
    if(kl!=27 || vl!=APPEND_RSV_VAL || rd64(key+3)!=s->ino ||
       rd64(key+11)!=s->generation || unpack_append_rsv(value,vl,&r)!=EFS_OK ||
       rd64(key+19)!=r.off || !r.len || r.off>UINT64_MAX-r.len ||
       r.off+r.len>s->watermark || r.state>APPEND_DONE) {
        s->rc=EFS_ERR_PROTO;return 1;
    }
    if(r.state==APPEND_OPEN) {
        ++s->open;
        if(r.off+r.len<=s->published) {
            if(s->count==s->capacity) {s->rc=EFS_ERR_PROTO;return 1;}
            s->rc=meta_write_check(s->kv,key,kl);
            if(s->rc!=EFS_OK)return 1;
            struct catchup_change *c=&s->changes[s->count++];
            memcpy(c->key,key,kl);memcpy(c->value,value,vl);
            c->value[44]=APPEND_DONE;c->value[45]=EFS_META_APPEND_COMPLETED;
            r.state=APPEND_DONE;
        }
    }
    if(r.off==s->frontier && r.state==APPEND_DONE)s->frontier=r.off+r.len;
    return 0;
}

/* Only reservation rows and their cursor belong to this inode group's log.
 * REPORT has already durably published its EOF on every touched lane group.
 * Never recompute that EOF from a replica's possibly partial lane view here.
 * Keep the closed cursor until the separate exact-image transaction retires
 * all lane barriers; a crash between phases remains safe and retryable. */
int efs_meta_apply_append_catchup(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
    uint64_t epoch,uint64_t published,uint32_t limit,const struct efs_opid *session)
{
    if(!kv || !ino || !gen || !limit || limit>EFS_META_APPEND_CATCHUP_MAX)return EFS_ERR_INVAL;
    uint32_t shard=efs_kv_inode_shard(ino),ki=0,kc=0,pl=0,sl=0;
    uint8_t inode[EFS_KV_KEY_MAX],cursor[EFS_KV_KEY_MAX],prefix[EFS_KV_KEY_MAX],start[EFS_KV_KEY_MAX];
    uint8_t value[APPEND_CUR_VAL];struct efs_meta_row row;struct append_cur cur;
    int rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc!=EFS_OK)return rc;
    if(row.generation!=gen || row.content_epoch!=epoch)return EFS_ERR_STALE;
    if(!S_ISREG(row.mode))return EFS_ERR_INVAL;
    if(session) {
        struct efs_opid_req q={.id=*session};q.id.seq=1;
        if(!efs_opid_req_valid(&q) || !session->session_epoch)return EFS_ERR_INVAL;
        rc=efs_session_accept(kv,shard,session->client_uuid,session->session_epoch);
        if(rc!=EFS_OK)return rc;
    }
    rc=efs_kv_key_inode(shard,ino,inode,&ki);
    if(rc==EFS_OK)rc=meta_write_check(kv,inode,ki);
    if(rc==EFS_OK)rc=efs_kv_key_append_cur(shard,ino,gen,cursor,&kc);
    if(rc==EFS_OK)rc=meta_write_check(kv,cursor,kc);
    if(rc==EFS_OK)rc=efs_txn_key_exclusive(kv,cursor,kc);
    if(rc==EFS_OK)rc=load_append_cur(kv,ino,gen,&cur);
    if(rc!=EFS_OK || !cur.nopen)return rc;
    if(cur.frontier>cur.watermark)return EFS_ERR_PROTO;
    if(cur.nopen>EFS_META_APPEND_CATCHUP_MAX)return EFS_ERR_OVERFLOW;
    if(cur.nopen>limit)return EFS_ERR_BUSY; /* recapture before changing any row */
    struct catchup_change *changes=calloc(cur.nopen,sizeof(*changes));
    struct efs_kv_item *items=calloc((size_t)cur.nopen+1,sizeof(*items));
    if(!changes || !items){free(changes);free(items);return EFS_ERR_NOMEM;}
    struct catchup_scan scan={.kv=kv,.changes=changes,.capacity=cur.nopen,
        .published=published,.frontier=cur.frontier,.watermark=cur.watermark,
        .generation=gen,.ino=ino};
    rc=efs_kv_key_append_rsv_prefix(shard,ino,gen,prefix,&pl);
    if(rc==EFS_OK)rc=efs_kv_key_append_rsv(shard,ino,gen,cur.frontier,start,&sl);
    if(rc==EFS_OK)rc=efs_kv_scan_from(kv,prefix,pl,start,sl,catchup_row,&scan);
    if(scan.rc!=EFS_OK)rc=scan.rc;
    if(rc==1)rc=EFS_ERR_PROTO;
    if(rc==EFS_OK && scan.open!=cur.nopen)rc=EFS_ERR_BUSY;
    if(rc==EFS_OK && scan.count) {
        cur.nopen-=scan.count;cur.frontier=scan.frontier;
        if(!cur.nopen && cur.frontier!=cur.watermark)rc=EFS_ERR_PROTO;
        else {
            for(uint32_t i=0;i<scan.count;++i)
                items[i]=(struct efs_kv_item){EFS_KV_PUT,changes[i].key,27,
                                              changes[i].value,APPEND_RSV_VAL};
            pack_append_cur(value,&cur);
            items[scan.count]=(struct efs_kv_item){EFS_KV_PUT,cursor,kc,value,sizeof(value)};
            rc=meta_write_batch(kv,items,scan.count+1);
        }
    }
    free(items);free(changes);return rc;
}

int efs_meta_collect_append_barrier_retirement(struct efs_kv *kv,efs_ino_t ino,
    int (*emit)(void *,const struct efs_kv_item *,uint32_t),void *ctx)
{
    if(!kv || !ino || !emit)return EFS_ERR_INVAL;
    struct efs_meta_row row;struct append_cur cur;
    uint8_t kc[EFS_KV_KEY_MAX],ki[EFS_KV_KEY_MAX],vi[INO_VAL];uint32_t nc=0,ni=0,n=0;
    uint8_t kl[EFS_META_LANES][EFS_KV_KEY_MAX],vl[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item items[EFS_META_LANES+2];
    int rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc==EFS_OK)rc=load_append_cur(kv,ino,row.generation,&cur);
    if(rc!=EFS_OK)return rc;
    if(cur.nopen)return EFS_ERR_BUSY;
    if(!cur.watermark)return EFS_OK; /* already retired / ordinary REPORT */
    if(cur.frontier!=cur.watermark)return EFS_ERR_PROTO;
    rc=efs_kv_key_append_cur(efs_kv_inode_shard(ino),ino,row.generation,kc,&nc);
    if(rc==EFS_OK)rc=efs_kv_key_inode(efs_kv_inode_shard(ino),ino,ki,&ni);
    if(rc==EFS_OK)rc=fence_lane_bits(kv,ino,row.generation,row.active_lanes,
                                    append_bar_clr,NULL,items,&n,kl,vl);
    if(rc!=EFS_OK)return rc;
    row.base_size=max_u64(row.base_size,cur.frontier);pack_inode(vi,&row);
    items[n++]=(struct efs_kv_item){EFS_KV_PUT,ki,ni,vi,sizeof(vi)};
    items[n++]=(struct efs_kv_item){EFS_KV_DEL,kc,nc,NULL,0};
    return emit(ctx,items,n);
}

int efs_meta_apply_append_state(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t *watermark, uint64_t *frontier,
                                uint32_t *nopen)
{
    struct efs_meta_row row;
    struct append_cur cur;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (watermark)
        *watermark = cur.watermark;
    if (frontier)
        *frontier = cur.frontier;
    if (nopen)
        *nopen = cur.nopen;
    return EFS_OK;
}

struct append_open_page {
    uint64_t *offs,*lens;
    uint32_t n,limit;
    int rc;
};
static int append_open_cb(void *user,const uint8_t *key,uint32_t kl,
                            const uint8_t *value,uint32_t length)
{
    struct append_open_page *p=user;struct append_rsv r;
    (void)key;(void)kl;
    p->rc=unpack_append_rsv(value,length,&r);
    if(p->rc!=EFS_OK)return 1;
    if(!r.len || r.len>UINT64_MAX-r.off || r.state>APPEND_DONE) {
        p->rc=EFS_ERR_PROTO;return 1;
    }
    if(r.state!=APPEND_OPEN)return 0;
    if(p->offs && p->lens){p->offs[p->n]=r.off;p->lens[p->n]=r.len;}
    return ++p->n==p->limit;
}
int efs_meta_apply_append_open(struct efs_kv *kv,efs_ino_t ino,uint64_t *offs,
                                uint64_t *lens,uint32_t *n)
{
    struct efs_meta_row row;struct append_cur cur;
    uint8_t prefix[EFS_KV_KEY_MAX],start[EFS_KV_KEY_MAX];uint32_t pl,sl;
    if(!kv || !n || !ino)return EFS_ERR_INVAL;
    uint32_t cap=*n;*n=0;
    int rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc==EFS_OK)rc=load_append_cur(kv,ino,row.generation,&cur);
    if(rc!=EFS_OK)return rc;
    if(!cur.nopen)return EFS_OK;
    if(!cap){*n=cur.nopen;return EFS_OK;}
    struct append_open_page page={.offs=offs,.lens=lens,
        .limit=cap<cur.nopen?cap:cur.nopen};
    rc=efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino),ino,row.generation,prefix,&pl);
    if(rc==EFS_OK)rc=efs_kv_key_append_rsv(efs_kv_inode_shard(ino),ino,row.generation,cur.frontier,start,&sl);
    if(rc==EFS_OK)rc=efs_kv_scan_from(kv,prefix,pl,start,sl,append_open_cb,&page);
    if(page.rc!=EFS_OK)return page.rc;
    if(rc!=EFS_OK && rc!=1)return rc;
    /* A concurrent cursor change invalidates this page; never report an
     * empty page as proof of completion while the captured cursor is OPEN. */
    if(page.n!=page.limit)return EFS_ERR_BUSY;
    *n=page.n;return EFS_OK;
}

struct session_append_scan {
    uint32_t shard,epoch;
    const uint8_t *uuid;
    efs_ino_t ino;
    uint64_t generation,offset;
    unsigned found;
    int rc;
};
static int session_append_row(void *user,const uint8_t *key,uint32_t kl,
                               const uint8_t *val,uint32_t vl)
{
    struct session_append_scan *s=user;struct append_rsv r;
    if(kl!=27 || vl!=APPEND_RSV_VAL ||
       unpack_append_rsv(val,vl,&r)!=EFS_OK || !r.len ||
       r.off>UINT64_MAX-r.len || r.state>APPEND_DONE ||
       r.off!=rd64(key+19) || !rd64(key+3) || !rd64(key+11) ||
       efs_kv_inode_shard(rd64(key+3))!=s->shard) {
        s->rc=EFS_ERR_PROTO;return 1;
    }
    if(r.state!=APPEND_OPEN || r.epoch!=s->epoch || memcmp(r.uuid,s->uuid,16))return 0;
    s->ino=rd64(key+3);s->generation=rd64(key+11);s->offset=r.off;s->found=1;
    return 1;
}
int efs_meta_append_session_next(struct efs_kv *kv,uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,
    efs_ino_t *ino,uint64_t *generation,uint64_t *offset,unsigned *found)
{
    if(!ino || !generation || !offset || !found)return EFS_ERR_INVAL;
    *found=0;
    int rc=efs_session_cleanup_floor(kv,shard,uuid,epoch);
    if(rc!=EFS_OK)return rc;
    struct session_append_scan scan={.shard=shard,.epoch=epoch,.uuid=uuid};
    uint8_t prefix[EFS_KV_KEY_MAX];uint32_t n;
    rc=efs_kv_key_append_rsv_shard_prefix(shard,prefix,&n);
    if(rc==EFS_OK)rc=efs_kv_scan_prefix(kv,prefix,n,session_append_row,&scan);
    if(rc!=EFS_OK && rc!=1)return rc;
    if(scan.rc!=EFS_OK)return scan.rc;
    if(scan.found) {*ino=scan.ino;*generation=scan.generation;*offset=scan.offset;}
    *found=scan.found;return EFS_OK;
}

#define DROP_RSV_MAX 32

struct drop_rsv_acc {
    const uint8_t *uuid;
    uint32_t epoch;
    efs_ino_t ino[DROP_RSV_MAX];
    uint64_t off[DROP_RSV_MAX];
    int n;
    int full;
};

static int drop_rsv_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct drop_rsv_acc *a = user;
    struct append_rsv rsv;
    int rc;

    if (klen < 27 || !val)
        return 0;
    rc = unpack_append_rsv(val, vlen, &rsv);
    if (rc != EFS_OK)
        return 0;
    if (rsv.state != APPEND_OPEN)
        return 0;
    if (rsv.epoch != a->epoch)
        return 0;
    if (memcmp(rsv.uuid, a->uuid, EFS_OPID_UUID_LEN) != 0)
        return 0;
    if (a->n >= DROP_RSV_MAX) {
        a->full = 1;
        return 1;
    }
    a->ino[a->n] = rd64(key + 3);
    a->off[a->n] = rd64(key + 19);
    a->n++;
    return 0;
}

int efs_meta_apply_append_drop_session(struct efs_kv *kv, uint32_t shard,
                                       const uint8_t uuid[EFS_OPID_UUID_LEN],
                                       uint32_t epoch)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    if (!kv || !uuid || shard > EFS_KV_SHARD_MASK)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_append_rsv_shard_prefix(shard, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    for (;;) {
        struct drop_rsv_acc acc;
        int i;

        memset(&acc, 0, sizeof(acc));
        acc.uuid = uuid;
        acc.epoch = epoch;
        rc = efs_kv_scan_prefix(kv, pref, plen, drop_rsv_cb, &acc);
        if (rc != EFS_OK && rc != 1)
            return rc;
        if (acc.n == 0)
            return EFS_OK;
        for (i = 0; i < acc.n; i++) {
            rc = efs_meta_apply_append_resolve(kv, acc.ino[i], acc.off[i],
                                               EFS_META_APPEND_FENCED_HOLE);
            if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
                return rc;
        }
        if (!acc.full)
            return EFS_OK;
    }
}

/* Last-close edge: a reservation still OPEN when the inode's final lease
 * closes can never complete (the writer that reserved it is gone — killed
 * mid-append), so resolve them all as holes and let the append frontier and
 * base_size settle (§7.3 ABORTED_HOLE). Without this a killed appender
 * strands OPEN records forever: the frontier wedges and the records
 * accumulate without bound. Published data is unaffected either way — the
 * chunk CAS already landed; this is frontier bookkeeping. */
int efs_meta_apply_append_drain_file(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    struct rsv_scan scan;
    uint32_t i;
    int rc, rc2;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = load_rsvs(kv, ino, row.generation, &scan);
    if (rc != EFS_OK) {
        free(scan.r);
        return rc;
    }
    rc = EFS_OK;
    for (i = 0; i < scan.n; i++) {
        if (scan.r[i].state != APPEND_OPEN)
            continue;
        rc2 = efs_meta_apply_append_resolve(kv, ino, scan.r[i].off,
                                            EFS_META_APPEND_ABORTED_HOLE);
        if (rc2 != EFS_OK && rc2 != EFS_ERR_NOT_FOUND && rc == EFS_OK)
            rc = rc2;
    }
    free(scan.r);
    return rc;
}

int efs_meta_apply_chunk_holds(struct efs_kv *kv, efs_ino_t ino,
                               uint64_t inode_gen, uint32_t chunk_index,
                               uint64_t gen, uint32_t span_off,
                               uint32_t span_len, uint64_t *committed)
{
    struct efs_meta_chunk ch;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen, nd, i;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (committed)
        *committed = 0;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, inode_gen, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, &ch));
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(&ch, ino, inode_gen, chunk_index));
    if (rc != EFS_OK) return rc;
    if (committed)
        *committed = ch.generation;
    if (gen == 0)
        return 0;
    if (span_len == 0)
        return ch.generation == gen ? 1 : 0;
    nd = trailer_n(val, vlen);
    for (i = 0; i < nd; i++) {
        struct efs_meta_delta d;

        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, i, &d));
        if (rc != EFS_OK)
            return rc;
        if (d.generation != gen)
            continue;
        if (d.len == 0 || (d.off == span_off && d.len == span_len))
            return 1;
    }
    return 0;
}

int efs_meta_apply_get_chunk_deltas(struct efs_kv *kv, efs_ino_t ino,
                                    uint32_t chunk_index,
                                    struct efs_meta_delta *out, uint32_t cap,
                                    uint32_t *n, uint64_t *newest)
{
    struct efs_meta_row row;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen, nd, i, wr;
    int rc;

    if (!kv || !n || ino == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (newest)
        *newest = 0;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, row.generation, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    struct efs_meta_chunk base;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, &base));
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(&base, ino, row.generation, chunk_index));
    if (rc != EFS_OK) return rc;
    nd = trailer_n(val, vlen);
    wr = nd < cap ? nd : cap;
    for (i = 0; i < wr; i++) {
        if (!out)
            break;
        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, i, &out[i]));
        if (rc != EFS_OK)
            return rc;
    }
    *n = nd;
    if (newest && nd) {
        struct efs_meta_delta last;

        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, nd - 1, &last));
        if (rc != EFS_OK)
            return rc;
        *newest = last.seq;
    }
    return EFS_OK;
}

int efs_meta_apply_get_chunk(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                             struct efs_meta_chunk *out)
{
    struct efs_meta_row row;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen;
    int rc;

    if (!kv || !out || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, row.generation, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, out));
    return rc == EFS_OK ? efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(out, ino, row.generation, chunk_index)) : rc;
}

/* One KV get for the image and the span trailer, keyed by a row the
 * caller already holds. GETCHUNKS did get_chunk + get_chunk_deltas per
 * chunk = four point gets (two of them the inode row) for one value;
 * a 64-chunk window under a hard-write was 68 ms (Sep 30). */
int efs_meta_apply_get_chunk_row(struct efs_kv *kv, efs_ino_t ino,
                                 uint64_t inode_gen, uint32_t chunk_index,
                                 struct efs_meta_chunk *out,
                                 struct efs_meta_delta *ds, uint32_t cap,
                                 uint32_t *n, uint64_t *newest)
{
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen, nd, i, wr;
    int rc;

    if (!kv || !out || !n || ino == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (newest)
        *newest = 0;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, inode_gen, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, out));
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(out, ino, inode_gen, chunk_index));
    if (rc != EFS_OK)
        return rc;
    nd = trailer_n(val, vlen);
    wr = nd < cap ? nd : cap;
    for (i = 0; i < wr && ds; i++) {
        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, i, &ds[i]));
        if (rc != EFS_OK)
            return rc;
    }
    *n = nd;
    if (newest && nd) {
        struct efs_meta_delta last;

        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, nd - 1, &last));
        if (rc != EFS_OK)
            return rc;
        *newest = last.seq;
    }
    return EFS_OK;
}

int efs_meta_get_chunk_view_tx(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint32_t ci, uint32_t cs,
                            efs_txn_coord_fn coord, void *ctx,
                            struct efs_meta_chunk_view *out)
{
    uint8_t key[EFS_KV_KEY_MAX], before[LANE_VAL], after[LANE_VAL];
    uint32_t kl = 0;
    uint8_t lane = (uint8_t)(ci % EFS_META_LANES);
    int rc;
    if (!kv || !out || !ino || !gen || cs < EFS_MIN_CHUNK_SIZE ||
        cs > EFS_MAX_CHUNK_SIZE || (cs & (cs - 1)))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_chunk_view view = {0};
        struct efs_txn_pending pend = {0};
        int moved = 0;
        struct efs_fence_history h1, h2;
        struct efs_fence_part parts[EFS_FENCE_PART_MAX] = {{0}};
        struct lane_rec stamp;
        uint32_t n1 = sizeof(before), n2 = sizeof(after);
        uint64_t epoch;
        /* Stamp first, history second: reversing these reads admits an
         * append/materialize/retire ABA between two empty histories. */
        rc = coord ? efs_txn_read_ex(kv, key, kl, coord, ctx, before, &n1, &pend) :
                     efs_kv_get(kv, key, kl, before, &n1);
        if (rc == EFS_ERR_NOT_FOUND) {
            n1 = 0;
            memset(&stamp, 0, sizeof(stamp));
        } else if (rc == EFS_OK) {
            rc = efs_kv_decode_result(kv, key, kl, unpack_lane(before, n1, &stamp));
        }
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
            return rc;
        rc = fence_history_read(kv, ino, gen, lane, coord, ctx, &pend, &h1);
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_apply_get_chunk_row(kv, ino, gen, ci, &view.base,
                                          view.deltas, EFS_CHUNK_DELTA_MAX,
                                          &view.ndelta, &view.delta_seq);
        if (rc != EFS_OK)
            return rc;
        if (view.ndelta > EFS_CHUNK_DELTA_MAX)
            return EFS_ERR_PROTO;
        rc = fence_history_read(kv, ino, gen, lane, coord, ctx, &pend, &h2);
        if (rc != EFS_OK)
            return rc;
        rc = coord ? efs_txn_read_ex(kv, key, kl, coord, ctx, after, &n2, &pend) :
                     efs_kv_get(kv, key, kl, after, &n2);
        if (rc == EFS_ERR_NOT_FOUND)
            n2 = 0;
        else if (rc != EFS_OK)
            return rc;
        if (coord) {
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
            if (rc != EFS_OK)
                return rc;
        }
        if (moved || n1 != n2 || memcmp(before, after, n1) ||
            memcmp(&h1, &h2, sizeof(h1)))
            continue;
        if (h1.count && h1.entries[h1.count - 1].epoch > stamp.fenced_epoch)
            return EFS_ERR_PROTO;
        view.fence_epoch = stamp.fenced_epoch;
        epoch = max_u64(stamp.fenced_epoch, view.base.content_epoch);
        parts[0] = (struct efs_fence_part){view.base.content_epoch, 0,
                                          view.base.generation ?
                                          (view.base.base_clip && view.base.base_clip < cs ?
                                           view.base.base_clip : cs) : 0};
        for (uint32_t i = 0; i < view.ndelta; ++i) {
            const struct efs_meta_delta *d = &view.deltas[i];
            epoch = max_u64(epoch, d->content_epoch);
            parts[i + 1] = (struct efs_fence_part){d->content_epoch, d->off, d->len};
        }
        rc = efs_fence_view_build(&view.bytes, epoch, (uint64_t)ci * cs,
                                   cs, &h1, parts, view.ndelta + 1);
        if (rc != EFS_OK)
            return EFS_ERR_PROTO;
        view.bytes.fence_epoch = stamp.fenced_epoch;
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_meta_get_chunk_view(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint32_t ci, uint32_t cs,
                            struct efs_meta_chunk_view *out)
{
    return efs_meta_get_chunk_view_tx(kv, ino, gen, ci, cs, NULL, NULL, out);
}

/* D25 read authority is the inode: its content epoch and fence history,
 * captured together. Two reads of each must agree, and no transaction the
 * reads depended on may have been decided in between. */
int efs_meta_get_inode_authority_tx(struct efs_kv *kv, efs_ino_t ino,
                                    efs_txn_coord_fn coord, void *ctx,
                                    struct efs_meta_row *row,
                                    struct efs_fence_history *history)
{
    if (!kv || !ino || !row || !history)
        return EFS_ERR_INVAL;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_row r1, r2;
        struct efs_fence_history h1, h2;
        struct efs_txn_pending pend = {0};
        int moved = 0;
        int rc = inode_read_tx(kv, ino, coord, ctx, &pend, &r1);
        if (rc == EFS_OK)
            rc = fence_history_read(kv, ino, r1.generation, EFS_META_FENCE_INODE,
                                    coord, ctx, &pend, &h1);
        if (rc == EFS_OK)
            rc = inode_read_tx(kv, ino, coord, ctx, &pend, &r2);
        if (rc == EFS_OK)
            rc = fence_history_read(kv, ino, r2.generation, EFS_META_FENCE_INODE,
                                    coord, ctx, &pend, &h2);
        if (rc == EFS_OK && coord)
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved || r1.generation != r2.generation ||
            r1.content_epoch != r2.content_epoch ||
            memcmp(&h1, &h2, sizeof(h1)))
            continue;
        if (h1.count && h1.entries[h1.count - 1].epoch > r1.content_epoch)
            return EFS_ERR_PROTO;
        *row = r2;
        *history = h1;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

/* A chunk view under the inode authority captured by
 * efs_meta_get_inode_authority_tx: masks come from the inode history, and
 * the lane stamp is not consulted. A part newer than the captured epoch
 * means a truncate committed after the capture: BUSY, recapture. */
int efs_meta_get_chunk_view_auth(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                 uint32_t ci, uint32_t cs, uint64_t epoch,
                                 const struct efs_fence_history *history,
                                 struct efs_meta_chunk_view *out)
{
    struct efs_meta_chunk_view view = {0};
    struct efs_fence_part parts[EFS_FENCE_PART_MAX] = {{0}};
    int rc;
    if (!kv || !out || !ino || !gen || !history || cs < EFS_MIN_CHUNK_SIZE ||
        cs > EFS_MAX_CHUNK_SIZE || (cs & (cs - 1)))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_chunk_row(kv, ino, gen, ci, &view.base, view.deltas,
                                      EFS_CHUNK_DELTA_MAX, &view.ndelta,
                                      &view.delta_seq);
    if (rc != EFS_OK)
        return rc;
    if (view.ndelta > EFS_CHUNK_DELTA_MAX)
        return EFS_ERR_PROTO;
    if (view.base.content_epoch > epoch)
        return EFS_ERR_BUSY;
    parts[0] = (struct efs_fence_part){view.base.content_epoch, 0,
                                      view.base.generation ?
                                      (view.base.base_clip && view.base.base_clip < cs ?
                                       view.base.base_clip : cs) : 0};
    for (uint32_t i = 0; i < view.ndelta; ++i) {
        const struct efs_meta_delta *d = &view.deltas[i];
        if (d->content_epoch > epoch)
            return EFS_ERR_BUSY;
        parts[i + 1] = (struct efs_fence_part){d->content_epoch, d->off, d->len};
    }
    if (efs_fence_view_build(&view.bytes, epoch, (uint64_t)ci * cs, cs, history,
                             parts, view.ndelta + 1) != EFS_OK)
        return EFS_ERR_PROTO;
    view.fence_epoch = epoch;
    view.bytes.fence_epoch = epoch;
    *out = view;
    return EFS_OK;
}

/* The lane stamp's adopted epoch; NOT_FOUND when the lane has no stamp. */
int efs_meta_get_lane_epoch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint8_t lane, uint64_t *epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;
    if (!kv || !ino || !gen || lane >= EFS_META_LANES || !epoch)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK)
        rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
    if (rc == EFS_OK)
        *epoch = ln.fenced_epoch;
    return rc;
}

/* D25 BOOTSTRAP on the lane's group: create a cross-group lane's stamp at
 * the inode epoch the proposer read, or adopt it if the stamp is older.
 * An equal or older epoch changes nothing. */
int efs_meta_apply_bootstrap_lane(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                  uint8_t lane, uint64_t epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;
    if (!kv || !ino || !gen || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND) {
        memset(&ln, 0, sizeof(ln));
        ln.fenced_epoch = epoch;
    } else if (rc == EFS_OK) {
        rc = unpack_lane(val, vn, &ln);
        if (rc != EFS_OK)
            return rc;
        if (!lane_adopt(&ln, epoch))
            return EFS_OK;
    } else {
        return rc;
    }
    pack_lane(val, &ln);
    return meta_write_put(kv, key, kl, val, LANE_VAL);
}

int efs_meta_apply_sweep_chunk(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint32_t ci,
                               const struct efs_meta_chunk_view *source,
                               const struct efs_meta_chunk *replacement)
{
    struct efs_meta_chunk_view live;
    struct packed_retirement retirement = {0};
    struct lane_rec lane;
    enum { CAP = 3 + EFS_CHUNK_DELTA_MAX };
    struct efs_kv_item items[CAP] = {0};
    uint8_t gk[CAP][EFS_KV_KEY_MAX], gv[CAP][EFS_META_GC_VAL];
    uint8_t ck[EFS_KV_KEY_MAX], lk[EFS_KV_KEY_MAX], lv[LANE_VAL];
    uint8_t expected[CHUNK_VAL_MAX], actual[CHUNK_VAL_MAX], value[CHUNK_VAL + EFS_PACK_REF_BYTES];
    uint32_t ckl = 0, lkl = 0, lvl = sizeof(lv), n = 0, en, an;
    uint8_t authority = (uint8_t)(ci % EFS_META_LANES);
    int rc;
    if (!source || source->ndelta > EFS_CHUNK_DELTA_MAX)
        return EFS_ERR_INVAL;
    rc = efs_meta_get_chunk_view(kv, ino, gen, ci, source->bytes.chunk_size, &live);
    if (rc != EFS_OK)
        return rc;
    en = pack_chunk_value(expected, &source->base, source->deltas, source->ndelta);
    an = pack_chunk_value(actual, &live.base, live.deltas, live.ndelta);
    if (en != an || memcmp(expected, actual, en) ||
        live.fence_epoch != source->fence_epoch ||
        live.delta_seq != source->delta_seq ||
        memcmp(&live.bytes, &source->bytes, sizeof(live.bytes)))
        return EFS_ERR_STALE;
    if (!replacement) {
        for (uint32_t i = 0; i < live.bytes.count; ++i)
            if (live.bytes.parts[i].len)
                return EFS_ERR_INVAL;
    } else {
        struct efs_meta_pub evidence = {0};
        if (replacement->content_epoch != live.bytes.revision)
            return EFS_ERR_STALE;
        evidence.candidate_gen = replacement->generation;
        evidence.coding_profile_id = replacement->coding_profile_id;
        evidence.ch = *replacement;
        rc = evidence_ok(&evidence);
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, authority), ino, gen,
                           authority, ci, ck, &ckl);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, authority), ino, gen,
                              authority, lk, &lkl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, lk, lkl, lv, &lvl);
    if (rc == EFS_OK)
        rc = unpack_lane(lv, lvl, &lane);
    if (rc != EFS_OK)
        return rc;
    lane.seq++;
    if (!replacement && lane.present)
        --lane.present;
    pack_lane(lv, &lane);
    if (replacement) {
        uint32_t length = pack_chunk_value(value, replacement, NULL, 0);
        items[n++] = (struct efs_kv_item){EFS_KV_PUT, ck, ckl, value, length};
    } else {
        items[n++] = (struct efs_kv_item){EFS_KV_DEL, ck, ckl, NULL, 0};
    }
    items[n++] = (struct efs_kv_item){EFS_KV_PUT, lk, lkl, lv, LANE_VAL};
    if (live.base.coding_profile_id == EFS_META_PROFILE_PACK) {
        if (ci) return EFS_ERR_PROTO;
        retirement.before = live.base; retirement.present = 1;
        if (replacement) { retirement.after = *replacement; retirement.has_after = 1; }
    } else if (live.base.generation && (!replacement ||
        (live.base.generation != replacement->generation &&
         !chunk_aliases(&live.base, replacement)))) {
        rc = gc_queue(items, &n, CAP, gk, gv, ino, authority, ci, &live.base);
        if (rc != EFS_OK)
            return rc;
    }
    for (uint32_t i = 0; i < live.ndelta; ++i) {
        const struct efs_meta_delta *d = &live.deltas[i];
        struct efs_meta_chunk dead = {0};
        if (!d->generation)
            continue;
        dead.generation = d->generation;
        memcpy(dead.nodes, d->nodes, sizeof(dead.nodes));
        memcpy(dead.checksums, d->checksums, sizeof(dead.checksums));
        if (replacement && (dead.generation == replacement->generation ||
                            chunk_aliases(&dead, replacement)))
            continue;
        rc = gc_queue(items, &n, CAP, gk, gv, ino, authority, ci, &dead);
        if (rc != EFS_OK)
            return rc;
    }
    return packed_retirement_commit(kv, ino, gen, &retirement, items, n, CAP);
}

struct check_scan {
    struct efs_kv *kv;
    int rc;
    int saw_root;
};

static int check_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct check_scan *c = user;
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    if (klen < 3)
        return 0;
    if (key[2] == EFS_KV_KIND_INODE) {
        rc = unpack_inode(val, vlen, &r);
        if (rc != EFS_OK) {
            c->rc = rc;
            return 1;
        }
        if (r.ino == EFS_ROOT_INO)
            c->saw_root = 1;
        if (efs_kv_inode_shard(r.ino) != (((uint32_t)key[0] << 8) | key[1])) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_TRUNC_PENDING) {
        if (klen != 11 || !rd64(key + 3) || vlen != 8 || !rd64(val) ||
            efs_kv_anchor_shard(efs_kv_inode_shard(rd64(key + 3))) !=
                (((uint32_t)key[0] << 8) | key[1])) {
            c->rc = EFS_ERR_PROTO; return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_APPEND_RECEIPT ||
        key[2] == EFS_KV_KIND_TRUNCATE_RECEIPT) {
        struct efs_opid_window window;
        if (klen != 31 || !rd64(key + 3) || !efs_load_be32(key + 27) ||
            efs_kv_inode_shard(rd64(key + 3)) != (((uint32_t)key[0] << 8) | key[1]) ||
            efs_opid_window_unpack(&window, val, vlen) != EFS_OK ||
            memcmp(window.client_uuid, key + 11, EFS_OPID_UUID_LEN) ||
            window.session_epoch != efs_load_be32(key + 27)) {
            c->rc = EFS_ERR_PROTO; return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_PUBLICATION_FLOOR) {
        if(klen!=43 || !rd64(key+3) || !rd64(key+11) || !efs_load_be32(key+39) ||
           efs_kv_lane_shard(rd64(key+3),efs_load_be32(key+19)%EFS_META_LANES)!=
             (((uint32_t)key[0]<<8)|key[1]) || vlen!=12 || efs_load_be32(val)!=1 || !rd64(val+4)) {
            c->rc=EFS_ERR_PROTO;return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_PUBLICATION) {
        uint32_t ci = klen == 51 ? efs_load_be32(key + 19) : 0;
        if (klen != 51 || !rd64(key + 3) || !rd64(key + 11) ||
            efs_kv_lane_shard(rd64(key + 3), ci % EFS_META_LANES) !=
                (((uint32_t)key[0] << 8) | key[1]) ||
            !efs_load_be32(key + 39) || !rd64(key + 43) ||
            !publication_receipt_valid(val,vlen)) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_LANE_AUTHORITY) {
        uint32_t cs; uint64_t epoch, floor;
        if (klen != 20 || !rd64(key + 3) || !rd64(key + 11) || key[19] >= EFS_META_LANES ||
            efs_kv_lane_shard(rd64(key + 3), key[19]) != (((uint32_t)key[0] << 8) | key[1]) ||
            unpack_lane_authority(val, vlen, &cs, &epoch, &floor) != EFS_OK) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_CONTENT_FENCE) {
        struct efs_fence_history history;
        uint32_t shard;
        uint8_t authority;
        if (klen != 20 || !rd64(key + 3) || !rd64(key + 11) ||
            key[19] > EFS_META_FENCE_INODE) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        authority = key[19];
        shard = authority == EFS_META_FENCE_INODE ?
                efs_kv_inode_shard(rd64(key + 3)) :
                efs_kv_lane_shard(rd64(key + 3), authority);
        if (shard != (((uint32_t)key[0] << 8) | key[1]) ||
            efs_meta_unpack_fence_history(val, vlen, &history) != EFS_OK) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] != EFS_KV_KIND_DENTRY)
        return 0;
    rc = unpack_dentry(val, vlen, &d);
    if (rc != EFS_OK) {
        c->rc = rc;
        return 1;
    }
    if (d.type == EFS_META_DENT_TOMBSTONE || d.ino == 0)
        return 0;
    rc = efs_meta_apply_get_inode(c->kv, d.ino, &r);
    if (rc == EFS_ERR_NOT_FOUND) {
        c->rc = EFS_ERR_IO; /* I9 */
        return 1;
    }
    if (rc != EFS_OK) {
        c->rc = rc;
        return 1;
    }
    return 0;
}

int efs_meta_apply_check(struct efs_kv *kv)
{
    struct check_scan c = { .kv = kv, .rc = EFS_OK, .saw_root = 0 };
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_kv_scan(kv, check_cb, &c);
    if (c.rc != EFS_OK)
        return c.rc;
    if (rc != EFS_OK)
        return rc;
    if (!c.saw_root)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

/* ---- W125 background packer: admit, switch, cancel ------------------- */

static int pack_chunk_key(efs_ino_t ino, uint64_t gen, uint8_t *key, uint32_t *kl)
{
    return efs_kv_key_chunk(efs_kv_lane_shard(ino, 0), ino, gen, 0, 0, key, kl);
}

/* Newest content epoch a row carries: its base or any span. */
static uint64_t pack_row_epoch(const uint8_t *val, uint32_t vlen,
                               const struct efs_meta_chunk *base)
{
    uint64_t epoch = base->content_epoch;
    uint32_t nd = trailer_n(val, vlen);
    for (uint32_t i = 0; i < nd; i++) {
        struct efs_meta_delta d;
        if (trailer_at(val, vlen, i, &d) == EFS_OK)
            epoch = max_u64(epoch, d.content_epoch);
    }
    return epoch;
}

int efs_meta_pack_source(struct efs_kv *kv, efs_ino_t ino, uint64_t inode_gen,
                         uint8_t digest[EFS_HASH_SIZE], struct efs_meta_chunk *base,
                         uint64_t *content_epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t kl = 0, vl = sizeof(val);
    struct efs_meta_chunk ch;
    if (!kv || !ino || !inode_gen || !digest) return EFS_ERR_INVAL;
    int rc = pack_chunk_key(ino, inode_gen, key, &kl);
    if (rc == EFS_OK) rc = kv_get_copy(kv, key, kl, val, sizeof(val), &vl);
    if (rc == EFS_OK) rc = unpack_chunk(val, vl, &ch);
    if (rc == EFS_OK) rc = efs_pack_chunk_identity(&ch, ino, inode_gen, 0);
    if (rc != EFS_OK) return rc;
    efs_hash(val, vl, digest);
    if (base) *base = ch;
    if (content_epoch) *content_epoch = pack_row_epoch(val, vl, &ch);
    return EFS_OK;
}

int efs_meta_apply_pack_admit(struct efs_kv *kv, uint32_t group,
                              const struct efs_pack_object *o)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    size_t len;
    if (!kv || !o || o->ledger.publication_group != group) return EFS_ERR_INVAL;
    struct efs_pack_ledger_usage u;
    efs_pack_ledger_usage(&o->ledger, &u);
    if (o->ledger.phase != EFS_PACK_ACTIVE || u.pending != o->ledger.count ||
        o->ledger.fence_acks || o->ledger.delete_acks) return EFS_ERR_INVAL;
    int rc = efs_pack_object_key(group, o->ledger.segment_id, o->ledger.segment_generation, key);
    if (rc != EFS_OK) return rc;
    uint8_t *value = malloc(2 * EFS_PACK_OBJECT_MAX_BYTES);
    if (!value) return EFS_ERR_NOMEM;
    uint8_t *old = value + EFS_PACK_OBJECT_MAX_BYTES;
    rc = efs_pack_object_encode(o, value, EFS_PACK_OBJECT_MAX_BYTES, &len);
    if (rc == EFS_OK) rc = efs_txn_key_exclusive(kv, key, sizeof(key));
    if (rc == EFS_OK) {
        uint32_t ol = EFS_PACK_OBJECT_MAX_BYTES;
        int g = efs_kv_get(kv, key, sizeof(key), old, &ol);
        if (g == EFS_OK) {
            /* A replay of the same admission is a no-op; a different object
             * under the same identity is refused. Once switched, the stored
             * ledger differs: a late replay never resets it. */
            struct efs_pack_object *cur = malloc(sizeof(*cur));
            if (!cur) rc = EFS_ERR_NOMEM;
            else {
                rc = efs_pack_object_decode(old, ol, cur);
                if (rc == EFS_OK && (memcmp(cur->ledger.body_digest, o->ledger.body_digest, EFS_HASH_SIZE) ||
                    memcmp(cur->ledger.reference_digest, o->ledger.reference_digest,
                           sizeof(o->ledger.reference_digest[0]) * o->ledger.count) ||
                    cur->ledger.count != o->ledger.count))
                    rc = EFS_ERR_EXIST;
                free(cur);
            }
            free(value);
            return rc;
        }
        if (g != EFS_ERR_NOT_FOUND) rc = g;
    }
    /* Packed rows become visible only on a store that older builds refuse. */
    if (rc == EFS_OK) rc = efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V2);
    if (rc == EFS_OK) rc = meta_write_put(kv, key, sizeof(key), value, (uint32_t)len);
    free(value);
    return rc;
}

int efs_meta_apply_pack_cancel(struct efs_kv *kv, uint32_t group, uint64_t id, uint64_t gen)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    size_t len;
    if (!kv) return EFS_ERR_INVAL;
    int rc = efs_pack_object_key(group, id, gen, key);
    if (rc != EFS_OK) return rc;
    struct efs_pack_object *o = malloc(sizeof(*o) + EFS_PACK_OBJECT_MAX_BYTES);
    if (!o) return EFS_ERR_NOMEM;
    uint8_t *value = (uint8_t *)(o + 1);
    rc = efs_txn_key_exclusive(kv, key, sizeof(key));
    if (rc == EFS_OK) rc = efs_pack_object_get(kv, group, id, gen, o);
    int changed = 0;
    if (rc == EFS_OK && o->ledger.phase == EFS_PACK_ACTIVE) {
        for (uint32_t i = 0; i < o->ledger.count; i++)
            if (o->ledger.state[i] == EFS_PACK_PENDING) { o->ledger.state[i] = EFS_PACK_CANCELLED; changed = 1; }
        if (changed) {
            int r = efs_pack_ledger_reclaim_begin(&o->ledger);
            if (r != EFS_OK && r != EFS_ERR_BUSY) rc = r;
        }
    }
    if (rc == EFS_OK && changed) rc = efs_pack_object_encode(o, value, EFS_PACK_OBJECT_MAX_BYTES, &len);
    if (rc == EFS_OK && changed) rc = meta_write_put(kv, key, sizeof(key), value, (uint32_t)len);
    free(o);
    return rc;
}

/* Per-member staging for one switch batch. */
struct pack_switch_slot {
    uint8_t ck[EFS_KV_KEY_MAX], lk[EFS_KV_KEY_MAX];
    uint32_t ckl, lkl;
    uint8_t cv[CHUNK_VAL + EFS_PACK_REF_BYTES], lv[LANE_VAL];
    uint32_t cvl;
    struct efs_meta_chunk old;
};
#define PACK_SWITCH_ITEMS_PER (3u + EFS_CHUNK_DELTA_MAX)

/* One member's checks. EFS_OK: switch it; EFS_ERR_STALE (or any logical
 * refusal): cancel it; EFS_ERR_IO/NOMEM: abort the whole entry. */
static int pack_switch_member(struct efs_kv *kv, const struct efs_pack_object *o,
                              const struct efs_pack_switch_member *m, unsigned at,
                              struct pack_switch_slot *s, uint8_t *val, uint32_t *vl,
                              uint64_t *epoch)
{
    struct efs_meta_row row;
    uint8_t digest[EFS_HASH_SIZE];
    if (o->ledger.state[at] != EFS_PACK_PENDING) return EFS_ERR_STALE;
    int rc = efs_meta_apply_get_inode(kv, m->ino, &row);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    if (row.generation != m->inode_generation || !row.nlink || !S_ISREG(row.mode))
        return EFS_ERR_STALE;
    rc = efs_lease_any(kv, m->ino, row.generation);
    if (rc == 1) return EFS_ERR_STALE;     /* open: retry in a later pass */
    if (rc != EFS_OK) return rc;
    rc = pack_chunk_key(m->ino, row.generation, s->ck, &s->ckl);
    if (rc == EFS_OK) rc = efs_kv_key_lane(efs_kv_lane_shard(m->ino, 0), m->ino,
                                           row.generation, 0, s->lk, &s->lkl);
    if (rc != EFS_OK) return rc;
    *vl = CHUNK_VAL_MAX;
    rc = kv_get_copy(kv, s->ck, s->ckl, val, CHUNK_VAL_MAX, vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    efs_hash(val, *vl, digest);
    if (memcmp(digest, m->source_digest, EFS_HASH_SIZE)) return EFS_ERR_STALE;
    rc = unpack_chunk(val, *vl, &s->old);
    if (rc == EFS_OK) rc = efs_pack_chunk_identity(&s->old, m->ino, row.generation, 0);
    if (rc != EFS_OK) return EFS_ERR_STALE;
    *epoch = pack_row_epoch(val, *vl, &s->old);
    if (m->ref.content_epoch != *epoch || m->ref.record_generation == s->old.generation)
        return EFS_ERR_STALE;
    for (uint32_t i = 0, nd = trailer_n(val, *vl); i < nd; i++) {
        struct efs_meta_delta d;
        if (trailer_at(val, *vl, i, &d) != EFS_OK) return EFS_ERR_STALE;
        if (d.generation == m->ref.record_generation) return EFS_ERR_STALE;
    }
    if (s->old.coding_profile_id == EFS_META_PROFILE_PACK &&
        s->old.packed.segment_id == o->ledger.segment_id &&
        s->old.packed.segment_generation == o->ledger.segment_generation)
        return EFS_ERR_STALE;
    struct lane_rec lane;
    uint32_t ll = sizeof(s->lv);
    rc = efs_kv_get(kv, s->lk, s->lkl, s->lv, &ll);
    if (rc == EFS_OK) rc = unpack_lane(s->lv, ll, &lane);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_PROTO) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    lane.seq++;
    pack_lane(s->lv, &lane);
    struct efs_meta_chunk nw;
    memset(&nw, 0, sizeof(nw));
    nw.generation = m->ref.record_generation;
    nw.coding_profile_id = EFS_META_PROFILE_PACK;
    nw.content_epoch = *epoch;
    memcpy(nw.nodes, o->nodes, sizeof(nw.nodes));
    memcpy(nw.checksums, o->checksums, sizeof(nw.checksums));
    nw.packed = m->ref;
    s->cvl = pack_chunk_value(s->cv, &nw, NULL, 0);
    return EFS_OK;
}

int efs_meta_apply_pack_switch(struct efs_kv *kv, const struct efs_pack_switch *sw,
                               uint32_t *switched)
{
    uint8_t okey[EFS_PACK_OBJECT_KEY_BYTES];
    size_t olen;
    uint32_t n = 0, live = 0;
    int rc;
    if (switched) *switched = 0;
    if (!kv || !sw || !sw->count || sw->count > EFS_PACK_SWITCH_MAX) return EFS_ERR_INVAL;
    rc = efs_pack_object_key(sw->group, sw->segment_id, sw->segment_generation, okey);
    if (rc != EFS_OK) return rc;
    const uint32_t cap = sw->count * PACK_SWITCH_ITEMS_PER + 1u + EFS_PACK_MAPPING_OBJECT_MAX;
    struct efs_pack_object *o = malloc(sizeof(*o));
    struct pack_switch_slot *slots = calloc(sw->count, sizeof(*slots));
    struct efs_kv_item *it = calloc(cap, sizeof(*it));
    uint8_t (*gk)[EFS_KV_KEY_MAX] = calloc(cap, EFS_KV_KEY_MAX);
    uint8_t (*gv)[EFS_META_GC_VAL] = calloc(cap, EFS_META_GC_VAL);
    struct efs_pack_mapping_change *rel = calloc(sw->count, sizeof(*rel));
    uint8_t *val = malloc(CHUNK_VAL_MAX), *ov = malloc(EFS_PACK_OBJECT_MAX_BYTES);
    struct efs_pack_mapping_plan plan = {0};
    uint32_t nrel = 0;
    if (!o || !slots || !it || !gk || !gv || !rel || !val || !ov) { rc = EFS_ERR_NOMEM; goto out; }
    rc = efs_txn_key_exclusive(kv, okey, sizeof(okey));
    if (rc == EFS_OK) rc = efs_pack_object_get(kv, sw->group, sw->segment_id, sw->segment_generation, o);
    if (rc != EFS_OK) goto out;
    if (o->ledger.phase != EFS_PACK_ACTIVE) { rc = EFS_ERR_STALE; goto out; }
    for (uint32_t i = 0; i < sw->count; i++) {
        const struct efs_pack_switch_member *m = &sw->members[i];
        struct pack_switch_slot *s = &slots[i];
        unsigned at;
        uint32_t vl = 0;
        uint64_t epoch = 0;
        int mrc = EFS_ERR_STALE;
        if (m->ref.publication_group != sw->group || m->ref.segment_id != sw->segment_id ||
            m->ref.segment_generation != sw->segment_generation || m->ref.ino != m->ino ||
            m->ref.inode_generation != m->inode_generation ||
            m->ref.publication_group != efs_raft_shard_group(efs_kv_lane_shard(m->ino, 0)))
            continue; /* not a record of this segment: nothing to cancel */
        if (efs_pack_ledger_slot(&o->ledger, &m->ref, &at) != EFS_OK) continue;
        /* A member already in the batch must not switch twice. */
        int dup = 0;
        for (uint32_t j = 0; j < i; j++) if (sw->members[j].ino == m->ino) dup = 1;
        if (!dup) mrc = pack_switch_member(kv, o, m, at, s, val, &vl, &epoch);
        if (mrc == EFS_ERR_IO || mrc == EFS_ERR_NOMEM) { rc = mrc; goto out; }
        if (mrc != EFS_OK) {
            (void)efs_pack_ledger_cancel(&o->ledger, &m->ref);
            continue;
        }
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, s->ck, s->ckl, s->cv, s->cvl};
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, s->lk, s->lkl, s->lv, LANE_VAL};
        /* The superseded per-file fragments (base and spans) are dead from
         * this commit on (L7); an old packed record is released instead. */
        if (s->old.coding_profile_id == EFS_META_PROFILE_PACK)
            rel[nrel++] = (struct efs_pack_mapping_change){m->ino, m->inode_generation, 0, &s->old, NULL};
        else if (s->old.generation) {
            rc = gc_queue(it, &n, cap, gk, gv, m->ino, 0, 0, &s->old);
            if (rc != EFS_OK) goto out;
        }
        for (uint32_t d = 0, nd = trailer_n(val, vl); d < nd; d++) {
            struct efs_meta_delta dd;
            struct efs_meta_chunk span;
            rc = trailer_at(val, vl, d, &dd);
            if (rc != EFS_OK) goto out;
            if (!dd.generation) continue;
            memset(&span, 0, sizeof(span));
            span.generation = dd.generation;
            memcpy(span.nodes, dd.nodes, sizeof(span.nodes));
            memcpy(span.checksums, dd.checksums, sizeof(span.checksums));
            rc = gc_queue(it, &n, cap, gk, gv, m->ino, 0, 0, &span);
            if (rc != EFS_OK) goto out;
        }
        rc = efs_pack_ledger_publish(&o->ledger, &m->ref);
        if (rc != EFS_OK) goto out;
        live++;
    }
    if (nrel) {
        rc = efs_pack_mapping_plan(kv, rel, nrel, &plan);
        if (rc != EFS_OK) goto out;
        if (n + plan.count + 1 > cap) { rc = EFS_ERR_NOMEM; goto out; }
        memcpy(it + n, plan.items, plan.count * sizeof(it[0]));
        n += plan.count;
    }
    rc = efs_pack_ledger_reclaim_begin(&o->ledger);
    if (rc != EFS_OK && rc != EFS_ERR_BUSY) goto out;
    rc = efs_pack_object_encode(o, ov, EFS_PACK_OBJECT_MAX_BYTES, &olen);
    if (rc != EFS_OK) goto out;
    it[n++] = (struct efs_kv_item){EFS_KV_PUT, okey, sizeof(okey), ov, (uint32_t)olen};
    rc = meta_write_batch(kv, it, n);
    if (rc == EFS_OK && switched) *switched = live;
out:
    efs_pack_mapping_plan_free(&plan);
    free(o); free(slots); free(it); free(gk); free(gv); free(rel); free(val); free(ov);
    return rc;
}

/* Every member durably accepted these reclaiming segments into its reclaim
 * journal (fenced, physical deletion owned by the node): forget them. A
 * segment still ACTIVE is refused; an absent one is already retired. */
int efs_meta_apply_pack_retire(struct efs_kv *kv, uint32_t group, const uint64_t *ids,
                               const uint64_t *gens, uint32_t n, uint32_t *retired)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    uint32_t done = 0;
    int rc = EFS_OK;
    if (retired) *retired = 0;
    if (!kv || (n && (!ids || !gens)) || n > EFS_PACK_RETIRE_MAX) return EFS_ERR_INVAL;
    struct efs_pack_object *o = malloc(sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    for (uint32_t i = 0; i < n && rc == EFS_OK; i++) {
        rc = efs_pack_object_key(group, ids[i], gens[i], key);
        if (rc == EFS_OK) rc = efs_txn_key_exclusive(kv, key, sizeof(key));
        if (rc == EFS_OK) rc = efs_pack_object_get(kv, group, ids[i], gens[i], o);
        if (rc == EFS_ERR_NOT_FOUND) { rc = EFS_OK; continue; }
        if (rc != EFS_OK) break;
        if (o->ledger.phase == EFS_PACK_ACTIVE) continue;
        /* K37: the segment's degraded mark goes first, so a replay after a
         * crash between the two deletes still finds the object and retires
         * both; a mark never outlives its segment. */
        uint8_t mk[EFS_PACK_OBJECT_KEY_BYTES];
        rc = efs_pack_degraded_key(group, ids[i], gens[i], mk);
        if (rc == EFS_OK) rc = meta_write_del(kv, mk, sizeof(mk));
        if (rc == EFS_ERR_NOT_FOUND) rc = EFS_OK;
        if (rc == EFS_OK) rc = meta_write_del(kv, key, sizeof(key));
        if (rc == EFS_ERR_NOT_FOUND) rc = EFS_OK;
        if (rc == EFS_OK) done++;
    }
    free(o);
    if (retired) *retired = done;
    return rc;
}

/* K37 segment degraded marks (pack_mapping.h). Serialized apply on the
 * segment's group; the object row is read under the same exclusion as the
 * packer's own commands. */
int efs_meta_apply_pack_degraded(struct efs_kv *kv, uint32_t group, uint32_t op, uint64_t id,
                                 uint64_t gen, uint32_t mask, uint32_t reason, int *outcome)
{
    uint8_t okey[EFS_PACK_OBJECT_KEY_BYTES], mkey[EFS_PACK_OBJECT_KEY_BYTES];
    uint8_t have = 0, why = 0, v[EFS_PACK_DEGRADED_VAL] = {0};
    if (outcome) *outcome = 0;
    if (!kv || !mask || mask > 7u || reason > 0xffu ||
        (op != EFS_MD_PACK_DEGRADED_MARK && op != EFS_MD_PACK_DEGRADED_CLEAR &&
         op != EFS_MD_PACK_DEGRADED_RETIRE) ||
        (op == EFS_MD_PACK_DEGRADED_MARK && !reason) ||
        (op == EFS_MD_PACK_DEGRADED_CLEAR && (mask & (mask - 1u))))
        return EFS_ERR_INVAL;
    int rc = efs_pack_object_key(group, id, gen, okey);
    if (rc == EFS_OK) rc = efs_pack_degraded_key(group, id, gen, mkey);
    if (rc != EFS_OK) return rc;
    rc = efs_txn_key_exclusive(kv, okey, sizeof(okey));
    if (rc != EFS_OK) return rc;
    rc = efs_pack_degraded_get(kv, group, id, gen, &have, &why);
    if (rc == EFS_ERR_NOT_FOUND) { have = 0; rc = EFS_OK; }
    if (rc != EFS_OK) return rc; /* a malformed mark is never overwritten */
    struct efs_pack_object *o = malloc(sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    int orc = efs_pack_object_get(kv, group, id, gen, o);
    if (orc != EFS_OK && orc != EFS_ERR_NOT_FOUND) { free(o); return orc; }
    struct efs_pack_ledger_usage u = {0};
    if (orc == EFS_OK) efs_pack_ledger_usage(&o->ledger, &u);
    int active = orc == EFS_OK && o->ledger.phase == EFS_PACK_ACTIVE;
    free(o);
    if (op == EFS_MD_PACK_DEGRADED_MARK) {
        /* Only an ACTIVE segment holds data that a lost fragment endangers. */
        if (!active) return EFS_ERR_STALE;
        if ((have | mask) == have && (why | reason) == why) return EFS_OK;
        v[0] = (uint8_t)(have | mask);
        v[1] = (uint8_t)(why | reason);
        rc = meta_write_put(kv, mkey, sizeof(mkey), v, sizeof(v));
    } else {
        if (!have) return EFS_OK; /* already cleared: idempotent */
        if (have != mask) return EFS_ERR_STALE; /* decided for another mark */
        if (op == EFS_MD_PACK_DEGRADED_RETIRE && active && (u.live || u.pending))
            return EFS_ERR_STALE;
        rc = meta_write_del(kv, mkey, sizeof(mkey));
    }
    if (rc == EFS_OK && outcome) *outcome = 1;
    return rc;
}

/* ---- D25 truncate sweep (protocol §4.2-§4.4) ---- */

#define SWEEP_SCAN_MAX 256u
#define SWEEP_CHANGE_MAX 64u
#define SWEEP_ITEMS (SWEEP_SCAN_MAX + SWEEP_CHANGE_MAX * (2u + EFS_CHUNK_DELTA_MAX) + 8u)

struct tsweep_row {
    uint32_t ci;
    uint32_t klen, vlen;
    uint8_t key[EFS_KV_KEY_MAX];
    uint8_t val[CHUNK_VAL_MAX];
};
struct tsweep_scan {
    efs_ino_t ino;
    uint64_t gen;
    uint8_t lane;
    uint32_t n;
    int more;
    struct tsweep_row *rows;
};
static int tsweep_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                         const uint8_t *val, uint32_t vlen)
{
    struct tsweep_scan *sc = user;
    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK || rd64(key + 3) != sc->ino ||
        rd64(key + 11) != sc->gen || key[19] != sc->lane)
        return 0;
    if (sc->n == SWEEP_SCAN_MAX) {
        sc->more = 1;
        return 1;
    }
    if (klen > EFS_KV_KEY_MAX || vlen > CHUNK_VAL_MAX)
        return 1;
    struct tsweep_row *r = &sc->rows[sc->n++];
    r->ci = efs_load_be32(key + 20);
    r->klen = klen;
    r->vlen = vlen;
    memcpy(r->key, key, klen);
    memcpy(r->val, val, vlen);
    return 0;
}

/* Visible length of a part at chunk offset `off` (file offset c + off) that
 * would otherwise show `len` bytes, under history S. */
static uint32_t sweep_visible(const struct efs_fence_history *s, uint64_t epoch,
                              uint64_t c, uint32_t off, uint32_t len)
{
    uint64_t end = efs_fence_valid_end(s, epoch), start = c + off;
    if (end <= start)
        return 0;
    return end - start < len ? (uint32_t)(end - start) : len;
}

struct tsweep_page {
    struct tsweep_row rows[SWEEP_SCAN_MAX];
    struct efs_kv_item it[SWEEP_ITEMS];
    uint8_t vals[SWEEP_CHANGE_MAX][CHUNK_VAL_MAX];
    uint8_t gc_keys[SWEEP_ITEMS][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_ITEMS][EFS_META_GC_VAL];
};

int efs_meta_apply_trunc_sweep(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint8_t lane, uint64_t target, uint32_t cs,
                               const struct efs_fence_history *snap)
{
    uint8_t lk[EFS_KV_KEY_MAX], lv[LANE_VAL], pref[EFS_KV_KEY_MAX], start[EFS_KV_KEY_MAX];
    uint32_t lkl = 0, lvn = sizeof(lv), plen = 0, slen = 0, n = 0, changed = 0, dropped = 0;
    struct lane_rec ln;
    struct packed_retirement retirement = {0};
    struct tsweep_scan sc;
    struct tsweep_page *pg;
    uint32_t i;
    int rc, stop = 0;

    if (!kv || !ino || !gen || lane >= EFS_META_LANES || !target || !snap ||
        cs < EFS_MIN_CHUNK_SIZE || cs > EFS_MAX_CHUNK_SIZE || (cs & (cs - 1)) ||
        efs_fence_history_valid(snap) != EFS_OK || !snap->count ||
        snap->entries[snap->count - 1].epoch != target)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, lk, &lkl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, lk, lkl, lv, &lvn);
    if (rc == EFS_ERR_NOT_FOUND) {
        memset(&ln, 0, sizeof(ln));
        ln.fenced_epoch = target;
    } else if (rc == EFS_OK) {
        rc = unpack_lane(lv, lvn, &ln);
        if (rc != EFS_OK)
            return rc;
    } else {
        return rc;
    }
    if (target < ln.sweep_target)
        return EFS_OK; /* obsolete command */
    if (target > ln.sweep_target) {
        ln.sweep_target = target;
        ln.sweep_next_ci = 0;
    }
    (void)lane_adopt(&ln, target);
    if (ln.swept_epoch >= target) {
        pack_lane(lv, &ln);
        return meta_write_put(kv, lk, lkl, lv, LANE_VAL);
    }
    pg = malloc(sizeof(*pg));
    if (!pg)
        return EFS_ERR_NOMEM;
    memset(&sc, 0, sizeof(sc));
    sc.ino = ino; sc.gen = gen; sc.lane = lane; sc.rows = pg->rows;
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane, 0, pref, &plen);
    if (rc == EFS_OK)
        rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane,
                              ln.sweep_next_ci > UINT32_MAX ? UINT32_MAX :
                              (uint32_t)ln.sweep_next_ci, start, &slen);
    if (rc == EFS_OK && ln.sweep_next_ci <= UINT32_MAX)
        rc = efs_kv_scan_from(kv, pref, 20, start, slen, tsweep_scan_cb, &sc);
    if (rc < 0) {
        free(pg);
        return rc;
    }
    rc = EFS_OK;
    uint64_t next = ln.sweep_next_ci;
    for (i = 0; i < sc.n && rc == EFS_OK && !stop; i++) {
        struct tsweep_row *r = &pg->rows[i];
        struct efs_meta_chunk base;
        struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
        uint32_t nd = trailer_n(r->val, r->vlen), j, need;
        uint64_t c = (uint64_t)r->ci * cs;
        int dirty = 0, live = 0;

        /* A row under a pending transaction intent waits: stop here, the
         * cursor stays on it, a later entry retries. */
        need = 3u + 2u * (nd + 1u);
        if (changed == SWEEP_CHANGE_MAX || n + need > SWEEP_ITEMS ||
            meta_write_check(kv, r->key, r->klen) != EFS_OK) {
            stop = 1;
            break;
        }
        rc = unpack_chunk(r->val, r->vlen, &base);
        for (j = 0; rc == EFS_OK && j < nd; j++)
            rc = trailer_at(r->val, r->vlen, j, &ds[j]);
        if (rc != EFS_OK)
            break;
        if (base.generation) {
            uint32_t blen = base.base_clip && base.base_clip < cs ? base.base_clip : cs;
            uint32_t vis = sweep_visible(snap, base.content_epoch, c, 0, blen);
            if (!vis) {
                if (base.coding_profile_id == EFS_META_PROFILE_PACK) {
                    if (retirement.present) { stop = 1; break; } /* one per batch */
                    retirement.before = base;
                    retirement.present = 1;
                } else {
                    rc = gc_queue(pg->it, &n, SWEEP_ITEMS, pg->gc_keys, pg->gc_vals,
                                  ino, lane, r->ci, &base);
                    if (rc != EFS_OK)
                        break;
                }
                uint64_t epoch = base.content_epoch;
                memset(&base, 0, sizeof(base));
                base.coding_profile_id = EFS_META_PROFILE_K2F1;
                base.content_epoch = epoch;
                dirty = 1;
            } else if (vis < blen) {
                base.base_clip = vis;
                dirty = 1;
            }
        }
        for (j = 0; j < nd && rc == EFS_OK; j++) {
            struct efs_meta_delta *d = &ds[j];
            uint32_t vis;
            if (!d->len || !d->generation)
                continue;
            vis = sweep_visible(snap, d->content_epoch, c, d->off, d->len);
            if (!vis) {
                struct efs_meta_chunk span;
                memset(&span, 0, sizeof(span));
                span.generation = d->generation;
                memcpy(span.nodes, d->nodes, sizeof(span.nodes));
                memcpy(span.checksums, d->checksums, sizeof(span.checksums));
                rc = gc_queue(pg->it, &n, SWEEP_ITEMS, pg->gc_keys, pg->gc_vals,
                              ino, lane, r->ci, &span);
                d->off = 0;
                d->len = 0; /* tombstone: its object is now GC's */
                dirty = 1;
            } else if (vis < d->len) {
                d->len = vis;
                dirty = 1;
            }
        }
        if (rc != EFS_OK)
            break;
        live = base.generation != 0;
        for (j = 0; j < nd; j++)
            live |= ds[j].len != 0;
        if (!live) {
            pg->it[n++] = (struct efs_kv_item){EFS_KV_DEL, r->key, r->klen, NULL, 0};
            dropped++;
            changed++;
        } else if (dirty) {
            uint32_t vl = pack_chunk_value(pg->vals[changed], &base, ds, nd);
            pg->it[n++] = (struct efs_kv_item){EFS_KV_PUT, r->key, r->klen,
                                               pg->vals[changed], vl};
            if (retirement.present && !retirement.has_after && r->ci == 0) {
                retirement.after = base;
                retirement.has_after = 1;
            }
            changed++;
        }
        next = (uint64_t)r->ci + 1;
    }
    if (rc == EFS_OK) {
        ln.sweep_next_ci = next;
        if (!stop && !sc.more)
            ln.swept_epoch = max_u64(ln.swept_epoch, target);
        ln.present = ln.present >= dropped ? ln.present - dropped : 0;
        if (changed)
            ln.seq++;
        pack_lane(lv, &ln);
        pg->it[n++] = (struct efs_kv_item){EFS_KV_PUT, lk, lkl, lv, LANE_VAL};
        rc = packed_retirement_commit(kv, ino, gen, &retirement, pg->it, n, SWEEP_ITEMS);
    }
    free(pg);
    return rc;
}

int efs_meta_get_lane_swept(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint8_t lane, uint64_t *swept_epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;
    if (!kv || !ino || !gen || lane >= EFS_META_LANES || !swept_epoch)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK)
        rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
    if (rc == EFS_OK)
        *swept_epoch = ln.swept_epoch;
    return rc;
}

/* Inode group: record that `lane` holds swept_epoch >= target. Obsolete or
 * foreign evidence is OK with no change. */
int efs_meta_apply_sweep_done(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane, uint64_t target)
{
    struct efs_meta_row row;
    struct efs_fence_history h;
    uint8_t k[EFS_KV_KEY_MAX], v[EFS_META_FENCE_BYTES];
    uint32_t kl = 0, vl = 0;
    int rc;
    if (!kv || !ino || lane >= EFS_META_LANES || !target)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    if (row.generation != gen || !(row.active_lanes & (1ull << lane)))
        return EFS_OK;
    rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE, NULL, NULL, NULL, &h);
    if (rc != EFS_OK)
        return rc;
    for (uint32_t i = 0; i < h.count; i++) {
        if (h.entries[i].epoch != target)
            continue;
        if (h.entries[i].done & (1ull << lane))
            return EFS_OK;
        h.entries[i].done |= 1ull << lane;
        rc = content_fence_key(ino, gen, EFS_META_FENCE_INODE, k, &kl);
        if (rc == EFS_OK)
            rc = efs_meta_pack_fence_history(&h, v, sizeof(v), &vl);
        return rc == EFS_OK ? meta_write_put(kv, k, kl, v, vl) : rc;
    }
    return EFS_OK;
}

/* Inode group: drop every entry <= target once all active lanes are done
 * for target. The record stays (count 0, revision kept); TRUNC_PENDING goes
 * with the last entry. */
int efs_meta_apply_retire(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          uint64_t target)
{
    struct efs_meta_row row;
    struct efs_fence_history h, next;
    uint8_t k[EFS_KV_KEY_MAX], v[EFS_META_FENCE_BYTES], kp[EFS_KV_KEY_MAX];
    uint32_t kl = 0, vl = 0, kpl = 0, at = UINT32_MAX;
    struct efs_kv_item it[2];
    int rc;
    if (!kv || !ino || !target)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND || (rc == EFS_OK && row.generation != gen)) {
        /* The file is gone: its pending sweep is the reaper's now. */
        uint8_t pv[8];
        uint32_t pvl = sizeof(pv);
        rc = trunc_pending_key(ino, kp, &kpl);
        if (rc == EFS_OK)
            rc = efs_kv_get(kv, kp, kpl, pv, &pvl);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_OK;
        if (rc != EFS_OK)
            return rc;
        if (pvl != 8 || rd64(pv) != gen)
            return EFS_OK;
        rc = meta_write_del(kv, kp, kpl);
        return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
    }
    if (rc != EFS_OK)
        return rc;
    rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE, NULL, NULL, NULL, &h);
    if (rc != EFS_OK)
        return rc;
    for (uint32_t i = 0; i < h.count; i++)
        if (h.entries[i].epoch == target)
            at = i;
    if (at == UINT32_MAX)
        return EFS_OK;
    if ((h.entries[at].done & row.active_lanes) != row.active_lanes)
        return EFS_ERR_INVAL;
    if (h.revision == UINT64_MAX)
        return EFS_ERR_INVAL;
    next = h;
    next.count = 0;
    for (uint32_t i = at + 1; i < h.count; i++)
        next.entries[next.count++] = h.entries[i];
    next.revision++;
    rc = content_fence_key(ino, gen, EFS_META_FENCE_INODE, k, &kl);
    if (rc == EFS_OK)
        rc = efs_meta_pack_fence_history(&next, v, sizeof(v), &vl);
    if (rc != EFS_OK)
        return rc;
    it[0] = (struct efs_kv_item){EFS_KV_PUT, k, kl, v, vl};
    if (next.count)
        return meta_write_batch(kv, it, 1);
    rc = trunc_pending_key(ino, kp, &kpl);
    if (rc != EFS_OK)
        return rc;
    it[1] = (struct efs_kv_item){EFS_KV_DEL, kp, kpl, NULL, 0};
    return meta_write_batch(kv, it, 2);
}

int efs_meta_pack_candidate(struct efs_kv *kv, efs_ino_t ino, uint64_t inode_gen,
                            struct efs_pack_candidate *out)
{
    struct efs_meta_row row;
    struct lane_rec lane;
    uint8_t lk[EFS_KV_KEY_MAX], lv[LANE_VAL];
    uint32_t lkl = 0, lvl = sizeof(lv);
    if (!kv || !ino || !out) return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    int rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    if (row.generation != inode_gen) return EFS_ERR_STALE;
    out->ino = ino; out->parent = row.parent; out->inode_generation = row.generation;
    out->mode = row.mode; out->nlink = row.nlink; out->active_lanes = row.active_lanes;
    memset(&lane, 0, sizeof(lane));
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, 0), ino, row.generation, 0, lk, &lkl);
    if (rc == EFS_OK) rc = efs_kv_get(kv, lk, lkl, lv, &lvl);
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, lk, lkl, unpack_lane(lv, lvl, &lane));
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    /* D25: the packed image is masked by the inode history (and the base
     * clip); a lane EOF counts only at the current epoch. */
    struct efs_fence_history hist;
    rc = fence_history_read(kv, ino, row.generation, EFS_META_FENCE_INODE,
                            NULL, NULL, NULL, &hist);
    if (rc != EFS_OK) return rc;
    out->size = lane.fenced_epoch == row.content_epoch ?
                max_u64(row.base_size, lane.max_end) : row.base_size;
    out->mtime = max_u64(row.base_mtime, lane.max_mtime);
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t kl = 0, vl = sizeof(val);
    rc = pack_chunk_key(ino, row.generation, key, &kl);
    if (rc == EFS_OK) rc = kv_get_copy(kv, key, kl, val, sizeof(val), &vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, kl, unpack_chunk(val, vl, &out->base));
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, kl, efs_pack_chunk_identity(&out->base, ino, row.generation, 0));
    if (rc != EFS_OK) return rc;
    efs_hash(val, vl, out->source_digest);
    out->content_epoch = pack_row_epoch(val, vl, &out->base);
    out->ndelta = trailer_n(val, vl);
    if (out->ndelta > EFS_CHUNK_DELTA_MAX) return EFS_ERR_PROTO;
    for (uint32_t i = 0; i < out->ndelta; i++) {
        rc = efs_kv_decode_result(kv, key, kl, trailer_at(val, vl, i, &out->deltas[i]));
        if (rc != EFS_OK) return rc;
        out->deltas[i].len = sweep_visible(&hist, out->deltas[i].content_epoch, 0,
                                           out->deltas[i].off, out->deltas[i].len);
    }
    out->base_visible = out->base.generation ?
        sweep_visible(&hist, out->base.content_epoch, 0, 0,
                      out->base.base_clip ? out->base.base_clip : UINT32_MAX) : 0;
    return EFS_OK;
}

int efs_meta_apply_pack_unindex(struct efs_kv *kv, uint32_t group,
                                const struct efs_pack_unindex_item *items, uint32_t n)
{
    uint32_t anchor = efs_pack_group_anchor(group);
    if (!kv || !anchor || (n && !items) || n > EFS_PACK_UNINDEX_MAX) return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < n; i++)
        if (items[i].epoch == EFS_PACK_CURSOR_EPOCH) return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t key[EFS_PACK_CANDIDATE_KEY_BYTES], v[EFS_PACK_CANDIDATE_VAL_BYTES];
        uint32_t vl = sizeof(v);
        int rc = efs_pack_candidate_key(anchor, items[i].epoch, items[i].ino, key);
        if (rc == EFS_OK) rc = efs_kv_get(kv, key, sizeof(key), v, &vl);
        if (rc == EFS_ERR_NOT_FOUND) continue;
        /* A longer value is INVAL with its size in vl (kv.h). */
        if ((rc == EFS_OK || rc == EFS_ERR_INVAL) && vl != EFS_PACK_CANDIDATE_VAL_BYTES) rc = EFS_ERR_PROTO;
        if (rc != EFS_OK) return rc;
        /* A publication re-added the key after the packer read it. */
        if (rd64(v) != items[i].inode_generation || rd64(v + 8) != items[i].stamp) continue;
        rc = meta_write_del(kv, key, sizeof(key));
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    }
    return EFS_OK;
}

int efs_meta_apply_pack_index(struct efs_kv *kv, uint32_t group, uint32_t next_shard, int done,
                              const struct efs_pack_index_item *items, uint32_t n)
{
    uint32_t anchor = efs_pack_group_anchor(group);
    if (!kv || !anchor || (n && !items) || n > EFS_PACK_INDEX_MAX ||
        next_shard > EFS_KV_SHARD_MASK + 1u) return EFS_ERR_INVAL;
    struct efs_kv_item *it = calloc(n + 1, sizeof(*it));
    uint8_t (*keys)[EFS_PACK_CANDIDATE_KEY_BYTES] = calloc(n + 1, EFS_PACK_CANDIDATE_KEY_BYTES);
    uint8_t (*vals)[EFS_PACK_CANDIDATE_VAL_BYTES] = calloc(n + 1, EFS_PACK_CANDIDATE_VAL_BYTES);
    int rc = it && keys && vals ? EFS_OK : EFS_ERR_NOMEM;
    for (uint32_t i = 0; rc == EFS_OK && i < n; i++) {
        if (items[i].epoch == EFS_PACK_CURSOR_EPOCH || items[i].ino <= EFS_ROOT_INO) { rc = EFS_ERR_INVAL; break; }
        rc = efs_pack_candidate_key(anchor, items[i].epoch, items[i].ino, keys[i]);
        be64(vals[i], items[i].inode_generation); /* stamp 0: calloc */
        it[i] = (struct efs_kv_item){EFS_KV_PUT, keys[i], EFS_PACK_CANDIDATE_KEY_BYTES, vals[i],
                                     EFS_PACK_CANDIDATE_VAL_BYTES};
    }
    if (rc == EFS_OK) rc = efs_pack_candidate_key(anchor, EFS_PACK_CURSOR_EPOCH, 1, keys[n]);
    if (rc == EFS_OK) {
        efs_store_be32(vals[n], next_shard);
        vals[n][4] = done ? 1 : 0;
        it[n] = (struct efs_kv_item){EFS_KV_PUT, keys[n], EFS_PACK_CANDIDATE_KEY_BYTES, vals[n], 5};
        rc = meta_write_batch(kv, it, n + 1);
    }
    free(it); free(keys); free(vals);
    return rc;
}

int efs_meta_pack_backfill_cursor(struct efs_kv *kv, uint32_t group, uint32_t *next_shard, int *done)
{
    uint8_t key[EFS_PACK_CANDIDATE_KEY_BYTES], v[8];
    uint32_t vl = sizeof(v);
    uint32_t anchor = efs_pack_group_anchor(group);
    if (!kv || !anchor || !next_shard || !done) return EFS_ERR_INVAL;
    *next_shard = 0; *done = 0;
    int rc = efs_pack_candidate_key(anchor, EFS_PACK_CURSOR_EPOCH, 1, key);
    if (rc == EFS_OK) rc = efs_kv_get(kv, key, sizeof(key), v, &vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc != EFS_OK) return rc;
    if (vl != 5) return EFS_ERR_PROTO;
    *next_shard = efs_load_be32(v);
    *done = v[4] != 0;
    return EFS_OK;
}
