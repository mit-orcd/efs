#!/usr/bin/env python3
"""Exercise the production D25 RPC retry and reply-validation boundary."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = (root / 'src/client/inode_rpc.c').read_text()
start = text.index('int efs_client_rpc_lane_writer_view(')
function = text[start:text.index('\n}', start) + 2]
helper_start = text.index('static int rpc_writer_retry_pause(')
helper = text[helper_start:text.index('\n}', helper_start) + 2]
source = r'''
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
uint64_t efs_client_rpc_deadline_ms(void) { return 0; }
int efs_client_rpc_past_deadline(void) { return 0; }
struct efs_conn { int unused; };
int efs_chunk_size_valid(uint32_t cs) { return cs == EFS_MIN_CHUNK_SIZE || cs == EFS_MIN_CHUNK_SIZE * 2; }
static struct efs_conn connection;
static struct efs_msg_lane_writer_view_reply replies[8];
static unsigned calls, releases, drops, sleeps;
static int fail_send, fail_recv, bad_type, bad_length;
static efs_node_id_t last_target;
static struct efs_conn *efs_client_conn_get(efs_node_id_t nid) {
    last_target = nid; return &connection;
}
static struct efs_conn *rpc_owner_conn_shard(uint32_t shard, efs_node_id_t *nid) {
    assert(shard == efs_kv_lane_shard(123, 17)); *nid = 1; return &connection;
}
static void efs_client_conn_drop(efs_node_id_t nid, struct efs_conn *c) {
    (void)nid; (void)c; ++drops;
}
static void efs_client_conn_release(efs_node_id_t nid, struct efs_conn *c) {
    (void)nid; (void)c; ++releases;
}
int efs_conn_send_msg(struct efs_conn *c, uint8_t type, const void *p, uint32_t n) {
    (void)c; assert(type == EFS_MSG_LANE_WRITER_VIEW);
    assert(n == sizeof(struct efs_msg_lane_writer_view));
    const struct efs_msg_lane_writer_view *q = p;
    assert(q->ino == 123 && q->generation == 456 && q->chunk_index == 17 && q->chunk_size == EFS_MIN_CHUNK_SIZE);
    return fail_send;
}
int efs_conn_recv_msg(struct efs_conn *c, uint8_t *type, void **p, uint32_t *n) {
    (void)c;
    *type = bad_type ? EFS_MSG_VERSION_REPLY : EFS_MSG_LANE_WRITER_VIEW_REPLY;
    *n = sizeof(replies[0]) - bad_length;
    *p = malloc(sizeof(replies[0])); assert(*p);
    memcpy(*p, &replies[calls++], sizeof(replies[0]));
    return fail_recv;
}
static int mock_sleep(unsigned usec) { (void)usec; ++sleeps; return 0; }
#define usleep mock_sleep
static int rpc_status_to_efs(uint8_t status) {
    switch (status) {
    case EFS_INODE_RPC_OK: return EFS_OK;
    case EFS_INODE_RPC_BUSY: return EFS_ERR_BUSY;
    case EFS_INODE_RPC_STALE: return EFS_ERR_STALE;
    default: return EFS_ERR_NOT_PRIMARY;
    }
}
''' + helper + '\n' + function + r'''
static void reset(void) {
    calls = releases = drops = sleeps = 0;
    fail_send = fail_recv = bad_type = bad_length = 0; last_target = 0;
    for (unsigned i = 0; i < 8; ++i) {
        replies[i] = (struct efs_msg_lane_writer_view_reply){0};
        replies[i].chunk_size = EFS_MIN_CHUNK_SIZE; replies[i].view.ino = 123; replies[i].view.generation = 456; replies[i].view.chunk_index = 17;
    }
}
int main(void) {
    struct efs_msg_lane_writer_view_reply out, saved;
    memset(&saved, 0xa5, sizeof(saved)); out = saved;
    reset(); replies[0].view.status = EFS_INODE_RPC_NOT_PRIMARY; replies[0].view.primary_id = 2;
    replies[1].view.status = EFS_INODE_RPC_BUSY;
    assert(efs_client_rpc_lane_writer_view(123, 456, 17, EFS_MIN_CHUNK_SIZE, &out) == EFS_OK);
    assert(calls == 3 && releases == 3 && !drops && sleeps == 1 && last_target == 2);
    reset(); out = saved;
    for (unsigned i = 0; i < 8; ++i) replies[i].view.status = EFS_INODE_RPC_BUSY;
    assert(efs_client_rpc_lane_writer_view(123, 456, 17, EFS_MIN_CHUNK_SIZE, &out) == EFS_ERR_BUSY);
    assert(calls == 8 && releases == 8 && !memcmp(&out, &saved, sizeof(out)));
    for (unsigned fault = 0; fault < 8; ++fault) {
        reset(); out = saved;
        if (fault == 0) ++replies[0].view.generation;
        if (fault == 1) ++replies[0].view.chunk_index;
        if (fault == 2) bad_type = 1;
        if (fault == 3) bad_length = 1;
        if (fault == 4) replies[0].view.authority_epoch = 1;
        if (fault == 6) replies[0].chunk_size *= 2;
        if (fault == 7) replies[0].reserved = 1;
        if (fault == 5) replies[0].view.history.count = EFS_FENCE_HISTORY_MAX + 1;
        assert(efs_client_rpc_lane_writer_view(123, 456, 17, EFS_MIN_CHUNK_SIZE, &out) == EFS_ERR_PROTO);
        assert(releases == 1 && !drops && !memcmp(&out, &saved, sizeof(out)));
    }
    reset(); out = saved; replies[0].view.status = EFS_INODE_RPC_STALE;
    assert(efs_client_rpc_lane_writer_view(123, 456, 17, EFS_MIN_CHUNK_SIZE, &out) == EFS_ERR_STALE);
    assert(!memcmp(&out, &saved, sizeof(out)));
    reset(); fail_send = 1;
    assert(efs_client_rpc_lane_writer_view(123, 456, 17, EFS_MIN_CHUNK_SIZE, &out) == EFS_ERR_NET);
    assert(drops == 1 && !releases && !calls);
    reset(); fail_recv = 1;
    assert(efs_client_rpc_lane_writer_view(123, 456, 17, EFS_MIN_CHUNK_SIZE, &out) == EFS_ERR_NET);
    assert(drops == 1 && !releases && calls == 1);
    puts("lane writer RPC: lane routing, geometry, redirects, bounded BUSY, malformed replies and failure retention PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-writer-view-rpc-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(source)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include')]
    command += shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS', ''))
    subprocess.run(command + [str(path), str(root / 'src/kv/kv_key.c'), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
