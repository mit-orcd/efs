/* Isolated pack/unpack + frame codec tests for src/wire. No sockets. */
#include "efs/wire.h"
#include "efs/protocol.h"
#include "efs/common.h"
#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void rt_struct(const char *name, size_t sz)
{
    uint8_t *a = malloc(sz);
    uint8_t *b = malloc(sz);
    uint8_t *buf = malloc(sz);
    if (!a || !b || !buf) {
        fprintf(stderr, "FAIL %s oom\n", name);
        failures++;
        free(a);
        free(b);
        free(buf);
        return;
    }
    memset(a, 0xA5, sz);
    memset(b, 0, sz);
    CHECK(efs_wire_pack(a, (uint32_t)sz, buf, (uint32_t)sz) == EFS_OK, name);
    CHECK(efs_wire_unpack(buf, (uint32_t)sz, b, (uint32_t)sz) == EFS_OK, name);
    CHECK(memcmp(a, b, sz) == 0, name);
    free(a);
    free(b);
    free(buf);
}

#define RT(T) rt_struct(#T, sizeof(T))

static void test_frame_roundtrip(void)
{
    uint8_t out[64];
    uint32_t out_len = 0;
    uint8_t type = 0;
    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    uint32_t flen = 0;
    const uint8_t p1[] = { 1, 2, 3 };
    const uint8_t p2[] = { 4, 5 };

    CHECK(efs_wire_frame_size(0, &flen) == EFS_OK && flen == 5, "empty size");
    CHECK(efs_wire_frame_encode(EFS_MSG_HEARTBEAT, NULL, 0, NULL, 0,
                                out, sizeof(out), &out_len) == EFS_OK,
          "empty encode");
    CHECK(out_len == 5, "empty frame len");
    CHECK(efs_wire_frame_decode(out, out_len, &type, &pl, &plen) == EFS_OK,
          "empty decode");
    CHECK(type == EFS_MSG_HEARTBEAT && plen == 0 && pl == NULL, "empty fields");

    CHECK(efs_wire_frame_encode(EFS_MSG_INODE_LOOKUP, p1, 1, NULL, 0,
                                out, sizeof(out), &out_len) == EFS_OK,
          "1-byte encode");
    CHECK(efs_wire_frame_decode(out, out_len, &type, &pl, &plen) == EFS_OK,
          "1-byte decode");
    CHECK(type == EFS_MSG_INODE_LOOKUP && plen == 1 && pl && pl[0] == 1,
          "1-byte fields");

    CHECK(efs_wire_frame_encode(EFS_MSG_INODE_CREATE, p1, 3, p2, 2,
                                out, sizeof(out), &out_len) == EFS_OK,
          "two-part encode");
    CHECK(out_len == 10, "two-part frame len");
    CHECK(efs_wire_frame_decode(out, out_len, &type, &pl, &plen) == EFS_OK,
          "two-part decode");
    CHECK(type == EFS_MSG_INODE_CREATE && plen == 5, "two-part plen");
    CHECK(pl && memcmp(pl, "\x01\x02\x03\x04\x05", 5) == 0, "two-part bytes");
}

static void test_frame_reject(void)
{
    uint8_t buf[16];
    uint8_t type;
    const uint8_t *pl;
    uint32_t plen;
    uint32_t be;
    uint8_t tiny[4];

    CHECK(efs_wire_frame_decode(NULL, 0, &type, &pl, &plen) == EFS_ERR_PROTO,
          "null decode");
    CHECK(efs_wire_frame_decode(tiny, 4, &type, &pl, &plen) == EFS_ERR_PROTO,
          "truncated header");

    memset(buf, 0, sizeof(buf));
    be = htonl(0);
    memcpy(buf, &be, 4);
    buf[4] = EFS_MSG_HEARTBEAT;
    CHECK(efs_wire_frame_decode(buf, 5, &type, &pl, &plen) == EFS_ERR_PROTO,
          "nlen 0");

    be = htonl(10); /* claims 9 payload bytes */
    memcpy(buf, &be, 4);
    buf[4] = EFS_MSG_HEARTBEAT;
    CHECK(efs_wire_frame_decode(buf, 5, &type, &pl, &plen) == EFS_ERR_PROTO,
          "nlen vs buffer mismatch");

    CHECK(efs_wire_frame_check_nlen(0) == EFS_ERR_PROTO, "check nlen 0");
    CHECK(efs_wire_frame_check_nlen(EFS_MSG_MAX_LEN) == EFS_OK, "check nlen max");
    CHECK(efs_wire_frame_check_nlen(EFS_MSG_MAX_LEN + 1) == EFS_ERR_PROTO,
          "check nlen over");

    CHECK(efs_wire_pack("x", 1, tiny, 0) == EFS_ERR_NOMEM, "pack cap");
    CHECK(efs_wire_unpack("xy", 2, tiny, 1) == EFS_ERR_PROTO, "unpack len");
}

static void test_status_offset(void)
{
    CHECK(offsetof(struct efs_msg_inode_reply, status) == 0,
          "inode_reply status@0");
    CHECK(offsetof(struct efs_msg_inode_lookup_path_reply, status) == 0,
          "lookup_path_reply status@0");
}

static void test_every_msg_struct(void)
{
    RT(struct efs_msg_meta_commit);
    RT(struct efs_msg_set_features);
    RT(struct efs_msg_features_reply);
    RT(struct efs_msg_get_features);
    RT(struct efs_msg_hello);
    RT(struct efs_msg_hello_ack);
    RT(struct efs_msg_get_chunk);
    RT(struct efs_msg_put_chunk);
    RT(struct efs_msg_put_meta);
    RT(struct efs_msg_meta_flush_begin);
    RT(struct efs_msg_meta_flush_begin_reply);
    RT(struct efs_msg_put_meta_reply);
    RT(struct efs_msg_list_nodes_reply);
    RT(struct efs_msg_status_reply);
    RT(struct efs_msg_shrink_quota);
    RT(struct efs_msg_node_left);
    RT(struct efs_msg_add_storage);
    RT(struct efs_msg_add_storage_reply);
    RT(struct efs_msg_create_export);
    RT(struct efs_msg_destroy_export);
    RT(struct efs_msg_join);
    RT(struct efs_msg_query_stats_reply);
    RT(struct efs_msg_export_entry);
    RT(struct efs_msg_list_exports_reply);
    RT(struct efs_msg_inode_lookup);
    RT(struct efs_msg_inode_lookup_path);
    RT(struct efs_msg_inode_lookup_path_reply);
    RT(struct efs_msg_heal_status_export);
    RT(struct efs_msg_heal_status_reply);
    RT(struct efs_msg_inode_create);
    RT(struct efs_msg_inode_create_shard);
    RT(struct efs_msg_inode_getattr);
    RT(struct efs_msg_inode_readdir);
    RT(struct efs_msg_inode_unlink);
    RT(struct efs_msg_inode_reply);
    RT(struct efs_msg_inode_readdir_reply);
    RT(struct efs_msg_inode_rename);
    RT(struct efs_msg_inode_rename_at);
    RT(struct efs_msg_inode_setattr);
    RT(struct efs_msg_inode_link);
    RT(struct efs_msg_inode_link_shard);
    RT(struct efs_msg_inode_unlink_shard);
    RT(struct efs_msg_inode_append);
    RT(struct efs_msg_inode_hold);
    RT(struct efs_msg_inode_flock);
    RT(struct efs_chunk_rec);
    RT(struct efs_ino_size_rec);
    RT(struct efs_msg_inode_getchunks);
    RT(struct efs_msg_inode_getchunks_reply);
    RT(struct efs_msg_inode_drop_chunks);
    RT(struct efs_msg_report_chunks);
    RT(struct efs_msg_upgrade_meta);
    RT(struct efs_msg_upgrade_meta_reply);
    RT(struct efs_msg_rdma_setup);
    RT(struct efs_msg_rdma_setup_reply);
}

int main(void)
{
    test_frame_roundtrip();
    test_frame_reject();
    test_status_offset();
    test_every_msg_struct();
    if (failures) {
        fprintf(stderr, "test_wire: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_wire: OK\n");
    return 0;
}
