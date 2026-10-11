#!/usr/bin/env python3
"""Unfenced hint bytes schedule groups only; missing/corrupt hints are benign."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static uint8_t host_lookup_plan_group(');b=s.index('\n}',a)+2
code=r'''
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#define efs_kv_get plan_kv_get
struct efs_raft_host {struct efs_kv *kv;};
static int parent_error,key_error,kv_error,decode_error;static uint8_t layout;
static uint32_t dsh;static efs_ino_t child;static unsigned key_calls,get_calls;
int efs_meta_apply_get_inode(struct efs_kv*k,efs_ino_t ino,struct efs_meta_row*r){(void)k;(void)ino;r->layout=layout;return parent_error;}
uint32_t efs_kv_dentry_shard(efs_ino_t ino,const char*n,uint8_t shape){(void)ino;(void)n;(void)shape;return dsh;}
int efs_kv_key_dentry(uint32_t shard,efs_ino_t ino,const char*n,uint8_t *key,uint32_t *length){(void)shard;(void)ino;(void)n;(void)key;*length=5;key_calls++;return key_error;}
int efs_kv_get(struct efs_kv*k,const uint8_t*p,uint32_t len,uint8_t*v,uint32_t*vl){(void)k;(void)p;(void)v;(void)vl;assert(len==5);get_calls++;return kv_error;}
int efs_meta_unpack_dentry(const uint8_t*p,uint32_t n,struct efs_meta_dentry*d){(void)p;(void)n;d->ino=child;return decode_error;}
'''+s[a:b]+r'''
int main(void){struct efs_raft_host h={0};layout=EFS_META_LAYOUT_LOCAL;dsh=1;child=2;
 assert(host_lookup_plan_group(&h,1,"name",0)==2 && key_calls==1 && get_calls==1);
 child=1;assert(host_lookup_plan_group(&h,1,"name",0)==0);
 child=0;assert(host_lookup_plan_group(&h,1,"name",0)==0);
 child=2;decode_error=EFS_ERR_PROTO;assert(host_lookup_plan_group(&h,1,"name",0)==0);decode_error=0;
 kv_error=EFS_ERR_IO;assert(host_lookup_plan_group(&h,1,"name",0)==0);kv_error=0;
 key_error=EFS_ERR_INVAL;assert(host_lookup_plan_group(&h,1,"name",0)==0);key_error=0;
 parent_error=EFS_ERR_BUSY;assert(host_lookup_plan_group(&h,1,"name",0)==0);parent_error=0;
 layout=255;assert(host_lookup_plan_group(&h,1,"name",0)==0);
 layout=EFS_META_LAYOUT_HASHED;dsh=2;unsigned before=key_calls;
 assert(host_lookup_plan_group(&h,1,"name",0)==2 && key_calls==before);
 puts("lookup planning: stale/unlinked/corrupt hints yield only scheduling groups; no authority or reply bytes PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lookup-plan-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
