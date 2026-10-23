#!/usr/bin/env python3
"""K5 review finding S2, kept as a regression check (run by `make test`).
The production mtime ownership wrapper first accepts epoch 1, then captures
the session row; the stubbed KV read supplies a row fenced in between
(established=1, reject_below=2). The executor is a callback spy, not a live
transaction; tests/test_session_owned_capture.c runs the same interleaving
against real session rows.
Run: python3 scripts/reviews/k05_mtime_fence_capture.py
On c71cd1066 (defect): rc=0, executor=1, prepares=2.
Fixed: rc=EFS_ERR_FENCED, executor=0, prepares=0.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/meta/append_plan.c').read_text()
helper=s[s.index('static int session_owner_capture('):s.index('\nstruct append_owner_context {')]
body=helper+s[s.index('struct mtime_owner_context {'):s.index('\nstruct finish_image {')]
code=r'''
#include "efs/meta_apply.h"
#include "efs/session.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
static int prepared, executor;
int efs_session_accept(struct efs_kv *kv,uint32_t s,const uint8_t *u,uint32_t e){(void)kv;(void)s;(void)u;assert(e==1);return EFS_OK;}
int efs_kv_key_sess_local(uint32_t s,const uint8_t *u,uint8_t *out,uint32_t *n){(void)u;out[0]=s>>8;out[1]=s;*n=2;return EFS_OK;}
static int get(void *c,const uint8_t *k,uint32_t n,uint8_t *v,uint32_t *len){(void)c;(void)k;(void)n;assert(*len>=8);memset(v,0,8);v[3]=1;v[7]=2;*len=8;return EFS_OK;}
static int prepare(void *c,const struct efs_txid *t,const struct efs_txn_parts *p,const struct efs_meta_mtime_image *i){(void)c;(void)t;(void)p;if(!prepared){assert(i->expected_len==8);assert(i->expected[7]==2);assert(!memcmp(i->expected,i->value,8));}prepared++;return EFS_OK;}
static int decide(void *c,const struct efs_txid *t,const struct efs_txn_parts *p,int d){(void)c;(void)t;(void)p;(void)d;return EFS_OK;}
/* Only the executor is stubbed: inspect the ownership image that the
 * production wrapper hands to its first prepare callback. */
int efs_meta_execute_mtime(const struct efs_meta_mtime_plan *p,const struct efs_txid *t,const struct efs_meta_mtime_ops *o,void *c){executor++;return o->prepare(c,t,&p->parts,&p->images[0]);}
'''+body+r'''
int main(void){
 struct efs_kv_ops k={.get=get};struct efs_kv kv={.ops=&k};
 struct efs_meta_mtime_plan p={.count=1};struct efs_opid id={.session_epoch=1};struct efs_txid t={0};struct efs_meta_mtime_ops o={prepare,decide,decide};
 int rc=efs_meta_execute_mtime_owned(&kv,&p,&id,&t,&o,NULL);
 printf("mtime wrapper rc=%d executor=%d prepares=%d; captured reject_below=2 for epoch=1\n",rc,executor,prepared);
 assert(rc==EFS_ERR_FENCED&&executor==0&&prepared==0);
 puts("k05 mtime fence capture: fenced captured image is refused before any prepare PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'r.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-I'+str(root/'include'),str(p),'-o',d+'/r'],check=True)
 subprocess.run([d+'/r'],check=True)
