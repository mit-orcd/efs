# NUC tiny-file creation — seven iterations, Oct 8, 2026

[Open follow-ups W93–W96](tiny-file-performance-plan-20261008.md) record the
remaining Raft, CREATE/lease, tiny-data and KV WAL opportunities.

## Current state after round 7

NUC runs experimental build `c22754236b38-tiny-r7` on `devel-find-stats`,
based on `c22754236b38`. The performance changes are recorded in a separate tiny-file commit;
these runtime observations describe the historical round-7 deployment. `/Users/mike/git/efs` and its concurrent edits are untouched.
The generator is running in the existing `tiny_files` screen against the new
`/data1/efs/mnt/tiny-profile-r7-fix`. All earlier trees and stores are retained.

### Why the rate falls after startup

Separate profiles were read for the FUSE client and the actual metadata
leaders: n1 leads group 0 (voters n1/n2/n3), n2 leads group 2 (voters n2/n3/n4).
Group 0 can commit on n1+n2, both on `/data1` (XFS, enterprise SATA).
Group 2 must reach n3 or n4; both share `/home`'s Samsung 970 EVO NVMe via
`/data2`. This is a durability-latency asymmetry, not evidence of exhausted
sequential bandwidth. Loaded n3 traces showed Raft and KV WAL syncs averaging
about 12 ms, applied markers 6.4 ms, and fragment syncs 20 ms before this round.

The generator assigns an entire folder (up to 1,000 files) to each of eight
workers. LOCAL directory placement keeps those files in the parent's metadata
group. Fast folders finish and workers take another task; slow folders occupy
worker slots much longer. The sampled eight active parent inodes all mapped
to group 2. This explains the gradual concentration on the slower path.

An alternating controlled comparison paused the generator and used eight
workers, 32 files each, 256-byte payloads, and open/write/close without explicit
fsync. Same directory sets, same leaders and storage placement before/after:

| Metadata group | Before round 7 (files/s) | After round 7 (files/s) |
|---|---:|---:|
| 0, two SATA replicas can form quorum | 159.9, 175.2 | 147.2, 163.5 |
| 2, an NVMe replica is required | 18.7, 18.9 | 31.1, 31.8 |

This is an observed roughly 67% improvement on the slow-group comparison,
not a proven whole-workload or sustained 67% gain. These are short runs across
one deployment/restart, not a randomized A/B/A study; namespace/background
state can differ. Fast-group throughput did not improve.

### Change and measured limits

The new change reserves Raft log space in 1 MiB increments on Linux. Valid
record length and the durable watermark exclude the zero tail. Existing
fdatasync barriers still gate acknowledgement; replay removes unused/torn
space and resumes at the valid tail, and rotation preserves the distinction.
Unsupported or failed preallocation falls back to ordinary exact writes;
non-Linux and NOSYNC paths retain ordinary appends. At most approximately
1 MiB of extra tail space is reserved per node log. Diagnostic log readers
now recognize zero padding, and the Makefile tracks the shared struct header.

The preceding bounded XFS microbenchmark measured 1 KiB durable appends on
NVMe at 7.1–7.8 ms versus 3.9 ms with preallocation; initialized space was
3.2 ms but would add zero-writing overhead, so it was not chosen. This is
principle-level evidence, not an EFS throughput result.

Fresh generator: **2,931 displayed files / 30 s = 98 files/s**. The untraced
27–57 s interval added 2,048 displayed files (about **68 files/s**), with NVMe
92.9% busy. Progress can lag up to 504 completed files; random folder/group
mix makes startup rates unsuitable for ranking these changes. Later lifetime
rates include intentional profiling/test pauses. The 1,000–10,000 files/s
target is not met.

Post-change perf was captured separately for client, n1, n2, and n3. Client
sampled CPU is concentrated in close/flush and fragment transfer; n2/n3 show
Raft pump, network and XFS sync work. These are inclusive on-CPU percentages,
not fractions of wall-clock waiting. The earlier context-switch profile is
count-weighted, not a time-weighted off-CPU profile. The post-change n3 Raft
sync mean was 7.2 ms, but queue/workload conditions differ; fragment syncs
still averaged 22.4 ms. Generator strace still shows open 90.6 ms, close
77.8 ms and fstat 20.8 ms during its instrumented window. Durability and
replicated metadata waits remain significant.

### Validation and evidence

Raft-store (including new allocation-boundary, crash, torn-tail, rotation and
reopen cases), Raft core, KV-LSM, concurrent CREATE and diagnostic parser tests
pass. Private buffered/TCP raw query RPC, nine virtual-interface gates, full
POSIX **216 pass / 0 fail / 1 skip (16.0 s)**, peer **64/64 (10.4 s)**,
and restart/oracle checks pass (112 files / 153 directories / 4,212 bytes).
These validate the
current legacy query implementation as a regression gate; they do not claim
completion of the separate `.efs/find` / `.efs/stats` replacement work.
Process crash/reopen tests do not simulate physical power loss.

Round 7 raw profiles, traces, group comparisons, build hashes and test logs (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-nuc-tiny-create/round-7`).
The original pre-change group and log-space results are in the parent evidence
directory. Population was resumed automatically after private testing.

## Historical state after round 6

The user requested one more round. NUC now runs experimental build
`c22754236b38-tiny-r6` on `devel-find-stats`. Source changes remain uncommitted;
the main devel checkout is untouched.

Round 6 reuses the committed inode already returned by CREATE to construct
FUSE's reply for newly created files. It removes one redundant GETATTR RPC.
Attribute and entry TTLs remain zero, the reply is not inserted into the lookup
memo, and later `fstat` calls still fetch current attributes. Existing-file
`O_CREAT` handling and server open-reference acquisition/release stay intact.

The new tree `/data1/efs/mnt/tiny-profile-r6` reached **5,271 displayed files
in 30 seconds (176 files/s)**. An untraced interval from 71 to 101 seconds added
1,126 displayed files, or about **38 files/s**. Progress counters are batched;
these are exploratory measurements rather than a controlled speedup claim.
NVMe utilization remained 93.5%, with about 331 flushes/s. Sustained creation
remains far below 1,000–10,000 files/s.

Validation: concurrent create result/error tests, Raft-store and KV-LSM suites,
raw query RPC checks, nine virtual-interface gates, **POSIX 216 pass / 0 fail /
1 skip (16.1 s)**, **peer 64/64 (12.0 s)**, and persisted restart against an
independent namespace oracle. Full suites ran in private buffered/TCP stores;
no physical power-loss acceptance is claimed. The generator was temporarily
paused for those tests and has resumed in the existing `tiny_files` screen.
Its later lifetime rate includes that pause. All output trees are retained.

Round 6 profiles, patches, hashes and test results (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-nuc-tiny-create/round-6`).

## Historical five-round checkpoint

Five user-requested implementation/build/restart/30s measurement/profile rounds
completed on devel-find-stats based on c22754236b38. Four main NUC daemons,
two mounts, TCP/buffered I/O; existing stores and all generated trees retained.

30s screen rates: **164, 93, 186, 127, 146 files/s**. No sustained speedup or
1000–10000files/s acceptance is established. Random folder shape, growing
retained namespace, restart work and batched progress counts confound comparisons.
Each rate preceded strace/perf, which perturb throughput.

Baseline measured waits: open131ms,close86ms,fstat29ms,write0.18ms. NVMe backing
n3/n4 was95%busy at3.6MiB/s with233flushes/s. Sync stacks identify Raft
create/HOLD/REPORT, KV WAL and applied-index persistence. CPU headroom remains;
small payload bandwidth is not the measured wall. Aggregate sync counts also
include background work and must not be presented as a per-file ratio.

Final candidate retains KV/Raft WAL fdatasync (bytes and file-length durability)
and skips a Raft sync request only when its durable byte watermark covers the
whole tail. The100msapplied-marker interval and200usproposal-delay experiments
were reverted after no measured benefit. Commit/close durability and open-reference
lifetime were not removed. Candidate is uncommitted, explicitly tagged
c22754236b38-tiny-rfinal in deployed binaries; exact hashes and source patch retained.

Raft-store/KV-LSM tests passed before each deployment. Final private buffered/TCP
fixture passes raw query RPC/bounds, nine virtual gates, full POSIX/peer suites
and persisted restart against an independent namespace oracle. This does not
simulate physical power loss or establish loaded performance acceptance.
Generator is resumed in screen tiny_files into /data1/efs/mnt/tiny-profile-final.

Next: measure per-file CREATE/HOLD/stat/REPORT stage counts, latency and batching,
with controlled stable namespace/background conditions. Avoid claiming sync flavor
changes solve replicated metadata round trips. Preserve all durability/publication,
open-unlinked and permission guarantees. W92 replacement command work is separate.

[Raw evidence and iteration ledger](../../results/measure/20261008-nuc-tiny-create/README.md).
