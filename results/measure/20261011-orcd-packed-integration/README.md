# ORCD packed integration — October 11, 2026

Development work on `devel-orcd`, following `4a58f97a`. The live cluster on
port 17432 remains `c52a4643` with legacy mounted writes. This evidence concerns
an isolated four-server TCP/direct-I/O cluster on port 19832 and private test
roots. All processes ran as `erbmi1`, UID 246111. No production stores were erased.

| Gate | Result / evidence |
| --- | --- |
| Full Linux regression | PASS: [clean full log](packed-hour-clean-full.log) |
| Both metadata groups: immutable admission, 128-permit limit, cancellation, fences, ordered floors, cold reopen | PASS: [group gate](packed-both-group-admission.log) |
| Snapshot capability, actual PUT/GET, publication/receipt, cohort deadline/sync/capacity/retry, degraded reader | PASS: [focused final gate](packed-hour-final-epoch-gate.log) |
| Actual 16-record coalesced network upload/publication and permit retirement with LIVE mappings retained | PASS: [RPC gate](packed-cohort-rpc.log) |
| Lost node1/node2, degraded reads, receipts and retired admission floor after four-server cold restart | PASS: [cohort recovery](packed-cohort-rpc-recovery.log), [initial recovery](packed-rpc-recovery.log) |
| Sustained RPC mappings/reads/receipts and retired-floor denial | PASS, 64 rounds / 1,024 checks: [repeated read gate](packed-repeated-read.log) |
| Actual FUSE packed open/read/stat and ordinary write/fsync | PASS: [mount gate](packed-fuse-gate.log) |
| Mounted promotion/grow/fsync, truncate-zero, unlink and hard-link survival; twelve neighbors intact | PASS: [lifecycle](packed-fuse-lifecycle.log) |
| Cold FUSE client and four-server restart after those mutations | PASS: [cold lifecycle](packed-fuse-cold-lifecycle.log) |
| Eight-worker randomized physical storage soak, buffered/direct, EC fallback, retries, fences, GC, cold quota | PASS, 1,200 seconds / 299,520 cohorts: [soak](packed-storage-soak.log), [exit status](packed-storage-soak.log.rc) |
| ASan: actual cohort, packed reader and metadata apply implementations | PASS: [ASan log](packed-asan-gate.log). Earlier callback warning in the append-only log was fixed; the final clean full build has no warnings. |
| UBSan | NOT RUN: [linker failure](packed-sanitizer-gate.log), missing host `/usr/lib64/libubsan.so.1.0.0` |
| Distributed physical fragment audit | Three 12,288-byte allocated fragments per original sixteen-record/8,192-byte cohort = 36,864 bytes / 4.5×: [exact paths](distributed-physical-allocation.json) |

Captured text logs have trailing whitespace removed.

`source-manifest.json` records the final changed code/test files by SHA-256.
The full suite passes before the final test-only extension to both groups;
that extension has its separate passing gate. Per-host runtime files record
private binary hashes and Raft readiness. Development binaries report build
identity `unknown` because the private overlay has no Git directory; their
hashes, source manifest and retained logs provide the provenance. This is not
a mixed-version network acceptance result.

The native fixture is `tests/stress/packed_rpc_gate.c`; `create` constructs
sixteen coalesced records, obtains Raft/session admission, sends real immutable
PUTs and retry PUTs, publishes each record and saves a fixture. `retire` retires
the upload permit without deleting LIVE mappings. `verify-retired` requires an
authoritative STALE upload probe and checks exact reads plus durable receipts.
The lifecycle gate subsequently modifies four records, so its cohort fixture
cannot now verify all original sixteen mappings unchanged.

Private server storage: `/tmp/efs-packed-rpc-20261011` on fcstor003–006;
binaries: `/tmp/efs-packed-rpc-20261011-bin`; PID file: storage root + `.pid`.
Four servers use `EFS_MD_RAFT_N=4 EFS_TRANSPORT=tcp EFS_COMMANDS_SERVER=0`,
node IDs 1–4, their 172.16.223.57–60 addresses, port 19832, quota 1G,
`--writers 0 --direct-io`, and join seed 172.16.223.57:19832. The private
FUSE mount `/tmp/efs-packed-mount-20261011` was drained and unmounted after
cold verification. The servers remain available for the next integration step.
[Final process and listening-port check](final-runtime.txt) verifies the private
servers run as erbmi1 and the separate live port remains available on all four
nodes.

The physical ratios exclude metadata/WAL/directory writes, obsolete ordinary
objects and NAND amplification. The soak is a storage correctness gate, not a
mounted files/s benchmark. The [live baseline checkpoint](live-baseline-checkpoint.txt)
shows 1,038,555 files at 5,618.5 seconds / 185 files/s cumulative on the old path;
private testing ran concurrently, so it is not a matched performance control.

Remaining: mounted cache/snapshot/close/fsync ownership and cohort scheduling;
distributed shared GC and abandoned-PENDING cancellation; aggregate memory
accounting, native ABI/capability rollout; mixed fault/recovery and matched
throughput/amplification acceptance. W95/P4.1 remains open.
