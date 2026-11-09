# Task AN — PUT STATUS authority measurement

> **Wave 38 coordinator note:** this page and the `scripts/an_*` measurement tools were merged; the branch's product commits were not (they overlap K35/K55/K48 work already on devel and are being ported separately). References below to repairs such as `871f9220d`, `b5e60f229`, `bca5ac4a7`, `c3f3f313f` describe `agent/devel-task-an`, not devel.

**AN COMPLETE — measured, no STATUS optimization**, on user-approved isolated
fcstor014 with hardware mlx5_2. Candidate f7b74f714 (product identical to
c3f3f313f) passed the full **548/0 FAIL/0 NOTRUN** gate, nine strict hardware
audits, all 25 required named repeats, ten additional directory confirmations,
the precise 36-cell matrix and 36 observer controls. Exact payloads, source
identity, ownership, cleanup and closed evidence hashes were reviewed.

No measured STATUS-specific benefit justifies proof reuse or batching; existing
authority checks remain. Observer CPU/GETATTR effects and fixed-rate/single-host
limits are explicit in the final disposition below. Historical failures and
fixtures remain preserved, with intermittent attribution unproven. No merge or
push; F1/whole-release coverage is not claimed complete.

## Scope and required acceptance

Task AN in the October 26 parallel-task file, added October 28. Source review
and canonical graph traversal confirm: each storage PUT checks the exact
ticket; its lane leader establishes session-owner authority and then takes
the participant proof. Admissions and actual quorum rounds differ because
readers can share a round. K32 already batches ADMIT/SETTLE; AN concerns STATUS.

Base is committed devel `9c4bfd171` (later than the task's minimum), isolated
worktree /Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-an.
Shared D39 and graph edits in /Users/mike/git/efs are untouched. Product changes repair the observed ENOENT retry in store.c, overlap required
degraded-mark work with a pending third COMMIT in data_commit.c, keep directory
migration physically enumerable and canonicalize retried GETCHUNKS records.
Client changes add failure-only protocol diagnostics. No new EFS setting,
wire command, retry count or deadline change.

Required matrix: 1/8/32 writers × TCP/RDMA × healthy/one member down,
three fresh runs each; report medians and spread. At the same per-writer
offered rate measure write throughput/p95/p99, a real GETATTR RPC at 10 Hz,
per-daemon CPU, and EFS_RAFT_OBS pump/lock high-water fields. Separate successful
storage-to-leader STATUS frame sends, owner RPC frame sends, accepted proof
admissions and actual new core quorum-round starts; never infer round count
from the number of requests. Below the measured threshold, stop without
optimization. Above it, preserve exact ticket/session checks with explicit
group/term scope, bounded queue/residence, no idle linger and no authority cache.

Before accepting a product change: deterministic revoke, SETTLE, leader-change,
halted/lagging follower and mixed-ticket/lost-reply interleavings; ticket upload,
batch and session-admission regressions; K35, K33 payload and W166 five of five;
full 8-job test-all gate and actual hardware RDMA. Failures require untouched
base comparison on the same host/workload. No retries or relaxed oracles turn
a failed row into a pass. The final current-candidate acceptance is recorded below; older scoped checks
do not substitute for it.

## Approved host and measurement method

User explicitly approved substituting isolated fcstor014 for xorinox/node9901.
Read-only inventory found xorinox's UID 1001 real client PID 234552 and
node9901's four real daemons/client; all remain untouched. fcstor014 was free,
with hardware mlx5_2 InfiniBand ACTIVE/4096 MTU, GCC 11.5, private FUSE 3.16.2
under /home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse. Raw captures use
the writable local NVMe /data1/01/efs/evidence; scratch clusters are under /tmp.
Two all-account quiet checks twenty seconds apart precede each controller;
ten-second ownership sampling continues. No existing-client exemption.

The committed tools are `scripts/evidence/an/an_put_probe.py`, `scripts/evidence/an/an_put_measure.py`,
`scripts/evidence/an/an_stat_client.c`, `scripts/evidence/an/an_host_run.py` and `scripts/evidence/an/an_base_control.py`. Export a committed
tree first; the probe refuses a Git checkout and records before/after hashes.
It adds temporary cumulative relaxed-atomic counters to two exported C files,
with a five-second dump through existing EFS_RAFT_OBS. Product decisions are
unchanged, but observer overhead/scheduling still need calibration. These are
measurement binaries, not an uninstrumented performance acceptance result.
Total admission/round counters include stat and background reads; normalization
by PUTs is aggregate workload cost, not exclusive causal attribution to STATUS.

Each row forms three private O_DIRECT daemons, 8 GiB quotas, group 0 leader on
node 1 and group 2 on node 2. Node 3 is the stopped follower in degraded rows.
One FUSE writer mount, four files/writer (8/192/384/512 KiB), two offered
operations/second/writer: overwrite, truncate/write, in-place rewrite. Packed
and ordinary data, 1 s pack age / 200 ms passes, 2 s indexing and 256 KiB
fault-daemon memtables stay enabled. Body bytes depend on writer, file and
version. No failed write is skipped; all acknowledged final versions and EOF
must match through a fresh independent FUSE process. The unchanged 30 s
write/close bound remains. Late offered work and missed stat slots are recorded.
No probe/state RPC is substituted for the real authority path.

An exported pilot is built and launched in detached screen with:

```sh
python3 scripts/evidence/an/an_host_run.py --base /tmp/efs-taskAN-FRESH \
  --out /data1/01/efs/evidence/an-put-status-FRESH --commit COMMIT
# --matrix adds 36 fresh 60-second cells after a passing 15-second pilot.
```

The base must contain src/ from git archive COMMIT. The script builds normal
and fault binaries in a separate measure/ copy, with a +ANprobe build identity,
and compiles the read-only stat helper against that exact library. Do not
stage/build/hash during active measurements or reuse fixture/evidence names.

## Preserved findings and current verdict

| Attempt | Product / harness | Result |
|---|---|---|
| b219 pilot | 9c4bfd171 + temporary probe / b219611d3 | Setup FAIL: harness assumed READY was the first stdout line; real membership startup prints first. Fixed with a bounded marker and retained startup diagnostics; no measurement accepted. |
| 9258 pilot | 9c4bfd171 + temporary probe / 9258fd89b | Healthy one-writer TCP FAIL: seq 12 EIO; client logs NO_QUORUM and failed clean unmount. |
| untouched base control | uninstrumented 9c4bfd171 / 061b7e1fa controller, 4fb5502b6 row harness | Healthy one-writer TCP FAIL: w0/f0, seq 20, in-place rewrite, version 6, 4096 bytes, EIO after 0.8204 s; writer FUSE exits 1. |

Base efsd identity: 9c4bfd171, branch devel-untouched-AN-control, built
2026-10-29T18:42:02Z on fcstor014. Product C before-hashes match the probe's
untouched source manifest. The counterless base control is explicitly labeled
DATA/TIMING CONTROL with unavailable authority counters; it is not a primary
AN counter result. Neither failed pilot supplies a valid latency/RPC table.

Raw roots on fcstor014 under /data1/01/efs/evidence/:
an-put-status-20261029-b219611d3-pilot,
an-put-status-20261029-9258fd89b-pilot,
an-put-status-20261029-base9c-control-061.
Failed instrumented fixture:
/tmp/efs-taskAN-9258fd89b/fixtures/pilot/cluster;
failed base fixture:
/tmp/efs-taskAN-9258fd89b/fixtures/base-control-4fb/cluster.
All three stores, client/daemon logs and both source/build trees are kept for
the open investigation. The earlier b219 setup fixture also stays in its tree.
No bulk is deleted and no raw payload/log is committed. The initial readiness
assertion did not save its first stdout line; the 9258 driver also did not
persist partial operation/acknowledgment lists on exception. Both telemetry
gaps are explicit; 4fb5502b6 preserves those lists and checks nonzero FUSE exits.
No storage-loss or packing attribution is established by the EIO alone.

## AN-F1 causal regression and repair

Additional untouched-base diagnostics are retained on fcstor014 at
/data1/01/efs/evidence/an-put-status-20261029-base9c-diagnostic-401 and
/data1/01/efs/evidence/an-put-status-20261029-base9c-syscalls-b36, with matching
fixture names below /tmp/efs-taskAN-9258fd89b/fixtures/. Existing ticket/RPC
logs report storage IO refusal on all three members. The failed-only syscall
trace shows each member's O_DIRECT exclusive fragment creation returning
ENOENT twice for the same pathname. The directory helper's thread-local
last-parent hint bypasses mkdir on that retry. Attribution to a specific
parent-removal caller remains unproven.

Regression-only commit `545c538f8` adds a deterministic production shard test:
warm the parent hint, unlink the owned fragment, remove its empty parent,
then repeat the PUT. Unchanged base store.c FAILs the new success/barrier
assertion. Raw build/test/exit receipts: fcstor014
/data1/01/efs/evidence/an-parent-base-545; failed fixture
/tmp/efs-store-durability-b3lA0H stays untouched. Reproduce with
`make -j8 tests/test_store_durability && ./tests/test_store_durability` on Linux
with O_DIRECT-capable TMPDIR. Failure exit is 134; the shell controller's
zero exit does not override that recorded test failure.

Repair `871f9220d` forces a real mkdir walk only after observed ENOENT on
creation. It keeps the original single retry, no-create refusal, O_DIRECT,
record/tail checks and all file/name barriers. The regression also reads back
payload and record and requires unchanged barrier counts. Production
`test_store_durability`, `test_store_overwrite`, and `test_store_gc` all PASS
(exit 0), logs and individual receipts at
/data1/01/efs/evidence/an-parent-fixed-871. No skipped XFS coverage is counted
as PASS; inspect the complete durability log for its filesystem-specific row.

Fresh corrected baseline is running from /tmp/efs-taskAN-fixed-871/src via
`an_host_run.py --base /tmp/efs-taskAN-fixed-871 --out
/data1/01/efs/evidence/an-put-status-fixed-871-matrix --commit 871f9220d
--matrix`. Private screen directory is /tmp/efs-taskAN-fixed-871/screen,
session taskAN-fixed-871. It requires two quiet checks, then builds a separate
probe tree, runs the pilot and stops on any failed matrix cell. No performance
verdict yet. The original failing base cannot justify a batching speedup;
this correctness-fixed baseline must be measured first.

Gate, named repeats, full matrix, observer calibration, before/after benefit
and closed-capture manifests remain pending. All earlier failures stay FAIL;
no raw artifacts or generated payloads are committed.


## Member-down teardown control and fresh protocol

The first corrected-product matrix at
/data1/01/efs/evidence/an-put-status-fixed-871-matrix stopped after its
32-writer RDMA member-down cell: all 3,840 offered operations succeeded,
all 128 files verified through the fresh mount, fragment errors were zero,
but both FUSE exits were 1. The precise failure was clean revocation
`rc=-13 phase=storage-fence`; flush and SETTLE were 0. This remains FAIL.
The preceding eight-writer healthy RDMA cell passed (960 operations, 32 files).
This partial matrix supplies no three-repeat acceptance or speedup claim.

This is the registered open K54 failure, not an AN authority optimization.
Control `f8d716dc1`, using the untouched 9c4bfd171 binaries, reproduced the
same storage-fence failure on a fresh read-only RDMA mount after hard-stopping
member 3. Evidence /data1/01/efs/evidence/an-base-down-f8d and fixture
/tmp/efs-taskAN-base-down-f8d remain. This narrow control proves the shutdown
signature exists on base; it is not a completed 32-writer baseline comparison.
Exact command: `python3 scripts/evidence/an/an_down_shutdown_control.py --source
/tmp/efs-taskAN-9258fd89b/src --work /tmp/efs-taskAN-base-down-f8d --out
/data1/01/efs/evidence/an-base-down-f8d`. Its diagnostic shell exit 0 means
reproduced as expected; result verdict FAIL and FUSE exit 1 remain authoritative.

Fresh protocol `8661b6a5f` keeps the hard stop, fixed-rate workload, 60-second
sample window and independent byte/EOF verification while member 3 is down.
Only after those finish does it restart its own private member, require
membership/voter readiness using existing bounds, and demand both original
clean-unmount exit checks. No K54 repair is claimed, and no failure is ignored.
This separates the AN measurement from K54's explicitly failing shutdown
scenario; it does not replace alpha's required K54 gate.

Fresh source /tmp/efs-taskAN-matrix-866/src, separate temporary probe tree,
private screen directory /tmp/efs-taskAN-matrix-866/screen, session
taskAN-matrix-866. Controller command: `python3 scripts/evidence/an/an_host_run.py --base
/tmp/efs-taskAN-matrix-866 --out /data1/01/efs/evidence/an-put-status-matrix-866
--commit 8661b6a5f --matrix`. Pilot PASS; matrix running. No active-host staging,
heavy review or hashing. The closed-capture summary tool `an_put_summarize.py`
requires 36 passing cells, three repeats per configuration and no ownership
interference; it reports separate normalized counters and medians/min/max.
Its pump/lock maxima use the same per-node counter-dump bracket, including
the documented stat/background traffic. Observer calibration still owed.

All following manifests were created after their controllers closed and
before the fresh pipeline began. Paths are relative to fcstor014
/data1/01/efs/evidence; no store or payload was deleted or exported.

| Closed capture | SHA256SUMS SHA-256 |
|---|---|
| an-put-status-20261029-b219611d3-pilot | 68cea8428ca236b153e278395ad86a1107dae7e690f0436e585d1f9fa7a277b7 |
| an-put-status-20261029-9258fd89b-pilot | 8a5343e2117988512692b95cdf031ab2fabcfa60103cd3fcbcb4583f275a0b49 |
| an-put-status-20261029-base9c-control-061 | 4f821c66302ec6b5bb80a43eeb551570d4739462bc10e1df6237b3304ea970bc |
| an-put-status-20261029-base9c-diagnostic-401 | 1dd1c100b08316aa77a04f6eeba77337439a79977e653e5a08c81d494237a347 |
| an-put-status-20261029-base9c-syscalls-b36 | 14f009dc8bdf1e20193ba9edf4a364d860ce8577b0c13fccda0dcd879f177124 |
| an-parent-base-545 | 0f1f50d73d55c440f0810f357d1cc0a5bdfcaa61a7c6040640031ef0646aad2d |
| an-parent-fixed-871 | 8cb8c288b052ed0c1dd16eee66166521d533e3dfe0d2c79d8a0bd4b81a5e847d |
| an-put-status-fixed-871-matrix | 358f062efa8cfaab764048363806a04d7adf0b51f2a503a847d8655aab69191d |
| an-base-down-f8d | 1cddfdd63fc37a9b4380c8b8d40325fe1151a4d77f94988b02003df0d18fa80a |


### Timing coverage clarification (not a new performance verdict)

Source review shows the existing `raft-obs pump_hold_max` is the sum of
selected pump phases, not an elapsed active-cycle or exact full mutex-hold
measurement. Its lock-wait and phase-sum high-water values remain useful,
but are labeled by their actual semantics. The running 866 matrix does not
provide the missing exact active-cycle/hold fields; no such coverage is
claimed from it.

Temporary probe `ec2d3aeb0` adds measurements from before mutex acquisition
to immediately before poll (active cycle, including lock wait, excluding
idle sleep), and from acquisition to immediately before unlock (full hold,
including diagnostic formatting). Only the pump thread records/dumps these
interval maxima, using the existing EFS_RAFT_OBS toggle. Counter generation
checks PASS locally; Linux compilation, runtime measurement and observer
calibration remain NOTRUN. This code has not been staged onto the active
host and changes no product source or authority decisions. Fresh precise
measurements are owed before treating that part of the shared kit as covered.

Native qualification driver `scripts/evidence/an/an_qualify.py` is prepared but NOTRUN.
It refuses an active/failed prior matrix, requires two quiet checks, uses
uninstrumented src/, runs the eight-job full gate (including the fast suite), then five
fresh K35 and K33 payload runs on each TCP/RDMA transport and five W166
runs (that existing test explicitly selects TCP). Named SKIP/NOTRUN outputs
are refused; gate exit 3 remains INCOMPLETE pending individual host-limit
review. Existing test retention flags and private TMPDIR retain investigation
fixtures. No deadline, fault, durability or test assertion changes.


## First completed counter/latency baseline — 8661b6a5f

**36/36 cells PASS for the recorded counters, payloads and latency/CPU window;
measurement kit INCOMPLETE for precise active-cycle/full-hold timing.**
Controller exit 0. All 59,040 offered operations completed, 1,968 final files
verified through independent mounts, 21,600 GETATTR RPCs completed with no
missed slots; no operation errors, live-member fragment errors, failed clean
exits or foreign/background ownership samples. Degraded cells keep member 3
hard-down through timing and independent verification, then restore it before
strict revocation. Original K54 and AN-F1 failures remain separate FAILs.

Numbers below are median [minimum–maximum] across three fresh 60-second runs.
RPCs count successful frames; all proofs/rounds include fixed stat and background
reads in the bracketing snapshots, not exclusive causal STATUS cost. Sharing
and amortization both affect normalization. A STATUS request is not a quorum
round. This is not an optimization comparison and proves no batching benefit.

| Writers / transport / state | STATUS RPCs / fragment | Owner RPCs / fragment | Proof admissions / fragment | Quorum rounds / fragment |
|---|---|---|---|---|
| 1 tcp healthy | 0.667 | 0.246 [0.233–0.330] | 5.249 [5.152–5.387] | 4.556 [4.542–4.652] |
| 1 tcp down | 0.500 | 0.452 [0.438–0.452] | 6.818 [6.632–6.828] | 6.425 [6.225–6.453] |
| 1 rdma healthy | 0.667 | 0.301 [0.232–0.318] | 5.240 [5.124–5.384] | 4.563 [4.452–4.632] |
| 1 rdma down | 0.500 | 0.432 [0.335–0.445] | 6.908 [6.730–6.922] | 6.473 [6.390–6.500] |
| 8 tcp healthy | 0.667 | 0.213 [0.204–0.220] | 4.160 [4.127–4.176] | 2.951 [2.890–2.959] |
| 8 tcp down | 0.500 | 0.270 [0.268–0.283] | 5.445 [5.400–5.511] | 4.186 [4.169–4.267] |
| 8 rdma healthy | 0.667 | 0.193 [0.192–0.195] | 4.112 [4.107–4.117] | 2.828 [2.827–2.842] |
| 8 rdma down | 0.500 | 0.260 [0.250–0.274] | 5.396 [5.351–5.509] | 4.127 [4.087–4.255] |
| 32 tcp healthy | 0.667 | 0.160 [0.156–0.161] | 3.979 [3.967–4.005] | 2.267 [2.264–2.288] |
| 32 tcp down | 0.500 | 0.195 [0.190–0.200] | 5.136 [5.107–5.149] | 3.481 [3.432–3.483] |
| 32 rdma healthy | 0.667 | 0.155 [0.150–0.172] | 3.997 [3.902–4.018] | 2.242 [2.175–2.265] |
| 32 rdma down | 0.500 | 0.186 [0.186–0.188] | 5.119 [5.006–5.135] | 3.382 [3.303–3.430] |

Closed root: fcstor014 /data1/01/efs/evidence/an-put-status-matrix-866.
All 351 SHA256SUMS entries verified (exit 0); manifest SHA-256
`a7468cfdbb15102c17a0a8587dd2c5f4ae3e9c2d96f7ee6ef2fb0b900e96e3b0`.
The committed summary method emits all per-node CPU and phase-sum/lock-wait
high-water medians/ranges in summary.json. `counter_latency_matrix_complete`
is true, while `complete` is false with 90 node/run precise-timing gaps.
The summary exit 1 is this explicit coverage gap; the recorded workload
controller exit remains 0. Captures, fixtures and both binary trees remain.

The first uninstrumented gate ran from the same exported src/;
src/, include/ and Makefile are identical between 8661b6a5f and current branch.
Driver 22bc49bb8, private screen taskAN-native-gate, raw
/data1/01/efs/evidence/an-native-gate-866-22bc49. Exact command:
`python3 /tmp/efs-taskAN-qualify-22bc49.py --source
/tmp/efs-taskAN-matrix-866/src --prior
/data1/01/efs/evidence/an-put-status-matrix-866 --out
/data1/01/efs/evidence/an-native-gate-866-22bc49 --commit 8661b6a5f --gate-only`.
Named repeats remain NOTRUN in this phase. The gate closed FAIL; see below.


### Application/RPC latency and daemon CPU (first baseline only)

Same three-repeat median [min–max]. Write times are milliseconds; GETATTR
latencies are microseconds; throughput is MiB/s (rounded to 0.001 here).
All cells delivered every offered operation, so throughput reflects the
fixed offered rate rather than maximum sustainable capacity. Full precision
and the legacy phase-sum/wait high-water ranges remain in summary.json.

| Writers / transport / state | Write p95 ms | Write p99 ms | Stat p50 µs | Stat p99 µs | MiB/s |
|---|---|---|---|---|---|
| 1 tcp healthy | 172.38 [161.63–206.93] | 207.78 [186.90–216.62] | 70 [69–70] | 108 [108–110] | 0.531 |
| 1 tcp down | 7.22 [6.96–7.67] | 9.29 [8.55–9.57] | 71 [69–74] | 118 [109–119] | 0.531 |
| 1 rdma healthy | 188.77 [161.06–200.16] | 209.89 [207.08–212.21] | 96 [94–100] | 137 [137–151] | 0.531 |
| 1 rdma down | 7.00 [6.62–7.08] | 8.71 [8.55–9.66] | 94 [90–97] | 135 [130–145] | 0.531 |
| 8 tcp healthy | 101.24 [96.48–114.13] | 165.72 [140.18–168.49] | 75 [75–77] | 131 [121–132] | 4.249 [4.247–4.250] |
| 8 tcp down | 31.44 [31.00–32.72] | 40.68 [39.97–41.00] | 78 [76–82] | 132 [121–142] | 4.247 [4.246–4.249] |
| 8 rdma healthy | 120.19 [96.87–130.91] | 169.95 [151.35–177.04] | 102 [99–103] | 163 [158–164] | 4.249 [4.248–4.249] |
| 8 rdma down | 31.70 [29.98–33.85] | 40.16 [35.47–42.40] | 93 [93–99] | 139 [138–146] | 4.249 [4.247–4.249] |
| 32 tcp healthy | 192.51 [184.42–193.14] | 264.72 [216.37–271.82] | 84 [81–86] | 7062 [3988–7569] | 16.991 [16.987–16.996] |
| 32 tcp down | 108.21 [106.16–117.40] | 128.40 [127.46–141.94] | 83 [81–85] | 3824 [2974–5880] | 16.992 [16.992–16.994] |
| 32 rdma healthy | 190.18 [187.53–206.78] | 242.37 [223.40–326.71] | 111 [110–118] | 8002 [5459–8228] | 16.987 [16.985–16.994] |
| 32 rdma down | 108.03 [105.14–110.76] | 129.68 [120.58–131.91] | 101 [101–103] | 5300 [3080–5980] | 16.995 [16.995–16.997] |

CPU seconds per daemon, measured over the application window. Node 1 leads
group 0 and node 2 group 2; node 3 is the deliberately stopped follower in
down cells. Values are not fractions of a whole host.

| Writers / transport / state | Node 1 CPU s | Node 2 CPU s | Node 3 CPU s |
|---|---|---|---|
| 1 tcp healthy | 1.27 [1.11–1.32] | 1.10 [1.06–1.25] | 0.67 [0.66–0.67] |
| 1 tcp down | 0.86 [0.85–0.88] | 0.84 [0.80–0.86] | stopped |
| 1 rdma healthy | 1.57 [1.48–1.73] | 1.64 [1.48–1.71] | 0.94 [0.94–0.95] |
| 1 rdma down | 1.12 [1.06–1.13] | 1.10 [1.02–1.13] | stopped |
| 8 tcp healthy | 4.90 [4.65–4.99] | 4.56 [4.48–4.79] | 2.95 [2.95–3.01] |
| 8 tcp down | 4.18 [4.11–4.25] | 4.12 [3.93–4.16] | stopped |
| 8 rdma healthy | 6.17 [6.05–6.20] | 6.28 [6.21–6.32] | 3.94 [3.94–4.00] |
| 8 rdma down | 5.12 [5.06–5.30] | 5.05 [5.00–5.05] | stopped |
| 32 tcp healthy | 16.13 [16.06–16.21] | 16.42 [16.38–16.45] | 10.74 [10.69–10.92] |
| 32 tcp down | 15.55 [15.50–16.12] | 15.54 [15.45–16.18] | stopped |
| 32 rdma healthy | 20.28 [19.08–21.08] | 20.03 [18.93–20.50] | 13.17 [13.10–13.22] |
| 32 rdma down | 18.62 [18.27–18.64] | 18.53 [18.17–18.68] | stopped |

## K35 slow-fsync blocker and matched controls — October 29

Both full gates closed **546 commands / 3 FAIL / 13 NOTRUN**: native 866
(666 s) at fcstor014 `/data1/01/efs/evidence/an-native-gate-866-22bc49`, and
untouched 9c (671 s) at `/data1/01/efs/evidence/an-base-gate-9c-aab9`.
B6 slow F0 failed EIO at 60.09/60.06 s; session-latch TSan and apply-ring
failed compilation because the system GCC linker script names an absent
`/usr/lib64/libtsan.so.0.0.0`. These are FAIL, not attach-refusal NOTRUN.
The early driver forced TCP: nine RDMA NOTRUNs reflect that selector, not
missing hardware. Fresh qualification defaults to auto on mlx5_2.

Actual B6 stores/logs remain respectively at
`/tmp/efs-taskAN-matrix-866/qualification-tmp/case-482/efs-b6-llc9opln/slow`
and `/tmp/efs-taskAN-9258fd89b/qualification-tmp/case-482/efs-b6-l9vfpmjy/slow`.
The old sanitizer harness removed its generated temporary C/binaries on
compiler failure; linker logs and committed generators remain. Harness
`a13c442da` retains subsequent generated fixtures, command receipts and output.
No lost generated fixture is claimed preserved.

Correction to an earlier conversational update: the untouched-base build
receipt still shows original `9c4bfd171`, branch `devel-untouched-AN-control`,
built 2026-10-29T18:42:02Z. Its build tree was reused, but no before/after
binary hash was captured; byte identity is unproven. Sources, failed stores,
logs and receipts remain. Subsequent comparisons use fresh trees.

The live failure logged PDD then exhausted the shared 60 s budget: joining
all COMMIT workers before metadata left no budget for the required degraded
mark while the faulted third COMMIT took 65 s. Repair `3ddfced51`/`e965262c3`
keeps every worker's original absolute deadline and all joins, but permits
metadata overlap at the budget midpoint only when every stripe already has
two completed verifier-matching, target-covered proofs. A failed mark still
fails with two proofs. Three actual matching proofs arriving before the same
deadline can retire without a mark. No deadline or durability oracle changed.

Initial regression `6a6fa2853` failed on base; initial repaired fixture
`/tmp/efs-taskAN-commit-fixed-e965` also failed because the virtual-clock
control could advance without waking the real condition wait. That evidence
is retained at `/data1/01/efs/evidence/an-commit-fixed-e965`. Synchronization
repair `9b46b07a1` signals clock readiness before completed worker signals.
In fresh matched trees `/tmp/efs-taskAN-commit-sync-9b46/{base,fixed}/src`,
only base data_commit.c is replaced with the exact 9c blob. Base slow-commit
exit 134; fixed exit 0. Fixed controls cover two proofs plus durable mark,
failed mark, fewer than two proofs, and late valid third proof after a failed
mark. Full data_commit and 65,536-chunk data_commit_index tests also exit 0.
Raw receipts: `/data1/01/efs/evidence/an-commit-sync-9b46`. This is focused
correctness evidence, not a live/full-gate or performance verdict.

Private matching libtsan 11.5.0-14.el9 and valgrind-devel 3.26.0-5.el9 RPMs
were downloaded from the configured MIT RHEL mirror, signature/digest checked
and extracted under `/tmp/efs-taskAN-deps/tools`; no system install. Script
`scripts/evidence/an/an_private_tools.sh` records package/runtime/wrapper hashes. The GCC wrapper
adds the private library path only for thread-sanitizer builds; normal EFS
builds still execute system GCC unchanged. Minimal TSan loader smoke exit 0
is not sanitizer qualification. No suppression or race-report waiver is used.

Fresh committed source `f62df2fc1` is validating via:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-slow-f62/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-slow-f62 --commit f62df2fc1 \
  --private-tools /tmp/efs-taskAN-deps/tools --sanitizer-preflight --slow-only
```

This requires actual sanitizer/Valgrind positive-negative controls and the
unchanged B6 slow oracle on TCP and hardware RDMA. Source transfer completed
before launch; private screen taskAN-slow-f62, two quiet checks 20 s apart,
continuous ownership sampling, all failures/fixtures retained. Controller exit 0; every command receipt 0. Session-latch and apply-ring race negative controls, RDMA Memcheck undefined-send control, and the four COMMIT controls under TSan PASS. TCP slow B6 PASS (197.2 s), RDMA slow B6 PASS (217.5 s): each retained 22 acknowledged files exact after recovery and orderly whole-cluster restart, with three degraded marks before fsync success. Fixtures `/tmp/efs-taskAN-slow-f62/qualification-tmp/efs-b6-oab8tlto` and `efs-b6-9przqu4n` remain. All 45 ownership samples show no foreign/background jobs. Closed 23-file SHA256SUMS manifest: `1dc909c42ebf529defb6d1712518ddef31cc7a1e55fef87510ea37b9c10b340c`. These focused checks do not replace the full gate or named five-of-five checks.
The STATUS proof prototype `a80065c60` was reverted by `66daccd4d` before any
Linux compile or runtime test; it is absent from this source. Precise timing,
observer calibration, fresh repaired baseline, optimization justification,
named five-of-five repeats and full native gate remain INCOMPLETE.


### Fresh full qualification — d4aa07160

Source `/tmp/efs-taskAN-full-d4aa/src`, raw
`/data1/01/efs/evidence/an-full-d4aa`, private screen taskAN-full-d4aa.
Source and dependency transfers completed before launch. Exact command:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-full-d4aa/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-full-d4aa --commit d4aa07160 \
  --private-tools /tmp/efs-taskAN-deps/tools \
  --cluster-scripts /tmp/efs-taskAN-full-d4aa/cluster-scripts
```

Product C is identical to the passing slow-fsync source f62df2fc1. The
fresh driver uses eight build/test jobs, automatic mlx5_2 selection,
individual hardware transport receipts, then five K35/K33 runs per TCP/RDMA
and five W166 TCP runs. The real cluster-script regression receives only
six committed files exported read-only from cluster commit
`301e21671afec9025e8a1f08735e49bf6cac2a0c`, under the private tree. That test
copies them again and uses fake transports; no real cluster is contacted.

The old sanitizer self-test's successful classification-control print named
an unavailable status, triggering the aggregate parser even when describing
a passing control. Its success wording now avoids status tokens; actual
unavailable cases still report themselves. Actual compiler/runtime failures
in that self-test now fail, rather than being reclassified as unavailable.
Generated sanitizer/Memcheck fixtures and positive/negative command receipts
are retained. No sanitizer suppression, durability change, deadline change or
transport-counter relaxation. Python syntax checks PASS; fresh runtime/full
results pending. The measurement oracle also now asserts every offered
operation and every 10 Hz stat slot, and allows existing EFS_RAFT_OBS=0 only
in counterless observer calibration. The historical 866 results already
reported exact counts, but remain a separate pre-COMMIT-repair baseline.

Observer calibration method is committed in `scripts/evidence/an/an_observer_calibrate.py`.
After a fresh precise matrix closes, it requires two quiet checks and runs
three fresh 60 s repeats of native OBS=0, native OBS=1 and probe OBS=1 at
1/32 writers on both transports. It keeps payload, missed-slot, cleanup and
ownership oracles, exports every row plus median/min/max RPC/write latency,
throughput and per-node CPU. It does not calibrate member-down cells and
must not be cited as degraded-state overhead proof. It has only passed
Python syntax validation; no calibration result is claimed. Run it serially:

```sh
python3 scripts/evidence/an/an_host_run.py --base /tmp/efs-taskAN-NEW \
  --out /data1/01/efs/evidence/an-precise-NEW --commit COMMIT --matrix
python3 scripts/evidence/an/an_observer_calibrate.py --base /tmp/efs-taskAN-NEW \
  --prior /data1/01/efs/evidence/an-precise-NEW \
  --out /data1/01/efs/evidence/an-calibration-NEW --commit COMMIT
```

NEW must be a fresh committed export with src/; no old failed tree is reused.
The calibration refuses incomplete prior runs and independently checks source
hashes against the probe manifest. Current graph validation passes nine tests,
with the known protocol.h parser warning, symbol-less files and unresolved
calls retained as coverage limits, not runtime evidence.


### d4aa full-gate verdict and hardware audits

The fresh gate closed **546 / 0 FAIL / 1 NOTRUN / INCOMPLETE**, exit 3,
726 s plus 17 s build. Seven declared wall-time budgets were exceeded
(K35 161.9/80 s, K38 37.5/35 s, K43 53.6/25 s, raft-ack 10.9/10 s,
D39 shards 56.6/25, 32.0/15, 10.9/5 s); these advisory overruns remain in
summary.txt, without changed hard deadlines. The remaining NOTRUN is case
141: this host has global THP `never`, and the old sparse-metadata negative
control did not acquire huge-page backing. The fixed small-page payload/RSS
assertions ran. This is not a complete full-gate PASS and is not an LSan
attach-refusal exception.

All nine separate native hardware checks have exit 0 and reviewed logs:
TCP/RDMA declaration frames; mlx5_2 xprt 68 application RDMA frames with
zero big TCP frames; segmented duplex application 9/9 with no app TCP;
original 50 ms segment/credit deadline; interruption cleanup; injected TCP
application-byte rejection; both 400-round bursts; 16 same-process reconnects
with no live connection/assembly leftovers. Positive counters report zero
post-setup TCP/violations. The injected TCP-byte negative control reports its
expected violation and retires the connection. Generic Soft-RoCE wording in
a test success line does not describe this mlx5_2 hardware device.

Named TCP K35/K33/W166 repeat 1 has passed; subsequent repeats are RUNNING.
No five-of-five or cleanup/ownership closure verdict is claimed yet. Closed
full-capture hashing waits until the entire active qualification closes.

A test-only candidate `d0386774d` attempts MADV_COLLAPSE (when available) in
each private sparse-metadata control mapping. Host sysfs is untouched. The
production NOHUGEPAGE mapping must still have no huge pages and at most
512 KiB RSS; the old negative still requires actual huge-page backing and
at least 2 MiB RSS, plus exact sparse payloads. Unsupported/unmaterialized
negative backing remains NOTRUN. Existing runtime limit remains 30 s,
with generated source/binary and timeout/compile/runtime receipts retained.
Only syntax validation has run; the live check waits for current named
qualification to close, then requires fresh full-gate verification if usable.


### Closed d4aa host-audit failure; named-window scope

**Overall FAIL_HOST_COVERAGE, controller 1** remains. All 25 named exits are
0: K35 and K33 five of five on both TCP/RDMA, W166 five of five TCP. All ten
K33 runs report zero retried writes and exact final payloads. Individual
results do not override the controller failure.

One of 265 ownership samples, 2026-10-29T21:29:30.105484Z, records same-account
UID 246111 efsd PID 2752782 with empty command `[""]`. No state/start-tick
identity was captured, so neither ownership nor a dead-process transition
is proven. Do not retroactively call it owned, dead or a clean full gate.
All raw fixtures, source, logs and failure receipts remain at the d4aa paths.
Closed 89-file manifest SHA-256:
`72cd9f6496776054ac63411d39c7ecb71914c82964a8a0418e2dbcae3e2cbe46`;
sha256sum -c exit 0. No bulk removed.

The native-identity receipt's host mtime, 1793309555.2937617
(21:32:35.2937617Z), was written after gate closure and before every native
and named command in the serial committed driver. All 188 ownership samples
from that lower bound through closure have no foreign/background jobs; no
owned mounts remain. `closed-review.json` records this scoped named-window
coverage, not a retrospective full-run PASS. data_commit.c SHA-256 is
`06fa4697a8b05158accdda182070b4c57b452f1f8d0340a02234dd4a45077162`,
matching the committed source. The historical unknown sample remains FAIL.

Audit revision `ed9b0bacf` records state/start ticks for foreign and verified
owned jobs, with no adoption cache or UID exemption. It moves the existing
dead-state observation after cwd ownership checks and recognizes only
observed zombie/dead Z/X/x states for empty commands; live R/S/D/I and unknown
states still block quiet for every UID. These states are documented by the
[Linux proc_pid_stat manual](https://www.man7.org/linux/man-pages/man5/proc_pid_stat.5.html).
Fixture tests cover those exclusions, unknown metadata and cross-UID source
paths; they PASS. This is not proof of the historical event's state. Exact
start/end wall/monotonic phase receipts are now retained in stages.jsonl.

### Private THP control and new full-gate confirmation — ed9b0bacf

The strengthened private-mapping control **PASS**, controller/run 0, raw
`/data1/01/efs/evidence/an-sparse-ed9`. Exact command is
`TMPDIR=/tmp/efs-taskAN-final-ed9/sparse-tmp python3 tests/test_sparse_metadata.py`
from the fresh committed source, after two quiet checks 20 s apart.
Host global THP remains never. Fixed: collapse refused EINVAL, RSS 96 KiB,
THP 0; old negative: collapse success, RSS/THP 49,152 KiB. Both payload sets
exact. Retained source/binary/reports:
`/tmp/efs-taskAN-final-ed9/sparse-tmp/efs-sparse-pages-bvbodhon`.
No sysfs or product policy changed; unsupported kernels still report NOTRUN.
The Linux collapse ABI value 25 is present in this host's kernel UAPI headers;
the test supplies it when older libc headers omit it.

Fresh source `/tmp/efs-taskAN-final-ed9/src`, private screen taskAN-final-ed9,
raw `/data1/01/efs/evidence/an-final-ed9`; full gate and native audits RUNNING:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-final-ed9/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-final-ed9 --commit ed9b0bacf \
  --private-tools /tmp/efs-taskAN-deps/tools \
  --cluster-scripts /tmp/efs-taskAN-final-ed9/cluster-scripts --gate-only
```

src/ and include/ are identical between d4aa07160 and ed9b0bacf, so the scoped
25 named test verdicts concern the same product code. This fresh protocol
changes the THP negative-control coverage and process-audit evidence; it
cannot erase any old FAIL. Precise pump matrix, observer calibration and any
optimization comparison remain pending after correctness qualification.


### ed9 full gate FAIL: post-resync directory omission (K48 recurrence)

The fresh gate closed **546 / 1 FAIL / 0 NOTRUN / FAIL**, controller/gate 1,
748 s plus 16 s build. All former 13 unavailable cases now ran, including
real THP-negative backing, Valgrind/sanitizer controls and hardware RDMA.
This coverage closure does not make a failed gate PASS. Seven advisory
budget overruns remain in summary.txt; hard deadlines are unchanged.
All 79 independent ownership samples have no foreign/background jobs.
Closed 22-file manifest SHA-256
`d047c6f15161fe3efd8386e1d9237fcf7fe8987e68ae4da983f429daba4e6605`.

Case 506, unchanged `python3 tests/test_w130_dir_fault.py`, fails after
resync: the previously complete 96-entry target listing returns 95, missing
`f0064`. W165 serve-first passed. Node 3 damaged target inode 529, halted
group 0 at entry 411/applied 410, then installed snapshot 711/term 1.
Actual failed fixture and all stores/logs remain:
`/tmp/efs-taskAN-final-ed9/qualification-tmp/case-506/efs-w130-ws682it1`.
No application retries or relaxed listing assertion are introduced.

Forensics use only disposable copies at `/tmp/efs-taskAN-k48-ed9/copies`.
The committed `scripts/evidence/k48/k48_store_scan.c` was compiled against this
exact source's libefs.a. All three copied KVs contain f0064 at shard 714,
inode 266769. Static READDIR returns 96 at every page size 1,2,4,8,16,32,64,128.
All final directory rows are SPLITTING with all 64 lane bits. This shows no
permanent dentry loss in the final captured state; it does not prove the
live serving/cursor interleaving or payload contents at failure time.
Raw build/scan receipts: `/data1/01/efs/evidence/an-k48-ed9-forensics`.
The original 1,089-file fixture manifest is
`244701e14c1fb9d55f26f4ff00fb0dcae88db249b4bb44f1c685c8d15a05603b`;
all entries still verify after reading only copies. Originals were not opened.

The existing [K48 evidence](k48-readdir-omission-20261028.md) documents a
fixed same-signature layout/bitmap race and unresolved historical attribution.
This new missing-name capture is a recurrence requiring attribution, not proof
that the old incident or the same fix explains it. At the ed9 failure, product metadata C was unchanged by AN; only store.c
and data_commit.c differed from 9c. The new
synthetic `scripts/evidence/an/an_readdir_commit_gap.c` explores a committed
cross-group DIR migration with source resolved and destination still prepared:
transaction-aware LOOKUP versus raw enumeration. Both exact ed9 and untouched 9c archives reproduce the synthetic gap: LOOKUP
returns EFS_OK while READDIR returns 95 without f0064 after source-only
RESOLVE; resolving the destination restores 96. Both runs exit 1 (FAIL),
with build 0. Raw `/data1/01/efs/evidence/an-k48-commit-gap-ee612`; source
`ee6122c52`, binaries `/tmp/efs-taskAN-k48-ed9/commit-gap-{ed9,base9c}`.
This is a real committed-migration materialization gap on the base product,
not proof of the exact interleaving behind either retained live omission. Export-only
`scripts/evidence/ao/an_readdir_probe.py` can record exact client/server page cookies,
replica IDs and names under existing EFS_RPC_PROF. It changes no decisions,
retry or metadata; any use is diagnostic, never native timing qualification.
Two local generation-check failures remain under `/private/tmp/an-readdir-probe-check-_beetzd0`
and `_g_jry21`; corrected generation and generated client syntax pass, but
Linux build/runtime is pending. No diagnostic source has been staged over an
active qualification.

### Untouched-product comparison for the new omission — CLOSED FAIL

Fresh `/tmp/efs-taskAN-k48-base-9c/src` contains ed9 harness/retention code,
with the two AN product files restored from exact 9c Git blobs. All **213**
src/include/Makefile blobs match untouched `9c4bfd171`; the pre-build proof is
`verified-product.json` beside src/. No failed build tree is reused. Binary
identity is `9c4bfd171+ANed9`, branch agent/devel-task-an, explicitly a base
control with matching harness. Private tools and cluster-script export match
ed9; all source/dependency transfers completed before launch. Raw
`/data1/01/efs/evidence/an-k48-base-9c`, private screen taskAN-k48-base-9c:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-k48-base-9c/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-k48-base-9c --commit 9c4bfd171+ANed9 \
  --private-tools /tmp/efs-taskAN-deps/tools \
  --cluster-scripts /tmp/efs-taskAN-k48-base-9c/cluster-scripts --gate-only
```

The comparison closed **546 / 3 FAIL / 0 NOTRUN**, 745 s plus 16 s build
(eight advisory overruns). The actual failures are case 30 data_commit's
slow regression (marks 0/retained 1), case 482 B6 slow F0 EIO at 60.02 s,
and case 529 store_durability's cached-parent regression. All are retained;
case 529 fixture is `qualification-tmp/case-529/efs-store-durability-1WCFGN`
under the fresh source's parent. W130 passed this occurrence; non-recurrence
does not clear the ed9 omission or establish historical causation. Closed
ownership/source verification and manifest review are still pending. Fresh precise timing and
observer calibration wait for correctness resolution; no STATUS optimization
is active.


### Materialization repair bca5ac4a7 — deterministic PASS, live qualification RUNNING

The candidate keeps the LOCAL dentry under a transaction GUARD instead of
resolving a source DEL alongside the hashed PUT. A later existing migration
pass requires fresh destination ReadIndex and a physical hashed key before
its normal DIR apply deletes LOCAL. GUARD resolution cannot resurrect a later
deletion. First-use lane registration now commits and physically resolves as
a separate commutative OR before any copy is prepared. Failed or ambiguous
registration stops before the hashed PUT; an unused registered lane is safe.
READDIR's existing per-RPC proof, bitmap/layout recheck and scan path are
unchanged. No deadline, durability, listing assertion or application retry changed.

The developmental first candidate tested 16 cuts successfully, while untouched
builder and guard-only negative controls both failed at 95/96 as expected.
Raw `/data1/01/efs/evidence/an-k48-materialize-dev`; all generated fixtures
remain. Its proposed scan-side fallback was rejected in review because it
could consult an unregistered lane without a fresh group proof. Commit
b5e60f229 was never Linux-staged or qualified; bca5ac4a7 removes that fallback
and uses durable registration instead. This records the abandoned design,
not a product qualification result.

`tests/test_dir_migration_materialization.py` extracts the actual host builder
and uses production memory KV, transaction and READDIR implementations.
Exact bca5ac4a7 build/regression **0/0 PASS**, 256 cuts: page sizes 1..128,
new/used lanes, source/destination-first RESOLVE, ABORT, registration failure,
lost replies/replayed resolution, conflicting deletion admission and tombstone
non-resurrection. The same test with untouched 9c builder/archive exits 1:
LOOKUP succeeds but the source-first cut returns 95. Positive fixture
`/tmp/efs-dir-materialization-q17vo8o_`; negative
`/tmp/efs-dir-materialization-i7etkgjj`; both retained. Raw
`/data1/01/efs/evidence/an-k48-materialize-bca`, closed receipt manifest
`c4cad10fee56a9057dac1e3a190fcd0d3573b5f06374b05feffb4c95b97e80dc`.
The short build/regression had one independent clean ownership sample;
the subsequent negative control is deterministic and not a performance run.
This does not attribute the exact interleaving of any historical live failure.

Fresh private source `/tmp/efs-taskAN-k48-bca/src`, private screen
`/tmp/efs-taskAN-k48-bca/screen` session `taskAN-k48-bca`. Exact committed
source archive finished transferring before launch. Raw
`/data1/01/efs/evidence/an-k48-bca`; its run.sh launches:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-k48-bca/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-k48-bca/qualification --commit bca5ac4a7 \
  --private-tools /tmp/efs-taskAN-deps/tools \
  --cluster-scripts /tmp/efs-taskAN-k48-bca/cluster-scripts
```

Same isolated fcstor014, hardware mlx5_2 and private verified runtime/headers;
no observer or preload. Two quiet checks 20 s apart precede eight-job full
test-all, all nine individual native transport audits and 25 strict named
K35/K33 TCP/RDMA plus W166 TCP repeats. A failure stops the protocol and
retains all fixtures. Full live verdict, focused K48/W130 repeats, precise
pump matrix and observer calibration remain incomplete. No STATUS optimization
is active. All prior failures and originating-host evidence remain intact.


### bca full gate CLOSED FAIL; K38 comparison CLOSED FAIL

The gate closed **547 / 5 FAIL / 0 NOTRUN**, 738 s plus 16 s build,
controller/outer 1. All 78 ownership samples have no foreign/background work.
Six advisory overruns remain, including K35 166.9 s versus its 80 s advisory
budget; no hard deadline changed. The actual builder regression passed in
0.8 s, W130 in 22.8 s and K48 in 3.8 s. These passing occurrences do not erase
or causally explain the original directory failure. Separate nine native
audit and 25 named repeats did not run because the full gate failed.
Closed 26-file qualification receipt manifest:
`ea5d392eafca4df348928d88822a765e41894b3aaf46f54525414400208d4f57`.

Four FAILs were caused by this session's incomplete source archive: cases
6 env inventory, 7 K register, 12 CLI docs and 134 tiny-fragment audit needed
omitted docs/ or tools/. These are preserved FAILs, not host NOTRUNs or passing
product checks. `scripts/evidence/an/an_export_source.py` now exports the entire committed
Git tree without path filtering and records the archive identity; it never
exports untracked runtime payloads/stores. Complete bca archive SHA-256 is
`6da21f3bfe82f0670d7129f6160a0e32eab7ea4172f117ce3b76476483d7257e`.
The failed source is not patched or reused for qualification.

Case 477 is a real runtime **K38 FAIL**, unchanged default
`python3 tests/test_k38_shared_chunk_pressure.py`: 24 writers, two clients,
200 records each, 47,008 B records, 32 MiB client body budget, hardware RDMA
selected by the gate's existing auto transport. Writer 4 on mnt-a received
pwrite EIO after 22 records; 4,622 of 4,800 offered records completed. No failed
write is ignored. Original fixture, all stores, writers.json and logs remain
`/tmp/efs-taskAN-k48-bca/qualification-tmp/case-477/efs-k38-jq3f2w4t`.
The small client log has `report-sync-fail stage=report total_ms=179 rc=-7`
(PROTO) at 23:39:11.375Z; this is an observed symptom, not root-cause proof.
No new retry, timeout, durability or byte-oracle change is introduced.

Fresh untouched comparison source `/tmp/efs-taskAN-k38-base-9c/src` has all
**212 src/include files** restored to exact 9c4bfd171 Git content and verified
by SHA-256 before build (`verified-product.json` beside src/). Makefile,
tests/scripts and complete gate assets match bca5ac4a7; the Makefile is harness,
not claimed untouched. Binary label `9c4bfd171+ANbca`. Raw
`/data1/01/efs/evidence/an-k38-base-9c`, private SCREENDIR sibling/screen,
session `taskAN-k38-base-9c`; run.sh repeats the same eight-job full test-all
with an_qualify.py --gate-only after two quiet checks 20 s apart. Old
store/COMMIT/materialization regressions may fail on the base; K38 must be
reviewed separately. No result is yet available and no baseline attribution
is claimed. The complete export fixes the four missing-asset prerequisites,
while the bca FAIL and every earlier investigation remain intact.


### Untouched K38 comparison closed; failure-only diagnostic RUNNING

The complete-source comparison closed **547 / 4 FAIL / 0 NOTRUN**, 619 s
plus 16 s build, outer/controller 1. The four failures are the known base
regressions: case 30 slow COMMIT control, 44 committed directory migration,
483 B6 slow F0 fsync EIO at the unchanged 60 s bound, and 530 cached-parent
store durability. Case 477 K38 passed this occurrence (23.4 s); W130 19.4 s
and K48 8.5 s also passed. The base did not reproduce K38, so it is not an
excuse for the candidate's retained EIO. Four missing-asset failures did not
recur with the complete export. All 66 ownership samples have no foreign or
background jobs. Closed 25-file receipt manifest SHA-256:
`e48b704c60eee8cd3240822775121557b27c157611a0a0e26118291ea805e6ed`.
All base failure fixtures remain under `/tmp/efs-taskAN-k38-base-9c/qualification-tmp`.

Source ef514fbd8 adds only failure diagnostics to the candidate's client:
PROTO phase labels at REPORT send, STALE pull/replay, GETCHUNKS framing and
publication reply frame/locator/verdict checks. Every error remains an error;
no retry, success substitution, deadline, durability, allocation or counter
semantics change. Mac inode_rpc syntax and controller pycompile passed.
Whole write.c Mac syntax encounters its existing Linux-only
pthread_condattr_setclock call; Linux build/runtime is the required verdict.

Fresh complete export SHA-256
`58f28356d2a9609c0fc9cabc7d4b3cfdcd5f5a7e70ae4914df963c7c05ab4fd1`;
source `/tmp/efs-taskAN-k38-proto-ef514/src`, private sibling/screen session
`taskAN-k38-proto`, raw `/data1/01/efs/evidence/an-k38-proto-ef514`.
The committed an_qualify.py diagnostic branch predeclares **at most five fresh
unchanged default K38 fixtures and stops at the first failure**, preserving
all stdout, logs and stores. run.sh executes:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-k38-proto-ef514/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-k38-proto-ef514/diagnostic --commit ef514fbd8 \
  --private-tools /tmp/efs-taskAN-deps/tools --k38-diagnostic-repeats 5
```

The host was free before transfer; transfer finished before launch and the
controller checks quiet twice 20 s apart. Same isolated fcstor014, mlx5_2,
auto transport, private FUSE/tools, 24 writers/two clients, 200 x 47,008 B
records per writer and 32 MiB client body budget. No allocation observer,
preload or extra trace option. This is diagnostic, never full qualification;
passing samples cannot erase the original failure. Source-failure attribution,
full clean gate and the AN timing/calibration work remain incomplete.


### GETCHUNKS abandoned-record retry defect — c3f3f313f, qualification RUNNING

All five ef514 standalone diagnostics closed 0/0, with 4,800 records exact on
both mounts and after remount each time, 17–24 pressure events and 1–9 STALE
replay rounds. Walls 27.0/32.8/36.8/22.4/49.0 s are diagnostic observations,
not a performance comparison or a K38 fix verdict. All five fixture paths
are printed in their retained logs. Twenty ownership samples are clear.
Closed 19-file manifest:
`b879e2925482ae5826313f4da06cd1009fdd2b4ae90355097f5963adc96ced98`.

Source review found a separate directly demonstrated protocol defect in the
production `host_getchunks_collect`: server_raft_host_getchunks resets count
on a double-collect retry but reuses reply record slots. A previously spanned
row that folds to zero spans leaves delta_base_n/delta_base_seq behind; a
previously packed record can leave its flag/reference on a later plain record
when an earlier row disappears. Strict client view validation then refuses
the mixed snapshot as PROTO. No exact live interleaving was captured in the
original K38 logs, so retrospective attribution remains unproven.

`tests/test_getchunks_retry_record.py` extracts the actual production collector
with controlled valid metadata snapshots, real wire/view validation and a byte
comparison to a fresh collect. It pins folded/shrinking spans, packed-slot
compaction and authority refusals. The pack-reference encoder is a narrow
identity stub for that case; it is not full packed-payload verification.
Untouched collector: initial spans valid, then **deltas=2/view=1, PROTO -7,
canonical mismatch / FAIL**. c3f3f313f clears each reused record before building
its new contents; all cuts validate and match fresh output. The existing
strict validator, proof/decision/recheck paths and all deadlines stay intact.
No new setting or retry was added.

Mac regression before/fixed: FAIL/PASS; the first generated harness had a
libc gets-name collision, corrected before product-negative evidence. Its
fixture `efs-getchunks-retry-5rqni_4a`, actual negative `41uw3h36` and fixed
`do0rofya` remain in this session's system temporary directory.
Native GCC regression **fixed 0 / untouched-negative 1**; fixtures
`/tmp/efs-getchunks-retry-yskfdm_3` and `/tmp/efs-getchunks-retry-lgyiu3oi`.
Raw `/data1/01/efs/evidence/an-getchunks-c3-regression` preserves both receipts.
Existing GETCHUNKS budget, publication RPC and READDIR cursor tests also pass
locally, including malformed-reply and authority-negative controls.

Complete c3f3f313f export SHA-256:
`e8a4f49288ef9441fc6a3c9d5bbfddcb161d3640938ee8259ddc37fe835e731e`.
Fresh source `/tmp/efs-taskAN-getchunks-c3/src`, private sibling/screen session
`taskAN-getchunks-c3`, raw `/data1/01/efs/evidence/an-getchunks-c3`.
run.sh invokes the same an_qualify.py full eight-job gate, nine individual
hardware RDMA audits and 25 strict named TCP/RDMA checks, commit c3f3f313f,
private tools and cluster script export. A failure stops the protocol; no
closed failure tree is modified. Native Linux regression passed before launch.
The earlier 992 loaded-diagnostic source was staged but never built or run;
the demonstrated fix superseded its planned loaded reproduction.

A separate fresh source `/tmp/efs-taskAN-precise-c3/src` was staged from the
same complete archive while the host was idle, before qualification launch.
It is reserved for the precise matrix/calibration only after correctness
receipts are reviewed; no measurement has started. Original K38, directory,
COMMIT/storage and host-coverage failures and their fixtures remain intact.
AN remains incomplete; no STATUS optimization is active.


### c3 gate CLOSED 548/1 FAIL/0 NOTRUN — replay harness declaration repair

The full gate closed FAIL, 754 s plus 16 s build. Only case 179,
`tests/test_replay_fan_budget.py`, failed: its extracted replay worker now
contains the failure-only fprintf diagnostic, but its C fixture lacked stdio.h.
This is this session's harness integration error, not a timeout or host NOTRUN.
Original compiler output is retained. Its old TemporaryDirectory removed the
generated C on failure; that gap is explicit (no EFS runtime store existed in
this unit fixture). f7b74f714 adds the declaration header and retains generated
C/binary/stdout/stderr/result or timeout receipts. All inherited/mid-run budget
and error-retention assertions and the original five-second runtime bound stay.

K38 passed this loaded occurrence (19.5 s, unchanged record/byte/pressure
checks); W130 22.5 s, K48 4.9 s and the new GETCHUNKS retry regression 0.1 s
also passed. All 80 ownership samples are clear. Separate native/named stages
did not run after the gate FAIL. Closed 22-file receipt manifest:
`f18c076ff2b9f02e5500697e799bd5742ad0d014a53476de20029643662467f1`.
No prior failure is cleared or reclassified by these passing occurrences.

f7b74f714 is **product-identical** to c3f3f313f (`git diff --exit-code` over
src/include/Makefile passes); it repairs the one test's declarations/retention.
Local replay-budget, GETCHUNKS budget and publication RPC checks pass. Native
harness receipts are under `/data1/01/efs/evidence/an-replay-harness-f7`.
The old compile failure is also recreated with cleanup-only retention so its
generated C can be kept; that is a new reproduction, not the original fixture.
Complete fresh archive SHA-256:
`176ec3a704d5391eb434081687ee8b842711de612f99fd20e83676f539ab0cfd`.
Source prefixes `/tmp/efs-taskAN-clean-f7/src` and `/tmp/efs-taskAN-precise-f7/src`
were staged while the host was free; all older prefixes remain unchanged.
Correctness qualification must pass before the reserved precise source runs.


Fresh f7 protocol is ACTIVE: source `/tmp/efs-taskAN-clean-f7/src`, private
SCREENDIR `/tmp/efs-taskAN-clean-f7/screen`, session `taskAN-clean-f7`, raw
`/data1/01/efs/evidence/an-clean-f7`. run.sh invokes:

```sh
python3 scripts/evidence/an/an_qualify.py --source /tmp/efs-taskAN-clean-f7/src \
  --prior /data1/01/efs/evidence/an-put-status-matrix-866 \
  --out /data1/01/efs/evidence/an-clean-f7/qualification --commit f7b74f714 \
  --private-tools /tmp/efs-taskAN-deps/tools \
  --cluster-scripts /tmp/efs-taskAN-clean-f7/cluster-scripts
```

Native fixed replay-budget harness exit 0 with fixture
`/tmp/efs-replay-budget-gbjaxumr`; recreated old compile failure exit 1, retained
`/tmp/efs-replay-budget-x9po2nnp`. No original directory was replaced.
Full gate, nine native RDMA receipts and all 25 named repeats still need review
before the idle `/tmp/efs-taskAN-precise-f7/src` measurement source may run.


### f7 full gate PASS; native 9/9 PASS; required named repeats RUNNING

Full **548/0 FAIL/0 NOTRUN PASS**, 750 s plus 16 s build. Seven advisory
budget overruns remain (K35 177.5 s/80 s, K38 44.5 s/35 s, coordinator-kill,
Raft-ack and three D39 shards); none is relabeled or given a longer bound.
K38, W130, K48, directory-materialization, GETCHUNKS retry and replay-budget
cases all ran and passed. This is the first complete passing gate for this
latest product/harness pair, not erasure of any earlier failure.

All nine individual hardware mlx5_2 native exits and complete logs reviewed:
protocol declaration TCP/RDMA; xprt 68 RDMA/0 large TCP; segmented app 9/9,
no app TCP; original 50 ms deadline; interrupted cleanup; both 400-round burst
modes; 16 reconnects with eight duplex/eight interrupted, no live assemblies
or connections left. Every positive audit counter is zero. The deliberate
TCP-byte negative control records exactly one violation and retires its RDMA
connection. Its exit 0 is the expected detection verdict, never a clean-data
sample. The test's generic Soft-RoCE label does not change the actual mlx5_2
InfiniBand hardware used. No checker or application-TCP rule was relaxed.

The same controller has entered the 25 named repeats (five K35 and five K33
on each TCP/RDMA transport, plus five W166 TCP). Those results, final ownership
and cleanup, source/binary checks and closed-log hashes remain pending. The
precise matrix/calibration sources stay idle until that review passes.


### f7 native protocol CLOSED PASS; directory confirmations active

All 25 named receipts and complete logs reviewed: K35 TCP 5/5 and RDMA 5/5
(3,850 exact files); K33 TCP 5/5 and RDMA 5/5 (1,008 exact files, zero retried
writes); W166 TCP 5/5 (ten exact fresh-reader payloads and the original 30 s
close bound). K35 retains its insufficient-proof EIO, degraded mark/rebuild,
leader-failure and four/five-member checks. No assertion or deadline changed.

All 273 ownership samples are clear, no private mounts remain and the final
all-account job inventory is empty. All 223 committed src/include/deps/Makefile
blob identities and recorded native binary hashes match. Reviewed receipt:
`/data1/01/efs/evidence/an-clean-f7/qualification/closed-review.json`.
Closed 91-file SHA256SUMS verified; manifest SHA-256
`421066cca5f8a3deb0decd5df4242411868b5eb749590e9474ee08238b6f77b9`.
All fixtures remain on fcstor014, with open investigation failures unchanged.

Additional directory confirmations use committed scripts/evidence/an/an_directory_confirm.py
(867817f1c) outside the verified f7 source: five fresh W130/K48 pairs, original
checks and deadlines, no rebuild, stop on first failure. Raw output:
`/data1/01/efs/evidence/an-dir-confirm-f7`. Exact invocation:

```sh
python3 /tmp/efs-taskAN-clean-f7/tools/an_directory_confirm.py \
  --source /tmp/efs-taskAN-clean-f7/src \
  --prior /data1/01/efs/evidence/an-clean-f7/qualification \
  --out /data1/01/efs/evidence/an-dir-confirm-f7
```

Two quiet checks 20 s apart and continuing ownership sampling apply. The
precise source remains idle until these results are reviewed; no measurement
runs alongside confirmations, builds or capture hashing.

The first added confirmation controller (867817f1c) FAILED before mounting:
its environment omitted the required private FUSE runtime, so system FUSE
3.10.2 refused `FUSE_3.12`. This is an avoidable controller setup failure,
not a passing W130 sample. Log/exit/ownership remain at
`/data1/01/efs/evidence/an-dir-confirm-f7`; fixture
`/tmp/efs-taskAN-clean-f7/directory-confirm-tmp/efs-w130-ufd5otqs` is retained.
Controller-only correction 624b94e18 supplies the same read-only FUSE 3.16.2
path used by the passing full protocol, and uses a new scratch directory.
Fresh confirmations write `an-dir-confirm-f7-624`; the failed attempt remains.
No product, test assertion, deadline or durability change.


### Fresh directory confirmations PASS; precise matrix ACTIVE

Corrected controller 624b94e18 finished all five W130 and five K48 runs with
individual/controller exit 0. Full log review: every K48 listing has 96 exact
names/payloads, no missing/extra entries; every W130 serve-first, frozen member,
quorum service and snapshot resync check passes without application retries.
Fourteen ownership samples clear, no owned mounts. The initial setup failure
remains separate. Both closed manifests were written before measurement:

| Raw evidence under /data1/01/efs/evidence | Files | SHA256SUMS SHA-256 |
| --- | ---: | --- |
| an-dir-confirm-f7 (FUSE prerequisite FAIL) | 5 | 5e5efa0cdf5174a381e9002dc3e99a4ceb2f508e94d40a87f700b7a96e40290e |
| an-dir-confirm-f7-624 (10 fresh checks PASS) | 23 | 462693f7c070fe45c1ddbdc205fbd3260dd78eeb0787c04d308180e9c574d13f |

The reserved exact f7 source now runs the precise matrix, private session
`taskAN-precise-f7`, SCREENDIR `/tmp/efs-taskAN-precise-f7/screen`:

```sh
cd /tmp/efs-taskAN-precise-f7/src
python3 scripts/evidence/an/an_host_run.py --base /tmp/efs-taskAN-precise-f7 \
  --out /data1/01/efs/evidence/an-put-status-precise-f7 \
  --commit f7b74f714 --matrix
```

This builds a temporary timing/counter probe in a separate `measure` copy,
then pilot and 36 fresh 60 s cells at the original fixed rate and topology.
Actual cycle/full mutex-hold fields must be present in every live node log;
missing fields cannot become a precise result. No builds/staging/heavy review
will run alongside measurement. Closed review/hashing precedes observer
calibration. No STATUS optimization has been selected from counter counts.


### Precise f7 matrix CLOSED PASS; observer controls ACTIVE

All 36 cells/controller exit 0; exact f7 exported product plus the committed
temporary counter/timing probe. All 59,040 offered operations completed,
1,968 final files matched exact fresh-reader payloads, 21,600 real GETATTRs,
zero missed slots, operation/fragment errors or cleanup errors. All 294
ownership samples clear; no private mounts remain. The parser accepts every
required field on all 90 live node/cell observations (member 3 stays down in
degraded windows). Captures include a separate 15 s pilot, not counted as a
matrix repeat. Full gate/named/native correctness remains a separate protocol.

Raw `/data1/01/efs/evidence/an-put-status-precise-f7`; closed 349-file manifest
SHA-256 `2f0627c4956275c4d2f37c1232d8aac70fc5318605ac793a2b6452975bc03158`,
verified with sha256sum before the next build. Reproduce with the preceding
an_host_run command and summarize using `python3 scripts/evidence/an/an_put_summarize.py RAW`.

Every entry below is median [minimum–maximum] across three fresh repeats.
STATUS counts only actual storage→leader frame sends; local authority checks
are separate. Proof/quorum counts include GETATTR and background traffic in
the bracketing snapshots, so they cannot be charged exclusively to STATUS.
Owner placement varies with fresh session identities. No batching is active;
batching residence is zero. No RPC-count reduction or healthy/down latency
difference is an optimization benefit.

| Writers / transport / state | STATUS RPC/fragment | Owner RPC/fragment | Proof admissions/fragment | Actual quorum rounds/fragment |
| --- | --- | --- | --- | --- |
| 1 / tcp / healthy | 0.667 [0.667–0.667] | 0.249 [0.242–0.319] | 5.254 [5.118–5.261] | 4.560 [4.487–4.600] |
| 1 / tcp / down | 0.500 [0.500–0.500] | 0.472 [0.445–0.483] | 6.928 [6.658–6.962] | 6.547 [6.247–6.575] |
| 1 / rdma / healthy | 0.667 [0.667–0.667] | 0.330 [0.237–0.334] | 5.271 [5.127–5.391] | 4.587 [4.470–4.619] |
| 1 / rdma / down | 0.500 [0.500–0.500] | 0.343 [0.333–0.348] | 6.827 [6.815–7.028] | 6.433 [6.357–6.605] |
| 8 / tcp / healthy | 0.667 [0.667–0.667] | 0.216 [0.213–0.217] | 4.162 [4.142–4.171] | 2.955 [2.940–2.957] |
| 8 / tcp / down | 0.500 [0.500–0.500] | 0.276 [0.263–0.276] | 5.485 [5.435–5.554] | 4.278 [4.195–4.307] |
| 8 / rdma / healthy | 0.667 [0.667–0.667] | 0.194 [0.192–0.213] | 4.111 [4.104–4.161] | 2.832 [2.802–2.889] |
| 8 / rdma / down | 0.500 [0.500–0.500] | 0.276 [0.261–0.279] | 5.460 [5.423–5.522] | 4.203 [4.162–4.247] |
| 32 / tcp / healthy | 0.667 [0.667–0.667] | 0.160 [0.157–0.161] | 3.950 [3.939–3.969] | 2.240 [2.224–2.252] |
| 32 / tcp / down | 0.500 [0.500–0.500] | 0.220 [0.193–0.222] | 5.106 [5.092–5.145] | 3.427 [3.422–3.485] |
| 32 / rdma / healthy | 0.667 [0.667–0.667] | 0.168 [0.165–0.168] | 3.934 [3.875–4.119] | 2.189 [2.150–2.373] |
| 32 / rdma / down | 0.500 [0.500–0.500] | 0.186 [0.186–0.211] | 5.091 [5.088–5.091] | 3.350 [3.344–3.356] |

| Writers / transport / state | MiB/s | Write p95 ms | Write p99 ms | GETATTR p50 µs | GETATTR p99 µs |
| --- | --- | --- | --- | --- | --- |
| 1 / tcp / healthy | 0.531 [0.531–0.531] | 160.12 [159.06–165.00] | 202.09 [168.71–210.97] | 70 [68–70] | 114 [108–119] |
| 1 / tcp / down | 0.531 [0.531–0.531] | 7.14 [6.79–7.86] | 9.18 [8.89–9.27] | 69 [68–70] | 108 [107–116] |
| 1 / rdma / healthy | 0.531 [0.531–0.531] | 166.80 [162.64–184.71] | 213.36 [205.39–233.25] | 94 [93–96] | 136 [134–137] |
| 1 / rdma / down | 0.531 [0.531–0.531] | 7.07 [6.51–7.23] | 9.32 [8.47–9.37] | 90 [89–91] | 126 [121–128] |
| 8 / tcp / healthy | 4.249 [4.249–4.249] | 115.80 [81.35–120.47] | 146.09 [123.95–202.76] | 77 [75–80] | 134 [125–138] |
| 8 / tcp / down | 4.250 [4.247–4.250] | 31.28 [31.00–31.53] | 38.96 [38.11–41.03] | 76 [76–77] | 127 [122–129] |
| 8 / rdma / healthy | 4.249 [4.249–4.250] | 115.55 [113.84–116.53] | 136.61 [132.20–173.43] | 100 [97–105] | 144 [143–164] |
| 8 / rdma / down | 4.249 [4.247–4.250] | 30.59 [30.53–32.23] | 40.95 [36.26–45.34] | 93 [91–95] | 136 [133–143] |
| 32 / tcp / healthy | 16.993 [16.990–16.997] | 192.23 [185.26–202.03] | 227.88 [224.49–252.52] | 86 [83–90] | 3616 [2925–4651] |
| 32 / tcp / down | 16.994 [16.994–16.995] | 115.20 [110.56–120.08] | 136.22 [127.83–140.28] | 84 [82–84] | 4874 [4709–8430] |
| 32 / rdma / healthy | 16.991 [16.988–16.994] | 201.15 [194.76–205.76] | 258.41 [242.34–258.84] | 108 [108–110] | 7242 [6137–7599] |
| 32 / rdma / down | 16.996 [16.991–16.997] | 111.02 [109.98–119.78] | 128.47 [126.71–138.85] | 101 [97–102] | 5407 [5005–6387] |

CPU is seconds per daemon in each 60 s window. Timing columns take the
largest live-node high-water within each cell, then median/range over its
three repeats: actual active cycle starts before mutex acquisition and ends
before the pump poll; full hold spans acquisition to unlock. These are maxima,
not per-acquisition percentiles or a wall-time wait-share estimate. The old
pump phase-sum field is retained in raw summaries but not mislabeled full hold.

| Writers / transport / state | Daemon CPU s, n1; n2; n3 | Cycle max µs | Full hold max µs | Wait max µs |
| --- | --- | --- | --- | --- |
| 1 / tcp / healthy | 1.30 [1.25–1.30]; 1.06 [1.05–1.08]; 0.67 [0.66–0.68] | 1679 [763–2325] | 1678 [762–2324] | 172 [162–221] |
| 1 / tcp / down | 0.85 [0.85–0.89]; 0.86 [0.80–0.86] | 508 [383–7059] | 508 [382–7059] | 227 [219–338] |
| 1 / rdma / healthy | 1.55 [1.47–1.70]; 1.64 [1.47–1.69]; 0.96 [0.94–0.96] | 2599 [1013–3396] | 2597 [1013–3394] | 210 [187–228] |
| 1 / rdma / down | 1.15 [1.08–1.17]; 1.01 [0.99–1.12] | 630 [475–2312] | 629 [457–2309] | 128 [87–248] |
| 8 / tcp / healthy | 4.94 [4.67–4.98]; 4.57 [4.53–4.82]; 3.01 [3.00–3.01] | 13136 [9199–13697] | 13135 [9197–13695] | 928 [734–1187] |
| 8 / tcp / down | 4.18 [4.07–4.34]; 4.10 [4.00–4.22] | 8005 [6852–8214] | 8003 [6849–8168] | 861 [725–1417] |
| 8 / rdma / healthy | 5.96 [5.96–6.43]; 6.07 [5.89–6.08]; 3.86 [3.83–3.92] | 19763 [16118–25946] | 19761 [16113–25943] | 1789 [944–2828] |
| 8 / rdma / down | 5.19 [5.05–5.24]; 4.98 [4.92–5.03] | 7802 [6769–8226] | 7798 [6767–8225] | 1276 [744–1537] |
| 32 / tcp / healthy | 15.79 [15.78–16.01]; 16.22 [16.17–16.22]; 10.55 [10.51–10.58] | 48254 [47108–52394] | 48250 [47107–52390] | 2962 [1995–3524] |
| 32 / tcp / down | 15.90 [15.58–16.43]; 15.23 [14.77–15.62] | 17348 [15026–26401] | 17346 [15022–26398] | 2906 [1526–5434] |
| 32 / rdma / healthy | 20.59 [20.13–20.73]; 18.79 [18.53–19.12]; 13.02 [12.77–13.18] | 59285 [54292–75665] | 59282 [54291–75660] | 4220 [3047–4726] |
| 32 / rdma / down | 18.63 [18.14–18.82]; 17.97 [17.70–18.61] | 16658 [13641–18966] | 16655 [13601–18963] | 2883 [2322–3217] |

The unchanged fixed-rate workload completes every offered slot; it is not a
saturation-throughput benchmark. Increased concurrency moves latency and
apply/drain high-water, but this is not a controlled STATUS-specific causal
comparison. Counter totals alone do not justify proof reuse or batching.
The healthy observer controls now compare native OBS=0, native OBS=1 and
probe OBS=1 at 1/32 writers, TCP/RDMA, three fresh repeats each:

```sh
python3 scripts/evidence/an/an_observer_calibrate.py --base /tmp/efs-taskAN-precise-f7 \
  --out /data1/01/efs/evidence/an-observer-calibrate-f7 \
  --prior /data1/01/efs/evidence/an-put-status-precise-f7 --commit f7b74f714
```

This is serial, after matrix review/hashing. No degraded calibration claim.
AN remains incomplete pending its result and the threshold/no-change decision.


## Final AN disposition — measured, no STATUS optimization

**COMPLETE for the user-approved fcstor014 scope; no merge/push.** Exact
product c3f3f313f plus test-only f7b74f714 passed the complete correctness
protocol, precise 36-cell matrix and 36 fresh observer controls. The prototype
in a80065c60 remains reverted (66daccd4d), never Linux-tested or performance
qualified. No proof cache, same-group proof reuse, STATUS queue or batch delay
is added. Exact ticket/session, term/group and storage authority checks remain.

### Closed observer controls and reproducibility

All 36 control/controller exits are zero, 71,280/71,280 offered operations,
2,376 exact final files, 21,600 real GETATTRs, no missed slots, operation or
cleanup errors. All 294 ownership samples are clear; no private mounts or
foreign jobs remain. No calibration ran alongside the matrix or capture hashing.
Raw `/data1/01/efs/evidence/an-observer-calibrate-f7`; closed 336-file verified
SHA256SUMS manifest SHA-256
`3e78d2d32a639510de6cb9edb40e82696bfe27e940876e74f95ec0e7d43a2340`.
The preceding calibration command recreates all controls from committed source.

Native OBS=0, native OBS=1 and probe OBS=1 each ran three fresh healthy cells
at 1/32 writers on both transports. No counterless control is substituted for
the primary four-counter matrix, and no degraded observer-calibration claim.
Entries are median [minimum–maximum] across three repeats:

| Writers / transport / mode | MiB/s | Write p95 ms | Write p99 ms | GETATTR p50 µs | GETATTR p99 µs |
| --- | --- | --- | --- | --- | --- |
| 1 / tcp / native-off | 0.531 [0.531–0.531] | 157.71 [119.34–162.03] | 189.48 [160.29–208.56] | 68 [67–68] | 112 [102–112] |
| 1 / tcp / native-on | 0.531 [0.531–0.531] | 164.18 [160.89–206.27] | 210.00 [191.36–212.18] | 69 [69–69] | 110 [108–115] |
| 1 / tcp / probe-on | 0.531 [0.531–0.531] | 160.29 [158.22–184.55] | 166.28 [166.12–202.14] | 70 [68–75] | 114 [105–116] |
| 1 / rdma / native-off | 0.531 [0.531–0.531] | 162.48 [162.14–197.89] | 207.65 [175.92–211.41] | 95 [94–101] | 135 [132–151] |
| 1 / rdma / native-on | 0.531 [0.531–0.531] | 162.76 [161.47–167.67] | 208.77 [202.86–214.62] | 95 [95–99] | 138 [134–147] |
| 1 / rdma / probe-on | 0.531 [0.531–0.531] | 162.21 [156.11–185.60] | 210.86 [168.94–217.31] | 95 [93–100] | 138 [136–143] |
| 32 / tcp / native-off | 16.987 [16.987–16.994] | 196.95 [196.20–208.70] | 235.76 [227.44–236.42] | 84 [82–86] | 4090 [3672–5557] |
| 32 / tcp / native-on | 16.996 [16.986–16.999] | 185.83 [185.64–199.15] | 247.41 [227.17–287.53] | 86 [85–91] | 3316 [2996–3452] |
| 32 / tcp / probe-on | 16.996 [16.994–16.999] | 187.71 [180.80–188.45] | 239.65 [217.24–249.41] | 84 [84–86] | 4177 [3134–4327] |
| 32 / rdma / native-off | 16.995 [16.994–16.996] | 195.67 [189.47–212.29] | 230.94 [229.66–268.38] | 110 [107–112] | 7288 [5205–7401] |
| 32 / rdma / native-on | 16.992 [16.990–16.996] | 206.93 [201.05–207.72] | 249.97 [242.96–254.28] | 108 [107–110] | 3528 [3179–6738] |
| 32 / rdma / probe-on | 16.989 [16.988–16.993] | 196.42 [192.24–211.08] | 281.27 [224.98–281.51] | 110 [106–114] | 4474 [3674–4904] |

| Writers / transport / mode | Daemon CPU s: n1; n2; n3 | FUSE CPU s |
| --- | --- | --- |
| 1 / tcp / native-off | 1.29 [1.08–1.31]; 1.05 [1.04–1.23]; 0.66 [0.65–0.66] | 0.13 [0.13–0.13] |
| 1 / tcp / native-on | 1.26 [1.13–1.33]; 1.09 [1.04–1.28]; 0.68 [0.68–0.68] | 0.13 [0.13–0.13] |
| 1 / tcp / probe-on | 1.27 [1.12–1.28]; 1.10 [1.09–1.28]; 0.67 [0.67–0.68] | 0.13 [0.12–0.13] |
| 1 / rdma / native-off | 1.77 [1.71–1.77]; 1.40 [1.39–1.47]; 0.92 [0.91–0.95] | 0.18 [0.17–0.18] |
| 1 / rdma / native-on | 1.77 [1.76–1.79]; 1.41 [1.40–1.41]; 0.93 [0.93–0.94] | 0.17 [0.17–0.17] |
| 1 / rdma / probe-on | 1.49 [1.48–1.71]; 1.67 [1.47–1.70]; 0.94 [0.93–0.94] | 0.18 [0.17–0.18] |
| 32 / tcp / native-off | 15.79 [15.65–15.80]; 16.09 [16.08–16.13]; 10.47 [10.44–10.54] | 6.83 [6.75–6.88] |
| 32 / tcp / native-on | 15.80 [15.69–15.83]; 16.02 [15.94–16.19]; 10.24 [10.18–10.30] | 6.72 [6.66–6.90] |
| 32 / tcp / probe-on | 16.97 [16.77–17.12]; 15.46 [15.34–15.57]; 10.56 [10.44–10.78] | 6.82 [6.80–6.82] |
| 32 / rdma / native-off | 19.81 [19.60–21.13]; 20.16 [19.09–20.32]; 13.29 [13.24–13.46] | 7.75 [7.72–7.84] |
| 32 / rdma / native-on | 20.83 [19.96–20.83]; 19.40 [18.90–20.08]; 13.17 [13.10–13.21] | 7.85 [7.79–7.92] |
| 32 / rdma / probe-on | 20.60 [19.11–20.67]; 19.10 [18.76–19.67]; 13.05 [12.99–13.19] | 7.78 [7.69–7.84] |

The observer is **not claimed neutral**. Native-off→native-on disjoint
three-repeat ranges occur at 1-writer TCP GETATTR p50 (+1.5%) and n3 CPU
(+3.0%), 32-writer TCP GETATTR p99 (−18.9%) and n3 CPU (−2.2%), and
32-writer RDMA n3 CPU (−0.9%). Native-on→probe-on disjoint ranges occur in
1-writer RDMA n1/n2 CPU (−15.8%/+18.4%) and 32-writer TCP n1/n2/n3 CPU
(+7.4%/−3.5%/+3.1%). Fresh session-owner routing varies, so these per-node
shifts are observed control differences, not a proven instrumentation-only
cause. All write p95/p99 and throughput ranges overlap between both adjacent
control modes. Small CPU effects cannot be advertised as optimization gains.

All 223 native source blobs match the qualified f7 commit; the temporary
probe's recorded before/after source hashes still match. Build/version logs
identify the native calibration and ANprobe builds. Final separate executable
hashes are in `/data1/01/efs/evidence/an-precise-f7-final-identity.json`, SHA-256
`a99afa99575d4d02166ae0e0192a94e910d0a178a657bbb369956a167fd44d76`.
These are explicitly post-run hashes, not invented pre-run binary receipts.
Native correctness binaries had their own before/after verified identities.

### Threshold decision and limits

The shared kit permits a change only with an end-to-end metric moving beyond
three-repeat spread. This experiment demonstrates all offered operations
completed and reports concurrency/state effects, but does not isolate a
STATUS-caused bottleneck or demonstrate a benefit from changing authority.
Large proof totals are not three independent quorum rounds per chunk; stat
and other metadata work share admissions/rounds. High-water peaks include
apply/drain work and do not establish STATUS wait share. Healthy/down latency
contrasts include the changed durable cohort and cannot justify proof reuse.

Therefore the threshold for introducing the proposed STATUS optimization is
**not demonstrated**. The safe task result is measured/no STATUS change, not
an assertion that this path can never benefit at a different offered rate or
on multiple physical hosts. Restoring the prototype would require a new
controlled causal A/B and the complete authority/correctness gates; it has
no measured performance verdict. The repaired correctness product remains
qualified separately: full 548/0 FAIL/0 NOTRUN, hardware audit 9/9, K35 and K33
TCP/RDMA each 5/5, W166 TCP 5/5, and extra W130/K48 each 5/5. Full gate includes
ticket upload/batch/session-admission and authority controls; no new STATUS
behavior needs an invented passing interleaving test.

All historical FAIL verdicts, original failed fixtures and open investigations
remain on their originating hosts. The one new FUSE-controller setup failure
also stays FAIL with its fixture/logs. No deadline, durability rule, application
retry or zero-application-TCP oracle was weakened; no bulk was deleted.
Original intermittent K38/directory histories are not retrospectively assigned
causes by later passes. Single isolated loopback host with real hardware RDMA,
fixed-rate 60 s cells and three repeats is the scope; F1/whole-release and
multi-host scale acceptance remain outside this completion.
