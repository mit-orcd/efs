# NUC qualification plan for the manager-approved merged release

This is a plan, not execution or rollout approval. Source is frozen locally at
`101598d2`; the release manager must provide the forthcoming merged commit,
archive/source hashes, configuration and qualification assignment first. AMD
owns the release worktree and merged release editing. NUC is this owner's
assigned cluster; regular f52 service and all existing stores/data are preserved.

## Admission prerequisites

1. Record manager-approved source commit, complete archive SHA, build-input
   hashes (production, headers/dependencies, tests, scripts and Makefile), build
   command/compiler, flags and five executable versions/SHA. Never infer the
   deployed image from a checkout branch name.
2. Include tracked evidence files referenced by documentation in the archive.
   The isolated101598d2 packaging omitted results and both complete native
   attempts failed the documentation link gate. This is a fixture/input defect,
   not permission to skip the gate. Retain both failed attempts; repair the
   bundle without changing approved engine bytes and rerun full native when
   assigned. Recheck generated docs, link references and namespace gate.
3. Verify durable command namespace45–51 reserved,52 APPEND_CATCHUP, Mac53/54
   allocations and transport141–144 against the actual merged canonical
   headers. Verify decoder/apply and pressure classification on every voter.
   Confirm native client/server layouts remain compatible.
4. Preserve single-entry caught-up completion and ordered owner/inode/lane
   guards through conflict resolution. Completion cannot acknowledge IO/NOMEM
   or advance its applied marker after an undurable batch. Lane barrier
   retirement is a separate exact-image transaction and must finish before
   REPORT reply. All voters must understand52 before it can be admitted;
   never replay an uncheckpointed52 log through an older binary.

## Native and focused gates

Run full Linux normal/fault native and relevant sanitizer gates on the exact
frozen merged source before mounted acceptance. Retain original failed and
passing attempts independently. Targeted gates include canonical namespace,
ordered APPEND PREPARE/cohorts, actual host one-proposal20k routing/verdicts,
actual-KV20k and32766 boundary/all64lanes, bounded captured-count BUSY/overflow,
partial REPORT, session/generation/epoch fencing, atomic IO/NOMEM apply-scope
rollback with no marker, cold/lost retirement COMMIT and retry-only retirement.

The new W112 actual production-function test must fail against the old broken
source and pass fixed normal/ASan/UBSan. A full-overwrite snapshot can replace
all bytes only when ownership is canonical; partial/spans retain range-only
replay. Fresh generation and lane/fence read observation still gate retry.
Never treat immutable PUT contents or a returned write count as publication.

## Isolated mounted acceptance on NUC

First verify no generator, writer or unrelated daemon owns the regular mounts.
Capture all existing regular image/config identities and old payload hashes.
Normally drain both clients and stop all regular daemons; do not force-discard
writes. Use private stores, four separate ports and policy-allowed `/data1/efs/mnt`
plus `mnt2`. Record real physical roots/device roles, quorum and mapped image
hashes. Never format, replace or inject faults into regular n1..n4 stores.

For each mode, direct then buffered, on the same approved source:

- Independent `tests/live/gc_reclamation.py --ordinary-overwrite-only
  --fixture-quota 4G --transport tcp --mode MODE`: warmed512-byte ordinary
  publication followed by262160-byte growth, exact131072-byte full overwrite,
  zero131072-byte full overwrite and323-byte partial replacement preserving
  suffix. Require successful write/fsync/flush/close, immediate exact primary
  and peer bytes, private-daemon crash/cold exact bytes and physical/queue drain.
  Retain each expected/actual hash and size;
  correct stat size alone cannot certify contents.
- Full recipe: `--fresh-namespace --posix --continue-posix-failure
  --append-burst 2000 --sync-writes --sync-write-fault --append-sync-boundary
  --append-replay-race --append-audit --stop-dirty-recovery
  --shutdown-mutations --wal-tail-recovery`. Use the approved merged helper's
  supported arguments; retain whole POSIX/peer TSV counts and every failure,
  skip/notrun, explicit unsupported mmap boundary, fault and cleanup verdict.
  Continuation after POSIX failure collects independent evidence, not success.
- Separate `--append-burst-only --append-burst 20000`: exact single-description
  O_APPEND records with unique4096-byte bodies, finalfsync0,81920000 exact bytes
  on primary/peer and after cold restart, physical fragments/queue/quota zero
  after deletion. Retain maximum write and final sync latency, payload hashes
  and source/binary/config manifest. The existing fixture outer budget10800s
  is separate from the unchanged30-second client REPORT/RPC limit. Do not widen
  that limit or infer final source acceptance from the old ba73 run.

Required private roots: two `--root` arguments for the existing four-voter
helper, separate empty allowed primary/peer mount paths and four free ports.
NUC is TCP. Explicit RDMA/software-RXE evidence belongs to the assigned AMD
owner; no silent TCP fallback or cross-cluster deployment is allowed here.
Monitor physical space/inodes/RSS, preserve raw stores/logs, and stop only by
normal owned cleanup unless the gate explicitly crashes its private processes.

## Preserved-store installation and cold gate

Only after native and both-mode mounted acceptance, and explicit manager
source/hash reconciliation, normally restore old service and verify its exact
prior payloads/images. Then, if assigned to deploy, back up source, normally
drain both mounts/all four daemons, install all five exact accepted images and
verify each mapped `/proc/PID/exe` SHA before admission. Preserve `.mkfs-done`,
existing n1..n4 stores, placement, quota and TCP/direct configuration. No
old-binary automatic rollback after a new durable command reaches a log.

Create a dedicated new2k unique append tree, require primary/peer exact bytes
and successful finalfsync. Normally drain/restart the same approved four-node
cluster, mount two fresh clients, verify every old sampled payload plus the
new bytes on both mounts, all ports/config/mapped images and normal restoration.
A progressing cold recovery outside a script's launch-readiness interval is
not grounds to kill it or discard a WAL. Diagnose/record readiness separately.

## W97 remains deferred until stability is accepted

No measured root-barrier benefit is established. After a specific performance
assignment, freeze matched builds with identical merged W52/W112 code and only
store.c root-scope differences (+corresponding native test).
Both variants require functional acceptance first. Three voters, SATA metadata,
SATA thenNVMe data, fixed sampled leader placement, base/root/root/base twice
per device; eight600-second cells with identical workloads/configuration.
Exclude matched late perf/strace windows from first/last60-second throughput
comparison; record actual sync counts, batch/phase counters, physical write/flush
means, queues/refusals, RSS/CPU and64 exact cold files per cell. Restore and
verify preserved regular service. Report both repeats, late files/s and change
in fractional decline; neutral/worse results keep W97 open. Fewer syscalls,
an old append fsync pass or device-latency correlation cannot prove cache/NAND
cause or a hardware ceiling.

All verdicts are source/configuration-specific. Close W52 only from the final
approved code's required gates; do not convert documentation/input failures,
old passes or release-owner AMD results into NUC acceptance. Report failures
and remaining compatibility gaps to the manager before further rollout.
