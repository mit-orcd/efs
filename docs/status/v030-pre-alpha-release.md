# v0.3.0-pre-alpha integration and acceptance

Status: integration in progress; not release accepted, tagged, deployed as a
release or pushed. User authorization, Oct 11: create an isolated integration
branch, coordinate all agents, add and test all improvements, prioritize
small-file packing and code-review findings. Release owner: agent 2 / AMD.

Branch: `release-v0.3.0-pre-alpha`, created from `devel` `48a2719d`.
Other agents retain their branches and cluster ownership. Shared `devel` and
`main` stay unchanged during integration. Main currently points to `53be1da7`;
latest existing tag is `v0.2.2-pre-alpha`. Only a frozen, accepted release
revision may be promoted and tagged. Push remains outside business hours in
America/New_York; no schedule or push is created by this checkpoint.

## Agent checkpoint contract

Commit logical work before submission. Supply exact checkpoint and tested
source/binary SHA, changes since devel, passed/failed/not-run gates and evidence,
deployed revision/configuration, wire/on-disk/upgrade implications, remaining
blockers and uncommitted work. Continue assigned work on the owner branch and
cluster. Do not merge/push shared devel/main, replace another owner's binaries,
wipe retained data or interrupt another owner's workload. Later checkpoints are
integrated explicitly; moving branch tips are not test identities.

## Checkpoint ledger

| Owner / branch | Submitted checkpoint | Integration state | Acceptance limits |
| --- | --- | --- | --- |
| AMD / devel-amd | `19f7efb3` | merged in `eff8d05a` | W94 CREATE/adoption closed; normal/fault native and both-mode full lifecycle pass. W97 repeated pressure comparisons continue; feedback stays opt-in. |
| ORCD / devel-orcd | code `79551691`, evidence `8d07b45a` | base merged in `ea5f5dc4`; exact-rejection repair in `8a7fd9be` | Full native and packed private lifecycle evidence; mounted concurrent POSIX times out. Pinned-stats, cold/shared-GC pass on ORCD; concurrent retest still times out. Bounded receipt-retirement `627d48ba` merged in `d1197381`; mounted acceptance remains pending. |
| Intel / devel-intel | `5f03ec76` plus fixture corrections through `5abf8e7b` (C source `3d69edb4`) | base merged in `22056d9b`; corrected fixtures integrated | Both-host native and complete direct/buffered private fixtures pass; supplemental fault matrix, remaining canonical/cold gates and retained-data deployment still pending. Production remains `7bde5f43`. |
| Review / devel-code-review | reports `1b0c6b7c`, fixes through `3ac828cd` | base merged in `5e381545`; further repairs in `9260d8a1` and `9acea823` | Reproduced use-after-free fixed; focused merged regression passes. Capacity accounting, recorder reap and incoming-chunk re-dirty fixes included; private find/flush/walker cleanup included. Full merged/Linux/live acceptance required. |
| NUC / devel-nuc | revised `ba73f304` | merged in `5cbe0ce4` | APPEND_CATCHUP tag52; focused merged gates pass. New-tag native rebuild, live/fault/cold/20k and measured W97 acceptance pending. Production remains `f52cd2dd-nuc`; preserved writer must finish before restart. |
| Mac / devel-mac | `b2c01fa0` | reconciled in `93bb30de`; complete-server compile corrections in `29b33f80` | Request-local authority masks and combined registration retained alongside ordered NUC/Intel reservation guards, tag52 completion and Mac prune/close cohorts. Tag53 experiment excluded. Owner review and merged gates pending. Concurrent CREATE timing still open. |

Submitted checkpoints are implementation inputs, not release certification.
Some are already contained by another branch; Git ancestry and source diffs
must determine overlap. The detached ancestry audit confirms bench, storage-engine, known-bugs,
find-stats, RDMA, gate-closure, compaction-pressure and primary checkout commits
are already contained. Old find-stats has four modified historical log files
and no uncommitted code; retain them untouched. No improvement is silently
dropped merely because its branch name was retired.

## Priority 1: mounted tiny-file packing

[ORCD packing](orcd-tiny-packing-20261011.md) targets actual tiny logical payloads.
[Mounted coalescing checkpoint and failed gates](orcd-mounted-coalescing-20261011.md)
records the deployed implementation separately from its pending acceptance.
The ordinary 512-byte sample occupies 208,896 fragment bytes (408 times payload).
Containers pack existing padded fragments; that is a separate optimization.
Keep both implementations and evidence, without treating containers as proof
that logical payload packing is active.

Require mounted cohort writes, singleton/deadline/fsync/close sealing, aggregate
memory/admission bounds, exact retained retry identities, per-record durable
publication, mixed ordinary/packed reads, overwrite/growth/truncate/unlink,
shared-object GC, cold restart and relevant lost-reply/partial-PUT/persistence/
coordinator/fencing fault gates. Missing members or transport replies never
substitute for durable proof. All applicable POSIX/peer gates must finish with
actual verdicts; known unsupported mmap and independently tracked repair gaps
must be named, not hidden by a blanket skip.

Compare alternating baseline/candidate runs with identical workload, durability,
placement policy, engine/I/O/transport configuration and observation settings.
Record files/s and durable close tails, cohort occupancy/deadlines, logical live
bytes, fragment and index/WAL writes, allocated bytes, CPU/RSS and post-delete
physical reclamation. Code inclusion does not activate a default. Mounted
packing remains unaccepted until these gates pass; a timeout blocks its release
acceptance and requires a repair/retest, not an optimistic performance claim.

## Priority 2: code-review correctness

[Review disposition](../reviews/release-disposition-20261011.md) protects active
packing/query consumers and public/native/wire compatibility. Integrate bounded
correctness fixes first: rollup allocation ownership, retained queue accounting
and recorder stop/reap. Each needs focused failure/boundary tests plus relevant
merged native and mounted gates. Broad refactors and API deletion must have
explicit consumer/contract validation; a base-specific unused-symbol scan is
not sufficient. Retain the independent packing model and RAM query oracle.

## Integration conflicts and protocol allocation

ORCD packing uses command/request tags45–51. NUC's undeployed tag45 collision
was corrected to APPEND_CATCHUP52 in `ba73f304`; tag45 native results are historical
and cannot certify52. Mac is reserving53 for its undeployed single-shard append
experiment; do not use that number elsewhere. Run the actual canonical-header
namespace gate on every merged revision. All voting servers must understand new
commands before admission; preserve native client/server layout compatibility
and document cold replay/rollback restrictions before rollout.

Resolved ORCD harness conflict retains both command-only and synthetic-pressure
options. Generated architecture is regenerated from source documents. NUC
persistence-refusal merge preserves every packed command and APPEND_CATCHUP
IO/NOMEM retry case; neither may advance past undurable state. The append
PREPARE regression retains physical-cohort checks while obsolete REPORT
completion assertions move to the actual new catch-up host regression.
Raw evidence is preserved byte-for-byte, including historical failures and
log whitespace; source/current-doc whitespace gates are checked separately.

## Release acceptance matrix and promotion

1. Finish checkpoint integration and reconcile owner-reported conflicts; audit
   committed detached worktrees and pending working changes. Record each
   improvement as included, pending or explicitly experimental, with a gate.
2. Freeze exact source/build inputs; run full Linux normal and fault native,
   relevant sanitizers/memory/namespace/durability regressions and doc gates.
3. Gate the same candidate in buffered/direct TCP on assigned clusters; software
   RDMA on AMD and hardware RDMA on ORCD only. Owner coordination precedes any
   shared-cluster deployment. Keep four-daemons/one-host and independent-host
   evidence distinct. VM storage timing is not hardware performance proof.
4. Complete full POSIX, peer/concurrent/extras, cold acknowledged bytes,
   retry/fencing/publication, fault recovery, physical GC and retained-data
   verification, including new packed/ordinary mixed workloads. Track normal
   service restoration and source/binary/configuration identity independently.
5. Summarize accepted improvements, opt-in flags, known unsupported features,
   blockers and wire/store/client/server upgrade/rollback requirements in release
   notes and new-user/operator/developer docs. Current fixed2+1 data/three-voter
   metadata implementation must not be described as D36's completed four-node
   two-loss protection guarantee.
6. Only after merged acceptance, promote that exact commit to devel/main, create
   annotated `v0.3.0-pre-alpha`, then push main/tag in the authorized out-of-hours
   window. No failed packing gate, branch-native pass or earlier deployment
   proves that the final merged release passes.

## Current release-branch gates

Focused merged command namespace, ordered APPEND PREPARE/cohort semantics,
APPEND_CATCHUP20k shape/refusal and rollup allocation-failure regressions pass.
Five architecture checks pass after regeneration. This is not full Linux,
mounted, performance or release acceptance. The release ledger will be updated
with exact final test/build identities and raw evidence before promotion.

The [integration checkpoint evidence](../../results/measure/20261011-v030-release-integration/focused-checkpoint.json)
records ten passed local focused gates. The packed-retry invocation exits1
because the fresh local worktree has no built libefs.a; retain that setup
failure and rerun after the Linux build. It establishes no production defect
or pass. The [detached ancestry audit](../../results/measure/20261011-v030-release-integration/detached-ancestry.json)
records exact worktree commits and tracked-change counts.

## AMD merged qualification — October 11

The initial `9acea823` fault build caught a duplicated namespace wrapper and
two missing APPEND cleanup contexts. `29b33f80` repairs complete-server
compilation; focused authority/ordered PREPARE tests pass. Its full suite then
caught missing LOOKUP test dependencies for speculative pair reads. `063bc8c4`
updates that regression, including stale hint and BUSY fallback assertions.
The next full run reached report-pressure and found a stale session-proof
helper extraction. The regression now covers the actual request-local bitmap
helper, foreign-group fresh reads and mutable epoch/state; combined
REGISTER_ESTABLISH business/storage verdict coverage is also included. These
are retained failed gate verdicts, not release acceptance.

AMD builds use isolated `/data1/efs/release-v030-20261011`; normal stores and
installed binaries remain intact. Preceding W97 comparisons and service
restoration exited0 before release builds began. A new private
`--mounted-packing-only --posix` gate explicitly enables packing and commands,
requires physical packed fragments, verifies peer/cold bytes through overwrite,
growth/truncate/unlink, runs core/extras and peer POSIX, and requires physical
packed reclamation. It preserves failed POSIX verdicts while collecting the
independent cold/GC checks. Its live verdict remains pending.

The merged native `bdee0d1c` run reached the late namespace-owner regression
but failed obsolete REGISTER-proof shortcut expectations. `f1d25b3e` retains
exact production proof-emitter error/fencing coverage and expands the actual
fresh-admission helper tests across command refusals, floor errors and fencing.
ORCD `627d48ba` is included in frozen candidate `d1197381`; full normal/fault
AMD native tests and serialized direct/buffered packed mounted gates are
running. Input hashes cover929 build/test/documentation files, with no
synchronization mismatches. NUC reports both tag52 native builds pass on
`ba73f304`; new-tag live/cold/20k acceptance remains pending.
