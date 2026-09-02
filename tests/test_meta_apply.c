/* Isolated applied-state SM over mem KV. No sockets, no cluster. */
#include "efs/meta_apply.h"
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

static void test_create_lookup_unlink(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t a = 0, b = 0;
    struct efs_meta_dentry d;
    struct efs_meta_row r;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &r) == EFS_OK, "root");
    CHECK(r.generation == 1 && S_ISDIR(r.mode), "root dir");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "a", &a)
              == EFS_OK && a,
          "create a");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "b", &b)
              == EFS_OK && b && b != a,
          "create b");
    CHECK((a & 0xFFF) == (EFS_ROOT_INO & 0xFFF), "co-located");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "a", &a)
              == EFS_ERR_EXIST,
          "dup");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "a", &d) == EFS_OK && d.ino == a,
          "lookup");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "a") == EFS_OK, "unlink");
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "x", &ino)
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_kv_mem_fail_next_batch(kv) == EFS_OK, "arm");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "y", &ino)
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    memset(&op, 0, sizeof(op));
    op.client_uuid[15] = 1;
    op.session_epoch = 1;
    op.seq = 1;
    CHECK(efs_meta_apply_create_file_op(kv, &op, EFS_ROOT_INO, S_IFREG | 0644,
                                        "once", &a) == EFS_OK && a,
          "first");
    CHECK(efs_meta_apply_create_file_op(kv, &op, EFS_ROOT_INO, S_IFREG | 0644,
                                        "once", &b) == EFS_OK && b == a,
          "replay");
    CHECK(efs_meta_apply_create_file_op(kv, &op, EFS_ROOT_INO, S_IFREG | 0644,
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "d", &ino)
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "d", &ino)
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "e", &ino)
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
    CHECK(efs_meta_apply_init(kv) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, EFS_ROOT_INO, S_IFREG | 0644, "f", &ino)
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
    if (failures) {
        fprintf(stderr, "test_meta_apply: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_meta_apply: OK\n");
    return 0;
}
