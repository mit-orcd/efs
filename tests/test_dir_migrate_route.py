#!/usr/bin/env python3
"""Run the actual migration router through a single-group/dual-group boundary.

The destination is already materialized: cleanup must reach the source leader
as a log command, rather than bounce back as another distributed plan.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()


def function(name):
    start = source.rindex(name)
    return source[start:source.index('\n}', start) + 2]


code = r'''
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HOST_DIR_LEN 10
#define HOST_SUBMIT_FORWARDED 0x80
#define EFS_IO_TIMEOUT_MS 30000
struct efs_raft_host { struct efs_kv *kv; int id; };
static struct efs_raft_host single = {0, 3}, dual = {0, 1};
static unsigned plans, applies, hops, cleanup_reads;
static uint8_t source_group;
static int read_rc, peek_rc, apply_rc;
static uint64_t rd64be(const uint8_t *p) {uint64_t n=0;for(int i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static void put64(uint8_t*p,uint64_t n){for(int i=7;i>=0;i--){p[i]=n;n>>=8;}}
static int host_read_index(struct efs_raft_host*h,uint8_t g,int*hint){(void)h;(void)g;(void)hint;cleanup_reads++;return read_rc;}
static int host_hosts(struct efs_raft_host*h,uint8_t g){return h==&dual || g==source_group;}
static int host_pick_peer(struct efs_raft_host*h,uint8_t*need,int n,int skip){assert(h==&single&&n==2&&need[0]!=need[1]&&skip==-1);return dual.id;}
int efs_meta_dir_migrate_peek(struct efs_kv*kv,efs_ino_t dir,char*name,uint32_t cap,uint32_t*shard,int*saw){(void)kv;(void)dir;assert(cap>=2);strcpy(name,"x");*shard=1;*saw=1;return peek_rc;}
int efs_meta_apply_get_inode(struct efs_kv*kv,efs_ino_t ino,struct efs_meta_row*out){(void)kv;(void)ino;memset(out,0,sizeof(*out));out->used_shards=1;return EFS_OK;}
int efs_kv_key_dentry(uint32_t sh,efs_ino_t dir,const char*name,uint8_t*out,uint32_t*n){(void)sh;(void)dir;(void)name;out[0]=0;*n=1;return EFS_OK;}
static int get_existing(void*ctx,const uint8_t*k,uint32_t n,uint8_t*v,uint32_t*vn){(void)ctx;(void)k;(void)n;memset(v,0,*vn);return EFS_OK;}
static int host_dir_migrate_txn(struct efs_raft_host*h,efs_ino_t dir,const char*name,uint32_t sh,int*hint){(void)h;(void)dir;(void)name;(void)sh;(void)hint;assert(!"already copied: no transaction may be restarted");return EFS_ERR_IO;}
static int host_rpc_submit(struct efs_raft_host*,int,uint8_t,const uint8_t*,uint32_t,struct efs_msg_raft_mkfs_reply*,int);
static int host_propose_wait(struct efs_raft_host*,uint8_t,const uint8_t*,uint32_t,int*);
''' + function('static int host_submit_serves_forward(') + '\n' + \
    function('static int host_dir_migrate_request_decode(') + '\n' + \
    function('static int host_dir_migrate(struct') + r'''
static int receive(struct efs_raft_host*h,uint8_t group,const uint8_t*cmd,uint32_t n){
 assert(++hops<=3);uint8_t packet[11]={group|HOST_SUBMIT_FORWARDED};memcpy(packet+1,cmd,n);uint8_t g;
 if(cmd[0]==EFS_MD_REQ_DIR_MIGRATE){
  assert(!host_submit_serves_forward(packet,n+1,&g));uint8_t canonical[10];
  int rc=host_dir_migrate_request_decode(group,cmd,n,canonical);if(rc)return rc;
  plans++;return host_dir_migrate(h,rd64be(canonical+2),canonical,10,0);
 }
 assert(host_submit_serves_forward(packet,n+1,&g));
 assert(h==&single&&group==source_group&&n==10&&cmd[0]==EFS_MD_CMD_DIR&&cmd[1]==EFS_MD_DIR_MIGRATE);
 applies++;return apply_rc;
}
static int host_rpc_submit(struct efs_raft_host*h,int peer,uint8_t group,const uint8_t*cmd,uint32_t n,struct efs_msg_raft_mkfs_reply*rep,int ms){assert(h==&single&&peer==dual.id&&ms==30000);rep->leader_hint=-1;rep->rc=receive(&dual,group,cmd,n);return EFS_OK;}
static int host_propose_wait(struct efs_raft_host*h,uint8_t group,const uint8_t*cmd,uint32_t n,int*hint){(void)hint;assert(h==&dual);return receive(&single,group,cmd,n);}
int main(void){
 struct efs_kv_ops ops={.get=get_existing};struct efs_kv kv={&ops,0};single.kv=dual.kv=&kv;
 uint8_t request[10]={EFS_MD_REQ_DIR_MIGRATE,0},cmd[10];put64(request+2,2);
 assert(!efs_md_cmd_applicable(EFS_MD_REQ_DIR_MIGRATE));
 source_group=efs_raft_shard_group(efs_kv_inode_shard(2));assert(source_group!=efs_raft_shard_group(1));
 assert(!host_dir_migrate_request_decode(source_group,request,10,cmd));
 for(unsigned n=0;n<10;n++)assert(host_dir_migrate_request_decode(source_group,request,n,cmd)==EFS_ERR_INVAL);
 uint8_t too_long[11]={0};memcpy(too_long,request,10);assert(host_dir_migrate_request_decode(source_group,too_long,11,cmd)==EFS_ERR_INVAL);
 request[1]=1;assert(host_dir_migrate_request_decode(source_group,request,10,cmd)==EFS_ERR_INVAL);request[1]=0;
 assert(host_dir_migrate_request_decode(source_group^2,request,10,cmd)==EFS_ERR_INVAL);
 put64(request+2,0);assert(host_dir_migrate_request_decode(source_group,request,10,cmd)==EFS_ERR_INVAL);put64(request+2,2);
 for(int verdict=0;verdict<2;verdict++){
  applies=plans=hops=cleanup_reads=0;read_rc=EFS_OK;peek_rc=EFS_OK;apply_rc=verdict?EFS_ERR_BUSY:EFS_OK;
  assert(receive(&single,source_group,request,10)==apply_rc);
  assert(plans==2&&applies==1&&hops==3&&cleanup_reads==3);
 }
 plans=applies=hops=0;read_rc=EFS_ERR_BUSY;assert(receive(&single,source_group,request,10)==EFS_ERR_BUSY&&plans==1&&!applies);
 puts("Migration route: single-group planner -> dual-group planner -> source log apply once; refusals and strict framing PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-migration-route-') as tmp:
    path = Path(tmp) / 'route.c'
    path.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=10)
# The production dispatcher must consume plans before its leader-only gate,
# and canonical log commands must no longer enter the planner branch.
submit = source[source.index('void server_raft_host_submit('):source.index('\nvoid server_raft_host_storage_session_fence(')]
assert submit.index('cmd[0] == EFS_MD_REQ_DIR_MIGRATE') < submit.index('/* A forwarded command is answered here only by the leader.')
assert 'cmd[1] == EFS_MD_DIR_MIGRATE' not in submit
