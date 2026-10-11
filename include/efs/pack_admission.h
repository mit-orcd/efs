#ifndef EFS_PACK_ADMISSION_H
#define EFS_PACK_ADMISSION_H
#include "efs/pack_mapping.h"
#define EFS_PACK_ADMISSION_MAX 128u
#define EFS_PACK_UPLOAD_HEADER 16u
#define EFS_PACK_UPLOAD_MAX (EFS_PACK_UPLOAD_HEADER + EFS_PACK_OBJECT_MAX_BYTES + EFS_PACK_MAX_RECORDS * EFS_PACK_REF_BYTES)
/* Canonical transport manifest; exact length, version and all refs validated. */
int efs_pack_upload_encode(const struct efs_pack_object *, const struct efs_pack_ref *, uint32_t count, uint8_t *, size_t, size_t *);
int efs_pack_upload_decode(const uint8_t *, size_t, struct efs_pack_object *, struct efs_pack_ref *, uint32_t capacity, uint32_t *count);
/* Serialized owning-group apply only. The host must establish a fresh global
 * session view before admission; apply rechecks the durable local epoch.
 * These APIs do not authorize network PUT or certify storage ACKs by themselves.
 * All references are immutable, in canonical record order, within one group. */
int efs_pack_identity(const struct efs_opid *, uint32_t group, uint64_t *id, uint64_t *generation);
int efs_pack_admission_validate(const struct efs_pack_object *, const struct efs_pack_ref *, uint32_t count);
int efs_pack_admit(struct efs_kv *, const struct efs_pack_object *, const struct efs_pack_ref *, uint32_t count);
/* Status checks the exact immutable manifest, independent of mutable lifetime
 * state. Absence/floor proof is never an upload permit; caller requires ACTIVE
 * and its own pending record plus fresh session/storage fencing checks. */
int efs_pack_admission_get(struct efs_kv *, const struct efs_pack_object *, struct efs_pack_object *);
/* ACK calls require an actual durable storage result for this exact object,
 * checked by the coordinator before proposing. No delete ACK before all fences. */
/* Exact lifetime identity remains checkable after upload floor retirement. */
int efs_pack_lifetime_get(struct efs_kv *, const struct efs_pack_object *, struct efs_pack_object *);
/* Serialized local apply only AFTER coordinator proves completed global
 * revocation. This independently checks the durable local rejection floor. */
int efs_pack_admission_cancel_verified(struct efs_kv *, const struct efs_pack_object *);
int efs_pack_admission_ack(struct efs_kv *, const struct efs_pack_object *, uint32_t member_bit, int deletion);
/* Cancellation requires colocated authoritative completed global revocation
 * and local floor. Distributed hosts must use a separate verified coordinator. */
int efs_pack_admission_cancel(struct efs_kv *, const struct efs_pack_object *);
/* Retire the first completed upload permit once no PENDING references remain.
 * Keep shared-object lifetime metadata for all live files; advancing this floor
 * fences new PUT/publication retries without waiting for those files to die. */
int efs_pack_upload_is_retired(struct efs_kv *, const struct efs_pack_object *, int *);
#define EFS_PACK_UPLOAD_RETIRED 4u /* floor proof, never a data PUT permit */
int efs_pack_upload_retire(struct efs_kv *, const struct efs_pack_object *);
/* Only RETIRED, and only the first outstanding stream entry. Remove the object only after
 * durably advancing its upload floor; retain storage fence certificates. */
int efs_pack_admission_retire(struct efs_kv *, const struct efs_pack_object *);
#endif
