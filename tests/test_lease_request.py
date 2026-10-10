#!/usr/bin/env python3
"""Retained exact lease ownership pins shared ACK slots across ambiguity."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
a=s.index('static int opid_begin(');b=s.index('/* A reservation identity',a)
c=s.index('int efs_client_lease_begin(');d=s.index('static int rpc_put_ticket(',c)
e=s.index('static void opid_refresh_ack(');f=s.index('static void opid_retry_ack(',e)
h=s.index('static int session_shard_admit(uint32_t shard,const struct efs_opid_req *q)\n{');j=s.index('\n}',h)+2
code=r'''
#include "client_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
int efs_client_rpc_past_deadline(void) {return 0;}
static void opid_seed_locked(void) {if(!g_client.opid_next){g_client.opid_uuid[0]=17;g_client.opid_epoch=4;g_client.opid_next=1;}}
static int admits,calls,probe_rc,submit_rc,admit_rc;
static uint64_t probe_known,submit_known;
static struct efs_opid_req expected;
static int revoke_proof,revoke_rc;
int efs_client_rpc_identity_revoked(const struct efs_opid *id,int *out) {
 assert(id->session_epoch);*out=revoke_proof;return revoke_rc;
}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id) {
 assert(shard==1&&id->client_uuid[0]==17&&id->session_epoch==4);admits++;return admit_rc;
}
int efs_client_rpc_lease_edge(efs_ino_t ino,uint64_t gen,const struct efs_opid_req *q,unsigned flags,uint64_t token,int probe,uint64_t *known) {
 assert(ino==4097&&gen==99&&flags==EFS_LEASE_EDGE_CLOSE&&!token);
 assert(!memcmp(&q->id,&expected.id,sizeof(q->id)));assert(q->ack>=expected.ack&&q->ack<q->id.seq);calls++;
 *known=probe?probe_known:submit_known;return probe?probe_rc:submit_rc;
}
'''+s[a:b]+s[e:f]+s[h:j]+s[c:d]+r'''
int main(void) {
 struct efs_client_lease_request r={0};
 assert(efs_client_lease_begin(&r,4097,0,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_INVAL);
 assert(!efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_CLOSE,0));expected=r.q;
 assert(efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_BUSY);
 admit_rc=EFS_ERR_NET;assert(efs_client_lease_resume(&r,0)==EFS_ERR_NET&&!calls&&!r.attempted);
 assert(efs_client_lease_reset(&r)==EFS_ERR_BUSY&&g_client.opid_inflight[r.slot]);
 admit_rc=0;submit_rc=EFS_ERR_NET;
 assert(efs_client_lease_resume(&r,0)==EFS_ERR_NET&&r.attempted&&!r.complete);
 struct efs_opid_req later;int later_slot=opid_begin(&later);assert(later_slot>=0&&later.ack<r.q.id.seq);opid_end(later_slot);
 probe_rc=EFS_ERR_BUSY;
 assert(efs_client_lease_resume(&r,1)==EFS_ERR_BUSY&&!r.complete&&admits==2);
 submit_rc=EFS_OK;submit_known=1;
 assert(!efs_client_lease_resume(&r,0)&&r.complete&&r.slot==-1);
 int old_calls=calls;assert(!efs_client_lease_resume(&r,0)&&calls==old_calls);
 later_slot=opid_begin(&later);assert(later_slot>=0&&later.ack>=r.q.id.seq);opid_end(later_slot);
 assert(!efs_client_lease_reset(&r));
 assert(!efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_CLOSE,0));expected=r.q;
 submit_known=0;submit_rc=EFS_ERR_NET;assert(efs_client_lease_resume(&r,0)==EFS_ERR_NET);
 probe_known=1;probe_rc=EFS_ERR_STALE;
 assert(efs_client_lease_resume(&r,1)==EFS_ERR_STALE&&r.complete&&r.slot==-1);
 assert(efs_client_lease_resume(&r,0)==EFS_ERR_STALE);assert(!efs_client_lease_reset(&r));
 /* An older namespace operation resolves after begin: retry must advance
  * only ACK, never seq/UUID/epoch/FileID/body, to escape window pressure. */
 struct efs_opid_req blocker;int blocker_slot=opid_begin(&blocker);
 assert(blocker_slot>=0);
 assert(!efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_CLOSE,0));expected=r.q;
 assert(r.q.ack<blocker.id.seq);opid_end(blocker_slot);
 probe_known=0;probe_rc=EFS_ERR_BUSY;submit_known=1;submit_rc=EFS_OK;
 assert(!efs_client_lease_resume(&r,0)&&r.complete&&r.q.ack==r.q.id.seq-1);
 assert(!efs_client_lease_reset(&r));probe_known=1;probe_rc=EFS_ERR_STALE;
 assert(!efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_CLOSE,0));expected=r.q;
 probe_known=0;probe_rc=EFS_ERR_STALE;
 revoke_rc=EFS_ERR_NOT_FOUND;revoke_proof=0;
 assert(efs_client_lease_resume(&r,1)==EFS_ERR_STALE&&!r.complete);
 assert(efs_client_lease_reset(&r)==EFS_ERR_BUSY);
 revoke_rc=0;assert(efs_client_lease_resume(&r,1)==EFS_ERR_STALE&&!r.complete);
 revoke_proof=1;assert(efs_client_lease_resume(&r,1)==EFS_ERR_STALE&&r.complete&&r.revoked&&r.slot==-1);
 assert(!efs_client_lease_reset(&r));revoke_proof=0;probe_known=1;probe_rc=EFS_ERR_STALE;
 struct efs_client_lease_request all[EFS_OPID_INFLIGHT]={0};
 for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(!efs_client_lease_begin(&all[i],4097,99,EFS_LEASE_EDGE_CLOSE,0));
 assert(efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_BUSY&&!r.active);
 for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++) {expected=all[i].q;assert(efs_client_lease_resume(&all[i],1)==EFS_ERR_STALE);assert(!efs_client_lease_reset(&all[i]));}
 assert(admits==2); /* one failed first touch, then one successful establishment */
 puts("retained sequenced lease: immutable retries, unknown probe, pinned ACK, durable negative proof and capacity PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-request-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'src/sim/opid.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
