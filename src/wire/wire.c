#include "efs/wire.h"
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
