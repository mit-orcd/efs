#include "meta_apply/apply_internal.h"
#include "meta_apply/rows.h"
#include "meta_apply/lookup.h"
#include "meta_apply/init.h"
#include "meta_apply/xattr.h"
#include "meta_apply/namespace.h"
#include "meta_apply/gc.h"
#include "meta_apply/lanes.h"
#include "meta_apply/sweep.h"
#include "meta_apply/append.h"

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

int pub_stage_owner(struct efs_kv *kv, const uint8_t *chunk_key,
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
        efs_telemetry_log(0, "meta: reaped a publication stage on contact: owner "
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

/* Reads a lane's sequence number. Absent is seq 0, which is a real value:
 * a lane with no record has nothing to contribute and nothing to change. */
int lane_seq_get(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
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
int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino, uint64_t generation,
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
