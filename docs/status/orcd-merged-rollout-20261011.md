# ORCD merged branch rollout — October 11, 2026

The user requested all current branch commits merged into `devel-orcd`, deployment to ORCD and testing. Starting at `346fe2da`, integrated `devel` (`48a2719d`), AMD (`aba08244`), Intel (`f9ffaa91`), Mac (`408043b6`) and NUC (`1658d628`). The capacity branch (`60f36282`) and fetched origin refs were already ancestors. Integration tip `ca9edea3` includes 81 newly reachable commits. No other worktree was reset or modified.

Resolved the Intel/AMD LOOKUP overlap by retaining transaction-aware foreign-coordinator forwarding and reusing only the current request's covered parent-group ReadIndex. Session participant fencing retains the storage barrier. Preserved the query-delta test targets and regenerated architecture views. The first complete Linux unit gate exposed the old mock interface in `test_lookup_read_group.py`; repair `c52a4643` adds the combined reader interface and missing-coordinator case. The repaired exact snapshot passed full Linux `make test` and builds on all eleven hosts.

Deployment preserves stores and prior sources at `/tmp/efs-before-orcd-merged-c52a464391a6`. The original screen benchmark stopped cleanly at 9,514,331 files, 29,951.3 seconds, 318 files/s cumulative. Seven clients drained before stopping any of the four daemons. Runtime settings remain files backend, direct I/O, native mlx5_2 RDMA and directory-local commands enabled.

The first mount attempt preceded server readiness and failed discovery. Startup was traversing the existing fragment inventory before listening; a separate readiness wait follows. Record this as an initial mount failure, not a passing mount. Do not format or replace stores to bypass cold recovery.

The current benchmark script is frozen at `~/orcd/scratch/efs/orcd-test/tiny_files-merged-c52a464391a6.py`, SHA-256 `3b05d2f8f5e08c16a6b6a04f3b79bef1b2763326f9e555d5f681c31547d3f850`. It writes fixed 512-byte readable payloads. The earlier running process originated before the script changed; these rates are not a matched old/new performance acceptance claim. New runs use a distinct directory and keep 16 workers, depth 8, up to 1,000 files per folder and the ten-million-file target.

Small evidence: [preflight](../../results/measure/20261011-orcd-merged-rollout/preflight.txt), [unit gate](../../results/measure/20261011-orcd-merged-rollout/full-linux-unit.txt), [deployment](../../results/measure/20261011-orcd-merged-rollout/deployment.txt), [mount readiness](../../results/measure/20261011-orcd-merged-rollout/mount-readiness.txt). Bulk build/unit/runtime logs remain under `~/orcd/scratch/efs/orcd-test/` on ORCD.

All four servers report the deployed revision and healthy cluster membership; all seven mounts use the rebuilt clients. Commands policy remains mask 3/version 2. `/001/dat01` still has its original 100 GiB size, owner/group, mode and four sampled 64 KiB SHA-256 values. This is sampled integrity, not a full-file checksum.

| Gate | Result |
| --- | --- |
| Eleven exact-snapshot builds | Pass |
| Complete Linux `make test`, `c52a4643` | Pass |
| Four-server health and seven rebuilt mounts | Pass |
| Retained 100 GiB file attributes/sample hashes | Pass |
| Seven-client all-suite, default 15-second minimum | 1,430 pass, 61 fail, 7 skip, 49 NOTRUN |
| Seven-client all-suite, original 30-second minimum | 1,475 pass, 23 fail, 7 skip, 42 NOTRUN; only fcstor007 has complete marker |
| Mounted long/numeric listings | Both reached the 60-second command deadline |
| Mounted JSON listing | Fails EBUSY; no owner-format gate closure |
| Cross-client suite, fcstor007/fcstor009 | 66/67 pass, 1 fail, complete; concurrent-append 15-second step deadline failed |
| Fresh tiny-file benchmark | Running: 85,963 files/271 seconds, 317 files/s cumulative at 12:58:22 UTC |

The minimum case budget does not shorten longer decorated budgets. Keep both first-run and comparable-deadline results. Failed virtual queries and deep-path timeouts reproduce the W109 symptom class; many cases killed after a stuck worker are cancellation consequences rather than independently attributed bugs. Recent server logs show compaction and repeated lane-sweep `rc=-13` on inode 10454092; correlation does not establish causality. No EBUSY mapping, deadlines or test assertions were weakened. W108's deployed formatter is present, but the live listing gate is blocked by query failure. W109/W110 acceptance remain open.

A process audit found no surviving workers from either seven-client suite; the active peer steps were accounted for. All eleven EFS processes run as ORCD account `erbmi1` (UID 246111), select RDMA/`mlx5_2`, and the four daemons retain explicit direct-I/O arguments.

A focused runner repeated both deep-path tests with the same 30-second per-case minimum: native storage passed both in 0.2 seconds total; one EFS runner passed both in 35.1 seconds total while the benchmark remained active. Per-case deadlines still applied. This supports a loaded latency problem rather than a universal inability to address deep paths; it is not a passing seven-client gate. [Control](../../results/measure/20261011-orcd-merged-rollout/deep-control.txt).

Additional evidence: [peer verdict](../../results/measure/20261011-orcd-merged-rollout/peer.tsv), [process settings](../../results/measure/20261011-orcd-merged-rollout/runtime-settings.json), [retired suite workers](../../results/measure/20261011-orcd-merged-rollout/worker-audit.json), [postflight](../../results/measure/20261011-orcd-merged-rollout/postflight.json), [test launch](../../results/measure/20261011-orcd-merged-rollout/test-launch.json), [counts and completion](../../results/measure/20261011-orcd-merged-rollout/posix-summary.json), [30-second failures](../../results/measure/20261011-orcd-merged-rollout/posix-failures-30s.json), [benchmark checkpoint](../../results/measure/20261011-orcd-merged-rollout/benchmark-checkpoint.txt). Bulk runs are `~/orcd/scratch/efs/orcd-test/merged-orcd-c52a464391a6/` and `merged-orcd-c52a464391a6-30s/`.
