#!/usr/bin/env python3
"""Exercise the production container-only empty-lane retry proof."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_container_gc_lane_empty(');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#define EFS_OK 0
#define EFS_ERR_NOT_FOUND -1
#define EFS_ERR_BUSY -13
#define EFS_KV_KEY_MAX 64
#define EFS_META_SWEEP_BATCH_MAX 64
typedef uint64_t efs_ino_t;
struct server{int storage_engine;};
struct efs_raft_host{struct server*s;void*kv;};
static int hosted=1,exists,read_rc,scan_rc,key_rc,after_exists,gets,reads,scans;
static uint32_t chunks,after_chunks;
static uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane;}
static uint8_t efs_raft_shard_group(uint32_t shard){return (shard&1)?2:0;}
static int host_hosts(struct efs_raft_host*h,uint8_t group){(void)h;(void)group;return hosted;}
static int efs_kv_key_lane(uint32_t shard,efs_ino_t ino,uint64_t gen,uint8_t lane,uint8_t*k,uint32_t*n){(void)shard;(void)ino;(void)gen;(void)lane;*k=1;*n=1;return key_rc;}
static int efs_kv_get(void*kv,const uint8_t*k,uint32_t n,uint8_t*v,uint32_t*vl){(void)kv;(void)k;(void)n;(void)v;(void)vl;gets++;return exists?EFS_OK:EFS_ERR_NOT_FOUND;}
static int host_read_index(struct efs_raft_host*h,uint8_t group,int*hint){(void)h;(void)group;(void)hint;reads++;exists=after_exists;chunks=after_chunks;return read_rc;}
static int efs_meta_lane_sweep_select(void*kv,efs_ino_t ino,uint64_t gen,uint8_t lane,uint32_t*i,uint32_t*n){(void)kv;(void)ino;(void)gen;(void)lane;(void)i;scans++;*n=chunks;return scan_rc;}
'''+s[a:b]+r'''
int main(void){
 struct server s={0};struct efs_raft_host h={&s,0};uint8_t covered=0;
 assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));assert(!reads&&!gets&&!scans);
 s.storage_engine=1;hosted=0;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));assert(!reads);hosted=1;
 exists=1;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));assert(!reads&&!scans);exists=0;
 read_rc=EFS_ERR_BUSY;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));assert(!covered&&!scans);read_rc=0;
 after_exists=1;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));assert(covered==1&&!scans);
 covered=0;exists=after_exists=0;after_chunks=1;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));assert(covered==1&&scans==1);
 covered=0;after_chunks=0;scan_rc=EFS_ERR_BUSY;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));scan_rc=0;
 covered=0;assert(host_container_gc_lane_empty(&h,42,9,0,&covered));assert(covered==1);int before=reads;
 assert(host_container_gc_lane_empty(&h,42,9,2,&covered));assert(reads==before);
 assert(host_container_gc_lane_empty(&h,42,9,1,&covered));assert(covered==5&&reads==before+1);
 covered=0;assert(host_container_gc_lane_empty(&h,42,9,0,&covered));assert(reads==before+2); /* next attempt must be fresh */
 key_rc=EFS_ERR_BUSY;assert(!host_container_gc_lane_empty(&h,42,9,0,&covered));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-container-empty-lane-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('container empty-lane: fresh proofs, post-proof absence, retained chunks, errors and request-local reuse PASS')
