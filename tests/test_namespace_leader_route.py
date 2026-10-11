#!/usr/bin/env python3
"""Route whole namespace writes only to a proven dual-group parent leader."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_namespace_leader_hint(')
b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#define HOST_NGROUPS 2
struct efs_raft {uint32_t voters;};
struct host_group {struct efs_raft *r;uint32_t voters;};
struct efs_raft_host {pthread_mutex_t mu;int n,raft_id;struct host_group groups[2];};
static int leader=-1;
static int host_local_leader(struct efs_raft_host *h,uint8_t group){(void)h;assert(group==0 || group==2);return leader;}
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){return &h->groups[group>>1];}
static uint32_t efs_raft_voters(struct efs_raft *r){return r->voters;}
static int hosts_group(int id,uint32_t voters){return id>=0 && id<32 && (voters&(1u<<id));}
'''+s[a:b]+r'''
int main(void){
 struct efs_raft r[2]={{7},{7}};
 struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.n=3,.raft_id=0,.groups={{&r[0],0},{&r[1],0}}};
 leader=1;assert(host_namespace_leader_hint(&h,2)==1);
 leader=0;assert(host_namespace_leader_hint(&h,0)==-1);
 leader=-1;assert(host_namespace_leader_hint(&h,0)==-1);
 leader=3;assert(host_namespace_leader_hint(&h,2)==-1);
 h.n=4;r[0].voters=7;r[1].voters=14;
 leader=3;assert(host_namespace_leader_hint(&h,2)==-1); /* group-2-only leader */
 leader=0;h.raft_id=3;assert(host_namespace_leader_hint(&h,0)==-1); /* group-0-only leader */
 leader=1;assert(host_namespace_leader_hint(&h,2)==1);
 leader=2;assert(host_namespace_leader_hint(&h,0)==2);
 h.groups[0].r=NULL;h.groups[0].voters=0;
 assert(host_namespace_leader_hint(&h,2)==-1); /* unknown config */
 h.groups[0].voters=7;assert(host_namespace_leader_hint(&h,2)==2);
 r[1].voters=2;assert(host_namespace_leader_hint(&h,0)==-1); /* removed / learner */
 pthread_mutex_destroy(&h.mu);
 puts("namespace leader routing: dual-group membership, unknown/removed/single-group leaders and self fallback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-route-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
for name in ['mkdir','rmdir']:
 a=s.index('void server_raft_host_'+name+'(efs_ino_t parent,');b=s.index('\n}',a)
 body=s[a:b];route=body.index('host_namespace_leader_hint(')
 assert route<body.index('host_namespace_session_admit(')
 assert route<body.index('host_read_index(')
 assert route<body.index('host_command_cohort_begin(')
 assert 'set_inode_rc(out,EFS_ERR_NOT_PRIMARY,hint);return;' in body
