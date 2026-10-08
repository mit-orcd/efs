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
                if (guarded && rc==EFS_OK) rc=efs_session_create(kv,uuid,1);
                if (guarded && rc==EFS_OK) rc=efs_session_register(kv,uuid,1,1);
                if (guarded && rc==EFS_OK) rc=efs_session_establish(kv,1,uuid,1);
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
    CHECK(efs_lease_close(kv,ino,1,uuid,1)==EFS_ERR_PROTO && efs_lease_any(kv,ino,1)==1,"legacy edges refuse guarded records");
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
    CHECK(efs_lease_guarded_retire(kv,1,uuid,1)==EFS_OK && guarded_rows(kv)==0,"barrier retires old ownership and replay floor");
    q.id.seq=6;q.ack=5;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_STALE,"retired old session cannot reopen after floor deletion");
    CHECK(efs_session_register(kv,uuid,2,1)==EFS_OK && efs_session_establish(kv,1,uuid,2)==EFS_OK,"new epoch establish");
    q=guarded_request(uuid,1,0);q.id.session_epoch=2;
    CHECK(efs_lease_guarded_edge(kv,ino,1,&q,EFS_LEASE_EDGE_OPEN,0)==EFS_OK,"new epoch has independent sequence identity");
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
        struct efs_txn_parts parts = {.n = 1, .shard = {7}};
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
                    CHECK(efs_session_create(kv, uuid, 1) == EFS_OK, "cold owner create");
                    CHECK(efs_session_register(kv, uuid, 1, 7) == EFS_OK, "cold owner register");
                    CHECK(efs_session_establish(kv, 7, uuid, 1) == EFS_OK, "cold owner establish");
                    CHECK(efs_session_prepare_owner(kv, 7, uuid, 1, &tx, &parts) == EFS_OK,
                          "cold prepare owner");
                    uint8_t key[EFS_KV_KEY_MAX], value[EFS_LEASE_PENDING_VALUE_LEN];
                    uint32_t len = 0;
                    CHECK(efs_kv_key_lease(7, 7, 1, uuid, 1, key, &len) == EFS_OK,
                          "cold lease key");
                    CHECK(efs_lease_pending_value(9, value) == EFS_OK, "cold lease value");
                    CHECK(efs_txn_prepare_excl_value(kv, &tx, &parts, key, len, NULL,
                                                     EFS_TXN_ABSENT, EFS_TXN_PUT, value,
                                                     sizeof(value)) == EFS_OK,
                          "cold pending lease intent");
                } else if (phase == 1) {
                    CHECK(efs_session_begin_fence(kv, uuid) == EFS_OK, "cold begin fence");
                    CHECK(efs_session_fence_local(kv, 7, uuid, 2) == EFS_ERR_BUSY,
                          "cold prepared guard blocks fence");
                } else if (phase == 2) {
                    CHECK(efs_txn_resolve(kv, &tx, 7, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) ==
                              EFS_OK,
                          "cold resolve owner");
                    CHECK(efs_lease_any(kv, 7, 1) == (int)commit, "cold resolved lease exact");
                    CHECK(efs_session_fence_local(kv, 7, uuid, 2) == EFS_OK,
                          "cold floor after resolve");
                    CHECK(efs_session_ack_fence(kv, uuid, 7) == EFS_OK, "cold ACK");
                    CHECK(efs_session_finish_fence(kv, uuid) == EFS_OK, "cold finish");
                    CHECK(efs_lease_drop_session(kv, 7, uuid, 1) == EFS_OK, "cold drop");
                } else {
                    CHECK(efs_txn_resolve(kv, &tx, 7, commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT) ==
                              EFS_OK,
                          "cold delayed resolve");
                    CHECK(efs_session_accept(kv, 7, uuid, 1) == EFS_ERR_STALE,
                          "cold floor retained");
                    CHECK(efs_lease_any(kv, 7, 1) == 0,
                          "cold delayed resolve cannot restore lease");
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
            CHECK(efs_lease_any(kv, 7, 1) == 0, "cold no early/retired ownership");
            if (phase < 2)
                CHECK(efs_session_fence_local(kv, 7, uuid, 2) == EFS_ERR_BUSY,
                      "cold owner barrier survived process exit");
            else
                CHECK(efs_session_accept(kv, 7, uuid, 1) == EFS_ERR_STALE,
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

int main(void)
{
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
