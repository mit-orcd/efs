# W94/W97 AMD unpaced overload and admission follow-up

Continue from [sustained acceptance](amd-sustained-20261011.md). The private
180-second direct reproduction uses the current 20-ms GC candidate, unchanged
4-GiB per-node quota, eight tiny workers and four unpaced large ring writers.
Installed daemon stores are retained; established mounts are borrowed with
automatic restoration. Two setup attempts failed before workload: missing
empty mount directories, then FUSE permission denial at a new mount path.
Neither is overload evidence.

Proposed experiment: opt-in data admission returns a distinct transient BUSY
for new charged files near 90% of logical quota when local GC has actually
deleted fragments within five seconds. Existing-generation retries/overwrites
remain eligible without a new charge; hard quota remains a terminal refusal.
Metadata, GC and read paths are not throttled. No wait occurs under storage
or GC locks. Client BUSY retries retain object identity, reuse healthy
connections, and remain bounded by the inherited request deadline.

This narrow logical-quota experiment is not W99 capacity-domain read-only
policy. Default admission remains unchanged. Require focused real-source
regressions, full native/fault gates, unpaced direct/buffered cold-byte and
physical-reclamation checks, bounded charged usage, and small-file progress
before selecting it. A new BUSY reply is safely rejected by older clients;
coordinated candidate clients are required for retry semantics. No D25/RDMA,
packing, consensus change or push. W94/W97 stay open pending actual verdicts.

## Candidate implementation and initial gates

The current 20-ms unpaced direct baseline reproduces ENOSPC at fsync, and
restores both established mounts (exit zero). This baseline still lacks its
own failed-run cold oracle; its workload refusal is not a data-loss claim.
`EFS_DATA_GC_ADMISSION=1` now enables the proposed 90%/recent-deletion experiment
for the ordinary files engine. Default is disabled; invalid values refuse
startup. Containers/packed-data pressure policy is outside this candidate.
A BUSY PUT is not a receipt or publication success. Metadata, GC, existing
fragments and actual quota rules are preserved. Retried generations and
placement remain fixed, connections remain reusable, and retries are limited
by the caller's existing deadline plus a 128-attempt ceiling.

Actual-store create/overwrite/QUOTA/BUSY boundary checks pass on AMD Linux.
Actual reply decoding, caller-bound quorum/drain, mixed negative replies,
BUSY recovery and expiry pass locally; full normal/fault native gates are
queued before live acceptance. The cold oracle now retains the known successful
individual tiny closes and large fsync prefixes even if another I/O is refused.
Unacknowledged fixture files are retired only after those bytes are verified.
The refusal list is independent of the harness cold/reclamation exit code:
exit zero with refusals is not a sustained-load pass.

Three initial cells use fresh 4-GiB stores: candidate code with admission off
(direct), admission on (direct), and admission on (buffered), unpaced for
180 seconds. Thirty-second GC/charged-usage samples and admission diagnostics
are retained. These independent salts/placements are functional comparisons,
not matched throughput A/B. Longer runs are conditional on actual verdicts.

## Initial control and direct-admission verdicts

Full normal/fault native suites pass. The exact production candidate is
committed as `d9db68e7` plus the final hard-failure cleanup preservation in
that commit; the recovery harness is `704b9700`, and expanded mixed-reply tests
are `fc826030`. Snapshot binaries carry the explicit experiment build label
`54e33e82-admission-normal`; selected source SHA256s and binary hashes are
retained, so this label is not misrepresented as the final source commit.

Admission-off control refuses four large-worker operations with ENOSPC after
122.21 seconds (requested 180), with 7,808 successful tiny closes. Cold recovery
verifies 512 retained acknowledged tiny files and 64 MiB of acknowledged large
fsync prefixes; all physical fragments drain in 65.13 seconds. Harness exit zero
means recovery/cleanup passed, while the retained refusal list means sustained
service failed.

Admission-on direct completes the requested 180 seconds without I/O refusals
(196.78 seconds including final generation settlement), with 5,312 successful
tiny closes. The same cold-byte checks pass, and physical reclamation drains
in 63.61 seconds. Diagnostics show BUSY admission near 3.865 GB (3.60 GiB)
(90% of the 4-GiB logical quota) on all four nodes; tiny completions continue.
This establishes the initial functional cell, not a throughput improvement or
a fixed physical-space bound under every persistence failure. Buffered also completes its 180-second request without refusals (202.18 seconds
including settlement), with 5,888 closes, the same cold-byte oracle and full
physical drain in 67.40 seconds. Both initial on-cells pass.

Conditional 600-second direct/buffered cells add per-worker tiny-close and
large-fsync progress samples, and a fresh acknowledged write plus another cold
restart after pressure cleanup. The installed service still uses the prior
normal binary and default-disabled admission. W94/W97 are not closed.

The conditional ten-minute unpaced gates are now running. All eight tiny and
four large writers have recorded progress in the initial direct samples;
final fairness, cold recovery and post-pressure write gates are not yet claimed.
The oracle corruption regression passes on AMD: retained refusal information
cannot waive corrupt acknowledged tiny bytes or large fsync-prefix bytes, and
unknown fixture files are not retired before all acknowledged checks pass.

[Reconciled cell summary](../../results/measure/20261011-amd-overload/summary.json)
keeps the sustained-load and cold/reclamation verdicts separate. The initial
control/direct-on/buffered-on sampled peak charged usage is 4.134/3.865/3.865
GB on the most occupied node. Thirty-second samples can miss the terminal
quota peak; the control refusal proves it reached hard quota. Sampled aggregate
four-daemon/FUSE CPU is 2.43/1.20/1.43 cores; workload-driver CPU is excluded.
These observed rates are not matched throughput/efficiency claims.
Full functional compatibility with admission enabled is queued only after both
long cells pass the no-refusal, last-period worker progress, cold/physical and
post-pressure cold-write checks.

## Ten-minute unpaced gates passed

| Mode, admission on | Successful tiny closes | Refused I/O | Physical drain | Fresh post-pressure cold write |
|---|---:|---:|---:|---|
| Direct | 16,320 | 0 | 58.09 s | Passed |
| Buffered | 16,384 | 0 | 53.98 s | Passed |

Both requests run for 600 seconds; final generation settlement extends wall
time to 618.05/616.99 seconds. Each cold oracle checks 512 retained tiny files
and 64 MiB of acknowledged large-file fsync prefixes. A subsequent fresh
32-KiB acknowledged write survives another cold restart, then fully reclaims.
All eight tiny workers and four large writers make progress over the final
observed interval, about 450→570 seconds: each tiny worker completes at least
335/333 closes in direct/buffered, and each large writer advances at least
1,804/1,912 fsynced 1-MiB writes. This is observed fairness for this workload,
not a starvation-free scheduling proof for arbitrary workloads.

Peak sampled node charge is 3,865,444,352/3,865,772,032 bytes, approximately
90% of the 4-GiB quota; no sample exceeds hard quota. These are 30-second
samples, not a universal instantaneous physical-allocation bound. Sampled
four-daemon/FUSE CPU is 1.07/1.13 cores and peak aggregate RSS 596/586 MiB.
Full-window tiny closes are 26.62/26.77 per second under unpaced large writes.
Admission controls offered work; these independent cases do not establish a
general throughput improvement.

The gated full direct functional suite passes with admission on: 216/216 POSIX,
67/67 two-client, zero skips/failures, plus guarded recovery, offline-member
retention, exact cold bytes and physical cleanup. Retained normal service
deployment is now authorized by those completed gates;
W94/W97 remain open globally for the integration and NUC persistence gates.
