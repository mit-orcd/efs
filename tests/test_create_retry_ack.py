#!/usr/bin/env python3
"""A CREATE retry frees completed receipts without changing its identity."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text()
def fn(name):
    m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M)
    assert m,name
    return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/opid.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t opid_cv=PTHREAD_COND_INITIALIZER;
int efs_client_rpc_past_deadline(void) {return 0;}
uint64_t efs_client_rpc_deadline_ms(void) {return 0;}
static void opid_seed_locked(void) {}
'''+ '\n'.join(fn(n) for n in ('opid_take_locked','opid_begin','opid_end','opid_refresh_ack','opid_retry_ack'))+r'''
int main(void) {
 g_client.opid_next=1;g_client.opid_epoch=1;g_client.opid_uuid[0]=1;
 struct efs_opid_req old,q;
 int oldest=opid_begin(&old);assert(oldest>=0);
 uint8_t cur[EFS_OPID_VAL_MAX],next[EFS_OPID_VAL_MAX];uint32_t cn=0;
 for(unsigned i=0;i<EFS_OPID_REPLY_CACHE-1;i++) {
  int slot=opid_begin(&q);assert(slot>=0 && q.ack==0);
  struct efs_opid_reply rep={.rc=EFS_OK,.ino=100+i};uint32_t nn=sizeof(next);
  assert(efs_opid_fold(cur,cn,q.id.client_uuid,q.id.session_epoch,&q,&rep,next,&nn)==EFS_OK);
  memcpy(cur,next,nn);cn=nn;opid_end(slot);
 }
 uint64_t next_sequence=g_client.opid_next;
 assert(opid_begin(&q)<0&&!q.id.seq&&g_client.opid_next==next_sequence);
 /* The reserved oldest slot can still commit into the durable cache. */
 struct efs_opid_reply oldest_reply={.rc=EFS_OK,.ino=7};uint32_t nn=sizeof(next);
 assert(efs_opid_fold(cur,cn,old.id.client_uuid,old.id.session_epoch,&old,&oldest_reply,next,&nn)==EFS_OK);
 memcpy(cur,next,nn);cn=nn;opid_end(oldest);
 oldest=opid_begin(&old);assert(oldest>=0);
 int current=opid_begin(&q);assert(current>=0&&q.ack==old.id.seq-1);
 uint64_t original_ack=q.ack;
 struct efs_opid immutable=q.id;
 struct efs_opid_reply rep={.rc=EFS_OK,.ino=42};
 uint8_t packet[sizeof(struct efs_msg_inode_create)+EFS_OPID_WIRE_LEN]={0};
 uint8_t *suffix=packet+sizeof(struct efs_msg_inode_create);
 efs_opid_req_pack(&q,suffix);opid_retry_ack(packet,sizeof(packet));
 efs_opid_req_unpack(&q,suffix);assert(q.ack==original_ack); /* oldest still owns identity */
 opid_end(oldest);opid_retry_ack(packet,sizeof(packet));efs_opid_req_unpack(&q,suffix);
 assert(!memcmp(&q.id,&immutable,sizeof(immutable)) && q.ack==q.id.seq-1);
 nn=sizeof(next);
 assert(efs_opid_fold(cur,cn,q.id.client_uuid,q.id.session_epoch,&q,&rep,next,&nn)==EFS_OK);
 struct efs_opid_reply replay;
 assert(efs_opid_probe(next,nn,&q.id,&replay)==1 && replay.rc==EFS_OK && replay.ino==42);
 opid_end(current);
 puts("CREATE retry ACK: full durable cache, pinned old operation, same identity, no self-ACK and replay recovery PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-ack-') as d:
    path=Path(d)/'test.c';path.write_text(code)
    subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'src/client'),str(path),
                    str(root/'libefs.a'),'-ldl','-lm','-libverbs','-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
