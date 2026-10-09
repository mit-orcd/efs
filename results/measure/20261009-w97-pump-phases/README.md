# W97 pump-phase reproduction, 2026-10-09

Source diagnostic commit 2cca7cfc; build 2cca7cfc-pressure. Same production
replication policy as a770290a; opt-in observation adds queue residence and
separate pump-lock, inbox-drain, KV WAL release and applied-index persistence
histograms. These timings do not control admission, ACKs or durability.

Raft/store tests pass with diagnostics off and on; actual host outbox regression
passes. Five-minute workload uses eight workers, seed 970001, fresh preserved
output tree, original tiny-file distributions and cooperative SIGUSR1 drain.
No compilation or functional test overlaps the measured workload.

Queue age is measured immediately before decoding each frame, so it includes
waiting in the shared inbox and behind earlier frames within the current drain.
Drain timing includes decoding, Raft persistence and metadata apply; it is not
pure Raft disk time. KV release and applied persistence are measured separately
after unlocking the host. Histograms include idle pump cycles; mean phase time
must be interpreted with count and total elapsed time, not compared blindly
against per-sync means. Timing adds uncalibrated observation overhead.

Dropped frames are refused replication messages at the bounded receiver inbox,
not lost files or durability tickets. Retransmission is part of Raft; these
counters alone do not establish exact retries, data-loss absence, or recovery
latency for each individual dropped message.

Result: 113.40 / 50.55 / 55.80 files/s across the three 100-second windows;
94,175 n3 drops (34,276 data / 59,899 empty), zero on n4. Core POSIX 212/212
in 70.8 s; clean restart, 32 sampled cold payloads on both mounts, six process
environments verified with observations disabled. See analysis.json for exact
histogram windows and docs/status/w97-solution-investigation-20261009.md for
attribution and remaining work. Peer/fault suites not repeated in this run.
