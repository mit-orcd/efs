#ifndef EFS_PACKED_SEGMENT_H
#define EFS_PACKED_SEGMENT_H
#include "efs/erasure.h"

/* Immutable packed segment format (W125). Only the background packer builds
 * segments; callers establish durable admission and ownership separately.
 * All integers on disk are big-endian; no native struct is persisted.
 * Version 2 raises the record limit from version 1's 4 KiB to 16 KiB (the
 * initial packing threshold); version 1 images stay readable. */
#define EFS_PACK_VERSION 2u
#define EFS_PACK_V1_MAX_PAYLOAD 4096u
#define EFS_PACK_HEADER_BYTES 96u
#define EFS_PACK_RECORD_BYTES 72u
#define EFS_PACK_REF_BYTES 96u
#define EFS_PACK_MAX_RECORDS 512u
#define EFS_PACK_MAX_PAYLOAD 16384u
struct efs_pack_record {
    efs_ino_t ino;
    uint64_t inode_generation, content_epoch, record_generation;
    uint32_t len;
    const uint8_t *data;
};
struct efs_pack_ref {
    efs_ino_t ino;
    uint64_t inode_generation, content_epoch, record_generation;
    uint64_t segment_id, segment_generation;
    uint32_t geometry, publication_group, offset, len;
    uint8_t digest[EFS_HASH_SIZE];
};
/* All input/output buffers must be distinct. A rejected build changes neither
 * the output image nor references. Image capacity must hold rounded geometry.
 * Records are unique by inode; the caller coalesces within one publication
 * group, never by a directory-wide/global transaction. */
int efs_pack_build(uint64_t segment_id, uint64_t segment_generation,
                   uint32_t publication_group, const struct efs_pack_record *,
                   uint32_t count, uint8_t *image, size_t capacity,
                   size_t *geometry, struct efs_pack_ref *refs);
int efs_pack_validate(const uint8_t *image, size_t geometry);
/* Returned bytes alias the validated immutable image; caller pins its lifetime.
 * The expected reference is authoritative metadata, not a record from the body.
 * No bytes escape on wrong identity, epoch, digest, or malformed geometry. */
int efs_pack_read(const uint8_t *image, size_t geometry,
                  const struct efs_pack_ref *, const uint8_t **data, uint32_t *len);
/* Records of a validated image, by table index (compaction enumerates a
 * segment's records to find the files it still holds). */
int efs_pack_record_count(const uint8_t *image, size_t geometry, uint32_t *count);
int efs_pack_record_at(const uint8_t *image, size_t geometry, uint32_t i, struct efs_pack_ref *out);
int efs_pack_ref_encode(const struct efs_pack_ref *, uint8_t out[EFS_PACK_REF_BYTES]);
int efs_pack_ref_decode(const uint8_t *, size_t len, struct efs_pack_ref *);
#endif
