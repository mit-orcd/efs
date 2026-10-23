#!/usr/bin/env python3
"""Execute the K13 mount ticket ledger: admission before PUT, ambiguity
retention, FileID binding, sweep of unreferenced objects and ordered,
batched settle per lane-shard stream."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "client_internal.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_client g_client;
static uint64_t next_seq=1;static unsigned gen_rpcs,admits,sessions,settles;
static int admit_rc,admit_lost,session_rc,unregistered,settle_rc,gen_rc;
static uint32_t settle_cap=EFS_PUT_TICKET_SETTLE_MAX;
static uint64_t settled[64];static unsigned nsettled;
static uint64_t referenced_object;
static pthread_mutex_t mock_mu=PTHREAD_MUTEX_INITIALIZER;
int efs_client_rpc_past_deadline(void){return 0;}
static uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_put_id(struct efs_opid *out){memset(out,0,sizeof(*out));out->client_uuid[0]=9;out->session_epoch=1;out->seq=next_seq++;return EFS_OK;}
int efs_client_rpc_getchunks_fileid(efs_export_id_t e,efs_ino_t ino,uint64_t g,uint32_t start,struct efs_chunk_rec *r,uint32_t *n,uint64_t *resolved)
{(void)e;(void)g;(void)start;(void)r;(void)n;gen_rpcs++;*resolved=ino==4097?7:0;return gen_rc;}
int efs_client_rpc_put_ticket(const struct efs_put_ticket *t,int probe,uint32_t *state)
{
 assert(!probe);*state=0;struct efs_meta_pub p={0};(void)p;
 /* The admitted body is exactly the immutable PUT identity. */
 assert(t->inode_gen==7 && t->member_mask==15 && t->fragment_len==32 && t->coding_profile==EFS_META_PROFILE_K2F1);
 assert(t->object==efs_meta_candidate_gen(t->id.client_uuid,1,t->id.seq,t->ci,0));
 admits++;
 if(unregistered){unregistered--;return EFS_ERR_UNREGISTERED;}
 if(admit_lost)return EFS_ERR_NET;
 if(admit_rc)return admit_rc;
 *state=EFS_PUT_TICKET_ADMITTED;return EFS_OK;
}
int efs_client_session_admit(uint32_t shard,const struct efs_opid *id){(void)shard;assert(id->seq);sessions++;return session_rc;}
int efs_client_rpc_put_ticket_settle(const struct efs_put_ticket *t,uint32_t n,uint32_t *retired)
{
 pthread_mutex_lock(&mock_mu);
 settles++;*retired=0;
 uint32_t shard=efs_kv_lane_shard(t[0].ino,t[0].ci%EFS_META_LANES);
 for(uint32_t i=0;i<n;i++){
  assert(efs_kv_lane_shard(t[i].ino,t[i].ci%EFS_META_LANES)==shard);
  if(i)assert(t[i].id.seq>t[i-1].id.seq);
 }
 int rc=settle_rc;
 if(!rc){*retired=n<settle_cap?n:settle_cap;for(uint32_t i=0;i<*retired;i++)settled[nsettled++]=t[i].id.seq;}
 pthread_mutex_unlock(&mock_mu);
 return rc;
}
int efs_client_put_referenced(efs_ino_t ino,uint32_t ci,uint64_t object){(void)ino;(void)ci;return object==referenced_object;}
#include "put_ticket_owner.c"
static const efs_node_id_t nodes[3]={1,2,3};
static uint8_t cks[3][EFS_HASH_SIZE];
static int admit(uint32_t ci,struct efs_put_ticket *t){return efs_client_put_admit(4097,ci,nodes,(const uint8_t (*)[EFS_HASH_SIZE])cks,32,0,0,t);}
int main(void){
 g_client.node_count=4;g_client.export_id=1;for(unsigned i=0;i<4;i++)g_client.nodes[i].id=i+1;
 for(unsigned i=0;i<3;i++)memset(cks[i],i+1,EFS_HASH_SIZE);
 struct efs_put_ticket a,b,c,d,e;
 /* A deleted or unresolvable file admits nothing and records nothing. */
 assert(efs_client_put_admit(4098,0,nodes,(const uint8_t (*)[EFS_HASH_SIZE])cks,32,0,0,&a)==EFS_ERR_PROTO);
 assert(!efs_client_put_outstanding());
 gen_rc=EFS_ERR_NOT_FOUND;assert(admit(0,&a)==EFS_ERR_NOT_FOUND && !efs_client_put_outstanding());gen_rc=0;
 /* Admission: one FileID lookup per inode; the lane shard admits the
  * session before ADMIT, and again after an UNREGISTERED refusal. */
 unregistered=1;assert(!admit(0,&a) && admits==2 && sessions==2);
 /* A ticket never offered to a server (session refused) needs no settle. */
 session_rc=EFS_ERR_FENCED;assert(admit(5,&e)==EFS_ERR_FENCED && admits==2 && efs_client_put_outstanding()==1);session_rc=0;
 assert(!admit(1,&b));assert(gen_rpcs==3);
 struct efs_meta_pub p={.ino=4097,.inode_gen=7,.chunk_index=0,.coding_profile_id=EFS_META_PROFILE_K2F1,.fresh_object=1,.ticketed=1,.put_fragment_len=32,.put_member_mask=15};
 p.put_id=a.id;p.publication_id=a.id;p.candidate_gen=a.object;memcpy(p.ch.nodes,nodes,sizeof(nodes));memcpy(p.ch.checksums,cks,sizeof(cks));
 struct efs_put_ticket want;assert(!efs_put_ticket_from_pub(&p,&want) && !memcmp(&want,&a,sizeof(a)));
 /* A lost admission reply is retained and settled later, never forgotten. */
 admit_lost=1;assert(admit(0,&c)==EFS_ERR_NET);admit_lost=0;
 assert(efs_client_put_outstanding()==3);
 /* Nothing is settled while PUTs are in flight ("putting"). */
 assert(!efs_client_put_settle());assert(!settles);
 /* a landed and is still named by a table; b landed, then was superseded. */
 referenced_object=a.object;
 efs_client_put_release(a.id.seq,1);efs_client_put_release(b.id.seq,1);
 efs_client_put_done(4097,1,b.object);
 /* Streams: ci 0 (a, c) and ci 1 (b) are different lanes. a is live and
  * heads its stream, so c waits; b settles alone. */
 assert(efs_kv_lane_shard(4097,0)!=efs_kv_lane_shard(4097,1));
 assert(!efs_client_put_settle());
 assert(nsettled==1 && settled[0]==b.id.seq && efs_client_put_outstanding()==2);
 /* The table drops a (REPORT confirmed): sweep marks it done, then a and c
  * settle in one ordered batch. */
 referenced_object=0;
 assert(!efs_client_put_settle());
 assert(nsettled==3 && settled[1]==a.id.seq && settled[2]==c.id.seq && !efs_client_put_outstanding());
 /* A partial retirement keeps the remainder, in order, for the next pass. */
 assert(!admit(2,&d) && !admit(2,&e));
 efs_client_put_release(d.id.seq,0);efs_client_put_release(e.id.seq,0);
 settle_cap=1;unsigned before=settles;
 assert(!efs_client_put_settle());
 assert(nsettled==5 && settled[3]==d.id.seq && settled[4]==e.id.seq && settles==before+2);
 settle_cap=EFS_PUT_TICKET_SETTLE_MAX;
 /* A refused settle (fenced mount) keeps the ticket for the collector. */
 assert(!admit(3,&a));efs_client_put_release(a.id.seq,0);
 settle_rc=EFS_ERR_FENCED;assert(efs_client_put_settle()==EFS_ERR_FENCED && efs_client_put_outstanding()==1);
 settle_rc=0;assert(!efs_client_put_settle() && !efs_client_put_outstanding());
 /* A stale admission is released as done, then settled (abandoned). */
 admit_rc=EFS_ERR_STALE;assert(admit(4,&a)==EFS_ERR_STALE && efs_client_put_outstanding()==1);admit_rc=0;
 assert(!efs_client_put_settle() && !efs_client_put_outstanding());
 efs_client_put_settle_stop();
 puts("K13 ticket ledger: FileID binding, admission before PUT, lost/stale admission retention, putting protection, sweep, ordered per-stream batched settle, partial and fenced retirement PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-owner-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-pthread','-lm','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
