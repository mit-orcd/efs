#!/usr/bin/env python3
"""Lock-free ReadIndex admission: ri_begun precedes probes, pump starts requests."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static void host_read_note_begin(');b=s.index('static int host_read_index_admitted(',a)
code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -11
#define EFS_ERR_NOT_PRIMARY -20
#define EFS_RAFT_LEADER 2
#define HOST_NGROUPS 2
struct efs_raft {uint64_t round,term;int role,pending,quorum,fail;};
struct host_group {uint64_t ri_begun,ri_request;struct efs_raft *r;int hosted;uint8_t group;};
struct server {int unused;};
struct efs_raft_host {pthread_mutex_t mu;struct server *s;struct host_group g[HOST_NGROUPS];};
struct host_view {int has,role;uint64_t commit,term,read_round;};
struct host_read_admission {uint64_t want,round,term;};
static struct host_group *group_slot(struct efs_raft_host*h,uint8_t g){return g==0?&h->g[0]:g==2?&h->g[1]:0;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static int efs_raft_read_pending(struct efs_raft*r){return r->pending;}
static int efs_raft_read_quorum(struct efs_raft*r){return r->quorum;}
static struct efs_raft_host *host;
static struct host_read_admission during;static int sends;
static int host_read_admit_view(struct efs_raft_host*,uint8_t,const struct host_view*,struct host_read_admission*);
/* The probe send: a reader admitted while it is on the wire must require
 * a later round than the one being sent. */
static int efs_raft_read_begin(struct efs_raft*r){
 if(r->fail)return r->fail;
 r->round++;r->pending=1;r->quorum=0;
 struct host_view v={1,EFS_RAFT_LEADER,40,r->term,0};
 assert(!host_read_admit_view(host,r==host->g[0].r?0:2,&v,&during));
 assert(during.round>r->round);sends++;
 return 0;
}
static int kicks,published;
static void host_pump_kick(struct efs_raft_host*h){(void)h;kicks++;}
static void host_publish_view(struct efs_raft_host*h,int i){(void)h;(void)i;published++;}
'''+s[a:b]+r'''
int main(void){
 struct server server={0};struct efs_raft core={.round=7,.term=11,.role=EFS_RAFT_LEADER};
 struct efs_raft other={.round=3,.term=11,.role=EFS_RAFT_LEADER};
 struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.s=&server,.g={{.r=&core,.hosted=1,.group=0},{.r=&other,.hosted=1,.group=2}}};
 host=&h;
 struct host_view v={1,EFS_RAFT_LEADER,40,11,0};struct host_read_admission ad;
 assert(!host_read_begin_tracked(&h,0,&core) && core.round==8 && h.g[0].ri_begun==8);
 assert(during.round==9 && during.want==40 && during.term==11);
 assert(!host_read_admit_view(&h,0,&v,&ad) && ad.round==9 && ad.term==11 && ad.want==40);
 /* Failed begin sends nothing; later admissions may only require more. */
 core.fail=EFS_ERR_BUSY;assert(host_read_begin_tracked(&h,0,&core)==EFS_ERR_BUSY);
 assert(h.g[0].ri_begun==9 && core.round==8 && sends==1);core.fail=0;
 assert(!host_read_admit_view(&h,0,&v,&ad) && ad.round==10);
 /* recv_persisting reports round 0 and refuses: ri_begun never lowers. */
 core.round=0;core.fail=EFS_ERR_BUSY;
 assert(host_read_begin_tracked(&h,0,&core)==EFS_ERR_BUSY && h.g[0].ri_begun==9);
 core.round=8;core.fail=0;
 /* A published round the tracker missed still cannot cover a reader. */
 v.read_round=12;assert(!host_read_admit_view(&h,0,&v,&ad) && ad.round==13);v.read_round=0;
 /* Refusals: not the leader, no replica, unknown group, saturation. */
 v.role=1;assert(host_read_admit_view(&h,0,&v,&ad)==EFS_ERR_NOT_PRIMARY);v.role=EFS_RAFT_LEADER;
 v.has=0;assert(host_read_admit_view(&h,0,&v,&ad)==EFS_ERR_NOT_PRIMARY);v.has=1;
 assert(host_read_admit_view(&h,1,&v,&ad)==EFS_ERR_NOT_PRIMARY);
 h.g[1].ri_begun=UINT64_MAX-1;assert(host_read_admit_view(&h,2,&v,&ad)==EFS_ERR_BUSY);h.g[1].ri_begun=0;
 /* Requests are monotone; one pump kick per raise, shared by later readers. */
 kicks=0;sends=0;host_read_request(&h,0,10);host_read_request(&h,0,10);host_read_request(&h,0,9);
 assert(h.g[0].ri_request==10 && kicks==1 && !sends); /* readers never begin */
 /* Round 8 in flight without quorum: nothing starts (one round in flight). */
 core.pending=1;core.quorum=0;sends=0;published=0;
 assert(!host_read_start_requested(&h) && sends==0);
 /* Its quorum arrives: one round starts for every queued reader. */
 core.quorum=1;assert(host_read_start_requested(&h)==1u && sends==1 && core.round==9 && published==1);
 assert(!host_read_start_requested(&h) && sends==1);
 /* Request 10 > round 9: the next quorum starts round 10, then idle. */
 core.quorum=1;assert(host_read_start_requested(&h)==1u && core.round==10 && sends==2);
 core.quorum=1;assert(!host_read_start_requested(&h) && sends==2);
 /* Followers, unhosted groups and !h->s never start requested rounds. */
 other.role=1;host_read_request(&h,2,5);assert(other.round==3 && sends==2);
 assert(!host_read_start_requested(&h));
 other.role=EFS_RAFT_LEADER;h.g[1].hosted=0;assert(!host_read_start_requested(&h));
 h.g[1].hosted=1;h.s=0;assert(!host_read_start_requested(&h));h.s=&server;
 assert(host_read_start_requested(&h)==2u && other.round==4 && h.g[1].ri_begun==4 && sends==3);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-tickets-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
 # Negative: raising ri_begun after the probe send lets a reader that
 # arrived during the send be covered by it.
 old='    host_read_note_begin(h,group,r);\n    return efs_raft_read_begin(r);'
 assert old in code
 late=Path(d)/'late.c'
 late.write_text(code.replace(old,'    int rc=efs_raft_read_begin(r);\n    host_read_note_begin(h,group,r);\n    return rc;'))
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(late),'-o',str(late.with_suffix(''))],check=True)
 assert subprocess.run([str(late.with_suffix(''))],capture_output=True,timeout=5).returncode!=0, \
  'late ri_begun negative unexpectedly passed'
print('Read admission: ri_begun before probes, arrival during send needs the next round, failure/persisting/saturation, monotone requests, pump-started shared rounds, late-store negative PASS')
