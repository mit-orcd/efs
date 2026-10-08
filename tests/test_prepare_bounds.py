#!/usr/bin/env python3
"""Exercise the production PREPARE encoders with maximum participants/key/payload."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(signature):
    a=s.index(signature)
    return s[a:s.index('\n}',a)+2]
code='''#include <efs/txn.h>
#include <efs/meta_cmd.h>
#include <assert.h>
#include <string.h>
'''+ '\n'.join(function(x) for x in ('static void wr32be(', 'static void wr64be(', 'static uint32_t pack_prep(', 'static uint32_t pack_prep_raw('))+r'''
int main(void) {
 struct efs_txn_parts p={0};struct efs_txid t={{0}};
 unsigned char key[EFS_KV_KEY_MAX],value[2*EFS_TXN_VALUE_MAX+9];
 struct { unsigned char before,body[EFS_TXN_PREPARE_MAX],after;} out;
 memset(key,1,sizeof(key));memset(value,2,sizeof(value));
 p.n=EFS_TXN_MAX_PART;for(unsigned i=0;i<p.n;i++)p.shard[i]=i;
 memset(&out,0xa5,sizeof(out));
 unsigned n=pack_prep(out.body,EFS_TXN_EXCL,&t,&p,key,sizeof(key),0,0,value,EFS_TXN_VALUE_MAX);
 assert(n<=sizeof(out.body)&&out.before==0xa5&&out.after==0xa5);
 assert(out.body[18]==EFS_TXN_MAX_PART);
 n=pack_prep_raw(out.body,EFS_TXN_EXCL_VALUE,&t,&p,key,sizeof(key),value,sizeof(value));
 assert(n<=sizeof(out.body)&&out.before==0xa5&&out.after==0xa5);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-prepare-bounds-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('PREPARE maximum participant/key/CAS payload canaries PASS')
