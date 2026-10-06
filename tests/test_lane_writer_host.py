#!/usr/bin/env python3
"""Execute the production lane read adapter on a host owning only that group."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'src/server/raft_host.c').read_text()
start=text.index('void server_raft_host_lane_writer_view(')
function=text[start:text.index('\n}',start)+2]
source=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host { int running, n; struct efs_kv *kv; };
static struct efs_raft_host host = {.running=1, .n=3};
static struct efs_raft_host *g_host=&host;
static unsigned reads, forwards, lane_reads;
static uint8_t lane_group;
static int owns=1, read_rc, lane_rc;
int efs_chunk_size_valid(uint32_t cs) { return cs == EFS_MIN_CHUNK_SIZE; }
static int host_hosts(struct efs_raft_host *h, uint8_t group) {
    (void)h; assert(group==lane_group); return owns;
}
static int host_pick_peer(struct efs_raft_host *h, const uint8_t *groups, int n, int skip) {
    (void)h; (void)skip; assert(n==1 && groups[0]==lane_group); return 1;
}
static int host_read_index(struct efs_raft_host *h, uint8_t group, int *hint) {
    (void)h; assert(group==lane_group); ++reads; *hint=1; return read_rc;
}
static int host_inode_rpc_peer(struct efs_raft_host *h, int rid, uint8_t type,
                                const void *req, uint32_t len, uint8_t reply_type,
                                void *out, uint32_t outlen) {
    (void)h; (void)req; assert(rid==1 && type==EFS_MSG_LANE_WRITER_VIEW &&
       len==sizeof(struct efs_msg_lane_writer_view) && reply_type==EFS_MSG_LANE_WRITER_VIEW_REPLY &&
       outlen==sizeof(struct efs_msg_lane_writer_view_reply));
    ++forwards; ((struct efs_msg_lane_writer_view_reply *)out)->view.status=EFS_INODE_RPC_BUSY;
    return 0;
}
static uint8_t rc_to_inode_status(int rc) {
    return rc==EFS_OK ? EFS_INODE_RPC_OK : rc==EFS_ERR_NOT_FOUND ?
      EFS_INODE_RPC_NOT_FOUND : EFS_INODE_RPC_BUSY;
}
int efs_meta_get_lane_writer_view(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                  uint32_t ci, uint32_t cs, struct efs_meta_writer_view *out) {
    (void)kv; assert(ino==123 && gen==456 && ci==17 && cs==EFS_MIN_CHUNK_SIZE);
    ++lane_reads; *out=(struct efs_meta_writer_view){.ino=ino,.generation=gen}; return lane_rc;
}
''' + function + r'''
int main(void) {
    struct efs_msg_lane_writer_view req={123,456,17,EFS_MIN_CHUNK_SIZE};
    struct efs_msg_lane_writer_view_reply out;
    lane_group=efs_raft_shard_group(efs_kv_lane_shard(req.ino,17));
    assert(lane_group!=efs_raft_shard_group(efs_kv_inode_shard(req.ino)));
    server_raft_host_lane_writer_view(&req,&out);
    assert(reads==1 && lane_reads==1 && !forwards &&
           efs_lane_writer_view_reply_valid(&req,&out)==EFS_OK);
    lane_rc=EFS_ERR_NOT_FOUND; server_raft_host_lane_writer_view(&req,&out);
    assert(out.view.status==EFS_INODE_RPC_NOT_FOUND && !forwards);
    read_rc=EFS_ERR_BUSY; server_raft_host_lane_writer_view(&req,&out);
    assert(reads==3 && lane_reads==2 && out.view.status==EFS_INODE_RPC_BUSY);
    owns=0; server_raft_host_lane_writer_view(&req,&out);
    assert(reads==3 && forwards==1 && out.view.status==EFS_INODE_RPC_BUSY);
    req.generation=0; server_raft_host_lane_writer_view(&req,&out);
    assert(reads==3 && forwards==1 && out.view.status==EFS_INODE_RPC_INVAL);
    puts("lane host: one ReadIndex, no inode group, single-group forwarding and cold absence PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lane-host-') as directory:
    path=Path(directory)/'test.c';path.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    cmd+=shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(path),str(root/'src/kv/kv_key.c'),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
