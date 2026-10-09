# W97 w97-partial-reply measurement

Source c5ff653e; installed build c5ff653e-pressure. TCP/direct I/O, command features
remain disabled. Eight-worker tiny-file workload with fixed seed 970001 and
fixed payload pool. Per-folder counts, depths and sizes retain the original
generator's distributions. Dedicated fresh output tree, maximum 500,000
files to prevent an early finish; cooperative stop at 300 seconds.
No compilation or functional suites overlap this timed run. All data retained.

Raw cumulative counters/histograms every five seconds, process/disk/memory
samples every five seconds. Histogram quantiles are bin upper bounds, not
exact percentiles; maximum is cumulative, not an interval maximum. Samples
are independent atomics. Metrics overhead is uncalibrated.

This is one sequential baseline/candidate pair: prefix names, leader placement,
namespace growth and device/cache state may differ. It establishes neither
randomized performance acceptance nor a physical hardware ceiling. Keep the
exploratory build-overlap run separate. Raft/store tests, negative regression
and final POSIX/peer/cold evidence describe actual gates run; broader W97
mixed-load/repeated/fault acceptance remains separately open.

## Rejection

Intervals: 122.24 / 72.20 / 38.73 files/s, versus clean baseline
112.18 / 60.52 / 48.44. Data-send counters fall from 235,836 to 148,748
across leaders and inbox drops from 91,932 to 79,300, but late throughput
regresses. Full core POSIX: 211/212, concurrent_appends exceeds 30 seconds.
Peer: 64/64 in 234.5 seconds. Baseline restored: append-only 2/2 in 4.6
seconds and identical full core suite 212/212 in 69.7 seconds. These are
sequential controls, not matched randomized repeats. Candidate was reverted
in 6005fc5d. Do not promote its synchronous-network unit-test success as
acceptance of actual asynchronous host behavior.

Data-send classification counts successful Raft send callbacks into the host
sender path, not actual wire transmissions. Queue replacement and transport
outcomes require separate attribution. Raw node logs are losslessly compressed;
rollback binaries remain on NUC and are excluded from repository evidence.
