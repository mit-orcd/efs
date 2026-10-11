# Intel devel-intel acceptance — 2026-10-11

Current production build is `7bde5f43`, running TCP/direct I/O. The `devel-intel` branch contains the REPORT owner preparation, committed MKDIR reply, partition-readiness, explicit-mode/deep-fixture, request-local namespace/APPEND proof and ordered APPEND PREPARE repairs. The latest REGISTER apply-verdict repair is under native validation after the frozen candidate failed live cleanup. This ledger remains in progress, retains each prior candidate's failures, and excludes incomplete-source runs from acceptance. The [earlier 775d935c acceptance](../archive/status-20261011/intel-deployment-acceptance-775d935c-20261011.md) preserves the original failures and workload evidence.

## Deployment and test contract

Two daemons per physical Intel host use TCP, with 400 GiB quota per daemon. Each host has one `/data1/efs/mnt` client seeded from the opposite host. `.efs` is enabled. Acceptance exercises both direct and buffered storage I/O, concurrent POSIX and peer operations, query publication/policy/failover/load gates, session and guarded-lease cleanup, cold payload recovery, real Valgrind daemon/client tests, sanitizer regressions and all independent live rounds. RDMA acceptance belongs on AMD software RDMA or `orcd-test` hardware IB/RDMA.

## Current review

Production remains `7bde5f43` on TCP/direct I/O. The frozen `02c13e0b` candidate passes normal native and 79 standalone scripts, all eight partition variants and both query matrices on both hosts. All four full direct/buffered gates **FAIL before POSIX**: adopted pending CREATE can be statted but UNLINK returns ENOENT. Both direct sequential gates also **FAIL** the unchanged 180-second physical reclamation deadline. [Exact-source failures and remaining gates](../../results/measure/20261011-intel-devel-intel/current-live-failures-02c13e0b.json) retain source/test hashes and actual results; main matrices continue through independent and fault gates. Child-free diagnostic, supplemental and publication-apply queues were retired before any fixture on the superseded source; their cases remain NOTRUN and required on the next candidate.

Root cause of the pending-CREATE unlink failure: the REGISTER-first caller needs the actual missing-owner result, but `apply_session_cmd` converted NOT_FOUND, BUSY and STALE to success. The fix preserves REGISTER business verdicts in the apply ring while allowing rejected committed REGISTER commands to advance the Raft cursor; IO/NOMEM still retry. The regression now executes both actual production return policies, passes the fixed source and fails actual `02c13e0b`. Metadata tests also verify REGISTER refuses a missing owner before CREATE. [Both-host complete native and sanitizer proof](../../results/measure/20261011-intel-devel-intel/register-verdict-native-proof.json) verifies all 172 source/Makefile inputs, six regression scripts and the real metadata test. All ten rows PASS on each host; full native times are 170.28 s / 144.65 s. Live cleanup, concurrent POSIX and production acceptance remain required; the sequential reclamation failure is not yet attributed or resolved.

Both modes on both hosts pass all three enabled `.efs` extras (stats read, find query, virtual directory contract), plus private mmap visibility. Writable mmap remains an explicit ENODEV SKIP: four PASS, one SKIP, zero FAIL/NOTRUN in each extras run. Seven query flags include RPC, FUSE, failover, load, noexec, policy and extras.

[Actual portal HTTP verification](../../results/measure/20261011-intel-devel-intel/portal-http-proof.json) passes with two physical Intel hosts, four daemons and two mounted clients. It uses one actual poll cycle and an ephemeral HTTP listener; no persistent portal was replaced. Deployment scripts support explicit checkpoint/overlap/no-op settings, but no bundle has been adopted. [Configuration proof](../../results/measure/20261011-intel-devel-intel/start-config-forwarding-proof.json) retains the actual process/driver checks.

The original tiny-file trees remain intact. At 13:26 UTC Intel1 has completed exactly 1,000,000 files with exit 0; Intel2 has created 921,110 of its original million-file target and continues writing. No replacement million-file tree was started. [Historical candidate snapshots](../archive/status-20261011/intel-candidate-history-20261011.md) preserve earlier failed deadlines, native successes, partial coverage and prior deployment observations. No failed gate is waived.

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

## Historical candidate observations

[Retained candidate history](../archive/status-20261011/intel-candidate-history-20261011.md) preserves the original failures, partial coverage, retry/inventory observations and cold recovery limits. These observations do not replace current production or final-candidate verdicts.

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
