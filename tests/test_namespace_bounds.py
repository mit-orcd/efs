#!/usr/bin/env python3
"""Production ancestry walk: exact shared-shard guards and bounded rejection."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_parts_add(');b=s.index('\nstruct host_idx_ref',a)
code=r'''
#include <efs/txn.h>
#include <efs/raft.h>
#include <efs/meta_apply.h>
#include <efs/kv_key.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
struct efs_raft_host { struct efs_kv *kv; };
static int self_parent,read_error;
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint) {
 (void)h;(void)g;(void)hint;return read_error;
}
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *r) {
 (void)kv;memset(r,0,sizeof(*r));r->mode=S_IFDIR|0755;
 r->parent=self_parent?ino:(ino>4096?ino-4096:EFS_ROOT_INO);return EFS_OK;
}
int efs_txn_ver_get(struct efs_kv *kv,const uint8_t *key,uint32_t n,uint64_t *v) {
 (void)kv;(void)key;(void)n;*v=1;return EFS_OK;
}
'''+s[a:b]+r'''
int main(void) {
 struct efs_raft_host h={0};struct efs_txn_parts p={0};
 struct {struct host_pver_guard g[EFS_TXN_NAMESPACE_MAX_PART];uint64_t canary;} out;
 int n,hint=0;out.canary=0xabcddcba;
 assert(host_pver_guard_chain(&h,63*4096+1,999,&p,out.g,&n,&hint)==EFS_OK);
 assert(n==64&&p.n==1&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,64*4096+1,999,&p,out.g,&n,&hint)==EFS_ERR_BUSY);
 assert(n==64&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,4097,4097,&p,out.g,&n,&hint)==EFS_ERR_INVAL&&n==0);
 self_parent=1;p.n=0;
 assert(host_pver_guard_chain(&h,4097,999,&p,out.g,&n,&hint)==EFS_ERR_INVAL);
 self_parent=0;read_error=EFS_ERR_IO;p.n=0;
 assert(host_pver_guard_chain(&h,4097,999,&p,out.g,&n,&hint)==EFS_ERR_IO&&n==0);
 p.n=0;for(unsigned i=0;i<EFS_TXN_NAMESPACE_MAX_PART;i++)assert(!host_parts_add(&p,i));
 assert(!host_parts_add(&p,0)&&p.n==EFS_TXN_NAMESPACE_MAX_PART);
 assert(host_parts_add(&p,4095)==EFS_ERR_BUSY&&p.n==EFS_TXN_NAMESPACE_MAX_PART);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-bound-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('namespace: exact 64 same-shard guards, 65 BUSY, observed cycles INVAL, ReadIndex IO, participant boundary PASS')
