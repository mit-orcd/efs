/* Isolated applied-state SM over mem KV. No sockets, no cluster. */
#include "efs/meta_apply.h"
#include "efs/dir_layout.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/opid.h"
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

static void pub(struct efs_kv *kv, efs_ino_t ino, uint32_t ci, uint64_t end,
                uint64_t gen, uint64_t expect, uint64_t now, const char *msg)
{
    struct efs_meta_pub p;
    struct efs_meta_chunk ch;
    struct efs_meta_row r;

    fill_ch(&ch);
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
    p.ch = ch;
    CHECK(efs_meta_apply_publish(kv, &p) == EFS_OK, msg);
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

    /* One lane. */
    pub(kv, ino, 0, 64, 0xA1, 0, T0 + 5, "publish lane 0");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.lanes == 1 && st.size == 64, "lane size");
    CHECK(st.mtime == T0 + 5 && st.ctime == T0 + 5, "write moved both times");
    CHECK(st.atime == T0, "noatime: reads do not move atime");

    /* A second lane, on a different shard, with a larger high-water mark.
     * This is the case a single row could not represent. */
    pub(kv, ino, 1, 200, 0xB2, 0, T0 + 7, "publish lane 1");
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.lanes == 2 && st.size == 200, "MAX over lanes");
    CHECK(st.mtime == T0 + 7, "newest write wins");

    /* Committed, not yet materialized: a write that has returned to its
     * caller. Ignoring it would report a size older than that write. */
    memset(&red, 0, sizeof(red));
    red.max_end = 500;
    red.max_mtime = T0 + 9;
    red.max_ctime = T0 + 9;
    commit_reduce(kv, 1, ino, 1, 1, &red, 1);
    CHECK(efs_meta_apply_getattr(kv, ino, coord_fn, &cc, &st) == EFS_OK, "stat");
    CHECK(st.size == 500 && st.mtime == T0 + 9, "committed reduction counted");

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

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "t", &ino)
              == EFS_OK,
          "create");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xA1, 0, T0 + 1, "pub0");
    pub(kv, ino, 64, 2ULL * EFS_MIN_CHUNK_SIZE, 0xA2, 0, T0 + 2, "pub64");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "chunk0");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_OK, "chunk64");

    memset(&t, 0, sizeof(t));
    t.size = 0;
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 10, &t) == EFS_OK, "to zero");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.base_size == 0 &&
              r.content_epoch == 1,
          "epoch+size");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_ERR_NOT_FOUND,
          "c0 deleted");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_ERR_NOT_FOUND,
          "c64 deleted");

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

    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "u", &ino)
              == EFS_OK,
          "create2");
    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xC1, 0, T0 + 1, "pub0b");
    pub(kv, ino, 64, 2ULL * EFS_MIN_CHUNK_SIZE, 0xC2, 0, T0 + 2, "pub64b");
    memset(&t, 0, sizeof(t));
    t.size = EFS_MIN_CHUNK_SIZE;
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 12, &t) == EFS_OK, "aligned");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK, "prefix kept");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 64, &got) == EFS_ERR_NOT_FOUND,
          "suffix deleted");

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
    CHECK(efs_meta_apply_truncate(kv, ino, T0 + 13, &t) == EFS_OK, "partial");
    CHECK(efs_meta_apply_get_chunk(kv, ino, 0, &got) == EFS_OK &&
              got.generation == 0xD2,
          "tail cas");
    CHECK(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK && r.base_size == 4096,
          "partial size");
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
                                         "z", &zino) == EFS_OK,
              "create z");
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

int main(void)
{
    test_create_lookup_unlink();
    test_i9();
    test_batch_fail();
    test_i16_durable();
    test_publish();
    test_cas_i20_i21();
    test_epoch_i22();
    test_evidence();
    test_i8_spread();
    test_row_attrs();
    test_readdir_local();
    test_readdir_spread();
    test_readdir_lane0();
    test_setattr_mode_owner();
    test_stat_collect();
    test_stat_fence_and_gen();
    test_stat_dir_hashed();
    test_utimens_fence();
    test_truncate_range_del();
    test_append_reserve();
    test_link_nlink();
    test_rmdir_rename();
    test_export_salt();
    test_lookup_path();
    if (failures) {
        fprintf(stderr, "test_meta_apply: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_meta_apply: OK\n");
    return 0;
}
