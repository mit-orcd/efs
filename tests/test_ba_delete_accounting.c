/* BA: actual production recovery/delete/settlement code. This fixture
 * assumes terminal deletion authority; test_reclaim tests its journal and
 * fences. The syscall injection chooses which authorized deleter wins. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
static int accounting_fsync(int);
#define fsync(fd) accounting_fsync(fd)
#include "../src/server/store.c"
#undef fsync
#include "../src/server/reclaim.c"
static int segment_unlink(const char *);
#define unlink(path) segment_unlink(path)
#include "../src/server/segment_store.c"
#undef unlink
#include "test_tmpdir.h"

static struct efsd_server S;
static int fail_sync;
static int accounting_fsync(int fd)
{
    if (fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
static int (*unlink_fault)(const char *);
static int segment_unlink(const char *p)
{
    return unlink_fault ? unlink_fault(p) : unlink(p);
}
static uint64_t used(void) { return server_local_node(&S)->used; }
static void setup(char *root, size_t len)
{
    efs_test_path(root, len, "efs-ba-account-XXXXXX");
    assert(efs_test_mkdtemp(root));
    memset(&S, 0, sizeof(S));
    S.id = S.node_count = S.storage_path_count = 1;
    S.nodes[0].id = 1;
    strcpy(S.storage_paths[0], root);
    pthread_mutex_init(&S.lock, NULL);
}
static void file(const char *path, uint64_t bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !ftruncate(fd, (off_t)bytes) && !fsync(fd));
    assert(!close(fd));
}
static void absent_recovery(void)
{
    char root[EFS_TEST_PATH], target[EFS_TEST_PATH + 32], live[EFS_TEST_PATH + 32];
    setup(root, sizeof(root));
    snprintf(target, sizeof(target), "%s/target", root);
    snprintf(live, sizeof(live), "%s/live", root);
    file(target, 8192); file(live, 8192);
    S.nodes[0].used = 16384;
    struct server_recovery_files first = {0}, retry = {0};
    assert(!server_recovery_drop(&S, target, 8192, 1, &first));
    assert(used() == 8192 && first.dropped == 1);
    /* A caller captured bytes before the first deletion; ENOENT is an
     * idempotent success, but cannot debit the unrelated live allocation. */
    assert(!server_recovery_drop(&S, target, 8192, 1, &retry));
    assert(used() == 8192 && retry.dropped == 0 && retry.bytes_dropped == 0);
    assert(!access(live, F_OK));
    puts("BA absent recovery: one unlink, one debit PASS");
}
static int recovery_wins(const char *path)
{
    unlink_fault = NULL;
    struct server_recovery_files c = {0};
    assert(!server_recovery_drop(&S, path, 8192, 1, &c));
    assert(c.dropped == 1);
    errno = ENOENT;
    return -1;
}
static void segment_unlink_race(void)
{
    char root[EFS_TEST_PATH], path[EFS_MAX_PATH + 128];
    setup(root, sizeof(root));
    uint64_t id = 0x5eba003;
    server_segment_path(&S, 0, id, 1, 0, path, sizeof(path));
    char dir[sizeof(path)]; strcpy(dir, path); *strrchr(dir, '/') = 0;
    assert(!sg_mkdirs(dir));
    file(path, 8192 + SG_TAIL);
    /* An unrelated allocation must survive both deleters' accounting. */
    char survivor[EFS_TEST_PATH + 32];
    snprintf(survivor, sizeof(survivor), "%s/survivor", root);
    file(survivor, 8192);
    S.nodes[0].used = 8192; /* target's acceptance credit already applied */
    S.reclaim_pending_bytes = 8192;
    rj.s = &S;
    struct rj_rec record = {.kind = EFS_RECLAIM_SEGMENT, .credited = 8192};
    unlink_fault = recovery_wins; /* after stat, before journal unlink */
    uint64_t removed = 0;
    assert(!server_segment_delete(&S, 0, id, 1, &removed));
    assert(removed == 0); /* Recovery owns the successful unlink and debit. */
    record.removed = removed;
    settle_record(&record);
    assert(used() == 8192 && S.reclaim_pending_bytes == 0);
    assert(!access(survivor, F_OK));
    puts("BA segment unlink race: loser owns no debit PASS");
}
static void recovery_sync_failure(void)
{
    char root[EFS_TEST_PATH], path[EFS_TEST_PATH + 32];
    setup(root, sizeof(root));
    snprintf(path, sizeof(path), "%s/target", root);
    file(path, 8192);
    S.nodes[0].used = 16384; /* another 8192-byte allocation remains live */
    struct server_recovery_files c = {0}, retry = {0};
    fail_sync = 1;
    assert(server_recovery_drop(&S, path, 8192, 1, &c) == EFS_ERR_IO);
    fail_sync = 0;
    /* The failure still reports IO; physical unlink ownership is neither
     * discarded by the failed barrier nor charged again on retry. */
    assert(used() == 8192 && c.dropped == 1 && c.bytes_dropped == 8192);
    assert(!server_recovery_drop(&S, path, 8192, 1, &retry));
    assert(used() == 8192 && !retry.dropped && !retry.bytes_dropped);
    puts("BA recovery failed namespace sync: physical debit once, IO retained PASS");
}
int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 &&
        (!strcmp(argv[1], "recovery") || !strcmp(argv[1], "segment") || !strcmp(argv[1], "sync"))));
    int failures = 0;
    for (int i = 0; i < 3; i++) {
        const char *name = i == 0 ? "recovery" : i == 1 ? "segment" : "sync";
        if (argc == 2 && strcmp(argv[1], name)) continue;
        pid_t p = fork(); assert(p >= 0);
        if (!p) { (i == 0 ? absent_recovery : i == 1 ? segment_unlink_race : recovery_sync_failure)(); exit(0); }
        int status; assert(waitpid(p, &status, 0) == p);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) failures++;
    }
    return failures ? 1 : 0;
}
