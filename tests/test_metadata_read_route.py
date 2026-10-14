#!/usr/bin/env python3
"""Exercise production metadata read-entry selection without a cluster."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = (root/'src/client/inode_rpc.c').read_text()

def fn(signature):
    a = s.index(signature)
    return s[a:s.index('\n}', a)+2]

code = r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include "metadata_read_route.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_conn { unsigned id; };
static struct { unsigned node_count; } g_client={4};
static struct efs_conn conns[5]={{0},{1},{2},{3},{4}};
static unsigned down,unavailable,calls[5];
static efs_node_id_t leaders[2];
static efs_node_id_t rpc_leader_of(uint8_t g){return leaders[g>>1];}
static int efs_client_node_is_down(efs_node_id_t id){return !!(down&(1u<<id));}
static struct efs_conn *efs_client_conn_get(efs_node_id_t id){assert(id>0&&id<=g_client.node_count);calls[id]++;return unavailable&(1u<<id)?NULL:&conns[id];}
static struct efs_conn *rpc_primary_conn(efs_node_id_t *out){(void)out;return NULL;}
''' + fn('static uint32_t raft_group_voters(') + fn('static struct efs_conn *raft_voter_conn_skip(') + fn('static struct efs_conn *raft_voter_conn(') + fn('static struct efs_conn *raft_dual_voter_conn(') + fn('static struct efs_conn *raft_read_voter_conn_skip(') + r'''
static void reset(void){down=unavailable=0;memset(calls,0,sizeof(calls));leaders[0]=leaders[1]=0;g_client.node_count=4;}
int main(void){efs_node_id_t id=0;
 assert(offsetof(struct efs_msg_inode_reply,read_node_id)==1);
 assert(offsetof(struct efs_msg_inode_reply,primary_id)==4);
 assert(offsetof(struct efs_msg_inode_reply,inode)==8);
 assert(sizeof(struct efs_msg_inode_reply)==8+sizeof(struct efs_inode));
 struct efs_meta_read_routes routes={.mu=PTHREAD_MUTEX_INITIALIZER};
 unsigned char request[16]={1};
 uint64_t key=efs_meta_read_route_key(EFS_MSG_INODE_GETATTR,request,sizeof(request));
 assert(!efs_meta_read_route_get(&routes,key));efs_meta_read_route_note(&routes,key,4);
 assert(efs_meta_read_route_get(&routes,key)==4);
 request[0]=2;assert(efs_meta_read_route_key(EFS_MSG_INODE_GETATTR,request,sizeof(request))!=key);
 assert(efs_meta_read_route_key(EFS_MSG_INODE_LOOKUP,request,sizeof(request))!=key);
 efs_meta_read_route_note(&routes,key+EFS_META_READ_ROUTES,2);
 assert(!efs_meta_read_route_get(&routes,key));
 pthread_mutex_destroy(&routes.mu);
 reset();leaders[0]=1;leaders[1]=4;
 assert(raft_read_voter_conn_skip(1,&id,0)==&conns[2]&&id==2&&!calls[1]&&!calls[4]);
 assert(raft_read_voter_conn_skip(2,&id,0)==&conns[2]&&id==2);
 reset();leaders[0]=3;assert(raft_read_voter_conn_skip(1,&id,0)==&conns[3]&&id==3&&!calls[2]);
 reset();leaders[0]=3;assert(raft_read_voter_conn_skip(1,&id,3)==&conns[2]&&id==2&&!calls[3]);
 reset();assert(raft_read_voter_conn_skip(1,&id,2)==&conns[3]&&id==3&&!calls[2]);
 reset();down=1u<<2;assert(raft_read_voter_conn_skip(2,&id,0)==&conns[3]&&id==3&&!calls[2]);
 reset();unavailable=1u<<2;assert(raft_read_voter_conn_skip(1,&id,0)==&conns[3]&&id==3&&calls[2]==1);
 reset();down=(1u<<2)|(1u<<3);assert(raft_read_voter_conn_skip(2,&id,0)==&conns[1]&&id==1);
 reset();leaders[0]=5;assert(raft_read_voter_conn_skip(1,&id,0)==&conns[2]);
 for(unsigned n=1;n<=3;n++){reset();g_client.node_count=n;leaders[0]=leaders[1]=n;assert(raft_read_voter_conn_skip(2,&id,0)==&conns[n]&&id==n);}
 puts("metadata read routes: split single-group leaders, dual leader, skip/down/pool refusal, fallback and small clusters PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-meta-read-route-') as d:
    p=Path(d)/'test.c'
    p.write_text(code)
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
