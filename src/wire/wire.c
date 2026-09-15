#include "efs/wire.h"
#include "efs/raft.h"
#include <arpa/inet.h>
#include <string.h>

int efs_wire_frame_check_nlen(uint32_t nlen)
{
    if (nlen == 0 || nlen > EFS_MSG_MAX_LEN)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

int efs_wire_frame_size(uint32_t payload_len, uint32_t *frame_len)
{
    if (!frame_len)
        return EFS_ERR_INVAL;
    if (payload_len > EFS_MSG_MAX_LEN - 1)
        return EFS_ERR_PROTO;
    *frame_len = 5u + payload_len;
    return EFS_OK;
}

int efs_wire_frame_header(uint8_t type, uint32_t payload_len, uint8_t out[5])
{
    uint32_t nlen;
    uint32_t be;
    if (!out)
        return EFS_ERR_INVAL;
    if (payload_len > EFS_MSG_MAX_LEN - 1)
        return EFS_ERR_PROTO;
    nlen = 1u + payload_len;
    be = htonl(nlen);
    memcpy(out, &be, 4);
    out[4] = type;
    return EFS_OK;
}

int efs_wire_frame_encode(uint8_t type, const void *p1, uint32_t n1,
                          const void *p2, uint32_t n2,
                          uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    uint32_t payload;
    uint32_t frame;
    int rc;
    uint32_t n1u = (n1 > 0 && p1) ? n1 : 0;
    uint32_t n2u = (n2 > 0 && p2) ? n2 : 0;

    if (!out)
        return EFS_ERR_INVAL;
    if (n1u > UINT32_MAX - n2u)
        return EFS_ERR_PROTO;
    payload = n1u + n2u;
    rc = efs_wire_frame_size(payload, &frame);
    if (rc != EFS_OK)
        return rc;
    if (out_cap < frame)
        return EFS_ERR_NOMEM;
    rc = efs_wire_frame_header(type, payload, out);
    if (rc != EFS_OK)
        return rc;
    if (n1u)
        memcpy(out + 5, p1, n1u);
    if (n2u)
        memcpy(out + 5 + n1u, p2, n2u);
    if (out_len)
        *out_len = frame;
    return EFS_OK;
}

int efs_wire_frame_decode(const uint8_t *in, uint32_t in_len,
                          uint8_t *type, const uint8_t **payload, uint32_t *plen)
{
    uint32_t be;
    uint32_t nlen;
    int rc;
    if (!in || in_len < 5)
        return EFS_ERR_PROTO;
    memcpy(&be, in, 4);
    nlen = ntohl(be);
    rc = efs_wire_frame_check_nlen(nlen);
    if (rc != EFS_OK)
        return rc;
    if (in_len != 4u + nlen)
        return EFS_ERR_PROTO;
    if (type)
        *type = in[4];
    if (plen)
        *plen = nlen - 1;
    if (payload)
        *payload = (nlen == 1) ? NULL : (in + 5);
    return EFS_OK;
}

int efs_wire_pack(const void *msg, uint32_t len, void *out, uint32_t out_cap)
{
    if (len == 0)
        return EFS_OK;
    if (!msg || !out)
        return EFS_ERR_INVAL;
    if (out_cap < len)
        return EFS_ERR_NOMEM;
    memcpy(out, msg, len);
    return EFS_OK;
}

int efs_wire_unpack(const void *in, uint32_t in_len, void *msg, uint32_t len)
{
    if (len == 0)
        return EFS_OK;
    if (!in || !msg)
        return EFS_ERR_INVAL;
    if (in_len != len)
        return EFS_ERR_PROTO;
    memcpy(msg, in, len);
    return EFS_OK;
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64be(uint8_t *p, uint64_t v)
{
    wr32be(p, (uint32_t)(v >> 32));
    wr32be(p + 4, (uint32_t)v);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64be(const uint8_t *p)
{
    return ((uint64_t)rd32be(p) << 32) | (uint64_t)rd32be(p + 4);
}

int efs_wire_raft_encode(const struct efs_raft_msg *msg, uint8_t *out,
                         uint32_t out_cap, uint32_t *out_len)
{
    uint32_t total;
    uint8_t *p;
    uint32_t i;

    if (!msg || !out)
        return EFS_ERR_INVAL;
    if (msg->nentries > EFS_RAFT_AE_MAX)
        return EFS_ERR_PROTO;
    total = EFS_WIRE_RAFT_HDR_LEN;
    for (i = 0; i < msg->nentries; i++) {
        uint32_t clen = msg->entries[i].clen;
        if (clen > EFS_WIRE_RAFT_MAX_CMD)
            return EFS_ERR_PROTO;
        if (clen && !msg->entries[i].cmd)
            return EFS_ERR_INVAL;
        total += 12u + clen;
    }
    if (out_cap < total)
        return EFS_ERR_NOMEM;

    p = out;
    p[0] = msg->type;
    p[1] = msg->group;
    p[2] = (uint8_t)(msg->vote_granted ? 1 : 0);
    p[3] = (uint8_t)(msg->success ? 1 : 0);
    p += 4;
    wr32be(p, (uint32_t)msg->from); p += 4;
    wr32be(p, (uint32_t)msg->to); p += 4;
    wr64be(p, msg->term); p += 8;
    wr64be(p, msg->boot_id); p += 8;
    wr64be(p, msg->last_log_index); p += 8;
    wr64be(p, msg->last_log_term); p += 8;
    wr64be(p, msg->prev_index); p += 8;
    wr64be(p, msg->prev_term); p += 8;
    wr64be(p, msg->leader_commit); p += 8;
    wr64be(p, msg->match_index); p += 8;
    wr32be(p, msg->nentries); p += 4;
    for (i = 0; i < msg->nentries; i++) {
        uint32_t clen = msg->entries[i].clen;
        wr64be(p, msg->entries[i].term); p += 8;
        wr32be(p, clen); p += 4;
        if (clen) {
            memcpy(p, msg->entries[i].cmd, clen);
            p += clen;
        }
    }
    if (out_len)
        *out_len = (uint32_t)(p - out);
    return EFS_OK;
}

int efs_wire_raft_decode(const uint8_t *in, uint32_t in_len,
                         struct efs_raft_msg *msg, uint8_t *cmd_buf,
                         uint32_t cmd_cap)
{
    const uint8_t *p;
    uint32_t nentries;

    if (!in || !msg)
        return EFS_ERR_INVAL;
    if (in_len < EFS_WIRE_RAFT_HDR_LEN)
        return EFS_ERR_PROTO;
    memset(msg, 0, sizeof(*msg));

    p = in;
    msg->type = p[0];
    msg->group = p[1];
    msg->vote_granted = p[2];
    msg->success = p[3];
    p += 4;
    msg->from = (int)rd32be(p); p += 4;
    msg->to = (int)rd32be(p); p += 4;
    msg->term = rd64be(p); p += 8;
    msg->boot_id = rd64be(p); p += 8;
    msg->last_log_index = rd64be(p); p += 8;
    msg->last_log_term = rd64be(p); p += 8;
    msg->prev_index = rd64be(p); p += 8;
    msg->prev_term = rd64be(p); p += 8;
    msg->leader_commit = rd64be(p); p += 8;
    msg->match_index = rd64be(p); p += 8;
    nentries = rd32be(p); p += 4;
    if (nentries > EFS_RAFT_AE_MAX)
        return EFS_ERR_PROTO;
    msg->nentries = nentries;
    if (nentries) {
        uint32_t off = 0; /* running offset into cmd_buf */
        uint32_t i;
        if (!cmd_buf)
            return EFS_ERR_INVAL;
        for (i = 0; i < nentries; i++) {
            uint64_t eterm;
            uint32_t clen;
            if ((uint32_t)(p - in) + 12u > in_len)
                return EFS_ERR_PROTO;
            eterm = rd64be(p); p += 8;
            clen = rd32be(p); p += 4;
            if (clen > EFS_WIRE_RAFT_MAX_CMD)
                return EFS_ERR_PROTO;
            if ((uint32_t)(p - in) + clen > in_len)
                return EFS_ERR_PROTO;
            if (off + clen > cmd_cap)
                return EFS_ERR_NOMEM;
            if (clen)
                memcpy(cmd_buf + off, p, clen);
            p += clen;
            msg->entries[i].term = eterm;
            msg->entries[i].clen = clen;
            msg->entries[i].cmd = clen ? cmd_buf + off : NULL;
            off += clen;
        }
        if ((uint32_t)(p - in) != in_len)
            return EFS_ERR_PROTO; /* trailing bytes */
    } else if (in_len != EFS_WIRE_RAFT_HDR_LEN) {
        return EFS_ERR_PROTO;
    }
    return EFS_OK;
}
