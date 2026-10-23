#ifndef EFS_MEMBERSHIP_H
#define EFS_MEMBERSHIP_H

/* W153: the cluster membership committed at formation (raft-mkfs). It is
 * the only source of the member count and of each metadata group's voters
 * (efs_md_group_voters over count); the HELLO node table never is.
 *
 * Wire/record encoding, carried by MKFS (group 0) and SALT (group 2) and
 * persisted in the member store's identity.bin:
 *   [version:1][count:1] then per member, in id order 1..count:
 *   [id:4 BE][port:2 BE][alen:1][addr:alen]
 * A record is valid only when the version is EFS_MEMBERSHIP_VERSION, count
 * is EFS_FORMATION_MIN..EFS_MAX_NODES, member i carries node id i+1, every
 * port is nonzero, every address is 1..EFS_MEMBER_ADDR_MAX printable
 * characters, no two members share an endpoint and nothing trails. */

#include "efs/common.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_MEMBERSHIP_VERSION 1u
/* 2+1 needs three distinct storage members. */
#define EFS_FORMATION_MIN 3u
#define EFS_MEMBER_ADDR_MAX 63u
#define EFS_MEMBERSHIP_MAX_LEN (2u + EFS_MAX_NODES * (7u + EFS_MEMBER_ADDR_MAX))

struct efs_member {
    efs_node_id_t id;
    uint16_t port;
    char addr[EFS_MEMBER_ADDR_MAX + 1];
};

struct efs_membership {
    uint32_t count;
    struct efs_member m[EFS_MAX_NODES];
};

/* EFS_OK and *len, or EFS_ERR_INVAL for an invalid membership / short out. */
int efs_membership_encode(const struct efs_membership *ms, uint8_t *out, size_t cap,
                          size_t *len);
/* EFS_OK, EFS_ERR_UNSUPPORTED (another version) or EFS_ERR_DECODE. */
int efs_membership_decode(const uint8_t *in, size_t len, struct efs_membership *out);
int efs_membership_valid(const struct efs_membership *ms);
int efs_membership_equal(const struct efs_membership *a, const struct efs_membership *b);
const struct efs_member *efs_membership_find(const struct efs_membership *ms,
                                             efs_node_id_t id);
/* Formation input: the joined node table at this moment. Needs node ids
 * exactly 1..n with n >= EFS_FORMATION_MIN and distinct endpoints; else
 * EFS_ERR_INVAL with err naming who joined and who is missing. */
int efs_membership_from_nodes(const struct efs_node *nodes, uint32_t count,
                              struct efs_membership *out, char *err, size_t errlen);

#endif
