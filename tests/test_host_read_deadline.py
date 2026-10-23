#!/usr/bin/env python3
"""Execute actual read scope and forwarded RPC with controlled transport waits."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
    a=s.index('static int '+name+'(');return s[a:s.index('\n}',a)+2]
scope=r'''
#include "efs/protocol.h"
#include "efs/network.h"
#include <assert.h>
#include <time.h>
#include <stdio.h>
#define HOST_READ_TRIES 80
#define HOST_TICK_US 5000
struct efs_raft_host {int unused;};
static uint64_t now_ms=10000,deadline,observed;static int status,expire;
uint64_t efs_net_deadline_ms(void){return deadline;}
void efs_net_set_deadline_ms(uint64_t value){deadline=value;}
int efs_net_remaining_ms(int cap){return !deadline?cap:now_ms>=deadline?0:cap;}
static int controlled_clock(clockid_t id,struct timespec *out)
{assert(id==CLOCK_MONOTONIC);out->tv_sec=now_ms/1000;out->tv_nsec=(long)(now_ms%1000)*1000000;return 0;}
static int host_remote_cmd_impl(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,struct efs_msg_raft_mkfs_reply *r,int rid)
{(void)h;assert(g==2 && rid==3 && (n==0 || cmd));(void)r;observed=deadline;if(expire)now_ms=deadline;return status;}
#define clock_gettime controlled_clock
'''+function('host_remote_cmd')+r'''
#undef clock_gettime
int main(void){struct efs_raft_host h;struct efs_msg_raft_mkfs_reply reply;uint8_t cmd=1;
 assert(!host_remote_cmd(&h,2,NULL,0,&reply,3) && observed==10400 && !deadline);
 deadline=10100;assert(!host_remote_cmd(&h,2,NULL,0,&reply,3) && observed==10100 && deadline==10100);
 deadline=9000;status=EFS_ERR_IO;assert(host_remote_cmd(&h,2,NULL,0,&reply,3)==EFS_ERR_BUSY && deadline==9000);
 now_ms=10000;deadline=15000;status=EFS_ERR_STALE;assert(host_remote_cmd(&h,2,NULL,0,&reply,3)==EFS_ERR_STALE && deadline==15000);
 status=EFS_ERR_IO;assert(host_remote_cmd(&h,2,NULL,0,&reply,3)==EFS_ERR_IO && deadline==15000);
 expire=1;assert(host_remote_cmd(&h,2,NULL,0,&reply,3)==EFS_ERR_BUSY && deadline==15000);
 now_ms=10000;deadline=15000;assert(host_remote_cmd(&h,2,&cmd,1,&reply,3)==EFS_ERR_IO && observed==15000 && deadline==15000);
 puts("read scope: one absolute400ms budget, shorter inherited/expired budgets, restoration, semantic/hard refusal and unchanged mutation uncertainty PASS");}
'''
rpc=r'''
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/wire.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define HOST_CMD_MAX 512
struct efs_raft_host {int raft_id;};
static int formed=1;static int adm_complete(void){return formed;}
static struct efs_conn peer;static int setters,drops,releases,sends,recvs,fail_send,fail_recv,bad_type,short_reply,expected_timeout,ordinary_gets,read_gets;
static int peer_addr(struct efs_raft_host *h,int rid,char *a,size_t n,uint16_t *port)
{(void)h;assert(rid==2);snprintf(a,n,"peer");*port=17432;return 0;}
struct efs_conn *server_peer_conn_get(const char *host,uint16_t port){assert(!strcmp(host,"peer") && port==17432);ordinary_gets++;return &peer;}
struct efs_conn *server_peer_read_conn_get(const char *host,uint16_t port){assert(!strcmp(host,"peer") && port==17432);read_gets++;return &peer; }
void server_peer_conn_drop(const char *h,uint16_t port,struct efs_conn *c){(void)h;(void)port;assert(c==&peer);drops++;}
void server_peer_conn_release(const char *h,uint16_t port,struct efs_conn *c){(void)h;(void)port;assert(c==&peer && c->recv_timeout_ms==17);releases++;}
void efs_conn_set_recv_timeout(struct efs_conn *c,int value){assert(c==&peer);setters++;c->recv_timeout_ms=value;}
int efs_set_send_timeout(int fd,int ms){assert(fd==42 && (ms==expected_timeout || ms==EFS_IO_TIMEOUT_MS));return 0;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n)
{assert(c==&peer && type==EFS_MSG_RAFT_MKFS && ((const uint8_t *)p)[0]==2 && n>=1 && c->recv_timeout_ms==expected_timeout);sends++;return fail_send;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **out,uint32_t *n)
{assert(c==&peer && c->recv_timeout_ms==expected_timeout);recvs++;if(fail_recv)return -1;
 *type=bad_type?0:EFS_MSG_RAFT_MKFS_REPLY;*n=sizeof(struct efs_msg_raft_mkfs_reply)-(short_reply?1:0);
 *out=calloc(1,sizeof(struct efs_msg_raft_mkfs_reply));return 0;}
'''+function('host_rpc_submit')+r'''
static void reset(int kind){peer=(struct efs_conn){.kind=kind,.fd=42,.recv_timeout_ms=17};setters=drops=releases=sends=recvs=fail_send=fail_recv=bad_type=short_reply=ordinary_gets=read_gets=0;expected_timeout=400;}
int main(void){struct efs_raft_host h={.raft_id=0};struct efs_msg_raft_mkfs_reply reply;uint8_t body[900]={0};
 for(int kind=EFS_CONN_TCP;kind<=EFS_CONN_RDMA;kind++) {
  reset(kind);assert(!host_rpc_submit(&h,2,2,NULL,0,&reply,400));assert(setters==2 && releases==1 && !drops && read_gets==1 && !ordinary_gets);
  reset(kind);assert(!host_rpc_submit(&h,2,2,body,sizeof(body),&reply,400));assert(setters==2 && releases==1 && ordinary_gets==1 && !read_gets);
  reset(kind);fail_send=1;assert(host_rpc_submit(&h,2,2,NULL,0,&reply,400)==EFS_ERR_IO && drops==1 && !recvs && !releases);
  reset(kind);fail_recv=1;assert(host_rpc_submit(&h,2,2,NULL,0,&reply,400)==EFS_ERR_IO && drops==1 && !releases);
  reset(kind);bad_type=1;assert(host_rpc_submit(&h,2,2,NULL,0,&reply,400)==EFS_ERR_IO && drops==1 && !releases);
  reset(kind);short_reply=1;assert(host_rpc_submit(&h,2,2,NULL,0,&reply,400)==EFS_ERR_IO && drops==1 && !releases);
  reset(kind);assert(host_rpc_submit(&h,0,2,NULL,0,&reply,400)==EFS_ERR_INVAL && !setters && !sends);
  /* D39: no forward (application work) before this member's formation completes. */
  reset(kind);formed=0;assert(host_rpc_submit(&h,2,2,NULL,0,&reply,400)==EFS_ERR_BUSY && !ordinary_gets && !read_gets && !sends);formed=1;
 }
 puts("forwarded RPC: TCP/RDMA receive limits, previous timeout restoration, large-frame ownership, lost/short/wrong replies drop connections and refuse authority, no forward before formation PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-deadline-') as t:
    p=Path(t)
    for name,code in [('scope',scope),('rpc',rpc)]:
        (p/(name+'.c')).write_text(code)
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/(name+'.c')),'-o',str(p/name)],check=True)
        subprocess.run([str(p/name)],check=True,timeout=10)
