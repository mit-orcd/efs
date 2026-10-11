#!/usr/bin/env python3
"""An RPC may share its own authority fences, never another RPC's fences."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/server/raft_host.c').read_text()
def function(name):
    start = source.index('static int '+name+'(')
    return source[start:source.index('\n}', start)+2]
code = r'''
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;};
struct host_txn_read_context {struct efs_raft_host *host;int unhosted_coordinator;};
static struct efs_meta_row row;
static int calls[3], read_fail, inode_fail, unhosted, coordinator,pair_fail,hint_empty;
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint) {
 (void)h;calls[g]++;if(g==2 && read_fail){*hint=7;return read_fail;}return EFS_OK;
}
static int host_txn_read_coord(void *u,const struct efs_txid *t,uint32_t s,int *d) {
 (void)u;(void)t;(void)s;(void)d;return EFS_OK;
}
int efs_meta_apply_get_inode_tx(struct efs_kv *k,efs_ino_t i,efs_txn_coord_fn f,void *u,struct efs_meta_row *r) {
 (void)k;(void)i;(void)f;*r=row;
 if(coordinator){((struct host_txn_read_context*)u)->unhosted_coordinator=1;return EFS_ERR_IO;}
 return inode_fail;
}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane%2?2:1;}
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;return !(unhosted && g==2);}
static void host_need_both(uint8_t need[2]){need[0]=0;need[1]=2;}
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *need,int n,int skip) {
 (void)h;(void)skip;assert(n==2 && need[0]==0 && need[1]==2);return 4;
}
int efs_meta_apply_get_inode(struct efs_kv*k,efs_ino_t i,struct efs_meta_row*r){(void)k;(void)i;*r=row;if(hint_empty)r->active_lanes=r->used_shards=0;return inode_fail;}
static int host_read_index_pair(struct efs_raft_host*h,uint8_t a,uint8_t b,int*hint){if(pair_fail)return pair_fail;int rc=host_read_index(h,a,hint);return rc?rc:host_read_index(h,b,hint);}
''' + function('host_read_inode_lanes_reuse') + function('host_read_inode_lanes_covered') + function('host_read_inode_lanes') + r'''
int main(void) {
 struct efs_raft_host h={0};int hint=-1;row.mode=S_IFREG;
 assert(!host_read_inode_lanes(&h,1,&hint));assert(calls[0]==1 && !calls[2]);
 memset(calls,0,sizeof(calls));row.active_lanes=3;
 /* Parent fence covers the child's own group; newly observed other lanes
  * still require authority, even if their activation followed that fence. */
 assert(!host_read_inode_lanes_covered(&h,1,&hint,1));assert(!calls[0] && calls[2]==1);
 memset(calls,0,sizeof(calls));
 assert(!host_read_inode_lanes_covered(&h,1,&hint,5));assert(!calls[0] && !calls[2]);
 /* The next independent request must establish both fresh group fences. */
 assert(!host_read_inode_lanes(&h,1,&hint));assert(calls[0]==1 && calls[2]==1);
 memset(calls,0,sizeof(calls));hint_empty=1;
 assert(!host_read_inode_lanes(&h,1,&hint) && calls[0]==1 && calls[2]==1);
 hint_empty=0;pair_fail=EFS_ERR_BUSY;memset(calls,0,sizeof(calls));
 assert(!host_read_inode_lanes(&h,1,&hint) && calls[0]==1 && calls[2]==1);
 pair_fail=EFS_ERR_NOT_PRIMARY;assert(!host_read_inode_lanes(&h,1,&hint));
 pair_fail=EFS_ERR_IO;assert(host_read_inode_lanes(&h,1,&hint)==EFS_ERR_IO);
 pair_fail=0;
 row.mode=S_IFDIR;row.layout=EFS_META_LAYOUT_HASHED;row.active_lanes=0;row.used_shards=3;
 memset(calls,0,sizeof(calls));
 assert(!host_read_inode_lanes_covered(&h,1,&hint,1));assert(!calls[0] && calls[2]==1);
 read_fail=EFS_ERR_IO;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_IO && hint==7);
 read_fail=EFS_ERR_BUSY;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_BUSY);
 read_fail=0;unhosted=1;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_NOT_PRIMARY && hint==4);
 unhosted=0;inode_fail=EFS_ERR_IO;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_IO);
 inode_fail=0;coordinator=1;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_NOT_PRIMARY);
 puts("operation ReadIndex: per-RPC fences, new lanes, directory shards and uncertainty PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-readindex-operation-') as directory:
    c = Path(directory)/'test.c'; c.write_text(code)
    binary = c.with_suffix('')
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
