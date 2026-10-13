#!/usr/bin/env python3
"""Execute the actual REAP pass: bounded foreground interference and fair retries."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_REAP_SOURCE',root/'src/server/raft_host.c')).read_text()
def function(start):
    a=s.index(start);return s[a:s.index('\n}',a)+2]
ctx=s[s.index('struct reap_scan_ctx {'):s.index('\n};',s.index('struct reap_scan_ctx {'))+3]
helper=function('static int host_gc_metadata_waiting(') if 'static int host_gc_metadata_waiting(' in s else ''
code=r'''
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define REAP_SCAN_MAX 32
#define REAP_KEY_LEN 11u
#define GC_FRAG_BUDGET_US 200000
#define HOST_WAITERS 512
struct fake_server {pthread_mutex_t lock;uint64_t quota,gc_scan_errors,gc_reap_seen[2],gc_reap_capped[2],gc_first_reap[2],gc_first_seen_us[2];};
struct efs_raft_host {struct fake_server *s;struct efs_kv *kv;pthread_mutex_t cv_mu,wait_mu;void *wait_head;struct {uint8_t active;} waiters[HOST_WAITERS];int gc_running,running;uint8_t reap_cursor[2][11];uint32_t reap_cursor_len[2];};
struct reap_times {uint64_t scan,sweep,fence,done;};
static struct efs_node node;static int missing_node,scan_error,physical_failure,lane_failure;
static uint64_t clock_us;static unsigned attempts,acks[4],lanes[4],dead[4];
static struct efs_node * __attribute__((unused)) server_local_node(struct fake_server *s){(void)s;return missing_node?NULL:&node;}
static uint64_t now_us_(void){return clock_us;}
uint64_t efs_iostats_now_us(void){return clock_us;}
static int env_on(const char *s){(void)s;return 0;}
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v*256+p[i];return v;}
static void wr64be(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-i*8));}
static void wr32be(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-i*8));}
int efs_meta_unpack_reap(const uint8_t *value,uint32_t len,uint64_t *gen,uint64_t *bits){(void)value;if(len!=16)return EFS_ERR_PROTO;*gen=3;*bits=1;return EFS_OK;}
int efs_kv_key_reap_prefix(uint32_t anchor,uint8_t *key,uint32_t *len){key[0]=anchor>>8;key[1]=anchor;key[2]=22;*len=3;return EFS_OK;}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 1;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return 1;}
int efs_meta_lane_sweep_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t lane,uint32_t *indices,uint32_t *n){(void)kv;(void)indices;assert(ino>=11&&ino<=14&&gen==3&&!lane);*n=0;return EFS_OK;}
static int host_gc_inode_nodes(struct efs_raft_host *h,efs_ino_t ino,uint64_t gen){(void)h;assert(gen==3&&lanes[ino-11]);attempts++;clock_us+=150000;return (int)ino==physical_failure?EFS_ERR_BUSY:EFS_OK;}
int efs_meta_reap_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,struct efs_meta_reap_key *keys,uint32_t *n){(void)kv;(void)keys;assert(ino>=11&&ino<=14&&gen==3);*n=0;return EFS_OK;}
static int host_gc_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len){(void)h;(void)group;unsigned i=(unsigned)rd64be(cmd+1)-11;assert(i<4);clock_us+=1000;
 if(cmd[0]==EFS_MD_CMD_LANE_SWEEP_BATCH){assert(len==19);lanes[i]++;return (int)i+11==lane_failure?EFS_ERR_BUSY:EFS_OK;}
 assert(cmd[0]==EFS_MD_CMD_REAP_BATCH&&len==18);assert((int)i+11!=physical_failure&&lanes[i]);acks[i]++;dead[i]=1;return EFS_OK;
}
'''+ctx+'\n'+function('static int reap_scan_cb(')+r'''
typedef int (*visit_fn)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);
int efs_kv_scan_from(struct efs_kv *kv,const uint8_t *prefix,uint32_t pn,const uint8_t *start,uint32_t sn,visit_fn cb,void *user){
 (void)kv;assert(pn==3);if(scan_error)return scan_error;
 for(unsigned i=0;i<4;i++) {uint8_t key[11],value[16]={0};memcpy(key,prefix,3);wr64be(key+3,i+11);
  if(dead[i]||(sn&&memcmp(key,start,sn)<0))continue;
  int rc=cb(user,key,11,value,16);if(rc)return rc;
 }
 return EFS_OK;
}
int efs_kv_scan_prefix(struct efs_kv *kv,const uint8_t *p,uint32_t n,visit_fn cb,void *u){return efs_kv_scan_from(kv,p,n,NULL,0,cb,u);}
'''+helper+'\n'+function('static void host_gc_reap_pass(')+r'''
static void reset(struct efs_raft_host *h){memset(acks,0,sizeof(acks));memset(dead,0,sizeof(dead));memset(lanes,0,sizeof(lanes));memset(h->reap_cursor_len,0,sizeof(h->reap_cursor_len));clock_us=attempts=0;scan_error=physical_failure=lane_failure=missing_node=0;node.used=100;h->wait_head=NULL;memset(h->waiters,0,sizeof(h->waiters));h->waiters[HOST_WAITERS-1].active=1;h->gc_running=h->running=1;h->s->quota=1000;}
int main(void){struct fake_server server={0};struct efs_raft_host h={.s=&server};pthread_mutex_init(&server.lock,NULL);pthread_mutex_init(&h.cv_mu,NULL);pthread_mutex_init(&h.wait_mu,NULL);
 for(unsigned group=0;group<=2;group+=2){unsigned gi=group/2;struct reap_times times={0};
  reset(&h);physical_failure=11;host_gc_reap_pass(&h,group,1,&times);
  assert(attempts==2&&!acks[0]&&acks[1]&&!acks[2]&&!acks[3]);assert(rd64be(h.reap_cursor[gi]+3)==12&&h.reap_cursor_len[gi]==11&&server.gc_reap_capped[gi]);
  host_gc_reap_pass(&h,group,1,&times);assert(attempts==4&&acks[2]&&acks[3]&&!acks[0]);assert(rd64be(h.reap_cursor[gi]+3)==14);
  host_gc_reap_pass(&h,group,1,&times);assert(!h.reap_cursor_len[gi]&&attempts==4);
  physical_failure=0;host_gc_reap_pass(&h,group,1,&times);assert(attempts==5&&acks[0]);
  reset(&h);h.waiters[HOST_WAITERS-1].active=0;h.wait_head=(void*)1;host_gc_reap_pass(&h,group,1,&times);assert(attempts==4); /* POSIX lock waiters are not metadata sleepers */
  reset(&h);node.used=900;host_gc_reap_pass(&h,group,1,&times);assert(attempts==4); /* capacity pressure must reclaim */
  reset(&h);missing_node=1;host_gc_reap_pass(&h,group,1,&times);assert(attempts==4); /* unknown usage not a free-space proof */
  reset(&h);lane_failure=11;host_gc_reap_pass(&h,group,1,&times);assert(!acks[0]&&attempts==2&&acks[1]&&acks[2]);
  reset(&h);h.gc_running=0;host_gc_reap_pass(&h,group,1,&times);assert(!attempts&&!h.reap_cursor_len[gi]);
  reset(&h);scan_error=EFS_ERR_IO;uint64_t before=server.gc_scan_errors;host_gc_reap_pass(&h,group,1,&times);assert(!attempts&&!h.reap_cursor_len[gi]&&server.gc_scan_errors==before+1);
 }
 pthread_mutex_destroy(&h.cv_mu);pthread_mutex_destroy(&h.wait_mu);pthread_mutex_destroy(&server.lock);
 puts("actual REAP fairness: waiting metadata budget, exact cursor, blocked-head retry, idle/quota speed, lane/storage refusal and shutdown PASS");
}
'''
code=code.replace('efs_kv_scan_from','mock_scan_from').replace('efs_kv_scan_prefix','mock_scan_prefix')
with tempfile.TemporaryDirectory(prefix='efs-reap-fairness-') as d:
    p=Path(d)/'test.c';p.write_text(code);exe=p.with_suffix('')
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
