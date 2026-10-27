# K43: a writer fails when the REPORT coordinator is killed (2026-10-26)

**Report: branch `agent/devel-taske`, head `07bc65131` (from devel `394f2b791`; never pushed). Gate on intel2 at the head: `TOTAL 509 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (779s)` (NOTRUN = host limits: no RDMA device, no valgrind, no cluster scripts). Base comparison: the deterministic incident-chain test passes on the incident-era build (`39a1cbbbc`), on pre-wave-14 (`404307b0a`) and on current devel alike — the incident's application failure needed soak-scale apply saturation (7.3 s of silent finisher resolve retries) and lost by 130 ms of REPORT budget, so no quiet-loopback build fails it; the unit gate `test_pub_stage.c::test_reap_aborted_on_contact` fails on the base (BUSY) and passes with the fix. Evidence: this page; raw intel2 `~/efs-artifacts/taskE-20261026/` (SHA256SUMS there). NOTRUN for the coordinator: the four-hour soak rerun, multi-host, RDMA, orcd-mini. Product defects, one line each: the K43 residual margin — an abort-decided stage BUSY-ed contacts for the resolve's whole flight — fixed (`pub_stage_reap_if_aborted`); the one-hour soak's RSS-plateau sub-check failed twice on the branch (+5.3..+11.6 % on four windows) while base passed once at the line — not attributable to the reap (no allocation, rare path), F1/Task-I's marginal-check territory, numbers below; `test_k22_leader_admission.py`'s 'node 1 leads' assumption broke in the pool — fixed here (leadership is stepped onto the pressured node).**

## Purpose and item

Checklist row **K43** (`alpha-1.0-2plus1-checklist.md`): a writer's
`write()` returned `EBUSY` and `close()` `EIO` in the Task D four-hour
soak (nuc, `b2fc06ba5`, event 15) when node 3 was SIGKILLed while it was
the group-0 leader and the coordinator of a 64 MiB private overwrite's
in-flight REPORT: the new leader refused the resent unit NOT_PRIMARY and
the finisher decided the unit's abort in 12 ms, but the 57 PREPARE
intents stayed until the recovery scan's age gate (`age=8.1s -> ABORT`),
130 ms after the client's 8 s REPORT budget ran out. No acknowledged data
was lost. Rule: an application write must not fail while one member is
down. **Blocks alpha: yes.**

## What the investigation found

The deterministic loopback reproduction of the incident's full chain —
six fault-build members, the unit parked between its PREPARE batch and
its decision (`EFS_FAULT_PUB_BARRIER` content `prepare`), the coordinator
SIGKILLed at the park, the resent unit refused NOT_PRIMARY on the
failover host (new one-shot hook `EFS_FAULT_PUB_UNIT_NP_FILE`, driving
the finisher's abort of the parked transaction, the incident's
`fail=unit/-15`), the finisher held between the abort's decision and its
RESOLVEs (the barrier's new `abort-resolve` stage), the client's CAS loss
against the staged intents (`report-split stale=1 rc=-14`) and its stale
replay — **passes on the incident-era product code (`39a1cbbbc`) and on
pre-wave-14 (`404307b0a`) and on current devel**: the client recovers in
about 2–3 s every time. The replay ladder, the W64 finisher's 12 ms abort
decision and wave-14's unconditional-image publish (`66bfc2786`) cover
the whole chain on a quiet host.

What the incident additionally needed, from its logs (nuc
`~/efs-artifacts/task-d-20261025-nuc2/endurance2/`): n2's finisher
decided the abort at 09:20:13.624 and then produced **no line for 7.3 s**
— the RESOLVEs retried silently against a group whose apply was saturated
(foreground commits 1–60/s plus packer churn under the soak) until the
recovery scan's age gate landed them at 09:20:20.933; the restarted
node 3 answered the resend from its pre-resolve KV at 09:20:20.792; the
client's budget had ended 1 ms earlier. The application-visible failure
was a timing margin, not a logic error reachable on a quiet loopback.

## The fix

`src/meta/meta_apply.c`: a stage row whose owner transaction has a
recorded ABORT is a resolve in flight, and the abort's resolve of it is
the delete itself — so `pub_record_same`'s foreign-owner BUSY now first
reads the owner txn's locally recorded decision and **reaps the stage on
contact** (`pub_stage_reap_if_aborted`). A later unit that meets the dead
intent proceeds at once instead of BUSY-ing for the resolve's whole
flight; under apply saturation the client's progress no longer waits on
the finisher at all. Undecided stages stay BUSY, COMMIT-decided stages
are never reaped, and the late resolve is a no-op. No wire or diagnostic
contract change.

This is the task's server-side sentence — "drive the abort of a lost
coordinator's PREPARE intents as soon as it is decided, not at the
recovery scan's cadence" — at the strongest point: any replica that has
applied the decision resolves a contacted stage immediately. The client
side needed no change: the K38/K39 ladder already retries within the
REPORT budget, and with the heal there is nothing left to outwait.

## Gates

- `tests/test_pub_stage.c::test_reap_aborted_on_contact` (unit, make
  test): **fails on the base** (`an abort-decided stage is reaped on
  contact`: BUSY) and passes with the heal; covers the negative cases
  (undecided stays BUSY, COMMIT-decided never reaped, the late resolve is
  a no-op, the re-staged chunk blocks a third txn).
- `tests/test_k43_coordinator_kill.py` (new, `make test-long`, budget 20 s
  / 2 slots): the end-to-end incident scenario above; asserts no
  application error with one member down, no intent ever reaches the
  recovery age gate, and byte-exact final content on a fresh reader.
  It passes on every build tried — see above for why; it gates the chain
  against regression.
- Named neighbors, 5 of 5 alone each on the branch build:
  `test_pub_unit_forward_abort.py`, `test_k38_shared_chunk_pressure.py`,
  `test_k36_recovery.py` — all PASS (15/15). The K43 end-to-end test 5/5.
- Rehearsal soak (900 s, `--profile rehearsal`, five clean returns, one
  SIGKILL crash, one client restart, both additions): **PASS**, no writer
  error (`result.json` verdict PASS).
- One-hour soak (`--hours 1`, default churn): writers clean in both runs —
  no EBUSY/EIO anywhere; the K43 acceptance is met. The soak *verdict*
  failed twice on its RSS-plateau sub-check (F1's area): run 1 two windows
  at +5.58/+5.59 %, run 2 four windows at +5.3/+6.0/+6.7/+11.6 % last-third
  slope, over the 5 % line. The untouched-base hour soak **passed once**
  (max +5.0 %, itself at the line), so this is not cleared as
  pre-existing; it also cannot be attributed to the contact reap (a rare
  path: one KV read+delete per aborted-stage contact, no allocation), and
  the plateau page itself documents this check reading falling sawtooth
  halves as growth. Coordinator decision needed: whether the marginal
  plateau overruns block K43's closure here or ride with F1/Task-I (the
  dedicated nuc plateau rerun). Numbers and windows are in the run's
  `result.json` under the artifacts path below.
- Full gate (`make test-all`, intel2, EFS_TEST_JOBS=8, tree at the head):
  **`TOTAL 509 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (779 s)`** — NOTRUNs are
  host limits (no RDMA device, no valgrind, no cluster-scripts path).
  OVERBUDGET 1: `test_k43_coordinator_kill.py` measured 54.6 s under the
  pool against its 25 s row — the honest 1.25x row (70 s) would push the
  budgeted gate past the D44 1,100 s target's bound, so the row stays at
  25 s with the notice standing for the in-flight W160 budget refit (Task
  J owns the re-fit from measured walls this round). A first gate run
  failed `test_k22_leader_admission.py` on its 'node 1 leads' assumption
  (fixed in this branch: leadership is stepped onto the pressured node;
  3/3 after) — that was my task-B test's brittleness, not a product
  defect.

## Environment / reproduction

Host intel2 (8 CPU, Rocky), fault build (`tests/faults/efsd`), TCP,
loopback, private stores under `~/efs-taskE/tmp`. Commands:

```
python3 tests/test_pub_stage.c            # built by make; unit gate
python3 tests/test_k43_coordinator_kill.py
python3 scripts/soak.py --work ~/efs-taskE/soak \
        --out ~/efs-artifacts/taskE-20261026/soak --seconds 900 --profile rehearsal
python3 scripts/soak.py --work ~/efs-taskE/soak1h --out ... --hours 1
```

Raw run logs, soak outputs and gate summaries: intel2
`~/efs-artifacts/taskE-20261026/` with `SHA256SUMS`. Scratch worktrees for
the era builds lived in `~/git/efs-k43base`/`~/git/efs-k43pre` (never
committed; deleted).

## Defects

- K43 (this item): fixed scoped — residual timing margin closed by the
  contact reap; the incident's application-visible failure is not
  reachable on a quiet loopback on any build tried (see above), so the
  four-hour soak rerun and multi-host stay NOTRUN for the coordinator.

## Results

Gate: `TOTAL 509 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (779 s)` on intel2 at
the head commit. Soak and neighbor results are in Gates above.
