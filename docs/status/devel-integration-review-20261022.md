# Recent devel integration review — October 22, 2026

**Current verdict: OPEN; second-pass review through `c71cd1066` below adds
K22 election liveness and K5 fencing findings. Not release qualification.**

First-pass scope: reviewed
`2925c92b0..e9ca0d07b` (46 commits including merges, 125 changed files), plus
status-only `95beaa053`. This starts after the preceding packing review.
Inventoried the complete range; detailed source review concentrated on changed
production paths, test wiring and their integration. This is not a claim that
every test fixture or every possible concurrency interleaving was verified.

No deployment, formatting or user workload was changed. Local checks ran on
macOS with Apple clang and Python 3.9; there was no mounted EFS, transport,
cluster topology or quota. The source tree has no built `libefs.a`.

## Findings

### R1 — P1: test-summary process reports success after failing tests

`scripts/make_test_summary.sh:25–48` accumulates failures and NOTRUN results,
but its Python runner never exits nonzero for them. The shell therefore returns
0 after a failed test campaign. This defect predates the range and remains in
the reporting code modified for W139/K25; the new NOTRUN reporting does not fix
exit-status consumers. It can make an automation accept a failed qualification.

**Reproduced:** executing the actual embedded Python runner with commands
`exit 7` and `printf 'SKIP injected prerequisite\n'` produced
`TOTAL 2 FAIL 1 NOTRUN 1` and process status **0**. The reproduction used only a
temporary directory, no build or cluster. Reproduce from the repo root:

```python
from pathlib import Path
import subprocess, tempfile
source = Path("scripts/make_test_summary.sh").read_text()
runner = source.split('python3 - <<"EOP"\n', 1)[1].rsplit("\nEOP", 1)[0]
with tempfile.TemporaryDirectory() as directory:
    Path(directory, "cmds.txt").write_text("exit 7\nprintf 'SKIP injected prerequisite\\n'\n")
    result = subprocess.run(["python3", "-c", runner], cwd=directory)
    print("runner exit:", result.returncode)
    print(Path(directory, "summary.txt").read_text())
```

Return a failing status when any command fails. Define a distinct incomplete
outcome for required NOTRUN gates so callers cannot equate exit 0 with full
qualification. Add failure, timeout, missing-prerequisite and all-pass tests of
the runner itself. Preserve per-command output rather than only its last line
when diagnosing a failure. Do not introduce a product behavior toggle.

### R2 — P2: transaction decision sweep can starve later records

Introduced by `7c237d5c2` (K7/W83). `efs_txn_scan_decisions` in
`src/meta/txn.c:1266` always starts at the prefix beginning, returns at most
`cap` entries and exposes no continuation. `host_txn_decision_sweep` in
`src/server/raft_host.c:9135` requests 32 entries. If those first 32 cannot
resolve (for example their participants are unavailable), they remain live;
every pass visits the same page. Later independently resolvable decisions in
that coordinator shard never reach retirement. Persisting the participant set
is the right direction, but bounded work must also guarantee eventual coverage.

Use a per-shard continuation with wraparound and a physical scan-work budget;
failed entries remain durable and are retried on later rounds. The tombstone
reclaim cursor is separate and does not solve this live-decision scan. Gate:
create more than 32 live decisions, keep the first page unresolved, and prove
later resolvable entries retire without deleting the blocked decisions. Repeat
across orderly restart/leadership change. **Source-confirmed control-flow gap;
cluster reproduction NOTRUN.**

### R3 — P2: new transaction encodings accept unrecognized trailing bytes

In `src/server/raft_host.c:2737–2758`, the extended DECIDE checks
`clen < 23 + 4*n`, and new TXN_RETIRE checks `clen < 29`. Both accept extra
bytes and execute as a valid current-format command. The W140 tag allow-list
cannot establish payload validity. A valid RETIRE plus an extra byte still
reclaims records and advances apply, instead of refusing an encoding this build
does not understand. This contradicts the current-format fail-closed policy.

Require exact lengths for these encodings before mutation; validate participant
shards and decision values as structural fields as well. Keep legitimate
transaction conflicts distinct from malformed encoding. Gate valid records,
all truncated lengths, appended bytes and invalid fields through the real host
apply path; malformed entries must leave rows/cursor unchanged and stop the
group through its existing unsupported-entry mechanism. **Source finding;
new malformed-frame execution tests NOTRUN.** No compatibility path is wanted.

### R4 — P2: handler drain is bounded, whole daemon shutdown is not established

`781df5750` bounds `shutdown_drain`, but after it succeeds,
`src/server/efsd.c:741–751` still stops the packer, writer pool, PUT sync,
reclaim, Raft and peer pool. For example `server_packer_stop` performs an
unbounded `pthread_join` (`src/server/packer.c:1371`). Those operations are
outside the 40-second handler deadline. The test extracts the handler drain;
it cannot establish the advertised whole-process stop bound.

Also, `scripts/server_processes.py:retire` checks process disappearance via
pidfd, not the daemon exit result. The new `_exit(3)` path can therefore look
like a successful stop to this control script. Process termination and orderly
shutdown must be separate qualification outcomes. This is a reporting/integration
gap, not evidence that an acknowledged payload was lost.

Carry a total shutdown deadline across phases and record whether all durability
barriers completed. Have the launcher/status evidence distinguish orderly exit
from forced exit without teardown; do not silently call the latter an orderly
restart PASS. Test a stuck background worker as well as a stuck connection,
and preserve stores on an incomplete shutdown. **Handler test PASS locally;
whole-process stuck-worker and forced-exit reporting gates NOTRUN.**

## Assessment of the other changes

| Area | Assessment and remaining acceptance |
| --- | --- |
| K1 Raft probe ACK/conflict hints/pre-vote | Keeping next_index monotonic on successful probes and pre-voting before increasing terms address the documented mechanisms. No additional concrete safety defect established in this review. The existing private-cluster evidence is not merged RDMA restart qualification. Header pre-vote comments should match implementation: prospective term is in prev_term, and a higher-term refusal can advance the receiver's term. |
| K2/W140 proposal tag allow-list | Correct shared proposal/apply namespace direction; `b29666695` correctly adds TXN_RETIRE after integration. It does not validate payloads (R3). |
| K3 retired NODE_LEFT and K4 protocol declaration | Removing unauthoritative membership mutation and requiring exact protocol 3 before RDMA upgrade/dispatch are appropriate. A protocol declaration is not an exact build-ID check or authentication; retain that distinction. |
| K7 transaction retirement | Replicated retirement and retaining verdicts through participant resolution are appropriate. R2/R3 remain; the unit test's library-level command sequence is not a full host/snapshot/leadership test. |
| K16 fragment identity records | Binding identity and payload digest improves I25, and rejecting unsupported root formats matches fresh-format policy. The documented lack of inode-generation/export-salt binding remains a limitation, not something this review closes. Private TCP buffered/direct evidence is scoped correctly; merged multi-host RDMA qualification remains open. |
| K24 fault-hook removal | Compiling compaction termination out of normal builds fits alpha's single production behavior. Dedicated fault builds retain the test. |
| K25 test wiring, TSan and W150 abandon scan | Added wiring and work-budget tests are useful; R1 remains. TSan availability failures must remain visible. The earlier packing mismatch/harness review is not closed by these unrelated fixes. |
| Alpha roadmap/status changes | Keep implementation, scoped acceptance and tag readiness separate. Fresh-format cutover does not authorize erasing a specific active campaign. No extra A/B production modes or compatibility work is requested. |

## Checks actually run

- `python3 tests/test_shutdown_bounded.py`: **PASS**, extracted handler gate and
  drain behavior only (about 51 ms cooperative drain; 317 ms for 300 ms refusal).
- `python3 tests/test_declaration_gate_handler.py`: **PASS**, source integration
  check, not a live socket/cluster qualification.
- `python3 tests/test_make_test_recipe.py`: **PASS**, reports 960 commands,
  63 unit binaries, 322 Python tests and one explicitly unwired test. These
  numbers describe wiring, not tests executed in this review.
- `python3 tests/test_submit_unknown_tag.py`: predicate check **PASS**, then
  **BLOCKED/NOTRUN** for its linked-log portion: local `libefs.a` is absent.
  The command exited nonzero; it is not an overall PASS or a product failure.
- Embedded summary-runner reproduction: **FAIL confirmed**, as in R1.
- Full Linux suites, merged RDMA/direct-I/O tests, restart/payload verification:
  **NOTRUN**. Earlier per-branch results remain their authors' scoped evidence.

Fix R1 before using the summary runner as an automated acceptance verdict;
address R2/R3 and qualify R4 before closing their alpha work items. No product
code is changed by this review.


## Second pass — devel through `c71cd1066`

Requested re-review covers the 39 additional commits in
`16083549b..c71cd1066`, including K5 session enforcement, K8 partial-write
base handling, K9 append oracle, K15 transaction-finisher handoff and K22
memory/admission bounds. Rechecked the earlier findings against this tree.
Source review is concentrated on production behavior and acceptance harnesses;
it is not exhaustive proof of all concurrent execution paths.

### S1 — P1: log-lag election veto can prevent recovery with free capacity

`src/raft/raft.c:low_capacity`, `capacity_hold`, `start_prevote` and
`start_election` (K22, `1661d5439`/`2e1ea8dea`) use `last_index - last_applied`
at 75% of the cap as an unconditional campaigning veto. That quantity includes
entries not yet known committed. After losing the leader or restarting with a
retained tail, applying those entries requires a leader to establish commitment.
If every eligible voter has such a tail, no voter sends a pre-vote, so nothing
can change the condition. Clearing KV pressure does not unblock it.

**Reproduced against production Raft and memory-store code:** three voters,
identical term-1 logs with 96 entries, applied/commit position 0, cap 128, no
memory pressure, 1,000 ticks each. All remain followers, with 70/69/67 held
campaigns and **zero messages sent**. The send callback merely counts: there
is nothing to deliver, so network loss is not the cause. With only the cap
removed as a diagnostic control, they send 412 messages (delivery is not
modeled and this control does not claim election completion).

Reproducer: `scripts/reviews/k22_capacity_tail.c`, with compile/run commands
in its header. It uses only memory stores and `/tmp/k22-tail`; no deployment,
flags or cluster data are changed. This demonstrates the state-machine veto;
a host-level orderly-restart reproduction remains NOTRUN. The small cap scales
the same threshold used by the host; it is not a proposed product setting.

Preserve memory and admission limits, but distinguish inability to apply from
an uncommitted tail that needs an election. Allow recovery leadership with
bounded control-entry headroom and client admission held until safe. Do not
apply uncommitted entries or weaken quorum rules. Add a three-voter test that
actually delivers messages, elects a leader from this state, commits/applies
the retained tail and resumes admission. The current capacity test only clears
an explicit pressure bit; it misses this self-sustaining log-lag condition.

### S2 — P1: mtime ownership can capture an already-fenced session row

`src/meta/append_plan.c:82–100`, new `efs_meta_execute_mtime_owned` (K5),
checks the session with `efs_session_accept`, then separately reads the local
session row into `c.owner.expected`. A fence can land between the reads. The
second row is checked only for length, copied unchanged, and used as the EXCL
comparison image. Consequently the CAS can succeed against the **fenced** row
and allow the old epoch's mtime transaction. A CAS protects changes after the
capture, not authorization of the captured bytes. `host_regular_mtime` does
not hold a lock across these reads and transaction preparation.

**Wrapper counterexample reproduced:** the first acceptance returns OK for
epoch 1; the second read supplies established=1/reject_below=2. The production
wrapper returns OK and calls the executor/prepare callbacks with that fenced
image. `scripts/reviews/k05_mtime_fence_capture.py` retains the reproduction.
It extracts the production ownership wrapper, injects the two-read interleaving,
and spies on the executor callback; it is not a full transaction/cluster test.

Validate established epoch and rejection floor from the exact captured image
before preparing it, as `efs_meta_execute_append_owned` already does. Retain
the EXCL guard for fences after capture. Add a barrier-controlled test with
real session rows: fence between accept and capture must refuse without inode
or lane changes; fence after capture must conflict or wait for the established
transaction order. Full transaction execution of this race is NOTRUN here.

### S3 — P2: session latch mixes atomic reads with plain concurrent writes

K5's `src/client/inode_rpc.c:efs_client_session_fence` writes
`g_client.session_fenced` with a plain assignment under `opid_mu`, while
`efs_client_session_fenced` reads it using `__atomic_load_n` without that mutex.
The acquire read does not synchronize with a plain write. `session_started`
has the same mixed-access pattern in session start/check. The fenced latch can
be written by one RPC thread while another checks it; this is a C data race,
not a safe publication scheme merely because integer stores are naturally
aligned. Server fencing remains necessary independently of the local latch.

Use consistent atomic accesses (including reads under the mutex), or use the
same mutex for every access. Use an atomic exchange if logging only the first
fence. Add a concurrent check/fence test under TSan. **Source-confirmed access
pattern; sanitizer reproduction NOTRUN in this review.**

### Earlier findings and packing harness recheck

| Earlier issue | Current disposition |
| --- | --- |
| R1 summary exits 0 with failing tests | **OPEN.** `scripts/make_test_summary.sh` unchanged since the first review; earlier reproduction remains applicable. |
| R2 live decision scan starvation | **OPEN.** `src/meta/txn.c` scan and host sweep still restart at the prefix beginning with a 32-record page. |
| R3 DECIDE/TXN_RETIRE trailing bytes | **OPEN.** Their new decoder still uses minimum rather than exact lengths. |
| R4 whole shutdown versus handler drain | **OPEN.** Daemon shutdown implementation unchanged; background stops remain outside the handler bound. |
| W125 output reuse / mismatch context | **Improved by `23354597c`.** Existing OUT is refused; local mismatches gain expected version, lengths, hashes, inode and management output. Credit these fixes rather than repeating the old findings unchanged. |
| W125 cleanup and remote/final evidence | **Addressed on `devel-w125h` (`6228f019c`..`8df4de684`, from devel `1bf4bd45f`; merged `56b7e48fe`).** `w125_p0_cluster.sh stop` now verifies the unmount and every owned daemon's orderly exit (status recorded by a `run-daemon` wrapper), KILLs only after `W125_STOP_WAIT` and reports it as `stopped forced` with rc 1; the wrapper keeps the store after any forced or unverified stop, says so in `done`, and records every cleanup status (exit 3 forced stop, 4 undeletable store). Remote reads still accept any version by design (a peer may have modified the file), but the first non-matching bytes are saved with the full sidecar and the sidecar says whether and when the read settled; a read that never settles is a mismatch (`unsettled`), never a transient. Final-verification mismatches are captured the same way. The hard link is replaced by a copy taken under the per-file lock by exclusive create. `tests/test_w125_harness.py` checks each path with stubs; see [the validate-once page](review-pack-validate-once-20261022.md#harness-gaps-closed-on-devel-w125h--october-22). Cluster reruns remain NOTRUN. |

### Assessment and checks

K8's change to reset a partial load's base generation is appropriate: an old
UNCOND flag must not turn a partial update into unconditional replacement.
The targeted unit checks both loaded-base and no-base cases. The original
fixed-build modify campaign is still NOTRUN; do not translate the code fix into
closure of every historical mismatch.

K9's exact acknowledged-record oracle is useful and its INCOMPLETE verdict is
honest. Allowing ambiguous failed appends in a diagnostic oracle is distinct
from error-free acceptance; use its strict-error mode for normal-operation
qualification. K15's explicit abort handoff preserves an already-decided COMMIT;
the extracted test checks that distinction. K5's removal of unregistered
mutation bypasses is directionally correct, but S2/S3 block treating session
lifecycle as fully qualified. K22 must pass S1 and its documented plateau and
snapshot-import gates before closure. No new product A/B modes or compatibility
work is proposed.

On macOS/Apple clang/Python 3.9, current source `c71cd1066`, no EFS mount or
cluster, the following commands **PASS**:

- `python3 tests/test_partial_uncond.py`
- `python3 tests/test_append_oracle.py`
- `python3 tests/test_mount_identity_rpc.py`
- `python3 tests/test_fin_abort_handoff.py`

The new diagnostic reproducers above expose defects; their successful execution
is **not** a passing product gate. All generated binaries live in temporary
locations; only reusable source is committed. Full Linux build/suites,
RDMA/direct-I/O deployment, host restart and payload qualification are **NOTRUN**
by this review. No production code is changed.


## Third pass — merged fixes at 0eb1511d2

**Verdict: OPEN.** Review of the merged implementation, not a cluster
qualification. All 15 named fix/evidence tips in the handoff are ancestors of
`0eb1511d2`: `e842d1f87`, `468513a96`, `42c631852`, `78e9100fd`,
`ded93816b`, `ba9f41dc2`, `4087f3f21`, `95160cc8d`, `b4a6c5a68`,
`8df4de684`, `12bfbe9e8`, `8c630edd5`, `aea319575`, `2fea76916`,
`4262ba8c1`. The subsequent documentation commit `af0996a3d` records the
merged `0eb1511d2` efs3 `make test` rc 0, with **15 NOTRUN** in the
checklist. This is success for executed checks, not complete qualification;
retain those 15 omissions in any release summary. That is reported evidence from the
other session, not a run performed or independently inspected here.

The original S1–S3 and R1–R3 mechanisms have been addressed: committed rather
than uncommitted backlog controls election eligibility; captured session rows
are checked in the existing owned wrappers and mount latches use atomics;
decision scanning has a bounded continuation; DECIDE/RETIRE validate exact
encodings; the summary runner returns failure/incomplete and retains failed
command output. The W125 harness now checks cleanup and records remote/final
mismatches. These improvements do not close the original mounted mismatch or
any outstanding live gate. K28 increases the cap and distributes packed data;
it still has two metadata groups with default voters confined to nodes 1–4,
not metadata throughput distributed over every additional node.

### T1 — P1: K15 introduces the K5 capture race in append-abandon

`src/meta/append_plan.c:929–940`, `efs_meta_abandon_append`, first calls
`efs_session_accept`, then separately reads the session row into the EXCL
comparison image. It checks only the image length. Unlike the repaired
reservation, mtime, completion and retirement wrappers, it does not call
`session_owner_capture` or check the captured established epoch/reject floor.
`host_append_abandon` (`src/server/raft_host.c`) does not serialize this
sequence against replicated fencing.

Interleaving: epoch 1 passes admission; FENCE_LOC raises reject_below to 2;
abandon captures that already-fenced row; its owner PREPARE compares the row
against those same bytes and can succeed. An old session can therefore retire
its receipt/reservation after fencing, contrary to this function's own promise
that a fenced owner uses session cleanup instead. This is a new integration
gap; it does not invalidate the existing mtime fix or claim demonstrated
payload corruption. **Source-confirmed; live/interleaving reproduction NOTRUN.**

Use the existing `session_owner_capture` in abandon, and extend the real-KV
barrier test to this entry point. Test fence/removal between acceptance and
capture (no prepare or receipt/reservation change), fence after capture
(conflict), and fence after owner prepare (wait for transaction resolution).
Reopen K5 for the authority invariant and K15 for this cleanup implementation;
K15 already remains INCOMPLETE for live failover/lost-reply qualification.

### T2 — P2: K1's forced-exit path can block beyond its deadline

`src/server/efsd.c:420–431` calls `shutdown_report`, then `fflush`, before
`_exit(3)` after the deadline has expired. `shutdown_report` at lines 395–416
performs synchronous stdio, open/write/fsync/close/rename on the storage root.
A stalled storage operation or blocked output pipe can prevent reaching
`_exit` indefinitely. The teardown worker's timed wait does not bound these
operations on the supervising thread. The current stuck-packer/handler test
exercises healthy reporting I/O and cannot establish the claimed whole-stop
bound. **Source-confirmed; blocked-report fault injection NOTRUN.**

Keep the outcome record and honest forced/unknown classifications, but make
reporting best-effort under an independent exit deadline; the final exit path
must not wait for filesystem I/O or stdio locks/flushes. Add blocked exit-record
I/O and blocked stderr tests alongside the stuck-worker test. Also account for
`stop_perf_recorder`, which runs before `shutdown_bounded` starts its clock,
when describing a signal-to-exit or stop-script deadline. Reopen K1's total
bound; the implemented phase deadlines and exit classification remain useful.

### Status corrections and verification limits

“Code: nothing open” and “no row reopened” are not supported after integration:
T1 and T2 require code changes. Existing live gates, K9's non-reproduction,
K15's incomplete qualification and scope decisions remain unchanged. A test
suite pass does not establish that every concurrency interleaving was tested.
The older `4090e717b` run predates the follow-up merges, so its claim to include
“every K merge” must be removed; use the exact merged `0eb1511d2` result instead.
No fresh-format deployment, store deletion, new product toggle or compatibility
requirement is proposed. The reported orphaned efs1 test processes were not
inspected or stopped in this review and cannot be declared cleaned up.

Local environment: macOS, Apple clang, Python 3.9, source `0eb1511d2` (later
`af0996a3d` changes documentation only), no EFS daemon/mount/cluster; transport,
I/O profile, fixture quota and RDMA are not applicable to these local checks.
Temporary fixtures/binaries only, no raw output committed. Commands run:

- `python3 scripts/reviews/k05_mtime_fence_capture.py` — PASS; existing mtime
  wrapper refuses the fenced image before any prepare, not coverage of abandon.
- `python3 tests/test_make_test_summary_runner.py` — PASS, including failure,
  timeout, NOTRUN, empty command list and retained failure output.
- `python3 tests/test_w125_harness.py` — PASS; synthetic cleanup and evidence
  checks, not a rerun of the original mounted mismatch.
- `python3 tests/test_shutdown_bounded.py` — PASS; cooperative handler 55 ms,
  stuck-handler refusal 324 ms. This is not the whole-daemon reporting test.
- `python3 tests/test_pack_object_remote.py` — PASS; extracted code/callback
  checks, not the five-node live gate.
- `cc -std=gnu11 -pthread -Iinclude scripts/reviews/k22_capacity_tail.c
  src/raft/raft.c src/raft/raft_mem.c src/common/batch_obs.c
  -o /tmp/efs-k22-review` then `/tmp/efs-k22-review 128` — the former all-held
  counterexample now sends 412 messages, zero held campaigns. This harness
  does not deliver messages: no claim of a completed election or plateau gate.
- `python3 tests/test_txn_apply_exact.py` — NOTRUN: compilation failed because
  this checkout has no `libefs.a`; no transaction assertions executed. Use the
  Linux build prerequisites and test command documented by that test.

Full Linux build, TSan, leak check, cluster gates, full payload checks and
blocked-report/abandon-race regressions are NOTRUN by this review. The other
session's efs3 result is retained with its provenance. Production code is
unchanged; these are review findings and status corrections.

### Response to the third pass (parent session, devel `62d30994f`)

Verified against the source at `62d30994f`; no code changed in this
response. Verdicts on the two findings and the corrections:

- **T1 confirmed, P1 stands.** `efs_meta_abandon_append`
  (`src/meta/append_plan.c`, after the `efs_session_accept` call) builds the
  owner image with a plain `efs_kv_get` of the `sess_local` row and checks
  only its length; `session_owner_capture`, which every other session-owned
  executor in the same file uses (reservation, mtime, completion, barrier
  retirement, `efs_session_prepare_owner`), is not called. The image is the
  EXCL compare base only, so a fence applied between acceptance and the
  capture leaves bytes that the owner PREPARE matches, and the old epoch
  retires its receipt or reservation after fencing. Same shape as S2, in
  code written beside the S2 fix by a session that never saw it. The fix is
  the one proposed: capture through `session_owner_capture` and extend
  `test_session_owned_capture` to this entry point with the three
  interleavings (fence before capture, fence after capture, fence after
  owner prepare). K5 reopened for the invariant; K15 keeps the code gap.
- **T2 confirmed, P2 stands.** `shutdown_forced` (`src/server/efsd.c`)
  calls `shutdown_report` (open, write, fsync, close, rename on the first
  storage root, then stderr) and two `fflush` calls before `_exit(3)`. A
  stalled root or a blocked stderr pipe holds the exit past the 50 s budget,
  and the 60 s stop-script wait then gives up without SIGKILL. Agreed fix:
  write the record from a helper thread or with the remaining budget as a
  hard bound, and `_exit` regardless; a record that could not be written is
  already classified "unknown" by `server_processes.py`, which is the right
  answer for that case. `stop_perf_recorder` runs before the clock starts and
  belongs in the signal-to-exit statement. K1 reopened for the total bound.
- **Status corrections accepted.** "Code: nothing open" and "no row stays
  reopened" were premature and are withdrawn in `62d30994f`. The
  `4090e717b` sentence is gone from the tally; the merged result is the
  `0eb1511d2` run on efs3. The 15 NOTRUN lines on efs3 are the VM's
  environment limits (no RDMA device, no fuse3 mount) counted by the summary
  runner, so they are listed gates for node9901, not passes. The two
  orphaned `test_reclaim` processes on efs1 had exited on their own by
  23:00 (`pgrep` found none); efs1's `/tmp` scratch is cleared. `make test`
  on an efs VM is suspended while the user's cluster runs there (`/tmp/efs`
  deployment, efsd on efs1 and efs3 since about 22:57).
- **`test_txn_apply_exact.py` NOTRUN here is expected**: it needs the Linux
  build (`libefs.a`), as its docstring says; it ran in the efs3 `make test`.
- **One addition, for the user's decision, not a defect (T3).** D38 alpha
  1.0 says metadata groups live on "at most 3 nodes (three-voter groups, as
  today)". K28 kept the four-node default layout: the shard group on nodes
  1–3 and SHARD2 on nodes 2–4, so metadata voters span four nodes. Either
  amend D38 to "at most 4 nodes, two three-voter groups, as today" or make
  `efs_md_group_voters` place both groups on nodes 1–3 (a one-line change,
  fresh format, cluster of three then carries all metadata). **Closed
  (user, Oct 22): D38 amended to "at most 4 nodes, two three-voter groups,
  as today"; K28's layout stands.**

Reopened rows and their next step: K5/T1 and K1/T2 are sub-session work
(implement, regression test, `make test` on an efs VM once the user's
cluster there is down), proposed to the user and not started.


## Review of the D38 amendment at e328744ad

**T3 CLOSED as a default-placement decision; live qualification NOTRUN.**
The amendment matches `include/efs/raft.h:efs_md_group_voters` and requires
no code or format change. Retain the recorded decision; no new placement
switch, compatibility path or migration is needed. This does not waive a
fresh-format requirement introduced by a different implementation change.

Corrections made by this review:

- State N = 3 explicitly: both groups use nodes 1–3. For N ≥ 4, SHARD uses
  1–3 and SHARD2 uses 2–4. “Nodes 1–4 whatever the cluster size” was wrong
  for the three-node alpha gate.
- Update the roadmap, which still said metadata was confined to three nodes,
  and D38's stale present-tense claim that packed segments use voter-only
  placement. K28's data-ring placement and remote object lookup are merged;
  their five-node live gate remains NOTRUN.
- Distinguish three replicated metadata voters (majority two) from 2+1
  erasure-coded data. One arbitrary node loss leaves both metadata groups
  with a quorum. Four physical metadata hosts do not give two-loss protection:
  losing nodes 2 and 3 leaves one voter per group. More than four cluster
  nodes add data participation under this default, not more metadata groups
  or metadata leaders.
- Call this the **default layout**, not an enforced four-node limit: the Raft
  membership API can reconfigure voters beyond node 4. This review does not
  claim that such reconfiguration has been qualified for alpha.

Local validation: macOS / Apple clang; source `e328744ad`; no cluster, mount,
transport, I/O profile or quota. `scripts/reviews/d38_default_voters.c` uses
the production header, checks the default masks for N = 3 through 16 and
checks that every single-node removal leaves at least two voters per group.
**PASS**, arithmetic only; no election, failover, payload or live acceptance
claim. Reproduce from the repository root:

```sh
cc -std=gnu11 -Iinclude scripts/reviews/d38_default_voters.c -o /tmp/efs-d38-check
/tmp/efs-d38-check
rm /tmp/efs-d38-check
```

Existing `tests/test_placement.c:k28_voters` also tests the default masks.
No production code changed. K5/T1 and K1/T2 remain open and on hold; K9/K15
remain INCOMPLETE and the existing live gates/scope decisions remain open.
