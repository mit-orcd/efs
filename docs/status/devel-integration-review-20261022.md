# Recent devel integration review — October 22, 2026

**Verdict: actionable findings OPEN; not release qualification.** Reviewed
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
