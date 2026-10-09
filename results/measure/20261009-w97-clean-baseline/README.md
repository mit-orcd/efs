# W97 w97-clean-baseline measurement

Source 4b149e88; installed build 4b149e88-batchobs. TCP/direct I/O, command features
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

Data-send classification counts successful Raft send callbacks into the host
sender path, not actual wire transmissions. Queue replacement and transport
outcomes require separate attribution. Raw node logs are losslessly compressed;
rollback binaries remain on NUC and are excluded from repository evidence.
