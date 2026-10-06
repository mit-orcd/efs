#!/usr/bin/env python3
"""Cold authority adoption validates export geometry before any transaction."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'src/server/raft_host.c').read_text();start=text.index('void server_raft_host_lane_bootstrap_rpc(')
function=text[start:text.index('\n}',start)+2]
source=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
struct efs_server { pthread_mutex_t lock; };
struct efs_raft_host { int running,n; struct efs_server *s; };
static struct efs_server server={.lock=PTHREAD_MUTEX_INITIALIZER};
static struct efs_raft_host host={1,4,&server}, *g_host=&host;
static struct efs_export ex={.chunk_size=EFS_MIN_CHUNK_SIZE};
static unsigned boots,views,puts,forwards; static int dual=1,boot_rc;
int efs_chunk_size_valid(uint32_t cs) { return cs==EFS_MIN_CHUNK_SIZE || cs==EFS_MIN_CHUNK_SIZE*2; }
static int host_hosts(struct efs_raft_host *h,uint8_t group) { (void)h;(void)group;return dual; }
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *groups,int n,int skip) {
    (void)h;(void)skip;assert(n==2 && groups[0]!=groups[1]);return 3;
}
static int host_inode_rpc_peer(struct efs_raft_host *h,int rid,uint8_t type,const void *q,
                                uint32_t len,uint8_t reply_type,void *out,uint32_t outlen) {
    (void)h;(void)q;assert(rid==3 && type==EFS_MSG_LANE_BOOTSTRAP &&
      len==sizeof(struct efs_msg_lane_bootstrap) && reply_type==EFS_MSG_LANE_BOOTSTRAP_REPLY &&
      outlen==sizeof(struct efs_msg_lane_writer_view_reply));++forwards;
    ((struct efs_msg_lane_writer_view_reply *)out)->view.status=EFS_INODE_RPC_BUSY;return 0;
}
static struct efs_export *server_export_acquire_locked(struct efs_server *s,efs_export_id_t id) {
    (void)s;return id==19 ? &ex:NULL;
}
static uint32_t server_data_chunk_size(struct efs_export *e) { return e->chunk_size; }
static void server_export_put(struct efs_server *s,struct efs_export *e) { (void)s;(void)e;++puts; }
int server_raft_host_lane_bootstrap(efs_ino_t ino,uint64_t gen,uint32_t ci,uint32_t cs,int *hint) {
    assert(ino==123 && gen==456 && ci==17 && cs==ex.chunk_size);++boots;*hint=3;return boot_rc;
}
void server_raft_host_lane_writer_view(const struct efs_msg_lane_writer_view *q,
                                      struct efs_msg_lane_writer_view_reply *out) {
    assert(q->generation==456 && q->chunk_size==ex.chunk_size && releases>=boots);++views;
    out->view.status=EFS_INODE_RPC_OK;
}
static uint8_t rc_to_inode_status(int rc) { return rc==EFS_OK?EFS_INODE_RPC_OK:EFS_INODE_RPC_BUSY; }
''' + function + r'''
int main(void) {
    struct efs_msg_lane_bootstrap q={19,123,456,17,EFS_MIN_CHUNK_SIZE};
    struct efs_msg_lane_writer_view_reply out;
    server_raft_host_lane_bootstrap_rpc(&q,&out);
    assert(boots==1 && views==1 && puts==1 && !forwards);
    q.chunk_size*=2;server_raft_host_lane_bootstrap_rpc(&q,&out);
    assert(boots==1 && views==1 && out.view.status==EFS_INODE_RPC_STALE);
    q.chunk_size=EFS_MIN_CHUNK_SIZE;q.export_id=20;
    server_raft_host_lane_bootstrap_rpc(&q,&out);assert(boots==1 && out.view.status==EFS_INODE_RPC_INVAL);
    q.export_id=19;boot_rc=EFS_ERR_BUSY;
    server_raft_host_lane_bootstrap_rpc(&q,&out);
    assert(boots==2 && views==1 && out.view.status==EFS_INODE_RPC_BUSY && out.view.primary_id==4);
    dual=0;server_raft_host_lane_bootstrap_rpc(&q,&out);
    assert(boots==2 && forwards==1 && out.view.status==EFS_INODE_RPC_BUSY);
    q.generation=0;server_raft_host_lane_bootstrap_rpc(&q,&out);
    assert(boots==2 && forwards==1 && out.view.status==EFS_INODE_RPC_INVAL);
    puts("cold bootstrap host: configured export geometry, exact FileID, bounded forwarding and failure propagation PASS");
}
'''
# Avoid shadowing stdio puts in the stub counter.
source=source.replace('puts,forwards','releases,forwards').replace('++puts','++releases').replace('puts==','releases==')
with tempfile.TemporaryDirectory(prefix='efs-bootstrap-host-') as directory:
    path=Path(directory)/'test.c';path.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include')]
    cmd+=shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(path),str(root/'src/kv/kv_key.c'),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
