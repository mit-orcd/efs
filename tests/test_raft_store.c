/* Durable Raft-store tests: the on-disk efs_raft_store must be observably
 * identical to the in-memory one, and must not lose an fsynced record.
 *
 * Two properties get their own hard cases, because they are the ones a
 * plausible-looking implementation gets wrong: a torn tail must be discarded
 * WITHOUT discarding good records behind it, and a real crash (a process that
 * _exits without flushing) must still find everything that was fsynced. */
#include "efs/common.h"
#include "efs/raft.h"
#include "efs/raft_disk.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

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

static int st_append(struct efs_raft_store *s, uint64_t idx, uint64_t term,
                     const char *cmd)
{
    return s->append(s, idx, term, (const uint8_t *)cmd,
                     (uint32_t)strlen(cmd));
}

/* Reads index idx and compares term and payload. */
static int st_check(struct efs_raft_store *s, uint64_t idx, uint64_t term,
                    const char *cmd)
{
    uint8_t buf[512];
    uint32_t clen = sizeof(buf);
    uint64_t got_term = 0;

    if (s->get(s, idx, &got_term, buf, &clen) != EFS_OK)
        return 0;
    if (got_term != term)
        return 0;
    if (clen != strlen(cmd))
        return 0;
    return clen == 0 || memcmp(buf, cmd, clen) == 0;
}

/* --- semantics: disk must match raft_mem exactly --------------------- */

/* Runs the same script against both stores and compares every answer, so
 * the two implementations cannot drift. */
static void drive_pair(struct efs_raft_store *a, struct efs_raft_store *b)
{
    uint64_t ai, at, bi, bt;
    uint64_t t1, t2;
    uint8_t b1[64], b2[64];
    uint32_t c1, c2;
    int r1, r2, k;

    for (k = 1; k <= 6; k++) {
        char cmd[32];

        snprintf(cmd, sizeof(cmd), "cmd-%d", k);
        r1 = st_append(a, (uint64_t)k, 7, cmd);
        r2 = st_append(b, (uint64_t)k, 7, cmd);
        CHECK(r1 == r2 && r1 == EFS_OK, "append rc differs");
    }
    /* Gap must be rejected by both. */
    r1 = st_append(a, 99, 7, "gap");
    r2 = st_append(b, 99, 7, "gap");
    CHECK(r1 == r2 && r1 != EFS_OK, "gap append should fail on both");

    /* In-place overwrite of an existing slot. */
    r1 = st_append(a, 3, 9, "rewritten");
    r2 = st_append(b, 3, 9, "rewritten");
    CHECK(r1 == r2 && r1 == EFS_OK, "overwrite rc differs");
    CHECK(st_check(a, 3, 9, "rewritten") && st_check(b, 3, 9, "rewritten"),
          "overwrite not visible");

    a->last(a, &ai, &at);
    b->last(b, &bi, &bt);
    CHECK(ai == bi && at == bt && ai == 6, "last differs");

    r1 = a->truncate_from(a, 5);
    r2 = b->truncate_from(b, 5);
    CHECK(r1 == r2 && r1 == EFS_OK, "truncate rc differs");
    a->last(a, &ai, &at);
    b->last(b, &bi, &bt);
    CHECK(ai == bi && ai == 4, "last after truncate differs");
    c1 = sizeof(b1);
    c2 = sizeof(b2);
    r1 = a->get(a, 5, &t1, b1, &c1);
    r2 = b->get(b, 5, &t2, b2, &c2);
    CHECK(r1 == r2 && r1 == EFS_ERR_NOT_FOUND, "truncated index still there");

    /* Short buffer: same rc and same required length from both. */
    c1 = 2;
    c2 = 2;
    r1 = a->get(a, 1, &t1, b1, &c1);
    r2 = b->get(b, 1, &t2, b2, &c2);
    CHECK(r1 == r2 && c1 == c2 && c1 == 5, "short-buffer answer differs");

    r1 = a->save_snap(a, 2, 7);
    r2 = b->save_snap(b, 2, 7);
    CHECK(r1 == r2 && r1 == EFS_OK, "save_snap rc differs");
    /* The snapshot index itself still answers, with an empty command. */
    CHECK(st_check(a, 2, 7, "") && st_check(b, 2, 7, ""),
          "snap index should report its term");
    c1 = sizeof(b1);
    c2 = sizeof(b2);
    r1 = a->get(a, 1, &t1, b1, &c1);
    r2 = b->get(b, 1, &t2, b2, &c2);
    CHECK(r1 == r2 && r1 == EFS_ERR_NOT_FOUND, "compacted prefix differs");
    CHECK(st_check(a, 3, 9, "rewritten") && st_check(b, 3, 9, "rewritten"),
          "entry above snap lost");

    r1 = a->truncate_from(a, 2);
    r2 = b->truncate_from(b, 2);
    CHECK(r1 == r2 && r1 != EFS_OK, "truncate into snapshot should fail");

    a->save_hard(a, 11, 3);
    b->save_hard(b, 11, 3);
    a->load_hard(a, &ai, (int32_t *)&at);
    b->load_hard(b, &bi, (int32_t *)&bt);
    CHECK(ai == bi && ai == 11, "hard state differs");

    /* InstallSnapshot past the log end (a follower whose log never held the
     * snapshotted prefix): raft_mem clamps the drop, disk must too — the
     * old disk guard rejected this with INVAL and wedged live catch-up. */
    r1 = a->save_snap(a, 100, 9);
    r2 = b->save_snap(b, 100, 9);
    CHECK(r1 == r2 && r1 == EFS_OK, "skip-ahead save_snap rc differs");
    a->last(a, &ai, &at);
    b->last(b, &bi, &bt);
    CHECK(ai == bi && ai == 100 && at == 9, "last after skip-ahead snap");
    c1 = sizeof(b1);
    c2 = sizeof(b2);
    r1 = a->get(a, 3, &t1, b1, &c1);
    r2 = b->get(b, 3, &t2, b2, &c2);
    CHECK(r1 == r2 && r1 == EFS_ERR_NOT_FOUND,
          "skip-ahead snap left old entries");
    /* A backwards snapshot is still invalid on both. */
    r1 = a->save_snap(a, 50, 9);
    r2 = b->save_snap(b, 50, 9);
    CHECK(r1 == r2 && r1 != EFS_OK, "backwards save_snap should fail");
}

static void test_matches_mem(void)
{
    struct efs_raft_store *mem = efs_raft_mem_create();
    struct efs_raft_disk *d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_NOSYNC);
    struct efs_raft_store *disk;
    uint32_t o = 0, n = 0;

    CHECK(mem != NULL && d != NULL, "open failed");
    if (!mem || !d)
        return;
    disk = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(disk != NULL, "group failed");
    if (!disk)
        return;
    /* Never saved: both must say NOT_FOUND, not "zero". */
    CHECK(mem->load_cfg(mem, &o, &n) == EFS_ERR_NOT_FOUND &&
              disk->load_cfg(disk, &o, &n) == EFS_ERR_NOT_FOUND,
          "unsaved cfg should be NOT_FOUND");
    drive_pair(mem, disk);
    efs_raft_mem_free(mem);
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

/* --- durability ------------------------------------------------------ */

static void test_reopen(void)
{
    struct efs_raft_disk *d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    struct efs_raft_store *s;
    uint64_t idx = 0, term = 0;
    int32_t vote = 0;
    uint32_t o = 0, n = 0;

    CHECK(d != NULL, "open failed");
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(st_append(s, 1, 4, "alpha") == EFS_OK, "append 1");
    CHECK(st_append(s, 2, 4, "beta") == EFS_OK, "append 2");
    CHECK(st_append(s, 3, 5, "gamma") == EFS_OK, "append 3");
    CHECK(s->save_hard(s, 5, 2) == EFS_OK, "save_hard");
    CHECK(s->save_cfg(s, 7, 0) == EFS_OK, "save_cfg");
    /* A second group on the SAME log, to prove the multiplexing survives. */
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_CTRL);
    CHECK(st_append(s, 1, 2, "ctrl-one") == EFS_OK, "ctrl append");
    CHECK(s->save_hard(s, 2, 1) == EFS_OK, "ctrl save_hard");
    efs_raft_disk_close(d);

    d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    CHECK(d != NULL, "reopen failed");
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(st_check(s, 1, 4, "alpha"), "entry 1 lost");
    CHECK(st_check(s, 2, 4, "beta"), "entry 2 lost");
    CHECK(st_check(s, 3, 5, "gamma"), "entry 3 lost");
    CHECK(s->last(s, &idx, &term) == EFS_OK && idx == 3 && term == 5,
          "last wrong after reopen");
    CHECK(s->load_hard(s, &term, &vote) == EFS_OK && term == 5 && vote == 2,
          "hard state lost");
    CHECK(s->load_cfg(s, &o, &n) == EFS_OK && o == 7 && n == 0, "cfg lost");
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_CTRL);
    CHECK(st_check(s, 1, 2, "ctrl-one"), "ctrl entry lost");
    CHECK(s->load_hard(s, &term, &vote) == EFS_OK && term == 2 && vote == 1,
          "ctrl hard state lost or crossed with the other group");
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

/* A truncate must survive a restart: if it does not, a follower that removed
 * conflicting entries gets them back and diverges from the leader. */
static void test_truncate_persists(void)
{
    struct efs_raft_disk *d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    struct efs_raft_store *s;
    uint64_t idx = 0, term = 0;

    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    st_append(s, 1, 1, "keep");
    st_append(s, 2, 1, "drop-me");
    st_append(s, 3, 1, "drop-me-too");
    CHECK(s->truncate_from(s, 2) == EFS_OK, "truncate");
    st_append(s, 2, 9, "replacement");
    efs_raft_disk_close(d);

    d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(s->last(s, &idx, &term) == EFS_OK && idx == 2 && term == 9,
          "truncate did not persist");
    CHECK(st_check(s, 1, 1, "keep"), "entry below truncate lost");
    CHECK(st_check(s, 2, 9, "replacement"), "replacement lost");
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

/* --- crash recovery -------------------------------------------------- */

static void test_torn_tail(void)
{
    struct efs_raft_disk *d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    struct efs_raft_store *s;
    char path[600];
    uint64_t idx = 0, term = 0;
    int fd;

    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    st_append(s, 1, 1, "one");
    st_append(s, 2, 1, "two");
    efs_raft_disk_close(d);

    /* Append a half-written record, the way a crash mid-write leaves one. */
    snprintf(path, sizeof(path), "%s/raft.log", g_dir);
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0, "open log");
    if (fd >= 0) {
        const char junk[] = "\x00\x00\x01\x00\x00\x00\x00\x40partial";
        CHECK(write(fd, junk, sizeof(junk) - 1) > 0, "write partial");
        close(fd);
    }
    d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    CHECK(d != NULL, "open must recover, not refuse, a torn tail");
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(s->last(s, &idx, &term) == EFS_OK && idx == 2,
          "good records behind the torn tail were lost");
    CHECK(st_check(s, 2, 1, "two"), "entry 2 lost");
    /* The tail must be gone, so the next append lands at a clean boundary. */
    CHECK(st_append(s, 3, 1, "three") == EFS_OK, "append after torn tail");
    efs_raft_disk_close(d);
    d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(st_check(s, 3, 1, "three"), "post-recovery append did not persist");
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

/* _exit() in a child: no atexit, no stdio flush, no clean close. Anything
 * that comes back was on the device because fsync put it there. */
static void test_crash_durability(void)
{
    struct efs_raft_disk *d;
    struct efs_raft_store *s;
    uint64_t idx = 0, term = 0;
    pid_t pid = fork();
    int status = 0;

    CHECK(pid >= 0, "fork");
    if (pid < 0)
        return;
    if (pid == 0) {
        d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
        if (!d)
            _exit(2);
        s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
        if (st_append(s, 1, 3, "survives") != EFS_OK)
            _exit(3);
        if (s->save_hard(s, 3, 1) != EFS_OK)
            _exit(4);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child failed");
    d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_SYNC);
    CHECK(d != NULL, "open after crash");
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(s->last(s, &idx, &term) == EFS_OK && idx == 1 && term == 3,
          "fsynced entry did not survive a crash");
    CHECK(st_check(s, 1, 3, "survives"), "fsynced payload lost");
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

/* --- rotation -------------------------------------------------------- */

/* The log must not grow without bound as snapshots make records dead, and
 * rotation must preserve the live state exactly. */
static void test_rotation(void)
{
    struct efs_raft_disk *d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_NOSYNC);
    struct efs_raft_store *s;
    uint64_t before, after, idx = 0, term = 0;
    char cmd[256];
    int k;

    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    memset(cmd, 'x', sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    for (k = 1; k <= 400; k++)
        CHECK(st_append(s, (uint64_t)k, 1, cmd) == EFS_OK, "bulk append");
    /* Snapshot through 399: only index 400 stays live. */
    CHECK(s->save_snap(s, 399, 1) == EFS_OK, "save_snap");
    before = efs_raft_disk_bytes(d);
    CHECK(efs_raft_disk_rotate(d) == EFS_OK, "rotate");
    after = efs_raft_disk_bytes(d);
    CHECK(after < before, "rotation did not shrink the log");
    CHECK(s->last(s, &idx, &term) == EFS_OK && idx == 400,
          "rotation lost the live tail");
    CHECK(st_check(s, 400, 1, cmd), "rotation lost the live payload");
    efs_raft_disk_close(d);

    /* And the rotated file must be what a restart reads. */
    d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_NOSYNC);
    CHECK(d != NULL, "reopen after rotation");
    if (!d)
        return;
    s = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    CHECK(s->last(s, &idx, &term) == EFS_OK && idx == 400,
          "rotated log did not replay");
    CHECK(st_check(s, 400, 1, cmd), "rotated payload did not replay");
    CHECK(s->get(s, 5, &term, NULL, (uint32_t *)&idx) == EFS_ERR_NOT_FOUND,
          "compacted entry came back");
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

/* --- the real state machine on the durable store --------------------- */

/* A single-node group must elect itself, commit, and — after a restart that
 * keeps only the store — still know its term, its vote and its log. */
/* A single-voter group never sends, but efs_raft_new still requires a sink. */
static int drop_send(void *net, const struct efs_raft_msg *msg)
{
    (void)net;
    (void)msg;
    return EFS_OK;
}

static int count_apply(void *app, uint64_t index, uint64_t term,
                       const uint8_t *cmd, uint32_t clen)
{
    (void)index;
    (void)term;
    (void)cmd;
    (void)clen;
    (*(int *)app)++;
    return EFS_OK;
}

static void test_raft_on_disk_store(void)
{
    struct efs_raft_disk *d = efs_raft_disk_open(g_dir, EFS_RAFT_DISK_NOSYNC);
    struct efs_raft_store *store;
    struct efs_raft_cfg cfg;
    struct efs_raft *r;
    int applied = 0, i;
    uint64_t commit;

    if (!d)
        return;
    store = efs_raft_disk_group(d, EFS_RAFT_GROUP_SHARD);
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = 0;
    cfg.n = 1;
    cfg.voters = 1;
    cfg.election_ticks = 2;
    cfg.heartbeat_ticks = 1;
    cfg.boot_id = 1;
    cfg.group = EFS_RAFT_GROUP_SHARD;
    cfg.store = store;
    cfg.store_ctx = store;
    cfg.send = drop_send;
    cfg.apply = count_apply;
    cfg.app = &applied;
    r = efs_raft_new(&cfg);
    CHECK(r != NULL, "raft_new on the disk store");
    if (!r) {
        efs_raft_disk_close(d);
        return;
    }
    for (i = 0; i < 8 && efs_raft_role(r) != EFS_RAFT_LEADER; i++)
        efs_raft_tick(r);
    CHECK(efs_raft_role(r) == EFS_RAFT_LEADER, "single node did not lead");
    for (i = 0; i < 5; i++) {
        uint8_t cmd[8] = { 'o', 'p', (uint8_t)('0' + i), 0, 0, 0, 0, 0 };

        CHECK(efs_raft_propose(r, cmd, 3, NULL) == EFS_OK, "propose");
    }
    for (i = 0; i < 8; i++)
        efs_raft_tick(r);
    commit = efs_raft_commit(r);
    CHECK(commit >= 5, "commands did not commit");
    CHECK(applied >= 5, "commands did not apply");
    efs_raft_free(r);

    /* Restart: same store, new boot_id, nothing else carried over. */
    applied = 0;
    cfg.boot_id = 2;
    r = efs_raft_new(&cfg);
    CHECK(r != NULL, "raft_new after restart");
    if (!r) {
        efs_raft_disk_close(d);
        return;
    }
    CHECK(efs_raft_term(r) > 0, "term lost across restart");
    for (i = 0; i < 8 && efs_raft_role(r) != EFS_RAFT_LEADER; i++)
        efs_raft_tick(r);
    for (i = 0; i < 8; i++)
        efs_raft_tick(r);
    /* The log survived, so the same commands re-apply from the snapshot
     * point — which is exactly why apply must be idempotent. */
    CHECK(efs_raft_commit(r) >= commit, "commit index went backwards");
    CHECK(applied >= 5, "log did not replay into the state machine");
    efs_raft_free(r);
    efs_raft_disk_close(d);
    rmtree(g_dir);
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/efs-raftstore-test-%d", (int)getpid());
    rmtree(g_dir);

    test_matches_mem();
    test_reopen();
    test_truncate_persists();
    test_torn_tail();
    test_crash_durability();
    test_rotation();
    test_raft_on_disk_store();

    rmtree(g_dir);
    if (failures) {
        fprintf(stderr, "test_raft_store: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_raft_store: OK\n");
    return 0;
}
