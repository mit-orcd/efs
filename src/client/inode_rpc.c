#include "client_internal.h"
#include "efs/opid.h"
#include "efs/request_receipt.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/publication.h"
#include "efs/put_ticket.h"
#include "efs/session.h"
#include "efs/session_revoke.h"
#include "metadata_read_route.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

/* EFS_RPC_PROF: split inode-RPC wall time into conn checkout vs send vs
 * recv vs BUSY backoff. Recv is the server+wire wait (the parked H3
 * question). Checkout is pool serialization (16 conns/node). Off unless
 * the env is set — empty still counts as on, same as EFS_LOCK_PROF. */
static int efs_rpc_prof_on = -1;
static unsigned long long efs_rpc_calls;
static unsigned long long efs_rpc_checkout_us;
static unsigned long long efs_rpc_send_us;
static unsigned long long efs_rpc_recv_us;
static unsigned long long efs_rpc_busy_us;
static unsigned long long efs_rpc_busy_n;
static unsigned long long efs_rpc_checkout_wait_n;
static unsigned long long efs_rpc_n_op[256];
static unsigned long long efs_rpc_recv_us_op[256];
static unsigned long long efs_rpc_busy_n_op[256];
static unsigned long long efs_rpc_last_dump_us;

static unsigned long long rpc_prof_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull +
           (unsigned long long)ts.tv_nsec / 1000ull;
}

/* An inode RPC that returned OK only after the retry loop had already
 * spent a second. attempts is the try that succeeded (1 = first try, so
 * the second was a single slow round, not the BUSY ladder). The holder
 * go-timeouts had no line at all. */
static void rpc_note_slow_ok(uint8_t type, int attempt, int saw_busy,
                             int status, unsigned long long t0)
{
    unsigned long long us;

    if (status != EFS_INODE_RPC_OK || t0 == 0)
        return;
    us = rpc_prof_now_us() - t0;
    if (us < 1000000ull)
        return;
    fprintf(stderr, "inode-rpc: slow-ok type=%u attempts=%d saw_busy=%d "
            "status=%d us=%llu\n",
            type, attempt + 1, saw_busy, status, us);
}

/* W155: what the current rpc_send_recv_* call put on the wire. Bit nid-1
 * of rpc_tl_sent: a send of the call's request to nid succeeded.
 * rpc_tl_lost: replies lost after a successful send (receive failure or
 * timeout), i.e. sends whose outcome the client does not know. */
static __thread uint64_t rpc_tl_sent;
static __thread unsigned rpc_tl_lost;

/* 1 for a directory mutation framed with its op-id suffix (I16). */
static int rpc_identified(uint8_t type, uint32_t req_len)
{
    return (type == EFS_MSG_INODE_CREATE &&
            req_len == sizeof(struct efs_msg_inode_create) + EFS_OPID_WIRE_LEN) ||
           (type == EFS_MSG_INODE_UNLINK &&
            req_len == sizeof(struct efs_msg_inode_unlink) + EFS_OPID_WIRE_LEN) ||
           (type == EFS_MSG_INODE_LINK &&
            req_len == sizeof(struct efs_msg_inode_link) + EFS_OPID_WIRE_LEN) ||
           (type == EFS_MSG_INODE_RENAME_AT &&
            req_len == sizeof(struct efs_msg_inode_rename_at) + EFS_OPID_WIRE_LEN);
}

/* G1/G4: the request identity for log lines. An identified directory op
 * prints uuid8/epoch/seq from its op-id suffix (native u32 epoch, native
 * u64 seq); a REPORT prints uuid8/epoch/mint:seq from its session and
 * publication suffixes (big-endian); anything else "-". The same strings
 * appear in the server's opid-receipt / report-receipt lines. */
static const char *rpc_req_ident(uint8_t type, const void *req, uint32_t len,
                                 char *buf, size_t cap)
{
    const uint8_t *p = req;

    snprintf(buf, cap, "-");
    if (!req)
        return buf;
    if (rpc_identified(type, len)) {
        uint32_t epoch;
        uint64_t seq;
        const uint8_t *q = p + len - EFS_OPID_WIRE_LEN;
        memcpy(&epoch, q + EFS_OPID_UUID_LEN, 4);
        memcpy(&seq, q + EFS_OPID_UUID_LEN + 4, 8);
        snprintf(buf, cap, "%02x%02x%02x%02x/%u/%llu", q[0], q[1], q[2], q[3], epoch,
                 (unsigned long long)seq);
    } else if (type == EFS_MSG_REPORT_CHUNKS &&
               len >= sizeof(struct efs_msg_report_chunks) + EFS_REPORT_SESSION_LEN) {
        const struct efs_msg_report_chunks *hdr = req;
        uint32_t flags;
        const uint8_t *sp = p + len - EFS_REPORT_SESSION_LEN;
        uint32_t epoch = 0;
        uint64_t mint = 0, seq = 0;
        memcpy(&flags, &hdr->sync, sizeof(flags));
        if (!(flags & EFS_REPORT_F_SESSION))
            return buf;
        for (unsigned i = 0; i < 4; i++)
            epoch = (epoch << 8) | sp[16 + i];
        if ((flags & EFS_REPORT_F_PUBID) &&
            len >= sizeof(*hdr) + EFS_REPORT_SESSION_LEN + EFS_REPORT_PUBID_LEN) {
            const uint8_t *pp = sp - EFS_REPORT_PUBID_LEN;
            for (unsigned i = 0; i < 8; i++) {
                mint = (mint << 8) | pp[i];
                seq = (seq << 8) | pp[8 + i];
            }
        }
        snprintf(buf, cap, "%02x%02x%02x%02x/%u/%llx:%llu", sp[0], sp[1], sp[2], sp[3],
                 epoch, (unsigned long long)mint, (unsigned long long)seq);
    }
    return buf;
}

/* W155: how long a client may spend resolving a request whose reply was
 * lost, on top of the budget that expired. EFS_RESOLVE_MS overrides; read
 * once. */
static uint64_t rpc_resolve_ms(void)
{
    static long ms = -1;
    if (ms < 0) {
        const char *e = getenv("EFS_RESOLVE_MS");
        long v = e && *e ? strtol(e, NULL, 10) : 8000;
        ms = v < 0 ? 0 : v;
    }
    return (uint64_t)ms;
}

static int rpc_prof_enabled(void)
{
    if (efs_rpc_prof_on < 0)
        efs_rpc_prof_on = getenv("EFS_RPC_PROF") != NULL;
    return efs_rpc_prof_on;
}

static void rpc_prof_add(uint8_t type, unsigned long long checkout_us,
                         unsigned long long send_us, unsigned long long recv_us,
                         unsigned long long busy_us)
{
    if (!rpc_prof_enabled())
        return;
    __atomic_add_fetch(&efs_rpc_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_checkout_us, checkout_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_send_us, send_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_recv_us, recv_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_busy_us, busy_us, __ATOMIC_RELAXED);
    if (busy_us) {
        __atomic_add_fetch(&efs_rpc_busy_n, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&efs_rpc_busy_n_op[type], 1, __ATOMIC_RELAXED);
    }
    if (checkout_us >= 100ull)
        __atomic_add_fetch(&efs_rpc_checkout_wait_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_n_op[type], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_rpc_recv_us_op[type], recv_us, __ATOMIC_RELAXED);
    unsigned long long now = rpc_prof_now_us();
    unsigned long long last = __atomic_load_n(&efs_rpc_last_dump_us,
                                            __ATOMIC_RELAXED);
    if (last && now - last < 2000000ull)
        return;
    if (!__atomic_compare_exchange_n(&efs_rpc_last_dump_us, &last, now, 0,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    fprintf(stderr,
            "RPC-PROF calls=%llu checkout_us=%llu send_us=%llu recv_us=%llu "
            "busy_us=%llu busy_n=%llu checkout_wait_n=%llu "
            "lookup=%llu/%lluus getattr=%llu/%lluus create=%llu/%lluus "
            "setattr=%llu/%lluus readdir=%llu/%lluus unlink=%llu/%lluus "
            "append=%llu/%lluus report=%llu/%lluus getchunks=%llu/%lluus "
            "lookup_path=%llu/%lluus\n",
            __atomic_load_n(&efs_rpc_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_checkout_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_send_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_busy_us, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_busy_n, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_checkout_wait_n, __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_LOOKUP], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_LOOKUP],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_GETATTR], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_GETATTR],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_CREATE], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_CREATE],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_SETATTR], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_SETATTR],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_READDIR], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_READDIR],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_UNLINK], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_UNLINK],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_APPEND], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_APPEND],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_REPORT_CHUNKS], __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_REPORT_CHUNKS],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_GETCHUNKS],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_GETCHUNKS],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_n_op[EFS_MSG_INODE_LOOKUP_PATH],
                            __ATOMIC_RELAXED),
            __atomic_load_n(&efs_rpc_recv_us_op[EFS_MSG_INODE_LOOKUP_PATH],
                            __ATOMIC_RELAXED));
}

static int rpc_status_to_efs(uint8_t st)
{
    switch (st) {
    case EFS_INODE_RPC_OK:          return EFS_OK;
    case EFS_INODE_RPC_NOT_FOUND:   return EFS_ERR_NOT_FOUND;
    case EFS_INODE_RPC_EXIST:       return EFS_ERR_EXIST;
    case EFS_INODE_RPC_QUOTA:       return EFS_ERR_QUOTA;
    case EFS_INODE_RPC_BUSY:        return EFS_ERR_BUSY;
    case EFS_INODE_RPC_INVAL:       return EFS_ERR_INVAL;
    case EFS_INODE_RPC_NOT_PRIMARY: return EFS_ERR_NOT_PRIMARY;
    case EFS_INODE_RPC_NOT_EMPTY:   return EFS_ERR_NOT_EMPTY;
    case EFS_INODE_RPC_STALE:       return EFS_ERR_STALE;
    case EFS_INODE_RPC_NODATA:      return EFS_ERR_NODATA;
    case EFS_INODE_RPC_OVERFLOW:    return EFS_ERR_OVERFLOW;
    case EFS_INODE_RPC_FENCED:
        efs_client_session_fence(EFS_ERR_FENCED);
        return EFS_ERR_FENCED;
    case EFS_INODE_RPC_UNREGISTERED: return EFS_ERR_UNREGISTERED;
    case EFS_INODE_RPC_SYMLINK:
    case EFS_INODE_RPC_DEEP:        return EFS_ERR_PROTO;
    default:                        return EFS_ERR_IO;
    }
}

/* Namespace mutations whose txn may PREPARE STALE against a moved dentry,
 * child row or emptiness witness. None of them has committed anything when
 * the server says STALE, and each re-evaluates from scratch, so the client
 * retries them like BUSY. REPORT_CHUNKS / APPEND own their STALE. */
static int stale_retryable(uint8_t type)
{
    return type == EFS_MSG_INODE_CREATE || type == EFS_MSG_INODE_UNLINK ||
           type == EFS_MSG_INODE_LINK || type == EFS_MSG_INODE_RENAME_AT;
}

/* I16 op-id for the directory mutations above (§7.9). One identity per
 * mount (random uuid, epoch 1), one seq space, and an in-flight table:
 * the contiguous ack sent with every request is (lowest in-flight seq)
 * - 1, so a server window advances past seqs it never served. A slot is
 * held from before the first send to after the final return — every
 * BUSY / STALE / NOT_PRIMARY retry inside rpc_send_recv_* preserves the
 * identity and operation payload. Retries may advance only their contiguous
 * ACK as older operations finish. Table full = the op
 * receives BUSY before any RPC; no mutation loses its identity. */
static pthread_mutex_t opid_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t opid_cv = PTHREAD_COND_INITIALIZER;

static void opid_seed_locked(void)
{
    int fd, got = 0;

    if (g_client.opid_next)
        return;
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, g_client.opid_uuid, EFS_OPID_UUID_LEN);
        close(fd);
        got = n == (ssize_t)EFS_OPID_UUID_LEN;
    }
    if (!got) {
        uint64_t a = (uint64_t)getpid() * 0x9E3779B97F4A7C15ull ^
                     (uint64_t)time(NULL);
        uint64_t b = (uint64_t)(uintptr_t)&g_client ^ 0xD1B54A32D192ED03ull;
        memcpy(g_client.opid_uuid, &a, 8);
        memcpy(g_client.opid_uuid + 8, &b, 8);
    }
    g_client.opid_uuid[0] |= 1; /* never the all-zero "no identity" */
    g_client.opid_epoch = 1;
    g_client.opid_next = 1;
    memset(g_client.opid_inflight, 0, sizeof(g_client.opid_inflight));
}

int efs_client_publication_id(uint64_t sequence, struct efs_opid *out)
{
    if(!sequence || !out)return EFS_ERR_INVAL;
    memset(out,0,sizeof(*out));
    pthread_mutex_lock(&opid_mu);opid_seed_locked();
    memcpy(out->client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN);
    out->session_epoch=g_client.opid_epoch;out->seq=sequence;
    pthread_mutex_unlock(&opid_mu);return EFS_OK;
}

int efs_client_put_id(struct efs_opid *out)
{
    if (!out) return EFS_ERR_INVAL;
    memset(out,0,sizeof(*out));
    pthread_mutex_lock(&opid_mu);opid_seed_locked();
    if (!g_client.put_next) g_client.put_next=1;
    if (g_client.put_next==UINT64_MAX) {
        pthread_mutex_unlock(&opid_mu);return EFS_ERR_BUSY;
    }
    memcpy(out->client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN);
    out->session_epoch=g_client.opid_epoch;out->seq=g_client.put_next++;
    pthread_mutex_unlock(&opid_mu);return EFS_OK;
}

/* opid_mu is held. Allocation and the receipt-window check are one action;
 * both waiting namespace admission and lock-owning lease admission use it.
 * `who` names the admitting call site for the diagnostics. */
static int opid_take_locked(struct efs_opid_req *q, const void *who)
{
    /* Admission diagnostics (EFS_OPID_STATS=1, observability only): why
     * takes are refused, and how old the oldest unresolved identity is when
     * the sequence window blocks. Function-local so the tests that compile
     * this function alone keep working; every field is guarded by opid_mu. */
    static struct {
        uint64_t start_ms[EFS_OPID_INFLIGHT];
        const void *who[EFS_OPID_INFLIGHT];
        uint64_t taken, refused_slot, refused_window, last_report_ms, age_max_ms;
        const void *age_max_who;
    } st;
    static int st_on_s = -1; /* any thread may be first: atomic, local copy (TSan) */
    int st_on = __atomic_load_n(&st_on_s, __ATOMIC_ACQUIRE);
    int slot=-1,low_slot=-1;uint64_t low=0,now_ms=0;
    opid_seed_locked();
    if (st_on < 0) {
        st_on = getenv("EFS_OPID_STATS") != NULL;
        __atomic_store_n(&st_on_s, st_on, __ATOMIC_RELEASE);
    }
    if (st_on) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        now_ms = (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    }
    for(unsigned i=0;i<EFS_OPID_INFLIGHT;i++) {
        uint64_t seq=g_client.opid_inflight[i];
        if(!seq){if(slot<0)slot=(int)i;}
        else if(!low || seq<low){low=seq;low_slot=(int)i;}
    }
    if(g_client.opid_next==UINT64_MAX)return -2;
    if(slot<0 || (low && g_client.opid_next-low>=EFS_OPID_REPLY_CACHE)) {
        if (st_on) {
            uint64_t age = low_slot >= 0 ? now_ms - st.start_ms[low_slot] : 0;
            if (slot >= 0) st.refused_window++; else st.refused_slot++;
            if (age > st.age_max_ms) { st.age_max_ms = age; st.age_max_who = st.who[low_slot]; }
            if (now_ms - st.last_report_ms >= 1000) {
                st.last_report_ms = now_ms;
                fprintf(stderr, "opid-stats: taken=%llu refused_window=%llu refused_slot=%llu "
                        "oldest_age_ms=%llu oldest_who=%p max_age_ms=%llu max_who=%p\n",
                        (unsigned long long)st.taken, (unsigned long long)st.refused_window,
                        (unsigned long long)st.refused_slot, (unsigned long long)age,
                        low_slot >= 0 ? st.who[low_slot] : NULL,
                        (unsigned long long)st.age_max_ms, st.age_max_who);
                st.age_max_ms = 0;
            }
        }
        return -1;
    }
    if (st_on) {
        st.taken++;
        st.start_ms[slot] = now_ms;
        st.who[slot] = who;
    }
    uint64_t seq=g_client.opid_next++;
    g_client.opid_inflight[slot]=seq;
    memcpy(q->id.client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN);
    q->id.session_epoch=g_client.opid_epoch;q->id.seq=seq;
    q->ack=(low?low:seq)-1;
    return slot;
}

/* Fill q and take a slot (returned; -1 = none, q left invalid). */
static int opid_begin(struct efs_opid_req *q)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    memset(q,0,sizeof(*q));
    /* A later known reply still occupies the server cache until the oldest
     * unresolved identity finishes. Reserve space for that oldest identity:
     * never issue more sequence positions than the smallest receipt cache.
     * Otherwise later replies fill it and the missing oldest cannot commit. */
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (!deadline)
        deadline = (uint64_t)started.tv_sec * 1000 + started.tv_nsec / 1000000 + 1000;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms >= deadline || efs_client_rpc_past_deadline()) break;
        pthread_mutex_lock(&opid_mu);
        int slot=opid_take_locked(q, __builtin_return_address(0));
        if(slot>=0) {
            pthread_mutex_unlock(&opid_mu);
            if (env_opid_verbose)
                fprintf(stderr,"opid-begin: seq=%llu ack=%llu slot=%d thread=%lu deadline_ms=%llu\n",
                    (unsigned long long)q->id.seq,(unsigned long long)q->ack,slot,
                    (unsigned long)pthread_self(),(unsigned long long)efs_client_rpc_deadline_ms());
            return slot;
        }
        if (slot == -2) {
            pthread_mutex_unlock(&opid_mu);
            return -2; /* sequence exhaustion is permanent */
        }
        /* No identity has been issued. Wait for an owner to finish within
         * the caller's existing budget, rather than exhausting an unrelated
         * one-second poll count while a valid older operation still runs.
         * Periodic wakeups recheck cancellation/deadline without holding
         * the mutex needed by the finishing owner. */
        uint64_t wait_ms = deadline - now_ms;
        if (wait_ms > 50) wait_ms = 50;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)wait_ms * 1000000;
        until.tv_sec += until.tv_nsec / 1000000000;
        until.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&opid_cv, &opid_mu, &until);
        pthread_mutex_unlock(&opid_mu);
    }
    uint64_t oldest=0,next;unsigned live=0;
    pthread_mutex_lock(&opid_mu);
    next=g_client.opid_next;
    for(unsigned i=0;i<EFS_OPID_INFLIGHT;i++) {
        uint64_t seq=g_client.opid_inflight[i];
        if(seq){live++;if(!oldest || seq<oldest)oldest=seq;}
    }
    pthread_mutex_unlock(&opid_mu);
    fprintf(stderr,"opid-admission: BUSY oldest=%llu next=%llu live=%u window=%u\n",
        (unsigned long long)oldest,(unsigned long long)next,live,EFS_OPID_REPLY_CACHE);
    return -1;
}


static void opid_end(int slot)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    if (slot < 0)
        return;
    pthread_mutex_lock(&opid_mu);
    uint64_t seq = g_client.opid_inflight[slot];
    g_client.opid_inflight[slot] = 0;
    pthread_cond_broadcast(&opid_cv);
    pthread_mutex_unlock(&opid_mu);
    if (env_opid_verbose)
        fprintf(stderr,"opid-end: seq=%llu slot=%d thread=%lu\n",
            (unsigned long long)seq,slot,(unsigned long)pthread_self());
}

/* Wait until any identity ends or max_ms passes. Callers retry a refused
 * admission after it: a retry then runs as soon as the window can move
 * instead of after a fixed 1 ms sleep, and never later than one. */
void efs_client_opid_wait(unsigned max_ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)max_ms * 1000000;
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
    pthread_mutex_lock(&opid_mu);
    pthread_cond_timedwait(&opid_cv, &opid_mu, &until);
    pthread_mutex_unlock(&opid_mu);
}

/* A reservation identity lives across the entire FUSE BUSY retry loop,
 * not just one transport attempt. Share namespace's contiguous-ACK tracker;
 * append's distinct KV kind keeps its result namespace separate. The full
 * mount UUID is shared with lease and lock ownership. */
int efs_client_append_begin(struct efs_opid_req *q)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    int slot = opid_begin(q);
    if (slot >= 0 && env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=APPEND\n",(unsigned long long)q->id.seq);
    return slot;
}

void efs_client_append_end(int slot)
{
    opid_end(slot);
}

/* Append the wire suffix after `slen` bytes of struct; new length. */
static uint32_t opid_suffix(uint8_t *buf, uint32_t slen, const struct efs_opid_req *q)
{
    if (!efs_opid_req_valid(q))
        return slen;
    efs_opid_req_pack(q, buf + slen);
    return slen + EFS_OPID_WIRE_LEN;
}

/* The identity is immutable, but its contiguous ACK may advance while a
 * retry waits behind older operations. Never ACK this operation itself. */
static void opid_refresh_ack(struct efs_opid_req *q)
{
    if (!efs_opid_req_valid(q)) return;
    uint64_t low=0;int owned=0;
    pthread_mutex_lock(&opid_mu);
    for (unsigned i=0;i<EFS_OPID_INFLIGHT;i++) {
        uint64_t seq=g_client.opid_inflight[i];
        if (seq==q->id.seq) owned=1;
        if (seq && (!low || seq<low)) low=seq;
    }
    if (owned && low && low-1>q->ack && low<=q->id.seq &&
        q->id.session_epoch==g_client.opid_epoch &&
        !memcmp(q->id.client_uuid,g_client.opid_uuid,EFS_OPID_UUID_LEN))
        q->ack=low-1;
    pthread_mutex_unlock(&opid_mu);
}
static void opid_retry_ack(void *packet, uint32_t length)
{
    struct efs_opid_req q;
    uint8_t *suffix=(uint8_t *)packet+length-EFS_OPID_WIRE_LEN;
    efs_opid_req_unpack(&q,suffix);
    opid_refresh_ack(&q);
    efs_opid_req_pack(&q,suffix);
}

/* Phase 2b: the metadata primary is the lowest-id live node. Mutations must
 * reach it (the meta-flush thread is primary-only). */
static struct efs_conn *rpc_primary_conn(efs_node_id_t *nid_out)
{
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = 0;
    for (uint32_t i = 0; i < __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE) && nlive < EFS_MAX_NODES; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (efs_client_node_is_down(id))
            continue;
        live[nlive++] = id;
    }
    efs_node_id_t best = efs_shard_owner_of(0, 1, live, nlive);
    if (best == 0)
        return NULL;
    struct efs_conn *conn = efs_client_conn_get(best);
    if (conn)
        *nid_out = best;
    return conn;
}

/* Route to the owner of an explicit shard id (not an inode). Do not
 * invent an ino to fake this — crafted inos collide with ROOT and
 * mis-route GETCHUNKS/REPORT after extent sharding. */

/* The metadata engine is the Raft+KV host, never the in-memory table.
 * Shards map to Raft groups by the
 * compiled-in rule (odd shard -> group 0, even -> group 2;
 * include/efs/raft.h), and each group's voters are a fixed node set
 * (group 0 = nodes 1,2,3; group 2 = nodes 2,3,4 for a 4-node cluster).
 * Any voter can answer or bounce the RPC to the leader, and
 * rpc_send_recv_shard follows the primary_id hint, so the client only
 * needs to reach *a* live voter of the right group. */
static uint32_t raft_group_voters(uint8_t group, int n)
{
    if (n <= 3)
        return (1u << n) - 1u;
    if (group == EFS_RAFT_GROUP_SHARD)
        return 0x7u; /* nodes 1,2,3 = raft ids 0,1,2 */
    return 0xeu;     /* nodes 2,3,4 = raft ids 1,2,3 */
}

/* Leader of each metadata group as the last OK reply named it
 * (set_inode_rc puts the ReadIndex group's leader in primary_id on OK
 * too). A follower-served read is a ReadIndex round trip to the leader
 * plus a catch-up wait before the KV get; sending it to the leader
 * removes that hop (Spark du Oct 2: 2.5 ms per stat on the follower
 * host vs 1.4 on the leader host). A wrong note costs the old path: the
 * leader forwards or answers NOT_PRIMARY with the hint and the reply
 * corrects the note. 0 = unknown; indexed by group >> 1. */
static efs_node_id_t g_group_leader[2];
static struct efs_meta_read_routes g_read_routes={.mu=PTHREAD_MUTEX_INITIALIZER};

static void rpc_note_leader(uint8_t group, efs_node_id_t nid)
{
    unsigned gi = group >> 1;

    if (gi >= 2 || nid == 0 || nid > EFS_RAFT_MAX_PEERS)
        return;
    efs_node_id_t previous=__atomic_exchange_n(&g_group_leader[gi],nid,__ATOMIC_RELAXED);
    if (previous != nid)
        efs_meta_read_route_reset(&g_read_routes);
}

static efs_node_id_t rpc_leader_of(uint8_t group)
{
    unsigned gi = group >> 1;

    if (gi >= 2)
        return 0;
    return __atomic_load_n(&g_group_leader[gi], __ATOMIC_RELAXED);
}

/* The last OK LOOKUP/GETATTR reply's leader note, keyed by the group of
 * the inode the reply describes (a LOOKUP's hint is the child's group,
 * host_read_inode_lanes sets it last). */
static void rpc_learn_leader(const struct efs_msg_inode_reply *r)
{
    if (!r || r->status != EFS_INODE_RPC_OK || r->primary_id == 0 ||
        r->inode.ino == 0)
        return;
    rpc_note_leader(efs_raft_shard_group(efs_kv_inode_shard(r->inode.ino)),
                    r->primary_id);
}

/* First live voter of the shard's group, not `skip`; the host bounces to
 * the leader and the retry loop follows primary_id, so any voter is a
 * fine entry point. The group's noted leader goes first when it is a live
 * voter (see g_group_leader). `skip` is the voter that just answered NOT_PRIMARY
 * with no hint: this pick is deterministic, so without it the same node
 * was asked 16 times over 10.3 s and the op failed EBUSY (Sep 29, fstor007
 * ecopy: 810 such lines, SETATTR/CREATE/RENAME_AT, both terms stable).
 * When every other voter is down the skipped one is still the answer. */
static struct efs_conn *raft_voter_conn_skip(uint32_t shard,
                                             efs_node_id_t *nid_out,
                                             efs_node_id_t skip)
{
    uint8_t group = efs_raft_shard_group(shard);
    int n = (int)__atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
    uint32_t voters = raft_group_voters(group, n);
    efs_node_id_t lead = rpc_leader_of(group);
    int pass;

    if (lead && lead != skip && lead <= (efs_node_id_t)n &&
        (voters & (1u << (lead - 1))) && !efs_client_node_is_down(lead)) {
        struct efs_conn *conn = efs_client_conn_get(lead);
        if (conn) {
            *nid_out = lead;
            return conn;
        }
    }
    for (pass = 0; pass < 2; pass++) {
        for (int rid = 0; rid < n && rid < EFS_RAFT_MAX_PEERS; rid++) {
            if (!(voters & (1u << rid)))
                continue;
            efs_node_id_t id = (efs_node_id_t)(rid + 1);
            if (pass == 0 && skip && id == skip)
                continue;
            if (efs_client_node_is_down(id))
                continue;
            struct efs_conn *conn = efs_client_conn_get(id);
            if (conn) {
                *nid_out = id;
                return conn;
            }
        }
        if (!skip)
            break;
    }
    return rpc_primary_conn(nid_out);
}

static struct efs_conn *raft_voter_conn(uint32_t shard,
                                        efs_node_id_t *nid_out)
{
    return raft_voter_conn_skip(shard, nid_out, 0);
}

/* A voter of EVERY metadata group. Reports carry recs for many inos whose
 * inode groups and lane groups differ, and only a dual-host can apply the
 * whole batch locally (the server forwards otherwise — this just saves the
 * hop). Whenever both groups are writable some dual-host is up: with the
 * compiled-in 4-node mapping (voters {1,2,3} and {2,3,4}) two quorums
 * always share a live node. Falls back to any group-0 voter when no
 * dual-host is reachable — the server side forwards from there. */
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t *nid_out,
                                            efs_node_id_t skip)
{
    int n = (int)__atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE);
    uint32_t dual = raft_group_voters(EFS_RAFT_GROUP_SHARD, n) &
                    raft_group_voters(EFS_RAFT_GROUP_SHARD2, n);

    /* Try the excluded peer only after the other dual hosts. Checking out
     * its socket first can consume the whole RPC deadline on a failed peer
     * or a full pool before the healthy alternative is even attempted. */
    for (unsigned pass = 0; pass < 2; pass++) {
        for (int rid = 0; rid < n && rid < EFS_RAFT_MAX_PEERS; rid++) {
            efs_node_id_t id = (efs_node_id_t)(rid + 1);
            if (!(dual & (1u << rid)) ||
                (pass == 0 ? id == skip : !skip || id != skip) ||
                efs_client_node_is_down(id))
                continue;
            struct efs_conn *conn = efs_client_conn_get(id);
            if (conn) {
                *nid_out = id;
                return conn;
            }
        }
    }
    return raft_voter_conn(EFS_ROOT_INO, nid_out);
}

static struct efs_conn *rpc_owner_conn_shard(uint32_t shard,
                                             efs_node_id_t *nid_out)
{
    return raft_voter_conn(shard, nid_out);
}


static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,
                              uint8_t expect, void *reply, uint32_t reply_len,
                              int recv_ms);
static int rpc_send_recv_dual_once(uint8_t type, const void *req, uint32_t req_len,
                                   uint8_t expect, void *reply, uint32_t reply_len,
                                   int recv_ms);

/* Send to the owner of `shard`. Retry NOT_PRIMARY. */
static int rpc_writer_retry_pause(unsigned attempt);

#if EFS_FAULTS
/* Test-only: consume a committed CREATE reply, then hide a replay as BUSY.
 * Retained FUSE CREATE uses separate framing and may switch submit/probe or
 * advance its ACK. Its creation arguments, identity and durable result must
 * stay identical; an unresolved probe is never a committed reply. */
static int rpc_fault_create_reply(uint8_t type, const void *req, uint32_t len,
                                  uint8_t rtype, void *payload, uint32_t plen)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static unsigned stage;
    static struct efs_msg_inode_create original;
    static struct efs_opid identity;
    static efs_ino_t ino;
    static uint64_t generation;
    static uint32_t shard;
    static int original_retained;
    const char *marker = getenv("EFS_FAULT_CREATE_RETRY_FILE");
    if (!marker || access(marker,F_OK)) return 0;
    const struct efs_msg_inode_create *create;
    struct efs_msg_inode_reply *reply;
    struct efs_msg_create_retained_reply *retained_reply = NULL;
    struct efs_opid_req q;
    if (type == EFS_MSG_CREATE_RETAINED &&
        len == sizeof(struct efs_msg_create_retained) &&
        rtype == EFS_MSG_CREATE_RETAINED_REPLY &&
        plen == sizeof(struct efs_msg_create_retained_reply)) {
        const struct efs_msg_create_retained *r = req;
        retained_reply = payload;
        if (!retained_reply->known || !retained_reply->routed ||
            !retained_reply->generation) return 0;
        create = &r->create; q = r->q; reply = &retained_reply->rpc;
    } else if (type == EFS_MSG_INODE_CREATE &&
               len == sizeof(*create)+EFS_OPID_WIRE_LEN &&
               rtype == EFS_MSG_INODE_CREATE_REPLY &&
               plen == sizeof(*reply)) {
        create = req; reply = payload;
        efs_opid_req_unpack(&q,(const uint8_t *)req+sizeof(*create));
    } else return 0;
    if (strcmp(create->name,"w58-live-retry") || reply->status != EFS_INODE_RPC_OK)
        return 0;
    pthread_mutex_lock(&mu);
    if (stage >= 3) { pthread_mutex_unlock(&mu); return 0; }
    if (!stage) {
        original = *create; identity = q.id; ino = reply->inode.ino;
        original_retained = retained_reply != NULL;
        if (retained_reply) {
            generation = retained_reply->generation; shard = retained_reply->shard;
        }
    }
    if (memcmp(&original,create,sizeof(original)) ||
        identity.seq != q.id.seq || identity.session_epoch != q.id.session_epoch ||
        memcmp(identity.client_uuid,q.id.client_uuid,EFS_OPID_UUID_LEN) ||
        ino != reply->inode.ino || original_retained != (retained_reply != NULL) ||
        (retained_reply && (generation != retained_reply->generation ||
                            shard != retained_reply->shard))) abort();
    fprintf(stderr,"test-create-retry: stage=%u seq=%llu ino=%llu identical=1\n",
            stage,(unsigned long long)q.id.seq,(unsigned long long)ino);
    unsigned current=stage++;
    if (current == 1) {
        reply->status=EFS_INODE_RPC_BUSY;
        if (retained_reply) {
            retained_reply->known=0; retained_reply->generation=0;
            memset(&reply->inode,0,sizeof(reply->inode));
        }
    }
    pthread_mutex_unlock(&mu);
    return current == 0 ? -1 : 0;
}
#endif

/* A transport failure invalidates this attempt's hint. Preserve the
 * request identity while retrying another voter, and never erase a newer
 * leader learned concurrently by another worker. Pool admission failure
 * is not evidence that a peer is down. */
static void rpc_retry_peer(uint32_t shard, efs_node_id_t nid,
                            efs_node_id_t *target, efs_node_id_t *skip, int failed)
{
    unsigned group = efs_raft_shard_group(shard) >> 1;
    *target = 0;
    *skip = nid;
    if (!nid) return;
    if (failed) efs_client_node_note_fail(nid);
    if (group < 2) {
        efs_node_id_t expected = nid;
        __atomic_compare_exchange_n(&g_group_leader[group], &expected, 0,
                                    0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
}

/* 1 when this request changes session-owned metadata (K5 coverage table in
 * docs/how-it-works/protocols/sessions.md). Reads and probes are not gated. */
static int rpc_type_mutates(uint8_t type, const void *req, uint32_t req_len)
{
    switch (type) {
    case EFS_MSG_INODE_CREATE: case EFS_MSG_INODE_UNLINK:
    case EFS_MSG_INODE_LINK: case EFS_MSG_INODE_RENAME_AT:
    case EFS_MSG_INODE_SETATTR: case EFS_MSG_INODE_APPEND:
    case EFS_MSG_INODE_HOLD: case EFS_MSG_REPORT_CHUNKS:
        return 1;
    case EFS_MSG_CREATE_RETAINED:
        return !(req && req_len >= sizeof(struct efs_msg_create_retained) &&
                 ((const struct efs_msg_create_retained *)req)->probe);
    case EFS_MSG_INODE_FLOCK: {
        uint32_t op = 0;
        if (req && req_len >= sizeof(struct efs_msg_inode_flock))
            memcpy(&op, (const uint8_t *)req +
                   offsetof(struct efs_msg_inode_flock, op), sizeof(op));
        return !(op & EFS_FLOCK_GETLK);
    }
    case EFS_MSG_XATTR: {
        uint8_t op = 0;
        if (req && req_len >= sizeof(struct efs_msg_xattr))
            op = ((const struct efs_msg_xattr *)req)->op;
        return op == EFS_XATTR_SET || op == EFS_XATTR_REMOVE;
    }
    default:
        return 0;
    }
}

static int rpc_send_recv_shard_once(uint32_t shard, uint8_t type, const void *req,
                                    uint32_t req_len, uint8_t expect, void *reply,
                                    uint32_t reply_len, uint32_t copy_cap,
                                    uint32_t *copied)
{
    rpc_tl_sent = 0;
    rpc_tl_lost = 0;
    if (rpc_type_mutates(type, req, req_len)) {
        int ready = efs_client_session_check();
        if (ready != EFS_OK)
            return ready;
    }
    efs_node_id_t target = 0; /* 0 = compute the owner from our view */
    efs_node_id_t skip = 0;   /* answered NOT_PRIMARY with no hint */
    int prof = rpc_prof_enabled();
    int saw_busy = 0;
    int read_route = type == EFS_MSG_INODE_LOOKUP || type == EFS_MSG_INODE_GETATTR;
    uint64_t read_key = read_route ? efs_meta_read_route_key(type,req,req_len) : 0;
    unsigned long long t_start = rpc_prof_now_us();
    union { struct efs_msg_inode_create create; struct efs_msg_inode_unlink unlink;
            struct efs_msg_inode_link link; struct efs_msg_inode_rename_at rename; } mutation;
    uint8_t create_retry[sizeof(mutation)+EFS_OPID_WIRE_LEN];
    uint32_t flock_op = 0;
    if (type == EFS_MSG_INODE_FLOCK && req &&
        req_len >= sizeof(struct efs_msg_inode_flock))
        memcpy(&flock_op, (const uint8_t *)req +
               offsetof(struct efs_msg_inode_flock, op), sizeof(flock_op));
    int identified_create=(type==EFS_MSG_INODE_CREATE && req_len==sizeof(mutation.create)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_UNLINK && req_len==sizeof(mutation.unlink)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_LINK && req_len==sizeof(mutation.link)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_RENAME_AT && req_len==sizeof(mutation.rename)+EFS_OPID_WIRE_LEN);
    if (identified_create) { memcpy(create_retry,req,req_len);req=create_retry; }
    for (int attempt = 0; attempt < 16; attempt++) {
        if (identified_create) opid_retry_ack(create_retry,req_len);
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
        if (target != 0) {
            conn = efs_client_conn_get(target);
            nid = target;
        } else if (read_route) {
            nid = efs_meta_read_route_get(&g_read_routes,read_key);
            conn = nid && nid != skip && nid <= __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE) &&
                   !efs_client_node_is_down(nid) ? efs_client_conn_get(nid) : NULL;
            if (!conn)
                conn = raft_voter_conn_skip(shard, &nid, skip);
        } else {
            conn = raft_voter_conn_skip(shard, &nid, skip);
        }
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn || efs_conn_send_msg(conn, type, req, req_len) != 0) {
            if (conn)
                efs_client_conn_drop(nid, conn);
            rpc_retry_peer(shard, nid, &target, &skip, conn != NULL);
            /* One dropped conn is not a failed create. Same 16-attempt
             * budget as BUSY; a hard NET only after it is used up
             * (dir_many_files EIO on a single recv failure). */
            if (attempt + 1 < 16) {
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=send\n",
                        type, attempt);
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        if (nid && nid <= 64)
            rpc_tl_sent |= 1ull << (nid - 1);
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
#if EFS_FAULTS
        if (!rc) rc=rpc_fault_create_reply(type,req,req_len,rtype,payload,plen);
#endif
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            char ident[64];
            free(payload);
            rpc_tl_lost++;
            efs_client_conn_drop(nid, conn);
            rpc_retry_peer(shard, nid, &target, &skip, 1);
            if (attempt + 1 < 16) {
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=recv rc=%d "
                        "nid=%u id=%s\n", type, attempt, rc, nid,
                        rpc_req_ident(type, req, req_len, ident, sizeof(ident)));
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        efs_client_node_note_ok(nid);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            rpc_tl_lost++; /* a reply arrived too late to be used */
            return EFS_ERR_BUSY;
        }
        if (rtype != expect || plen < reply_len) {
            fprintf(stderr, "inode-rpc: proto type=%u rtype=%u plen=%u "
                    "reply_len=%u nid=%u\n",
                    type, rtype, plen, reply_len, nid);
            free(payload);
            return EFS_ERR_PROTO;
        }
        {
            uint32_t cap = copy_cap ? copy_cap : reply_len;
            uint32_t ncopy;

            if (cap < reply_len) {
                free(payload);
                return EFS_ERR_PROTO;
            }
            ncopy = plen < cap ? plen : cap;
            memcpy(reply, payload, ncopy);
            if (copied)
                *copied = ncopy;
        }
        free(payload);
        struct efs_msg_inode_reply *r = reply;
        if (read_route && r->status == EFS_INODE_RPC_OK && r->read_node_id &&
            r->read_node_id <= __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE)) {
            rpc_learn_leader(r);
            efs_meta_read_route_note(&g_read_routes,read_key,r->read_node_id);
        }
        if (r->status == EFS_INODE_RPC_BUSY) {
            saw_busy = 1;
            /* INODE_APPEND uses BUSY as the unflushed-reservation barrier.
             * The caller must flush + report before retrying; spinning here
             * holds g_append_mu for ~10s and deadlocks concurrent O_APPEND. */
            /* An unlock has no reservation/conflict barrier for its caller
             * to resolve. Retry the identical owner/session/range after
             * transient authority/proposal BUSY, with the existing budget.
             * Grants retain their immediate conflict response. */
            if (r->status == EFS_INODE_RPC_BUSY &&
                (type == EFS_MSG_INODE_APPEND ||
                (type == EFS_MSG_INODE_FLOCK && !(flock_op & EFS_FLOCK_UN)) ||
                type == EFS_MSG_CREATE_RETAINED)) {
                rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
                return EFS_OK;
            }
            /* Extra-shard owner is still assembling pages after restart.
             * Same target — do not flip to another node. */
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status == EFS_INODE_RPC_STALE && stale_retryable(type)) {
            /* PREPARE can report the participant leader whose canonical
             * version disagreed with this replica. Re-reading repeatedly on
             * the same coordinator cannot resolve that replica-local drift. */
            target = r->primary_id && r->primary_id <= EFS_MAX_NODES &&
                     r->primary_id != nid ? r->primary_id : 0;
            skip = target ? 0 : nid;
            /* A directory-op txn lands STALE when a same-name dentry, the
             * child row, or an emptiness witness moved between its read
             * and its PREPARE (the parent row itself is a commutative
             * reduce since §7.2 and never conflicts). Nothing committed;
             * re-running re-evaluates against the new state (EEXIST /
             * ENOENT / ENOTEMPTY as appropriate), so retry like BUSY.
             * REPORT_CHUNKS owns its STALE (W1 refetch+overlay) — never
             * retried here. */
            saw_busy = 1;
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
        if (t3 > t2 && t3 - t2 > 10000)
            fprintf(stderr, "inode-rpc: slow-recv type=%u us=%llu\n",
                    type, t3 - t2);
        if (r->status != EFS_INODE_RPC_NOT_PRIMARY) {
            rpc_note_slow_ok(type, attempt, saw_busy, r->status, t_start);
            return EFS_OK;
        }
        /* NOT_PRIMARY: retry on the server-reported primary. No hint (or
         * the hint is the node that just answered) = an election is in
         * progress or a stale leader just stepped down and has not heard
         * the new one yet (0.5-1 s). That is transient, same as BUSY:
         * back off and re-pick, do not fail. Failing here turned into
         * ENOENT/EIO at every caller (stat_ino maps any rc to NOT_FOUND),
         * e.g. GETATTR of a just-made dir right after a leader freeze. */
        if (r->primary_id == 0 || r->primary_id == nid) {
            unsigned long long sleep_start = rpc_prof_now_us();
            saw_busy = 1;
            target = 0;
            skip = nid;
            /* The noted leader no longer is one: forget it so the
             * re-pick does not land here again. */
            if (rpc_leader_of(efs_raft_shard_group(shard)) == nid)
                __atomic_store_n(&g_group_leader[efs_raft_shard_group(shard) >> 1],
                                 0, __ATOMIC_RELAXED);
            fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=no-hint from=%u\n",
                    type, attempt, nid);
            if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2,
                         rpc_prof_now_us() - sleep_start);
            continue;
        }
        target = r->primary_id;
    }
    /* Exhausted BUSY retries is not "no primary" — that mapped to EIO
     * on dir-rename under load (isolated PASS). Say so: an app-visible
     * EBUSY after 10 s of retries on an idle cluster is a server bug and
     * the server line (raft-host: mkdir/create/unlink ... rc=) is the pair. */
    if (saw_busy) {
        char ident[64];
        fprintf(stderr, "inode-rpc: shard=%u type=%u exhausted 16 BUSY/STALE "
                "retries (%.1f s) -> EBUSY id=%s\n", shard, type, 10.35,
                rpc_req_ident(type, req, req_len, ident, sizeof(ident)));
    }
    return saw_busy ? EFS_ERR_BUSY : EFS_ERR_NOT_PRIMARY;
}

/* W155: an identified directory op (I16 op-id) whose reply was lost after
 * a successful send, and whose retry loop then ran out of budget, has an
 * unknown outcome: the server may have applied it. Its durable receipt is
 * the op-id window on the serving shard, which answers a same-identity
 * re-send with the recorded verdict instead of running it again
 * (`opid-receipt: replay` on the server). So resolve by re-sending the
 * identical request under a fresh, bounded deadline (rpc_resolve_ms): an
 * applied op returns its original reply, an unapplied one runs now. Only
 * when the bound also passes without an answer does the caller see the
 * error, and the line says the outcome is unknown. */
static int rpc_resolve_identified(int dual, uint32_t shard, uint8_t type,
                                  const void *req, uint32_t req_len, uint8_t expect,
                                  void *reply, uint32_t reply_len, uint32_t copy_cap,
                                  uint32_t *copied, int recv_ms, int rc)
{
    char ident[64];
    uint64_t bound = rpc_resolve_ms();
    uint64_t previous, start, end;
    unsigned passes = 0, lost = rpc_tl_lost;
    int first = rc;

    if (!bound || !rpc_identified(type, req_len) || !rpc_tl_lost ||
        (rc != EFS_ERR_BUSY && rc != EFS_ERR_NET))
        return rc;
    rpc_req_ident(type, req, req_len, ident, sizeof(ident));
    previous = efs_client_rpc_deadline_ms();
    start = rpc_prof_now_us() / 1000ull;
    end = start + bound;
    fprintf(stderr, "inode-rpc: resolve type=%u id=%s rc=%d lost=%u: reply lost, "
            "same-identity lookup for %llu ms\n", type, ident, rc, lost,
            (unsigned long long)bound);
    while (rpc_prof_now_us() / 1000ull < end) {
        efs_client_rpc_set_deadline_ms(end);
        passes++;
        rc = dual ? rpc_send_recv_dual_once(type, req, req_len, expect, reply, reply_len,
                                            recv_ms)
                  : rpc_send_recv_shard_once(shard, type, req, req_len, expect, reply,
                                             reply_len, copy_cap, copied);
        lost += rpc_tl_lost;
        if (rc != EFS_ERR_BUSY && rc != EFS_ERR_NET)
            break;
    }
    efs_client_rpc_set_deadline_ms(previous);
    if (rc == EFS_OK)
        fprintf(stderr, "inode-rpc: resolve type=%u id=%s outcome=answered status=%u "
                "passes=%u ms=%llu\n", type, ident,
                ((const struct efs_msg_inode_reply *)reply)->status, passes,
                (unsigned long long)(rpc_prof_now_us() / 1000ull - start));
    else
        fprintf(stderr, "inode-rpc: resolve type=%u id=%s outcome=%s rc=%d first_rc=%d "
                "passes=%u lost=%u ms=%llu\n", type, ident,
                rc == EFS_ERR_BUSY || rc == EFS_ERR_NET ? "unknown" : "refused", rc, first,
                passes, lost, (unsigned long long)(rpc_prof_now_us() / 1000ull - start));
    return rc;
}

static int rpc_send_recv_shard(uint32_t shard, uint8_t type, const void *req,
                               uint32_t req_len, uint8_t expect, void *reply,
                               uint32_t reply_len, uint32_t copy_cap,
                               uint32_t *copied)
{
    int rc = rpc_send_recv_shard_once(shard, type, req, req_len, expect, reply,
                                      reply_len, copy_cap, copied);
    return rpc_resolve_identified(0, shard, type, req, req_len, expect, reply, reply_len,
                                  copy_cap, copied, 0, rc);
}

static int rpc_send_recv_owner(efs_ino_t ino, uint8_t type, const void *req,
                               uint32_t req_len, uint8_t expect, void *reply,
                               uint32_t reply_len)
{
    /* The KV shard space is the fixed 12-bit one (ino & 0xFFF). */
    return rpc_send_recv_shard(efs_export_shard_of(ino, EFS_KV_SHARD_BITS),
                               type, req, req_len, expect, reply, reply_len,
                               0, NULL);
}

/* Client-admitted mutation (K5): admit the target shard for this mount's
 * session (cached per shard), send, and on an UNREGISTERED refusal drop the
 * cached admission, admit again and resend the identical request once. The
 * reply's first byte is the inode RPC status for every caller. */
static int rpc_send_recv_admitted(uint32_t shard, const struct efs_opid *id,
                                  uint8_t type, const void *req, uint32_t req_len,
                                  uint8_t expect, void *reply, uint32_t reply_len,
                                  uint32_t copy_cap, uint32_t *copied)
{
    for (int pass = 0;; pass++) {
        int rc = efs_client_session_admit(shard, id);
        if (rc != EFS_OK)
            return rc;
        rc = rpc_send_recv_shard(shard, type, req, req_len, expect, reply,
                                 reply_len, copy_cap, copied);
        if (rc != EFS_OK || pass ||
            *(const uint8_t *)reply != EFS_INODE_RPC_UNREGISTERED)
            return rc;
        efs_client_session_forget(shard);
    }
}

int efs_client_rpc_lookup_path(efs_export_id_t export_id, efs_ino_t start,
                               const char *path, uint32_t flags,
                               struct efs_msg_inode_lookup_path_reply *out)
{
    struct efs_msg_inode_lookup_path req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.flags = flags;
    req.start = start ? start : EFS_ROOT_INO;
    if (path)
        strncpy(req.path, path, sizeof(req.path) - 1);
    struct efs_msg_inode_lookup_path_reply r;
    int rc = rpc_send_recv_owner(req.start, EFS_MSG_INODE_LOOKUP_PATH, &req,
                                 sizeof(req), EFS_MSG_INODE_LOOKUP_PATH_REPLY,
                                 &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r;
    return EFS_OK;
}

int efs_client_rpc_lookup(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, struct efs_inode *out)
{
    struct efs_msg_inode_lookup req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    if (name)
        strncpy(req.name, name, EFS_MAX_NAME - 1);
    struct efs_msg_inode_reply r;
    /* The raft host resolves dentry placement (local vs hashed layout)
     * internally and bounces when the answering node lacks a group, so the
     * client always enters at the parent directory's shard. */
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_LOOKUP, &req, sizeof(req),
                                 EFS_MSG_INODE_LOOKUP_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    rpc_learn_leader(&r);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_create(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, uint32_t mode, uid_t uid, gid_t gid,
                          uint32_t flags, efs_ino_t *out_ino,
                          struct efs_inode *out)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_trace_s = -1;
    int env_opid_trace = __atomic_load_n(&env_opid_trace_s, __ATOMIC_ACQUIRE);
    if (env_opid_trace < 0) {
        env_opid_trace = getenv("EFS_OPID_TRACE") != NULL;
        __atomic_store_n(&env_opid_trace_s, env_opid_trace, __ATOMIC_RELEASE);
    }
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    uint8_t buf[sizeof(struct efs_msg_inode_create) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_create *req = (struct efs_msg_inode_create *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->parent = parent;
    if (name)
        strncpy(req->name, name, EFS_MAX_NAME - 1);
    req->mode = mode;
    req->uid = (uint32_t)uid;
    req->gid = (uint32_t)gid;
    req->flags = flags;
    if ((flags & EFS_CREATE_F_HOLD) && !g_client.flock_token)
        g_client.flock_token = 1;
    req->owner = g_client.flock_token;
    slot = opid_begin(&q);
    /* A full local receipt window has sent no CREATE and allocated no
     * identity. Wait only inside an existing caller deadline, as retained
     * regular CREATE does; admitted transport retries keep one identity. */
    while (slot == -1 && efs_client_rpc_deadline_ms() &&
           !efs_client_rpc_past_deadline())
        slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    if (env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=%s parent=%llu name=%s\n",
            (unsigned long long)q.id.seq,S_ISDIR(mode)?"MKDIR":"CREATE",
            (unsigned long long)parent,name?name:"");
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    /* Files co-locate with the parent (one group). MKDIR scatters the
     * child inode, so a parent-group-only voter has to bounce the whole
     * RPC to a dual-host — an extra RTT on ~half of mkdirs. Send dirs
     * to a dual-host so both groups are local. */
    int rc = S_ISDIR(mode)
                 ? rpc_send_recv_dual(EFS_MSG_INODE_CREATE, buf, slen,
                                      EFS_MSG_INODE_CREATE_REPLY, &r,
                                      sizeof(r), 0)
                 : rpc_send_recv_owner(parent, EFS_MSG_INODE_CREATE, buf,
                                       slen, EFS_MSG_INODE_CREATE_REPLY,
                                       &r, sizeof(r));
    if (env_opid_trace && S_ISDIR(mode))
        fprintf(stderr,"opid-mkdir: parent=%llu name=%s seq=%llu transport_rc=%d status=%u\n",
            (unsigned long long)parent,name?name:"",(unsigned long long)q.id.seq,
            rc,rc==EFS_OK?r.status:255u);
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (r.inode.parent != parent ||
        (name && strncmp(r.inode.name, name, EFS_MAX_NAME - 1) != 0))
        fprintf(stderr,
                "rpc-create: REPLY MISMATCH req parent=%llu name=%s -> "
                "got ino=%llu parent=%llu name=%s mode=%o\n",
                (unsigned long long)parent, name ? name : "",
                (unsigned long long)r.inode.ino,
                (unsigned long long)r.inode.parent, r.inode.name,
                r.inode.mode);
    if (out_ino)
        *out_ino = r.inode.ino;
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Remember established participant floors, not ACTIVE authority or ticket
 * permission. Every mutation still validates its server-side fence; PUT
 * STATUS/ADMIT additionally checks fresh owner authority on every request. */
int efs_client_session_admit(uint32_t shard,const struct efs_opid *identity)
{
    if(shard>=4096 || !identity || !identity->session_epoch)return EFS_ERR_INVAL;
    unsigned nonzero=0;
    for(unsigned i=0;i<EFS_OPID_UUID_LEN;i++)nonzero|=identity->client_uuid[i];
    if(!nonzero)return EFS_ERR_INVAL;
    uint32_t byte=shard/8;uint8_t bit=(uint8_t)(1u<<(shard%8));
    pthread_mutex_lock(&opid_mu);
    int same=g_client.append_session_epoch==identity->session_epoch &&
        !memcmp(g_client.append_session_uuid,identity->client_uuid,EFS_OPID_UUID_LEN);
    int established=same && (g_client.append_session_shards[byte]&bit);
    pthread_mutex_unlock(&opid_mu);
    if(established)return EFS_OK;
    int rc=efs_client_rpc_publication_session(shard,identity);
    if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(&opid_mu);
    if(g_client.append_session_epoch!=identity->session_epoch ||
       memcmp(g_client.append_session_uuid,identity->client_uuid,EFS_OPID_UUID_LEN)) {
        memset(g_client.append_session_shards,0,sizeof(g_client.append_session_shards));
        memcpy(g_client.append_session_uuid,identity->client_uuid,EFS_OPID_UUID_LEN);
        g_client.append_session_epoch=identity->session_epoch;
    }
    g_client.append_session_shards[byte]|=bit;
    pthread_mutex_unlock(&opid_mu);return EFS_OK;
}

int efs_client_create_begin(struct efs_client_create_request *r,efs_ino_t parent,
                             const char *name,uint32_t mode,uid_t uid,gid_t gid)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    if(!r || !parent || !name || !name[0] ||
       strnlen(name,EFS_MAX_NAME)>=EFS_MAX_NAME || !S_ISREG(mode))return EFS_ERR_INVAL;
    if(r->active)return EFS_ERR_BUSY;
    struct efs_opid_req q;int slot=opid_begin(&q);
    if(slot<0)return EFS_ERR_BUSY;
    if(env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=RETAINED_CREATE parent=%llu name=%s\n",
            (unsigned long long)q.id.seq,(unsigned long long)parent,name);
    memset(r,0,sizeof(*r));r->slot=slot;r->active=1;
    r->wire.q=q;r->wire.create.export_id=g_client.export_id;
    r->wire.create.parent=parent;r->wire.create.mode=mode;
    r->wire.create.uid=uid;r->wire.create.gid=gid;
    r->wire.create.flags=EFS_CREATE_F_PENDING;
    memcpy(r->wire.create.name,name,strlen(name)+1);
    return EFS_OK;
}

static int create_request_rpc(struct efs_client_create_request *r,int probe)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_trace_s = -1;
    int env_opid_trace = __atomic_load_n(&env_opid_trace_s, __ATOMIC_ACQUIRE);
    if (env_opid_trace < 0) {
        env_opid_trace = getenv("EFS_OPID_TRACE") != NULL;
        __atomic_store_n(&env_opid_trace_s, env_opid_trace, __ATOMIC_RELEASE);
    }
    struct efs_msg_create_retained request=r->wire;
    request.probe=probe;
    struct efs_msg_create_retained_reply reply={0};
    int rc=rpc_send_recv_owner(request.create.parent,EFS_MSG_CREATE_RETAINED,
        &request,sizeof(request),EFS_MSG_CREATE_RETAINED_REPLY,&reply,sizeof(reply));
    if(rc!=EFS_OK)return rc;
    if(reply.known>1 || reply.routed>1 || (reply.known && !reply.routed) ||
       (reply.routed && reply.shard>=EFS_SESSION_BITS) ||
       (r->wire.routed && reply.routed && r->wire.shard!=reply.shard))return EFS_ERR_PROTO;
    if(reply.routed){r->wire.routed=1;r->wire.shard=reply.shard;}
    rc=rpc_status_to_efs(reply.rpc.status);
    /* A successful submission without a replay receipt is still ambiguous.
     * Keep the same identity/slot and retry until durable terminal proof;
     * returning OK here makes FUSE stop its retry loop before completion. */
    if(!reply.known && rc==EFS_OK)rc=EFS_ERR_BUSY;
    if(reply.known) {
        if(rc==EFS_ERR_BUSY || rc==EFS_ERR_NOT_PRIMARY ||
           (rc==EFS_OK && (!reply.rpc.inode.ino || !reply.generation)))return EFS_ERR_PROTO;
        r->inode=reply.rpc.inode;r->generation=reply.generation;
        r->complete=1;r->result=rc;opid_end(r->slot);r->slot=-1;
    }
    if(r->attempted && !reply.known && env_opid_trace)
        fprintf(stderr,"opid-create: unresolved seq=%llu parent=%llu shard=%u routed=%u rc=%d probe=%d\n",
            (unsigned long long)r->wire.q.id.seq,(unsigned long long)r->wire.create.parent,
            r->wire.shard,r->wire.routed,rc,probe);
    return rc;
}

static int create_request_revocation(struct efs_client_create_request *r,int rc)
{
    if(!r->complete && (rc==EFS_ERR_STALE || rc==EFS_ERR_FENCED || rc==EFS_ERR_NOT_FOUND)) {
        int revoked=0;
        if(efs_client_rpc_identity_revoked(&r->wire.q.id,&revoked)==EFS_OK && revoked) {
            r->revoked=1;r->complete=1;r->result=EFS_ERR_STALE;
            opid_end(r->slot);r->slot=-1;return EFS_ERR_STALE;
        }
    }
    return rc;
}

int efs_client_create_resume(struct efs_client_create_request *r,int probe_only)
{
    if(!r || !r->active || probe_only<0 || probe_only>1)return EFS_ERR_INVAL;
    if(r->complete)return r->result;
    int rc=create_request_rpc(r,1);
    if(r->complete || probe_only || rc!=EFS_ERR_BUSY)return create_request_revocation(r,rc);
    /* The parent read-index may be BUSY before the server can resolve the
     * dentry shard. This is a valid unresolved probe, not malformed proof.
     * Retain the identity and let the caller retry without submitting. */
    if(!r->wire.routed)return EFS_ERR_BUSY;
    if(!r->admitted) {
        rc=efs_client_session_admit(r->wire.shard,&r->wire.q.id);
        if(rc!=EFS_OK)return create_request_revocation(r,rc);
        r->admitted=1;
    }
    opid_refresh_ack(&r->wire.q);
    r->attempted=1;
    return create_request_revocation(r,create_request_rpc(r,0));
}

int efs_client_create_reset(struct efs_client_create_request *r)
{
    if(!r)return EFS_ERR_INVAL;
    if(r->active && !r->complete)return EFS_ERR_BUSY;
    memset(r,0,sizeof(*r));return EFS_OK;
}
int efs_client_create_cancel(struct efs_client_create_request *r)
{
    if(!r)return EFS_ERR_INVAL;
    if(r->active && r->attempted && !r->complete)return EFS_ERR_BUSY;
    if(r->active && !r->complete)opid_end(r->slot);
    memset(r,0,sizeof(*r));return EFS_OK;
}

int efs_client_rpc_getattr(efs_export_id_t export_id, efs_ino_t ino,
                           struct efs_inode *out)
{
    struct efs_msg_inode_getattr req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_GETATTR, &req, sizeof(req),
                                 EFS_MSG_INODE_GETATTR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    rpc_learn_leader(&r);
    efs_client_epoch_note(ino, r.inode.content_epoch);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Raft-mode readdir: the KV scans a dir in NAME order, so pagination uses
 * the server's (src, name) resume cookie, not an ino cursor (name order !=
 * ino order — an ino filter skips entries). src_io/name_io are in/out:
 * pass 0/"" for the first page, then the previous reply's cookie verbatim.
 * done_out=1 when the scan is exhausted. */
int efs_client_rpc_readdir_cur(efs_export_id_t export_id, efs_ino_t parent,
                               struct efs_inode *ents, uint32_t *inout_count,
                               uint32_t *src_io, char *name_io,
                               uint32_t *done_out)
{
    struct efs_msg_inode_readdir req;
    struct efs_conn *conn;
    efs_node_id_t nid = 0;
    uint8_t rtype = 0;
    void *payload = NULL;
    uint32_t plen = 0;
    uint32_t shard = efs_kv_inode_shard(parent);
    int rc;

    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    if (inout_count && !*inout_count)
        return EFS_ERR_INVAL;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    req.max_ents = inout_count ? *inout_count : EFS_READDIR_MAX;
    if (req.max_ents > EFS_READDIR_MAX)
        req.max_ents = EFS_READDIR_MAX;
    req.after_src = src_io ? *src_io : 0;
    if (name_io)
        strncpy(req.after_name, name_io, EFS_MAX_NAME - 1);
    int prof = rpc_prof_enabled();
    unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
    conn = rpc_owner_conn_shard(shard, &nid);
    unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
    if (!conn)
        return EFS_ERR_NET;
    if (efs_conn_send_msg(conn, EFS_MSG_INODE_READDIR, &req, sizeof(req)) != 0) {
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
    rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
    unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
    if (rc != 0) {
        free(payload);
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    efs_client_conn_release(nid, conn);
    if (efs_client_rpc_past_deadline()) {
        free(payload);
        return EFS_ERR_BUSY;
    }
    rpc_prof_add(EFS_MSG_INODE_READDIR, t1 - t0, t2 - t1, t3 - t2, 0);
    if (rtype != EFS_MSG_INODE_READDIR_REPLY ||
        plen < sizeof(struct efs_msg_inode_readdir_reply)) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    {
        struct efs_msg_inode_readdir_reply *r = payload;
        uint32_t n;
        if (r->status != EFS_INODE_RPC_OK) {
            int st = rpc_status_to_efs(r->status);
            free(payload);
            return st;
        }
        if (r->count > req.max_ents || r->next_done > 1 ||
            !memchr(r->next_name, 0, sizeof(r->next_name)) ||
            (!r->next_done && r->next_src == req.after_src &&
             !strcmp(r->next_name, req.after_name))) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        n = r->count;
        if (inout_count && n > *inout_count)
            n = *inout_count;
        if (ents && n)
            memcpy(ents, r->ents, n * sizeof(ents[0]));
        if (inout_count)
            *inout_count = n;
        if (src_io)
            *src_io = r->next_src;
        if (name_io) {
            strncpy(name_io, r->next_name, EFS_MAX_NAME - 1);
            name_io[EFS_MAX_NAME - 1] = 0;
        }
        if (done_out)
            *done_out = r->next_done;
    }
    free(payload);
    return EFS_OK;
}

int efs_client_rpc_writer_view(efs_ino_t ino, uint64_t gen, uint32_t ci,
                                struct efs_msg_inode_writer_view_reply *out)
{
    if (!ino || !out)
        return EFS_ERR_INVAL;
    struct efs_msg_inode_writer_view req = {ino, gen, ci, 0};
    uint32_t shard = efs_kv_inode_shard(ino);
    efs_node_id_t target = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = target;
        struct efs_conn *conn = target ? efs_client_conn_get(target) :
                                         rpc_owner_conn_shard(shard, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_INODE_WRITER_VIEW, &req, sizeof(req))) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t type = 0;
        uint32_t len = 0;
        void *payload = NULL;
        if (efs_conn_recv_msg(conn, &type, &payload, &len)) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (type != EFS_MSG_INODE_WRITER_VIEW_REPLY || len != sizeof(*out)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        struct efs_msg_inode_writer_view_reply reply;
        memcpy(&reply, payload, sizeof(reply));
        free(payload);
        if (reply.status == EFS_INODE_RPC_NOT_PRIMARY && reply.primary_id &&
            reply.primary_id != nid) {
            target = reply.primary_id;
            continue;
        }
        if (reply.status == EFS_INODE_RPC_BUSY) {
            if (attempt == 7 || rpc_writer_retry_pause(attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        int rc = rpc_status_to_efs(reply.status);
        if (rc != EFS_OK)
            return rc;
        rc = efs_writer_view_reply_valid(&req, &reply);
        if (rc == EFS_OK)
            *out = reply;
        return rc;
    }
    return EFS_ERR_BUSY;
}

static int rpc_writer_retry_pause(unsigned attempt)
{
    unsigned delay = 50000u << (attempt < 4 ? attempt : 4);
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    if (deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
        if (now >= deadline)
            return EFS_ERR_BUSY;
        uint64_t remaining = deadline - now;
        if (remaining < (delay + 999u) / 1000u)
            delay = (unsigned)(remaining * 1000u);
    }
    usleep(delay);
    return efs_client_rpc_past_deadline() ? EFS_ERR_BUSY : EFS_OK;
}

/* Raw session replies carry an EFS rc, not an inode status byte. Keep this
 * separate from rpc_send_recv_shard so a Raft rc cannot be misinterpreted. */
static int session_command_reply(uint32_t shard,const uint8_t *command,uint32_t bytes,
    uint64_t *known,int probe)
{
    uint8_t body[EFS_MD_SESS_LEASE_EDGE_LEN+1];
    if(known)*known=0;
    if (bytes>sizeof(body)-1) return EFS_ERR_INVAL;
    if(probe==2 && (!known || bytes!=22 || command[0]!=EFS_MD_CMD_SESSION ||
                    command[1]!=0))return EFS_ERR_INVAL;
    body[0]=efs_raft_shard_group(shard);memcpy(body+1,command,bytes);
    efs_node_id_t target=0,skip=0;
    int lost=0;
    for (unsigned attempt=0;attempt<8;attempt++) {
        if (efs_client_rpc_past_deadline()) return lost?EFS_ERR_NET:EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):
            raft_voter_conn_skip(shard,&nid,skip);
        uint8_t type=0;uint32_t len=0;void *payload=NULL;
        /* K34: a group leader in orderly stop closes its connections; one
         * failed exchange is not the verdict. Re-pick another voter after a
         * pause (the identical identified command: same UUID, epoch, seq),
         * as inode RPCs do; NET only once the attempts or the budget are
         * used up, so the caller keeps the request as unknown. */
        if (!conn || efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,body,bytes+1) ||
            efs_conn_recv_msg(conn,&type,&payload,&len)) {
            free(payload);
            if (conn) efs_client_conn_drop(nid,conn);
            rpc_retry_peer(shard,nid,&target,&skip,conn!=NULL);
            lost=1;
            if (rpc_writer_retry_pause(attempt)!=EFS_OK) return EFS_ERR_NET;
            continue;
        }
        efs_client_conn_release(nid,conn);
        if (type!=EFS_MSG_RAFT_MKFS_REPLY || len!=sizeof(struct efs_msg_raft_mkfs_reply)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,payload,sizeof(reply));free(payload);
        if (efs_client_rpc_past_deadline()) return EFS_ERR_BUSY;
        /* Session/lease commands name their authoritative group as inode
         * replies do. Retain only a validated successful routing hint; this
         * cache is never session authority or proof of a stored verdict. */
        if (reply.rc==EFS_OK && reply.leader_hint>=0 &&
            reply.leader_hint<EFS_MAX_NODES)
            rpc_note_leader(body[0],(efs_node_id_t)(reply.leader_hint+1));
        if(probe==2 && reply.rc==EFS_OK){*known=reply.salt;return EFS_OK;}
        if(known && probe!=2) {
            if(reply.salt>1 || (reply.salt &&
               (reply.rc==EFS_ERR_NOT_PRIMARY || reply.rc==EFS_ERR_NO_QUORUM)))
                return EFS_ERR_PROTO;
            if(reply.salt) {*known=1;return reply.rc;}
            if(reply.rc==EFS_OK)return EFS_ERR_PROTO; /* no proof of stored result */
            if(probe && reply.rc==EFS_ERR_BUSY)return EFS_ERR_BUSY;
        }
        if (reply.rc==EFS_ERR_NOT_PRIMARY) {
            if (reply.leader_hint < -1 || reply.leader_hint >= EFS_MAX_NODES) return EFS_ERR_PROTO;
            target=reply.leader_hint>=0 && (efs_node_id_t)(reply.leader_hint+1)!=nid ?
                (efs_node_id_t)(reply.leader_hint+1):0;
        } else if (reply.rc!=EFS_ERR_BUSY && reply.rc!=EFS_ERR_NO_QUORUM) {
            if (reply.rc==EFS_ERR_FENCED) efs_client_session_fence(reply.rc);
            return reply.rc;
        }
        if (rpc_writer_retry_pause(attempt)!=EFS_OK) return EFS_ERR_BUSY;
    }
    return lost?EFS_ERR_NET:EFS_ERR_BUSY;
}

int efs_client_rpc_identity_revoked(const struct efs_opid *id,int *revoked)
{
    if(!revoked)return EFS_ERR_INVAL;
    *revoked=0;
    if(!id || !id->session_epoch)return EFS_ERR_INVAL;
    unsigned nonzero=0;for(unsigned i=0;i<EFS_OPID_UUID_LEN;i++)nonzero|=id->client_uuid[i];
    if(!nonzero)return EFS_ERR_INVAL;
    uint8_t query[22]={EFS_MD_CMD_SESSION,0};
    memcpy(query+2,id->client_uuid,EFS_OPID_UUID_LEN);
    uint64_t view=0;
    int rc=session_command_reply(efs_kv_session_shard(id->client_uuid),query,sizeof(query),&view,2);
    if(rc!=EFS_OK)return rc;
    uint32_t epoch=(uint32_t)view;unsigned state=(unsigned)((view>>32)&255u);
    if((view>>41) || !epoch || (state!=EFS_SESSION_ACTIVE && state!=EFS_SESSION_FENCING))
        return EFS_ERR_PROTO;
    *revoked=state==EFS_SESSION_ACTIVE && epoch>id->session_epoch;
    return EFS_OK;
}

static int publication_session_command(uint32_t shard,const uint8_t *command,uint32_t bytes)
{return session_command_reply(shard,command,bytes,NULL,0);}

static void lease_command_be(uint8_t *out,uint64_t value,unsigned bytes)
{while(bytes){out[--bytes]=(uint8_t)value;value>>=8;}}

/* Identity, generation and body belong to the caller across ambiguity. Probe
 * never registers a session or invents an edge. known=1 is durable result proof;
 * an ordinary error with known=0 is not permission to forget pending ownership. */
int efs_client_rpc_lease_edge(efs_ino_t ino,uint64_t generation,
    const struct efs_opid_req *q,unsigned flags,uint64_t pending_token,
    int probe,uint64_t *known)
{
    if(!known)return EFS_ERR_INVAL;
    *known=0;
    if(!ino || !generation || !q || !efs_opid_req_valid(q) ||
       !q->id.session_epoch || q->ack>=q->id.seq || probe<0 || probe>1 ||
       !flags || (flags&~7u) || (flags&3u)==3u ||
       !!(flags&EFS_LEASE_EDGE_CLEAR_PENDING)!=!!pending_token)return EFS_ERR_INVAL;
    uint8_t command[EFS_MD_SESS_LEASE_EDGE_LEN]={EFS_MD_CMD_SESSION,
        probe?EFS_MD_SESS_LEASE_PROBE_DEFERRED:EFS_MD_SESS_LEASE_EDGE_DEFERRED};
    memcpy(command+2,q->id.client_uuid,EFS_OPID_UUID_LEN);
    lease_command_be(command+18,q->id.session_epoch,4);
    lease_command_be(command+22,q->id.seq,8);lease_command_be(command+30,q->ack,8);
    lease_command_be(command+38,ino,8);lease_command_be(command+46,generation,8);
    lease_command_be(command+54,flags,4);lease_command_be(command+58,pending_token,8);
    return session_command_reply(efs_kv_inode_shard(ino),command,sizeof(command),known,probe);
}

/* Call before byte admission. CREATE does not reset an existing session;
 * REGISTER and host-side ESTABLISH check its authoritative epoch/fence.
 * Retry the same UUID/epoch after an ambiguous reply, never mint a replacement
 * identity to bypass a fence. Queries and retirement do not call this helper. */
/* ---- Mount session lifecycle (K5, I23) -------------------------------
 * One session per mount: a random uuid at epoch 1, created on its owner
 * shard before the first mutation. A touched shard is admitted (REGISTER on
 * the owner, then ESTABLISH on the shard) before the client's first
 * mutation there and cached per shard; an UNREGISTERED refusal drops the
 * cached bit and admits again. A FENCED refusal, or an owner that no longer
 * knows or has moved past this epoch, latches the mount fenced: mutations
 * then fail locally and are never resent under the old epoch. Re-mount to
 * obtain a new session. */
void efs_client_session_fence(int rc)
{
    /* Every access to both latches is atomic (S3): the exchange makes the
     * first fencer, and only it, log; checkers read without opid_mu. */
    int first=!__atomic_exchange_n(&g_client.session_fenced,1,__ATOMIC_ACQ_REL);
    if (first) {
        char hex[2*EFS_OPID_UUID_LEN+1];
        for (unsigned i=0;i<EFS_OPID_UUID_LEN;i++)
            snprintf(hex+2*i,3,"%02x",g_client.opid_uuid[i]);
        fprintf(stderr,"efs-client: session %s epoch %u fenced (%s); "
                "mutations now fail EIO until re-mount\n",hex,g_client.opid_epoch,
                efs_strerror(rc));
    }
}

int efs_client_session_fenced(void)
{
    return __atomic_load_n(&g_client.session_fenced,__ATOMIC_ACQUIRE);
}

void efs_client_session_forget(uint32_t shard)
{
    if (shard>=4096) return;
    pthread_mutex_lock(&opid_mu);
    g_client.append_session_shards[shard/8]&=(uint8_t)~(1u<<(shard%8));
    pthread_mutex_unlock(&opid_mu);
}

/* Owner view: (epoch, state) of this uuid. NOT_FOUND = no session. */
static int session_owner_view(const struct efs_opid *id,uint32_t shard,uint64_t *view)
{
    uint8_t query[22]={EFS_MD_CMD_SESSION,0};
    memcpy(query+2,id->client_uuid,EFS_OPID_UUID_LEN);
    lease_command_be(query+18,shard,4);
    *view=0;
    int rc=session_command_reply(efs_kv_session_shard(id->client_uuid),query,
                                 sizeof(query),view,2);
    if(rc!=EFS_OK)return rc;
    uint32_t epoch=(uint32_t)*view;unsigned state=(unsigned)((*view>>32)&255u);
    if((*view>>41) || !epoch || (state!=EFS_SESSION_ACTIVE && state!=EFS_SESSION_FENCING))
        return EFS_ERR_PROTO;
    return EFS_OK;
}

int efs_client_session_start(void)
{
    struct efs_opid id;
    int rc=efs_client_publication_id(1,&id); /* seeds the mount identity */
    if(rc!=EFS_OK)return rc;
    if(efs_client_session_fenced())return EFS_ERR_FENCED;
    if(__atomic_load_n(&g_client.session_started,__ATOMIC_ACQUIRE))return EFS_OK;
    uint32_t owner=efs_kv_session_shard(id.client_uuid);
    uint64_t view=0;
    rc=session_owner_view(&id,owner,&view);
    if(rc==EFS_ERR_NOT_FOUND) {
        /* CREATE is a no-op when the same (uuid, epoch) already exists, so
         * an ambiguous first reply is safe to repeat; any other epoch for
         * this uuid is refused by the owner. */
        uint8_t command[22]={EFS_MD_CMD_SESSION,EFS_MD_SESS_CREATE};
        memcpy(command+2,id.client_uuid,EFS_OPID_UUID_LEN);
        lease_command_be(command+18,id.session_epoch,4);
        rc=publication_session_command(owner,command,sizeof(command));
        if(rc==EFS_OK)rc=session_owner_view(&id,owner,&view);
    }
    if(rc!=EFS_OK)return rc;
    if((uint32_t)view!=id.session_epoch ||
       ((view>>32)&255u)!=EFS_SESSION_ACTIVE) {
        efs_client_session_fence(EFS_ERR_FENCED);
        return EFS_ERR_FENCED;
    }
    __atomic_store_n(&g_client.session_started,1,__ATOMIC_RELEASE);
    return EFS_OK;
}

int efs_client_session_check(void)
{
    if(efs_client_session_fenced())return EFS_ERR_FENCED;
    if(__atomic_load_n(&g_client.session_started,__ATOMIC_ACQUIRE))return EFS_OK;
    return efs_client_session_start();
}


/* REGISTER/ESTABLISH refuse an epoch that is no longer the owner's ACTIVE
 * epoch, or that lies below the participant's rejection floor, with STALE:
 * for this mount that is a fence. */
static int admission_verdict(int rc)
{
    if (rc==EFS_ERR_STALE || rc==EFS_ERR_FENCED) {
        efs_client_session_fence(EFS_ERR_FENCED);
        return EFS_ERR_FENCED;
    }
    return rc;
}

int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *identity)
{
    if (!identity || shard>=4096 || !identity->session_epoch) return EFS_ERR_INVAL;
    unsigned nonzero=0;
    for (unsigned i=0;i<EFS_OPID_UUID_LEN;i++) nonzero|=identity->client_uuid[i];
    if (!nonzero) return EFS_ERR_INVAL;
    int rc=efs_client_session_check();
    if (rc!=EFS_OK) return rc;
    uint8_t command[26]={EFS_MD_CMD_SESSION,0};
    memcpy(command+2,identity->client_uuid,EFS_OPID_UUID_LEN);
    uint32_t owner=efs_kv_session_shard(identity->client_uuid);
    uint64_t view=0;
    rc=session_owner_view(identity,shard,&view);
    int registered=0;
    if(rc==EFS_ERR_NOT_FOUND) {
        /* The session this mount created is gone (re-format or cleanup):
         * never create a replacement under the same identity. */
        efs_client_session_fence(EFS_ERR_FENCED);
        return EFS_ERR_FENCED;
    }
    if(rc!=EFS_OK)return rc;
    {
        uint32_t epoch=(uint32_t)view;unsigned state=(unsigned)((view>>32)&255u);
        if(epoch!=identity->session_epoch) {
            efs_client_session_fence(EFS_ERR_FENCED);
            return EFS_ERR_FENCED;
        }
        if(state==EFS_SESSION_FENCING)return EFS_ERR_BUSY;
        registered=!!(view&(1ull<<40));
    }
    lease_command_be(command+18,identity->session_epoch,4);
    lease_command_be(command+22,shard,4);
    if(!registered && efs_raft_shard_group(owner)==efs_raft_shard_group(shard)) {
        /* One replicated transition checks the owner epoch and participant
         * rejection floor atomically. Cross-group admission must still register
         * durably before establishing the participant. */
        command[1]=EFS_MD_SESS_REGISTER_ESTABLISH;
        return admission_verdict(publication_session_command(shard,command,sizeof(command)));
    }
    if(!registered) {
        command[1]=EFS_MD_SESS_REGISTER;
        rc=admission_verdict(publication_session_command(owner,command,sizeof(command)));
        if(rc)return rc;
    }
    /* ESTABLISH always rechecks fresh owner authority and the participant's
     * durable rejection floor. This is an RPC-local proof, not a client cache. */
    command[1]=EFS_MD_SESS_ESTABLISH;
    return admission_verdict(publication_session_command(shard,command,sizeof(command)));
}

/* Production ownership primitive: an unknown reply never frees its ACK slot
 * or permits replacing identity/body. Namespace and publication use the same
 * full mount UUID; legacy append/HOLD still need a versioned adoption bridge. */
/* Lease callers own an inode edge stripe. An older retained identity can
 * need that stripe to finish: waiting for its receipt capacity here would
 * keep both operations blocked until the new caller's deadline. Allocate
 * once, or leave q invalid so the caller can release its stripe and retry.
 * This changes no identity, ACK, capacity or inherited deadline. */
static int opid_try_begin(struct efs_opid_req *q)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    memset(q,0,sizeof(*q));
    if(efs_client_rpc_past_deadline())return -1;
    pthread_mutex_lock(&opid_mu);
    int slot=opid_take_locked(q, __builtin_return_address(0));
    pthread_mutex_unlock(&opid_mu);
    if(slot>=0 && env_opid_verbose)
        fprintf(stderr,"opid-begin: seq=%llu ack=%llu slot=%d thread=%lu deadline_ms=%llu\n",
            (unsigned long long)q->id.seq,(unsigned long long)q->ack,slot,
            (unsigned long)pthread_self(),(unsigned long long)efs_client_rpc_deadline_ms());
    return slot;
}

int efs_client_lease_begin(struct efs_client_lease_request *r,efs_ino_t ino,
                           uint64_t generation,unsigned flags,uint64_t token)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    if (!r || !ino || !generation || !flags || (flags & ~7u) ||
        (flags & 3u) == 3u ||
        !!(flags & EFS_LEASE_EDGE_CLEAR_PENDING) != !!token)
        return EFS_ERR_INVAL;
    if (r->active) return EFS_ERR_BUSY;
    struct efs_opid_req q;
    int slot = opid_try_begin(&q);
    if (slot < 0) return EFS_ERR_BUSY;
    if(env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=LEASE ino=%llu flags=%u\n",
            (unsigned long long)q.id.seq,(unsigned long long)ino,flags);
    *r = (struct efs_client_lease_request){.q=q,.ino=ino,
        .generation=generation,.pending_token=token,.flags=flags,
        .slot=slot,.active=1};
#if EFS_FAULTS
    /* Corrupt only the guarded request's FileID before its first send. The
     * caller retains CREATE's correct generation for pending-only cleanup. */
    const char *marker=getenv("EFS_FAULT_CREATE_ADOPT_NEGATIVE_FILE");
    if (flags==(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING) &&
        marker && unlink(marker)==0) {
        r->generation=generation==UINT64_MAX ? generation-1 : generation+1;
        fprintf(stderr,"test-create-adopt-negative-request ino=%llu generation=%llu original=%llu\n",
            (unsigned long long)ino,(unsigned long long)r->generation,(unsigned long long)generation);
    }
#endif
    return EFS_OK;
}

static int lease_request_result(struct efs_client_lease_request *r,int rc,
                                 uint64_t known)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_trace_s = -1;
    int env_opid_trace = __atomic_load_n(&env_opid_trace_s, __ATOMIC_ACQUIRE);
    if (env_opid_trace < 0) {
        env_opid_trace = getenv("EFS_OPID_TRACE") != NULL;
        __atomic_store_n(&env_opid_trace_s, env_opid_trace, __ATOMIC_RELEASE);
    }
    if(!known && env_opid_trace)
        fprintf(stderr,"opid-lease: unresolved seq=%llu ino=%llu flags=%u rc=%d\n",
            (unsigned long long)r->q.id.seq,(unsigned long long)r->ino,r->flags,rc);
    if (known == 1) {
        r->complete=1;
        r->result=rc;
        opid_end(r->slot);
        r->slot=-1;
    }
    if(!r->complete && (rc==EFS_ERR_STALE || rc==EFS_ERR_FENCED || rc==EFS_ERR_NOT_FOUND)) {
        int revoked=0;
        if(efs_client_rpc_identity_revoked(&r->q.id,&revoked)==EFS_OK && revoked) {
            r->revoked=1;r->complete=1;r->result=EFS_ERR_STALE;
            opid_end(r->slot);r->slot=-1;return EFS_ERR_STALE;
        }
    }
    return rc;
}

int efs_client_lease_resume(struct efs_client_lease_request *r,int probe_only)
{
    if (!r || !r->active || probe_only<0 || probe_only>1)
        return EFS_ERR_INVAL;
    if (r->complete) return r->result;
    uint64_t known=0;
    int rc;
    if (r->attempted || probe_only) {
        rc=efs_client_rpc_lease_edge(r->ino,r->generation,&r->q,r->flags,
                                     r->pending_token,1,&known);
        if (known || probe_only || rc!=EFS_ERR_BUSY)
            return lease_request_result(r,rc,known);
        /* Unknown probe is not success; retry only the retained exact edge. */
    }
    if (!r->admitted) {
        /* Reuse only a completed establishment for this exact UUID, epoch
         * and shard. The guarded edge still validates server revocation. */
        rc=efs_client_session_admit(efs_kv_inode_shard(r->ino),&r->q.id);
        if (rc!=EFS_OK) return lease_request_result(r,rc,0);
        r->admitted=1;
    }
    opid_refresh_ack(&r->q);
    r->attempted=1;
    rc=efs_client_rpc_lease_edge(r->ino,r->generation,&r->q,r->flags,
                                 r->pending_token,0,&known);
#if EFS_FAULTS
    /* Private functional gate: hide one consumed durable combined-adoption
     * reply. Leave the exact request active so normal cleanup must prove its
     * receipt before compensating CLOSE; no descriptor was acknowledged. */
    const char *marker=getenv("EFS_FAULT_CREATE_ADOPT_REPLY_FILE");
    if (rc==EFS_OK && known==1 &&
        r->flags==(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING) &&
        marker && unlink(marker)==0) {
        fprintf(stderr,"test-create-adopt-reply-loss seq=%llu ino=%llu known=1 hidden=1\n",
                (unsigned long long)r->q.id.seq,(unsigned long long)r->ino);
        return lease_request_result(r,EFS_ERR_NET,0);
    }
#endif
    return lease_request_result(r,rc,known);
}

int efs_client_lease_reset(struct efs_client_lease_request *r)
{
    if (!r) return EFS_ERR_INVAL;
    if (r->active && !r->complete) return EFS_ERR_BUSY;
    memset(r,0,sizeof(*r));
    return EFS_OK;
}

/* probe_only 0: ADMIT, 1: STATUS, 2: SETTLE of n tickets of one stream
 * (*state = how many leading tickets are retired); K32: 3: ADMIT_BATCH,
 * 4: SETTLE_BATCH of 2..EFS_PUT_TICKET_BATCH_MAX tickets of one group
 * (*state = bit i set when ticket i is ADMITTED / retired). */
static int rpc_put_ticket(const struct efs_put_ticket *ticket,uint32_t n,int probe_only,
                          uint32_t *state)
{
    if(!state)return EFS_ERR_INVAL;
    *state=0;
    if(probe_only<0 || probe_only>4 || !n ||
       n>(probe_only==2?EFS_PUT_TICKET_SETTLE_MAX:probe_only>2?EFS_PUT_TICKET_BATCH_MAX:1u) ||
       (probe_only>2 && n<2))
        return EFS_ERR_INVAL;
    if(probe_only!=1) {
        int ready=efs_client_session_check();
        if(ready!=EFS_OK)return ready;
    }
    _Static_assert(EFS_PUT_TICKET_BATCH_MAX<=32u,"batch reply mask is 32 bits");
    uint8_t body[4+(EFS_PUT_TICKET_SETTLE_MAX>EFS_PUT_TICKET_BATCH_MAX?
                    EFS_PUT_TICKET_SETTLE_MAX:EFS_PUT_TICKET_BATCH_MAX)*EFS_PUT_TICKET_WIRE_LEN];
    uint32_t len;
    uint8_t *at=body+2;
    if(probe_only>=2){body[2]=(uint8_t)(n>>8);body[3]=(uint8_t)n;at=body+4;}
    for(uint32_t i=0;i<n;i++){
        int rc=efs_put_ticket_encode(&ticket[i],at+i*EFS_PUT_TICKET_WIRE_LEN);if(rc!=EFS_OK)return rc;
    }
    len=(uint32_t)(at-body)+n*EFS_PUT_TICKET_WIRE_LEN;
    uint32_t shard=efs_kv_lane_shard(ticket->ino,ticket->ci%EFS_META_LANES);
    body[0]=efs_raft_shard_group(shard);
    static const uint8_t tag[5]={EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_MD_REQ_PUT_TICKET_STATUS,
        EFS_MD_CMD_PUT_TICKET_SETTLE,EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH,
        EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH};
    body[1]=tag[probe_only];
    efs_node_id_t target=0,skip=0;
    int lost=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        if(efs_client_rpc_past_deadline())return lost?EFS_ERR_NET:EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):raft_voter_conn_skip(shard,&nid,skip);
        uint8_t type=0;uint32_t bytes=0;void *payload=NULL;
        /* K39: the lane group's leader in orderly stop (a member down) closes
         * its connections; one lost exchange is not the verdict. Every
         * command here is idempotent on the host (ADMIT of an admitted
         * ticket is OK, SETTLE of a retired one is OK, STATUS reads), so
         * re-pick another voter after a pause, the identical body, as
         * session_command_reply does (K34). NET only once the attempts or
         * the budget are used up; the ledger keeps a sent ticket for its
         * settle. It was NET at the first loss: a write placed around the
         * stopped member failed EIO (test_k35_fsync_degraded.py case 7). */
        if(!conn || efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,body,len) ||
           efs_conn_recv_msg(conn,&type,&payload,&bytes)) {
            free(payload);
            if(conn)efs_client_conn_drop(nid,conn);
            rpc_retry_peer(shard,nid,&target,&skip,conn!=NULL);
            lost=1;
            int last=attempt+1==8 || rpc_writer_retry_pause(attempt)!=EFS_OK;
            fprintf(stderr,"put-ticket: lost exchange op=%d shard=%u nid=%u attempt=%u%s\n",
                    probe_only,shard,nid,attempt,last?" -> NET":"; another voter");
            if(last)return EFS_ERR_NET;
            continue;
        }
        efs_client_conn_release(nid,conn);
        if(type!=EFS_MSG_RAFT_MKFS_REPLY || bytes!=sizeof(struct efs_msg_raft_mkfs_reply)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,payload,bytes);free(payload);
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        if(reply.rc==EFS_OK && probe_only>2) {
            if(reply.salt>>n)return EFS_ERR_PROTO;
            *state=(uint32_t)reply.salt;return EFS_OK;
        }
        if(reply.rc==EFS_OK) {
            if(probe_only==2 ? reply.salt>n :
               (reply.salt!=EFS_PUT_TICKET_ADMITTED && reply.salt!=EFS_PUT_TICKET_PUBLISHED &&
                reply.salt!=EFS_PUT_TICKET_REJECTED))
                return EFS_ERR_PROTO;
            if(!probe_only && reply.salt!=EFS_PUT_TICKET_ADMITTED)return EFS_ERR_STALE;
            *state=(uint32_t)reply.salt;return EFS_OK;
        }
        if(reply.salt)return EFS_ERR_PROTO;
        if(reply.rc==EFS_ERR_NOT_PRIMARY) {
            if(reply.leader_hint< -1 || reply.leader_hint>=EFS_MAX_NODES)return EFS_ERR_PROTO;
            target=reply.leader_hint>=0 && (efs_node_id_t)(reply.leader_hint+1)!=nid?
                (efs_node_id_t)(reply.leader_hint+1):0;
        } else if(reply.rc!=EFS_ERR_BUSY && reply.rc!=EFS_ERR_NO_QUORUM) {
            if(reply.rc==EFS_ERR_FENCED)efs_client_session_fence(reply.rc);
            return reply.rc;
        }
        if(rpc_writer_retry_pause(attempt)!=EFS_OK)return EFS_ERR_BUSY;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_put_ticket(const struct efs_put_ticket *ticket,int probe_only,uint32_t *state)
{
    if (!state) return EFS_ERR_INVAL;
    *state=0;
    return probe_only<0 || probe_only>1 ? EFS_ERR_INVAL : rpc_put_ticket(ticket,1,probe_only,state);
}
int efs_client_rpc_put_ticket_settle(const struct efs_put_ticket *tickets,uint32_t n,
                                     uint32_t *retired)
{
    return rpc_put_ticket(tickets,n,2,retired);
}

int efs_client_rpc_put_ticket_batch(const struct efs_put_ticket *tickets,uint32_t n,
                                    int settle,uint32_t *mask)
{
    return rpc_put_ticket(tickets,n,settle?4:3,mask);
}

/* K35: propose the degraded mark of one published object's fragment on the
 * lane shard's group and wait for its verdict. */
int efs_client_rpc_degraded_mark(efs_ino_t ino,uint64_t inode_gen,uint64_t object,
                                 uint32_t ci,uint32_t fi,efs_node_id_t node)
{
    if(!ino || !inode_gen || !object || fi>=EFS_NUM_FRAGMENTS || !node || node>EFS_MAX_NODES)
        return EFS_ERR_INVAL;
    uint8_t body[1+EFS_MD_DEGRADED_MARK_LEN];
    uint32_t shard=efs_kv_lane_shard(ino,(uint8_t)(ci%EFS_META_LANES));
    body[0]=efs_raft_shard_group(shard);
    body[1]=EFS_MD_CMD_DEGRADED_MARK;
    for(int i=0;i<8;i++){
        body[2+i]=(uint8_t)(ino>>(56-8*i));
        body[10+i]=(uint8_t)(inode_gen>>(56-8*i));
        body[18+i]=(uint8_t)(object>>(56-8*i));
    }
    for(int i=0;i<4;i++){
        body[26+i]=(uint8_t)(ci>>(24-8*i));
        body[31+i]=(uint8_t)((uint32_t)node>>(24-8*i));
    }
    body[30]=(uint8_t)fi;
    efs_node_id_t target=0,skip=0;
    int lost=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        if(efs_client_rpc_past_deadline())return lost?EFS_ERR_NET:EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):raft_voter_conn_skip(shard,&nid,skip);
        uint8_t type=0;uint32_t bytes=0;void *payload=NULL;
        /* K39: fsync with a member down marks on the lane group, whose
         * leader may be that member. The host applies a mark once (a
         * repeat adds nothing), so a lost exchange re-picks another voter
         * as rpc_put_ticket does. */
        if(!conn || efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,body,sizeof(body)) ||
           efs_conn_recv_msg(conn,&type,&payload,&bytes)) {
            free(payload);
            if(conn)efs_client_conn_drop(nid,conn);
            rpc_retry_peer(shard,nid,&target,&skip,conn!=NULL);
            lost=1;
            int last=attempt+1==8 || rpc_writer_retry_pause(attempt)!=EFS_OK;
            fprintf(stderr,"degraded-mark: lost exchange ino=%llu ci=%u nid=%u attempt=%u%s\n",
                    (unsigned long long)ino,ci,nid,attempt,last?" -> NET":"; another voter");
            if(last)return EFS_ERR_NET;
            continue;
        }
        efs_client_conn_release(nid,conn);
        if(type!=EFS_MSG_RAFT_MKFS_REPLY || bytes!=sizeof(struct efs_msg_raft_mkfs_reply)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,payload,bytes);free(payload);
        if(reply.rc==EFS_ERR_NOT_PRIMARY) {
            if(reply.leader_hint< -1 || reply.leader_hint>=EFS_MAX_NODES)return EFS_ERR_PROTO;
            target=reply.leader_hint>=0 && (efs_node_id_t)(reply.leader_hint+1)!=nid?
                (efs_node_id_t)(reply.leader_hint+1):0;
        } else if(reply.rc!=EFS_ERR_BUSY && reply.rc!=EFS_ERR_NO_QUORUM) {
            return reply.rc;
        }
        if(rpc_writer_retry_pause(attempt)!=EFS_OK)return EFS_ERR_BUSY;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_publication(const struct efs_msg_publication *request, int query_only,
                               struct efs_msg_publication_reply *out)
{
    struct efs_meta_pub p;uint8_t digest[EFS_HASH_SIZE];
    if(query_only<0 || query_only>2 || !out || !request)return EFS_ERR_INVAL;
    struct efs_msg_publication req=*request;
    memset(out,0,sizeof(*out));
    /* W154: a two-fragment object publishes as degraded (not in the digest). */
    req.rec.publish_flags|=efs_client_degraded_flags(req.rec.ino,req.rec.chunk_index,
                                                     req.rec.chunk_generation);
    if(efs_publication_from_request(&req,&p)!=EFS_OK)return EFS_ERR_INVAL;
    if(efs_publication_digest(&p,digest)!=EFS_OK)return EFS_ERR_INVAL;
    if(!query_only) {
        int ready=efs_client_session_check();
        if(ready!=EFS_OK)return ready;
    }
    uint32_t shard=efs_kv_lane_shard(req.rec.ino,req.rec.chunk_index%EFS_META_LANES);
    efs_node_id_t target=0;
    for(unsigned attempt=0;attempt<8;attempt++) {
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):rpc_owner_conn_shard(shard,&nid);
        if(!conn)return EFS_ERR_NET;
        uint8_t msg=query_only==2?EFS_MSG_PUBLICATION_RETIRE:query_only?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION;
        if(efs_conn_send_msg(conn,msg,&req,sizeof(req))) {
            efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        uint8_t type=0;uint32_t len=0;void *payload=NULL;
        if(efs_conn_recv_msg(conn,&type,&payload,&len)) {
            free(payload);efs_client_conn_drop(nid,conn);return EFS_ERR_NET;
        }
        efs_client_conn_release(nid,conn);
        if(type!=(query_only==2?EFS_MSG_PUBLICATION_RETIRE_REPLY:query_only?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY) || len!=sizeof(*out)) {
            free(payload);return EFS_ERR_PROTO;
        }
        struct efs_msg_publication_reply reply;memcpy(&reply,payload,sizeof(reply));free(payload);
        if(reply.rpc.status==EFS_INODE_RPC_NOT_PRIMARY && reply.rpc.primary_id && reply.rpc.primary_id!=nid) {
            target=reply.rpc.primary_id;continue;
        }
        if(reply.rpc.status==EFS_INODE_RPC_BUSY) {
            if(rpc_writer_retry_pause(attempt)!=EFS_OK)return EFS_ERR_BUSY;
            continue;
        }
        int rc=rpc_status_to_efs(reply.rpc.status);if(rc!=EFS_OK)return rc;
        if ((reply.receipt_index && (!reply.receipt_term ||
             reply.receipt_group!=efs_raft_shard_group(shard))) ||
            (!reply.receipt_index && (reply.receipt_term || reply.receipt_group)))
            return EFS_ERR_PROTO;
        if(memcmp(digest,reply.digest,sizeof(digest)) ||
           (reply.state!=EFS_PUBLICATION_COMMITTED && reply.state!=EFS_PUBLICATION_REJECTED && reply.state!=EFS_PUBLICATION_RETIRED && reply.state!=EFS_PUBLICATION_PENDING && reply.state!=EFS_PUBLICATION_APPLY_PENDING) ||
           (reply.state==EFS_PUBLICATION_RETIRED && reply.verdict!=EFS_META_PUBLICATION_RETIRED) ||
           ((reply.state==EFS_PUBLICATION_PENDING || reply.state==EFS_PUBLICATION_APPLY_PENDING) && reply.verdict!=EFS_META_PUBLICATION_PENDING) ||
           (reply.state==EFS_PUBLICATION_APPLY_PENDING && !reply.receipt_index) ||
           (query_only==2 && reply.state!=EFS_PUBLICATION_RETIRED && reply.state!=EFS_PUBLICATION_PENDING) ||
           (reply.state==EFS_PUBLICATION_COMMITTED && reply.verdict!=EFS_OK) ||
           (reply.state==EFS_PUBLICATION_REJECTED && reply.verdict!=EFS_ERR_STALE && reply.verdict!=EFS_ERR_INVAL &&
            reply.verdict!=EFS_ERR_FENCED && reply.verdict!=EFS_ERR_UNREGISTERED))
            return EFS_ERR_PROTO;
        if(reply.state==EFS_PUBLICATION_REJECTED && reply.verdict==EFS_ERR_FENCED)
            efs_client_session_fence(EFS_ERR_FENCED);
        if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
        if(reply.state==EFS_PUBLICATION_COMMITTED || reply.state==EFS_PUBLICATION_RETIRED)
            efs_client_degraded_drop(req.rec.ino,req.rec.chunk_index,req.rec.chunk_generation);
        *out=reply;return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_lane_writer_view(efs_ino_t ino, uint64_t gen, uint32_t ci, uint32_t cs,
                                struct efs_msg_lane_writer_view_reply *out)
{
    if (!ino || !gen || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    struct efs_msg_lane_writer_view req = {ino, gen, ci, cs};
    uint32_t shard = efs_kv_lane_shard(ino, ci % 64u);
    efs_node_id_t target = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = target;
        struct efs_conn *conn = target ? efs_client_conn_get(target) :
                                         rpc_owner_conn_shard(shard, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_LANE_WRITER_VIEW, &req, sizeof(req))) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t type = 0;
        uint32_t len = 0;
        void *payload = NULL;
        if (efs_conn_recv_msg(conn, &type, &payload, &len)) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (type != EFS_MSG_LANE_WRITER_VIEW_REPLY || len != sizeof(*out)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        struct efs_msg_lane_writer_view_reply reply;
        memcpy(&reply, payload, sizeof(reply));
        free(payload);
        if (reply.view.status == EFS_INODE_RPC_NOT_PRIMARY && reply.view.primary_id &&
            reply.view.primary_id != nid) {
            target = reply.view.primary_id;
            continue;
        }
        if (reply.view.status == EFS_INODE_RPC_BUSY) {
            if (rpc_writer_retry_pause(attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        int rc = rpc_status_to_efs(reply.view.status);
        if (rc != EFS_OK)
            return rc;
        rc = efs_lane_writer_view_reply_valid(&req, &reply);
        if (rc == EFS_OK)
            *out = reply;
        return rc;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_lane_bootstrap(efs_export_id_t export_id, efs_ino_t ino, uint64_t gen, uint32_t ci, uint32_t cs,
                                struct efs_msg_lane_writer_view_reply *out)
{
    if (!ino || !gen || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    struct efs_msg_lane_bootstrap req = {export_id, ino, gen, ci, cs};
    uint32_t shard = efs_kv_inode_shard(ino);
    efs_node_id_t target = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = target;
        struct efs_conn *conn = target ? efs_client_conn_get(target) :
                                         rpc_owner_conn_shard(shard, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_LANE_BOOTSTRAP, &req, sizeof(req))) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t type = 0;
        uint32_t len = 0;
        void *payload = NULL;
        if (efs_conn_recv_msg(conn, &type, &payload, &len)) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        if (type != EFS_MSG_LANE_BOOTSTRAP_REPLY || len != sizeof(*out)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        struct efs_msg_lane_writer_view_reply reply;
        memcpy(&reply, payload, sizeof(reply));
        free(payload);
        if (reply.view.status == EFS_INODE_RPC_NOT_PRIMARY && reply.view.primary_id &&
            reply.view.primary_id != nid) {
            target = reply.view.primary_id;
            continue;
        }
        if (reply.view.status == EFS_INODE_RPC_BUSY) {
            if (rpc_writer_retry_pause(attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        int rc = rpc_status_to_efs(reply.view.status);
        if (rc != EFS_OK)
            return rc;
        struct efs_msg_lane_writer_view validation = {ino, gen, ci, cs};
        rc = efs_lane_writer_view_reply_valid(&validation, &reply);
        if (rc == EFS_OK)
            *out = reply;
        return rc;
    }
    return EFS_ERR_BUSY;
}

int efs_client_rpc_writer_admission_view(efs_export_id_t export_id, efs_ino_t ino,
                                          uint64_t gen, uint32_t ci, uint32_t cs,
                                          struct efs_msg_lane_writer_view_reply *out)
{
    if (!ino || !gen || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    struct efs_msg_lane_writer_view_reply view;
    int rc = efs_client_rpc_lane_writer_view(ino, gen, ci, cs, &view);
    /* Only authoritative absence is a bootstrap request. BUSY/STALE/I/O errors
     * never fall back to an inode lookup or mutate authority implicitly. */
    if (rc == EFS_ERR_NOT_FOUND && !efs_client_rpc_past_deadline())
        rc = efs_client_rpc_lane_bootstrap(export_id, ino, gen, ci, cs, &view);
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    if (rc != EFS_OK)
        return rc;
    struct efs_msg_lane_writer_view req = {ino, gen, ci, cs};
    rc = efs_lane_writer_view_reply_valid(&req, &view);
    if (rc == EFS_OK)
        *out = view;
    return rc;
}

int efs_client_rpc_getchunks_fileid(efs_export_id_t export_id, efs_ino_t ino,
                                    uint64_t generation,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count, uint64_t *resolved_generation)
{
    struct efs_msg_inode_getchunks req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.generation = generation;
    req.start = start;
    req.max = inout_count ? *inout_count : EFS_GETCHUNKS_MAX;
    /* Enter at the inode's shard; the host walks the file's lane shards
     * itself and bounces when it does not host them. */
    uint32_t shard = efs_export_shard_of(ino, EFS_KV_SHARD_BITS);
    struct efs_msg_inode_getchunks_reply *r = NULL;
    void *payload = NULL;
    efs_node_id_t target = 0, skip = 0;
    for (int attempt = 0; attempt < 16; attempt++) {
        if (efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        efs_node_id_t nid = 0;
        int prof = rpc_prof_enabled();
        unsigned long long t0 = prof ? rpc_prof_now_us() : 0;
        struct efs_conn *conn = target ? efs_client_conn_get(target)
                                      : raft_voter_conn_skip(shard, &nid, skip);
        if (target)
            nid = target;
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn || efs_conn_send_msg(conn, EFS_MSG_INODE_GETCHUNKS, &req,
                                      sizeof(req)) != 0) {
            if (conn) efs_client_conn_drop(nid, conn);
            /* This immutable read is safe to repeat after endpoint loss.
             * Drop a dead leader hint and choose a different live voter;
             * preserve FileID, range, outputs and the original deadline. */
            skip = nid; target = 0;
            if (attempt == 15) return EFS_ERR_NET;
            if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        uint8_t rtype = 0;
        uint32_t plen = 0;
        payload = NULL;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            free(payload);
            efs_client_conn_drop(nid, conn);
            payload = NULL;
            skip = nid; target = 0;
            if (attempt == 15) return EFS_ERR_NET;
            if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            continue;
        }
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            return EFS_ERR_BUSY;
        }
        rpc_prof_add(EFS_MSG_INODE_GETCHUNKS, t1 - t0, t2 - t1, t3 - t2, 0);
        /* Variable length: only recs[0..count) are present and read. */
        if (rtype != EFS_MSG_INODE_GETCHUNKS_REPLY ||
            efs_getchunks_reply_len_ok(payload, plen) != EFS_OK) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        r = payload;
        if (r->status == EFS_INODE_RPC_NOT_PRIMARY) {
            efs_node_id_t hint = r->primary_id;
            target = hint && hint != nid ? hint : 0;
            skip = nid;
            free(payload);
            payload = NULL;
            r = NULL;
            if (!target && (attempt == 15 ||
                            rpc_writer_retry_pause((unsigned)attempt) != EFS_OK))
                return EFS_ERR_BUSY;
            continue;
        }
        if (r->status != EFS_INODE_RPC_BUSY)
            break;
        free(payload);
        payload = NULL;
        r = NULL;
        if (attempt == 15 || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
            return EFS_ERR_BUSY;
    }
    if (!r) {
        free(payload);
        return EFS_ERR_BUSY;
    }
    if (r->status != EFS_INODE_RPC_OK) {
        int st = rpc_status_to_efs(r->status);
        free(payload);
        return st;
    }
    if (efs_getchunks_reply_valid(&req, r) != EFS_OK) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    uint32_t n = r->count;
    if (inout_count && n > *inout_count)
        n = *inout_count;
    if (recs && n)
        memcpy(recs, r->recs, n * sizeof(recs[0]));
    if (inout_count)
        *inout_count = n;
    if (resolved_generation)
        *resolved_generation = r->generation;
    efs_client_epoch_note(ino, r->authority_epoch); /* also for an empty reply */
    free(payload);
    return EFS_OK;
}

int efs_client_rpc_getchunks(efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count)
{
    return efs_client_rpc_getchunks_fileid(export_id, ino, 0, start, recs,
                                          inout_count, NULL);
}

int efs_client_rpc_unlink(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, int is_dir)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    uint8_t buf[sizeof(struct efs_msg_inode_unlink) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_unlink *req = (struct efs_msg_inode_unlink *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->parent = parent;
    if (name)
        strncpy(req->name, name, EFS_MAX_NAME - 1);
    req->is_dir = is_dir ? 1 : 0;
    slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    if(env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=%s parent=%llu name=%s\n",
            (unsigned long long)q.id.seq,is_dir?"RMDIR":"UNLINK",
            (unsigned long long)parent,name?name:"");
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_UNLINK, buf, slen,
                                 EFS_MSG_INODE_UNLINK_REPLY, &r, sizeof(r));
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_rename_at(efs_export_id_t export_id, efs_ino_t old_parent,
                             const char *old_name, efs_ino_t new_parent,
                             const char *new_name, struct efs_inode *out)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    uint8_t buf[sizeof(struct efs_msg_inode_rename_at) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_rename_at *req = (struct efs_msg_inode_rename_at *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->old_parent = old_parent;
    req->new_parent = new_parent;
    if (old_name)
        strncpy(req->old_name, old_name, EFS_MAX_NAME - 1);
    if (new_name)
        strncpy(req->new_name, new_name, EFS_MAX_NAME - 1);
    slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    if(env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=RENAME parent=%llu name=%s new_parent=%llu new_name=%s\n",
            (unsigned long long)q.id.seq,(unsigned long long)old_parent,old_name?old_name:"",
            (unsigned long long)new_parent,new_name?new_name:"");
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(old_parent, EFS_MSG_INODE_RENAME_AT, buf,
                                 slen, EFS_MSG_INODE_RENAME_AT_REPLY,
                                 &r, sizeof(r));
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_setattr(efs_export_id_t export_id, efs_ino_t ino,
                           uint32_t mask, uint32_t mode, uid_t uid, gid_t gid,
                           uint64_t size, uint64_t mtime, uint32_t mtime_nsec,
                           uint64_t atime, uint32_t atime_nsec,
                           struct efs_inode *out)
{
    struct efs_msg_inode_setattr req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.mask = mask;
    req.mode = mode;
    req.uid = (uint32_t)uid;
    req.gid = (uint32_t)gid;
    req.size = size;
    req.mtime = mtime;
    req.mtime_nsec = mtime_nsec;
    req.atime = atime;
    req.atime_nsec = atime_nsec;
    struct efs_msg_inode_reply r;
    int rc;
    {
        /* Every SETATTR carries the mount session (K5): the inode shard is
         * admitted first and the apply refuses a fenced or unregistered
         * identity. D25: a truncate's opid also records its result with the
         * effect; an ambiguous failure is resolved only by retrying the same
         * identity, never by issuing a new one. */
        uint8_t buf[sizeof(req) + EFS_OPID_WIRE_LEN];
        struct efs_opid_req q;
        int slot = opid_begin(&q);
        if (slot < 0)
            return EFS_ERR_BUSY;
        rc = EFS_ERR_BUSY;
        for (int t = 0; t < 8 && !efs_client_rpc_past_deadline(); t++) {
            if (t)
                usleep(2000u << (unsigned)(t < 4 ? t : 4));
            opid_refresh_ack(&q);
            memcpy(buf, &req, sizeof(req));
            uint32_t slen = opid_suffix(buf, sizeof(req), &q);
            rc = rpc_send_recv_admitted(efs_kv_inode_shard(ino), &q.id,
                                        EFS_MSG_INODE_SETATTR, buf, slen,
                                        EFS_MSG_INODE_SETATTR_REPLY, &r,
                                        sizeof(r), 0, NULL);
            if (rc == EFS_OK && r.status == EFS_INODE_RPC_BUSY)
                rc = EFS_ERR_BUSY;
            if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM && rc != EFS_ERR_BUSY)
                break;
        }
        opid_end(slot);
    }
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (mask & EFS_SETATTR_SIZE)
        efs_client_epoch_note(ino, r.inode.content_epoch);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* K15/W64 fault build: lose the reply of every Nth granted reservation
 * (EFS_FAULT_APPEND_LOSE_EVERY=N, at most EFS_FAULT_APPEND_LOSE_MAX, default
 * 4). The server has committed the reservation; the client sees a transport
 * loss for that identity, as after a dropped connection, and gives up. */
#if EFS_FAULTS
static int append_reply_fault(const struct efs_opid_req *q, efs_ino_t ino)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static unsigned granted, lost;
    const char *every = getenv("EFS_FAULT_APPEND_LOSE_EVERY");
    const char *max = getenv("EFS_FAULT_APPEND_LOSE_MAX");
    unsigned n = every ? (unsigned)strtoul(every, NULL, 10) : 0;
    unsigned cap = max ? (unsigned)strtoul(max, NULL, 10) : 4;
    if (!n) return 0;
    pthread_mutex_lock(&mu);
    int lose = ++granted % n == 0 && lost < cap;
    if (lose) lost++;
    pthread_mutex_unlock(&mu);
    if (lose) {
        fprintf(stderr, "append-reserve fault ino=%llu seq=%llu lost-reply=1\n",
                (unsigned long long)ino, (unsigned long long)q->id.seq);
        fflush(stderr);
    }
    return lose;
}
#endif

/* Cross-client O_APPEND: reserve the next len bytes at the owner.
 * new_size_out is this reservation's end; start_out is the reserved
 * offset (reply atime) when the host stamped reserved+len. */


int efs_client_rpc_append_reserve(efs_export_id_t export_id, efs_ino_t ino,
                                  uint64_t len, const struct efs_opid_req *q,
                                  uint64_t *new_size_out, uint64_t *start_out)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    uint8_t buf[sizeof(struct efs_msg_inode_append) + EFS_APPEND_ACK_LEN];
    struct efs_msg_inode_append *req = (struct efs_msg_inode_append *)buf;
    uint8_t *su = buf + sizeof(*req);
    struct efs_msg_inode_reply r;
    int rc;

    if (!efs_opid_req_valid(q) || q->ack >= q->id.seq)
        return EFS_ERR_INVAL;
    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->ino = ino;
    req->len = len;
    struct efs_opid_req current = *q;
    /* BUSY may have released the per-inode append lock. Refresh only the
     * retirement watermark; preserve this operation's UUID/epoch/sequence. */
    pthread_mutex_lock(&opid_mu);
    uint64_t low = q->id.seq;
    for (int i = 0; i < EFS_OPID_INFLIGHT; i++) {
        uint64_t pending = g_client.opid_inflight[i];
        if (pending && pending < low) low = pending;
    }
    if (low - 1 > current.ack) current.ack = low - 1;
    pthread_mutex_unlock(&opid_mu);
    efs_opid_req_pack(&current, su);
    if(env_opid_verbose)
        fprintf(stderr,"opid-append-rpc: seq=%llu ino=%llu len=%llu\n",
            (unsigned long long)q->id.seq,(unsigned long long)ino,(unsigned long long)len);
    rc = rpc_send_recv_admitted(efs_kv_inode_shard(ino), &q->id,
                                EFS_MSG_INODE_APPEND, buf, sizeof(buf),
                                EFS_MSG_INODE_APPEND_REPLY, &r, sizeof(r), 0, NULL);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
#if EFS_FAULTS
    if (append_reply_fault(q, ino)) return EFS_ERR_NET;
#endif
    if (new_size_out)
        *new_size_out = r.inode.size;
    if (start_out)
        *start_out = r.inode.atime;
    return EFS_OK;
}

/* Same identity and suffix as the reserve; len carries EFS_APPEND_ABANDON.
 * The contiguous ACK is refreshed exactly as for a reserve retry and never
 * covers q itself (the caller still owns its in-flight slot). */
int efs_client_rpc_append_abandon(efs_export_id_t export_id, efs_ino_t ino,
                                  uint64_t len, const struct efs_opid_req *q,
                                  int *retired)
{
    uint8_t buf[sizeof(struct efs_msg_inode_append) + EFS_APPEND_ACK_LEN];
    struct efs_msg_inode_append *req = (struct efs_msg_inode_append *)buf;
    struct efs_msg_inode_reply r;
    if (retired) *retired = 0;
    if (!efs_opid_req_valid(q) || q->ack >= q->id.seq || !len ||
        (len & EFS_APPEND_ABANDON))
        return EFS_ERR_INVAL;
    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->ino = ino;
    req->len = len | EFS_APPEND_ABANDON;
    struct efs_opid_req current = *q;
    pthread_mutex_lock(&opid_mu);
    uint64_t low = q->id.seq;
    for (int i = 0; i < EFS_OPID_INFLIGHT; i++) {
        uint64_t pending = g_client.opid_inflight[i];
        if (pending && pending < low) low = pending;
    }
    if (low - 1 > current.ack) current.ack = low - 1;
    pthread_mutex_unlock(&opid_mu);
    efs_opid_req_pack(&current, buf + sizeof(*req));
    int rc = rpc_send_recv_admitted(efs_kv_inode_shard(ino), &q->id,
                                    EFS_MSG_INODE_APPEND, buf, sizeof(buf),
                                    EFS_MSG_INODE_APPEND_REPLY, &r, sizeof(r), 0, NULL);
    if (rc != EFS_OK) return rc;
    if (r.status != EFS_INODE_RPC_OK) return rpc_status_to_efs(r.status);
    if (retired) *retired = r.inode.nlink == 1;
    return EFS_OK;
}

int efs_client_rpc_hold(efs_export_id_t export_id, efs_ino_t ino, int open,
                        uint64_t owner)
{
    uint8_t buf[sizeof(struct efs_msg_inode_hold)+EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_hold *req=(struct efs_msg_inode_hold *)buf;
    struct efs_opid identity;
    int rc=efs_client_publication_id(1,&identity);
    if (rc!=EFS_OK) return rc;
    memset(buf,0,sizeof(buf));
    req->export_id=export_id;req->ino=ino;req->flags=open?1u:0u;
    req->owner=owner?owner:g_client.flock_token;
    memcpy(buf+sizeof(*req),identity.client_uuid,EFS_OPID_UUID_LEN);
    memcpy(buf+sizeof(*req)+EFS_OPID_UUID_LEN,&identity.session_epoch,4);
    struct efs_msg_inode_reply reply;
    rc=rpc_send_recv_admitted(efs_kv_inode_shard(ino),&identity,EFS_MSG_INODE_HOLD,
                              buf,sizeof(buf),EFS_MSG_INODE_HOLD_REPLY,&reply,
                              sizeof(reply),0,NULL);
    return rc==EFS_OK?rpc_status_to_efs(reply.status):rc;
}

int efs_client_rpc_flock(efs_export_id_t export_id, efs_ino_t ino, uint32_t op,
                         uint64_t owner)
{
    return efs_client_rpc_flock_range(export_id, ino, op, owner, 0, 0, NULL);
}

int efs_client_rpc_flock_range(efs_export_id_t export_id, efs_ino_t ino,
                               uint32_t op, uint64_t owner, uint64_t start,
                               uint64_t end, struct efs_msg_inode_reply *reply_out)
{
    uint8_t buf[sizeof(struct efs_msg_inode_flock) + EFS_FLOCK_RANGE_LEN + EFS_SESS_WIRE_LEN];
    struct efs_msg_inode_flock *req = (struct efs_msg_inode_flock *)buf;
    struct efs_msg_inode_reply r;
    uint32_t slen = sizeof(*req);
    int rc;

    struct efs_opid identity;
    rc=efs_client_publication_id(1,&identity);
    if (rc!=EFS_OK) return rc;
    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->ino = ino;
    req->op = op;
    req->owner = owner;
    /* Always send the half-open range. [0,0] is not a lock (host
     * rejects start>=end); treat that as whole-file. Skipping the
     * suffix used to default [0,~0] on the server — same for flock,
     * but a botched FCNTL [0,0] then looked like a whole-file lock. */
    if (start == 0 && end == 0)
        end = ~(uint64_t)0;
    memcpy(buf + sizeof(*req), &start, 8);
    memcpy(buf + sizeof(*req) + 8, &end, 8);
    slen += EFS_FLOCK_RANGE_LEN;
    memcpy(buf+slen,identity.client_uuid,EFS_OPID_UUID_LEN);
    memcpy(buf+slen+EFS_OPID_UUID_LEN,&identity.session_epoch,4);
    slen += EFS_SESS_WIRE_LEN;
    /* GETLK is a leader read: it carries the identity but needs no
     * admission. Every grant and release is session-checked. */
    rc = (op & EFS_FLOCK_GETLK)
             ? rpc_send_recv_owner(ino, EFS_MSG_INODE_FLOCK, buf, slen,
                                   EFS_MSG_INODE_FLOCK_REPLY, &r, sizeof(r))
             : rpc_send_recv_admitted(efs_kv_inode_shard(ino), &identity,
                                      EFS_MSG_INODE_FLOCK, buf, slen,
                                      EFS_MSG_INODE_FLOCK_REPLY, &r, sizeof(r),
                                      0, NULL);
    if (rc != EFS_OK)
        return rc;
    if (reply_out)
        *reply_out = r;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_link(efs_export_id_t export_id, efs_ino_t src_ino,
                        efs_ino_t new_parent, const char *new_name,
                        struct efs_inode *out)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_opid_verbose_s = -1;
    int env_opid_verbose = __atomic_load_n(&env_opid_verbose_s, __ATOMIC_ACQUIRE);
    if (env_opid_verbose < 0) {
        env_opid_verbose = getenv("EFS_OPID_TRACE_VERBOSE") != NULL;
        __atomic_store_n(&env_opid_verbose_s, env_opid_verbose, __ATOMIC_RELEASE);
    }
    uint8_t buf[sizeof(struct efs_msg_inode_link) + EFS_OPID_WIRE_LEN];
    struct efs_msg_inode_link *req = (struct efs_msg_inode_link *)buf;
    struct efs_opid_req q;
    uint32_t slen;
    int slot;

    memset(buf, 0, sizeof(buf));
    req->export_id = export_id;
    req->src_ino = src_ino;
    req->new_parent = new_parent;
    if (new_name)
        strncpy(req->new_name, new_name, EFS_MAX_NAME - 1);
    slot = opid_begin(&q);
    if (slot < 0)
        return EFS_ERR_BUSY;
    if(env_opid_verbose)
        fprintf(stderr,"opid-owner: seq=%llu kind=LINK ino=%llu parent=%llu name=%s\n",
            (unsigned long long)q.id.seq,(unsigned long long)src_ino,
            (unsigned long long)new_parent,new_name?new_name:"");
    slen = opid_suffix(buf, sizeof(*req), &q);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(new_parent, EFS_MSG_INODE_LINK, buf, slen,
                                 EFS_MSG_INODE_LINK_REPLY, &r, sizeof(r));
    opid_end(slot);
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}


void efs_client_rpc_set_deadline_ms(uint64_t mono_ms)
{
    efs_net_set_deadline_ms(mono_ms);
}

uint64_t efs_client_rpc_deadline_ms(void)
{
    return efs_net_deadline_ms();
}

static int rpc_past_deadline(void)
{
    struct timespec ts;
    uint64_t now;

    uint64_t deadline = efs_net_deadline_ms();
    if (!deadline)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    return now >= deadline;
}

int efs_client_rpc_past_deadline(void)
{
    return rpc_past_deadline();
}

/* rpc_send_recv_shard with the dual-host picker (raft mode reports). Same
 * NOT_PRIMARY hint-following; BUSY backs off and retries. recv_ms > 0
 * bounds the reply wait for this call instead of the conn's 30 s
 * default: a REPORT's service time grows with its record count, and a
 * recv timeout on a request the server is still executing re-sends the
 * whole request (Oct 1 2026: 8 × 20 GiB close REPORTs, attempts=6,
 * 116 s). */
static int rpc_send_recv_dual_once(uint8_t type, const void *req, uint32_t req_len,
                                   uint8_t expect, void *reply, uint32_t reply_len,
                                   int recv_ms)
{
    rpc_tl_sent = 0;
    rpc_tl_lost = 0;
    if (rpc_type_mutates(type, req, req_len)) {
        int ready = efs_client_session_check();
        if (ready != EFS_OK)
            return ready;
    }
    efs_node_id_t target = 0;
    efs_node_id_t skip = 0;
    int saw_busy = 0;
    int prof = rpc_prof_enabled();
    unsigned long long t_start = rpc_prof_now_us();
    union { struct efs_msg_inode_create create; struct efs_msg_inode_unlink unlink;
            struct efs_msg_inode_link link; struct efs_msg_inode_rename_at rename; } mutation;
    uint8_t create_retry[sizeof(mutation)+EFS_OPID_WIRE_LEN];
    int identified_create=(type==EFS_MSG_INODE_CREATE && req_len==sizeof(mutation.create)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_UNLINK && req_len==sizeof(mutation.unlink)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_LINK && req_len==sizeof(mutation.link)+EFS_OPID_WIRE_LEN) ||
        (type==EFS_MSG_INODE_RENAME_AT && req_len==sizeof(mutation.rename)+EFS_OPID_WIRE_LEN);
    if (identified_create) { memcpy(create_retry,req,req_len);req=create_retry; }
    /* MKDIR owns a bounded callback deadline. Keep its original identity
     * while a cross-shard prepare drains instead of stopping after 16 tries.
     * Other users retain the legacy attempt cap. */
    int attempts = identified_create && type == EFS_MSG_INODE_CREATE &&
        S_ISDIR(((const struct efs_msg_inode_create *)req)->mode) &&
        efs_client_rpc_deadline_ms() ? 64 : 16;
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (identified_create) opid_retry_ack(create_retry,req_len);
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        unsigned long long t0;
        if (rpc_past_deadline())
            return EFS_ERR_BUSY;
        t0 = prof ? rpc_prof_now_us() : 0;
        if (target != 0) {
            conn = efs_client_conn_get(target);
            nid = target;
        } else {
            conn = raft_dual_voter_conn(&nid, skip);
        }
        unsigned long long t1 = prof ? rpc_prof_now_us() : 0;
        if (!conn || efs_conn_send_msg(conn, type, req, req_len) != 0) {
            if (conn)
                efs_client_conn_drop(nid, conn);
            if (attempt + 1 < attempts) {
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        unsigned long long t2 = prof ? rpc_prof_now_us() : 0;
        if (nid && nid <= 64)
            rpc_tl_sent |= 1ull << (nid - 1);
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        if (recv_ms > 0)
            efs_conn_set_recv_timeout(conn, recv_ms);
        unsigned long long t_recv = rpc_prof_now_us();
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        unsigned long long t3 = prof ? rpc_prof_now_us() : 0;
        if (rc != 0) {
            char ident[64];
            free(payload);
            rpc_tl_lost++;
            efs_client_conn_drop(nid, conn);
            if (attempt + 1 < attempts) {
                /* recv_ms is the configured bound and wait_ms what this
                 * receive waited; cut= says what ended it: the call's
                 * deadline (a sync REPORT's 8 s budget), the bound, or the
                 * connection. A11 finding 2 read recv_ms=30001 on a receive
                 * the 8 s budget had cut. */
                int bound = recv_ms > 0 ? recv_ms : EFS_IO_TIMEOUT_MS;
                unsigned long long waited = (rpc_prof_now_us() - t_recv) / 1000ull;
                fprintf(stderr, "inode-rpc: retry type=%u attempt=%d why=recv rc=%d "
                        "req_len=%u recv_ms=%d wait_ms=%llu cut=%s nid=%u id=%s\n",
                        type, attempt, rc, req_len, bound, waited,
                        rpc_past_deadline() ? "deadline" :
                        waited + 50u >= (unsigned long long)bound ? "bound" : "conn", nid,
                        rpc_req_ident(type, req, req_len, ident, sizeof(ident)));
                if (rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                    return EFS_ERR_BUSY;
                continue;
            }
            return EFS_ERR_NET;
        }
        if (recv_ms > 0)
            efs_conn_set_recv_timeout(conn, 0); /* pooled conn: default again */
        efs_client_conn_release(nid, conn);
        if (efs_client_rpc_past_deadline()) {
            free(payload);
            rpc_tl_lost++; /* a reply arrived too late to be used */
            return EFS_ERR_BUSY;
        }
        if (rtype != expect || plen < reply_len) {
            fprintf(stderr, "inode-rpc: proto type=%u rtype=%u plen=%u "
                    "reply_len=%u nid=%u\n",
                    type, rtype, plen, reply_len, nid);
            free(payload);
            return EFS_ERR_PROTO;
        }
        memcpy(reply, payload, reply_len);
        free(payload);
        struct efs_msg_inode_reply *r = reply;
        if (r->status == EFS_INODE_RPC_BUSY) {
            saw_busy = 1;
            skip = nid;
            target = 0;
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt + 1 == attempts || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status == EFS_INODE_RPC_STALE && stale_retryable(type)) {
            /* PREPARE can report the participant leader whose canonical
             * version disagreed with this replica. Re-reading repeatedly on
             * the same coordinator cannot resolve that replica-local drift. */
            target = r->primary_id && r->primary_id <= EFS_MAX_NODES &&
                     r->primary_id != nid ? r->primary_id : 0;
            skip = target ? 0 : nid;
            /* See rpc_send_recv_shard: a directory-op txn that lost a
             * dentry / witness race committed nothing; re-run it.
             * REPORT_CHUNKS owns its STALE (W1). */
            saw_busy = 1;
            unsigned long long sleep_start = rpc_prof_now_us();
            if (attempt + 1 == attempts || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        if (r->status != EFS_INODE_RPC_NOT_PRIMARY) {
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
            rpc_note_slow_ok(type, attempt, saw_busy, r->status, t_start);
            return EFS_OK;
        }
        if (r->primary_id == 0 || r->primary_id == nid) {
            /* Hintless NOT_PRIMARY = election in progress; transient,
             * back off like BUSY (see rpc_send_recv_shard). */
            unsigned long long sleep_start = rpc_prof_now_us();
            saw_busy = 1;
            target = 0;
            if (attempt + 1 == attempts || rpc_writer_retry_pause((unsigned)attempt) != EFS_OK)
                return EFS_ERR_BUSY;
            unsigned long long sleep_us = rpc_prof_now_us() - sleep_start;
            rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, sleep_us);
            continue;
        }
        rpc_prof_add(type, t1 - t0, t2 - t1, t3 - t2, 0);
        target = r->primary_id;
    }
    if (saw_busy)
        fprintf(stderr, "inode-rpc: dual type=%u exhausted %d BUSY/STALE "
                "retries (%.1f s) -> EBUSY\n", type, attempts, (rpc_prof_now_us() - t_start) / 1000000.0);
    return saw_busy ? EFS_ERR_BUSY : EFS_ERR_NOT_PRIMARY;
}

static int rpc_send_recv_dual(uint8_t type, const void *req, uint32_t req_len,
                              uint8_t expect, void *reply, uint32_t reply_len,
                              int recv_ms)
{
    int rc = rpc_send_recv_dual_once(type, req, req_len, expect, reply, reply_len, recv_ms);
    return rpc_resolve_identified(1, 0, type, req, req_len, expect, reply, reply_len, 0,
                                  NULL, recv_ms, rc);
}

uint64_t efs_client_rpc_last_sent(void)
{
    return rpc_tl_sent;
}

/* W155: ask nid for its receipt of an earlier REPORT (EFS_REPORT_F_RECEIPT).
 * Runs nothing on the host. EFS_OK and *out filled when nid answered. */
static int rpc_report_receipt_query(efs_node_id_t nid, const struct efs_opid *session,
                                    const struct efs_report_pubid *pubid,
                                    struct efs_msg_report_receipt_reply *out)
{
    uint8_t buf[sizeof(struct efs_msg_report_chunks) + EFS_REPORT_PUBID_LEN +
                EFS_REPORT_SESSION_LEN];
    struct efs_msg_report_chunks *hdr = (struct efs_msg_report_chunks *)buf;
    struct efs_conn *conn;
    uint8_t rtype = 0;
    void *payload = NULL;
    uint32_t plen = 0;
    int rc;

    memset(buf, 0, sizeof(buf));
    hdr->export_id = g_client.export_id;
    hdr->sync = EFS_REPORT_F_SESSION | EFS_REPORT_F_PUBID | EFS_REPORT_F_RECEIPT;
    efs_report_pubid_pack(buf + sizeof(*hdr), pubid);
    efs_report_session_pack(buf + sizeof(*hdr) + EFS_REPORT_PUBID_LEN, session);
    conn = efs_client_conn_get(nid);
    if (!conn)
        return EFS_ERR_NET;
    if (efs_conn_send_msg(conn, EFS_MSG_REPORT_CHUNKS, buf, sizeof(buf)) != 0) {
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    efs_conn_set_recv_timeout(conn, 2000);
    rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
    if (rc != 0) {
        free(payload);
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    efs_conn_set_recv_timeout(conn, 0);
    efs_client_conn_release(nid, conn);
    if (rtype != EFS_MSG_REPORT_CHUNKS_REPLY || plen < sizeof(*out)) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    memcpy(out, payload, sizeof(*out));
    free(payload);
    return out->rpc.status == EFS_INODE_RPC_OK ? EFS_OK : rpc_status_to_efs(out->rpc.status);
}

/* REPORT rcs that say nothing definite about the request. */
static int report_rc_transient(int rc)
{
    return rc == EFS_ERR_NET || rc == EFS_ERR_BUSY || rc == EFS_ERR_NO_QUORUM ||
           rc == EFS_ERR_NOT_PRIMARY || rc == EFS_ERR_NOT_FOUND;
}

int efs_client_rpc_report_resolve(efs_export_id_t export_id,
                                  const struct efs_chunk_rec *recs, uint32_t count,
                                  const struct efs_ino_size_rec *irecs,
                                  uint32_t ino_count, int sync,
                                  const struct efs_report_pubid *pubid, uint64_t sent,
                                  int rc, efs_ino_t ino)
{
    struct efs_opid session;
    struct efs_receipt_key key;
    char ident[64];
    uint64_t bound = rpc_resolve_ms(), previous, start, end;
    unsigned queries = 0, resends = 0, pass = 0;
    const char *outcome = "unknown";
    efs_node_id_t from = 0;
    int first = rc, last_asked = 0, last_none = 0;

    if (!pubid || !sent || !bound || !report_rc_transient(rc))
        return rc;
    if (efs_client_publication_id(1, &session) != EFS_OK ||
        !efs_report_session_valid(&session))
        return rc;
    memset(&key, 0, sizeof(key));
    memcpy(key.uuid, session.client_uuid, sizeof(key.uuid));
    key.epoch = session.session_epoch;
    key.mint_ns = pubid->mint_ns;
    key.seq = pubid->seq;
    efs_receipt_key_format(&key, ident, sizeof(ident));
    previous = efs_client_rpc_deadline_ms();
    start = rpc_prof_now_us() / 1000ull;
    end = start + bound;
    fprintf(stderr, "report-resolve id=%s ino=%llu recs=%u rc=%d sent=%llx: outcome not "
            "known; same-identity lookup for %llu ms\n", ident, (unsigned long long)ino,
            count, rc, (unsigned long long)sent, (unsigned long long)bound);
    efs_client_rpc_set_deadline_ms(end);
    while (rpc_prof_now_us() / 1000ull < end) {
        int pending = 0, none = 0, asked = 0;

        for (efs_node_id_t nid = 1; nid <= 64 && nid <= __atomic_load_n(&g_client.node_count, __ATOMIC_ACQUIRE); nid++) {
            struct efs_msg_report_receipt_reply rr;

            if (!(sent & (1ull << (nid - 1))))
                continue;
            asked++;
            queries++;
            if (rpc_report_receipt_query(nid, &session, pubid, &rr) != EFS_OK)
                continue;
            if (rr.state == EFS_RECEIPT_STATE_PENDING) {
                pending = 1;
            } else if (rr.state == EFS_RECEIPT_STATE_DONE) {
                if (rr.outcome == EFS_RECEIPT_OUT_APPLIED) {
                    rc = EFS_OK;
                    outcome = "applied";
                    from = nid;
                    goto done;
                }
                if (rr.outcome == EFS_RECEIPT_OUT_PARTIAL) {
                    rc = EFS_ERR_STALE;
                    outcome = "partial";
                    from = nid;
                    goto done;
                }
                if (rr.outcome == EFS_RECEIPT_OUT_NONE)
                    none++;
            }
        }
        last_asked = asked;
        last_none = none;
        if (!pending) {
            /* Nothing proves it applied: send the same identity again. A
             * host that applied it answers from its receipt or skips the
             * committed records; one that did not runs it now. */
            resends++;
            rc = efs_client_rpc_report_dirty_pub(export_id, recs, count, irecs, ino_count,
                                                 sync, pubid);
            sent |= rpc_tl_sent;
            if (rc == EFS_OK) {
                outcome = "resent";
                goto done;
            }
            if (!report_rc_transient(rc)) {
                outcome = "answered"; /* a definite verdict, e.g. STALE */
                goto done;
            }
        }
        if (rpc_writer_retry_pause(pass < 3 ? pass : 3) != EFS_OK)
            break;
        pass++;
    }
done:
    efs_client_rpc_set_deadline_ms(previous);
    /* none=a/b: of the b hosts asked last, a held a receipt saying nothing
     * was proposed. That is not proof (a host can restart or evict), so an
     * unresolved request stays unknown. */
    fprintf(stderr, "report-resolve id=%s ino=%llu outcome=%s rc=%d first_rc=%d from=%u "
            "queries=%u resends=%u none=%d/%d ms=%llu%s\n", ident, (unsigned long long)ino,
            outcome, rc, first, from, queries, resends, last_none, last_asked,
            (unsigned long long)(rpc_prof_now_us() / 1000ull - start),
            strcmp(outcome, "unknown") ? "" : " -> EIO, outcome unknown after the bound");
    if (!strcmp(outcome, "unknown"))
        return EFS_ERR_IO;
    return rc;
}

#if EFS_FAULTS
/* Private EOF-contract fault: omit one size record after valid construction.
 * File contains OMIT_SIZE <ino>; OFF or a missing file disables it. */
static uint64_t report_fault_omit_size(void)
{
    const char *path = getenv("EFS_FAULT_REPORT_SIZE_FILE");
    unsigned long long ino = 0;
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "OMIT_SIZE %llu", &ino) != 1) ino = 0;
    fclose(f);
    return (uint64_t)ino;
}
#endif

int efs_client_rpc_report_dirty_raft(efs_export_id_t export_id,
                                     const struct efs_chunk_rec *recs,
                                     uint32_t count,
                                     const struct efs_ino_size_rec *irecs,
                                     uint32_t ino_count, int sync)
{
    return efs_client_rpc_report_dirty_pub(export_id, recs, count, irecs,
                                           ino_count, sync, NULL);
}

int efs_client_rpc_report_dirty_pub(efs_export_id_t export_id,
                                    const struct efs_chunk_rec *recs,
                                    uint32_t count,
                                    const struct efs_ino_size_rec *irecs,
                                    uint32_t ino_count, int sync,
                                    const struct efs_report_pubid *pubid)
{
    rpc_tl_sent = 0; /* W155: what this call sends, for report-resolve */
    rpc_tl_lost = 0;
    if (count == 0 && ino_count == 0 && !sync)
        return EFS_OK;
    if ((count && !recs) || (ino_count && !irecs))
        return EFS_ERR_INVAL;
    if (efs_client_rpc_past_deadline())
        return EFS_ERR_BUSY;
    size_t limit = EFS_MSG_MAX_LEN - 1u;
    size_t len = sizeof(struct efs_msg_report_chunks) + EFS_REPORT_SESSION_LEN +
                 (pubid ? EFS_REPORT_PUBID_LEN : 0u);
    /* Reject impossible counts before touching arrays or allocating. Keep
     * every subtraction in size_t and the eventual wire cast representable. */
    if (ino_count > (limit - len) / sizeof(struct efs_ino_size_rec))
        return EFS_ERR_INVAL;
    len += (size_t)ino_count * sizeof(struct efs_ino_size_rec);
    if (count > (limit - len) / offsetof(struct efs_chunk_rec, deltas))
        return EFS_ERR_INVAL;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (!(i & 255u) && efs_client_rpc_past_deadline())
            return EFS_ERR_BUSY;
        if (recs[i].delta_base_n > EFS_CHUNK_DELTA_MAX)
            return EFS_ERR_INVAL;
        size_t bytes = efs_chunk_rec_wire_size(&recs[i]);
        if (bytes > limit - len)
            return EFS_ERR_INVAL;
        len += bytes;
    }
    struct efs_opid session;
    /* Query the shared mount identity; this does not allocate/ACK an operation. */
    int identity_rc = efs_client_publication_id(1, &session);
    if (identity_rc != EFS_OK) return identity_rc;
    if (!efs_report_session_valid(&session)) return EFS_ERR_INVAL;
    uint8_t *buf = malloc(len);
    if (!buf)
        return EFS_ERR_NOMEM;
    struct efs_msg_report_chunks *hdr = (struct efs_msg_report_chunks *)buf;
    hdr->export_id = export_id;
    hdr->count = count;
    hdr->sync = (sync ? EFS_REPORT_F_SYNC : 0u) | EFS_REPORT_F_SESSION |
                (pubid ? EFS_REPORT_F_PUBID : 0u);
    hdr->ino_count = ino_count;
    uint8_t *p = buf + sizeof(*hdr);
    if (count) {
        for (i = 0; i < count; i++) {
            /* W154: a two-fragment object publishes as degraded. */
            uint32_t dgf = efs_client_degraded_flags(recs[i].ino, recs[i].chunk_index,
                                                     recs[i].chunk_generation);
            size_t n;
            if (dgf) {
                struct efs_chunk_rec stamped = recs[i];
                stamped.publish_flags |= dgf;
                n = efs_chunk_rec_pack(p, len - (size_t)(p - buf), &stamped);
            } else {
                n = efs_chunk_rec_pack(p, len - (size_t)(p - buf), &recs[i]);
            }
            if (n == 0) {
                free(buf);
                return EFS_ERR_NOMEM;
            }
            p += n;
        }
    }
#if EFS_FAULTS
    uint64_t omit = report_fault_omit_size();
    hdr->ino_count = 0;
    for (i = 0; i < ino_count; i++) {
        if (omit && irecs[i].ino == omit) continue;
        memcpy(p, &irecs[i], sizeof(*irecs));
        p += sizeof(*irecs);
        hdr->ino_count++;
    }
#else
    if (ino_count)
        memcpy(p, irecs, (size_t)ino_count * sizeof(*irecs));
    p += (size_t)ino_count * sizeof(*irecs);
#endif
    if (pubid) {
        efs_report_pubid_pack(p, pubid);
        p += EFS_REPORT_PUBID_LEN;
    }
    efs_report_session_pack(p, &session);
    uint32_t wire_len = (uint32_t)(p + EFS_REPORT_SESSION_LEN - buf);
    struct efs_msg_inode_reply r;
    /* Reply wait sized to the work: the server packs, publishes and
     * waits for a Raft commit per record (13 us/rec alone, 8 concurrent
     * 160k-rec REPORTs went past 30 s). 0.5 ms per record on top of the
     * default; D24's periodic REPORT keeps a sequential writer's close
     * near 8192 records, so this is the bound for the rest. */
    int recv_ms = EFS_IO_TIMEOUT_MS + (int)(count / 2u);
    int rc = rpc_send_recv_dual(EFS_MSG_REPORT_CHUNKS, buf, wire_len,
                                EFS_MSG_REPORT_CHUNKS_REPLY, &r, sizeof(r),
                                recv_ms);
    free(buf);
    if (rc != EFS_OK)
        return rc;
    rc = rpc_status_to_efs(r.status);
    if (rc == EFS_OK && efs_client_degraded_pending())
        for (i = 0; i < count; i++)
            efs_client_degraded_drop(recs[i].ino, recs[i].chunk_index,
                                     recs[i].chunk_generation);
    return rc;
}

int efs_client_rpc_xattr(efs_export_id_t export_id, efs_ino_t ino, uint8_t op,
                         uint32_t flags, const void *name, uint16_t nlen,
                         const void *val, uint32_t vlen, void *out,
                         uint32_t *out_len)
{
    uint8_t reqbuf[sizeof(struct efs_msg_xattr) + EFS_XATTR_NAME_MAX +
                   EFS_XATTR_VALUE_MAX + EFS_SESS_WIRE_LEN];
    struct efs_msg_xattr_reply rep;
    struct efs_msg_xattr *req;
    uint32_t req_len, got = 0, hdr;
    int rc;

    if (nlen > EFS_XATTR_NAME_MAX || vlen > EFS_XATTR_VALUE_MAX)
        return EFS_ERR_INVAL;
    if ((nlen && !name) || (vlen && !val))
        return EFS_ERR_INVAL;
    req = (struct efs_msg_xattr *)reqbuf;
    memset(req, 0, sizeof(*req));
    req->export_id = export_id;
    req->flags = flags;
    req->ino = ino;
    req->vlen = vlen;
    req->nlen = nlen;
    req->op = op;
    if (nlen)
        memcpy(reqbuf + sizeof(*req), name, nlen);
    if (vlen)
        memcpy(reqbuf + sizeof(*req) + nlen, val, vlen);
    req_len = (uint32_t)sizeof(*req) + (uint32_t)nlen + vlen;
    hdr = (uint32_t)offsetof(struct efs_msg_xattr_reply, data);
    memset(&rep, 0, hdr);
    if (op == EFS_XATTR_SET || op == EFS_XATTR_REMOVE) {
        /* Mutations carry the mount session (K5); GET/LIST are reads. */
        struct efs_opid identity;
        rc = efs_client_publication_id(1, &identity);
        if (rc != EFS_OK)
            return rc;
        memcpy(reqbuf + req_len, identity.client_uuid, EFS_OPID_UUID_LEN);
        memcpy(reqbuf + req_len + EFS_OPID_UUID_LEN, &identity.session_epoch, 4);
        req_len += EFS_SESS_WIRE_LEN;
        rc = rpc_send_recv_admitted(efs_kv_inode_shard(ino), &identity,
                                    EFS_MSG_XATTR, reqbuf, req_len,
                                    EFS_MSG_XATTR_REPLY, &rep, hdr, sizeof(rep),
                                    &got);
    } else
        rc = rpc_send_recv_shard(efs_export_shard_of(ino, EFS_KV_SHARD_BITS),
                                 EFS_MSG_XATTR, reqbuf, req_len,
                                 EFS_MSG_XATTR_REPLY, &rep, hdr, sizeof(rep), &got);
    if (rc != EFS_OK)
        return rc;
    if (rep.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(rep.status);
    if (got < hdr || rep.nbytes > got - hdr || rep.nbytes > EFS_XATTR_BLOB_MAX)
        return EFS_ERR_PROTO;
    if (out_len) {
        if (out && *out_len < rep.nbytes) {
            *out_len = rep.nbytes;
            return EFS_ERR_INVAL;
        }
        if (out && rep.nbytes)
            memcpy(out, rep.data, rep.nbytes);
        *out_len = rep.nbytes;
    }
    return EFS_OK;
}

static int query_snapshot_rpc(const struct efs_msg_query_snapshot *req,uint32_t req_len,
                                struct efs_msg_query_snapshot_reply *reply,char **text,efs_node_id_t preferred)
{
    if (!req || !reply || !text) return EFS_ERR_INVAL;
    *text=NULL;memset(reply,0,sizeof(*reply));
    efs_node_id_t target=preferred,skip=0,pinned=0;
    if (req->version==EFS_QUERY_PAGE_VERSION) {
        uint64_t token=((const struct efs_msg_query_snapshot_v4 *)req)->token;
        if (token) target=pinned=(efs_node_id_t)(token>>56);
    }
    int last=EFS_ERR_BUSY;
    for (unsigned attempt=0;attempt<16;attempt++) {
        if (efs_client_rpc_past_deadline()) return EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target ? efs_client_conn_get(target) : raft_dual_voter_conn(&nid,skip);
        if (!conn) {
            last=EFS_ERR_NET;
            if (pinned) return last;
            skip=nid;target=0;
        }
        else {
            uint8_t type=0;void *payload=NULL;uint32_t len=0;
            if (efs_conn_send_msg(conn,EFS_MSG_QUERY_SNAPSHOT,req,req_len) ||
                efs_conn_recv_msg(conn,&type,&payload,&len)) {
                efs_client_conn_drop(nid,conn);free(payload);last=EFS_ERR_NET;
                if (pinned) return last;
                skip=nid;target=0;
            } else {
                efs_client_conn_release(nid,conn);
                if (type!=EFS_MSG_QUERY_SNAPSHOT_REPLY || len<sizeof(*reply)) { free(payload);return EFS_ERR_PROTO; }
                memcpy(reply,payload,sizeof(*reply));
                if (reply->version!=req->version || reply->text_len>req->max_output ||
                    len!=sizeof(*reply)+reply->text_len || (reply->status && reply->text_len)) {
                    free(payload);return EFS_ERR_PROTO;
                }
                last=reply->status;
                if (!last) {
                    /* Local routing metadata: repeated unpinned FUSE callbacks
                     * must return to the publisher that created this page. */
                    reply->primary_id=nid;
                    *text=malloc((size_t)reply->text_len+1);
                    if (!*text) { free(payload);return EFS_ERR_NOMEM; }
                    if (memchr((uint8_t *)payload+sizeof(*reply),0,reply->text_len)) {
                        free(*text);*text=NULL;free(payload);return EFS_ERR_PROTO;
                    }
                    memcpy(*text,(uint8_t *)payload+sizeof(*reply),reply->text_len);
                    (*text)[reply->text_len]=0;free(payload);return EFS_OK;
                }
                free(payload);
                if (last!=EFS_ERR_BUSY && last!=EFS_ERR_NOT_PRIMARY && last!=EFS_ERR_NO_QUORUM) return last;
                if (pinned) return last;
                target=last==EFS_ERR_NOT_PRIMARY && reply->primary_id<=EFS_MAX_NODES ? reply->primary_id : 0;
                skip=target ? 0 : nid;
            }
        }
        if (attempt+1<16 && rpc_writer_retry_pause(attempt)!=EFS_OK) return EFS_ERR_BUSY;
    }
    return last;
}

int efs_client_rpc_query_snapshot(const struct efs_msg_query_snapshot *req,
                                    struct efs_msg_query_snapshot_reply *reply,char **text)
{
    if (!req || req->version!=EFS_QUERY_VERSION || req->operation!=EFS_QUERY_STATS) return EFS_ERR_INVAL;
    return query_snapshot_rpc(req,sizeof(*req),reply,text,0);
}

/* Preferred owner is a recoverable first-page hint; token ownership remains strict. */
int efs_client_rpc_query_snapshot_v4_at(const struct efs_msg_query_snapshot_v4 *req,
                                       efs_node_id_t preferred,
                                       struct efs_msg_query_snapshot_reply *reply,char **text)
{
    if (!req || preferred>EFS_MAX_NODES || req->base.version!=EFS_QUERY_PAGE_VERSION) return EFS_ERR_INVAL;
    return query_snapshot_rpc(&req->base,sizeof(*req),reply,text,preferred);
}

/* Transport of the shared revocation coordinator (session_revoke.h): one
 * EFS_MSG_RAFT_MKFS to a voter of the payload's group, NOT_PRIMARY redirects,
 * BUSY repeated only where efs_session_busy_retry_ok allows. A transport
 * failure is ambiguous and never resent. */
static int session_revoke_submit(void *ctx,const uint8_t *payload,uint32_t plen,
                                 struct efs_msg_raft_mkfs_reply *out)
{
    (void)ctx;
    /* Any shard of the group routes to its voters; its anchor will do. */
    uint32_t shard=payload[0]==EFS_RAFT_GROUP_SHARD?1u:2u;
    efs_node_id_t target=0;
    memset(out,0,sizeof(*out));out->leader_hint=-1;out->rc=EFS_ERR_BUSY;
    for (unsigned attempt=0;attempt<8;attempt++) {
        if (efs_client_rpc_past_deadline()) return out->rc=EFS_ERR_BUSY;
        efs_node_id_t nid=target;
        struct efs_conn *conn=target?efs_client_conn_get(target):rpc_owner_conn_shard(shard,&nid);
        if (!conn) return out->rc=EFS_ERR_NET;
        if (efs_conn_send_msg(conn,EFS_MSG_RAFT_MKFS,payload,plen)) {
            efs_client_conn_drop(nid,conn);return out->rc=EFS_ERR_NET;
        }
        uint8_t type=0;uint32_t len=0;void *reply=NULL;
        if (efs_conn_recv_msg(conn,&type,&reply,&len)) {
            free(reply);efs_client_conn_drop(nid,conn);return out->rc=EFS_ERR_NET;
        }
        efs_client_conn_release(nid,conn);
        if (type!=EFS_MSG_RAFT_MKFS_REPLY || len!=sizeof(*out)) {
            free(reply);return out->rc=EFS_ERR_PROTO;
        }
        memcpy(out,reply,sizeof(*out));free(reply);
        if (out->rc==EFS_ERR_BUSY && efs_session_busy_retry_ok(payload,plen)) {
            if (rpc_writer_retry_pause(attempt)!=EFS_OK) return out->rc;
            continue;
        }
        if (out->rc!=EFS_ERR_NOT_PRIMARY) return out->rc;
        if (out->leader_hint<-1 || out->leader_hint>=EFS_MAX_NODES) return out->rc=EFS_ERR_PROTO;
        target=out->leader_hint>=0 && (efs_node_id_t)(out->leader_hint+1)!=nid ?
            (efs_node_id_t)(out->leader_hint+1):0;
        if (rpc_writer_retry_pause(attempt)!=EFS_OK) return out->rc;
    }
    return out->rc;
}

/* Revoke progress for the stop tooling: the fence stage, then cleanup. */
static void session_revoke_progress(void *ctx,const char *stage,uint32_t done,uint32_t total)
{
    (void)ctx;
    efs_client_stop_phase("revoking",(strcmp(stage,"cleanup")?0u:(uint64_t)total)+done,
                          2ull*total);
}

static char g_session_end_reason[128];

const char *efs_client_session_end_reason(void)
{
    return g_session_end_reason;
}


/* D31: a clean unmount revokes its own session after its last settle, so
 * the bodies it abandoned (and the tickets it could not settle) become
 * collectable without an operator: the group leaders drain its streams
 * into the orphan ledger and collect them. The barrier is the operator's
 * (efs_session_revoke); a failure leaves it resumable by `efs-mgmt
 * raft-session fence`. Always (D42): there is no keep-open mode. Per-shard
 * steps run on EFS_CLIENT_REVOKE_PARALLEL pool connections at once: one
 * step per shard at a time took 11 s for 765 touched shards (A11). */
#define EFS_CLIENT_REVOKE_PARALLEL 16u
int efs_client_session_end(void)
{
    if (!__atomic_load_n(&g_client.session_started,__ATOMIC_ACQUIRE) ||
        efs_client_session_fenced())
        return EFS_OK;
    struct efs_opid id;
    int rc=efs_client_publication_id(1,&id);
    if (rc!=EFS_OK) {
        snprintf(g_session_end_reason,sizeof(g_session_end_reason),
                 "revoke rc=%d phase=identity",rc);
        return rc;
    }
    uint64_t saved=efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(0);
    const struct efs_session_rpc rpc={NULL,session_revoke_submit,EFS_CLIENT_REVOKE_PARALLEL,
                                      session_revoke_progress};
    struct efs_session_revoke_result res;
    rc=efs_session_revoke(&rpc,id.client_uuid,&res);
    efs_client_rpc_set_deadline_ms(saved);
    /* Latch: nothing may run under the revoked epoch from here on. */
    __atomic_store_n(&g_client.session_fenced,1,__ATOMIC_RELEASE);
    fprintf(stderr,"efs: session revoked at unmount: epoch=%u touched=%u rc=%d "
            "ms=%llu fence_ms=%llu cleanup_ms=%llu cleaned=%u probe_failed=%u%s%s\n",
            id.session_epoch,res.ntouch,rc,(unsigned long long)res.total_ms,
            (unsigned long long)res.fence_ms,(unsigned long long)res.cleanup_ms,
            res.cleaned,res.probe_failed,
            rc==EFS_OK?"":" phase=",rc==EFS_OK?"":res.phase);
    if (rc!=EFS_OK)
        snprintf(g_session_end_reason,sizeof(g_session_end_reason),
                 "revoke rc=%d phase=%s shard=%u touched=%u",rc,res.phase,res.shard,res.ntouch);
    return rc;
}
