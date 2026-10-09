/* Same CREATE identity must retain EEXIST across unlink, cold replay and reuse. */
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/opid.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static unsigned base, port;
static struct efs_msg_inode_reply call(unsigned type, const void *req,
                                       unsigned size, unsigned seq)
{
    unsigned char packet[1024];
    struct efs_opid_req q = {0};
    assert(size + EFS_OPID_WIRE_LEN <= sizeof(packet));
    memcpy(packet, req, size);
    memcpy(q.id.client_uuid, "W58-negative", 12);
    q.id.session_epoch = 1; q.id.seq = seq; /* ack stays zero: retain verdict 2 */
    efs_opid_req_pack(&q, packet + size);
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        int fd = efs_connect_tcp("127.0.0.1", port);
        assert(fd >= 0);
        efs_set_recv_timeout(fd, 5000); efs_set_send_timeout(fd, 5000);
        assert(!efs_send_msg(fd, type, packet, size + EFS_OPID_WIRE_LEN));
        void *body = NULL; unsigned len = 0; unsigned char kind = 0;
        int rc = efs_recv_msg(fd, &kind, &body, &len); close(fd);
        if (rc) { free(body); usleep(10000); continue; }
        assert(kind == type + 1 && len == sizeof(struct efs_msg_inode_reply));
        struct efs_msg_inode_reply rep; memcpy(&rep, body, sizeof(rep)); free(body);
        if (rep.status == EFS_INODE_RPC_NOT_PRIMARY) {
            if (rep.primary_id >= 1 && rep.primary_id <= 4)
                port = base + rep.primary_id - 1;
            usleep(10000); continue;
        }
        if (rep.status == EFS_INODE_RPC_BUSY) { usleep(10000); continue; }
        return rep;
    }
    abort();
}
int main(int argc, char **argv)
{
    assert(argc == 3); base = port = atoi(argv[1]);
    struct efs_msg_inode_create create = {0};
    create.export_id = 1; create.parent = 1; create.mode = S_IFREG | 0600;
    strcpy(create.name, "w58-negative");
    struct efs_msg_inode_unlink unlink = {0};
    unlink.export_id = 1; unlink.parent = 1; strcpy(unlink.name, create.name);
    if (!strcmp(argv[2], "prepare")) {
        assert(call(EFS_MSG_INODE_CREATE, &create, sizeof(create), 1).status == EFS_INODE_RPC_OK);
        assert(call(EFS_MSG_INODE_CREATE, &create, sizeof(create), 2).status == EFS_INODE_RPC_EXIST);
        assert(call(EFS_MSG_INODE_UNLINK, &unlink, sizeof(unlink), 3).status == EFS_INODE_RPC_OK);
    } else {
        assert(!strcmp(argv[2], "replay"));
        assert(call(EFS_MSG_INODE_CREATE, &create, sizeof(create), 2).status == EFS_INODE_RPC_EXIST);
        struct efs_msg_inode_reply replacement = call(EFS_MSG_INODE_CREATE, &create, sizeof(create), 4);
        assert(replacement.status == EFS_INODE_RPC_OK);
        assert(call(EFS_MSG_INODE_CREATE, &create, sizeof(create), 2).status == EFS_INODE_RPC_EXIST);
        struct efs_msg_inode_reply replay = call(EFS_MSG_INODE_CREATE, &create, sizeof(create), 4);
        assert(replay.status == EFS_INODE_RPC_OK && replay.inode.ino == replacement.inode.ino);
        assert(call(EFS_MSG_INODE_UNLINK, &unlink, sizeof(unlink), 5).status == EFS_INODE_RPC_OK);
    }
    puts("CREATE original EEXIST identity retained PASS");
}
