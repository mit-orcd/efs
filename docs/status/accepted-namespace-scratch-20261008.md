# Accepted namespace, scratch and command policies — Oct 8, 2026

The user accepted these recommendations after discussing W84's transaction
limit and W91's command implementation. These decisions supersede the open
policy questions below; they do not claim implementation, rollout or gate
closure. [Decision register](decisions.md), [current work queue](README.md).

## D32 — W84 bounded namespace transactions

Accept the existing **144-participant transaction envelope** as the current
product limit. This is not a directory-depth limit: distinct directory lanes,
ancestry guards and fixed namespace participants share the envelope. Its sizing
rationale is 64 directory lanes + 64 ancestry participants + 16 fixed/headroom
participants, not a proof that every valid POSIX operation fits. Do not promise
universal depth 143 or discard ancestry checks to admit an operation.

Operations beyond capacity must fail promptly and atomically, leave the
namespace unchanged and expose an explanatory diagnostic distinguishing the
capacity refusal from transient contention or missing entries. Final errno and
operator-visible diagnostics need a consistent implementation contract and tests.
Supporting larger operations is a separate protocol enhancement, not an
unapproved increase of fixed arrays or the one-byte participant encoding.

**Gates:** exact envelope and overflow boundaries, different lane/ancestry
combinations, no namespace mutation on refusal, cycle detection, prompt failure,
concurrent renames and PREPARE/COMMIT coordinator death with cold recovery.
Existing AMD maximum-envelope evidence remains valid for its tested build;
verify remaining diagnostics/refusal behavior before closing W84. Historical
statements that the finite-limit policy is undecided are superseded here.

## D33 — W90 opt-in fixed scratch expiry

Expiry is disabled by default. Operators designate scratch roots and configure
default TTL and maximum retention; **no universal TTL is selected**. Seven days
is an example only. A new file receives a materialized deadline from its
containing directory's effective default. Default changes affect future files;
reads, writes and rename do not renew or replace an existing deadline.

Owners may explicitly renew within operator limits; permanent retention or
clearing a deadline requires policy permission. Check current ownership and
policy authoritatively. Initially support singly linked regular files only:
refuse enabling expiry on multiply linked files and refuse new hard links to
expiring files. Defer symlink expiry and recursive/nonempty-directory expiry.
Do not repurpose POSIX timestamps.

Use durable FileID/revision-bound metadata and a deadline index, bounded
conditional namespace deletion and ordinary GC. A deadline is eligibility,
not a precise disappearance/reclamation guarantee. Open descriptors retain
ordinary open-unlinked lifetime. Renewal committed before deletion invalidates
that deletion; renewal after removal must report the actual result, not revive
a reused path. Pause destructive expiry when clock health is uncertain;
replicated apply follows committed decisions, not independent replica clocks.

Age never grants authority to delete unpublished fragments. D28/D31 and
W72/W86 publication/session/storage fences still apply. No expiry activation
before auditing concurrent publication, namespace and GC ownership.

**Gates:** inheritance and policy authorization, renewal/deletion races,
rename/path reuse, hard-link refusal, open-unlinked access, clock uncertainty
and jumps, restart/leader change, concurrent publication and offline members.
Prove physical reclamation separately from namespace deletion and bound worker
work/memory. Existing POSIX/peer suites remain required.

## D34 — W91 directory-local .efs commands

`.efs/` is an opt-in synthetic read-only command directory per export, exposing
`help`, `ls`, `find` and `stats`, scoped to the containing directory regardless
of working directory. **Commands** is the user-facing term. Initially implement
them as readable executable scripts requiring Python 3 on supported Linux/macOS
clients. The OS executes command bytes and EFS processes query requests;
`cat .efs/ls` displays source and never executes the query.

Use a separate versioned, bounded query-path protocol for parameters and
immutable results; exact path encoding remains implementation design, subject
to argument safety and resource/permission gates. Queries use server-owned
indexes/rollups, not client tree downloads or read-triggered whole-tree rebuilds.
Expose freshness and completeness explicitly; offer JSON and NUL-safe output,
escape terminal control characters and preserve accurate errors. No full-tree
atomic snapshot promise is introduced by per-open immutable result ownership.

Real existing `.efs` entries take precedence and collisions are reported;
feature enablement cannot overwrite or hide user data. The Oct 8 user revision
replaces synthetic `.find` and `.stats` with `.efs/find` and `.efs/stats`; legacy
compatibility is not required and real legacy-named user entries remain valid.
Exclude synthetic entries from user traversal,
counts and expiry. Enforce caller permissions, current authorization and safe
cache isolation throughout. Executable mode grants no additional visibility.
`noexec` prevents direct invocation; document explicit interpreter invocation
where supported. NFS/gateway support is advertised only after credential,
path-resolution and invocation gates pass. No installed EFS utility is required
for accepted supported command use; an interpreter is a stated prerequisite.

**Gates:** source reads versus execution, arguments and unusual names, different
working directories, symlink invocation, noexec, collisions/feature transitions,
unprivileged users and permission changes, immutable multi-read/seek results,
freshness, explicit partial/failure outcomes and bounded memory/RPC/runtime.
Run existing virtual-interface and POSIX/peer gates; expiry-specific output
requires D33 implementation. Existing virtual-read correctness work comes first.

## D35 — W81 sustained-contention directory spreading

Retain the existing size trigger and add a directory-local sustained-contention
trigger: serialization wait and queued operations across several observation
windows, with enough activity to justify migration. Request rate alone or
cluster-wide storage/compaction pressure must not trigger directory spreading.
Use hysteresis/cooldowns, bounded migration concurrency and fair progress for
unrelated directories, reusing committed layout/migration machinery.

Measure AMD workloads before selecting explicit configurable numeric thresholds,
window lengths, cooldowns and concurrency limits. Those values remain measured
implementation tuning, not evidence that the policy is deployed. Coordinate
with W66 metadata scaling; do not promise more hosts/shard leadership than the
actual placement supports. POSIX paths and operation semantics remain unchanged.

**Gates:** hot directories below the size threshold spread under sustained
contention; cold/small directories and globally overloaded workloads do not
churn. Validate bounded migration, correct lookup/create/unlink/readdir/rename,
restart/leader change, fair progress, rates/tails and RSS on a named build.

## Implementation order and status

W84 limit/refusal contract first; existing virtual-interface correctness, then
W91 commands, W90 expiry, and W81 alongside metadata-scaling measurements.
This ordering does not cancel the authorized replay/readiness package or its
dependency gates. All four designs are accepted; code and acceptance remain
separate, and this documentation change closes no implementation package.
