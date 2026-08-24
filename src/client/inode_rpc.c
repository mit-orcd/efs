#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
    case EFS_INODE_RPC_NOT_EMPTY:   return EFS_ERR_NOT_EMPTY;
    case EFS_INODE_RPC_SYMLINK:
    case EFS_INODE_RPC_DEEP:        return EFS_ERR_PROTO;
    default:                        return EFS_ERR_IO;
    }
}

/* Phase 2b: the metadata primary is the lowest-id live node. Mutations must
 * reach it (the meta-flush thread is primary-only). */
static struct efs_conn *rpc_primary_conn(efs_node_id_t *nid_out)
{
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = 0;
    for (uint32_t i = 0; i < g_client.node_count && nlive < EFS_MAX_NODES; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (efs_client_node_is_down(id))
            continue;
        live[nlive++] = id;
    }
    efs_node_id_t best = efs_shard_owner_of(0, 1, live, nlive);
    if (best == 0)
        return NULL;
    struct efs_conn *conn = efs_client_conn_get(best);
    if (conn)
        *nid_out = best;
    return conn;
}

/* Route to live[shard % nlive] when bits>0. Mutations still require the
 * export primary until that owner flushes (server rejects NOT_PRIMARY). */
static struct efs_conn *rpc_owner_conn(efs_ino_t ino, efs_node_id_t *nid_out)
{
    uint32_t sc = g_client.export.root.shard_count;
    uint32_t bits = g_client.export.root.shard_bits;
    if (sc <= 1 || bits == 0)
        return rpc_primary_conn(nid_out);
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = 0;
    for (uint32_t i = 0; i < g_client.node_count && nlive < EFS_MAX_NODES; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (efs_client_node_is_down(id))
            continue;
        live[nlive++] = id;
    }
    uint32_t shard = efs_export_shard_of(ino, bits);
    efs_node_id_t owner = efs_shard_owner_of(shard, sc, live, nlive);
    if (owner == 0)
        return rpc_primary_conn(nid_out);
    struct efs_conn *conn = efs_client_conn_get(owner);
    if (conn)
        *nid_out = owner;
    return conn;
}

/* Send to the shard owner (today: export primary). Retry NOT_PRIMARY. */
static int rpc_send_recv_owner(efs_ino_t ino, uint8_t type, const void *req,
                               uint32_t req_len, uint8_t expect, void *reply,
                               uint32_t reply_len)
{
    efs_node_id_t target = 0; /* 0 = compute the owner from our view */
    for (int attempt = 0; attempt < 16; attempt++) {
        efs_node_id_t nid = 0;
        struct efs_conn *conn;
        if (target != 0) {
            conn = efs_client_conn_get(target);
            nid = target;
        } else {
            conn = rpc_owner_conn(ino, &nid);
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
        if (r->status == EFS_INODE_RPC_BUSY) {
            /* INODE_APPEND uses BUSY as the unflushed-reservation barrier.
             * The caller must flush + report before retrying; spinning here
             * holds g_append_mu for ~10s and deadlocks concurrent O_APPEND. */
            if (type == EFS_MSG_INODE_APPEND || type == EFS_MSG_INODE_FLOCK)
                return EFS_OK;
            /* Extra-shard owner is still assembling pages after restart.
             * Same target — do not flip to another node. */
            unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
            usleep(50000u << shift);
            continue;
        }
        if (r->status != EFS_INODE_RPC_NOT_PRIMARY)
            return EFS_OK;
        /* NOT_PRIMARY: retry on the server-reported primary. */
        if (r->primary_id == 0 || r->primary_id == nid)
            return EFS_ERR_NOT_PRIMARY; /* no better info */
        target = r->primary_id;
    }
    return EFS_ERR_NOT_PRIMARY;
}

int efs_client_rpc_lookup_path(efs_export_id_t export_id, const char *path,
                               uint32_t flags,
                               struct efs_msg_inode_lookup_path_reply *out)
{
    struct efs_msg_inode_lookup_path req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.flags = flags;
    if (path)
        strncpy(req.path, path, sizeof(req.path) - 1);
    struct efs_msg_inode_lookup_path_reply r;
    int rc = rpc_send_recv_owner(EFS_ROOT_INO, EFS_MSG_INODE_LOOKUP_PATH, &req,
                                 sizeof(req), EFS_MSG_INODE_LOOKUP_PATH_REPLY,
                                 &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r;
    return EFS_OK;
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
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_LOOKUP, &req, sizeof(req),
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
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_CREATE, &req, sizeof(req),
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
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_GETATTR, &req, sizeof(req),
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
                           struct efs_inode *ents, uint32_t *inout_count,
                           uint32_t start)
{
    struct efs_msg_inode_readdir req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.parent = parent;
    req.max_ents = inout_count ? *inout_count : EFS_READDIR_MAX;
    req.start = start;
    /* Readdir reply is not efs_msg_inode_reply (no primary_id). */
    efs_node_id_t nid = 0;
    struct efs_conn *conn = rpc_owner_conn(parent, &nid);
    if (!conn)
        return EFS_ERR_NET;
    if (efs_conn_send_msg(conn, EFS_MSG_INODE_READDIR, &req, sizeof(req)) != 0) {
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
    if (rtype != EFS_MSG_INODE_READDIR_REPLY ||
        plen < sizeof(struct efs_msg_inode_readdir_reply)) {
        free(payload);
        return EFS_ERR_PROTO;
    }
    struct efs_msg_inode_readdir_reply *r = payload;
    if (r->status != EFS_INODE_RPC_OK) {
        int st = rpc_status_to_efs(r->status);
        free(payload);
        return st;
    }
    uint32_t n = r->count;
    if (inout_count && n > *inout_count)
        n = *inout_count;
    if (ents && n)
        memcpy(ents, r->ents, n * sizeof(ents[0]));
    if (inout_count)
        *inout_count = n;
    free(payload);
    return EFS_OK;
}

int efs_client_rpc_getchunks(efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count)
{
    struct efs_msg_inode_getchunks req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.start = start;
    req.max = inout_count ? *inout_count : EFS_GETCHUNKS_MAX;
    struct efs_msg_inode_getchunks_reply *r = NULL;
    void *payload = NULL;
    for (int attempt = 0; attempt < 16; attempt++) {
        efs_node_id_t nid = 0;
        struct efs_conn *conn = rpc_owner_conn(ino, &nid);
        if (!conn)
            return EFS_ERR_NET;
        if (efs_conn_send_msg(conn, EFS_MSG_INODE_GETCHUNKS, &req,
                              sizeof(req)) != 0) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        uint8_t rtype = 0;
        uint32_t plen = 0;
        payload = NULL;
        int rc = efs_conn_recv_msg(conn, &rtype, &payload, &plen);
        if (rc != 0) {
            efs_client_conn_drop(nid, conn);
            return EFS_ERR_NET;
        }
        efs_client_conn_release(nid, conn);
        if (rtype != EFS_MSG_INODE_GETCHUNKS_REPLY ||
            plen < sizeof(struct efs_msg_inode_getchunks_reply)) {
            free(payload);
            return EFS_ERR_PROTO;
        }
        r = payload;
        if (r->status != EFS_INODE_RPC_BUSY)
            break;
        free(payload);
        payload = NULL;
        r = NULL;
        unsigned shift = (unsigned)(attempt < 4 ? attempt : 4);
        usleep(50000u << shift);
    }
    if (!r) {
        free(payload);
        return EFS_ERR_BUSY;
    }
    if (r->status != EFS_INODE_RPC_OK) {
        int st = rpc_status_to_efs(r->status);
        free(payload);
        return st;
    }
    uint32_t n = r->count;
    if (inout_count && n > *inout_count)
        n = *inout_count;
    if (recs && n)
        memcpy(recs, r->recs, n * sizeof(recs[0]));
    if (inout_count)
        *inout_count = n;
    free(payload);
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
    int rc = rpc_send_recv_owner(parent, EFS_MSG_INODE_UNLINK, &req, sizeof(req),
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
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_RENAME, &req, sizeof(req),
                                 EFS_MSG_INODE_RENAME_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

int efs_client_rpc_rename_at(efs_export_id_t export_id, efs_ino_t old_parent,
                             const char *old_name, efs_ino_t new_parent,
                             const char *new_name, struct efs_inode *out)
{
    struct efs_msg_inode_rename_at req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.old_parent = old_parent;
    req.new_parent = new_parent;
    if (old_name)
        strncpy(req.old_name, old_name, EFS_MAX_NAME - 1);
    if (new_name)
        strncpy(req.new_name, new_name, EFS_MAX_NAME - 1);
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(old_parent, EFS_MSG_INODE_RENAME_AT, &req,
                                 sizeof(req), EFS_MSG_INODE_RENAME_AT_REPLY,
                                 &r, sizeof(r));
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
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_SETATTR, &req, sizeof(req),
                                 EFS_MSG_INODE_SETATTR_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out)
        *out = r.inode;
    return EFS_OK;
}

/* Cross-client O_APPEND: reserve the next len bytes at the owner; returns
 * the post-advance size (append offset = *new_size_out - len). */
int efs_client_rpc_append_reserve(efs_export_id_t export_id, efs_ino_t ino,
                                  uint64_t len, uint64_t *new_size_out)
{
    struct efs_msg_inode_append req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.len = len;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_APPEND, &req, sizeof(req),
                                 EFS_MSG_INODE_APPEND_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (new_size_out)
        *new_size_out = r.inode.size;
    return EFS_OK;
}

int efs_client_rpc_hold(efs_export_id_t export_id, efs_ino_t ino, int open,
                        uint64_t owner)
{
    struct efs_msg_inode_hold req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.flags = open ? 1u : 0u;
    req.owner = owner;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_HOLD, &req, sizeof(req),
                                 EFS_MSG_INODE_HOLD_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_rpc_flock(efs_export_id_t export_id, efs_ino_t ino, uint32_t op,
                         uint64_t owner)
{
    struct efs_msg_inode_flock req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.op = op;
    req.owner = owner;
    struct efs_msg_inode_reply r;
    int rc = rpc_send_recv_owner(ino, EFS_MSG_INODE_FLOCK, &req, sizeof(req),
                                 EFS_MSG_INODE_FLOCK_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
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
    int rc = rpc_send_recv_owner(new_parent, EFS_MSG_INODE_LINK, &req, sizeof(req),
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
 * owner of route_ino. sync=1 makes that owner commit before replying. */
int efs_client_rpc_report_dirty(efs_export_id_t export_id,
                                const struct efs_chunk_rec *recs,
                                uint32_t count,
                                const struct efs_ino_size_rec *irecs,
                                uint32_t ino_count, int sync,
                                efs_ino_t route_ino)
{
    if (count == 0 && ino_count == 0 && !sync)
        return EFS_OK;
    if (!route_ino)
        route_ino = EFS_ROOT_INO;
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
    int rc = rpc_send_recv_owner(route_ino, EFS_MSG_REPORT_CHUNKS, buf,
                                 (uint32_t)len, EFS_MSG_REPORT_CHUNKS_REPLY,
                                 &r, sizeof(r));
    free(buf);
    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);
}

int efs_client_load_shard(uint32_t shard)
{
    uint32_t n = g_client.export.root.shard_count;
    uint32_t bits = g_client.export.root.shard_bits;
    if (bits == 0 || n <= 1)
        return shard == 0 ? EFS_OK : EFS_ERR_NOT_FOUND;
    if (shard >= n)
        return EFS_ERR_INVAL;
    int rc = efs_export_load_shard(&g_client.export, shard);
    if (rc == EFS_OK)
        efs_export_evict_cold_shards(&g_client.export, EFS_SHARD_LRU_KEEP);
    return rc;
}
