#!/usr/bin/env python3
"""Exercise production GC command decoders, local and forwarded verdicts."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = (root/'src/server/raft_host.c').read_text()
def function(name):
    m = re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{', text, re.M)
    if not m: raise RuntimeError(name)
    return text[m.start():text.index('\n}', m.end())+2]
apply=text[text.index('static int host_apply(void *app'):];apply=apply[apply.index('    ret = (cmd[0]'):apply.index('    if (a0) {')]
source = r'''
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
struct server {uint64_t gc_sweep_errors,gc_reap_errors,gc_ack_errors;};
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft r;struct server *s;struct efs_kv *kv;};
static int verdict, waits, forwards, durable;
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){(void)g;return &h->r;}
static int efs_raft_role(struct efs_raft*r){return r->leader;}
static int efs_raft_leader(struct efs_raft*r){(void)r;return 2;}
static int efs_raft_propose(struct efs_raft*r,const uint8_t*c,uint32_t n,uint64_t*i){(void)r;(void)c;(void)n;*i=41;return 0;}
static uint64_t efs_raft_term(struct efs_raft*r){(void)r;return 7;}
static int efs_raft_durable(struct efs_raft*r,uint64_t i){(void)r;assert(i==41);return durable;}
static void host_pump_kick(struct efs_raft_host*h){(void)h;}
static int host_wait_settled(struct efs_raft_host*h,uint8_t g,uint64_t i,uint64_t t,int*p){(void)h;(void)g;(void)i;(void)t;(void)p;assert(0);return 0;}
static int host_wait_verdict(struct efs_raft_host*h,uint8_t g,uint64_t i,uint64_t t,int*p){(void)h;(void)g;(void)p;assert(i==41&&t==7);waits++;return verdict;}
static int host_wait_applied(struct efs_raft_host*h,uint8_t g,uint64_t i,int*p){(void)h;(void)g;(void)p;assert(i==41);waits++;return 0;}
static int host_remote_cmd(struct efs_raft_host*h,uint8_t g,const uint8_t*c,uint32_t n,struct efs_msg_raft_mkfs_reply*p,int lid){(void)h;(void)g;(void)c;(void)n;assert(lid==2);forwards++;p->index=41;return verdict;}
static uint64_t rd64be(const uint8_t*p){uint64_t x=0;for(int i=0;i<8;i++)x=(x<<8)|p[i];return x;}
static uint32_t rd32be(const uint8_t*p){return (uint32_t)rd64be((const uint8_t[]){0,0,0,0,p[0],p[1],p[2],p[3]});}
int efs_meta_apply_lane_sweep(struct efs_kv*k,efs_ino_t i,uint64_t g,uint8_t l){(void)k;assert(i==42&&g==7&&l==3);return verdict;}
int efs_meta_apply_orphan_reclaim(struct efs_kv*k,efs_ino_t i,uint64_t g){(void)k;assert(i==42&&g==7);return verdict;}
int efs_meta_apply_reap_done(struct efs_kv*k,efs_ino_t i,uint64_t g){(void)k;assert(i==42&&g==7);return verdict;}
int efs_meta_apply_gc_ack(struct efs_kv*k,const struct efs_gc_ack_item*p,uint32_t n){(void)k;assert(n==1&&p->ino==42&&p->gen==7&&p->lane==3&&p->ci==8&&p->frag==2);return verdict;}
''' + '\n'.join(function(n) for n in ('apply_lane_sweep_cmd','apply_reap_done_cmd','apply_orphan_reap_cmd','apply_gc_ack_cmd','host_bg_propose','host_gc_propose')) + '\nstatic int core_result(uint8_t opcode,int rc){uint8_t cmd[1]={opcode};int ret;\n'+apply+'\nreturn ret;}\n' + r'''
int main(void){
 struct server s={0};struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.s=&s,.r={1}};
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
 for(unsigned op=0;op<4;op++){
  uint8_t codes[]={EFS_MD_CMD_LANE_SWEEP,EFS_MD_CMD_REAP_DONE,EFS_MD_CMD_GC_ACK,EFS_MD_CMD_ORPHAN_REAP};
  assert(core_result(codes[op],EFS_ERR_BUSY)==EFS_OK);
  assert(core_result(codes[op],EFS_ERR_IO)==EFS_ERR_IO);
  assert(core_result(codes[op],EFS_ERR_NOMEM)==EFS_ERR_NOMEM);
 }
 verdict=0;durable=EFS_ERR_IO;assert(host_gc_propose(&h,0,cmd,18)==EFS_ERR_IO);
 puts("GC apply decoders and durable local/forwarded verdict propagation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-verdict-') as d:
    path=Path(d)/'test.c';path.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
