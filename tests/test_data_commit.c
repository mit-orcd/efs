/* Client COMMIT on fsync: only touched nodes, in parallel, failures retained. */
#include "../src/client/data_commit.c"
#include <assert.h>
#include <stdio.h>

static struct efs_conn conns[8];
static int commits[8], fail_io[8], fail_net[8];
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
    __atomic_fetch_add(&commits[id], 1, __ATOMIC_RELAXED);
    return fail_net[id] ? -1 : 0;
}
int efs_conn_recv_msg(struct efs_conn *c, uint8_t *type, void **p, uint32_t *n)
{
    int id = (int)(c - conns);
    struct efs_msg_data_commit_reply *r = calloc(1, sizeof(*r));
    r->rc = fail_io[id] ? EFS_ERR_IO : EFS_OK;
    r->roots = 1;
    *type = EFS_MSG_DATA_COMMIT_REPLY; *p = r; *n = sizeof(*r);
    return 0;
}
static void reset(void) { memset(commits, 0, sizeof(commits)); }

int main(void)
{
    /* Nothing written: no RPC. */
    assert(efs_client_data_commit() == EFS_OK && !commits[1] && !commits[3]);
    /* Touched nodes 1 and 3: one COMMIT each, then nothing pending. */
    efs_client_commit_note(1); efs_client_commit_note(3); efs_client_commit_note(3);
    reset(); assert(efs_client_data_commit() == EFS_OK);
    assert(commits[1] == 1 && commits[3] == 1 && !commits[2] && !commits[4]);
    reset(); assert(efs_client_data_commit() == EFS_OK && !commits[1] && !commits[3]);
    /* A failed barrier on node 3: EIO, node 3 stays marked, node 1 does not. */
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
    /* The caller's deadline is restored. */
    deadline = 12345; efs_client_commit_note(4);
    assert(efs_client_data_commit() == EFS_OK && deadline == 12345);
    puts("data commit: touched nodes only, parallel, failed and lost nodes retained, deadline restored PASS");
    return 0;
}
