#ifndef EFS_MEMBER_STORE_H
#define EFS_MEMBER_STORE_H

/* D39 C2/C3: a member's store identity under <mdraft>.
 *   owner.lock    exclusive flock held for the process's life, taken before
 *                 anything else in the store; a second process stops "owned".
 *   boot.bin      the durable boot counter (incarnation): magic, version,
 *                 counter, checksum. Created once at store initialization
 *                 (counter 0); every start writes max(fresh, saved + 1) with
 *                 fsync before anything is advertised.
 *   identity.bin  written at initialization: the member id and configuration
 *                 digest the store belongs to, the formation state
 *                 (unformed / forming / formed) and the pair once learned.
 * identity.bin is the "initialized" mark: it is written after boot.bin, so
 * a store with boot.bin and no identity.bin crashed during initialization
 * and is refused (C13 3, fail-closed), as is a store holding Raft or KV data
 * but no identity (a store of another build). Files are replaced with
 * write-temp, fsync, rename, fsync-directory, so a failed start leaves them
 * as they were. */

#include "efs/cluster_conf.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_MEMBER_UNFORMED 0u
#define EFS_MEMBER_FORMING  1u
#define EFS_MEMBER_FORMED   2u

#define EFS_BOOT_FILE_LEN 32u
#define EFS_IDENTITY_FILE_LEN 72u

struct member_store {
    char dir[4096];
    int lockfd;
    uint32_t member_id;
    uint8_t digest[EFS_CLUSTER_DIGEST_LEN];
    uint32_t state;
    int pair_known;
    uint64_t salt;
    int created; /* this start initialized the store */
};

/* Take ownership of dir (created if missing) and load or initialize its
 * identity for member_id under digest. EFS_OK, or an error with err set;
 * on error ownership is released. */
int member_store_open(struct member_store *ms, const char *dir, uint32_t member_id,
                      const uint8_t digest[EFS_CLUSTER_DIGEST_LEN], char *err, size_t errlen);
/* Advance the boot counter: writes and returns max(fresh, saved + 1). */
int member_store_boot(struct member_store *ms, uint64_t fresh, uint64_t *incarnation,
                      char *err, size_t errlen);
/* Persist the learned pair (unformed -> forming) or the formed state.
 * A different pair than the persisted one is EFS_ERR_FENCED (never written). */
int member_store_learn_pair(struct member_store *ms, uint64_t salt, char *err, size_t errlen);
int member_store_mark_formed(struct member_store *ms, char *err, size_t errlen);
void member_store_close(struct member_store *ms);

/* Encodings, exposed for tests. */
void member_store_boot_encode(uint64_t counter, uint8_t out[EFS_BOOT_FILE_LEN]);
int member_store_boot_decode(const uint8_t *in, size_t len, uint64_t *counter);
const char *member_store_state_name(uint32_t state);

#endif
