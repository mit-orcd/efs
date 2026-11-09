/* Task EB: the open-reference table through its owner interface (open.h).
 *
 * Links the real src/client/efs_fuse/open.c and nothing else of the
 * client. The lease edge RPCs, the FileID fetch and the helpers open.c
 * takes from other files (support, getattr, release, the .efs command
 * surface, the RPC deadline) are link-time definitions here; the lease
 * stub answers each OPEN, CLOSE or CLEAR edge with a chosen verdict.
 *
 * Cases: an inode without a row; two opens and two closes (one HOLD, one
 * CLOSE, the row gone); a last close whose CLOSE edge fails: the row is
 * retained, counts as open and as retained, a reopen fails with the edge's
 * error, the retry pass drains it once the edge answers; the append burst
 * and handoff bookkeeping on a real row and on no row; 3,000 retained rows
 * across colliding hash buckets drained by the retry pass; four threads
 * opening and closing distinct inodes while another counts (TSan entry).
 * Task ER: the open.c edges of six former splice tests, one function per
 * case named after the test it replaces (open_lease_*, open_busy_*,
 * open_retry_lock_budget, fuse_lease_retention_*, append_burst_clock). */
#include "efs_fuse/open.h"
#include "efs_fuse/support.h"
#include "efs_fuse/find_res.h"
#include "efs_fuse/getattr.h"
#include "efs_fuse/release.h"
#include "efs_fuse/writeback.h"
#include "commands/source.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

static __thread uint64_t t_deadline;
/* The lease stub's verdicts: the next OPEN, CLOSE and CLEAR answers; a CLOSE
 * answer for one inode; begins refused for admission (flags filter, 0 any);
 * every CLOSE begin of one inode refused; OPEN answers left unknown (BUSY, N
 * times or -1 forever); non-OK answers as completed verdicts; STALE as a
 * completed revocation; the combined OPEN|CLEAR edge refused (1) or unknown
 * first and refused later (2). */
static int open_rc, close_rc, adopt_rc, close_ino_rc, open_busy, known, revocation, combined_stale;
static efs_ino_t close_ino, busy_close_ino, watch_ino;
static unsigned begin_busy, busy_flags, block_flags;
/* Wire sends per kind (first attempt of an edge), begins, resumes, fetches. */
static unsigned opens, closes, adopts, begins_open, begins_close, begins_clear, resumes, fetches, waits, seq;
static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int entered, unblock;

uint64_t stats_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
uint64_t efs_client_rpc_deadline_ms(void) { return t_deadline; }
void efs_client_rpc_set_deadline_ms(uint64_t ms) { t_deadline = ms; }
int efs_client_rpc_past_deadline(void)
{
    return t_deadline && stats_now_ms() >= t_deadline;
}
/* open.c's admission wait: never with the watched inode's stripe held. */
void efs_client_opid_wait(unsigned max_ms)
{
    if (watch_ino) {
        pthread_mutex_t *edge = open_edge_mu(watch_ino);
        assert(!pthread_mutex_trylock(edge));
        pthread_mutex_unlock(edge);
        waits++;
    }
    usleep(max_ms * 1000u);
}

int efs_client_lease_begin(struct efs_client_lease_request *r, efs_ino_t ino,
                           uint64_t gen, unsigned flags, uint64_t token)
{
    assert(!r->active && gen);
    assert(!!(flags & EFS_LEASE_EDGE_CLEAR_PENDING) == !!token);
    pthread_mutex_lock(&stub_mu);
    if (flags & EFS_LEASE_EDGE_OPEN) begins_open++;
    else if (flags & EFS_LEASE_EDGE_CLEAR_PENDING) begins_clear++;
    else begins_close++;
    int busy = (begin_busy && (!busy_flags || flags == busy_flags)) ||
               (flags == EFS_LEASE_EDGE_CLOSE && ino == busy_close_ino);
    if (begin_busy && busy && (!busy_flags || flags == busy_flags))
        begin_busy--;
    unsigned id = ++seq;
    pthread_mutex_unlock(&stub_mu);
    if (busy)
        return EFS_ERR_BUSY;
    *r = (struct efs_client_lease_request){.ino = ino, .generation = gen,
                                           .pending_token = token, .flags = flags, .active = 1};
    r->q.id.seq = id;
    return EFS_OK;
}
int efs_client_lease_resume(struct efs_client_lease_request *r, int probe)
{
    assert(!probe && r->active);
    if (r->complete)
        return r->result;
    if (block_flags && r->flags == block_flags) {
        pthread_mutex_lock(&gate_mu);
        entered = 1;
        pthread_cond_broadcast(&gate_cv);
        while (!unblock)
            pthread_cond_wait(&gate_cv, &gate_mu);
        pthread_mutex_unlock(&gate_mu);
    }
    int open = !!(r->flags & EFS_LEASE_EDGE_OPEN);
    int clear = !!(r->flags & EFS_LEASE_EDGE_CLEAR_PENDING);
    pthread_mutex_lock(&stub_mu);
    resumes++;
    if (!r->attempted) {
        if (open) opens++;
        if (clear) adopts++;
        if (!open && !clear) closes++;
    }
    int rc;
    if (open && clear && combined_stale) {
        rc = EFS_ERR_STALE;
        if (combined_stale == 2 && !r->attempted)
            rc = EFS_ERR_NET;
        else
            r->complete = 1, r->result = rc;
    } else if (open && open_busy) {
        if (open_busy > 0) open_busy--;
        rc = EFS_ERR_BUSY;
    } else if (open) {
        rc = open_rc;
    } else if (clear) {
        rc = adopt_rc;
    } else {
        rc = r->ino == close_ino ? close_ino_rc : close_rc;
    }
    if (rc != EFS_OK && known)
        r->complete = 1, r->result = rc;
    if (rc == EFS_ERR_STALE && revocation)
        r->revoked = 1, r->complete = 1, r->result = rc;
    pthread_mutex_unlock(&stub_mu);
    r->attempted = 1;
    if (rc == EFS_OK) { r->complete = 1; r->result = rc; }
    return rc;
}
int efs_client_lease_reset(struct efs_client_lease_request *r)
{
    assert(!r->active || r->complete);
    memset(r, 0, sizeof(*r));
    return EFS_OK;
}
int efs_client_create_cancel(struct efs_client_create_request *r) { (void)r; return EFS_OK; }
int efs_client_create_reset(struct efs_client_create_request *r) { (void)r; return EFS_OK; }
int efs_client_create_resume(struct efs_client_create_request *r, int probe) { (void)r; (void)probe; return EFS_OK; }
int efs_client_rpc_getchunks_fileid(efs_export_id_t ex, efs_ino_t ino, uint64_t gen, uint32_t start,
                                    struct efs_chunk_rec *recs, uint32_t *count, uint64_t *out)
{
    (void)ex; (void)ino; (void)gen; (void)start; (void)recs; *count = 0; *out = 99;
    pthread_mutex_lock(&stub_mu);
    fetches++;
    pthread_mutex_unlock(&stub_mu);
    return EFS_OK;
}
uint64_t efs_client_map_clock(void) { return 1; }
void efs_client_map_install(efs_ino_t i, const struct efs_chunk_rec *r, uint32_t n, uint64_t u) { (void)i; (void)r; (void)n; (void)u; }
void efs_client_prefetch_open(efs_ino_t i, uint64_t s) { (void)i; (void)s; }
int efs_client_read(efs_ino_t ino, uint64_t off, size_t size, char *buf, size_t *out) { (void)ino; (void)off; (void)size; (void)buf; *out = 0; return EFS_OK; }
void efs_client_stage_pin(efs_ino_t ino) { (void)ino; }
void efs_client_stage_unpin(efs_ino_t ino) { (void)ino; }
int efs_client_stat_refresh(efs_ino_t ino, struct efs_inode *out) { (void)ino; (void)out; return EFS_OK; }
struct efs_client g_client;
const struct fuse_ctx *ll_ctx(void) { return NULL; }
int check_access(const struct efs_inode *ino, uid_t uid, gid_t gid, int mask) { (void)ino; (void)uid; (void)gid; (void)mask; return 0; }
int command_script(const char *term) { (void)term; return 0; }
int commands_enabled(void) { return 0; }
void efs_file_close(struct fuse_file_info *fi) { fi->fh = 0; }
struct efs_file_handle *efs_file_handle(const struct fuse_file_info *fi) { (void)fi; return NULL; }
efs_ino_t efs_file_ino(const struct fuse_file_info *fi, efs_ino_t fallback) { return fi && fi->fh ? (efs_ino_t)fi->fh : fallback; }
int efs_file_open(struct fuse_file_info *fi, efs_ino_t ino) { fi->fh = ino; return 0; }
void efs_fuse_log_err(const char *w, int rc, efs_ino_t ino, uint64_t off, size_t size, const char *p) { (void)w; (void)rc; (void)ino; (void)off; (void)size; (void)p; }
int efs_fuse_truncate_ino(fuse_ino_t ino, off_t size, struct fuse_file_info *fi) { (void)ino; (void)size; (void)fi; return 0; }
int efs_rc_to_errno(int rc) { return rc ? EIO : 0; }
int feature_enabled(uint32_t bit) { (void)bit; return 0; }
int find_res_open(struct fuse_file_info *fi, fuse_ino_t ino, efs_ino_t d, const char *q, const char *p) { (void)fi; (void)ino; (void)d; (void)q; (void)p; return 0; }
int find_query_read(efs_ino_t d, const char *q, const char *p, char *b, size_t s, off_t o) { (void)d; (void)q; (void)p; (void)b; (void)s; (void)o; return 0; }
int fuse_stat_errno(int rc) { return rc ? EIO : 0; }
int stats_snapshot(efs_ino_t d, struct stats_ent *out) { (void)d; (void)out; return 0; }
int virt_kind(fuse_ino_t ino) { (void)ino; return 0; }
efs_ino_t virt_parent(fuse_ino_t ino) { return (efs_ino_t)ino; }
int virtual_access(efs_ino_t ino) { (void)ino; return 0; }
int virtual_file_open(struct fuse_file_info *fi, efs_ino_t ino, const char *t, size_t l) { (void)fi; (void)ino; (void)t; (void)l; return 0; }
int vq_lookup(fuse_ino_t n, efs_ino_t *d, char *t, size_t c) { (void)n; (void)d; (void)t; (void)c; return 0; }
int vq_version_matches(efs_ino_t ino, const char *t, size_t l) { (void)ino; (void)t; (void)l; return 0; }

/* release.c's last-close path, as open.c's CREATE failure path calls it. */
int efs_fuse_release_ino(fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)fi;
    int leased = 0, rc = 0;
    pthread_mutex_t *edge = open_edge_mu(ino);
    pthread_mutex_lock(edge);
    if (efs_close_note(ino, &leased) && leased)
        rc = efs_open_release_pending(ino);
    pthread_mutex_unlock(edge);
    return rc;
}

/* The two table operations that need the inode stripe, taken here. */
static int release_pending(efs_ino_t ino)
{
    pthread_mutex_t *edge = open_edge_mu(ino);
    pthread_mutex_lock(edge);
    int rc = efs_open_release_pending(ino);
    pthread_mutex_unlock(edge);
    return rc;
}
static int last_close(efs_ino_t ino)
{
    int leased = -1;
    int last = efs_close_note(ino, &leased);
    assert(!last || leased == 1);
    return last;
}

/* Task ER cases. Each starts from no retained row and default verdicts. */
static void reset_stub(void)
{
    open_rc = close_rc = adopt_rc = close_ino_rc = open_busy = known = revocation = combined_stale = 0;
    close_ino = busy_close_ino = watch_ino = 0;
    begin_busy = busy_flags = block_flags = 0;
    entered = unblock = 0;
    assert(!efs_open_retained() && !t_deadline);
}
static void drop(efs_ino_t ino) /* the last close, proven */
{
    assert(last_close(ino) && release_pending(ino) == EFS_OK && !efs_client_ino_is_open(ino));
}
static void *opener42(void *arg)
{
    (void)arg;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    return NULL;
}
static void *closer42(void *arg)
{
    (void)arg;
    struct fuse_file_info fi = {0};
    assert(efs_fuse_release_ino(42, &fi) == EFS_OK);
    return NULL;
}
static void gate_wait(void)
{
    pthread_mutex_lock(&gate_mu);
    while (!entered)
        pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}
static void gate_open(void)
{
    pthread_mutex_lock(&gate_mu);
    unblock = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

/* From test_open_lease.py: shared opens take one HOLD and one FileID fetch (none
 * when the caller has the generation); a failed HOLD retains the row (count
 * 0, close pending) until its edge is proven. */
static void open_lease_shared_and_failed_hold(void)
{
    reset_stub();
    unsigned o = opens, f = fetches;
    assert(efs_open_acquire_fileid(42, 0) == EFS_OK && efs_open_acquire_fileid(42, 0) == EFS_OK);
    assert(opens == o + 1 && fetches == f + 1);
    assert(!last_close(42) && efs_client_ino_is_open(42));
    drop(42);
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && fetches == f + 1);
    drop(42);
    open_rc = EFS_ERR_IO;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_IO);
    assert(efs_client_ino_is_open(42) && efs_open_retained() == 1);
    open_rc = EFS_OK;
    assert(release_pending(42) == EFS_OK && !efs_client_ino_is_open(42));
}

/* From test_open_lease.py: the inode stripe serializes the lease edges. A second
 * open waits while the first is in its OPEN RPC (one HOLD); an open racing a
 * last close in its CLOSE RPC waits for it, then takes a fresh HOLD. */
static void open_lease_edge_serialization(void)
{
    reset_stub();
    pthread_t a, b;
    unsigned o = opens, c = closes;
    block_flags = EFS_LEASE_EDGE_OPEN;
    assert(!pthread_create(&a, NULL, opener42, NULL));
    gate_wait();
    assert(pthread_mutex_trylock(open_edge_mu(42)) != 0);
    assert(!pthread_create(&b, NULL, opener42, NULL));
    gate_open();
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    assert(opens == o + 1 && !last_close(42));
    entered = unblock = 0;
    block_flags = EFS_LEASE_EDGE_CLOSE;
    assert(!pthread_create(&a, NULL, closer42, NULL));
    gate_wait();
    assert(pthread_mutex_trylock(open_edge_mu(42)) != 0);
    assert(!pthread_create(&b, NULL, opener42, NULL));
    gate_open();
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    assert(closes == c + 1 && opens == o + 2);
    block_flags = 0;
    drop(42);
}

/* From test_open_lease.py: 20,000 rows across colliding hash buckets, every one
 * found while open and gone after its last close. */
static void open_lease_hashed_rows(void)
{
    reset_stub();
    for (efs_ino_t i = 1; i <= 20000; i++)
        assert(efs_open_acquire_fileid(i * 4096, 99) == EFS_OK);
    for (efs_ino_t i = 1; i <= 20000; i++)
        assert(efs_client_ino_is_open(i * 4096));
    for (efs_ino_t i = 1; i <= 20000; i++)
        drop(i * 4096);
    assert(!efs_open_retained());
}

/* Was test_open_busy.py: a refused admission (no identity taken) is retried
 * outside the stripe within the caller's budget, for the OPEN and for the
 * pending CLOSE a reopen must prove first. */
static void open_busy_admission(void)
{
    reset_stub();
    int leased = 0;
    unsigned b = begins_open, c;
    watch_ino = 42;
    waits = 0;
    begin_busy = 3;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    assert(begins_open == b + 4 && waits == 3 && !begin_busy && !t_deadline);
    assert(efs_close_note(42, &leased) && leased && efs_open_retained() == 1);
    b = begins_open;
    c = begins_close;
    waits = 0;
    begin_busy = 3;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    assert(begins_close == c + 4 && begins_open == b + 1 && waits == 3 && !efs_open_retained());
    watch_ino = 0;
    drop(42);
}

/* Was test_open_busy.py: an unknown OPEN answer is resumed on the same identity
 * until proven; a completed rejection is final (one identity, no row); an
 * I/O failure retains the unknown OPEN for its compensating CLOSE. */
static void open_busy_verdicts(void)
{
    reset_stub();
    unsigned b = begins_open, r = resumes;
    open_busy = 2;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    assert(begins_open == b + 1 && resumes == r + 3 && !t_deadline);
    drop(42);
    b = begins_open;
    open_busy = 1;
    known = 1;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_BUSY);
    assert(begins_open == b + 1 && !efs_client_ino_is_open(42) && !t_deadline);
    known = 0;
    b = begins_open;
    open_rc = EFS_ERR_IO;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_IO);
    assert(begins_open == b + 1 && efs_open_retained() == 1);
    open_rc = EFS_OK;
    assert(release_pending(42) == EFS_OK && begins_open == b + 1 && !efs_client_ino_is_open(42));
}

/* Was test_open_busy.py: the caller's deadline bounds the whole open. An unknown
 * OPEN past it is retained on its exact identity (no new OPEN proves it) and
 * the inherited deadline is kept; a contender on a held stripe gives up at
 * the deadline without an identity or a row. */
static void open_busy_deadline(void)
{
    reset_stub();
    unsigned b = begins_open;
    uint64_t d = stats_now_ms() + 20;
    open_busy = -1;
    t_deadline = d;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_BUSY && t_deadline == d);
    assert(begins_open == b + 1 && efs_open_retained() == 1);
    open_busy = 0;
    t_deadline = 0;
    assert(release_pending(42) == EFS_OK && begins_open == b + 1 && !efs_client_ino_is_open(42));
    pthread_mutex_t *edge = open_edge_mu(42);
    pthread_mutex_lock(edge);
    d = stats_now_ms() + 20;
    t_deadline = d;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_BUSY && t_deadline == d);
    assert(begins_open == b + 1 && !efs_client_ino_is_open(42));
    pthread_mutex_unlock(edge);
    t_deadline = 0;
}

/* Was test_open_retry_lock_budget.py: the retry pass takes an inode stripe only
 * within its budget. A held stripe makes the pass answer BUSY without
 * touching the edge or losing the row, which the next pass drains; a
 * retained CLOSE without an identity ends the lease half of the pass after
 * one attempt instead of rescanning it. */
static void open_retry_lock_budget(void)
{
    reset_stub();
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    close_rc = EFS_ERR_NET;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET);
    close_rc = EFS_OK;
    pthread_mutex_t *edge = open_edge_mu(42);
    pthread_mutex_lock(edge);
    unsigned r = resumes, c;
    assert(efs_open_retry_pass(stats_now_ms() + 40) == EFS_ERR_BUSY);
    assert(resumes == r && efs_open_retained() == 1);
    pthread_mutex_unlock(edge);
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_OK && !efs_client_ino_is_open(42));
    busy_close_ino = 42;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    assert(last_close(42) && release_pending(42) == EFS_ERR_BUSY && efs_open_close_unadmitted(42));
    c = begins_close;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_ERR_BUSY && begins_close == c + 1);
    busy_close_ino = 0;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_OK && !efs_client_ino_is_open(42));
}

/* From test_fuse_lease_retention.py: ambiguous CLOSE verdicts retain the row; a
 * reopen proves the retained CLOSE (or fails with its error) before a fresh
 * HOLD; an unknown OPEN with its CLOSE behind it is resumed, never re-sent;
 * a negative CLOSE verdict keeps the row until proven. */
static void fuse_lease_retention_close_verdicts(void)
{
    reset_stub();
    unsigned o = opens;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    close_rc = EFS_ERR_NET;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET && efs_open_retained() == 1);
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_NET && opens == o + 1);
    close_rc = EFS_OK;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && opens == o + 2 && !efs_open_retained());
    drop(42);
    open_rc = close_rc = EFS_ERR_NET;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_NET && efs_open_retained() == 1);
    o = opens;
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_NET && opens == o);
    open_rc = close_rc = EFS_OK;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_OK && !efs_client_ino_is_open(42));
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && last_close(42));
    close_rc = EFS_ERR_NOT_FOUND;
    assert(release_pending(42) == EFS_ERR_NOT_FOUND && efs_open_retained() == 1);
    close_rc = EFS_OK;
    assert(release_pending(42) == EFS_OK && !efs_client_ino_is_open(42));
}

/* From test_fuse_lease_retention.py: a retained CLOSE that holds an identity (its
 * CLOSE edge, or the CREATE adoption before it) is retried before a newer
 * retained CLOSE still waiting for admission, so the identity window cannot
 * deadlock behind it. Inode 43 is linked after 42 and refused admission. */
static void fuse_lease_retention_retry_order(int adoption)
{
    reset_stub();
    int handed = 0;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    if (adoption) {
        adopt_rc = EFS_ERR_NET;
        assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_NET && handed);
    }
    assert(efs_open_acquire_fileid(43, 99) == EFS_OK);
    busy_close_ino = 43;
    assert(last_close(43) && release_pending(43) == EFS_ERR_BUSY);
    close_rc = EFS_ERR_NET;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET);
    close_rc = adopt_rc = EFS_OK;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_ERR_BUSY);
    assert(!efs_client_ino_is_open(42) && efs_client_ino_is_open(43));
    busy_close_ino = 0;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_OK && !efs_open_retained());
}

/* From test_fuse_lease_retention.py: every retained identity gets a turn; an
 * older CLOSE whose verdict stays unknown cannot starve a newer one. */
static void fuse_lease_retention_retry_turns(void)
{
    reset_stub();
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && efs_open_acquire_fileid(43, 99) == EFS_OK);
    close_rc = EFS_ERR_NET;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET);
    assert(last_close(43) && release_pending(43) == EFS_ERR_NET);
    close_rc = EFS_OK;
    close_ino = 42;
    close_ino_rc = EFS_ERR_NET;
    assert(efs_open_retry_pass(stats_now_ms() + 200) == EFS_ERR_NET);
    assert(efs_client_ino_is_open(42) && !efs_client_ino_is_open(43) && efs_open_retained() == 1);
    close_ino = 0;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_OK && !efs_open_retained());
}

/* From test_fuse_lease_retention.py: the retry thread drains a retained CLOSE
 * once its edge answers, and stops on request. */
static void fuse_lease_retention_retry_thread(void)
{
    reset_stub();
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    close_rc = EFS_ERR_NET;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET);
    close_rc = EFS_OK;
    efs_open_retry_start();
    while (efs_open_retained())
        sched_yield();
    efs_open_retry_stop();
    assert(!efs_client_ino_is_open(42));
}

/* From test_fuse_lease_retention.py: an unknown CREATE adoption is proven before
 * the last close sends its CLOSE (a reopen fails with its error meanwhile);
 * STALE ends a retained row only as a completed revocation, for the CLOSE
 * and for the adoption before it. */
static void fuse_lease_retention_adoption(void)
{
    reset_stub();
    int handed = 0;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    adopt_rc = EFS_ERR_NET;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_NET && handed);
    unsigned c = closes;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET && closes == c);
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_NET && closes == c);
    adopt_rc = EFS_OK;
    assert(release_pending(42) == EFS_OK && closes == c + 1 && !efs_client_ino_is_open(42));
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && last_close(42));
    close_rc = EFS_ERR_STALE;
    assert(release_pending(42) == EFS_ERR_STALE && efs_open_retained() == 1);
    revocation = 1;
    assert(release_pending(42) == EFS_OK && !efs_client_ino_is_open(42));
    revocation = 0;
    close_rc = EFS_OK;
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    adopt_rc = EFS_ERR_NET;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_NET && handed);
    assert(last_close(42));
    adopt_rc = close_rc = EFS_ERR_STALE;
    revocation = 1;
    assert(release_pending(42) == EFS_OK && !efs_client_ino_is_open(42));
}

/* From test_fuse_lease_retention.py: the CREATE adoption edge. A first open takes
 * OPEN and CLEAR in one combined edge; an existing owner sends only CLEAR.
 * A lost combined answer retains its exact identity (a reopen resumes it, no
 * new edge); a refused admission is retried outside the stripe; a proven
 * refusal leaves no row and sends no CLOSE; a refusal learned after an
 * unknown answer clears the token on its own CLEAR, without a CLOSE; a
 * spent deadline hands nothing off; generation and token are required. */
static void fuse_lease_retention_combined(void)
{
    reset_stub();
    int handed = 0;
    unsigned o = opens, a = adopts, b, c;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_OK && handed);
    assert(opens == o + 1 && adopts == a + 1);
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_OK && handed);
    assert(opens == o + 1 && adopts == a + 2 && !last_close(42));
    drop(42);
    open_rc = EFS_ERR_NET;
    o = opens;
    b = begins_open;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_NET && handed);
    assert(efs_open_retained() == 1);
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_NET && opens == o + 1 && begins_open == b + 1);
    open_rc = EFS_OK;
    assert(release_pending(42) == EFS_OK && !efs_client_ino_is_open(42));
    begin_busy = 2;
    busy_flags = EFS_LEASE_EDGE_OPEN | EFS_LEASE_EDGE_CLEAR_PENDING;
    watch_ino = 42;
    waits = 0;
    b = begins_open;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_OK && handed);
    assert(begins_open == b + 3 && waits == 2 && !begin_busy);
    watch_ino = 0;
    busy_flags = 0;
    drop(42);
    combined_stale = 1;
    c = closes;
    handed = 0;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_STALE && handed);
    assert(!efs_client_ino_is_open(42) && closes == c);
    combined_stale = 2;
    begin_busy = 1;
    busy_flags = EFS_LEASE_EDGE_CLEAR_PENDING;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_NET && handed);
    assert(efs_open_retained() == 1 && !begin_busy);
    assert(release_pending(42) == EFS_OK && !efs_client_ino_is_open(42) && closes == c);
    combined_stale = 0;
    busy_flags = 0;
    t_deadline = stats_now_ms() - 1;
    handed = 1;
    assert(efs_open_acquire_created(42, 99, 3, &handed) == EFS_ERR_BUSY && !handed);
    assert(!efs_client_ino_is_open(42));
    t_deadline = 0;
    assert(efs_open_acquire_created(42, 0, 3, &handed) == EFS_ERR_INVAL);
    assert(efs_open_acquire_created(42, 99, 0, &handed) == EFS_ERR_INVAL);
}

static void *churn(void *arg)
{
    efs_ino_t base = (efs_ino_t)(uintptr_t)arg;
    for (unsigned i = 0; i < 2000; i++) {
        efs_ino_t ino = base + i * 4096;
        assert(efs_open_acquire_fileid(ino, 99) == EFS_OK);
        assert(efs_client_ino_is_open(ino));
        assert(last_close(ino) && release_pending(ino) == EFS_OK);
    }
    return NULL;
}
static void *counter(void *arg)
{
    unsigned *seen = arg;
    for (unsigned i = 0; i < 200; i++) {
        *seen += efs_open_retained() + (unsigned)efs_client_ino_is_open(7 + i * 4096);
        sched_yield();
    }
    return NULL;
}

int main(void)
{
    int leased = -1, publishing = -1;

    /* No row: every operation answers "nothing" and nothing is created. */
    assert(!efs_client_ino_is_open(7) && !efs_open_retained());
    assert(!efs_close_note(7, &leased) && leased == 0);
    assert(!efs_open_append_handoff(7, &publishing) && !publishing);
    assert(!efs_open_append_burst_note(7, 1000));
    efs_open_append_burst_published(7, EFS_OK, 1000);
    assert(release_pending(7) == EFS_OK && !efs_open_close_unadmitted(7));
    assert(!efs_client_ino_is_open(7));

    /* Two opens, one HOLD; the second close is the last and proves CLOSE. */
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && opens == 1);
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && opens == 1);
    assert(efs_client_ino_is_open(42) && !efs_open_retained());
    assert(!efs_close_note(42, &leased) && !leased && efs_client_ino_is_open(42));
    assert(last_close(42) && efs_client_ino_is_open(42) && efs_open_retained() == 1);
    assert(release_pending(42) == EFS_OK && closes == 1);
    assert(!efs_client_ino_is_open(42) && !efs_open_retained());

    /* A CLOSE edge that fails leaves the row retained: open to the evictor
     * and GETATTR, counted by efs_open_retained, refused to a reopen with
     * the edge's error, drained by the retry pass once the edge answers. */
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    close_rc = EFS_ERR_NET;
    assert(last_close(42) && release_pending(42) == EFS_ERR_NET);
    assert(efs_client_ino_is_open(42) && efs_open_retained() == 1);
    assert(!efs_open_close_unadmitted(42)); /* the edge has an identity */
    assert(efs_open_acquire_fileid(42, 99) == EFS_ERR_NET && efs_open_retained() == 1);
    assert(efs_open_retry_pass(stats_now_ms() + 200) == EFS_ERR_NET && efs_open_retained() == 1);
    assert(efs_open_retry_pass(stats_now_ms()) == EFS_ERR_BUSY); /* past deadline */
    close_rc = EFS_OK;
    assert(efs_open_retry_pass(stats_now_ms() + 1000) == EFS_OK);
    assert(!efs_client_ino_is_open(42) && !efs_open_retained());
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK && last_close(42) && release_pending(42) == EFS_OK);

    /* Append bookkeeping on a real row: 32 writes or one second publish
     * (append_burst_clock: the age restarts when the publication is done,
     * and a failed drain publishes again). */
    assert(efs_open_acquire_fileid(42, 99) == EFS_OK);
    for (unsigned i = 0; i < 32; i++)
        assert(!efs_open_append_burst_note(42, 5000));
    assert(efs_open_append_burst_note(42, 5000) == 1);
    assert(efs_open_append_handoff(42, &publishing) == 0 && publishing);
    assert(!efs_open_append_burst_note(42, 5000)); /* publication running */
    efs_open_append_burst_published(42, EFS_OK, 5300);
    assert(efs_open_append_handoff(42, &publishing) == 5400 && !publishing);
    assert(!efs_open_append_burst_note(42, 5350) && !efs_open_append_burst_note(42, 6299));
    assert(efs_open_append_burst_note(42, 6300) == 1); /* one second since 5300 */
    efs_open_append_burst_published(42, EFS_ERR_BUSY, 6400);
    assert(efs_open_append_handoff(42, &publishing) == 5400 && !publishing);
    assert(efs_open_append_burst_note(42, 6401) == 1); /* a failed drain publishes again */
    efs_open_append_burst_published(42, EFS_OK, 6500);
    assert(last_close(42) && release_pending(42) == EFS_OK && !efs_client_ino_is_open(42));

    /* 3,000 retained rows across colliding buckets, drained by the pass. */
    close_rc = EFS_ERR_BUSY;
    for (efs_ino_t i = 1; i <= 3000; i++) {
        assert(efs_open_acquire_fileid(i * 4096, 99) == EFS_OK);
        assert(last_close(i * 4096) && release_pending(i * 4096) == EFS_ERR_BUSY);
    }
    assert(efs_open_retained() == 3000 && efs_client_ino_is_open(3000 * 4096));
    close_rc = EFS_OK;
    unsigned passes = 0;
    while (efs_open_retained() && passes++ < 10)
        assert(efs_open_retry_pass(stats_now_ms() + 1000) != EFS_ERR_NET);
    assert(!efs_open_retained() && passes <= 2 && !efs_client_ino_is_open(4096));

    open_lease_shared_and_failed_hold();
    open_lease_edge_serialization();
    open_lease_hashed_rows();
    open_busy_admission();
    open_busy_verdicts();
    open_busy_deadline();
    open_retry_lock_budget();
    fuse_lease_retention_close_verdicts();
    fuse_lease_retention_retry_order(0);
    fuse_lease_retention_retry_order(1);
    fuse_lease_retention_retry_turns();
    fuse_lease_retention_retry_thread();
    fuse_lease_retention_adoption();
    fuse_lease_retention_combined();
    reset_stub();

    /* Four openers and closers on distinct inodes, one counter beside them. */
    pthread_t t[5];
    unsigned seen = 0, before = opens;
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&t[i], NULL, churn, (void *)(uintptr_t)(1000 + i)));
    assert(!pthread_create(&t[4], NULL, counter, &seen));
    for (unsigned i = 0; i < 5; i++)
        pthread_join(t[i], NULL);
    assert(!efs_open_retained() && opens == before + 8000);
    puts("open interface: no row, two opens, retained CLOSE and the retry pass, append burst, 3000 retained rows, open_lease, open_busy, open_retry_lock_budget, fuse_lease_retention cases, concurrent churn PASS");
    return 0;
}
