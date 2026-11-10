#!/usr/bin/env python3
"""Actual readdir RPC rejects over-counts, broken cursors and late replies."""
from pathlib import Path
import subprocess,tempfile
import csrc
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_rpc_readdir_cur(');b=s.index('\n}',a)+2
code=r'''
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int expired,late,fail,framing;static unsigned freed,calls;static uint32_t count,next_done;static int bad_name,stuck;
struct efs_conn{int unused;};static struct efs_conn connection;
int efs_client_rpc_past_deadline(void){return expired;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t s,efs_node_id_t*n){(void)s;*n=1;return &connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return 0;}
static void rpc_prof_add(int t,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)t;(void)a;(void)b;(void)c;(void)d;}
static int pauses;static int rpc_writer_retry_pause(unsigned attempt){(void)attempt;pauses++;return 0;}
static int rpc_status_to_efs(uint8_t s){return s==EFS_INODE_RPC_BUSY?EFS_ERR_BUSY:EFS_ERR_IO;}
void efs_readdir_trace(const char*w,uint32_t pe,uint64_t pa,uint32_t m,uint32_t as,const char*an,int rc,const struct efs_msg_inode_readdir_reply*r){(void)w;(void)pe;(void)pa;(void)m;(void)as;(void)an;(void)rc;(void)r;}
static void test_free(void*p){if(p)freed++;free(p);}
#define free test_free
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){(void)c;(void)p;assert(t==EFS_MSG_INODE_READDIR&&n==sizeof(struct efs_msg_inode_readdir));return 0;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){(void)c;calls++;struct efs_msg_inode_readdir_reply*r=calloc(1,sizeof(*r));r->status=EFS_INODE_RPC_OK;r->format=EFS_READDIR_FORMAT;r->count=count;r->next_done=next_done;r->next_src=stuck?0:2;if(bad_name)memset(r->next_name,'a',sizeof(r->next_name));r->ents[0].ino=123;r->ents[0].mode=0100644;strcpy(r->ents[0].name,"entry");
if(framing==1)r->format++;if(framing==4)memset(r->ents[0].name,'x',EFS_MAX_NAME);if(framing==5)r->ents[0].ino=0;if(framing==6)strcpy(r->ents[0].name,"a/b");if(framing==7)r->ents[0].reserved=1;if(framing==8)r->status=255;if(framing==9)r->next_src=65;if(framing==10)r->ents[0].mode=0;if(framing==11||(framing==12&&calls==1)){r->status=EFS_INODE_RPC_BUSY;r->count=0;}
*p=r;*t=EFS_MSG_INODE_READDIR_REPLY;*n=efs_readdir_reply_len(r);if(framing==2)(*n)--;if(framing==3)(*n)++;if(late)expired=1;return fail;}
'''+s[a:b]+r'''
static void reset(void){expired=late=fail=bad_name=stuck=framing=0;freed=calls=pauses=0;count=1;next_done=1;}
int main(void){struct efs_inode ents[2];uint32_t n,src,done;char name[EFS_MAX_NAME];
 for(int f=0;f<17;f++){reset();n=2;src=0;done=77;memset(name,0,sizeof(name));memset(ents,0x5a,sizeof(ents));
 if(f==0)count=EFS_READDIR_MAX+1;if(f==1)count=3;if(f==2)next_done=2;if(f==3)bad_name=1;if(f==4){stuck=1;next_done=0;}if(f==5)late=1;if(f==6)fail=1;if(f>=7)framing=f-6;
 int rc=efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done);assert(rc==(f==5?EFS_ERR_BUSY:f==6?EFS_ERR_NET:EFS_ERR_PROTO)&&freed==1&&n==2&&!src&&done==77&&ents[0].ino==UINT64_C(0x5a5a5a5a5a5a5a5a));}
 reset();n=2;src=0;assert(!efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)&&n==1&&src==2&&done==1&&ents[0].ino==123);
 reset();count=0;next_done=0;n=2;src=0;memset(name,0,sizeof(name));assert(!efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)&&!n&&!done&&src==2);
 reset();framing=12;n=2;src=0;done=77;memset(name,0,sizeof(name));
 assert(!efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)&&calls==2&&freed==2&&pauses==1&&n==1&&done==1&&ents[0].ino==123);
 reset();framing=11;n=2;src=0;done=77;memset(name,0,sizeof(name));memset(ents,0x5a,sizeof(ents));
 assert(efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)==EFS_ERR_BUSY&&calls==16&&freed==16&&pauses==15&&n==2&&done==77&&src==0&&ents[0].ino==UINT64_C(0x5a5a5a5a5a5a5a5a));
 reset();expired=1;n=2;assert(efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)==EFS_ERR_BUSY&&!calls);
 reset();n=0;assert(efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)==EFS_ERR_INVAL&&!calls);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-reply-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-misleading-indentation','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('readdir reply: count/cursor limits, output retention, late admission and failed buffer cleanup PASS')

# Execute the real peer forwarding receive boundary too. A malformed frame
# must not be zero-extended/copied into output or returned to the peer pool.
host=csrc.module('raft_host');a=host.index('static int host_inode_rpc_peer(');b=host.index('\n}',a)+2
forward=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
struct efs_raft_host {int raft_id;};
struct efs_conn {int unused;};static struct efs_conn connection;
static int fault,drops,releases;
static int peer_addr(struct efs_raft_host*h,int id,char*name,size_t cap,uint16_t*port){(void)h;(void)id;strncpy(name,"127.0.0.1",cap);*port=19001;return 0;}
static struct efs_conn *server_peer_conn_get(const char*h,uint16_t p){(void)h;(void)p;return &connection;}
static void server_peer_conn_drop(const char*h,uint16_t p,struct efs_conn*c){(void)h;(void)p;(void)c;drops++;}
static void server_peer_conn_release(const char*h,uint16_t p,struct efs_conn*c){(void)h;(void)p;(void)c;releases++;}
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t len){(void)c;(void)p;assert(t==EFS_MSG_INODE_READDIR&&len==sizeof(struct efs_msg_inode_readdir));return 0;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**buf,uint32_t*len){
 (void)c;struct efs_msg_inode_readdir_reply*r=calloc(1,sizeof(*r));
 r->format=EFS_READDIR_FORMAT;r->count=1;r->next_done=0;r->next_src=1;
 r->ents[0].ino=9;r->ents[0].mode=0100644;strcpy(r->ents[0].name,"file");
 if(fault==1)r->format++;if(fault==2)r->ents[0].reserved=1;
 if(fault==3)r->next_src=0;if(fault==4){r->count=2;r->ents[1]=r->ents[0];strcpy(r->ents[1].name,"other");}
 *buf=r;*len=efs_readdir_reply_len(r);*t=EFS_MSG_INODE_READDIR_REPLY;
 if(fault==5)(*len)++;if(fault==6)*t=EFS_MSG_INODE_GETATTR_REPLY;
 return 0;
}
'''+host[a:b]+r'''
int main(void){struct efs_raft_host h={0};struct efs_msg_inode_readdir q={0};
 struct efs_msg_inode_readdir_reply out,before;q.format=EFS_READDIR_FORMAT;q.max_ents=1;
 for(fault=1;fault<=6;fault++){
  memset(&out,0x5a,sizeof(out));before=out;drops=releases=0;
  assert(host_inode_rpc_peer(&h,1,EFS_MSG_INODE_READDIR,&q,sizeof(q),EFS_MSG_INODE_READDIR_REPLY,&out,sizeof(out))==-1);
  assert(drops==1&&!releases&&!memcmp(&out,&before,sizeof(out)));
 }
 fault=drops=releases=0;
 assert(!host_inode_rpc_peer(&h,1,EFS_MSG_INODE_READDIR,&q,sizeof(q),EFS_MSG_INODE_READDIR_REPLY,&out,sizeof(out)));
 assert(!drops&&releases==1&&out.count==1&&out.ents[0].ino==9&&out.ents[1].ino==0);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-forward-') as d:
 p=Path(d)/'t.c';p.write_text(forward)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-misleading-indentation','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('readdir peer forwarding: exact framing and request-relative cursor/count; reject before output/pool reuse PASS')
