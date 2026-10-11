#include "efs/pack_admission.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/checksum.h"
#include "efs/raft.h"
#include "efs/session.h"
#include "efs/txn.h"
#include <string.h>
static void put(uint8_t *p, uint64_t n, unsigned b)
{ while (b) { p[--b] = (uint8_t)n; n >>= 8; } }
static uint64_t get(const uint8_t *p, unsigned b)
{ uint64_t n = 0; while (b--) n = (n << 8) | *p++; return n; }
static uint32_t anchor(uint32_t group)
{ return group == EFS_RAFT_GROUP_SHARD ? 1 : group == EFS_RAFT_GROUP_SHARD2 ? 2 : 0; }
int efs_pack_identity(const struct efs_opid *owner, uint32_t group, uint64_t *id, uint64_t *generation)
{
    struct efs_opid_req req = {0}; if (!owner || !owner->session_epoch || !id || !generation || !anchor(group)) return EFS_ERR_INVAL;
    req.id = *owner; if (!efs_opid_req_valid(&req)) return EFS_ERR_INVAL;
    uint8_t bytes[44] = {0}, digest[EFS_HASH_SIZE];
    memcpy(bytes, "EFSPID01", 8); memcpy(bytes + 8, owner->client_uuid, 16);
    put(bytes + 24, owner->session_epoch, 4); put(bytes + 28, owner->seq, 8); put(bytes + 36, group, 4);
    efs_hash(bytes, sizeof(bytes), digest);
    *id = get(digest, 8); *generation = get(digest + 8, 8);
    if (!*id) *id = 1;
    if (!*generation) *generation = 1;
    return EFS_OK;
}
/* Normalize only mutable lifetime fields. The stream binds every placement,
 * checksum, reference slot, captured member, body digest and owner. */
static int manifest(const struct efs_pack_object *o, uint8_t digest[EFS_HASH_SIZE])
{
    if (!o) return EFS_ERR_INVAL;
    struct efs_pack_object copy = *o;
    copy.ledger.phase = EFS_PACK_ACTIVE; copy.ledger.fence_acks = copy.ledger.delete_acks = 0;
    if (copy.ledger.count > EFS_PACK_MAX_RECORDS) return EFS_ERR_INVAL;
    memset(copy.ledger.state, EFS_PACK_PENDING, copy.ledger.count);
    uint8_t bytes[EFS_PACK_OBJECT_MAX_BYTES]; size_t len;
    int rc = efs_pack_object_encode(&copy, bytes, sizeof(bytes), &len);
    if (rc != EFS_OK) return rc;
    uint64_t id, gen; rc = efs_pack_identity(&o->ledger.owner, o->ledger.publication_group, &id, &gen);
    if (rc != EFS_OK || id != o->ledger.segment_id || gen != o->ledger.segment_generation) return EFS_ERR_INVAL;
    efs_hash(bytes, len, digest); return EFS_OK;
}
int efs_pack_admission_validate(const struct efs_pack_object *o, const struct efs_pack_ref *refs, uint32_t count)
{
    uint8_t digest[EFS_HASH_SIZE]; int rc = manifest(o, digest);
    if (rc != EFS_OK || !refs || !count || count != o->ledger.count ||
        o->ledger.phase != EFS_PACK_ACTIVE || o->ledger.fence_acks || o->ledger.delete_acks) return EFS_ERR_INVAL;
    uint64_t offset = EFS_PACK_HEADER_BYTES + count * EFS_PACK_RECORD_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        const struct efs_pack_ref *r = refs + i; uint8_t wire[EFS_PACK_REF_BYTES];
        if (o->ledger.state[i] != EFS_PACK_PENDING || efs_pack_ref_encode(r, wire) ||
            r->segment_id != o->ledger.segment_id || r->segment_generation != o->ledger.segment_generation ||
            r->geometry != o->ledger.geometry || r->publication_group != o->ledger.publication_group ||
            r->offset != offset || efs_raft_shard_group(efs_kv_lane_shard(r->ino, 0)) != r->publication_group) return EFS_ERR_INVAL;
        for (uint32_t j = 0; j < i; j++) if (refs[j].ino == r->ino) return EFS_ERR_INVAL;
        efs_hash(wire, sizeof(wire), digest);
        if (memcmp(digest, o->ledger.reference_digest[i], sizeof(digest))) return EFS_ERR_INVAL;
        offset += r->len;
    }
    size_t geometry; if (efs_segment_geometry(offset + EFS_HASH_SIZE, &geometry) || geometry != o->ledger.geometry) return EFS_ERR_INVAL;
    return EFS_OK;
}
int efs_pack_upload_encode(const struct efs_pack_object *o, const struct efs_pack_ref *refs,
    uint32_t count, uint8_t *out, size_t capacity, size_t *bytes)
{
    if (!out || !bytes) return EFS_ERR_INVAL;
    int rc = efs_pack_admission_validate(o, refs, count); if (rc) return rc;
    uint8_t image[EFS_PACK_UPLOAD_MAX]; size_t len;
    rc = efs_pack_object_encode(o, image + EFS_PACK_UPLOAD_HEADER, EFS_PACK_OBJECT_MAX_BYTES, &len);
    if (rc) return rc;
    size_t total = EFS_PACK_UPLOAD_HEADER + len + count * EFS_PACK_REF_BYTES;
    if (capacity < total) return EFS_ERR_OVERFLOW;
    memcpy(image, "EFSUPLD1", 8); put(image + 8, len, 4); put(image + 12, count, 4);
    for (uint32_t i = 0; i < count; i++) {
        rc = efs_pack_ref_encode(refs + i, image + EFS_PACK_UPLOAD_HEADER + len + i * EFS_PACK_REF_BYTES);
        if (rc) return rc;
    }
    memcpy(out, image, total); *bytes = total; return EFS_OK;
}
int efs_pack_upload_decode(const uint8_t *in, size_t bytes, struct efs_pack_object *o,
    struct efs_pack_ref *refs, uint32_t capacity, uint32_t *count)
{
    if (!in || !o || !refs || !count || bytes < EFS_PACK_UPLOAD_HEADER ||
        bytes > EFS_PACK_UPLOAD_MAX || memcmp(in, "EFSUPLD1", 8)) return EFS_ERR_PROTO;
    uint32_t len = get(in + 8, 4), n = get(in + 12, 4);
    if (!n || n > EFS_PACK_MAX_RECORDS || n > capacity || len > EFS_PACK_OBJECT_MAX_BYTES ||
        bytes != EFS_PACK_UPLOAD_HEADER + (size_t)len + (size_t)n * EFS_PACK_REF_BYTES) return EFS_ERR_PROTO;
    struct efs_pack_object object; struct efs_pack_ref records[EFS_PACK_MAX_RECORDS];
    int rc = efs_pack_object_decode(in + EFS_PACK_UPLOAD_HEADER, len, &object); if (rc) return rc;
    for (uint32_t i = 0; i < n; i++) {
        rc = efs_pack_ref_decode(in + EFS_PACK_UPLOAD_HEADER + len + i * EFS_PACK_REF_BYTES, EFS_PACK_REF_BYTES, records + i);
        if (rc) return rc;
    }
    rc = efs_pack_admission_validate(&object, records, n); if (rc) return EFS_ERR_PROTO;
    *o = object; memcpy(refs, records, n * sizeof(*refs)); *count = n; return EFS_OK;
}
static int keys(const struct efs_pack_object *o, uint8_t object[19], uint8_t stream[31], uint8_t floor[23])
{
    uint8_t digest[EFS_HASH_SIZE]; int rc = manifest(o, digest); if (rc) return rc;
    rc = efs_pack_object_key(o->ledger.publication_group, o->ledger.segment_id, o->ledger.segment_generation, object);
    if (rc) return rc;
    put(stream, anchor(o->ledger.publication_group), 2); stream[2] = EFS_KV_KIND_PACK_STREAM;
    memcpy(stream + 3, o->ledger.owner.client_uuid, 16); put(stream + 19, o->ledger.owner.session_epoch, 4);
    put(stream + 23, o->ledger.owner.seq, 8); memcpy(floor, stream, 23); floor[2] = EFS_KV_KIND_PACK_FLOOR;
    return EFS_OK;
}
static int floor_get(struct efs_kv *kv, const uint8_t key[23], uint64_t *floor)
{
    uint8_t value[12]; uint32_t len = sizeof(value); int rc = efs_kv_get(kv, key, 23, value, &len);
    *floor = 0; if (rc == EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc) return rc;
    if (len != sizeof(value) || get(value, 4) != 1 || !get(value + 4, 8)) return EFS_ERR_PROTO;
    *floor = get(value + 4, 8); return EFS_OK;
}
int efs_pack_admission_get(struct efs_kv *kv, const struct efs_pack_object *request, struct efs_pack_object *out)
{
    uint8_t object[19], stream[31], floor[23], value[48], digest[EFS_HASH_SIZE]; uint64_t f;
    if (!kv || !out) return EFS_ERR_INVAL;
    int rc = keys(request, object, stream, floor); if (rc) return rc;
    rc = floor_get(kv, floor, &f); if (rc) return rc;
    if (request->ledger.owner.seq <= f) return EFS_ERR_STALE;
    uint32_t len = sizeof(value); rc = efs_kv_get(kv, stream, sizeof(stream), value, &len); if (rc) return rc;
    rc = manifest(request, digest); if (rc) return rc;
    if (len != sizeof(value) || get(value, 8) != request->ledger.segment_id ||
        get(value + 8, 8) != request->ledger.segment_generation || memcmp(value + 16, digest, sizeof(digest))) return EFS_ERR_PROTO;
    struct efs_pack_object actual;
    rc = efs_pack_object_get(kv, request->ledger.publication_group, request->ledger.segment_id, request->ledger.segment_generation, &actual);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_PROTO;
    if (rc) return rc;
    rc = manifest(&actual, digest); if (rc || memcmp(value + 16, digest, sizeof(digest))) return EFS_ERR_PROTO;
    *out = actual; return EFS_OK;
}
struct scan { uint32_t count; uint64_t first; int rc; };
static int scan_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct scan *s = u;
    if (kl != 31 || !get(k + 23, 8) || vl != 48 || !get(v, 8) || !get(v + 8, 8)) { s->rc = EFS_ERR_PROTO; return 1; }
    if (!s->count) s->first = get(k + 23, 8);
    return ++s->count > EFS_PACK_ADMISSION_MAX;
}
static int scan_stream(struct efs_kv *kv, const uint8_t key[31], struct scan *s)
{
    memset(s, 0, sizeof(*s)); int rc = efs_kv_scan_prefix(kv, key, 23, scan_cb, s);
    return rc == EFS_OK || rc == 1 ? s->rc : rc;
}
static int unlocked(struct efs_kv *kv, const uint8_t *key, uint32_t len)
{ return efs_txn_key_exclusive(kv, key, len); }
int efs_pack_admit(struct efs_kv *kv, const struct efs_pack_object *o, const struct efs_pack_ref *refs, uint32_t count)
{
    if (!kv) return EFS_ERR_INVAL;
    int rc = efs_pack_admission_validate(o, refs, count); if (rc) return rc;
    rc = efs_session_accept(kv, anchor(o->ledger.publication_group), o->ledger.owner.client_uuid, o->ledger.owner.session_epoch); if (rc) return rc;
    struct efs_pack_object current; rc = efs_pack_admission_get(kv, o, &current);
    if (!rc) {
        if (current.ledger.phase != EFS_PACK_ACTIVE) return EFS_ERR_STALE;
        for (uint32_t i = 0; i < count; i++) if (current.ledger.state[i] > EFS_PACK_LIVE) return EFS_ERR_STALE;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND) return rc;
    uint8_t object[19], stream[31], floor[23]; rc = keys(o, object, stream, floor); if (rc) return rc;
    struct scan scan; rc = scan_stream(kv, stream, &scan); if (rc) return rc;
    if (scan.count >= EFS_PACK_ADMISSION_MAX) return EFS_ERR_BUSY;
    rc = efs_pack_object_get(kv, o->ledger.publication_group, o->ledger.segment_id, o->ledger.segment_generation, &current);
    if (!rc) return EFS_ERR_PROTO;
    if (rc != EFS_ERR_NOT_FOUND) return rc;
    rc = unlocked(kv, object, sizeof(object)); if (rc) return rc;
    rc = unlocked(kv, stream, sizeof(stream)); if (rc) return rc;
    rc = unlocked(kv, floor, sizeof(floor)); if (rc) return rc;
    rc = efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V1); if (rc) return rc;
    uint8_t bytes[EFS_PACK_OBJECT_MAX_BYTES], permit[48], capkey[3], capval[4] = {0, 0, 0, EFS_KV_READER_PACK_V1}; size_t len;
    rc = efs_pack_object_encode(o, bytes, sizeof(bytes), &len); if (rc) return rc;
    put(permit, o->ledger.segment_id, 8); put(permit + 8, o->ledger.segment_generation, 8); rc = manifest(o, permit + 16); if (rc) return rc;
    memcpy(capkey, object, 2); capkey[2] = EFS_KV_KIND_PACK_READER;
    rc = unlocked(kv, capkey, sizeof(capkey)); if (rc) return rc;
    struct efs_kv_item items[] = {{EFS_KV_PUT, object, sizeof(object), bytes, len},
        {EFS_KV_PUT, stream, sizeof(stream), permit, sizeof(permit)}, {EFS_KV_PUT, capkey, sizeof(capkey), capval, sizeof(capval)}};
    return efs_kv_batch(kv, items, 3);
}
static int save(struct efs_kv *kv, const struct efs_pack_object *o)
{
    uint8_t key[19], bytes[EFS_PACK_OBJECT_MAX_BYTES]; size_t len;
    int rc = efs_pack_object_key(o->ledger.publication_group, o->ledger.segment_id, o->ledger.segment_generation, key);
    if (!rc) rc = unlocked(kv, key, sizeof(key));
    if (!rc) rc = efs_pack_object_encode(o, bytes, sizeof(bytes), &len);
    return rc ? rc : efs_kv_put(kv, key, sizeof(key), bytes, len);
}
static int lifetime_get(struct efs_kv *kv, const struct efs_pack_object *request, struct efs_pack_object *out)
{
    uint8_t expected[EFS_HASH_SIZE], actual[EFS_HASH_SIZE];
    int rc = manifest(request, expected); if (rc) return rc;
    rc = efs_pack_object_get(kv, request->ledger.publication_group, request->ledger.segment_id,
        request->ledger.segment_generation, out); if (rc) return rc;
    rc = manifest(out, actual);
    return rc ? rc : memcmp(expected, actual, sizeof(actual)) ? EFS_ERR_PROTO : EFS_OK;
}
int efs_pack_upload_is_retired(struct efs_kv *kv, const struct efs_pack_object *request, int *out)
{
    if (!kv || !out) return EFS_ERR_INVAL;
    *out = 0; uint8_t key[19], stream[31], floor[23]; uint64_t f;
    int rc = keys(request, key, stream, floor); if (!rc) rc = floor_get(kv, floor, &f);
    if (!rc) *out = request->ledger.owner.seq <= f;
    return rc;
}
int efs_pack_upload_retire(struct efs_kv *kv, const struct efs_pack_object *request)
{
    if (!kv) return EFS_ERR_INVAL;
    uint8_t key[19], stream[31], floor[23], value[12]; uint64_t f;
    int rc = keys(request, key, stream, floor); if (rc) return rc;
    rc = floor_get(kv, floor, &f); if (rc || request->ledger.owner.seq <= f) return rc;
    struct efs_pack_object o; rc = efs_pack_admission_get(kv, request, &o); if (rc) return rc;
    for (uint32_t i = 0; i < o.ledger.count; i++) if (o.ledger.state[i] == EFS_PACK_PENDING) return EFS_ERR_BUSY;
    struct scan scan; rc = scan_stream(kv, stream, &scan); if (rc) return rc;
    if (!scan.count || scan.first != o.ledger.owner.seq) return EFS_ERR_BUSY;
    rc = unlocked(kv, key, sizeof(key)); if (rc) return rc;
    rc = unlocked(kv, stream, sizeof(stream)); if (rc) return rc;
    rc = unlocked(kv, floor, sizeof(floor)); if (rc) return rc;
    put(value, 1, 4); put(value + 4, o.ledger.owner.seq, 8);
    struct efs_kv_item items[] = {{EFS_KV_DEL, stream, sizeof(stream), NULL, 0},
        {EFS_KV_PUT, floor, sizeof(floor), value, sizeof(value)}};
    return efs_kv_batch(kv, items, 2);
}
int efs_pack_admission_ack(struct efs_kv *kv, const struct efs_pack_object *request, uint32_t bit, int deletion)
{
    if (deletion != 0 && deletion != 1) return EFS_ERR_INVAL;
    struct efs_pack_object o; int rc = lifetime_get(kv, request, &o); if (rc) return rc;
    rc = deletion ? efs_pack_ledger_delete_ack(&o.ledger, bit) : efs_pack_ledger_fence_ack(&o.ledger, bit);
    return rc ? rc : save(kv, &o);
}
int efs_pack_admission_cancel(struct efs_kv *kv, const struct efs_pack_object *request)
{
    struct efs_pack_object o; int rc = lifetime_get(kv, request, &o); if (rc) return rc;
    rc = efs_session_reclaimable(kv, anchor(o.ledger.publication_group), o.ledger.owner.client_uuid, o.ledger.owner.session_epoch); if (rc) return rc;
    for (uint32_t i = 0; i < o.ledger.count; i++) if (o.ledger.state[i] == EFS_PACK_PENDING) o.ledger.state[i] = EFS_PACK_CANCELLED;
    rc = efs_pack_ledger_reclaim_begin(&o.ledger);
    if (rc != EFS_OK && rc != EFS_ERR_BUSY) return rc;
    return save(kv, &o);
}
int efs_pack_admission_retire(struct efs_kv *kv, const struct efs_pack_object *request)
{
    if (!kv) return EFS_ERR_INVAL;
    struct efs_pack_object o; int rc = lifetime_get(kv, request, &o);
    if (rc == EFS_ERR_NOT_FOUND) {
        uint8_t key[19], stream[31], floor[23]; uint64_t f;
        rc = keys(request, key, stream, floor); if (!rc) rc = floor_get(kv, floor, &f);
        return rc ? rc : request->ledger.owner.seq <= f ? EFS_OK : EFS_ERR_NOT_FOUND;
    }
    if (rc) return rc;
    if (o.ledger.phase != EFS_PACK_RETIRED) return EFS_ERR_BUSY;
    rc = efs_pack_upload_retire(kv, request); if (rc) return rc;
    /* Floor durability is established before dropping the last lifetime row.
     * A crash between these batches leaves a safe, retryable retained object. */
    uint8_t key[19]; rc = efs_pack_object_key(o.ledger.publication_group, o.ledger.segment_id, o.ledger.segment_generation, key);
    if (!rc) rc = unlocked(kv, key, sizeof(key));
    return rc ? rc : efs_kv_del(kv, key, sizeof(key));
}
