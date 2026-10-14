# Intel live tiny-file profiles — 2026-10-14

**Diagnostic capture PASS; speedup qualification NOTRUN; existing-tree read
correctness FAIL.** Purpose: the human-requested performance investigation of
the main Intel cluster and its clients, following
[package 3 deployment readiness](intel-external-clients-deployment-20261014.md).
This does not reopen W100 or qualify a performance implementation.

Subsequent performance implementation: [LSM candidate indexing](intel-lsm-candidate-index-20261014.md)
is deployed at6cbd99e54. Bounded engine and mounted comparisons improve;
full/sustained qualification remains NOTRUN. The proposals/observations below
are historical source evidence, not a claim that the implementation is pending.

Subsequent correction: [direct checksum-tail EIO](intel-direct-tail-eio-20261014.md)
identifies and fixes the stored buffered-tail/direct-reader incompatibility.
Main Intel/all four clients redeployed;32previous failures per client pass the
bounded smoke. Historical observations below remain unchanged; a complete
old-tree inventory was not rerun.

## Tested deployment and workload

Frozen running source `85f2a199aa391647a1ac49ab43f1a4f1e5d5880e`, devel,
build ID `85f2a199aa39`, build time `2026-10-14T18:23:50Z`. Native GCC
`-O3 -g -fno-omit-frame-pointer -march=native -mtune=native`; binary SHA256s,
hardware/OS and topology are recorded in the linked deployment report and
rechecked through `/proc/PID/exe` in each capture. Intel1/2 each run two
400 GiB files-engine daemons on ports17432/17433 and one main FUSE client.
Daemon direct I/O, TCP; `.efs` client/server, packing, containers and expiry OFF.
AMD/xorinox use their independent `/data1/efs/intel-client` releases and
`/data1/efs/intel-mnt`. Intel clients use `/data1/efs/mnt`.

|Host|CPU / logical CPUs|Data-interface negotiated speed|
|---|---|---|
|Intel1 and Intel2|Core Ultra7 256V /8 each|`enp128s0`,5000Mb/s each|
|AMD|Ryzen AI MAX+395 /32|`enxc84d4435029d`,2500Mb/s|
|xorinox|Ryzen9 7940HS /16|`enp2s0`,2500Mb/s|

Speeds were read from `/sys/class/net/INTERFACE/speed`; external clients are
on2.5Gb/s links even though the Intel pair negotiates5Gb/s.

No redeployment, daemon/client restart, cache flush, kernel setting, storage
mutation, workload launch or workload parameter change was performed. Existing
private Intel processes on27432/27433, AMD's own cluster and xorinox VMs remain
running; they are excluded from target PIDs but can affect host-wide counters.

Existing human workloads discovered at capture time:

- AMD PID3653848: `python3 ./tiny_files.py --max-depth 8
  --max-files-per-folder 1000 --max-files 1000000 --workers 4
  /data1/efs/intel-mnt/tiny-files1`. New-file payloads default to1–512bytes;
  ordinary buffered Python writes, close per file, no explicit application
  fsync. These are application semantics; the daemon uses direct I/O.
- Xorinox PID653649: `python3 ./tiny_files_md5.py --workers 4
  /data1/efs/intel-mnt/`. Recursive read-only crawl; each file has path-based
  open, fstat, read, fstat, path-based stat and close, with stability checks.
  Its ongoing manifest is not a complete inventory or snapshot.
- Both main Intel FUSE clients were idle during the primary capture.

## Measurements

45seconds of99Hz user-cycle perf sampling, approximately15:30:04–15:30:51 EDT
(19:30 UTC); separate12second wall-clock strace summaries immediately after.
Additional10second application summaries followed around15:32 EDT. No lost
perf samples reported. Intel exposes separate Atom/Core PMUs: percentages below
are ranges across the two event groups, not their sum. CPU percentages are
fractions of sampled user CPU, not fractions of wall time. Sleeping RPC and
storage waits are not CPU samples. Short syscall traces perturb throughput;
their elapsed times overlap across threads and must not be summed as a
single-thread runtime or used as an untraced benchmark.

|Target|Observed hot path or wait|Interpretation|
|---|---|---|
|Intel1 daemons|`efs_meta_apply_getattr` inclusive69.83–72.16%; beneath `server_raft_host_lookup`, `efs_txn_reduce_read_ex`, `lsm_scan_prefix`, `lookup`|Strongest CPU concentration is metadata read/reduction work|
|Intel1 daemons|Self `memcmp`19.60–20.13%, `kv_seg_excludes`8.98–10.34%, `merge_scan`5.61–8.67%, `search_block`6.28–8.40%|Comparisons, candidate traversal and block lookups dominate, rather than one expensive hashing loop|
|Intel2 daemons|`host_pump`/apply, mutexes, KV search and BLAKE3; mutex-lock self6.29–9.59%, `host_waiters_wake`3.11–4.86%|Replication/apply and synchronization also matter; the hosts do not have identical roles|
|AMD Intel FUSE|BLAKE3 self25.28%, lock/unlock27.24% combined; close/flush path inclusive51.49%|Full fragment construction/publication for tiny writes, plus synchronization|
|Xorinox Intel FUSE|Read callback inclusive29.81%, LOOKUP17.27%, OPEN9.35%, GETATTR8.15%; memmove self12.30%|Substantial metadata and reconstruction work for very small reads|
|Intel1/2 main FUSE|Zero application I/O bytes/syscalls in `/proc/PID/io` delta; timer/poll/futex waits|Idle clients are not a throughput measurement|
|AMD application, traced10s|1031opens,1029closes; mean open17.756ms, close19.260ms, write44us|Create/close publication dominates the application write call|
|Xorinox application, traced10s|1295opens,1084reads,1081path-stats; mean open17.542ms, path-stat12.260ms, read4.540ms|Path/metadata work is larger than hashing or moving a few hundred payload bytes|

The means are syscall means over this concurrent live trace, not file-latency
percentiles. Open counts include directory operations. Both application traces
contain ordinary futex interruptions; strace's errors column is not an EFS
retry or packet-loss counter. FUSE nonblocking recvfrom EAGAIN is also not
evidence of failed RPCs.

MANIFEST snapshots shortly after sampling contain L0/L1 counts:
n1=141/144, n2=263/287, n3=263/287, n4=138/144. These are persisted manifest
observations, not an atomic cluster snapshot or proof of a compaction failure.
Recent logs show successful compactions and occasional100–130ms Raft apply/
read waits. Each LSM is processing a substantial pre-existing namespace.

Across the approximately46second perf interval, aggregate main-daemon CPU
was about1.12cores on Intel1 and1.02cores on Intel2 (user+system ticks,
100Hz); client CPU was about0.06core AMD and0.07core xorinox. This is not
whole-host saturation. A serialized critical path can limit a four-worker
workload without saturating eight cores.

Storage and transport are competing costs, but these observations do not make
either the leading explanation for metadata CPU:

- Separate12second daemon traces report15,394 fsync calls and27,037 fdatasync
  calls across four daemons. Mean fsync447–570us; mean fdatasync1.020–1.431ms
  on the three nodes that issued it in that trace. Calls span different EFS
  persistence paths; aggregate summaries cannot attribute every sync to a file
  or establish device latency distributions. Successful PUT/disk-write counters
  sampled later show cumulative mean disk writes2.9–4.2ms, zero PUT errors.
- Actual data-interface deltas have zero RX/TX errors/drops. Largest physical
  interface transmit delta is Intel2:1,810,066,940bytes in approximately46s,
  about39MB/s, well below the nominal5Gb/s ceiling. Counters include unrelated
  host activity; this is not a network ceiling benchmark.
- Host TCP retransmit increments: Intel1=0, Intel2=2, AMD=4, xorinox=0.
  Intel2 socket snapshots account for one additional retransmission on an
  Intel1→Intel2 EFS connection and one on a loopback EFS connection. AMD's
  surviving Intel socket snapshots show no retransmission counter. Short-lived
  connections and unrelated traffic prevent assigning all host retransmits.
  Host TCP segment counters are affected by offloads and are not wire packet
  loss rates. These small counts do not support loss as the main bottleneck.

## Most promising performance theory

**Reduce metadata read amplification and time under the LSM mutex first.**
This is a proposal, not a demonstrated speedup or proof of mutex wait causality.

Source explains the CPU stacks:

- [GETATTR](../../src/meta/meta_apply.c) (`efs_meta_apply_getattr`) collects
  the inode and active lanes, reads pending transaction reductions and verifies
  stability. `efs_txn_reduce_read_ex` in
  [txn.c](../../src/meta/txn.c) reads the materialized lane and scans its
  reduction prefix; GETATTR subsequently reads the lane again. Pending committed
  reductions must remain visible. Simply ignoring them would be incorrect.
- [kv_lsm.c](../../src/kv/kv_lsm.c) `lookup` and `merge_scan` iterate L0+L1
  segment arrays. Range exclusion already exists; adding the same exclusion
  test again is not a new optimization. `lsm_get` holds `l->mu` for cached
  searches; misses can release it around pinned pread with view validation.
  `lsm_scan_from` holds it through the merge. L1 is explicitly sorted/disjoint.
- [kv_seg.c](../../src/kv/kv_seg.c) already binary-searches indexed block
  entries in `search_block`; replacing a supposed linear block search would
  likewise duplicate an optimization already present.

First experiment: publish in-memory, view-consistent candidate indexes with
segment changes. Binary-search full L1 key bounds for point/prefix ranges;
index overlapping L0 candidates by range while preserving newest-first
precedence. Allocate/initialize scan sources for actual candidates rather than
the entire segment inventory. Measure how many candidates remain before adding
a more elaborate filter: L0 overlap can limit this first optimization's gain.
Keep memtable/pending/apply-staging precedence, tombstones, pinning and retry on
view change. Unknown bounds must include a segment; never infer absence from
an incomplete index. This needs no on-disk format change or relaxed freshness.

Then evaluate returning a decoded lane plus its reduction from one consistent
collect to avoid duplicate reads. Preserve the existing sequence/retry checks;
do not just delete the second lookup. An RPC-scoped exact-result reuse is a
safer starting point than a long-lived attribute cache. Zero FUSE timeouts and
the existing one-shot LOOKUP memo already have correctness constraints; raising
TTLs without cross-client invalidation is not the recommendation.

This can plausibly improve tiny-file traversal and reduce collateral apply
delay under load. It cannot yet justify a specific throughput multiplier:
70% user CPU is not70% end-to-end time. Even a2x reduction of that CPU component
would imply about1.54x Intel1 user-CPU efficiency under an unchanged mix,
not a measured1.54x application speedup. Four-worker RPC/durability waits may
be the next bound. No2x/10x real-world performance claim is established.

Two follow-ups, in priority order:

1. Lower path-walk RPC count in the workload/client interface. Keep directories
   open and use descriptor-relative open/stat in the crawler and writer while
   retaining no-follow/stability checks. Deep absolute paths plus zero FUSE
   entry/attribute TTL repeat ancestor lookups. Compare identical trees and
   results before calling it faster. This is a workflow optimization, not a
   replacement for improving ordinary absolute-path behavior.
2. Investigate ordinary-files tiny-write amplification separately.
   [write.c](../../src/client/write.c) `dcache_put_now_budgeted` encodes and
   hashes a full128KiB stripe into three64KiB fragments even for a1–512byte
   write; actual PUT counters confirm65,536bytes per fragment. That is
   192KiB payload across three PUTs before headers/metadata (384x a512byte
   file; about768x the mean256byte payload). Tiny-file close also publishes
   metadata. Evaluate exact byte/flush/RPC counts first; safe durability
   coalescing or a future right-sized ordinary fragment design needs separate
   correctness/recovery design. Do not enable packing/containers, skip durable
   acknowledgements, or increase retry windows as a performance fix.

## Read correctness discovered during observation

**FAIL, cause unassigned.** Xorinox's live manifest snapshot contains29,022
successful file hashes and316 EIO records, all in the old
`tiny-intel2-1m-20261010` tree,7,466,682 successful bytes. Later client log
entries say `decode error (efs_rc=-9)` and show retained delta mappings across
a metadata refresh. Server GET counters also contain errors. These are
correctness failures, not evidence that TCP lost the data. The manifest was
read while growing; counts are a dated observation, not a final failure rate.

Read-only rechecks of these three existing paths fail with errno5 on ALL four
clients (Intel1, Intel2, AMD, xorinox):

- `tiny-intel2-1m-20261010/folder_115666/file_000000.txt`, inode1885194.
- `tiny-intel2-1m-20261010/folder_115402/file_000000.txt`, inode2158298.
- `tiny-intel2-1m-20261010/folder_115402/file_000001.txt`, inode2162394.

Controls `tiny-intel2-1m-20261010/file_000002.txt`, `file_000001.txt`,
`file_000000.txt` succeed with identical lengths/SHA256 through all four
mounts (inodes14042,9946,5850). This establishes reproduction across clients,
not missing-fragment, corruption, migration or new-code causation. No store or
file was repaired, erased or rewritten. Earlier512-file readiness samples
remain valid for those samples; they never established whole-tree integrity.
Resolve this independently before accepting a successful-read performance
baseline. Existing human workloads were left running.

## Reproduction and next acceptance experiment

Prerequisites: same-user SSH, Linux perf/strace/python3/findmnt/ss, allowed
same-user ptrace/perf (current perf_event_paranoid=-1, ptrace_scope=0).
Do not alter sysctls just to reproduce. Confirm current PIDs before attaching;
never substitute a private Intel or AMD-local daemon. From the repository:

```sh
ssh intel1_efs 'bash -s -- /data1/efs/NEW-PROFILE 3837635,3837642,3838384 45' < scripts/profile_intel_live.sh
ssh intel2_efs 'bash -s -- /data1/efs/NEW-PROFILE 4111174,4111181,4111567 45' < scripts/profile_intel_live.sh
ssh amd_efs 'bash -s -- /data1/efs/NEW-PROFILE 3217005 45' < scripts/profile_intel_live.sh
ssh xorinox_efs 'bash -s -- /data1/efs/NEW-PROFILE 650200 45' < scripts/profile_intel_live.sh
```

Launch these concurrently with distinct/new output directories. The helper
validates ownership, samples CPU, `/proc` I/O/thread waits and host TCP/interface
counters, then takes12second summary-only syscall traces. `timeout -s INT`
detaches strace; it does not signal the target. For the application summaries:

```sh
timeout -s INT 10 strace -f -c -w -p APPLICATION_PID -o application-summary.txt
```

Expected timeout exit124 is the bounded detach. Check perf exit and lost samples,
nonempty summaries, original PIDs/executables and continuing human jobs afterward.
All eight service targets remained running and all tracers detached in this run.
No fixture was created; cleanup concerns only new profiler output directories.
Leave existing mounts/workloads/stores untouched.

For an implementation comparison, use an isolated matched Intel fixture and
identical source/profile/storage age: small-file creation and crawl independently
and concurrently,1/4/16 workers, repeated untraced baselines and candidates.
Record successful files/s, payload and wire/disk bytes/file, CPU/file, RPC count,
segment candidates/scan, lock hold/wait and p50/p95/p99 open/close/read/stat.
Repeat with growing L0/L1 inventory and warm/cold client metadata. Gate on exact
bytes/EOF and complete inventory; rename/unlink/truncate races, transaction
prepare/decision/resolve, compaction with retained readers, cancellation,
allocation/I/O faults, restart and native/sanitizer qualification must pass.
Do not remove ReadIndex or transaction visibility to win the comparison.

Raw captures retained outside Git at
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-live-profile-20261014/`
and on each host at `/data1/efs/intel-profile-20261014-1931/`.
Local final archive SHA256s:

|Archive|SHA256|
|---|---|
|intel1_efs.tar.gz|`88cc75a2d476bd06cd35cacaebabd893e8304f52a615a07159438571ba3fb3bb`|
|intel2_efs.tar.gz|`6a24d7562d5992e6183b6789be9d6caa844e0429dc13e62dc51259a2ec7b8373`|
|amd_efs.tar.gz|`14ee4ae0c0ecbb80cc52ebd4e83a8d14ff665de96fc78e4660ea93874b8d30aa`|
|xorinox_efs.tar.gz|`4343aa3c103e9d94df51e4de4a1bbe9970f64b587adeed5916f0cfbf34de2760`|

These files contain raw traces/profiles, not production payload copies; none
are committed. The script and independent reproduction instructions do not
require access to these retained captures.
