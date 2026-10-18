#!/usr/bin/env python3
"""expiry show's path lookup follows the namespace leader's NOT_PRIMARY hint."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/mgmt/efs_mgmt.c').read_text()
a=s.index('static int sess_leader_address(');b=s.index('\n}\n',a)+3
c=s.index('static int expiry_lookup(');d=s.index('\n}\n',c)+3
source=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static unsigned lookups,discoveries,waits,seed_lookups;
static int no_hint,loop,fail_transport,status_after=EFS_INODE_RPC_OK;
static const char *last_host;
static int efs_connect_tcp(const char *host,uint16_t port){
 assert((!strcmp(host,"seed")&&port==1)||(!strcmp(host,"leader.example")&&port==9300));last_host=host;return 7;}
static int efs_set_recv_timeout(int fd,int n){(void)fd;(void)n;return 0;}
static int efs_set_send_timeout(int fd,int n){(void)fd;(void)n;return 0;}
static int usleep(unsigned n){assert(n==50000);waits++;return 0;}
static int close(int fd){assert(fd==7);return 0;}
static int send_recv(int fd,uint8_t t,const void *p,uint32_t n,uint8_t *rt,void **out,uint32_t *len){
 assert(fd==7);
 if(t==EFS_MSG_LIST_NODES){
  discoveries++;struct efs_msg_list_nodes_reply *r=calloc(1,sizeof(*r));assert(r);
  r->node_count=1;r->nodes[0].id=3;strcpy(r->nodes[0].addr,"leader.example");r->nodes[0].port=9300;
  *out=r;*len=sizeof(*r);*rt=EFS_MSG_LIST_NODES_REPLY;return 0;}
 assert(t==EFS_MSG_INODE_LOOKUP_PATH && n==sizeof(struct efs_msg_inode_lookup_path));
 assert(!strcmp(((const struct efs_msg_inode_lookup_path*)p)->path,"/expiry-gates/inspection"));
 lookups++;int seed=!strcmp(last_host,"seed");seed_lookups+=seed;
 struct efs_msg_inode_lookup_path_reply *r=calloc(1,sizeof(*r));assert(r);
 if(seed || loop){r->status=EFS_INODE_RPC_NOT_PRIMARY;r->primary_id=no_hint?0:3;}
 else {r->status=status_after;r->inode.ino=42;}
 *out=r;*len=sizeof(*r);*rt=EFS_MSG_INODE_LOOKUP_PATH_REPLY;return fail_transport;}
'''+s[a:b]+s[c:d]+r'''
static void reset(void){lookups=discoveries=waits=seed_lookups=0;no_hint=loop=fail_transport=0;status_after=EFS_INODE_RPC_OK;}
int main(void){
 struct efs_inode ino;const char *p="/expiry-gates/inspection";
 /* Seed is not the namespace leader: one redirect to node 3. */
 reset();memset(&ino,0,sizeof(ino));assert(expiry_lookup("seed",1,p,&ino)==EFS_OK && ino.ino==42 && lookups==2 && discoveries==1 && !waits);
 /* The leader's own verdicts are final, not retried. */
 reset();status_after=EFS_INODE_RPC_NOT_FOUND;assert(expiry_lookup("seed",1,p,&ino)==EFS_ERR_NOT_FOUND && lookups==2);
 reset();status_after=EFS_INODE_RPC_ERROR;assert(expiry_lookup("seed",1,p,&ino)==EFS_ERR_BUSY && lookups==2);
 /* No leader known: retry the seed with a pause, within the budget. */
 reset();no_hint=1;assert(expiry_lookup("seed",1,p,&ino)==EFS_ERR_NOT_PRIMARY && lookups==8 && seed_lookups==8 && waits==8 && !discoveries);
 /* A redirect loop stays bounded. */
 reset();loop=1;assert(expiry_lookup("seed",1,p,&ino)==EFS_ERR_NOT_PRIMARY && lookups==8);
 /* Transport failure is terminal. */
 reset();fail_transport=1;assert(expiry_lookup("seed",1,p,&ino)==EFS_ERR_IO && lookups==1);
 puts("expiry show lookup: leader redirect, final leader verdicts, unhinted bounded retry, bounded loop and terminal transport PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-expiry-lookup-') as d:
 p=Path(d)/'t.c';p.write_text(source)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
