#ifndef EFS_CLUSTER_CONF_H
#define EFS_CLUSTER_CONF_H

/* D39 C1: the configured member list (cluster.conf), the same file on every
 * daemon and given to `efs-mgmt raft-mkfs --cluster`. One member per line,
 * `<id> <ipv4>:<port>`; `#` starts a comment; blank lines and whitespace
 * are ignored; line order is irrelevant. Refused (fail-closed) before
 * anything serves: a non-canonical decimal id, a duplicate id or endpoint,
 * fewer than 3 or more than EFS_MAX_NODES members, ids that are not exactly
 * 1..N, an endpoint that is not a canonical dotted IPv4 address with a
 * decimal port 1..65535, the wildcard or broadcast address, a host name, a
 * missing port, an extra field.
 *
 * Canonical records: "efs-cluster-config 1\n" then "<id> <addr>:<port>\n"
 * per member in id order. The digest is SHA-256 of those bytes; it never
 * includes the salt. Membership is fixed: ids never derive from file order
 * or from who is reachable. */

#include "efs/common.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_CLUSTER_MIN_MEMBERS 3
#define EFS_CLUSTER_MAX_MEMBERS EFS_MAX_NODES
#define EFS_CLUSTER_DIGEST_LEN 32
#define EFS_CLUSTER_CONF_MAX_BYTES 65536
#define EFS_CLUSTER_CANONICAL_HEADER "efs-cluster-config 1\n"

struct efs_cluster_member {
    uint32_t id;    /* 1..n */
    char addr[16];  /* canonical dotted IPv4 */
    uint16_t port;  /* 1..65535 */
};

struct efs_cluster_conf {
    uint32_t n;
    struct efs_cluster_member m[EFS_CLUSTER_MAX_MEMBERS]; /* m[id-1] */
    uint8_t digest[EFS_CLUSTER_DIGEST_LEN];
};

/* EFS_OK, or EFS_ERR_INVAL with a one-line reason in err. */
int efs_cluster_conf_parse(const char *text, size_t len, struct efs_cluster_conf *out,
                           char *err, size_t errlen);
int efs_cluster_conf_load(const char *path, struct efs_cluster_conf *out,
                          char *err, size_t errlen);
/* Canonical record bytes (no NUL); returns their length, or 0 if cap is short. */
size_t efs_cluster_conf_canonical(const struct efs_cluster_conf *c, char *buf, size_t cap);
/* The member whose configured endpoint equals addr:port (canonical text
 * compare), or 0. A non-canonical addr never matches. */
uint32_t efs_cluster_conf_member_at(const struct efs_cluster_conf *c, const char *addr,
                                    uint16_t port);
/* EFS_OK when s is a canonical dotted IPv4 address that may name a member
 * (not 0.0.0.0, not 255.255.255.255); *ip receives it in network order. */
int efs_cluster_addr_canonical(const char *s, uint32_t *ip);

void efs_sha256(const void *data, size_t len, uint8_t out[32]);
/* First 12 hex digits, for log lines and refusals; out holds 13 bytes. */
void efs_cluster_digest_short(const uint8_t digest[EFS_CLUSTER_DIGEST_LEN], char out[13]);

/* Process-wide configuration for code that needs the member list without a
 * server handle (efs-mgmt, the declaration). NULL until set. */
const struct efs_cluster_conf *efs_cluster_conf_current(void);
void efs_cluster_conf_set_current(const struct efs_cluster_conf *c);

#endif
