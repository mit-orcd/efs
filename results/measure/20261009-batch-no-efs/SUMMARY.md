# SUMMARY — 20261009-batch-no-efs

## Purpose and linked item

W93 control run: same internal-batching/queue measurement as
`20261009-batch-internals`, but with the synthetic `.efs` catalog fully
disabled (`EFS_COMMANDS_SERVER=0` on all servers, `EFS_COMMANDS=0` on both
mounts), to test whether enabled `.efs` refresh is a necessary condition for
the observed throughput decline. Status page:
`docs/status/batch-no-efs-profile-20261009.md`; W93 in
`docs/backlog/work-items.md`. Added to raw-evidence by commit `6431dd92b`.

## Verdict

NOTRUN as an optimization gate (observational control). The measured finding:
the rate decline persists without catalog work — 118.40 → 81.69 → 42.37
files/s over three 100 s windows — so enabled `.efs` refresh is ruled out as a
necessary condition for this run's decline. Validation PASSes: Raft and
durable-store tests with diagnostics off and on; cooperative loader stop
(exit 0 at 300.565 s); final state retains `.efs=0`, diagnostics=0,
TCP/direct I/O; both cold mounts agree on 32 sampled payloads. n3 dropped
112,982 inbox frames during the run — quantified here by the new counters,
still an open pressure finding, not corruption evidence.

## Source commit and build identity

- Source diagnostic commit `c227bcbd`, build ID `c227bcbd-batchobs`,
  branch `devel-find-stats`.
- `deployed-version.txt`: `efsd c227bcbd (branch devel-find-stats, built
  2026-10-09T13:02:09Z on nuc)`; binary SHA256s in `deployed-sha256.txt`.

## Environment

- Same NUC cluster as the batch-internals run: 4 efsd daemons,
  127.0.0.1:17432–17435, quota 187.5G each, TCP transport, direct I/O
  (`identity.json`, `disabled-environment.json`).
- `.efs` disabled everywhere; `EFS_BATCH_OBS=1` during capture; existing
  cluster data retained.
- Fixture: fresh `tiny-batch-no-efs-20261009` directory; final output 24,545
  files in 58 populated directories.

## Method

Same harness as `20261009-batch-internals` (scripts in run dir: `deploy.sh`,
`capture.py`, `loader.py`, `analyze.py`; `python3 analyze.py` reproduces
`analysis.json`). Workload: original `scripts/tiny_files.py` parameters,
8 workers, 300 s capture 13:04:59–13:09:59 UTC, 5 s samples and counter
dumps, cooperative SIGUSR1 stop. New counters in this build classify tail
mismatch behind/ahead/equal-index term, attribute active batch endings once,
and count every dropped inbox frame. Node-1 startup exceeded the script
timeout; an idempotent retry completed startup after replay. Raw node logs
kept as lossless gzip; rollback binaries remain on NUC (not in repo).

## Results

| Node | Entries/WAL sync | Mean sync section | Nonempty inbox msgs | Dropped frames |
|---|---:|---:|---:|---:|
| n1 | 1.001 | 0.101 ms | 1.07 | 0 |
| n2 | 1.165 | 0.113 ms | 1.11 | 0 |
| n3 | 2.206 | 5.226 ms | 31.69 | 112,982 |
| n4 | 1.474 | 4.570 ms | 4.45 | 0 |

- Per-window rates: **118.40 → 81.69 → 42.37 files/s** (`rates.json`).
- n3 mean sync sections rise 3.211 → 8.443 → 6.634 ms; n4 2.870 → 7.272 →
  6.100 ms; n3 mean inbox depth 30.04 → 252.58 → 14.94; drops per window
  55,103 / 42,638 / 15,241.
- Every measured nonempty tail mismatch is **behind** the local tail (n2
  16,461; n3 84,450; n4 60,013); ahead and equal-index term mismatches are
  zero. These are eligibility checks (incl. rechecks), not unique operations.
- Active cross-request batches contain one request each.

## Limitations and remaining gates

- Control for catalog work only — not a matched randomized A/B (restart
  changed leaders and path distribution; instrumentation overhead
  uncalibrated). Does not exclude additional catalog overhead when enabled,
  nor establish the historical decline's exact cause.
- Does not isolate device service time from scheduling, filesystem,
  concurrent I/O or replication-retry feedback; `disk-intervals.json` is
  device-wide, not isolated SSD latency.
- Behind-tail traffic must still go through Raft's normal matching/ACK path;
  do not discard merely as ineligible. W93 optimization acceptance remains
  open.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | bf3ebfe21a90d2ed |
| deployed-version.txt | 5135ef6fa7cff533 |
| deployed-sha256.txt | c91f67480988724f |
| identity.json | 99b0ed02dd1d4226 |
| loader-command.json | 2c29fda4293368b1 |
| loader-exit.json | e4ba89528d7d5c7f |
| analysis.json | e94fcb10e65f6b89 |
| rates.json | 9effa437d5159d37 |
| validation.txt | eb353eb4506a541c |
| disabled-environment.json | c03cfcf3b0051ccb |
| cold-peer-smoke.json | 6ecf5df1c8a0cb67 |
| disk-intervals.json | 256958ae34813003 |
