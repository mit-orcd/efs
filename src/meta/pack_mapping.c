#include "efs/pack_mapping.h"
#include "efs/kv_key.h"
#include "efs/checksum.h"
#include "efs/raft.h"
#include "efs/txn.h"
#include <stdlib.h>
#include <string.h>

static void put(uint8_t *p, uint64_t n, unsigned bytes)
{ while (bytes) { p[--bytes] = (uint8_t)n; n >>= 8; } }
static uint64_t get(const uint8_t *p, unsigned bytes)
{ uint64_t n = 0; while (bytes--) n = (n << 8) | *p++; return n; }
uint32_t efs_pack_group_anchor(uint32_t group)
{
    if (group == EFS_RAFT_GROUP_SHARD) return 1;
    if (group == EFS_RAFT_GROUP_SHARD2) return 2;
    return 0;
}
int efs_pack_object_key(uint32_t group, uint64_t id, uint64_t generation,
    uint8_t out[EFS_PACK_OBJECT_KEY_BYTES])
{
    uint32_t shard = efs_pack_group_anchor(group);
    if (!out || !shard || !id || !generation) return EFS_ERR_INVAL;
    put(out, shard, 2); out[2] = EFS_KV_KIND_PACK_SEGMENT;
    put(out + 3, id, 8); put(out + 11, generation, 8);
    return EFS_OK;
}
static int placement_valid(const struct efs_pack_object *o)
{
    if (!o || !efs_pack_group_anchor(o->ledger.publication_group)) return 0;
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!o->nodes[i] || o->nodes[i] > EFS_MAX_NODES ||
            !(o->ledger.member_mask & (1u << (o->nodes[i] - 1)))) return 0;
        for (unsigned j = 0; j < i; j++) if (o->nodes[i] == o->nodes[j]) return 0;
    }
    return 1;
}
int efs_pack_object_encode(const struct efs_pack_object *o, uint8_t *out,
    size_t cap, size_t *bytes)
{
    if (!out || !bytes || !placement_valid(o)) return EFS_ERR_INVAL;
    /* Encode privately: failed serialization cannot expose a partial object. */
    uint8_t image[EFS_PACK_OBJECT_MAX_BYTES]; size_t len;
    memset(image, 0, EFS_PACK_OBJECT_HEADER);
    int rc = efs_pack_ledger_encode(&o->ledger, image + EFS_PACK_OBJECT_HEADER,
        EFS_PACK_LEDGER_MAX_BYTES, &len);
    if (rc != EFS_OK) return rc;
    size_t total = EFS_PACK_OBJECT_HEADER + len + EFS_HASH_SIZE;
    if (cap < total) return EFS_ERR_OVERFLOW;
    memcpy(image, "EFSPMAP2", 8); put(image + 8, total, 4);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) put(image + 12 + i * 4, o->nodes[i], 4);
    memcpy(image + 24, o->checksums, sizeof(o->checksums));
    efs_hash(image, total - EFS_HASH_SIZE, image + total - EFS_HASH_SIZE);
    memcpy(out, image, total); *bytes = total;
    return EFS_OK;
}
int efs_pack_object_decode(const uint8_t *in, size_t bytes, struct efs_pack_object *out)
{
    if (!in || !out || bytes < EFS_PACK_OBJECT_HEADER + EFS_PACK_LEDGER_HEADER +
            EFS_PACK_LEDGER_RECORD + 2 * EFS_HASH_SIZE ||
        bytes > EFS_PACK_OBJECT_MAX_BYTES || memcmp(in, "EFSPMAP2", 8) ||
        get(in + 8, 4) != bytes || get(in + 120, 8)) return EFS_ERR_PROTO;
    uint8_t sum[EFS_HASH_SIZE];
    efs_hash(in, bytes - EFS_HASH_SIZE, sum);
    if (memcmp(sum, in + bytes - EFS_HASH_SIZE, sizeof(sum))) return EFS_ERR_CHECKSUM;
    struct efs_pack_object *o = calloc(1, sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    int rc = efs_pack_ledger_decode(in + EFS_PACK_OBJECT_HEADER,
        bytes - EFS_PACK_OBJECT_HEADER - EFS_HASH_SIZE, &o->ledger);
    if (rc == EFS_OK) {
        for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) o->nodes[i] = (uint32_t)get(in + 12 + i * 4, 4);
        memcpy(o->checksums, in + 24, sizeof(o->checksums));
        if (!placement_valid(o)) rc = EFS_ERR_PROTO;
    }
    if (rc == EFS_OK) *out = *o;
    free(o);
    return rc;
}
int efs_pack_object_get(struct efs_kv *kv, uint32_t group, uint64_t id, uint64_t gen,
    struct efs_pack_object *out)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    uint32_t len = EFS_PACK_OBJECT_MAX_BYTES;
    if (!kv || !out) return EFS_ERR_INVAL;
    uint8_t *value = malloc(EFS_PACK_OBJECT_MAX_BYTES);
    if (!value) return EFS_ERR_NOMEM;
    int rc = efs_pack_object_key(group, id, gen, key);
    if (rc == EFS_OK) rc = efs_kv_get(kv, key, sizeof(key), value, &len);
    if (rc == EFS_OK) rc = efs_pack_object_decode(value, len, out);
    free(value);
    if (rc == EFS_OK && (out->ledger.publication_group != group ||
        out->ledger.segment_id != id || out->ledger.segment_generation != gen)) return EFS_ERR_PROTO;
    return rc;
}
int efs_pack_chunk_identity(const struct efs_meta_chunk *ch, efs_ino_t ino,
    uint64_t inode_gen, uint32_t ci)
{
    if (!ch) return EFS_ERR_INVAL;
    if (ch->coding_profile_id != EFS_META_PROFILE_PACK) return EFS_OK;
    const struct efs_pack_ref *r = &ch->packed;
    return ci == 0 && r->ino == ino && r->inode_generation == inode_gen &&
        r->record_generation == ch->generation && r->content_epoch == ch->content_epoch &&
        r->publication_group == efs_raft_shard_group(efs_kv_lane_shard(ino, 0))
        ? EFS_OK : EFS_ERR_PROTO;
}
struct entry {
    struct efs_pack_object object;
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES], value[EFS_PACK_OBJECT_MAX_BYTES];
    uint8_t changed;
};
struct work { unsigned count; struct entry *entries[EFS_PACK_MAPPING_OBJECT_MAX]; };
void efs_pack_mapping_plan_free(struct efs_pack_mapping_plan *p)
{
    if (!p) return;
    struct work *w = p->storage;
    if (w) { for (unsigned i = 0; i < w->count; i++) free(w->entries[i]); free(w); }
    memset(p, 0, sizeof(*p));
}
static int load(struct efs_kv *kv, struct work *w, const struct efs_pack_ref *r,
    struct entry **out)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    int rc = efs_pack_object_key(r->publication_group, r->segment_id, r->segment_generation, key);
    if (rc != EFS_OK) return rc;
    for (unsigned i = 0; i < w->count; i++) if (!memcmp(key, w->entries[i]->key, sizeof(key))) {
        *out = w->entries[i]; return EFS_OK;
    }
    if (w->count == EFS_PACK_MAPPING_OBJECT_MAX) return EFS_ERR_BUSY;
    rc = efs_txn_key_exclusive(kv, key, sizeof(key));
    if (rc != EFS_OK) return rc;
    struct entry *e = calloc(1, sizeof(*e));
    if (!e) return EFS_ERR_NOMEM;
    rc = efs_pack_object_get(kv, r->publication_group, r->segment_id, r->segment_generation, &e->object);
    if (rc != EFS_OK) { free(e); return rc; }
    memcpy(e->key, key, sizeof(key)); w->entries[w->count++] = e;
    *out = e; return EFS_OK;
}
/* The row's placement must be the segment's, and its reference one of the
 * ledger's records. */
static int slot(struct entry *e, const struct efs_meta_chunk *ch, unsigned *at)
{
    const struct efs_pack_object *o = &e->object;
    if (ch->packed.geometry != o->ledger.geometry || memcmp(ch->nodes, o->nodes, sizeof(o->nodes)) ||
        memcmp(ch->checksums, o->checksums, sizeof(o->checksums))) return EFS_ERR_STALE;
    return efs_pack_ledger_slot(&o->ledger, &ch->packed, at);
}
static int same_ref(const struct efs_meta_chunk *a, const struct efs_meta_chunk *b)
{
    uint8_t x[EFS_PACK_REF_BYTES], y[EFS_PACK_REF_BYTES];
    return !efs_pack_ref_encode(&a->packed, x) && !efs_pack_ref_encode(&b->packed, y) &&
        !memcmp(x, y, sizeof(x));
}
int efs_pack_mapping_plan(struct efs_kv *kv, const struct efs_pack_mapping_change *changes,
    uint32_t count, struct efs_pack_mapping_plan *out)
{
    if (!kv || !out || (count && !changes)) return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    struct work *w = NULL; int rc = EFS_OK;
    for (unsigned i = 0; i < count; i++) {
        const struct efs_pack_mapping_change *c = &changes[i];
        const struct efs_meta_chunk *before = c->before, *after = c->after;
        int oldpack = before && before->coding_profile_id == EFS_META_PROFILE_PACK;
        int newpack = after && after->coding_profile_id == EFS_META_PROFILE_PACK;
        if (!oldpack && !newpack) continue;
        if ((oldpack && (rc = efs_pack_chunk_identity(before, c->ino, c->inode_generation, c->chunk_index))) ||
            (newpack && (rc = efs_pack_chunk_identity(after, c->ino, c->inode_generation, c->chunk_index))))
            goto fail;
        /* Only the switch creates packed references. */
        if (newpack && !(oldpack && same_ref(before, after))) { rc = EFS_ERR_PROTO; goto fail; }
        if (!w) { w = calloc(1, sizeof(*w)); if (!w) { rc = EFS_ERR_NOMEM; goto fail; } out->storage = w; }
        struct entry *e; unsigned at;
        rc = load(kv, w, &before->packed, &e);
        if (rc == EFS_OK) rc = slot(e, before, &at);
        if (rc != EFS_OK) goto fail;
        if (e->object.ledger.phase != EFS_PACK_ACTIVE || e->object.ledger.state[at] != EFS_PACK_LIVE) {
            rc = EFS_ERR_STALE; goto fail;
        }
        if (newpack) continue; /* unchanged reference */
        rc = efs_pack_ledger_release(&e->object.ledger, &before->packed);
        if (rc != EFS_OK) goto fail;
        e->changed = 1;
    }
    if (w) for (unsigned i = 0; i < w->count; i++) {
        struct entry *e = w->entries[i]; size_t len;
        if (!e->changed) continue;
        rc = efs_pack_ledger_reclaim_begin(&e->object.ledger);
        if (rc != EFS_OK && rc != EFS_ERR_BUSY) goto fail;
        rc = efs_pack_object_encode(&e->object, e->value, sizeof(e->value), &len);
        if (rc != EFS_OK) goto fail;
        out->items[out->count++] = (struct efs_kv_item){EFS_KV_PUT, e->key, sizeof(e->key), e->value, (uint32_t)len};
    }
    return EFS_OK;
fail:
    efs_pack_mapping_plan_free(out); return rc;
}
int efs_pack_switch_encode(const struct efs_pack_switch *s, uint8_t *out, size_t cap, size_t *bytes)
{
    if (!s || !out || !bytes || !efs_pack_group_anchor(s->group) || !s->segment_id ||
        !s->segment_generation || !s->count || s->count > EFS_PACK_SWITCH_MAX) return EFS_ERR_INVAL;
    size_t n = 19u + (size_t)s->count * EFS_MD_PACK_SWITCH_MEMBER_BYTES;
    if (cap < n) return EFS_ERR_OVERFLOW;
    uint8_t refs[EFS_PACK_SWITCH_MAX][EFS_PACK_REF_BYTES];
    for (uint32_t i = 0; i < s->count; i++)
        if (efs_pack_ref_encode(&s->members[i].ref, refs[i]) != EFS_OK) return EFS_ERR_INVAL;
    out[0] = (uint8_t)s->group; put(out + 1, s->segment_id, 8); put(out + 9, s->segment_generation, 8);
    put(out + 17, s->count, 2);
    for (uint32_t i = 0; i < s->count; i++) {
        uint8_t *m = out + 19 + (size_t)i * EFS_MD_PACK_SWITCH_MEMBER_BYTES;
        put(m, s->members[i].ino, 8); put(m + 8, s->members[i].inode_generation, 8);
        memcpy(m + 16, s->members[i].source_digest, EFS_HASH_SIZE);
        memcpy(m + 16 + EFS_HASH_SIZE, refs[i], EFS_PACK_REF_BYTES);
    }
    *bytes = n;
    return EFS_OK;
}
int efs_pack_switch_decode(const uint8_t *in, size_t len, struct efs_pack_switch *out)
{
    if (!in || !out || len < 19) return EFS_ERR_PROTO;
    uint32_t count = (uint32_t)get(in + 17, 2);
    if (!efs_pack_group_anchor(in[0]) || !count || count > EFS_PACK_SWITCH_MAX ||
        len != 19u + (size_t)count * EFS_MD_PACK_SWITCH_MEMBER_BYTES) return EFS_ERR_PROTO;
    out->group = in[0]; out->segment_id = get(in + 1, 8); out->segment_generation = get(in + 9, 8);
    out->count = count;
    if (!out->segment_id || !out->segment_generation) return EFS_ERR_PROTO;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *m = in + 19 + (size_t)i * EFS_MD_PACK_SWITCH_MEMBER_BYTES;
        struct efs_pack_switch_member *x = &out->members[i];
        x->ino = get(m, 8); x->inode_generation = get(m + 8, 8);
        memcpy(x->source_digest, m + 16, EFS_HASH_SIZE);
        if (efs_pack_ref_decode(m + 16 + EFS_HASH_SIZE, EFS_PACK_REF_BYTES, &x->ref) != EFS_OK)
            return EFS_ERR_PROTO;
    }
    return EFS_OK;
}
