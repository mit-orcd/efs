/* Isolated session + open-lease SM. No sockets, no Raft. */
#include "efs/session.h"
#include "efs/txn.h"
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
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

#define T0 1000000000000000000ull

static const struct efs_meta_attrs g_at = { 1000, 1000, T0 };

static void uuid_of(uint8_t u[EFS_OPID_UUID_LEN], uint8_t n)
{
    memset(u, 0, EFS_OPID_UUID_LEN);
    u[15] = n;
}


struct admission_fault {struct efs_kv *base;int error;unsigned batches;};
static int admission_get(void *ctx,const uint8_t*k,uint32_t kn,uint8_t*v,uint32_t*vn)
{ return efs_kv_get(((struct admission_fault*)ctx)->base,k,kn,v,vn); }
static int admission_prefix(void *ctx,const uint8_t*p,uint32_t pn,
    int (*cb)(void*,const uint8_t*,uint32_t,const uint8_t*,uint32_t),void*u)
{ return efs_kv_scan_prefix(((struct admission_fault*)ctx)->base,p,pn,cb,u); }
static int admission_batch(void *ctx,const struct efs_kv_item *items,uint32_t n)
{
    struct admission_fault *f=ctx;f->batches++;
    CHECK(n==2,"combined admission exactly two rows");
    return f->error?f->error:efs_kv_batch(f->base,items,n);
}
static void test_register_establish(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16];uuid_of(uuid,227);
    CHECK(kv!=NULL,"combined admission KV");if(!kv)return;
    struct efs_session_rec rec;
    CHECK(efs_session_register_establish(kv,uuid,1,7)==EFS_ERR_NOT_FOUND,"missing global never ACTIVE");
    CHECK(!efs_session_create(kv,uuid,1),"combined global create");
    static const struct efs_kv_ops ops={.get=admission_get,.scan_prefix=admission_prefix,.batch=admission_batch};
    struct admission_fault fault={kv,EFS_ERR_IO,0};struct efs_kv wrapped={&ops,&fault};
    int errors[]={EFS_ERR_IO,EFS_ERR_NOMEM};
    for(unsigned i=0;i<2;i++){
        fault.error=errors[i];
        CHECK(efs_session_register_establish(&wrapped,uuid,1,7)==errors[i],"batch failure audible");
        CHECK(!efs_session_get(kv,uuid,&rec) && !efs_session_bit_get(rec.touched,7),"failed batch no touched bit");
        CHECK(efs_session_accept(kv,7,uuid,1)==EFS_ERR_BUSY,"failed batch no floor");
    }
    fault.error=0;CHECK(!efs_session_register_establish(&wrapped,uuid,1,7),"atomic retry");
    CHECK(fault.batches==3 && !efs_session_accept(kv,7,uuid,1),"one batch establishes floor");
    CHECK(!efs_session_get(kv,uuid,&rec) && efs_session_bit_get(rec.touched,7),"one batch registers domain");
    CHECK(!efs_session_register_establish(kv,uuid,1,7),"combined idempotence");
    CHECK(!efs_session_begin_fence(kv,uuid),"BEGIN after combined includes participant");
    CHECK(efs_session_register_establish(kv,uuid,1,7)==EFS_ERR_STALE,"FENCING rejects even existing old bit");
    CHECK(efs_session_register_establish(kv,uuid,2,7)==EFS_ERR_BUSY,"new epoch waits for fence");
    CHECK(!efs_session_fence_local(kv,7,uuid,2) && !efs_session_ack_fence(kv,uuid,7) && !efs_session_finish_fence(kv,uuid),"full participant barrier");
    CHECK(!efs_session_register_establish(kv,uuid,2,7),"next ACTIVE epoch admitted");
    CHECK(efs_session_accept(kv,7,uuid,1)==EFS_ERR_STALE && !efs_session_accept(kv,7,uuid,2),"floor retained");
    CHECK(efs_session_register_establish(kv,uuid,0,7)==EFS_ERR_INVAL,"zero epoch refused");
    CHECK(efs_session_register_establish(kv,uuid,2,EFS_SESSION_BITS)==EFS_ERR_INVAL,"invalid domain refused");
    struct efs_txid tx={{0}};tx.bytes[15]=81;struct efs_txn_parts parts={.n=1,.shard={7}};
    CHECK(!efs_session_prepare_owner(kv,7,uuid,2,&tx,&parts),"prepare session owner");
    CHECK(efs_session_register_establish(kv,uuid,2,7)==EFS_ERR_BUSY,"compound cannot overwrite prepared floor");
    CHECK(!efs_txn_resolve(kv,&tx,7,EFS_TXN_ABORT),"drain owner guard");
    efs_kv_mem_free(kv);
    char directory[]="/tmp/efs-session-admission-cold-XXXXXX";CHECK(mkdtemp(directory)!=NULL,"cold admission directory");
    struct efs_kv_lsm_cfg cfg={0};kv=efs_kv_lsm_open(directory,&cfg);CHECK(kv!=NULL,"cold admission KV");
    if(kv){
        CHECK(!efs_session_create(kv,uuid,1) && !efs_session_register_establish(kv,uuid,1,7),"durable two-row admission");
        efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(directory,&cfg);CHECK(kv!=NULL,"cold reopen");
        if(kv){CHECK(!efs_session_get(kv,uuid,&rec) && efs_session_bit_get(rec.touched,7) && !efs_session_accept(kv,7,uuid,1),"cold touched bit and floor survive together");efs_kv_lsm_close(kv);}
    }
}

static void test_register_and_fence(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t u[EFS_OPID_UUID_LEN];
    struct efs_session_rec r;

    uuid_of(u, 1);
    CHECK(kv != NULL, "kv");
    CHECK(efs_session_register(kv, u, 1, 1) == EFS_ERR_NOT_FOUND,
          "register missing owner before create");
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

static void test_registered_before_establish_fence(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16]={0x73};
    CHECK(kv!=NULL,"first-touch fence KV");if(!kv)return;
    CHECK(efs_session_create(kv,uuid,1)==EFS_OK,"first-touch owner ACTIVE");
    CHECK(efs_session_register(kv,uuid,1,7)==EFS_OK,"participant registered before BEGIN");
    CHECK(efs_session_begin_fence(kv,uuid)==EFS_OK,"BEGIN captures registered participant");
    CHECK(efs_session_register(kv,uuid,1,7)==EFS_OK,"FENCING old-bit success is only idempotent");
    CHECK(efs_session_accept(kv,7,uuid,1)==EFS_ERR_BUSY,"floor is not yet established");
    CHECK(efs_session_fence_local(kv,7,uuid,2)==EFS_OK,"fence creates absent rejection floor");
    CHECK(efs_session_establish(kv,7,uuid,1)==EFS_ERR_STALE,"late old ESTABLISH cannot cross floor");
    CHECK(efs_session_accept(kv,7,uuid,1)==EFS_ERR_STALE,"late mutation stays refused");
    CHECK(efs_session_ack_fence(kv,uuid,7)==EFS_OK,"captured participant ACK");
    CHECK(efs_session_finish_fence(kv,uuid)==EFS_OK,"barrier finishes after participant fence");
    CHECK(efs_session_establish(kv,7,uuid,1)==EFS_ERR_STALE,"completed barrier cannot resurrect old floor");
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

static int count_lease_key(void *ctx,const uint8_t *key,uint32_t klen,
                            const uint8_t *val,uint32_t vlen)
{
    (void)key;(void)klen;(void)val;(void)vlen;(*(unsigned*)ctx)++;return 0;
}
static void test_pending_create_ownership(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];uuid_of(uuid,9);
    efs_ino_t ino=0;struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_LEASE_PENDING_VALUE_LEN],pref[EFS_KV_KEY_MAX];
    uint32_t klen=0,plen=0;uint64_t token=UINT64_C(0xfedcba9876543210);
    CHECK(kv!=NULL,"pending KV");if(!kv)return;
    CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"pending init");
    CHECK(efs_meta_apply_create_file(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"pending",&ino)==EFS_OK,"pending file");
    CHECK(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK,"pending inode");
    CHECK(efs_kv_key_lease(efs_kv_inode_shard(ino),ino,row.generation,uuid,1,key,&klen)==EFS_OK,"pending key");
    CHECK(efs_lease_pending_value(token,value)==EFS_OK,"encode full-width token");
    CHECK(efs_lease_pending_value(0,value)==EFS_ERR_INVAL && efs_lease_pending_value(token,NULL)==EFS_ERR_INVAL,"invalid pending token rejected");
    CHECK(efs_kv_put(kv,key,klen,value,sizeof(value))==EFS_OK,"seed staged pending value");
    CHECK(efs_meta_apply_unlink(kv,EFS_ROOT_INO,"pending",T0)==EFS_OK,"unlink pending CREATE");
    CHECK(efs_lease_close(kv,ino,row.generation,uuid,1)==EFS_OK && efs_lease_any(kv,ino,row.generation)==1,"normal last close cannot drop unadopted CREATE");
    CHECK(efs_meta_apply_reclaim(kv,ino)==EFS_ERR_BUSY,"pending CREATE protects open-unlinked inode");
    CHECK(efs_lease_open(kv,ino,row.generation,uuid,1)==EFS_OK,"another local OPEN joins same record");
    CHECK(efs_kv_key_lease_prefix(efs_kv_inode_shard(ino),ino,row.generation,pref,&plen)==EFS_OK,"lease prefix");
    unsigned count=0;CHECK(efs_kv_scan_prefix(kv,pref,plen,count_lease_key,&count)==EFS_OK && count==1,"I19 one lease key despite pending and normal ownership");
    CHECK(efs_lease_close(kv,ino,row.generation,uuid,1)==EFS_OK && efs_lease_any(kv,ino,row.generation)==1,"normal edge preserves pending token");
    CHECK(efs_lease_pending_close(kv,ino,row.generation,uuid,1,token-1,1)==EFS_ERR_STALE,"wrong adoption token refused");
    CHECK(efs_lease_open(kv,ino,row.generation,uuid,1)==EFS_OK,"normal OPEN before abandoned-create cleanup");
    CHECK(efs_lease_pending_close(kv,ino,row.generation,uuid,1,token,0)==EFS_OK && efs_lease_any(kv,ino,row.generation)==1,"abandoned pending cleanup preserves another normal open");
    CHECK(efs_lease_pending_close(kv,ino,row.generation,uuid,1,token,1)==EFS_ERR_STALE && efs_lease_any(kv,ino,row.generation)==1,"delayed pending final close cannot delete a normal-only lease");
    CHECK(efs_lease_close(kv,ino,row.generation,uuid,1)==EFS_OK && efs_lease_any(kv,ino,row.generation)==0,"normal final close removes remaining ownership");
    CHECK(efs_lease_pending_value(token+1,value)==EFS_OK && efs_kv_put(kv,key,klen,value,sizeof(value))==EFS_OK,"new pending claim");
    CHECK(efs_lease_pending_close(kv,ino,row.generation,uuid,1,token,1)==EFS_ERR_STALE && efs_lease_any(kv,ino,row.generation)==1,"old cleanup cannot delete new pending claim");
    CHECK(efs_lease_pending_close(kv,ino,row.generation,uuid,1,token+1,1)==EFS_OK && efs_lease_any(kv,ino,row.generation)==0,"matching final-close token releases pending ownership");
    CHECK(efs_lease_pending_close(kv,ino,row.generation,uuid,1,token+1,1)==EFS_OK,"completed cleanup is idempotent");
    value[0]=2;value[1]=0;
    CHECK(efs_kv_put(kv,key,klen,value,sizeof(value))==EFS_OK,"malformed ownership fixture");
    CHECK(efs_lease_open(kv,ino,row.generation,uuid,1)==EFS_ERR_PROTO && efs_lease_close(kv,ino,row.generation,uuid,1)==EFS_ERR_PROTO,"malformed ownership is not overwritten or discarded");
    CHECK(efs_lease_pending_value(token,value)==EFS_OK && efs_kv_put(kv,key,klen,value,sizeof(value))==EFS_OK,"restore valid ownership");
    CHECK(efs_lease_open(kv,ino,row.generation,uuid,1)==EFS_OK,"normal plus pending before durable revoke");
    CHECK(efs_lease_drop_session(kv,efs_kv_inode_shard(ino),uuid,1)==EFS_OK && efs_lease_any(kv,ino,row.generation)==0,"session cleanup removes the one combined lease");
    CHECK(efs_meta_apply_reclaim(kv,ino)==EFS_OK,"reclaim after session ownership removed");
    efs_kv_mem_free(kv);
}


/* Exit without closing the engine after each acknowledged ownership edge.
 * Recovery must preserve the token and the independent normal-owner bit. */
static void test_negative_create_cold(void)
{
    char dir[]="/tmp/efs-negative-create-XXXXXX";
    struct efs_kv_lsm_cfg cfg={0};uint8_t uuid[EFS_OPID_UUID_LEN];uuid_of(uuid,23);
    CHECK(mkdtemp(dir)!=NULL,"negative cold directory");
    struct efs_opid_req q={0};memcpy(q.id.client_uuid,uuid,sizeof(uuid));q.id.session_epoch=1;q.id.seq=1;
    for(unsigned phase=0;phase<2;phase++) {
        pid_t pid=fork();CHECK(pid>=0,"negative cold fork");if(pid<0)break;
        if(!pid) {
            struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);int rc=kv?EFS_OK:EFS_ERR_IO;efs_ino_t ino=0;
            if(!phase) {
                if(!rc)rc=efs_meta_apply_init(kv,T0);
                if(!rc)rc=efs_session_create(kv,uuid,1);
                if(!rc)rc=efs_session_register(kv,uuid,1,1);
                if(!rc)rc=efs_session_establish(kv,1,uuid,1);
                if(!rc)rc=efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"conflict",&ino);
                if(!rc) {
                    rc=efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"conflict",0,-1,&q,uuid,1,&ino);
                    if(rc==EFS_ERR_EXIST)rc=EFS_OK;
                }
            } else if(!rc)rc=efs_meta_apply_unlink(kv,1,"conflict",T0+1);
            _exit(rc==EFS_OK?0:1);
        }
        int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status),"negative CREATE/delete acknowledged before crash");
        struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);CHECK(kv!=NULL,"negative verdict cold reopen");if(!kv)break;
        efs_ino_t ino=0;struct efs_meta_dentry dent;
        CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"conflict",0,-1,&q,uuid,1,&ino)==EFS_ERR_EXIST && !ino,"cold negative retry cannot create pending lease");
        CHECK(efs_meta_apply_lookup(kv,1,"conflict",&dent)==(phase?EFS_ERR_NOT_FOUND:EFS_OK),"cold negative retry preserves namespace");
        efs_kv_lsm_close(kv);
    }
    DIR *d=opendir(dir);if(d) {
        struct dirent *e;while((e=readdir(d))!=NULL) {
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
            char path[1024];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);CHECK(!unlink(path),"remove negative cold file");
        }
        closedir(d);CHECK(!rmdir(dir),"remove negative cold directory");
    }
}

static void test_pending_cold_edges(void)
{
  for (unsigned guarded=0;guarded<2;guarded++) {
    char dir[] = "/tmp/efs-pending-lease-XXXXXX";
    struct efs_kv_lsm_cfg cfg = {0};
    uint8_t uuid[EFS_OPID_UUID_LEN], key[EFS_KV_KEY_MAX];
    uint32_t klen = 0;
    efs_ino_t ino = 0;
    const uint64_t token = 1;
    uuid_of(uuid, 10);
    if (!mkdtemp(dir)) { CHECK(0, "pending cold directory"); return; }
    for (unsigned phase = 0; phase < 4+2*guarded; phase++) {
        pid_t pid = fork();
        CHECK(pid >= 0, "pending cold fork");
        if (pid < 0) break;
        if (pid == 0) {
            struct efs_kv *kv = efs_kv_lsm_open(dir, &cfg);
            int rc = kv ? EFS_OK : EFS_ERR_IO;
            struct efs_opid_req edge = {0};
            memcpy(edge.id.client_uuid,uuid,sizeof(uuid));
            edge.id.session_epoch=1;edge.id.seq=phase+1;edge.ack=phase;
            if (rc == EFS_OK && phase == 0) {
                rc = efs_meta_apply_init(kv, T0);
                if (rc==EFS_OK) rc=efs_session_create(kv,uuid,1);
                if (rc==EFS_OK) rc=efs_session_register(kv,uuid,1,1);
                if (rc==EFS_OK) rc=efs_session_establish(kv,1,uuid,1);
                struct efs_opid_req q = {0};
                memcpy(q.id.client_uuid, uuid, sizeof(uuid));
                q.id.session_epoch = 1; q.id.seq = token;
                if (rc == EFS_OK)
                    rc = efs_meta_apply_create_file_log_pending(kv, &g_at,
                           EFS_ROOT_INO, S_IFREG|0600, "cold-pending", 0, -1,
                           &q, uuid, 1, &ino);
            } else if (rc == EFS_OK && phase == 1) {
                rc = guarded ? efs_lease_guarded_edge(kv,ino,1,&edge,EFS_LEASE_EDGE_OPEN,0)
                             : efs_lease_open(kv,ino,1,uuid,1);
            } else if (rc == EFS_OK && phase == 2) {
                rc = guarded ? efs_lease_guarded_edge(kv,ino,1,&edge,EFS_LEASE_EDGE_CLOSE,0)
                             : efs_lease_close(kv,ino,1,uuid,1);
            } else if (rc == EFS_OK && phase==3) {
                rc = guarded ? efs_lease_guarded_edge(kv,ino,1,&edge,
                               EFS_LEASE_EDGE_CLOSE|EFS_LEASE_EDGE_CLEAR_PENDING,token)
                             : efs_lease_pending_close(kv,ino,1,uuid,1,token,1);
            } else if (rc == EFS_OK && phase==5) {
                rc=efs_session_fence_local(kv,1,uuid,2);
            } else if (rc == EFS_OK) {
                rc=efs_lease_guarded_edge(kv,ino+(UINT64_C(1)<<EFS_KV_SHARD_BITS),1,
                                           &edge,EFS_LEASE_EDGE_OPEN,0);
                if (rc==EFS_ERR_NOT_FOUND) rc=EFS_OK;
            }
            _exit(rc == EFS_OK ? 0 : 1);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0, "acknowledged pending cold edge");
        struct efs_kv *kv = efs_kv_lsm_open(dir, &cfg);
        CHECK(kv != NULL, "recover pending cold edge");
        if (!kv) break;
        if (phase == 0) {
            struct efs_meta_dentry dent;
            int rc = efs_meta_apply_lookup(kv, EFS_ROOT_INO, "cold-pending", &dent);
            CHECK(rc == EFS_OK, "cold CREATE namespace persisted with ownership");
            if (rc != EFS_OK) { efs_kv_lsm_close(kv); break; }
            ino = dent.ino;
            CHECK(efs_kv_key_lease(efs_kv_inode_shard(ino), ino, 1, uuid, 1,
                                 key, &klen) == EFS_OK, "pending cold key");
        }
        uint8_t value[18], expected[18]={0};
        uint32_t len = sizeof(value);
        int rc = efs_kv_get(kv, key, klen, value, &len);
        if ((!guarded && phase==3) || phase>=4) {
            CHECK(rc==EFS_ERR_NOT_FOUND,"cold acknowledged cleanup remains absent");
        } else {
            uint32_t expected_len=EFS_LEASE_PENDING_VALUE_LEN;
            CHECK(efs_lease_pending_value(token,expected)==EFS_OK,"cold expected token");
            if (guarded && phase) {
                memset(expected,0,sizeof(expected));expected[0]=3;
                expected[1]=phase==1?3:(phase==2?2:0);
                expected[9]=(uint8_t)(phase+1);expected[17]=phase<3?1:0;
                expected_len=sizeof(expected);
            } else if (phase==1) expected[1]|=1;
            CHECK(rc==EFS_OK && len==expected_len && !memcmp(value,expected,expected_len),
                  "cold token, guarded sequence and ownership bits exact");
            if (guarded && phase==3)
                CHECK(efs_lease_any(kv,ino,1)==0,"cold closed record holds no ownership");
        }
        if (guarded && phase==5) {
            uint8_t wkey[EFS_KV_KEY_MAX];uint32_t wl=0;
            CHECK(efs_kv_key_opid(1,uuid,1,wkey,&wl)==EFS_OK,"cold fenced window key");
            wkey[2]=EFS_KV_KIND_LEASE_EDGE;len=sizeof(value);
            CHECK(efs_kv_get(kv,wkey,wl,value,&len)==EFS_ERR_NOT_FOUND,"cold local fence removed guarded window");
        }
        if (guarded && phase>=4) {
            struct efs_opid_req old={0};memcpy(old.id.client_uuid,uuid,sizeof(uuid));
            old.id.session_epoch=1;old.id.seq=2;old.ack=1;
            CHECK(efs_lease_guarded_edge(kv,ino,1,&old,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,
                  "cold retired floor rejects delayed OPEN after tombstone deletion");
        }
        efs_kv_lsm_close(kv);
    }
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            CHECK(unlink(path) == 0, "remove owned pending fixture file");
        }
        closedir(d);
        CHECK(rmdir(dir) == 0, "remove owned pending fixture directory");
    }
  }
}

static void guarded_session_init(struct efs_kv *kv, uint8_t uuid[EFS_OPID_UUID_LEN])
{
    uuid_of(uuid, 20);
    CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"guarded metadata init");
    CHECK(efs_session_create(kv,uuid,1)==EFS_OK,"guarded session create");
    CHECK(efs_session_register(kv,uuid,1,1)==EFS_OK,"guarded shard register");
    CHECK(efs_session_establish(kv,1,uuid,1)==EFS_OK,"guarded shard establish");
}
static struct efs_opid_req guarded_request(const uint8_t *uuid,uint64_t seq,uint64_t ack)
{
    struct efs_opid_req q={0};
    memcpy(q.id.client_uuid,uuid,EFS_OPID_UUID_LEN);q.id.session_epoch=1;q.id.seq=seq;q.ack=ack;
    return q;
}
static unsigned guarded_rows(struct efs_kv *kv)
{
    uint8_t key[EFS_KV_KEY_MAX];uint32_t len=0;unsigned count=0;
    CHECK(efs_kv_key_lease_shard_prefix(1,key,&len)==EFS_OK,"guarded lease prefix");
    CHECK(efs_kv_scan_prefix(kv,key,len,count_lease_key,&count)==EFS_OK,"guarded row scan");
    return count;
}
static int (*guarded_batch_original)(void *,const struct efs_kv_item *,uint32_t);
static int guarded_batch_fail;
static int guarded_batch_fault(void *ctx,const struct efs_kv_item *items,uint32_t count)
{
    return guarded_batch_fail?EFS_ERR_IO:guarded_batch_original(ctx,items,count);
}
static void test_guarded_lease_edges(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];efs_ino_t ino=0;
    CHECK(kv!=NULL,"guarded KV");if(!kv)return;
    guarded_session_init(kv,uuid);
    CHECK(efs_meta_apply_create_file(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"edges",&ino)==EFS_OK,"guarded file");
    struct efs_opid_req q=guarded_request(uuid,2,0);
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"CLOSE before delayed OPEN");
    CHECK(efs_lease_any(kv,ino,1)==0,"closed replay row is not an active lease");
    int verdict=123;
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0,&verdict)==1 && verdict==EFS_OK,"read-only lost-reply probe finds durable CLOSE");
    q.id.seq=1;
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0,&verdict)==0,"read-only missing probe does not acquire ownership");
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"late OPEN cannot resurrect CLOSE");
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0,&verdict)==1 && verdict==EFS_ERR_STALE,"read-only probe preserves recorded stale verdict");
    q.id.seq=3;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"later OPEN accepted");
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_PROTO,"same identity different payload refused");
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0,&verdict)==EFS_ERR_PROTO,"read-only probe rejects changed payload");
    q.id.seq=2;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && efs_lease_any(kv,ino,1)==1,"replayed CLOSE cannot delete later OPEN");
    CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_ERR_STALE && efs_lease_any(kv,ino,1)==1,"legacy edges refuse guarded ownership mutation");
    uint8_t malformed_key[EFS_KV_KEY_MAX],saved_value[1024],bad_value[1024];
    uint32_t malformed_len=0,saved_len=sizeof(saved_value);
    CHECK(efs_kv_key_opid(1,uuid,1,malformed_key,&malformed_len)==EFS_OK,"guarded window corruption key");
    malformed_key[2]=EFS_KV_KIND_LEASE_EDGE;
    CHECK(efs_kv_get(kv,malformed_key,malformed_len,saved_value,&saved_len)==EFS_OK,"save guarded window");
    memcpy(bad_value,saved_value,saved_len);bad_value[12]=EFS_OPID_REPLY_CACHE+1;
    CHECK(efs_kv_put(kv,malformed_key,malformed_len,bad_value,saved_len)==EFS_OK,"seed malformed guarded window");
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0,&verdict)==EFS_ERR_PROTO && efs_lease_any(kv,ino,1)==1,"malformed window cannot authorize a reply or drop ownership");
    CHECK(efs_kv_put(kv,malformed_key,malformed_len,saved_value,saved_len)==EFS_OK,"restore guarded window");
    q.id.seq=4;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"guarded final CLOSE");
    q.id.seq=5;q.ack=4;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK && guarded_rows(kv)==1,"ack retirement preserves later OPEN on same key");
    struct efs_kv_ops ops=*kv->ops;const struct efs_kv_ops *original=kv->ops;
    guarded_batch_original=ops.batch;ops.batch=guarded_batch_fault;kv->ops=&ops;
    q.id.seq=6;q.ack=5;guarded_batch_fail=1;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_IO && efs_lease_any(kv,ino,1)==1,"failed effect/verdict batch preserves ownership");
    guarded_batch_fail=0;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && efs_lease_any(kv,ino,1)==0,"retry commits final close");
    CHECK(efs_meta_apply_unlink(kv,EFS_ROOT_INO,"edges",T0)==EFS_OK,"unlink guarded closed file");
    int reclaimed=efs_meta_apply_reclaim(kv,ino);
    CHECK(reclaimed==EFS_OK || reclaimed==EFS_ERR_NOT_FOUND,"closed tombstone does not block reclaim");
    struct efs_meta_row dead;
    CHECK(efs_meta_apply_get_inode(kv,ino,&dead)==EFS_ERR_NOT_FOUND,"guarded closed inode actually removed");
    q.id.seq=7;q.ack=6;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_NOT_FOUND,"future OPEN cannot resurrect reclaimed inode");
    q=guarded_request(uuid,1,0);
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"ack floor rejects old OPEN after row retirement");
    q=guarded_request(uuid,8,7);
    CHECK(efs_lease_guarded_edge(kv,EFS_ROOT_INO,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_INVAL,"guarded OPEN rejects directories");
    CHECK(efs_lease_guarded_probe(kv,EFS_ROOT_INO,1,&q,EFS_LEASE_EDGE_OPEN,0,&verdict)==1 && verdict==EFS_ERR_INVAL,"directory refusal has a durable replay verdict");
    kv->ops=original;efs_kv_mem_free(kv);
}
static int (*guarded_del_original)(void *,const uint8_t *,uint32_t);
static int guarded_del_fail;
static int guarded_del_fault(void *ctx,const uint8_t *key,uint32_t len)
{
    return guarded_del_fail?EFS_ERR_IO:guarded_del_original(ctx,key,len);
}
static void test_guarded_pending_and_fence(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];efs_ino_t ino=0;
    CHECK(kv!=NULL,"guarded pending KV");if(!kv)return;
    guarded_session_init(kv,uuid);
    struct efs_opid_req create=guarded_request(uuid,1,0);
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"guarded-pending",0,-1,&create,uuid,1,&ino)==EFS_OK,"atomic pending before adoption");
    struct efs_opid_req q=guarded_request(uuid,3,1);
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"normal OPEN before delayed pending close");
    q.id.seq=2;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE|EFS_LEASE_EDGE_CLEAR_PENDING,1)==EFS_ERR_STALE && efs_lease_any(kv,ino,1)==1,"late combined final-close cannot drop later ownership");
    q.id.seq=4;q.ack=3;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLEAR_PENDING,1)==EFS_OK && efs_lease_any(kv,ino,1)==1,"abandoned CREATE cleanup preserves normal OPEN");
    q.id.seq=5;q.ack=4;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && efs_lease_any(kv,ino,1)==0,"normal last close after pending cleanup");
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_BUSY,"cannot retire floor before durable global fence");
    q.id.seq=6;q.ack=5;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"active owner before revocation");
    uint8_t window[EFS_KV_KEY_MAX],value[1024];uint32_t wl=0,vl=sizeof(value);
    CHECK(efs_kv_key_opid(1,uuid,1,window,&wl)==EFS_OK,"guarded retirement window key");
    window[2]=EFS_KV_KIND_LEASE_EDGE;
    CHECK(efs_kv_get(kv,window,wl,value,&vl)==EFS_OK,"guarded replay window before fence");
    CHECK(efs_session_begin_fence(kv,uuid)==EFS_OK,"begin guarded fence");
    CHECK(efs_session_fence_local(kv,1,uuid,2)==EFS_OK,"durable local reject floor");
    vl=sizeof(value);
    CHECK(efs_kv_get(kv,window,wl,value,&vl)==EFS_ERR_NOT_FOUND,"local fence retires redundant guarded verdicts");
    CHECK(efs_lease_any(kv,ino,1)==1,"local fence retains active ownership until global barrier");
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_BUSY,"local floor alone cannot retire replay identity");
    q.id.seq=6;q.ack=5;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"local fence rejects old edges before global barrier finishes");
    CHECK(efs_session_ack_fence(kv,uuid,1)==EFS_OK && efs_session_finish_fence(kv,uuid)==EFS_OK,"complete durable barrier");
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_BUSY && guarded_rows(kv)==1,"retirement cannot expand into old ownership deletion");
    struct efs_session_cleanup_key cleanup[EFS_SESSION_CLEANUP_MAX];uint32_t nc=0;
    CHECK(efs_session_cleanup_select(kv,1,uuid,1,cleanup,&nc)==EFS_OK && nc==1 &&
          efs_session_cleanup_apply(kv,1,uuid,1,cleanup,nc)==EFS_OK,"bounded ownership phase precedes retirement");
    struct efs_txid retire_tx={{117}};struct efs_txn_parts retire_parts={.n=1,.shard={1}};
    uint8_t replacement=1;
    CHECK(efs_txn_prepare_excl_value(kv,&retire_tx,&retire_parts,window,wl,NULL,
          EFS_TXN_ABSENT,EFS_TXN_PUT,&replacement,1)==EFS_OK,"prepared receipt ownership");
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_BUSY,"retirement respects prepared receipt ownership");
    CHECK(efs_txn_resolve(kv,&retire_tx,1,EFS_TXN_ABORT)==EFS_OK,"release receipt ownership");
    uint8_t retained[13]={1};
    CHECK(efs_kv_put(kv,window,wl,retained,sizeof(retained))==EFS_OK,"seed retained reply window");
    const struct efs_kv_ops *original_ops=kv->ops;struct efs_kv_ops fault_ops=*original_ops;
    guarded_del_original=fault_ops.del;fault_ops.del=guarded_del_fault;kv->ops=&fault_ops;
    guarded_del_fail=1;
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_IO,"retirement propagates storage failure");
    vl=sizeof(value);
    CHECK(efs_kv_get(kv,window,wl,value,&vl)==EFS_OK && vl==sizeof(retained),"failed retirement keeps replay window");
    guarded_del_fail=0;kv->ops=original_ops;
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_OK && guarded_rows(kv)==0,"empty phase retires only replay floor");
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_OK,"retirement is idempotent");
    q.id.seq=6;q.ack=5;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"retired old session cannot reopen after floor deletion");
    CHECK(efs_session_register(kv,uuid,2,1)==EFS_OK && efs_session_establish(kv,1,uuid,2)==EFS_OK,"new epoch establish");
    q=guarded_request(uuid,1,0);q.id.session_epoch=2;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"new epoch has independent sequence identity");
    efs_kv_mem_free(kv);
    kv=efs_kv_mem_create();CHECK(kv!=NULL,"combined adoption KV");if(!kv)return;
    guarded_session_init(kv,uuid);
    create=guarded_request(uuid,1,0);ino=0;
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"combined-adoption",0,-1,&create,uuid,1,&ino)==EFS_OK,"combined adoption starts pending");
    q=guarded_request(uuid,2,1);
    unsigned combined=EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING;
    const struct efs_kv_ops *combined_original=kv->ops;
    struct efs_kv_ops combined_ops=*combined_original;
    guarded_batch_original=combined_ops.batch;combined_ops.batch=guarded_batch_fault;kv->ops=&combined_ops;
    guarded_batch_fail=1;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,combined,1)==EFS_ERR_IO && efs_lease_any(kv,ino,1)==1,"failed combined batch retains pending ownership");
    int combined_verdict=999;
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,combined,1,&combined_verdict)==0,"failed combined batch publishes no success receipt");
    guarded_batch_fail=0;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,combined,1)==EFS_OK && efs_lease_any(kv,ino,1)==1,"atomic combined edge adopts normal ownership");
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,combined,1,&combined_verdict)==1 && combined_verdict==EFS_OK,"lost combined reply has exact durable verdict");
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,combined,1)==EFS_OK,"identical combined retry survives cleared pending token");
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,combined,2)==EFS_ERR_PROTO,"changed combined pending token cannot replay success");
    q.id.seq=3;q.ack=2;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && efs_lease_any(kv,ino,1)==0,"combined adoption leaves no pending hold after final CLOSE");
    kv->ops=combined_original;
    efs_kv_mem_free(kv);
    kv=efs_kv_mem_create();CHECK(kv!=NULL,"negative combined adoption KV");if(!kv)return;
    guarded_session_init(kv,uuid);ino=0;create=guarded_request(uuid,1,0);
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"negative-combined",0,-1,&create,uuid,1,&ino)==EFS_OK,"negative adoption pending fixture");
    q=guarded_request(uuid,2,1);
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,2,&q,combined,1)==EFS_ERR_STALE,"wrong FileID records negative combined verdict");
    CHECK(efs_lease_guarded_probe_deferred(kv,ino,2,&q,combined,1,&combined_verdict)==1 && combined_verdict==EFS_ERR_STALE,"negative combined edge has durable exact proof");
    CHECK(!efs_lease_any(kv,ino,2) && efs_lease_any(kv,ino,1)==1,"negative combined edge acquires nothing and preserves original pending ownership");
    struct efs_opid_req clear=guarded_request(uuid,3,1);
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&clear,EFS_LEASE_EDGE_CLEAR_PENDING,1)==EFS_OK && !efs_lease_any(kv,ino,1),"original CREATE token can clear without fictional CLOSE");
    CHECK(efs_lease_guarded_probe_deferred(kv,ino,2,&q,combined,1,&combined_verdict)==1 && combined_verdict==EFS_ERR_STALE,"later cleanup preserves original negative verdict");
    efs_kv_mem_free(kv);
}
static void test_guarded_retire_missing_authority(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];efs_ino_t ino=0;
    CHECK(kv!=NULL,"missing authority KV");if(!kv)return;
    guarded_session_init(kv,uuid);
    CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"authority",&ino)==EFS_OK,"authority file");
    struct efs_opid_req q=guarded_request(uuid,1,0);
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"authority active owner");
    uint8_t key[EFS_KV_KEY_MAX];uint32_t kl=0;
    CHECK(efs_kv_key_session(efs_kv_session_shard(uuid),uuid,key,&kl)==EFS_OK && efs_kv_del(kv,key,kl)==EFS_OK,"simulate unhosted session owner");
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_NOT_FOUND,"missing global authority cannot claim successful retirement");
    int verdict=EFS_ERR_BUSY;
    CHECK(efs_lease_any(kv,ino,1)==1 && efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0,&verdict)==1 && verdict==EFS_OK,"missing authority preserves ownership and replay evidence");
    efs_kv_mem_free(kv);
}
static void test_guarded_window_bound(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];
    CHECK(kv!=NULL,"guarded bound KV");if(!kv)return;
    guarded_session_init(kv,uuid);
    for(unsigned i=1;i<=EFS_OPID_REPLY_CACHE;i++) {
        struct efs_opid_req q=guarded_request(uuid,i,0);
        efs_ino_t ino=((uint64_t)(i+100)<<EFS_KV_SHARD_BITS)|1;
        CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"bounded unacknowledged closed row");
        CHECK(efs_lease_any(kv,ino,1)==0,"missing inode CLOSE holds no ownership");
    }
    uint8_t wkey[EFS_KV_KEY_MAX],wvalue[1024];uint32_t wklen=0,wn=sizeof(wvalue);
    CHECK(efs_kv_key_opid(1,uuid,1,wkey,&wklen)==EFS_OK,"bounded window key");
    wkey[2]=EFS_KV_KIND_LEASE_EDGE;
    CHECK(efs_kv_get(kv,wkey,wklen,wvalue,&wn)==EFS_OK && wn==13+60*EFS_OPID_REPLY_CACHE,"replay storage has a fixed worst-case byte bound");
    efs_ino_t next=((uint64_t)200<<EFS_KV_SHARD_BITS)|1;
    struct efs_opid_req q=guarded_request(uuid,EFS_OPID_REPLY_CACHE+1,0);
    CHECK(efs_lease_guarded_edge(kv,next,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_BUSY && guarded_rows(kv)==EFS_OPID_REPLY_CACHE,"window pressure refuses new effect without unbounded tombstones");
    struct efs_kv_ops ops=*kv->ops;const struct efs_kv_ops *original=kv->ops;
    guarded_batch_original=ops.batch;ops.batch=guarded_batch_fault;kv->ops=&ops;
    q.ack=EFS_OPID_REPLY_CACHE;guarded_batch_fail=1;
    CHECK(efs_lease_guarded_edge(kv,next,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_IO && guarded_rows(kv)==EFS_OPID_REPLY_CACHE,"failed ack/effect batch cannot lose replay tombstones");
    guarded_batch_fail=0;
    CHECK(efs_lease_guarded_edge(kv,next,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && guarded_rows(kv)==1,"contiguous ack retires bounded closed rows atomically");
    q.id.seq=1;q.ack=0;
    CHECK(efs_lease_guarded_edge(kv,next,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"retired sequence remains fenced");
    kv->ops=original;efs_kv_mem_free(kv);
}

static void test_guarded_malformed_ownership(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];efs_ino_t ino=0;
    CHECK(kv!=NULL,"malformed guarded KV");if(!kv)return;
    guarded_session_init(kv,uuid);
    CHECK(efs_meta_apply_create_file(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"malformed",&ino)==EFS_OK,"malformed guarded file");
    struct efs_opid_req q=guarded_request(uuid,1,0);
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"valid guarded owner");
    uint8_t key[EFS_KV_KEY_MAX],good[18],bad[18];uint32_t len=0,n=sizeof(good);
    CHECK(efs_kv_key_lease(1,ino,1,uuid,1,key,&len)==EFS_OK,"malformed lease key");
    CHECK(efs_kv_get(kv,key,len,good,&n)==EFS_OK && n==sizeof(good),"save guarded ownership");
    memcpy(bad,good,sizeof(bad));bad[1]=0;bad[17]=1;
    CHECK(efs_kv_put(kv,key,len,bad,sizeof(bad))==EFS_OK,"malformed closed row retains a token");
    q.id.seq=2;
    CHECK(efs_lease_any(kv,ino,1)==1 && efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_PROTO,"malformed ownership conservatively protects inode and refuses mutation");
    int verdict=0;
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0,&verdict)==0,"failed malformed mutation records no success");
    CHECK(efs_kv_put(kv,key,len,good,sizeof(good))==EFS_OK,"restore ownership after evidence check");
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && efs_lease_any(kv,ino,1)==0,"same identity retries after malformed evidence repaired");
    efs_kv_mem_free(kv);
}

static void test_guarded_edge_permutations(void)
{
    unsigned tested=0;
    for(unsigned a=1;a<=4;a++)for(unsigned b=1;b<=4;b++)
    for(unsigned c=1;c<=4;c++)for(unsigned d=1;d<=4;d++) {
        if(a==b || a==c || a==d || b==c || b==d || c==d)continue;
        struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];efs_ino_t ino=0;
        CHECK(kv!=NULL,"permutation KV");if(!kv)return;
        guarded_session_init(kv,uuid);
        CHECK(efs_meta_apply_create_file(kv,&g_at,EFS_ROOT_INO,S_IFREG|0600,"order",&ino)==EFS_OK,"permutation file");
        unsigned order[4]={a,b,c,d},latest=0;
        for(unsigned i=0;i<4;i++) {
            struct efs_opid_req q=guarded_request(uuid,order[i],0);
            int expect=order[i]>latest?EFS_OK:EFS_ERR_STALE;
            unsigned flags=(order[i]&1)?EFS_LEASE_EDGE_OPEN:EFS_LEASE_EDGE_CLOSE;
            CHECK(efs_lease_guarded_edge(kv,ino,1,&q,flags,0)==expect,"all arrival orders obey per-file sequence");
            if(order[i]>latest)latest=order[i];
            CHECK(efs_lease_any(kv,ino,1)==(int)(latest&1),"ownership follows newest edge, not arrival order");
        }
        struct efs_opid_req q=guarded_request(uuid,5,4);
        CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK && guarded_rows(kv)==1,"all arrival orders safely retire old closed edge");
        q=guarded_request(uuid,2,0);
        CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_STALE && efs_lease_any(kv,ino,1)==1,"retired delayed CLOSE preserves new ownership in every order");
        efs_kv_mem_free(kv);tested++;
    }
    CHECK(tested==24,"all four-edge permutations covered");
}

/* Pending adoption cleanup is independent of the normal-open bit. A stale
 * cleanup keeps a bounded owner until a fresh, resolved edge can release it. */
static void test_pending_adoption_permutations(void)
{
    unsigned tested=0;
    for(unsigned a=2;a<=6;a++)for(unsigned b=2;b<=6;b++)
    for(unsigned c=2;c<=6;c++)for(unsigned d=2;d<=6;d++)for(unsigned e=2;e<=6;e++) {
        unsigned order[5]={a,b,c,d,e},bits=0;
        for(unsigned i=0;i<5;i++)bits|=1u<<order[i];
        if(bits!=124)continue;
        struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[EFS_OPID_UUID_LEN];efs_ino_t ino=0;
        CHECK(kv!=NULL,"pending permutation KV");if(!kv)return;
        guarded_session_init(kv,uuid);
        struct efs_opid_req create=guarded_request(uuid,1,0);
        CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"adopt",0,-1,&create,uuid,1,&ino)==EFS_OK,"pending permutation CREATE");
        uint64_t latest=0;int normal=0,pending=1;
        for(unsigned i=0;i<5;i++) {
            unsigned seq=order[i],flags=seq==4?EFS_LEASE_EDGE_CLEAR_PENDING:
                    ((seq==2 || seq==5)?EFS_LEASE_EDGE_OPEN:EFS_LEASE_EDGE_CLOSE);
            struct efs_opid_req q=guarded_request(uuid,seq,0);
            int expect=seq<latest?EFS_ERR_STALE:EFS_OK;
            CHECK(efs_lease_guarded_edge(kv,ino,1,&q,flags,seq==4?1:0)==expect,"pending/adoption edge ordering verdict");
            if(expect==EFS_OK) {
                latest=seq;
                if(seq==4)pending=0;else normal=flags==EFS_LEASE_EDGE_OPEN;
            }
            CHECK(efs_lease_any(kv,ino,1)==(normal || pending),"each adoption ordering preserves precisely owned state");
            int verdict=EFS_ERR_BUSY;
            CHECK(efs_lease_guarded_probe(kv,ino,1,&q,flags,seq==4?1:0,&verdict)==1 && verdict==expect,"ambiguous adoption edge resolves exact stored verdict");
        }
        CHECK(!normal,"highest final-close always removes normal ownership");
        struct efs_opid_req cleanup=guarded_request(uuid,7,6);
        CHECK(efs_lease_guarded_edge(kv,ino,1,&cleanup,pending?EFS_LEASE_EDGE_CLEAR_PENDING:EFS_LEASE_EDGE_CLOSE,pending?1:0)==EFS_OK && !efs_lease_any(kv,ino,1),"fresh cleanup releases stale pending owner after all outcomes known");
        struct efs_opid_req old=guarded_request(uuid,2,0);
        CHECK(efs_lease_guarded_edge(kv,ino,1,&old,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"retired adoption cannot resurrect ownership");
        CHECK(efs_meta_apply_unlink(kv,1,"adopt",T0+1)==EFS_OK,"reclaim after resolved adoption cleanup");
        struct efs_meta_row row;CHECK(efs_meta_apply_get_inode(kv,ino,&row)==EFS_ERR_NOT_FOUND,"no orphan after adoption permutations");
        efs_kv_mem_free(kv);tested++;
    }
    CHECK(tested==120,"all five pending/adoption edge permutations covered");
}

static void test_prepared_owner_fence(void)
{
    for (unsigned commit = 0; commit < 2; commit++) {
        struct efs_kv *kv = efs_kv_mem_create();
        CHECK(kv != NULL, "owner fence kv");
        if (!kv)
            continue;
        uint8_t uuid[EFS_OPID_UUID_LEN];
        uuid_of(uuid, 201 + commit);
        struct efs_txid tx = {{0}}, other = {{0}};
        tx.bytes[15] = 1;
        other.bytes[15] = 2;
        struct efs_txn_parts parts = {.n = 1, .shard = {7}};
        CHECK(efs_session_create(kv, uuid, 1) == EFS_OK, "owner global session");
        CHECK(efs_session_register(kv, uuid, 1, 7) == EFS_OK, "owner register");
        CHECK(efs_session_establish(kv, 7, uuid, 1) == EFS_OK, "owner establish");
        CHECK(efs_session_fence_local(kv,7,uuid,0)==EFS_ERR_INVAL,"zero epoch is not a fence barrier");
        CHECK(efs_session_prepare_owner(kv, 8, uuid, 1, &tx, &parts) == EFS_ERR_INVAL,
              "owner must be participant");
        CHECK(efs_session_prepare_owner(kv, 7, uuid, 1, &tx, &parts) == EFS_OK,
              "prepare owner guard");
        CHECK(efs_session_prepare_owner(kv, 7, uuid, 1, &tx, &parts) == EFS_OK,
              "owner retry idempotent");
        CHECK(efs_session_prepare_owner(kv, 7, uuid, 1, &other, &parts) == EFS_ERR_BUSY,
              "concurrent owner no wait");
        uint8_t lease_key[EFS_KV_KEY_MAX], pending[EFS_LEASE_PENDING_VALUE_LEN];
        uint32_t lease_len = 0;
        CHECK(efs_kv_key_lease(7, 7, 1, uuid, 1, lease_key, &lease_len) == EFS_OK,
              "prepared lease key");
        CHECK(efs_lease_pending_value(1, pending) == EFS_OK, "prepared pending value");
        CHECK(efs_txn_prepare_excl_value(kv, &tx, &parts, lease_key, lease_len, NULL,
                                         EFS_TXN_ABSENT, EFS_TXN_PUT, pending,
                                         sizeof(pending)) == EFS_OK,
              "lease and owner same transaction");
        CHECK(efs_lease_any(kv, 7, 1) == 0, "prepared lease not materialized before commit");
        CHECK(efs_session_begin_fence(kv, uuid) == EFS_OK, "begin owner fence");
        CHECK(efs_session_fence_local(kv, 7, uuid, 2) == EFS_ERR_BUSY,
              "prepared owner blocks fence ACK");
        CHECK(efs_session_establish(kv, 7, uuid, 2) == EFS_ERR_BUSY,
              "new establish cannot overwrite prepared guard");
        CHECK(efs_session_reclaimable(kv, 7, uuid, 1) == EFS_ERR_BUSY,
              "cannot retire unresolved owner");
        const struct efs_kv_ops *saved_ops = kv->ops;
        struct efs_kv_ops fault_ops = *saved_ops;
        guarded_batch_original = fault_ops.batch;
        fault_ops.batch = guarded_batch_fault;
        kv->ops = &fault_ops;
        guarded_batch_fail = 1;
        CHECK(efs_txn_resolve(kv, &tx, 7, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) == EFS_ERR_IO,
              "failed resolve keeps owner and effect atomic");
        guarded_batch_fail = 0;
        kv->ops = saved_ops;
        CHECK(efs_session_fence_local(kv, 7, uuid, 2) == EFS_ERR_BUSY,
              "failed resolve cannot release guard early");
        CHECK(efs_lease_any(kv, 7, 1) == 0, "failed resolve publishes no pending lease");
        CHECK(efs_txn_resolve(kv, &tx, 7, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) == EFS_OK,
              "drain prepared owner");
        CHECK(efs_lease_any(kv, 7, 1) == (int)commit, "resolved owner and lease agree");
        CHECK(efs_session_fence_local(kv, 7, uuid, 2) == EFS_OK,
              "floor only after transaction resolution");
        CHECK(efs_txn_resolve(kv, &tx, 7, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) == EFS_OK,
              "late resolve idempotent");
        CHECK(efs_session_accept(kv, 7, uuid, 1) == EFS_ERR_STALE,
              "late resolve cannot roll floor back");
        CHECK(efs_session_prepare_owner(kv, 7, uuid, 1, &other, &parts) == EFS_ERR_STALE,
              "fenced owner cannot prepare");
        CHECK(efs_session_ack_fence(kv, uuid, 7) == EFS_OK, "ack drained shard");
        CHECK(efs_session_finish_fence(kv, uuid) == EFS_OK, "finish drained owner barrier");
        CHECK(efs_session_reclaimable(kv, 7, uuid, 1) == EFS_OK, "fully drained owner may retire");
        CHECK(efs_lease_drop_session(kv, 7, uuid, 1) == EFS_OK,
              "drop committed owner after complete barrier");
        CHECK(efs_lease_any(kv, 7, 1) == 0, "no ownership survives completed revocation");
        efs_kv_mem_free(kv);
    }
}

static void test_prepared_owner_cold(void)
{
    for (unsigned commit = 0; commit < 2; commit++) {
        char dir[] = "/tmp/efs-prepared-owner-XXXXXX";
        if (!mkdtemp(dir)) {
            CHECK(0, "cold owner root");
            return;
        }
        struct efs_kv_lsm_cfg cfg = {0};
        uint8_t uuid[EFS_OPID_UUID_LEN];
        uuid_of(uuid, 211 + commit);
        struct efs_txid tx = {{0}};
        tx.bytes[15] = 9;
        struct efs_txn_parts parts = {.n = 1, .shard = {1}};
        for (unsigned phase = 0; phase < 4; phase++) {
            pid_t pid = fork();
            CHECK(pid >= 0, "cold owner fork");
            if (pid < 0)
                break;
            if (!pid) {
                struct efs_kv *kv = efs_kv_lsm_open(dir, &cfg);
                if (!kv)
                    _exit(2);
                if (!phase) {
                    CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"cold pending namespace init");
                    CHECK(efs_session_create(kv, uuid, 1) == EFS_OK, "cold owner create");
                    CHECK(efs_session_register(kv, uuid, 1, 1) == EFS_OK, "cold owner register");
                    CHECK(efs_session_establish(kv, 1, uuid, 1) == EFS_OK, "cold owner establish");
                    CHECK(efs_session_prepare_owner(kv, 1, uuid, 1, &tx, &parts) == EFS_OK,
                          "cold prepare owner");
                    uint8_t key[EFS_KV_KEY_MAX], value[EFS_LEASE_PENDING_VALUE_LEN];
                    uint32_t len = 0;
                    CHECK(efs_kv_key_lease(1, 4097, 1, uuid, 1, key, &len) == EFS_OK,
                          "cold lease key");
                    CHECK(efs_lease_pending_value(9, value) == EFS_OK, "cold lease value");
                    CHECK(efs_txn_prepare_excl_value(kv, &tx, &parts, key, len, NULL,
                                                     EFS_TXN_ABSENT, EFS_TXN_PUT, value,
                                                     sizeof(value)) == EFS_OK,
                          "cold pending lease intent");
                } else if (phase == 1) {
                    CHECK(efs_session_begin_fence(kv, uuid) == EFS_OK, "cold begin fence");
                    CHECK(efs_session_fence_local(kv, 1, uuid, 2) == EFS_ERR_BUSY,
                          "cold prepared guard blocks fence");
                } else if (phase == 2) {
                    CHECK(efs_txn_resolve(kv, &tx, 1, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) ==
                              EFS_OK,
                          "cold resolve owner");
                    CHECK(efs_lease_any(kv, 4097, 1) == (int)commit, "cold resolved lease exact");
                    CHECK(efs_session_fence_local(kv, 1, uuid, 2) == EFS_OK,
                          "cold floor after resolve");
                    CHECK(efs_session_ack_fence(kv, uuid, 1) == EFS_OK, "cold ACK");
                    CHECK(efs_session_finish_fence(kv, uuid) == EFS_OK, "cold finish");
                    CHECK(efs_lease_drop_session(kv, 1, uuid, 1) == EFS_OK, "cold drop");
                } else {
                    CHECK(efs_txn_resolve(kv, &tx, 1, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) ==
                              EFS_OK,
                          "cold delayed resolve");
                    CHECK(efs_session_accept(kv, 1, uuid, 1) == EFS_ERR_STALE,
                          "cold floor retained");
                    CHECK(efs_lease_any(kv, 4097, 1) == 0,
                          "cold delayed resolve cannot restore lease");
                    struct efs_opid_req q={0};memcpy(q.id.client_uuid,uuid,sizeof(uuid));q.id.session_epoch=1;q.id.seq=10;
                    efs_ino_t late=0;struct efs_meta_dentry dent;
                    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"after-fence",0,-1,&q,uuid,1,&late)==EFS_ERR_STALE&&!late,"cold delayed CREATE cannot publish an old owner");
                    CHECK(efs_meta_apply_lookup(kv,1,"after-fence",&dent)==EFS_ERR_NOT_FOUND,"cold fenced CREATE has no name");
                }
                _exit(failures ? 1 : 0); /* deliberately no LSM close/checkpoint */
            }
            int status = 0;
            CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "cold owner acknowledged phase");
            struct efs_kv *kv = efs_kv_lsm_open(dir, &cfg);
            CHECK(kv != NULL, "cold owner reopen");
            if (!kv)
                break;
            CHECK(efs_lease_any(kv, 4097, 1) == 0, "cold no early/retired ownership");
            if (phase < 2)
                CHECK(efs_session_fence_local(kv, 1, uuid, 2) == EFS_ERR_BUSY,
                      "cold owner barrier survived process exit");
            else
                CHECK(efs_session_accept(kv, 1, uuid, 1) == EFS_ERR_STALE,
                      "cold revocation survived process exit");
            efs_kv_lsm_close(kv);
        }
        DIR *owned = opendir(dir);
        CHECK(owned != NULL, "cold owner cleanup");
        if (owned) {
            struct dirent *de;
            while ((de = readdir(owned)))
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
                    char path[1024];
                    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
                    CHECK(!unlink(path), "cold owner remove file");
                }
            closedir(owned);
            CHECK(!rmdir(dir), "cold owner remove root");
        }
    }
}

static void test_pending_create_session_fence(void)
{
    struct efs_kv *kv=efs_kv_mem_create();CHECK(kv!=NULL,"pending admission KV");if(!kv)return;
    CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"pending admission init");
    uint8_t uuid[EFS_OPID_UUID_LEN];uuid_of(uuid,224);
    struct efs_opid_req q={0};memcpy(q.id.client_uuid,uuid,sizeof(uuid));q.id.session_epoch=1;q.id.seq=1;
    efs_ino_t ino=0;struct efs_meta_dentry dent;struct efs_opid_reply rep;
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"admission",0,-1,&q,uuid,1,&ino)==EFS_ERR_BUSY,"unestablished pending CREATE refuses ownership");
    CHECK(efs_meta_apply_lookup(kv,1,"admission",&dent)==EFS_ERR_NOT_FOUND,"unestablished CREATE has no namespace effect");
    CHECK(efs_meta_apply_opid_probe(kv,1,&q.id,&rep)==0,"unestablished CREATE records no verdict");
    CHECK(efs_session_create(kv,uuid,1)==EFS_OK&&efs_session_register(kv,uuid,1,1)==EFS_OK&&efs_session_establish(kv,1,uuid,1)==EFS_OK,"pending session admitted");
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"admission",0,-1,&q,uuid,1,&ino)==EFS_OK&&ino,"admitted pending CREATE");
    uint8_t replay_key[EFS_KV_KEY_MAX],replay_value[EFS_OPID_VAL_MAX];uint32_t replay_len=0,replay_bytes=sizeof(replay_value);
    CHECK(efs_kv_key_opid(1,uuid,1,replay_key,&replay_len)==EFS_OK&&efs_kv_get(kv,replay_key,replay_len,replay_value,&replay_bytes)==EFS_OK,"save pre-fence replay evidence");
    CHECK(efs_session_begin_fence(kv,uuid)==EFS_OK&&efs_session_fence_local(kv,1,uuid,2)==EFS_OK&&efs_session_ack_fence(kv,uuid,1)==EFS_OK&&efs_session_finish_fence(kv,uuid)==EFS_OK,"pending session durably revoked");
    CHECK(efs_lease_drop_session(kv,1,uuid,1)==EFS_OK&&!efs_lease_any(kv,ino,1),"revoked pending ownership removed");
    /* Model a crash after persisting the floor but before old-window deletion. */
    CHECK(efs_kv_put(kv,replay_key,replay_len,replay_value,replay_bytes)==EFS_OK,"retained old replay row behind durable floor");
    CHECK(efs_meta_apply_opid_probe(kv,1,&q.id,&rep)==EFS_ERR_STALE,"durable floor supersedes retained successful receipt");
    efs_ino_t late=0;
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"admission",0,-1,&q,uuid,1,&late)==EFS_ERR_STALE&&!late,"fenced replay cannot grant adoption");
    q.id.seq=2;q.ack=1;
    CHECK(efs_meta_apply_create_file_log_pending(kv,&g_at,1,S_IFREG|0600,"late",0,-1,&q,uuid,1,&late)==EFS_ERR_STALE&&!late,"delayed pending CREATE cannot cross local fence");
    CHECK(efs_meta_apply_lookup(kv,1,"late",&dent)==EFS_ERR_NOT_FOUND,"fenced CREATE leaves no name");
    CHECK(efs_meta_apply_opid_probe(kv,1,&q.id,&rep)==EFS_ERR_STALE,"fenced CREATE has no readable new verdict");
    CHECK(!efs_lease_any(kv,ino,1),"late retry cannot resurrect removed pending ownership");
    efs_kv_mem_free(kv);
}

static void test_bounded_session_cleanup(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16],other[16];
    struct efs_session_cleanup_key first[8],next[8],again[8],bad[8];uint32_t n=0,m=0;
    uuid_of(uuid,61);uuid_of(other,62);
    CHECK(kv!=NULL,"bounded cleanup KV");if(!kv)return;
    CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"bounded cleanup init");
    efs_ino_t ino=0;struct efs_meta_row row;
    CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"bounded",&ino)==EFS_OK,
          "bounded cleanup live inode");
    CHECK(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK,"bounded cleanup generation");
    uint32_t shard=efs_kv_inode_shard(ino);uint8_t key[EFS_KV_KEY_MAX],one=1;uint32_t len;
    for (uint32_t i=0;i<13;i++) {
        efs_ino_t file=ino+(uint64_t)i*4096;
        CHECK(efs_kv_key_lease(shard,file,row.generation,uuid,1,key,&len)==EFS_OK &&
              efs_kv_put(kv,key,len,&one,1)==EFS_OK,"seed old lease");
    }
    for (uint32_t i=0;i<8;i++) {
        CHECK(efs_kv_key_lock(shard,ino,row.generation,1,0,UINT64_MAX,1,i+1,uuid,1,key,&len)==EFS_OK &&
              efs_kv_put(kv,key,len,&one,1)==EFS_OK,"seed old lock");
    }
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,first,&n)==EFS_ERR_BUSY && !n,
          "missing floor is not empty cleanup");
    CHECK(efs_session_establish(kv,shard,uuid,1)==EFS_OK,"cleanup establish");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,first,&n)==EFS_ERR_BUSY,
          "active session cannot be cleaned");
    CHECK(efs_session_fence_local(kv,shard,uuid,2)==EFS_OK,"cleanup durable floor");
    CHECK(efs_kv_key_lease(shard,ino,row.generation,other,1,key,&len)==EFS_OK &&
          efs_kv_put(kv,key,len,&one,1)==EFS_OK,"seed foreign owner");
    CHECK(efs_kv_key_lease(shard,ino,row.generation,uuid,2,key,&len)==EFS_OK &&
          efs_kv_put(kv,key,len,&one,1)==EFS_OK,"seed newer epoch");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,first,&n)==EFS_OK && n==8,
          "select only bounded old-owner keys");
    memcpy(bad,first,sizeof(bad));bad[1]=bad[0];
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,bad,n)==EFS_ERR_INVAL,"duplicate rejects whole batch");
    memcpy(bad,first,sizeof(bad));bad[n-1].key[19]^=1;
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,bad,n)==EFS_ERR_INVAL,"foreign UUID rejects whole batch");
    memcpy(bad,first,sizeof(bad));bad[n-1].key[38]=2;
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,bad,n)==EFS_ERR_INVAL,"newer epoch rejects whole batch");
    struct efs_txid tx={{99}};struct efs_txn_parts parts={.n=1,.shard={shard}};
    CHECK(efs_txn_prepare_excl_value(kv,&tx,&parts,first[0].key,first[0].len,&one,1,
          EFS_TXN_PUT,&one,1)==EFS_OK,"prepared cleanup key fixture");
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,first,n)==EFS_ERR_BUSY,
          "prepared row refuses cleanup before any deletion");
    CHECK(efs_txn_resolve(kv,&tx,shard,EFS_TXN_ABORT)==EFS_OK,"later resolution releases cleanup");
    struct efs_kv_ops ops=*kv->ops;
    guarded_batch_original=ops.batch;ops.batch=guarded_batch_fault;kv->ops=&ops;
    guarded_batch_fail=1;
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,first,n)==EFS_ERR_IO,"atomic cleanup failure");
    guarded_batch_fail=0;
    uint8_t before_marker[EFS_KV_KEY_MAX],before_value[EFS_CLOSE_STEP_VALUE];uint32_t before_len,before_vlen=sizeof(before_value);
    CHECK(efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,before_marker,&before_len)==EFS_OK,"atomic marker key");
    before_marker[2]=EFS_KV_KIND_CLOSE_STEP;
    CHECK(efs_kv_get(kv,before_marker,before_len,before_value,&before_vlen)==EFS_ERR_NOT_FOUND,
          "failed cleanup cannot persist marker ahead of lease deletion");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,again,&m)==EFS_OK && m==n,
          "failed cleanup retains all old keys");
    for(uint32_t i=0;i<n;i++) CHECK(again[i].len==first[i].len &&
          !memcmp(again[i].key,first[i].key,first[i].len),"failure retains exact selected batch");
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,first,n)==EFS_OK,"apply exact cleanup batch");
    uint8_t marker[EFS_KV_KEY_MAX],value[EFS_CLOSE_STEP_VALUE];uint32_t ml,vl=sizeof(value);
    CHECK(efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,marker,&ml)==EFS_OK,"close obligation key");
    marker[2]=EFS_KV_KIND_CLOSE_STEP;
    CHECK(efs_kv_get(kv,marker,ml,value,&vl)==EFS_OK && vl==sizeof(value) && value[35]==255,
          "lease deletion atomically retains durable close obligation");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,next,&m)==EFS_OK && m==8,"next bounded phase");
    CHECK(efs_kv_del(kv,marker,ml)==EFS_OK,"retire owned first obligation fixture");
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,first,n)==EFS_OK,"exact replay safe");
    vl=sizeof(value);
    CHECK(efs_kv_get(kv,marker,ml,value,&vl)==EFS_ERR_NOT_FOUND,"exact lease cleanup replay cannot recreate retired obligation");
    uint32_t k=0;
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,again,&k)==EFS_OK && k==m,"replay keeps next batch");
    for(uint32_t i=0;i<m;i++) CHECK(again[i].len==next[i].len &&
          !memcmp(again[i].key,next[i].key,next[i].len),"replay cannot consume later keys");
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,next,m)==EFS_OK,"second mixed batch");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,next,&m)==EFS_OK && m==5,"final lock batch");
    CHECK(efs_session_cleanup_apply(kv,shard,uuid,1,next,m)==EFS_OK,"final exact batch");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,next,&m)==EFS_OK && !m,"lease lock phase empty");
    uint8_t got[8];uint32_t gl=sizeof(got);
    CHECK(efs_kv_key_lease(shard,ino,row.generation,other,1,key,&len)==EFS_OK &&
          efs_kv_get(kv,key,len,got,&gl)==EFS_OK,"foreign owner survives");
    gl=sizeof(got);
    CHECK(efs_kv_key_lease(shard,ino,row.generation,uuid,2,key,&len)==EFS_OK &&
          efs_kv_get(kv,key,len,got,&gl)==EFS_OK,"new epoch survives");
    /* A malformed matching row must not turn into a false empty phase. */
    CHECK(efs_kv_key_lease(shard,ino,row.generation,uuid,1,key,&len)==EFS_OK &&
          efs_kv_put(kv,key,len-1,&one,1)==EFS_OK,"malformed lease fixture");
    CHECK(efs_session_cleanup_select(kv,shard,uuid,1,next,&m)==EFS_ERR_PROTO && !m,
          "malformed cleanup fails closed");
    efs_kv_mem_free(kv);
}

static void test_deferred_close_identity(void)
{
    struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16];efs_ino_t ino=0;
    guarded_session_init(kv,uuid);
    CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"deferred",&ino)==EFS_OK,"deferred file");
    struct efs_opid_req q=guarded_request(uuid,1,0);
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"deferred open");
    uint8_t key[EFS_KV_KEY_MAX],before[EFS_CLOSE_STEP_VALUE],value[EFS_CLOSE_STEP_VALUE];uint32_t len,n;
    CHECK(efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),ino,key,&len)==EFS_OK,"close step key");
    key[2]=EFS_KV_KIND_CLOSE_STEP;
    q.id.seq=2;q.ack=1;
    const struct efs_kv_ops *saved=kv->ops;struct efs_kv_ops ops=*saved;
    guarded_batch_original=ops.batch;ops.batch=guarded_batch_fault;kv->ops=&ops;guarded_batch_fail=1;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_IO,"close obligation atomic failure");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_ERR_NOT_FOUND,"failed close no obligation");
    CHECK(efs_lease_any(kv,ino,1)==1,"failed close retains ownership");
    guarded_batch_fail=0;kv->ops=saved;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"atomic deferred close retry");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_OK && n==36,"identified close obligation");
    CHECK(!memcmp(value+8,uuid,16) && value[27]==1 && value[35]==2,"close identity exact UUID epoch sequence");
    memcpy(before,value,sizeof(value));
    int verdict=123;
    CHECK(efs_lease_guarded_probe_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0,&verdict)==1 && verdict==EFS_OK,"deferred lost reply probe");
    CHECK(efs_lease_guarded_probe(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0,&verdict)==EFS_ERR_PROTO,"legacy/deferred identities distinct");
    struct efs_txid tx={{82}};struct efs_txn_parts parts={.n=1};parts.shard[0]=efs_kv_anchor_shard(efs_kv_inode_shard(ino));
    CHECK(efs_txn_prepare_excl_value(kv,&tx,&parts,key,len,value,n,EFS_TXN_PUT,value,n)==EFS_OK,"prepare close obligation");
    q.id.seq=3;q.ack=1;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_BUSY,"reopen blocked by close ownership");
    CHECK(!efs_lease_any(kv,ino,1),"BUSY cannot acquire lease");
    CHECK(efs_lease_guarded_probe_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0,&verdict)==0,"BUSY does not retire identity");
    CHECK(efs_txn_resolve(kv,&tx,parts.shard[0],EFS_TXN_ABORT)==EFS_OK,"release close ownership");
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"same reopen identity retries");
    q.id.seq=2;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK && efs_lease_any(kv,ino,1)==1,"old close cannot remove reopened lease");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_OK && !memcmp(before,value,n),"old close does not recreate obligation");
    CHECK(efs_close_step_apply(kv,ino,before,EFS_CLOSE_STEP_RETIRE)==EFS_ERR_BUSY,"early retirement retains reopened lease");
    q.id.seq=4;q.ack=3;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"next close changes obligation");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_OK && value[35]==4,"later close has new exact identity");
    CHECK(efs_close_step_apply(kv,ino,before,EFS_CLOSE_STEP_RETIRE)==EFS_ERR_STALE,"obsolete cleanup cannot retire later close");
    uint8_t obligation[EFS_KV_KEY_MAX],one=1;uint32_t obligation_len;
    uint32_t inode_shard=efs_kv_inode_shard(ino);
    CHECK(efs_kv_key_lock(inode_shard,ino,1,1,0,UINT64_MAX,1,1,uuid,1,obligation,&obligation_len)==EFS_OK &&
          efs_kv_put(kv,obligation,obligation_len,&one,1)==EFS_OK,"early retirement lock fixture");
    CHECK(efs_close_step_apply(kv,ino,value,EFS_CLOSE_STEP_RETIRE)==EFS_ERR_BUSY,"early retirement retains outstanding locks");
    CHECK(efs_close_step_apply(kv,ino,value,EFS_CLOSE_STEP_LOCKS)==EFS_OK,"identified lock phase");
    CHECK(efs_kv_key_append_rsv(inode_shard,ino,1,0,obligation,&obligation_len)==EFS_OK &&
          efs_kv_put(kv,obligation,obligation_len,&one,1)==EFS_OK,"early retirement append fixture");
    CHECK(efs_close_step_apply(kv,ino,value,EFS_CLOSE_STEP_RETIRE)==EFS_ERR_BUSY,"early retirement retains outstanding append rows");
    CHECK(efs_kv_del(kv,obligation,obligation_len)==EFS_OK,"clear owned append fixture");
    CHECK(efs_kv_key_append_cur(inode_shard,ino,1,obligation,&obligation_len)==EFS_OK &&
          efs_kv_put(kv,obligation,obligation_len,&one,1)==EFS_OK,"early retirement cursor fixture");
    CHECK(efs_close_step_apply(kv,ino,value,EFS_CLOSE_STEP_RETIRE)==EFS_ERR_BUSY,"early retirement retains append cursor");
    CHECK(efs_kv_del(kv,obligation,obligation_len)==EFS_OK,"clear owned cursor fixture");
    CHECK(efs_close_step_apply(kv,ino,value,EFS_CLOSE_STEP_RETIRE)==EFS_OK,"empty append/lock state retires exact obligation");
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"lost close reply remains known after cleanup");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_ERR_NOT_FOUND,"known close replay cannot recreate retired obligation");
    q.id.seq=5;q.ack=3;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"reopen after cleanup");
    q.id.seq=6;q.ack=5;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"new close after cleanup");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_OK && value[35]==6,"new obligation survives old identity retirement");
    q.id.seq=2;q.ack=1;
    CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_STALE,"retired close cannot resurrect old obligation");
    n=sizeof(value);CHECK(efs_kv_get(kv,key,len,value,&n)==EFS_OK && value[35]==6,"retired retry preserves later close");
    efs_kv_mem_free(kv);
}

/* Cold replay across 8+1 ownership pages; retirement retains epoch fencing. */
static void test_guarded_retire_cold(void)
{
    char dir[]="/tmp/efs-retire-cold-XXXXXX";
    CHECK(mkdtemp(dir)!=NULL,"retire cold root");
    struct efs_kv_lsm_cfg cfg={0};
    uint8_t uuid[16];uuid_of(uuid,219);
    for(unsigned phase=0;phase<5;phase++) {
        pid_t pid=fork();CHECK(pid>=0,"retire cold fork");if(pid<0)break;
        if(!pid) {
            struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);
            if(!kv)_exit(2);
            if(!phase) {
                CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"retire cold init");
                CHECK(efs_session_create(kv,uuid,1)==EFS_OK &&
                      efs_session_register(kv,uuid,1,1)==EFS_OK &&
                      efs_session_establish(kv,1,uuid,1)==EFS_OK,"retire cold establish");
                for(unsigned i=0;i<9;i++) {
                    uint8_t key[EFS_KV_KEY_MAX],one=1;uint32_t len;
                    CHECK(efs_kv_key_lease(1,4097+(uint64_t)i*4096,1,uuid,1,key,&len)==EFS_OK &&
                          efs_kv_put(kv,key,len,&one,1)==EFS_OK,"retire cold old rows");
                }
                CHECK(efs_session_begin_fence(kv,uuid)==EFS_OK &&
                      efs_session_fence_local(kv,1,uuid,2)==EFS_OK &&
                      efs_session_ack_fence(kv,uuid,1)==EFS_OK &&
                      efs_session_finish_fence(kv,uuid)==EFS_OK,"retire cold barrier");
            } else if(phase<3) {
                CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_ERR_BUSY,"cold retirement refuses residual ownership");
                struct efs_session_cleanup_key keys[EFS_SESSION_CLEANUP_MAX];uint32_t n=0;
                CHECK(efs_session_cleanup_select(kv,1,uuid,1,keys,&n)==EFS_OK &&
                      n==(phase==1?8:1),"cold bounded exact ownership page");
                CHECK(efs_session_cleanup_apply(kv,1,uuid,1,keys,n)==EFS_OK,"cold apply ownership page");
            } else {
                CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_OK,"cold empty retirement replay");
                struct efs_opid_req q=guarded_request(uuid,1,0);
                CHECK(efs_lease_guarded_edge(kv,4097,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,
                      "cold retired epoch cannot resurrect old lease");
            }
            efs_kv_lsm_close(kv);_exit(failures?1:0);
        }
        int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) &&
                           !WEXITSTATUS(status),"retire cold child result");
    }
    DIR *d=opendir(dir);CHECK(d!=NULL,"retire cold cleanup");
    if(d) {
        struct dirent *e;while((e=readdir(d))) {
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
            char path[1024];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);
            CHECK(!unlink(path),"retire cold remove owned file");
        }
        closedir(d);CHECK(!rmdir(dir),"retire cold remove owned root");
    }
}

static void test_legacy_lease_prepared(void)
{
 struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16];efs_ino_t ino=0;
 CHECK(kv!=NULL,"legacy prepared KV");if(!kv)return;
 guarded_session_init(kv,uuid);
 CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"legacy-prepared",&ino)==EFS_OK,"legacy prepared file");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_OK,"legacy prepared initial owner");
 uint32_t shard=efs_kv_inode_shard(ino),kl;uint8_t key[EFS_KV_KEY_MAX],one=1;
 CHECK(efs_kv_key_lease(shard,ino,1,uuid,1,key,&kl)==EFS_OK,"legacy prepared key");
 struct efs_txid tx={{188}};struct efs_txn_parts parts={.n=1,.shard={shard}};
 CHECK(efs_txn_prepare_excl_value(kv,&tx,&parts,key,kl,&one,1,EFS_TXN_PUT,&one,1)==EFS_OK,"legacy lease prepared owner");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_ERR_BUSY,"legacy OPEN cannot bypass prepared lease");
 CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_ERR_BUSY && efs_lease_any(kv,ino,1)==1,"legacy CLOSE preserves prepared lease");
 struct efs_opid_req edge=guarded_request(uuid,1,0);
 CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&edge,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_BUSY,"guarded OPEN cannot bypass prepared pending lease");
 edge.id.seq=2;
 CHECK(efs_lease_guarded_edge_deferred(kv,ino,1,&edge,EFS_LEASE_EDGE_CLOSE,0)==EFS_ERR_BUSY,"guarded CLOSE cannot bypass prepared pending lease");
 int verdict=0;
 CHECK(efs_lease_guarded_probe_deferred(kv,ino,1,&edge,EFS_LEASE_EDGE_CLOSE,0,&verdict)==0,"blocked pending lease never records false CLOSE success");
 CHECK(efs_txn_resolve(kv,&tx,shard,EFS_TXN_ABORT)==EFS_OK,"legacy prepared lease abort");
 CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_OK,"legacy close after resolution");
 efs_kv_mem_free(kv);
}
static void test_legacy_rejection_floor(void)
{
 struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16]={173};efs_ino_t ino=0;
 CHECK(kv!=NULL,"legacy floor KV");if(!kv)return;
 CHECK(efs_meta_apply_init(kv,1)==EFS_OK,"legacy floor root");
 CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"legacy-floor",&ino)==EFS_OK,"legacy floor file");
 uint32_t shard=efs_kv_inode_shard(ino);
 CHECK(efs_session_accept_legacy(kv,shard,uuid,1)==EFS_OK,"legacy absent compatibility");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_OK,"legacy absent OPEN");
 CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_OK,"legacy old close");
 CHECK(efs_session_fence_local(kv,shard,uuid,2)==EFS_OK,"legacy durable reject floor");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_ERR_STALE && efs_lease_any(kv,ino,1)==0,"legacy old OPEN cannot resurrect revoked ownership");
 CHECK(efs_lease_open(kv,ino,1,uuid,2)==EFS_ERR_BUSY,"legacy new epoch needs establishment");
 uint8_t key[EFS_KV_KEY_MAX],bad=0;uint32_t kl;
 CHECK(efs_kv_key_sess_local(shard,uuid,key,&kl)==EFS_OK,"legacy floor key");
 CHECK(efs_kv_put(kv,key,kl,&bad,1)==EFS_OK,"legacy malformed floor");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_ERR_PROTO && efs_lease_any(kv,ino,1)==0,"legacy malformed authority fails closed");
 efs_kv_mem_free(kv);
}
static void test_legacy_sequenced_handoff(void)
{
 struct efs_kv *kv=efs_kv_mem_create();uint8_t uuid[16];uuid_of(uuid,237);
 CHECK(kv!=NULL,"handoff KV");if(!kv)return;
 CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"handoff init");
 efs_ino_t ino=0;CHECK(efs_meta_apply_create_file(kv,&g_at,1,S_IFREG|0600,"handoff",&ino)==EFS_OK,"handoff inode");
 uint32_t shard=efs_kv_inode_shard(ino);
 CHECK(efs_session_establish(kv,shard,uuid,1)==EFS_OK,"handoff establish");
 struct efs_opid_req q={0};memcpy(q.id.client_uuid,uuid,16);q.id.session_epoch=1;q.id.seq=1;
 CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"handoff sequenced OPEN");
 uint8_t key[EFS_KV_KEY_MAX],before[18],after[18];uint32_t kl=0,n=sizeof(before);
 CHECK(efs_kv_key_lease(shard,ino,1,uuid,1,key,&kl)==EFS_OK&&efs_kv_get(kv,key,kl,before,&n)==EFS_OK&&n==18,"handoff saved owner");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_OK,"legacy observes existing normal owner");
 CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_ERR_STALE,"legacy CLOSE cannot erase sequenced owner");
 CHECK(efs_lease_pending_close(kv,ino,1,uuid,1,1,1)==EFS_ERR_STALE,"legacy pending cleanup cannot erase sequenced owner");
 n=sizeof(after);CHECK(efs_kv_get(kv,key,kl,after,&n)==EFS_OK&&n==18&&!memcmp(before,after,n),"handoff sequence bytes preserved");
 q.id.seq=2;q.ack=1;
 CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK,"handoff exact CLOSE");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_ERR_STALE,"legacy OPEN cannot resurrect closed sequenced owner");
 CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_ERR_STALE&&!efs_lease_any(kv,ino,1),"legacy CLOSE cannot erase replay tombstone");
 uint8_t pending[EFS_LEASE_PENDING_VALUE_LEN];
 CHECK(efs_lease_pending_value(3,pending)==EFS_OK&&efs_kv_put(kv,key,kl,pending,sizeof(pending))==EFS_OK,"handoff pending fixture");
 q.id.seq=4;q.ack=2;
 CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"sequenced normal owner preserves pending token");
 n=sizeof(before);CHECK(efs_kv_get(kv,key,kl,before,&n)==EFS_OK&&n==18,"handoff saved pending owner");
 CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_OK,"legacy observes pending plus normal owner");
 CHECK(efs_lease_pending_close(kv,ino,1,uuid,1,3,0)==EFS_ERR_STALE,"legacy matching token cannot downgrade sequenced pending owner");
 n=sizeof(after);CHECK(efs_kv_get(kv,key,kl,after,&n)==EFS_OK&&n==18&&!memcmp(before,after,n),"handoff pending bytes preserved");
 q.id.seq=5;q.ack=4;
 CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_CLOSE|EFS_LEASE_EDGE_CLEAR_PENDING,3)==EFS_OK&&!efs_lease_any(kv,ino,1),"sequenced matching token clears both owners");
 /* Strict format checks fail closed, including empty and short values. */
 for(unsigned len=0;len<18;len++) {
   memset(after,0,sizeof(after));after[0]=3;after[1]=1;after[9]=1;
   CHECK(efs_kv_put(kv,key,kl,after,len)==EFS_OK,"handoff malformed fixture");
   CHECK(efs_lease_open(kv,ino,1,uuid,1)==EFS_ERR_PROTO,"handoff truncation refuses grant");
 }
 memset(after,0,sizeof(after));after[0]=3;after[1]=1;
 CHECK(efs_kv_put(kv,key,kl,after,18)==EFS_OK&&efs_lease_open(kv,ino,1,uuid,1)==EFS_ERR_PROTO,"handoff zero sequence rejected");
 efs_kv_mem_free(kv);
}
static void test_epoch_bound_finish(void)
{
    struct efs_kv *kv=efs_kv_mem_create();CHECK(kv,"epoch finish kv");
    uint8_t uuid[16]={0x71};struct efs_session_rec row;
    CHECK(efs_session_create(kv,uuid,1)==EFS_OK,"epoch finish session");
    CHECK(efs_session_begin_fence(kv,uuid)==EFS_OK,"epoch two begins");
    CHECK(efs_session_finish_fence_epoch(kv,uuid,3)==EFS_ERR_STALE,"future finish refused");
    CHECK(efs_session_finish_fence_epoch(kv,uuid,2)==EFS_OK,"exact epoch finishes");
    CHECK(efs_session_finish_fence_epoch(kv,uuid,2)==EFS_OK,"exact epoch retry idempotent");
    CHECK(efs_session_begin_fence(kv,uuid)==EFS_OK,"next epoch begins");
    CHECK(efs_session_finish_fence_epoch(kv,uuid,2)==EFS_ERR_STALE,"delayed finish cannot complete later barrier");
    CHECK(efs_session_get(kv,uuid,&row)==EFS_OK && row.epoch==3 && row.state==EFS_SESSION_FENCING,"later barrier remains fencing");
    CHECK(efs_session_finish_fence_epoch(kv,uuid,3)==EFS_OK,"later exact epoch completes");
    efs_kv_mem_free(kv);
}

int main(void)
{
    test_epoch_bound_finish();
    test_legacy_sequenced_handoff();
    test_legacy_rejection_floor();
    test_legacy_lease_prepared();
    test_guarded_retire_cold();
    test_deferred_close_identity();
    test_bounded_session_cleanup();
    test_pending_create_session_fence();
    test_prepared_owner_fence();
    test_prepared_owner_cold();
    test_pending_adoption_permutations();
    test_guarded_edge_permutations();
    test_guarded_malformed_ownership();
    test_guarded_lease_edges();
    test_guarded_pending_and_fence();
    test_guarded_retire_missing_authority();
    test_guarded_window_bound();
    test_negative_create_cold();
    test_pending_cold_edges();
    test_pending_create_ownership();
    test_register_and_fence();
    test_registered_before_establish_fence();
    test_register_establish();
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
