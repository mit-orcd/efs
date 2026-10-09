/* Actual client deferred lease transport; only disposable private fixture files. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static uint64_t legacy_cached(unsigned port,unsigned *node,efs_ino_t ino,
    const uint8_t uuid[16],unsigned expected)
{
 struct efs_msg_inode_append request={.export_id=1,.ino=ino,.len=8};
 uint8_t bytes[sizeof(request)+EFS_APPEND_OPID_LEN];uint32_t epoch=1;uint64_t sequence=1;
 memcpy(bytes,&request,sizeof(request));memcpy(bytes+sizeof(request),uuid,16);
 memcpy(bytes+sizeof(request)+16,&epoch,4);memcpy(bytes+sizeof(request)+20,&sequence,8);
 for(unsigned attempt=0;attempt<120;attempt++) {
  int fd=efs_connect_tcp("127.0.0.1",port+*node);if(fd<0){*node=(*node+1)%4;usleep(10000);continue;}
  assert(!efs_set_recv_timeout(fd,5000) && !efs_set_send_timeout(fd,5000));
  int rc=efs_send_msg(fd,EFS_MSG_INODE_APPEND,bytes,sizeof(bytes));uint8_t type=0;void *body=NULL;uint32_t length=0;
  if(!rc)rc=efs_recv_msg(fd,&type,&body,&length);
  close(fd);
  if(rc){free(body);usleep(10000);continue;}
  assert(type==EFS_MSG_INODE_APPEND_REPLY && length==sizeof(struct efs_msg_inode_reply));
  struct efs_msg_inode_reply reply;memcpy(&reply,body,length);free(body);
  if(reply.status==EFS_INODE_RPC_NOT_PRIMARY){*node=reply.primary_id?reply.primary_id-1:(*node+1)%4;assert(*node<4);usleep(10000);continue;}
  if(reply.status==EFS_INODE_RPC_BUSY){usleep(10000);continue;}
  assert(reply.status==expected);return reply.inode.atime;
 }
 assert(!"legacy cached append deadline");return 0;
}
int main(int argc,char **argv)
{
 assert(argc==5 || argc==6);unsigned port=(unsigned)atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],NULL,10);
 struct efs_opid_req q={0};assert(strlen(argv[3])==32);
 for(unsigned i=0;i<16;i++){unsigned n;assert(sscanf(argv[3]+2*i,"%2x",&n)==1);q.id.client_uuid[i]=(uint8_t)n;}
 q.id.session_epoch=1;g_client.export_id=1;g_client.node_count=4;
 for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
 efs_client_conn_init();uint64_t generation=0;uint32_t count=1;
 assert(efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&generation)==EFS_OK && generation);
 if(!strcmp(argv[4],"cached")) {
  assert(argc==6);unsigned node=0;uint64_t offset=legacy_cached(port,&node,ino,q.id.client_uuid,EFS_INODE_RPC_OK);
  assert(legacy_cached(port,&node,ino,q.id.client_uuid,EFS_INODE_RPC_OK)==offset);
  char address[64];snprintf(address,sizeof(address),"127.0.0.1:%u",port);
  pid_t child=fork();assert(child>=0);
  if(!child){execl(argv[5],argv[5],"raft-session",address,"fence",argv[3],(char *)NULL);_exit(127);}
  int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
  legacy_cached(port,&node,ino,q.id.client_uuid,EFS_INODE_RPC_STALE);
  printf("actual hot legacy append cache: exact offset=%llu; durable fence rejects cached replay STALE PASS\n",(unsigned long long)offset);return 0;
 }
 int opening=!strcmp(argv[4],"open"),replay=!strcmp(argv[4],"replay"),revoked=!strcmp(argv[4],"revoked");
 assert(opening || replay || revoked || !strcmp(argv[4],"close"));
 if(revoked) {
  memcpy(g_client.opid_uuid,q.id.client_uuid,16);g_client.opid_epoch=1;g_client.opid_next=1;
  uint64_t owner=0;for(unsigned i=0;i<8;i++)owner=owner<<8|q.id.client_uuid[i];
  assert(owner && !memcmp(q.id.client_uuid+8,(uint8_t[8]){0},8));
  assert(efs_client_rpc_hold(1,ino,1,owner)==EFS_ERR_STALE);
  assert(efs_client_rpc_flock(1,ino,EFS_FLOCK_EX,owner)==EFS_ERR_STALE);
  puts("actual full-UUID HOLD/FLOCK after durable revocation rejected STALE PASS");return 0;
 }
 q.id.seq=opening?1:2;q.ack=opening?0:1;unsigned flags=opening?EFS_LEASE_EDGE_OPEN:EFS_LEASE_EDGE_CLOSE;
 if(opening)assert(efs_client_rpc_publication_session(efs_kv_inode_shard(ino),&q.id)==EFS_OK);
 uint64_t known=0;
 assert(efs_client_rpc_lease_edge(ino,generation,&q,flags,0,replay,&known)==EFS_OK && known==1);
 assert(efs_client_rpc_lease_edge(ino,generation,&q,flags,0,1,&known)==EFS_OK && known==1);
 if(!replay)assert(efs_client_rpc_lease_edge(ino,generation,&q,flags,0,0,&known)==EFS_OK && known==1);
 q.id.seq=3;q.ack=2;
 assert(efs_client_rpc_lease_edge(ino,generation,&q,EFS_LEASE_EDGE_OPEN,0,1,&known)==EFS_ERR_BUSY && known==0);
 printf("actual client lease %s: generation=%llu stored exact replay and unknown probe PASS\n",argv[4],(unsigned long long)generation);
 return 0;
}
