/* Isolated POSIX lock SM. No sockets, no Raft. */
#include "efs/lock.h"
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

static void fill_req(struct efs_lock_req *r, efs_ino_t ino, uint64_t gen,
                     uint8_t domain, uint8_t type, uint64_t start, uint64_t end,
                     uint8_t kind, uint64_t id, uint8_t un)
{
    memset(r, 0, sizeof(*r));
    r->ino = ino;
    r->generation = gen;
    r->domain = domain;
    r->type = type;
    r->start = start;
    r->end = end;
    uuid_of(r->owner.uuid, un);
    r->owner.epoch = 1;
    r->owner.kind = kind;
    r->owner.id = id;
}

static void test_conflict_and_domains(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_row row;
    struct efs_lock_req a, b;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "f", &ino)
              == EFS_OK,
          "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK, "row");
    fill_req(&a, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
             EFS_LOCK_PROC, 1, 1);
    fill_req(&b, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX, 5, 15,
             EFS_LOCK_PROC, 2, 2);
    CHECK(efs_lock_grant(kv, &a) == EFS_OK, "A ex");
    CHECK(efs_lock_grant(kv, &b) == EFS_ERR_AGAIN, "B conflict");
    b.type = EFS_LOCK_SH;
    CHECK(efs_lock_grant(kv, &b) == EFS_ERR_AGAIN, "shared vs ex");
    fill_req(&b, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_SH, 20, 30,
             EFS_LOCK_PROC, 2, 2);
    CHECK(efs_lock_grant(kv, &b) == EFS_OK, "nonoverlap shared");
    fill_req(&b, ino, row.generation, EFS_LOCK_FLOCK, EFS_LOCK_EX, 0,
             ~(uint64_t)0, EFS_LOCK_OFD, 9, 2);
    CHECK(efs_lock_grant(kv, &b) == EFS_OK, "flock independent");
    fill_req(&b, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
             EFS_LOCK_OFD, 9, 2);
    CHECK(efs_lock_grant(kv, &b) == EFS_ERR_AGAIN, "OFD vs classic");
    a.generation = row.generation + 1;
    CHECK(efs_lock_grant(kv, &a) == EFS_ERR_STALE, "stale gen");
    efs_kv_mem_free(kv);
}

static void test_cap_and_drop(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    efs_ino_t ino = 0;
    struct efs_meta_row row;
    struct efs_lock_req r;
    int i, rc;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "g", &ino)
              == EFS_OK,
          "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK, "row");
    for (i = 0; i < EFS_LOCK_CAP; i++) {
        fill_req(&r, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX,
                 (uint64_t)i * 2, (uint64_t)i * 2 + 1, EFS_LOCK_PROC, 1, 1);
        rc = efs_lock_grant(kv, &r);
        CHECK(rc == EFS_OK, "cap fill");
    }
    fill_req(&r, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX,
             EFS_LOCK_CAP * 2, EFS_LOCK_CAP * 2 + 1, EFS_LOCK_PROC, 1, 1);
    CHECK(efs_lock_grant(kv, &r) == EFS_ERR_NOLCK, "ENOLCK");
    CHECK(efs_lock_drop_session(kv, efs_kv_inode_shard(ino), r.owner.uuid, 1) ==
              EFS_OK,
          "drop");
    CHECK(efs_lock_count(kv, ino, row.generation) == 0, "gone");
    efs_kv_mem_free(kv);
}

int main(void)
{
    test_conflict_and_domains();
    test_cap_and_drop();
    if (failures) {
        fprintf(stderr, "test_lock: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_lock: OK\n");
    return 0;
}
