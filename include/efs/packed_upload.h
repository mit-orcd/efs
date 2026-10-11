#ifndef EFS_PACKED_UPLOAD_H
#define EFS_PACKED_UPLOAD_H
#include "efs/pack_admission.h"
#include "efs/put_upload.h"
/* PUT: export:4, fragment:4, manifest-bytes:4, EFSUPLD1 manifest, payload.
 * GET: export:4, segment:8, generation:8, fragment:4, geometry:4.
 * GET reply: signed rc:4, payload-bytes:4, checksum:32, payload (success only). */
#define EFS_PACKED_PUT_HEADER 12u
#define EFS_PACKED_GET_HEADER 28u
#define EFS_PACKED_GET_REPLY 40u
static inline void efs_packed_wr64(uint8_t *p, uint64_t n)
{ for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(n >> (56 - 8 * i)); }
static inline uint64_t efs_packed_rd64(const uint8_t *p)
{ uint64_t n = 0; for (unsigned i = 0; i < 8; i++) n = (n << 8) | p[i]; return n; }
#endif
