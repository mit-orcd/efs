# ORCD-test second profile cycle: no validated new speedup — October 28, 2026

K32 write cost, K38 writeback ordering, and reliable qualification tooling.
**Copy optimization UNQUALIFIED and withdrawn; later slowdown remains OPEN.** Candidate
`8c20f501dac94348d72f19a980bce4eb090d9dc6` remains in branch history but is
withdrawn by `1f8c56e14f2a076c2b3aaaa46641558d7b618e0d`.
The first-round durability-index fix remains; retained product C/header sources
are byte-identical to `3769f44bb2ada18aa6959a4dc66d110a661bd1de`.
Harness fixes are committed as `2499242de`, with test-budget inventory correction
`ebac01ded`, on `agent/orcd-dd-profile-20261028`, not merged or pushed.
The retained build is restored and its live checks pass. The isolated full gate
is **FAIL: 529 commands, 3 FAIL, 5 NOTRUN**. No new throughput or release PASS.

[First-round evidence and unresolved failures](orcd-test-dd-profile-20261028.md)
remain valid independently. This second round does not erase them.

## Fresh profile and rejected change

A fresh parent capture records zero lost CPU samples on the client. Memmove
accounts for 48.90% of sampled user cycles, including approximately 12.83%
for snapshot extraction under reclaim and 9.71% for copying a completed
snapshot back into its retained dirty-cache body (`dcache_install_image`).
These are sampled CPU costs, not wall-clock waits.

The proposed shortcut retained the recovery snapshot and removed only the
return copy of a provably unchanged full overwrite. Callers supplied explicit
unmerged-full provenance; the installer also required a nonzero current
snapshot sequence, no newer installed image, no dirty bytes or partial ranges,
a full base and exact lengths. Merged images, replay and newer bytes kept the
original install/overlay path. No durability, publication, allocator, pin,
wire/store format or configuration change was intended.

The candidate's install-copy frame fell below the report's 1% threshold.
Approximate captured client cycles fell from 79.5 billion to 70.0 billion,
while total memmove share remained about 49%. That CPU observation did **not**
predict better end-to-end performance. Its cause is not resolved: changed
pacing, cache locality or batching are hypotheses, not demonstrated causes.
The copy stays in the retained product; no artificial delay or weaker
correctness check was substituted to obtain a PASS.

## Timing and control

Four storage nodes fcstor003–006, six existing NVMe roots each; seven clients
fcstor007–013. Native RDMA mlx5_2, O_DIRECT server fragment I/O, buffered FUSE
writes. Automatic safe-headroom quota (no `--quota` override; 20% reserve,
minimum 1 GiB per filesystem), no quota-policy change or reformat. All nodes use the same build within a phase;
other clusters are untouched. All timings below are unprofiled, single dd on
fcstor007, a fresh 8 GiB zero file, `bs=1M count=8192 conv=fdatasync`.
Regression workloads never run concurrently with these measurements.

| Phase | MiB/s per timed run | Median MiB/s |
| --- | --- | ---: |
| Initial parent `3769f44bb` | 342.04, 331.06, 332.56 | 332.56 |
| Initial candidate `8c20f501d` | 221.73, 270.20, 218.10 | 221.73 |
| Restart + 8 GiB warm-up, parent | 311.34, 315.55, 292.73 | 311.34 |
| Same-leader placement + 8 GiB warm-up, candidate | 208.52, 207.71, 200.33 | 207.71 |
| Restored parent, same placement + warm-up | 210.40, 179.04, 197.21 | 197.21 |

The latter comparison uses group 0 on node 2 and group 2 on node 4. Parent
readiness records that placement; candidate originally elected nodes 1 and 2,
so explicit diagnostic `raft-step-down NODE GROUP TARGET` transfers installed
the parent's placement, followed by stable-leader/root-read readiness and an
identical 8 GiB warm-up. This changes no election/default policy. All initial
results remain, including the slower candidate runs. The controlled candidate
median is **33.3% lower in that pair**, but the restored parent subsequently
falls to 197.21 MiB/s as well. **That reversal invalidates attribution of the
slowdown to copy elision.** The early comparison was insufficiently isolated
from evolving store/load/scheduling state. The cause remains unproven; the
candidate stays withdrawn because no stable throughput benefit is established.
The original first-round index speedup describes its measured phase, not a
promise that later runs sustain that rate. The 1 GB/s target remains unmet.

The first control attempt was interrupted during client mount setup by an
SSH login connection reset. No candidate timings ran in that attempt. The
attempted multiplexed connection also refused channels; ordinary SSH worked.
The remaining mounts were restored before measurements resumed in a new
evidence directory. Those setup failures are retained, not relabelled PASS.
The restored-parent reversal is included above. Additional source-only login
controller preflights exposed Python 3.6's older subprocess API and a reused
archive hash differing at packaging time; both stopped before services were
touched. Successful orchestration uses the ordinary working SSH path, one
login session and a content-addressed committed-source archive. Each failed
setup has its own retained controller/log directory. No failing data case was
rerun in place or relabelled PASS.

## Correctness checks and harness defects

The candidate passed native and ASan/UBSan production-function ordering tests
on the Mac, and Linux isolated build/focused tests: K38 install ordering,
client memory/recovery ownership, writer-fence merge, stale replay, fold
observations and both durability-index tests. Its temporary copied-byte bound
fails (SIGABRT) on the original implementation; this proves the proposed
copy-elision effect, not a correctness defect in that original code. Candidate
tests were reverted with the candidate; the original ordering tests remain.

Candidate mounted POSIX: **220/223 PASS, 0 FAIL, 3 SKIP**, 22.1 s. A 1 GiB
nonzero, per-MiB identified pattern was written/fdatasynced on fcstor008 and
verified **every byte** on fcstor009 before and after an orderly full cold
restart, and after the control restarts/transfers: PASS. This covers that
payload, not every existing file or power loss.

Two independently useful harness fixes stay (`2499242de`):

- `orcd_test_ior.sh` previously printed FAIL but returned zero after failed
  cases. It now returns nonzero if any case fails, retaining all four case
  logs/results. A synthetic MPI test verifies both all-pass and two failed
  launch cases, and is wired into `make test-long`.
- `orcd_test_ready.py` now distinguishes explicitly retired retained replicas
  from current voters. It still requires every current voter, agreed terms
  and membership, stable leaders, applied recovery floors and a fresh root
  read. Tests admit a stale retired nonvoter but refuse a retired/missing
  current voter; parser coverage includes both retired values. This commits
  the readiness correction used during the first cycle rather than depending
  on an unavailable private helper.

Initial candidate IOR single-client cases PASS: easy write/read
365.29/1476.26 MiB/s, hard 34.25/154.35 MiB/s. Both wide launches return **255**:
the compute-hop SSH did not forward the agent, so MPI authentication failed
before the data workload. The old runner's zero exit never qualifies them.
They remain failed attempts. The rerun uses agent forwarding on both hops,
preflights SSH to all six peer clients, uses the committed repaired runner in
a separate evidence directory, and propagates any remaining error. Its results
are recorded below; full-gate limitations follow.

| Forwarded IOR case | Candidate write/read MiB/s | Restored parent write/read MiB/s |
| --- | ---: | ---: |
| little easy | 235.24 / 1586.90 | 266.75 / 1518.29 |
| little hard | 38.06 / 191.12 | 31.05 / 162.53 |
| wide easy, 56 ranks / 7 clients | 261.04 / 25507 | 300.21 / 22216 |
| wide hard, same 56 ranks | 18.57 / 649.88 | 16.71 / 698.45 |

All four cases return zero with data checking in each forwarded run. Read
rates are not cold-device rates. The restored run includes a bounded,
read-only resource-monitoring slice, not a controlled uninstrumented
performance A/B. These numbers establish no new scale-out improvement.

Restored mounted POSIX: **222/223 PASS, 0 FAIL, 1 SKIP** (mmap), 13.2 s,
using the same `--suite all --jobs 16` and unchanged test budgets. The
candidate-created nonzero payload also matches every byte from the peer
after restoring/restarting the parent. Original 28-instance stress/W121
failures remain outside this rerun's qualification; no full parallel-stress
PASS is claimed.

## Final isolated code gate

On fcstor007, committed source `1f8c56e14f2a076c2b3aaaa46641558d7b618e0d`,
private libfuse prefix, RDMA device mlx5_2:

```sh
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 \
EFS_RDMA_DEV=mlx5_2 bash scripts/make_test_summary.sh
```

**529 commands, 3 FAIL, 5 NOTRUN, 760 s, 5 OVERBUDGET; build 35 s.**
The three failures are two TSan link failures (missing
`/usr/lib64/libtsan.so.0.0.0`, session latch and apply-ring tests) and the
make-test inventory check: the two newly wired ORCD tests lacked budget rows.
That omission is ours, corrected in `ebac01ded` with the existing 5-second
minimum and one slot each; no existing budget was raised. Fresh native Linux
follow-up passed the recipe, profiling guard, IOR exit-status, readiness and
environment-inventory tests. Local focused checks also pass. The original
full-gate result remains FAIL; it was not rerun or relabelled after the fix.

The five NOTRUN cases need absent cluster stop/remount scripts, Memcheck
headers, a fault-enabled compactor build, a Git checkout for graph-scope
selection, and a working real TSan toolchain, respectively. Over-budget tests
are K43 (53.1/25 s), Raft ack window (11.0/10), and D39 shard tests
(57.7/25, 32.1/15, 10.7/5); these are recorded, not relaxed.
The four earlier LSan cases and B6 quota check pass in this run. Their earlier
failures stay historical evidence; this does not resolve the original quota
incident or establish that the sanitizer failures are permanent host limits.

## Remaining costs and limits

A 45-second resource slice during retained IOR (`retained-ior-resources2/`)
records about 0.49–0.99 CPU cores per EFS daemon on average, maximum observed
individual-thread samples 54–60%, and client007 mostly idle in that slice.
This is a specific workload interval, not a peak-capacity measurement or
proof that no critical thread/other client can limit throughput. Storage
process affinity is CPUs 0–23; client007's main affinity is 0–11. Leaf
`cpu.max` was unavailable, so ancestor CPU limits were not ruled out.

Physical XFS space use is 3–8%, inode use 1–3%. First data NVMes peak at
12–82% utilization; the other data devices peak below 9% in this slice.
Maximum observed write-await is 3.76 ms. Raft watermark admission-refusal
and deferred-apply counts remain zero in the two captured status snapshots;
KV compaction counters advance. Metadata defaults to the first storage root
(`efsd.c`/`raft_host.c`), so this skew and the small-operation consensus path
are worth investigating. Device counters include other activity; this does
not prove a WAL bottleneck or validate device health.

The first raw CPU profiles still identify recovery snapshot copies,
transport-frame copies and RPC waits. Those copies carry ownership/lifetime
requirements; removing a sampled hot function is not proof of improving the
critical path. The existing [K32 cost report](k32-write-cost-profile-20261028.md)
measures 5.657 ticket-lifecycle Raft entries/MiB. Current admission cohorts
count jobs only after workers enter, so accepted but not-yet-running jobs
can miss coalescing. Measuring cohort fill, successful single fallbacks,
settlement/publication waits and metadata sync latency is the next focused
investigation. This is a code-based lead, **not a proven cause** of the later
slowdown, and no batching or durability change is shipped here.

The first resource-collector template attempt was syntactically invalid and
sampled nothing; it is retained separately. The corrected collector's child
code was compile-checked before execution and all five collectors exited
zero. A later rank-progress snapshot found no active IOR ranks; it supplies
no straggler attribution.

## Reproduction and retention

Use the committed sampler and payload oracle, with Linux perf/sysstat, the
same private libfuse/MPI/IOR prerequisites and an SSH agent (no BatchMode):

```sh
python3 scripts/orcd_dd_profile.py --label UNIQUE --seconds 120 \
  --source FULL_INSTALLED_COMMIT
dd if=/dev/zero of=UNIQUE_FILE bs=1M count=8192 conv=fdatasync status=progress
python3 tests/test_k38_install_order.py
python3 tests/test_client_memory.py
python3 tests/test_writer_fence_merge.py
python3 tests/test_k38_stale_span_replay.py
python3 tests/test_fold_observation.py
python3 tests/test_orcd_ready.py
python3 tests/test_orcd_ior_exit.py
python3 scripts/orcd_dd_payload.py prepare /tmp/efs-mount/UNIQUE.bin
# Different client, before and after a checked restart:
python3 scripts/orcd_dd_payload.py verify /tmp/efs-mount/UNIQUE.bin
bash scripts/orcd_test_ior.sh UNIQUE_OUTPUT /tmp/efs-mount
```

For a timing comparison, use the same committed deployment on all nodes in
each phase, checked stops/readiness, identical 8 GiB warm-up and three timed
runs, with the recorded leader placement and no competing regression suite.
Keep warm-up rows separate. The candidate can be reconstructed from its
explicit historical commit; there is no need to reintroduce it into devel.

Raw evidence stays on ORCD under
`/home/erbmi1/orcd/scratch/efs/orcd-test/dd-profile-20261028/`:
`round2-parent-3769/`, `round2-fixed-8c20f501d/`, and `round2/` (preflight,
initial dd, failed/control/resumed attempts, restored build and IOR suites).
Local controller scripts/logs remain `/tmp/efs-orcd-deploy-20261028/`; selected
closed text records and exact symbol dependencies are retained with the raw
captures at the final seal. No raw artifacts or payloads go into Git or to
the Mac. All first-round failures remain retained. Prune only explicitly
passed payloads/stopped scratch after hashing, using the user's scoped
cleanup approval; retain failed fixtures and rejected-candidate dependencies.
Final process-executable hashes match installed `3769f44bb` binaries on all
four servers and seven clients, all seven mounts are present, and fresh
stable-leader/applied-root readiness passes. The cluster is left running.
The final follow-up's five focused tests pass at `ebac01ded`; its product
C/header diff against `3769f44bb` is empty.

Exact captured product ELFs are retained under `round2/final-symbols/` with
hash checks, plus available matching-build-ID libraries. There are 86
unavailable dependency records across hosts/phases (compressed kernel modules
and libc build-ID mismatches); the lists are retained. Existing text reports
remain evidence, but complete independent symbol replay is not claimed.

Approved cleanup removed only the stopped passed candidate-preflight and
budget-follow-up trees on fcstor007 and passed POSIX fixture directories on
fcstor008. `ROUND2-PRUNED.txt` records exact paths; all second-round dd
fixtures, current stores, withdrawn-candidate dependencies, failed fixtures
and final failed-gate tree remain for the OPEN investigations, listed in
`ROUND2-KEEP-OPEN.txt`. No raw profiles/logs were deleted. The final evidence
writer initially stopped after cleanup because login Python 3.6 lacks
`time_ns`; its failure is retained and a seal-only continuation completed
without repeating tests, cleanup or deployment. Closed finalization scripts
and its failure log are separately retained at
`/home/erbmi1/orcd/scratch/efs/orcd-test/dd-profile-finalization-20261028/`;
its verified `SHA256SUMS` digest is
`c3b4a22c35acf3f3486ff96d505383146327541abdb868e51f9b77f9250fc376`.

The closed controller records, source proof, health and focused results are
under `round2/final-followup-ebac01ded/`. Final root manifest `ROUND2.sha256`
is verified by `ROUND2.checked`; 1,050 retained files; manifest SHA-256:
`14cdb70f4b2cc0358cfe91ef73d525775094228387d52c0cc2a6a75f18658e58`.
