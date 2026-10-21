# 20260921-203405-w8-root — root/sub-dir mkdir burst after the txn-scan fix

## Purpose and linked item
W8 (9-node POSIX suite 1): re-test of root and sub-directory mkdir+rmdir
bursts after the txn resolve/drop scan fix (`165e779`). Verifies the 1.2–6 s
root mkdir stalls are gone and isolates the remaining fresh-parent hang.
Adding commit: `d5cbee67f` ("raft-host/client: always log BUSY/STALE dir-op
outcomes (rate-limited)").

## Verdict
Mixed: PASS for the root burst (9/9, 0.005–0.203 s; previously 6/9 with 3
hung), FAIL for the sub-dir burst (8/9; fcstor009 hung in D-state until the
10 s timeout killed it, rc=137). SUMMARY.txt corrects an initial misread: the
sub burst was 8/9, not 9/9 (the counter had matched the literal "MKDIR_OK"
inside the echoed killed command line; script fixed to anchor the match).

## Source commit and build identity
`165e779699cd-dirty` on fcstor003–006 (preflight.txt, 2026-09-21T20:34:05Z).

## Environment
Cluster 19810: efsd on fcstor003–006, g0 voters 0x7 / g2 voters 0xe,
commit==applied, idle at start. Clients fcstor007–015 on FUSE /tmp/efs-mount,
TCP transport. Driver: node9901.

## Method
Burst of parallel `mkdir`+`rmdir` of one fresh dir per host at the mount root
(mkdir-root-fcstor*.txt), then the same in a fresh sub-directory
(mkdir-sub-fcstor*.txt), driven from node9901 over ssh with a 10 s timeout.
one-root.txt / parent.txt are single-op sanity checks. Server thread states
sampled at 4 s and 9 s during each burst (sample-*.txt); raft state captured
before each burst (raft-before-*.txt); mounts remounted before the sub burst
and restored at the end (remount-*.txt).

## Results
| burst | ok/9 | latency | notes |
|-------|------|---------|-------|
| root | 9/9 | 0.005–0.203 s | was 6/9 with 1.2–6 s + 3 hung before `165e779` |
| sub | 8/9 | — | fcstor009 killed at 10 s timeout (rc=137, D-state) |

The single fresh-parent hang showed 16 BUSY/STALE retries on an idle cluster
with no server log line — logging for those outcomes was added in the next
build (`d5cbee67f`).

## Limitations and remaining gates
The fresh-parent sub-dir hang is unresolved in this run (next work item).
Sample snapshots (sample-*.txt) show all server threads idle in futex waits
during the hang. Superseded by later W8 sub-item closures (I17/I16).

## Evidence index
- `ed941eee2cfbe622` SUMMARY.txt — verdict and the 8/9 correction note
- `397f787de430d5db` preflight.txt — build identity and raft state
- `c2ccdf30e1ef8656` mkdir-sub-fcstor009.txt — the hung op (rc=137)
- `a0879a96f72c891f` one-root.txt — single root mkdir sanity (0.103 s)
- `1f7c64b73d1c236c` sample-root-4s.txt — server/client thread states
- `04876ebb834c9495` remount-final.txt — mount restore check
