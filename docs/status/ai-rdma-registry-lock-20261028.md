# Task AI — RDMA registry lifetime lock, October 28, 2026

**OPEN; qualification RUNNING, no product locking change.** Task AI in the
[alpha checklist](alpha-1.0-2plus1-checklist.md) measures before choosing an
optimization. Native real-RDMA transport smoke and one-rank filesystem/IOR
smoke PASS scoped. The matched matrix and follow-up gates are not yet complete;
there is no threshold verdict or throughput improvement claim.

## Placement and source

The user authorized trying fstor007, node9900 and fcstor007–015, with
`/data1/01`. node9901 was occupied by Task H; node9900, fstor007, fcstor007
and fcstor012 also showed other EFS/test work. No existing cluster was stopped
or changed. fcstor015 showed no EFS/test workload in the preflight and has
active mlx5_0 and mlx5_2 devices. Measurements select **mlx5_0 only**.

fcstor015: RHEL 9.8, kernel `5.14.0-687.39.1.el9_8.x86_64`, 24 logical CPUs,
`/data1/01` on `/dev/nvme0n1`, approximately 7 TiB free at preflight.
The user's directory authorization supersedes the task sheet's original
node9901-only assignment. These are single-host hardware-RDMA measurements,
not multi-host fabric qualification or a two-device scaling result.

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

Follow-up runner `b2f120f06` waits for this existing matrix; it never launches
a duplicate. It runs source-bound, real-verbs 1/8/32-stream request/reply
checks with concurrent connection destruction/reuse (32 cycles, overlap
count recorded), three repeats and separate TCP/native/probe controls.
Every echo is checked and all connection/assembly ownership must return to
zero. Its extra wake diagnostic reports **poll-return to first successful
pending-ring publication**, excluding kernel wake-to-run and NIC-to-CQE delay.
No broader wake latency claim follows from that measurement.

The planned correctness gates are real-RDMA `test_rdma_xprt`, all seven
segmented modes, handoff/progress/fork tests, physical reclamation and POSIX
over an RDMA mount, then the full `make test-all` summary. They remain NOTRUN
until their retained outputs close. The full gate's NOTRUN entries must be
reported individually; exit zero alone is not proof of every live gate.

Task criterion: investigate an optimization if 32-stream waiting exceeds 5%
or a demonstrated end-to-end gain justifies it. Below threshold, measured
no-change is valid. No lifetime pinning, registry partitioning or dispatch
batching has been implemented or selected.

## Closed results and retained failure

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

## Raw evidence and remaining work

Host raw build/launcher logs and binary manifests:
`fcstor015:/home/erbmi1/efs-artifacts/taskAI-20261028/fcstor015/`.
Private source, diagnostic trees, closed smoke, active matrix and waiting
follow-up: `fcstor015:/data1/01/efs-taskAI-20261028/`.
`matrix-d433ab689/state.json` and `followup-1/state.json` are the current
stage records. Raw output is outside Git; failed evidence stays on the host.
Full final capture hashes and pruning are owed at closure; no absent hash or
capture is counted as verified.

Still owed: matrix medians/spread, 32-stream registry/overhead assessment,
connection-churn and dispatch results, correctness gate classifications,
K32 control table, final evidence manifest and checked own-fixture cleanup.
The AI checkbox remains open. No change to an existing cluster, merge or push.
