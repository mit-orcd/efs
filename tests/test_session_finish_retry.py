#!/usr/bin/env python3
"""Execute the shared fence coordinator: FINISH keeps its epoch while certificates are BUSY."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('SESSION_BARRIER_SOURCE',root/'src/common/session_revoke.c')).read_text()
def function(name):
    a=s.index(name)
    return s[a:s.index('\n}',a)+2]
# The per-shard stage runner the coordinator calls (parallel=0: in the caller).
stages=s[s.index('static void wr64be_sess('):s.index('/* Coordinator-driven I23 barrier')]
code=r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/session.h"
#include "efs/session_revoke.h"
static uint64_t now_ms=1000,deadline;
static unsigned begins,finishes,cleanups,floor_calls,floor_failures;
static int floor_error=EFS_ERR_NOT_PRIMARY;
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
static void wr32be_sess(uint8_t *p,uint32_t x){p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static uint32_t read32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int seed;
static int sess_rpc(const struct efs_session_rpc *rpc,const uint8_t *p,uint32_t bytes,struct efs_msg_raft_mkfs_reply *r){
 if(p[1]==EFS_MD_REQ_SESSION_CLEANUP_PROBE){assert(bytes==34&&!memcmp(p+2,identity,16)&&finishes>=3);
  memset(r,0,sizeof(*r));r->salt=~0ull;return EFS_OK;} /* every shard still owes cleanup */
 assert(rpc->ctx==&seed&&!memcmp(p+3,identity,16));
 memset(r,0,sizeof(*r));r->leader_hint=-1;
 if(p[2]==EFS_MD_SESS_BEGIN){assert(bytes==19);begins++;}
 else if(p[2]==EFS_MD_SESS_STORAGE_FENCE){assert(bytes==23&&read32(p+19)==5);}
 else if(p[2]==EFS_MD_SESS_FINISH){assert(bytes==23&&read32(p+19)==5);finishes++;if(finishes<3)return EFS_ERR_BUSY;}
 else if(!p[2]){
  assert(bytes==23);uint32_t sh=read32(p+19);
  r->salt=sh==7?((uint64_t)(finishes>=3?1:2)<<32)|5:(sh==EFS_SESSION_BITS?2:0);
 }
 else if(p[2]==EFS_MD_SESS_FENCE_LOC){assert(bytes==35&&read32(p+19)==5&&read32(p+23)==0&&!read32(p+27)&&read32(p+31)==2);
  floor_calls++;if(floor_failures){floor_failures--;return floor_error;}}
 else {assert(p[2]==EFS_MD_SESS_ACK&&bytes==35&&read32(p+19)==5&&read32(p+23)==0&&!read32(p+27)&&read32(p+31)==2);}
 return EFS_OK;
}
int efs_session_cleanup_bounded(const struct efs_session_rpc *rpc,const uint8_t id[EFS_OPID_UUID_LEN],uint32_t old,uint32_t shard,int append,struct efs_msg_raft_mkfs_reply *r){
 assert(rpc->ctx==&seed);(void)r;assert(!memcmp(id,identity,16)&&old==4&&shard==1&&append==(int)cleanups);cleanups++;return EFS_OK;
}
'''+function('int efs_session_storage_barrier(')+'\n'+function('static void sess_pack_hdr(')+'\n'+stages+function('int efs_session_revoke(')+r'''
int main(void){identity[0]=42;const struct efs_session_rpc rpc={&seed,NULL,0,NULL};struct efs_session_revoke_result res;
 assert(!efs_session_revoke(&rpc,identity,&res)&&res.epoch==5&&res.state==1&&res.ntouch==1);assert(begins==1&&finishes==3&&cleanups==2&&deadline==0);
 uint8_t p[35]={0};struct efs_msg_raft_mkfs_reply reply;
 sess_pack_hdr(p,0,EFS_MD_SESS_FENCE_LOC,identity);wr32be_sess(p+19,5);wr32be_sess(p+31,2);
 now_ms=1000;floor_calls=0;floor_failures=2;
 assert(!efs_session_storage_barrier(&rpc,p,sizeof(p),&reply)&&floor_calls==3&&!deadline);
 /* Permanent non-admission obeys the shorter inherited deadline. */
 now_ms=1000;deadline=1200;floor_calls=0;floor_failures=1000;
 assert(efs_session_storage_barrier(&rpc,p,sizeof(p),&reply)==EFS_ERR_NOT_PRIMARY);
 assert(floor_calls==2&&now_ms==1200&&deadline==1200);
 /* Ambiguous transport errors are still never resent. */
 now_ms=1000;deadline=0;floor_calls=0;floor_failures=1;floor_error=EFS_ERR_NET;
 assert(efs_session_storage_barrier(&rpc,p,sizeof(p),&reply)==EFS_ERR_NET&&floor_calls==1&&!deadline);
 return 0;}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)/'test.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('session FINISH: BUSY retries keep UUID/epoch, one BEGIN, both cleanup stages, same-epoch handoff retries, permanent deadline and no ambiguous resend PASS')

# BEGIN cannot be starved by soft new-work admission: it freezes an existing
# session before cleanup. Execute the production predicate at memory/log
# pressure; new session/namespace work must remain refused.
host=Path(os.environ.get('SESSION_ADMIT_SOURCE',root/'src/server/raft_host.c')).read_text()
a=host.index('static int host_admit(');body=host[a:host.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#define EFS_RAFT_LEADER 1
#define HOST_SESS_HDR_LEN 18u
struct efs_raft_host {void *kv;uint64_t admit_busy;};
struct efs_raft {int leader,memory;uint64_t pending;};
struct efs_raft_log_progress {uint64_t unapplied,max_unapplied;};
static int efs_raft_role(struct efs_raft*r){return r->leader;}
static int efs_kv_lsm_apply_room(void*p,int n){assert(n==1);return ((struct efs_raft*)p)->memory?EFS_ERR_BUSY:EFS_OK;}
static int efs_raft_log_progress(struct efs_raft*r,struct efs_raft_log_progress*p){p->max_unapplied=100;p->unapplied=r->pending;return EFS_OK;}
'''+body+r'''
int main(void){struct efs_raft r={.leader=1};struct efs_raft_host h={.kv=&r};
 uint8_t begin[HOST_SESS_HDR_LEN+1]={EFS_MD_CMD_SESSION,EFS_MD_SESS_BEGIN};begin[2]=42;
 uint8_t create[]={EFS_MD_CMD_CREATE};
 for(unsigned memory=0;memory<2;memory++){
  r.memory=memory;r.pending=memory?0:75;
  assert(!host_admit(&h,&r,begin,HOST_SESS_HDR_LEN));
  assert(host_admit(&h,&r,create,sizeof(create))==EFS_ERR_BUSY);
  begin[1]=EFS_MD_SESS_CREATE;assert(host_admit(&h,&r,begin,HOST_SESS_HDR_LEN)==EFS_ERR_BUSY);
  begin[1]=EFS_MD_SESS_REGISTER;assert(host_admit(&h,&r,begin,HOST_SESS_HDR_LEN)==EFS_ERR_BUSY);
  begin[1]=EFS_MD_SESS_BEGIN;
  assert(host_admit(&h,&r,begin,HOST_SESS_HDR_LEN-1)==EFS_ERR_BUSY);
  assert(host_admit(&h,&r,begin,HOST_SESS_HDR_LEN+1)==EFS_ERR_BUSY);
 }
 r.memory=0;r.pending=74;assert(!host_admit(&h,&r,create,sizeof(create)));
 puts("session BEGIN: cleanup reserve at soft memory/log pressure, exact encoding, new work still refused PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-begin-admit-') as directory:
    p=Path(directory)/'test.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
