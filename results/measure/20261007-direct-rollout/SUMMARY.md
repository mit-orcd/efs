# Production direct extent retention — Oct 7

Implementation: `3c4436fb`. NUC four daemons verified with `--direct-io`.
Single-client 216/217 PASS, zero failures, one unsupported mmap skip;
two-client 64/64 PASS; durability prepare and cold-remount verify 26/26 PASS.
Raw rollout log and TSVs are retained alongside this checkpoint.

Mac ARM build and unit tests passed. The initial direct-I/O run failed with
timeouts and stopped with NOTRUN cases; this is not acceptance evidence.
The user corrected the mac target to buffered I/O, then deferred that cluster
during maintenance. No further mac operations are authorized for this phase.
