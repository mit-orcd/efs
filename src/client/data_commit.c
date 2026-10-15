#include "client_internal.h"
#include "efs/protocol.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* Servers acknowledge data PUTs before they are durable (NFS unstable
 * writes). Every node a data PUT was sent to since the last successful
 * COMMIT is remembered here; fsync sends DATA_COMMIT to each of them in
 * parallel, and a node with nothing unsynced answers at once. Marked at send
 * time: an extra COMMIT is harmless, a missed one is not. A node whose
 * COMMIT fails stays marked for the next fsync. */
static uint64_t commit_dirty_nodes;

void efs_client_commit_note(efs_node_id_t node)
{
    if (node < 64)
        __atomic_fetch_or(&commit_dirty_nodes, 1ull << node, __ATOMIC_ACQ_REL);
}

struct commit_job { efs_node_id_t node; uint64_t deadline; int rc; };

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
    efs_client_rpc_set_deadline_ms(saved);
    return NULL;
}

int efs_client_data_commit(void)
{
    uint64_t mask = __atomic_exchange_n(&commit_dirty_nodes, 0, __ATOMIC_ACQ_REL);
    if (!mask)
        return EFS_OK;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        __atomic_fetch_or(&commit_dirty_nodes, mask, __ATOMIC_ACQ_REL);
        return EFS_ERR_IO;
    }
    uint64_t deadline = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 60000;
    uint64_t parent = efs_client_rpc_deadline_ms();
    if (parent && parent < deadline)
        deadline = parent;
    struct commit_job jobs[64];
    pthread_t th[64];
    int started[64] = {0}, n = 0;
    for (efs_node_id_t node = 0; node < 64; node++) {
        if (!(mask & (1ull << node)))
            continue;
        jobs[n] = (struct commit_job){ .node = node, .deadline = deadline, .rc = EFS_ERR_NET };
        started[n] = pthread_create(&th[n], NULL, commit_one, &jobs[n]) == 0;
        if (!started[n])
            commit_one(&jobs[n]);
        n++;
    }
    int rc = EFS_OK;
    uint64_t failed = 0;
    for (int i = 0; i < n; i++) {
        if (started[i])
            pthread_join(th[i], NULL);
        if (jobs[i].rc != EFS_OK) {
            failed |= 1ull << jobs[i].node;
            if (rc == EFS_OK || jobs[i].rc == EFS_ERR_IO)
                rc = jobs[i].rc;
        }
    }
    if (failed)
        __atomic_fetch_or(&commit_dirty_nodes, failed, __ATOMIC_ACQ_REL);
    return rc;
}
