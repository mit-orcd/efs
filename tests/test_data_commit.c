/* Client COMMIT on fsync: per-node acknowledged/committed accounting.
 * Regression: a concurrent fsync must not return while the only COMMIT
 * covering its data is still pending or failed (shared-mask race). */
#include "../src/client/data_commit.c"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static struct efs_conn conns[8];
static int commits[8], fail_io[8], fail_net[8];
static int hold[8];          /* reply withheld while set */
static uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline = d; }
struct efs_conn *efs_client_conn_get(efs_node_id_t id) { assert(id < 8); return &conns[id]; }
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
    struct efs_msg_data_commit_reply *r = calloc(1, sizeof(*r));
    r->rc = __atomic_load_n(&fail_io[id], __ATOMIC_ACQUIRE) ? EFS_ERR_IO : EFS_OK;
    r->roots = 1;
    *type = EFS_MSG_DATA_COMMIT_REPLY; *p = r; *n = sizeof(*r);
    return 0;
}
static void reset(void) { memset(commits, 0, sizeof(commits)); }

struct fsync_thread { pthread_t t; int rc, done; };
static void *do_fsync(void *arg)
{
    struct fsync_thread *f = arg;
    f->rc = efs_client_data_commit();
    __atomic_store_n(&f->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    /* Nothing acknowledged: no RPC. */
    assert(efs_client_data_commit() == EFS_OK && !commits[1] && !commits[3]);
    /* Acknowledged on nodes 1 and 3: one COMMIT each, then nothing pending. */
    efs_client_commit_note(1); efs_client_commit_note(3); efs_client_commit_note(3);
    reset(); assert(efs_client_data_commit() == EFS_OK);
    assert(commits[1] == 1 && commits[3] == 1 && !commits[2] && !commits[4]);
    reset(); assert(efs_client_data_commit() == EFS_OK && !commits[1] && !commits[3]);
    /* A failed barrier on node 3: EIO; node 3 stays behind, node 1 does not. */
    efs_client_commit_note(1); efs_client_commit_note(3); fail_io[3] = 1;
    reset(); assert(efs_client_data_commit() == EFS_ERR_IO);
    assert(commits[1] == 1 && commits[3] == 1);
    fail_io[3] = 0;
    reset(); assert(efs_client_data_commit() == EFS_OK && !commits[1] && commits[3] == 1);
    /* A lost connection is retried by the next fsync as well. */
    efs_client_commit_note(2); fail_net[2] = 1;
    reset(); assert(efs_client_data_commit() == EFS_ERR_NET && commits[2] == 1);
    fail_net[2] = 0;
    reset(); assert(efs_client_data_commit() == EFS_OK && commits[2] == 1);

    /* Concurrent fsyncs: while the first COMMIT to node 5 is withheld, a
     * second fsync must not return success either. */
    efs_client_commit_note(5);
    hold[5] = 1; reset();
    struct fsync_thread a = {0}, b = {0};
    pthread_create(&a.t, NULL, do_fsync, &a);
    while (!__atomic_load_n(&commits[5], __ATOMIC_ACQUIRE))
        usleep(1000);
    pthread_create(&b.t, NULL, do_fsync, &b);
    usleep(50000);
    assert(!__atomic_load_n(&a.done, __ATOMIC_ACQUIRE));
    assert(!__atomic_load_n(&b.done, __ATOMIC_ACQUIRE));
    __atomic_store_n(&hold[5], 0, __ATOMIC_RELEASE);
    pthread_join(a.t, NULL); pthread_join(b.t, NULL);
    assert(a.rc == EFS_OK && b.rc == EFS_OK && commits[5] == 2);

    /* Concurrent fsyncs where the pending COMMIT fails: neither succeeds,
     * and the node is committed again afterwards. */
    efs_client_commit_note(6);
    hold[6] = 1; fail_io[6] = 1; reset();
    struct fsync_thread c = {0}, d = {0};
    pthread_create(&c.t, NULL, do_fsync, &c);
    while (!__atomic_load_n(&commits[6], __ATOMIC_ACQUIRE))
        usleep(1000);
    pthread_create(&d.t, NULL, do_fsync, &d);
    while (__atomic_load_n(&commits[6], __ATOMIC_ACQUIRE) < 2)
        usleep(1000);
    __atomic_store_n(&hold[6], 0, __ATOMIC_RELEASE);
    pthread_join(c.t, NULL); pthread_join(d.t, NULL);
    assert(c.rc == EFS_ERR_IO && d.rc == EFS_ERR_IO);
    fail_io[6] = 0; reset();
    assert(efs_client_data_commit() == EFS_OK && commits[6] == 1);

    /* A PUT acknowledged after a COMMIT's snapshot needs another COMMIT. */
    efs_client_commit_note(7);
    reset(); assert(efs_client_data_commit() == EFS_OK && commits[7] == 1);
    efs_client_commit_note(7);
    reset(); assert(efs_client_data_commit() == EFS_OK && commits[7] == 1);

    /* The caller's deadline is restored. */
    deadline = 12345; efs_client_commit_note(4);
    assert(efs_client_data_commit() == EFS_OK && deadline == 12345);
    puts("data commit: acknowledged nodes only, parallel, concurrent fsyncs wait for "
         "their own COMMIT, failures and lost nodes retried, deadline restored PASS");
    return 0;
}
