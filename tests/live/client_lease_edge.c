/* Actual client deferred lease transport; only disposable private fixture files. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
 assert(argc==5);unsigned port=(unsigned)atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],NULL,10);
 struct efs_opid_req q={0};assert(strlen(argv[3])==32);
 for(unsigned i=0;i<16;i++){unsigned n;assert(sscanf(argv[3]+2*i,"%2x",&n)==1);q.id.client_uuid[i]=(uint8_t)n;}
 q.id.session_epoch=1;g_client.export_id=1;g_client.node_count=4;
 for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
 efs_client_conn_init();uint64_t generation=0;uint32_t count=1;
 assert(efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&generation)==EFS_OK && generation);
 int opening=!strcmp(argv[4],"open"),replay=!strcmp(argv[4],"replay"),revoked=!strcmp(argv[4],"revoked");
 assert(opening || replay || revoked || !strcmp(argv[4],"close"));
 if(revoked) {
  uint64_t owner=0;for(unsigned i=0;i<8;i++)owner=owner<<8|q.id.client_uuid[i];
  assert(owner && !memcmp(q.id.client_uuid+8,(uint8_t[8]){0},8));
  assert(efs_client_rpc_hold(1,ino,1,owner)==EFS_ERR_STALE);
  assert(efs_client_rpc_flock(1,ino,EFS_FLOCK_EX,owner)==EFS_ERR_STALE);
  puts("actual legacy HOLD/FLOCK after durable revocation rejected STALE PASS");return 0;
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
