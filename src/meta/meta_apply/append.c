/* meta_apply/append.c: APPEND reservations and completion, lazy truncate,
 * session drain. */

#include "apply_internal.h"
#include "append.h"
#include "rows.h"
#include "lookup.h"
#include "init.h"
#include "lanes.h"

/* Defined in ../meta_apply.c until its area is carved out. */
int lane_seq_get(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                 uint8_t lane, uint64_t *seq);
int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino, uint64_t generation,
                    uint64_t bits,
                    int (*upd)(struct lane_rec *ln, void *arg),
                    void *arg, struct efs_kv_item *it, uint32_t *n,
                    uint8_t k_ln[][EFS_KV_KEY_MAX],
                    uint8_t v_ln[][LANE_VAL]);

#define APPEND_CUR_VAL 24
#define APPEND_RSV_VAL 48
#define APPEND_OPEN 0
#define APPEND_DONE 1

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

int load_append_cur(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
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

int trunc_pending_key(efs_ino_t ino, uint8_t *key, uint32_t *kl)
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
