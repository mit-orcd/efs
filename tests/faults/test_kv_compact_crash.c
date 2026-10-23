/* Compaction crash recovery (K24 / W149). No sockets, no cluster.
 *
 * kv_compact.c is compiled into this binary with -DEFS_FAULTS=1, so the
 * EFS_FAULT_COMPACT_DIE hook exists here and nowhere else: the library in
 * libefs.a and every daemon built from it carry no such exit. A forked child
 * opens the store, compacts, and dies after its first finished output
 * segment is durable but before the manifest rename. Reopening must still
 * serve every key from the old segments. Before K24 this case lived in
 * tests/test_kv_lsm.c and the hook was read in every build. */
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../test_tmpdir.h"

#if !EFS_FAULTS
#error "tests/faults/test_kv_compact_crash needs -DEFS_FAULTS=1 (see the Makefile rule)"
#endif

static int failures;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static char g_dir[512];

static void rmtree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char p[1024];

    if (!d)
        return;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static struct efs_kv *open_store(void)
{
    struct efs_kv_lsm_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_SYNC;
    cfg.l0_max = 3;
    return efs_kv_lsm_open(g_dir, &cfg);
}

static int put_s(struct efs_kv *kv, const char *k, const char *v)
{
    return efs_kv_put(kv, (const uint8_t *)k, (uint32_t)strlen(k),
                      (const uint8_t *)v, (uint32_t)strlen(v));
}

/* EFS_OK and v matches, or the rc otherwise. */
static int get_is(struct efs_kv *kv, const char *k, const char *v)
{
    uint8_t buf[256];
    uint32_t len = sizeof(buf);
    int rc = efs_kv_get(kv, (const uint8_t *)k, (uint32_t)strlen(k), buf, &len);

    if (rc != EFS_OK)
        return rc;
    if (len != strlen(v) || memcmp(buf, v, len) != 0)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

/* Kill after the compaction output file is durable and before the manifest
 * rename. Reopen must still serve every key from the old segments. */
static void test_compact_crash(void)
{
    struct efs_kv *kv;
    pid_t pid;
    int status = -1;
    int i, bad = 0;

    rmtree(g_dir);
    kv = open_store();
    CHECK(kv != NULL, "open crash-compact");
    if (!kv)
        return;
    for (i = 0; i < 20; i++) {
        char k[32], v[32];

        snprintf(k, sizeof(k), "ck%02d", i);
        snprintf(v, sizeof(v), "cv%02d", i);
        CHECK(put_s(kv, k, v) == EFS_OK, "put ck");
    }
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush ck");
    efs_kv_lsm_close(kv);
    setenv("EFS_FAULT_COMPACT_DIE", "1", 1);
    pid = fork();
    CHECK(pid >= 0, "fork compact");
    if (pid == 0) {
        kv = open_store();
        if (!kv)
            _exit(2);
        if (efs_kv_lsm_compact(kv) == EFS_OK)
            _exit(0);
        _exit(3);
    }
    unsetenv("EFS_FAULT_COMPACT_DIE");
    if (pid > 0) {
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 99, "died after segment");
    }
    kv = open_store();
    CHECK(kv != NULL, "reopen after compact crash");
    if (!kv)
        return;
    for (i = 0; i < 20; i++) {
        char k[32], v[32];

        snprintf(k, sizeof(k), "ck%02d", i);
        snprintf(v, sizeof(v), "cv%02d", i);
        if (get_is(kv, k, v) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "keys survived compact crash");
    efs_kv_lsm_close(kv);
}

int main(void)
{
    efs_test_own(efs_test_path(g_dir,sizeof(g_dir),"efs-kvcrash-%d",(int)getpid()));
    rmtree(g_dir);
    test_compact_crash();
    rmtree(g_dir);
    if (failures) {
        fprintf(stderr, "test_kv_compact_crash: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv_compact_crash: ok\n");
    return 0;
}
