#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/put_ticket.h"
#include "efs/session.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static struct efs_meta_pub make_pub(efs_ino_t ino, uint64_t gen, uint64_t seq)
{
    struct efs_meta_pub p = {0};
    p.ino = ino;
    p.inode_gen = gen;
    p.new_size = 64;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.fresh_object = 1;
    p.durable_result = 1;
    p.ticketed = 1;
    p.put_fragment_len = 32;
    p.put_member_mask = 15;
    p.put_id.client_uuid[0] = 93;
    p.put_id.session_epoch = 1;
    p.put_id.seq = seq;
    p.publication_id = p.put_id;
    p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, 1, seq, 0, 0);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = i + 1;
        memset(p.ch.checksums[i], i + 1, EFS_HASH_SIZE);
    }
    return p;
}
static struct efs_meta_pub setup(struct efs_kv *kv)
{
    struct efs_meta_attrs a = {0, 0, 1};
    efs_ino_t ino;
    struct efs_meta_row row;
    assert(efs_meta_apply_init(kv, 1) == EFS_OK);
    assert(efs_meta_apply_create_file(kv, &a, EFS_ROOT_INO, S_IFREG | 0600, "tickets",
                                      &ino) == EFS_OK);
    assert(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK);
    struct efs_meta_pub p = make_pub(ino, row.generation, 1);
    uint32_t sh = efs_kv_lane_shard(ino, 0);
    assert(efs_session_create(kv, p.put_id.client_uuid, 1) == EFS_OK);
    assert(efs_session_register(kv, p.put_id.client_uuid, 1, sh) == EFS_OK);
    assert(efs_session_establish(kv, sh, p.put_id.client_uuid, 1) == EFS_OK);
    return p;
}
static void revoke_session(struct efs_kv *kv, const struct efs_meta_pub *p)
{
    const uint8_t *u = p->put_id.client_uuid;
    assert(efs_session_begin_fence(kv, u) == EFS_OK);
    assert(efs_session_fence_local(kv, efs_kv_lane_shard(p->ino, 0), u, 2) == EFS_OK);
}
static void finish(struct efs_kv *kv, const struct efs_meta_pub *p)
{
    const uint8_t *u = p->put_id.client_uuid;
    assert(efs_session_ack_fence(kv, u, efs_kv_lane_shard(p->ino, 0)) == EFS_OK);
    assert(efs_session_finish_fence(kv, u) == EFS_OK);
}
static void metadata(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv);
    struct efs_meta_pub p = setup(kv);
    struct efs_put_ticket t;
    uint32_t state;
    assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
    /* Publication without admission cannot acquire ownership. */
    assert(efs_meta_apply_publish(kv, &p) == EFS_ERR_NOT_FOUND);
    struct efs_meta_chunk ch;
    assert(efs_meta_apply_get_chunk(kv, p.ino, 0, &ch) == EFS_ERR_NOT_FOUND);
    assert(efs_put_ticket_admit(kv, &t) == EFS_OK);
    assert(efs_put_ticket_admit(kv, &t) == EFS_OK);
    struct efs_put_ticket changed = t;
    changed.member_mask = 3;
    assert(efs_put_ticket_admit(kv, &changed) == EFS_ERR_INVAL);
    changed = t;
    changed.member_mask = 31;
    assert(efs_put_ticket_admit(kv, &changed) == EFS_ERR_INVAL);
    changed = t;
    changed.delta_off = 1;
    assert(efs_put_ticket_admit(kv, &changed) == EFS_ERR_INVAL);
    struct efs_meta_pub foreign = p;
    foreign.publication_id.client_uuid[0]++;
    assert(efs_put_ticket_from_pub(&foreign, &changed) == EFS_ERR_INVAL);
    changed = t;
    changed.body_digest[0] ^= 1;
    assert(efs_put_ticket_admit(kv, &changed) == EFS_ERR_PROTO);
    changed = t;
    changed.object++;
    assert(efs_put_ticket_admit(kv, &changed) == EFS_ERR_INVAL);
    changed = t;
    changed.ino += 4096;
    assert(efs_put_ticket_admit(kv, &changed) == EFS_ERR_PROTO);
    assert(efs_put_ticket_retire(kv, &t) == EFS_ERR_BUSY);
    assert(efs_put_ticket_reclaim_begin(kv, &t) == EFS_ERR_BUSY);
    /* Faulting the atomic batch cannot publish a mapping without ownership. */
    assert(efs_kv_mem_fail_next_batch(kv) == EFS_OK);
    assert(efs_meta_apply_publish(kv, &p) == EFS_ERR_IO);
    assert(efs_meta_apply_get_chunk(kv, p.ino, 0, &ch) == EFS_ERR_NOT_FOUND);
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_OK &&
           state == EFS_PUT_TICKET_ADMITTED);
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_OK &&
           state == EFS_PUT_TICKET_PUBLISHED);
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
    /* An uncommitted second body stays owned but unpublishable across revoke. */
    struct efs_meta_pub q = make_pub(p.ino, p.inode_gen, 2);
    struct efs_put_ticket orphan;
    assert(efs_put_ticket_from_pub(&q, &orphan) == EFS_OK);
    assert(efs_put_ticket_admit(kv, &orphan) == EFS_OK);
    /* A new publication identity can rebase the same immutable body. */
    q.expected_gen = p.candidate_gen;
    q.publication_id.seq = 100;
    struct efs_put_ticket rebased;
    assert(efs_put_ticket_from_pub(&q, &rebased) == EFS_OK);
    assert(!memcmp(rebased.body_digest, orphan.body_digest, EFS_HASH_SIZE));
    revoke_session(kv, &q);
    assert(efs_put_ticket_reclaim_begin(kv, &orphan) == EFS_ERR_BUSY);
    assert(efs_meta_apply_publish(kv, &q) == EFS_ERR_STALE);
    finish(kv, &q);
    assert(efs_put_ticket_reclaim_begin(kv, &t) ==
           EFS_ERR_BUSY); /* live mapping protected */
    assert(efs_put_ticket_reclaim_begin(kv, &orphan) == EFS_OK);
    assert(efs_put_ticket_reclaim_begin(kv, &orphan) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &orphan) ==
           EFS_ERR_BUSY); /* deletion ACKs missing */
    assert(efs_put_ticket_delete_ack(kv, &orphan, 3) == EFS_ERR_INVAL);
    for (unsigned bit = 1; bit <= 4; bit <<= 1)
        assert(efs_put_ticket_delete_ack(kv, &orphan, bit) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &orphan) ==
           EFS_ERR_BUSY); /* fourth member still offline */
    assert(efs_put_ticket_delete_ack(kv, &orphan, 8) == EFS_OK);
    assert(efs_put_ticket_delete_ack(kv, &orphan, 8) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &orphan) ==
           EFS_ERR_BUSY); /* first ticket still outstanding */
    assert(efs_put_ticket_delete_ack(kv, &t, 1) == EFS_ERR_BUSY);
    assert(efs_kv_mem_fail_next_batch(kv) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &t) == EFS_ERR_IO);
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_OK &&
           state == EFS_PUT_TICKET_PUBLISHED);
    assert(efs_put_ticket_retire(kv, &t) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &orphan) == EFS_OK);
    assert(efs_put_ticket_get(kv, &orphan, &state) == EFS_ERR_STALE);
    assert(efs_put_ticket_admit(kv, &orphan) == EFS_ERR_STALE);
    assert(efs_put_ticket_retire(kv, &orphan) == EFS_OK);
    assert(efs_meta_apply_get_chunk(kv, p.ino, 0, &ch) == EFS_OK &&
           ch.generation == p.candidate_gen);
    efs_kv_mem_free(kv);
}
static void capacity(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv);
    struct efs_meta_pub p = setup(kv);
    struct efs_put_ticket t;
    for (unsigned i = 1; i <= EFS_PUT_TICKET_MAX; i++) {
        struct efs_meta_pub q = make_pub(p.ino, p.inode_gen, i);
        assert(efs_put_ticket_from_pub(&q, &t) == EFS_OK);
        assert(efs_put_ticket_admit(kv, &t) == EFS_OK);
    }
    struct efs_meta_pub q = make_pub(p.ino, p.inode_gen, EFS_PUT_TICKET_MAX + 1);
    assert(efs_put_ticket_from_pub(&q, &t) == EFS_OK);
    assert(efs_put_ticket_admit(kv, &t) == EFS_ERR_BUSY);
    struct efs_put_ticket first;
    assert(efs_put_ticket_from_pub(&p, &first) == EFS_OK);
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &first) == EFS_OK);
    assert(efs_put_ticket_admit(kv, &t) == EFS_OK); /* bounded window makes progress */
    efs_kv_mem_free(kv);
}
static void restart(void)
{
    char dir[] = "/tmp/efs-put-ticket-XXXXXX";
    assert(mkdtemp(dir));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        struct efs_kv *kv = efs_kv_lsm_open(dir, NULL);
        assert(kv);
        struct efs_meta_pub p = setup(kv);
        struct efs_put_ticket t;
        assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
        assert(efs_put_ticket_admit(kv, &t) == EFS_OK);
        revoke_session(kv, &p);
        _exit(0); /* crash before global barrier completes */
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st));
    struct efs_kv *kv = efs_kv_lsm_open(dir, NULL);
    assert(kv);
    struct efs_meta_dentry d;
    struct efs_meta_row row;
    assert(efs_meta_apply_lookup(kv, EFS_ROOT_INO, "tickets", &d) == EFS_OK);
    assert(efs_meta_apply_get_inode(kv, d.ino, &row) == EFS_OK);
    struct efs_meta_pub p = make_pub(d.ino, row.generation, 1);
    struct efs_put_ticket t;
    assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
    assert(efs_put_ticket_reclaim_begin(kv, &t) == EFS_ERR_BUSY);
    finish(kv, &p);
    assert(efs_put_ticket_reclaim_begin(kv, &t) == EFS_OK);
    efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(dir, NULL);
    assert(kv);
    uint32_t state;
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_OK &&
           state == EFS_PUT_TICKET_RECLAIMING);
    assert(efs_put_ticket_retire(kv, &t) == EFS_ERR_BUSY);
    assert(efs_put_ticket_delete_ack(kv, &t, 1) == EFS_OK);
    efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(dir, NULL);
    assert(kv);
    assert(efs_put_ticket_retire(kv, &t) == EFS_ERR_BUSY);
    for (unsigned bit = 2; bit <= 8; bit <<= 1)
        assert(efs_put_ticket_delete_ack(kv, &t, bit) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &t) == EFS_OK);
    efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(dir, NULL);
    assert(kv);
    assert(efs_put_ticket_admit(kv, &t) == EFS_ERR_STALE);
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_ERR_STALE);
    efs_kv_lsm_close(kv);
    DIR *dp = opendir(dir);
    assert(dp);
    struct dirent *e;
    char path[1024];
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        assert(!unlink(path));
    }
    closedir(dp);
    assert(!rmdir(dir));
}
int main(void)
{
    metadata();
    capacity();
    restart();
    puts("PUT tickets: admission, atomic ownership, revocation, bounded "
         "retirement and crash replay PASS");
    return 0;
}
