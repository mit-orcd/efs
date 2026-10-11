#include "client_internal.h"
#include "efs/pack_cohort.h"
#include "efs/writer_state.h"
#include "efs/packed_upload.h"
#include "efs/placement.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/publication_ack.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#define PACK_WRITERS 16u
#define PACK_PUBLISH_WORKERS 4u
#define PACK_PUBLISH_STACK_BYTES (256u << 10)
#define PACK_PUBLISH_STACK_ALIGN (64u << 10)
#define PACK_DELAY_NS 10000000ULL
#define PACK_CAPABILITY_NS 5000000000ULL
struct pack_writer {
    struct efs_writer_plan plan;
    struct efs_publication_ack_queue *acks;
    uint64_t object;
    uint32_t len;
    uint8_t put_bound,published,loaded,done;
};
/* Nonmoving, charged BEFORE accepting any byte. Includes every copy, encoded
 * fragment and wire staging buffer; cache body/sidecar/ACK queues have their
 * own shared-pool charges. One bounded cohort per metadata group. */
struct pack_context {
    struct efs_pack_cohort cohort;
    struct efs_pack_object object;
    struct pack_writer writers[PACK_WRITERS];
    uint8_t snapshot[EFS_MIN_CHUNK_SIZE];
    uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_SEGMENT_MAX_SIZE/2];
    uint8_t wire[EFS_PACKED_PUT_HEADER+EFS_PACK_UPLOAD_MAX];
    /* Charged with this nonmoving context before accepting any byte. Join
     * every helper before reusing either its stack or any writer state. */
    uint8_t publication_stacks[PACK_PUBLISH_WORKERS-1][PACK_PUBLISH_STACK_BYTES+PACK_PUBLISH_STACK_ALIGN];
    uint32_t members,put_mask;
    uint64_t retry_after;
    int busy,last_error,built,stack_busy;
};
/* Only immutable authority/ACK state survives completed publication. Body
 * buffers can serve the next cohort while this bounded slot retains cleanup. */
struct pack_retirement {
    struct efs_pack_object object;
    struct efs_pack_ref refs[PACK_WRITERS];
    struct efs_publication_ack_queue *acks[PACK_WRITERS];
    uint8_t wire[EFS_PACK_UPLOAD_MAX+2];
    uint32_t group,count;
    int active,busy,last_error;
};
static struct pack_retirement *pack_retirements[2];
/* At most two cohorts' worth of charged ACK queues across active + cleanup. */
static unsigned pack_ack_credits;
static pthread_mutex_t pack_mu=PTHREAD_MUTEX_INITIALIZER;
static struct pack_context *pack_groups[2];
static pthread_t pack_tid;
static int pack_started,pack_running;
static uint32_t pack_capability_members;
static uint64_t pack_capability_checked;
static uint64_t pack_now(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
int efs_client_pack_enabled(void)
{
    const char *s=getenv("EFS_PACKED_WRITES");return s && !strcmp(s,"1");
}
static int pack_pause(void)
{
    if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
    usleep(1000);return efs_client_rpc_past_deadline()?EFS_ERR_BUSY:EFS_OK;
}
static int pack_command_wire(uint32_t group,const struct efs_pack_object *object,
    const struct efs_pack_ref *refs,uint32_t count,uint8_t *wire,size_t capacity,uint8_t tag)
{
    size_t n;wire[0]=(uint8_t)group;wire[1]=tag;
    int rc=efs_pack_upload_encode(object,refs,count,wire+2,capacity-2,&n);if(rc)return rc;
    efs_node_id_t target=0;
    for(unsigned attempt=0;attempt<12;attempt++) {
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        efs_node_id_t node=target?target:g_client.nodes[attempt%g_client.node_count].id;
        struct efs_conn *conn=efs_client_conn_get(node);if(!conn){rc=EFS_ERR_NET;continue;}
        uint8_t type=0;void *raw=NULL;uint32_t bytes=0;
        if(efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,wire,(uint32_t)n+2) ||
           efs_conn_recv_msg(conn,&type,&raw,&bytes)) {
            free(raw);efs_client_conn_drop(node,conn);rc=EFS_ERR_NET;continue;
        }
        efs_client_conn_release(node,conn);
        if(type!=EFS_MSG_RAFT_MKFS_REPLY || bytes!=sizeof(struct efs_msg_raft_mkfs_reply)) {
            free(raw);return EFS_ERR_PROTO;
        }
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,raw,sizeof(reply));free(raw);
        rc=reply.rc;
        if(!rc && reply.salt!=(tag==EFS_MD_CMD_PACK_UPLOAD_RETIRE?EFS_PACK_UPLOAD_RETIRED:EFS_PACK_ACTIVE))return EFS_ERR_PROTO;
        if(!rc)return efs_client_rpc_past_deadline()?EFS_ERR_BUSY:EFS_OK;
        if(rc==EFS_ERR_NOT_PRIMARY && reply.leader_hint>=0 && reply.leader_hint<EFS_MAX_NODES)
            target=(efs_node_id_t)reply.leader_hint+1;
        else if(rc!=EFS_ERR_BUSY && rc!=EFS_ERR_NET && rc!=EFS_ERR_NOT_PRIMARY)return rc;
        if(pack_pause())return EFS_ERR_BUSY;
    }
    return rc;
}
static int pack_command(struct pack_context *p,uint8_t tag)
{
    return pack_command_wire(p->cohort.group,&p->object,p->cohort.refs,
        p->cohort.count,p->wire,sizeof(p->wire),tag);
}
/* Publication and independent receipt retirement lease the same already
 * charged stacks. Never run either set of helpers during another lease. */
struct pack_parallel_run {
    void *context;
    int (*record)(void *,uint32_t);
    uint32_t next,count;
    uint64_t deadline;
    int results[PACK_WRITERS];
};
static void *pack_parallel_worker(void *arg)
{
    struct pack_parallel_run *run=arg;
    uint64_t previous=efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(run->deadline);
    for(;;) {
        uint32_t i=__atomic_fetch_add(&run->next,1,__ATOMIC_RELAXED);
        if(i>=run->count)break;
        run->results[i]=run->record(run->context,i);
    }
    efs_client_rpc_set_deadline_ms(previous);return NULL;
}
static int pack_parallel(struct pack_context *p,uint32_t count,
    int (*record)(void *,uint32_t),void *context)
{
    if(!p || !record || count>PACK_WRITERS)return EFS_ERR_INVAL;
    struct pack_parallel_run run={.context=context,.record=record,.count=count,
        .deadline=efs_client_rpc_deadline_ms()};
    pthread_t threads[PACK_PUBLISH_WORKERS-1];unsigned started=0;
    for(unsigned i=0;i<PACK_PUBLISH_WORKERS-1 && i+1<count;i++) {
        pthread_attr_t attr;if(pthread_attr_init(&attr))break;
        uintptr_t stack=((uintptr_t)p->publication_stacks[i]+PACK_PUBLISH_STACK_ALIGN-1)&~(uintptr_t)(PACK_PUBLISH_STACK_ALIGN-1);
        int rc=pthread_attr_setstack(&attr,(void *)stack,PACK_PUBLISH_STACK_BYTES);
        if(!rc)rc=pthread_create(&threads[started],&attr,pack_parallel_worker,&run);
        pthread_attr_destroy(&attr);if(rc)break;
        started++;
    }
    (void)pack_parallel_worker(&run);
    for(unsigned i=0;i<started;i++)pthread_join(threads[i],NULL);
    for(uint32_t i=0;i<count;i++)if(run.results[i])return run.results[i];
    return EFS_OK;
}
static int pack_retire_one(void *context,uint32_t i)
{
    struct pack_retirement *r=context;if(!r->acks[i])return EFS_OK;
    int rc=efs_client_publication_retire(r->acks[i]);if(rc)return rc;
    rc=efs_client_publication_ack_free(r->acks[i]);if(rc)return rc;
    r->acks[i]=NULL;
    pthread_mutex_lock(&pack_mu);pack_ack_credits--;pthread_mutex_unlock(&pack_mu);
    return EFS_OK;
}
static int pack_retire_group(unsigned gi)
{
    pthread_mutex_lock(&pack_mu);
    struct pack_retirement *r=pack_retirements[gi];
    if(!r || !r->active){pthread_mutex_unlock(&pack_mu);return EFS_OK;}
    struct pack_context *p=pack_groups[gi];
    if(!p){pthread_mutex_unlock(&pack_mu);return EFS_ERR_PROTO;}
    if(r->busy || p->busy || p->stack_busy){pthread_mutex_unlock(&pack_mu);return EFS_ERR_BUSY;}
    r->busy=1;p->stack_busy=1;pthread_mutex_unlock(&pack_mu);
    /* Each ACK belongs to a different file stream. Preserve ordered retirement
     * within every queue; upload authority advances only after all succeed. */
    int rc=pack_parallel(p,r->count,pack_retire_one,r);
    if(!rc)rc=pack_command_wire(r->group,&r->object,r->refs,r->count,
        r->wire,sizeof(r->wire),EFS_MD_CMD_PACK_UPLOAD_RETIRE);
    pthread_mutex_lock(&pack_mu);
    r->last_error=rc;r->busy=0;p->stack_busy=0;if(!rc)r->active=0;
    pthread_mutex_unlock(&pack_mu);return rc;
}
static int pack_capabilities(uint32_t *members)
{
    *members=0;
    if(g_client.node_count<3 || g_client.node_count>EFS_MAX_NODES)return EFS_ERR_BUSY;
    uint32_t roster=0;
    for(uint32_t i=0;i<g_client.node_count;i++) {
        efs_node_id_t id=g_client.nodes[i].id;
        if(!id || id>EFS_MAX_NODES || (roster&(1u<<(id-1))))return EFS_ERR_PROTO;
        roster|=1u<<(id-1);
    }
    uint64_t now=pack_now();
    pthread_mutex_lock(&pack_mu);
    int cached=pack_capability_members==roster && now>=pack_capability_checked &&
        now-pack_capability_checked<PACK_CAPABILITY_NS;
    pthread_mutex_unlock(&pack_mu);
    if(cached){*members=roster;return EFS_OK;}
    /* This cache is only a compatibility preflight. Every actual PUT still
     * obtains fresh exact admission/FileID authority and a durable fence. */
    for(uint32_t i=0;i<g_client.node_count;i++) {
        efs_node_id_t id=g_client.nodes[i].id;
        if(!id || id>EFS_MAX_NODES)return EFS_ERR_PROTO;
        struct efs_conn *c=efs_client_conn_get(id);if(!c)return EFS_ERR_NET;
        uint8_t type=0;void *raw=NULL;uint32_t n=0;
        if(efs_conn_send_msg(c,EFS_MSG_PACK_CAPABILITIES,NULL,0) || efs_conn_recv_msg(c,&type,&raw,&n)) {
            free(raw);efs_client_conn_drop(id,c);return EFS_ERR_NET;
        }
        efs_client_conn_release(id,c);
        int ok=type==EFS_MSG_PACK_CAPABILITIES_REPLY && n==4 && efs_put_upload_rd32(raw)==1;
        free(raw);if(!ok)return EFS_ERR_PROTO;
        *members|=1u<<(id-1);
    }
    pthread_mutex_lock(&pack_mu);
    pack_capability_members=*members;pack_capability_checked=pack_now();
    pthread_mutex_unlock(&pack_mu);
    return EFS_OK;
}
static int pack_publish_record(struct pack_context *p,uint32_t i)
{
    struct pack_writer *w=&p->writers[i];
    if(__atomic_load_n(&w->done,__ATOMIC_ACQUIRE))return EFS_OK;
    if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
    int rc;
    if(!w->put_bound) {
        struct efs_chunk_rec put={.ino=w->plan.ino,.chunk_generation=w->object,
            .publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT|EFS_CHUNK_REC_F_PACKED_BASE,.packed=p->cohort.refs[i]};
        memcpy(put.nodes,p->object.nodes,sizeof(put.nodes));memcpy(put.checksums,p->object.checksums,sizeof(put.checksums));
        rc=efs_dcache_finish_publication(w->plan.ino,w->plan.generation,0,w->object,w->plan.cache_sequence,EFS_OK,&put);
        if(rc)return rc;
        w->put_bound=1;
    }
    if(!w->published) {
        rc=efs_client_publication_writeback(w->acks,w->plan.ino,w->plan.generation,0,w->object,w->plan.cache_sequence,w->len);
        if(rc) {
            int verdict;
            if((rc==EFS_ERR_STALE || rc==EFS_ERR_INVAL) &&
                !efs_publication_ack_rejected(w->acks,w->plan.cache_sequence,&verdict) && verdict==rc) {
                /* The exact rejection released the old publication token,
                 * not its accepted bytes. Leave the dirty cache owned so the
                 * enclosing ordinary flush fetches/rebases a fresh view.
                 * Retain this ACK for exact retirement with its neighbors. */
                __atomic_store_n(&w->done,1,__ATOMIC_RELEASE);return EFS_OK;
            }
            return rc;
        }
        w->published=1;
    }
    if(!w->loaded) {
        rc=efs_client_pull_chunks_range(w->plan.ino,0,1);if(rc)return rc;
        efs_dcache_drop(w->plan.ino,0);w->loaded=1;
    }
    /* Close/fsync require consumed durable publication and a loaded mapping.
     * Retirement is separately retained; it never changes file visibility. */
    __atomic_store_n(&w->done,1,__ATOMIC_RELEASE);
    return EFS_OK;
}
static int pack_publish_one(void *context,uint32_t i)
{return pack_publish_record(context,i);}
static int pack_publish_cohort(struct pack_context *p)
{return pack_parallel(p,p->cohort.count,pack_publish_one,p);}
static int pack_progress(struct pack_context *p)
{
    int rc=EFS_OK;
    if(!p->built) {
        rc=efs_pack_cohort_seal(&p->cohort,&p->cohort.owner);if(rc)return rc;
        rc=efs_pack_ledger_init(p->cohort.image,p->cohort.geometry,p->cohort.refs,
            p->cohort.count,&p->cohort.owner,p->members,&p->object.ledger);if(rc)return rc;
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)frags[i]=p->fragments[i];
        rc=efs_encode_segment(p->cohort.image,p->cohort.geometry,p->cohort.geometry,frags);if(rc)return rc;
        efs_place_fragments(g_client.nodes,g_client.node_count,p->object.ledger.segment_id,0,p->object.nodes);
        for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)efs_hash(frags[i],p->cohort.geometry/2,p->object.checksums[i]);
        p->built=1;
    }
    if(p->put_mask!=7) {
        rc=pack_command(p,EFS_MD_CMD_PACK_ADMIT);if(rc)return rc;
        size_t manifest;
        rc=efs_pack_upload_encode(&p->object,p->cohort.refs,p->cohort.count,
            p->wire+EFS_PACKED_PUT_HEADER,sizeof(p->wire)-EFS_PACKED_PUT_HEADER,&manifest);if(rc)return rc;
        efs_put_upload_wr32(p->wire,g_client.export_id);efs_put_upload_wr32(p->wire+8,(uint32_t)manifest);
        for(unsigned fi=0;fi<EFS_NUM_FRAGMENTS;fi++) {
            if(p->put_mask&(1u<<fi))continue;
            if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
            efs_put_upload_wr32(p->wire+4,fi);efs_node_id_t node=p->object.nodes[fi];
            struct efs_conn *c=efs_client_conn_get(node);if(!c)return EFS_ERR_NET;
            uint8_t type=0;void *raw=NULL;uint32_t n=0;
            if(efs_conn_send_msg_parts(c,EFS_MSG_PUT_PACKED,p->wire,EFS_PACKED_PUT_HEADER+(uint32_t)manifest,
                 p->fragments[fi],(uint32_t)p->cohort.geometry/2) || efs_conn_recv_msg(c,&type,&raw,&n)) {
                free(raw);efs_client_conn_drop(node,c);return EFS_ERR_NET;
            }
            efs_client_conn_release(node,c);
            rc=type==EFS_MSG_PUT_PACKED_REPLY && n==4?(int32_t)efs_put_upload_rd32(raw):EFS_ERR_PROTO;
            free(raw);if(rc)return rc;
            p->put_mask|=1u<<fi;
        }
    }
    rc=pack_publish_cohort(p);if(rc)return rc;
    return EFS_OK;
}
static int pack_flush_group(unsigned gi,int force)
{
    pthread_mutex_lock(&pack_mu);
    struct pack_context *p=pack_groups[gi];
    if(!p || !p->cohort.count){pthread_mutex_unlock(&pack_mu);return EFS_OK;}
    if(p->busy || p->stack_busy){pthread_mutex_unlock(&pack_mu);return EFS_ERR_BUSY;}
    uint64_t now=pack_now();
    if(!force && (now<p->retry_after || !efs_pack_cohort_due(&p->cohort,now,0))) {
        pthread_mutex_unlock(&pack_mu);return EFS_OK;
    }
    p->busy=1;pthread_mutex_unlock(&pack_mu);
    int rc=pack_progress(p);
    pthread_mutex_lock(&pack_mu);p->last_error=rc;p->busy=0;p->retry_after=pack_now()+10000000;
    if(!rc) {
        struct pack_retirement *r=pack_retirements[gi];
        if(r->active)rc=EFS_ERR_BUSY;
        else {
            r->object=p->object;r->group=p->cohort.group;r->count=p->cohort.count;
            memcpy(r->refs,p->cohort.refs,r->count*sizeof(r->refs[0]));
            for(uint32_t i=0;i<r->count;i++)r->acks[i]=p->writers[i].acks;
            r->active=1;r->last_error=EFS_OK;
        }
    }
    if(!rc) {
        uint32_t group=p->cohort.group;
        memset(p,0,sizeof(*p));(void)efs_pack_cohort_init(&p->cohort,group,PACK_WRITERS,PACK_DELAY_NS);
    }
    pthread_mutex_unlock(&pack_mu);return rc;
}
static void *pack_worker(void *unused)
{
    (void)unused;
    while(__atomic_load_n(&pack_running,__ATOMIC_ACQUIRE)) {
        for(unsigned gi=0;gi<2;gi++) {
            efs_client_rpc_set_deadline_ms(pack_now()/1000000+8000);
            (void)pack_retire_group(gi);(void)pack_flush_group(gi,0);efs_client_rpc_set_deadline_ms(0);
        }
        usleep(1000);
    }
    return NULL;
}
static int pack_start(void)
{
    pthread_mutex_lock(&pack_mu);
    if(!pack_started) {
        __atomic_store_n(&pack_running,1,__ATOMIC_RELEASE);
        if(pthread_create(&pack_tid,NULL,pack_worker,NULL)) {
            __atomic_store_n(&pack_running,0,__ATOMIC_RELEASE);pthread_mutex_unlock(&pack_mu);return EFS_ERR_NOMEM;
        }
        pack_started=1;
    }
    pthread_mutex_unlock(&pack_mu);return EFS_OK;
}
/* Caller serializes this FileID against ordinary writes, flush and truncate.
 * NOT_FOUND means no byte was accepted and the ordinary path may proceed. */
int efs_client_pack_write(efs_ino_t ino,uint64_t off,uint32_t len,const uint8_t *data)
{
    if(!efs_client_pack_enabled() || off || !len || len>EFS_PACK_MAX_PAYLOAD ||
        (g_client.export.chunk_size ? g_client.export.chunk_size : EFS_DEFAULT_CHUNK_SIZE)!=EFS_MIN_CHUNK_SIZE)return EFS_ERR_NOT_FOUND;
    struct efs_inode inode;
    pthread_mutex_lock(&g_client.idx_mu);int found=efs_export_get_inode(&g_client.export,ino,&inode);
    pthread_mutex_unlock(&g_client.idx_mu);
    if(found || inode.size || !S_ISREG(inode.mode) || efs_dcache_ino_pinned(ino))return EFS_ERR_NOT_FOUND;
    int rc=pack_start();if(rc)return rc;
    uint32_t members;rc=pack_capabilities(&members);if(rc)return rc;
    /* GETCHUNKS already reads current inode authority and returns its exact
     * FileID. Use that same fresh observation to prove an absent base instead
     * of making a separate generation-discovery WRITER_VIEW round trip. */
    struct efs_chunk_rec base_rec;uint32_t count=1;uint64_t gen=0;
    rc=efs_client_rpc_getchunks_fileid(g_client.export_id,ino,0,0,&base_rec,&count,&gen);if(rc)return rc;
    if(!gen)return EFS_ERR_PROTO;
    if(count)return EFS_ERR_NOT_FOUND;
    struct efs_msg_lane_writer_view_reply view;
    rc=efs_client_rpc_lane_writer_view(ino,gen,0,EFS_MIN_CHUNK_SIZE,&view);
    if(rc==EFS_ERR_NOT_FOUND)rc=efs_client_rpc_lane_bootstrap(g_client.export_id,ino,gen,0,EFS_MIN_CHUNK_SIZE,&view);
    if(rc)return rc;
    /* A size-zero inode after truncate is not a fresh lane. Its fence and
     * retained cache identity require the ordinary reconstruction path. */
    if(view.view.authority_epoch || view.view.history.count)return EFS_ERR_NOT_FOUND;
    struct efs_opid identity;rc=efs_client_publication_id(1,&identity);if(rc)return rc;
    rc=efs_client_rpc_publication_session(efs_kv_lane_shard(ino,0),&identity);if(rc)return rc;
    unsigned gi=efs_raft_shard_group(efs_kv_lane_shard(ino,0))==0?0:1;
    for(;;) {
        pthread_mutex_lock(&pack_mu);struct pack_context *p=pack_groups[gi];
        if(!p) {
            p=efs_buf_metadata_alloc(sizeof(*p));
            if(!p){pthread_mutex_unlock(&pack_mu);return EFS_ERR_NOMEM;}
            struct pack_retirement *r=efs_buf_metadata_alloc(sizeof(*r));
            if(!r){efs_buf_metadata_free(p,sizeof(*p));pthread_mutex_unlock(&pack_mu);return EFS_ERR_NOMEM;}
            memset(r,0,sizeof(*r));pack_retirements[gi]=r;
            memset(p,0,sizeof(*p));efs_pack_cohort_init(&p->cohort,gi?2:0,PACK_WRITERS,PACK_DELAY_NS);pack_groups[gi]=p;
        }
        if(p->busy || p->cohort.sealed || p->cohort.count==PACK_WRITERS || pack_ack_credits==2*PACK_WRITERS) {
            pthread_mutex_unlock(&pack_mu);rc=pack_flush_group(gi,1);
            if(rc!=EFS_OK && rc!=EFS_ERR_BUSY)return rc;
            rc=pack_retire_group(gi);
            if(rc!=EFS_OK && rc!=EFS_ERR_BUSY)return rc;
            if(pack_pause())return EFS_ERR_BUSY;
            continue;
        }
        if(!p->cohort.count) {
            p->members=members;rc=efs_client_put_id(&p->cohort.owner);
            if(rc){pthread_mutex_unlock(&pack_mu);return rc;}
            p->busy=1;
            pthread_mutex_unlock(&pack_mu);
            rc=efs_client_rpc_publication_session(gi?2:1,&p->cohort.owner);
            pthread_mutex_lock(&pack_mu);
            p->busy=0;
            if(rc){pthread_mutex_unlock(&pack_mu);return rc;}
        }
        struct pack_writer *w=&p->writers[p->cohort.count];
        /* Cache admission accounts presence under the directory/index locks.
         * Reserve this context, then release the scheduler lock before any
         * cache operation, including private-candidate failure cleanup. */
        p->busy=1;pack_ack_credits++;
        pthread_mutex_unlock(&pack_mu);
        rc=efs_client_publication_admit_write(&w->acks,ino,gen,0,&view,0,data,len);
        struct efs_msg_inode_getchunks_reply *base=NULL;
        if(!rc) {
            base=efs_buf_metadata_alloc(sizeof(*base));if(!base)rc=EFS_ERR_NOMEM;
            else {memset(base,0,sizeof(*base));base->ino=ino;base->generation=gen;
                rc=efs_dcache_snapshot_publication(ino,gen,0,&view,base,&w->plan,p->snapshot,EFS_MIN_CHUNK_SIZE);}
        }
        if(base)efs_buf_metadata_free(base,sizeof(*base));
        if(!rc) {
            w->object=efs_meta_candidate_gen(identity.client_uuid,identity.session_epoch,w->plan.cache_sequence,0,0);w->len=len;
            rc=efs_dcache_begin_publication(ino,gen,0,&w->plan,w->object);
            if(!rc) {
                struct efs_pack_record record={ino,gen,w->plan.publish_epoch,w->object,len,p->snapshot};
                pthread_mutex_lock(&pack_mu);
                rc=efs_pack_cohort_offer(&p->cohort,&record,w->plan.cache_sequence,pack_now());
                pthread_mutex_unlock(&pack_mu);
            }
        }
        if(rc) {
            /* Pre-accept failure: no PUT/admission send and no application
             * success. The private candidate alone may be abandoned. */
            if(w->acks)(void)efs_dcache_abort_pack_admission(ino,gen);
            if(w->acks)(void)efs_client_publication_ack_free(w->acks);
            memset(w,0,sizeof(*w));
            pthread_mutex_lock(&pack_mu);p->busy=0;pack_ack_credits--;pthread_mutex_unlock(&pack_mu);return rc;
        }
        /* The accepted snapshot stays reserved until its size is visible. */
        efs_dcache_note_packed_size(ino,len);
        pthread_mutex_lock(&pack_mu);p->busy=0;pthread_mutex_unlock(&pack_mu);
        return EFS_OK;
    }
}
static int pack_flush_run(efs_ino_t ino)
{
    for(;;) {
        int pending=0;unsigned gi=0;
        pthread_mutex_lock(&pack_mu);
        for(unsigned g=0;g<2 && !pending;g++) {
            struct pack_context *p=pack_groups[g];if(!p)continue;
            for(uint32_t i=0;i<p->cohort.count;i++)if(p->cohort.records[i].ino==ino && !__atomic_load_n(&p->writers[i].done,__ATOMIC_ACQUIRE)){pending=1;gi=g;break;}
        }
        pthread_mutex_unlock(&pack_mu);if(!pending)return EFS_OK;
        int rc=pack_flush_group(gi,0);
        if(rc!=EFS_OK && rc!=EFS_ERR_BUSY)return rc;
        if(pack_pause())return EFS_ERR_BUSY;
    }
}
int efs_client_pack_flush(efs_ino_t ino)
{
    if(!efs_client_pack_enabled())return EFS_OK;
    uint64_t previous=efs_client_rpc_deadline_ms();
    if(!previous)efs_client_rpc_set_deadline_ms(pack_now()/1000000+8000);
    int rc=pack_flush_run(ino);efs_client_rpc_set_deadline_ms(previous);return rc;
}
static int pack_drain_run(void)
{
    for(unsigned gi=0;gi<2;gi++) {
        int rc=pack_retire_group(gi);if(rc)return rc;
        rc=pack_flush_group(gi,1);if(rc)return rc;
        rc=pack_retire_group(gi);if(rc)return rc;
    }
    return EFS_OK;
}
int efs_client_pack_drain(void)
{
    if(!efs_client_pack_enabled())return EFS_OK;
    uint64_t previous=efs_client_rpc_deadline_ms();
    if(!previous)efs_client_rpc_set_deadline_ms(pack_now()/1000000+8000);
    int rc=pack_drain_run();
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}
void efs_client_pack_stop(void)
{
    pthread_mutex_lock(&pack_mu);int started=pack_started;
    __atomic_store_n(&pack_running,0,__ATOMIC_RELEASE);pthread_mutex_unlock(&pack_mu);
    if(started)pthread_join(pack_tid,NULL);
    for(unsigned gi=0;gi<2;gi++)if(pack_groups[gi] && !pack_groups[gi]->cohort.count) {
        efs_buf_metadata_free(pack_groups[gi],sizeof(*pack_groups[gi]));pack_groups[gi]=NULL;
    }
    for(unsigned gi=0;gi<2;gi++)if(pack_retirements[gi] && !pack_retirements[gi]->active) {
        efs_buf_metadata_free(pack_retirements[gi],sizeof(*pack_retirements[gi]));pack_retirements[gi]=NULL;
    }
    pack_started=0;
}
