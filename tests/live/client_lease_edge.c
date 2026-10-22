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
int main(int argc,char **argv)
{
 assert(argc==5 || argc==6);unsigned port=(unsigned)atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],NULL,10);
 struct efs_opid_req q={0};assert(strlen(argv[3])==32);
 for(unsigned i=0;i<16;i++){unsigned n;assert(sscanf(argv[3]+2*i,"%2x",&n)==1);q.id.client_uuid[i]=(uint8_t)n;}
 q.id.session_epoch=1;g_client.export_id=1;g_client.node_count=4;
 for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
 efs_client_conn_init();uint64_t generation=0;uint32_t count=1;
 assert(efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&generation)==EFS_OK && generation);
 /* K5: this process acts as the mount whose session is argv[3]/epoch 1. */
 memcpy(g_client.opid_uuid,q.id.client_uuid,16);g_client.opid_epoch=1;g_client.opid_next=1;
 int opening=!strcmp(argv[4],"open"),replay=!strcmp(argv[4],"replay"),revoked=!strcmp(argv[4],"revoked");
 assert(opening || replay || revoked || !strcmp(argv[4],"close"));
 if(revoked) {
  /* Complete the barrier with the operator CLI, then the mount's HOLD and
   * FLOCK are refused FENCED and nothing is resent under epoch 1. */
  assert(argc==6);char address[64];snprintf(address,sizeof(address),"127.0.0.1:%u",port);
  pid_t child=fork();assert(child>=0);
  if(!child){execl(argv[5],argv[5],"raft-session",address,"fence",argv[3],(char *)NULL);_exit(127);}
  int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
  uint64_t owner=0;for(unsigned i=0;i<8;i++)owner=owner<<8|q.id.client_uuid[i];
  assert(owner && !memcmp(q.id.client_uuid+8,(uint8_t[8]){0},8));
  assert(efs_client_rpc_hold(1,ino,1,owner)==EFS_ERR_FENCED);
  assert(efs_client_rpc_flock(1,ino,EFS_FLOCK_EX,owner)==EFS_ERR_FENCED);
  assert(efs_client_session_fenced());
  puts("actual full-UUID HOLD/FLOCK after durable revocation rejected FENCED, mount latched PASS");return 0;
 }
 assert(efs_client_session_start()==EFS_OK);
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
