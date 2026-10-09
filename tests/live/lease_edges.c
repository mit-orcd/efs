/* Private live RPC gate for sequenced lease effects. No production activation. */
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/txn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void be(uint8_t *p,uint64_t v,unsigned n){while(n){p[--n]=(uint8_t)v;v>>=8;}}
static void owner_rpc(unsigned port, const uint8_t *payload, unsigned length, int expected)
{
    for (unsigned a = 0; a < 120; a++) {
        int fd = efs_connect_tcp("127.0.0.1", port + a % 4);
        if (fd < 0) {
            usleep(10000);
            continue;
        }
        efs_set_recv_timeout(fd, 1000);
        efs_set_send_timeout(fd, 1000);
        int rc = efs_send_msg(fd, EFS_MSG_RAFT_MKFS, payload, length);
        uint8_t type = 0;
        void *body = NULL;
        uint32_t len = 0;
        if (!rc)
            rc = efs_recv_msg(fd, &type, &body, &len);
        close(fd);
        if (rc) {
            free(body);
            usleep(10000);
            continue;
        }
        assert(type == EFS_MSG_RAFT_MKFS_REPLY && len == sizeof(struct efs_msg_raft_mkfs_reply));
        struct efs_msg_raft_mkfs_reply reply;
        memcpy(&reply, body, sizeof(reply));
        free(body);
        if (reply.rc == EFS_ERR_NOT_PRIMARY) {
            usleep(10000);
            continue;
        }
        printf("owner command=%u rc=%d expected=%d\n", payload[1], reply.rc, expected);
        assert(reply.rc == expected);
        return;
    }
    assert(!"owner RPC deadline");
}
static void owner_fence(unsigned port, const char *hex, unsigned shard)
{
    uint8_t uuid[16], p[1024] = {0}, key[EFS_KV_KEY_MAX], value[8] = {0};
    uint32_t klen = 0, n = 0;
    assert(strlen(hex) == 32);
    for (unsigned i = 0; i < 16; i++) {
        unsigned b;
        assert(sscanf(hex + 2 * i, "%2x", &b) == 1);
        uuid[i] = b;
    }
    assert(efs_kv_key_sess_local(shard, uuid, key, &klen) == EFS_OK);
    be(value, 1, 4);
    p[0] = efs_raft_shard_group(shard);
    p[1] = EFS_MD_CMD_PREPARE;
    p[2] = EFS_TXN_EXCL_VALUE;
    memcpy(p + 3, uuid, 16);
    p[19] = 1;
    be(p + 20, shard, 4);
    be(p + 24, klen, 2);
    memcpy(p + 26, key, klen);
    assert(efs_txn_encode_excl_value(p + 26 + klen, sizeof(p) - 26 - klen, &n, value, 8,
                                     EFS_TXN_PUT, value, 8) == EFS_OK);
    owner_rpc(port, p, 26 + klen + n, EFS_OK);
    uint8_t fence[27] = {0};
    fence[0] = p[0];
    fence[1] = EFS_MD_CMD_SESSION;
    fence[2] = EFS_MD_SESS_FENCE_LOC;
    memcpy(fence + 3, uuid, 16);
    be(fence + 19, 2, 4);
    be(fence + 23, shard, 4);
    /* No malformed/routed fence may look like an acknowledged barrier. */
    owner_rpc(port, fence, sizeof(fence) - 1, EFS_ERR_INVAL);
    fence[0] = efs_raft_shard_group(shard ^ 1u);
    owner_rpc(port, fence, sizeof(fence), EFS_ERR_INVAL);
    fence[0] = p[0];
    be(fence + 19, 0, 4);
    owner_rpc(port, fence, sizeof(fence), EFS_ERR_INVAL);
    be(fence + 19, 2, 4);
    be(fence + 23, EFS_SESSION_BITS, 4);
    owner_rpc(port, fence, sizeof(fence), EFS_ERR_INVAL);
    be(fence + 23, shard, 4);
    uint8_t extra[sizeof(fence) + 1];
    memcpy(extra, fence, sizeof(fence));
    extra[sizeof(fence)] = 0;
    owner_rpc(port, extra, sizeof(extra), EFS_ERR_INVAL);
    owner_rpc(port, fence, sizeof(fence), EFS_ERR_BUSY);
    uint8_t resolve[23] = {0};
    resolve[0] = p[0];
    resolve[1] = EFS_MD_CMD_RESOLVE;
    memcpy(resolve + 2, uuid, 16);
    be(resolve + 18, shard, 4);
    resolve[22] = EFS_TXN_ABORT;
    owner_rpc(port, resolve, sizeof(resolve), EFS_OK);
    owner_rpc(port, fence, sizeof(fence), EFS_OK);
    puts("prepared owner blocks wire fence ACK; resolution releases barrier PASS");
}
static void legacy_hold(unsigned port,efs_ino_t ino,uint64_t owner,unsigned flags,unsigned expected)
{
 struct efs_msg_inode_hold request={.export_id=1,.ino=ino,.flags=flags,.owner=owner};
 for(unsigned attempt=0;attempt<120;attempt++) {
  int fd=efs_connect_tcp("127.0.0.1",port+attempt%4);if(fd<0){usleep(10000);continue;}
  efs_set_recv_timeout(fd,2000);efs_set_send_timeout(fd,2000);
  uint8_t type=0;void *body=NULL;uint32_t size=0;
  int rc=efs_send_msg(fd,EFS_MSG_INODE_HOLD,&request,sizeof(request));
  if(!rc)rc=efs_recv_msg(fd,&type,&body,&size);
  close(fd);
  if(rc){free(body);usleep(10000);continue;}
  assert(type==EFS_MSG_INODE_HOLD_REPLY && size==sizeof(struct efs_msg_inode_reply));
  struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
  if(reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
  assert(reply.status==expected);return;
 }
 assert(!"legacy hold guard deadline");
}
static void legacy_guard(unsigned port,efs_ino_t ino)
{
 uint8_t p[1024]={0},key[EFS_KV_KEY_MAX],value[EFS_CLOSE_STEP_VALUE]={0},txid[16]={191};
 uint32_t shard=efs_kv_anchor_shard(efs_kv_inode_shard(ino)),kl=0,n=0;
 assert(efs_kv_key_reap(shard,ino,key,&kl)==EFS_OK);key[2]=EFS_KV_KIND_CLOSE_STEP;
 be(value,1,8);value[8]=191;be(value+24,1,4);be(value+28,1,8);
 p[0]=efs_raft_shard_group(shard);p[1]=EFS_MD_CMD_PREPARE;p[2]=EFS_TXN_EXCL_VALUE;
 memcpy(p+3,txid,16);p[19]=1;be(p+20,shard,4);be(p+24,kl,2);memcpy(p+26,key,kl);
 assert(efs_txn_encode_excl_value(p+26+kl,sizeof(p)-26-kl,&n,NULL,EFS_TXN_ABSENT,EFS_TXN_PUT,value,sizeof(value))==EFS_OK);
 owner_rpc(port,p,26+kl+n,EFS_OK);
 legacy_hold(port,ino,UINT64_C(1910000001),1,EFS_INODE_RPC_BUSY);
 uint8_t resolve[23]={0};resolve[0]=p[0];resolve[1]=EFS_MD_CMD_RESOLVE;
 memcpy(resolve+2,txid,16);be(resolve+18,shard,4);resolve[22]=EFS_TXN_ABORT;
 owner_rpc(port,resolve,sizeof(resolve),EFS_OK);
 legacy_hold(port,ino,UINT64_C(1910000001),1,EFS_INODE_RPC_OK);
 legacy_hold(port,ino,UINT64_C(1910000001),0,EFS_INODE_RPC_OK);
 puts("legacy wire OPEN returns BUSY behind prepared close; later RESOLVE and retry acquire/release PASS");
}
int main(int argc,char **argv)
{
 if(argc==4&&!strcmp(argv[1],"--legacy-guard")){legacy_guard(atoi(argv[2]),strtoull(argv[3],NULL,10));return 0;}
 if(argc==5&&!strcmp(argv[1],"--owner-fence")){owner_fence(atoi(argv[2]),argv[3],atoi(argv[4]));return 0;}
 if(argc==3){
  unsigned port=atoi(argv[1]);struct efs_msg_inode_getattr q={0};q.export_id=1;q.ino=strtoull(argv[2],0,10);
  for(unsigned a=0;a<120;a++){
   int fd=efs_connect_tcp("127.0.0.1",port+a%4);if(fd<0)continue;
   efs_set_recv_timeout(fd,1000);efs_set_send_timeout(fd,1000);
   int rc=efs_send_msg(fd,EFS_MSG_INODE_GETATTR,&q,sizeof(q));uint8_t type=0;void *body=NULL;uint32_t len=0;
   if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
   close(fd);
   if(rc){free(body);continue;}
   assert(type==EFS_MSG_INODE_GETATTR_REPLY && len==sizeof(struct efs_msg_inode_reply));
   struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
   if(reply.status==EFS_INODE_RPC_BUSY || reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
   printf("status=%u gen=%llu nlink=%u shard=%u\n",reply.status,(unsigned long long)1 /* fresh CREATE generation; fixture never truncates */,reply.inode.nlink,efs_kv_inode_shard(q.ino));return 0;
  }
  return 1;
 }
 assert(argc==11);unsigned port=atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],0,10);
 uint64_t gen=strtoull(argv[3],0,10),seq=strtoull(argv[5],0,10),ack=strtoull(argv[6],0,10);
 unsigned flags=atoi(argv[7]),probe=atoi(argv[8]);int expected=atoi(argv[9]),discard=atoi(argv[10]);
 uint8_t p[1+EFS_MD_SESS_LEASE_EDGE_LEN]={0};unsigned shard=efs_kv_inode_shard(ino);
 p[0]=efs_raft_shard_group(shard);p[1]=EFS_MD_CMD_SESSION;p[2]=probe?EFS_MD_SESS_LEASE_PROBE:EFS_MD_SESS_LEASE_EDGE;
 if(getenv("EFS_TEST_DEFERRED_LEASE"))p[2]=probe?EFS_MD_SESS_LEASE_PROBE_DEFERRED:EFS_MD_SESS_LEASE_EDGE_DEFERRED;
 assert(strlen(argv[4])==32);for(unsigned i=0;i<16;i++){unsigned b;assert(sscanf(argv[4]+2*i,"%2x",&b)==1);p[3+i]=b;}
 be(p+19,1,4);be(p+23,seq,8);be(p+31,ack,8);be(p+39,ino,8);be(p+47,gen,8);be(p+55,flags,4);
 for(unsigned attempt=0;attempt<120;attempt++){
  int fd=efs_connect_tcp("127.0.0.1",port+attempt%4);if(fd<0){usleep(10000);continue;}
  efs_set_recv_timeout(fd,1000);efs_set_send_timeout(fd,1000);
  int rc=efs_send_msg(fd,EFS_MSG_RAFT_MKFS,p,sizeof(p));
  /* Deliberately discard a reply, then resolve solely through a read-only probe. */
  if(discard && !rc){usleep(200000);close(fd);puts("reply discarded");return 0;}
  uint8_t type=0;void *body=NULL;uint32_t len=0;
  if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
  close(fd);
  if(rc){free(body);usleep(10000);continue;}
  assert(type==EFS_MSG_RAFT_MKFS_REPLY && len==sizeof(struct efs_msg_raft_mkfs_reply));
  struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
  if(reply.rc==EFS_ERR_NOT_PRIMARY || (reply.rc==EFS_ERR_BUSY && expected!=EFS_ERR_BUSY)){usleep(10000);continue;}
  printf("rc=%d known=%llu shard=%u group=%u\n",reply.rc,(unsigned long long)reply.salt,shard,p[0]);
  if(reply.rc!=expected){fprintf(stderr,"expected rc=%d\n",expected);return 1;}
  if(probe && expected==EFS_OK)assert(reply.salt==1);
  return 0;
 }
 fprintf(stderr,"lease edge RPC deadline expired\n");return 1;
}
