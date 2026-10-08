# Backlog — open items, someday items

[Docs index](../README.md) · [Status (the queue)](../status/README.md) ·
[Architecture](../how-it-works/architecture.md)

**status/ is the queue** — ordered and gated; follow its current work direction
and queue position. W identifiers are stable identities, not priority numbers. **backlog/ holds details and parked ideas** — a detail here may belong to an
active queue item; it has no priority beyond what the status index/handoff says.

- [work-items.md](work-items.md) — long-form text for the open W items:
  source evidence, numbered steps, binding Forbidden lists.
- [decision-contracts.md](decision-contracts.md) — active accepted D25–D27
  details and gates, required when changing those components.
- [product-gaps.md](product-gaps.md) — what is missing before this is a
  filesystem you could run (audited capability inventory).
- [ideas.md](ideas.md) — parked ideas and landed scaling history; never
  instructions.
- [storage-engine-v2.md](storage-engine-v2.md) — proposed independent
  EFS-relevant I/O principle experiments, XFS optimizations, and an experimental container
  backend; includes the Oct 7 AMD baseline and recovery/acceptance gates.
