#!/usr/bin/env python3
"""Production transaction reads reuse authority, never decisions or absence."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_txn_coord(');b=s.index('static void stat_to_inode(',a)
code=r'''
#include "efs/txn.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;};
static unsigned hosted=5,reads[3],decision_gets;
static int read_rc,decision_rc,current=EFS_TXN_COMMIT;
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;return !!(hosted&(1u<<g));}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){(void)h;(void)hint;reads[g]++;return read_rc;}
int efs_txn_decision_get(struct efs_kv *kv,uint32_t shard,const struct efs_txid *id,int *out){(void)kv;(void)shard;(void)id;decision_gets++;if(!decision_rc)*out=current;return decision_rc;}
''' + s[a:b] + r'''
static void reset(void){hosted=5;reads[0]=reads[1]=reads[2]=decision_gets=0;read_rc=decision_rc=0;current=EFS_TXN_COMMIT;}
int main(void){
 struct efs_raft_host h={.kv=(struct efs_kv*)1};struct efs_txid id={{1}};int out=0;
 reset();struct host_txn_read_context first={.host=&h};
 assert(!host_txn_read_coord(&first,&id,1,&out)&&out==EFS_TXN_COMMIT&&reads[0]==1&&decision_gets==1);
 current=EFS_TXN_ABORT;assert(!host_txn_read_coord(&first,&id,3,&out)&&out==EFS_TXN_ABORT&&reads[0]==1&&decision_gets==2);
 struct host_txn_read_context next={.host=&h};assert(!host_txn_read_coord(&next,&id,1,&out)&&reads[0]==2&&decision_gets==3);
 reset();first=(struct host_txn_read_context){.host=&h,.covered=1};
 decision_rc=EFS_ERR_NOT_FOUND;out=99;
 assert(host_txn_read_coord(&first,&id,1,&out)==EFS_ERR_NOT_FOUND&&out==99&&!reads[0]&&decision_gets==1);
 decision_rc=0;assert(!host_txn_read_coord(&first,&id,1,&out)&&out==EFS_TXN_COMMIT&&!reads[0]&&decision_gets==2);
 assert(!host_txn_read_coord(&first,&id,2,&out)&&reads[2]==1&&first.covered==5);
 assert(!host_txn_read_coord(&first,&id,4,&out)&&reads[2]==1&&decision_gets==4);
 reset();first=(struct host_txn_read_context){.host=&h};decision_rc=EFS_ERR_NOT_FOUND;
 assert(host_txn_read_coord(&first,&id,1,&out)==EFS_ERR_NOT_FOUND&&first.covered==1&&reads[0]==1);
 decision_rc=0;assert(!host_txn_read_coord(&first,&id,1,&out)&&reads[0]==1&&decision_gets==2);
 int errors[]={EFS_ERR_BUSY,EFS_ERR_NO_QUORUM,EFS_ERR_IO};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  reset();first=(struct host_txn_read_context){.host=&h};read_rc=errors[i];
  assert(host_txn_read_coord(&first,&id,1,&out)==(read_rc==EFS_ERR_BUSY?EFS_ERR_BUSY:EFS_ERR_IO));
  assert(!first.covered&&!decision_gets&&reads[0]==1);read_rc=0;
  assert(!host_txn_read_coord(&first,&id,1,&out)&&reads[0]==2&&decision_gets==1);
 }
 reset();first=(struct host_txn_read_context){.host=&h,.covered=5};hosted=1;
 assert(host_txn_read_coord(&first,&id,2,&out)==EFS_ERR_IO&&first.unhosted_coordinator&&!reads[2]&&!decision_gets);
 reset();first=(struct host_txn_read_context){.host=&h};decision_rc=EFS_ERR_IO;
 assert(host_txn_read_coord(&first,&id,1,&out)==EFS_ERR_IO&&!first.covered&&decision_gets==1);
 puts("transaction read reuse: RPC-local fences, new groups/requests, fresh decisions, missing-decision uncertainty and authority/storage refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-read-reuse-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
