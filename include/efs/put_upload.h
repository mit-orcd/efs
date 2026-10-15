#ifndef EFS_PUT_UPLOAD_H
#define EFS_PUT_UPLOAD_H
#include "efs/put_ticket.h"
/* New opcode, fixed-endian header: export, fragment index, immutable ticket.
 * Exact fragment bytes follow. No NEW path hint: an ambiguous retry must probe
 * every storage root before choosing a destination. Reply: signed rc + path + big-endian boot verifier. */
#define EFS_PUT_UPLOAD_HEADER_LEN (8u + EFS_PUT_TICKET_WIRE_LEN)
#define EFS_PUT_UPLOAD_REPLY_LEN 13u
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
#endif
