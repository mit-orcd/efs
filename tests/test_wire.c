/* Isolated pack/unpack + frame codec tests for src/wire. No sockets. */
#include "efs/wire.h"
#include "efs/protocol.h"
#include "efs/common.h"
#include "efs/raft.h"
#include <arpa/inet.h>
#include <assert.h>
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
    RT(struct efs_msg_features_reply);
    RT(struct efs_msg_get_features);
    RT(struct efs_msg_hello);
    RT(struct efs_msg_hello_ack);
    RT(struct efs_msg_get_chunk);
    RT(struct efs_msg_put_chunk);
    RT(struct efs_msg_list_nodes_reply);
    RT(struct efs_msg_status_reply);
    RT(struct efs_msg_shrink_quota);
    RT(struct efs_msg_node_left);
    RT(struct efs_msg_add_storage);
    RT(struct efs_msg_add_storage_reply);
    RT(struct efs_msg_join);
    RT(struct efs_msg_query_stats_reply);
    RT(struct efs_msg_inode_lookup);
    RT(struct efs_msg_inode_lookup_path);
    RT(struct efs_msg_inode_lookup_path_reply);
    RT(struct efs_msg_heal_status_export);
    RT(struct efs_msg_heal_status_reply);
    RT(struct efs_msg_inode_create);
    RT(struct efs_msg_inode_getattr);
    RT(struct efs_msg_inode_readdir);
    RT(struct efs_msg_inode_unlink);
    RT(struct efs_msg_inode_reply);
    RT(struct efs_msg_inode_readdir_reply);
    RT(struct efs_msg_inode_rename_at);
    RT(struct efs_msg_inode_setattr);
    RT(struct efs_msg_inode_link);
    RT(struct efs_msg_inode_append);
    RT(struct efs_msg_inode_hold);
    RT(struct efs_msg_inode_flock);
    RT(struct efs_chunk_rec);
    RT(struct efs_ino_size_rec);
    RT(struct efs_msg_inode_getchunks);
    RT(struct efs_msg_inode_getchunks_reply);
    RT(struct efs_msg_report_chunks);
    RT(struct efs_msg_rdma_setup);
    RT(struct efs_msg_rdma_setup_reply);
    RT(struct efs_msg_raft_mkfs_reply);
    RT(struct efs_msg_raft_status_reply);
    RT(struct efs_raft_group_status);
}

static void test_raft_codec(void)
{
    struct efs_raft_msg a, b;
    uint8_t buf[256];
    uint8_t cmd[17];
    uint8_t cmd_out[64];
    uint32_t len = 0;
    int i;

    memset(&a, 0, sizeof(a));
    a.type = EFS_RAFT_MSG_VOTE_REQ;
    a.group = EFS_RAFT_GROUP_SHARD;
    a.from = 1;
    a.to = 2;
    a.term = 7;
    a.boot_id = 0x0102030405060708ULL;
    a.last_log_index = 11;
    a.last_log_term = 6;
    CHECK(efs_wire_raft_encode(&a, buf, sizeof(buf), &len) == EFS_OK,
          "vote encode");
    CHECK(len == EFS_WIRE_RAFT_HDR_LEN, "vote hdr len");
    CHECK(efs_wire_raft_decode(buf, len, &b, cmd_out, sizeof(cmd_out)) == EFS_OK,
          "vote decode");
    CHECK(b.type == a.type && b.group == a.group && b.from == a.from &&
              b.to == a.to && b.term == a.term && b.boot_id == a.boot_id &&
              b.last_log_index == a.last_log_index &&
              b.last_log_term == a.last_log_term && b.nentries == 0,
          "vote fields");

    memset(&a, 0, sizeof(a));
    for (i = 0; i < 17; i++)
        cmd[i] = (uint8_t)(0xA0 + i);
    a.type = EFS_RAFT_MSG_AE_REQ;
    a.group = EFS_RAFT_GROUP_SHARD2;
    a.from = 0;
    a.to = 3;
    a.term = 9;
    a.prev_index = 4;
    a.prev_term = 8;
    a.leader_commit = 4;
    a.nentries = 1;
    a.entries[0].term = 9;
    a.entries[0].clen = 17;
    a.entries[0].cmd = cmd;
    CHECK(efs_wire_raft_encode(&a, buf, sizeof(buf), &len) == EFS_OK,
          "ae encode");
    CHECK(len == EFS_WIRE_RAFT_HDR_LEN + 12u + 17u, "ae len");
    CHECK(efs_wire_raft_decode(buf, len, &b, cmd_out, sizeof(cmd_out)) == EFS_OK,
          "ae decode");
    CHECK(b.nentries == 1 && b.entries[0].clen == 17 &&
              b.entries[0].term == 9 && b.group == EFS_RAFT_GROUP_SHARD2 &&
              memcmp(b.entries[0].cmd, cmd, 17) == 0,
          "ae fields");

    /* Batched AppendEntries: a 3-entry round-trip (catch-up batching). */
    {
        static const uint8_t c0[3] = {0x01, 0x02, 0x03};
        static const uint8_t c1[5] = {0x04, 0x05, 0x06, 0x07, 0x08};
        static const uint8_t c2[1] = {0x09};
        memset(&a, 0, sizeof(a));
        a.type = EFS_RAFT_MSG_AE_REQ;
        a.group = EFS_RAFT_GROUP_SHARD;
        a.from = 0;
        a.to = 1;
        a.term = 5;
        a.prev_index = 10;
        a.prev_term = 5;
        a.leader_commit = 10;
        a.nentries = 3;
        a.entries[0].term = 5; a.entries[0].clen = 3; a.entries[0].cmd = c0;
        a.entries[1].term = 5; a.entries[1].clen = 5; a.entries[1].cmd = c1;
        a.entries[2].term = 6; a.entries[2].clen = 1; a.entries[2].cmd = c2;
        CHECK(efs_wire_raft_encode(&a, buf, sizeof(buf), &len) == EFS_OK,
              "ae3 encode");
        CHECK(len == EFS_WIRE_RAFT_HDR_LEN + 3u * 12u + 3u + 5u + 1u,
              "ae3 len");
        CHECK(efs_wire_raft_decode(buf, len, &b, cmd_out, sizeof(cmd_out)) ==
                  EFS_OK,
              "ae3 decode");
        CHECK(b.nentries == 3 && b.entries[0].term == 5 &&
                  b.entries[0].clen == 3 && b.entries[1].clen == 5 &&
                  b.entries[2].term == 6 && b.entries[2].clen == 1 &&
                  memcmp(b.entries[0].cmd, c0, 3) == 0 &&
                  memcmp(b.entries[1].cmd, c1, 5) == 0 &&
                  memcmp(b.entries[2].cmd, c2, 1) == 0,
              "ae3 fields");
    }

    /* InstallSnapshot rides the AE entry layout (nentries=1 = blob). */
    memset(&a, 0, sizeof(a));
    a.type = EFS_RAFT_MSG_SNAP_REQ;
    a.group = EFS_RAFT_GROUP_SHARD;
    a.from = 0;
    a.to = 3;
    a.term = 4;
    a.last_log_index = 20;
    a.last_log_term = 3;
    a.leader_commit = 20;
    a.nentries = 1;
    a.entries[0].term = 3;
    a.entries[0].clen = 17;
    a.entries[0].cmd = cmd;
    CHECK(efs_wire_raft_encode(&a, buf, sizeof(buf), &len) == EFS_OK,
          "snap encode");
    CHECK(efs_wire_raft_decode(buf, len, &b, cmd_out, sizeof(cmd_out)) ==
              EFS_OK,
          "snap decode");
    CHECK(b.type == EFS_RAFT_MSG_SNAP_REQ && b.last_log_index == 20 &&
              b.last_log_term == 3 && b.nentries == 1 &&
              b.entries[0].clen == 17 &&
              memcmp(b.entries[0].cmd, cmd, 17) == 0,
          "snap fields");
    memset(&a, 0, sizeof(a));
    a.type = EFS_RAFT_MSG_SNAP_REP;
    a.from = 3;
    a.to = 0;
    a.term = 4;
    a.success = 1;
    a.match_index = 20;
    CHECK(efs_wire_raft_encode(&a, buf, sizeof(buf), &len) == EFS_OK,
          "snap-rep encode");
    CHECK(efs_wire_raft_decode(buf, len, &b, cmd_out, sizeof(cmd_out)) ==
              EFS_OK,
          "snap-rep decode");
    CHECK(b.type == EFS_RAFT_MSG_SNAP_REP && b.success == 1 &&
              b.match_index == 20 && b.nentries == 0,
          "snap-rep fields");

    CHECK(efs_wire_raft_decode(buf, 10, &b, cmd_out, sizeof(cmd_out)) ==
              EFS_ERR_PROTO,
          "short decode");
}

static void test_getchunks_reply_validation(void)
{
    struct efs_msg_inode_getchunks req = {0};
    struct efs_msg_inode_getchunks_reply *r = calloc(1, sizeof(*r));
    assert(r);
    req.ino = 123; req.start = 5; req.max = 2;
    r->ino = req.ino; r->generation = 7;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_OK, "empty GETCHUNKS reply");
    req.generation = 8;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS FileID mismatch");
    req.generation = 7;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_OK, "GETCHUNKS exact FileID");
    r->generation = 0;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS absent FileID");
    r->generation = 7;
    r->count = 1; r->recs[0].ino = 123; r->recs[0].chunk_index = 5;
    r->recs[0].read_view.chunk_size = EFS_MIN_CHUNK_SIZE; r->recs[0].read_view.count = 1;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_OK, "valid GETCHUNKS reply");
    r->count = EFS_GETCHUNKS_MAX + 1;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS count bound");
    r->count = 3;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS requested count bound");
    r->count = 1; ++r->recs[0].ino;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS inode identity");
    --r->recs[0].ino; r->recs[0].chunk_index = 4;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS lower chunk bound");
    r->recs[0].chunk_index = EFS_CHUNK_GROUP_SIZE;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS group bound");
    r->recs[0].chunk_index = 5; r->count = 2; r->recs[1] = r->recs[0];
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS duplicate rows");
    r->recs[1].chunk_index = 6;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_OK, "GETCHUNKS ordered rows");
    r->recs[1].read_view.count = 0;
    CHECK(efs_getchunks_reply_valid(&req, r) == EFS_ERR_PROTO, "GETCHUNKS malformed part view");
    free(r);
}

static void test_writer_view_reply(void)
{
    struct efs_msg_inode_writer_view req = {123, 456, 17, 0};
    struct efs_msg_inode_writer_view_reply r = {0};
    r.ino = req.ino; r.generation = req.generation; r.chunk_index = req.chunk_index;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_OK, "writer zero epoch reply");
    r.authority_epoch = 3; r.history.count = 2;
    r.history.entries[0] = (struct efs_content_fence){1, 100};
    r.history.entries[1] = (struct efs_content_fence){3, 50};
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO,
          "writer reply cannot invent completeness across a retired gap");
    r.oldest_complete_epoch = 2;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_OK, "writer valid conservative floor");
    r.history.count = EFS_FENCE_HISTORY_MAX + 1;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO, "writer oversized history");
    r.history.count = 0; r.oldest_complete_epoch = 3;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_OK, "writer retired history reply");
    req.generation = 0;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_OK, "writer bootstrap discovers FileID");
    r.generation = 0;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO, "writer bootstrap needs nonzero FileID");
    req.generation = r.generation = 456;
    ++r.generation;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO, "writer wrong FileID reply");
    --r.generation; ++r.chunk_index;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO, "writer wrong chunk reply");
    --r.chunk_index; r.oldest_complete_epoch = 4;
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO, "writer future floor");
    r.oldest_complete_epoch = 3; r.history.count = 1;
    r.history.entries[0] = (struct efs_content_fence){4, 1};
    CHECK(efs_writer_view_reply_valid(&req, &r) == EFS_ERR_PROTO, "writer future fence");
    rt_struct("writer-view-request", sizeof(req));
    rt_struct("writer-view-reply", sizeof(r));
}

int main(void)
{
    test_writer_view_reply();
    test_getchunks_reply_validation();
    test_frame_roundtrip();
    test_frame_reject();
    test_status_offset();
    test_every_msg_struct();
    test_raft_codec();
    if (failures) {
        fprintf(stderr, "test_wire: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_wire: OK\n");
    return 0;
}
