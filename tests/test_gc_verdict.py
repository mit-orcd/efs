#!/usr/bin/env python3
"""Exercise production GC command decoders, local and forwarded verdicts."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
def function(name):
    m = re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{', text, re.M)
    if not m: raise RuntimeError(name)
    return text[m.start():text.index('\n}', m.end())+2]
append_len=re.search(r'^#define HOST_APPEND_RSV_LEN (\d+)',text,re.M).group(1)
lock_len=re.search(r'^#define HOST_LOCK_LEN (\d+)',text,re.M).group(1)
apply=text[text.index('static int host_apply(void *app'):];apply=apply[apply.index('    ret = (cmd[0]'):apply.index('    /* Finish the atomic apply scope')]
source = '#define HOST_LOCK_LEN '+lock_len+'\n#define HOST_APPEND_RSV_LEN '+append_len+'\n#define HOST_APPEND_RES_LEN 38\n#define APPLY_LOG(...) ((void)0)\n'+r'''
#include "efs/common.h"
#include "efs/meta_apply.h"
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define EFS_RAFT_LEADER 1
struct efs_raft {int leader;};
struct server {uint64_t gc_sweep_errors,gc_reap_errors,gc_ack_errors;int storage_engine,repair_experiment;};
struct efs_raft_host {int gc_running;pthread_mutex_t mu;struct efs_raft r;struct server *s;struct efs_kv *kv;};
static int verdict, waits, forwards, durable;
static int cohort_calls,cohort_rc,settled_calls,settled_rc;
static uint32_t voters=7;static uint64_t expected_index=41,expected_term=7;
static uint32_t efs_raft_voters(struct efs_raft*r){(void)r;return voters;}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){(void)g;return &h->r;}
static int efs_raft_role(struct efs_raft*r){return r->leader;}
static int efs_raft_leader(struct efs_raft*r){(void)r;return 2;}
static int __attribute__((unused)) efs_raft_durable(struct efs_raft*r,uint64_t i){(void)r;assert(i==41);return durable;}
static int host_wait_settled(struct efs_raft_host*h,uint8_t g,uint64_t i,uint64_t t,int*p){(void)h;(void)g;(void)p;assert(i==expected_index&&t==expected_term);settled_calls++;return settled_rc;}
static int host_wait_verdict(struct efs_raft_host*h,uint8_t g,uint64_t i,uint64_t t,int*p){(void)h;(void)g;(void)p;assert(i==expected_index&&t==expected_term);waits++;return verdict;}
static int host_wait_applied(struct efs_raft_host*h,uint8_t g,uint64_t i,int*p){(void)h;(void)g;(void)p;assert(i==41);waits++;return 0;}
static int host_remote_cmd(struct efs_raft_host*h,uint8_t g,const uint8_t*c,uint32_t n,struct efs_msg_raft_mkfs_reply*p,int lid){(void)h;(void)g;(void)c;(void)n;assert(lid==2);forwards++;p->index=41;return verdict;}
static int host_propose_impl(struct efs_raft_host*h,uint8_t g,const uint8_t*c,uint32_t n,uint64_t*i,uint64_t*t,int*hint,int local){
 (void)g;(void)c;(void)n;assert(!hint);
 assert(!pthread_mutex_trylock(&h->mu));pthread_mutex_unlock(&h->mu);
 if(local){cohort_calls++;*i=42;*t=9;return cohort_rc;}
 *i=41;*t=7;return durable;
}
static uint64_t rd64be(const uint8_t*p){uint64_t x=0;for(int i=0;i<8;i++)x=(x<<8)|p[i];return x;}
static uint32_t rd32be(const uint8_t*p){return (uint32_t)rd64be((const uint8_t[]){0,0,0,0,p[0],p[1],p[2],p[3]});}
static void wr32be(uint8_t*p,uint32_t x){for(int i=3;i>=0;i--){p[i]=x;x>>=8;}}
uint32_t efs_kv_lane_shard(efs_ino_t i,uint8_t l){assert(i==42&&l==3);return 9;}
uint8_t efs_raft_shard_group(uint32_t s){assert(s==9);return 0;}
int efs_meta_lane_sweep_select(struct efs_kv*k,efs_ino_t i,uint64_t g,uint8_t l,uint32_t*c,uint32_t*n){(void)k;assert(i==42&&g==7&&l==3);c[0]=3;*n=1;return verdict;}
int efs_meta_apply_lane_sweep(struct efs_kv*k,efs_ino_t i,uint64_t g,uint8_t l){(void)k;assert(i==42&&g==7&&l==3);return verdict;}
int efs_meta_apply_lane_sweep_batch(struct efs_kv*k,efs_ino_t i,uint64_t g,uint8_t l,const uint32_t*c,uint32_t n){(void)k;assert(i==42&&g==7&&l==3&&n==1&&c[0]==3);return verdict;}
int efs_meta_apply_orphan_reclaim(struct efs_kv*k,efs_ino_t i,uint64_t g){(void)k;assert(i==42&&g==7);return verdict;}
int efs_meta_apply_reap_done(struct efs_kv*k,efs_ino_t i,uint64_t g){(void)k;assert(i==42&&g==7);return verdict;}
int efs_meta_apply_reap_batch(struct efs_kv*k,efs_ino_t i,uint64_t g,const struct efs_meta_reap_key*p,uint32_t n){(void)k;assert(i==42&&g==7&&n==1&&p[0].len==27&&p[0].key[0]==9);return verdict;}
int efs_meta_apply_gc_ack(struct efs_kv*k,const struct efs_gc_ack_item*p,uint32_t n){(void)k;assert(n==1&&p->ino==42&&p->gen==7&&p->lane==3&&p->ci==8&&p->frag==2);return verdict;}
int efs_meta_apply_append_resolve(struct efs_kv*k,efs_ino_t i,uint64_t o,int v){(void)k;assert(i==42&&o==7&&v==1);return verdict;}
''' + '\n'.join(function(n) for n in ('apply_append_res_cmd','host_pack_sweep_next','apply_lane_sweep_cmd','apply_reap_done_cmd','apply_orphan_reap_cmd','apply_gc_ack_cmd','host_bg_propose','host_gc_propose')) + '\nstatic int core_result(uint8_t opcode,int rc,uint32_t clen){uint8_t cmd[64]={opcode};if(opcode==EFS_MD_CMD_SESSION)cmd[1]=EFS_MD_SESS_LEASE_LOCK_BATCH;assert(clen<=sizeof(cmd));int ret;\n'+apply+'\nreturn ret;}\n' + r'''
int main(void){
 (void)host_propose_impl;(void)efs_raft_voters;
 assert(core_result(EFS_MD_CMD_SESSION,EFS_ERR_BUSY,3)==EFS_OK);
 assert(core_result(EFS_MD_CMD_SESSION,EFS_ERR_IO,3)==EFS_ERR_IO);
 assert(core_result(EFS_MD_CMD_SESSION,EFS_ERR_NOMEM,3)==EFS_ERR_NOMEM);
 struct server s={0};struct efs_raft_host h={.gc_running=1,.mu=PTHREAD_MUTEX_INITIALIZER,.s=&s,.r={1}};
 uint8_t resolved[38]={EFS_MD_CMD_APPEND_RES};resolved[8]=42;resolved[16]=7;resolved[17]=1;
 int resolve_values[]={EFS_OK,EFS_ERR_BUSY,EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_PROTO};
 for(unsigned i=0;i<sizeof(resolve_values)/sizeof(resolve_values[0]);i++){
  verdict=resolve_values[i];assert(apply_append_res_cmd(&h,resolved,38,41)==verdict);
 }
 verdict=EFS_ERR_NOT_FOUND;assert(apply_append_res_cmd(&h,resolved,38,41)==EFS_OK);
 assert(apply_append_res_cmd(&h,resolved,37,41)==EFS_ERR_PROTO);
 assert(core_result(EFS_MD_CMD_APPEND_RES,EFS_ERR_BUSY,38)==EFS_OK);
 assert(core_result(EFS_MD_CMD_APPEND_RES,EFS_ERR_PROTO,37)==EFS_OK);
 assert(core_result(EFS_MD_CMD_APPEND_RES,EFS_ERR_IO,38)==EFS_ERR_IO);
 assert(core_result(EFS_MD_CMD_APPEND_RES,EFS_ERR_NOMEM,38)==EFS_ERR_NOMEM);
 uint8_t cmd[25]={EFS_MD_CMD_LANE_SWEEP};cmd[8]=42;cmd[16]=7;cmd[17]=3;
 int values[]={EFS_OK,EFS_ERR_BUSY,EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_PROTO,EFS_ERR_NOT_PRIMARY};
 for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++){
  verdict=values[i];assert(apply_lane_sweep_cmd(&h,cmd,18,41)==verdict);assert(apply_reap_done_cmd(&h,cmd,17,41)==verdict);assert(apply_orphan_reap_cmd(&h,cmd,17,41)==verdict);
  assert(host_gc_propose(&h,0,cmd,18)==verdict);
  h.r.leader=0;assert(host_gc_propose(&h,0,cmd,18)==verdict);h.r.leader=1;
 }
 assert(waits==7&&forwards==6&&s.gc_sweep_errors==10);
 assert(apply_lane_sweep_cmd(&h,cmd,17,41)==EFS_ERR_PROTO);
 assert(apply_reap_done_cmd(&h,cmd,16,41)==EFS_ERR_PROTO);
 uint8_t ack[25]={EFS_MD_CMD_GC_ACK,0,1};ack[10]=42;ack[18]=7;ack[19]=3;ack[23]=8;ack[24]=2;
 for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++){verdict=values[i];assert(apply_gc_ack_cmd(&h,ack,sizeof(ack),41)==verdict);}
 assert(apply_gc_ack_cmd(&h,ack,24,41)==EFS_ERR_PROTO);
 cmd[0]=EFS_MD_CMD_LANE_SWEEP_BATCH;cmd[18]=1;cmd[22]=3;
 verdict=EFS_ERR_BUSY;
 assert(apply_lane_sweep_cmd(&h,cmd,23,41)==EFS_ERR_BUSY);
 assert(apply_lane_sweep_cmd(&h,cmd,22,41)==EFS_ERR_PROTO);
 cmd[18]=9;assert(apply_lane_sweep_cmd(&h,cmd,23,41)==EFS_ERR_PROTO);cmd[18]=1;
 uint64_t errors=s.gc_sweep_errors;
 assert(host_gc_propose(&h,0,cmd,23)==EFS_ERR_BUSY && s.gc_sweep_errors==errors);
 uint8_t request[18]={EFS_MD_REQ_SWEEP_NEXT},packed[51];uint32_t packed_len=0;
 request[8]=42;request[16]=7;request[17]=3;
 verdict=EFS_OK;
 assert(host_pack_sweep_next(&h,0,request,18,packed,&packed_len)==EFS_OK);
 assert(packed_len==23 && packed[0]==EFS_MD_CMD_LANE_SWEEP_BATCH && packed[18]==1 && packed[22]==3);
 assert(host_pack_sweep_next(&h,2,request,18,packed,&packed_len)==EFS_ERR_INVAL);
 assert(host_pack_sweep_next(&h,0,request,17,packed,&packed_len)==EFS_ERR_INVAL);
 verdict=EFS_ERR_IO;
 assert(host_pack_sweep_next(&h,0,request,18,packed,&packed_len)==EFS_ERR_IO);
 verdict=EFS_ERR_BUSY;errors=s.gc_sweep_errors;
 assert(host_gc_propose(&h,0,request,18)==EFS_ERR_BUSY && s.gc_sweep_errors==errors);
 uint8_t reap[50]={EFS_MD_CMD_REAP_BATCH};reap[8]=42;reap[16]=7;reap[17]=1;reap[18]=27;reap[19]=9;
 for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++){verdict=values[i];assert(apply_reap_done_cmd(&h,reap,50,41)==verdict);}
 assert(apply_reap_done_cmd(&h,reap,49,41)==EFS_ERR_PROTO);
 reap[17]=9;assert(apply_reap_done_cmd(&h,reap,50,41)==EFS_ERR_PROTO);reap[17]=1;
 verdict=EFS_ERR_BUSY;errors=s.gc_reap_errors;
 assert(host_gc_propose(&h,0,reap,50)==EFS_ERR_BUSY && s.gc_reap_errors==errors);
 for(unsigned op=0;op<6;op++){
  uint8_t codes[]={EFS_MD_CMD_LANE_SWEEP,EFS_MD_CMD_REAP_DONE,EFS_MD_CMD_GC_ACK,EFS_MD_CMD_ORPHAN_REAP,EFS_MD_CMD_LANE_SWEEP_BATCH,EFS_MD_CMD_REAP_BATCH,EFS_MD_CMD_CLOSE_STEP};
  assert(core_result(codes[op],EFS_ERR_BUSY,53)==EFS_OK);
  assert(core_result(codes[op],EFS_ERR_IO,53)==EFS_ERR_IO);
  assert(core_result(codes[op],EFS_ERR_NOMEM,53)==EFS_ERR_NOMEM);
 }
 /* CREATE's namespace/lease/replay batch may fail after the log is durable.
  * A follower must retry this index, rather than lose an acknowledged create. */
 assert(core_result(EFS_MD_CMD_APPEND_COMPLETE_ATOMIC,EFS_ERR_IO,53)==EFS_ERR_IO);
 assert(core_result(EFS_MD_CMD_APPEND_COMPLETE_ATOMIC,EFS_ERR_NOMEM,53)==EFS_ERR_NOMEM);
 assert(core_result(EFS_MD_CMD_APPEND_COMPLETE_ATOMIC,EFS_ERR_BUSY,53)==EFS_OK);
 assert(core_result(EFS_MD_CMD_APPEND_COMPLETE_ATOMIC,EFS_ERR_STALE,53)==EFS_OK);
 assert(core_result(EFS_MD_CMD_CREATE,EFS_ERR_IO,53)==EFS_ERR_IO);
 assert(core_result(EFS_MD_CMD_CREATE,EFS_ERR_NOMEM,53)==EFS_ERR_NOMEM);
 assert(core_result(EFS_MD_CMD_CREATE,EFS_ERR_EXIST,53)==EFS_OK);
 assert(core_result(EFS_MD_CMD_CREATE,EFS_ERR_BUSY,53)==EFS_OK);
 assert(core_result(EFS_MD_CMD_APPEND_RSV,EFS_ERR_IO,53)==EFS_ERR_IO);
 assert(core_result(EFS_MD_CMD_APPEND_RSV,EFS_ERR_NOMEM,53)==EFS_ERR_NOMEM);
 assert(core_result(EFS_MD_CMD_APPEND_RSV,EFS_ERR_BUSY,53)==EFS_OK);
 assert(core_result(EFS_MD_CMD_APPEND_RSV,EFS_ERR_IO,45)==EFS_OK);
 /* The container-only cleanup cohort was removed: no path joins it. */
 verdict=0;
 int count=cohort_calls;
 s.repair_experiment=1;assert(!host_bg_propose(&h,0,cmd,18,1));s.repair_experiment=0;
 voters=1;assert(!host_bg_propose(&h,0,cmd,18,1));voters=0;assert(!host_bg_propose(&h,0,cmd,18,1));voters=7;
 h.r.leader=0;assert(!host_bg_propose(&h,0,cmd,18,1));h.r.leader=1;
 s.storage_engine=0;assert(!host_bg_propose(&h,0,cmd,18,1));
 assert(cohort_calls==count);
 verdict=0;durable=EFS_ERR_IO;assert(host_gc_propose(&h,0,cmd,18)==EFS_ERR_IO);
 durable=0;settled_rc=EFS_ERR_NOT_PRIMARY;
 assert(host_bg_propose(&h,0,cmd,18,0)==EFS_ERR_NOT_PRIMARY);
 settled_rc=0;assert(host_bg_propose(&h,0,cmd,18,0)==EFS_OK);
 __atomic_store_n(&h.gc_running,0,__ATOMIC_RELEASE);
 int stopped_waits=waits,stopped_forwards=forwards;
 assert(host_gc_propose(&h,0,cmd,18)==EFS_ERR_BUSY);
 assert(waits==stopped_waits && forwards==stopped_forwards);
 puts("GC apply decoders and durable local/forwarded verdict propagation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-verdict-') as d:
    path=Path(d)/'test.c';path.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-I'+str(root/'include'),str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
