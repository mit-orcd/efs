#!/usr/bin/env python3
"""Production CREATE wrapper and retry loop preserve identity and real errors.
Network outcomes and the identity allocator are mocked; request packing,
CREATE construction, routing retry loop and status mapping are production code.
"""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/client/inode_rpc.c').read_text()
def function(name):
    m = re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M)
    if not m: raise RuntimeError(name)
    return s[m.start():s.index('\n}',m.end())+2]
code = r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "metadata_read_route.h"
#include "efs/opid.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
struct efs_conn {int fd;};
static struct efs_conn connection;
static struct {uint64_t flock_token;unsigned node_count;} g_client={.node_count=4};
static struct efs_meta_read_routes g_read_routes={.mu=PTHREAD_MUTEX_INITIALIZER};
static int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
static void rpc_learn_leader(const struct efs_msg_inode_reply*r){(void)r;}
static unsigned g_group_leader[2];
static unsigned mode,sends,begins,ends,pauses;
static unsigned char original[2048];static uint32_t original_len;
static int active;
static void opid_retry_ack(void *packet,uint32_t length){(void)packet;(void)length;}
static int opid_begin(struct efs_opid_req *q) {
 assert(!active);active=1;begins++;memset(q,0,sizeof(*q));
 q->id.client_uuid[0]=19;q->id.session_epoch=3;q->id.seq=begins;return 7;
}
static void opid_end(int slot){assert(slot==7&&active);active=0;ends++;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return 0;}
static void rpc_prof_add(unsigned t,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)t;(void)a;(void)b;(void)c;(void)d;}
static void rpc_note_slow_ok(unsigned t,int a,int b,unsigned st,unsigned long long start){(void)t;(void)a;(void)b;(void)st;(void)start;}
static int efs_client_rpc_past_deadline(void){return 0;}
static unsigned session_checks;
static int efs_client_session_check(void){session_checks++;return EFS_OK;} /* K5 */
static void efs_client_session_fence(int rc){(void)rc;assert(0);}
static uint64_t efs_client_rpc_deadline_ms(void){return 0;}
static int rpc_writer_retry_pause(unsigned n){(void)n;pauses++;return EFS_OK;}
static void efs_client_node_note_fail(efs_node_id_t n){(void)n;}
static void efs_client_node_note_ok(efs_node_id_t n){(void)n;}
static unsigned rpc_leader_of(unsigned g){return g_group_leader[g>>1];}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n==2);return &connection;}
static struct efs_conn *raft_voter_conn_skip(uint32_t sh,efs_node_id_t *n,efs_node_id_t skip){(void)sh;(void)skip;*n=1;return &connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;assert(c==&connection);}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;assert(c==&connection);}
uint32_t efs_export_shard_of(efs_ino_t ino,uint32_t bits){return ino&((1u<<bits)-1);}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *req,uint32_t len){
 assert(c==&connection&&type==EFS_MSG_INODE_CREATE&&active);
 assert(len==sizeof(struct efs_msg_inode_create)+EFS_OPID_WIRE_LEN);
 if(!sends){assert(len<=sizeof(original));memcpy(original,req,len);original_len=len;}
 else assert(len==original_len&&!memcmp(original,req,len));
 sends++;return mode==0&&sends==1?-1:0;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **payload,uint32_t *len){
 assert(c==&connection&&active);
 if(mode==1&&sends==1)return -1;
 struct efs_msg_inode_reply *r=calloc(1,sizeof(*r));assert(r);
 *payload=r;*len=sizeof(*r);*type=EFS_MSG_INODE_CREATE_REPLY;
 r->inode.ino=42;r->inode.parent=1;strcpy(r->inode.name,"unique");
 if(mode==4)r->status=EFS_INODE_RPC_EXIST;
 else if(mode==5||(mode==2&&sends==1))r->status=EFS_INODE_RPC_BUSY;
 else if(mode==3&&sends==1){r->status=EFS_INODE_RPC_NOT_PRIMARY;r->primary_id=2;}
 return 0;
}
'''
code += r'''
static __thread uint64_t rpc_tl_sent;static __thread unsigned rpc_tl_lost; /* W155 */
static void efs_client_rpc_set_deadline_ms(uint64_t d){(void)d;}
'''
code += '\n'.join(function(n) for n in ['rpc_status_to_efs','stale_retryable','opid_suffix','rpc_retry_peer','rpc_type_mutates','rpc_identified','rpc_req_ident','rpc_resolve_ms','rpc_send_recv_shard_once'])
code += r'''
static int rpc_send_recv_dual_once(uint8_t type,const void *req,uint32_t len,uint8_t expect,void *reply,uint32_t size,int unused){(void)unused;return rpc_send_recv_shard_once(1,type,req,len,expect,reply,size,0,NULL);}
'''
code += '\n'.join(function(n) for n in ['rpc_resolve_identified','rpc_send_recv_shard','rpc_send_recv_owner'])
code += r'''
static int rpc_send_recv_dual(uint8_t type,const void *req,uint32_t len,uint8_t expect,void *reply,uint32_t size,int unused){(void)unused;return rpc_send_recv_shard(1,type,req,len,expect,reply,size,0,NULL);}
'''+function('efs_client_rpc_create')+r'''
int main(void){
 for(mode=0;mode<6;mode++)for(unsigned directory=0;directory<2;directory++){
  sends=pauses=0;unsigned before=begins;efs_ino_t ino=0;struct efs_inode row={0};
  int rc=efs_client_rpc_create(1,1,"unique",(directory?S_IFDIR:S_IFREG)|0600,0,0,0,&ino,&row);
  assert(begins==before+1&&ends==begins&&!active);
  if(mode==4)assert(rc==EFS_ERR_EXIST&&sends==1&&ino==0);
  else if(mode==5)assert(rc==EFS_ERR_BUSY&&sends==16&&pauses==15&&ino==0);
  else assert(rc==EFS_OK&&sends==2&&ino==42&&row.ino==42);
 }
 puts("CREATE retry identity: send/recv loss, BUSY, leader redirect, real EEXIST and exhausted BUSY for files/directories PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-retry-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-libverbs','-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
