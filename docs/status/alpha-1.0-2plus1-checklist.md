# Phase 1 — alpha 1.0: finish 2+1 on 3, 4 and larger clusters

**User-directed release scope, October 22, 2026. Status: OPEN; no release
candidate or topology acceptance recorded by this checklist.**

Phase 1 finishes and qualifies the existing **two data fragments plus one parity
fragment** architecture at every supported cluster size. Completion is the
**alpha 1.0** milestone. Only afterwards do we continue the remaining
[distributed-cluster roadmap](distributed-cluster-roadmap-20261022.md).
This is release sequencing, not a claim that the final D36 design is implemented.
No tag, merge, push, deployment or data deletion is performed by this document.

**Decision (October 22):** the user keeps >4-node clusters in alpha 1.0;
2+2 and broader metadata distribution follow alpha 1.0. The four-node cap is
therefore alpha work (K28/W151), a prerequisite of A3.

**Review (October 22, devel `165689c06`):**
[plan and code review](alpha-1.0-plan-review-20261022.md). Main finding: the
code caps a cluster at four nodes (`EFS_MAX_NODES`, voter sets, placement,
client hint checks), so the >4-node column needs an implementation item or a
narrower claim; also a K27 for the ior write regression, the four-node
packing/metadata placement facts for F4, and `EFS_MD_RAFT_N` as a derived
fact under A8.

## Order of work (user direction, October 22)

Alpha 1.0 is reached in three stages, in this order:

| Stage | Goal | Sections | Done when |
| --- | --- | --- | --- |
| 1. Known bugs | Fix or decide every known defect in the alpha scope | **K** | Every K row is CLOSED (fixed with a regression test and its gate), VERIFIED on the candidate, or DECIDED out of scope by the user with the limitation published |
| 2. Functional completeness | One supported behavior and every in-scope feature working on 3, 4 and >4 hosts | **A**, **S**, **B**, **C**, **D**, **E1–E4** | All items PASS on the frozen candidate |
| 3. Performance | Several measure–improve rounds inside the alpha scope; no change that belongs to the post-alpha roadmap | **P** | Rounds stop when the user calls the result acceptable or the remaining headroom needs post-alpha work; Stage 2 gates rerun after each round |

**F** closes the release after all three stages.
- **Stage 1 comes first.** Qualification (Stage 2) does not start while a K
  row is open, because a run that never hits a known bug does not show it is
  fixed.
- **New bugs block.** A bug found in Stage 2 or 3 becomes a new K row and
  blocks again.
- **Performance comes last.** No performance work is done before Stage 3,
  except where a correctness gate needs it (bounded progress, D4).
  Performance changes never relax correctness or durability checks.
- **Stage 3 stays inside alpha.** A performance change that needs 2+2, more
  or redistributed metadata groups, leader or voter redistribution (W66),
  all-node query service (W98), the directory-spread policy (W81), repair
  (W74) or any new on-disk/wire profile is post-alpha work, however large the
  measured gain. It is recorded as a finding with its measurement and left
  for the roadmap. (User direction, October 22.)

## Scope and meaning of done

| Cluster | Phase-1 stripe | What extra nodes must do | Explicit limitation |
| --- | --- | --- | --- |
| 3 nodes | D1, D2, P on three distinct nodes | All three participate in data placement | No spare node is implied; code tolerates one missing fragment for reconstruction |
| 4 nodes | Still D1, D2, P on three distinct nodes per stripe | Placement rotates across all four; the fourth is not a fixed idle spare | **Not 2+2; no two-node-loss protection claim** |
| More than 4 | Still three distinct nodes per stripe | Independent stripes use different subsets across the supported node set | More capacity/work distribution, not stronger per-stripe protection |

Metadata placement follows [D38](decisions.md): in alpha 1.0 every metadata
group has three voters (one loss). For three nodes, both groups use nodes
1–3; for four or more, the default groups use 1–3 and 2–4 (amended Oct 22,
T3). Data placement uses all nodes. Later alphas widen metadata
placement (2.0: 2+2 on four nodes; 3.0/4.0: N − M configurable). Packed
segments are data and follow the data ring across all N nodes in alpha 1.0
(D38, decided Oct 22; W151 sub-item).

This milestone completes current-path correctness, durability and bounded
progress. It does not require the later many-group metadata architecture, a new
2+2 codec, linear throughput scaling or elimination of all local serialization.
Current metadata placement and packed-segment voter restrictions must be measured
and disclosed. A placement restriction must not silently strand capacity or
misreport usable space; correct it or explicitly narrow the alpha support claim.

Unplanned power-loss recovery and automatic repair/rebuild remain deferred under
the existing scope. Controlled missing-fragment decode tests below establish
2+1 reconstruction only; they do not establish whole-system D36 preservation,
metadata quorum availability, or automatic repair. Keep those later gates open.

## K. Stage 1: known defects in the alpha scope

The register is built from the [work queue](work-queue.md) and this session's
findings, as of October 22. Status text there may be older than devel. Each
row is checked again on current devel before work starts.
- **FIX:** an open bug. Fix it, add a regression test, and pass the item's
  gate.
- **VERIFY:** the fix is in devel, but its gate is not recorded on the alpha
  profile (RDMA, direct I/O, `.efs` and packing on, distinct hosts). Run the
  gate on the candidate.
- **DECIDE:** a scope question for the user. Either fix it, or record it as
  an alpha limitation (F4).
- **TEST:** a harness or reporting defect. Fix it before relying on that
  test's verdict.

| ID | Item | Kind | Disposition |
| --- | --- | --- | --- |
| K1 | **W145** remaining parts: acknowledged-write check after the deposed-leader fix, snapshot sent to a near follower, restart election storms, `stop.sh` with a blocked handler | Raft safety / liveness | FIXED, merged Oct 22 (`86b6ef567`): 30,929,219@62 was never acknowledged; a probe ACK no longer lowers `next_index` and conflict hints skip the term run; pre-vote ends restart election storms; efsd stops are bounded (40 s drain). VERIFY: orcd-mini gates NOTRUN **REOPENED Oct 22 (R4): the 40 s drain bounds handlers only; after it `efsd` still stops the packer, writer pool, PUT sync, reclaim, Raft and peer pool with unbounded joins, and `scripts/server_processes.py retire` treats the `_exit(3)` forced exit as a successful stop. FIX: a total shutdown deadline across phases, forced exit reported as such, stuck-worker test**. **R4 FIXED, merged Oct 22 (`9fd38ffb4`): one 50 s deadline from the end of the accept loop covers the handler drain (four fifths) and every stop phase, run on a teardown thread; a stuck phase ends in a forced exit (rc 3) naming the phase and worker with stores preserved; the exit record `log/efsd.exit` states the outcome and the PUT-sync, reclaim-journal and usage-flush barriers (rc 4 when one failed); `server_processes.py` and `server.sh stop` exit 0 only for an orderly stop and a start refuses after a non-orderly predecessor; stuck-worker test on an `EFS_FAULTS` daemon; `make test` PASS on efs3 ([evidence](k01b-r4-shutdown-deadline-20261022.md)). NOTRUN: `stop.sh` qualification on orcd-mini; the devops `stop.sh` patch is not applied.**  **Third review at `0eb1511d2`: code gap OPEN — T2: forced-exit reporting is outside the effective deadline; see [review](devel-integration-review-20261022.md#third-pass--merged-fixes-at-0eb1511d2).** **T2 FIXED, merged Oct 23 (`c083b98b5`; `de98832b7`): after the deadline the exit record and stderr lines are best effort on helper threads with a 5 s bound before `_exit(3)`, the deadline counts from the stop signal with the recorders inside it (signal to exit at most 55 s), and blocked-record and blocked-stderr tests pass on intel1 and fail on the pre-fix daemon ([evidence](k01-t2-forced-exit-bound-20261022.md)).** |
| K2 | **W140** proposal RPC accepts metadata tags this build cannot apply (halts a group) | safety | FIXED, merged Oct 22 (`a5eb7e6ff`): one allow-list (`EFS_MD_CMD_APPLIED`) drives the dispatcher and every proposal path; unknown tags refused PROTO at submit; drift test. Oct 24: covers 90/91/92/250; a TXN_BATCH with an inner tag the apply halts on is now refused too ([evidence](test-hygiene-k32-w149-w140-20261024.md), merged into devel `e7900d8a7`); **Oct 25 Task C: private three-member TCP loopback cluster gate PASS** (both groups, follower redirect, no added entry, subsequent CREATE; [evidence](task-c-20261025.md)); branch `agent/devel-task-c`, not merged; overall decoder qualification pending |
| K3 | **W132** `NODE_LEFT` from any connection drops a node | safety | FIXED, merged Oct 22 (`1fad577e2`): message 24 retired and refused fail-closed; handler, struct and remover deleted |
| K4 | **W133** client protocol declaration gate is fail-open | correctness | FIXED, merged Oct 22 (`918ed9207`): every frame but CLIENT_HELLO and HEARTBEAT needs an accepted declaration; exact protocol version (4 after K5); peers, mgmt, bench and live tests declare. VERIFY: `w125_p3_compat.sh` on hardware NOTRUN |
| K5 | **W134** unregistered session identities bypass fencing (with **W72** production session lifecycle) | correctness | FIXED, merged into devel Oct 22 (`a443863b1`): one session check at every apply (`efs_session_accept`: UNREGISTERED or FENCED, no legacy acceptance, `efs_session_accept_legacy` deleted); the mount establishes a session, admits each touched shard and latches fenced; every metadata mutation carries the session (protocol 4); `efs-mgmt` raw mutations establish their own. VERIFY: the fence cluster gate (`scripts/k05_session_fence_gate.sh`) and the NUC/AMD live harnesses are NOTRUN for the parent session; partitioned-client and leader-change barrier gates stay W72 work **REOPENED Oct 22 (S2 P1, S3 P2): `efs_meta_execute_mtime_owned` (`src/meta/append_plan.c`) checks the session, then reads the session row again and uses that unverified image as the CAS base, so a fence landing between the two reads lets the old epoch's mtime transaction through (wrapper counterexample `scripts/reviews/k05_mtime_fence_capture.py`); and the client's `session_fenced`/`session_started` latches mix plain writes with atomic reads. FIX: validate epoch and floor from the captured image before preparing it (as the append path does); make the latch accesses consistently atomic; TSan test**. **S2 and S3 FIXED, merged into devel Oct 22 (`3bcde5e2a`): `468513a96` authorizes every session-owned executor from its captured row (mtime, APPEND reservation/completion, barrier retirement, `efs_session_prepare_owner`); `42c631852` makes both latches atomic. Barrier test with real session rows, the review reproducer and a TSan check/fence harness (with negative control) run in `make test`; `make test` PASS on efs1. Fence cluster gate NOTRUN. [Evidence](k05-s2-s3-20261022.md)**  **Third review at `0eb1511d2`: code gap OPEN — T1: append-abandon captures a fenced session row without validating it; see [review](devel-integration-review-20261022.md#third-pass--merged-fixes-at-0eb1511d2).** T1 FIXED, merged Oct 23 (`9a88e332e`): abandon authorizes from the captured session row; barrier test PASS, `make test` on amd PASS with three pre-existing host exclusions, fence cluster gate NOTRUN. [Evidence](k05-t1-abandon-capture-20261022.md) |
| K6 | **W127–W131** fail-closed apply (in devel since `08cc16af4`; private RDMA gate PASS) | correctness | VERIFY multi-host |
| K7 | **W83** transaction decision records are never retired | lifecycle / growth | FIXED, merged Oct 22 (`907a5891f`): replicated `TXN_RETIRE` compacts decisions to tombstones and reclaims them after a 60 s retention from a per-shard cursor; unknown decisions answer STALE (readers retry), never the old value; a sweeper re-drives orphaned decisions. Note: retention must stay above the 5 s recovery age and clock skew (no assert yet) **REOPENED Oct 22 (R2 P2, R3 P2): `efs_txn_scan_decisions` always restarts at the prefix with a 32-record page, so 32 unresolvable live decisions starve every later one; the extended DECIDE and TXN_RETIRE decoders accept trailing bytes (`clen <` minimum instead of exact). FIX: per-shard continuation with wraparound for the live-decision sweep; exact lengths and field validation, malformed entries halt the group through the unsupported-entry path; tests through the real apply**. **R2 and R3 FIXED, merged Oct 22 (`85d8dd438`): per-shard continuation cursor with wraparound (`HOST_DEC_SCAN_WORK` 256 keys per pass; the recovery mark stays armed until a full clean page), exact DECIDE/TXN_RETIRE lengths with shard and verdict validation (malformed entries halt through the unsupported-entry path), retention ≥ 10× the recovery age asserted; tests run through the real apply in `make test`. Follow-up noted by the session: the RESOLVE, DROP and PREPARE decoders still accept a minimum length, not an exact one.** |
| K8 | **W148** modify-under-load returned wrong bytes, correct size, on a never-packed 3.6 MB file | data correctness | FIXED, merged Oct 22 (`71ce4ff35` on devel). Root cause: `dcache_apply_init` let a partial write with a loaded base keep the slot's UNCOND base, so the flush published the write-time image (from an unrefreshed local chunk entry: zeros or a peer's lost bytes) as the whole chunk with no merge and no CAS. Reproduced 3× in 9 attempts on node9901 (RDMA, devel `7c510c60f`); a cold read from a fresh client returned the same bad bytes. The two-client regression (`tests/posix/posix_2client.py`) fails 3/3 before and passes 3/3 after. VERIFY: the w125 modify rerun on the fixed build is NOTRUN; the kept W148 stores and hashes are in the session hand-over ([review](review-pack-validate-once-20261022.md)) |
| K9 | **W87** buffered concurrent append loses records under load | data correctness | **CLOSED without resolution (user, Oct 24, [D41](decisions.md)): never reproduced, no root cause, no fix; reopens if a record loss is seen again.** Was: **INCOMPLETE** (Oct 22): not reproduced (23 oracle rounds, 19,597 records, churn/GC/POSIX load on efs2; VM run, not formal evidence; [attempt](w87-append-oracle-efs2-20261022.md)); no root cause, no fix. The exact oracle (`tests/live/append_oracle.py`, `append_oracle_cluster.sh`) and `tests/test_append_oracle.py` are in devel for the parent session to run on the real clusters. **Xorinox (Oct 23, merged Oct 23 (`306a01fd4`)): not reproduced in 110 oracle rounds (84,480 acknowledged records, 27 with three-daemon restart) under churn, GC and POSIX load; all exact, no write errors; W87 stays OPEN ([evidence](gates-xorinox-20261022.md#k9--w87-concurrent-o_append-oracle)).** |
| K10 | **W121** a read right after a successful fsync returns the previous file's payload (FUSE kernel fork/page-pinning; kernel fix deferred) | data correctness | DECIDED Oct 23 (D40): known limitation in alpha 1.0, documented with its trigger in the release notes (F4); no client workaround. Was DECIDE |
| K11 | **W55** span committed but fragment PUTs never landed | data correctness | VERIFY Linux FUSE. NFS re-export and macOS clients are **DECIDED out of alpha** (user, Oct 22): beta backlog **W152**; their W55 triggers are not isolated in alpha |
| K12 | **W58** `O_EXCL` create answered EEXIST (historical trigger not isolated) | correctness | VERIFY |
| K13 | **W88** a folded span retry resurrects after history eviction; **W86** default PUT/flush ticket integration staged | publication correctness | FIX (decided Oct 23, D40): finish the PUT/flush ticket integration W86 and the W88 replay for alpha; W137 stays required. Was FIX or DECIDE . **FIXED on branch `devel-k13` (`c860c3dbd`; devel merged in `a79b59de1`, `make test-all` PASS), not merged (Oct 23, [evidence](k13-w86-ticket-default-20261023.md)): the ticketed PUT/REPORT is the only data path, W88 and W137 pass, `make test` and a private three-daemon gate pass on intel1; the D31 abandoned-upload collector is INCOMPLETE**. **D31 collector of settled abandoned uploads PASS (unit and in-process) on branch `devel-k13d31` (`1a8da8af0`), not merged; cluster gate NOTRUN and dead clients' unsettled tickets still uncollected ([evidence](k13-d31-collection-20261023.md)).** **Oct 24: dead and departing clients closed, merged into devel (`e6a21692d`; tested at `59a2308b8`): revoked-stream drain into the orphan ledger, unmount revokes its own session, RECLAIMING path removed; unit and in-process PASS, amd `make test-all` FAILs are host limits shared with the base; live collection gates NOTRUN ([evidence](k13-d31-drain-20261024.md)).** |
| K14 | **W77** atomic multi-chunk publication and observation | integration gap | FIX (decided Oct 23, D40): atomic multi-chunk publication and observation in alpha 1.0. Was FIX or DECIDE. FIXED merged into devel Oct 23 (`d2369a49d`): each multi-chunk write publishes as one cross-group transaction (staged per-chunk PREPAREs, one decision, truncate pin) and multi-chunk reads use one validated snapshot; unit tests, `make test` and the intel2 three-daemon gate PASS (0 torn of 329,530 reads across leader changes, a writer-client kill, undecided, committed and aborted units; the same workload tore 15.8 % of reads on devel), RDMA, orcd-mini and multi-host NOTRUN ([evidence](k14-w77-atomic-publication-20261023.md)). |
| K15 | **W64** append reservation replay across leader changes; **W52** REPORT after many `O_APPEND` writes exceeds 30 s | correctness | **INCOMPLETE** (merged Oct 22). W52: shape A was already in tree (`32b472c5b`); the live gate harness `tests/live/append_failover.py` is in devel; the 20k gate and the before/after A/B are NOTRUN. W64: a coordinator that lost its group mid-transaction left PREPAREs until the 5 s recovery scan (writes failed EBUSY); the finisher now re-proposes ABORT at the new leader (`test_fin_abort_handoff.py`), `efs-mgmt raft-step-down` added; the live failover gate is NOTRUN. Lost-reply cleanup merged Oct 22 (`a51832413`): a reservation granted with a lost reply retires decide-once, by session fence (FENCED_HOLE) or the live owner's abandon of that identity (receipt STALE, ABORTED_HOLE); late completion refused, completion never overtaken (`test_append_cleanup`); the lost-reply live gate (`append_failover.py --lose-reserve-every`) is NOTRUN. [Evidence](k15-w52-w64-20261022.md) Live gates on intel2, Oct 23, devel `163241584`: lost-reply gate FAIL (integrity held in every view and the 4 injected losses retired cleanly, but a load stall with no leader change caused 25 extra failed writes, 2 BUSY abandons and 1 extra hole), W52 20k FAIL indication (26 REPORT receive timeouts after 2,120 of 20,000 records, interrupted), failover gates, second runs and A/B NOTRUN ([live gates](k15-live-gates-20261022.md)). T1 FIXED, merged Oct 23 (`9a88e332e`): append-abandon authorizes from the captured session row (`session_owner_capture`), fenced or unregistered owners refused before any prepare; barrier test with three interleavings PASS. **Oct 24 (branch merged into devel (`a2326a369`)): accounting rule in `append_failover.py`: a failure the client saw, whose record is absent and whose hole a matching abandon explains, is accounted and reported separately; a lost acknowledged record or an unexplained hole stays a mismatch (`tests/test_append_failover_accounting.py` in `make test`, 9 negative controls). The harness now starts efsd with the K33 CLI. amd `make test-all` fails only what the base fails. Live gates NOTRUN ([evidence](k15-k29-accounting-rule-20261024.md)).** **Oct 24 (A11 finding 2, merged into devel (`1fee9bb04`; tested at `6654d5f15`)): the nuc loopback "30 s REPORT timeout" was an 8 s BUSY stall (`recv_ms` printed the bound): a DECIDE COMMIT lost to a step-down left its PREPAREs to the 5 s recovery scan. FIXED: such a DECIDE (and a failed DROP) goes to the finisher as ABORT at the new leader; a REPORT now completes within 3 s per leader change crossed. `tests/test_report_stepdown.py` (fault `EFS_FAULT_DECIDE_LOST`) 5 of 5 PASS, control FAIL; `append_failover.py` step-down reruns on amd 5 of 5 PASS, 0 REPORT timeouts, slowest REPORT 1.8 s; the harness now checks REPORT latency per leader change ([evidence](a11-f2-report-stepdown-20261024.md)). The 20k W52 gate and multi-host/RDMA failover gates stay NOTRUN.** |
| K16 | **W75** identity-bound fragment integrity format (fresh format, no compatibility) | integrity | FIXED, merged Oct 22 (`e9ca0d07b`): fragment record format 2 binds identity and payload digest; GET refuses as INTEGRITY, GC and segments verify identity; old roots refuse startup. **Fresh format required at the next deploy.** VERIFY: RDMA and hardware gates NOTRUN ([evidence](k16-w75-identity-format-20261022.md)) |
| K17 | **0a/D27** STALE replay convergence: orcd-mini C1 rerun | correctness | VERIFY |
| K18 | **W54**, **W38** fold/GC data loss: multi-host hardware RDMA gate | correctness | VERIFY |
| K19 | **W61**, **W62**, **W63** GC: final drain and lost-ledger reconciliation | reclaim correctness | VERIFY |
| K20 | **W42** capacity reporting on any node count; protection debt (with B5) | correctness | VERIFY. **Oct 24 (B5, branch merged into devel (`5a09fd938`)): the open one-QUOTA-member question is answered and FIXED: a full member no longer creates silent protection debt (placed around, or ENOSPC at three); status/df equal the 3-of-N bound at 3 to 8 loopback members ([evidence](b-geometry-capacity-20261024.md)). Named real-host capacity acceptance NOTRUN.** |
| K21 | **W119** concurrent RDMA failures and blocked clean stop (recorded Oct 15, before many fixes); **W65** graceful-shutdown wait | correctness / liveness | VERIFY first, then FIX |
| K22 | **W23** universal follower memory/lag bound | resource bound | FIXED, merged into devel Oct 22 (derived KV memory budget, unapplied-log bound with busy AppendEntries replies, leader admission at 3/4, leadership requires capacity per D30; `efs-mgmt raft-status` shows the bound). VERIFY: the production-scale plateau measurement, snapshot import into a stalled follower and multi-host/RDMA runs are NOTRUN for the parent session ([summary](../../results/measure/20261022-k22-follower-bound/SUMMARY.md)) **REOPENED Oct 22 (S1 P1): `low_capacity` counts `last_index − last_applied`, which includes entries not yet known committed; after a leader loss or a restart with a retained tail every voter can be vetoed from campaigning, nothing clears it, and no messages are sent (reproduced: `scripts/reviews/k22_capacity_tail.c`, three voters, 96-entry tails, cap 128, zero messages in 1,000 ticks). FIX: distinguish an uncommitted tail that needs an election from inability to apply; allow recovery leadership with bounded headroom while admission stays held; a delivering three-voter test that elects, commits the tail and resumes**. **S1 FIXED, merged Oct 22 (`001b5da30`): `low_capacity` counts only `commit_index − last_applied`, so an uncommitted tail never vetoes a campaign; `capacity_hold` retries `apply_committed`; the three-voter delivering test in `test_follower_bound` elects, commits a 96-entry tail and resumes; `make test` PASS on an efs VM.** **Live on xorinox (Oct 23, merged Oct 23 (`306a01fd4`)): PASS (scoped) for a 302 s plateau at fault-build caps (32,768 entries, 8 MiB budget, follower RSS flat at about 100 MB, quorum commit rate unchanged) and for SIGSTOP stall and AppendEntries catch-up; NOTRUN: the production-scale plateau (about 6 h here), snapshot import (needs a 1 GiB log lag) and live leader admission at 3/4 ([evidence](gates-xorinox-20261022.md#k22--w23-follower-memory-and-lag-bound)).** **Loopback gates Oct 25 (`agent/devel-taskb`, `82ad8364e`): snapshot import into a stalled follower PASS (INSTALLED 4 s after resume, caught up 2 s later, RSS 2.1x warm, byte-exact read-back through the imported replica), live leader admission at 3/4 PASS (host admission counter climbs, capacity stepdown moves leadership, 3 s drain, 443 client writes with zero errors after the move); defects found and fixed on the branch: send_snap retry storm, raft-resync healing a W127-halted replica; opened W162/W163. Still NOTRUN: production-scale plateau and 1 GiB lag on real hosts (driver scripts/k22_snapshot_import.sh), RDMA/multi-host ([evidence](k22-k6-loopback-taskb-20261025.md)).** |
| K23 | **W71** hardware power-loss gate; **W74** automatic repair; **W123** crash recovery | durability scope | DECIDED Oct 23 (D40): W71, W74 and W123 stay deferred past alpha 1.0; W154 rebuild-on-return is the only repair in alpha. Was DECIDE (deferred by current scope) |
| K24 | **W149** `EFS_KV_COMPACT_DIE` exits the daemon in normal builds | build hygiene / safety | FIXED, merged Oct 22 (`19555889f`): the hook exists only in `EFS_FAULTS` builds, one gate form everywhere, crash-recovery test moved to `tests/faults`. Oct 24: normal builds name each fault variable as ignored at startup ([evidence](test-hygiene-k32-w149-w140-20261024.md)) |
| K25 | **W135**–**W139**, **W141**, **W120**, **W150** test harness and reporting defects | test correctness | FIXED, merged Oct 22 (`35647dd0f`): docs gate out of `make test` (`make check` runs both), every unit binary and Python test wired or listed with a reason, TSan gate runs or fails, NOTRUN counted by the summary runner, condition-based waits, W150 test added. W137 waits on K13 **REOPENED Oct 22 (R1 P1): `scripts/make_test_summary.sh`'s runner never exits non-zero on failures or NOTRUN, so automation reads a failed campaign as success (reproduced). FIX: failing exit status on any failure, a distinct incomplete status for required NOTRUN gates, runner self-tests; also make every test honour `TMPDIR`**. **R1 FIXED, merged Oct 22 (`6df08cc3a`;** [evidence](k25b-summary-exit-tmpdir-20261022.md)**):** runner exit 0 PASS / 1 FAIL / 3 INCOMPLETE with self-tests, test scratch under `TMPDIR` and removed on success; `make test` PASS twice on efs2 |
| K26 | LeakSanitizer aborts on node9900/node9901; F1 needs a working leak check. On the efs1–3 build VMs (Fedora 42, aarch64, 1 GB RAM, tmpfs `/tmp`): `test_reclaim` and `tests/test_server_processes.py` failed on pristine devel and on every K branch. Cause found Oct 22: efsd's capacity headroom rule (`storage_capacity.h`, 20% free) refuses the 972 MB tmpfs `/tmp` and the 88%-full root disk ("Storage quota: cannot establish safe headroom; refusing startup"); both PASS with `TMPDIR=/data1/tmp` (the 20 GB XFS data disk), the full documented toolchain was applied on all three on Oct 22 (kickstart package set, devops build deps, `devops/profiling/HOST-SETUP.md`: perf, bpftrace, sysstat, fio, iperf3, blktrace, numactl, trace-cmd, dwarves, gdb, valgrind, sanitizer runtimes, glibc debuginfo, profiling sysctls, memlock, perf group with tracefs, cache-drop helper); Tart VMs expose no hardware PMU, so `perf` samples `cpu-clock`, not `cycles` (the W138 TSan gate failed without `libtsan`), and the root disks run full (old build trees under `~/build` must be deleted). Tests that hard-code `/tmp` instead of honouring `TMPDIR` (for example `test_query_disk`) leave stale `efs-*` directories that fill the tmpfs (857 found on efs3 on Oct 22); making every test honour `TMPDIR` and clean up is a K25-class harness item. The merged devel of Oct 22 was otherwise clean on efs1 | environment | **Leak gate DONE on `devel-k26` (`2fea76916`), merged Oct 22 (`b8ef49e58`) ([evidence](k26-leak-gate-20261022.md)):** `make leak-check` runs 25 unit binaries (the four ASan-only tests plus KV, Raft, store, txn, session, lock) under valgrind memcheck. It prints PASS/FAIL per binary and a verdict, exits 1 on any definite leak and 3 (`NOTRUN: valgrind missing`) without valgrind, and `make check` runs it. **PASS on efs2 and efs1 (25/25, about 6 min each); `make test` PASS (185 s).** No product leak; two test defects fixed or isolated. LSan works on efs1–3 (`ptrace_scope` 0) and fails only where `ptrace_scope` is 2 (ORCD). NOTRUN: `make leak-check` on node9901 (x86_64). Open: W126 peeks wait on `mu` while a writer is queued on `view_rw` |
| K27 | ior easy write on `d94ca85bf` is 710–863 MiB/s against 1,329 MiB/s on `08cc16af4`, same cluster, three runs, reads unchanged ([deploy report](orcd-mini-deploy-d94ca85bf-20261022.md)) | regression, unattributed | VERIFY (A/B/A′ on orcd-mini, needs a restart) **Oct 23 A/B/A′ attempt, fresh format per phase (orcd-mini wiped with the user's OK): phase A `08cc16af4` ior easy write 1,239.75 / 1,147.33 / 1,137.65 MiB/s, read 3,899 / 4,975 / 4,409 MiB/s (8 ranks, 16 GiB). Phase B on devel `bf16436e8` could not run: RDMA client replies are lost (K31). Phase A′ NOTRUN. Raw ior outputs on node9900 under `/data1/efs/orcd-mini-client/prof/k27/A-08cc16af4/` (sha256 `01a9d83e…cb28`, `cdbd2015…9db4`, `a5da3ad5…0a`).** **CLOSED Oct 23: no write regression on fresh formats; A `08cc16af4` 1,175, B devel `24e58fb1d` 1,242, A′ `08cc16af4` 1,282 MiB/s mean ior easy write; the Oct 22 drop came from the aged cluster ([evidence](k31-k27-orcd-mini-20261023.md)).** |
| K28 | **W151** the code caps a cluster at four nodes (`EFS_MAX_NODES`, `EFS_MD_RAFT_N`, voter sets, placement ring, client hint bound); the user keeps >4 nodes in alpha 1.0 (decision Oct 22) | implementation gap / scope | FIXED, merged Oct 22 (`a2942cb0f`). Plan: raise the cap with voter sets for n ≥ 5, placement and capacity tests at 5..max, membership and client tables; packed segments on the data ring across all N nodes (D38); fresh format, no compatibility. **Implemented: cap 16, three voters per group on nodes 1–4, segments on the data ring with a remote pack-object read; unit tests and `make test` PASS on efs1; live 5-node gate NOTRUN ([evidence](k28-w151-cluster-cap-20261022.md))** T3 closed (user, Oct 22): D38 amended to two three-voter groups on nodes 1–4; the K28 layout stands. **Live 5-node gate PASS on xorinox (Oct 23, merged Oct 23 (`306a01fd4`)): five members, voters on 1–3 and 2–4, fragments and packed segments on all five stores, node 5 admits and reclaims segments through the remote pack-object read, one older file returned EIO during the node 1 outage (F1); all files read correctly after rejoin; finding F1 (a stripe acknowledged with two fragments in the window before a node is marked down is never re-protected, so one later single-node outage gave EIO until that node returned; deferred repair scope) ([evidence](gates-xorinox-20261022.md#k28--w151-five-node-cluster)).** F1 is fixed by W154 rebuild on return, merged into devel Oct 23 (`f47a32754`) ([evidence](w154-rebuild-on-return-20261023.md)). |
| K29 | Append path stalls under sustained load with no leader change: on intel2 (K15 live gates, two private three-daemon clusters plus the user's cluster on one 8-CPU host) both lanes fell from 10 to about 0 records/s for 10 minutes; REPORT and APPEND_RESERVE round trips reached 30 s (26 and 12 receive timeouts), `txn-recover` aborted coordinated appends after 10–17 s (EBUSY/EIO), two abandons returned BUSY, the group-0 log grew about 57 Raft entries per 4 KiB record, followers rejected AppendEntries `prev-beyond-log` through the whole run, leader `apply-sleep` max 7 s. Integrity held in every view. | availability / performance | VERIFY first on a dedicated host, one lane at a time, host iowait sampled; then FIX. Found Oct 23 ([K15 live gates](k15-live-gates-20261022.md)); cause not established (host load, software stall or both). The harness also needs a rule for abandons caused by real timeouts (today every non-injected failure and its hole count as a mismatch). Also seen on nuc (W148 rerun, TCP, direct I/O, i5): 20–30 s server stalls under the w125 load produced 30 s receive timeouts, opid-admission BUSY and ambiguous-completion EIO/EBUSY in 6 of 9 attempts ([gates-nuc](gates-nuc-20261022.md)); same class, see W155. **Cause found Oct 23 (branch `claude/devel-k29-2185a1`, not merged): every append reservation and barrier retirement proposed one Raft entry per active writer lane (4 KiB records at 128 KiB chunks reach 64 lanes by record 2,048, 139 group-0 entries per record), and commit probes overtook their batches in the two-lane outbox (`prev-beyond-log`); fixed by one TXN_BATCH entry per group (11 per record at any lane count) and probes on the acknowledged prefix, `make test-all` PASS on xorinox, orcd-mini load verification NOTRUN ([evidence](k29-append-stall-20261023.md)).** **FIXED, merged Oct 23 (`6b621993d`): per-lane Raft entries and probe-overtakes-batch rejects fixed; orcd-mini ior write +24%, 28.6 → 21.8 entries per 1 MiB write; the K15-style append load rerun is NOTRUN ([evidence](k29-append-stall-20261023.md)).** **Oct 24: the harness rule for real-timeout abandons is merged into devel (`a2326a369`) ([evidence](k15-k29-accounting-rule-20261024.md)).** |
| K30 | Intermittent `make test` failures on devel on amd (32 CPU; Oct 23, merged `9b38af915`, full suite 1,038 commands: 4 FAIL, 14 NOTRUN; isolated reruns in parentheses): `tests/test_direct_read_fork_oracle.py` fork+COW case `FAIL files=6400 prefill=1 rounds=2 read_repeats=100` (2 of 3; K5/T1 saw 2 of 6 on unmodified devel); `tests/test_reclaim` case "old meta/ copy fails closed": `reclaim: journal verification FAILED in .../meta/gc-journal; data PUTs refused until recovery` three times, then the `server_reclaim_init(&S)==EFS_OK` assertion at `test_reclaim.c:118` (2 of 3), so the copied meta/ is being refused where the test expects a clean boot; `tests/test_kv_lsm` `test_kv_lsm.c:1057: peeks return while a reader holds the view` (1 of 3), the `test_peek_beside_reader` liveness case the K26 session had to skip under memcheck, which is open **W126** (peeks wait on `mu` while a writer is queued on `view_rw`), so that one is a product ordering defect, not a test flake. Also `tests/test_client_processes.py` needs root on any host running a foreign `efs-fuse` whose argv has a relative mount path (`scripts/client_processes.py` reads the process cwd). | test correctness | FIXED, merged Oct 23 (`cadb3b563`): on amd, reclaim/kv/fork went from 12/3/10 of 25 FAIL to 0 of 50/25/25 (reclaim aside-rename collision is a product fix with regression test; fork-oracle activity sampling, the KV compactor race and two later `test_reclaim` races are test fixes; the W126 peek limitation stays open, pinned by a deterministic case) ([evidence](k30-intermittent-tests-20261023.md)). Found Oct 23 ([K5/T1 evidence](k05-t1-abandon-capture-20261022.md)). |
| K31 | **Devel RDMA clients lose larger replies.** On orcd-mini, freshly formatted, devel `bf16436e8` daemons healthy (both groups led, peers in sync), every FUSE client over RDMA (node9900 and node9901, mlx5_0, segmented-v1) stalls: small replies arrive, but the first reply of a few hundred bytes (a 453-byte INODE_GETATTR reply to a 21-byte request, server counters show it sent) never reaches the client; each reconnect repeats it, so LOOKUP/GETATTR time out at 30 s and mkdir returns EIO. The same client over TCP against the same cluster works (stat, mkdir). The last good RDMA build here was `d94ca85bf`; candidate changes since then include K28 (`EFS_MAX_NODES` 16, larger structs), K5/W72 session fields, W133 declaration gate and K16. Every Oct 22–23 sub-session gate ran over TCP, and RDMA unit tests are NOTRUN on hosts without RDMA, so nothing caught it. | correctness / transport, alpha blocker | FIX: bisect `d94ca85bf..bf16436e8` on an RDMA host (node9901 private cluster), find the reply-size or segmentation change, add an RDMA reply-size regression to the RDMA tests. Found Oct 23 during K27. **FIXED Oct 23 (`24e58fb1d`): K5 started the mount session before the daemon fork, so over RDMA the CQ poller thread lived in the parent and the daemon had none; the session now starts after the fork and a foreign process is refused; RDMA probe and all orcd-mini suites PASS ([evidence](k31-k27-orcd-mini-20261023.md)).** |
| K32 | ior easy write fell from ~1.3 GiB/s to ~0.36–0.5 GiB/s after the fourth wave; reads unchanged. K14 makes every multi-chunk FUSE write its own cross-group transaction (~20 Raft entries per 1 MiB write, from 0.1); K13 adds ~9 (per-object ticket admission and settle). Latency-bound, servers mostly idle. | performance regression, alpha blocker | FIX: batch many K14 units per transaction (a superset published atomically keeps each unit's contract) and batch K13 admissions/settles; gate: ior easy write ≥ ~1.2 GiB/s with the K14 torn-read and K13 ticket gates still passing. Found Oct 23 ([evidence](k32-write-regression-20261023.md)). **Code fixed Oct 23 and merged (`ea2dc4443`).** Consecutive units of one file in a REPORT publish as one transaction, with PREPAREs in TXN_BATCH. Admissions are one ADMIT_BATCH per group per write, and settles are packed per group (Raft tags 90, 91). Counted entries per 1 MiB write: 23.0 → 3.0 at eight writes per REPORT. amd `make test-all` fails only what the base fails. The K14 torn-read gate passes 30/30, including a new eight-units-per-transaction phase. **The ior easy write gate on orcd-mini is NOTRUN** ([fix evidence](k32-write-fix-20261023.md)). |
| K33 | **W157** simple startup and safe online node addition (user amendment Oct 23, supersedes D39/D40 fixed membership): seed/`--join` startup with no cluster file, node id or member count; automatic formation at three agreeing members; safe additions with the 3-to-4 SHARD2 voter transition and usable added capacity; removal deferred. The `--cluster` feature is removed and the D39 admission merge reverted (`d7bca79d6`); W153 (merged `4026e254d`) derives the count and voters from the committed membership. | scope / implementation gap, alpha blocker | IN PROGRESS: model and [protocol](k33-membership-protocol-20261023.md) done; increment 1 merged (`0098e06c4`) ([evidence](k33-w157-formation-addition-20261023.md)): automatic formation, committed revisions with store tokens, pending joiners, SHARD2 {1,2,3} → {2,3,4} by joint consensus with catch-up, storage-only members from five, retained placement; code gate PASS with host limits (xorinox `make test-all` at `0098e06c4`: 433 commands, 0 FAIL, 13 NOTRUN). Increment 2 merged (`53080d744`) ([evidence](k33-w157-autoid-discovery-20261024.md)): no operator node ids (`efsd ADDR:PORT STORAGE` creates or restarts, `efsd --join [SEED] STORAGE` joins; `--node-id` removed), M4′ promise formation (the seed proposes ids in endpoint order, each joiner persists a promise, group 0 commits), additions take `count + 1` (concurrent joiners distinct, a retried join the same id), local-subnet discovery by UDP broadcast on each interface's own subnet (several clusters need a seed; no answer is said and waited; finds peers only), identity v3 (fresh format); code gate PASS with host limits (xorinox `make test-all` at `53080d744`: 436 commands, 0 FAIL, 13 NOTRUN). Live client view refresh merged (`8b4cfbe77`) ([evidence](k33-client-view-20261024.md)): mounted clients install later revisions without a remount (wire types 245/246); code gate PASS with host limits (intel1 `make -k test-all` at `8b4cfbe77`, exit 0). Placement spread fix merged (`95a0e678a`) ([evidence](k33-placement-spread-20261024.md)): the stripe start, packed segments and spares use a full-avalanche hash (fmix64, multiply-shift); unit tests for N = 3..16 PASS; code gate PASS with host limits (amd `make test-all` at `f7553b06e`: 442 commands, 11 FAIL, all also failing on the base `3e01ba95c`, 5 NOTRUN); real daemons: every added node takes new fragments (`{4: 25, 6: 22, 7: 21, 8: 18}`, base `{4: 0, …}`). Acceptance qualification merged (`2ebd4fd85`) ([evidence](k33-acceptance-20261024.md)): interrupted additions at every handoff (new hooks after-learn, learner-added, joint-started, after-g2-copy; concurrent interrupted joins), exact packed and unpacked payloads across additions, a whole-cluster restart and each single-member outage at five, seed loss, and growth bookkeeping 3→6 (capacity, df, protection and abandoned-upload reporting, automatic quota, reclaim, W154) PASS as loopback daemon tests; fixed: reclaim stalled once node 1's group-2 replica retired, and a whole-cluster restart waited 30 s per starting peer; code gate PASS with host limits (xorinox `make test-all`: 446 commands, 0 FAIL, 13 NOTRUN). Remaining: live multi-host (including addition under client load), cross-subnet, RDMA and orcd-mini gates NOTRUN (parent). **Oct 25 (A13 interrupted join, branch `claude/sweet-allen-560f74` on devel `1fc526f66`, not merged) ([evidence](k33-join-retry-20261025.md)): a first start killed between `boot.bin` and `identity.bin` (Task A Candidate A) or during the storage root's O_DIRECT probe was refused on every retry; both now start again with the same command (boot counter 0 proves the token never left the process; the probe page is removed), every other refusal kept (lost identity, wiped store, unknown root data). `tests/test_k33_join_retry.py` (fault points `before-identity`, `before-hello`); intel1 `make test-all` at `bfd0abdf6` (8 jobs): 499 commands, 0 FAIL, 14 NOTRUN host limits (two earlier runs: one known intermittent each); A13 on `loopback-3` PASS with the kill before the commit.** |
| K34 | Acknowledged files unreadable (`EIO`) while one member stops in order: `tests/test_k33_payload.py` failed 4 of 6 runs on nuc, 3 of 5 on intel1 (F24, qualification E3, B6) | single-loss availability, alpha blocker | **FIXED merged into devel (`fc5c1d64e`; tested at `6c09649a3`)** ([evidence](k34-stop-put-ack-20261024.md)). Three causes, each a live member treated as gone or a gone member as live: (1) a chunk acknowledged on the stopping member and one other while the third, up, answered BUSY (its ticket read hit the leaving group leader): a stopping efsd now acknowledges no fragment, and the client resends a BUSY member's fragment instead of a degraded acknowledgement (D40/W154 only for an unreachable or failed member); (2) an open after close failed `EIO` because lease/session commands gave up at the first failed exchange with a stopping leader: they now re-pick a voter; (3) a packed read (and a three-member write) skipped a member that was back but still in the client's 30 s down-mark: reads and degraded acknowledgements now probe it first. New `tests/test_k34_stop_put.py` (forced interleaving, red on the base); nuc `make test-all` at `6c09649a3`: 450 commands, 0 FAIL, 14 NOTRUN host limits; `tests/test_k33_payload.py` 20 of 20 runs PASS at `6c09649a3` (base 2 of 8 FAIL; the test now waits for the W154 rebuild before the next outage). Live multi-host stop under load NOTRUN (parent). |
| K35 | **F22** `fsync`/`fdatasync` COMMIT on every known storage node and return `EIO` while any member is down (`data_commit.c`), contradicting D40's write availability with one node down ([F22](s-docs-alpha-1.0-20261024.md)) | availability / durability contract, alpha blocker | FIX (user, Oct 24, [D42](decisions.md)): degraded success only with proof per affected stripe (matching durable fragments to reconstruct the acknowledged version, durable metadata for version and placement, no unresolved publication or incarnation ambiguity); never by ignoring failed COMMIT replies; W154 restores protection and the degraded state stays visible; on 4+ nodes place three fragments on available nodes. **FIXED, PASS (scoped, loopback), merged into devel (`376369aa8`; tested at `36ac1b7a6`)** ([evidence](k35-fsync-degraded-20261024.md), code `817a71b1f`, `36ac1b7a6`): fsync proves each stripe this client wrote to the file, COMMIT only on the nodes holding its unproven fragments (PUT verifier per fragment); two durable plus a durable degraded mark (the W154 publication mark, or new Raft tag 249 `EFS_MD_CMD_DEGRADED_MARK` before success), else `EIO` with the stripe logged; on 4+ members a hard-failed member's fragment is resent once, then placed on an available member; the client reprobes a down peer from placement; `efs-mgmt status` shows `Protection:`. xorinox `make test-all`: exit 0 at `36ac1b7a6`, 452 test commands, 0 FAIL, 14 NOTRUN host limits (raw: xorinox `~/efs-artifacts/k35-20261024/`, `SHA256SUMS` `06836fd2…6a1e`). Live multi-host and RDMA verification NOTRUN (parent). |
| K36 | **F23** a store after an unclean stop (crash, `SIGKILL`, forced exit rc 3, power loss) stays "recovery uncertain" and COMMIT returns `EIO` for good; no verified recovery exists and a member cannot be removed, so every client's `fsync` fails from then on ([F23](s-docs-alpha-1.0-20261024.md)) | durability / availability, alpha blocker | FIX (user, Oct 24, [D42](decisions.md)): verified recovery that establishes log, metadata, fragment and journal consistency before the fence clears; an earlier ambiguous write is not thereby proven. Crash-recovery tests in alpha; power-loss qualification a separate scope decision. **Implemented, scoped PASS on loopback; merged into devel (`0f0a79eac`, recovery mark on Raft tag 248; tested at `87034b518`)** ([evidence](k36-verified-recovery-20261024.md)): a recovery thread proves boot counter, reclaim journal, session certificates, every local fragment, each hosted group applied to its leader's commit and every published reference against its digest, drops what it cannot prove and marks those stripes for W154 rebuild (Raft tag 248, wire 243/244), then clears the fence; `sync-status` shows phase, counts and reason. Crash tests during PUT, publication, reclaim, KV compaction and recovery, torn and missing fragments, fence held and cleared, COMMIT after recovery with the old client's fsync still `EIO`: PASS (8 of 8 consecutive runs). Merged-tree gate on intel1 at `87034b518`: `make test-long` PASS, `make test` PASS except `test_get_integrity.py`, fixed in `merge-wave5` (`1898c4c63`) and PASS at `c19bf9fdb`; host-limit NOTRUNs only. Power-loss qualification, multi-host and RDMA NOTRUN. **Packed segments (Oct 24, [evidence](k36-k37-segment-recovery-20261024.md), merged into devel (`fc3af9169`)):** a referenced segment fragment recovery drops is no longer only counted: recovery commits K37's segment mark (`EFS_MD_CMD_PACK_DEGRADED`, reason `R_RECOVERY`, forwarded to the group leader, read back) before the fence clears, K37's rebuild restores and verifies it, and a mask with a second bit is reported unrecoverable, never healed; `sync-status` counts segments marked, rebuilt and unrecoverable. Daemon test `tests/test_k36_segment_recovery.py` PASS. |
| K37 | A packed segment (D38: one 2+1 stripe holding many small files) with a missing fragment was never re-protected: K36 can only count referenced segment fragments that are missing, and no segment rebuild existed, so such a segment stayed one loss away from losing every packed file in it (the alpha 1.0 rule: every acknowledged write survives any single-member loss once protection is restored) | durability, alpha blocker | **FIXED, PASS (scoped, loopback), merged into devel (`0061a2343`; tested at `b205cb9db`)** ([evidence](k37-segment-rebuild-20261024.md), code `b48ec444f`, `5ef2b8d6d`, `b205cb9db`): one durable degraded mark per segment (KV kind 249, Raft tag 247 `EFS_MD_CMD_PACK_DEGRADED`) written when a segment (packing or compaction) is copied with two fragments durable and the third PUT not acknowledged (one durable fragment cancels), when the group leader's audit of a member that returned with a new incarnation (wire 241/242, full verification of every placed fragment, only once the member's stores are settled) finds a fragment missing or torn, and when a rebuild source is proven lost (two bits: unrecoverable, reported, never cleared by a rebuild); the leader rebuilds the fragment from the other two, verifies it against the segment checksums, stores it with a rebuild PUT that the storage node accepts only under the exact mark, and clears the mark only after the placement node read it back and verified it; `efs-mgmt status` counts degraded and unrecoverable segments and the cluster state stays DEGRADED until they clear; a packed read with fewer than two fragments is `EIO` (was `ENOENT`). Daemon test `tests/test_k37_segment_rebuild.py` (7 steps) PASS; `tests/test_k33_payload.py` now also waits for `Degraded segments: 0` before the next outage (3 of its 5 outages pack a segment degraded in the detection window). intel2 `make test-all` at `b205cb9db`: 459 commands, 1 FAIL that also fails on the base (`tests/test_get_integrity.py`, a READ_VERIFY test / K34 merge interaction), 14 NOTRUN host limits (raw: intel2 `~/efs-artifacts/k37-20261024/`, `SHA256SUMS` `61fbd81d…98f0`). Live multi-host, RDMA and orcd-mini NOTRUN (parent). **K36 integration (Oct 24, [evidence](k36-k37-segment-recovery-20261024.md), merged into devel (`fc3af9169`)):** a fragment K36 verified recovery drops is marked by the recovering member itself through the same `PACK_DEGRADED` MARK before its fence clears (one mechanism: the audit sweeps only settled stores and skips marked fragments), then rebuilt here; a recovered segment is re-protected without waiting for the audit. |
| K38 | **W161** ior hard (8 ranks, one shared file) on orcd-mini node9901, devel `5a7463440`, RDMA, direct I/O: `write()` returned `EIO` once (1 of 4 runs; node9900 0 of 1; passed on `d94ca85bf`). A REPORT answered STALE (`report-split stale=1 rc=-14`); the client's same-identity resolution gave `outcome=partial rc=-14 first_rc=-13`, retained the error as background pressure, and fsync/flush kept returning the stale generation until the file was removed. Hard write throughput also fell from 47.9 to 16–21 MiB/s ([deploy](orcd-mini-deploy-5a7463440-20261025.md)) | data correctness / error resolution, alpha blocker | FIX: an application write must not get `EIO` on a healthy cluster. **Proven and fixed Oct 25 on branch `claude/vibrant-wiles-325684` ([evidence](k38-stale-span-replay-20261025.md)); merge and the orcd-mini ior hard rerun pending.** A unit test on the production replay proves the cause: with the span's dcache body gone, `dcache_replay_stale_ex_budgeted` returned `EFS_OK` and the putid kept the superseded base (state reached when an older snapshot's PUT lands after a newer one committed and `putid_note` re-inserts it). Fix: the body-less replay drops a superseded span, else rebuilds it on the committed image from its own landed object, else fails closed with a writeback error; `putid_note` refuses the late landing; a record sent unchanged in two STALE rounds on a moved base is stopped (`report-stuck`); `report-resolve` names the stuck record of a PARTIAL receipt. The required loopback test also found two silent losses of acknowledged bytes in the same path (an older PUT's image installed over newer body bytes; the STALE classifier taking a newer local PUT for a commit), both fixed. Loopback (24 writers on one client, shared chunks, dcache pressure): base 6 of 6 runs fail (5 `EIO` with the K38 signature, 1 with lost records), fix 20 of 20 pass. Two writing clients failed ~50 % with unit BUSY until the budget (base too): K40, fixed separately. The throughput drop (47.9 → 16–21 MiB/s) is not addressed. |
| K39 | `write()`/`fsync` return `EIO` with one member down on a 4-member loopback cluster, 2 of 3 runs of `tests/test_k35_fsync_degraded.py` (cases 7 and 8) once the client's away-mark window is the product default of 10 s (the test's `EFS_PUT_AWAY_MARK_MS=600000`, later the fault-only `EFS_FAULT_AWAY_MARK_MS`, had hidden it; efs-fuse has no fault build, so the setting was a no-op since). Found by the merge gate of W159, the flake sweep, the dead-code sweep and the docs audit; logs kept on intel2 `~/efs-artifacts/merge-wave8b-20261025/k35-keep/` | data path / availability with one member down, alpha blocker | **FIXED (scoped) on branch `claude/k39-write-eio`** (code `7c4c2d318`, tests `bd330dd3c`, not merged, not pushed) ([evidence](k39-write-eio-one-member-down-20261025.md)). Cause of the `EIO`: the client's put-ticket ADMIT to the lane group's leader as it stopped lost its exchange and `rpc_put_ticket` returned NET at once (`put-ticket admit ... rc=-6`, then `flush-meta stage=report rc=-6`; the REPORT itself was never sent, and no stale mark was involved): it now re-picks another voter (the commands are idempotent), as does the fsync degraded mark. Two more client defects on the same path made stripes degraded instead of placed around: a member back for under a second was still client-down-marked and no spare (placement now probes it), and `put_fragments_parallel_once` counted an earlier acknowledgement twice and recorded a drain-time loss as no failure. intel1: K35 test 20 of 20 at the 10 s default (base 4 of 8 FAIL); `make test-all` 485 commands, 0 FAIL, 14 NOTRUN host limits. Open: `test_k33_payload.py` 4 of 10 read `EIO` on the fix, 5 of 10 on the base, same client line (`fetch published ... rc=-6`): the K33 read session's item, not K39. |
| K40 | **W162** two FUSE clients writing one shared file (chunks shared across clients; loopback `tests/test_k38_shared_chunk_pressure.py --clients 2`, found by K38) got `EIO` from `write()`/`fsync`: REPORT units answered BUSY for the whole 8 s budget, `report-resolve ... outcome=unknown -> EIO`; unit stages coordinated on the inode shard stayed prepared until `txn-recover ... -> ABORT` 5-20 s later (3 of 4 runs on the K38 branch, ~3 of 7 on devel `5af760ff1`) | data path / publication unit ABORT, alpha blocker | FIX: an application write must not get `EIO` on a healthy cluster. **Fixed Oct 25 on branch `claude/musing-grothendieck-1a6c0c` ([evidence](k40-two-client-unit-busy-20261025.md)); merge and orcd-mini rerun pending.** A forwarded PREPARE `TXN_BATCH` refused STALE (CAS loser) or BUSY for one chunk still installed the other chunks' stages but held no ref; with k == 0 the unit skipped its ABORT. The unit now ABORTs once any entry was submitted (`d7a6eed7e`, found independently by the F1 follow-ups, cherry-picked). Two clients: base 3 of 4 FAIL, fix 10 of 10 PASS, 0 recovery ABORTs; deterministic `tests/test_pub_unit_forward_abort.py` fails 5 of 5 without the fix. `--clients 2` is the K38 loopback default. |
| K41 | The packer placed a segment fragment on a member that had been stopped 27 s earlier and wrote the segment degraded (two fragments, `packer: segment g0 ... written degraded: fragment 0 for node 1 not acknowledged (rc=-6)`); the files it packed were fully protected before packing. The group leader (node 2, 8-member loopback, `tests/test_k33_growth.py`) never logged `heartbeat: node 1 ... marked down` while three other members did within 7 s, so `efs_pack_destinations` still counted node 1 live; when the test then wiped node 5 (the segment's second holder) seven seconds later, a fresh mount read the 2-byte file `EIO` (`fetch published ... rc=-6`): one fragment left, nothing to rebuild from. Reproduced 1 of 3 alone on intel2 and once in the wave-9 merge gate, both times with the group leaders spread (group 0 on node 2); passes with both groups led by node 3. Latent since K37 let the packer write degraded (wave 6); the test began to catch it this wave | single-loss durability of acknowledged small files, alpha blocker candidate | FIX: the packer places only on members the leader knows live (heartbeat marks and Raft peer liveness, or a probe of the three destinations before the segment is written, as K35's spare reprobe does for client PUT), and a destination refused at write time is replaced by a live spare rather than written degraded; find why the leader kept no down mark for 27 s (membership refresh resetting `hb_fail_streak` is the suspect); a read of a one-fragment segment reports `EIO` with the segment named. Evidence: intel2 `~/efs-artifacts/k41-20261025` (daemon and client logs of the failing run, `SHA256SUMS`). **Cause (Oct 25):** not a streak reset: the heartbeat probed members serially with 30 s timeouts, and committed joiners not yet answering held each cycle 30 s, so the long-standing members (both group leaders) never marked node 1 down. **FIXED (scoped)** on branch `k41-packer-placement` (`531c7c783`, devel `1fc526f66`): bounded parallel heartbeats with per-node-id marks that survive table rebuilds; the packer counts heartbeat, peer-pool and Raft voter liveness, probes each destination and replaces a failed or refusing one by the next live ring member (degraded only with no live spare); a short packed read is `EIO` naming the segment. intel2 `make test-all` 0 FAIL ([evidence](k41-packer-placement-20261025.md)). |
| K42 | **W164** a clean unmount on a healthy cluster failed its session revoke with BUSY in the cleanup phase and exited 1 (Task A candidate D: peer client on loopback-5 after the 68/68 two-client suite, `revoke rc=-13 phase=cleanup shard=1677 touched=89`, no outstanding tickets). Reproduced on amd at devel `1fc526f66` on the first loaded run: the peer's append cleanup selected its receipt on a shared inode, and `efs_meta_retire_append_session` refused because another session's append was in flight (OPEN reservation). Nothing was proposed, but the server answered a bare BUSY and the client never repeats an uncertified BUSY. The first gate found the lease-phase twin: the cleanup batch applied as a no-op BUSY while the other client's close transaction held the inode's last-close marker, and the reply carried the entry's index | clean client stop (E2), alpha blocker | FIX: the server certifies every cleanup refusal that committed nothing (append: OPEN, frontier, refused PREPARE even when its ABORT decide fails, barrier retirement, REAP owed, salts 4–7; lease batch applied as no-op BUSY, salt 8). The client repeats certified BUSY within a 20 s per-shard budget and otherwise fails closed with `shard=N reason=...`. Test `tests/test_k42_peer_stop_open.py` (fault hook `EFS_FAULT_APPEND_HOLD_FILE`), reproduction loop `tests/live/e2_peer_stop_loop.py` ([evidence](e2-clean-stop-busy-20261025.md)). FIXED on branch `claude/great-curie-e4cfb1`, not merged. |
| K43 | A writer's `write()` returned `EBUSY` and `close()` `EIO` (64 MiB private overwrite, no chunk shared between clients) when one of six members was SIGKILLed: the Task D four-hour soak on nuc (`b2fc06ba5`, devel `39a1cbbbc` product code, loopback, TCP) at its second daemon crash (node 3, group-0 leader and coordinator of the file's in-flight REPORT). The new leader refused the publication unit NOT_PRIMARY and decided the abort (`coordinator lost its group`), but its 57 PREPARE intents on shard 1 stayed until `txn-recover ... age=8.1s -> ABORT`; the client's resend 7 s later lost its CAS against them (`report-split stale=1 rc=-14`), `report-resolve ... outcome=partial rc=-14 first_rc=-13 stuck=unrecorded (an apply-time CAS loser)`, and `flush-meta` returned BUSY to the application 0.13 s before recovery aborted the intents. Retained stores: all 512 acknowledged versions and 3,379 appends exact on two fresh readers; only the failed write's file holds mixed bytes. The first crash event of the same run (3,480 s) passed | data path / availability with one member down | FIX: an application write must not fail while one member is down: a REPORT whose unit was aborted with its coordinator must wait for (or drive) the abort of its own PREPARE intents, or be answered as not applied, before it is resent; a STALE caused only by its own aborted intents is not a CAS loss. Add a deterministic test that kills the coordinator between PREPARE and decision. **Blocks alpha: yes.** No product fix on the Task D branch. Evidence: [Task D](task-d-20261025.md#efs-finding-k43-one-members-sigkill-surfaced-ebusy-to-a-writer); raw nuc `~/efs-artifacts/task-d-20261025-nuc2/endurance2/` and `endurance2-retained-check/`. |
| K44 | **W167** retired-seed MKFS BUSY after online addition: a remote SALT verdict was followed by a wait on the frozen local group-2 replica; retired masks/admission and non-group-0 MKFS also needed routing corrections ([origin](orcd-test-devel-rdma-394f2b791-20261027.md)) | membership / management routing (K33), liveness | **FIXED (scoped, private node9900 TCP/RDMA)** on `agent/devel-task-k`, tested `00e85a299`, not merged ([evidence](k44-retired-replica-routing-20261026.md)). Live verdicts / target masks, retired NOT_PRIMARY resync/status hints, full MKFS proxy, bounded idempotent notices and observed CFG convergence; retired Raft control participation preserved. Seed sweeps, one-loss transition, seven K33 suites, 218/218 POSIX core each transport and steady-state queries/exact payloads PASS. Full gate 508 commands, 4 FAIL shared base LSan instrumentation, 14 NOTRUN; leak verification NOTRUN. Cold-query skips remain INCOMPLETE. Blocks alpha: **yes**, until multi-host orcd-mini gate (explicit lifecycle approval) and merged qualification. |

### Recheck on devel `ab1920fae` (October 22)

Checked by reading the code and the backlog entries; nothing was run on a
cluster. No row was found already fixed.

| Row | Finding on devel |
| --- | --- |
| K1 | Fixed Oct 22 (`86b6ef567`); R4 fixed and merged Oct 22 (`9fd38ffb4`); reopened by T2, fixed and merged Oct 23 (`c083b98b5`); see the K row. `stop.sh` qualification NOTRUN (gates-nuc session). Live stop gate PASS on nuc at `163241584` (idle 3-daemon cluster stops orderly rc 0, every barrier done, under 1 s; a parked packer is forced to rc 3 at its 6000 ms deadline with stores preserved and a restart serves; the restarted node's own later stop returned rc 4, `put_sync failed`, recovery-uncertain after the forced stop, so no durability certainty is claimed), merged Oct 23 (`2213a4120`) ([evidence](gates-nuc-20261022.md)). |
| K2 | Fixed Oct 22 (`a5eb7e6ff`); see the K row. |
| K3 | Fixed Oct 22 (`1fad577e2`); see the K row. |
| K4 | Fixed Oct 22 (`918ed9207`); see the K row. |
| K5 | Fixed Oct 22 on devel (`a443863b1`); reopened by S2/S3, both fixed and merged Oct 22 (`3bcde5e2a`); reopened by T1, fixed and merged Oct 23 (`9a88e332e`); see the K row. Fence cluster gate NOTRUN (gates-nuc session). Fence cluster gate PASS on nuc at `163241584` (run 2, idle host; run 1 FAIL on two 15 s POSIX timeouts under load, every fence check passed), merged Oct 23 (`2213a4120`) ([evidence](gates-nuc-20261022.md)). |
| K6 | Merged. W130 one-replica DIR fault injection DONE on the loopback gate Oct 25 (`agent/devel-taskb`): halt with the documented diagnostic, quorum keeps serving, `efs-mgmt raft-resync` recovers in 14-20 s (recovery implemented on the branch), 6/6 PASS; W162 (serve-path decode not fail-closed) opened. Remaining: multi-host runs ([evidence](k22-k6-loopback-taskb-20261025.md)). |
| K7 | Fixed Oct 22 (`907a5891f`); R2/R3 fixed and merged Oct 22 (`85d8dd438`); see the K row. |
| K8 | Fixed Oct 22 (`71ce4ff35`); see the K row. |
| K9, K13, K14 | K9 CLOSED without resolution (user, Oct 24, D41: never reproduced; reopens on a new sighting); K13 and K14 were decided FIX (D40, Oct 23) and are merged with D31 collection, the D31 drain and K32 batching (live RDMA gates NOTRUN, see their rows); K15 and K16 are fixed (see their rows). |
| K10 | Open. User-deferred kernel fix; test harnesses now isolate it (`exclusive_create_visibility.py --spawn`). |
| K11, K12, K17–K20 | VERIFY. The fixes are recorded on NUC/AMD/TCP or private stores; the alpha profile on separate hosts is NOTRUN. |
| K21 | W119's evidence (Oct 15) predates the RDMA, query, apply and Raft fixes since then. Rerun its original gates before diagnosing. W65 is closed for TCP only; it overlaps K1 part 4. |
| K22 | Fixed Oct 22 (`28883f2b1`); S1 fixed and merged Oct 22 (`001b5da30`); see the K row. |
| K23 | Deferred by the current alpha scope; needs the user's confirmation. |
| K24 | Fixed Oct 22 (`19555889f`); see the K row. |
| K25 | Fixed Oct 22 (`35647dd0f`); R1 fixed and merged Oct 22 (`6df08cc3a`); see the K row. |
| K26 | Gate done and merged Oct 22 (`b8ef49e58`): `make leak-check` PASS on efs2 and efs1 ([evidence](k26-leak-gate-20261022.md)); NOTRUN on node9901. x86_64: `make leak-check` PASS 25/25 (291 s) on nuc at `163241584`, merged Oct 23 (`2213a4120`) ([evidence](gates-nuc-20261022.md)). |
| K29 | Open (found Oct 23 by the K15 live gates); needs a dedicated-host rerun. |
| K30 | Fixed and merged Oct 23; 0/25 FAIL per case on amd ([evidence](k30-intermittent-tests-20261023.md)). **Oct 25 flake sweep** ([evidence](flake-sweep-20261025.md)): ten 8-job gate runs and three serial `test-long` runs on intel1 at `0b01a2a1e` failed six commands at random; three product defects (the client's away mark, K35; the packer's single PUT attempt, D4; re-forwarded admissions), three test defects (K37 case 7, B geometry, K36 case 11), the query-first NOTRUN and the runner self-test fixed on branch `claude/devel-flake-sweep-1505b6`; the final-gate attempts found a B6 refuse-mode test race and the step-down stall, a third product defect (forwarded PUT ticket admissions forwarded again during an election until the peer pools stalled 5 s; `8f1d50ed8`, 5/28 loaded runs before, 0/40 after). Open: two single read EIOs without logs (K36 case 8, K33 payload). Final gate NOTRUN on the branch (merged into merge-wave8b and gated there). |
| K31 | Fixed Oct 23 (`24e58fb1d`); see the K row. |
| K32 | Code fixed Oct 23 and merged (`ea2dc4443`); ior easy write gate on orcd-mini NOTRUN ([evidence](k32-write-fix-20261023.md)). Oct 24: `test_k32_entry_count` load flake was the test (1 ms linger is a bound); counts made schedule-independent, 0/288 FAIL at 8 jobs ([evidence](test-hygiene-k32-w149-w140-20261024.md)). |
| K33 | OPEN; increment 1 (model, protocol, automatic formation, online addition with operator ids) merged (`0098e06c4`), code gate PASS with host limits (xorinox `make test-all` at `0098e06c4`: 433 commands, 0 FAIL, 13 NOTRUN) ([evidence](k33-w157-formation-addition-20261023.md)); increment 2 (automatic ids, positional CLI, discovery) merged (`53080d744`), code gate PASS with host limits (xorinox: 436 commands, 0 FAIL, 13 NOTRUN) ([evidence](k33-w157-autoid-discovery-20261024.md)); live client view refresh merged (`8b4cfbe77`, intel1 `make -k test-all` exit 0) ([evidence](k33-client-view-20261024.md)); placement spread fix merged (`95a0e678a`), code gate PASS with host limits on amd ([evidence](k33-placement-spread-20261024.md)); acceptance items (interruptions, exact payloads, seed loss, growth bookkeeping) PASS on loopback with two fixes, code gate PASS with host limits (xorinox `make test-all` at `2ebd4fd85`: 446 commands, 0 FAIL, 13 NOTRUN) ([evidence](k33-acceptance-20261024.md)); interrupted first starts retry (Oct 25, [evidence](k33-join-retry-20261025.md), branch, not merged); the live multi-host, cross-subnet, RDMA and orcd-mini gates remain. |
| K34 | Found Oct 24 (F24/E3/B6); fixed merged into devel (`fc5c1d64e`; tested at `6c09649a3`); see the K row ([evidence](k34-stop-put-ack-20261024.md)). |
| K35 | Found Oct 24 (F22); decided D42; fixed and merged (`376369aa8`): `fsync` proves each stripe, COMMIT only on the nodes holding the file's fragments, degraded only with a durable mark; xorinox `make test-all` 0 FAIL ([evidence](k35-fsync-degraded-20261024.md)). Live multi-host verification NOTRUN. **Oct 25:** case 7 failed 2 of 10 gate runs: the client's 10 s away mark on a member that had just failed a PUT excluded it as a spare, so another member's stop was acknowledged degraded instead of placed around; fixed (`fc79fe086`: away members are spares of last resort, an acknowledgement clears the mark) and the case forces the interleaving ([flake sweep](flake-sweep-20261025.md)). |
| K36 | Found Oct 24 (F23); decided D42; implemented and merged (`0f0a79eac`): verified recovery clears the fence only once log, metadata, fragments, journal and ledgers are proven; dropped fragments are marked for rebuild (tag 248); intel1 gate PASS with host limits ([evidence](k36-verified-recovery-20261024.md)). Power-loss qualification is a separate scope decision; live runs NOTRUN. Segment fragments dropped by recovery are marked for K37 only on branch `claude/devel-k36-k37-segments-5885d9` (not merged). **Oct 25:** case 11 chose a segment the packer had already written degraded (1 of 10 gate runs); the case waits for `Degraded segments: 0` first ([flake sweep](flake-sweep-20261025.md)). |
| K37 | Found Oct 24 by K36; fixed and merged (`0061a2343`): packed segments copied with two durable fragments carry a degraded mark (tag 247, KV kind 249) and are rebuilt on return; intel2 gate PASS with host limits ([evidence](k37-segment-rebuild-20261024.md)). Live runs NOTRUN. **Oct 25:** the packer sent each fragment once, so one transient `NOT_PRIMARY` from a follower under load wrote a segment degraded (D4 `put_errors=1`, 1 of 10 gate runs); a fragment refused BUSY, NOT_PRIMARY or AGAIN is sent again first (`fc79fe086`), K37 case 1b forces it with `EFS_FAULT_SEGMENT_PUT_TRANSIENT`; case 7 of the test matched old log lines after a leader change (2 of 10), fixed ([flake sweep](flake-sweep-20261025.md)). |
| K38 | Found Oct 25 on orcd-mini (W161); cause proven and fixed Oct 25 on branch `claude/vibrant-wiles-325684`, merged into devel Oct 25 (wave 9, `9ae2f36d3`; tested at `9ae2f36d3`): body-less STALE span replay rebuilds or fails closed, livelock bound, resolve names the record, plus two same-path data-loss fixes; intel2 `make test-all` PASS with host limits at `bac34da00` (483 commands, 0 FAIL, 16 NOTRUN; a first run had 1 FAIL, `test_k36_recovery.py` step 8 read EIO, which passed 3 of 3 alone on the branch and on the base and did not recur) ([evidence](k38-stale-span-replay-20261025.md)). orcd-mini ior hard rerun NOTRUN. |
| K39 | Found Oct 25 by the wave-8b merge gate once the away-mark override became fault-only; fixed (scoped) Oct 25 on branch `claude/k39-write-eio` (ticket and degraded-mark RPCs re-pick a voter after a lost exchange; placement probes a just-returned member; acknowledgements counted once), intel1 K35 20/20 and `make test-all` 0 FAIL ([evidence](k39-write-eio-one-member-down-20261025.md)); merged into devel Oct 25 (wave 9, `9ae2f36d3`); the K33 payload read `EIO` (base and fix alike) stays open. |
| K40 | Found Oct 25 by K38 (loopback, two clients); fixed Oct 25 on branch `claude/musing-grothendieck-1a6c0c`, merged into devel Oct 25 (wave 9, `9ae2f36d3`): unit ABORT after any submitted PREPARE entry ([evidence](k40-two-client-unit-busy-20261025.md)). orcd-mini NOTRUN. |
| K41 | Found Oct 25 by the wave-9 merge gate (`test_k33_growth.py`, leaders spread); cause: serial heartbeats stalled 30 s per silent joiner, and the packer trusted the mark alone. Fixed (scoped) Oct 25 on branch `k41-packer-placement` (`531c7c783`), not merged: intel2 `make test-all` 499 commands, 0 FAIL, 14 NOTRUN host limits; new `test_k41_packer_placement.py` fails on the base, passes on the fix; growth test 10 of 10 PASS (2 spread) ([evidence](k41-packer-placement-20261025.md)). Live/RDMA NOTRUN. |
| K42 | Found Oct 25 by Task A (candidate D); reproduced and fixed Oct 25 on branch `claude/great-curie-e4cfb1` (devel `1fc526f66`, code `e62cc1d07` … `b9d5f9443`), not merged: certified cleanup waits are retried, and a stop still refused fails closed with its reason. intel2 at `b9d5f9443`: 20 of 20 clean stops of a loaded peer (68-case two-client suite before each); `make test-all` at 8 jobs 499 commands, 0 FAIL, 14 NOTRUN host limits (amd, where it was reproduced, went offline before the final runs) ([evidence](e2-clean-stop-busy-20261025.md)). Qualification rerun (loopback-5 `gc-reclamation`) and live multi-host runs NOTRUN. |
| K43 | Found Oct 26 by the Task D four-hour soak (nuc, `b2fc06ba5`, 7,195 s, 15th event); open, not reproduced deterministically yet (one of two daemon SIGKILLs in the run). No acknowledged data lost (retained-store read PASS). Blocks alpha: yes. K42 is taken by the unmerged branch `claude/great-curie-e4cfb1` (W164). |
| K44 | **FIXED (scoped)** on `agent/devel-task-k`, tested `00e85a299`, not merged: private node9900 TCP/RDMA seed/added-member management, exact FUSE/POSIX and stable queries PASS; default gate 508/4 FAIL/14 NOTRUN (four shared base LSan instrumentation errors). Blocks alpha: yes pending multi-host orcd-mini approval/gate and merged qualification; cold-query cases remain INCOMPLETE ([evidence](k44-retired-replica-routing-20261026.md)). |

**Oct 23 evidence qualification:** nuc capture bundles are reported lost;
xorinox raw outputs were deleted. Their PASS labels below are historical
reported scoped outcomes. Independent raw-evidence revalidation is
**INCOMPLETE**, and gates requiring those captures need retained reruns.
This does not invalidate separate prior runs or change recorded FAIL/NOTRUN
outcomes. See the [response review](devel-integration-review-20261022.md#review-of-the-gate-review-response-2be72f2c2--october-23).

### Stage 1 tally, end of Oct 22 (adjusted after the integration review)

**Additional alpha blocker W153:** the unset/empty metadata count silently
selects four nodes and can remove single-loss tolerance on three-node
clusters. Resolve authoritative bootstrap membership and strict configuration
validation, then pass the startup and single-loss gates in the
[W153 review](devel-integration-review-20261022.md#review-of-a76b63f42--w153-is-an-alpha-correctness-gate).
The existing K28 implementation and T3 layout decision remain credited;
W153 is not closed by their voter-mask unit tests.


Fixed and merged into devel, unit-tested, live gates NOTRUN: K2, K3, K4, K8,
K16, K24, K28 (W151: cap 16, segments on the data ring; 5-node live gate
NOTRUN). **Reopened by the
[integration review](devel-integration-review-20261022.md) and fixed again,
merged Oct 22 evening:** K5 (S2, S3; `3bcde5e2a`), K7 (R2, R3; `85d8dd438`),
K22 (S1; `001b5da30`), K1 (R4; `9fd38ffb4`), K25 (R1; `6df08cc3a`). The
third review reopened K1 (T2) and K5 (T1, also affecting K15); both are
fixed and merged Oct 23 (K1 `c083b98b5`, K5 `9a88e332e`), so no row stays
reopened. New Oct 23: K29 (append stall under load, found by the K15 live
gates on intel2, integrity intact) and K30 (intermittent `test_reclaim` and
`test_direct_read_fork_oracle` failures on unmodified devel), both VERIFY
first. Live gates run Oct 23 and merged: K28 five-node PASS on xorinox (finding
F1 → W154), K22 scoped PASS (production-scale plateau, snapshot import and
leader admission at 3/4 still NOTRUN), K9 not reproduced in 110 rounds, K26
leak-check PASS on x86_64, K5 fence gate PASS, K1 stop qualification PASS,
W148 rerun: signature 0 of 9, NOT REPRODUCED on TCP; the gate itself FAILS
on ambiguous-completion EIO/EBUSY (→ W155), and its raw bundles were lost
with the session (summary-only evidence). INCOMPLETE: K9 (W87 not reproduced; oracle in devel), K15 (W64 finisher
handoff and lost-reply cleanup merged, `a51832413`; the failover and
lost-reply live gates ran once on intel2 Oct 23: lost-reply FAIL and W52 FAIL indication under the K29 stall, failover modes and second runs NOTRUN). VERIFY on the real clusters: K6, K11, K12,
K17–K21, K27. User decisions taken Oct 23 (D40): K10 known limitation, K13 and K14 FIX in alpha, K23 stays deferred, W154 rebuild-on-return, D39 amended (simple startup, automatic identity, online addition; removal deferred), W153a after the model, orcd-mini reformat approved. K26 closed as a gate:
`make leak-check` (valgrind memcheck, 25 unit binaries) merged (`b8ef49e58`),
PASS on efs2, NOTRUN on node9901. The W125 harness gap (cleanup,
remote and final-verification evidence, self-tests) is merged (`56b7e48fe`).
The merged devel passed `make test` with zero error lines on efs1 at
`95beaa053`, 927/927 on efs2 at K5's HEAD, and (K8 session, Oct 22 evening)
rc 0 on efs1 at `4090e717b`, and rc 0 with zero error lines (15 NOTRUN, no
kept scratch) on efs3 at `0eb1511d2`, the tree with every second-wave merge
(`TMPDIR=/data1/tmp`; a one-line TMPDIR fix to K7's sweep test was needed
after K25's hygiene gate met it).

### D39 admission reverted, Oct 23

User decision: the `--cluster` file feature is removed; the admission merge
`246bae7cf` is reverted (`d7bca79d6`), seed/`--join` startup is back.
`make test-all` at `d7bca79d6` on amd: 428 commands, 0 FAIL, 14 NOTRUN
(environment). Alpha work from the membership thread: W153 (count and
voters from committed membership, formation at three), then node addition
(K33/W157). orcd-mini can run devel HEAD again without a devops change.

### Fifth wave merged, Oct 23

W156 (`d58de2b2f`), K13 D31 collector (`00c865924`), K29 (`6b621993d`) and D39
admission stages 1–2 with stage 3 partial (`246bae7cf`) are in devel. Three
parallel-branch number collisions were fixed in the merges (Raft tag 86 →
`TXN_BATCH` 88; message types 177/178 → `FORMATION_READ` 179/180; KV kind 51
earlier) and `tests/test_kv_kind_unique.py` now checks KV kinds, Raft tags
and message types. `make test-all` at `1324cbd99` on intel2: 431 commands,
0 FAIL, 14 NOTRUN (environment). orcd-mini at `6b621993d` (before D39): ior
easy write 444 MiB/s, 21.8 Raft entries per 1 MiB write (K32 still OPEN).
**Current code gap:** these D39 builds require `--cluster` and reject `--join`.
The later user amendment requires simple startup and online addition (A13);
a devops workaround alone does not satisfy alpha. Update the protocol and
normal launch interface to the accepted contract.

### Fourth wave merged, Oct 23

W154 (`f47a32754`), K14 (`d2369a49d`), K30 (`b64dbe934`), W155 oracle
(`92886b8e2`), D39 model (`16d716308`) and K13 (`1f3b25997`, after its
rebase) are in devel; a KV-kind collision between W154 and K14 (both 51)
was fixed with a uniqueness test (`bd2cb0d5a`). `make test-all` on intel1 at
`1f3b25997`: 425 commands, 0 FAIL, 14 NOTRUN (no RDMA device, no valgrind
header, no cluster scripts), ThreadSanitizer gates included. All of it was
tested over TCP only; the orcd-mini RDMA gates for W154, K14 and K13 are
NOTRUN.

### Third wave, Oct 22 night (sub-sessions, Opus 5.5 medium)

User direction: everything left on Stage 1 that is code or a runnable gate
goes to sub-sessions; build and test hosts are now the bare-metal machines
amd, intel1, intel2, xorinox and nuc (one session per host, private
daemons on 18xxx/19xxx ports under `$HOME`, the user's live clusters on
17432–17435 untouched, hosts cleaned at the end); the efs1–3 VMs carry the
user's own cluster and are not used. Sessions and branches: K5/T1 fix
(`devel-k05t1`, amd); K1/T2 fix (`devel-k01t2`, intel1); K15 live gates
W52 20k, W64 failover and lost-reply (`devel-k15gates`, intel2); gates on
nuc: K26 leak-check x86, K5 fence gate, K1 stop qualification, W148 rerun
(`devel-gates-nuc`); gates on xorinox: K28 five-node, K22 plateau and
stalled follower, K9 W87 oracle (`devel-gates-xor`). Not sent: W153a (held
for the D39 decision), K27 (orcd-mini restart needs the user), the RDMA
multi-host VERIFY rows K6 and K18 and the remaining VERIFY rows K11, K12,
K17, K19–K21 (next wave once hosts free up), and the user's decisions K10,
K13, K14, K23.

### Proposed fix order

Each step is one change or a small group. A sub-session implements it with
its regression test and runs `make test` on an efs1–3 build VM; that is the
whole of a sub-session's job. Cluster gates, A/B measurements and long runs
are not sub-session work: they stay NOTRUN in the sub-session's report and
the parent session runs them on the real clusters (node9901 private `/tmp`
clusters, orcd-mini with the user's go-ahead). An item whose implementation
itself needs an A/B or a long run stops at that point and is marked
INCOMPLETE with what was done and what is missing (user direction, Oct 22).

1. **Your scope decisions.** No code. These change what Stage 1 contains:
   - K10: W121, the kernel FUSE issue;
   - K13: whether the PUT-ticket stack W86 is in alpha or archived (this
     also settles W137);
   - K14: atomic multi-chunk publication;
   - K23: power loss, repair and crash recovery.
2. **Trustworthy tests:**
   - K24 (`EFS_KV_COMPACT_DIE` under `EFS_FAULTS`);
   - K25 (W135, W136, W138, W139, W141, W150, W120; W137 per step 1);
   - K26 (a working leak gate: fix LeakSanitizer on the hosts, or use valgrind).

   These are small, and every later verdict depends on them.
3. **Small fail-open holes:** K3 (delete the `NODE_LEFT` handler), then K2
   (refuse unknown tags at submit). One request can halt or shrink a cluster
   today.
4. **Raft:**
   - K1 part 2 first, while orcd-mini's logs still exist. Was a client
     acknowledged?
   - Then parts 1, 3 and 4, and the private deposed-leader reproduction.
5. **K8 reproduction.** Runs on node9901 in the background while steps 3–4
   are coded; the fix follows from the saved bytes.
6. **K4:** the client declaration matrix, plus refusal tests for TCP and RDMA.
7. **Data correctness:** K9 (W87), K15 (W64, W52), then K16 (W75; a format
   change, so a fresh format).
8. **K5 with W72:** register every mutation path, then remove the bypass.
   This is the largest item.
9. **Bounded state:** K7 (W83 decision retirement) and K22 (W23 compactor
   bound).
10. **K13/K14, if step 1 keeps them in scope.**
11. **VERIFY sweep on separate hosts with one candidate:** K6, K11, K12,
    K17–K21. This needs the 3-, 4- and 5-plus-host clusters, so they are
    set up in parallel with steps 2–10.

## A. Stage 2: freeze a reproducible candidate and support matrix

- [ ] **A1** Choose one devel candidate commit and identify all daemon/client
  binaries and test-harness revisions. Test fixes belong in the candidate;
  do not qualify an undocumented mixture of binaries and scripts.
- [ ] **A2** Record the actual enabled features and transport/storage settings.
  Include RDMA/direct I/O, enabled `.efs` and background packing for the current
  target profile; containers remain disabled, expiry is policy-governed and
  qualified both off and on (D43, Oct 25). If any of those choices
  changes, amend this matrix before calling the run alpha acceptance.
- [ ] **A3** Specify supported node-count range. Qualify 3 nodes, 4 nodes and at
  least one concrete larger topology (minimum 5). Prerequisite: K28/W151, the
  code's four-node cap is raised; until then the third column is UNSUPPORTED,
  not NOTRUN. Record further coverage of
  the claimed maximum and topology-dependent limits; one 5-node PASS does not
  qualify arbitrary cluster sizes.
- [ ] **A4** Use distinct physical hosts for distributed acceptance. A private
  cluster with three daemons on one host is useful regression evidence, not
  proof of three independent node/failure domains.
- [ ] **A5** Record quota, disks, RDMA devices, mount options, clients, concurrency,
  deadlines, fixture counts/sizes and expected results for every topology.
  Fresh-format setup only when a format change requires it; no compatibility work.

## A13. Simple startup and online growth (accepted Oct 23 amendment)

- [ ] **A13 — Required alpha gate.** Implement and pass the
  [startup and node-addition checklist](alpha-1.0-node-addition-20261023.md):
  minimal seed/join commands, no mandatory member-list file, automatic initial
  three-member formation, safe live 3-to-4 and further additions, usable added
  capacity, stable existing data placement, interrupted-join recovery and
  single-loss verification. Always 2+1. Online removal remains deferred.
  Current D39 fixed-membership code/model does not close this gate.
  **Oct 25: real-host gate `a13-node-addition` in `scripts/qualify.py`
  ([Stage 2 gates](stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent). It grows its own
  cluster 3 → 4 (a join killed and retried: the same member) → 5 with the seed
  down, under a client load: SHARD2 `{1,2,3}` → `{2,3,4}`, usable capacity
  of the added quotas, new bytes on the added members, payloads exact.**

## A6–A12. One supported behavior: audit and remove A/B modes

**Mandatory alpha 1.0 gate, added by user direction October 22.** EFS should work
with built-in policies and good defaults, without users choosing competing
implementations. Retaining old/new behavior behind hidden environment variables
is not completion. Automatic adaptation within tested bounds is compatible with
one supported policy; fixed constants are not required where adaptation is better.

The qualification cost is combinatorial: n independent binary switches permit
2^n configurations (10 switches: 1,024). Constraints reduce reachable combinations,
but testing each flag separately does not establish their interactions. Removing
alternatives reduces both product complexity and the supported test matrix.

- [ ] **A6 — Inventory.** Audit environment variables, CLI options, configuration
  fields, runtime management settings and compile-time feature modes. For each,
  record its reader/call sites, default, alternative behavior, dependencies,
  deployment/portal exposure and tests. Include undocumented and dead settings.
  Find readers that take the name as an argument (`env_u64("EFS_…")` and the
  `getenv(name)` helpers in `bufpool.c`, `write.c`, `packer.c`, `raft_host.c`,
  `sim_disk.c`); grepping `getenv("` alone undercounts.
  **Oct 24 (branch merged into devel (`6ed4be4a8`)):
  [inventory](a6-settings-inventory-20261024.md) of every variable, CLI
  option, runtime management setting and compile-time mode, found by
  `scripts/env_inventory.py` (helpers by fixed point); `make test` fails a
  new `EFS_*` reader, mode or removed name without a row. Before figure:
  139 variables read on `a2326a369` (13 A/B, 44 numeric, 9 deployment, 34
  observability, 37 fault/test hooks of which 3 honored by normal builds,
  2 other); 127 on the branch.**
- [ ] **A7 — Decide and remove alternatives.** For behavior-changing A/B switches,
  select and qualify one supported implementation or automatic policy. Remove the
  losing code path, parser/environment reader and obsolete tests/docs together.
  Do not merely hide the flag, change its default or force every switch on.
  No replacement compatibility switch or fallback to the retired behavior.
  **Oct 24: the 15 selectors (13 variables, `--direct-io`/`--no-direct-io`
  and `--no-persist`) are in a
  [decision table](a6-settings-inventory-20261024.md#decision-table-for-the-parent-a7)
  with a recommendation each, for the user; none removed. `EFS_RDMA_FIRST`
  turned out to be trace only, not a selector.**
  **`EFS_READ_VERIFY` (D42): done Oct 24.** The
  [GET-path audit](read-verify-audit-20261024.md) found the client re-hash
  not redundant (with it off, 12 of 51 negative cases returned wrong bytes
  on chunk GET, reconstruction, retries and spans); the re-hash is built in,
  the variable is removed and refused at startup.
  **Oct 24, A7 done as decided in D42 ([evidence](a7-removals-20261024.md);
  merged into devel (`6069ada62`)):** 12 variables and
  the 3 efsd flags removed with their code paths and refused at startup;
  fragment I/O is `O_DIRECT` only and a root without it is refused; Raft
  batching, KV bloom, disk query backend, 16-chunk prefetch and unmount
  revoke are the one behavior; `.efs` follows the cluster commands-policy;
  expiry is compiled out (`EFS_BUILD_EXPIRY`); `EFS_RDMA_FIRST` is
  `EFS_RDMA_TRACE`. **Oct 25, D43:** expiry ships again, in every build
  (no `EFS_BUILD_EXPIRY`, `EFS_EXPIRY` stays refused and names the policy);
  the cluster expiry policy (`efs-mgmt expiry-policy NODE:PORT cluster`,
  versioned, off by default) is its only control; its tests are back in
  `make test-all` and the `d5-expiry` qualify gate
  ([evidence](d43-expiry-20261025.md)). A/B selectors 15 → 1: `EFS_READ_VERIFY` waits on its
  GET-path audit. Live qualification of the retained paths stays open (A11).
- [ ] **A8 — Keep only justified configuration.** Retain deployment inputs such as
  nodes, disks, addresses and quotas. Prefer built-in or automatically derived
  internal batching/concurrency/maintenance settings; justify any remaining
  operator tuning with a concrete operational need and a bounded test scope.
  Keep observability separate from behavior selection. Fault injection belongs
  in test builds and must not be a normal deployment option. Record a disposition
  for every inventoried setting; intentional transport/platform support is not
  automatically an obsolete A/B implementation.
  **Oct 24, decision-free part (branch `claude/devel-a6-inventory-7f975e`):
  every setting has a disposition; 11 numeric tunables that no test or
  launch script sets are built in; `EFS_GC_DISABLE` and
  `EFS_TEST_MEMBER_EXIT`/`_ONCE`, honored by normal builds, are
  `EFS_FAULT_*` hooks in `EFS_FAULTS` builds (`tests/faults/efsd`); the 34
  observability switches change no behavior; no dead setting in the
  product; 19 removed names stop efsd and efs-fuse at startup. Open: the
  33 remaining numeric tunables need a qualified built-in value or bound,
  and the A7 decisions.**
- [ ] **A9 — Metadata leadership.** Remove `EFS_METADATA_BALANCE` as an operator
  choice after selecting and qualifying the automatic policy. Do not force its
  present common-voter/co-located-leader preference on as a substitute for that
  decision. Preserve quorum, safe transfer and catch-up; expose actual leadership
  diagnostically. This does not claim the later many-group design is complete.
  **Oct 24 (D42, [A7 evidence](a7-removals-20261024.md#metadata-leadership-a9-part)):
  the variable and the co-location pump are removed; elections alone place
  leaders; `efs-mgmt status` reports actual placement per group and node
  (`raft-status` per node). Placement is not yet measured under A11 load.**
  **Oct 25 ([A6–A12 hygiene](a12-hygiene-20261025.md#a9-leader-placement-item-2),
  branch `claude/fervent-wilson-67282f`, not merged):** `scripts/leader_placement.py`
  records who leads each metadata group over a run, through forced
  step-downs and leader restarts. It reports each group's distribution and
  the share of time both leaders sat on one node; `make test` checks its
  parsing. On loopback (intel2, TCP, 600 s each, 12 step-downs, 6 restarts)
  both leaders sat on one node 41.5 % of the run at 3 members and 20.7 % at
  4 and 5. Elections neither co-locate nor spread them. The real-host
  measurement under A11 load is the parent's.
- [ ] **A10 — Operational consistency.** Update launch/deployment scripts, portal
  settings, help and generated settings listings with each removal. Stale launch
  configurations must be detected by validation rather than silently presented
  as effective settings. No new deployment should require a hidden toggle to
  obtain the qualified behavior.
  **Oct 24 (A7): help, user docs and the A6 inventory updated; every removed
  setting stops efsd/efs-fuse with a clear message. Launch, env, portal and
  gateway changes outside the repository are listed with file and line in the
  [A7 evidence](a7-removals-20261024.md#for-the-parent-changes-outside-this-repository-a10);
  not yet applied.**
- [ ] **A11 — Reduced qualification matrix.** Publish the remaining supported
  configuration dimensions and rationale. Qualify the selected paths under the
  topology/profile gates below, including automatic-policy boundaries, load
  changes and orderly restart. Report before/after behavior-switch counts and
  reachable configuration counts where defensible; do not claim exhaustive
  coverage from testing flags individually or from pairwise tests alone.
  **Oct 24, matrix and plan published ([A11](a11-qualification-matrix-20261024.md);
  merged into devel (`646a43041`)); qualification
  NOTRUN.** Dimensions: transport (`tcp`, `rdma`; `auto` resolves to one),
  members 3–16, `O_DIRECT` only, one root per node, one or two client
  mounts, one feature profile, 33 numeric tunables at built-in values.
  Selector × transport × member count: 1,376,256 before A6/A7, 28 now
  (A/B selectors 15 → 0). Qualified exhaustively: TCP/RDMA × 3, 4, 5
  distinct hosts (six full runs); 6–16 sampled (loopback 6–8 only). Every
  Stage 2 item is mapped to its gates, cells, duration and prerequisites in
  `scripts/qualify_matrix.json`; `qualify.py run --matrix … --topology NAME`
  runs one topology. No stub gate is left (Oct 25: A13, C5, D1/D2, D3 and E4
  are [real gates](stage2-gates-20261024.md), loopback-3 PASS; B6 and D4 since
  their merge). **Blocks A4:** no distinct-host topology can
  form today (firewalld on the Rocky hosts allows only ssh; amd has no wired
  link; no RDMA fabric across three hosts).
- [ ] **A12 — Closure.** Every A/B selector has either been removed with its
  alternative implementation or is an explicitly justified, reviewed support
  dimension under A8. No unexplained experimental, legacy or correctness-policy
  toggle remains in the alpha product. Record failures and gate results for the
  integrated candidate; flag removal alone is not correctness evidence.
  **Oct 25 ([closure](a12-closure-20261025.md), branch
  `claude/fervent-wilson-67282f`, not merged):** every A6 selector is listed
  with its disposition. All 15 are removed and refused, and `EFS_TRANSPORT`
  is a justified A8 support dimension. A/B selectors 15 → 0; reachable A/B
  combinations 32,768 → 1; variables a normal build reads 105 → 77. Every
  remaining reader has a retained kind: deployment input, numeric tunable,
  observability, fault/test hook or tool input. `env_inventory.py --check`
  now fails on any other kind. A dead-code sweep and the removal of buffered
  I/O remnants from the harnesses and docs are in the
  [hygiene evidence](a12-hygiene-20261025.md). D43 (Oct 25) ships expiry and
  `.efs`; expiry code was left untouched. The retained paths' live
  qualification (A11, D42 direct I/O) stays NOTRUN.

**Review of A6–A12 (October 22, at devel `a569de971`).** These are starting
facts for A6, not decisions.

- **Size.** `src/` and `include/` name 118 distinct `EFS_*` environment
  variables: 90 through literal `getenv("…")`, the rest through name-taking
  helpers (16 `EFS_PACK_*` tunables alone in `packer.c`). Efsd/efs-fuse CLI
  options and the compile-time modes `EFS_FAULTS` and `EFS_BENCH_BUILD` come on
  top. A6 should publish this count as the "before" figure A11 asks for.
- **Kinds.** The 2^n argument fits true A/B selectors. Examples to classify
  first: `EFS_METADATA_BALANCE`, `EFS_QUERY_BACKEND`, `EFS_RDMA_FIRST`,
  `EFS_READ_VERIFY`, `EFS_READ_PREFETCH`, `EFS_FUSE_SPLICE_READ`,
  `EFS_KV_BLOOM`, `EFS_RAFT_BATCH_OVERLAP`, `EFS_RAFT_BATCH_NOOP`. Numeric
  tunables (most `EFS_PACK_*`, `EFS_QUERY_*_MIB`, `EFS_PUT_SYNC_*`) are not
  binary; A8/A11 should qualify a built-in value or a bounded automatic range
  for each, not count them as 2^n. Trace/debug/profiling switches (`*_DBG`,
  `*_TRACE`, `*_PROF`, `*_OBS`) are observability under A8 and should be
  confirmed not to change behavior.
- **Feature enablement is also A/B.** `EFS_COMMANDS`/`EFS_COMMANDS_SERVER` and
  `EFS_EXPIRY` select whether whole features exist. A2 fixes the alpha profile
  (`.efs` on, containers off; expiry policy-governed since D43, Oct 25). A7 must decide whether the disabled
  features leave the alpha product or stay as a justified dimension under A8;
  shipping them dark behind a variable is the pattern this gate rejects.
- **Fault injection leaks into normal builds.** `EFS_KV_COMPACT_DIE`
  (`src/kv/kv_compact.c:270`) is read in every build and makes the daemon
  `_exit(99)` mid-compaction. `test_kv_lsm` relies on it in the ordinary test
  build. Move it and its test under `EFS_FAULTS` (A8). Most other fault sites are
  gated, but five use `#ifdef EFS_FAULTS` and the rest `#if EFS_FAULTS`. Use one
  form, so that `-DEFS_FAULTS=0` cannot turn the `#ifdef` sites on.
- **Ledger.** A6–A10 and A12 are code and documentation work, done once per
  candidate, not per topology. Only A11 qualification is per topology. The
  ledger row should read that way, so that three NOTRUN cells don't suggest the
  audit runs three times.

## S. Stage 2: functional scope

- [ ] **S1 — Surface list.** Mark every user-facing surface in or out, with
  a reason. Candidates in scope: Linux FUSE mount over RDMA with direct I/O
  (and TCP if supported), `.efs` commands (**W91/W92** named gates),
  background packing (**W125**), logical truncate (**D25**), expiry under
  the cluster expiry policy (**D43**, Oct 25), the `efs-mgmt`
  operator commands, and the start/stop/deploy scripts. Candidates out of
  scope: containers, 2+2, automatic repair
  and crash recovery (K23). **Decided out (user, Oct 22):** NFS re-export and
  macOS clients, on the beta backlog as **W152**; alpha supports the Linux FUSE
  client only, and F4 says so.
  **DRAFT Oct 24 ([surface list](../using/alpha-1.0-surface.md),
  [evidence](s-docs-alpha-1.0-20261024.md)):** every binary, `efsd`/`efs-fuse`
  option, `efs-mgmt` and `.efs` command and user-settable environment
  variable marked in or out with a reason, checked against `a2326a369`.
  Rows marked *proposed* (for example `--no-direct-io`, `--writers`,
  `--no-persist`, `add-node`, `shrink-quota`, `raft-step-down`) wait for the
  user; findings F2–F8, F11, F12 list help-text gaps and profile defaults.
  **Audited Oct 25 against `ec3e09306` ([audit](docs-audit-20261025.md)):**
  every binary, option, `efs-mgmt` and `.efs` command and refusal exercised
  on a loopback cluster of that build; the page lists every `raft-<op>`,
  `status`'s sections and exit status, and the removed `EFS_COMMANDS`;
  `tests/test_cli_docs.py` (`make test`) fails when `cli_help.h` and the
  user pages name different commands or options. Still *proposed*:
  `--writers`, `sync-status`, `pack-compact`, `raft-resync`, `shrink-quota`,
  `raft-step-down`, `data-commit`; F12 (port authentication) waits for the
  user. **Scope change Oct 25 (D43, user):** expiry and `.efs` ship in
  alpha 1.0; the surface list, guide, POSIX page and release notes say so.
  At `ec3e09306` expiry is still compiled out of the product build and
  being re-enabled (audit finding U19).
- [ ] **S2 — POSIX subset.** Publish the supported operations and every known
  gap (for example, mmap is skipped in the extras suite). Each gap is either
  fixed or justified as a limitation (F4). No gap counts as PASS.
  **DRAFT Oct 24 ([POSIX subset](../using/alpha-1.0-posix.md)):** supported
  operations from the core (216), extras (5), two-client (68) and
  durability (27) cases; deviations P1–P14 with their suite cases (shared
  mmap W104, K10 fork readback, `renameat2` flags, `fallocate` modes,
  non-`user.` xattrs, `mknod`, D28, D32, W159 open). Recorded runs are on
  `cb970ca7c` and `163241584`; none on the base; C1 per topology NOTRUN.
  **Audited Oct 25 ([audit](docs-audit-20261025.md)):** case counts match
  the suites at `ec3e09306` (218 core, 5 extras, 68 two-client, 27
  durability); P15 no longer says COMMIT is fenced until a fresh format
  (K36); P13 (W159) records the Oct 25 diagnosis; `.efs` extras depend on
  the commands-policy, not on the removed `EFS_COMMANDS`.
- [ ] **S3 — Operator workflows.** Format, start, stop, status, `df`,
  quotas, `raft-status`, `raft-resync`, packing control, and failure
  messages. Each one is documented and covered by a test.
  **DRAFT Oct 24 ([alpha guide](../using/alpha-1.0-guide.md)):** format
  (automatic at three members, optional `mkfs`), start, status, mount and
  unmount, `df`, quotas, adding a node, adding storage, stop, restart and
  orderly shutdown with the current CLI. Test coverage gaps: `add-storage`
  and `add-node` have no test, `raft-resync` only a gate script (F11, F20;
  fixed Oct 24). **Audited Oct 25 ([audit](docs-audit-20261025.md)):**
  every workflow run on a loopback cluster at `ec3e09306`; the guide now
  describes `status`'s full output and exit status, the clean-stop output
  and stop record, the restart rules after a forced, failed-barrier or
  killed daemon (verified recovery, K36), K34, K37 and `Protection: LOST`.
  Open finding U4: a clean client stop fails at the session revoke
  (`revoke rc=-13 phase=storage-fence`) while any member is down, because
  the storage fence must reach every configured member; the user decides
  whether that is an alpha limitation or a fix.
- [ ] **S4 — Effective settings.** Decide whether **W122**, the listing of
  effective settings, is in alpha. It supports A8 and A10.
  **STRUCTURE Oct 24 ([effective settings](../using/alpha-1.0-surface.md#s4--effective-settings)):**
  where each effective value is visible today (startup log, `status`,
  `raft-status`, `version`, `sync-status`, `commands-policy get`; tunables
  not at all) and the page layout once A6 lands. The A6 inventory exists
  and the A7 removals are done (D42); **waits for** the user's W122
  decision only (described, not decided, in the Oct 25
  [audit](docs-audit-20261025.md)).
- [ ] **S5 — User documentation.** Install, configuration, supported
  topologies, the reduced configuration matrix (A11) and the published
  limitations.
  **DRAFT Oct 24 ([alpha guide](../using/alpha-1.0-guide.md)):** install,
  configuration, topology (3–16 nodes, 2+1, distinct hosts), transport and
  direct I/O, quotas, failure behavior (one node down, rebuild on return,
  two down). The A11 matrix waits for A11. **Revised Oct 25
  ([audit](docs-audit-20261025.md)):** failure behavior brought up to K34,
  K35, K36 and K37 and checked on a loopback cluster; `operations.md`,
  `power-user.md`, `quickstart.md`, `efs-commands.md` and the README no
  longer describe buffered I/O, `EFS_COMMANDS`, splice or `add-storage` as
  current; `v030-candidate.md` is marked historical.

## B. Verify geometry, placement and usable capacity

- [ ] **B1** Check ordinary chunks and packed segments use exactly D1/D2/P, each
  on a distinct eligible node. Verify actual persisted fragment inventories and
  authoritative mappings, not just placement-function outputs.
  **Oct 24 (branch merged into devel (`5a09fd938`)): loopback
  PASS at 3 to 8 members: every chunk row, span and pack object of the test
  files names D1/D2/P on three distinct members, and every store's fragment
  records match them (on the row's node, nowhere else). FIXED: the reader
  decoded a row without valid nodes from placement recomputed now; it is
  refused. Real-host run NOTRUN ([evidence](b-geometry-capacity-20261024.md)).**
- [ ] **B2** Check partial chunks, empty files, alignment/padding, checksums and
  boundary sizes. Reconstruct exact payloads from each valid two-fragment pair;
  reject corrupt or mismatched fragments rather than returning fabricated bytes.
  **Oct 24: loopback PASS: 19 boundary sizes (0 B to 1 MiB + 3) exact
  through every mount; on the stores D1, D2 and P = D1 ^ D2 and every pair
  rebuilds the chunk; a partial chunk is a whole 2+1 span object; a corrupt
  D1, or another chunk's D1, is refused and the bytes come from D2 + P; two
  corrupt fragments are EIO. Unit: every pair for empty/page/half-chunk/
  chunk edges at 128 KiB to 1 MiB chunks and segments. Real-host run
  NOTRUN ([evidence](b-geometry-capacity-20261024.md)).**
- [ ] **B3** With diverse inodes/chunk indexes, confirm 3-node placement uses all
  three; 4-node placement rotates the omitted node; larger placement uses the
  intended node set. Report placement histograms, roles and per-node writes.
  **Oct 24: loopback PASS, grown 3 -> 8 one member at a time: three use all
  three for every chunk, four rotate the omitted member (the two chunks of a
  file omit different members), five to eight give every member every role;
  histograms in the evidence. Unit: exact rotation at four, every role
  within 10 % of the mean at 3 to 8 (dense and gapped ids). Real-host run
  NOTRUN ([evidence](b-geometry-capacity-20261024.md)).**
- [ ] **B4** Audit packing placement separately: record group voters and eligible
  fragment destinations, demonstrating what capacity is actually usable. Do not
  infer packed placement from ordinary-file placement results.
  **Oct 24: loopback PASS: packed segments recorded per pack object; group
  2 votes on {2,3,4} from four members while segment fragments land on
  every member (voters' share 0.73 at four, 0.38 to 0.44 at seven and eight); usable
  packing capacity is the ring's 3-of-N bound, not the voters'. Real-host
  run NOTRUN ([evidence](b-geometry-capacity-20261024.md)).**
- [ ] **B5** Verify logical quota versus physical accounting, small-file overhead,
  reserved headroom and ENOSPC. Report actual usage; 1.5x coding overhead excludes
  metadata, alignment and other storage overhead. No false free-space claims.
  **Oct 24: loopback PASS. FIXED: one full stripe member was acknowledged as
  a degraded two-fragment stripe (base: 200 MiB acknowledged with a 16 MiB
  member full, never ENOSPC); now the object is placed on a member that is
  not full, or the write fails ENOSPC. Measured: whole chunks 1.5x payload
  (1.59x allocated with record pages), a partial chunk a whole stripe
  (192 KiB payload), packed small files about 1.7x; status on every member
  and df on every mount equal the 3-of-N bound, +2/3 GiB per 1 GiB
  addition; three members with one full report 0 free; automatic quota =
  headroom budget. Real-host run NOTRUN ([evidence](b-geometry-capacity-20261024.md)).**
- [ ] **B6** Exercise a deliberately unavailable fragment/destination under a
  bounded controlled test. Reads reconstruct or fail explicitly; writes must
  never report success without their documented durable publication requirement.
  Verify the result after orderly restore/restart, without claiming repair.
  **Oct 24: loopback PASS, not merged (branch `claude/devel-b6-d4-4f65ac`,
  `834f0b7ec`). `tests/test_b6_unavailable.py`: node 3 of three down,
  refusing (new `EFS_FAULT_DEST_FILE`), slow past every client deadline
  (8 s; COMMIT 65 s; GETs 45 s) and full (16 MiB quota), for ordinary and
  packed files during write (one in flight), fsync and read: every read
  exact, fsync only with marks, writes degraded from node 3 only (full:
  ENOSPC, nothing degraded), `Protection: DEGRADED` shown and cleared by
  W154 after the orderly restore, every acknowledged file exact after the
  restore and a whole-cluster restart. FIXED: a packed read waited out a
  slow member (30 s, EBUSY) instead of using the other two fragments; each
  segment fragment now has the 2 s bound. Real-host gate `b6-member-down`
  in `scripts/qualify.py` (loopback PASS); real-host run NOTRUN; no repair
  beyond W154 ([evidence](b6-d4-unavailable-progress-20261024.md)).**

## C. Finish the enabled foreground paths

- [ ] **C1** Core mounted POSIX, extras and two-client suites pass on each
  topology. Unsupported cases have explicit scope and SKIP/NOTRUN, not PASS.
- [ ] **C2** Verify create/write/fsync/close/reopen; random and sequential I/O;
  overwrite, sparse holes, append, truncate, rename, hard links, unlink/open
  handles and session fencing with exact expected contents and metadata.
- [ ] **C3** Exercise concurrent readers/writers on same and separate files,
  independent directories and large hot directories. No unexplained EIO,
  stale payloads, lost names or indefinite EBUSY; legitimate refusals stay visible.
- [ ] **C4** Qualify publication receipts, retries and lost replies on the current
  path. A successful acknowledgement must not outrun durable payload/mapping
  ownership; a retry must not repeat a completed mutation incorrectly.
- [ ] **C5** Run the current apply/protocol/session correctness gates associated
  with W127–W134/W72 and D25 where applicable. Carry forward scoped PASS evidence,
  but verify the integrated candidate and retain remaining limitations.
  **Oct 25: real-host gate `c5-protocol-live` in `scripts/qualify.py`
  ([Stage 2 gates](stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent): W133
  declaration gate and W140 tags over the wire on every member, removed
  settings refused on every host, K5 session fence and W72 unmount revoke.**

## D. Packing, reclaim and enabled `.efs`

- [ ] **D1** Verify small-file packing produces real packed mappings and inventory
  savings, with exact reads before/after packing and after orderly cold restart.
  **Oct 25: real-host gate `d1-packing` in `scripts/qualify.py`
  ([Stage 2 gates](stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent).**
- [ ] **D2** Exercise overwrite/truncate/unlink and compaction of shared segments;
  retained files stay intact, retired references are reclaimed exactly once and
  space eventually becomes reusable without deleting live fragments.
  **Oct 25: `d1-packing` overwrites, truncates and unlinks packed files and
  requests compaction on every topology ([Stage 2 gates](stage2-gates-20261024.md),
  loopback PASS); `gc-reclamation` FAILs on `merge-wave6` and its base
  (`fsync acknowledged after unclean stop`).**
- [ ] **D3** Run `.efs` commands/options and concurrent query suites while ordinary
  I/O continues. Check result correctness, authorization, freshness and bounded
  refusal/retry behavior, including initial catalog creation and restart.
  **Oct 25: real-host gate `d3-efs-load` in `scripts/qualify.py`
  ([Stage 2 gates](stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent).**
- [ ] **D4** Demonstrate bounded progress with packing, indexing and KV maintenance
  active: foreground deadlines pass, memory/queues remain bounded and background
  work progresses. Diagnose repeated stalls, rather than relaxing tests to pass.
  CPU optimizations are required here only when needed to meet these gates;
  broader optimization remains later roadmap work.
  **Oct 24: loopback PASS, not merged (branch `claude/devel-b6-d4-4f65ac`,
  `834f0b7ec`). `scripts/d4_progress.py` (qualify plugin `d4-progress`) and
  `tests/test_d4_progress.py`: small-file load with packing, `.efs` index
  refresh and KV flush/compaction active (counted from new raft-status
  `kv_flushes`/`kv_compactions` and the new pack-compact backlog line);
  every foreground op within 10 s (max 0.87 s), client RSS 147 MiB ≤ 1024,
  server RSS 74 MiB ≤ 2048, memtable and L0 within their design bounds,
  pack backlog 117 ≤ 50,000, index pending age 1.8 s ≤ 60, drain 4.4 s ≤
  150, 2935 files exact (gate run); through qualify on product daemons
  (16 writers, 90 s) PASS, drain 31 s. FIXED (repeated stall): a member whose first
  `.efs` catalog capture ran before formation was refused UNFORMED, waited
  a whole 60 s interval and failed every query EIO meanwhile (104 of 104 in
  the first gate run); now a prompt retry and "not indexed yet". Finding
  for Stage 3: cold packed small-file reads 8–33 ms each. Real-host runs
  NOTRUN ([evidence](b6-d4-unavailable-progress-20261024.md)).**
  **Oct 25 (F1 follow-ups, branch `claude/devel-f1-followups-ae2deb`,
  [evidence](f1-followups-20261025.md)):** a fixed-working-set variant
  (`scripts/d4_progress.py --working-set`, `tests/test_d4_plateau.py`) rewrites
  2048 files, packed and unpacked, for 30 and 60 minutes with no net creation.
  Every D4 bound held (efsd peak 102 MiB of 2048, efs-fuse 197 of 1024), every
  file exact, foreground ops within 10 s; efsd RSS is not flat (F1). The
  20-minute create load now ends with 0 packer put errors (was 3; flake-sweep
  fix `fc79fe086`); the 60-minute rewrite load at its highest rate still
  counted 5. Found and fixed on the rewrite load: three rewrite EIOs from a
  publication unit that skipped its ABORT. The drain after a long rewrite load
  takes 390–415 s (index changes from segment compaction), against 3.5 s
  after the create load; the long runs use a 900 s drain bound.
  **Oct 25 (D4 drain, branch `claude/interesting-mclean-65362a`,
  [evidence](d4-pack-drain-20261025.md)):** the drain stall had two causes,
  both fixed. A small file rewritten across a pack epoch had two candidate
  keys, and a segment holding both failed silently every pass. The D25
  truncate sweep's backlog (about 5 inodes a second) held the index drain.
  The 60-minute run (intel1) drains in 54.5 s within the default 150 s, with
  0 packer put errors (the fragment resend is now bounded by 2 s, not 4
  tries); `test_d4_plateau.py` is back in `make test-long`; 8-job gate PASS
  with host limits. Found then: quota use grew linearly under the rewrite
  load (52 % at 60 minutes on intel1, ENOSPC at about 45 minutes on amd).
  **Oct 26 ([evidence](d4-reclaim-20261026.md)):** the cause was abandoned
  uploads, collectable only at unmount (D31). Each resizing rewrite
  abandoned its first upload because the REPORT host checked an
  unconditional tail image against the client's stale span list. Fixed:
  stored data stays at 118–278 MiB per node over 60 minutes, and the D4
  driver bounds abandoned uploads and stored data. Open: about 3.5 % of
  `O_TRUNC` overwrites still abandon an upload.
  **Oct 26 (Task D soak, loopback, member churn,
  [evidence](task-d-20261025.md#soak-tables-attempt-3-nuc-b2fc06ba5)):** for
  7,195 s with 11 clean returns, a daemon SIGKILL, a client SIGKILL and a
  join, packing advanced on every group-0 leader through each leadership
  change (`packed_total` up to 10,708 per leader term, candidates ≤ 210),
  `.efs` queries stayed under 0.41 s, and every qualifying write-load RSS
  window (5 windows, 37 process rows) stayed within 5 % last-third growth;
  role coverage NOTRUN (the run stopped on K43 first). Not a D4 PASS.

## E. Full verification and orderly lifecycle

- [ ] **E1** Record an exact manifest of successfully acknowledged files and
  expected contents. Verify every payload and its relevant metadata after the
  mixed workload on each topology; sampled checks cannot close this gate.
- [ ] **E2** Cleanly stop clients and unmount without force or lost pending writes;
  drain and stop daemons, restart the same-format stores, establish readiness,
  remount and repeat full verification. Record all stop/readiness deadlines.
  **Client stop on a busy client fixed (Oct 24, [evidence](e2-clean-stop-20261024.md),
  merged into devel (`29226931c`)):** the unmount's
  session revoke is bounded and parallel (2,058 touched shards 63 s → 11 s,
  ~870 shards 8.7 s → 1.5 s on nuc loopback), and `client.sh stop` waits on
  the client's reported stop phases (30 s without progress or 600 s in all
  fail; a failed flush, settle or revoke fails with its reason; no forced
  kill). `nuc-private-3` posix-durability, e1-manifest and e2-client-stop
  PASS (one host); the distinct-host E2 run remains open.
- [ ] **E3** Exercise orderly leader turnover and client reconnect, including
  catch-up of returning members. No stuck stale leader, missing committed state
  or spurious readiness. This is not sudden-power-loss qualification.
- [ ] **E4** Run small-file scale and large-file fixtures with declared counts,
  quotas and concurrency; report actual completed versus target counts. Include
  cold reads, concurrent query load and delete/reclaim checks.
  **Oct 25: real-host gate `e4-scale` in `scripts/qualify.py`
  ([Stage 2 gates](stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent); the
  defaults (5,000 small and 2 × 256 MiB files per member) have not run.**
  **Task D interaction endurance (Oct 26, branch `claude/stoic-goldwasser-06dbca`,
  [evidence](task-d-20261025.md#finish-on-intel2-and-nuc--2026-10-26)): FAIL,
  not acceptance.** The 15-minute rehearsal PASS on intel2 (`4cb79d09d`,
  1,102 s) and nuc (`b2fc06ba5`, 1,166 s); the four-hour soak on nuc
  (`b2fc06ba5`) stopped at 7,195 s, at its 15th event, on product finding
  **K43** (one member's SIGKILL surfaced `EBUSY` to a writer); 14 events and
  seven exact two-mount verifications passed before it, and the retained
  stores read every acknowledged version exact. A harness defect (a
  returning member's documented "not indexed yet") was fixed on the way.
  nuc `make test-all`: candidate 502/2 FAIL/15 NOTRUN, base `39a1cbbbc`
  502/4/15, no candidate-only failure. The `soak` plugin's default is its own loopback fixture, not
  qualification of a parent real-host topology.
- [ ] **E5** Moved to Stage 3 as **P1**.

## P. Stage 3: performance

Starts only after Stage 2 passes on the frozen candidate. Stage 3 is run as
**rounds**. Each round measures, picks the largest in-scope limiter, changes
it on a private build, proves the gain A/B, and produces a new candidate on
which the affected Stage 2 gates are rerun. Rounds continue until the user
calls the result acceptable for alpha or the remaining limiters are all
post-alpha work.

**Scope fence.** Before any change, name the limiter and check it against
the post-alpha list in "Order of work": metadata group count or placement,
leader/voter distribution, 2+2, all-node query service, directory policy,
repair, new formats. Anything on that list is measured and recorded, never
changed in Stage 3. The §9 scaling table of the
[architecture](../how-it-works/architecture.md) is the reference for what
"where did it stop" must name; alpha does not have to meet §9, but every
round says which physical or software limiter it hit.

- [ ] **P1 — Baseline (formerly E5).** On each topology, record throughput,
  operation p50/p95/p99, resource use and per-node activity for the declared
  workloads: the user's tiny-file creation, ior easy/hard, the two-client and
  core suites' timings, delete/reclaim, `.efs` under load. Same candidate,
  same fixtures, three runs each.
- [ ] **P2 — Per-round goal.** Each round names its workload, topology,
  metric and the limiter it attacks, with reference to the §9 row it belongs
  to. No invented thresholds and no promise of linear scaling; a round's
  goal is "this limiter is gone or measurably smaller", not a number picked
  in advance.
- [ ] **P3 — Candidate work, in scope.** Each as an A/B on private builds,
  so no runtime switch is added:
  - **K27**: the ior easy write drop between `08cc16af4` and `d94ca85bf`
    (first, because it may be a regression rather than a limiter);
  - **W124**: fast delete / reclaim;
  - **W126** step 4: KV maintenance interference;
  - findings 3–5 of the [ORCD tiny-file profile](orcd-tiny-profile-20261022.md):
    `.efs` indexing and LSM maintenance; the packing-CPU A/B on orcd-mini;
  - the **W44**/**D26** retained-table GC latency regression;
  - **W53**: the create/close-storm p99 regression (evidence, then the user
    decides);
  - client-side limiters that the baseline shows (FUSE, prefetch,
    connection fan-out), within the current protocol.
  **Measured only, not changed in alpha:** **W66** leader placement and
  group count (record the two-group concentration as the limiter where it
  is one), **W98**, **W81**, 2+2 codec effects.
- [ ] **P4 — Round record.** Before/after results with the exact builds and
  commands, the limiter named, the Stage 2 gates rerun on the new candidate,
  and the decision (keep or revert). A change is kept only if its gain is
  reproduced and the reruns pass. Findings parked for post-alpha go to the
  roadmap with their measurement.

## F. Release evidence and alpha 1.0 exit

- [ ] **F1** Unit/integration, enabled-feature and applicable memory/concurrency
  checks have explicit PASS/FAIL/NOTRUN results. Docs and code tests both report;
  an unavailable diagnostic is disclosed, never silently counted as passed.
  **Memory and concurrency gates run on the candidate Oct 25 (xorinox, branch
  `claude/devel-f1-gates-46ff64` on `merge-wave7`;
  [evidence](f1-memory-concurrency-gates-20261025.md)):** `make leak-check`
  PASS 41/41 (25 + 16 new unit binaries and harnesses), `make asan-check`
  (new: ASan + UBSan + LeakSanitizer over the sanitizer-switch harnesses, the
  new parsers and decoders, and the daemons) PASS 47/47, `make tsan-check` (new:
  ThreadSanitizer over KV, Raft store, client data commit, member view, the
  K36 recovery thread, K37 rebuild, E2 revoke, W155 receipts and the D4 load)
  PASS 18/18 on the last code commit, `make test-all` PASS (exit 0, 861 commands, host NOTRUN lines only). 15 findings fixed: UB in the recovery
  thread, a cookie leak at daemon exit, a misaligned read in `test_wire`, and
  twelve data races (shutdown flags stored plainly and polled atomically, a
  HELLO reading the member store while the member thread rewrote it, the
  startup banner racing the rejoin thread, counters and lazy env flags written
  under different locks or none; 21 W143 lazy flags made atomic), each with
  the gate entry that found it as regression test. 20-minute D4 RSS: bounded
  (client peak 247 MiB of 1 GiB, servers 134 MiB of 2 GiB), not flat while the
  namespace grows; LSan at daemon exits finds no leak. NOTRUN: RDMA memcheck
  (no device on xorinox). **Oct 25 follow-ups, branch
  `claude/devel-f1-followups-ae2deb` ([evidence](f1-followups-20261025.md)):**
  `test_k36_recovery.py` under TSan PASS 10 of 10 (was 7 of 10). The causes
  were the crash hooks' `_exit` (libtsan finalised for a second, so a
  "crashed" daemon took a rebuild PUT; hooks now `exit_group`), a race in the
  `EFS_FAULT_PUT_BUSY_FILE` hook and a case-11 test defect. The RSS plateau
  is measured on a fixed working set (`tests/test_d4_plateau.py`: rewrites
  only, no net creation). **Plateau FAIL, bounded not flat:** the clients
  plateau. efsd's live heap is flat, but glibc arenas held freed memory.
  efsd now caps arenas at 8, fixes the mmap threshold at 256 KiB and trims
  every 15 s, which cuts its 60-minute peak by about a third (101.8 MiB).
  It still grows 8–13 % over minutes 40–60 with throughput, tracking the
  connection-thread high-water mark (open). The packer put errors were
  `NOT_PRIMARY` from a follower's authority read and are covered by the
  flake-sweep fix (`fc79fe086`) on the 20-minute load (3 before, 0 after),
  but not at the highest rewrite rate (5 in 60 minutes; the refusal outlasts
  its 200 ms retry; open). The fixed working set also found rewrite EIOs: a
  W77 publication unit skipped its ABORT when its forwarded PREPARE batch
  was refused. Fixed, with a regression test that fails 5 of 5 without the
  fix. Gates: `make test-all` 480 commands, 0 FAIL, 14 NOTRUN host limits (`04408a3e4`); `make tsan-check` PASS 18/18 and `make asan-check` PASS 47/47 (`758dd144c`, the same code). Also open, each seen
  once: a late finisher abort (one EIO in 33 748 rewrites on a doubly loaded
  host) and one foreground EBUSY after five failed SETATTR receives.
- [ ] **F2** Triage each failure on the selected candidate and repeat affected
  acceptance after fixes. Retain failed evidence and explain harness corrections.
  No unresolved enabled-path corruption, lost acknowledged state, indefinite
  progress failure or unexplained mounted-I/O failure may be called PASS.
- [ ] **F3** Complete the topology ledger below, link exact reproduction commands
  or committed drivers and evidence summaries. Raw captures stay outside Git.
  **Oct 25 Task A rehearsal complete with findings:** [three- and five-member AMD loopback](task-a-20261025.md)
  both full runs completed (21 PASS / 1 harness FAIL / 1 NOTRUN); the GC
  sequencing correction passes affected coverage, with a separate product
  revoke failure retained. Code gate: 479 commands, 11 FAIL (base's subset),
  6 NOTRUN. This remains one-host regression evidence; the physical-topology
  ledger and product findings stay open.
- [ ] **F4** Publish alpha limitations: fixed metadata-group count and
  default placement of two three-voter groups (both on 1–3 for N = 3; on
  1–3 and 2–4 for N ≥ 4, D38 as amended), actual packing placement, supported
  node range, Linux FUSE client only, deferred unplanned repair, degraded
  state shown only to the operator (`efs-mgmt status` has shown it since
  W154; the mount does not, finding F10), and lack of 2+2/two-loss
  protection on 4+ nodes.
  **DRAFT Oct 24 ([release notes](../release-notes/alpha-1.0.md)):**
  limitations L1–L28 with trigger, what the user sees and reference, and
  the open K rows that must close or be added before release.
  **Revised Oct 25 ([audit](docs-audit-20261025.md)):** checked against
  D40–D42 and the K rows at `ec3e09306`: L6, L7, L8 and L28 now describe
  verified recovery and segment rebuild (K36, K37) instead of "deferred";
  the open-items paragraph lists K34–K37 as fixed on loopback with live
  runs NOTRUN; L22 and L24 are marked as open user decisions (F12, W122).
  Release-notes entries decided so far (D40, Oct 23), one each:
  - **K10/W121 readback after fsync:** a read issued right after a
    successful fsync can return the previous file's payload; triggered by
    the FUSE kernel fork/page-pinning read path, no client workaround in
    alpha ([diagnosis](orcd-w121-fork-read-20261016.md)).
  - **Erasure profile:** 2+1 only, on 3, 4 and more than 4 nodes.
  - **Membership:** online addition required; online removal deferred (A13).
  - **Clients:** Linux FUSE client only (no NFS re-export, no macOS; W152).
  - **Repair:** automatic repair is deferred except W154 rebuild-on-return of
    stripes acknowledged with two fragments.
  - **Power loss:** no power-loss (sudden host power-off) qualification.
- [ ] **F5** Review the integrated candidate and approve alpha 1.0 only when all
  mandatory in-scope gates pass. Identify any excluded platform/feature explicitly;
  a required gate left NOTRUN blocks that support claim. Then perform the
  separately authorized tag/main merge/push workflow.

| Evidence | 3 physical nodes | 4 physical nodes | >4 physical nodes (record N) |
| --- | --- | --- | --- |
| Known-defect register K (Stage 1; once per candidate) | OPEN | (same) | (same) |
| Functional scope S decided (once per candidate) | OPEN | (same) | (same) |
| Candidate and profile | NOTRUN | NOTRUN | UNSUPPORTED until K28 |
| Flag audit and A/B removal (A6–A10, A12; once per candidate) | NOTRUN | (same) | (same) |
| Reduced-matrix qualification (A11) | NOTRUN | NOTRUN | NOTRUN |
| Placement/capacity and decode | NOTRUN | NOTRUN | NOTRUN |
| POSIX and concurrent I/O | NOTRUN | NOTRUN | NOTRUN |
| Packing/shared-fragment GC | NOTRUN | NOTRUN | NOTRUN |
| Enabled `.efs` under load | NOTRUN | NOTRUN | NOTRUN |
| Full payloads + orderly restart | NOTRUN | NOTRUN | NOTRUN |
| Scale/progress evidence (E4, D4) | NOTRUN | NOTRUN | NOTRUN |
| Performance baseline and targets (P1–P2, Stage 3) | NOTRUN | NOTRUN | NOTRUN |
| Alpha topology acceptance | OPEN | OPEN | OPEN |

These are candidate-specific release cells, not an assertion that no earlier
tests passed. For example, [the October 22 orcd-mini report](orcd-mini-deploy-08cc16af4-20261022.md)
contains useful RDMA/direct-I/O POSIX, D25 and reclaim results, but its three
daemons share node9901; it also records harness revisions, a docs failure and
unavailable diagnostics. It does not fill this multi-host 3/4/>4 matrix wholesale.

## After alpha 1.0

The [D38 ladder](decisions.md): alpha 2.0 adds 2+2 metadata protection on at
most four nodes (the D36 four-node protocol, W66/W74); alpha 3.0 adds
configurable 2+1 metadata placement on N − M nodes; alpha 4.0 the same at 2+2.
More metadata groups and distributed ownership (W66/W98/W81), 2+2 data
stripes, broader background-work efficiency and integrated scale acceptance
follow the same path. D36 remains the final protection contract; alpha 1.0 is
explicitly the 2+1 milestone: default three-voter metadata groups both on
1–3 for N = 3, on 1–3 and 2–4 for N ≥ 4, and data placement across all nodes.
