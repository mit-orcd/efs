#!/usr/bin/env python3
"""A11 finding 1: the revoke coordinator runs per-shard steps in parallel.

Compiles src/common/session_revoke.c against a thread-safe fake owner with
1500 touched shards and checks, for parallel=16 and parallel=0 (caller only):
- every shard gets FENCE_LOC before its ACK, every ACK precedes FINISH and
  every cleanup follows FINISH, each step exactly once per shard; FENCE_LOC
  and ACK carry one bitmap word's shards of one Raft group (odd or even);
- parallel=16 really overlaps steps (more than one in flight) and is faster
  than serial when each step costs a commit latency; parallel=0 never
  leaves the calling thread;
- progress reaches done == total for both stages;
- cleanup asks one probe per word and group and cleans only the shards
  it does not prove empty; a failed probe cleans every shard;
- a failed FENCE_LOC stops the stage (no FINISH, phase participant-floor), a
  failed cleanup names its shard (phase cleanup), never a completed revoke.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include "efs/network.h"
#include "efs/session.h"
#include "efs/session_revoke.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static __thread uint64_t deadline;
void efs_net_set_deadline_ms(uint64_t d){deadline=d;}
uint64_t efs_net_deadline_ms(void){return deadline;}
int efs_net_remaining_ms(int cap){(void)cap;return 1000;}
uint32_t efs_kv_session_shard(const uint8_t *u){(void)u;return 3;}
#define N 600
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_t caller;
static unsigned fenced[EFS_SESSION_BITS],acked[EFS_SESSION_BITS],cleaned[EFS_SESSION_BITS][2];
static unsigned probe_fail,probes,inflight,maxflight,finished,foreign,step_us=10,fail_fence,fail_clean,progress_fence,progress_clean;
static uint32_t shard_of(unsigned i){return (i*7u)%EFS_SESSION_BITS;}
static int touched(uint32_t sh){for(unsigned i=0;i<N;i++)if(shard_of(i)==sh)return 1;return 0;}
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void enter(void)
{
    pthread_mutex_lock(&mu);
    if(!pthread_equal(pthread_self(),caller))foreign++;
    if(++inflight>maxflight)maxflight=inflight;
    pthread_mutex_unlock(&mu);
    if(step_us)usleep(step_us);
}
static void leave(void){pthread_mutex_lock(&mu);inflight--;pthread_mutex_unlock(&mu);}
static int submit(void *ctx,const uint8_t *p,uint32_t n,struct efs_msg_raft_mkfs_reply *r)
{
    (void)ctx;memset(r,0,sizeof(*r));r->leader_hint=-1;
    if(p[1]==EFS_MD_REQ_SESSION_CLEANUP_PROBE) {
        /* Shards with sh%5==0 still owe cleanup; the rest are proven empty. */
        uint32_t word=rd32(p+22);uint64_t mask=(uint64_t)rd32(p+26)<<32|rd32(p+30);
        assert(n==1+EFS_MD_REQ_SESSION_CLEANUP_PROBE_LEN&&rd32(p+18)==1&&mask);
        enter();pthread_mutex_lock(&mu);assert(finished);probes++;pthread_mutex_unlock(&mu);leave();
        if(probe_fail)return r->rc=EFS_ERR_BUSY;
        for(unsigned b=0;b<64;b++)if(mask>>b&1&&(word*64+b)%5==0)r->salt|=1ull<<b;
        return r->rc=EFS_OK;
    }
    if(p[1]==EFS_MD_REQ_SESSION_APPEND_NEXT) {
        uint32_t sh=rd32(p+22);enter();pthread_mutex_lock(&mu);
        assert(finished&&touched(sh)&&cleaned[sh][0]==1&&!cleaned[sh][1]);cleaned[sh][1]++;
        pthread_mutex_unlock(&mu);leave();r->salt=1;return r->rc=EFS_OK;
    }
    assert(p[1]==EFS_MD_CMD_SESSION);
    switch(p[2]) {
    case EFS_MD_SESS_BEGIN: return r->rc=EFS_OK;
    case 0: {
        uint32_t sh=rd32(p+19);
        if(sh==3){r->salt=(uint64_t)(finished?EFS_SESSION_ACTIVE:EFS_SESSION_FENCING)<<32|2;return EFS_OK;}
        assert(sh>=EFS_SESSION_BITS);
        for(unsigned b=0;b<64;b++)if(touched((sh-EFS_SESSION_BITS)*64+b))r->salt|=1ull<<b;
        return EFS_OK;
    }
    case EFS_MD_SESS_STORAGE_FENCE: return r->rc=EFS_OK;
    case EFS_MD_SESS_FENCE_LOC: {
        uint32_t word=rd32(p+23);uint64_t mask=(uint64_t)rd32(p+27)<<32|rd32(p+31);
        assert(n==1+EFS_MD_SESS_FENCE_LOC_LEN&&rd32(p+19)==2&&mask);
        /* One group per FENCE_LOC: only odd or only even shards. */
        assert(!(mask&0x5555555555555555ull)||!(mask&0xaaaaaaaaaaaaaaaaull));
        enter();pthread_mutex_lock(&mu);int fail=0;
        for(unsigned b=0;b<64;b++)if(mask>>b&1){
            uint32_t sh=word*64+b;assert(touched(sh)&&!fenced[sh]&&!finished);fenced[sh]++;
            fail|=fail_fence&&sh==shard_of(N/2);
        }
        pthread_mutex_unlock(&mu);leave();
        return r->rc=fail?EFS_ERR_IO:EFS_OK;
    }
    case EFS_MD_SESS_ACK: {
        uint32_t word=rd32(p+19);uint64_t mask=(uint64_t)rd32(p+23)<<32|rd32(p+27);
        assert(n==1+EFS_MD_SESS_ACK_LEN&&mask);
        enter();pthread_mutex_lock(&mu);
        for(unsigned b=0;b<64;b++)if(mask>>b&1){
            uint32_t sh=word*64+b;assert(fenced[sh]==1&&!acked[sh]);acked[sh]++;
        }
        pthread_mutex_unlock(&mu);leave();
        return r->rc=EFS_OK;
    }
    case EFS_MD_SESS_FINISH:
        pthread_mutex_lock(&mu);
        for(unsigned i=0;i<N;i++)assert(acked[shard_of(i)]==1);
        assert(!inflight);finished=1;pthread_mutex_unlock(&mu);return r->rc=EFS_OK;
    case EFS_MD_SESS_LEASE_LOCK_NEXT: {
        uint32_t sh=rd32(p+23);enter();pthread_mutex_lock(&mu);
        assert(finished&&touched(sh)&&!cleaned[sh][0]);cleaned[sh][0]++;pthread_mutex_unlock(&mu);leave();
        if(fail_clean&&sh==shard_of(N/3))return r->rc=EFS_ERR_NET;
        r->salt=1;return r->rc=EFS_OK;
    }
    }
    assert(!"unexpected request");return EFS_ERR_INVAL;
}
static void progress(void *ctx,const char *stage,uint32_t done,uint32_t total)
{
    (void)ctx;assert(total==N&&done>=1&&done<=N);
    pthread_mutex_lock(&mu);
    unsigned *top=strcmp(stage,"cleanup")?&progress_fence:&progress_clean;
    if(done>*top)*top=done;
    pthread_mutex_unlock(&mu);
}
static double run(unsigned parallel,int expect,const char *phase,uint32_t shard)
{
    memset(fenced,0,sizeof(fenced));memset(acked,0,sizeof(acked));memset(cleaned,0,sizeof(cleaned));
    inflight=maxflight=finished=foreign=progress_fence=progress_clean=probes=0;caller=pthread_self();
    uint8_t uuid[16]={9};
    const struct efs_session_rpc rpc={NULL,submit,parallel,progress};
    struct efs_session_revoke_result res;
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    int rc=efs_session_revoke(&rpc,uuid,&res);
    clock_gettime(CLOCK_MONOTONIC,&b);
    if(rc!=expect)fprintf(stderr,"rc=%d phase=%s\n",rc,res.phase);
    assert(rc==expect&&res.ntouch==N);
    if(phase)assert(!strcmp(res.phase,phase)&&res.shard==shard);
    if(!expect) {
        assert(res.state==EFS_SESSION_ACTIVE&&progress_fence==N&&progress_clean==N);
        for(unsigned i=0;i<N;i++) {
            unsigned want=probe_fail||shard_of(i)%5==0;
            assert(cleaned[shard_of(i)][0]==want&&cleaned[shard_of(i)][1]==want);
        }
        assert(probes>=N/64&&probes<=2*(EFS_SESSION_BITS/64)); /* one per word and group */
        unsigned owed=0;for(unsigned i=0;i<N;i++)owed+=probe_fail||shard_of(i)%5==0;
        assert(res.cleaned==owed&&res.probe_failed==(probe_fail?probes:0));
    }
    if(parallel<=1)assert(!foreign&&maxflight==1);
    else assert(maxflight>1&&maxflight<=parallel);
    assert(deadline==0);
    return (b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9;
}
int main(void)
{
    run(16,EFS_OK,NULL,0);
    run(0,EFS_OK,NULL,0);
    fail_fence=1;run(16,EFS_ERR_IO,"participant-floor",0);assert(!finished);
    run(0,EFS_ERR_IO,"participant-floor",0);assert(!finished);fail_fence=0;
    probe_fail=1;run(16,EFS_OK,NULL,0);run(0,EFS_OK,NULL,0);
    fail_clean=1;run(16,EFS_ERR_NET,"cleanup",shard_of(N/3));
    run(0,EFS_ERR_NET,"cleanup",shard_of(N/3));fail_clean=0;probe_fail=0;
    /* One commit latency per step: 100 us x 4 steps x 600 shards. */
    step_us=100;
    probe_fail=1;double serial=run(1,EFS_OK,NULL,0),parallel=run(16,EFS_OK,NULL,0);probe_fail=0;
    printf("serial %.2f s, parallel(16) %.2f s\n",serial,parallel);
    assert(parallel*4<serial);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-revoke-parallel-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    exe = p.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'include'), str(p), str(root / 'src/common/session_revoke.c'),
                   '-pthread', '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=120).stdout
print(out.strip())
print('session revoke: parallel per-shard stages keep FENCE_LOC<ACK<FINISH<cleanup, '
      'stop on the first failure and report progress PASS')
