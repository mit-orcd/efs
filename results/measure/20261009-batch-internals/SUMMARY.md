# SUMMARY — 20261009-batch-internals

## Purpose and linked item

W93 internal durability batching and queue measurement on the NUC cluster, using
opt-in diagnostic instrumentation (commits `ebe72fdb`, `9fd7862e`). Status page:
`docs/status/batch-internals-profile-20261009.md`; also referenced from
`docs/status/catalog.md` and `docs/backlog/work-items.md` (W93). Added to
raw-evidence by commit `511cce9a1`.

## Verdict

NOTRUN as an optimization-acceptance gate — this is an observational run, and
W93 optimization acceptance is explicitly not closed. All supporting validation
PASSes: Raft/store tests off and on, the 3/1/1 cohort/rotation observer oracle,
native test recipes (via a documented continuation after a staging
interruption), cooperative loader stop (exit 0), and a 32-file cold peer-read
smoke. Material open finding: n3 logged at least ten inbox-full dropped
replication frames at workload start (log suppresses further drops; total
unknown) — a pressure/retry issue to investigate, not proof of corruption.

## Source commit and build identity

- Source: instrumentation `ebe72fdb` + monotonic-timestamp fix `9fd7862e`,
  branch `devel-find-stats`; HELLO identity `9fd7862e-batchobs`.
- `deployed-version.txt`: `efsd 9fd7862e (branch devel-find-stats, built
  2026-10-09T06:29:11Z on nuc)`.
- `deployed-sha256.txt`: efsd `6c866dbfc37cb6b4…`, efs-fuse
  `7e54acf46b9a1a96…`.

## Environment

- NUC cluster, 4 efsd daemons on 127.0.0.1:17432–17435, join topology n2–n4 →
  n1; quota 187.5G each; TCP transport, direct I/O (`identity.json`).
- Storage: n1/n2 on SATA (`/data1`), n3/n4 on NVMe (`/data2`).
- Both mounts with `.efs` enabled; `EFS_BATCH_OBS=1` during capture only.
- Fixture: fresh `tiny-batch-internals-20261009` tree; existing data and the
  old 50,000-file tree retained.

## Method

Scripts retained in the run dir: `deploy.sh` (clean stop, isolated-build
install, restart), `capture.py` (5 s observation sampling), `loader.py`
(wrapper preserving the original `scripts/tiny_files.py` payload/folder RNG),
`analyze.py` (reproduce with `python3 analyze.py`; endpoints in
`analysis.json`). Workload: 8 workers, max depth 8, up to 1000 files/folder,
max 50,000 files, 1–512-byte payloads, 300 s window 06:34:13–06:39:13 UTC,
61 host/process/disk samples. Cooperative stop via SIGUSR1 at file boundaries.
Cleanup: collector verifies loader exit; cluster restarted with
`EFS_BATCH_OBS=0` (final state in `final-*.json/txt`); new data left at
`/data1/efs/mnt/tiny-batch-internals-20261009`; old binaries under
`/data1/efs/batch-internals-20261009/rollback`.

## Results

| Node | Active proposers mean | Shared hold mean | Nonempty inbox msgs mean | Entries/WAL barrier | Mean barrier section |
|---|---:|---:|---:|---:|---:|
| n1 / SATA | 1.008 | 12.0 us | 1.09 | 1.011 | 0.143 ms |
| n2 / SATA | 1.180 | 112.4 us | 1.15 | 1.170 | 0.155 ms |
| n3 / NVMe | no samples | no shared holds | 29.71 | 2.114 | 5.835 ms |
| n4 / NVMe | no samples | no shared holds | 5.06 | 1.409 | 5.341 ms |

- Batch boundaries: 81.86% of n3 host segments end on group switches; 3.86%
  reach the 16-message cap; none reach the 1 MiB byte cap.
- Every eligible cross-request follower batch on n2/n3/n4 holds exactly one
  request; 70.26% (n3) / 55.23% (n4) end on ineligible messages; rejections
  are dominated by empty requests and tail mismatches (no term/config/
  admission/entry/byte rejections).
- Assembly vs persistence: n3 11.8 us assembly vs 5.47 ms total; n4 14.2 us
  vs 5.77 ms.
- Rate decline over three 100 s windows: 93.60 → 52.19 → 38.88 files/s while
  the follower queue stays backlogged.
- Confound: unchanged `.efs` catalog cap (W92) causes repeated n2 refresh
  rc=-2 at 1024 MiB during the run.

## Limitations and remaining gates

- No throughput improvement or hardware ceiling is proven; instrumentation is
  not overhead-calibrated and restart changed leader/traffic placement, so no
  matched A/B gain claims.
- Tail mismatches are not classified behind/ahead/term-different; total frame
  drops unknown (rate-limited logging).
- No new POSIX/peer suite or hardware power-loss gate claimed. Next steps per
  README: receiver-pressure/group-interleaving investigation, per-boundary
  rejection attribution, bounded coalescing policy comparison on identical
  fixtures.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 9b513240580849b7 |
| deployed-version.txt | a508592788f25740 |
| deployed-sha256.txt | cb99022a32504ada |
| identity.json | 6db2c6aec8c98556 |
| loader-command.json | f5adbd6c066006aa |
| loader-exit.json | 6e7d9c7a8222ffbb |
| analysis.json | 9b76f36f8f9d1469 |
| rates.json | 2abd4a2650a3d6b9 |
| complete.json | eb9a76324c8b84a2 |
| cold-peer-smoke.json | f8bf6bffa6ffb1ea |
| validation/batch-obs-remaining-tests.log | a5834f4bceff2ad6 |
