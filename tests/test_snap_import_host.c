/* K108 follow-up: the real raft_host.c snapshot-import path (host_snap_chunk,
 * host_snap_import_pass, snap_import_poll), driven by hand on one thread over
 * LSM stores. raft_host.c is compiled into this file like tests/decoders/apply.c.
 *
 *  1. a failed importer is discarded: a one-chunk image whose importer fails
 *     is not retried forever, the resend queues a fresh import and converges;
 *  2. the last import batch is durable when the chunk is acked, even under the
 *     pump's KV WAL hold (the ack saves the Raft snapshot record at once);
 *  3. an import cancelled while its open runs leaves import_block clear;
 *  4. the pass waits out a running apply (h->mu) before it exports. */
#include "../src/server/raft_host.c"
#include "efs/kv_lsm.h"
#include "test_tmpdir.h"

#include <assert.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, m); exit(1); } } while (0)

#define N_SRC 200u          /* image keys: inodes 1..200 */
#define N_HOST_LO 101u      /* host keys: inodes 101..300, other values */
#define N_HOST_HI 300u
#define VLEN 4096u          /* ~800 KB image: larger than the importer's 256 KiB window, under one 4 MiB chunk */
#define INCL 10u

/* test_cancel_in_open: cancel the import from inside its open, as a
 * newer InstallSnapshot would while the GC thread scans. */
static struct efs_raft_host *cancel_host;
int __real_efs_kv_import_open(struct efs_kv *, uint8_t, const char *, const char *,
                              struct efs_kv_import **);
int __wrap_efs_kv_import_open(struct efs_kv *kv, uint8_t group, const char *in,
                              const char *local, struct efs_kv_import **out)
{
    if (cancel_host) {
        snap_import_cancel(cancel_host, &cancel_host->g[0]);
        cancel_host = NULL;
    }
    return __real_efs_kv_import_open(kv, group, in, local, out);
}

/* The pass must take h->mu (the lock every raft apply runs under) after it
 * blocks applies and before it flushes and exports the local store. */
static struct efs_raft_host *watch_host;
static int watch_locks;
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m)
{
    if (watch_host && m == &watch_host->mu)
        watch_locks++;
    return __real_pthread_mutex_lock(m);
}
int __real_efs_kv_lsm_flush(struct efs_kv *);
int __wrap_efs_kv_lsm_flush(struct efs_kv *kv)
{
    if (watch_host)
        CHECK(watch_locks > 0, "the pass waits out a running apply (h->mu) before its export");
    return __real_efs_kv_lsm_flush(kv);
}

static void put_ino(struct efs_kv *kv, uint32_t i, uint8_t variant)
{
    uint8_t k[64], v[VLEN];
    uint32_t kl;

    CHECK(efs_kv_key_inode(1, i, k, &kl) == EFS_OK, "key");
    memset(v, (int)(i + variant), sizeof(v));
    CHECK(efs_kv_put(kv, k, kl, v, VLEN) == EFS_OK, "put");
}

struct fixture {
    char dir[EFS_TEST_PATH], img_path[EFS_TEST_PATH + 16];
    struct efs_raft_host *h;
    uint8_t *img;
    size_t img_len;
};

static void fixture_make(struct fixture *f)
{
    char sub[EFS_TEST_PATH + 16];
    struct efs_kv *src;
    struct efs_kv_lsm_view *view;
    struct efs_kv_lsm_cfg nosync = {.sync_mode = EFS_KV_LSM_NOSYNC};
    FILE *fp;

    memset(f, 0, sizeof(*f));
    efs_test_path(f->dir, sizeof(f->dir), "efs-snaphost-XXXXXX");
    CHECK(efs_test_mkdtemp(f->dir) != NULL, "tmpdir");
    /* the leader's store, exported as the snapshot image */
    snprintf(sub, sizeof(sub), "%s/src", f->dir);
    src = efs_kv_lsm_open(sub, &nosync);    /* only its export is used */
    CHECK(src != NULL, "open src");
    for (uint32_t i = 1; i <= N_SRC; i++)
        put_ino(src, i, 0);
    CHECK(efs_kv_lsm_flush(src) == EFS_OK, "flush src");
    CHECK(efs_kv_lsm_view_pin(src, &view) == EFS_OK, "pin");
    snprintf(f->img_path, sizeof(f->img_path), "%s/img.kvx", f->dir);
    CHECK(efs_kv_lsm_view_export(view, 0, f->img_path) == EFS_OK, "export");
    efs_kv_lsm_view_unpin(view);
    efs_kv_lsm_close(src);
    fp = fopen(f->img_path, "rb");
    CHECK(fp != NULL, "open img");
    fseek(fp, 0, SEEK_END);
    f->img_len = (size_t)ftell(fp);
    rewind(fp);
    f->img = malloc(f->img_len);
    CHECK(f->img && fread(f->img, 1, f->img_len, fp) == f->img_len, "read img");
    fclose(fp);
    /* the rejoining member */
    f->h = calloc(1, sizeof(*f->h));
    CHECK(f->h != NULL, "host");
    snprintf(sub, sizeof(sub), "%s/host", f->dir);
    f->h->kv = efs_kv_lsm_open(sub, NULL);
    CHECK(f->h->kv != NULL, "open host kv");
    CHECK(efs_kv_lsm_sync_hold(f->h->kv) == EFS_OK, "seed hold");  /* one fsync, not 200 */
    for (uint32_t i = N_HOST_LO; i <= N_HOST_HI; i++)
        put_ino(f->h->kv, i, 1);
    CHECK(efs_kv_lsm_sync_release(f->h->kv) == EFS_OK, "seed release");
    snprintf(f->h->mdraft, sizeof(f->h->mdraft), "%s", f->dir);
    f->h->gc_running = 1;
    CHECK(pthread_mutex_init(&f->h->snap_mu, NULL) == 0 &&
          pthread_mutex_init(&f->h->mu, NULL) == 0, "mutex");
    for (int i = 0; i < HOST_NGROUPS; i++) {
        f->h->g[i].host = f->h;
        f->h->g[i].group = (uint8_t)i;
        f->h->g[i].part_fd = -1;
    }
}

static void fixture_free(struct fixture *f)
{
    free(f->img);
    efs_kv_lsm_close(f->h->kv);
    free(f->h);
}

/* The host holds exactly the image's keys, with the image's values. */
static int converged(struct fixture *f)
{
    uint8_t k[64], v[VLEN + 8], want[VLEN];
    uint32_t kl, vl;

    for (uint32_t i = 1; i <= N_HOST_HI; i++) {
        int rc;

        CHECK(efs_kv_key_inode(1, i, k, &kl) == EFS_OK, "key");
        vl = sizeof(v);
        rc = efs_kv_get(f->h->kv, k, kl, v, &vl);
        if (i > N_SRC) {
            if (rc != EFS_ERR_NOT_FOUND)
                return 0;
            continue;
        }
        memset(want, (int)i, sizeof(want));
        if (rc != EFS_OK || vl != VLEN || memcmp(v, want, VLEN) != 0)
            return 0;
    }
    return 1;
}

static int send_chunk(struct fixture *f)
{
    return host_snap_chunk(&f->h->g[0], INCL, 1, 0, f->img, (uint32_t)f->img_len, 1);
}

/* Leader retries until OK; the GC thread's pass runs between the retries. */
static int drive(struct fixture *f, int max)
{
    int rc = EFS_ERR_BUSY;

    for (int i = 0; i < max && rc == EFS_ERR_BUSY; i++) {
        rc = send_chunk(f);
        if (rc == EFS_ERR_BUSY)
            host_snap_import_pass(f->h);
    }
    return rc;
}

static void test_failed_importer_is_discarded(void)
{
    struct fixture f;
    struct host_group *g;
    int rc;

    fixture_make(&f);
    g = &f.h->g[0];
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "chunk queues the import");
    watch_host = f.h;
    host_snap_import_pass(f.h);
    watch_host = NULL;
    CHECK(g->import_state == SNAP_IMP_DONE && g->import_im, "importer is open");
    /* the image is cut after open, past the importer's window: the first
     * step fails, stickily */
    CHECK(truncate(g->import_path, (off_t)(f.img_len / 2)) == 0, "truncate");
    rc = send_chunk(&f);
    CHECK(rc != EFS_OK && rc != EFS_ERR_BUSY, "the step error reaches the leader");
    CHECK(g->import_state == SNAP_IMP_IDLE && !g->import_im,
          "the failed importer is discarded");
    CHECK(!__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE),
          "applies are not blocked after the failure");
    /* the leader's resend (offset 0, done = 1, whole image) starts afresh */
    CHECK(drive(&f, 8) == EFS_OK, "the resend converges");
    CHECK(converged(&f), "KV is exactly the image");
    fixture_free(&f);
}

static void test_last_batch_durable_under_hold(void)
{
    struct fixture f;
    uint64_t prefix = 0;
    int covered = 0;

    fixture_make(&f);
    CHECK(efs_kv_lsm_sync_hold(f.h->kv) == EFS_OK, "pump hold");
    CHECK(drive(&f, 8) == EFS_OK, "import acked");
    /* This is the instant snap_installed would save the Raft record. */
    CHECK(efs_kv_lsm_sync_detach(f.h->kv, &prefix) == EFS_OK && prefix, "detach");
    CHECK(efs_kv_lsm_sync_covered(f.h->kv, prefix, &covered) == EFS_OK && covered,
          "every import batch is fsynced before the ack");
    CHECK(converged(&f), "KV is exactly the image");
    fixture_free(&f);
}

static void test_cancel_during_open_clears_block(void)
{
    struct fixture f;
    struct host_group *g;

    fixture_make(&f);
    g = &f.h->g[0];
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "queued");
    cancel_host = f.h;
    host_snap_import_pass(f.h);
    CHECK(!cancel_host, "the cancel ran inside open");
    CHECK(g->import_state == SNAP_IMP_IDLE && !g->import_im, "cancelled import dropped");
    CHECK(!__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE),
          "a cancelled run whose open succeeded leaves applies unblocked");
    CHECK(drive(&f, 8) == EFS_OK, "the next InstallSnapshot converges");
    CHECK(converged(&f), "KV is exactly the image");
    fixture_free(&f);
}

int main(void)
{
    test_failed_importer_is_discarded();
    test_last_batch_durable_under_hold();
    test_cancel_during_open_clears_block();
    printf("PASS snapshot import host path: failed importer discarded, last batch durable, cancel clears the block\n");
    return 0;
}
