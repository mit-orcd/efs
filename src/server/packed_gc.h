/* Owning-group GC, one bounded page per pass. Network/disk I/O runs outside
 * host/KV locks. The durable object row is the only replay cursor for ACKs. */
static int host_session_cleanup_admit(struct efs_raft_host *,const uint8_t *,uint32_t,uint8_t,int *);
static int host_pack_gc_storage(struct efs_raft_host *h,const struct efs_pack_object *o,
    uint32_t member,int deletion)
{
    uint8_t wire[EFS_PACK_OBJECT_MAX_BYTES+1];size_t n;
    int rc=efs_pack_object_encode(o,wire+1,sizeof(wire)-1,&n);
    if(rc)return rc;
    wire[0]=(uint8_t)deletion;
    if(member==h->s->id)return server_packed_gc(h->s,wire,(uint32_t)n+1);
    char addr[64];uint16_t port;
    if(!member || peer_addr(h,(int)member-1,addr,sizeof(addr),&port))return EFS_ERR_IO;
    struct efs_conn *pc=server_peer_conn_get(addr,port);if(!pc)return EFS_ERR_NET;
    if(pc->kind==EFS_CONN_TCP && pc->fd>=0) {
        efs_set_recv_timeout(pc->fd,HOST_SEND_IO_MS);efs_set_send_timeout(pc->fd,HOST_SEND_IO_MS);
    }
    void *reply=NULL;uint32_t length=0;uint8_t type=0;
    rc=efs_conn_send_msg(pc,EFS_MSG_GC_PACKED,wire,(uint32_t)n+1) ||
       efs_conn_recv_msg(pc,&type,&reply,&length) ? EFS_ERR_NET : EFS_OK;
    if(!rc)rc=type==EFS_MSG_GC_PACKED_REPLY && length==4 ? (int32_t)efs_put_upload_rd32(reply) : EFS_ERR_PROTO;
    free(reply);
    if(!rc) {
        if(pc->kind==EFS_CONN_TCP && pc->fd>=0) {
            efs_set_recv_timeout(pc->fd,EFS_IO_TIMEOUT_MS);efs_set_send_timeout(pc->fd,EFS_IO_TIMEOUT_MS);
        }
        server_peer_conn_release(addr,port,pc);
    } else server_peer_conn_drop(addr,port,pc);
    return rc;
}
struct pack_gc_scan {
    struct efs_pack_object object[8];uint8_t skip[19],last[19];
    uint32_t count,skip_len,visited,last_len;int error;
};
static uint8_t pack_gc_cursor[HOST_NGROUPS][19];
static uint32_t pack_gc_cursor_len[HOST_NGROUPS];
static int pack_gc_scan_cb(void *user,const uint8_t *key,uint32_t kl,const uint8_t *value,uint32_t vl)
{
    struct pack_gc_scan *scan=user;
    if(kl!=19){scan->error=EFS_ERR_PROTO;return 1;}
    if(scan->skip_len && !memcmp(key,scan->skip,19))return 0;
    memcpy(scan->last,key,19);scan->last_len=19;scan->visited++;
    struct efs_pack_object o;
    int rc=efs_pack_object_decode(value,vl,&o);
    if(rc){scan->error=rc;return 1;}
    int pending=0;
    for(uint32_t i=0;i<o.ledger.count;i++)pending|=o.ledger.state[i]==EFS_PACK_PENDING;
    if(o.ledger.phase!=EFS_PACK_ACTIVE || pending)scan->object[scan->count++]=o;
    return scan->count==8 || scan->visited==2048;
}
static int host_pack_gc_command(struct efs_raft_host *h,const struct efs_pack_object *o,uint8_t tag,uint32_t bit,int deletion)
{
    uint8_t cmd[EFS_PACK_OBJECT_MAX_BYTES+6];size_t n;
    cmd[0]=tag;cmd[1]=(uint8_t)deletion;wr32be(cmd+2,bit);
    int rc=efs_pack_object_encode(o,cmd+6,sizeof(cmd)-6,&n);if(rc)return rc;
    /* Only this verified coordinator emits mutation commands. Public control
     * RPC rejects them; leader loss leaves durable progress for the new pass. */
    pthread_mutex_lock(&h->mu);
    struct efs_raft *r=group_raft(h,o->ledger.publication_group);
    int lead=r && efs_raft_role(r)==EFS_RAFT_LEADER;
    pthread_mutex_unlock(&h->mu);
    if(!lead)return EFS_ERR_NOT_PRIMARY;
    return host_gc_propose(h,(uint8_t)o->ledger.publication_group,cmd,(uint32_t)n+6);
}
static void host_gc_pack_pass(struct efs_raft_host *h,uint8_t group,uint32_t anchor)
{
    static struct pack_gc_scan scan;int gi=gc_gi(group);
    uint8_t prefix[3];prefix[0]=(uint8_t)(anchor>>8);prefix[1]=(uint8_t)anchor;prefix[2]=EFS_KV_KIND_PACK_OBJECT;
    memset(&scan,0,sizeof(scan));scan.skip_len=pack_gc_cursor_len[gi];
    if(scan.skip_len)memcpy(scan.skip,pack_gc_cursor[gi],19);
    int rc=efs_kv_scan_from(h->kv,prefix,3,scan.skip_len?scan.skip:NULL,scan.skip_len,pack_gc_scan_cb,&scan);
    if(rc<0 || scan.error){__atomic_fetch_add(&h->s->gc_scan_errors,1,__ATOMIC_RELAXED);return;}
    pack_gc_cursor_len[gi]=scan.last_len;
    if(scan.last_len)memcpy(pack_gc_cursor[gi],scan.last,19);
    uint64_t deadline=now_us_()+GC_FRAG_BUDGET_US;
    for(uint32_t i=0;i<scan.count && h->gc_running;i++) {
        struct efs_pack_object o=scan.object[i];rc=EFS_OK;
        if(o.ledger.publication_group!=group)continue;
        if(o.ledger.phase==EFS_PACK_ACTIVE) {
            uint8_t admit[HOST_SESS_REG_LEN]={EFS_MD_CMD_SESSION,EFS_MD_SESS_LEASE_LOCK_BATCH};int hint=-1;
            memcpy(admit+2,o.ledger.owner.client_uuid,16);wr32be(admit+18,o.ledger.owner.session_epoch);wr32be(admit+22,anchor);
            rc=host_session_cleanup_admit(h,admit,sizeof(admit),group,&hint);
            if(!rc)rc=host_pack_gc_command(h,&o,EFS_MD_CMD_PACK_CANCEL,0,0);
            if(rc)continue;
            rc=efs_pack_lifetime_get(h->kv,&o,&o);if(rc)continue;
        }
        if(o.ledger.phase==EFS_PACK_RECLAIMING) {
            for(int deletion=0;deletion<2 && !rc;deletion++) {
                uint32_t ack=deletion?o.ledger.delete_acks:o.ledger.fence_acks;
                for(uint32_t member=1;member<=EFS_MAX_NODES;member++) {
                    uint32_t bit=1u<<(member-1);
                    if(!(o.ledger.member_mask&bit) || (ack&bit))continue;
                    rc=host_pack_gc_storage(h,&o,member,deletion);
                    if(!rc)rc=host_pack_gc_command(h,&o,EFS_MD_CMD_PACK_GC_ACK,bit,deletion);
                    if(rc)break;
                    rc=efs_pack_lifetime_get(h->kv,&o,&o);if(rc)break;
                    if(now_us_()>deadline)break;
                }
                if(o.ledger.fence_acks!=o.ledger.member_mask)break;
            }
        }
        if(!rc && o.ledger.phase==EFS_PACK_RETIRED)
            (void)host_pack_gc_command(h,&o,EFS_MD_CMD_PACK_GC_RETIRE,0,0);
        if(now_us_()>deadline){pack_gc_cursor_len[gi]=0;break;}
    }
}
