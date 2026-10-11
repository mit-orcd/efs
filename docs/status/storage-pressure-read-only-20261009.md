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

## Metadata preservation and node-loss clarification

User requirement: under no circumstances may the cluster lose authoritative
metadata. Admission under storage pressure must fail safely rather than discard
metadata, acknowledge an unpersisted mutation or override lost quorum. This is
a required safety objective, not a claim of survival after every possible disk
or correlated node failure; the supported failure envelope must be explicit.

The user expected three nodes to use 2+1 and four nodes to use 2+2, with any
one/two node losses respectively survivable. Current code does not implement
that four-node guarantee: data remains 2+1, and the default metadata groups
have three voters each (nodes 1/2/3 and 2/3/4). Losing nodes 2 and 3 leaves
neither metadata group with its required majority. A four-node data 2+2 code
alone would not solve metadata consensus availability.

D36 supersedes the former architecture ladder: four nodes must provide
maximum two-loss protection, and larger clusters add scale. Ordinary majority
Raft needs five voters for automatic authority after two losses; the required
four-node metadata protocol remains unresolved. Current group_voters() still
selects three voters for clusters larger than three. See
[the binding decision](node-count-protection-20261009.md).
Distinguish temporary quorum unavailability from permanent loss of committed
metadata. No unsafe quorum override or automatic fresh metadata initialization
is acceptable as a pressure-recovery strategy. Independent backups remain a
separate protection against failure beyond the configured envelope.

Follow-up: audit actual durable metadata acknowledgements, placement, membership
and recovery against the declared failure target, and test every allowed node
loss combination. Do not advertise any-two-of-four survival. Coordinate this
consensus requirement with W66 and the architecture rather than changing voter
sets as part of W92 cache work.

## Oct 11: startup quota admission

`efsd` now derives a maximum startup fragment-payload quota from each backing
filesystem. It reserves 20% of total filesystem bytes, with a 1 GiB minimum,
then budgets 7/8 of the remaining available bytes for new payloads. Existing
inventoried payload bytes are added back, capped at 7/8 of total bytes minus
the reserve. External occupancy therefore reduces the allowance. Storage roots
sharing a filesystem are grouped before calculating this budget; their usage
is summed and their capacity counted once. This applies to files and containers.

An omitted `--quota` selects this estimate. An explicit quota above the estimate,
a failed inventory, or a filesystem already inside its protected reserve
refuses startup before listening or joining peers. Quota diagnostics select
GiB or TiB; management and portal usable-capacity reporting retain human units.

This startup estimate does **not** reserve disk blocks, coordinate independent
daemon budgets on a shared filesystem, or protect against later external
consumption. Aggregate node quotas also do not enforce individual target
budgets when data placement is uneven. Those runtime capacity-domain admission
requirements remain open under W99; the startup check must not be described
as completion of proactive storage protection. Existing explicit quotas below
the estimate continue to apply. Runtime shrink only reduces that quota.

Validation: policy/unit tests cover external occupancy, shared roots, insufficient
reserve and integer overflow. ORCD fcstor007 private daemon startup accepted an
automatic 280.59 GiB quota and explicit 1 GiB quota, and rejected 1000 TiB before
listening. Fixtures used temporary local stores and were stopped and removed;
installed service and user data were untouched. All five architecture doc gates
passed. This is not a constrained-filesystem runtime pressure gate.
