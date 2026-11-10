#!/usr/bin/env python3
"""K104 (Task FF): a refusal after a leader transfer must stop the client
from sending the group's requests to the old leader first.

Runs the real leader note, read routes, rpc_forget_leader and
raft_voter_conn_skip from src/client/inode_rpc.c. Node 3 led group 2, so the
note and a read route name it; group 2 moved to node 1 and node 3 is now a
halted follower (TCP open, no reply). After the refusal the next pick must be
a live voter, never node 3. Also checks that the shard RPC loop's hinted and
hintless NOT_PRIMARY paths and the session and put-ticket loops call the
helper. Against the old source the helper is missing and the test fails."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/client/inode_rpc.c').read_text()

def fn(signature):
    a = s.index(signature)
    return s[a:s.index('\n}', a) + 2] + '\n'

def body(signature):
    a = s.index(signature)
    return s[a:s.index('\n}\n', a)]

# The production loops call the helper on every NOT_PRIMARY they follow.
shard = body('static int rpc_send_recv_shard_once(')
hinted = shard[:shard.index('target = r->primary_id;\n    }\nexhausted:')]
assert re.search(r'rpc_forget_leader\(efs_raft_shard_group\(shard\), nid, r->primary_id\);\s*$', hinted), \
    'shard loop: a hinted NOT_PRIMARY must forget the old leader'
nohint = shard[shard.index('if (r->primary_id == 0 || r->primary_id == nid) {'):]
assert 'rpc_forget_leader(efs_raft_shard_group(shard), nid, 0);' in nohint[:nohint.index('continue;')]
for name in ('static int session_command_reply(', 'static int rpc_put_ticket('):
    b = body(name)
    np = b[b.index('NOT_PRIMARY) {'):]
    assert 'rpc_forget_leader(body[0],nid,' in np[:np.index('target=')], name
assert 'rpc_forget_leader(efs_raft_shard_group(shard), nid, 0);' in fn('static void rpc_retry_peer(')

code = r'''
#include "efs/common.h"
#include "efs/raft.h"
#include "metadata_read_route.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_conn { efs_node_id_t node; };
static struct { unsigned node_count; } g_client = { 3 };
static struct efs_conn conns[4];
static unsigned asked_halted;
static int efs_client_node_is_down(efs_node_id_t n) { (void)n; return 0; } /* not yet marked: halted moments ago */
static struct efs_conn *efs_client_conn_get(efs_node_id_t n) { if (n == 3) asked_halted++; conns[n].node = n; return &conns[n]; }
static struct efs_conn *rpc_primary_conn(efs_node_id_t *nid) { *nid = 1; return &conns[1]; }
''' + fn('static uint32_t raft_group_voters(') + '''
static efs_node_id_t g_group_leader[2];
static struct efs_meta_read_routes g_read_routes={.mu=PTHREAD_MUTEX_INITIALIZER};
''' + fn('static void rpc_note_leader(') + fn('static efs_node_id_t rpc_leader_of(') + \
    fn('static void rpc_forget_leader(') + fn('static struct efs_conn *raft_voter_conn_skip(') + r'''
int main(void)
{
    const uint8_t g2 = EFS_RAFT_GROUP_SHARD2;
    uint32_t shard = 0; /* even shard: group 2 */
    efs_node_id_t nid = 0;
    assert(efs_raft_shard_group(shard) == g2);
    /* Node 3 led group 2: the note and a read route name it. */
    rpc_note_leader(g2, 3);
    efs_meta_read_route_note(&g_read_routes, 77, 3);
    assert(raft_voter_conn_skip(shard, &nid, 0)->node == 3 && asked_halted == 1);
    /* Group 2 moved to node 1: node 3 refuses with hint 1. */
    rpc_forget_leader(g2, 3, 1);
    assert(rpc_leader_of(g2) == 0 && efs_meta_read_route_get(&g_read_routes, 77) == 0);
    asked_halted = 0;
    assert(raft_voter_conn_skip(shard, &nid, 0)->node == 1 && nid == 1 && !asked_halted);
    /* A newer note learned concurrently is kept by a refusal naming it. */
    rpc_note_leader(g2, 2);
    rpc_forget_leader(g2, 3, 2);
    assert(rpc_leader_of(g2) == 2);
    /* A refusal by another member whose hint disagrees drops a stale note. */
    rpc_note_leader(g2, 3);
    rpc_forget_leader(g2, 1, 2);
    assert(rpc_leader_of(g2) == 0);
    /* A hintless refusal by the noted leader drops it; by another keeps it. */
    rpc_note_leader(g2, 2);
    rpc_forget_leader(g2, 1, 0);
    assert(rpc_leader_of(g2) == 2);
    rpc_forget_leader(g2, 2, 0);
    assert(rpc_leader_of(g2) == 0);
    puts("client leader note: a refusal after a transfer forgets the old leader and its read routes; no request goes to the halted member PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-leader-forget-') as d:
    p = Path(d) / 'test.c'; p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'include'), '-I' + str(root / 'src/client'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
