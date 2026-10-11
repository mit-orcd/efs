#include "efs/packed_reader.h"
#include "efs/checksum.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *fragments[3];
static int missing = -1, corrupt = -1, busy, calls;
static uint64_t expected_id, expected_gen;
static int fetch(void *context, efs_node_id_t node, uint64_t id, uint64_t generation,
    uint32_t fi, uint32_t geometry, uint8_t *out, uint32_t capacity)
{
    assert(!context && node == fi + 1 && id == expected_id && generation == expected_gen && capacity == geometry / 2);
    calls++; if (busy) return EFS_ERR_BUSY;
    if ((int)fi == missing) return EFS_ERR_NOT_FOUND;
    memcpy(out, fragments[fi], capacity); if ((int)fi == corrupt) out[0] ^= 1; return EFS_OK;
}
int main(void)
{
    uint8_t payload[512] = {97}, image[EFS_SEGMENT_MAX_SIZE], output[EFS_MIN_CHUNK_SIZE], saved[EFS_MIN_CHUNK_SIZE];
    struct efs_pack_record records[16]; struct efs_pack_ref refs[16];
    for (unsigned i = 0; i < 16; i++) records[i] = (struct efs_pack_record){101 + i * 2, 7, 0, 23 + i, 512, payload};
    size_t geometry; assert(!efs_pack_build(41, 43, 0, records, 16, image, sizeof(image), &geometry, refs));
    expected_id = 41; expected_gen = 43; uint8_t *storage = malloc(geometry * 3 / 2); assert(storage);
    size_t scratch_size = efs_packed_read_scratch_size(refs);
    assert(scratch_size == geometry * 5 / 2);
    uint8_t *scratch = malloc(scratch_size); assert(scratch);
    memset(output, 0x5a, sizeof(output)); memcpy(saved, output, sizeof(output)); calls = 0;
    uint8_t sums[3][32]; const efs_node_id_t nodes[3] = {1, 2, 3};
    for (unsigned i = 0; i < 3; i++) fragments[i] = storage + geometry / 2 * i;
    assert(!efs_encode_segment(image, geometry, geometry, fragments));
    for (unsigned i = 0; i < 3; i++) efs_hash(fragments[i], geometry / 2, sums[i]);
    assert(efs_packed_read_base_scratch(refs, nodes, sums, fetch, NULL, output,
        sizeof(output), scratch, scratch_size - 1) == EFS_ERR_NOMEM);
    assert(!calls && !memcmp(output, saved, sizeof(output)));
    assert(efs_packed_read_base_scratch(refs, nodes, sums, fetch, NULL, scratch,
        sizeof(output), scratch, scratch_size) == EFS_ERR_INVAL);
    assert(!calls);
    for (int failed = -1; failed < 3; failed++) {
        missing = failed; corrupt = -1;
        for (unsigned i = 0; i < 16; i++) {
            memset(output, 0xa5, sizeof(output)); calls = 0;
            assert(!efs_packed_read_base_scratch(refs + i, nodes, sums, fetch, NULL,
                output, sizeof(output), scratch, scratch_size));
            assert(!memcmp(output, payload, 512));
            for (unsigned j = 512; j < sizeof(output); j++) assert(!output[j]);
            assert(calls >= 2 && calls <= 3);
        }
    }
    missing = -1; corrupt = 0;
    assert(!efs_packed_read_base(refs, nodes, sums, fetch, NULL, output, sizeof(output)));
    missing = 1; memset(output, 0x5a, sizeof(output)); memcpy(saved, output, sizeof(output));
    assert(efs_packed_read_base(refs, nodes, sums, fetch, NULL, output, sizeof(output)) != EFS_OK);
    assert(!memcmp(output, saved, sizeof(output)));
    missing = corrupt = -1; busy = 1; calls = 0;
    assert(efs_packed_read_base(refs, nodes, sums, fetch, NULL, output, sizeof(output)) == EFS_ERR_BUSY && calls == 1);
    assert(!memcmp(output, saved, sizeof(output))); busy = 0;
    struct efs_pack_ref bad = refs[0]; bad.digest[0] ^= 1;
    assert(efs_packed_read_base(&bad, nodes, sums, fetch, NULL, output, sizeof(output)) != EFS_OK);
    assert(!memcmp(output, saved, sizeof(output)));
    struct efs_chunk_rec rec = {.ino = 101, .file_generation = 7, .chunk_generation = 23,
        .publish_flags = EFS_CHUNK_REC_F_PACKED_BASE, .packed = refs[0]};
    rec.read_view.count = 1; rec.read_view.chunk_size = EFS_MIN_CHUNK_SIZE; rec.read_view.parts[0].len = 512;
    assert(!efs_chunk_rec_view_valid(&rec)); rec.file_generation++;
    assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO); rec.file_generation--;
    rec.publish_flags = 0; assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO);
    struct efs_chunk_entry ce = {.generation = 23, .packed = refs[0]}; uint64_t key = efs_chunk_read_key(&ce);
    ce.packed.segment_generation++; assert(key != efs_chunk_read_key(&ce));
    free(scratch); free(storage); puts("packed reader: borrowed scratch, pre-fetch capacity refusal, every degraded pair, checksum fallback, immutable identity, zero fill, deadline, unchanged failure output and cache identity PASS");
}
