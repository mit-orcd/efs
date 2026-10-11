#!/usr/bin/env python3
"""Actual fence CLI keeps its captured epoch through both storage barriers."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/mgmt/efs_mgmt.c').read_text();a=s.index('static int cmd_raft_session_fence(');b=s.index('\n}',a)+2
code=r'''
#include <efs/protocol.h>
#include <efs/meta_cmd.h>
#include <efs/session.h>
#include <efs/kv_key.h>
#include <efs/raft.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned mode,barriers,cleanups,finished;
uint32_t efs_kv_session_shard(const uint8_t uuid[16]){assert(uuid[0]==42);return 31;}
static uint32_t read32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void wr32be_mgmt(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(24-8*i);}
static void sess_pack_hdr(uint8_t *p,uint8_t group,uint8_t sub,const uint8_t uuid[16]){p[0]=group;p[1]=EFS_MD_CMD_SESSION;p[2]=sub;memcpy(p+3,uuid,16);}
static int sess_rpc(const char *host,uint16_t port,const uint8_t *p,uint32_t n,struct efs_msg_raft_mkfs_reply *r){
 assert(!strcmp(host,"owned")&&port==1);memset(r,0,sizeof(*r));r->leader_hint=1;
 if(p[2]==EFS_MD_SESS_BEGIN){assert(n==19);return 0;}
 if(p[2]==EFS_MD_SESS_ACK){assert(n==23&&read32(p+19)==7&&barriers==2);return 0;}
 assert(p[2]==0&&n==23);uint32_t sh=read32(p+19);
 r->salt=sh==31?2|((uint64_t)(finished?EFS_SESSION_ACTIVE:EFS_SESSION_FENCING)<<32):sh==EFS_SESSION_BITS?1u<<7:0;
 return 0;
}
static int sess_storage_barrier(const char *host,uint16_t port,const uint8_t *p,uint32_t n,struct efs_msg_raft_mkfs_reply *r){
 (void)r;assert(!strcmp(host,"owned")&&port==1&&p[1]==EFS_MD_CMD_SESSION&&p[3]==42&&read32(p+19)==2);barriers++;
 if(barriers==1){assert(p[2]==EFS_MD_SESS_STORAGE_FENCE&&n==23);return 0;}
 if(barriers==2){assert(p[2]==EFS_MD_SESS_FENCE_LOC&&n==27&&read32(p+23)==7);return mode==1?EFS_ERR_BUSY:0;}
 assert(barriers==3&&p[2]==EFS_MD_SESS_FINISH&&n==23);
 if(mode==2)return EFS_ERR_BUSY;
 if(mode==3)return EFS_ERR_STALE;
 finished=1;return 0;
}
static int sess_cleanup_bounded(const char *h,uint16_t port,const uint8_t uuid[16],uint32_t epoch,uint32_t shard,int app,struct efs_msg_raft_mkfs_reply *r){
 (void)r;assert(!strcmp(h,"owned")&&port==1&&uuid[0]==42&&epoch==1&&shard==7&&finished);assert(app==(int)cleanups);cleanups++;return 0;
}
'''+s[a:b]+r'''
int main(void){uint8_t uuid[16]={42};assert(!cmd_raft_session_fence("owned",1,uuid)&&barriers==3&&cleanups==2);
 for(mode=1;mode<=3;mode++){barriers=cleanups=finished=0;assert(cmd_raft_session_fence("owned",1,uuid)==1&&!cleanups&&!finished);}
 puts("Session fence CLI: exact epoch participant and FINISH barriers; BUSY/STALE never report completion or start cleanup PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-finish-') as td:
 p=Path(td)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
