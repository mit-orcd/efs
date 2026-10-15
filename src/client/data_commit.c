#include "client_internal.h"
#include "efs/protocol.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* Servers acknowledge data PUTs before they are durable (NFS unstable
 * writes); fsync must COMMIT every node holding this client's acknowledged
 * data. Per node, `acked` counts PUTs the server acknowledged and
 * `committed` is the highest acked count a successful COMMIT covered.
 *
 * fsync snapshots each node's acked count after the file's PUTs were
 * acknowledged and before sending COMMIT, so the COMMIT (which covers
 * everything the server acknowledged before it arrived) covers the snapshot.
 * Every fsync sends its own COMMIT to each node that is behind; the server
 * folds concurrent ones into one barrier. `committed` only advances after a
 * successful reply, so a concurrent, slow or failed COMMIT never lets
 * another fsync return before its own data is durable, and a failed node is
 * committed again by the next fsync. */
static uint64_t node_acked[64], node_committed[64];

void efs_client_commit_note(efs_node_id_t node)
{
    if (node < 64)
        __atomic_add_fetch(&node_acked[node], 1, __ATOMIC_ACQ_REL);
}

static void committed_advance(efs_node_id_t node, uint64_t to)
{
    uint64_t cur = __atomic_load_n(&node_committed[node], __ATOMIC_ACQUIRE);
    while (cur < to &&
           !__atomic_compare_exchange_n(&node_committed[node], &cur, to, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ;
}

struct commit_job { efs_node_id_t node; uint64_t target, deadline; int rc; };

static void *commit_one(void *arg)
{
    struct commit_job *j = arg;
    uint64_t saved = efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(j->deadline);
    j->rc = EFS_ERR_NET;
    struct efs_conn *conn = efs_client_conn_get(j->node);
    if (conn) {
        uint8_t type = 0; void *reply = NULL; uint32_t bytes = 0;
        if (efs_conn_send_msg(conn, EFS_MSG_DATA_COMMIT, NULL, 0) ||
            efs_conn_recv_msg(conn, &type, &reply, &bytes)) {
            efs_client_conn_drop(j->node, conn);
        } else {
            efs_client_conn_release(j->node, conn);
            if (type == EFS_MSG_DATA_COMMIT_REPLY &&
                bytes == sizeof(struct efs_msg_data_commit_reply)) {
                const struct efs_msg_data_commit_reply *r = reply;
                j->rc = r->rc == EFS_OK ? EFS_OK : EFS_ERR_IO;
            } else {
                j->rc = EFS_ERR_PROTO;
            }
        }
        free(reply);
    }
    if (j->rc == EFS_OK)
        committed_advance(j->node, j->target);
    efs_client_rpc_set_deadline_ms(saved);
    return NULL;
}

int efs_client_data_commit(void)
{
    struct commit_job jobs[64];
    pthread_t th[64];
    int started[64] = {0}, n = 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return EFS_ERR_IO;
    uint64_t deadline = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 60000;
    uint64_t parent = efs_client_rpc_deadline_ms();
    if (parent && parent < deadline)
        deadline = parent;
    for (efs_node_id_t node = 0; node < 64; node++) {
        uint64_t target = __atomic_load_n(&node_acked[node], __ATOMIC_ACQUIRE);
        if (target <= __atomic_load_n(&node_committed[node], __ATOMIC_ACQUIRE))
            continue;
        jobs[n] = (struct commit_job){ .node = node, .target = target,
                                       .deadline = deadline, .rc = EFS_ERR_NET };
        started[n] = pthread_create(&th[n], NULL, commit_one, &jobs[n]) == 0;
        if (!started[n])
            commit_one(&jobs[n]);
        n++;
    }
    int rc = EFS_OK;
    for (int i = 0; i < n; i++) {
        if (started[i])
            pthread_join(th[i], NULL);
        if (jobs[i].rc != EFS_OK && (rc == EFS_OK || jobs[i].rc == EFS_ERR_IO))
            rc = jobs[i].rc;
    }
    return rc;
}
