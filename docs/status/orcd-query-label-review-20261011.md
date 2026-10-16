# Second diagnostic label review — October 11, 2026

Pinned source `1fb5339f5bd4fb878ed3ae679bb9ef7e041f7be1`.
Read-only adequacy review requested by AMD before another expensive run.
No release edits, alternate instrumentation, new cluster staging or deployment.
ORCD production checkpoint and live runtime remain 83cff804/4fe291b0.

The instrumentation is **adequate for the narrower query hypotheses**: retained
catalog absence versus current-read/binding refusal, transaction-kind activity
across a rejected cut, and initial lag writer-lock wait. It **cannot establish
a shared cause of foreground directory admission/proposal waits**; those phases
are not instrumented. No additional authority/ownership defect was found in
this bounded changed-surface review. This is not full native or live acceptance.

## Exact meaning of labels

`root-catalog-absent` means catalog inode lookup returned NOT_FOUND and the
fresh transaction-aware inode lookup succeeded. That branch returns its
existing BUSY before checking fresh S_ISDIR/nlink, permissions or binding.
It proves a current row was found, not an authorized, linked, reachable new
directory. A controlled successful regular-file lookup also produces this
label. Use the emitted inode field to distinguish root from ancestor;
the requested root alone is not the failing row.

`root-live-txn` and `binding-live-txn` label the transaction-aware lookup API's
unchanged error. A BUSY can be relevant to resolution, but the label alone does
not prove a pending transaction marker; a hard backend IO error also uses the
same label with its actual IO status. Do not summarize every such event as
transaction contention. `root-catalog-read` identifies a non-NOT_FOUND catalog
lookup error. Parent/generation and binding-mismatch labels identify actual
inequality checks, but do not log both compared values or establish which
mutation changed them. Binding-missing preserves the old NOT_FOUND→BUSY rule.

`root-expiry` traces the existing entry deadline check; it does not prove each
underlying read respected an exact shared realtime limit. Permission refusal,
non-directory/unlinked fresh row on the cached path, auth cache exhaustion and
some ancestry exits retain broader wrappers without their own new leaf label.
A wrapper with no new leaf should not be assigned to one of the logged leaves
by elimination. Wrapper-only refusals remain unresolved rather than belonging to a leaf by elimination.

Root/page wrappers propagate nested failures and are not independent counts.
The root helper's elapsed time starts before catalog lookup: a live-read error
includes earlier catalog work, and parent/generation time includes both lookups.
The binding helper measures its binding lookup sequence. None is exclusive
backend fsync, Raft or transaction-resolution time. Millisecond rounding can
produce zero. A successful root cache hit is not separately timed/traced.

## Counter and applied-vector scope

The eight fixed saturating counters count successful in-memory mutation records
for INTENT/GUARD/REDUCE/DECISION PUT/DEL while query tracking is enabled. Repeated
writes, deletion tombstones and retry materialization can count separately.
They are not unique transactions, currently pending records, participant masks,
durable acknowledgements or completed batch counts. A later batch/flush failure
can occur after earlier in-memory records were counted and the store becomes
sticky-failed. Ordinary inode mutations do not change these counters.

They are process-local hints. Replay disables query tracking; tracking-disabled
activity is not counted; restart/saturation prevent using absence of a delta as
proof of absent work. They retain 64 bytes per KV and snapshot, not per entry.
The snapshot's counters and transaction revision are copied with its KV image
under the same KV mutex. The live revision and activity **getters are separate
mutex acquisitions**. The caller holds host.mu, which serializes host apply,
but claiming one atomic live revision/counter tuple additionally requires the
premise that every relevant write obeys that host serialization. The APIs alone
do not supply that combined observation.

Pin/live applied vectors are diagnostic local observations, not a distributed
participant seal or proof that every covered transaction completed. They cannot
identify which operation or key produced a kind delta. Preserve actual-cut
refusal, imports and fresh participant authority regardless of logged counts.
Counter sampling errors do not grant permission; no diagnostic value is used
in the production verdict. Additional 64-byte snapshot retention is included by
the existing sizeof-based pin admission, so very tight boundary budgets may
refuse a pin earlier; diagnostics are not literally zero-cost.

## Logging and run adequacy

Initial `lag-lock` now times the previously missing writer acquisition. It
runs on the existing 100ms publisher loop even when refresh is not due or is in
backoff, potentially about 10 events/sec per capable publisher. It is not one
record per build attempt. Logging occurs after acquisition while holding the
query writer lock. Transaction activity/vector events are printed while holding
host.mu. Root logging can occur while holding a borrowed query view. Synchronous
stderr logging perturbs those resource intervals; an instrumented result cannot
be substituted for quiet pressure acceptance. Existing late publication-lock
and nested wrapper caveats remain.

Before freezing the run hypothesis, state that this source tests query root
branch/cut activity/lag-lock causes. If the intended conclusion includes which
foreground directory admission, namespace transaction or proposal/verdict phase
was slow, the required discriminator remains absent. Do not infer that cause
from a matching time window, eight aggregate counters or a zero lock sample.
No alternate implementation or diagnostic patch is proposed here.

## Focused validation

The actual root/binding extract on an archive of 1fb5339f passes local
ASan/UBSan with trace off/on, including original refusal/status/output checks
and measured controlled durations. An evidence-only boundary extension on the
same production functions additionally proves:

- Fresh successful regular-file lookup after catalog absence still returns BUSY
  through `root-catalog-absent` before directory/access validation.
- Hard current-read IO remains IO through the `root-live-txn` label.

The reproducer is
[the retained boundary harness](../../results/measure/20261011-orcd-rollout/1fb5339f-label-review/test_root_label_limits.py).
Set `EFS_REVIEW_SNAPSHOT` to an archive of 1fb5339f and run with
`CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer'`.
It extracts production helpers and supplies controlled endpoints; it does not
perform actual transaction contention, full Linux LSM, daemon shutdown, mounted
pressure or cold/physical acceptance. The new real-LSM counter regression was
pending Linux qualification when this review was requested; no pass is claimed
for it here. No tests/budgets/fences were removed or relaxed.
