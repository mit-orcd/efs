#!/usr/bin/env python3
"""A command a member forwarded is never forwarded again (flake sweep, Oct 25).

tests/test_report_stepdown.py wrote for 5.2-6.0 s across a metadata leader
change in 5 of 28 runs beside a pool run. The call-level trace showed why:
data PUT admissions (PUT_TICKET_ADMIT, tag 39) forwarded between two
followers of the group whose leader had just stepped down, node 1 to the
old leader and the old leader on, each hop holding a peer-pool connection,
until both pools were full and every queued forward waited out the 5 s
checkout and failed BUSY (`propose-slow group=2 tag=39 ... ms=5000 rc=-13`
65 times on two nodes). The submit path answered a forwarded generic
command NOT_PRIMARY on a follower since Sep 21, but the ticket admission
branches propose before that check, and host_propose forwarded on.

Now host_rpc_submit marks a member's forward in the group byte
(HOST_SUBMIT_FORWARDED), server_raft_host_submit serves a marked command
with host_serving_forward set (DIR_MIGRATE excepted: it is bounced to a
dual-host on purpose), and host_propose refuses to forward while it is set:
NOT_PRIMARY with the leader it knows, never itself, for the forwarder's
host_remote_cmd to retry. A client's or this process's own submit still
forwards one hop.

This test compiles host_submit_serves_forward and host_forward_refused from
src/server/raft_host.c and checks their contract, then checks the wiring:
the sender sets the mark, the submit entry sets the flag from the helper
and restores it, and host_propose consults the helper before its forward.
The end-to-end evidence (loaded step-down loops before and after) is in
docs/status/flake-sweep-20261025.md.
"""
import re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
s = (root / 'src/server/raft_host.c').read_text()


def fn(name):
    """The definition of name (not a prototype): its signature ends ')' + newline + '{'."""
    a = 0
    while True:
        a = s.index(name + '(', a)
        close = s.index(')', a)
        while s[close + 1:close + 3] != '\n{' and s[close + 1] != ';':
            close = s.index(')', close + 1)
        if s[close + 1:close + 3] == '\n{':
            break
        a = close
    a = s.rfind('\n', 0, a) + 1
    return s[a:s.index('\n}\n', a) + 3]


def define(name):
    m = re.search(rf'^#define {name} .*$', s, re.M)
    assert m, name
    return m.group(0) + '\n'


code = r'''
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + define('HOST_SUBMIT_FORWARDED') + define('HOST_DIR_LEN') + \
    fn('static int host_submit_serves_forward') + fn('static int host_forward_refused') + r'''
int main(void)
{
    uint8_t p[1 + 16] = {0}, g = 0xff;
    /* A client's or this process's own submit: forwards one hop. */
    p[0] = 2; p[1] = 39;
    assert(!host_submit_serves_forward(p, 2, &g) && g == 2);
    p[0] = 0;
    assert(!host_submit_serves_forward(p, 2, &g) && g == 0);
    /* A member's forward: served here, never forwarded on. */
    p[0] = 2 | HOST_SUBMIT_FORWARDED;
    assert(host_submit_serves_forward(p, 2, &g) && g == 2);
    p[0] = 0 | HOST_SUBMIT_FORWARDED;
    assert(host_submit_serves_forward(p, 1, &g) && g == 0);
    /* DIR_MIGRATE is bounced to a dual-host on purpose: it may forward. */
    p[0] = 2 | HOST_SUBMIT_FORWARDED; p[1] = EFS_MD_CMD_DIR; p[2] = EFS_MD_DIR_MIGRATE;
    assert(!host_submit_serves_forward(p, 1 + HOST_DIR_LEN, &g) && g == 2);
    assert(host_submit_serves_forward(p, HOST_DIR_LEN, &g)); /* too short to be one */
    assert(!host_submit_serves_forward(p, 0, &g) && g == 0);
    /* The refusal names the leader this member knows, never itself. */
    int hint = 7;
    assert(!host_forward_refused(0, 1, 0, &hint) && hint == 7);
    assert(host_forward_refused(1, 1, 0, &hint) && hint == 1);
    assert(host_forward_refused(1, -1, 0, &hint) && hint == -1);
    assert(host_forward_refused(1, 0, 0, &hint) && hint == -1);
    assert(host_forward_refused(1, 2, 0, NULL));
    puts("ok");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-fwd-once-') as d:
    src = Path(d) / 't.c'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
                    str(src), '-o', str(src.with_suffix(''))], check=True)
    out = subprocess.run([str(src.with_suffix(''))], check=True, capture_output=True, text=True).stdout
    assert out.strip() == 'ok', out

# Wiring: the sender marks, the entry sets and restores, propose consults.
send = fn('static int host_rpc_submit')
assert 'payload[0] = (uint8_t)(group | HOST_SUBMIT_FORWARDED);' in send, send[:400]
entry = fn('void server_raft_host_submit')
# The marker is restored on every return (scope cleanup) and set from the
# helper before any branch can propose; the body never reads the raw byte.
assert re.search(r'\{\n    __attribute__\(\(cleanup\(host_serving_forward_restore\)\)\) int serving_outer = '
                 r'host_serving_forward;', entry), entry[:600]
assert 'host_serving_forward = *previous;' in fn('static void host_serving_forward_restore')
setter = entry.index('host_serving_forward = host_submit_serves_forward(payload, plen, &group);')
for first in ('host_propose', 'host_remote_cmd(', 'host_put_ticket_batch_submit('):
    assert entry.find(first) < 0 or entry.index(first) > setter, first
assert 'payload[0]' not in entry, 'the submit entry reads the marked group byte'
prop = fn('static int host_propose_impl')
refuse = prop.index('host_forward_refused(host_serving_forward, lid, h->raft_id, leader_hint)')
assert refuse < prop.index('rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);'), 'forward before refusal'
print('forward once: a member-forwarded submit is served without a further forward (DIR_MIGRATE '
      'excepted), client submits forward one hop, the refusal hints the known leader PASS')
