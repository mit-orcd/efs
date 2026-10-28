# EFS defect-navigation graph

Source base: `ebac01dedac785d64dfa3c49efc0ca6fbadccc03`; exact inputs: `source-manifest.json`.
Graphify 0.9.97; 16,332 nodes, 68,143 directed typed edges, 516 communities.
Parallel edges retained. Community clustering uses an undirected projection only; stored edges remain directed.
Extraction cost: 0 input / 0 output LLM tokens (local structural extraction).

## Known-bug navigation checks

- W127-apply-refusal: PASS — Python extracts production C into a compiled fixture: a curated exercises link, not a Python-to-C runtime call. Does not prove live group withdrawal or RDMA acceptance.
- W145-snapshot-unproven-tail: PASS — Locates the snapshot receive/install path and the concrete commit-bound regression. A graph path is not proof of Raft safety or live failover.
- W125-packed-source-CAS: PASS — C oracle checks stale-source cancellation and ledger outcomes. Does not establish the unresolved W148 mismatch cause or full packing qualification.

## Relationships

- `affects`: 3
- `calls`: 26020
- `case_of`: 116
- `contains`: 24880
- `defines`: 1845
- `documents_finding`: 3
- `exercises`: 3
- `imports`: 6564
- `imports_from`: 813
- `indirect_call`: 650
- `inherits`: 16
- `invokes`: 2
- `mentions_symbol`: 2129
- `method`: 623
- `rationale_for`: 1220
- `references`: 2089
- `references_path`: 1074
- `regression_for`: 3
- `uses`: 90

## Extraction warnings

- warning: 1 file(s) had syntax errors and may be partially extracted: include/efs/protocol.h (error region at line 322, 833 symbol(s) extracted)
- warning: 49 code file(s) yielded no symbols (0.19 MB) — a data file, or source this extractor models no symbols for: include/efs/batch_obs.h, include/efs/checksum.h, include/efs/dir_layout.h, include/efs/erasure.h, include/efs/frag_record.h (+44 more)

## Limits

Health warning: 34 self-call edges retained. These can be recursion or ambiguous callback resolution; inspect source before interpreting them.
Unresolved edges omitted: {'calls': 1406}. These are extraction gaps, not absent dependencies.
Literal mentions and reviewed exercises links are not calls or runtime coverage. AST edges retain their confidence labels.
No automatic lock ownership, shared-state access, persistence-order, happens-before, or exhaustive build-variant analysis.
Known-bug checks validate navigation, not a rerun of product acceptance or independent rediscovery of the bug.
External library internals are excluded. Function pointers, macros and embedded C/shell may be unresolved.
