/* D39 admission: identity tuple, bindings, fence words, formation states
 * and the role predicates. See admission.h and the consolidated model. */
#include "admission.h"
#include "member_store.h"
#include "efs/common.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct adm_binding {
    int has;
    uint64_t inc, counter;
    int pair_known;
    uint64_t salt;
};

static pthread_mutex_t adm_mu = PTHREAD_MUTEX_INITIALIZER;       /* binding leaf lock */
static pthread_mutex_t adm_ident_mu = PTHREAD_MUTEX_INITIALIZER; /* identity events */
static struct efs_cluster_conf adm_cfg;
static int adm_inited;
static uint32_t adm_self;
static uint32_t adm_self_ip;
static uint64_t adm_inc, adm_ctr;
static int adm_pair_known;
static uint64_t adm_salt;
static int adm_known_g[ADM_NGROUPS];
static uint64_t adm_known_salt[ADM_NGROUPS];
static int adm_is_complete;
static uint32_t adm_store_state;
static uint64_t adm_gen;
static struct member_store *adm_store;
static struct adm_binding adm_b[EFS_CLUSTER_MAX_MEMBERS + 1];
static uint64_t adm_fence[EFS_CLUSTER_MAX_MEMBERS + 1]; /* C11 atomic words */

const struct efs_admission_ops adm_ops = {adm_declare, adm_frame};

void adm_reset(void)
{
    pthread_mutex_lock(&adm_mu);
    memset(&adm_cfg, 0, sizeof(adm_cfg));
    adm_inited = 0;
    adm_self = adm_self_ip = 0;
    adm_inc = adm_ctr = 0;
    adm_pair_known = 0;
    adm_salt = 0;
    memset(adm_known_g, 0, sizeof(adm_known_g));
    memset(adm_known_salt, 0, sizeof(adm_known_salt));
    adm_is_complete = 0;
    adm_store_state = EFS_MEMBER_UNFORMED;
    adm_gen = 0;
    adm_store = NULL;
    memset(adm_b, 0, sizeof(adm_b));
    for (int i = 0; i <= EFS_CLUSTER_MAX_MEMBERS; i++)
        __atomic_store_n(&adm_fence[i], 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&adm_mu);
}

int adm_init(const struct efs_cluster_conf *conf, uint32_t local_id, uint64_t inc,
             struct member_store *store)
{
    uint32_t ip = 0;
    if (!conf || local_id < 1 || local_id > conf->n ||
        efs_cluster_addr_canonical(conf->m[local_id - 1].addr, &ip) != EFS_OK)
        return EFS_ERR_INVAL;
    adm_reset();
    pthread_mutex_lock(&adm_mu);
    adm_cfg = *conf;
    adm_self = local_id;
    adm_self_ip = ip;
    adm_inc = inc;
    adm_ctr = 0;
    adm_store = store;
    if (store) {
        adm_pair_known = store->pair_known;
        adm_salt = store->salt;
        adm_store_state = store->state;
    }
    adm_inited = 1;
    pthread_mutex_unlock(&adm_mu);
    return EFS_OK;
}

int adm_ready(void) { return __atomic_load_n(&adm_inited, __ATOMIC_ACQUIRE); }
const struct efs_cluster_conf *adm_conf(void) { return adm_inited ? &adm_cfg : NULL; }
uint32_t adm_local_id(void) { return adm_self; }

uint8_t adm_channel(int remote_known, uint64_t remote_salt, int local_known, uint64_t local_salt)
{
    if (!remote_known && !local_known)
        return EFS_CHANNEL_BOOTSTRAP;
    if (!remote_known || !local_known)
        return EFS_CHANNEL_CATCHUP;
    return remote_salt == local_salt ? EFS_CHANNEL_FORMED : EFS_CHANNEL_NONE;
}

const char *adm_channel_name(uint8_t c)
{
    switch (c) {
    case EFS_CHANNEL_BOOTSTRAP: return "bootstrap";
    case EFS_CHANNEL_CATCHUP: return "catch-up";
    case EFS_CHANNEL_FORMED: return "formed";
    case EFS_CHANNEL_CLIENT: return "client";
    case EFS_CHANNEL_MGMT: return "mgmt";
    default: return "none";
    }
}

static void local_identity_locked(struct efs_identity *out)
{
    memset(out, 0, sizeof(*out));
    out->member_id = adm_self;
    out->endpoint_ip = adm_self_ip;
    out->endpoint_port = adm_self ? adm_cfg.m[adm_self - 1].port : 0;
    out->pair_known = (uint8_t)adm_pair_known;
    out->incarnation = adm_inc;
    out->counter = adm_ctr;
    out->salt = adm_pair_known ? adm_salt : 0;
    memcpy(out->digest, adm_cfg.digest, EFS_CLUSTER_DIGEST_LEN);
}

void adm_local_identity(struct efs_identity *out)
{
    pthread_mutex_lock(&adm_mu);
    local_identity_locked(out);
    pthread_mutex_unlock(&adm_mu);
}

/* "member 2 at 10.0.0.2:7000 inc 9 ctr 1 pair 1234|unknown cfg abcdef012345" */
static void fmt_identity(const struct efs_identity *d, char *out, size_t cap)
{
    char ip[INET_ADDRSTRLEN] = "?", dg[13], pair[32];
    struct in_addr a = {d->endpoint_ip};
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
    efs_cluster_digest_short(d->digest, dg);
    if (d->pair_known)
        snprintf(pair, sizeof(pair), "%llu", (unsigned long long)d->salt);
    else
        snprintf(pair, sizeof(pair), "unknown");
    snprintf(out, cap, "member %u at %s:%u inc %llu ctr %llu pair %s cfg %s", d->member_id, ip,
             (unsigned)d->endpoint_port, (unsigned long long)d->incarnation,
             (unsigned long long)d->counter, pair, dg);
}

static int pair_conflict(int ak, uint64_t as, int bk, uint64_t bs)
{
    return ak && bk && as != bs;
}

static int predicate_locked(const struct efs_identity *d, char *why, size_t wl)
{
    uint32_t m = d->member_id;
    char ip[INET_ADDRSTRLEN] = "?";
    if (!adm_inited) {
        snprintf(why, wl, "admission not ready");
        return ADM_REFUSE;
    }
    /* 1. configured, not local, endpoint = the configured record. */
    if (m < 1 || m > adm_cfg.n || m == adm_self) {
        snprintf(why, wl, "member %u is not a configured peer", m);
        return ADM_REFUSE;
    }
    struct in_addr a = {d->endpoint_ip};
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
    if (strcmp(ip, adm_cfg.m[m - 1].addr) || d->endpoint_port != adm_cfg.m[m - 1].port) {
        snprintf(why, wl, "endpoint %s:%u is not the configured record %s:%u of member %u", ip,
                 (unsigned)d->endpoint_port, adm_cfg.m[m - 1].addr,
                 (unsigned)adm_cfg.m[m - 1].port, m);
        return ADM_REFUSE;
    }
    /* 2. digest. */
    if (memcmp(d->digest, adm_cfg.digest, EFS_CLUSTER_DIGEST_LEN)) {
        char dd[13], ld[13];
        efs_cluster_digest_short(d->digest, dd);
        efs_cluster_digest_short(adm_cfg.digest, ld);
        snprintf(why, wl, "configuration digest %s differs from local %s", dd, ld);
        return ADM_REFUSE;
    }
    /* 3a. cross-peer pair compatibility. */
    if (pair_conflict(d->pair_known, d->salt, adm_pair_known, adm_salt)) {
        snprintf(why, wl, "pair %llu conflicts with local pair %llu",
                 (unsigned long long)d->salt, (unsigned long long)adm_salt);
        return ADM_REFUSE;
    }
    /* 3b. member history: once bound to P, always P. */
    const struct adm_binding *b = &adm_b[m];
    if (b->has && b->pair_known && (!d->pair_known || d->salt != b->salt)) {
        snprintf(why, wl, "member %u once bound to pair %llu may not declare %s", m,
                 (unsigned long long)b->salt, d->pair_known ? "another pair" : "unknown");
        return ADM_REFUSE;
    }
    /* 4. freshness against the high-water mark. */
    if (!b->has || d->incarnation > b->inc)
        return ADM_ACCEPT;
    if (d->incarnation < b->inc) {
        snprintf(why, wl, "stale incarnation %llu < %llu", (unsigned long long)d->incarnation,
                 (unsigned long long)b->inc);
        return ADM_REFUSE;
    }
    if (d->counter > b->counter)
        return ADM_ACCEPT;
    if (d->counter < b->counter) {
        snprintf(why, wl, "stale counter %llu < %llu", (unsigned long long)d->counter,
                 (unsigned long long)b->counter);
        return ADM_REFUSE;
    }
    if (d->pair_known == b->pair_known && (!d->pair_known || d->salt == b->salt))
        return ADM_IDEMPOTENT;
    snprintf(why, wl, "equal counter %llu with a different pair", (unsigned long long)d->counter);
    return ADM_REFUSE;
}

int adm_predicate(const struct efs_identity *d, char *why, size_t whylen)
{
    char tmp[256];
    pthread_mutex_lock(&adm_mu);
    int v = predicate_locked(d, why ? why : tmp, why ? whylen : sizeof(tmp));
    pthread_mutex_unlock(&adm_mu);
    return v;
}

/* C6: binding fields, then the fence word. The caller holds adm_mu; the
 * release of that lock is the declaration's ordering point. */
static void publish_locked(const struct efs_identity *d)
{
    struct adm_binding *b = &adm_b[d->member_id];
    b->has = 1;
    b->inc = d->incarnation;
    b->counter = d->counter;
    b->pair_known = d->pair_known;
    b->salt = d->pair_known ? d->salt : 0;
    __atomic_store_n(&adm_fence[d->member_id], d->incarnation, __ATOMIC_RELEASE);
}

static void refuse_log(const char *what, const struct efs_identity *theirs,
                       const struct efs_identity *ours, const char *why)
{
    static uint64_t refused;
    uint64_t n = __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
    if (n > 20 && n % 1000)
        return;
    char a[200], b[200];
    fmt_identity(theirs, a, sizeof(a));
    fmt_identity(ours, b, sizeof(b));
    fprintf(stderr, "admission: refused %s from %s (local %s): %s (%llu so far)\n", what, a, b,
            why, (unsigned long long)n);
}

int adm_declare(const struct efs_msg_client_hello *h, struct efs_msg_client_hello_reply *rep,
                struct efs_decl_gate *g)
{
    char why[256] = "";
    struct efs_identity local;
    int rc = EFS_OK;

    pthread_mutex_lock(&adm_mu);
    local_identity_locked(&local);
    rep->id = local;
    rep->complete = (uint8_t)adm_is_complete;
    if (!adm_inited) {
        rc = EFS_ERR_BUSY;
        snprintf(why, sizeof(why), "admission not ready");
    } else if (h->role == EFS_ROLE_MGMT) {
        if (memcmp(h->id.digest, adm_cfg.digest, EFS_CLUSTER_DIGEST_LEN)) {
            char dd[13], ld[13];
            efs_cluster_digest_short(h->id.digest, dd);
            efs_cluster_digest_short(adm_cfg.digest, ld);
            snprintf(why, sizeof(why), "mgmt configuration digest %s differs from local %s",
                     dd, ld);
            rc = EFS_ERR_FENCED;
        } else {
            g->role = EFS_ROLE_MGMT;
            g->channel = EFS_CHANNEL_MGMT;
            g->id = h->id;
        }
    } else if (h->role == EFS_ROLE_CLIENT) {
        if (!adm_is_complete) {
            snprintf(why, sizeof(why), "not formed");
            rc = EFS_ERR_NOT_FORMED;
        } else {
            g->role = EFS_ROLE_CLIENT;
            g->channel = EFS_CHANNEL_CLIENT;
        }
    } else {
        int v = predicate_locked(&h->id, why, sizeof(why));
        if (v == ADM_REFUSE) {
            rc = EFS_ERR_FENCED;
        } else {
            if (v == ADM_ACCEPT)
                publish_locked(&h->id);
            g->role = EFS_ROLE_PEER;
            g->id = h->id;
            g->channel = adm_channel(h->id.pair_known, h->id.salt, adm_pair_known, adm_salt);
        }
    }
    pthread_mutex_unlock(&adm_mu);
    if (rc != EFS_OK)
        refuse_log(h->role == EFS_ROLE_PEER ? "peer declaration" :
                   h->role == EFS_ROLE_MGMT ? "mgmt declaration" : "client declaration",
                   &h->id, &local, why);
    return rc;
}

int adm_check_reply(uint32_t intended, const struct efs_msg_client_hello_reply *rep,
                    char *why, size_t whylen)
{
    char tmp[256];
    char *w = why ? why : tmp;
    size_t wl = why ? whylen : sizeof(tmp);
    struct efs_identity local;
    int rc = EFS_OK;

    pthread_mutex_lock(&adm_mu);
    local_identity_locked(&local);
    if (rep->rc != EFS_OK) {
        snprintf(w, wl, "declaration refused by the responder (rc=%d)", rep->rc);
        rc = rep->rc;
    } else if (rep->role != EFS_ROLE_PEER || rep->id.member_id != intended) {
        snprintf(w, wl, "reply from member %u, intended member %u", rep->id.member_id, intended);
        rc = EFS_ERR_PROTO;
    } else {
        int v = predicate_locked(&rep->id, w, wl);
        if (v == ADM_REFUSE)
            rc = EFS_ERR_FENCED;
        else if (v == ADM_ACCEPT)
            publish_locked(&rep->id);
    }
    pthread_mutex_unlock(&adm_mu);
    if (rc != EFS_OK)
        refuse_log("declaration reply", &rep->id, &local, w);
    return rc;
}

static int control_locked(const struct efs_identity *t)
{
    if (!adm_inited || t->member_id < 1 || t->member_id > adm_cfg.n || t->member_id == adm_self)
        return EFS_ERR_INVAL;
    if (memcmp(t->digest, adm_cfg.digest, EFS_CLUSTER_DIGEST_LEN))
        return EFS_ERR_INVAL;
    const struct adm_binding *b = &adm_b[t->member_id];
    if (!b->has)
        return EFS_ERR_UNREGISTERED;
    if (t->incarnation < b->inc)
        return EFS_ERR_STALE;
    if (t->incarnation > b->inc)
        return EFS_ERR_UNREGISTERED;
    if (pair_conflict(adm_pair_known, adm_salt, t->pair_known, t->salt))
        return EFS_ERR_FENCED;
    if (b->pair_known && (!t->pair_known || t->salt != b->salt))
        return EFS_ERR_FENCED;
    return EFS_OK;
}

int adm_check_control(const struct efs_identity *tag)
{
    if (tag->incarnation < __atomic_load_n(&adm_fence[tag->member_id <= EFS_CLUSTER_MAX_MEMBERS ?
                                                       tag->member_id : 0], __ATOMIC_ACQUIRE))
        return EFS_ERR_STALE; /* C11 pre-filter: refuse early, never admit */
    pthread_mutex_lock(&adm_mu);
    int rc = control_locked(tag);
    pthread_mutex_unlock(&adm_mu);
    return rc;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int adm_check_raft_frame(const struct efs_identity *tag, const uint8_t *frame, uint32_t len)
{
    /* Wire: [type][group][vote][flags][from:4][to:4][term:8][boot:8]... */
    if (!frame || len < 28)
        return EFS_ERR_PROTO;
    uint32_t from = rd32(frame + 4), to = rd32(frame + 8);
    uint64_t boot = (uint64_t)rd32(frame + 20) << 32 | rd32(frame + 24);
    if (from + 1 != tag->member_id || to + 1 != adm_self)
        return EFS_ERR_INVAL; /* misdirected or another member's frame */
    if (boot != tag->incarnation)
        return boot < tag->incarnation ? EFS_ERR_STALE : EFS_ERR_UNREGISTERED;
    return adm_check_control(tag);
}

int adm_check_app(const struct efs_identity *tag)
{
    int rc = EFS_OK;
    pthread_mutex_lock(&adm_mu);
    const struct adm_binding *b = tag->member_id >= 1 && tag->member_id <= EFS_CLUSTER_MAX_MEMBERS
                                      ? &adm_b[tag->member_id] : NULL;
    if (!adm_inited || !b || !b->has)
        rc = EFS_ERR_UNREGISTERED;
    else if (memcmp(tag->digest, adm_cfg.digest, EFS_CLUSTER_DIGEST_LEN))
        rc = EFS_ERR_INVAL;
    else if (tag->incarnation != b->inc || tag->counter != b->counter ||
             tag->pair_known != b->pair_known || (b->pair_known && tag->salt != b->salt))
        rc = EFS_ERR_STALE; /* not the current binding */
    else if (!adm_is_complete)
        rc = EFS_ERR_NOT_FORMED;
    else if (adm_channel(b->pair_known, b->salt, adm_pair_known, adm_salt) != EFS_CHANNEL_FORMED)
        rc = EFS_ERR_NOT_FORMED;
    pthread_mutex_unlock(&adm_mu);
    return rc;
}

int adm_frame(struct efs_decl_gate *g, uint8_t type, const void *payload, uint32_t len)
{
    int allow = efs_role_allow(g->role, type, len);
    int rc = EFS_OK;
    const char *what = "message outside the role allowlist";

    if (allow == EFS_ALLOW_NONE) {
        rc = EFS_ERR_INVAL;
    } else if (g->role == EFS_ROLE_CLIENT) {
        rc = EFS_OK; /* admitted only once complete; complete never reverts */
    } else if (g->role == EFS_ROLE_MGMT) {
        if (allow == EFS_ALLOW_APP && !adm_complete()) {
            rc = EFS_ERR_NOT_FORMED;
            what = "mgmt application command before formation";
        }
    } else if (allow == EFS_ALLOW_CONTROL) {
        what = "peer control frame";
        rc = type == EFS_MSG_RAFT ? adm_check_raft_frame(&g->id, (const uint8_t *)payload + 0, len)
                                  : adm_check_control(&g->id);
    } else {
        what = "peer application frame";
        rc = adm_check_app(&g->id);
    }
    if (rc == EFS_OK)
        return EFS_GATE_DISPATCH;
    struct efs_identity local;
    char why[96];
    adm_local_identity(&local);
    snprintf(why, sizeof(why), "type %u rc %d (%s)", type, rc,
             rc == EFS_ERR_STALE ? "stale" : rc == EFS_ERR_UNREGISTERED ? "redeclare required" :
             rc == EFS_ERR_FENCED ? "pair conflict" : rc == EFS_ERR_NOT_FORMED ? "not formed" :
             "not allowed");
    refuse_log(what, &g->id, &local, why);
    return EFS_GATE_REFUSE;
}

uint64_t adm_fence_word(uint32_t member)
{
    return member <= EFS_CLUSTER_MAX_MEMBERS ? __atomic_load_n(&adm_fence[member], __ATOMIC_ACQUIRE) : 0;
}

int adm_has_binding(uint32_t member)
{
    pthread_mutex_lock(&adm_mu);
    int has = member >= 1 && member <= EFS_CLUSTER_MAX_MEMBERS && adm_b[member].has;
    pthread_mutex_unlock(&adm_mu);
    return has;
}

/* Identity event (C2, C10). Persist first (no leaf lock: never I/O under
 * it), then publish under the leaf lock. */
static int formation_event(int gi, uint64_t salt, const uint8_t digest[EFS_CLUSTER_DIGEST_LEN],
                           int install)
{
    char err[256] = "";
    int rc = EFS_OK, learned = 0, now_complete = 0;

    if (gi < 0 || gi >= ADM_NGROUPS)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&adm_ident_mu);
    pthread_mutex_lock(&adm_mu);
    int known = adm_pair_known;
    uint64_t local = adm_salt;
    int digest_ok = !memcmp(digest, adm_cfg.digest, EFS_CLUSTER_DIGEST_LEN);
    pthread_mutex_unlock(&adm_mu);
    if (!digest_ok || (known && local != salt)) {
        char dd[13], ld[13];
        efs_cluster_digest_short(digest, dd);
        efs_cluster_digest_short(adm_cfg.digest, ld);
        fprintf(stderr, "admission: group %d formation pair (salt %llu, cfg %s) conflicts with "
                "the local pair (salt %s%llu, cfg %s); stopping without rewriting the store\n",
                gi ? 2 : 0, (unsigned long long)salt, dd, known ? "" : "unknown ",
                (unsigned long long)local, ld);
        pthread_mutex_unlock(&adm_ident_mu);
        return EFS_ERR_FENCED;
    }
    if (!known) {
        if (adm_store)
            rc = member_store_learn_pair(adm_store, salt, err, sizeof(err));
        if (rc != EFS_OK) {
            fprintf(stderr, "admission: %s\n", err);
            pthread_mutex_unlock(&adm_ident_mu);
            return rc;
        }
        learned = 1;
    }
    pthread_mutex_lock(&adm_mu);
    if (learned) {
        adm_pair_known = 1;
        adm_salt = salt;
        if (adm_store_state == EFS_MEMBER_UNFORMED)
            adm_store_state = EFS_MEMBER_FORMING;
    }
    if (install || learned) {
        adm_ctr++;
        adm_gen++;
    }
    adm_known_g[gi] = 1;
    adm_known_salt[gi] = salt;
    if (!adm_is_complete && adm_known_g[0] && adm_known_g[1] &&
        adm_known_salt[0] == adm_known_salt[1]) {
        adm_is_complete = 1;
        now_complete = adm_store_state != EFS_MEMBER_FORMED;
    }
    uint64_t ctr = adm_ctr;
    pthread_mutex_unlock(&adm_mu);
    if (now_complete) {
        if (adm_store && member_store_mark_formed(adm_store, err, sizeof(err)) != EFS_OK)
            fprintf(stderr, "admission: %s; formed state not persisted\n", err);
        else {
            pthread_mutex_lock(&adm_mu);
            adm_store_state = EFS_MEMBER_FORMED;
            pthread_mutex_unlock(&adm_mu);
        }
    }
    pthread_mutex_unlock(&adm_ident_mu);
    fprintf(stderr, "admission: group %d formation pair salt %llu %s%s; counter %llu%s\n",
            gi ? 2 : 0, (unsigned long long)salt, install ? "installed" : "known",
            learned ? " (pair learned)" : "", (unsigned long long)ctr,
            now_complete ? "; formation complete" : "");
    return EFS_OK;
}

int adm_formation_install(int gi, uint64_t salt, const uint8_t digest[EFS_CLUSTER_DIGEST_LEN])
{
    return formation_event(gi, salt, digest, 1);
}

int adm_formation_observe(int gi, uint64_t salt, const uint8_t digest[EFS_CLUSTER_DIGEST_LEN])
{
    return formation_event(gi, salt, digest, 0);
}

int adm_complete(void)
{
    pthread_mutex_lock(&adm_mu);
    int c = adm_is_complete;
    pthread_mutex_unlock(&adm_mu);
    return c;
}

int adm_known(int gi, uint64_t *salt)
{
    if (gi < 0 || gi >= ADM_NGROUPS)
        return 0;
    pthread_mutex_lock(&adm_mu);
    int k = adm_known_g[gi];
    if (salt)
        *salt = adm_known_salt[gi];
    pthread_mutex_unlock(&adm_mu);
    return k;
}

int adm_pair(uint64_t *salt)
{
    pthread_mutex_lock(&adm_mu);
    int k = adm_pair_known;
    if (salt)
        *salt = adm_salt;
    pthread_mutex_unlock(&adm_mu);
    return k;
}

uint32_t adm_state(void)
{
    pthread_mutex_lock(&adm_mu);
    uint32_t s = adm_store_state;
    pthread_mutex_unlock(&adm_mu);
    return s;
}

uint64_t adm_generation(void)
{
    pthread_mutex_lock(&adm_mu);
    uint64_t g = adm_gen;
    pthread_mutex_unlock(&adm_mu);
    return g;
}

uint64_t adm_counter(void)
{
    pthread_mutex_lock(&adm_mu);
    uint64_t c = adm_ctr;
    pthread_mutex_unlock(&adm_mu);
    return c;
}

uint64_t adm_incarnation(void)
{
    pthread_mutex_lock(&adm_mu);
    uint64_t i = adm_inc;
    pthread_mutex_unlock(&adm_mu);
    return i;
}
