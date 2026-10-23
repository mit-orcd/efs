/* W153: committed formation membership. See efs/membership.h. */
#include "efs/membership.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void mb_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int addr_ok(const char *a, size_t n)
{
    if (n < 1 || n > EFS_MEMBER_ADDR_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)a[i] <= 0x20 || (unsigned char)a[i] >= 0x7f)
            return 0;
    return 1;
}

int efs_membership_valid(const struct efs_membership *ms)
{
    if (!ms || ms->count < EFS_FORMATION_MIN || ms->count > EFS_MAX_NODES)
        return 0;
    for (uint32_t i = 0; i < ms->count; i++) {
        const struct efs_member *m = &ms->m[i];
        if (m->id != i + 1 || !m->port ||
            !addr_ok(m->addr, strnlen(m->addr, sizeof(m->addr))) ||
            strnlen(m->addr, sizeof(m->addr)) == sizeof(m->addr))
            return 0;
        for (uint32_t j = 0; j < i; j++)
            if (ms->m[j].port == m->port && !strcmp(ms->m[j].addr, m->addr))
                return 0;
    }
    return 1;
}

int efs_membership_encode(const struct efs_membership *ms, uint8_t *out, size_t cap,
                          size_t *len)
{
    size_t off = 2;
    if (!out || !len || !efs_membership_valid(ms) || cap < 2)
        return EFS_ERR_INVAL;
    out[0] = (uint8_t)EFS_MEMBERSHIP_VERSION;
    out[1] = (uint8_t)ms->count;
    for (uint32_t i = 0; i < ms->count; i++) {
        const struct efs_member *m = &ms->m[i];
        size_t alen = strlen(m->addr);
        if (cap - off < 7 + alen)
            return EFS_ERR_INVAL;
        out[off] = (uint8_t)(m->id >> 24);
        out[off + 1] = (uint8_t)(m->id >> 16);
        out[off + 2] = (uint8_t)(m->id >> 8);
        out[off + 3] = (uint8_t)m->id;
        out[off + 4] = (uint8_t)(m->port >> 8);
        out[off + 5] = (uint8_t)m->port;
        out[off + 6] = (uint8_t)alen;
        memcpy(out + off + 7, m->addr, alen);
        off += 7 + alen;
    }
    *len = off;
    return EFS_OK;
}

int efs_membership_decode(const uint8_t *in, size_t len, struct efs_membership *out)
{
    size_t off = 2;
    if (!in || !out || len < 2)
        return EFS_ERR_DECODE;
    if (in[0] != EFS_MEMBERSHIP_VERSION)
        return EFS_ERR_UNSUPPORTED;
    memset(out, 0, sizeof(*out));
    out->count = in[1];
    if (out->count < EFS_FORMATION_MIN || out->count > EFS_MAX_NODES)
        return EFS_ERR_DECODE;
    for (uint32_t i = 0; i < out->count; i++) {
        struct efs_member *m = &out->m[i];
        if (len - off < 7)
            return EFS_ERR_DECODE;
        m->id = (uint32_t)in[off] << 24 | (uint32_t)in[off + 1] << 16 |
                (uint32_t)in[off + 2] << 8 | in[off + 3];
        m->port = (uint16_t)(in[off + 4] << 8 | in[off + 5]);
        size_t alen = in[off + 6];
        if (alen > EFS_MEMBER_ADDR_MAX || len - off - 7 < alen)
            return EFS_ERR_DECODE;
        memcpy(m->addr, in + off + 7, alen);
        m->addr[alen] = '\0';
        if (!addr_ok(m->addr, alen))
            return EFS_ERR_DECODE;
        off += 7 + alen;
    }
    if (off != len || !efs_membership_valid(out))
        return EFS_ERR_DECODE;
    return EFS_OK;
}

int efs_membership_equal(const struct efs_membership *a, const struct efs_membership *b)
{
    if (!a || !b || a->count != b->count)
        return 0;
    for (uint32_t i = 0; i < a->count && i < EFS_MAX_NODES; i++)
        if (a->m[i].id != b->m[i].id || a->m[i].port != b->m[i].port ||
            strcmp(a->m[i].addr, b->m[i].addr))
            return 0;
    return 1;
}

const struct efs_member *efs_membership_find(const struct efs_membership *ms,
                                             efs_node_id_t id)
{
    if (!ms || id < 1 || id > ms->count || ms->count > EFS_MAX_NODES)
        return NULL;
    return &ms->m[id - 1];
}

int efs_membership_from_nodes(const struct efs_node *nodes, uint32_t count,
                              struct efs_membership *out, char *err, size_t errlen)
{
    const struct efs_node *by_id[EFS_MAX_NODES + 1] = {0};
    char joined[1024] = "", missing[256] = "";
    size_t jl = 0, ml = 0;
    uint32_t n = 0;

    if (!nodes || !out) {
        mb_err(err, errlen, "no node table");
        return EFS_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < count && i < EFS_MAX_NODES; i++) {
        efs_node_id_t id = nodes[i].id;
        if (id < 1 || id > EFS_MAX_NODES) {
            mb_err(err, errlen, "node table holds node id %u outside 1..%d", id, EFS_MAX_NODES);
            return EFS_ERR_INVAL;
        }
        if (by_id[id]) {
            mb_err(err, errlen, "node table holds node id %u twice", id);
            return EFS_ERR_INVAL;
        }
        by_id[id] = &nodes[i];
        if (id > n)
            n = id;
    }
    for (uint32_t id = 1; id <= EFS_MAX_NODES; id++) {
        if (by_id[id] && jl < sizeof(joined))
            jl += (size_t)snprintf(joined + jl, sizeof(joined) - jl, "%snode %u at %s:%u",
                                   jl ? ", " : "", id, by_id[id]->addr, by_id[id]->port);
        if (!by_id[id] && (id <= n || id <= EFS_FORMATION_MIN) && ml < sizeof(missing))
            ml += (size_t)snprintf(missing + ml, sizeof(missing) - ml, "%snode %u",
                                   ml ? ", " : "", id);
    }
    if (ml) {
        mb_err(err, errlen, "formation needs at least %u storage members with node ids 1..n "
               "and no gaps; joined: %s; missing: %s", EFS_FORMATION_MIN,
               jl ? joined : "none", missing);
        return EFS_ERR_INVAL;
    }
    out->count = n;
    for (uint32_t id = 1; id <= n; id++) {
        struct efs_member *m = &out->m[id - 1];
        size_t alen = strnlen(by_id[id]->addr, sizeof(by_id[id]->addr));
        if (!by_id[id]->port || !addr_ok(by_id[id]->addr, alen)) {
            mb_err(err, errlen, "node %u advertises an unusable endpoint '%.*s:%u'", id,
                   (int)alen, by_id[id]->addr, by_id[id]->port);
            return EFS_ERR_INVAL;
        }
        m->id = id;
        m->port = by_id[id]->port;
        memcpy(m->addr, by_id[id]->addr, alen);
        m->addr[alen] = '\0';
        for (uint32_t j = 1; j < id; j++)
            if (out->m[j - 1].port == m->port && !strcmp(out->m[j - 1].addr, m->addr)) {
                mb_err(err, errlen, "nodes %u and %u advertise the same endpoint %s:%u; "
                       "formation needs distinct storage members", j, id, m->addr, m->port);
                return EFS_ERR_INVAL;
            }
    }
    return EFS_OK;
}
