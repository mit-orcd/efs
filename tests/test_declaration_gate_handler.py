#!/usr/bin/env python3
"""W133 (K4): the handler runs every frame through the declaration gate before
dispatch, and every program that opens a connection to efsd declares first."""
from pathlib import Path
import glob, re
root = Path(__file__).resolve().parents[1]
proto = (root/'include/efs/protocol.h').read_text()
handler = (root/'src/server/handler.c').read_text()

# One supported protocol: no minimum, no range, anywhere in the tree.
for path in [root/'include', root/'src', root/'tests', root/'scripts']:
    for f in path.rglob('*'):
        if f.is_file() and f.suffix in ('.c', '.h', '.py', '.sh') and f != Path(__file__).resolve():
            assert 'EFS_PROTOCOL_' + 'MIN_VERSION' not in f.read_text(errors='replace'), f
assert re.search(r'#define EFS_PROTOCOL_VERSION 3u\n', proto), 'protocol 3 = every request gated'

# The gate runs on every frame before the switch; a refusal closes the
# connection without a reply; an answered declaration dispatches nothing.
loop = handler[handler.index('void server_handle_conn('):]
assert 'struct efs_decl_gate decl = {0};' in loop, 'one gate per connection'
gate = loop.index('gate = efs_decl_gate_frame(&decl, conn, type, payload, payload_len);')
refuse = loop.index('if (gate == EFS_GATE_REFUSE)\n            close_conn = 1;', gate)
skip = loop.index('if (close_conn) {', refuse)
answered = loop.index('if (gate == EFS_GATE_ANSWERED) {', skip)
dispatch = loop.index('switch (type) {')
assert gate < refuse < skip < answered < dispatch, 'gate, refusal, skip, answered, dispatch'
block = loop[skip:loop.index('}', skip)]
assert 'break;' in block and 'free(to_free);' in block, block
ans = loop[answered:loop.index('switch (type) {', answered)]
assert 'continue;' in ans and 'free(to_free);' in ans and 'efs_rdma_message_release' in ans, ans
assert 'case EFS_MSG_CLIENT_HELLO' not in loop, 'the declaration is answered by the gate, never dispatched'
assert 'EFS_MSG_HEARTBEAT' in loop[dispatch:], 'HEARTBEAT is still served'
assert 'declared' not in loop.replace('decl', ''), 'no second declaration state'
assert 'undeclared_refuse' not in handler, 'the old three-message refusal is gone'

# Every connection opener declares first. Programs that connect and send
# nothing (reachability probes) and loopback harnesses that are their own
# server are the only efs_connect_tcp callers left outside the helper.
mgmt = (root/'src/mgmt/efs_mgmt.c').read_text()
assert mgmt.count('efs_connect_tcp(') == 1, 'efs-mgmt connects through efs_connect_declared'
probe = mgmt[mgmt.index('static int probe_node_up('):]
probe = probe[:probe.index('\n}\n')]
assert 'efs_connect_tcp(' in probe and 'efs_fd_declare(pfd)' in probe, 'status probe declares under its own bound'
for f in sorted(glob.glob(str(root/'tests/live/*.c'))) + [str(root/'tests/measure/metadata_route_smoke.c')]:
    assert 'efs_connect_tcp(' not in Path(f).read_text(), (f, 'live programs connect through efs_connect_declared')
node = (root/'src/client/node_cache.c').read_text()
get = node[node.index('struct efs_conn *efs_client_conn_get('):]
get = get[:get.index('\n}\n')]
assert get.index('efs_conn_declare(nc)') < get.index('efs_rdma_client_upgrade(nc)'), 'client declares before the RDMA upgrade'
disc = node[node.index('int efs_client_discover_nodes('):]
disc = disc[:disc.index('\n}\n')]
assert disc.index('efs_fd_declare(fd)') < disc.index('EFS_MSG_LIST_NODES'), 'discovery declares before LIST_NODES'
peer = (root/'src/server/peer_pool.c').read_text()
pc = peer[peer.index('static struct efs_conn *peer_connect('):]
pc = pc[:pc.index('\n}\n')]
assert pc.index('efs_conn_declare(nc)') < pc.index('efs_rdma_client_upgrade(nc)'), 'peers declare before the RDMA upgrade'
fuse = (root/'src/client/efs_fuse.c').read_text()
boot = fuse[fuse.index('static int raft_bootstrap_metadata('):]
boot = boot[:boot.index('\nready:')]
assert boot.index('efs_fd_declare(fd)') < boot.index('EFS_MSG_RAFT_STATUS'), 'bootstrap declares before RAFT_STATUS'
# Raw-socket Python senders and the W127 gate script declare too.
hello = "struct.pack('>IB',17,165)+b'EFSP'+struct.pack('=IQ',3,0)"
for f in ['tests/live/expiry_lifecycle.py', 'tests/live/fragment_integrity.py', 'tests/live/fragment_tail_smoke.py',
          'tests/live/io_stats_smoke.py', 'tests/live/gc_reclamation.py', 'tests/live/query_rpc.py',
          'tests/live/raft_read_freshness.py', 'scripts/w127_incompatible_entry.sh']:
    assert hello in (root/f).read_text(), (f, 'declares with CLIENT_HELLO v3')
assert 'PASS 2 old client' in (root/'scripts/w125_p3_compat.sh').read_text() or \
       'check "2 daemons logged the refusal"' in (root/'scripts/w125_p3_compat.sh').read_text(), 'compat script expects refusal'
print('declaration gate: handler gates every frame before dispatch; every opener declares; protocol 3 exact PASS')
