/* Durable PUT ownership, with bounded admission and explicit replay floors.
 * This module is staged until the data-plane/session coordinator is wired. */
#include "efs/put_ticket.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/publication.h"
#include "efs/session.h"
#include <string.h>

static void wr32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (24 - 8 * i));
}
static void wr64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}
static uint32_t rd32(const uint8_t *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        v = (v << 8) | p[i];
    return v;
}
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
static uint32_t ticket_shard(const struct efs_put_ticket *t)
{
    return efs_kv_lane_shard(t->ino, t->ci % EFS_META_LANES);
}
static int ticket_key(const struct efs_put_ticket *t, uint8_t key[51])
{
    struct efs_opid_req req = {0};
    if (!t || !t->ino || !t->inode_gen || !t->object || !t->id.session_epoch)
        return EFS_ERR_INVAL;
    if ((!t->delta_len && t->delta_off) ||
        (uint64_t)t->delta_off + t->delta_len > (uint64_t)t->fragment_len * 2)
        return EFS_ERR_INVAL;
    req.id = t->id;
    if (!t->member_mask || (t->member_mask & ~((1u << EFS_MAX_NODES) - 1u)) ||
        !t->fragment_len || t->fragment_len > EFS_MAX_CHUNK_SIZE / 2 ||
        __builtin_popcount(t->member_mask) < EFS_NUM_FRAGMENTS ||
        t->coding_profile != EFS_META_PROFILE_K2F1)
        return EFS_ERR_INVAL;
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!t->nodes[i])
            return EFS_ERR_INVAL;
        for (unsigned j = 0; j < i; j++)
            if (t->nodes[i] == t->nodes[j])
                return EFS_ERR_INVAL;
    }
    if (!efs_opid_req_valid(&req))
        return EFS_ERR_INVAL;
    if (t->object != efs_meta_candidate_gen(t->id.client_uuid, t->id.session_epoch,
                                            t->id.seq, t->ci, 0))
        return EFS_ERR_INVAL;
    uint32_t sh = ticket_shard(t);
    key[0] = (uint8_t)(sh >> 8);
    key[1] = (uint8_t)sh;
    key[2] = EFS_KV_KIND_PUT_TICKET;
    memcpy(key + 3, t->id.client_uuid, 16);
    wr32(key + 19, t->id.session_epoch);
    /* Sequence first: one bounded ordered stream per session/lane shard. */
    wr64(key + 23, t->id.seq);
    wr64(key + 31, t->ino);
    wr64(key + 39, t->inode_gen);
    wr32(key + 47, t->ci);
    return EFS_OK;
}
static void ticket_value(const struct efs_put_ticket *t, uint32_t state,
                         uint8_t val[EFS_PUT_TICKET_VAL_LEN])
{
    wr32(val, 1);
    wr32(val + 4, state);
    wr64(val + 8, t->object);
    memcpy(val + 16, t->body_digest, EFS_HASH_SIZE);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++)
        wr32(val + 48 + i * 4, t->nodes[i]);
    memcpy(val + 60, t->checksums, sizeof(t->checksums));
    wr32(val + 156, t->delta_off);
    wr32(val + 160, t->delta_len);
    wr32(val + 164, t->coding_profile);
    wr32(val + 168, t->fragment_len);
    wr32(val + 172, t->member_mask);
    wr32(val + 176, 0);
}
static int ticket_floor(struct efs_kv *kv, const uint8_t key[51], uint64_t *floor)
{
    uint8_t fk[23], val[12];
    uint32_t len = sizeof(val);
    memcpy(fk, key, sizeof(fk));
    fk[2] = EFS_KV_KIND_PUT_FLOOR;
    int rc = efs_kv_get(kv, fk, sizeof(fk), val, &len);
    if (rc == EFS_ERR_NOT_FOUND) {
        *floor = 0;
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (len != sizeof(val) || rd32(val) != 1 || !rd64(val + 4))
        return EFS_ERR_PROTO;
    *floor = rd64(val + 4);
    return EFS_OK;
}
int efs_put_ticket_from_pub(const struct efs_meta_pub *p, struct efs_put_ticket *t)
{
    if (!p || !t || !p->fresh_object || !p->put_fragment_len ||
        memcmp(p->publication_id.client_uuid, p->put_id.client_uuid,
               EFS_OPID_UUID_LEN) ||
        p->publication_id.session_epoch != p->put_id.session_epoch)
        return EFS_ERR_INVAL;
    struct efs_meta_pub body = *p;
    body.publication_id = p->put_id;
    body.ticketed = 0;
    /* Admission binds only immutable PUT bytes/geometry/placement. A CAS
     * retry may use another expected mapping without changing its body. */
    body.expected_gen = 0;
    body.new_size = 0;
    body.content_epoch = 0;
    body.delta_base_n = 0;
    body.delta_base_seq = 0;
    memset(t, 0, sizeof(*t));
    t->id = p->put_id;
    t->ino = p->ino;
    t->inode_gen = p->inode_gen;
    t->ci = p->chunk_index;
    t->object = p->candidate_gen;
    memcpy(t->nodes, p->ch.nodes, sizeof(t->nodes));
    memcpy(t->checksums, p->ch.checksums, sizeof(t->checksums));
    t->delta_off = p->delta_off;
    t->delta_len = p->delta_len;
    t->coding_profile = p->coding_profile_id;
    t->fragment_len = p->put_fragment_len;
    t->member_mask = p->put_member_mask;
    uint8_t input[EFS_HASH_SIZE + 8];
    int rc = efs_publication_digest(&body, input);
    if (rc != EFS_OK)
        return rc;
    wr32(input + EFS_HASH_SIZE, t->fragment_len);
    wr32(input + EFS_HASH_SIZE + 4, t->member_mask);
    efs_hash(input, sizeof(input), t->body_digest);
    return EFS_OK;
}
int efs_put_ticket_get(struct efs_kv *kv, const struct efs_put_ticket *t,
                       uint32_t *state)
{
    uint8_t key[51], val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t len = sizeof(val);
    uint64_t floor;
    if (!kv || !state)
        return EFS_ERR_INVAL;
    int rc = ticket_key(t, key);
    if (rc == EFS_OK)
        rc = ticket_floor(kv, key, &floor);
    if (rc != EFS_OK)
        return rc;
    if (t->id.seq <= floor)
        return EFS_ERR_STALE;
    rc = efs_kv_get(kv, key, sizeof(key), val, &len);
    if (rc != EFS_OK)
        return rc;
    if (len != sizeof(val) || rd32(val) != 1 || rd32(val + 4) < 1 ||
        rd32(val + 4) > 3 || rd64(val + 8) != t->object ||
        memcmp(val + 16, t->body_digest, EFS_HASH_SIZE))
        return EFS_ERR_PROTO;
    uint8_t expected[EFS_PUT_TICKET_VAL_LEN];
    ticket_value(t, rd32(val + 4), expected);
    if (memcmp(expected, val, 176) || (rd32(val + 176) & ~t->member_mask) ||
        (rd32(val + 4) != EFS_PUT_TICKET_RECLAIMING && rd32(val + 176)))
        return EFS_ERR_PROTO;
    *state = rd32(val + 4);
    return EFS_OK;
}
struct ticket_scan {
    unsigned count;
    uint64_t first;
    int rc;
};
static int scan_ticket(void *user, const uint8_t *key, uint32_t kl, const uint8_t *val,
                       uint32_t vl)
{
    struct ticket_scan *s = user;
    if (kl != 51 || !rd64(key + 23) || vl != EFS_PUT_TICKET_VAL_LEN || rd32(val) != 1 ||
        rd32(val + 4) < 1 || rd32(val + 4) > 3 || !rd64(val + 8)) {
        s->rc = EFS_ERR_PROTO;
        return 1;
    }
    if (!s->count)
        s->first = rd64(key + 23);
    return ++s->count >= EFS_PUT_TICKET_MAX + 1;
}
static int scan_stream(struct efs_kv *kv, const uint8_t key[51], struct ticket_scan *s)
{
    memset(s, 0, sizeof(*s));
    int rc = efs_kv_scan_prefix(kv, key, 23, scan_ticket, s);
    return rc == EFS_OK || rc == 1 ? s->rc : rc;
}
int efs_put_ticket_admit(struct efs_kv *kv, const struct efs_put_ticket *t)
{
    uint8_t key[51], val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t state;
    if (!kv)
        return EFS_ERR_INVAL;
    int rc = ticket_key(t, key);
    if (rc != EFS_OK)
        return rc;
    rc =
        efs_session_accept(kv, ticket_shard(t), t->id.client_uuid, t->id.session_epoch);
    if (rc != EFS_OK)
        return rc;
    rc = efs_put_ticket_get(kv, t, &state);
    if (rc == EFS_OK)
        return state == EFS_PUT_TICKET_ADMITTED ? EFS_OK : EFS_ERR_STALE;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    struct ticket_scan s;
    rc = scan_stream(kv, key, &s);
    if (rc != EFS_OK)
        return rc;
    if (s.count >= EFS_PUT_TICKET_MAX)
        return EFS_ERR_BUSY;
    /* A sequence cannot identify two different bodies/files in this lane.
     * Probe its 31-byte prefix before admitting a new key. */
    struct ticket_scan same = {0};
    rc = efs_kv_scan_prefix(kv, key, 31, scan_ticket, &same);
    if (rc != EFS_OK && rc != 1)
        return rc;
    if (same.rc)
        return same.rc;
    if (same.count)
        return EFS_ERR_PROTO;
    ticket_value(t, EFS_PUT_TICKET_ADMITTED, val);
    return efs_kv_put(kv, key, sizeof(key), val, sizeof(val));
}
int efs_put_ticket_publish_item(struct efs_kv *kv, const struct efs_put_ticket *t,
                                uint8_t key[51], uint8_t val[EFS_PUT_TICKET_VAL_LEN],
                                struct efs_kv_item *item)
{
    uint32_t state;
    if (!kv || !item || !key || !val)
        return EFS_ERR_INVAL;
    int rc = ticket_key(t, key);
    if (rc == EFS_OK)
        rc = efs_session_accept(kv, ticket_shard(t), t->id.client_uuid,
                                t->id.session_epoch);
    if (rc == EFS_OK)
        rc = efs_put_ticket_get(kv, t, &state);
    if (rc != EFS_OK)
        return rc;
    if (state != EFS_PUT_TICKET_ADMITTED)
        return EFS_ERR_STALE;
    ticket_value(t, EFS_PUT_TICKET_PUBLISHED, val);
    *item = (struct efs_kv_item){EFS_KV_PUT, key, 51, val, EFS_PUT_TICKET_VAL_LEN};
    return EFS_OK;
}
int efs_put_ticket_reclaim_begin(struct efs_kv *kv, const struct efs_put_ticket *t)
{
    uint8_t key[51], val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t state;
    if (!kv)
        return EFS_ERR_INVAL;
    int rc = ticket_key(t, key);
    if (rc == EFS_OK)
        rc = efs_session_reclaimable(kv, ticket_shard(t), t->id.client_uuid,
                                     t->id.session_epoch);
    if (rc == EFS_OK)
        rc = efs_put_ticket_get(kv, t, &state);
    if (rc != EFS_OK)
        return rc;
    if (state == EFS_PUT_TICKET_PUBLISHED)
        return EFS_ERR_BUSY;
    if (state == EFS_PUT_TICKET_RECLAIMING)
        return EFS_OK;
    ticket_value(t, EFS_PUT_TICKET_RECLAIMING, val);
    return efs_kv_put(kv, key, sizeof(key), val, sizeof(val));
}
/* Coordinator accepts a member ACK only after that member durably fences
 * old-epoch PUTs and confirms deletion. The ledger itself preserves every
 * missing member across leadership changes and restart. */
int efs_put_ticket_delete_ack(struct efs_kv *kv, const struct efs_put_ticket *t,
                              uint32_t member_bit)
{
    uint8_t key[51], val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t len = sizeof(val), state;
    if (!kv || !t || !member_bit || (member_bit & (member_bit - 1)) ||
        !(t->member_mask & member_bit))
        return EFS_ERR_INVAL;
    int rc = efs_put_ticket_get(kv, t, &state);
    if (rc != EFS_OK)
        return rc;
    if (state != EFS_PUT_TICKET_RECLAIMING)
        return EFS_ERR_BUSY;
    rc = ticket_key(t, key);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, key, sizeof(key), val, &len);
    if (rc != EFS_OK)
        return rc;
    uint32_t acked = rd32(val + 176);
    if (acked & member_bit)
        return EFS_OK;
    wr32(val + 176, acked | member_bit);
    return efs_kv_put(kv, key, sizeof(key), val, sizeof(val));
}
int efs_put_ticket_retire(struct efs_kv *kv, const struct efs_put_ticket *t)
{
    uint8_t key[51], fk[23], val[12];
    uint32_t state;
    uint64_t floor;
    if (!kv)
        return EFS_ERR_INVAL;
    int rc = ticket_key(t, key);
    if (rc == EFS_OK)
        rc = ticket_floor(kv, key, &floor);
    if (rc != EFS_OK)
        return rc;
    if (t->id.seq <= floor)
        return EFS_OK;
    rc = efs_put_ticket_get(kv, t, &state);
    if (rc != EFS_OK)
        return rc;
    if (state == EFS_PUT_TICKET_ADMITTED)
        return EFS_ERR_BUSY;
    if (state == EFS_PUT_TICKET_RECLAIMING) {
        uint8_t pending[EFS_PUT_TICKET_VAL_LEN];
        uint32_t len = sizeof(pending);
        rc = efs_kv_get(kv, key, sizeof(key), pending, &len);
        if (rc != EFS_OK)
            return rc;
        if (rd32(pending + 176) != t->member_mask)
            return EFS_ERR_BUSY;
    }
    struct ticket_scan s;
    rc = scan_stream(kv, key, &s);
    if (rc != EFS_OK)
        return rc;
    if (!s.count || s.first != t->id.seq)
        return EFS_ERR_BUSY;
    memcpy(fk, key, sizeof(fk));
    fk[2] = EFS_KV_KIND_PUT_FLOOR;
    wr32(val, 1);
    wr64(val + 4, t->id.seq);
    struct efs_kv_item items[2] = {{EFS_KV_DEL, key, 51, NULL, 0},
                                   {EFS_KV_PUT, fk, 23, val, sizeof(val)}};
    return efs_kv_batch(kv, items, 2);
}
