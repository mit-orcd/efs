#!/usr/bin/env python3
"""Compile production stats serialization for legacy and retained snapshots."""
from pathlib import Path
import json, os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('            struct efs_query_rollup stats;',s.index('static int query_consume('))
b=s.index('        } else if (!rc) {',a)
code=r'''
#include "efs/query_catalog.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
struct query_request {void *page;uint64_t page_token;char text[4096];size_t len;};
struct {struct efs_query_catalog *catalog;} catalog_view,*view=&catalog_view;
struct {uint64_t root;} request,*req=&request;
int efs_query_catalog_stats(const struct efs_query_catalog *c,efs_ino_t ino,struct efs_query_rollup *out){
 (void)c;(void)ino;*out=(struct efs_query_rollup){.tree_files=7,.tree_bytes=512};return 0;
}
static int query_append(struct query_request *q,const char *s,size_t n){assert(q->len+n<sizeof(q->text));memcpy(q->text+q->len,s,n);q->len+=n;q->text[q->len]=0;return 0;}
static void render(int pinned){
 struct query_request state={.page=pinned?(void *)1:NULL,.page_token=144115188075855873},*q=&state;
 int rc=0;query_append(q,"{",1);
'''+s[a:b]+r'''
 assert(!rc);puts(q->text);
}
int main(void){render(0);render(1);return 0;}
'''
# The extracted block uses the production q->req binding.
code=code.replace('q->req->root','req->root')
with tempfile.TemporaryDirectory(prefix='efs-stats-snapshot-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 legacy,pinned=map(json.loads,subprocess.check_output([str(binary)],text=True).splitlines())
 assert legacy=={'stats':pinned['stats']} and pinned['stats']['tree_files']==7
 assert pinned['page']=={'token':'144115188075855873','cursor':'0','more':False,'more_confirmed':False}
print('stats serialization: legacy envelope and stable retained snapshot token PASS')
