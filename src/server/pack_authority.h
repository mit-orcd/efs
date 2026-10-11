/* One read-only envelope owns all coverage. Forward the entire immutable
 * cohort to a dual-group replica, never grant local coverage from remote
 * barriers. Exact FileID/epoch reads remain per record; local PUT guards
 * close revocation and exact-object GC races after this observation. */
static int host_pack_authority_peer(struct efs_raft_host *h,int rid,
    const uint8_t *request,uint32_t bytes,struct efs_msg_pack_authority_reply *out)
{
    char addr[64];uint16_t port=0;
    if(rid<0 || rid==h->raft_id || peer_addr(h,rid,addr,sizeof(addr),&port))return EFS_ERR_NET;
    struct efs_conn *conn=server_peer_conn_get(addr,port);if(!conn)return EFS_ERR_NET;
    uint8_t type=0;void *raw=NULL;uint32_t len=0;
    /* Old packed peers ignore unknown opcodes. Probe the existing service
     * before sending 143; compatibility never substitutes for authority. */
    uint8_t capability[4];efs_put_upload_wr32(capability,3);
    if(efs_conn_send_msg(conn,EFS_MSG_PACK_CAPABILITIES,capability,sizeof(capability)) ||
       efs_conn_recv_msg(conn,&type,&raw,&len)) {
        free(raw);server_peer_conn_drop(addr,port,conn);return EFS_ERR_NET;
    }
    int supported=type==EFS_MSG_PACK_CAPABILITIES_REPLY && len==4 &&
                  efs_put_upload_rd32(raw)==3;
    free(raw);raw=NULL;
    if(!supported) {
        server_peer_conn_drop(addr,port,conn);return EFS_ERR_PROTO;
    }
    if(efs_conn_send_msg(conn,EFS_MSG_PACK_AUTHORITY,request,bytes) ||
       efs_conn_recv_msg(conn,&type,&raw,&len)) {
        free(raw);server_peer_conn_drop(addr,port,conn);return EFS_ERR_NET;
    }
    if(type!=EFS_MSG_PACK_AUTHORITY_REPLY || len!=sizeof(*out)) {
        free(raw);server_peer_conn_drop(addr,port,conn);return EFS_ERR_PROTO;
    }
    struct efs_msg_pack_authority_reply reply;memcpy(&reply,raw,sizeof(reply));free(raw);
    if(reply.version!=out->version || reply.group!=out->group || reply.count!=out->count ||
       memcmp(reply.digest,out->digest,EFS_HASH_SIZE)) {
        server_peer_conn_drop(addr,port,conn);return EFS_ERR_PROTO;
    }
    server_peer_conn_release(addr,port,conn);*out=reply;return EFS_OK;
}
void server_raft_host_pack_authority(const uint8_t *request,uint32_t bytes,
                                   struct efs_msg_pack_authority_reply *out)
{
    struct efs_raft_host *h=g_host;
    struct efs_pack_object object,current;
    struct efs_pack_ref refs[EFS_PACK_AUTHORITY_MAX];uint32_t count=0;
    memset(out,0,sizeof(*out));out->rc=EFS_ERR_INVAL;
    if(!h || !h->running || h->s->storage_engine || !request || bytes<2 ||
       bytes>EFS_PACK_UPLOAD_MAX+2 || request[0]!=EFS_PACK_AUTHORITY_VERSION)return;
    int rc=efs_pack_upload_decode(request+2,bytes-2,&object,refs,EFS_PACK_AUTHORITY_MAX,&count);
    if(rc || request[1]!=object.ledger.publication_group)return;
    out->version=EFS_PACK_AUTHORITY_VERSION;out->group=request[1];out->count=count;
    efs_hash(request,bytes,out->digest);
    uint8_t groups[2];host_need_both(groups);
    if(!host_hosts_all(h,groups,2)) {
        int skip=-1,last=EFS_ERR_NET,unsupported=0;
        for(int tries=0;tries<h->n;tries++) {
            int rid=host_pick_peer(h,groups,2,skip);if(rid<0)break;
            rc=host_pack_authority_peer(h,rid,request,bytes,out);skip=rid;
            if(!rc && out->rc!=EFS_ERR_NOT_PRIMARY)return;
            if(rc==EFS_ERR_PROTO)unsupported=1;
            last=rc?rc:out->rc;
        }
        /* Unsupported read-only service permits independently fresh legacy
         * checks. A positive result is never inferred from a missing peer. */
        out->rc=unsupported?EFS_ERR_PROTO:last;return;
    }
    uint8_t covered=0;int hint=-1;
    for(int i=0;i<2;i++) {
        rc=host_read_index(h,groups[i],&hint);
        if(rc){out->rc=rc;return;}
        covered|=(uint8_t)(1u<<groups[i]);
    }
    uint32_t shard=object.ledger.publication_group==EFS_RAFT_GROUP_SHARD?1:2;
    uint8_t query[HOST_SESS_HDR_LEN+4]={EFS_MD_CMD_SESSION,HOST_SESS_GET};
    memcpy(query+2,object.ledger.owner.client_uuid,16);wr32be(query+18,shard);
    uint8_t owner=efs_raft_shard_group(efs_kv_session_shard(object.ledger.owner.client_uuid));
    uint64_t salt=0;
    /* Both owner and participant barriers were obtained in THIS request. */
    rc=host_session_get_covered(h,query,sizeof(query),owner,&hint,&salt,covered);
    if(!rc) {
        uint32_t epoch=(uint32_t)salt;uint8_t state=(uint8_t)(salt>>32);
        if(epoch!=object.ledger.owner.session_epoch)rc=EFS_ERR_STALE;
        else if(state==EFS_SESSION_FENCING)rc=EFS_ERR_BUSY;
        else if(state!=EFS_SESSION_ACTIVE)rc=EFS_ERR_PROTO;
        else if(!(salt&(1ull<<40)))rc=EFS_ERR_BUSY;
    }
    if(!rc)rc=efs_session_accept(h->kv,shard,object.ledger.owner.client_uuid,object.ledger.owner.session_epoch);
    if(!rc)rc=efs_pack_admission_get(h->kv,&object,&current);
    if(!rc && current.ledger.phase!=EFS_PACK_ACTIVE)rc=EFS_ERR_STALE;
    for(uint32_t i=0;!rc && i<count;i++) {
        if(current.ledger.state[i]>EFS_PACK_LIVE || efs_ino_is_meta_table(refs[i].ino)){rc=EFS_ERR_STALE;break;}
        struct efs_msg_inode_writer_view req={refs[i].ino,refs[i].inode_generation,0,0};
        struct efs_msg_inode_writer_view_reply reply;
        server_raft_host_writer_view_reuse(&req,&reply,&covered);
        if(reply.status!=EFS_INODE_RPC_OK) {
            rc=reply.status==EFS_INODE_RPC_STALE || reply.status==EFS_INODE_RPC_NOT_FOUND?EFS_ERR_STALE:
                reply.status==EFS_INODE_RPC_BUSY?EFS_ERR_BUSY:
                reply.status==EFS_INODE_RPC_NOT_PRIMARY?EFS_ERR_NOT_PRIMARY:EFS_ERR_IO;
        } else if(efs_writer_view_reply_valid(&req,&reply))rc=EFS_ERR_PROTO;
        else if(reply.authority_epoch!=refs[i].content_epoch)rc=EFS_ERR_STALE;
    }
    out->rc=rc;
}
