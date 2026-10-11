# W94/W97 AMD lifecycle and replication-feedback acceptance

Status: in progress on `devel-amd`; normal AMD stores are retained. This starts
from admission closure `31337fee` and documentation scope split `70538988`.
No W111 NUC work, RDMA, D25 activation, production packing or pushes.

W94 already has measured combined CREATE adoption savings, sequenced lease
edges, cold/reply-loss/negative verdicts and sustained AMD admission acceptance.
Review remaining lifecycle gates against the current REPORT/session code;
full production flush/PUT-ticket adoption remains independently staged. Do not
close the whole work item by silently activating that staged data plane.

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
updates. Those files are synchronized and the corrected gate is rerunning;
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

Full direct feedback-enabled lifecycle/POSIX/peer, cold bytes and physical GC
passes. Buffered and the subsequent real fault/pressure matrix are running;
normal service feedback remains disabled pending the final verdicts.
