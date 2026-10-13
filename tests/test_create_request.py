#!/usr/bin/env python3
"""Execute retained CREATE across admission/reply ambiguity and route changes."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
a=s.index('static int opid_begin(');b=s.index('/* A reservation identity',a)
c=s.index('static void opid_refresh_ack(');d=s.index('static void opid_retry_ack(',c)
e=s.index('int efs_client_session_admit(');f=s.index('int efs_client_rpc_getattr(',e)
code=r'''
#include "client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t opid_cv=PTHREAD_COND_INITIALIZER;
int efs_client_rpc_past_deadline(void) {return 0;}
uint64_t efs_client_rpc_deadline_ms(void) {return 0;}
static void opid_seed_locked(void) {if(!g_client.opid_next){g_client.opid_uuid[0]=29;g_client.opid_epoch=7;g_client.opid_next=1;}}
static int admits,submits,probes,admit_rc,transport_rc,stored,negative,bad_route,unknown_ok,unrouted_busy;
static struct efs_opid identity;
static int rpc_status_to_efs(uint8_t status) {
 if(status==EFS_INODE_RPC_OK)return EFS_OK;
 if(status==EFS_INODE_RPC_EXIST)return EFS_ERR_EXIST;
 return EFS_ERR_BUSY;
}
static int revoke_proof,revoke_rc;
int efs_client_rpc_identity_revoked(const struct efs_opid *id,int *out) {
 assert(id->session_epoch);*out=revoke_proof;return revoke_rc;
}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id) {
 assert(shard==121 && !memcmp(id,&identity,sizeof(identity)));admits++;return admit_rc;
}
static int rpc_send_recv_owner(efs_ino_t parent,uint8_t type,const void *body,uint32_t bytes,uint8_t expect,void *out,uint32_t out_bytes) {
 assert(parent==4097&&type==EFS_MSG_CREATE_RETAINED&&expect==EFS_MSG_CREATE_RETAINED_REPLY);
 assert(bytes==sizeof(struct efs_msg_create_retained)&&out_bytes==sizeof(struct efs_msg_create_retained_reply));
 const struct efs_msg_create_retained *r=body;
 assert(!memcmp(&r->q.id,&identity,sizeof(identity))&&r->q.ack<r->q.id.seq);
 assert(!strcmp(r->create.name,"new")&&r->create.mode==(S_IFREG|0600)&&r->create.uid==11&&r->create.gid==12&&r->create.flags==EFS_CREATE_F_PENDING);
 if(r->probe)probes++;else {submits++;assert(r->routed&&r->shard==121);}
 if(transport_rc)return transport_rc;
 struct efs_msg_create_retained_reply *reply=out;
 if(unrouted_busy){assert(r->probe);reply->rpc.status=EFS_INODE_RPC_BUSY;return EFS_OK;}
 reply->routed=1;reply->shard=bad_route?122:121;
 reply->known=stored;reply->rpc.status=stored?(negative?EFS_INODE_RPC_EXIST:EFS_INODE_RPC_OK):(unknown_ok&&!r->probe?EFS_INODE_RPC_OK:EFS_INODE_RPC_BUSY);
 if(stored&&!negative){reply->rpc.inode.ino=4217;reply->generation=1;}
 return EFS_OK;
}
'''+s[a:b]+s[c:d]+s[e:f]+r'''
int main(void) {
 g_client.export_id=1;
 struct efs_client_create_request r={0};
 assert(efs_client_create_begin(&r,4097,"new",S_IFDIR|0700,11,12)==EFS_ERR_INVAL);
 assert(!efs_client_create_begin(&r,4097,"new",S_IFREG|0600,11,12));identity=r.wire.q.id;
 assert(efs_client_create_reset(&r)==EFS_ERR_BUSY);
 /* A busy parent read-index can precede shard discovery. Keep the exact
  * identity/slot and do not admit or submit until routing is known. */
 unrouted_busy=1;
 for(int i=0;i<3;i++) {
  assert(efs_client_create_resume(&r,0)==EFS_ERR_BUSY&&!r.wire.routed&&!r.attempted&&!r.complete);
  assert(!admits&&!submits&&g_client.opid_inflight[r.slot]==identity.seq);
 }
 unrouted_busy=0;
 assert(efs_client_create_resume(&r,1)==EFS_ERR_BUSY&&r.wire.routed&&!admits&&!submits);
 admit_rc=EFS_ERR_NET;assert(efs_client_create_resume(&r,0)==EFS_ERR_NET&&!r.attempted&&!submits);
 assert(!efs_client_create_cancel(&r));
 assert(!efs_client_create_begin(&r,4097,"new",S_IFREG|0600,11,12));identity=r.wire.q.id;
 admit_rc=0;assert(efs_client_create_resume(&r,0)==EFS_ERR_BUSY&&r.attempted&&submits==1);
 assert(efs_client_create_cancel(&r)==EFS_ERR_BUSY&&g_client.opid_inflight[r.slot]);
 /* Submission OK is not a terminal receipt. Retain the original identity,
  * retry ownership and cancellation barrier until a known probe completes. */
 unknown_ok=1;
 assert(efs_client_create_resume(&r,0)==EFS_ERR_BUSY&&!r.complete&&r.slot>=0);
 assert(g_client.opid_inflight[r.slot]==identity.seq);
 assert(efs_client_create_cancel(&r)==EFS_ERR_BUSY);
 unknown_ok=0;
 int count=submits;stored=1;
 assert(!efs_client_create_resume(&r,0)&&r.complete&&r.slot==-1&&r.inode.ino==4217&&r.generation==1&&submits==count);
 assert(!efs_client_create_resume(&r,0)&&submits==count);assert(!efs_client_create_reset(&r));
 stored=0;
 assert(!efs_client_create_begin(&r,4097,"new",S_IFREG|0600,11,12));identity=r.wire.q.id;
 assert(efs_client_create_resume(&r,1)==EFS_ERR_BUSY);bad_route=1;
 assert(efs_client_create_resume(&r,0)==EFS_ERR_PROTO&&!r.complete&&r.wire.shard==121);
 bad_route=0;stored=1;negative=1;
 assert(efs_client_create_resume(&r,1)==EFS_ERR_EXIST&&r.complete&&!r.inode.ino&&r.slot==-1);
 assert(!efs_client_create_reset(&r));
 /* Exercise a revoked first touch after losing the local establishment cache. */
 memset(g_client.append_session_shards,0,sizeof(g_client.append_session_shards));
 stored=0;negative=0;admit_rc=EFS_ERR_STALE;
 assert(!efs_client_create_begin(&r,4097,"new",S_IFREG|0600,11,12));identity=r.wire.q.id;
 revoke_rc=EFS_ERR_NOT_FOUND;assert(efs_client_create_resume(&r,0)==EFS_ERR_STALE&&!r.complete);
 revoke_rc=0;revoke_proof=1;
 assert(efs_client_create_resume(&r,0)==EFS_ERR_STALE&&r.complete&&r.revoked&&r.slot==-1&&!r.attempted);
 assert(!efs_client_create_reset(&r));
 puts("retained CREATE: immutable body/identity, unrouted BUSY, pure probe, admission ambiguity, cancellation barrier, exact proof and pinned routing PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-request-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'src/sim/opid.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
