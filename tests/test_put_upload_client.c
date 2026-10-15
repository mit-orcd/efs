#include "../src/client/put_upload.c"
#include <assert.h>
#include <stdio.h>
struct efs_client g_client;
static struct efs_put_ticket ticket;
static struct efs_conn connection;
static unsigned probes,sends,releases,drops;
static uint32_t observed=1;
static int probe_rc,send_fail,recv_fail,expired,expire_on_reply,malformed,reply_rc;
static uint64_t deadline;
int efs_client_put_manifest(const struct efs_client_put_request *r,struct efs_put_ticket *out)
{assert(r);*out=ticket;return EFS_OK;}
int efs_client_put_resume(struct efs_client_put_request *r,int only,uint32_t *state)
{assert(r&&only==1);probes++;*state=observed;return probe_rc;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_rpc_past_deadline(void){return expired||(deadline==1);}
struct efs_conn *efs_client_conn_get(efs_node_id_t id){assert(id==1);return &connection;}
void efs_client_commit_note(efs_node_id_t id,uint64_t epoch){assert(id==1&&epoch==123);}
void efs_client_conn_release(efs_node_id_t id,struct efs_conn *c){assert(id==1&&c==&connection);releases++;}
void efs_client_conn_drop(efs_node_id_t id,struct efs_conn *c){assert(id==1&&c==&connection);drops++;}
int efs_conn_send_msg_parts(struct efs_conn *c,uint8_t type,const void *header,uint32_t hl,const void *data,uint32_t dl)
{
 assert(c==&connection&&type==EFS_MSG_PUT_TICKETED&&hl==EFS_PUT_UPLOAD_HEADER_LEN&&dl==32);
 assert(efs_put_upload_rd32(header)==1&&efs_put_upload_rd32((const uint8_t *)header+4)==0);
 struct efs_put_ticket decoded;assert(!efs_put_ticket_decode((const uint8_t *)header+8,EFS_PUT_TICKET_WIRE_LEN,&decoded));
 assert(!memcmp(&decoded,&ticket,sizeof(ticket))&&((const uint8_t *)data)[0]==91);sends++;return send_fail;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **payload,uint32_t *bytes)
{
 assert(c==&connection);*payload=NULL;if(recv_fail)return -1;
 uint8_t *p=malloc(EFS_PUT_UPLOAD_REPLY_LEN);assert(p);efs_put_upload_wr32(p,(uint32_t)reply_rc);p[4]=2;efs_put_epoch_write(p+5,123);
 if(expire_on_reply)expired=1;
 *payload=p;*type=malformed?EFS_MSG_PUT_CHUNK_REPLY:EFS_MSG_PUT_TICKETED_REPLY;*bytes=EFS_PUT_UPLOAD_REPLY_LEN;return 0;
}
int main(void)
{
 struct efs_meta_pub p={.ino=4097,.inode_gen=7,.coding_profile_id=EFS_META_PROFILE_K2F1,.fresh_object=1,.put_fragment_len=32,.put_member_mask=15};
 p.put_id.client_uuid[0]=1;p.put_id.session_epoch=1;p.put_id.seq=1;p.publication_id=p.put_id;
 p.candidate_gen=efs_meta_candidate_gen(p.put_id.client_uuid,1,1,0,0);
 uint8_t data[32];memset(data,91,sizeof(data));
 for(unsigned i=0;i<3;i++){p.ch.nodes[i]=i+1;efs_hash(data,sizeof(data),p.ch.checksums[i]);}
 assert(!efs_put_ticket_from_pub(&p,&ticket));g_client.export_id=1;
 struct efs_client_put_request *owner=(void *)&ticket;uint8_t path=0;
 deadline=1;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_BUSY&&!sends&&!probes&&deadline==1);deadline=0;
 data[31]^=1;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_PROTO&&!probes&&!sends);data[31]^=1;
 observed=2;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_STALE&&!sends&&deadline==0);observed=1;
 probe_rc=EFS_ERR_STALE;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_STALE&&!sends);probe_rc=0;
 recv_fail=1;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_NET&&sends==1&&drops==1&&path==255&&!deadline);
 recv_fail=0;assert(!efs_client_put_fragment(owner,0,data,32,&path)&&sends==2&&releases==1&&path==2&&!deadline);
 reply_rc=EFS_ERR_QUOTA;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_QUOTA&&path==255);reply_rc=0;
 malformed=1;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_PROTO&&path==255);malformed=0;
 expire_on_reply=1;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_BUSY&&path==255&&!deadline);expired=0;expire_on_reply=0;
 send_fail=1;unsigned before=drops;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_NET&&drops==before+1&&path==255&&!deadline);send_fail=0;
 reply_rc=1;assert(efs_client_put_fragment(owner,0,data,32,&path)==EFS_ERR_PROTO);reply_rc=0;
 puts("ticketed PUT client: immutable fragments, ADMITTED-only fresh probe, bounded inherited deadline, lost reply drop and exact retry PASS");
}
