# Cluster test operations

This page preserves the operational guidance formerly kept in editor-specific
rules. Current work and handoffs live in `docs/status/README.md` and
`docs/status/in-flight.md`; measurements live in `results/`. Historical cluster
facts are not a substitute for checking the running system.

## Before a cluster run

Run `bash tests/preflight.sh`, optionally with `--expect-build <id>`. Record
server/client build IDs, daemon counts, Raft health, free space, mounted FUSE
filesystem type and leftover workloads. A mount directory by itself is not proof
of a live export. Abort a filesystem measurement if `findmnt` does not identify
`fuse.efs-fuse` or the endpoint is disconnected.

Build on the target node in its local source tree, not an NFS home. Mixed build
IDs cannot form a cluster; use the cluster's deploy/restart scripts and their
stop/drain policy. Prefer exact executable names or discovered endpoint-owner
PIDs for process inspection. Broad `pgrep -f`/`pkill -f` patterns can match the
SSH command itself or unrelated work. An ordinary stop must preserve dirty data;
force-discard explicitly accepts loss and is not a routine deployment workaround.

## Remote execution and evidence

Use finite timeouts appropriate to the expected command duration. A timeout is
a failed gate, not a pass or permission to retry indefinitely. Long fcstor tests
belong in a detached job on a cluster node, with an identifiable run name, output
log and exit status; keep the login-node launch short. Existing legacy helpers
are external to this repository and can be selected through the test scripts'
SSH configuration. Removing repository `.cursor` does not install or remove
those helpers.

If a tool channel loses its exit status, inspect the job transcript and process
state before repeating a mutation. Missing tool output does not prove that the
command never ran. Keep deploy/wipe attempts uniquely identified and avoid
concurrent builds or destructive jobs in the same source/storage directory.

## Measurement and cleanup

For publication-inclusive FUSE write throughput, include the flush in timing (`dd conv=fsync`
or fio `--end_fsync=1`, without time-based overwrite loops). Direct I/O bypasses
the kernel cache, not EFS's userspace cache. Cold-read measurements need remounts
or independent clients. Raw buffered benchmark rates measure cache acceptance;
see `docs/how-it-works/performance.md` for benchmark definitions and baselines.

After a completed test, remove only that run's temporary processes, detached
jobs, scratch and go/ready state. Check active jobs before cleaning shared
locations. Detach FUSE before removing a mount directory. Preserve evidence for
current failures and baselines according to `results/README.md`; a failed run
must leave enough logs to diagnose it. Never erase another active run's files.

A timed successful FUSE flush does not independently establish target
persistence or hardware power-loss durability; [W71](../backlog/work-items.md#w71)
records the inspected production-store gap.
