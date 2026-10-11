# Release disposition and branch revalidation

October 11, 2026. Review checkpoint for v0.3.0-pre-alpha. No production code or test data is deleted by this checkpoint. The original reports and inventory retain their pinned baseline counts; this addendum overrides deletion recommendations for release coordination.

## Exact inspected commits

Original review base: `48a2719d19fb5a6b0464935c70a967a1f1a86236`.

- `devel-orcd`: `a48076c4e9373ea47c143c231e2b3cdb3c1e20fe`
- `devel-nuc`: `1f1037a6aa001546982defcb4e5e3997d694076f`
- `devel-mac`: `b2c01fa05afa1287cd358f352ebfbc831b570e31`

These are committed snapshots, not the branches' later tips or their worktree changes. Exact-token search evidence is retained in [the revalidation inventory](release-revalidation-20261011.json). Original reproducible scratch copies and probes are in [the probe archive](structural-review-probes-20261011.tar.gz); its README describes commands, and its manifest hashes the source files. Full source copies inside the archive are historical scratch evidence, not alternate implementations to build or merge.

Archive SHA-256: `3c2758887fe5ea82a40c5b6a031da05dc3aab6c549ceeeaae12c484f9b2d485d`.

## Release correctness and compatibility gates

| Finding | Release disposition | Focused acceptance |
|---|---|---|
| S1 partial multi-realloc failure | Correctness blocker until fixed or the queue is safely removed. The exact unsafe three-allocation sequence remains in all three inspected `src/meta/metadata.c` files. The baseline ASan probe proves dangling-owner/double-free behavior; it does not diagnose a cluster crash. Prefer a small ownership-safe growth fix to broad rollup removal in a release checkpoint. | Force each of the three allocation failures, both with empty arrays and with live/moved owners. Cleanup and retry under ASan must be safe, existing entries unchanged, capacity consistent. Run create/link/unlink/rename and staged-byte pressure gates afterward. |
| S1 retained queue payload omitted from staged estimate | Memory-accounting release risk if the release promises a bounded client cache. Queue retains 17 bytes per capacity slot after clearing its count; baseline probe shows 1,088 uncharged bytes at first growth. Not proof of observed OOM. | Account capacity, not count, or remove the queue safely; grow/clear/reuse it repeatedly under the configured cache bound and verify allocation admission. |
| S6 recorder stop after successful reap | Correctness fix required for enabled profiling: copies may SIGKILL a PID already reaped. No observed wrong-process incident. A small return/clear-on-reap fix is preferable to recorder-framework consolidation. | Stub successful reap and assert no subsequent kill; also EINTR, ECHILD, already-exited child and timeout TERM/KILL/reap. |
| Packed read/write, snapshots and RPC layouts | Compatibility gate, not a newly proven bug: preserve active packing and reserved/native wire layout; do not shrink inode/RPC structs as cleanup. | Packed write/read/restart/hash, overwrite/truncate/unlink/GC, corrupt fragments, terminal ACK/retry, leader/snapshot recovery and mixed-version rejection. |
| Stats/query lifecycle | Compatibility gate, not a newly proven bug: preserve server query authority and new lookup/open snapshot pinning. | `test_query_stats_snapshot.py`, `test_query_callback_affinity.py`, `test_query_snapshot_failover.py`, nonempty mounted stats, mutation between lookup/open and large incremental deltas. |
| Public APIs and RDMA completion contract | No public-header removals without a consumer/compatibility decision. An in-tree absence is not an external ABI guarantee. | Linux builds and public-header consumers; real RDMA outstanding payload lifetime/disconnect/timeout tests for any quiesce-contract change. |

The allocator and recorder findings are distinct from speculative cleanup. Their isolated reproductions remain baseline evidence; newer source inspection confirms the relevant implementation still exists, but no new full branch build or live-cluster execution was performed here.

## New active consumers and revised cleanup boundaries

ORCD adds `src/client/pack_write.c`, pack admission/mapping, packed upload/fence/GC and packed-reader integration. `read.c` consumes `ce.packed.segment_id` and `efs_packed_read_base_scratch`; `write.c` has active pack admission/wiring. `EFS_PACKED_WRITES=1` selects this path. NUC carries packed segment/ledger/fence support too. This invalidates any general claim that packing has no production consumer.

The old `pack_ino/pack_off/pack_len` fields still occur in inode copies and legacy layout/seek branches. Inspected `stat_to_inode` implementations still start with zero initialization, while the new packed mapping uses a separate chunk-entry reference. This supports a distinction between legacy inode slots and the new packed model; it does **not** authorize deleting either contract. **Defer S4 pack-field behavior removal and its private-layout savings until the integrated release passes packing compatibility gates.** Remove misleading “no pack staging” comments only against current source. The initial S4 allocator-store cleanup may still be assessed separately.

ORCD's FUSE stats code now pins command snapshots across lookup/open. MAC changes query catalog/disk/incremental machinery and refresh scheduling. Current stats still call `efs_client_rpc_query_snapshot` and obtain a server-owned aggregate, not `efs_export_format_stats_ex`. The local recursive rollup chain remains a candidate after the release, with immediate routing counts preserved; never delete authoritative query rollups because the similarly named local fields are obsolete.

Exact-token searches of all three pins still find R2–R8 wrapper symbols at declarations/definitions and, for read-cache/RDMA helpers, stale comments/contracts; no new executable caller was found in src/include/tests/scripts. R1's old parse/run chain remains separate from live find/query handling. This reconfirms only their bounded in-tree candidate status. Keep native/wire APIs and actual RPC implementations intact; a removable v3 convenience wrapper does not make v3 support removable.

## Bounded cleanup and refactoring

- R1 private legacy find chain; R5–R7 internal wrappers; R9 no-op guard: separate small cleanup patches with existing query/authority/memory gates. Prefer after correctness checkpoints.
- R2–R4 and R8: public API/contract decisions first. R10 CLI: still a tested compatibility sentinel; explicit retirement required.
- S2 obsolete flush mode and S3 disabled benchmark permission mode: retain their original narrow boundaries and validate against the eventual integrated release source before implementing.
- S5 common cache patch primitive: bounded consolidation; preserve differing base-fetch admission policies and fail-closed flush merging.
- S7 checked address parser: useful validation fix; agree on rejection behavior. S8 byte-order helper sharing: byte-identical golden encodings and malformed input gates; preserve record formats.
- S6 shared profiling implementation, S9 direct-KV namespace migration, S10 write-path sharing, S11 simulator/planner sharing: refactoring work, not release blockers merely due to line count. Fix concrete S6 reap behavior independently.
- S12 publication/tickets/lane state, S13 selectable RAM oracle, storage containers and indexed-packing experiments: retain. New pack publication consumers strengthen this recommendation.
- Test evidence pruning: defer active release evidence; use the original selective manifest recommendations for historical raw captures only after owner review.

The original 1,249–1,514-line combined estimate is a **historical base-specific opportunity range**, not a release deletion budget. Excluding the entire 35–55-line S4 estimate conservatively leaves 1,214–1,459 historical candidate lines; this is not an updated integrated-release patch estimate. No production cleanup is committed here.

## Validation of this documentation checkpoint

The prior report records baseline scratch/probe checks and their limitations. This checkpoint rechecks committed branch consumers and pins every inspected SHA. Run documentation gates on the report branch; release tests belong to each implementation checkpoint and the coordinating release branch. Do not cherry-pick the archive's scratch source into production.
