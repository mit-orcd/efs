#include "efs/packed_reader.h"
#include "efs/checksum.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *fragments[3];
static uint8_t sums[3][32];
static int missing = -1, corrupt = -1, busy, calls, owned_mode;
static uint64_t expected_id, expected_gen;
static int fetch(void *context, efs_node_id_t node, uint64_t id, uint64_t generation,
    uint32_t fi, uint32_t geometry, uint8_t *out, uint32_t capacity,
    const uint8_t *expected_sum, const uint8_t **fragp, uint8_t **ownedp)
{
    assert(!context && node == fi + 1 && id == expected_id && generation == expected_gen && capacity == geometry / 2);
    /* The reader passes this fragment's mapping digest down, unmodified. */
    assert(!memcmp(expected_sum, sums[fi], 32));
    calls++;
    *fragp = NULL; *ownedp = NULL;
    if (busy) return EFS_ERR_BUSY;
    if ((int)fi == missing) return EFS_ERR_NOT_FOUND;
    uint8_t *dst = out;
    if (owned_mode) {
        /* RDMA-style handover at a nonzero offset: the reader must use the
         * returned fragment pointer, never assume an offset, and free it. */
        uint8_t *buf = malloc(64 + capacity);
        assert(buf);
        memset(buf, 0, 64);
        dst = buf + 64;
        *ownedp = buf;
    }
    memcpy(dst, fragments[fi], capacity);
    if ((int)fi == corrupt) dst[0] ^= 1;
    uint8_t h[EFS_HASH_SIZE]; efs_hash(dst, capacity, h);
    if (memcmp(h, expected_sum, sizeof(h))) { free(*ownedp); *ownedp = NULL; return EFS_ERR_CHECKSUM; }
    *fragp = dst;
    return EFS_OK;
}
static void verify_full_read(const struct efs_pack_ref *refs, const efs_node_id_t nodes[3],
    uint8_t *output, uint8_t *scratch, size_t scratch_size, const uint8_t *payload)
{
    for (unsigned i = 0; i < 16; i++) {
        memset(output, 0xa5, EFS_MIN_CHUNK_SIZE); calls = 0;
        assert(!efs_packed_read_base_scratch(refs + i, nodes, (const uint8_t (*)[32])sums, fetch, NULL,
            output, EFS_MIN_CHUNK_SIZE, scratch, scratch_size));
        assert(!memcmp(output, payload, 512));
        for (unsigned j = 512; j < EFS_MIN_CHUNK_SIZE; j++) assert(!output[j]);
        assert(calls >= 2 && calls <= 3);
    }
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
    const efs_node_id_t nodes[3] = {1, 2, 3};
    for (unsigned i = 0; i < 3; i++) fragments[i] = storage + geometry / 2 * i;
    assert(!efs_encode_segment(image, geometry, geometry, fragments));
    for (unsigned i = 0; i < 3; i++) efs_hash(fragments[i], geometry / 2, sums[i]);
    assert(efs_packed_read_base_scratch(refs, nodes, (const uint8_t (*)[32])sums, fetch, NULL, output,
        sizeof(output), scratch, scratch_size - 1) == EFS_ERR_NOMEM);
    assert(!calls && !memcmp(output, saved, sizeof(output)));
    assert(efs_packed_read_base_scratch(refs, nodes, (const uint8_t (*)[32])sums, fetch, NULL, scratch,
        sizeof(output), scratch, scratch_size) == EFS_ERR_INVAL);
    assert(!calls);
    for (int failed = -1; failed < 3; failed++) {
        missing = failed; corrupt = -1;
        verify_full_read(refs, nodes, output, scratch, scratch_size, payload);
    }
    /* Same matrix with the fetch handing back owned buffers at a +64 offset. */
    missing = -1; owned_mode = 1;
    verify_full_read(refs, nodes, output, scratch, scratch_size, payload);
    /* Owned buffer whose fragment fails its digest is freed by the fetch and
     * the reader falls back to the remaining pair. */
    corrupt = 0;
    verify_full_read(refs, nodes, output, scratch, scratch_size, payload);
    owned_mode = 0; corrupt = -1;
    missing = -1; corrupt = 0;
    assert(!efs_packed_read_base(refs, nodes, (const uint8_t (*)[32])sums, fetch, NULL, output, sizeof(output)));
    missing = 1; memset(output, 0x5a, sizeof(output)); memcpy(saved, output, sizeof(output));
    assert(efs_packed_read_base(refs, nodes, (const uint8_t (*)[32])sums, fetch, NULL, output, sizeof(output)) != EFS_OK);
    assert(!memcmp(output, saved, sizeof(output)));
    missing = corrupt = -1; busy = 1; calls = 0;
    assert(efs_packed_read_base(refs, nodes, (const uint8_t (*)[32])sums, fetch, NULL, output, sizeof(output)) == EFS_ERR_BUSY && calls == 1);
    assert(!memcmp(output, saved, sizeof(output))); busy = 0;
    struct efs_pack_ref bad = refs[0]; bad.digest[0] ^= 1;
    assert(efs_packed_read_base(&bad, nodes, (const uint8_t (*)[32])sums, fetch, NULL, output, sizeof(output)) != EFS_OK);
    assert(!memcmp(output, saved, sizeof(output)));
    /* AK: one hash, two authorities, each catching corruption independently. */
    uint8_t frag_copy[EFS_SEGMENT_MAX_SIZE / 2];
    memcpy(frag_copy, fragments[0], geometry / 2);
    uint8_t wrong[32]; memcpy(wrong, sums[0], 32); wrong[0] ^= 1;
    int which = 0;
    assert(!efs_packed_fragment_verify(frag_copy, geometry / 2, sums[0], sums[0], &which));
    frag_copy[0] ^= 1;
    assert(efs_packed_fragment_verify(frag_copy, geometry / 2, sums[0], sums[0], &which) == EFS_ERR_CHECKSUM
           && which == 1); /* the reply digest is the first to refuse */
    memcpy(frag_copy, fragments[0], geometry / 2);
    assert(efs_packed_fragment_verify(frag_copy, geometry / 2, sums[0], wrong, &which) == EFS_ERR_CHECKSUM
           && which == 2); /* a stale/wrong mapping digest is caught separately */
    /* GETCHUNKS packed records: the reference must name this file's chunk
     * zero record; a malformed one is refused before any fetch. */
    static struct efs_chunk_rec rec;
    rec.ino = 101; rec.chunk_generation = 23; rec.publish_flags = EFS_CHUNK_REC_F_PACKED;
    assert(!efs_pack_ref_encode(&refs[0], rec.packed_ref));
    rec.read_view.count = 1; rec.read_view.chunk_size = EFS_MIN_CHUNK_SIZE; rec.read_view.parts[0].len = 512;
    assert(!efs_chunk_rec_view_valid(&rec));
    rec.ino++; assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO); rec.ino--;
    rec.chunk_generation++; assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO); rec.chunk_generation--;
    rec.packed_ref[0] ^= 1; assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO); rec.packed_ref[0] ^= 1;
    rec.chunk_index = 1; assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO); rec.chunk_index = 0;
    rec.publish_flags = EFS_CHUNK_REC_F_RESERVED; assert(efs_chunk_rec_view_valid(&rec) == EFS_ERR_PROTO);
    static struct efs_chunk_entry ce; ce.generation = 23; ce.packed = refs[0]; uint64_t key = efs_chunk_read_key(&ce);
    ce.packed.segment_generation++; assert(key != efs_chunk_read_key(&ce));
    free(scratch); free(storage); puts("packed reader: borrowed scratch, pre-fetch capacity refusal, every degraded pair, owned-buffer handover, one hash two authorities, immutable identity, zero fill, deadline, unchanged failure output and cache identity PASS");
}
