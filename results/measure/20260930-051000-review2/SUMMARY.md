# SUMMARY — results/measure/20260930-051000-review2

Second post-fix review of the 05:06Z roll (`06916bc7e5c1-dirty`: F1, F2, mtime
flush) under the user's fstor007 load.

## Purpose and linked item

- Follow-up of `20260930-040600-postfix-review`: verify the F1/F2 + mtime-flush
  roll under real client load (find, ecopy --verify, ecrawl, rsync).

## Verdict

**PASS** (review completed): client perf + strace reductions and server log
reductions captured; findings ordered by cost in SUMMARY.txt (see file).

## Source commit and build identity

`06916bc7e5c1-dirty` (F1, F2, mtime flush); client fstor007 `client.sh
--perf`; servers without recorders.

## Environment

fcstor003–006 servers, fstor007 client (perf record + strace), RDMA; review
window 05:10–05:19Z.

## Method

Inputs: `client-log-last.txt` (293-line fuse log), `client-perf-flat.txt`
(252K samples), `client-perf-callers.txt`, `strace-reduce.txt` (per-syscall
reduction), `server-logs.txt` (efsd.log reductions fcstor003–006),
`ib-hw-counters-0530Z.txt` (mlx5 hw counters delta base).

## Results

Findings are ordered by time cost in SUMMARY.txt — the numbered list there is
the conclusion set for this roll.

## Limitations and remaining gates

The ecopy metadata-mismatch thread continues in
`20260930-080000-ecopy-times-review` (four client bugs) and
`20260930-044100-mtime-utimens-close`.

## Evidence index

- `SUMMARY.txt` — ordered findings (run `shasum -a 256 SUMMARY.txt |
  cut -c1-16` here)
- `client-perf-flat.txt`, `client-perf-callers.txt`, `strace-reduce.txt`,
  `server-logs.txt`, `ib-hw-counters-0530Z.txt`
