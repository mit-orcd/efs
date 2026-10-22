/* A packed segment is validated once per image (ledger init, compaction
 * enumeration, packed-base reads), and every reference read through that
 * one validation is still checked: identity, generations, offset, digest
 * and the record's payload bytes. */
#include "efs/pack_ledger.h"
#include "efs/checksum.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N EFS_PACK_MAX_RECORDS
static struct efs_pack_record records[N];
static struct efs_pack_ref refs[N];
static uint8_t payload[N][8];

static void put32(uint8_t *p, uint32_t n) { for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)n; n >>= 8; } }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void resign(uint8_t *image)
{
    efs_hash(image, 64, image + 64);
    uint32_t used = get32(image + 20);
    efs_hash(image, used - EFS_HASH_SIZE, image + used - EFS_HASH_SIZE);
}
static size_t build(uint8_t *image, unsigned count, uint64_t id)
{
    size_t geometry;
    for (unsigned i = 0; i < count; i++) {
        memset(payload[i], (int)(i * 7 + 1), sizeof(payload[i]));
        records[i] = (struct efs_pack_record){.ino = 1000 + i, .inode_generation = 3, .content_epoch = 5,
            .record_generation = 7 + i, .len = sizeof(payload[i]), .data = payload[i]};
    }
    assert(!efs_pack_build(id, 1, EFS_RAFT_GROUP_SHARD, records, count, image, EFS_SEGMENT_MAX_SIZE,
                           &geometry, refs));
    return geometry;
}
static void read_refused(const struct efs_pack_view *v, const struct efs_pack_ref *r)
{
    const uint8_t *data = (const uint8_t *)1;
    uint32_t len = 99;
    assert(efs_pack_view_read(v, r, &data, &len) != EFS_OK && !data && !len);
}

static void validate_once(void)
{
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE), *copy = malloc(EFS_SEGMENT_MAX_SIZE);
    assert(image && copy);
    size_t g = build(image, N, 41);
    struct efs_opid owner = {.client_uuid = {9}, .session_epoch = 2, .seq = 41};
    struct efs_pack_ledger *ledger = malloc(sizeof(*ledger));
    assert(ledger);
    /* Ledger init: one whole-image validation for N references (was N+1). */
    uint64_t before = efs_pack_validations();
    assert(!efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger));
    assert(efs_pack_validations() - before == 1);
    /* Compaction's enumeration: one validation, then every record. */
    struct efs_pack_view v;
    before = efs_pack_validations();
    assert(!efs_pack_view_open(image, g, &v) && v.count == N);
    for (uint32_t i = 0; i < v.count; i++) {
        struct efs_pack_ref r;
        uint8_t a[EFS_PACK_REF_BYTES], b[EFS_PACK_REF_BYTES];
        assert(!efs_pack_view_record_at(&v, i, &r));
        assert(!efs_pack_ref_encode(&r, a) && !efs_pack_ref_encode(&refs[i], b) && !memcmp(a, b, sizeof(a)));
    }
    /* Packed bases sharing the segment: reads add no validation. */
    for (uint32_t i = 0; i < N; i++) {
        const uint8_t *data;
        uint32_t len;
        assert(!efs_pack_view_read(&v, &refs[i], &data, &len));
        assert(len == sizeof(payload[i]) && !memcmp(data, payload[i], len));
    }
    assert(efs_pack_validations() - before == 1);

    /* Wrong references are refused through the view. */
    struct efs_pack_ref bad;
    bad = refs[3]; bad.digest[5] ^= 1; read_refused(&v, &bad);
    bad = refs[3]; bad.inode_generation++; read_refused(&v, &bad);
    bad = refs[3]; bad.record_generation++; read_refused(&v, &bad);
    bad = refs[3]; bad.content_epoch++; read_refused(&v, &bad);
    bad = refs[3]; bad.segment_generation++; read_refused(&v, &bad);
    bad = refs[3]; bad.offset = refs[4].offset; read_refused(&v, &bad);   /* another record */
    bad = refs[3]; bad.offset++; read_refused(&v, &bad);                  /* no record */
    bad = refs[3]; bad.ino = refs[4].ino; read_refused(&v, &bad);
    bad = refs[3]; bad.len--; read_refused(&v, &bad);
    /* ... and by ledger init, which must not admit them. */
    struct efs_pack_ref keep = refs[3];
    refs[3].digest[0] ^= 1;
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep; refs[3].record_generation++;
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep; refs[3].offset = refs[4].offset;
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep; refs[3].ino = refs[4].ino; /* a duplicate inode among the references */
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep;

    /* A payload flip: refused at open, and through an already-open view. */
    memcpy(copy, image, g);
    copy[refs[9].offset] ^= 0x40;
    struct efs_pack_view cv;
    assert(efs_pack_view_open(copy, g, &cv) == EFS_ERR_CHECKSUM && !cv.image);
    assert(efs_pack_ledger_init(copy, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    memcpy(copy, image, g);
    assert(!efs_pack_view_open(copy, g, &cv));
    copy[refs[9].offset] ^= 0x40;              /* mutated after validation */
    read_refused(&cv, &refs[9]);
    struct efs_pack_ref r;
    assert(efs_pack_view_record_at(&cv, 9, &r) == EFS_ERR_CHECKSUM);
    const uint8_t *data;
    uint32_t len;
    assert(!efs_pack_view_read(&cv, &refs[10], &data, &len)); /* other records stay readable */
    /* A header changed after validation, or a view not made by open. */
    memcpy(copy, image, g);
    assert(!efs_pack_view_open(copy, g, &cv));
    copy[40 + 7] ^= 1;                         /* segment generation */
    read_refused(&cv, &refs[0]);
    assert(efs_pack_view_record_at(&cv, 0, &r) == EFS_ERR_INVAL);
    struct efs_pack_view forged = v;
    forged.image = copy;                       /* retargeted onto another buffer */
    memcpy(copy, image, g);
    read_refused(&forged, &refs[0]);
    memset(&forged, 0, sizeof(forged));
    read_refused(&forged, &refs[0]);
    forged = v; forged.count = 2;
    assert(efs_pack_view_record_at(&forged, 0, &r) == EFS_ERR_INVAL);
    /* Duplicate inode, with valid checksums: refused by the sorted check. */
    memcpy(copy, image, g);
    memcpy(copy + EFS_PACK_HEADER_BYTES + 400 * EFS_PACK_RECORD_BYTES, copy + EFS_PACK_HEADER_BYTES + 17 * EFS_PACK_RECORD_BYTES, 8);
    resign(copy);
    assert(efs_pack_validate(copy, g) == EFS_ERR_PROTO);
    assert(efs_pack_ledger_init(copy, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    /* Wrong record count in the header is still refused. */
    memcpy(copy, image, g);
    put32(copy + 24, N - 1); resign(copy);
    assert(efs_pack_view_open(copy, g, &cv) != EFS_OK);
    free(ledger);
    free(image);
    free(copy);
}

int main(void)
{
    validate_once();
    puts("packed segment validated once per image; refs and corruption PASS");
    return 0;
}
