# ORCD-test: quadratic client durability lookup — October 28, 2026

**Purpose:** profile the slow single-stream `dd`, fix its largest demonstrated
cost, and retest (K32 write cost; K35 durability proof). **Scoped fix PASS:**
client durability bookkeeping consumed 65.21% of baseline user-cycle samples;
the exact-identity index removes its quadratic work. Matched unprofiled median
throughput rises **207.46 → 331.90 MiB/s (1.60×)** and FUSE fsync handling falls
**8.43–8.76 s → 0.14–0.20 s**. **The 1 GB/s target is NOT MET. Full code gate
FAIL: 527 commands, 8 FAIL, 5 NOTRUN.** Six sanitizer/runtime failures reproduce
on the parent; the probe-inventory omission has a passing focused correction;
the full-quota assertion remains an unresolved incident. No release PASS.

Branch `agent/orcd-dd-profile-20261028`, from devel `9e4d66a8c`. Reproducer and
sampler `f5b782726`; product fix **`3769f44bb`**. Later changes on this branch
are tests, inventory/evidence and the regenerated navigation graph. No merge
or push. The cluster is intentionally left on the fixed product snapshot.

## Environment and source control

- Four storage hosts fcstor003–006, six existing NVMe roots per host; seven
  clients fcstor007–013. Timed `dd` runs on fcstor007 only.
- RHEL 9.8, kernel `5.14.0-687.39.1.el9_8`, EPYC 7402P / 24 visible CPUs and
  251 GiB per host; private libfuse 3.16.2, native RDMA `mlx5_2`, server fragment
  I/O O_DIRECT, normal buffered FUSE writes. No quota/transport/mode changes.
- Existing stores kept, checked stops and readiness on each deployment;
  no reformat. Every daemon and mounted client has the same source identity
  within a phase. Unrelated Task V processes/mounts are excluded by explicit
  ORCD-test store/mount ownership, never stopped.
- CPU baseline: installed `74df76244`, whose `data_commit.c` differs from
  `f5b782726` only by the compiled-out test counter. The **timed comparison**
  uses complete parent `f5b782726` and child `3769f44bb` deployments, so other
  devel changes since `74df76244` are not credited to this fix.

## What the raw profiles establish

Same-user CPU sampling on the FUSE client and **all four** efsd processes,
99 Hz `cycles:u` with DWARF stacks. All five CPU reports record zero lost
samples. Baseline client self costs:

| Symbol | User-cycle samples |
| --- | ---: |
| `stripe_note_locked` | 36.08% |
| `stripe_prove` | 29.13% |
| `__memmove_avx_unaligned_erms` | 18.14% |

The old table hashes by inode only. All a file's chunks therefore share one
linked chain. Each new PUT searches every preceding chunk; fsync's COMMIT
fold looks up every captured object in that chain again, while holding the
client-wide `commit_mu`. Both paths do quadratic work as one file grows.
This is CPU and local head-of-line blocking, not a requirement of durability.

The fix retains the inode chain for one-file snapshots, adds a hash index on
the full `(ino, chunk, PUT object)` identity and a previous-link for constant
work unlinking from the inode chain. Expected lookup/proof work is linear
over the file; collision matching still compares the whole identity. Storage
verifiers, acknowledgement-sequence bounds, degraded marks, retry/error rules,
table cap and locking authority are unchanged. Additional bounded memory is
eight bytes per record plus a 256 KiB bucket array (about 2.25 MiB at cap).
No format, wire change, new environment reader or operational switch.

In the fixed profile these bookkeeping symbols are below the report's 1%
threshold. Client copies now dominate (51.12% in the main memmove symbol):
dirty-image extraction/reinstallation, RDMA frame construction and FUSE buffer
copying. Blocking stacks include PUT completion polling and PUT-ticket RPC
waits, plus REPORT receive. In the fixed CPU capture, dirty-image extraction
and reinstallation account for about 12.29% and 14.66% of sampled stacks,
RDMA frame copying about 15.69%, and FUSE copying about 5.08%. These stack
attributions are samples, not additive end-to-end latency accounting. They
are remaining optimization candidates, not fixed by this change.

Server CPU is spread over BLAKE3, copying, KV comparisons/scans, CRC and locks;
there is no matching single 65% server function. Interval iostat samples show
NVMe peak utilization of 29–61% in the baseline capture, 32–53% after the fix;
the largest observed write-await is 1.81 ms. These observations do **not**
establish peak device capacity or exclude synchronous I/O/metadata latency.
Capture-window daemon averages include idle time and must not be mistaken
for DD-only utilization or whole-machine CPU saturation.

Ptrace and scheduler tracepoints are denied on this host. `perf` software
context-switch sampling works and records blocking stacks, but their report
percentages count switch samples, **not accumulated off-CPU seconds**. The
first full-rate switch capture is about 52 GiB and loses events (29.68%
reported on fcstor003; lost chunks on all five hosts). Its postprocessing
exceeded the controller deadline; raw data and partial reports are retained, and only the
sampler's own expensive report processes were stopped. Later captures sample
every 100 switches. This does not affect the independent unprofiled timings.
No kernel setting, product flag, tracing privilege or test timeout was changed.

## Matched measurements

Every row is one new 8 GiB zero-filled file, one process on fcstor007:

```sh
dd if=/dev/zero of=UNIQUE_FILE bs=1M count=8192 conv=fdatasync status=progress
```

| Build | Wall seconds, sync included | MiB/s | FUSE fsync handling ms |
| --- | ---: | ---: | ---: |
| parent `f5b782726`, 1 | 39.817 | 205.74 | 8426 |
| parent `f5b782726`, 2 | 39.163 | 209.18 | 8759 |
| fix `3769f44bb`, 1 | 23.610 | 346.97 | 175 |
| fix `3769f44bb`, 2 | 24.682 | 331.90 | 200 |
| fix `3769f44bb`, 3 | 27.155 | 301.68 | 144 |

Fsync durations are the existing `fsync-split` records correlated by file
inode; they cover the FUSE flush/publication/durability-proof path, **not an
isolated disk syscall**. Every dd returns zero with exact expected file size.
Fresh names, no dropped cache, no altered publication/durability policy.

For context only: the original `74df76244` unprofiled run was 54.45 s /
150.44 MiB/s; its heavily profiled run 60.75 s / 134.84 MiB/s. The fixed
profiled run is 26.24 s / 312.17 MiB/s. Those profiler timings are **not**
the matched speedup calculation.

Repeated IOR on the fixed snapshot, one iteration with `-R -e`, the original
four-case script (wide cases `-C`, 8 ranks per client):

| Case | Ranks / clients | Write MiB/s | Read MiB/s |
| --- | --- | ---: | ---: |
| easy, 8 GiB, 1 MiB transfers | 1 / 1 | 401.06 | 1514.13 |
| shared, 20,000 × 47,008-byte segments | 1 / 1 | 42.58 | 165.18 |
| easy, 2 GiB/rank, 1 MiB transfers | 56 / 7 | 354.76 | 22488 |
| shared, 1,900 × 47,008-byte segments/rank | 56 / 7 | 16.96 | 604.62 |

Every case returns zero with data checking. **No scale-out bandwidth fix is
established:** aggregate wide write rates are not better than the original
campaign. Read rates are not proven cold-device rates (no cache eviction).
The earlier `74df76244` IOR campaign is operational context, not a controlled
parent comparison: source changes and unrelated shared-host activity differ.

## Correctness and regression evidence

- Existing `test_data_commit` PASS: verifier changes, missing members,
  degraded-mark refusal, concurrent fsync, late acknowledgements, full-table
  sweep and fail-closed capacity handling.
- New 65,536-chunk test fails on the old algorithm at its deterministic work
  bound (SIGABRT, not a timing threshold). Fixed extended test: **291,264
  inspected records**, including forced identity-index and inode-bucket
  collisions, exact versions/resends, arbitrary removal, slot reuse and
  per-file proof isolation. Mac native + AddressSanitizer/UBSan PASS; Linux
  native extended test PASS. The final collision extension supplements the
  original regression in the full-gate snapshot.
- Real `test_k35_fsync_degraded.py` PASS on the fixed source, 67.9 s; both C
  durability tests PASS in the same gate.
- Main RDMA cluster: 1 GiB nonzero, per-MiB identified pattern written and
  fdatasynced on fcstor008; **every byte** verified on fcstor009 before and
  after a checked full cluster cold restart. PASS. This is not verification
  of every existing workload file or every byte in the zero-valued dd files.
- Mounted full POSIX on fcstor008, 16 jobs: **220/223 PASS, 0 FAIL, 3 SKIP**
  (mmap unsupported and two short query freshness budgets). This does not
  erase the earlier `74df76244` 28-instance stress failures or qualify that
  high-concurrency workload on the fixed build.

Full Linux gate: `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8
EFS_BUILD_JOBS=8 EFS_RDMA_DEV=mlx5_2 bash scripts/make_test_summary.sh`, in
an isolated git-archive tree, with private libfuse. **527 / 8 FAIL / 5 NOTRUN,
644 s, BUILD 35 s, 5 OVERBUDGET.** No timeout/budget adjustment or ignored
application error. The deployed binaries are never rebuilt by this gate.

| Failure | Disposition |
| --- | --- |
| A6 inventory missing new counting seam | Test-only macro now documented in the compile-time inventory; focused inventory gate and its negative controls PASS. No runtime option added. Original failure kept. |
| Four LeakSanitizer fatal runtime failures: read-index pair, host outbox, Raft ACK window, stage LRU | Same failures on the unchanged parent; no LSan acceptance claimed. |
| Two TSan link failures: session latch, apply ring | `/usr/lib64/libtsan.so.0.0.0` absent; same failures on parent. Native subchecks do not substitute for TSan. |
| B6 full quota: W0 succeeds, remaining writers ENOSPC, where oracle requires all ENOSPC | Original closed fixture retained. Two guarded parent and two guarded fixed-build full-quota repeats PASS. Attribution remains open; later passes never replace the original failure. |

NOTRUN: cluster lifecycle scripts absent on the build host, RDMA Memcheck
headers absent, fault-build-only stalled compactor, git-dependent graph
selection in the archive, sanitizer runtime gate. The original FAIL and
NOTRUN classifications stand; this is not a fully passing code gate.

An initial focused parent-comparison attempt used an incorrect shortened
backup path; a nonfatal `cd` let it run from the fixed private source tree.
Those comparisons are **INVALID**, and the diagnostic fixed follow-up was
also relabelled by that build. Their logs remain under `gate-followup/`,
with `INVALID-PARENT-COMPARISON.txt`. Two later missing-fault-binary attempts
were setup failures. None of these are acceptance results. The timed dd A/B
and original full gate are unaffected: they recorded their own exact source
identities before these follow-ups.

The replacement `gate-followup-guarded/{parent,fixed}` runs make directory
and revision checks fatal, verify `data_commit.c` against the exact git
revision's SHA-256 before and after the build, and rebuild normal/fault
binaries together. Six runtime failures reproduce on the true parent; both
parent full-quota runs and both fixed runs PASS. The original full-quota
failure remains **OPEN**. Reclaimed capacity between fill and small-write
probes is one hypothesis, not established cause; the oracle is unchanged.
The profiling coordinator now requires an expected full installed source
hash and refuses any mismatched target before capture; three focused tests
exercise that guard.

## Reproduce and retain

Prerequisites: normal ORCD-test SSH agent (no BatchMode), matching deployed
snapshot, Linux perf/sysstat and mounted RDMA export. Scripts committed here:

```sh
python3 scripts/orcd_dd_profile.py --label UNIQUE --seconds 120 \
  --source 3769f44bb2ada18aa6959a4dc66d110a661bd1de
make -j8 tests/test_data_commit tests/test_data_commit_index
./tests/test_data_commit
./tests/test_data_commit_index
# Before/after a checked cluster restart, on separate mounted clients:
python3 scripts/orcd_dd_payload.py prepare /tmp/efs-mount/UNIQUE.bin
python3 scripts/orcd_dd_payload.py verify  /tmp/efs-mount/UNIQUE.bin
# Same four IOR cases as the original campaign:
bash scripts/orcd_test_ior.sh NEW_OUTPUT /tmp/efs-mount
```

The sampler validates same-user PID and exact target mount/store arguments,
records executable hashes, samples all four daemons and the client, and signals
only its own profiler process groups. Profiling and all unprofiled measurements
are separate. A stop marker ends sampling when dd exits; hard time bounds
remain. Closed raw data stays on ORCD, never in Git or copied to the Mac:

`/home/erbmi1/orcd/scratch/efs/orcd-test/dd-profile-20261028/`

This contains original/fixed raw perf captures, binary identities, iostat,
pidstat, per-thread observations, dd logs/results, deployment records,
full-gate original failures, focused parent/fix results and reproduction
controllers where already retained; local operation scripts/logs also remain
at `/tmp/efs-orcd-deploy-20261028/` on the controller. The original failed
quota store remains at
`fcstor007:/tmp/efs-dd-regression-3769f44bb/tmp/case-464/efs-b6-9z7vya65`;
`open-quota/` retains its archive, per-file hashes and archive-byte verification.
It is kept for the OPEN quota incident. Exact profiled executables are copied
and byte-compared under `symbols/HOST/REVISION/`; source backups remain as
open performance-analysis dependencies. No fixtures or build trees were
pruned: automatic approval review rejected the broad cleanup/upload action,
so those remain temporarily alongside the evidence. No controller-directory
upload or remote deletion was performed by that rejected action. Final
readiness, all eleven installed identities and seven mounts are checked;
the fixed cluster stays running. The final manifest digest is recorded below. No raw run
artifact is committed. No production-mode or release-completion claim.


Evidence seal: `SHA256SUMS` covers 360 retained files; its SHA-256 is
`ae4e38833dc639d7dbad91c5253263e2d5da361f0194a94f98827fea028ba47b`.
`SHA256SUMS.checked` records successful verification of every listed file.
The quota archive was additionally opened and each contained regular file's
bytes checked against the original file's SHA-256 before the seal.
