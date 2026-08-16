#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int rpc_first_fd(void)
{
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        if (efs_client_node_is_down(g_client.nodes[i].id))
            continue;
        int fd = efs_client_conn_get(g_client.nodes[i].id);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

static int rpc_send_recv(uint8_t type, const void *req, uint32_t req_len,
                         uint8_t expect, void *reply, uint32_t reply_len)
{
    int fd = rpc_first_fd();
    if (fd < 0)
        return EFS_ERR_NET;
    if (efs_send_msg(fd, type, req, req_len) != 0) {
        efs_client_conn_drop(g_client.nodes[0].id, fd);
        return EFS_ERR_NET;
    }
    uint8_t rtype = 0;
    void *payload = NULL;
    uint32_t plen = 0;
    int rc = efs_recv_msg(fd, &rtype, &payload, &plen);
    if (rc != 0) {
        efs_client_conn_drop(g_client.nodes[0].id, fd);
        return EFS_ERR_NET;
    }
    efs_client_conn_release(g_client.nodes[0].id, fd);
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
    case EFS_INODE_RPC_OK:        return EFS_OK;
    case EFS_INODE_RPC_NOT_FOUND: return EFS_ERR_NOT_FOUND;
    case EFS_INODE_RPC_EXIST:     return EFS_ERR_EXIST;
    case EFS_INODE_RPC_QUOTA:     return EFS_ERR_QUOTA;
    case EFS_INODE_RPC_BUSY:      return EFS_ERR_BUSY;
    default:                      return EFS_ERR_IO;
    }
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
                          efs_ino_t *out_ino)
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
    int rc = rpc_send_recv(EFS_MSG_INODE_CREATE, &req, sizeof(req),
                           EFS_MSG_INODE_CREATE_REPLY, &r, sizeof(r));
    if (rc != EFS_OK)
        return rc;
    if (r.status != EFS_INODE_RPC_OK)
        return rpc_status_to_efs(r.status);
    if (out_ino)
        *out_ino = r.inode.ino;
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
    int rc = rpc_send_recv(EFS_MSG_INODE_UNLINK, &req, sizeof(req),
                           EFS_MSG_INODE_UNLINK_REPLY, &r, sizeof(r));
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
