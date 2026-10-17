#!/usr/bin/env python3
"""Actual query redirect prefers a proven dual leader; fresh proofs stay intact."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
def fn(name):
    starts=['static int '+name+'(', 'int '+name+'(']
    a=next(s.index(x) for x in starts if x in s)
    return s[a:s.index('\n}',a)+2]
code=r"""
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#define HOST_NGROUPS 2
#define EFS_OK 0
#define EFS_ERR_INVAL -1
#define EFS_ERR_NOT_FOUND -3
#define EFS_ERR_IO -6
#define EFS_ERR_BUSY -13
#define EFS_ERR_NOT_PRIMARY -15
struct efs_raft {int leader;uint32_t voters;};
struct host_group {struct efs_raft *r;uint32_t voters;};
struct efs_server_query_view {void *catalog;};
struct efs_raft_host {pthread_mutex_t mu;pthread_rwlock_t query_lock;
 int n,raft_id,query_enabled,query_error,capable;struct host_group groups[2];struct efs_server_query_view query_view;};
static struct efs_raft_host *g_host;
static int barriers,consumed,barrier_rc,lose_capability;
static struct host_group *group_slot(struct efs_raft_host*h,uint8_t g){assert(g==0||g==2);return &h->groups[g>>1];}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){return group_slot(h,g)->r;}
struct host_view {int has,leader;uint32_t voters;};
/* Published routing snapshot of a local replica. */
static void host_view_get(struct efs_raft_host*h,uint8_t g,struct host_view*v){struct efs_raft*r=group_raft(h,g);v->has=r!=NULL;v->leader=r?r->leader:-1;v->voters=r?r->voters:0;}
static uint32_t efs_raft_voters(struct efs_raft*r){return r->voters;}
static uint32_t group_voters(uint8_t g,int n){(void)g;(void)n;return 0;}
static int hosts_group(int id,uint32_t voters){return id>=0&&id<32&&(voters&(1u<<id));}
enum{WP_HOLD,WP_MU,WP_APPEND,WP_SPIN,WP_PERSIST,WP_MU_AFTER,WP_PROPOSE,WP_COMMIT_APPLY,WP_WAKE,WP_VERDICT,WP_APPLY,WP_COVER,WP_REPORT_MU};__attribute__((unused)) static inline int write_prof_on(void){return 0;}__attribute__((unused)) static inline uint64_t wprof_now(void){return 0;}__attribute__((unused)) static inline void wprof_add(int p,uint64_t us){(void)p;(void)us;}__attribute__((unused)) static inline void wprof_tick(void){}
static int host_fwd_leader_pick(struct efs_raft_host*h,uint8_t g,int skip){(void)h;(void)g;(void)skip;return -1;}
static void host_need_both(uint8_t *g){g[0]=0;g[1]=2;}
static int host_query_capable(struct efs_raft_host*h){return h->capable;}
static int host_read_index_pair(struct efs_raft_host*h,uint8_t a,uint8_t b,int *hint)
{assert(a==0&&b==2);(void)hint;barriers++;if(lose_capability)h->capable=0;return barrier_rc;}
static int consume(void *ctx,const struct efs_server_query_view*v,int error)
{assert(ctx==g_host&&v==&g_host->query_view&&!error);consumed++;return 0;}
"""+fn('host_pick_peer')+s[s.index('static uint32_t host_route_voters('):s.index('\n}',s.index('static uint32_t host_route_voters('))+2]+fn('host_leader_cover_pick')+fn('server_raft_host_query_read')+r"""
int main(void){struct efs_raft r[2]={{2,7},{3,14}};
 struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.query_lock=PTHREAD_RWLOCK_INITIALIZER,.n=4,.raft_id=0,.query_enabled=1,.groups={{&r[0],0},{&r[1],0}},.query_view={(void*)1}};
 g_host=&h;uint16_t target=0;uint8_t need[2]={0,2};
 assert(host_leader_cover_pick(&h,0,need,2)==2);
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_PRIMARY&&target==3&&!barriers&&!consumed);
 r[0].leader=0;r[1].leader=2;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_PRIMARY&&target==3);
 r[1].leader=3; /* Both known leaders are single-group; keep dual fallback. */
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_PRIMARY&&target==2);
 r[0].leader=r[1].leader=-1;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_PRIMARY&&target==2);
 r[0].leader=9;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_PRIMARY&&target==2);
 r[0].leader=2;r[0].voters=3; /* Removed/nonvoting leader cannot be preferred. */
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_PRIMARY&&target==2);
 r[0].voters=7;h.capable=1;h.raft_id=1;
 assert(!server_raft_host_query_read(consume,&h,&target)&&!target&&barriers==1&&consumed==1);
 barrier_rc=EFS_ERR_BUSY;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_BUSY&&consumed==1);
 barrier_rc=EFS_ERR_IO;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_IO&&consumed==1);
 barrier_rc=0;lose_capability=1;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_BUSY&&consumed==1);
 h.query_enabled=0;
 assert(server_raft_host_query_read(consume,&h,&target)==EFS_ERR_NOT_FOUND&&consumed==1);
 pthread_rwlock_destroy(&h.query_lock);pthread_mutex_destroy(&h.mu);
 puts("query target: actual dual leader preference, single/unknown/removed fallback, capable local serve and fresh refusal PASS");
}
"""
with tempfile.TemporaryDirectory(prefix='efs-query-route-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
