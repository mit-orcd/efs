/* K33 live client view (efs/member_view.h, src/client/node_cache.c): a
 * mounted client installs a later committed membership revision without a
 * remount and refuses every other record.
 *
 * 1. Judgement: same, later, older, another cluster, another formation, a
 *    replaced member (token, endpoint) and invalid records.
 * 2. MEMBERSHIP_READ reply parsing: the server's rc, short, oversized,
 *    trailing and undecodable replies.
 * 3. Table extension: new members are appended in id order, rows already in
 *    the table are never written, a row the record does not name (or names
 *    at another endpoint) is refused; concurrent readers of the published
 *    count always see complete rows.
 * 4. The client runtime against five loopback fake members (no daemons):
 *    the mount refuses a table that does not match the record, mounts at
 *    revision 1, the refresh thread installs revision 2 (node 4), keeps it
 *    against another cluster, a divergent record and an older revision,
 *    and a request for node 5 after revision 3 commits waits for the view
 *    (no remount, bounded) and connects on demand; an id that is no member
 *    is answered within the bound and then from the negative cache. */
#include "efs/member_view.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "../src/client/client_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c, what) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

#define NSRV 5
static uint16_t ports[NSRV + 1];

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void base(struct efs_membership *m, uint64_t cluster)
{
    memset(m, 0, sizeof(*m));
    m->count = m->formed = 3;
    m->cluster = cluster;
    for (uint32_t i = 0; i < 3; i++) {
        m->m[i].id = i + 1;
        m->m[i].port = ports[i + 1];
        m->m[i].token = 0xa000u + i + 1;
        snprintf(m->m[i].addr, sizeof(m->m[i].addr), "127.0.0.1");
    }
}

static void plus(const struct efs_membership *a, uint32_t id, uint64_t token,
                 struct efs_membership *out)
{
    char err[256];
    int rc = efs_membership_plus(a, "127.0.0.1", ports[id], token, out, err, sizeof(err));
    CHECK(rc == EFS_OK && out->count == id, err);
}

static struct efs_membership r1, r2, r3, other, diverge, wrong_ep;

static void records(void)
{
    base(&r1, 0x5eed);
    plus(&r1, 4, 0xa004, &r2);
    plus(&r2, 5, 0xa005, &r3);
    base(&other, 0xbeef);
    plus(&r1, 4, 0xdead, &diverge); /* member 4 is another store */
    base(&wrong_ep, 0x5eed);
    wrong_ep.m[1].port = (uint16_t)(ports[2] + 1000);
}

static void judgement(void)
{
    struct efs_membership f4 = r2, bad = r2;
    CHECK(efs_member_view_judge(&r1, &r1) == EFS_VIEW_SAME, "same");
    CHECK(efs_member_view_judge(&r1, &r2) == EFS_VIEW_ADVANCE, "one later revision");
    CHECK(efs_member_view_judge(&r1, &r3) == EFS_VIEW_ADVANCE, "two revisions at once");
    CHECK(efs_member_view_judge(&r3, &r1) == EFS_VIEW_OLDER, "older");
    CHECK(efs_member_view_judge(&r2, &other) == EFS_VIEW_OTHER_CLUSTER, "another cluster");
    CHECK(efs_member_view_judge(&r2, &diverge) == EFS_VIEW_DIVERGENT, "replaced member 4");
    CHECK(efs_member_view_judge(&r1, &wrong_ep) == EFS_VIEW_DIVERGENT, "member 2 moved");
    f4.formed = 4; /* the same members as another formation */
    CHECK(efs_member_view_judge(&r2, &f4) == EFS_VIEW_DIVERGENT, "another formation count");
    bad.m[3].token = 0;
    CHECK(efs_member_view_judge(&r1, &bad) == EFS_VIEW_INVALID, "invalid candidate");
    CHECK(efs_member_view_revision(&r1) == 1 && efs_member_view_revision(&r3) == 3 &&
          efs_member_view_revision(&bad) == 0, "revision numbers");
    CHECK(!strcmp(efs_view_verdict_name(EFS_VIEW_OTHER_CLUSTER), "another cluster"), "names");
}

static void parsing(void)
{
    struct efs_msg_membership_reply r;
    struct efs_membership out;
    size_t len = 0;
    memset(&r, 0, sizeof(r));
    CHECK(efs_membership_encode(&r2, r.record, sizeof(r.record), &len) == EFS_OK, "encode");
    r.len = (uint32_t)len;
    CHECK(efs_member_view_parse_reply(&r, EFS_MEMBERSHIP_REPLY_HDR + r.len, &out) == EFS_OK &&
          efs_membership_equal(&out, &r2), "round trip");
    CHECK(efs_member_view_parse_reply(&r, EFS_MEMBERSHIP_REPLY_HDR + r.len - 1, &out) ==
          EFS_ERR_DECODE, "short record");
    CHECK(efs_member_view_parse_reply(&r, 4, &out) == EFS_ERR_DECODE, "short header");
    CHECK(efs_member_view_parse_reply(&r, EFS_MEMBERSHIP_REPLY_HDR + r.len + 1, &out) ==
          EFS_ERR_DECODE, "trailing byte");
    r.record[0] = 1; /* record version 1 */
    CHECK(efs_member_view_parse_reply(&r, EFS_MEMBERSHIP_REPLY_HDR + r.len, &out) != EFS_OK,
          "another record version");
    r.rc = EFS_ERR_UNFORMED;
    CHECK(efs_member_view_parse_reply(&r, EFS_MEMBERSHIP_REPLY_HDR, &out) == EFS_ERR_UNFORMED,
          "server rc");
    r.rc = EFS_OK;
    r.len = EFS_MEMBERSHIP_MAX_LEN + 1;
    CHECK(efs_member_view_parse_reply(&r, sizeof(r), &out) == EFS_ERR_DECODE, "oversized");
}

static void rows_of(const struct efs_membership *m, struct efs_node *t)
{
    memset(t, 0, sizeof(*t) * EFS_MAX_NODES);
    for (uint32_t i = 0; i < m->count; i++) {
        t[i].id = m->m[i].id;
        t[i].port = m->m[i].port;
        snprintf(t[i].addr, sizeof(t[i].addr), "%s", m->m[i].addr);
        t[i].quota = 1000 + i; /* LIST_NODES facts the view must keep */
    }
}

static struct efs_node race_rows[EFS_MAX_NODES];
static uint32_t race_count;
static int race_stop; /* atomic: volatile is not a synchronisation */
static int race_bad;

static void *race_reader(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&race_stop, __ATOMIC_ACQUIRE)) {
        uint32_t n = __atomic_load_n(&race_count, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < n; i++)
            if (race_rows[i].id != i + 1 || !race_rows[i].port || !race_rows[i].addr[0])
                race_bad++;
    }
    return NULL;
}

static void extension(void)
{
    struct efs_node t[EFS_MAX_NODES];
    uint32_t n = 0;
    char err[256];

    rows_of(&r1, t);
    t[0].id = 2; t[1].id = 1; /* LIST_NODES order is not id order */
    t[0].port = ports[2]; t[1].port = ports[1];
    CHECK(efs_member_view_extend_rows(t, 3, &r3, &n, err, sizeof(err)) == EFS_OK && n == 5,
          "two members appended");
    CHECK(t[3].id == 4 && t[4].id == 5 && t[3].port == ports[4] && t[4].token == 0xa005 &&
          !strcmp(t[4].addr, "127.0.0.1"), "appended rows from the record");
    CHECK(t[0].id == 2 && t[0].quota == 1000 && t[2].quota == 1002, "old rows untouched");
    CHECK(efs_member_view_extend_rows(t, 5, &r3, &n, err, sizeof(err)) == EFS_OK && n == 5,
          "nothing to add");

    rows_of(&r1, t);
    t[1].port = (uint16_t)(ports[2] + 1);
    CHECK(efs_member_view_extend_rows(t, 3, &r2, &n, err, sizeof(err)) == EFS_ERR_FENCED &&
          strstr(err, "node 2 at 127.0.0.1"), "a row at another endpoint is refused");
    rows_of(&r2, t);
    CHECK(efs_member_view_extend_rows(t, 4, &r1, &n, err, sizeof(err)) == EFS_ERR_FENCED &&
          strstr(err, "node 4"), "a row the record does not name is refused");
    rows_of(&r1, t);
    t[2].id = 1;
    CHECK(efs_member_view_extend_rows(t, 3, &r1, &n, err, sizeof(err)) == EFS_ERR_FENCED,
          "duplicate row refused");

    rows_of(&r1, t);
    uint32_t cnt = 3;
    CHECK(efs_member_view_publish(t, &cnt, &diverge, err, sizeof(err)) == EFS_OK && cnt == 4,
          "publish only extends the table; judging is the caller's");
    cnt = 3;
    t[0].port = 1;
    CHECK(efs_member_view_publish(t, &cnt, &r3, err, sizeof(err)) == EFS_ERR_FENCED && cnt == 3,
          "a refused publish leaves the count");

    /* Readers never see a published count before its rows. */
    for (int it = 0; it < 300; it++) {
        pthread_t th[2];
        rows_of(&r1, race_rows);
        race_count = 3;
        __atomic_store_n(&race_stop, 0, __ATOMIC_RELEASE);
        for (int k = 0; k < 2; k++)
            pthread_create(&th[k], NULL, race_reader, NULL);
        efs_member_view_publish(race_rows, &race_count, &r2, err, sizeof(err));
        efs_member_view_publish(race_rows, &race_count, &r3, err, sizeof(err));
        __atomic_store_n(&race_stop, 1, __ATOMIC_RELEASE);
        for (int k = 0; k < 2; k++)
            pthread_join(th[k], NULL);
        CHECK(race_count == 5, "race: final count");
    }
    CHECK(race_bad == 0, "race: a reader saw an incomplete row");
}

/* ---- loopback fake members ---- */

static pthread_mutex_t srv_mu = PTHREAD_MUTEX_INITIALIZER;
static struct efs_membership serve[NSRV + 1];
static int serving[NSRV + 1];   /* 0: answer UNFORMED */
static int reads[NSRV + 1];
static int lfd[NSRV + 1];

static void *conn_thread(void *arg)
{
    intptr_t pack = (intptr_t)arg;
    int fd = (int)(pack >> 8), who = (int)(pack & 0xff);
    for (;;) {
        uint8_t type = 0;
        void *p = NULL;
        uint32_t len = 0;
        if (efs_recv_msg(fd, &type, &p, &len) != 0)
            break;
        if (type == EFS_MSG_CLIENT_HELLO) {
            struct efs_msg_client_hello_reply r = {efs_client_hello_check(p, len),
                                                   EFS_PROTOCOL_VERSION};
            efs_send_msg(fd, EFS_MSG_CLIENT_HELLO_REPLY, &r, sizeof(r));
        } else if (type == EFS_MSG_MEMBERSHIP_READ) {
            struct efs_msg_membership_reply r;
            size_t ml = 0;
            memset(&r, 0, sizeof(r));
            pthread_mutex_lock(&srv_mu);
            reads[who]++;
            if (!serving[who])
                r.rc = EFS_ERR_UNFORMED;
            else if (efs_membership_encode(&serve[who], r.record, sizeof(r.record), &ml))
                r.rc = EFS_ERR_INVAL;
            r.len = (uint32_t)ml;
            pthread_mutex_unlock(&srv_mu);
            efs_send_msg(fd, EFS_MSG_MEMBERSHIP_READ_REPLY, &r, EFS_MEMBERSHIP_REPLY_HDR + r.len);
        } else {
            free(p);
            break;
        }
        free(p);
    }
    close(fd);
    return NULL;
}

static void *accept_thread(void *arg)
{
    int who = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd[who], NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        pthread_t t;
        pthread_create(&t, NULL, conn_thread, (void *)(((intptr_t)fd << 8) | who));
        pthread_detach(t);
    }
}

static void listeners(void)
{
    for (int i = 1; i <= NSRV; i++) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int one = 1;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        lfd[i] = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(lfd[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (lfd[i] < 0 || bind(lfd[i], (struct sockaddr *)&a, sizeof(a)) ||
            listen(lfd[i], 64) || getsockname(lfd[i], (struct sockaddr *)&a, &al)) {
            perror("listen");
            exit(2);
        }
        ports[i] = ntohs(a.sin_port);
        pthread_t t;
        pthread_create(&t, NULL, accept_thread, (void *)(intptr_t)i);
        pthread_detach(t);
    }
}

static void serve_all(const struct efs_membership *m, int upto)
{
    pthread_mutex_lock(&srv_mu);
    for (int i = 1; i <= NSRV; i++) {
        serving[i] = i <= upto;
        if (m)
            serve[i] = *m;
    }
    pthread_mutex_unlock(&srv_mu);
}

static void serve_one(int who, const struct efs_membership *m)
{
    pthread_mutex_lock(&srv_mu);
    serve[who] = *m;
    serving[who] = 1;
    pthread_mutex_unlock(&srv_mu);
}

static int wait_revision(uint32_t want, int ms)
{
    int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (efs_client_view_revision() == want)
            return 1;
        usleep(20000);
    }
    return efs_client_view_revision() == want;
}

static void runtime(void)
{
    char addr[NSRV][32];
    const char *list[EFS_MAX_NODES] = {0};
    uint64_t installs = 0, refusals = 0, r0 = 0;

    for (int i = 0; i < 3; i++) {
        snprintf(addr[i], sizeof(addr[i]), "127.0.0.1:%u", ports[i + 1]);
        list[i] = addr[i];
    }
    pthread_mutex_init(&g_client.lock, NULL);
    efs_client_init_nodes(&g_client, list, 3);

    /* The mount refuses a record whose member 2 is at another endpoint. */
    serve_all(&wrong_ep, 3);
    CHECK(efs_client_view_mount() == EFS_ERR_FENCED && g_client.node_count == 3 &&
          efs_client_view_revision() == 0, "mount refuses a table the record contradicts");
    CHECK(efs_client_view_start() != EFS_OK, "no refresh without a mounted record");

    serve_all(&r1, 3);
    CHECK(efs_client_view_mount() == EFS_OK && efs_client_view_revision() == 1 &&
          g_client.node_count == 3, "mounted at revision 1");
    CHECK(efs_client_view_start() == EFS_OK, "refresh thread");

    /* Another cluster from node 1: refused, logged, view unchanged. */
    serve_one(1, &other);
    usleep(450000);
    efs_client_view_counters(&installs, &refusals);
    CHECK(efs_client_view_revision() == 1 && g_client.node_count == 3 && refusals >= 1 &&
          installs == 0, "another cluster refused");

    /* Revision 2 commits: installed without a remount. */
    serve_all(&r2, 4);
    CHECK(wait_revision(2, 3000), "revision 2 installed");
    CHECK(g_client.node_count == 4 && g_client.nodes[3].id == 4 &&
          g_client.nodes[3].port == ports[4], "node 4 in the table");
    efs_client_view_counters(&installs, &r0);

    /* A divergent record and an older revision change nothing. */
    serve_one(1, &diverge);
    serve_one(2, &r1);
    usleep(600000);
    efs_client_view_counters(&installs, &refusals);
    CHECK(efs_client_view_revision() == 2 && g_client.node_count == 4 && installs == 1 &&
          refusals > r0, "divergent refused, older ignored");
    CHECK(g_client.nodes[3].port == ports[4], "member 4 not replaced by the divergent record");

    /* Revision 3: a request for node 5 waits for the view and connects. */
    serve_all(&r3, 5);
    int64_t t0 = now_ms();
    struct efs_conn *c = efs_client_conn_get(5);
    int64_t took = now_ms() - t0;
    CHECK(c != NULL && took < 2500, "node 5 connected on demand within the bound");
    if (c)
        efs_client_conn_release(5, c);
    CHECK(efs_client_view_revision() == 3 && g_client.node_count == 5, "revision 3 installed");
    CHECK(!efs_client_node_is_down(5), "node 5 is usable");

    /* Not a member: bounded, then the negative cache answers at once. */
    t0 = now_ms();
    CHECK(efs_client_conn_get(9) == NULL, "id 9 is not a member");
    took = now_ms() - t0;
    CHECK(took <= 2600, "unknown id answered within the bound");
    t0 = now_ms();
    CHECK(efs_client_conn_get(9) == NULL && now_ms() - t0 < 200, "negative cache");

    efs_client_view_stop();
    pthread_mutex_lock(&srv_mu);
    int any = 0;
    for (int i = 1; i <= NSRV; i++)
        any += reads[i] > 0;
    pthread_mutex_unlock(&srv_mu);
    CHECK(any >= 4, "the view asked several members (rotation)");
}

int main(void)
{
    setenv("EFS_TRANSPORT", "tcp", 1);
    setenv("EFS_CLIENT_VIEW_POLL_MS", "100", 1);
    listeners();
    records();
    judgement();
    parsing();
    extension();
    runtime();
    if (failures) {
        fprintf(stderr, "member view: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("K33 member view: judgement, reply parsing, table extension (concurrent readers), "
           "mount refusal, refresh install, cluster/divergent/older refusal, on-demand member: "
           "%d checks PASS\n", checks);
    return 0;
}
