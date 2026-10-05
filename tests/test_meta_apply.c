/* Isolated applied-state SM over mem KV. No sockets, no cluster. */
#include "efs/meta_apply.h"
#include "efs/metadata.h"
#include "efs/raft.h"
#include "efs/dir_layout.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include "efs/txn.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* A fixed stamp, so a test can assert exact times. Apply takes the time as
 * an argument precisely so it is not the wall clock. */
#define T0 1000000000000000000ull

static const struct efs_meta_attrs g_at = { 1000, 1000, T0 };

static void test_create_lookup_unlink(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t a = 0, b = 0;
    struct efs_meta_dentry d;
    struct efs_meta_row r;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    CHECK(r.generation == 1 && S_ISDIR(r.mode), "root dir");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "a", &a)
              == EFS_OK && a,
          "create a");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "b", &b)
              == EFS_OK && b && b != a,
          "create b");
    CHECK((a & 0xFFF) == (EFS_ROOT_INO & 0xFFF), "co-located");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "a", &a)
              == EFS_ERR_EXIST,
          "dup");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "a", &d) == EFS_OK && d.ino == a,
          "lookup");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "a", T0) == EFS_OK, "unlink");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "a", &d) == EFS_ERR_NOT_FOUND,
          "gone");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_mkdir(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t d = 0;
    struct efs_meta_dentry dent;
    struct efs_meta_row r, root;
    uint64_t salt = 0;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &root) == EFS_OK, "root");
    CHECK(root.nlink == 2, "root nlink 2");
    CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "d", &d)
              == EFS_OK && d,
          "mkdir");
    CHECK(efs_meta_apply_export_salt(kv, &salt) == EFS_OK, "salt");
    CHECK(efs_kv_inode_shard(d) == efs_kv_mkdir_shard(EFS_ROOT_INO, "d", salt),
          "scattered");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "d", &dent) == EFS_OK &&
              dent.ino == d && dent.type == S_IFDIR,
          "lookup");
    CHECK(efs_meta_apply_get_inode(kv, d, &r) == EFS_OK && S_ISDIR(r.mode) &&
              r.nlink == 2 && r.parent == EFS_ROOT_INO,
          "child row");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &root) == EFS_OK &&
              root.nlink == 3,
          "parent nlink++");
    CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "d", &d)
              == EFS_ERR_EXIST,
          "dup");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "x",
                                     &d) == EFS_ERR_INVAL,
          "create_file rejects dir");
    CHECK(efs_meta_apply_rmdir(kv, EFS_ROOT_INO, "d", T0 + 1) == EFS_OK, "rmdir");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "d", &dent) ==
              EFS_ERR_NOT_FOUND,
          "gone");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_create_remote_parent(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t parent = 3; /* odd shard → group 0; never inserted */
    efs_ino_t ino = 0, miss = 0;
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    char name[8];
    char same[8];
    int i, found = 0, same_g = 0;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    for (i = 0; i < 256; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        if (efs_raft_shard_group(efs_kv_dentry_shard(parent, name,
                                                    EFS_META_LAYOUT_HASHED)) !=
            efs_raft_shard_group(efs_kv_inode_shard(parent))) {
            found = 1;
            break;
        }
    }
    CHECK(found, "hashed name on other group");
    CHECK(efs_meta_apply_create_file(kv, &g_at, parent, S_IFREG | 0644, name,
                                     &ino) == EFS_OK &&
              ino,
          "create without local parent");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.parent == parent,
          "child row");
    CHECK(efs_meta_apply_lookup(kv, parent, name, &d) == EFS_OK && d.ino == ino,
          "lookup without parent row");
    for (i = 0; i < 256; i++) {
        snprintf(same, sizeof(same), "s%d", i);
        if (efs_raft_shard_group(efs_kv_dentry_shard(parent, same,
                                                    EFS_META_LAYOUT_HASHED)) ==
            efs_raft_shard_group(efs_kv_inode_shard(parent))) {
            same_g = 1;
            break;
        }
    }
    CHECK(same_g, "same-group hashed name");
    CHECK(efs_meta_apply_create_file(kv, &g_at, parent, S_IFREG | 0644, same,
                                     &miss) == EFS_ERR_NOT_FOUND,
          "same-group missing parent is a hole");
    miss = 0;
    CHECK(efs_meta_apply_create_file_log(kv, &g_at, parent, S_IFREG | 0644, same,
                                         &miss) == EFS_OK &&
              miss,
          "log apply writes even if parent missing");
    CHECK(efs_meta_apply_get_inode(kv, miss, &r) == EFS_OK && r.parent == parent,
          "log child row");
    efs_kv_mem_free(kv);
}

/* Dual-host 004 sees a LOCAL parent; g2-only 006 does not. Guessing HASHED
 * from the miss writes a different ino than the leader's LOCAL alloc. */
static void test_create_log_at_matches(void)
{
    struct efs_kv *have = efs_kv_mem_create();
    struct efs_kv *miss = efs_kv_mem_create();
    efs_ino_t parent = 0, want = 0, got = 0;
    struct efs_meta_row r;
    uint32_t dsh;

    CHECK(have && miss, "kv");
    CHECK(efs_meta_apply_init(have, T0) == EFS_OK, "init have");
    CHECK(efs_meta_apply_init(miss, T0) == EFS_OK, "init miss");
    CHECK(efs_meta_apply_mkdir(have, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "p",
                               &parent) == EFS_OK &&
              parent,
          "parent");
    dsh = efs_kv_dentry_shard(parent, "f", EFS_META_LAYOUT_LOCAL);
    CHECK(efs_meta_apply_peek_alloc(have, dsh, &want) == EFS_OK && want,
          "peek");
    CHECK(efs_meta_apply_create_file_log_at(have, &g_at, parent, S_IFREG | 0644,
                                            "f", want, EFS_META_LAYOUT_LOCAL,
                                            &got) == EFS_OK &&
              got == want,
          "leader write");
    got = 0;
    CHECK(efs_meta_apply_create_file_log_at(miss, &g_at, parent, S_IFREG | 0644,
                                            "f", want, EFS_META_LAYOUT_LOCAL,
                                            &got) == EFS_OK &&
              got == want,
          "hollow follower write");
    CHECK(efs_meta_apply_get_inode(miss, want, &r) == EFS_OK &&
              r.parent == parent,
          "follower has leader ino");
    efs_kv_mem_free(have);
    efs_kv_mem_free(miss);
}

static void wr64_test(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

/* A stale alloc watermark sitting on a live row must not reuse that ino.
 * Reuse overwrites the row and inherits leftover children (mkdir p/c EEXIST). */
static void test_alloc_skips_live(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t a = 0, b = 0, c = 0, peek = 0;
    uint8_t k[EFS_KV_KEY_MAX], v[8];
    uint32_t kl = 0, shard;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "a",
                                     &a) == EFS_OK &&
              a,
          "a");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "b",
                                     &b) == EFS_OK &&
              b && b != a,
          "b");
    shard = efs_kv_inode_shard(a);
    CHECK(efs_kv_key_alloc(shard, k, &kl) == EFS_OK, "alloc key");
    wr64_test(v, a);
    CHECK(efs_kv_put(kv, k, kl, v, 8) == EFS_OK, "rewind");
    CHECK(efs_meta_apply_peek_alloc(kv, shard, &peek) == EFS_OK && peek &&
              peek != a && peek != b,
          "peek skips live");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "c",
                                     &c) == EFS_OK &&
              c == peek && c != a && c != b,
          "create skips live");
    efs_kv_mem_free(kv);
}

/* Two proposers peek the same alloc watermark before either applies and
 * both carry ino=want. The apply must not let the second overwrite the
 * first's row (IO-500 ior-easy: 36 files, 10 distinct inos). Same log on
 * two replicas must pick the same fallback ino. */
static void test_create_log_at_dup_hint(void)
{
    struct efs_kv *a = efs_kv_mem_create();
    struct efs_kv *b = efs_kv_mem_create();
    efs_ino_t parent = 0, want = 0, f1 = 0, f2 = 0, g1 = 0, g2 = 0, d1 = 0,
              d2 = 0;
    struct efs_meta_row r;
    struct efs_meta_dentry de;
    uint32_t dsh, csh;
    uint64_t salt = 0;

    CHECK(a && b, "kv");
    CHECK(efs_meta_apply_init(a, T0) == EFS_OK, "init a");
    CHECK(efs_meta_apply_init(b, T0) == EFS_OK, "init b");
    CHECK(efs_meta_apply_mkdir(a, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "p",
                               &parent) == EFS_OK &&
              parent,
          "parent a");
    CHECK(efs_meta_apply_mkdir(b, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "p",
                               &g1) == EFS_OK &&
              g1 == parent,
          "parent b");
    dsh = efs_kv_dentry_shard(parent, "f1", EFS_META_LAYOUT_LOCAL);
    CHECK(efs_kv_dentry_shard(parent, "f2", EFS_META_LAYOUT_LOCAL) == dsh,
          "same shard (LOCAL dir)");
    CHECK(efs_meta_apply_peek_alloc(a, dsh, &want) == EFS_OK && want, "peek");
    CHECK(efs_meta_apply_create_file_log_at(a, &g_at, parent, S_IFREG | 0644,
                                            "f1", want, EFS_META_LAYOUT_LOCAL,
                                            &f1) == EFS_OK &&
              f1 == want,
          "f1 takes hint");
    CHECK(efs_meta_apply_create_file_log_at(a, &g_at, parent, S_IFREG | 0644,
                                            "f2", want, EFS_META_LAYOUT_LOCAL,
                                            &f2) == EFS_OK &&
              f2 && f2 != f1,
          "f2 dup hint gets a fresh ino");
    CHECK(efs_meta_apply_lookup(a, parent, "f1", &de) == EFS_OK && de.ino == f1,
          "f1 dentry intact");
    CHECK(efs_meta_apply_get_inode(a, f1, &r) == EFS_OK && r.nlink == 1,
          "f1 row intact");
    CHECK(efs_meta_apply_lookup(a, parent, "f2", &de) == EFS_OK && de.ino == f2,
          "f2 dentry");
    /* replica parity */
    g1 = g2 = 0;
    CHECK(efs_meta_apply_create_file_log_at(b, &g_at, parent, S_IFREG | 0644,
                                            "f1", want, EFS_META_LAYOUT_LOCAL,
                                            &g1) == EFS_OK &&
              g1 == f1,
          "replica f1");
    CHECK(efs_meta_apply_create_file_log_at(b, &g_at, parent, S_IFREG | 0644,
                                            "f2", want, EFS_META_LAYOUT_LOCAL,
                                            &g2) == EFS_OK &&
              g2 == f2,
          "replica f2 same fallback");
    /* a stale hint BELOW the watermark (row already unlinked) must not be
     * reused either — (ino, gen=1) would alias the dead file's GC/lease
     * records */
    CHECK(efs_meta_apply_unlink(a, parent, "f1", T0) == EFS_OK, "unlink f1");
    CHECK(efs_meta_apply_create_file_log_at(a, &g_at, parent, S_IFREG | 0644,
                                            "f3", f1, EFS_META_LAYOUT_LOCAL,
                                            &g1) == EFS_OK &&
              g1 && g1 != f1 && g1 != f2,
          "stale hint below watermark not reused");
    /* mkdir same-group fast path: same rule */
    CHECK(efs_meta_apply_export_salt(a, &salt) == EFS_OK, "salt");
    csh = efs_kv_mkdir_shard(parent, "d1", salt);
    CHECK(efs_meta_apply_peek_alloc(a, csh, &want) == EFS_OK && want, "peek d");
    CHECK(efs_meta_apply_mkdir_at(a, &g_at, parent, S_IFDIR | 0755, "d1", want,
                                  EFS_META_LAYOUT_LOCAL, &d1) == EFS_OK &&
              d1 == want,
          "d1 takes hint");
    {
        /* mkdir scatters the child by name; find a sibling on d1's shard */
        char dn[16];
        int i;

        for (i = 2; i < 100000; i++) {
            snprintf(dn, sizeof(dn), "d%d", i);
            if (efs_kv_mkdir_shard(parent, dn, salt) == csh)
                break;
        }
        CHECK(i < 100000, "sibling on same child shard");
        CHECK(efs_meta_apply_mkdir_at(a, &g_at, parent, S_IFDIR | 0755, dn,
                                      want, EFS_META_LAYOUT_LOCAL,
                                      &d2) == EFS_OK &&
                  d2 && d2 != d1,
              "d2 dup hint gets a fresh ino");
    }
    CHECK(efs_meta_apply_get_inode(a, d1, &r) == EFS_OK && S_ISDIR(r.mode),
          "d1 row intact");
    efs_kv_mem_free(a);
    efs_kv_mem_free(b);
}

/* The shard ALLOC key is shared with the txn path (cross-group mkdir /
 * hashed create PREPARE an EXCL intent on it). Both orders must be safe:
 * intent pending → log-path create is BUSY (never the same ino); log-path
 * create first → it bumps the key's version so a PREPARE at the version a
 * txn read before it is STALE. (posix names_*: file cafeé + dir aaaa… on
 * shard 650 both got ino 4746.) */
static void test_alloc_vs_txn_intent(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t parent = 0, want = 0, f1 = 0, f2 = 0;
    uint32_t shard, ka = 0;
    uint8_t k_alloc[EFS_KV_KEY_MAX], v[8];
    uint64_t aver = 0, aver2 = 0;
    struct efs_txid t;
    struct efs_txn_parts p;
    int rc;

    CHECK(kv, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "p",
                               &parent) == EFS_OK &&
              parent,
          "parent");
    shard = efs_kv_dentry_shard(parent, "f1", EFS_META_LAYOUT_LOCAL);
    CHECK(efs_kv_key_alloc(shard, k_alloc, &ka) == EFS_OK, "alloc key");
    CHECK(efs_meta_apply_peek_alloc(kv, shard, &want) == EFS_OK && want, "peek");
    CHECK(efs_txn_ver_get(kv, k_alloc, ka, &aver) == EFS_OK, "aver");

    /* (1) a txn claimed the watermark: same ino a peeker would carry */
    memset(&t, 0, sizeof(t));
    t.bytes[0] = 0xa1;
    memset(&p, 0, sizeof(p));
    p.n = 1;
    p.shard[0] = shard;
    memset(v, 0, sizeof(v));
    CHECK(efs_txn_prepare_excl(kv, &t, &p, k_alloc, ka, aver, EFS_TXN_PUT, v,
                               8) == EFS_OK,
          "txn prepare alloc");
    rc = efs_meta_apply_create_file_log_at(kv, &g_at, parent, S_IFREG | 0644,
                                           "f1", want, EFS_META_LAYOUT_LOCAL,
                                           &f1);
    CHECK(rc == EFS_ERR_BUSY, "create under alloc intent is BUSY");
    rc = efs_meta_apply_create_file(kv, &g_at, parent, S_IFREG | 0644, "f1",
                                    &f1);
    CHECK(rc == EFS_ERR_BUSY, "create (no hint) under alloc intent is BUSY");
    CHECK(efs_txn_drop(kv, &t, shard) == EFS_OK, "txn drop");
    CHECK(efs_meta_apply_create_file_log_at(kv, &g_at, parent, S_IFREG | 0644,
                                           "f1", want, EFS_META_LAYOUT_LOCAL,
                                           &f1) == EFS_OK &&
              f1 == want,
          "create after drop takes the hint");

    /* (2) log-path create first: the version moved, an outdated PREPARE
     * (the txn read aver before the create) is STALE, not a silent win */
    CHECK(efs_txn_ver_get(kv, k_alloc, ka, &aver2) == EFS_OK && aver2 > aver,
          "log-path alloc bumped ver");
    t.bytes[0] = 0xa2;
    CHECK(efs_txn_prepare_excl(kv, &t, &p, k_alloc, ka, aver, EFS_TXN_PUT, v,
                               8) == EFS_ERR_STALE,
          "outdated txn prepare on alloc is STALE");
    CHECK(efs_txn_prepare_excl(kv, &t, &p, k_alloc, ka, aver2, EFS_TXN_PUT, v,
                               8) == EFS_OK,
          "fresh-version prepare OK");
    CHECK(efs_txn_drop(kv, &t, shard) == EFS_OK, "txn drop 2");
    CHECK(efs_meta_apply_create_file(kv, &g_at, parent, S_IFREG | 0644, "f2",
                                     &f2) == EFS_OK &&
              f2 && f2 != f1,
          "second create distinct");
    efs_kv_mem_free(kv);
}

/* §7.2: a cross-group CREATE into a directory is invisible to the log path
 * until it resolves (dentry EXCL + dseq REDUCE_ADD + row REDUCE_INO). A
 * same-group log-path rmdir of that directory must not DEL the row from
 * under the intent (it would land the txn's nlink++ on a deleted row, or
 * make a dir with children "empty"): BUSY, then OK once the intent is gone.
 * Same for an unlink of a file whose row carries a pending reduce
 * (link's nlink++). */
static void test_log_delete_busy_under_intent(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t d = 0, f = 0;
    uint32_t dshard, fshard, kd = 0, kf = 0, ki = 0;
    uint8_t k_dseq[EFS_KV_KEY_MAX], k_fino[EFS_KV_KEY_MAX], k_dino[EFS_KV_KEY_MAX];
    struct efs_txid t;
    struct efs_txn_parts p;
    struct efs_txn_ino_delta delta;
    struct efs_meta_dentry dent;

    CHECK(kv, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "d",
                               &d) == EFS_OK &&
              d,
          "dir");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "f", &f) == EFS_OK &&
              f,
          "file");
    dshard = efs_kv_inode_shard(d);
    fshard = efs_kv_inode_shard(f);
    memset(&t, 0, sizeof(t));
    t.bytes[0] = 0xb1;
    memset(&p, 0, sizeof(p));
    p.n = 1;

    /* (1) rmdir vs. a pending create-into-d: witness bump */
    p.shard[0] = dshard;
    CHECK(efs_kv_key_dseq(dshard, d, 0, k_dseq, &kd) == EFS_OK, "dseq key");
    CHECK(efs_txn_prepare_add(kv, &t, &p, k_dseq, kd, 1) == EFS_OK, "add prep");
    CHECK(efs_meta_apply_rmdir(kv, EFS_ROOT_INO, "d", T0 + 1) == EFS_ERR_BUSY,
          "log rmdir under a pending dseq bump is BUSY");
    CHECK(efs_txn_drop(kv, &t, dshard) == EFS_OK, "drop add");

    /* (2) rmdir vs. a pending reduce on the dir row itself (mkdir's
     * nlink++ into d) */
    t.bytes[0] = 0xb2;
    CHECK(efs_kv_key_inode(dshard, d, k_dino, &ki) == EFS_OK, "dino key");
    memset(&delta, 0, sizeof(delta));
    delta.d_nlink = 1;
    delta.d_nents = 1;
    CHECK(efs_txn_prepare_ino_delta(kv, &t, &p, k_dino, ki, &delta) == EFS_OK,
          "ino delta prep");
    CHECK(efs_meta_apply_rmdir(kv, EFS_ROOT_INO, "d", T0 + 1) == EFS_ERR_BUSY,
          "log rmdir under a pending row reduce is BUSY");
    CHECK(efs_txn_drop(kv, &t, dshard) == EFS_OK, "drop delta");
    CHECK(efs_meta_apply_rmdir(kv, EFS_ROOT_INO, "d", T0 + 1) == EFS_OK,
          "rmdir once the intents are gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "d", &dent) ==
              EFS_ERR_NOT_FOUND,
          "d gone");

    /* (3) unlink of the last name vs. a pending link nlink++ */
    t.bytes[0] = 0xb3;
    p.shard[0] = fshard;
    CHECK(efs_kv_key_inode(fshard, f, k_fino, &kf) == EFS_OK, "fino key");
    delta.d_nents = 0;
    CHECK(efs_txn_prepare_ino_delta(kv, &t, &p, k_fino, kf, &delta) == EFS_OK,
          "link delta prep");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "f", T0 + 1) == EFS_ERR_BUSY,
          "log unlink under a pending nlink++ is BUSY");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "f", &dent) == EFS_OK,
          "f still there (nothing half-applied)");
    CHECK(efs_txn_drop(kv, &t, fshard) == EFS_OK, "drop link delta");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "f", T0 + 1) == EFS_OK,
          "unlink after drop");
    efs_kv_mem_free(kv);
}

static void test_mkdir_log_at_matches(void)
{
    struct efs_kv *have = efs_kv_mem_create();
    struct efs_kv *miss = efs_kv_mem_create();
    efs_ino_t parent = 0, want = 0, got = 0;
    struct efs_meta_row r;
    uint32_t csh;
    uint64_t salt = 0;

    CHECK(have && miss, "kv");
    CHECK(efs_meta_apply_init(have, T0) == EFS_OK, "init have");
    CHECK(efs_meta_apply_init(miss, T0) == EFS_OK, "init miss");
    CHECK(efs_meta_apply_mkdir(have, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "p",
                               &parent) == EFS_OK &&
              parent,
          "parent");
    CHECK(efs_meta_apply_export_salt(have, &salt) == EFS_OK, "salt");
    csh = efs_kv_mkdir_shard(parent, "d", salt);
    CHECK(efs_meta_apply_peek_alloc(have, csh, &want) == EFS_OK && want,
          "peek");
    CHECK(efs_meta_apply_mkdir_at(have, &g_at, parent, S_IFDIR | 0755, "d", want,
                                  EFS_META_LAYOUT_LOCAL, &got) == EFS_OK &&
              got == want,
          "leader write");
    got = 0;
    CHECK(efs_meta_apply_mkdir_at(miss, &g_at, parent, S_IFDIR | 0755, "d", want,
                                  EFS_META_LAYOUT_LOCAL, &got) == EFS_OK &&
              got == want,
          "hollow follower write");
    CHECK(efs_meta_apply_get_inode(miss, want, &r) == EFS_OK &&
              r.parent == parent && S_ISDIR(r.mode),
          "follower has leader ino");
    efs_kv_mem_free(have);
    efs_kv_mem_free(miss);
}

static void test_i9(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0;
    struct efs_meta_dentry d;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "x", &ino)
              == EFS_OK,
          "create");
    CHECK(efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen) == EFS_OK,
          "key");
    CHECK(efs_kv_del(kv, key, klen) == EFS_OK, "drop inode");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "x", &d) == EFS_OK && d.ino == ino,
          "dentry remains");
    CHECK(efs_meta_apply_resolve(kv, EFS_ROOT_INO, "x", NULL, NULL) == EFS_ERR_IO,
          "I9");
    efs_kv_mem_free(kv);
}

static void test_batch_fail(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_dentry d;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_kv_mem_fail_next_batch(kv) == EFS_OK, "arm");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "y", &ino)
              == EFS_ERR_IO,
          "create fail");
    CHECK(ino == 0, "no ino");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "y", &d) == EFS_ERR_NOT_FOUND,
          "no dentry");
    efs_kv_mem_free(kv);
}

static void test_i16_durable(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_opid op;
    efs_ino_t a = 0, b = 0, c = 0;
    struct efs_meta_dentry d;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    memset(&op, 0, sizeof(op));
    op.client_uuid[15] = 1;
    op.session_epoch = 1;
    op.seq = 1;
    CHECK(efs_meta_apply_create_file_op(kv, &op, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                        "once", &a) == EFS_OK && a,
          "first");
    CHECK(efs_meta_apply_create_file_op(kv, &op, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                        "once", &b) == EFS_OK && b == a,
          "replay");
    CHECK(efs_meta_apply_create_file_op(kv, &op, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                        "other", &c) == EFS_OK && c == a,
          "replay other name");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "other", &d) == EFS_ERR_NOT_FOUND,
          "no second name");

    /* Log-path directory ops with an op-id (I16 for the production host):
     * the retry of a committed op returns the recorded verdict, not the
     * EEXIST / ENOENT the namespace would now give. */
    {
        struct efs_opid_req q;
        struct efs_opid_reply got;
        efs_ino_t dir = 0, dir2 = 0, f = 0, f2 = 0;
        uint32_t dsh;

        memset(&q, 0, sizeof(q));
        q.id = op;
        q.id.seq = 2;
        CHECK(efs_meta_apply_mkdir_log_op(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755,
                                          "d", 0, -1, &q, &dir) == EFS_OK && dir,
              "mkdir op");
        CHECK(efs_meta_apply_mkdir_log_op(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755,
                                          "d", 0, -1, &q, &dir2) == EFS_OK &&
                  dir2 == dir,
              "mkdir replay = same ino, not EEXIST");
        CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "d",
                                   &dir2) == EFS_ERR_EXIST,
              "same name without an op-id is EEXIST");
        dsh = efs_kv_dentry_shard(EFS_ROOT_INO, "d", EFS_META_LAYOUT_LOCAL);
        CHECK(efs_meta_apply_opid_probe(kv, dsh, &q.id, &got) == 1 &&
                  got.ino == dir,
              "probe sees the mkdir verdict");
        q.id.seq = 3;
        q.ack = 2;
        CHECK(efs_meta_apply_create_file_log_op(kv, &g_at, dir, S_IFREG | 0644,
                                                "f", 0, -1, &q, &f) == EFS_OK && f,
              "create op");
        CHECK(efs_meta_apply_create_file_log_op(kv, &g_at, dir, S_IFREG | 0644,
                                                "f", 0, -1, &q, &f2) == EFS_OK &&
                  f2 == f,
              "create replay");
        q.id.seq = 4;
        q.ack = 3;
        CHECK(efs_meta_apply_unlink_op(kv, dir, "f", T0 + 5, &q) == EFS_OK,
              "unlink op");
        CHECK(efs_meta_apply_lookup(kv, dir, "f", &d) == EFS_ERR_NOT_FOUND,
              "f gone");
        CHECK(efs_meta_apply_unlink_op(kv, dir, "f", T0 + 5, &q) == EFS_OK,
              "unlink replay is OK, not ENOENT");
        CHECK(efs_meta_apply_unlink(kv, dir, "f", T0 + 5) == EFS_ERR_NOT_FOUND,
              "without an op-id it is ENOENT");
        q.id.seq = 5;
        q.ack = 4;
        CHECK(efs_meta_apply_rmdir_op(kv, EFS_ROOT_INO, "d", T0 + 6, &q) == EFS_OK,
              "rmdir op");
        CHECK(efs_meta_apply_rmdir_op(kv, EFS_ROOT_INO, "d", T0 + 6, &q) == EFS_OK,
              "rmdir replay is OK");
        CHECK(efs_meta_apply_opid_probe(kv, dsh, &q.id, &got) == 1 &&
                  got.ino == dir,
              "rmdir verdict names the removed dir");
        /* the ack watermark reclaimed the older cache entries but they
         * still answer OK (acked stub) */
        q.id.seq = 2;
        CHECK(efs_meta_apply_opid_probe(kv, dsh, &q.id, &got) == 1 &&
                  got.rc == EFS_OK,
              "acked seq still completed");
    }
    efs_kv_mem_free(kv);
}

static void test_publish(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_chunk ch, got;
    struct efs_meta_pub p;
    int i;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "d", &ino)
              == EFS_OK,
          "create");
    memset(&ch, 0, sizeof(ch));
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        ch.nodes[i] = (efs_node_id_t)(i + 1);
        ch.checksums[i][0] = (uint8_t)(0x10 + i);
    }
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_ERR_NOT_FOUND,
          "unpublished");
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.chunk_index = 0;
    p.new_size = 64;
    p.expected_gen = 0;
    p.candidate_gen = 0xA1;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "publish");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "get");
    CHECK(got.generation == 0xA1 && got.nodes[0] == 1 &&
              got.checksums[2][0] == 0x12,
          "chunk bytes");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void fill_ch(struct efs_meta_chunk *ch)
{
    int i;

    memset(ch, 0, sizeof(*ch));
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        ch->nodes[i] = (efs_node_id_t)(i + 1);
        ch->checksums[i][0] = (uint8_t)(0x10 + i);
    }
}

static void test_cas_i20_i21(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_chunk ch;
    struct efs_meta_pub p;
    struct efs_meta_row r;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "d", &ino)
              == EFS_OK,
          "create");
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.new_size = 64;
    p.candidate_gen = 0xA1;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "first");
    p.candidate_gen = 0xB2;
    p.new_size = 999;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "I20 expected 0");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.base_size == 64,
          "I21 size held");
    p.expected_gen = 0xA1;
    p.new_size = 128;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "successor");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.base_size == 128,
          "grew");
    p.expected_gen = 0xB2;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "idempotent");
    efs_kv_mem_free(kv);
}

static void test_publish_stale_then_retry(void)
{
    /* W1: a writer whose expected_gen is behind must STALE, then succeed
     * after refreshing expected from the committed mapping. */
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_chunk ch, got;
    struct efs_meta_pub p;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "w1", &ino) == EFS_OK,
          "create");
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.new_size = 64;
    p.candidate_gen = 0xA1;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "first");
    p.candidate_gen = 0xB2;
    p.new_size = 128;
    p.expected_gen = 0;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "behind");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "get");
    CHECK(got.generation == 0xA1, "still A");
    p.expected_gen = got.generation;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "retry");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xB2,
          "landed");
    efs_kv_mem_free(kv);
}

/* Disjoint sub-chunk publishes append spans and leave the base
 * generation alone. Overlap and a full chain are STALE. A full image
 * CAS has to name the span list it folded, and then the trailer is gone. */
static void test_chunk_deltas(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_chunk ch, got;
    struct efs_meta_pub p;
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t nd = 0, i;
    uint64_t newest = 0;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "delta", &ino) == EFS_OK,
          "create");
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.new_size = 128 * 1024;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    p.delta_off = 0;
    p.delta_len = 47008;
    p.candidate_gen = 0xD1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "span0");
    p.delta_off = 47008;
    p.delta_len = 47008;
    p.candidate_gen = 0xD2;
    ch.checksums[0][0] = 0x22;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "span1");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "base");
    CHECK(got.generation == 0, "base gen held");
    CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX,
                                          &nd, &newest) == EFS_OK,
          "list");
    CHECK(nd == 2 && ds[0].generation == 0xD1 && ds[1].generation == 0xD2 &&
              ds[0].off == 0 && ds[1].off == 47008 && newest != 0,
          "two spans");
    p.candidate_gen = 0xD2;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "replay");
    CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX,
                                          &nd, &newest) == EFS_OK &&
              nd == 2,
          "replay did not append");
    /* The same object under another, disjoint range is another
     * client's record of the identical merged image (content-hash
     * names): it is a new span, not a replay (IOR hard, Sep 30). */
    {
        struct efs_meta_row row;
        uint64_t cur = 0;

        CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK, "row");
        CHECK(efs_meta_apply_chunk_holds(kv, ino, row.generation, 0, 0xD2,
                                         47008, 47008, &cur) == 1,
              "holds D2 at its range");
        CHECK(efs_meta_apply_chunk_holds(kv, ino, row.generation, 0, 0xD2,
                                         94016, 1000, &cur) == 0,
              "does not hold D2 at another range");
        CHECK(efs_meta_apply_chunk_holds(kv, ino, row.generation, 0, 0xD2,
                                         0, 0, &cur) == 0,
              "a span gen is not the base");
        p.candidate_gen = 0xD2;
        p.delta_off = 94016;
        p.delta_len = 1000;
        CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "same object, new range");
        CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds,
                                              EFS_CHUNK_DELTA_MAX, &nd,
                                              &newest) == EFS_OK &&
                  nd == 3 && ds[2].generation == 0xD2 && ds[2].off == 94016,
              "second range of one object appended");
        CHECK(efs_meta_apply_chunk_holds(kv, ino, row.generation, 0, 0xD2,
                                         94016, 1000, &cur) == 1,
              "now held");
        p.delta_base_n = nd;
        p.delta_base_seq = newest;
    }
    p.candidate_gen = 0xD3;
    p.delta_off = 100;
    p.delta_len = 50;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "overlap");
    p.delta_len = 0;
    p.delta_base_n = 0;
    p.delta_base_seq = 0;
    p.candidate_gen = 0xE1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "fold blind");
    p.delta_base_n = nd;
    p.delta_base_seq = newest;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "fold");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xE1,
          "folded gen");
    /* D1: the fold keeps the folded spans' candidate_gens as len-0
     * tombstones so a late replay of one of them is a no-op. */
    CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX,
                                          &nd, &newest) == EFS_OK &&
              nd == 3 && ds[0].len == 0 && ds[1].len == 0 &&
              ds[2].len == 0 && ds[0].generation == 0xD1 &&
              ds[1].generation == 0xD2 && ds[2].generation == 0xD2,
          "fold left three tombstones");
    /* W17 step 3 (a): replay of a folded span is a no-op. */
    p.delta_off = 0;
    p.delta_len = 47008;
    p.candidate_gen = 0xD1;
    p.expected_gen = 0;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "replay of folded span");
    CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX,
                                          &nd, &newest) == EFS_OK &&
              nd == 3,
          "folded replay did not append");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xE1,
          "folded replay kept the base");
    /* W17 step 3 (b): a span after the fold attaches to the new base
     * without naming it (expected_gen is the full-image CAS only). */
    p.delta_off = 47008;
    p.delta_len = 1000;
    p.candidate_gen = 0xE2;
    p.expected_gen = 0;
    ch.checksums[0][0] = 0x33;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "span after fold");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xE1,
          "span after fold kept base E1");
    CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX,
                                          &nd, &newest) == EFS_OK &&
              nd == 4 && ds[3].generation == 0xE2 && ds[3].len == 1000,
          "one live span over the fold");
    /* W17 step 3 (c): overlap with that live span is STALE; a
     * tombstone's old range is not an overlap. */
    p.delta_off = 47500;
    p.delta_len = 100;
    p.candidate_gen = 0xE3;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE,
          "overlap after fold");
    p.delta_off = 10;
    p.delta_len = 100;
    p.candidate_gen = 0xE4;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK,
          "tombstone range is free");
    /* A full-image CAS after the fold must name the current base. */
    CHECK(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX,
                                          &nd, &newest) == EFS_OK,
          "list after fold");
    p.delta_len = 0;
    p.delta_base_n = nd;
    p.delta_base_seq = newest;
    p.candidate_gen = 0xF1;
    p.expected_gen = 0;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE,
          "full image with the wrong base");
    p.expected_gen = 0xE1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "second fold");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xF1,
          "second fold landed");

    /* A fresh file fills the chain and refuses the next span. */
    efs_kv_mem_free(kv);
    kv = efs_kv_mem_create();
    CHECK(kv != NULL, "kv2");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init2");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "full", &ino) == EFS_OK,
          "create2");
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.new_size = 128 * 1024;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    p.delta_len = 64;
    for (i = 0; i < EFS_CHUNK_DELTA_MAX; i++) {
        p.delta_off = i * 64;
        p.candidate_gen = 0x100 + i;
        ch.checksums[0][0] = (uint8_t)i;
        p.ch = ch;
        CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "fill");
    }
    p.delta_off = EFS_CHUNK_DELTA_MAX * 64;
    p.candidate_gen = 0x1FF;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "chain full");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0,
          "full chain kept the base");
    efs_kv_mem_free(kv);
}

static void test_epoch_i22(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_chunk ch;
    struct efs_meta_pub p;
    struct efs_meta_row r;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "e", &ino)
              == EFS_OK,
          "create");
    fill_ch(&ch);
    CHECK(efs_meta_apply_epoch_fence(kv, ino) == EFS_OK, "fence");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.content_epoch == 1,
          "epoch 1");
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.new_size = 8;
    p.candidate_gen = 0xC3;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.content_epoch = 0;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "old epoch");
    p.content_epoch = 1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "new epoch");
    efs_kv_mem_free(kv);
}

static void test_evidence(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_chunk ch;
    struct efs_meta_pub p;
    struct efs_meta_chunk got;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "f", &ino)
              == EFS_OK,
          "create");
    fill_ch(&ch);
    ch.nodes[2] = ch.nodes[0];
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.candidate_gen = 1;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_INVAL, "dup nodes");
    fill_ch(&ch);
    p.ch = ch;
    p.coding_profile_id = 99;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "bad profile");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_ERR_NOT_FOUND,
          "nothing published");
    efs_kv_mem_free(kv);
}

static const char *spread_name(efs_ino_t parent)
{
    static char buf[16];
    int i;

    for (i = 0; i < 4096; i++) {
        snprintf(buf, sizeof(buf), "n%d", i);
        if (efs_kv_dir_lane(buf) != 0 &&
            efs_kv_dentry_shard(parent, buf, EFS_META_LAYOUT_HASHED) !=
                efs_kv_inode_shard(parent))
            return buf;
    }
    return "n1";
}

/* Two HASHED names on distinct off-home dir lanes, so a collect that only
 * looked at the inode row (or only one lane) cannot fake a MAX. */
static int hashed_pair(efs_ino_t parent, char *a, char *b)
{
    uint8_t la = 0xff;
    int i;

    a[0] = b[0] = 0;
    for (i = 0; i < 8192; i++) {
        char buf[16];
        uint8_t lane;
        uint32_t sh;

        snprintf(buf, sizeof(buf), "n%d", i);
        lane = efs_kv_dir_lane(buf);
        sh = efs_kv_dentry_shard(parent, buf, EFS_META_LAYOUT_HASHED);
        if (lane == 0 || sh == efs_kv_inode_shard(parent))
            continue;
        if (la == 0xff) {
            snprintf(a, 16, "%s", buf);
            la = lane;
        } else if (lane != la) {
            snprintf(b, 16, "%s", buf);
            return 0;
        }
    }
    return -1;
}

static void test_i8_spread(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t foo = 0, bar = 0;
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    const char *nm;
    int rc;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "foo",
                                     &foo) == EFS_OK,
          "foo");
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_SPLITTING,
          "SPLITTING");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "foo", T0) == EFS_OK, "unlink");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "foo", &d) ==
              EFS_ERR_NOT_FOUND,
          "I8 hashed wins");
    rc = efs_meta_dir_migrate_one(kv, EFS_ROOT_INO);
    CHECK(rc == EFS_OK || rc == EFS_ERR_NOT_FOUND, "migrate");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "foo", &d) ==
              EFS_ERR_NOT_FOUND,
          "no resurrect");
    nm = spread_name(EFS_ROOT_INO);
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, nm, &bar)
              == EFS_OK &&
              bar,
          "hashed create");
    CHECK((bar & 0xFFF) ==
              efs_kv_dentry_shard(EFS_ROOT_INO, nm, EFS_META_LAYOUT_HASHED),
          "co-located with dentry shard");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, nm, &d) == EFS_OK &&
              d.ino == bar,
          "lookup hashed");
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, nm, &d) == EFS_OK &&
              d.ino == bar,
          "still there");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* Collects a whole directory by paging with `page` entries at a time, so the
 * same assertions cover a single-page listing and a heavily paged one. */
struct dir_list {
    char names[64][EFS_MAX_NAME];
    uint32_t n;
    uint32_t calls;
};

static int list_dir(struct efs_kv *kv, efs_ino_t dir, uint32_t page,
                    struct dir_list *l)
{
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent ent[16];
    uint32_t got, i;
    int rc;

    memset(&cur, 0, sizeof(cur));
    memset(l, 0, sizeof(*l));
    if (page > 16)
        page = 16;
    while (!cur.done) {
        rc = efs_meta_apply_readdir(kv, dir, &cur, ent, page, &got);
        if (rc != EFS_OK)
            return rc;
        l->calls++;
        if (l->calls > 500)
            return EFS_ERR_IO; /* cursor is not advancing */
        for (i = 0; i < got; i++) {
            if (l->n >= 64)
                return EFS_ERR_NOMEM;
            snprintf(l->names[l->n], EFS_MAX_NAME, "%s", ent[i].name);
            l->n++;
        }
    }
    return EFS_OK;
}

static int list_has(const struct dir_list *l, const char *name)
{
    uint32_t i;

    for (i = 0; i < l->n; i++)
        if (strcmp(l->names[i], name) == 0)
            return 1;
    return 0;
}

static int list_dups(const struct dir_list *l)
{
    uint32_t i, j;

    for (i = 0; i < l->n; i++)
        for (j = i + 1; j < l->n; j++)
            if (strcmp(l->names[i], l->names[j]) == 0)
                return 1;
    return 0;
}

static void test_readdir_local(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct dir_list l;
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent ent[16];
    efs_ino_t ino = 0;
    uint32_t got = 0, pages[] = { 1, 2, 3, 16 };
    char nm[32];
    int i, k;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");

    /* Empty directory: one call, no entries, and it says so. */
    memset(&cur, 0, sizeof(cur));
    CHECK(efs_meta_apply_readdir(kv, EFS_ROOT_INO, &cur, ent, 16, &got) == EFS_OK,
          "readdir empty");
    CHECK(got == 0 && cur.done, "empty and done");

    for (i = 0; i < 10; i++) {
        snprintf(nm, sizeof(nm), "f%02d", i);
        CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                         nm, &ino) == EFS_OK, "create");
    }
    /* Every page size must produce the same 10 names, in key order, with no
     * duplicates — that is the whole readdir contract for one shard. */
    for (k = 0; k < (int)(sizeof(pages) / sizeof(pages[0])); k++) {
        CHECK(list_dir(kv, EFS_ROOT_INO, pages[k], &l) == EFS_OK, "list");
        CHECK(l.n == 10, "all entries");
        CHECK(!list_dups(&l), "no duplicates");
        CHECK(strcmp(l.names[0], "f00") == 0 && strcmp(l.names[9], "f09") == 0,
              "ordered");
    }
    /* Paging one at a time really pages: 10 entries cannot arrive in one call. */
    CHECK(list_dir(kv, EFS_ROOT_INO, 1, &l) == EFS_OK, "list");
    CHECK(l.calls >= 10, "paged");

    /* A name unlinked before the scan is absent; the type comes through. */
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "f05", T0) == EFS_OK, "unlink");
    CHECK(list_dir(kv, EFS_ROOT_INO, 4, &l) == EFS_OK, "list");
    CHECK(l.n == 9 && !list_has(&l, "f05"), "unlinked gone");

    memset(&cur, 0, sizeof(cur));
    CHECK(efs_meta_apply_readdir(kv, EFS_ROOT_INO, &cur, ent, 16, &got) == EFS_OK,
          "readdir");
    CHECK(got == 9 && (ent[0].d.type & S_IFMT) == S_IFREG, "dentry type");
    CHECK(ent[0].d.ino != 0, "dentry ino");

    /* A file is not a directory. */
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "f00", &ent[0].d) == EFS_OK,
          "lookup");
    memset(&cur, 0, sizeof(cur));
    CHECK(efs_meta_apply_readdir(kv, ent[0].d.ino, &cur, ent, 16, &got) ==
              EFS_ERR_INVAL, "not a directory");
    efs_kv_mem_free(kv);
}

/* A spread directory is up to 64 ranges on 64 shards. readdir has to visit
 * the lanes the directory actually uses — including lanes the migrator
 * populated — and must not return a name twice or resurrect a deleted one. */
static void test_readdir_spread(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct dir_list l;
    struct efs_meta_row r;
    efs_ino_t ino = 0;
    uint32_t page[] = { 1, 3, 16 };
    char nm[32];
    int i, k;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    for (i = 0; i < 12; i++) {
        snprintf(nm, sizeof(nm), "s%02d", i);
        CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                         nm, &ino) == EFS_OK, "create");
    }

    /* SPLITTING: entries still local, plus whatever has migrated. */
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    for (k = 0; k < (int)(sizeof(page) / sizeof(page[0])); k++) {
        CHECK(list_dir(kv, EFS_ROOT_INO, page[k], &l) == EFS_OK, "list");
        CHECK(l.n == 12, "all during SPLITTING");
        CHECK(!list_dups(&l), "no duplicates during SPLITTING");
    }

    /* Migrate a few, then check the overlap window: each name must appear
     * exactly once whether it has moved or not. */
    for (i = 0; i < 5; i++)
        CHECK(efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK, "migrate");
    for (k = 0; k < (int)(sizeof(page) / sizeof(page[0])); k++) {
        CHECK(list_dir(kv, EFS_ROOT_INO, page[k], &l) == EFS_OK, "list");
        CHECK(l.n == 12, "all mid-migration");
        CHECK(!list_dups(&l), "no duplicates mid-migration");
    }

    /* A delete during SPLITTING writes a tombstone on the hashed side; the
     * migrator must not bring the name back and readdir must not show it. */
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "s11", T0) == EFS_OK, "unlink");
    CHECK(list_dir(kv, EFS_ROOT_INO, 3, &l) == EFS_OK, "list");
    CHECK(l.n == 11 && !list_has(&l, "s11"), "tombstoned name hidden");
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_HASHED, "hashed");

    /* Fully hashed: the used-lane bitmap is now the only thing that tells
     * readdir where the entries are, so this is what a migrator that forgot
     * to record a lane would break. */
    for (k = 0; k < (int)(sizeof(page) / sizeof(page[0])); k++) {
        CHECK(list_dir(kv, EFS_ROOT_INO, page[k], &l) == EFS_OK, "list");
        CHECK(l.n == 11, "all when HASHED");
        CHECK(!list_dups(&l), "no duplicates when HASHED");
        CHECK(list_has(&l, "s00") && list_has(&l, "s10"), "specific names");
        CHECK(!list_has(&l, "s11"), "deleted stays deleted");
    }
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* Dir lane 0 is the directory's own inode shard, so a lane-0 name occupies
 * the same key in both layouts. That makes it the one name for which "move
 * it to the hashed side" and "check whether the hashed side owns it" are
 * both answered by the name's own record, and getting either backwards
 * either deletes the name or hides it. One name in 64 hits this, so a test
 * that does not pick a lane-0 name on purpose only covers it by luck. */
static void test_readdir_lane0(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct dir_list l;
    struct efs_meta_row r;
    struct efs_meta_dentry d;
    efs_ino_t ino = 0;
    const char *l0 = "al", *l0b = "be";
    char nm[32];
    int i;

    CHECK(kv != NULL, "kv");
    /* State the premise, so this test reports the cause and not a symptom
     * if the lane hash or the stride construction ever changes. */
    CHECK(efs_kv_dir_lane(l0) == 0 && efs_kv_dir_lane(l0b) == 0, "lane 0 names");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_kv_dentry_shard(EFS_ROOT_INO, l0, EFS_META_LAYOUT_HASHED) ==
              efs_kv_inode_shard(EFS_ROOT_INO), "lane 0 aliases parent shard");

    /* LOCAL: the aliasing must not stop an ordinary unlink from removing the
     * only copy of the name. */
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     l0, &ino) == EFS_OK, "create lane0");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, l0, T0) == EFS_OK, "unlink lane0");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, l0, &d) == EFS_ERR_NOT_FOUND,
          "lane0 unlinked in LOCAL dir");
    CHECK(list_dir(kv, EFS_ROOT_INO, 16, &l) == EFS_OK && l.n == 0,
          "empty after lane0 unlink");

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     l0, &ino) == EFS_OK, "recreate lane0");
    for (i = 0; i < 4; i++) {
        snprintf(nm, sizeof(nm), "n%02d", i);
        CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                         nm, &ino) == EFS_OK, "create");
    }

    /* SPLITTING: the local pass owns lane 0's range, so the lane-0 name is
     * listed exactly once and is not mistaken for an already-migrated name. */
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    CHECK(list_dir(kv, EFS_ROOT_INO, 2, &l) == EFS_OK, "list");
    CHECK(l.n == 5 && list_has(&l, l0), "lane0 survives SPLITTING");
    CHECK(!list_dups(&l), "no duplicates");

    /* A lane-0 name created during the split registers lane 0 in the bitmap,
     * which is what would make readdir scan that range a second time. */
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     l0b, &ino) == EFS_OK, "create lane0 mid-split");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              (r.used_shards & 1ull) != 0, "lane 0 registered");
    CHECK(list_dir(kv, EFS_ROOT_INO, 2, &l) == EFS_OK, "list");
    CHECK(l.n == 6 && !list_dups(&l), "no duplicate local range");

    /* The migrator must leave both lane-0 names alone and still finish. */
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              (r.used_shards & 1ull) != 0, "lane 0 still registered");
    CHECK(list_dir(kv, EFS_ROOT_INO, 2, &l) == EFS_OK, "list");
    CHECK(l.n == 6 && list_has(&l, l0) && list_has(&l, l0b),
          "lane0 names survive the migration");
    CHECK(!list_dups(&l), "no duplicates when HASHED");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, l0, &d) == EFS_OK,
          "lane0 still resolvable");

    /* And unlink still works once the aliased key is the hashed key. */
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, l0, T0) == EFS_OK, "unlink");
    CHECK(list_dir(kv, EFS_ROOT_INO, 3, &l) == EFS_OK, "list");
    CHECK(l.n == 5 && !list_has(&l, l0), "lane0 unlinked when HASHED");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* SETATTR mode/owner: the single-shard class. The rule that actually gets
 * broken in implementations is ctime-yes/mtime-no, so that is the assertion
 * this test exists for. */
static void test_setattr_mode_owner(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_row r;
    struct efs_meta_setattr sa;
    efs_ino_t ino = 0;
    uint64_t mt, ct;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "f", &ino) == EFS_OK, "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    mt = r.base_mtime;
    ct = r.base_ctime;
    CHECK(r.uid == 1000 && r.gid == 1000, "born with creator identity");

    /* chmod: permission bits change, the type does not, ctime moves, mtime
     * does not. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0600;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 5, &sa) == EFS_OK, "chmod");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK((r.mode & 07777u) == 0600, "mode set");
    CHECK((r.mode & S_IFMT) == S_IFREG, "type preserved");
    CHECK(r.base_ctime == T0 + 5, "chmod moved ctime");
    CHECK(r.base_mtime == mt, "chmod left mtime alone");

    /* A mode carrying a bogus type cannot reassign the inode's type. */
    sa.mode = S_IFDIR | 0755;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 6, &sa) == EFS_OK, "chmod2");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK((r.mode & S_IFMT) == S_IFREG, "type still regular");
    CHECK((r.mode & 07777u) == 0755, "perm bits applied");

    /* chown/chgrp, independently selectable. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_UID;
    sa.uid = 4242;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 7, &sa) == EFS_OK, "chown");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.uid == 4242 && r.gid == 1000, "uid only");
    CHECK((r.mode & 07777u) == 0755, "mode untouched by chown");

    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_GID;
    sa.gid = 77;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 8, &sa) == EFS_OK, "chgrp");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.uid == 4242 && r.gid == 77, "gid only");
    CHECK(r.base_mtime == mt && r.base_ctime == T0 + 8, "times");

    /* Apply is a pure function of the entry: re-applying a committed entry
     * must not move anything, and a clock that stepped backwards must not
     * drag ctime back with it. */
    sa.mask = EFS_META_SET_GID;
    sa.gid = 77;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 8, &sa) == EFS_OK, "replay");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.base_ctime == T0 + 8, "replay idempotent");
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 1, &sa) == EFS_OK, "backwards");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.base_ctime == T0 + 8, "ctime MAX-clamped");
    (void)ct;

    /* A stale inode handle is an error, not a chmod applied to whatever now
     * owns that ino. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0400;
    sa.expect_gen = r.generation + 1;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 9, &sa) == EFS_ERR_STALE,
          "stale handle rejected");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK((r.mode & 07777u) == 0755, "stale attempt changed nothing");
    sa.expect_gen = r.generation;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 9, &sa) == EFS_OK, "right gen");

    /* Wrong-class and malformed calls are refused rather than half-applied. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = 0x80u;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 10, &sa) == EFS_ERR_INVAL,
          "unknown mask bit");
    sa.mask = EFS_META_SET_MODE;
    CHECK(efs_meta_apply_setattr(kv, 999999, T0 + 10, &sa) == EFS_ERR_NOT_FOUND,
          "missing inode");
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 10, NULL) == EFS_ERR_INVAL,
          "null attrs");

    /* Directories take the same path. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0700;
    CHECK(efs_meta_apply_setattr(kv, EFS_ROOT_INO, T0 + 11, &sa) == EFS_OK,
          "chmod dir");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "get");
    CHECK((r.mode & S_IFMT) == S_IFDIR && (r.mode & 07777u) == 0700, "dir mode");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_row_attrs(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_row r, p;
    struct efs_meta_attrs at;
    efs_ino_t ino = 0;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    memset(&at, 0, sizeof(at));
    at.uid = 4242;
    at.gid = 77;
    at.now = T0 + 5;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, "a",
                                     &ino) == EFS_OK, "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.uid == 4242 && r.gid == 77, "identity from the entry");
    CHECK(r.base_mtime == T0 + 5 && r.base_ctime == T0 + 5 &&
              r.base_atime == T0 + 5, "times from the entry");
    CHECK(r.mtime_gen == 0, "no utimens yet");
    /* Adding an entry moves the containing directory's mtime and ctime. */
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &p) == EFS_OK, "root");
    CHECK(p.base_mtime == T0 + 5 && p.base_ctime == T0 + 5, "parent times");

    /* A clock that steps backwards must not move a directory's mtime back. */
    at.now = T0 + 1;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, "b",
                                     &ino) == EFS_OK, "create older");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &p) == EFS_OK, "root");
    CHECK(p.base_mtime == T0 + 5, "parent mtime is MAX-clamped");

    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "a", T0 + 9) == EFS_OK,
          "unlink");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &p) == EFS_OK, "root");
    CHECK(p.base_mtime == T0 + 9 && p.base_ctime == T0 + 9, "unlink moved times");
    efs_kv_mem_free(kv);
}

/* A coordinator over the same KV, as a real shard leader would be. */
struct coord_ctx {
    struct efs_kv *kv;
    int fail; /* the authority cannot be reached (I9) */
};

static int coord_fn(void *user, const struct efs_txid *t, uint32_t shard,
                    int *dec)
{
    struct coord_ctx *c = user;

    if (c->fail)
        return EFS_ERR_IO;
    return efs_txn_decision_get(c->kv, shard, t, dec);
}

static struct efs_txn_parts one_part(uint32_t s)
{
    struct efs_txn_parts p;

    memset(&p, 0, sizeof(p));
    p.n = 1;
    p.shard[0] = s;
    return p;
}

/* Prepares a committed reduction on a lane without materializing it: the
 * state a stat() must still see, because a transaction is visible at its
 * decision and the reducer runs later. */
static void commit_reduce(struct efs_kv *kv, uint8_t idb, efs_ino_t ino,
                          uint64_t gen, uint8_t lane,
                          const struct efs_txn_reduce *red, int decide)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0, shard = efs_kv_lane_shard(ino, lane);
    struct efs_txn_parts p = one_part(shard);
    struct efs_txid t;

    memset(&t, 0, sizeof(t));
    t.bytes[0] = idb;
    CHECK(efs_kv_key_lane(shard, ino, gen, lane, key, &klen) == EFS_OK, "lane key");
    CHECK(efs_txn_prepare_reduce(kv, &t, &p, key, klen, red) == EFS_OK, "prepare");
    if (decide)
        CHECK(efs_txn_decide(kv, efs_txn_coordinator(&t, &p), &t,
                             EFS_TXN_COMMIT) == EFS_OK, "decide");
}

static void pub_ch(struct efs_kv *kv, efs_ino_t ino, uint32_t ci, uint64_t end,
                   uint64_t gen, uint64_t expect, uint64_t now,
                   const struct efs_meta_chunk *ch, const char *msg)
{
    struct efs_meta_pub p;
    struct efs_meta_row r;

    memset(&p, 0, sizeof(p));
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row for epoch");
    p.ino = ino;
    p.chunk_index = ci;
    p.new_size = end;
    p.expected_gen = expect;
    p.candidate_gen = gen;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.content_epoch = r.content_epoch; /* a live writer knows the epoch */
    p.now = now;
    p.ch = *ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, msg);
}

static void pub(struct efs_kv *kv, efs_ino_t ino, uint32_t ci, uint64_t end,
                uint64_t gen, uint64_t expect, uint64_t now, const char *msg)
{
    struct efs_meta_chunk ch;

    fill_ch(&ch);
    pub_ch(kv, ino, ci, end, gen, expect, now, &ch, msg);
}

static int utimens_at(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                      uint32_t mask, uint64_t mtime, uint64_t atime)
{
    struct efs_meta_row r;
    struct efs_meta_utimens u;
    int rc;

    rc = efs_meta_apply_get_inode(kv, ino, &r);
    if (rc != EFS_OK)
        return rc;
    memset(&u, 0, sizeof(u));
    u.mask = mask;
    u.mtime = mtime;
    u.atime = atime;
    u.expect_gen = r.generation;
    if (mask & EFS_META_SET_MTIME)
        u.mtime_gen = r.mtime_gen + 1;
    return efs_meta_apply_utimens(kv, ino, now, &u);
}

/* A row whose creating txn is COMMITted but not yet RESOLVEd on the row's
 * shard (coordinator lost its RESOLVEs; recovery is seconds away) must be
 * visible to a handler-side stat: the client already holds OK, or gets it
 * from the op-id window on the dentry shard. Sep 23 2026: `mkdir ENOENT`
 * on a directory that exists (freeze run, fcstor013). UNDECIDED stays
 * absent; ABORT stays absent; the plain apply-path read never sees it. */
static void test_stat_committed_unresolved_row(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_stat st;
    struct efs_meta_row row;
    struct efs_txid t;
    struct efs_txn_parts p;
    uint8_t key[EFS_KV_KEY_MAX], img[512];
    uint32_t klen = 0, n, coord, sh;
    efs_ino_t ino = 0;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    /* Build a real row image the simple way, then turn it back into a
     * pending intent. */
    CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "d",
                               &ino) == EFS_OK, "mkdir");
    sh = efs_kv_inode_shard(ino);
    CHECK(efs_kv_key_inode(sh, ino, key, &klen) == EFS_OK, "key");
    n = sizeof(img);
    CHECK(efs_kv_get(kv, key, klen, img, &n) == EFS_OK, "row image");
    CHECK(efs_kv_del(kv, key, klen) == EFS_OK, "drop row");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) ==
              EFS_ERR_NOT_FOUND, "gone");

    memset(&t, 0, sizeof(t));
    t.bytes[0] = 0x5c;
    p = one_part(sh);
    CHECK(efs_txn_prepare_excl(kv, &t, &p, key, klen, 0, EFS_TXN_PUT, img, n) ==
              EFS_OK, "intent");
    /* UNDECIDED: not visible. */
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) ==
              EFS_ERR_NOT_FOUND, "undecided absent");
    /* Authority unreachable while an intent is met: IO, never absence (I9). */
    cc.fail = 1;
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_ERR_IO,
          "no authority = IO");
    cc.fail = 0;
    /* COMMIT at the coordinator, RESOLVE still pending: visible. */
    coord = efs_txn_coordinator(&t, &p);
    CHECK(efs_txn_decide(kv, coord, &t, EFS_TXN_COMMIT) == EFS_OK, "decide");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_ERR_NOT_FOUND,
          "apply-path read still absent");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK,
          "committed unresolved row visible to stat");
    CHECK(st.ino == ino && S_ISDIR(st.mode) && st.nlink == 2, "row attrs");
    CHECK(efs_meta_apply_get_inode_tx(kv, ino, coord_fn, &cc, &row) == EFS_OK &&
              row.ino == ino, "get_inode_tx");
    CHECK(efs_meta_apply_get_inode_tx(kv, ino, NULL, NULL, &row) ==
              EFS_ERR_NOT_FOUND, "NULL coord = plain read");
    /* After RESOLVE the plain read sees it too. */
    CHECK(efs_txn_resolve(kv, &t, sh, EFS_TXN_COMMIT) == EFS_OK, "resolve");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK, "materialized");
    efs_kv_mem_free(kv);
}

/* Same window, on the dentry. rmdir lookup of a name whose creating txn
 * is COMMITted but not yet RESOLVEd must see the dentry; a plain lookup
 * must not. Sep 23 freeze: d-fcstor011-1-22 existed, rmdir returned
 * ENOENT. */
static void test_lookup_committed_unresolved_dentry(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_dentry dent;
    struct efs_meta_row root, row;
    struct efs_txid t;
    struct efs_txn_parts p;
    uint8_t key[EFS_KV_KEY_MAX], img[512];
    uint32_t klen = 0, n, sh, coord;
    efs_ino_t ino = 0;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_mkdir(kv, &g_at, EFS_ROOT_INO, S_IFDIR | 0755, "c",
                               &ino) == EFS_OK, "mkdir");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &root) == EFS_OK, "root");
    sh = root.layout == EFS_META_LAYOUT_LOCAL
             ? efs_kv_inode_shard(EFS_ROOT_INO)
             : efs_kv_dentry_shard(EFS_ROOT_INO, "c", EFS_META_LAYOUT_HASHED);
    CHECK(efs_kv_key_dentry(sh, EFS_ROOT_INO, "c", key, &klen) == EFS_OK, "key");
    n = sizeof(img);
    CHECK(efs_kv_get(kv, key, klen, img, &n) == EFS_OK, "dentry image");
    CHECK(efs_kv_del(kv, key, klen) == EFS_OK, "drop dentry");
    CHECK(efs_meta_apply_lookup_tx(kv, EFS_ROOT_INO, "c", coord_fn, &cc,
                                   &dent) == EFS_ERR_NOT_FOUND, "gone");

    memset(&t, 0, sizeof(t));
    t.bytes[0] = 0x5d;
    p = one_part(sh);
    CHECK(efs_txn_prepare_excl(kv, &t, &p, key, klen, 0, EFS_TXN_PUT, img, n) ==
              EFS_OK, "intent");
    CHECK(efs_meta_apply_lookup_tx(kv, EFS_ROOT_INO, "c", coord_fn, &cc,
                                   &dent) == EFS_ERR_NOT_FOUND, "undecided absent");
    cc.fail = 1;
    CHECK(efs_meta_apply_lookup_tx(kv, EFS_ROOT_INO, "c", coord_fn, &cc,
                                   &dent) == EFS_ERR_IO, "no authority = IO");
    cc.fail = 0;
    coord = efs_txn_coordinator(&t, &p);
    CHECK(efs_txn_decide(kv, coord, &t, EFS_TXN_COMMIT) == EFS_OK, "decide");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "c", &dent) ==
              EFS_ERR_NOT_FOUND, "plain lookup still absent");
    CHECK(efs_meta_apply_lookup_tx(kv, EFS_ROOT_INO, "c", coord_fn, &cc,
                                   &dent) == EFS_OK && dent.ino == ino,
          "committed unresolved dentry visible");
    CHECK(efs_meta_apply_resolve_tx(kv, EFS_ROOT_INO, "c", coord_fn, &cc, &dent,
                                    &row) == EFS_OK && row.ino == ino,
          "resolve sees the row");
    CHECK(efs_txn_resolve(kv, &t, sh, EFS_TXN_COMMIT) == EFS_OK, "resolve");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "c", &dent) == EFS_OK &&
              dent.ino == ino, "materialized");
    efs_kv_mem_free(kv);
}

static void test_stat_collect(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_stat st;
    struct efs_txn_reduce red;
    efs_ino_t ino = 0;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "s",
                                     &ino) == EFS_OK, "create");

    /* A file nobody has written has no lanes at all: size and times come
     * straight off the row, and stat() costs one read. */
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.lanes == 0 && st.attempts == 1, "no lanes");
    CHECK(st.size == 0 && st.mtime == T0 && st.uid == 1000 && st.nlink == 1,
          "row attrs");
    CHECK((st.mode & S_IFMT) == S_IFREG, "mode");
    CHECK(st.alloc == 0, "no lanes, no present chunks");

    /* One lane. */
    pub(kv, ino, 0, 64, 0xA1, 0, T0 + 5, "publish lane 0");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.lanes == 1 && st.size == 64, "lane size");
    CHECK(st.mtime == T0 + 5 && st.ctime == T0 + 5, "write moved both times");
    CHECK(st.atime == T0, "noatime: reads do not move atime");
    CHECK(st.alloc == 1, "one present chunk");

    /* A second lane, on a different shard, with a larger high-water mark.
     * This is the case a single row could not represent. */
    pub(kv, ino, 1, 200, 0xB2, 0, T0 + 7, "publish lane 1");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.lanes == 2 && st.size == 200, "MAX over lanes");
    CHECK(st.mtime == T0 + 7, "newest write wins");
    CHECK(st.alloc == 2, "present counts sum across lanes");

    /* A replay of a landed publish is a no-op, count included. */
    pub(kv, ino, 0, 64, 0xA1, 0, T0 + 5, "replay publish lane 0");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.alloc == 2, "replay does not double count");

    /* Committed, not yet materialized: a write that has returned to its
     * caller. Ignoring it would report a size older than that write. */
    memset(&red, 0, sizeof(red));
    red.max_end = 500;
    red.max_mtime = T0 + 9;
    red.max_ctime = T0 + 9;
    commit_reduce(kv, 1, ino, 1, 1, &red, 1);
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.size == 500 && st.mtime == T0 + 9, "committed reduction counted");
    CHECK(st.alloc == 2, "a reduction carries no present count");

    /* Prepared but undecided: NOT part of the state yet, and its mere
     * presence must not make the collect retry — the vector is still
     * consistent, so attempts stays 1. */
    memset(&red, 0, sizeof(red));
    red.max_end = 9000;
    red.max_mtime = T0 + 50;
    commit_reduce(kv, 2, ino, 1, 0, &red, 0);
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.size == 500 && st.mtime == T0 + 9, "undecided excluded");
    CHECK(st.attempts == 1, "undecided alone is not a retry");

    /* An unreachable authority is a resource failure. Answering "absent"
     * here would publish a size the file may not have (I9). */
    cc.fail = 1;
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_ERR_IO,
          "I9: never absent");
    cc.fail = 0;

    /* Once that transaction's decision lands it counts, without anything
     * having to materialize it. */
    {
        struct efs_txn_parts p = one_part(efs_kv_lane_shard(ino, 0));
        struct efs_txid t;

        memset(&t, 0, sizeof(t));
        t.bytes[0] = 2;
        CHECK(efs_txn_decide(kv, efs_txn_coordinator(&t, &p), &t,
                             EFS_TXN_COMMIT) == EFS_OK, "late decide");
    }
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.size == 9000 && st.mtime == T0 + 50, "decision is the visibility");

    /* More undecided transactions than the collect can track. It cannot
     * prove its vector held, so it must not answer: a plausible-looking size
     * that no serialization produced is worse than a retry. */
    {
        int i;

        for (i = 0; i < EFS_TXN_MAX_PENDING + 1; i++) {
            struct efs_txn_reduce r2;

            memset(&r2, 0, sizeof(r2));
            r2.max_end = 1000000 + (uint64_t)i;
            commit_reduce(kv, (uint8_t)(20 + i), ino, 1, 0, &r2, 0);
        }
    }
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_ERR_BUSY,
          "unvalidatable read retries, then gives up");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* Writes a row back. Truncate is not built yet, so the test stands in for
 * the row half of what it will commit. utimens has its own apply. */
static void put_row(struct efs_kv *kv, const struct efs_meta_row *r)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_INO_BYTES];
    uint32_t klen = 0;

    CHECK(efs_kv_key_inode(efs_kv_inode_shard(r->ino), r->ino, key, &klen) ==
              EFS_OK, "ino key");
    CHECK(efs_meta_pack_inode(r, val, sizeof(val)) == EFS_OK, "pack row");
    CHECK(efs_kv_put(kv, key, klen, val, sizeof(val)) == EFS_OK, "put row");
}

/* LOOKUP_PATH has to walk directories, but MKDIR is a 2-shard txn and is
 * not this module. A planted LOCAL dir is enough to exercise the walk. */
static void plant_dir(struct efs_kv *kv, efs_ino_t parent, const char *name,
                      efs_ino_t ino, uint32_t mode)
{
    struct efs_meta_row r;
    struct efs_meta_dentry d;
    uint8_t k_dent[EFS_KV_KEY_MAX], v_dent[EFS_META_DENT_BYTES];
    uint32_t kd = 0;

    memset(&r, 0, sizeof(r));
    r.ino = ino;
    r.generation = 1;
    r.mode = mode;
    r.nlink = S_ISDIR(mode) ? 2 : 1;
    r.parent = parent;
    r.uid = 1000;
    r.gid = 1000;
    r.base_mtime = r.base_atime = r.base_ctime = T0;
    put_row(kv, &r);
    memset(&d, 0, sizeof(d));
    d.ino = ino;
    d.generation = 1;
    d.type = mode & S_IFMT;
    CHECK(efs_kv_key_dentry(efs_kv_inode_shard(parent), parent, name, k_dent,
                            &kd) == EFS_OK,
          "dent key");
    CHECK(efs_meta_pack_dentry(&d, v_dent, sizeof(v_dent)) == EFS_OK, "pack dent");
    CHECK(efs_kv_put(kv, k_dent, kd, v_dent, sizeof(v_dent)) == EFS_OK, "put dent");
}

/* Same-dir directory rename bumps parent_version and keeps the inode. */
static void test_dir_rename(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    efs_ino_t d = 17;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    plant_dir(kv, EFS_ROOT_INO, "d", d, S_IFDIR | 0755);
    CHECK(efs_meta_apply_get_inode(kv, d, &r) == EFS_OK && r.parent_version == 0,
          "pver0");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, "d", EFS_ROOT_INO, "e", T0 + 1) ==
              EFS_OK,
          "rename");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "d", &dent) == EFS_ERR_NOT_FOUND,
          "old gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "e", &dent) == EFS_OK &&
              dent.ino == d && (dent.type & S_IFMT) == S_IFDIR,
          "new name");
    CHECK(efs_meta_apply_get_inode(kv, d, &r) == EFS_OK && r.parent == EFS_ROOT_INO &&
              r.parent_version == 1 && S_ISDIR(r.mode),
          "pver");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, "e", EFS_ROOT_INO, "e", T0 + 1) ==
              EFS_OK,
          "self");
    efs_kv_mem_free(kv);
}

/* Cross-dir file rename: dest parent changes, src name gone.
 * Plant the file too: create_file's first ino on shard S is S itself, which
 * would clobber a planted parent at 21/37. */
static void test_rename_cross_dir(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    efs_ino_t a = 21, b = 37, f = 53;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    plant_dir(kv, EFS_ROOT_INO, "a", a, S_IFDIR | 0755);
    plant_dir(kv, EFS_ROOT_INO, "b", b, S_IFDIR | 0755);
    plant_dir(kv, a, "f", f, S_IFREG | 0644);
    CHECK(efs_meta_apply_rename(kv, a, "f", b, "g", T0 + 1) == EFS_OK, "cross");
    CHECK(efs_meta_apply_lookup(kv, a, "f", &dent) == EFS_ERR_NOT_FOUND,
          "src gone");
    CHECK(efs_meta_apply_lookup(kv, b, "g", &dent) == EFS_OK && dent.ino == f,
          "dest");
    CHECK(efs_meta_apply_get_inode(kv, f, &r) == EFS_OK && r.parent == b,
          "parent");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, "a", a, "x", T0 + 2) ==
              EFS_ERR_INVAL,
          "into self");
    efs_kv_mem_free(kv);
}

/* HASHED same-dir file rename: dest name on a distinct off-home lane,
 * src gone, parent row times stay on dir-lanes. */
static void test_rename_hashed(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    struct efs_meta_attrs at;
    efs_ino_t f = 0;
    char na[16], nb[16];
    uint64_t born;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    born = r.base_mtime;
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = T0 + 1;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, na,
                                     &f) == EFS_OK &&
              f,
          "create");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, nb, T0 + 2) ==
              EFS_OK,
          "hashed rename");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, na, &dent) ==
              EFS_ERR_NOT_FOUND,
          "old gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, nb, &dent) == EFS_OK &&
              dent.ino == f,
          "new name");
    CHECK(efs_meta_apply_get_inode(kv, f, &r) == EFS_OK &&
              r.parent == EFS_ROOT_INO,
          "parent");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_HASHED && r.base_mtime == born,
          "hashed rename did not stamp the inode row's times");
    efs_kv_mem_free(kv);
}

/* HASHED parent last-link unlink: src gone, parent row times stay on
 * dir-lanes. */
static void test_unlink_hashed(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    struct efs_meta_attrs at;
    efs_ino_t f = 0;
    char na[16], nb[16];
    uint64_t born;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    born = r.base_mtime;
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = T0 + 1;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, na,
                                     &f) == EFS_OK &&
              f,
          "create");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, na, T0 + 2) == EFS_OK,
          "hashed unlink");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, na, &dent) ==
              EFS_ERR_NOT_FOUND,
          "gone");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_HASHED && r.base_mtime == born,
          "hashed unlink did not stamp the inode row's times");
    efs_kv_mem_free(kv);
}

/* HASHED dest LINK: alias on a distinct off-home lane, nlink=2, parent
 * row times stay on dir-lanes. */
static void test_link_hashed(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    struct efs_meta_attrs at;
    efs_ino_t f = 0;
    char na[16], nb[16];
    uint64_t born;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    born = r.base_mtime;
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = T0 + 1;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, na,
                                     &f) == EFS_OK &&
              f,
          "create");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, nb, T0 + 2) ==
              EFS_OK,
          "hashed link");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, nb, &dent) == EFS_OK &&
              dent.ino == f,
          "alias");
    CHECK(efs_meta_apply_get_inode(kv, f, &r) == EFS_OK && r.nlink == 2,
          "nlink 2");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_HASHED && r.base_mtime == born,
          "hashed link did not stamp the inode row's times");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, nb, T0 + 2) ==
              EFS_ERR_EXIST,
          "dup dest");
    efs_kv_mem_free(kv);
}

/* SPLITTING dest LINK writes hashed. Tombstone dest is not EXIST. */
static void test_link_splitting(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    struct efs_meta_attrs at;
    efs_ino_t f = 0, pre = 0;
    char na[16], nb[16];
    uint64_t born;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = T0 + 1;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, "pre",
                                     &pre) == EFS_OK &&
              pre,
          "pre");
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_SPLITTING,
          "SPLITTING");
    born = r.base_mtime;
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, na,
                                     &f) == EFS_OK &&
              f,
          "create hashed");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, nb, T0 + 2) ==
              EFS_OK,
          "splitting dest link");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, nb, &dent) == EFS_OK &&
              dent.ino == f,
          "alias");
    CHECK(efs_meta_apply_get_inode(kv, f, &r) == EFS_OK && r.nlink == 2,
          "nlink 2");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_SPLITTING && r.base_mtime == born,
          "splitting dest link did not stamp the inode row's times");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, nb, T0 + 2) ==
              EFS_ERR_EXIST,
          "dup dest");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, "pre",
                              T0 + 2) == EFS_ERR_EXIST,
          "local leftover dest");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, na, T0 + 3) == EFS_OK,
          "unlink src");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, nb, EFS_ROOT_INO, na, T0 + 4) ==
              EFS_OK,
          "link onto tombstone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, na, &dent) == EFS_OK &&
              dent.ino == f,
          "tombstone dest");
    CHECK(efs_meta_apply_get_inode(kv, f, &r) == EFS_OK && r.nlink == 2,
          "nlink 2 after tombstone dest");
    efs_kv_mem_free(kv);
}

/* SPLITTING dest RENAME writes hashed. Src drop is I8. Apply helper
 * does not replace (EXIST); leftover dest stays. Leftover src can
 * move onto a tombstone dest. */
static void test_rename_splitting(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    struct efs_meta_attrs at;
    efs_ino_t f = 0, pre = 0, q = 0;
    char na[16], nb[16];

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = T0 + 1;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, "pre",
                                     &pre) == EFS_OK &&
              pre,
          "pre");
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, "q",
                                     &q) == EFS_OK &&
              q,
          "q");
    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK &&
              r.layout == EFS_META_LAYOUT_SPLITTING,
          "SPLITTING");
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, na,
                                     &f) == EFS_OK &&
              f,
          "create hashed");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, na, EFS_ROOT_INO, nb, T0 + 2) ==
              EFS_OK,
          "hashed dest");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, na, &dent) ==
              EFS_ERR_NOT_FOUND,
          "src gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, nb, &dent) == EFS_OK &&
              dent.ino == f,
          "hashed dest");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, nb, EFS_ROOT_INO, "q",
                                T0 + 3) == EFS_ERR_EXIST,
          "leftover dest EXIST");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, "pre", EFS_ROOT_INO, na,
                                T0 + 4) == EFS_OK,
          "leftover src onto tombstone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "pre", &dent) ==
              EFS_ERR_NOT_FOUND,
          "pre gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, na, &dent) == EFS_OK &&
              dent.ino == pre,
          "tombstone dest");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "q", &dent) == EFS_OK &&
              dent.ino == q,
          "leftover dest still");
    efs_kv_mem_free(kv);
}

/* HASHED empty child rmdir from a LOCAL parent. */
static void test_rmdir_hashed(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_dentry dent;
    struct efs_meta_row r;
    efs_ino_t d = 41;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    plant_dir(kv, EFS_ROOT_INO, "hd", d, S_IFDIR | 0755);
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    r.nlink++;
    put_row(kv, &r);
    CHECK(efs_meta_dir_begin_split(kv, d) == EFS_OK, "split");
    while (efs_meta_dir_migrate_one(kv, d) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, d) == EFS_OK, "HASHED child");
    CHECK(efs_meta_apply_rmdir(kv, EFS_ROOT_INO, "hd", T0 + 1) == EFS_OK,
          "rmdir hashed");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "hd", &dent) ==
              EFS_ERR_NOT_FOUND,
          "gone");
    efs_kv_mem_free(kv);
}

static void test_lookup_path(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_path_hop hops[EFS_META_PATH_MAX];
    uint32_t n = 0;
    efs_ino_t a = 100, b = 200, f = 300;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint32_t ki = 0;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    plant_dir(kv, EFS_ROOT_INO, "a", a, S_IFDIR | 0755);
    plant_dir(kv, a, "b", b, S_IFDIR | 0700);
    plant_dir(kv, b, "f", f, S_IFREG | 0644);

    CHECK(efs_meta_apply_lookup_path(kv, 0, "/", hops, 8, &n) == EFS_OK &&
              n == 1 && hops[0].ino == EFS_ROOT_INO,
          "empty is start");
    CHECK(efs_meta_apply_lookup_path(kv, 0, "/a/b/f", hops, 8, &n) == EFS_OK &&
              n == 3,
          "deep");
    CHECK(hops[0].ino == a && S_ISDIR(hops[0].mode), "a");
    CHECK(hops[1].ino == b && (hops[1].mode & 07777u) == 0700, "b mode");
    CHECK(hops[2].ino == f && S_ISREG(hops[2].mode) && hops[2].uid == 1000, "f");

    /* Resume: cap 2 of a 3-component path, then continue from the terminal. */
    CHECK(efs_meta_apply_lookup_path(kv, 0, "/a/b/f", hops, 2, &n) == EFS_OK &&
              n == 2 && hops[1].ino == b,
          "first batch");
    CHECK(efs_meta_apply_lookup_path(kv, hops[1].ino, "f", hops, 8, &n) ==
              EFS_OK &&
              n == 1 && hops[0].ino == f,
          "resume");

    CHECK(efs_meta_apply_lookup_path(kv, 0, "/a//b/", hops, 8, &n) == EFS_OK &&
              n == 2 && hops[1].ino == b,
          "extra slashes");
    CHECK(efs_meta_apply_lookup_path(kv, 0, "/a/nope", hops, 8, &n) ==
              EFS_ERR_NOT_FOUND,
          "missing");
    CHECK(efs_meta_apply_lookup_path(kv, 0, "/a/b/f/x", hops, 8, &n) ==
              EFS_ERR_INVAL,
          "through a file");

    {
        char longn[EFS_MAX_NAME + 8];
        memset(longn, 'x', sizeof(longn) - 1);
        longn[0] = '/';
        longn[sizeof(longn) - 1] = '\0';
        CHECK(efs_meta_apply_lookup_path(kv, 0, longn, hops, 8, &n) ==
                  EFS_ERR_NAMETOOLONG,
              "component too long");
    }

    /* I9: the name exists, the inode does not. Absence would let a CREATE
     * mint a second ino for a live object. */
    CHECK(efs_kv_key_inode(efs_kv_inode_shard(b), b, k_ino, &ki) == EFS_OK,
          "b key");
    CHECK(efs_kv_del(kv, k_ino, ki) == EFS_OK, "drop b");
    CHECK(efs_meta_apply_lookup_path(kv, 0, "/a/b/f", hops, 8, &n) == EFS_ERR_IO,
          "I9");
    efs_kv_mem_free(kv);
}

static void test_stat_fence_and_gen(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_stat st;
    struct efs_meta_row r;
    efs_ino_t ino = 0;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "t",
                                     &ino) == EFS_OK, "create");
    pub(kv, ino, 0, 64, 0xA1, 0, T0 + 5, "lane 0");
    pub(kv, ino, 1, 4096, 0xB2, 0, T0 + 6, "lane 1");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.size == 4096, "grown");

    /* Truncate's fence has to reach every active lane. A lane it skipped
     * would keep claiming the pre-truncate high-water mark and win the MAX,
     * undoing the truncate — and lane 1 is not on the inode's shard, so
     * "fence the inode's lane" is not enough. */
    CHECK(efs_meta_apply_epoch_fence(kv, ino) == EFS_OK, "fence");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    r.base_size = 10; /* what a truncate to 10 writes on the row */
    put_row(kv, &r);
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.size == 10, "no lane resurrects the old size");
    CHECK(st.mtime == T0 + 6, "a truncate does not move time backwards");

    /* utimens fences lane mtimes by generation, because it is the only
     * setter that can move a time BACKWARDS. A lane stamped before the bump
     * must not win the MAX against the value utimens wrote. */
    CHECK(utimens_at(kv, ino, T0 + 2, EFS_META_SET_MTIME, T0 + 2, 0) == EFS_OK,
          "utimens");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 2, "utimens wins over stale lane mtimes");

    /* A write after the bump re-stamps its lane at the new generation and is
     * visible again. */
    pub(kv, ino, 1, 4096, 0xC3, 0xB2, T0 + 20, "post-utimens write");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 20, "new write visible");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_stat_dir_hashed(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_stat st;
    struct efs_meta_row r;
    struct efs_meta_attrs at;
    struct efs_meta_setattr sa;
    struct efs_txn_reduce red;
    efs_ino_t a = 0, b = 0;
    char na[16], nb[16];
    uint64_t born;
    uint8_t lane_b;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    born = r.base_mtime;

    CHECK(efs_meta_dir_begin_split(kv, EFS_ROOT_INO) == EFS_OK, "split");
    while (efs_meta_dir_migrate_one(kv, EFS_ROOT_INO) == EFS_OK)
        ;
    CHECK(efs_meta_dir_finish_hashed(kv, EFS_ROOT_INO) == EFS_OK, "HASHED");

    /* A LOCAL getattr is the row. After HASHED, creates must not funnel
     * times back onto that row or the spread was a no-op. */
    memset(&at, 0, sizeof(at));
    at.uid = 1000;
    at.gid = 1000;
    at.now = T0 + 10;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, na,
                                     &a) == EFS_OK,
          "create a");
    at.now = T0 + 20;
    CHECK(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, nb,
                                     &b) == EFS_OK,
          "create b");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "row");
    CHECK(r.layout == EFS_META_LAYOUT_HASHED, "still HASHED");
    CHECK(r.base_mtime == born && r.base_ctime == born,
          "hashed create did not touch the inode row's times");
    CHECK(efs_meta_apply_getattr(kv, EFS_ROOT_INO, coord_fn, &cc, &st) == EFS_OK,
          "stat");
    CHECK(st.mtime == T0 + 20 && st.ctime == T0 + 20, "MAX over dir lanes");
    CHECK(st.lanes >= 2, "both lanes collected");
    CHECK(st.size == r.base_size, "dir size is not a lane MAX");

    /* chmod still lives on the row (ctime, not mtime) and wins the MAX. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0700;
    CHECK(efs_meta_apply_setattr(kv, EFS_ROOT_INO, T0 + 30, &sa) == EFS_OK,
          "chmod dir");
    CHECK(efs_meta_apply_getattr(kv, EFS_ROOT_INO, coord_fn, &cc, &st) == EFS_OK,
          "stat");
    CHECK(st.mtime == T0 + 20, "chmod left mtime");
    CHECK(st.ctime == T0 + 30, "chmod ctime on the row");
    CHECK((st.mode & 07777u) == 0700, "mode");

    /* Committed reduction on a dir lane counts before materialization. */
    lane_b = efs_kv_dir_lane(nb);
    memset(&red, 0, sizeof(red));
    red.max_mtime = T0 + 40;
    red.max_ctime = T0 + 40;
    commit_reduce(kv, 3, EFS_ROOT_INO, r.generation, lane_b, &red, 1);
    CHECK(efs_meta_apply_getattr(kv, EFS_ROOT_INO, coord_fn, &cc, &st) == EFS_OK,
          "stat");
    CHECK(st.mtime == T0 + 40 && st.ctime == T0 + 40, "dir-lane reduction");

    /* dir_mtime_gen is the row's mtime_gen: a backwards utimens hides older
     * lane mtimes the same way a file's does. */
    CHECK(utimens_at(kv, EFS_ROOT_INO, T0 + 3, EFS_META_SET_MTIME, T0 + 3, 0) ==
              EFS_OK,
          "utimens dir");
    CHECK(efs_meta_apply_getattr(kv, EFS_ROOT_INO, coord_fn, &cc, &st) == EFS_OK,
          "stat");
    CHECK(st.mtime == T0 + 3, "utimens wins over stale dir-lane mtimes");

    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, na, T0 + 50) == EFS_OK,
          "unlink");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "row");
    CHECK(r.base_mtime == T0 + 3, "unlink did not touch the row");
    CHECK(efs_meta_apply_getattr(kv, EFS_ROOT_INO, coord_fn, &cc, &st) == EFS_OK,
          "stat");
    CHECK(st.mtime == T0 + 50, "unlink stamped its dir lane");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_utimens_fence(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_stat st;
    struct efs_meta_row r;
    struct efs_meta_setattr sa;
    struct efs_meta_utimens u;
    efs_ino_t ino = 0;
    uint64_t gen;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "u",
                                     &ino) == EFS_OK, "create");
    pub(kv, ino, 0, 64, 0xA1, 0, T0 + 10, "lane 0");
    pub(kv, ino, 1, 4096, 0xB2, 0, T0 + 11, "lane 1");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 11, "write mtime");

    /* atime-only does not fence: lane mtimes must still win. */
    CHECK(utimens_at(kv, ino, T0 + 12, EFS_META_SET_ATIME, 0, T0 + 1) == EFS_OK,
          "atime only");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.mtime_gen == 0, "atime did not bump mtime_gen");
    CHECK(r.base_atime == T0 + 1, "atime assigned exactly");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 11, "lane mtime still visible");
    CHECK(st.atime == T0 + 1, "getattr atime");

    CHECK(utimens_at(kv, ino, T0 + 13, EFS_META_SET_MTIME, T0 + 2, 0) == EFS_OK,
          "backwards");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.mtime_gen == 1 && r.base_mtime == T0 + 2, "row");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 2, "utimens wins");
    CHECK(st.ctime == T0 + 13, "utimens moved ctime");

    /* chmod after utimens must not hide the explicit mtime: it does not bump
     * mtime_gen, so a generation that guarded both times would be a bug. */
    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0600;
    CHECK(efs_meta_apply_setattr(kv, ino, T0 + 20, &sa) == EFS_OK, "chmod");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 2, "chmod left utimens mtime");
    CHECK(st.ctime == T0 + 20, "chmod ctime");

    /* Re-applying the same generation is a no-op (Raft replay). */
    gen = r.mtime_gen;
    memset(&u, 0, sizeof(u));
    u.mask = EFS_META_SET_MTIME;
    u.mtime = T0 + 2;
    u.mtime_gen = gen;
    u.expect_gen = r.generation;
    CHECK(efs_meta_apply_utimens(kv, ino, T0 + 13, &u) == EFS_OK, "replay");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.mtime_gen == gen, "replay did not bump");

    memset(&u, 0, sizeof(u));
    u.mask = EFS_META_SET_MTIME;
    u.mtime = T0 + 4;
    u.mtime_gen = gen + 1;
    u.expect_gen = r.generation + 1;
    CHECK(efs_meta_apply_utimens(kv, ino, T0 + 14, &u) == EFS_ERR_STALE,
          "stale handle");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "get");
    CHECK(r.base_mtime == T0 + 2, "stale changed nothing");

    pub(kv, ino, 1, 4096, 0xC3, 0xB2, T0 + 30, "write after fence");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.mtime == T0 + 30, "new write visible");

    memset(&u, 0, sizeof(u));
    CHECK(efs_meta_apply_utimens(kv, ino, T0, &u) == EFS_ERR_INVAL, "empty mask");
    u.mask = EFS_META_SET_MTIME;
    u.mtime_gen = 0;
    CHECK(efs_meta_apply_utimens(kv, ino, T0, &u) == EFS_ERR_INVAL, "gen 0");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_truncate_range_del(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_row r;
    struct efs_meta_chunk got, ch;
    struct efs_meta_truncate t;
    struct efs_meta_pub tail, p;
    struct coord_ctx cc;
    struct efs_meta_stat st;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "t", &ino)
              == EFS_OK,
          "create");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xA1, 0, T0 + 1, "pub0");
    pub(kv, ino, 64, 2ULL * EFS_MIN_CHUNK_SIZE, 0xA2, 0, T0 + 2, "pub64");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "chunk0");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_OK, "chunk64");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 2, "two chunks present");

    memset(&t, 0, sizeof(t));
    t.size = 0;
    t.lane_mask = ~0ULL; /* single-group test KV holds every lane */
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 10, &t) == EFS_OK, "to zero");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.base_size == 0 &&
              r.content_epoch == 1,
          "epoch+size");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_ERR_NOT_FOUND,
          "c0 deleted");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_ERR_NOT_FOUND,
          "c64 deleted");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 0, "truncate to zero subtracts every chunk");

    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.new_size = 64;
    p.expected_gen = 0;
    p.candidate_gen = 0xB1;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.content_epoch = 0;
    p.now = T0 + 11;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "old epoch blocked");
    p.content_epoch = 1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "new epoch ok");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 1, "post-fence publish counts again");

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "u", &ino)
              == EFS_OK,
          "create2");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xC1, 0, T0 + 1, "pub0b");
    pub(kv, ino, 64, 2ULL * EFS_MIN_CHUNK_SIZE, 0xC2, 0, T0 + 2, "pub64b");
    memset(&t, 0, sizeof(t));
    t.size = EFS_MIN_CHUNK_SIZE;
    t.lane_mask = ~0ULL;
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 12, &t) == EFS_OK, "aligned");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "prefix kept");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_ERR_NOT_FOUND,
          "suffix deleted");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 1, "aligned truncate subtracts the suffix");

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "v", &ino)
              == EFS_OK,
          "create3");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xD1, 0, T0 + 1, "pub0c");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row");
    fill_ch(&ch);
    memset(&tail, 0, sizeof(tail));
    tail.ino = ino;
    tail.chunk_index = 0;
    tail.new_size = 4096;
    tail.expected_gen = 0xD1;
    tail.candidate_gen = 0xD2;
    tail.coding_profile_id = EFS_META_PROFILE_K2F1;
    tail.content_epoch = r.content_epoch;
    tail.now = T0 + 13;
    tail.ch = ch;
    memset(&t, 0, sizeof(t));
    t.size = 4096;
    t.tail = &tail;
    t.lane_mask = ~0ULL;
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 13, &t) == EFS_OK, "partial");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xD2,
          "tail cas");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.base_size == 4096,
          "partial size");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 1, "tail CAS keeps the present count");

    /* Multi-lane range delete: ci 0/64 are lane 0, ci 1/65 are lane 1, so a
     * truncate to 2 chunks must delete one key from EACH lane. Regression:
     * the delete scratch array was indexed from a per-lane base, so the
     * second lane overwrote the first lane's keys and the wrong chunks were
     * deleted (a same-lane pair like 0/64 cannot see it). */
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "m", &ino)
              == EFS_OK,
          "create4");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xE1, 0, T0 + 1, "pub0m");
    pub(kv, ino, 1, 2ULL * EFS_MIN_CHUNK_SIZE, 0xE2, 0, T0 + 2, "pub1m");
    pub(kv, ino, 64, 65ULL * EFS_MIN_CHUNK_SIZE, 0xE3, 0, T0 + 3, "pub64m");
    pub(kv, ino, 65, 66ULL * EFS_MIN_CHUNK_SIZE, 0xE4, 0, T0 + 4, "pub65m");
    memset(&t, 0, sizeof(t));
    t.size = 2ULL * EFS_MIN_CHUNK_SIZE;
    t.lane_mask = ~0ULL;
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 14, &t) == EFS_OK, "multi-lane");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "m c0 kept");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 1, &got) == EFS_OK, "m c1 kept");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_ERR_NOT_FOUND,
          "m c64 deleted");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 65, &got) == EFS_ERR_NOT_FOUND,
          "m c65 deleted");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 2, "multi-lane truncate subtracts per lane");

    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void mkop(struct efs_opid *op, uint8_t id, uint64_t seq)
{
    memset(op, 0, sizeof(*op));
    op->client_uuid[15] = id;
    op->session_epoch = 1;
    op->seq = seq;
}

static void test_append_reserve(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_opid op;
    struct efs_meta_stat st;
    struct efs_meta_pub p;
    struct efs_meta_chunk ch;
    struct efs_txn_reduce red;
    efs_ino_t ino = 0;
    uint64_t off = 0, off2 = 0;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "a",
                                     &ino) == EFS_OK,
          "create");

    mkop(&op, 7, 1);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 64, &op, coord_fn, &cc, &off) ==
                  EFS_OK &&
              off == 0,
          "empty eof");
    CHECK(efs_meta_apply_append_reserve(kv, ino, 64, &op, coord_fn, &cc, &off2) ==
                  EFS_OK &&
              off2 == 0,
          "i16 replay");
    pub(kv, ino, 0, 64, 0xA1, 0, T0 + 1, "within bar");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 0,
          "frontier hides pub");
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.chunk_index = 0;
    p.new_size = 128;
    p.expected_gen = 0xA1;
    p.candidate_gen = 0xA2;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.now = T0 + 2;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_BUSY, "past watermark");
    CHECK(efs_meta_apply_append_resolve(kv, ino, off, EFS_META_APPEND_COMPLETED) ==
              EFS_OK,
          "complete");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 64,
          "visible after resolve");
    CHECK(efs_meta_apply_append_resolve(kv, ino, off, EFS_META_APPEND_COMPLETED) ==
              EFS_OK,
          "resolve replay");

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "b",
                                     &ino) == EFS_OK,
          "create b");
    pub(kv, ino, 0, 64, 0xB1, 0, T0 + 1, "pwrite");
    mkop(&op, 7, 2);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 64, &op, coord_fn, &cc, &off) ==
                  EFS_OK &&
              off == 64,
          "pwrite then reserve");
    mkop(&op, 7, 3);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 32, &op, coord_fn, &cc, &off2) ==
                  EFS_OK &&
              off2 == 128,
          "serial offsets");
    pub(kv, ino, 0, 96, 0xB2, 0xB1, T0 + 2, "within two");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 64,
          "cap at frontier");
    CHECK(efs_meta_apply_append_resolve(kv, ino, off, EFS_META_APPEND_COMPLETED) ==
              EFS_OK,
          "A done");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 128,
          "B still hidden");
    CHECK(efs_meta_apply_append_resolve(kv, ino, off2,
                                       EFS_META_APPEND_COMPLETED) == EFS_OK,
          "B done");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 160,
          "burst drained");

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "h",
                                     &ino) == EFS_OK,
          "create h");
    mkop(&op, 7, 4);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 64, &op, coord_fn, &cc, &off) ==
              EFS_OK,
          "hole rsv");
    CHECK(efs_meta_apply_append_resolve(kv, ino, off,
                                       EFS_META_APPEND_ABORTED_HOLE) == EFS_OK,
          "aborted");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 64,
          "hole in size");

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "z",
                                     &ino) == EFS_OK,
          "create z");
    mkop(&op, 9, 5);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 64, &op, coord_fn, &cc, &off) ==
              EFS_OK,
          "fenced rsv");
    mkop(&op, 8, 1);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 32, &op, coord_fn, &cc, &off2) ==
              EFS_ERR_BUSY,
          "other session rsv blocked");
    {
        uint8_t u9[EFS_OPID_UUID_LEN];
        uint32_t nopen = 99;

        memset(u9, 0, sizeof(u9));
        u9[15] = 9;
        CHECK(efs_meta_apply_append_drop_session(kv, efs_kv_inode_shard(ino), u9,
                                                 1) == EFS_OK,
              "fenced drop");
        CHECK(efs_meta_apply_append_state(kv, ino, NULL, NULL, &nopen) == EFS_OK &&
                  nopen == 0,
              "dropped session cleared nopen");
        CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
                  st.size == 64,
              "fenced hole in size");
        CHECK(efs_meta_apply_append_drop_session(kv, efs_kv_inode_shard(ino), u9,
                                                 1) == EFS_OK,
              "drop replay");
        CHECK(efs_meta_apply_append_reserve(kv, ino, 32, &op, coord_fn, &cc,
                                            &off2) == EFS_OK &&
                  off2 == 64,
              "other session after drop");
    }

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "r",
                                     &ino) == EFS_OK,
          "create r");
    pub(kv, ino, 0, 64, 0xC1, 0, T0 + 1, "lane for reduce");
    memset(&red, 0, sizeof(red));
    red.max_end = 500;
    red.max_mtime = T0 + 9;
    red.max_ctime = T0 + 9;
    commit_reduce(kv, 3, ino, 1, 0, &red, 1);
    mkop(&op, 7, 5);
    CHECK(efs_meta_apply_append_reserve(kv, ino, 16, &op, coord_fn, &cc, &off) ==
                  EFS_OK &&
              off == 500,
          "pending reduction is eof");
    {
        efs_ino_t zino = 0;
        struct efs_opid zop;
        uint64_t zoff = 0, wm = 0, fr = 0;
        uint32_t nopen = 0;

        CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                         "s", &zino) == EFS_OK,
              "create stand-in");
        memset(&zop, 0, sizeof(zop));
        CHECK(efs_meta_apply_append_reserve(kv, zino, 64, &zop, coord_fn, &cc,
                                            &zoff) == EFS_OK &&
                  zoff == 0,
              "zero uuid reserve");
        CHECK(efs_meta_apply_append_state(kv, zino, &wm, &fr, &nopen) == EFS_OK &&
                  wm == 64 && nopen == 1,
              "zero uuid watermark");
    }
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_link_nlink(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t a = 0;
    struct efs_meta_dentry dent;
    struct efs_meta_row r;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "a",
                                     &a) == EFS_OK &&
              a,
          "create");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, "a", EFS_ROOT_INO, "b", T0 + 1) ==
              EFS_OK,
          "link");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "b", &dent) == EFS_OK &&
              dent.ino == a,
          "alias");
    CHECK(efs_meta_apply_get_inode(kv, a, &r) == EFS_OK && r.nlink == 2,
          "nlink 2");
    CHECK(efs_meta_apply_link(kv, EFS_ROOT_INO, "a", EFS_ROOT_INO, "b", T0 + 1) ==
              EFS_ERR_EXIST,
          "dup dest");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "a", T0 + 2) == EFS_OK,
          "unlink src");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "a", &dent) == EFS_ERR_NOT_FOUND,
          "src gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "b", &dent) == EFS_OK &&
              dent.ino == a,
          "alias lives");
    CHECK(efs_meta_apply_get_inode(kv, a, &r) == EFS_OK && r.nlink == 1,
          "nlink 1");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "b", T0 + 3) == EFS_OK,
          "last link");
    CHECK(efs_meta_apply_get_inode(kv, a, &r) == EFS_ERR_NOT_FOUND, "inode gone");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_rmdir_rename(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t f = 0;
    struct efs_meta_dentry dent;
    struct efs_meta_row r;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "f",
                                     &f) == EFS_OK,
          "file");
    CHECK(efs_meta_apply_rmdir(kv, EFS_ROOT_INO, "f", T0) == EFS_ERR_INVAL,
          "rmdir file");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, "f", EFS_ROOT_INO, "g", T0 + 1) ==
              EFS_OK,
          "rename");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "f", &dent) == EFS_ERR_NOT_FOUND,
          "old gone");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "g", &dent) == EFS_OK &&
              dent.ino == f,
          "new name");
    CHECK(efs_meta_apply_get_inode(kv, f, &r) == EFS_OK && r.parent == EFS_ROOT_INO,
          "parent");
    CHECK(efs_meta_apply_rename(kv, EFS_ROOT_INO, "g", EFS_ROOT_INO, "g", T0 + 1) ==
              EFS_OK,
          "self");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_export_salt(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_kv *kv2 = efs_kv_mem_create();
    uint64_t salt = 1;
    const uint64_t golden = 0x9e3779b97f4a7c15ULL;

    CHECK(kv && kv2, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_export_salt(kv, &salt) == EFS_OK && salt == 0,
          "init salt 0");
    CHECK(efs_meta_apply_mkfs(kv2, T0, golden) == EFS_OK, "mkfs salt");
    CHECK(efs_meta_apply_export_salt(kv2, &salt) == EFS_OK && salt == golden,
          "stored");
    CHECK(efs_meta_apply_mkfs(kv2, T0, golden ^ 1) == EFS_OK, "idempotent");
    CHECK(efs_meta_apply_export_salt(kv2, &salt) == EFS_OK && salt == golden,
          "salt not overwritten");
    efs_kv_mem_free(kv);
    efs_kv_mem_free(kv2);
}

/* The even-group salt record (EFS_MD_CMD_SALT): a node hosting only the
 * even group never applies MKFS, so its salt must come from the anchor-2
 * record — and the salted MKDIR placement must match the group-0 node's. */
static void test_export_salt_anchor(void)
{
    struct efs_kv *g0 = efs_kv_mem_create();
    struct efs_kv *g2 = efs_kv_mem_create();
    uint64_t salt0 = 0, salt2 = 0;
    const uint64_t golden = 0x9e3779b97f4a7c15ULL;

    CHECK(g0 && g2, "kv");
    /* No record anywhere: NOT_FOUND, never a guessed 0. */
    CHECK(efs_meta_apply_export_salt(g2, &salt2) == EFS_ERR_NOT_FOUND,
          "absent is NOT_FOUND");
    CHECK(efs_meta_apply_mkfs(g0, T0, golden) == EFS_OK, "mkfs g0");
    CHECK(efs_meta_apply_salt_record(g2, efs_kv_anchor_shard(2), golden) ==
              EFS_OK,
          "salt record g2");
    CHECK(efs_meta_apply_export_salt(g0, &salt0) == EFS_OK && salt0 == golden,
          "g0 salt");
    CHECK(efs_meta_apply_export_salt(g2, &salt2) == EFS_OK && salt2 == golden,
          "g2 salt via anchor");
    /* Idempotent replay; mismatch is refused, never overwritten. */
    CHECK(efs_meta_apply_salt_record(g2, efs_kv_anchor_shard(2), golden) ==
              EFS_OK,
          "replay ok");
    CHECK(efs_meta_apply_salt_record(g2, efs_kv_anchor_shard(2), golden ^ 1) ==
              EFS_ERR_PROTO,
          "mismatch refused");
    CHECK(efs_meta_apply_export_salt(g2, &salt2) == EFS_OK && salt2 == golden,
          "still golden");
    /* Placement parity across several names: the hash computed from g2's
     * anchor record matches g0's for every name. */
    {
        static const char *const names[] = { "d", "mdtest-easy", "x0",
                                             "a-much-longer-directory-name" };
        size_t i;

        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            CHECK(efs_kv_mkdir_shard(EFS_ROOT_INO, names[i], salt2) ==
                      efs_kv_mkdir_shard(EFS_ROOT_INO, names[i], salt0),
                  "placement parity");
    }
    efs_kv_mem_free(g0);
    efs_kv_mem_free(g2);
}

/* SYMLINK is CREATE with S_IFLNK; the target is published bytes, not a
 * column on the inode row. Directories still cannot take a chunk map. */
static void test_symlink(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_ctx cc;
    struct efs_meta_dentry d;
    struct efs_meta_stat st;
    struct efs_meta_pub p;
    struct efs_meta_chunk ch;
    efs_ino_t ino = 0;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFLNK | 0777,
                                     "s", &ino) == EFS_OK && ino,
          "create");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "s", &d) == EFS_OK &&
              d.ino == ino && (d.type & S_IFMT) == S_IFLNK,
          "dent");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 0 && S_ISLNK(st.mode),
          "empty");
    pub(kv, ino, 0, 11, 0x51, 0, T0 + 1, "target");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.size == 11 && S_ISLNK(st.mode),
          "size");
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = EFS_ROOT_INO;
    p.chunk_index = 0;
    p.new_size = 11;
    p.candidate_gen = 0x51;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_INVAL, "dir");
    efs_kv_mem_free(kv);
}

/* Cross-group lane machinery (a group's log only writes its own shards'
 * keys): ACTIVATE_LANE on the inode group, LANE_FENCE on the lane's group,
 * and lane-local publishes that never touch the inode row. One KV here —
 * the group split is the host's routing concern; what is tested is that the
 * three applies compose and are idempotent. */
static void test_cross_group_lane(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_row r;
    struct efs_meta_chunk got, ch;
    struct efs_meta_pub p;
    struct coord_ctx cc;
    struct efs_meta_stat st;
    uint8_t lane;
    uint32_t ci;

    CHECK(kv != NULL, "kv");
    memset(&cc, 0, sizeof(cc));
    cc.kv = kv;
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "x", &ino) == EFS_OK, "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row");
    CHECK(r.active_lanes == 0, "no lanes yet");

    /* ACTIVATE_LANE: sets the bit, idempotent, NOT_FOUND is a no-op. */
    lane = 1;
    ci = lane; /* first chunk on lane 1 */
    CHECK(efs_meta_apply_activate_lane(kv, ino, lane) == EFS_OK, "activate");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK &&
              (r.active_lanes & (1ULL << lane)) != 0,
          "bit set");
    CHECK(efs_meta_apply_activate_lane(kv, ino, lane) == EFS_OK, "replay");
    CHECK(efs_meta_apply_activate_lane(kv, 999999, lane) == EFS_OK,
          "deleted ino no-op");
    CHECK(efs_meta_apply_activate_lanes(kv, ino, (1ULL << 3) | (1ULL << 5))
              == EFS_OK,
          "mask");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK &&
              (r.active_lanes & ((1ULL << 1) | (1ULL << 3) | (1ULL << 5)))
                  == ((1ULL << 1) | (1ULL << 3) | (1ULL << 5)),
          "mask bits");

    /* Lane-local publish: no row read, no row touch — the bitmap bit came
     * from ACTIVATE_LANE, and base_size must not move for a lane that is
     * not the inode's own shard. */
    fill_ch(&ch);
    memset(&p, 0, sizeof(p));
    p.ino = ino;
    p.chunk_index = ci;
    p.new_size = 2ULL * EFS_MIN_CHUNK_SIZE;
    p.expected_gen = 0;
    p.candidate_gen = 0xF1;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.content_epoch = 0;
    p.now = T0 + 1;
    p.ch = ch;
    p.lane_local = 1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_INVAL, "gen 0 inval");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row2");
    p.inode_gen = r.generation;
    p.mtime_gen = r.mtime_gen;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "lane-local pub");
    CHECK(efs_meta_apply_get_chunk(kv, ino, ci, &got) == EFS_OK &&
              got.generation == 0xF1,
          "chunk visible");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 1, "lane-local publish counts");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK &&
              r.base_size == 0,
          "base_size untouched");
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "lane-local replay");

    /* LANE_FENCE (truncate to 0's lane-1 share): deletes the lane's chunks,
     * fences the epoch, clamps max_end; idempotent replay keeps a later
     * publish visible. */
    CHECK(efs_meta_apply_lane_fence(kv, ino, r.generation, lane, 1, 0, 0, 0)
              == EFS_OK,
          "lane fence");
    CHECK(efs_meta_apply_get_chunk(kv, ino, ci, &got) == EFS_ERR_NOT_FOUND,
          "lane chunk deleted");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 0, "lane fence subtracts");
    p.candidate_gen = 0xF2;
    p.expected_gen = 0;
    p.content_epoch = 0; /* pre-fence epoch: must be rejected by the lane */
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE, "fence rejects");
    p.content_epoch = 1;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, "post-fence pub");
    CHECK(efs_meta_apply_lane_fence(kv, ino, r.generation, lane, 1, 0, 0, 0)
              == EFS_OK,
          "fence replay");
    CHECK(efs_meta_apply_get_chunk(kv, ino, ci, &got) == EFS_OK &&
              got.generation == 0xF2,
          "replay kept newer pub");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK &&
              st.alloc == 1, "fence replay does not subtract again");

    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* ---- data-plane GC (L7): records, acks, lane sweep, reap ---- */

static uint64_t gc_rd64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

struct gc_probe {
    int n;
    uint64_t gen[8];   /* chunk generation from the record key */
    uint8_t lane[8];
    uint32_t ci[8];
    uint8_t val[EFS_META_GC_VAL];
};

static int gc_probe_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct gc_probe *p = user;

    if (klen < 24 || vlen < EFS_META_GC_VAL || p->n >= 8)
        return 0;
    if (p->n == 0)
        memcpy(p->val, val, EFS_META_GC_VAL);
    p->gen[p->n] = gc_rd64(key + 11);
    p->lane[p->n] = key[19];
    p->ci[p->n] = ((uint32_t)key[20] << 24) | ((uint32_t)key[21] << 16) |
                  ((uint32_t)key[22] << 8) | (uint32_t)key[23];
    p->n++;
    return 0;
}

/* Every GC record lives on one of the two anchor shards (1 = odd group,
 * 2 = even group); the single test KV holds both. */
static int gc_probe(struct efs_kv *kv, struct gc_probe *p)
{
    uint8_t pre[EFS_KV_KEY_MAX];
    uint32_t plen = 0;

    memset(p, 0, sizeof(*p));
    if (efs_kv_key_gc_prefix(1, pre, &plen) != EFS_OK)
        return -1;
    if (efs_kv_scan_prefix(kv, pre, plen, gc_probe_cb, p) != EFS_OK)
        return -1;
    if (efs_kv_key_gc_prefix(2, pre, &plen) != EFS_OK)
        return -1;
    if (efs_kv_scan_prefix(kv, pre, plen, gc_probe_cb, p) != EFS_OK)
        return -1;
    return 0;
}

static int gc_probe_find(const struct gc_probe *p, uint64_t gen, uint8_t lane,
                         uint32_t ci)
{
    int i;

    for (i = 0; i < p->n; i++) {
        if (p->gen[i] == gen && p->lane[i] == lane && p->ci[i] == ci)
            return 1;
    }
    return 0;
}

static void gc_ack_all(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                       uint8_t lane, uint32_t ci, const char *msg)
{
    struct efs_gc_ack_item items[EFS_NUM_FRAGMENTS];
    int i;

    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        items[i].ino = ino;
        items[i].gen = gen;
        items[i].lane = lane;
        items[i].ci = ci;
        items[i].frag = (uint8_t)i;
    }
    CHECK(efs_meta_apply_gc_ack(kv, items, EFS_NUM_FRAGMENTS) == EFS_OK, msg);
}

/* Raw chunk-key presence probe: after an unlink the inode row is gone, so
 * efs_meta_apply_get_chunk (which resolves the row first) cannot speak for
 * the orphaned chunk keys the reaper inherits. */
static int chunk_present(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                         uint32_t ci)
{
    uint8_t key[EFS_KV_KEY_MAX], val[512];
    uint32_t kl = 0, vl = sizeof(val);
    uint8_t lane = (uint8_t)(ci % EFS_META_LANES);

    if (efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane, ci,
                         key, &kl) != EFS_OK)
        return -1;
    return efs_kv_get(kv, key, kl, val, &vl) == EFS_OK;
}

static void test_gc_reap(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0, ino2 = 0;
    struct efs_meta_row r;
    struct efs_meta_chunk got;
    struct efs_meta_truncate t;
    struct gc_probe pr;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint8_t ack_bits = 0xff;
    uint8_t k_reap[EFS_KV_KEY_MAX], v[EFS_META_REAP_VAL];
    uint32_t krl = 0, vl;
    uint64_t gen = 0, gen2 = 0;
    int i;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "g", &ino) == EFS_OK && ino,
          "create");
    /* lane = ci % 64: ci 0 -> lane 0, ci 1 -> lane 1. */
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xA1, 0, T0 + 1, "pub0");
    pub(kv, ino, 1, 2ULL * EFS_MIN_CHUNK_SIZE, 0xA2, 0, T0 + 2, "pub1");

    /* Publish CAS supersede: the old generation's fragment set must
     * survive the chunk key as a GC record. The overwrite carries NEW
     * content (different checksums) — an identical fragment set would be
     * an alias (the truncate tail stub), which must NOT emit a record. */
    {
        struct efs_meta_chunk newc;

        fill_ch(&newc);
        for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
            newc.checksums[i][0] = (uint8_t)(0x50 + i);
        pub_ch(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xB2, 0xA1, T0 + 3, &newc,
               "re-pub0");
    }
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 1, "one GC record");
    CHECK(gc_probe_find(&pr, 0xA1, 0, 0), "record is the superseded gen");
    CHECK(efs_meta_unpack_gc(pr.val, EFS_META_GC_VAL, nodes, &ack_bits,
                             sums) == EFS_OK,
          "unpack");
    CHECK(ack_bits == 0, "no acks yet");
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        CHECK(nodes[i] == (efs_node_id_t)(i + 1), "nodes carried");
        CHECK(sums[i][0] == (uint8_t)(0x10 + i), "sums carried");
    }
    /* A partial ack keeps the record; the last ack retires it. */
    {
        struct efs_gc_ack_item one;

        one.ino = ino;
        one.gen = 0xA1;
        one.lane = 0;
        one.ci = 0;
        one.frag = 0;
        CHECK(efs_meta_apply_gc_ack(kv, &one, 1) == EFS_OK, "ack 1");
        CHECK(gc_probe(kv, &pr) == 0 && pr.n == 1, "record survives 1 ack");
    }
    {
        struct efs_gc_ack_item two[2];

        for (i = 0; i < 2; i++) {
            two[i].ino = ino;
            two[i].gen = 0xA1;
            two[i].lane = 0;
            two[i].ci = 0;
            two[i].frag = (uint8_t)(i + 1);
        }
        CHECK(efs_meta_apply_gc_ack(kv, two, 2) == EFS_OK, "ack 2+3");
        CHECK(gc_probe(kv, &pr) == 0 && pr.n == 0, "record retired");
    }

    /* Unlink the last link: the row goes, the reap marker appears, and
     * the chunk keys are still there (the sweep has not run yet). */
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row pre-unlink");
    gen = r.generation;
    CHECK(r.active_lanes == 0x3, "lanes 0+1 active");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "g", T0 + 4) == EFS_OK,
          "unlink");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_ERR_NOT_FOUND,
          "row gone");
    CHECK(efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)), ino,
                          k_reap, &krl) == EFS_OK,
          "reap key");
    vl = sizeof(v);
    CHECK(efs_kv_get(kv, k_reap, krl, v, &vl) == EFS_OK, "marker present");
    {
        uint64_t mgen = 0, mlanes = 0;

        CHECK(efs_meta_unpack_reap(v, vl, &mgen, &mlanes) == EFS_OK,
              "marker unpack");
        CHECK(mgen == gen && mlanes == 0x3, "marker carries gen+lanes");
    }
    CHECK(chunk_present(kv, ino, gen, 0) == 1, "c0 pre-sweep");
    CHECK(chunk_present(kv, ino, gen, 1) == 1, "c1 pre-sweep");

    /* Lane sweep: the lane's chunk keys go, one GC record per chunk. */
    CHECK(efs_meta_apply_lane_sweep(kv, ino, gen, 0) == EFS_OK,
          "sweep lane0");
    CHECK(chunk_present(kv, ino, gen, 0) == 0, "c0 swept");
    CHECK(chunk_present(kv, ino, gen, 1) == 1, "c1 intact");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 1 &&
          gc_probe_find(&pr, 0xB2, 0, 0),
          "sweep emits GC (0xB2)");
    CHECK(efs_meta_apply_lane_sweep(kv, ino, gen, 1) == EFS_OK,
          "sweep lane1");
    CHECK(chunk_present(kv, ino, gen, 1) == 0, "c1 swept");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 2 &&
          gc_probe_find(&pr, 0xA2, 1, 1),
          "sweep emits GC (0xA2)");
    /* Idempotent: a replayed sweep finds an empty lane, adds nothing. */
    CHECK(efs_meta_apply_lane_sweep(kv, ino, gen, 0) == EFS_OK,
          "sweep replay");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 2, "no new records");

    /* REAP_DONE: the marker goes; a replay is a no-op. */
    CHECK(efs_meta_apply_reap_done(kv, ino, gen) == EFS_OK, "reap done");
    vl = sizeof(v);
    CHECK(efs_kv_get(kv, k_reap, krl, v, &vl) == EFS_ERR_NOT_FOUND,
          "marker gone");
    CHECK(efs_meta_apply_reap_done(kv, ino, gen) == EFS_OK, "reap replay");

    /* Acks drain the swept records. */
    gc_ack_all(kv, ino, 0xB2, 0, 0, "ack 0xB2");
    gc_ack_all(kv, ino, 0xA2, 1, 1, "ack 0xA2");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 0, "all records retired");

    /* Truncate-to-zero emits one GC record per deleted chunk. */
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "t2", &ino2) == EFS_OK && ino2,
          "create2");
    pub(kv, ino2, 0, EFS_MIN_CHUNK_SIZE, 0xC1, 0, T0 + 5, "t2 pub0");
    pub(kv, ino2, 1, 2ULL * EFS_MIN_CHUNK_SIZE, 0xC2, 0, T0 + 6, "t2 pub1");
    CHECK(efs_meta_apply_get_inode(kv, ino2, &r) == EFS_OK, "row2");
    gen2 = r.generation;
    memset(&t, 0, sizeof(t));
    t.size = 0;
    t.lane_mask = ~0ULL; /* single-group test KV holds every lane */
    CHECK(efs_meta_apply_truncate(kv, ino2, T0 + 10, &t) == EFS_OK, "trunc0");
    CHECK(efs_meta_apply_get_chunk(kv, ino2, 0, &got) == EFS_ERR_NOT_FOUND,
          "t2 c0 gone");
    CHECK(efs_meta_apply_get_chunk(kv, ino2, 1, &got) == EFS_ERR_NOT_FOUND,
          "t2 c1 gone");
    CHECK(chunk_present(kv, ino2, gen2, 0) == 0, "t2 c0 key gone");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 2 &&
          gc_probe_find(&pr, 0xC1, 0, 0) && gc_probe_find(&pr, 0xC2, 1, 1),
          "truncate emits GC per chunk");

    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* The raft truncate tail stub writes no fragments: it aliases the
 * superseded row's placement so reads keep finding the surviving prefix.
 * The apply side must NOT queue a GC record for an aliased supersede (the
 * reaper's checksum-conditional delete would match and remove the live
 * tail data), and an unlink of the file must still reclaim the fragments
 * through the live row's real checksums. */
static void test_gc_tail_alias(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0, ino2 = 0;
    struct efs_meta_row r;
    struct efs_meta_chunk got;
    struct efs_meta_truncate t;
    struct efs_meta_pub tail;
    struct gc_probe pr;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    uint8_t ack_bits = 0;
    uint64_t gen = 0;
    int i;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "ta", &ino) == EFS_OK && ino,
          "create");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xD1, 0, T0 + 1, "pub0");

    /* Shrink into the chunk: the tail stub aliases the 0xD1 fragment set
     * under a fresh generation. No GC record — the fragments are shared
     * with the live row, not dead. */
    memset(&tail, 0, sizeof(tail));
    fill_ch(&tail.ch); /* identical to the 0xD1 row: the alias */
    tail.chunk_index = 0;
    tail.new_size = 4000;
    tail.expected_gen = 0xD1;
    tail.candidate_gen = 0xD2;
    tail.coding_profile_id = EFS_META_PROFILE_K2F1;
    memset(&t, 0, sizeof(t));
    t.size = 4000;
    t.tail = &tail;
    t.lane_mask = ~0ULL; /* single-group test KV holds every lane */
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 2, &t) == EFS_OK,
          "truncate alias tail");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 0, "alias emits no GC record");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK,
          "tail row live");
    CHECK(got.generation == 0xD2, "tail carries the new gen");
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        CHECK(got.nodes[i] == (efs_node_id_t)(i + 1), "tail nodes aliased");
        CHECK(got.checksums[i][0] == (uint8_t)(0x10 + i),
              "tail sums aliased");
    }

    /* Unlink still reclaims: the sweep records the LIVE aliased gen with
     * the real checksums, so the reaper's conditional delete matches. */
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row pre-unlink");
    gen = r.generation;
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "ta", T0 + 3) == EFS_OK,
          "unlink");
    CHECK(efs_meta_apply_lane_sweep(kv, ino, gen, 0) == EFS_OK, "sweep");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 1 &&
          gc_probe_find(&pr, 0xD2, 0, 0),
          "sweep records the aliased gen");
    CHECK(efs_meta_unpack_gc(pr.val, EFS_META_GC_VAL, nodes, &ack_bits,
                             sums) == EFS_OK,
          "unpack");
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        CHECK(sums[i][0] == (uint8_t)(0x10 + i), "real sums swept");
    gc_ack_all(kv, ino, 0xD2, 0, 0, "ack 0xD2");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 0, "retired");

    /* Control: a tail carrying NEW content (different checksums — a real
     * zero-fill write) supersedes for real and must emit a record. */
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "tb", &ino2) == EFS_OK && ino2,
          "create2");
    pub(kv, ino2, 0, EFS_MIN_CHUNK_SIZE, 0xE1, 0, T0 + 4, "pub0");
    memset(&tail, 0, sizeof(tail));
    fill_ch(&tail.ch);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        tail.ch.checksums[i][0] = (uint8_t)(0x70 + i); /* new content */
    tail.chunk_index = 0;
    tail.new_size = 4000;
    tail.expected_gen = 0xE1;
    tail.candidate_gen = 0xE2;
    tail.coding_profile_id = EFS_META_PROFILE_K2F1;
    memset(&t, 0, sizeof(t));
    t.size = 4000;
    t.tail = &tail;
    t.lane_mask = ~0ULL;
    CHECK(efs_meta_apply_truncate(kv, ino2, T0 + 5, &t) == EFS_OK,
          "truncate new-content tail");
    CHECK(gc_probe(kv, &pr) == 0 && pr.n == 1 &&
          gc_probe_find(&pr, 0xE1, 0, 0),
          "new-content tail emits GC for the superseded gen");

    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

/* D26: the pending-GC watermark tracks records through insert (publish
 * supersede, lane sweep) and removal (GC_ACK retire), folds inserts that
 * land while unknown into the derive (upward only, never undercounts),
 * and clamps back to zero via zero_if only when nothing raced. */
static int wm_count_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    uint64_t *n = user;

    (void)key;
    (void)val;
    if (klen == 24 && vlen >= EFS_META_GC_VAL)
        (*n)++;
    return 0;
}

static uint64_t wm_count_anchor(struct efs_kv *kv, uint32_t anch)
{
    uint8_t pre[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    uint64_t n = 0;

    if (efs_kv_key_gc_prefix(anch, pre, &plen) != EFS_OK)
        return UINT64_MAX;
    if (efs_kv_scan_prefix(kv, pre, plen, wm_count_cb, &n) != EFS_OK)
        return UINT64_MAX;
    return n;
}

static void test_gc_watermark(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_row r;
    struct efs_gc_ack_item ack;
    efs_ino_t ino = 0;
    uint64_t gen = 0;
    uint32_t anch;
    int64_t p0;
    int i;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "wm", &ino) == EFS_OK && ino, "create");
    anch = efs_kv_anchor_shard(efs_kv_lane_shard(ino, 0));

    /* Unknown (post-recovery/import) state: inserts accumulate out of
     * sight of a peek until the derive folds them in. */
    efs_meta_gc_pending_invalidate(anch);
    CHECK(efs_meta_gc_pending_peek(anch) < 0, "unknown after invalidate");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xF1, 0, T0 + 1, "pub0");
    CHECK(efs_meta_gc_pending_peek(anch) < 0, "first publish queues no GC");
    {
        struct efs_meta_chunk newc;

        fill_ch(&newc);
        for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
            newc.checksums[i][0] = (uint8_t)(0x60 + i);
        pub_ch(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xF2, 0xF1, T0 + 2, &newc,
               "re-pub0");
    }
    CHECK(efs_meta_gc_pending_peek(anch) < 0, "bump while unknown stays hidden");
    CHECK(wm_count_anchor(kv, anch) == 1, "scan sees the superseded record");
    efs_meta_gc_pending_derived(anch, 1);
    p0 = efs_meta_gc_pending_peek(anch);
    /* The scan counted the record and the fold adds the queued bump:
     * upward only, never undercounts. */
    CHECK(p0 == 2, "insert while unknown folds into the derive");

    /* A partial ack keeps the record and the watermark; retiring lowers.
     * A replayed ack is a no-op for the count. */
    memset(&ack, 0, sizeof(ack));
    ack.ino = ino;
    ack.gen = 0xF1;
    ack.lane = 0;
    ack.ci = 0;
    ack.frag = 0;
    CHECK(efs_meta_apply_gc_ack(kv, &ack, 1) == EFS_OK, "ack 1");
    CHECK(efs_meta_gc_pending_peek(anch) == p0, "partial ack keeps count");
    ack.frag = 1;
    CHECK(efs_meta_apply_gc_ack(kv, &ack, 1) == EFS_OK, "ack 2");
    ack.frag = 2;
    CHECK(efs_meta_apply_gc_ack(kv, &ack, 1) == EFS_OK, "ack 3");
    CHECK(wm_count_anchor(kv, anch) == 0, "record retired");
    CHECK(efs_meta_gc_pending_peek(anch) == p0 - 1, "retire lowers");
    CHECK(efs_meta_apply_gc_ack(kv, &ack, 1) == EFS_OK, "ack replay");
    CHECK(efs_meta_gc_pending_peek(anch) == p0 - 1, "replay keeps count");

    /* Lane sweep bumps per swept record; its acks lower. */
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK, "row pre-unlink");
    gen = r.generation;
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "wm", T0 + 3) == EFS_OK,
          "unlink");
    CHECK(efs_meta_apply_lane_sweep(kv, ino, gen, 0) == EFS_OK, "sweep");
    CHECK(efs_meta_gc_pending_peek(anch) == p0, "sweep bumps one");
    gc_ack_all(kv, ino, 0xF2, 0, 0, "ack swept");
    CHECK(efs_meta_gc_pending_peek(anch) == p0 - 1, "ack lowers again");
    CHECK(wm_count_anchor(kv, anch) == 0, "drained");

    /* Upward drift clamps only when a full pass found nothing and no
     * note raced it (a stale expect fails). */
    efs_meta_gc_pending_note(anch, 1); /* counted, nothing live */
    CHECK(efs_meta_gc_pending_zero_if(anch, p0 - 1) == 0,
          "stale expect fails");
    CHECK(efs_meta_gc_pending_zero_if(anch, p0) == 1, "zeroed");
    CHECK(efs_meta_gc_pending_peek(anch) == 0, "empty pass is a peek");

    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

static void test_xattr(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_meta_row row;
    efs_ino_t a = 0;
    uint8_t got[8], list[64], key[EFS_KV_KEY_MAX];
    uint32_t n, kl = 0;
    const uint8_t *efs = (const uint8_t *)"user.efs";
    const uint8_t *other = (const uint8_t *)"user.other";

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_xattr(kv, 999999, T0, EFS_XATTR_SET, 0, efs, 8,
                               (const uint8_t *)"1", 1) == EFS_ERR_NOT_FOUND,
          "missing inode");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "xa", &a) == EFS_OK && a,
          "create");
    CHECK(efs_meta_apply_xattr(kv, a, T0 + 5, EFS_XATTR_SET, 0, efs, 8,
                               (const uint8_t *)"1", 1) == EFS_OK,
          "set");
    n = sizeof(got);
    CHECK(efs_meta_xattr_get(kv, a, efs, 8, got, &n) == EFS_OK && n == 1 &&
              got[0] == '1',
          "get");
    CHECK(efs_meta_apply_xattr(kv, a, T0 + 6, EFS_XATTR_SET, EFS_XATTR_CREATE,
                               efs, 8, (const uint8_t *)"2", 1) == EFS_ERR_EXIST,
          "create flag");
    CHECK(efs_meta_apply_xattr(kv, a, T0 + 6, EFS_XATTR_SET, EFS_XATTR_REPLACE,
                               other, 10, (const uint8_t *)"z", 1) ==
              EFS_ERR_NODATA,
          "replace missing");
    CHECK(efs_meta_apply_xattr(kv, a, T0 + 6, EFS_XATTR_SET, 0, other, 10,
                               (const uint8_t *)"ab", 2) == EFS_OK,
          "second");
    n = sizeof(got);
    CHECK(efs_meta_xattr_get(kv, a, efs, 8, got, &n) == EFS_OK && n == 1 &&
              got[0] == '1',
          "first kept");
    n = sizeof(list);
    CHECK(efs_meta_xattr_list(kv, a, list, &n) == EFS_OK && n > 0, "list");
    CHECK(efs_meta_apply_xattr(kv, a, T0 + 7, EFS_XATTR_REMOVE, 0, efs, 8,
                               NULL, 0) == EFS_OK,
          "remove");
    n = sizeof(got);
    CHECK(efs_meta_xattr_get(kv, a, efs, 8, got, &n) == EFS_ERR_NODATA, "gone");
    n = sizeof(got);
    CHECK(efs_meta_xattr_get(kv, a, other, 10, got, &n) == EFS_OK && n == 2 &&
              got[0] == 'a' && got[1] == 'b',
          "other kept");
    CHECK(efs_meta_apply_xattr(kv, a, T0 + 8, EFS_XATTR_REMOVE, 0,
                               (const uint8_t *)"user.nope", 9, NULL, 0) ==
              EFS_ERR_NODATA,
          "remove missing");
    CHECK(efs_meta_apply_get_inode(kv, a, &row) == EFS_OK &&
              row.base_ctime >= T0 + 5,
          "ctime");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "xa", T0 + 9) == EFS_OK,
          "unlink");
    CHECK(efs_kv_key_xattr(efs_kv_inode_shard(a), a, key, &kl) == EFS_OK, "key");
    n = sizeof(got);
    CHECK(efs_kv_get(kv, key, kl, got, &n) == EFS_ERR_NOT_FOUND, "key deleted");
    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
    efs_kv_mem_free(kv);
}

int main(void)
{
    test_create_lookup_unlink();
    test_xattr();
    test_mkdir();
    test_create_remote_parent();
    test_create_log_at_matches();
    test_alloc_skips_live();
    test_mkdir_log_at_matches();
    test_create_log_at_dup_hint();
    test_alloc_vs_txn_intent();
    test_log_delete_busy_under_intent();
    test_i9();
    test_batch_fail();
    test_i16_durable();
    test_publish();
    test_cas_i20_i21();
    test_publish_stale_then_retry();
    test_chunk_deltas();
    test_epoch_i22();
    test_evidence();
    test_i8_spread();
    test_row_attrs();
    test_readdir_local();
    test_readdir_spread();
    test_readdir_lane0();
    test_setattr_mode_owner();
    test_stat_collect();
    test_stat_committed_unresolved_row();
    test_lookup_committed_unresolved_dentry();
    test_stat_fence_and_gen();
    test_stat_dir_hashed();
    test_utimens_fence();
    test_truncate_range_del();
    test_cross_group_lane();
    test_append_reserve();
    test_link_nlink();
    test_rmdir_rename();
    test_export_salt();
    test_export_salt_anchor();
    test_symlink();
    test_dir_rename();
    test_rename_cross_dir();
    test_rename_hashed();
    test_unlink_hashed();
    test_rmdir_hashed();
    test_link_hashed();
    test_link_splitting();
    test_rename_splitting();
    test_lookup_path();
    test_gc_reap();
    test_gc_tail_alias();
    test_gc_tail_alias();
    test_gc_watermark();
    if (failures) {
        fprintf(stderr, "test_meta_apply: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_meta_apply: OK\n");
    return 0;
}
