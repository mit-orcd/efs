#!/usr/bin/env python3
"""Execute actual host_apply plus public admission prefix; adapter stubs only."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
b=s.index('static int host_apply(void');apply=s[b:s.index('\n}',b)+2]
b=s.index('void server_raft_host_submit(');end=s.index('    if(clen && (cmd[0]==EFS_MD_REQ_EXPIRY_CLOCK || cmd[0]==EFS_MD_REQ_EXPIRY_CLOCK_RESET)) {',b)
public=s[b:end]+'}\n'
a=s.index('static int host_repair_write_read(');fresh=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/meta_cmd.h"
#include "efs/metadata.h"
#include "efs/session.h"
#include "efs/meta_apply.h"
#include "efs/repair_job.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#define HOST_APPLY_RC_MASK 7
#define HOST_APPEND_RSV_LEN 99
#define HOST_LOCK_LEN 99
#define HOST_SESS_CREATE_LEN 100
#define EFS_FAULTS 0
struct server {uint32_t id;int repair_experiment;};
struct efs_raft_host {pthread_mutex_t mu;struct server *s;void *kv;int running,snap_batch;uint64_t apply_extra,obs_apply_cnt,obs_apply_cycle_us;};
struct host_group {struct efs_raft_host *host;int import_block;void *r;uint8_t group;uint64_t arc_idx[8],arc_term[8],arc_extra[8];int arc_rc[8];};
struct efs_msg_raft_mkfs_reply {int rc,leader_hint;};
static struct efs_raft_host *g_host;
static int verdict,cursors,scopes,role=EFS_RAFT_LEADER,flip_role;static uint64_t current_term=1;static struct host_group *slot;
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return slot;}
static int raft_obs_on(void){return 0;}
static uint64_t now_us_(void){return 0;}
int efs_raft_role(const struct efs_raft *r){(void)r;return role;}
uint64_t efs_raft_term(const struct efs_raft *r){(void)r;return current_term;}
static void host_mark_incomplete(struct host_group *g){(void)g;}
static void host_clear_incomplete(struct host_group *g){(void)g;}
static int efs_kv_lsm_apply_begin(struct efs_kv *kv){(void)kv;scopes++;return 0;}
static int efs_kv_lsm_apply_end(struct efs_kv *kv,const struct efs_kv_item *cursor){(void)kv;if(cursor)cursors++;return 0;}
static void host_kv_cursor(uint8_t g,uint64_t i,uint8_t *k,uint8_t *v){(void)g;(void)i;memset(k,0,7);memset(v,0,16);}
static int host_apply_dispatch(struct efs_raft_host *h,uint64_t i,uint64_t t,uint8_t g,const uint8_t *c,uint32_t n){(void)h;(void)i;(void)t;(void)g;(void)c;(void)n;return verdict;}
static int owns=1,read_rc,authority_rc,reads,authorities,forwards;
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 2;}
int efs_repair_job_decode(const uint8_t *in,uint32_t n,struct efs_repair_job *j){if(n!=EFS_REPAIR_JOB_BYTES)return EFS_ERR_PROTO;memset(j,0,sizeof(*j));j->ino=9;j->owner=1;j->epoch=1;j->state=in[0];j->flags=in[1];return 0;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return owns;}
static int host_remote_cmd(struct efs_raft_host *h,uint8_t g,const uint8_t *c,uint32_t n,struct efs_msg_raft_mkfs_reply *o,int hint){(void)h;(void)g;(void)c;(void)n;(void)hint;forwards++;o->rc=EFS_ERR_BUSY;return 0;}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){(void)h;(void)g;(void)hint;reads++;if(flip_role)role=0;return read_rc;}
int efs_repair_write_authority(struct efs_kv *kv,const struct efs_repair_job *j){(void)kv;(void)j;authorities++;return authority_rc;}
int efs_repair_seal_authority(struct efs_kv *kv,const struct efs_repair_job *j){(void)kv;(void)j;authorities++;return authority_rc;}
'''+fresh+apply+public+r'''
int main(void){
 struct efs_kv kv={0};struct efs_raft_host h={.kv=&kv,.running=1};struct host_group g={.host=&h};g_host=&h;
 uint8_t cmd[]={EFS_MD_CMD_REPAIR_JOB,1};
 int logical[]={EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_INVAL,EFS_ERR_PROTO};
 for(unsigned j=0;j<4;j++){
  verdict=logical[j];int before=cursors;assert(host_apply(&g,10+j,1,cmd,2)==0);assert(cursors==before+1);assert(g.arc_rc[(10+j)&7]==logical[j]);
  uint8_t other[]={EFS_MD_CMD_CREATE};verdict=0;assert(!host_apply(&g,20+j,1,other,1));assert(cursors==before+2);
 }
 int storage[]={EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned j=0;j<2;j++){verdict=storage[j];int before=cursors;assert(host_apply(&g,30+j,1,cmd,2)==storage[j]);assert(cursors==before);assert(g.arc_rc[(30+j)&7]==storage[j]);}
 struct efs_msg_raft_mkfs_reply reply;uint8_t raw[]={0,EFS_MD_CMD_REPAIR_JOB,3};
 server_raft_host_submit(raw,sizeof(raw),&reply);assert(reply.rc==EFS_ERR_ACCES);
 assert(scopes==10);
 struct server node={1,1};h.s=&node;pthread_mutex_init(&h.mu,NULL);slot=&g;g.r=&g;
 uint8_t query[2+EFS_REPAIR_JOB_BYTES]={2,EFS_MD_REQ_REPAIR_WRITE_AUTHORITY,EFS_REPAIR_ADMITTED};
 node.repair_experiment=0;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_ACCES&&!reads&&!authorities);node.repair_experiment=1;
 read_rc=EFS_ERR_IO;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_IO&&reads==1&&!authorities);
 read_rc=0;authority_rc=EFS_ERR_STALE;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_STALE&&reads==2&&authorities==1);
 authority_rc=0;server_raft_host_submit(query,sizeof(query),&reply);assert(!reply.rc&&reads==3&&authorities==2);
 owns=0;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_BUSY&&forwards==1&&reads==3&&authorities==2);owns=1;
 query[0]=0;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_INVAL&&reads==3);
 query[0]=2;query[2]=EFS_REPAIR_SEALED;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_INVAL&&reads==3);
 query[2]=EFS_REPAIR_ADMITTED;query[3]=EFS_REPAIR_DATA_DURABLE;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_INVAL&&reads==3);
 query[3]=0;current_term=2;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_STALE&&authorities==2);current_term=1;
 flip_role=1;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_NOT_PRIMARY&&authorities==2);flip_role=0;
 int before_reads=reads;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_BUSY&&reads==before_reads&&forwards==2);
 role=EFS_RAFT_LEADER;node.id=2;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_STALE&&authorities==2);
 query[1]=EFS_MD_REQ_REPAIR_SEAL_AUTHORITY;current_term=3;authority_rc=EFS_ERR_BUSY;
 int old_authorities=authorities;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_BUSY&&authorities==old_authorities+1);
 authority_rc=0;server_raft_host_submit(query,sizeof(query),&reply);assert(!reply.rc&&authorities==old_authorities+2);
 flip_role=1;server_raft_host_submit(query,sizeof(query),&reply);assert(reply.rc==EFS_ERR_NOT_PRIMARY&&authorities==old_authorities+2);
 assert(cursors==8);return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'host.c';p.write_text(code);b=Path(d)/'host'
 subprocess.run(['cc','-std=gnu11','-Wno-unused-variable','-I'+str(root/'include'),str(p),'-o',str(b)],check=True)
 subprocess.run([b],check=True)
print('actual host repair verdict progression, IO retry cursor, public refusal PASS')
