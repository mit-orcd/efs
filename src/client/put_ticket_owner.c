#include "client_internal.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
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
    int result = EFS_OK;
    for (uint32_t k = 0; k < nb; k++) {
        struct efs_put_ticket t[EFS_PUT_TICKET_SETTLE_MAX];
        uint32_t n = 0, retired = 0;
        pthread_mutex_lock(&g_ledger.mu);
        for (uint32_t j = 0; j < g_pass[k].n; j++) {
            struct ledger_entry *e = ledger_find(g_pass[k].seq[j]);
            if (!e) break;
            t[n++] = e->ticket;
        }
        pthread_mutex_unlock(&g_ledger.mu);
        if (!n) continue;
        int rc = efs_client_rpc_put_ticket_settle(t, n, &retired);
        if (ticket_debug())
            fprintf(stderr, "put-ticket settle shard=%u n=%u first=%llu retired=%u rc=%d\n",
                    g_pass[k].shard, n, (unsigned long long)t[0].id.seq, retired, rc);
        if (rc == EFS_OK && retired) {
            pthread_mutex_lock(&g_ledger.mu);
            for (uint32_t j = 0; j < retired; j++) {
                struct ledger_entry *gone = ledger_find(t[j].id.seq);
                if (!gone) continue;
                uint32_t at = (uint32_t)(gone - g_ledger.e);
                memmove(gone, gone + 1, (size_t)(g_ledger.n - at - 1) * sizeof(*gone));
                g_ledger.n--;
            }
            pthread_mutex_unlock(&g_ledger.mu);
            *progress = 1;
        }
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
    for (unsigned round = 0; progress && round < 64 && rc == EFS_OK; round++)
        rc = settle_pass(&progress);
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
