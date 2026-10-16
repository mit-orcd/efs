# v0.3.0-pre-alpha candidate notes

**Latest exact-source result:** `4e239d029029b8105a6e07f0e69a26fdc449de32` passes AMD ordinary-files qualification after the deep-directory fix; .efs remains disabled. [Report](../status/v030-deep-directory-fix-20261012.md). No push or tag. Earlier failed checkpoints below are historical.

**Latest alpha scope:** ordinary files/TCP/direct only, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`. `.efs`, RDMA, buffered and dedicated repair/recovery are excluded.

This is an integration candidate, not a published or tagged release. The
[acceptance ledger](../status/v030-pre-alpha-release.md) records exact tested
commits, failures and remaining gates. No push has occurred.

## Current qualification state

Execution is stopped. Source410afed4/v2 passes AMD full normal/fault native and focused/real-LSM sanitizer prerequisites, but release acceptance is blocked by the c2 AMD stats/find deadlines, Mac c2 mounted timeouts, uncompleted final-source mounted/cold/ownership gates and unresolved retained Mac namespace ownership. The duplicate CREATE fault injector is fixed in410; its strict mounted three-stage/cold gate remains pending. Historical results below do not certify this candidate. [Stop checkpoint](../archive/status-20261012/v030-stop-checkpoint-20261011.md).

## Included changes

- Exact terminal-rejection cleanup and bounded parallel receipt retirement
  are included. Concurrent ORCD directory/query timeouts remain unresolved.
- Mac request-local authority reuse and combined session registration are
  reconciled with ordered APPEND reservation guards and NUC's tag52 completion.
  Eligible single-shard APPEND now includes Mac's durable tag53 reservation and tag54 completion paths, CREATE
  adoption and coverage-aware leader routing; merged acceptance is pending.
- Code-review repairs cover rollup allocation ownership and retained-capacity
  accounting, recorder shutdown after reap, incoming chunk retention after a
  full write-pipeline failure, strict address-port parsing and typed FUSE read errors (allocation failure
  returns ENOMEM). Private legacy
  find/flush/walker paths were removed with focused compatibility regressions.
- Previously committed storage, cache/index, CREATE/publication and
  transport work is retained through the branch ancestry audit. Native tests
  cover the merged code; individual cluster acceptance remains explicit in
  the ledger rather than inferred from older branch results.

## Operator settings and upgrade constraints

Preserve retained stores and use an agreed upgrade/recovery procedure before
rollout.

Upgrade all voting servers to understand new durable commands before admitting
them. Metadata tags45-51 are reserved; APPEND_CATCHUP uses52. Unknown
committed commands cannot safely be handled by rolling back to old server
binaries. Client/server build negotiation and native wire layouts must also
match.

Select fragment I/O with `--direct-io` or `--no-direct-io`, and transport with
`EFS_TRANSPORT=tcp|rdma|auto`. RDMA and GPU-device DMA have separate
acceptance scopes.

Receiver feedback (`EFS_RAFT_FLOW`) and the staged D25 logical resize were
removed from devel on October 16, 2026. No staged ticketed data-plane
activation is implied by this release integration.

## Historical gates and continuing limits

The merged AMD normal/fault native suites pass at `d1197381`. Mmap remains
unsupported and is explicitly skipped in extras. Later parser and seven-client
changes pass full normal/fault native and four sanitizer gates at `f1c322e1`.
A bounded GC scheduler correction is accepted on AMD at `bbb84e41`: complete
merged normal/fault native and sanitizer gates pass. The newer
batch-integrated `f0bb0208` has separate full merged normal/fault native and
seven sanitizer passes.

Four loopback daemons on one AMD host are functional evidence, not proof of
independent hardware failure resilience. Current fixed2+1 data and three-voter
metadata groups do not implement D36's four-node/two-loss guarantee. ORCD
seven-client directory/query timeouts, owner-reported live/cold/long APPEND gates and
final exact-source acceptance still block promotion and tagging.

The GC scheduler correction preserves the one-second default idle.
`EFS_GC_BACKLOG_IDLE_MS=20..1000` is the existing opt-in setting for shorter
backlog idle, used at 20ms in these private AMD fixtures. Acceleration requires
a capped page and actual local physical deletion; blocked, idle and fence-only
passes retain normal backoff. All durable member fences/ACKs and work budgets
remain required. A focused regression pass alone does not close the live gate.

All voting servers must support durable tags53 and54 before new APPEND commands are
admitted. Retained stores containing new commands cannot roll back to older
binaries. General cross-shard APPEND retains ordered guards and tag52 completion.

## Current alpha capability scope

The planned v0.3.0-pre-alpha targets files storage over TCP with direct I/O. `.efs` commands, stats/find and the derived query-cache service are excluded: configure FUSE with `EFS_COMMANDS=0` and every daemon with `EFS_COMMANDS_SERVER=0`. Dedicated repair/recovery remains unqualified. Candidate acceptance is incomplete; no release tag or push has been made.
