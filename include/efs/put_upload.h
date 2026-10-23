#ifndef EFS_PUT_UPLOAD_H
#define EFS_PUT_UPLOAD_H
#include "efs/protocol.h"
#include "efs/put_ticket.h"
/* EFS_MSG_PUT_CHUNK (K13): fixed-endian header of export, fragment index and
 * the immutable admitted ticket; exactly ticket.fragment_len bytes follow.
 * No NEW path hint: an ambiguous retry must probe every storage root before
 * choosing a destination. Reply: EFS_PUT_REPLY_LEN bytes, status + path +
 * big-endian boot verifier (efs_put_reply_encode). */
#define EFS_PUT_UPLOAD_HEADER_LEN (8u + EFS_PUT_TICKET_WIRE_LEN)
typedef char efs_put_upload_header_len_check[EFS_PUT_UPLOAD_HEADER_LEN == EFS_PUT_HEADER_LEN ? 1 : -1];
static inline void efs_put_upload_wr32(uint8_t *p,uint32_t value)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(24-8*i));}
static inline uint32_t efs_put_upload_rd32(const uint8_t *p)
{return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static inline int efs_put_upload_header(const struct efs_put_ticket *ticket,
    efs_export_id_t export_id,uint32_t fragment,uint8_t out[EFS_PUT_UPLOAD_HEADER_LEN])
{
 if(!out || !export_id || fragment>=EFS_NUM_FRAGMENTS)return EFS_ERR_INVAL;
 int rc=efs_put_ticket_encode(ticket,out+8);if(rc!=EFS_OK)return rc;
 efs_put_upload_wr32(out,export_id);efs_put_upload_wr32(out+4,fragment);return EFS_OK;
}
/* Storage verdict -> reply status. Only OK is a stored fragment. A ticket
 * that is not ADMITTED or a fenced session is terminal (STALE); leadership
 * and pressure are retryable (BUSY) with the same ticket. */
static inline uint8_t efs_put_reply_status(int rc)
{
 switch(rc){
 case EFS_OK: return EFS_PUT_CHUNK_OK;
 case EFS_ERR_QUOTA: return EFS_PUT_CHUNK_QUOTA_EXCEEDED;
 case EFS_ERR_BUSY: case EFS_ERR_NOT_PRIMARY: case EFS_ERR_NO_QUORUM: case EFS_ERR_NET:
  return EFS_PUT_CHUNK_BUSY;
 case EFS_ERR_STALE: case EFS_ERR_FENCED: return EFS_PUT_CHUNK_STALE;
 default: return EFS_PUT_CHUNK_ERROR;
 }
}
/* Reply status -> client verdict; unknown status is a protocol error. */
static inline int efs_put_reply_rc(uint8_t status)
{
 switch(status){
 case EFS_PUT_CHUNK_OK: return EFS_OK;
 case EFS_PUT_CHUNK_QUOTA_EXCEEDED: return EFS_ERR_QUOTA;
 case EFS_PUT_CHUNK_BUSY: return EFS_ERR_BUSY;
 case EFS_PUT_CHUNK_STALE: return EFS_ERR_STALE;
 case EFS_PUT_CHUNK_ERROR: return EFS_ERR_IO;
 default: return EFS_ERR_PROTO;
 }
}
static inline void efs_put_reply_encode(int rc,uint8_t path,uint64_t boot_epoch,
                                        uint8_t out[EFS_PUT_REPLY_LEN])
{
 out[0]=efs_put_reply_status(rc);out[1]=rc==EFS_OK?path:0xff;
 efs_put_epoch_write(out+2,boot_epoch);
}
/* One received reply frame. A frame of any other type or length, or an OK
 * without a boot verifier, is a protocol error and never a stored fragment. */
static inline int efs_put_reply_decode(uint8_t type,const uint8_t *p,uint32_t len,
                                       uint8_t *path,uint64_t *boot_epoch)
{
 if(path)*path=0xff;
 if(boot_epoch)*boot_epoch=0;
 if(type!=EFS_MSG_PUT_CHUNK_REPLY || !p || len!=EFS_PUT_REPLY_LEN)return EFS_ERR_PROTO;
 int rc=efs_put_reply_rc(p[0]);uint64_t epoch=efs_put_epoch_read(p+2);
 if(rc==EFS_OK && !epoch)return EFS_ERR_PROTO;
 if(rc==EFS_OK && path)*path=p[1];
 if(boot_epoch)*boot_epoch=epoch;
 return rc;
}
#endif
