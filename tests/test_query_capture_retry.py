#!/usr/bin/env python3
"""Execute production cut capture: bounded recapture never reuses a failed pin."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_query_source_once(')
b=s.index('/* Disk builders stream bounded scratch',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HOST_NGROUPS 2
#define HOST_QUERY_MEMTABLE 1
struct efs_kv_lsm_snapshot {unsigned serial;};
struct efs_server_query_view {uint64_t boot_id,source_ns,applied[2],terms[2],pin_ms;};
struct efs_raft_host {pthread_mutex_t mu,snap_mu;void *kv;uint64_t boot_id,query_deadline_ms;
 struct {void *r;uint8_t group;uint64_t import_gen;} g[2];};
static struct efs_kv_lsm_snapshot pins[4];
static unsigned captures,releases,barriers,collisions;
static int capable=1,barrier_rc,revision_rc,import_change,pin_rc,expire;
static uint64_t clock_ms=100;
static uint64_t query_mono_ms(void){return clock_ms;}
static uint64_t now_ns(void){return 999;}
static int host_query_capable(struct efs_raft_host *h){(void)h;return capable;}
static uint64_t efs_raft_applied(void *r){(void)r;return 11;}
static uint64_t efs_raft_term(void *r){(void)r;return 7;}
static int efs_kv_lsm_snapshot_pin(void *kv,int budget,struct efs_kv_lsm_snapshot **out)
{(void)kv;assert(budget==1 && captures<4);pins[captures].serial=captures+1;*out=&pins[captures++];if(pin_rc)*out=NULL;return pin_rc;}
static int efs_kv_lsm_snapshot_revisions(struct efs_kv_lsm_snapshot *p,uint64_t *r,unsigned n)
{assert(p && n==4096);memset(r,0,n*sizeof(*r));r[0]=p->serial;return 0;}
static int efs_kv_lsm_snapshot_transaction_revision(struct efs_kv_lsm_snapshot *p,uint64_t *out)
{*out=p->serial;return 0;}
static int host_read_index_pair(struct efs_raft_host *h,uint8_t first,uint8_t second,int *hint)
{(void)hint;assert(first==0 && second==2);barriers++;if(import_change)h->g[1].import_gen++;
 if(expire)clock_ms=h->query_deadline_ms;
 return barrier_rc;}
static int efs_kv_lsm_transaction_revision(void *kv,uint64_t *out)
{(void)kv;*out=captures+(captures<=collisions);return revision_rc;}
static void efs_kv_lsm_snapshot_release(struct efs_kv_lsm_snapshot *p)
{assert(p && p->serial==captures && releases+1==captures);releases++;}
'''+s[a:b]+r'''
static struct efs_raft_host host;
static int run(void){captures=releases=barriers=0;clock_ms=100;host.query_deadline_ms=200;
 struct efs_kv_lsm_snapshot *p=(void*)1;struct efs_server_query_view view;
 uint64_t revisions[4096],imports[2];int rc=host_query_source(&host,&p,&view,revisions,imports);
 if(rc)assert(!p);else {assert(p->serial==captures && releases+1==captures);
 assert(view.boot_id==42 && view.source_ns==999 && revisions[0]==captures);
 assert(imports[1]==host.g[1].import_gen);efs_kv_lsm_snapshot_release(p);}
 assert(captures==releases || pin_rc);return rc;}
int main(void){pthread_mutex_init(&host.mu,NULL);pthread_mutex_init(&host.snap_mu,NULL);
 host.boot_id=42;host.g[0].group=0;host.g[1].group=2;
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
