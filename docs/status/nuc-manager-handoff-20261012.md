# NUC handoff to release manager — October 12, 2026

State captured approximately 00:34 UTC. The direct human instruction in
**efs-coding 6 - mgr** was verified: document current state, coordinate all five
branches, then receive stability assignments. New feature work and new
candidate deployments are paused. Existing gates finish safely. No subagents
exist; no shared devel/main mutation or push is authorized here.

## Frozen source and integration

- Owner worktree: `/Users/mike/Documents/Codex/2026-10-07/i-l/work/efs-find-stats`.
- Branch: `devel-nuc`; frozen source **`101598d2f2d0ce9ac99ab59f840130f3c8f0e218`**.
- Clean before this handoff; no uncommitted code or experiments to import.
- [Exact ordered inventory](../../results/measure/20261012-nuc-manager-handoff/merge-ready-commits.txt)
  contains 189 commits not in the current local devel tip; common base
  `48a2719d19fb5a6b0464935c70a967a1f1a86236`. Many are already in the release
  branch: use ancestry/content checks, not blind duplicate cherry-picks.
- Main logical order: W97 root barrier `19cbb11f`, certificate exception
  `766814c8`; W52 one-completion entry `32b472c5`, wire correction `ba73f304`;
  subsequent evidence/scheduler commits; W112 minimal repair **`627958bd`**
  (same patch as release `fdfbd9c4`); minimal isolated live fixture **`444cd4f0`**;
  required pre-promotion ordinary gates **`101598d2`**. Keep both existing
  packing fixtures and the ported ordinary gate on integration.
- `63c9cb15` removes two accidentally copied binary Raft logs from current HEAD,
  retains original paths/hashes and leaves history intact. Do not resurrect
  their copies when reconciling evidence. No compiled binaries are tracked in
  the current retrieved gate evidence.

Merge-ready means committed/reviewable, **not accepted for rollout**. W52 and
W97 remain open. The root-scope optimization is experimental pending matched
performance acceptance. Earlier pressure/batching flags retain their recorded
experimental disposition and defaults; do not enable them from source inclusion.
AMD is the sole release worktree editor/executor; this owner has not edited it.

## Installed versus active versus rebuilt

Installed source/binaries `/data1/efs/src` remain **`f52cd2dd-nuc`**. Exact five
versions/hashes are in [installed images](../../results/measure/20261011-nuc-resume/clock-install/images.json).
Regular stores, `.mkfs-done` and retained payload trees are preserved. Regular
four-node service is normally stopped while the private fixture owns policy
mounts `/data1/efs/mnt` and `mnt2`; no pending regular writes were discarded.

Active old fixture uses **`ba73f304-w52-nuc`**, source
`/data1/efs/devel-nuc-20261011/catchup-base-src`, four private voters on
`127.0.0.1:26590..26593`, TCP/direct I/O, quota 1G each. n1/n2 stores:
`/data1/efs/gc-direct-dly7jx6q/n1,n2`; n3/n4:
`/home/efs/gc-direct-j16ejmaf/n3,n4`. These are private stores, not regular n1..n4.
Regular ports are 17432..17435. NUC remains this owner's exclusive cluster;
no AMD/Intel/ORCD changes were made.

Configuration: commands disabled (`EFS_COMMANDS=0`, server=0), flow control=1,
early-retire/overlap/noop/group-drain experiments=0, applied interval=10ms,
batch observation=0. The queued W97 design uses observation=1 equally in all
cells, three voters, SATA metadata, SATA then NVMe data, node1 leading both
metadata groups and ABBA base/root/root/base (eight cells ×600 seconds).
Nothing in that matrix has run; no throughput improvement is claimed.

Repaired builds use frozen **`101598d2`**, including W112 identically in both
variants. Only `store.c`, `packed_fence.h` and the corresponding durability test
differ. Build IDs: `101598d2-w52-nuc` / `101598d2-root-nuc`. Isolated staging:
`/data1/efs/devel-nuc-20261011/catchup-w112-native-staging`.
[Input manifest and running build receipt](../../results/measure/20261012-nuc-manager-handoff/)
retain all166 C/H+Makefile hashes, source archive SHA and wrapper SHA. Archive
excludes results; canonical root overlay remains the hash-verified f52 baseline.
No repaired binaries have been installed or admitted. Compilation overlaps
old correctness-only verification, never a W97 timed comparison.

## Running/held jobs

- **129781**: original base full-live controller. Child **140594** is finishing
  old direct20k exact peer/cold/physical verification. Do not interrupt its
  writer/verification or replace its source/binaries/stores.
- **145724 /145725**: repaired baseline/root isolated Linux native build/test
  controllers; clean/all exits0 for both, complete native still RUNNING at
  capture. They may finish normally; do not launch new mounted gates afterward
  until manager reconciliation/assignment.
- Execution session **2695**: bounded guard waits for original long-direct exit0,
  then signals only the next verified empty buffered fixture before clients or
  writes. It records a receipt and lets original controller normally restore
  installed f52 service. If its scheduling timeout expires, that is not a gate
  failure; current owner must renew the same safe guard. Never force-discard
  pending writes to stop the recipe.
- **129782..129785**: installer, preserved cold, root full-live and W97 ABBA were
  safely held/stopped while idle. Exact command/no-child/no-started-work receipt
  is [committed](../../results/measure/20261011-nuc-resume/catchup-w112-hold/receipt.json).
  **Do not restart them unchanged**: original inputs have the shared W112 bug.
- Free space at capture: SATA207GiB, NVMe463GiB. No tiny generator is running.

The existing guarded cleanup/restoration is preservation, not a new deployment.
Moving source directories or launching a new candidate is postponed until the
manager's merged candidate/stability assignment is explicit.

## Exact verdicts and limits

| Source / gate | Verdict at capture |
| --- | --- |
| Old installed f52 full native, full direct+buffered POSIX216/peer67 and2k | PASS; prior ledger/raw |
| Old f52 preserved20k fsync/warm exact +cold36 payload checks | PASS; compilation interfered, correctness-only |
| ba73 corrected tag52 base/root complete Linux native | PASS both; superseded source, no W112 repair |
| ba73 base full direct POSIX216/216, peer67/67,2k fsync/peer/cold and selected sync/recovery/WAL/GC | PASS0; raw75bc |
| ba73 base separate20k direct | Writes20000,81920000 bytes in5534.208s; fsync0 in0.283547s; both warm hashes1075295d…47d4. Cold/physical/final exit still RUNNING |
| ba73 buffered full/20k, root full-live, installation/new preserved cold, W97 ABBA | NOT RUN / held; no acceptance inheritance |
| New W112 actual-function test against old ba73 | FAIL as expected: full-owned sent-byte assertion; shared defect confirmed |
| Fixed627958bd normal and actual ASan/UBSan flags | PASS locally; retained w112-focused raw/provenance; not Linux/live acceptance |
| New101598d2 base/root clean+all | PASS both; native RUNNING; mounted ordinary/full/20k/cold/ABBA NOT RUN |

[NUC acceptance ledger](nuc-w52-w97-resume-20261011.md) and
[raw evidence](../../results/measure/20261011-nuc-resume/) retain historical
failures, corrections and exact build provenance. Tag45 builds were superseded
before admission and cannot certify52. W112 release-owner AMD qualification
reports both-mode four-case immediate primary/peer/cold physical passes; this
is not NUC acceptance. The actual NUC old-function failure independently shows
why promotion was held.

## Correctness and compatibility blockers

1. W52: final repaired source must complete full native, isolated ordinary
   overwrite, both-mode full POSIX/peer/fault/GC, exact20k append and preserved
   installation/cold gates. One inode-group proposal completes all caught-up
   reservations; a separate exact-image lane-barrier retirement transaction
   must still finish before REPORT reply. No timeout/durability relaxation.
2. W112: accepted canonical full-overwrite bytes must survive STALE replay.
   Fixed full-owned snapshot binds retry to fresh committed generation and
   lane/fence observation; partial/spans must retain range-only ownership.
   The old code acknowledges corrupted contents after successfulfsync/cold.
   It must not be promoted merely because old ordinary suites passed.
3. W97: measured slowdown reduction remains entirely unproven for this matched
   candidate. Retain early/late unprofiled windows, actual sync counters,
   physical latency, leader/config identity, all cold checks and restoration.
   Eight accepted cells and truthful reduction metrics are still required;
   no SLC-cache/NAND/hardware-ceiling claim follows from the existing evidence.
4. Durable metadata allocation: ORCD45–51; **APPEND_CATCHUP52**; Mac53/54 are
   owner allocations in release, not implemented by this branch. Transport
   IDs141/142 and143/144 are separately reserved by packing owners. No reuse.
   Verify final canonical-header namespace and all decoders after merging.
   All voting servers must understand52 before admission. Wire38/58 bytes,
   bounded32766 reservation scope; no persistent-row format change. Never
   automatically downgrade a log containing uncheckpointed52 to an old binary.
5. Preserve IO/NOMEM committed-entry retry/no-applied-marker semantics, ordered
   owner/inode/lane guards, exact-image lane retirement and packing consumers
   during conflict resolution. Do not regress single-entry completion to the
   older per-reservation/small-cohort completion loops.

After this committed handoff, freeze code, preserve/finish existing jobs, report
final evidence separately and wait for a specific stability assignment. Do not
promote main, push, merge shared branches or deploy an unreconciled candidate.

## Completed-job follow-up — 00:37 UTC

Oldba73 direct20k completes with exit0:20000 unique4096-byte records,
81920000 exact bytes, writes5534.208s and finalfsync0 in0.283547s. Primary,
peer and cold SHA all1075295d16ff4f24d1548ba048cd3fc46e986163a833f7ef9f622b5b10a647d4;
physical fragment inventory, queues and quota zero PASS. The following empty
buffered fixture was stopped before children/mounts/writes; exact receipt is
retained. Controller is normally restoring installedf52 service; restoration
is still pending. This accepts that old direct gate only, not repaired101598d2.

Both new isolated native controllers finish with clean/all0, but native2/run1:
full native stops at the architecture documentation link check because the
new archive deliberately omitted tracked results referenced by those docs.
This input-packaging mistake is not a production-code failure or a native pass.
Both raw native/failure/provenance attempts are retained in the handoff evidence
native-base/native-root folders. No restart, skip or new live gate is launched.
The [merged qualification plan](../archive/status-20261012/nuc-merged-qualification-plan-20261012.md)
requires a complete source/evidence archive and full rerun on the manager's
approved input. Source remains frozen101598d2. No repaired image is deployed.

## Safely idle and ready — 00:40 UTC

Normal preserved restoration passes0. Independent final read-only audit checks
all five installed image hashes, four mapped daemon and two mapped client
hashes/configurations, all17432..17435 listeners, both mounts, closed private
26590..26593 ports, retained `.mkfs-done` and all36 prior payload hashes.
Installed source remainsf52cd2dd-nuc. Every owned gate/build controller has
finished or was held; there is no active writer or new-candidate deployment.
[Final proof and restoration logs](../../results/measure/20261012-nuc-manager-handoff/final-restoration/)
retain exact processes/paths/config and payloads. The original base recipe records a failure from the safely superseded empty
buffered fixture and leaves its overall run-exit marker absent; do not invent
an overall exit marker. Its actual interrupted-fixture exit, restoration0 and
completed old direct20k0 remain separate genuine verdicts.

Code is still frozen101598d2; only documentation/raw evidence followed.
NUC is ready for the manager-approved merged source/hash. No new live/native
rerun, source installation, performance matrix, shared merge or push proceeds
until that explicit qualification assignment.
