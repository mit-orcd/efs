#!/usr/bin/env python3
"""Actual REPORT first-touch helper: bounded workers, dedup, refusal and joins."""
from pathlib import Path
import subprocess,tempfile,os,shlex
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text();a=s.index('struct host_report_session_work {');b=s.index('/* I16 leader-side probe:',a)
source=r'''
#include <pthread.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#define EFS_SESSION_BITS 64
#define EFS_META_LANES 8
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_NOT_FOUND -4
#define EFS_ERR_INVAL -1
#define EFS_ERR_STALE -7
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;uint64_t seq;};
struct efs_opid_req {struct efs_opid id;uint64_t ack;};
struct efs_chunk_rec {uint64_t ino;uint32_t chunk_index;};
struct efs_raft_host {void *kv;};
static unsigned calls[64],active,max_active,create_calls,fail_after=99;
static int existing,refusal,fail_shard=-1;
static uint32_t efs_kv_inode_shard(uint64_t ino) {return ino==999?64:1;}
static uint32_t efs_kv_lane_shard(uint64_t ino,uint32_t lane) {(void)ino;return 2+lane;}
static int efs_session_accept(void *kv,uint32_t shard,const uint8_t *uuid,uint32_t epoch) {
 (void)kv;assert(uuid[0]==17&&epoch==2);return refusal ? refusal : existing ? EFS_OK : shard%2 ? EFS_ERR_BUSY : EFS_ERR_NOT_FOUND;
}
static int host_namespace_session_admit(struct efs_raft_host *h,uint32_t shard,const struct efs_opid_req *q,int *hint) {
 (void)h;assert(q->id.client_uuid[0]==17 && q->id.session_epoch==2 && q->id.seq==1 && !q->ack);
 __atomic_add_fetch(&calls[shard],1,__ATOMIC_RELAXED);
 unsigned n=__atomic_add_fetch(&active,1,__ATOMIC_RELAXED),old;
 do {old=__atomic_load_n(&max_active,__ATOMIC_RELAXED);if(old>=n)break;} while(!__atomic_compare_exchange_n(&max_active,&old,n,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
 usleep(10000);__atomic_sub_fetch(&active,1,__ATOMIC_RELAXED);
 *hint=3;return (int)shard==fail_shard ? EFS_ERR_STALE : EFS_OK;
}
static int tracked_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg) {
 if(create_calls++>=fail_after)return EAGAIN;
 return pthread_create(thread,attr,fn,arg);
}
#define pthread_create tracked_create
''' + s[a:b] + r'''
#undef pthread_create
static void reset(void) {memset(calls,0,sizeof(calls));active=max_active=create_calls=0;existing=refusal=0;fail_shard=-1;fail_after=99;}
int main(void) {
 struct efs_raft_host h={0};struct efs_opid session={.client_uuid={17},.session_epoch=2};int hint=-1;
 struct efs_chunk_rec recs[16];for(unsigned i=0;i<16;i++)recs[i]=(struct efs_chunk_rec){1,i};
 for(unsigned allowed=0;allowed<=3;allowed++) {
  reset();fail_after=allowed;assert(!host_report_session_admit(&h,recs,16,&session,&hint));
  assert(!active && max_active<=allowed+1 && max_active>=1);
  for(unsigned shard=1;shard<10;shard++)assert(calls[shard]==1);
 }
 reset();assert(!pthread_mutex_lock(&host_report_session_parallel));
 assert(!host_report_session_admit(&h,recs,16,&session,&hint));
 assert(!pthread_mutex_unlock(&host_report_session_parallel));assert(max_active==1 && !create_calls && !active);
 reset();existing=1;assert(!host_report_session_admit(&h,recs,16,&session,&hint));assert(!active && !create_calls);
 reset();refusal=EFS_ERR_STALE;assert(host_report_session_admit(&h,recs,16,&session,&hint)==EFS_ERR_STALE);assert(!active && !create_calls);
 reset();fail_shard=1;hint=-1;assert(host_report_session_admit(&h,recs,16,&session,&hint)==EFS_ERR_STALE);
 assert(hint==3 && !active && max_active<=4);for(unsigned i=0;i<64;i++)assert(calls[i]<=1);
 reset();recs[0].ino=999;assert(host_report_session_admit(&h,recs,16,&session,&hint)==EFS_ERR_INVAL);assert(!active && !create_calls);
 puts("REPORT sessions: exact dedup, bounded four-way admission, serial/create-failure fallback, fencing and joined error ownership PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-session-') as td:
 p=Path(td)/'test.c';p.write_text(source);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
