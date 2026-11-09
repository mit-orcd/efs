/* Task EB: the open-reference table through its owner interface (open.h).
 *
 * Links the real src/client/efs_fuse/open.c and nothing else of the
 * client. The lease edge RPCs, the FileID fetch and the helpers open.c
 * takes from other files (support, getattr, release, the .efs command
 * surface, the RPC deadline) are link-time definitions here; the lease
 * stub answers the next OPEN or CLOSE edge with a chosen rc.
 *
 * Cases: an inode without a row; two opens and two closes (one HOLD, one
 * CLOSE, the row gone); a last close whose CLOSE edge fails: the row is
 * retained, counts as open and as retained, a reopen fails with the edge's
 * error, the retry pass drains it once the edge answers; the append burst
 * and handoff bookkeeping on a real row and on no row; 3,000 retained rows
 * across colliding hash buckets drained by the retry pass; four threads
 * opening and closing distinct inodes while another counts (TSan entry). */
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
static int open_rc, close_rc; /* the next OPEN / CLOSE edge's answer (resume) */
static unsigned opens, closes, seq;
static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;

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
void efs_client_opid_wait(unsigned max_ms) { usleep(max_ms * 1000u); }

int efs_client_lease_begin(struct efs_client_lease_request *r, efs_ino_t ino,
                           uint64_t gen, unsigned flags, uint64_t token)
{
    assert(!r->active);
    *r = (struct efs_client_lease_request){.ino = ino, .generation = gen,
                                           .pending_token = token, .flags = flags, .active = 1};
    pthread_mutex_lock(&stub_mu);
    r->q.id.seq = ++seq;
    pthread_mutex_unlock(&stub_mu);
    return EFS_OK;
}
int efs_client_lease_resume(struct efs_client_lease_request *r, int probe)
{
    assert(!probe && r->active);
    if (r->complete)
        return r->result;
    int open = !!(r->flags & EFS_LEASE_EDGE_OPEN);
    pthread_mutex_lock(&stub_mu);
    int rc = open ? open_rc : close_rc;
    if (open) opens++; else closes++;
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

    /* Append bookkeeping on a real row: 32 writes or one second publish. */
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

    /* Four openers and closers on distinct inodes, one counter beside them. */
    pthread_t t[5];
    unsigned seen = 0;
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&t[i], NULL, churn, (void *)(uintptr_t)(1000 + i)));
    assert(!pthread_create(&t[4], NULL, counter, &seen));
    for (unsigned i = 0; i < 5; i++)
        pthread_join(t[i], NULL);
    assert(!efs_open_retained() && opens == 1 + 2 + 1 + 3000 + 8000);
    puts("open interface: no row, two opens, retained CLOSE and the retry pass, append burst, 3000 retained rows, concurrent churn PASS");
    return 0;
}
