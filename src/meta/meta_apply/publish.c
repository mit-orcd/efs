/* meta_apply/publish.c: chunk publication: receipts, admit, retire, the
 * publish CAS. */

#include "apply_internal.h"
#include "publish.h"
#include "rows.h"
#include "gc.h"
#include "append.h"

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

int publication_receipt_valid(const uint8_t *v,uint32_t n)
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

int publish_inner_as(struct efs_kv *kv, const struct efs_meta_pub *p,
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
