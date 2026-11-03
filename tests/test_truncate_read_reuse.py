#!/usr/bin/env python3
"""Run the production truncate handler; fences stay inside one RPC."""
from pathlib import Path
import os, subprocess, tempfile
import csrc
root = Path(__file__).resolve().parents[1]
s = csrc.module('raft_host','EFS_TRUNCATE_HOST_SOURCE')
a = s.index('static void host_truncate_lazy(')
b = s.index('\nvoid server_raft_host_setattr(', a)
code = r'''
#include "efs/meta_apply.h"
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define EFS_RAFT_GROUP_SHARD 0
#define EFS_RAFT_GROUP_SHARD2 2
#define HOST_TRUNC_LAZY_LEN 86u
#define HOST_TRUNC_LAZY_TRIES 16
struct efs_raft_host {int running;struct efs_kv *kv;};
static struct efs_raft_host h={.running=1};
static struct efs_raft_host *g_host=&h;
static unsigned reads[3],collects,proposals,verdicts,forwarded;
static int fail_group=-1,fail_verdict,post_error,hosted=1;
static uint64_t read_epoch;
static uint8_t efs_raft_shard_group(uint32_t shard){return shard&1?0:2;}
static int host_hosts(struct efs_raft_host *p,uint8_t g){(void)p;(void)g;return hosted;}
static int host_txn_coord(void *ctx,const struct efs_txid *id,uint32_t sh,int *d){(void)ctx;(void)id;(void)sh;(void)d;return EFS_OK;}
static int host_read_index(struct efs_raft_host *p,uint8_t g,int *hint){(void)p;*hint=g+1;reads[g]++;return g==fail_group?EFS_ERR_BUSY:EFS_OK;}
int efs_meta_apply_get_inode_tx(struct efs_kv *kv,efs_ino_t ino,efs_txn_coord_fn co,void *ctx,struct efs_meta_row *row){(void)kv;(void)ino;(void)co;(void)ctx;memset(row,0,sizeof(*row));row->mode=S_IFREG|0644;row->generation=1;row->content_epoch=read_epoch;row->base_size=100;return EFS_OK;}
static int host_read_inode_lanes_reuse(struct efs_raft_host *p,efs_ino_t ino,int *hint,uint8_t *covered){(void)ino;for(unsigned g=0;g<=2;g+=2)if(!(*covered&(1u<<g))){int rc=host_read_index(p,g,hint);if(rc)return rc;*covered|=1u<<g;}return EFS_OK;}
static int __attribute__((unused)) host_read_inode_lanes(struct efs_raft_host *p,efs_ino_t ino,int *hint){uint8_t covered=0;return host_read_inode_lanes_reuse(p,ino,hint,&covered);}
int efs_meta_apply_getattr(struct efs_kv *kv,efs_ino_t ino,efs_txn_coord_fn co,void *ctx,struct efs_meta_stat *st){(void)kv;(void)co;(void)ctx;collects++;if(post_error&&proposals)return EFS_ERR_IO;memset(st,0,sizeof(*st));st->ino=ino;st->size=proposals?10:100;return EFS_OK;}
static void host_fwd_truncate_opid(struct efs_raft_host *p,efs_ino_t ino,uint64_t sz,uint64_t mt,uint32_t ns,const struct efs_opid_req *q,struct efs_msg_inode_reply *out){(void)p;(void)ino;(void)sz;(void)mt;(void)ns;(void)q;forwarded++;out->status=EFS_INODE_RPC_BUSY;}
static uint64_t now_ns(void){return 1;}
static void wr64be(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=v;v>>=8;}}
static int host_propose(struct efs_raft_host *p,uint8_t g,const uint8_t *cmd,uint32_t n,uint64_t *idx,uint64_t *term,int *hint){(void)p;(void)g;(void)hint;assert(n==86&&cmd[0]==EFS_MD_CMD_TRUNCATE);proposals++;*idx=proposals;*term=1;return EFS_OK;}
static int host_wait_verdict(struct efs_raft_host *p,uint8_t g,uint64_t idx,uint64_t term,int *hint){(void)p;(void)g;(void)idx;(void)term;(void)hint;verdicts++;if(fail_verdict&&verdicts==1)return EFS_ERR_STALE;read_epoch++;return EFS_OK;}
static void set_inode_rc(struct efs_msg_inode_reply *out,int rc,int hint){(void)hint;out->status=rc==EFS_OK?EFS_INODE_RPC_OK:EFS_INODE_RPC_BUSY;}
static void stat_to_inode(const struct efs_meta_stat *st,struct efs_inode *out){out->ino=st->ino;out->size=st->size;}
static void reply_note_epoch(struct efs_raft_host *p,efs_ino_t ino,efs_txn_coord_fn co,void *ctx,struct efs_inode *out){(void)p;(void)ino;(void)co;(void)ctx;out->content_epoch=read_epoch;}
''' + s[a:b] + r'''
static void reset(void){reads[0]=reads[2]=collects=proposals=verdicts=forwarded=0;fail_group=-1;fail_verdict=post_error=0;read_epoch=1;hosted=1;}
int main(void){
 struct efs_opid_req q={.id={.client_uuid={1},.session_epoch=1,.seq=1}};
 struct efs_msg_inode_reply out;
 reset();host_truncate_lazy(1,10,0,0,&q,&out);
 assert(out.status==EFS_INODE_RPC_OK&&out.inode.size==10&&out.inode.content_epoch==2);
 assert(reads[0]==1&&reads[2]==1&&collects==2&&proposals==1);
 /* A second RPC must obtain both fences again. */
 host_truncate_lazy(1,10,0,0,&q,&out);assert(reads[0]==2&&reads[2]==2);
 reset();fail_verdict=1;host_truncate_lazy(1,10,0,0,&q,&out);
 assert(out.status==EFS_INODE_RPC_OK&&proposals==2&&collects==3);
 assert(reads[0]==1&&reads[2]==1); /* rereads/compare-and-apply stay */
 for(int g=0;g<=2;g+=2){reset();fail_group=g;host_truncate_lazy(1,10,0,0,&q,&out);assert(out.status!=EFS_INODE_RPC_OK&&!proposals);fail_group=-1;host_truncate_lazy(1,10,0,0,&q,&out);assert(out.status==EFS_INODE_RPC_OK&&reads[g]==2);}
 reset();post_error=1;host_truncate_lazy(1,10,0,0,&q,&out);assert(proposals==1&&out.status!=EFS_INODE_RPC_OK);
 reset();hosted=0;host_truncate_lazy(1,10,0,0,&q,&out);assert(forwarded==1&&!reads[0]&&!proposals);
 reset();q.ack=q.id.seq;host_truncate_lazy(1,10,0,0,&q,&out);assert(out.status==EFS_INODE_RPC_INVAL&&!proposals);
 puts("truncate read reuse: per-RPC fences, fresh collects/CAS retries, refusal, forwarding and no post-commit error waiver PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-truncate-read-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/sim/opid.c'),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
