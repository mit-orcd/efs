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
| K1 | **W145** remaining parts: acknowledged-write check after the deposed-leader fix, snapshot sent to a near follower, restart election storms, `stop.sh` with a blocked handler | Raft safety / liveness | FIX |
| K2 | **W140** proposal RPC accepts metadata tags this build cannot apply (halts a group) | safety | FIX |
| K3 | **W132** `NODE_LEFT` from any connection drops a node | safety | FIX |
| K4 | **W133** client protocol declaration gate is fail-open | correctness | FIX |
| K5 | **W134** unregistered session identities bypass fencing (with **W72** production session lifecycle) | correctness | FIX |
| K6 | **W127–W131** fail-closed apply (in devel since `08cc16af4`; private RDMA gate PASS) | correctness | VERIFY multi-host |
| K7 | **W83** transaction decision records are never retired | lifecycle / growth | FIX |
| K8 | **W148** modify-under-load returned wrong bytes, correct size, on a never-packed 3.6 MB file (suspected read race: same-size overwrite then truncate-up) | data correctness | FIX ([review](review-pack-validate-once-20261022.md)) |
| K9 | **W87** buffered concurrent append loses records under load | data correctness | FIX |
| K10 | **W121** a read right after a successful fsync returns the previous file's payload (FUSE kernel fork/page-pinning; kernel fix deferred) | data correctness | DECIDE |
| K11 | **W55** span committed but fragment PUTs never landed (NFS/macOS triggers not isolated) | data correctness | VERIFY Linux FUSE; DECIDE NFS/macOS |
| K12 | **W58** `O_EXCL` create answered EEXIST (historical trigger not isolated) | correctness | VERIFY |
| K13 | **W88** a folded span retry resurrects after history eviction; **W86** default PUT/flush ticket integration staged | publication correctness | FIX or DECIDE |
| K14 | **W77** atomic multi-chunk publication and observation | integration gap | FIX or DECIDE |
| K15 | **W64** append reservation replay across leader changes (partial); **W52** REPORT after many `O_APPEND` writes exceeds 30 s | correctness | FIX |
| K16 | **W75** identity-bound fragment integrity format (fresh format, no compatibility) | integrity | FIX |
| K17 | **0a/D27** STALE replay convergence: orcd-mini C1 rerun | correctness | VERIFY |
| K18 | **W54**, **W38** fold/GC data loss: multi-host hardware RDMA gate | correctness | VERIFY |
| K19 | **W61**, **W62**, **W63** GC: final drain and lost-ledger reconciliation | reclaim correctness | VERIFY |
| K20 | **W42** capacity reporting on any node count; protection debt (with B5) | correctness | VERIFY |
| K21 | **W119** concurrent RDMA failures and blocked clean stop (recorded Oct 15, before many fixes); **W65** graceful-shutdown wait | correctness / liveness | VERIFY first, then FIX |
| K22 | **W23** universal follower memory/lag bound | resource bound | FIX |
| K23 | **W71** hardware power-loss gate; **W74** automatic repair; **W123** crash recovery | durability scope | DECIDE (deferred by current scope) |
| K24 | **W149** `EFS_KV_COMPACT_DIE` exits the daemon in normal builds | build hygiene / safety | FIX |
| K25 | **W135**–**W139**, **W141**, **W120**, **W150** test harness and reporting defects | test correctness | TEST |
| K26 | LeakSanitizer aborts on node9900/node9901; F1 needs a working leak check | environment | FIX host or use another leak gate |
| K27 | ior easy write on `d94ca85bf` is 710–863 MiB/s against 1,329 MiB/s on `08cc16af4`, same cluster, three runs, reads unchanged ([deploy report](orcd-mini-deploy-d94ca85bf-20261022.md)) | regression, unattributed | VERIFY (A/B/A′ on orcd-mini, needs a restart) |
| K28 | **W151** the code caps a cluster at four nodes (`EFS_MAX_NODES`, `EFS_MD_RAFT_N`, voter sets, placement ring, client hint bound); the user keeps >4 nodes in alpha 1.0 (decision Oct 22) | implementation gap / scope | FIX: raise the cap with voter sets for n ≥ 5, placement and capacity tests at 5..max, membership and client tables; fresh format, no compatibility |

### Recheck on devel `ab1920fae` (October 22)

Checked by reading the code and the backlog entries; nothing was run on a
cluster. No row was found already fixed.

| Row | Finding on devel |
| --- | --- |
| K1 | Root cause fixed (`76614e8e8`, regression test). Four parts still open, as listed in W145: (1) why a near follower got a 4.7 GB snapshot instead of a tail walk-back (likely a `match_index` 0 rejection resetting `next_index` to 1); (2) whether a client was acknowledged for 30,929,219@62; (3) hundreds of elections per restart; (4) a daemon blocked in a group read outlasts `stop.sh`'s wait. The private reproduction gate is NOTRUN. |
| K2 | Open. Submit still refuses only `EFS_MD_CMD_IS_RESERVED` (`raft_host.c:3381`, `:12075`). |
| K3 | Open. `handler.c:856` still handles `EFS_MSG_NODE_LEFT`; nothing sends it. |
| K4 | Open. Only three requests need a declaration (`handler.c:200`), the version check is `>=` (`:219`), and `scripts/w125_p3_compat.sh` still expects old clients to work. |
| K5 | Open. `efs_session_accept_legacy` has 8 callers (`lock.c`, `meta_apply.c` ×4, `session.c`, `raft_host.c`), and the "legacy … remain compatible" path is at `raft_host.c:11913`. Tied to W72, which is partial. |
| K6 | Merged. Remaining: the W130 one-replica DIR fault injection, and multi-host runs. |
| K7 | Open. `EFS_KV_KIND_DECISION` keys are written and read (`txn.c:972`, `:996`); nothing deletes them. |
| K8 | Open. Repro harness fixed (`a1ceff4cb`); reruns NOTRUN. |
| K9, K13–K16 | Open; backlog unchanged since Oct 11–15 (W87, W88, W86, W77, W64, W52, W75). |
| K10 | Open. User-deferred kernel fix; test harnesses now isolate it (`exclusive_create_visibility.py --spawn`). |
| K11, K12, K17–K20 | VERIFY. The fixes are recorded on NUC/AMD/TCP or private stores; the alpha profile on separate hosts is NOTRUN. |
| K21 | W119's evidence (Oct 15) predates the RDMA, query, apply and Raft fixes since then. Rerun its original gates before diagnosing. W65 is closed for TCP only; it overlaps K1 part 4. |
| K22 | Open. Stalled-compactor hook in tree; the valid memory/lag bound is still owed. |
| K23 | Deferred by the current alpha scope; needs the user's confirmation. |
| K24 | Open (`kv_compact.c:272`, used by `test_kv_lsm.c:1085`). |
| K25 | All open. W135: `check-architecture` still inside `test:`. W136: stub `efs_client_pack_flush` still there. W137: fake reply still 5 bytes. W138: TSan still prints SKIP. W139: `test_rdma_segmented`, `test_expiry`, `test_rdma_xprt` built but never run. W141: fixed 200 × 25 ms wait. W120 and W150 open. |
| K26 | Open (LeakSanitizer aborts under ptrace on both nodes). |

### Proposed fix order

Each step is one change or a small group. Each gets a regression test, then
`make test` and a private-cluster gate.

1. **Your scope decisions.** No code. These change what Stage 1 contains:
   - K10: W121, the kernel FUSE issue;
   - K11: NFS and macOS clients;
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
  scope: containers, expiry (**W90**; resolve under A7), NFS re-export, macOS
  clients, 2+2, automatic repair and crash recovery (K23).
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
- [ ] **F4** Publish alpha limitations: fixed metadata-group count/concentration,
  actual packing placement, supported node range, deferred unplanned repair,
  and lack of 2+2/two-loss protection on 4+ nodes.
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

Resume the remaining roadmap: 2+2 for four and larger clusters (W74), D36
metadata protection (W66), more metadata groups and distributed ownership
(W66/W98/W81), then broader background-work efficiency and integrated scale
acceptance. D36 remains the final protection contract; alpha 1.0 is explicitly
the intermediate 2+1 milestone on all three topology classes.
