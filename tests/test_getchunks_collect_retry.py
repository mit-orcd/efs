#!/usr/bin/env python3
"""A GETCHUNKS retry must not inherit fields from the previous collect.

Run the production collector twice over one reused output, as its enclosing
retry loop does. A fold removes spans; a mapping can also move into a slot
previously used for a packed record. The client's strict validator stays on.
"""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
import csrc
root = Path(__file__).resolve().parents[1]
s = Path(sys.argv[1]).read_text() if len(sys.argv)>1 else csrc.module('raft_host')
a=s.index('struct host_chunk_stamp {');b=s.index('\n};',a)+3
stamp=s[a:b]
a=s.index('static int host_getchunks_collect(');b=s.index('\n}',a)+2
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/packed_segment.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;uint64_t obs_stage_busy;};
static struct efs_meta_chunk_view supplied;
static uint64_t now_ns(void){return 1;}
static int host_hosts(struct efs_raft_host*h,uint8_t group){(void)h;(void)group;return 1;}
static int host_read_index(struct efs_raft_host*h,uint8_t group,int*hint){(void)h;(void)group;(void)hint;return EFS_OK;}
#define host_txn_coord NULL
int efs_chunk_size_valid(uint32_t cs){return cs==EFS_DEFAULT_CHUNK_SIZE;}
int efs_meta_get_chunk_view_auth(struct efs_kv*kv,efs_ino_t ino,uint64_t gen,uint32_t ci,
 uint32_t cs,uint64_t epoch,const struct efs_fence_history*hist,struct efs_meta_chunk_view*out){
 (void)kv;(void)epoch;(void)hist;assert(ino==42&&gen==7&&ci==0&&cs==EFS_DEFAULT_CHUNK_SIZE);*out=supplied;return EFS_OK;}
int efs_meta_pub_stage_visible(struct efs_kv*kv,efs_ino_t ino,uint64_t gen,uint32_t ci,
 efs_txn_coord_fn coord,void*ctx,struct efs_txn_pending*pend){
 (void)kv;(void)ino;(void)gen;(void)ci;(void)coord;(void)ctx;(void)pend;return EFS_OK;}
int efs_pack_ref_encode(const struct efs_pack_ref*r,uint8_t out[EFS_PACK_REF_BYTES]){
 memset(out,0,EFS_PACK_REF_BYTES);efs_store_be64(out,r->ino);efs_store_be64(out+24,r->record_generation);return EFS_OK;}
'''+stamp+'\n'+s[a:b]+r'''
static void view(int packed,unsigned deltas,uint64_t object){
 memset(&supplied,0,sizeof(supplied));supplied.base.generation=object;
 supplied.base.coding_profile_id=packed?EFS_META_PROFILE_PACK:0;
 supplied.base.packed.ino=42;supplied.base.packed.record_generation=object;
 supplied.bytes.chunk_size=EFS_DEFAULT_CHUNK_SIZE;supplied.bytes.count=deltas+1;
 supplied.bytes.parts[0].len=EFS_DEFAULT_CHUNK_SIZE;supplied.ndelta=deltas;
 supplied.delta_seq=deltas?55:0;
 if(deltas){supplied.deltas[0].off=1;supplied.deltas[0].len=2;supplied.deltas[0].generation=999;
  supplied.bytes.parts[1].off=1;supplied.bytes.parts[1].len=2;}
}
static void collect(struct efs_msg_inode_getchunks_reply*out){
 struct efs_raft_host h={0};struct efs_meta_row row={.generation=7,.active_lanes=1};
 struct efs_fence_history hist={0};struct efs_txn_pending pend={0};
 struct host_chunk_stamp stamps[EFS_GETCHUNKS_MAX];uint32_t ns=0,seen=0;uint64_t ri=0;int hint=-1,fwd=0;
 out->count=0; /* production retry loop reuses the remaining output bytes */
 assert(host_getchunks_collect(&h,42,0,1,1,EFS_DEFAULT_CHUNK_SIZE,&row,&hist,
   out,stamps,&ns,&seen,&pend,&ri,&hint,&fwd)==EFS_OK);
 assert(!fwd&&out->count==1&&ns==1);
}
int main(void){
 struct efs_msg_inode_getchunks_reply out={.ino=42,.generation=7};
 struct efs_msg_inode_getchunks req={.ino=42,.generation=7,.max=1};
 view(0,1,101);collect(&out);assert(efs_getchunks_reply_valid(&req,&out)==EFS_OK);
 view(0,0,202);collect(&out);
 assert(efs_getchunks_reply_valid(&req,&out)==EFS_OK); /* old code: PROTO */
 assert(!out.recs[0].delta_base_n&&!out.recs[0].delta_base_seq);
 memset(&out,0,sizeof(out));out.ino=42;out.generation=7;
 view(1,0,303);collect(&out);assert(efs_getchunks_reply_valid(&req,&out)==EFS_OK);
 view(0,0,404);collect(&out);
 assert(efs_getchunks_reply_valid(&req,&out)==EFS_OK); /* old packed flag/ref */
 uint8_t zero[EFS_PACK_REF_BYTES]={0};
 assert(!out.recs[0].publish_flags&&!memcmp(out.recs[0].packed_ref,zero,sizeof(zero)));
 puts("GETCHUNKS collect retry: folded deltas and packed-slot fields do not leak; strict validation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-getchunks-collect-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',
  '-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
