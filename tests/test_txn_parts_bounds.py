#!/usr/bin/env python3
"""Exercise production participant encoding at exact capacity and malformed counts."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=(root/'src/meta/txn.c').read_text()
def fn(name):
 a=src.index('static '+name);return src[a:src.index('\n}',a)+2]
code=r'''
#include "efs/txn.h"
#include <assert.h>
#include <string.h>
'''+fn('void be32(')+'\n'+fn('int pack_parts(')+r'''
int main(void){
 struct efs_txn_parts p={0};uint8_t out[EFS_TXN_PARTS_BYTES+2],saved[sizeof(out)];
 p.n=EFS_TXN_MAX_PART;for(unsigned i=0;i<p.n;i++)p.shard[i]=0x01020300u+i;
 memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
 assert(pack_parts(out+1,EFS_TXN_PARTS_BYTES-1,&p)==EFS_ERR_INVAL&&!memcmp(out,saved,sizeof(out)));
 assert(pack_parts(out+1,EFS_TXN_PARTS_BYTES,&p)==EFS_TXN_PARTS_BYTES);
 assert(out[0]==0xa5&&out[sizeof(out)-1]==0xa5&&out[1]==EFS_TXN_MAX_PART);
 for(unsigned i=0;i<p.n;i++){unsigned off=2+4*i;assert(out[off]==1&&out[off+1]==2&&out[off+2]==3&&out[off+3]==i);}
 unsigned invalid[]={0,EFS_TXN_MAX_PART+1,255};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  p.n=invalid[i];memset(out,0xa5,sizeof(out));
  assert(pack_parts(out+1,EFS_TXN_PARTS_BYTES,&p)==EFS_ERR_INVAL&&!memcmp(out,saved,sizeof(out)));
 }
 p.n=1;assert(pack_parts(out+1,4,&p)==EFS_ERR_INVAL&&!memcmp(out,saved,sizeof(out)));
 assert(pack_parts(out+1,5,&p)==5&&out[1]==1&&out[6]==0xa5);
 assert(pack_parts(NULL,5,&p)==EFS_ERR_INVAL&&pack_parts(out,5,NULL)==EFS_ERR_INVAL);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-parts-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O3','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('Transaction parts: exact configured-capacity participant wire, canaries, short output, invalid counts and null pointers PASS')
