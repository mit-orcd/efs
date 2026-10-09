# W93 overlap suffix experiment — 2026-10-09

Branch `devel-persistence-pressure`, NUC only, TCP/direct I/O, command service off.
Preserved production stores and prior test trees. No main devel checkout changes.

Source commits:
- `e3ed79e8`: read-only matching-prefix/duplicate/conflict classification.
- `ca810bac`: default-off guarded overlap suffix durability cohorts, actual-core regressions.

The diagnostic build precedes the frozen `ca810bac` production build. Build logs,
installed version/checksums and each timed cell's process identity/selected flags
are retained. Both 900-second cells use the identical efsd SHA256; only
`EFS_RAFT_BATCH_OVERLAP` changes. FLOW=1, NOOP=1, GROUP_DRAIN=0, OBS=1 and
COMMANDS_SERVER=0 in both. Compare the checksum/flags in `identity.json`.

`balanced-load.py` chooses directories by the verified source group mapping;
four writers per group, deterministic 1–512-byte payloads, CREATE/write/close.
Each worker has a 32,000-file cap; any cap hit invalidates unclipped performance
acceptance. Preparation is outside the timer. SIGUSR1 drains current writes;
all hosted replicas must cover a sampled leader commit cut before restarting.
`balanced-capture.py` samples processes/diskstats every five seconds, space/read
probes every minute and sequential Raft status every 300 seconds. Read probes
are small and identical across modes; their sparse tails are not acceptance
for rare latency events. Logs are compressed after timing; no compiler,
functional suite or profiler runs during either interval. Completed baseline
evidence is copied during candidate restart, before candidate timing.

`balanced-analyze.py` reports three interval rates, exact operation latency
quantiles and diagnostic histogram/counter deltas. Five-second publication
means BO boundaries can differ by a few seconds from 900-second workload timing.
`balanced-resources.py` is offline only: sampled RSS, sampled replica lag,
space and opcode census. Command counts are log appends, not unique requests
or durability barriers. `command-opcodes.json` comes from this build's header.

`balanced-verify.py` checks first and last four acknowledged files per writer
on both cold mounts, 256 exact payload checks. It was strengthened before its
post-run invocation; this does not change the timed workload. Fault gates use
private stores while preserved service stores are stopped, then restore them
with experimental options and diagnostics off. Their process-crash/fault
coverage does not simulate hardware power loss.

Controller locking uses `flock --close`. The diagnostic deployment initially
allowed child daemons to inherit its lock; the next deployment refused, then a
graceful stop released it. `inherited-lock-stop` retains the recovery evidence;
no lock replacement or forced write discard occurred.

Interpret `inbox_drop` with FLOW=1 as BUSY admission refusals, not lost user
files. Raft retains durable-log authority and retries. Sequential store/device
state, retained data and background GC remain confounders. This is one A/B pair;
no repeated gain or hardware ceiling is proved.

See `docs/status/w93-overlap-suffix-20261009.md` for final results and limitations.
Rollback binaries stay on NUC and are excluded from this evidence directory.

Raw console logs and TSVs retain original padding and empty terminal fields.
Whitespace checks apply to source/docs/harness scripts; raw outputs are not
rewritten merely to remove their intentional trailing spaces or tabs.
