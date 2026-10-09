# Transaction persistence must remain retryable

Two reproduced failures:

* Host apply converted PREPARE/DECIDE/RESOLVE/DROP IO/NOMEM into an advanced
  Raft apply index. Actual extracted return-policy regression failed before fix.
* RESOLVE replaced a failed version read with zero and committed anyway. AMD
  fault regression returned OK for an injected IO error before fix.

Host apply now preserves IO/NOMEM at the pending index. Business conflicts still
advance with their verdict in the ring. RESOLVE propagates failed version reads
before its atomic batch, preserving old data, versions, intents and epoch guards.

Native AMD unit tests inject IO/NOMEM/PROTO version reads and IO/NOMEM batches;
retry commits exactly once, increments version7 to8, and only then permits
revocation. Raft core persistence-retry and the actual host policy tests pass.
Normal direct/TCP full POSIX, namespace guards, cold lease/ticket replay, append
and physical GC/restart gates pass. Full code regressions pass.

The actual fault daemon refuses transaction persistence on one participating
nonleader. Daemon3 stays15–17 entries behind commit while the quorum continues;
release catches it up. Restart preserves exact linked bytes and nlink2.

Failed live attempts are retained: daemon4 did not receive transaction entries;
selecting the client seed as the stalled follower caused LINK EIO. The passing
fixture chooses an actual nonseed follower. The seed routing/availability EIO
is a separate open finding, not covered by that passing result.

These are process/WAL and injected persistence gates on one AMD host with four
daemons, not physical device failure or hardware power-loss proof.
