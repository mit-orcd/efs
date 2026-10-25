#include "efs/record_decode.h"
#include "server_internal.h"
#include "dest_fault.h"
#include "efs/put_upload.h"
#include "efs/store.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include "efs/meta_apply.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* The one data PUT path (K13): every fragment names an ADMITTED ticket.
 * Ticket authority is read before taking the local write guard. Holding the
 * guard over a metadata RPC can deadlock a Raft apply waiting to seal it.
 * A concurrently completed revocation is caught by the durable floor before
 * storage; a seal in progress waits for this guarded store to finish. */
static int put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                      uint8_t *path_used);

static int put_refuse_stopping(const struct efs_put_ticket *t,uint32_t fi)
{
    static uint64_t refused;
    uint64_t n=__atomic_add_fetch(&refused,1,__ATOMIC_RELAXED);
    if(n<=10 || !(n%1000))
        fprintf(stderr,"put-upload: stopping; fragment not acknowledged ino=%llu ci=%u fi=%u "
                "(%llu so far)\n",(unsigned long long)t->ino,t->ci,fi,(unsigned long long)n);
    return EFS_ERR_IO;
}

#if EFS_FAULTS
/* K34 fault hooks (tests/test_k34_stop_put.py), EFS_FAULTS builds only (A6):
 * EFS_FAULT_PUT_BUSY_FILE=<path> answers this member's fragment PUTs BUSY
 * while <path> exists (a ticket read during a group leader change);
 * EFS_FAULT_PUT_STOP_AFTER=<n> raises SIGTERM on this member right after it
 * stored its n-th fragment, so the orderly stop begins with that PUT in
 * hand. */
static int put_test_busy(void)
{
    static const char *path;
    static int init;
    if(!__atomic_load_n(&init,__ATOMIC_ACQUIRE)){
        path=getenv("EFS_FAULT_PUT_BUSY_FILE");__atomic_store_n(&init,1,__ATOMIC_RELEASE);
    }
    if(!path || !*path || access(path,F_OK)!=0)return 0;
    static uint64_t n;
    if(__atomic_add_fetch(&n,1,__ATOMIC_RELAXED)<=10)
        fprintf(stderr,"put-upload: EFS_FAULT_PUT_BUSY_FILE present; answering BUSY\n");
    return 1;
}

static void put_test_stop_after_store(void)
{
    static int limit=-1;
    static uint32_t stored;
    int n=__atomic_load_n(&limit,__ATOMIC_ACQUIRE);
    if(n<0){
        const char *e=getenv("EFS_FAULT_PUT_STOP_AFTER");
        n=e?atoi(e):0;__atomic_store_n(&limit,n,__ATOMIC_RELEASE);
    }
    if(n<=0 || __atomic_add_fetch(&stored,1,__ATOMIC_RELAXED)!=(uint32_t)n)return;
    fprintf(stderr,"put-upload: EFS_FAULT_PUT_STOP_AFTER=%d reached; raising SIGTERM\n",n);
    raise(SIGTERM);
}
#else
static int put_test_busy(void) { return 0; }
static void put_test_stop_after_store(void) { }
#endif

int server_put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                       uint8_t *path_used)
{
    int rc=put_upload(s,body,bytes,path_used);
    static int debug=-1; /* idempotent first read on any connection thread (TSan) */
    int dbg=__atomic_load_n(&debug,__ATOMIC_ACQUIRE);
    if(dbg<0){dbg=getenv("EFS_PUT_TICKET_DBG")!=NULL;__atomic_store_n(&debug,dbg,__ATOMIC_RELEASE);}
    if(rc!=EFS_OK && dbg && bytes>=EFS_PUT_UPLOAD_HEADER_LEN)
        fprintf(stderr,"put-upload refused fi=%u seq=%llu rc=%d\n",efs_put_upload_rd32(body+4),
                (unsigned long long)(((uint64_t)efs_put_upload_rd32(body+28)<<32)|efs_put_upload_rd32(body+32)),rc);
    return rc;
}
static int put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                      uint8_t *path_used)
{
    if(path_used)*path_used=0xff;
    if(!s || !body || !path_used || bytes<EFS_PUT_UPLOAD_HEADER_LEN)return EFS_ERR_INVAL;
    uint32_t export_id=1,fi;
    struct efs_put_ticket ticket;
    int rc=efs_put_upload_header_decode(body,EFS_PUT_UPLOAD_HEADER_LEN,&ticket,&fi);
    if(rc!=EFS_OK || ticket.nodes[fi]!=s->id ||
       bytes-EFS_PUT_UPLOAD_HEADER_LEN!=ticket.fragment_len)return EFS_ERR_INVAL;
    uint8_t checksum[EFS_HASH_SIZE];
    const uint8_t *data=body+EFS_PUT_UPLOAD_HEADER_LEN;
    efs_hash(data,ticket.fragment_len,checksum);
    if(memcmp(checksum,ticket.checksums[fi],EFS_HASH_SIZE))return EFS_ERR_PROTO;

    /* K34: a member in orderly stop acknowledges no fragment. The client
     * counts the refusal (PUT_CHUNK_ERROR) as a member that is gone, so a
     * two-fragment object names the leaving member as missing (W154) and
     * never holds one of its two fragments there. */
    if(server_stop_requested())return put_refuse_stopping(&ticket,fi);
    if(put_test_busy())return EFS_ERR_BUSY;
    if((rc=server_test_dest_fault("put"))!=EFS_OK)return rc;

    uint8_t query[2+EFS_PUT_TICKET_WIRE_LEN];
    query[0]=efs_raft_shard_group(efs_kv_lane_shard(ticket.ino,ticket.ci%EFS_META_LANES));
    query[1]=EFS_MD_REQ_PUT_TICKET_STATUS;memcpy(query+2,body+8,EFS_PUT_TICKET_WIRE_LEN);
    struct efs_msg_raft_mkfs_reply authority;
    server_raft_host_submit(query,sizeof(query),&authority);
    if(authority.rc!=EFS_OK && getenv("EFS_PUT_TICKET_DBG"))
        fprintf(stderr,"put-upload status rc=%d hint=%d group=%u\n",authority.rc,
                authority.leader_hint,query[0]);
    if(authority.rc!=EFS_OK)return authority.rc;
    if(authority.salt!=EFS_PUT_TICKET_ADMITTED)return EFS_ERR_STALE;

    /* The ADMITTED ticket under a fresh ReadIndex is the storage authority.
     * Its FileID is enforced at publication; a deleted file's ticket settles
     * GONE and its body joins the orphan ledger. No typed-writer lane view
     * here: on the REPORT path it is BUSY until REPORT bootstraps the lane,
     * which refused fragments and left chunks with two of three (K13). */

    struct efs_put_storage_guard guard={0};
    rc=server_put_session_begin(s,ticket.id.client_uuid,ticket.id.session_epoch,&guard);
    if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(&s->lock);
    struct efs_export *ex=server_export_acquire_or_create_locked(s,export_id);
    /* A storage node learns the data geometry from its first PUT, here
     * only from a ticket that is ADMITTED under a live session. */
    /* Writer threads read chunk_size lock-free (server_frag_len): the one
     * store that sets the geometry is atomic (TSan, F1 gate). */
    if(ex && (ex->chunk_size==0 || ex->chunk_size==EFS_DEFAULT_CHUNK_SIZE))
        __atomic_store_n(&ex->chunk_size,ticket.fragment_len*2u,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&s->lock);
    if(!ex)rc=EFS_ERR_NOMEM;
    else if(server_frag_len(ex,ticket.ino)!=ticket.fragment_len)rc=EFS_ERR_INVAL;
    else {
        struct efs_store store;struct efs_nvme_store context;
        struct efs_frag_id fid={.export_id=export_id,.ino=ticket.ino,
            .chunk_index=ticket.ci,.fragment_index=fi,.chunk_generation=ticket.object,
            .inode_generation=ticket.inode_gen,.coding_profile_id=ticket.coding_profile};
        efs_store_nvme_bind(&store,&context,s,ex);
        efs_tls_path_used=-1;efs_tls_path_hint=-1;
        rc=efs_store_put(&store,&fid,data,ticket.fragment_len,ticket.checksums[fi]);
        efs_tls_path_hint=-1;
        if(rc==EFS_OK && efs_tls_path_used>=0 && efs_tls_path_used<256)
            *path_used=(uint8_t)efs_tls_path_used;
        efs_tls_path_used=-1;
        if(rc==EFS_OK)put_test_stop_after_store();
    }
    server_export_put(s,ex);
    server_put_session_end(&guard);
    /* A stop that began while the fragment was stored: stored, never
     * acknowledged (an unacknowledged object is the orphan path's). */
    if(rc==EFS_OK && server_stop_requested()){*path_used=0xff;rc=put_refuse_stopping(&ticket,fi);}
    return rc;
}

static void put_wr64(uint8_t *p,uint64_t v){efs_put_upload_wr32(p,(uint32_t)(v>>32));efs_put_upload_wr32(p+4,(uint32_t)v);}
static uint64_t put_rd64(const uint8_t *p){return ((uint64_t)efs_put_upload_rd32(p)<<32)|efs_put_upload_rd32(p+4);}

/* K13: one degraded mark on the wire (EFS_REBUILD_MARK_LEN), the
 * DEGRADED_CLEAR body without its tag and rebuilt byte. */
void server_degraded_mark_encode(uint8_t *q, const struct efs_meta_degraded *d)
{
    put_wr64(q, d->ino);
    put_wr64(q + 8, d->inode_gen);
    put_wr64(q + 16, d->object_gen);
    efs_put_upload_wr32(q + 24, d->chunk_index);
    q[28] = d->lane;
    q[29] = d->missing_mask;
    q += 30;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++, q += 4)
        efs_put_upload_wr32(q, d->nodes[i]);
    memcpy(q, d->checksums, sizeof(d->checksums));
}

int server_degraded_mark_decode(const uint8_t *q, struct efs_meta_degraded *d)
{
    return efs_degraded_mark_decode(q, EFS_REBUILD_MARK_LEN, d);
}

/* W154 rebuild of a published object's missing fragment (K13): no ticket
 * names a published object, so the durable degraded mark is the authority.
 * The fragment must be the mark's one missing fragment, placed on this node,
 * with the published checksum. */
int server_rebuild_put(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                       uint8_t *path_used)
{
    if(path_used)*path_used=0xff;
    if(!s || !body || !path_used || bytes<EFS_REBUILD_PUT_HEADER_LEN)return EFS_ERR_INVAL;
    struct efs_meta_degraded d;
    uint32_t export_id=efs_put_upload_rd32(body);
    if(export_id!=1 || server_degraded_mark_decode(body+4,&d)!=EFS_OK)return EFS_ERR_INVAL;
    uint32_t fi=(uint32_t)__builtin_ctz(d.missing_mask);
    const uint8_t *data=body+EFS_REBUILD_PUT_HEADER_LEN;
    uint32_t len=bytes-EFS_REBUILD_PUT_HEADER_LEN;
    if(efs_ino_is_meta_table(d.ino) || d.nodes[fi]!=s->id)return EFS_ERR_INVAL;
    uint8_t checksum[EFS_HASH_SIZE];
    efs_hash(data,len,checksum);
    if(memcmp(checksum,d.checksums[fi],EFS_HASH_SIZE))return EFS_ERR_PROTO;
    int rc=server_raft_host_degraded_check(body+4);
    if(rc!=EFS_OK)return rc==EFS_ERR_NOT_FOUND?EFS_ERR_STALE:rc;
    pthread_mutex_lock(&s->lock);
    struct efs_export *ex=server_export_acquire_or_create_locked(s,export_id);
    pthread_mutex_unlock(&s->lock);
    if(!ex)return EFS_ERR_NOMEM;
    if(server_frag_len(ex,d.ino)!=len)rc=EFS_ERR_INVAL;
    else {
        struct efs_store store;struct efs_nvme_store context;
        struct efs_frag_id fid={.export_id=export_id,.ino=d.ino,.chunk_index=d.chunk_index,
            .fragment_index=fi,.chunk_generation=d.object_gen};
        efs_store_nvme_bind(&store,&context,s,ex);
        efs_tls_path_used=-1;efs_tls_path_hint=-1;
        rc=efs_store_put(&store,&fid,data,len,d.checksums[fi]);
        efs_tls_path_hint=-1;
        if(rc==EFS_OK && efs_tls_path_used>=0 && efs_tls_path_used<256)
            *path_used=(uint8_t)efs_tls_path_used;
        efs_tls_path_used=-1;
    }
    server_export_put(s,ex);
    if(getenv("EFS_PUT_TICKET_DBG"))
        fprintf(stderr,"rebuild-put ino=%llu ci=%u fi=%u rc=%d\n",(unsigned long long)d.ino,
                d.chunk_index,fi,rc);
    return rc;
}
