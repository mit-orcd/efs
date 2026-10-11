# Intel devel-intel acceptance — 2026-10-11

Current production build is `7bde5f43` on `devel-intel`; the branch also contains the REPORT owner preparation, committed MKDIR reply, partition readiness and explicit-mode/deep-fixture repairs. This ledger remains in progress. The complete-source matrix is `intel-session-accept-7bde5f43`. Historical candidate verdicts below are retained separately; an incomplete-source run is excluded from acceptance. The [earlier 775d935c acceptance](../archive/status-20261011/intel-deployment-acceptance-775d935c-20261011.md) preserves the original failures and workload evidence.

## Deployment and test contract

Two daemons per physical Intel host use TCP, with 400 GiB quota per daemon. Each host has one `/data1/efs/mnt` client seeded from the opposite host. `.efs` is enabled. Acceptance exercises both direct and buffered storage I/O, concurrent POSIX and peer operations, query publication/policy/failover/load gates, session and guarded-lease cleanup, cold payload recovery, real Valgrind daemon/client tests, sanitizer regressions and all independent live rounds. RDMA acceptance belongs on AMD software RDMA or `orcd-test` hardware IB/RDMA.

## Current review

Snapshot at 11:28 UTC: frozen `e07345c3` passes the normal native target and all 79 additional standalone scripts on both hosts (81 build/native/standalone rows each). Intel2 passes all eight direct/buffered partition variants and both query suites, including enabled `.efs` command/policy/load/failover checks. Its direct full gate remains **FAIL**: core 212/214 has unchanged 30-second deadline failures in depth-100 nesting and concurrent process appends; peer visibility passes 66/66 (337.9 s). All independent later cleanup/restart oracles pass, including drained deletion queues, empty physical inventory, zero quota and rejected late PUTs. Total gate time is 1,083.35 s. [Current source checkpoint](../../results/measure/20261011-intel-devel-intel/current-live-checkpoint-e07345c3.json) preserves the exact revision, source/test hashes, completed rows and pending gate names. Intel1 has started the current live gates after its preceding matrix terminated. Buffered, remaining canonical rounds, supplemental faults and production acceptance remain pending; these partial successes do not establish full acceptance.

A diagnostic ABBA comparison is queued after each host's complete current matrix, before the unchanged supplemental suite. It uses the same normal binaries, original depth-60/depth-100 and concurrent append functions/budgets, peer 800-record append oracle, all five optional POSIX extras with `.efs` enabled, and cold/GC checks. It compares defaults with the existing opt-in 100 ms checkpoint/overlap/no-op bundle; no production setting is changed and no full-matrix failure is waived. Diagnostic completion only serializes fixtures and is not an acceptance marker. The bundle needs complete Intel acceptance before any adoption.

The existing million-file trees are preserved: Intel1 completed exactly 1,000,000 files with exit zero; Intel2 is actively writing the original tree, with 795,608 files observed at 11:28 UTC (17 files/s). No additional million-file tree has been started. Twelve historical notes have been archived; this ledger and unresolved W100–W106/W97 work remain active.

The APPEND candidate similarly reuses the owner group's immediately preceding fresh authority proof. Ordinary session reads, later requests, unmatched/remote owners, global epoch/state checks, local floors, replay identity and transaction checks retain their original behavior. The actual reader regression checks matching/foreign groups, subsequent request freshness, live epoch/state/bitmap decoding, malformed bounds and hard refusal; the prior actual reader fails its redundant-read assertion. Complete native targets pass on Intel1 (147.05 s) and Intel2 (140.07 s), plus the reader, APPEND admission and REPORT regressions. [Exact 172-input proof](../../results/measure/20261011-intel-devel-intel/append-request-proof.json) includes the 171 C/header/dependency inputs and changed Makefile. No live latency improvement or final acceptance is claimed. The previous namespace candidate's 80 native/build/standalone rows pass on both hosts; its waiting live queues were retired before any fixture started and remain NOTRUN on that revision. All live, concurrent, query and fault suites remain required on the combined candidate.

The next namespace candidate reuses the destination group's fresh ReadIndex only within the same CREATE, MKDIR, RMDIR, UNLINK or RENAME request. It removes a duplicate initial quorum read; other groups, REPORT/LINK admission, local epoch-floor refusals and the post-ESTABLISH read remain unchanged. Exact-source full native targets pass on Intel1 (158.09 s) and Intel2 (134.21 s), together with namespace and REPORT regressions. The actual prior admission body fails the extra-read negative control. [Source and native proof](../../results/measure/20261011-intel-devel-intel/namespace-request-proof.json) does not claim a live latency improvement or final acceptance. The preceding corrected candidate passed Intel1 direct full/core 214/214, but its buffered run retained three 30-second timeouts (depth 60, concurrent creates and concurrent process appends). Full final-source direct/buffered reruns remain required.

Harness checks now select direct or buffered mode explicitly for the I/O-counter and lost-publication recovery tests; both modes pass on both hosts. The sparse multi-lane mtime test accepts explicit node addresses for the split-host Intel cluster and passes on both current direct-I/O mounts. Deep-path setup uses directory handles for the ancestors, then performs MKDIR and all leaf operations using the complete absolute path at depth 100. Both hosts pass within the unchanged 30-second limit. The original quadratic-setup timeout remains performance evidence; this harness repair does not establish a latency improvement in ordinary full-path walks. [Mode and deep-path evidence](../../results/measure/20261011-intel-devel-intel/harness-mode-and-deep-proof.json) separates these targeted verdicts from pending buffered mtime/deep and full-suite reruns.

The combined REPORT/MKDIR candidate passes complete native rechecks on Intel1 (148.49 s) and Intel2 (136.29 s), plus all five targeted regressions. MKDIR returns the exact authoritative committed inode/attributes through the existing CREATE result API instead of issuing another GETATTR after success; permission checks, inherited credentials, refusal results and zero cache timeouts remain intact. Both actual old helpers fail their new negative controls. [Native source and regression proof](../../results/measure/20261011-intel-devel-intel/owner-mkdir-native-proof.json) records all 171 matching C/header/dependency hashes and separates provisional private build labels from production identity. Initial credential-mock signature failures are retained; the updated mock preserves the original inheritance and refused-MKDIR assertions. The same C/header/dependency source passes the complete native target and all 78 standalone scripts on both hosts. Intel1 direct sequential cold bytes and physical reclamation pass (275.69 s); complete corrected live acceptance remains pending.

The partition test accepts hard IO or explicit BUSY only with the exact epoch-2 FENCING refusal and independently verifies durable state, old-leader refusal and healed completion. Initial variant results are Intel1 8/8 PASS, Intel2 7/8 PASS; Intel2's cross-parent transaction began before group 2 had a fresh quorum view. The latest test first waits on a readonly group-2 session query within its existing readiness budget. It retains the transaction success and stale-authority assertions. All variants require reruns on the combined candidate.

The proposed REPORT owner preparation change builds and passes the complete native target and three session/admission regressions on both Intel hosts. It issues one durable owner CREATE per REPORT batch while retaining each shard's fresh read, REGISTER, authoritative ESTABLISH and floor validation. The old actual helper fails the new one-CREATE negative control. Live mixed/worker/session/guard and sequential checks remain pending before deployment. An initial native failure referenced a missing immutable evidence file; complete-input rechecks pass (Intel1 144.56 s, Intel2 139.87 s).

Production 7bde5f43 has completed both modes and returned to direct I/O. In each mode and on each host, core is 213/214 (100-level directory deadline failure), extras are 2 PASS / 2 FAIL / 1 unsupported mmap SKIP, and the wired peer suite is 66/66 PASS. Each of the four cold persistence runs passes 27/27. Final runtime identity and 256 immutable size/SHA256 samples per host pass. Intel1 has completed exactly 1,000,000 files; Intel2 is actively creating files in the existing tree, with 690,058 observed at 09:45 UTC.

Intel1's 27 independent gates are terminal: 25 PASS, two FAIL (buffered sequential physical reclamation deadline and W82 partition storage fence returning hard IO while retaining FENCING). Intel2 also finishes 25/27: direct sequential REPORT fsync EIO and the original W82 refusal oracle fail. Both hosts retain 19 PASS / 2 repair-protection GAP in each fault mode. The admission follow-on runs at the original limits. Idle obsolete supplemental queues were retired without interrupting any fixture, retaining NOTRUN records; their remaining scenarios require the final candidate.

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

## Historical candidate observations (not current verdicts)

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
| Full private direct | FAIL before POSIX; admission follow-on active | FAIL, core 213/214, peer 66/66 PASS |
| Full private buffered | FAIL, core 213/214 and peer 66/66 PASS | FAIL, core 213/214 and peer 66/66 PASS |
| Valgrind, both modes | PASS | PASS |
| Independent live gates | 25/27 PASS, two FAIL | 25/27 PASS, two FAIL |
| Repair/protection faults | 19 PASS / 2 GAP per mode | 19 PASS / 2 GAP per mode |
| Production concurrent core, both modes | 213/214, FAIL | 213/214, FAIL |
| Cold persistence, both modes | 27/27 PASS | 27/27 PASS |

Intel1's direct private run reaches the second guarded-lease session admission, then receives explicit `EFS_ERR_BUSY` from ESTABLISH. It stops before the later POSIX tier; those cases are NOTRUN in that attempt. Commit `e9c395c7` retries the unchanged administrative request only on explicit BUSY within the original 60-second command budget. Hard errors, stale epochs and expiration still fail. Regression tests pass locally and on both Intel hosts. Full private reruns in both modes are queued after the current owned fixtures finish. The original failed aggregate remains recorded. All six real query gates passed on both hosts and modes, including exact count/byte convergence and every payload check using the bounded readers; the unnecessary idle query-repeat queues were retired.

Three additional dated Intel repair notes (rename flag refusal, empty xattr names and creation inheritance) are archived after their named current POSIX contracts pass. [Named contract evidence](../../results/measure/20261011-intel-devel-intel/intel-repair-contracts-7bde5f43.json) retains per-mode rows; these passes do not close the remaining full-suite failures.
