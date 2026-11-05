#include "meta_apply/apply_internal.h"
#include "meta_apply/rows.h"
#include "meta_apply/lookup.h"
#include "meta_apply/init.h"
#include "meta_apply/xattr.h"
#include "meta_apply/namespace.h"
#include "meta_apply/gc.h"
#include "meta_apply/publish.h"
#include "meta_apply/lanes.h"
#include "meta_apply/sweep.h"
#include "meta_apply/attr.h"
#include "meta_apply/append.h"

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
