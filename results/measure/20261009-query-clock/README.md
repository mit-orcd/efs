# One fix: batched catalog deadline clock reads

Code commit/deployed build: 64cdf183d265, devel-find-stats. Shutdown is still
checked atomically on every cancellation callback. The monotonic deadline is
checked on the first callback and every 256 callbacks, and unconditionally
again before publishing. Thus deadline detection may lag by at most 255
callbacks, not a guaranteed wall-time bound; an expired build cannot publish.
The counter belongs exclusively to the publisher thread. Snapshot, metadata,
lane dependency and memory-cap behavior are unchanged.

Private publication/RPC, two-mount options, bounded export, retained snapshot,
permissions and restart reconstruction gates pass; POSIX 212/212 and peer
64/64. Deployment used graceful mount/server shutdown, preserved existing
data, retained direct I/O/TCP and .efs enabled, exit zero. The candidate also
contains the previously committed command-help correction.

Repeated simultaneous four-daemon perf capture: 30 seconds, 199 Hz, 11,977
samples, zero lost. Fresh workload tiny-clock-64cdf183: depth eight, 1,000
files/folder, eight workers, bounded 8,000 files to preserve catalog headroom.
At 41 seconds the screen reports 6,521 files, 159 files/s.

Compared with ../20261009-all-efsd (build 093295be288b): node 2's inclusive
clock_gettime share falls from 5.89% to 0.23%, about 96% lower. Its query-worker
share falls from 73.86% to 67.01%; catalog builder from 72.52% to 65.84%.
Per-PID reports use relative cycle percentages; inclusive rows overlap.

| Node | Previous window CPU | After window CPU |
| --- | ---: | ---: |
| 1 | 11.4% | 13.9% |
| 2 | 58.0% | 86.0% |
| 3 | 10.5% | 49.7% |
| 4 | 4.8% | 9.6% |

One core=100%; counters include perf startup/teardown. Overall CPU reduction
is NOT established: different live workload rate, partition mutation mix,
post-restart activity and more node-3 catalog work confound the comparison.
Latest 60 node-2 publications have median 284 ms, range 244–342 ms, with
1,135,362 files and 1,066,146,653 accounted bytes at the final publication.
The prior profile's nearby publication median was 316 ms; these windows are
not a controlled same-source benchmark. Do not attribute that difference
solely to this change. Remaining array reconstruction, allocation/page faults
and index work still dominate. No hardware-bound throughput claim.

Raw perf, full node-2 report (including costs below 0.3%), all daemon reports,
counter snapshots, publication excerpts, screen, private gates and deployment
record retained here. The bounded loader completed without increasing the cap.
