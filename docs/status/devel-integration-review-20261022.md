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
| W125 cleanup and remote/final evidence | **Still incomplete.** Mount absence is now checked, but owned daemon termination is not; helper still sends KILL and finishes with success. Cleanup return status is ignored. Remote reads still accept any version, and remote/final mismatch bytes are not captured. A hard link preserves inode reachability, not an immutable version: modifiers can continue changing it after the capture lock is released. |

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
