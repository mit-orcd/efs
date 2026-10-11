#!/usr/bin/env python3
"""Compile production stream admission/lifetime; exercise independent caller pins."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/server/raft_host.c').read_text()
a = s.index('#define HOST_QUERY_PAGE_SESSIONS ')
b = s.index('static int host_query_cancel', a)
code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct efs_query_catalog {int refs;};
struct efs_server_query_view {struct efs_query_catalog *catalog;};
struct {int raft_id;uint64_t boot_id;} host={1,987},*g_host=&host;
static uint64_t clock_ms=1000;
static uint64_t query_mono_ms(void){return clock_ms;}
static uint64_t now_ns(void){return 567890;}
static int query_export_expired(uint64_t now,uint64_t touched,uint64_t born){return now-touched>60000||now-born>3600000;}
static void efs_query_catalog_free(struct efs_query_catalog *c);
''' + s[a:b] + r'''
static void efs_query_catalog_free(struct efs_query_catalog *c){
 /* Last-reference destruction must never hold stream admission/lifetime lock. */
 assert(!pthread_mutex_trylock(&query_page_mu));pthread_mutex_unlock(&query_page_mu);
 if(c)assert(c->refs-->0);
}
static struct efs_msg_query_snapshot_v4 req={.base={.uid=100,.gid=100,.root=1},.limit=1};
static void *independent(void *unused){(void)unused;struct efs_msg_query_snapshot_v4 r=req;r.nonce=2;struct query_page_session *p;int created;assert(!query_page_acquire(&r,&p,&created)&&created);query_page_release(p,1);return NULL;}
int main(void){
 struct query_page_session *slots[HOST_QUERY_PAGE_SESSIONS],*p;int created;
 uint64_t tokens[HOST_QUERY_PAGE_SESSIONS];struct efs_query_catalog catalog={0};
 assert(!query_page_acquire(&req,&p,&created)&&created);uint64_t first=p->token;
 /* A reserved slot holds no global lock: a different stream can progress. */
 pthread_t t;assert(!pthread_create(&t,NULL,independent,NULL));assert(!pthread_join(t,NULL));
 assert(query_page_acquire(&req,&slots[0],&created)==EFS_ERR_BUSY);
 p->view.catalog=&catalog;catalog.refs++;query_page_release(p,0);
 assert(!query_page_acquire(&req,&p,&created)&&!created&&p->token==first);query_page_release(p,0);
 for(unsigned i=0;i<HOST_QUERY_PAGE_SESSIONS;i++){
  req.nonce=i;assert(!query_page_acquire(&req,&slots[i],&created));
  tokens[i]=slots[i]->token;
  for(unsigned j=0;j<i;j++)assert(tokens[j]!=tokens[i]);
  if(created){slots[i]->view.catalog=&catalog;catalog.refs++;}
  query_page_release(slots[i],0);
 }
 req.nonce=100;assert(query_page_acquire(&req,&p,&created)==EFS_ERR_BUSY);
 req.nonce=0;req.token=tokens[0];req.base.uid=101;
 assert(query_page_acquire(&req,&p,&created)==EFS_ERR_ACCES);req.base.uid=100;
 assert(!query_page_acquire(&req,&p,&created)&&!created);
 clock_ms+=60001;query_page_sweep(0);assert(p->token==tokens[0]); /* busy pin cannot expire */
 assert(catalog.refs==1);query_page_release(p,0);
 req.token=tokens[1];req.nonce=1;assert(query_page_acquire(&req,&p,&created)==EFS_ERR_STALE);
 req.token=tokens[0];req.nonce=0;assert(!query_page_acquire(&req,&p,&created));query_page_release(p,1);
 assert(!catalog.refs);assert(query_page_acquire(&req,&p,&created)==EFS_ERR_STALE);
 req.token=0;assert(!query_page_acquire(&req,&p,&created)&&created&&p->token!=first);
 query_page_release(p,0);query_page_sweep(1);
 puts("query streams: independent progress, 16-slot bound, identity/token isolation, same-open reuse, busy/idle expiry and balanced catalog release PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-query-streams-') as td:
    p=Path(td)/'test.c';p.write_text(code);exe=p.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
