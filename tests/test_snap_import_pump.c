/* K113: a member importing snapshot X keeps that import when the leader moves
 * on to a newer snapshot Y, and the pump (not the leader's retries) steps the
 * import and installs it in the Raft core. Reuses the fixture of
 * tests/test_snap_import_host.c (raft_host.c is compiled into it); that
 * file's own checks run first, unchanged. */
#define main snap_import_host_main
#include "test_snap_import_host.c"
#include <sys/eventfd.h>
#undef main

/* K113: the pump installs a finished import in the Raft core. */
static int imported_calls;
static uint64_t imported_incl, imported_term;
int __wrap_efs_raft_snap_imported(struct efs_raft *r, uint64_t incl, uint64_t term)
{
    (void)r;
    imported_calls++;
    imported_incl = incl;
    imported_term = term;
    return EFS_OK;
}

/* K113 round 3: the Raft core's position and role, and the core's record of
 * the dropped image. */
static uint64_t stub_applied;
static int stub_role = EFS_RAFT_FOLLOWER;
static int drop_calls;
static uint64_t drop_incl, drop_term;
uint64_t __wrap_efs_raft_applied(const struct efs_raft *r)
{
    (void)r;
    return stub_applied;
}
int __wrap_efs_raft_role(const struct efs_raft *r)
{
    (void)r;
    return stub_role;
}
void __wrap_efs_raft_snap_import_drop(struct efs_raft *r, uint64_t incl, uint64_t term)
{
    (void)r;
    drop_calls++;
    drop_incl = incl;
    drop_term = term;
}

/* Run the GC pass with stderr captured into line (the telemetry log). */
static void pass_captured(struct efs_raft_host *h, char *line, size_t cap)
{
    char path[EFS_TEST_PATH];
    int fd, saved;
    ssize_t n;

    efs_test_path(path, sizeof(path), "snap-pass-log-XXXXXX");
    fd = efs_test_mkstemp(path);
    CHECK(fd >= 0, "log file");
    fflush(stderr);
    saved = dup(2);
    dup2(fd, 2);
    host_snap_import_pass(h);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    n = pread(fd, line, cap - 1, 0);
    close(fd);
    unlink(path);
    line[n < 0 ? 0 : n] = 0;
}

/* The KV refuses the batch (L0 stall): one step per pump call, and the pump
 * goes back to its normal wait instead of polling with timeout 0. */
static int step_busy_left, step_progress_left, step_calls, step_hard_error;
static int resync_calls;
int __wrap_efs_raft_request_resync(struct efs_raft *r)
{
    (void)r;
    resync_calls++;
    return EFS_OK;
}
int __real_efs_kv_import_step(struct efs_kv_import *, int *);
int __wrap_efs_kv_import_step(struct efs_kv_import *im, int *done)
{
    step_calls++;
    if (step_hard_error) {
        step_hard_error = 0;
        *done = 0;
        return EFS_ERR_IO;
    }
    if (step_busy_left > 0) {
        step_busy_left--;
        *done = 0;
        return EFS_ERR_BUSY;
    }
    if (step_progress_left > 0) {   /* an accepted batch that is not the last */
        step_progress_left--;
        *done = 0;
        return EFS_OK;
    }
    return __real_efs_kv_import_step(im, done);
}

/* 1 if the pump was kicked since the last call (drains the eventfd). */
static int kicked(struct efs_raft_host *h)
{
    uint64_t v;

    return read(h->pump_efd, &v, sizeof(v)) == (ssize_t)sizeof(v);
}

static void test_kv_busy_does_not_spin_the_pump(void)
{
    struct fixture f;
    struct host_group *g;
    int i;

    fixture_make(&f);
    g = &f.h->g[0];
    g->r = (struct efs_raft *)&f;
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "queued");
    host_snap_import_pass(f.h);
    CHECK(g->import_state == SNAP_IMP_DONE, "importer is open");
    f.h->pump_efd = eventfd(0, EFD_NONBLOCK);
    CHECK(f.h->pump_efd >= 0, "eventfd");
    step_busy_left = 5;
    step_progress_left = 1;
    step_calls = 0;
    imported_calls = 0;
    for (i = 1; i <= 5; i++) {
        host_snap_import_pump(f.h);
        CHECK(step_calls == i, "one step per pump call");
        CHECK(kicked(f.h) == 0, "the KV refused the batch: the pump keeps its normal wait");
    }
    CHECK(!imported_calls && g->import_state == SNAP_IMP_DONE, "nothing installed while refused");
    host_snap_import_pump(f.h);
    CHECK(kicked(f.h) == 1, "an accepted step that is not the last: the next cycle runs at once");
    for (i = 0; i < 64 && !imported_calls; i++)
        host_snap_import_pump(f.h);
    CHECK(imported_calls == 1 && converged(&f), "the import finishes once the KV accepts");
    close(f.h->pump_efd);
    fixture_free(&f);
}

static void test_newer_chunk_keeps_import_and_pump_installs(void)
{
    struct fixture f;
    struct host_group *g;
    struct efs_kv_import *im;
    uint32_t steps0;
    int i;

    fixture_make(&f);
    g = &f.h->g[0];
    g->r = (struct efs_raft *)&f;           /* only passed to the wrapped install */
    f.h->pump_efd = -1;
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "chunk of X queues the import");
    host_snap_import_pass(f.h);
    CHECK(g->import_state == SNAP_IMP_DONE && g->import_im, "X's importer is open");
    im = g->import_im;
    /* the leader moved on: chunk 0 of a newer image Y, one-chunk and multi-chunk */
    CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, (uint32_t)f.img_len, 1) == EFS_ERR_AGAIN,
          "closing chunk of Y at offset 0 is refused");
    CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, 16, 0) == EFS_ERR_AGAIN,
          "first chunk of Y is refused");
    CHECK(g->import_state == SNAP_IMP_DONE && g->import_im == im &&
          g->import_incl == INCL && access(g->import_path, F_OK) == 0,
          "X's import and its .part are untouched");
    CHECK(__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE), "applies stay blocked");
    /* no leader retry of X: only the pump runs */
    steps0 = g->import_steps;
    for (i = 0; i < 64 && !imported_calls; i++)
        host_snap_import_pump(f.h);
    CHECK(g->import_steps > steps0, "the pump advances the import without a retry");
    CHECK(imported_calls == 1 && imported_incl == INCL && imported_term == 1,
          "X is installed in the Raft core by the pump");
    CHECK(g->import_state == SNAP_IMP_IDLE && !g->import_im &&
          !__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE), "import finished, applies unblocked");
    CHECK(converged(&f), "KV is exactly X's image");
    CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, 16, 0) == EFS_OK,
          "Y is accepted once X is installed");
    fixture_free(&f);
}

/* A hard step error: the importer is dropped, the pump logs the failure and
 * asks for a resync, does not kick itself, and a chunk at offset 0 starts a
 * fresh import instead of being refused for good. */
static void test_hard_error_requests_resync(void)
{
    struct fixture f;
    struct host_group *g;
    char path[EFS_TEST_PATH];
    char line[512] = "";
    int fd, saved, i;
    ssize_t n;

    fixture_make(&f);
    g = &f.h->g[0];
    g->r = (struct efs_raft *)&f;
    f.h->pump_efd = eventfd(0, EFD_NONBLOCK);
    CHECK(f.h->pump_efd >= 0, "eventfd");
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "queued");
    host_snap_import_pass(f.h);
    CHECK(g->import_state == SNAP_IMP_DONE, "importer is open");
    efs_test_path(path, sizeof(path), "snap-pump-log-XXXXXX");
    fd = efs_test_mkstemp(path);
    CHECK(fd >= 0, "log file");
    fflush(stderr);
    saved = dup(2);
    dup2(fd, 2);
    step_hard_error = 1;
    imported_calls = 0;
    resync_calls = 0;
    host_snap_import_pump(f.h);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    n = pread(fd, line, sizeof(line) - 1, 0);
    close(fd);
    unlink(path);
    if (n < 0) n = 0;
    line[n] = 0;
    CHECK(strstr(line, "raft-snap: import failed group=0 incl=") != NULL &&
          strstr(line, "rc=-1") != NULL, "the failure is logged");
    CHECK(resync_calls == 1, "a resync is requested");
    CHECK(!imported_calls, "nothing is installed");
    CHECK(kicked(f.h) == 0, "no kick after a hard error");
    CHECK(g->import_state == SNAP_IMP_IDLE && !g->import_im &&
          !__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE),
          "the failed importer is dropped and applies are unblocked");
    for (i = 0; i < 4; i++)
        host_snap_import_pump(f.h);
    CHECK(resync_calls == 1, "the pump does not ask again");
    CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, 16, 0) == EFS_OK,
          "a chunk at offset 0 is accepted afterwards");
    CHECK(access(g->import_path, F_OK) != 0, "the failed image file is unlinked");
    close(f.h->pump_efd);
    fixture_free(&f);
}

/* C1/C2: an image still QUEUED when the member has applied past it (or leads)
 * is dropped before the KV is touched; below it, the import runs as before. */
static void test_queued_import_dropped(uint64_t applied, int role, int expect_drop)
{
    struct fixture f;
    struct host_group *g;
    char line[4096];
    char path[EFS_MAX_PATH];

    fixture_make(&f);
    g = &f.h->g[0];
    g->r = (struct efs_raft *)&f;
    stub_applied = applied;
    stub_role = role;
    drop_calls = 0;
    import_open_calls = 0;
    step_calls = 0;
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "chunk of X queues the import");
    snprintf(path, sizeof(path), "%s", g->import_path);
    CHECK(access(path, F_OK) == 0, "the image file exists");
    pass_captured(f.h, line, sizeof(line));
    if (expect_drop) {
        CHECK(import_open_calls == 0 && step_calls == 0, "the KV import was never called");
        CHECK(!converged(&f) && !g->import_im, "the KV is untouched");
        CHECK(g->import_state == SNAP_IMP_IDLE, "state is IDLE");
        CHECK(!__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE), "import_block is clear");
        CHECK(access(path, F_OK) != 0, "the image file is gone");
        CHECK(strstr(line, "raft-snap: import dropped group=0 incl=10 applied=") != NULL,
              "the drop is logged");
        CHECK(drop_calls == 1 && drop_incl == INCL && drop_term == 1,
              "the core is told to forget the record");
        CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, 16, 0) == EFS_OK,
              "a chunk at offset 0 is accepted afterwards");
    } else {
        CHECK(import_open_calls == 1 && g->import_state == SNAP_IMP_DONE && g->import_im,
              "applied below X: the import runs");
        CHECK(!drop_calls && strstr(line, "import dropped") == NULL, "nothing is dropped");
        CHECK(__atomic_load_n(&g->import_block, __ATOMIC_ACQUIRE), "applies stay blocked");
    }
    stub_applied = 0;
    stub_role = EFS_RAFT_FOLLOWER;
    fixture_free(&f);
}

int main(void)
{
    CHECK(snap_import_host_main() == 0, "tests/test_snap_import_host.c checks");
    test_newer_chunk_keeps_import_and_pump_installs();
    imported_calls = 0;
    test_kv_busy_does_not_spin_the_pump();
    test_hard_error_requests_resync();
    test_queued_import_dropped(INCL, EFS_RAFT_FOLLOWER, 1);
    test_queued_import_dropped(INCL + 5, EFS_RAFT_FOLLOWER, 1);
    test_queued_import_dropped(INCL - 1, EFS_RAFT_LEADER, 1);
    test_queued_import_dropped(INCL - 1, EFS_RAFT_FOLLOWER, 0);
    printf("PASS K113: newer snapshot chunk keeps the import, the pump steps and installs it without a leader retry\n");
    return 0;
}
