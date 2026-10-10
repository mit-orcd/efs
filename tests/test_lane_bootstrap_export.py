#!/usr/bin/env python3
"""Actual cold bootstrap handler: no prerequisite data PUT or implicit geometry."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('void server_raft_host_lane_bootstrap_rpc(');b=s.index('\nvoid server_raft_host_lane_writer_view',a)
code=r'''
#include "efs/protocol.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include <pthread.h>
#include <assert.h>
#include <string.h>
int efs_chunk_size_valid(uint32_t n){return n>=EFS_MIN_CHUNK_SIZE && n<=EFS_MAX_CHUNK_SIZE && !(n&(n-1));}
struct server {pthread_mutex_t lock;};
struct efs_raft_host {int running,n;struct server *s;};
static struct server server={.lock=PTHREAD_MUTEX_INITIALIZER};
static struct efs_raft_host host={.running=1,.n=4,.s=&server},*g_host=&host;
static struct efs_export configured;static int present,boot_rc,boot_calls,put_calls;
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 2;}
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return 1;}
static int host_pick_peer(struct efs_raft_host*h,uint8_t*g,int n,int skip){(void)h;(void)g;(void)n;(void)skip;return -1;}
static int host_inode_rpc_peer(struct efs_raft_host*h,int peer,int type,const void*q,size_t qn,int rt,void*out,size_t on){(void)h;(void)peer;(void)type;(void)q;(void)qn;(void)rt;(void)out;(void)on;return -1;}
static struct efs_export *server_export_acquire_locked(struct server*s,uint32_t id){(void)s;assert(id==1);return present?&configured:NULL;}
static uint32_t server_data_chunk_size(const struct efs_export *ex){return ex?ex->chunk_size:EFS_DEFAULT_CHUNK_SIZE;}
static void server_export_put(struct server*s,struct efs_export*ex){(void)s;assert(ex);put_calls++;}
static int rc_to_inode_status(int rc){return rc==EFS_ERR_NOT_FOUND?EFS_INODE_RPC_NOT_FOUND:EFS_INODE_RPC_ERROR;}
static int server_raft_host_lane_bootstrap(efs_ino_t ino,uint64_t gen,uint32_t ci,uint32_t cs,int *hint){assert(ino==10 && gen==20 && ci==1);(void)hint;assert(cs==EFS_DEFAULT_CHUNK_SIZE || cs==2*EFS_DEFAULT_CHUNK_SIZE);boot_calls++;return boot_rc;}
static void server_raft_host_lane_writer_view(const struct efs_msg_lane_writer_view*q,struct efs_msg_lane_writer_view_reply*out){out->view.status=EFS_INODE_RPC_OK;out->view.ino=q->ino;out->view.generation=q->generation;out->chunk_size=q->chunk_size;}
'''+s[a:b]+r'''
int main(void){struct efs_msg_lane_bootstrap q={.export_id=1,.ino=10,.generation=20,.chunk_index=1,.chunk_size=EFS_DEFAULT_CHUNK_SIZE};struct efs_msg_lane_writer_view_reply out;
 server_raft_host_lane_bootstrap_rpc(&q,&out);assert(out.view.status==EFS_INODE_RPC_OK && boot_calls==1 && !put_calls);
 q.chunk_size*=2;server_raft_host_lane_bootstrap_rpc(&q,&out);assert(out.view.status==EFS_INODE_RPC_STALE && boot_calls==1);
 present=1;configured.chunk_size=q.chunk_size;server_raft_host_lane_bootstrap_rpc(&q,&out);assert(out.view.status==EFS_INODE_RPC_OK && boot_calls==2 && put_calls==1);
 boot_rc=EFS_ERR_NOT_FOUND;server_raft_host_lane_bootstrap_rpc(&q,&out);assert(out.view.status==EFS_INODE_RPC_NOT_FOUND && boot_calls==3);
 q.export_id=0;server_raft_host_lane_bootstrap_rpc(&q,&out);assert(out.view.status==EFS_INODE_RPC_INVAL && boot_calls==3);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-cold-export-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('cold lane admission: no prerequisite PUT, configured geometry checked, missing FileID refused PASS')
