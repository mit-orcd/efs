#!/usr/bin/env python3
"""Exercise actual packed progress under ambiguous and terminal RPC outcomes."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/publication_writeback.c').read_text()
a=s.index('int efs_client_publication_progress(')
b=s.index('\nint efs_client_publication_writeback(',a)
code=r'''
#include "efs/publication_ack.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct efs_msg_publication body;
static int lookup_rc, rpc_rc, session_rc;
static unsigned submits,probes,sessions,state;
int efs_publication_ack_request(const struct efs_publication_ack_queue *q,uint64_t seq,struct efs_msg_publication *out){(void)q;assert(seq==body.id.seq);*out=body;return lookup_rc;}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){assert(ino==body.rec.ino&&lane==0);return 1;}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id){assert(shard==1&&!memcmp(id,&body.id,sizeof(*id)));sessions++;return session_rc;}
int efs_client_rpc_publication(const struct efs_msg_publication *req,int mode,struct efs_msg_publication_reply *out){assert(!memcmp(req,&body,sizeof(body)));assert(mode==0||mode==1);if(mode)probes++;else submits++;if(rpc_rc)return rpc_rc;memset(out,0,sizeof(*out));out->state=state;out->verdict=state==EFS_PUBLICATION_REJECTED?EFS_ERR_STALE:0;out->receipt_index=123;return 0;}
'''+s[a:b]+r'''
int main(void){struct efs_publication_ack_queue q={0};struct efs_msg_publication_reply out,before;
 body.id.client_uuid[0]=9;body.id.session_epoch=7;body.id.seq=11;body.rec.ino=101;body.rec.file_generation=19;body.rec.chunk_generation=29;body.rec.publish_flags=EFS_CHUNK_REC_F_PACKED_BASE;
 memset(&out,0x5a,sizeof(out));before=out;
 lookup_rc=EFS_ERR_NOT_FOUND;assert(efs_client_publication_progress(&q,11,&out)==EFS_ERR_NOT_FOUND&&!submits&&!probes&&!sessions);lookup_rc=0;
 /* Neither ambiguous transport nor missing authority fabricates consumption. */
 const int errors[]={EFS_ERR_NET,EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_NOT_FOUND,EFS_ERR_IO};
 for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++){rpc_rc=errors[i];assert(efs_client_publication_progress(&q,11,&out)==errors[i]);assert(!memcmp(&out,&before,sizeof(out)));}
 assert(submits==5&&!probes&&!sessions);
 /* Retrying exactly the same immutable intent can recover its durable result. */
 rpc_rc=0;state=EFS_PUBLICATION_COMMITTED;assert(!efs_client_publication_progress(&q,11,&out)&&out.receipt_index==123);
 state=EFS_PUBLICATION_REJECTED;assert(!efs_client_publication_progress(&q,11,&out)&&out.verdict==EFS_ERR_STALE);
 state=EFS_PUBLICATION_PENDING;assert(efs_client_publication_progress(&q,11,&out)==EFS_ERR_BUSY);
 state=EFS_PUBLICATION_APPLY_PENDING;assert(efs_client_publication_progress(&q,11,&out)==EFS_ERR_BUSY);
 state=EFS_PUBLICATION_RETIRED;assert(efs_client_publication_progress(&q,11,&out)==EFS_ERR_STALE);
 assert(submits==10&&!probes&&!sessions);
 body.rec.publish_flags=0;state=EFS_PUBLICATION_COMMITTED;assert(!efs_client_publication_progress(&q,11,&out)&&probes==1&&submits==10);
 rpc_rc=EFS_ERR_BUSY;session_rc=EFS_ERR_IO;assert(efs_client_publication_progress(&q,11,&out)==EFS_ERR_IO&&probes==2&&sessions==1&&submits==10);
 puts("packed progress: exact-body direct retry, lost reply/missing authority retention, terminal rejection, pending/retired refusal and legacy status/session behavior PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-packed-progress-') as td:
 p=Path(td)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
