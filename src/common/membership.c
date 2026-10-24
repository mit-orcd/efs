/* W153/K33: the committed membership record. See efs/membership.h. */
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
    if (!ms || ms->count > EFS_MAX_NODES || ms->formed < EFS_FORMATION_MIN ||
        ms->formed > ms->count || !ms->cluster)
        return 0;
    for (uint32_t i = 0; i < ms->count; i++) {
        const struct efs_member *m = &ms->m[i];
        if (m->id != i + 1 || !m->port || !m->token ||
            !addr_ok(m->addr, strnlen(m->addr, sizeof(m->addr))) ||
            strnlen(m->addr, sizeof(m->addr)) == sizeof(m->addr))
            return 0;
        for (uint32_t j = 0; j < i; j++)
            if ((ms->m[j].port == m->port && !strcmp(ms->m[j].addr, m->addr)) ||
                ms->m[j].token == m->token)
                return 0;
    }
    return 1;
}

static void put_be(uint8_t *p, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)v;
}

static uint64_t get_be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = v << 8 | p[i];
    return v;
}

int efs_membership_encode(const struct efs_membership *ms, uint8_t *out, size_t cap,
                          size_t *len)
{
    size_t off = 11;
    if (!out || !len || !efs_membership_valid(ms) || cap < off)
        return EFS_ERR_INVAL;
    out[0] = (uint8_t)EFS_MEMBERSHIP_VERSION;
    out[1] = (uint8_t)ms->count;
    out[2] = (uint8_t)ms->formed;
    put_be(out + 3, ms->cluster, 8);
    for (uint32_t i = 0; i < ms->count; i++) {
        const struct efs_member *m = &ms->m[i];
        size_t alen = strlen(m->addr);
        if (cap - off < 15 + alen)
            return EFS_ERR_INVAL;
        put_be(out + off, m->id, 4);
        put_be(out + off + 4, m->port, 2);
        put_be(out + off + 6, m->token, 8);
        out[off + 14] = (uint8_t)alen;
        memcpy(out + off + 15, m->addr, alen);
        off += 15 + alen;
    }
    *len = off;
    return EFS_OK;
}

int efs_membership_decode(const uint8_t *in, size_t len, struct efs_membership *out)
{
    size_t off = 11;
    if (!in || !out || len < 1)
        return EFS_ERR_DECODE;
    if (in[0] != EFS_MEMBERSHIP_VERSION)
        return EFS_ERR_UNSUPPORTED;
    if (len < off)
        return EFS_ERR_DECODE;
    memset(out, 0, sizeof(*out));
    out->count = in[1];
    out->formed = in[2];
    out->cluster = get_be(in + 3, 8);
    if (out->count > EFS_MAX_NODES || out->formed < EFS_FORMATION_MIN || out->formed > out->count)
        return EFS_ERR_DECODE;
    for (uint32_t i = 0; i < out->count; i++) {
        struct efs_member *m = &out->m[i];
        if (len - off < 15)
            return EFS_ERR_DECODE;
        m->id = (efs_node_id_t)get_be(in + off, 4);
        m->port = (uint16_t)get_be(in + off + 4, 2);
        m->token = get_be(in + off + 6, 8);
        size_t alen = in[off + 14];
        if (alen > EFS_MEMBER_ADDR_MAX || len - off - 15 < alen)
            return EFS_ERR_DECODE;
        memcpy(m->addr, in + off + 15, alen);
        m->addr[alen] = '\0';
        if (!addr_ok(m->addr, alen))
            return EFS_ERR_DECODE;
        off += 15 + alen;
    }
    if (off != len || !efs_membership_valid(out))
        return EFS_ERR_DECODE;
    return EFS_OK;
}

static int member_eq(const struct efs_member *a, const struct efs_member *b)
{
    return a->id == b->id && a->port == b->port && a->token == b->token &&
           !strcmp(a->addr, b->addr);
}

int efs_membership_equal(const struct efs_membership *a, const struct efs_membership *b)
{
    return a && b && a->count == b->count && efs_membership_extends(a, b);
}

int efs_membership_extends(const struct efs_membership *a, const struct efs_membership *b)
{
    if (!a || !b || a->cluster != b->cluster || a->formed != b->formed ||
        a->count < b->count || a->count > EFS_MAX_NODES)
        return 0;
    for (uint32_t i = 0; i < b->count; i++)
        if (!member_eq(&a->m[i], &b->m[i]))
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

const struct efs_member *efs_membership_find_ep(const struct efs_membership *ms,
                                                const char *addr, uint16_t port)
{
    if (!ms || !addr || ms->count > EFS_MAX_NODES)
        return NULL;
    for (uint32_t i = 0; i < ms->count; i++)
        if (ms->m[i].port == port && !strcmp(ms->m[i].addr, addr))
            return &ms->m[i];
    return NULL;
}

int efs_membership_plus(const struct efs_membership *ms, const char *addr, uint16_t port,
                        uint64_t token, struct efs_membership *out, char *err, size_t errlen)
{
    if (!efs_membership_valid(ms) || !out || !addr) {
        mb_err(err, errlen, "no valid membership to extend");
        return EFS_ERR_INVAL;
    }
    if (ms->count >= EFS_MAX_NODES) {
        mb_err(err, errlen, "the cluster has the maximum of %d members", EFS_MAX_NODES);
        return EFS_ERR_INVAL;
    }
    *out = *ms;
    struct efs_member *m = &out->m[out->count];
    memset(m, 0, sizeof(*m));
    m->id = out->count + 1;
    m->port = port;
    m->token = token;
    snprintf(m->addr, sizeof(m->addr), "%s", addr);
    out->count++;
    if (!efs_membership_valid(out)) {
        mb_err(err, errlen, "node %u at %s:%u (token %016llx) cannot be member %u: its "
               "endpoint or store token is already a member, or the endpoint is unusable",
               m->id, addr, port, (unsigned long long)token, m->id);
        return EFS_ERR_INVAL;
    }
    return EFS_OK;
}

int efs_membership_from_nodes(const struct efs_node *nodes, uint32_t count, uint64_t cluster,
                              struct efs_membership *out, char *err, size_t errlen)
{
    const struct efs_node *by_id[EFS_MAX_NODES + 1] = {0};
    char joined[1024] = "", missing[256] = "";
    size_t jl = 0, ml = 0;
    uint32_t n = 0;

    if (!nodes || !out || !cluster) {
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
    }
    while (n < EFS_MAX_NODES && by_id[n + 1])
        n++;
    for (uint32_t id = 1; id <= EFS_MAX_NODES; id++) {
        if (by_id[id] && jl < sizeof(joined))
            jl += (size_t)snprintf(joined + jl, sizeof(joined) - jl, "%snode %u at %s:%u",
                                   jl ? ", " : "", id, by_id[id]->addr, by_id[id]->port);
        if (!by_id[id] && id <= EFS_FORMATION_MIN && ml < sizeof(missing))
            ml += (size_t)snprintf(missing + ml, sizeof(missing) - ml, "%snode %u",
                                   ml ? ", " : "", id);
    }
    if (n < EFS_FORMATION_MIN) {
        mb_err(err, errlen, "formation needs at least %u storage members with node ids 1..n; "
               "joined: %s; missing: %s", EFS_FORMATION_MIN, jl ? joined : "none", missing);
        return EFS_ERR_INVAL;
    }
    out->count = n;
    out->formed = n;
    out->cluster = cluster;
    for (uint32_t id = 1; id <= n; id++) {
        struct efs_member *m = &out->m[id - 1];
        size_t alen = strnlen(by_id[id]->addr, sizeof(by_id[id]->addr));
        if (!by_id[id]->port || !addr_ok(by_id[id]->addr, alen)) {
            mb_err(err, errlen, "node %u advertises an unusable endpoint '%.*s:%u'", id,
                   (int)alen, by_id[id]->addr, by_id[id]->port);
            return EFS_ERR_INVAL;
        }
        if (!by_id[id]->token) {
            mb_err(err, errlen, "node %u at %.*s:%u has not announced its store token", id,
                   (int)alen, by_id[id]->addr, by_id[id]->port);
            return EFS_ERR_INVAL;
        }
        m->id = id;
        m->port = by_id[id]->port;
        m->token = by_id[id]->token;
        memcpy(m->addr, by_id[id]->addr, alen);
        m->addr[alen] = '\0';
        for (uint32_t j = 1; j < id; j++)
            if (out->m[j - 1].port == m->port && !strcmp(out->m[j - 1].addr, m->addr)) {
                mb_err(err, errlen, "nodes %u and %u advertise the same endpoint %s:%u; "
                       "formation needs distinct storage members", j, id, m->addr, m->port);
                return EFS_ERR_INVAL;
            }
            else if (out->m[j - 1].token == m->token) {
                mb_err(err, errlen, "nodes %u and %u announce the same store token; "
                       "formation needs distinct storage members", j, id);
                return EFS_ERR_INVAL;
            }
    }
    return EFS_OK;
}
