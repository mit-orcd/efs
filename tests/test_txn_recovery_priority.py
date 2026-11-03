#!/usr/bin/env python3
"""Execute the real bounded recovery scheduler: known intents precede cold discovery."""
from pathlib import Path
import os, shlex, subprocess, tempfile
import csrc
root = Path(__file__).resolve().parents[1]
s = csrc.module('raft_host')
def function(prefix):
    start = s.index(prefix, s.index('#define HOST_REC_AGE_NS'))
    return s[start:s.index('\n}', start)+2]
macros = '\n'.join(line for line in s.splitlines() if line.startswith((
    '#define HOST_REC_AGE_NS ', '#define HOST_REC_SHARDS ', '#define HOST_REC_MAX ',
    '#define HOST_REC_YIELD_US ')))
code = r'''
#include "efs/txn.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct efs_raft_host {
 struct efs_kv *kv; int gc_running,running;
 pthread_mutex_t rec_mu;
 uint64_t rec_mark[4096],rec_generation[4096],rec_scans;
 uint8_t rec_observed[4096];uint32_t rec_hot_next,rec_cold_next;
};
static struct efs_raft_host host;
static uint64_t clock_ns;
static unsigned scans,recovered,inject,scan_count[4096];
static uint8_t pending[4096];static int leads[2];
static uint64_t now_ns(void){return clock_ns;}
static int usleep(unsigned n){(void)n;return 0;}
static int host_leads(struct efs_raft_host*h,uint8_t group){(void)h;return leads[group==EFS_RAFT_GROUP_SHARD2];}
uint32_t efs_txn_coordinator(const struct efs_txid*t,const struct efs_txn_parts*p){(void)t;return p->shard[0];}
int efs_txn_decision_get(struct efs_kv*kv,uint32_t sh,const struct efs_txid*t,int*d){(void)kv;(void)sh;(void)t;(void)d;return EFS_ERR_NOT_FOUND;}
static void host_txn_recover_one(struct efs_raft_host*h,uint32_t sh,const struct efs_txn_pending_rec*r){(void)h;(void)r;recovered++;pending[sh]=0;}
static unsigned host_txn_decision_sweep(struct efs_raft_host*h,uint32_t sh,uint64_t now){(void)h;(void)sh;(void)now;return 0;}
''' + macros + '\n' + function('static void host_rec_mark(') + '\n' + function('static void host_rec_mark_all(') + '\n' + function('static uint64_t txid_ns(') + r'''
int efs_txn_scan_pending(struct efs_kv*kv,uint32_t sh,struct efs_txn_pending_rec*r,uint32_t cap,uint32_t*n){
 (void)kv;assert(cap);scans++;scan_count[sh]++;
 if(inject && sh==3820){inject=0;host_rec_mark(&host,sh);pending[sh]=1;*n=0;return EFS_OK;}
 *n=pending[sh];
 if(*n){memset(r,0,sizeof(*r));uint64_t born=clock_ns-6000000000ull;memcpy(r->t.bytes,&born,8);r->parts.n=1;r->parts.shard[0]=sh;}
 return EFS_OK;
}
''' + function('static void host_txn_recover_pass(') + r'''
static void reset(void){
 memset(&host,0,sizeof(host));assert(!pthread_mutex_init(&host.rec_mu,0));
 host.kv=(struct efs_kv *)(uintptr_t)1;host.gc_running=host.running=1;
 clock_ns=10000000000ull;scans=recovered=inject=0;leads[0]=leads[1]=1;
 memset(pending,0,sizeof(pending));memset(scan_count,0,sizeof(scan_count));
}
static void finish(void){assert(!pthread_mutex_destroy(&host.rec_mu));}
int main(void){
 reset();host_rec_mark_all(&host);pending[3820]=1;host_rec_mark(&host,3820);clock_ns+=6000000000ull;
 host_txn_recover_pass(&host);
 fprintf(stderr,"high-shard recovery: recovered=%u scans=%u cap=%u\n",recovered,scans,HOST_REC_SHARDS);
 assert(recovered==1 && !pending[3820] && scan_count[3820]==1 && scans==HOST_REC_SHARDS);finish();
 /* A concurrent PREPARE must survive a scan that saw no record. */
 reset();host_rec_mark(&host,3820);clock_ns+=6000000000ull;inject=1;
 host_txn_recover_pass(&host);assert(!recovered && pending[3820] && host.rec_mark[3820]);
 host_txn_recover_pass(&host);assert(recovered==1 && !pending[3820]);finish();
 /* Both queues make progress, with one scan per shard/pass and no
  * increase to the 64-shard work limit. Cursors rotate past hot shards. */
 reset();host_rec_mark_all(&host);
 for(unsigned i=100;i<200;i++)host_rec_mark(&host,i);
 clock_ns+=6000000000ull;host_txn_recover_pass(&host);
 unsigned hot=0,cold=0;
 for(unsigned i=0;i<4096;i++){assert(scan_count[i]<=1);if(i>=100&&i<200)hot+=scan_count[i];else cold+=scan_count[i];}
 assert(scans==HOST_REC_SHARDS && hot>=HOST_REC_SHARDS/2 && cold>=HOST_REC_SHARDS/2);
 host_txn_recover_pass(&host);assert(scans==2*HOST_REC_SHARDS);
 for(unsigned i=0;i<4096;i++){assert(scan_count[i]<=1);}
 finish();
 /* Empty cold discovery lends its unused work to known intents. */
 reset();for(unsigned i=100;i<200;i++)host_rec_mark(&host,i);
 clock_ns+=6000000000ull;host_txn_recover_pass(&host);assert(scans==HOST_REC_SHARDS);finish();
 /* Cold discovery that finds a record promotes it for timely retry and
  * confirmation instead of waiting for another entire cold walk. */
 reset();host_rec_mark_all(&host);pending[0]=1;clock_ns+=6000000000ull;
 host_txn_recover_pass(&host);assert(recovered==1 && host.rec_observed[0]);
 clock_ns+=6000000000ull;host_txn_recover_pass(&host);assert(!host.rec_mark[0]);finish();
 /* A follower does no recovery, and future/young marks are not due. */
 reset();host_rec_mark(&host,3820);clock_ns+=4000000000ull;host_txn_recover_pass(&host);assert(!scans);
 clock_ns-=5000000000ull;host_txn_recover_pass(&host);assert(!scans);
 clock_ns+=10000000000ull;leads[0]=leads[1]=0;host_txn_recover_pass(&host);assert(!scans);finish();
 reset();host_rec_mark(&host,3820);clock_ns+=6000000000ull;leads[1]=0;
 host_txn_recover_pass(&host);assert(!scans && host.rec_mark[3820]);finish();
 puts("txn recovery: high-shard priority, concurrent marks, bounded fair discovery and age/leadership gates PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-recovery-priority-') as d:
    p=Path(d);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-pthread','-Wall','-Wextra','-Werror',
                   '-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
