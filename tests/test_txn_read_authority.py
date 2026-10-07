#!/usr/bin/env python3
"""A coordinator lookup cannot interpret failed fresh authority as absence."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'src/server/raft_host.c').read_text()
match=re.search(r'^static int host_txn_coord\([^;]+?\)\n\{',text,re.M)
function=text[match.start():text.index('\n}',match.end())+2]
code=r'''
#include "efs/txn.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
struct efs_raft_host { struct efs_kv *kv; };
static int read_rc,decision_rc,reads,gets;
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint) {
 (void)h;(void)group;(void)hint;reads++;return read_rc;
}
int efs_txn_decision_get(struct efs_kv *kv,uint32_t shard,const struct efs_txid *id,int *decision) {
 (void)kv;(void)shard;(void)id;gets++;*decision=1;return decision_rc;
}
'''+function+r'''
int main(void) {
 struct efs_raft_host host={.kv=(struct efs_kv *)1};struct efs_txid id={{1}};int decision=0;
 int refusals[]={EFS_ERR_BUSY,EFS_ERR_NOT_PRIMARY,EFS_ERR_IO};
 for(unsigned i=0;i<3;i++) {
  read_rc=refusals[i];assert(host_txn_coord(&host,&id,1,&decision)==EFS_ERR_IO);
  assert(!gets && !decision);
 }
 read_rc=EFS_OK;decision_rc=EFS_ERR_NOT_FOUND;
 assert(host_txn_coord(&host,&id,1,&decision)==EFS_ERR_NOT_FOUND && gets==1);
 decision_rc=EFS_OK;assert(!host_txn_coord(&host,&id,1,&decision) && decision==1 && gets==2 && reads==5);
 puts("transaction coordinator: failed quorum authority never becomes decision absence PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-authority-') as work:
 p=Path(work)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
