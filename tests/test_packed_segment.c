#include "efs/packed_segment.h"
#include "efs/checksum.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t n)
{
    for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)n; n >>= 8; }
}
static unsigned get32(const uint8_t *p)
{
    return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];
}
static void resign(uint8_t *image)
{
    efs_hash(image, 64, image + 64);
    unsigned used = get32(image + 20);
    assert(used <= EFS_SEGMENT_MAX_SIZE && used >= 32);
    efs_hash(image, used - 32, image + used - 32);
}
static void expect_rejected(const uint8_t *image, size_t size, struct efs_pack_ref *ref)
{
    const uint8_t *data = (const uint8_t *)1; uint32_t len = 111;
    assert(efs_pack_read(image, size, ref, &data, &len) != EFS_OK);
    assert(!data && !len);
}
int main(void)
{
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE), *copy = malloc(EFS_SEGMENT_MAX_SIZE);
    uint8_t *decoded = malloc(EFS_SEGMENT_MAX_SIZE), *fragments = malloc(EFS_SEGMENT_MAX_SIZE * 3 / 2);
    struct efs_pack_record records[EFS_PACK_MAX_RECORDS];
    struct efs_pack_ref refs[EFS_PACK_MAX_RECORDS];
    uint8_t payload[EFS_PACK_MAX_PAYLOAD];
    assert(image && copy && decoded && fragments);
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 37 + i / 19);
    for (unsigned i = 0; i < EFS_PACK_MAX_RECORDS; i++)
        records[i] = (struct efs_pack_record){.ino=100+i,.inode_generation=17,
            .content_epoch=23,.record_generation=29+i,.len=512,.data=payload};
    unsigned counts[] = {1,16,64,128,200};
    size_t size = 0;
    for (unsigned k = 0; k < sizeof(counts)/sizeof(counts[0]); k++) {
        unsigned count = counts[k];
        assert(!efs_pack_build(41, 43, 7, records, count, image, EFS_SEGMENT_MAX_SIZE, &size, refs));
        assert(!efs_pack_validate(image, size));
        assert(size / 2 % 4096 == 0);
        uint8_t *f[3] = {fragments, fragments + size/2, fragments + size};
        assert(!efs_encode_segment(image, size, size, f));
        for (int missing = 0; missing < 3; missing++) {
            memset(f[missing], 0xef, size / 2);
            assert(!efs_decode_segment(f, size, (missing+1)%3, (missing+2)%3, missing, decoded, size));
            for (unsigned i = 0; i < count; i++) {
                const uint8_t *data; uint32_t len;
                assert(!efs_pack_read(decoded, size, &refs[i], &data, &len));
                assert(len == 512 && !memcmp(data, payload, len));
                uint8_t encoded[EFS_PACK_REF_BYTES]; struct efs_pack_ref ref;
                assert(!efs_pack_ref_encode(&refs[i], encoded));
                assert(!efs_pack_ref_decode(encoded, sizeof(encoded), &ref));
                assert(!efs_pack_read(decoded, size, &ref, &data, &len));
            }
            assert(!efs_encode_segment(image, size, size, f));
        }
        uint64_t allocated = 3 * (size/2 + 4096);
        printf("PACK_GEOMETRY records=%u payload=%u geometry=%zu fragment_bytes=%llu ratio=%.6f\n",
            count, count*512, size, (unsigned long long)allocated, (double)allocated/(count*512));
    }
    /* Records enumerate exactly as built (compaction's view of a segment). */
    {
        struct efs_pack_view v; struct efs_pack_ref r; uint8_t a[EFS_PACK_REF_BYTES], b[EFS_PACK_REF_BYTES];
        assert(!efs_pack_build(41, 43, 7, records, 16, image, EFS_SEGMENT_MAX_SIZE, &size, refs));
        assert(!efs_pack_view_open(image, size, &v) && v.count == 16);
        for (unsigned i = 0; i < 16; i++) {
            assert(!efs_pack_view_record_at(&v, i, &r));
            assert(!efs_pack_ref_encode(&r, a) && !efs_pack_ref_encode(&refs[i], b) && !memcmp(a, b, sizeof(a)));
        }
        assert(efs_pack_view_record_at(&v, 16, &r) == EFS_ERR_INVAL);
    }
    /* Maximum record count remains bounded even for one-byte payloads. */
    for (unsigned i = 0; i < EFS_PACK_MAX_RECORDS; i++) records[i].len = 1;
    assert(!efs_pack_build(41,43,7,records,EFS_PACK_MAX_RECORDS,image,EFS_SEGMENT_MAX_SIZE,&size,refs));
    assert(!efs_pack_validate(image,size));
    /* Boundary payloads including bytes much smaller than a filesystem block. */
    unsigned lengths[] = {1,17,512,4096,7000};
    for (unsigned k = 0; k < sizeof(lengths)/sizeof(lengths[0]); k++) {
        for (unsigned i = 0; i < 16; i++) records[i].len = lengths[k];
        assert(!efs_pack_build(41,43,7,records,16,image,EFS_SEGMENT_MAX_SIZE,&size,refs));
        assert(!efs_pack_validate(image,size));
    }
    /* Version 2 holds records up to 16 KiB; a version 1 image keeps its 4 KiB
     * limit, so relabelling a larger record as version 1 is refused. */
    uint8_t *big = malloc(EFS_PACK_MAX_PAYLOAD); assert(big);
    for (unsigned i = 0; i < EFS_PACK_MAX_PAYLOAD; i++) big[i] = (uint8_t)(i * 7);
    for (unsigned i = 0; i < 7; i++) { records[i].len = EFS_PACK_MAX_PAYLOAD; records[i].data = big; }
    assert(!efs_pack_build(41,43,7,records,7,image,EFS_SEGMENT_MAX_SIZE,&size,refs));
    assert(!efs_pack_validate(image,size));
    for (unsigned i = 0; i < 7; i++) {
        const uint8_t *data; uint32_t len;
        assert(!efs_pack_read(image,size,&refs[i],&data,&len));
        assert(len == EFS_PACK_MAX_PAYLOAD && !memcmp(data,big,len));
    }
    memcpy(copy,image,size); put32(copy+8,1); resign(copy);
    assert(efs_pack_validate(copy,size)==EFS_ERR_PROTO);
    records[0].len = EFS_PACK_V1_MAX_PAYLOAD;
    assert(!efs_pack_build(41,43,7,records,1,image,EFS_SEGMENT_MAX_SIZE,&size,refs));
    memcpy(copy,image,size); put32(copy+8,1); resign(copy);
    assert(!efs_pack_validate(copy,size));
    for (unsigned i = 0; i < EFS_PACK_MAX_RECORDS; i++) { records[i].len = 512; records[i].data = payload; }
    free(big);
    assert(!efs_pack_build(41,43,7,records,16,image,EFS_SEGMENT_MAX_SIZE,&size,refs));
    /* Every authoritative identity component fences a wrong-record read. */
    struct efs_pack_ref bad = refs[0];
#define CHANGE(field) do { bad=refs[0]; bad.field++; expect_rejected(image,size,&bad); } while(0)
    CHANGE(ino); CHANGE(inode_generation); CHANGE(content_epoch); CHANGE(record_generation);
    CHANGE(segment_id); CHANGE(segment_generation); CHANGE(publication_group); CHANGE(offset); CHANGE(len);
    bad=refs[0]; bad.geometry += 8192; expect_rejected(image,size,&bad);
    bad=refs[0]; bad.digest[0] ^= 1; expect_rejected(image,size,&bad);
    /* Corrupt header, table, payload, footer and padded tail. */
    for (size_t i = 0; i < size; i += 97) {
        memcpy(copy,image,size); copy[i] ^= 1;
        expect_rejected(copy,size,&refs[0]);
    }
    /* Valid checksums must not disguise malformed range, duplicate or version. */
    memcpy(copy,image,size); put32(copy+EFS_PACK_HEADER_BYTES+32,UINT32_MAX); resign(copy);
    assert(efs_pack_validate(copy,size)==EFS_ERR_PROTO);
    memcpy(copy,image,size); memcpy(copy+EFS_PACK_HEADER_BYTES+EFS_PACK_RECORD_BYTES,
                                   copy+EFS_PACK_HEADER_BYTES,8); resign(copy);
    assert(efs_pack_validate(copy,size)==EFS_ERR_PROTO);
    memcpy(copy,image,size); put32(copy+8,3); resign(copy);
    assert(efs_pack_validate(copy,size)==EFS_ERR_PROTO);
    memcpy(copy,image,size); put32(copy+24,EFS_PACK_MAX_RECORDS+1); resign(copy);
    assert(efs_pack_validate(copy,size)==EFS_ERR_PROTO);
    assert(efs_pack_validate(image,size-1)==EFS_ERR_PROTO);
    assert(efs_pack_validate(NULL,size)==EFS_ERR_PROTO);
    /* Reject ambiguous cohorts, overlarge objects and capacity shortages atomically. */
    memcpy(copy,image,size); struct efs_pack_ref saved = refs[0]; size_t old_size=size;
    records[1].ino=records[0].ino;
    assert(efs_pack_build(41,43,7,records,16,image,EFS_SEGMENT_MAX_SIZE,&size,refs)==EFS_ERR_INVAL);
    assert(size==old_size && !memcmp(copy,image,size) && !memcmp(&saved,&refs[0],sizeof(saved)));
    records[1].ino=101;
    assert(efs_pack_build(41,43,7,records,16,image,size-1,&size,refs)==EFS_ERR_OVERFLOW);
    assert(!memcmp(copy,image,size));
    records[0].len=EFS_PACK_MAX_PAYLOAD+1;
    assert(efs_pack_build(41,43,7,records,16,image,EFS_SEGMENT_MAX_SIZE,&size,refs)==EFS_ERR_INVAL);
    records[0].len=512;
    assert(efs_pack_build(41,43,7,records,EFS_PACK_MAX_RECORDS,image,EFS_SEGMENT_MAX_SIZE,&size,refs)==EFS_ERR_OVERFLOW);
    assert(efs_pack_build(41,43,7,records,0,image,EFS_SEGMENT_MAX_SIZE,&size,refs)==EFS_ERR_INVAL);
    assert(efs_pack_build(41,43,7,records,EFS_PACK_MAX_RECORDS+1,image,EFS_SEGMENT_MAX_SIZE,&size,refs)==EFS_ERR_INVAL);
    free(image); free(copy); free(decoded); free(fragments);
    puts("packed object identity, EC recovery, corruption, bounds and atomic rejection PASS");
    return 0;
}
