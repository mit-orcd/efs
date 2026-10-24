/* K36 / F23 (decision D42): verified recovery after an unclean stop.
 *
 * A crash, SIGKILL, forced exit or power loss leaves every storage root
 * "recovery uncertain" (store.c): writes acknowledged before the last
 * durable barrier may be gone, so COMMIT returns EIO. This thread proves
 * the store again, then clears that fence. In order:
 *   identity   every root's durability record names this boot (previous
 *              boot + 1) under this process's verifier; the member store
 *              advanced the incarnation before anything was served;
 *   journal    the reclaim journal was verified and replayed at startup
 *              (or, when it failed verification, rebuilt by its own
 *              recovery thread) - waited for, never assumed;
 *   ledgers    every storage session certificate (K13 PUT floor) reads
 *              back intact; leftovers of an interrupted seal are removed;
 *   fragments  every local fragment file (chunk and segment) is read whole
 *              and proven: an I25 record naming exactly the object its
 *              name names, and BLAKE3(payload) equal to the record's
 *              digest. A torn, misnamed or mismatching file is dropped;
 *   raft       each metadata group this node hosts answers a ReadIndex from
 *              this node: the replayed log and KV reached the leader's
 *              commit (a replica that missed a committed row is resynced
 *              from the leader's snapshot first);
 *   tickets    every ADMITTED ticket naming this store whose fragment is
 *              not proven here is remembered (its publication may come);
 *   metadata   every published object (chunk base and live span) placing a
 *              fragment here is checked against the published digest. A
 *              missing or mismatching fragment is dropped and its stripe
 *              gets a W154 degraded mark (EFS_MD_CMD_RECOVERY_MARK), so the
 *              group leader rebuilds it from the two peers. Every packed
 *              segment holding switched files is checked the same way; one
 *              not proven is dropped and counted (no segment rebuild);
 * then the fence clears (COMMIT allowed again). The thread goes on to wait
 * for each remembered ticket to leave ADMITTED (a published one is marked
 * as above) and counts the marked fragments W154 rebuilds here.
 *
 * What recovery cannot prove is dropped, never served. It does not prove
 * an earlier ambiguous write: the PUT verifier changed with this boot, so
 * a client holding acknowledgements of the previous boot keeps failing
 * fsync (client data_commit.c), and an error a client saw stays an error.
 * A transient obstacle (no leader yet, a peer unreachable) is retried and
 * shown as the reason; a failed verification leaves the store fenced with
 * that reason until restart. Every step is idempotent, so a crash during
 * recovery leaves the record unclean and the next start recovers again. */
#include "server_internal.h"
#include "efs/frag_record.h"
#include "efs/meta_apply.h"
#include "efs/put_ticket.h"
#include "efs/protocol.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RV_EXPORT 1u
#define RV_RETRY_MS 500u
#define RV_WATCH_MS 2000u

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t tid;
    int started, stop;
    struct efsd_server *s;
    struct efs_recovery_stats st;   /* under mu, except the file counters */
    struct server_recovery_files files;
    uint64_t t0_ms;
    struct efs_meta_degraded *marks; /* marked by this run: rebuild watch */
    uint32_t nmarks, capmarks;
    struct efs_put_ticket *waiting;  /* ADMITTED tickets whose fragment is not here */
    uint32_t nwaiting, capwaiting;
} rv = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static uint64_t rv_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

#if EFS_FAULTS
/* Test-only: EFS_FAULT_CRASH=<point>[:<n>] exits 137, as a SIGKILL would
 * leave the store, at the nth pass of that point (default the first). */
void server_fault_crash(const char *point)
{
    static uint64_t hits;
    const char *v = getenv("EFS_FAULT_CRASH");
    size_t n = strlen(point);
    if (!v || strncmp(v, point, n) || (v[n] && v[n] != ':'))
        return;
    uint64_t want = v[n] == ':' ? strtoull(v + n + 1, NULL, 10) : 1;
    if (__atomic_add_fetch(&hits, 1, __ATOMIC_RELAXED) != (want ? want : 1))
        return;
    fprintf(stderr, "fault: injected crash at %s\n", point);
    fflush(stderr);
    _exit(137);
}

/* Test-only: EFS_FAULT_RECOVERY_HOLD_FILE names a file; while it exists
 * recovery waits just before clearing the fence. */
static int rv_held(void)
{
    const char *v = getenv("EFS_FAULT_RECOVERY_HOLD_FILE");
    return v && *v && access(v, F_OK) == 0;
}
#else
static int rv_held(void) { return 0; }
#endif

static int rv_stopping(void)
{
    return __atomic_load_n(&rv.stop, __ATOMIC_ACQUIRE);
}

/* Sleep up to ms; 1 when the daemon is stopping. */
static int rv_wait(unsigned ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000u;
    until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&rv.mu);
    if (!rv.stop)
        pthread_cond_timedwait(&rv.cv, &rv.mu, &until);
    int stop = rv.stop;
    pthread_mutex_unlock(&rv.mu);
    return stop;
}

static void rv_phase(uint32_t phase)
{
    pthread_mutex_lock(&rv.mu);
    rv.st.phase = phase;
    rv.st.reason[0] = 0;
    pthread_mutex_unlock(&rv.mu);
    fprintf(stderr, "recovery: phase %s\n", efs_recovery_phase_name(phase));
}

__attribute__((format(printf, 1, 2)))
static void rv_reason(const char *fmt, ...)
{
    char buf[sizeof(rv.st.reason)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&rv.mu);
    int changed = strcmp(buf, rv.st.reason) != 0;
    memcpy(rv.st.reason, buf, sizeof(buf));
    pthread_mutex_unlock(&rv.mu);
    if (changed)
        fprintf(stderr, "recovery: waiting: %s\n", buf);
}

#define RV_ADD(field, n) do { pthread_mutex_lock(&rv.mu); rv.st.field += (n); \
                              pthread_mutex_unlock(&rv.mu); } while (0)

/* A peer, leader or election to wait out, as opposed to a failed proof. */
static int rv_transient(int rc)
{
    return rc == EFS_ERR_BUSY || rc == EFS_ERR_NET || rc == EFS_ERR_NOT_PRIMARY ||
           rc == EFS_ERR_UNFORMED || rc == EFS_ERR_AGAIN || rc == EFS_ERR_NO_QUORUM;
}

static int rv_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int rv_fail(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&rv.mu);
    va_start(ap, fmt);
    vsnprintf(rv.st.reason, sizeof(rv.st.reason), fmt, ap);
    va_end(ap);
    if (rv.st.state == EFS_RECOVERY_VERIFIED) {
        /* After the fence: a settle or rebuild step failed; retried. */
        fprintf(stderr, "recovery: after verification: %s; retrying\n", rv.st.reason);
        pthread_mutex_unlock(&rv.mu);
        return -1;
    }
    rv.st.state = EFS_RECOVERY_FAILED;
    rv.st.elapsed_ms = rv_now_ms() - rv.t0_ms;
    fprintf(stderr, "recovery: FAILED in phase %s: %s; the store stays fenced (COMMIT "
            "returns EIO) until a restart verifies it\n",
            efs_recovery_phase_name(rv.st.phase), rv.st.reason);
    pthread_mutex_unlock(&rv.mu);
    return -1;
}

static int rv_grow(void **v, uint32_t n, uint32_t *cap, size_t each)
{
    if (n < *cap)
        return 0;
    uint32_t want = *cap ? *cap * 2 : 256;
    void *p = realloc(*v, (size_t)want * each);
    if (!p)
        return -1;
    *v = p;
    *cap = want;
    return 0;
}

/* ---- identity, journal, ledgers ---- */

static int rv_identity(void)
{
    char why[160];
    int rc = server_put_sync_identity(rv.s, why, sizeof(why));
    if (rc != EFS_OK)
        return rv_fail("boot counter: %s", why);
    if (!rv.s->incarnation)
        return rv_fail("member store holds no incarnation");
    fprintf(stderr, "recovery: identity node=%u incarnation=%llu: every root's record names "
            "this boot\n", rv.s->id, (unsigned long long)rv.s->incarnation);
    return 0;
}

static int rv_journal(void)
{
    for (;;) {
        struct efs_reclaim_status st;
        server_reclaim_status(rv.s, &st);
        if (!st.fail_closed && !st.recovering) {
            fprintf(stderr, "recovery: reclaim journal epoch=%llu verified, records=%llu "
                    "replayed\n", (unsigned long long)st.journal_epoch,
                    (unsigned long long)st.journal_records);
            return 0;
        }
        rv_reason("reclaim journal failed verification; waiting for its rebuild");
        if (rv_wait(RV_RETRY_MS))
            return 1;
    }
}

/* Storage session certificates (put_fence.c): sealed by tmp + fsync +
 * rename, so a crash leaves at most a "<name>.tmp.XXXXXX" behind. */
static int rv_ledgers(void)
{
    char dir[EFS_MAX_PATH + 32];
    uint32_t certs = 0, leftovers = 0;
    snprintf(dir, sizeof(dir), "%s/meta/put-session", rv.s->storage_paths[0]);
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT)
            return 0;
        return rv_fail("cannot read %s: %s", dir, strerror(errno));
    }
    struct dirent *e;
    int bad = 0;
    char badname[96] = "";
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[EFS_MAX_PATH + 320];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (strstr(e->d_name, ".tmp.")) {
            if (unlink(path) && errno != ENOENT) {
                bad = 1;
                snprintf(badname, sizeof(badname), "%.64s", e->d_name);
                break;
            }
            leftovers++;
            continue;
        }
        uint8_t uuid[16];
        uint32_t floor = 0;
        int ok = strlen(e->d_name) == 32;
        for (unsigned i = 0; ok && i < 16; i++) {
            unsigned b;
            ok = sscanf(e->d_name + 2 * i, "%2x", &b) == 1;
            uuid[i] = (uint8_t)b;
        }
        if (!ok || server_put_session_floor(rv.s, uuid, &floor) != EFS_OK) {
            bad = 1;
            snprintf(badname, sizeof(badname), "%.64s", e->d_name);
            break;
        }
        certs++;
    }
    closedir(d);
    if (bad)
        return rv_fail("storage session certificate %s/%s unreadable or damaged", dir, badname);
    fprintf(stderr, "recovery: %u storage session certificates verified, %u interrupted seals "
            "removed\n", certs, leftovers);
    return 0;
}

/* ---- local fragments ---- */

static int rv_fragments(void)
{
    int rc = server_recovery_scan_local(rv.s, &rv.files, &rv.stop);
    if (rc == EFS_ERR_BUSY && rv_stopping())
        return 1;
    if (rc != EFS_OK)
        return rv_fail("local fragment scan: %s", efs_strerror(rc));
    fprintf(stderr, "recovery: fragments checked=%llu dropped=%llu (%llu bytes) foreign=%llu\n",
            (unsigned long long)__atomic_load_n(&rv.files.checked, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&rv.files.dropped, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&rv.files.bytes_dropped, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&rv.files.foreign, __ATOMIC_RELAXED));
    server_fault_crash_point("recovery-local");
    return 0;
}

/* ---- raft ---- */

static int rv_raft(void)
{
    uint32_t proven = 0, groups = 0;
    for (int gi = 0; gi < 2; gi++) {
        uint8_t group;
        int hosted, rc;
        while ((rc = server_raft_host_recovery_group(gi, &group, &hosted)) != EFS_OK) {
            rv_reason("raft host not running");
            if (rv_wait(RV_RETRY_MS))
                return 1;
        }
        groups++;
        if (!hosted)
            continue;
        for (;;) {
            char why[160] = "";
            rc = server_raft_host_recovery_prove(group, why, sizeof(why));
            if (rc == EFS_OK || rc == EFS_ERR_NOT_FOUND)
                break;
            if (!rv_transient(rc))
                return rv_fail("%s", why[0] ? why : efs_strerror(rc));
            rv_reason("%s", why);
            if (rv_wait(RV_RETRY_MS))
                return 1;
        }
        if (rc == EFS_OK) {
            proven++;
            fprintf(stderr, "recovery: group %u replica applied to the leader's commit "
                    "(ReadIndex from this node)\n", group);
        }
    }
    pthread_mutex_lock(&rv.mu);
    rv.st.groups_proven = proven;
    rv.st.groups = groups;
    pthread_mutex_unlock(&rv.mu);
    return 0;
}

/* ---- inventory pages ---- */

typedef int (*rv_rec_fn)(const uint8_t *rec);

/* Every page of one inventory over both groups; transient errors retry the
 * same page. */
static int rv_inventory(uint32_t what, rv_rec_fn fn)
{
    uint8_t *recs = malloc((size_t)EFS_RECOVERY_SCAN_PAGE * EFS_PUT_TICKET_WIRE_LEN);
    uint32_t reclen = what == EFS_RECOVERY_SCAN_CHUNKS ? EFS_REBUILD_MARK_LEN
                    : what == EFS_RECOVERY_SCAN_SEGMENTS ? EFS_RECOVERY_SEGMENT_REC_LEN
                                                         : EFS_PUT_TICKET_WIRE_LEN;
    if (!recs)
        return rv_fail("out of memory");
    for (int gi = 0; gi < 2; gi++) {
        uint8_t group;
        int hosted;
        if (server_raft_host_recovery_group(gi, &group, &hosted) != EFS_OK) {
            free(recs);
            return rv_fail("raft host stopped");
        }
        struct efs_recovery_cursor cur = {0};
        while (cur.shard < EFS_RECOVERY_SHARD_END) {
            struct efs_recovery_cursor next = cur;
            uint32_t n = 0;
            int rc = server_raft_host_recovery_scan(group, what, rv.s->id, &next, recs, &n);
            if (rc != EFS_OK) {
                if (!rv_transient(rc)) {
                    free(recs);
                    return rv_fail("group %u %s inventory: %s", group,
                                   what == EFS_RECOVERY_SCAN_CHUNKS ? "chunk"
                                   : what == EFS_RECOVERY_SCAN_SEGMENTS ? "segment" : "ticket",
                                   efs_strerror(rc));
                }
                rv_reason("group %u inventory: %s", group, efs_strerror(rc));
                if (rv_wait(RV_RETRY_MS)) {
                    free(recs);
                    return 1;
                }
                continue;
            }
            for (uint32_t i = 0; i < n; i++) {
                int r = fn(recs + (size_t)i * reclen);
                if (r) {
                    free(recs);
                    return r;
                }
            }
            cur = next;
            if (rv_stopping()) {
                free(recs);
                return 1;
            }
        }
    }
    free(recs);
    return 0;
}

static int rv_note_mark(const struct efs_meta_degraded *d)
{
    pthread_mutex_lock(&rv.mu);
    int rc = rv_grow((void **)&rv.marks, rv.nmarks, &rv.capmarks, sizeof(*d));
    if (!rc)
        rv.marks[rv.nmarks++] = *d;
    pthread_mutex_unlock(&rv.mu);
    return rc;
}

/* The stripe of a fragment this store does not hold: an identical mark
 * already there, or write one. Retries a transient verdict. */
static int rv_mark(const struct efs_meta_degraded *d)
{
    uint8_t mark[EFS_REBUILD_MARK_LEN];
    server_degraded_mark_encode(mark, d);
    for (;;) {
        int rc = server_raft_host_degraded_check(mark);
        if (rc == EFS_OK) {
            RV_ADD(already_marked, 1);
            return rv_note_mark(d) ? rv_fail("out of memory") : 0;
        }
        if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_STALE)
            rc = server_raft_host_recovery_mark(d);
        if (rc == EFS_OK) {
            RV_ADD(marked, 1);
            server_fault_crash_point("recovery-marked");
            return rv_note_mark(d) ? rv_fail("out of memory") : 0;
        }
        if (rc == EFS_ERR_NOT_FOUND) {
            RV_ADD(superseded, 1);
            return 0;
        }
        if (rc == EFS_ERR_STALE) {
            RV_ADD(unrecoverable, 1);
            fprintf(stderr, "recovery: ino=%llu ci=%u gen=%llx: fragment %d lost here while "
                    "another fragment of the stripe is already marked missing; the 2+1 code "
                    "cannot rebuild it\n", (unsigned long long)d->ino, d->chunk_index,
                    (unsigned long long)d->object_gen, __builtin_ctz(d->missing_mask));
            return 0;
        }
        if (!rv_transient(rc))
            return rv_fail("degraded mark ino=%llu ci=%u: %s", (unsigned long long)d->ino,
                           d->chunk_index, efs_strerror(rc));
        rv_reason("degraded mark ino=%llu ci=%u: %s", (unsigned long long)d->ino,
                  d->chunk_index, efs_strerror(rc));
        if (rv_wait(RV_RETRY_MS))
            return 1;
    }
}

/* Is this store's fragment of the object proven here (dropping what is
 * not)? 1 yes, 0 no, -1 failed (reported). */
static int rv_have(const struct efs_meta_degraded *d)
{
    int fi = __builtin_ctz(d->missing_mask);
    struct server_recovery_files c = {0}; /* the local scan already counted these files */
    int rc = server_recovery_fragment_check(rv.s, RV_EXPORT, d->ino, d->chunk_index, (uint32_t)fi,
                                            d->object_gen, d->checksums[fi], &c);
    __atomic_fetch_add(&rv.files.dropped, c.dropped, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rv.files.bytes_dropped, c.bytes_dropped, __ATOMIC_RELAXED);
    if (rc == EFS_OK)
        return 1;
    if (rc == EFS_ERR_NOT_FOUND)
        return 0;
    rv_fail("fragment ino=%llu ci=%u fi=%d gen=%llx: %s", (unsigned long long)d->ino,
            d->chunk_index, fi, (unsigned long long)d->object_gen, efs_strerror(rc));
    return -1;
}

static int rv_chunk_ref(const uint8_t *rec)
{
    struct efs_meta_degraded d;
    if (server_degraded_mark_decode(rec, &d) != EFS_OK ||
        d.nodes[__builtin_ctz(d.missing_mask)] != rv.s->id)
        return rv_fail("malformed inventory record");
    RV_ADD(refs_checked, 1);
    int have = rv_have(&d);
    if (have < 0)
        return -1;
    if (have)
        return 0;
    RV_ADD(refs_missing, 1);
    return rv_mark(&d);
}

static uint32_t rv_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A packed segment fragment the metadata places here (the object holds
 * switched files, so the copy was durable before the switch). One that is
 * not proven is dropped and counted: the two peers still serve it, and
 * W154 has no segment rebuild. */
static int rv_segment_ref(const uint8_t *rec)
{
    uint32_t group = rv_rd32(rec), fi = rv_rd32(rec + 20);
    uint64_t id = ((uint64_t)rv_rd32(rec + 4) << 32) | rv_rd32(rec + 8);
    uint64_t gen = ((uint64_t)rv_rd32(rec + 12) << 32) | rv_rd32(rec + 16);
    struct server_recovery_files c = {0};
    if (fi >= EFS_NUM_FRAGMENTS || !id || !gen)
        return rv_fail("malformed segment inventory record");
    RV_ADD(segments_checked, 1);
    int rc = server_segment_recovery_check(rv.s, group, id, gen, fi, rec + 28, &c);
    __atomic_fetch_add(&rv.files.dropped, c.dropped, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rv.files.bytes_dropped, c.bytes_dropped, __ATOMIC_RELAXED);
    if (rc == EFS_OK)
        return 0;
    if (rc != EFS_ERR_NOT_FOUND)
        return rv_fail("segment g%u id=%016llx gen=%llx fragment %u: %s", group,
                       (unsigned long long)id, (unsigned long long)gen, fi, efs_strerror(rc));
    RV_ADD(segments_missing, 1);
    fprintf(stderr, "recovery: segment g%u id=%016llx gen=%llx fragment %u not proven here; "
            "its two peers serve it, no rebuild path for segments\n", group,
            (unsigned long long)id, (unsigned long long)gen, fi);
    return 0;
}

static void rv_ticket_mark(const struct efs_put_ticket *t, struct efs_meta_degraded *d)
{
    memset(d, 0, sizeof(*d));
    d->ino = t->ino;
    d->inode_gen = t->inode_gen;
    d->object_gen = t->object;
    d->chunk_index = t->ci;
    d->lane = (uint8_t)(t->ci % EFS_META_LANES);
    memcpy(d->nodes, t->nodes, sizeof(d->nodes));
    memcpy(d->checksums, t->checksums, sizeof(d->checksums));
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        if (t->nodes[i] == rv.s->id)
            d->missing_mask = (uint8_t)(1u << i);
}

static int rv_ticket(const uint8_t *rec)
{
    struct efs_put_ticket t;
    struct efs_meta_degraded d;
    if (efs_put_ticket_decode(rec, EFS_PUT_TICKET_WIRE_LEN, &t) != EFS_OK)
        return rv_fail("malformed ticket record");
    rv_ticket_mark(&t, &d);
    if (!d.missing_mask)
        return rv_fail("ticket record names another store");
    RV_ADD(tickets_checked, 1);
    int have = rv_have(&d);
    if (have < 0)
        return -1;
    if (have)
        return 0;
    pthread_mutex_lock(&rv.mu);
    int rc = rv_grow((void **)&rv.waiting, rv.nwaiting, &rv.capwaiting, sizeof(t));
    if (!rc)
        rv.waiting[rv.nwaiting++] = t;
    rv.st.tickets_waiting = rv.nwaiting;
    pthread_mutex_unlock(&rv.mu);
    return rc ? rv_fail("out of memory") : 0;
}

/* The ADMITTED tickets still naming this store, as a sorted key set. */
static struct efs_put_ticket *rv_live;
static uint32_t rv_nlive, rv_caplive;
static int rv_live_add(const uint8_t *rec)
{
    struct efs_put_ticket t;
    if (efs_put_ticket_decode(rec, EFS_PUT_TICKET_WIRE_LEN, &t) != EFS_OK)
        return rv_fail("malformed ticket record");
    if (rv_grow((void **)&rv_live, rv_nlive, &rv_caplive, sizeof(t)))
        return rv_fail("out of memory");
    rv_live[rv_nlive++] = t;
    return 0;
}

static int rv_same_ticket(const struct efs_put_ticket *a, const struct efs_put_ticket *b)
{
    return !memcmp(a->id.client_uuid, b->id.client_uuid, sizeof(a->id.client_uuid)) &&
           a->id.session_epoch == b->id.session_epoch && a->id.seq == b->id.seq &&
           a->ino == b->ino && a->ci == b->ci && a->object == b->object;
}

/* One pass over the remembered tickets: one that left ADMITTED (published,
 * rejected or drained) or whose fragment arrived is done; a published
 * object gets its mark. Runs after the fence cleared: no promise depends
 * on it (the PUT verifier changed, so the client that held the old boot's
 * acknowledgement fails its fsync), only the stripe's redundancy does, and
 * a crash before it finishes is caught by the next recovery's inventory. */
static int rv_settle_pass(void)
{
    rv_nlive = 0;
    int rc = rv_inventory(EFS_RECOVERY_SCAN_TICKETS, rv_live_add);
    if (rc)
        return rc;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < rv.nwaiting; i++) {
        struct efs_put_ticket t = rv.waiting[i];
        struct efs_meta_degraded d;
        int live = 0;
        for (uint32_t k = 0; k < rv_nlive && !live; k++)
            live = rv_same_ticket(&t, &rv_live[k]);
        rv_ticket_mark(&t, &d);
        int have = rv_have(&d);
        if (have < 0 || (!have && !live && (rc = rv_mark(&d)) != 0)) {
            /* Keep this and the unvisited ones for the next pass. */
            for (uint32_t j = i; j < rv.nwaiting; j++)
                rv.waiting[keep++] = rv.waiting[j];
            break;
        }
        if (!have && live)
            rv.waiting[keep++] = t;
    }
    pthread_mutex_lock(&rv.mu);
    rv.nwaiting = keep;
    rv.st.tickets_waiting = keep;
    pthread_mutex_unlock(&rv.mu);
    return rc;
}

/* One pass over the marks this run wrote or found: a retired one was
 * rebuilt here (a proven copy) or superseded. */
static void rv_watch_pass(void)
{
    uint32_t keep = 0, n = rv.nmarks;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t mark[EFS_REBUILD_MARK_LEN];
        struct efs_meta_degraded d = rv.marks[i];
        server_degraded_mark_encode(mark, &d);
        int rc = rv_stopping() ? EFS_ERR_BUSY : server_raft_host_degraded_check(mark);
        if (rc == EFS_OK || rv_transient(rc)) {
            rv.marks[keep++] = d;
            continue;
        }
        struct server_recovery_files ignore = {0};
        int fi = __builtin_ctz(d.missing_mask);
        if (server_recovery_fragment_check(rv.s, RV_EXPORT, d.ino, d.chunk_index, (uint32_t)fi,
                                           d.object_gen, d.checksums[fi], &ignore) == EFS_OK)
            RV_ADD(rebuilt, 1);
        else
            RV_ADD(superseded, 1);
    }
    pthread_mutex_lock(&rv.mu);
    memmove(rv.marks + keep, rv.marks + n, (size_t)(rv.nmarks - n) * sizeof(*rv.marks));
    rv.nmarks = keep + (rv.nmarks - n);
    rv.st.rebuild_waiting = rv.nmarks;
    pthread_mutex_unlock(&rv.mu);
}

/* After the fence: settle the remembered tickets and count what W154
 * rebuilt here, until both are done or the daemon stops. */
static void rv_after(void)
{
    rv_phase(EFS_RECOVERY_PHASE_REBUILD);
    int settled = !rv.nwaiting;
    while (!rv_stopping()) {
        if (rv.nwaiting && rv_settle_pass() == 0 && !rv.nwaiting) {
            settled = 1;
            fprintf(stderr, "recovery: every remembered ticket settled\n");
        }
        rv_watch_pass();
        if (settled && !rv.nmarks)
            break;
        if (rv.nwaiting)
            rv_reason("%u ADMITTED tickets name a fragment this store does not hold",
                      rv.nwaiting);
        rv_wait(RV_WATCH_MS);
    }
    if (rv_stopping())
        return;
    pthread_mutex_lock(&rv.mu);
    rv.st.phase = 0;
    rv.st.reason[0] = 0;
    fprintf(stderr, "recovery: rebuild complete: rebuilt=%llu superseded=%llu\n",
            (unsigned long long)rv.st.rebuilt, (unsigned long long)rv.st.superseded);
    pthread_mutex_unlock(&rv.mu);
}

static void *rv_main(void *arg)
{
    (void)arg;
    int r = 0;
    rv_phase(EFS_RECOVERY_PHASE_IDENTITY);
    r = rv_identity();
    if (!r) { rv_phase(EFS_RECOVERY_PHASE_JOURNAL); r = rv_journal(); }
    if (!r) { rv_phase(EFS_RECOVERY_PHASE_LEDGERS); r = rv_ledgers(); }
    if (!r) { rv_phase(EFS_RECOVERY_PHASE_FRAGMENTS); r = rv_fragments(); }
    if (!r) { rv_phase(EFS_RECOVERY_PHASE_RAFT); r = rv_raft(); }
    /* Tickets before the published objects: one published after its
     * ticket was read is seen by the chunk inventory, one still ADMITTED
     * there is remembered and settled after the fence (rv_after). */
    if (!r) { rv_phase(EFS_RECOVERY_PHASE_TICKETS); r = rv_inventory(EFS_RECOVERY_SCAN_TICKETS, rv_ticket); }
    if (!r) { rv_phase(EFS_RECOVERY_PHASE_METADATA); r = rv_inventory(EFS_RECOVERY_SCAN_CHUNKS, rv_chunk_ref); }
    if (!r) r = rv_inventory(EFS_RECOVERY_SCAN_SEGMENTS, rv_segment_ref);
    if (!r) {
        rv_phase(EFS_RECOVERY_PHASE_FENCE);
        while (rv_held() && !rv_stopping()) {
            rv_reason("held by EFS_FAULT_RECOVERY_HOLD_FILE");
            rv_wait(200);
        }
        r = rv_stopping();
    }
    if (!r) {
        server_put_sync_recovered();
        pthread_mutex_lock(&rv.mu);
        rv.st.state = EFS_RECOVERY_VERIFIED;
        rv.st.elapsed_ms = rv_now_ms() - rv.t0_ms;
        rv.st.reason[0] = 0;
        struct efs_recovery_stats st = rv.st;
        pthread_mutex_unlock(&rv.mu);
        fprintf(stderr, "recovery: verified in %llu ms; fence cleared, COMMIT allowed: "
                "files checked=%llu dropped=%llu refs checked=%llu missing=%llu marked=%llu "
                "already_marked=%llu superseded=%llu unrecoverable=%llu tickets checked=%llu "
                "segments checked=%llu missing=%llu groups proven=%u/%u\n",
                (unsigned long long)st.elapsed_ms,
                (unsigned long long)__atomic_load_n(&rv.files.checked, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&rv.files.dropped, __ATOMIC_RELAXED),
                (unsigned long long)st.refs_checked, (unsigned long long)st.refs_missing,
                (unsigned long long)st.marked, (unsigned long long)st.already_marked,
                (unsigned long long)st.superseded, (unsigned long long)st.unrecoverable,
                (unsigned long long)st.tickets_checked, (unsigned long long)st.segments_checked,
                (unsigned long long)st.segments_missing, st.groups_proven, st.groups);
        rv_after();
    } else if (r > 0) {
        fprintf(stderr, "recovery: interrupted by stop in phase %s; the store stays fenced\n",
                efs_recovery_phase_name(rv.st.phase));
    }
    return NULL;
}

int server_recovery_start(struct efsd_server *s)
{
    if (!s || rv.started || !server_put_sync_uncertain())
        return EFS_OK;
    rv.s = s;
    rv.t0_ms = rv_now_ms();
    pthread_mutex_lock(&rv.mu);
    rv.st.state = EFS_RECOVERY_RUNNING;
    rv.st.incarnation = s->incarnation;
    pthread_mutex_unlock(&rv.mu);
    fprintf(stderr, "recovery: started after an unclean stop; COMMIT returns EIO until the "
            "store is verified\n");
    if (efsd_pthread_create(&rv.tid, rv_main, NULL) != 0) {
        rv_fail("cannot start the recovery thread");
        return EFS_ERR_NOMEM;
    }
    rv.started = 1;
    return EFS_OK;
}

void server_recovery_stop(void)
{
    pthread_mutex_lock(&rv.mu);
    rv.stop = 1;
    pthread_cond_broadcast(&rv.cv);
    pthread_mutex_unlock(&rv.mu);
    if (rv.started)
        pthread_join(rv.tid, NULL);
    rv.started = 0;
}

void server_recovery_stats(struct efs_recovery_stats *out)
{
    pthread_mutex_lock(&rv.mu);
    *out = rv.st;
    if (rv.st.state == EFS_RECOVERY_RUNNING)
        out->elapsed_ms = rv_now_ms() - rv.t0_ms;
    pthread_mutex_unlock(&rv.mu);
    out->files_checked = __atomic_load_n(&rv.files.checked, __ATOMIC_RELAXED);
    out->files_dropped = __atomic_load_n(&rv.files.dropped, __ATOMIC_RELAXED);
    out->bytes_dropped = __atomic_load_n(&rv.files.bytes_dropped, __ATOMIC_RELAXED);
    out->files_foreign = __atomic_load_n(&rv.files.foreign, __ATOMIC_RELAXED);
}
