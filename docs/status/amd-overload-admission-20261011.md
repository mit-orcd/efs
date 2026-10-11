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
