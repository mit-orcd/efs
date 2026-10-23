/* D39 admission, in process, through the real entry points: the
 * configuration parser and digest (C1), the member store (C2/C3: owner
 * lock, boot counter, identity and pair), the declaration gate with the
 * daemon's admission (C4/C5/C6), the per-frame predicates the handler and
 * the Raft inbox drain apply (C7 (a)/(b)), the formation state (C3/C10)
 * and the peer pool's declare-and-validate (C4) against an admitter in a
 * forked process. Trace names follow tests/test_d39_model.py.
 *
 *   test_d39_admission            run every trace
 *   test_d39_admission conf       parse stdin; print "OK <digest>" or "REFUSED <reason>"
 */
#include "efs/cluster_conf.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "../src/server/admission.h"
#include "../src/server/member_store.h"
#include "test_tmpdir.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* peer_pool.c's entry points (linked from src/server/peer_pool.c). */
struct efs_conn *server_peer_conn_new(const char *host, uint16_t port);
struct efs_conn *server_peer_conn_get(const char *host, uint16_t port);
void server_peer_conn_release(const char *host, uint16_t port, struct efs_conn *c);
void server_peer_pool_init(void);

static int checks;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static char tmp_root[4096];

/* ---------------------------------------------------------------- C1 */

static void conf_from(struct efs_cluster_conf *c, const char *text)
{
    char err[256];
    CHECK(efs_cluster_conf_parse(text, strlen(text), c, err, sizeof(err)) == EFS_OK, err);
}

static int conf_refused(const char *text)
{
    struct efs_cluster_conf c;
    char err[256] = "";
    return efs_cluster_conf_parse(text, strlen(text), &c, err, sizeof(err)) != EFS_OK && err[0];
}

static void hex(const uint8_t *d, char out[65])
{
    for (int i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_conf(void)
{
    struct efs_cluster_conf a, b, c;
    uint8_t d[32];
    char h[65];
    /* SHA-256 known answers (FIPS 180-4 examples). */
    efs_sha256("abc", 3, d);
    hex(d, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc");
    efs_sha256("", 0, d);
    hex(d, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty");
    const char *l56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    efs_sha256(l56, strlen(l56), d);
    hex(d, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 448 bits");

    /* s5_three_members_no_count, s6_canonical_digest */
    conf_from(&a, "1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17432\n");
    conf_from(&b, "# comment\n3   10.0.0.2:17432  # third\n\n1 10.0.0.1:17432\n2\t10.0.0.1:17433\n");
    CHECK(a.n == 3 && !memcmp(a.digest, b.digest, 32), "whitespace, comments, order do not matter");
    conf_from(&c, "1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17433\n");
    CHECK(memcmp(a.digest, c.digest, 32), "a different list has a different digest");
    char canon[256];
    size_t cl = efs_cluster_conf_canonical(&a, canon, sizeof(canon));
    CHECK(cl && !strncmp(canon, "efs-cluster-config 1\n1 10.0.0.1:17432\n", 38), "canonical records");
    efs_sha256(canon, cl, d);
    CHECK(!memcmp(d, a.digest, 32), "digest = SHA-256 of the canonical records");
    /* s5_malformed_input_refused */
    CHECK(conf_refused("1 10.0.0.1:1\n2 10.0.0.1:2\n3junk 10.0.0.1:3\n"), "3junk");
    CHECK(conf_refused("1 10.0.0.1:1\n2 10.0.0.1:2\n2 10.0.0.1:3\n"), "duplicate id");
    CHECK(conf_refused("1 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:2\n"), "duplicate endpoint");
    CHECK(conf_refused("1 10.0.0.1:1\n2 10.0.0.1:2\n"), "two members");
    CHECK(conf_refused("1 10.0.0.1:1\n2 10.0.0.1:2\n4 10.0.0.1:3\n"), "gap in ids");
    CHECK(conf_refused("1 0.0.0.0:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "wildcard");
    CHECK(conf_refused("1 255.255.255.255:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "broadcast");
    CHECK(conf_refused("1 node1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "host name");
    CHECK(conf_refused("1 10.0.0.01:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "leading zero octet");
    CHECK(conf_refused("01 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "leading zero id");
    CHECK(conf_refused("1 10.0.0.1\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "no port");
    CHECK(conf_refused("1 10.0.0.1:0\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "port zero");
    CHECK(conf_refused("1 10.0.0.1:65536\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "port 65536");
    CHECK(conf_refused("1 10.0.0.1:1 x\n2 10.0.0.1:2\n3 10.0.0.1:3\n"), "extra field");
    CHECK(conf_refused("1 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n4 10.0.0.1:4\n5 10.0.0.1:5\n"
                       "6 10.0.0.1:6\n7 10.0.0.1:7\n8 10.0.0.1:8\n9 10.0.0.1:9\n10 10.0.0.1:10\n"
                       "11 10.0.0.1:11\n12 10.0.0.1:12\n13 10.0.0.1:13\n14 10.0.0.1:14\n"
                       "15 10.0.0.1:15\n16 10.0.0.1:16\n17 10.0.0.1:17\n"), "seventeen");
    conf_from(&c, "1 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n4 10.0.0.1:4\n5 10.0.0.1:5\n"
                  "6 10.0.0.1:6\n7 10.0.0.1:7\n8 10.0.0.1:8\n9 10.0.0.1:9\n10 10.0.0.1:10\n"
                  "11 10.0.0.1:11\n12 10.0.0.1:12\n13 10.0.0.1:13\n14 10.0.0.1:14\n"
                  "15 10.0.0.1:15\n16 10.0.0.1:16\n");
    CHECK(c.n == 16, "sixteen members");
    /* The endpoint selects the member; nothing else does. */
    CHECK(efs_cluster_conf_member_at(&a, "10.0.0.1", 17433) == 2, "endpoint selects member 2");
    CHECK(efs_cluster_conf_member_at(&a, "10.0.0.2", 17432) == 3, "endpoint selects member 3");
    CHECK(!efs_cluster_conf_member_at(&a, "10.0.0.1", 17434), "unconfigured port");
    CHECK(!efs_cluster_conf_member_at(&a, "10.0.0.01", 17432), "non-canonical address never matches");
    CHECK(!efs_cluster_conf_member_at(&a, "0.0.0.0", 17432), "wildcard never matches");
    printf("test_d39_admission: C1 configuration and digest PASS\n");
}

/* ---------------------------------------------------------------- C2 */

static void store_dir(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", tmp_root, name);
}

static void write_file(const char *dir, const char *name, const void *b, size_t n)
{
    char p[4200];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, b, n) == (ssize_t)n, "write fixture");
    close(fd);
}

static size_t read_file(const char *dir, const char *name, uint8_t *b, size_t cap)
{
    char p[4200];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, b, cap);
    close(fd);
    return n > 0 ? (size_t)n : 0;
}

static void test_store(void)
{
    struct efs_cluster_conf a, b;
    struct member_store ms, ms2;
    char dir[4096], err[1024];
    uint64_t inc = 0, inc2 = 0;
    uint8_t before[64], after[64];
    conf_from(&a, "1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17432\n");
    conf_from(&b, "1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17433\n");

    /* s22_first_formation_of_a_fresh_store */
    store_dir(dir, sizeof(dir), "fresh/mdraft");
    char parent[4096];
    store_dir(parent, sizeof(parent), "fresh");
    mkdir(parent, 0755);
    CHECK(member_store_open(&ms, dir, 1, a.digest, err, sizeof(err)) == EFS_OK, err);
    CHECK(ms.created && ms.state == EFS_MEMBER_UNFORMED && !ms.pair_known, "initialized unformed");
    CHECK(member_store_boot(&ms, 5, &inc, err, sizeof(err)) == EFS_OK && inc == 5,
          "first start: max(fresh, 0 + 1)");
    /* s18_single_owner_and_monotonic_incarnation: a second process stops. */
    pid_t pid = fork();
    if (!pid) {
        struct member_store o;
        char e[512];
        _exit(member_store_open(&o, dir, 1, a.digest, e, sizeof(e)) == EFS_ERR_BUSY &&
              strstr(e, "owned") ? 0 : 1);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st),
          "a second process on the store stops: owned");
    CHECK(member_store_boot(&ms, 3, &inc, err, sizeof(err)) == EFS_OK && inc == 6,
          "lower fresh value: saved + 1");
    CHECK(member_store_boot(&ms, 1000, &inc, err, sizeof(err)) == EFS_OK && inc == 1000,
          "higher fresh value wins");
    member_store_close(&ms);
    CHECK(member_store_open(&ms, dir, 1, a.digest, err, sizeof(err)) == EFS_OK && !ms.created,
          "reopen after release");
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_OK && inc == 1001,
          "monotonic across restarts");
    /* Identity binding: another member id or configuration is refused. */
    member_store_close(&ms);
    CHECK(member_store_open(&ms2, dir, 2, a.digest, err, sizeof(err)) == EFS_ERR_FENCED,
          "store of member 1 refused as member 2");
    CHECK(member_store_open(&ms2, dir, 1, b.digest, err, sizeof(err)) == EFS_ERR_FENCED,
          "store of configuration A refused under configuration B");
    CHECK(member_store_open(&ms, dir, 1, a.digest, err, sizeof(err)) == EFS_OK,
          "a refused open released ownership");
    /* C3: pair learned -> forming; a conflicting pair is never written. */
    CHECK(member_store_learn_pair(&ms, 77, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMING, "pair learned: forming");
    size_t nb = read_file(dir, "identity.bin", before, sizeof(before));
    CHECK(member_store_learn_pair(&ms, 78, err, sizeof(err)) == EFS_ERR_FENCED,
          "conflicting pair refused");
    size_t na = read_file(dir, "identity.bin", after, sizeof(after));
    CHECK(nb == na && !memcmp(before, after, nb), "identity unchanged by the refusal");
    CHECK(member_store_mark_formed(&ms, err, sizeof(err)) == EFS_OK, "formed");
    member_store_close(&ms);
    CHECK(member_store_open(&ms, dir, 1, a.digest, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMED && ms.pair_known && ms.salt == 77,
          "formed state and pair persist");

    /* s22_damaged_counter_in_initialized_store_refused (each damage kind),
     * and a refused start leaves the counter file as it was. */
    uint8_t good[EFS_BOOT_FILE_LEN], bad[EFS_BOOT_FILE_LEN], now[EFS_BOOT_FILE_LEN + 8];
    CHECK(read_file(dir, "boot.bin", good, sizeof(good)) == EFS_BOOT_FILE_LEN, "boot.bin size");
    memcpy(bad, good, sizeof(bad));
    bad[20] ^= 1;
    write_file(dir, "boot.bin", bad, sizeof(bad));
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_CHECKSUM, "corrupt counter");
    CHECK(read_file(dir, "boot.bin", now, sizeof(now)) == sizeof(bad) && !memcmp(now, bad, sizeof(bad)),
          "refused start leaves the counter file");
    write_file(dir, "boot.bin", good, 12);
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_DECODE, "short counter");
    memcpy(bad, good, sizeof(bad));
    bad[11] = 2; /* version */
    write_file(dir, "boot.bin", bad, sizeof(bad));
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_UNSUPPORTED, "wrong version");
    char p[4200];
    snprintf(p, sizeof(p), "%s/boot.bin", dir);
    unlink(p);
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_NOT_FOUND, "missing counter");
    /* s20_maximum_counter_refused */
    member_store_boot_encode(UINT64_MAX, bad);
    write_file(dir, "boot.bin", bad, sizeof(bad));
    CHECK(member_store_boot(&ms, 1, &inc, err, sizeof(err)) == EFS_ERR_OVERFLOW &&
          strstr(err, "exhausted"), "exhausted counter");
    CHECK(read_file(dir, "boot.bin", now, sizeof(now)) == sizeof(bad) && !memcmp(now, bad, sizeof(bad)),
          "exhausted counter file unchanged");
    write_file(dir, "boot.bin", good, sizeof(good));
    /* s18_persist_failure_fails_closed (not observable as root). */
    if (geteuid() != 0) {
        chmod(dir, 0500);
        uint64_t keep = 0;
        CHECK(member_store_boot_decode(good, sizeof(good), &keep) == EFS_OK, "decode");
        CHECK(member_store_boot(&ms, 1, &inc2, err, sizeof(err)) == EFS_ERR_IO,
              "persistence failure stops startup");
        chmod(dir, 0755);
        CHECK(read_file(dir, "boot.bin", now, sizeof(now)) == sizeof(good) &&
              !memcmp(now, good, sizeof(good)), "failed persistence leaves the counter");
    } else {
        printf("test_d39_admission: persistence-failure check NOTRUN as root\n");
    }
    member_store_close(&ms);

    /* c2_uninitialized_store_with_counter_refused */
    store_dir(dir, sizeof(dir), "crashinit");
    mkdir(dir, 0755);
    write_file(dir, "boot.bin", good, sizeof(good));
    CHECK(member_store_open(&ms2, dir, 1, a.digest, err, sizeof(err)) == EFS_ERR_UNSUPPORTED &&
          strstr(err, "never initialized"), "uninitialized store with a counter refused");
    /* A store of another format (Raft/KV data, no identity) is refused. */
    store_dir(dir, sizeof(dir), "otherformat");
    mkdir(dir, 0755);
    snprintf(p, sizeof(p), "%s/kv", dir);
    mkdir(p, 0755);
    CHECK(member_store_open(&ms2, dir, 1, a.digest, err, sizeof(err)) == EFS_ERR_UNSUPPORTED,
          "store without identity but with data refused");
    printf("test_d39_admission: C2/C3 member store PASS\n");
}

/* ---------------------------------------------------------- C4-C7 gate */

struct pipe_ { struct efs_conn *srv, *cli; struct efs_decl_gate g; };

static void pipe_open(struct pipe_ *p)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    p->srv = efs_conn_wrap_tcp(sv[1], 1);
    p->cli = efs_conn_wrap_tcp(sv[0], 0);
    CHECK(p->srv && p->cli, "wrap");
    memset(&p->g, 0, sizeof(p->g));
}

static void pipe_close(struct pipe_ *p)
{
    efs_conn_destroy(p->srv);
    efs_conn_destroy(p->cli);
}

static int serve(struct pipe_ *p)
{
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t len = 0;
    CHECK(efs_conn_recv_msg(p->srv, &type, &payload, &len) == 0, "server recv");
    int v = efs_decl_gate_frame(&p->g, p->srv, type, payload, len);
    free(payload);
    return v;
}

static int declare(struct pipe_ *p, uint8_t role, const struct efs_identity *id,
                   struct efs_msg_client_hello_reply *rep)
{
    struct efs_msg_client_hello h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "EFSP", 4);
    h.version = EFS_PROTOCOL_VERSION;
    h.role = role;
    if (id)
        h.id = *id;
    CHECK(efs_conn_send_msg(p->cli, EFS_MSG_CLIENT_HELLO, &h, sizeof(h)) == 0, "send hello");
    int v = serve(p);
    uint8_t type = 0;
    void *r = NULL;
    uint32_t len = 0;
    CHECK(efs_conn_recv_msg(p->cli, &type, &r, &len) == 0 && type == EFS_MSG_CLIENT_HELLO_REPLY &&
          len == sizeof(*rep), "declaration reply");
    memcpy(rep, r, sizeof(*rep));
    free(r);
    return v;
}

static int frame(struct pipe_ *p, uint8_t type, const void *payload, uint32_t len)
{
    CHECK(efs_conn_send_msg(p->cli, type, payload, len) == 0, "send frame");
    return serve(p);
}

static struct efs_identity ident(const struct efs_cluster_conf *c, uint32_t m, uint64_t inc,
                                 uint64_t ctr, int pair_known, uint64_t salt)
{
    struct efs_identity id;
    memset(&id, 0, sizeof(id));
    id.member_id = m;
    if (m >= 1 && m <= c->n) {
        efs_cluster_addr_canonical(c->m[m - 1].addr, &id.endpoint_ip);
        id.endpoint_port = c->m[m - 1].port;
    }
    id.incarnation = inc;
    id.counter = ctr;
    id.pair_known = (uint8_t)pair_known;
    id.salt = pair_known ? salt : 0;
    memcpy(id.digest, c->digest, 32);
    return id;
}

static int peer_declares(const struct efs_identity *id, uint8_t *channel)
{
    struct pipe_ p;
    struct efs_msg_client_hello_reply rep;
    pipe_open(&p);
    int v = declare(&p, EFS_ROLE_PEER, id, &rep);
    if (channel)
        *channel = rep.channel;
    if (v != EFS_GATE_ANSWERED)
        CHECK(rep.rc != EFS_OK && rep.id.member_id == adm_local_id(),
              "a refusal names the responder's identity");
    pipe_close(&p);
    return v == EFS_GATE_ANSWERED;
}

/* A wire Raft frame header: [type][group][vote][flags][from][to][term][boot]. */
static void raft_hdr(uint8_t *f, uint32_t from_member, uint32_t to_member, uint64_t boot)
{
    memset(f, 0, 80);
    f[0] = 2; /* any Raft message type */
    uint32_t from = from_member - 1, to = to_member - 1;
    for (int i = 0; i < 4; i++) {
        f[4 + i] = (uint8_t)(from >> (24 - 8 * i));
        f[8 + i] = (uint8_t)(to >> (24 - 8 * i));
    }
    for (int i = 0; i < 8; i++)
        f[20 + i] = (uint8_t)(boot >> (56 - 8 * i));
}

static void test_gate(void)
{
    struct efs_cluster_conf c3, c3b;
    struct pipe_ p, q, r;
    struct efs_msg_client_hello_reply rep;
    char dir[4096], err[1024];
    struct member_store ms;
    uint8_t ch = 0, rf[80];
    const uint64_t P = 0x5151, Q = 0x7272;
    conf_from(&c3, "1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17432\n");
    conf_from(&c3b, "1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17433\n");
    store_dir(dir, sizeof(dir), "gate/mdraft");
    char parent[4096];
    store_dir(parent, sizeof(parent), "gate");
    mkdir(parent, 0755);
    CHECK(member_store_open(&ms, dir, 1, c3.digest, err, sizeof(err)) == EFS_OK, err);
    CHECK(adm_init(&c3, 1, 100, &ms) == EFS_OK, "adm_init");
    efs_decl_gate_set_admission(&adm_ops);

    /* s12_unconfigured_member_or_endpoint_refused, s5_different_lists_cannot_form */
    struct efs_identity id = ident(&c3, 2, 50, 0, 0, 0);
    id.member_id = 4;
    CHECK(!peer_declares(&id, NULL), "member 4 is not configured");
    id = ident(&c3, 1, 50, 0, 0, 0);
    CHECK(!peer_declares(&id, NULL), "a declaration as the local member is refused");
    id = ident(&c3, 2, 50, 0, 0, 0);
    id.endpoint_port = 17434;
    CHECK(!peer_declares(&id, NULL), "endpoint is not member 2's configured record");
    id = ident(&c3b, 2, 50, 0, 0, 0);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_PEER, &id, &rep) == EFS_GATE_REFUSE && rep.rc == EFS_ERR_FENCED &&
          !memcmp(rep.id.digest, c3.digest, 32), "a different digest is refused, both named");
    CHECK(!p.g.declared && !adm_has_binding(2), "a refusal changes nothing");
    pipe_close(&p);

    /* Mgmt in any state (s10_mgmt_works_unformed_clients_and_peer_rpcs_refused,
     * s12_mgmt_allowlist). */
    struct efs_identity mg;
    memset(&mg, 0, sizeof(mg));
    memcpy(mg.digest, c3b.digest, 32);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_MGMT, &mg, &rep) == EFS_GATE_REFUSE, "mgmt with another digest");
    pipe_close(&p);
    memcpy(mg.digest, c3.digest, 32);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_MGMT, &mg, &rep) == EFS_GATE_ANSWERED &&
          rep.channel == EFS_CHANNEL_MGMT && !rep.complete, "mgmt admitted before formation");
    CHECK(frame(&p, EFS_MSG_STATUS, NULL, 0) == EFS_GATE_DISPATCH, "mgmt STATUS");
    CHECK(frame(&p, EFS_MSG_RAFT_MKFS, NULL, 0) == EFS_GATE_DISPATCH, "mgmt RAFT_MKFS");
    struct efs_msg_formation_read fr = {.group = 2};
    CHECK(frame(&p, EFS_MSG_FORMATION_READ, &fr, sizeof(fr)) == EFS_GATE_DISPATCH,
          "mgmt FORMATION_READ");
    CHECK(frame(&p, EFS_MSG_RAFT_STEP_DOWN, &fr, sizeof(fr)) == EFS_GATE_DISPATCH,
          "mgmt RAFT_STEP_DOWN");
    uint8_t submit[2] = {0, 18};
    CHECK(frame(&p, EFS_MSG_RAFT_MKFS, submit, 2) == EFS_GATE_REFUSE,
          "mgmt metadata command before formation refused");
    pipe_close(&p);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_MGMT, &mg, &rep) == EFS_GATE_ANSWERED, "mgmt again");
    CHECK(frame(&p, EFS_MSG_GC_INODE_BATCH, NULL, 0) == EFS_GATE_REFUSE, "peer-only type refused");
    pipe_close(&p);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_MGMT, &mg, &rep) == EFS_GATE_ANSWERED, "mgmt again");
    CHECK(frame(&p, 16 /* JOIN */, NULL, 0) == EFS_GATE_REFUSE, "JOIN: no membership change");
    pipe_close(&p);
    /* A client is refused until formation completes. */
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_CLIENT, NULL, &rep) == EFS_GATE_REFUSE &&
          rep.rc == EFS_ERR_NOT_FORMED, "client refused: not formed");
    pipe_close(&p);
    /* One declaration per connection. */
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_MGMT, &mg, &rep) == EFS_GATE_ANSWERED, "mgmt");
    CHECK(declare(&p, EFS_ROLE_MGMT, &mg, &rep) == EFS_GATE_REFUSE, "second declaration refused");
    pipe_close(&p);

    /* C5 sequence on member 3 while the local pair is unknown. */
    id = ident(&c3, 3, 9, 0, 0, 0);
    pipe_open(&q); /* connection C, declared unknown */
    CHECK(declare(&q, EFS_ROLE_PEER, &id, &rep) == EFS_GATE_ANSWERED &&
          rep.channel == EFS_CHANNEL_BOOTSTRAP && rep.id.member_id == 1 && rep.id.incarnation == 100,
          "bootstrap channel; the reply names the responder");
    CHECK(adm_fence_word(3) == 9, "fence word follows the binding");
    raft_hdr(rf, 3, 1, 9);
    CHECK(frame(&q, EFS_MSG_RAFT, rf, sizeof(rf)) == EFS_GATE_DISPATCH, "Raft on bootstrap");
    raft_hdr(rf, 2, 1, 9);
    CHECK(frame(&q, EFS_MSG_RAFT, rf, sizeof(rf)) == EFS_GATE_REFUSE,
          "a frame naming another sender is misdirected");
    pipe_close(&q);
    pipe_open(&q);
    CHECK(declare(&q, EFS_ROLE_PEER, &id, &rep) == EFS_GATE_ANSWERED, "C redeclared");
    struct efs_identity tagC = id; /* an item queued in the inbox from C */
    CHECK(adm_check_control(&tagC) == EFS_OK, "queued item valid now");
    /* s19_identical_redeclaration_is_idempotent */
    CHECK(peer_declares(&id, NULL), "identical redeclaration");
    /* s19_unknown_to_known_at_higher_counter_accepted (connection D) */
    struct efs_identity idP = ident(&c3, 3, 9, 1, 1, P);
    CHECK(peer_declares(&idP, &ch) && ch == EFS_CHANNEL_CATCHUP, "unknown -> P, catch-up");
    /* s16_old_connection_none_frames_refused_after_P and
     * s16_frame_paused_in_inbox_across_install_refused_at_effect */
    raft_hdr(rf, 3, 1, 9);
    CHECK(frame(&q, EFS_MSG_RAFT, rf, sizeof(rf)) == EFS_GATE_REFUSE,
          "old unknown connection refused once the binding carries P");
    CHECK(adm_check_control(&tagC) == EFS_ERR_FENCED,
          "item queued before the declaration refused at drain (check at dispatch is not enough)");
    pipe_close(&q);
    /* s20_genuine_downgrade_refused; s19 step 3b at any incarnation */
    CHECK(!peer_declares(&id, NULL), "member once bound to P may not declare unknown");
    struct efs_identity down = ident(&c3, 3, 10, 0, 0, 0);
    CHECK(!peer_declares(&down, NULL), "not even at a newer incarnation");
    struct efs_identity idQ = ident(&c3, 3, 10, 0, 1, Q);
    CHECK(!peer_declares(&idQ, NULL), "nor with another pair");
    /* s19_equal_counter_different_pair_refused_at_step4 (member 2) */
    struct efs_identity m2 = ident(&c3, 2, 50, 2, 0, 0);
    CHECK(peer_declares(&m2, NULL), "member 2 at counter 2, unknown");
    struct efs_identity m2P = ident(&c3, 2, 50, 2, 1, P);
    CHECK(!peer_declares(&m2P, NULL), "equal counter, different pair: inconsistent");
    struct efs_identity m2old = ident(&c3, 2, 50, 1, 0, 0);
    CHECK(!peer_declares(&m2old, NULL), "stale counter");
    struct efs_identity m2stale = ident(&c3, 2, 49, 9, 0, 0);
    CHECK(!peer_declares(&m2stale, NULL), "stale incarnation");
    /* s20_lagging_peer_is_not_a_downgrade comes below, once the local pair is P. */

    /* C3/C10: formation install and complete (s10_group2_paused_node1_stays_forming). */
    uint64_t gen0 = adm_generation();
    CHECK(adm_formation_install(0, P, c3.digest) == EFS_OK, "MKFS installed");
    CHECK(adm_pair(NULL) && !adm_complete() && adm_state() == EFS_MEMBER_FORMING &&
          adm_counter() == 1 && adm_generation() == gen0 + 1,
          "pair learned: forming, counter + 1, outgoing connections retired");
    /* s8_mismatched_persisted_salt_with_matching_configuration */
    CHECK(adm_formation_observe(1, Q, c3.digest) == EFS_ERR_FENCED, "conflicting SALT pair stops");
    CHECK(adm_formation_install(1, P, c3b.digest) == EFS_ERR_FENCED, "another digest stops");
    CHECK(!adm_complete() && ms.salt == P, "nothing changed");
    /* s20_lagging_peer_is_not_a_downgrade: member 2 (no pair in its binding) */
    struct efs_identity m2n = ident(&c3, 2, 50, 3, 0, 0);
    CHECK(peer_declares(&m2n, &ch) && ch == EFS_CHANNEL_CATCHUP, "P -> unknown is catch-up");
    /* s14_both_know_P_group2_paused_application_refused */
    struct efs_identity m2p = ident(&c3, 2, 50, 4, 1, P);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_PEER, &m2p, &rep) == EFS_GATE_ANSWERED &&
          rep.channel == EFS_CHANNEL_FORMED && rep.id.pair_known && rep.id.salt == P,
          "formed channel");
    CHECK(frame(&p, EFS_MSG_INODE_LOOKUP, rf, 8) == EFS_GATE_REFUSE,
          "application refused while formation is not complete");
    pipe_close(&p);
    /* s19_newer_incarnation_with_conflicting_pair_refused_at_step3 (3a) */
    struct efs_identity m2Q = ident(&c3, 2, 60, 0, 1, Q);
    CHECK(!peer_declares(&m2Q, NULL), "conflicts with the local pair");
    uint32_t state_before = adm_state();
    CHECK(adm_formation_install(1, P, c3.digest) == EFS_OK && adm_complete() &&
          adm_state() == EFS_MEMBER_FORMED && ms.state == EFS_MEMBER_FORMED && state_before == EFS_MEMBER_FORMING,
          "SALT installed: complete, formed persisted");

    /* s26_counter_only_transition_raft_continues_application_refused */
    struct efs_identity a1 = ident(&c3, 2, 70, 0, 1, P), a2 = ident(&c3, 2, 70, 1, 1, P);
    pipe_open(&p);
    CHECK(declare(&p, EFS_ROLE_PEER, &a1, &rep) == EFS_GATE_ANSWERED, "connection A");
    CHECK(frame(&p, EFS_MSG_INODE_LOOKUP, rf, 8) == EFS_GATE_DISPATCH, "application on A");
    pipe_open(&q);
    CHECK(declare(&q, EFS_ROLE_PEER, &a2, &rep) == EFS_GATE_ANSWERED, "connection B, counter 1");
    raft_hdr(rf, 2, 1, 70);
    CHECK(frame(&p, EFS_MSG_RAFT, rf, sizeof(rf)) == EFS_GATE_DISPATCH, "Raft on A continues");
    CHECK(frame(&p, EFS_MSG_INODE_LOOKUP, rf, 8) == EFS_GATE_REFUSE,
          "application on A refused: not the current binding");
    CHECK(frame(&q, EFS_MSG_INODE_LOOKUP, rf, 8) == EFS_GATE_DISPATCH, "application on B");
    pipe_close(&p);
    /* s26_older_incarnation_frame_after_newer_declaration */
    struct efs_identity a3 = ident(&c3, 2, 71, 0, 1, P);
    CHECK(peer_declares(&a3, NULL), "member 2 restarted (incarnation 71)");
    raft_hdr(rf, 2, 1, 70);
    CHECK(frame(&q, EFS_MSG_RAFT, rf, sizeof(rf)) == EFS_GATE_REFUSE,
          "old-incarnation frames are stale everywhere");
    CHECK(adm_check_control(&a2) == EFS_ERR_STALE && adm_check_app(&a2) == EFS_ERR_STALE,
          "queued items of the old incarnation refused");
    CHECK(adm_fence_word(2) == 71, "fence word moved with the binding");
    struct efs_identity reord = ident(&c3, 2, 70, 9, 1, P);
    CHECK(!peer_declares(&reord, NULL), "a reordered older declaration cannot lower the mark");
    struct efs_identity hi = ident(&c3, 2, 72, 0, 1, P);
    CHECK(adm_check_control(&hi) == EFS_ERR_UNREGISTERED, "a newer tuple without declaration: redeclare");
    pipe_close(&q);
    /* Client after completion (s8_declared_client_attempts_peer_traffic). */
    pipe_open(&r);
    CHECK(declare(&r, EFS_ROLE_CLIENT, NULL, &rep) == EFS_GATE_ANSWERED && rep.complete,
          "client admitted once complete");
    CHECK(frame(&r, EFS_MSG_INODE_LOOKUP, rf, 8) == EFS_GATE_DISPATCH, "client LOOKUP");
    CHECK(frame(&r, EFS_MSG_RAFT, rf, sizeof(rf)) == EFS_GATE_REFUSE, "client Raft refused");
    pipe_close(&r);

    /* s14_configured_endpoint_answering_as_wrong_member and reply checks. */
    struct efs_msg_client_hello_reply rr;
    memset(&rr, 0, sizeof(rr));
    rr.rc = EFS_OK;
    rr.version = EFS_PROTOCOL_VERSION;
    rr.role = EFS_ROLE_PEER;
    rr.id = ident(&c3, 3, 20, 0, 1, P);
    CHECK(adm_check_reply(2, &rr, err, sizeof(err)) != EFS_OK, "reply from member 3, intended 2");
    rr.id = ident(&c3b, 2, 80, 0, 1, P);
    CHECK(adm_check_reply(2, &rr, err, sizeof(err)) != EFS_OK, "reply with another digest");
    rr.id = ident(&c3, 2, 80, 0, 1, P);
    CHECK(adm_check_reply(2, &rr, err, sizeof(err)) == EFS_OK && adm_fence_word(2) == 80,
          "a valid reply is a declaration from the responder");
    rr.rc = EFS_ERR_FENCED;
    CHECK(adm_check_reply(2, &rr, err, sizeof(err)) == EFS_ERR_FENCED, "a refusal is surfaced");

    /* c10_formed_restart_verifies_before_service */
    member_store_close(&ms);
    CHECK(member_store_open(&ms, dir, 1, c3.digest, err, sizeof(err)) == EFS_OK &&
          ms.state == EFS_MEMBER_FORMED && ms.salt == P, "reopened formed store");
    CHECK(adm_init(&c3, 1, 101, &ms) == EFS_OK && !adm_complete() && adm_pair(NULL),
          "restart: pair known, complete not yet");
    pipe_open(&r);
    CHECK(declare(&r, EFS_ROLE_CLIENT, NULL, &rep) == EFS_GATE_REFUSE,
          "no client service before complete is re-derived");
    pipe_close(&r);
    CHECK(adm_formation_observe(0, P, c3.digest) == EFS_OK &&
          adm_formation_observe(1, P, c3.digest) == EFS_OK && adm_complete() && adm_counter() == 0,
          "re-derived from the applied records: no identity event");
    CHECK(adm_formation_observe(0, Q, c3.digest) == EFS_ERR_FENCED, "a conflicting record stops");
    member_store_close(&ms);
    printf("test_d39_admission: C4-C7 declaration gate, predicates and formation PASS\n");
}

/* ---------------------------------------------------- peer pool (C4) */

struct child_conn { int fd; int report; };

static void *child_serve(void *arg)
{
    struct child_conn *cc = arg;
    struct efs_conn *c = efs_conn_wrap_tcp(cc->fd, 1);
    struct efs_decl_gate g;
    memset(&g, 0, sizeof(g));
    for (;;) {
        uint8_t type = 0;
        void *payload = NULL;
        uint32_t len = 0;
        if (efs_conn_recv_msg(c, &type, &payload, &len) != 0)
            break;
        int v = efs_decl_gate_frame(&g, c, type, payload, len);
        free(payload);
        if (v == EFS_GATE_REFUSE)
            break;
        if (v == EFS_GATE_ANSWERED) {
            char line[96];
            int n = snprintf(line, sizeof(line), "%u %llu %llu %u\n", g.id.member_id,
                             (unsigned long long)g.id.incarnation,
                             (unsigned long long)g.id.counter, g.id.pair_known);
            if (write(cc->report, line, (size_t)n) != n)
                break;
        } else if (type == EFS_MSG_HEARTBEAT) {
            efs_conn_send_msg(c, EFS_MSG_HEARTBEAT_ACK, NULL, 0);
        }
    }
    efs_conn_destroy(c);
    free(cc);
    return NULL;
}

/* An admitter in its own process: member `as` of conf, serving lfd. */
static pid_t admitter(int lfd, const struct efs_cluster_conf *conf, uint32_t as, int report)
{
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid)
        return pid;
    adm_init(conf, as, 500, NULL);
    efs_decl_gate_set_admission(&adm_ops);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0)
            _exit(0);
        struct child_conn *cc = malloc(sizeof(*cc));
        cc->fd = fd;
        cc->report = report;
        pthread_t t;
        pthread_create(&t, NULL, child_serve, cc);
        pthread_detach(t);
    }
}

static int read_report(int fd, char *buf, size_t cap)
{
    usleep(100 * 1000);
    ssize_t n = read(fd, buf, cap - 1);
    buf[n > 0 ? n : 0] = '\0';
    return n > 0 ? (int)n : 0;
}

static void test_peer_pool(void)
{
    int lfd = efs_listen_tcp("127.0.0.1", 0, 16);
    CHECK(lfd >= 0, "listen");
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    CHECK(getsockname(lfd, (struct sockaddr *)&sa, &sl) == 0, "getsockname");
    unsigned port = ntohs(sa.sin_port);
    char text[256], other[256], buf[512];
    snprintf(text, sizeof(text), "1 127.0.0.2:1\n2 127.0.0.1:%u\n3 127.0.0.3:1\n", port);
    snprintf(other, sizeof(other), "1 127.0.0.2:1\n2 127.0.0.1:%u\n3 127.0.0.3:2\n", port);
    struct efs_cluster_conf conf, conf_other;
    conf_from(&conf, text);
    conf_from(&conf_other, other);
    int rp[2];
    CHECK(pipe(rp) == 0, "pipe");
    fcntl(rp[0], F_SETFL, O_NONBLOCK);
    server_peer_pool_init();
    efs_cluster_conf_set_current(&conf);
    CHECK(adm_init(&conf, 1, 900, NULL) == EFS_OK, "declarer member 1");
    efs_decl_gate_set_admission(&adm_ops);

    /* The intended member at its endpoint, same configuration: used. */
    pid_t pid = admitter(lfd, &conf, 2, rp[1]);
    struct efs_conn *c = server_peer_conn_new("127.0.0.1", (uint16_t)port);
    CHECK(c != NULL, "peer connection declared and validated");
    CHECK(adm_has_binding(2) && adm_fence_word(2) == 500,
          "the reply was accepted as a declaration from member 2");
    CHECK(read_report(rp[0], buf, sizeof(buf)) && !strcmp(buf, "1 900 0 0\n"),
          "member 2 admitted member 1's tuple");
    efs_conn_destroy(c);
    /* Pooled connections are retired by an identity event and redeclared. */
    struct efs_conn *p1 = server_peer_conn_get("127.0.0.1", (uint16_t)port);
    CHECK(p1 != NULL, "pooled connection");
    server_peer_conn_release("127.0.0.1", (uint16_t)port, p1);
    struct efs_conn *p2 = server_peer_conn_get("127.0.0.1", (uint16_t)port);
    CHECK(p2 == p1, "reused while the identity is unchanged");
    server_peer_conn_release("127.0.0.1", (uint16_t)port, p2);
    (void)read_report(rp[0], buf, sizeof(buf));
    CHECK(adm_formation_install(0, 4242, conf.digest) == EFS_OK, "identity event (pair learned)");
    struct efs_conn *p3 = server_peer_conn_get("127.0.0.1", (uint16_t)port);
    CHECK(p3 != NULL, "pooled connection after the identity event");
    CHECK(read_report(rp[0], buf, sizeof(buf)) && !strcmp(buf, "1 900 1 1\n"),
          "the pool redeclared the new tuple (counter 1, pair known)");
    server_peer_conn_release("127.0.0.1", (uint16_t)port, p3);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    /* The configured endpoint answering as the wrong member: discarded. */
    CHECK(adm_init(&conf, 1, 901, NULL) == EFS_OK, "declarer reset");
    pid = admitter(lfd, &conf, 3, rp[1]);
    c = server_peer_conn_new("127.0.0.1", (uint16_t)port);
    CHECK(c == NULL && !adm_has_binding(3) && !adm_has_binding(2),
          "a reply from member 3 at member 2's endpoint is never used");
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    /* Another configuration at the intended endpoint: refused both ways. */
    pid = admitter(lfd, &conf_other, 2, rp[1]);
    c = server_peer_conn_new("127.0.0.1", (uint16_t)port);
    CHECK(c == NULL && !adm_has_binding(2), "different digest: no connection, no binding");
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    /* An unconfigured endpoint is never dialed. */
    CHECK(server_peer_conn_new("127.0.0.1", (uint16_t)(port == 65535 ? 1 : port + 1)) == NULL,
          "unconfigured endpoint");
    close(lfd);
    printf("test_d39_admission: peer pool declare-and-validate PASS\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "conf")) {
        static char text[EFS_CLUSTER_CONF_MAX_BYTES + 1];
        size_t n = fread(text, 1, sizeof(text) - 1, stdin);
        struct efs_cluster_conf c;
        char err[256], h[65];
        if (efs_cluster_conf_parse(text, n, &c, err, sizeof(err)) != EFS_OK) {
            printf("REFUSED %s\n", err);
            return 0;
        }
        hex(c.digest, h);
        printf("OK %s\n", h);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    efs_test_path(tmp_root, sizeof(tmp_root), "efs-d39-admission-XXXXXX");
    CHECK(efs_test_mkdtemp(tmp_root) != NULL, "mkdtemp");
    test_conf();
    test_store();
    test_gate();
    test_peer_pool();
    printf("test_d39_admission: OK (%d checks)\n", checks);
    return 0;
}
