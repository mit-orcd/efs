#!/usr/bin/env python3
"""Actual query RPC and dual-host picker: exclude failed hints, retain page affinity."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_QUERY_SOURCE',str(root/'src/client/inode_rpc.c'))).read_text()
a=s.index('static struct efs_conn *raft_dual_voter_conn(');b=s.index('\nstatic struct efs_conn *rpc_owner_conn_shard',a);picker=s[a:b]
a=s.index('static int query_snapshot_rpc(');b=s.index('\nint efs_client_rpc_query_snapshot(',a);rpc=s[a:b]
code=r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_conn {unsigned node;};
static struct {unsigned node_count;} g_client={4};
static struct efs_conn conns[5]={{0},{1},{2},{3},{4}};
static unsigned scenario,calls[5],sent,received;
static int past;
static uint32_t raft_group_voters(unsigned group,int n){(void)n;return group?14:7;}
static int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){
 assert(n>0 && n<5);calls[n]++;
 if(scenario==1 && n==2)past=1; /* a skipped pool would consume the deadline */
 if(scenario==4 && n==3)return NULL;
 return &conns[n];
}
static struct efs_conn *raft_voter_conn(efs_ino_t ino,efs_node_id_t*n){(void)ino;*n=1;return efs_client_conn_get(1);}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){assert(n==c->node);}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){assert(n==c->node);}
static int efs_client_rpc_past_deadline(void){return past;}
static int rpc_writer_retry_pause(unsigned a){(void)a;return past?EFS_ERR_BUSY:EFS_OK;}
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){
 (void)t;(void)p;(void)n;sent++;
 if((scenario==2 && c->node==2) || ((scenario==3 || scenario==5) && c->node==3))return EFS_ERR_NET;
 return EFS_OK;
}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){
 struct efs_msg_query_snapshot_reply r={0};received++;r.version=EFS_QUERY_PAGE_VERSION;
 if((scenario==3 || scenario==4) && received==1){r.status=EFS_ERR_NOT_PRIMARY;r.primary_id=3;}
 else if(scenario==6){r.status=EFS_ERR_BUSY;}
 else {r.status=EFS_OK;r.text_len=2;}
 *t=EFS_MSG_QUERY_SNAPSHOT_REPLY;*n=sizeof(r)+r.text_len;*p=malloc(*n);assert(*p);
 memcpy(*p,&r,sizeof(r));if(r.text_len)memcpy((char*)*p+sizeof(r),"{}",2);(void)c;return 0;
}
'''+picker+rpc+r'''
static void reset(unsigned x){scenario=x;past=0;sent=received=0;memset(calls,0,sizeof(calls));}
int main(void){
 efs_node_id_t nid=0;reset(1);assert(raft_dual_voter_conn(&nid,2)==&conns[3]);assert(nid==3 && calls[2]==0 && !past);
 struct efs_msg_query_snapshot_v4 q={0};q.base.version=EFS_QUERY_PAGE_VERSION;q.base.max_output=16;
 struct efs_msg_query_snapshot_reply r;char*text=NULL;
 for(unsigned x=2;x<=4;x++){
  reset(x);assert(query_snapshot_rpc(&q.base,sizeof(q),&r,&text)==EFS_OK);assert(text && !strcmp(text,"{}"));free(text);
  if(x==2)assert(calls[2]==1 && calls[3]==1);
  else assert(calls[3]==1 && calls[2]==2);
 }
 q.token=(uint64_t)3<<56;reset(5);assert(query_snapshot_rpc(&q.base,sizeof(q),&r,&text)==EFS_ERR_NET);assert(calls[3]==1 && calls[2]==0 && !text);
 reset(6);assert(query_snapshot_rpc(&q.base,sizeof(q),&r,&text)==EFS_ERR_BUSY);assert(calls[3]==1 && calls[2]==0 && !text);
 puts("query routing: alternate before excluded pool, failed hints cleared, pinned pages never rerouted PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-query-failover-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
