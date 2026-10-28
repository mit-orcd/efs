# EFS defect-navigation graph

Source base: `4ea10be40caca5bbfb16a3f74202452bd25af1fc`; exact inputs: `source-manifest.json`.
Graphify 0.9.97; 16,383 nodes, 68,314 directed typed edges, 490 communities.
Parallel edges retained. Community clustering uses an undirected projection only; stored edges remain directed.
Extraction cost: 0 input / 0 output LLM tokens (local structural extraction).

## Known-bug navigation checks

- W127-apply-refusal: PASS — Python extracts production C into a compiled fixture: a curated exercises link, not a Python-to-C runtime call. Does not prove live group withdrawal or RDMA acceptance.
- W145-snapshot-unproven-tail: PASS — Locates the snapshot receive/install path and the concrete commit-bound regression. A graph path is not proof of Raft safety or live failover.
- W125-packed-source-CAS: PASS — C oracle checks stale-source cancellation and ledger outcomes. Does not establish the unresolved W148 mismatch cause or full packing qualification.
- F1-stripe-prepublication: PASS — Regression covers pre-publication capacity pressure, wrong FileID and publication racing a NOT_FOUND mark. Storage transport is stubbed; live mount/failover acceptance is separate.
- F2-pending-receipt-ownership: PASS — Production table and hash, colliding pending owners; duplicate execution is refused. Does not assert a prior duplicate durable publication occurred.
- F3-raft-index-width: PASS — Exercises real memory and disk stores with alias indices, valid high indices, snapshots and reopening. No claim of a previously reachable client consensus exploit.

## Relationships

- `affects`: 6
- `calls`: 26049
- `case_of`: 116
- `contains`: 24946
- `defines`: 1845
- `documents_finding`: 6
- `exercises`: 6
- `imports`: 6584
- `imports_from`: 813
- `indirect_call`: 645
- `inherits`: 16
- `invokes`: 2
- `mentions_symbol`: 2167
- `method`: 627
- `rationale_for`: 1221
- `references`: 2098
- `references_path`: 1071
- `regression_for`: 6
- `uses`: 90

## Extraction warnings

- warning: 1 file(s) had syntax errors and may be partially extracted: include/efs/protocol.h (error region at line 322, 833 symbol(s) extracted)
- warning: 49 code file(s) yielded no symbols (0.19 MB) — a data file, or source this extractor models no symbols for: include/efs/batch_obs.h, include/efs/checksum.h, include/efs/dir_layout.h, include/efs/erasure.h, include/efs/frag_record.h (+44 more)

## Limits

Health warning: 34 self-call edges retained. These can be recursion or ambiguous callback resolution; inspect source before interpreting them.
Unresolved edges omitted: {'calls': 1410}. These are extraction gaps, not absent dependencies.
Literal mentions and reviewed exercises links are not calls or runtime coverage. AST edges retain their confidence labels.
No automatic lock ownership, shared-state access, persistence-order, happens-before, or exhaustive build-variant analysis.
Known-bug checks validate navigation, not a rerun of product acceptance or independent rediscovery of the bug.
External library internals are excluded. Function pointers, macros and embedded C/shell may be unresolved.
