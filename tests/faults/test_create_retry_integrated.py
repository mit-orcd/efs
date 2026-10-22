#!/usr/bin/env python3
"""Run actual shared CREATE fault hook and retained caller together."""
from pathlib import Path
import os,re,subprocess,tempfile,json,shlex
root=Path(os.environ['EFS_CREATE_TEST_ROOT']) if 'EFS_CREATE_TEST_ROOT' in os.environ else Path(__file__).resolve().parents[2]
source=Path(os.environ.get('EFS_CREATE_FAULT_SOURCE',str(root/'src/client/inode_rpc.c'))).read_text()
def function(name):
 match=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',source,re.M)
 assert match,name
 return source[match.start():source.index('\n}',match.end())+2]
code=r'''
#include "client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
void efs_client_session_fence(int rc){(void)rc;}
'''+function('rpc_fault_create_reply')+function('rpc_status_to_efs')+r'''
static unsigned wire_calls,ends,created;
static struct efs_msg_inode_create original_create;
static struct efs_opid original_id;
static void opid_end(int slot){assert(slot==7);assert(++ends==1);}
/* Controlled authoritative receipt; no server/storage acceptance is claimed.
 * The actual shared fault hook runs exactly where the transport calls it. */
static int rpc_send_recv_owner(efs_ino_t parent,uint8_t type,const void *body,
 uint32_t len,uint8_t expected,void *payload,uint32_t plen) {
 const struct efs_msg_create_retained *r=body;
 struct efs_msg_create_retained_reply *reply=payload;
 assert(parent==r->create.parent && type==EFS_MSG_CREATE_RETAINED);
 assert(len==sizeof(*r) && expected==EFS_MSG_CREATE_RETAINED_REPLY && plen==sizeof(*reply));
 if(!wire_calls){original_create=r->create;original_id=r->q.id;created++;}
 assert(created==1 && !memcmp(&original_create,&r->create,sizeof(original_create)));
 assert(!memcmp(&original_id,&r->q.id,sizeof(original_id)));
 wire_calls++;
 *reply=(struct efs_msg_create_retained_reply){0};
 reply->known=reply->routed=1;reply->shard=1;reply->generation=99;
 reply->rpc.status=EFS_INODE_RPC_OK;reply->rpc.inode.ino=4097;
 int rc=rpc_fault_create_reply(type,body,len,expected,payload,plen);
 return rc ? EFS_ERR_NET : EFS_OK;
}
'''+function('create_request_rpc')+r'''
int main(void){
 struct efs_client_create_request r={0};
 r.wire.create.parent=1;r.wire.create.mode=0100600;
 r.wire.create.flags=EFS_CREATE_F_PENDING;
 strcpy(r.wire.create.name,"w58-live-retry");
 r.wire.q.id.client_uuid[0]=19;r.wire.q.id.session_epoch=7;r.wire.q.id.seq=42;
 r.slot=7;r.active=r.admitted=r.attempted=1;
 int rc=EFS_ERR_BUSY;unsigned attempts=0;
 while(!r.complete && attempts<8){
  r.wire.q.ack=attempts;rc=create_request_rpc(&r,attempts%2);attempts++;
  if(!r.complete){assert(!ends && r.slot==7);assert(rc==EFS_ERR_NET || rc==EFS_ERR_BUSY);}
 }
 assert(r.complete && r.result==EFS_OK && rc==EFS_OK && r.slot==-1);
 assert(ends==1 && created==1 && r.inode.ino==4097 && r.generation==99);
 assert(r.wire.routed==1 && r.wire.shard==1);
 printf("actual retained caller: attempts=%u wire_calls=%u one receipt, stable identity/inode/generation, ACK/probe changes, one slot release PASS\n",attempts,wire_calls);
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-integrated-') as directory:
 p=Path(directory)/'test.c';p.write_text(code);marker=Path(directory)/'marker';marker.touch();binary=p.with_suffix('')
 cc=shlex.split(os.environ.get('CC','cc'))
 subprocess.run([*cc,'-O1','-g','-std=c99','-D_GNU_SOURCE','-DEFS_FAULTS=1','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-lpthread','-o',str(binary)],check=True)
 result=subprocess.run([str(binary)],env=dict(os.environ,EFS_FAULT_CREATE_RETRY_FILE=str(marker)),capture_output=True,text=True)
 print(result.stdout,end='');print(result.stderr,end='');assert result.returncode==0,result.returncode
 stages=re.findall(r'test-create-retry: stage=(\d+) seq=(\d+) ino=(\d+) identical=1',result.stderr)
 print(json.dumps({'stages':stages,'actual_function_names':['rpc_fault_create_reply','create_request_rpc','rpc_status_to_efs'],'scope':'controlled receipt/transport; live server, dentry and OPEN token cold oracles separately required'}))
 assert len(stages)==3 and [int(x[0]) for x in stages]==[0,1,2],stages
 assert len({x[1:] for x in stages})==1 and stages[0][1:]==('42','4097'),stages
print('single shared fault injection across retained caller PASS')
