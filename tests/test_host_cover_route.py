#!/usr/bin/env python3
"""Execute production leader routing against committed group membership."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
r=s.index('static uint32_t host_route_voters(');a=s.index('static int host_leader_cover_pick(');b=s.index('\n}',a)+2
code=r"""
#include "efs/raft.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#define HOST_NGROUPS 2
struct host_group {struct efs_raft *r;uint32_t voters;};
struct efs_raft_host {pthread_mutex_t mu;int n,raft_id;struct host_group g[2];};
struct host_view {int has,leader;uint32_t voters;};
static int leaders[2];static uint32_t masks[2];
static struct host_group *group_slot(struct efs_raft_host*h,uint8_t g){return g==0||g==2?&h->g[g/2]:NULL;}
static uint32_t group_voters(uint8_t g,int n){return n<=3?(1u<<n)-1:g==0?7:14;}
static int hosts_group(int id,uint32_t voters){return id>=0 && id<32 && (voters&(1u<<id));}
enum{WP_COVER};static int write_prof_on(void){return 0;}static uint64_t wprof_now(void){return 0;}static void wprof_add(int p,uint64_t us){(void)p;(void)us;}
/* The published routing snapshot: a local replica's leader and committed
 * voters (has=0 without one). */
static void host_view_get(struct efs_raft_host*h,uint8_t group,struct host_view*v){
 struct host_group*s=group_slot(h,group);v->has=s&&s->r;
 v->leader=v->has?leaders[(uintptr_t)s->r-1]:-1;v->voters=v->has?masks[(uintptr_t)s->r-1]:0;}
/* Routing must never take the host mutex. */
#define pthread_mutex_lock(m) (abort(),0)
#define pthread_mutex_unlock(m) (abort(),0)
"""+s[r:a]+s[a:b]+r"""
int main(void){struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.n=4,.raft_id=2};
 h.g[0].r=(struct efs_raft*)1;h.g[1].r=(struct efs_raft*)2;masks[0]=7;masks[1]=14;
 uint8_t both[]={0,2};leaders[0]=1;leaders[1]=1;
 assert(host_leader_cover_pick(&h,0,both,2)==1);
 leaders[0]=0;assert(host_leader_cover_pick(&h,0,both,2)==-1);
 leaders[1]=3;assert(host_leader_cover_pick(&h,2,both,2)==-1);
 /* A one-group retained result probe may route to that group's leader. */
 assert(host_leader_cover_pick(&h,2,both+1,1)==3);
 leaders[0]=2;assert(host_leader_cover_pick(&h,0,both,2)==-1);
 leaders[0]=-1;assert(host_leader_cover_pick(&h,0,both,2)==-1);
 leaders[0]=4;assert(host_leader_cover_pick(&h,0,both,2)==-1);
 h.n=3;masks[0]=masks[1]=7;leaders[0]=1;leaders[1]=2;
 assert(host_leader_cover_pick(&h,0,both,2)==-1); /* full membership, split leaders */
 leaders[1]=1;
 assert(host_leader_cover_pick(&h,0,both,2)==1);
 masks[1]=5;assert(host_leader_cover_pick(&h,0,both,2)==-1);
 h.g[1].r=NULL;h.g[1].voters=5;assert(host_leader_cover_pick(&h,0,both,2)==-1);
 h.g[1].voters=7;assert(host_leader_cover_pick(&h,0,both,2)==-1); /* missing leadership proof */
 assert(host_leader_cover_pick(&h,0,NULL,2)==-1 && host_leader_cover_pick(&h,0,both,0)==-1);
 puts("leader routing: full committed coverage, three/four-node layouts, membership changes, missing/self leaders and single-group loop refusal PASS");
}
"""
with tempfile.TemporaryDirectory(prefix='efs-cover-route-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
# Both callers return the hint before allocation/proof, retaining exact body.
a=s.index('void server_raft_host_append(');b=s.index('\n}',a)
assert s[a:b].index('host_leader_cover_pick')<s[a:b].index('pthread_mutex_trylock(lock)')
a=s.index('void server_raft_host_create_retained(');b=s.index('\n}',a)
assert s[a:b].index('host_leader_cover_pick')<s[a:b].index('host_retained_create_proof')

a=s.index('void server_raft_host_report(');b=s.index('/* Register first-touch participants',a)
assert s[a:b].index('host_leader_cover_pick')<s[a:b].index('rc = host_read_index')
assert 'if(session && count==1)' in s[a:b] and 'host_need_both(both)' in s[a:b]
# REPORT calls the helper before authority without holding the host mutex
# (live fsync regression); the helper itself no longer takes it.
assert 'pthread_mutex_lock(&h->mu)' not in s[a:b]
