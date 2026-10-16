#include "client_internal.h"
#include "efs/protocol.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* Every fsync covers all current storage nodes, including peer writes and
 * a newly mounted client. A PUT verifier identifies a continuous durable server history.
 * Clean restarts preserve it only after all roots prove a final barrier.
 * Never silently forget an acknowledged-but-uncommitted incarnation: the
 * payload may already have been released, so a changed verifier is sticky EIO.
 * The server separately fences uncertain unclean recovery for fresh clients. */
static pthread_mutex_t commit_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t acked, committed, epoch; int lost; } node_commit[64];

void efs_client_commit_note(efs_node_id_t node, uint64_t epoch)
{
    if (node >= 64) return;
    pthread_mutex_lock(&commit_mu);
    if (!epoch || (node_commit[node].epoch != epoch &&
                   node_commit[node].acked > node_commit[node].committed))
        node_commit[node].lost = 1;
    node_commit[node].epoch = epoch;
    ++node_commit[node].acked;
    pthread_mutex_unlock(&commit_mu);
}

struct commit_job { efs_node_id_t node; uint64_t target, epoch, deadline; int rc; };

static void *commit_one(void *arg)
{
    struct commit_job *j = arg;
    uint64_t saved = efs_client_rpc_deadline_ms();
    efs_client_rpc_set_deadline_ms(j->deadline);
    j->rc = EFS_ERR_NET;
    /* COMMIT is idempotent. A planned restart leaves an old pooled socket;
     * retry transport failure once with a new connection, inside the same
     * deadline and acknowledged snapshot. Never retry a verifier/error reply. */
    for(unsigned attempt=0;attempt<2;attempt++) {
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
                    j->rc = r->rc == EFS_OK && r->boot_epoch ? EFS_OK : EFS_ERR_IO;
                    pthread_mutex_lock(&commit_mu);
                    /* Even another successful fsync cannot erase a mismatch for
                     * this call's own snapshot. A later PUT must not hide it. */
                    if (j->target && j->epoch != r->boot_epoch) {
                        node_commit[j->node].lost = 1;
                        j->rc = EFS_ERR_IO;
                    }
                    if (node_commit[j->node].lost) j->rc = EFS_ERR_IO;
                    if (j->rc == EFS_OK && j->target > node_commit[j->node].committed)
                        node_commit[j->node].committed = j->target;
                    pthread_mutex_unlock(&commit_mu);
                } else {
                    j->rc = EFS_ERR_PROTO;
                }
            }
            free(reply);
        }
        if(j->rc!=EFS_ERR_NET)break;
    }
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
    uint64_t mask = 0;
    pthread_mutex_lock(&g_client.lock);
    for (uint32_t i=0; i<g_client.node_count; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (id >= 64) { pthread_mutex_unlock(&g_client.lock); return EFS_ERR_IO; }
        mask |= 1ull << id;
    }
    pthread_mutex_unlock(&g_client.lock);
    pthread_mutex_lock(&commit_mu);
    for (efs_node_id_t node=0; node<64; node++) {
        if (node_commit[node].lost) { pthread_mutex_unlock(&commit_mu); return EFS_ERR_IO; }
        if (node_commit[node].acked > node_commit[node].committed) mask |= 1ull << node;
    }
    pthread_mutex_unlock(&commit_mu);
    if (!mask) return EFS_ERR_IO; /* no topology is not a durability proof */
    for (efs_node_id_t node = 0; node < 64; node++) {
        if (!(mask & (1ull << node))) continue;
        pthread_mutex_lock(&commit_mu);
        uint64_t target = node_commit[node].acked > node_commit[node].committed
                            ? node_commit[node].acked : 0;
        jobs[n] = (struct commit_job){ .node = node, .target = target,
            .epoch = node_commit[node].epoch, .deadline = deadline, .rc = EFS_ERR_NET };
        pthread_mutex_unlock(&commit_mu);
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
