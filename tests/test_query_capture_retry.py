#!/usr/bin/env python3
"""Execute production cut capture: bounded recapture never reuses a failed pin."""
from pathlib import Path
import os, shlex, subprocess, tempfile, sys
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_query_source_once(')
b=s.index('/* Disk builders stream bounded scratch',a)
body=s[a:b]
refresh=(root/'src/server/query_refresh.h').read_text()
if '--baseline-cut-backoff' in sys.argv:
    refresh=refresh.replace('if(cut_retry && failed &&','if(0 && cut_retry && failed &&',1)
if '--unsafe-all-busy-retry' in sys.argv:
    refresh=refresh.replace('rc==EFS_ERR_BUSY && transaction_changed && capture_ms<EFS_QUERY_CUT_RETRY_MS','rc==EFS_ERR_BUSY || (transaction_changed && capture_ms==UINT64_MAX)',1)
if '--unsafe-no-transaction-fence' in sys.argv:
    assert 'transaction_before!=transaction_after' in body
    body=body.replace('transaction_before!=transaction_after','0',1)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HOST_NGROUPS 2
#define HOST_QUERY_MEMTABLE 1
struct efs_kv_lsm_snapshot {unsigned serial;int side_a,side_b,ordinary;};
struct efs_server_query_view {uint64_t boot_id,source_ns,applied[2],terms[2],pin_ms;};
struct efs_raft_host {pthread_mutex_t mu,snap_mu;void *kv;uint64_t boot_id,query_deadline_ms;
 struct {void *r;uint8_t group;uint64_t import_gen;} g[2];};
static struct efs_kv_lsm_snapshot pins[4];
static unsigned captures,releases,barriers,collisions;
static int capable=1,barrier_rc,revision_rc,import_change,pin_rc,expire;
static uint64_t clock_ms=100;
static int model,live_a=1,live_b,ordinary;
static uint64_t quiet_start=500,quiet_end=700;
static unsigned cohort_fail_after;
static uint64_t query_mono_ms(void){return clock_ms;}
static uint64_t now_ns(void){return 999;}
static int host_query_capable(struct efs_raft_host *h){(void)h;return capable;}
static uint64_t efs_raft_applied(void *r){(void)r;return 11;}
static uint64_t efs_raft_term(void *r){(void)r;return 7;}
static int efs_kv_lsm_snapshot_pin(void *kv,int budget,struct efs_kv_lsm_snapshot **out)
{(void)kv;assert(budget==1 && captures<4);pins[captures]=(struct efs_kv_lsm_snapshot){.serial=captures+1,.side_a=live_a,.side_b=live_b,.ordinary=ordinary};*out=&pins[captures++];if(pin_rc)*out=NULL;return pin_rc;}
static int efs_kv_lsm_snapshot_revisions(struct efs_kv_lsm_snapshot *p,uint64_t *r,unsigned n)
{assert(p && n==4096);memset(r,0,n*sizeof(*r));r[0]=p->serial;return 0;}
static int efs_kv_lsm_snapshot_transaction_revision(struct efs_kv_lsm_snapshot *p,uint64_t *out)
{*out=p->serial;return 0;}
static int efs_kv_lsm_snapshot_transaction_activity(struct efs_kv_lsm_snapshot *p,uint64_t out[8])
{(void)p;memset(out,0,8*sizeof(*out));return 0;}
static int efs_kv_lsm_transaction_activity(void *kv,uint64_t out[8])
{(void)kv;memset(out,0,8*sizeof(*out));return 0;}
static int host_read_index_pair(struct efs_raft_host *h,uint8_t first,uint8_t second,int *hint)
{(void)hint;assert(first==0 && second==2);barriers++;if(model>=3)clock_ms+=10;
 if(cohort_fail_after && captures>=cohort_fail_after)return EFS_ERR_BUSY;
 if(import_change)h->g[1].import_gen++;
 if(model==1)live_b=1; /* missing participant applies after pin */
 if(model==2)ordinary++; /* atomic entry changes no transaction revision */
 if(expire)clock_ms=h->query_deadline_ms;
 return barrier_rc;}
static int efs_kv_lsm_transaction_revision(void *kv,uint64_t *out)
{(void)kv;*out=captures+(model==1 ? pins[captures-1].side_b!=live_b : model>=3 ? !(clock_ms>=quiet_start && clock_ms<=quiet_end) : captures<=collisions);return revision_rc;}
static void efs_kv_lsm_snapshot_release(struct efs_kv_lsm_snapshot *p)
{assert(p && p->serial==captures && releases+1==captures);releases++;}
'''+refresh+body+r'''
static struct efs_raft_host host;
static void scheduler_oracle(void) {
 uint64_t last=0,now=0,admitted=0;int failed=0,fast=0,success=0;unsigned work=0,proofs=0;
 model=3;
 while(now<1500) {
  now+=100;
  if(!efs_query_refresh_due_reason(now,1,last,256,0,failed,fast,256,2000))continue;
  captures=releases=barriers=0;clock_ms=now;host.query_deadline_ms=now+200;
  struct efs_kv_lsm_snapshot *p=NULL;struct efs_server_query_view v;
  uint64_t rev[4096],imp[2];int changed=0;
  int rc=host_query_source(&host,&p,&v,rev,imp,&changed,NULL);
  work+=captures;proofs+=2*barriers;assert(captures<=4 && clock_ms<=host.query_deadline_ms);
  fast=efs_query_refresh_cut_retry(rc,changed,clock_ms-now);
  failed=efs_query_refresh_failures(failed,0,rc);
  if(rc) {assert(!p && releases==captures);last=clock_ms;}
  else {assert(p && releases+1==captures);efs_kv_lsm_snapshot_release(p);success=1;admitted=clock_ms;last=now;break;}
  now=clock_ms;
 }
 assert(success && admitted>=quiet_start && admitted<=quiet_end);
 assert(work==9 && proofs==18); /* two rejected four-cut cohorts then fresh cut */
 /* Success coalesces for the existing 2s interval. */
 assert(!efs_query_refresh_due_reason(last+1999,1,last,10000,0,0,0,256,2000));
 assert(efs_query_refresh_due_reason(last+2000,1,last,10000,0,0,0,256,2000));
 printf("quiet cut captured at %llu ms with %u pins/%u group proofs PASS\n",(unsigned long long)admitted,work,proofs);
 /* No quiet interval: bounded admission/pin ownership, never publication. */
 quiet_start=UINT64_MAX;quiet_end=UINT64_MAX;last=0;now=0;failed=fast=0;work=proofs=0;unsigned cohorts=0;
 while(now<1000) {
  now+=100;
  if(!efs_query_refresh_due_reason(now,1,last,256,0,failed,fast,256,2000))continue;
  captures=releases=barriers=0;clock_ms=now;host.query_deadline_ms=now+200;
  struct efs_kv_lsm_snapshot *p=NULL;struct efs_server_query_view v;uint64_t rev[4096],imp[2];int changed=0;
  int rc=host_query_source(&host,&p,&v,rev,imp,&changed,NULL);
  assert(rc==EFS_ERR_BUSY && changed && !p && captures==4 && releases==4);
  work+=captures;proofs+=2*barriers;cohorts++;
  fast=efs_query_refresh_cut_retry(rc,changed,clock_ms-now);failed=1;last=clock_ms;now=clock_ms;
 }
 assert(cohorts==5 && work==20 && proofs==40 && now==1100);
 /* A prior collision followed by authority failure is not cut-only. */
 captures=releases=barriers=0;clock_ms=100;host.query_deadline_ms=300;cohort_fail_after=2;
 struct efs_kv_lsm_snapshot *p=NULL;struct efs_server_query_view v;uint64_t rev[4096],imp[2];int changed=1;
 assert(host_query_source(&host,&p,&v,rev,imp,&changed,NULL)==EFS_ERR_BUSY);
 assert(!changed && !p && captures==2 && releases==2 && !efs_query_refresh_cut_retry(EFS_ERR_BUSY,changed,20));
 cohort_fail_after=0;model=0;
 assert(!efs_query_refresh_cut_retry(EFS_ERR_BUSY,0,10));
 assert(!efs_query_refresh_cut_retry(EFS_ERR_IO,1,10));
 assert(!efs_query_refresh_cut_retry(EFS_ERR_BUSY,1,200));
 assert(efs_query_refresh_cut_retry(EFS_ERR_BUSY,1,199));
 for(int failure=1;failure<=5;failure++) {
  uint64_t interval=efs_query_refresh_retry(failure,2000);
  assert(!efs_query_refresh_due_reason(100+interval-1,1,100,10000,0,failure,0,256,2000));
  assert(efs_query_refresh_due_reason(100+interval,1,100,10000,0,failure,0,256,2000));
 }
 assert(!efs_query_refresh_due_reason(1000,0,100,0,0,1,1,256,2000));
 puts("persistent cut contention: 5 cohorts/20 pins/40 group proofs over 1100 modeled ms, no publication; later authority refusal, general/slow/error backoff and successful coalescing PASS");
}

static int run(void){captures=releases=barriers=0;clock_ms=100;host.query_deadline_ms=200;
 struct efs_kv_lsm_snapshot *p=(void*)1;struct efs_server_query_view view;
 uint64_t revisions[4096],imports[2];int final_changed=-1;
 struct efs_query_capture_work work={0};
 int rc=host_query_source(&host,&p,&view,revisions,imports,&final_changed,&work);
 assert(work.pins==captures && work.pins_ok==(pin_rc?0:captures) && work.pairs==barriers);
 assert(final_changed==(rc==EFS_ERR_BUSY && captures<=collisions && !import_change && !barrier_rc));
 if(rc)assert(!p);else {assert(p->serial==captures && releases+1==captures);
 assert(view.boot_id==42 && view.source_ns==999 && revisions[0]==captures);
 assert(imports[1]==host.g[1].import_gen);efs_kv_lsm_snapshot_release(p);}
 assert(captures==releases || pin_rc);return rc;}
int main(void){pthread_mutex_init(&host.mu,NULL);pthread_mutex_init(&host.snap_mu,NULL);
 host.boot_id=42;host.g[0].group=0;host.g[1].group=2;
 /* A post-admission transaction can resolve A before B is locally applied.
  * The pinned image has A only; fresh proof delivers B and advances revision.
  * Accepting that old pin would publish a mixed distributed transaction. */
 model=1;live_b=0;captures=releases=barriers=0;host.query_deadline_ms=200;
 struct efs_kv_lsm_snapshot *mixed=NULL;struct efs_server_query_view view;
 uint64_t revisions[4096],imports[2];int changed=0;
 assert(host_query_source_once(&host,&mixed,&view,revisions,imports,&changed,NULL)==EFS_ERR_BUSY);
 assert(changed && !mixed && captures==1 && releases==1 && live_b==1);
 assert(pins[0].side_a==1 && pins[0].side_b==0); /* pin itself remains immutable */
 live_b=0;assert(!run() && captures==2 && releases==2 && pins[1].side_b==1);
 model=2;ordinary=0;assert(!run() && captures==1 && ordinary==1 && pins[0].ordinary==0);
 model=0;
 puts("mixed distributed cut: reject A-only image after B proof, stable complete recapture and ordinary atomic update noninvalidation PASS");
 scheduler_oracle();

 assert(!run() && captures==1 && barriers==1);
 collisions=2;assert(!run() && captures==3 && barriers==3);
 collisions=9;assert(run()==EFS_ERR_BUSY && captures==4 && barriers==4);
 expire=1;assert(run()==EFS_ERR_BUSY && captures==1);expire=0;collisions=0;
 import_change=1;assert(run()==EFS_ERR_BUSY && captures==1);import_change=0;
 barrier_rc=EFS_ERR_BUSY;assert(run()==EFS_ERR_BUSY && captures==1);barrier_rc=0;
 revision_rc=EFS_ERR_IO;assert(run()==EFS_ERR_IO && captures==1);revision_rc=0;
 capable=0;assert(run()==EFS_ERR_NOT_PRIMARY && !captures && !barriers);capable=1;
 pin_rc=EFS_ERR_NOMEM;assert(run()==EFS_ERR_NOMEM && captures==1 && !barriers);pin_rc=0;
 puts("query capture: fresh pin/barriers on each collision, four-cut/deadline bound, import/authority/error refusal and balanced releases PASS");}
'''
code='#include "'+str(root/'src/common/query_stage_trace.h')+'"\n'+code
with tempfile.TemporaryDirectory(prefix='efs-query-capture-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-pthread','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
