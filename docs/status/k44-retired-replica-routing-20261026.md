# K44 / W167 — routing at a retired replica (October 26)

`agent/devel-task-k`, source `ae0262306`: final gate NOTRUN/queued; base `4066f74df` reproduces K44 (expected negative-control FAIL). Evidence: `docs/status/k44-retired-replica-routing-20261026.md`. Final TCP/RDMA and base gate comparison pending; orcd-mini NOTRUN, requires its own stop/format/deploy approval. Product defects: K44 retired-seed MKFS BUSY; retired resync accepted; non-group-0 MKFS readback cannot finish. Candidate fixes are not yet qualified.

## Purpose and verdict

Task K in the coordinator's October 26 task brief, [K44](alpha-1.0-2plus1-checklist.md)
and [W167](work-queue.md). **IN PROGRESS**, not a release PASS or closed item.
The origin [ORCD-test run](orcd-test-devel-rdma-394f2b791-20261027.md)
reported repeatable MKFS BUSY after automatic formation and an online addition.
All this task's execution is on **node9900**, in private `/tmp` trees;
existing orcd-mini services, stores and mounts are untouched.

## Confirmed mechanism and changes

The untouched base's four-member fixture reproduces `raft-mkfs rc=-13` at
the seed. Its log records retirement followed by
`apply-sleep ... group=2 idx=6 applied=4` for approximately 400 ms. The
forwarded SALT's leader verdict was already available, but the caller then
waited for its retired local replica to apply that index. The replica's
old Raft object also remained eligible for local proposal admission checks.
This is not a slow group requiring a longer timeout.

The candidate separates application service from consensus participation:

- A retired replica cannot supply local proposal admission or a local
  verdict. Requests use the current voters and the live leader's actual
  submit verdict, including rejections. A local wait racing retirement
  returns NOT_PRIMARY, never success inferred from the old cursor.
- A forwarded request reaching a retired replica returns NOT_PRIMARY and a
  known live leader hint; it never creates a forwarding chain. Configuration
  forwarding follows the same one-hop rule. Routing does not use the
  retired replica's obsolete voter mask or its frozen leader view.
- `raft-status` keeps `retired=1` and the historical local indices, but
  reports the leader learned from successful live-group routing. The hint
  may be unknown until observed; it is not an authoritative read proof.
- Retired replicas refuse `raft-resync` with NOT_PRIMARY and cannot start
  new snapshot exports. The CLI diagnostic now says leader **or retired**,
  rather than incorrectly asserting every such refusal means leader.
- Raft receive and tick **remain enabled**. Retirement is derived from the
  target membership and can precede completion of JOINT/COLD: an old voter
  still owes votes and AppendEntries, particularly with another voter down.
  Campaigns remain disabled by the existing retirement rule. Once removed,
  a replica is not guaranteed to receive COLD; a stale joint configuration
  alone does not license new application service or a local apply wait.
- A member without group 0 routes the whole MKFS operation to an original
  group-0 voter. Forwarding just its first entry cannot work: the operation
  next reads the committed membership from group 0 locally before SALT.
  This high-level route terminates at a group-0 host under alpha's fixed
  three-voter group-0 layout.

No format, wire, quorum, durability, compatibility or behavior-switch change.
No deployment-tool repository was edited.

## Sources, attempts and evidence

Base: `4066f74df5b997d70f263ae8dc1063b1a6049b92`.
Candidate source: `ae0262306` (product changes through `ffb645805`; the last
commit adds only the serialized qualification controller).

| Attempt | Result and scope |
|---|---|
| Untouched base, TCP, original focused sweep | **FAIL as expected:** retired-seed MKFS BUSY; other named seed operations and exact FUSE payload work. Retired `raft-resync` incorrectly returns success. |
| `a58c679b9`, focused sweep, TCP and RDMA | PASS for the original focused assertions; provisional, superseded source. |
| `d643adcd9`, expanded sweep, TCP and RDMA | Both exited 0. K33 growth passed in 92.3 s. Full gate still running at this checkpoint; it is not final-source qualification. |
| Corrected `ae0262306` | Queued after the earlier attempt, with no build/test overlap. Final results pending. |

Two harness defects in the provisional controller are retained, not hidden:
`tests/test_k33_acceptance.py` does not exist; acceptance was split into
seed-loss, interruption and payload tests. Also, a standalone sweep named
`tests/test_*.py` was not wired into the test inventory. The corrected
controller uses those actual acceptance tests; the sweep is now
`scripts/task_k_routing.py`. The persistent regression is an extension of
the already-wired K33 growth test, with its existing budget; no new long-test
command or larger gate target was added.

The provisional gate has reported `test_stop_progress.py` and
`test_posix_expiry.py` stop failures. They remain unclassified until the
corrected candidate and, if required, untouched base run under the same
host/command. These are not attributed to the routing fix or dismissed as
host conditions without that comparison.

## Reproduction and acceptance

The host is RHEL 9.8, 192 visible logical CPUs, private libfuse at
`/data1/efs/deps/libfuse`; the controller records kernel, CPU, memory, free
space, compiler, libfuse and verbs information in `environment.txt`.
Build concurrency is 8. Fragment I/O is O_DIRECT only. TCP uses loopback;
RDMA uses `mlx5_0` and `172.16.99.0`, without TCP fallback. Each focused
fixture has four private 1 GiB stores and a seed-only FUSE mount; endpoints
are selected from 19xxx. The added-member transition is also checked while
node 2 is down: old quorum {1,3}, new quorum {3,4}; node 2 is restarted
before the management sweep.

Export the named source with `git archive`, then on node9900:

```sh
cd /tmp/efs-taskK-20261026-final/candidate
bash scripts/task_k_after_attempt.sh ae0262306 \
  "$HOME/efs-artifacts/taskK-20261026" 172.16.99.0
```

That controller waits for the previous task-owned attempt and refuses any
leftover private daemon or mount before calling:

```sh
bash scripts/task_k_qualify.sh ae0262306 \
  "$HOME/efs-artifacts/taskK-20261026/qualification-ae0262306" 172.16.99.0
```

The committed driver runs the focused sweep once over each transport,
K33 growth/seed-loss/interrupt/payload/client-view/autoid/bookkeeping, the
fast `make test`, and `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8
scripts/make_test_summary.sh`. Any failed final gate triggers the untouched
base's same-host/same-command gate. The one injected base reproduction
script is moved outside its source tree first so it cannot pollute the base
test inventory. A nonzero outcome remains nonzero; no test is retried into
passing and no deadline is loosened.

The focused sweep covers MKFS at the seed and added member, Raft status,
step-down and resync refusal on the retired group, commands-policy get/set,
expiry-policy get, status, pack-compact summary, sync-status, data-commit,
I/O statistics, version, GC/expiry status, lookup/getattr/readdir/getchunks,
path lookup, expiry clock, and a same-voter-set configuration request.
Seed-only FUSE checks exercise create, fsync/fdatasync, exact reads, truncate,
xattrs, flock and byte locks, hardlinks/symlinks, statvfs, rename, unlink,
mkdir and rmdir. This is not a claim that every argument of every management
command, every low-level request subtype or every failure interleaving has
been exhaustively checked. The final command/result table is pending.

## Raw retention, cleanup and remaining work

Raw evidence stays on node9900 at
`/home/erbmi1/efs-artifacts/taskK-20261026/`, including `base/`, the provisional
`fixed/` sweeps, `qualification-d643adcd9/`, the dispatch log and final
`qualification-ae0262306/`. No raw capture or store is in Git or copied to
the Mac. Build identities and binary hashes are recorded by the controller.

**Open investigation:** bulk private stores/source trees are temporarily
retained under `/tmp/efs-taskK-20261026*` while the gates finish and failures
are classified. After review, retain closed logs/results, write and verify
`SHA256SUMS` and its own hash, prune inactive stores and source trees with
`edelete`, and record `PRUNED.txt`. Hash/pruning receipts are **not yet
available** at this checkpoint.

The multi-host orcd-mini stage is NOTRUN. It needs explicit approval through
the coordinator for that cluster's stop, fresh format and deployment; this
task does not infer that approval from permission for private node9900
fixtures. No fcstor003–013 host is used. `orcd-test.py init-export` should
propagate an unsuccessful format command rather than silently writing or
assuming an init marker; any needed change belongs to its owner in
`~/git/cluster`, not this branch.
