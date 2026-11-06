# EFS defect-navigation graph

Source base: `2744821f493eba5f698e22b932855ed25cde6e61`; exact inputs: `source-manifest.json`.
Graphify 0.9.97; 18,048 nodes, 75,018 directed typed edges, 568 communities.
Parallel edges retained. Community clustering uses an undirected projection only; stored edges remain directed.
Extraction cost: 0 input / 0 output LLM tokens (local structural extraction).

## Known-bug navigation checks

- W127-apply-refusal: PASS — Python extracts production C into a compiled fixture: a curated exercises link, not a Python-to-C runtime call. Does not prove live group withdrawal or RDMA acceptance.
- W145-snapshot-unproven-tail: PASS — Locates the snapshot receive/install path and the concrete commit-bound regression. A graph path is not proof of Raft safety or live failover.
- W125-packed-source-CAS: PASS — C oracle checks stale-source cancellation and ledger outcomes. Does not establish the unresolved W148 mismatch cause or full packing qualification.
- F1-stripe-prepublication: PASS — Regression covers pre-publication capacity pressure, wrong FileID and publication racing a NOT_FOUND mark. Storage transport is stubbed; live mount/failover acceptance is separate.
- F2-pending-receipt-ownership: PASS — Production table and hash, colliding pending owners; duplicate execution is refused. Does not assert a prior duplicate durable publication occurred.
- F3-raft-index-width: PASS — Exercises real memory and disk stores with alias indices, valid high indices, snapshots and reopening. No claim of a previously reachable client consensus exploit.
- F4-sweep-metadata-deadline: PASS — The C oracle calls efs_client_data_commit, which calls stripe_prove. Production bookkeeping with a synthetic monotonic jump and stubbed RPCs; verifies inherited absolute budget and retained obligations. Does not establish a live-cluster wall-clock timing bound.
- F5-retired-stripe-two-lost: PASS — The C oracle calls efs_client_data_commit, which calls stripe_prove. Component retirement requires a publication/terminal fence followed by an authoritative absence read. GETCHUNKS transport and base/span/packed/fence cases are exercised separately in test_getchunks_budget.py. No live data-loss claim.

## Relationships

- `affects`: 8
- `calls`: 27544
- `case_of`: 118
- `contains`: 26971
- `defines`: 1993
- `documents_finding`: 8
- `exercises`: 7
- `imports`: 8067
- `imports_from`: 990
- `indirect_call`: 749
- `inherits`: 17
- `invokes`: 2
- `mentions_symbol`: 3004
- `method`: 644
- `rationale_for`: 1395
- `references`: 2161
- `references_path`: 1236
- `regression_for`: 8
- `uses`: 96

## Extraction warnings

- warning: 1 file(s) had syntax errors and may be partially extracted: include/efs/protocol.h (error region at line 331, 851 symbol(s) extracted)
- warning: 76 code file(s) yielded no symbols (0.25 MB) — a data file, or source this extractor models no symbols for: include/efs/batch_obs.h, include/efs/checksum.h, include/efs/dir_layout.h, include/efs/erasure.h, include/efs/frag_record.h (+71 more)

## Limits

Health warning: 34 self-call edges retained. These can be recursion or ambiguous callback resolution; inspect source before interpreting them.
Unresolved edges omitted: {'calls': 1778}. These are extraction gaps, not absent dependencies.
Literal mentions and reviewed exercises links are not calls or runtime coverage. AST edges retain their confidence labels.
No automatic lock ownership, shared-state access, persistence-order, happens-before, or exhaustive build-variant analysis.
Known-bug checks validate navigation, not a rerun of product acceptance or independent rediscovery of the bug.
External library internals are excluded. Function pointers, macros and embedded C/shell may be unresolved.
