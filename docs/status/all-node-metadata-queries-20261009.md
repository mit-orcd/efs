# W98 — from fixed placement to distributed metadata capacity

Registered Oct 9, 2026 as an **open architecture discussion**, expanded to
capture the scaling goal for ordinary POSIX metadata and `.efs` queries.
No running-cluster topology or placement policy changes with this note.

Today 4096 logical KV shards map to two fixed Raft groups. With three nodes,
every node hosts both groups. With four nodes, group 0 uses nodes 1/2/3 and
group 2 uses nodes 2/3/4; nodes 2 and 3 alone can publish a complete local query
catalog. Node 4 contributes authoritative group-2 work and data storage.
Membership is currently limited to four nodes. Extending that placement rule
unchanged would leave additional nodes without metadata groups; eight nodes
are a target configuration rather than a current supported baseline.

The requested direction is **locality-aware metadata sharding with bounded
replication**, using partitioned consensus (Multi-Raft). A versioned map assigns
logical shards to configurable groups, places their replicas across failure
domains, and distributes leadership. Additional nodes host and lead selected
groups, increasing metadata storage and execution capacity. Replica count stays
independent of cluster size: a usual three-voter group still commits an ordinary
update through its own quorum when the cluster grows to many more nodes.

This covers lookup/stat, create/unlink, chmod/timestamps, readdir, rename, chunk
mappings, append, leases and locks. Co-locate related authority where practical
to keep common operations within few groups. Cross-group operations coordinate
only affected groups through transaction protocols. Directory partitioning and
file lanes address hotspots when warranted; distributing unrelated work does
not itself accelerate one contended directory or file.

For `.efs` queries, build partition-local indexes and distribute query execution
to eligible replicas, with bounded aggregation at any entry node. Forwarding to
today's complete publishers can help transition, but does not satisfy capacity
scaling. Full authoritative replication everywhere is not the required target.
Query cuts must preserve cross-group transaction consistency, fresh permissions,
complete coverage and stable pagination. Publisher loss either resumes the exact
retained generation or explicitly expires its cursor; it cannot silently switch
to another generation. Quorum loss and query-publisher loss are distinct gates.

The transition first measures the current layout, then removes fixed membership
and group assumptions, introduces versioned routing and ownership handoff, and
expands/rebalances groups in measured stages. Migration must preserve authority,
committed state and recovery ownership rather than merely change a hash function.
Shared scheduling, connections and WAL batching keep group growth affordable;
queries and migration need caps so they do not starve POSIX metadata operations.

Acceptance requires measured gains in namespace capacity, ordinary metadata
throughput and query throughput at 4/8 and larger node counts, with bounded
resources and explicit latency targets. Measure participants per operation and
replication traffic: cluster growth must not turn routine updates into
cluster-wide coordination. Include hotspots, failover, movement, permissions,
pagination and transaction recovery tests.

W66 owns authoritative distribution, routing and migration. W98 owns the
end-to-end scaling discussion and distributed query design built on it; W92's
local disk-backed cache implementation can continue independently.

[Work item and acceptance scope](../backlog/work-items.md#w98) ·
[Current status index](README.md)
