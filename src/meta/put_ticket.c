/* Durable PUT ownership, with bounded admission and explicit replay floors.
 * Every data PUT and REPORT record is bound to one of these tickets (K13). */
#include "efs/put_ticket.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/publication.h"
#include "efs/session.h"
#include "efs/txn.h"
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
static int state_stored(uint32_t state)
{
    return state == EFS_PUT_TICKET_ADMITTED || state == EFS_PUT_TICKET_PUBLISHED ||
           state == EFS_PUT_TICKET_RECLAIMING || state == EFS_PUT_TICKET_REJECTED;
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
int efs_put_ticket_encode(const struct efs_put_ticket *t,
                           uint8_t out[EFS_PUT_TICKET_WIRE_LEN])
{
    uint8_t key[EFS_PUT_TICKET_KEY_LEN];
    if(!out)return EFS_ERR_INVAL;
    int rc=ticket_key(t,key);if(rc!=EFS_OK)return rc;
    memcpy(out,t->id.client_uuid,16);wr32(out+16,t->id.session_epoch);
    wr64(out+20,t->id.seq);wr64(out+28,t->ino);wr64(out+36,t->inode_gen);
    wr32(out+44,t->ci);wr64(out+48,t->object);memcpy(out+56,t->body_digest,32);
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)wr32(out+88+4*i,t->nodes[i]);
    memcpy(out+100,t->checksums,sizeof(t->checksums));
    wr32(out+196,t->delta_off);wr32(out+200,t->delta_len);
    wr32(out+204,t->coding_profile);wr32(out+208,t->fragment_len);
    wr32(out+212,t->member_mask);return EFS_OK;
}
int efs_put_ticket_decode(const uint8_t *in,uint32_t bytes,struct efs_put_ticket *out)
{
    if(!in || !out || bytes!=EFS_PUT_TICKET_WIRE_LEN)return EFS_ERR_INVAL;
    struct efs_put_ticket t={0};uint8_t key[EFS_PUT_TICKET_KEY_LEN];
    memcpy(t.id.client_uuid,in,16);t.id.session_epoch=rd32(in+16);t.id.seq=rd64(in+20);
    t.ino=rd64(in+28);t.inode_gen=rd64(in+36);t.ci=rd32(in+44);
    t.object=rd64(in+48);memcpy(t.body_digest,in+56,32);
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)t.nodes[i]=rd32(in+88+4*i);
    memcpy(t.checksums,in+100,sizeof(t.checksums));
    t.delta_off=rd32(in+196);t.delta_len=rd32(in+200);t.coding_profile=rd32(in+204);
    t.fragment_len=rd32(in+208);t.member_mask=rd32(in+212);
    int rc=ticket_key(&t,key);if(rc!=EFS_OK)return rc;
    *out=t;return EFS_OK;
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
    if (len != sizeof(val) || rd32(val) != 1 || !state_stored(rd32(val + 4)) ||
        rd64(val + 8) != t->object ||
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
        !state_stored(rd32(val + 4)) || !rd64(val + 8)) {
        s->rc = EFS_ERR_PROTO;
        return 1;
    }
    if (!s->count)
        s->first = rd64(key + 23);
    s->count++;
    return 1; /* sequence-ordered: the first key is the stream head */
}
/* Head of one session/lane-shard stream. */
static int stream_head(struct efs_kv *kv, const uint8_t key[51], struct ticket_scan *s)
{
    memset(s, 0, sizeof(*s));
    int rc = efs_kv_scan_prefix(kv, key, 23, scan_ticket, s);
    return rc == EFS_OK || rc == 1 ? s->rc : rc;
}
/* Live-ticket count per stream: the admission bound without a full scan. */
static void count_key(const uint8_t key[51], uint8_t ck[23])
{
    memcpy(ck, key, 23);
    ck[2] = EFS_KV_KIND_PUT_COUNT;
}
static int stream_count(struct efs_kv *kv, const uint8_t key[51], uint32_t *count)
{
    uint8_t ck[23], val[8];
    uint32_t len = sizeof(val);
    count_key(key, ck);
    int rc = efs_kv_get(kv, ck, sizeof(ck), val, &len);
    if (rc == EFS_ERR_NOT_FOUND) {
        *count = 0;
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (len != sizeof(val) || rd32(val) != 1 || !rd32(val + 4) ||
        rd32(val + 4) > EFS_PUT_TICKET_MAX)
        return EFS_ERR_PROTO;
    *count = rd32(val + 4);
    return EFS_OK;
}
/* Batch item for a new count; zero deletes the key. */
static void count_item(const uint8_t key[51], uint32_t count, uint8_t ck[23], uint8_t cv[8],
                       struct efs_kv_item *item)
{
    count_key(key, ck);
    wr32(cv, 1);
    wr32(cv + 4, count);
    *item = count ? (struct efs_kv_item){EFS_KV_PUT, ck, 23, cv, 8}
                  : (struct efs_kv_item){EFS_KV_DEL, ck, 23, NULL, 0};
}
/* A new stream entry in state ADMITTED (admission) or REJECTED (settle of a
 * ticket whose admission never applied: a late admission then finds it). */
static int insert_ticket(struct efs_kv *kv, const struct efs_put_ticket *t,
                         const uint8_t key[51], uint32_t state)
{
    uint8_t val[EFS_PUT_TICKET_VAL_LEN], ck[23], cv[8];
    uint32_t live;
    int rc = stream_count(kv, key, &live);
    if (rc != EFS_OK)
        return rc;
    if (live >= EFS_PUT_TICKET_MAX)
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
    ticket_value(t, state, val);
    struct efs_kv_item items[2] = {{EFS_KV_PUT, key, 51, val, sizeof(val)}};
    count_item(key, live + 1, ck, cv, &items[1]);
    return efs_kv_batch(kv, items, 2);
}
int efs_put_ticket_admit(struct efs_kv *kv, const struct efs_put_ticket *t)
{
    uint8_t key[51];
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
    return insert_ticket(kv, t, key, EFS_PUT_TICKET_ADMITTED);
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
/* D31: the orphan record key, on the anchor shard of the ticket's group. */
static int orphan_key(const struct efs_put_ticket *t, uint8_t ok[EFS_PUT_ORPHAN_KEY_LEN])
{
    uint8_t key[51];
    int rc = ticket_key(t, key);
    if (rc != EFS_OK)
        return rc;
    uint32_t anchor = efs_kv_anchor_shard(ticket_shard(t));
    ok[0] = (uint8_t)(anchor >> 8);
    ok[1] = (uint8_t)anchor;
    ok[2] = EFS_KV_KIND_PUT_ORPHAN;
    ok[3] = key[0];
    ok[4] = key[1];
    memcpy(ok + 5, key + 3, 48);
    return EFS_OK;
}
/* Retire the stream head. REJECTED moves its body into the orphan ledger in
 * the same batch; RECLAIMING requires every captured member's deletion ACK. */
static int retire_head(struct efs_kv *kv, const struct efs_put_ticket *t,
                       const uint8_t key[51], uint32_t state)
{
    uint8_t fk[23], val[12], ok[EFS_PUT_ORPHAN_KEY_LEN], ov[EFS_PUT_TICKET_VAL_LEN], ck[23], cv[8];
    uint32_t live;
    struct ticket_scan s;
    int rc = stream_head(kv, key, &s);
    if (rc != EFS_OK)
        return rc;
    if (!s.count || s.first != t->id.seq)
        return EFS_ERR_BUSY;
    rc = stream_count(kv, key, &live);
    if (rc != EFS_OK)
        return rc;
    if (!live)
        return EFS_ERR_PROTO;
    memcpy(fk, key, sizeof(fk));
    fk[2] = EFS_KV_KIND_PUT_FLOOR;
    wr32(val, 1);
    wr64(val + 4, t->id.seq);
    struct efs_kv_item items[4] = {{EFS_KV_DEL, key, 51, NULL, 0},
                                   {EFS_KV_PUT, fk, 23, val, sizeof(val)}};
    unsigned n = 2;
    count_item(key, live - 1, ck, cv, &items[n++]);
    if (state == EFS_PUT_TICKET_REJECTED) {
        rc = orphan_key(t, ok);
        if (rc != EFS_OK)
            return rc;
        ticket_value(t, EFS_PUT_TICKET_REJECTED, ov);
        items[n++] = (struct efs_kv_item){EFS_KV_PUT, ok, sizeof(ok), ov, sizeof(ov)};
    }
    return efs_kv_batch(kv, items, n);
}
int efs_put_ticket_retire(struct efs_kv *kv, const struct efs_put_ticket *t)
{
    uint8_t key[51];
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
    return retire_head(kv, t, key, state);
}

int efs_put_ticket_retired(struct efs_kv *kv,const struct efs_put_ticket *t,int *out)
{
    if (!kv || !out) return EFS_ERR_INVAL;
    *out=0;
    uint8_t key[EFS_PUT_TICKET_KEY_LEN];uint64_t floor;
    int rc=ticket_key(t,key);
    if (rc==EFS_OK)rc=ticket_floor(kv,key,&floor);
    if (rc==EFS_OK)*out=t->id.seq<=floor;
    return rc;
}

int efs_put_ticket_state(struct efs_kv *kv,const struct efs_put_ticket *t,uint32_t *state)
{
    if (!state) return EFS_ERR_INVAL;
    *state=0;
    int retired;
    int rc=efs_put_ticket_retired(kv,t,&retired);
    if (rc!=EFS_OK) return rc;
    if (retired) { *state=EFS_PUT_TICKET_RETIRED; return EFS_OK; }
    return efs_put_ticket_get(kv,t,state);
}

int efs_put_ticket_reject(struct efs_kv *kv,const struct efs_put_ticket *t)
{
    uint8_t key[51],val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t state;
    if (!kv) return EFS_ERR_INVAL;
    int rc=ticket_key(t,key);
    if (rc==EFS_OK) rc=efs_put_ticket_get(kv,t,&state);
    if (rc!=EFS_OK) return rc;
    if (state==EFS_PUT_TICKET_REJECTED) return EFS_OK;
    if (state==EFS_PUT_TICKET_PUBLISHED) return EFS_ERR_STALE;
    if (state!=EFS_PUT_TICKET_ADMITTED) return EFS_ERR_BUSY;
    ticket_value(t,EFS_PUT_TICKET_REJECTED,val);
    return efs_kv_put(kv,key,sizeof(key),val,sizeof(val));
}

int efs_put_ticket_settle(struct efs_kv *kv,const struct efs_put_ticket *t)
{
    uint8_t key[51];
    uint32_t state;
    if (!kv) return EFS_ERR_INVAL;
    int rc=ticket_key(t,key);
    if (rc==EFS_OK) rc=efs_session_accept(kv,ticket_shard(t),t->id.client_uuid,t->id.session_epoch);
    if (rc==EFS_OK) rc=efs_put_ticket_state(kv,t,&state);
    if (rc==EFS_ERR_NOT_FOUND) {
        /* The owner gave up an admission whose apply it never saw. */
        rc=insert_ticket(kv,t,key,EFS_PUT_TICKET_REJECTED);
        if (rc!=EFS_OK) return rc;
        state=EFS_PUT_TICKET_REJECTED;
    }
    if (rc!=EFS_OK) return rc;
    if (state==EFS_PUT_TICKET_RETIRED) return EFS_OK;
    if (state==EFS_PUT_TICKET_RECLAIMING) return EFS_ERR_BUSY;
    if (state==EFS_PUT_TICKET_ADMITTED) {
        /* A staged multi-chunk publication (K14) may still publish this
         * object at RESOLVE; never abandon under it. Settle later. */
        struct efs_txid tx;struct efs_txn_parts parts;
        int staged=efs_meta_pub_stage_peek(kv,t->ino,t->inode_gen,t->ci,&tx,&parts);
        if (staged==EFS_OK) return EFS_ERR_BUSY;
        if (staged!=EFS_ERR_NOT_FOUND) return staged;
        rc=efs_put_ticket_reject(kv,t);
        if (rc!=EFS_OK) return rc;
        state=EFS_PUT_TICKET_REJECTED;
    }
    return retire_head(kv,t,key,state);
}

/* The stored record must be exactly this ticket's REJECTED value plus a
 * proper subset of its fragments acknowledged. */
static int orphan_value_ok(const struct efs_put_ticket *t,const uint8_t *val,uint32_t len,
                           uint32_t *acked)
{
    uint8_t want[EFS_PUT_TICKET_VAL_LEN];
    if (len != EFS_PUT_TICKET_VAL_LEN) return EFS_ERR_PROTO;
    ticket_value(t,EFS_PUT_TICKET_REJECTED,want);
    uint32_t mask=rd32(val+176);
    if (memcmp(val,want,176) || mask>=EFS_PUT_ORPHAN_ALL) return EFS_ERR_PROTO;
    if (acked) *acked=mask;
    return EFS_OK;
}

int efs_put_ticket_orphan_get(struct efs_kv *kv,const struct efs_put_ticket *t,uint32_t *acked)
{
    uint8_t key[EFS_PUT_ORPHAN_KEY_LEN],val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t len=sizeof(val);
    if (!kv) return EFS_ERR_INVAL;
    int rc=orphan_key(t,key);
    if (rc==EFS_OK) rc=efs_kv_get(kv,key,sizeof(key),val,&len);
    return rc==EFS_OK?orphan_value_ok(t,val,len,acked):rc;
}

int efs_put_ticket_orphan(struct efs_kv *kv,const struct efs_put_ticket *t)
{
    return efs_put_ticket_orphan_get(kv,t,NULL);
}

int efs_put_ticket_orphan_ack(struct efs_kv *kv,const struct efs_put_ticket *t,uint32_t frags)
{
    uint8_t key[EFS_PUT_ORPHAN_KEY_LEN],val[EFS_PUT_TICKET_VAL_LEN];
    uint32_t len=sizeof(val),acked;
    if (!kv || !frags || (frags & ~EFS_PUT_ORPHAN_ALL)) return EFS_ERR_INVAL;
    int rc=orphan_key(t,key);
    /* Body deletion is acknowledged only behind this shard's durable
     * rejection floor: no ADMIT, PUBLISH or SETTLE of the epoch applies. */
    if (rc==EFS_OK) rc=efs_session_local_revoked(kv,ticket_shard(t),t->id.client_uuid,
                                                t->id.session_epoch);
    if (rc==EFS_OK) rc=efs_kv_get(kv,key,sizeof(key),val,&len);
    if (rc==EFS_ERR_NOT_FOUND) {
        /* Already collected (a retried acknowledgement), or never orphaned. */
        int retired=0;
        rc=efs_put_ticket_retired(kv,t,&retired);
        return rc!=EFS_OK?rc:retired?EFS_OK:EFS_ERR_NOT_FOUND;
    }
    if (rc==EFS_OK) rc=orphan_value_ok(t,val,len,&acked);
    if (rc!=EFS_OK) return rc;
    acked|=frags;
    if (acked==EFS_PUT_ORPHAN_ALL) return efs_kv_del(kv,key,sizeof(key));
    if (acked==rd32(val+176)) return EFS_OK;
    wr32(val+176,acked);
    return efs_kv_put(kv,key,sizeof(key),val,sizeof(val));
}

void efs_put_ticket_orphan_prefix(uint32_t anchor,uint8_t prefix[3])
{
    prefix[0]=(uint8_t)(anchor>>8);prefix[1]=(uint8_t)anchor;prefix[2]=EFS_KV_KIND_PUT_ORPHAN;
}

int efs_put_ticket_orphan_decode(const uint8_t *key,uint32_t kl,const uint8_t *val,
                                 uint32_t vl,struct efs_put_ticket *out,uint32_t *acked)
{
    if (!key || !val || !out) return EFS_ERR_INVAL;
    if (kl!=EFS_PUT_ORPHAN_KEY_LEN || key[2]!=EFS_KV_KIND_PUT_ORPHAN ||
        vl!=EFS_PUT_TICKET_VAL_LEN || rd32(val)!=1 || rd32(val+4)!=EFS_PUT_TICKET_REJECTED)
        return EFS_ERR_PROTO;
    struct efs_put_ticket t={0};
    memcpy(t.id.client_uuid,key+5,16);t.id.session_epoch=rd32(key+21);t.id.seq=rd64(key+25);
    t.ino=rd64(key+33);t.inode_gen=rd64(key+41);t.ci=rd32(key+49);
    t.object=rd64(val+8);memcpy(t.body_digest,val+16,EFS_HASH_SIZE);
    for (unsigned i=0;i<EFS_NUM_FRAGMENTS;i++) t.nodes[i]=rd32(val+48+4*i);
    memcpy(t.checksums,val+60,sizeof(t.checksums));
    t.delta_off=rd32(val+156);t.delta_len=rd32(val+160);t.coding_profile=rd32(val+164);
    t.fragment_len=rd32(val+168);t.member_mask=rd32(val+172);
    uint8_t want[EFS_PUT_ORPHAN_KEY_LEN];
    /* The key must be the one this ticket derives (anchor and lane shard). */
    if (orphan_key(&t,want)!=EFS_OK || memcmp(want,key,sizeof(want)))
        return EFS_ERR_PROTO;
    int rc=orphan_value_ok(&t,val,vl,acked);
    if (rc==EFS_OK) *out=t;
    return rc;
}
