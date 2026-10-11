#!/usr/bin/env python3
"""Actual replay helper must distinguish a missing and materializing receipt."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_opid_replay(');b=s.index('\n}',a)+2
source=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_IO -1
#define EFS_KV_KEY_MAX 512
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;uint64_t seq;};
struct efs_opid_req {struct efs_opid id;uint64_t ack;};
struct efs_opid_reply {int rc;uint64_t ino;};
struct efs_raft_host {void *kv;uint64_t obs_opid_replay;};
static unsigned probes,checks,keys;
static int result,pending,key_error,resolve_during_check;
static int efs_opid_req_valid(const struct efs_opid_req *q){return q && q->id.seq && q->id.client_uuid[0];}
static int efs_meta_apply_opid_probe(void *kv,uint32_t shard,const struct efs_opid *id,struct efs_opid_reply *rep){
 (void)kv;assert(shard==1 && id->client_uuid[0]==17);probes++;
 if(result==1){rep->rc=0;rep->ino=4242;}return result;
}
static int efs_kv_key_opid(uint32_t shard,const uint8_t *uuid,uint32_t epoch,uint8_t *key,uint32_t *bytes){
 assert(shard==1 && uuid[0]==17 && epoch==1);keys++;*bytes=3;memset(key,19,3);return key_error;
}
static int efs_txn_key_busy(void *kv,const uint8_t *key,uint32_t bytes){
 (void)kv;assert(bytes==3 && key[0]==19);checks++;
 if(resolve_during_check)result=1;
 return pending;
}
''' + s[a:b] + r'''
static void reset(void){probes=checks=keys=0;result=pending=key_error=resolve_during_check=0;}
int main(void){
 struct efs_raft_host h={0};struct efs_opid_req q={.id={.client_uuid={17},.session_epoch=1,.seq=1}};struct efs_opid_reply rep={0};
 reset();pending=EFS_ERR_BUSY;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_BUSY);assert(probes==1 && checks==1 && !h.obs_opid_replay);
 reset();pending=EFS_ERR_IO;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_IO);assert(probes==1 && checks==1);
 reset();key_error=EFS_ERR_IO;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_IO);assert(probes==1 && !checks);
 reset();resolve_during_check=1;assert(host_opid_replay(&h,1,&q,&rep)==1 && rep.ino==4242);assert(probes==2 && checks==1 && h.obs_opid_replay==1);
 reset();result=1;pending=EFS_ERR_BUSY;assert(host_opid_replay(&h,1,&q,&rep)==1);assert(probes==1 && !keys && h.obs_opid_replay==2);
 reset();assert(!host_opid_replay(&h,1,&q,&rep));assert(probes==2 && checks==1);
 reset();result=EFS_ERR_IO;assert(host_opid_replay(&h,1,&q,&rep)==EFS_ERR_IO);assert(probes==1 && !keys);
 reset();assert(!host_opid_replay(&h,1,NULL,&rep));assert(!probes && !keys);
 reset();q.id.session_epoch=0;assert(!host_opid_replay(&h,1,&q,&rep));assert(probes==1 && !keys);
 puts("Namespace replay: pending receipt refusal, concurrent materialization re-probe, existing verdict and hard-error propagation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-receipt-') as td:
 p=Path(td)/'test.c';p.write_text(source);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
