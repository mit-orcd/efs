#!/usr/bin/env python3
"""Actual inactive coordinator wrapper + real memory journal.
ReadIndex, writer authority and proposal execution are adapters, not quorum proof.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('int server_raft_host_repair_record(');fn=s[a:s.index('\n}',a)+2];a=s.index('int server_raft_host_repair_prepare(');prepare=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/repair_job.h"
#include "efs/meta_cmd.h"
#include "efs/protocol.h"
#include "efs/raft.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
struct server {uint32_t id;int repair_experiment;};
struct efs_raft_host {struct efs_kv *kv;int running;pthread_mutex_t mu;struct server *s;};
struct host_group {struct efs_raft *r;};
static struct efs_raft_host *g_host;static struct host_group group_slot_value;
static int role=EFS_RAFT_LEADER,read_rc,proposal_rc,lost_reply,proposals,reads;
static uint64_t term=1,entry_term;static struct host_group *group_slot(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return &group_slot_value;}
int efs_raft_role(const struct efs_raft *r){(void)r;return role;}
uint64_t efs_raft_term(const struct efs_raft *r){(void)r;return term;}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){(void)h;(void)g;(void)hint;reads++;return read_rc;}
static void wr32be(uint8_t *b,uint32_t v){b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v;}
void server_raft_host_writer_view(const struct efs_msg_inode_writer_view *q,struct efs_msg_inode_writer_view_reply *r){memset(r,0,sizeof(*r));r->status=EFS_INODE_RPC_OK;r->ino=q->ino;r->generation=q->generation;r->chunk_index=q->chunk_index;}
static int host_propose_wait_idx(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,int *hint,uint64_t *idx){(void)hint;(void)idx;proposals++;if(proposal_rc)return proposal_rc;int rc=efs_repair_apply_command_term(h->kv,g,entry_term?entry_term:term,cmd,n);return !rc&&lost_reply?EFS_ERR_BUSY:rc;}
'''+prepare+fn+r'''
int main(void){
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);struct server node={1,1};struct efs_raft_host h={.kv=kv,.running=1,.s=&node};pthread_mutex_init(&h.mu,NULL);g_host=&h;group_slot_value.r=(void*)&h;
 struct efs_meta_attrs at={0,0,1};efs_ino_t ino;assert(!efs_meta_apply_init(kv,1));assert(!efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"coordinator",&ino));
 struct efs_meta_row row;assert(!efs_meta_apply_get_inode(kv,ino,&row));struct efs_meta_pub p={.ino=ino,.candidate_gen=101,.new_size=131072,.coding_profile_id=EFS_META_PROFILE_K2F1};for(unsigned i=0;i<3;i++){p.ch.nodes[i]=i+1;memset(p.ch.checksums[i],10+i,32);}assert(!efs_meta_apply_publish(kv,&p));
 struct efs_repair_job q={.ino=ino,.inode_gen=row.generation,.object=101,.fragment_len=65536,.epoch=1,.owner=1},j,got;q.uuid[0]=1;q.target_store[0]=7;assert(!efs_repair_capture(kv,&q,&j));
 node.repair_experiment=0;assert(server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0)==EFS_ERR_ACCES&&!proposals&&!reads);node.repair_experiment=1;
 read_rc=EFS_ERR_IO;assert(server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0)==EFS_ERR_IO&&!proposals);read_rc=0;
 role=EFS_RAFT_FOLLOWER;assert(server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0)==EFS_ERR_NOT_PRIMARY&&!proposals);role=EFS_RAFT_LEADER;
 node.id=2;assert(server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0)==EFS_ERR_STALE&&!proposals);node.id=1;
 entry_term=2;assert(server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0)==EFS_ERR_STALE);assert(efs_repair_get(kv,&j,&got)==EFS_ERR_NOT_FOUND);entry_term=0;
 struct efs_repair_job prepared;uint8_t state,flags;
 assert(!server_raft_host_repair_prepare(&j,&prepared,&state,&flags)&&!state&&!flags);
 assert(prepared.owner==1&&prepared.epoch==1);
 role=EFS_RAFT_FOLLOWER;assert(server_raft_host_repair_prepare(&j,&prepared,&state,&flags)==EFS_ERR_NOT_PRIMARY);role=EFS_RAFT_LEADER;
 lost_reply=1;assert(server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0)==EFS_ERR_BUSY);assert(!efs_repair_get(kv,&j,&got));assert(efs_repair_gc_ready(kv,ino,row.generation,0,101)==EFS_ERR_BUSY);lost_reply=0;
 assert(!server_raft_host_repair_prepare(&j,&prepared,&state,&flags)&&state==EFS_REPAIR_ADMITTED&&!flags);
 term=2;assert(server_raft_host_repair_prepare(&j,&prepared,&state,&flags)==EFS_ERR_STALE);term=1;
 assert(!server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0));
 uint8_t receipt[EFS_REPAIR_RECEIPT_BYTES];assert(!efs_repair_receipt_encode(&j,EFS_REPAIR_RECEIPT_SEAL,receipt));int before=proposals;assert(server_raft_host_repair_record(EFS_REPAIR_OP_SEAL,&j,2,receipt,sizeof(receipt))==EFS_ERR_BUSY&&proposals==before);
 assert(!efs_repair_receipt_encode(&j,EFS_REPAIR_RECEIPT_DATA,receipt));receipt[68]^=1;before=reads;assert(server_raft_host_repair_record(EFS_REPAIR_OP_DATA,&j,0,receipt,sizeof(receipt))==EFS_ERR_PROTO&&reads==before);receipt[68]^=1;
 term=2;node.id=2;assert(server_raft_host_repair_record(EFS_REPAIR_OP_DATA,&j,0,receipt,sizeof(receipt))==EFS_ERR_STALE);
 assert(!server_raft_host_repair_record(EFS_REPAIR_OP_CANCEL,&j,0,NULL,0));assert(!efs_repair_get(kv,&j,&got)&&got.flags==EFS_REPAIR_CANCELLED);
 assert(!efs_repair_receipt_encode(&j,EFS_REPAIR_RECEIPT_SEAL,receipt));lost_reply=1;assert(server_raft_host_repair_record(EFS_REPAIR_OP_SEAL,&j,2,receipt,sizeof(receipt))==EFS_ERR_BUSY);assert(!efs_repair_get(kv,&j,&got)&&got.state==EFS_REPAIR_SEALED);lost_reply=0;
 assert(!server_raft_host_repair_record(EFS_REPAIR_OP_SEAL,&j,2,receipt,sizeof(receipt)));assert(!server_raft_host_repair_record(EFS_REPAIR_OP_RETIRE,&j,1,NULL,0));assert(!server_raft_host_repair_record(EFS_REPAIR_OP_RETIRE,&j,1,NULL,0));
 q.uuid[0]=2;q.epoch=2;q.owner=2;assert(!efs_repair_capture(kv,&q,&j));assert(!server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0));
 assert(!efs_repair_receipt_encode(&j,EFS_REPAIR_RECEIPT_DATA,receipt));assert(!server_raft_host_repair_record(EFS_REPAIR_OP_DATA,&j,0,receipt,sizeof(receipt)));assert(!efs_repair_get(kv,&j,&got)&&(got.flags&EFS_REPAIR_DATA_DURABLE));
 efs_kv_mem_free(kv);puts("inactive actual coordinator wrapper+memory journal: malformed receipt before authority, current owner/committed term, lost reply debt, old-owner cancel/seal/retire, new UUID takeover PASS; ReadIndex/proposal/writer RPC are adapters");return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'coordinator.c';p.write_text(code);b=Path(d)/'coordinator'
 subprocess.run(['cc','-D_GNU_SOURCE','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-lpthread','-lm','-ldl','-o',str(b)],check=True)
 subprocess.run([b],check=True)
