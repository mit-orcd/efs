#!/usr/bin/env python3
"""Actual readdir RPC rejects over-counts, broken cursors and late replies."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_rpc_readdir_cur(');b=s.index('\n}',a)+2
code=r'''
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int expired,late,fail;static unsigned freed,calls;static uint32_t count,next_done;static int bad_name,stuck;
struct efs_conn{int unused;};static struct efs_conn connection;
int efs_client_rpc_past_deadline(void){return expired;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t s,efs_node_id_t*n){(void)s;*n=1;return &connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return 0;}
static void rpc_prof_add(int t,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)t;(void)a;(void)b;(void)c;(void)d;}
static int rpc_status_to_efs(uint8_t s){(void)s;return EFS_ERR_IO;}
static void test_free(void*p){if(p)freed++;free(p);}
#define free test_free
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){(void)c;(void)p;assert(t==EFS_MSG_INODE_READDIR&&n==sizeof(struct efs_msg_inode_readdir));return 0;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){(void)c;calls++;struct efs_msg_inode_readdir_reply*r=calloc(1,sizeof(*r));r->status=EFS_INODE_RPC_OK;r->count=count;r->next_done=next_done;r->next_src=stuck?0:2;if(bad_name)memset(r->next_name,'a',sizeof(r->next_name));r->ents[0].ino=123;*p=r;*t=EFS_MSG_INODE_READDIR_REPLY;*n=sizeof(*r);if(late)expired=1;return fail;}
'''+s[a:b]+r'''
static void reset(void){expired=late=fail=bad_name=stuck=0;freed=calls=0;count=1;next_done=1;}
int main(void){struct efs_inode ents[2];uint32_t n,src,done;char name[EFS_MAX_NAME];
 for(int f=0;f<7;f++){reset();n=2;src=0;done=77;memset(name,0,sizeof(name));memset(ents,0x5a,sizeof(ents));
 if(f==0)count=EFS_READDIR_MAX+1;if(f==1)count=3;if(f==2)next_done=2;if(f==3)bad_name=1;if(f==4){stuck=1;next_done=0;}if(f==5)late=1;if(f==6)fail=1;
 int rc=efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done);assert(rc==(f==5?EFS_ERR_BUSY:f==6?EFS_ERR_NET:EFS_ERR_PROTO)&&freed==1&&n==2&&!src&&done==77&&ents[0].ino==UINT64_C(0x5a5a5a5a5a5a5a5a));}
 reset();n=2;src=0;assert(!efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)&&n==1&&src==2&&done==1&&ents[0].ino==123);
 reset();count=0;next_done=0;n=2;src=0;memset(name,0,sizeof(name));assert(!efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)&&!n&&!done&&src==2);
 reset();expired=1;n=2;assert(efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)==EFS_ERR_BUSY&&!calls);
 reset();n=0;assert(efs_client_rpc_readdir_cur(1,42,ents,&n,&src,name,&done)==EFS_ERR_INVAL&&!calls);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-reply-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-misleading-indentation','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('readdir reply: count/cursor limits, output retention, late admission and failed buffer cleanup PASS')
