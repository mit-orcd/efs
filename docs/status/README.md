# Project status — work queue and evidence

Reviewed and reorganized October 15, 2026. This directory is the work queue and
acceptance index; [backlog](../backlog/README.md) holds detailed work items and
parked ideas. W numbers identify work, not priority. Explicit human direction
takes precedence over dated assignments. Implementation, scoped test PASS and
full acceptance are separate states.

## Start here

| Need | Read |
| --- | --- |
| Ordered work and unresolved source findings | [Work queue](work-queue.md), preserving existing order and recorded verdicts |
| Execution handoff and scope caveats | [Handoff](in-flight.md) |
| Accepted architecture and pending decisions | [Decision register D1–D37](decisions.md), [specification/public-path review](spec-implementation.md) |
| Find a feature, platform or dated test report | [Complete topic catalog](catalog.md) |
| Earlier assignments, chronology and superseded captures | [Status archive](archive/README.md), [project archive](../archive/README.md) |

## Recent evidence and remaining acceptance

[Full code-gate runtime review](make-test-runtime-review-20261023.md):
execution review complete; speedup NOTRUN. Fix the fault-daemon dependency,
then build harnesses incrementally and schedule isolated cases concurrently.
The fast/long split does not reduce full-gate work or qualify parallel safety.

[W155 modify oracle and correlation tooling](w155-oracle-correlation-20261023.md):
**self-tests PASS on nuc; the W148 campaign rerun with it is NOTRUN** (branch
`devel-w155h`, not merged). A failed modify records its syscall stages and
quarantines the file with syscall-boundary candidates. It is never a PASS.
`w155_correlate.py` builds one table per failure and lists logging gaps
G1–G8. The first is that no server line records a request's receipt. The
nuc W148 raw bundles are not lost: their location and sha256 are in the page.

[Intel cluster + xorinox client CPU profile](intel-profile-20261023.md):
**analysis only.** Not CPU-bound (servers 1.1–1.7 of 8 cores, client ≤0.7);
write 155 MiB/s, read 179 MiB/s, tiny 123 files/s. Tiny files spin in 1 ms
BUSY loops on the client's 16-position identity-admission window. Raw perf
data stays on intel1/intel2/xorinox (paths and hashes in the page).

[Intel fresh deploy of `devel-testing`, all POSIX suites](intel-devel-testing-posix-20261023.md):
**PASS at 100% of supported cases** (core 216/216 on intel1 and xorinox,
two-host 68/68, persist 27/27, extras 4/5 + mmap SKIP by W104, expiry 134
checks). No EFS defect; devops `smoke-persist.sh` `.efs` remount bug fixed
(devops `ff9701f`).

[K28 / W151 cluster cap and packed-segment ring](k28-w151-cluster-cap-20261022.md):
**merged into devel (`a2942cb0f`); unit tests and `make test` PASS on
efs1; the live 5-node gate is NOTRUN (parent session).** Cap 16 nodes (not 64:
uint32 node masks, 4 KiB node-table entries); each metadata group keeps three
voters on nodes 1–4; packed segments use the data ring with a remote
linearizable pack-object read on non-voter nodes.

[K25b summary-runner exit status and test TMPDIR](k25b-summary-exit-tmpdir-20261022.md):
**PASS (scoped), merged into devel; R1 closed.**
`make_test_summary.sh` exits 1 on any failure or timeout and 3 (INCOMPLETE)
on NOTRUN, which fixes R1. Tests keep scratch under `$TMPDIR` and remove it on success.
Two consecutive `make test` runs on efs2 passed without scratch growth. One
earlier run hit a `server.sh` start flake, which is unresolved.

[W87/K9 append oracle on efs2](w87-append-oracle-efs2-20261022.md):
**INCOMPLETE: not reproduced, no root cause; W87 OPEN.** 23 exact rounds, 19,597 acknowledged records on
a private 3-daemon TCP buffered cluster under churn/GC and POSIX load; no loss,
duplicate or reorder warm or cold. No fix; bounded rerun NOTRUN.

[K22 / W23 follower memory/lag bound](../../results/measure/20261022-k22-follower-bound/SUMMARY.md):
**merged into devel; unit test (EFS_FAULTS stall
hook) and `make test` PASS on efs3; the plateau measurement on a real cluster
is NOTRUN (parent session).** Two earlier scaled runs on efs3 are
informational only: one found the stalled follower winning an election,
which the capacity gate on campaigning and leadership fixed. Snapshot import
into a stalled follower is NOTRUN.

[K26 leak gate](k26-leak-gate-20261022.md): **`make leak-check` PASS on
efs2 and efs1 (25/25 unit binaries under valgrind memcheck, about 6 min) and
`make test` PASS on both; merged into devel (`b8ef49e58`).** It works without LeakSanitizer, which
fails only where `ptrace_scope` is 2 (ORCD); `make check` runs it. No product
leak found. Open: W126 peeks can wait behind a writer queued on `view_rw`.
node9901 run NOTRUN.

[K30 intermittent tests](k30-intermittent-tests-20261023.md): **fixed on
`devel-k30`, not merged; 0/25 FAIL per case on amd.** `test_reclaim` was a
product bug (same-second journal aside rename refused startup); the fork
oracle and `test_kv_lsm` were test races. W126 peek limitation stays open.

[K15 W52/W64 append gates](k15-w52-w64-20261022.md): **W64 INCOMPLETE**.
A leader change left append transactions PREPARED until recovery, so writes
failed with EBUSY. The fix and its regression test are on `devel-k15`, and the
matched failover gate is NOTRUN. Lost-reply cleanup (decide-once retirement by
fence or live-owner abandon) is on `devel-k15b` with unit tests passing; its
live gate is NOTRUN. **W52: shape A in tree; exact 20k gate and A/B NOTRUN.**

[Gate evidence-loss review](devel-integration-review-20261022.md#review-of-the-gate-review-response-2be72f2c2--october-23):
Nuc bundles are reported lost and xorinox raw outputs deleted. Their scoped
PASS labels are historical reported outcomes; independent revalidation is
INCOMPLETE. W148 remains reported FAIL, W155 unresolved; no gate is closed
by missing evidence. Separate prior evidence remains separate. The acceptance
in `2a40e3d54` was reviewed with no new findings; this evidence-policy review
is settled, while the recorded qualification work remains open.

[Oct 23 gate-merge review](devel-integration-review-20261022.md#review-of-the-oct-23-gate-merges-and-recap--october-23):
**Qualification remains OPEN.** Scoped passes stand; W154 protection policy
needs a decision. W155 demonstrates ambiguous visible completion, not proof
of successful durability or rollback. A blanket old-or-new rewrite oracle
is insufficient. K1's post-restart rc 4, loaded errors, raw-evidence losses
and foreign-client discovery limits remain recorded.

[D39 consolidated model](d39-consolidated-model-20261023.md): **MODEL PASS on
branch `devel-d39model`, not merged.** The single authoritative statement of
the accepted D39 rules, executable as `tests/d39_model.py` in `make test`:
101 traces from §§8–33, 11 negative controls under superseded rules and a
randomized interleaving search. It adds three model findings the proposal must
accept and lists nine unresolved points (C13). Implementation and cluster
gates NOTRUN.

[D39 latest review (§34)](d39-cluster-membership-proposal-20261022.md):
`802416d39` resolves §32's predicate, critical-section scheduling and Raft
ownership concerns; accepted with no new findings. Next: the consolidated
executable model and the recorded gates. D39 proposed, W153a held; model
and implementation gates NOTRUN. Documentation agreement is not protocol PASS.

[D39 proposal: configured cluster membership](d39-cluster-membership-proposal-20261022.md):
**PROPOSED, awaiting the user.** One member list for every daemon and
`raft-mkfs`, hashed into HELLO and the committed MKFS, replacing `--join`,
`--node-id` and `EFS_MD_RAFT_N` (option B). **Review in §6: bootstrap direction
supported; peer admission, both-group formation and online transitions need
specification.** User decisions are scope, identity selector and initial
formation policy; W153a remains held. **Oct 23 response review (§8): reuse
the existing declaration gate; specify pre/post-MKFS admission and partial
formation before implementing salt+digest identity.** Protocol gates NOTRUN;
no code changed. **§10 review of `8e8a8e4c0`: formed-peer admission can
strand lagging voters; local group installation is not global formation;
formation management needs an explicit role/path.** §12 reviews the new
§11 table: preserve configured voters during catch-up, cover both connection
directions and recovery elections; declared endpoints do not authenticate
peers. D39 remains proposed; W153a held.

[W153 startup review](devel-integration-review-20261022.md#review-of-a76b63f42--w153-is-an-alpha-correctness-gate):
**OPEN, alpha 1.0 blocker.** Unset/empty metadata count defaults to four;
three-node deployments can lose their promised single-loss tolerance.
Derive topology from configured membership, validate full input and node
agreement; startup and live gates NOTRUN. T3 remains closed.

[D38 amendment review](devel-integration-review-20261022.md#review-of-the-d38-amendment-at-e328744ad):
**T3 CLOSED for the default layout.** Three nodes: both groups on 1–3;
four or more: groups on 1–3 and 2–4. Roadmap and packed-placement wording
corrected; local quorum arithmetic PASS, live qualification NOTRUN.

[Recent devel integration review](devel-integration-review-20261022.md):
**Third pass at `0eb1511d2`: OPEN.** Original S1–S3 and R1–R3 fixes are
merged. New T1 (P1): append-abandon repeats the session capture/fence race
(K5/K15). T2 (P2): synchronous forced-exit reporting can exceed K1's shutdown
deadline. Reopen K5 and K1; K15 stays INCOMPLETE with a code gap. Focused
local checks PASS; linked transaction test NOTRUN (missing `libefs.a`).
The efs3 merged-suite pass is reported by `af0996a3d`; live gates remain open.

[K1 / W145 R4 whole-shutdown deadline](k01b-r4-shutdown-deadline-20261022.md):
**implemented on `devel-k01b`, merged `9fd38ffb4`; unit, private-daemon and full
`make test` PASS on efs3.** A 50 s deadline covers the handler drain and
teardown phases; T2 remains OPEN for blocking exit-report I/O after that deadline. A stuck worker gives rc 3 with the phase and worker named,
and the stop script reports "stopped, forced, stores preserved", never
orderly. A devops `stop.sh` patch is required and not applied. orcd-mini
`stop.sh` qualification NOTRUN.

[K1 / W145 remaining parts](k01-w145-remaining-20261022.md): **merged into
devel (`86b6ef567`). Near-follower snapshot fixed (a match-0 probe ACK reset
`next_index` to 1); 30,929,219@62 was not acknowledged to any client;
pre-vote ends restart election storms (efs1 cluster: terms +19 before,
+1 after); efsd stops are bounded (handlers close before the next
request, 40 s drain).** Unit tests PASS; orcd-mini gates NOTRUN.
[K16 / W75 identity-bound fragment record](k16-w75-identity-format-20261022.md):
**merged into devel (`e9ca0d07b`); fresh format required.** Fragment record format 2 binds export,
inode, chunk, candidate generation, fragment index and length to the payload;
GET/GC/segment reads refuse unknown formats and mismatched identities; root
format 0 refuses startup. Inode generation and export salt are not bound.
RDMA and hardware gates NOTRUN.

[orcd-mini deploy of `d94ca85bf`](orcd-mini-deploy-d94ca85bf-20261022.md):
**deployed; all functional suites PASS** (core 216/216, extras, durability,
`.efs` 10/10, two-client 67/67, ior hard). **ior easy write down ~40% vs
`08cc16af4` (710–863 vs ~1,330 MiB/s); unattributed, A/B NOTRUN.**

[Packing validate-once review and assessment](review-pack-validate-once-20261022.md):
**merged into devel `842a014b3` by user decision; read mismatch OPEN.** The
mismatched file was 3.3–3.6 MB in every completed version; no demonstrated
packed-read involvement. Overwrite followed by truncate-up is a hypothesis.
Independent review of `a1ceff4cb` identified output-reuse, unchecked
shutdown and mismatch-context gaps in the improved repro; the harness gaps
(checked stop, remote/final mismatch sidecars, immutable captures,
self-tests) are closed and merged (`56b7e48fe`); qualification NOTRUN.

[K4 / W133 declaration gate](../backlog/work-items.md#w133-fix):
**fixed, merged into devel.** Every request except
CLIENT_HELLO and HEARTBEAT needs an accepted declaration of exactly
protocol 3. Unit gates PASS on efs3 over TCP; the RDMA unit path and the
cluster refusal run are NOTRUN.

[Alpha 1.0 plan and code review](alpha-1.0-plan-review-20261022.md):
**the plan's order holds and its K register matches the code; the code caps
a cluster at four nodes, so ">4 nodes" is an implementation item or a
narrower claim.** W145–W147 fixes read correct; `raft-resync` mark is not
persisted; ior write regression needs a K row.

[K31 fixed, K27 closed, orcd-mini on devel `24e58fb1d`](k31-k27-orcd-mini-20261023.md): **RDMA reply loss came from the K5 mount session starting before the daemon fork (poller thread lost); fixed in `24e58fb1d`. K27: no ior write regression on fresh formats. All orcd-mini suites PASS.**

[D40, alpha 1.0 scope decisions Oct 23](decisions.md#d40--alpha-10-scope-decisions-taken-oct-23-user-in-one-pass):
**D39 accepted (fixed membership, endpoint identity, all members at formation; model first); W154 rebuild on return; K10 known limitation; K13 and K14 fixed in alpha; K23 stays deferred; orcd-mini reformat and the K27 A/B approved.**

[Alpha 1.0 Stage 1, Oct 22](alpha-1.0-2plus1-checklist.md#stage-1-tally-end-of-oct-22-adjusted-after-the-integration-review):
**7 K rows fixed and merged (K2, K3, K4, K8, K16, K24, K28); of the 5 rows
REOPENED by the [integration review](devel-integration-review-20261022.md),
their fixes are merged; the third review reopened K1 (T2) and K5 (T1), both fixed and merged Oct 23 (`c083b98b5`, `9a88e332e`); the K15 live gates on intel2 ([evidence](k15-live-gates-20261022.md)) FAILED under a load stall with integrity intact, opening K29; amd `make test` exposed intermittent devel failures, K30; the Oct 23 live gates are merged: [xorinox](gates-xorinox-20261022.md) K28 five-node PASS with finding F1 (W154), K22 scoped PASS, K9 not reproduced in 110 rounds; [nuc](gates-nuc-20261022.md) K26 x86_64 PASS, K5 fence gate PASS, K1 stop PASS, W148 signature 0 of 9 (NOT REPRODUCED on TCP) but the gate FAILS on ambiguous-completion EIO/EBUSY (W155); the nuc raw bundles were lost with the session; both confirmed by the [parent session's response](devel-integration-review-20261022.md#response-to-the-third-pass-parent-session-devel-62d30994f) (T3, the D38 placement question, closed by amending D38: two three-voter groups on nodes 1–4, K28's layout stands),
with T1 also affecting K15; the K26 valgrind leak gate is merged;
2 INCOMPLETE (K9, K15); live gates NOTRUN. devel `0eb1511d2` returned `make test`
rc 0 on efs3 with 15 NOTRUN: executed checks passed, full qualification INCOMPLETE.** Highlights: W148 wrong-bytes root
cause fixed (UNCOND slot base), sessions registered on every mutation
(protocol 4), identity-bound fragment format 2 (fresh format needed),
follower memory/lag bound, pre-vote, bounded stops.

[Phase 1 / alpha 1.0 checklist](alpha-1.0-2plus1-checklist.md): **OPEN**.
Finish and qualify **2+1 on 3, 4 and >4 physical nodes** before the remaining
roadmap. **Order: (1) known bugs, register K1–K26; (2) functional
completeness, A/S/B/C/D/E; (3) performance, P.** Candidate-specific gates cover placement, enabled I/O, packing/GC,
`.efs`, full payloads and orderly restart. **A6–A12 also require a flag/mode
audit, removal of competing A/B implementations, built-in policies and a reduced
qualification matrix.** 2+2, many-group distribution and
full D36 protection remain later milestones.

[Distributed EFS cluster roadmap](distributed-cluster-roadmap-20261022.md):
3-node 2+1, 4+ node 2+2 target diagrams; current codec/two-group gaps, D36
metadata protection, distributed background work and staged acceptance. Roadmap
only; no implementation or protection gate closed.

[ORCD tiny-file CPU profile analysis](orcd-tiny-profile-20261022.md):
`08cc16af4f77`, four 30-second captures; repeated whole-segment validation
confirmed in packing, plus abandonment scans, `.efs` indexing and LSM maintenance.
Validate-once and bounded abandon scan merged into devel (`842a014b3`; unit,
node9900 private-cluster and `make test` gates); orcd-mini CPU A/B NOTRUN.

[orcd-mini deploy of `08cc16af4`](orcd-mini-deploy-08cc16af4-20261022.md):
**cleanup merge deployed; all suites and gates PASS over RDMA + `.efs`**
after two harness fixes (`e4bd1b6d7`, merged in `99c7a1c75`): faults 24/24,
`.efs` commands gate PASS.

[orcd-mini fresh-format deploy with D25](orcd-mini-d25-deploy-20261022.md):
**deployed `ee1087129`; all suites and D25 step-5 gates PASS.** Fresh
format (user-authorized wipe). Core 216/216, extras 4/5 + mmap skip,
durability 27/27, two-client 67/67, ior rc 0, GC RDMA 71 checks, faults
24/24, D25 gates 0 FAIL. make test fails only on environmental issues.
Fixed on the way: `.efs` EBUSY for a minute after every restart
(`ef7b1735d`, `ff93f8e86`) and two `make_test_summary.sh` bugs.

[Dead-code audit and independent review](../reviews/dead-code-review-20261021.md):
44 audit entries at `0bdf55256`; targeted review confirms fail-open apply and
protocol paths, but corrects PROTO classification, gate ordering and deletion
scope. Follow-up review of `36d612b4d` accepts the substantive corrections. No product fixes
implemented; Linux results are attributed to the audit author. The tooling
fixes landed in `040734d26`. The findings now have work items:
[W127–W134](../backlog/work-items.md#w127) (apply, protocol and session
correctness, all OPEN) and [W135–W139](../backlog/work-items.md#w135)
(test-suite defects and qualification gaps, all OPEN). The [scope/priority review](../backlog/work-items.md#w127-w139-review) distinguishes confirmed unsafe code, W131 defensive hardening
and test evidence gaps; W127–W131 form one implementation package, not five
independent error-handling designs. No acceptance gates are waived.

[D25 logical truncate protocol](d25-logical-truncate-design-20261021.md):
**implemented through step 5 on branch `d25-logical-truncate`; all D25 gates PASS on a private node9901 cluster.** One 86-byte inode-group
TRUNCATE entry commits size, epoch and a pruned inode fence history, with
success-only opid receipts; reads mask by the inode history; lanes adopt
the epoch lazily behind an ACTIVATE/BOOTSTRAP floor; a bounded lane-local
sweep deletes or clips superseded rows, and SWEEP_DONE/RETIRE free history.
Final review's corrections are folded in: one `adopt` transition resets lane
EOF, receipt capacity is checked before commit (BUSY, never unprotected),
history revision persists, g4/g5 gate acceptance. Both call-site audits
are done (§8). [Step 2](d25-step2-gates-20261021.md) (`3e133c257`): inode-authority
views, lane adoption, BOOTSTRAP floor; g4/g5 step-2 form PASS on node9901, and a
pre-existing peer read of truncated data (fails on 47ba6b590) is fixed.
[Step 3](d25-step3-gates-20261021.md) (lazy 86-byte TRUNCATE, off by default): g0-g4
PASS on node9901, sparse 100 GiB O_TRUNC 0.024 s, after fixing a pre-existing
stale-cache republish the eager baseline 47ba6b590 still fails (UNCOND images now
carry their formation epoch). [Step 4](d25-step4-gates-20261022.md): lane-cursor sweep,
SWEEP_DONE/RETIRE, base clips, masked packing; g5-g8 PASS (2 GiB O_TRUNC 0.020 s, sweep
9 s, space returned; history bound, restart mid-sweep, packed files).
[Step 5](d25-step5-gates-20261022.md): the eager path, LANE_FENCE, tail stubs, the
client tail rewrite and the switch are removed; logical truncate is the only path, and
every D25 gate (g0-g8, step-2 g4/g5, the repro) passes again on node9901, and after
merging devel on node9900 over TCP and RDMA. Multi-host placement and the sweep
scheduler's both-groups-local assumption remain.
Earlier reviews and responses are kept as superseded history.
Replaces the declined 256 MiB EBUSY refusal; W43 stays open.

[orcd-test tiny-files slow-client profile](orcd-test-tiny-files-profile-20261022.md):
**explained, no fix.** The 2× per-client gap in the 7 × 10M tiny-files run is metadata-group
placement: inode parity put 4 client trees in group 2 and 3 in group 0, and one node
led both groups. Commit-to-apply latency on that leader is the limit, not server CPU. Clean A/B on fresh formats: spreading leaders (`EFS_METADATA_BALANCE=2`) is 27% slower, because routing sends both groups' requests to the dual-group node; colocated leaders give a 1.07× gap. New:
a group-2 follower received no appends for 30 minutes after a snapshot-time
reconnect, until a snapshot install caught it up.

[orcd-test devel suites over TCP and RDMA](orcd-test-devel-suites-20261022.md):
**PASS** on devel `08cc16af4`, `.efs` on, both transports: POSIX all 218/221 (3 SKIP by
design/budget) on 7 clients, persist across a full restart and transport switch,
two-client 67/67, every live `.efs` harness, IOR little/wide easy/hard with
verification. Four harness bugs fixed on `claude/orcd-test-suites`; W121 reproduced
(still OPEN, kernel).

[orcd-test cleanup and W127–W131 run](orcd-test-cleanup-w127-20261022.md):
**PASS** on a reformatted orcd-test (RDMA, `.efs`): POSIX core 216/216 on 7
concurrent clients twice, the live `.efs` harnesses, a restart on existing
data. Branch `claude/dead-code-cleanup-5f7087`, not merged.

[W66 acceptance checklist](metadata-scaling.md#w66-acceptance-checklist-consolidated-oct-21)
(distribution, fail-closed hardening, MKDIR/born-HASHED, conditional split
rediscovery; B1, B2 and B4 PARTIAL and B3 PASS on the cleanup branch (W127–W131), the rest NOTRUN, directory-spread policy OPEN-DECISION; no backward
compatibility: B5/B8 and legacy parts dropped) consolidates the
review thread below. Final review accepts this policy: group B can proceed;
A7 is independent. Existing scoped restart/reformat authorization persists,
with no automatic formatting on store refusal or redundant approval gate.
[W66 single-leader concentration review](metadata-scaling.md#single-leader-concentration-review--oct-21):
**analysis only, NOTRUN.** Two fixed metadata groups mean at most two
leaders; extra nodes do not automatically receive groups under the default
placement. Follower reads exist, but
routing prefers leaders. Next: a decision page separating replica, leader,
read-routing and execution placement; measure per-group traffic and blocked
time first; transaction locality and D36's replica count gate more groups.
The no-backward-compatibility policy supersedes the historical legacy replay
and migration proposals. Use one current format and fresh-format cutovers;
keep fail-closed refusal and current-format restart/replay/snapshot tests.
Do not build split rediscovery for discarded stores: retain those gates only
if the chosen current design can still create SPLITTING directories.
Earlier measurements and review findings remain evidence, not legacy-support
requirements. Implementation and runtime qualification remain NOTRUN.

**Raw evidence moved off devel (2026-10-21).** 23,126 raw files (logs, TSV,
JSON, fixture stores) under `results/`, `docs/status/evidence/` and seven
data files under `docs/` now live only at tag `archive/raw-evidence-20261021`
(branch `raw-evidence`). Devel keeps the 302 summaries under `results/`; 523
links in 96 pages now name the archived path. Read one with
`git show archive/raw-evidence-20261021:<path>`.

[orcd-mini profile on `08cc16af4`](orcd-mini-profile-08cc16af4-20261022.md):
**profiling only.**
- Two tiny-file writers: ~3,000 creates/s, limited by the leader's `h->mu`
  and apply lag (known plan).
- W126's LSM fix holds: 1.4% lock waits.
- New [W143](../backlog/work-items.md#w143): efs-fuse spent ~4% of its CPU
  in `getenv` on per-op paths. Fixed, deployed in `46496416e`, verified and closed.
- New [W144](../backlog/work-items.md#w144): with split leadership,
  LOOKUP/GETATTR at a group follower took `h->mu` for the leader hint (22%
  of lock waits). Fixed, deployed in `784bcc6ee`, verified and closed.
- Observation: at ~16 M files, leader terms move under load (group 2:
  58 → 63 in 15 min).
- New [W145](../backlog/work-items.md#w145), OPEN, Raft safety: after an
  InstallSnapshot, the deposed group-2 leader n2 committed its own
  unreplicated entry (30929219@62 against the cluster's 63) and wedged.
  Fixed in `76614e8e8` with a regression test and deployed.
- n2 repaired 16:20Z with the new `efs-mgmt raft-resync`, after closing two
  more bugs found on the way:
  - [W146](../backlog/work-items.md#w146): snapshot images over 4 GiB could
    not be installed;
  - [W147](../backlog/work-items.md#w147): a transfer restarted on every
    send while the leader committed.
- All nodes are in sync and POSIX smoke passes 216/216. orcd-mini runs
  `a569de971`.

[W126 on orcd-mini, `8fb4247ad` A/B](orcd-mini-w126-ab-8fb4247ad-20261022.md):
**deployed. Lock-wait gate PASS; create-rate gate FAIL.**
- Read-only LSM peeks take `mu` only, and flush file I/O yields the
  exclusive view.
- Leader LSM lock waits are 2–4× lower at matched points (A/B/A').
- Creates/s followed the run order (736/598/551), not the build. Throughput
  is bounded by `h->mu` and fsync.
- `make test` 813/813 with one environmental failure; POSIX smoke 216/216.
- The user's 32M-file run was stopped at ~5.67 M files for the deploy.

[orcd-mini profile under the 32M-file create run, `47ba6b590`](orcd-mini-profile-47ba6b590-20261021.md):
**profiling only; no PASS/FAIL gate.**
- From ~1.0 to ~2.6 M files, creates fell from 2,564 to 1,130 files/s.
- Leader LSM lock waits rose from 3.3% to 16–18% of its switches. LOOKUP
  reads convoy behind the writer-preferring KV view lock.
  [W126](../backlog/work-items.md#w126) is opened for this; step 1 is
  implemented with a regression test. `make test`, deployment and the live
  A/B are NOTRUN.
- Corrected: the reported 30.6/15.9-core n1 "bursts" were an artifact of
  `/proc` accounting with perf attached (A/B: 6.8 vs 4.5 cores). No work
  item.
- The `.efs` catalog rebuild grew from 21 s to 58 s of each 60 s refresh.
- Directory spreading started; the spread pass itself is cheap.
- `h->mu` waits did not grow (8–13%).
- Errors: only `report-split` `EFS_ERR_BUSY` apply-wait timeouts.

[ORCD-test detached campaign](orcd-test-detached-campaign-20261020.md):
**FAIL / BLOCKED.** 8,437,752 successful-write counts of 40 M target;
117 writer-thread EIO reports, `.efs` command/query failures, post-load
remount discovery failure. Initial core 216/216, peer 67/67 and persistence
27/27 PASS; RDMA and packing exercised. Original evidence retained. Authorized
repair in progress: full retained-payload verification, bounded packing-maintenance
scans, refresh-aware query tests and restart readiness. Candidate `d7ddcc045`
adds reviewed read headroom; isolated tests PASS, not deployed; acceptance open.
Baseline complete: all 8,437,752 paths attempted, 49 EBUSY failures; all 117
uncertain-write payloads match. Repair `643f6104f` deployed on preserved stores
after clean stops, readiness PASS (9.6 s), and all remounts PASS. Full retained
verification from zero running without errors at 1.34 M checked. POSIX core
215/216 (one 15 s mtime timeout); peer 67/67 and focused `.efs` commands PASS.
Options/concurrent-load gates PASS (5,853 queries, zero BUSY; 19,379 payloads
verified). Retained verification at 2.96 M without errors. Isolated mtime
case still exceeds 15 s under read load (80-append diagnostics 9.11–18.46 s,
mtime monotonic). Full retained verification PASS: all 8,437,752 payloads, zero errors, including
all 49 prior EBUSY paths; all 117 uncertain writes match. Unchanged core suite
passed 216/216 after readers finished; loaded timeout remains preserved.
New 40 M-target workload running in a separate fixture (32 writers/host);
new scale/restart gates remain open. At 2.24 M new writes (zero write errors),
concurrent queries under scale fail NODATA after warmup. Diagnostic rerun passed
without a fix; per-node capture confirmed one catalog remained unindexed after
another warmed. Corrected all-serving-node warmup gate is running; concurrent
NODATA still fails. Corrected gate PASS: 2,853 successful queries and all
4,459 test payloads verified. Scale at 5.06 M new files, zero write errors;
40 M target remains open. Original failure preserved. At7.76 M new files,
a new close EIO on643f6104f exhausted the8 s REPORT budget. Its uncertain
256-byte payload matches, but the close remains FAIL. Four new profiles confirm
group2 term5533→5536 in25 s; election-stall root cause remains under investigation.
At8.28 M new files nine closes have failed; eight share the same32 ms window.
All nine uncertain payloads match. Contention captures retained with lost-sample
and symbolization limitations. At8.80 M new files, ten closes have failed;
all ten uncertain payloads match,118 writers continue. Synchronous memtable
flushes remain a candidate stall source; next orderly restart will enable
existing Raft/flush timing diagnostics. At9.28 M new files,12 failed closes
(all uncertain payloads match). Full new-fixture verification is queued behind
all writer completions; final manifests/errors are preserved. Verification jobs
expire15:14 UTC, before daemons16:13 UTC; monitor/resume checkpoints as needed.
At10.58 M new files,18 closes failed with the same REPORT deadline; all18
uncertain payloads match,110 writers continue. At11.00 M new files,25 closes
have failed; all25 uncertain payloads match and103 writers continue.
Qualification remains FAIL. Five-hour run completed at10:44 UTC with
11,840,878/40 M acknowledged files and45 failed closes; all45 uncertain payloads
match. Full new-fixture verification now runs on all four hosts using frozen
final manifests, deadline15:14:33 UTC. Cold restart and stall repair remain open.
At12:28 UTC5.22 M verified with zero errors. Reviewed new partial-overwrite
and splitting-READDIR fixes4058d8141/f8b7a7818 for the next build; not deployed.

[W124 — fast delete](../backlog/fast-delete-reclaim.md): **parts 1–2
implemented and deployed on orcd-mini (`de0c856b4`);**
[evidence](w124-reclaim-20261018.md).
- Batched journal-first node reclaim replaces the per-inode reaper (removed,
  no switch); quota is credited at member acceptance and settled exactly
  at deletion; journal loss fails closed.
- orcd-mini drained a 5.58 M-inode backlog per node in ~18 min (old
  reaper: 48–57 fragments/s); usage after a cold restart equals the
  running count on all three nodes.
- Unit, `make test`, private GC/expiry/SIGKILL gates and all orcd-mini
  POSIX suites PASS; see the evidence page for the mixed-workload test.
- Slot reuse and subtree delete stay deferred.

[Readdir misses names at the directory-split frontier](readdir-split-frontier-20261021.md):
**PASS on a private cluster; C7 rerun on orcd-mini NOTRUN (stopped at the user's request; design roadblock: directories must always be spread, not co-located then migrated).** A listing of a
directory being split (>65,536 entries) scanned hashed lanes before the
local range, so names the migrator moved between pages were lost. The local
range is now listed first. 500 k-file reproducer: 0 missing in 12 of 12
listings (was 3–8 per listing).

[W125 closure tests C0–C8](w125-closure-20261021.md): **STOPPED; C1
rerun 2, C9 and C10 NOTRUN** at the user's request (design roadblock:
directories must always be spread, not co-located then migrated). **No
data errors from packing.** C0 (2 h, both clients), C2, C3, C4, C5 and
C6 (backfill on upgrade) PASS; three packing defects were fixed on devel,
each with a rerun (idle compaction batch, segment quota reserve,
grown-file fold).
- Packed md5 crawl is 14% faster than unpacked. Client RSS plateaus at
  about 1.06 GB.
- Defects outside packing: large-file EIO under concurrent append (0a) and
  readdir skipping entries at 1M entries.
- The C1 rerun found a pre-existing client defect: a small overwrite
  after chunk-map eviction published zeros plus the overwrite as the whole
  chunk. Fixed in `4058d8141`, and orcd-mini now runs `4058d81416a0`.
- Not run: C1 rerun 2 (stopped), C9 24 h soak (cancelled), C10.

[0a: lane epoch behind the inode](w0a-lane-epoch-lag-20261021.md):
**fix PASS on a private node9901 cluster; orcd-mini C1 rerun NOTRUN
(stopped at the user's request).** The C1 large-file EIO came from a
cross-group lane stamp lagging the inode's epoch, so every publish failed
the authority check until D27 stalled it. Fixed builds: 0 stalls and 0
publish EIO in 3 runs of the C1 workload, about 64 min (unfixed: 2 stalls,
11 EIO in 15 min).

[W125 packer throughput and busy budget](w125-packer-throughput-20261020.md):
**gates PASS; copy throughput ~1,020 → ~5,000 files/s; lag-controlled busy
budget bounds the backlog but costs a saturating writer 37% (FAIL vs
foreground target); policy decision open.** orcd-mini runs `445b8b549c82`.
- ior on a fresh orcd-mini matches W124/pre-change builds; one ior-hard
  fsync failure is open item 0a (STALE replay), not packing.

[W125 on devel: merge, full regression and busy-rate default](w125-devel-20261019.md):
**PASS.** W125 merged into devel (`1b2c3b638`); devel `27c20cd4b` passes
`make test`, crash points, two-client, durability, GC, expiry 6/6, landing
8/8 and the P5 matrix; orcd-mini runs it (smoke 216/216, two-client 67/67,
persist 27/27, live packing PASS).
- Busy rate 50 files/s by default: packing during the workload −0.2%
  (16 workers) and +0.4% (one writer) against base.
- Stale uncommitted Raft/KV edits saved unmerged on `raft-rotate-wip`.

[W125 on orcd-mini: fresh deploy of `e6771854e`](w125-orcd-mini-deploy-e6771854e-20261019.md):
**PASS.** orcd-mini wiped and redeployed with `w125-packing`; smoke
216/216, XFS comparison, extras 0 fail, persist 27/27, two-client 67/67.
- 20,000 small files: 1,337 MB → 78 MB per node, verified from the other
  client, fully reclaimed after delete. User workload profile not run.

[W125 P7: node9901 qualification](w125-p7-node9901-20261019.md):
**crash injection, two-client, durability, GC and expiry PASS;
one-writer −10.9% while packing.**
- Found and fixed: segments placed on non-voters in 4-node clusters.
- Found and fixed: fsync EIO when a node restarted inside the client's
  down cooldown (COMMIT now reprobes; latent in base). Expiry gate with
  the packer active 8/8 PASS (8 of 13 before). orcd-mini: see above.

[W125 P6: landing checks and POSIX core](w125-p6-20261019.md): **PASS.**
- Behavioural landing checks 7/7 (packing, retirement, large and empty
  files, repack after modification, backfill, no switch).
- POSIX core 216/216 in three runs with the packer packing during the
  suite.

[W125 P5: modifying packed files](w125-p5-20261019.md): **matrix PASS.**
- Overwrite, mid-file write, append, shrink, truncate to zero, extend and
  shrink-then-extend all read back correctly, are repacked, and survive a
  remount; files grown past 16 KiB move to per-file fragments.
- Hole punch is EOPNOTSUPP for every efs file. New clients refuse to mount
  daemons that do not accept their protocol (no backward compatibility).

[W125 P3: client read path and handshake](w125-p3-20261019.md):
**packed read-back PASS; old clients on new daemons refused PASS; new
clients on old daemons refused but hang until their deadline.**
- 3,000 packed files (1 B–16 KiB) verified byte for byte, also after a
  remount.
- Base-build client: GETCHUNKS refused and logged, EIO on packed reads,
  writes still work.

[W125 P4: packer budget, compaction and backfill](w125-p4-budget-20261019.md):
**foreground impact while packing PASS; compaction and backfill PASS;
read-back NOTRUN (P3).**
- Packing concurrently: −1.2% (16 workers), −4.3% (one writer), three
  runs each; packer idle within noise.
- 60,000 files packed 45 s after the workload; 152 segments, 10.5 MB per
  node against 4.18 GB.
- 15-of-16 random delete: 10 segments compacted to 3; a base-build store
  is backfilled and packed after upgrade (its MANIFEST becomes KVLSM2).

[W125 P4 early: background packer on a private cluster](w125-p4-early-20261018.md):
**packing works end to end; space PASS; foreground impact while packing
FAILS the 10% target (not tuned yet).**
- 60,000 tiny files packed into 156 segments: 10.6 MB per node against
  4.18 GB (395× less); directories 120,598 → 37 per node.
- Whole-tree delete retires every segment within 20 s, no copying.
- Foreground while packing concurrently: −12% (16 workers), −20% (one
  writer); packer idle (index only) −4 to −5%, one run each.
- Next: rate budget and back-off, repeated runs, compaction, backfill;
  read-back needs P3.

[W125 P1 early: singleton publication cost](w125-p1-early-20261018.md):
**the GO condition FAILS for the archived packing code.**
- Packed singleton close is 3.53 ms against 0.65 ms (single writer).
- Throughput falls 71–78%; 9.9 Raft entries per file against 4.0.
- Space is 19× smaller.
- The user chose background packing after close (2026-10-18); W125's
  design and plan are revised accordingly.

[W125 P0: allocation and cost go/no-go](w125-p0-20261018.md): **conditional
GO for option (a) with singleton segments and merge compaction.**
- A packed singleton costs 8,749 B per node on XFS against 71,241 today
  (8.1× less, measured), and about 175 B per file after merging
  (modelled).
- Seal windows only hurt this workload (one sequential writer per folder).
- Condition: a singleton's publication must cost no more than today's
  close (at 1.5× of it, throughput falls 16%).
- Metadata is +1 Raft entry per file (today about 4.0) unless admission
  and publication are fused.

[W125 — small-file packing by default](../backlog/small-file-packing-default.md):
**proposed; revised 2026-10-18 to background packing after close.**
- Files are written exactly as today (close and fsync unchanged). An
  always-on background packer moves closed files below 16 KiB into shared
  segments of ordinary fragments, lifetime-grouped by publication group,
  directory subtree and creation epoch; no data in metadata rows.
- Their per-file fragments are retired through W124's batched journal,
  extended to superseded generations.
- The same copy-forward engine compacts mostly-dead segments (overhead
  ≤1/t). Existing small files are backfilled.
- [Implementation plan](../backlog/small-file-packing-plan.md): P0
  decisions, port P1–P3 (format, storage node, read path and client
  handshake), packer P4, packed-file modification P5, landing checks P6,
  qualification and landing P7.
- Replaces the current ~800× raw amplification for sub-KiB files (298 GiB
  per node on orcd-mini for ~1–1.5 GB of data).
- One write path and no switch that disables the packer; the prior state
  is preserved as a tag before it lands. All gates NOTRUN.
- P1–P3, P5, P6 and most of P4 (packer, budget, compaction, backfill)
  done on branch `w125-packing`, `make test` PASS on node9901,
  private-cluster checks PASS; P7 (qualification, orcd-mini with the
  user's go-ahead, landing) open; not merged or deployed.

[orcd-mini deletes, GC and crawl on `5d673efa8`](orcd-mini-gc-delete-profile-5d673efa8-20261018.md):
**profiling only.**
- GC reclaim is schedule-bound: 0.18–0.21 GiB/min per node, one pass per
  second capped at 32 reap markers per group, the GC thread idle about
  2/3 of the time.
- A read-only open/close costs ~2 Raft commits (lease edges).
- The crawl runs at 49.0 K stat ops/s, CPU-bound in the LOOKUP KV read
  (`kv_mtab_pos`).

[orcd-mini profile on `5d673efa8`](orcd-mini-profile-5d673efa8-20261018.md):
**profiling only; no new hot path.**
- Leader lock waits are 11.6% of its switches (15.6% on `9099380f3`).
- The cover-pick and verdict waits stay gone. What remains is proposals
  (44% of lock waits), `host_pump` (11%) and REPORT (7%).
- CPU: LOOKUP/GETATTR KV reads (`lsm_get` 7–8%, `kv_mtab_pos` 3–4%), and
  the 60 s catalog refresh (8% while it runs).
- The test's writer filled orcd-mini's quota. The tree is deleted; GC is
  reclaiming at ~0.23 GiB/min per node.

[orcd-mini deploy `5d673efa8`](orcd-mini-deploy-5d673efa8-20261018.md):
**deployed from branch `raft-host-mu`; every suite PASS, private
`posix_expiry` 6/6 (was 4/6).**
- `h->mu` steps 1–3, plus a fix for creates that failed fsync with EIO for
  up to 30 s after a storage member restarted (client and server connect
  cooldowns).
- User's tiny-file workload: writer +10%, efsd CPU −6%, context switches
  −17%, reader unchanged.
- Open: the intermittent `expiry show` failure (now names its step);
  creates still fail while a member is down (product gap §1.2).

[`h->mu` plan steps 1–3](raft-host-mu-steps-20261017.md): **branch
`raft-host-mu`; private RDMA A/B PASS, `make test` PASS; deployed in
`5d673efa8`.**
- Verdict reads and leader-cover picks no longer wait for `h->mu`
  (79 → 0.2 µs and 113 → 0.1 µs mean).
- Leader lock-wait share fell 16.7% → 12.6%. Creates/s, CPU per create and
  latency are unchanged within noise.

[`h->mu` contention plan and review](raft-host-mu-plan-20261016.md):
**NOTRUN; plan only.** Measure per-phase timing → routing snapshot →
race-free result-ring reads → reassess → conditional REPORT change →
proposal queue only if justified. Routing and the result ring are A/B'd
separately. Rechecked at `d68d9a58a`: the code findings still hold. The
`9099380f3` profile is the live lock-wait baseline: submit 27.9%, REPORT
13.9%, leader-cover picks 8.9%. Per-phase timing is still NOTRUN.

[orcd-mini `e04a80f63`: deferral, expiry fixtures, refused-startup fence](orcd-mini-open-items-e04a80f63-20261017.md):
**deployed; core, extras, two-client, persist and private GC POSIX PASS;
`posix_expiry` 2/4 (intermittent).**
- Cleanup deferral now works live: `deferred_cleanup` climbs, and there
  are no catalog publishes while only GC runs.
- A startup refused before serving no longer fences the roots. orcd-mini
  was fenced by exactly that (quota check after `/data1` filled). It was
  repaired with the user's approval (orderly stop, syncfs, records set
  clean), and the quota lowered to 720 GiB.

[orcd-mini profile on `9099380f3`](orcd-mini-profile-9099380f3-20261017.md):
**profiling only.** Under two writers, a delete and two crawlers, the cv_mu
wake-up waits are gone from the leader's top lock waits (45% on
`f4a373b13`). Lock waits are 15.6% of leader switches, nearly all `h->mu`
on submit, REPORT, retained CREATE and unlink. This is the live step-1
baseline for the [Raft host mutex plan](raft-host-mu-plan-20261016.md).

[orcd-mini deploy `9099380f3`: "not indexed yet"](orcd-mini-deploy-9099380f3-20261017.md):
**deployed; every POSIX suite PASS, no failures.** smoke 216/216, XFS
comparison PASS, extras 0 fail (the `.efs` freshness checks SKIP with "not
indexed yet"), two-client 67/67, persist 27/27, private GC + POSIX gates
TCP and RDMA PASS.
- `.efs` commands on a directory newer than the catalog now exit 75 with
  "not indexed yet" in 34 ms; before, they returned EBUSY after retries.
- All other temp clusters on node9900/node9901 are stopped. Old raw
  captures under `/tmp`, `/data1/efs/prof/ab`, `/data1/efs/work` and
  `/data1/efs/testsuite` were deleted at the user's direction.

[orcd-mini deploy `8ad79fcd5` and full POSIX qualification](orcd-mini-deploy-8ad79fcd5-20261017.md):
**deployed; core POSIX PASS.** smoke 216/216, XFS comparison PASS,
two-client 67/67, persist 27/27, private GC + POSIX TCP and RDMA PASS.
- `posix_extras` fails 2/5: `.efs` stats/find on a just-created directory
  return EBUSY under the 60 s catalog refresh default.
- `posix_expiry` fails in its fixture ("different inode group"),
  identically on the previous release `4aff22c62f9f`.

[Raft log rotation off the metadata path](raft-rotate-stall-20261017.md):
**implementation, unit and private-cluster A/B PASS; orcd-mini live
NOTRUN (needs a deploy).**
- orcd-mini's ~14 s stall on all three daemons was rotation rewriting
  1.5 GB under `d->mu`, called from `save_snap` with the pump's `h->mu`
  held.
- Rotation now runs on its own thread: chunked image copy, unlocked suffix
  copy, short locked swap, logical offsets, 8 MiB write-behind.
- Private RDMA cluster, 128 MiB snapshots:
  - longest `d->mu` hold went from the whole rotation (5.3 s) to 1.1–3.8 ms;
  - longest metadata stall went from 5.46 s to 4.36 s.
- What remains is the snapshot export (3.6–4.7 s here, all three replicas at
  once). That is the next gate.

[GC backlog CPU is query-catalog rebuilds](orcd-mini-gc-query-catalog-20261017.md):
**deployed to orcd-mini (`859b2b9eb320`); all live suites PASS** (smoke
216/216, two-client 67/67, persist 27/27, private GC gate TCP and RDMA).
During a GC-only backlog, efsd CPU fell from about 52% to 3–7% per node.
The cleanup deferral does not take effect under Raft apply buffering (open
defect); the 60 s cap and exact dependencies carry the gain.
- In a passive orcd-mini profile during a GC backlog, 90% of efsd CPU was the
  query-catalog publisher rebuilding an unchanged catalog every ~2.3 s. GC
  itself was about 1–3%.
- Change: reap cleanup of unlinked inodes no longer schedules a refresh.
  Unchanged shards are rescanned only when a shard they actually read changed
  (exact 4096-shard sets). New `EFS_QUERY_REFRESH_MS`; its default changes
  from 2 s to 60 s.
- GC schedule untouched.

[Client staging evictor: O(1) LRU](orcd-mini-stage-evict-20261017.md):
**deployed to both orcd-mini clients (`155695ccd`); PASS under the
user's workload shape:** the node9900 writer held about 1,000 and then
about 1,900 files/s, against about 50 before.
- Once over its 256 MB cap, the old evictor scanned the whole LRU table for
  every 64 evictions, so its cost grew with every inode the client had
  seen. On node9900, creates fell to about 50 files/s, at 207 ms each.
- Now the LRU is ordered: passes are O(batch), and the evictor drains to a
  7/8 watermark.
- In 10-minute runs the new client holds creates flat after warm-up
  (1,319/s at 10 min, against 1,127/s still falling for the old one).
  Client mutex waits fell from 25% to 10%.
- A first-minutes drop also happens with eviction disabled; it is open.
- Separate, pre-existing: a 14 s Raft log rotation on all three daemons
  exhausted the client retry budget and failed one close with EIO.

[Waiter wake-up outside `cv_mu`](orcd-mini-waiter-wake-20261016.md):
**deployed to orcd-mini (`4aff22c62`); smoke 216/216 PASS.**
`host_waiters_wake` decides under `cv_mu` and signals after unlocking.
- Under the user's workload, condvar re-acquire is 6.8% of the leader's lock
  waits (45.1% on `f4a373b13`).
- The leader's lock waits are now ~72% `h->mu`: proposals 29%, verdict
  reads 17%, leader-cover picks 14%.
- Private A/B: 21.4% → 2.2%; creates/s and latency unchanged within noise.

[orcd-mini profile on `f4a373b13`](orcd-mini-profile-f4a373b13-20261016.md):
**profiling only.** Under the user's workload, LOOKUP ReadIndex lock waits
stay at 0.8% (9.1% before the fix). The leader's top lock wait (45%) is woken
waiters re-acquiring `cv_mu` after `host_waiters_wake` signals while holding
it; proposals on `h->mu` come next.

[ORCD-test devel qualification `48644535f`](orcd-test-beta-48644535f-20261016.md):
**deployed; overall FAIL.** RDMA/direct-I/O, .efs enabled, expiry off;
packing/containers removed. Live core POSIX 1,512/1,512, cross-host 67/67,
cold durability 189/189, 109,452 exact mixed cycles and 4,715 queries PASS.
Faults 24/24 PASS; private GC passes on unchanged repeat after a peer-startup
proof race. Follow-up on node9901 fixes the Valgrind KV/query/daemon leaks
(per-thread KV buffer; daemon 5,160 → 0 B), both instrumented deadlines, the
missed `TEST_BINS +=` units and the GC peer proof race; ORCD-test rerun pending. The .efs cleanup-wrapper name mismatch now has a focused PASS
with the committed cleanup-name correction, including physical cleanup.
Annotated RDMA unit rechecks PASS. Kernel W121 and automatic recovery W71
remain open; this is not large-namespace/400-GiB acceptance.

[ReadIndex admission without `h->mu`](orcd-mini-readindex-lock-20261016.md):
**deployed to orcd-mini (`8391950f0`); PASS under the user's tiny-file
workload** (POSIX smoke 216/216). Fresh metadata readers admit themselves from
the published view plus a per-group `ri_begun`, raised before probes leave.
The pump starts requested rounds, so LOOKUP and GETATTR no longer queue on
the Raft host mutex. orcd-mini, 2 runs before and 2 after:
- ReadIndex lock waits 8.4% → 1.4% of efsd context switches;
- node9900 stat p50 2.34 → 1.70–1.82 ms;
- writer 1,420–1,506 → 1,793–1,797 files/s.

The private-cluster A/B agrees. One run after the change had a create p99
spike of 24–27 ms, which is not explained.

[W123 — automatic crash recovery](../backlog/crash-safe-durable-versions.md): **proposed new feature; source review only.** Reuse root barrier watermarks if epoch history, exact write tags and retained file versions prove recovery. No extra fsync-time metadata commit remains a hypothesis; eight-byte/zero-I/O cost claims are unproven. Implementation and acceptance NOTRUN; W71 remains open.

[orcd-mini deployment of `09f8af09c`](orcd-mini-deploy-09f8af09c-20261016.md)
(now running `47ba6b590`: W123-branch reclaim-retirement and fragment-attempt fixes ported; oversized-truncate refusal not merged, D25 logical truncate proposed instead; make test and smoke 216/216 PASS; under a 32M-file create load: core 216/216, extras 4/5 + 1 skip, durability 27/27, two-client 67/67, ior easy/hard rc 0, no user-visible errors)
(daemons `4aff22c62`, clients `155695ccd`; smoke POSIX 216/216 PASS):
**deployed; live POSIX 216/216, two-client 67/67, durability 27/27 PASS.**
`make test` has 4 environmental failures. GC crash-restart and fault-suite
integration FAIL on the known W71 uncertain-recovery fence, the same as the
previous deployment `4d6530dd8`. Includes the RDMA credit fix.

[Crash-path gates under the W71 fence](crash-gate-fence-fixtures-20261016.md):
**PASS (scoped), test-only change.** The fault suite uses a fresh cluster for
each server kill and asserts fsync EIO after restart: 24/24 PASS (RDMA
mlx5_0, direct). The GC gate asserts the EIO after its four-server crash, then
fresh-formats before POSIX and the offline-member section: PASS, with POSIX
216/216 and two-client 67/67. The W71 recovery blocker stays open.

[W122 — effective settings discovery](../backlog/work-items.md#w122): **PROPOSED**
read-only settings catalogs for `efsd` and `efs-fuse`, exposed through management
and the portal with defaults, effective values and startup/runtime scope. The
portal launch-environment draft is partial; backend implementation and acceptance
are NOTRUN. Small-cluster AMD/Intel qualification is sufficient.

[Native LMCache versus NIXL for AMD/NVIDIA GPU transfers](efs-kv-gpu-api-comparison-20261016.md): **API review only; implementation and hardware tests NOTRUN.** Prefer NIXL/UCX data movement plus EFS cache control and a GPU-aware adapter extension. Both current LMCache L2 paths use host buffers.

[LMCache through native NIXL: EFS integration direction](efs-lmcache-nixl-direction-20261016.md): **design only; implementation and performance NOTRUN.** Target native asynchronous EFS transfers; AMD GPU-direct support and shared publication/eviction require qualification.

[ORCD Raft/KV decisions](orcd-raft-kv-20261016.md): mixed correctness **PASS**
(297,651 exact readbacks; 57,935 queries), loaded POSIX **FAIL** (seven
15-second timeouts). Keep Raft batching, 100 ms checkpoint interval, concurrent
KV readers and 32 cache stripes; scan-seek cache benefit remains unproven here.
Copied-lock snapshot test fixtures fixed; 40/40 repeated KV runs PASS.
Orderly restart, 49 cross-client markers, 224 fixed payloads and seven cold
queries PASS; quiet POSIX reruns 21/21 PASS do not close loaded timeouts.
W121 remains open with kernel remediation deferred.

Experimental tiny-file packing and container-storage work was removed from
devel on October 16 (D37); its code, tests and records are kept at tag
`archive/packing-containers-20261016` (branch `packing-containers`).

The linear directory-split migrator (not continued; the user wants
directories always spread instead of migrated) is parked at tag
`archive/split-migrator-linear-20261021`. Its readdir-during-split and
stale-replay siblings are on devel (`f8b7a7818`, `9341e7209`).
Packing is to return as an always-on background packer under
[W125](../backlog/small-file-packing-default.md).

Experimental group A was removed from devel on October 16: the manual repair
experiment, the staged D25 logical resize, data-GC admission, Raft admission
pressure, receiver feedback with early retire and group drain, and
`EFS_SKIP_META_FLUSH`. The last commit with them is tag
`archive/experimental-group-a-20261016` (`d57da8af6`). Their durable numbers
fail closed: Raft tags 45–51/55–57 halt apply and are refused on submit, KV
kinds 36–41 and wire messages 131–150 stay reserved, PREPARE kind 9 is
refused, and daemons refuse roots carrying repair markers
(`tests/test_retired_root_markers.py`). **Removal PASS (scoped):** node9901
`make all tests` clean; `make test` 765 commands with 4 environmental
failures: LeakSanitizer is broken on that host (a trivial leak program fails
too; the three affected tests pass with `detect_leaks=0`), and
`check-architecture` needs the omitted `results/` (5/5 on the checkout).

[RDMA credit overrun](orcd-mini-rdma-credit-20261016.md): **PASS on
orcd-mini under the user's workload** (`09f8af09c`): group-2 commits 229 →
~6,500/s; loaded create/stat p50 71/21 → 6.1/2.7 ms. Next bottleneck: LOOKUP
ReadIndex on the Raft host mutex. DATA plus CREDIT frames could exceed a peer's
posted receives; the RNR retry turned 30 µs request/reply rounds into 2 ms
and Raft commits into 26 ms on orcd-mini. `e84d42cbb` plus the
review fix `bbebedea8` bound each window at half the smaller receive pool
(6 receives by default). Private 3-node RDMA cluster: 16-thread creates
348–388 → 2,529–2,675 files/s; stat p90 2.3–4.5 → 0.3 ms.

[orcd-mini truncate](orcd-mini-truncate-20261016.md): **scoped PASS
(functional).** A staged DEL's garbage vlen wedged one replica's apply
(`apply lane-fence rc=-5` forever; truncate EIO, mkdir/rm EBUSY), fixed in
`502277d19`. Truncating files over ~1 GiB returned EIO (32-chunk lane-fence
cap); `4d6530dd8` pages the fence and reads the TRUNCATE verdict. A 100 GiB
`dd if=/dev/null` now completes in 4.3 s; data/regrow checks PASS. Crash
during paging NOTRUN.

[ORCD queued-read-vote comparison](orcd-queued-read-vote-20261016.md):
**NO—keep reverted.** Frozen `fa29f9277`, native RDMA/direct, `.efs` enabled,
no expiry; OFF–ON–ON–OFF across seven clients. POSIX
failures15/0/10/0;22,182 mounted queries PASS, no repeatable net benefit.
**W121 OPEN, kernel remediation deferred:** [fork/read diagnosis](orcd-w121-fork-read-20261016.md). Correct daemon replies can reach stale application pages on ORCD; the performance harness now isolates query-launch processes. Original evidence: 255 immediate readbacks returned wrong bytes after successful
fsync across both variants. W119 remains open; this is not full acceptance.


[Raft follower batching default](intel-raft-follower-batching-20261016.md):
**scoped PASS (performance)** measured on `3fba43af7` with the opt-in bundle
(`EFS_RAFT_BATCH_OVERLAP=1`, `EFS_RAFT_BATCH_NOOP=1`, 100 ms applied interval),
now the default for both engines: lease edges 25 → 3.7–3.9 ms, AMD small-file
creates 77 → 345/s, busy-follower pump drains 12–16 → 1.3–1.6 ms. Same day:
retained-close fix and hashed open table (`ae3a49d8a`), concurrent LSM reads
(`3fba43af7`). Review found and fixed a nested-read deadlock in the
concurrent reads (`6b5c34624`); merged with the AMD durability branch.
Qualification NOTRUN.

[ORCD fresh-store read/close baseline](orcd-read-close-baseline-20261016.md):
W119 native RDMA/direct baseline on frozen852caf1ae, features disabled,
exclusive after graceful crawler/service stop. Three fresh-store core runs
216/216 PASS (11.3/11.0/11.1s), local peer67/67 PASS; no seven-client run. TCP leg pending scope clarification;
no A/B conclusion or AMD timeout closure.

[AMD asynchronous durability fixes](amd-async-durability-20261015.md):
**durability scoped PASS, loaded POSIX FAIL** on `c4178ea81`, branch
`devel-amd-durability-20261015` from `84ffe7a68`. Early ACKs retained; all-node
COMMIT, boot verifiers and persistent unclean-recovery refusal pass focused
and live RDMA/direct gates. Native, serial216/216, peer67/67 and cold27/27
pass; jobs16 is214/216 (`names_crazy_dirs`, `concurrent_creates_same_dir`
time out at15s). Follow-up `1a009c5ab` retains the memtable-prefix and
REPORT owner-preparation fixes (full native PASS); two quiet jobs16 runs
remain214/216; the restored follow-up is213/216 with an additional mtime
timeout, while peer passes67/67. Measured delay is principally fresh quorum waiting, not
connection checkout. A queued-vote scheduling experiment worsened loaded
results and was reverted. Clean-restart follow-up `b6454a7f8` passes focused
and live RDMA/direct durability tests: mounted writer/peer survive two clean
daemon restarts. Unclean recovery still fences COMMIT indefinitely; verified
recovery and per-file loss tracking remain missing. W71/W119 stay open; no
seven-client run or power-cut claim. Deployed follow-up `e53d20aa1`: core
serial216/216, peer67/67 and cold27/27 PASS; jobs16 remains FAIL215/216
(`names_crazy_dirs`, unchanged15s). Extras1 PASS/4 SKIP. No independent
reviewed commits met the selected merge scope; dependent changes deferred.
Single-case strace/perf follow-up `f62519501` removes the measured512-slot
wake scan; focused safety/sanitizer tests PASS. Isolated names case passes,
but quiet jobs16 remains215/216 then214/216: names still times out at15s.
W119 stays OPEN. Jobs16 correlation now reproduces the names timeout during
create/read/list: LOOKUP averages5.98ms,5.95ms in parent ReadIndex; follower
queued probes average8.89–9.48ms. Open/close dominate the traced case, not
directory enumeration. Follow-up close phases identify append-lock and REPORT
waits (one names close:53ms lock,56ms REPORT). Opportunistic queued-probe
draining and wider append stripes were not retained; the stripe comparison
was confounded by an independent AMD build. Original scheduling/64 stripes
restored; opt-in timing and deadline regressions retained. W119 remains OPEN;
no accepted timeout fix. Restored jobs16 FAIL205/216 (ten timeouts, one
unlink EBUSY); independent Intel-client storage benchmarks prevent claiming
a quiet isolated comparison. User moved the investigation to exclusive NUC: source852caf1ae build and
seven focused units PASS. User subsequently authorized TCP/direct on NUC:
clean four-node baseline core216/216 twice (16.9/21.2s) and peer67/67 PASS; isolated names0.8s
and deep>64 1.5s PASS, original deadlines/features unchanged. NUC RDMA remains
NOTRUN (no device/Soft-RoCE kernel); AMD timeout closure is not claimed.


[Asynchronous PUT acknowledgement](intel-async-durability-20261015.md):
**scoped PASS** on `3edc7b846` (async ACK from `d8af2c325`, concurrent-fsync
COMMIT fix, 200 ms read-mapping TTL with refresh-ahead and close-to-open), now
the only data PUT mode. On Intel/xorinox 2 GiB writes 151 → 166–180 MiB/s,
with fsync 141 → 176–179, four writers 94–101 → 173 (network-bound: 1.5×
erasure-coded bytes on 2.5 GbE); reads 237–255 MiB/s (88–92% of the link).
Two-client peer suite 67/67 PASS. Crash/power-loss qualification NOTRUN.

[AMD ordinary RDMA/direct POSIX](amd-rdma-posix-20261015.md): **FAIL loaded,
scoped functional PASS** on `c8d057391`, branch `devel-amd-posix-20261015`
from devel `761c91c42`. Full native/strict transport, serial216/216, peer67/67,
cold27/27 and paused-follower32MiB pass. One-mount jobs16 is215/216:
unusual-name directories still exceed15s. Fresh-read scheduling/log-lock and
ACK-window fixes are committed; the loaded gate stays open. Queries, expiry
and seven-client load are excluded. This predates
asynchronous PUT acknowledgement (`d8af2c325`) and does not qualify it.

[Grouped fragment durability](intel-cohort-durability-20261015.md) (superseded):
opt-in `EFS_PUT_COHORT=1` in `5cef06ac4`, removed by `d8af2c325`. On Intel with the xorinox
client, four concurrent writers 94–101 → 119–132 MiB/s and single-stream
146–149 → 169–170 MiB/s, with half the device flushes per MiB; local model
2.5–3x. Kept as evidence for why per-fragment barriers limited writes.

[Intel read metadata and read-cache fixes](intel-read-metadata-cache-20261015.md):
**PASS (scoped)** on `b3e93177c138` (compact GETCHUNKS replies) and
`49d721a4f180` (read-cache admission past the speculative budget), Intel
cluster with the xorinox client. Wire bytes per byte read 1.24→1.08 (1 MiB)
and 10.49→1.15 (32 KiB); single-stream 1 MiB 124→218 MiB/s, 32 KiB 12→43 MiB/s.
POSIX/peer/persistence/fault qualification NOTRUN; old clients reject the new
servers, so the AMD Intel-export client is stopped.

[ORCD TCP/direct, `.efs` disabled rerun](orcd-tcp-direct-disabled-20261015.md):
**FAIL** on the same `b3e93177c138` product source:895 PASS/120 FAIL/520 NOTRUN
in the seven-client round, peer61/67, clean stop refused. All23 successfully
prepared persistence cases verify after authorized forced restart; private
TCP POSIX/peer/GC, faults and memory gates pass. Current ORCD profile is
TCP/direct/files with commands and expiry disabled.

[ORCD latest-devel qualification, October 15](orcd-devel-qualification-20261015.md): deployment
PASS on `b3e93177c138`; qualification **FAIL**. Seven-client POSIX, cross-host
peer, enabled commands and clean stop have failures; W119/W120 track follow-up.
Valgrind and the standalone native-RDMA fault suite pass their selected scopes.

[October 15 open-item reconciliation](open-item-reconciliation-20261015.md) identifies superseded
implementation claims, existing closures, deferred work and gates that remain.
It includes current-source checks; it is not new runtime qualification.

These are entry points to exact-source reports, not claims about the currently
running clusters. Newer source commits do not inherit earlier test verdicts.

| Area | Evidence and limits |
| --- | --- |
| Intel/AMD sequential reads | [October 15 read coordination](intel-read-coordination-20261015.md) and [AMD read profile](intel-amd-read-profile-20261015.md) are measurement evidence. [Read metadata and read-cache fixes](intel-read-metadata-cache-20261015.md) record scoped PASS for `b3e93177c138`/`49d721a4f180`; full qualification of those sources remains NOTRUN. |
| Intel write path | [Eight-writer deployment](intel-default-eight-writers-20261015.md), [pressure draining](intel-pressure-drain-20261014.md), [sync batching](intel-sync-batching-20261014.md). Scoped smoke/native results; observations do not establish universal throughput gains. [Grouped durability design](intel-cohort-durability-20261015.md) is a local model only. |
| REPORT / W100 | [Newer-snapshot report](intel-report-newer-snapshot-20261014.md): W100 closed by scoped human acceptance October 14. Older OPEN entries are historical; this does not establish the cause or resolution of the original ORCD EIO. |
| Integrated RDMA source | [Merged devel qualification](devel-rdma-merge-qualification-20261014.md). AMD software-RDMA evidence does not replace ORCD hardware gates. |
| `.efs` functional versus scale work | [Three work packages and feature matrix](three-work-packages-20261013.md), [AMD package 2](amd-package2-query-20261013.md), [ORCD package 1](orcd-package1-20261013.md), [later query fixes](orcd-query-name-ordinals-20261013.md). General 400 GiB acceptance remains recorded as on hold; targeted fixes and marker checks do not close it. |
| Small-cluster follow-ups | [ORCD findings reproducible on AMD/Intel](orcd-small-cluster-followups-20261013.md). Preserve unresolved ordinary-I/O and scale obligations separately. |
| Expiry | [Implementation](expiry-implementation-20261012.md) and [foreground fairness](expiry-foreground-latency-20261013.md) retain scoped evidence. Expiry stays excluded from the three-package profiles. |

The full chronological index is [preserved intact](archive/status-index-before-restructure-20261015.md).
Its old deployment instructions and grouped open counts are historical, not a
live inventory or an instruction to resume paused work.

## Maintaining this index

Keep this page short: link the current item ledger rather than prepending run
narratives. Put exact source/build, feature flags, environment, commands,
PASS/FAIL/NOTRUN, limitations and remaining gates in the evidence page, following
[the project evidence rule](../../AGENTS.md#evidence-and-reproducibility).
Raw captures stay outside Git; existing evidence is not deleted before an
adequate summary and reproduction instructions exist.

Add each new finding under an unused W identity in the [queue](work-queue.md),
with class, state, evidence, next action and acceptance gate. Check the queue,
[work-item details](../backlog/work-items.md) and decision register for collisions.
Keep supporting pages discoverable in the [catalog](catalog.md). Accepted design
is not completed implementation; close only against the named acceptance gates.

Archive only superseded records, preserve their findings and source-specific
verdicts, and update incoming links. Archiving does not close an item, cancel a
gate or authorize deployment. Keep unresolved work linked from its queue/ledger.
