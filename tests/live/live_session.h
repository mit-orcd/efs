/* K5 helpers for raw-RPC live harnesses: every metadata mutation names a
 * session that exists on its owner shard (CREATE) and, for client-admitted
 * paths (HOLD, FLOCK, APPEND, SETATTR, XATTR), is established on the target
 * shard (REGISTER + ESTABLISH, or one REGISTER_ESTABLISH in one group).
 * Namespace hosts admit their own served shard but never create a session.
 * Daemons listen on 127.0.0.1:port..port+3. */
#ifndef EFS_LIVE_SESSION_H
#define EFS_LIVE_SESSION_H
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline void live_session_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* One session command to `group`; retries leader changes and BUSY. */
static inline int live_session_command(unsigned port, uint8_t group, uint8_t sub,
                                const uint8_t uuid[16], uint32_t epoch,
                                uint32_t shard, int with_shard)
{
    uint8_t body[27] = {group, EFS_MD_CMD_SESSION, sub};
    memcpy(body + 3, uuid, 16);
    live_session_be32(body + 19, epoch);
    live_session_be32(body + 23, shard);
    unsigned node = 0;
    for (unsigned attempt = 0; attempt < 400; attempt++) {
        int fd = efs_connect_declared("127.0.0.1", port + node);
        if (fd < 0) { node = (node + 1) % 4; usleep(10000); continue; }
        efs_set_recv_timeout(fd, 5000); efs_set_send_timeout(fd, 5000);
        uint8_t type = 0; void *reply = NULL; uint32_t bytes = 0;
        int rc = efs_send_msg(fd, EFS_MSG_RAFT_MKFS, body, with_shard ? 27 : 23);
        if (!rc) rc = efs_recv_msg(fd, &type, &reply, &bytes);
        close(fd);
        if (rc) { free(reply); node = (node + 1) % 4; usleep(10000); continue; }
        assert(type == EFS_MSG_RAFT_MKFS_REPLY && bytes == sizeof(struct efs_msg_raft_mkfs_reply));
        struct efs_msg_raft_mkfs_reply r; memcpy(&r, reply, sizeof(r)); free(reply);
        if (r.rc == EFS_ERR_NOT_PRIMARY) {
            node = r.leader_hint >= 0 && r.leader_hint < 4 ? (unsigned)r.leader_hint : (node + 1) % 4;
            usleep(10000); continue;
        }
        if (r.rc == EFS_ERR_BUSY || r.rc == EFS_ERR_NO_QUORUM) { usleep(10000); continue; }
        return r.rc;
    }
    return EFS_ERR_BUSY;
}

/* CREATE is a no-op for an existing session. */
static inline void live_session_create(unsigned port, const uint8_t uuid[16], uint32_t epoch)
{
    uint8_t owner = efs_raft_shard_group(efs_kv_session_shard(uuid));
    assert(live_session_command(port, owner, EFS_MD_SESS_CREATE, uuid, epoch, 0, 0) == EFS_OK);
}

static inline void live_session_admit(unsigned port, const uint8_t uuid[16], uint32_t epoch,
                               uint32_t shard)
{
    uint8_t owner = efs_raft_shard_group(efs_kv_session_shard(uuid));
    uint8_t group = efs_raft_shard_group(shard);
    if (owner == group) {
        assert(live_session_command(port, group, EFS_MD_SESS_REGISTER_ESTABLISH,
                                    uuid, epoch, shard, 1) == EFS_OK);
        return;
    }
    assert(live_session_command(port, owner, EFS_MD_SESS_REGISTER, uuid, epoch, shard, 1) == EFS_OK);
    assert(live_session_command(port, group, EFS_MD_SESS_ESTABLISH, uuid, epoch, shard, 1) == EFS_OK);
}
#endif
