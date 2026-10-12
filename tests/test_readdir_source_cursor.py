#!/usr/bin/env python3
"""Execute the production server callback across empty-name source boundaries."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text();a=s.index('void server_raft_host_readdir(');b=s.index('\n}',a)+2
code=r'''
#include <efs/protocol.h>
#include <efs/meta_apply.h>
#include <efs/raft.h>
#include <efs/kv_key.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
struct efs_raft_host {int running;struct efs_kv *kv;};
static int partial,pending,fwd_calls,reads,lanes,read_fail;
static void host_need_both(uint8_t groups[2]){groups[0]=EFS_RAFT_GROUP_SHARD;groups[1]=EFS_RAFT_GROUP_SHARD2;}
static struct efs_raft_host host={1,NULL};static struct efs_raft_host *g_host=&host;
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane;}
static int host_hosts(struct efs_raft_host*h,uint8_t group){(void)h;return !partial||group==EFS_RAFT_GROUP_SHARD2;}
static int host_read_index(struct efs_raft_host*h,uint8_t group,int*hint){(void)h;(void)group;(void)hint;reads++;return read_fail?EFS_ERR_BUSY:EFS_OK;}
static int host_read_inode_lanes_covered(struct efs_raft_host*h,efs_ino_t ino,int*hint,uint8_t covered){(void)h;(void)hint;assert(covered==(1u<<efs_raft_shard_group(efs_kv_inode_shard(ino))));lanes++;return pending?EFS_ERR_NOT_PRIMARY:EFS_OK;}
static uint8_t rc_to_inode_status(int rc){return rc==EFS_OK?EFS_INODE_RPC_OK:rc==EFS_ERR_NOT_PRIMARY?EFS_INODE_RPC_NOT_PRIMARY:EFS_INODE_RPC_ERROR;}
static void host_fwd_readdir(struct efs_raft_host*h,efs_ino_t ino,uint32_t max,uint32_t src,const char*name,struct efs_msg_inode_readdir_reply*out,const uint8_t*groups,int n){(void)h;assert(partial&&ino==42&&max==17&&src==1&&!name[0]&&n==2&&groups[0]==EFS_RAFT_GROUP_SHARD&&groups[1]==EFS_RAFT_GROUP_SHARD2);fwd_calls++;memset(out,0,sizeof(*out));out->status=EFS_INODE_RPC_OK;out->count=1;out->ents[0].ino=777;}
int efs_meta_apply_get_inode(struct efs_kv*kv,efs_ino_t ino,struct efs_meta_row*out){(void)kv;(void)ino;memset(out,0,sizeof(*out));out->mode=S_IFDIR|0755;out->layout=partial?EFS_META_LAYOUT_LOCAL:EFS_META_LAYOUT_HASHED;out->used_shards=3;return EFS_OK;}
int efs_meta_apply_readdir(struct efs_kv*kv,efs_ino_t ino,struct efs_meta_dir_cursor*cur,struct efs_meta_dir_ent*out,uint32_t max,uint32_t*n){
 (void)kv;(void)ino;*n=0;if(cur->src>=3){cur->done=1;return EFS_OK;}
 unsigned index=0,src=cur->src;
 if(cur->name[0]){unsigned last;assert(sscanf(cur->name,"source%u-%u",&src,&last)==2&&src==cur->src);index=last+1;}
 unsigned length=src==2?1:64;
 while(index<length&&*n<max){struct efs_meta_dir_ent*e=&out[(*n)++];memset(e,0,sizeof(*e));snprintf(e->name,sizeof(e->name),"source%u-%03u",src,index);e->d.ino=src*64+index+1;e->d.type=S_IFDIR|0755;strcpy(cur->name,e->name);index++;}
 if(index==length){cur->src++;cur->name[0]=0;if(cur->src==3)cur->done=1;}
 return EFS_OK;
}
'''+s[a:b]+r'''
static void full_scan(unsigned max){struct efs_msg_inode_readdir_reply r;uint32_t src=0;char name[EFS_MAX_NAME]="";unsigned count=0,pages=0;
 do{server_raft_host_readdir(42,max,src,name,&r);assert(!r.status&&r.count<=max);for(unsigned i=0;i<r.count;i++)assert(r.ents[i].ino==++count);src=r.next_src;strcpy(name,r.next_name);assert(++pages<200);}while(!r.next_done);
 assert(count==129&&src==3);
}
int main(void){struct efs_msg_inode_readdir_reply r;server_raft_host_readdir(42,64,1,"",&r);assert(!r.status&&r.count==64&&r.ents[0].ino==65&&r.next_src==2&&!r.next_name[0]);full_scan(64);full_scan(17);full_scan(1);assert(reads==lanes);int old_reads=reads,old_lanes=lanes;read_fail=1;server_raft_host_readdir(42,17,1,"",&r);assert(r.status!=EFS_INODE_RPC_OK&&reads==old_reads+1&&lanes==old_lanes);read_fail=0;partial=pending=1;server_raft_host_readdir(42,17,1,"",&r);assert(!r.status&&r.count==1&&r.ents[0].ino==777&&fwd_calls==1);partial=0;server_raft_host_readdir(42,17,1,"",&r);assert(r.status==EFS_INODE_RPC_NOT_PRIMARY&&fwd_calls==1);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-source-') as d:
 p=Path(d)/'t.c';p.write_text(code);subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True);subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('Server READDIR: source-only empty-name cursors survive exact/full/partial page boundaries;129 unique entries and EOF PASS')
