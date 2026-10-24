#!/usr/bin/env python3
"""Production session CLI redirect routing; transport ambiguity is not retried."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/mgmt/efs_mgmt.c').read_text()
a=s.index('static int sess_leader_address(')
rpc=s.index('static int sess_rpc(',a)
b=s.index('\n}\n',rpc)+3  # only the named helper, not adjacent CLI commands
source=r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/session_revoke.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static unsigned requests,discoveries,connections,waits;
static int fail_transport,loop,invalid_count,unknown,no_hint,busy;
static uint32_t expected_len=27;
static uint8_t immutable[27];
int efs_connect_declared(const char *host,uint16_t port) {
 assert((!strcmp(host,"seed")&&port==1)||(!strcmp(host,"leader.example")&&port==9300));connections++;return 7;
}
static int efs_set_recv_timeout(int fd,int n){(void)fd;(void)n;return 0;}
static int efs_set_send_timeout(int fd,int n){(void)fd;(void)n;return 0;}
static int usleep(unsigned n){assert(n==50000);waits++;return 0;}
static int close(int fd){assert(fd==7);return 0;}
static int send_recv(int fd,uint8_t t,const void *p,uint32_t n,uint8_t *rt,void **out,uint32_t *len) {
 assert(fd==7);
 if(t==EFS_MSG_LIST_NODES) {
  discoveries++;struct efs_msg_list_nodes_reply *r=calloc(1,sizeof(*r));assert(r);
  r->node_count=invalid_count?EFS_MAX_NODES+1:1;r->nodes[0].id=unknown?1:3;
  strcpy(r->nodes[0].addr,"leader.example");r->nodes[0].port=9300;
  *out=r;*len=sizeof(*r);*rt=EFS_MSG_LIST_NODES_REPLY;return 0;
 }
 assert(t==EFS_MSG_RAFT_MKFS && n==expected_len && !memcmp(p,immutable,n));requests++;
 struct efs_msg_raft_mkfs_reply *r=calloc(1,sizeof(*r));assert(r);
 r->rc=(requests==1 || loop)?(busy?EFS_ERR_BUSY:EFS_ERR_NOT_PRIMARY):EFS_OK;r->leader_hint=no_hint?-1:2;
 *out=r;*len=sizeof(*r);*rt=EFS_MSG_RAFT_MKFS_REPLY;return fail_transport;
}
''' +s[a:b]+r'''
static void reset(void){requests=discoveries=connections=waits=0;fail_transport=loop=invalid_count=unknown=no_hint=busy=0;expected_len=27;memset(immutable,0xa5,sizeof(immutable));}
int main(void) {
 struct efs_msg_raft_mkfs_reply out;memset(immutable,0xa5,sizeof(immutable));
 reset();assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_OK && requests==2 && discoveries==1);
 reset();no_hint=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_OK && requests==2 && waits==1 && !discoveries);
 reset();no_hint=loop=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_ERR_NOT_PRIMARY && requests==8 && waits==8 && !discoveries);
 reset();no_hint=fail_transport=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_ERR_IO && requests==1 && !waits);
 reset();fail_transport=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_ERR_IO && requests==1 && !discoveries);
 reset();loop=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_ERR_NOT_PRIMARY && requests==8);
 reset();invalid_count=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_ERR_PROTO && requests==1);
 reset();unknown=1;assert(sess_rpc("seed",1,immutable,sizeof(immutable),&out)==EFS_ERR_NOT_PRIMARY && requests==1);
 reset();busy=1;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=EFS_MD_SESS_FENCE_LOC;
 assert(sess_rpc("seed",1,immutable,27,&out)==EFS_OK && requests==2 && waits==1 && !discoveries);
 reset();busy=loop=1;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=EFS_MD_SESS_FENCE_LOC;
 assert(sess_rpc("seed",1,immutable,27,&out)==EFS_ERR_BUSY && requests==8 && waits==8);
 reset();busy=1;expected_len=23;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=0;
 assert(sess_rpc("seed",1,immutable,23,&out)==EFS_OK && requests==2 && waits==1);
 reset();busy=1;expected_len=23;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=EFS_MD_SESS_STORAGE_FENCE;
 assert(sess_rpc("seed",1,immutable,23,&out)==EFS_OK && requests==2 && waits==1);
 reset();busy=1;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=EFS_MD_SESS_BEGIN;
 assert(sess_rpc("seed",1,immutable,27,&out)==EFS_ERR_BUSY && requests==1 && !waits);
 reset();busy=1;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=EFS_MD_SESS_LEASE_LOCK_NEXT;
 assert(sess_rpc("seed",1,immutable,27,&out)==EFS_ERR_BUSY && requests==1 && !waits);
 reset();busy=1;immutable[1]=EFS_MD_CMD_SESSION;immutable[2]=EFS_MD_SESS_FENCE_LOC;fail_transport=1;
 assert(sess_rpc("seed",1,immutable,27,&out)==EFS_ERR_IO && requests==1 && !waits);
 puts("management session routing: advertised leader, exact retries, bounded loop and ambiguous transport PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mgmt-session-') as d:
 p=Path(d)/'test.c';p.write_text(source);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
