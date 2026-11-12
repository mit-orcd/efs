#include "read_proof.h"
#include "efs/wire.h"
#include "efs/network.h"
#include "efs/batch_obs.h"
#include "efs/telemetry.h"
#include <errno.h>
#include <string.h>
#include <time.h>

/* The life of a metadata ReadIndex request: see read_proof.h for the
 * contract. The bodies below moved from raft_host.c unchanged except for how
 * they reach the state (the owner's struct instead of the host's, the six
 * functions of read_proof_host.h instead of host statics). */

struct read_proof_prof g_read_proof_prof;

static uint64_t now_us_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void host_deadline_us(struct timespec *ts, long us)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += us / 1000000;
    ts->tv_nsec += (us % 1000000) * 1000;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int host_past_deadline(const struct timespec *end)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec != end->tv_sec)
        return now.tv_sec > end->tv_sec;
    return now.tv_nsec >= end->tv_nsec;
}

void read_proof_init(struct read_proof *rp, struct efs_raft_host *host,
                     struct group_views *gv, const int *raft_id,
                     const int *members)
{
    memset(rp, 0, sizeof(*rp));
    rp->rp_host = host;
    rp->rp_gv = gv;
    rp->rp_raft_id = raft_id;
    rp->rp_members = members;
}

void read_proof_attach(struct read_proof *rp, int gi, uint8_t group)
{
    rp->rp_slot[gi].rp_group = group;
}

void read_proof_set_telemetry(struct read_proof *rp, struct efs_telemetry *t)
{
    rp->rp_telemetry = t;
}

/* A follower's ReadIndex is one RPC to the leader per caller. The dual
 * hosts follow both groups and serve every GETCHUNKS (two ReadIndexes
 * per 64-chunk window); 9 clients × 16 pull threads put ~300 of those
 * RPCs in flight per group at once, and each waited a heartbeat round
 * at the leader (`getchunks slow … ri_ms=36..53`, Sep 30). Readers that
 * arrive while a round is in flight share the NEXT round (never the one
 * already sent: a commit between its send and this reader's arrival
 * would be missed), so any number of concurrent followers' reads cost
 * two leader RPCs per round trip. Errors are shared by that round's
 * waiters; each caller keeps its own deadline. */
struct host_ri_wait {
    struct host_ri_wait *next;
    uint64_t need, index;
    int done, rc, hint;
};
struct host_ri_coal {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int inflight;
    uint64_t started;
    struct host_ri_wait *waiters; /* Caller stacks, linked only under mu. */
};

/* Group ids are EFS_RAFT_GROUP_SHARD (0) and EFS_RAFT_GROUP_SHARD2 (2). */
static struct host_ri_coal g_ri_coal[READ_PROOF_NGROUPS] = {
    {.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER},
    {.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER},
};

static void host_ri_complete(struct host_ri_coal *c, uint64_t round, int rc,
                              uint64_t index, int hint)
{
    /* Freeze each caller's FIRST sufficient reply. A newer completion must
     * not replace it with an unrelated suffix while that caller reacquires
     * mu; doing so makes its subsequent own-apply wait chase future writes. */
    for (struct host_ri_wait *w=c->waiters; w; w=w->next) {
        if (!w->done && w->need<=round) {
            w->index=rc==EFS_OK ? index : 0;
            w->hint=rc==EFS_OK ? hint : -1;
            w->rc=rc;
            w->done=1;
        }
    }
}

static void host_ri_remove(struct host_ri_coal *c, struct host_ri_wait *w)
{
    struct host_ri_wait **p=&c->waiters;
    while (*p && *p!=w) p=&(*p)->next;
    if (*p) *p=w->next;
}

static int host_remote_read_index(struct read_proof *rp, uint8_t group,
                                  int leader, const struct timespec *end,
                                  uint64_t *idx, int *hint)
{
    struct host_ri_coal *c;
    int rc;

    if ((group >> 1) >= READ_PROOF_NGROUPS || (group & 1u)) {
        struct efs_msg_raft_mkfs_reply rep;
        rc = read_proof_host_remote(rp->rp_host, group, leader, &rep);
        if (rc == EFS_OK) { *idx=rep.index; *hint=rep.leader_hint; }
        return rc;
    }
    c=&g_ri_coal[group >> 1];
    pthread_mutex_lock(&c->mu);
    if (c->started==UINT64_MAX) {
        pthread_mutex_unlock(&c->mu);
        return EFS_ERR_BUSY;
    }
    struct host_ri_wait w={.need=c->started+1,.next=c->waiters};
    c->waiters=&w;
    for (;;) {
        if (w.done) {
            rc=w.rc; *idx=w.index; *hint=w.hint;
            host_ri_remove(c,&w);
            pthread_mutex_unlock(&c->mu);
            return rc;
        }
        if (!c->inflight) {
            struct efs_msg_raft_mkfs_reply rep;
            uint64_t round=++c->started;
            c->inflight=1;
            pthread_mutex_unlock(&c->mu);
            memset(&rep,0,sizeof(rep));
            rc=read_proof_host_remote(rp->rp_host,group,leader,&rep);
            pthread_mutex_lock(&c->mu);
            c->inflight=0;
            host_ri_complete(c,round,rc,rep.index,rep.leader_hint);
            pthread_cond_broadcast(&c->cv);
            continue;
        }
        if (pthread_cond_timedwait(&c->cv,&c->mu,end)==ETIMEDOUT && !w.done) {
            host_ri_remove(c,&w);
            pthread_mutex_unlock(&c->mu);
            return EFS_ERR_BUSY;
        }
    }
}

/* ReadIndex for one group, batched and lock-free on the hot path. The
 * raft core keeps ONE read round (read_begin resets its acks), so rounds
 * used to be serialized by a host-wide read_mu that every inode handler
 * then held across its whole KV read + propose — one metadata RPC at a
 * time per leader (33 threads queued on it under the 9-host posix suite,
 * lookup 53 ms avg). A reader records commit_index on arrival (`want`),
 * queues for the next round if one is pending, and is satisfied by any
 * finished round begun after admission, whose read_index >= want with
 * applied >= read_index. Previously completed rounds cannot cover new reads. It
 * reads all of that from the published view. The reader
 * never takes h->mu: it admits itself from the view and ri_begun, raises
 * ri_request, and the pump (or the read-ACK path, which already holds
 * h->mu) begins that round once the previous one has its quorum. Under the
 * orcd-mini tiny-file workload the per-reader h->mu admission/begin was
 * 9-11% of all efsd context switches (docs/status/orcd-mini-readindex-
 * lock-20261016.md). Waiting is on a per-reader slot in the wait bracket, never
 * on h->mu — the pump must keep its heartbeat cadence under 100+
 * concurrent handlers. Not the leader: forward (blocking peer RPC, no
 * host lock held). */
/* An authoritative remote read succeeded, but this follower cannot apply
 * its index in time. Route the caller to that leader instead of repeatedly
 * timing out the same stale local view. This is a hint, never read permission. */
static int host_follower_read_result(int rc,int self,int peers,int remote_hint,
                                     int *leader_hint)
{
    if(rc==EFS_ERR_BUSY && remote_hint>=0 && remote_hint<peers &&
       remote_hint!=self) {
        if(leader_hint)*leader_hint=remote_hint;
        return EFS_ERR_NOT_PRIMARY;
    }
    return rc;
}

struct host_read_admission {uint64_t want,round,term;};

/* h->mu held. Raise ri_begun to the round read_begin is about to start
 * before any of its probes can leave. A reader that loads N from ri_begun
 * and then requires round N+1 can only be covered by probes sent after its
 * load, i.e. after it arrived. Monotone: a failed begin only makes later
 * admissions require a later round. recv_persisting reports round 0 and
 * refuses the begin, so the max keeps the real value. */
static void host_read_note_begin(struct read_proof *rp,uint8_t group,
                                 struct efs_raft *r)
{
    struct read_proof_slot *g=read_proof_slot_of(rp,group);
    uint64_t next=efs_raft_read_round(r);
    if(!g || next>=UINT64_MAX-1)return;
    next++;
    if(next>__atomic_load_n(&g->ri_begun,__ATOMIC_RELAXED))
        __atomic_store_n(&g->ri_begun,next,__ATOMIC_SEQ_CST);
}
int read_proof_begin(struct read_proof *rp,uint8_t group,struct efs_raft *r)
{
    host_read_note_begin(rp,group,r);
    return efs_raft_read_begin(r);
}

/* Fresh-read admission without h->mu. The view supplies role, term and
 * the commit floor; ri_begun supplies the first round whose probes cannot
 * precede this arrival. The proof itself is still the core's quorum round
 * for that term, checked by host_read_index_admitted. */
static int host_read_admit_view(struct read_proof *rp,uint8_t group,
                                const struct host_view *v,
                                struct host_read_admission *a)
{
    struct read_proof_slot *g=read_proof_slot_of(rp,group);
    if(!g || !v->has || v->role!=EFS_RAFT_LEADER)return EFS_ERR_NOT_PRIMARY;
    uint64_t begun=__atomic_load_n(&g->ri_begun,__ATOMIC_SEQ_CST);
    /* Defense in depth: a published round is begun even if a future
     * caller started it without read_proof_begin. */
    if(v->read_round>begun)begun=v->read_round;
    if(begun>=UINT64_MAX-1)return EFS_ERR_BUSY;
    *a=(struct host_read_admission){v->commit,begun+1,v->term};
    return EFS_OK;
}

/* Ask for round `round` without h->mu: raise ri_request and wake the pump.
 * Many readers share one raise and one probe round. */
static void host_read_request(struct read_proof *rp,uint8_t group,
                              uint64_t round)
{
    struct read_proof_slot *g=read_proof_slot_of(rp,group);
    if(!g)return;
    uint64_t cur=__atomic_load_n(&g->ri_request,__ATOMIC_ACQUIRE);
    while(cur<round)
        if(__atomic_compare_exchange_n(&g->ri_request,&cur,round,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            read_proof_host_kick(rp->rp_host);return;
        }
}

/* h->mu held, cores[i] the core of slot i (NULL if not hosted): start the round readers asked for once the previous round
 * has its quorum (one round in flight per group, as before). Returns the
 * groups whose view needs publishing. */
unsigned read_proof_start_requested(struct read_proof *rp,
                                    struct efs_raft *const *cores)
{
    unsigned started=0;
    for(unsigned i=0;i<READ_PROOF_NGROUPS;i++){
        struct read_proof_slot *g=&rp->rp_slot[i];
        struct efs_raft *r=cores[i];
        uint64_t want=__atomic_load_n(&g->ri_request,__ATOMIC_ACQUIRE);
        if(!want || !r || efs_raft_role(r)!=EFS_RAFT_LEADER ||
           efs_raft_read_round(r)>=want ||
           (efs_raft_read_pending(r) && !efs_raft_read_quorum(r)))
            continue;
        if(read_proof_begin(rp,g->rp_group,r)==EFS_OK){
            host_publish_view(rp->rp_gv,(int)i,r);started|=1u<<i;
        }
    }
    return started;
}


static int host_read_index_admitted(struct read_proof *rp, uint8_t group,
                           int *leader_hint,const struct host_read_admission *admission,
                           uint64_t *proof_index, int proof_only)
{
    struct timespec end;
    struct host_view v;
    struct host_read_floor floor={0};
    uint64_t want, round = 0, admitted_term = 0;
    int prof = read_prof_on();
    uint64_t p0 = prof ? now_us_() : 0;
    int first = 1, requested = 0, gate_hint;
    if (proof_index)
        *proof_index = 0;
    if (leader_hint)
        *leader_hint = -1;
    /* W127/B4: never serve reads from a group stuck below a committed
     * entry it cannot apply. */
    if (read_proof_host_gate(rp->rp_host, group, &gate_hint)) {
        if (leader_hint) *leader_hint = gate_hint;
        return EFS_ERR_NOT_PRIMARY;
    }
    /* Admission and round starts never take h->mu. */
    host_deadline_us(&end, (long)HOST_READ_TRIES * HOST_TICK_US);
    host_view_get(rp->rp_gv, group, &v);
    if (!v.has)
        return EFS_ERR_NOT_PRIMARY;
    want = admission ? admission->want : v.commit;
    if (admission) {round=admission->round;admitted_term=admission->term;}
    for (;;) {
        int rc;

        if (read_proof_host_gate(rp->rp_host, group, &gate_hint)) {
            if (leader_hint) *leader_hint = gate_hint;
            return EFS_ERR_NOT_PRIMARY;
        }
        if (!v.has)
            return EFS_ERR_NOT_PRIMARY;
        if (leader_hint)
            *leader_hint = v.leader;
        if (v.role != EFS_RAFT_LEADER) {
            if (proof_only)
                return EFS_ERR_NOT_PRIMARY;
            uint64_t ridx = 0;
            int rhint = -1;
            uint64_t p1;

            if (v.leader == *rp->rp_raft_id)
                return EFS_ERR_NOT_PRIMARY;
            rc = host_remote_read_index(rp, group, v.leader, &end, &ridx,
                                        &rhint);
            p1 = prof ? now_us_() : 0;
            if (prof)
                read_prof_add(&g_read_proof_prof.ri_remote, p1 - p0);
            if (rc == EFS_OK) {
                rc = read_proof_host_wait_applied(rp->rp_host, group, ridx, leader_hint);
                rc = host_follower_read_result(rc,*rp->rp_raft_id,__atomic_load_n(rp->rp_members,__ATOMIC_ACQUIRE),rhint,leader_hint);
            }
            if (prof)
                read_prof_add(&g_read_proof_prof.ri_remote_wait, now_us_() - p1);
            if (rc != EFS_OK) {
                if (prof) efs_telemetry_log(0,"raft-host: admitted-read-remote group=%u rc=%d leader=%d returned_hint=%d remote_index=%llu term=%llu applied=%llu\n",group,rc,v.leader,rhint,(unsigned long long)ridx,(unsigned long long)v.term,(unsigned long long)v.applied);
                return rc;
            }
            if (proof_index)
                *proof_index = ridx;
            if (leader_hint && rhint >= 0)
                *leader_hint = rhint;
            return EFS_OK;
        }
        if (!round) {
            /* Admit this request BEFORE any heartbeat eligible to cover it.
             * Readers arriving during an existing round queue for the next. */
            struct host_read_admission fresh;
            rc=host_read_admit_view(rp,group,&v,&fresh);
            if(rc==EFS_ERR_NOT_PRIMARY){host_view_get(rp->rp_gv,group,&v);continue;}
            if(rc!=EFS_OK)return rc;
            want=fresh.want;round=fresh.round;admitted_term=fresh.term;
            host_view_get(rp->rp_gv,group,&v);
        }
        if (!requested) {
            /* The pump starts the round under h->mu; every reader that
             * needs the same round shares it. */
            host_read_request(rp,group,round);
            requested=1;
        }
        if (v.term != admitted_term) {
            if (prof) efs_telemetry_log(0,"raft-host: admitted-read-term group=%u admitted_term=%llu term=%llu admitted_round=%llu raw_round=%llu proven_round=%llu role=%d leader=%d\n",group,(unsigned long long)admitted_term,(unsigned long long)v.term,(unsigned long long)round,(unsigned long long)v.read_round,(unsigned long long)v.proven_round,v.role,v.leader);
            return EFS_ERR_NOT_PRIMARY;
        }
        (void)host_read_floor_ready(&v,&floor,want,round,admitted_term,proof_only);
        if(floor.context && v.read_context!=floor.context)return EFS_ERR_NOT_PRIMARY;
        if(floor.index) {
            if(proof_only || v.applied>=floor.index) {
                if(proof_index)*proof_index=floor.index;
                if(prof)read_prof_add(first?&g_read_proof_prof.ri_leader_covered:&g_read_proof_prof.ri_leader,now_us_()-p0);
                return EFS_OK;
            }
        }
        if (proof_only && proof_index &&
            host_view_quorum_index(&v, want, round, admitted_term, proof_index)) {
            if (prof)
                read_prof_add(first ? &g_read_proof_prof.ri_leader_covered
                                    : &g_read_proof_prof.ri_leader,
                              now_us_() - p0);
            return EFS_OK;
        }
        if (host_view_covers(&v, want, round, admitted_term)) {
            if (proof_index)
                *proof_index = v.proven_index;
            if (prof)
                read_prof_add(first ? &g_read_proof_prof.ri_leader_covered
                                    : &g_read_proof_prof.ri_leader,
                              now_us_() - p0);
            return EFS_OK;
        }
        first = 0;
        if (host_past_deadline(&end)) {
            if (prof) efs_telemetry_log(0,"raft-host: admitted-read-timeout group=%u want=%llu admitted_round=%llu admitted_term=%llu role=%d leader=%d term=%llu raw_round=%llu raw_index=%llu raw_done=%d pending=%d proven_round=%llu proven_index=%llu proven_done=%d applied=%llu commit=%llu\n",
                group,(unsigned long long)want,(unsigned long long)round,(unsigned long long)admitted_term,v.role,v.leader,(unsigned long long)v.term,(unsigned long long)v.read_round,(unsigned long long)v.read_index,v.read_done,v.read_pending,(unsigned long long)v.proven_round,(unsigned long long)v.proven_index,v.proven_done,(unsigned long long)v.applied,(unsigned long long)v.commit);
            return EFS_ERR_BUSY;
        }
        /* read_done lands when heartbeat replies arrive — the pump
         * publishes the view and wakes the sleepers each cycle. */
        group_view_wait_lock(rp->rp_gv);
        host_view_get(rp->rp_gv, group, &v);
        /* Registration and wake use the same predicate inside the wait bracket. A
         * quorum arriving just before registration must not park an already
         * satisfied proof RPC until another pump/view change. */
        if (!host_read_floor_ready(&v, &floor, want, round, admitted_term, proof_only)) {
            int waiting_apply = prof && host_view_wait_apply(&v, want, round, admitted_term);
            uint64_t sleep_begin = prof ? efs_batch_obs_now_us() : 0;
            uint64_t s0 = now_us_();
            host_waiter_sleep(rp->rp_gv, group, 3, want, v.stamp, &end,
                              round, admitted_term, proof_only, &floor);
            s0 = now_us_() - s0;
            if (prof) read_prof_add(waiting_apply ? &g_read_proof_prof.ri_sleep_apply :
                                                   &g_read_proof_prof.ri_sleep_quorum, efs_batch_obs_now_us() - sleep_begin);
            if (s0 > 20000)
                efs_telemetry_printf(rp->rp_telemetry, "raft-host: read-sleep us=%llu group=%u "
                        "want=%llu read_idx=%llu applied=%llu pending=%d\n",
                        (unsigned long long)s0, group,
                        (unsigned long long)want,
                        (unsigned long long)v.read_index,
                        (unsigned long long)v.applied, v.read_pending);
        }
        group_view_wait_unlock(rp->rp_gv);
        host_view_get(rp->rp_gv, group, &v);
    }
}

int read_proof_read(struct read_proof *rp,uint8_t group,int *hint)
{
    return host_read_index_admitted(rp,group,hint,NULL,NULL,0);
}

int read_proof_prove(struct read_proof *rp,uint8_t group,int *hint,
                     uint64_t *index)
{
    return host_read_index_admitted(rp,group,hint,NULL,index,1);
}

/* Directory walks can overlap both metadata groups on every
 * component. Eight blocked proof helpers otherwise force other admitted
 * reads to serialize independent rounds. Eight persistent 128KiB helpers
 * (1MiB of stacks).
 * Saturation/init failure still falls back, never queues or reuses a proof. */
static unsigned host_read_pair_capacity(void)
{
    return 8u;
}
static void host_read_pair_note(int parallel)
{
    if(read_prof_on())__atomic_fetch_add(parallel?&g_read_proof_prof.pair_parallel:
        &g_read_proof_prof.pair_scalar,1,__ATOMIC_RELAXED);
}
#define HOST_READ_PAIR_LIMIT() host_read_pair_capacity()
#define HOST_READ_PAIR_NOTE(parallel) host_read_pair_note(parallel)
#include "read_index_pair_remote.h"
#undef HOST_READ_PAIR_LIMIT
#undef HOST_READ_PAIR_NOTE

/* Overlap two independent hosted-leader read rounds. Both admissions are
 * captured before either probe is sent. A pending older round cannot cover
 * either admission; the pump starts the rounds both requests asked for.
 * Different hosted leaders use bounded independent proofs; unhosted groups
 * retain ordinary scalar routing. Neither path shares coverage across RPCs. */
int read_proof_read_pair(struct read_proof *rp,uint8_t first,uint8_t second,
                         int *hint)
{
    if(first==second)return read_proof_read(rp,first,hint);
    struct host_read_admission admission[2];
    uint8_t groups[2]={first,second};int rc=EFS_OK;
    /* Admit both from the published view and ri_begun
     * before either round is requested; no h->mu. */
    struct host_view v[2];int lead[2];
    for(unsigned i=0;i<2;i++){
        host_view_get(rp->rp_gv,groups[i],&v[i]);
        lead[i]=v[i].has && v[i].role==EFS_RAFT_LEADER;
    }
    if(!v[0].has || !v[1].has)
        return host_read_index_scalar_pair(rp,first,second,hint);
    if(!lead[0] && !lead[1])
        return host_read_index_remote_pair(rp,first,second,hint);
    for(unsigned i=0;i<2;i++){
        if(!lead[i])continue;
        rc=host_read_admit_view(rp,groups[i],&v[i],&admission[i]);
        if(rc!=EFS_OK)return rc;
    }
    for(unsigned i=0;i<2;i++)
        if(lead[i])host_read_request(rp,groups[i],admission[i].round);
    for(unsigned i=0;i<2;i++){
        rc=lead[i]?host_read_index_admitted(rp,groups[i],hint,&admission[i],NULL,0):
                   read_proof_read(rp,groups[i],hint);
        if(rc!=EFS_OK)return rc;
    }
    return EFS_OK;
}

