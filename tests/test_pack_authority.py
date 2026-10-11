#!/usr/bin/env python3
"""Actual authority envelope: fresh barriers, every FileID, strict forwarding."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include "efs/protocol.h"
#include "efs/packed_upload.h"
#include "efs/checksum.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_GET 0
struct fake_server {int storage_engine;};
struct efs_raft_host {int running,raft_id,n;struct fake_server *s;struct efs_kv *kv;};
static struct efs_raft_host *g_host;
struct efs_conn {int x;};static struct efs_conn conn;
static int hosted=1,reads,views,read_rc,local_rc,admit_rc,bad_view,phase=EFS_PACK_ACTIVE;
static int sends,drops,releases,corrupt,lost,state_bad,cap_unsupported;
static uint8_t sent_type;
static uint64_t salt=(1ull<<40)|((uint64_t)EFS_SESSION_ACTIVE<<32)|7;
static struct efs_pack_object object;
static uint8_t sent[32];static uint32_t sent_bytes;
void efs_hash(const void *data,size_t n,uint8_t out[EFS_HASH_SIZE])
{memset(out,0,EFS_HASH_SIZE);const uint8_t *p=data;for(size_t i=0;i<n;i++)out[i%EFS_HASH_SIZE]^=p[i];}
int efs_pack_upload_decode(const uint8_t *p,size_t n,struct efs_pack_object *o,struct efs_pack_ref *refs,uint32_t cap,uint32_t *count)
{if(n!=2 || !p[0] || p[0]>cap)return EFS_ERR_INVAL;*count=p[0];*o=object;for(uint32_t i=0;i<*count;i++)refs[i]=(struct efs_pack_ref){.ino=101+i*2,.inode_generation=3,.content_epoch=0};return 0;}
static void host_need_both(uint8_t *g){g[0]=0;g[1]=2;}
static int host_hosts_all(struct efs_raft_host *h,const uint8_t *g,int n)
{(void)h;assert(n==2 && g[0]==0 && g[1]==2);return hosted;}
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *g,int n,int skip)
{(void)h;assert(n==2 && g[0]==0 && g[1]==2);return skip<0?1:-1;}
static int peer_addr(struct efs_raft_host *h,int rid,char *a,size_t n,uint16_t *port)
{(void)h;assert(rid==1);snprintf(a,n,"peer");*port=1;return 0;}
static struct efs_conn *server_peer_conn_get(const char *a,uint16_t p){assert(!strcmp(a,"peer") && p==1);return &conn;}
static void server_peer_conn_drop(const char *a,uint16_t p,struct efs_conn *c){(void)a;(void)p;assert(c==&conn);drops++;}
static void server_peer_conn_release(const char *a,uint16_t p,struct efs_conn *c){(void)a;(void)p;assert(c==&conn);releases++;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n)
{assert(c==&conn && n==4);sent_type=type;sends++;
 if(type==EFS_MSG_PACK_CAPABILITIES){assert(efs_put_upload_rd32(p)==3);return 0;}
 assert(type==EFS_MSG_PACK_AUTHORITY);memcpy(sent,p,n);sent_bytes=n;return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **p,uint32_t *n)
{assert(c==&conn);if(lost)return EFS_ERR_NET;
 if(sent_type==EFS_MSG_PACK_CAPABILITIES){*type=EFS_MSG_PACK_CAPABILITIES_REPLY;*n=4;*p=calloc(1,4);efs_put_upload_wr32(*p,cap_unsupported?0:3);return 0;}
 struct efs_msg_pack_authority_reply r={.version=1,.group=0,.count=sent[2]};efs_hash(sent,sent_bytes,r.digest);
 if(corrupt==1)r.version++;if(corrupt==2)r.group=2;if(corrupt==3)r.count++;if(corrupt==4)r.digest[0]^=1;
 *type=corrupt==5?0:EFS_MSG_PACK_AUTHORITY_REPLY;*n=sizeof(r)+(corrupt==6?1:0)-(corrupt==7?1:0);*p=calloc(1,sizeof(r)+1);memcpy(*p,&r,sizeof(r));return 0;}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint)
{(void)h;(void)hint;assert(g==(reads%2?2:0));reads++;return read_rc;}
static void wr32be(uint8_t *p,uint32_t n){for(int i=0;i<4;i++)p[i]=n>>(24-8*i);}
uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN]){assert(uuid[0]==42);return 2;}
static int host_session_get_covered(struct efs_raft_host *h,const uint8_t *p,uint32_t n,uint8_t g,int *hint,uint64_t *out,uint8_t covered)
{(void)h;(void)hint;assert(reads==2 && n==22 && g==2 && covered==5 && p[2]==42 && p[21]==1);*out=salt;return 0;}
int efs_session_accept(struct efs_kv *kv,uint32_t shard,const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch)
{(void)kv;assert(shard==1 && uuid[0]==42 && epoch==7);return local_rc;}
int efs_pack_admission_get(struct efs_kv *kv,const struct efs_pack_object *request,struct efs_pack_object *out)
{(void)kv;assert(request->ledger.owner.client_uuid[0]==42);*out=*request;out->ledger.phase=phase;if(state_bad)out->ledger.state[15]=EFS_PACK_CANCELLED;return admit_rc;}
void server_raft_host_writer_view_reuse(const struct efs_msg_inode_writer_view *q,struct efs_msg_inode_writer_view_reply *r,uint8_t *covered)
{assert(*covered==5 && q->ino==(efs_ino_t)(101+views*2) && q->generation==3);views++;memset(r,0,sizeof(*r));r->ino=q->ino;r->generation=3;
 if(views==16){if(bad_view==1)r->oldest_complete_epoch=r->authority_epoch=1;if(bad_view==2)r->generation=4;if(bad_view==3)r->status=EFS_INODE_RPC_BUSY;if(bad_view==4)r->status=EFS_INODE_RPC_STALE;}}
'''+(root/'src/server/pack_authority.h').read_text()+r'''
static int run(uint8_t *q){reads=views=0;struct efs_msg_pack_authority_reply r;server_raft_host_pack_authority(q,4,&r);return r.rc;}
int main(void){struct fake_server server={0};struct efs_raft_host h={.running=1,.raft_id=0,.n=2,.s=&server};g_host=&h;
 object.ledger.owner.client_uuid[0]=42;object.ledger.owner.session_epoch=7;
 uint8_t q[]={1,0,16,99};assert(!run(q) && reads==2 && views==16);
 read_rc=EFS_ERR_BUSY;assert(run(q)==EFS_ERR_BUSY && reads==1 && !views);read_rc=0;
 salt++;assert(run(q)==EFS_ERR_STALE && !views);salt--;
 salt=(1ull<<40)|((uint64_t)EFS_SESSION_FENCING<<32)|7;assert(run(q)==EFS_ERR_BUSY && !views);
 salt=((uint64_t)EFS_SESSION_ACTIVE<<32)|7;assert(run(q)==EFS_ERR_BUSY && !views);salt|=1ull<<40;
 local_rc=EFS_ERR_STALE;assert(run(q)==EFS_ERR_STALE && !views);local_rc=0;
 admit_rc=EFS_ERR_PROTO;assert(run(q)==EFS_ERR_PROTO && !views);admit_rc=0;
 phase=EFS_PACK_RECLAIMING;assert(run(q)==EFS_ERR_STALE && !views);phase=EFS_PACK_ACTIVE;
 state_bad=1;assert(run(q)==EFS_ERR_STALE && views==15);state_bad=0;
 for(bad_view=1;bad_view<=4;bad_view++){int rc=run(q);assert(views==16 && rc==(bad_view==1 || bad_view==4?EFS_ERR_STALE:bad_view==2?EFS_ERR_PROTO:EFS_ERR_BUSY));}bad_view=0;
 hosted=0;assert(!run(q) && !reads && !views && sends==2 && releases==1);
 for(corrupt=1;corrupt<=7;corrupt++){assert(run(q)==EFS_ERR_PROTO && !reads && !views);}corrupt=0;
 lost=1;assert(run(q)==EFS_ERR_NET && drops==8);lost=0;
 cap_unsupported=1;int before=sends;assert(run(q)==EFS_ERR_PROTO && sends==before+1 && !reads && !views);
 cap_unsupported=0;
 q[2]=17;assert(run(q)==EFS_ERR_INVAL);q[2]=16;q[1]=2;assert(run(q)==EFS_ERR_INVAL);
 puts("pack authority: one fresh dual-group proof, all16 exact FileIDs/epochs, session/reclamation refusal, strict whole-cohort forwarding PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-authority-') as t:
    p=Path(t);(p/'test.c').write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
