#!/usr/bin/env python3
"""Exercise the host's actual apply-return policy, including business verdicts."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
defines='\n'.join(line for line in s.splitlines() if line.startswith(('#define HOST_APPEND_RSV_LEN ', '#define HOST_LOCK_LEN ')))
a=s.index('    ret = (cmd[0] == EFS_MD_CMD_PREPARE')
# Test only verdict classification; the atomic commit scope has its own tests.
b=s.index('    /* Finish the atomic apply scope before publishing its verdict. */',a)
code=defines+'\n'+'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
static int policy(unsigned tag,unsigned sub,int rc) {
 unsigned char cmd[2]={tag,sub};unsigned clen=2;int ret;
'''+s[a:b]+'''
 return ret;
}
int main(void) {
 unsigned txn[]={EFS_MD_CMD_PREPARE,EFS_MD_CMD_DECIDE,EFS_MD_CMD_RESOLVE,EFS_MD_CMD_DROP,
 EFS_MD_CMD_PUBLISH,EFS_MD_CMD_PUBLICATION,EFS_MD_CMD_PUBLICATION_ADMIT,
 EFS_MD_CMD_PUBLICATION_RETIRE,EFS_MD_CMD_REPORT_PUBLISH,EFS_MD_CMD_ACTIVATE_LANE};
 int business[]={EFS_OK,EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_PROTO,EFS_ERR_INVAL,EFS_ERR_NOT_FOUND};
 for(unsigned i=0;i<sizeof(txn)/sizeof(txn[0]);i++) {
  assert(policy(txn[i],0,EFS_ERR_IO)==EFS_ERR_IO);
  assert(policy(txn[i],0,EFS_ERR_NOMEM)==EFS_ERR_NOMEM);
  for(unsigned j=0;j<sizeof(business)/sizeof(business[0]);j++)assert(policy(txn[i],0,business[j])==EFS_OK);
 }
 for(unsigned j=0;j<sizeof(business)/sizeof(business[0]);j++)
  assert(policy(EFS_MD_CMD_SESSION,EFS_MD_SESS_FINISH,business[j])==EFS_OK);
 assert(policy(EFS_MD_CMD_SESSION,EFS_MD_SESS_FINISH,EFS_ERR_IO)==EFS_ERR_IO);
 assert(policy(EFS_MD_CMD_SESSION,EFS_MD_SESS_FINISH,EFS_ERR_NOMEM)==EFS_ERR_NOMEM);
 for(unsigned j=0;j<sizeof(business)/sizeof(business[0]);j++) {
  int value=business[j];
  int expected=(value==EFS_ERR_STALE || value==EFS_ERR_PROTO || value==EFS_ERR_INVAL)?EFS_OK:value;
  assert(policy(EFS_MD_CMD_QUERY_POLICY,0,value)==expected);
 }
 assert(policy(EFS_MD_CMD_QUERY_POLICY,0,EFS_ERR_IO)==EFS_ERR_IO);
 assert(policy(EFS_MD_CMD_QUERY_POLICY,0,EFS_ERR_NOMEM)==EFS_ERR_NOMEM);
 assert(policy(EFS_MD_CMD_SESSION,EFS_MD_SESS_FENCE_LOC,EFS_ERR_BUSY)==EFS_OK);
 assert(policy(EFS_MD_CMD_SESSION,EFS_MD_SESS_FENCE_LOC,EFS_ERR_IO)==EFS_ERR_IO);
 assert(policy(EFS_MD_CMD_PUT_TICKET_ADMIT,0,EFS_ERR_IO)==EFS_ERR_IO);
 puts("transaction/publication Raft apply: IO/NOMEM preserve pending index; business conflicts advance with ring verdict; policy CAS conflicts advance while policy IO/NOMEM/BUSY retry; session fence and ticket storage failures retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-apply-return-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
