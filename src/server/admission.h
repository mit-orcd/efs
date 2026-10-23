#ifndef EFS_ADMISSION_H
#define EFS_ADMISSION_H

/* D39 admission (docs/status/d39-consolidated-model-20261023.md C2-C7,
 * C10, C11): the local identity tuple, per-member bindings (the high-water
 * mark of accepted declarations), the incarnation fence words, the
 * formation states and the `complete` predicate, and the role-specific
 * predicates every connection and frame passes.
 *
 * Locking: one binding leaf lock (adm_mu). It never takes h->mu, never does
 * I/O and is never held across Raft disk I/O. Identity events (pair
 * learned, formation record installed) persist first under a separate
 * identity mutex, then publish under the leaf lock; the leaf lock release
 * is the ordering point (C6). */

#include "efs/cluster_conf.h"
#include "efs/protocol.h"
#include <stddef.h>
#include <stdint.h>

struct member_store;

#define ADM_ACCEPT     0
#define ADM_IDEMPOTENT 1
#define ADM_REFUSE     2

/* Group index: 0 = group 0 (MKFS), 1 = group 2 (SALT). */
#define ADM_NGROUPS 2

/* Set up for member local_id of conf at incarnation inc. store (may be
 * NULL in tests: nothing is persisted) carries the persisted pair. */
int adm_init(const struct efs_cluster_conf *conf, uint32_t local_id, uint64_t inc,
             struct member_store *store);
void adm_reset(void); /* tests */
int adm_ready(void);
const struct efs_cluster_conf *adm_conf(void);
uint32_t adm_local_id(void);
void adm_local_identity(struct efs_identity *out);

/* C5, evaluated under the leaf lock. why (may be NULL) names the refusal. */
int adm_predicate(const struct efs_identity *d, char *why, size_t whylen);
/* Gate hooks (efs_admission_ops). */
int adm_declare(const struct efs_msg_client_hello *h, struct efs_msg_client_hello_reply *rep,
                struct efs_decl_gate *g);
int adm_frame(struct efs_decl_gate *g, uint8_t type, const void *payload, uint32_t len);
extern const struct efs_admission_ops adm_ops;
/* The declarer's check of a declaration reply from the member it intended
 * to reach (C4, C5). EFS_OK: the connection may be used. */
int adm_check_reply(uint32_t intended, const struct efs_msg_client_hello_reply *rep,
                    char *why, size_t whylen);

/* C7 (b) independent Raft control from a tagged frame: EFS_OK or a refusal
 * (EFS_ERR_STALE stale, EFS_ERR_UNREGISTERED redeclare required,
 * EFS_ERR_FENCED pair conflict/downgrade, EFS_ERR_INVAL not a peer). */
int adm_check_control(const struct efs_identity *tag);
/* C7 (b) for one wire Raft frame on a declared peer connection: the tag
 * check plus sender, receiver and incarnation equal to the tag. */
int adm_check_raft_frame(const struct efs_identity *tag, const uint8_t *frame, uint32_t len);
/* C7 (a) new application work from a peer tag. */
int adm_check_app(const struct efs_identity *tag);
/* C11 pre-filter: the binding's incarnation (0: none). May refuse early,
 * never admits. */
uint64_t adm_fence_word(uint32_t member);
int adm_has_binding(uint32_t member);

/* C10 formation. install: MKFS (group index 0) or SALT (1) applied locally
 * (an install or a matching repeat): learn the pair if unknown, counter + 1,
 * known[g] = pair. observe: the pair of a group read by FORMATION_READ or
 * re-derived at restart from the applied record; counts as an identity event
 * only when it teaches the pair. EFS_ERR_FENCED: the pair conflicts with the
 * local one (the caller stops the daemon; nothing was written). */
int adm_formation_install(int gi, uint64_t salt, const uint8_t digest[EFS_CLUSTER_DIGEST_LEN]);
int adm_formation_observe(int gi, uint64_t salt, const uint8_t digest[EFS_CLUSTER_DIGEST_LEN]);
int adm_complete(void);
int adm_known(int gi, uint64_t *salt);
int adm_pair(uint64_t *salt);
uint32_t adm_state(void); /* EFS_MEMBER_* as persisted */
/* Bumped by every identity event; pooled outgoing connections declared at
 * an older generation are retired (C2). */
uint64_t adm_generation(void);
uint64_t adm_counter(void);
uint64_t adm_incarnation(void);

const char *adm_channel_name(uint8_t channel);
uint8_t adm_channel(int remote_known, uint64_t remote_salt, int local_known, uint64_t local_salt);

#endif
