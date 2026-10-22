# SUMMARY — results/measure/20260930-063500-perf-dir-review

Review of the remaining perf captures (ls/ls -lart/rsync/ecrawl/find×3/du/
dd_16k) from the user's Sep 30 session; headlining: `du -hs` reported 52K for
3.83 TB of files because every regular file reports st_blocks=0.

## Purpose and linked item

- Completes the review of `~/orcd/scratch/efs/perf/efs-mount/` after the wedge
  analysis (`20260930-060000-dd-wedge`). The du/st_blocks finding is the
  sparse-accounting item (ecrawl agrees: total_allocated_bytes=0, 21150 of
  21503 files sparse-heuristic).

## Verdict

**FAIL** (defect demonstrated): du sums st_blocks; 52K for 3.83 TB is exactly
103 symlinks × 512 B — all regular files report st_blocks=0 (`find -ls` shows
0 blocks for the 100 GiB files too). Correctness bug in st_blocks accounting;
the rest of the directory's tools are reviewed per-tool in SUMMARY.txt.

## Source commit and build identity

Per the Sep 30 rolls (see postfix-review); not recorded separately here.

## Environment

fcstor003–006 + fstor007, RDMA, the user's perf session tree.

## Method

Per-tool strace/output review: `du.out.txt`, `find1/find2.out.txt`, straces;
times local UTC-4.

## Results

| tool | finding |
|---|---|
| du -hs | 52K for 3.83 TB — st_blocks=0 on regular files |
| ecrawl | total_allocated_bytes=0, 21150/21503 sparse-heuristic |
| find -ls | 0 blocks even on 100 GiB files |

## Limitations and remaining gates

st_blocks accounting repaired later (D17: the chunk row's existence counts);
verify against a later gate before citing as current.

## Evidence index

- `SUMMARY.txt` — per-tool review (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `du.out.txt`, `find1.out.txt`, `find2.out.txt` and the straces
