/* Isolated store + transport + EC tests. Loop backend: no sockets.
 * Conn adapter: socketpair through the existing efs_conn I/O. */
#include "efs/store.h"
#include "efs/transport.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void test_mem_store(void)
{
    struct efs_store *s = efs_store_mem_create();
    struct efs_frag_id id = { .export_id = 1, .ino = 7, .chunk_index = 3,
                              .fragment_index = 1 };
    struct efs_frag_id miss = { .export_id = 1, .ino = 7, .chunk_index = 3,
                                .fragment_index = 2 };
    uint8_t buf[64];
    uint8_t got[64];
    uint8_t sum[EFS_HASH_SIZE];
    uint8_t sum_got[EFS_HASH_SIZE];
    uint32_t len;
    int sum_ok = 0;
    const char *pay = "fragment-bytes";

    CHECK(s != NULL, "mem create");
    memset(buf, 0, sizeof(buf));
    memcpy(buf, pay, strlen(pay));
    efs_hash(buf, (size_t)strlen(pay), sum);

    CHECK(efs_store_put(s, &id, buf, (uint32_t)strlen(pay), sum) == EFS_OK,
          "put");
    len = sizeof(got);
    memset(got, 0, sizeof(got));
    CHECK(efs_store_get(s, &id, got, &len, sum_got, &sum_ok) == EFS_OK, "get");
    CHECK(len == strlen(pay), "get len");
    CHECK(memcmp(got, pay, len) == 0, "get bytes");
    CHECK(sum_ok == 1, "sum_ok");
    CHECK(memcmp(sum, sum_got, EFS_HASH_SIZE) == 0, "sum bytes");

    len = sizeof(got);
    CHECK(efs_store_get(s, &miss, got, &len, NULL, NULL) == EFS_ERR_NOT_FOUND,
          "miss");
    CHECK(efs_store_del(s, &id) == EFS_OK, "del");
    len = sizeof(got);
    CHECK(efs_store_get(s, &id, got, &len, NULL, NULL) == EFS_ERR_NOT_FOUND,
          "gone");
    CHECK(efs_store_del(s, &id) == EFS_ERR_NOT_FOUND, "del miss");

    /* overwrite + no checksum sidecar */
    CHECK(efs_store_put(s, &id, buf, 4, NULL) == EFS_OK, "put nosum");
    CHECK(efs_store_put(s, &id, buf, 8, NULL) == EFS_OK, "overwrite");
    len = sizeof(got);
    sum_ok = 1;
    CHECK(efs_store_get(s, &id, got, &len, NULL, &sum_ok) == EFS_OK, "get2");
    CHECK(len == 8 && sum_ok == 0, "overwrite fields");

    efs_store_mem_free(s);
}

static void test_loop_transport(void)
{
    struct efs_transport *a = NULL, *b = NULL;
    uint8_t type = 0;
    void *pl = NULL;
    uint32_t plen = 0;
    const uint8_t p1[] = { 1, 2, 3 };
    const uint8_t p2[] = { 4, 5 };

    CHECK(efs_transport_loop_pair(&a, &b) == EFS_OK, "pair");
    CHECK(a && b, "ends");
    CHECK(efs_transport_wait_request(b) == EFS_ERR_AGAIN, "empty wait");
    CHECK(efs_transport_recv(b, &type, &pl, &plen) == EFS_ERR_AGAIN, "empty recv");

    CHECK(efs_transport_send(a, EFS_MSG_HEARTBEAT, NULL, 0) == EFS_OK, "send empty");
    CHECK(efs_transport_wait_request(b) == EFS_OK, "wait after send");
    CHECK(efs_transport_recv(b, &type, &pl, &plen) == EFS_OK, "recv empty");
    CHECK(type == EFS_MSG_HEARTBEAT && plen == 0 && pl == NULL, "empty fields");

    CHECK(efs_transport_send_parts(a, EFS_MSG_PUT_CHUNK, p1, 3, p2, 2) == EFS_OK,
          "send parts");
    CHECK(efs_transport_recv(b, &type, &pl, &plen) == EFS_OK, "recv parts");
    CHECK(type == EFS_MSG_PUT_CHUNK && plen == 5, "parts plen");
    CHECK(pl && memcmp(pl, "\x01\x02\x03\x04\x05", 5) == 0, "parts bytes");
    free(pl);

    /* reverse direction */
    CHECK(efs_transport_send(b, EFS_MSG_PUT_CHUNK_REPLY, p1, 1) == EFS_OK,
          "reply send");
    CHECK(efs_transport_recv(a, &type, &pl, &plen) == EFS_OK, "reply recv");
    CHECK(type == EFS_MSG_PUT_CHUNK_REPLY && plen == 1 && pl && ((uint8_t *)pl)[0] == 1,
          "reply fields");
    free(pl);

    efs_transport_loop_free(a);
    efs_transport_loop_free(b);
}

static void test_ec_through_store(void)
{
    struct efs_store *s = efs_store_mem_create();
    const size_t cs = EFS_CHUNK_SIZE;
    const size_t fl = cs / 2;
    uint8_t *chunk = malloc(cs);
    uint8_t *decoded = malloc(cs);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * fl);
    uint8_t *fragments[EFS_NUM_FRAGMENTS];
    struct efs_frag_id id = { .export_id = 9, .ino = 42, .chunk_index = 0,
                              .fragment_index = 0 };
    int missing, a, b, fi;
    uint32_t len;

    CHECK(s && chunk && decoded && frag_buf, "ec alloc");
    if (!s || !chunk || !decoded || !frag_buf)
        goto out;
    fragments[0] = frag_buf;
    fragments[1] = frag_buf + fl;
    fragments[2] = frag_buf + 2 * fl;
    memset(chunk, 0x5A, cs);
    CHECK(efs_encode_chunk(chunk, cs, cs, fragments) == EFS_OK, "encode");
    for (fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        id.fragment_index = (uint32_t)fi;
        CHECK(efs_store_put(s, &id, fragments[fi], (uint32_t)fl, NULL) == EFS_OK,
              "put frag");
    }
    /* lose fragment 1, reconstruct from 0 and 2 */
    id.fragment_index = 1;
    CHECK(efs_store_del(s, &id) == EFS_OK, "del frag 1");
    missing = 1;
    a = 0;
    b = 2;
    for (fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (fi == missing) {
            memset(fragments[fi], 0, fl);
            continue;
        }
        id.fragment_index = (uint32_t)fi;
        len = (uint32_t)fl;
        CHECK(efs_store_get(s, &id, fragments[fi], &len, NULL, NULL) == EFS_OK,
              "get frag");
        CHECK(len == (uint32_t)fl, "frag len");
    }
    memset(decoded, 0, cs);
    CHECK(efs_decode_chunk(fragments, cs, a, b, missing, decoded, cs) == EFS_OK,
          "decode");
    CHECK(memcmp(chunk, decoded, cs) == 0, "decoded bytes");

out:
    efs_store_mem_free(s);
    free(chunk);
    free(decoded);
    free(frag_buf);
}

static void test_conn_adapter(void)
{
    int sv[2] = { -1, -1 };
    struct efs_conn *cli = NULL, *srv = NULL;
    struct efs_transport *ta = NULL, *tb = NULL;
    uint8_t type = 0;
    void *pl = NULL;
    uint32_t plen = 0;
    const uint8_t body[] = { 9, 8, 7 };

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    cli = efs_conn_wrap_tcp(sv[0], 0);
    srv = efs_conn_wrap_tcp(sv[1], 1);
    CHECK(cli && srv, "wrap");
    ta = efs_transport_from_conn(cli, 1);
    tb = efs_transport_from_conn(srv, 1);
    CHECK(ta && tb, "from_conn");

    CHECK(efs_transport_send(ta, EFS_MSG_GET_CHUNK, body, 3) == EFS_OK,
          "conn send");
    CHECK(efs_transport_recv(tb, &type, &pl, &plen) == EFS_OK, "conn recv");
    CHECK(type == EFS_MSG_GET_CHUNK && plen == 3, "conn fields");
    CHECK(pl && memcmp(pl, body, 3) == 0, "conn bytes");
    free(pl);

    CHECK(efs_transport_send(tb, EFS_MSG_GET_CHUNK_REPLY, body, 1) == EFS_OK,
          "conn reply send");
    CHECK(efs_transport_recv(ta, &type, &pl, &plen) == EFS_OK, "conn reply recv");
    CHECK(type == EFS_MSG_GET_CHUNK_REPLY && plen == 1 && pl &&
              ((uint8_t *)pl)[0] == 9,
          "conn reply");
    free(pl);

    efs_transport_conn_free(ta);
    efs_transport_conn_free(tb);
}

static void test_fileid_isolation(void)
{
    struct efs_store *s = efs_store_mem_create();
    struct efs_frag_id a = { .export_id = 1, .ino = 7, .inode_generation = 1,
                             .chunk_generation = 11, .chunk_index = 0,
                             .fragment_index = 0, .coding_profile_id = 1 };
    struct efs_frag_id b = a;
    uint8_t pa[4] = { 1, 2, 3, 4 };
    uint8_t pb[4] = { 9, 8, 7, 6 };
    uint8_t got[4];
    uint32_t len;

    CHECK(s != NULL, "mem");
    b.inode_generation = 2;
    CHECK(efs_store_put(s, &a, pa, 4, NULL) == EFS_OK, "put a");
    CHECK(efs_store_put(s, &b, pb, 4, NULL) == EFS_OK, "put b");
    len = sizeof(got);
    CHECK(efs_store_get(s, &a, got, &len, NULL, NULL) == EFS_OK && got[0] == 1,
          "a bytes");
    len = sizeof(got);
    CHECK(efs_store_get(s, &b, got, &len, NULL, NULL) == EFS_OK && got[0] == 9,
          "b bytes");
    b.chunk_generation = 99;
    len = sizeof(got);
    CHECK(efs_store_get(s, &b, got, &len, NULL, NULL) == EFS_ERR_NOT_FOUND,
          "other cand");
    efs_store_mem_free(s);
}

int main(void)
{
    test_mem_store();
    test_loop_transport();
    test_ec_through_store();
    test_conn_adapter();
    test_fileid_isolation();
    if (failures) {
        fprintf(stderr, "test_data: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_data: OK\n");
    return 0;
}
