#include "efs/frag_record.h"
#include "efs/checksum.h"
#include <string.h>

static const uint8_t magic[8] = {'E', 'F', 'S', 'F', 'R', 'A', 'G', 0};

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
static int ident_valid(const struct efs_frag_ident *id)
{
    if (!id || !id->object || !id->generation || !id->payload_len ||
        id->fragment_index >= EFS_NUM_FRAGMENTS)
        return 0;
    if (id->kind == EFS_FRAG_KIND_CHUNK)
        return id->coding_profile == EFS_FRAG_PROFILE_K2F1;
    if (id->kind == EFS_FRAG_KIND_SEGMENT)
        return id->coding_profile == EFS_FRAG_PROFILE_K2F1 && !id->chunk_index;
    return 0;
}

int efs_frag_record_encode(const struct efs_frag_ident *id,
                           const uint8_t payload_digest[EFS_HASH_SIZE],
                           uint8_t out[EFS_FRAG_RECORD_BYTES])
{
    if (!out || !payload_digest || !ident_valid(id))
        return EFS_ERR_INVAL;
    memset(out, 0, EFS_FRAG_RECORD_BYTES);
    memcpy(out, magic, 8);
    put(out + 8, EFS_FRAG_RECORD_VERSION, 2);
    put(out + 10, id->kind, 2);
    put(out + 12, EFS_FRAG_RECORD_BYTES, 4);
    put(out + 16, id->space, 4);
    put(out + 20, id->fragment_index, 4);
    put(out + 24, id->object, 8);
    put(out + 32, id->generation, 8);
    put(out + 40, id->chunk_index, 4);
    put(out + 44, id->payload_len, 4);
    put(out + 48, id->coding_profile, 4);
    memcpy(out + 56, payload_digest, EFS_HASH_SIZE);
    efs_hash(out, 88, out + 88);
    return EFS_OK;
}

int efs_frag_record_parse(const uint8_t *rec, size_t len,
                          struct efs_frag_ident *id,
                          uint8_t payload_digest[EFS_HASH_SIZE])
{
    static const uint8_t zero[8];
    uint8_t bound[EFS_HASH_SIZE];
    if (!rec || len < EFS_FRAG_RECORD_BYTES)
        return EFS_ERR_CHECKSUM; /* torn or absent evidence */
    if (memcmp(rec, magic, 8) || get(rec + 8, 2) != EFS_FRAG_RECORD_VERSION ||
        get(rec + 12, 4) != EFS_FRAG_RECORD_BYTES || get(rec + 52, 4) ||
        memcmp(rec + 120, zero, 8))
        return EFS_ERR_FRAG_FORMAT;
    uint32_t kind = (uint32_t)get(rec + 10, 2);
    if (kind != EFS_FRAG_KIND_CHUNK && kind != EFS_FRAG_KIND_SEGMENT)
        return EFS_ERR_FRAG_FORMAT;
    efs_hash(rec, 88, bound);
    if (memcmp(bound, rec + 88, EFS_HASH_SIZE))
        return EFS_ERR_CHECKSUM;
    struct efs_frag_ident r = {
        .kind = kind,
        .space = (uint32_t)get(rec + 16, 4),
        .fragment_index = (uint32_t)get(rec + 20, 4),
        .object = get(rec + 24, 8),
        .generation = get(rec + 32, 8),
        .chunk_index = (uint32_t)get(rec + 40, 4),
        .payload_len = (uint32_t)get(rec + 44, 4),
        .coding_profile = (uint32_t)get(rec + 48, 4),
    };
    if (!ident_valid(&r))
        return EFS_ERR_FRAG_FORMAT;
    if (id)
        *id = r;
    if (payload_digest)
        memcpy(payload_digest, rec + 56, EFS_HASH_SIZE);
    return EFS_OK;
}

int efs_frag_record_verify(const uint8_t *rec, size_t len,
                           const struct efs_frag_ident *expect,
                           uint8_t payload_digest[EFS_HASH_SIZE],
                           const char **field)
{
    struct efs_frag_ident got;
    if (field)
        *field = NULL;
    if (!expect)
        return EFS_ERR_INVAL;
    int rc = efs_frag_record_parse(rec, len, &got, payload_digest);
    if (rc != EFS_OK)
        return rc;
    const char *bad = got.kind != expect->kind ? "kind" :
        got.space != expect->space ? "space" :
        got.object != expect->object ? "object" :
        got.generation != expect->generation ? "generation" :
        got.chunk_index != expect->chunk_index ? "chunk_index" :
        got.fragment_index != expect->fragment_index ? "fragment_index" :
        got.coding_profile != expect->coding_profile ? "coding_profile" :
        got.payload_len != expect->payload_len ? "payload_len" : NULL;
    if (bad) {
        if (field)
            *field = bad;
        return EFS_ERR_FRAG_IDENTITY;
    }
    return EFS_OK;
}

int efs_frag_verify(const uint8_t *payload, uint32_t len, const uint8_t *rec,
                    size_t rec_len, const struct efs_frag_ident *expect,
                    uint8_t payload_digest[EFS_HASH_SIZE], const char **field)
{
    uint8_t want[EFS_HASH_SIZE], got[EFS_HASH_SIZE];
    int rc = efs_frag_record_verify(rec, rec_len, expect, want, field);
    if (rc != EFS_OK)
        return rc;
    if (!payload || len != expect->payload_len)
        return EFS_ERR_CHECKSUM;
    efs_hash(payload, len, got);
    if (memcmp(got, want, EFS_HASH_SIZE)) {
        if (field)
            *field = "payload";
        return EFS_ERR_CHECKSUM;
    }
    if (payload_digest)
        memcpy(payload_digest, want, EFS_HASH_SIZE);
    return EFS_OK;
}
