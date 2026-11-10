/* Task EC: the .find result cache through its owner interface (find_res.h).
 *
 * Links the real src/client/efs_fuse/find_res.c and nothing else of the
 * client. The query RPC, the caller context and credentials, the virtual
 * inode helpers (vq_intern, vq_version_matches, virtual_file_open) and the
 * stat helper are link-time definitions here. The RPC answers with text
 * naming the caller it was fetched for ("u<uid> g<gid> n<ngroups> <term>
 * #<call>"), so a test can tell whose result it was served.
 *
 * Cases: LOOKUP, OPEN, GETATTR and the fd-less READ each fetch and serve
 * the caller's fresh result; OPEN refuses bytes whose version changed
 * (-ESTALE); fetch errors pass through and a failed fetch never serves the
 * table's bytes; another uid gets its own result, not the cached one; the
 * publisher hint is kept per caller and term; 40 terms through 16 slots;
 * the 64 MiB byte budget evicts the oldest and refuses a result larger
 * than the budget; a third concurrent fetch answers -EBUSY; unmount frees
 * every slot (ASan leak check); four threads with distinct uids racing on
 * shared and private terms are only ever served their own result (TSan
 * entry). */
#include "efs_fuse/find_res.h"
#include "efs_fuse/support.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread struct fuse_ctx t_ctx;
static __thread int t_has_ctx;
static __thread uint32_t t_ngroups;
static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stub_cv = PTHREAD_COND_INITIALIZER;
static unsigned rpc_calls, rpc_parked, rpc_hold;
static int rpc_rc, version_ok = 1;
static size_t rpc_claim_len; /* when set, the reply claims this length */
static efs_node_id_t last_hint;
static __thread efs_node_id_t t_last_hint;

uint64_t stats_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
uint64_t efs_client_rpc_deadline_ms(void) { return 0; }
void efs_client_rpc_set_deadline_ms(uint64_t ms) { (void)ms; }
int efs_rc_to_errno(int rc)
{
    return rc == EFS_ERR_ACCES ? -EACCES : rc == EFS_ERR_BUSY ? -EBUSY : rc ? -EIO : 0;
}
const struct fuse_ctx *ll_ctx(void) { return t_has_ctx ? &t_ctx : NULL; }
int query_credentials(struct efs_msg_query_snapshot *q)
{
    q->uid = t_has_ctx ? t_ctx.uid : 0;
    q->gid = t_has_ctx ? t_ctx.gid : 0;
    q->ngroups = t_ngroups;
    for (uint32_t i = 0; i < t_ngroups; i++) q->groups[i] = 5000 + i;
    return 0;
}
int efs_client_rpc_query_snapshot_v4_at(const struct efs_msg_query_snapshot_v4 *req,
                                        efs_node_id_t preferred,
                                        struct efs_msg_query_snapshot_reply *reply, char **text)
{
    pthread_mutex_lock(&stub_mu);
    unsigned call = ++rpc_calls;
    last_hint = preferred;
    if (rpc_hold) {
        rpc_parked++;
        pthread_cond_broadcast(&stub_cv);
        while (rpc_hold) pthread_cond_wait(&stub_cv, &stub_mu);
        rpc_parked--;
    }
    int rc = rpc_rc;
    size_t claim = rpc_claim_len;
    pthread_mutex_unlock(&stub_mu);
    t_last_hint = preferred;
    if (rc) return rc;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "u%u g%u n%u %s #%u", req->base.uid, req->base.gid,
                     req->base.ngroups, req->base.term, call);
    memset(reply, 0, sizeof(*reply));
    reply->primary_id = 7;
    *text = strdup(buf);
    assert(*text);
    reply->text_len = claim ? (uint32_t)claim : (uint32_t)n;
    return 0;
}
uint64_t virtual_version(const char *text, size_t len)
{
    (void)text;
    return len; /* the stubs never read bytes past a claimed length */
}
static __thread size_t interned_len;
fuse_ino_t vq_intern(efs_ino_t dir_ino, const char *term, uint64_t version, size_t len)
{
    (void)term; (void)version;
    interned_len = len;
    return (fuse_ino_t)(dir_ino | (1ULL << 60));
}
int vq_version_matches(efs_ino_t ino, const char *text, size_t len)
{
    (void)ino; (void)text; (void)len;
    return version_ok;
}
int virtual_file_open(struct fuse_file_info *fi, efs_ino_t ino, const char *text, size_t len)
{
    (void)ino;
    char *copy = malloc(len + 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, text, len);
    copy[len] = '\0';
    fi->fh = (uint64_t)(uintptr_t)copy;
    return 0;
}
void stat_set_size_blocks(struct stat *st, uint64_t size, uint64_t allocated)
{
    (void)allocated;
    st->st_size = (off_t)size;
}

/* A v3. page name for a FIND of `term` (efs/command_path.h). */
static void term_name(char *out, size_t cap, const char *term)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned char raw[184] = {EFS_QUERY_FIND};
    size_t n = 64 + strlen(term), o = 3;
    assert(n <= sizeof(raw) && cap > 3 + (n * 8 + 5) / 6);
    memcpy(raw + 64, term, strlen(term));
    memcpy(out, "v3.", 3);
    unsigned acc = 0, bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | raw[i]; bits += 8;
        while (bits >= 6) { bits -= 6; out[o++] = b64[(acc >> bits) & 63]; }
        acc &= (1u << bits) - 1;
    }
    if (bits) out[o++] = b64[(acc << (6 - bits)) & 63];
    out[o] = '\0';
}

static void as_caller(uid_t uid, gid_t gid, uint32_t ngroups)
{
    t_has_ctx = 1;
    t_ctx.uid = uid;
    t_ctx.gid = gid;
    t_ngroups = ngroups;
}

static int open_copy(efs_ino_t dir, const char *name, char **copy)
{
    struct fuse_file_info fi = {0};
    int rc = find_res_open(&fi, (fuse_ino_t)(dir | (1ULL << 60)), dir, "/.find/x", name);
    *copy = rc ? NULL : (char *)(uintptr_t)fi.fh;
    return rc;
}

static int stat_size(efs_ino_t dir, const char *name, off_t *size)
{
    struct efs_inode parent = {.ino = dir};
    struct stat st;
    int rc = find_query_fill_stat(&parent, "/.find/x", name, &st);
    if (!rc) *size = st.st_size;
    return rc;
}

static void serve_own_result(void)
{
    char name[300], buf[512], want[64];
    term_name(name, sizeof(name), "report");
    as_caller(1001, 2001, 3);
    fuse_ino_t q = 0;
    unsigned before = rpc_calls;
    assert(find_res_intern(42, "/.find/x", name, &q) == 0 && q == (42 | (1ULL << 60)));
    assert(rpc_calls == before + 1);
    snprintf(want, sizeof(want), "u1001 g2001 n3 report #%u", rpc_calls);
    assert(interned_len == strlen(want));
    char *copy;
    assert(open_copy(42, name, &copy) == 0);
    snprintf(want, sizeof(want), "u1001 g2001 n3 report #%u", rpc_calls);
    assert(!strcmp(copy, want) && rpc_calls == before + 2); /* OPEN fetched again */
    free(copy);
    off_t size = 0;
    assert(stat_size(42, name, &size) == 0 && rpc_calls == before + 3);
    snprintf(want, sizeof(want), "u1001 g2001 n3 report #%u", rpc_calls);
    assert(size == (off_t)strlen(want));
    int n = find_query_read(42, "/.find/x", name, buf, sizeof(buf), 0);
    snprintf(want, sizeof(want), "u1001 g2001 n3 report #%u", rpc_calls);
    assert(n == (int)strlen(want) && !memcmp(buf, want, (size_t)n));
    assert(find_query_read(42, "/.find/x", name, buf, sizeof(buf), n) == 0); /* EOF */
    assert(find_query_read(42, "/.find/x", name, buf, 4, 1) == 4 && !memcmp(buf, "1001", 4));
    version_ok = 0;
    assert(open_copy(42, name, &copy) == -ESTALE); /* bytes changed since LOOKUP */
    version_ok = 1;
    assert(t_last_hint == 7); /* the publisher of this caller's previous page */
}

static void errors_never_serve_cached_bytes(void)
{
    char name[300], buf[64];
    term_name(name, sizeof(name), "report");
    as_caller(1001, 2001, 3);
    fuse_ino_t q;
    assert(find_res_intern(43, "/.find/x", name, &q) == 0); /* a slot now holds bytes */
    rpc_rc = EFS_ERR_ACCES;
    assert(find_res_intern(43, "/.find/x", name, &q) == -EACCES);
    char *copy;
    assert(open_copy(43, name, &copy) == -EACCES);
    off_t size;
    assert(stat_size(43, name, &size) == -EACCES);
    assert(find_query_read(43, "/.find/x", name, buf, sizeof(buf), 0) == -EACCES);
    rpc_rc = EFS_ERR_BUSY;
    assert(find_res_intern(43, "/.find/x", name, &q) == -EBUSY);
    rpc_rc = 0;
    assert(find_res_intern(43, "/.find/x", "not-a-page-name", &q) == -EINVAL);
}

static void other_uid_gets_its_own(void)
{
    char name[300];
    term_name(name, sizeof(name), "shared");
    as_caller(1001, 2001, 0);
    char *a, *b;
    assert(open_copy(44, name, &a) == 0 && !strncmp(a, "u1001 g2001 ", 12));
    as_caller(1002, 2001, 0);
    assert(open_copy(44, name, &b) == 0 && !strncmp(b, "u1002 g2001 ", 12));
    assert(last_hint == 0); /* no hint from the other uid's slot */
    as_caller(1001, 2002, 0);
    free(a);
    assert(open_copy(44, name, &a) == 0 && !strncmp(a, "u1001 g2002 ", 12) && last_hint == 0);
    free(a); free(b);
}

static void many_terms_through_sixteen_slots(void)
{
    as_caller(1003, 2003, 1);
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < 40; i++) {
            char term[32], name[300], *copy;
            snprintf(term, sizeof(term), "t%02d", i);
            term_name(name, sizeof(name), term);
            assert(open_copy(100 + (efs_ino_t)i, name, &copy) == 0);
            assert(strstr(copy, term));
            free(copy);
        }
}

static void byte_budget(void)
{
    char a[300], b[300];
    term_name(a, sizeof(a), "big-a");
    term_name(b, sizeof(b), "big-b");
    as_caller(1004, 2004, 0);
    off_t size;
    rpc_claim_len = 40u << 20;
    assert(stat_size(200, a, &size) == 0 && size == (off_t)(40u << 20));
    assert(stat_size(201, b, &size) == 0 && size == (off_t)(40u << 20)); /* evicts a */
    rpc_claim_len = (64u << 20) + 1;
    assert(stat_size(202, a, &size) == -ENOMEM);
    rpc_claim_len = 0;
    assert(stat_size(203, a, &size) == 0);
}

static void *parked_fetch(void *arg)
{
    as_caller(1005, 2005, 0);
    char name[300];
    term_name(name, sizeof(name), (const char *)arg);
    off_t size;
    assert(stat_size(300, name, &size) == 0);
    return NULL;
}

static void third_fetch_is_busy(void)
{
    pthread_t t[2];
    pthread_mutex_lock(&stub_mu);
    rpc_hold = 1;
    pthread_mutex_unlock(&stub_mu);
    assert(!pthread_create(&t[0], NULL, parked_fetch, "p0"));
    assert(!pthread_create(&t[1], NULL, parked_fetch, "p1"));
    pthread_mutex_lock(&stub_mu);
    while (rpc_parked < 2) pthread_cond_wait(&stub_cv, &stub_mu);
    unsigned calls = rpc_calls;
    pthread_mutex_unlock(&stub_mu);
    char name[300];
    term_name(name, sizeof(name), "p2");
    as_caller(1005, 2005, 0);
    off_t size;
    assert(stat_size(300, name, &size) == -EBUSY && rpc_calls == calls); /* no RPC sent */
    pthread_mutex_lock(&stub_mu);
    rpc_hold = 0;
    pthread_cond_broadcast(&stub_cv);
    pthread_mutex_unlock(&stub_mu);
    for (int i = 0; i < 2; i++) assert(!pthread_join(t[i], NULL));
    assert(stat_size(300, name, &size) == 0);
}

/* F1, F2 (Task EC): two callers fetch the same directory and term at once.
 * Same uid and gid with other supplementary groups: each must be served
 * the result fetched with its own groups. Two uids: neither may see a miss
 * (GETATTR answered -1, EPERM, OPEN -EAGAIN) because the other's fetch
 * replaced the slot. Two threads stay under the fetch limit. */
struct pair_arg { uid_t uid; uint32_t ngroups; unsigned served_other, missed; };

static void *pair_churn(void *p)
{
    struct pair_arg *a = p;
    as_caller(a->uid, 4000, a->ngroups);
    char want[32], name[300];
    snprintf(want, sizeof(want), "u%u g4000 n%u ", a->uid, a->ngroups);
    term_name(name, sizeof(name), "pair");
    for (int i = 0; i < 3000; i++) {
        char *copy;
        off_t size;
        int rc = open_copy(600, name, &copy);
        if (rc) { assert(rc == -EAGAIN); a->missed++; }
        else { if (strncmp(copy, want, strlen(want))) a->served_other++; free(copy); }
        rc = stat_size(600, name, &size);
        if (rc) { assert(rc == -1); a->missed++; }
    }
    return NULL;
}

static void same_term_pairs(void)
{
    struct pair_arg groups[2] = {{.uid = 4001, .ngroups = 1}, {.uid = 4001, .ngroups = 2}};
    struct pair_arg uids[2] = {{.uid = 4002}, {.uid = 4003}};
    pthread_t t[2];
    for (int i = 0; i < 2; i++) assert(!pthread_create(&t[i], NULL, pair_churn, &groups[i]));
    for (int i = 0; i < 2; i++) assert(!pthread_join(t[i], NULL));
    for (int i = 0; i < 2; i++) assert(!pthread_create(&t[i], NULL, pair_churn, &uids[i]));
    for (int i = 0; i < 2; i++) assert(!pthread_join(t[i], NULL));
    printf("same-term pairs: other groups' result served %u/%u, misses %u/%u (uids)\n",
           groups[0].served_other, groups[1].served_other, uids[0].missed, uids[1].missed);
    assert(!groups[0].served_other && !groups[1].served_other); /* F1 */
    assert(!groups[0].missed && !groups[1].missed && !uids[0].missed && !uids[1].missed); /* F2 */
}

static void *churn(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    as_caller(2000 + id, 3000, 0);
    char prefix[32], shared[300], own[300], term[32];
    snprintf(prefix, sizeof(prefix), "u%u g3000 ", 2000 + id);
    term_name(shared, sizeof(shared), "both");
    snprintf(term, sizeof(term), "own%u", id);
    term_name(own, sizeof(own), term);
    for (int i = 0; i < 400; i++) {
        char *copy;
        fuse_ino_t q;
        off_t size;
        int rc = open_copy(500, (i & 1) ? own : shared, &copy);
        if (rc == -EBUSY) continue; /* fetch limit */
        assert(rc == 0 && !strncmp(copy, prefix, strlen(prefix)));
        free(copy);
        rc = find_res_intern(500, "/.find/x", shared, &q);
        assert(rc == 0 || rc == -EBUSY);
        rc = stat_size(500, own, &size);
        assert(rc == 0 || rc == -EBUSY);
    }
    return NULL;
}

/* Case filter: no argument runs every case; an argument runs the cases whose
 * name contains it; --list prints the names; an argument that matches no case
 * exits 2, so a misspelling is not a silent pass. */
static int g_argc, g_list;
static char **g_argv;
static char *g_used;

static void run_init(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    g_list = argc == 2 && !strcmp(argv[1], "--list");
    g_used = calloc((size_t)argc, 1);
    if (!g_used)
        abort();
}

static int run_case(const char *name)
{
    int hit = g_argc == 1;

    if (g_list) {
        puts(name);
        return 0;
    }
    for (int i = 1; i < g_argc; i++)
        if (strstr(name, g_argv[i]))
            hit = g_used[i] = 1;
    return hit;
}
#define RUN(fn) do { if (run_case(#fn)) fn(); } while (0)

static int run_unmatched(void)
{
    int bad = 0;

    for (int i = 1; i < g_argc && !g_list; i++)
        if (!g_used[i]) {
            fprintf(stderr, "%s: no case matches '%s' (see --list)\n", g_argv[0], g_argv[i]);
            bad = 1;
        }
    return bad;
}

static void concurrent_callers(void)
{
    pthread_t t[4];
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&t[i], NULL, churn, (void *)i));
    for (int i = 0; i < 4; i++) assert(!pthread_join(t[i], NULL));
    find_res_clear();
}

int main(int argc, char **argv)
{
    run_init(argc, argv);
    RUN(serve_own_result);
    RUN(errors_never_serve_cached_bytes);
    RUN(other_uid_gets_its_own);
    RUN(many_terms_through_sixteen_slots);
    RUN(byte_budget);
    RUN(third_fetch_is_busy);
    RUN(same_term_pairs);
    RUN(concurrent_callers);
    if (g_list)
        return 0;
    if (run_unmatched())
        return 2;
    puts("find_res interface: own fresh result on LOOKUP/OPEN/GETATTR/READ, ESTALE, errors pass "
         "through, per-uid results and hints, 16 slots, 64 MiB budget, fetch limit, clear, "
         "concurrent callers PASS");
    return 0;
}
