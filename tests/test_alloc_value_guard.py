#!/usr/bin/env python3
"""Execute the host allocator guard with equal values/different sidecars and races."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()
def function(prefix):
    start = source.index(prefix)
    return source[start:source.index('\n}', start) + 2]
code = r'''
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include <assert.h>
#include <string.h>
struct efs_raft_host { struct efs_kv *kv; };
struct host_idx_ref { int unused; };
static int host_prep_raw(struct efs_raft_host *h,uint32_t shard,int kind,
 const struct efs_txid *t,const struct efs_txn_parts *parts,const uint8_t *key,
 uint32_t length,const uint8_t *payload,uint32_t bytes,struct host_idx_ref *ref,int *hint)
{
 (void)ref;(void)hint;assert(shard==7&&kind==EFS_TXN_EXCL_VALUE);
 return efs_txn_apply_prepare(h->kv,kind,t,parts,key,length,payload,bytes);
}
''' + function('static int host_alloc_capture(') + function('static int host_prep_alloc_value(') + r'''
static void be64(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
int main(void)
{
 struct efs_raft_host proposer={efs_kv_mem_create()},participant={efs_kv_mem_create()},empty={efs_kv_mem_create()};
 assert(proposer.kv&&participant.kv&&empty.kv);
 uint8_t key[EFS_KV_KEY_MAX],vk[EFS_KV_KEY_MAX],old[8],next[8],before[8],version[8],got[8];
 uint32_t kl=0,vl=0,bytes=0,n=8;be64(old,1000);be64(next,5096);
 assert(!efs_kv_key_alloc(7,key,&kl)&&!efs_kv_key_ver(key,kl,vk,&vl));
 assert(!efs_kv_put(proposer.kv,key,kl,old,8)&&!efs_kv_put(participant.kv,key,kl,old,8));
 be64(version,19);assert(!efs_kv_put(proposer.kv,vk,vl,version,8));
 be64(version,15);assert(!efs_kv_put(participant.kv,vk,vl,version,8));
 struct efs_txid t={{1}};struct efs_txn_parts parts={.n=1,.shard={7}};
 /* The former cross-node version predicate rejects equal watermarks. */
 assert(efs_txn_prepare_excl(participant.kv,&t,&parts,key,kl,19,EFS_TXN_PUT,next,8)==EFS_ERR_STALE);
 assert(!host_alloc_capture(&proposer,7,key,&kl,before,&bytes)&&bytes==8&&!memcmp(before,old,8));
 assert(!host_prep_alloc_value(&participant,7,&t,&parts,key,kl,before,bytes,next,NULL,NULL));
 assert(!efs_txn_resolve(participant.kv,&t,7,EFS_TXN_COMMIT));
 assert(!efs_kv_get(participant.kv,key,kl,got,&n)&&n==8&&!memcmp(got,next,8));
 /* A real concurrent allocation between capture and PREPARE must refuse
  * the old predicate; neither version drift nor a fresh local counter may
  * authorize rewinding an advanced watermark. */
 t.bytes[0]=2;
 assert(host_prep_alloc_value(&participant,7,&t,&parts,key,kl,before,bytes,next,NULL,NULL)==EFS_ERR_STALE);
 assert(!host_alloc_capture(&empty,7,key,&kl,before,&bytes)&&bytes==EFS_TXN_ABSENT);
 assert(host_prep_alloc_value(&participant,7,&t,&parts,key,kl,before,bytes,next,NULL,NULL)==EFS_ERR_STALE);
 assert(!host_prep_alloc_value(&empty,7,&t,&parts,key,kl,before,bytes,next,NULL,NULL));
 assert(!efs_txn_resolve(empty.kv,&t,7,EFS_TXN_COMMIT));
 assert(!efs_kv_put(empty.kv,key,kl,next,7));
 assert(host_alloc_capture(&empty,7,key,&kl,before,&bytes)==EFS_ERR_PROTO);
 efs_kv_mem_free(proposer.kv);efs_kv_mem_free(participant.kv);efs_kv_mem_free(empty.kv);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-alloc-value-') as directory:
    path = Path(directory) / 'gate.c'
    path.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'), str(path), str(root / 'libefs.a'), '-pthread', '-lm', '-ldl', '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=5)
print('allocator guard: equal bytes/different versions, concurrent advance, absent key and malformed watermark PASS')
