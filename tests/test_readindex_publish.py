#!/usr/bin/env python3
"""Exercise actual coherent host publication; historical proof is not raw quorum."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text();r=(root/'src/raft/raft.c').read_text()
def func(src,name):
 a=src.index(name+'(');a=src.rfind('\n',0,a)+1;return src[a:src.index('\n}',a)+2]
a=s.index('    uint64_t v_seq;');b=s.index('\n};',a)
group=s[a:b]
a=s.index('struct host_view {');b=s.index('\n};',a)+3
view=s[a:b]
pub=s[s.index('static int host_publication_committed('):s.index('\n}',s.index('static int host_publication_committed('))]
condition=pub[pub.index('if (v.read_done'):pub.index(') break;',pub.index('if (v.read_done'))][4:]
code=r'''
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define EFS_RAFT_LEADER 2
struct efs_raft {int recv_persisting,role,storage_failed,quorum,leader,pending;uint64_t recv_stable_commit,commit,applied,current_term,read_round,read_index;
 uint64_t read_epoch,read_proven_round,read_proven_index,read_proven_term;uint32_t read_proven_cfg_old,read_proven_cfg_new,log_old,log_new;
 uint64_t read_applied_round,read_applied_index,read_applied_term;
 uint32_t read_applied_cfg_old,read_applied_cfg_new;};
#define last_applied applied
static uint64_t efs_raft_commit(struct efs_raft*r){return r->recv_persisting?r->recv_stable_commit:r->commit;}
static uint64_t efs_raft_applied(struct efs_raft*r){return r->applied;}
static uint64_t efs_raft_term(struct efs_raft*r){return r->current_term;}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static int efs_raft_leader(struct efs_raft*r){return r->leader;}
static uint32_t efs_raft_voters(struct efs_raft*r){(void)r;return 7;}
static uint64_t efs_raft_read_index(struct efs_raft*r){return r->recv_persisting?0:r->read_index;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->recv_persisting?0:r->read_round;}
static int efs_raft_read_pending(struct efs_raft*r){return !r->recv_persisting&&r->pending;}
static pthread_mutex_t slow_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slow_cv=PTHREAD_COND_INITIALIZER;
static int slow_quorum,entered,released;
static int efs_raft_read_quorum(const struct efs_raft*r){
 pthread_mutex_lock(&slow_mu);
 if(slow_quorum){entered=1;pthread_cond_broadcast(&slow_cv);while(!released)pthread_cond_wait(&slow_cv,&slow_mu);}
 pthread_mutex_unlock(&slow_mu);return !r->recv_persisting&&!r->storage_failed&&r->role==EFS_RAFT_LEADER&&r->quorum;
}
''' +func(r,'efs_raft_read_context')+func(r,'efs_raft_read_proven')+'\nstruct host_group {struct efs_raft*r;\n'+group+r'''
};
struct efs_raft_host {struct host_group g[1];};
static struct host_group *group_slot(struct efs_raft_host*h,uint8_t g){assert(!g);return h->g;}
#define V_LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define V_STORE(p,v) __atomic_store_n(&(p),(v),__ATOMIC_RELEASE)
''' +view+'\n'+func(s,'host_publish_view')+'\n'+func(s,'host_view_get')+'\n'+func(s,'host_view_covers')+'\n'+r'''
static int publication_ready(struct host_view v,uint64_t round,uint64_t want){return '''+condition+r''';}
static struct efs_raft core;
static struct efs_raft_host host;
static int done;
static void set_view(int ready){
 core=(struct efs_raft){.role=2,.leader=0,.quorum=ready,.pending=!ready,
 .read_epoch=7,.current_term=9,.commit=60,.applied=ready?60:50,.read_round=12,.read_index=60,
 .read_proven_round=11,.read_proven_index=50,.read_proven_term=9,
 .read_proven_cfg_old=7,.log_old=7};
 host.g[0].r=&core;host_publish_view(&host,0);
}
static void *publish(void *unused){(void)unused;
 for(int i=0;i<100000;i++)set_view(i&1);
 __atomic_store_n(&done,1,__ATOMIC_RELEASE);return NULL;
}
static void *slow_publish(void *u){(void)u;host_publish_view(&host,0);return NULL;}
int main(void){struct host_view v;set_view(0);host_view_get(&host,0,&v);
 assert(host_view_covers(&v,50,11,9));
 assert(!host_view_covers(&v,60,12,9));
 assert(!publication_ready(v,12,60)); /* exact actual publication predicate */
 set_view(1);host_view_get(&host,0,&v);
 assert(host_view_covers(&v,60,12,9)&&publication_ready(v,12,60));
 assert(!publication_ready(v,13,60));
 /* A receive-side higher-term transition must replace the previously
  * published leader/quorum view before the host handoff exposes it. Core
  * getter fencing is independently exercised by the real-core phase test. */
 core.recv_persisting=1;core.recv_stable_commit=60;core.role=1;core.current_term=10;
 host_publish_view(&host,0);host_view_get(&host,0,&v);
 assert(v.role==1&&v.term==10&&!v.read_context&&!v.read_done&&!v.proven_done);
 assert(!host_view_covers(&v,60,12,9));
 set_view(1);
 core.storage_failed=1;core.quorum=0;host_publish_view(&host,0);host_view_get(&host,0,&v);
 assert(!v.read_context&&!v.proven_done&&!host_view_covers(&v,50,11,9));
 host.g[0].r=NULL;host_publish_view(&host,0);host_view_get(&host,0,&v);
 assert(!v.has&&!v.read_done&&!v.proven_done&&!v.proven_index&&!v.proven_round);
 set_view(1);
 uint64_t seq=host.g[0].v_seq,stamp=host.g[0].v_stamp;
 host_publish_view(&host,0);assert(host.g[0].v_seq==seq && host.g[0].v_stamp==stamp);
 /* A blocked core/storage getter must not leave readers spinning on odd
  * sequence. The old coherent proof cannot cover an arrival after its send. */
 core.current_term=10;core.quorum=0;core.pending=1;core.read_round=13;
 pthread_mutex_lock(&slow_mu);slow_quorum=1;pthread_mutex_unlock(&slow_mu);
 pthread_t slow;assert(!pthread_create(&slow,NULL,slow_publish,NULL));
 pthread_mutex_lock(&slow_mu);while(!entered)pthread_cond_wait(&slow_cv,&slow_mu);pthread_mutex_unlock(&slow_mu);
 assert(!(__atomic_load_n(&host.g[0].v_seq,__ATOMIC_ACQUIRE)&1));
 host_view_get(&host,0,&v);assert(v.term==9 && v.read_round==12);
 assert(!host_view_covers(&v,60,13,9));
 pthread_mutex_lock(&slow_mu);released=1;pthread_cond_broadcast(&slow_cv);pthread_mutex_unlock(&slow_mu);
 assert(!pthread_join(slow,NULL));slow_quorum=0;
 host_view_get(&host,0,&v);assert(v.term==10 && v.read_round==13 && !v.proven_done);
 assert(!host_view_covers(&v,60,12,9));
 set_view(0);pthread_t t;assert(!pthread_create(&t,NULL,publish,NULL));
 unsigned reads=0;do {host_view_get(&host,0,&v);reads++;
 assert(v.has&&v.proven_done&&v.term==9&&v.read_round==12);
 if(v.read_done){assert(!v.read_pending&&v.proven_round==12&&v.proven_index==60&&v.applied==60);assert(publication_ready(v,12,60));}
 else {assert(v.read_pending&&v.proven_round==11&&v.proven_index==50&&v.applied==50);assert(!publication_ready(v,12,60));}
 }while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE));
 assert(!pthread_join(t,NULL));assert(reads);
 puts("actual host publish/get: raw publication quorum, admitted completion, missing replica and coherent concurrent snapshots PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-publish-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=30)
