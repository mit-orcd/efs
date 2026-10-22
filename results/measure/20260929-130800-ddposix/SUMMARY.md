# SUMMARY — results/measure/20260929-130800-ddposix

DD + posix combined-workload capture: per-node efsd and fuse logs under the
mixed load, reduced into `ana/`.

## Purpose and linked item

- Mixed dd+posix workload analysis (same Sep 29 investigation window as
  `20260929-040800-idle-trace`); per-node daemon+client logs under load.

## Verdict

**PASS** (capture completed): per-node `log-fcstor0XX-{efsd,fuse}.txt` for
fcstor003–006 collected under `ana/`.

## Source commit and build identity

Not recorded in the run.

## Environment

Servers fcstor003–006 + fuse client(s), dd+posix concurrent workload,
2026-09-29 13:08Z.

## Method

Run dd and posix workloads concurrently, collect efsd and fuse logs per node
into `ana/`.

## Results

Logs-only capture; conclusions live in the Sep 29–30 analysis runs
(`20260930-040600-postfix-review` and siblings).

## Limitations and remaining gates

No score file; the capture supports the postfix-review analysis.

## Evidence index

- `ana/log-fcstor003-efsd.txt` … `ana/log-fcstor006-fuse.txt` (run
  `shasum -a 256 <file> | cut -c1-16` here)
