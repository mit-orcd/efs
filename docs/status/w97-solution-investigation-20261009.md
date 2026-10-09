# W97 solution investigation — 2026-10-09

Branch: devel-persistence-pressure, created from da653c33.
NUC SSH is unavailable (Host is down, then connection timeout). No deployment
or server-policy change was made. Last verified state: command features
disabled, TCP and direct I/O.

## Code findings and candidate solutions

In src/raft/raft.c, ae_inflight_fresh permits replacement of an unanswered
non-full window whenever send_idx extends past its end. Thus small-entry
proposals can send progressively longer overlapping windows. Full windows
already suppress this until a reply or heartbeat-age timeout. The behavior
intentionally avoids an earlier 50 ms latency penalty for fast followers;
test_ae_full_window_not_resent_per_propose documents that tradeoff.

on_ae_rep clears in-flight tracking on every ordinary reply. An older
successful reply can therefore clear tracking for a newer overlapping window.
Measure this before blaming it for NUC drops. A candidate fix would retain
tracking when a reply covers only part of the outstanding end, with explicit
proof for rejection hints, term changes, commit/read probes and timeout
recovery. This alone would not suppress extending-window replacements.

The shared host inbox is limited to 256 frames and 32 MiB. Overflow drops a
frame and returns EFS_ERR_BUSY, without issuing a decoded Raft pressure reply.
The existing pressure-reply backoff therefore does not automatically cover
this overflow branch. n3 middle-window mean depth 252.58 approaches the frame
limit. Increasing queues alone is not justified.

## Investigation order and acceptance

1. Count initial, extending and timeout sends per group/peer; correlate reply
   coverage, queue residence, dropped frame types/groups and durable syncs
   with accepted replication progress. Keep diagnostics opt-in and bounded.
2. Test older-reply tracking with delayed/out-of-order overlapping replies,
   lost requests/replies, term changes, read probes and durability failures.
3. Evaluate bounded suppression/coalescing of extending replacements for slow
   followers, preserving fast-follower progress and commit/read probes. Never
   discard behind-tail traffic without normal Raft matching and ACK semantics.
4. Isolate representative NVMe WAL/payload persistence, competing I/O,
   scheduling and device temperature. Compare cohort/preallocation principles
   at equal durability; device-wide timings do not prove a hardware ceiling.

Reconnect NUC, verify command features remain disabled and inspect active
workers before any deployment. Preserve existing data, use fresh workload
paths and cooperative drain. Repeat fixed-seed sustained baseline/candidate
runs, idle/mixed p99 and W97 durability/recovery/POSIX/peer acceptance gates.
No production fix or performance gain is claimed at this checkpoint.

[Baseline measurement](batch-no-efs-profile-20261009.md).
