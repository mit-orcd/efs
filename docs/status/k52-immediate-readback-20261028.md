# K52 — immediate read-back under `.efs` load

`agent/devel-task-x`, tested code `86035a633`, base `3debca7ec`.
First gate: **TOTAL 522 FAIL 6 NOTRUN 14 (584s)**; base reproduces the recipe
failure, other controls pending. D3: 46,443 exact files; kernel fork control
FAIL. K52 remains OPEN (historical buffers unavailable). Evidence:
`docs/status/k52-immediate-readback-20261028.md`. Raw:
`/home/erbmi1/efs-artifacts/taskX-20261028/`; qualification still RUNNING.


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
| test_readindex_pair.py | LSan fatal, ptrace unavailable | pending |
| test_host_outbox.py | same | pending |
| test_raft_ack_window.py | same | pending |
| test_stage_evict_lru.py | same | pending |
| EFS_B6_MODES=full test_b6_unavailable.py | W0 succeeds; test expected every later write ENOSPC | pending; original failed fixture retained |
| test_make_test_recipe.py | test_code_graph.py neither wired nor listed with reason | reproduces on untouched 3debca7ec |

The first gate inherited `EFS_TRANSPORT=tcp` from the D3 controller. Node9900
has an ACTIVE mlx5_0 port; its RDMA skips cannot establish absent hardware.
A second complete gate with that override removed is queued, preserving this
first result. No leak suppression is applied to either complete gate. Focused
address-only controls will be labelled as such, with leak qualification NOTRUN.
The first gate's other skipped capabilities are missing external cluster
scripts, valgrind, a normal-build fault-only subcase and the sanitizer runtime
probe. Full skip lists and timing overruns remain in the original summary.

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
A fresh in-process comparison is queued after the gate on ports 19660–19663,
with new `inproc2` fixture/output paths. No occupied-port check was bypassed.

Candidate driver regression suite **PASS** on node9900. Source content
verified against Git objects: base 2,280 files and candidate 2,281 files, no
mismatch. Candidate binary:
`efsd 86035a633 (branch agent/devel-task-x, built 2026-10-28T19:32:40Z on node9900)`.

Host: AMD EPYC 9474F, 192 logical CPUs, 385,951 MiB RAM; RHEL kernel
`5.14.0-687.39.1.el9_8.x86_64`, Python 3.9.25, private libfuse 3.16.2 from
`/data1/efs/deps/libfuse` (system 3.10.2 is insufficient). Relevant fuse module
strings include `iov_iter_get_pages2`; this is supporting API evidence, not
a patched-kernel A/B. Ptrace is disallowed, so syscall tracing is NOTRUN.
Python entry-point instrumentation records the original command selecting
`_posixsubprocess.fork_exec` and the explicit replacement selecting
`os.posix_spawn`; it is not a syscall trace.

No historical failure or release gate is closed by the proposed fix.
Distinct-host rerun, RDMA and patched-kernel verification are NOTRUN.
