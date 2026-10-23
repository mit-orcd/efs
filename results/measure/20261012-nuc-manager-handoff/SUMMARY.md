# Run summary: 20261012-nuc-manager-handoff

## Purpose and linked item

State-capture/handoff checkpoint for the NUC owner to the release manager on
2026-10-12: freeze source at `101598d2`, document installed-vs-active builds,
record exact verdicts of running/held gates, and wait for a merged-candidate
assignment. Linked status page:
`docs/status/nuc-manager-handoff-20261012.md` (work items W52, W97, W112).
Not a release gate run — a coordination evidence bundle.

## Verdict

Mixed per gate (exactly as the raw exits support):

- Old ba73 base full direct recipe (POSIX 216/216, peer 67/67, 2k): PASS
  (`old-base-controller/full-direct-exit.txt` = 0).
- Old ba73 separate direct 20k gate: PASS exit 0 — 20,000 unique 4 KiB records,
  81,920,000 exact bytes, writes 5534.208 s, final fsync 0.283547 s, primary/
  peer/cold SHA256 all `1075295d…47d4`, physical/quota zero.
- Next empty buffered fixture: safely interrupted (SIGTERM, exit -2) BEFORE
  mounts/writes because shared W112 corruption required repaired builds; the
  controller's overall run-exit marker is deliberately absent — do not invent
  one (`old-base-controller/failure.txt` records the assertion).
- New repaired `101598d2` base/root native builds: clean/all PASS (exit 0) but
  full native suite FAILED at the architecture-doc link check (native exit 2,
  run exit 1) — an input-packaging mistake (archive omitted tracked results the
  docs reference), NOT a production-code failure and NOT a native pass.
- Final restoration of installed f52 service: PASS exit 0 (5 image hashes, 4
  daemons, 2 clients, ports 17432–17435, 36 payload hashes verified).

## Source commit and build identity

- Frozen source: `101598d2f2d0ce9ac99ab59f840130f3c8f0e218` (branch devel-nuc);
  archive `efs-catchup-101598d2.tar.gz` SHA256 `fa633c0f…1bffa`; build IDs
  `101598d2-w52-nuc` / `101598d2-root-nuc` (`native-base/provenance.json`).
- Installed service remains `f52cd2dd-nuc` (efsd sha256 `952e5b94…`, built
  2026-10-11T18:55:50Z; `final-restoration/verified-service.json`).
- Old active fixture used `ba73f304-w52-nuc` (superseded; contains shared W112
  defect — old code acknowledges corrupted full-overwrite contents).
- `merge-ready-commits.txt`: 189 commits ahead of common base `48a2719d`.

## Environment

NUC host (owner-exclusive). Regular 4-node service on ports 17432–17435
(normally stopped during private fixtures); private 4-voter fixture on
127.0.0.1:26590–26593, TCP/direct I/O, 1G quota each; private stores under
`/data1/efs/evidence/gc-direct-dly7jx6q` (n1/n2) and `/home/efs/gc-direct-j16ejmaf`
(n3/n4); mounts `/data1/efs/mnt`, `mnt2`. Commands disabled, flow control=1,
early-retire/overlap/noop/group-drain=0, applied interval 10 ms.

## Method

Preserved scripts: `native-build-staging.py` (isolated native build/test
controllers 145724/145725) and the original base full-live controller (129781)
running `tests/live/gc_reclamation.py` from
`/data1/efs/devel-nuc-20261011/catchup-base-src` (full-direct, long-direct 20k,
then a held empty buffered fixture — see `empty-buffered-stop.json`). Execution
session 2695 guarded the stop; controller then restored the installed f52
service and a read-only audit verified images/ports/payloads
(`final-restoration/`). Free space at capture: SATA 207 GiB, NVMe 463 GiB.

## Results

| Gate | Verdict |
|---|---|
| ba73 full direct (POSIX 216/peer 67/2k) | PASS exit 0 |
| ba73 direct 20k | PASS exit 0 (writes 5534.208 s, fsync 0.283547 s, SHA `1075295d…47d4`) |
| ba73 empty buffered fixture | interrupted pre-write (exit -2), no overall run marker |
| 101598d2 base/root clean+all | PASS exit 0 both variants |
| 101598d2 full native | FAIL exit 2 at doc link check — input packaging, not code |
| f52 service restoration + audit | PASS exit 0 |

## Limitations and remaining gates

- W52/W97/W112 all remain open; no repaired image was installed or admitted;
  mounted ordinary/full/20k/cold/W97-ABBA gates for `101598d2` are NOT RUN.
- The native FAIL is a packaging error; merged qualification plan requires a
  complete archive and full rerun on the manager-approved input.
- Do not restart the held controllers (129782–129785) unchanged: original
  inputs carry the shared W112 bug.

## Evidence index (paths relative to run dir)

- `native-base/run-exit.txt` / `native-root/run-exit.txt` — 4355a46b19d348dc (both 1)
- `native-base/native.log` — 060b816f4ee9459a (doc-gate failure detail)
- `native-base/provenance.json` — 9a4b8c8bebcf94e1 (build identity/archive SHA)
- `old-base-controller/full-direct-exit.txt` — 9a271f2a916b0b6e; `long-direct.log` — 1db5b360c598e7d6
- `old-base-controller/full-buffered-exit.txt` — 4b883e04ed1a2af3; `failure.txt` — cebaea48e46e3159
- `final-restoration/restore-exit.txt` — 9a271f2a916b0b6e; `verified-service.json` — 0e5103130d04aa1f
- `merge-ready-commits.txt` — 418b3c5b56779ccc; `native-launch.json` — 60f3c81016952773
- `empty-buffered-stop.json` — 8d407845cd162d36; `frozen-input.json` — cb1ff90aca5b8fad

**Raw evidence note (2026-10-23):** the bulk data in `gc-direct-dly7jx6q` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
