# K100: auto-mode RDMA setup fails closed after its request was sent (Task DX)

Verdict: **PASS** (K100 FIXED, blocks alpha: no). Linked item: K100 in
[the checklist](../alpha-1.0-2plus1-checklist.md). Purpose: close the fail-open
fallback that Task DU recorded under "Limits" of
[K94](k94-rdma-upgrade-budget-20261107.md): in `transport auto`, a connection
whose RDMA SETUP exchange failed after the request went out was kept as TCP,
though the peer may already have switched it to RDMA.

## Defect

`efs_rdma_client_upgrade` (`src/common/rdma.c`) returned -1 for every failure,
and both callers, `peer_connect` (`src/server/peer_pool.c`, pooled and private
daemon-to-daemon connections) and `efs_client_conn_get`
(`src/client/node_cache.c`, `efs-fuse`), kept the connection as TCP in auto
mode on -1. The server publishes RDMA (`setup_publish`) before it sends its
reply (`src/server/handler.c`, `EFS_MSG_RDMA_SETUP`), so a client that loses or
rejects that reply (send/recv error or timeout, wrong reply type, an
undecodable record, a local `setup_peer` failure) can be talking TCP to a peer
that now reads the connection as RDMA. Strict mode (`transport rdma`) already
destroyed the connection. No incident was traced to this; it is a fail-closed
rule violation found by reading the code.

The protocol declaration before the upgrade needs no change: both callers
already destroy the connection on any declaration failure.

## Fix (`e21db3207`)

- `efs_rdma_client_upgrade` keeps -1 (TCP fallback allowed) for failures before
  the request is sent (no device, `conn_create`, encode) and for the peer's
  explicit refusal, the 1-byte `EFS_RDMA_STATUS_UNSUPPORTED` reply, which every
  server failure path sends without publishing RDMA. Every other failure after
  the send returns the new `EFS_RDMA_UPGRADE_BROKEN` (-2, `include/efs/rdma.h`).
- `peer_connect` and `efs_client_conn_get` destroy the connection on
  `EFS_RDMA_UPGRADE_BROKEN` in every mode; the caller fails or retries on a new
  connection, as for any connect failure. The client path has the same
  fallback, so it gets the same check.
- `tests/test_peer_pool_deadline.py`: the stubs can now hand `peer_connect` a
  live connection with RDMA available in auto mode. New case: an upgrade that
  returns -1 keeps the connection (no destroy); one that returns
  `EFS_RDMA_UPGRADE_BROKEN` returns NULL and destroys it.

## Tested

- Source: devel `658a5c67c` (wave 62, K94 included) as the before tree; after
  tree = `658a5c67c` plus the `e21db3207` diff. Both shipped with `git archive`
  (tarball sha256 `c8c6a2dc...5c69`, patch sha256 `c2397370...94ab`), so the
  build id reads `unknown`; built with `make -j32` and
  `make -j32 test-bins tests/faults/efsd`, gcc 11.5.0, `-O3 -march=native`,
  rpath to the private libfuse.
- Host node9900 (192 CPUs), `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`,
  `TMPDIR=/tmp/efs-taskDX-tmp`, harness-chosen loopback ports, buffered I/O,
  `transport auto` (loopback picks RDMA on this host's HCA). Task DY's
  `test_k36_recovery` runs shared the host during the gate.
- Unit case before: the new harness file on the before tree, with the constant
  that devel lacks injected (`CC="cc -DEFS_RDMA_UPGRADE_BROKEN=-2" python3
  tests/test_peer_pool_deadline.py`). After: the same file on the after tree.
- `make test`; TSan on the daemon tests that open pooled peer and client RDMA
  connections; one 8-job gate; one solo K38 run with kept logs.

## Results

| Check | Before (`658a5c67c`) | After (`e21db3207`) |
| --- | --- | --- |
| `test_peer_pool_deadline.py` K100 case | abort (SIGABRT) at `!server_peer_conn_new(...) && destroys==before+1`: the broken upgrade was kept as TCP | PASS (all four lines) |
| `make test` | not run | rc 0 |
| TSan `k96_status_slots_live k38_swept_hint_live k88_chunk_hints_live d4_progress heartbeat_shutdown k36_recovery` | not run | VERDICT PASS, 6 of 6, 0 failed (220 s) |
| 8-job `make test-all` gate | not run | `TOTAL 593 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (632s)`, no row over budget |
| Solo K38 (`EFS_K38_KEEP=1`), daemon and mount logs | not run | PASS, 10.4 s (write+fsync 5.6 s); 526 `RDMA transport up`, 0 `segmented RDMA setup failed`, 0 `RDMA upgrade to` (auto fallback notes), 0 `RDMA send type=95 FAILED` |

NOTRUN rows are host limits: `test_cluster_stop` (no `~/git/cluster` on the
host), `test_rdma_memcheck` and `test_leak_gate` (no valgrind),
`test_qualify_rdma_live` (hardware runs NOTRUN). Gate timings of the touched
rows: `test_k38_shared_chunk_pressure` 11.6 s (budget 20 s),
`test_k36_recovery` 26.5 s (55 s), `test_peer_pool_deadline` 0.1 s (30 s).
Build warnings are the same 8 on both trees.

## Reproduction

```bash
# on an RDMA host with libfuse 3.16 (EFS_FUSE_PREFIX); before = 658a5c67c, after = e21db3207
make -j32 && make -j32 test-bins tests/faults/efsd
python3 tests/test_peer_pool_deadline.py          # after tree
git show e21db3207:tests/test_peer_pool_deadline.py > tests/test_peer_pool_deadline.py   # before tree only
CC="cc -DEFS_RDMA_UPGRADE_BROKEN=-2" python3 tests/test_peer_pool_deadline.py           # before tree: aborts
make test
EFS_SAN_ONLY="k96_status_slots_live k38_swept_hint_live k88_chunk_hints_live d4_progress heartbeat_shutdown k36_recovery" bash tests/sanitizer_check.sh tsan
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh   # summary.txt line 1
EFS_K38_KEEP=1 python3 tests/test_k38_shared_chunk_pressure.py
grep -c 'segmented RDMA setup failed' <kept>/k/*.log   # expect 0; delete <kept> afterwards
```

Cleanup: remove `/tmp/efs-taskDX` and `/tmp/efs-taskDX-tmp`.

## Limits

- The classification inside `efs_rdma_client_upgrade` (which failures count as
  after-send) is verified by reading the code and by the gate's success path
  (526 upgrades, 0 failures); no test injects a lost or bad SETUP reply against
  real verbs. The harness case covers the callers' handling of both codes.
- Not distinguished: a peer that refuses by closing the TCP connection rather
  than replying UNSUPPORTED is now treated as broken (destroyed). That
  connection could not carry TCP traffic anyway.
- The client path (`efs_client_conn_get`) has no unit case of its own; it is
  the same three-line check, exercised by the gate's mounts.
- Hosts: node9900 only (loopback, one HCA). fstor007 and distinct-host RDMA
  were not run.

## Raw captures

`~/efs-artifacts/taskDX-20261108/` on the ORCD hosts (NFS home): unit before
and after output, `make-test.txt`, `tsan.txt`, `gate-summary.txt`,
`timings.tsv`, `gate.txt` (driver rc), the K38 solo run output and its log
counts (`k38-solo-counts.tsv`), `inputs.sha256` (tarball and patch). 10 files
plus `SHA256SUMS`, sha256
`e095dfd190aa9982bbe39f47709f91ad78d354a622e6da6d042047415aba8e38`.
Reproduction does not depend on them.
