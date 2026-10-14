#!/usr/bin/env python3
"""A batched path fences each encountered group once within that RPC."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('void server_raft_host_lookup_path(');handler=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/meta_apply.h"
#include "efs/protocol.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_raft_host {int running;struct efs_kv *kv;};
static struct efs_raft_host h={1,NULL},*g_host=&h;
static int calls[3],fail,unhosted,forwarded,missing,route=-1;
int host_read_leader_hint(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return route;}
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;return !(unhosted && g==2);}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){
 (void)h;calls[g]++;*hint=2;return g==2?fail:0;
}
static int host_read_inode_lanes_reuse(struct efs_raft_host *h,efs_ino_t ino,int *hint,uint8_t *covered){
 uint8_t g=efs_raft_shard_group(efs_kv_inode_shard(ino));
 if(!(*covered&(1u<<g))){int rc=host_read_index(h,g,hint);if(rc)return rc;*covered|=1u<<g;}
 return 0;
}
static void host_need_both(uint8_t need[2]){need[0]=0;need[1]=2;}
static void host_fwd_lookup_path(struct efs_raft_host *h,efs_ino_t start,const char *path,
 struct efs_msg_inode_lookup_path_reply *out,const uint8_t *g,int n){
 (void)h;(void)start;(void)path;(void)g;(void)n;forwarded++;out->status=EFS_INODE_RPC_NOT_PRIMARY;
}
static int host_txn_coord(void *h,const struct efs_txid *t,uint32_t s,int *d){(void)h;(void)t;(void)s;(void)d;return 0;}
uint32_t efs_kv_dentry_shard(efs_ino_t parent,const char *name,uint8_t layout){(void)name;(void)layout;return (parent+1)&EFS_KV_SHARD_MASK;}
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *row){
 (void)kv;assert(calls[efs_raft_shard_group(efs_kv_inode_shard(ino))]>0);
 memset(row,0,sizeof(*row));row->ino=ino;row->mode=S_IFDIR|0755;row->uid=22;row->gid=33;return 0;
}
int efs_meta_apply_lookup_tx(struct efs_kv *kv,efs_ino_t ino,const char *name,efs_txn_coord_fn f,void *u,struct efs_meta_dentry *dent){
 (void)kv;(void)name;(void)f;(void)u;
 assert(calls[efs_raft_shard_group(efs_kv_dentry_shard(ino,name,0))]>0);
 dent->ino=ino+1;return missing?EFS_ERR_NOT_FOUND:0;
}
int efs_meta_apply_getattr(struct efs_kv *kv,efs_ino_t ino,efs_txn_coord_fn f,void *u,struct efs_meta_stat *st){
 (void)kv;(void)f;(void)u;memset(st,0,sizeof(*st));st->ino=ino;return 0;
}
static void set_inode_rc(struct efs_msg_inode_reply *out,int rc,int hint){(void)hint;
 out->status=rc==EFS_ERR_NOT_PRIMARY?EFS_INODE_RPC_NOT_PRIMARY:rc==0?EFS_INODE_RPC_OK:rc==EFS_ERR_NOT_FOUND?EFS_INODE_RPC_NOT_FOUND:EFS_INODE_RPC_ERROR;
}
static void stat_to_inode(const struct efs_meta_stat *st,struct efs_inode *ino){ino->ino=st->ino;}
'''+handler+r'''
int main(void){
 struct efs_msg_inode_lookup_path_reply out;char path[160]={0};
 route=2;server_raft_host_lookup_path(1,"d",&out);
 assert(out.status==EFS_INODE_RPC_NOT_PRIMARY && !calls[0] && !calls[2] && !forwarded);route=-1;
 for(int i=0;i<64;i++)strcat(path,"/d");
 server_raft_host_lookup_path(1,path,&out);
 assert(out.status==EFS_INODE_RPC_OK && out.inode.ino==65 && out.ancestor_count==63);
 assert(calls[0]==1 && calls[2]==1);
 for(int i=0;i<63;i++)assert(out.ancestors[i].ino==2u+i && out.ancestors[i].uid==22 && out.ancestors[i].gid==33);
 memset(calls,0,sizeof(calls));server_raft_host_lookup_path(1,path,&out);
 assert(out.status==EFS_INODE_RPC_OK && calls[0]==1 && calls[2]==1);
 strcat(path,"/d");memset(calls,0,sizeof(calls));server_raft_host_lookup_path(1,path,&out);
 assert(out.status==EFS_INODE_RPC_ERROR); /* Existing bounded path protocol. */
 fail=EFS_ERR_IO;memset(calls,0,sizeof(calls));server_raft_host_lookup_path(1,"d",&out);
 assert(out.status==EFS_INODE_RPC_ERROR && calls[0]==1 && calls[2]==1 && !forwarded);
 fail=0;unhosted=1;memset(calls,0,sizeof(calls));server_raft_host_lookup_path(1,"d",&out);
 assert(out.status==EFS_INODE_RPC_NOT_PRIMARY && forwarded==1 && calls[0]==1 && !calls[2]);
 unhosted=0;missing=1;memset(calls,0,sizeof(calls));server_raft_host_lookup_path(1,"d",&out);
 assert(out.status==EFS_INODE_RPC_NOT_FOUND);
 puts("path authority: 64 ancestors/two fresh fences, next RPC, routing, IO and bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-path-authority-') as d:
    c=Path(d)/'test.c';c.write_text(code);binary=c.with_suffix('')
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    *shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-I'+str(root/'include'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
