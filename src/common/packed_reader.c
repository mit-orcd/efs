#include "efs/packed_reader.h"
#include "efs/checksum.h"
#include <stdlib.h>
#include <string.h>
int efs_packed_read_base(const struct efs_pack_ref *ref, const efs_node_id_t nodes[3],
    const uint8_t sums[3][EFS_HASH_SIZE], efs_packed_fetch_fn fetch, void *context, uint8_t *out, uint32_t size)
{
    uint8_t wire[EFS_PACK_REF_BYTES];
    if (!ref || !nodes || !sums || !fetch || !out || !efs_chunk_size_valid(size) ||
        ref->len > size || efs_pack_ref_encode(ref, wire)) return EFS_ERR_INVAL;
    for (unsigned i = 0; i < 3; i++) { if (!nodes[i]) return EFS_ERR_INVAL;
        for (unsigned j = 0; j < i; j++) if (nodes[i] == nodes[j]) return EFS_ERR_INVAL; }
    uint32_t fl = ref->geometry / 2;
    uint8_t *storage = malloc(3u * fl + ref->geometry); if (!storage) return EFS_ERR_NOMEM;
    uint8_t *f[] = {storage, storage + fl, storage + 2 * fl}, *image = storage + 3 * fl;
    unsigned good[2], count = 0; int last = EFS_ERR_DECODE;
    for (unsigned i = 0; i < 3 && count < 2; i++) {
        int rc = fetch(context, nodes[i], ref->segment_id, ref->segment_generation, i, ref->geometry, f[i], fl);
        if (rc == EFS_ERR_BUSY) { last = rc; break; } /* Preserve caller deadline. */
        if (rc) { last = rc; continue; }
        uint8_t hash[EFS_HASH_SIZE]; efs_hash(f[i], fl, hash);
        if (memcmp(hash, sums[i], sizeof(hash))) { last = EFS_ERR_CHECKSUM; continue; }
        good[count++] = i;
    }
    if (count < 2) { free(storage); return last; }
    unsigned missing = 3 - good[0] - good[1];
    int rc = efs_decode_segment(f, ref->geometry, good[0], good[1], missing, image, ref->geometry);
    if (rc) { free(storage); return EFS_ERR_DECODE; }
    const uint8_t *payload; uint32_t len;
    rc = efs_pack_read(image, ref->geometry, ref, &payload, &len);
    if (!rc) { memset(out, 0, size); memcpy(out, payload, len); }
    free(storage); return rc;
}
