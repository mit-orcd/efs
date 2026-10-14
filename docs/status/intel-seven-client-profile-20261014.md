# Private Intel seven-client strace/perf investigation — 2026-10-14

**Diagnostic capture PASS; concurrent POSIX FAIL; W100 OPEN.** Follow-up to
[receive persistence](intel-receive-persistence-20261014.md) and the
[queue guard](intel-queue-guard-20261014.md). No filesystem correctness fix or
acceptance claim is made by this profiling checkpoint.

## Source, topology and provenance correction

Test/client source: devel `37ff1b4d45d048ed55f9585b89935c2e4928289f`.
The tracked production C/header files and Makefile selected by the
source check match that commit on both new build trees (208 files total,
zero mismatches). Clients built normally on both hosts at
2026-10-14T13:34:02Z, `-O3 -g -fno-omit-frame-pointer -march=native`.

An earlier statement that all seven prior clients were clean devel was wrong:
Intel2 still ran the discarded, uncommitted single-shard retry experiment.
Intel1 was restored correctly. The 1468/1512 untraced result and first profiling
capture therefore describe a **mixed-client** deployment, not a matched devel
baseline. The original retry experiment was not committed. Do not transfer
either result to exact-source acceptance or infer an old/new speedup.

All seven active test mounts were drained and cleanly stopped, then remounted
from immutable, git-archived devel source at
`/data1/efs/w100-private/devel-profile-37ff1b4d` on each host. No forced discard,
mkfs, quota increase or private daemon restart was used.

The four private daemon executables retain their existing `devel-rpcdeadline`
build labels, built13:00:35Z/13:00:36Z. Their production source/header files
match37ff except the unused Intel2 client-only `inode_rpc.c` experiment.
`nm`/`strings` on the live daemon ELFs show no `efs_client_rpc_*`,
`rpc_send_recv_shard` or either retry-exhaustion string: this experimental
object is not linked into those daemon executables. This is source/symbol
evidence plus recorded binary hashes, not a new37ff-stamped daemon build.

| Host / role | Live PIDs | Executable SHA256 |
|---|---|---|
|Intel1 private daemons|3573478,3573479|`7451f30efb4d26679f6f34fb88134e2e79ede7dd37cd555bd2ac0593ec5cb8e4`|
|Intel2 private daemons|3837388,3837389|`82a238708e29a0c9ce35aa0e87514ea9b8bcb9191f84f0c7b1b312eb5ca08ac3`|
|Intel1 test clients|3592081,3592113,3592145,3592177|`e1dc49117b4a45c66690e2318ac72db807a33505e00efd03b03cb4c55ae7e54b`|
|Intel2 test clients|3851110,3851140,3851170|`d48d87b39493dcb4ad525a1e795f31072826a724fc94826029e45f25504b935e`|

Four EFS nodes: Intel1 `.207` nodes1/2, Intel2 `.209` nodes3/4,
ports27432/27433. Two16GiB files-engine stores per host under
`/data{1,2}/efs/w100-private-deadline-posix/nN`; direct I/O.
Both hosts have eight CPUs and approximately15GiB RAM, Rocky Linux10.2.
TCP; `.efs` client/server0, packing0, expiry0, files engine (containers unused).
The private command policy remains version1/mask0. Actual `/proc` arguments,
environment, executable hashes and absent synthetic `.efs` were checked.

Runtime: `/data1/efs/w100-private/runtime-deadline-posix`.
Intel1 uses `/mnt` and `/diag-mounts/client-{1,2,3}` under that runtime;
Intel2 uses `/mnt` and `/diag-mounts/client-{1,2}`. Every mount seeds the
opposite host27432. The other pre-existing `extra-mounts` were idle and were
not included in the seven-client workload or tracing targets.

Each of seven independent FUSE clients runs all216 core cases with16 jobs:
up to112 concurrent test cases, with additional child processes inside some
cases. Each case owns a distinct namespace on the same export. This is not
a seven-writer shared-file oracle. Original30s minimum case budgets and
longer decorators (including75s for `dir_many_files`) are retained. Serial
cases run after that client's parallel cases. Each suite has900s outer bound,
10s kill grace. No failed case was retried inside the run.

## Capture and result

Matched run starts09:35:47 EDT (13:35:47Z). Client outer durations128.8–139.3s;
recorders run145.7–150.2s, including the coordinated startup wait. Seven TSVs
are complete; every suite exits1.

| Host | Clients | PASS | FAIL | Total |
|---|---:|---:|---:|---:|
|Intel1|4|823|41|864|
|Intel2|3|620|28|648|
|Total|7|1443|69|1512|

Failures:53 timeouts and16 EBUSY outcomes (including child-process failures
and an errno oracle receiving16). Every client times out in `dir_many_files`,
`names_crazy_dirs`, `dir_deep_nesting_beyond_64`,
`concurrent_appends_two_proc` and `mtime_monotonic_many_writes`.
Six each time out in depth60 and same-directory creates; five in concurrent
create/unlink; one in crazy-name roundtrip. Remaining cases fail with EBUSY.
No application EIO or wrong-byte failure is reported by these TSVs.

`perf record -e cpu-clock -F49 --call-graph dwarf,8192` and software `perf stat`
cover all seven selected clients and four daemons throughout. Both recordings
report zero lost samples. All33 per-process `strace` windows exist, no attach
stderr, at offsets approximately10–18,40–48,70–78s. Captured syscalls include
futex, poll/epoll, read/write, socket send/receive, positional I/O and sync;
`-s0` omits payload bytes. Completed SIGINT/detach recorder exits are expected
(-2), and no profiler/test process remains. Strace samples about24s/process;
the whole run is not continuously syscall-traced.

## Findings and limits

- **Admission waits are directly observed.** Client traces include12,184
  completed timed waits on `opid_cv`, about470.71 summed thread-seconds across
  the seven clients' sampled windows. The50ms wakeups are repeated waits, not
  individual multi-second syscalls. Client logs contain92 final
  `opid-admission: BUSY` events. Examples have only one live oldest identity
  while `next-oldest=16`; this is receipt-window backpressure, not proof that
  the window should be widened. The current trace does not label every
  outstanding identity's owning operation, so it cannot establish which
  oldest append/namespace request caused each refusal.
- **Server coordination is visible.** Futex addresses were matched to ELF
  symbols; `h->mu`/`h->cv_mu` and ReadIndex coalescing (`g_ri_coal`) are active.
  Node1 has22,742 completed host-mutex waits,97.77 summed thread-seconds,
  maximum66.213ms; node4 has14,109,46.76seconds, maximum51.994ms.
  Node2's leading ReadIndex condition words account for about132.62 summed
  thread-seconds. Thread sums overlap and are not wall duration. These are
  measured contention points, not proof of a new mutex deadlock.
- **Do not mistake idle threads for blocked application work.** The longest
  client futex waits (up to7.399s in these windows) include `g_inval` and
  `g_reclaim` background conditions. `g_client+0xcc28/cc2c` is the metadata
  flusher condition, not the operation-ID condition. Their elapsed wait alone
  cannot diagnose a filesystem deadlock. GDB was used on executable symbols
  and type information only; live pointer values were read from `/proc/PID/mem`.
  No GDB inferior was attached or changed.
- **No single multi-second disk-sync stall was captured.** Per-daemon maximum
  `fdatasync`: n1=72.868ms,n2=15.383ms,n3=44.582ms,n4=28.351ms.
  Maximum captured `fsync` is73.352ms. Repeated Raft-log/KV-WAL barriers are
  substantial: the sampled two Intel1 daemons perform8618 Raft-log and4514
  KV-WAL syncs; Intel2 performs9654 and6164 respectively. Many short serialized
  waits can still accumulate beyond case budgets. These24s windows do not
  exclude longer stalls elsewhere in the run.
- **Profiles do not show general CPU saturation.** Outside tracing windows,
  the selected EFS daemons+clients average about0.78 Intel1 CPU and0.70 Intel2
  CPU during the first120 workload seconds. Host-wide busy time is45.5%/30.9%
  of eight CPUs, with17.7% iowait on both; these host totals include test
  workers and any other activity. Flat CPU samples are spread across network
  processing, kernel locking, metadata lookup, allocation and Raft wakeups;
  no one userspace computation dominates. CPU sampling cannot itself measure
  time spent asleep.
- **REPORT rejection remains an independent lead.** Node2 logs14
  `invalid REPORT rec ...; retaining batch` events, `pack-invalid/-5`, in
  this corrected run. This is not `pack-nosize`; current logging does not
  identify which record invariant failed. There is no matching application
  EIO claim in the TSVs and no demonstrated causal link to the69 failures.
  In-flight/cancelled or unlinked test files must be distinguished from a
  live malformed publication before changing publication behavior.
- **Instrumentation changes the workload.** First-window BUSY messages start
  near attach time. Traced host busy time rises to70.6%/47.1%, and ptrace adds
  context switches. Therefore69 failures are diagnostic evidence, not a
  regression count versus the earlier mixed-source44 or persistence37.
  Untraced intervals within this run have already been affected by earlier
  tracing; they are not an independent clean acceptance baseline.

The leading investigation target is the lifetime and progress of the oldest
operation identity during mixed namespace/append traffic, correlated with
server ReadIndex/admission/proposal waits. Add operation-type/identity and
stage-duration evidence without changing the window, deadlines or retry
budget; isolate the REPORT record rejection separately. The present capture
does not prove a complete root cause and does not justify widening limits.

## Merged-code review

The latest merge37ff adds virtual-query lookup reservations and query
close/routing/status handling. Those optional interfaces are disabled here;
the ordinary create/unlink/append functions are not directly changed by that
merge. Similar loaded failures were already documented on a58 before37ff.
This narrows the search but is not an old/new controlled regression test.

Relevant earlier common-path changes are admission04f4c8c76, REPORT/EOF
integration3d9d72af7 and61e5c939d, host-mutex handoff382d537d6,
ordered-frame guard1ac603128 and receive persistencea0daf0ae3.
The traces motivate inspection of these interactions; no commit is identified
as faulty solely from its subject, and none was reverted for this capture.

## Reproduction, raw evidence and cleanup

Prerequisites: existing owned private fixture/mounts as above, SSH aliases,
matching devel clients already built/mounted, Python3, `perf`, `strace`,
`gdb`, `nm`, `readelf`; access to one's own processes. Both hosts already had
`perf_event_paranoid=-1` and `ptrace_scope=0`; no root changes were made.

```sh
python3 scripts/intel_private_posix_profile.py --output /OUTSIDE/GIT/unique-run
# On each host, using the remote_output printed by the controller:
python3 scripts/intel_private_profile_summary.py /data1/efs/w100-private/results/profile-seven-TIMESTAMP
```

The controller validates only the existing selected private processes, runs
the seven suites, records live binaries/configuration and emits small reports.
It does not deploy, restart daemons, mount clients or modify storage policy.
The summary pairs strace unfinished/resumed rows and maps futex addresses;
its GDB use reads types from ELF without attaching an inferior. It emits CPU
reports for nominal non-strace intervals20–38,50–68,80–110s. Actual trace
boundaries are retained to check those selections. Preserve full call graphs
for any follow-up; a flat hot-symbol list cannot prove an off-CPU cause.

Raw matched capture on **both hosts**:
`/data1/efs/w100-private/results/profile-seven-20261014T133535Z`.
Earlier mixed-source profile: `profile-seven-20261014T133105Z` under the same
parent. Local collected small/raw diagnostic files:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-profile-20261014/`.
No raw logs, recordings, payloads or copied C evidence are added to Git.

Perf-data SHA256: Intel1
`39f0d5ae7ba728f049c409dbb8d9ad2b3c88869af1f023a432fb12e10a61ff20`;
Intel2 `25f68d450070545b95d092d291d64fec81703a3c86b79cdd754fcc8f7cdc00a8`.
Each host's `capture-sha256.txt` covers the33 traces split across the hosts,
perf data, resource samples and manifests/summaries.

All owned recorders detached after completion; verified no remaining profiler
or profile-suite process. The seven matched private mounts remain serving for
further diagnosis. To retire them, use the matching source's
`scripts/client.sh stop MOUNT`, then the fixture's guarded daemon lifecycle
only after every client is drained. Do not force-discard or remove retained
failed-test payloads. Production Intel remains on17432/17433 with original
daemon/client PIDs3046489/3046496/3143563 and3321797/3321804/3412068; ORCD was
not contacted. W100 loaded correctness/acceptance remains open.
