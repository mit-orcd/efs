/* W153/K33: the committed membership (efs/membership.h) and the member
 * store (src/server/member_store.c): formation input from the joined node
 * table (two members refused, three and five committed, the contiguous ids
 * taken, shared endpoints and tokens refused), record encoding and every
 * malformed record refused, revisions (one-member extensions, prefix
 * order), the group-anchor apply rules, the voters derived from the
 * committed count, the store's ownership lock, durable boot counter,
 * token, the K33 joining / seed / promised / formed identities (M4') and
 * persisted revisions, and the discovery datagrams and subnet broadcast
 * addresses (src/server/discovery.c). */
#include "efs/membership.h"
#include "efs/kv.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/checksum.h"
#include "../src/server/member_store.h"
#include "../src/server/discovery.h"
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
        t[i].token = 0x1000u + ids[i];
    }
}

static void formation_input(void)
{
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m;
    char err[1024];

    const uint32_t two[] = {1, 2};
    table(t, 2, two);
    CHECK(efs_membership_from_nodes(t, 2, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL, "two refused");
    CHECK(strstr(err, "at least 3") && strstr(err, "missing: node 3") &&
          strstr(err, "node 1 at 10.0.0.1:18600") && strstr(err, "node 2 at 10.0.0.2:18600"), err);

    const uint32_t three[] = {2, 3, 1}; /* table order is irrelevant */
    table(t, 3, three);
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_OK && m.count == 3 &&
          m.formed == 3 && m.cluster == 77 && m.m[0].id == 1 && m.m[2].id == 3 &&
          m.m[2].token == 0x1003 && !strcmp(m.m[0].addr, "10.0.0.1"), "three");
    CHECK(efs_md_group_voters(EFS_RAFT_GROUP_SHARD, (int)m.count) == 0x7 &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m.count) == 0x7, "three: both on 1..3");

    const uint32_t five[] = {1, 2, 3, 4, 5};
    table(t, 5, five);
    CHECK(efs_membership_from_nodes(t, 5, 77, &m, err, sizeof(err)) == EFS_OK && m.count == 5, "five");
    CHECK(efs_md_group_voters(EFS_RAFT_GROUP_SHARD, (int)m.count) == 0x7 &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m.count) == 0xe, "five: {1,2,3} {2,3,4}");

    const uint32_t gap[] = {1, 2, 4};
    table(t, 3, gap);
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "missing: node 3"), err);
    /* K33: the contiguous ids form; node 5 after the gap joins later. */
    const uint32_t later[] = {5, 1, 2, 3};
    table(t, 4, later);
    CHECK(efs_membership_from_nodes(t, 4, 77, &m, err, sizeof(err)) == EFS_OK && m.count == 3,
          "contiguous prefix");
    table(t, 3, three);
    t[1].token = t[0].token;
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "same store token"), err);
    table(t, 3, three);
    t[2].token = 0;
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "store token"), err);
    table(t, 3, three);
    CHECK(efs_membership_from_nodes(t, 3, 0, &m, err, sizeof(err)) == EFS_ERR_INVAL, "no cluster");

    table(t, 3, three);
    t[1].port = t[0].port;
    strcpy(t[1].addr, t[0].addr);
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "same endpoint"), err);

    table(t, 3, three);
    t[2].id = 2;
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "twice"), err);

    table(t, 3, three);
    t[0].port = 0;
    CHECK(efs_membership_from_nodes(t, 3, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "unusable endpoint"), err);
    table(t, 0, NULL);
    CHECK(efs_membership_from_nodes(t, 0, 77, &m, err, sizeof(err)) == EFS_ERR_INVAL &&
          strstr(err, "joined: none") && strstr(err, "missing: node 1, node 2, node 3"), err);
}

static void record(void)
{
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m, back;
    uint8_t buf[EFS_MEMBERSHIP_MAX_LEN + 8], bad[EFS_MEMBERSHIP_MAX_LEN + 8];
    size_t len = 0;
    const uint32_t five[] = {1, 2, 3, 4, 5};
    const size_t H = 11, E = 15 + 8; /* header; one member with "10.0.0.N" */

    table(t, 5, five);
    CHECK(efs_membership_from_nodes(t, 5, 77, &m, NULL, 0) == EFS_OK, "five");
    CHECK(efs_membership_encode(&m, buf, sizeof(buf), &len) == EFS_OK && len == H + 5 * E,
          "encode");
    CHECK(efs_membership_decode(buf, len, &back) == EFS_OK && efs_membership_equal(&m, &back) &&
          back.formed == 5 && back.cluster == 77, "round trip");
    CHECK(efs_membership_find(&back, 4) && efs_membership_find(&back, 4)->port == 18600 &&
          efs_membership_find(&back, 4)->token == 0x1004 &&
          !efs_membership_find(&back, 6) && !efs_membership_find(&back, 0), "find");
    CHECK(efs_membership_find_ep(&back, "10.0.0.2", 18600) == &back.m[1] &&
          !efs_membership_find_ep(&back, "10.0.0.2", 18601), "find by endpoint");
    CHECK(efs_membership_encode(&m, buf, len - 1, &len) == EFS_ERR_INVAL, "short out");
    efs_membership_encode(&m, buf, sizeof(buf), &len);

    /* Every malformed record is refused, never misread. */
    memcpy(bad, buf, len);
    bad[0] = 1;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_UNSUPPORTED, "other version");
    memcpy(bad, buf, len);
    bad[2] = 2;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "formed below 3");
    bad[2] = 6;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "formed above count");
    memcpy(bad, buf, len);
    bad[1] = EFS_MAX_NODES + 1;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "count above max");
    memcpy(bad, buf, len);
    memset(bad + 3, 0, 8);
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "cluster 0");
    CHECK(efs_membership_decode(buf, len - 1, &back) == EFS_ERR_DECODE, "truncated");
    memcpy(bad, buf, len);
    bad[len] = 0;
    CHECK(efs_membership_decode(bad, len + 1, &back) == EFS_ERR_DECODE, "trailing byte");
    memcpy(bad, buf, len);
    bad[H + 3] = 9; /* member 1 claims id 9 */
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "id out of order");
    memcpy(bad, buf, len);
    bad[H + 4] = bad[H + 5] = 0;
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "port 0");
    memcpy(bad, buf, len);
    memset(bad + H + 6, 0, 8);
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "token 0");
    memcpy(bad, buf, len);
    memcpy(bad + H + E + 6, bad + H + 6, 8); /* member 2's token = member 1's */
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "shared token");
    memcpy(bad, buf, len);
    bad[H + 15] = ' ';
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "unprintable address");
    memcpy(bad, buf, len);
    memcpy(bad + H + E + 15, bad + H + 15, 8); /* member 2's address = member 1's */
    CHECK(efs_membership_decode(bad, len, &back) == EFS_ERR_DECODE, "shared endpoint");
    CHECK(efs_membership_decode(buf, 2, &back) == EFS_ERR_DECODE, "header only");
}

/* K33 (M3): revisions extend by one member; prefix order decides history. */
static void revisions(void)
{
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m3, m4, m4b, m5, other;
    char err[512];
    const uint32_t ids[] = {1, 2, 3};

    table(t, 3, ids);
    efs_membership_from_nodes(t, 3, 77, &m3, NULL, 0);
    CHECK(efs_membership_plus(&m3, "10.0.0.4", 18600, 0x1004, &m4, err, sizeof(err)) == EFS_OK &&
          m4.count == 4 && m4.formed == 3 && m4.m[3].id == 4 && m4.m[3].token == 0x1004, err);
    CHECK(efs_membership_plus(&m4, "10.0.0.5", 18600, 0x1005, &m5, err, sizeof(err)) == EFS_OK &&
          m5.count == 5, err);
    CHECK(efs_membership_plus(&m3, "10.0.0.9", 18600, 0x1009, &m4b, err, sizeof(err)) == EFS_OK,
          "competing id 4");
    CHECK(efs_membership_plus(&m3, "10.0.0.2", 18600, 0x2222, &other, err, sizeof(err)) ==
          EFS_ERR_INVAL && strstr(err, "already a member"), "endpoint taken");
    CHECK(efs_membership_plus(&m3, "10.0.0.9", 18600, 0x1002, &other, err, sizeof(err)) ==
          EFS_ERR_INVAL, "token taken");
    CHECK(efs_membership_extends(&m5, &m3) && efs_membership_extends(&m5, &m4) &&
          efs_membership_extends(&m4, &m4) && !efs_membership_extends(&m3, &m4), "prefix order");
    CHECK(!efs_membership_extends(&m4b, &m4) && !efs_membership_extends(&m4, &m4b) &&
          efs_membership_extends(&m4b, &m3), "competing revisions diverge");
    other = m4;
    other.cluster = 78;
    CHECK(!efs_membership_extends(&other, &m3), "another cluster");
    other = m4;
    other.formed = 4;
    CHECK(!efs_membership_extends(&other, &m3), "another formation");
    CHECK(efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m3.count) == 0x7 &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m4.count) == 0xe &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)m5.count) == 0xe &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD, (int)m5.count) == 0x7,
          "SHARD2 {1,2,3} -> {2,3,4} at four; five adds storage only");

    /* The anchor apply rules (MKFS keep first, MEMBERS +1, SALT copy). */
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t r3[EFS_MEMBERSHIP_MAX_LEN], r4[EFS_MEMBERSHIP_MAX_LEN], r4b[EFS_MEMBERSHIP_MAX_LEN],
        r5[EFS_MEMBERSHIP_MAX_LEN], got[EFS_MEMBERSHIP_MAX_LEN];
    size_t l3, l4, l4b, l5;
    uint32_t gl = 0;
    efs_membership_encode(&m3, r3, sizeof(r3), &l3);
    efs_membership_encode(&m4, r4, sizeof(r4), &l4);
    efs_membership_encode(&m4b, r4b, sizeof(r4b), &l4b);
    efs_membership_encode(&m5, r5, sizeof(r5), &l5);
    const int one = EFS_MEMBERSHIP_EXTEND_ONE, any = EFS_MEMBERSHIP_EXTEND_ANY;
    CHECK(kv && efs_meta_apply_membership_record(kv, 1, r4, (uint32_t)l4, one) == EFS_ERR_UNFORMED,
          "MEMBERS before formation");
    CHECK(efs_meta_apply_membership_record(kv, 1, r3, (uint32_t)l3, EFS_MEMBERSHIP_KEEP_FIRST) ==
          EFS_OK, "MKFS");
    CHECK(efs_meta_apply_membership_record(kv, 1, r5, (uint32_t)l5, one) == EFS_ERR_FENCED,
          "MEMBERS skipping a revision");
    CHECK(efs_meta_apply_membership_record(kv, 1, r4, (uint32_t)l4, one) == EFS_OK, "MEMBERS +1");
    CHECK(efs_meta_apply_membership_record(kv, 1, r4, (uint32_t)l4, one) == EFS_OK, "retry");
    CHECK(efs_meta_apply_membership_record(kv, 1, r4b, (uint32_t)l4b, one) == EFS_ERR_FENCED,
          "competing id 4 loses");
    CHECK(efs_meta_apply_membership_record(kv, 1, r3, (uint32_t)l3, one) == EFS_OK &&
          efs_meta_apply_membership_record(kv, 1, r3, (uint32_t)l3, EFS_MEMBERSHIP_KEEP_FIRST) ==
              EFS_OK, "history and repeated MKFS");
    CHECK(efs_meta_membership_get(kv, 1, got, sizeof(got), &gl) == EFS_OK && gl == l4 &&
          !memcmp(got, r4, l4), "anchor holds revision 2");
    CHECK(efs_meta_apply_membership_record(kv, 2, r3, (uint32_t)l3, any) == EFS_OK &&
          efs_meta_apply_membership_record(kv, 2, r5, (uint32_t)l5, any) == EFS_OK &&
          efs_meta_apply_membership_record(kv, 2, r4, (uint32_t)l4, any) == EFS_OK &&
          efs_meta_apply_membership_record(kv, 2, r4b, (uint32_t)l4b, any) == EFS_ERR_FENCED,
          "group-2 copy: any later revision, history OK, divergent fenced");
    CHECK(efs_meta_membership_get(kv, 2, got, sizeof(got), &gl) == EFS_OK && gl == l5 &&
          !memcmp(got, r5, l5), "copy holds revision 3");
    efs_kv_mem_free(kv);
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
    char base[EFS_TEST_PATH], dir[4200], sdir[4200], jdir[4200], err[1024];
    struct member_store ms, ms2;
    struct efs_node t[EFS_MAX_NODES];
    struct efs_membership m3, m4, m4f, m5, other;
    uint64_t inc = 0;
    uint8_t good[EFS_BOOT_FILE_LEN], bad[EFS_BOOT_FILE_LEN], now[EFS_BOOT_FILE_LEN + 4];
    uint8_t ident[EFS_IDENTITY_FILE_LEN];
    const uint32_t ids[] = {1, 2, 3, 4};

    efs_test_path(base, sizeof(base), "efs-w153-store-XXXXXX");
    if (!efs_test_mkdtemp(base)) { perror("mkdtemp"); exit(1); }
    snprintf(dir, sizeof(dir), "%s/mdraft", base);
    snprintf(sdir, sizeof(sdir), "%s/seed", base);
    snprintf(jdir, sizeof(jdir), "%s/joiner", base);
    /* K33: a fresh store started with --join is joining: a token, no node
     * id, no cluster; the boot counter is max(fresh, saved+1). */
    CHECK(member_store_open(&ms, dir, 0, NULL, 0, err, sizeof(err)) == EFS_OK && ms.created &&
          ms.state == EFS_MEMBER_JOINING && ms.token && !ms.member_id && !ms.cluster &&
          !ms.port, err);
    uint64_t token = ms.token;
    table(t, 3, ids);
    t[1].token = token;
    efs_membership_from_nodes(t, 3, 77, &m3, NULL, 0);
    efs_membership_plus(&m3, "10.0.0.4", 18600, 0x1004, &m4, NULL, 0);
    table(t, 4, ids);
    t[1].token = token;
    efs_membership_from_nodes(t, 4, 77, &m4f, NULL, 0); /* formed with four: another history */
    CHECK(member_store_boot(&ms, 5, &inc, err, sizeof(err)) == EFS_OK && inc == 5, "boot fresh");
    /* The exclusive lock: a second owner (another process) is refused. */
    pid_t pid = fork();
    if (pid == 0) {
        struct member_store o;
        char e[512];
        _exit(member_store_open(&o, dir, 0, NULL, 0, e, sizeof(e)) == EFS_ERR_BUSY &&
              strstr(e, "owned by another running process") ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "second owner refused");
    CHECK(member_store_boot(&ms, 3, &inc, err, sizeof(err)) == EFS_OK && inc == 6, "saved+1");
    CHECK(member_store_boot(&ms, 1000, &inc, err, sizeof(err)) == EFS_OK && inc == 1000, "fresh");
    member_store_close(&ms);

    /* Restart: the counter and token persist; it is still joining (opening
     * it as a seed does not turn it into one). */
    CHECK(member_store_open(&ms, dir, 1, "10.0.0.2", 18600, err, sizeof(err)) == EFS_OK &&
          !ms.created && ms.token == token && ms.state == EFS_MEMBER_JOINING, err);
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_OK && inc == 1001, "restart");

    /* M4': a proposal naming this store is promised at its id and endpoint;
     * the same again is OK; another proposal is refused and not written. */
    {
        struct efs_membership w = m3;
        w.m[1].token = token + 1;
        CHECK(member_store_promise(&ms, &w, err, sizeof(err)) == EFS_ERR_FENCED &&
              strstr(err, "not in the membership") && ms.state == EFS_MEMBER_JOINING, err);
    }
    CHECK(member_store_promise(&ms, &m4, err, sizeof(err)) == EFS_ERR_INVAL,
          "a revision after formation is no proposal");
    CHECK(member_store_promise(&ms, &m3, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_PROMISED && ms.member_id == 2 && ms.port == 18600 &&
          !strcmp(ms.addr, "10.0.0.2") && ms.cluster == 77, err);
    CHECK(member_store_promise(&ms, &m3, err, sizeof(err)) == EFS_OK, "same proposal again");
    other = m3;
    other.cluster = 78;
    CHECK(member_store_promise(&ms, &other, err, sizeof(err)) == EFS_ERR_FENCED &&
          ms.cluster == 77, "another cluster's proposal");
    CHECK(member_store_learn(&ms, &other, err, sizeof(err)) == EFS_ERR_FENCED &&
          strstr(err, "belongs to cluster"), err);
    CHECK(member_store_learn(&ms, &m4f, err, sizeof(err)) == EFS_ERR_FENCED &&
          ms.state == EFS_MEMBER_PROMISED, "a formation other than the promised one");
    member_store_close(&ms);
    CHECK(member_store_open(&ms, dir, 0, NULL, 0, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_PROMISED && ms.member_id == 2 &&
          efs_membership_equal(&ms.members, &m3), "the promise survives a restart");

    /* Formation: persisted once; the same record is OK, a later revision is
     * persisted, an earlier one keeps the newer, another history FENCED. */
    CHECK(member_store_learn(&ms, &m3, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMED, err);
    CHECK(member_store_learn(&ms, &m3, err, sizeof(err)) == EFS_OK, "same record again");
    CHECK(member_store_promise(&ms, &m3, err, sizeof(err)) == EFS_OK, "formed holds the proposal");
    CHECK(member_store_learn(&ms, &m4f, err, sizeof(err)) == EFS_ERR_FENCED, "other history");
    {
        struct efs_membership w = m4;
        w.m[1].token = token + 1;
        CHECK(member_store_learn(&ms, &w, err, sizeof(err)) == EFS_ERR_FENCED &&
              strstr(err, "store was replaced"), err);
    }
    CHECK(member_store_learn(&ms, &m4, err, sizeof(err)) == EFS_OK && ms.members.count == 4,
          "revision 2");
    CHECK(member_store_learn(&ms, &m3, err, sizeof(err)) == EFS_OK && ms.members.count == 4,
          "an earlier revision keeps the newer");
    member_store_close(&ms);
    /* Restart derives the count (and so the voters) from the newest revision. */
    CHECK(member_store_open(&ms, dir, 0, NULL, 0, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMED && ms.members.count == 4 && ms.member_id == 2 &&
          efs_membership_equal(&ms.members, &m4) &&
          efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)ms.members.count) == 0xe, err);
    CHECK(efs_membership_plus(&m4, "10.0.0.5", 18600, 0x1005, &m5, NULL, 0) == EFS_OK &&
          member_store_learn(&ms, &m5, err, sizeof(err)) == EFS_OK && ms.members.count == 5, err);

    /* A seed: node 1 at its endpoint with a fresh cluster identity, kept
     * across restarts; it promises only its own cluster as node 1. */
    CHECK(member_store_open(&ms2, sdir, 1, NULL, 0, err, sizeof(err)) == EFS_ERR_INVAL,
          "a seed needs its endpoint");
    CHECK(member_store_open(&ms2, sdir, 1, "10.0.0.1", 18600, err, sizeof(err)) == EFS_OK &&
          ms2.created && ms2.state == EFS_MEMBER_SEED && ms2.member_id == 1 && ms2.cluster &&
          ms2.port == 18600 && !strcmp(ms2.addr, "10.0.0.1"), err);
    uint64_t cl = ms2.cluster, stok = ms2.token;
    member_store_close(&ms2);
    CHECK(member_store_open(&ms2, sdir, 1, "10.0.0.1", 18600, err, sizeof(err)) == EFS_OK &&
          !ms2.created && ms2.state == EFS_MEMBER_SEED && ms2.cluster == cl, err);
    table(t, 3, ids);
    t[0].token = stok;
    efs_membership_from_nodes(t, 3, cl + 1, &other, NULL, 0);
    CHECK(member_store_promise(&ms2, &other, err, sizeof(err)) == EFS_ERR_FENCED &&
          strstr(err, "cannot promise"), err);
    efs_membership_from_nodes(t, 3, cl, &other, NULL, 0);
    CHECK(member_store_promise(&ms2, &other, err, sizeof(err)) == EFS_OK &&
          ms2.state == EFS_MEMBER_PROMISED && ms2.member_id == 1, err);
    member_store_close(&ms2);

    /* A joining store added to a formed cluster learns the committed
     * revision naming it and takes that id. */
    CHECK(member_store_open(&ms2, jdir, 0, NULL, 0, err, sizeof(err)) == EFS_OK, err);
    {
        struct efs_membership w;
        CHECK(efs_membership_plus(&m4, "10.0.0.7", 18607, ms2.token, &w, NULL, 0) == EFS_OK &&
              member_store_learn(&ms2, &w, err, sizeof(err)) == EFS_OK &&
              ms2.state == EFS_MEMBER_FORMED && ms2.member_id == 5 && ms2.port == 18607, err);
    }
    member_store_close(&ms2);

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

    /* identity.bin: a corrupt or truncated identity, another version, or a
     * malformed record or a node id that disagrees with the record under a
     * valid checksum, is refused. */
    CHECK(read_file(dir, "identity.bin", ident, sizeof(ident)) == EFS_IDENTITY_FILE_LEN, "identity size");
    {
        uint8_t c[EFS_IDENTITY_FILE_LEN];
        memcpy(c, ident, sizeof(c));
        c[30] ^= 1;
        write_file(dir, "identity.bin", c, sizeof(c));
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED,
              "corrupt identity");
        write_file(dir, "identity.bin", ident, 40);
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED,
              "short identity");
        memcpy(c, ident, sizeof(c));
        c[11] = 2; /* identity version 2 (an earlier build), checksum recomputed */
        efs_hash(c, EFS_IDENTITY_FILE_LEN - 32, c + EFS_IDENTITY_FILE_LEN - 32);
        write_file(dir, "identity.bin", c, sizeof(c));
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "another version"), err);
        memcpy(c, ident, sizeof(c));
        c[EFS_IDENTITY_REC_AT + 2] = 2; /* record's formation count 2, checksum recomputed */
        efs_hash(c, EFS_IDENTITY_FILE_LEN - 32, c + EFS_IDENTITY_FILE_LEN - 32);
        write_file(dir, "identity.bin", c, sizeof(c));
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "disagrees"), err);
        memcpy(c, ident, sizeof(c));
        c[15] = 3; /* node id 3 while the record holds this store as node 2 */
        efs_hash(c, EFS_IDENTITY_FILE_LEN - 32, c + EFS_IDENTITY_FILE_LEN - 32);
        write_file(dir, "identity.bin", c, sizeof(c));
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "disagrees"), err);
        write_file(dir, "identity.bin", ident, sizeof(ident));
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_OK, err);
        member_store_close(&ms2);
    }

    /* A store that started before and lost its identity, and another
     * build's store, are refused; an initialization interrupted before
     * identity.bin (boot counter still 0) initializes again. */
    {
        char p[4300];
        uint8_t zero[EFS_BOOT_FILE_LEN];
        snprintf(p, sizeof(p), "%s/identity.bin", dir);
        unlink(p);
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "lost identity.bin"), err);
        write_file(dir, "boot.bin", good, 12);
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "unreadable"), err);
        member_store_boot_encode(0, zero);
        write_file(dir, "boot.bin", zero, sizeof(zero));
        write_file(dir, ".identity.bin.tmp", ident, 40); /* the half-written temp */
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_OK && ms2.created &&
              ms2.state == EFS_MEMBER_JOINING && ms2.token && ms2.token != token, err);
        CHECK(member_store_boot(&ms2, 7, &inc, err, sizeof(err)) == EFS_OK && inc == 7, err);
        member_store_close(&ms2);
        unlink(p);
        write_file(dir, "boot.bin", zero, sizeof(zero));
        CHECK(member_store_open(&ms2, dir, 1, "10.0.0.2", 18600, err, sizeof(err)) == EFS_OK &&
              ms2.created && ms2.state == EFS_MEMBER_SEED && ms2.member_id == 1, err);
        member_store_close(&ms2);
        unlink(p);
        write_file(dir, "boot.bin", zero, sizeof(zero));
        snprintf(p, sizeof(p), "%s/log", dir);
        mkdir(p, 0755);
        CHECK(member_store_open(&ms2, dir, 0, NULL, 0, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
              strstr(err, "another build"), err);
        rmdir(p);
        snprintf(p, sizeof(p), "%s/boot.bin", dir);
        unlink(p);
        snprintf(p, sizeof(p), "%s/kv", dir);
        mkdir(p, 0755);
        CHECK(member_store_open(&ms2, dir, 1, "10.0.0.2", 18600, err, sizeof(err)) ==
              EFS_ERR_UNSUPPORTED && strstr(err, "another build"), err);
        rmdir(p);
    }
}

/* K33: discovery datagrams and the subnet broadcast address. */
static void discovery(void)
{
    uint8_t p[EFS_DISC_PROBE_LEN], a[EFS_DISC_ANSWER_MAX + 8], bad[EFS_DISC_ANSWER_MAX + 8];
    struct efs_disc_answer in, out;
    uint64_t nonce = 0;

    efs_disc_probe_encode(p, 0x0102030405060708ull);
    CHECK(efs_disc_probe_decode(p, sizeof(p), &nonce) == 0 && nonce == 0x0102030405060708ull,
          "probe round trip");
    CHECK(efs_disc_probe_decode(p, sizeof(p) - 1, &nonce) != 0, "short probe");
    p[7] = 2;
    CHECK(efs_disc_probe_decode(p, sizeof(p), &nonce) != 0, "probe of another version");
    memset(&in, 0, sizeof(in));
    in.cluster = 0xabcdef;
    in.count = 4;
    in.formed = 1;
    snprintf(in.addr, sizeof(in.addr), "10.0.0.11");
    in.port = 17500;
    snprintf(in.build_id, sizeof(in.build_id), "0123abcd");
    size_t n = efs_disc_answer_encode(a, sizeof(a), 9, &in);
    CHECK(n == 34 + 9 + 8, "answer length");
    CHECK(efs_disc_answer_decode(a, n, 9, &out) == 0 && out.cluster == in.cluster &&
          out.count == 4 && out.formed == 1 && !strcmp(out.addr, "10.0.0.11") &&
          out.port == 17500 && !strcmp(out.build_id, "0123abcd"),
          "answer round trip: a non-default service port is advertised");
    CHECK(efs_disc_answer_decode(a, n, 10, &out) != 0, "answer to another probe");
    CHECK(efs_disc_answer_decode(a, n - 1, 9, &out) != 0, "truncated answer");
    memcpy(bad, a, n);
    bad[n] = 0;
    CHECK(efs_disc_answer_decode(bad, n + 1, 9, &out) != 0, "trailing byte");
    memcpy(bad, a, n);
    memset(bad + 17, 0, 8);
    CHECK(efs_disc_answer_decode(bad, n, 9, &out) != 0, "cluster 0");
    memcpy(bad, a, n);
    bad[33] = ' ';
    CHECK(efs_disc_answer_decode(bad, n, 9, &out) != 0, "unprintable address");
    memcpy(bad, a, n);
    bad[8] = 1;
    CHECK(efs_disc_answer_decode(bad, n, 9, &out) != 0, "a probe is no answer");
    in.port = 0;
    CHECK(efs_disc_answer_encode(a, sizeof(a), 9, &in) == 0, "port 0 not encoded");
    /* The subnet's own mask, never an assumed /24. */
    CHECK(efs_disc_broadcast(0x0a00000b, 0xffffff00) == 0x0a0000ff, "/24");
    CHECK(efs_disc_broadcast(0x0a00010b, 0xfffffc00) == 0x0a0003ff, "/22");
    CHECK(efs_disc_broadcast(0xac10050b, 0xffff0000) == 0xac10ffff, "/16");
    CHECK(efs_disc_broadcast(0x7f000001, 0xff000000) == 0x7fffffff, "loopback /8");
    struct efs_disc_target t[EFS_DISC_MAX_TARGETS];
    char names[512];
    CHECK(efs_disc_targets("no-such-interface-k33", t, EFS_DISC_MAX_TARGETS, names,
                           sizeof(names)) == 0, "an interface that does not exist");
}

int main(void)
{
    formation_input();
    record();
    revisions();
    store();
    discovery();
    if (failures) {
        fprintf(stderr, "membership: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("membership: formation input (2 refused, 3 and 5 committed, contiguous ids, "
           "duplicates refused), record encoding and malformed records, revisions and the "
           "anchor apply rules, voters from the committed count, store lock, boot counter, "
           "token, joining/seed/promised/formed identities and persisted revisions, discovery "
           "datagrams and broadcast addresses: %d checks PASS\n", checks);
    return 0;
}
