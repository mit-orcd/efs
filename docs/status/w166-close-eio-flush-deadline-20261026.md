# W166 — option B implementation and xorinox qualification

Report: `agent/devel-task-g`, based on devel `394f2b791`; not merged or pushed.
**DECIDED; the two reproduced responsive-node pressure paths are implemented
and qualified on xorinox.** Product changes: `878c00e96`, `a72adf70a`.
Final tested source: `4bcd52ca0`. Original full-process SIGSTOP / production
snapshot-window acceptance and live RDMA/multi-host gates remain **NOTRUN**;
this does not close every scenario in the original W166 item.

Original qualification full-gate result, preserved verbatim:
`TOTAL 510 FAIL 1 NOTRUN 13 STATUS FAIL (833s) OVERBUDGET 1 BUILD 11s`.
The sole failure is `test_k38_shared_chunk_pressure.py`, also failing in the
unchanged-base full gate. **No new full-gate failures; no unconditional suite
PASS.** That result remains a historical failure. The subsequent
[K38 continuation](k38-task-g-repair-20261027.md) reproduces it with complete
captures, ports reviewed publication/pressure repairs and finishes at **512
commands, 0 FAIL, 13 NOTRUN / INCOMPLETE** (842 s + 24 s build). A pre-fault
W166 transfer BUSY in its first repaired run is retained and corrected with
bounded setup retries. Both final W166 cases pass and read back exactly
through a fresh mount. No additional W166 product change or deadline
extension was needed; original SIGSTOP/RDMA/multi-host acceptance is still open.

## Decision and implementation

User: **“yes, implement option B in your branch, keep the 30 second deadline,
fix both failure paths and finish the qualification on xorinox.”**

The decision memo recommended B because a healthy quorum already held the
needed authority while a storage member's KV prefix was parked. Extending a
wait cannot help that replica make progress. Alternative A, progress-bounded
renewal, would need operation-relevant progress evidence, bounded retained
memory and coordinated PUT/REPORT/receipt budgets; it was not implemented.

- **PUT authority:** a hosted follower delegates the **complete** ticket
  STATUS request to the lane leader. The receiver performs fresh owner/session
  and registered-participant checks, the lane read proof, session floor and
  exact immutable-ticket lookup against the KV it actually reads. A remotely
  obtained ReadIndex is never permission to inspect stale local KV. STATUS
  uses the bounded read pool/budget, not the mutation pool. Forwarded wrong
  targets, including unhosted members, answer NOT_PRIMARY without another hop.
  The storage member's guarded durable session fence still precedes its write.
- **REPORT:** a full L0 alone no longer refuses publication while the bounded
  memtable/WAL tail still has room. The old REPORT-only check caused repeated
  BUSY before the actual capacity gate fired, exhausting 8 s REPORT + 8 s
  resolution. Leader-only KV-memory/unapplied-log admission, apply-side bounds,
  receipt identities and unknown-outcome semantics remain enforced.

The **30,000 ms outer flush deadline and 8 s inner budgets are unchanged**.
No new behavior selector, BUSY-to-down classification, quota-to-degraded
conversion, optimistic receipt inference or weakened protection was added.
The fault hook is compiled only under `EFS_FAULTS` and inventoried in A6.

## Environment and results

Xorinox / user `maik`, 16 logical CPUs, Linux
`6.12.0-211.67.1.el10_2.x86_64`, XFS home filesystem, GCC and system libfuse
3.16.2. Private three-member loopback clusters, free reserved ports, TCP,
O_DIRECT, two storage writers/member, product background maintenance defaults.
Only node 1 has compressed KV limits and a parked compactor during pressure.
No production cluster, `efs` user's mount, protected port or `/data1/efs` store
was operated on. Build logs, compiler/hardware inventory and binary hashes
are retained; the final daemon identifies as `4bcd52ca0` on this branch.

| Experiment | Actual outcome |
| --- | --- |
| Unchanged `394f2b791`, 300 s pressure, 1 GiB/member; driver `4a0d7460f` | **FAIL**, 36 close EIOs at 29,999.56–30,001.29 ms; PUT consumed the budget. All 331 confirmed-close files exact on the same mount. |
| Separate forensic run on the same product, 120 s; driver `7cdbe0f55` | **FAIL**, eight 30 s PUT failures and four 8+8 s REPORT/resolution failures. All 228 confirmed-close files exact on the same mount. Tracing is diagnostic, not a timing comparison. |
| Instrumentation-only baseline `ddff5fcfd`, original 2 MiB deterministic fixture | **FAIL both cases:** REPORT 16.061 s; forced follower STATUS 6.505 s. No product fix in this baseline. |
| Fix `878c00e96`, same 2 MiB fixture | **PASS:** REPORT 9.7 ms, PUT 12.7 ms; both exact through a fresh client. Wall 31.09 s. |
| Fixed `430cd676c`, original 1 GiB/member, 300 s | **FAIL: quota exhaustion**, not a W166 EIO reproduction: 2,346 close ENOSPC + 2,269 write ENOSPC, zero EIO. All 21,996 acknowledged payloads exact on initial same-mount readback. Wall 490.36 s. Failures are retained, never promoted to success. |
| First 8 GiB attempt, `430cd676c` | **HARNESS FAILURE before workload:** node 3 won formation; the driver assumed node 1 would lead. Retained separately. |
| Corrected setup `f00c91c41`, 8 GiB/member, 300 s | **PASS scoped pressure replay:** zero syscall errors; all 24,018 acknowledged payloads exact on initial same-mount readback. Wall 508.90 s. Close p50/p99/max 21.68 / 191.64 / 2,871.57 ms. |
| Five standalone K22 runs, `f00c91c41` | **PASS 5/5**, 3,985 / 4,334 / 4,750 / 4,470 / 3,992 successful writes; zero early or late client errors. Host refusal and the 3/4 backlog watermark observed, leadership moves, then each replica drains after release. These tests do not read every payload. |
| Final 1 MiB deterministic fixture, baseline product `ddff5fcfd`, driver/helper `4bcd52ca0` | **FAIL both cases:** REPORT 16.008 s, PUT 6.507 s. The immediate STATUS fault reaches the 128-attempt cap sooner than the real 400 ms proof waits in the original 30 s captures. |
| Final `4bcd52ca0`, same 1 MiB fixture | **PASS:** REPORT 9.98 ms, PUT 11.08 ms; both exact through a fresh client. Wall 20.35 s alone, 28.8 s / rc 0 in the full pool. |
| Retained 1 GiB fixture, same-build orderly process restart + new FUSE client | **PASS:** all **21,996 / 21,996** acknowledged payloads exact, zero read errors; ready in 1.64 s; wall 189.62 s. |
| Retained 8 GiB fixture, same-build orderly process restart + new FUSE client | **PASS:** all **24,018 / 24,018** acknowledged payloads exact, zero read errors; ready in 1.64 s; wall 417.42 s. |

The 8 GiB replay is a **declared fixture change**: the repaired path made enough
progress to exhaust the original 1 GiB quota. Memory limits, concurrency,
300 s pressure and deadlines were unchanged. The pressure replay keeps the
original 2 MiB L0 / 1 MiB memtable / 4 MiB KV budget. The dedicated regression
uses a 1 MiB L0 cap with the same 4 MiB KV budget to reach the same full-L0 /
remaining-memory condition faster. Four 2 KiB writers and four 16-worker
metadata-load processes drive the pressure replay.

All **46,014** acknowledged pressure payloads were checked byte-for-byte after
orderly full-process restarts through new clients, eliminating the writer's
cache as their readback source. Verifier `71d856de6` checks matching binary
identity, existing stores, original quota/endpoints, readiness, every recorded
successful close, unmount and every daemon's exit. It neither retries failed
reads nor counts failed writes. These are **not power-loss or OS-reboot proofs**;
no claim is made that every failed/ambiguous write was absent or durable.

## Full gates and harness corrections

| Full `make test-all`, 8 jobs | Preserved result |
| --- | --- |
| First branch `ec7c639d7` | `TOTAL 509 FAIL 2 NOTRUN 13 STATUS FAIL (803s) OVERBUDGET 2 BUILD 23s`: pre-fault leadership-transfer BUSY and reclaim test cleanup. W166 regression itself passed. |
| Unchanged base `394f2b791` | `TOTAL 508 FAIL 2 NOTRUN 13 STATUS FAIL (862s) BUILD 23s`: shared-chunk pressure EIO and the identical reclaim cleanup assertion. |
| Final branch `4bcd52ca0` | `TOTAL 510 FAIL 1 NOTRUN 13 STATUS FAIL (833s) OVERBUDGET 1 BUILD 11s`: only the same shared-chunk pressure test. K22 rc 0 / 213.3 s, W166 rc 0 / 28.8 s, reclaim rc 0 / 15.0 s. |

Setup now explicitly transfers leadership before arming faults instead of
assuming the first election's winner. It retries only documented pre-fault
catch-up BUSY in the existing transfer window; hard errors fail. The unit gate
checks eventual catch-up, permanent BUSY expiry and immediate hard-error
refusal. K22 failures retain their fixture automatically; five standalone
runs explicitly used `--keep`.

The reclaim failure was a **test path-length bug**. Its 256-byte cleanup
command was 257 bytes at the first branch's nested TMPDIR, truncating the
state-file name and leaving state behind after deleting its journal directory.
The unchanged-base full pool reproduced the same `accept_range` assertion;
39 short-path standalone/parallel controls passed. The test now sizes the
buffer for all three paths and checks every formatting result; no reclaim
product code or assertion was weakened. The longer-path standalone control
and the final pool pass.

The final pool has one unrelated OVERBUDGET: geometry grow-a 122.1 s against
120 s. W166's row is **40 s, one slot**: 28.8 s × 1.25, rounded up to 5 s.
The D44 target stays 1,100 s; the budgeted gate stays below its existing bound.
No deadline was increased to manufacture acceptance.

**Shared-chunk acceptance remains OPEN.** Both base and final pool got EIO
in `test_k38_shared_chunk_pressure.py` (one client's writers, REPORT/BUSY
visible in captured tails). The first branch pool passed that case. This does
not establish recurrence of K40's old PREPARE-abort bug, nor prove a new cause;
no K38/K40 fix or closure is claimed. That test auto-cleaned its failed stores,
so only its captured failure output/client tails remain, not a full replayable
store capture. The 13 NOTRUNs cover unavailable RDMA hardware, cluster devops
scripts, sanitizer/Valgrind runtimes and the normal-build follower-bound hook.

## Original ladder and reproduction

The original pressured member had **already stepped down** before the first
30 s close failure: 1,497 unapplied entries versus its 12,288 admission
watermark, while healthy voters applied. The forensic ticket for inode 237763,
sequence 229, object `0x759a0134f4ab0209` sent fragments 0/1 once each to healthy
nodes and fragment 2 **66 times** to node 1. The node refused each STATUS with
NOT_PRIMARY; the data reply became BUSY and discarded the hint. Median resend
gap was 452.55 ms. CLOSE-PROF: PUT 30,001 ms, REPORT 0 ms. The separate inode
122898 had PUT 379 ms, REPORT/resolution 16,000 ms, unknown receipt outcome.
Absent receipts are not abort proofs. The client's old `published degraded
for rebuild` trace was local bookkeeping before publication, not durability
or successful publication evidence.

From a clean private `git archive` on xorinox, build the chosen revision:

```sh
make -j8 EFS_GIT_ID=4bcd52ca0 EFS_GIT_BRANCH=agent/devel-task-g \
  EFS_GIT_DESCRIBE=4bcd52ca0 all tests/faults/efsd
python3 -u tests/test_w166_authority.py
# Original pressure: --quota 1G; declared extended-capacity fixture: 8G.
python3 -u scripts/w166_flush_diagnostic.py --work "$HOME/PRIVATE/pressure" \
  --seconds 300 --quota 8G --source-revision 4bcd52ca0
for n in 1 2 3 4 5; do python3 -u tests/test_k22_leader_admission.py --keep; done
EFS_GIT_ID=4bcd52ca0 EFS_GIT_BRANCH=agent/devel-task-g \
  EFS_GIT_DESCRIBE=4bcd52ca0 EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 \
  EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh
# Verifier from 71d856de6; SOURCE is that fixture's matching binary tree.
python3 -u scripts/w166_verify_retained.py --run "$HOME/PRIVATE/pressure" \
  --source "$HOME/PRIVATE/source" --output "$HOME/PRIVATE/cold-read" --quota 8G
```

Set TMPDIR beneath the private home-directory run root. Runs are sequential;
retain each output under a unique name. Diagnostic rc 1 records a failed
workload, rc 2 means no failure reproduced, rc 3 is harness failure; inspect
errno and result JSON, never treat ENOSPC as the W166 EIO signature. For the
baseline deterministic test, use binaries from `ddff5fcfd` (only fault
instrumentation on original product code) with the final committed driver and
`k33_daemons.py`. Both original and final fixture versions fail there. The
original forensic driver revisions and optional strace method remain in Git
and in the original evidence; their inode/ticket values are run-specific.

## Retention and limits

**New evidence:** `/home/maik/efs-artifacts/taskG-20261026/implementation/`:
306 manifest entries, about 111.4 MB. Closed driver/client/daemon logs, syscall
outcomes, status snapshots, all full-gate summaries/timings/commands/failure
outputs, restart results, exact driver copies, compiler/hardware inventory and
binary hashes remain on xorinox. Manifest `SHA256SUMS` SHA-256:
**db322266b71f14d9e953f2b0572d7045b06206e3651535c734898273984150c0**.
Every copied file was checked against its closed source; all manifest entries
verified. `COPY_MANIFEST.json`, `pre-prune.json` and `manifest-verify.log` record
those checks. Verification output is excluded from the manifest itself.

**Original evidence unchanged:** parent directory's 61-file manifest SHA-256
**90aa33419088b2c500630233e494524ce1eda16274f08d3a00e68627afd76a43**.
It covers the original captures; the new subdirectory has its own manifest.

`PRUNED.txt` and `edelete.log` record removal of owned private source/build,
Raft/KV/fragment and generated-payload bulk with `~/bin/edelete --force --delete`
after checked child shutdown, mount absence, completed restart verification
and hash-verified copying (110,889 files / 134,764 directories, zero errors).
No Task G workload or mount remains. Recreate synthetic stores from the
recorded source/method. The first failed K22 full-pool setup also auto-cleaned
its pre-fault fixture; its management error is captured and the correction is
independently covered by a deterministic unit test.

**Remaining:** original full-process SIGSTOP / production-window acceptance,
live RDMA and multi-host gates, and the separately open shared-chunk full-pool
failure. No sampled payloads, missing captures or quiet rerun are counted as
passing those gates.
