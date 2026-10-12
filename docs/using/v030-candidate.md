# v0.3.0-pre-alpha candidate notes

**Latest alpha scope:** ordinary files/TCP/direct only, `EFS_PACKED_WRITES=0`, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`. Small-file packing is excluded alongside containers, `.efs`, RDMA, buffered and dedicated repair/recovery. Earlier packing qualification requirements below are historical. [Binding packing exclusion](../status/v030-packing-exclusion-20261012.md).

This is an integration candidate, not a published or tagged release. The
[acceptance ledger](../status/v030-pre-alpha-release.md) records exact tested
commits, failures and remaining gates. No push has occurred.

## Current qualification state

Execution is stopped. Source410afed4/v2 passes AMD full normal/fault native and focused/real-LSM sanitizer prerequisites, but release acceptance is blocked by the c2 AMD stats/find deadlines, Mac c2 mounted timeouts, uncompleted final-source mounted/cold/ownership gates and unresolved retained Mac namespace ownership. The duplicate CREATE fault injector is fixed in410; its strict mounted three-stage/cold gate remains pending. Historical results below do not certify this candidate. [Stop checkpoint](../status/v030-stop-checkpoint-20261011.md).

## Included changes

- Tiny-file packing supports opt-in mounted writes, mixed ordinary/packed
  reads, exact publication retries, shared-object GC and cold recovery.
  Exact terminal-rejection cleanup and bounded parallel receipt retirement
  are included. Concurrent ORCD directory/query timeouts remain unresolved.
- Mac request-local authority reuse and combined session registration are
  reconciled with ordered APPEND reservation guards and NUC's tag52 completion.
  Eligible single-shard APPEND now includes Mac's durable tag53 reservation and tag54 completion paths, CREATE
  adoption and coverage-aware leader routing; merged acceptance is pending.
  Whole-cohort packed authority and fresh legacy fallback are also integrated.
- Code-review repairs cover rollup allocation ownership and retained-capacity
  accounting, recorder shutdown after reap, incoming chunk retention after a
  full write-pipeline failure, strict address-port parsing and typed FUSE read errors (allocation failure
  returns ENOMEM). Private legacy
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
match. Packed batching transport IDs141/142 are integrated with capability-negotiated
scalar fallback. The bounded implementation preserves per-record durable
receipts and validates the entire reply before exposing record verdicts; live
concurrent acceptance remains open.

Select fragment I/O with `--direct-io` or `--no-direct-io`, and transport with
`EFS_TRANSPORT=tcp|rdma|auto`. Keep the storage engine choice independent:
`--storage-engine files|containers`. Passing one engine's mounted gates does
not certify the other. RDMA and GPU-device DMA have separate acceptance scopes.

Receiver feedback remains opt-in with `EFS_RAFT_FLOW=1`; completed AMD
comparisons show throughput gains with variable tails and drain times. They
do not close all W97 obligations. No D25 or staged ticketed data-plane
activation is implied by this release integration.

## Historical gates and continuing limits

The merged AMD normal/fault native suites and two-client packed direct/buffered
POSIX, peer, cold-byte and physical-GC gates pass at `d1197381`. Mmap remains
unsupported and is explicitly skipped in extras. Later parser and seven-client
changes pass full normal/fault native and four sanitizer gates at `f1c322e1`.
Seven-client AMD direct and buffered runs at the production-identical
`39b4297b` pass all file-operation and cold-byte checks but exceed the original
180-second physical packed GC deadline. A bounded scheduler correction is
accepted on AMD at `bbb84e41`: complete merged normal/fault native and sanitizer
gates pass, and seven-client/jobs4 physical GC passes within the unchanged
deadline in both I/O modes. The newer batch-integrated `f0bb0208` has separate
full merged normal/fault native and seven sanitizer passes. Seven-client/jobs16
packing still fails in both modes, matching the ORCD blocker.

Four loopback daemons on one AMD host are functional evidence, not proof of
independent hardware failure resilience. Current fixed2+1 data and three-voter
metadata groups do not implement D36's four-node/two-loss guarantee. ORCD
seven-client directory/query timeouts, owner-reported live/cold/long APPEND gates and
final exact-source acceptance still block promotion and tagging.

The packed GC scheduler correction preserves the one-second default idle.
`EFS_GC_BACKLOG_IDLE_MS=20..1000` is the existing opt-in setting for shorter
backlog idle, used at 20ms in these private AMD fixtures. Acceleration requires
a capped page and actual local physical deletion; blocked, idle and fence-only
passes retain normal backoff. All durable member fences/ACKs and work budgets
remain required. A focused regression pass alone does not close the live gate.

All voting servers must support durable tags53 and54 before new APPEND commands are
admitted. Retained stores containing new commands cannot roll back to older
binaries. General cross-shard APPEND retains ordered guards and tag52 completion.

## Current alpha capability scope

The planned v0.3.0-pre-alpha targets files storage over TCP with direct I/O. `.efs` commands, stats/find and the derived query-cache service are excluded: configure FUSE with `EFS_COMMANDS=0` and every daemon with `EFS_COMMANDS_SERVER=0`. Containers and dedicated repair/recovery remain unqualified. Candidate acceptance is incomplete; no release tag or push has been made.
