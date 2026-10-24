#ifndef EFS_MEMBER_VIEW_H
#define EFS_MEMBER_VIEW_H

/* K33 live client view: a mounted client installs a later committed
 * membership revision without a remount (docs/status/k33-membership-protocol-
 * 20261023.md M7, "Client view").
 *
 * The client keeps the record it mounted with (or last installed) and asks a
 * member for its committed record (EFS_MSG_MEMBERSHIP_READ). A record is
 * installed only when it is a valid later revision of the installed one: same
 * cluster, same formation, and the installed members are its first members
 * unchanged (efs_membership_extends). Installing appends the new members to
 * the client's node table; rows already in the table are never rewritten, so
 * a reader that saw the old count keeps a consistent table, and the count is
 * published last with release order. Old data keeps its recorded placement:
 * nothing here touches a published chunk. Anything else is refused and the
 * installed record stays (fail-closed). */

#include "efs/common.h"
#include "efs/membership.h"
#include <stddef.h>
#include <stdint.h>

enum efs_view_verdict {
    EFS_VIEW_SAME = 0,      /* the installed revision */
    EFS_VIEW_ADVANCE,       /* a later revision of the installed record: install */
    EFS_VIEW_OLDER,         /* an earlier revision (the member has not learned yet) */
    EFS_VIEW_OTHER_CLUSTER, /* another cluster identity */
    EFS_VIEW_DIVERGENT,     /* same cluster, not on the installed record's chain */
    EFS_VIEW_INVALID,       /* not a valid record */
};

const char *efs_view_verdict_name(int verdict);
/* revision = count - formed + 1 (membership.h); 0 for an invalid record. */
uint32_t efs_member_view_revision(const struct efs_membership *m);
/* Judge a candidate record against the installed one (both decoded). */
int efs_member_view_judge(const struct efs_membership *cur, const struct efs_membership *cand);

/* Reply of EFS_MSG_MEMBERSHIP_READ: EFS_OK and the decoded record, the
 * server's rc (EFS_ERR_UNFORMED before formation), or EFS_ERR_DECODE for a
 * malformed reply (short, length beyond the record, trailing bytes). */
int efs_member_view_parse_reply(const void *payload, uint32_t len, struct efs_membership *out);

/* The node-table rows for record m: rows[0..count) must each be m's member
 * at that id and endpoint (else EFS_ERR_FENCED, err names the row). Rows for
 * m's members the table lacks are written at rows[count..] in id order and
 * *new_count is the table size with them. Rows below count are not written,
 * so concurrent readers of the old count are unaffected. */
int efs_member_view_extend_rows(struct efs_node *rows, uint32_t count,
                                const struct efs_membership *m, uint32_t *new_count,
                                char *err, size_t errlen);
/* extend_rows into the live table, then store *count with release order. */
int efs_member_view_publish(struct efs_node *rows, uint32_t *count,
                            const struct efs_membership *m, char *err, size_t errlen);

#endif
