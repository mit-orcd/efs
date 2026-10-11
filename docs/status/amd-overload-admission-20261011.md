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
