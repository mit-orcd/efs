#!/usr/bin/env python3
"""Actual admitted reader returns its exact proof floor, not later apply."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def fn(name):
 a=s.index('static int '+name+'(');return s[a:s.index('\n}',a)+2]
a=s.index('struct host_view {');view=s[a:s.index('\n};',a)+3]
code=r'''
#include "efs/raft.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#define HOST_READ_TRIES 1
#define HOST_TICK_US 5000
struct efs_raft_host {pthread_mutex_t mu,cv_mu;int raft_id,n;};
struct host_read_admission {uint64_t want,round,term;};
struct read_prof_acc {int unused;};
static struct {struct read_prof_acc ri_remote,ri_remote_wait,ri_leader_covered,ri_leader;} g_read_prof;
'''+view+r'''
static struct host_view current;
static int remote_rc,apply_rc,admissions;
static int read_prof_on(void){return 0;}
static uint64_t now_us_(void){return 0;}
static void read_prof_add(struct read_prof_acc*a,uint64_t n){(void)a;(void)n;}
static void host_deadline_us(struct timespec*t,long us){(void)us;*t=(struct timespec){0};}
static int host_past_deadline(const struct timespec*t){(void)t;return 1;}
static void host_view_get(struct efs_raft_host*h,uint8_t g,struct host_view*v){(void)h;(void)g;*v=current;}
static int host_remote_read_index(struct efs_raft_host*h,uint8_t g,int l,const struct timespec*t,uint64_t*i,int*hint){(void)h;(void)g;(void)l;(void)t;*i=222;*hint=1;return remote_rc;}
static int host_wait_applied(struct efs_raft_host*h,uint8_t g,uint64_t i,int*hint){(void)h;(void)g;(void)hint;assert(i==222);return apply_rc;}
static int host_follower_read_result(int rc,int id,int n,int hint,int*out){(void)id;(void)n;(void)hint;(void)out;return rc;}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){(void)h;(void)g;return NULL;}
static int host_read_admit(struct efs_raft*r,struct host_read_admission*a,int*start){(void)r;admissions++;*a=(struct host_read_admission){101,11,2};*start=1;current.proven_round=11;return EFS_OK;}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){(void)h;(void)g;}
static void host_pump_kick(struct efs_raft_host*h){(void)h;}
int efs_raft_role(const struct efs_raft*r){(void)r;assert(0);return 0;}
uint64_t efs_raft_read_round(const struct efs_raft*r){(void)r;assert(0);return 0;}
int efs_raft_read_pending(const struct efs_raft*r){(void)r;assert(0);return 0;}
int efs_raft_read_quorum(const struct efs_raft*r){(void)r;assert(0);return 0;}
int efs_raft_read_covers(const struct efs_raft*r,uint64_t n){(void)r;(void)n;assert(0);return 0;}
int efs_raft_read_begin(struct efs_raft*r){(void)r;assert(0);return EFS_OK;}
static void host_waiter_sleep(struct efs_raft_host*h,uint8_t g,int any,uint64_t i,uint64_t seen,const struct timespec*t){(void)h;(void)g;(void)any;(void)i;(void)seen;(void)t;assert(0);}
'''+fn('host_view_covers')+fn('host_view_wait_apply')+fn('host_read_index_admitted')+r'''
int main(void){
 struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.cv_mu=PTHREAD_MUTEX_INITIALIZER,.n=4};
 struct host_read_admission a={101,11,2};int hint;uint64_t proof=9999;
 current=(struct host_view){.has=1,.role=EFS_RAFT_LEADER,.leader=0,.term=2,.commit=1000,.applied=900,.proven_done=1,.proven_index=110,.proven_round=11,.read_pending=1};
 assert(host_read_index_admitted(&h,0,&hint,&a,&proof)==EFS_OK&&proof==110); /* concurrent suffix never extends this proof target */
 current.proven_round=10;assert(host_read_index_admitted(&h,0,&hint,&a,&proof)==EFS_ERR_BUSY&&proof==0);
 current.proven_round=11;current.proven_done=0;assert(host_read_index_admitted(&h,0,&hint,&a,&proof)==EFS_ERR_BUSY&&proof==0);
 current.proven_done=1;current.applied=109;assert(host_read_index_admitted(&h,0,&hint,&a,&proof)==EFS_ERR_BUSY&&proof==0);
 current.applied=900;current.term=3;assert(host_read_index_admitted(&h,0,&hint,&a,&proof)==EFS_ERR_NOT_PRIMARY&&proof==0);
 current.term=2;current.proven_round=10;assert(host_read_index_admitted(&h,0,&hint,NULL,&proof)==EFS_OK&&proof==110&&admissions==1);
 current.role=EFS_RAFT_FOLLOWER;current.leader=1;assert(host_read_index_admitted(&h,0,&hint,NULL,&proof)==EFS_OK&&proof==222);
 apply_rc=EFS_ERR_BUSY;assert(host_read_index_admitted(&h,0,&hint,NULL,&proof)==EFS_ERR_BUSY&&proof==0);
 apply_rc=0;remote_rc=EFS_ERR_IO;assert(host_read_index_admitted(&h,0,&hint,NULL,&proof)==EFS_ERR_IO&&proof==0);
 pthread_mutex_destroy(&h.mu);pthread_mutex_destroy(&h.cv_mu);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-result-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('ReadIndex result: exact proof floor, fresh admission, term/quorum/own apply and remote failure refusal PASS')
