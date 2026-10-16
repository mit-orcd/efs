# ORCD package 1 — deployment and open scale gates, October 13

Current user scope is feature correctness/acceptance; latency/resources are
measurements and existing bounded gates, with no new performance tuning.

Current live frozen devel source is `f7e3c81e2d5bd4715b6c6dc86debb10b093c0d03`.
Package 1 remains OPEN; all-gates acceptance is not yet PASS. See the
[three-package assignment](three-work-packages-20261013.md). Package 2 fixes
are being qualified on AMD; ordinary-file Intel work is independent.

## Initial devel deployment

Frozen committed source `19b14609fe4d9881648d120fa76ce13a4bd5d385` built on
all eleven ORCD nodes. Source input checks, actual native RDMA descriptors,
server direct-I/O/files arguments and client/server command flags pass.
`EFS_EXPIRY=0` are explicit.
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
startup; helper creation failure falls back to caller work. No cached counter replaces a scan and no durability
barrier or quota reserve is removed.

Actual extracted helper regression PASS locally: zero/default, one, two, six
and maximum roots; exact once per root, max4 overlap, complete join, unknown
root/error and overflow refusal, and first/partial helper
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
Expiry and unplanned-event repair remain deferred/excluded.

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
RDMA/direct-I/O/files profile, with commands enabled and expiry disabled.
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

The post-publication focused attempt finds two additional **FAIL** results:
`efs_commands` (34.004 s) and `efs_command_options` (33.153 s) each exceed
an individual peer `find` 30-second deadline; `efs_command_pages` PASS (60.984 s).
A read-only syscall trace of the options query shows successful continuation
reads, not a single blocked call: consecutive opens take 5.864, 6.736, 2.815,
2.058 and 3.170 seconds. The source selects a parent-key range only for LOCAL
immediate listings or recursive leaf directories. Recursive directories with
children use the global name-index cursor, scanning unrelated entries in the
15.2-million-inode catalogue even for this 13-file subtree. Bounded recursive
subtree continuation is an open acceptance correction; no deadline widening or
helper tuning has been applied. AMD has the exact reproducer and trace finding.

The six-suite attempt finishes with commands/options/virtual/load FAIL and
pages/large-delta PASS; temporary peer drains and unmounts normally. Virtual
mutation and nested-authorization checks do not observe changes within their
bounds; load readers return BUSY throughout. Logs show refresh QUOTA while an
older catalogue remains pinned. Conservative aggregate accounting may correctly
refuse a third approximately14.48GB generation; investigate retained stream
lifetime alongside subtree continuation, preserving the32GiB limit.

Independent owner-name/numeric checks PASS on all seven clients. Default,
human-readable, help and JSON stats checks also PASS on all seven after fixing
an operations-harness oracle that incorrectly required the2TB export root to
use GiB rather than TiB. The400GiB fixture oracle remains exact. Seven active DD
workloads appear in resource samples; concurrent acceptance runs under that
ordinary I/O, separately from the paused tiny-file screen launchers.

Seven-client concurrent pre-cold gate: all21 `.efs` query cases PASS (3/client,
jobs3, unchanged30s), but the accompanying core gate **FAIL**:1501/1512 PASS,
11 failures. Ten are EBUSY in deep mkdir, name creation, concurrent create/unlink
or unlink; one concurrent high/low extension reports EOF8,519,680 rather than
8,388,609 on fcstor010. These are ordinary POSIX acceptance failures under
jobs16/client and the active DD workload; do not label the overall concurrent
gate PASS or hide them behind the query-only result. A separate same-profile
EOF reproducer repeats the existing POSIX oracle160times/client with16workers:
4/1120 FAIL on fcstor008/009/012, all the same chunk-rounded EOF; failed
fixtures remain. Seven isolated single-case reruns PASS but do not clear it.
Existing cache/publication tracing on temporary clients gives960/960 PASS and
normal drains; a traced concurrent bulk-I/O reproduction is running. Post-cold and full payload gates remain NOTRUN.

Reproduce the shared-core EOF defect independently of `.efs` with
`python3 tests/live/concurrent_extend_eof.py /tmp/efs-mount --cases 160 --workers 16`
on each client simultaneously. The script reuses the committed POSIX high/low
extension oracle, forks no reader, retries no operation and preserves failed
fixtures. Requires an active ordinary-file mount and the source tree's
`tests/posix/posix_suite.py`; run against a qualified native binary, recording
source/build/profile. Successful owned fixtures are removed. Apply any
shared-core fix first under package3's Intel disabled-feature profile, then
requalify ORCD RDMA/direct-I/O with commands enabled. Human authorizes Intel
coordination after its current network workload finishes; do not interrupt it.

The committed wrapper77e922fc is qualified separately from the installed9b
source, without changing its858 deployed inputs. SHA256
`ee4e60168b7b7e7193d1b4d989a4236b30fe7e2beb66c7383acba758ca1d6c13`.
A second160-case/16-worker run on all7 existing mounts finds **12/1120 FAIL**:
nine chunk-rounded EOFs, one EOF shrink to1,048,577 and two cleanup EBUSY with
correct size. The shrunk file is inode13242869 at
`/tmp/efs-mount/extend-eof-x9n53rmk/124/f`; later readonly checks on fcstor012
and007 agree on size1,048,577, low byte0x4c and empty high-offset read.
Do not call this only an oversize issue or a transient local stat. Traced fresh
clients pass another960 cases while each also writes/deletes2GiB; all three
temporary mounts drain normally. Those passes do not clear long-lived-client
failures. Intel has this qualified handoff and must finish its current work
before applying shared-core fixes on Intel/devel with the disabled-feature
profile. Raw wrapper JSON and traces remain in operations/BULK, outside Git.

## Helper retry window correction

The initial stats timeout also exposes a helper-bound gap: `read_result` retries
up to8 transient refusals, each backed by the existing8-second client RPC budget.
The candidate keeps those request budgets and short-refusal retry count, but
stops retrying after one8-second monotonic window; a sleep cannot start another
attempt after that window expires. It does not interrupt a filesystem call or
claim a new end-to-end hard timeout. Busy catalogue startup now has a bounded
retry window rather than multiplying the RPC budget across8 refusals.

All34 command-source/protocol/helper tests PASS locally. New deterministic
oracles cover a slow refusal, expiry during sleep and successful short transient
recovery. Restoring the old helper fails the slow-refusal oracle (8 opens rather
than1), so its negative control PASSes. The embedded source header is regenerated.
ORCD native/mounted qualification of this combined candidate remains NOTRUN;
all11 builds of earlier subtree-only integration8743e699 PASS but are not deployed.
Native request limits, query16/32GiB caps, cache format and feature flags are
unchanged. The original live9b failures stay recorded until final-source gates.

## Combined f7e3c81e ORCD deployment and acceptance

The frozen devel revision combines the bounded LOCAL subtree correction and
helper retry-window fix. All11 staged/installed hosts match863 committed build
inputs. Full native `make -j4 test`, all six full-library query ASan/UBSan/leak
harnesses and startup-inventory sanitizer PASS on exactf7 source. Native fcstor006
efsd SHA256 `035a3b5c9a6f20cf439e6936e6f5fdcbae36fe0b046fbf0d67b868433b4ee45b`;
efs-fuse `016fc6642a266b5172823418d47127ac70046cbaca4357a0e9e6c788fad896ca`.
Candidate f7 is an ancestor of integrated devel5cf9fb40; only later diagnostic
script/docs distinguish that integration head, with no source/include/deps/
Makefile production difference. No working-tree mixture was deployed.

Stopped seven identified nonroot DD processes with SIGINT and stable pidfd/
UID/start-time/argv checks. Partial outputs75.95–85.92GB remain; no output was
deleted or truncated. Seven clients drain/unmount normally before four daemons
stop. Installed/startup/profile, remount and acknowledged large/512-byte marker
verification PASS on all7. Initial server readiness88.131s. Actual profile is
native RDMA/mlx5_2, server direct-I/O/files, client/server commands enabled,
expiry disabled. Real400GiB subtree stats is exact with
refresh_error0; full payload verification is still NOTRUN.

| Focused suite on f7e3c81e | Verdict | Seconds |
| --- | --- | ---: |
| commands | PASS | 8.773 |
| options | PASS | 5.871 |
| pages | PASS | 14.279 |
| virtual interfaces | PASS | 85.247 |
| loaded commands | PASS | 22.690 |
| large delta, payload | FAIL: EIO during write/flush/fsync | 12.077 |

The temporary peer drains normally. The payload failure stops the aggregate
gate driver; it is not relabelled by the five successes. Main-client log records
REPORT type67 resource-busy after8016ms, one chunk, inode8562082, then fsync
EFS_ERR_BUSY; the application sees EIO. Preceding opid-admission messages show
oldest12431/next12447/live3 and oldest12433/next12449/live4 at unchanged window16.
Source has a fixed1000×1ms opid admission poll and unchanged8s sync REPORT budget.
Intel has the evidence for correctness investigation, without budget/window
widening. Earlier EOF growth/shrink and ordinary EBUSY findings remain OPEN.

Separately labelled metadata-only large delta PASS:1025 exact creations, updates
and mtime/zero-size fields, followed by512 deletions, rename and hardlink. Both
peer syscall and query count/byte oracles agree with refresh_error0. Phases take
5.158/8.586s; temporary peer stop PASS. It isolates query functionality but does
not replace payload qualification. Independent all7 stats/owner and concurrent
checks continue; post-test cold and complete payload/path gates remain OPEN.

## Work on hold — 2026-10-13

The user explicitly paused `.efs` work. Package 1 is **ON HOLD, acceptance
OPEN/FAIL**, not complete. No further tests, fixes, deployment or restart are
authorized under this paused work. Intel and AMD have been asked to wrap up
safely, preserve candidates/evidence, document their exact status and pause the
coordinated work. Their wrap-up commits are not yet received or integrated.

Last deployed source remains f7e3c81e2d5bd4715b6c6dc86debb10b093c0d03, with
the RDMA/direct-I/O/files profile and `.efs` client/server enabled; expiry disabled. The running cluster and preserved stores are
left intact. Resource/cache observers receive their normal stop flags; no
daemons are killed or storage erased. Seven tiny-file launchers remain paused;
seven DD processes were stopped cleanly and their partial outputs preserved.
Resume instructions and stable process identities remain in
`operations/devel-efs/package1-paused-screen-launchers.json`, benchmark screen
state and DD clean-stop records. Do not blindly resume the queued root-directory
benchmark command on fcstor010; preserve historical invalid fixtures.

The completed final concurrent attempt has **1511/1512 ordinary cases PASS,
one FAIL on fcstor013; all21/21 virtual query cases PASS**. Some queries expose
stale snapshots with refresh_error -13, so successful query cases do not prove
refresh health. Raw per-host TSV/logs remain at
`/home/erbmi1/orcd/scratch/efs/orcd-test/fcstor007` through `fcstor013`, prefix
`profile-posix-f7e3c81e2d5b-400g-pre-cold`; local aggregate is
`operations/devel-efs/400g-posix-pre-cold.json`. The exact failing ordinary case
needs log review on resumption. Concurrent human-readable/JSON stats and owner
checks also FAIL EBUSY on fcstor013 while overlapping this workload. This differs
from the intended sequential gate order; causation is unproven and the failures
are retained. Sequential reruns are NOTRUN.

Resume priorities: review Intel's qualified admission correction (currently an
uncommitted candidate, preserving receipt window16 and REPORT8s); obtain AMD's
exact-f7 helper qualification; resolve payload/fsync EIO and ordinary EOF/EBUSY
failures; rerun all focused and concurrent gates on one frozen source; then
verify same-source orderly cold restart within300s and every byte/path of the
real400GiB fixture. Final-source complete payload/path and post-test cold gates
remain NOTRUN. No final acceptance latency/resource result is claimed.
