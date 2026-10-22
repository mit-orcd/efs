#!/usr/bin/env python3
"""W132 / K3: a NODE_LEFT frame (retired ID 24) is refused fail-closed.

Nothing sends NODE_LEFT, and the handler that dropped a node from the
membership for any connection is gone. The test embeds the real receive
and refusal prefix of server_handle_conn, sends a NODE_LEFT frame over a
socketpair and checks: no reply, the connection is closed, nothing is
dispatched, membership is unchanged. A HEARTBEAT control frame proves the
embedded loop dispatches live frames, so the refusal is not a harness
artifact."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
proto = (root/'include/efs/protocol.h').read_text()
handler = Path(os.environ.get('EFS_HANDLER_SOURCE', root/'src/server/handler.c')).read_text()

# Source gate: the ID is retired, the struct and the handler are gone, and
# no server code path removes a node on request.
enum = proto[proto.index('enum efs_msg_type'):]
enum = enum[:enum.index('};')]
assert 'EFS_MSG_NODE_LEFT' not in enum, 'NODE_LEFT must not be a live message'
assert not re.search(r'^\s*EFS_MSG_\w+\s*=\s*24,', enum, re.M), 'ID 24 reused'
macro = re.search(r'#define EFS_MSG_IS_RETIRED\(t\) \\\n((?:.*\\\n)*.*\n)', proto).group(1)
assert 24 in map(int, re.findall(r'== (\d+)', macro)), 'ID 24 must be retired'
assert 'struct efs_msg_node_left' not in proto
assert 'EFS_MSG_NODE_LEFT' not in handler and 'node_left' not in handler
for p in list((root/'src').rglob('*.[ch]')) + list((root/'include').rglob('*.h')):
    assert 'server_remove_node_from_cluster' not in p.read_text(), p

# Behavioural gate: the real prefix of server_handle_conn (frame read,
# declaration gate, retired refusal, close) in front of a stub dispatcher
# that records what reaches the switch and mutates a fake membership the
# way the deleted handler would have.
a = handler.index('void server_handle_conn(')
prefix = handler[a:handler.index('switch (type) {', a) + len('switch (type) {')]
def fn(name):
    i = handler.index('static int %s(uint8_t type)' % name)
    return handler[i:handler.index('\n}', i)+2]
source = r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define EFS_HANDLER_TLS_MAX (1024 * 1024)
static __thread uint8_t *tls_payload;
static __thread uint32_t tls_payload_cap;
static int rdma_first_on(void) { return 0; }
int efs_conn_wait_request(struct efs_conn *c) { (void)c; return EFS_CONN_TCP; }
int efs_rdma_message_wait(struct efs_rdma_conn *rc, int ms) { (void)rc; (void)ms; abort(); }
void *efs_rdma_message_frame(struct efs_rdma_conn *rc, uint32_t *len) { (void)rc; (void)len; abort(); }
void efs_rdma_message_release(struct efs_rdma_conn *rc) { (void)rc; abort(); }
uint32_t efs_rdma_max_frame(struct efs_rdma_conn *rc) { (void)rc; abort(); }
int efs_recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}
static int dispatched, dispatched_type = -1;
static uint32_t membership = 3; /* nodes the deleted handler would have shrunk */
/* W133 gate stub with the real undeclared policy: before a declaration
 * only CLIENT_HELLO and HEARTBEAT pass (test_declaration_gate covers the
 * real gate). Retired IDs never reach it. */
int efs_decl_gate_frame(struct efs_decl_gate *g, struct efs_conn *c, uint8_t type,
                        const void *payload, uint32_t len)
{
    (void)c; (void)payload; (void)len;
    assert(!EFS_MSG_IS_RETIRED(type));
    return !g->declared && type != EFS_MSG_HEARTBEAT ? EFS_GATE_REFUSE : EFS_GATE_DISPATCH;
}
''' + fn('retired_refuse') + prefix + r'''
        default:
            (void)rc;
            dispatched++;
            dispatched_type = type;
            membership--;
            break;
        }
        free(to_free);
    }
}
static void send_frame(int fd, uint8_t type, const void *payload, uint32_t plen)
{
    uint32_t len = htonl(1 + plen);
    assert(write(fd, &len, 4) == 4 && write(fd, &type, 1) == 1);
    if (plen) assert(write(fd, payload, plen) == (ssize_t)plen);
}
static int run(uint8_t type, uint32_t plen, int half_close, uint8_t *reply, size_t cap)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    uint8_t payload[64] = {0}; payload[0] = 2; /* node_id 2, little-endian */
    send_frame(sv[1], type, payload, plen);
    if (half_close) assert(shutdown(sv[1], SHUT_WR) == 0);
    else send_frame(sv[1], EFS_MSG_HEARTBEAT, NULL, 0); /* must never be answered */
    struct efs_conn conn; memset(&conn, 0, sizeof(conn));
    conn.kind = EFS_CONN_TCP; conn.is_server = 1; conn.fd = sv[0];
    server_handle_conn(&conn);          /* returns: the server closes the conn */
    close(sv[0]);
    ssize_t n = read(sv[1], reply, cap);
    /* Closed with no reply bytes: EOF, or ECONNRESET where the kernel resets
     * a peer that closed with an unread frame queued (Linux AF_UNIX). */
    int closed_unanswered = n == 0 || (n < 0 && errno == ECONNRESET);
    close(sv[1]);
    return closed_unanswered ? 0 : n > 0 ? (int)n : -1;
}
int main(void)
{
    uint8_t reply[64];
    /* NODE_LEFT with the old 4-byte node_id payload: refused, no reply, closed. */
    assert(run(24, 4, 0, reply, sizeof(reply)) == 0);
    assert(dispatched == 0 && dispatched_type == -1 && membership == 3);
    /* An empty NODE_LEFT is refused the same way. */
    assert(run(24, 0, 0, reply, sizeof(reply)) == 0);
    assert(dispatched == 0 && membership == 3);
    /* Control: a live frame reaches the dispatcher on this harness. */
    assert(run(EFS_MSG_HEARTBEAT, 0, 1, reply, sizeof(reply)) == 0);
    assert(dispatched == 1 && dispatched_type == EFS_MSG_HEARTBEAT && membership == 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-node-left-') as d:
    path = Path(d)/'test.c'; path.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc'))+['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I'+str(root/'include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, stderr=subprocess.DEVNULL)
print('node-left: ID 24 retired, handler and remover deleted; a NODE_LEFT frame gets no reply, '
      'the connection closes, nothing dispatches, membership unchanged PASS')
