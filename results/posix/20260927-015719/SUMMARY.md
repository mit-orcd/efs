# SUMMARY — results/posix/20260927-015719

9-host POSIX suite run during the Sep 27 2026 chunked InstallSnapshot rollout.

## Purpose and linked item

- Linked item: **W11 — chunked InstallSnapshot** (`docs/backlog/work-items.md`,
  DONE Sep 27). Per `docs/archive/project-history.md` (Sep 27 2026), this is the
  first 9-host posix run after `roll_efsd.sh --all` of the sorted-diff
  `efs_kv_group_import` fix (`src/kv/kv_snap.c`): "9-host posix was 199–200/201
  on the second run (`results/posix/20260927-015719`), not the clean 200/201 of
  `20260926-164123`". The clean rerun that met the W11 bar is
  `results/posix/20260927-033723`; this run is the earlier, not-clean one.
- Added to the branch by commit `718be2fb0` (2026-09-26, "raft: truncate the
  log with a chunked snapshot file").

## Verdict

**FAIL.** 4 of 9 EFS client hosts had one failing test each (single-test,
different test per host); the other 5 hosts were clean 200/201. The XFS
baseline did not run (mount missing, wrapper timeout), so the per-host
`compare-*.txt` files carry no comparison verdict. The W11 gate itself was
later met by rerun `20260927-033723`; this run's failures are unresolved
within the run.

## Source commit and build identity

Not recorded in the run (no deployed-version/identity files). Project-history
associates the Sep 27 W11 rollout with running build `612ee9ba9202-dirty`,
transport TCP.

## Environment

- EFS clients: fcstor007–fcstor015 (9 hosts), FUSE mount `/tmp/efs-mount`,
  suite start 2026-09-27T01:57:3xZ (per TSV headers).
- Baseline: node9901, XFS at `/data1/efs` — not mounted; baseline run errored
  and timed out.
- Transport: TCP (per project-history; "19810 stayed on TCP").
- Suite shape: 201 tests, `jobs=16` (196 parallel + 5 serial after),
  per-test timeout 15 s.

## Method

Per host, the suite runner `tests/posix/posix_suite.py` was executed under a
`run_tests` wrapper (exact command preserved in `xfs-baseline.tsv`):

```
PYTHONUNBUFFERED=1 timeout -k 5 385 \
  python3 /tmp/efs/tests/posix/posix_suite.py '<mnt>' \
  --timeout-s 15 --jobs 16 --results '<tsv>'
```

with a 400 s outer deadline; a timeout emits a placeholder TSV. Per-host EFS
results were then diffed against the baseline with `tests/posix/compare.py`
(`compare-fcstor*.txt`). Repo path of the suite: `tests/posix/posix_suite.py`.

## Results

| host | pass | fail | skip | dur (s) | failing test |
|---|---|---|---|---|---|
| fcstor007 | 199 | 1 | 1 | 13.6 | `content_random_roundtrip` (EIO) |
| fcstor008 | 200 | 0 | 1 | 14.0 | — |
| fcstor009 | 200 | 0 | 1 | 13.9 | — |
| fcstor010 | 200 | 0 | 1 | 13.9 | — |
| fcstor011 | 199 | 1 | 1 | 12.9 | `dir_many_files` (EIO) |
| fcstor012 | 199 | 1 | 1 | 14.8 | `dir_deep_nesting` (ENOTEMPTY) |
| fcstor013 | 200 | 0 | 1 | 13.5 | — |
| fcstor014 | 199 | 1 | 1 | 14.3 | `names_crazy_roundtrip` (EIO) |
| fcstor015 | 200 | 0 | 1 | 13.4 | — |

The skip on every host is `mmap_write_read` ("mmap unsupported: ENODEV").
Comparison/baseline: none usable — the XFS baseline produced no TSV, so every
`compare-fcstor*.txt` reports "not run: 1" and 0 classified pairs.

## Limitations and remaining gates

- Failures are one test per host on 4 hosts, three EIO and one ENOTEMPTY, on
  different tests — pattern looks flaky/environmental, but nothing in the run
  diagnoses them; they were superseded by the clean rerun `20260927-033723`.
- No XFS baseline comparison exists for this run (baseline mount missing).
- `mmap_write_read` remains skipped on EFS mounts (unsupported, known).

## Evidence index

| file | sha256 (16) | carries |
|---|---|---|
| `efs-fcstor007.tsv` | ad0d5e748a38bc03 | per-test results + FAIL/summary, fcstor007 |
| `efs-fcstor008.tsv` | 601337f159a75218 | clean-host results + summary |
| `efs-fcstor011.tsv` | 7a9967bd9a368281 | FAIL `dir_many_files` |
| `efs-fcstor012.tsv` | e035f7dc00a21d1c | FAIL `dir_deep_nesting` |
| `efs-fcstor014.tsv` | 8c68bde847ec2049 | FAIL `names_crazy_roundtrip` |
| `efs-fcstor015.tsv` | ea7a0263454e886c | clean-host results + summary |
| `xfs-baseline.tsv` | 7c4f28876a1656c6 | baseline failure + exact suite command |
| `compare-fcstor007.txt` | 2cc0e0068b16197f | degenerate comparison (no baseline) |

(`efs-fcstor009/010/013.tsv` and the remaining `compare-*.txt` follow the same
pattern; fcstor009/010/013 are clean 200/201.)
