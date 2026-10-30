#!/usr/bin/env python3
"""Execute MKDIR's actual apply-time reservation helper under hot allocations."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_MKDIR_ALLOC_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static int host_mkdir_alloc_reserve(');b=s.index('\n}',a)+2
code=r'''
#include "efs/txn.h"
#include "efs/meta_apply.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;};
struct host_idx_ref {int rc;};
static int submit_fail,wait_fail,submits,waits;
static void be64(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static int host_prep_raw(struct efs_raft_host *h,uint32_t sh,int kind,
 const struct efs_txid *t,const struct efs_txn_parts *parts,const uint8_t *key,
 uint32_t length,const uint8_t *pay,uint32_t bytes,struct host_idx_ref *ref,int *hint){
 (void)hint;assert(sh==7&&kind==EFS_TXN_ALLOC_INODE&&bytes==0);submits++;
 if(submit_fail)return submit_fail;
 /* Ordinary CREATEs queued before this proposal advance the watermark. */
 uint8_t value[8];be64(value,7+32*4096);
 assert(!efs_kv_put(h->kv,key,length,value,8));
 ref->rc=efs_txn_apply_prepare(h->kv,kind,t,parts,key,length,pay,bytes);
 return 0;
}
static int host_wait_refs(struct efs_raft_host *h,struct host_idx_ref *refs,int n,int *hint){
 (void)h;(void)hint;assert(n==1);waits++;return wait_fail?wait_fail:refs[0].rc;
}
'''+s[a:b]+r'''
int main(void){
 struct efs_raft_host h={efs_kv_mem_create()};assert(h.kv);
 struct efs_txid t={{1}};struct efs_txn_parts parts={.n=2,.shard={1,7}};
 efs_ino_t ino=0;int hint=-1;uint8_t key[EFS_KV_KEY_MAX];uint32_t kl=0;
 assert(!efs_kv_key_alloc(7,key,&kl));
 assert(!host_mkdir_alloc_reserve(&h,7,&t,&parts,&ino,&hint)&&ino==7+32*4096&&submits==1&&waits==1);
 assert(efs_txn_key_busy(h.kv,key,kl)==EFS_ERR_BUSY);
 assert(!efs_txn_drop(h.kv,&t,7));
 submit_fail=EFS_ERR_NOT_PRIMARY;ino=0;
 assert(host_mkdir_alloc_reserve(&h,7,&t,&parts,&ino,&hint)==EFS_ERR_NOT_PRIMARY&&!ino&&waits==1);
 submit_fail=0;wait_fail=EFS_ERR_BUSY;
 assert(host_mkdir_alloc_reserve(&h,7,&t,&parts,&ino,&hint)==EFS_ERR_BUSY&&!ino);
 assert(!efs_txn_drop(h.kv,&t,7));
 efs_kv_mem_free(h.kv);
 puts("MKDIR apply-time allocator: hot prefix, owned selection, failed submission and ambiguous apply wait PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mkdir-alloc-') as td:
 p=Path(td)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
# Handler transfers authority to COMMIT/finisher before an ambiguous result;
# early construction errors retain cleanup ownership.
a=s.index('void server_raft_host_mkdir(');body=s[a:s.index('\n/* Empty RMDIR:',a)]
assert body.index('host_mkdir_alloc_reserve(')<body.index('crow.ino = ino;')<body.index('stage = 9;')
assert body.index('alloc_reserved=0; /* decision owner/finisher')<body.index('rc = host_txn_commit(')
assert 'if (alloc_reserved && rc!=EFS_OK) (void)host_drop_parts(h,&t,&parts,&hint);' in body
