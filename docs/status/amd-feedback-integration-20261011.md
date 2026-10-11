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
