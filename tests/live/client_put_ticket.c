/* Opt-in actual durable ticket transport on disposable private fixture stores.
 * Exercises staged ticketed data-plane PUT only; no abandoned collection or activation. */
#include "../../src/client/client_internal.h"
#include "efs/put_ticket.h"
#include "efs/put_upload.h"
#include "efs/checksum.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static void fence(unsigned port,const char *mgmt,const struct efs_opid *id)
{
    char hex[33],address[64];for(unsigned i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",id->client_uuid[i]);
    snprintf(address,sizeof(address),"127.0.0.1:%u",port);pid_t child=fork();assert(child>=0);
    if(!child){execl(mgmt,mgmt,"raft-session",address,"fence",hex,(char *)NULL);_exit(127);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void fragment_probe(unsigned port,const struct efs_put_ticket *ticket,int cold,int expected)
{
 uint8_t *data=malloc(ticket->fragment_len);assert(data);memset(data,91,ticket->fragment_len);
 for(unsigned fi=0;fi<3;fi++) {
  int fd=efs_connect_tcp("127.0.0.1",port+ticket->nodes[fi]-1);assert(fd>=0);
  efs_set_recv_timeout(fd,15000);efs_set_send_timeout(fd,15000);
  if(cold) {
   struct efs_msg_get_chunk get={.export_id=1,.ino=ticket->ino,.chunk_index=ticket->ci,
     .fragment_index=fi,.chunk_generation=ticket->object};
   assert(!efs_send_msg(fd,EFS_MSG_GET_CHUNK,&get,sizeof(get)));
   uint8_t type;void *reply=NULL;uint32_t bytes;
   assert(!efs_recv_msg(fd,&type,&reply,&bytes));
   assert(type==EFS_MSG_GET_CHUNK_REPLY&&bytes==1+32+ticket->fragment_len&&*(uint8_t *)reply==EFS_GET_CHUNK_OK);
   assert(!memcmp((uint8_t *)reply+1,ticket->checksums[fi],32));
   assert(!memcmp((uint8_t *)reply+33,data,ticket->fragment_len));free(reply);
  }
  uint8_t header[EFS_PUT_UPLOAD_HEADER_LEN];assert(!efs_put_upload_header(ticket,1,fi,header));
  assert(!efs_send_msg_parts(fd,EFS_MSG_PUT_TICKETED,header,sizeof(header),data,ticket->fragment_len));
  uint8_t type;void *reply=NULL;uint32_t bytes;
  assert(!efs_recv_msg(fd,&type,&reply,&bytes));close(fd);
  assert(type==EFS_MSG_PUT_TICKETED_REPLY&&bytes==EFS_PUT_UPLOAD_REPLY_LEN);
  int rc=(int32_t)efs_put_upload_rd32(reply);free(reply);
  if(rc!=expected)fprintf(stderr,"ticket upload fi=%u rc=%d expected=%d\n",fi,rc,expected);
  assert(rc==expected);
 }
 free(data);
}
int main(int argc,char **argv)
{
    assert(argc==5 || argc==6);unsigned port=atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],0,10);
    int cold=!strcmp(argv[4],"cold"),save=!strcmp(argv[4],"save");
    int discard=atoi(argv[4]);assert(discard==0 || discard==1);
    assert((!cold && !save) || argc==6);
    g_client.export_id=1;g_client.node_count=4;
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
    efs_client_conn_init();uint64_t generation=0;uint32_t count=1;
    assert(!efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&generation) && generation);
    if(cold) {
        uint8_t bytes[EFS_PUT_TICKET_WIRE_LEN];struct efs_put_ticket t;uint32_t state=0;
        FILE *in=fopen(argv[5],"rb");assert(in && fread(bytes,1,sizeof(bytes),in)==sizeof(bytes) && fgetc(in)==EOF);fclose(in);
        assert(!efs_put_ticket_decode(bytes,sizeof(bytes),&t) && t.ino==ino && t.inode_gen==generation);
        assert(!efs_client_rpc_put_ticket(&t,1,&state) && state==EFS_PUT_TICKET_ADMITTED);
        assert(!efs_client_rpc_put_ticket(&t,0,&state) && state==EFS_PUT_TICKET_ADMITTED);
        fragment_probe(port,&t,1,EFS_OK);
        fence(port,argv[3],&t.id);
        fragment_probe(port,&t,1,EFS_ERR_STALE);
        assert(efs_client_rpc_put_ticket(&t,1,&state)==EFS_ERR_STALE && !state);
        assert(efs_client_rpc_put_ticket(&t,0,&state)==EFS_ERR_STALE && !state);
        puts("actual PUT ticket cold restart: exact body admission and all three fragments survive; completed fence rejects actual late PUT/status/replay PASS");return 0;
    }
    struct efs_meta_pub p={.ino=ino,.inode_gen=generation,.fresh_object=1,.put_fragment_len=EFS_MIN_CHUNK_SIZE/2,
        .put_member_mask=15,.coding_profile_id=EFS_META_PROFILE_K2F1};
    while(efs_raft_shard_group(efs_kv_lane_shard(ino,p.chunk_index%EFS_META_LANES))==efs_raft_shard_group(efs_kv_inode_shard(ino)))p.chunk_index++;
    assert(p.chunk_index<EFS_META_LANES);
    assert(!efs_client_publication_id(1,&p.put_id));p.publication_id=p.put_id;
    p.candidate_gen=efs_meta_candidate_gen(p.put_id.client_uuid,1,p.put_id.seq,p.chunk_index,0);
    uint8_t *fragment=malloc(p.put_fragment_len);assert(fragment);memset(fragment,91,p.put_fragment_len);
    for(unsigned i=0;i<3;i++){p.ch.nodes[i]=i+1;efs_hash(fragment,p.put_fragment_len,p.ch.checksums[i]);}
    free(fragment);
    struct efs_put_ticket t;assert(!efs_put_ticket_from_pub(&p,&t));
    uint32_t shard=efs_kv_lane_shard(ino,p.chunk_index%EFS_META_LANES),state=99;
    struct efs_client_put_request *owned=NULL;
    uint64_t budget_before=0,budget_live=0,budget_reserved=0,budget_limit=0;
    efs_buf_metadata_stats(&budget_before,&budget_reserved,&budget_limit);
    assert(!efs_client_put_begin(&owned,&p));
    efs_buf_metadata_stats(&budget_live,&budget_reserved,&budget_limit);assert(budget_live>budget_before);
    struct efs_put_ticket captured;assert(!efs_client_put_manifest(owned,&captured));
    assert(!memcmp(&captured,&t,sizeof(t)));
    /* Reordering configured slots must not reinterpret an admitted mask. */
    g_client.nodes[0].id=4;g_client.nodes[3].id=1;
    assert(efs_client_put_resume(owned,0,&state)==EFS_ERR_STALE && !state);
    g_client.nodes[0].id=1;g_client.nodes[3].id=4;
    /* Probe has no session or ticket admission side effects. */
    assert(efs_client_rpc_put_ticket(&t,1,&state)==EFS_ERR_NOT_FOUND && !state);
    assert(efs_client_put_resume(owned,1,&state)==EFS_ERR_NOT_FOUND && !state);
    assert(!efs_client_rpc_publication_session(shard,&t.id));
    assert(efs_client_rpc_put_ticket(&t,1,&state)==EFS_ERR_NOT_FOUND && !state);
    if(discard) {
        uint8_t bytes[2+EFS_PUT_TICKET_WIRE_LEN];bytes[0]=efs_raft_shard_group(shard);bytes[1]=EFS_MD_CMD_PUT_TICKET_ADMIT;
        assert(!efs_put_ticket_encode(&t,bytes+2));
        int fd=efs_connect_tcp("127.0.0.1",port);assert(fd>=0);
        assert(!efs_send_msg(fd,EFS_MSG_RAFT_MKFS,bytes,sizeof(bytes)));close(fd);
    }
    assert(!efs_client_put_resume(owned,0,&state) && state==EFS_PUT_TICKET_ADMITTED);
    assert(efs_client_put_free(owned)==EFS_ERR_BUSY);
    p.ch.checksums[0][0]^=1;
    assert(!efs_client_put_manifest(owned,&captured) && !memcmp(&captured,&t,sizeof(t)));
    assert(!efs_client_put_resume(owned,1,&state) && state==EFS_PUT_TICKET_ADMITTED);
    struct efs_put_ticket changed=t;changed.body_digest[0]^=1;
    assert(efs_client_rpc_put_ticket(&changed,0,&state)==EFS_ERR_PROTO && !state);
    assert(efs_client_rpc_put_ticket(&changed,1,&state)==EFS_ERR_PROTO && !state);
    changed=t;changed.ino+=4096;
    assert(efs_client_rpc_put_ticket(&changed,0,&state)==EFS_ERR_PROTO && !state);
    fragment_probe(port,&t,0,EFS_OK);
    if(save) {
        uint8_t bytes[EFS_PUT_TICKET_WIRE_LEN];assert(!efs_put_ticket_encode(&t,bytes));
        FILE *out=fopen(argv[5],"wb");assert(out && fwrite(bytes,1,sizeof(bytes),out)==sizeof(bytes) && !fclose(out));
        puts("actual PUT ticket exact body saved for cold replay PASS");return 0;
    }
    fence(port,argv[3],&t.id);
    fragment_probe(port,&t,1,EFS_ERR_STALE);
    assert(efs_client_put_resume(owned,0,&state)==EFS_ERR_STALE && !state);
    assert(efs_client_put_free(owned)==EFS_ERR_BUSY);
    efs_buf_metadata_stats(&budget_live,&budget_reserved,&budget_limit);assert(budget_live>budget_before);
    assert(efs_client_rpc_put_ticket(&t,0,&state)==EFS_ERR_STALE && !state);
    assert(efs_client_rpc_put_ticket(&t,1,&state)==EFS_ERR_STALE && !state);
    printf("actual retained PUT ticket discard=%d cross-group shard=%u: budgeted immutable body, membership reorder refusal, pure probes, exact admission/replay, body collision and revoked-owner retention PASS\n",discard,shard);
}
