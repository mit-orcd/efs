/* W153: the committed formation membership (efs/membership.h) and the
 * member store (src/server/member_store.c): formation input from the
 * joined node table (two members refused, three and five committed, gaps
 * and shared endpoints refused), record encoding and every malformed
 * record refused, the voters derived from the committed count, and the
 * store's ownership lock, durable boot counter and persisted membership. */
#include "efs/membership.h"
#include "efs/raft.h"
#include "efs/checksum.h"
#include "../src/server/member_store.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int checks, failures;
#define CHECK(c, what) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static void table(struct efs_node *t, uint32_t n, const uint32_t *ids)
{
    memset(t, 0, sizeof(*t) * EFS_MAX_NODES);
    for (uint32_t i = 0; i < n; i++) {
        t[i].id = ids[i];
        snprintf(t[i].addr, sizeof(t[i].addr), "10.0.0.%u", ids[i]);
        t[i].port = 18600;
    }
}

static void formation_input(void)
{
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m;
    char err[1024];

    const uint32_t two[] = {1, 2};
    table(t, 2, two);
    CHECK(efs_membership_from_nodes(t, 2, &m, err, sizeof(err)) == EFS_ERR_INVAL, "two refused");
    CHECK(strstr(err, "at least 3") && strstr(err, "missing: node 3") &&
          strstr(err, "node 1 at 10.0.0.1:18600") && strstr(err, "node 2 at 10.0.0.2:18600"), err);

    const uint32_t three[] = {2, 3, 1}; /* table order is irrelevant */
    table(t, 3, three);
    CHECK(efs_membership_from_nodes(t, 3, &m, err, sizeof(err)) == EFS_OK && m.count == 3 &&
          m.m[0].id == 1 && m.m[2].id == 3 && !strcmp(m.m[0].addr, "10.0.0.1"), "three");
    CHECK(efs_md_group_voters(EFS_RAFT_GROUP_SHARD, (int)m.count) == 0x7 &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m.count) == 0x7, "three: both on 1..3");

    const uint32_t five[] = {1, 2, 3, 4, 5};
    table(t, 5, five);
    CHECK(efs_membership_from_nodes(t, 5, &m, err, sizeof(err)) == EFS_OK && m.count == 5, "five");
    CHECK(efs_md_group_voters(EFS_RAFT_GROUP_SHARD, (int)m.count) == 0x7 &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m.count) == 0xe, "five: {1,2,3} {2,3,4}");

    const uint32_t gap[] = {1, 2, 4};
    table(t, 3, gap);
    CHECK(efs_membership_from_nodes(t, 3, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "missing: node 3"), err);

    table(t, 3, three);
    t[1].port = t[0].port;
    strcpy(t[1].addr, t[0].addr);
    CHECK(efs_membership_from_nodes(t, 3, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "same endpoint"), err);

    table(t, 3, three);
    t[2].id = 2;
    CHECK(efs_membership_from_nodes(t, 3, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "twice"), err);

    table(t, 3, three);
    t[0].port = 0;
    CHECK(efs_membership_from_nodes(t, 3, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "unusable endpoint"), err);
    table(t, 0, NULL);
    CHECK(efs_membership_from_nodes(t, 0, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "joined: none") && strstr(err, "missing: node 1, node 2, node 3"), err);
}

static void record(void)
{
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m, back;
    uint8_t buf[EFS_MEMBERSHIP_MAX_LEN + 8], bad[EFS_MEMBERSHIP_MAX_LEN + 8];
    size_t len = 0;
    const uint32_t five[] = {1, 2, 3, 4, 5};

    table(t, 5, five);
    CHECK(efs_membership_from_nodes(t, 5, &m, NULL, 0) == EFS_OK, "five");
    CHECK(efs_membership_encode(&m, buf, sizeof(buf), &len) == EFS_OK &&
          len == 2 + 5 * (7 + strlen("10.0.0.1")), "encode");
    CHECK(efs_membership_decode(buf, len, &back) == EFS_OK && efs_membership_equal(&m, &back),
          "round trip");
    CHECK(efs_membership_find(&back, 4) && efs_membership_find(&back, 4)->port == 18600 &&
          !efs_membership_find(&back, 6) && !efs_membership_find(&back, 0), "find");
    CHECK(efs_membership_encode(&m, buf, len - 1, &len) == EFS_ERR_INVAL, "short out");
    efs_membership_encode(&m, buf, sizeof(buf), &len);

    /* Every malformed record is refused, never misread. */
    memcpy(bad, buf, len);
    bad[0] = 2;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_UNSUPPORTED, "other version");
    memcpy(bad, buf, len);
    bad[1] = 2;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "count below 3");
    bad[1] = EFS_MAX_NODES + 1;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "count above max");
    CHECK(efs_membership_decode(buf, len - 1, &back) == EFS_ERR_DECODE, "truncated");
    memcpy(bad, buf, len);
    bad[len] = 0;
    CHECK(efs_membership_decode(bad, len + 1, &back) == EFS_ERR_DECODE, "trailing byte");
    memcpy(bad, buf, len);
    bad[2 + 3] = 9; /* member 1 claims id 9 */
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "id out of order");
    memcpy(bad, buf, len);
    bad[2 + 4] = bad[2 + 5] = 0;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "port 0");
    memcpy(bad, buf, len);
    bad[2 + 7] = ' ';
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "unprintable address");
    memcpy(bad, buf, len);
    memcpy(bad + 2 + 15 + 7, bad + 2 + 7, 8); /* member 2's address = member 1's */
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "shared endpoint");
    CHECK(efs_membership_decode(buf, 1, &back) == EFS_ERR_DECODE, "header only");
}

static int write_file(const char *dir, const char *name, const void *b, size_t n)
{
    char p[4200];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, b, n);
    close(fd);
    return w == (ssize_t)n ? 0 : -1;
}

static ssize_t read_file(const char *dir, const char *name, void *b, size_t n)
{
    char p[4200];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, b, n);
    close(fd);
    return r;
}

static void store(void)
{
    char base[EFS_TEST_PATH], dir[4200], err[1024];
    struct member_store ms, ms2;
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m3, m4;
    uint64_t inc = 0;
    uint8_t good[EFS_BOOT_FILE_LEN], bad[EFS_BOOT_FILE_LEN], now[EFS_BOOT_FILE_LEN + 4];
    uint8_t ident[EFS_IDENTITY_FILE_LEN];
    const uint32_t ids[] = {1, 2, 3, 4};

    efs_test_path(base, sizeof(base), "efs-w153-store-XXXXXX");
    if (!efs_test_mkdtemp(base)) { perror("mkdtemp"); exit(1); }
    snprintf(dir, sizeof(dir), "%s/mdraft", base);
    table(t, 3, ids);
    efs_membership_from_nodes(t, 3, &m3, NULL, 0);
    table(t, 4, ids);
    efs_membership_from_nodes(t, 4, &m4, NULL, 0);

    /* Fresh store: initialized unformed, boot counter max(fresh, saved+1). */
    CHECK(member_store_open(&ms, dir, 2, err, sizeof(err)) == EFS_OK && ms.created &&
          ms.state == EFS_MEMBER_UNFORMED, err);
    CHECK(member_store_boot(&ms, 5, &inc, err, sizeof(err)) == EFS_OK && inc == 5, "boot fresh");
    /* The exclusive lock: a second owner (another process) is refused. */
    pid_t pid = fork();
    if (pid == 0) {
        struct member_store o;
        char e[512];
        _exit(member_store_open(&o, dir, 2, e, sizeof(e)) == EFS_ERR_BUSY &&
              strstr(e, "owned by another running process") ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "second owner refused");
    CHECK(member_store_boot(&ms, 3, &inc, err, sizeof(err)) == EFS_OK && inc == 6, "saved+1");
    CHECK(member_store_boot(&ms, 1000, &inc, err, sizeof(err)) == EFS_OK && inc == 1000, "fresh");
    member_store_close(&ms);

    /* Restart: the counter persists across starts; another node id is fenced. */
    CHECK(member_store_open(&ms, dir, 2, err, sizeof(err)) == EFS_OK && !ms.created, err);
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_OK && inc == 1001, "restart");
    member_store_close(&ms);
    CHECK(member_store_open(&ms2, dir, 3, err, sizeof(err)) == EFS_ERR_FENCED &&
          strstr(err, "belongs to node 2"), err);

    /* Formation: persisted once; the same record is OK, another FENCED. */
    CHECK(member_store_open(&ms, dir, 2, err, sizeof(err)) == EFS_OK, err);
    CHECK(member_store_learn(&ms, &m3, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMED, err);
    CHECK(member_store_learn(&ms, &m3, err, sizeof(err)) == EFS_OK, "same record again");
    CHECK(member_store_learn(&ms, &m4, err, sizeof(err)) == EFS_ERR_FENCED, "other record");
    member_store_close(&ms);
    /* Restart derives the count (and so the voters) from the record. */
    CHECK(member_store_open(&ms, dir, 2, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMED && ms.members.count == 3 &&
          efs_membership_equal(&ms.members, &m3) &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)ms.members.count) == 0x7, err);

    /* Boot counter: corrupt, short, other version, missing and exhausted
     * counters are refused and left as they were. */
    CHECK(read_file(dir, "boot.bin", good, sizeof(good)) == EFS_BOOT_FILE_LEN, "boot.bin size");
    memcpy(bad, good, sizeof(bad));
    bad[17] ^= 1;
    write_file(dir, "boot.bin", bad, sizeof(bad));
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_CHECKSUM, "corrupt counter");
    CHECK(read_file(dir, "boot.bin", now, sizeof(now)) == sizeof(bad) && !memcmp(now, bad, sizeof(bad)),
          "corrupt counter left alone");
    write_file(dir, "boot.bin", good, 12);
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_DECODE, "short counter");
    memcpy(bad, good, sizeof(bad));
    bad[11] = 9;
    write_file(dir, "boot.bin", bad, sizeof(bad));
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_UNSUPPORTED, "other version");
    member_store_boot_encode(UINT64_MAX, bad);
    write_file(dir, "boot.bin", bad, sizeof(bad));
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_OVERFLOW, "exhausted");
    {
        char p[4300];
        snprintf(p, sizeof(p), "%s/boot.bin", dir);
        unlink(p);
        CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_NOT_FOUND, "missing");
    }
    write_file(dir, "boot.bin", good, sizeof(good));
    member_store_close(&ms);

    /* identity.bin: a corrupt or truncated identity, or a malformed record
     * under a valid checksum, is refused. */
    CHECK(read_file(dir, "identity.bin", ident, sizeof(ident)) == EFS_IDENTITY_FILE_LEN, "identity size");
    {
        uint8_t c[EFS_IDENTITY_FILE_LEN];
        memcpy(c, ident, sizeof(c));
        c[30] ^= 1;
        write_file(dir, "identity.bin", c, sizeof(c));
        CHECK(member_store_open(&ms2, dir, 2, err, sizeof(err)) == EFS_ERR_UNSUPPORTED, "corrupt identity");
        write_file(dir, "identity.bin", ident, 40);
        CHECK(member_store_open(&ms2, dir, 2, err, sizeof(err)) == EFS_ERR_UNSUPPORTED, "short identity");
        memcpy(c, ident, sizeof(c));
        c[24 + 1] = 2; /* record count 2, checksum recomputed */
        efs_hash(c, EFS_IDENTITY_FILE_LEN - 32, c + EFS_IDENTITY_FILE_LEN - 32);
        write_file(dir, "identity.bin", c, sizeof(c));
        CHECK(member_store_open(&ms2, dir, 2, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "disagrees"), err);
        write_file(dir, "identity.bin", ident, sizeof(ident));
        CHECK(member_store_open(&ms2, dir, 2, err, sizeof(err)) == EFS_OK, err);
        member_store_close(&ms2);
    }

    /* Interrupted initialization and another build's store are refused. */
    {
        char p[4300];
        snprintf(p, sizeof(p), "%s/identity.bin", dir);
        unlink(p);
        CHECK(member_store_open(&ms2, dir, 2, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "interrupted"), err);
        snprintf(p, sizeof(p), "%s/boot.bin", dir);
        unlink(p);
        snprintf(p, sizeof(p), "%s/kv", dir);
        mkdir(p, 0755);
        CHECK(member_store_open(&ms2, dir, 2, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "another build"), err);
        rmdir(p);
    }
}

int main(void)
{
    formation_input();
    record();
    store();
    if (failures) {
        fprintf(stderr, "membership: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("membership: formation input (2 refused, 3 and 5 committed, gaps/duplicates "
           "refused), record encoding and malformed records, voters from the committed "
           "count, store lock, boot counter and persisted membership: %d checks PASS\n", checks);
    return 0;
}
