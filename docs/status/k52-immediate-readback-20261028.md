# K52 — immediate read-back under `.efs` load

`agent/devel-task-x`, tested code `86035a633`, base `3debca7ec`.
**TOTAL 522 FAIL 5 NOTRUN 5 (662s)**; all five failing commands reproduce
on base. Earlier quota failure stays unresolved. Evidence:
`docs/status/k52-immediate-readback-20261028.md`. D3: 46,443 exact files;
K52 OPEN, original buffers missing, four-host follow-up awaiting approval.
Raw: `/home/erbmi1/efs-artifacts/taskX-20261028/`. Assigned-host work complete.

## Purpose and current scope

Investigate K52 without treating a later exact read as a passing first read.
The private cluster has four daemons on one node9900 host, TCP, O_DIRECT,
packing and default `.efs` policy, 4 GiB per daemon. It is regression evidence,
not four distinct failure domains or A4 acceptance. Existing orcd-mini and
orcd-test stores and mounts are not part of this experiment.

## Corrections to the assignment

The retained [Task V report](a4-lab-topology-dryrun-20261028.md) and its original
`driver-run1/gates/d3-efs-load/07-efs-load.log` do **not** contain observed
hashes/lengths or writer IDs for the original eight failed comparisons.
Richer diagnostics were added afterward. They cannot recover information
that the failed operations never recorded.

| Operation | Time UTC, Oct 28 | Original writer | Original observed bytes/hash | Individual K10 attribution |
| --- | --- | --- | --- | --- |
| f00170 | 15:57:09.056 | unavailable | unavailable | unclassifiable |
| f00628 | 15:57:19.624 | unavailable | unavailable | unclassifiable |
| f00679 | 15:57:20.887 | unavailable | unavailable | unclassifiable |
| f00781 | 15:57:23.860 | unavailable | unavailable | unclassifiable |
| f01174 | 15:57:37.207 | unavailable | unavailable | unclassifiable |
| f01282 | 15:57:41.039 | unavailable | unavailable | unclassifiable |
| f01528 | 15:57:52.588 | unavailable | unavailable | unclassifiable |
| f02582 | 15:58:55.079 | unavailable | unavailable | unclassifiable |

Two paths were later narrowed to writer 5 by namespace state; this does not
recover their failed buffers. Task V's later 64/64 exact candidate checks on
each of two mounts remain later observations only. Its original FAIL stands.
The read follows `write_exact`, whose finally closes the descriptor after
fsync. Thus this is **after close**, not an fsync-to-close window. The files
are fresh exclusive creates, not successive versions of one inode.

K10's trigger is a fork racing another thread's FUSE direct-I/O read; fsync
itself is not the trigger. The caller's destination can retain stale bytes,
which need not equal a complete immediately preceding file. A previous-file
hash is useful diagnostic evidence, not a necessary/sufficient causal test.

## Harness change and negative controls

`efs_cmd` explicitly uses `os.posix_spawn`, with separate temporary output
files, original command arguments, nonzero status propagation and the original
timeout. It never forks the address space holding mounted-I/O buffers. This
avoids dependence on `subprocess` fast-path conditions: the old `cwd='/'`
launch precluded CPython's posix_spawn path. The virtual commands locate their
directory from their script path, not the launcher's working directory.

Reads, writes, fsync, close, query count/deadline requirements, exact catalog
answers, and final expected payload checks are unchanged. No retry or
any-version acceptance is added. K10 remains the user's documented kernel
limitation; this is not a kernel or EFS data-path repair.

The Task V driver regression checks prohibit both Python fork entry points,
observe actual posix_spawn calls, check large stdout/stderr, literal arguments,
nonzero exit propagation, original timeout enforcement and child reaping.
Its existing wrong-payload negative control still fails the first read and
preserves that evidence after a later exact read.

`tests/live/k52_readback.py` runs D3's original fork-launching queries with
in-process or spawned readers. It retains original failures and records
expected/observed length/hash, previous expected hash and equality, phase and
reader mode. A spawned reader uses a distinct address space; it does not retry
a failed parent read. It uses the same 200 static files, eight writers,
four query workers, 300-second load and 10/30-second deadlines.

## Reproduction

On an idle private host with FUSE, a compatible libfuse and O_DIRECT support,
build the frozen source. Run the original base first, then the candidate in
separate scratch/output directories. Preserve all failures and do not relaunch
an output directory. Example original/corrected driver invocation:

```sh
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json \
  --topology loopback-4 --candidate BUILD_ID \
  --port-base 19640 --private-port-base 19740 \
  --gates identity,topology,d3-efs-load \
  --out OUTSIDE_GIT/NEW_RESULT --work /tmp/PRIVATE_NEW_WORK --keep-work
```

`topology` reports NOTRUN on one physical host even with four members. It must
not be presented as distinct-host acceptance. For the isolated-reader control,
start an owned cluster with the same profile, then run:

```sh
python3 tests/live/k52_readback.py PRIVATE_MOUNT NEW_FIXTURE --reader spawn
python3 tests/live/orcd_raft_kv/fork_repro.py PRIVATE_MOUNT/NEW_FORK fork --seconds 30
python3 tests/live/orcd_raft_kv/fork_repro.py PRIVATE_MOUNT/NEW_SPAWN spawn --seconds 30
```

Run the code gate in detached screen from the frozen source, preserving its
output before another run:

```sh
env -u EFS_TRANSPORT EFS_FUSE_PREFIX=/data1/efs/deps/libfuse \
  EFS_GIT_ID=86035a633 EFS_GIT_BRANCH=agent/devel-task-x \
  EFS_GIT_DESCRIBE=86035a633 EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 \
  scripts/make_test_summary.sh
```

Base controls use the same Python commands listed in the failure table,
fresh private TMPDIRs, base identity `3debca7ec`, and a separate
`ASAN_OPTIONS=detect_leaks=0` invocation for address-only coverage. B6:
`EFS_B6_MODES=full EFS_B6_KEEP=1 EFS_B6_ARTIFACTS=NEW_OUTPUT python3 tests/test_b6_unavailable.py`.
It retains its original 16 MiB limiting-member quota and existing deadlines.

The `fork` control deliberately exercises the known kernel limitation; its
failure is preserved, never waived as a passing data-path check. Stop clients
cleanly, prove mount absence, stop only owned daemons and preserve failed
fixtures until investigated. Never operate on existing user clusters.

## Qualification results

Corrected D3 `86035a633` **PASS**: 46,443 I/O operations and all 46,443
successful files exact on final read-back, 6,515 queries over six generations,
zero I/O/query errors and no deadline violations. I/O maximum 0.226 seconds;
query maximum 15.282 seconds; load converged in 54.303 seconds. The existing
bounded BUSY rule handled 12 query refusals (longest episode 20.879 seconds,
within the unchanged 30-second deadline); data writes returned no BUSY/EIO.
The body took 503.577 seconds. Overall rc 3 still records single-host topology
NOTRUN. Timing/throughput differences from baseline are not a controlled
performance attribution: salts, placement, election history and scheduling
are not held identical.

First code gate on the same candidate:

```text
TOTAL 522 FAIL 6 NOTRUN 14 STATUS FAIL (584s) OVERBUDGET 4 BUILD 10s
```

| Failing command | Recorded observation | Base/control status |
| --- | --- | --- |
| test_readindex_pair.py | LSan fatal, ptrace unavailable | same fatal on untouched base; address-only PASS |
| test_host_outbox.py | same | same fatal on untouched base; address-only PASS |
| test_raft_ack_window.py | same | same fatal on untouched base; address-only PASS |
| test_stage_evict_lru.py | same | same fatal on untouched base; address-only PASS |
| EFS_B6_MODES=full test_b6_unavailable.py | W0 succeeds; test expected every later write ENOSPC | isolated base PASS; later candidate gate PASS; original failure remains unresolved |
| test_make_test_recipe.py | test_code_graph.py neither wired nor listed with reason | reproduces on untouched 3debca7ec |

The first gate inherited `EFS_TRANSPORT=tcp` from the D3 controller. This
incorrectly constrained the RDMA capability checks despite an ACTIVE mlx5_0
port. The original result is retained; a second complete gate removed that
override (and did not suppress sanitizer checks):

```text
TOTAL 522 FAIL 5 NOTRUN 5 STATUS FAIL (662s) OVERBUDGET 6 BUILD 0s
```

Nine RDMA commands now ran successfully: declaration frame path,
`test_rdma_xprt`, and seven segmented modes. This is hardware unit coverage,
not a D3 RDMA profile or distinct-host RDMA qualification. Remaining NOTRUNs:
external cluster stop/remount scripts absent; RDMA Memcheck and real leak
fixtures lack valgrind; normal `test_follower_bound` lacks its fault-only
subcase; sanitizer runtime-probe fixtures are unavailable on this host.

Four LSan fatal failures and the graph-test recipe failure remain, and each
reproduces in an unchanged-base focused invocation on the same host. Host
policy is `ptrace_scope=2`, `TracerPid=0`, seccomp off and SELinux disabled.
All four focused address-only controls (`ASAN_OPTIONS=detect_leaks=0`) PASS;
they **do not qualify leaks**. Neither complete gate used that suppression.
The actual gate status remains FAIL. Full-suite baseline was not run; the
base comparison is these focused commands, not a paired whole-gate A/B.

The graph file itself is tracked. The missing wiring is a test-integration
issue: `test_code_graph.py` includes Git-backed source selection and hook
fixtures. Its owner should wire it with its prerequisites or give an explicit
dedicated-gate reason; silently omitting it is what the recipe check refuses.
No graph-acceptance result is claimed by Task X.

The B6 failure is separate and **not cleared by its later passes**. Its
three-member fixture fills node 3's 16 MiB quota until a 1 MiB file is refused,
then expects every 300,000-byte/small write to fail. That inference needs
review: residual capacity and concurrent packing/reclaim can admit smaller
fully protected writes. The closed failed client log has no degraded-ack
lines, but later full-mode assertions and cold checks were skipped by the
failure, so W0's protection is not qualified here. The isolated unchanged-base
control passed all checks, including 48 acknowledged files exact after restart;
the second candidate gate also passed B6. Neither proves this first observation
harmless. The original stores remain archived for investigation. No B6 oracle,
quota, durability or product behavior was changed.

No timeout or budget was increased. The first gate has four timing overruns;
the second has six (K38, stop-progress, K43, and three D39 model shards), all
listed with measured durations in their retained summaries.

Baseline `3debca7ec`: D3 **PASS**, K52 **NOT REPRODUCED** on this run.
37,506 successful I/O operations and every one of their payloads exact on
final read-back; 3,881 queries over six generations, zero I/O/query errors,
I/O maximum 0.311 seconds, query maximum 17.948 seconds. Load catalog
converged in 48.889 seconds. Gate body 1,192.783 seconds. The driver overall
is NOTRUN (rc 3) because four members on one host do not satisfy the topology
gate. This run used queries on the second local mount; Task V used the writer
mount. The same-mount controls are separate.

Spawned-reader same-mount D3 **PASS**: 17,995 files exact immediately in their
spawned readers and again in the final read-back, 6,037 queries over six
generations, zero I/O/query errors, I/O maximum 0.441 seconds, query maximum
9.782 seconds; load convergence 60.236 seconds. Original fork-launching queries
remain in this control. Reader process startup reduces write throughput, so
this is not an equal-throughput performance A/B or proof about the eight
historical buffers.

The existing 30-second fixed-file kernel control reproduces W121/K10:
**78,689 wrong reads / 240,982 with fork**, all eight readers affected; the
first recorded successful 512-byte reads left the caller's `?` prefill in
place. The spawn control has **0 wrong reads / 294,327**. The former is an
expected deliberate exposure of the documented kernel limitation, recorded as
a failing control, not a passing correctness check. All diagnostic clients and
four daemons then stopped cleanly, rc 0 and mounts absent.

The first queued in-process comparison refused port 19640 before starting a
daemon or workload: the next owned fixed qualification already held the port.
The refusal and rc 1 remain in `inproc-controller.log` / `inproc.rc`; no data
comparison ran. The original queue pause did not prevent that scheduling race.
The subsequent in-process comparison used fresh ports 19660–19663 and new
`inproc2` fixture/output paths after the first gate finished. No occupied-port
check was bypassed.

Same-mount in-process D3 with the **original fork-launching queries PASS**:
36,105 successful files exact immediately and on final read-back, 5,519 queries
over six generations, zero I/O/query errors, I/O maximum 0.314 seconds, query
maximum 17.642 seconds. Five bounded query BUSY refusals were handled under
the original rule (longest episode 17.501 seconds). Thus K52 is **NOT
REPRODUCED**, including the closer same-mount pattern. All clients and daemons
stopped cleanly. This prevents the stronger claim that only in-process readers
fail, or that these runs establish K52 equals K10.

Candidate driver regression suite **PASS** on node9900. Source content
verified against Git objects: base 2,280 files and candidate 2,281 files, no
mismatch; candidate verified again after both gates. Product `src/` and
`include/` equal Task V's `b2d57e421` and have no Task X changes. Candidate binary:
`efsd 86035a633 (branch agent/devel-task-x, built 2026-10-28T19:32:40Z on node9900)`.

Host: AMD EPYC 9474F, 192 logical CPUs, 385,951 MiB RAM; RHEL kernel
`5.14.0-687.39.1.el9_8.x86_64`, Python 3.9.25, private libfuse 3.16.2 from
`/data1/efs/deps/libfuse` (system 3.10.2 is insufficient). Relevant fuse module
strings include `iov_iter_get_pages2`; this is supporting API evidence, not
a patched-kernel A/B. Ptrace is disallowed, so syscall tracing is NOTRUN.
Python entry-point instrumentation records the original command selecting
`_posixsubprocess.fork_exec` and the explicit replacement selecting
`os.posix_spawn`; it is not a syscall trace.

## Evidence retention and cleanup

Stable raw root on **node9900**:
`/home/erbmi1/efs-artifacts/taskX-20261028/`.
Final `SHA256SUMS`: **281 files**, verified with `sha256sum -c`;
manifest SHA-256:
`a1a86a2eedcaa125c9eda5cc807853a80b219cc0faacf9dff181ca67c88a6a90`.
`gate-first/` and `gate-auto/` keep separate summaries, timings, exact command
lists and every failure log. Original/corrected D3 outputs and closed daemon
and client logs, controls, build/source manifests, controller scripts and
cleanup receipts remain. Available preliminary Mac-driver logs are retained
separately; those attempts encountered sandbox/Linux `/proc` limitations and
are not qualification results. The vendor-private fork interception was fixed
before the qualified Linux driver run.

Eight critical Task V files were copied and hash-verified without modifying
the original evidence: original D3 result/output, run/cluster descriptions,
original manifests and the two later-read JSON records. This is a critical
subset, not a copy of every Task V capture. The copied original top manifest
matches its published hash
`16cb6e720c06b8b2e8972ee1a8726f3b988563c0530d01a732a3bfd205f351c5`.

The failed B6 fixture is **kept for the OPEN investigation**:
`failed-b6-first-0.tar.gz`, 52,397,982 bytes, all **915 regular files** verified
against closed-source lengths and SHA-256s, plus full gzip validation;
archive SHA-256:
`6c2002946b9b0a06997743995454428eca6444334727b143987861720c483fd4`.
Its member inventory and retention record are included in the master manifest.

Only after preservation verified did `~/bin/edelete --force --delete` remove
`/tmp/efs-taskX-20261028`. Cleanup rc 0; the tree is absent, with zero owned
processes/mounts before and after. `PRUNED.txt` names removed source/build trees,
passed D3 stores/payloads, successful B6 control stores and test scratch.
Exact-store replay for passing D3 runs is forfeited; committed recipes remain.
Existing user clusters and their data were not stopped, reformatted or deployed.

## Verdict and remaining work

The fork-free query launcher and its regression are implemented and exercised;
the corrected one-host D3 gate passes. The known kernel race is independently
reproduced, but both original-query D3 controls pass too. No new product read
failure is demonstrated, no packing change is justified, and the eight
historical buffers cannot be reconstructed. **K52 remains OPEN and blocks
alpha**; changing its row to FIXED or DECIDED would overstate this evidence.

The assignment's per-original-mismatch causal classification is impossible
from the retained observations. A new four-host TCP reproduction with the rich
diagnostics is the useful next test. Approval was requested for fresh private
fcstor012–015 scratch, 4 GiB/member, ports 19680–19683; Task X explicitly
requires permission before reusing those hosts. No reuse has occurred.
Distinct-host follow-up, D3 over RDMA and patched-kernel verification remain
NOTRUN. The five baseline-reproduced full-gate failures and the intermittent
quota-oracle observation remain separate integration/qualification work.
