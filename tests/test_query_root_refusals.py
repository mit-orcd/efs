#!/usr/bin/env python3
"""Execute actual root authority/binding branches with controlled boundaries."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int query_directory_binding(');b=s.index('\nstatic int query_ancestry(',a)
code=r'''
#include "efs/meta_apply.h"
#include "efs/name_index.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define HOST_QUERY_AUTH_N 8
#define host_txn_coord NULL
#define efs_meta_apply_get_inode_tx controlled_get_inode_tx
#define efs_meta_apply_lookup_tx controlled_lookup_tx
struct efs_server_query_view {void *catalog;uint64_t generation;};
struct efs_raft_host {void *kv;};
struct query_auth {int valid;struct efs_meta_row row;};
struct query_request {struct efs_raft_host *h;const struct efs_msg_query_snapshot *req;
 const struct efs_server_query_view *view;uint64_t deadline;struct query_auth *auth;unsigned auth_count;};
static uint64_t clock_ms=100;
static int cached_rc,fresh_rc,link_rc,lookup_rc,access_rc,wrong_generation,wrong_parent,wrong_binding,reads;
static uint64_t query_mono_ms(void){return clock_ms;}
static int efs_query_catalog_inode(void *c,efs_ino_t ino,struct efs_meta_row *r,void *stat)
{(void)c;(void)stat;assert(ino==42);reads++;clock_ms+=3;
 *r=(struct efs_meta_row){.ino=42,.generation=9,.parent=12};return cached_rc;}
static int efs_meta_apply_get_inode_tx(void *kv,efs_ino_t ino,void *coord,void *ctx,struct efs_meta_row *r)
{(void)kv;(void)coord;(void)ctx;assert(ino==42);reads++;clock_ms+=7;
 *r=(struct efs_meta_row){.ino=42,.generation=wrong_generation?10:9,.parent=wrong_parent?13:12,.mode=S_IFDIR|0755,.nlink=1};return fresh_rc;}
static int efs_query_catalog_directory_link(void *c,efs_ino_t ino,struct efs_name_entry *link)
{(void)c;assert(ino==42);reads++;clock_ms+=5;memset(link,0,sizeof(*link));link->parent=12;link->dentry.generation=9;strcpy(link->name,"entry");return link_rc;}
static int efs_meta_apply_lookup_tx(void *kv,efs_ino_t parent,const char *name,void *coord,void *ctx,struct efs_meta_dentry *d)
{(void)kv;(void)coord;(void)ctx;assert(parent==12&&!strcmp(name,"entry"));reads++;clock_ms+=11;
 *d=(struct efs_meta_dentry){.ino=42,.generation=wrong_binding?10:9};return lookup_rc;}
static int query_access(const struct efs_msg_query_snapshot *req,const struct efs_meta_row *r,int mask)
{(void)req;assert(r->ino==42&&mask==5);return access_rc;}
'''+s[a:b]+r'''
static int run(unsigned deadline){struct efs_msg_query_snapshot req={.root=42};struct efs_raft_host h={0};
 struct efs_server_query_view view={.generation=2};struct query_auth auth[8]={0};
 struct query_request q={.h=&h,.req=&req,.view=&view,.deadline=deadline,.auth=auth};
 struct efs_meta_row *row=NULL;clock_ms=100;reads=0;
 int rc=query_auth_dir(&q,42,5,&row);if(!rc)assert(row&&row->ino==42&&q.auth_count==1);
 else {assert(!row);}
 return rc;}
int main(void){
 assert(!run(1000)&&reads==4);
 cached_rc=EFS_ERR_NOT_FOUND;assert(run(1000)==EFS_ERR_BUSY&&reads==2);
 fresh_rc=EFS_ERR_BUSY;assert(run(1000)==EFS_ERR_BUSY&&reads==2);fresh_rc=cached_rc=0;
 cached_rc=EFS_ERR_IO;assert(run(1000)==EFS_ERR_IO&&reads==1);cached_rc=0;
 fresh_rc=EFS_ERR_BUSY;assert(run(1000)==EFS_ERR_BUSY&&reads==2);fresh_rc=0;
 wrong_generation=1;assert(run(1000)==EFS_ERR_BUSY&&reads==2);wrong_generation=0;
 wrong_parent=1;assert(run(1000)==EFS_ERR_BUSY&&reads==2);wrong_parent=0;
 link_rc=EFS_ERR_BUSY;assert(run(1000)==EFS_ERR_BUSY&&reads==3);link_rc=0;
 lookup_rc=EFS_ERR_NOT_FOUND;assert(run(1000)==EFS_ERR_BUSY&&reads==4);
 lookup_rc=EFS_ERR_BUSY;assert(run(1000)==EFS_ERR_BUSY&&reads==4);lookup_rc=0;
 wrong_binding=1;assert(run(1000)==EFS_ERR_BUSY&&reads==4);wrong_binding=0;
 assert(run(99)==EFS_ERR_BUSY&&!reads);
 access_rc=EFS_ERR_ACCES;assert(run(1000)==EFS_ERR_ACCES&&reads==4);
 puts("root refusal: cached new root, live TXN, catalog IO, parent/generation, binding missing/TXN/mismatch, expiry and permission retain exact refusal without output PASS");
}
'''
code='#include "'+str(root/'src/common/query_stage_trace.h')+'"\n'+code
with tempfile.TemporaryDirectory(prefix='efs-query-root-') as tmp:
 p=Path(tmp)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 off=subprocess.run([str(exe)],env={**os.environ,'EFS_QUERY_STAGE_TRACE':'0'},capture_output=True,text=True,check=True)
 assert 'query-stage:' not in off.stderr
 on=subprocess.run([str(exe)],env={**os.environ,'EFS_QUERY_STAGE_TRACE':'1'},capture_output=True,text=True,check=True)
 for label in ('root-catalog-absent','root-live-txn','root-catalog-read','root-parent-generation','binding-catalog','binding-missing','binding-live-txn','binding-mismatch','root-expiry'):
  assert 'stage='+label+' ' in on.stderr,label
 assert 'elapsed_ms=10 before=42 after=2 inode=42' in on.stderr
 assert 'elapsed_ms=16 before=42 after=2 inode=42' in on.stderr
 print(off.stdout.strip());print('opt-in root stages: disabled quiet; exact labels, actual leaf durations and root/catalog/inode identity PASS')
