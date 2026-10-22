# SUMMARY — results/measure/20260930-060000-dd-wedge

Wedged mount after a 100 GiB dd: steady 1.0–1.2 GB/s for 106 s, then close
EIO; sha256sum ENOENT; `ls` EBUSY; unmount reports metadata flush still
failing (rc=-13) after 60 s.

## Purpose and linked item

- The Sep 30 06:00Z wedge: 100 GiB `dd bs=1M` to `/tmp/efs-mount/001/dat01`
  dies at close. Sibling analyses: `20260930-063500-perf-dir-review`,
  `20260930-051000-review2`.

## Verdict

**FAIL** (defect demonstrated): close REPORT of 819200 records (every chunk)
returned BUSY after 18.6 s (`report-loop ino=112106 rounds=1 busy=1 ms=18582
rc=-13`), then 16 BUSY/STALE retries exhausted → EBUSY; unmount:
`UNMOUNT DATA LOSS: metadata flush still failing (rc=-13) after 60s`.

## Source commit and build identity

Servers `e002771e56e4-dirty` (rolled 05:43Z); client fstor007
`client.sh --perf` on the same tree.

## Environment

fcstor003–006 servers, fstor007 client; before the dd, ls/rsync/ecrawl/find/du
were all fine (01:50–01:56 local, `other-tools.txt`).

## Method

100 GiB dd bs=1M (`dd_1m.out.txt`), then sha256sum and ls as probes; unmount
attempt logged.

## Results

| phase | outcome |
|---|---|
| dd 100 GiB | 106 s at 1.0–1.2 GB/s, then close EIO |
| sha256sum | openat ENOENT after 33.5 s |
| ls /tmp/efs-mount | EBUSY after 16.7 s |
| unmount | metadata flush failing rc=-13 after 60 s |

## Limitations and remaining gates

The close-time whole-file REPORT (819200 records in one batch) is the wedge
mechanism — later REPORT batching/budget work (W17.1/D23/W41) addresses it.

## Evidence index

- `SUMMARY.txt` — timeline + report-loop lines (run `shasum -a 256 SUMMARY.txt
  | cut -c1-16` here)
- `dd_1m.out.txt`, `client-log.txt`, `other-tools.txt`
