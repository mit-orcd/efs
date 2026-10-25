#ifndef EFS_FRAG_RECORD_H
#define EFS_FRAG_RECORD_H

#include "efs/common.h"

/* I25 identity-bound fragment record (fragment format 2).
 *
 * Every fragment file on a storage root is written and read with O_DIRECT
 * (D42) and laid out as
 *     payload padded to 4 KiB || 4 KiB tail page (the record starts the page)
 * This is the only layout. A file of any other size, such as the removed
 * buffered layout (payload || bare 128-byte record), is refused: GET answers
 * INTEGRITY and GC will not touch it (tests/test_store_gc.c).
 * The record names the immutable object the payload belongs to and carries
 * two digests:
 *     payload_digest = BLAKE3(payload)                 (the wire digest)
 *     bound_digest   = BLAKE3(record bytes [0, 88))    (identity + payload)
 * Because bytes [56, 88) are payload_digest, bound_digest covers the
 * immutable identity together with the payload. Verification checks, in
 * order: magic/version/kind/reserved (EFS_ERR_FRAG_FORMAT), bound_digest
 * (EFS_ERR_CHECKSUM), every identity field against the identity the reader
 * derived from its request (EFS_ERR_FRAG_IDENTITY); the caller then checks
 * BLAKE3(payload) == payload_digest (EFS_ERR_CHECKSUM). No earlier format is
 * read: a store holding one is refused at startup. Layout, big-endian:
 *
 *   0  magic "EFSFRAG\0"        8
 *   8  version (2)              2
 *  10  kind (1 chunk, 2 seg)    2
 *  12  record bytes (128)       4
 *  16  space                    4   export id | pack group
 *  20  fragment index           4
 *  24  object                   8   ino | segment id
 *  32  generation               8   chunk generation | segment generation
 *  40  chunk index              4   0 for a segment
 *  44  payload length           4
 *  48  coding profile           4   1 = fixed 2+1
 *  52  reserved, zero           4
 *  56  payload digest          32
 *  88  bound digest            32
 * 120  reserved, zero           8
 */
#define EFS_FRAG_RECORD_BYTES   128u
#define EFS_FRAG_RECORD_VERSION 2u
#define EFS_FRAG_KIND_CHUNK     1u
#define EFS_FRAG_KIND_SEGMENT   2u
#define EFS_FRAG_PROFILE_K2F1   1u

struct efs_frag_ident {
    uint32_t kind;
    uint32_t space;
    uint32_t fragment_index;
    uint32_t chunk_index;
    uint32_t payload_len;
    uint32_t coding_profile;
    uint64_t object;
    uint64_t generation;
};

/* Fills out with a complete record. EFS_ERR_INVAL for an incomplete identity
 * (zero object/generation/length, unknown kind or profile). */
int efs_frag_record_encode(const struct efs_frag_ident *id,
                           const uint8_t payload_digest[EFS_HASH_SIZE],
                           uint8_t out[EFS_FRAG_RECORD_BYTES]);

/* Self-consistency only (format + bound digest). *id and digest may be NULL. */
int efs_frag_record_parse(const uint8_t *rec, size_t len,
                          struct efs_frag_ident *id,
                          uint8_t payload_digest[EFS_HASH_SIZE]);

/* parse + every identity field equal to *expect. On EFS_ERR_FRAG_IDENTITY,
 * *field (when non-NULL) names the first mismatching field. */
int efs_frag_record_verify(const uint8_t *rec, size_t len,
                           const struct efs_frag_ident *expect,
                           uint8_t payload_digest[EFS_HASH_SIZE],
                           const char **field);

/* Record + payload: verify the record, then BLAKE3(payload) against it. */
int efs_frag_verify(const uint8_t *payload, uint32_t len, const uint8_t *rec,
                    size_t rec_len, const struct efs_frag_ident *expect,
                    uint8_t payload_digest[EFS_HASH_SIZE], const char **field);

#endif
