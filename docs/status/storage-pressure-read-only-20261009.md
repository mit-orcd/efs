# W99 — proactive storage pressure and read-only admission

Registered Oct 9, 2026. User-requested policy; implementation and threshold
selection remain open. This note does not change running cluster behavior.

The user's preceding request is to enter read-only before nodes run out of
capacity, and make the cluster read-only when too many nodes cannot accept
writes. The follow-up applies this requirement to **every EFS component that
writes to disk**, including data storage, metadata/Raft/KV persistence and the
local derived `.efs` cache. In that context, the requirement is proactive
handling rather than waiting for an ENOSPC failure. Preserve this intent when
selecting thresholds; do not treat the cache as exempt from pressure control.

Production storage layouts must support:

| Layout | Capacity domains |
| --- | --- |
| Data, metadata and cache separate | Three independent disks/filesystems |
| Cache and metadata share storage | Their combined writes consume one domain; data is separate |
| Data and metadata share storage | Their combined writes consume one domain; cache is separate |
| All share storage | All writers consume the same domain |

Include multiple data targets per node. Identify actual backing capacity domains,
not just configured path names: different paths can share a filesystem. Track
free bytes, free inodes, quotas and component budgets together; account for
concurrent writers sharing a domain. Logical component limits alone cannot
protect filesystem capacity or reserve room for other writers.

Before exhausting a domain, stop admitting writes that would consume its
protected reserve. Keep capacity for already accepted durable work, Raft/KV
progress, recovery and cleanup. Read-only is an admission state, not an OS
read-only remount that prevents safe cleanup. Continue reads and permit
space-releasing operations only where they can still be committed safely.
Do not acknowledge writes that cannot meet durability requirements.

The cluster-level state must reflect writable placement and metadata quorum
requirements rather than a fixed count of read-only nodes. Different layouts
and target failures can remove different capabilities. A cache-only domain under
pressure must stop cache writes proactively and report query freshness/errors;
whether it also blocks authoritative mutations is an explicit pending policy
choice, not an implemented consequence. Shared cache/metadata storage must
protect metadata progress from cache allocation.

Define visible component, target, node and cluster pressure states with reasons,
thresholds and recovery hysteresis. Avoid silently serving stale query results
as fresh. Define restart behavior, bounded retention/reclamation and how operators
recover capacity. No numeric thresholds or automatic cache-to-cluster escalation
are selected by this note.

Acceptance requires constrained-capacity tests for each layout above, including
shared concurrent writers, byte and inode exhaustion, quota refusal, pending
accepted work, cleanup, cold restart and threshold recovery. Verify acknowledged
bytes and metadata, truthful query freshness, no duplicate effects, and safe
cluster admission when writable placement or metadata quorum is unavailable.

W92 owns cache implementation and its budgets. W99 coordinates pressure policy
across disk writers; consensus/routing changes require coordination with W66.

[Work item](../backlog/work-items.md#w99) · [Status index](README.md)
