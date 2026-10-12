#!/usr/bin/env python3
"""Exercise actual source_hash with adversarial opaque KV row sizes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/meta/repair_job.c').read_text();a=s.index('static int source_hash(');fn=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/repair_job.h"
#include <assert.h>
#include <string.h>
static int calls;
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 1;}
int efs_txn_key_exclusive(struct efs_kv *kv,const uint8_t *k,uint32_t n){(void)kv;(void)k;(void)n;return 0;}
static void w32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
int efs_kv_key_chunk(uint32_t sh,efs_ino_t ino,uint64_t gen,uint8_t lane,uint32_t ci,uint8_t *k,uint32_t *n){(void)sh;(void)ino;(void)gen;(void)lane;(void)ci;memset(k,0,32);*n=32;return 0;}
int efs_kv_key_lane(uint32_t sh,efs_ino_t ino,uint64_t gen,uint8_t lane,uint8_t *k,uint32_t *n){(void)sh;(void)ino;(void)gen;(void)lane;memset(k,0,32);*n=32;return 0;}
static int get(void *ctx,const uint8_t *k,uint32_t kl,uint8_t *v,uint32_t *n){(void)ctx;(void)k;(void)kl;calls++;memset(v,1,*n);return 0;}
void efs_hash(const void *p,size_t n,uint8_t *o){(void)p;(void)n;memset(o,0,32);}
'''+fn+r'''
int main(void){struct efs_kv_ops ops={.get=get};struct efs_kv kv={.ops=&ops};struct efs_repair_job j={.ino=9,.inode_gen=1};uint8_t h[32];assert(source_hash(&kv,&j,h)==EFS_ERR_OVERFLOW);assert(calls==1);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'bounds.c';p.write_text(code);b=Path(d)/'bounds'
 subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined','-I'+str(root/'include'),str(p),'-o',str(b)],check=True)
 subprocess.run([b],check=True)
print('actual source_hash full-capacity row ASan/UBSan PASS')
