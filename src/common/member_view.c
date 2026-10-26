/* K33 live client view. See efs/member_view.h. */
#include "efs/member_view.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <string.h>

const char *efs_view_verdict_name(int verdict)
{
    switch (verdict) {
    case EFS_VIEW_SAME: return "same revision";
    case EFS_VIEW_ADVANCE: return "later revision";
    case EFS_VIEW_OLDER: return "older revision";
    case EFS_VIEW_OTHER_CLUSTER: return "another cluster";
    case EFS_VIEW_DIVERGENT: return "divergent record";
    default: return "invalid record";
    }
}

uint32_t efs_member_view_revision(const struct efs_membership *m)
{
    return m && efs_membership_valid(m) ? m->count - m->formed + 1 : 0;
}

int efs_member_view_judge(const struct efs_membership *cur, const struct efs_membership *cand)
{
    if (!cur || !cand || !efs_membership_valid(cand) || !efs_membership_valid(cur))
        return EFS_VIEW_INVALID;
    if (cand->cluster != cur->cluster)
        return EFS_VIEW_OTHER_CLUSTER;
    if (efs_membership_extends(cand, cur))
        return cand->count == cur->count ? EFS_VIEW_SAME : EFS_VIEW_ADVANCE;
    if (efs_membership_extends(cur, cand))
        return EFS_VIEW_OLDER;
    /* Another formation count, or a member replaced on the way. */
    return EFS_VIEW_DIVERGENT;
}

int efs_member_view_parse_reply(const void *payload, uint32_t len, struct efs_membership *out)
{
    struct efs_msg_membership_reply r;
    if (!payload || !out || len < EFS_MEMBERSHIP_REPLY_HDR || len > sizeof(r))
        return EFS_ERR_DECODE;
    memset(&r, 0, sizeof(r));
    memcpy(&r, payload, len);
    if (r.rc != EFS_OK)
        return r.rc < 0 && r.len == 0 && len == EFS_MEMBERSHIP_REPLY_HDR ? r.rc : EFS_ERR_DECODE;
    if (r.len == 0 || r.len > EFS_MEMBERSHIP_MAX_LEN || len != EFS_MEMBERSHIP_REPLY_HDR + r.len)
        return EFS_ERR_DECODE;
    int rc = efs_membership_decode(r.record, r.len, out);
    if (rc != EFS_OK)
        return rc;
    return efs_membership_valid(out) ? EFS_OK : EFS_ERR_DECODE;
}

int efs_member_view_extend_rows(struct efs_node *rows, uint32_t count,
                                const struct efs_membership *m, uint32_t *new_count,
                                char *err, size_t errlen)
{
    uint32_t n = count;
    if (err && errlen)
        err[0] = '\0';
    if (!rows || !new_count || !efs_membership_valid(m) || count > EFS_MAX_NODES)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < count; i++) {
        const struct efs_member *p = efs_membership_find(m, rows[i].id);
        if (!p || p->port != rows[i].port || strcmp(p->addr, rows[i].addr) != 0) {
            if (err && errlen)
                snprintf(err, errlen, "node %u at %s:%u is not member %u of the record%s%s:%u",
                         rows[i].id, rows[i].addr, rows[i].port, rows[i].id,
                         p ? ", which names " : "", p ? p->addr : "", p ? p->port : 0);
            return EFS_ERR_FENCED;
        }
        for (uint32_t j = 0; j < i; j++)
            if (rows[j].id == rows[i].id) {
                if (err && errlen)
                    snprintf(err, errlen, "node %u appears twice in the table", rows[i].id);
                return EFS_ERR_FENCED;
            }
    }
    for (uint32_t k = 0; k < m->count; k++) {
        const struct efs_member *p = &m->m[k];
        int have = 0;
        for (uint32_t i = 0; i < count && !have; i++)
            have = rows[i].id == p->id;
        if (have)
            continue;
        if (n >= EFS_MAX_NODES)
            return EFS_ERR_INVAL;
        struct efs_node *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        r->id = p->id;
        snprintf(r->addr, sizeof(r->addr), "%s", p->addr);
        r->port = p->port;
        r->token = p->token;
    }
    *new_count = n;
    return EFS_OK;
}

int efs_member_view_publish(struct efs_node *rows, uint32_t *count,
                            const struct efs_membership *m, char *err, size_t errlen)
{
    uint32_t n = 0;
    if (!count)
        return EFS_ERR_INVAL;
    int rc = efs_member_view_extend_rows(rows, __atomic_load_n(count, __ATOMIC_ACQUIRE), m, &n,
                                         err, errlen);
    if (rc == EFS_OK)
        __atomic_store_n(count, n, __ATOMIC_RELEASE);
    return rc;
}
