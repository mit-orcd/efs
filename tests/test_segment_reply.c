/* AK: GET_SEGMENT reply receive contract (efs_conn_recv_segment_reply).
 * A socketpair peer speaks raw wire frames; every malformed shape must be
 * fully drained so the connection stays reusable, a success body must land
 * directly in caller storage, and error replies must map to their status. */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/checksum.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define CAP 65536u

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void send_frame(int fd, uint8_t type, const void *payload, uint32_t plen)
{
    uint8_t be[4];
    put_be32(be, plen + 1);
    assert(write(fd, be, 4) == 4);
    assert(write(fd, &type, 1) == 1);
    if (plen)
        assert(write(fd, payload, plen) == (ssize_t)plen);
}

/* A well-formed success reply: status 0, bytes CAP, digest of the body. */
static void send_success(int fd, const uint8_t *body, uint8_t digest[EFS_HASH_SIZE])
{
    uint8_t hdr[EFS_SEGMENT_GET_REPLY] = {0};
    put_be32(hdr + 4, CAP);
    efs_hash(body, CAP, hdr + 8);
    if (digest)
        memcpy(digest, hdr + 8, EFS_HASH_SIZE);
    uint8_t *frame = malloc(EFS_SEGMENT_GET_REPLY + CAP);
    assert(frame);
    memcpy(frame, hdr, EFS_SEGMENT_GET_REPLY);
    memcpy(frame + EFS_SEGMENT_GET_REPLY, body, CAP);
    send_frame(fd, EFS_MSG_GET_SEGMENT_REPLY, frame, EFS_SEGMENT_GET_REPLY + CAP);
    free(frame);
}

static struct efs_conn *mkconn(int *peer)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    *peer = sv[1];
    struct efs_conn *c = efs_conn_wrap_tcp(sv[0], 0);
    assert(c);
    return c;
}

int main(void)
{
    uint8_t *body = malloc(CAP), *want = malloc(CAP);
    assert(body && want);
    for (uint32_t i = 0; i < CAP; i++) want[i] = (uint8_t)(i * 2654435761u >> 13);
    uint8_t hdr[EFS_SEGMENT_GET_REPLY];
    uint8_t *owned;
    int peer;

    /* Success: body lands in caller storage, no owned buffer. */
    struct efs_conn *c = mkconn(&peer);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    assert(!owned && !memcmp(body, want, CAP));
    assert(hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0);
    efs_conn_destroy(c);
    close(peer);

    /* Error reply maps to its negative status; the conn stays usable. */
    c = mkconn(&peer);
    uint8_t e[EFS_SEGMENT_GET_REPLY] = {0};
    put_be32(e, (uint32_t)EFS_ERR_NOT_FOUND);
    send_frame(peer, EFS_MSG_GET_SEGMENT_REPLY, e, sizeof(e));
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_ERR_NOT_FOUND);
    assert(!owned);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    assert(!memcmp(body, want, CAP));
    efs_conn_destroy(c);
    close(peer);

    /* Error reply that still claims bytes: drained, PROTO, conn reusable. */
    c = mkconn(&peer);
    put_be32(e, (uint32_t)EFS_ERR_INVAL);
    put_be32(e + 4, 4096);
    send_frame(peer, EFS_MSG_GET_SEGMENT_REPLY, e, sizeof(e));
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_ERR_PROTO);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    assert(!memcmp(body, want, CAP));
    efs_conn_destroy(c);
    close(peer);

    /* Wrong message type: drained, PROTO, conn reusable. */
    c = mkconn(&peer);
    send_frame(peer, EFS_MSG_PUT_SEGMENT_REPLY, e, sizeof(e));
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_ERR_PROTO);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    efs_conn_destroy(c);
    close(peer);

    /* Short reply (plen < header): drained, PROTO, conn reusable. */
    c = mkconn(&peer);
    send_frame(peer, EFS_MSG_GET_SEGMENT_REPLY, e, 20);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_ERR_PROTO);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    efs_conn_destroy(c);
    close(peer);

    /* Oversized reply (plen beyond header + capacity): drained, PROTO. */
    c = mkconn(&peer);
    {
        uint32_t plen = EFS_SEGMENT_GET_REPLY + CAP + 100;
        uint8_t *frame = calloc(1, plen);
        assert(frame);
        put_be32(frame + 4, CAP);
        send_frame(peer, EFS_MSG_GET_SEGMENT_REPLY, frame, plen);
        free(frame);
    }
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_ERR_PROTO);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    assert(!memcmp(body, want, CAP));
    efs_conn_destroy(c);
    close(peer);

    /* bytes smaller than capacity with a matching plen: PROTO, reusable. */
    c = mkconn(&peer);
    {
        uint32_t small = CAP / 2;
        uint8_t *frame = calloc(1, EFS_SEGMENT_GET_REPLY + small);
        assert(frame);
        put_be32(frame + 4, small);
        send_frame(peer, EFS_MSG_GET_SEGMENT_REPLY, frame, EFS_SEGMENT_GET_REPLY + small);
        free(frame);
    }
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_ERR_PROTO);
    memset(body, 0, CAP);
    send_success(peer, want, NULL);
    assert(efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned) == EFS_OK);
    assert(!memcmp(body, want, CAP));
    efs_conn_destroy(c);
    close(peer);

    /* Peer close mid-frame: NET, the caller would drop the conn. */
    c = mkconn(&peer);
    send_success(peer, want, NULL);
    close(peer);
    /* First reply may still be readable; the next receive must fail NET. */
    int rc1 = efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned);
    int rc2 = efs_conn_recv_segment_reply(c, hdr, body, CAP, &owned);
    assert(rc1 == EFS_ERR_NET || rc2 == EFS_ERR_NET);
    efs_conn_destroy(c);

    free(body);
    free(want);
    puts("segment reply: direct body receive, status mapping, wrong type, short, "
         "oversized, undersized and peer-close shapes, conn reuse after every drained refusal PASS");
}
