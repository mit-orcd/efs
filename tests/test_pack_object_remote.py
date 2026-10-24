#!/usr/bin/env python3
"""K28/W151 (D38): a segment node that hosts no replica of the segment's
group reads the pack object from a voter. Only a hosting voter's answer
after its ReadIndex counts; every transport error, refusal, wrong echo or
bad object ends in BUSY (PUT refused, reclaim kept BUSY), never in
NOT_FOUND. The serving side refuses unless it hosts the group, and returns
NOT_FOUND only after a successful ReadIndex."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static uint32_t host_pack_degraded_mask(')
b=s.index('\n}',s.index('int server_raft_host_pack_object(uint32_t group'))+2
code=r'''
#include "src/server/server_internal.h"
#include "efs/pack_mapping.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_raft_host {int raft_id,running;struct efs_kv *kv;};
static struct efs_raft_host *g_host;
static uint32_t voters=0xeu;            /* nodes 2,3,4 = raft ids 1,2,3 */
static int leader=2,hosted,read_rc,get_rc,calls,asked[EFS_RAFT_MAX_PEERS];
static int fail[EFS_RAFT_MAX_PEERS],refuse[EFS_RAFT_MAX_PEERS],missing[EFS_RAFT_MAX_PEERS];
static int bad_echo[EFS_RAFT_MAX_PEERS],bad_object[EFS_RAFT_MAX_PEERS],reads;
static int hosts_group(int rid,uint32_t v){return rid>=0&&rid<EFS_RAFT_MAX_PEERS&&(v>>rid&1u);}
static uint32_t host_route_voters(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return voters;}
static int host_fwd_leader_pick(struct efs_raft_host *h,uint8_t g,int skip){(void)h;(void)g;(void)skip;return leader;}
static int host_hosts(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return hosted;}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){(void)h;(void)g;*hint=-1;reads++;return read_rc;}
uint32_t efs_pack_group_anchor(uint32_t g){return g==0?1:g==2?2:0;}
static int mark_rc=EFS_ERR_NOT_FOUND;static uint8_t mark_mask;
int efs_pack_degraded_get(struct efs_kv *kv,uint32_t g,uint64_t id,uint64_t gen,uint8_t *m,uint8_t *why){
 (void)kv;(void)g;(void)id;(void)gen;(void)why;if(mark_rc==EFS_OK)*m=mark_mask;return mark_rc;}
static uint32_t remote_degraded;
int efs_pack_object_get(struct efs_kv *kv,uint32_t g,uint64_t id,uint64_t gen,struct efs_pack_object *o){
 (void)kv;if(get_rc)return get_rc;
 memset(o,0,sizeof(*o));
 o->ledger.segment_id=id;o->ledger.segment_generation=gen;o->ledger.publication_group=g;o->nodes[0]=5;return EFS_OK;}
int efs_pack_object_encode(const struct efs_pack_object *o,uint8_t *p,size_t cap,size_t *len){
 assert(cap>=24);memcpy(p,&o->ledger.segment_id,8);memcpy(p+8,&o->ledger.segment_generation,8);
 memcpy(p+16,&o->ledger.publication_group,4);memcpy(p+20,&o->nodes[0],4);*len=24;return EFS_OK;}
int efs_pack_object_decode(const uint8_t *p,size_t len,struct efs_pack_object *o){
 if(len!=24)return EFS_ERR_PROTO;
 memset(o,0,sizeof(*o));memcpy(&o->ledger.segment_id,p,8);
 memcpy(&o->ledger.segment_generation,p+8,8);memcpy(&o->ledger.publication_group,p+16,4);memcpy(&o->nodes[0],p+20,4);return EFS_OK;}
static int host_inode_rpc_peer(struct efs_raft_host *h,int rid,uint8_t type,const void *body,uint32_t bytes,
                               uint8_t expected,void *out,uint32_t out_bytes){
 assert(h==g_host&&rid!=h->raft_id&&hosts_group(rid,voters));
 assert(type==EFS_MSG_PACK_OBJECT_READ&&expected==EFS_MSG_PACK_OBJECT_READ_REPLY);
 assert(bytes==sizeof(struct efs_msg_pack_object_read)&&out_bytes==sizeof(struct efs_msg_pack_object_read_reply));
 calls++;asked[rid]++;
 if(fail[rid])return -1;
 const struct efs_msg_pack_object_read *q=body;struct efs_msg_pack_object_read_reply *r=out;
 assert(!q->reserved);
 r->group=q->group;r->id=q->id;r->generation=q->generation;
 if(bad_echo[rid])r->id++;
 if(refuse[rid]){r->rc=EFS_ERR_NOT_PRIMARY;return 0;}
 if(missing[rid]){r->rc=EFS_ERR_NOT_FOUND;return 0;}
 struct efs_pack_object o;memset(&o,0,sizeof(o));
 o.ledger.segment_id=q->id+(bad_object[rid]?1:0);o.ledger.segment_generation=q->generation;
 o.ledger.publication_group=q->group;o.nodes[0]=5;
 size_t len;efs_pack_object_encode(&o,r->object,sizeof(r->object),&len);r->len=(uint32_t)len;r->rc=EFS_OK;
 r->degraded=remote_degraded;return 0;}
static void reset(void){calls=0;memset(asked,0,sizeof(asked));memset(fail,0,sizeof(fail));memset(refuse,0,sizeof(refuse));
 memset(missing,0,sizeof(missing));memset(bad_echo,0,sizeof(bad_echo));memset(bad_object,0,sizeof(bad_object));}
'''+s[a:b]+r'''
int main(void){
 struct efs_kv *kv=(struct efs_kv *)(uintptr_t)1;
 struct efs_raft_host host={.raft_id=4,.running=1,.kv=kv};g_host=&host; /* node 5: no metadata */
 struct efs_pack_object o;
 /* Leader answers: one RPC, object identity checked. */
 reset();assert(server_raft_host_pack_object(2,77,3,&o)==EFS_OK&&calls==1&&asked[2]==1);
 assert(o.ledger.segment_id==77&&o.ledger.segment_generation==3&&o.ledger.publication_group==2&&o.nodes[0]==5);
 /* Leader unreachable: another voter answers. Non-voters are never asked. */
 reset();fail[2]=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_OK&&asked[2]==1&&asked[1]==1&&!asked[0]&&!asked[4]);
 /* NOT_FOUND only from a voter's own answer, after others refused. */
 reset();refuse[2]=refuse[1]=1;missing[3]=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_NOT_FOUND);
 /* Every failure mode ends in BUSY, never NOT_FOUND. */
 reset();fail[1]=fail[2]=fail[3]=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_BUSY&&calls==3);
 reset();refuse[1]=refuse[2]=refuse[3]=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_BUSY);
 reset();bad_echo[1]=bad_echo[2]=bad_echo[3]=1;missing[1]=missing[2]=missing[3]=1;
 assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_BUSY);
 reset();bad_object[1]=bad_object[2]=bad_object[3]=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_BUSY);
 /* A stale leader hint outside the voters is skipped; each voter asked once. */
 reset();leader=4;fail[1]=fail[2]=fail[3]=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_BUSY&&calls==3&&!asked[4]);
 leader=2;
 /* Voters of the other group are not asked for this one. */
 reset();voters=0x7u;fail[0]=fail[1]=fail[2]=1;assert(server_raft_host_pack_object(0,9,1,&o)==EFS_ERR_BUSY&&calls==3&&!asked[3]);
 voters=0xeu;
 /* K37: the degraded mask rides the remote answer; refusals leave it BAD. */
 uint32_t dg=0;
 reset();remote_degraded=4;assert(server_raft_host_pack_state(2,77,3,&o,&dg)==EFS_OK&&dg==4);
 reset();fail[1]=fail[2]=fail[3]=1;assert(server_raft_host_pack_state(2,77,3,&o,&dg)==EFS_ERR_BUSY&&dg==EFS_PACK_READ_DEGRADED_BAD);
 remote_degraded=0;
 /* A hosting node reads locally: ReadIndex then the KV, no RPC. */
 reset();hosted=1;reads=0;host.raft_id=1;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_OK&&!calls&&reads==1);
 read_rc=EFS_ERR_BUSY;assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_BUSY&&!calls);read_rc=0;
 /* K37 local read: unmarked 0, the mark's mask, an unreadable mark BAD. */
 assert(server_raft_host_pack_state(2,77,3,&o,&dg)==EFS_OK&&dg==0);
 mark_rc=EFS_OK;mark_mask=2;assert(server_raft_host_pack_state(2,77,3,&o,&dg)==EFS_OK&&dg==2);
 mark_rc=EFS_ERR_PROTO;assert(server_raft_host_pack_state(2,77,3,&o,&dg)==EFS_OK&&dg==EFS_PACK_READ_DEGRADED_BAD);
 mark_rc=EFS_ERR_NOT_FOUND;
 /* Serving side. */
 struct efs_msg_pack_object_read q={.group=2,.id=77,.generation=3};
 struct efs_msg_pack_object_read_reply *r=calloc(1,sizeof(*r));assert(r);
 hosted=0;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_NOT_PRIMARY&&!r->len&&r->id==77);
 hosted=1;reads=0;read_rc=EFS_ERR_STALE;get_rc=EFS_ERR_NOT_FOUND;
 server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_STALE&&reads==1&&!r->len);
 read_rc=0;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_NOT_FOUND&&!r->len);
 get_rc=0;server_raft_host_pack_object_serve(&q,r);
 assert(r->rc==EFS_OK&&r->len==24&&r->group==2&&r->id==77&&r->generation==3&&!r->degraded);
 mark_rc=EFS_OK;mark_mask=5;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_OK&&r->degraded==5);
 mark_rc=EFS_ERR_NOT_FOUND;
 q.reserved=1;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_INVAL);q.reserved=0;
 q.group=1;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_INVAL);q.group=2;
 q.id=0;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_INVAL);
 host.running=0;q.id=77;server_raft_host_pack_object_serve(&q,r);assert(r->rc==EFS_ERR_NOT_PRIMARY);
 assert(server_raft_host_pack_object(2,77,3,&o)==EFS_ERR_NOT_PRIMARY);
 free(r);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-object-remote-') as directory:
    p=Path(directory)/'t.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root),'-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('pack object remote read: leader then voters, hosting-voter ReadIndex only, every failure BUSY (never NOT_FOUND), local path unchanged, K37 degraded mask beside the object PASS')
