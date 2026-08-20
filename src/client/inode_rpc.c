#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static struct efs_conn *rpc_first_conn(efs_node_id_t *nid_out)
{
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        if (efs_client_node_is_down(g_client.nodes[i].id))
            continue;
        struct efs_conn *conn = efs_client_conn_get(g_client.nodes[i].id);
        if (conn) {
            *nid_out = g_client.nodes[i].id;
            return conn;
        }
    }
    return NULL;
}

static int rpc_send_recv(uint8_t type, const void *req, uint32_t req_len,
                         uint8_t expect, void *reply, uint32_t reply_len)
{
    efs_node_id_t nid = 0;
    struct efs_conn *conn = rpc_first_conn(&nid);
    if (!conn)
        return EFS_ERR_NET;
    if (efs_conn_send_msg(conn, type, req, req_len) != 0) {
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    uint8_t rtype = 0;
    void *payload = NULL;
    uint32_t plen = 0;
    int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
    if (rc != 0) {
        efs_client_conn_drop(nid, conn);
        return EFS_ERR_NET;
    }
    efs_client_conn_release(nid, conn);
    if (rtype != expect || plen < reply_len) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    memcpy(reply, payload, reply_len);
    free(payload);
    return EFS_OK;
}

static int rpc_status_to_efs(uint8_t st)
{
    switch (st) {
    case EFS_INODE_RPC_OK:          return EFS_OK;
    case EFS_INODE_RPC_NOT_FOUND:   return EFS_ERR_NOT_FOUND;
    case EFS_INODE_RPC_EXIST:       return EFS_ERR_EXIST;
    case EFS_INODE_RPC_QUOTA:       return EFS_ERR_QUOTA;
    case EFS_INODE_RPC_BUSY:        return EFS_ERR_BUSY;
    case EFS_INODE_RPC_INVAL:       return EFS_ERR_INVAL;
    case EFS_INODE_RPC_NOT_PRIMARY: return EFS_ERR_NOT_PRIMARY;
    default:                        return EFS_ERR_IO;
    }
}

/* Phase 2b: the metadata primary is the lowest-id live node. Mutations must
 * reach it (the meta-flush thread is primary-only). */
static struct efs_conn *rpc_primary_conn(efs_node_id_t *nid_out)
{
    efs_node_id_t best = 0;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (efs_client_node_is_down(id))
            continue;
        if (best == 0 || id < best)
            best = id;
    }
    if (best == 0)
        return NULL;
    struct efs_conn *conn = efs_client_conn_get(best);
    if (conn)
        *nid_out = best;
    return conn;
}

/* Phase 2b: send a mutation to the primary, retrying on NOT_PRIMARY with the
 * primary_id the server reports (the client's liveness view can be stale).
 * The reply must be a struct efs_msg_inode_reply. */
static int rpc_send_recv_primary(uint8_t type, const void *req, uint32_t req_len,
                                 uint8_t expect, void *reply, uint32_t reply_len)
{
    efs_node_id_t target = 0; /* 0 = compute the primary from our view */
    for (int attempt = 0; attempt < 4; attempt++) {
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        if (target != 0) {
            conn = efs_client_conn_get(target);
            nid = target;
        } else {
            conn = rpc_primary_conn(&nid);
        }
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, type, req, req_len) != 0) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t rtype = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        if (rc != 0) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (rtype != expect || plen < reply_len) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        memcpy(reply, payload, reply_len);
        free(payload);
        struct efs_msg_inode_reply *r = reply;
        if (r->status != EFS_INODE_RPC_NOT_PRIMARY)
            return EFS_OK;
        /* NOT_PRIMARY: retry on the server-reported primary. */
        if (r->primary_id == 0 || r->primary_id == nid)
            return EFS_ERR_NOT_PRIMARY; /* no better info */
        target = r->primary_id;
    }
    return EFS_ERR_NOT_PRIMARY;
}

int efs_client_rpc_lookup(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, struct efs_inode *out)
{
    struct efs_msg_inode_lookup req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    if (name)
        strncpy(req.name, name, EFS_MAX_NAME - 1);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv(EFS_MSG_INODE_LOOKUP, &req, sizeof(req),
                           EFS_MSG_INODE_LOOKUP_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_create(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, uint32_t mode, uid_t uid, gid_t gid,
                          efs_ino_t *out_ino, struct efs_inode *out)
{
    struct efs_msg_inode_create req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    if (name)
        strncpy(req.name, name, EFS_MAX_NAME - 1);
    req.mode = mode;
    req.uid = (uint32_t)uid;
    req.gid = (uint32_t)gid;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_primary(EFS_MSG_INODE_CREATE, &req, sizeof(req),
                                   EFS_MSG_INODE_CREATE_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out_ino)
        *out_ino = r.inode.ino;
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_getattr(efs_export_id_t export_id, efs_ino_t ino,
                           struct efs_inode *out)
{
    struct efs_msg_inode_getattr req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv(EFS_MSG_INODE_GETATTR, &req, sizeof(req),
                           EFS_MSG_INODE_GETATTR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_readdir(efs_export_id_t export_id, efs_ino_t parent,
                           struct efs_inode *ents, uint32_t *inout_count)
{
    struct efs_msg_inode_readdir req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    req.max_ents = inout_count ? *inout_count : EFS_READDIR_MAX;
    struct efs_msg_inode_readdir_reply r;
    int rc = rpc_send_recv(EFS_MSG_INODE_READDIR, &req, sizeof(req),
                           EFS_MSG_INODE_READDIR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    uint32_t n = r.count;
    if (inout_count && n > *inout_count)
        n = *inout_count;
    if (ents && n)
        memcpy(ents, r.ents, n * sizeof(ents[0]));
    if (inout_count)
        *inout_count = n;
    return EFS_OK;
}

int efs_client_rpc_unlink(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, int is_dir)
{
    struct efs_msg_inode_unlink req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    if (name)
        strncpy(req.name, name, EFS_MAX_NAME - 1);
    req.is_dir = is_dir ? 1 : 0;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_primary(EFS_MSG_INODE_UNLINK, &req, sizeof(req),
                                   EFS_MSG_INODE_UNLINK_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_rename(efs_export_id_t export_id, efs_ino_t ino,
                          efs_ino_t new_parent, const char *new_name,
                          struct efs_inode *out)
{
    struct efs_msg_inode_rename req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.new_parent = new_parent;
    if (new_name)
        strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_primary(EFS_MSG_INODE_RENAME, &req, sizeof(req),
                                   EFS_MSG_INODE_RENAME_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_setattr(efs_export_id_t export_id, efs_ino_t ino,
                           uint32_t mask, uint32_t mode, uid_t uid, gid_t gid,
                           uint64_t size, uint64_t mtime, uint32_t mtime_nsec,
                           uint64_t atime, struct efs_inode *out)
{
    struct efs_msg_inode_setattr req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.mask = mask;
    req.mode = mode;
    req.uid = (uint32_t)uid;
    req.gid = (uint32_t)gid;
    req.size = size;
    req.mtime = mtime;
    req.mtime_nsec = mtime_nsec;
    req.atime = atime;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_primary(EFS_MSG_INODE_SETATTR, &req, sizeof(req),
                                   EFS_MSG_INODE_SETATTR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_link(efs_export_id_t export_id, efs_ino_t src_ino,
                        efs_ino_t new_parent, const char *new_name,
                        struct efs_inode *out)
{
    struct efs_msg_inode_link req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.src_ino = src_ino;
    req.new_parent = new_parent;
    if (new_name)
        strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_primary(EFS_MSG_INODE_LINK, &req, sizeof(req),
                                   EFS_MSG_INODE_LINK_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Phase 2b: report dirty metadata (chunk mappings + inode size/mtime) to the
 * metadata primary, replacing the client blob flush. sync=1 makes the primary
 * commit the export before replying (the fsync durability barrier). */
int efs_client_rpc_report_dirty(efs_export_id_t export_id,
                                const struct efs_chunk_rec *recs,
                                uint32_t count,
                                const struct efs_ino_size_rec *irecs,
                                uint32_t ino_count, int sync)
{
    if (count == 0 && ino_count == 0 && !sync)
        return EFS_OK;
    size_t len = sizeof(struct efs_msg_report_chunks) +
                 (size_t)count * sizeof(struct efs_chunk_rec) +
                 (size_t)ino_count * sizeof(struct efs_ino_size_rec);
    uint8_t *buf = malloc(len);
    if (!buf)
        return EFS_ERR_NOMEM;
    struct efs_msg_report_chunks *hdr = (struct efs_msg_report_chunks *)buf;
    hdr->export_id = export_id;
    hdr->count = count;
    hdr->sync = sync ? 1u : 0u;
    hdr->ino_count = ino_count;
    uint8_t *p = buf + sizeof(*hdr);
    if (count) {
        memcpy(p, recs, (size_t)count * sizeof(*recs));
        p += (size_t)count * sizeof(*recs);
    }
    if (ino_count)
        memcpy(p, irecs, (size_t)ino_count * sizeof(*irecs));
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_primary(EFS_MSG_REPORT_CHUNKS, buf, (uint32_t)len,
                                   EFS_MSG_REPORT_CHUNKS_REPLY, &r, sizeof(r));
    free(buf);
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_load_shard(uint32_t shard)
{
    uint32_t n = g_client.export.root.shard_count;
    if (n <= 1)
        return shard == 0 ? EFS_OK : EFS_ERR_NOT_FOUND;
    if (shard >= n)
        return EFS_ERR_INVAL;
    /* Multi-shard page fetch lands in a later cut; shard 0 is the v5 blob. */
    if (shard == 0)
        return EFS_OK;
    return EFS_ERR_NOT_FOUND;
}
