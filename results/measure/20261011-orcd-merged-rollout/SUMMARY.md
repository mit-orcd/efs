# 20261011-orcd-merged-rollout — evidence summary

## Purpose and linked item

ORCD merged-branch rollout: merge all current devel/AMD/Intel/Mac/NUC branch
commits into `devel-orcd`, deploy to the ORCD cluster and run live acceptance.
Linked status page: `docs/status/orcd-merged-rollout-20261011.md`; run
preserved by commit `de348f871` ("record merged rollout and failed loaded
acceptance gates"). Open items: W108 owner-name rollout gate (blocked by query
failure), W109/W110 acceptance. An existing `README.md` in the run dir records
the same rollout context.

## Verdict

**FAIL** for loaded live acceptance; unit/build gates PASS. Full Linux
`make test` passed on the repaired snapshot, but the seven-client full POSIX
suite under load failed at both 15 s and 30 s per-case minimums, and the
cross-client peer suite had one deadline failure. Failed virtual queries and
deep-path timeouts reproduce the W109 symptom class; W109/W110 gates remain
open.

## Source commit and build identity

- Deployed runtime `c52a464391a685a766ee49aa76a162de58162a81` on branch
  `devel-orcd` (per README and status page). Integration tip was `ca9edea3`;
  repair `c52a4643` fixed the combined LOOKUP test fixture.
- Fresh tiny-file benchmark script frozen with SHA-256
  `3b05d2f8f5e08c16a6b6a04f3b79bef1b2763326f9e555d5f681c31547d3f850`
  (recorded in README; script itself kept outside Git).
- No `deployed-sha256.txt` in the run; binary identity via revision reports
  in `postflight.json`/status page.

## Environment

- ORCD cluster: four `efsd` servers on fcstor003–fcstor006 (node-ids 1–4,
  172.16.223.57–60:17432), six storage dirs each, `--direct-io`,
  `EFS_MD_RAFT_N=4`.
- Seven native RDMA (`mlx5_2`) efs-fuse clients on fcstor007–fcstor013,
  mounted at `/tmp/efs-mount`; commands policy mask 3/version 2.
- Files backend; directory-local commands enabled. Quota ≈29.31 TiB/node,
  ~494.5 GiB used per node.
- Retained user file `/001/dat01`, 100 GiB (107374182400 bytes), verified by
  four sampled 64 KiB SHA-256 values pre/post (sampled, not full checksum).
- All eleven EFS processes run as ORCD account `erbmi1` (UID 246111).

## Method

Procedure and gate table in `docs/status/orcd-merged-rollout-20261011.md`;
bulk build/unit/runtime logs kept outside Git under
`~/orcd/scratch/efs/orcd-test/` (`merged-orcd-c52a464391a6/` and
`merged-orcd-c52a464391a6-30s/`). Sequence: build exact snapshot on eleven
hosts, run complete Linux `make test`, drain seven clients and redeploy four
daemons (stores preserved at `/tmp/efs-before-orcd-merged-c52a464391a6`),
then run the full POSIX suite on all seven clients concurrently at 15 s
minimum per case, rerun at the original 30 s minimum, run the cross-client
peer suite (fcstor007/fcstor009), and a focused deep-path control
(native + single EFS runner, unchanged 30 s budgets). A 16-worker tiny-files
benchmark (fixed 512-byte payloads, depth 8, ≤1000 files/folder,
10M-file target) ran concurrently as background load.

## Results

| Gate | Result |
| --- | --- |
| Eleven exact-snapshot builds | Pass |
| Complete Linux `make test` at `c52a4643` | Pass (rc 0) |
| Four-server health, seven rebuilt mounts | Pass |
| Retained 100 GiB file attrs/sample hashes | Pass |
| Seven-client suite, 15 s minimum | 1430 pass, 61 fail, 7 skip, 49 NOTRUN; no suite complete |
| Seven-client suite, 30 s minimum | 1475 pass, 23 fail, 7 skip, 42 NOTRUN; only fcstor007 complete |
| Cross-client peer (fcstor007/fcstor009) | 66/67 pass, 1 fail (concurrent-append 15 s step deadline), complete |
| Deep-path control (native / single EFS) | Both pass: 0.2 s native, 35.1 s EFS under load |
| Mounted long/numeric listings | Both hit 60 s command deadline |
| Mounted JSON listing | Fails EBUSY; owner-format gate not closed |
| Fresh tiny-file benchmark | In progress at capture: 85,963 files / 271 s, 317 files/s |

Failures are dominated by `dir_deep_nesting[_beyond_64]`, `names_crazy_dirs`,
`virt_find_query`, `virt_stats_readable` timeouts and workers killed by
SIGTERM after a stuck sibling (cancellation consequences, not independent
bugs). Server logs in the window show compaction and repeated lane-sweep
`rc=-13` on inode 10454092; correlation, not proven causality. The deep-path
control supports a loaded-latency problem rather than a universal deep-path
addressing failure. The initial mount attempt preceded server readiness and
failed discovery — recorded as an initial mount failure, not a pass.

## Limitations and remaining gates

- Loaded acceptance open; many 15 s failures are cancellation cascades from
  stuck workers, so per-case attribution needs the 30 s comparison.
- Benchmark old/new throughput comparison is explicitly not claimed (earlier
  process source was not frozen).
- File integrity is sampled hashes only. Bulk per-suite logs remain outside
  Git; this dir holds aggregates, TSVs and audits.
- Open: W108 owner-name gate (blocked by query EBUSY), W109/W110.

## Evidence index

| File | sha256 (16) | Content |
| --- | --- | --- |
| `posix-summary.json` | 49d8996256ec922d | Exact PASS/FAIL/SKIP/NOTRUN counts and completion markers, both deadlines |
| `posix-failures.json` | db34bc6041cbda5c | Per-client 15 s failure list |
| `posix-failures-30s.json` | e88ac2cf12e90496 | Per-client 30 s failure list |
| `peer.tsv` | a2691cac8adfb82e | Cross-client suite: 66 pass/1 fail, complete |
| `deep-control.txt` | cd2cffef89978378 | Native vs single-EFS deep-path control results |
| `runtime-settings.json` | 1a626f6a0e927d05 | Daemon argv/env (direct-I/O, RDMA mlx5_2) |
| `postflight.json` | 13c7060110945e3c | Retained-file samples, cluster status, commands policy |
| `preflight.txt` | 8125e081d0bf0daf | Pre-deployment 100 GiB file attributes/hashes |
| `full-linux-unit.txt` | c5b178efa2b7cedd | Repaired complete Linux unit gate record |
| `test-launch.json` | 52513289b8e04f90 | Runner PIDs per client plus benchmark PID |
