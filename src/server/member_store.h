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
 *   identity.bin  the store token (K33: 64 random bits minted once at
 *                 initialization; a wiped store is a new token), the state
 *                 and what it holds, checksummed:
 *                   joining   a store started with --join: no node id, no
 *                             cluster, no endpoint yet;
 *                   seed      a store that created a cluster (efsd
 *                             ADDR:PORT STORAGE): node id 1, the cluster
 *                             identity minted at creation, its endpoint;
 *                   promised  the formation proposal it promised (M4'):
 *                             its node id and endpoint in that record;
 *                   formed    the newest committed membership revision it
 *                             learned (efs/membership.h).
 *                 Node ids are never an operator input: id 1 is the seed,
 *                 formation allocates the others, an addition count + 1.
 * identity.bin is the "initialized" mark: it is written after boot.bin, so
 * a store with boot.bin at counter 0 and no identity.bin crashed during
 * initialization, before its token left the process, and is initialized
 * again (an interrupted join or seed start is retried with the same
 * command). A store with no identity.bin but a boot counter above 0 (it
 * started before), an unreadable one, or Raft or KV data (a store of
 * another build) is refused. member_store_open lists every state's
 * outcome. Files are replaced with write-temp, fsync, rename,
 * fsync-directory, so a failed start leaves them as they were. */

#include "efs/membership.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_MEMBER_JOINING  0u
#define EFS_MEMBER_FORMED   1u
#define EFS_MEMBER_SEED     2u
#define EFS_MEMBER_PROMISED 3u

#define EFS_BOOT_FILE_LEN 32u
#define EFS_IDENTITY_REC_AT 112u
#define EFS_IDENTITY_FILE_LEN (EFS_IDENTITY_REC_AT + EFS_MEMBERSHIP_MAX_LEN + 32u)

struct member_store {
    char dir[4096];
    int lockfd;
    uint32_t member_id; /* 0 while joining */
    uint64_t token;
    uint32_t state;
    uint64_t cluster;   /* 0 while joining */
    char addr[EFS_MEMBER_ADDR_MAX + 1]; /* this store's endpoint; empty while joining */
    uint16_t port;
    struct efs_membership members; /* promised: the proposal; formed: the newest revision */
    int created; /* this start initialized the store */
};

/* Take ownership of dir (created if missing) and load its identity. A
 * missing identity is initialized: with seed set as a seed at addr:port
 * (node id 1, a fresh cluster identity), otherwise as a joining store.
 * EFS_OK, or an error with err set; on error ownership is released. */
int member_store_open(struct member_store *ms, const char *dir, int seed, const char *addr,
                      uint16_t port, char *err, size_t errlen);
/* M4': persist the formation proposal (revision 1) that names this
 * store's token: joining -> promised at the proposed id; a seed only as
 * node 1 at its endpoint in its own cluster. The same proposal again, or
 * a formed store that already holds it, is EFS_OK; any other proposal is
 * EFS_ERR_FENCED and never written. */
int member_store_promise(struct member_store *ms, const struct efs_membership *proposal,
                         char *err, size_t errlen);
/* Advance the boot counter: writes and returns max(fresh, saved + 1). */
int member_store_boot(struct member_store *ms, uint64_t fresh, uint64_t *incarnation,
                      char *err, size_t errlen);
/* Persist a committed membership revision naming this store's token
 * (at its node id and endpoint once it has them): joining, seed or
 * promised -> formed (a promised store only for a record extending its
 * proposal, a seed only in its cluster), or a later revision of the
 * stored one. The same or an earlier revision of the stored one is EFS_OK
 * and changes nothing; a record of another cluster, one that diverges from
 * the stored one, or one that does not name this store is EFS_ERR_FENCED
 * and never written. */
int member_store_learn(struct member_store *ms, const struct efs_membership *m,
                       char *err, size_t errlen);
void member_store_close(struct member_store *ms);
#if EFS_FAULTS
/* K33 test hook (efsd sets it to server_test_member_exit): called at
 * "before-identity", initialization's boot.bin written, identity.bin not. */
extern void (*member_store_fault_exit)(const char *point);
#endif

/* Pure record codecs. A failed decode leaves *ms unchanged (including
 * caller-owned path and lock). No filesystem access or synchronization. */
int member_store_identity_encode(const struct member_store *ms, uint8_t out[EFS_IDENTITY_FILE_LEN]);
int member_store_identity_decode(const uint8_t *buf, size_t len, struct member_store *ms,
                                  char *err, size_t errlen);
/* Encodings, exposed for tests. */
void member_store_boot_encode(uint64_t counter, uint8_t out[EFS_BOOT_FILE_LEN]);
int member_store_boot_decode(const uint8_t *in, size_t len, uint64_t *counter);
const char *member_store_state_name(uint32_t state);

#endif
