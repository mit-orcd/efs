# Phase 1 — alpha 1.0: finish 2+1 on 3, 4 and larger clusters

**User-directed release scope, October 22, 2026. Status: OPEN; no release
candidate or topology acceptance recorded by this checklist.**

Phase 1 finishes and qualifies the existing **two data fragments plus one parity
fragment** architecture at every supported cluster size. Completion is the
**alpha 1.0** milestone. Only afterwards do we continue the remaining
[distributed-cluster roadmap](distributed-cluster-roadmap-20261022.md).
This is release sequencing, not a claim that the final D36 design is implemented.
No tag, merge, push, deployment or data deletion is performed by this document.

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

## A. Freeze a reproducible candidate and support matrix

- [ ] **A1** Choose one devel candidate commit and identify all daemon/client
  binaries and test-harness revisions. Test fixes belong in the candidate;
  do not qualify an undocumented mixture of binaries and scripts.
- [ ] **A2** Record the actual enabled features and transport/storage settings.
  Include RDMA/direct I/O, enabled `.efs` and background packing for the current
  target profile; containers and expiry remain disabled. If any of those choices
  changes, amend this matrix before calling the run alpha acceptance.
- [ ] **A3** Specify supported node-count range. Qualify 3 nodes, 4 nodes and at
  least one concrete larger topology (minimum 5). Record further coverage of
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
- [ ] **E5** Record throughput, operation p50/p95/p99, resource use and per-node
  activity. Acceptance is correctness and bounded operation under the declared
  load, not a promise of linear scaling or an invented performance threshold.

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
| Candidate and profile | NOTRUN | NOTRUN | NOTRUN |
| Flag audit and A/B removal (A6–A10, A12; once per candidate) | NOTRUN | (same) | (same) |
| Reduced-matrix qualification (A11) | NOTRUN | NOTRUN | NOTRUN |
| Placement/capacity and decode | NOTRUN | NOTRUN | NOTRUN |
| POSIX and concurrent I/O | NOTRUN | NOTRUN | NOTRUN |
| Packing/shared-fragment GC | NOTRUN | NOTRUN | NOTRUN |
| Enabled `.efs` under load | NOTRUN | NOTRUN | NOTRUN |
| Full payloads + orderly restart | NOTRUN | NOTRUN | NOTRUN |
| Scale/progress/resource evidence | NOTRUN | NOTRUN | NOTRUN |
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
