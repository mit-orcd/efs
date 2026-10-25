# K38: a packed read EIO after "Protection: full" during single-member outages (Oct 25)

**Verdict: PASS (scoped, loopback, amd), branch `claude/kind-nash-0f2244`,
not merged, not pushed.** `tests/test_k33_payload.py` **30 of 30 runs PASS** on the fix, alternating with the base, which **failed 8 of 30** with the same step-5 read EIO. The new
`tests/test_k38_protection_now.py` fails with either half of the fix
reverted and passes with the fix. `make test-all` on amd: final tree `f0b51c0ed` 480 commands, 14 FAIL: the 12 that also fail on untouched devel `5a7463440` (479 commands, 12 FAIL: 9 RDMA host limits, `test_create_retry_fault.py` timeout, `test_stop_progress.py` unmount, `test_sanitizer_gate.py`), `test_d4_progress.py` (packer `put_errors=1`; fails on devel too, 1 of 4 alternating runs) and `test_report_stepdown.py` (a 3 s latency bound at load 19; 3 of 3 standalone PASS on both trees).
Live multi-host verification is NOTRUN (parent session).

- **Purpose:** alpha 1.0 single-loss availability (every acknowledged write
  stays readable through the loss of any one member): the remaining
  `tests/test_k33_payload.py` step-5 failure after K34 and the flake-sweep
  client fix. K33 acceptance and K34 rows of the
  [status index](README.md); found intermittent on devel by
  W159 (`docs/status/w159-fixture-posix-timeouts-20261025.md` on branch `claude/devel-w159-fe09e8`, not merged) (1 of 10 on `ec3e09306`).
- **Base:** devel `5a7463440` plus the flake-sweep client fix
  (`fc79fe086`, `src/client/write.c` and its K35 test; an away-marked
  member is a spare of last resort) taken as `27cd98c09`, so this item
  chases what remains after it. That commit also adds the A6 row for
  `EFS_PUT_AWAY_MARK_MS`.
- **Tested source:** `f0b51c0ed` for the gate: `27cd98c09` (client fix
  taken), `42dd9d795` (the fix, hooks and test), `3e2e7093e` (test step
  selection, budget), `779691cc3` (two slice tests follow the taken client
  fix, from flake-sweep `36f2e3874`), `f0b51c0ed` (K36 test wait); this page
  follows. The payload A/B and the K38 controls ran on `3e2e7093e`; the
  later commits change tests only (`git diff 3e2e7093e f0b51c0ed`: three
  test files).
- **Builds:** `git archive` of each commit piped over ssh to amd
  (`~/k38/base`, `~/k38/src`, `~/k38/devel`), `make -j all
  tests/faults/efsd` (no `.git`, so build id `unknown`; identity by
  commit and sha256). Fix: `tests/faults/efsd` `f1797057…1e52`, `efsd`
  `be5ef786…b0d`, `efs-fuse` `633e402d…948f`; base: `tests/faults/efsd`
  `a616a856…f49ac1f7a`, `efsd` `1c729e1b…728f`, `efs-fuse` `8139e72a…3298`.
- **Host:** amd, 32 CPUs, one NVMe, Ubuntu (Python 3.14), RDMA auto on
  rxe0 falling back to TCP for every peer, buffered I/O; loopback daemons
  on ports the tests choose, stores and mounts under `~/k38/tmp`. No live
  cluster, mount, `efs` user resource or 17xxx/27xxx port was touched.
  amd was shared with other work: load average 1.7–3.2 during the
  reproduction, 9–40 from about 15:05 UTC (another session's daemons)
  through the A/B tail, the gates and the reruns.

## Cause

The payload test stops each of five members in turn and, before the next
stop, waits until `efs-mgmt status` says `Degraded chunks: 0` and
`Degraded segments: 0` (`wait_protected`). Every reproduced failure
(4 kept runs, 4 parallel loops on the base, 4 of 9 runs failed) was the same
read: a small, **packed** file, `efs-fuse read: network error
(efs_rc=-6)`, its segment on two fragments, one of them on the member just
stopped.

Traced run (`repro-base-2/run-1`, node 4 stopping; times UTC):

| Time | Where | Event |
| --- | --- | --- |
| 14:28:18.575 | node 3 | orderly stop done (step 5, k = 3) |
| 14:28:19.994 | node 3 | started again; listening 20.007, rejoined |
| ~14:28:20.29 | node 2 (group 0 leader, packer) | segment `e1cc110e061ab602` for the batch written while node 3 was down: fragment 0 to node 3 over node 2's peer pool fails NET, fragments 1 and 2 stored on nodes 4 and 5 |
| 14:28:20.344 | node 2 | `written degraded: fragment 0 for node 3 not acknowledged (rc=-6); marked for rebuild on return`; mark committed, files switched, `packed_total` +7 |
| 20.345–20.58 | test | `wait_packed` sees +7; `wait_protected` returns, so `efs-mgmt status` said `Degraded chunks: 0` and `Degraded segments: 0` (the pass had sampled before 20.344) |
| ~14:28:20.58 | node 4 | stopped (k = 4); shutdown done 20.663 |
| 14:28:22.483 | client | `fetch published ino=475137 ci=0 rc=-6 ... efs-fuse read: network error`; only node 5 holds a fragment; the rebuild never ran (no fragment 0 on node 3 at the end) |

Two defects line up:

1. **Status reported the last rebuild pass's sample.** `Degraded chunks`
   and `Degraded segments` in the STATUS reply were stored by the W154 and
   K37 rebuild passes of the GC loop (about once a second). A mark
   committed after the pass sampled was reported as 0, and `Protection:
   full`, until the next pass: the window the test (or an operator taking
   the next member down) acted in. This alone explains all four kept
   failures; in `repro-base-4/run-4` the segment was written degraded while
   its member was really down (14:33:09.109, node 2 back at 09.356, node 3
   stopped at 09.6) and the stale `0` let node 3 go.
2. **The packer wrote segments degraded around a member that was up.** In
   three of the four failures the "missing" member had returned 0.24–0.37 s
   before (`repro-base-1/run-2` node 3, `-2/run-1` node 3, `-3/run-2` node
   1). The peer pool keeps connections across a member's restart (a stale
   one fails only when used) and refuses a peer during its connect cooldown
   until a heartbeat answers; the packer took that NET for a missing
   fragment. Not the flake sweep's transient BUSY/NOT_PRIMARY/AGAIN
   refusal (that fix retries status codes; this is the transport).

Ruled out: no `I25` integrity refusal, no BUSY from a fragment GET, no
client down-mark on the reading path (`read_reprobe_down` found nothing to
probe), no chunk (unpacked) read failed, every stop orderly.

## Fix

- **STATUS** (`src/server/handler.c`, `src/server/segment_rebuild.c`):
  for the groups a member leads (the sampled bits, unchanged), the reply's
  degraded chunk count is the apply-kept W154 count
  (`efs_meta_degraded_count_peek`; may overshoot, never undershoot) and the
  degraded/unrecoverable segment marks are counted in the local replica at
  the reply (`server_segment_degraded_now`, a prefix scan of the marks).
  The packer's `server_segment_mark` returns only once the mark is in the
  leader's replica, so a status after a degraded write reports it.
- **Packer** (`src/server/packer.c`, `pk_put_fragment`): a fragment PUT
  that fails NET on the pooled connection, or gets none from the pool, is
  sent once more on a new private connection (`server_peer_conn_new`)
  before it counts as missing; `packer: segment fragment for node N sent on
  a new connection after the pooled one failed: rc=R`. A member that is
  down fails that connect too; the packer then writes degraded and backs
  off as before.
- **Test hooks** (`EFS_FAULTS` builds, A6 rows):
  `EFS_FAULT_REBUILD_SAMPLE_HOLD=<path>` stops both rebuild passes before
  they sample or rebuild while the file exists;
  `EFS_FAULT_SEGMENT_PUT_STALE=<dir>` fails the packer's next pooled
  connection to node N once (`<dir>/stale-N`, consumed).
- **Test:** `tests/test_k38_protection_now.py` (`make test-long`, budget
  90 s, 1 slot): three members; (1) stale pooled connection → no degraded
  segment, the resend line; (2) passes held, node N refuses segment PUTs,
  a batch packed degraded → every member's status at once `Degraded
  segments: ≥ 1`, `Protection: DEGRADED`, then rebuilt; (3) passes held,
  node N stopped, a 1 MiB file fsynced → `Degraded chunks: ≥ 1`,
  `Protection: DEGRADED`, then rebuilt and every file exact through a fresh
  mount. No wire, store or setting change; no new non-fault `EFS_*` reader.

## Results

| Check | Result |
| --- | --- |
| Reproduction, base `27cd98c09`, 4 loops in parallel (stop at first failure) | **4 of 9 runs FAIL**, each the step-5 packed read EIO above; daemon and client logs kept (`repro-base-*/run-*`) |
| `test_k38_protection_now.py` on the fix | PASS in 2 standalone runs (23 s; 51 s beside the A/B lanes) and in all three gates; the very first run stopped on the test's own regex (`Protection: DEGRADED:` read as `DEGRADED:`), fixed before the commit |
| Same test, retry disabled (`fresh < 1`), step 1 | **FAIL**: `segment written degraded around a live member` |
| Same test, STATUS block removed, step 2 | **FAIL**: `status right after a degraded segment write: (0, 0, 'full')` |
| Same test, STATUS block removed, step 3 | **FAIL**: `status right after a degraded chunk acknowledgement: (0, 0, 'full')` |
| `test_k33_payload.py` A/B: 3 lanes in parallel, each alternating fix and base, 10 pairs | **Fix 30 of 30 PASS** (24–175 s, median 120 s). **Base 22 of 30 PASS, 8 FAIL**, every one `OSError: [Errno 5]` in step 5 on a small packed file (`efs-fuse read: network error (efs_rc=-6)`) right after a `written degraded` segment. 14:41–15:19 UTC; another session's runs shared amd from 15:05. Higher than W159's 1 of 10 serial: three lanes in parallel load the disk and stretch the packer's PUTs |
| The packer resend in the fix's payload runs | 16 resends on a new connection in the fix's 30 runs, all `rc=0`; 41 segments still written degraded (the new connection failed too, a member stopping or stopped), each counted by status and rebuilt before the next stop (the test's `wait_protected`) |
| `test_k36_segment_recovery.py` step 7 (found by the `779691cc3` gate) | Fix 2 of 9 FAIL, devel 0 of 3: the test waited exactly 60 s for the rebuild pass's `UNRECOVERABLE` line, which the pass repeats at most once a minute per group; a mark that turned lost 50 ms after a reporting pass was first named 60.2 s later (kept run `k36ab/run-9`). On devel the status sample started that 60 s wait about a pass later and hid the race. Test defect, wait now 75 s (`f0b51c0ed`): 6 of 6 PASS and PASS in the final gate. The lost state itself is in status at once (`unrecoverable=`). |
| amd `make test-all` (`EFS_TEST_JOBS=8 EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh`), fix vs untouched devel `5a7463440` | **Fix `f0b51c0ed`: 480 commands, 14 FAIL, 6 NOTRUN (449 s).** **Devel `5a7463440`: 479 commands, 12 FAIL, 6 NOTRUN (476 s).** Shared: 9 RDMA on rxe0 (`test_declaration_gate`, `test_rdma_xprt`, 7 × `test_rdma_segmented`), `test_create_retry_fault.py` (30 s timeout under the host's load), `test_stop_progress.py` (FUSE unmount refused; gvfs on devel), `test_sanitizer_gate.py` (leak self-test sees no report on this host). Fix only: `test_d4_progress.py` `put_errors=1` — devel fails it too in alternating standalone runs (fix 2 of 4, devel 1 of 4; the flake sweep's transient-refusal retry, not on this branch); `test_report_stepdown.py` write max 6.1 s > 3 s at load average 19 — standalone 3 of 3 PASS on each tree (max 1.5–2.5 s), the flake sweep's known 1-in-10. `test_k33_payload.py` PASS (145 s), `test_k38_protection_now.py` PASS (52 s), `test_k36_segment_recovery.py` PASS. Earlier gates on this branch: `3e2e7093e` 15 FAIL (the two slice harnesses that did not know the taken client fix's `put_away_mark_ms`/`node_note_back`, fixed in `779691cc3`, and D4); `779691cc3` 13 FAIL (D4 and `test_k36_segment_recovery.py`: below). amd was shared with another session's daemons (load average 15–40) during every gate. |

## Limitations and remaining gates

- Loopback, one host, TCP: regression evidence, not qualification. Live
  multi-host single-member outages under load stay NOTRUN (parent session).
- The STATUS count covers what the metadata knows. A stripe this client
  acknowledged degraded but has not yet published (writes without fsync;
  the REPORT follows) is not in any count; `fsync` proves it (K35). Not
  seen in any run here.
- A member that is really down still gets segments written degraded (by
  design, K37); protection returns only after its rebuild, and status now
  says so at once.
- The flake-sweep packer retry (`fc79fe086`, transient status codes) is not
  on this branch; it edits the same block of `pk_segment` and merges beside
  this change (different function for the transport retry).

## Reproduce

On a Linux host with `/dev/fuse` and `fusermount3`, in a private tree:

```
make -j all tests/faults/efsd
python3 tests/test_k38_protection_now.py
EFS_K38_STEPS=2 python3 tests/test_k38_protection_now.py   # one step
EFS_K33_KEEP=1 EFS_READ_FRAG_PROF=1 python3 tests/test_k33_payload.py
EFS_TEST_JOBS=8 EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh
```

`scripts/k38_payload_ab.sh FIX_TREE BASE_TREE OUTDIR 10` (three in parallel)
runs each tree's `tests/test_k33_payload.py` alternately,
keeps the work directory (`EFS_K33_KEEP=1`) of every failed run and deletes
the others; a side loop copied every `written degraded` and
`sent on a new connection` line from the live work directories. To find
the cause in a kept run: the client's `efs-fuse read: network error` line
names the inode; the packer's `written degraded` line names the segment
and the member; `find n*/data/segments -name '<segment id>*'` shows which
members hold fragments; the `==== start` and `shutdown: outcome` lines
give each member's stop and return.

Raw (outside Git): amd `~/efs-artifacts/k38-20261025/` — builds, the four
kept base failures (`repro-base-*`), the A/B lanes (`ab-lane*`, verdicts,
kept failures), `ab-events.txt`, the K38 test runs and controls
(`k38test-*`), and the gates (`gate-*`); `SHA256SUMS` (11,705 files, 824 MB) sha256
`4df49442…ae9b`. Private trees under `~/k38` deleted after the run.
