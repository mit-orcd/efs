# K44 / W167 — routing at a retired replica (October 26)

`agent/devel-task-k`, source `7029a6266`: final gate RUNNING; base `4066f74df` reproduces BUSY, expected FAIL. `docs/status/k44-retired-replica-routing-20261026.md`. orcd-mini NOTRUN (lifecycle approval). Defects: retired MKFS; retired resync; nonhost MKFS; notice NET after restart; stale CFG convergence. Core TCP/RDMA + joint PASS; full gate pending; old stop/reclaim findings OPEN.

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

The one-loss sweep also exposed a separate immediate NET outcome: the
restarted member was answering Raft/status, but the sender's peer pool still
returned BUSY during failed-connect backoff. The membership notice is
idempotent, so it now retries only transient BUSY/NET/IO/AGAIN using the same 5,000 ms value as the existing notice RPC I/O timeout, now
as one absolute budget covering all attempts, and any shorter caller deadline.
Identity and encoding refusals are never retried; no client write deadline
changes. Final one-loss evidence: seed MKFS succeeds in 0.556 s after 12 notice
attempts ending in rc=0; the added member's MKFS then succeeds in 0.004 s.

The command sweep found a second retired-view consumer in the CLI:
`raft-change` polled the seed's historical voter set for eight seconds and
then reported success even though that check never converged. It now
resolves the successful responder's leader hint, checks that live member's
configuration and reports `converged=1` only when observed; exhaustion
returns BUSY rather than false success. The sweep checks application-status
fields as well as process exits, since some diagnostic commands return
zero even on a rejected application request.

No format, wire, quorum, durability, compatibility or behavior-switch change.
No deployment-tool repository was edited.

## Sources, attempts and evidence

Base: `4066f74df5b997d70f263ae8dc1063b1a6049b92`.
Candidate source: `7029a6266` (product changes through `fcc8533d8`; `7029a6266` reorders assertions only. Includes the membership-notice
retry and live-responder convergence check).

| Attempt | Result and scope |
|---|---|
| Untouched base, TCP, original focused sweep | **FAIL as expected:** retired-seed MKFS BUSY; other named seed operations and exact FUSE payload work. Retired `raft-resync` incorrectly returns success. |
| `a58c679b9`, focused sweep, TCP and RDMA | PASS for the original focused assertions; provisional, superseded source. |
| `d643adcd9`, expanded sweep, TCP and RDMA | Both exited 0. K33 growth passed in 92.3 s. Full gate: `TOTAL 508 FAIL 10 NOTRUN 14 STATUS FAIL (614s) BUILD 0s`; it is not final-source qualification. |
| `ae0262306` dispatch | Canceled before any build/test after the provisional gate exposed missing unit mocks. |
| `7e4ddcb5e` | Both one-loss TCP/RDMA sweeps **FAIL** with NET: node 2's notice returns BUSY after its restart. All seven focused K33 acceptance tests PASS. Gate: `TOTAL 508 FAIL 10 NOTRUN 14 STATUS FAIL (574s) OVERBUDGET 1 BUILD 0s`; base comparison running. Its one-loss base control also failed with NET rather than BUSY and does not qualify the precise original signature. |
| `afc6d1466` and `6cd513dc7` dispatches | Canceled before any build/test as further observed issues were fixed; their cancellation records are retained. |
| `fcc8533d8` dispatch | Canceled before its negative control/build/test; `7029a6266` keeps the original BUSY assertion before the additional CLI-convergence assertion (all assertions remain). |
| Untouched-base full gate | `TOTAL 508 FAIL 5 NOTRUN 14 STATUS FAIL (764s) BUILD 11s`: four LSan fatal instrumentation errors and the expiry fixture's client-stop failure. |
| Final `7029a6266` | Healthy TCP, healthy RDMA and separate one-loss joint-transition sweeps **PASS**, 26 recorded management calls each plus exact FUSE checks. Final driver against the untouched base **FAILS as expected with `rc=-13` BUSY**. All seven focused K33 suites PASS. Fast suite FAILS at the same read-pair LSan instrumentation error as the base gate. Full gate running. |

Two harness defects in the provisional controller are retained, not hidden:
`tests/test_k33_acceptance.py` does not exist; acceptance was split into
seed-loss, interruption and payload tests. Also, a standalone sweep named
`tests/test_*.py` was not wired into the test inventory. The corrected
controller uses those actual acceptance tests; the sweep is now
`scripts/task_k_routing.py`. The persistent regression is an extension of
the already-wired K33 growth test, with its existing budget; no new long-test
command or larger gate target was added.

The provisional gates reported `test_stop_progress.py` and
`test_posix_expiry.py` stop failures, compile failures in extracted-helper
mocks, the stale resync diagnostic assertion, the unwired driver inventory
failure and four LeakSanitizer fatal instrumentation errors. The compile,
diagnostic and inventory defects have candidate corrections with added
behavioral assertions; they are not dismissed as environment failures. They remain unclassified until the
corrected candidate and, if required, untouched base run under the same
host/command. These are not attributed to the routing fix or dismissed as
host conditions without that comparison. The `7e4ddcb5e` gate additionally
failed `test_reclaim` at `accept_range` during PUT racing acceptance; the
base gate passed that command in 11 s. This test includes store/reclaim/
segment-store implementations and **stubs** the Raft host; those source files
are unchanged by this branch. The assertion did not print the actual
acceptance rc, so the race remains **OPEN/unlocalized**, not proved harmless
and not resolved by a later passing run. The old stop-progress failure also
remains distinct from the base's passing stop-progress result. Preserve both
in the final comparison.

## Final focused command table — October 27

Source `7029a6266`, build identity:
`efsd 7029a6266 (branch agent/devel-task-k, built 2026-10-27T04:00:30Z on node9900)`.
RDMA daemon logs confirm `segmented-v1 on mlx5_0`, rather than only the
presence of the device or a requested environment value.

| Check through seed unless named | TCP | RDMA | One-loss transition, TCP |
|---|---|---|---|
| MKFS through retired seed | rc=0, 0.004 s | rc=0, 0.004 s | rc=0, 0.556 s |
| MKFS through added member 4 | rc=0, 0.003 s | rc=0, 0.003 s | rc=0, 0.004 s |
| Commands-policy get and same-mask versioned set | PASS | PASS | PASS |
| Expiry-policy get; expiry-clock get | PASS | PASS | PASS |
| Raft/status, sync-status, data-commit, I/O/version/GC/expiry status | PASS | PASS | PASS |
| Pack-compact summary (no compaction requested) | PASS | PASS | PASS |
| Root/file getattr, lookup, path lookup, readdir, getchunks | PASS | PASS | PASS |
| Same-voter-set CFG convergence | observed, 0.007 s | observed, 0.009 s | observed, 0.006 s |
| Step-down and resync on retired group | NOT_PRIMARY, as required | same | same |
| Seed-only FUSE operations and exact payloads | PASS | PASS | PASS |

Each sweep has 26 recorded CLI calls: 24 successful calls and two expected
NOT_PRIMARY refusals. Lower-level CLI status fields are checked independently
of process exit status. All three sweep processes exit 0. Payloads are
1,048,832 bytes before truncate, then exactly the expected first 4,099 bytes;
hardlink, symlink and renamed-file reads also compare those exact bytes.

## Reproduction and acceptance

The host is RHEL 9.8, 192 visible logical CPUs, private libfuse at
`/data1/efs/deps/libfuse`; the controller records kernel, CPU, memory, free
space, compiler, libfuse and verbs information in `environment.txt`.
Build concurrency is 8. Fragment I/O is O_DIRECT only. TCP uses loopback;
RDMA uses `mlx5_0` and `172.16.99.0`, without TCP fallback. Each focused
fixture has four private 1 GiB stores and a seed-only FUSE mount; endpoints
are selected from 19xxx. A separate `--one-loss-transition` fixture checks addition while node 2
is down: old quorum {1,3}, new quorum {3,4}; node 2 is restarted before its
management sweep. The default reproduction keeps all four members up so
an unrelated restart transport failure cannot count as the BUSY signature.

Export the named source with `git archive` into a fresh private tree, then
on node9900 (this direct command needs no prior run or private capture):

```sh
cd /tmp/efs-taskK-repro/candidate
bash scripts/task_k_qualify.sh afc6d1466 \
  "$HOME/efs-artifacts/taskK-repro" 172.16.99.0
```

For the base negative control, archive `4066f74df` into another fresh tree,
build with the same libfuse and 8 jobs, copy only the committed
`scripts/task_k_routing.py` driver into its `scripts/`, and run it with
`EFS_TRANSPORT=tcp`, a private TMPDIR, `--port-min 19201` and a fresh `--out`
directory. Expect the final assertion to name `raft-mkfs rc=-13`; a different
error is retained as a different failure. The base product source remains
unchanged. Move the added measurement driver out before a base gate.

The actual detached run uses `scripts/task_k_after_attempt.sh afc6d1466
ARTIFACTS_PARENT 172.16.99.0 7e4ddcb5e`: this waits for the preceding run's
controller and its base comparison, checks both trees for leftover processes
and mounts, and then invokes the same direct qualification command. Its
per-run negative-control outputs have unique source-tagged paths.

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
`fixed/` sweeps, `qualification-d643adcd9/`, the source-tagged dispatch logs and
`qualification-7e4ddcb5e/` and final `qualification-7029a6266/`. No raw capture or store is in Git or copied to
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
