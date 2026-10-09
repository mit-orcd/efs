# D36 — three-node minimum, four-node maximum safety, larger clusters scale

**Accepted directly by the user Oct 9, 2026.** Governing contract is
[architecture §2](../how-it-works/architecture.md); implementation remains open.

User wording:

> 3 nodes is the bare minimum to run efs
> 4 nodes increase safety (max level), we can never lose more than 2 node's worth of data
> greater than 4 nodes increase scale (data volume, performance for all dimensions)

Interpretation in this conversation: three nodes must preserve acknowledged
data and authoritative metadata through any one permanent node loss; four
nodes must preserve them through any two. Above four nodes, maximum protection
remains two losses; extra nodes increase capacity and performance across data,
metadata and query service. This is the supported failure envelope, not a claim
that more than two physical nodes cannot fail. Cache loss must be recoverable
from authoritative metadata and must not weaken its protection.

Default data profiles are 2+1 at three nodes and 2+2 from four nodes onward.
The exact four-node metadata protocol, continued read/write availability after
loss and profile migration design are unresolved. Ordinary majority Raft cannot
provide automatic authoritative operation after any two of four voter losses.
A data code alone cannot preserve authoritative commit identity. Do not add an
implicit fifth node/witness, override quorum or use highest-term survivor merge
to claim compliance. Prove preservation and safe recovery before advertising it.

## Implementation ownership and gates

W74 owns data profiles, durable protection evidence, debt and repair. W66 owns
metadata topology/consensus; its scope must include the D36 four-node protection
protocol. W92 owns rebuildable local query caches and W98 all-node query scaling.
W99 protects every disk writer from storage exhaustion. Current fixed 2+1 data
and three-voter metadata groups do not implement the four-node safety target.

Acceptance must exercise all single-node losses at three nodes and all six
two-node-loss pairs at four nodes, including permanent storage loss and cold
restart. Independently verify acknowledged payloads, namespace/attributes,
durable operation verdicts and authoritative commit history. Prove safe
partition behavior and failure during publication/reconfiguration. State
availability limits separately. Larger-node tests must retain the same
protection envelope and measure data, metadata and query scale; adding nodes
alone is not proof of distribution or performance.

## Supersession and agent handoff

D36 supersedes the former four-node/f=1 and f=3/seven-node normative ladder.
Historical Round 4/6 notes remain provenance, not the current requirement.
Future agents must preserve D36, report implementation gaps separately and
request an explicit user decision before changing this contract. Finding that
current code or Raft cannot satisfy it does not revoke it. No runtime protection
change is made by this documentation commit.

[Decision register](decisions.md) · [W74](../backlog/work-items.md#w74) ·
[Status index](README.md)
