# ORCD package 1 — deployment and open scale gates, October 13

Package 1 remains OPEN; all-gates acceptance is not yet PASS. See the
[three-package assignment](three-work-packages-20261013.md). Package 2 fixes
are being qualified on AMD; ordinary-file Intel work is independent.

## Initial devel deployment

Frozen committed source `19b14609fe4d9881648d120fa76ce13a4bd5d385` built on
all eleven ORCD nodes. Source input checks, actual native RDMA descriptors,
server direct-I/O/files arguments and client/server command flags pass.
`EFS_EXPIRY=0` and `EFS_PACKED_WRITES=0` are explicit; no container backend.
Four servers fcstor003–006, seven FUSE clients fcstor007–013, mlx5_2 hardware
RDMA. Site account erbmi1 UID/GID246111, no root operations. Existing budgets
and deadlines are unchanged. The complete 400 GiB fixture is preserved.

Focused RHEL native Raft/store, query catalog/disk/I/O, concurrent append-offset
and WAL fault, captured-prefix worker, CLOSE/EOF, GC shutdown/fairness and
paired-read regressions PASS. Paired-read ASan/UBSan and negative controls PASS
with the configured private compiler/runtime. The first invocation used system
cc for a hardcoded sanitizer command and could not find libubsan; correcting
PATH to the existing private compiler fixed this environment prerequisite.
Complete AMD normal native results remain source-specific; this focused run is
not a complete ORCD fault/sanitizer matrix.

Benchmark worker interruption and client drains were normal. A new tiny-file
process appeared after the initial workload inventory and held directory FDs
on fcstor011, so unmount refused and mutation admission resumed. No server was
stopped until all clients unmounted. Reversibly pause the seven known benchmark
screen launcher shells, normally close their respawned workers, then retry;
all seven unmounts succeed without force or lazy detach. Retain process/screen
identities and files for resumption. This is an actual workload holder finding,
not proof of a FUSE cleanup correctness failure.

All eleven binaries installed, all four servers started and all seven clients
remounted with the requested profile. Independently specified fsynced large
and 512-byte markers retain exact size/bytes after redeployment. No formatting,
authoritative-store erasure, release-branch change or push.

**Initial 300-second startup gate FAIL:** listeners remain unavailable through
303.458 seconds. Logs show approximately 336–357 seconds before storage quota
and transport setup, followed by roughly13 seconds of Raft initialization.
Operational readiness/remount afterward restores service but does not replace
this failed deadline with a PASS. Resource samples cover all eleven hosts.

## Startup correction under development

`server_init_local_usage` always reconciles the advisory usage counter by a
full scan; startup then separately rescans every root for safe quota. The
proposed correction inventories each root once, shares the same observed counts
for both decisions, and overlaps ordinary-file roots with at most three helper
threads plus the caller. Every helper is joined before returning, accepting
counts or exposing an endpoint. Unknown results and counter overflow refuse
startup; helper creation failure falls back to caller work. Container scan
policy remains serial. No cached counter replaces a scan and no durability
barrier or quota reserve is removed.

Actual extracted helper regression PASS locally: zero/default, one, two, six
and maximum roots; exact once per root, max4 overlap, complete join, unknown
root/error and overflow refusal, serial containers, and first/partial helper
creation failures. The serial negative fails the overlap oracle. Native Linux
real-store/build and sanitizer qualification and a new full-size300-second
orderly cold gate remain pending. `tests/test_server_processes.py` cannot qualify
its Linux /proc identity fixture on this Mac; its unchanged Linux check remains
required.

Reproduce the new helper regression with
`python3 tests/test_storage_root_inventory.py`; then Linux `make -j2 test`.
Run the deployed four-daemon/seven-client orderly restart and verify actual
`storage-inventory` phase durations, usage/quota and independently known
acknowledged bytes. Do not increase the300-second readiness deadline.

## Remaining package 1 gates

Review and integrate the AMD compact-index and legacy-view retirement fixes;
measure generation and aggregate peak within16/32GiB. Run all six focused
commands, default/human/JSON stats and owner names on seven clients, concurrent
full-catalogue query/I/O, same-source normal cold restart, every one of1,638,400
payloads and exported paths (429,496,729,600 logical bytes), loaded latency and
resource measurements. Historical passes do not transfer to the new source.
Expiry, packing, containers and unplanned-event repair remain deferred/excluded.

Raw stage/source/runtime proofs, workload/holder/paused-shell records, startup
failure, daemon diagnostics and resource samples remain outside Git under
`operations/devel-efs` and `/home/erbmi1/orcd/scratch/efs/orcd-test`. Existing
scripts there reproduce deployment orchestration; native regression sources
and the three-package profile are committed. No raw artifacts are committed.
