/* I23 revocation barrier coordinator; see include/efs/session_revoke.h. */
#include "efs/session_revoke.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/network.h"
#include "efs/raft.h"
#include "efs/session.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void wr32be_sess(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (24 - 8 * i));
}


static int sess_rpc(const struct efs_session_rpc *rpc, const uint8_t *payload, uint32_t plen,
                    struct efs_msg_raft_mkfs_reply *out)
{
    return rpc->submit(rpc->ctx, payload, plen, out);
}

/* Repeating this exact monotonic certificate barrier cannot reopen a session.
 * A cold peer pool can temporarily reject a configured storage member; retain
 * the UUID/epoch and cap the entire retry sequence, including socket waits. */
int efs_session_storage_barrier(const struct efs_session_rpc *rpc,
                                const uint8_t *payload, uint32_t plen,
                                struct efs_msg_raft_mkfs_reply *out)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)) return EFS_ERR_IO;
    uint64_t saved=efs_net_deadline_ms();
    uint64_t deadline=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000+15000;
    if(saved && saved<deadline) deadline=saved;
    efs_net_set_deadline_ms(deadline);
    int rc;
    do {
        rc=sess_rpc(rpc,payload,plen,out);
        if(rc!=EFS_ERR_BUSY) break;
        if(efs_net_remaining_ms(0x7fffffff)<=0) break;
        usleep(100000);
    } while(efs_net_remaining_ms(0x7fffffff)>0);
    efs_net_set_deadline_ms(saved);
    return rc;
}

static void sess_pack_hdr(uint8_t *payload, uint8_t group, uint8_t sub,
                          const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    payload[0] = group;
    payload[1] = EFS_MD_CMD_SESSION;
    payload[2] = sub;
    memcpy(payload + 3, uuid, EFS_OPID_UUID_LEN);
}

/* One RPC proposes at most eight exact lease/lock deletions. Stop on an
 * ambiguous transport result: reselection is allowed only after a definite
 * successful reply, and the durable epoch floor preserves replay safety. */
int efs_session_cleanup_bounded(const struct efs_session_rpc *rpc,
                                const uint8_t uuid[EFS_OPID_UUID_LEN],
                                uint32_t old_epoch, uint32_t shard, int appends,
                                struct efs_msg_raft_mkfs_reply *reply)
{
    uint8_t payload[27] = {0};
    uint32_t length=sizeof(payload);
    if(appends) {
        payload[0]=efs_raft_shard_group(shard);payload[1]=EFS_MD_REQ_SESSION_APPEND_NEXT;
        memcpy(payload+2,uuid,16);wr32be_sess(payload+18,old_epoch);
        wr32be_sess(payload+22,shard);length=26;
    } else {
        sess_pack_hdr(payload, efs_raft_shard_group(shard), EFS_MD_SESS_LEASE_LOCK_NEXT, uuid);
        wr32be_sess(payload + 19, old_epoch);wr32be_sess(payload + 23, shard);
    }
    /* An invocation has a finite work budget; unfinished ownership remains
     * durable and must not be reported as a completed cleanup. */
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return EFS_ERR_IO;
    uint64_t saved = efs_net_deadline_ms();
    uint64_t deadline = (uint64_t)start.tv_sec * 1000 + start.tv_nsec / 1000000 + 30000;
    if (saved && saved < deadline) deadline = saved;
    efs_net_set_deadline_ms(deadline);
    int result = EFS_ERR_BUSY;
    for (unsigned batch = 0; batch < 4096; batch++) {
        if (efs_net_remaining_ms(0x7fffffff) <= 0) break;
        int rc = sess_rpc(rpc, payload, length, reply);
        /* NOT_PRIMARY explicitly rejects proposal admission. Cold owners can
         * remain leaderless beyond sess_rpc's redirect window. Retry only
         * that definite rejection, with the same UUID/epoch/shard and the
         * original invocation deadline (also bounding socket waits). */
        if (rc == EFS_ERR_NOT_PRIMARY ||
            (rc == EFS_ERR_BUSY && !reply->index && !reply->term &&
             reply->salt == (appends ? EFS_MD_SESSION_APPEND_RETRY_SALT :
                                      EFS_MD_SESSION_CLEANUP_RETRY_SALT))) {
            usleep(50000);
            continue;
        }
        if (rc != EFS_OK) { result = rc; break; }
        if (reply->salt == 1) { result = EFS_OK; break; }
        if (reply->salt != 0) { result = EFS_ERR_PROTO; break; }
    }
    efs_net_set_deadline_ms(saved);
    return result;
}

static void wr64be_sess(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint64_t sess_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* The touched shards of one bitmap word that share a Raft group: one
 * FENCE_LOC and one ACK cover them all. */
struct sess_unit {
    uint32_t word;
    uint64_t mask;
};

/* One stage of work spread over rpc->parallel workers, one unit per item:
 * the fence stage runs FENCE_LOC+ACK, the cleanup stage one probe
 * (EFS_MD_REQ_SESSION_CLEANUP_PROBE) and both cleanup phases for every
 * shard the probe could not prove empty. Items are independent within a stage; the stages stay ordered by
 * the caller. Concurrent proposals share their group's fsync, so a session
 * with thousands of touched shards no longer pays one commit latency per
 * shard and step (A11 finding 1). The first failure stops new items;
 * in-flight items finish. Every step is idempotent, so a partial stage
 * stays resumable by a later fence. */
struct sess_stage {
    const struct efs_session_rpc *rpc;
    const uint8_t *uuid;
    const struct sess_unit *units;
    uint32_t n, total, ssh, epoch, old_epoch;
    int cleanup;
    uint64_t deadline;
    const char *label;
    pthread_mutex_t mu;
    uint32_t next, done, pending, probe_failed;
    int rc;
    uint32_t failed;
    const char *phase;
    struct efs_msg_raft_mkfs_reply last;
};

static int sess_item_step(struct sess_stage *st, uint32_t i,
                          struct efs_msg_raft_mkfs_reply *r, const char **phase,
                          uint32_t *failed)
{
    _Static_assert(EFS_MD_SESSION_CLEANUP_PROBE_LEN <= EFS_MD_SESS_FENCE_LOC_LEN,
                   "one payload buffer for both requests");
    uint8_t payload[1 + EFS_MD_SESS_FENCE_LOC_LEN];
    const struct sess_unit *u = &st->units[i];
    uint8_t group = efs_raft_shard_group(u->word * 64u + (uint32_t)__builtin_ctzll(u->mask));
    int rc;

    if (st->cleanup) {
        /* A probe only narrows the work: any failure, or a shard it does
         * not prove empty, takes the per-shard path with its own proofs. */
        uint64_t pending = u->mask;
        memset(payload, 0, sizeof(payload));
        payload[0] = group;
        payload[1] = EFS_MD_REQ_SESSION_CLEANUP_PROBE;
        memcpy(payload + 2, st->uuid, EFS_OPID_UUID_LEN);
        wr32be_sess(payload + 2 + EFS_OPID_UUID_LEN, st->old_epoch);
        wr32be_sess(payload + 6 + EFS_OPID_UUID_LEN, u->word);
        wr64be_sess(payload + 10 + EFS_OPID_UUID_LEN, u->mask);
        if (sess_rpc(st->rpc, payload, 1 + EFS_MD_SESSION_CLEANUP_PROBE_LEN, r) == EFS_OK)
            pending &= r->salt;
        else
            __atomic_fetch_add(&st->probe_failed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&st->pending, (uint32_t)__builtin_popcountll(pending), __ATOMIC_RELAXED);
        *phase = "cleanup";
        for (uint32_t b = 0; b < 64; b++) {
            uint32_t sh = u->word * 64u + b;
            if (!(pending >> b & 1))
                continue;
            *failed = sh;
            rc = efs_session_cleanup_bounded(st->rpc, st->uuid, st->old_epoch, sh, 0, r);
            if (rc == EFS_OK)
                rc = efs_session_cleanup_bounded(st->rpc, st->uuid, st->old_epoch, sh, 1, r);
            if (rc != EFS_OK)
                return rc;
        }
        return EFS_OK;
    }
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, group, EFS_MD_SESS_FENCE_LOC, st->uuid);
    wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN, st->epoch);
    wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN + 4, u->word);
    wr64be_sess(payload + 3 + EFS_OPID_UUID_LEN + 8, u->mask);
    *phase = "participant-floor";
    rc = efs_session_storage_barrier(st->rpc, payload, 1 + EFS_MD_SESS_FENCE_LOC_LEN, r);
    if (rc != EFS_OK)
        return rc;
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(st->ssh), EFS_MD_SESS_ACK, st->uuid);
    wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN, u->word);
    wr64be_sess(payload + 3 + EFS_OPID_UUID_LEN + 4, u->mask);
    *phase = "participant-ack";
    return sess_rpc(st->rpc, payload, 1 + EFS_MD_SESS_ACK_LEN, r);
}

static void *sess_stage_worker(void *arg)
{
    struct sess_stage *st = arg;
    struct efs_msg_raft_mkfs_reply r = {.leader_hint = -1};
    uint64_t saved = efs_net_deadline_ms();

    efs_net_set_deadline_ms(st->deadline);
    for (;;) {
        const char *phase = NULL;
        uint32_t i, done, failed = 0;
        int rc;

        pthread_mutex_lock(&st->mu);
        i = st->rc == EFS_OK && st->next < st->n ? st->next++ : st->n;
        pthread_mutex_unlock(&st->mu);
        if (i >= st->n)
            break;
        rc = sess_item_step(st, i, &r, &phase, &failed);
        pthread_mutex_lock(&st->mu);
        st->last = r;
        if (rc != EFS_OK && st->rc == EFS_OK) {
            st->rc = rc;
            st->failed = failed;
            st->phase = phase;
        }
        if (rc == EFS_OK)
            st->done += (uint32_t)__builtin_popcountll(st->units[i].mask);
        done = st->done;
        pthread_mutex_unlock(&st->mu);
        if (rc == EFS_OK && st->rpc->progress)
            st->rpc->progress(st->rpc->ctx, st->label, done, st->total);
    }
    efs_net_set_deadline_ms(saved);
    return NULL;
}

static int sess_stage_run(struct sess_stage *st)
{
    pthread_t workers[EFS_SESSION_REVOKE_PARALLEL_MAX];
    unsigned want = st->rpc->parallel, started = 0;

    if (want > EFS_SESSION_REVOKE_PARALLEL_MAX)
        want = EFS_SESSION_REVOKE_PARALLEL_MAX;
    if (want > st->n)
        want = st->n;
    st->deadline = efs_net_deadline_ms();
    if (pthread_mutex_init(&st->mu, NULL))
        return EFS_ERR_NOMEM;
    /* The caller is one worker; a thread that cannot start only narrows
     * the stage. */
    while (started + 1 < want &&
           !pthread_create(&workers[started], NULL, sess_stage_worker, st))
        started++;
    sess_stage_worker(st);
    for (unsigned w = 0; w < started; w++)
        pthread_join(workers[w], NULL);
    pthread_mutex_destroy(&st->mu);
    return st->rc;
}

/* Coordinator-driven I23 barrier (10.5c-35c). BEGIN freezes touched[],
 * FENCE_LOC+ACK every set bit, FINISH, then bounded lease/append cleanup on
 * each touched shard. GET shard>=4096 reads one word of the bitmap. */
int efs_session_revoke(const struct efs_session_rpc *rpc,
                       const uint8_t uuid[EFS_OPID_UUID_LEN],
                       struct efs_session_revoke_result *res)
{
    uint8_t payload[1 + 26];
    struct efs_msg_raft_mkfs_reply r = {.leader_hint = -1};
    uint32_t ssh, epoch = 0, old_epoch = 0, state = 0, word, ntouch = 0, nunit = 0, stopped = 0;
    uint32_t cleaned = 0, probe_failed = 0;
    static __thread struct sess_unit units[2 * (EFS_SESSION_BITMAP / 8u)];
    struct sess_stage stage;
    uint64_t t0 = sess_now_ms(), t_fence = 0, t_cleanup = 0, mark;
    int rc;
    const char *phase = "begin";

    ssh = efs_kv_session_shard(uuid);
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), EFS_MD_SESS_BEGIN, uuid);
    rc = sess_rpc(rpc, payload, 1 + 18, &r);
    if (rc != EFS_OK)
        goto out;
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), 0, uuid);
    wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN, ssh);
    rc = sess_rpc(rpc, payload, 1 + 18 + 4, &r);
    if (rc != EFS_OK)
        goto out;
    epoch = (uint32_t)(r.salt & 0xffffffffull);
    state = (uint32_t)((r.salt >> 32) & 0xffull);
    if (state != 2 || epoch == 0) {
        rc = EFS_ERR_BUSY;
        goto out;
    }
    old_epoch = epoch - 1;
    /* Seal every storage member before acknowledging metadata shards. A
     * non-hosting member cannot be inferred from the metadata Raft quorum. */
    memset(payload,0,sizeof(payload));
    sess_pack_hdr(payload,efs_raft_shard_group(ssh),EFS_MD_SESS_STORAGE_FENCE,uuid);
    wr32be_sess(payload+3+EFS_OPID_UUID_LEN,epoch);
    phase = "storage-fence";
    rc=efs_session_storage_barrier(rpc,payload,1+22,&r);
    if(rc!=EFS_OK)goto out;

    for (word = 0; word < (EFS_SESSION_BITMAP / 8u); word++) {
        uint64_t bits;
        uint32_t b;

        memset(payload, 0, sizeof(payload));
        sess_pack_hdr(payload, efs_raft_shard_group(ssh), 0, uuid);
        wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN, EFS_SESSION_BITS + word);
        rc = sess_rpc(rpc, payload, 1 + 18 + 4, &r);
        if (rc != EFS_OK)
            goto out;
        bits = r.salt;
        /* Odd shards and even shards of a word are in different groups
         * (efs_raft_shard_group); a FENCE_LOC never mixes them. */
        for (b = 0; b < 2; b++) {
            uint64_t mask = bits & (b ? 0x5555555555555555ull : 0xaaaaaaaaaaaaaaaaull);
            if (mask)
                units[nunit++] = (struct sess_unit){word, mask};
        }
        ntouch += (uint32_t)__builtin_popcountll(bits);
    }
    memset(&stage, 0, sizeof(stage));
    stage.rpc = rpc; stage.uuid = uuid; stage.units = units; stage.n = nunit;
    stage.total = ntouch;
    stage.ssh = ssh; stage.epoch = epoch; stage.old_epoch = old_epoch;
    stage.label = "fence"; stage.last = r;
    mark = sess_now_ms();
    rc = sess_stage_run(&stage);
    t_fence = sess_now_ms() - mark;
    r = stage.last;
    if (rc != EFS_OK) {
        phase = stage.phase;
        goto out;
    }
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), EFS_MD_SESS_FINISH, uuid);
    /* Freeze the epoch captured after BEGIN. Repeated storage verification
     * must never finish a later epoch advanced by another coordinator. */
    wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN, epoch);
    phase = "finish";
    rc = efs_session_storage_barrier(rpc, payload, 1 + 22, &r);
    if (rc != EFS_OK)
        goto out;
    phase = "cleanup";
    memset(&stage, 0, sizeof(stage));
    stage.rpc = rpc; stage.uuid = uuid; stage.units = units; stage.n = nunit;
    stage.total = ntouch;
    stage.ssh = ssh; stage.epoch = epoch; stage.old_epoch = old_epoch;
    stage.cleanup = 1; stage.label = "cleanup"; stage.last = r;
    mark = sess_now_ms();
    rc = sess_stage_run(&stage);
    t_cleanup = sess_now_ms() - mark;
    r = stage.last;
    cleaned = stage.pending;
    probe_failed = stage.probe_failed;
    if (rc != EFS_OK) {
        stopped = stage.failed;
        goto out;
    }
    memset(payload, 0, sizeof(payload));
    sess_pack_hdr(payload, efs_raft_shard_group(ssh), 0, uuid);
    wr32be_sess(payload + 3 + EFS_OPID_UUID_LEN, ssh);
    rc = sess_rpc(rpc, payload, 1 + 18 + 4, &r);
    if (rc != EFS_OK)
        goto out;
    epoch = (uint32_t)(r.salt & 0xffffffffull);
    state = (uint32_t)((r.salt >> 32) & 0xffull);
    if (state != 1) {
        rc = EFS_ERR_BUSY;
        goto out;
    }
out:
    if (res) {
        res->epoch = epoch;
        res->state = state;
        res->ntouch = ntouch;
        res->old_epoch = old_epoch;
        res->shard = stopped;
        res->phase = phase;
        res->last = r;
        res->fence_ms = t_fence;
        res->cleanup_ms = t_cleanup;
        res->total_ms = sess_now_ms() - t0;
        res->cleaned = cleaned;
        res->probe_failed = probe_failed;
    }
    return rc;
}
