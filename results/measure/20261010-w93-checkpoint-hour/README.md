# NUC W93 checkpoint cadence, October 10, 2026

Report: [status](../../../docs/status/w93-checkpoint-cadence-20261010.md).
The user authorized an hour of integration, fixes and NUC-only investigation.
Frozen branch tips are in `merge-tips.json`; all sixteen are ancestors of the
final branch. Production source/build is d4f2ddb3; later source commits add
private test harnesses. Installed/executing hashes and environments are captured
per cell and in `final-service-state.json`.

## Evidence interpretation

`10ms`, `100ms`: 600-second matched-duration cells. `10ms-return`: 180 seconds,
so use `common-window.py` for equal first-180-second comparisons. Retained state
and sequential drive state remain confounders; no hardware ceiling is claimed.
Preparation, graceful startup/drain, compilation, profiles and fault tests are
outside timed cells. Fresh roots and deterministic 1–512-byte payloads, eight
workers split equally between verified inode groups, fixed immutable read probes.

Initial `posix.log`: 211/212, concurrent append timeout at the original 30-second
budget. Isolated diagnostic increases the budget; it is not acceptance. Clean
`posix-repeat.log`: 212/212, unchanged budget. `peer.log`: 64/64. First timeout
remains unexplained. `merged-test.log` fails because evidence was omitted from
the isolated build copy; corrected complete candidate tests pass.

The first diagnostic perf recording occurred after the short append test and
measures idle/background work. The later `checkpoint-perf` captures a separate
active diagnostic workload; its throughput is not acceptance. Raw perf binaries
stay on NUC at `/data1/efs/evidence/w93-checkpoint-hour-20261010`, with retained SHA256
and decoded reports. Strace wall times aggregate threads and are not CPU time
or exclusive device utilization. Intentional timeout/INT detach exit 124 is
recorded. Raw log padding is retained.

Marker-failure replay uses private stores/PIDs, not service SIGKILL. Early missing
mount / before-cadence assertion failures are preserved. Final gate adds direct
offline follower KV verification; client reads alone cannot prove follower KV.
Real KV fdatasync failure is a separate gate. These are process-crash tests, not
physical power-loss verification. `raw-sync` is an independent buffered XFS
4096-byte overwrite + fdatasync probe, QD1/4 independent files; not EFS payload
throughput or an O_DIRECT benchmark. Its NVMe path resolves under `/home`,
where n3/n4 actually reside; `/data2` alone would identify the wrong filesystem.

Final restoration keeps 10 ms, FLOW=1, all other Raft experiment switches and
OBS=0, command features disabled, TCP/direct I/O; both service mounts and existing
stores preserved. `cold-verification.log` samples exact first/last payloads
from all three timed trees through both cold mounts. Gates must pass before
claiming acceptance; consult recorded exit files and final report.

The full 100 ms fault batch fails ACK-next append replay, 31/32 wrong offsets.
Append-only 10 and 100 ms controls pass afterward; those omit earlier full-batch
sync/crash work and do not prove cadence causality. Final-state restoration-only
mode records the failure and unexecuted gates; no finish-pass exists. No adoption
or W64/W93/W97 closure. Stronger offline follower oracle remains unexecuted.
