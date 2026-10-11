#!/usr/bin/env python3
"""A captured dentry followed by a missing inode must not become name absence."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('void server_raft_host_lookup(');b=s.index('\n}',a)+2
code=r'''
#include <efs/meta_apply.h>
#include <efs/protocol.h>
#include <efs/raft.h>
#include <assert.h>
#include <string.h>
struct efs_raft_host {int running;struct efs_kv *kv;};
static struct efs_raft_host h={1,0},*g_host=&h;
static struct {uint64_t lk_fwd,lk_total,lk_ri_parent,lk_kv,lk_lanes,lk_getattr;} g_read_prof;
static int missing_name,missing_inode,forward,read_error,pending_phase,coord_error,forwarded_lookups,pair_error,plan_other;
static int read_prof_on(void){return 0;}
static uint64_t now_us_(void){return 0;}
static void read_prof_add(uint64_t*p,uint64_t n){*p+=n;}
static void read_prof_tick(void){}
static int host_hosts(struct efs_raft_host*p,uint8_t g){(void)p;return !forward||g==efs_raft_shard_group(efs_kv_inode_shard(1));}
static int host_read_index(struct efs_raft_host*p,uint8_t g,int*i){(void)p;(void)g;(void)i;return read_error;}
static uint8_t host_lookup_plan_group(struct efs_raft_host*p,efs_ino_t i,const char*n,uint8_t pg){(void)p;(void)i;(void)n;return plan_other?(pg==0?2:0):pg;}
static int host_read_index_pair(struct efs_raft_host*p,uint8_t first,uint8_t second,int*i){if(first!=second && pair_error)return pair_error;int rc=host_read_index(p,first,i);return !rc && first!=second?host_read_index(p,second,i):rc;}

static void host_fwd_lookup(struct efs_raft_host*p,efs_ino_t i,const char*n,struct efs_msg_inode_reply*r,const uint8_t*g,int c){(void)p;(void)g;(void)c;forwarded_lookups++;r->status=EFS_INODE_RPC_OK;r->inode.ino=2;r->inode.parent=i;strcpy(r->inode.name,n);}
static void host_fwd_getattr(struct efs_raft_host*p,efs_ino_t i,struct efs_msg_inode_reply*r,const uint8_t*g,int c){(void)p;(void)g;(void)c;r->status=missing_inode?EFS_INODE_RPC_NOT_FOUND:EFS_INODE_RPC_OK;r->inode.ino=i;}
static int host_txn_coord(void*u,const struct efs_txid*t,uint32_t sh,int*d){(void)t;if(!host_hosts(u,efs_raft_shard_group(sh)))return EFS_ERR_IO;*d=EFS_TXN_COMMIT;return coord_error;}
int efs_meta_apply_get_inode(struct efs_kv*k,efs_ino_t i,struct efs_meta_row*r){(void)k;(void)i;memset(r,0,sizeof(*r));r->layout=EFS_META_LAYOUT_LOCAL;return 0;}
int efs_meta_apply_lookup_tx(struct efs_kv*k,efs_ino_t i,const char*n,efs_txn_coord_fn f,void*u,struct efs_meta_dentry*r){(void)k;(void)i;(void)n;r->ino=pending_phase==2?3:2;if(pending_phase==1){struct efs_txid t={{0}};int d;int rc=f(u,&t,2,&d);if(rc)return rc;}return missing_name?EFS_ERR_NOT_FOUND:0;}
static int host_read_inode_lanes_covered(struct efs_raft_host*p,efs_ino_t i,int*h,uint8_t covered){(void)p;(void)i;(void)h;assert(covered);return missing_inode?EFS_ERR_NOT_FOUND:0;}
static void host_need_both(uint8_t g[2]){g[0]=0;g[1]=1;}
int efs_meta_apply_getattr(struct efs_kv*k,efs_ino_t i,efs_txn_coord_fn f,void*u,struct efs_meta_stat*r){(void)k;memset(r,0,sizeof(*r));r->ino=i;if(pending_phase==2){struct efs_txid t={{0}};int d;return f(u,&t,2,&d);}return 0;}
static void set_inode_rc(struct efs_msg_inode_reply*r,int rc,int h){(void)h;r->status=rc==0?EFS_INODE_RPC_OK:rc==EFS_ERR_BUSY?EFS_INODE_RPC_BUSY:rc==EFS_ERR_NOT_FOUND?EFS_INODE_RPC_NOT_FOUND:EFS_INODE_RPC_ERROR;}
static void stat_to_inode(const struct efs_meta_stat*s,struct efs_inode*i){i->ino=s->ino;}
'''.replace('int efs_meta_apply_get_inode(',s[s.index('struct host_txn_read_context {'):s.index('static void stat_to_inode(')]+'int efs_meta_apply_get_inode(',1)+s[a:b]+r'''
int main(void){struct efs_msg_inode_reply r;
 for(forward=0;forward<2;forward++){
  missing_name=0;missing_inode=1;server_raft_host_lookup(1,"replacement",&r);assert(r.status==EFS_INODE_RPC_BUSY);
  missing_inode=0;server_raft_host_lookup(1,"replacement",&r);assert(r.status==EFS_INODE_RPC_OK&&r.inode.ino==2);
  missing_name=1;server_raft_host_lookup(1,"absent",&r);assert(r.status==EFS_INODE_RPC_NOT_FOUND);
  read_error=EFS_ERR_IO;server_raft_host_lookup(1,"replacement",&r);assert(r.status==EFS_INODE_RPC_ERROR);read_error=0;
 }
 forward=1;missing_name=missing_inode=0;
 for(pending_phase=1;pending_phase<=2;pending_phase++){
  int old=forwarded_lookups;server_raft_host_lookup(1,"pending",&r);
  assert(r.status==EFS_INODE_RPC_OK&&r.inode.ino==2&&forwarded_lookups==old+1);
 }
 forward=0;pending_phase=0;plan_other=1;pair_error=EFS_ERR_BUSY;
 server_raft_host_lookup(1,"stale-hint",&r);assert(r.status==EFS_INODE_RPC_OK);
 pair_error=EFS_ERR_NOT_PRIMARY;server_raft_host_lookup(1,"stale-hint",&r);assert(r.status==EFS_INODE_RPC_OK);
 pair_error=EFS_ERR_IO;server_raft_host_lookup(1,"hint-io",&r);assert(r.status==EFS_INODE_RPC_ERROR);
 plan_other=pair_error=0;
 forward=0;pending_phase=1;coord_error=EFS_ERR_IO;
 int old=forwarded_lookups;server_raft_host_lookup(1,"corrupt",&r);
 assert(r.status==EFS_INODE_RPC_ERROR&&forwarded_lookups==old);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-lookup-replacement-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('LOOKUP replacement: missing captured inode retryable locally and forwarded; true absence and IO preserved PASS')
