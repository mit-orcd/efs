#!/usr/bin/env python3
"""D39 admission against the model (docs/status/d39-consolidated-model-20261023.md).

1. The C configuration parser (src/common/cluster_conf.c) accepts and refuses
   exactly what the model's parse_config does, and its digest equals the
   model's SHA-256 of the canonical records.
   (make runs tests/test_d39_admission itself: the model's traces through the
   real entry points, member store, declaration gate + admission, frame
   predicates, formation state, peer pool.)
2. The daemon wiring the traces cannot reach in process: identity comes only
   from --cluster and the endpoint, EFS_MD_RAFT_N is refused, every Raft
   frame is checked at receive and again per item at the inbox drain, and a
   peer connection is validated before the RDMA upgrade.
"""
from pathlib import Path
import subprocess, sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests'))
import d39_model as M  # noqa: E402

binary = root / 'tests/test_d39_admission'
assert binary.exists(), 'build tests/test_d39_admission first (make tests)'


def c_parse(text):
    out = subprocess.run([str(binary), 'conf'], input=text.encode(), capture_output=True,
                         check=True).stdout.decode().strip()
    kind, _, rest = out.partition(' ')
    return kind, rest


cases = [
    '1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17432\n',
    '# comment\n3   10.0.0.2:17432  # third\n\n1 10.0.0.1:17432\n2\t10.0.0.1:17433\n',
    '1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17433\n',
    M.default_conf(3), M.default_conf(4), M.default_conf(5), M.default_conf(16),
    ''.join('%d 10.0.0.1:%d\n' % (i, i) for i in range(1, 17)),
    # refused (s5_malformed_input_refused and more)
    '1 10.0.0.1:1\n2 10.0.0.1:2\n3junk 10.0.0.1:3\n',
    '1 10.0.0.1:1\n2 10.0.0.1:2\n2 10.0.0.1:3\n',
    '1 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:2\n',
    '1 10.0.0.1:1\n2 10.0.0.1:2\n',
    ''.join('%d 10.0.0.1:%d\n' % (i, i) for i in range(1, 18)),
    '1 10.0.0.1:1\n2 10.0.0.1:2\n4 10.0.0.1:3\n',
    '1 0.0.0.0:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 255.255.255.255:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 node1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.01:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '01 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.1:0\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.1:065\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.1:65536\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.1:1 x\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '1 10.0.0.256:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    '0 10.0.0.1:1\n1 10.0.0.1:2\n2 10.0.0.1:3\n',
    '',
]
accepted = refused = 0
for text in cases:
    kind, rest = c_parse(text)
    try:
        want = M.parse_config(text)
    except M.ConfigError:
        want = None
    if want is None:
        assert kind == 'REFUSED' and rest, (text, kind, rest)
        refused += 1
    else:
        assert kind == 'OK' and rest == want.digest(), (text, kind, rest, want.digest())
        accepted += 1
print('d39 admission: configuration parity with the model (%d accepted, %d refused) PASS'
      % (accepted, refused))

src = {p: (root / p).read_text() for p in (
    'src/server/efsd.c', 'src/server/raft_host.c', 'src/server/handler.c',
    'src/server/peer_pool.c', 'src/mgmt/efs_mgmt.c', 'include/efs/cli_help.h')}
efsd = src['src/server/efsd.c']
assert '"--cluster"' in efsd and 'efs_cluster_conf_member_at(&server.cluster, server.addr' in efsd
assert 'was removed (D39)' in efsd and '"--node-id"' in efsd and '"--join"' in efsd, \
    '--node-id and --join stop startup with a clear error'
assert 'EFS_MD_RAFT_N was removed' in efsd, 'W153a: a stale EFS_MD_RAFT_N stops startup'
own = efsd.index('member_store_open(&server.mstore')
assert own < efsd.index('server_storage_roots_init(&server)') < efsd.index('efs_listen_tcp('), \
    'store ownership and the boot counter come before anything is opened or advertised'
host = src['src/server/raft_host.c']
assert 'getenv("EFS_MD_RAFT_N")' not in host and 'n = (int)s->cluster.n;' in host
assert 'h->boot_id = s->incarnation;' in host and 'host_boot_bump' not in host
drain = host[host.index('static void drain_inbox('):]
drain = drain[:drain.index('\n}\n')]
assert drain.index('adm_check_control(&local[i].tag)') < drain.index('efs_wire_raft_decode('), \
    'each drained Raft item is checked against the binding before any effect'
queued = host[host.index('static void host_note_queued_reads('):]
queued = queued[:queued.index('\n}\n')]
assert queued.index('adm_check_control(&items[i].tag)') < queued.index('efs_raft_read_control('), \
    'queued read-proof inspection checks each item'
assert '(void)server_raft_host_inbox(payload, payload_len, &decl.id);' in src['src/server/handler.c'], \
    'inbox items carry their frame\'s tag'
peer = src['src/server/peer_pool.c']
pc = peer[peer.index('static struct efs_conn *peer_connect('):]
pc = pc[:pc.index('\n}\n')]
assert pc.index('adm_check_reply(member, &rep') < pc.index('efs_rdma_client_upgrade(nc)'), \
    'a peer connection is used only after its reply validates'
assert 'EFS_MD_CMD_CFG' in host and 'membership is fixed by' in host, 'online membership change refused'
assert '"raft-change"' in src['src/mgmt/efs_mgmt.c'] and 'was removed (D39)' in src['src/mgmt/efs_mgmt.c']
assert '--node-id ID' not in src['include/efs/cli_help.h'] and '--join HOST' not in src['include/efs/cli_help.h']
print('d39 admission: daemon wiring (identity, W153a, drain checks, peer validation) PASS')
