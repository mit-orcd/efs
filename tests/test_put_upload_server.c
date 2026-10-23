/* Actual wire decoder, checksums and durable fence; mock authority and disk. */
#include "../src/server/put_fence.c"
#include "../src/server/put_upload.c"
#include <assert.h>
#include "test_tmpdir.h"
static struct efsd_server server;
static struct efs_export export;
static struct efs_put_ticket expected;
static uint8_t expected_wire[EFS_PUT_TICKET_WIRE_LEN];
static int authority_rc,store_rc,race_fence,stores,authorities,export_refs;
static uint32_t authority_state=EFS_PUT_TICKET_ADMITTED;
static uint8_t inode_status=EFS_INODE_RPC_OK;
static uint64_t inode_gen=7;
__thread int efs_tls_path_hint,efs_tls_path_used;
void server_raft_host_submit(const uint8_t *p,uint32_t bytes,struct efs_msg_raft_mkfs_reply *r)
{
 assert(bytes==2+EFS_PUT_TICKET_WIRE_LEN&&p[1]==EFS_MD_REQ_PUT_TICKET_STATUS);
 authorities++;memset(r,0,sizeof(*r));r->rc=authority_rc;r->salt=authority_state;
 if(memcmp(p+2,expected_wire,sizeof(expected_wire)))r->rc=EFS_ERR_PROTO;
 if(race_fence)assert(!server_put_session_fence(&server,expected.id.client_uuid,2));
}
void server_raft_host_writer_view(const struct efs_msg_inode_writer_view *q,struct efs_msg_inode_writer_view_reply *r)
{assert(q->ino==expected.ino);memset(r,0,sizeof(*r));r->status=inode_status;r->ino=q->ino;r->generation=inode_gen;r->chunk_index=q->chunk_index;}
struct efs_export *server_export_acquire_or_create_locked(struct efsd_server *s,efs_export_id_t id)
{assert(s==&server&&id==1);export_refs++;return &export;}
void server_export_put(struct efsd_server *s,struct efs_export *ex)
{assert(s==&server);if(ex){assert(ex==&export&&export_refs==1);export_refs--;}}
static int store(void *ctx,const struct efs_frag_id *id,const uint8_t *data,uint32_t bytes,const uint8_t sum[32])
{
 assert(ctx==&server&&id->ino==expected.ino&&id->chunk_generation==expected.object&&id->inode_generation==7);
 assert(id->fragment_index==0&&bytes==expected.fragment_len&&data[0]==91&&sum[0]==expected.checksums[0][0]);
 assert(efs_tls_path_hint==-1);stores++;efs_tls_path_used=1;return store_rc;
}
void efs_store_nvme_bind(struct efs_store *st,struct efs_nvme_store *ctx,struct efsd_server *s,struct efs_export *ex)
{static const struct efs_store_ops ops={.put=store};(void)ctx;assert(s==&server&&ex==&export);st->ops=&ops;st->ctx=s;}
int main(void)
{
 char root[EFS_TEST_PATH];efs_test_path(root,sizeof(root),"efs-put-upload-XXXXXX");assert(efs_test_mkdtemp(root));
 server.storage_path_count=1;server.id=1;strcpy(server.storage_paths[0],root);pthread_mutex_init(&server.lock,NULL);
 export.id=1;export.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
 struct efs_meta_pub p={.ino=4097,.inode_gen=7,.coding_profile_id=EFS_META_PROFILE_K2F1,.fresh_object=1,.put_fragment_len=EFS_MIN_CHUNK_SIZE/2,.put_member_mask=15};
 p.put_id.client_uuid[0]=1;p.put_id.session_epoch=1;p.put_id.seq=1;p.publication_id=p.put_id;
 p.candidate_gen=efs_meta_candidate_gen(p.put_id.client_uuid,1,1,0,0);
 uint32_t bytes=EFS_PUT_UPLOAD_HEADER_LEN+p.put_fragment_len;uint8_t *frame=malloc(bytes+1);assert(frame);
 memset(frame+EFS_PUT_UPLOAD_HEADER_LEN,91,p.put_fragment_len);
 for(unsigned i=0;i<3;i++){p.ch.nodes[i]=i+1;efs_hash(frame+EFS_PUT_UPLOAD_HEADER_LEN,p.put_fragment_len,p.ch.checksums[i]);}
 assert(!efs_put_ticket_from_pub(&p,&expected));assert(!efs_put_ticket_encode(&expected,expected_wire));
 assert(!efs_put_upload_header(&expected,1,0,frame));uint8_t path=0;
 assert(server_put_upload(&server,frame,bytes-1,&path)==EFS_ERR_INVAL&&path==255&&!stores&&!authorities);
 assert(server_put_upload(&server,frame,bytes+1,&path)==EFS_ERR_INVAL&&!stores);
 frame[3]=2;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_INVAL&&!stores);frame[3]=1;
 frame[7]=1;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_INVAL&&!stores);frame[7]=0;
 frame[bytes-1]^=1;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_PROTO&&!stores&&!authorities);frame[bytes-1]^=1;
 frame[8+56]^=1;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_PROTO&&!stores);frame[8+56]^=1;
 authority_rc=EFS_ERR_NOT_FOUND;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_NOT_FOUND&&!stores);
 authority_rc=EFS_ERR_IO;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_IO&&!stores);authority_rc=0;
 authority_state=EFS_PUT_TICKET_PUBLISHED;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_STALE&&!stores);
 authority_state=EFS_PUT_TICKET_RECLAIMING;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_STALE&&!stores);authority_state=1;
 inode_gen=8;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_STALE&&!stores);inode_gen=7;
 inode_status=EFS_INODE_RPC_NOT_FOUND;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_STALE&&!stores);inode_status=0;
 export.chunk_size=EFS_DEFAULT_CHUNK_SIZE*2;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_INVAL&&!stores&&!export_refs&&export.chunk_size==EFS_DEFAULT_CHUNK_SIZE*2);export.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
 store_rc=EFS_ERR_IO;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_IO&&path==255&&stores==1&&!export_refs);
 store_rc=0;assert(!server_put_upload(&server,frame,bytes,&path)&&path==1&&stores==2&&!export_refs);
 assert(!server_put_upload(&server,frame,bytes,&path)&&stores==3); /* exact retry */
 race_fence=1;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_STALE&&stores==3&&!export_refs);
 race_fence=0;assert(server_put_upload(&server,frame,bytes,&path)==EFS_ERR_STALE&&stores==3);
 free(frame);char cert[EFS_MAX_PATH+80];pthread_mutex_t *mu;assert(!storage_fence_path(&server,expected.id.client_uuid,cert,&mu));assert(!unlink(cert));*strrchr(cert,'/')=0;assert(!rmdir(cert));*strrchr(cert,'/')=0;assert(!rmdir(cert));assert(!rmdir(root));
 puts("ticketed PUT receiver: exact frame/checksum/FileID, ADMITTED-only authority, IO retention, exact retry and revoke-before-store floor PASS");
}
