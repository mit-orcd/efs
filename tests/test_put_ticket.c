#include "efs/kv_key.h"
#include "efs/protocol.h"
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
#include "test_tmpdir.h"

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
static void codec(void)
{
    struct efs_meta_pub p=make_pub(4097,8,0x0102030405060708ull);
    p.chunk_index=17;p.candidate_gen=efs_meta_candidate_gen(p.put_id.client_uuid,1,p.put_id.seq,17,0);
    struct efs_put_ticket t,decoded={0};assert(!efs_put_ticket_from_pub(&p,&t));
    struct {uint8_t before,wire[EFS_PUT_TICKET_WIRE_LEN],after;} bytes={.before=29,.after=71};
    assert(!efs_put_ticket_encode(&t,bytes.wire));assert(bytes.before==29 && bytes.after==71);
    assert(bytes.wire[20]==1 && bytes.wire[27]==8 && bytes.wire[44]==0 && bytes.wire[47]==17);
    assert(!efs_put_ticket_decode(bytes.wire,sizeof(bytes.wire),&decoded));
    uint8_t again[EFS_PUT_TICKET_WIRE_LEN];assert(!efs_put_ticket_encode(&decoded,again));
    assert(!memcmp(again,bytes.wire,sizeof(again)));
    decoded.ino=123;
    assert(efs_put_ticket_decode(bytes.wire,sizeof(bytes.wire)-1,&decoded)==EFS_ERR_INVAL && decoded.ino==123);
    assert(efs_put_ticket_decode(bytes.wire,sizeof(bytes.wire)+1,&decoded)==EFS_ERR_INVAL && decoded.ino==123);
    bytes.wire[48]^=1;
    assert(efs_put_ticket_decode(bytes.wire,sizeof(bytes.wire),&decoded)==EFS_ERR_INVAL && decoded.ino==123);
    bytes.wire[48]^=1;bytes.wire[212]=0xff;
    assert(efs_put_ticket_decode(bytes.wire,sizeof(bytes.wire),&decoded)==EFS_ERR_INVAL && decoded.ino==123);
    assert(efs_put_ticket_encode(NULL,again)==EFS_ERR_INVAL);
    puts("PUT ticket codec: fixed endian, exact length, malformed geometry/object/members and output canaries PASS");
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
    /* K28: a member bit beyond the cluster cap is refused. */
    changed.member_mask = t.member_mask | (1u << EFS_MAX_NODES);
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
    assert(efs_meta_apply_publish(kv, &q) == EFS_ERR_FENCED);
    finish(kv, &q);
    assert(efs_put_ticket_reclaim_begin(kv, &t) ==
           EFS_ERR_BUSY); /* live mapping protected */
    assert(efs_put_ticket_reclaim_begin(kv, &orphan) == EFS_OK);
    assert(efs_put_ticket_reclaim_begin(kv, &orphan) == EFS_OK);
    assert(efs_put_ticket_retire(kv, &orphan) ==
           EFS_ERR_BUSY); /* deletion ACKs missing */
    /* After revocation the owner cannot settle; only the collector acts. */
    assert(efs_put_ticket_settle(kv,&orphan)==EFS_ERR_FENCED);
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
    assert(efs_put_ticket_retire(kv,&t)==EFS_OK);
    int retired=0;assert(!efs_put_ticket_retired(kv,&t,&retired) && retired);
    assert(!efs_put_ticket_retire(kv,&t));
    assert(efs_put_ticket_retire(kv, &orphan) == EFS_OK);
    assert(efs_put_ticket_get(kv, &orphan, &state) == EFS_ERR_STALE);
    assert(efs_put_ticket_admit(kv, &orphan) == EFS_ERR_FENCED);
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
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-put-ticket-XXXXXX");
    assert(efs_test_mkdtemp(dir));
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
    assert(efs_put_ticket_admit(kv, &t) == EFS_ERR_FENCED);
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

/* K13 REPORT path: a non-durable ticketed publication. */
static struct efs_meta_pub report_pub(efs_ino_t ino, uint64_t gen, uint64_t seq,
                                      uint32_t off, uint32_t len, uint64_t expected)
{
    struct efs_meta_pub p = make_pub(ino, gen, seq);
    p.durable_result = 0;
    p.delta_off = off;
    p.delta_len = len;
    p.expected_gen = expected;
    return p;
}
static void report_admit(struct efs_kv *kv, const struct efs_meta_pub *p, struct efs_put_ticket *t)
{
    assert(efs_put_ticket_from_pub(p, t) == EFS_OK);
    assert(efs_put_ticket_admit(kv, t) == EFS_OK);
}
static unsigned live_spans_of(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t nd = 0;
    uint64_t newest = 0;
    unsigned n = 0;
    assert(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX, &nd, &newest) == EFS_OK);
    for (uint32_t i = 0; i < nd; i++)
        if (ds[i].generation == gen && ds[i].len) n++;
    return n;
}
static void report_path(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv);
    struct efs_meta_pub base = setup(kv);
    efs_ino_t ino = base.ino;
    uint64_t gen = base.inode_gen;
    struct efs_meta_chunk ch;
    struct efs_put_ticket t, u;
    uint32_t state;
    /* Never admitted: refused, no mapping, no ticket created. */
    struct efs_meta_pub p = report_pub(ino, gen, 1, 0, 0, 0);
    assert(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE);
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_ERR_NOT_FOUND);
    assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_ERR_NOT_FOUND);
    /* Admitted: publish binds PUBLISHED atomically; exact replay is a no-op. */
    report_admit(kv, &p, &t);
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
    assert(efs_put_ticket_get(kv, &t, &state) == EFS_OK && state == EFS_PUT_TICKET_PUBLISHED);
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_OK && ch.generation == p.candidate_gen);
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
    /* A lost CAS keeps the ticket ADMITTED; the same object retries on a fresh base. */
    struct efs_meta_pub q = report_pub(ino, gen, 2, 0, 0, 12345);
    report_admit(kv, &q, &u);
    assert(efs_meta_apply_publish(kv, &q) == EFS_ERR_STALE);
    assert(efs_put_ticket_get(kv, &u, &state) == EFS_OK && state == EFS_PUT_TICKET_ADMITTED);
    q.expected_gen = p.candidate_gen;
    struct efs_put_ticket again;
    assert(efs_put_ticket_from_pub(&q, &again) == EFS_OK && !memcmp(&again, &u, sizeof(u)));
    assert(efs_meta_apply_publish(kv, &q) == EFS_OK);
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_OK && ch.generation == q.candidate_gen);
    /* A different body under the same ticket identity is refused. */
    struct efs_meta_pub forged = q;
    forged.ch.checksums[0][0] ^= 1;
    assert(efs_meta_apply_publish(kv, &forged) == EFS_ERR_PROTO);
    /* Settle in stream order: the second cannot retire before the first. */
    assert(efs_put_ticket_settle(kv, &u) == EFS_ERR_BUSY);
    assert(efs_put_ticket_get(kv, &u, &state) == EFS_OK && state == EFS_PUT_TICKET_PUBLISHED);
    assert(efs_put_ticket_settle(kv, &t) == EFS_OK);
    assert(efs_put_ticket_settle(kv, &u) == EFS_OK);
    assert(efs_put_ticket_settle(kv, &u) == EFS_OK); /* idempotent once retired */
    assert(efs_put_ticket_orphan(kv, &t) == EFS_ERR_NOT_FOUND); /* published: no orphan */
    /* A retired ticket never takes effect again, even with a matching CAS. */
    p.expected_gen = q.candidate_gen;
    assert(efs_meta_apply_publish(kv, &p) == EFS_ERR_STALE);
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_OK && ch.generation == q.candidate_gen);
    assert(efs_put_ticket_admit(kv, &t) == EFS_ERR_STALE);
    /* Abandon: the owner settles an unpublished ticket; it can never publish
     * and its body is recorded for revocation-gated collection. */
    struct efs_meta_pub r = report_pub(ino, gen, 3, 0, 0, q.candidate_gen);
    struct efs_put_ticket rt;
    report_admit(kv, &r, &rt);
    assert(efs_put_ticket_settle(kv, &rt) == EFS_OK);
    assert(efs_put_ticket_orphan(kv, &rt) == EFS_OK);
    assert(efs_meta_apply_publish(kv, &r) == EFS_ERR_STALE);
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_OK && ch.generation == q.candidate_gen);
    /* A lost admission: settle records it REJECTED, a late admit is refused. */
    struct efs_meta_pub lost = report_pub(ino, gen, 4, 0, 0, q.candidate_gen);
    struct efs_meta_pub later = report_pub(ino, gen, 5, 0, 0, q.candidate_gen);
    struct efs_put_ticket lt, lat;
    assert(efs_put_ticket_from_pub(&lost, &lt) == EFS_OK);
    report_admit(kv, &later, &lat);
    assert(efs_put_ticket_settle(kv, &lt) == EFS_OK);
    assert(efs_put_ticket_admit(kv, &lt) == EFS_ERR_STALE);
    assert(efs_put_ticket_orphan(kv, &lt) == EFS_OK);
    /* Deleted file at pack time: GONE settles the ticket as REJECTED. */
    assert(efs_meta_apply_publish_reject(kv, &later, EFS_OK) == EFS_OK);
    assert(efs_put_ticket_get(kv, &lat, &state) == EFS_OK && state == EFS_PUT_TICKET_REJECTED);
    assert(efs_meta_apply_publish(kv, &later) == EFS_ERR_STALE);
    assert(efs_put_ticket_settle(kv, &lat) == EFS_OK && efs_put_ticket_orphan(kv, &lat) == EFS_OK);
    /* Revocation: the fenced owner can neither publish nor settle. */
    struct efs_meta_pub z = report_pub(ino, gen, 6, 0, 0, q.candidate_gen);
    struct efs_put_ticket zt;
    report_admit(kv, &z, &zt);
    revoke_session(kv, &z);
    assert(efs_meta_apply_publish(kv, &z) == EFS_ERR_FENCED);
    assert(efs_put_ticket_settle(kv, &zt) == EFS_ERR_FENCED);
    assert(efs_put_ticket_get(kv, &zt, &state) == EFS_OK && state == EFS_PUT_TICKET_ADMITTED);
    efs_kv_mem_free(kv);
    puts("K13 REPORT tickets: unadmitted refusal, atomic PUBLISHED, exact replay no-op, lost-CAS retry, body collision, ordered settle, retired/abandoned/lost-admission/GONE refusal and fenced owner PASS");
}

/* W88 (round 6 fixture, ticketed): eight spans, a fold, a later full image
 * and a churn span evict the first span's tombstone from the trailer. Its
 * byte-identical retry must not reappear. legacy=1 replays the same history
 * on the unticketed library path, the negative control: it must resurrect. */
static unsigned w88_replay(int legacy)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv);
    struct efs_meta_pub base = setup(kv);
    efs_ino_t ino = base.ino;
    uint64_t gen = base.inode_gen, seq = 0;
    struct efs_put_ticket t;
    struct efs_meta_pub first = {0}, p;
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t nd = 0;
    uint64_t newest = 0;
#define W88_PUB(pp) do { \
        if (legacy) { (pp).ticketed = 0; (pp).put_fragment_len = 0; (pp).put_member_mask = 0; } \
        else report_admit(kv, &(pp), &t); \
        assert(efs_meta_apply_publish(kv, &(pp)) == EFS_OK); \
    } while (0)
    for (unsigned i = 0; i < EFS_CHUNK_DELTA_MAX; i++) {
        p = report_pub(ino, gen, ++seq, i, 1, 0);
        W88_PUB(p);
        if (!i) first = p;
    }
    assert(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX, &nd, &newest) == EFS_OK);
    p = report_pub(ino, gen, ++seq, 0, 0, 0);
    p.delta_base_n = nd; p.delta_base_seq = newest;
    W88_PUB(p);
    uint64_t fold = p.candidate_gen;
    assert(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX, &nd, &newest) == EFS_OK);
    p = report_pub(ino, gen, ++seq, 0, 0, fold);
    p.delta_base_n = nd; p.delta_base_seq = newest; p.ch.checksums[0][0] = 0x77;
    W88_PUB(p);
    uint64_t later = p.candidate_gen;
    p = report_pub(ino, gen, ++seq, 40, 1, 0);
    W88_PUB(p);
    /* Lost original ACK: the owner retries the first span byte for byte. */
    int rc = efs_meta_apply_publish(kv, &first);
    struct efs_meta_chunk ch;
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_OK && ch.generation == later);
    unsigned back = live_spans_of(kv, ino, first.candidate_gen);
    if (!legacy) {
        assert(rc == EFS_OK && !back); /* PUBLISHED: exact no-op */
        /* After the owner retired it, the retry is refused outright. */
        assert(efs_put_ticket_from_pub(&first, &t) == EFS_OK);
        assert(efs_put_ticket_settle(kv, &t) == EFS_OK);
        assert(efs_meta_apply_publish(kv, &first) == EFS_ERR_STALE);
        assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_OK && ch.generation == later);
        back = live_spans_of(kv, ino, first.candidate_gen);
        assert(!back);
    }
#undef W88_PUB
    efs_kv_mem_free(kv);
    return back;
}

int main(void)
{
    codec();
    metadata();
    capacity();
    restart();
    report_path();
    assert(w88_replay(0) == 0);
    /* Negative control: the same history without tickets resurrects. */
    assert(w88_replay(1) > 0);
    puts("W88 ticketed folded-span retry after trailer eviction: no resurrection (PUBLISHED no-op, retired STALE); unticketed control resurrects PASS");
    puts("PUT tickets: admission, atomic ownership, revocation, bounded "
         "retirement and crash replay PASS");
    return 0;
}
