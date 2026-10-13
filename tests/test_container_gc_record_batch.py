#!/usr/bin/env python3
"""Actual container record dispatcher: bounded ownership, join and refusal."""
import os, shlex, subprocess, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('EFS_RAFT_HOST_SOURCE', root/'src/server/raft_host.c')).read_text()
a = source.index('#define HOST_GC_RECORD_BATCH 32')
b = source.index('/* REAP pass over one group', a)
prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define EFS_NUM_FRAGMENTS 3
struct efs_raft_host {int gc_running,running;};
struct efs_export {int id;};
struct gc_scan_ctx {uint8_t keys[256][24],vals[256][8];};
struct efs_gc_ack_item {uint64_t ino,gen;uint32_t ci;uint8_t lane,frag;};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int permits,created,active,peak,arrived,participants,released,calls[256];
static int create_worker(pthread_t *t,const pthread_attr_t *attr,void *(*fn)(void *),void *v){
 if(created>=permits)return EAGAIN;
 created++;return pthread_create(t,attr,fn,v);
}
static void host_gc_record(struct efs_raft_host *h,struct efs_export *ex,
 const uint8_t *key,const uint8_t *val,struct efs_gc_ack_item *acks,int *nack){
 int i=key[0];assert(h->running && h->gc_running && ex->id==7 && val[0]==255-i);
 assert(!*nack);
 pthread_mutex_lock(&mu);calls[i]++;active++;if(active>peak)peak=active;
 if(!released){arrived++;if(arrived==participants){released=1;pthread_cond_broadcast(&cv);}
 while(!released)pthread_cond_wait(&cv,&mu);}
 pthread_mutex_unlock(&mu);
 /* Private results represent the existing durable-delete contract:
  * rejected fragment 2 and every fragment of record 9 produce no ACK. */
 if(i!=9)for(int fi=0;fi<EFS_NUM_FRAGMENTS;fi++)if(fi!=2){
  acks[*nack]=(struct efs_gc_ack_item){.ino=(uint64_t)i,.gen=1000+(uint64_t)i,
   .ci=(uint32_t)i+300,.lane=(uint8_t)(i%64),.frag=(uint8_t)fi};(*nack)++;
 }
 pthread_mutex_lock(&mu);active--;pthread_mutex_unlock(&mu);
}
#define pthread_create create_worker
'''
suffix = r'''
#undef pthread_create
int main(void){
 struct efs_raft_host h={1,1};struct efs_export ex={7};struct gc_scan_ctx scan={0};
 for(int i=0;i<256;i++){scan.keys[i][0]=(uint8_t)i;scan.vals[i][0]=(uint8_t)(255-i);}
 for(int limit=0;limit<=3;limit++)for(int first=0;first<256;first+=32){
  permits=limit;created=active=peak=arrived=released=0;participants=limit+1;
  memset(calls,0,sizeof(calls));
  struct host_gc_record_batch batch={.h=&h,.ex=&ex,.scan=&scan,.first=first,.count=32};
  host_gc_records_join(&batch);
  assert(!active && created==limit && peak==limit+1);
  for(int i=0;i<256;i++)assert(calls[i]==(i>=first && i<first+32));
  for(int i=0;i<32;i++){
   int record=first+i;assert(batch.nack[i]==(record==9?0:2));
   for(int j=0;j<batch.nack[i];j++){
    struct efs_gc_ack_item *ack=&batch.acks[i][j];
    assert(ack->ino==(uint64_t)record && ack->gen==1000+(uint64_t)record &&
     ack->ci==(uint32_t)record+300 && ack->lane==record%64 && ack->frag!=2);
   }
  }
 }
 struct host_gc_record_batch batch={.h=&h,.ex=&ex,.scan=&scan,.count=1};
 permits=3;created=active=peak=arrived=released=0;participants=1;
 host_gc_records_join(&batch);assert(!created && !active && batch.nack[0]==2);
 for(int state=0;state<4;state++){
  memset(&batch,0,sizeof(batch));batch.h=&h;batch.ex=&ex;batch.scan=&scan;
  h.running=state!=0;h.gc_running=state!=1;batch.count=state==2?0:state==3?33:32;
  created=0;host_gc_records_join(&batch);assert(!created && !batch.next);
 }
 puts("container fragment dispatcher: bounded four participants, all joins, private per-record identities/ACKs, failures, creation fallback and stop PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-record-') as directory:
    p = Path(directory)/'test.c'
    p.write_text(prefix+source[a:b]+suffix)
    flags = ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
             *shlex.split(os.environ.get('EFS_TEST_CFLAGS', '')), '-pthread']
    subprocess.run([*flags, str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True, timeout=15)
    # Disabling helper launch must fail the actual overlap control.
    p.write_text(prefix+source[a:b].replace('while(n<3 &&', 'while(n<0 &&')+suffix)
    subprocess.run([*flags, str(p), '-o', str(p.with_suffix(''))], check=True)
    try:
        negative = subprocess.run([str(p.with_suffix(''))], timeout=2)
        assert negative.returncode, 'serial negative unexpectedly passed'
    except subprocess.TimeoutExpired:
        pass
    print('disabled parallel dispatcher negative refused as expected')

body = source[source.index('static int host_gc_frag_pass('):source.index('static void host_dir_spread_pass(')]
assert 'h->s->storage_engine && !h->s->repair_experiment' in body
assert body.index('host_gc_records_join(&batch)') < body.index('batch.acks[record]')
assert 'nack+EFS_NUM_FRAGMENTS>GC_ACK_MAX' in body
assert 'host_gc_record(h, ex, c.keys[i], c.vals[i], acks, &nack)' in body
assert body.index('server_export_put(') > body.index('host_gc_records_join(')
