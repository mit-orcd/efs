# Phase 1 — alpha 1.0: finish 2+1 on 3, 4 and larger clusters

**User-directed release scope, October 22, 2026. Status: OPEN; no release
candidate or topology acceptance recorded by this checklist.**

**Current state (Nov 2):** source: devel at the latest wave gate (`99fb88ea7`, wave 52, [November ledger](2026-11/ledger.md)). Verdict: OPEN; no release candidate or topology acceptance recorded. Remaining failures: the open and VERIFY rows of section K (blocks-alpha flag per row; K88 is FIXED, blocks alpha: no). Active run: none claimed here; see [in-flight.md](in-flight.md) and the [Stage 1 matrix](stage1-acceptance-matrix-20261030.md). Next action: run the matrix's acceptance campaign, then close K rows against their named gates.

Phase 1 finishes and qualifies the existing **two data fragments plus one parity
fragment** architecture at every supported cluster size. Completion is the
**alpha 1.0** milestone. Afterwards comes [alpha 1.1 architectural
consolidation](alpha-1.1-consolidation.md), followed by the remaining
[distributed-cluster roadmap](2026-10/distributed-cluster-roadmap-20261022.md).
Alpha 1.1 does not move or waive any task or release gate in this checklist.
The Stage 1 cross-path due-diligence tasks AY–BB below are correctness audits,
not a requirement to complete alpha 1.1 restructuring before alpha 1.0.
This is release sequencing, not a claim that the final D36 design is implemented.
No tag, merge, push, deployment or data deletion is performed by this document.

**Decision (October 22):** the user keeps >4-node clusters in alpha 1.0;
2+2 and broader metadata distribution follow alpha 1.0. The four-node cap is
therefore alpha work (K28/W151), a prerequisite of A3.

**Review (October 22, devel `165689c06`):**
[plan and code review](2026-10/alpha-1.0-plan-review-20261022.md). Main finding: the
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
  except where a correctness gate needs it (bounded progress, D4), and the
  explicitly authorized Stage 1 hot-path tasks AB–AX below (Oct 28).
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

Current canonical states (Task Y, reviewed devel `9e4d66a8c`):

- **FIXED:** merged implementation and scoped regression evidence; not full alpha acceptance.
- **VERIFIED:** the row's required gate recorded on the frozen candidate; none is newly awarded here.
- **VERIFY:** implementation/safeguards recorded, with the named live recheck or acceptance still owed. For historical incidents such as K11/K12/K21, this does not assert a proven original root cause.
- **DECIDED:** user scope decision, with its D number and limitation.
- **CLOSED:** explicit closure with its reason; not necessarily a code fix.
- **OPEN:** unresolved issue, with `blocks alpha: yes/no` and owner.

Each status cell starts with one plain canonical word, then current evidence,
then its unmodified prior history. A historical FIX/NOTRUN/not-merged phrase
is not the current verdict. Contradictions and scopes are explicitly listed in
[Task Y's reconciliation](2026-10/k-register-reconciled-20261028.md); that report does
not override an evidence-page verdict. **FIXED is not VERIFIED**, and a
structurally reconciled register does not close Stage 1.

Historical October 22 legend (retained):

- **FIX:** an open bug. Fix it, add a regression test, and pass the item's
  gate.
- **VERIFY:** the fix is in devel, but its gate is not recorded on the alpha
  profile (RDMA, direct I/O, `.efs` and packing on, distinct hosts). Run the
  gate on the candidate.
- **DECIDE:** a scope question for the user. Either fix it, or record it as
  an alpha limitation (F4).
- **TEST:** a harness or reporting defect. Fix it before relying on that
  test's verdict.

<!-- K-REGISTER:BEGIN (generated by scripts/check_k_register.py --write) -->
| ID | Title | Kind | State | Blocks alpha | Row |
| --- | --- | --- | --- | --- | --- |
| K1 | efsd stop is unbounded and reports a forced exit as success (W145) | Raft safety / liveness | VERIFY | unstated | [K001](k/K001.md) |
| K2 | Proposal RPC accepts metadata tags this build cannot apply | safety | VERIFY | unstated | [K002](k/K002.md) |
| K3 | W132 `NODE_LEFT` from any connection drops a node | safety | VERIFIED | unstated | [K003](k/K003.md) |
| K4 | Client protocol declaration gate accepts undeclared clients | correctness | VERIFY | unstated | [K004](k/K004.md) |
| K5 | Unregistered session identities bypass fencing | correctness | VERIFY | unstated | [K005](k/K005.md) |
| K6 | Metadata apply is not fail-closed (W127-W131) | correctness | VERIFY | unstated | [K006](k/K006.md) |
| K7 | W83 transaction decision records are never retired | lifecycle / growth | VERIFIED | unstated | [K007](k/K007.md) |
| K8 | W148 modify-under-load returned wrong bytes, correct size, on a never-packed 3.6 MB file | data correctness | VERIFIED | unstated | [K008](k/K008.md) |
| K9 | W87 buffered concurrent append loses records under load | data correctness | CLOSED | unstated | [K009](k/K009.md) |
| K10 | W121 a fork racing another thread's FUSE direct-I/O read can leave stale bytes in the caller's buffe | data correctness | DECIDED | unstated | [K010](k/K010.md) |
| K11 | Span committed but fragment PUTs never landed (W55) | data correctness | VERIFY | unstated | [K011](k/K011.md) |
| K12 | O_EXCL create answered EEXIST | correctness | VERIFY | unstated | [K012](k/K012.md) |
| K13 | Folded span retry resurrects after history eviction (W88, W86) | publication correctness | VERIFY | unstated | [K013](k/K013.md) |
| K14 | Multi-chunk publication is not atomic (torn reads, W77) | integration gap | VERIFY | unstated | [K014](k/K014.md) |
| K15 | Append reservation replay across leader changes; slow REPORT (W64, W52) | correctness | VERIFY | yes | [K015](k/K015.md) |
| K16 | Fragment integrity is not bound to identity (W75) | integrity | VERIFY | unstated | [K016](k/K016.md) |
| K17 | STALE replay convergence: C1 passed, pub-stale criterion open (0a, D27) | correctness | VERIFY | unstated | [K017](k/K017.md) |
| K18 | Fold/GC data loss (W54, W38): distinct-host RDMA gate not run | correctness | VERIFY | unstated | [K018](k/K018.md) |
| K19 | GC final drain and lost-ledger reconciliation (W61, W62, W63) | reclaim correctness | VERIFY | unstated | [K019](k/K019.md) |
| K20 | Capacity reporting wrong on any node count; silent protection debt (W42) | correctness | VERIFY | unstated | [K020](k/K020.md) |
| K21 | Concurrent RDMA failures and blocked clean stop (W119, W65) | correctness / liveness | VERIFY | unstated | [K021](k/K021.md) |
| K22 | Raft follower memory and lag are not bounded (W23) | resource bound | VERIFY | yes | [K022](k/K022.md) |
| K23 | W71 hardware power-loss gate; W74 automatic repair; W123 crash recovery | durability scope | DECIDED | unstated | [K023](k/K023.md) |
| K24 | W149 `EFS_KV_COMPACT_DIE` exits the daemon in normal builds | build hygiene / safety | VERIFIED | unstated | [K024](k/K024.md) |
| K25 | W135–W139, W141, W120, W150 test harness and reporting defects | test correctness | VERIFIED | unstated | [K025](k/K025.md) |
| K26 | LeakSanitizer aborts on node9900/node9901; F1 needs a working leak check | environment | VERIFIED | unstated | [K026](k/K026.md) |
| K27 | ior easy write on `d94ca85bf` is 710–863 MiB/s against 1,329 MiB/s on `08cc16af4`, same cluster, thr | regression, unattributed | CLOSED | unstated | [K027](k/K027.md) |
| K28 | W151 the code caps a cluster at four nodes (`EFS_MAX_NODES`, `EFS_MD_RAFT_N`, voter sets, placement | implementation gap / scope | FIXED | unstated | [K028](k/K028.md) |
| K29 | Append path stalls near zero for minutes without a leader change | availability / performance | VERIFY | unstated | [K029](k/K029.md) |
| K30 | Intermittent `make test` failures on devel on amd (32 CPU; Oct 23, merged `9b38af915`, full suite 1, | test correctness | VERIFIED | unstated | [K030](k/K030.md) |
| K31 | Devel RDMA clients lose larger replies | correctness / transport, alpha blocker | FIXED | unstated | [K031](k/K031.md) |
| K32 | ior easy write fell from ~1.3 GiB/s to ~0.36–0.5 GiB/s after the fourth wave; reads unchanged | performance regression (Stage 3 by D50) | DECIDED | no | [K032](k/K032.md) |
| K33 | Simple startup and safe online node addition incomplete (W157) | scope / implementation gap, alpha blocker | VERIFY | unstated | [K033](k/K033.md) |
| K34 | Acknowledged files unreadable (EIO) while one member stops in order | single-loss availability, alpha blocker | VERIFY | unstated | [K034](k/K034.md) |
| K35 | fsync returns EIO while any storage member is down (F22) | availability / durability contract, alpha blocker | VERIFY | unstated | [K035](k/K035.md) |
| K36 | Store stays recovery uncertain after an unclean stop; fsync EIO (F23) | durability / availability, alpha blocker | VERIFY | unstated | [K036](k/K036.md) |
| K37 | Packed segment with a missing fragment is never re-protected | durability, alpha blocker | VERIFY | unstated | [K037](k/K037.md) |
| K38 | ior hard shared-file write returns EIO after a STALE REPORT (W161) | data correctness / error resolution, alpha blocker | VERIFY | yes | [K038](k/K038.md) |
| K39 | write and fsync return EIO with one member down on a 4-member cluster | data path / availability with one member down, alpha blocker | VERIFY | unstated | [K039](k/K039.md) |
| K40 | Two FUSE clients writing one shared file get EIO from unit BUSY (W162) | data path / publication unit ABORT, alpha blocker | VERIFY | unstated | [K040](k/K040.md) |
| K41 | Packer writes a segment degraded to a member stopped 27 s earlier | single-loss durability of acknowledged small files, alpha blocker candidate | VERIFY | unstated | [K041](k/K041.md) |
| K42 | Clean unmount fails its session revoke with BUSY (append-reap-owed) | clean client stop (E2), alpha blocker | VERIFY | yes | [K042](k/K042.md) |
| K43 | write returns EBUSY and close EIO when one of six members is SIGKILLed | data path / availability with one member down | VERIFY | yes | [K043](k/K043.md) |
| K44 | Retired-seed MKFS answers BUSY after online addition (W167) | membership / management routing (K33), liveness | VERIFY | yes | [K044](k/K044.md) |
| K45 | A follower that had installed a snapshot received a delayed AppendEntries whose previous index lay b | Raft safety / availability | FIXED | yes | [K045](k/K045.md) |
| K46 | A replica with `raft-resync` requested could still win an election | Raft safety | FIXED | yes | [K046](k/K046.md) |
| K47 | A packed read with one returned (down-marked) holder answering NET, one valid fragment and one absen | single-loss availability (packed files) | FIXED | yes | [K047](k/K047.md) |
| K48 | Healthy-quorum listing misses a name with one replica halted (W130) | directory enumeration / availability with one replica halted | VERIFY | yes | [K048](k/K048.md) |
| K49 | RDMA request dispatch uses misaligned casts and UBSan stops the daemon | RDMA request safety / availability | VERIFY | yes | [K049](k/K049.md) |
| K50 | RMDIR returns EBUSY after a fixed sixteen-attempt admission BUSY cap | namespace availability under pressure | VERIFY | yes | [K050](k/K050.md) |
| K51 | Task L | shutdown lifetime safety | VERIFIED | yes | [K051](k/K051.md) |
| K52 | Task V | read-after-write correctness | DECIDED | no | [K052](k/K052.md) |
| K53 | ORCD dd-profile full gate (fcstor007, `3769f44bb`) | quota admission correctness | CLOSED | yes | [K053](k/K053.md) |
| K54 | Member-down clean unmount | clean stop with one member down | FIXED | yes | [K054](k/K054.md) |
| K55 | Task AI real-RDMA full gate on fcstor015 | slow-member fsync availability / bounded durable marking | FIXED | unstated | [K055](k/K055.md) |
| K56 | `make asan-check` fails five tests at startup (`env_removed`, `k36_recovery`, `k36_segment_recovery` | sanitizer tooling / startup | VERIFIED | no | [K056](k/K056.md) |
| K57 | Task AY audit | upload completion / segment placement | VERIFIED | no | [K057](k/K057.md) |
| K58 | Task AZ audit | durability / destructive action on unproven evidence | VERIFIED | no | [K058](k/K058.md) |
| K59 | Task BA audit | deletion non-resurrection / packed storage | VERIFIED | no | [K059](k/K059.md) |
| K60 | Task BA audit | quota accounting / recovery and reclaim | VERIFIED | no | [K060](k/K060.md) |
| K61 | Task BA audit | deletion non-resurrection / ordinary objects | FIXED | no | [K061](k/K061.md) |
| K62 | Task BB | client metadata / data visibility | FIXED | yes | [K062](k/K062.md) |
| K63 | Task BB | client mutation authority | FIXED | yes | [K063](k/K063.md) |
| K64 | Task BB | permission / namespace authority | FIXED | yes | [K064](k/K064.md) |
| K65 | Task AN | storage PUT / correctness | VERIFIED | no | [K065](k/K065.md) |
| K66 | Task EIO | append / correctness | FIXED | yes | [K066](k/K066.md) |
| K67 | Task EIO | truncate / latency | VERIFIED | no | [K067](k/K067.md) |
| K68 | Task EIO | read cache / performance | VERIFIED | no | [K068](k/K068.md) |
| K69 | Task EIO | client OPEN / correctness | FIXED | yes | [K069](k/K069.md) |
| K70 | Task EIO | client CREATE / correctness | FIXED | yes | [K070](k/K070.md) |
| K71 | Task EIO | client lease cleanup / liveness | VERIFIED | no | [K071](k/K071.md) |
| K72 | Task AE | availability / logging | FIXED | no | [K072](k/K072.md) |
| K73 | Pre-existing reclaim-journal `rj.recovered` increment raced its status reader (TSan). | diagnostic counter data race | VERIFIED | no | [K073](k/K073.md) |
| K74 | Task AO | directory visibility / correctness | FIXED | yes | [K074](k/K074.md) |
| K75 | Task AO | metadata migration / availability | FIXED | yes | [K075](k/K075.md) |
| K76 | Task AO | metadata migration / correctness | VERIFIED | no | [K076](k/K076.md) |
| K77 | Task AO | locks / correctness | VERIFIED | no | [K077](k/K077.md) |
| K78 | Task AO | namespace admission / availability | VERIFIED | no | [K078](k/K078.md) |
| K79 | Task BI/AE | availability / logging | FIXED | no | [K079](k/K079.md) |
| K80 | Task BI/AB | availability / logging | FIXED | no | [K080](k/K080.md) |
| K81 | Task BL | availability / logging | FIXED | no | [K081](k/K081.md) |
| K82 | Task BR (batch F rehearsal, TSan lane) | recovery / data race | FIXED | no | [K082](k/K082.md) |
| K83 | Task BR | client / data race | FIXED | no | [K083](k/K083.md) |
| K84 | Task BR | kv / undefined behaviour | FIXED | no | [K084](k/K084.md) |
| K85 | Task BM | observability / segment rebuild | FIXED | no | [K085](k/K085.md) |
| K86 | Task CE | session admission / namespace create under load | FIXED | no | [K086](k/K086.md) |
| K87 | Task CI | durability / PUT syncer | FIXED | no | [K087](k/K087.md) |
| K88 | Task CJ | client write publication / single-writer data loss, alpha blocker | FIXED | no | [K088](k/K088.md) |
| K89 | Task DA | raft / commit propagation latency | FIXED | no | [K089](k/K089.md) |
| K90 | Task DA | client unmount / drain after a refused write | FIXED | no | [K090](k/K090.md) |
| K91 | Task DH | membership / HELLO relay, stop latency | FIXED | no | [K091](k/K091.md) |
| K92 | Task DH | client read / stale-map retry latency | FIXED | no | [K092](k/K092.md) |
| K93 | Task DH, TSan on `tests/test_member_limit.py` (node9900, base `104eb5e10` and fixed tree alike) | server membership store / data race | FIXED | no | [K093](k/K093.md) |
| K94 | Task DH | daemon peer pool / RDMA setup under a spent read-proof budget | FIXED | no | [K094](k/K094.md) |
| K95 | Task DJ | mgmt status / metadata group leader | FIXED | no | [K095](k/K095.md) |
| K96 | Task DQ (follow-up of Task DK's `efs-bench` measurement) | peer pool / PUT authority / availability | FIXED | no | [K096](k/K096.md) |
| K97 | Task DR (follow-up of Task DQ) | client / data race | FIXED | no | [K097](k/K097.md) |
| K98 | Task DS (follow-up of Task DR) | server / data race | FIXED | no | [K098](k/K098.md) |
| K99 | Follow-up of Task DS | server / data race | FIXED | no | [K099](k/K099.md) |
| K100 | Task DX (follow-up of Task DU, K94 "Limits") | server, client / transport | FIXED | no | [K100](k/K100.md) |
| K101 | Task EC (found by the before-reading of the `.find` result cache ownership pilot) | client / FUSE `.find` | FIXED | no | [K101](k/K101.md) |
| K102 | Failed write of a removed inode is retried forever; clean stop refused | client write-back / clean stop, alpha blocker | VERIFY | yes | [K102](k/K102.md) |
| K103 | Task FB (found while diagnosing K38, Nov 10) | performance / Raft read path | FIXED | no | [K103](k/K103.md) |
| K104 | Wave 70 merge gate (Nov 10, node9900, `b4bafedae`) | availability with one member unresponsive | FIXED | no | [K104](k/K104.md) |
| K105 | Packer read error on a fresh small file fails D4 (cause inferred) | packer / small-file read, possibly data availability | VERIFY | no | [K105](k/K105.md) |
| K106 | Request in flight to a halted member waits the 30 s transport timeout | availability with one member unresponsive | OPEN | yes | [K106](k/K106.md) |
| K107 | Wave 72 merge (Nov 10, amd, TSan lane) | server / Raft host view, TSan lane | FIXED | no | [K107](k/K107.md) |
| K108 | Snapshot import holds the whole key diff in memory (13.41 GiB peak) | server / resource bound | VERIFY | yes | [K108](k/K108.md) |
| K109 | A stalled member's storage-fence error fails every clean unmount | server, client / clean stop with one member slow | VERIFY | yes | [K109](k/K109.md) |
| K110 | fsync returns EIO after sequential SIGKILLs of members | durability proof / fsync availability across member restarts | OPEN | yes | [K110](k/K110.md) |
| K111 | fsync stays EIO for good after a second kill inside a barrier window | durability proof / fsync availability across member restarts | OPEN | no | [K111](k/K111.md) |
| K112 | One of three RDMA stop loops refused 20 of 20 clean stops (20 s budget) | clean stop on a healthy cluster / transaction retry | VERIFY | yes | [K112](k/K112.md) |
| K113 | Snapshot rejoin never completes under sustained load (import restarts) | server / Raft snapshot rejoin under load | VERIFY | yes | [K113](k/K113.md) |
| K114 | Reply waits outside the K106 (a) cut still last the 30 s timeout | availability with one member unresponsive | OPEN | yes | [K114](k/K114.md) |
| K115 | TCP receive ignores its receive timeout under an RPC deadline | client / transport bounds | OPEN | no | [K115](k/K115.md) |
| K116 | After a restart older folded L0 values win over a newer flush | kv / LSM manifest order at load | FIXED | no | [K116](k/K116.md) |
| K117 | Recovery ledger sweep removed the temp file of a seal in progress | server / recovery sweep (K36) | FIXED | no | [K117](k/K117.md) |
| K118 | Local fragment rebuild skipped the degraded-mark check and retire mark | server / rebuild onto the rebuilding node (rest of K61) | VERIFY | no | [K118](k/K118.md) |
| K119 | A failed snapshot import blocks the member until a leader sends an image | server / Raft snapshot import, failed state | OPEN | no | [K119](k/K119.md) |
| K120 | A blocking FLOCK that is sent again queues a second waiter | client, metadata / file locks under retry | OPEN | no | [K120](k/K120.md) |
| K121 | A mutation the caller gave up on can still be applied later | client, metadata / late execution after a failed retry | OPEN | yes | [K121](k/K121.md) |
| K122 | A late REPORT copy can publish a superseded object over a newer one | client, metadata / publication under retry | OPEN | yes | [K122](k/K122.md) |
| K123 | TSan lane: packer moves none of the refused files after a member returns | test lane / packer after a member returns (TSan only) | FIXED | no | [K123](k/K123.md) |
| K124 | TSan: unsynchronized retire counters (finisher against recovery thread) | data race on statistics counters (TSan lane) | FIXED | no | [K124](k/K124.md) |

Current counts: 47 FIXED / 21 VERIFIED / 40 VERIFY / 3 CLOSED / 4 DECIDED / 9 OPEN (124 rows).
<!-- K-REGISTER:END -->


**BB branch scope:** K57/K58 rows and their linked summaries are copied
unchanged from devel `126ba81d4` for numbering continuity. Their upstream
server fixes are not included or revalidated in this pre-wave-37 BB candidate.

### Recheck: candidate register reconciliation (October 28)

Current first sentences mirror the main register at reviewed devel
`9e4d66a8c` (wave 28); they are not new live-gate verdicts. Historical
October 22 recheck: devel `ab1920fae`, code/backlog reading only, no cluster
run; at that time no row was found already fixed. Its original findings
remain after each current sentence. Grouped rows are expanded; missing
standalone rows K15/K16/K27/K28 are added without inventing historical runs.

Each row's finding is in its file under [k/](k/README.md).

**Oct 23 evidence qualification:** nuc capture bundles are reported lost;
xorinox raw outputs were deleted. Their PASS labels below are historical
reported scoped outcomes. Independent raw-evidence revalidation is
**INCOMPLETE**, and gates requiring those captures need retained reruns.
This does not invalidate separate prior runs or change recorded FAIL/NOTRUN
outcomes. See the [response review](2026-10/devel-integration-review-20261022.md#review-of-the-gate-review-response-2be72f2c2--october-23).

### Stage 1 tally, end of Oct 22 (adjusted after the integration review)

*Oct 30 note: this tally is historical. Wave notes (historical; the counts above are generated): Current counts (43 FIXED / 21 VERIFIED / 37 VERIFY / 3 CLOSED / 4 DECIDED / 5 OPEN, 113 rows, after wave 76 (Nov 10): K113 new and OPEN from the wave 76 merge gate, a member rejoining by snapshot restarts its import for every newer snapshot and never finishes under sustained load (blocks alpha: yes); Task FO's K109 fix is accepted but held out of devel until K113 is fixed, so K109 stays OPEN; K112 diagnosed by Task FQ ([page](2026-11/k112-stop-refused-txn-abort-20261110.md)), no fix, stays OPEN; wave 75 before it changed no count (K106 parts (b) and (c) fixed, part (a) open). From this wave on the register is one file per row ([k/](k/README.md), Task T1): the counts are the generated line above, and later wave notes go to the ledger, not here. OPEN are K106, K109, K111, K112, K113. Before that 43 FIXED / 21 VERIFIED / 37 VERIFY / 3 CLOSED / 4 DECIDED / 4 OPEN, 112 rows, after wave 74 (Nov 10): Task FK's fix ([page](2026-11/k105-packer-read-error-20261110.md)), K105 from OPEN to VERIFY, cause inferred, blocks alpha: no; Task FN's fix ([page](2026-11/k108-snapshot-import-memory-20261110.md)), K108 from OPEN to VERIFY, the production-scale recheck owed, still blocks alpha; Task EY's batch D on candidate `9bb50fe79` ([page](2026-11/batch-d-distinct-host-20261109.md)): verdict FAIL, dated clauses on 31 rows, no status change from it, and K112 new and OPEN from its third clean-stop loop (blocks alpha: yes); Task T7's interface-test cases changed no K row; OPEN are K106, K109, K111, K112. Before that 43 FIXED / 21 VERIFIED / 35 VERIFY / 3 CLOSED / 4 DECIDED / 5 OPEN, 111 rows, after wave 73 (Nov 10): Task FH's K110 barrier proof ([page](2026-11/k110-fsync-after-sigkill-20261110.md)), K110 new and VERIFY, the distinct-host H1 kill rerun owed, blocks alpha until it passes; K111 new and OPEN, the residual stripe that fix leaves at `EIO` (blocks alpha: no); Task FM's fix ([page](2026-11/k107-view-reader-spin-20261110.md)), K107 from OPEN to FIXED; Task FP's fix ([page](2026-11/k42-append-reap-owed-20261110.md)), K42 from OPEN to VERIFY, the RDMA 4-member clean-stop rerun owed, still blocks alpha; Task EZ's batch S on candidate `9bb50fe79` ([page](2026-11/batch-s-soak-20261109.md)): S1 four-host soak PASS, S2 K22 plateau PASS, dated clauses on K22, K29 and K43, and K108 (rejoin memory through snapshot import) and K109 (a stalled member fails clean unmounts) new and OPEN, both blocks alpha: yes (coordinator's ruling); Task FI's K32 attribution ([page](2026-11/k32-ior-easy-attribution-20261110.md)) and the structure-review tasks T3, T5, T6 and T8 changed no K row; OPEN are K105, K106, K108, K109, K111. Before that 42 FIXED / 21 VERIFIED / 33 VERIFY / 3 CLOSED / 4 DECIDED / 4 OPEN, 107 rows, after the K42 reopening (Nov 10): K42 from VERIFY to OPEN, its live clean-stop gate failed on Task EY's 4-member RDMA cell (`reason=append-reap-owed` for the whole 20 s cleanup budget), blocks alpha: yes; OPEN are K42, K105, K106, K107. Before that 42 FIXED / 21 VERIFIED / 34 VERIFY / 3 CLOSED / 4 DECIDED / 3 OPEN, 107 rows, after D50 (Nov 10): K32 from OPEN to DECIDED, moved to Stage 3 by the user ([D50](decisions.md#d50--k32-moves-to-stage-3-user-nov-10)); OPEN are K105, K106, K107. Before that 42 FIXED / 21 VERIFIED / 34 VERIFY / 3 CLOSED / 3 DECIDED / 4 OPEN, 107 rows, after wave 72 (Nov 10): Task FG's K103 fix ([page](2026-11/k103-read-probe-20261110.md)) and Task FF's K104 fixes ([page](2026-11/ao-halted-replica-stall-20261110.md)), both from OPEN to FIXED; K106 new and OPEN, the halted-member waits Task FF saw and the routing fixes do not remove (blocks alpha: yes, coordinator's ruling); K107 new and OPEN from the wave 72 TSan lane, group-view readers that starve the publisher under TSan (blocks alpha: no); Task FE's B6 room test fix ([page](2026-11/b6-room-enospc-20261110.md)), Task FD's pump_logging floor change ([page](2026-11/pump-logging-floor-20261110.md)) and Task FJ's stop.sh patch page ([page](2026-11/k01b-stop-sh-patch-20261110.md)) changed no K row; OPEN are K32, K105, K106, K107. Before that 40 FIXED / 21 VERIFIED / 34 VERIFY / 3 CLOSED / 3 DECIDED / 4 OPEN after wave 71 (Nov 10): K105 new and OPEN from the wave 71 merge gate, one packer read error on a young small file in `test_d4_progress.py`; Task FC's K102 fix ([page](2026-11/k102-stuck-client-20261110.md)), K102 from OPEN to VERIFY, the orcd-mini clean stop owed, still blocks alpha; the batch F harness fixes ([page](2026-11/batch-f-harness-fixes-20261109.md): GC fixture scan race, io bench start window, k41 placement-chance retry) changed no K row. Before that 40 FIXED / 21 VERIFIED / 33 VERIFY / 3 CLOSED / 3 DECIDED / 4 OPEN after wave 70 (Nov 10): K104 new and OPEN from the wave 70 merge gate, a 30 s stall and `EBUSY` in `test_ao_readdir_live.py` with one replica halted, seen only under gate load since wave 69; Task EW's orcd-mini batch O on candidate `9bb50fe79` ([page](2026-11/orcd-mini-batch-o-20261109.md)): K8 VERIFIED on that candidate, K32 reopened, K102 new and OPEN (client stuck on a deleted file); K26 and K51 VERIFIED at the merge, because batch F and batch O together meet their whole acceptance on that candidate; Task FB's K38 fix ([page](2026-11/k38-recurrence-20261110.md)): K38 from OPEN to VERIFY, the orcd-mini ior hard rerun owed, still blocks alpha; K103 new and OPEN, the Raft read probe a compacted follower always rejects (found by Task FB, blocks alpha: no). Before that 41 FIXED / 18 VERIFIED / 35 VERIFY / 3 CLOSED / 3 DECIDED / 1 OPEN after wave 69 (Task EX batch F on candidate `9bb50fe79`, Nov 9; Task FA's A8 audit, 18 tunables built in, 14 test-only, 8 justified, changed no K row): K56 and K73 VERIFIED on F2; the 16 F1-only rows VERIFIED on candidate `9bb50fe79` on F1 gate run 3 (594 run, 0 FAIL) after runs 1 and 2 each failed one load-sensitive test, kept as findings, not waived; `9bb50fe79` is no longer the release candidate (K38 recurred on orcd-mini, K102 is new), so batch F reruns on the fixed candidate and re-checks these rows; the other F rows keep their status with a dated clause ([batch F](2026-11/batch-f-candidate-20261109.md)); after wave 68, no K row changed: the apply visibility contract (Task ET), contract-to-case maps in the owner guides (Task EU), the reading-set method with validation and necessary expansion reported apart (Task EV); after wave 67, no K row changed: the k37 degraded-count `begin` keeps the latest apply's marks (Task EK), `EFS_FUSE_PREFIX` alone drives the fuse3 build (EL), the seven sequence binaries on the sanitizer lists (EM), 40 budget rows refit down from the wave 60 to 65 gates (EN), no second K101-shaped cache (EO), stop files already cleaned since Task DI (EP), every evidence script cited (EQ), two open.c splice tests migrated onto the interface test (ER), host-limit NOTRUN rows marked and COMPLETE in the gate summary (ES); after wave 66, no K row changed: the code graph's `tested_by` edges follow `tests_for.py`'s resolver (Task EF), one current-status block per active W171 page (Task EH), the unmount report line and the GC watermark derive no longer counting a follower's inserts twice, both written by blind readers (Task EI), the K54 fence test asserts an added revoked-epoch fragment, not an unchanged file set (Task EJ); after wave 65: K101 FIXED (a `.find` operation could be served another group set's result or find no slot after a concurrent fetch replaced it, found by the before-reading of the `.find` cache ownership pilot, Task EC; the cache is private to `find_res.c`), the ReadIndex entry map and baseline before the `raft_host.c` split (Task DO), guide-count drift check, full tag audit and prefix tags (Task ED), the Nov 9 review into briefs EF to EI (Task EE), a case filter on the seven sequence test binaries (Task EG); after wave 64, no K row changed: client RDMA reply waits see a crashed member's TCP close and the `test_k36_recovery` row 40 to 35 s (Task DZ), the W154 degraded-mark count no longer counts marks applied while unknown twice (`test_k37_segment_rebuild` step 3, Task DY), `test_k96_status_slots_live` runs one round (Task EA), the open-reference table owned by `open.c` behind `open.h`, per-component test tags and component guides in the code graph (Task EB); after wave 63: K100 FIXED (an auto-mode RDMA upgrade that fails after its setup request was sent destroys the connection, Task DX), the W154 and K37 rebuild skips driven by the failed call's cost instead of a fixed 10 s and the `test_k36_recovery` row 55 to 40 s (Task DY, no K row); after wave 62: K93 FIXED (member-store identity read under `h->mu`, Task DV), K94 FIXED (pooled RDMA peer setup on at least the connect bound, a spent caller fails before sending, Task DU), K99 FIXED (`frag_loc_cache` relaxed atomics, follow-up of Task DS), K38 distinct-host RDMA shared-chunk acceptance PASS on fcstor010-014 but K38 stays OPEN pending the orcd-mini `ior hard` rerun and the user's acceptance (Task DW); after wave 61: K98 FIXED (`host_export_salt` TSan race, Task DS), the write-back `ready` flag and start-failure join under the pool mutex (Task DT); after wave 60: K96 FIXED (STATUS forwards held the read-proof slots, Task DQ), K97 FIXED (`rpc_resolve_ms` TSan race, Task DR), write-back pool state private behind `writeback.h` (Task DM), the `test_k38_protection_now` 30 s stall traced to the packer back-off (Task DP, no production change); after wave 59: K95 FIXED (status protection race, Task DJ), K88 hint kept per inode and chunk (Task DK), `meta_apply.c` split into `src/meta/meta_apply/` (Task DL); K93 and K94 OPEN, K38 fixed for the image-base stall and proven on real daemons by Task DG, still OPEN pending the orcd-mini `ior hard` and RDMA shared-chunk reruns) and the acceptance owed per row are in the [Stage 1 acceptance matrix](stage1-acceptance-matrix-20261030.md).*

**Additional alpha blocker W153:** the unset/empty metadata count silently
selects four nodes and can remove single-loss tolerance on three-node
clusters. Resolve authoritative bootstrap membership and strict configuration
validation, then pass the startup and single-loss gates in the
[W153 review](2026-10/devel-integration-review-20261022.md#review-of-a76b63f42--w153-is-an-alpha-correctness-gate).
The existing K28 implementation and T3 layout decision remain credited;
W153 is not closed by their voter-mask unit tests.


Fixed and merged into devel, unit-tested, live gates NOTRUN: K2, K3, K4, K8,
K16, K24, K28 (W151: cap 16, segments on the data ring; 5-node live gate
NOTRUN). **Reopened by the
[integration review](2026-10/devel-integration-review-20261022.md) and fixed again,
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

- [ ] **BC — Unlink in a loaded parent: avoid the `recompute_times_up` child rescan.**
  Found by AW ([evidence](2026-10/aw-child-vector-index-20261028.md)). `rollup_sub_under`
  (`src/meta/metadata.c`) ends in `recompute_times_up`, which rescans every child
  of the parent (and each ancestor) to recompute exact time bounds. At 1 M
  children this is 64-78 % of unlink time, so AW's O(1) child-vector deletion does
  not change end-to-end unlink latency, and D removals stay O(D²). AW tried an
  exact-skip fast path and reverted it: the convergence check caught a stale
  `parent_tmin`. **Measure first** (comparisons and unlink ns at D = 1 K/100 K/1 M
  with the child-vector index in place); a fix must keep the time bounds exact or
  document the allowed staleness, with the AW convergence check as a gate. Do not
  silently expand AW.

### Stage 1 additional letter tasks — observability interference (AB–AF)

**User-directed Stage 1 addition, October 28.** AA is already assigned to W170;
these five tasks continue at **AB**. All are **OPEN; implementation and measured
qualification NOTRUN**. Source review at `3c866b7bf` identified the paths below;
it did not measure a slowdown or demonstrate a correctness failure. These are
lettered investigation/repair tasks, not new confirmed K defects and not changes
to the K-register counts. If a task demonstrates a correctness failure, register
it in K with its retained reproduction. The unrelated P3-A–P3-F data/metadata
performance tasks remain in Stage 3.0. The graphify fixer retains ownership of
the separate data-commit correctness fixes.

- [x] **AB — Remove diagnostic output from the Raft critical section.**
  `src/server/raft_host.c::host_pump` calls `host_obs_dump` before releasing
  `h->mu`; with `EFS_RAFT_OBS` enabled, it formats and prints role/term and
  periodic statistics while holding the main Raft mutex. Capture a bounded
  snapshot under the proper owner locks; format and emit off the critical
  section and pump path, coordinating with AE. Preserve event identity/order
  and important failure/role-change evidence. Gate: deterministic slow/full log
  sink, concurrent proposals and status, shutdown and sanitizer checks; measure
  mutex hold/wait and Raft progress with diagnostics enabled and disabled.
  **Oct 30 outcome ([AB/AE page](2026-10/ab-ae-raft-logging-20261030.md)):** the periodic dump
  already left `h->mu` with the wave 39 reporter (K72). Task BI moved the last two
  lock-held writers behind the same reporter: term/role events (bounded numeric
  capture under `h->mu`, format and submit after unlock) and the applier's session
  events, whose synchronous `SESS_BEGIN` line made a checked unmount fail
  `revoke rc=-13` with a blocked sink (K80, fixed). Gate on amd: `test_pump_logging`
  (blocked sink, concurrent writes and stat, role events, unmount with the sink
  blocked) PASS; TSan and ASan+UBSan+LSan PASS; three-run hold/wait/progress
  medians with diagnostics on and off show identical Raft progress and an OBS=1
  hold maximum of 34.5 ms to 8.2 ms. Full-gate result in the page.

- [x] **AC — Avoid unused RDMA totals in routine I/O logging.**
  `src/server/iostats.c::efs_iostats_dump` uses `efs_iostats_snapshot`, which
  calls `src/common/rdma.c::efs_rdma_totals`: a scan of 4,096 registry slots
  under `g_reg_lock`, also used by completion dispatch and connection lifetime
  management. The periodic I/O line does not print these RDMA totals. Separate
  the cheap I/O snapshot from requested RDMA aggregation; retain full totals
  for the management reply that needs them. Gate: routine dumps take no registry
  lock/scan, management totals still include live and retired connections,
  concurrent connect/close/poll is safe, TCP/no-RDMA builds work. Measure poller
  interference and status cost across connection counts.
  **Qualified within executed scope; merged into devel (wave 33) from `agent/devel-task-ac`**, tested
  `5f9ddae71`, base `1d2852da4`: intel1 full gate546/0FAIL/12NOTRUN
  (host limits, INCOMPLETE); node9901 native RDMA/management PASS. Three-repeat
  TCP/RDMA measurements prove scans36→0; no consistent speedup, TCP throughput
  median0.36% lower. Shared occupancy disclosed; raw evidence retained and
  scratch pruned ([AC evidence](2026-10/ac-iostats-rdma-scan-20261028.md)).
- [x] **AD — Bound contention from always-on counters: measured, no change.**
  `src/server/iostats.c::efs_iostats_add` updates shared per-class atomic
  counters, latency sums/maxima and a last-64 sample ring on GET/PUT/disk-write
  paths; detailed writer statistics are enabled by benchmarks/tests, not the
  daemon. There is
  no counter mutex here: investigate cache-line contention, not an invented
  global-lock defect. Measure per-op cost and scaling before choosing bounded,
  cache-line-separated worker shards or another collection scheme. Gate:
  concurrent update/snapshot, worker retirement, bounded memory and preserved
  counter meaning; state sampling/aggregation semantics explicitly, especially
  for percentiles. No unbounded thread registry or silent loss of error counts.
  **Oct 28 outcome:** [node9900 measurements](2026-10/ad-counter-contention-20261028.md)
  at `1d2852da4`: three baseline/no-op repeats at 8/16/32 writers on TCP and
  RDMA. At 32 writers every daemon is below 0.036% sampled counter-body CPU,
  below the explicit 1% stop criterion; leave the bounded shared counters and
  current p50/error semantics unchanged. 42 original D4 cases and a clean
  paired capture repair pass; one original IBS capture remains partial. Full
  `test-all`: 545 commands, 0 FAIL / 4 NOTRUN (host limits/self-test marker),
  690 s plus 15 s build, six individual overbudgets. No product change or toggle.
- [x] **AE — Keep routine logging from blocking the Raft pump.**
  Even after `h->mu` is released, `host_pump` calls the always-on I/O/KV dumps
  and optional batch diagnostics synchronously. `src/common/log_ts.c::ts_write`
  writes to the destination under the stdio stream lock; batch diagnostics also
  hold `flockfile(stderr)` while formatting a large record. Evaluate a bounded
  telemetry reporter with periodic-record coalescing and explicit overflow
  accounting. Moving a write outside `h->mu` alone does not free the pump.
  Gate: slow/blocked/failed log destination, queue saturation and bounded stop;
  no unbounded queue, hidden failures or dependence of consensus progress on
  routine telemetry delivery. Define critical-event retention/error reporting
  separately; preserve timestamps and whole-record integrity. Coordinate AB
  so neither task introduces a second incompatible reporting queue.
  **Oct 30 disposition: stays OPEN ([AB/AE page](2026-10/ab-ae-raft-logging-20261030.md)).**
  Ported by wave 39: the whole `agent/devel-task-ae` product change (K72, K73);
  the audit finds nothing left to port (28 commits: all in devel as a squashed
  port, evidence-only, or dropped fixture races). Done in BI: the pump's last
  lock-held writers (K80). Open: a SIGTERM'd `efsd` does not exit within 60 s while
  its stderr is a blocked pipe, because non-pump threads sit in synchronous
  `fprintf(stderr)` and stop joins them (**K79**, reproduction
  `pump_logging_profile.py --stop-blocked`); critical-event retention under a full
  mailbox (events are counted, not kept) needs the same design decision. The
  full-gate `test_pump_logging` can still fail with foreground `EIO` under 8-job
  load (one nuc failure, three idle reruns PASS).
  **Oct 31 (Task BP, [K79 page](2026-10/k79-stop-blocked-stderr-20261031.md)): ticked.** K79 is FIXED: the routine
  stderr writers go through the reporter and the stop-blocked acceptance (SIGTERM with a full
  stderr pipe: rc 0 within the 55 s bound) now holds and is in the gate. Still true: routine
  lines are counted, not kept, when the 32-slot mailbox overflows.

- [x] **AF — Decouple management status from consensus execution.**
  `src/server/raft_host.c::server_raft_host_status` takes `h->mu` while collecting
  both groups and their peer progress, then obtains KV statistics and reads salt
  and root metadata. Measure contention under realistic and aggressive polling.
  Evaluate a bounded published diagnostic snapshot, with age/version and clearly
  defined coherence, so pollers do not repeatedly serialize on the pump mutex.
  Do not use stale diagnostic data as authoritative admission, membership,
  recovery or durability permission. Gate: term/leader changes, retired groups,
  concurrent recovery and frequent polling; consumers must handle stale/unavailable
  snapshots explicitly. Verify current management fields and operational scripts
  before changing their freshness contract.
  **Measured, no product change on `agent/devel-task-af`**: tested
  `f570fb0cd`, base `486aa3a46`;30 closed xorinox/fstor007 TCP/RDMA trials,
  no10Hz pump/write penalty outside spread, hardware CPU/stat cost retained.
  Full gate545/1FAIL/11NOTRUN; sole K35 fsync EIO reproduces on untouched
  base, unresolved. Both failed fixtures preserved; raw evidence hashed,
  successful scratch pruned ([AF evidence](2026-10/af-status-snapshot-20261028.md)).

**Shared completion rule:** retain original failures, exact baseline/candidate
builds, commands, host/topology/profile and raw evidence outside Git. Use bounded
local regressions first, then separately retained loaded checks on owned fixtures;
never interfere with another session's cluster. Report diagnostic cost separately
from formal workload timing. Demonstrate the specific interference reduction and
no correctness, visibility, shutdown, memory-bound or error-reporting regression;
run affected tests and applicable sanitizer/concurrency gates. No new user-facing
A/B flags, reduced durability or relaxed deadlines. A source-level opportunity
alone is not a measured bottleneck; an unrun gate remains NOTRUN. Suggested order:
AC first, AB/AE together, then AD/AF according to measurement.

### Stage 1 additional letter tasks — hot-path serialization (AG–AJ)

**User-directed extension, October 28; source reviewed at `08a1606df`.**
All four are **OPEN / implementation and measurements NOTRUN**. The canonical
code graph supplied callers; the named source bodies and lock scopes were read
to verify each mechanism. These are confirmed serialization points, not measured
bottlenecks or demonstrated correctness bugs. No K-register count changes.
The Stage 1 exception above applies; P3-A–P3-F and AB–AF retain their own scope.

- [x] **AG — Client buffer allocation and budget critical section.**
  Graph path: `chunk_put_worker` / `prefetch_ahead` → allocation wrappers →
  `src/client/bufpool.c::buf_alloc`. Reservations, allocations, frees and metadata
  charges share `g_bp_mu` across the mount. A cold-slab allocation holds it while
  calling `malloc`, publishing a region through `efs_rdma_zc_region_add` (which
  takes `g_zc_lock`) and filling the free list; heap fallback/free also runs under
  this lock. Region publication here is not hardware MR registration, which is
  lazy elsewhere. Measure cold/warm lock hold and wait at mixed read/write loads.
  Evaluate reserving bounded capacity before allocating outside the lock and/or
  bounded local freelists. Preserve atomic credit ownership, aggregate hard
  bounds, metadata bounds, demand-read/drain headroom and thread-exit refunds;
  do not replace a real resource bound with approximate per-thread accounting.
  Gates: `test_bufpool`, client allocator/large-allocation and sparse-metadata
  coverage plus deterministic simultaneous slab growth, allocation failure,
  thread exit and saturated writers with demand reads. Retain exact accounting
  and ownership under ASan/TSan; measure end-to-end gain as well as allocator cost.
  **Measured 2026-10-28, no change** (`docs/status/2026-10/ag-bufpool-lock-20261028.md`):
  `g_bp_mu` wait share ≤ 0.0004 % at 32 streams (threshold 5 %), cold slab growth
  25–54 µs a handful of times per client; the lock stays as is.
- [x] **AH — Mount-wide chunk-map read/write contention.** MEASURED, no change: `g_client.idx_mu` sees 2.4-5.6k acquisitions/s, flat with writer count (the fsync-patterned workload is the limiter); trylock misses reach 25-30 % at 32 writers but the median contended wait is ≈0.35 us and the aggregate wait stays ≤0.15 % of wall — far below the stated action threshold (~1 % of wall or contention visibly capping throughput). Nothing was partitioned. [evidence](2026-10/ah-chunk-map-lock-20261028.md)
  `src/client/read.c::chunk_get_worker` and `rdcache_map_gen` take
  `g_client.idx_mu` to read chunk mappings; `src/client/write.c` takes the same
  mutex around chunk/base/delta updates (for example `dcache_put_now_budgeted`).
  Independent files therefore meet at one map lock despite striped data caches.
  The source explicitly requires the lock because map arrays can realloc; simply
  removing it or returning unpinned pointers is unsafe. Measure lock frequency,
  hold/wait and map-growth effects for many independent files. Evaluate stable
  map ownership with partitioned locking or a safe read-mostly scheme, including
  all resize/invalidation paths. Gates: concurrent map growth, truncate, span
  publication/folding, cache invalidation and same-/cross-client exact reads;
  preserve inode generation/epoch authority, lock order and bounded metadata.
  Existing `test_sparse_metadata.py` is a starting gate, not proof that readers
  may race mutations. This task is distinct from network startup and ReadIndex
  overlap in P3-B/P3-E.
- [x] **AI — RDMA completion dispatch lifetime locking (measured no-change, scoped).**
  [Measurement evidence](2026-10/ai-rdma-registry-lock-20261028.md): measurements
  COMPLETE scoped on user-authorized fcstor015; 27 matrix cells and concurrent
  teardown/churn passed, acquisition fractions below the 5% trigger. Keep the
  current lifetime lock for this single-device scope; no product change.
  RDMA reclamation/POSIX passed. Corrected full gate **546/1 FAIL/4 NOTRUN**;
  sole failure K55 independently reproduces on the untouched base. Its
  [subsequent branch fix](2026-10/k55-slow-commit-20261028.md) is independently qualified
  and merged into devel (wave 32). Task AI closure uses the base-failure allowance, not
  a green release gate. All failed evidence retained/verified; passed bulk pruned.
  Kernel/NIC wake latency and multiple-device contention are not claimed.
  Graph path: `recv_poller` → `harvest_recv_wcs` → `pend_push` in
  `src/common/rdma.c`. Every received CQE locks process-wide `g_reg_lock`,
  validates the slot generation, and publishes the completion (including an
  eventfd wake when needed) before unlocking. This covers normal completions,
  unlike AC's unnecessary diagnostic registry scan. Measure lock cost at high
  message rates, multiple devices and connection churn before choosing per-device
  registries, shorter pinned-lifetime sections or bounded batch handling.
  Preserve stale-QP generation rejection, connection teardown safety, single
  producer ring ownership and lost-wakeup prevention. Do not remove the separate
  `recv_handoff_mu` polling/repost synchronization introduced for correctness.
  Gates: existing RDMA handoff/progress/fork/segmentation tests, delayed stale
  CQEs during destroy/reuse, ring pressure and wakeup races, then real RDMA
  loaded qualification; TCP-only mocks cannot qualify this optimization.
- [x] **AJ — Server export reference acquisition on data I/O.**
  Graph/source path: GET in `src/server/handler.c::server_handle_conn` and PUT
  in `src/server/put_upload.c::server_put_upload` acquire an export under
  `s->lock`; `src/server/store.c::server_export_put` takes that same server-wide
  lock to release its in-flight reference. Export lookup scans the bounded
  export table, and first use can initialize an export under the lock. Disk I/O
  itself is outside this critical section: do not describe it as serialized
  disk access. Measure acquire/release contention at high small-I/O concurrency.
  Evaluate stable export references or per-export lifetime synchronization with
  a separately protected registry. Preserve destroy admission, in-flight drain,
  geometry initialization and reference balance on every error path. Gates:
  concurrent first use, GET/PUT against destroy/stop, failed I/O and acquisition
  failures, plus export-handle and ticketed-upload tests and memory/concurrency
  checks. No cached reference may outlive its export or bypass ticket authority.
  **Measured 2026-10-28, no change** (`docs/status/2026-10/aj-export-reference-lock-20261028.md`):
  `s->lock` wait share ≤ 0.021 % at 64 connections / ~59k GET/s (threshold 5 %),
  flat mean wait ~100 ns at 8/32/64 connections, no 1-vs-8-export effect; the
  lock stays as is.

**Deduplication and acceptance:** connection checkout already uses per-node
`conn_lock[idx]`; the read cache already has striped locks. Neither is a new
"global lock" finding here. AG concerns resource ownership, not AD's diagnostic
counters; AH concerns the local map, not metadata consensus placement; AI adds
normal completion dispatch beyond AC's status scan; AJ concerns export lifetime,
not AF's management status snapshot. Apply AB–AF's retained-evidence and bounded
qualification rules. Establish a baseline before selecting a redesign; report
no-benefit results rather than replacing a necessary lock merely because it is
shared. Any demonstrated correctness defect gets its own K record. No new runtime
A/B selectors, weakened durability, expanded format scope or foreign-cluster use.

### Stage 1 additional letter tasks — copies, allocation and repeated work (AK–AM)

**User-directed extension, October 28; source reviewed at `327d44a18`.**
All three are **OPEN / implementation and measurements NOTRUN**. Graph queries
for `packed_fetch_run`, `efs_packed_read_base_scratch` and
`efs_client_degraded_flags` identified paths; the source bodies below establish
the mechanisms. This is a static review, not a runtime profile or a new K defect.
No binary, cluster, workload or acceptance verdict changed. The Stage 1 exception
applies; no new behavior flags are proposed.

- [x] **AK — Packed reads: remove avoidable fragment staging and repeated hashing.**
  **DONE on branch `agent/devel-taskak` (`1c2fd4188`), not merged** (Oct 29,
  [evidence](2026-10/ak-packed-read-staging-20261028.md)): one fragment hash verified
  against both authorities (reply digest, then mapping digest, the failing
  authority named); TCP receives into caller scratch, RDMA transfers
  message-buffer ownership; segment/record validation unchanged. Gates:
  packed-reader/packed-segment/wire-shape units PASS on intel2 and node9901;
  the eight RDMA-device gate commands PASS on node9901; `make test-all` on
  intel2 is 547/1/12 — the one FAIL (`test_b6_unavailable.py` slow mode)
  reproduces identically on devel `9c4bfd171` (write path, pre-existing).
  Cold packed-read client CPU −26…−34 % on TCP; blake3 share 41.4→33.6 %
  (TCP), 27.9→22.2 % (RDMA, wait-dominated host).
  `src/client/read.c::packed_fetch_run` receives an owned whole reply through
  `efs_conn_recv_msg`, hashes its fragment against the reply digest, copies the
  entire fragment into the supplied scratch and frees the reply. Then
  `src/common/packed_reader.c::efs_packed_read_base_scratch` hashes those bytes
  again against the mapping's expected fragment digest. These are different
  authorities: retain both comparisons and the segment/record validation.
  Evaluate one payload hash checked against both expected digests, with explicit
  immutable-buffer ownership, and a bounded receive into caller storage or
  ownership transfer. TCP's generic receive allocates; RDMA takes ownership of
  its message buffer, so do not claim an extra malloc in both transports.
  The existing `efs_conn_recv_msg_into` assumes a one-byte status layout;
  GET_SEGMENT has a different reply header and cannot blindly use that helper.
  Gates: packed-reader and packed-segment tests, corrupt payload, reply digest
  and mapping digest independently, short/oversized/error replies, all two-of-three
  fragment combinations, deadline expiry and connection reuse. Compare TCP/RDMA
  allocated bytes, copied/hashed bytes, CPU and cold packed-read latency with
  identical integrity checks. This is separate from P3-B's request overlap and
  does not add an unbounded segment cache or remove client integrity verification.
- [x] **AL — Reads: allocate decode scratch only for the selected path.**
  `src/client/read.c::efs_client_decode_placed_chunk_attempts` calls
  `decode_frag_scratch(3 * frag_len)` before deciding that the mapping is packed.
  The packed branch instead allocates its own `5 * geometry / 2` scratch and
  returns without using the ordinary fragment storage. For ordinary reads where
  `2 * frag_len == chunk_size`, both data pointers are replaced by the output
  halves, leaving only the parity fragment needing separate storage. The current
  scratch is TLS-reused and freed at thread exit, not malloced afresh per read;
  the opportunity is unnecessary first allocation/growth and retained capacity.
  Move path selection before ordinary scratch acquisition and evaluate one-fragment
  scratch for the in-place case, preserving the fallback for other geometry.
  Gates: packed-only workers, mixed geometries, zero/hole reads, each missing
  fragment, failed reads and worker exit under ASan/LSan; prove aliasing and
  reconstruction safety in `src/data/erasure.c::decode`. Measure cold allocation
  counts and retained bytes across worker counts as well as read latency. This
  changes the caller's demand, not AG's allocator locking or budget mechanism;
  preserve demand-read headroom and existing resource bounds.
  **FIXED 2026-10-29** (`cbeb09621`, [evidence](2026-10/al-decode-scratch-20261028.md)):
  path selection before scratch acquisition, one-fragment scratch in place;
  retained decode scratch per mount 17.3 → 8.0 MiB mixed, packed-only now
  holds none; latency inside the spread, in-place aliasing pinned by
  `test_erasure` for every missing-fragment position; TSan PASS, gate FAIL 1
  is the K55 base failure.
- [x] **AM — REPORT: batch degraded-object bookkeeping without changing publication.**
  **MEASURED, NO CHANGE (record and stop) on branch `agent/devel-taskam`
  (`3fe865d98`, measurement assets only), merged into devel (wave 32)** (Oct 29,
  [evidence](2026-10/am-report-degraded-batch-20261028.md)): the empty-table fast
  path takes zero `g_dgobj_mu` acquisitions at 1/8/32 writers; with one
  pending object, an 8,192-entry table or a member down, the flags/drop pair
  costs ~200–340 ns per record with no contention growth at 32 writers —
  0.0025 % of REPORT wall on intel1, ≤ 0.023 % on node9901, two orders below
  the 5 % threshold. No batch is built; named gates (k35, REPORT
  partial-verdict, resolved-retry, session tests) PASS; test-all on intel1
  546/1/12 with the one FAIL the pre-existing devel b6 slow-mode defect.
  `src/client/inode_rpc.c::efs_client_rpc_report_dirty_pub` calls
  `efs_client_degraded_flags` separately while serializing every chunk and, after
  successful publication while degraded state remains pending, calls
  `efs_client_degraded_drop` for each chunk. Both functions in
  `src/client/write.c` probe the same object table under `g_dgobj_mu` when its
  live count is nonzero. A single pending degraded object can therefore cause
  locks/probes for many unrelated records; the empty-table fast path already
  avoids these locks and must be included in the baseline. Evaluate bounded
  batch lookup/retirement to amortize lock acquisition, not an unbounded critical
  section over a whole REPORT. Capture flags consistently with the publication
  being serialized; retire only the exact acknowledged object generation, never
  a replacement or unresolved/failed publication. Preserve missing-fragment
  marks, receipts, retry identity, deadlines and concurrent insert/repair behavior.
  Gates: K35 degraded fsync, REPORT partial verdict/resolved retry/session tests,
  plus forced note/drop/publication interleavings and sparse/dense degraded tables.
  Measure probes, lock acquisitions/hold/wait and REPORT p95 versus batch size.
  This is distinct from P3-C's ticket-array compaction and AH's chunk-map lock;
  generic REPORT buffer reuse is not claimed necessary by this finding.

**Acceptance and deduplication:** preserve the existing zero-fragment hash reuse,
in-place ordinary decode, worker pools and striped caches. AB–AJ and P3-A–P3-F
remain unchanged. Apply their retained baseline/comparison evidence rules: exact
source/build, topology, transport, I/O mode, fixture and concurrency; raw captures
outside Git; unchanged correctness/deadline gates; memory and p95/p99 results.
No gain is claimed until measured, and no existing failure evidence is replaced.

### Stage 1 additional letter tasks — RPC and proof batching (AN–AO)

**User-directed extension, October 28; source reviewed at `d62a753d9`.**
Both tasks are **OPEN / implementation and measurements NOTRUN**. Canonical graph
queries for `server_put_upload` and `efs_fuse_readdir_ino`, followed by source
inspection of the downstream handlers, establish the sequences below. They are
optimization candidates, not measured bottlenecks or permission to delete
correctness checks. No K count, live gate or failure verdict changes.

- [x] **AN — PUT storage authority: measure status requests and duplicate proof work.**
  **COMPLETE / measured, no STATUS optimization:** [AN evidence](2026-10/an-put-status-batching-20261028.md),
  `agent/devel-task-an` from `9c4bfd171`; user-approved isolated fcstor014.
  Exact f7 product: full 548/0 FAIL/0 NOTRUN, strict hardware audit 9/9,
  K35/K33 TCP/RDMA and W166 TCP 5/5; extra W130/K48 5/5 each. Precise 36-cell
  RPC/timing matrix plus 36 observer controls reviewed, medians/ranges on AN
  and four-counter table on K32. No demonstrated STATUS-specific benefit;
  authority checks unchanged. Observer CPU/GETATTR effects and single-host,
  fixed-rate limits explicit. Original FAILs/fixtures retained; no release PASS.
  Every fragment in `src/server/put_upload.c::server_put_upload` submits an exact
  `PUT_TICKET_STATUS` before storage admission. In
  `src/server/raft_host.c::server_raft_host_submit`, a nonleader delegates that
  entire status request to the lane leader (W166); the leader calls
  `host_session_establish_admit` for authoritative session state, then
  `host_read_index` for the ticket's group before inspecting the exact ticket
  and participant session floor. The session helper itself obtains an owner
  proof, locally or via a remote command. When owner and lane group coincide,
  this sequence calls ReadIndex twice for one read-only authority request.
  Three fragment uploads also independently request the same ticket status.
  Count storage-to-leader status RPCs, session-owner RPCs, proof admissions and
  actual quorum rounds separately: `host_read_index_admitted` already shares
  readers needing the same round, so three requests do not imply three separate
  quorum exchanges. K32 already batches ticket ADMIT/SETTLE; do not duplicate it.
  First evaluate one fresh, applied proof covering the same-group status reads
  with explicit scope and term/context validity. Then evaluate bounded status
  batches per destination/group or safe concurrent request sharing, with a
  verdict per exact ticket. Different groups still require their own authority.
  No reusable time-based ADMITTED cache, client assertion or stale follower KV
  may authorize a PUT. Keep W166 routing, session fencing, storage guards,
  immutable ticket identity, generation checks, deadlines and stop refusal.
  Gates: ticket upload/batch/session-admission coverage; deterministic revoke,
  SETTLE, leader change and halted/lagging follower interleavings between proof,
  lookup and storage admission; mixed valid/stale batch entries and lost replies.
  Run normal and degraded multi-client writes on TCP and RDMA, measuring RPCs
  per fragment, quorum rounds, batching wait and write p95/p99. Bound queue size
  and residence time; no low-load batching delay may break existing deadlines.
  **Wave 38 coordinator note:** only the evidence and measurement scripts were merged. AN's branch also carries product repairs (K35 and K55-style COMMIT overlap in `data_commit.c`, directory-migration visibility in `raft_host.c`, GETCHUNKS record clearing, a store.c ENOENT parent retry) that overlap work already on devel; they are being checked against current devel in a separate port, not merged here.
- [x] **AO — Directory listing: amortize serial page RPCs and repeated proofs.** *(Wave 40: bounded compact pages and migration repairs ported; AO COMPLETE scoped, B6 and live-error attribution still open; [port](2026-10/ao-product-port-20261030.md).)*
  `src/client/efs_fuse.c::efs_fuse_readdir_ino` first refreshes the parent via
  `efs_client_stat_refresh` and checks current permissions, then synchronously
  loops over `efs_client_rpc_readdir_cur`. Each reply has at most
  `EFS_READDIR_MAX == 64` entries (`include/efs/protocol.h`): 4096 returned names
  therefore require at least 64 page RPCs, in addition to the initial refresh.
  `src/server/raft_host.c::server_raft_host_readdir` reads the parent with
  ReadIndex on each call and obtains the needed lane proofs; each page starts a
  new call's coverage. The server already reuses proof coverage within a call,
  and the FUSE directory handle already retains its collected listing, so do not
  propose either as a missing optimization. Evaluate bounded larger/variable
  pages or a bounded multi-page response with explicit byte, scan and time caps.
  A cursor depends on the preceding reply; blindly sending later page RPCs in
  parallel is not valid. Do not simply keep proof bits across calls or remove
  the initial permission refresh. Combining parent authority with enumeration
  would require an explicit authorization protocol that rejects access before
  exposing names, including peer chmod and caller group semantics; that is not
  justified by a cached parent row. Preserve exact source/name cookies, EOF,
  source-only cursor transitions and K48 layout/bitmap revalidation with fresh
  authority for newly introduced groups. Gates: readdir permission-authority,
  reply-boundary, source-cursor, collection and K48 tests; long names, tiny user
  buffers, concurrent rename/split/unlink, denied access, leader loss and halted
  replica. Measure first-result and full-list latency, page RPCs, proof rounds,
  peak client/server memory and fairness against concurrent metadata mutations.
  Larger pages must not trade bounded progress for fewer network messages.

**Deduplication and acceptance:** AN concerns storage-side ticket authority,
not P3-A's PUT startup overlap, P3-C's local settlement compaction or AM's REPORT
bookkeeping. AO concerns directory page transport, not P3-E's GETCHUNKS proofs
or P3-F's CREATE proofs. Existing ReadIndex round sharing, ticket cohorts,
REPORT transaction batching and deep-path LOOKUP_PATH support remain intact.
The path walk's restart-from-root fallback was examined but is not added here:
current direct callers are the benchmark path, and a separate measured case is
needed before treating it as a general FUSE hot-path priority. Apply AB–AM's
unchanged baseline, raw-evidence retention and correctness requirements. No
new configuration toggle, weakened integrity/authority, or speed claim is part
of these tasks; implementation and live comparison remain NOTRUN.

### Stage 1 additional letter tasks — disk I/O and write amplification (AP–AR)

**User-directed extension, October 28; source reviewed at `b42015536`.**
All three are **OPEN / implementation and measurements NOTRUN**. Graph traversal
of `sg_read_tail`, `sg_write` and `server_write_fragment_to_path`, checked against
the source bodies, establishes the mechanisms. No new correctness defect or
measured storage bottleneck is claimed. No live cluster, failure evidence or K
verdict changes; the Stage 1 exception applies.

- [x] **AP — Packed segment reads: combine adjacent payload and tail I/O.**
  `src/server/segment_store.c::server_segment_read` first calls `sg_read_tail`
  and then `server_frag_pread` for the payload. The helper in
  `src/server/store.c` rounds the tail-record read to a 4 KiB O_DIRECT read,
  allocates an aligned bounce for each call, and copies out. Thus a successful
  segment GET issues separate tail and payload reads even though they are
  adjacent in the same file. Ordinary `read_file_bytes` already reads payload
  and tail together; do not report ordinary reads as missing that optimization.
  Evaluate one bounded aligned read of the payload plus tail, followed by the
  same full identity, record and payload-hash validation before serving bytes.
  This saves a read submission/bounce opportunity, not necessarily device bytes:
  the same payload and tail still need reading. Preserve short/torn-tail errors,
  exact generation/geometry checks, O_DIRECT and no buffered fallback. Measure
  syscalls, device requests, bytes, allocations and packed GET p95/p99 on actual
  storage, including cold and repeated reads. Gates: segment storage/recovery,
  packed reader, direct-I/O-required and K37 coverage; missing/truncated files,
  wrong identity, bad payload, read error and concurrent reclaim. Separate this
  server disk task from AK's client receive copy/double hash work.
  **Measured 2026-10-29, no change** (`docs/status/2026-10/ap-segment-read-combined-20261028.md`):
  a combined read saves one pread + one bounce per packed GET, but cold and
  repeated packed-read p95/p99 are identical within a 2–4× wider rep spread on
  intel2 NVMe at 1/8 readers; the threshold is not met and the code stays as is.
- [x] **AQ — Segment directory barriers: share only covered namespace fsyncs.**
  `src/server/segment_store.c::sg_write` writes and fsyncs each newly created
  fragment file, then `sg_sync_path` opens/fsyncs/closes its directory.
  `sg_mkdirs` additionally syncs parents for newly created directory levels.
  Ordinary fragments already use `directory_sync_group_run` through
  `fragment_directory_group_barrier`; segment directory syncs do not. Evaluate reuse of
  that bounded grouping mechanism for concurrent segment namespace barriers,
  retaining each caller's descriptor and matching device/inode identity.
  Only mutations completed before a batch's barrier starts can share its
  result; arrivals during I/O require a later barrier. Keep per-file data
  durability and all required ancestry ordering. No permanent 'directory was
  synced' cache, arbitrary delay or blanket fsync removal. Segment buckets may
  distribute jobs so sharing is rare: measure actual same-directory overlap,
  barrier counts and pack/rebuild latency before selecting this optimization.
  Gates: `test_directory_sync_group.c`, segment recovery/rebuild tests, injected
  file/directory fsync failures, new nested directories, descriptor reuse,
  concurrent creators and crash at each write/barrier boundary. Every successful
  reply must retain the same durability guarantee. This is distinct from P3-D's
  client COMMIT workers and the ordinary root barrier, which already groups work.
  **Measured 2026-10-29, no change** (`docs/status/2026-10/aq-segment-dir-barriers-20261028.md`):
  same-directory in-flight overlap 0.20–1.27 % at full packer rate (threshold
  10 %), 0 % under K37 rebuilds, median directory fsync 1 µs; the sync group
  is not adopted for segment directories.
- [x] **AR — Ordinary PUT retries: quantify and reduce identical-object rewrites.**
  **MEASURED, NO CHANGE (record and stop) on branch `agent/devel-taskar`
  (`9b5de147e`, measurement assets only), merged into devel (wave 32)** (Oct 29,
  [evidence](2026-10/ar-put-retry-rewrites-20261028.md)): zero EEXIST rewrites in the
  D4 small-file load, the append_failover baseline and its
  --lose-reserve-every gate (all three daemons, ar_over=0; reserve-reply loss
  writes new objects, never re-PUTs an existing one). Counter validated by
  forcing one lost PUT reply per fragment
  (EFS_FAULT_PUT_REPLY_COLLISION=1): exactly one 64 KiB rewrite per node.
  Below the 1 % threshold in both loads; no completion-proof or verified-reuse
  design is built. Named gates PASS (store overwrite, PUT ticket, PUT upload,
  PUT ticket RPC); test-all on intel1 546/1/12 with the one FAIL the
  pre-existing devel b6 slow-mode defect.
  `src/server/store.c::server_write_fragment_to_path` treats EEXIST on the
  exclusive-create attempt as an overwrite and calls `shard_io_thread` again
  with `no_create`. That path writes the full padded payload and I25 tail; it
  does not compare the existing object first. A retry after a lost reply can
  therefore submit the same object's bytes again. Existing same-size overwrite
  handling already preserves extents and avoids needless ftruncate: this task
  concerns bytes rewritten, not that fixed inode-size issue. Establish retry
  frequency and logical versus submitted/device bytes under delayed/lost replies.
  Evaluate a bounded, lifetime-safe completion proof for identical immutable
  objects, or verified existing-object reuse only where it costs less than the
  write. Reading the entire payload just to skip a write may be worse; compare
  both costs. A filename, matching tail digest or prior volatile success alone
  is insufficient: preserve payload integrity, ticket/session authority, exact
  identity/length, quota accounting and the current root durability epoch/boot
  proof. Torn or failed writes must be repaired or refused, never acknowledged
  as already complete. Gates: store overwrite and ticket upload tests; identical
  concurrent retries, conflicting content, lost reply, quota exhaustion, damaged
  payload with intact tail, interrupted write, restart and failed root barrier.
  Measure normal traffic too so the common first PUT gains no extra read probe.
  This is separate from AN's authority RPC batching and P3-C's ticket compaction.

**Deduplication and acceptance:** KV WAL already shares fdatasync boundaries;
Raft persistence batching, ordinary directory grouping, range-limited LSM
compaction, single-write payload/tail layout and same-size extent preservation
are not new findings. Do not reopen W123 checkpoint work or replace its failed
measurements with these static observations. Apply AB–AO's exact build/environment,
retained raw evidence and unchanged correctness/deadline requirements. Distinguish
logical bytes, syscall bytes, filesystem/device write amplification and barrier
counts. O_DIRECT is not a durability guarantee. No format change, new knob,
removed barrier or performance PASS is authorized by merely listing a candidate.

### Stage 1 additional letter tasks — wakeups and polling (AS–AU)

**User-directed extension, October 28; source reviewed at `7dc73fb68`.**
All three are **OPEN / implementation and measurements NOTRUN**. Graph paths
through `demand_read_alloc`, `put_win_close` and `host_pump_kick` were verified
against their callers, wait predicates and source bodies. These establish work
that occurs, not measured scheduler overhead or a correctness defect. Existing
failures, K counts and acceptance verdicts are unchanged.

- [x] **AS — Demand-read capacity: replace repeated timed allocation attempts with bounded notification.**
  `src/client/read.c::demand_read_alloc` first tries allocation and trims the
  read cache, then retries allocation with 1 ms sleeps for the first 100 failed
  loop attempts and 10 ms thereafter. It stops at the inherited RPC deadline,
  or its existing approximately 30 s fallback without one. Capacity release in
  `src/client/bufpool.c::efs_buf_free` does not notify these sleepers. This only
  occurs under allocation pressure, not on every healthy read. Evaluate a
  capacity-generation/predicate wait with atomic check-and-enqueue against
  release, avoiding both missed refunds and a broadcast herd. Cover all capacity
  return paths, including thread-credit destruction, rather than only body free;
  allocation failure unrelated to budget must still terminate within the bound.
  Preserve aggregate accounting, read headroom, lock order, fairness to writers,
  shutdown and deadlines. Gates: bufpool and allocator tests plus a forced free
  between failed allocation and sleep, mixed allocation sizes, many waiting
  readers, exhausted writers, thread exit and no capacity return. Measure failed
  attempts, timed wakeups, context switches and release-to-read latency. Separate
  this wait policy from AG's allocator critical section and AL's scratch sizing.
  **Measured 2026-10-29, no change** (`docs/status/2026-10/as-demand-read-wait-20261028.md`):
  the wait loop never engaged — zero first-failures in ~5.1M demand allocations
  across 10 runs (32 readers over a 2 GiB working set with 0/8/32 saturating
  writers, plus a clamped 32 MiB/tier budget); there is no wake-up cost to
  remove, and the retry ladder stays as the pathological-case safety net.
- [x] **AT — PUT-window completion: wake affected predicates instead of every waiter.**
  **MEASURED, NO CHANGE (record and stop) on branch `agent/devel-taskat`
  (`dfd5fd191`, measurement assets only), merged into devel (wave 36)** (Oct 29,
  [evidence](2026-10/at-put-window-wakeups-20261028.md)): 3.5/17.9/84.6 wakes per
  satisfied waiter at 1/8/32 independent-file writers, zero deadline
  timeouts. Wait durations track PUT completion time (2–8 ms at 1 writer,
  256 ms–1 s at 32), not wake latency, and broadcast-wake CPU is ~0.05 % of
  the 16-core host: no measurable end-to-end gain from per-inode wake
  ownership, and the lost-wake invariant is untouched. Named gates PASS
  (put_window_deadline, k38 shared-chunk, k35 fsync-degraded); test-all on
  xorinox 546/1/11 with the one FAIL the pre-existing devel b6 slow defect.
  `src/client/write.c::put_win_close` broadcasts on the single `put_win_cv` after
  every close, even while that inode still has open windows. `put_win_wait`
  checks an inode-specific predicate under `put_win_mu`, but also wakes at most
  every 50 ms to recheck the deadline. Thus unrelated inode waiters can wake,
  acquire the same mutex and go back to sleep. The overflow state deliberately
  makes every inode busy and is an exception requiring broader notification.
  Evaluate notification on relevant busy-to-ready transitions, with bounded
  per-inode/sharded waiter ownership, and deadline waits that do not periodically
  wake merely to rediscover an unchanged predicate. Do not replace broadcast
  with an arbitrary single signal: it can select a waiter whose inode is still
  busy and strand the eligible one. Preserve overflow/global release, multiple
  waiters per inode, slot reuse, errors and the inherited operation deadline.
  Gates: `test_put_window_deadline.py`, deterministic simultaneous inode windows,
  partial closes, overflow entry/exit, completion-before-sleep and shutdown;
  concurrent flush/read/REPORT exact-data checks. Measure wakes per satisfied
  waiter, lock reacquisition, timeout accuracy and p95 under independent-file
  writes. This is not P3-D's COMMIT worker reuse or AM's degraded-object table.
- [x] **AU — Raft pump kicks: coalesce notifications without losing work.**
  `src/server/raft_host.c::host_pump_kick` performs a nonblocking eventfd write
  for every call from proposal/read/inbox/control paths; the pump later polls
  and drains the eventfd. The kernel counter already combines notifications,
  so one kick does not imply one context switch. The candidate is avoiding
  redundant write syscalls while work is already signaled or being drained,
  not adding an event-driven pump (it already exists). Evaluate a race-proven
  pending-work notification protocol or bounded producer batching only after
  measuring kick rate versus drains, useful pump cycles and scheduler wakeups.
  An arrival between drain, clearing notification state and sleep must either
  be processed or leave a readable event; no queue work may rely on a missed
  wake being rescued by the timer. Preserve retained-slice handling in
  `host_pump_poll_timeout`, lock-free kick behavior, shutdown, read requests,
  persistence completion, and wall-time Raft ticks/election cadence. No spinning
  replacement, arbitrary batching delay or change to durability ordering.
  Gates: host inbox/read admission coverage and deterministic arrival at each
  drain/clear/poll boundary, retained work after a bounded slice, eventfd EINTR/
  EAGAIN, stop and heavy proposals; then loaded leader/follower failover tests.
  This concerns the Raft pump eventfd, not AI's RDMA completion lifetime lock,
  AE's logging, or W123 checkpoint persistence.
  **Measured 2026-10-29, no change** (`docs/status/2026-10/au-pump-kicks-20261028.md`):
  the pump thread's own eventfd syscalls cost ≤ ~1.1 % of one CPU, the entire
  candidate saving is bounded by ~0.8 % of one CPU per daemon, and step-down
  runs show kicks/drain of 1.12–1.42; the stop rule holds, no pending flag.

**Deduplication and acceptance:** GET/PUT worker pools already signal batch
completion only when the last job finishes, queue-space signals are conditional
on a previously full queue, and ordinary fragment reads already avoid the old
per-fragment thread-pool hop. These are not missing optimizations. Keep required
RDMA wakeups, lifetime synchronization and Raft proof notifications; do not infer
that a condvar broadcast or timer is unnecessary merely from its presence.
Apply AB–AR's exact-source, retained-evidence and unchanged-gate requirements.
Measure idle/low-load and saturated cases, context switches and tail latency,
including starvation and cancellation. No new switches or performance PASS
claims; no implementation or live test was run for this review.

### Stage 1 additional letter tasks — workload-sized scans (AV–AX)

**User-directed extension, October 28; source reviewed at `ee66f837b`.**
All three are **OPEN / implementation and measurements NOTRUN**. Graph queries
for `server_raft_host_report`, `child_idx_del` and `sync_hardlink_attrs` were
checked against the function bodies and callers. Complexity below describes
those loops, not a measured end-to-end slowdown or an unbounded input: message
and staging limits still apply. No K defect, acceptance or failure verdict changes.

- [x] **AV — REPORT: index the chunk-to-inode-size join once per request.**
  In `src/server/raft_host.c::server_raft_host_report`, the main record loop
  searches `irecs[0..ino_count)` from the start for each of `count` chunk records.
  This is O(C × I) comparisons in the worst case for C chunks and I inode-size
  records; with one chunk per distinct inode it can grow quadratically within
  the bounded frame. This is not the separate `ic[32]` row cache, whose linear
  lookup has a fixed small bound. Evaluate a bounded request-local inode index
  or order-preserving indexed join built once, retaining original record order
  and immutable publication identity. Preserve first-match behavior for duplicate
  size records unless a separately reviewed validation rule changes it; zero
  EOF is explicit and absence is not permission to guess size. Preserve deleted
  inode handling, captured FileID/epoch, partial verdicts, deadlines and allocation
  failure before any new side effects. Gates: REPORT zero-EOF, frame-boundary,
  partial-verdict, retry/resolve and K38 bounds; add mixed/duplicate/missing inode
  records and adversarial order. Measure comparison count, CPU, transient memory
  and latency while varying C and I independently, including the common one-inode
  case. This is not AM's degraded-object bookkeeping, AN's status proofs or
  P3-C's ticket compaction. The client chunk-only-inode fallback has another
  size-array scan, but it already bypasses normal inodes through `dirty_set_has`;
  do not label all client REPORT construction quadratic from that rare branch.
  **Measured 2026-10-29, no change** (`docs/status/2026-10/av-report-join-20261028.md`):
  the join costs 1–32 comparisons per REPORT across big/small/mixed/openmany
  loads — C and I are never both large in one real frame and first-match makes
  the common case linear; even an adversarial C=I=512 frame is ~0.1 ms, far
  below the REPORT's own wall cost. No index is justified.
- [x] **AW — Local child vectors: avoid a parent-sized search on deletion and slot moves.**
  **IMPLEMENTED (`50218c827`), merged in wave 37** (Oct 29,
  [evidence](2026-10/aw-child-vector-index-20261028.md)): a reverse slot→position map
  per child vector, charged to the same staged memory budget; deletion and
  relocation repair are O(1) (~6 map operations instead of ~D comparisons at
  1 K/100 K/1 M children). Rollup semantics unchanged (convergence verified).
  End-to-end unlink latency is unchanged because the dominant cost is the
  pre-existing `recompute_times_up` rescan (a separate O(D) mechanism,
  recorded as a follow-up finding; the exact-skip fast path tried here was
  reverted after the correctness check caught a stale bound). Gates:
  deterministic child-vec test (realloc, cross-parent swap, aliases,
  grow/delete cycles, allocation failure, root), sparse-metadata,
  stage-evict, rename/unlink coverage, ASan/LSan PASS; test-all on intel1
  547/2/12 (b6 = pre-existing; K41 = timing-sensitive, baseline and
  isolation both PASS).
  `src/meta/metadata.c::child_idx_del` searches all slots of the parent's child
  vector before swap-removing the match. `child_idx_replace_slot` searches that
  vector again to fix a relocated inode row; `remove_inode_slot` invokes it
  when swapping the last row into a removed slot. Graph callers include local
  unlink/forget, rename and RPC-row upsert. For a loaded parent with D children,
  an operation can scan O(D); an adversarial sequence of D removals can do
  O(D²) comparisons, even though the actual vector deletion is O(1).
  This is the in-memory export/staging index, not the authoritative Raft KV
  directory scan or the split migrator. Evaluate a reverse slot-to-child-position
  index or equivalent bounded representation. Update it on both vector swaps
  and inode-row moves, parent rebinding, rebuild and eviction. Preserve hardlink
  row identity, per-name indexing, root handling, sharded tables, memory charging
  and failure atomicity; do not cache unstable array pointers across growth.
  Gates: sparse-metadata/stage-eviction and rename/unlink coverage, with forced
  realloc, swap of a row from another parent, multiple aliases, repeated grow/
  delete cycles and allocation failure. Measure comparisons against loaded
  child count, retained index bytes and local mutation latency. Distinct from
  AH's chunk-map locking and AO's server READDIR page transport.
- [x] **AX — Loaded hardlink aliases: visit aliases rather than unrelated inode rows.**
  `src/meta/metadata.c::sync_hardlink_attrs` scans `ex->inode_count` to propagate
  shared attributes for a non-directory with nlink > 1; size, mode, owner, time
  and RPC upsert callers use it. Single-link files and directories already return
  immediately. Link/unlink nlink updates also contain full-table scans, and
  `stamp_ctime_loaded` can visit shard tables for multiply linked inodes. Thus
  cost depends on unrelated loaded rows, not just the inode's alias count.
  Evaluate an inode-to-alias-row index, potentially sharing AW's slot-lifetime
  machinery but with a different key and invariant. Preserve each alias's parent,
  name/slab ownership and rollups while updating shared attributes, nlink and
  timestamps. Cover row relocation, unlink with open handles, last alias,
  remote adoption, cross-shard aliases, eviction/reload and allocation failure.
  Gates: metadata hardlink semantics, stage eviction, sparse metadata and POSIX
  link/rename/chmod/truncate cases; compare a fixed number of aliases while
  increasing unrelated loaded rows, then vary aliases independently. Report
  index memory and update cost as well as lookup speed. This task does not
  claim ordinary single-link writes scan the table, nor replace authoritative
  metadata reads with a local alias index.

**Deduplication and acceptance:** bounded 32-entry REPORT row-cache, 256-entry
mtime pins, fixed-size node/connection tables and hash-probe loops are not called
quadratic merely because they contain loops. Existing inode/name/chunk hash
indexes, hashed READDIR name deduplication and P3-C's repeated ticket memmove
remain separate. Keep AB–AU's exact build/environment, retained raw evidence,
unchanged correctness/deadline and memory-bound gates. Use operation/comparison
counts to establish scaling before proposing data-structure complexity; retain
small-workload baselines. No new behavior flags, implementation or live
performance acceptance is included in this documentation review.

### Stage 1 additional letter tasks — cross-path due diligence (AY–BB)

**User-directed correctness review, October 28; source inspected at `cb1a6a434`.**
These are the **four strongest additional examples** from the reviewed paths,
not an exhaustive whole-codebase audit or ten items padded with duplicates.
All are **OPEN / audit qualification NOTRUN**. Graph navigation and source reads
confirm separate mechanisms; they do not prove a defect. Unlike the optimization
items above, these are Stage 1 due-diligence gates on authority, lifetime and
failure semantics. No K defect or performance PASS is inferred from duplication.
  **Done (Oct 29, `agent/devel-ax`):** above the threshold (13–20 ms per chmod/truncate/link on a hard-linked file at 1 M loaded rows, 0.001 ms after); per-table alias vector replaces the scans ([evidence](2026-10/ax-hardlink-alias-index-20261028.md)). Gate xorinox 553 / 0 FAIL / 10 NOTRUN.

- [x] **AY — Ordinary versus packed upload: what does successful completion prove?**
  Ordinary `src/server/put_upload.c::server_put_upload` checks the exact ticket
  under fresh authority, enters a storage-session guard and writes through the
  export/store path. Ordinary data durability is tracked by root barriers and
  boot/epoch proofs (`src/server/store.c`, client `data_commit.c`). Packed
  `src/server/segment_store.c::server_segment_put` checks pack-object authority,
  uses a segment guard/reclaim fence and packing quota reserve; `sg_write`
  explicitly fsyncs the file and directory. Rebuild has another admission branch.
  These are legitimate potential differences, but both paths publish a successful
  PUT outcome into higher-level publication/protection logic.
  **Audit:** trace each caller's interpretation of success through metadata
  publication, fsync, restart and rebuild. Tabulate authority, in-flight owner,
  acknowledged versus durable state, quota charge/refund, exact duplicate handling
  and error cleanup. In particular, ordinary retries can overwrite while the
  normal segment EEXIST path checks the tail/digest without the rebuild branch's
  full payload check: establish the preconditions that make each retry safe,
  rather than assuming EEXIST means a proven object. Test partial write, intact
  tail/corrupt body, lost reply, quota failure, stop and reclaim races with
  equivalent semantic outcomes. Preserve ticket versus pack-ledger distinctions.
  **Done:** reviewed completion contracts and cross-path fault results, with
  defects fixed/retested or explicitly tracked as blockers. AN/AQ/AR still own
  RPC/barrier/rewrite optimization; this task owns correctness of their contracts.
- [x] **AZ — Verification policy across GET, audit and recovery.**
  Ordinary serving uses `store.c::read_fragment_verified`; packed serving uses
  `segment_store.c::server_segment_read`; packed audit uses
  `server_segment_check`; recovery uses the verification paths in `store.c`
  orchestrated by `recovery.c`. They share `efs_frag_record_verify`, but separately
  handle file size, payload reads/hash comparison, expected metadata digest,
  errors and destructive recovery action. Packed audit distinguishes MISSING,
  BAD and ERROR; a GET returning an I/O error is not proof of absence.
  **Audit:** build a table of identity/length/digest checks and result categories
  for each entry point, including which authority supplies the expected identity.
  Run one semantic fixture matrix through the real wrappers: valid object, wrong
  kind/generation/holder geometry, short/extra tail, bad body with valid record,
  wrong metadata digest, ENOENT and injected read/permission errors. A fixture
  need not have identical bytes across different object formats. No unproven
  object may be served, and transient I/O failure must not silently become
  corruption, absence or permission to delete. Verify recovery's durable degraded
  mark before clearing its fence. Different actions may be correct by context;
  record their justification. Existing record unit tests alone do not cover this.
  **Done:** matrix outcomes and retained regressions for gaps; identify reusable
  policy primitives only after proving the guarantees match. AK/AP retain their
  hashing/copy/I/O scope; W123/K36 failures and qualification remain independent.
  **Audited 2026-10-29** (`docs/status/2026-10/az-verification-policy-audit-20261029.md`):
  six wrappers tabulated, one fixture matrix (12 cases) through all of them
  (`tests/test_verification_matrix.c`). One demonstrated gap, **K58**: verified
  recovery deleted a fragment on an I/O error; fixed with a retained regression.
  Other differences (extra tail, where the expected digest is compared, tickets
  marked after the fence) are justified in the page; no shared primitive proposed
  beyond the recovery verdict predicate.
- [x] **BA — Ordinary orphan, superseded-object and segment deletion authority.**
  `src/server/put_orphan.c::server_put_orphan_delete` requires a durable session
  floor and authoritative orphan status before exact-object deletion and directory
  cleanup. `store.c::server_gc_superseded_delete` and the reclaim journal handle
  other ordinary objects. Packed deletion enters through ledger/reclaim authority
  and `segment_store.c::server_segment_delete`, which unlinks generation-named
  fragment paths; its low-level helper relies on its caller's fence. Quota credit,
  physical unlink, namespace durability and journal completion are separate steps.
  **Audit:** enumerate every reachable caller, including replay/recovery/rebuild,
  and prove the fence, exact identity and non-resurrection guarantee for each.
  Trace credit-before-delete, retry and crash behavior; neither absence nor a
  cached retirement decision alone authorizes all object kinds. Exercise stale
  generation, concurrent PUT/rebuild, already missing files, unlink/fsync failure
  and crash between credit, journal, unlink and acknowledgement. Verify no live
  published object is removed and no retry double-credits space. Do not equate
  a guard mutex with durable authority or add per-file deletion to live segments.
  **Done:** a deletion-authority/caller inventory and passing current-format
  replay/interleaving gates using reclaim, orphan and pack-ledger tests. This is
  a correctness audit, not AQ's barrier sharing or AR's duplicate-write avoidance.
  **Audited 2026-10-29** (`docs/status/2026-10/ba-deletion-authority-audit-20261029.md`):
  eleven delete entry points tabulated with fence, identity and non-resurrection.
  **K59** (delayed segment PUT resurrects a retired segment) and **K60** (two
  deleters debit one file) fixed with retained reproductions; **K61** (delayed
  ordinary PUT rewrites a superseded fragment) open with its reproduction, fixed by Task BH ([evidence](2026-10/k61-delayed-put-stray-20261030.md)). The
  three-member crash matrix was not run; the reclaim harness covers the orderings.
- [x] **BB — Client metadata overlays versus authoritative metadata (scoped audit).**
  [Audit and retained controls](2026-10/bb-overlay-authority-audit-20261029.md): COMPLETE
  scoped on `agent/devel-bb`, source `02f861e4a`; base matrix 5/8 FAIL;
  xorinox full **553/0 FAIL/10 NOTRUN**, core 218/218 and peer 72/72 PASS.
  ORCD TCP/RDMA matrices 8/8 each and RDMA core/peer/transport PASS.
  K62–K64 (renumbered; BA took K59–K61) still require integration; external cluster-script gate NOTRUN.
  Earlier failures/stores retained, raw captures sealed and independently verified.
  Local `src/meta/metadata.c` inode/name/child/alias indexes and mutators coexist
  with Raft-backed rows in `src/meta/meta_apply.c`. They are not two equivalent
  authoritative replicas. `src/client/ops.c::efs_client_adopt_lookup` adopts an
  RPC row then overlays local state; `efs_client_stat_open` obtains RPC metadata
  but retains local accepted state with explicit nlink/error handling. FUSE
  permission-sensitive listing instead refreshes authority before enumeration.
  **Audit:** document field-by-field authority for size, epoch/generation, nlink,
  mode/uid/gid, timestamps, names and packing state across lookup, open getattr,
  refresh, REPORT, truncate and unlink-open. Check implementation rather than
  comments: the stat-open comment says local-first/RPC-on-miss, while the reviewed
  body calls RPC even when it has a local row. Correct misleading comments as
  part of the audit, not by changing behavior to match them. Exercise remote
  chmod/chown/rename/link/unlink alongside dirty local writes, stale/reordered
  replies, truncation, eviction/reload and RPC failure. Preserve read-your-writes
  without promoting a stale cache into permission or namespace authority.
  **Done:** one reviewed overlay contract and tests for every differing path;
  repair demonstrated gaps. AW/AX still own index performance; this audit does
  not replace those tasks or require a new local authority/cache protocol.

**Priority and closure:** start with BA and AY (irreversible deletion and
successful-write meaning), then AZ and BB. Each audit records source/build,
actual entry points, owner/authority, ordering, error outcomes, tests and evidence.
Close only with a reviewed justified-difference result or a fixed and qualified
finding; an unknown cell remains OPEN/NOTRUN. Put confirmed bugs into the K
register with preserved failures. Cross-link existing tests and owning tasks
rather than rerunning or reimplementing them without a gap. No blanket rewrite,
backwards compatibility, new flags, timeout relaxation or evidence deletion.

These Stage 1 audits do not bring all alpha 1.1 restructuring into alpha 1.0.
Small necessary correctness repairs belong here; broad ownership/interface
consolidation remains [alpha 1.1](alpha-1.1-consolidation.md). A demonstrated
in-scope defect cannot be deferred merely by calling it architectural debt.

## A. Stage 2: freeze a reproducible candidate and support matrix

**Review Nov 9 (devel `7526fd4c7`; corrections dated Nov 9 below).**
Stage 2 has not started: K38 is OPEN and recurred on candidate `9bb50fe79`
in the orcd-mini batch, so the candidate will change (A1). The matrix
`scripts/qualify_matrix.json` cannot run the cells D46 requires: seven of
its ten topologies name the withdrawn lab hosts (amd, intel1, intel2,
xorinox, nuc; the three `bare-metal-*` ones are BLOCKED), `fcstor-4x1-tcp`
uses fcstor012–015 where fcstor015 refuses the test account's key, and
`orcd-mini-rdma` still describes three daemons where the cluster runs four.
It has no 3-host or 5-host fcstor topology, no distinct-host RDMA topology
and no 13-node topology. **Open for the user:** D46 names fcstor007–011 with
storage under `/data1/01`; those hosts carry another campaign since Nov 9
and agents treat `/data1` as read-only, and D46 item 2 leaves the 13-node
storage/client split and transports open. Until that is ruled, A3, A4, A5,
A11 and every per-topology item below have no topology to run on. Every
PASS recorded in sections A13, B, C, D and E is from one host or loopback
on October builds and withdrawn hosts; none can be repeated as recorded.
The distinct-host cells of the Stage 1 campaign (Task EY, fcstor010–014)
serve K rows on `9bb50fe79` and are rerun on the frozen candidate.

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
  code's four-node cap is raised (implemented, cap 16; see K28).
  Historical prerequisite wording: “until then the third column is UNSUPPORTED,
  not NOTRUN.” Current larger-topology acceptance is NOTRUN, not unsupported
  by the former four-node cap. Record further coverage of
  the claimed maximum and topology-dependent limits; one 5-node PASS does not
  qualify arbitrary cluster sizes.
- [ ] **A4** Use distinct physical hosts for distributed acceptance. A private
  cluster with three daemons on one host is useful regression evidence, not
  proof of three independent node/failure domains. Task V defines a private
  `fcstor-4x1-tcp` topology (user-selected fcstor012–015 in place of the lab
  hosts); [four-host dry run](2026-10/a4-lab-topology-dryrun-20261028.md) completed with D3
  FAIL (K52 immediate read-back mismatch); other eight requested gates PASS.
  Topology defined; this rehearsal does not close A4. **D46 (user, Oct 28):**
  3, 4 and 5 distinct storage hosts on fcstor007–011 (`/data1/01`), each over
  TCP and RDMA (six cells); alpha 1.0 must also work on all 13 fcstor nodes
  (fcstor003–015). Execution NOTRUN.
- [ ] **A5** Record quota, disks, RDMA devices, mount options, clients, concurrency,
  deadlines, fixture counts/sizes and expected results for every topology.
  Fresh-format setup only when a format change requires it; no compatibility work.

## A13. Simple startup and online growth (accepted Oct 23 amendment)

- [ ] **A13 — Required alpha gate.** Implement and pass the
  [startup and node-addition checklist](2026-10/alpha-1.0-node-addition-20261023.md):
  minimal seed/join commands, no mandatory member-list file, automatic initial
  three-member formation, safe live 3-to-4 and further additions, usable added
  capacity, stable existing data placement, interrupted-join recovery and
  single-loss verification. Always 2+1. Online removal remains deferred.
  Current D39 fixed-membership code/model does not close this gate.
  **Oct 25: real-host gate `a13-node-addition` in `scripts/qualify.py`
  ([Stage 2 gates](2026-10/stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
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
  [inventory](../how-it-works/settings.md) of every variable, CLI
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
  [decision table](2026-10/a6-settings-inventory-20261024.md#decision-table-for-the-parent-a7)
  with a recommendation each, for the user; none removed. `EFS_RDMA_FIRST`
  turned out to be trace only, not a selector.**
  **`EFS_READ_VERIFY` (D42): done Oct 24.** The
  [GET-path audit](2026-10/read-verify-audit-20261024.md) found the client re-hash
  not redundant (with it off, 12 of 51 negative cases returned wrong bytes
  on chunk GET, reconstruction, retries and spans); the re-hash is built in,
  the variable is removed and refused at startup.
  **Oct 24, A7 done as decided in D42 ([evidence](2026-10/a7-removals-20261024.md);
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
  ([evidence](2026-10/d43-expiry-20261025.md)). A/B selectors 15 → 0 (Nov 9 correction: this line said 15 → 1 with
  `EFS_READ_VERIFY` waiting on its audit; the audit and removal above were
  done Oct 24, and A12 counts 0). Live qualification of the retained paths
  stays open (A11).
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
  **Nov 10, numeric audit (Task FA, D46 item 7, branch
  `claude/epic-yonath-69bc96`; [evidence](2026-11/a8-tunables-20261110.md)):
  all 40 numeric tunables (the 36 "Keep until A8" rows and four `#ifndef`
  constants) have a disposition: 18 built in, 14 test-only (6 `EFS_FAULT_*`
  hooks in `EFS_FAULTS` builds, 7 test hooks with no environment name, the
  `EFS_STOP_MS` test seam), 8 justified operator configuration (the five
  client memory budgets, `EFS_QUERY_REFRESH_MS`,
  `EFS_RAFT_APPLIED_INTERVAL_MS`, `--writers`),
  0 for the user. 28 more names are refused at startup (61 in all). Finding
  for the user: the nuc-efs launch defaults `EFS_RAFT_APPLIED_INTERVAL_MS`
  to 1000 ms against the built-in 100 ms (not changed). A8's numeric part is
  done once the branch merges; the item stays open for the deployment-input
  review and the live qualification of the retained paths.**
- [ ] **A9 — Metadata leadership.** Remove `EFS_METADATA_BALANCE` as an operator
  choice after selecting and qualifying the automatic policy. Do not force its
  present common-voter/co-located-leader preference on as a substitute for that
  decision. Preserve quorum, safe transfer and catch-up; expose actual leadership
  diagnostically. This does not claim the later many-group design is complete.
  **Oct 24 (D42, [A7 evidence](2026-10/a7-removals-20261024.md#metadata-leadership-a9-part)):
  the variable and the co-location pump are removed; elections alone place
  leaders; `efs-mgmt status` reports actual placement per group and node
  (`raft-status` per node). Placement is not yet measured under A11 load.**
  **Oct 25 ([A6–A12 hygiene](2026-10/a12-hygiene-20261025.md#a9-leader-placement-item-2),
  on devel since `2e8a09de5`, Oct 25; Nov 9 correction of "not merged"):** `scripts/leader_placement.py`
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
  [A7 evidence](2026-10/a7-removals-20261024.md#for-the-parent-changes-outside-this-repository-a10);
  not yet applied.**
  **Nov 9 (read-only check):** `~/git/devops/nuc-efs` at `8bc872e` (Oct 24)
  no longer sets or forwards any removed setting; the names remain only in
  `test_start_mode.py`'s list of removed keys. Portal and gateway settings
  were not checked.
- [ ] **A11 — Reduced qualification matrix.** Publish the remaining supported
  configuration dimensions and rationale. Qualify the selected paths under the
  topology/profile gates below, including automatic-policy boundaries, load
  changes and orderly restart. Report before/after behavior-switch counts and
  reachable configuration counts where defensible; do not claim exhaustive
  coverage from testing flags individually or from pairwise tests alone.
  **Oct 24, matrix and plan published ([A11](2026-10/a11-qualification-matrix-20261024.md);
  merged into devel (`646a43041`)); qualification
  NOTRUN.** Dimensions: transport (`tcp`, `rdma`; `auto` resolves to one),
  members 3–16, `O_DIRECT` only, one root per node, one or two client
  mounts, one feature profile, 33 numeric tunables at built-in values.
  Selector × transport × member count: 1,376,256 before A6/A7, 28 now
  (A/B selectors 15 → 0). Required exhaustive qualification: TCP/RDMA × 3, 4, 5
  distinct hosts (six full runs); 6–16 sampled (loopback 6–8 only). Every
  Stage 2 item is mapped to its gates, cells, duration and prerequisites in
  `scripts/qualify_matrix.json`; `qualify.py run --matrix … --topology NAME`
  runs one topology. No stub gate is left (Oct 25: A13, C5, D1/D2, D3 and E4
  are [real gates](2026-10/stage2-gates-20261024.md), loopback-3 PASS; B6 and D4 since
  their merge). The original lab-host topology was blocked by firewalld and
  amd's wired link; Task V's user-selected fcstor012–015 private TCP topology
  has formed and run the private four-host dry run (historical D3 FAIL; later
  Task X corrected D3 PASS, K52 DECIDED under K10/D40). A4
  acceptance remains open; no three-host RDMA topology is qualified by this run.
- [ ] **A12 — Closure.** Every A/B selector has either been removed with its
  alternative implementation or is an explicitly justified, reviewed support
  dimension under A8. No unexplained experimental, legacy or correctness-policy
  toggle remains in the alpha product. Record failures and gate results for the
  integrated candidate; flag removal alone is not correctness evidence.
  **Oct 25 ([closure](2026-10/a12-closure-20261025.md), on devel; Nov 9
  correction of "not merged"):** every A6 selector is listed
  with its disposition. All 15 are removed and refused, and `EFS_TRANSPORT`
  is a justified A8 support dimension. A/B selectors 15 → 0; reachable A/B
  combinations 32,768 → 1; variables a normal build reads 105 → 77. Every
  remaining reader has a retained kind: deployment input, numeric tunable,
  observability, fault/test hook or tool input. `env_inventory.py --check`
  now fails on any other kind. A dead-code sweep and the removal of buffered
  I/O remnants from the harnesses and docs are in the
  [hygiene evidence](2026-10/a12-hygiene-20261025.md). D43 (Oct 25) ships expiry and
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
  [evidence](2026-10/s-docs-alpha-1.0-20261024.md)):** every binary, `efsd`/`efs-fuse`
  option, `efs-mgmt` and `.efs` command and user-settable environment
  variable marked in or out with a reason, checked against `a2326a369`.
  Rows marked *proposed* (for example `--no-direct-io`, `--writers`,
  `--no-persist`, `add-node`, `shrink-quota`, `raft-step-down`) wait for the
  user; findings F2–F8, F11, F12 list help-text gaps and profile defaults.
  **Audited Oct 25 against `ec3e09306` ([audit](2026-10/docs-audit-20261025.md)):**
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
  being re-enabled (audit finding U19). **Nov 9 correction:** expiry is
  in every build since D43 (no `EFS_BUILD_EXPIRY` in the Makefile). Seven
  rows are still *proposed* on the surface page (`--writers`, `sync-status`,
  `pack-compact`, `raft-resync`, `shrink-quota`, `raft-step-down`,
  `data-commit`); D46 item 8 left them with the user.
- [ ] **S2 — POSIX subset.** Publish the supported operations and every known
  gap (for example, mmap is skipped in the extras suite). Each gap is either
  fixed or justified as a limitation (F4). No gap counts as PASS.
  **DRAFT Oct 24 ([POSIX subset](../using/alpha-1.0-posix.md)):** supported
  operations from the core (216), extras (5), two-client (68) and
  durability (27) cases; deviations P1–P14 with their suite cases (shared
  mmap W104, K10 fork readback, `renameat2` flags, `fallocate` modes,
  non-`user.` xattrs, `mknod`, D28, D32, W159 open). Recorded runs are on
  `cb970ca7c` and `163241584`; none on the base; C1 per topology NOTRUN.
  **Audited Oct 25 ([audit](2026-10/docs-audit-20261025.md)):** case counts match
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
  fixed Oct 24). **Audited Oct 25 ([audit](2026-10/docs-audit-20261025.md)):**
  every workflow run on a loopback cluster at `ec3e09306`; the guide now
  describes `status`'s full output and exit status, the clean-stop output
  and stop record, the restart rules after a forced, failed-barrier or
  killed daemon (verified recovery, K36), K34, K37 and `Protection: LOST`.
  Finding U4: a clean client stop failed at the session revoke
  (`revoke rc=-13 phase=storage-fence`) while any member was down, because
  the storage fence had to reach every configured member. **D46: required
  fix, K54: FIXED in wave 42 (majority-sealed fence and revoke-ladder re-routing,
  any one member down; [evidence](2026-10/k54-member-down-unmount-20261030.md));
  the orcd-mini stage is NOTRUN until the live campaign.**
- [x] **S4 — Effective settings.** **D46: W122**, the listing of effective
  settings, is required in alpha 1.0. It supports A8 and A10.
  **DONE Oct 30 ([evidence](2026-10/s4-effective-settings-20261030.md)):**
  `efs-mgmt settings NODE:PORT` lists the daemon's 9 deployment inputs and
  the numeric tunables (value, default, provenance, kind), the cluster
  `.efs` and expiry policies, the durability window, the transport and the
  build; `efs-mgmt settings --mount PATH` lists the client's. Read-only, one
  page, no new setting. Tests: `tests/test_settings_inventory.py` (a
  setting without a row fails) and `tests/test_settings_listing.py`.
  Surface page: [effective settings](../using/alpha-1.0-surface.md#s4--effective-settings);
  limitation L24 is removed from the release notes.
- [ ] **S5 — User documentation.** Install, configuration, supported
  topologies, the reduced configuration matrix (A11) and the published
  limitations.
  **DRAFT Oct 24 ([alpha guide](../using/alpha-1.0-guide.md)):** install,
  configuration, topology (3–16 nodes, 2+1, distinct hosts), transport and
  direct I/O, quotas, failure behavior (one node down, rebuild on return,
  two down). The A11 matrix waits for A11. **Revised Oct 25
  ([audit](2026-10/docs-audit-20261025.md)):** failure behavior brought up to K34,
  K35, K36 and K37 and checked on a loopback cluster; `operations.md`,
  `power-user.md`, `quickstart.md`, `efs-commands.md` and the README no
  longer describe buffered I/O, `EFS_COMMANDS`, splice or `add-storage` as
  current; `v030-candidate.md` is marked historical.
  **Nov 9:** S2, S3 and S5 were last audited against `ec3e09306` (Oct 25);
  `tests/test_cli_docs.py` guards command and option names only. A re-audit
  on the frozen candidate is owed for all three.

## B. Verify geometry, placement and usable capacity

- [ ] **B1** Check ordinary chunks and packed segments use exactly D1/D2/P, each
  on a distinct eligible node. Verify actual persisted fragment inventories and
  authoritative mappings, not just placement-function outputs.
  **Oct 24 (branch merged into devel (`5a09fd938`)): loopback
  PASS at 3 to 8 members: every chunk row, span and pack object of the test
  files names D1/D2/P on three distinct members, and every store's fragment
  records match them (on the row's node, nowhere else). FIXED: the reader
  decoded a row without valid nodes from placement recomputed now; it is
  refused. Real-host run NOTRUN ([evidence](2026-10/b-geometry-capacity-20261024.md)).**
- [ ] **B2** Check partial chunks, empty files, alignment/padding, checksums and
  boundary sizes. Reconstruct exact payloads from each valid two-fragment pair;
  reject corrupt or mismatched fragments rather than returning fabricated bytes.
  **Oct 24: loopback PASS: 19 boundary sizes (0 B to 1 MiB + 3) exact
  through every mount; on the stores D1, D2 and P = D1 ^ D2 and every pair
  rebuilds the chunk; a partial chunk is a whole 2+1 span object; a corrupt
  D1, or another chunk's D1, is refused and the bytes come from D2 + P; two
  corrupt fragments are EIO. Unit: every pair for empty/page/half-chunk/
  chunk edges at 128 KiB to 1 MiB chunks and segments. Real-host run
  NOTRUN ([evidence](2026-10/b-geometry-capacity-20261024.md)).**
- [ ] **B3** With diverse inodes/chunk indexes, confirm 3-node placement uses all
  three; 4-node placement rotates the omitted node; larger placement uses the
  intended node set. Report placement histograms, roles and per-node writes.
  **Oct 24: loopback PASS, grown 3 -> 8 one member at a time: three use all
  three for every chunk, four rotate the omitted member (the two chunks of a
  file omit different members), five to eight give every member every role;
  histograms in the evidence. Unit: exact rotation at four, every role
  within 10 % of the mean at 3 to 8 (dense and gapped ids). Real-host run
  NOTRUN ([evidence](2026-10/b-geometry-capacity-20261024.md)).**
- [ ] **B4** Audit packing placement separately: record group voters and eligible
  fragment destinations, demonstrating what capacity is actually usable. Do not
  infer packed placement from ordinary-file placement results.
  **Oct 24: loopback PASS: packed segments recorded per pack object; group
  2 votes on {2,3,4} from four members while segment fragments land on
  every member (voters' share 0.73 at four, 0.38 to 0.44 at seven and eight); usable
  packing capacity is the ring's 3-of-N bound, not the voters'. Real-host
  run NOTRUN ([evidence](2026-10/b-geometry-capacity-20261024.md)).**
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
  headroom budget. Real-host run NOTRUN ([evidence](2026-10/b-geometry-capacity-20261024.md)).**
- [ ] **B6** Exercise a deliberately unavailable fragment/destination under a
  bounded controlled test. Reads reconstruct or fail explicitly; writes must
  never report success without their documented durable publication requirement.
  Verify the result after orderly restore/restart, without claiming repair.
  **Oct 24: loopback PASS (on devel since `74e03592a`, Oct 24; Nov 9
  correction of "not merged"). `tests/test_b6_unavailable.py`: node 3 of three down,
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
  beyond W154 ([evidence](2026-10/b6-d4-unavailable-progress-20261024.md)).**

## C. Finish the enabled foreground paths

- [ ] **C1** Core mounted POSIX, extras and two-client suites pass on each
  topology. Unsupported cases have explicit scope and SKIP/NOTRUN, not PASS.
  **Nov 9:** gates `posix-core`, `posix-extras`, `posix-2client`; one-host
  PASS in the `loopback-3-full` run at `babc80ced`
  ([Stage 2 gates](2026-10/stage2-gates-20261024.md)); real hosts NOTRUN.
- [ ] **C2** Verify create/write/fsync/close/reopen; random and sequential I/O;
  overwrite, sparse holes, append, truncate, rename, hard links, unlink/open
  handles and session fencing with exact expected contents and metadata.
  **Nov 9:** gates `posix-core`, `posix-durability`; one-host PASS in the
  same run; real hosts NOTRUN.
- [ ] **C3** Exercise concurrent readers/writers on same and separate files,
  independent directories and large hot directories. No unexplained EIO,
  stale payloads, lost names or indefinite EBUSY; legitimate refusals stay visible.
  **Nov 9:** gates `posix-2client`, `posix-core`, `append-failover`;
  one-host PASS in the same run; real hosts NOTRUN.
- [ ] **C4** Qualify publication receipts, retries and lost replies on the current
  path. A successful acknowledgement must not outrun durable payload/mapping
  ownership; a retry must not repeat a completed mutation incorrectly.
  **Nov 9:** gates `append-failover`, `k14-torn-read`, `k13-tickets`;
  one-host PASS in the same run; real hosts NOTRUN.
- [ ] **C5** Run the current apply/protocol/session correctness gates associated
  with W127–W134/W72 and D25 where applicable. Carry forward scoped PASS evidence,
  but verify the integrated candidate and retain remaining limitations.
  **Oct 25: real-host gate `c5-protocol-live` in `scripts/qualify.py`
  ([Stage 2 gates](2026-10/stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent): W133
  declaration gate and W140 tags over the wire on every member, removed
  settings refused on every host, K5 session fence and W72 unmount revoke.**

## D. Packing, reclaim and enabled `.efs`

- [ ] **D1** Verify small-file packing produces real packed mappings and inventory
  savings, with exact reads before/after packing and after orderly cold restart.
  **Oct 25: real-host gate `d1-packing` in `scripts/qualify.py`
  ([Stage 2 gates](2026-10/stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent).**
- [ ] **D2** Exercise overwrite/truncate/unlink and compaction of shared segments;
  retained files stay intact, retired references are reclaimed exactly once and
  space eventually becomes reusable without deleting live fragments.
  **Oct 25: `d1-packing` overwrites, truncates and unlinks packed files and
  requests compaction on every topology ([Stage 2 gates](2026-10/stage2-gates-20261024.md),
  loopback PASS); `gc-reclamation` FAILs on `merge-wave6` and its base
  (`fsync acknowledged after unclean stop`).**
  **Nov 9 correction:** the driver's `gc-reclamation` (now `--posix` plus
  `gc-put-barrier`) PASSes since Task CG, Oct 31
  ([page](2026-10/k11-put-barrier-case-20261031.md)); real hosts NOTRUN.
- [ ] **D3** Run `.efs` commands/options and concurrent query suites while ordinary
  I/O continues. Check result correctness, authorization, freshness and bounded
  refusal/retry behavior, including initial catalog creation and restart.
  **Oct 25: real-host gate `d3-efs-load` in `scripts/qualify.py`
  ([Stage 2 gates](2026-10/stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent).**
- [ ] **D4** Demonstrate bounded progress with packing, indexing and KV maintenance
  active: foreground deadlines pass, memory/queues remain bounded and background
  work progresses. Diagnose repeated stalls, rather than relaxing tests to pass.
  CPU optimizations are required here only when needed to meet these gates;
  broader optimization remains later roadmap work.
  **Oct 24: loopback PASS (on devel since `74e03592a`, Oct 24; Nov 9
  correction of "not merged"). `scripts/d4_progress.py` (qualify plugin `d4-progress`) and
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
  NOTRUN ([evidence](2026-10/b6-d4-unavailable-progress-20261024.md)).**
  **Oct 25 (F1 follow-ups, branch `claude/devel-f1-followups-ae2deb`,
  [evidence](2026-10/f1-followups-20261025.md)):** a fixed-working-set variant
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
  [evidence](2026-10/d4-pack-drain-20261025.md)):** the drain stall had two causes,
  both fixed. A small file rewritten across a pack epoch had two candidate
  keys, and a segment holding both failed silently every pass. The D25
  truncate sweep's backlog (about 5 inodes a second) held the index drain.
  The 60-minute run (intel1) drains in 54.5 s within the default 150 s, with
  0 packer put errors (the fragment resend is now bounded by 2 s, not 4
  tries); `test_d4_plateau.py` is back in `make test-long`; 8-job gate PASS
  with host limits. Found then: quota use grew linearly under the rewrite
  load (52 % at 60 minutes on intel1, ENOSPC at about 45 minutes on amd).
  **Oct 26 ([evidence](2026-10/d4-reclaim-20261026.md)):** the cause was abandoned
  uploads, collectable only at unmount (D31). Each resizing rewrite
  abandoned its first upload because the REPORT host checked an
  unconditional tail image against the client's stale span list. Fixed:
  stored data stays at 118–278 MiB per node over 60 minutes, and the D4
  driver bounds abandoned uploads and stored data. Open: about 3.5 % of
  `O_TRUNC` overwrites still abandon an upload. **Nov 9 correction:** the
  [same page](2026-10/d4-reclaim-20261026.md) records that residual FIXED
  (`acb8a7318`, narrowed in `988efba3e`).
  **Oct 26 (Task D soak, loopback, member churn,
  [evidence](2026-10/task-d-20261025.md#soak-tables-attempt-3-nuc-b2fc06ba5)):** for
  7,195 s with 11 clean returns, a daemon SIGKILL, a client SIGKILL and a
  join, packing advanced on every group-0 leader through each leadership
  change (`packed_total` up to 10,708 per leader term, candidates ≤ 210),
  `.efs` queries stayed under 0.41 s, and every qualifying write-load RSS
  window (5 windows, 37 process rows) stayed within 5 % last-third growth;
  role coverage NOTRUN (the run stopped on K43 first). Not a D4 PASS.

- [ ] **D5** Expiry under the cluster expiry policy (D43): qualified with the
  policy off and on (A2). **Added Nov 9:** the matrix has carried this item
  and its gate `d5-expiry` (`tests/posix/posix_expiry.py`, `make test-long`)
  since Oct 25 ([evidence](2026-10/d43-expiry-20261025.md)); the checklist had
  no checkbox for it. Loopback fixture only; real hosts NOTRUN.

## E. Full verification and orderly lifecycle

- [ ] **E1** Record an exact manifest of successfully acknowledged files and
  expected contents. Verify every payload and its relevant metadata after the
  mixed workload on each topology; sampled checks cannot close this gate.
  **Nov 9:** gate `e1-manifest`; one-host PASS in the `loopback-3-full` run
  at `babc80ced` and on `nuc-private-3` (E2 note); real hosts NOTRUN.
- [ ] **E2** Cleanly stop clients and unmount without force or lost pending writes;
  drain and stop daemons, restart the same-format stores, establish readiness,
  remount and repeat full verification. Record all stop/readiness deadlines.
  **Client stop on a busy client fixed (Oct 24, [evidence](2026-10/e2-clean-stop-20261024.md),
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
  **Nov 9:** gate `e3-turnover`; one-host PASS in the `loopback-3-full` run;
  member catch-up needs per-node stops, which the orcd-mini topology cannot
  do; real hosts NOTRUN.
- [ ] **E4** Run small-file scale and large-file fixtures with declared counts,
  quotas and concurrency; report actual completed versus target counts. Include
  cold reads, concurrent query load and delete/reclaim checks.
  **Oct 25: real-host gate `e4-scale` in `scripts/qualify.py`
  ([Stage 2 gates](2026-10/stage2-gates-20261024.md)); loopback dry run PASS on xorinox,
  real hosts NOTRUN (parent); the
  defaults (5,000 small and 2 × 256 MiB files per member) have not run.**
  **Task D interaction endurance (Oct 26, branch `claude/stoic-goldwasser-06dbca`,
  [evidence](2026-10/task-d-20261025.md#finish-on-intel2-and-nuc--2026-10-26)): FAIL,
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

- **K32 is a Stage 3 input ([D50](decisions.md#d50--k32-moves-to-stage-3-user-nov-10), user, Nov 10).** `ior easy` write is at 0.36 to 0.6 GiB/s against about 1.3 GiB/s before the fourth wave (row K32, DECIDED). The P1 baseline reruns the O2 measurement; P3-G and P3-J are its candidate causes; Task FI's Stage 1 measurements, once committed, are the starting point.
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
  - findings 3–5 of the [ORCD tiny-file profile](2026-10/orcd-tiny-profile-20261022.md):
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

### Stage 3.0 letter tasks — data and metadata performance

These **P3-A–P3-F** identifiers belong to Stage 3 of **alpha 1.0**, not the
alpha 3.0 roadmap or the earlier Stage 1 session letters. All six are
**OPEN / measurement NOTRUN**. Graph-led source review at `b9c2fad85` found
optimization candidates, not measured bottlenecks or proven speedups. Planning
now does not waive the Stage 2 prerequisite above. The separate
**efs-coding - graphify fixer** session owns the two data-commit correctness
findings (metadata-RPC deadline propagation and closed-object retirement);
these performance tasks do not duplicate or close them.

- [ ] **P3-A — Data writes: overlap fragment request startup.**
  `src/client/write.c::put_fragments_parallel_once` checks out connections
  sequentially, then polls/sends to each peer sequentially before collecting
  replies. Measure checkout, send/backpressure and reply time separately on
  healthy and congested peers. Evaluate bounded concurrent/nonblocking startup
  so one slow peer does not delay requests to healthy peers. Preserve immutable
  tickets, distinct-node placement, acknowledgement accounting, transport
  semantics and the original end-to-end deadline. Gate: payload verification,
  partial sends, pool exhaustion and peer failure, plus throughput and tail
  latency on both qualified TCP and RDMA profiles.
- [ ] **P3-B — Data reads: overlap the two fragment GET starts.**
  `src/client/read.c::get_two_parallel_run` calls `get_pair_start` serially;
  connection checkout/send for the first peer can delay the second even though
  replies are polled together. Measure cold/warm sequential and random reads
  under pool pressure and a slow peer; evaluate bounded startup overlap.
  Preserve fragment identity/digest checks, fallback decode, shared deadlines
  and ownership of outstanding replies/buffers. Gate: exact reads, cancellation,
  stale mappings and peer failure, with throughput and p95/p99 latency; do not
  buy a healthy-path gain with unbounded extra requests or memory.
- [ ] **P3-C — Write bookkeeping: compact settled tickets once per batch.**
  `src/client/put_ticket_owner.c::settle_forget` removes each retired ticket
  with a separate suffix `memmove` under `g_ledger.mu`. Measure bytes moved and
  mutex hold/wait time across ledger occupancy and settlement batch sizes.
  Evaluate one stable compaction per batch, preserving sequence order, exact
  retired masks/leading counts, closure notifications and concurrently added
  or active tickets. Gate: sparse/dense retirement, lost settlement replies,
  concurrent PUT/settle and bounded-capacity regressions; show reduced copying
  and lock occupancy and check end-to-end write/close latency.
- [ ] **P3-D — fsync: reuse bounded COMMIT workers and share covered work.**
  `src/client/data_commit.c::stripe_prove` creates and joins workers per sweep
  and participating node. Measure thread creation, overlapping COMMIT RPCs and
  fsync latency under concurrent files before selecting worker reuse, coalescing
  or both. A shared result may cover only acknowledgements preceding its COMMIT
  and matching its storage boot verifier; retain per-stripe proof, degraded
  marks, caller deadlines and error propagation. Gate: acknowledgements arriving
  during COMMIT, incarnation changes, failed replies and independent caller
  deadlines, plus exact data after restart. Never substitute one cluster-wide
  durability assertion for per-stripe proof.
- [ ] **P3-E — Metadata reads: overlap GETCHUNKS group proofs.**
  `src/server/raft_host.c::server_raft_host_getchunks` obtains the second
  collection's ReadIndex proofs sequentially for the groups it observed.
  Evaluate the existing `host_read_index_pair` machinery for overlapping those
  proofs, after auditing its local/remote and split-leader cases. Keep the first
  and second collections, post-admission proof requirement, applied-index
  coverage, transaction decisions and inode epoch/history recheck. Gate:
  concurrent publication/truncate, changed leaders and lagging followers;
  measure proof wait, retries, RPC count and GETCHUNKS/read p95/p99. No reuse
  of an earlier completed proof as permission for a new read.
- [ ] **P3-F — Metadata writes: overlap CREATE routing proofs.**
  `src/server/raft_host.c::host_create_ex` reads the parent before discovering
  and fencing an uncovered target group. Measure the extra round on cross-group
  creates. Evaluate speculative parent/target paired proofs using the existing
  lookup-planning approach, then revalidate routing against authoritative parent
  state. A wrong hint must not add an unrelated group's failure as a CREATE
  dependency. Preserve session/opid receipts, allocation and transaction verdicts.
  Gate: stale hints, changing directory state, leader changes, lost replies and
  concurrent same-name creates; compare create/close throughput and p95/p99.

**Suggested order:** baseline first; P3-C and P3-E are the first bounded
experiments, then P3-A/P3-B, with P3-D/P3-F selected by measured cost. This is
not a claim that any candidate dominates the workload. Existing persistent
GET/PUT pools, bounded prefetch, full-chunk copy avoidance and admission batching
must be accounted for, not proposed again as missing features.

**Shared acceptance:** use P1's frozen workload/topology and three runs per build,
record exact source/profile and all-user host activity, and compare throughput,
p50/p95/p99, CPU, allocations/copies, lock/queue wait, RPC/Raft-entry counts and
memory bounds as relevant. Separate profiler attribution from formal timing.
Retain failures and raw evidence; reproduce gains on private baseline/candidate
builds, rerun affected Stage 2 correctness and applicable sanitizer/concurrency
gates, then record keep/revert under P4. No new user-facing A/B flags, relaxed
bounds, weaker durability or new format/placement scope. No task is complete
merely because a microbenchmark improved; the affected end-to-end workload must
be checked for regressions.

### Stage 3.0 letter tasks — server and protocol theories (Nov 9 review)

**P3-G–P3-K** come from a code review of the server and of the client-server
and server-server protocol at `9bb50fe79`
([theories, evidence and refuting measurements](2026-11/stage3-server-protocol-theories-20261109.md)).
All five are **OPEN / theory, measurement NOTRUN**: each joins a measured
baseline already in the tree to a verified code fact and names the run that
would refute it. Common reading: on the measured workloads the daemons are
idle (9 of 192 cores for 1,649 creates/s, leader samples 63 % in the kernel),
so throughput at fixed concurrency is set by the latency of one consensus
round and by the number of sequential rounds per operation. The rules of the
first six apply: Stage 2 first, P1 baseline, no new switches, no weaker
durability, no format or placement scope.

- [ ] **P3-G — Consensus round: shorten the thread handoff chain.**
  One round wakes a sleeping thread eight times (proposer to pump by eventfd,
  pump to per-peer sender thread through a heap copy, follower connection
  thread to its pump through the inbox copy, the same back, pump to proposer);
  the pump sleeps in `poll` without spinning
  (`src/server/raft_host.c::host_propose_impl`, `host_send`, `host_sender`,
  `server_raft_host_inbox`, `host_pump_poll_timeout`). Theory: round latency is
  the sum of those wakes, not disk or CPU work; a stat with no disk write
  takes 1.0–1.8 ms p50. Measure `EFS_WRITE_PROF` phases (`commit_apply`
  against `persist`) and `perf sched latency` first. Evaluate, smallest first:
  a bounded pump spin while a round is outstanding; the pump sending on the
  peer socket itself with the sender thread as the backpressure path; the
  follower's connection thread running the AppendEntries receive step.
  Preserve the durable-before-ack rule, receive-persist cohorts, `h->mu`
  ownership rules and bounded idle CPU. Gate: TSan lane, K1 stop gates, leader
  change and slow-follower faults; compare create, stat, unlink and fsync
  p50/p99 and creates/s. If `persist` dominates, close with that finding.
- [ ] **P3-H — Small files: fewer sequential rounds per file.**
  Create (with its lease), ticket admission, fragment PUTs, publication,
  settlement and close are up to five consensus rounds and one storage round
  per tiny file; W94 measured about +20 % creates/s for one round removed.
  The ticket already carries the body digest and fragment checksums
  (`include/efs/put_ticket.h`), so for a single-object file admission and
  publication name the same object and settlement can retire in the
  publication's apply. First record a per-file histogram of entry tags and of
  the rounds the writer blocks on. Preserve K13 (fragments accepted only for
  an admitted ticket) and K14 (atomic publication). A fused
  admission-publication needs a new log command encoding and a rule for when
  the client may hold fragments until the publish round: both need the user's
  scope decision before any code.
- [ ] **P3-I — Read-only opens: lease edges per file read.**
  Every first open of an inode on a mount, read-only included, takes the HOLD
  lease by a log command and the last release returns it
  (`src/client/efs_fuse/open.c::efs_fuse_open_ino_run`); open p50 is 0.7 ms
  above stat p50. Evaluate a bounded linger of the lease after the last close
  with batched returns, so re-opens are free and close edges stop competing
  with writers on group 0. Removing the open edge for read-only opens changes
  what an unlinked open file promises and is the user's decision, not part of
  this task. Gate: unlink-while-open, session expiry and K54 clean unmount
  with lingered leases; measure lease entries as a share of group 0 commits
  under a read-mostly tree and drop the task below a fifth.
- [ ] **P3-J — Ticket admission: combine concurrent single ADMITs at the leader.**
  Task U's named follow-up, not listed here before: 48–73 % of FUSE admissions
  are single `ADMIT` entries although `ADMIT_BATCH` exists, because the cohort
  forms on the client from kernel callback timing. The leader can combine the
  admissions waiting in one proposal window into one `ADMIT_BATCH` (existing
  tag). Rerun Task U's matrix untraced; compare entries/MiB and ior easy
  write. K13 admission semantics, per-ticket verdicts and bounds unchanged.
- [ ] **P3-K — PUT barrier: keep unrelated dirty data out of `syncfs`.**
  The fragment barrier is `syncfs(root)` every 200 ms
  (`src/server/store.c::put_sync_barrier_locked`); fragments are `O_DIRECT`,
  but KV segments are written buffered and synced at their end
  (`src/kv/kv_seg.c`), so each barrier writes back a running compaction and
  the Raft and KV log syncs of that moment queue behind it. Theory: this is
  the p99 at 5–20 times p50 with idle CPUs. Test first on private builds that differ only in the barrier window
  (50, 200 and 1,000 ms; the `EFS_PUT_SYNC_MS` setting was built in by Task FA,
  wave 69) under tiny-file load; the spikes
  should follow the barrier period and `last_barrier_us`. Evaluate pacing
  compaction output with `sync_file_range`, as the Raft log rotation already
  does. The barrier keeps covering every acknowledged fragment.

**Order for these five:** P3-G's refuting measurement first (it decides
whether round latency or round count is the larger term), then P3-H's
histogram, P3-K's no-code test, P3-J with the ior easy baseline, P3-I last.
**Reviewed and left out** (numbers on the page): an owner per group (group 2
carries 1–2 % of commits; the concentration is W66), single-threaded apply
(2–3 % of leader CPU), the sorted-array memtable, the single RDMA poller,
fragment-file and chunk-size costs (formats), and a clock-based read lease
(would rest read correctness on clock drift).

## F. Release evidence and alpha 1.0 exit

- [ ] **F1** Unit/integration, enabled-feature and applicable memory/concurrency
  checks have explicit PASS/FAIL/NOTRUN results. Docs and code tests both report;
  an unavailable diagnostic is disclosed, never silently counted as passed.
  **Task I reviewed Oct 28 UTC ([evidence](2026-10/task-i-20261026.md)):**
  `47ca08eaf` xorinox gate 518 / 0 FAIL / 13 NOTRUN, hour FAIL stored
  data +~56.8%; RSS PASS, 2080 exact and zero packer errors. Historical
  xorinox measurements were shared-client, not proven quiet. Node9900
  `47ca08eaf` RDMA FAIL RSS; TCP FAIL load RSS +14.05%, 2080 exact,
  pack errors 0/0, drain 81.3 s. Reclaim-fairness `6550b678d` and
  allocator lifetime `d7e265746` have native unit evidence, new full gates/
  hours owed (`d7e265746` hardware gate FAIL: four LSan runtime
  failures, independently reproduced; corrected `00ccd12f3` gate 524 total /
  0 FAIL / 18 NOTRUN, native RDMA 9/9 PASS; hour FAIL foreground RSS
  +13.04% and drain, 2080 exact, pack errors 0/0; TCP hour PASS with
  all RSS <=1.63%, drain 92.9 s, 2080 exact; overall pair FAIL).
  AMD native default RoCE PASS 9/9; full gate FAIL 3/520, hour NOTRUN;
  fixed-harness/cleanup continuation `d7e265746` timed out waiting for W123;
  no store touched. Fresh `14010248c` preservation/cleanup succeeded: six
  synthetic lines exact, peer/three daemons stopped cleanly; native RDMA 9/9
  PASS, gate FAIL 1/526 on observer compilation, corrected unit PASS; fresh
  small-page candidate `a083dcbf3` live protocol now PASS (scoped): node9900
  RDMA/TCP and AMD TCP hours, each 2080 exact/pack errors 0/0, independent
  RSS within bounds; drains 77.8/147.0/78.9 s. Hardware is shared-client;
  AMD interval quiet. Gates 527/0 FAIL with 18/14 NOTRUN; native RDMA 9/9
  each. Task I protocol complete, F1 remains unchecked for missing release
  coverage. Original failures, narrow TCP drain margin and unproven prior
  intermittent cause remain recorded.
  **Memory and concurrency gates run on the candidate Oct 25 (xorinox, branch
  `claude/devel-f1-gates-46ff64` on `merge-wave7`;
  [evidence](2026-10/f1-memory-concurrency-gates-20261025.md)):** `make leak-check`
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
  `claude/devel-f1-followups-ae2deb` ([evidence](2026-10/f1-followups-20261025.md)):**
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
  **Oct 25 Task A rehearsal complete with findings:** [three- and five-member AMD loopback](2026-10/task-a-20261025.md)
  both full runs completed (21 PASS / 1 harness FAIL / 1 NOTRUN); the GC
  sequencing correction passes affected coverage, with a separate product
  revoke failure retained. Code gate: 479 commands, 11 FAIL (base's subset),
  6 NOTRUN. This remains one-host regression evidence; the physical-topology
  ledger and product findings stay open.
  **Oct 28 Task P second rehearsal:** [intel2 three-/five-member matrices](2026-10/stage2-rehearsal-2-20261027.md)
  completed at `b8f232f40`: 23 PASS / 1 driver FAIL / 1 NOTRUN and
  24 PASS / 0 FAIL / 1 NOTRUN. Both rehearsal soaks PASS. The three-member
  D5 port collision is fixed at `dac2361ba`; targeted identity/D5/end-identity
  PASS (3/0/0), including 218/218 expiry-enabled POSIX cases. Historical
  full-run FAIL remains; the corrected final driver has not had another full
  matrix run. Earlier code gate 516 / 0 FAIL / 14 NOTRUN (858 s); final
  `dbebab983` code gate 516 / 0 FAIL / 14 NOTRUN (739 s). Three driver
  fixes have negative controls, including plan mode with no lifecycle
  effects; raw manifests verified and owned scratch pruned. Both F3
  rehearsal columns and timing findings are on the page. This does not fill distinct-host cells or
  endurance RSS/power-loss qualification; F3 stays unchecked.
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
  **Revised Oct 25 ([audit](2026-10/docs-audit-20261025.md)):** checked against
  D40–D42 and the K rows at `ec3e09306`: L6, L7, L8 and L28 now describe
  verified recovery and segment rebuild (K36, K37) instead of "deferred";
  the open-items paragraph lists K34–K37 as fixed on loopback with live
  runs NOTRUN; L22 and L24 are marked as open user decisions (F12, W122).
  Release-notes entries decided so far (D40, Oct 23), one each:
  - **K10/W121 fork/read race:** a fork while another thread has a
    FUSE direct-I/O read in flight can leave stale bytes in the caller's
    buffer on affected kernels. Fsync is not the trigger; no client
    workaround in alpha ([diagnosis](2026-10/orcd-w121-fork-read-20261016.md)).
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

**Task Y register snapshot, devel `9e4d66a8c` (Oct 28): 52 rows;
FIXED 11, VERIFIED 0, VERIFY 35, DECIDED 2, CLOSED 2, OPEN 2.**
The three Stage 1 cells below share these counts, not three independent
qualification results. OPEN: **K48** (Task S/coordinator historical
attribution), **K52** (Task X readback classification). VERIFY:
**K1, K2, K4, K5, K6, K8, K11–K22, K29, K32–K44, K49–K51**
(35 rows; each names its owed gate/owner in K and in the
[remaining-work table](2026-10/k-register-reconciled-20261028.md#remaining-verify-work)).
K9/K27 are CLOSED with their different reasons; K10/K23 are DECIDED with
D40/D42 scope limits. No K row is promoted to VERIFIED without a
frozen-candidate gate. Functional decisions and the flag audit remain
Task Z work; A4 topology, S4/W122, F12 and power-loss scope are not decided
by this count reconciliation (they are decided in D45 and D46).

| Evidence | 3 physical nodes | 4 physical nodes | >4 physical nodes (record N) |
| --- | --- | --- | --- |
| Known-defect register K (Stage 1; once per candidate) | OPEN — rows through K88 after wave 51: 46 FIXED / 0 VERIFIED / 35 VERIFY / 3 DECIDED / 3 CLOSED / 1 OPEN (K38, reopened by Task CV Nov 4); K88 (a single writer loses a chunk to its own async REPORT, `report-stuck`; the `rc=-4` and `stage=3 rc=-13` seen in `test_bench_published` are not the cause) found by Task CJ, FIXED by Task CK, blocks alpha: no: the slot-less `efs-bench` write path kept `base=0` after a background REPORT committed the chunk's first image, the committed image is now the CAS base and `efs-bench` fails on a recorded writeback error; loaded reproducer 12 of 3360 runs before, 0 of 2960 after (2880 at 24 copies and 80 at 8, against the 3000 at 24 copies the row asked), intel1 gate 0 FAIL, no deterministic interleaving check yet (Task CM); acceptance: `test_bench_published` in F1 on the candidate; K87 (failed root `syncfs` retried in a tight loop by the PUT syncer) FIXED by Task CI, blocks alpha: no; K79's TSan stop-bound miss from wave 49's lane F2 (`stop_blocked_stderr`) FIXED by Task CH, `shutdown_leave` exits with `exit_group`, K79 stays FIXED; wave 49 acceptance on devel 8e53122cd: K82-K84 lane F2 PASS (Task CA, xorinox), K16, K19, K42 lane F4 PASS and the K11 put-barrier case repaired and PASS (Tasks CB and CG, intel1), K2 lane F5 26 of 26 PASS (Task CC, amd); K86 (valid session refused `UNREGISTERED` on a concurrent first mkdir under load; rc=-28 was not ENOSPC) found by Task CE, FIXED by Task CF, blocks alpha: no (acceptance: loaded reproducer and `test_bench_published` in F1); K82-K84 (TSan races in `k36_recovery`, TSan race on `rpc_prof_enabled`, UBSan null member access in `lsm_slot_in`, from Task BR's batch F rehearsal) FIXED by Tasks BV, BX and BW in wave 48, blocks alpha: no (acceptance: batch F lane F2 on the candidate); K85 (`UNRECOVERABLE` report window) FIXED by Task BM in wave 48, its one-line fix already on devel from wave 47, blocks alpha: no; K79 (daemon exit with blocked stderr) FIXED by Task BP in wave 47, blocks alpha: no; K48 moved to VERIFY by Task BG (blocks alpha: yes until the storm gate on the frozen candidate and the K74 live listing); K61 FIXED by Task BH; K81 (REPORT reply held behind the blocked stderr sink) FIXED by Task BL in wave 46; K54 and K55 are FIXED (K54's orcd-mini real-host stage PASSed 24/24 on devel in the [Task BQ rehearsal](2026-10/orcd-mini-k54-batch-o-20261031.md)); per-row owed acceptance in the [Stage 1 acceptance matrix](stage1-acceptance-matrix-20261030.md) | Same register/counts; distinct-host gates owed | Same register/counts; acceptance remains OPEN ([Task Y](2026-10/k-register-reconciled-20261028.md)) |
| Functional scope S decided (once per candidate) | PARTLY DECIDED — D45 + D46 (user, Oct 28); W122, the K54 fix and the 33-tunable audit are required work; S1 command dispositions and `--writers` remain open | (same) | (same) |
| Candidate and profile | NOTRUN — reviewed devel `9e4d66a8c`, not a frozen release candidate; K counts above (0 VERIFIED), A1/A2/A5 profile owed | NOTRUN — same counts; A4 decision and frozen profile owed | NOTRUN — K28 cap/layout is implemented (16-node cap); same counts, concrete N/failure domains/profile unqualified; prior “UNSUPPORTED until K28” was historical |
| Flag audit and A/B removal (A6–A10, A12; once per candidate) | PASS scoped static/startup audit at `9e4d66a8c`, driver `1164b55a4` ([Task Z](2026-10/stage1-decision-sheet-20261028.md)); A8/A9/A10 and frozen-candidate obligations remain; K register counts above are not an audit verdict (11 FIXED / 0 VERIFIED / 35 VERIFY / 3 DECIDED / 2 CLOSED / 3 OPEN) | Same audit/decision owner; named K live gates remain | Same audit/decision owner; no new A/B selector or user decision approved by Task Y |
| Reduced-matrix qualification (A11) | NOTRUN | NOTRUN | NOTRUN |
| Placement/capacity and decode | NOTRUN | NOTRUN | NOTRUN |
| POSIX and concurrent I/O | NOTRUN | NOTRUN | NOTRUN |
| Packing/shared-fragment GC | NOTRUN | NOTRUN | NOTRUN |
| Enabled `.efs` under load | NOTRUN | NOTRUN | NOTRUN |
| Full payloads + orderly restart | NOTRUN | NOTRUN | NOTRUN |
| Scale/progress evidence (E4, D4) | NOTRUN | NOTRUN | NOTRUN |
| Performance baseline and targets (P1–P2, Stage 3) | NOTRUN | NOTRUN | NOTRUN |
| Alpha topology acceptance | OPEN — topology decided (D45, Oct 28): the orcd-test cluster in 3/4/5/13-storage-host shapes, clients on the remaining fcstor hosts; campaign order fixed, starts after K48 and K52 | OPEN | OPEN |

These are candidate-specific release cells, not an assertion that no earlier
tests passed. For example, [the October 22 orcd-mini report](2026-10/orcd-mini-deploy-08cc16af4-20261022.md)
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
