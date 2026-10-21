# 20261010-amd-three-hour — AMD three-hour non-RDMA closure round

## Purpose and linked item
Three-hour exclusive-AMD closure round on branch `devel-amd` (based on devel
`746ae9f0`), started 2026-10-11 03:38:46 UTC (Oct 10 EDT) with a 06:38:46 UTC
deadline. Linked status page: `docs/status/amd-non-rdma-20261010.md`.
Work items: W89 (measurement defect), W66 (namespace coordinator routing),
W73/W95 documentation reconciliation, symlink request/test mismatch, W94
(combined first-descriptor adoption A/B + fault lifecycle gates).

## Verdict
FAIL overall (mixed round; failures preserved, not erased):
- **PASS** — W89 follower-cap/recovery gate: 2240 acknowledged 1 MiB writes,
  276 exact TSV/raw observations, InstallSnapshot recovery, cold-value checks.
- **PASS** — Baseline direct/TCP acceptance at 8cb41c929ca0: 216 core + 67
  peer POSIX, cold/replay/physical-GC gates.
- **PASS** — W66 routing repair: 64 competing mkdir races per I/O mode survive
  cold restart; focused regressions pass. (Closes the reproduced placement
  defect only; broader W66 obligations remain open.)
- **PASS** — W94 A/B: all 16 finite cases pass; medians +18.7% direct, +23.3%
  buffered; 131,072 exact cold files with physical-reclamation proof; durable
  Raft log shows 192 vs 128 committed creation/close lease commands per
  64-file control (3 vs 2 per file).
- **FAIL** — Complete live fault matrix: direct run aborts in
  `lease_edges --legacy-guard` (PREPARE insert of absent CLOSE_STEP marker,
  `owner_rpc` retry budget exhausted); buffered follow-up never started. Kept
  open; earlier individual passes are not a substitute.
- Preserved non-acceptance failures: initial direct FUSE mount exit
  (`direct.exit=1`), baseline buffered peer MKDIR EIO (`round.exit=1`, 66/67
  peer), native-ISA server Memcheck abort on unsupported AVX-512
  (`adopt-session-valgrind.exit=132`), symlink-isolation doc-gate failure
  (`fault-native-symlink-rejected.exit=2`), reproduced pre-fix
  combined-negative ownership leak (`combined-negative-before.exit=1`).

## Source commit and build identity
- Baseline: efsd `8cb41c929ca0`, branch devel-amd, built 2026-10-11T03:42:40Z
  on amd-395 (`evidence/build-identity.txt`); binary sha256s in
  `evidence/binary-sha256.txt`.
- W89 fault build: `cae522ff038a-fault` (`evidence/pressure-build-identity.txt`).
- A/B frozen builds: `93e8af7e-adopt-route` (combined) and
  `92fd2d62-separate-lease` (separate), built 2026-10-11T04:05–04:11Z.
- Fault gates: `9c7c7a28-retained-combined-fault`; final normal build
  `9c7c7a28-final-normal` passes `make -j8 all test` (`native.exit=0`).

## Environment
- Host `amd-395` (AMD cluster, exclusive access), one NVMe.
- Four private `efsd` processes, ports 24190–24193, TCP, file storage,
  separate fresh stores, direct or buffered I/O — functional coverage, not
  four physical failure domains.
- Installed stores/daemons (ports 17432–17435) preserved; FUSE mounts borrowed
  via drain-safe client script and restored with TCP and `.efs` commands
  disabled. No RDMA gates, no D25 activation.

## Method
- `w94-ab-run.sh`: four alternating separate/combined pairs per I/O mode;
  eight workers create/write/close 8192 deterministic nonzero 512-byte files;
  close durability enabled; per-case cold verification after private daemon
  crash/restart, then unlink + physical-reclamation check.
- `lease-proof.py --source SOURCE --root REMOTE_AB_ROOT`: decodes stopped
  private Raft stores with the production-log decoder, CRC-validates, bounds
  by persisted applied indexes.
- `source-proof.py`, `analyze-w94.py`, `inventory-proof.py` for identity,
  acceptance and physical-inventory proofs (repo: scripts live under the run
  dir / devel-amd tooling; `candidate-run.sh` at run root).
- Fault-only runner: deterministic consumed-reply and invalid-FileID faults in
  the real FUSE client; `final-native-run.sh` reruns the full native suite
  with `EFS_FAULTS=0` after restoration.

## Results
| TCP mode | Separate files/s median (min–max) | Combined files/s median (min–max) | Median paired gain |
|---|---|---|---|
| Direct | 280.0 (277.2–282.3) | 330.8 (323.9–343.6) | +18.7% |
| Buffered | 274.6 (270.8–278.3) | 337.1 (333.6–340.4) | +23.3% |

- Open p99 (median of per-case p99): direct 15.98 → 7.12 ms; buffered
  14.19 → 9.31 ms. Close p99: direct 110.57 → 102.95 ms; buffered
  113.71 → 96.16 ms.
- Committed lease commands per 64-file control: 192 separate vs 128 combined.

## Limitations and remaining gates
- Finite creation workloads only; no sustained-service or universal RSS bound.
  Percentiles are medians of per-case percentiles, not pooled.
- Legacy guarded-lease PREPARE timeout undiagnosed; full two-mode live fault
  matrix rerun outstanding. W94 full integration, W66 D36/leadership/
  migration/scale, D30/W23 bounds and RDMA acceptance remain open.
- Oct 11 audit correction: original lease-count script misread the 16-byte
  applied checkpoint; corrected reader re-proves 192 vs 128 (see
  `docs/status/amd-integration-20261011.md`). Invalid prior proof retained
  separately.

## Evidence index (sha256 first 16 chars)
- `README.md` `a14c08d90eb7a5a1` — round method and checkpoint write-up
- `evidence/historical-attempts.md` `4f71c259da1fa112` — preserved-failure ledger
- `evidence/w94-ab/comparison.json` `7766aade42184207` — 16-case A/B observations/summary
- `evidence/w94-ab/lease-consensus-proof.json` `e4c3539d96eb9bf3` — 192-vs-128 committed lease proof
- `evidence/pressure.log` `e4ed33a1beabc568` — W89 gate log
- `evidence/adopt-fault/direct.log` `a6b98cf79e22253a` — failed live matrix (legacy guard abort)
- `evidence/adopt-fault/round.exit` `4355a46b19d348dc` — live matrix exit 1
- `evidence/restoration-proof.json` `0fd62adfcb14b421` — final restoration audit
- `evidence/build-identity.txt` `57c949edf341c126` — baseline build identity
- `evidence/final-native/native.log` `5ed0ae3e20af05b9` — final EFS_FAULTS=0 native suite pass
