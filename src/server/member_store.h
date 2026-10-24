#ifndef EFS_MEMBER_STORE_H
#define EFS_MEMBER_STORE_H

/* A member's store identity under <meta root>/mdraft (D39 C2, adapted to
 * the seed/--join startup by W153):
 *   owner.lock    exclusive flock held for the process's life, taken before
 *                 anything else in the store; a second process stops "owned".
 *   boot.bin      the durable boot counter (incarnation, the Raft boot id):
 *                 magic, version, counter, checksum. Created once at store
 *                 initialization (counter 0); every start writes
 *                 max(fresh, saved + 1) with fsync before anything is served.
 *   identity.bin  the node id the store belongs to, the store token (K33:
 *                 64 random bits minted once at initialization; a wiped
 *                 store is a new token), the formation state (unformed /
 *                 formed) and, once formed, the newest committed membership
 *                 revision it learned (efs/membership.h), checksummed.
 * identity.bin is the "initialized" mark: it is written after boot.bin, so
 * a store with boot.bin and no identity.bin crashed during initialization
 * and is refused, as is a store holding Raft or KV data but no identity (a
 * store of another build). Files are replaced with write-temp, fsync,
 * rename, fsync-directory, so a failed start leaves them as they were. */

#include "efs/membership.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_MEMBER_UNFORMED 0u
#define EFS_MEMBER_FORMED   1u

#define EFS_BOOT_FILE_LEN 32u
#define EFS_IDENTITY_FILE_LEN (32u + EFS_MEMBERSHIP_MAX_LEN + 32u)

struct member_store {
    char dir[4096];
    int lockfd;
    uint32_t member_id;
    uint64_t token;
    uint32_t state;
    struct efs_membership members; /* valid when state == EFS_MEMBER_FORMED */
    int created; /* this start initialized the store */
};

/* Take ownership of dir (created if missing) and load or initialize its
 * identity for member_id. EFS_OK, or an error with err set; on error
 * ownership is released. A store of another node id is EFS_ERR_FENCED. */
int member_store_open(struct member_store *ms, const char *dir, uint32_t member_id,
                      char *err, size_t errlen);
/* Advance the boot counter: writes and returns max(fresh, saved + 1). */
int member_store_boot(struct member_store *ms, uint64_t fresh, uint64_t *incarnation,
                      char *err, size_t errlen);
/* Persist a committed membership revision naming this store (its member
 * id with its token): unformed -> formed, or a later revision of the stored
 * one. The same or an earlier revision of the stored one is EFS_OK and
 * changes nothing; a record of another cluster, one that diverges from the
 * stored one, or one that does not name this store's token at its id is
 * EFS_ERR_FENCED and never written. */
int member_store_learn(struct member_store *ms, const struct efs_membership *m,
                       char *err, size_t errlen);
void member_store_close(struct member_store *ms);

/* Encodings, exposed for tests. */
void member_store_boot_encode(uint64_t counter, uint8_t out[EFS_BOOT_FILE_LEN]);
int member_store_boot_decode(const uint8_t *in, size_t len, uint64_t *counter);
const char *member_store_state_name(uint32_t state);

#endif
