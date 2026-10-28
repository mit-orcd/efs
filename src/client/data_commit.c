#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/put_ticket.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* K35 (D42): fsync proves durability per stripe, never per cluster.
 *
 * Every acknowledged fragment of every PUT object is recorded here with the
 * node's acknowledgement sequence and the PUT verifier (boot epoch) of the
 * incarnation that acknowledged it. fsync COMMITs only the nodes holding
 * the file's unproven fragments; a fragment is durable when a COMMIT sent
 * after its acknowledgement succeeds with the same verifier. A verifier
 * that changed before the COMMIT is a lost fragment (the payload may have
 * been released already: never resent, never forgotten).
 *
 * A stripe is proven when two or three fragments are durable and a third
 * that is not is covered by a durable degraded mark: the one its
 * publication carried (W154, never acknowledged) or one this fsync
 * proposes (EFS_MD_CMD_DEGRADED_MARK) before it returns. Fewer than two
 * durable fragments, or a mark that cannot be written, is EIO with the
 * stripe named in the log; the record stays, so a later fsync proves it
 * again or fails again. A proven stripe leaves the table. */
#define FRAG_PENDING 1 /* acknowledged, not yet proven */
#define FRAG_DURABLE 2 /* COMMIT from the acknowledging incarnation */
#define FRAG_LOST 3    /* that node's COMMIT came from another incarnation */
#define FRAG_MISSING 4 /* never acknowledged: published degraded (W154) */
#define FRAG_MARKED 5  /* marked degraded after publication (K35) */

struct stripe {
    efs_ino_t ino;
    uint64_t inode_gen, object;
    uint32_t ci;
    uint32_t next; /* bucket chain or free list: index + 1, 0 ends */
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint64_t seq[EFS_NUM_FRAGMENTS], epoch[EFS_NUM_FRAGMENTS];
    uint8_t state[EFS_NUM_FRAGMENTS];
};

#define STRIPE_BUCKETS (1u << 16)
#ifndef STRIPE_CAP
#define STRIPE_CAP (1u << 18) /* records; a full table is swept first */
#endif
#define COMMIT_NODES 64
_Static_assert(EFS_MAX_NODES < COMMIT_NODES, "node ids index the COMMIT tables");

static pthread_mutex_t commit_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t node_acked[COMMIT_NODES];
static struct stripe *g_st;
static uint32_t g_st_cap, g_st_used, g_st_live, g_st_free;
static uint32_t g_bucket[STRIPE_BUCKETS];
#ifndef EFS_COMMIT_LOOKUP_PROBE
#define EFS_COMMIT_LOOKUP_PROBE() ((void)0)
#endif
/* One sweep at a time; PUTs that meet a full table wait for it. */
static pthread_mutex_t sweep_mu = PTHREAD_MUTEX_INITIALIZER;

uint64_t efs_client_commit_note(efs_node_id_t node)
{
    if (node >= COMMIT_NODES)
        return 0;
    pthread_mutex_lock(&commit_mu);
    uint64_t seq = ++node_acked[node];
    pthread_mutex_unlock(&commit_mu);
    return seq;
}

static uint32_t bucket_of(efs_ino_t ino)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 48) & (STRIPE_BUCKETS - 1);
}

/* Caller holds commit_mu. */
static struct stripe *stripe_find(efs_ino_t ino, uint32_t ci, uint64_t object)
{
    for (uint32_t at = g_bucket[bucket_of(ino)]; at; at = g_st[at - 1].next) {
        EFS_COMMIT_LOOKUP_PROBE();
        struct stripe *p = &g_st[at - 1];
        if (p->ino == ino && p->ci == ci && p->object == object)
            return p;
    }
    return NULL;
}

/* Caller holds commit_mu. */
static void stripe_remove(efs_ino_t ino, uint32_t ci, uint64_t object)
{
    uint32_t *link = &g_bucket[bucket_of(ino)];
    while (*link) {
        EFS_COMMIT_LOOKUP_PROBE();
        uint32_t at = *link;
        struct stripe *p = &g_st[at - 1];
        if (p->ino == ino && p->ci == ci && p->object == object) {
            *link = p->next;
            memset(p, 0, sizeof(*p));
            p->next = g_st_free;
            g_st_free = at;
            g_st_live--;
            return;
        }
        link = &p->next;
    }
}

/* Caller holds commit_mu. EFS_ERR_NOMEM when full. */
static int stripe_insert(const struct stripe *s)
{
    uint32_t at;
    if (g_st_free) {
        at = g_st_free;
        g_st_free = g_st[at - 1].next;
    } else {
        if (g_st_used == g_st_cap) {
            uint32_t cap = g_st_cap ? g_st_cap * 2 : 1024;
            if (cap > STRIPE_CAP)
                cap = STRIPE_CAP;
            if (cap == g_st_cap)
                return EFS_ERR_NOMEM;
            struct stripe *nv = realloc(g_st, (size_t)cap * sizeof(*nv));
            if (!nv)
                return EFS_ERR_NOMEM;
            g_st = nv;
            g_st_cap = cap;
        }
        at = ++g_st_used;
    }
    struct stripe *p = &g_st[at - 1];
    *p = *s;
    uint32_t b = bucket_of(s->ino);
    p->next = g_bucket[b];
    g_bucket[b] = at;
    g_st_live++;
    return EFS_OK;
}

static int stripe_note_locked(const struct efs_put_ticket *t,
                              const uint64_t seq[EFS_NUM_FRAGMENTS],
                              const uint64_t epoch[EFS_NUM_FRAGMENTS])
{
    struct stripe *p = stripe_find(t->ino, t->ci, t->object);
    if (p) {
        /* The same object again (a resent ticket): a new acknowledgement
         * of a fragment not yet proven replaces the older one. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            if (seq[i] && (p->state[i] == FRAG_PENDING || p->state[i] == FRAG_MISSING ||
                           p->state[i] == FRAG_LOST)) {
                p->state[i] = FRAG_PENDING;
                p->seq[i] = seq[i];
                p->epoch[i] = epoch[i];
            }
        return EFS_OK;
    }
    struct stripe s;
    memset(&s, 0, sizeof(s));
    s.ino = t->ino;
    s.inode_gen = t->inode_gen;
    s.object = t->object;
    s.ci = t->ci;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        s.nodes[i] = t->nodes[i];
        s.seq[i] = seq[i];
        s.epoch[i] = epoch[i];
        s.state[i] = seq[i] ? FRAG_PENDING : FRAG_MISSING;
    }
    return stripe_insert(&s);
}

static int stripe_prove(efs_ino_t ino, int all);

int efs_client_stripe_note(const struct efs_put_ticket *t,
                           const uint64_t seq[EFS_NUM_FRAGMENTS],
                           const uint64_t epoch[EFS_NUM_FRAGMENTS])
{
    int acked = 0;
    if (!t || !t->ino || !t->object || !t->inode_gen)
        return EFS_ERR_INVAL;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!t->nodes[i] || t->nodes[i] >= COMMIT_NODES || (seq[i] && !epoch[i]))
            return EFS_ERR_INVAL;
        acked += seq[i] != 0;
    }
    if (acked < 2)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&commit_mu);
    int rc = stripe_note_locked(t, seq, epoch);
    pthread_mutex_unlock(&commit_mu);
    if (rc != EFS_ERR_NOMEM)
        return rc;
    /* Full: prove every recorded stripe (COMMIT, marks) and retry once. A
     * table that stays full refuses the PUT rather than forget a record. */
    pthread_mutex_lock(&sweep_mu);
    pthread_mutex_lock(&commit_mu);
    rc = stripe_note_locked(t, seq, epoch);
    pthread_mutex_unlock(&commit_mu);
    if (rc == EFS_ERR_NOMEM) {
        (void)stripe_prove(0, 1);
        pthread_mutex_lock(&commit_mu);
        rc = stripe_note_locked(t, seq, epoch);
        pthread_mutex_unlock(&commit_mu);
    }
    pthread_mutex_unlock(&sweep_mu);
    if (rc == EFS_ERR_NOMEM) {
        fprintf(stderr, "data-commit: %u unproven stripes recorded (cap %u); refusing "
                "PUT ino=%llu chunk=%u until fsync proves them\n", g_st_live,
                (unsigned)STRIPE_CAP, (unsigned long long)t->ino, t->ci);
        rc = EFS_ERR_BUSY;
    }
    return rc;
}

struct commit_job { efs_node_id_t node; uint64_t target, epoch, deadline; int rc; };

/* One COMMIT. rc OK with the reply's verifier in epoch, or the failure. */
static void *commit_one(void *arg)
{
    struct commit_job *j = arg;
    uint64_t saved = efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(j->deadline);
    j->rc = EFS_ERR_NET;
    /* COMMIT is idempotent. A planned restart leaves an old pooled socket;
     * retry transport failure once with a new connection, inside the same
     * deadline and acknowledged snapshot. Never retry a verifier/error reply. */
    for(unsigned attempt=0;attempt<2;attempt++) {
        struct efs_conn *conn = efs_client_conn_get(j->node);
        if (conn) {
            uint8_t type = 0; void *reply = NULL; uint32_t bytes = 0;
            if (efs_conn_send_msg(conn, EFS_MSG_DATA_COMMIT, NULL, 0) ||
                efs_conn_recv_msg(conn, &type, &reply, &bytes)) {
                efs_client_conn_drop(j->node, conn);
            } else {
                efs_client_conn_release(j->node, conn);
                if (type == EFS_MSG_DATA_COMMIT_REPLY &&
                    bytes == sizeof(struct efs_msg_data_commit_reply)) {
                    const struct efs_msg_data_commit_reply *r = reply;
                    /* A refusal (failed barrier, uncertain recovery) proves
                     * nothing on this node. */
                    j->rc = r->rc == EFS_OK && r->boot_epoch ? EFS_OK : EFS_ERR_IO;
                    j->epoch = r->boot_epoch;
                } else {
                    j->rc = EFS_ERR_PROTO;
                }
            }
            free(reply);
        }
        if(j->rc!=EFS_ERR_NET)break;
        /* A peer that restarted inside the client's down cooldown is
         * listening again before any checkout re-probes it; probe it here
         * rather than leave its fragments unproven. */
        if (!conn && !efs_client_node_reprobe(j->node))
            break;
    }
    efs_client_rpc_set_deadline_ms(saved);
    return NULL;
}

static char frag_letter(uint8_t st)
{
    return st == FRAG_DURABLE ? 'D' : st == FRAG_PENDING ? 'P' : st == FRAG_LOST ? 'L' :
           st == FRAG_MISSING ? 'M' : st == FRAG_MARKED ? 'K' : '?';
}

static void stripe_log(const char *what, const struct stripe *s, int durable,
                       const char *why, int rc)
{
    fprintf(stderr, "%s ino=%llu chunk=%u gen=%llx nodes=%u,%u,%u fragments=%c%c%c: "
            "%d of 3 durable; %s (rc=%d)\n", what, (unsigned long long)s->ino, s->ci,
            (unsigned long long)s->object, (unsigned)s->nodes[0], (unsigned)s->nodes[1],
            (unsigned)s->nodes[2], frag_letter(s->state[0]), frag_letter(s->state[1]),
            frag_letter(s->state[2]), durable, why, rc);
}

/* Prove the recorded stripes of `ino` (all of them when `all`). */
static int stripe_prove(efs_ino_t ino, int all)
{
    struct commit_job jobs[COMMIT_NODES];
    pthread_t th[COMMIT_NODES];
    int started[COMMIT_NODES] = {0}, slot[COMMIT_NODES], n = 0;
    uint64_t target[COMMIT_NODES] = {0};
    struct stripe *snap = NULL;
    uint32_t ns = 0, cap = 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return EFS_ERR_IO;
    uint64_t deadline = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 60000;
    uint64_t parent = efs_client_rpc_deadline_ms();
    if (parent && parent < deadline)
        deadline = parent;

    pthread_mutex_lock(&commit_mu);
    for (uint32_t b = all ? 0 : bucket_of(ino); b < STRIPE_BUCKETS; b++) {
        for (uint32_t at = g_bucket[b]; at; at = g_st[at - 1].next) {
            const struct stripe *p = &g_st[at - 1];
            if (!all && p->ino != ino)
                continue;
            if (ns == cap) {
                uint32_t nc = cap ? cap * 2 : 64;
                struct stripe *nv = realloc(snap, (size_t)nc * sizeof(*nv));
                if (!nv) {
                    pthread_mutex_unlock(&commit_mu);
                    free(snap);
                    return EFS_ERR_NOMEM;
                }
                snap = nv;
                cap = nc;
            }
            snap[ns++] = *p;
        }
        if (!all)
            break;
    }
    for (efs_node_id_t node = 0; node < COMMIT_NODES; node++)
        target[node] = node_acked[node];
    pthread_mutex_unlock(&commit_mu);
    if (!ns)
        return EFS_OK; /* nothing this client wrote is unproven */

    uint64_t need = 0;
    for (uint32_t k = 0; k < ns; k++)
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            if (snap[k].state[i] == FRAG_PENDING)
                need |= 1ull << snap[k].nodes[i];
    for (efs_node_id_t node = 0; node < COMMIT_NODES; node++) {
        slot[node] = -1;
        if (!(need & (1ull << node)))
            continue;
        jobs[n] = (struct commit_job){ .node = node, .target = target[node],
                                       .deadline = deadline, .rc = EFS_ERR_NET };
        slot[node] = n;
        started[n] = pthread_create(&th[n], NULL, commit_one, &jobs[n]) == 0;
        if (!started[n])
            commit_one(&jobs[n]);
        n++;
    }
    for (int i = 0; i < n; i++)
        if (started[i])
            pthread_join(th[i], NULL);

    /* Fold the COMMIT verdicts into the live records (a concurrent fsync
     * may have proven or retired some meanwhile). */
    pthread_mutex_lock(&commit_mu);
    for (uint32_t k = 0; k < ns; k++) {
        struct stripe *p = stripe_find(snap[k].ino, snap[k].ci, snap[k].object);
        if (!p) {
            snap[k].ino = 0; /* proven and retired by another fsync */
            continue;
        }
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            int s = slot[p->nodes[i]];
            if (p->state[i] != FRAG_PENDING || s < 0 || jobs[s].rc != EFS_OK ||
                p->seq[i] > jobs[s].target)
                continue;
            p->state[i] = p->epoch[i] == jobs[s].epoch ? FRAG_DURABLE : FRAG_LOST;
        }
        snap[k] = *p;
    }
    pthread_mutex_unlock(&commit_mu);

    int rc = EFS_OK;
    unsigned logged = 0, failed = 0, marked = 0;
    const char *what = all ? "data-commit sweep" : "fsync";
    for (uint32_t k = 0; k < ns; k++) {
        struct stripe *s = &snap[k];
        int durable = 0, open = -1, opens = 0, retire = 0;
        if (!s->ino)
            continue;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (s->state[i] == FRAG_DURABLE)
                durable++;
            else if (s->state[i] == FRAG_PENDING || s->state[i] == FRAG_LOST) {
                open = i;
                opens++;
            }
        }
        if (durable >= 2 && !opens) {
            retire = 1;
        } else if (durable == 2 && opens == 1) {
            /* No tolerance left: the mark must be durable before success. */
            int mrc = efs_client_rpc_degraded_mark(s->ino, s->inode_gen, s->object, s->ci,
                                                   (uint32_t)open, s->nodes[open]);
            if (mrc == EFS_OK) {
                s->state[open] = FRAG_MARKED;
                if (marked++ < 8)
                    stripe_log(what, s, durable, "marked degraded for rebuild on return", mrc);
                retire = 1;
            } else if (mrc == EFS_ERR_NOT_FOUND) {
                /* Rewritten, truncated, packed or unlinked since: this object
                 * is no longer the published version. */
                retire = 1;
            } else {
                if (logged++ < 8)
                    stripe_log(what, s, durable, "degraded mark not written", mrc);
                failed++;
            }
        } else {
            if (logged++ < 8)
                stripe_log(what, s, durable, s->state[0] == FRAG_LOST || s->state[1] == FRAG_LOST ||
                           s->state[2] == FRAG_LOST
                               ? "need 2 (a member of another incarnation lost a fragment)"
                               : "need 2", EFS_ERR_IO);
            failed++;
        }
        if (retire) {
            pthread_mutex_lock(&commit_mu);
            stripe_remove(s->ino, s->ci, s->object);
            pthread_mutex_unlock(&commit_mu);
        }
    }
    if (marked > 8)
        fprintf(stderr, "%s: %u more stripes marked degraded\n", what, marked - 8);
    if (failed) {
        if (logged > 8)
            fprintf(stderr, "%s: %u more stripes without proof\n", what, logged - 8);
        rc = EFS_ERR_IO;
    }
    free(snap);
    return rc;
}

int efs_client_data_commit(efs_ino_t ino)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return stripe_prove(ino, 0);
}
