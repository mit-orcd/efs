#!/usr/bin/env python3
"""Execute actual v4 page dispatch: one fresh cut, fresh continuations, refusal."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('int server_raft_host_query_page(')
b=s.index('\nint server_raft_host_start(',a)
code=r'''
#include "efs/protocol.h"
#include "efs/name_index.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define HOST_QUERY_REPLY_BYTES (64ULL<<20)
#define HOST_QUERY_AUTH_N 8192
struct efs_server_query_view {int unused;};
struct {int query_enabled;pthread_mutex_t mu;struct {uint8_t group;} g[2];} host,*g_host=&host;
static uint32_t query_requests;static uint64_t query_reply_bytes;
struct query_allocation {uint64_t bytes;};
struct query_auth {int unused;};
struct query_request {void *h;const struct efs_msg_query_snapshot *req;
 const struct efs_query_filters *filters;const struct efs_msg_query_snapshot_v4 *page;
 uint64_t page_token,deadline;struct efs_msg_query_snapshot_reply *reply;char *text;struct query_auth *auth;};
struct query_page_session {uint64_t token;struct efs_server_query_view view;int refresh_error;};
static struct query_page_session slot={.token=99};
static int fresh=1,captures,barriers,consumed,barrier_rc,consume_rc,capable=1,released,cleared;
static uint64_t query_mono_ms(void){return 100;}
static int host_query_policy_operation(unsigned op){(void)op;return 0;}
int efs_query_filters_valid(const struct efs_query_filters *f){(void)f;return 1;}
static int query_page_acquire(const struct efs_msg_query_snapshot_v4 *r,struct query_page_session **p,int *created)
{(void)r;*p=&slot;*created=fresh;return 0;}
static int query_page_capture(void *ctx,const struct efs_server_query_view *v,int error)
{(void)ctx;(void)v;(void)error;return 0;}
static int server_raft_host_query_read(int (*fn)(void *,const struct efs_server_query_view *,int),void *ctx,uint16_t *primary)
{*primary=2;captures++;barriers++;return barrier_rc?barrier_rc:fn(ctx,&slot.view,0);}
static int host_read_index_pair(void *h,uint8_t first,uint8_t second,int *hint)
{assert(h==g_host && first==0 && second==2);(void)hint;barriers++;return barrier_rc;}
static int host_query_capable(void *h){assert(h==g_host);return capable;}
static int query_consume(void *ctx,const struct efs_server_query_view *v,int error)
{struct query_request *q=ctx;assert(q->auth && v==&slot.view && !error);consumed++;return consume_rc;}
static void query_page_release(struct query_page_session *p,int clear)
{assert(p==&slot);released++;cleared=clear;}
'''+s[a:b]+r'''
static struct efs_msg_query_snapshot_v4 req={.base={.version=EFS_QUERY_PAGE_VERSION,
 .root=1,.operation=EFS_QUERY_STATS,.max_output=16384,.visit_budget=100,.match=EFS_NAME_PREFIX},.limit=1};
static int run(void){void *raw=NULL;uint32_t bytes=0;captures=barriers=consumed=released=cleared=0;
 assert(!server_raft_host_query_page(&req,&raw,&bytes));assert(raw && released==1);
 struct efs_msg_query_snapshot_reply *r=raw;assert(r->version==EFS_QUERY_PAGE_VERSION);
 int rc=r->status;struct query_allocation *a=(struct query_allocation *)raw-1;
 query_reply_bytes-=a->bytes;query_requests--;free(a);assert(!query_reply_bytes && !query_requests);return rc;}
int main(void){host.query_enabled=1;host.g[0].group=0;host.g[1].group=2;pthread_mutex_init(&host.mu,NULL);
 assert(!run() && captures==1 && barriers==1 && consumed==1 && !cleared);
 fresh=0;req.token=99;assert(!run() && !captures && barriers==1 && consumed==1);
 barrier_rc=EFS_ERR_BUSY;assert(run()==EFS_ERR_BUSY && barriers==1 && !consumed && !cleared);
 fresh=1;req.token=0;assert(run()==EFS_ERR_BUSY && captures==1 && barriers==1 && !consumed && cleared);
 barrier_rc=0;consume_rc=EFS_ERR_ACCES;assert(run()==EFS_ERR_ACCES && consumed==1 && cleared);
 consume_rc=0;fresh=0;req.token=99;req.limit=0;assert(!run() && !captures && !barriers && !consumed && cleared);
 req.limit=1;capable=0;assert(run()==EFS_ERR_BUSY && barriers==1 && !consumed);
 puts("query pages: first-page capture supplies fresh authority once; continuations recheck, failed cuts emit no bytes, fresh permission refusal and close PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-query-page-barriers-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-pthread','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
