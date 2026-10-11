# Intel devel-intel acceptance — 2026-10-11

Current production build is `7bde5f43`, running TCP/direct I/O. The `devel-intel` branch contains the REPORT owner preparation, committed MKDIR reply, partition-readiness, explicit-mode/deep-fixture, request-local namespace/APPEND proof and ordered APPEND PREPARE repairs. The REGISTER apply-verdict repair passes the real pending-CREATE path; the latest request-local ACTIVE-proof repair passes native/sanitizer checks and awaits complete live acceptance. This ledger remains in progress, retains each prior candidate's failures, and excludes incomplete-source runs from acceptance. The [earlier 775d935c acceptance](../archive/status-20261011/intel-deployment-acceptance-775d935c-20261011.md) preserves the original failures and workload evidence.

## Deployment and test contract

Two daemons per physical Intel host use TCP, with 400 GiB quota per daemon. Each host has one `/data1/efs/mnt` client seeded from the opposite host. `.efs` is enabled. Acceptance exercises both direct and buffered storage I/O, concurrent POSIX and peer operations, query publication/policy/failover/load gates, session and guarded-lease cleanup, cold payload recovery, real Valgrind daemon/client tests, sanitizer regressions and all independent live rounds. RDMA acceptance belongs on AMD software RDMA or `orcd-test` hardware IB/RDMA.

## Current review

The new request-local REGISTER proof candidate passes [both-host complete native and sanitizer validation](../../results/measure/20261011-intel-devel-intel/register-active-proof-native.json): ten PASS rows each, full native 145.71 s / 140.91 s, 172 exact source/Makefile inputs and six actual regression scripts plus real metadata tests. REGISTER's atomic apply result supplies a matching ACTIVE epoch/shard proof; an index/term-matched result can replace its repeated owner query. FENCING idempotent success, unavailable proof reads, unhosted owners and mismatched values retain the independent fresh query. Post-ESTABLISH freshness and monotonic local rejection floors remain required. A registered-but-unestablished participant is explicitly fenced before delayed establishment in the real metadata test. The actual prior consumer fails the redundant-query negative control. Wire and stored command formats are unchanged; a live throughput gain remains unproven.

The preceding `f9e77c9f` candidate passes the pending-CREATE adoption/close/reclaim path through real FUSE on both hosts, confirming the REGISTER verdict repair. Intel1 direct core is **213/214**, failing the unchanged 30-second concurrent-create case. Intel2 direct core additionally hits the 75-second many-file deadline and aborted concurrent workers; unexecuted cases are not passes. Intel2 buffered query fails the original greater-than-100 writer throughput oracle with 79 writes; concurrent query readers, publication changes and the enabled `.efs` extras pass. These source-specific failures remain acceptance obligations.

Frozen candidate `5f5fb14d` is comparing default persistence settings with the existing opt-in checkpoint/overlap/no-op bundle before choosing one uniform configuration for the entire direct/buffered matrix. Diagnostic completion cannot waive a failed full gate. The selected configuration still requires complete native/standalone, all 110 main gates, all 30 canonical rounds, 84 supplemental rows and both publication-apply modes on each host before production rollout.

Production remains `7bde5f43` on TCP/direct I/O. The frozen `02c13e0b` candidate passes normal native and 79 standalone scripts, all eight partition variants and both query matrices on both hosts. All four full direct/buffered gates **FAIL before POSIX**: adopted pending CREATE can be statted but UNLINK returns ENOENT. Both direct sequential gates also **FAIL** the unchanged 180-second physical reclamation deadline. [Exact-source failures and remaining gates](../../results/measure/20261011-intel-devel-intel/current-live-failures-02c13e0b.json) retain source/test hashes and actual results; completed verdicts are retained while repaired-source matrices require every independent and fault gate. Child-free diagnostic, supplemental and publication-apply queues were retired before any fixture on the superseded source; their cases remain NOTRUN and required on the next candidate.

Root cause of the pending-CREATE unlink failure: the REGISTER-first caller needs the actual missing-owner result, but `apply_session_cmd` converted NOT_FOUND, BUSY and STALE to success. The fix preserves REGISTER business verdicts in the apply ring while allowing rejected committed REGISTER commands to advance the Raft cursor; IO/NOMEM still retry. The regression now executes both actual production return policies, passes the fixed source and fails actual `02c13e0b`. Metadata tests also verify REGISTER refuses a missing owner before CREATE. [Both-host complete native and sanitizer proof](../../results/measure/20261011-intel-devel-intel/register-verdict-native-proof.json) verifies all 172 source/Makefile inputs, six regression scripts and the real metadata test. All ten rows PASS on each host; full native times are 170.28 s / 144.65 s. Live cleanup, concurrent POSIX and production acceptance remain required; the sequential reclamation failure is not yet attributed or resolved.

Both modes on both hosts pass all three enabled `.efs` extras (stats read, find query, virtual directory contract), plus private mmap visibility. Writable mmap remains an explicit ENODEV SKIP: four PASS, one SKIP, zero FAIL/NOTRUN in each extras run. Seven query flags include RPC, FUSE, failover, load, noexec, policy and extras.

[Actual portal HTTP verification](../../results/measure/20261011-intel-devel-intel/portal-http-proof.json) passes with two physical Intel hosts, four daemons and two mounted clients. It uses one actual poll cycle and an ephemeral HTTP listener; no persistent portal was replaced. Deployment scripts support explicit checkpoint/overlap/no-op settings, but no bundle has been adopted. [Configuration proof](../../results/measure/20261011-intel-devel-intel/start-config-forwarding-proof.json) retains the actual process/driver checks.

The original tiny-file trees remain intact. At 14:36 UTC both hosts have completed exactly 1,000,000 files in their original trees, each with exit 0: [two million files in total](../../results/measure/20261011-intel-devel-intel/original-tiny-jobs-complete.json). No replacement million-file tree was started. [Historical candidate snapshots](../archive/status-20261011/intel-candidate-history-20261011.md) preserve earlier failed deadlines, native successes, partial coverage and prior deployment observations. No failed gate is waived.

A [historical fault coverage correction](../../results/measure/20261011-intel-devel-intel/e073-fault-coverage-correction.json) verifies the actual E073 outputs: each host and mode has 14 integration PASS, one repair GAP and one simulator NOTRUN because `fault_sim` was not built. Any earlier 19 PASS / two GAP aggregate attributed to E073 is incorrect. The current runner explicitly builds the simulator after native compilation and before fault suites. Complete fault coverage requires all 21 distinct layer/case/node/fragment identities, 19 PASS and the two specifically named repair GAPs; missing simulator output cannot satisfy it.

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


The comparison harness initially read a daemon environment before exec and pre-created the query output directory. Both failed attempts are retained separately; corrected diagnostics read configuration after listening/exec and let the unchanged query script create its output. No test budget or workload is relaxed. Complete candidate acceptance remains pending.

## Host reboot interruption — 16:12 UTC

Both Intel hosts rebooted shortly before this checkpoint. Production daemons, mounted clients and the waiting/current private runners exited. The installed production source remains `7bde5f43`; its four daemons were relaunched with the existing TCP/direct, 400 GiB configuration. The restore completes at 16:14 UTC: all four daemons are ready, both opposite-host mounts are present, `.efs` is enabled and installed/live binary identity and four 400 GiB quotas pass. [Production recovery evidence](../../results/measure/20261011-intel-devel-intel/host-reboot-production-recovery.json) also verifies exact preserved sizes and SHA256 for 256 tiny-file samples per host. No candidate production rollout occurred.

The diagnostic retained Intel1 eight completed rows (seven aggregate passes and one observer-only failure after the underlying query passed), and Intel2 six completed passing rows. Intel2 buffered third cohort passed cold reclamation but its query and fourth row lack terminal results; the corrected Intel1 observer rerun also lacks a terminal result. These host-interrupted attempts are not passes. The complete 110/30/84/2 matrix still awaits configuration selection and launch/resume.
