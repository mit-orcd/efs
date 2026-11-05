/* meta_apply/rows.c: the KV write checks and the row codecs (inode, lane,
 * dentry, fence history, lane authority, chunk). */

#include "apply_internal.h"
#include "rows.h"

/* All ordinary metadata mutations honor prepared EXCL images. REDUCEs
 * remain commutative. Chunk mutations share the lane's exclusion even when
 * a sweep deletes rows before writing/deleting the lane stamp. Checks and
 * the final write are serialized by the participant's Raft apply log. */
int pub_stage_owner(struct efs_kv *kv, const uint8_t *chunk_key,
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

int meta_write_check(struct efs_kv *kv, const uint8_t *key, uint32_t kl)
{
    return meta_write_check_as(kv, key, kl, NULL);
}

/* `dry` runs every check and writes nothing: a staged publication's PREPARE
 * proves the publish would apply now (W77). */
int meta_write_batch_as(struct efs_kv *kv, const struct efs_kv_item *items,
                               uint32_t count, const struct efs_txid *self, int dry)
{
    for (uint32_t i = 0; i < count; ++i) {
        int rc = meta_write_check_as(kv, items[i].key, items[i].klen, self);
        if (rc != EFS_OK)
            return rc;
    }
    return dry ? EFS_OK : efs_kv_batch(kv, items, count);
}

int meta_write_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                             uint32_t count)
{
    return meta_write_batch_as(kv, items, count, NULL, 0);
}

int meta_write_put(struct efs_kv *kv, const uint8_t *key, uint32_t kl,
                           const uint8_t *value, uint32_t vl)
{
    int rc = meta_write_check(kv, key, kl);
    return rc == EFS_OK ? efs_kv_put(kv, key, kl, value, vl) : rc;
}

int meta_write_del(struct efs_kv *kv, const uint8_t *key, uint32_t kl)
{
    int rc = meta_write_check(kv, key, kl);
    return rc == EFS_OK ? efs_kv_del(kv, key, kl) : rc;
}

/* Rows of an inode that no name references: the query catalog picks the
 * deletion up at its next rebuild instead of scheduling one (kv_lsm.h). */
int meta_cleanup_del(struct efs_kv *kv, const uint8_t *key, uint32_t kl)
{
    efs_kv_lsm_cleanup_scope(1);
    int rc = meta_write_del(kv, key, kl);
    efs_kv_lsm_cleanup_scope(0);
    return rc;
}
int meta_cleanup_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                              uint32_t count)
{
    efs_kv_lsm_cleanup_scope(1);
    int rc = meta_write_batch(kv, items, count);
    efs_kv_lsm_cleanup_scope(0);
    return rc;
}

void pack_lane(uint8_t *p, const struct lane_rec *l)
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
int unpack_lane(const uint8_t *p, uint32_t n, struct lane_rec *l)
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
int lane_adopt(struct lane_rec *ln, uint64_t epoch)
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
int dir_lane_stamp(struct efs_kv *kv, const struct efs_meta_row *dir,
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
int dseq_bump(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
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

void pack_inode(uint8_t *p, const struct efs_meta_row *r)
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

int unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r)
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

int content_fence_key(efs_ino_t ino, uint64_t gen, uint8_t authority,
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

int fence_history_read(struct efs_kv *kv, efs_ino_t ino,
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

uint64_t writer_history_floor(uint64_t epoch, const struct efs_fence_history *h)
{
    uint64_t floor = epoch;
    for (uint32_t i = h->count; i && h->entries[i - 1].epoch == floor; --i)
        --floor;
    return floor;
}

int lane_authority_key(efs_ino_t ino, uint64_t gen, uint8_t lane,
                               uint8_t *key, uint32_t *kl)
{
    if (!ino || !gen || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    int rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, kl);
    if (rc == EFS_OK)
        key[2] = EFS_KV_KIND_LANE_AUTHORITY;
    return rc;
}

void pack_lane_authority(uint8_t *v, uint32_t cs, uint64_t epoch, uint64_t floor)
{
    memset(v, 0, EFS_META_LANE_AUTHORITY_BYTES);
    efs_store_be32(v, 1); efs_store_be32(v + 4, cs); be64(v + 8, epoch); be64(v + 16, floor);
    efs_store_be32(v + 24, EFS_META_LANES);
}

int unpack_lane_authority(const uint8_t *v, uint32_t len, uint32_t *cs,
                                  uint64_t *epoch, uint64_t *floor)
{
    if (len != EFS_META_LANE_AUTHORITY_BYTES || efs_load_be32(v) != 1 ||
        !efs_chunk_size_valid(efs_load_be32(v + 4)) || efs_load_be32(v + 24) != EFS_META_LANES ||
        efs_load_be32(v + 28) || rd64(v + 16) > rd64(v + 8))
        return EFS_ERR_PROTO;
    *cs = efs_load_be32(v + 4); *epoch = rd64(v + 8); *floor = rd64(v + 16);
    return EFS_OK;
}

void pack_dentry(uint8_t *p, const struct efs_meta_dentry *d)
{
    be64(p + 0, d->ino);
    be64(p + 8, d->generation);
    efs_store_be32(p + 16, d->type);
}

int unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d)
{
    if (!p || !d || n < DENT_VAL)
        return EFS_ERR_PROTO;
    memset(d, 0, sizeof(*d));
    d->ino = rd64(p + 0);
    d->generation = rd64(p + 8);
    d->type = efs_load_be32(p + 16);
    return EFS_OK;
}

int kv_get_copy(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
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

uint32_t chunk_header_size(const uint8_t *p, uint32_t n)
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

int unpack_chunk(const uint8_t *p, uint32_t n, struct efs_meta_chunk *ch)
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
uint32_t trailer_n(const uint8_t *v, uint32_t n)
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

int trailer_at(const uint8_t *v, uint32_t n, uint32_t i,
                      struct efs_meta_delta *d)
{
    if (i >= trailer_n(v, n))
        return EFS_ERR_INVAL;
    return unpack_delta_rec(v + chunk_header_size(v, n) + DELTA_HDR + i * DELTA_REC, d);
}

int ranges_overlap(uint32_t a, uint32_t al, uint32_t b, uint32_t bl)
{
    if (al == 0 || bl == 0)
        return 0;
    return (uint64_t)a < (uint64_t)b + bl && (uint64_t)b < (uint64_t)a + al;
}

/* Write base image + n deltas. n == 0 keeps the historical short value
 * so a chunk with no deltas stays CHUNK_VAL bytes. */
uint32_t pack_chunk_value(uint8_t *dst, const struct efs_meta_chunk *ch,
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
int chunk_aliases(const struct efs_meta_chunk *a,
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
