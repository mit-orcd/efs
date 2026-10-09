# W97 exploratory replication attribution

Diagnostic source 4b149e88, installed build 4b149e88-batchobs. TCP/direct I/O,
commands disabled on four servers and both mounts. Workload seed 970001,
eight workers, original tiny-file size/depth/count distributions, fixed
readable payload bytes, cooperative SIGUSR1 stop after 300 seconds.

This exploratory run overlaps regression/candidate compilation and tests.
Do not use its throughput as a clean baseline or a performance gain estimate.
Its traffic counters establish that extending windows and partial replies
occur under load, and classify dropped data versus empty frames. Separate
clean baseline and candidate evidence supersede its throughput interpretation.
Counters are independently sampled cumulative atomics, with approximately
five-second boundary resolution. Observation overhead is uncalibrated.

Data-send classification counts successful Raft send callbacks into the host
sender path, not actual wire transmissions. Queue replacement and transport
outcomes require separate attribution. Raw node logs are losslessly compressed;
rollback binaries remain on NUC and are excluded from repository evidence.
