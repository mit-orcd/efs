/* K113: a member importing snapshot X keeps that import when the leader moves
 * on to a newer snapshot Y, and the pump (not the leader's retries) steps the
 * import and installs it in the Raft core. Reuses the fixture of
 * tests/test_snap_import_host.c (raft_host.c is compiled into it); that
 * file's own checks run first, unchanged. */
#define main snap_import_host_main
#include "test_snap_import_host.c"
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
    CHECK(send_chunk(&f) == EFS_ERR_BUSY, "chunk of X queues the import");
    host_snap_import_pass(f.h);
    CHECK(g->import_state == SNAP_IMP_DONE && g->import_im, "X's importer is open");
    im = g->import_im;
    /* the leader moved on: chunk 0 of a newer image Y, one-chunk and multi-chunk */
    CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, (uint32_t)f.img_len, 1) == EFS_ERR_BUSY,
          "closing chunk of Y at offset 0 is refused");
    CHECK(host_snap_chunk(g, INCL + 1, 1, 0, f.img, 16, 0) == EFS_ERR_BUSY,
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


int main(void)
{
    CHECK(snap_import_host_main() == 0, "tests/test_snap_import_host.c checks");
    test_newer_chunk_keeps_import_and_pump_installs();
    printf("PASS K113: newer snapshot chunk keeps the import, the pump steps and installs it without a leader retry\n");
    return 0;
}
