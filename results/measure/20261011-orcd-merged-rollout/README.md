# ORCD merged rollout evidence — Oct 11, 2026

Runtime `c52a464391a685a766ee49aa76a162de58162a81`, branch `devel-orcd`, four direct-I/O servers and seven native mlx5_2 RDMA clients. This snapshot integrates devel/AMD/Intel/Mac/NUC commits and repairs the combined LOOKUP test fixture. Full Linux units pass; loaded live acceptance does not.

See [rollout and limitations](../../../docs/status/orcd-merged-rollout-20261011.md). `posix-summary.json` counts exact rows and preserves completion markers. The 15-second minimum run and repeated prior 30-second minimum run have separate TSVs; longer per-case budgets remain. `peer.tsv` is complete: 66 pass, one concurrent-append step deadline fail. `deep-control.txt` records native/EFS focused tests at the unchanged per-case deadline. Failed virtual command probes do not close the owner-name rollout gate.

`preflight.txt` and `postflight.json` preserve 100 GiB file attributes and four sample hashes; this is not a full-file checksum. `worker-audit.json` accounts for the running peer steps and shows prior suite workers retired. `runtime-settings.json` records only selected EFS environment variables, process arguments and ownership.

Original unit failure log: `~/orcd/scratch/efs/orcd-test/fcstor006/unit-w95-ca9edea3ac61.log` (fixture compile failure). Repaired complete gate: `unit-w95-c52a464391a6.log`, rc 0. Bulk live suites: `merged-orcd-c52a464391a6/` and `merged-orcd-c52a464391a6-30s/` below the same scratch root.

Fresh benchmark remains running on fcstor007, PID 1564996, logging to `merged-orcd-c52a464391a6/fcstor007/tiny-files.log`. It uses a frozen fixed-512-byte script with SHA-256 `3b05d2f8f5e08c16a6b6a04f3b79bef1b2763326f9e555d5f681c31547d3f850`, 16 workers, maximum depth 8, 1,000 files per folder and ten-million target in `/tmp/efs-mount/tiny-files-merged-c52a464391a6`. The earlier process source was not frozen; no matched old/new throughput acceptance is claimed. Original screen run was interrupted cleanly and its data retained.
