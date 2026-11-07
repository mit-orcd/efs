/* efs-fuse: open references, lease acquire and retry, open and read. */
#include "fuse_internal.h"
#include "open.h"
#include "support.h"
#include "writeback.h"
#include "namespace.h"
#include "release.h"
#include "../commands/source.h"

/* Per-ino open-description count. The sequenced server lease is an edge
 * trigger: acquired at this client's first open of the ino, released at the
 * last close (one lease per (session, inode)). The kernel never relays a
 * flock UNLOCK on close, so the last-close lease edge is also what drops
 * the inode's server-side locks. */
pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;
static struct efs_open_ref *g_open_refs;

/* At most one row per inode. Rows retained for an unconfirmed last close
 * stay until the CLOSE edge is proven, so the table can hold far more rows
 * than open files; every per-inode lookup (getattr, open, release and the
 * cache evictor's per-inode pin test) goes through the hash, never a walk
 * of the table under g_open_mu (Oct 16 xorinox: 20,308 rows, ~80% of
 * efs-fuse CPU in list walks). All helpers run with g_open_mu held. */
#define OPEN_REF_BUCKETS 4096u
#define OPEN_DRAIN_PER_PASS 4096u
static struct efs_open_ref *g_open_hash[OPEN_REF_BUCKETS];

static uint32_t open_ref_bucket(efs_ino_t ino)
{
    return (uint32_t)(((uint64_t)ino * 0x9E3779B97F4A7C15ULL) >> 52) &
           (OPEN_REF_BUCKETS - 1);
}

struct efs_open_ref *open_ref_find(efs_ino_t ino)
{
    for (struct efs_open_ref *r = g_open_hash[open_ref_bucket(ino)]; r; r = r->hnext)
        if (r->ino == ino)
            return r;
    return NULL;
}

static void open_ref_link(struct efs_open_ref *r)
{
    uint32_t b = open_ref_bucket(r->ino);
    r->prev = NULL;
    r->next = g_open_refs;
    if (g_open_refs)
        g_open_refs->prev = r;
    g_open_refs = r;
    r->hnext = g_open_hash[b];
    g_open_hash[b] = r;
}

static void open_ref_unlink(struct efs_open_ref *r)
{
    struct efs_open_ref **pp = &g_open_hash[open_ref_bucket(r->ino)];
    if (r->prev)
        r->prev->next = r->next;
    else if (g_open_refs == r)
        g_open_refs = r->next;
    if (r->next)
        r->next->prev = r->prev;
    while (*pp && *pp != r)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = r->hnext;
    r->next = r->prev = r->hnext = NULL;
}

#define OPEN_EDGE_SHARDS 64
static pthread_mutex_t g_open_edge_mu[OPEN_EDGE_SHARDS];
static pthread_once_t g_open_edge_once = PTHREAD_ONCE_INIT;
static void open_edge_init(void)
{
    for (unsigned i = 0; i < OPEN_EDGE_SHARDS; ++i)
        pthread_mutex_init(&g_open_edge_mu[i], NULL);
}
pthread_mutex_t *open_edge_mu(efs_ino_t ino)
{
    pthread_once(&g_open_edge_once, open_edge_init);
    return &g_open_edge_mu[efs_ino_lock_slot(ino, OPEN_EDGE_SHARDS)];
}

/* Returns 1 when this is the first open (caller must acquire the HOLD). */
static int efs_open_note(efs_ino_t ino)
{
    struct efs_open_ref *r;
    int first = EFS_ERR_NOMEM;

    pthread_mutex_lock(&g_open_mu);
    r = open_ref_find(ino);
    if (!r) {
        r = calloc(1, sizeof(*r));
        if (r) {
            r->ino = ino;
            open_ref_link(r);
        }
    }
    if (r) {
        first = (r->n == 0);
        r->n++;
    }
    pthread_mutex_unlock(&g_open_mu);
    return first;
}

/* Size of the regular file a read-only open is acquiring (0 otherwise). */
static __thread uint64_t t_open_read_size;

static void efs_open_set_leased(efs_ino_t ino)
{
    struct efs_open_ref *r;

    pthread_mutex_lock(&g_open_mu);
    if ((r = open_ref_find(ino)))
        r->leased = 1;
    pthread_mutex_unlock(&g_open_mu);
}

/* Returns 1 when this was the last close. *leased is 1 when that close
 * must release the HOLD taken on the first open. */
static uint64_t efs_open_retry_order(const struct efs_open_ref *r)
{
    uint64_t order = UINT64_MAX;
    if (r->edge.active && !r->edge.complete && r->edge.q.id.seq)
        order = r->edge.q.id.seq;
    if (r->adoption.active && !r->adoption.complete &&
        r->adoption.q.id.seq && r->adoption.q.id.seq < order)
        order = r->adoption.q.id.seq;
    return order;
}

int efs_close_note(efs_ino_t ino, int *leased)
{
    struct efs_open_ref *r;
    int last = 0;

    if (leased)
        *leased = 0;
    pthread_mutex_lock(&g_open_mu);
    if ((r = open_ref_find(ino))) {
        if (r->n > 0)
            r->n--;
        if (r->n == 0) {
            if (leased)
                *leased = r->leased;
            if (r->leased) {
                r->close_pending = 1;
                r->retry_order = efs_open_retry_order(r);
                r->retry_turn = 0;
            } else {
                open_ref_unlink(r);
                free(r);
            }
            last = 1;
        }
    }
    pthread_mutex_unlock(&g_open_mu);
    return last;
}

/* Caller holds the inode edge stripe. A failed reply retains the old
 * ownership, preventing a fresh OPEN from racing its delayed release. */
static int efs_open_release_pending_run(efs_ino_t ino)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_trace_s = -1;
    int env_opid_trace = __atomic_load_n(&env_opid_trace_s, __ATOMIC_ACQUIRE);
    if (env_opid_trace < 0) {
        env_opid_trace = getenv("EFS_OPID_TRACE") != NULL;
        __atomic_store_n(&env_opid_trace_s, env_opid_trace, __ATOMIC_RELEASE);
    }
    struct efs_open_ref *owner = NULL;
    pthread_mutex_lock(&g_open_mu);
    owner = open_ref_find(ino);
    pthread_mutex_unlock(&g_open_mu);
    if (!owner || !owner->close_pending) return EFS_OK;
    /* The stripe keeps this row stable. Resolve a possibly committed OPEN
     * before creating its compensating CLOSE; never replace unknown identity. */
    if (owner->adoption.active) {
        int adoption_rc=efs_client_lease_resume(&owner->adoption,0);
        if (!owner->adoption.complete)
            return adoption_rc==EFS_OK?EFS_ERR_BUSY:adoption_rc;
        if (adoption_rc!=EFS_OK && !owner->adoption.revoked) {
            if (owner->adoption.flags!=(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING)) return adoption_rc;
            /* A later probe may prove the combined OPEN was refused. It
             * acquired no normal ownership, but still owns CREATE's pending
             * token. Allocate pending-only cleanup before replacing that
             * completed negative identity; admission failure keeps its body. */
            owner->leased=0;
            struct efs_client_lease_request clear={0};
            int rc=efs_client_lease_begin(&clear,ino,owner->lease_generation,
                EFS_LEASE_EDGE_CLEAR_PENDING,owner->adoption.pending_token);
            if (rc!=EFS_OK) return rc;
            if (env_opid_trace)
                fprintf(stderr,"opid-create-adopt: rejected seq=%llu ino=%llu rc=%d terminal=1\n",
                    (unsigned long long)owner->adoption.q.id.seq,(unsigned long long)ino,adoption_rc);
            (void)efs_client_lease_reset(&owner->adoption);
            owner->adoption=clear;
            adoption_rc=efs_client_lease_resume(&owner->adoption,0);
            if ((adoption_rc!=EFS_OK && !owner->adoption.revoked) || !owner->adoption.complete)
                return adoption_rc==EFS_OK?EFS_ERR_BUSY:adoption_rc;
        }
        (void)efs_client_lease_reset(&owner->adoption);
    }
    struct efs_client_lease_request *request = &owner->edge;
    int rc;
    if (request->flags & EFS_LEASE_EDGE_OPEN) {
        rc = efs_client_lease_resume(request, 0);
        if (!request->complete) return rc == EFS_OK ? EFS_ERR_BUSY : rc;
        if (rc == EFS_OK) {
            (void)efs_client_lease_reset(request);
        } else {
            /* A stored negative OPEN acquired nothing; completed global
             * revocation proves any old ownership is no longer active. */
            (void)efs_client_lease_reset(request);
            owner->leased = 0;
        }
    }
    if (!request->active && owner->leased && owner->lease_generation) {
        rc = efs_client_lease_begin(request, ino, owner->lease_generation,
                                     EFS_LEASE_EDGE_CLOSE, 0);
        if (rc != EFS_OK) return rc;
    }
    if (request->active) {
        rc = efs_client_lease_resume(request, 0);
        if ((rc != EFS_OK && !request->revoked) || !request->complete)
            return rc == EFS_OK ? EFS_ERR_BUSY : rc;
        (void)efs_client_lease_reset(request);
    }
    pthread_mutex_lock(&g_open_mu);
    struct efs_open_ref *done = open_ref_find(ino);
    if (done && !done->n && done->close_pending) {
        open_ref_unlink(done);
        free(done);
    }
    pthread_mutex_unlock(&g_open_mu);
    return EFS_OK;
}

/* The caller's inode stripe protects request bodies; the table mutex
 * publishes only their scheduling key. A new CLOSE may acquire an identity
 * after close_note. Publish it before returning an ambiguous reply, so a
 * newer unallocated CLOSE cannot starve the identity holding admission. */
int efs_open_release_pending(efs_ino_t ino)
{
    int rc = efs_open_release_pending_run(ino);
    pthread_mutex_lock(&g_open_mu);
    struct efs_open_ref *r = open_ref_find(ino);
    if (r && r->close_pending)
        r->retry_order = efs_open_retry_order(r);
    pthread_mutex_unlock(&g_open_mu);
    return rc;
}

/* A retained last close whose CLOSE edge could not take an identity
 * position (none allocated yet). Caller holds the inode stripe. */
int efs_open_close_unadmitted(efs_ino_t ino)
{
    pthread_mutex_lock(&g_open_mu);
    struct efs_open_ref *r = open_ref_find(ino);
    int waiting = r && !r->n && r->close_pending && r->leased && !r->edge.active;
    pthread_mutex_unlock(&g_open_mu);
    return waiting;
}

int wb_lock_budget(pthread_mutex_t *mu)
{
    if (!efs_client_rpc_deadline_ms())
        return pthread_mutex_lock(mu) == 0 ? EFS_OK : EFS_ERR_IO;
    for (;;) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        int rc = pthread_mutex_trylock(mu);
        if (!rc)
            return EFS_OK;
        if (rc != EBUSY)
            return EFS_ERR_IO;
        usleep(1000);
    }
}

/* One retained release per iteration, with no RPC under the table mutex. */
static int efs_open_drain_pending(void)
{
    static uint64_t counter; /* protected by g_open_mu */
    efs_ino_t ino = 0;
    uint64_t oldest = UINT64_MAX, turn = UINT64_MAX;
    struct efs_open_ref *selected = NULL;
    pthread_mutex_lock(&g_open_mu);
    for (struct efs_open_ref *r = g_open_refs; r; r = r->next)
        if (r->close_pending && (!ino || (r->retry_order != UINT64_MAX && oldest == UINT64_MAX) ||
            ((r->retry_order == UINT64_MAX) == (oldest == UINT64_MAX) &&
            (r->retry_turn < turn ||
            (r->retry_turn == turn && r->retry_order < oldest))))) {
            ino = r->ino;
            oldest = r->retry_order;
            turn = r->retry_turn;
            selected = r;
        }
    if (selected) {
        if (counter == UINT64_MAX) {
            for (struct efs_open_ref *r = g_open_refs; r; r = r->next)
                r->retry_turn = 0;
            counter = 0;
        }
        /* Give every retained identity a turn even when an older CLOSE has
         * an unresolved or negative verdict. Never discard that old owner. */
        selected->retry_turn = ++counter;
    }
    pthread_mutex_unlock(&g_open_mu);
    if (!ino) return EFS_OK;
    pthread_mutex_t *edge = open_edge_mu(ino);
    int rc = wb_lock_budget(edge);
    if (rc != EFS_OK)
        return rc;
    rc = efs_open_release_pending(ino);
    pthread_mutex_unlock(edge);
    if (rc == EFS_ERR_BUSY) {
        /* Selection prioritizes every already-identified release. If this
         * row still has no identity, another queue/foreground owner holds
         * the receipt window. Do not scan it 4096 more times under opid_mu;
         * return the pass to CREATE proof, outside the inode stripe. */
        int unadmitted = 0;
        pthread_mutex_lock(&g_open_mu);
        for (struct efs_open_ref *r = g_open_refs; r; r = r->next)
            if (r->ino == ino) {
                unadmitted = r->close_pending && r->retry_order == UINT64_MAX;
                break;
            }
        pthread_mutex_unlock(&g_open_mu);
        if (unadmitted)
            return EFS_ERR_AGAIN;
    }
    return rc == EFS_OK ? EFS_ERR_BUSY : rc;
}

/* Every returned regular descriptor, including CREATE, owns a server lease.
 * The edge stripe prevents a second open returning before proven OPEN,
 * and prevents a delayed last-close release deleting a new lease. */

/* BUSY without durable result proof owns the same OPEN edge. Wait within the
 * caller's single budget rather than return a transient contention as EBUSY
 * or replace an ambiguous identity. A completed rejection remains audible. */
static int efs_open_lease_resume_wait(struct efs_client_lease_request *request)
{
    int rc=EFS_ERR_BUSY;
    while(!efs_client_rpc_past_deadline()) {
        rc=efs_client_lease_resume(request,0);
        if(request->complete || rc!=EFS_ERR_BUSY)break;
        usleep(1000);
    }
    if(!request->complete && efs_client_rpc_past_deadline())rc=EFS_ERR_BUSY;
    return rc;
}

/* Every returned regular descriptor, including CREATE, owns a server lease.
 * The edge stripe prevents a second open returning before proven OPEN,
 * and prevents a delayed last-close release deleting a new lease. */
static int efs_open_acquire_fileid_run(efs_ino_t ino, uint64_t create_generation,
                                      uint64_t pending_token, int *handed_off,
                                      int *admission_busy)
{
    *admission_busy=0;
    pthread_mutex_t *edge = open_edge_mu(ino);
    int lock_rc=wb_lock_budget(edge);
    if(lock_rc!=EFS_OK)return lock_rc;
    int rc = efs_open_release_pending(ino);
    if (rc != EFS_OK) {
        /* Pending release may have deferred local receipt admission. Retry
         * outside the stripe so its oldest owner can make progress too. */
        *admission_busy = rc == EFS_ERR_BUSY;
        pthread_mutex_unlock(edge);
        return rc;
    }
    int first = efs_open_note(ino);
    rc = first < 0 ? first : EFS_OK;
    if (first > 0) {
        uint64_t generation = create_generation;
        uint32_t count = 1;
        /* A completed retained CREATE supplies authoritative FileID. Other
         * opens fetch it; cached attrs must not invent a generation. The
         * guarded OPEN validates that FileID across truncate/recovery. */
        if (!generation) {
            /* The FileID reply carries chunk 0's record: keep it as the
             * read's mapping. Asking for a 64-chunk window here moved
             * sequential readers' refresh-ahead into every open (98 KB
             * replies, 64 chunk rows read under the metadata leader's KV
             * lock per open; Oct 16 Intel profile). */
            struct efs_chunk_rec *recs = malloc(sizeof(*recs));
            uint64_t pulled = efs_client_map_clock();
            count = 1;
            rc = efs_client_rpc_getchunks_fileid(g_client.export_id, ino, 0, 0,
                                               recs, &count, &generation);
            if (rc == EFS_OK && recs && count)
                efs_client_map_install(ino, recs, count, pulled);
            free(recs);
            if (rc == EFS_OK && t_open_read_size)
                efs_client_prefetch_open(ino, t_open_read_size);
        }
        if (rc == EFS_OK && !generation) rc = EFS_ERR_PROTO;
        struct efs_open_ref *owner = NULL;
        pthread_mutex_lock(&g_open_mu);
        owner = open_ref_find(ino);
        pthread_mutex_unlock(&g_open_mu);
        if (rc == EFS_OK) {
            /* First CREATE adoption can establish normal ownership and clear
             * its pending token in one existing atomic lease edge. Keep that
             * exact edge in adoption so failure cleanup resolves it before
             * CLOSE. No other opener can pass the stripe before its verdict. */
            struct efs_client_lease_request *request = pending_token ?
                &owner->adoption : &owner->edge;
            unsigned flags = EFS_LEASE_EDGE_OPEN |
                (pending_token ? EFS_LEASE_EDGE_CLEAR_PENDING : 0);
            rc = efs_client_lease_begin(request, ino, generation, flags, pending_token);
            if(rc==EFS_ERR_BUSY && !request->active)*admission_busy=1;
            if (rc == EFS_OK) {
                if (pending_token) *handed_off = 1;
                owner->lease_generation = generation;
                efs_open_set_leased(ino);
                rc = efs_open_lease_resume_wait(request);
                if (rc == EFS_OK && !request->complete) rc = EFS_ERR_BUSY;
                if (rc == EFS_OK && pending_token)
                    (void)efs_client_lease_reset(request);
            }
        }
        if (rc != EFS_OK) {
            (void)efs_close_note(ino, NULL);
            (void)efs_open_release_pending(ino);
        }
    }
    pthread_mutex_unlock(edge);
    return rc;
}

/* Budget includes serialization, FileID lookup, the retained OPEN and any
 * compensating cleanup. Never extend an inherited deadline. */
static int efs_open_acquire_fileid_mode(efs_ino_t ino, uint64_t create_generation,
                                       uint64_t pending_token, int *handed_off)
{
    uint64_t previous=efs_client_rpc_deadline_ms();
    uint64_t deadline=stats_now_ms()+30000;
    if(previous && previous<deadline)deadline=previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc=EFS_ERR_BUSY;
    while(!efs_client_rpc_past_deadline()) {
        int admission_busy=0;
        rc=efs_open_acquire_fileid_run(ino,create_generation,pending_token,
                                     handed_off,&admission_busy);
        /* Failed local admission allocates no identity; a retained release
         * keeps its exact identity. Both must retry outside the stripe so
         * an older owner on a colliding inode can finish and free capacity. */
        if(rc!=EFS_ERR_BUSY || !admission_busy)break;
        efs_client_opid_wait(1);
    }
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

int efs_open_acquire_fileid(efs_ino_t ino, uint64_t create_generation)
{
    int handed_off = 0;
    return efs_open_acquire_fileid_mode(ino, create_generation, 0, &handed_off);
}

static int efs_open_acquire(efs_ino_t ino)
{
    return efs_open_acquire_fileid(ino, 0);
}

/* CLEAR_PENDING shares the inode stripe with OPEN/CLOSE. A descriptor is
 * returned only after exact adoption proof; rejection retains this request
 * for the last-close compensation path. */
static int efs_open_adopt(efs_ino_t ino,uint64_t generation,uint64_t token,
                          int *handed_off)
{
    *handed_off=0;
    pthread_mutex_t *stripe=open_edge_mu(ino);
    int rc=EFS_ERR_BUSY;
    while(!efs_client_rpc_past_deadline()) {
        int lock_rc=wb_lock_budget(stripe);
        if(lock_rc!=EFS_OK)return lock_rc;
        struct efs_open_ref *owner=NULL;
        pthread_mutex_lock(&g_open_mu);
        owner=open_ref_find(ino);
        pthread_mutex_unlock(&g_open_mu);
        rc=EFS_ERR_STALE;
        int terminal=0;
        if(owner && owner->n && owner->leased && owner->lease_generation==generation) {
            struct efs_client_lease_request *adoption=&owner->adoption;
            if(!adoption->active)
                rc=efs_client_lease_begin(adoption,ino,generation,
                                          EFS_LEASE_EDGE_CLEAR_PENDING,token);
            else if(adoption->ino==ino && adoption->generation==generation &&
                    adoption->pending_token==token &&
                    adoption->flags==EFS_LEASE_EDGE_CLEAR_PENDING)
                rc=EFS_OK;
            if(rc==EFS_OK) {
                *handed_off=1;
                rc=efs_client_lease_resume(adoption,0);
                terminal=adoption->complete;
                if(rc==EFS_OK && terminal)
                    (void)efs_client_lease_reset(adoption);
                else if(rc==EFS_OK)rc=EFS_ERR_BUSY;
            }
        }
        pthread_mutex_unlock(stripe);
        /* Retry only unallocated admission or this exact unknown adoption.
         * A known rejection is final. Waiting never holds the inode stripe. */
        if(rc!=EFS_ERR_BUSY || terminal)break;
        usleep(1000);
    }
    return rc;
}

int efs_open_acquire_created(efs_ino_t ino, uint64_t generation,
                                    uint64_t token, int *handed_off)
{
    *handed_off = 0;
    if (!generation || !token) return EFS_ERR_INVAL;
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline) deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_open_acquire_fileid_mode(ino, generation, token, handed_off);
    /* Another descriptor already owns the normal lease. Preserve it and
     * clear only this CREATE token using the existing retained adoption. */
    if (rc == EFS_OK && !*handed_off) {
        rc = efs_open_adopt(ino, generation, token, handed_off);
        if (rc != EFS_OK) {
            struct fuse_file_info fi = {0};
            (void)efs_fuse_release_ino(ino, &fi);
        }
    }
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

#define EFS_PENDING_CREATES_MAX 128u
static pthread_mutex_t g_pending_create_mu=PTHREAD_MUTEX_INITIALIZER;
static struct efs_pending_create *g_pending_creates;
static unsigned g_pending_create_count;
struct efs_pending_create *efs_pending_create_alloc(void)
{
    struct efs_pending_create *entry=calloc(1,sizeof(*entry));
    if(!entry)return NULL;
    pthread_mutex_lock(&g_pending_create_mu);
    if(g_pending_create_count==EFS_PENDING_CREATES_MAX){free(entry);entry=NULL;}
    else g_pending_create_count++;
    pthread_mutex_unlock(&g_pending_create_mu);return entry;
}
void efs_pending_create_free(struct efs_pending_create *entry)
{
    pthread_mutex_lock(&g_pending_create_mu);g_pending_create_count--;
    pthread_mutex_unlock(&g_pending_create_mu);free(entry);
}
void efs_pending_create_queue(struct efs_pending_create *entry)
{
    entry->next=NULL;
    pthread_mutex_lock(&g_pending_create_mu);
    struct efs_pending_create **tail=&g_pending_creates;
    while(*tail)tail=&(*tail)->next;
    *tail=entry;
    pthread_mutex_unlock(&g_pending_create_mu);
}
/* Resolve abandoned CREATE, then clear only its exact pending token. Never
 * unlink a name after an ambiguous result: other clients may have moved it. */
static int efs_pending_create_drain(void)
{
    pthread_mutex_lock(&g_pending_create_mu);
    struct efs_pending_create *entry=g_pending_creates;
    if(entry)g_pending_creates=entry->next;
    unsigned count=g_pending_create_count;
    pthread_mutex_unlock(&g_pending_create_mu);
    if(!entry)return count?EFS_ERR_BUSY:EFS_OK;
    struct efs_client_create_request *request=&entry->request;
    int rc=EFS_OK;
    if(!request->attempted && !request->complete) {
        (void)efs_client_create_cancel(request);
    } else {
        rc=efs_client_create_resume(request,0);
        if(!request->complete)goto pending;
        if(request->result==EFS_OK) {
            pthread_mutex_t *stripe=open_edge_mu(request->inode.ino);
            rc=wb_lock_budget(stripe);
            if(rc!=EFS_OK)goto pending;
            rc=efs_open_release_pending(request->inode.ino);
            if(rc==EFS_OK && !entry->cleanup.active)
                rc=efs_client_lease_begin(&entry->cleanup,request->inode.ino,
                    request->generation,EFS_LEASE_EDGE_CLEAR_PENDING,request->wire.q.id.seq);
            if(rc==EFS_OK)rc=efs_client_lease_resume(&entry->cleanup,0);
            pthread_mutex_unlock(stripe);
            if((rc!=EFS_OK && !entry->cleanup.revoked) || !entry->cleanup.complete)goto pending;
            (void)efs_client_lease_reset(&entry->cleanup);
        }
        (void)efs_client_create_reset(request);
    }
    efs_pending_create_free(entry);return EFS_ERR_BUSY;
pending:
    efs_pending_create_queue(entry);
    return rc==EFS_OK?EFS_ERR_BUSY:rc;
}

/* A lost last-close reply must not pin the shared ACK indefinitely until
 * unmount. Retry oldest ownership first, with bounded per-pass RPC time. */
static pthread_t g_open_retry_thread;
static int g_open_retry_running, g_open_retry_started;
/* Both queues share the bounded identity window. A lease admission wait
 * can consume the entire pass while an older abandoned CREATE owns the
 * slot that would unblock it. Reserve half the pass for CREATE proof;
 * neither queue may starve the other or extend the caller's deadline. */
int efs_open_retry_pass(uint64_t deadline)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    if (previous && previous < deadline)
        deadline = previous;
    uint64_t now = stats_now_ms();
    if (deadline <= now)
        return EFS_ERR_BUSY;
    uint64_t share = (deadline - now) / 2;
    if (share > 500)
        share = 500;
    uint64_t split = now + share;
    efs_client_rpc_set_deadline_ms(split);
    /* Drain as many retained releases as the share allows; one per pass
     * (10/s) could never keep up with a stream of failed last closes. */
    int lease_rc = EFS_OK;
    for (unsigned i = 0; i < OPEN_DRAIN_PER_PASS && !efs_client_rpc_past_deadline(); i++) {
        lease_rc = efs_open_drain_pending();
        if (lease_rc == EFS_ERR_AGAIN) {
            lease_rc = EFS_ERR_BUSY; /* retained, but admission is elsewhere */
            break;
        }
        if (lease_rc == EFS_OK)
            break; /* nothing retained */
    }
    efs_client_rpc_set_deadline_ms(deadline);
    int create_rc = efs_pending_create_drain();
    efs_client_rpc_set_deadline_ms(previous);
    return lease_rc != EFS_OK ? lease_rc : create_rc;
}

static void *efs_open_retry_worker(void *unused)
{
    (void)unused;
    while (__atomic_load_n(&g_open_retry_running,__ATOMIC_ACQUIRE)) {
        (void)efs_open_retry_pass(stats_now_ms()+1000);
        usleep(100000);
    }
    return NULL;
}
void efs_open_retry_start(void)
{
    __atomic_store_n(&g_open_retry_running,1,__ATOMIC_RELEASE);
    if(pthread_create(&g_open_retry_thread,NULL,efs_open_retry_worker,NULL)==0)
        g_open_retry_started=1;
    else {
        __atomic_store_n(&g_open_retry_running,0,__ATOMIC_RELEASE);
        fprintf(stderr,"efs-fuse: lease retry worker unavailable; releases require synchronous retry\n");
    }
}
void efs_open_retry_stop(void)
{
    __atomic_store_n(&g_open_retry_running,0,__ATOMIC_RELEASE);
    if(g_open_retry_started){pthread_join(g_open_retry_thread,NULL);g_open_retry_started=0;}
}

/* Client-cache design Part A, pin rule 2: a ghost (nlink=0) row is
 * evictable only once no fd has it open. */
int efs_client_ino_is_open(efs_ino_t ino)
{
    struct efs_open_ref *r;
    int open = 0;

    pthread_mutex_lock(&g_open_mu);
    r = open_ref_find(ino);
    open = r != NULL;
    pthread_mutex_unlock(&g_open_mu);
    return open;
}

/* Part D: kernel page cache is the coherence hole. FOPEN_DIRECT_IO on
 * every regular open (not just O_DIRECT). Application O_DIRECT still
 * requires 4 KiB alignment; FOPEN_DIRECT_IO does not — the kernel
 * sends ordinary unaligned FUSE reads/writes once cache is bypassed.
 * libfuse 3.10.2 fuse_reply_open cannot set FOPEN_PARALLEL_DIRECT_WRITES.
 * Requests ARE 1 MiB here (Oct 1 2026 strace of a 1 MiB dd: read(/dev/fuse)
 * = 1048656, reply writev = 1048592): libfuse 3.10.2 negotiates
 * FUSE_MAX_PAGES with max_write, so a 1 MiB write() is one FUSE request
 * and eight 128 KiB chunks per ll_write_buf. */
void fuse_fi_direct_io(struct fuse_file_info *fi)
{
    if (!fi)
        return;
    fi->direct_io = 1;
    fi->keep_cache = 0;
}

int fuse_odirect_unaligned(const struct fuse_file_info *fi,
                                  off_t offset, size_t size)
{
    return fi && (fi->flags & O_DIRECT) &&
           ((offset & 4095) || (size & 4095));
}

static int efs_fuse_open_ino_run(fuse_ino_t ino, struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    if (vk == 1 || vk == 2) {
        int access = virtual_access(virt_parent(ino));
        if (access) return access;
    }
    if (vk == 1) {
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        struct stats_ent snapshot;
        int rc = stats_snapshot(virt_parent(ino), &snapshot);
        if (rc) return rc;
        rc = virtual_file_open(fi, ino, snapshot.text, snapshot.len);
        if (rc) return rc;
        fuse_fi_direct_io(fi);
        return 0;
    }
    if (vk == 3) {
        efs_ino_t dir_ino;
        char term[EFS_MAX_NAME];
        char query_path[EFS_MAX_PATH];
        if (!commands_enabled())
            return -ENOENT;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EACCES;
        if (vq_lookup(ino, &dir_ino, term, sizeof(term)) != 0)
            return -ENOENT;
        if (command_script(term)) {
            int access=virtual_access(dir_ino);if (access) return access;
            int rc=virtual_file_open(fi,ino,efs_command_source,sizeof(efs_command_source)-1);
            if (!rc) fuse_fi_direct_io(fi);
            return rc;
        }
        snprintf(query_path,sizeof(query_path),"/%s/%s",EFS_FIND_NAME,term);
        int rc = find_query_ensure(dir_ino, query_path, term);
        if (rc) return rc;
        rc = -EAGAIN; /* Eviction between ensure and pin must not look empty. */
        pthread_mutex_lock(&g_find_res_mu);
        for (int i = 0; i < EFS_FIND_RES_N; i++) {
            if (g_find_res[i].valid && find_res_caller(i) && g_find_res[i].dir_ino == dir_ino &&
                strcmp(g_find_res[i].term, term) == 0) {
                rc = vq_version_matches(ino, g_find_res[i].text, g_find_res[i].len) ?
                    virtual_file_open(fi, ino, g_find_res[i].text, g_find_res[i].len) : -ESTALE;
                break;
            }
        }
        pthread_mutex_unlock(&g_find_res_mu);
        if (rc) return rc;
        fuse_fi_direct_io(fi);
        return 0;
    }
    if (vk == 2 || vk == 4)
        return -EISDIR;

    struct efs_inode row;
    int lrc = efs_client_stat_refresh((efs_ino_t)ino, &row);
    if (lrc != EFS_OK)
        return fuse_stat_errno(lrc);
    {
        const struct fuse_ctx *ctx = ll_ctx();
        int accmode = fi ? (fi->flags & O_ACCMODE) : O_RDONLY;
        int mask = 0;
        if (accmode != O_WRONLY)
            mask |= R_OK;
        if (accmode != O_RDONLY)
            mask |= W_OK;
        if (mask && ctx && check_access(&row, ctx->uid, ctx->gid, mask) != 0)
            return -EACCES;
    }
    if (fi) {
        fi->fh = row.ino;
        fuse_fi_direct_io(fi);
    }
    if (fi && (fi->flags & O_TRUNC) && (fi->flags & O_ACCMODE) != O_RDONLY) {
        if (efs_mode_is_dir(row.mode))
            return -EISDIR;
        int trc = efs_fuse_truncate_ino(row.ino, 0, fi);
        if (trc != 0)
            return trc;
    }
    if (fi && !efs_mode_is_dir(row.mode)) {
        int frc = efs_file_open(fi, row.ino);
        if (frc)
            return frc;
    }
    if (fi && !efs_mode_is_dir(row.mode)) {
        t_open_read_size = (fi->flags & O_ACCMODE) == O_RDONLY && S_ISREG(row.mode) ?
            row.size : 0;
        int hrc = efs_open_acquire(row.ino);
        t_open_read_size = 0;
        if (hrc != EFS_OK) {
            efs_file_close(fi);
            return fuse_stat_errno(hrc);
        }
    }
    return 0;
}

/* OPEN's staged row must survive until the open-reference pin takes over.
 * stat_ino drops its table locks before permission checks and OPEN lease
 * acquisition; without an operation pin the evictor can remove the exact
 * EOF in that gap, and a later buffered write has no row to update. */
int efs_fuse_open_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    if (virt_kind(ino))
        return efs_fuse_open_ino_run(ino, fi);
    efs_client_stage_pin((efs_ino_t)ino);
    int rc = efs_fuse_open_ino_run(ino, fi);
    efs_client_stage_unpin((efs_ino_t)ino);
    return rc;
}


int efs_fuse_read_ino(fuse_ino_t ino, char *buf, size_t size, off_t offset,
                             struct fuse_file_info *fi)
{
    int vk = virt_kind(ino);
    if ((vk == 1 || vk == 3) && fi) {
        /* Startup policy revokes new queries, not bytes already owned by an
         * authorized descriptor. No server/index work is needed for this read. */
        struct efs_file_handle *h = efs_file_handle(fi);
        if (!h || !h->snapshot) return -EBADF;
        if (offset < 0) return -EINVAL;
        if ((uint64_t)offset >= h->snapshot_len) return 0;
        size_t n = h->snapshot_len - (size_t)offset;
        if (n > size) n = size;
        memcpy(buf, h->snapshot + offset, n);
        return (int)n;
    }
    if (vk == 1) {
        struct stats_ent e;
        if (!feature_enabled(EFS_FEATURE_STATS))
            return -ENOENT;
        if (stats_snapshot(virt_parent(ino), &e) != 0)
            return -ENOENT;
        if (offset >= e.len)
            return 0;
        size_t avail = (size_t)e.len - (size_t)offset;
        if (avail > size)
            avail = size;
        memcpy(buf, e.text + offset, avail);
        return (int)avail;
    }
    if (vk == 3) {
        efs_ino_t dir_ino;
        char term[EFS_MAX_NAME];
        char query_path[EFS_MAX_PATH];
        if (!commands_enabled())
            return -ENOENT;
        if (vq_lookup(ino, &dir_ino, term, sizeof(term)) != 0)
            return -ENOENT;
        snprintf(query_path,sizeof(query_path),"/%s/%s",EFS_FIND_NAME,term);
        return find_query_read(dir_ino, query_path, term, buf, size, offset);
    }
    if (vk == 2 || vk == 4)
        return -EISDIR;

    efs_ino_t file = efs_file_ino(fi, (efs_ino_t)ino);
    if (fuse_odirect_unaligned(fi, offset, size))
        return -EINVAL;

    size_t got = 0;
    int rc = efs_client_read(file, (uint64_t)offset, size, buf, &got);
    if (rc != 0) {
        efs_fuse_log_err("read", rc, file, (uint64_t)offset, size, NULL);
        return efs_rc_to_errno(rc);
    }
    return (int)got;
}
