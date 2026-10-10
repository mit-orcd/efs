#include "client_internal.h"
#include "efs/put_upload.h"
#include "efs/checksum.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Caller serializes the retained owner and keeps its immutable fragment bytes
 * through every ambiguous reply. This RPC never cancels or frees that owner. */
int efs_client_put_fragment(struct efs_client_put_request *owner,uint32_t fragment,
                            const uint8_t *data,uint32_t len,uint8_t *path_used)
{
    if(path_used)*path_used=0xff;
    if(!owner || !data || !path_used || fragment>=EFS_NUM_FRAGMENTS)return EFS_ERR_INVAL;
    struct efs_put_ticket ticket;
    int rc=efs_client_put_manifest(owner,&ticket);
    if(rc!=EFS_OK)return rc;
    if(len!=ticket.fragment_len)return EFS_ERR_INVAL;
    uint8_t hash[EFS_HASH_SIZE];efs_hash(data,len,hash);
    if(memcmp(hash,ticket.checksums[fragment],EFS_HASH_SIZE))return EFS_ERR_PROTO;
    uint8_t header[EFS_PUT_UPLOAD_HEADER_LEN];
    rc=efs_put_upload_header(&ticket,g_client.export_id,fragment,header);
    if(rc!=EFS_OK)return rc;
    uint64_t saved=efs_client_rpc_deadline_ms();struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return EFS_ERR_IO;
    uint64_t deadline=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000+15000;
    if(saved && saved<deadline)deadline=saved;
    efs_client_rpc_set_deadline_ms(deadline);
    uint32_t state=0;struct efs_conn *conn=NULL;
    if(efs_client_rpc_past_deadline()){rc=EFS_ERR_BUSY;goto out;}
    rc=efs_client_put_resume(owner,1,&state);
    if(rc==EFS_OK && state!=EFS_PUT_TICKET_ADMITTED)rc=EFS_ERR_STALE;
    if(rc==EFS_OK && efs_client_rpc_past_deadline())rc=EFS_ERR_BUSY;
    if(rc!=EFS_OK)goto out;
    conn=efs_client_conn_get(ticket.nodes[fragment]);
    if(!conn){rc=EFS_ERR_NET;goto out;}
    uint8_t type=0;void *reply=NULL;uint32_t bytes=0;
    if(efs_conn_send_msg_parts(conn,EFS_MSG_PUT_TICKETED,header,sizeof(header),data,len) ||
       efs_conn_recv_msg(conn,&type,&reply,&bytes)) {
        free(reply);efs_client_conn_drop(ticket.nodes[fragment],conn);conn=NULL;
        rc=EFS_ERR_NET;goto out;
    }
    efs_client_conn_release(ticket.nodes[fragment],conn);conn=NULL;
    if(type!=EFS_MSG_PUT_TICKETED_REPLY || bytes!=EFS_PUT_UPLOAD_REPLY_LEN)rc=EFS_ERR_PROTO;
    else {
        rc=(int32_t)efs_put_upload_rd32(reply);
        if(rc>EFS_OK || rc<EFS_ERR_ILSEQ)rc=EFS_ERR_PROTO;
        if(rc==EFS_OK)*path_used=((uint8_t *)reply)[4];
    }
    free(reply);
    /* Late success is ambiguous to the caller's deadline; retain the owner. */
    if(efs_client_rpc_past_deadline())rc=EFS_ERR_BUSY;
out:
    if(rc!=EFS_OK)*path_used=0xff;
    efs_client_rpc_set_deadline_ms(saved);
    return rc;
}
