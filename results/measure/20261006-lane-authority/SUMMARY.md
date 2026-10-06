# D25 lane-local authority foundation — Oct 6 2026

Accepted decision: lane-local authoritative views before D25 activation.

Implemented:

- Durable FileID/lane geometry, epoch and complete-history floor, with local
  stamp/history collection and bounded retries. No inode or coordinator read.
- Cold bootstrap PREPARE subtype: freeze inode/history/bitmap, then atomically
  prepare lane stamp/history/authority. Durable decide/resolve use existing
  transaction recovery. Active bitmap includes bootstrapped lanes.
- Fence transactions and the internal single-authority fence carry installed
  lane authority atomically. Missing authority is NOT_FOUND; unresolved local
  intents are BUSY; lost history cannot keep an obsolete complete floor.
- Lane-routed writer-view RPC and client validation with exact FileID and
  configured geometry. Single-group hosts need only the lane ReadIndex.

Validation:

- aarch64 Linux private `/tmp/efs-lane-authority-review`: full build and full
  `make test` PASS. This directory is separate from the deployed `/tmp/efs`.
- Linux metadata suite under ASan/UBSan with leak detection: PASS. Corrected two
  pre-existing W36 test-fixture wrapper leaks discovered by LeakSanitizer.
- Production client RPC and server read-adapter extraction tests: normal and
  ASan/UBSan PASS. Tests assert actual lane routing and one-group ReadIndex,
  redirects/BUSY bounds, holes, invalid identity/geometry/replies, and failure
  preservation.
- Metadata tests cover partial lane-before-inode resolution, bootstrap replay,
  failed atomic storage, aborted fences, missing/retired history and repeated
  fence races with unchanged output on failure.
- Architecture documentation gate: 5/5 PASS; `git diff --check` PASS.

Limits: public logical truncate and epoch-aware FUSE admission remain disabled.
The cold coordinator is internal; its network/FUSE admission hookup is next.
Its current capture requires a dual-host for the inode and lane groups, while
ordinary views have no dual-host requirement. Bootstrap restart/snapshot gates,
coherent floor retirement, dead-FileID cleanup, live sweep progress and measured
100/1000-client distribution remain outstanding. No live mount or workload
rerun was performed for this checkpoint.
