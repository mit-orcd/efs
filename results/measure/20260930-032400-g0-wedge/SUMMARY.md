# SUMMARY — results/measure/20260930-032400-g0-wedge

Group-0 wedge: an rsync from fstor007 froze because group-0 follower reads
returned BUSY for 10 s per op while the apply loop slept 400 ms per entry.

## Purpose and linked item

- Wedge diagnosis (g0 apply stall) — client-visible EBUSY/ENOENT storm on
  2026-09-30 03:16–03:19Z; feeds the apply-wait redesign (later W17/W17.1
  budgeted retries).

## Verdict

**FAIL** (defect demonstrated): fuse.log shows LOOKUP (type 43) exhausting
16 BUSY/STALE retries (10.3 s) → EBUSY ×6 and GETATTR (type 47) ×5; rsync's
generator got ENOENT after 16.76 s five times and was ^C'd. Servers showed no
election/term change (g0 term 41, g2 term 261); fsync fine; fcstor003/004
efsd.log repeats `raft-host: apply-sleep us=400030 group=0 idx=437638
applied=437637`.

## Source commit and build identity

Not recorded in the run.

## Environment

fcstor003–006 servers + fstor007 client; group-0 reads BUSY on the two
followers (16 retries × 400 ms apply wait).

## Method

Live-capture during the freeze: `fuse-and-strace-tails.txt`,
`rsync-long-syscalls.txt`, `efsd-logs.txt`, `gdb-raft-state.txt`,
`raft-log-tails.txt`.

## Results

Client: LOOKUP/GETATTR EBUSY after 10.3 s retry budgets; rsync ENOENTs at
23:16:51–23:18:48 local, aborted by user. Server: apply-sleep 400 ms loop on
g0 with applied trailing commit by one; wait_timeouts 80/112 on fcstor004/003,
0 on fcstor005.

## Limitations and remaining gates

Fixed by later apply-wait/ReadIndex work; the gate evidence is the subsequent
clean gates (e.g. 20260927-033723 lineage, W11-gate status).

## Evidence index

- `SUMMARY.txt` — timeline + analysis (run `shasum -a 256 SUMMARY.txt |
  cut -c1-16` here)
- `fuse-and-strace-tails.txt`, `efsd-logs.txt`, `gdb-raft-state.txt`,
  `raft-log-tails.txt`
