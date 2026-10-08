struct gc_inode_job {
    struct efs_raft_host *host;
    struct efs_node node;
    efs_ino_t ino;
    uint64_t gen;
    int rc;
};
static void *host_gc_inode_node(void *argument)
{
    struct gc_inode_job *job = argument;
    struct efs_raft_host *h = job->host;
    struct efs_node *node = &job->node;
    int rc = EFS_ERR_IO;
    if (node->id == h->s->id)
        rc = server_gc_inode(h->s, 1, job->ino, job->gen);
    else {
        struct efs_conn *pc = server_peer_conn_get(node->addr, node->port);
        if (pc) {
            if (pc->kind == EFS_CONN_TCP) {
                efs_set_recv_timeout(pc->fd, HOST_SEND_IO_MS);
                efs_set_send_timeout(pc->fd, HOST_SEND_IO_MS);
            }
            struct efs_msg_gc_inode req = {
                .export_id = 1, .ino = job->ino, .generation = job->gen};
            uint8_t type;
            void *reply = NULL;
            uint32_t len = 0;
            if (!efs_conn_send_msg(pc, EFS_MSG_GC_INODE, &req, sizeof(req)) &&
                !efs_conn_recv_msg(pc, &type, &reply, &len) &&
                type == EFS_MSG_GC_INODE_REPLY &&
                len == sizeof(struct efs_msg_gc_inode_reply))
                rc = ((struct efs_msg_gc_inode_reply *)reply)->rc;
            free(reply);
            if (rc == EFS_OK || rc == EFS_ERR_BUSY) {
                if (pc->kind == EFS_CONN_TCP) {
                    efs_set_recv_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
                    efs_set_send_timeout(pc->fd, EFS_IO_TIMEOUT_MS);
                }
                server_peer_conn_release(node->addr, node->port, pc);
            } else
                server_peer_conn_drop(node->addr, node->port, pc);
        }
    }
    job->rc = rc;
    return NULL;
}
static int host_gc_inode_nodes(struct efs_raft_host *h, efs_ino_t ino, uint64_t gen)
{
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t count;
    pthread_mutex_lock(&h->s->lock);
    count = h->s->node_count;
    memcpy(nodes, h->s->nodes, count * sizeof(nodes[0]));
    pthread_mutex_unlock(&h->s->lock);
    uint32_t present = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!nodes[i].id || nodes[i].id > (uint32_t)h->n)
            return EFS_ERR_BUSY;
        present |= 1u << (nodes[i].id - 1);
    }
    /* Missing members still retain the durable retry ledger. Concurrent
     * calls do not relax the all-member fence/inventory requirement. */
    int result = present == ((1u << h->n) - 1u) ? EFS_OK : EFS_ERR_BUSY;
    enum { FANOUT = 4 };
    for (uint32_t offset = 0; offset < count; offset += FANOUT) {
        struct gc_inode_job jobs[FANOUT];
        pthread_t workers[FANOUT];
        int launched[FANOUT] = {0};
        uint32_t batch = count - offset;
        if (batch > FANOUT) batch = FANOUT;
        for (uint32_t i = 0; i < batch; i++) {
            jobs[i] = (struct gc_inode_job){
                .host=h, .node=nodes[offset+i], .ino=ino, .gen=gen,
                .rc=EFS_ERR_IO};
            /* Run the final member on the GC thread: at most three extra
             * threads, and no allocation failure can skip a member. */
            if (i + 1 < batch && !pthread_create(&workers[i], NULL,
                                                host_gc_inode_node, &jobs[i]))
                launched[i] = 1;
            else
                host_gc_inode_node(&jobs[i]);
        }
        for (uint32_t i = 0; i < batch; i++) {
            if (launched[i]) pthread_join(workers[i], NULL);
            if (jobs[i].rc != EFS_OK) result = jobs[i].rc;
        }
        /* Every worker is joined before jobs leave scope, before the next
         * inode, and before host shutdown can destroy peers/shared state. */
    }
    return result;
}
