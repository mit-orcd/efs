#!/usr/bin/env python3
"""Actual fragment-GC pass only reports successfully progressing budget stops."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_GC_FRAGMENT_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static int host_gc_frag_pass(');b=s.index('\n}',a)+2
ctx=s[s.index('struct gc_scan_ctx {'):s.index('\n};',s.index('struct gc_scan_ctx {'))+3]
source=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define GC_SCAN_MAX 4
#define GC_KEY_LEN 24
#define EFS_META_GC_VAL 64
#define GC_ACK_MAX 6
#define GC_FRAG_BUDGET_US 200000
#define GC_FRAG_YIELD_US 0
struct efs_gc_ack_item {int unused;};
struct efs_export {int unused;};
struct fake_server {int repair_experiment;uint64_t gc_scan_errors,gc_pending[2],gc_sampled_mask,gc_missing_exports;};
struct efs_raft_host {void *kv;struct fake_server *s;int gc_running,running;};
'''+ctx+r'''

static uint8_t g_gc_cur[2][GC_KEY_LEN];static uint32_t g_gc_cur_len[2];
static uint64_t clock_us,record_cost;static unsigned attempts;static int64_t pending;static int full,records,scan_error,flush_error,acks_enabled,missing_export,scan_calls,flush_calls;
static struct efs_export export;
static uint64_t now_us_(void){return clock_us;}
static int env_on(const char *s){(void)s;return 0;}
static int gc_gi(uint8_t group){return group==0?0:1;}
static int efs_kv_key_gc_prefix(uint32_t anchor,uint8_t *out,uint32_t *n){(void)out;*n=3;return anchor?0:EFS_ERR_INVAL;}
static int64_t efs_meta_gc_pending_peek(uint32_t a){(void)a;return pending;}
static void efs_meta_gc_pending_derived(uint32_t a,uint64_t n){(void)a;pending=n;}
static int efs_meta_gc_pending_zero_if(uint32_t a,int64_t n){(void)a;if(pending==n){pending=0;return 1;}return 0;}
static int gc_count_cb(void *ctx,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn){(void)ctx;(void)k;(void)kn;(void)v;(void)vn;return 0;}
static int gc_scan_cb(void *ctx,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn){(void)ctx;(void)k;(void)kn;(void)v;(void)vn;return 0;}
typedef int (*cb_t)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);
static int efs_kv_scan_prefix(void *kv,const uint8_t *p,uint32_t n,cb_t cb,void *ctx){(void)kv;(void)p;(void)n;(void)cb;*(uint64_t *)ctx=records;return scan_error;}
static int efs_kv_scan_from(void *kv,const uint8_t *p,uint32_t pn,const uint8_t *start,uint32_t sn,cb_t cb,void *ctx){(void)kv;(void)p;(void)pn;(void)start;(void)sn;(void)cb;scan_calls++;struct gc_scan_ctx *c=ctx;c->n=records;c->full=full;for(int i=0;i<records;i++)c->keys[i][GC_KEY_LEN-1]=(uint8_t)(i+1);return scan_error;}
static struct efs_export *host_gc_export(struct efs_raft_host *h){(void)h;return missing_export?NULL:&export;}
static void server_export_put(struct fake_server *s,struct efs_export *ex){(void)s;(void)ex;}
static void host_gc_record(struct efs_raft_host *h,struct efs_export *ex,const uint8_t *k,const uint8_t *v,struct efs_gc_ack_item *acks,int *n){(void)h;(void)ex;(void)k;(void)v;(void)acks;attempts++;clock_us+=record_cost;if(acks_enabled)(*n)+=3;}
static int host_gc_ack_flush(struct efs_raft_host *h,uint8_t g,struct efs_gc_ack_item *acks,int *n){(void)h;(void)g;(void)acks;flush_calls++;clock_us+=GC_FRAG_BUDGET_US+1;if(flush_error)return flush_error;*n=0;return 0;}
'''+s[a:b]+r'''
static void reset(void){clock_us=record_cost=attempts=0;pending=100;full=1;records=1;scan_error=flush_error=missing_export=scan_calls=flush_calls=0;acks_enabled=1;memset(g_gc_cur_len,0,sizeof(g_gc_cur_len));}
int main(void){struct fake_server server={0};struct efs_raft_host h={.s=&server,.gc_running=1,.running=1};
 for(unsigned group=0;group<=2;group+=2){
  reset();assert(host_gc_frag_pass(&h,group,1)==1 && scan_calls==1 && flush_calls==1);
  reset();records=4;assert(host_gc_frag_pass(&h,group,1)==1 && flush_calls==2);
  reset();records=4;record_cost=150000;
  assert(host_gc_frag_pass(&h,group,1)==1&&attempts==2&&flush_calls==1&&scan_calls==1);
  assert(g_gc_cur_len[gc_gi(group)]==GC_KEY_LEN&&g_gc_cur[gc_gi(group)][GC_KEY_LEN-1]==2);
  reset();records=4;record_cost=250000;acks_enabled=0;
  assert(!host_gc_frag_pass(&h,group,1)&&attempts==1&&g_gc_cur[gc_gi(group)][GC_KEY_LEN-1]==1);
  reset();pending=0;assert(!host_gc_frag_pass(&h,group,1) && !scan_calls);
  reset();full=0;assert(!host_gc_frag_pass(&h,group,1));
  reset();acks_enabled=0;assert(!host_gc_frag_pass(&h,group,1));
  reset();flush_error=EFS_ERR_IO;assert(!host_gc_frag_pass(&h,group,1));
  reset();scan_error=EFS_ERR_IO;assert(!host_gc_frag_pass(&h,group,1));
  reset();missing_export=1;assert(!host_gc_frag_pass(&h,group,1));
  reset();h.gc_running=0;assert(!host_gc_frag_pass(&h,group,1));h.gc_running=1;
  reset();pending=-1;scan_error=EFS_ERR_IO;assert(!host_gc_frag_pass(&h,group,1));
  reset();records=0;assert(!host_gc_frag_pass(&h,group,1) && pending==0);
  reset();assert(!host_gc_frag_pass(&h,group,0) && !scan_calls);
 }
 puts("fragment GC actual pass: both groups, full progressing budget and mid-batch ACKs; idle, blocked, errors, missing export, exhaustion and shutdown retain backoff PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-frag-backlog-') as d:
 p=Path(d)/'t.c';p.write_text(source);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
