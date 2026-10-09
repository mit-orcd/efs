# W97 group-isolated sender priority coalescing

Source a770290a; build a770290a-pressure. Partial-reply retention candidate
c5ff653e was rejected and reverted in 6005fc5d before this build. Baseline
4b149e88 passed all 212 core POSIX tests after restoration; its append-only
control passed 2/2. This candidate changes only the sender's same-type
priority coalescing boundary: independent Raft groups retain their messages.
Emergency vote eviction of a heartbeat remains unchanged, as do queue limits,
durability and Raft authority.

Host helper regression fails on original code and passes on this candidate.
Raft/store tests pass with diagnostics off and on; host pressure policy passes.
Workload seed 970001, fixed readable payload pool, eight workers, same size/
depth/count distributions. Maximum 2,000,000 files prevents early completion;
the measured prefix is comparable while below the earlier 500,000 target.
Cooperative stop at 300 seconds; fresh output tree, existing data preserved.
No compiler or functional suite overlaps the timed run.

Comparison remains sequential on a growing namespace with changing device
state and possibly leader placement. Quantiles are histogram bin upper
bounds, independently sampled cumulative fields, roughly five-second dump
boundaries. Observer overhead is uncalibrated. Do not call this a randomized
A/B, a complete W97 closure, or proof of a hardware ceiling.

Data-send classification counts successful Raft send callbacks into the host
sender path, not actual wire transmissions. Queue replacement and transport
outcomes require separate attribution. Raw node logs are losslessly compressed;
rollback binaries remain on NUC and are excluded from repository evidence.
