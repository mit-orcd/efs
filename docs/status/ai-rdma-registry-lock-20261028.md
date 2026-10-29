# Task AI — RDMA registry lifetime lock, October 28, 2026

**COMPLETE, scoped measured no-change; alpha qualification is not PASS.** Task AI in the
[alpha checklist](alpha-1.0-2plus1-checklist.md) measures before choosing an
optimization. Native real-RDMA transport smoke and one-rank filesystem/IOR
smoke PASS scoped. All 27 matrix cells and the stream/churn measurements
completed. The measured acquisition fraction stays below the 5% trigger;
**keep the current lifetime lock for this single-device scope**. No throughput
improvement from changing that lock has been demonstrated. Corrected full gate:
**546 commands, 1 FAIL (K55, independently reproduced on the untouched base),
4 NOTRUN, 725 seconds; build 34 seconds, 7 overbudget.** The task sheet allows
the base's same failure; it remains an OPEN alpha blocker, not a passing B6
gate. No existing cluster was changed, and no merge or push was performed.

Subsequent K55 work is independently qualified on its own branch: see
[the fix and retained results](k55-slow-commit-20261028.md). That does not
change this page's historical failed gate or waive K53/K54.

## Placement and source

The user authorized trying fstor007, node9900 and fcstor007–015, with
`/data1/01`. node9901 was occupied by Task H; node9900, fstor007, fcstor007
and fcstor012 also showed other EFS/test work. No existing cluster was stopped
or changed. fcstor015 showed no EFS/test workload in the preflight and has
active mlx5_0 and mlx5_2 devices. Measurements select **mlx5_0 only**.

fcstor015: AMD EPYC 7402P, 24 cores/logical CPUs, two NUMA nodes, RHEL 9.8,
kernel `5.14.0-687.39.1.el9_8.x86_64`,
`/data1/01` on `/dev/nvme0n1`, approximately 7 TiB free at preflight.
The user's directory authorization supersedes the task sheet's original
node9901-only assignment. These are single-host hardware-RDMA measurements,
not multi-host fabric qualification or a two-device scaling result. mlx5_0's
link layer is Ethernet/RoCE, firmware `16.34.1002`, active MTU 1024. Do not
describe this as multi-host InfiniBand qualification.
All three logical members share this host/NVMe: no physical-node-loss or
power-loss protection acceptance follows from this fixture.

Branch `agent/devel-task-ai`, base `486aa3a46`. Exact native export and
binaries: `d1bf8e2bd`, whose only changes from that base are measurement tools.
Disposable diagnostic export: the same source, with registry lock calls
wrapped by `rdma_registry_probe.py`; build ID `d1bf8e2bd-ai-probe`.
The exported shipping `src/common/rdma.c` is unchanged. Every cluster uses
matching client/server binaries, never mixed builds.

Build prerequisites: the private libfuse under
`~/orcd/scratch/efs/orcd-test/deps/libfuse`, its `lib/pkgconfig` and runtime
`lib` on the respective paths; FUSE linked with an rpath. `make -j8 all`
and `tests/test_rdma_xprt tests/test_rdma_segmented` built successfully.
Native and probe binary hashes, diagnostic input/output hashes and build log
are retained with the raw evidence below.

## Measurement and acceptance

The shipping source has **four acquisition sites**, in `harvest_recv_wcs`,
`conn_create`, `efs_rdma_conn_destroy` and `efs_rdma_totals`; the task sheet's
"12 sites" is not the acquisition count at this source. The collector checks
this exact set and refuses source drift. It leaves all critical-section
boundaries intact, including generation validation, ring publication and
eventfd wake under the registry lock. `recv_handoff_mu` is untouched.

Registry-only diagnostics accumulate bounded per-thread calls, acquisition
intervals and hold intervals after unlocking; no per-CQE logging. Overflow,
in-flight records and missing process dumps make the evidence INCOMPLETE.
The acquisition interval includes clock/lock-call overhead. Its first-to-last
span is not the application window. Compare the matched native and probe runs
before interpreting instrumented percentages. An isolated one-call percentage
is not a load result. Pure parser/source round-trip and Linux native collector
tests passed, including 8,000 acquisitions from eight joined threads.

Runner `d433ab689`, `tests/measure/rdma_registry_cluster.py`:

```sh
python3 rdma-registry-cluster-d433ab689.py \
  /data1/01/efs-taskAI-20261028 \
  /data1/01/efs-taskAI-20261028/matrix-d433ab689 \
  --ior "$HOME/orcd/scratch/efs-io500/io500/bin/ior" \
  --mpi /usr/mpi/gcc/openmpi-5.0.10rc2.2605121430/bin/prterun
```

Fresh private three-member loopback clusters, 16 GiB/node quotas, eight writer
threads/node, automatic formation, discovery disabled. Fragment I/O is always
O_DIRECT; no removed selector is passed. Default background packing/query
workers remain enabled; expiry policy is unset. Each cell uses one FUSE mount.
TCP/native RDMA/probe RDMA, 1/8/32 streams, three repeats, alternating native
transport order by repeat. Local MPI is oversubscribed at 32 ranks on 24 CPUs;
that limitation applies equally to paired controls.

Each cell performs 30 seconds of fixed-rate 4-KiB rewrites, 20 scheduled
operations/second/stream, eight fixed files/stream, checked writes/fsyncs and
exact final payloads. Slow operations may overrun the scheduled duration;
there is no retry-to-green. IOR easy: 64 MiB/rank, file-per-process, 1 MiB
transfers. IOR hard: shared file, 128 segments/rank of 47,008 bytes. Both have
separate write and checked read runs with timestamp payloads and fixed seed.
These bounded geometries are **not full IO500 scores** or the earlier K32
multi-host acceptance workload.

The observer retains daemon/client CPU deltas, 10-Hz stat syscall samples and
two-second process metadata samples. FUSE attribute caching applies: stat
latency is not a per-RPC latency distribution. Observed foreign work or an
unknown observation prevents timing qualification; sampling is not an
exclusive host lease. No profiler runs during this matrix. IOR commands have
1,800-second bounds. Clean `client.sh stop`, mount absence, client exit zero
and all three daemon exits zero are required. A failure preserves its live
state/stores for diagnosis, without lazy detach or forced shutdown.

Follow-up runner `4ea7a9885` continues after this existing matrix; it never
launches a duplicate. It runs source-bound, real-verbs 1/8/32-stream request/reply
checks with concurrent connection destruction/reuse (32 cycles, overlap
count recorded), three repeats and separate TCP/native/probe controls.
Every echo is checked and all connection/assembly ownership must return to
zero. Its non-RDMA microbenchmark control uses **AF_UNIX socketpair byte
streams**, despite the `tcp` dispatch label; it is not a TCP/IP benchmark.
Only the filesystem matrix above supplies the actual TCP/IP IOR control.
Its extra wake diagnostic reports **poll-return to the first completed,
non-broken pending-ring dispatch**, including the eventfd handoff. The timestamp
is after `pend_push` returns, not at its earlier head publication store. It
excludes kernel wake-to-run, NIC-to-CQE and consumer-handler dispatch delay.
No broader wake latency claim follows from that measurement.

Real-RDMA `test_rdma_xprt`, all seven segmented modes, handoff/progress/fork
tests, physical reclamation and POSIX over an RDMA mount passed. Core POSIX:
218/218, no fail/skip/notrun. Peer checks, rejoin/restart reclamation and
empty physical inventories also passed. The full `make test-all` summary is
complete with the base failure and four explicit host limits recorded below;
exit zero alone is not proof of every live gate.

Task criterion: investigate an optimization if 32-stream waiting exceeds 5%
or a demonstrated end-to-end gain justifies it. Below threshold, measured
no-change is valid. No lifetime pinning, registry partitioning or dispatch
batching has been implemented or selected.

## Closed results and retained failure

All 27 fresh-cluster cells passed final rewrite payloads, IOR checked reads,
sampled no-foreign-work checks, clean mount stop and all daemon exits. The
acquisition percentage below is `sum(acquisition interval)/first-to-last span`
for each receive-poller record, across three repeats and four processes/cell.
It includes clock/lock-call overhead and startup/background/shutdown intervals;
it is not an exact production blocking-time or CPU-wait metric.

| Streams | Matrix acquisition fraction, min–max | Harvest lock acquisitions/s, min–max | Mean hold, min–max |
| --- | --- | --- | --- |
| 1 | 0.0037–0.0241% | 1,005–6,999 | 867–1,310 ns |
| 8 | 0.0266–0.1396% | 6,982–39,104 | 893–1,612 ns |
| 32 | 0.0614–0.2429% | 15,457–64,215 | 981–1,683 ns |

The heavier same-process real-RDMA echo/churn diagnostic completed three
repeats at each concurrency, 25,000 echoes/stream plus 32 connection cycles.
Every cycle completed both endpoints' teardown while load was still active;
all payloads matched and live connections/assemblies returned to zero.
Each 32-stream run recorded 3,200,512 harvest lock acquisitions, including
protocol control frames. This counts CQEs with an in-range registry index
before checking generation/status; rejected stale completions would count too.
It is not a NIC-wide completion or successfully published-CQE counter.

| Streams | Registry-only acquisition fraction, three repeats | Native RDMA median echoes/s | Probe median echoes/s |
| --- | --- | --- | --- |
| 1 | 0.4970–0.5307% | 40,509 | 39,874 |
| 8 | 2.5101–2.6775% | 226,683 | 203,712 |
| 32 | 2.8104–2.8448% | 243,538 | 228,319 |

At 32 streams, mean acquisition interval is 30.7–31.3 ns, mean hold
802–809 ns and harvest acquisition rate 908,275–916,284/s. Instrumentation
reduces median component throughput about 6.25% there (about 10.13% at eight
streams); it is not free. These are diagnostic measurements, not a claim of
the native application's exact mutex-blocking percentage. Holding the lock
also contains useful generation/lifetime/publication work: low acquisition
delay does not prove the poller or that work has no cost. The native echo
rate's plateau from eight to 32 streams is a separate finding, not evidence
that removing this mutex would improve it.

Wake diagnostics: one stream produced 49,987–49,995 channel-return samples
per repeat, mean 2.63–2.93 microseconds; eight streams produced 143–282,
mean 3.43–5.08 microseconds; 32 streams produced only 9–35, mean 3.32–6.57
microseconds. Busy polling normally harvests those high-load completions
without sleeping, so the 32-stream wake sample is small. The largest observed
channel-return delay was 323.331 microseconds in an eight-stream repeat.
No kernel wake-to-run, NIC arrival-to-dispatch, or high-percentile guarantee
is inferred from these samples.

Actual TCP/IP IOR medians and min–max spread (MiB/s), three repeats:

| Streams/transport | Easy write | Easy checked read | Hard write | Hard checked read |
| --- | --- | --- | --- | --- |
| 1 TCP | 160.18 (156.51–309.62) | 1004.12 (941.83–1072.82) | 53.41 (10.23–86.20) | 188.00 (174.87–190.30) |
| 1 RDMA | 140.80 (121.95–262.79) | 904.65 (894.09–979.37) | 21.35 (7.79–82.41) | 173.73 (152.73–193.43) |
| 8 TCP | 179.56 (176.84–183.72) | 1112.54 (934.97–1685.82) | 12.49 (12.18–16.79) | 127.40 (126.91–201.47) |
| 8 RDMA | 201.58 (191.60–370.15) | 945.88 (859.94–1315.19) | 14.71 (13.58–44.08) | 161.18 (159.31–182.60) |
| 32 TCP | 322.26 (175.60–322.42) | 949.33 (906.06–956.37) | 36.99 (13.06–41.33) | 137.80 (133.78–153.31) |
| 32 RDMA | 165.71 (153.01–300.99) | 896.07 (842.00–939.91) | 13.06 (12.95–35.62) | 299.88 (289.52–302.88) |

These short IOR geometries have broad write spread, fresh elections and
default background activity; leader placement was not experimentally fixed.
Reads use the writer mount and may hit caches. Do not infer a causal TCP/RDMA
ranking, storage bandwidth, or K32 acceptance from these numbers. They supply
the requested bounded control, not a repeat-until-green performance claim.

Median per-repeat p99 stat syscall latency at 1/8/32 streams: native TCP
50.92/46.98/82.97 ms, RDMA 13.24/41.41/130.08 ms. Median per-repeat p95 whole
rewrite operation: TCP 52.64/66.16/89.48 ms, RDMA 11.67/35.89/91.18 ms.
The raw array's historical `fsync_syscall_s` name includes open, write, fsync
and close; it is **not isolated fsync latency**. CPU records cover the three
daemons and FUSE client, excluding MPI ranks. Their 32-stream median summed
CPU/work-wall ratios are 3.29 cores TCP and 2.98 RDMA, not a host or per-thread
CPU saturation proof. Raw per-process deltas and phase maxima remain retained.

**Recommendation:** measured no-change for Task AI's single-device profile.
Keep generation rejection, teardown lifetime protection, SPSC ownership,
wakeup ordering and `recv_handoff_mu`. No lock-removal throughput gain was
tested or established; multi-device partitioning remains unmeasured.

- Native `test_rdma_xprt`: real mlx5_0 transport PASS; 68 application frames,
  174,703 application bytes per endpoint, zero measured/bootstrap/post-setup
  TCP payload and zero violations.
- Diagnostic transport smoke: complete bounded records, DIAGNOSTIC_ONLY;
  280 harvest acquisitions, approximately 32 ns mean acquisition interval and
  652 ns mean hold. This brief run is not the 32-stream contention verdict.
- One-rank cluster smoke: final rewritten payload checks and both IOR checked
  reads PASS; mount stop zero, no mount left, all daemon exits zero; no foreign
  work observed. The shortened five-second rewrite smoke is not a matrix row.
- First runner `251e1a1ca` failed before formation because it passed the removed
  `--direct-io` option. All three daemons exited 1. The CLI refusal and failed
  output remain retained. Runner `d433ab689` removes the obsolete argument;
  mandatory direct I/O is unchanged. This is a runner prerequisite correction,
  not an EFS correctness or performance regression.
- Waiting follow-up 1 was superseded before any workload, with its exact PID,
  argv, uid and waiting state retained in `superseded-before-work.json`, to
  require a whole connection cycle (including destruction) to overlap load.
  Follow-up 2 stopped while compiling the wake-only diagnostic: the standalone
  command used `-Werror` without the production Makefile's existing
  `-Wno-stringop-truncation` policy. Follow-up 3 (`4ea7a9885`) matches that
  policy for the copied production translation unit; new workload code retains
  `-Werror`. No completed matrix or measured failure was rerun or replaced.

## Full code-gate findings and corrected recheck

Initial full gate at `d1bf8e2bd`: **545 commands, 7 FAIL, 4 NOTRUN,
977 seconds, 9 overbudget; build 17 seconds. Not PASS.** No `src/`, `include/`
or Makefile content differs between the tested `d1bf8e2bd` and base
`486aa3a46` (`git diff 486aa3a46 d1bf8e2bd -- src include Makefile` is empty).
This rules out a Task AI product-code delta; it does not waive the failures
or establish that every base run reproduces them. Independent serial base
checks ran afterwards, without another EFS/test workload. Controller
`7028c56e4` compared 213 product/Make/test/helper inputs byte-for-byte before
building untouched `486aa3a46`; combined input hash
`df5f1f37205325476848377022dc902d878d51e0201b68bcd7a28b04efcf63ba`.
The slow case reproduced F0 EIO at 60.04 s (151.69 s case); the full-quota
case passed once (47.22 s). That is not an eight-job base full-gate rerun or
a fix/waiver for intermittent K53. Base failed fixture:
`/data1/01/efs-taskAI-20261028/base-tmp/efs-b6-hg8zq5fj`.

- **K55, slow-member fsync availability, OPEN:**
  `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py`, case 481,
  failed because F0 returned EIO at 60.01 s; the other six fsyncs returned
  success in approximately eight seconds each. The first fsync deliberately
  slows one storage COMMIT to 65 seconds. Retained fixture:
  `/data1/01/efs-taskAI-20261028/tmp/case-481/efs-b6-_axe6l25`.
  Its client log records `fragments=PDD`, then `sweep deadline exhausted;
  obligations retained`, not a degraded mark. The printed `0 of 3 durable`
  is logged before counting that PDD snapshot and does **not** mean that the
  two D fragments lack proof. Source `stripe_prove` starts COMMIT workers
  concurrently but joins every worker before marking; `commit_one` may use
  both transport attempts inside the same 60-second sweep deadline. After
  the slow worker consumes it, the sweep refuses before installing its
  required degraded mark. That is a source-supported explanation; a focused
  interleaving/transport isolation test is still owed. No false durability
  success or rollback is inferred. The fix must preserve the absolute bound,
  proof requirements and durable degraded mark, not ignore the slow member
  or turn EIO into success. Distinct from K54's clean-unmount revoke problem.
- **K53 recurrence, full-quota oracle, OPEN:**
  `EFS_B6_MODES=full python3 tests/test_b6_unavailable.py`, case 482,
  returned ENOSPC for W0–W2 and Q0–Q2, but Q3 succeeded. Retained fixture:
  `/data1/01/efs-taskAI-20261028/tmp/case-482/efs-b6-jicq6e20`.
  This matches the already-open full-quota admission finding's signature;
  the exact cause remains unproven. The unchanged oracle still fails.

Other initial failures and corrections:

| Case | Classification | Correction / current evidence |
| --- | --- | --- |
| 503, K22 snapshot import | Test transport environment: its cluster/FUSE explicitly used TCP, but four `efs-bench` loaders inherited parent strict RDMA and silently failed initialization; insufficient metadata load, wrapper timeout at 900 s | `18eb32bd4` passes `env=env` to that subprocess. Four required snapshot advances and all deadlines unchanged; corrected full-gate case 504 PASS, 275.8 s |
| 507, W166 authority | Same missing environment on its `efs-bench` L0-filling subprocess; raw output says segmented RDMA setup failed against its intentionally TCP fixture | `18eb32bd4` passes the fixture environment; assertions/budgets unchanged; corrected case 508 PASS, 10.3 s |
| 533, session latch TSan; 543, apply-ring TSan | Host dependency failure: GCC 11 linker script names absent `/usr/lib64/libtsan.so.0.0.0`; package not installed | Matching signed runtime privately extracted; actual positive/negative gates PASS, corrected cases 534/544, 0.7/1.2 s, not merely startup acceptance |
| 540, make recipe | Task AI's new parser test was not wired | `18eb32bd4` adds it to the fast suite; local recipe/parser checks PASS. No slow-test budget added. `7d5e065bd` labels its completed parser-only scope without a misleading missing-hardware marker |

Matching dependency `libtsan-11.5.0-14.el9.x86_64.rpm`, obtained using the
host's configured repositories and private DNF cache/state/log paths;
`rpmkeys --checksig`: digests/signatures OK. SHA-256
`4e407df94292ad96486d01fd1b0e272ecd599ed3dfbef959fe61a2eb4032f13a`.
It is extracted under `deps/tsan-root`, not installed system-wide. The
private `gcc-private-tsan` wrapper executes `/usr/bin/gcc` with a `-B` prefix
to that library directory; the private unversioned link names this matching
runtime. Runtime lookup uses that directory in this gate's `LD_LIBRARY_PATH`.
No system package, library, ASLR setting or other agent's environment changed.

The timed-out K22 test left one disconnected mount, kernel identity `0:61`,
under case 503. After checking its exact path/uid and absence of live owned
EFS processes, **normal** `fusermount3 -u` returned zero and mount absence
was verified; no lazy detach, force-discard or store modification.
Receipt `k22-timeout-unmount.json` is retained. A first queued base check
refused an incomplete source transfer before any build/test; that error is
retained. The complete export then passed the 213-input comparison and ran
once. The corrected gate requires `EXPORT_COMPLETE` before starting.

Corrected full code export **`7d5e065bd`**, controller `b31419c5e`, output
`recheck-7d5e065bd`: complete after the base checks, **546/1 FAIL/4 NOTRUN**.
The one failure is K55 again: F0 EIO at 60.01 s; failed fixture
`/data1/01/efs-taskAI-20261028/recheck-tmp/case-482/efs-b6-hdcpfewb`.
K53's full-quota command passed on this attempt, which does not waive its
earlier failure. Product `src/` and
`include/` still have zero diff from the measured/native source; only test
wiring and the two fixture environment corrections affect the gate recipe.
This is not another performance attempt. All original failures remain intact.

The four host limits are: THP negative control not materialized (small-page
proof ran); external cluster stop/remount scripts absent; RDMA Memcheck's
`valgrind/memcheck.h` absent; generic sanitizer-gate host attach refusal.
The separate TSan latch and ring checks did run successfully. Overbudget
commands remain reported (K35 degraded fsync, K43 coordinator kill, Raft ACK
window, K22 leader admission and three D39 model shards); no budget was raised.
Current Linux collector `--native` also PASS: eight joined threads, 8,000
paired acquisitions and complete counters, in addition to its parser checks.

## Raw evidence and remaining work

Host raw build/launcher logs and binary manifests:
`fcstor015:/home/erbmi1/efs-artifacts/taskAI-20261028/fcstor015/`.
Private retained diagnostic outputs and open-finding builds:
`fcstor015:/data1/01/efs-taskAI-20261028/`.
`matrix-d433ab689/state.json`, `base-check-complete-export/state.json` and
`recheck-7d5e065bd/state.json` are the phase records. The initial failing
`followup-3/state.json` remains unchanged. Raw output is outside Git; failed
evidence stays on the host.
Closed non-store measurement manifest: `closed-measurements-manifest-v2.json`
in the raw host directory, 412 files,
SHA-256 `3adb47d1e64f89e299554fbcfc646f75928512deed21513667d5304c7b723f72`.
The original manifest is retained unchanged; v2 explicitly excludes 81 runtime
Raft logs accidentally matched by the original broad `*.log` glob.

Final retained bundle: the same raw directory's **`sealed-final/`**, 520 copied
closed records/captures, component executables and dependency/provenance files.
Every copy was checked against its original SHA-256 and fsynced, with metadata
and directory entries, **before** any passed fixture was removed.

| Retained record | SHA-256 |
| --- | --- |
| `manifest.json` | `f65ef820843becf200ab35f647af919bfe26c48c1cc626b54fdb24f354257a6c` |
| `failed-fixture-inventory.json` | `dfcf428123f87a9bc919a25efc9e175f2fa4ba5a26ff11463b5d37d1ebb185ff` |
| `closure-receipts.json` | `524d6f0b35e0e577b69728a5e448ba9bc4f012f592b6020000bb5507dbf7aea8` |
| `cleanup-complete.json` | `9c09105987c7f0eeabd7ad03f06f37469d8f3f592613a8e5b0a2acf4ebfdaefc` |

Additional raw `open-build-fingerprints.json` records 15 retained product/fault
binary hashes at declared builds `d1bf8e2bd`, `486aa3a46` and `7d5e065bd`,
SHA-256 `0e5d5cd670ff9d1fd3c31b034810f0bb3e013ef48c2021b89ee6b453c9e688e5`.
The retained native efsd, FUSE, management and transport-test binaries still
match all four original pre-measurement hashes.

Closure tool `498c9b18a`, `tests/measure/rdma_registry_close.py`: inspect the
sealed `prune-plan.json`, then its `prune` subcommand uses only
`~/bin/edelete --force --delete` on explicitly passed, owned, unmounted targets.
It refuses observed foreign EFS/test work. **1,091 targets pruned**: 1,002
passed command temporary directories, 81 matrix node stores, three passed
smoke node stores, two passed GC roots, the diagnostic probe build and two
private DNF cache/state directories. No live export was mutated for a new run.

**All seven failed roots remain on fcstor015** (exact roots in the inventory
and prune plan). All 8,698 failed regular files were rehashed before and after
pruning, and their inventory/modes/owners/link metadata compared equal. The
native, untouched-base and corrected-gate builds are deliberately retained
for OPEN K55/K53 investigation and original failed-gate review. This is the
remaining bulk-retention exception; it is not a claim that the failures are
fixed. Passed store replay is forfeited after pruning; recreate those stores
from the scripts and exact source instead. Logs/results/counters are retained.
`PRUNED.txt` and the complete deletion receipts record every removed target.
Final checks found no owned EFS processes or mounts and no observed foreign
EFS/test workload; this is not an exclusive host lease. Other clusters and
system libraries were untouched.

Task AI is complete under its measured-no-change/base-failure allowance.
Kernel scheduling/NIC-arrival latency, multiple-HCA contention, multi-host
fabric performance, the full K32 IOR geometry and alpha release acceptance
remain outside these measurements. **K55 still needs its own fix and gate;
K53 remains open.** K32 controls are cross-referenced on its page.
