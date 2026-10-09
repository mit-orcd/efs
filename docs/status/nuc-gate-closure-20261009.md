# NUC W73/W64 gate checkpoint — October 9 UTC / October 8 local

Branch under test: `devel-gate-closure`, production commit
`3e1d4c6e9c7d596c1389c8e639d423bed73ced6b`. The user reported new code, but
local branch inspection before and during this run still resolved to this
commit; origin advertised no `devel-gate-closure` branch. `devel` had advanced
independently and was not substituted. The new test harness changes described
below are not production implementation changes.

NUC uses files-engine private fixtures on `/data1/efs` and `/data2/efs`, with
`/data1/efs/mnt` and `mnt2` borrowed under a runner lock. Installed stores are
preserved. Installed service restoration uses direct I/O/TCP and
`EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`. No tiny-file generator or independent
live gate was active. No RDMA device/sysfs class exists on NUC; both RXE cells
are unavailable, not passed. Process-crash tests do not establish hardware
power-loss durability or physical multi-host behavior.

## Initial TCP mode runs

Both direct and buffered fixtures passed the focused gates before POSIX:

- O_SYNC/O_DSYNC, 31-byte and 2 MiB ordinary/append writes, peer bytes before
  close, authoritative peer EOF extension, cold server crash hashes.
- Lost append replies, 32 identical concurrent requests, inode-group leader
  death, ACK retirement, changed-payload refusal and exact cold append bytes.
- Failed synchronous publication retaining partial bodies for fsync and cold
  recovery; dirty-stop recovery; acknowledged mutations during leader stop.
- Physical GC, last-open-descriptor retention, late-PUT fencing, offline-member
  reclamation and rejoined-member/cold-read checks reached before POSIX.

Direct POSIX: **212 passed, 1 failed, 4 skipped, 0 not run (107.4 s)**.
Buffered POSIX: **212 passed, 1 failed, 4 skipped, 0 not run (72.9 s)**.
Both failures were `concurrent_appends`, exceeding its existing 30-second
budget. Neither is accepted as a green run. POSIX failure stopped the original
harness before its peer suite and 800-record mixed append/GC audit.

The initial direct run overlapped the full unit target's storage churn. That
confounds attribution; no claim is made that contention caused the timeout.
The buffered failure also occurred after that churn stage finished. A clean
repeat without unit/generator traffic is recorded below.

## Full unit target

`make test` passed after restoring the committed `results/` tree required by
the documentation link check. The first source-only copy omitted that evidence
and stopped at documentation links before reaching later unit stages. Its
failure log is retained alongside the successful complete run. This was a
copy/fixture issue, not a repaired production defect.

## Mount-wide sync acceptance

The test harness adds `--mount-sync`, a focused gate using `-o sync` and requiring
`findmnt` to confirm the kernel mount flag. Writer descriptors use ordinary
O_WRONLY, with O_APPEND only in append cases: no O_SYNC/O_DSYNC and no explicit
fsync before the peer check. The parent holds the writer open until the second
mount verifies the entire syscall byte count and contents. Cases cover 31 bytes
and 2 MiB, ordinary and append writes, plus peer lane EOF extension. A full
private-daemon crash/restart verifies exact cold hashes, then physical deletion
drains. This gate passes in **both direct/TCP and buffered/TCP**.

The new optional `--continue-posix-failure` / `--continue-regression-failure`
preserves failing POSIX/peer commands
and exit status while allowing independent later peer/append/GC gates to run.
The implementation retains a nonzero final exit for a recorded regression
failure. Its full live continuation path remains unverified at this checkpoint;
default fail-fast behavior is unchanged.

## Clean repeat and remaining acceptance

The clean direct/TCP repeat passes POSIX **213 passed, 0 failed, 4 skipped
(81.1 s console wall time, including cleanup)**. The skips are unsupported
shared mmap and three opt-in-disabled `.efs` cases. The peer suite subsequently
reports EIO in concurrent append and append-versus-truncate, then BUSY namespace
probes. Its final inventory and the clean buffered run are still pending.

The failure-time trace records 30-second inode RPC receive waits and REPORT
operations ending `EFS_ERR_BUSY` after approximately eight seconds. Captured
group-2 state has commit 14,053 versus applied 14,028; contemporaneous disk I/O
counters report zero GET/PUT/disk errors. This is evidence of metadata progress
trouble, not a proved cause or proof of acknowledged-byte loss. Do not attribute
it solely to unit workload contention: the peer failure is in the clean run.

NUC subsequently became unreachable at `10.0.0.203`: repeated SSH connect
timeouts and three ping probes with 100% packet loss. The last SSH attempt reported `Host is down`. No successful reconnect
or final installed-service verification has occurred at this checkpoint.
Unreachability alone does not prove whether the host or detached runners stopped.

The clean direct peer suite was still running after EIO and BUSY failures. Its
final summary and mixed append/GC audit remain unverified. The clean buffered
repeat is queued behind direct-run restoration; it was not observed starting.
Do not label either run complete. Automatic installed-service restoration is
configured, but its final success is **unverified** until access returns.
No host reboot or forced installed-store discard was attempted.

Next: restore SSH access, inspect runner exit/status and active fixtures,
collect final manifests/TSVs, finish unreached independent gates and verify
all four installed daemons and both mounts. Investigate peer append EIO/BUSY
before W64 acceptance. Resolve the user-reported new-code location before
claiming this exercises newer production code.
W64 production-session fencing and broader unknown-outcome/failover acceptance
remain open; these legacy/durable-receipt gates do not close those requirements.
W73's RXE mode matrix remains unavailable on NUC. Hardware power-loss acceptance
remains separate under W71.

[Raw evidence](../../results/measure/20261009-nuc-w73-w64/) contains exact
commands, manifests, source/binary hashes, JSON verdicts and POSIX/peer tables.
Private fixture stores and detailed daemon/FUSE traces remain on NUC; they are
not copied into Git.

## Runner and evidence limits

The initial restored daemon inherited the runner lock FD. This prevented the
queued mount-sync runner acquiring the same lock. A new shared lock was used
and restoration now closes FD 9 before launching service children. Both
mount-sync modes and their restoration completed under the corrected runner
before the clean direct repeat started. No installed mount was force-detached.

The local export predates the later peer failures: it contains complete initial
and mount-sync verdicts, and a partial clean-direct snapshot. Later peer errors
and `/data1/efs/gc-direct-agr8e5h7/peer-failure-live-status.txt` remain on NUC.
The values above were observed over SSH before access failed. Retrieve those
artifacts before treating the partial export as a finished run.
