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
| K2 | **W140** proposal RPC accepts metadata tags this build cannot apply (halts a group) | safety | FIXED, merged Oct 22 (`a5eb7e6ff`): one allow-list (`EFS_MD_CMD_APPLIED`) drives the dispatcher and every proposal path; unknown tags refused PROTO at submit; drift test |
| K3 | **W132** `NODE_LEFT` from any connection drops a node | safety | FIXED, merged Oct 22 (`1fad577e2`): message 24 retired and refused fail-closed; handler, struct and remover deleted |
| K4 | **W133** client protocol declaration gate is fail-open | correctness | FIXED, merged Oct 22 (`918ed9207`): every frame but CLIENT_HELLO and HEARTBEAT needs an accepted declaration; exact protocol version (4 after K5); peers, mgmt, bench and live tests declare. VERIFY: `w125_p3_compat.sh` on hardware NOTRUN |
| K5 | **W134** unregistered session identities bypass fencing (with **W72** production session lifecycle) | correctness | FIXED, merged into devel Oct 22 (`a443863b1`): one session check at every apply (`efs_session_accept`: UNREGISTERED or FENCED, no legacy acceptance, `efs_session_accept_legacy` deleted); the mount establishes a session, admits each touched shard and latches fenced; every metadata mutation carries the session (protocol 4); `efs-mgmt` raw mutations establish their own. VERIFY: the fence cluster gate (`scripts/k05_session_fence_gate.sh`) and the NUC/AMD live harnesses are NOTRUN for the parent session; partitioned-client and leader-change barrier gates stay W72 work **REOPENED Oct 22 (S2 P1, S3 P2): `efs_meta_execute_mtime_owned` (`src/meta/append_plan.c`) checks the session, then reads the session row again and uses that unverified image as the CAS base, so a fence landing between the two reads lets the old epoch's mtime transaction through (wrapper counterexample `scripts/reviews/k05_mtime_fence_capture.py`); and the client's `session_fenced`/`session_started` latches mix plain writes with atomic reads. FIX: validate epoch and floor from the captured image before preparing it (as the append path does); make the latch accesses consistently atomic; TSan test**. **S2 and S3 FIXED, merged into devel Oct 22 (`3bcde5e2a`): `468513a96` authorizes every session-owned executor from its captured row (mtime, APPEND reservation/completion, barrier retirement, `efs_session_prepare_owner`); `42c631852` makes both latches atomic. Barrier test with real session rows, the review reproducer and a TSan check/fence harness (with negative control) run in `make test`; `make test` PASS on efs1. Fence cluster gate NOTRUN. [Evidence](k05-s2-s3-20261022.md)**  **Third review at `0eb1511d2`: code gap OPEN — T1: append-abandon captures a fenced session row without validating it; see [review](devel-integration-review-20261022.md#third-pass--merged-fixes-at-0eb1511d2).** T1 FIXED, merged Oct 23 (`9a88e332e`): abandon authorizes from the captured session row; barrier test PASS, `make test` on amd PASS with three pre-existing host exclusions, fence cluster gate NOTRUN. [Evidence](k05-t1-abandon-capture-20261022.md) |
| K6 | **W127–W131** fail-closed apply (in devel since `08cc16af4`; private RDMA gate PASS) | correctness | VERIFY multi-host |
| K7 | **W83** transaction decision records are never retired | lifecycle / growth | FIXED, merged Oct 22 (`907a5891f`): replicated `TXN_RETIRE` compacts decisions to tombstones and reclaims them after a 60 s retention from a per-shard cursor; unknown decisions answer STALE (readers retry), never the old value; a sweeper re-drives orphaned decisions. Note: retention must stay above the 5 s recovery age and clock skew (no assert yet) **REOPENED Oct 22 (R2 P2, R3 P2): `efs_txn_scan_decisions` always restarts at the prefix with a 32-record page, so 32 unresolvable live decisions starve every later one; the extended DECIDE and TXN_RETIRE decoders accept trailing bytes (`clen <` minimum instead of exact). FIX: per-shard continuation with wraparound for the live-decision sweep; exact lengths and field validation, malformed entries halt the group through the unsupported-entry path; tests through the real apply**. **R2 and R3 FIXED, merged Oct 22 (`85d8dd438`): per-shard continuation cursor with wraparound (`HOST_DEC_SCAN_WORK` 256 keys per pass; the recovery mark stays armed until a full clean page), exact DECIDE/TXN_RETIRE lengths with shard and verdict validation (malformed entries halt through the unsupported-entry path), retention ≥ 10× the recovery age asserted; tests run through the real apply in `make test`. Follow-up noted by the session: the RESOLVE, DROP and PREPARE decoders still accept a minimum length, not an exact one.** |
| K8 | **W148** modify-under-load returned wrong bytes, correct size, on a never-packed 3.6 MB file | data correctness | FIXED, merged Oct 22 (`71ce4ff35` on devel). Root cause: `dcache_apply_init` let a partial write with a loaded base keep the slot's UNCOND base, so the flush published the write-time image (from an unrefreshed local chunk entry: zeros or a peer's lost bytes) as the whole chunk with no merge and no CAS. Reproduced 3× in 9 attempts on node9901 (RDMA, devel `7c510c60f`); a cold read from a fresh client returned the same bad bytes. The two-client regression (`tests/posix/posix_2client.py`) fails 3/3 before and passes 3/3 after. VERIFY: the w125 modify rerun on the fixed build is NOTRUN; the kept W148 stores and hashes are in the session hand-over ([review](review-pack-validate-once-20261022.md)) |
| K9 | **W87** buffered concurrent append loses records under load | data correctness | **INCOMPLETE** (Oct 22): not reproduced (23 oracle rounds, 19,597 records, churn/GC/POSIX load on efs2; VM run, not formal evidence; [attempt](w87-append-oracle-efs2-20261022.md)); no root cause, no fix. The exact oracle (`tests/live/append_oracle.py`, `append_oracle_cluster.sh`) and `tests/test_append_oracle.py` are in devel for the parent session to run on the real clusters. **Xorinox (Oct 23, merged Oct 23 (`306a01fd4`)): not reproduced in 110 oracle rounds (84,480 acknowledged records, 27 with three-daemon restart) under churn, GC and POSIX load; all exact, no write errors; W87 stays OPEN ([evidence](gates-xorinox-20261022.md#k9--w87-concurrent-o_append-oracle)).** |
| K10 | **W121** a read right after a successful fsync returns the previous file's payload (FUSE kernel fork/page-pinning; kernel fix deferred) | data correctness | DECIDE |
| K11 | **W55** span committed but fragment PUTs never landed | data correctness | VERIFY Linux FUSE. NFS re-export and macOS clients are **DECIDED out of alpha** (user, Oct 22): beta backlog **W152**; their W55 triggers are not isolated in alpha |
| K12 | **W58** `O_EXCL` create answered EEXIST (historical trigger not isolated) | correctness | VERIFY |
| K13 | **W88** a folded span retry resurrects after history eviction; **W86** default PUT/flush ticket integration staged | publication correctness | FIX or DECIDE |
| K14 | **W77** atomic multi-chunk publication and observation | integration gap | FIX or DECIDE |
| K15 | **W64** append reservation replay across leader changes; **W52** REPORT after many `O_APPEND` writes exceeds 30 s | correctness | **INCOMPLETE** (merged Oct 22). W52: shape A was already in tree (`32b472c5b`); the live gate harness `tests/live/append_failover.py` is in devel; the 20k gate and the before/after A/B are NOTRUN. W64: a coordinator that lost its group mid-transaction left PREPAREs until the 5 s recovery scan (writes failed EBUSY); the finisher now re-proposes ABORT at the new leader (`test_fin_abort_handoff.py`), `efs-mgmt raft-step-down` added; the live failover gate is NOTRUN. Lost-reply cleanup merged Oct 22 (`a51832413`): a reservation granted with a lost reply retires decide-once, by session fence (FENCED_HOLE) or the live owner's abandon of that identity (receipt STALE, ABORTED_HOLE); late completion refused, completion never overtaken (`test_append_cleanup`); the lost-reply live gate (`append_failover.py --lose-reserve-every`) is NOTRUN. [Evidence](k15-w52-w64-20261022.md) Live gates on intel2, Oct 23, devel `163241584`: lost-reply gate FAIL (integrity held in every view and the 4 injected losses retired cleanly, but a load stall with no leader change caused 25 extra failed writes, 2 BUSY abandons and 1 extra hole), W52 20k FAIL indication (26 REPORT receive timeouts after 2,120 of 20,000 records, interrupted), failover gates, second runs and A/B NOTRUN ([live gates](k15-live-gates-20261022.md)). T1 FIXED, merged Oct 23 (`9a88e332e`): append-abandon authorizes from the captured session row (`session_owner_capture`), fenced or unregistered owners refused before any prepare; barrier test with three interleavings PASS. |
| K16 | **W75** identity-bound fragment integrity format (fresh format, no compatibility) | integrity | FIXED, merged Oct 22 (`e9ca0d07b`): fragment record format 2 binds identity and payload digest; GET refuses as INTEGRITY, GC and segments verify identity; old roots refuse startup. **Fresh format required at the next deploy.** VERIFY: RDMA and hardware gates NOTRUN ([evidence](k16-w75-identity-format-20261022.md)) |
| K17 | **0a/D27** STALE replay convergence: orcd-mini C1 rerun | correctness | VERIFY |
| K18 | **W54**, **W38** fold/GC data loss: multi-host hardware RDMA gate | correctness | VERIFY |
| K19 | **W61**, **W62**, **W63** GC: final drain and lost-ledger reconciliation | reclaim correctness | VERIFY |
| K20 | **W42** capacity reporting on any node count; protection debt (with B5) | correctness | VERIFY |
| K21 | **W119** concurrent RDMA failures and blocked clean stop (recorded Oct 15, before many fixes); **W65** graceful-shutdown wait | correctness / liveness | VERIFY first, then FIX |
| K22 | **W23** universal follower memory/lag bound | resource bound | FIXED, merged into devel Oct 22 (derived KV memory budget, unapplied-log bound with busy AppendEntries replies, leader admission at 3/4, leadership requires capacity per D30; `efs-mgmt raft-status` shows the bound). VERIFY: the production-scale plateau measurement, snapshot import into a stalled follower and multi-host/RDMA runs are NOTRUN for the parent session ([summary](../../results/measure/20261022-k22-follower-bound/SUMMARY.md)) **REOPENED Oct 22 (S1 P1): `low_capacity` counts `last_index − last_applied`, which includes entries not yet known committed; after a leader loss or a restart with a retained tail every voter can be vetoed from campaigning, nothing clears it, and no messages are sent (reproduced: `scripts/reviews/k22_capacity_tail.c`, three voters, 96-entry tails, cap 128, zero messages in 1,000 ticks). FIX: distinguish an uncommitted tail that needs an election from inability to apply; allow recovery leadership with bounded headroom while admission stays held; a delivering three-voter test that elects, commits the tail and resumes**. **S1 FIXED, merged Oct 22 (`001b5da30`): `low_capacity` counts only `commit_index − last_applied`, so an uncommitted tail never vetoes a campaign; `capacity_hold` retries `apply_committed`; the three-voter delivering test in `test_follower_bound` elects, commits a 96-entry tail and resumes; `make test` PASS on an efs VM.** **Live on xorinox (Oct 23, merged Oct 23 (`306a01fd4`)): PASS (scoped) for a 302 s plateau at fault-build caps (32,768 entries, 8 MiB budget, follower RSS flat at about 100 MB, quorum commit rate unchanged) and for SIGSTOP stall and AppendEntries catch-up; NOTRUN: the production-scale plateau (about 6 h here), snapshot import (needs a 1 GiB log lag) and live leader admission at 3/4 ([evidence](gates-xorinox-20261022.md#k22--w23-follower-memory-and-lag-bound)).** |
| K23 | **W71** hardware power-loss gate; **W74** automatic repair; **W123** crash recovery | durability scope | DECIDE (deferred by current scope) |
| K24 | **W149** `EFS_KV_COMPACT_DIE` exits the daemon in normal builds | build hygiene / safety | FIXED, merged Oct 22 (`19555889f`): the hook exists only in `EFS_FAULTS` builds, one gate form everywhere, crash-recovery test moved to `tests/faults` |
| K25 | **W135**–**W139**, **W141**, **W120**, **W150** test harness and reporting defects | test correctness | FIXED, merged Oct 22 (`35647dd0f`): docs gate out of `make test` (`make check` runs both), every unit binary and Python test wired or listed with a reason, TSan gate runs or fails, NOTRUN counted by the summary runner, condition-based waits, W150 test added. W137 waits on K13 **REOPENED Oct 22 (R1 P1): `scripts/make_test_summary.sh`'s runner never exits non-zero on failures or NOTRUN, so automation reads a failed campaign as success (reproduced). FIX: failing exit status on any failure, a distinct incomplete status for required NOTRUN gates, runner self-tests; also make every test honour `TMPDIR`**. **R1 FIXED, merged Oct 22 (`6df08cc3a`;** [evidence](k25b-summary-exit-tmpdir-20261022.md)**):** runner exit 0 PASS / 1 FAIL / 3 INCOMPLETE with self-tests, test scratch under `TMPDIR` and removed on success; `make test` PASS twice on efs2 |
| K26 | LeakSanitizer aborts on node9900/node9901; F1 needs a working leak check. On the efs1–3 build VMs (Fedora 42, aarch64, 1 GB RAM, tmpfs `/tmp`): `test_reclaim` and `tests/test_server_processes.py` failed on pristine devel and on every K branch. Cause found Oct 22: efsd's capacity headroom rule (`storage_capacity.h`, 20% free) refuses the 972 MB tmpfs `/tmp` and the 88%-full root disk ("Storage quota: cannot establish safe headroom; refusing startup"); both PASS with `TMPDIR=/data1/tmp` (the 20 GB XFS data disk), the full documented toolchain was applied on all three on Oct 22 (kickstart package set, devops build deps, `devops/profiling/HOST-SETUP.md`: perf, bpftrace, sysstat, fio, iperf3, blktrace, numactl, trace-cmd, dwarves, gdb, valgrind, sanitizer runtimes, glibc debuginfo, profiling sysctls, memlock, perf group with tracefs, cache-drop helper); Tart VMs expose no hardware PMU, so `perf` samples `cpu-clock`, not `cycles` (the W138 TSan gate failed without `libtsan`), and the root disks run full (old build trees under `~/build` must be deleted). Tests that hard-code `/tmp` instead of honouring `TMPDIR` (for example `test_query_disk`) leave stale `efs-*` directories that fill the tmpfs (857 found on efs3 on Oct 22); making every test honour `TMPDIR` and clean up is a K25-class harness item. The merged devel of Oct 22 was otherwise clean on efs1 | environment | **Leak gate DONE on `devel-k26` (`2fea76916`), merged Oct 22 (`b8ef49e58`) ([evidence](k26-leak-gate-20261022.md)):** `make leak-check` runs 25 unit binaries (the four ASan-only tests plus KV, Raft, store, txn, session, lock) under valgrind memcheck. It prints PASS/FAIL per binary and a verdict, exits 1 on any definite leak and 3 (`NOTRUN: valgrind missing`) without valgrind, and `make check` runs it. **PASS on efs2 and efs1 (25/25, about 6 min each); `make test` PASS (185 s).** No product leak; two test defects fixed or isolated. LSan works on efs1–3 (`ptrace_scope` 0) and fails only where `ptrace_scope` is 2 (ORCD). NOTRUN: `make leak-check` on node9901 (x86_64). Open: W126 peeks wait on `mu` while a writer is queued on `view_rw` |
| K27 | ior easy write on `d94ca85bf` is 710–863 MiB/s against 1,329 MiB/s on `08cc16af4`, same cluster, three runs, reads unchanged ([deploy report](orcd-mini-deploy-d94ca85bf-20261022.md)) | regression, unattributed | VERIFY (A/B/A′ on orcd-mini, needs a restart) |
| K28 | **W151** the code caps a cluster at four nodes (`EFS_MAX_NODES`, `EFS_MD_RAFT_N`, voter sets, placement ring, client hint bound); the user keeps >4 nodes in alpha 1.0 (decision Oct 22) | implementation gap / scope | FIXED, merged Oct 22 (`a2942cb0f`). Plan: raise the cap with voter sets for n ≥ 5, placement and capacity tests at 5..max, membership and client tables; packed segments on the data ring across all N nodes (D38); fresh format, no compatibility. **Implemented: cap 16, three voters per group on nodes 1–4, segments on the data ring with a remote pack-object read; unit tests and `make test` PASS on efs1; live 5-node gate NOTRUN ([evidence](k28-w151-cluster-cap-20261022.md))** T3 closed (user, Oct 22): D38 amended to two three-voter groups on nodes 1–4; the K28 layout stands. **Live 5-node gate PASS on xorinox (Oct 23, merged Oct 23 (`306a01fd4`)): five members, voters on 1–3 and 2–4, fragments and packed segments on all five stores, node 5 admits and reclaims segments through the remote pack-object read, one older file returned EIO during the node 1 outage (F1); all files read correctly after rejoin; finding F1 (a stripe acknowledged with two fragments in the window before a node is marked down is never re-protected, so one later single-node outage gave EIO until that node returned; deferred repair scope) ([evidence](gates-xorinox-20261022.md#k28--w151-five-node-cluster)).** |
| K29 | Append path stalls under sustained load with no leader change: on intel2 (K15 live gates, two private three-daemon clusters plus the user's cluster on one 8-CPU host) both lanes fell from 10 to about 0 records/s for 10 minutes; REPORT and APPEND_RESERVE round trips reached 30 s (26 and 12 receive timeouts), `txn-recover` aborted coordinated appends after 10–17 s (EBUSY/EIO), two abandons returned BUSY, the group-0 log grew about 57 Raft entries per 4 KiB record, followers rejected AppendEntries `prev-beyond-log` through the whole run, leader `apply-sleep` max 7 s. Integrity held in every view. | availability / performance | VERIFY first on a dedicated host, one lane at a time, host iowait sampled; then FIX. Found Oct 23 ([K15 live gates](k15-live-gates-20261022.md)); cause not established (host load, software stall or both). The harness also needs a rule for abandons caused by real timeouts (today every non-injected failure and its hole count as a mismatch). Also seen on nuc (W148 rerun, TCP, direct I/O, i5): 20–30 s server stalls under the w125 load produced 30 s receive timeouts, opid-admission BUSY and ambiguous-completion EIO/EBUSY in 6 of 9 attempts ([gates-nuc](gates-nuc-20261022.md)); same class, see W155. |
| K30 | Intermittent `make test` failures on devel on amd (32 CPU; Oct 23, merged `9b38af915`, full suite 1,038 commands: 4 FAIL, 14 NOTRUN; isolated reruns in parentheses): `tests/test_direct_read_fork_oracle.py` fork+COW case `FAIL files=6400 prefill=1 rounds=2 read_repeats=100` (2 of 3; K5/T1 saw 2 of 6 on unmodified devel); `tests/test_reclaim` case "old meta/ copy fails closed": `reclaim: journal verification FAILED in .../meta/gc-journal; data PUTs refused until recovery` three times, then the `server_reclaim_init(&S)==EFS_OK` assertion at `test_reclaim.c:118` (2 of 3), so the copied meta/ is being refused where the test expects a clean boot; `tests/test_kv_lsm` `test_kv_lsm.c:1057: peeks return while a reader holds the view` (1 of 3), the `test_peek_beside_reader` liveness case the K26 session had to skip under memcheck, which is open **W126** (peeks wait on `mu` while a writer is queued on `view_rw`), so that one is a product ordering defect, not a test flake. Also `tests/test_client_processes.py` needs root on any host running a foreign `efs-fuse` whose argv has a relative mount path (`scripts/client_processes.py` reads the process cwd). | test correctness | VERIFY then FIX: reproduce each on a quiet host with counts; an assertion in `test_reclaim` on unmodified devel is either a test race or a real reclaim bug and must be told apart. Found Oct 23 ([K5/T1 evidence](k05-t1-abandon-capture-20261022.md)). |

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
| K6 | Merged. Remaining: the W130 one-replica DIR fault injection, and multi-host runs. |
| K7 | Fixed Oct 22 (`907a5891f`); R2/R3 fixed and merged Oct 22 (`85d8dd438`); see the K row. |
| K8 | Fixed Oct 22 (`71ce4ff35`); see the K row. |
| K9, K13, K14 | K9 INCOMPLETE (harness merged, not reproduced); K13, K14 await the user's scope decision; K15 and K16 are fixed (see their rows). |
| K10 | Open. User-deferred kernel fix; test harnesses now isolate it (`exclusive_create_visibility.py --spawn`). |
| K11, K12, K17–K20 | VERIFY. The fixes are recorded on NUC/AMD/TCP or private stores; the alpha profile on separate hosts is NOTRUN. |
| K21 | W119's evidence (Oct 15) predates the RDMA, query, apply and Raft fixes since then. Rerun its original gates before diagnosing. W65 is closed for TCP only; it overlaps K1 part 4. |
| K22 | Fixed Oct 22 (`28883f2b1`); S1 fixed and merged Oct 22 (`001b5da30`); see the K row. |
| K23 | Deferred by the current alpha scope; needs the user's confirmation. |
| K24 | Fixed Oct 22 (`19555889f`); see the K row. |
| K25 | Fixed Oct 22 (`35647dd0f`); R1 fixed and merged Oct 22 (`6df08cc3a`); see the K row. |
| K26 | Gate done and merged Oct 22 (`b8ef49e58`): `make leak-check` PASS on efs2 and efs1 ([evidence](k26-leak-gate-20261022.md)); NOTRUN on node9901. x86_64: `make leak-check` PASS 25/25 (291 s) on nuc at `163241584`, merged Oct 23 (`2213a4120`) ([evidence](gates-nuc-20261022.md)). |
| K29 | Open (found Oct 23 by the K15 live gates); needs a dedicated-host rerun. |
| K30 | Open (found Oct 23 on amd); intermittent on unmodified devel. |

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
K17–K21, K27. Waiting on the user: K10, K13, K14, K23. K26 closed as a gate:
`make leak-check` (valgrind memcheck, 25 unit binaries) merged (`b8ef49e58`),
PASS on efs2, NOTRUN on node9901. The W125 harness gap (cleanup,
remote and final-verification evidence, self-tests) is merged (`56b7e48fe`).
The merged devel passed `make test` with zero error lines on efs1 at
`95beaa053`, 927/927 on efs2 at K5's HEAD, and (K8 session, Oct 22 evening)
rc 0 on efs1 at `4090e717b`, and rc 0 with zero error lines (15 NOTRUN, no
kept scratch) on efs3 at `0eb1511d2`, the tree with every second-wave merge
(`TMPDIR=/data1/tmp`; a one-line TMPDIR fix to K7's sweep test was needed
after K25's hygiene gate met it).

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
  target profile; containers and expiry remain disabled. If any of those choices
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
- [ ] **A7 — Decide and remove alternatives.** For behavior-changing A/B switches,
  select and qualify one supported implementation or automatic policy. Remove the
  losing code path, parser/environment reader and obsolete tests/docs together.
  Do not merely hide the flag, change its default or force every switch on.
  No replacement compatibility switch or fallback to the retired behavior.
- [ ] **A8 — Keep only justified configuration.** Retain deployment inputs such as
  nodes, disks, addresses and quotas. Prefer built-in or automatically derived
  internal batching/concurrency/maintenance settings; justify any remaining
  operator tuning with a concrete operational need and a bounded test scope.
  Keep observability separate from behavior selection. Fault injection belongs
  in test builds and must not be a normal deployment option. Record a disposition
  for every inventoried setting; intentional transport/platform support is not
  automatically an obsolete A/B implementation.
- [ ] **A9 — Metadata leadership.** Remove `EFS_METADATA_BALANCE` as an operator
  choice after selecting and qualifying the automatic policy. Do not force its
  present common-voter/co-located-leader preference on as a substitute for that
  decision. Preserve quorum, safe transfer and catch-up; expose actual leadership
  diagnostically. This does not claim the later many-group design is complete.
- [ ] **A10 — Operational consistency.** Update launch/deployment scripts, portal
  settings, help and generated settings listings with each removal. Stale launch
  configurations must be detected by validation rather than silently presented
  as effective settings. No new deployment should require a hidden toggle to
  obtain the qualified behavior.
- [ ] **A11 — Reduced qualification matrix.** Publish the remaining supported
  configuration dimensions and rationale. Qualify the selected paths under the
  topology/profile gates below, including automatic-policy boundaries, load
  changes and orderly restart. Report before/after behavior-switch counts and
  reachable configuration counts where defensible; do not claim exhaustive
  coverage from testing flags individually or from pairwise tests alone.
- [ ] **A12 — Closure.** Every A/B selector has either been removed with its
  alternative implementation or is an explicitly justified, reviewed support
  dimension under A8. No unexplained experimental, legacy or correctness-policy
  toggle remains in the alpha product. Record failures and gate results for the
  integrated candidate; flag removal alone is not correctness evidence.

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
  (`.efs` on, containers and expiry off). A7 must decide whether the disabled
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
  background packing (**W125**), logical truncate (**D25**), the `efs-mgmt`
  operator commands, and the start/stop/deploy scripts. Candidates out of
  scope: containers, expiry (**W90**; resolve under A7), 2+2, automatic repair
  and crash recovery (K23). **Decided out (user, Oct 22):** NFS re-export and
  macOS clients, on the beta backlog as **W152**; alpha supports the Linux FUSE
  client only, and F4 says so.
- [ ] **S2 — POSIX subset.** Publish the supported operations and every known
  gap (for example, mmap is skipped in the extras suite). Each gap is either
  fixed or justified as a limitation (F4). No gap counts as PASS.
- [ ] **S3 — Operator workflows.** Format, start, stop, status, `df`,
  quotas, `raft-status`, `raft-resync`, packing control, and failure
  messages. Each one is documented and covered by a test.
- [ ] **S4 — Effective settings.** Decide whether **W122**, the listing of
  effective settings, is in alpha. It supports A8 and A10.
- [ ] **S5 — User documentation.** Install, configuration, supported
  topologies, the reduced configuration matrix (A11) and the published
  limitations.

## B. Verify geometry, placement and usable capacity

- [ ] **B1** Check ordinary chunks and packed segments use exactly D1/D2/P, each
  on a distinct eligible node. Verify actual persisted fragment inventories and
  authoritative mappings, not just placement-function outputs.
- [ ] **B2** Check partial chunks, empty files, alignment/padding, checksums and
  boundary sizes. Reconstruct exact payloads from each valid two-fragment pair;
  reject corrupt or mismatched fragments rather than returning fabricated bytes.
- [ ] **B3** With diverse inodes/chunk indexes, confirm 3-node placement uses all
  three; 4-node placement rotates the omitted node; larger placement uses the
  intended node set. Report placement histograms, roles and per-node writes.
- [ ] **B4** Audit packing placement separately: record group voters and eligible
  fragment destinations, demonstrating what capacity is actually usable. Do not
  infer packed placement from ordinary-file placement results.
- [ ] **B5** Verify logical quota versus physical accounting, small-file overhead,
  reserved headroom and ENOSPC. Report actual usage; 1.5x coding overhead excludes
  metadata, alignment and other storage overhead. No false free-space claims.
- [ ] **B6** Exercise a deliberately unavailable fragment/destination under a
  bounded controlled test. Reads reconstruct or fail explicitly; writes must
  never report success without their documented durable publication requirement.
  Verify the result after orderly restore/restart, without claiming repair.

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

## D. Packing, reclaim and enabled `.efs`

- [ ] **D1** Verify small-file packing produces real packed mappings and inventory
  savings, with exact reads before/after packing and after orderly cold restart.
- [ ] **D2** Exercise overwrite/truncate/unlink and compaction of shared segments;
  retained files stay intact, retired references are reclaimed exactly once and
  space eventually becomes reusable without deleting live fragments.
- [ ] **D3** Run `.efs` commands/options and concurrent query suites while ordinary
  I/O continues. Check result correctness, authorization, freshness and bounded
  refusal/retry behavior, including initial catalog creation and restart.
- [ ] **D4** Demonstrate bounded progress with packing, indexing and KV maintenance
  active: foreground deadlines pass, memory/queues remain bounded and background
  work progresses. Diagnose repeated stalls, rather than relaxing tests to pass.
  CPU optimizations are required here only when needed to meet these gates;
  broader optimization remains later roadmap work.

## E. Full verification and orderly lifecycle

- [ ] **E1** Record an exact manifest of successfully acknowledged files and
  expected contents. Verify every payload and its relevant metadata after the
  mixed workload on each topology; sampled checks cannot close this gate.
- [ ] **E2** Cleanly stop clients and unmount without force or lost pending writes;
  drain and stop daemons, restart the same-format stores, establish readiness,
  remount and repeat full verification. Record all stop/readiness deadlines.
- [ ] **E3** Exercise orderly leader turnover and client reconnect, including
  catch-up of returning members. No stuck stale leader, missing committed state
  or spurious readiness. This is not sudden-power-loss qualification.
- [ ] **E4** Run small-file scale and large-file fixtures with declared counts,
  quotas and concurrency; report actual completed versus target counts. Include
  cold reads, concurrent query load and delete/reclaim checks.
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
- [ ] **F2** Triage each failure on the selected candidate and repeat affected
  acceptance after fixes. Retain failed evidence and explain harness corrections.
  No unresolved enabled-path corruption, lost acknowledged state, indefinite
  progress failure or unexplained mounted-I/O failure may be called PASS.
- [ ] **F3** Complete the topology ledger below, link exact reproduction commands
  or committed drivers and evidence summaries. Raw captures stay outside Git.
- [ ] **F4** Publish alpha limitations: fixed metadata-group count and
  default placement of two three-voter groups (both on 1–3 for N = 3; on
  1–3 and 2–4 for N ≥ 4, D38 as amended), actual packing placement, supported
  node range, Linux FUSE client only, deferred unplanned repair, no
  degraded-state indication, and lack of 2+2/two-loss protection on 4+ nodes.
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
