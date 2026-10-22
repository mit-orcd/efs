# SUMMARY — results/measure/20260927-w11-gate

W11 gate: repeated server bounce + frozen-posix-recovery checks, final gate
state recorded after the W11 chunked-InstallSnapshot roll.

## Purpose and linked item

- **W11** (chunked InstallSnapshot) gate evidence: bounce and freeze checks,
  posix gate outcome history. Added by `718be2fb0` ("raft: truncate the log
  with a chunked snapshot file").

## Verdict

**PASS** (current gate state): bounce check `bounce5.txt` OK=1 (efsd restart
510 ms, both groups FOLLOWER, commit==applied, raft.log 4796 B, apply_max=0);
metadata latency recovered (`mdlat.txt`: mkdir med 3.0 ms, create+close med
53 ms). Recorded history: 20260927-015843 FAIL (one rmdir EBUSY on fcstor012,
nlink=2 want 3, arc_term_miss 0→3) → 20260927-020111 PASS (9/9 hosts,
children=0, rmdir ok, no txn-recover ABORT).

## Source commit and build identity

Not recorded in the run (gate status files only).

## Environment

fcstor003/004 raft status (g0 leader fcstor004, term 6965; g2 leader node 3),
plus fcstor012 posix-freeze evidence.

## Method

`bounce5.txt` server-bounce gate, `mdlat.txt` metadata latency probe,
`posix-freeze.txt` frozen-suite rmdir/nlink checks, `status.txt` raft state.

## Results

| check | result |
|---|---|
| bounce5 | OK=1 (510 ms restart, commit==applied) |
| mdlat mkdir med | 3.0 ms |
| mdlat create+close med | 53.0 ms |
| posix-freeze history | 015843 FAIL (rmdir EBUSY, nlink mismatch) → 020111 PASS |

## Limitations and remaining gates

Point-in-time gate evidence; the W11 roll's wider qualification is the
20260927-033723 clean gate (200/201 all nine hosts).

## Evidence index

- `bounce5.txt`, `mdlat.txt`, `posix-freeze.txt`, `status.txt` (run
  `shasum -a 256 <file> | cut -c1-16` here)
