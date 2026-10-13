# ORCD package 1 — deployment and open scale gates, October 13

Current user scope is feature correctness/acceptance; latency/resources are
measurements and existing bounded gates, with no new performance tuning.

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

## Startup correction qualification

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
creation failures. The serial negative fails the overlap oracle.

Isolated ORCD fcstor006 Linux native qualification **PASS** at startup correction
`dd1f4c35f43c9199a6b1daeb61e1d7ca04925aa5`: `make -j4 all`, storage-capacity
extracted regression, real store GC/durability/overwrite, root-inventory normal
and ASan/UBSan with leak detection, and unchanged Linux /proc server/perf owner
fixtures. Binary efsd SHA256
`c50e4af718e181eb6934561518c4ed3ef08bbe4e993ee9ec96ec4a8e63828ee6`;
RHEL9.8 on fcstor006, GCC system compiler with the existing private sanitizer
runtime and libfuse3. Native tests operate only on private temporary fixtures;
this is neither mounted-scale acceptance nor a live startup timing result.
The first runner named the Python capacity test as a native executable and
stopped before regressions; the corrected invocation below passes all checks.

From an isolated committed Linux build with `CC` selecting the configured
compiler/runtime, reproduce with `make -j4 all`,
`python3 tests/test_storage_capacity.py`, `./tests/test_store_gc`,
`./tests/test_store_durability`, `./tests/test_store_overwrite`,
`python3 tests/test_storage_root_inventory.py`, then
`ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 CC="$CC -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_storage_root_inventory.py`
and `python3 tests/test_server_processes.py`. Tests own and clean their private
fixtures, retain unrelated process identities and normally stop owned daemons.
A new full-size 300-second orderly cold gate remains pending.

Reproduce the new helper regression with
`python3 tests/test_storage_root_inventory.py`; then Linux `make -j2 test`.
Run the deployed four-daemon/seven-client orderly restart and verify actual
`storage-inventory` phase durations, usage/quota and independently known
acknowledged bytes. Do not increase the300-second readiness deadline.

## Focused mounted baseline

All six suites on deployed `19b14609` **FAIL**: `efs_commands`20.136s,
`efs_command_options`25.292s, `efs_command_pages`0.816s,
`virtual_interfaces`0.415s, `efs_commands_load`14.082s and
`query_large_delta`122.891s. Query reads return ENOSPC on both main and peer
mounts. Help/listing and selected permission cases pass, but none closes its
suite. Private peer orderly stop **PASS**. This preserves the footprint failure
at unchanged budgets rather than attributing it to payload disk capacity.
Reproduce `python3 scripts/orcd_efs_commands.py /tmp/efs-mount PEER --query-host
172.16.223.58 --output NEW_EXTERNAL_DIRECTORY`, with two same-export native
RDMA mounts and the requested profile. Preserve failures and normally unmount
the peer; do not widen the per-suite deadlines or delete authoritative stores.

Reviewed devel ordinary-file metadata read-admission experiment `2b927c1b`
was initially integrated in the private candidate. Intel's later loaded run
regressed despite native passes, so devel `cecc3524` reverts it. The ORCD
candidate includes that rejection and preserves the previous production
read-admission behavior. No ORCD deployment of this experiment occurred.
AMD is correcting legacy retirement
after replacement-link rename succeeds but root fsync fails: abandoning the
candidate must retain the old checkpoint until durable replacement publication.
Do not deploy the earlier retirement candidate as a qualified format transition.

## Distributed query harness routing

AMD's localhost node-ID redirects use consecutive ports. ORCD nodes use
separate addresses at port17432; retained page tokens identify the owner node,
not the initial seed. Shared `query_rpc.routed_query` now accepts explicit
`EFS_QUERY_TEST_NODES` JSON endpoint mapping, retains the existing localhost
fallback, routes retained stream/CLOSE directly to its owner and bounds fresh
request redirects. The ORCD command runner sets all four known endpoints.
`python3 tests/test_query_rpc_routing.py` **PASS** locally: distributed redirect,
retained stream/CLOSE after a different seed, localhost compatibility, refusal
propagation, unmapped owner and redirect loop. No mounted pass is inferred;
server transport/production code is unchanged by this harness correction.

## Integrated devel hardware deployment

Frozen devel `d54949ecb50482db0099a34345b7daff47977078` contains the startup
correction, AMD compact-directory references and legacy publication corrections
through `3409e0c6`, distributed command-gate routing, and Intel's rejected
experiment revert. Eleven native staged builds and **857 source/test input
hashes per host PASS**. Full normal native `make -j4 test` on fcstor006 PASS;
six query tests with their entire linked library built with ASan/UBSan and leak
detection PASS (`test_query_catalog`, `test_query_disk`, `test_query_delta`,
`test_query_disk_io`, `test_query_policy`, `test_query_refresh`), plus the root
inventory sanitizer oracle. Use a separate clean sanitizer tree, not an
unsanitized supporting library. Normal efsd SHA256 on fcstor006
`6ffc253f8701db4e034a665b9119411f0ce0e78492ab1f126f4fcbf470cb83e9`;
efs-fuse `884e839bc1724e4550bda7e72ce9e1129d4d50f8440efe988282155e034c11ca`.
Build with the same recorded native flags/private runtime as the initial run.

AMD's exact `3409e0c6` handoff passes normal/fault native, six fully linked
query sanitizers, genuine v5→v6 mounted transition and next refresh, orderly
quorum refusal/restoration, command/options/pages and loaded10,268-payload
oracle. These small-target results authorize scale testing, not ORCD closure.
See its [source-specific evidence](amd-package2-query-20261013.md).

Orderly deployment **PASS**: all seven normal unmounts, four normal server stops,
all eleven source/profile installations, seven remounts and independently known
acknowledged marker bytes. Exact installed inputs and actual native RDMA/file
engine/direct-I/O/feature flags PASS. **Server readiness85.091s**, unchanged
300s ceiling; fcstor004's complete six-root inventory67.365s accounts for
1,396,495,020,032 payload bytes. This deployment improvement is not yet the
required same-source cold-restart acceptance.

**Full catalogue build still FAIL / ENOSPC** at unchanged16/32GiB: projected
v6 admission fails at raw shard3640/3641 and13,507,436 inodes, before
stats/persist, versus the v5 baseline shard3002 /11,122,417 inodes. Retained v5
checkpoint10,543,618 inodes,13,018,089,332 actual file bytes per publisher;
13,026,375,540 bytes including directory entries. Both original checkpoints
remain because replacement has not published. Do not erase them or authoritative
stores to turn this into a passing transition.

The permanent name-index reservation currently charges two full n/a copies
for every namespace entry although merge scratch is confined to the partition
being sorted. AMD is investigating explicit transient reservations and release,
with real peak/fault proof; no budget increase or unqualified accounting change
has been deployed. Remaining mounted gates are NOTRUN on this source while
catalogue admission fails. Resource/cache samples retain all11/4 observations
outside Git; sampling alone does not prove unobserved peak safety.

Reproduce the full fixture oracle after command/concurrency and normal cold
restart gates with `python3 tests/live/efs_query_capacity.py verify /tmp/efs-mount
FIXTURE NEW_EXTERNAL_RESULTS --files 1638400 --bytes 262144 --workers 8`. The fixture name is
`devel-efs-400g-0dd2a7c7912b-20261012`. It checks every acknowledged payload and
exported path; mount-ID checks avoid forks while direct reads are in flight.
Run the latency sampler as a separate process and collect all daemon/client
RSS, CPU, I/O and available memory before resuming the paused screen benchmarks.

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

## Integrated Intel and AMD acceptance attempt

Deployed source `9b2fac55fc69732999c33929f319d6ef78507b68` combines Intel
`5f8ec779`/`0eab1309` checked cleanup with AMD `d17f940f` measured sort-peak
accounting. Conflicting sort test hooks were reconciled so both fault oracles
remain active. The Intel helper tuning and read-admission experiment are absent.
Budgets and cache format are unchanged from the compact-index correction.

All eleven hosts match the 858 committed build inputs and requested live
RDMA/direct-I/O/files profile, with commands enabled and packing/expiry disabled.
Full native `make -j4 test` and six full-library query ASan/UBSan/leak checks,
plus the inventory helper sanitizer, PASS on ORCD. Orderly deployment and seven
acknowledged-byte markers PASS; initial server readiness is 92.003 seconds.

Both publishers subsequently publish complete v6 catalogues with 15,208,532
inodes and 14,473,155,513 file bytes per generation. Observed aggregate bytes
remain below 32 GiB. The real 400 GiB subtree stats returns exactly 1,638,400
files and 429,496,729,600 bytes, with refresh error zero. This is catalogue
coverage, not full payload verification.

The first stats preflight during catalogue construction **FAILS** its unchanged
30-second timeout. Its logs are retained separately. A post-publication attempt
passes the same stats deadline and proceeds to focused commands, concurrent
queries, orderly cold restart and full payload verification. Those gates remain
OPEN until completed; this later probe does not erase the initial failure.
