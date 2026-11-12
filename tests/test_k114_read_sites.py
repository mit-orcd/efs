#!/usr/bin/env python3
"""K114 fix 2: the read-only receive sites that call the reply-wait hook (unit part; the live part is test_k114_read_sites_live.py).

The production text of efs_client_rpc_getchunks_fileid and query_snapshot_rpc (src/client/inode_rpc.c) is compiled with
stubbed transport: the member is silent (every receive fails, as after a cut), and the stub records each call.
Usage: test_k114_read_sites.py getchunks|query|pinned|report|mkdir|bounded (one scenario each):
 getchunks  the hook runs once per attempt, on the connection just sent to and before its receive;
 query      an unpinned query snapshot calls the hook before its receive, then repeats on another voter (skip = failed node);
 pinned     a query pinned to a token's node is not cut (its failed receive is returned, not repeated);
 report     K114 fix 3: a REPORT through rpc_send_recv_dual_once never calls the hook (K122), run by behaviour with type 67 as
            the real caller sends it and with other lengths and bounds; the readdir, writer-view, lane-writer-view,
            publication and receipt-query receives stay without it (text; their failure is returned to the caller, no repeat on
            another member: docs/status/2026-11/k114-fix2-read-sites-20261111.md);
 mkdir      K114 fix 3: an identified INODE_CREATE (a directory) through rpc_send_recv_dual_once calls the hook once per attempt,
            and after the cut repeats on the other dual voter with the same op-id bytes;
 bounded    K114 fix 3: the same request with a bound of its own (recv_ms > 0) is not cut (the probe must not outlast it)."""
from pathlib import Path
import os,shlex,subprocess,sys,tempfile
sys.path.insert(0,str(Path(__file__).resolve().parent));from sanitize_env import cflags,wrap
case=sys.argv[1];assert case in('getchunks','query','pinned','report','mkdir','bounded'),case
root=Path(__file__).resolve().parents[1];r=(root/'src/client/inode_rpc.c').read_text()
def cut(t,x,y):
 """text from the definition of x (not a prototype or a call) to the next y"""
 i=t.index(x)
 while t.index('{',i)>t.index(';',i)or t[t.rindex('\n',0,i)+1:i].strip():i=t.index(x,i+1)
 return t[i:t.index(y,i)]
if case=='report':
 for name,end in(('int efs_client_rpc_readdir_cur(','int efs_client_rpc_writer_view('),
   ('int efs_client_rpc_writer_view(','int efs_client_rpc_publication_session('),('int efs_client_rpc_publication(','int efs_client_rpc_lane_writer_view('),
   ('static int rpc_report_receipt_query(','/* REPORT rcs that say nothing')):
  assert 'efs_client_conn_await_reply' not in cut(r,name,end),name
CC=shlex.split(os.environ.get('CC','cc'))
DUAL=case in('report','mkdir','bounded')
def build(code):
 with tempfile.TemporaryDirectory(prefix='efs-k114-read-sites-') as d:
  p=Path(d)/'t.c';p.write_text(code)
  subprocess.run(CC+['-std=gnu11','-pthread']+cflags()+['-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
  q=subprocess.run(wrap()+[str(p.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=90)
  print(q.stdout,end='');assert q.returncode==0,(q.returncode,q.stderr)
if DUAL:
 dual=(cut(r,'static int rpc_identified(','/* G1/G4')+cut(r,'static int rpc_type_mutates(','static int rpc_send_recv_shard_once(')+
       cut(r,'static int rpc_send_recv_dual_once(','static int rpc_send_recv_dual('))
 # one send/receive attempt per voter, every receive fails (the member is silent); a call runs two attempts, then the deadline ends it
 calls={'report':'''
 /* REPORT as the caller sends it (recv_ms sized to the record count) and with the default bound, a short and a long request */
 static const struct{uint32_t len;int ms;}cfg[]={{200,0},{200,30000},{sizeof req,0},{sizeof req,45000}};
 for(unsigned i=0;i<4;i++){reset();
  int rc=rpc_send_recv_dual_once(EFS_MSG_REPORT_CHUNKS,req,cfg[i].len,EFS_MSG_REPORT_CHUNKS_REPLY,&rep,sizeof rep,cfg[i].ms);
  assert(rc==EFS_ERR_BUSY);expect("srdsrd");assert(hook_calls==0);}''',
  'mkdir':'''
 reset();
 int rc=rpc_send_recv_dual_once(EFS_MSG_INODE_CREATE,req,idlen,EFS_MSG_INODE_CREATE_REPLY,&rep,sizeof rep,0);
 assert(rc==EFS_ERR_BUSY);expect("shrdshrd");assert(hook_calls==2);
 assert(npick==2&&picked[0]==1&&picked[1]==2);              /* the repeat goes to the other dual voter */
 assert(nsent==2&&sent_len[0]==idlen&&sent_len[1]==idlen&&!memcmp(sent[0],sent[1],idlen)); /* same op-id bytes */
 /* a create without an op-id suffix is not replay-safe: never cut */
 reset();rc=rpc_send_recv_dual_once(EFS_MSG_INODE_CREATE,req,idlen-EFS_OPID_WIRE_LEN,EFS_MSG_INODE_CREATE_REPLY,&rep,sizeof rep,0);
 assert(rc==EFS_ERR_BUSY);expect("srdsrd");assert(hook_calls==0);''',
  'bounded':'''
 reset();
 int rc=rpc_send_recv_dual_once(EFS_MSG_INODE_CREATE,req,idlen,EFS_MSG_INODE_CREATE_REPLY,&rep,sizeof rep,2000);
 assert(rc==EFS_ERR_BUSY);expect("srdsrd");assert(hook_calls==0);'''}[case]
 build(r'''
#include "client_internal.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int hook_calls, order[64], on, past, deadline_after=4, picked[8], npick, down[3], nsent;
static uint32_t sent_len[8];static uint8_t sent[8][1024];
static __thread uint64_t rpc_tl_sent;static __thread unsigned rpc_tl_lost;
static struct efs_conn c1, c2;
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return 0;}
static void rpc_prof_add(int t,unsigned long long a,unsigned long long b,unsigned long long c,int d){(void)t;(void)a;(void)b;(void)c;(void)d;}
static void rpc_note_slow_ok(uint8_t t,int a,int b,int s,unsigned long long t0){(void)t;(void)a;(void)b;(void)s;(void)t0;}
static const char *rpc_req_ident(uint8_t t,const void *q,uint32_t l,char *b,size_t cap){(void)t;(void)q;(void)l;(void)cap;b[0]='-';b[1]=0;return b;}
static int rpc_writer_retry_pause(unsigned a){(void)a;return EFS_OK;}
static void opid_retry_ack(void *p,uint32_t l){(void)p;(void)l;}
static int stale_retryable(uint8_t t){(void)t;return 0;}
static int rpc_past_deadline(void){return ++past>deadline_after;}
int efs_client_rpc_past_deadline(void){return rpc_past_deadline();}
uint64_t efs_client_rpc_deadline_ms(void){return 1;}
int efs_client_session_check(void){return EFS_OK;}
struct efs_conn *efs_client_conn_get(efs_node_id_t n){(void)n;return &c2;}
/* like the real picker: the first voter not marked down (the hook marks a silent member down); a plain failure marks nothing */
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t *n,efs_node_id_t skip){(void)skip;*n=down[1]?2:1;picked[npick++]=(int)*n;return *n==1?&c1:&c2;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t t,const void *b,uint32_t l){(void)c;(void)t;order[on++]='s';
 if(nsent<8&&l<=sizeof sent[0]){memcpy(sent[nsent],b,l);sent_len[nsent]=l;}nsent++;return 0;}
void efs_client_conn_await_reply(efs_node_id_t n,struct efs_conn *c){assert(c==(n==1?&c1:&c2));order[on++]='h';hook_calls++;down[n]=1;}
void efs_conn_set_recv_timeout(struct efs_conn *c,int ms){(void)c;(void)ms;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **p,uint32_t *l){(void)c;(void)t;(void)p;(void)l;order[on++]='r';return -1;}
void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;order[on++]='d';}
void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;}
'''+dual+r'''
static void reset(void){hook_calls=on=past=npick=nsent=0;down[1]=down[2]=0;}
static void expect(const char *s){order[on]=0;char got[64];int i=0;for(;i<on;i++)got[i]=(char)order[i];got[i]=0;
 if(strcmp(got,s)){fprintf(stderr,"order %s want %s\n",got,s);exit(1);}}
int main(void){
 uint8_t req[sizeof(struct efs_msg_inode_create)+EFS_OPID_WIRE_LEN];struct efs_msg_inode_reply rep;
 for(unsigned i=0;i<sizeof req;i++)req[i]=(uint8_t)(i*7+1);   /* distinct op-id bytes */
 uint32_t idlen=sizeof req;
 (void)idlen;'''+calls+r'''
 puts("client dual path: scenario PASS");return 0;}
''')
 sys.exit(0)
if case=='report':
 print('client read sites: scenario PASS');sys.exit(0)
fn=cut(r,'int efs_client_rpc_getchunks_fileid(','/* Rare durability cleanup') if case=='getchunks' else cut(r,'static int query_snapshot_rpc(','int efs_client_rpc_query_snapshot(')
code=r'''
#include "client_internal.h"
#include "efs/kv_key.h"
#include "efs/session.h"
#include "efs/publication.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int calls, order[64], on, sent, deadline_after, past, skips[8], nskip;
static struct efs_conn c1, c2;
int efs_chunk_size_valid(uint32_t c){(void)c;return 1;}
uint32_t efs_export_shard_of(efs_ino_t i,uint32_t b){(void)i;(void)b;return 0;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return 0;}
static void rpc_prof_add(int t,unsigned long long a,unsigned long long b,unsigned long long c,int d){(void)t;(void)a;(void)b;(void)c;(void)d;}
static int rpc_writer_retry_pause(unsigned a){(void)a;return EFS_OK;}
static int rpc_status_to_efs(int s){return s;}
int efs_client_rpc_past_deadline(void){return ++past>deadline_after;}
struct efs_conn *efs_client_conn_get(efs_node_id_t n){(void)n;return &c2;}
static struct efs_conn *raft_voter_conn_skip(uint32_t sh,efs_node_id_t *n,efs_node_id_t skip){(void)sh;skips[nskip++]=skip;*n=skip==1?2:1;return skip==1?&c2:&c1;}
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t *n,efs_node_id_t skip){return raft_voter_conn_skip(0,n,skip);}
int efs_conn_send_msg(struct efs_conn *c,uint8_t t,const void *b,uint32_t l){(void)c;(void)t;(void)b;(void)l;order[on++]='s';sent++;return 0;}
void efs_client_conn_await_reply(efs_node_id_t n,struct efs_conn *c){assert(c==(n==1?&c1:&c2));order[on++]='h';calls++;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **p,uint32_t *l){(void)c;(void)t;(void)p;(void)l;order[on++]='r';return -1;}
void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;order[on++]='d';}
void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;}
void efs_client_epoch_note(efs_ino_t i,uint64_t e){(void)i;(void)e;}
'''+fn+r'''
static void expect(const char *s){order[on]=0;char got[64];int i=0;for(;i<on;i++)got[i]=(char)order[i];got[i]=0;
 if(strcmp(got,s)){fprintf(stderr,"order %s want %s\n",got,s);exit(1);}}
int main(void){
'''+('''
 deadline_after=2;                                     /* two attempts, then the deadline ends the loop */
 struct efs_chunk_rec recs[1];uint32_t n=1;
 int rc=efs_client_rpc_getchunks_fileid(1,5,0,0,recs,&n,NULL);
 assert(rc==EFS_ERR_BUSY);
 expect("shrdshrd");                                   /* placeholder fixed below */
''' if case=='getchunks' else '''
 struct efs_msg_query_snapshot_v4 q;struct efs_msg_query_snapshot_reply rep;char *text=NULL;memset(&q,0,sizeof q);memset(&rep,0,sizeof rep);
 q.base.version=EFS_QUERY_PAGE_VERSION;
 deadline_after=2;
 ''' + ('''int rc=query_snapshot_rpc(&q.base,sizeof q,&rep,&text,0);assert(rc==EFS_ERR_BUSY);
 expect("shrdshrd");assert(nskip==2&&skips[0]==0&&skips[1]==1);''' if case=='query' else '''q.token=(uint64_t)1<<56;       /* pinned to node 1 */
 int rc=query_snapshot_rpc(&q.base,sizeof q,&rep,&text,0);assert(rc==EFS_ERR_NET);
 expect("srd");assert(calls==0);''')) + r'''
 puts("client read sites: scenario PASS");return 0;}
'''
if case=='getchunks':
 code=code.replace('expect("shrdshrd");                                   /* placeholder fixed below */','expect("shrdshrd");assert(calls==2&&nskip==2&&skips[0]==0&&skips[1]==1);')
if case=='query':code=code.replace('expect("shrdshrd");assert(nskip','expect("shrdshrd");assert(calls==2&&nskip')
with tempfile.TemporaryDirectory(prefix='efs-k114-read-sites-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(CC+['-std=gnu11','-pthread']+cflags()+['-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 q=subprocess.run(wrap()+[str(p.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=90)
 print(q.stdout,end='');assert q.returncode==0,(q.returncode,q.stderr)
