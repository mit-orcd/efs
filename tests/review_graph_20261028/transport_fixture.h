/* Frozen baseline transport fixture for the defect discriminator.
 * Source: ef19b1b74 tests/test_data_commit.c; product bookkeeping stays real. */
/* K35 (D42): fsync proves each stripe the file's writes touched.
 * Deterministic, against stubbed storage nodes 1..7: COMMIT only on the
 * nodes holding the file's unproven fragments; success with two durable
 * fragments only after the third is covered by a durable degraded mark;
 * EIO (stripe named in the log) with fewer than two, with an uncertain or
 * re-incarnated member holding a needed fragment, or without the mark.
 * Built with a small table so the full-table sweep is reached. */
#define STRIPE_CAP 16u
#include "../../src/client/data_commit.c"
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
static int marks, mark_rc = EFS_OK;
static efs_ino_t mark_ino; static uint32_t mark_ci, mark_fi; static efs_node_id_t mark_node;
int efs_client_rpc_degraded_mark(efs_ino_t ino, uint64_t inode_gen, uint64_t object,
                                 uint32_t ci, uint32_t fi, efs_node_id_t node)
{
    assert(inode_gen == 5 && object == ino * 1000 + ci);
    marks++; mark_ino = ino; mark_ci = ci; mark_fi = fi; mark_node = node;
    return mark_rc;
}

static void reset(void) { memset(commits, 0, sizeof(commits)); marks = 0; }
static void heal(void)
{
    memset(fail_io, 0, sizeof(fail_io)); memset(fail_net, 0, sizeof(fail_net));
    memset(die_in_commit, 0, sizeof(die_in_commit)); memset(down, 0, sizeof(down));
}
/* One acknowledged PUT object: ack bit i set = fragment i acknowledged. */
static int put(efs_ino_t ino, uint32_t ci, efs_node_id_t a, efs_node_id_t b, efs_node_id_t c,
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
static int live(void) { pthread_mutex_lock(&commit_mu); int n = (int)g_st_live; pthread_mutex_unlock(&commit_mu); return n; }

struct fsync_thread { pthread_t t; efs_ino_t ino; int rc, done; };
static void *do_fsync(void *arg)
{
    struct fsync_thread *f = arg;
    f->rc = efs_client_data_commit(f->ino);
    __atomic_store_n(&f->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

