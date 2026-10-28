/* K35 (D42): fsync proves each stripe the file's writes touched.
 * Deterministic, against stubbed storage nodes 1..7: COMMIT only on the
 * nodes holding the file's unproven fragments; success with two durable
 * fragments only after the third is covered by a durable degraded mark;
 * EIO (stripe named in the log) with fewer than two, with an uncertain or
 * re-incarnated member holding a needed fragment, or without the mark.
 * Built with a small table so the full-table sweep is reached. */
#define STRIPE_CAP 16u
#include "../src/client/data_commit.c"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#define NN 8
static struct efs_conn conns[NN];
static int commits[NN], fail_io[NN], fail_net[NN], die_in_commit[NN];
static int hold[NN];          /* reply withheld while set */
static int down[NN];          /* in cooldown and not listening */
static uint64_t epochs[NN];
static __thread uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline = d; }
struct efs_conn *efs_client_conn_get(efs_node_id_t id) { assert(id && id < NN); return down[id] ? NULL : &conns[id]; }
int efs_client_node_reprobe(efs_node_id_t id) { return !down[id]; }
void efs_client_conn_release(efs_node_id_t id, struct efs_conn *c) { (void)id; (void)c; }
void efs_client_conn_drop(efs_node_id_t id, struct efs_conn *c) { (void)id; (void)c; }
int efs_conn_send_msg(struct efs_conn *c, uint8_t type, const void *p, uint32_t n)
{
    int id = (int)(c - conns);
    assert(type == EFS_MSG_DATA_COMMIT && !p && !n);
    __atomic_fetch_add(&commits[id], 1, __ATOMIC_ACQ_REL);
    return __atomic_load_n(&fail_net[id], __ATOMIC_ACQUIRE) ? -1 : 0;
}
int efs_conn_recv_msg(struct efs_conn *c, uint8_t *type, void **p, uint32_t *n)
{
    int id = (int)(c - conns);
    while (__atomic_load_n(&hold[id], __ATOMIC_ACQUIRE))
        usleep(1000);
    if (die_in_commit[id]) { /* the member died after the request arrived */
        down[id] = 1;
        return -1;
    }
    struct efs_msg_data_commit_reply *r = calloc(1, sizeof(*r));
    r->rc = __atomic_load_n(&fail_io[id], __ATOMIC_ACQUIRE) ? EFS_ERR_IO : EFS_OK;
    r->roots = 1;
    r->boot_epoch = epochs[id];
    *type = EFS_MSG_DATA_COMMIT_REPLY; *p = r; *n = sizeof(*r);
    return 0;
}
static int terminal_rc = EFS_ERR_BUSY, terminal_calls;
int efs_client_put_terminal(efs_ino_t ino, uint64_t gen, uint32_t ci, uint64_t object)
{
    assert(ino && gen && object); (void)ci;
    terminal_calls++;
    return terminal_rc;
}
static int marks, mark_rc = EFS_OK, publish_during_mark;
static efs_ino_t mark_ino; static uint32_t mark_ci, mark_fi; static efs_node_id_t mark_node;
int efs_client_rpc_degraded_mark(efs_ino_t ino, uint64_t inode_gen, uint64_t object,
                                 uint32_t ci, uint32_t fi, efs_node_id_t node)
{
    assert(inode_gen == 5 && object == ino * 1000 + ci);
    marks++; mark_ino = ino; mark_ci = ci; mark_fi = fi; mark_node = node;
    if (publish_during_mark) {
        struct efs_chunk_rec rec = {.ino=ino, .chunk_index=ci,
            .chunk_generation=object, .file_generation=inode_gen};
        publish_during_mark = 0;
        efs_client_stripes_published(&rec, 1);
    }
    return mark_rc;
}

static void reset(void) { memset(commits, 0, sizeof(commits)); marks = 0; }
static void heal(void)
{
    memset(fail_io, 0, sizeof(fail_io)); memset(fail_net, 0, sizeof(fail_net));
    memset(die_in_commit, 0, sizeof(die_in_commit)); memset(down, 0, sizeof(down));
}
/* One acknowledged PUT object: ack bit i set = fragment i acknowledged. */
static int put_unpublished(efs_ino_t ino, uint32_t ci, efs_node_id_t a, efs_node_id_t b, efs_node_id_t c,
               unsigned ack)
{
    struct efs_put_ticket t;
    uint64_t seq[3] = {0}, ep[3] = {0};
    memset(&t, 0, sizeof(t));
    t.ino = ino; t.ci = ci; t.inode_gen = 5; t.object = ino * 1000 + ci;
    t.nodes[0] = a; t.nodes[1] = b; t.nodes[2] = c;
    for (int i = 0; i < 3; i++)
        if (ack & (1u << i)) { seq[i] = efs_client_commit_note(t.nodes[i]); ep[i] = epochs[t.nodes[i]]; }
    return efs_client_stripe_note(&t, seq, ep);
}
/* Existing cases model an acknowledged REPORT before their fsync. */
static int put(efs_ino_t ino, uint32_t ci, efs_node_id_t a, efs_node_id_t b, efs_node_id_t c,
               unsigned ack)
{
    int rc = put_unpublished(ino, ci, a, b, c, ack);
    if (rc == EFS_OK) {
        struct efs_chunk_rec rec = {.ino=ino, .chunk_index=ci,
            .chunk_generation=ino*1000+ci, .file_generation=5};
        efs_client_stripes_published(&rec, 1);
    }
    return rc;
}
static int live(void) { pthread_mutex_lock(&commit_mu); int n = (int)g_st_live; pthread_mutex_unlock(&commit_mu); return n; }

struct fsync_thread { pthread_t t; efs_ino_t ino; int rc, done; };
static void *do_fsync(void *arg)
{
    struct fsync_thread *f = arg;
    f->rc = efs_client_data_commit(f->ino);
    __atomic_store_n(&f->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    for (int i = 1; i < NN; i++) epochs[i] = 101;
    /* Full-table pressure before REPORT: NOT_FOUND is not a retirement
     * certificate. All obligations survive, and the new PUT gets BUSY. */
    for (efs_ino_t ino=900; ino<916; ino++)
        assert(put_unpublished(ino,0,1,2,3,7)==EFS_OK);
    epochs[3]++;
    mark_rc=EFS_ERR_NOT_FOUND;
    reset(); assert(put_unpublished(916,0,1,2,3,7)==EFS_ERR_BUSY);
    assert(marks==16 && live()==16 && stripe_find(900,0,900000));
    mark_rc=EFS_OK;
    for (efs_ino_t ino=900; ino<916; ino++) {
        struct efs_chunk_rec rec={.ino=ino,.chunk_generation=ino*1000,.file_generation=5};
        efs_client_stripes_published(&rec,1);
        assert(efs_client_data_commit(ino)==EFS_OK);
    }
    assert(!live());
    /* A publication acknowledgement during the mark RPC is too late to
     * justify this RPC's earlier NOT_FOUND. Retry must actually mark. */
    assert(put_unpublished(950,0,1,2,3,7)==EFS_OK);
    struct efs_chunk_rec wrong={.ino=950,.chunk_generation=950000,.file_generation=6};
    efs_client_stripes_published(&wrong,1);
    assert(!stripe_find(950,0,950000)->published);
    epochs[3]++; mark_rc=EFS_ERR_NOT_FOUND; publish_during_mark=1;
    reset(); assert(efs_client_data_commit(950)==EFS_ERR_IO);
    assert(live()==1 && stripe_find(950,0,950000)->published);
    mark_rc=EFS_OK;
    reset(); assert(efs_client_data_commit(950)==EFS_OK && marks==1 && !live());
    /* A replay's abandoned object: local done/missing state does not
     * authorize retirement. A durable terminal ticket AND a new absence
     * check do; a present mapping still needs its degraded mark. */
    assert(put_unpublished(951,0,1,2,3,7)==EFS_OK);
    down[3]=1;mark_rc=EFS_ERR_NOT_FOUND;terminal_rc=EFS_ERR_BUSY;
    reset();assert(efs_client_data_commit(951)==EFS_ERR_IO && live()==1 && marks==1);
    terminal_rc=EFS_ERR_NET;
    reset();assert(efs_client_data_commit(951)==EFS_ERR_IO && live()==1 && marks==1);
    terminal_rc=EFS_OK;
    reset();assert(efs_client_data_commit(951)==EFS_OK && !live() && marks==2);
    assert(put_unpublished(952,0,1,2,3,7)==EFS_OK);
    struct efs_put_ticket wrong_ticket={.ino=952,.inode_gen=6,.object=952000};
    efs_client_stripe_closed(&wrong_ticket);
    assert(!stripe_find(952,0,952000)->closed);
    wrong_ticket.inode_gen=5;efs_client_stripe_closed(&wrong_ticket);
    reset();assert(efs_client_data_commit(952)==EFS_OK && !live() && marks==1);
    terminal_rc=EFS_ERR_BUSY;mark_rc=EFS_OK;heal();
    /* Nothing written: nothing to prove, no COMMIT. */
    reset(); assert(efs_client_data_commit(9) == EFS_OK && !commits[1]);
    assert(efs_client_data_commit(0) == EFS_ERR_INVAL);
    /* All up: COMMIT only the stripe's three nodes, never node 4. */
    assert(put(10, 0, 1, 2, 3, 7) == EFS_OK);
    reset(); assert(efs_client_data_commit(10) == EFS_OK);
    assert(commits[1] == 1 && commits[2] == 1 && commits[3] == 1 && !commits[4] && !marks);
    reset(); assert(efs_client_data_commit(10) == EFS_OK && !commits[1] && !live());
    /* An unrelated member down (node 4, four members): success, no mark. */
    down[4] = 1;
    assert(put(11, 0, 1, 2, 3, 7) == EFS_OK);
    reset(); assert(efs_client_data_commit(11) == EFS_OK && !commits[4] && !marks);
    heal();
    /* A member down before the write: acknowledged on 1 and 2, published
     * degraded (W154 mark in the publication's batch); node 3 is never
     * asked and no further mark is needed. */
    down[3] = 1;
    assert(put(12, 0, 1, 2, 3, 3) == EFS_OK);
    reset(); assert(efs_client_data_commit(12) == EFS_OK);
    assert(commits[1] == 1 && commits[2] == 1 && !commits[3] && !marks && !live());
    heal();
    /* A member down after the write, before fsync: two durable, the third
     * unproven, so the degraded mark is written before success. */
    assert(put(13, 2, 1, 2, 3, 7) == EFS_OK);
    down[3] = 1;
    reset(); assert(efs_client_data_commit(13) == EFS_OK);
    assert(marks == 1 && mark_ino == 13 && mark_ci == 2 && mark_fi == 2 && mark_node == 3 && !live());
    heal();
    /* A member that dies during COMMIT (request sent, no reply). */
    assert(put(14, 0, 1, 2, 3, 7) == EFS_OK);
    die_in_commit[2] = 1;
    reset(); assert(efs_client_data_commit(14) == EFS_OK);
    assert(commits[2] == 1 && marks == 1 && mark_fi == 1 && mark_node == 2 && !live());
    heal();
    /* Two members down: EIO, no mark, and EIO again while they stay away;
     * the record is kept, so once both return with the same incarnation
     * their fragments are proven durable after all, without a mark. */
    assert(put(15, 0, 1, 2, 3, 7) == EFS_OK);
    down[2] = down[3] = 1;
    reset(); assert(efs_client_data_commit(15) == EFS_ERR_IO && !marks);
    reset(); assert(efs_client_data_commit(15) == EFS_ERR_IO && !marks && live() == 1);
    heal();
    reset(); assert(efs_client_data_commit(15) == EFS_OK && !marks && !live());
    /* One member down at the write and a second before fsync: one durable
     * fragment is no proof. */
    assert(put(16, 0, 1, 2, 3, 3) == EFS_OK);
    fail_net[2] = 1;
    reset(); assert(efs_client_data_commit(16) == EFS_ERR_IO && !marks && !commits[3]);
    heal();
    reset(); assert(efs_client_data_commit(16) == EFS_OK && !live());
    /* Incarnation mismatch: node 3 restarted with a new verifier before its
     * fragment was committed. Not needed: marked, success. Two such members
     * hold needed fragments: EIO for good (never resent, never forgotten). */
    assert(put(17, 0, 1, 2, 3, 7) == EFS_OK);
    epochs[3] = 102;
    reset(); assert(efs_client_data_commit(17) == EFS_OK && marks == 1 && mark_fi == 2);
    assert(put(18, 0, 1, 2, 3, 7) == EFS_OK);
    epochs[2] = 102; epochs[3] = 103;
    reset(); assert(efs_client_data_commit(18) == EFS_ERR_IO && !marks);
    reset(); assert(efs_client_data_commit(18) == EFS_ERR_IO && !marks && live() == 1);
    const int lost = live(); /* ino 18 stays: its proof can never come */
    /* Acknowledgements from the new incarnations are proven as usual. */
    for (int i = 1; i < NN; i++) epochs[i] = 104;
    assert(put(19, 0, 1, 2, 3, 7) == EFS_OK);
    reset(); assert(efs_client_data_commit(19) == EFS_OK && !marks);
    /* Uncertain recovery (COMMIT refused, F23/K36): one member is covered by
     * a mark, two are EIO. */
    assert(put(20, 0, 1, 2, 3, 7) == EFS_OK);
    fail_io[1] = 1;
    reset(); assert(efs_client_data_commit(20) == EFS_OK && marks == 1 && mark_fi == 0 && mark_node == 1);
    assert(put(21, 0, 1, 2, 3, 7) == EFS_OK);
    fail_io[2] = 1;
    reset(); assert(efs_client_data_commit(21) == EFS_ERR_IO && !marks);
    heal();
    reset(); assert(efs_client_data_commit(21) == EFS_OK && !marks && live() == lost);
    /* A mark that cannot be written is no proof; a later fsync writes it. */
    assert(put(22, 0, 1, 2, 3, 7) == EFS_OK);
    down[3] = 1; mark_rc = EFS_ERR_NET;
    reset(); assert(efs_client_data_commit(22) == EFS_ERR_IO && marks == 1);
    mark_rc = EFS_OK;
    reset(); assert(efs_client_data_commit(22) == EFS_OK && marks == 1 && !commits[1]);
    /* The object was rewritten, truncated, packed or unlinked meanwhile:
     * no longer the published version, nothing to mark. */
    assert(put(23, 0, 1, 2, 3, 7) == EFS_OK);
    mark_rc = EFS_ERR_NOT_FOUND;
    reset(); assert(efs_client_data_commit(23) == EFS_OK && marks == 1 && live() == lost);
    mark_rc = EFS_OK;
    heal();
    /* Per file: fsync of one file never asks another file's nodes, and a
     * down node under another file does not fail it. */
    assert(put(24, 0, 1, 2, 3, 7) == EFS_OK);
    assert(put(25, 0, 4, 5, 6, 7) == EFS_OK);
    down[5] = down[6] = 1;
    reset(); assert(efs_client_data_commit(24) == EFS_OK && !commits[4] && !commits[5]);
    reset(); assert(efs_client_data_commit(25) == EFS_ERR_IO && commits[4] == 1);
    heal();
    reset(); assert(efs_client_data_commit(25) == EFS_OK && live() == lost);
    /* Packed small files: many one-chunk files, one member down after the
     * writes; each fsync marks its own stripe. */
    for (efs_ino_t f = 100; f < 108; f++)
        assert(put(f, 0, 1 + f % 3, 1 + (f + 1) % 3, 1 + (f + 2) % 3, 7) == EFS_OK);
    down[2] = 1;
    reset();
    for (efs_ino_t f = 100; f < 108; f++)
        assert(efs_client_data_commit(f) == EFS_OK);
    assert(marks == 8 && live() == lost && commits[3] == 8 && !commits[2]);
    heal();
    /* Concurrent fsyncs of one file both wait for the COMMIT that covers
     * their data; a failure is seen by both. */
    assert(put(30, 0, 1, 2, 3, 7) == EFS_OK);
    hold[1] = 1; reset();
    struct fsync_thread a = {.ino = 30}, b = {.ino = 30};
    pthread_create(&a.t, NULL, do_fsync, &a);
    while (!__atomic_load_n(&commits[1], __ATOMIC_ACQUIRE)) usleep(1000);
    pthread_create(&b.t, NULL, do_fsync, &b);
    while (__atomic_load_n(&commits[1], __ATOMIC_ACQUIRE) < 2) usleep(1000);
    assert(!__atomic_load_n(&a.done, __ATOMIC_ACQUIRE) && !__atomic_load_n(&b.done, __ATOMIC_ACQUIRE));
    __atomic_store_n(&hold[1], 0, __ATOMIC_RELEASE);
    pthread_join(a.t, NULL); pthread_join(b.t, NULL);
    assert(a.rc == EFS_OK && b.rc == EFS_OK && live() == lost);
    assert(put(31, 0, 1, 2, 3, 7) == EFS_OK);
    hold[1] = 1; fail_io[1] = fail_io[2] = 1; reset();
    a = (struct fsync_thread){.ino = 31}; b = (struct fsync_thread){.ino = 31};
    pthread_create(&a.t, NULL, do_fsync, &a); pthread_create(&b.t, NULL, do_fsync, &b);
    while (__atomic_load_n(&commits[1], __ATOMIC_ACQUIRE) < 2) usleep(1000);
    __atomic_store_n(&hold[1], 0, __ATOMIC_RELEASE);
    pthread_join(a.t, NULL); pthread_join(b.t, NULL);
    assert(a.rc == EFS_ERR_IO && b.rc == EFS_ERR_IO);
    heal();
    assert(efs_client_data_commit(31) == EFS_OK && live() == lost);
    /* An acknowledgement after the COMMIT was sent is not covered by it. */
    assert(put(32, 0, 1, 2, 3, 7) == EFS_OK);
    hold[3] = 1; reset();
    a = (struct fsync_thread){.ino = 32};
    pthread_create(&a.t, NULL, do_fsync, &a);
    while (!__atomic_load_n(&commits[3], __ATOMIC_ACQUIRE)) usleep(1000);
    assert(put(32, 1, 1, 2, 3, 7) == EFS_OK); /* acknowledged meanwhile */
    __atomic_store_n(&hold[3], 0, __ATOMIC_RELEASE);
    pthread_join(a.t, NULL);
    assert(a.rc == EFS_OK && live() == lost + 1);   /* chunk 1 waits for its own fsync */
    reset(); assert(efs_client_data_commit(32) == EFS_OK && commits[1] == 1 && live() == lost);
    /* Malformed records are refused, never stored. */
    {
        struct efs_put_ticket t = {.ino = 1, .inode_gen = 5, .object = 1, .nodes = {1, 2, 3}};
        uint64_t one[3] = {1, 0, 0}, two[3] = {1, 2, 0}, ep[3] = {101, 101, 0}, bad[3] = {101, 0, 0};
        assert(efs_client_stripe_note(&t, one, ep) == EFS_ERR_INVAL);
        assert(efs_client_stripe_note(&t, two, bad) == EFS_ERR_INVAL);
        t.nodes[2] = 0;
        assert(efs_client_stripe_note(&t, two, ep) == EFS_ERR_INVAL && live() == lost);
    }
    /* A full table is swept (every record proven) before a new record; a
     * table full of unprovable records refuses the PUT. */
    for (uint32_t ci = 0; ci < STRIPE_CAP - lost; ci++)
        assert(put(40, ci, 1, 2, 3, 7) == EFS_OK);
    assert(live() == (int)STRIPE_CAP);
    reset(); assert(put(41, 0, 1, 2, 3, 7) == EFS_OK && live() == lost + 1 && commits[1] == 1);
    assert(efs_client_data_commit(41) == EFS_OK);
    for (uint32_t ci = 0; ci < STRIPE_CAP - lost; ci++)
        assert(put(42, ci, 1, 2, 3, 3) == EFS_OK);
    down[2] = 1;
    assert(put(43, 0, 1, 2, 3, 7) == EFS_ERR_BUSY && live() == (int)STRIPE_CAP);
    heal();
    assert(put(43, 0, 1, 2, 3, 7) == EFS_OK && live() == lost + 1);
    assert(efs_client_data_commit(43) == EFS_OK && live() == lost);
    puts("data commit (K35): per-stripe proof, COMMIT only on the file's nodes; member down "
         "before/after the write and dying in COMMIT marked degraded; unrelated member "
         "ignored; two down, uncertain or re-incarnated needed fragments and unwritten marks "
         "EIO; superseded objects, packed small files, concurrent fsyncs, late "
         "acknowledgements and the full-table sweep PASS");
    return 0;
}
