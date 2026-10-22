# truncate-big-20261002-012442 — W43 truncate_big gate (step b state)

## Purpose and linked item

Gate run of `tests/stress/truncate_big.sh` for **W43 — `truncate`/
`O_TRUNC` of a file with > 32 chunks in a lane is a silent no-op; the
apply answers OK on NOMEM** (`docs/backlog/work-items.md` W43;
`docs/archive/project-history.md` Oct 2 2026 01:20Z handoff: "posix2
63/63, `tests/stress/truncate_big.sh` fcstor010 **exit 3**
(`results/stress/truncate-big-20261002-012442`: all four truncates
returned EIO, every file unchanged, 0 lies)"). This run captures the
expected intermediate state after W43 step b (failed apply now surfaces
EIO instead of lying) and before D25 (logical truncation + background
reclamation) made big truncates succeed.

## Verdict

**PASS** for the step-b "no lie" gate: script exit 3 (`TRUNC_ERR`) —
all four cases returned EIO, every file kept its original size and md5,
0 lies (an exit 1 would have been the W43 lie: OK with old size/md5).
**NOTRUN / still open** for the D25 end state (exit 0, truncate actually
succeeds): chunk 100 rows were still present in every case.

## Source commit and build identity

Not recorded in the run directory. `docs/archive/project-history.md`
(Oct 2 2026 01:20Z) records that the gates in this window ran on the
`start740` cluster at build `141071a3e9ba-dirty`; the run timestamp
(2026-10-02 01:24:42Z) falls in that window. (Directory added to the
branch by commit `2b5a25df4` "… truncate_big gate"; archival metadata,
not the tested build.)

## Environment

- Cluster: fcstor003–006 `efsd`, leader `172.16.223.57:19810` (script
  default), FUSE mounted at `/tmp/efs-mount`; run executed from client
  fcstor010 per project-history (script default `HOST=fcstor007.ib`).
- The four test inodes (72951, 77047, 81143, 85239) sit on inode group
  2; servers logged `apply lane-fence rc=-2` ×4 on fcstor004–006, one
  per refused truncate, with no `apply truncate rc=` line (the fence
  fails before the inode-group entry is proposed).
- Fixture per case: fresh file under
  `/tmp/efs-mount/truncbig-20261002-012442/`; non-zero sources staged in
  `/dev/shm/truncbig/` on the client.

## Method

Script: `tests/stress/truncate_big.sh` (in the repo). Per case: create
the big file with `dd bs=1M conv=fsync`, then attempt the truncate,
then verify with `stat` size, `md5sum` read through `dd iflag=direct`,
and `efs-mgmt raft-getchunks <leader> <ino> 100` (chunk 100 = byte
12.5 MiB, past the target EOF) from `MGMT_HOST=fcstor003.ib`. Exit
codes: 0 = all PASS (post-D25), 3 = TRUNC_ERR (honest refusal, expected
until D25), 1 = the lie. Cases:

| Case | Big file | Rewrite |
| --- | --- | --- |
| `1g-same` | 1 GiB | `dd` 10 MiB of the SAME bytes with O_TRUNC |
| `1g-diff` | 1 GiB | `dd` 10 MiB of DIFFERENT bytes with O_TRUNC |
| `300m-diff` | 300 MiB (every lane > 32 chunks) | `dd` 10 MiB different |
| `1g-zero` | 1 GiB | `truncate -s 0` |

## Results

| Case | Create size | Truncate rc | Size after | md5 after | Chunk 100 row |
| --- | --- | --- | --- | --- | --- |
| 1g-same | 1073741824 | 1 (EIO on open-for-O_TRUNC) | 1073741824 (unchanged) | unchanged (`46272a99…`) | present (28 rows cis 100–127) |
| 1g-diff | 1073741824 | 1 (EIO) | 1073741824 (unchanged) | unchanged | present |
| 300m-diff | 314572800 | 1 (EIO) | 314572800 (unchanged) | unchanged | present |
| 1g-zero | 1073741824 | 1 (EIO on `truncate -s 0`) | 1073741824 (unchanged) | unchanged | present |

All four cases: `TRUNC_ERR`, file fully intact — exactly the expected
pre-D25 behavior. Script exit 3, 0 lies.

## Limitations and remaining gates

- Big truncates still fail with EIO; the D25 gate (exit 0 with gates
  t1–t9: retained prefix, zero tail after re-extension, sweep vs
  new-epoch writes, restart mid-sweep, apply bound, durable truncation
  history) remained open at this date.
- Oct 6 note (recorded in `docs/backlog/work-items.md` W43): two
  lane-spanning truncates in flight make a REFUSED file's reads fail
  with EIO for ~1 s while the lane settles; the posix
  `truncate_big_*_honest` tests were made `@serial` with read retries to
  keep the lie gate deterministic. Whether that transient EIO is
  acceptable was open.
- Run captured client-side outputs only; server-side evidence
  (`apply lane-fence rc=-2` lines) is described in project-history, not
  in this directory.

## Evidence index

| File | sha256 (first 16) |
| --- | --- |
| `summary.txt` | 61191948a38e3acf |
| `1g-same.create` | 6ca8d2dfdb2484b3 |
| `1g-same.truncate` | ba627f0a3bd2e8f1 |
| `1g-same.chunk100` | 3530d4ed3f6fe326 |
| `1g-diff.truncate` | 9ddd86fd6f5540e8 |
| `1g-diff.chunk100` | ffe800c50b004385 |
| `300m-diff.truncate` | 2bab2bde9e03dc3e |
| `300m-diff.chunk100` | 10039628963e3abd |
| `1g-zero.truncate` | d5ade5db5bb1e4e3 |
| `1g-zero.chunk100` | fcb39a46cc04bf68 |
