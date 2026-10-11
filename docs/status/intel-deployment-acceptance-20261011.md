# Intel devel-intel acceptance — 2026-10-11

Current production build is `7bde5f43` on `devel-intel`; the branch also contains the test-only admission repair `e9c395c7`. This ledger remains in progress. The complete-source matrix is `intel-session-accept-7bde5f43`. Historical candidate verdicts below are retained separately; an incomplete-source run is excluded from acceptance. The [earlier 775d935c acceptance](../archive/status-20261011/intel-deployment-acceptance-775d935c-20261011.md) preserves the original failures and workload evidence.

## Deployment and test contract

Two daemons per physical Intel host use TCP, with 400 GiB quota per daemon. Each host has one `/data1/efs/mnt` client seeded from the opposite host. `.efs` is enabled. Acceptance exercises both direct and buffered storage I/O, concurrent POSIX and peer operations, query publication/policy/failover/load gates, session and guarded-lease cleanup, cold payload recovery, real Valgrind daemon/client tests, sanitizer regressions and all independent live rounds. RDMA acceptance belongs on AMD software RDMA or `orcd-test` hardware IB/RDMA.

## Repairs on this branch

- Preserve failed pooled-query routing decisions and exact first-page publisher ownership.
- Preserve authoritative feature-discovery errors rather than presenting unavailable policy as successful disabled commands.
- Require both file-count and byte-count convergence in the loaded query oracle.
- Free real direct-I/O thread-local buffers when TCP workers retire.
- Bound and join REPORT first-touch session admissions while retaining their durable fencing proofs.
- Scope offline cleanup checks to owned test inodes, while retaining unrelated marker diagnostics.
- Retire an empty tiny-file close obligation through the existing atomic barrier; held leases, locks, append reservations and append cursors still refuse early retirement.
- Preserve replay identity while a committed namespace receipt is still materializing.
- Retry explicit BUSY responses for readonly session queries and exact-epoch fences within the existing bounded management budget. Fence participant floors within the existing barrier deadline and send the captured epoch to FINISH, preventing a later session epoch from being finished.
- Preserve retryable fresh-quorum refusal through transaction reads and route foreign-coordinator reads from partial hosts to dual hosts.
- Reuse a LOOKUP quorum proof only within the same request and group, retaining transaction-aware reads and other-group proofs.
- Avoid remote Git scans when the deployment supplies verified build identity.
- Batch sequential Raft replay into bounded 64 KiB windows; hard read errors refuse recovery without truncating the durable log.
- Make memory verdicts require complete unit, client and daemon evidence; deliberate crash-child leak lifetimes remain explicitly excluded rather than claimed leak-free.

## Acceptance remains open

Original failures are retained alongside reruns. Concurrent mixed-workload `fsync` EIO and timeout observations remain under investigation. Earlier production core and extras timeout verdicts do not become passes because later isolated commands succeed. Writable shared mmap remains an explicit unsupported skip. Repair/protection faults retain the W66/W74 gap under D36.

Intel1 completed exactly 1,000,000 files with exit 0. Intel2's strict inventory later verified 611,060 existing files and its writer added 31, reaching 611,091 before stopping on EEXIST for a newly created folder. A retained server trace shows COMMIT returned BUSY before the receipt materialized; the receipt guard repairs that demonstrated retry gap. Earlier resume inventory EIO is retained separately. No existing file is overwritten or silently skipped. Resuming actual creation and checking immutable size/SHA256 samples remain required.

Repeated partial-host trials of the routing repair pass on both hosts in both modes: 16 peer mkdir race repeats (128 races) plus the two deep-directory cases at unchanged deadlines. The old diagnostic fails with a matching missing-coordinator-group trace. These targeted results do not close the full concurrent POSIX gate.

Real durable replay tests pass on both hosts, including EINTR, short reads, payloads crossing buffer boundaries and nondestructive injected EIO. Normal replay uses 9 preads for a 559,624-byte test log instead of 2,049. The old implementation's EIO negative control truncates that private log to 8 bytes; the repaired implementation preserves the entire file and refuses open. The earlier production cold readiness failure at 180 seconds remains recorded; The `1b6796ef` restart still failed readiness. Devops `15b1db0` launches all persisted peers before readiness checks; the previously missing n4 then started in about 22 seconds and both opposite-host mounts were restored. The fresh `e66feefd` rollout then passed startup in 105.25 seconds, restored all four peers and both opposite-host mounts, and passed immutable size/SHA256 checks for 256 payload samples on each host. `.efs/help` reads pass on both hosts. Mode-switch and final recovery acceptance remain required.

Terminal per-host and per-mode verdicts, runtime identity, current tiny-file progress and remaining W-item obligations will be recorded after the active matrices finish.

The focused exact-epoch fencing run passes direct mode on Intel1. Intel2 direct mode fails in the concurrent tiny/large workload with close EIO and retained flush-meta BUSY evidence; it does not establish a fencing pass. Buffered follow-ons and full final-source verdicts remain pending.

The complete-source final native target and 75 additional standalone regressions pass on both hosts. Intel2 query/.efs gates pass direct (69.30 s) and buffered (59.88 s). Intel1 direct fails loaded-reader generation advancement and buffered fails the unchanged 12-second stats subprocess budget; neither is accepted. Strict resumed inventory precedes actual tiny-file creation; a running inventory is not reported as writer progress.

REPORT admission repair `8238f20b` returns BUSY before side effects when a multi-shard batch cannot obtain the bounded admission workers. It preserves existing deadlines, fencing and exact publication retry identity. Full native rechecks pass on both hosts. Targeted worker retirement, mixed tiny/large writes, cold-byte checks, session cleanup, guarded leases and physical reclamation pass on both hosts in both modes: Intel1 direct 538.83 s / buffered 540.22 s; Intel2 direct 472.26 s / buffered 413.91 s. Initial review-tree native failures due to incomplete documentation inputs remain recorded separately from the successful complete-input rechecks.

Cold ticket regression `008faaa0` retries only NOTPRIMARY/BUSY with the identical ticket and payload inside the original deadline. Hard failures and stale-ticket assertions remain failures. Actual CLI/sanitizer regressions and focused live cold-ticket checks pass on both hosts in both modes.

The pre-backpressure production direct run passes all 66 wired two-client cases. Core results remain failed: Intel1 211/214, Intel2 206/214. Each extras run has two passes, two failures and one unsupported mmap skip. Both cold persistence checks pass. The final revision's direct and buffered matrices are running; these older failures are not overwritten. Ten historical status checkpoints and repair notes have been archived; W100–W106 and current acceptance remain active.

The final `8238f20b` native target and all 76 additional standalone regressions pass on both hosts. The filtered deployment completes in 24.80 s; aggregate startup/mount checks complete in 181.86 s without changing per-node readiness deadlines. Runtime checks verify the installed build identity, all four 400 GiB direct-I/O daemons, opposite-host seed clients and enabled `.efs` commands. Final query gates: Intel2 direct PASS (60.70 s), Intel1 buffered PASS (107.18 s), Intel1 direct FAIL (111.97 s), with a retained 12-second loaded-stats subprocess timeout. Other final-source acceptance phases remain pending.

Intel2 final direct resume strictly verifies 614,948 existing files in 392.8 seconds before actual creation resumes (615,119 observed). Both hosts pass the 256 immutable payload sample checks after deployment. Final production concurrent core/extras/peer tests run while the resumed writer is active. Final private direct acceptance still retains a beyond-64-level directory timeout; no deadline is increased.

Intel2 final buffered query passes reader freshness (generations 2–7) and exact closed-writer publication (601 files / 153,856 logical bytes), then the original sequential payload oracle exceeds the unchanged 150-second harness budget. The bounded-reader repair is committed with both-host regression proof; its live follow-ons remain pending.


## Current 7bde5f43 matrix

The deployment completed in 25.30 seconds and startup/mount checks in 216.45 seconds at unchanged per-node limits. Runtime identity, four 400 GiB quotas, direct I/O, opposite-host client seeds and `.efs/help` pass. Preserved size/SHA256 checks pass for 256 samples per host. Intel2 resumes its existing tree after strict inventory; inventory alone is not counted as creation.

| Tier | Intel1 | Intel2 |
| --- | --- | --- |
| Native target | PASS | PASS |
| Additional standalone scripts | 78/78 PASS | 78/78 PASS |
| Six query/.efs gates, direct | PASS, 68.77 s | PASS, 68.66 s |
| Six query/.efs gates, buffered | PASS, 57.78 s | PASS, 220.55 s |
| Full private direct | FAIL before POSIX; rerun queued | Running |
| Full private buffered | FAIL, core 213/214 and peer 66/66 PASS | Queued |
| Valgrind, independent gates, faults | Queued | Queued |
| Production concurrent/persistence, both modes | Queued | Queued |

Intel1's direct private run reaches the second guarded-lease session admission, then receives explicit `EFS_ERR_BUSY` from ESTABLISH. It stops before the later POSIX tier; those cases are NOTRUN in that attempt. Commit `e9c395c7` retries the unchanged administrative request only on explicit BUSY within the original 60-second command budget. Hard errors, stale epochs and expiration still fail. Regression tests pass locally and on both Intel hosts. Full private reruns in both modes are queued after the current owned fixtures finish. The original failed aggregate remains recorded. All six real query gates passed on both hosts and modes, including exact count/byte convergence and every payload check using the bounded readers; the unnecessary idle query-repeat queues were retired.

Three additional dated Intel repair notes (rename flag refusal, empty xattr names and creation inheritance) are archived after their named current POSIX contracts pass. [Named contract evidence](../../results/measure/20261011-intel-devel-intel/intel-repair-contracts-7bde5f43.json) retains per-mode rows; these passes do not close the remaining full-suite failures.
