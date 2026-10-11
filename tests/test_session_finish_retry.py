#!/usr/bin/env python3
"""Execute the CLI fence: FINISH keeps its epoch while certificates are BUSY."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/mgmt/efs_mgmt.c').read_text()
def function(name):
    a=s.index('static '+name)
    return s[a:s.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/session.h"
static uint64_t now_ms=1000,deadline;
static unsigned begins,finishes,cleanups;
static uint8_t identity[16];
static int fake_clock(clockid_t clock,struct timespec *out){assert(clock==CLOCK_MONOTONIC);out->tv_sec=now_ms/1000;out->tv_nsec=0;return 0;}
static int fake_sleep(unsigned us){now_ms+=us/1000;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
static uint64_t efs_net_deadline_ms(void){return deadline;}
static void efs_net_set_deadline_ms(uint64_t d){deadline=d;}
static int efs_net_remaining_ms(int cap){(void)cap;return deadline>now_ms?(int)(deadline-now_ms):0;}
static uint32_t efs_kv_session_shard(const uint8_t *id){assert(!memcmp(id,identity,16));return 7;}
static uint8_t efs_raft_shard_group(uint32_t sh){(void)sh;return 0;}
static void wr32be_mgmt(uint8_t *p,uint32_t x){p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static uint32_t read32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int sess_rpc(const char *host,uint16_t port,const uint8_t *p,uint32_t bytes,struct efs_msg_raft_mkfs_reply *r){
 assert(!strcmp(host,"seed")&&port==1&&!memcmp(p+3,identity,16));
 memset(r,0,sizeof(*r));r->leader_hint=-1;
 if(p[2]==EFS_MD_SESS_BEGIN){assert(bytes==19);begins++;}
 else if(p[2]==EFS_MD_SESS_STORAGE_FENCE){assert(bytes==23&&read32(p+19)==5);}
 else if(p[2]==EFS_MD_SESS_FINISH){assert(bytes==23&&read32(p+19)==5);finishes++;if(finishes<3)return EFS_ERR_BUSY;}
 else if(!p[2]){
  assert(bytes==23);uint32_t sh=read32(p+19);
  r->salt=sh==7?((uint64_t)(finishes>=3?1:2)<<32)|5:(sh==EFS_SESSION_BITS?2:0);
 }
 else if(p[2]==EFS_MD_SESS_FENCE_LOC){assert(bytes==27&&read32(p+19)==5&&read32(p+23)==1);}
 else {assert(p[2]==EFS_MD_SESS_ACK&&bytes==23&&read32(p+19)==1);}
 return EFS_OK;
}
static int sess_cleanup_bounded(const char *host,uint16_t port,const uint8_t *id,uint32_t old,uint32_t shard,int append,struct efs_msg_raft_mkfs_reply *r){
 (void)host;(void)port;(void)r;assert(!memcmp(id,identity,16)&&old==4&&shard==1&&append==(int)cleanups);cleanups++;return EFS_OK;
}
'''+function('int sess_storage_barrier(')+'\n'+function('void sess_pack_hdr(')+'\n'+function('int cmd_raft_session_fence(')+r'''
int main(void){identity[0]=42;assert(!cmd_raft_session_fence("seed",1,identity));assert(begins==1&&finishes==3&&cleanups==2&&deadline==0);return 0;}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)/'test.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('session FINISH: BUSY retries keep UUID/epoch, one BEGIN, both cleanup stages PASS')
