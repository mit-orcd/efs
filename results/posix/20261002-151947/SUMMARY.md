# SUMMARY — results/posix/20261002-151947

Single-host POSIX suite gate run, Oct 2 2026 15:19Z, after the P0/P1
performance work and the W50/D26 closure.

## Purpose and linked item

- Linked items: **W50** (repeating GC record set — CLOSED, deletes were not
  stuck) and **D26** (GC scan cursor resumes past the last emitted key). Per
  `docs/archive/project-history.md` (Oct 2 2026): "15:19Z gate on that same
  server build (clients fcstor007–015 remounted, no wipe): posix jobs=1
  200/201 (`results/posix/20261002-151947`)", run alongside posix2 63/63
  (`results/posix2/20261002-152031`) and IO-500 9×4
  (`results/io500/20261002-152126-rdma`).
- Added to the branch by commit `33b04eec8` (2026-10-02, "W50 closed … and the
  D26 scan cursor resumes past the last emitted key").
- Note: the run dir captures only client fcstor008, although project-history
  describes a fcstor007–015 gate; the other hosts' TSVs are not in this run.

## Verdict

**PASS** (with one known skip). EFS on fcstor008: 200/201 pass, 0 fail, 1 skip
(`mmap_write_read`, "mmap unsupported: ENODEV"), 27.1 s. Comparison against
the XFS baseline: 0 EFS bugs (no baseline-PASS/target-FAIL pair), 0
not-equivalent; the compare tool's verdict string "not an unqualified success"
is solely due to the mmap skip, not to any failure.

## Source commit and build identity

Not recorded in the run (no deployed-version/identity files). Project-history
(Oct 2, 15:00Z) records the servers on `111a07527093-dirty`, built `--perf`,
clients remounted 14:57Z without a wipe.

## Environment

- EFS client: fcstor008, FUSE mount `/tmp/efs-mount`, suite start
  2026-10-02T15:20:30Z (per TSV header).
- Baseline: node9901, XFS at `/data1/efs`, start 2026-10-02T15:20:01Z.
- 201 tests; jobs count not recorded in the TSV (project-history says jobs=1;
  the 27.1 s duration vs ~14 s at jobs=16 on Sep 27 is consistent with that).
- Transport / direct-vs-buffered I/O / fixture size: not recorded in the run.

## Method

Suite runner `tests/posix/posix_suite.py` executed per host against the mount
(same runner family as the Sep 27 runs: `--timeout-s 15`, per-test budget
15 s); results diffed against the XFS baseline with `tests/posix/compare.py`
(output in `compare-fcstor008.txt`). No wipe of server state before the gate
(per project-history).

## Results

| target | pass | fail | skip | notrun | dur (s) |
|---|---|---|---|---|---|
| EFS fcstor008 | 200 | 0 | 1 | 0 | 27.1 |
| XFS baseline node9901 | 198 | 0 | 3 | 0 | 13.7 |

Comparison (201 tests):

| category | count |
|---|---|
| both pass | 197 |
| EFS bugs (baseline PASS, target FAIL) | 0 |
| not equivalent | 0 |
| target better | 3 (`virt_find_query`, `virt_find_not_a_real_dir`, `virt_stats_readable` — EFS-only virtual-dir features, SKIP on XFS) |
| target skipped | 1 (`mmap_write_read`) |

## Limitations and remaining gates

- Only one of the gated clients (fcstor008) is preserved in this run dir; the
  fcstor007/009–015 legs of the 15:19Z gate are not in the raw evidence.
- `mmap_write_read` remains skipped on EFS mounts (unsupported, known).
- The 3 `virt_*` passes have no XFS counterpart (feature absent on baseline),
  so they are unverified against a reference.
- Companion gates for the same build: posix2 `results/posix2/20261002-152031`
  and IO-500 `results/io500/20261002-152126-rdma`.

## Evidence index

| file | sha256 (16) | carries |
|---|---|---|
| `efs-fcstor008.tsv` | 0ec3b49d79a537b6 | EFS per-test results, skip reason, summary line |
| `xfs-baseline.tsv` | fc9db71e9c496b38 | XFS baseline results + summary |
| `compare-fcstor008.txt` | 277fe100d42fb3de | baseline comparison and verdict accounting |
