/* Reuse the current unit fixture's storage transport stubs and helpers.
 * All client stripe bookkeeping is the production implementation. */
#define main upstream_data_commit_main
#include "../test_data_commit.c"
#undef main
int main(int argc, char **argv)
{
    int fixed = argc > 1 && !strcmp(argv[1], "fixed");
    for (int i = 1; i < NN; i++) epochs[i] = 101;
    /* PUTs have acknowledged, but REPORT publication has not run yet. */
    for (efs_ino_t ino = 900; ino < 916; ino++)
        assert(put(ino, 0, 1, 2, 3, 7) == EFS_OK);
    assert(live() == 16);
    epochs[3] = 102; /* node 3 restarted after acknowledging these PUTs */
    mark_rc = EFS_ERR_NOT_FOUND; /* correct for objects not published yet */
    reset();
    int put_rc = put(916, 0, 1, 2, 3, 7); /* capacity pressure sweeps */
    int remaining = live();
    printf("sweep attempted marks=%d, remaining stripe records=%d\n", marks, remaining);
    int forgot = stripe_find(900, 0, 900000) == NULL;
    /* REPORT can now publish object 900000. No production callback marks the
     * client record published/reinstates it: check its next fsync behavior. */
    mark_rc = EFS_OK;
    reset();
    int rc = efs_client_data_commit(900);
    printf("later fsync rc=%d COMMITs=%d,%d,%d marks=%d record forgotten=%d\n",
           rc, commits[1], commits[2], commits[3], marks, forgot);
    int confirmed = remaining == 1 && forgot && rc == EFS_OK && !marks &&
                    !commits[1] && !commits[2] && !commits[3];
    printf("PREPUBLICATION_RETIREMENT_CONFIRMED=%d\n", confirmed);
    free(g_st);
    return fixed ? (!forgot && remaining == 16 && put_rc == EFS_ERR_BUSY && marks == 1 && rc == EFS_OK ? 0 : 1) : (confirmed && put_rc == EFS_OK ? 0 : 1);
}
