# ORCD Raft/KV decisions — October 16, 2026

W119 normal-operation follow-up; W121 kernel gate remains OPEN. **Mixed
workload correctness PASS; full acceptance FAIL** because the original loaded
POSIX runs retain seven 15-second timeouts. Kernel remediation is deferred by
the user; query processes are isolated from mounted-reader address spaces.
This mitigation does not qualify arbitrary applications that fork while reading.
See [W121 diagnosis](orcd-w121-fork-read-20261016.md).

## Source, configuration and reproducibility

Ten rounds froze product source `4d6530dd83b086493a65b97cb344daf93291d166`.
Retained fixes include staged-DEL validation `502277d19`, paged truncate/fence
verdicts `4d6530dd8`, and nested-read ownership `6b5c34624`. The final standard
binaries are built from `c99491567b429af78c791d3e69afd0f7c07c1b71`; its
`src`, `include`, `deps` and Makefile are identical to the measured source.
The snapshot fixture correction described below changes tests only.

ORCD: four storage nodes fcstor003–006, seven clients fcstor007–013;
site user erbmi1 (UID 246111), native RDMA (`mlx5_2`), files storage
with server direct I/O, `.efs` enabled, expiry disabled,
four metadata nodes. Each round used fresh stores with a 17 GiB quota per
server. These are short fresh-namespace comparisons, not 10M-file or 400 GiB
qualification. Unplanned recovery remains out of scope.

Reproduce using [the committed runner and matrix](../../tests/live/orcd_raft_kv/README.md).
Each round has seven simultaneous 90-second workloads, eight writer threads
and two isolated query processes per client. Writers create unique 512-byte
files, write/fsync/close, compare every byte and unlink. Queries validate JSON
and exact 32-entry immutable `ls`/`find` fixtures, plus successful `stats`.
First and final defaults also run core POSIX with 16 jobs/client and unchanged
15-second case deadlines. Three default rounds bracket the ablations.

The serial variant changes only outer KV reader locking, preserving nested
ownership and correctness fixes. The noseek variant disables shared seek-cache
fill, retaining lookup and private scan behavior. The stripe1 variant changes
32 segment-cache lock stripes to one, preserving atomic accounting. Variants
exist only in isolated build trees; no new production tuning flags were added.

## Mounted results and decisions

All 70 client workload exits succeeded: **297,651 exact readbacks and 57,935
queries, zero mixed-operation errors**. Percentiles pool client samples;
write includes create/write/fsync/close, read includes open/read/close. CPU is
summed daemon CPU seconds divided by observed duration; RSS is the largest
sampled daemon peak, not a combined cluster allocation.

| Configuration | File cycles/s | Write/fsync p95 ms | Read p95 ms | `ls` p95 ms | Server CPU cores, total | Largest daemon peak RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| default1 | 330.5 | 135.7 | 34.1 | 305.2 | 1.6 | 257.1 |
| batchoff | 304.2 | 136.2 | 71.4 | 715.4 | 1.4 | 248.0 |
| overlapoff | 338.4 | 128.4 | 47.5 | 393.4 | 1.7 | 245.2 |
| noopoff | 314.8 | 132.4 | 32.8 | 274.9 | 1.6 | 268.1 |
| checkpoint10 | 336.5 | 123.7 | 49.7 | 401.5 | 1.7 | 226.4 |
| default2 | 329.1 | 131.5 | 31.5 | 266.9 | 1.7 | 264.8 |
| serial | 324.9 | 124.3 | 50.1 | 408.8 | 1.8 | 205.3 |
| noseek | 340.0 | 124.7 | 32.6 | 283.1 | 1.7 | 267.5 |
| stripe1 | 355.7 | 134.4 | 39.6 | 344.3 | 1.7 | 249.2 |
| default3 | 333.2 | 127.1 | 31.7 | 273.4 | 1.7 | 226.0 |

| Mechanism | Recommendation | Evidence and limit |
|---|---|---|
| Raft no-op and overlap batching | Keep both enabled | Disabling both drops cycles and raises read p95 to 71 ms and query p95 to 715 ms. Overlap alone off also worsens read/query tails. No-op alone off loses roughly 5% of cycles versus controls. |
| Applied checkpoint interval | Keep 100 ms | 10 ms has no balanced gain; read/query tails worsen. |
| Concurrent KV readers | Keep enabled | Serialized readers worsen read/query tails and consume more CPU in this workload. |
| Segment-cache lock stripes | Keep 32 | One stripe increases file-cycle throughput but worsens read/query tails. No claim of superiority for every metric. |
| Scan-seek cache fill | Retain existing implementation; benefit unproven here | Mounted noseek result is approximately neutral. This run does not justify claiming an ORCD speedup from cache fill. |
| Queued-read-vote experiment | Keep reverted/off | [Earlier OFF–ON–ON–OFF decision](orcd-queued-read-vote-20261016.md) remains NO. |

These sequential short trials expose workload tradeoffs, not statistically
established universal gains. Faster writer loops can change query contention.
Physical I/O, read syscall and read-character counters are retained by the
collector; they do not establish a meaningful noseek advantage.

## KV timeout investigation

Initial final-staging `timeout 120 ./tests/test_kv_lsm` timed out on fcstor005;
three other storage nodes passed. Its final compaction message matches the
snapshot scan-bounds stage. Twenty stage-marked full reruns passed, so a clean
rerun alone was insufficient evidence to close it.

Source inspection found two invalid test fixtures: `mock=*live` and
`overflow_store=*budget_store` copied pthread synchronization objects from a
live KV engine, then reinitialized only `mu`. Since concurrent-reader support,
`efs_kv_lsm_snapshot_pin` also acquires `view_rw`. A copied held lock has no
owner that will unlock that copy. An isolated probe held the live read lock,
copied the old fixture, released the original and reproduced a hang in the
snapshot-only run. This establishes the fixture defect and is consistent with
the original intermittent timeout; no original blocked-process stack survived.
ORCD rejected debugger attachment (`ptrace: Operation not permitted`).

The fix constructs zeroed synthetic fixtures with independently initialized
mutexes and rwlocks; it does not copy live engine synchronization or mutable
ownership. Segment references remain the intended bounded fixture. Overflow
refusal is tested without touching live counts. No production KV lock or
timeout was changed. Original timeout logs remain evidence.

**Corrected suite PASS: 40/40 full runs**, ten on each storage node, each
retaining its 120-second deadline. Meta-apply and Bloom checks pass on all four
nodes. The 20 focused concurrent-KV comparisons (five per node) also pass.

Reproduce validation from the corrected tree on each storage host:

```sh
make -j8 tests/test_kv_lsm tests/test_kv_seg_bloom tests/test_kv_lsm_concurrent
for i in $(seq 1 10); do timeout 120 ./tests/test_kv_lsm >kv-$i.log 2>&1 || exit; done
timeout 60 ./tests/test_kv_seg_bloom
timeout 60 ./tests/test_kv_lsm_concurrent 10
```

Logs must be directed outside the repository. Tests own PID-specific `/tmp`
stores and remove them after success; retain a failed fixture for diagnosis.
To reproduce the old fixture defect in a disposable exported pre-fix tree,
replace `struct kv_lsm *live=kv->ctx,mock=*live;` in the scan-bounds test with:

```c
struct kv_lsm *live=kv->ctx;
pthread_rwlock_rdlock(&live->view_rw);
struct kv_lsm mock=*live;
pthread_rwlock_unlock(&live->view_rw);
```

Build the test and run `EFS_TEST_KV_SNAPSHOT_ONLY=1 timeout 5
./tests/test_kv_lsm`. The copied held lock stalls snapshot pin; kill only that
owned test process. This deliberately forces the invalid copied-lock state,
not the exact scheduling of the original intermittent failure.

Focused local NOSYNC KV runs (10 seconds each, four storage hosts) exercise
writes, concurrent gets/scans, flush/compaction and nested gets in scan
callbacks. These are semantic/microbenchmark tests, not RDMA or durability
measurements. Means per host per second:

| Variant | Runs | Writes/s | Gets/s | Scans/s |
|---|---:|---:|---:|---:|
| Default | 8 | 33,300 | 548,155 | 18,279 |
| One stripe | 4 | 33,525 | 528,050 | 17,616 |
| No seek fill | 4 | 33,675 | 548,727 | 18,300 |
| Serialized readers | 4 | 27,300 | 459,963 | 15,343 |

This supports retaining concurrent readers. Seek-fill benefit remains
unproven; one stripe has lower read/scan throughput in this fixture.

## Orderly restart and payload verification

**PASS:** all seven clients unmounted and all four daemons stopped cleanly.
Standard `/tmp/efs` binaries from c99491567 replaced the experimental launch
paths and restarted the same default3 stores without formatting. After remount:
49/49 cross-client 2 MiB marker comparisons, 224/224 fixed 512-byte payloads,
and 7/7 cold `.efs/ls` JSON checks passed. All payload comparisons check full
contents, not merely size. The first verifier stopped after marker reads on a
harness `KeyError: root`; workload JSON records `host`, not `root`. The committed
runner now derives its deterministic fixture path from that host field, and
all checks were rerun on the already restarted stores.

Final `/proc` inventory verifies exactly one owned EFS process per host,
UID 246111 throughout, `/tmp/efs/efsd` or `/tmp/efs/efs-fuse`, RDMA device
handles and requested feature environment. All four daemons have the staged
standard SHA256 below and `--direct-io`. No ablation
binary or diagnostic FUSE binary remains active. `cold-restart.json` and
`final-runtime.json` retain the detailed verification and identities.

## Loaded POSIX failures retained

- First default: 1,507/1,512 PASS; deep-directory timeout on 007/008/011/013,
  and repeated-mtime timeout on 008.
- Final default: 1,510/1,512 PASS; deep-directory and unusual-name directory
  timeouts on 012.
- Combined: **3,017/3,024 PASS, seven FAIL, no SKIP/NOTRUN**. Quiet reruns are
  diagnostic and cannot replace this concurrent acceptance gate.
- After restart, all three cases on each of seven clients pass with jobs=1:
  **21/21 PASS**, unchanged deadlines. The seven loaded failures remain open.

## Raw evidence and identities

Raw logs, resource samples, binary hashes, workload JSON, original and fixed
KV tests, POSIX results and controller records stay outside Git:

- Shared ORCD root:
  `/home/erbmi1/orcd/scratch/efs/orcd-test/raft-kv-4d6530dd8-20261016/`.
- Local controller root:
  `/Users/mike/Documents/Codex/2026-10-08/efs/operations/orcd-w121-20261016/`.
- Measured default daemon SHA256:
  `9a758a94e70a00c2d32304db880ffcd368c3e7f599045cb8b4801f7cde1b7819`.
- Staged standard daemon SHA256:
  `8e0b4f8c4eb7e8bf7c31878355a7a0de9978133ab9886332159974e7d7f951ca`.

`comparison.json` preserves all ten rounds; per-host resource identity records
and `binary.sha256` preserve build and transport identities. Final runtime
identity and cold verification are captured separately. Setup interruptions
were an SSH connection burst (reduced control concurrency) and fresh MKFS
before a leader existed (bounded retry of explicit NOT_PRIMARY/BUSY only).
Neither interruption is counted as a measured performance result. Existing
large stores and all experiment stores are preserved inactive after switching
the standard deployment to the final default store.
