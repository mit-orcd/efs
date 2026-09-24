/* Isolated session + open-lease SM. No sockets, no Raft. */
#include "efs/session.h"
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
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

#define T0 1000000000000000000ull

static const struct efs_meta_attrs g_at = { 1000, 1000, T0 };

static void uuid_of(uint8_t u[EFS_OPID_UUID_LEN], uint8_t n)
{
    memset(u, 0, EFS_OPID_UUID_LEN);
    u[15] = n;
}

static void test_register_and_fence(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t u[EFS_OPID_UUID_LEN];
    struct efs_session_rec r;

    uuid_of(u, 1);
    CHECK(kv != NULL, "kv");
    CHECK(efs_session_create(kv, u, 1) == EFS_OK, "create");
    CHECK(efs_session_create(kv, u, 1) == EFS_OK, "create idemp");
    CHECK(efs_session_register(kv, u, 1, 1) == EFS_OK, "reg 1");
    CHECK(efs_session_register(kv, u, 1, 1) == EFS_OK, "reg 1 idemp");
    CHECK(efs_session_establish(kv, 1, u, 1) == EFS_OK, "est");
    CHECK(efs_session_accept(kv, 1, u, 1) == EFS_OK, "accept");
    CHECK(efs_session_accept(kv, 2, u, 1) == EFS_ERR_BUSY, "need register");
    CHECK(efs_session_begin_fence(kv, u) == EFS_OK, "begin");
    CHECK(efs_session_begin_fence(kv, u) == EFS_OK, "begin idemp");
    CHECK(efs_session_register(kv, u, 1, 2) == EFS_ERR_STALE, "no new shard");
    CHECK(efs_session_register(kv, u, 1, 1) == EFS_OK, "old bit idemp");
    CHECK(efs_session_register(kv, u, 2, 1) == EFS_ERR_BUSY, "new epoch waits");
    CHECK(efs_session_fence_local(kv, 1, u, 2) == EFS_OK, "fence local");
    CHECK(efs_session_accept(kv, 1, u, 1) == EFS_ERR_STALE, "old epoch");
    CHECK(efs_session_accept(kv, 1, u, 2) == EFS_ERR_BUSY, "new not established");
    CHECK(efs_session_finish_fence(kv, u) == EFS_ERR_BUSY, "no ack");
    CHECK(efs_session_ack_fence(kv, u, 1) == EFS_OK, "ack");
    CHECK(efs_session_ack_fence(kv, u, 1) == EFS_OK, "ack idemp");
    CHECK(efs_session_finish_fence(kv, u) == EFS_OK, "finish");
    CHECK(efs_session_get(kv, u, &r) == EFS_OK && r.state == EFS_SESSION_ACTIVE &&
              r.epoch == 2,
          "ACTIVE(2)");
    CHECK(efs_session_accept(kv, 1, u, 1) == EFS_ERR_STALE, "still stale");
    CHECK(efs_session_register(kv, u, 2, 1) == EFS_OK, "reg epoch 2");
    CHECK(efs_session_establish(kv, 1, u, 2) == EFS_OK, "est 2");
    CHECK(efs_session_accept(kv, 1, u, 2) == EFS_OK, "accept 2");
    CHECK(efs_session_barrier_done(kv, u, 1) == EFS_OK, "barrier");
    efs_kv_mem_free(kv);
}

static void test_i19_lease(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t u[EFS_OPID_UUID_LEN];
    efs_ino_t ino = 0;
    struct efs_meta_row row;
    struct efs_meta_dentry d;

    uuid_of(u, 2);
    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_session_create(kv, u, 1) == EFS_OK, "sess");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "f", &ino)
              == EFS_OK && ino,
          "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK, "row");
    CHECK(efs_lease_open(kv, ino, row.generation, u, 1) == EFS_OK, "open");
    CHECK(efs_lease_open(kv, ino, row.generation + 1, u, 1) == EFS_ERR_STALE,
          "bad gen");
    CHECK(efs_lease_any(kv, ino, row.generation) == 1, "held");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "f", T0) == EFS_OK, "unlink");
    CHECK(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "f", &d) == EFS_ERR_NOT_FOUND,
          "dentry gone");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK && row.nlink == 0,
          "orphan");
    CHECK(efs_meta_apply_reclaim(kv, ino) == EFS_ERR_BUSY, "lease holds");
    CHECK(efs_session_create(kv, u, 1) == EFS_OK, "sess still");
    CHECK(efs_session_begin_fence(kv, u) == EFS_OK, "begin");
    CHECK(efs_session_barrier_done(kv, u, 1) == EFS_ERR_BUSY, "before ACTIVE");
    CHECK(efs_session_finish_fence(kv, u) == EFS_OK, "empty touched finish");
    CHECK(efs_session_barrier_done(kv, u, 1) == EFS_OK, "barrier");
    CHECK(efs_lease_drop_session(kv, efs_kv_inode_shard(ino), u, 1) == EFS_OK,
          "drop");
    CHECK(efs_lease_any(kv, ino, row.generation) == 0, "empty");
    CHECK(efs_meta_apply_reclaim(kv, ino) == EFS_OK, "reclaim");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_ERR_NOT_FOUND, "gone");
    efs_kv_mem_free(kv);
}

static void test_unlink_no_lease(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_row row;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "g", &ino)
              == EFS_OK,
          "create");
    CHECK(efs_meta_apply_unlink(kv, EFS_ROOT_INO, "g", T0) == EFS_OK, "unlink");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_ERR_NOT_FOUND,
          "deleted");
    efs_kv_mem_free(kv);
}

/* FENCE_LOC drops that shard's op-id window for the epoch it fences
 * (fence_epoch - 1). A window on a shard that was not fenced stays. */
static void test_fence_drops_opid(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t u[EFS_OPID_UUID_LEN];
    uint8_t key[EFS_KV_KEY_MAX], other[EFS_KV_KEY_MAX], val[8];
    uint32_t klen = 0, olen = 0, vlen;
    uint8_t one = 1;

    uuid_of(u, 9);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_opid(7, u, 1, key, &klen) == EFS_OK, "key");
    CHECK(efs_kv_key_opid(8, u, 1, other, &olen) == EFS_OK, "other");
    CHECK(efs_kv_put(kv, key, klen, &one, 1) == EFS_OK, "put");
    CHECK(efs_kv_put(kv, other, olen, &one, 1) == EFS_OK, "put other");
    CHECK(efs_session_fence_local(kv, 7, u, 2) == EFS_OK, "fence 7");
    vlen = sizeof(val);
    CHECK(efs_kv_get(kv, key, klen, val, &vlen) == EFS_ERR_NOT_FOUND, "dropped");
    vlen = sizeof(val);
    CHECK(efs_kv_get(kv, other, olen, val, &vlen) == EFS_OK, "other shard kept");
    CHECK(efs_session_fence_local(kv, 7, u, 2) == EFS_OK, "fence idemp");
    efs_kv_mem_free(kv);
}

int main(void)
{
    test_register_and_fence();
    test_i19_lease();
    test_unlink_no_lease();
    test_fence_drops_opid();
    if (failures) {
        fprintf(stderr, "test_session: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_session: OK\n");
    return 0;
}
