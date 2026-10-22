/* Isolated POSIX lock SM. No sockets, no Raft. */
#include "efs/lock.h"
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/common.h"
#include "efs/txn.h"
#include "efs/session.h"
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

/* K5: a lock owner's session must be established on the inode shard. */
static int establish(struct efs_kv *kv, efs_ino_t ino, uint8_t n)
{
    uint8_t u[EFS_OPID_UUID_LEN];
    uuid_of(u, n);
    return efs_session_establish(kv, efs_kv_inode_shard(ino), u, 1);
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
    struct efs_lock_req a, b, hit;

    CHECK(kv != NULL, "kv");
    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644, "f", &ino)
              == EFS_OK,
          "create");
    CHECK(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK, "row");
    CHECK(establish(kv, ino, 1) == EFS_OK && establish(kv, ino, 2) == EFS_OK, "owners");
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
    fill_req(&b, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX, 5, 15,
             EFS_LOCK_PROC, 2, 2);
    CHECK(efs_lock_getlk(kv, &b, &hit) == EFS_OK, "getlk");
    CHECK(hit.type == EFS_LOCK_EX, "getlk type");
    CHECK(hit.start == 0 && hit.end == 10, "getlk range");
    CHECK(hit.owner.id == 1, "getlk owner");
    fill_req(&b, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
             EFS_LOCK_PROC, 1, 1);
    CHECK(efs_lock_getlk(kv, &b, &hit) == EFS_OK, "getlk self");
    CHECK(hit.type == 0, "getlk self unlck");
    fill_req(&b, ino, row.generation, EFS_LOCK_FCNTL, EFS_LOCK_EX, 40, 50,
             EFS_LOCK_PROC, 2, 2);
    CHECK(efs_lock_getlk(kv, &b, &hit) == EFS_OK, "getlk miss");
    CHECK(hit.type == 0, "getlk miss unlck");
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
    CHECK(establish(kv, ino, 1) == EFS_OK, "owner");
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

static int close_fail;
static int (*close_batch)(void *,const struct efs_kv_item *,uint32_t);
static int close_batch_fault(void *ctx,const struct efs_kv_item *items,uint32_t count)
{
    return close_fail?EFS_ERR_IO:close_batch(ctx,items,count);
}
static void test_atomic_final_close(void)
{
    struct efs_kv *kv=efs_kv_mem_create();efs_ino_t ino=0;
    struct efs_meta_row row;struct efs_lock_req r;
    CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"atomic close init");
    CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"close",&ino)==EFS_OK,"atomic close inode");
    CHECK(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK,"atomic close row");
    CHECK(establish(kv,ino,1)==EFS_OK,"atomic close owner");
    for(unsigned domain=0;domain<2;domain++)for(unsigned i=0;i<EFS_LOCK_CAP;i++) {
        fill_req(&r,ino,row.generation,domain,EFS_LOCK_EX,2*i,2*i+1,EFS_LOCK_PROC,1,1);
        CHECK(efs_lock_grant(kv,&r)==EFS_OK,"fill both capped domains");
    }
    uint8_t key[EFS_KV_KEY_MAX],one=EFS_LOCK_EX;uint32_t len;
    CHECK(efs_kv_key_lock(efs_kv_inode_shard(ino),ino,row.generation,r.domain,r.start,r.end,
          r.owner.kind,r.owner.id,r.owner.uuid,r.owner.epoch,key,&len)==EFS_OK,"last lock key");
    struct efs_txid tx={{81}};
    struct efs_txn_parts parts={.n=1};parts.shard[0]=efs_kv_inode_shard(ino);
    CHECK(efs_txn_prepare_excl_value(kv,&tx,&parts,key,len,&one,1,EFS_TXN_PUT,&one,1)==EFS_OK,"prepare last lock");
    CHECK(efs_lock_drop_file(kv,ino,row.generation)==EFS_ERR_BUSY,"prepared last lock blocks whole close");
    CHECK(efs_lock_count(kv,ino,row.generation)==64,"BUSY preserves all locks");
    CHECK(efs_txn_resolve(kv,&tx,parts.shard[0],EFS_TXN_ABORT)==EFS_OK,"resolve last lock");
    const struct efs_kv_ops *saved=kv->ops;struct efs_kv_ops ops=*saved;
    close_batch=ops.batch;ops.batch=close_batch_fault;kv->ops=&ops;close_fail=1;
    CHECK(efs_lock_drop_file(kv,ino,row.generation)==EFS_ERR_IO,"atomic close IO failure");
    CHECK(efs_lock_count(kv,ino,row.generation)==64,"IO preserves all locks");
    close_fail=0;
    CHECK(efs_lock_drop_file(kv,ino,row.generation)==EFS_OK,"retry atomic final close");
    CHECK(efs_lock_count(kv,ino,row.generation)==0,"both domains empty");
    CHECK(efs_lock_drop_file(kv,ino,row.generation)==EFS_OK,"final close replay empty");
    kv->ops=saved;
    /* Corrupt/imported over-cap rows must not cause unbounded deletion or a
     * partial effect before the envelope violation is detected. */
    for(unsigned i=0;i<=EFS_LOCK_CAP;i++) {
        CHECK(efs_kv_key_lock(parts.shard[0],ino,row.generation,0,2*i,2*i+1,
              EFS_LOCK_PROC,1,r.owner.uuid,1,key,&len)==EFS_OK &&
              efs_kv_put(kv,key,len,&one,1)==EFS_OK,"seed over-cap import");
    }
    CHECK(efs_lock_drop_file(kv,ino,row.generation)==EFS_ERR_PROTO,"over-cap refuses atomically");
    CHECK(efs_lock_count(kv,ino,row.generation)==33,"over-cap preserves rows");
    efs_kv_mem_free(kv);
}

static void test_rejection_floor(void)
{
 struct efs_kv *kv=efs_kv_mem_create();efs_ino_t ino=0;struct efs_meta_row row;struct efs_lock_req r;
 CHECK(kv!=NULL,"floor lock KV");if(!kv)return;
 CHECK(efs_meta_apply_init(kv,1)==EFS_OK && efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"floor-lock",&ino)==EFS_OK && efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK,"floor lock file");
 fill_req(&r,ino,row.generation,EFS_LOCK_FCNTL,EFS_LOCK_EX,0,1,EFS_LOCK_PROC,1,93);
 CHECK(efs_lock_grant(kv,&r)==EFS_ERR_UNREGISTERED && efs_lock_count(kv,ino,row.generation)==0,"unregistered lock owner refused at apply");
 uint8_t zero[EFS_OPID_UUID_LEN]={0};struct efs_lock_req anonymous=r;memcpy(anonymous.owner.uuid,zero,sizeof(zero));
 CHECK(efs_lock_grant(kv,&anonymous)==EFS_ERR_UNREGISTERED,"lock without identity refused at apply");
 CHECK(establish(kv,ino,93)==EFS_OK,"establish lock owner");
 CHECK(efs_kv_mem_fail_next_batch(kv)==EFS_OK,"inject grant storage failure");
 CHECK(efs_lock_grant(kv,&r)==EFS_ERR_IO && efs_lock_count(kv,ino,row.generation)==0,"failed grant owns no row and retains IO verdict");
 CHECK(efs_lock_grant(kv,&r)==EFS_OK,"registered lock exact retry");
 CHECK(efs_session_fence_local(kv,efs_kv_inode_shard(ino),r.owner.uuid,2)==EFS_OK,"durable lock owner rejection floor");
 CHECK(efs_lock_grant(kv,&r)==EFS_ERR_FENCED,"revoked identical lock is not false grant success");
 CHECK(efs_lock_release(kv,&r)==EFS_OK,"old lock cleanup remains allowed");
 CHECK(efs_lock_grant(kv,&r)==EFS_ERR_FENCED && efs_lock_count(kv,ino,row.generation)==0,"revoked range cannot recreate ownership");
 r.domain=EFS_LOCK_FLOCK;r.owner.kind=EFS_LOCK_OFD;
 CHECK(efs_lock_grant(kv,&r)==EFS_ERR_FENCED,"both lock domains honor floor");
 r.owner.epoch=2;
 CHECK(efs_lock_grant(kv,&r)==EFS_ERR_UNREGISTERED,"next lock epoch needs establishment");
 efs_kv_mem_free(kv);
}

int main(void)
{
    test_rejection_floor();
    test_conflict_and_domains();
    test_cap_and_drop();
    test_atomic_final_close();
    if (failures) {
        fprintf(stderr, "test_lock: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_lock: OK\n");
    return 0;
}
