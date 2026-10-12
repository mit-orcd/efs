#!/usr/bin/env python3
"""A LOOKUP reuses only the same request's already-read parent group."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_read_inode_lanes_reuse(');b=s.index('\n/* Stable per-group order',a)
code=r'''
#include <stdint.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_IO -1
#define EFS_ERR_NOT_PRIMARY -6
#define EFS_META_LAYOUT_LOCAL 0
#define EFS_META_LANES 64
typedef uint64_t efs_ino_t;
struct efs_meta_row {unsigned mode,layout;uint64_t used_shards,active_lanes;};
struct efs_raft_host {void *kv;};
struct host_view {int has,leader;};
static inline void host_view_get(struct efs_raft_host *h,uint8_t group,struct host_view *v){(void)h;v->has=1;v->leader=group+1;}

static struct efs_meta_row row;
static unsigned reads[3],inode_reads,unhosted;
static int read_error,inode_error,coordinator_unhosted,hint_error,pair_error;
static struct efs_meta_row scheduling_hint;
static uint32_t efs_kv_inode_shard(efs_ino_t ino){return (uint32_t)ino;}
static uint8_t efs_raft_shard_group(uint32_t shard){return (shard&1)?0:2;}
static uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane+1;}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint){(void)h;reads[group]++;*hint=group+1;return read_error;}
static int efs_meta_apply_get_inode(void *kv,efs_ino_t ino,struct efs_meta_row *out){(void)kv;(void)ino;*out=scheduling_hint;return hint_error;}
static int host_read_index_pair(struct efs_raft_host *h,uint8_t first,uint8_t second,int *hint){if(pair_error)return pair_error;int rc=host_read_index(h,first,hint);return rc?rc:host_read_index(h,second,hint);}
struct host_txn_read_context {struct efs_raft_host *host;int unhosted_coordinator;};
static int host_txn_read_coord;
static int efs_meta_apply_get_inode_tx(void *kv,efs_ino_t ino,int coord,void *h,struct efs_meta_row *out){(void)kv;(void)ino;(void)coord;((struct host_txn_read_context *)h)->unhosted_coordinator=coordinator_unhosted;inode_reads++;*out=row;return inode_error;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;return !(unhosted&(1u<<group));}
static void host_need_both(uint8_t need[2]){need[0]=0;need[1]=2;}
static int host_pick_peer(struct efs_raft_host *h,uint8_t *need,int n,int skip){(void)h;assert(need[0]==0&&need[1]==2&&n==2&&skip==-1);return 4;}
''' + s[a:b] + r'''
static void reset(void){reads[0]=reads[2]=inode_reads=unhosted=0;read_error=inode_error=coordinator_unhosted=hint_error=pair_error=0;row=(struct efs_meta_row){.mode=S_IFDIR,.layout=1,.used_shards=15};scheduling_hint=row;}
int main(void){struct efs_raft_host h={0};int hint;struct host_view view;host_view_get(&h,0,&view);assert(view.leader==1);
 reset();hint=1;assert(!host_read_inode_lanes_covered(&h,1,&hint,1));assert(!reads[0]&&reads[2]==1&&inode_reads==1);
 reset();hint=1;row.used_shards=1;assert(!host_read_inode_lanes_covered(&h,1,&hint,1));assert(!reads[0]&&!reads[2]&&inode_reads==1&&hint==1);
 reset();assert(!host_read_inode_lanes_covered(&h,2,&hint,1));assert(reads[2]==1&&!reads[0]&&inode_reads==1);
 reset();assert(!host_read_inode_lanes(&h,1,&hint));assert(reads[0]==1&&reads[2]==1);
 /* The paired read leaves group2's leader; successful reply describes group0. */
 assert(hint==1);
 reset();hint=3;assert(!host_read_inode_lanes_covered(&h,1,&hint,5));assert(!reads[0]&&!reads[2]&&hint==1);
 reset();hint=1;assert(!host_read_inode_lanes(&h,2,&hint));assert(hint==3);
 reset();read_error=EFS_ERR_BUSY;hint=-1;assert(host_read_inode_lanes(&h,1,&hint)==EFS_ERR_BUSY);assert(hint==1);

 reset();inode_error=EFS_ERR_BUSY;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_BUSY);assert(inode_reads==1&&!reads[0]&&!reads[2]);
 reset();inode_error=EFS_ERR_IO;coordinator_unhosted=1;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_NOT_PRIMARY);assert(inode_reads==1&&!reads[0]&&!reads[2]);
 reset();read_error=EFS_ERR_IO;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_IO);assert(reads[2]==1);
 reset();unhosted=1u<<2;assert(host_read_inode_lanes_covered(&h,1,&hint,1)==EFS_ERR_NOT_PRIMARY);assert(hint==4&&!reads[2]);
 reset();row.mode=S_IFREG;row.active_lanes=2;assert(!host_read_inode_lanes_covered(&h,1,&hint,1));assert(reads[2]==1);
 reset();scheduling_hint.used_shards=1;assert(!host_read_inode_lanes(&h,1,&hint));assert(reads[0]==1&&reads[2]==1&&inode_reads==1);
 reset();hint_error=EFS_ERR_IO;assert(!host_read_inode_lanes(&h,1,&hint));assert(reads[0]==1&&reads[2]==1&&inode_reads==1);
 reset();pair_error=EFS_ERR_BUSY;assert(!host_read_inode_lanes(&h,1,&hint));assert(reads[0]==1&&reads[2]==1&&inode_reads==1);
 reset();pair_error=EFS_ERR_IO;assert(host_read_inode_lanes(&h,1,&hint)==EFS_ERR_IO);assert(!inode_reads);
 puts("LOOKUP fresh quorum reuse: same-request same-group only; pending transactions, other groups, absent hosts and hard errors preserved PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lookup-read-group-') as td:
 p=Path(td)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_DEFAULT_SOURCE','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
