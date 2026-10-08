/* Compile only principles_index.c with -Dfsync=efs_index_test_fsync and
 * -Dpwrite=efs_index_test_pwrite. No production fault injection. */
#include "src/bench/principles_index.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef NDEBUG
#error This test requires assertions (including checked I/O) enabled
#endif
static unsigned sync_call, fail_sync;
static long write_budget = -1;
static unsigned scan_call, fail_scan;
struct dirent *efs_index_test_readdir(DIR *dir)
{
    if (++scan_call == fail_scan) {
        errno = EIO;
        return NULL;
    }
    return readdir(dir);
}
int efs_index_test_fsync(int fd)
{
    if (++sync_call == fail_sync) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
ssize_t efs_index_test_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (write_budget == 0) {
        errno = ENOSPC;
        return -1;
    }
    if (write_budget > 0 && n > (size_t)write_budget)
        n = (size_t)write_budget;
    ssize_t got = pwrite(fd, buf, n, off);
    if (got > 0 && write_budget > 0)
        write_budget -= got;
    return got;
}
static void remove_owned(const char *root)
{
    DIR *dir = opendir(root);
    assert(dir);
    struct dirent *de;
    while ((de = readdir(dir)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            char path[512];
            assert(snprintf(path, sizeof(path), "%s/%s", root, de->d_name) < (int)sizeof(path));
            assert(!unlink(path));
        }
    assert(!closedir(dir));
    assert(!rmdir(root));
}
static void failure_case(unsigned barrier, long budget)
{
    char root[] = "/tmp/efs-index-fault-XXXXXX";
    assert(mkdtemp(root));
    struct principles_index *m = principles_index_open(root, 16, 4u << 20, 1, 0);
    assert(m);
    struct principles_change c = {1, 3, 0, "old"};
    assert(!principles_index_publish(m, &c, 1));
    c = (struct principles_change){1, 3, 1, "new"};
    sync_call = 0;
    fail_sync = barrier;
    write_budget = budget;
    assert(principles_index_publish(m, &c, 1) == -1);
    assert(errno == (barrier ? EIO : ENOSPC));
    /* Failure cannot be bypassed by a later successful barrier or request. */
    fail_sync = 0;
    write_budget = -1;
    assert(principles_index_publish(m, &c, 1) == -1 && errno == EINVAL);
    uint64_t removed;
    assert(principles_index_collect(m, &removed) == -1 && errno == EINVAL);
    principles_index_close(m);
    m = principles_index_open(root, 16, 4u << 20, 0, 1);
    assert(m);
    char b[8];
    uint32_t n = sizeof(b);
    uint64_t gen;
    assert(!principles_index_get(m, 1, b, &n, &gen) && n == 3);
    /* Only a failed final directory barrier has installed the new manifest. */
    assert(gen == (barrier == 4 ? 2u : 1u));
    assert(!memcmp(b, barrier == 4 ? "new" : "old", 3));
    principles_index_close(m);
    m = principles_index_open(root, 16, 4u << 20, 0, 0);
    assert(m);
    int rc = principles_index_collect(m, &removed);
    /* Incomplete scratch headers are not sufficient deletion authority. */
    int unidentifiable = budget >= 0 && (budget < 24 || (budget >= 79 && budget < 111));
    if (unidentifiable)
        assert(rc == -1 && errno == EIO);
    else {
        assert(!rc);
        c.expected_generation = gen;
        assert(!principles_index_publish(m, &c, 1));
    }
    principles_index_close(m);
    remove_owned(root);
}
static void collection_failure(void)
{
    char root[] = "/tmp/efs-index-fault-XXXXXX";
    assert(mkdtemp(root));
    struct principles_index *m = principles_index_open(root, 16, 4u << 20, 1, 0);
    assert(m);
    struct principles_change c = {1, 3, 0, "old"};
    assert(!principles_index_publish(m, &c, 1));
    c.expected_generation = 1;
    c.data = "new";
    assert(!principles_index_publish(m, &c, 1));
    sync_call = 0;
    fail_sync = 1;
    uint64_t removed;
    assert(principles_index_collect(m, &removed) == -1 && errno == EIO);
    fail_sync = 0;
    assert(principles_index_collect(m, &removed) == -1 && errno == EINVAL);
    principles_index_close(m);
    m = principles_index_open(root, 16, 4u << 20, 0, 1);
    assert(m);
    char b[8];
    uint32_t n = sizeof(b);
    uint64_t gen;
    assert(!principles_index_get(m, 1, b, &n, &gen) && gen == 2 && !memcmp(b, "new", 3));
    principles_index_close(m);
    remove_owned(root);
}
static void inventory_failures(void)
{
    char root[] = "/tmp/efs-index-fault-XXXXXX";
    assert(mkdtemp(root));
    scan_call = 0;
    fail_scan = 1;
    assert(!principles_index_open(root, 16, 4u << 20, 1, 0) && errno == EIO);
    fail_scan = 0;
    /* Failed empty-directory inventory must not create even the lock file. */
    struct principles_index *m = principles_index_open(root, 16, 4u << 20, 1, 0);
    assert(m);
    struct principles_change c = {1, 3, 0, "old"};
    assert(!principles_index_publish(m, &c, 1));
    struct principles_index_stats st;
    scan_call = 0;
    fail_scan = 1;
    assert(principles_index_stats(m, &st) == -1 && errno == EIO);
    scan_call = 0;
    c.expected_generation = 1;
    c.data = "new";
    assert(principles_index_publish(m, &c, 1) == -1 && errno == EIO);
    fail_scan = 0;
    /* Pre-admission failure changes nothing and can safely retry. */
    assert(!principles_index_publish(m, &c, 1));
    scan_call = 0;
    fail_scan = 2;
    uint64_t removed;
    assert(principles_index_collect(m, &removed) == -1 && errno == EIO);
    fail_scan = 0;
    principles_index_close(m);
    scan_call = 0;
    fail_scan = 1;
    assert(!principles_index_open(root, 16, 4u << 20, 0, 1) && errno == EIO);
    fail_scan = 0;
    m = principles_index_open(root, 16, 4u << 20, 0, 1);
    assert(m);
    char b[8];
    uint32_t n = sizeof(b);
    uint64_t gen;
    assert(!principles_index_get(m, 1, b, &n, &gen) && gen == 2 && !memcmp(b, "new", 3));
    principles_index_close(m);
    remove_owned(root);
}
int main(void)
{
    for (unsigned barrier = 1; barrier <= 4; barrier++)
        failure_case(barrier, -1);
    /* New segment header=24, one body record=55, manifest starts at byte79. */
    const long cuts[] = {0, 8, 24, 30, 78, 79, 87, 110, 111, 1000};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++)
        failure_case(0, cuts[i]);
    collection_failure();
    inventory_failures();
    puts("indexed fault gates: four publication barriers, ten short-write/ENOSPC boundaries, "
         "sticky failure, read-only recovery, inventory errors and GC-sync failure PASS");
    return 0;
}
