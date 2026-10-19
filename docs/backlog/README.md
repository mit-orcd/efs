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

- [W123: automatic crash recovery](crash-safe-durable-versions.md) — proposed new feature; per-file visible/durable versions, recovery ordering and acceptance gates.
- [W124: fast delete](fast-delete-reclaim.md) — proposed; batched node-local reclaim, quota credit at delete commit, then packing (W125), re-assessed slot reuse and subtree delete.
- [W125 implementation plan](small-file-packing-plan.md) — phases P0–P7: decisions, port from the archive, read path, background packer and compaction, packed-file modification, landing checks, qualification.
- [W125: small-file packing](small-file-packing-default.md) — proposed, revised to background packing after close; write path unchanged, an always-on packer moves closed small files into shared fragments (no data in metadata rows) with lifetime grouping and compaction against pinned holes; no switch, prior state kept as a tag.
