#!/usr/bin/env python3
"""Actual fragment-pass return under bounded scans, ACK failure and no work."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_gc_frag_pass(');b=s.index('static void host_dir_spread_pass',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define GC_ACK_MAX 8
#define GC_KEY_LEN 24
#define GC_FRAG_BUDGET_US 200000
#define GC_FRAG_YIELD_US 0
struct gc_scan_ctx { int n,full,skip_len; unsigned char skip[24],keys[2][24],vals[2][128]; };
struct efs_gc_ack_item { int unused; };
struct efs_export { int unused; };
struct server { uint64_t gc_scan_errors,gc_missing_exports,gc_pending[2]; unsigned gc_sampled_mask; };
struct efs_raft_host { struct server *s; void *kv; int gc_running,running; };
static unsigned char g_gc_cur[2][24]; static int g_gc_cur_len[2];
static int full,scan_error,ack_error,blocked,missing_export,ticks;
static int64_t pending; static struct efs_export ex;
static uint64_t now_us_(void) { return ticks++ ? 200001 : 0; }
static int env_on(const char *s) { (void)s; return 0; }
static int gc_gi(unsigned g) { return g==0?0:1; }
static int efs_kv_key_gc_prefix(uint32_t a,unsigned char *p,uint32_t *n) { (void)a;memset(p,0,3);*n=3;return EFS_OK; }
static int64_t efs_meta_gc_pending_peek(uint32_t a) { (void)a;return pending; }
static int gc_count_cb(void) { return 0; } static int gc_scan_cb(void) { return 0; }
static int efs_kv_scan_prefix(void *kv,unsigned char *p,uint32_t n,int (*cb)(void),uint64_t *v) { (void)kv;(void)p;(void)n;(void)cb;*v=0;return 0; }
static void efs_meta_gc_pending_derived(uint32_t a,uint64_t n) { (void)a;pending=n; }
static int efs_kv_scan_from(void *kv,unsigned char *p,uint32_t n,void *cur,int len,int (*cb)(void),struct gc_scan_ctx *c) { (void)kv;(void)p;(void)n;(void)cur;(void)len;(void)cb;c->n=pending?1:0;c->full=full;return scan_error?-1:full; }
static struct efs_export *host_gc_export(struct efs_raft_host *h) { (void)h;return missing_export?NULL:&ex; }
static int efs_meta_gc_pending_zero_if(uint32_t a,int64_t p) { (void)a;(void)p;pending=0;return 1; }
static int host_gc_ack_flush(struct efs_raft_host *h,uint8_t g,struct efs_gc_ack_item *a,int *n) { (void)h;(void)g;(void)a;*n=0;return ack_error?EFS_ERR_BUSY:EFS_OK; }
static void host_gc_record(struct efs_raft_host *h,struct efs_export *e,unsigned char *k,unsigned char *v,struct efs_gc_ack_item *a,int *n) { (void)h;(void)e;(void)k;(void)v;(void)a;if(!blocked)*n=1; }
static void server_export_put(struct server *s,struct efs_export *e) { (void)s;(void)e; }
'''+s[a:b]+r'''
static int run_case(int is_full,int is_blocked,int fail_ack,int fail_scan,int missing,int64_t pend) {
 struct server server={0};struct efs_raft_host h={&server,NULL,1,1};
 full=is_full;blocked=is_blocked;ack_error=fail_ack;scan_error=fail_scan;missing_export=missing;pending=pend;ticks=0;
 memset(g_gc_cur_len,0,sizeof(g_gc_cur_len));
 return host_gc_frag_pass(&h,0,1);
}
int main(void) {
 assert(run_case(1,0,0,0,0,100)==1); /* overwrite backlog, no REAP marker */
 assert(run_case(0,0,0,0,0,100)==0); /* incomplete scan does not prove budget cap */
 assert(run_case(1,1,0,0,0,100)==0); /* blocked/offline deletions */
 assert(run_case(1,0,1,0,0,100)==0); /* failed durable ACK */
 assert(run_case(1,0,0,1,0,100)==0); /* failed scan */
 assert(run_case(1,0,0,0,1,100)==0); /* unavailable export */
 assert(run_case(1,0,0,0,0,0)==0); /* empty */
 puts("GC fragment backlog: actual budget/ACK/blocked/scan/export/empty cases PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-fragment-idle-') as temp:
 p=Path(temp)/'test.c';p.write_text(code)
 command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(command,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
