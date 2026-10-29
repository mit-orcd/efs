#include "efs/packed_reader.h"
#include "efs/checksum.h"
#include <stdlib.h>
#include <string.h>
size_t efs_packed_read_scratch_size(const struct efs_pack_ref *ref)
{
    uint8_t wire[EFS_PACK_REF_BYTES];
    if (efs_pack_ref_encode(ref, wire)) return 0;
    return (size_t)ref->geometry * 5u / 2u;
}
int efs_packed_fragment_verify(const uint8_t *frag, uint32_t len,
    const uint8_t *reply_digest, const uint8_t *mapping_sum, int *which)
{
    uint8_t hash[EFS_HASH_SIZE];
    efs_hash(frag, len, hash);
    if (memcmp(hash, reply_digest, sizeof(hash))) { *which = 1; return EFS_ERR_CHECKSUM; }
    if (memcmp(hash, mapping_sum, sizeof(hash))) { *which = 2; return EFS_ERR_CHECKSUM; }
    return EFS_OK;
}
int efs_packed_read_base_scratch(const struct efs_pack_ref *ref, const efs_node_id_t nodes[3],
    const uint8_t sums[3][EFS_HASH_SIZE], efs_packed_fetch_fn fetch, void *context, uint8_t *out, uint32_t size,
    void *scratch, size_t capacity)
{
    uint8_t wire[EFS_PACK_REF_BYTES];
    if (!ref || !nodes || !sums || !fetch || !out || !efs_chunk_size_valid(size) ||
        ref->len > size || efs_pack_ref_encode(ref, wire)) return EFS_ERR_INVAL;
    for (unsigned i = 0; i < 3; i++) { if (!nodes[i]) return EFS_ERR_INVAL;
        for (unsigned j = 0; j < i; j++) if (nodes[i] == nodes[j]) return EFS_ERR_INVAL; }
    size_t needed = efs_packed_read_scratch_size(ref);
    if (!scratch || capacity < needed) return EFS_ERR_NOMEM;
    uintptr_t start = (uintptr_t)scratch, dest = (uintptr_t)out;
    if (start > UINTPTR_MAX - needed || dest > UINTPTR_MAX - size ||
        (start < dest + size && dest < start + needed)) return EFS_ERR_INVAL;
    uint32_t fl = ref->geometry / 2;
    uint8_t *storage = scratch;
    uint8_t *f[] = {storage, storage + fl, storage + 2 * fl}, *image = storage + 3 * fl;
    /* A transport may hand the fragment back in its own buffer (RDMA) instead
     * of receiving into the scratch slot; decode only reads fragments. */
    uint8_t *owned[EFS_NUM_FRAGMENTS] = {NULL, NULL, NULL};
    unsigned good[2], count = 0; int last = EFS_ERR_DECODE;
    for (unsigned i = 0; i < 3 && count < 2; i++) {
        const uint8_t *frag = NULL;
        int rc = fetch(context, nodes[i], ref->segment_id, ref->segment_generation, i,
                       ref->geometry, f[i], fl, sums[i], &frag, &owned[i]);
        if (rc == EFS_ERR_BUSY) { last = rc; break; } /* Preserve caller deadline. */
        if (rc) { last = rc; continue; }
        f[i] = (uint8_t *)frag;
        good[count++] = i;
    }
    int rc = last;
    if (count >= 2) {
        unsigned missing = 3 - good[0] - good[1];
        rc = efs_decode_segment(f, ref->geometry, good[0], good[1], missing, image, ref->geometry);
        if (rc) rc = EFS_ERR_DECODE;
        else {
            const uint8_t *payload; uint32_t len;
            rc = efs_pack_read(image, ref->geometry, ref, &payload, &len);
            if (!rc) { memset(out, 0, size); memcpy(out, payload, len); }
        }
    }
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) free(owned[i]);
    return rc;
}
int efs_packed_read_base(const struct efs_pack_ref *ref, const efs_node_id_t nodes[3],
    const uint8_t sums[3][EFS_HASH_SIZE], efs_packed_fetch_fn fetch, void *context, uint8_t *out, uint32_t size)
{
    size_t needed = efs_packed_read_scratch_size(ref);
    if (!needed) return EFS_ERR_INVAL;
    void *scratch = malloc(needed);
    if (!scratch) return EFS_ERR_NOMEM;
    int rc = efs_packed_read_base_scratch(ref, nodes, sums, fetch, context, out, size, scratch, needed);
    free(scratch);
    return rc;
}
