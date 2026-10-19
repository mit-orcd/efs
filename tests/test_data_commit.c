/* Client COMMIT on fsync: per-node acknowledged/committed accounting.
 * Regression: a concurrent fsync must not return while the only COMMIT
 * covering its data is still pending or failed (shared-mask race). */
#include "../src/client/data_commit.c"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static struct efs_conn conns[8];
static int commits[8], fail_io[8], fail_net[8], fail_net_once[8];
static int hold[8];          /* reply withheld while set */
struct efs_client g_client;
static __thread uint64_t deadline;
static uint64_t epochs[8];
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline = d; }
static int cooldown[8], listening[8], reprobes[8];
struct efs_conn *efs_client_conn_get(efs_node_id_t id) { assert(id < 8); return cooldown[id] ? NULL : &conns[id]; }
int efs_client_node_reprobe(efs_node_id_t id)
{
    reprobes[id]++;
    if (!listening[id]) return 0;
    cooldown[id] = 0;
    return 1;
}
void efs_client_conn_release(efs_node_id_t id, struct efs_conn *c) { (void)id; (void)c; }
void efs_client_conn_drop(efs_node_id_t id, struct efs_conn *c) { (void)id; (void)c; }
int efs_conn_send_msg(struct efs_conn *c, uint8_t type, const void *p, uint32_t n)
{
    int id = (int)(c - conns);
    assert(type == EFS_MSG_DATA_COMMIT && !p && !n);
    __atomic_fetch_add(&commits[id], 1, __ATOMIC_ACQ_REL);
    if(__atomic_exchange_n(&fail_net_once[id],0,__ATOMIC_ACQ_REL))return -1;
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
    r->boot_epoch = epochs[id];
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
    pthread_mutex_init(&g_client.lock, NULL);
    g_client.node_count=4;
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;epochs[i+1]=101;}
    /* Fresh client and a repeated fsync both COMMIT every storage node. */
    for(int run=0;run<2;run++) {
        reset(); assert(efs_client_data_commit()==EFS_OK);
        for(int i=1;i<=4;i++) assert(commits[i]==1);
    }
    /* A peer restarted inside the client's down cooldown: COMMIT probes it
     * and proceeds; a peer still down is probed once and fails the fsync. */
    reset();cooldown[3]=1;listening[3]=1;assert(efs_client_data_commit()==EFS_OK);
    assert(commits[3]==1 && reprobes[3]==1 && !cooldown[3]);
    reset();cooldown[3]=1;listening[3]=0;reprobes[3]=0;
    assert(efs_client_data_commit()==EFS_ERR_NET && commits[3]==0 && reprobes[3]==1);
    cooldown[3]=0;
    reset();fail_net_once[2]=1;assert(efs_client_data_commit()==EFS_OK);
    assert(commits[2]==2 && commits[1]==1 && commits[3]==1 && commits[4]==1);
    /* Peer writes: no local notes, but a peer's failed barrier is visible. */
    fail_io[3]=1; assert(efs_client_data_commit()==EFS_ERR_IO); fail_io[3]=0;
    fail_net[2]=1; assert(efs_client_data_commit()==EFS_ERR_NET); fail_net[2]=0;
    assert(efs_client_data_commit()==EFS_OK);
    efs_client_commit_note(1,101);
    hold[1]=1;reset();
    struct fsync_thread a={0},b={0};
    pthread_create(&a.t,NULL,do_fsync,&a);
    while(!__atomic_load_n(&commits[1],__ATOMIC_ACQUIRE)) usleep(1000);
    pthread_create(&b.t,NULL,do_fsync,&b);
    while(__atomic_load_n(&commits[1],__ATOMIC_ACQUIRE)<2) usleep(1000);
    assert(!__atomic_load_n(&a.done,__ATOMIC_ACQUIRE));
    assert(!__atomic_load_n(&b.done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&hold[1],0,__ATOMIC_RELEASE);
    pthread_join(a.t,NULL);pthread_join(b.t,NULL);
    assert(a.rc==EFS_OK && b.rc==EFS_OK);
    /* Concurrent failures never advance the acknowledged snapshot. */
    efs_client_commit_note(1,101);hold[1]=1;fail_io[1]=1;reset();
    a=(struct fsync_thread){0};b=(struct fsync_thread){0};
    pthread_create(&a.t,NULL,do_fsync,&a);pthread_create(&b.t,NULL,do_fsync,&b);
    while(__atomic_load_n(&commits[1],__ATOMIC_ACQUIRE)<2) usleep(1000);
    __atomic_store_n(&hold[1],0,__ATOMIC_RELEASE);
    pthread_join(a.t,NULL);pthread_join(b.t,NULL);
    assert(a.rc==EFS_ERR_IO && b.rc==EFS_ERR_IO);
    fail_io[1]=0;assert(efs_client_data_commit()==EFS_OK);
    /* A clean stop drains prior PUTs and preserves the all-root verifier.
     * An unresolved client therefore survives repeated clean restarts and
     * new PUTs without needing to remount or clear any loss flag. */
    efs_client_commit_note(1,101);
    assert(efs_client_data_commit()==EFS_OK);
    efs_client_commit_note(1,101);efs_client_commit_note(1,101);
    assert(efs_client_data_commit()==EFS_OK && !node_commit[1].lost);
    /* A restart AFTER committed data is not an unresolved epoch. */
    epochs[1]=102;assert(efs_client_data_commit()==EFS_OK);
    efs_client_commit_note(1,102);assert(efs_client_data_commit()==EFS_OK);
    deadline=12345;assert(efs_client_data_commit()==EFS_OK && deadline==12345);deadline=0;
    /* Lost unstable data: new COMMIT verifier, EIO persists across retries. */
    efs_client_commit_note(1,102);epochs[1]=103;
    assert(efs_client_data_commit()==EFS_ERR_IO);
    assert(efs_client_data_commit()==EFS_ERR_IO);
    memset(node_commit,0,sizeof(node_commit)); /* new client instance */
    /* A new PUT after reboot cannot erase an old unresolved PUT epoch. */
    efs_client_commit_note(2,101);efs_client_commit_note(2,102);epochs[2]=102;
    assert(efs_client_data_commit()==EFS_ERR_IO);
    memset(node_commit,0,sizeof(node_commit));
    epochs[4]=0;assert(efs_client_data_commit()==EFS_ERR_IO);
    puts("data commit: cooldown reprobe, all-node peer/fresh-client fsync, concurrent barriers, failures, verifier changes and sticky lost-data EIO PASS");
    return 0;
}
