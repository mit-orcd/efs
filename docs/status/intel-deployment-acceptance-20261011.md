# Intel devel-intel acceptance — 2026-10-11

Current production build is `7bde5f43`, running TCP/direct I/O. The `devel-intel` branch contains the REPORT owner preparation, committed MKDIR reply, partition-readiness, explicit-mode/deep-fixture, request-local namespace/APPEND proof and ordered APPEND PREPARE repairs. The REGISTER apply-verdict repair passes the real pending-CREATE path; the cleanup admission repairs pass native/sanitizer checks and cold live cleanup, while full Intel1 concurrency acceptance remains unresolved. This ledger remains in progress, retains each prior candidate's failures, and excludes incomplete-source runs from acceptance. The [earlier 775d935c acceptance](../archive/status-20261011/intel-deployment-acceptance-775d935c-20261011.md) preserves the original failures and workload evidence.

## Deployment and test contract

Two daemons per physical Intel host use TCP, with 400 GiB quota per daemon. Each host has one `/data1/efs/mnt` client seeded from the opposite host. `.efs` is enabled. Acceptance exercises both direct and buffered storage I/O, concurrent POSIX and peer operations, query publication/policy/failover/load gates, session and guarded-lease cleanup, cold payload recovery, real Valgrind daemon/client tests, sanitizer regressions and all independent live rounds. RDMA acceptance belongs on AMD software RDMA or `orcd-test` hardware IB/RDMA.

## Current review

The new request-local REGISTER proof candidate passes [both-host complete native and sanitizer validation](../../results/measure/20261011-intel-devel-intel/register-active-proof-native.json): ten PASS rows each, full native 145.71 s / 140.91 s, 172 exact source/Makefile inputs and six actual regression scripts plus real metadata tests. REGISTER's atomic apply result supplies a matching ACTIVE epoch/shard proof; an index/term-matched result can replace its repeated owner query. FENCING idempotent success, unavailable proof reads, unhosted owners and mismatched values retain the independent fresh query. Post-ESTABLISH freshness and monotonic local rejection floors remain required. A registered-but-unestablished participant is explicitly fenced before delayed establishment in the real metadata test. The actual prior consumer fails the redundant-query negative control. Wire and stored command formats are unchanged; a live throughput gain remains unproven.

Commit `b7b8170f` retries only explicit NOT_PRIMARY cleanup refusals within the original 30-second invocation deadline, preserving the request UUID, revoked epoch and shard. The deadline also bounds socket waits and restores an inherited deadline on exit. Transport ambiguity, uncertified BUSY and hard session refusals still stop. The actual cleanup-body regression passes for lease and append cleanup, deadline exhaustion and inherited limits, and fails against the prior body at the cold-retry assertion. Cleanup and routing checks pass normally and with ASan/UBSan on both Intel hosts. Its actual failed matrix is retained: Intel2 direct cold cleanup returned uncertified BUSY; Intel1 concurrent creation and process append timed out. This correction does not address the earlier concurrency performance failures.

Commit `3db4acf6` adds an explicit BUSY retry certificate only to the server's pre-proposal lease-cleanup selection refusal. The client requires zero index/term and that certificate, keeps the same work identity and original deadline, and rejects ordinary BUSY or a submitted-batch reply. The actual server branch and client loop pass normal/ASan/UBSan checks on both Intel hosts; both preceding bodies fail their respective certificate/retry negative controls. Fresh default full acceptance passes both modes on Intel2 and cold cleanup on both hosts. Intel1 direct still fails concurrent create and process append at the unchanged deadlines. Earlier candidate `b7b8170f` fails Intel2 direct cold cleanup on BUSY; that refusal is preserved, not reclassified as success.

The private 100 ms checkpoint/overlap/no-op trial on `3db4acf6` passes Intel2 direct full acceptance and Intel1 concurrent creation, but Intel1 process append still fails its original 30-second deadline. A 1,000 ms checkpoint trial is being prepared with unchanged source, exact normal binaries and all existing workload/deadline oracles. Checkpoint progress is a bounded replay hint; durability and cold recovery remain mandatory. Neither trial qualifies for production until complete acceptance.

The selected configuration requires all 110 main gates, all 30 canonical rounds, 84 supplemental rows and both publication-apply modes on each host, plus complete fault identities, before production rollout. Tests use private storage. Obsolete fixtures finish owned cleanup before their waiting parents are retired.

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


[Historical diagnostic and reboot snapshots](../archive/status-20261011/intel-candidate-history-20261011.md) retain the observation-time scope; [production recovery evidence](../../results/measure/20261011-intel-devel-intel/host-reboot-production-recovery.json) verifies the restored four-daemon service and 256 exact tiny-file payload samples per host.
