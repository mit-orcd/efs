# v0.3.0-pre-alpha candidate notes

This is an integration candidate, not a published or tagged release. The
[acceptance ledger](../status/v030-pre-alpha-release.md) records exact tested
commits, failures and remaining gates. No push has occurred.

## Included changes

- Tiny-file packing supports opt-in mounted writes, mixed ordinary/packed
  reads, exact publication retries, shared-object GC and cold recovery.
  Exact terminal-rejection cleanup and bounded parallel receipt retirement
  are included. Concurrent ORCD directory/query timeouts remain unresolved.
- Mac request-local authority reuse and combined session registration are
  reconciled with ordered APPEND reservation guards and NUC's tag52 completion.
  Mac's separate single-shard tag53 experiment is not integrated or accepted.
- Code-review repairs cover rollup allocation ownership and retained-capacity
  accounting, recorder shutdown after reap, incoming chunk retention after a
  full write-pipeline failure, and strict address-port parsing. Private legacy
  find/flush/walker paths were removed with focused compatibility regressions.
- Previously committed storage-engine, cache/index, CREATE/publication and
  transport work is retained through the branch ancestry audit. Native tests
  cover the merged code; individual cluster acceptance remains explicit in
  the ledger rather than inferred from older branch results.

## Operator settings and upgrade constraints

Keep ordinary writes as the default. `EFS_PACKED_WRITES=1` enables packing in
the client; all participating servers must support the packed capabilities.
Capability failure must refuse packing before accepting bytes. Once packed
metadata is written, the durable minimum-reader requirement is monotonic:
turning the flag off does not make a store safe for an older reader. Preserve
retained stores and use an agreed upgrade/recovery procedure before rollout.

Upgrade all voting servers to understand new durable commands before admitting
them. Packing reserves metadata tags45-51; APPEND_CATCHUP uses52. Unknown
committed commands cannot safely be handled by rolling back to old server
binaries. Client/server build negotiation and native wire layouts must also
match. Proposed packed batching transport IDs141/142 are reserved but have no
accepted implementation in this candidate.

Select fragment I/O with `--direct-io` or `--no-direct-io`, and transport with
`EFS_TRANSPORT=tcp|rdma|auto`. Keep the storage engine choice independent:
`--storage-engine files|containers`. Passing one engine's mounted gates does
not certify the other. RDMA and GPU-device DMA have separate acceptance scopes.

Receiver feedback remains opt-in with `EFS_RAFT_FLOW=1`; completed AMD
comparisons show throughput gains with variable tails and drain times. They
do not close all W97 obligations. No D25 or staged ticketed data-plane
activation is implied by this release integration.

## Current limits

The merged AMD normal/fault native suites and two-client packed direct/buffered
POSIX, peer, cold-byte and physical-GC gates pass at `d1197381`. Mmap remains
unsupported and is explicitly skipped in extras. Later parser and seven-client
changes are undergoing a new frozen qualification at `f1c322e1`.

Four loopback daemons on one AMD host are functional evidence, not proof of
independent hardware failure resilience. Current fixed2+1 data and three-voter
metadata groups do not implement D36's four-node/two-loss guarantee. ORCD
seven-client packing timeouts, owner-reported live/cold/long APPEND gates and
final exact-source acceptance still block promotion and tagging.
