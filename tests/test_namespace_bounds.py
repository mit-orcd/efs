#!/usr/bin/env python3
"""Production ancestry walk: exact shared-shard guards and bounded rejection."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_namespace_capacity(');expiry_start=s.index('static int host_expiry_collect(',a)
s=s[:expiry_start]+s[s.index('\n}',expiry_start)+2:]
b=s.index('\nstruct host_idx_ref',a)
def function(path,name):
    text=(root/path).read_text()
    match=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',text,re.M)
    assert match,name
    return text[match.start():text.index('\n}',match.end())+2]
mapping='\n'.join(function(path,name) for path,name in (
 ('src/server/raft_host.c','rc_to_inode_status'),
 ('src/client/inode_rpc.c','rpc_status_to_efs'),
 ('src/client/efs_fuse.c','efs_rc_to_errno')))
code=r'''
#include <efs/txn.h>
#include <efs/raft.h>
#include <efs/meta_apply.h>
#include <efs/kv_key.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <efs/protocol.h>
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
'''+mapping+s[a:b]+r'''
int main(void) {
 assert(rc_to_inode_status(EFS_ERR_OVERFLOW)==EFS_INODE_RPC_OVERFLOW);
 assert(rpc_status_to_efs(EFS_INODE_RPC_OVERFLOW)==EFS_ERR_OVERFLOW);
 assert(efs_rc_to_errno(EFS_ERR_OVERFLOW)==-EOVERFLOW);
 assert(rc_to_inode_status(EFS_ERR_BUSY)==EFS_INODE_RPC_BUSY);
 assert(rpc_status_to_efs(EFS_INODE_RPC_BUSY)==EFS_ERR_BUSY);
 assert(efs_rc_to_errno(EFS_ERR_BUSY)==-EBUSY);
 struct efs_raft_host h={0};struct efs_txn_parts p={0};
 struct {struct host_pver_guard g[EFS_TXN_NAMESPACE_MAX_PART];uint64_t canary;} out;
 int n,hint=0;out.canary=0xabcddcba;
 assert(host_pver_guard_chain(&h,63*4096+1,999,&p,out.g,&n,&hint)==EFS_OK);
 assert(n==64&&p.n==1&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,64*4096+1,999,&p,out.g,&n,&hint)==EFS_OK);
 assert(n==65&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,(EFS_TXN_NAMESPACE_MAX_PART-1)*4096+1,999,&p,out.g,&n,&hint)==EFS_OK);
 assert(n==EFS_TXN_NAMESPACE_MAX_PART&&p.n==1&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,EFS_TXN_NAMESPACE_MAX_PART*4096+1,999,&p,out.g,&n,&hint)==EFS_ERR_OVERFLOW);
 assert(n==EFS_TXN_NAMESPACE_MAX_PART&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,100*4096+1,30*4096+1,&p,out.g,&n,&hint)==EFS_ERR_INVAL);
 assert(n==70&&out.canary==0xabcddcba);
 p.n=0;assert(host_pver_guard_chain(&h,4097,4097,&p,out.g,&n,&hint)==EFS_ERR_INVAL&&n==0);
 self_parent=1;p.n=0;
 assert(host_pver_guard_chain(&h,4097,999,&p,out.g,&n,&hint)==EFS_ERR_INVAL);
 self_parent=0;read_error=EFS_ERR_IO;p.n=0;
 assert(host_pver_guard_chain(&h,4097,999,&p,out.g,&n,&hint)==EFS_ERR_IO&&n==0);
 read_error=EFS_ERR_BUSY;p.n=0;
 assert(host_pver_guard_chain(&h,4097,999,&p,out.g,&n,&hint)==EFS_ERR_BUSY&&n==0);
 read_error=0;
 /* Existing lane participants share the same envelope with ancestry. */
 p.n=0;for(unsigned i=2;i<EFS_TXN_NAMESPACE_MAX_PART+1;i++)assert(!host_parts_add(&p,i));
 assert(!host_pver_guard_chain(&h,1,999,&p,out.g,&n,&hint));
 assert(n==1&&p.n==EFS_TXN_NAMESPACE_MAX_PART&&out.canary==0xabcddcba);
 p.n=0;for(unsigned i=2;i<EFS_TXN_NAMESPACE_MAX_PART+2;i++)assert(!host_parts_add(&p,i));
 assert(host_pver_guard_chain(&h,1,999,&p,out.g,&n,&hint)==EFS_ERR_OVERFLOW);
 assert(n==0&&p.n==EFS_TXN_NAMESPACE_MAX_PART&&out.canary==0xabcddcba);
 p.n=0;for(unsigned i=0;i<EFS_TXN_NAMESPACE_MAX_PART;i++)assert(!host_parts_add(&p,i));
 assert(!host_parts_add(&p,0)&&p.n==EFS_TXN_NAMESPACE_MAX_PART);
 assert(host_parts_add(&p,4095)==EFS_ERR_OVERFLOW&&p.n==EFS_TXN_NAMESPACE_MAX_PART);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-bound-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('namespace: 65/144 same-shard guards, 145 OVERFLOW, deep observed cycle INVAL, ReadIndex IO, participant boundary PASS')
