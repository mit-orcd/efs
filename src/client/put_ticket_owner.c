#include "client_internal.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* K13 mount ticket ledger. Every data PUT object is named by a ticket that
 * is admitted before its first fragment is sent. The ledger holds every
 * ticket this mount may have admitted, in sequence order, until the lane
 * shard retires it: an ambiguous admission or PUT still has to be settled.
 * A ticket is done once no REPORT will carry it again (its mapping was
 * confirmed, its object was superseded, or its PUT failed); settle then
 * retires it (PUBLISHED) or abandons it (REJECTED, body kept for the
 * revocation-gated collector, D31) in per-stream sequence order. */
#define LEDGER_MAX 65536u
struct ledger_entry {
    struct efs_put_ticket ticket;
    uint8_t putting; /* PUT or its table update in progress: never swept */
    uint8_t done;
    uint8_t sent; /* an ADMIT may have reached a server */
};
static struct {
    pthread_mutex_t mu, settle_mu;
    struct ledger_entry *e;
    uint32_t n, cap;
} g_ledger = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};

static uint32_t ticket_shard(const struct efs_put_ticket *t)
{
    return efs_kv_lane_shard(t->ino, t->ci % EFS_META_LANES);
}

/* EFS_PUT_TICKET_DBG=1: log refused admissions and settle batches. */
static int ticket_debug(void)
{
    static int on = -1;
    if (on < 0) on = getenv("EFS_PUT_TICKET_DBG") != NULL;
    return on;
}

/* ino -> FileID generation. Tickets bind one FileID; inode numbers are never
 * reused, so a resolved generation stays valid for the inode's life. */
#define GEN_SLOTS 4096u
static struct { efs_ino_t ino; uint64_t gen; } g_gen[GEN_SLOTS];
static pthread_mutex_t g_gen_mu = PTHREAD_MUTEX_INITIALIZER;

static int file_generation(efs_ino_t ino, uint64_t *gen)
{
    uint32_t slot = (uint32_t)((ino * 0x9E3779B97F4A7C15ull) >> 52) % GEN_SLOTS;
    pthread_mutex_lock(&g_gen_mu);
    if (g_gen[slot].ino == ino && g_gen[slot].gen) {
        *gen = g_gen[slot].gen;
        pthread_mutex_unlock(&g_gen_mu);
        return EFS_OK;
    }
    pthread_mutex_unlock(&g_gen_mu);
    uint32_t count = 1;
    uint64_t resolved = 0;
    int rc = efs_client_rpc_getchunks_fileid(g_client.export_id, ino, 0, 0, NULL, &count,
                                             &resolved);
    if (rc != EFS_OK) return rc;
    if (!resolved) return EFS_ERR_PROTO;
    pthread_mutex_lock(&g_gen_mu);
    g_gen[slot].ino = ino;
    g_gen[slot].gen = resolved;
    pthread_mutex_unlock(&g_gen_mu);
    *gen = resolved;
    return EFS_OK;
}

/* Caller holds g_ledger.mu. */
static struct ledger_entry *ledger_find(uint64_t seq)
{
    uint32_t lo = 0, hi = g_ledger.n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_ledger.e[mid].ticket.id.seq < seq) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_ledger.n && g_ledger.e[lo].ticket.id.seq == seq ? &g_ledger.e[lo] : NULL;
}

/* Build the immutable ticket and append it in sequence order. The sequence
 * is allocated under the ledger lock, so the ledger stays sorted. */
static int ledger_add(efs_ino_t ino, uint32_t ci, uint64_t gen,
                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                      uint32_t frag_len, uint32_t delta_off, uint32_t delta_len,
                      struct efs_put_ticket *out)
{
    if (g_client.node_count < EFS_NUM_FRAGMENTS || g_client.node_count > EFS_MAX_NODES)
        return EFS_ERR_INVAL;
    struct efs_meta_pub p;
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.chunk_index = ci;
    p.inode_gen = gen;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.fresh_object = 1;
    p.ticketed = 1;
    p.put_fragment_len = frag_len;
    p.put_member_mask = g_client.node_count >= 32 ? 0xffffffffu :
                        (1u << g_client.node_count) - 1u;
    p.delta_off = delta_off;
    p.delta_len = delta_len;
    memcpy(p.ch.nodes, nodes, sizeof(p.ch.nodes));
    memcpy(p.ch.checksums, checksums, sizeof(p.ch.checksums));
    pthread_mutex_lock(&g_ledger.mu);
    if (g_ledger.n >= LEDGER_MAX) {
        pthread_mutex_unlock(&g_ledger.mu);
        return EFS_ERR_BUSY;
    }
    if (g_ledger.n == g_ledger.cap) {
        uint32_t cap = g_ledger.cap ? g_ledger.cap * 2 : 256;
        struct ledger_entry *grown = realloc(g_ledger.e, (size_t)cap * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&g_ledger.mu);
            return EFS_ERR_NOMEM;
        }
        g_ledger.e = grown;
        g_ledger.cap = cap;
    }
    int rc = efs_client_put_id(&p.put_id);
    if (rc == EFS_OK) {
        p.publication_id = p.put_id;
        p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, p.put_id.session_epoch,
                                                 p.put_id.seq, ci, 0);
        rc = efs_put_ticket_from_pub(&p, out);
    }
    if (rc == EFS_OK) {
        uint8_t wire[EFS_PUT_TICKET_WIRE_LEN];
        rc = efs_put_ticket_encode(out, wire);
    }
    if (rc != EFS_OK) {
        pthread_mutex_unlock(&g_ledger.mu);
        return rc;
    }
    struct ledger_entry *e = &g_ledger.e[g_ledger.n++];
    memset(e, 0, sizeof(*e));
    e->ticket = *out;
    e->putting = 1;
    pthread_mutex_unlock(&g_ledger.mu);
    return EFS_OK;
}

static void ledger_mark_sent(uint64_t seq)
{
    pthread_mutex_lock(&g_ledger.mu);
    struct ledger_entry *e = ledger_find(seq);
    if (e) e->sent = 1;
    pthread_mutex_unlock(&g_ledger.mu);
}

/* ---- K32 · admission group commit ------------------------------------------
 *
 * One ADMIT entry per object was eight Raft entries per 1 MiB write. Tickets
 * queue per Raft group; one queued thread at a time sends every ticket
 * queued for its group (up to EFS_PUT_TICKET_BATCH_MAX) as one ADMIT_BATCH,
 * so the queue fills while the previous batch is in flight. The sender
 * first waits, bounded by EFS_PUT_ADMIT_LINGER_US, for the running members
 * of its own cohort (the other chunks of its write). A ticket the batch did
 * not admit, a failed batch RPC and a lone ticket all take the single ADMIT
 * path below, with its exact verdicts and retries; admission is idempotent,
 * so a batch that committed unseen costs nothing. */
#define ADMIT_GROUPS 4u /* Raft group ids (efs_raft_shard_group) */
struct admit_slot {
    struct admit_slot *next;
    struct efs_put_ticket *ticket;
    int done, admitted;
};
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct admit_slot *head[ADMIT_GROUPS], **tail[ADMIT_GROUPS];
    uint32_t queued[ADMIT_GROUPS];
    int sending[ADMIT_GROUPS];
} g_admit = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, {0}, {0}, {0}};
static __thread struct efs_put_admit_cohort *tl_cohort;
static __thread int tl_cohort_queued;

void efs_client_put_cohort_enter(struct efs_put_admit_cohort *c)
{
    pthread_mutex_lock(&g_admit.mu);
    tl_cohort = c;
    tl_cohort_queued = 0;
    if (c) c->running++;
    pthread_mutex_unlock(&g_admit.mu);
}

/* Caller holds g_admit.mu. */
static void cohort_arrived(void)
{
    if (tl_cohort && !tl_cohort_queued && tl_cohort->running) {
        tl_cohort->running--;
        pthread_cond_broadcast(&g_admit.cv);
    }
    tl_cohort_queued = 1;
}

void efs_client_put_cohort_leave(void)
{
    pthread_mutex_lock(&g_admit.mu);
    cohort_arrived();
    tl_cohort = NULL;
    pthread_mutex_unlock(&g_admit.mu);
}

static void admit_deadline(struct timespec *ts, long us)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += us * 1000L;
    ts->tv_sec += ts->tv_nsec / 1000000000L;
    ts->tv_nsec %= 1000000000L;
}

/* 1 when a batch admitted *t; 0: use the single form. */
static int admit_batched(struct efs_put_ticket *t)
{
    uint32_t g = efs_raft_shard_group(ticket_shard(t));
    struct admit_slot me = {NULL, t, 0, 0};
    struct efs_put_admit_cohort *mine = tl_cohort;

    if (g >= ADMIT_GROUPS) return 0;
    pthread_mutex_lock(&g_admit.mu);
    if (!g_admit.tail[g]) g_admit.tail[g] = &g_admit.head[g];
    *g_admit.tail[g] = &me;
    g_admit.tail[g] = &me.next;
    g_admit.queued[g]++;
    cohort_arrived();
    while (!me.done) {
        if (g_admit.sending[g]) {
            pthread_cond_wait(&g_admit.cv, &g_admit.mu);
            continue;
        }
        g_admit.sending[g] = 1;
        struct timespec until;
        admit_deadline(&until, EFS_PUT_ADMIT_LINGER_US);
        while (mine && mine->running && g_admit.queued[g] < EFS_PUT_TICKET_BATCH_MAX &&
               pthread_cond_timedwait(&g_admit.cv, &g_admit.mu, &until) == 0)
            ;
        struct admit_slot *take[EFS_PUT_TICKET_BATCH_MAX];
        struct efs_put_ticket batch[EFS_PUT_TICKET_BATCH_MAX];
        uint32_t n = 0, mask = 0;
        while (g_admit.head[g] && n < EFS_PUT_TICKET_BATCH_MAX) {
            take[n] = g_admit.head[g];
            g_admit.head[g] = take[n]->next;
            batch[n] = *take[n]->ticket;
            n++;
        }
        if (!g_admit.head[g]) g_admit.tail[g] = &g_admit.head[g];
        g_admit.queued[g] -= n;
        pthread_mutex_unlock(&g_admit.mu);
        int rc = EFS_ERR_INVAL;
        if (n >= 2) {
            for (uint32_t i = 0; i < n; i++) ledger_mark_sent(batch[i].id.seq);
            rc = efs_client_rpc_put_ticket_batch(batch, n, 0, &mask);
            if (ticket_debug())
                fprintf(stderr, "put-ticket admit batch group=%u n=%u mask=%#x rc=%d\n",
                        g, n, mask, rc);
        }
        pthread_mutex_lock(&g_admit.mu);
        for (uint32_t i = 0; i < n; i++) {
            take[i]->admitted = rc == EFS_OK && (mask >> i & 1u);
            take[i]->done = 1;
        }
        g_admit.sending[g] = 0;
        pthread_cond_broadcast(&g_admit.cv);
    }
    pthread_mutex_unlock(&g_admit.mu);
    return me.admitted;
}

int efs_client_put_admit(efs_ino_t ino, uint32_t ci,
                         const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                         uint32_t frag_len, uint32_t delta_off, uint32_t delta_len,
                         struct efs_put_ticket *out)
{
    if (!ino || !nodes || !checksums || !out) return EFS_ERR_INVAL;
    uint64_t gen = 0;
    int rc = file_generation(ino, &gen);
    if (rc != EFS_OK) return rc;
    for (unsigned attempt = 0; ; attempt++) {
        rc = ledger_add(ino, ci, gen, nodes, checksums, frag_len, delta_off, delta_len, out);
        if (rc != EFS_ERR_BUSY) break;
        efs_client_put_settle_kick();
        if (efs_client_rpc_past_deadline() || attempt >= 50) return EFS_ERR_BUSY;
        usleep(20000);
    }
    if (rc != EFS_OK) return rc;
    /* From here the ticket may be admitted: it stays in the ledger until a
     * settle retires it, whatever this call returns. The lane shard must
     * know this session first (K5); the client caches that per shard. */
    uint32_t state = 0;
    rc = efs_client_session_admit(ticket_shard(out), &out->id);
    if (rc == EFS_OK && admit_batched(out))
        return EFS_OK;
    for (unsigned attempt = 0; rc == EFS_OK; attempt++) {
        if (!attempt) ledger_mark_sent(out->id.seq);
        rc = efs_client_rpc_put_ticket(out, 0, &state);
        /* UNREGISTERED: this lane shard has not admitted the session (K5). */
        if ((rc == EFS_ERR_UNREGISTERED || rc == EFS_ERR_NOT_FOUND) && attempt < 8) {
            rc = efs_client_session_admit(ticket_shard(out), &out->id);
            if (rc == EFS_OK) continue;
        }
        if (rc != EFS_ERR_BUSY || efs_client_rpc_past_deadline() || attempt >= 50) break;
        /* Stream full: retire what this mount is done with, then retry. */
        efs_client_put_settle_kick();
        usleep(20000);
        rc = EFS_OK;
    }
    if (rc == EFS_OK && state != EFS_PUT_TICKET_ADMITTED) rc = EFS_ERR_STALE;
    if (rc != EFS_OK) efs_client_put_release(out->id.seq, 0);
    if (rc != EFS_OK && ticket_debug())
        fprintf(stderr, "put-ticket admit ino=%llu ci=%u seq=%llu shard=%u rc=%d state=%u\n",
                (unsigned long long)ino, ci, (unsigned long long)out->id.seq,
                ticket_shard(out), rc, state);
    return rc;
}

void efs_client_put_release(uint64_t seq, int landed)
{
    pthread_mutex_lock(&g_ledger.mu);
    struct ledger_entry *e = ledger_find(seq);
    if (e && !landed && !e->sent) {
        /* Never offered to any server: nothing to settle. */
        uint32_t at = (uint32_t)(e - g_ledger.e);
        memmove(e, e + 1, (size_t)(g_ledger.n - at - 1) * sizeof(*e));
        g_ledger.n--;
    } else if (e) {
        e->putting = 0;
        if (!landed) e->done = 1;
    }
    pthread_mutex_unlock(&g_ledger.mu);
}

void efs_client_put_done(efs_ino_t ino, uint32_t ci, uint64_t object)
{
    if (!object) return;
    pthread_mutex_lock(&g_ledger.mu);
    for (uint32_t i = 0; i < g_ledger.n; i++) {
        struct ledger_entry *e = &g_ledger.e[i];
        if (e->ticket.object == object && e->ticket.ino == ino && e->ticket.ci == ci &&
            !e->putting)
            e->done = 1;
    }
    pthread_mutex_unlock(&g_ledger.mu);
}

uint32_t efs_client_put_outstanding(void)
{
    pthread_mutex_lock(&g_ledger.mu);
    uint32_t n = g_ledger.n;
    pthread_mutex_unlock(&g_ledger.mu);
    return n;
}

/* One pass. Per lane-shard stream, the done tickets that precede its first
 * live ticket are settled in sequence order, up to one SETTLE batch each.
 * Runs under settle_mu only. */
#define PASS_STREAMS 1024u
struct settle_batch {
    uint32_t shard, n;
    int closed; /* a live ticket of this stream was seen */
    uint64_t seq[EFS_PUT_TICKET_SETTLE_MAX];
};
static struct settle_batch g_pass[PASS_STREAMS];

/* Ledger entries of these sequences, until the first one that is gone. */
static uint32_t settle_tickets(const uint64_t *seq, uint32_t n, struct efs_put_ticket *t)
{
    uint32_t got = 0;
    pthread_mutex_lock(&g_ledger.mu);
    for (uint32_t j = 0; j < n; j++) {
        struct ledger_entry *e = ledger_find(seq[j]);
        if (!e) break;
        t[got++] = e->ticket;
    }
    pthread_mutex_unlock(&g_ledger.mu);
    return got;
}

static void settle_forget(const struct efs_put_ticket *t, uint32_t n, uint32_t mask, int leading)
{
    pthread_mutex_lock(&g_ledger.mu);
    for (uint32_t j = 0; j < n; j++) {
        if (leading ? j >= mask : !(mask >> j & 1u)) continue;
        struct ledger_entry *gone = ledger_find(t[j].id.seq);
        if (!gone) continue;
        uint32_t at = (uint32_t)(gone - g_ledger.e);
        memmove(gone, gone + 1, (size_t)(g_ledger.n - at - 1) * sizeof(*gone));
        g_ledger.n--;
    }
    pthread_mutex_unlock(&g_ledger.mu);
}

/* (Raft group, lane shard) order of the pass's streams (g_pass indices). */
static int settle_stream_cmp(const void *x, const void *y)
{
    const struct settle_batch *a = &g_pass[*(const uint32_t *)x], *b = &g_pass[*(const uint32_t *)y];
    uint32_t ga = efs_raft_shard_group(a->shard), gb = efs_raft_shard_group(b->shard);
    if (ga != gb) return ga < gb ? -1 : 1;
    return a->shard < b->shard ? -1 : a->shard > b->shard;
}

/* One stream, one SETTLE (*state = leading retired count). */
static int settle_stream(const struct settle_batch *sb, int *progress)
{
    struct efs_put_ticket t[EFS_PUT_TICKET_SETTLE_MAX];
    uint32_t retired = 0, n = settle_tickets(sb->seq, sb->n, t);
    if (!n) return EFS_OK;
    int rc = efs_client_rpc_put_ticket_settle(t, n, &retired);
    if (ticket_debug())
        fprintf(stderr, "put-ticket settle shard=%u n=%u first=%llu retired=%u rc=%d\n",
                sb->shard, n, (unsigned long long)t[0].id.seq, retired, rc);
    if (rc == EFS_OK && retired) {
        settle_forget(t, n, retired, 1);
        *progress = 1;
    }
    return rc;
}

/* Several streams of one group, one SETTLE_BATCH (mask of retired). */
static int settle_streams(const uint32_t *k, uint32_t nk, int *progress)
{
    struct efs_put_ticket t[EFS_PUT_TICKET_BATCH_MAX];
    uint32_t n = 0, mask = 0;
    for (uint32_t i = 0; i < nk; i++)
        n += settle_tickets(g_pass[k[i]].seq, g_pass[k[i]].n, t + n);
    if (n < 2) {
        if (!n) return EFS_OK;
        struct settle_batch one = {ticket_shard(&t[0]), 1, 0, {t[0].id.seq}};
        return settle_stream(&one, progress);
    }
    int rc = efs_client_rpc_put_ticket_batch(t, n, 1, &mask);
    if (ticket_debug())
        fprintf(stderr, "put-ticket settle batch streams=%u n=%u mask=%#x rc=%d\n",
                nk, n, mask, rc);
    if (rc == EFS_OK && mask) {
        settle_forget(t, n, mask, 0);
        *progress = 1;
    }
    return rc;
}

static int settle_pass(int *progress)
{
    uint32_t nb = 0;
    *progress = 0;
    for (uint32_t i = 0; ; i++) {
        struct efs_put_ticket t;
        int done, putting;
        pthread_mutex_lock(&g_ledger.mu);
        if (i >= g_ledger.n) {
            pthread_mutex_unlock(&g_ledger.mu);
            break;
        }
        t = g_ledger.e[i].ticket;
        done = g_ledger.e[i].done;
        putting = g_ledger.e[i].putting;
        pthread_mutex_unlock(&g_ledger.mu);
        uint32_t shard = ticket_shard(&t), k;
        for (k = 0; k < nb && g_pass[k].shard != shard; k++) ;
        if (k == nb) {
            if (nb == PASS_STREAMS) continue; /* next pass */
            g_pass[nb].shard = shard; g_pass[nb].n = 0; g_pass[nb].closed = 0; nb++;
        }
        if (g_pass[k].closed || g_pass[k].n == EFS_PUT_TICKET_SETTLE_MAX) continue;
        /* A ticket no table names any more will never be reported again.
         * Checked without the ledger lock: the tables take their own locks
         * and call efs_client_put_done under them. A landed object is in
         * its table before its PUT releases the ticket. */
        if (!done && !putting && !efs_client_put_referenced(t.ino, t.ci, t.object)) {
            pthread_mutex_lock(&g_ledger.mu);
            struct ledger_entry *e = ledger_find(t.id.seq);
            if (e && !e->putting) e->done = done = 1;
            pthread_mutex_unlock(&g_ledger.mu);
        }
        if (!done) { g_pass[k].closed = 1; continue; }
        g_pass[k].seq[g_pass[k].n++] = t.id.seq;
    }
    /* K32: the streams of one Raft group share SETTLE_BATCH entries (one
     * entry per stream was most of a write's settle cost: a file's chunks
     * spread over 64 lane-shard streams). Whole streams only, sorted by
     * lane shard; a stream alone keeps the single SETTLE. */
    uint32_t order[PASS_STREAMS], no = 0;
    for (uint32_t k = 0; k < nb; k++)
        if (g_pass[k].n) order[no++] = k;
    qsort(order, no, sizeof(order[0]), settle_stream_cmp);
    int result = EFS_OK;
    for (uint32_t a = 0, b; a < no; a = b) {
        uint32_t g = efs_raft_shard_group(g_pass[order[a]].shard), total = g_pass[order[a]].n;
        for (b = a + 1; b < no && efs_raft_shard_group(g_pass[order[b]].shard) == g &&
                        total + g_pass[order[b]].n <= EFS_PUT_TICKET_BATCH_MAX; b++)
            total += g_pass[order[b]].n;
        int rc = b - a == 1 ? settle_stream(&g_pass[order[a]], progress)
                            : settle_streams(order + a, b - a, progress);
        if (rc != EFS_OK && rc != EFS_ERR_BUSY && result == EFS_OK) result = rc;
        if (rc == EFS_ERR_FENCED || rc == EFS_ERR_NET) break;
    }
    return result;
}

int efs_client_put_settle(void)
{
    if (pthread_mutex_trylock(&g_ledger.settle_mu)) return EFS_ERR_BUSY;
    int progress = 1, rc = EFS_OK;
    uint64_t saved = efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(0);
    for (unsigned round = 0; progress && round < 64 && rc == EFS_OK; round++) {
        if (efs_client_stop_owner())
            efs_client_stop_phase("settling", round + 1, 64);
        rc = settle_pass(&progress);
    }
    efs_client_rpc_set_deadline_ms(saved);
    pthread_mutex_unlock(&g_ledger.settle_mu);
    return rc;
}

/* Background settle: off the FUSE, REPORT and fsync threads. */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t tid;
    int started, stop, kicked;
} g_settle = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};

static void *settle_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_settle.mu);
    while (!g_settle.stop) {
        if (!g_settle.kicked) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_settle.cv, &g_settle.mu, &ts);
        }
        if (g_settle.stop) break;
        g_settle.kicked = 0;
        pthread_mutex_unlock(&g_settle.mu);
        if (efs_client_put_outstanding()) (void)efs_client_put_settle();
        pthread_mutex_lock(&g_settle.mu);
    }
    pthread_mutex_unlock(&g_settle.mu);
    return NULL;
}

void efs_client_put_settle_kick(void)
{
    pthread_mutex_lock(&g_settle.mu);
    if (!g_settle.started && !g_settle.stop &&
        !pthread_create(&g_settle.tid, NULL, settle_main, NULL))
        g_settle.started = 1;
    g_settle.kicked = 1;
    pthread_cond_signal(&g_settle.cv);
    pthread_mutex_unlock(&g_settle.mu);
}

void efs_client_put_settle_stop(void)
{
    pthread_mutex_lock(&g_settle.mu);
    int started = g_settle.started;
    g_settle.stop = 1;
    pthread_cond_signal(&g_settle.cv);
    pthread_mutex_unlock(&g_settle.mu);
    if (started) pthread_join(g_settle.tid, NULL);
    pthread_mutex_lock(&g_settle.mu);
    g_settle.started = 0;
    pthread_mutex_unlock(&g_settle.mu);
    /* Last pass at unmount. Unretired tickets stay for the collector. */
    int rc = efs_client_put_settle();
    fprintf(stderr, "efs: put tickets outstanding at unmount: %u (settle rc=%d)\n",
            efs_client_put_outstanding(), rc);
}
