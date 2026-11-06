/* meta_apply/pub_stage.c: staged publication (W77) and its transaction
 * resolve and retire. */

#include "apply_internal.h"
#include "pub_stage.h"
#include "publish.h"

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
