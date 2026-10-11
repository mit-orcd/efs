#!/usr/bin/env python3
"""Applied REGISTER proof is exact; namespace admission never caches that proof."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_NAMESPACE_OWNER_SOURCE',root/'src/server/raft_host.c')).read_text()
# Reconciled callers now pass a request-local group bitmap, never a remembered
# owner-preparation verdict. Fresh ACTIVE/epoch/floor behavior is exercised by
# test_namespace_admission_rounds and test_session_read_proof.
assert s.count('host_namespace_session_admit_after_group(h,dsh,q,0,') == 2
assert s.count('host_namespace_session_admit_covered(h,dsh,q,') == 3
start=s.index('static uint64_t host_session_register_active_proof(')
end=s.index('static int apply_session_cmd(',start)
code=r'''
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_raft_host {struct efs_kv *kv;uint64_t apply_extra;};
static int register_rc,get_rc;
static struct efs_session_rec owner;
int efs_session_register(struct efs_kv *kv,const uint8_t id[EFS_OPID_UUID_LEN],uint32_t epoch,uint32_t shard){
 (void)kv;assert(id[0]==19&&epoch==3&&shard==5);return register_rc;
}
int efs_session_get(struct efs_kv *kv,const uint8_t id[EFS_OPID_UUID_LEN],struct efs_session_rec *out){
 (void)kv;assert(id[0]==19);*out=owner;return get_rc;
}
int efs_session_bit_get(const uint8_t *bits,uint32_t shard){return !!(bits[shard/8]&(1u<<(shard%8)));}
'''+s[start:end]+r'''
int main(void){
 struct efs_raft_host h={0};uint8_t uuid[16]={19};
 owner.epoch=3;owner.state=EFS_SESSION_ACTIVE;owner.touched[0]=1u<<5;
 assert(!host_session_register_apply(&h,uuid,3,5));
 assert(h.apply_extra==((1ull<<63)|(5ull<<32)|3));
 assert(h.apply_extra!=host_session_register_active_proof(4,5));
 assert(h.apply_extra!=host_session_register_active_proof(3,6));
 const int errors[]={EFS_ERR_NOT_FOUND,EFS_ERR_INVAL,EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
  register_rc=errors[i];h.apply_extra=UINT64_MAX;
  assert(host_session_register_apply(&h,uuid,3,5)==errors[i]&&!h.apply_extra);
 }
 register_rc=0;
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
  get_rc=errors[i];h.apply_extra=UINT64_MAX;
  assert(!host_session_register_apply(&h,uuid,3,5)&&!h.apply_extra);
 }
 get_rc=0;
 owner.state=EFS_SESSION_FENCING;h.apply_extra=UINT64_MAX;
 assert(!host_session_register_apply(&h,uuid,3,5)&&!h.apply_extra);
 owner.state=EFS_SESSION_ACTIVE;owner.epoch=4;
 assert(!host_session_register_apply(&h,uuid,3,5)&&!h.apply_extra);
 owner.epoch=3;owner.touched[0]=0;
 assert(!host_session_register_apply(&h,uuid,3,5)&&!h.apply_extra);
 puts("REGISTER proof: exact ACTIVE epoch/shard only; fencing, absent bit and all read/apply refusals clear stale proof PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-owner-once-') as td:
 p=Path(td)/'check.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
