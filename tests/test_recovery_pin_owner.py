#!/usr/bin/env python3
"""Execute production recovery scheduling: retained pins keep their decision owner."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static void host_txn_recover_pass(');b=s.index('\n/* W125:',a)
code=r'''
#include <efs/txn.h>
#include <efs/kv_key.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HOST_REC_MAX 32
#define HOST_REC_SHARDS 64
#define HOST_REC_AGE_NS 5000000000ULL
#define HOST_REC_YIELD_US 1000
#define EFS_RAFT_GROUP_SHARD 0
#define EFS_RAFT_GROUP_SHARD2 2
struct efs_raft_host {struct efs_kv *kv;pthread_mutex_t rec_mu;uint64_t rec_mark[4096];unsigned rec_scans;int running,gc_running;};
static int pin_only,decision_rc,recovered,swept;
static uint64_t now_ns(void){return 100000000000ULL;}
static uint64_t txid_ns(const struct efs_txid *t){(void)t;return 1;}
static int host_leads(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return 1;}
int efs_txn_scan_pending(struct efs_kv *kv,uint32_t shard,struct efs_txn_pending_rec *out,uint32_t cap,uint32_t *n){(void)kv;assert(shard==1&&cap==32);memset(out,0,sizeof(*out));out->parts.n=1;out->parts.shard[0]=1;out->pin_only=pin_only;*n=1;return EFS_OK;}
uint32_t efs_txn_coordinator(const struct efs_txid *t,const struct efs_txn_parts *p){(void)t;return p->shard[0];}
int efs_txn_decision_get(struct efs_kv *kv,uint32_t shard,const struct efs_txid *t,int *decision){(void)kv;(void)t;assert(shard==1);*decision=EFS_TXN_ABORT;return decision_rc;}
static void host_txn_recover_one(struct efs_raft_host *h,uint32_t shard,const struct efs_txn_pending_rec *rec){(void)h;(void)rec;assert(shard==1);recovered++;}
static uint32_t host_txn_decision_sweep(struct efs_raft_host *h,uint32_t shard,uint64_t now){(void)h;assert(shard==1&&now==now_ns());swept++;return 1;}
static int fake_usleep(unsigned us){assert(us==1000);return 0;}
#define usleep fake_usleep
''' + s[a:b] + r'''
static void run(int pin,int rc,int recover){struct efs_kv kv={0};struct efs_raft_host h={.kv=&kv,.running=1,.gc_running=1,.rec_mu=PTHREAD_MUTEX_INITIALIZER};h.rec_mark[1]=1;pin_only=pin;decision_rc=rc;recovered=swept=0;host_txn_recover_pass(&h);assert(recovered==recover&&swept==1&&h.rec_mark[1]==now_ns());pthread_mutex_destroy(&h.rec_mu);}
int main(void){
 run(1,EFS_OK,0); /* decided pin remains with decision sweep, never discarded */
 run(1,EFS_ERR_NOT_FOUND,1); /* an undecided pin still drives recovery */
 run(1,EFS_ERR_STALE,1); /* reclaimed decision needs ordinary stale cleanup */
 run(1,EFS_ERR_IO,0); /* unreadable authority is never an abort certificate */
 run(0,EFS_OK,1);run(0,EFS_ERR_NOT_FOUND,1); /* actual staged work unchanged */
 puts("Recovery: decided retention pin uses decision sweep; undecided/mixed payload recovers; I/O never authorizes ABORT PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-recovery-pin-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
