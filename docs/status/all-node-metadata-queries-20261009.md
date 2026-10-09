# W98 — metadata queries through all nodes

Registered Oct 9, 2026 as an **open architecture discussion**, separately from
W92's local disk-backed `.efs` cache implementation.

The requested outcome is that any cluster node can serve metadata queries.
Choose whether it forwards queries, executes a federated query over partition
indexes, or maintains additional derived-index replicas. Full authoritative
metadata replication on every node is only one possible design.

In the current default eight-node layout, group 0 uses nodes 1/2/3 and group 2
uses nodes 2/3/4. Nodes 2 and 3 can build a complete local catalog; nodes 1 and 4
have only one group, and nodes 5–8 have no default metadata group. Adding nodes
therefore does not automatically add complete query publishers.

Discuss consistency cuts, permissions, freshness, pagination, failover, bounded
resources and failure-domain placement before choosing a design. Coordinate
consensus/routing changes with W66; keep W92 focused on its local backend.

[Work item and acceptance scope](../backlog/work-items.md#w98) ·
[Current status index](README.md)
