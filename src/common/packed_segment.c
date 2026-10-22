#include "efs/packed_segment.h"
#include "efs/checksum.h"
#include <stdlib.h>
#include <string.h>

static void put(uint8_t *p, uint64_t v, unsigned n)
{
    while (n) { p[--n] = (uint8_t)v; v >>= 8; }
}
static uint64_t get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}
static int geometry_valid(size_t n)
{
    return n >= EFS_SEGMENT_ALIGNMENT && n <= EFS_SEGMENT_MAX_SIZE &&
           n % EFS_SEGMENT_ALIGNMENT == 0;
}
static int ref_valid(const struct efs_pack_ref *r)
{
    return r && r->ino && r->inode_generation && r->record_generation &&
           r->segment_id && r->segment_generation && geometry_valid(r->geometry) &&
           r->len && r->len <= EFS_PACK_MAX_PAYLOAD &&
           r->offset >= EFS_PACK_HEADER_BYTES + EFS_PACK_RECORD_BYTES &&
           (uint64_t)r->offset + r->len + EFS_HASH_SIZE <= r->geometry;
}
int efs_pack_ref_encode(const struct efs_pack_ref *r, uint8_t out[EFS_PACK_REF_BYTES])
{
    if (!out || !ref_valid(r)) return EFS_ERR_INVAL;
    put(out, r->ino, 8); put(out + 8, r->inode_generation, 8);
    put(out + 16, r->content_epoch, 8); put(out + 24, r->record_generation, 8);
    put(out + 32, r->segment_id, 8); put(out + 40, r->segment_generation, 8);
    put(out + 48, r->geometry, 4); put(out + 52, r->publication_group, 4);
    put(out + 56, r->offset, 4); put(out + 60, r->len, 4);
    memcpy(out + 64, r->digest, EFS_HASH_SIZE);
    return EFS_OK;
}
int efs_pack_ref_decode(const uint8_t *in, size_t len, struct efs_pack_ref *out)
{
    struct efs_pack_ref r;
    if (!in || !out || len != EFS_PACK_REF_BYTES) return EFS_ERR_PROTO;
    memset(&r, 0, sizeof(r));
    r.ino = get(in, 8); r.inode_generation = get(in + 8, 8);
    r.content_epoch = get(in + 16, 8); r.record_generation = get(in + 24, 8);
    r.segment_id = get(in + 32, 8); r.segment_generation = get(in + 40, 8);
    r.geometry = (uint32_t)get(in + 48, 4); r.publication_group = (uint32_t)get(in + 52, 4);
    r.offset = (uint32_t)get(in + 56, 4); r.len = (uint32_t)get(in + 60, 4);
    memcpy(r.digest, in + 64, EFS_HASH_SIZE);
    if (!ref_valid(&r)) return EFS_ERR_PROTO;
    *out = r;
    return EFS_OK;
}
int efs_pack_build(uint64_t id, uint64_t generation, uint32_t group,
                   const struct efs_pack_record *records, uint32_t count,
                   uint8_t *image, size_t capacity, size_t *geometry,
                   struct efs_pack_ref *refs)
{
    if (!id || !generation || !records || !count || count > EFS_PACK_MAX_RECORDS ||
        !image || !geometry || !refs) return EFS_ERR_INVAL;
    size_t used = EFS_PACK_HEADER_BYTES + (size_t)count * EFS_PACK_RECORD_BYTES + EFS_HASH_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        const struct efs_pack_record *r = &records[i];
        if (!r->ino || !r->inode_generation || !r->record_generation || !r->data ||
            !r->len || r->len > EFS_PACK_MAX_PAYLOAD) return EFS_ERR_INVAL;
        for (uint32_t j = 0; j < i; j++)
            if (r->ino == records[j].ino) return EFS_ERR_INVAL;
        used += r->len;
    }
    size_t size;
    if (efs_segment_geometry(used, &size) != EFS_OK || capacity < size)
        return EFS_ERR_OVERFLOW;
    /* No validation after this point: callers can retry a rejected build safely. */
    memset(image, 0, size);
    memcpy(image, "EFSPACK1", 8);
    put(image + 8, EFS_PACK_VERSION, 4); put(image + 12, EFS_PACK_HEADER_BYTES, 4);
    put(image + 16, size, 4); put(image + 20, used, 4); put(image + 24, count, 4);
    put(image + 32, id, 8); put(image + 40, generation, 8); put(image + 48, group, 4);
    efs_hash(image, 64, image + 64);
    uint32_t offset = EFS_PACK_HEADER_BYTES + count * EFS_PACK_RECORD_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        const struct efs_pack_record *r = &records[i];
        uint8_t *p = image + EFS_PACK_HEADER_BYTES + i * EFS_PACK_RECORD_BYTES;
        struct efs_pack_ref *ref = &refs[i];
        memset(ref, 0, sizeof(*ref));
        ref->ino = r->ino; ref->inode_generation = r->inode_generation;
        ref->content_epoch = r->content_epoch; ref->record_generation = r->record_generation;
        ref->segment_id = id; ref->segment_generation = generation;
        ref->geometry = (uint32_t)size; ref->publication_group = group;
        ref->offset = offset; ref->len = r->len;
        efs_hash(r->data, r->len, ref->digest);
        put(p, r->ino, 8); put(p + 8, r->inode_generation, 8);
        put(p + 16, r->content_epoch, 8); put(p + 24, r->record_generation, 8);
        put(p + 32, offset, 4); put(p + 36, r->len, 4);
        memcpy(p + 40, ref->digest, EFS_HASH_SIZE);
        memcpy(image + offset, r->data, r->len);
        offset += r->len;
    }
    efs_hash(image, used - EFS_HASH_SIZE, image + used - EFS_HASH_SIZE);
    *geometry = size;
    return EFS_OK;
}
static uint64_t validations;
uint64_t efs_pack_validations(void)
{
    return __atomic_load_n(&validations, __ATOMIC_RELAXED);
}
static int ino_order(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
int efs_pack_validate(const uint8_t *image, size_t size)
{
    uint8_t digest[EFS_HASH_SIZE];
    uint64_t inos[EFS_PACK_MAX_RECORDS];
    __atomic_fetch_add(&validations, 1, __ATOMIC_RELAXED);
    if (!image || !geometry_valid(size)) return EFS_ERR_PROTO;
    uint32_t version = (uint32_t)get(image + 8, 4);
    uint32_t max_payload = version == 1 ? EFS_PACK_V1_MAX_PAYLOAD : EFS_PACK_MAX_PAYLOAD;
    if (memcmp(image, "EFSPACK1", 8) || (version != 1 && version != EFS_PACK_VERSION) ||
        get(image + 12, 4) != EFS_PACK_HEADER_BYTES || get(image + 16, 4) != size ||
        !get(image + 32, 8) || !get(image + 40, 8)) return EFS_ERR_PROTO;
    if (get(image + 28, 4)) return EFS_ERR_PROTO;
    for (unsigned i = 52; i < 64; i++) if (image[i]) return EFS_ERR_PROTO;
    efs_hash(image, 64, digest);
    if (memcmp(digest, image + 64, EFS_HASH_SIZE)) return EFS_ERR_CHECKSUM;
    uint32_t used = (uint32_t)get(image + 20, 4), count = (uint32_t)get(image + 24, 4);
    if (!count || count > EFS_PACK_MAX_RECORDS || used > size ||
        used < EFS_PACK_HEADER_BYTES + count * EFS_PACK_RECORD_BYTES + EFS_HASH_SIZE)
        return EFS_ERR_PROTO;
    size_t rounded;
    if (efs_segment_geometry(used, &rounded) != EFS_OK || rounded != size)
        return EFS_ERR_PROTO;
    efs_hash(image, used - EFS_HASH_SIZE, digest);
    if (memcmp(digest, image + used - EFS_HASH_SIZE, EFS_HASH_SIZE)) return EFS_ERR_CHECKSUM;
    for (size_t i = used; i < size; i++) if (image[i]) return EFS_ERR_PROTO;
    uint64_t end = EFS_PACK_HEADER_BYTES + count * EFS_PACK_RECORD_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = image + EFS_PACK_HEADER_BYTES + i * EFS_PACK_RECORD_BYTES;
        uint64_t ino = get(p, 8), offset = get(p + 32, 4), len = get(p + 36, 4);
        if (!ino || !get(p + 8, 8) || !get(p + 24, 8) || !len ||
            len > max_payload || offset != end ||
            offset + len > used - EFS_HASH_SIZE) return EFS_ERR_PROTO;
        inos[i] = ino;
        efs_hash(image + offset, (size_t)len, digest);
        if (memcmp(digest, p + 40, EFS_HASH_SIZE)) return EFS_ERR_CHECKSUM;
        end += len;
    }
    if (end != used - EFS_HASH_SIZE) return EFS_ERR_PROTO;
    /* Unique inodes: sorted, not pairwise (a full table was 130k compares). */
    qsort(inos, count, sizeof(inos[0]), ino_order);
    for (uint32_t i = 1; i < count; i++) if (inos[i - 1] == inos[i]) return EFS_ERR_PROTO;
    return EFS_OK;
}
/* Binds a view to the exact image, geometry and header it validated. */
static void view_seal(const uint8_t *image, size_t size, uint8_t out[EFS_HASH_SIZE])
{
    uint8_t in[16 + EFS_PACK_HEADER_BYTES];
    put(in, (uint64_t)(uintptr_t)image, 8); put(in + 8, size, 8);
    memcpy(in + 16, image, EFS_PACK_HEADER_BYTES);
    efs_hash(in, sizeof(in), out);
}
static int view_valid(const struct efs_pack_view *v)
{
    uint8_t seal[EFS_HASH_SIZE];
    if (!v || !v->image || !geometry_valid(v->geometry) || !v->count ||
        v->count > EFS_PACK_MAX_RECORDS || get(v->image + 24, 4) != v->count) return 0;
    view_seal(v->image, v->geometry, seal);
    return !memcmp(seal, v->seal, EFS_HASH_SIZE);
}
int efs_pack_view_open(const uint8_t *image, size_t size, struct efs_pack_view *v)
{
    if (!v) return EFS_ERR_INVAL;
    memset(v, 0, sizeof(*v));
    int rc = efs_pack_validate(image, size);
    if (rc != EFS_OK) return rc;
    v->image = image; v->geometry = size; v->count = (uint32_t)get(image + 24, 4);
    view_seal(image, size, v->seal);
    return EFS_OK;
}
/* Table entry i, with its payload hashed against its digest. */
static int view_record(const struct efs_pack_view *v, uint32_t i, const uint8_t **entry)
{
    uint8_t digest[EFS_HASH_SIZE];
    const uint8_t *p = v->image + EFS_PACK_HEADER_BYTES + i * EFS_PACK_RECORD_BYTES;
    uint64_t offset = get(p + 32, 4), len = get(p + 36, 4);
    if (!len || len > EFS_PACK_MAX_PAYLOAD ||
        offset < EFS_PACK_HEADER_BYTES + (uint64_t)v->count * EFS_PACK_RECORD_BYTES ||
        offset + len + EFS_HASH_SIZE > v->geometry) return EFS_ERR_PROTO;
    efs_hash(v->image + offset, (size_t)len, digest);
    if (memcmp(digest, p + 40, EFS_HASH_SIZE)) return EFS_ERR_CHECKSUM;
    *entry = p;
    return EFS_OK;
}
int efs_pack_view_read(const struct efs_pack_view *v, const struct efs_pack_ref *r,
                       const uint8_t **data, uint32_t *len)
{
    if (!data || !len) return EFS_ERR_INVAL;
    *data = NULL; *len = 0;
    if (!ref_valid(r) || !view_valid(v)) return EFS_ERR_INVAL;
    const uint8_t *image = v->image;
    if (r->geometry != v->geometry || r->segment_id != get(image + 32, 8) ||
        r->segment_generation != get(image + 40, 8) ||
        r->publication_group != get(image + 48, 4)) return EFS_ERR_STALE;
    /* Validated record offsets strictly increase: find the record at the
     * reference's offset; any other inode or field there is stale. */
    uint32_t lo = 0, hi = v->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t off = get(image + EFS_PACK_HEADER_BYTES + mid * EFS_PACK_RECORD_BYTES + 32, 4);
        if (off == r->offset) { lo = mid; hi = mid + 1; break; }
        if (off < r->offset) lo = mid + 1; else hi = mid;
    }
    if (lo >= hi) return EFS_ERR_STALE;
    const uint8_t *p;
    int rc = view_record(v, lo, &p);
    if (rc != EFS_OK) return rc;
    if (r->ino != get(p, 8) || r->inode_generation != get(p + 8, 8) ||
        r->content_epoch != get(p + 16, 8) || r->record_generation != get(p + 24, 8) ||
        r->offset != get(p + 32, 4) || r->len != get(p + 36, 4) ||
        memcmp(r->digest, p + 40, EFS_HASH_SIZE)) return EFS_ERR_STALE;
    *data = image + r->offset; *len = r->len;
    return EFS_OK;
}
int efs_pack_read(const uint8_t *image, size_t size, const struct efs_pack_ref *r,
                  const uint8_t **data, uint32_t *len)
{
    struct efs_pack_view v;
    if (!data || !len) return EFS_ERR_INVAL;
    *data = NULL; *len = 0;
    if (!ref_valid(r)) return EFS_ERR_INVAL;
    int rc = efs_pack_view_open(image, size, &v);
    return rc == EFS_OK ? efs_pack_view_read(&v, r, data, len) : rc;
}
int efs_pack_view_record_at(const struct efs_pack_view *v, uint32_t i, struct efs_pack_ref *out)
{
    if (!out || !view_valid(v)) return EFS_ERR_INVAL;
    if (i >= v->count) return EFS_ERR_INVAL;
    const uint8_t *p, *image = v->image;
    int rc = view_record(v, i, &p);
    if (rc != EFS_OK) return rc;
    memset(out, 0, sizeof(*out));
    out->ino = get(p, 8); out->inode_generation = get(p + 8, 8);
    out->content_epoch = get(p + 16, 8); out->record_generation = get(p + 24, 8);
    out->offset = (uint32_t)get(p + 32, 4); out->len = (uint32_t)get(p + 36, 4);
    memcpy(out->digest, p + 40, EFS_HASH_SIZE);
    out->segment_id = get(image + 32, 8); out->segment_generation = get(image + 40, 8);
    out->publication_group = (uint32_t)get(image + 48, 4); out->geometry = (uint32_t)v->geometry;
    return ref_valid(out) ? EFS_OK : EFS_ERR_PROTO;
}
