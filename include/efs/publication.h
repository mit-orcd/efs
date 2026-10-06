#ifndef EFS_PUBLICATION_H
#define EFS_PUBLICATION_H
#include "efs/meta_apply.h"
#include "efs/protocol.h"
/* Canonical operation identity and immutable intent. Host timestamps and routing are excluded.
 * A changed base, fence, range, size or object is a NEW publication, never a retry. */
int efs_publication_digest(const struct efs_meta_pub *, uint8_t out[EFS_HASH_SIZE]);
int efs_publication_from_rec(const struct efs_chunk_rec *, uint64_t size,
                             struct efs_meta_pub *);
#endif
