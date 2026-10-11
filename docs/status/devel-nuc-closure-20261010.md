# NUC closure session — October 10, 2026

User assignment: review devel, create and use `devel-nuc`, reserve NUC
exclusively for three hours, prioritize documentation items with short remaining
NUC gates. Session starts October 10 at 23:41 EDT (October 11 03:41 UTC);
the work window ends October 11 at 02:41 EDT (06:41 UTC).

## Branch and preserved service

Branch starts at devel `746ae9f099351624f5c38fa872dcc6f4daa5e7c3`.
Worktree: `/Users/mike/Documents/Codex/2026-10-07/i-l/work/efs-find-stats`.
Other checkouts, AMD, Intel and Xorinox are outside this session.
Production code initially matches that captured devel tip; the early branch
commits add measurement and missing operator acceptance tests.

Remote isolated source and evidence: `/data1/efs/devel-nuc-20261010`.
Existing four-node stores and both service mounts are preserved. Measurements
cleanly drain the regular clients and daemons, then restore the installed
service in `finally`. No reboot, drive setting change, installed-store wipe,
forced dirty discard, main merge, push or tag is requested.

## Ordered work and acceptance

| Item | Selected remaining scope | Session state |
| --- | --- | --- |
| W96 | Production KV WAL reservation on/off, both physical drives, repeated ABBA batch distributions, bounded allocation, reset/reopen and exact replay; full native/POSIX/peer/fault/cold gates | Running; no closure yet |
| W78 | Absent/dead/invalid/unrelated/wrong-storage/stale-start PID and owned daemon/recorder retirement | Missing absent/dead cases added; current-code NUC run queued |
| W79 | Both aliases and help, real consumed successful mkfs replies, uncertain-outcome retry preserves namespace/salt, explicit extra-argument refusal | Missing reply-loss gate added; direct/buffered NUC runs queued |
| W70 | Accepted/refused quota reductions, unchanged bytes and no fictitious migration | Current-code direct/buffered × one/two-root repeat queued |
| W80 | Retired QUERY_STATS gives nonzero unavailable response with no false totals in normal/raw output; current help and legacy/malformed reply tests | Current-code nonempty direct/buffered × one/two-root repeat queued |
| W97 | Three-writer durable file controls outside EFS; growing/reset files versus preallocated rings, reversed repeats, barrier distributions, disk/CPU counters and exact reopened blocks | Prepared; later stage queued |

W73/W64 retain production-session/platform obligations. W53 explicitly requires
investigation followed by the user's keep/revert decision. W54/W38's physical
multi-host gate cannot close on a single NUC. W92's current billion-entry and
400 GiB fixtures belong to the retained separate execution environment. These
are not replaced by unrelated NUC smoke passes. RDMA placement follows W106:
AMD software RDMA or orcd-test hardware RDMA, not this NUC session.

## Measurement limits

The W96 harness includes production `kv_wal.c` and differs only by the existing
compile-time reservation constant (zero control versus default 1 MiB). It
persists one or sixteen 128-byte values per atomic record. Every 8 MiB it
checks replay, resets the covered WAL and verifies bounded reservations. Timed
throughput includes these checks and reset barriers; reported sync counts also
include reservation-tail repair and the final reopen check. This is a WAL
principle measurement, not end-to-end file creation or a complete production
metadata batch-distribution model. Sync percentiles have 1 microsecond bins
with a 1-second overflow bin and a separately retained exact maximum.

The W97 control uses three independent 64 KiB writers, each with a 256 MiB
file. Growing mode truncates and durably resets at the boundary; ring mode
preallocates before timing and overwrites slots. It measures allocation/rotation
costs; it neither implements an alternative EFS storage engine nor establishes
NAND-cache occupancy. Full current files are reopened and checked exactly,
without claiming kernel-cold or power-loss coverage. Sync histograms overflow
at 100 ms, retain the overflow count and exact maximum. Passive counters retain
host contention and process CPU rather than assuming zero software overhead.

## Terminal evidence

Not complete. Populate verdicts and links from retained raw files after all
selected runs, including failures, restoration and the exact code/build IDs.
