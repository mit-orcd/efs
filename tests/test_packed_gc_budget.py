#!/usr/bin/env python3
"""Actual packed GC pass signals saturated pages and time-budget exhaustion."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/packed_gc.h').read_text()
start=s.index('static int host_gc_pack_pass(')
code=r'''
#include "efs/pack_admission.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define efs_kv_scan_from mock_kv_scan_from
#define HOST_NGROUPS 2
#define HOST_SESS_REG_LEN 26
#define GC_FRAG_BUDGET_US 200000
struct server {uint64_t gc_scan_errors;};
struct efs_raft_host {struct efs_kv *kv;struct server *s;int gc_running;};
struct pack_gc_scan {struct efs_pack_object object[8];uint8_t skip[19],last[19];uint32_t count,skip_len,visited,last_len;int error;};
static uint8_t pack_gc_cursor[2][19];static uint32_t pack_gc_cursor_len[2];
static unsigned candidates,visited,phase,storage_calls,commands,removed;
static int scan_error,storage_error,cleanup_error;
static uint64_t clock_us,storage_us;
static struct efs_pack_object current;
static int gc_gi(uint8_t g){assert(g==0||g==2);return g/2;}
static uint64_t now_us_(void){return clock_us;}
static void wr32be(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static int pack_gc_scan_cb(void *v,const uint8_t *k,uint32_t kl,const uint8_t *b,uint32_t bl){(void)v;(void)k;(void)kl;(void)b;(void)bl;return 0;}
int efs_kv_scan_from(struct efs_kv *kv,const uint8_t *prefix,uint32_t pl,const uint8_t *after,uint32_t al,int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *v){
 (void)kv;(void)after;(void)al;assert(pl==3&&prefix[2]==EFS_KV_KIND_PACK_OBJECT&&cb==pack_gc_scan_cb);
 struct pack_gc_scan *scan=v;scan->count=candidates;scan->visited=visited;scan->error=scan_error;
 for(unsigned i=0;i<candidates;i++)scan->object[i].ledger=(struct efs_pack_ledger){.publication_group=0,.member_mask=1,.phase=phase};
 return candidates==8?1:0;
}
static int host_session_cleanup_admit(struct efs_raft_host *h,const uint8_t *cmd,uint32_t n,uint8_t group,int *hint){(void)h;(void)group;(void)hint;assert(n==26&&cmd[1]==EFS_MD_SESS_LEASE_LOCK_BATCH);return cleanup_error;}
static int host_pack_gc_storage(struct efs_raft_host *h,const struct efs_pack_object *o,uint32_t member,int deletion){(void)h;assert(member==1&&o->ledger.phase==EFS_PACK_RECLAIMING);if(deletion)assert(o->ledger.fence_acks==1);storage_calls++;clock_us+=storage_us;if(!storage_error&&deletion)removed++;return storage_error;}
static int host_pack_gc_command(struct efs_raft_host *h,const struct efs_pack_object *o,uint8_t tag,uint32_t bit,int deletion){
 (void)h;commands++;current=*o;
 if(tag==EFS_MD_CMD_PACK_GC_ACK){assert(bit==1);if(deletion){assert(current.ledger.fence_acks==1);current.ledger.delete_acks=1;current.ledger.phase=EFS_PACK_RETIRED;}else current.ledger.fence_acks=1;}
 else assert(tag==EFS_MD_CMD_PACK_GC_RETIRE);
 return 0;
}
int efs_pack_lifetime_get(struct efs_kv *kv,const struct efs_pack_object *o,struct efs_pack_object *out){(void)kv;(void)o;*out=current;return 0;}
'''+s[start:]+r'''
static void reset(void){candidates=visited=storage_calls=commands=removed=0;phase=EFS_PACK_RETIRED;scan_error=storage_error=cleanup_error=0;clock_us=storage_us=0;memset(pack_gc_cursor_len,0,sizeof(pack_gc_cursor_len));}
int main(void){struct server server={0};struct efs_raft_host h={.s=&server,.gc_running=1};
 reset();assert(!host_gc_pack_pass(&h,0,1)&&!commands);
 reset();candidates=4;assert(!host_gc_pack_pass(&h,0,1)&&commands==4&&!storage_calls);
 reset();candidates=8;assert(host_gc_pack_pass(&h,0,1)==1&&commands==8);
 reset();visited=2048;assert(host_gc_pack_pass(&h,0,1)==1&&!commands);
 reset();candidates=8;scan_error=EFS_ERR_IO;assert(!host_gc_pack_pass(&h,0,1)&&server.gc_scan_errors==1&&!commands);
 reset();candidates=8;phase=EFS_PACK_ACTIVE;cleanup_error=EFS_ERR_BUSY;assert(host_gc_pack_pass(&h,0,1)==1&&!storage_calls&&!commands&&!removed);
 reset();candidates=1;phase=EFS_PACK_RECLAIMING;storage_us=150001;assert(host_gc_pack_pass(&h,0,1)==1&&storage_calls==2&&removed==1&&commands==3);
 reset();candidates=8;phase=EFS_PACK_RECLAIMING;storage_error=EFS_ERR_NET;assert(host_gc_pack_pass(&h,0,1)==1&&!removed&&storage_calls==8&&!commands);
 puts("packed GC budget: empty/partial/full pages, scan ceiling, refusal, blocked owners, durable fence-before-delete and elapsed cap PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-gc-budget-') as td:
 p=Path(td)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
