/* W133 (K4): the client protocol declaration gate is fail-closed.
 *
 * The server-side gate (efs_decl_gate_frame) and the client-side helpers
 * (efs_fd_declare / efs_conn_declare / efs_connect_declared) are run against
 * each other over real frames:
 *   - TCP frame path over a socketpair, always;
 *   - RDMA frame path over a QP loopback, when a usable IB device exists
 *     (otherwise that part is reported NOTRUN and the test still passes).
 * Checked: an undeclared request is refused with no reply; HEARTBEAT is the
 * only request served before a declaration; a declaration naming any other
 * version is answered EFS_ERR_PROTO and refused, even on a declared
 * connection; the exact version is accepted and the next request served; a
 * client refuses a server that answers with another version. */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* One server turn: wait for a frame on either channel and run the gate on
 * it, as server_handle_conn does before dispatch. */
static int serve_one(struct efs_conn *srv, struct efs_decl_gate *g, uint8_t *type_out)
{
    int chan = efs_conn_wait_request(srv);
    CHECK(chan >= 0, "wait_request");
    srv->recv_chan = chan;
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t len = 0;
    CHECK(efs_conn_recv_msg(srv, &type, &payload, &len) == 0, "server recv");
    int v = efs_decl_gate_frame(g, srv, type, payload, len);
    free(payload);
    if (type_out)
        *type_out = type;
    return v;
}

static int fd_has_nothing(int fd)
{
    uint8_t b;
    ssize_t n = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

struct declare_job { int fd; struct efs_conn *conn; int rc; };
static void *fd_declare_thread(void *arg)
{
    struct declare_job *j = arg;
    j->rc = efs_fd_declare(j->fd);
    return NULL;
}
static void *conn_declare_thread(void *arg)
{
    struct declare_job *j = arg;
    j->rc = efs_conn_declare(j->conn);
    return NULL;
}

static void test_policy(void)
{
    int exempt = 0;
    for (int t = 0; t < 256; t++) {
        int req = efs_declaration_required((uint8_t)t);
        if (t == EFS_MSG_CLIENT_HELLO || t == EFS_MSG_HEARTBEAT) {
            CHECK(!req, "declaration and heartbeat are the only exempt messages");
            exempt++;
        } else {
            CHECK(req, "every other message needs a declaration");
        }
    }
    CHECK(exempt == 2, "exactly two exempt messages");

    struct efs_msg_client_hello h = {{'E', 'F', 'S', 'P'}, EFS_PROTOCOL_VERSION, 0, EFS_ROLE_CLIENT, {0}, {0}};
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_OK, "exact version accepted");
    h.role = 0;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_ERR_PROTO, "a declaration names a role");
    h.role = 9;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_ERR_PROTO, "unknown role refused");
    h.role = EFS_ROLE_PEER;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_OK, "peer role well formed");
    h.role = EFS_ROLE_CLIENT;
    h.features = ~0ull;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_OK, "features do not gate");
    h.version = EFS_PROTOCOL_VERSION + 1;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_ERR_PROTO, "newer version refused");
    h.version = EFS_PROTOCOL_VERSION - 1;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_ERR_PROTO, "older version refused");
    h.version = 0;
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_ERR_PROTO, "version 0 refused");
    h.version = EFS_PROTOCOL_VERSION;
    CHECK(efs_client_hello_check(&h, sizeof(h) - 1) == EFS_ERR_PROTO, "short declaration refused");
    CHECK(efs_client_hello_check(&h, sizeof(h) + 1) == EFS_ERR_PROTO, "long declaration refused");
    CHECK(efs_client_hello_check(&h, 0) == EFS_ERR_PROTO, "empty declaration refused");
    CHECK(efs_client_hello_check(NULL, sizeof(h)) == EFS_ERR_PROTO, "missing payload refused");
    memcpy(h.magic, "EFSQ", 4);
    CHECK(efs_client_hello_check(&h, sizeof(h)) == EFS_ERR_PROTO, "wrong magic refused");
}

/* The frame-level sequence, shared by the TCP and RDMA paths. cli_fd is the
 * raw client fd for the TCP path (crafted frames); cli is the client conn
 * (NULL on the TCP path, so the raw fd is used for everything). */
static void run_gate_sequence(struct efs_conn *srv, int cli_fd, struct efs_conn *cli,
                              const char *path)
{
    struct efs_decl_gate g = {0};
    uint8_t type = 0;
    struct efs_msg_inode_lookup lk;
    memset(&lk, 0, sizeof(lk));
    lk.parent = 1;
    memcpy(lk.name, "x", 1);
#define SEND(t, p, n)                                                            \
    CHECK((cli ? efs_conn_send_msg(cli, (t), (p), (n))                           \
               : efs_send_msg(cli_fd, (t), (p), (n))) == 0, "client send")
#define RECV(t, p, n)                                                            \
    CHECK((cli ? efs_conn_recv_msg(cli, (t), (p), (n))                           \
               : efs_recv_msg(cli_fd, (t), (p), (n))) == 0, "client recv")

    /* 1. Undeclared request: refused, no reply, still undeclared. */
    SEND(EFS_MSG_INODE_LOOKUP, &lk, sizeof(lk));
    CHECK(serve_one(srv, &g, &type) == EFS_GATE_REFUSE, "undeclared LOOKUP refused");
    CHECK(type == EFS_MSG_INODE_LOOKUP && !g.declared, "state after refusal");
    if (!cli) {
        usleep(20000);
        CHECK(fd_has_nothing(cli_fd), "a refused frame gets no reply");
    }
    /* Formerly fail-open requests are gated too. */
    SEND(EFS_MSG_PUT_CHUNK, &lk, sizeof(lk));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "undeclared PUT_CHUNK refused");
    SEND(EFS_MSG_STATUS, NULL, 0);
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "undeclared STATUS refused");
    SEND(EFS_MSG_RDMA_SETUP, &lk, sizeof(lk));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "undeclared RDMA_SETUP refused");

    /* 2. HEARTBEAT is served without a declaration and does not declare. */
    SEND(EFS_MSG_HEARTBEAT, NULL, 0);
    CHECK(serve_one(srv, &g, &type) == EFS_GATE_DISPATCH, "undeclared HEARTBEAT dispatched");
    CHECK(type == EFS_MSG_HEARTBEAT && !g.declared, "heartbeat does not declare");

    /* 3. Wrong version: answered with EFS_ERR_PROTO + served version, refused. */
    struct efs_msg_client_hello bad = {{'E', 'F', 'S', 'P'}, EFS_PROTOCOL_VERSION + 1, 0, EFS_ROLE_CLIENT, {0}, {0}};
    SEND(EFS_MSG_CLIENT_HELLO, &bad, sizeof(bad));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "newer declaration refused");
    CHECK(!g.declared, "refused declaration leaves the connection undeclared");
    {
        void *rep = NULL;
        uint32_t rlen = 0;
        RECV(&type, &rep, &rlen);
        CHECK(type == EFS_MSG_CLIENT_HELLO_REPLY &&
              rlen == sizeof(struct efs_msg_client_hello_reply), "refusal reply frame");
        const struct efs_msg_client_hello_reply *r = rep;
        CHECK(r->rc == EFS_ERR_PROTO && r->version == EFS_PROTOCOL_VERSION,
              "refusal names EFS_ERR_PROTO and the served version");
        free(rep);
    }
    bad.version = EFS_PROTOCOL_VERSION - 1;
    SEND(EFS_MSG_CLIENT_HELLO, &bad, sizeof(bad));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "older declaration refused");
    { void *rep = NULL; uint32_t rlen = 0; RECV(&type, &rep, &rlen); free(rep); }
    bad.version = EFS_PROTOCOL_VERSION;
    memcpy(bad.magic, "EFSX", 4);
    SEND(EFS_MSG_CLIENT_HELLO, &bad, sizeof(bad));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "wrong magic refused");
    { void *rep = NULL; uint32_t rlen = 0; RECV(&type, &rep, &rlen); free(rep); }
    SEND(EFS_MSG_CLIENT_HELLO, &bad, 4);
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "short declaration refused");
    { void *rep = NULL; uint32_t rlen = 0; RECV(&type, &rep, &rlen); free(rep); }
    SEND(EFS_MSG_INODE_LOOKUP, &lk, sizeof(lk));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "still undeclared after bad declarations");

    /* 4. The real client declaration is accepted; the next request is served. */
    {
        struct declare_job j = { cli_fd, cli, -1 };
        pthread_t th;
        CHECK(pthread_create(&th, NULL, cli ? conn_declare_thread : fd_declare_thread, &j) == 0,
              "declare thread");
        CHECK(serve_one(srv, &g, &type) == EFS_GATE_ANSWERED, "declaration answered");
        CHECK(type == EFS_MSG_CLIENT_HELLO && g.declared, "connection declared");
        pthread_join(th, NULL);
        CHECK(j.rc == EFS_OK, "client saw the declaration accepted");
    }
    SEND(EFS_MSG_INODE_LOOKUP, &lk, sizeof(lk));
    CHECK(serve_one(srv, &g, &type) == EFS_GATE_DISPATCH, "declared LOOKUP dispatched");
    CHECK(type == EFS_MSG_INODE_LOOKUP, "dispatched type");
    SEND(EFS_MSG_PUT_CHUNK, &lk, sizeof(lk));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_DISPATCH, "declared PUT_CHUNK dispatched");
    SEND(EFS_MSG_HEARTBEAT, NULL, 0);
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_DISPATCH, "declared HEARTBEAT dispatched");

    /* 5. One declaration per connection (D39): a later declaration, exact
     *    or mismatched, is answered with a refusal and closes it. */
    bad.version = EFS_PROTOCOL_VERSION + 7;
    memcpy(bad.magic, "EFSP", 4);
    SEND(EFS_MSG_CLIENT_HELLO, &bad, sizeof(bad));
    CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "mismatched redeclaration refused");
    { void *rep = NULL; uint32_t rlen = 0; RECV(&type, &rep, &rlen); free(rep); }
    {
        struct declare_job j = { cli_fd, cli, -1 };
        pthread_t th;
        CHECK(pthread_create(&th, NULL, cli ? conn_declare_thread : fd_declare_thread, &j) == 0,
              "redeclare thread");
        CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE, "exact redeclaration refused");
        pthread_join(th, NULL);
        CHECK(j.rc == EFS_ERR_PROTO, "redeclaration refused");
    }
#undef SEND
#undef RECV
    printf("test_declaration_gate: %s frame path OK\n", path);
}

/* Without a daemon admission the gate admits the CLIENT role only: a peer
 * or mgmt declaration is refused (fail-closed). */
static void test_roles_without_admission(void)
{
    uint8_t roles[2] = {EFS_ROLE_PEER, EFS_ROLE_MGMT};
    for (int i = 0; i < 2; i++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
        struct efs_conn *srv = efs_conn_wrap_tcp(sv[1], 1);
        struct efs_decl_gate g = {0};
        struct efs_msg_client_hello h;
        memset(&h, 0, sizeof(h));
        memcpy(h.magic, "EFSP", 4);
        h.version = EFS_PROTOCOL_VERSION;
        h.role = roles[i];
        CHECK(efs_send_msg(sv[0], EFS_MSG_CLIENT_HELLO, &h, sizeof(h)) == 0, "send");
        CHECK(serve_one(srv, &g, NULL) == EFS_GATE_REFUSE && !g.declared, "role refused");
        uint8_t type = 0;
        void *rep = NULL;
        uint32_t rlen = 0;
        CHECK(efs_recv_msg(sv[0], &type, &rep, &rlen) == 0 && rlen == sizeof(struct efs_msg_client_hello_reply) &&
              ((struct efs_msg_client_hello_reply *)rep)->rc != EFS_OK, "refusal answered");
        free(rep);
        efs_conn_destroy(srv);
        close(sv[0]);
    }
    /* Roles have disjoint allowlists; JOIN and unknown types match none. */
    CHECK(efs_role_allow(EFS_ROLE_CLIENT, EFS_MSG_RAFT, 80) == EFS_ALLOW_NONE, "client Raft");
    CHECK(efs_role_allow(EFS_ROLE_CLIENT, EFS_MSG_RAFT_MKFS, 0) == EFS_ALLOW_NONE, "client mkfs");
    CHECK(efs_role_allow(EFS_ROLE_CLIENT, EFS_MSG_INODE_LOOKUP, 8) == EFS_ALLOW_APP, "client lookup");
    CHECK(efs_role_allow(EFS_ROLE_CLIENT, EFS_MSG_PACK_COMPACT, 8) == EFS_ALLOW_APP, "client maintenance once formed");
    CHECK(efs_role_allow(EFS_ROLE_CLIENT, EFS_MSG_RAFT_STEP_DOWN, 8) == EFS_ALLOW_NONE, "step-down is mgmt-only");
    CHECK(efs_role_allow(EFS_ROLE_MGMT, EFS_MSG_RAFT_MKFS, 0) == EFS_ALLOW_MGMT, "mgmt mkfs");
    CHECK(efs_role_allow(EFS_ROLE_MGMT, EFS_MSG_RAFT_MKFS, 9) == EFS_ALLOW_APP, "mgmt submit");
    CHECK(efs_role_allow(EFS_ROLE_PEER, EFS_MSG_RAFT, 80) == EFS_ALLOW_CONTROL, "peer Raft");
    CHECK(efs_role_allow(EFS_ROLE_PEER, EFS_MSG_FORMATION_READ, 8) == EFS_ALLOW_CONTROL, "peer read");
    CHECK(efs_role_allow(EFS_ROLE_PEER, EFS_MSG_RAFT_MKFS, 0) == EFS_ALLOW_NONE, "peer mkfs");
    CHECK(efs_role_allow(EFS_ROLE_PEER, EFS_MSG_GC_FRAG_BATCH, 8) == EFS_ALLOW_APP, "peer data plane");
    for (uint8_t r = EFS_ROLE_CLIENT; r <= EFS_ROLE_PEER; r++) {
        CHECK(efs_role_allow(r, 16 /* JOIN */, 8) == EFS_ALLOW_NONE, "JOIN refused");
        CHECK(efs_role_allow(r, 250, 8) == EFS_ALLOW_NONE, "unknown type refused");
    }
    printf("test_declaration_gate: roles without admission and allowlists OK\n");
}

static void test_tcp_path(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct efs_conn *srv = efs_conn_wrap_tcp(sv[1], 1);
    CHECK(srv, "wrap server");
    run_gate_sequence(srv, sv[0], NULL, "TCP");
    efs_conn_destroy(srv);
    close(sv[0]);
}

/* The client refuses a server that accepts it but serves another version:
 * a fail-open old daemon answering {EFS_OK, 2} must not be used. */
static void test_client_refuses_other_version(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct declare_job j = { sv[0], NULL, -1 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, fd_declare_thread, &j) == 0, "thread");
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t len = 0;
    CHECK(efs_recv_msg(sv[1], &type, &payload, &len) == 0 && type == EFS_MSG_CLIENT_HELLO,
          "server saw the declaration");
    const struct efs_msg_client_hello *h = payload;
    CHECK(len == sizeof(*h) && h->version == EFS_PROTOCOL_VERSION &&
          !memcmp(h->magic, "EFSP", 4), "client declares exactly EFS_PROTOCOL_VERSION");
    free(payload);
    struct efs_msg_client_hello_reply rep;
    memset(&rep, 0, sizeof(rep));
    rep.rc = EFS_OK;
    rep.version = EFS_PROTOCOL_VERSION - 1;
    CHECK(efs_send_msg(sv[1], EFS_MSG_CLIENT_HELLO_REPLY, &rep, sizeof(rep)) == 0, "reply");
    pthread_join(th, NULL);
    CHECK(j.rc == EFS_ERR_PROTO, "client refuses an accepting server of another version");

    /* And a refusing server's rc is surfaced as is. */
    j.rc = -1;
    CHECK(pthread_create(&th, NULL, fd_declare_thread, &j) == 0, "thread 2");
    CHECK(efs_recv_msg(sv[1], &type, &payload, &len) == 0, "server saw declaration 2");
    free(payload);
    rep.rc = EFS_ERR_PROTO;
    rep.version = EFS_PROTOCOL_VERSION + 1;
    CHECK(efs_send_msg(sv[1], EFS_MSG_CLIENT_HELLO_REPLY, &rep, sizeof(rep)) == 0, "reply 2");
    pthread_join(th, NULL);
    CHECK(j.rc == EFS_ERR_PROTO, "refused declaration reported");

    /* A closed connection (old daemon that refuses by closing) is EFS_ERR_NET. */
    j.rc = -1;
    CHECK(pthread_create(&th, NULL, fd_declare_thread, &j) == 0, "thread 3");
    CHECK(efs_recv_msg(sv[1], &type, &payload, &len) == 0, "server saw declaration 3");
    free(payload);
    close(sv[1]);
    pthread_join(th, NULL);
    CHECK(j.rc == EFS_ERR_NET, "closed connection is a network refusal");
    close(sv[0]);
}

/* efs_connect_declared over loopback TCP: fd on acceptance, -1 (and the fd
 * closed) when the server refuses. */
struct accept_job { int lfd; int accept_version; int saw; };
static void *accept_thread(void *arg)
{
    struct accept_job *a = arg;
    int fd = accept(a->lfd, NULL, NULL);
    CHECK(fd >= 0, "accept");
    struct efs_conn *srv = efs_conn_wrap_tcp(fd, 1);
    CHECK(srv, "wrap accepted");
    struct efs_decl_gate g = {0};
    uint8_t type = 0;
    if (a->accept_version) {
        int v = serve_one(srv, &g, &type);
        a->saw = (v == EFS_GATE_ANSWERED && g.declared && type == EFS_MSG_CLIENT_HELLO);
        /* The client's first real request must now be dispatchable. */
        if (a->saw && serve_one(srv, &g, &type) == EFS_GATE_DISPATCH && type == EFS_MSG_STATUS)
            a->saw = 2;
    } else {
        /* An old daemon: answer with its own version, accepting. */
        void *payload = NULL;
        uint32_t len = 0;
        CHECK(efs_conn_recv_msg(srv, &type, &payload, &len) == 0, "recv");
        free(payload);
        struct efs_msg_client_hello_reply rep;
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.version = EFS_PROTOCOL_VERSION - 1;
        efs_conn_send_msg(srv, EFS_MSG_CLIENT_HELLO_REPLY, &rep, sizeof(rep));
        a->saw = 1;
    }
    efs_conn_destroy(srv);
    return NULL;
}

static void test_connect_declared(void)
{
    int lfd = efs_listen_tcp("127.0.0.1", 0, 4);
    CHECK(lfd >= 0, "listen");
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    CHECK(getsockname(lfd, (struct sockaddr *)&sa, &sl) == 0, "getsockname");
    uint16_t port = ntohs(sa.sin_port);

    struct accept_job a = { lfd, 1, 0 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, accept_thread, &a) == 0, "accept thread");
    int fd = efs_connect_declared("127.0.0.1", port);
    CHECK(fd >= 0, "connect_declared accepted");
    CHECK(efs_send_msg(fd, EFS_MSG_STATUS, NULL, 0) == 0, "first request after declaration");
    pthread_join(th, NULL);
    CHECK(a.saw == 2, "server answered the declaration and dispatched the request");
    close(fd);

    a.accept_version = 0;
    a.saw = 0;
    CHECK(pthread_create(&th, NULL, accept_thread, &a) == 0, "accept thread 2");
    fd = efs_connect_declared("127.0.0.1", port);
    CHECK(fd < 0, "connect_declared refuses a server of another version");
    pthread_join(th, NULL);
    CHECK(a.saw == 1, "old server answered");
    close(lfd);
}

struct upgrade_req { struct efs_conn *c; int rc; };
static void *upgrade_thread(void *arg)
{
    struct upgrade_req *u = arg;
    u->rc = efs_rdma_client_upgrade(u->c);
    return NULL;
}

static int test_rdma_path(void)
{
    if (!efs_rdma_available()) {
        printf("test_declaration_gate: RDMA frame path NOTRUN (no usable IB device)\n");
        return 0;
    }
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct efs_conn *cli = efs_conn_wrap_tcp(sv[0], 0);
    struct efs_conn *srv = efs_conn_wrap_tcp(sv[1], 1);
    CHECK(cli && srv, "wrap");
    struct upgrade_req u = { cli, -1 };
    pthread_t cth;
    CHECK(pthread_create(&cth, NULL, upgrade_thread, &u) == 0, "cthread");
    {
        uint8_t type = 0;
        void *pl = NULL;
        uint32_t plen = 0;
        CHECK(efs_recv_msg(sv[1], &type, &pl, &plen) == 0 && type == EFS_MSG_RDMA_SETUP, "SETUP");
        uint8_t rep[EFS_RMSG_SETUP_BYTES];
        uint32_t rlen = sizeof(rep);
        CHECK(efs_rdma_server_accept(srv, pl, plen, &rep, &rlen) == 0, "server_accept");
        free(pl);
        CHECK(efs_send_msg(sv[1], EFS_MSG_RDMA_SETUP_REPLY, &rep, rlen) == 0, "SETUP_REPLY");
    }
    pthread_join(cth, NULL);
    CHECK(u.rc == 0 && cli->rc && srv->rc, "both ends RDMA");
    run_gate_sequence(srv, sv[0], cli, "RDMA");
    efs_conn_destroy(cli);
    efs_conn_destroy(srv);
    return 1;
}

int main(void)
{
    test_policy();
    test_roles_without_admission();
    test_tcp_path();
    test_client_refuses_other_version();
    test_connect_declared();
    int rdma = test_rdma_path();
    printf("test_declaration_gate: OK (protocol %u, TCP PASS, RDMA %s)\n",
           EFS_PROTOCOL_VERSION, rdma ? "PASS" : "NOTRUN");
    return 0;
}
