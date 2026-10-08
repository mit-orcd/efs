# Mac D30/W23/W89 pressure foundations

See [checkpoint](../../../docs/status/compaction-pressure-20261008.md).

- `kv-pressure.log`: real unchanged 1 GiB production KV cap, finite tail growth,
  idle drain and durable reopen. Engine-only, no Raft follower bound claimed.
- `mac-pressure-final-tests.log`: full Linux make test after fixture isolation fix.
- `mac-pressure-private-build.log`: isolated efs-owned fault-build tests.
- `mac-pressure-health.log`, `mac-pressure-policy.log`: restored matching-build
  Mac daemons as efs, direct/TCP, commands disabled, converged groups.
- `source-sha256.txt`: source identity; rsynced guest builds report unknown.

Initial fixture setup failed because the new KV pressure test left a key behind
for the subsequent scan-order test. The final fixture removes its store before
that test. Earlier setup also encountered an exited Tart launcher session and a
nonwritable /data1 parent; the final experiment uses an efs-owned private parent.
No installed store was deleted and no production L0 threshold was lowered.

Package remains open for shared Raft admission/replica integration and the
complete stalled-follower/cancellation/leadership/failure/cold-byte gates.
