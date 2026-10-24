#ifndef EFS_MEMBERSHIP_H
#define EFS_MEMBERSHIP_H

/* W153/K33: the committed cluster membership. It is the only source of
 * the member count and of each metadata group's voters (efs_md_group_voters
 * over count); the HELLO node table never is. See
 * docs/status/k33-membership-protocol-20261023.md (M1-M3).
 *
 * A record is one revision of an ordered sequence under a stable cluster
 * identity: formation commits revision 1 (count == formed); every addition
 * commits the previous record plus exactly one member with id count + 1.
 * revision = count - formed + 1.
 *
 * Wire/record encoding, carried by MKFS (group 0), SALT and MEMBERS
 * (groups 0 and 2) and persisted in the member store's identity.bin:
 *   [version:1][count:1][formed:1][cluster:8 BE] then per member, in id
 *   order 1..count: [id:4 BE][port:2 BE][token:8 BE][alen:1][addr:alen]
 * A record is valid only when the version is EFS_MEMBERSHIP_VERSION, formed
 * is EFS_FORMATION_MIN..count, count <= EFS_MAX_NODES, cluster != 0,
 * member i carries node id i+1 and a nonzero token, every port is nonzero,
 * every address is 1..EFS_MEMBER_ADDR_MAX printable characters, no two
 * members share an endpoint or a token and nothing trails. */

#include "efs/common.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_MEMBERSHIP_VERSION 2u
/* 2+1 needs three distinct storage members. */
#define EFS_FORMATION_MIN 3u
#define EFS_MEMBER_ADDR_MAX 63u
#define EFS_MEMBERSHIP_MAX_LEN (11u + EFS_MAX_NODES * (15u + EFS_MEMBER_ADDR_MAX))

struct efs_member {
    efs_node_id_t id;
    uint16_t port;
    uint64_t token; /* the member's store token (member_store.h) */
    char addr[EFS_MEMBER_ADDR_MAX + 1];
};

struct efs_membership {
    uint32_t count;
    uint32_t formed;  /* formation count: revision 1 has count == formed */
    uint64_t cluster; /* stable cluster identity: the committed MKFS salt */
    struct efs_member m[EFS_MAX_NODES];
};

/* EFS_OK and *len, or EFS_ERR_INVAL for an invalid membership / short out. */
int efs_membership_encode(const struct efs_membership *ms, uint8_t *out, size_t cap,
                          size_t *len);
/* EFS_OK, EFS_ERR_UNSUPPORTED (another version) or EFS_ERR_DECODE. */
int efs_membership_decode(const uint8_t *in, size_t len, struct efs_membership *out);
int efs_membership_valid(const struct efs_membership *ms);
int efs_membership_equal(const struct efs_membership *a, const struct efs_membership *b);
/* 1 if a equals b or is a later revision of b: same cluster and formation,
 * b's members are a's first b->count members. */
int efs_membership_extends(const struct efs_membership *a, const struct efs_membership *b);
const struct efs_member *efs_membership_find(const struct efs_membership *ms,
                                             efs_node_id_t id);
/* The member at this endpoint, or NULL. */
const struct efs_member *efs_membership_find_ep(const struct efs_membership *ms,
                                                const char *addr, uint16_t port);
/* The next revision: ms plus member count + 1 at addr:port with token.
 * EFS_ERR_INVAL (err set) when the result would be invalid: full, the
 * endpoint or token already a member. */
int efs_membership_plus(const struct efs_membership *ms, const char *addr, uint16_t port,
                        uint64_t token, struct efs_membership *out, char *err, size_t errlen);
/* Formation input: the joined node table at this moment. Takes the
 * contiguous ids 1..k (k >= EFS_FORMATION_MIN) with their endpoints and
 * tokens; nodes after a gap join later as additions. EFS_ERR_INVAL with err
 * naming who joined and who is missing when k < EFS_FORMATION_MIN, or for a
 * shared endpoint, a missing token or a duplicate id. */
int efs_membership_from_nodes(const struct efs_node *nodes, uint32_t count, uint64_t cluster,
                              struct efs_membership *out, char *err, size_t errlen);

#endif
