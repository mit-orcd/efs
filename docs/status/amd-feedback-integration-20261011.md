# W94/W97 AMD lifecycle and replication-feedback acceptance

Status: W94 CREATE/adoption closed; W97 acceptance in progress on `devel-amd`.
Normal AMD stores are retained. This starts
from admission closure `31337fee` and documentation scope split `70538988`.
No W111 NUC work, RDMA, D25 activation, production packing or pushes.

W94 CREATE/adoption closes with measured savings, sequenced lease edges,
cold/reply-loss/negative verdicts and sustained AMD admission acceptance,
plus current REPORT/session and allocation/concurrency regressions. The user
explicitly retains full production flush/PUT-ticket integration under
W72/W86/W88; that staged data plane remains inactive.

W97's receiver-feedback candidate is opt-in (`EFS_RAFT_FLOW=1`). Exercise the
actual Linux async helpers and full direct/buffered TCP lifecycle/POSIX/peer,
cold acknowledged bytes, APPEND/sync and physical GC gates on the current exact
artifacts before any performance trial. Early retirement and group-drain
experiments remain disabled. Existing AMD admission/20-ms GC settings are held
constant. Private fixtures borrow established mounts with restoration; the
normal four-daemon service is not replaced by this trial.

Next: bounded pressure/recovery and repeated alternating feedback-off/on
comparisons, including worker progress, operation tails, physical sends,
admitted/BUSY feedback, detached work, catch-up, cold bytes and CPU/RSS. A
missing pressure event is not feedback acceptance; fewer refused frames alone
is not performance acceptance. Retain failures and distinguish private-cell
results from normal service settings. W97 remains open until actual verdicts.

## Reproduced W97 slow-lane starvation

The production sender selection reserves the only slot for due data before
critical frames and probes. If each round trip exceeds the pacing interval,
every selection is due and replies/votes/fresh-read probes can starve. Even
wider lanes can indefinitely fill control slots with critical traffic ahead
of probes. The original-helper regression fails; a bounded rotating selection
across critical, group-0 probe, group-2 probe and ordinary-data classes passes
locally. Entry-lane and critical-frame FIFO, probe identity and transport-only
feedback semantics are preserved. Linux async/full native and private two-mode
gates on the corrected build are next; no normal service promotion yet.

The first live invocation refused setup because the harness requires two root
arguments; no workload ran. Its exit/log is retained separately.

## Focused lifecycle and pressure instrumentation

A new actual FUSE-bookkeeping allocation regression confirms local ENOMEM
allocates no OPEN/adoption edge and transfers no pending CREATE token. It
passes locally and on AMD; the original caller retains exact cleanup ownership.
This is a regression addition, not a newly found lease bug.

The private pressure fixture selects an actual nonleader member, then adds
100 ms before its real Raft log fsync/fdatasync during a nominal 30-second
window. It always performs the real barrier and preserves errors. The Linux
interposer gate passes selected-path/marker handling, real bytes, EBADF and
healing. After the delay is removed, every hosted peer must catch up to frozen
leader commit cuts for both groups before cold restart. Stage snapshots retain
actual elapsed times and chosen member; no hardware/device conclusion follows.

Queued acceptance is serialized: full direct/buffered lifecycle suites, then
real combined-adoption reply-loss/negative and certificate-persistence faults
in both modes, followed by three alternating direct feedback-off/on pairs,
a buffered enabled pressure cell and an observation-disabled direct cell.
Each pressure cell retains per-worker progress, cold bytes, physical cleanup
and a fresh post-pressure cold write. Independent roots/placements remain a
performance confounder; actual receiver pressure must be present before any
feedback acceptance claim.

The first fault-native invocation passed its code tests but failed documentation
checks because the source snapshot omitted the new status leaf/generated
updates. Those files were synchronized and the corrected gate passed;
the original failure log is retained, not counted as a pass.

## Corrected native and deterministic race gates passed

Corrected full fault-native and normal-native gates pass on AMD. The same
slow-lane fairness regression fails specifically at its starvation assertion
against the original production source on AMD Linux, and passes the corrected
source including Linux async transport coverage. Assertions retain critical
FIFO and bounded per-class progress without imposing a fixed inter-class order.

A deterministic CREATE-adoption/competing-OPEN barrier test holds the combined
reply while another opener reaches the actual inode-stripe wait. That opener
cannot acquire a second lease or descriptor before adoption completes. Both
then own descriptors under one lease; the first close retains ownership and
the last close releases it once. The actual helper test passes locally and on
AMD; it is not a live network-race or distributed-failover claim.

Full direct and buffered feedback-enabled lifecycle/POSIX/peer, cold bytes,
physical GC and subsequent real fault matrices pass. Pressure comparisons
are running; normal service feedback remains disabled pending final verdicts.

## W94 — CREATE/adoption closed by user-scoped decision

The user explicitly closes W94 for CREATE/adoption and retains broader
flush/PUT-ticket integration under W72/W86/W88. No D25 activation, default
ticketed PUT adoption or full I23 publication claim follows from this split.

Full corrected normal/fault native passes; both TCP storage modes pass
216/216 POSIX and 67/67 peer tests, with zero failures/skips. Both real fault
matrices pass combined adoption reply loss and negative verdicts, original
token cleanup, cold name reuse, identical CREATE/APPEND retries, guarded
REPORT/session fences, certificate-persistence refusal/resume, sync/PUT faults
and physical GC. Actual-helper allocation and competing-OPEN tests pass.
The [previous throughput and applied lease proof](amd-non-rdma-20261010.md#w94--combined-first-descriptor-adoption-candidate)
and [unpaced sustained/cold/physical acceptance](amd-overload-admission-20261011.md)
remain the measured performance and sustained-service evidence. This closure
does not turn the synthetic W97 slow-follower trials into a new W94 A/B.

[Current full functional logs](../../results/measure/20261011-amd-feedback/evidence/functional/direct/run.log),
[buffered functional logs](../../results/measure/20261011-amd-feedback/evidence/functional/buffered/run.log)
and both-mode fault results are retained under the same evidence root. Native
loopback/software-crash tests do not certify independent hardware failures.

W97 remains open while feedback comparisons finish. The first feedback-off
pilot reaches 256 queued frames and 75,556 refusals, then passes both-group
frozen-cut catch-up, cold bytes, 53.43-s physical drain and a new post-pressure
cold write. Its slow window was extended by monitoring latency, so the pilot
pair is excluded from paired performance acceptance. Fixed exposure timing and
a replacement off/on pair provide three comparable direct pairs (2, 3, 4).
The first enabled observation exercises actual BUSY/probe feedback, with fewer
refusals; no sustained improvement or global W97 closure is claimed yet.

## Completed comparisons retained by release integration

All pressure cells and both normal-mount restoration markers exit0. The pilot
pair1 remains excluded; replacement direct pairs2/3/4 all verify progress by
every tiny/large worker, actual delayed follower recovery, cold acknowledged
bytes, physical empty inventory and a fresh post-pressure cold write. These
results describe the previous AMD feedback build, not the merged release
candidate. [Compact raw JSON, identities and derived comparison](../../results/measure/20261011-amd-feedback-final/comparison-summary.json)
retain exact source-tree and binary hashes per fixture.

| Direct TCP, median of three comparable cells | Feedback off | Feedback on |
| --- | ---: | ---: |
| Tiny closed files/s (actual complete load wall time) | 30.97 | 34.48 |
| Concurrent large writes MiB/s (same wall time) | 95.23 | 103.32 |
| Maximum 30-second-window close p99, ms | 1864.06 | 1758.23 |
| Physical GC drain, seconds | 65.92 | 72.25 |
| Sampled aggregate CPU cores | 1.65 | 1.86 |
| Sampled peak aggregate RSS, MiB | 355.28 | 377.86 |

The median throughput gains are 11.3% tiny and 8.5% large, but pair4 is weaker
and its GC drain worsens. Window p99 values are not a pooled latency percentile.
CPU/RSS samples cover about150seconds, not the complete load. Independent
placements/salts and a synthetic delay on one physical host limit causal and
hardware conclusions. Buffered enabled and unobserved enabled controls pass
independently; they do not establish a buffered paired speedup. Receiver
feedback remains opt-in and global W97 is not closed by these comparisons.
