# SUMMARY — results/perf/20260918-w4-honest4

W4 loaded-table rerun: 4-client fio after bounce/table growth, plus the 8 GiB
dd+fsync gate (W3/W4). Write throughput collapses vs the morning run; rw-1m
times out on the fsync/REPORT tail.

## Purpose and linked item

- **W4** (performance) multi-client gated rerun and the 8 GiB dd+fsync gate
  (`dd8g.sh`: 8 GiB per client, non-zero source, flush in the clock). Added by
  commit `30c41ee47` ("W6 correctness batch + the 100 ms metadata floor").

## Verdict

**FAIL** (gate incomplete): after the bounce/grown table, 4-client sw-1m fell
from AGG 1589 to 281 MiB/s and rw-1m **timed out on all 4 hosts (400 s
efs-ssh timeout, fsync/REPORT tail — do not raise the timeout)**; 4/9-client
fio reads and 9-client fio writes were not finished for that reason.

## Source commit and build identity

Not recorded in the run.

## Environment

4 clients fcstor007–010 (later 9 for dd8g, fcstor007–015), TCP 19810,
leftover-1 table (grown), FUSE_OK.

## Method

`dd8g.sh` (8 GiB dd+fsync, src=/tmp/src8g all-0x5a, own file per client,
remount HEAD/TAIL 0x5a verified on 007 and 013) and 4-client fio matrix
(4-ow/rw/sw-1m, 4-rw-128k).

## Results

| gate | result |
|---|---|
| 4-client fio sw-1m | AGG 281 MiB/s (was 1589 morning) |
| 4-client fio ow-1m | 240 MiB/s |
| 4-client fio rw-1m | TIMEOUT 400 s on all 4 hosts (fsync/REPORT tail) |
| dd8g 1-client (W3) | 13.501 s / 639 MiB/s = 3.8 % of 16.7 GB/s |

raw.tsv: sw-1m 4-client per-host 75.3 / 62.8 MiB/s (fcstor007/008).

## Limitations and remaining gates

Gate not completed (timeouts); numbers documented the pre-floor REPORT tail
cost that the 100 ms metadata floor addresses (`30c41ee47`).

## Evidence index

- `gate.txt` — gate narrative and numbers (run `shasum -a 256 gate.txt |
  cut -c1-16` in this dir)
- `dd8g.sh` — gate script
- `raw.tsv` — machine-readable matrix
- `dd8g-4-fcstor007.txt` … `dd8g-9-*.txt` — per-client dd output
