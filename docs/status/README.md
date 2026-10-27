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

[W160 Task J budget calibration](gate-budgets-refit-20261026.md):
**ORCD-first stages finished with baseline failures**, branch
`agent/devel-task-j`, untouched devel `394f2b791`: node9901 508 commands,
5 FAIL, 5 NOTRUN, 769 s; fstor007 compatible-FUSE gate 508 commands, 1 FAIL,
5 NOTRUN, 750 s. Failures are four LSan runtime errors (separate
leak-disabled checks passed), a hostname-dependent wrapper assertion and a
long-TMPDIR cleanup-buffer assertion. Initial fstor007 FUSE prerequisite
failure is retained too. Closed raw manifests and task-owned cleanup are
recorded. Intel run1 on untouched devel4066f74df FAILED: 508 commands,
2 FAIL, 14 NOTRUN, 616 s (K22 fixture leader assumption; K36 packed-segment
recovery assertion, unclassified). Controller stopped before runs2/3; raw
manifest verified and Task J scratch pruned. **Refit INCOMPLETE**, pending
resolution of those baseline failures. The user authorized fixes on this
branch; candidate8f6cd4b5f exposed24 early client EIOs per K22 run despite its
old PASS line, so it is not qualified. A shared L0/RAM leadership-pressure
candidate4c4446e02 and strict zero-error test are queued for qualification
(W166), with a negative control and three repetitions required before calibration.
Budgets/slots/target unchanged; original failures remain failures.

[Wave 15 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 26 (`a24cabefd`; gate at `a24cabefd`)**: Task B's K22/K6 loopback gates ([evidence](k22-k6-loopback-taskb-20261025.md)): `raft-resync` heals a W127-halted replica from the leader's snapshot image (a resyncing peer rejects AppendEntries with match 0, the leader exports an image outside the byte cadence and ships it; an import covering the halted entry clears the halt), a snapshot chunk that cannot be queued backs off one heartbeat instead of re-reading it every pump wake, a DIR step whose stored row the decoder refuses halts the group (W130) instead of being skipped, K22 leader admission at 3/4 of the unapplied bound, with `EFS_FAULT_SNAP_BYTES` and `EFS_FAULT_DIR_CORRUPT_FILE` fault hooks and three new `make test-long` gates (`test_k22_snapshot_import.py`, `test_k22_leader_admission.py`, `test_w130_dir_fault.py`). intel2 `make test-all` (8 jobs): 508 commands in 775 s, 0 FAIL, 14 NOTRUN host limits, PASS with host limits (a first run with the W130 test in the pool: 509 commands, 1 FAIL, that test). **W130 gate:** `test_w130_dir_fault.py` failed once in the wave's first pool run on its own open finding W165 (a create met the damaged row on the serve path before any DIR apply and failed EIO for good; 4 of 5 alone), so it is its own gate, `make test-w130`, until W165 is fixed. **W160 note:** the two K22 gates add 1,340 slot-seconds; the budgeted gate is 1,644 s and the target moved from 990 s to **1,100 s (18 min 20 s)**; they stay in `make test-long` ([D44](decisions.md), user, Oct 26). Task B's findings are queued as W165 (serve-path damaged row) and W166 (close EIO at the flush deadline).

[Wave 14 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 26 (`a81bc8dc0`, fast-forward; gate at the same commit)**: the D4 drain and reclaim branch ([drain](d4-pack-drain-20261025.md), [reclaim](d4-reclaim-20261026.md)): one packer candidate per file per pass, the fragment resend bounded by 2 s inside K41's placement loop, the truncate sweep takes an inode to retire in one pass and waits 2 s for the writer's own publication, an unconditional full image replaces spans of an older content epoch but is still refused by an unseen live span of its own epoch (`988efba3e`: the wave-13 gate's K38 writer EIO was this branch's `acb8a7318` dropping another writer's span; K36's EBUSY did not reproduce); 60-minute run: stored data bounded, drain 72 s, 0 put errors, abandoned uploads 610 → 41. intel1 8-job gate 0 FAIL twice, intel2 `make test-all` (8 jobs): 506 commands in 641 s, 0 FAIL, 14 NOTRUN host limits, PASS with host limits. The W160 target is 16 min 30 s (990 s) for the D4 plateau driver back in `make test-long` and the pack dup-candidate test.

[D4 reclaim lag: abandoned uploads from refused unconditional images](d4-reclaim-20261026.md):
**branch `claude/interesting-mclean-65362a` (devel wave 13 merged at `ed01bb005`), not merged, not pushed; head `988efba3e`: intel1 `make test-all` 8-job pool twice, 506 commands, 0 FAIL, 14 NOTRUN host limits (565 s, 684 s); K36 and K38 5 of 5 each. 60-minute run at `acb8a7318`: PASS on every check, 41 abandoned uploads (was 610 at `66bfc2786`, linear quota growth to 4.2 GiB before).**
The quota growth under the D4 rewrite load came from abandoned uploads, which D31 collects only when the mount's session
ends. Resizing rewrites abandoned their first upload when the REPORT host refused the previous tail image (stale span list,
`66bfc2786`), and when the D25 sweep zeroed a base the writer's own truncate had masked (`acb8a7318`: unconditional publications
replace the committed base, captured records publish on a swept base, sweeps wait 2 s). The wave-13 merge gate found that an
unconditional image could drop another writer's span (K38); narrowed in `988efba3e`. The D4 driver bounds abandoned uploads and
stored data. Open: a live mount's abandoned uploads still wait for its unmount (D31 epoch rotation).

[D4 drain after a rewrite load and packer put errors at a high rewrite rate](d4-pack-drain-20261025.md):
**branch `claude/interesting-mclean-65362a` (F1 follow-ups merged), not merged, not pushed; xorinox `make test-all` 8-job pool at `9670b895b`: 500 commands, 0 FAIL, 13 NOTRUN host limits, 760 s; intel1 60-minute run: drain 54.5 s ≤ 150, 0 put errors (RSS criterion FAIL on one efs-fuse, F1).**
The F1 drain stall was not leases: a small file rewritten across a pack epoch had two candidate keys,
and a segment holding both failed silently every pass (fixed; new `test_pack_dup_candidate.py` red 4 of 4,
green). The D25 truncate sweep retired about 5 inodes a second and its backlog held the index drain
(now one pass per inode, 32 a pass: drain 22–25 s). The packer's fragment resend is bounded by 2 s
(K37 case 1c). `test_d4_plateau.py` is back in `make test-long`. New open finding: reclaim lags a high
rewrite rate (quota 52 % at 60 min on intel1, ENOSPC on amd).

[Wave 13 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 26 (`899c0a2e0`; gate at the same commit)**: K42/W164 ([evidence](e2-clean-stop-busy-20261025.md): a clean unmount repeats certified no-op cleanup waits instead of failing on one BUSY, the stop names its shard and the server's reason; 20 of 20 clean stops), Task D's endurance acceptance ([evidence](task-d-20261025.md): rehearsal PASS on intel2 and nuc, four-hour soak FAIL at 7,195 s on **K43** (blocks alpha), harness fixes for a returning member's not-indexed query answer and the soak tables), and the efsd RSS plateau work ([evidence](efsd-rss-plateau-20261025.md): idle connection reaping in both pools, the reclaim journal ring releases pages, segment reads and compaction in one mapping; plateau PASS on one 60-minute xorinox run). intel2 `make test-all` (8 jobs): 504 commands in 632 s, 0 FAIL, 14 NOTRUN host limits, PASS with host limits. **Held back:** the D4 drain and reclaim branch `claude/interesting-mclean-65362a` (`eec27ed50`; its evidence pages `d4-pack-drain-20261025.md` and `d4-reclaim-20261026.md` live on that branch): with it merged the same gate failed twice with different writers' errors under the 8-job pool (`test_k38_shared_chunk_pressure.py` writer `EIO`, then `test_k36_recovery.py` step 8 `mkdir` `EBUSY` after a member kill), both passing alone; without it 0 FAIL. Its publish and truncate-sweep changes are to be re-gated on their own branch (intel2 `~/efs-artifacts/merge-wave13-20261026/held-back/`).

[Task D: seeded member-churn endurance](task-d-20261025.md):
**four-hour soak FAIL on EFS finding K43; branch
`claude/stoic-goldwasser-06dbca`, not merged or pushed.** devel `39a1cbbbc`
with `agent/devel-task-d` merged and one harness fix (`b2fc06ba5`: a
returning member's documented "not indexed yet" `.efs` answer is waited out
under the recovery bound). Rehearsal PASS on intel2 (`4cb79d09d`, 1,102 s)
and nuc (`b2fc06ba5`, 1,166 s). The four-hour soak on nuc stopped at 7,195 s,
at its 15th event: SIGKILL of the group-0 leader that coordinated an
in-flight REPORT left its PREPARE intents until recovery aborted them, the
client's resend lost its CAS against them and the writer got `EBUSY`
(**K43, blocks alpha**). 14 events and seven exact two-mount verifications
passed before it; the retained stores read all 512 acknowledged versions and
3,379 appends exact; every qualifying RSS window PASS, role coverage NOTRUN.
Earlier attempts: intel2 interrupted at 2,768 s and nuc attempt08 at 9 m 52 s
by other sessions' builds (environmental). nuc `make test-all` (8 jobs): candidate `08c949ea0` 502
commands, 2 FAIL, 15 NOTRUN; base `39a1cbbbc` 502, 4 FAIL, 15 NOTRUN; both
candidate failures (`test_k41_packer_placement`, `test_k38_shared_chunk_pressure`)
also fail on the base: no candidate-only failure. Raw: nuc
`~/efs-artifacts/task-d-20261025-nuc2/`, intel2
`~/efs-artifacts/task-d-20261025-intel2/`.

[Wave 12 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 25 (`78b9e0981`; gate at `78b9e0981`)**: K41/W163 ([evidence](k41-packer-placement-20261025.md): bounded parallel heartbeat probes with the streak and down mark kept per node id across table rebuilds, the packer probes its three destinations and replaces a failed one with the next live ring member before writing, a packed read that reached a holder but gathered fewer than two fragments is `EIO` naming the segment; intel2 gate 0 FAIL, `test_k33_growth.py` 10 of 10) and Task C's last test-hygiene commits (K37 degraded-write events by identity, the refusal test checks every replica's log for the refused tag, K33 payload fixtures kept on failure). intel2 `make test-all` (8 jobs): 502 commands in 657 s, 0 FAIL, 15 NOTRUN host limits, PASS with host limits.

[Wave 11 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 25 (`d3051f273`; gate at `d3051f273`)**: K33/W157 join retry ([evidence](k33-join-retry-20261025.md): a first start killed between `boot.bin` and `identity.bin`, or in the O_DIRECT probe, is retried with the same command; a store that started before and lost its identity stays refused; intel1 gate 0 FAIL, A13 PASS with the kill before the commit) and the Task C/D agents' latest evidence commits. intel2 `make test-all` (8 jobs): 499 commands in 545 s, 0 FAIL, 15 NOTRUN host limits, PASS with host limits.

[Wave 10 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 25 (`358ebc378`; gate at `358ebc378`)**: the F1 follow-ups ([evidence](f1-followups-20261025.md): K36 under TSan 10/10, the W77 publication-unit ABORT after a refused forwarded PREPARE, packer put errors 3 → 0 on the 20-minute D4 load, glibc arena cap and fixed mmap threshold with a periodic trim, crash hooks exit through `exit_group`; the efsd RSS plateau stays **OPEN**, 8–13 % growth late in 60-minute runs, and `test_d4_plateau.py` is a measurement driver outside `make test-long`), Task C's remaining decoder commits (frag-record digest kept on refusal, malformed error envelopes rejected) and Task D's endurance-harness commits (RSS qualified only in steady write-load windows, cleanup resume, writer stop before a failed mount transition). intel2 `make test-all` (8 jobs): 498 commands in 647 s, 0 FAIL, 15 NOTRUN host limits (the extra one is test_soak_driver's own 'rehearsal NOTRUN' marker), PASS with host limits.

[K33/W157: an interrupted join is retried](k33-join-retry-20261025.md): **PASS (loopback), branch `claude/sweet-allen-560f74` on devel `1fc526f66` (code `d9186e38a`, `bfd0abdf6`), not merged, not pushed; intel1 `make test-all` at `bfd0abdf6` (8 jobs): 499 commands, 0 FAIL, 14 NOTRUN host limits (two earlier runs had one known intermittent each: K38 shared-chunk EIO, B6 refuse); A13 on `loopback-3` PASS with the kill before the commit (`a13_interrupt_s` 0.05) and at 0.1 s (after the commit). Raw: intel1 `~/efs-artifacts/k33-join-retry-20261025/` (`SHA256SUMS` `cc1ec97b…7963`).** Task A's Candidate A: a first start killed between `boot.bin` and `identity.bin`, or during the storage root's O_DIRECT probe, was refused on every retry. A store whose boot counter is still 0 (its token never left the process) now initializes again, and a new root's leftover probe page is removed; a lost identity, a wiped store at a member's endpoint and unknown root data stay refused. Deterministic test `tests/test_k33_join_retry.py` (fault points `before-identity`, `before-hello`). Live multi-host A13 stays NOTRUN.

[K41: packer placement on a member already known down](k41-packer-placement-20261025.md): **FIXED (scoped), branch `k41-packer-placement` on devel `1fc526f66` (`531c7c783`), not merged, not pushed; intel2 `make test-all` at 8 jobs: 499 commands, 0 FAIL, 14 NOTRUN host limits (656 s).** Serial heartbeats with 30 s timeouts let a silent joiner stall liveness detection, so the group leader never marked a stopped member down and its packer wrote a segment degraded onto it. Heartbeats now run in parallel under a 2 s deadline with per-node-id marks; the packer also uses peer-pool and Raft voter liveness, probes each destination and replaces a failed or refusing one by a live spare; a short packed read is `EIO` naming the segment. `test_k41_packer_placement.py` fails on the base and passes; growth test 10 of 10 PASS (2 spread). Live, RDMA and orcd-mini NOTRUN.

[K42: clean unmount BUSY in revoke cleanup](e2-clean-stop-busy-20261025.md): **FIXED, branch `claude/great-curie-e4cfb1` on devel `1fc526f66` (code `e62cc1d07` … `b9d5f9443`), not merged, not pushed.** Task A's candidate D was reproduced on amd on the first loaded run. Cause: cleanup steps that committed nothing (the peer's append cleanup meeting another client's in-flight append or its refused retirement transaction; the lease batch meeting its close transaction) reached the client as a bare BUSY that it never retries. The server now names such refusals and the client retries them within 20 s, or fails closed with the shard and reason. Intel2 at `b9d5f9443`: 20 of 20 clean stops of a loaded peer (68-case two-client suite before each); `make test-all` at 8 jobs 499 commands, 0 FAIL, 14 NOTRUN host limits (amd, where it was reproduced, went offline before the final runs). Raw: intel2 and amd `~/efs-artifacts/e2-clean-stop-20261025/`.

[Wave 9 merge](alpha-1.0-2plus1-checklist.md): **merged into devel Oct 25 (`9ae2f36d3`; gate at `9ae2f36d3`)**: wave 8b (W159 applied-wait, flake sweep, A6–A12 hygiene, docs audit 2) with the K39 fix, K38/W161 with K40/W162, the K33 payload server fix (status protection now, packer fragment resend), D43 expiry compiled in and policy-governed with the create-path follow-up, and the Task A/C/D agent branches (Stage 2 rehearsal driver, decoder validation fail-closed, endurance harness). intel2 `make test-all` (8 jobs): 498 commands in 561 s, 0 FAIL, 14 NOTRUN host limits (no RDMA device, no valgrind, no cluster scripts, fault-build-only case), PASS with host limits. Merge-time fixes: a forwarded expiry deletion may forward its own steps (the flake sweep's never-forward-again rule refused it); Task D's 15-minute soak is `make test-soak`, outside the W160 test-all target, which is 16 min since this wave; loopback-5 runs loopback-3's gates. Found: **K41** (packer places on a stopped member the leader never marked down, then one more loss loses the segment), OPEN. Two earlier runs of the gate on this tree: 4 FAIL (the fixes above), then 3 FAIL (apply-wait build race, put-drain include, K41).

[K39: write EIO with one member down](k39-write-eio-one-member-down-20261025.md): **FIXED (scoped), branch `claude/k39-write-eio` on merge-wave8b `9a28c1b1c` (code `7c4c2d318`, tests `bd330dd3c`), not merged, not pushed; intel1: `test_k35_fsync_degraded.py` 20 of 20 at the 10 s default (base 4 of 8 FAIL), `make test-all` at 8 jobs 485 commands in 471 s, 0 FAIL, 14 NOTRUN host limits. Raw: intel1 `~/efs-artifacts/k39-20261025/` (`SHA256SUMS` `ef7faa5a…a88c`).** The `EIO` was the client's put-ticket ADMIT to a stopping lane-group leader: one lost exchange was NET at once (the "REPORT" in `flush-meta stage=report` was never sent). Ticket and fsync degraded-mark RPCs now re-pick a voter; placement probes a member back for under a second that the client still marked down; an earlier fragment acknowledgement counts once and a drain-time loss is a hard failure (each was a degraded stripe where place-around was possible). The wave-8b merge waits on this branch. Open: `test_k33_payload.py` read `EIO` with one member down, 4 of 10 on the fix and 5 of 10 on the base (same `fetch published ... rc=-6` line), the K33 read session's item.

[A12 closure](a12-closure-20261025.md) and [A6–A12 hygiene: dead-code sweep, buffered I/O remnants, A9 leader placement](a12-hygiene-20261025.md):
**branch `claude/fervent-wilson-67282f` on devel `5a7463440`, not merged, not pushed; intel2 `make test-all` at `ab07a9c40` (8 jobs): 480 commands in 493 s, 2 FAIL, 16 NOTRUN host limits. Both FAILs are pre-existing intermittents: `test_k33_payload.py` node-down EIO (also 2/2 FAIL on the base) and B6 `refuse` vs K37 packing; each passed 2/2 alone on the branch. Raw: intel2 `~/efs-artifacts/a12-20261025/` (`SHA256SUMS` `bf87d4c8…5759`).**
A12: every A6 selector is removed and refused, or justified (`EFS_TRANSPORT`, A8). A/B selectors 15 → 0; reachable A/B combinations 32,768 → 1; variables a normal build reads 105 → 77. `env_inventory.py --check` now fails any read setting whose kind is not a retained one. The sweep removed two orphaned functions and the fields nothing read, and dropped the buffered/direct options from the live harnesses, the fault harness, `qualify.py` and `bench_profile.py`. A9 on loopback (600 s, 12 step-downs, 6 leader restarts): both leaders on one node 41.5 % of the run at 3 members and 20.7 % at 4 and 5. Real-host placement and live qualification of the retained paths stay open. Expiry code is untouched (D43).

[K40 / W162: two clients on shared chunks, unit stages left undecided until EIO](k40-two-client-unit-busy-20261025.md):
**code fix PASS (scoped), branch `claude/musing-grothendieck-1a6c0c` on the K38 branch `77d973b1c`, not merged, not pushed; intel2 `make test-all` PASS with host limits at `30d7caf79` (484 commands, 0 FAIL, 16 NOTRUN; a first run had 1 FAIL, the known `test_k33_payload.py` step-5 read EIO, base 2 of 8 alone); orcd-mini NOTRUN.**
A forwarded PREPARE batch refused for one CAS-lost chunk left the other chunks' stages with no ABORT until the recovery scan, so every REPORT on them was BUSY past the 8 s budget. The unit now ABORTs once any entry was submitted (F1's `d7a6eed7e`, cherry-picked). Loopback two clients: base 3 of 4 runs `EIO`, fix 10 of 10 PASS with no recovery ABORT; the deterministic forwarded-refusal test fails 5 of 5 without the fix. Two clients are now the K38 loopback default.

[K38 / W161: STALE span re-sent on its old base until EIO](k38-stale-span-replay-20261025.md):
**code fix PASS (scoped), branch `claude/vibrant-wiles-325684` on devel `5af760ff1`, not merged, not pushed; intel2 `make test-all` PASS with host limits at `bac34da00` (483 commands, 0 FAIL, 16 NOTRUN; a first run had 1 FAIL, `test_k36_recovery.py` step 8 read EIO, which passed 3 of 3 alone on the branch and on the base and did not recur); orcd-mini ior hard rerun NOTRUN.**
A unit test on the production replay proves the cause; the body-less span replay now rebuilds or fails closed, a record re-sent unchanged after two STALE rounds is stopped, and `report-resolve` names the stuck record. The loopback test found and the branch fixes two silent losses of acknowledged bytes in the same path. Loopback, 24 writers sharing chunks under dcache pressure: base 6 of 6 runs fail (5 `EIO` with the K38 signature, 1 with lost records), fix 20 of 20 pass. Two writing clients on shared chunks failed ~50 % with unit BUSY (base too): K40 above.

[K38: packed read EIO after "Protection: full" in the K33 payload outages](k38-payload-protection-now-20261025.md):
**PASS (scoped, loopback, amd), branch `claude/kind-nash-0f2244`, not merged, not pushed; `tests/test_k33_payload.py` 30 of 30 PASS alternating with the base (devel `5a7463440` + the flake-sweep client fix), which failed 8 of 30; `make test-all` on amd at `f0b51c0ed`: 480 commands, 14 FAIL, 6 NOTRUN; untouched devel 479, 12 FAIL; the 12 are shared (RDMA, host limits), the other two (D4 `put_errors`, the step-down latency bound under load) fail on devel or pass standalone on both (raw: amd `~/efs-artifacts/k38-20261025/`, `SHA256SUMS` `4df49442…ae9b`).**
Cause of the remaining step-5 `EIO`: the packer wrote a segment degraded (often around a member that had returned 0.3 s earlier: a stale pooled peer connection or its connect cooldown gave NET) and `efs-mgmt status` still said `Degraded segments: 0`, `Protection: full`, because both degraded counts were the last rebuild pass's sample; the test (as an operator would) then stopped the member holding one of the two fragments. Fixed: STATUS counts the leader's marks at the reply; the packer resends a NET fragment once on a new connection. New `tests/test_k38_protection_now.py` (fails with either half reverted). Live multi-host NOTRUN.

[D43 follow-up: creates keep the ordinary path until a folder policy is enabled](d43-expiry-fast-create-20261025.md):
**PASS (scoped, two hosts), not merged, not pushed; branch `claude/funny-blackburn-010f41` on the D43 tip, code `b5b9ffc03`. Expiry on with no folder policy now costs as much as off (amd 0.93x/1.01x, baseline 2.2x); nuc `make test-all` at 8 jobs: 484 commands, 0 FAIL, 15 NOTRUN host limits (amd: 11 FAIL, all failing on the baseline there too). Raw: nuc and amd `~/efs-artifacts/d43-fast-20261025/` (`SHA256SUMS` `2eaff79d…d92e`, `37bf20b4…9035`).**
A folder-policy count (KV kind 57, both anchors) moves in the policy
write's transaction; with expiry on and no folder policy enabled, creates
take the ordinary path behind a per-group apply fence (no cache window).

[D43: expiry ships under the cluster expiry policy](d43-expiry-20261025.md):
**PASS (scoped, one host), not merged, not pushed; branch `claude/ecstatic-mccarthy-911369`, code `6b731298d`. nuc `make test-all` at 8 jobs: 484 commands, 0 FAIL, 15 NOTRUN host limits (earlier runs: one known intermittent each, D4 `put_errors=1` twice beside the new fixture before its budget fix); `d5-expiry` loopback dry run PASS (POSIX 218/218 with expiry on). Raw: nuc `~/efs-artifacts/d43-expiry-20261025/` (`SHA256SUMS` `6d6d768e…3d63`).**
No build switch (`EFS_BUILD_EXPIRY` and `make test-expiry` removed) and no
environment toggle (`EFS_EXPIRY` refused, naming the policy command); the
cluster expiry policy (`efs-mgmt expiry-policy NODE:PORT cluster`,
versioned, off by default) changes at runtime; a deadline acts only while
its source folder policy is enabled. Expiry tests back in `make test-all`;
new qualify gate `d5-expiry` (item D5) on every matrix topology.

[Task A: full three- and five-member Stage 2 rehearsal](task-a-20261025.md):
**rehearsal complete with product findings; branch `agent/devel-task-a`,
full matrices `c3c4fa2bd`, affected GC and final code gate `a6ba11c3a`.
Final `make test-all`: 479 commands, 11 FAIL (all among the base's 12),
6 NOTRUN.** Both full loopback runs: 21 PASS / 1 harness FAIL / 1 NOTRUN,
about 66/61 minutes; GC sequencing corrected, affected GC PASS on one run,
healthy cleanup BUSY on the other. Join retry refusal and rewrite/degraded-read
EIO remain product findings. Real-host/RDMA qualification stays NOTRUN.
Raw evidence remains external; private AMD test tree removed.

[efsd RSS plateau under the fixed-working-set D4 load (F1 follow-ups item 2)](efsd-rss-plateau-20261025.md):
**branch `claude/wizardly-mcclintock-0e2efd`, not merged, not pushed; RSS plateau PASS on one 60-minute run at `66a3a4024` (xorinox: efsd +0.6 / −0.8 / −0.1 %, efs-fuse +0.0 / +3.6 %; the D4 run itself FAIL on one truncate EIO, not memory); FAIL on every nuc run. xorinox at `66a3a4024`: `make tsan-check` PASS 18/18, `make asan-check` PASS 47/47, `make test-all` 481 commands, 2 FAIL (K33 read EIO under the pool: `test_k33_payload.py` fails 1 of 3 alone on the base too; `test_k33_growth.py` did not reproduce), 14 NOTRUN host limits.**
The connection-thread hypothesis is refuted as the main cause (about 0.15 MiB per thread), but no pool
ever closed an idle connection; both pools now close one after 30 s idle. Bounded: the reclaim journal
ring, whose pages were never released (half the baseline's growth), and the packer's compaction
buffers, which fragmented the leader's arenas. On nuc the last run still stepped up at minute 48, when
the host's page cache ran short (slab 14.6 GB, 395 MB cached), with elections; a concurrency step still
raises arena memory for the rest of the load. amd went down during the session (cause not established).

[F1 follow-ups: K36 under TSan, RSS plateau on a fixed working set, packer put errors, rewrite EIO](f1-followups-20261025.md):
**branch `claude/devel-f1-followups-ae2deb`, not merged, not pushed; xorinox: `make test-all` 480 commands, 0 FAIL, 14 NOTRUN host limits (`04408a3e4`); `make tsan-check` PASS 18/18, `make asan-check` PASS 47/47 (`758dd144c`, same code); RSS plateau FAIL (bounded, not flat).**
`test_k36_recovery.py` under TSan PASS 10 of 10 (crash hooks now `exit_group`; a hook race and a test
defect fixed). The D4 fixed-working-set variant rewrites 2048 files for 30 and 60 minutes with no net
creation. Plateau FAIL, bounded not flat: the clients plateau; efsd's live heap is flat, but glibc arenas held
freed memory. efsd now caps arenas at 8, fixes the mmap threshold at 256 KiB and trims every 15 s, cutting its
60-minute peak by about a third. It still grows 8–13 % over minutes 40–60 with throughput (open). The packer
put errors were follower `NOT_PRIMARY`; with the flake-sweep fix the 20-minute D4 load ends with 0 (was 3), the
60-minute rewrite load still with 5. New product fix: a W77 publication unit whose forwarded PREPARE batch
was refused skipped its ABORT, which made rewrites EIO (regression test red 5 of 5, green 6 of 6).

[Task C: transactional decoder validation](task-c-20261025.md):
**branch `agent/devel-task-c`, code `cd5ed9dc5`, not merged.** Original randomized
campaign PASS 25/25, expanded two-hour apply PASS, private three-node W140
refusal PASS, ASan PASS 47/47. Corrected full-suite and five affected decoder
reruns RUNNING; acceptance PENDING. Raw evidence stays outside Git on intel1.

[Oct 25 K22/K6 loopback gates: snapshot import into a stalled follower, leader admission at 3/4, W130 one-replica DIR fault with raft-resync recovery](k22-k6-loopback-taskb-20261025.md):
**branch `agent/devel-taskb` (from devel `5af760ff1`), not merged, not pushed; xorinox loopback, fault build, TCP. `make test-all` at the head: `TOTAL 482 FAIL 0 NOTRUN 15 STATUS INCOMPLETE (765s)` (NOTRUN = host limits: no RDMA, no cluster scripts).** All three gates PASS as new `make test-long` tests with budget rows; two product defects found and fixed on the branch (send_snap retry storm; raft-resync could not heal a halted replica), two opened (W162 serve-path decode not fail-closed, W163 close EIO while a leader's apply lags). NOTRUN: production-scale 1 GiB lag on real hosts, RDMA, multi-host.

[Oct 25 merge: F1 memory and concurrency gates, W160 gate speed](f1-memory-concurrency-gates-20261025.md):
**merged into devel; merged-tree `make test-all` on intel2 at `55eafe6d0`, first run at `EFS_TEST_JOBS=8` with the W160 budgets: 479 commands in 574 s, 1 FAIL, 16 NOTRUN host limits.** The FAIL is `test_b_geometry.py` grow-b's automatic-quota slack assertion, the known intermittent the flake sweep fixes (test defect, branch `claude/devel-flake-sweep-1505b6`, not merged); it passed 3 of 3 reruns on the same tree. Raw: intel2 `~/efs-artifacts/merge-wave8a-20261025/`, `SHA256SUMS` there.

[Oct 25 user-docs audit against `ec3e09306`: every command, option, workflow and failure case exercised on a loopback cluster](docs-audit-20261025.md):
**docs updated; docs gate PASS; nuc `make test-all` at `e1ce4a4d4`: 472 commands, 4 FAIL at 8 jobs (3 PASS alone, `test_report_stepdown.py` intermittent, 2 of 4 alone), 14 NOTRUN host limits (raw: nuc `~/efs-artifacts/docs-audit-20261025/`, `SHA256SUMS` `51666fda…05bf5`); no product code changed.**
The guide, surface list, POSIX subset, release notes, operations, power-user, quick start and
`efs-commands` pages now match the binaries (K34–K37, E2, `status` output and exit status,
restart after a forced, failed-barrier or killed daemon, `.efs` by policy only, `O_DIRECT` only).
New `make test` check `tests/test_cli_docs.py`: every `efs-mgmt` command and `efsd`/`efs-fuse`
option in `cli_help.h` is named in the user docs, and the docs name no command or flag the help
lacks. The pages follow D43 (expiry and `.efs` ship in alpha 1.0); at this base expiry is still
compiled out and being re-enabled (U19). Open for the user: U4, a clean client stop fails at the session revoke
(`storage-fence`, `EFS_ERR_BUSY`) while any member is down; S4/W122 and F12 unchanged.

[Oct 25 merge: REPORT step-down fix, packer UNINDEX race, K36+K37 segment marks, E2 clean stop, Stage 2 real gates, B6 per-segment checks](a11-qualification-matrix-20261024.md):
**merged into devel; merged-tree `make test-all` on intel2 at `0b01a2a1e`: 471 commands, 0 FAIL, 15 NOTRUN host limits (2,604 s; raw: intel2 `~/efs-artifacts/merge-wave7-20261025/`, `SHA256SUMS` in that directory).** A11 findings 1 and 2 are fixed; every Stage 2 item now maps to a real `scripts/qualify.py` gate; live and real-host runs stay NOTRUN; every store needs a fresh format.

[F1: memory and concurrency gates on the candidate](f1-memory-concurrency-gates-20261025.md):
**branch `claude/devel-f1-gates-46ff64` on `merge-wave7`, not merged, not pushed; xorinox, last code commit `c32cdf544`: `make leak-check` PASS 41/41, `make asan-check` PASS 47/47, `make tsan-check` PASS 18/18 (`test_k36_recovery.py` intermittent under TSan), `make test-all` PASS (861 commands, exit 0); RDMA memcheck NOTRUN (no device). Raw: xorinox `~/efs-artifacts/f1-gates-20261025/` (`SHA256SUMS` `825a4507…ab106`).**
Two new gates (`tests/sanitizer_check.sh`: ThreadSanitizer and ASan+UBSan+LeakSanitizer over
a sanitized copy of the tree, unit binaries, harnesses and daemons, with a self-test in `make test`),
16 new entries in the leak gate. 15 findings fixed in product and test code: UB in the K36 recovery
thread, a `fopencookie` cookie leak at every daemon exit, twelve data races (shutdown flags,
member-store copy during formation, startup banner vs rejoin, counters and lazy env flags), a
misaligned read in `test_wire`. 20-minute D4 RSS bounded (client 247 MiB, servers 134 MiB) but not
flat while the namespace grows. Open: `test_k36_recovery.py` step 4 intermittent under TSan; 3–7
packer put errors over a 20-minute load (not memory; logs not retained).

[Gate speed: `make test-all` under 15 minutes at 8 jobs (W160)](gate-speed-20261025.md):
**PARTIAL PASS, not merged; branch `claude/devel-gate-speed-8aee30` on merge-wave7: nuc `make test-all` at 8 jobs 811 s / 839 s (0 FAIL) / 989 s (2 pre-existing intermittents) against 1,182–1,217 s with 3–6 load failures before; serial 2,754 s → 2,311 s. Three clean runs under 15 min not reached: the nuc's one NVMe (raw: nuc `~/efs-artifacts/gate-speed-20261025/`).**
The code gate had grown to 44 minutes (serial on intel2) and failed six commands under an 8-way pool on the nuc.
Runner budgets and disk-weighted slots per `test-long` command (`tests/budgets.tsv`, largest first,
`OVERBUDGET` lines), a drift check in `tests/test_make_test_recipe.py`, B6, B geometry and the D39 model as
independent commands, parallel verification and fast payloads in the K33 tests and the B/D4 drivers, status
polls instead of settle sleeps, and ports held for the life of a loopback cluster. No assertion relaxed; four
tests check more. Follow-ups: 15 s unmount storage fence to a down member, 32 s `efs-mgmt status` with a frozen
member, intermittent EIO on a degraded read in `test_k33_payload.py` under host load (pre-existing).

[W159: fixture POSIX timeouts, slow durable barriers and the 400 ms applied-wait](w159-fixture-posix-timeouts-20261025.md):
**CAUSE FOUND, fix PARTIAL, PASS (scoped, loopback); branch `claude/devel-w159-fe09e8` on `merge-wave7`, not merged, not pushed; amd `make test-all` on the fixed tree: see the page.**
The expiry fixture's closing POSIX run timed out two metadata-heavy cases
because its one consumer NVMe answers a durable barrier in 7–28 ms (29× amd)
and four members queue on it: about 39 fsync/fdatasync per create-and-unlink
pair across the members, so the Raft pump's apply lag passed the server's
fixed 400 ms applied-wait, each expiry answered BUSY and the client's backoff
ladder made the 2–7 s RPCs. Reproduced on amd on the untouched base with an
emulated 7 ms barrier queued on one lock (`scripts/w159_fixture.py`); the
same fixture on amd's disk passes everything with no RPC over 1 s. The
applied-wait for a member's own committed entry is now bounded by progress
(`src/server/apply_wait.h`, 400 ms grants renewed while the member applies,
2 s cap): `concurrent_creates_same_dir` passes under the emulation,
`dir_many_files` passes one round in two there (the barrier cost, a Stage 3
follow-up).

[Flake sweep: every intermittent failure of the gate](flake-sweep-20261025.md):
**FIXED, final gate NOTRUN (stopped after the merge into merge-wave8b, gated there); branch `claude/devel-flake-sweep-1505b6` (head of the code: `8f1d50ed8`), not merged, not pushed.** Ten 8-job `make test-all` runs (471 commands) and three serial `test-long` runs (62) on intel1 at `0b01a2a1e` failed six commands at random; the final-gate attempts surfaced a seventh. Product defects fixed: the client's away mark blocked place-around (K35 case 7, 2/10 + 1/3), the packer took one transient `NOT_PRIMARY` for a missing fragment (D4, 1/10), forwarded PUT ticket admissions were forwarded again during an election until the peer pools stalled for 5 s (report step-down, 1/10; 5/28 loaded before, 0/40 after; new wire mark on member forwards). Test defects fixed: K37 case 7, B geometry, K36 case 11, B6 refuse mode, query-first NOTRUN, the runner self-test's false NOTRUN. Open, single occurrences without logs: K36 case 8 read EIO, K33 payload read-back EIO. Raw: intel1 `~/efs-artifacts/flake-sweep-20261025/`.


[Packer UNINDEX race: an O_TRUNC rewrite left unpacked (B6 follow-up)](pack-unindex-race-20261024.md):
**FIXED, PASS (scoped), not merged; branch `claude/interesting-clarke-cfb30b`, code `6ef6e6037`; xorinox `make test-all`: 467 commands, 0 FAIL, 14 NOTRUN host limits (devel: 1 FAIL, `test_d4_progress.py`); amd: 9 FAIL, all RDMA and all failing on devel there too (raw: xorinox and amd `~/efs-artifacts/pkrace-testall-{src,base}-20261024/`).**
The packer's UNINDEX deleted a candidate key unconditionally, so a rewrite whose publication
re-added the key before the UNINDEX committed was never packed (B6 full: `packed 11 of 12`).
Candidate values are now `[inode generation][publish stamp]` and UNINDEX deletes only on an
exact match (fresh format). New unit case red without the comparison, green with it; B6 full
15 of 15 PASS with the fix (the live race did not fire on devel in 31 runs). Multi-host NOTRUN.

[B6 under K37, `test_get_integrity.py`, and a packer unindex race](b6-k37-merge-tests-20261024.md):
**test fixes PASS (scoped, loopback), merged into devel (`0b01a2a1e`; tested at `335bfb5a0`), not pushed; B6 3 of 3 and `test_get_integrity.py` 3 of 3 on intel1; intel2 `make test-all` at `335bfb5a0`: 467 commands, 1 FAIL in an untouched test (`test_k33_payload.py`, intermittent EIO), 15 NOTRUN host limits (raw: intel1/intel2 `~/efs-artifacts/b6-k37-335bfb5a0-20261024/`, `SHA256SUMS` `2d0eda66…e769` / `a115365d…c825`). B6 mode `full`: known intermittent FAIL, not fixed.**
B6's "nothing packed during a fault" predated K37's decided policy. B6 now
checks that every segment written degraded names node 3, that nothing is
packed without one, that those files read exact, and that `Degraded
segments: 0` after the restore. It does not check how each mark ends,
because compaction retires marks too. Open finding (follow-up): `efs_meta_apply_pack_unindex` deletes a
candidate key unconditionally, so an `O_TRUNC` rewrite scanned at size 0
can lose its re-put candidate and stay unpacked (no data loss; on devel
too). Proposed fix: compare-and-delete UNINDEX (Raft command and KV value
format change). Also seen: `test_k36_recovery.py` step 11 assumes segments
in both metadata groups (placement-dependent, failed once).

[Oct 24 merge: docs-review fixes, B6/D4, K37, A11 and the K33 live-harness port](a11-qualification-matrix-20261024.md):
**merged into devel; merged-tree `make test-all` on intel2 at `646a43041`: 467 commands, 2 FAIL from the merge (a duplicate `read_reprobe_down` stub in `test_get_integrity.py`; `test_b6_unavailable.py` still asserting no packing during a fault, which K37's policy allows with a degraded mark), both fixed in `12d2a7957` and rerun PASS on intel2; 15 NOTRUN host limits (raw: intel2 `~/efs-artifacts/merge-wave6-20261024/`, `SHA256SUMS` `fd750fd9…d662`).** `add-node` and `add-storage` are gone (JOIN/ADD_STORAGE wire types removed). Live gates stay NOTRUN; every store needs a fresh format.

[A11 finding 2: REPORT across a metadata leader step-down](a11-f2-report-stepdown-20261024.md):
**FIXED, PASS (scoped, loopback), merged into devel (`1fee9bb04`; tested at `6654d5f15`); amd `make test-all` at `04562f14a`: 468 commands, 12 FAIL, all also failing on the base, 6 NOTRUN host limits; raw: amd `~/efs-artifacts/report-stepdown-20261024/`, `SHA256SUMS` `3857346d…41a6d6`.**
The nuc "30 s REPORT timeout" was an 8 s BUSY stall (`recv_ms` printed the
configured bound): a DECIDE COMMIT that failed NOT_PRIMARY at the step-down
was handed to nobody, so its PREPAREs held the inode until the 5 s recovery
scan. Such a DECIDE, and a failed DROP, now go to the finisher as ABORT at
the new leader (decide-once keeps a COMMIT that survived); a BUSY DECIDE with
no decision after 1 s is aborted the same way. Bound: a REPORT completes
within 3 s per leader change crossed. Daemon test `tests/test_report_stepdown.py`
(fault `EFS_FAULT_DECIDE_LOST`) 5 of 5 PASS, control FAIL (6.1 s);
`append_failover.py` step-down reruns 5 of 5 PASS with 0 REPORT timeouts and
no recovery-scan stall, slowest REPORT 1.8 s (base: stalls in 4 of 5, 2 over
the bound). Multi-host, RDMA and orcd-mini NOTRUN.

[Stage 2 gates A13, C5, D1/D2, D3, E4 in `scripts/qualify.py`](stage2-gates-20261024.md):
**PASS (scoped, loopback), branch `claude/devel-stage2-gates-359865` on `merge-wave6`, not merged; no stub gate is left.**
`a13-node-addition` grows its own cluster 3 → 4 (a killed join retried: the same member; SHARD2
`{1,2,3}` → `{2,3,4}`) → 5 with the seed down under a client load; `c5-protocol-live` drives the
W133 declaration gate and W140 tag refusal over the wire, removed settings, a session fence and the
clean-unmount revoke; `d1-packing` packed rows, stored-object savings, D2 mutations and compaction,
cold restart; `d3-efs-load` `.efs` queries exact under I/O with deadlines; `e4-scale` declared
fixtures sized by member count, cold reads under queries, delete/reclaim. xorinox loopback-3 full
run at `babc80ced`: the five new gates PASS (18 PASS, 1 FAIL `gc-reclamation`, 1 NOTRUN A4).
xorinox `make -k test-all` at `babc80ced`: 420 commands, 1 FAIL, 12 NOTRUN host limits; the FAIL,
`test_b6_unavailable.py` refuse mode (files packed while a member refuses), fails 3 of 3 on the base
`646a43041`, as does `gc-reclamation`. FIXED: `test_get_integrity.py` did not compile on
`merge-wave6`. Raw: xorinox `~/efs-artifacts/stage2-gates-20261024/`, `SHA256SUMS` `78ec26cc…d0a4`.
Real hosts NOTRUN (parent).

[Oct 24 merge: READ_VERIFY, K34, A7 removals, K35, K36](decisions.md#d42--alpha-10-decisions-taken-oct-24-user-with-review-conditions):
**merged into devel; merged-tree `make test-all` on intel2 at `0f0a79eac`: 459 commands, 0 FAIL, 14 NOTRUN host limits (1,839 s; raw: intel2 `~/efs-artifacts/merge-wave5-0f0a79eac-20261024/`, `SHA256SUMS` `01b2d681…dcd9`).** The intermediate gate at `376369aa8` had one FAIL (a missing K34 stub in `test_get_integrity.py`, fixed in `1898c4c63`). K36's recovery mark is Raft tag 248 beside K35's tag 249. devops `8bc872e` and cluster `1402762` drop the removed settings (not pushed). Every store needs a fresh format. Live gates stay NOTRUN.

[K36 / F23: verified recovery after an unclean stop](k36-verified-recovery-20261024.md):
**PASS (scoped, loopback), merged onto `merge-wave5` at `c19bf9fdb`, not in devel** (branch `claude/devel-k36-recovery-70f9de`; recovery mark is Raft tag 248, K35 holds 249).
A crashed or killed store now proves itself (boot counter, reclaim journal,
session certificates, every local fragment, each hosted metadata group at its
leader's commit, every published reference against its digest), drops what it
cannot prove, marks those stripes for W154 rebuild and clears the COMMIT
fence; fragments the crashed run acknowledged stay unproven in their client's `fsync`.
Daemon crash tests during PUT, publication, reclaim, KV compaction and
recovery PASS (8 of 8 runs). Merged-tree gate on intel1 at `87034b518`: `test-long` 53/53 PASS,
`test` 418 of 419 PASS, the other `test_get_integrity.py` (a merge-wave5 stub gap, fixed by
`1898c4c63`; PASS at `c19bf9fdb`); host-limit NOTRUNs only. Power-loss qualification, multi-host and RDMA NOTRUN.

[Alpha 1.0 docs review: findings F2–F6, F11, F13, F14, F18–F20 fixed](s-docs-fixes-alpha-1.0-20261024.md):
**PASS (scoped), merged into devel (`77d937d27`); amd `make test-all` at `f588f6efd`: 461 commands, 11 FAIL, all also failing on the base (RDMA on `rxe0`, a fault-test timeout, `test_get_integrity.py` not compiling since the merges); raw: amd `~/efs-artifacts/sdocs-fixes-20261024/`, `SHA256SUMS` `a1ed1a7f…3b7a`.** Every
`efs-mgmt` command has help, checked against the dispatcher in `make test`;
`add-node` (use `efsd --join`), `add-storage` and `efs-fuse --bench*` are
removed and refused; a node's storage roots are fixed when its store is
created (packed segments are placed by root count) and a changed root set is
refused at startup; a 17th joiner is refused and exits; new POSIX cases
(F13) and daemon tests for `raft-resync`, fixed roots and the member limit;
README and quick start corrected. The [findings table](s-docs-alpha-1.0-20261024.md#findings-docs-and-code-disagreements)
records every outcome. Stores formatted by an earlier build are refused.

[B6 and D4: an unavailable fragment destination, bounded progress under background work](b6-d4-unavailable-progress-20261024.md):
**PASS (scoped, loopback), merged into devel (`1b3fd25b7`; tested at `834f0b7ec`); xorinox `make test-all` at `834f0b7ec`: exit 0, 0 FAIL, 14 NOTRUN host limits (raw: xorinox `~/efs-artifacts/b6-d4-20261024/`, `SHA256SUMS` `f5e519ff…8840`).**
B6: node 3 of three down, refusing, slow past every client deadline and
full, for ordinary and packed files during write, fsync and read; reads
exact, fsync only with proof, W154 clears after the orderly restore. D4:
foreground deadlines, client/server RSS, KV memtable/L0, pack and index
backlogs bounded and drained under a small-file load with packing,
indexing and compaction active. FIXED: a packed read waited out a slow
member and failed EBUSY; a just-formed member's first `.efs` catalog waited
60 s and every query on it failed EIO. Real-host gates `b6-member-down`
and `d4-progress` in `scripts/qualify.py`, NOTRUN (parent).

[K37: rebuild on return for packed segments](k37-segment-rebuild-20261024.md):
**FIXED, PASS (scoped, loopback), branch `claude/devel-pack-rebuild-fa5139`,
code `b48ec444f`, `5ef2b8d6d`, `b205cb9db`, not merged.** A packed segment with a missing fragment
gets one durable degraded mark (KV kind 249, Raft tag 247): written
degraded by packing or compaction (two fragments durable; one cancels),
found missing or torn by the leader's audit of a returning member (wire
241/242), or a rebuild source proven lost (unrecoverable, reported, never
cleared). The leader rebuilds, verifies, stores under the exact mark and
clears only after the placement node verified the copy; `efs-mgmt status`
counts degraded segments and the cluster stays DEGRADED until they clear.
Daemon test `tests/test_k37_segment_rebuild.py` PASS. intel2 `make test-all` at `b205cb9db`: 459 commands, 1 FAIL that also fails on the base (`tests/test_get_integrity.py`, a READ_VERIFY test / K34 merge interaction), 14 NOTRUN host limits (raw: intel2 `~/efs-artifacts/k37-20261024/`, `SHA256SUMS` `61fbd81d…98f0`). Live
multi-host, RDMA and orcd-mini NOTRUN (parent).

[K36 + K37: segment fragments dropped by verified recovery are rebuilt](k36-k37-segment-recovery-20261024.md):
**PASS (scoped, loopback), branch `claude/devel-k36-k37-segments-5885d9`,
code `ad7fe1ded`, not merged.** A referenced packed-segment fragment that
K36 recovery drops is now marked through K37's one authority
(`EFS_MD_CMD_PACK_DEGRADED`, reason `R_RECOVERY`, forwarded to the group
leader and read back) before the recovery fence clears; the audit sweeps
only settled stores and skips marked fragments, so the two cannot disagree.
K37 rebuilds and verifies the fragment and clears the mark; a second loss
before the rebuild is reported unrecoverable, never healed. Daemon test
`tests/test_k36_segment_recovery.py` PASS. intel1 `make test-all` at
`ad7fe1ded`: 468 commands, 2 FAIL that also fail on the base
(`test_get_integrity.py` stub, `test_b6_unavailable.py` packing during a
fault), 15 NOTRUN host limits (raw: intel1
`~/efs-artifacts/k36-k37-segments-20261024/`, `SHA256SUMS` `9817196b…b7fd`).
Live multi-host, RDMA and orcd-mini NOTRUN (parent).

[E2 / A11 finding 1: clean client stop on a busy client](e2-clean-stop-20261024.md):
**PASS (scoped, one host), branch `claude/devel-e2-clean-stop-a53dcb` on `merge-wave6`, not merged.**
The unmount's session revoke took about 11 s for 765 touched shards (one
Raft commit per shard and step, and three directory `fsync`s in every
FENCE_LOC apply), so `client.sh stop` gave up after its fixed 10 s. Now the
per-shard work runs 16 wide, FENCE_LOC/ACK cover a bitmap word per entry
(new encodings: fresh format), the certificate's directories are synced
once, and a cleanup probe skips empty shards: 2,058 shards 63 s → 11 s, ~870
shards 8.7 s → 1.5 s. efs-fuse writes its stop phases to a stop record and
`client_processes.py retire` waits while they advance (30 s stall, 600 s
total), reporting a stall or a failed revoke with its reason and never
killing (hook `EFS_FAULT_REVOKE_BEGIN_FILE`). `nuc-private-3` rerun:
posix-durability (busy mount `touched=747`, stop 2.3 s), e1-manifest and
e2-client-stop PASS. nuc `make test-all` at `e6b3317fc`: 469 commands, 2 FAIL, both
failing identically on the base (`test_get_integrity.py`,
`test_b6_unavailable.py`), 15 NOTRUN host limits (raw: nuc
`~/efs-artifacts/e2-stop-20261024/`, `SHA256SUMS` `84a5b55d…de857`).

[A11: reduced qualification matrix and the Stage 2 run plan](a11-qualification-matrix-20261024.md):
**plan PASS (scoped), qualification NOTRUN; merged into devel (`646a43041`).**
Dimensions left after A6/A7/D42: transport (`tcp`, `rdma`), 3–16 members,
`O_DIRECT` only, one root per node, one or two client mounts, built-in
tunables. Selector × transport × member count 1,376,256 → 28; qualified
exhaustively: TCP/RDMA × 3, 4, 5 distinct hosts; 6–16 sampled. Every
Stage 2 item mapped in `scripts/qualify_matrix.json`; `qualify.py run
--matrix … --topology NAME` runs a topology (orcd-mini, intel, loopback,
bare-metal 3/4/5). The seven NOTRUN stub gates are real gates now (B6/D4 merge,
[Stage 2 gates](stage2-gates-20261024.md)). **A4 blocked:** no
distinct-host topology can form today (host firewalls, amd's network, no
three-host RDMA). Driver fixes: the `b-checks` plugin called
`b_checks.py` with options it rejects; E3 did not retry a step-down refused
with "retry". Findings on nuc: a clean `client.sh stop` is refused when the
unmount session revoke takes over 10 s (2 of 2 full private runs); one
30 s REPORT timeout in append failover (8,000/8,000 records exact; an 8 s
BUSY stall behind a DECIDE lost to the step-down, [fixed](a11-f2-report-stepdown-20261024.md)). The four
live harnesses now use the K33 CLI: on amd k14, k13 and fragment-integrity
PASS, gc-reclamation FAIL on the same refused client stop. nuc `make test-all` at `8d858a791`:
458 commands, 1 FAIL, 14 NOTRUN host limits; the FAIL,
`tests/test_get_integrity.py`, no longer compiles on `merge-wave5` (no stub
for K34's `read_reprobe_down`) and fails the same on the base `376369aa8`.

[D42 `EFS_READ_VERIFY`: GET-path integrity audit](read-verify-audit-20261024.md):
**PASS (scoped), merged into devel (`ae3e7ab75`).** The
storage node's record check does not cover the wire or the client's
buffers; with verification off the base returned wrong bytes in 12 of 51
negative cases (chunk GET, parity reconstruction, retries, spans; packed
reads were already verified). The client re-hash is now built in on every
chunk path, `EFS_READ_VERIFY` is removed and stops efsd/efs-fuse when set.
intel2 `make test-all` at `dff495ed6`: 456 commands, 0 FAIL, 14 NOTRUN host
limits (base: 1 FAIL, the intermittent K34 `test_k33_payload.py`). Cost on a
loopback cold read: +0.49 efs-fuse CPU s per GiB (0.81 → 1.30), −6 %
throughput.

[Oct 24: A7 removals decided in D42](a7-removals-20261024.md):
**PASS (scoped): `make test-all` on amd at `735b82562` fails only the 11 commands that also fail on the base (RDMA on rxe0, a fault-test timeout, K34 in `test_k33_payload.py`); live qualification NOTRUN.** Direct I/O only (storage without `O_DIRECT` refused),
12 selectors and 3 efsd flags removed and refused, expiry compiled out
(reversed by D43 on Oct 25: [D43 expiry](d43-expiry-20261025.md)),
`EFS_RDMA_TRACE`, strict `EFS_TRANSPORT=rdma`; A/B selectors 15 → 1
(`EFS_READ_VERIFY`). Branch merged into devel (`6069ada62`).
External launch/portal changes for the parent are listed there.

[K35: fsync with a member down, only with proof (D42, F22)](k35-fsync-degraded-20261024.md):
**FIXED, PASS (scoped, loopback), not merged; branch `claude/devel-k35-fsync-b63807`, code `817a71b1f`, `36ac1b7a6`; xorinox `make test-all`: exit 0 at `36ac1b7a6`, 452 test commands, 0 FAIL, 14 NOTRUN host limits (raw: xorinox `~/efs-artifacts/k35-20261024/`, `SHA256SUMS` `06836fd2…6a1e`).**
fsync COMMITs only the nodes holding the file's unproven fragments and succeeds
per stripe with two durable fragments and a durable degraded mark (new Raft tag
249), else `EIO` with the stripe logged; on four or more members fragments go
to available members; `status` shows `Protection:`. Live multi-host and RDMA
verification NOTRUN (parent).

[Oct 24 merge: A6 inventory, S1–S5/F4 docs, B1–B5, Stage 2 driver](a6-settings-inventory-20261024.md):
**merged into devel; merged-tree `make test-all` on intel2 at `c894ec5ae`: 456 commands, 0 FAIL, 14 NOTRUN host limits (1,831 s; raw: intel2 `~/efs-artifacts/merge-wave4-20261024/`, `SHA256SUMS` `2bf6bbe2…1f10`).** `test_k33_payload.py` passed in this run; it fails intermittently (K34, fix in progress). Open for the user: the A/B decision table (A7) and findings F22/F23 (fsync with a member down; unclean-stop stores).

[K34: acknowledged files unreadable while one member stops](k34-stop-put-ack-20261024.md):
**PASS (scoped), merged into devel (`fc5c1d64e`; tested at `6c09649a3`).**
`tests/test_k33_payload.py` read `EIO` with one member stopping, for three
reasons: a chunk acknowledged on the stopping member and one other while
the third answered BUSY; lease/session commands that gave up at the first
failed exchange with a stopping leader; and a stale client down-mark that
skipped a member that was back. Fixed in efsd (no acknowledgement while
stopping) and the client (complete the stripe, re-pick a voter, probe a
down-marked member). New `tests/test_k34_stop_put.py`. On nuc: `make
test-all` 450 commands, 0 FAIL, 14 NOTRUN host limits; `tests/test_k33_payload.py` 20 of 20 runs PASS (base: 2 of 8 FAIL). Live
multi-host stop under load NOTRUN. **Oct 25:** the step-5 `EIO` came back intermittently on later devel
(W159: 1 of 10; amd, three parallel lanes: 8 of 30) from a fourth cause,
a segment written degraded while status still reported full protection:
fixed by [K38](k38-payload-protection-now-20261025.md) (30 of 30).

[Oct 24 merge: placement spread, K33 acceptance, test hygiene, W126, K15/K29 rule](k33-acceptance-20261024.md):
**merged into devel; merged-tree `make test-all` on intel2 at `a2326a369`: 449 commands, 0 FAIL, 14 NOTRUN host limits (1,340 s; raw: intel2 `~/efs-artifacts/merge-wave3-20261024/`, `SHA256SUMS` `3e1f0f95…b2c`).** Placement needs a fresh format. Live gates for all five stay NOTRUN.

[Alpha 1.0 user docs and limitations, S1–S5 and F4](s-docs-alpha-1.0-20261024.md):
**drafts merged into devel (`3a2b44706`), not approved.**
[Surface list and effective settings](../using/alpha-1.0-surface.md),
[POSIX subset](../using/alpha-1.0-posix.md),
[guide](../using/alpha-1.0-guide.md) and
[release notes draft](../release-notes/alpha-1.0.md), checked against
`a2326a369`; 24 docs/code findings (help gaps, untested `add-storage`, no
port authentication, profile defaults); stale claims in `operations.md` and
`power-user.md` corrected. S4 waits for A6 and the W122 decision.
**Code gate on nuc: `make -k test-all` exit 0 once, but `test_k33_payload.py`
fails 4 of 6 runs of the unchanged base: a file acknowledged during a
member's orderly stop reads `EIO` until it returns (F24, proposed K34).**
Also found: `fsync` returns `EIO` while any member is down, and for good
after a member's unclean stop (F22, F23).

[B1–B5 geometry, placement and usable capacity](b-geometry-capacity-20261024.md):
**PASS (scoped, loopback) merged into devel (`5a09fd938`).**
xorinox `make test-all` at `17aac881c`: 453 commands, 0 FAIL, 13 NOTRUN
host limits. New `tests/test_b_geometry.c` (unit) and
`tests/test_b_geometry.py` (3 to 8 members, every store's fragment records
against the chunk rows and pack objects) plus the driver
`scripts/b_checks.py` for real hosts. FIXED: a full stripe member was
acknowledged as a degraded stripe (silent protection debt; base took
200 MiB with a 16 MiB member full) and is now placed around or ENOSPC; the
reader no longer decodes from recomputed placement. Real-host driver run
NOTRUN (parent). Found: `test_k33_payload.py` EIO on a packed read with a
member down (base 2/3, branch 1/3), B6 follow-up.

[D41: K9 / W87 closed without resolution](decisions.md#d41--k9--w87-closed-without-resolution-user-oct-24):
**user decision Oct 24: the append record loss was never reproduced and has no root cause; K9 is closed and reopens if a loss is seen again.** Not a passing gate.

[A6 settings inventory and the decision-free part of A7/A8](a6-settings-inventory-20261024.md):
**PASS (scoped), merged into devel (`6ed4be4a8`).** amd `make test-all` fails only what the untouched base fails (RDMA on rxe0, one fault-test timeout).
Before figure for A11: 139 `EFS_*` variables read on the base (13 A/B
selectors, 44 numeric tunables, 34 observability, 3 test hooks in normal
builds), 127 on the branch. `make test` fails a new reader without an
inventory row. Removed: 11 unset tunables (built-in values), the three
normal-build test hooks (now `EFS_FAULT_*` in `EFS_FAULTS` builds); 19
removed names stop efsd and efs-fuse. The 15 A/B selectors (13 variables,
2 efsd options) are in a [decision table](a6-settings-inventory-20261024.md#decision-table-for-the-parent-a7)
for the user; none removed. Portal and launch-script changes listed for the
parent.

[Stage 2 qualification driver `scripts/qualify.py`](stage2-qualify-driver-20261024.md):
**driver PASS (scoped), merged into devel (`ebfdbfe78`).**
One command runs the POSIX suites, the live harnesses, E1–E3 and a pluggable
B-checks gate against a described cluster, with per-gate commands, deadlines,
verdicts, live build identity and fixtures, `SHA256SUMS`, a summary draft and
the F3 row; destructive steps need `--allow`. Bookkeeping test in `make test`.
Loopback dry run on intel1 (three daemons, one host): POSIX core 216/216,
2-client, durability, append failover, E1, E2 PASS; K13, K14, fragment
integrity and GC reclamation FAIL on their stale `--node-id` CLI; E3 FAIL:
EIO on the mount while one member is stopped orderly (turnover itself clean).
intel1 `make test-all`: 450 commands, 0 FAIL except `tests/test_k33_payload.py`,
a read EIO in its member-outage phase that fails 3 of 5 runs on the base too
(proposed K34). Real-host runs NOTRUN (parent).

[K15/K29 append harness accounting rule](k15-k29-accounting-rule-20261024.md):
**PASS (scoped), merged into devel (`a2326a369`).**
`tests/live/append_failover.py` accounts a write the client saw fail (record
absent, hole explained by its abandon) and reports it separately; a lost
acknowledged record or an unexplained hole stays a mismatch. Unit check with
negative controls in `make test`. The harness (and `append_oracle_cluster.sh`)
now starts efsd with the K33 CLI. amd `make test-all` fails only what the
untouched base fails. Live failover gates NOTRUN (parent session).

[Oct 24 merge: W155, D31 drain, K33 increment 2, K33 client view](k33-client-view-20261024.md#after-the-merge):
**merged into devel; Merged-tree `make test-all` on intel2 (W155, D31 drain, K33 increment 2 and live client view at `910bbeef4`): 442 commands, 0 FAIL, 14 NOTRUN host limits (raw: intel2 `~/efs-artifacts/merge-wave2-20261024/`, `SHA256SUMS` `0d13a146…4ecf`). The first run at `3e01ba95c` failed only `tests/test_k33_client_view.py`, which waited for a joiner log line that increment 2 renamed; fixed in `910bbeef4`.** Before the next deploy, devops `start.sh` and cluster `cluster.sh` must move to the positional `efsd ADDR:PORT STORAGE` / `efsd --join SEED STORAGE` form (`--node-id` is refused), and every store needs a fresh format.

[Test hygiene: K32 entry-count flake, W149, W140](test-hygiene-k32-w149-w140-20261024.md):
**PASS (scoped) merged into devel (`e7900d8a7`).**
On nuc at `8cd6e42be`, serial `make test-all` PASS, and the summary runner
at 8 jobs reports 443 commands, 0 FAIL, 14 NOTRUN host limits (twice).

- `test_k32_entry_count` failed 40/48 on the base under the runner. The
  test was wrong: the 1 ms cohort linger is a bound with a single-ADMIT
  fallback. Its counts no longer depend on scheduling: 0/288 FAIL.
- W149: normal builds name fault variables as ignored.
- W140: a TXN_BATCH with an inner tag the apply halts on is now refused at
  the gate. The cluster gate is NOTRUN.
- The merged K33 client-view test now waits for the increment-2 joiner
  lines.

[W126 peek ordering fix](w126-peek-order-20261024.md): **PASS (scoped) merged into devel (`d5a1df25e`; tested at `680a7d0dd`).** Peeks take a
separate `peek_mu` after `view_rw`, so a writer queued on `view_rw` behind
a reader no longer holds them back (the K30 pinned case is now a liveness
test). On intel1: TSan clean on the KV tests, `make leak-check` 25/25, and
`make test-all` 442 commands with 1 FAIL and 14 host NOTRUN. The FAIL,
`test_k33_client_view.py`, fails 5/5 on base `3e01ba95c` too. The orcd-mini
A/B rerun is NOTRUN.

[orcd-mini fresh deploy of `5a7463440` (RDMA), POSIX and ior](orcd-mini-deploy-5a7463440-20261025.md):
**core 218/218, extras 4/5 + mmap SKIP, durability 27/27, `.efs` 10/10,
two-client 68/68 PASS; ior easy PASS on both clients; ior hard FAIL 1 of 4
runs on node9901 (EIO from a stale-generation REPORT, W161).** Expiry NOTRUN
(`posix_expiry.py` not ported off `--disable-efs-commands`).

[orcd-mini fresh deploy of `cb970ca7c` (RDMA, direct I/O)](orcd-mini-deploy-cb970ca7c-20261024.md):
**core 216/216, extras 4/5 + mmap SKIP, durability 27/27, `.efs` 10/10,
two-client 68/68 PASS; expiry 29/29 checks PASS** after the fixture's
formation retry (`eb8333ddc`); its closing core run failed one case (W159).

[K32 fix: batched K14 publication and K13 tickets](k32-write-fix-20261023.md):
**code fix PASS (scoped), merged into devel (`ea2dc4443`).** Merged-tree `make test-all` on intel2 (K32 + K33 at `2d03d9837`): 435 commands, 0 FAIL, 14 NOTRUN host limits (raw: intel2 `~/efs-artifacts/merge-k32-k33-20261024/`, `SHA256SUMS` `b5649ea9…8c1c`). A REPORT's consecutive units of one file publish as one
transaction, with PREPAREs in TXN_BATCH. Ticket admissions and settles are
batched per Raft group (new tags 90, 91). Counted entries per 1 MiB write:
23.0 → 3.0 at eight writes per REPORT. On amd, `make test-all` fails only
what the base fails, and the K14 torn-read gate passes 30/30. **The ior
easy write gate on orcd-mini is NOTRUN** (parent session).

[K33 placement spread for more than four nodes](k33-placement-spread-20261024.md):
**code gate PASS with host limits (amd `make test-all` at `f7553b06e`:
442 commands, 11 FAIL, all also failing on the base); branch
merged into devel (`5692db50f`); live gates NOTRUN.** The stripe start was
`fnv1a(ino) % n`; inodes of one shard differ only in high bits, so runs of
new files started on one rank (eight members: a node got nothing). Start,
packed segments and down-node spares now use fmix64 with a multiply-shift
reduction; every N = 3..16 stays within ±10 % and no 64-file window leaves
a member idle; in the daemon growth test added node 4 went from 0 to 25
new fragments. New writes only; needs a fresh format.

[K33 acceptance: node addition against the alpha checklist](k33-acceptance-20261024.md):
**code gate PASS with host limits (xorinox `make test-all` at `2ebd4fd85`:
446 commands, 0 FAIL, 13 NOTRUN); merged into devel (`cbe38a86d`); live multi-host, RDMA and
orcd-mini NOTRUN.** Loopback daemon tests for four checklist items: additions
interrupted at every durable handoff (new hooks) and concurrent interrupted
joins converge to one membership without duplicate ids or orphaned
fragments; packed and unpacked payloads exact across 3→4→5 under a writer,
a whole-cluster restart and each single-member outage at five; seed loss
with joins through other members, unreachable members kept; 3→6 capacity,
`df`, protection and abandoned-upload reporting, automatic quota, reclaim
and W154 rebuild. Fixed: reclaim stalled after growth (a retired group-2
replica waited on its own apply) and a whole-cluster restart waited 30 s
per starting peer. **Oct 25:** the payload test's single-member outages
waited on `efs-mgmt status` counts that lagged a degraded segment write by
up to a GC pass (read `EIO`, base 8 of 30 on amd); fixed by
[K38](k38-payload-protection-now-20261025.md), payload 30 of 30.

[K33 live client view refresh](k33-client-view-20261024.md):
**code gate PASS with host limits (intel1 `make -k test-all` at `8b4cfbe77`,
exit 0); merged into devel (`f00cfdd75`); live
addition under client load and RDMA NOTRUN.** A mounted FUSE client reads
the committed membership record (`MEMBERSHIP_READ`, wire types 245/246),
installs later revisions without a remount, refuses another cluster or a
divergent record, places new writes on added members and keeps old data's
recorded placement.

[K33/W157 automatic formation and node addition](k33-w157-formation-addition-20261023.md):
**model PASS; implementation merged (`0098e06c4`;
increment 1, operator node ids; Raft tags renumbered to 92, 93 at merge;
merged-tree `make test-all` on intel2 0 FAIL), code gate PASS with host limits (xorinox `make test-all` at `0098e06c4`: 433 commands, 0 FAIL, 13 NOTRUN); live addition
gates NOTRUN.** Three joined members form without `raft-mkfs`; one or two
stay unready; a node with the next id joins a live cluster by `--join` any
member: a committed membership revision (store tokens, one cluster
identity), group 2 moves {1,2,3} → {2,3,4} by joint consensus with
catch-up, later nodes add storage only, old data keeps its recorded
placement. [Protocol](k33-membership-protocol-20261023.md) (M1–M9).
Live client view refresh: [above](k33-client-view-20261024.md).

[K33/W157 increment 2: automatic node ids, positional CLI, discovery](k33-w157-autoid-discovery-20261024.md):
**code gate PASS with host limits (xorinox `make test-all` at `53080d744`:
436 commands, 0 FAIL, 13 NOTRUN); model PASS; merged into devel; live multi-host, cross-subnet,
RDMA and orcd-mini gates NOTRUN; needs a fresh format.** `efsd ADDR:PORT
STORAGE` creates a cluster or restarts; `efsd --join [SEED] STORAGE` joins
through a member or a cluster found on the local subnet; no node id (the
seed is 1, formation allocates by promise, additions take `count + 1`).
Open finding: placement collapses for power-of-two member counts.

[W153 committed membership](w153-committed-membership-20261023.md): **code
fixed and merged (`4026e254d`); merged-tree `make test-all` on intel2 0 FAIL;
live formation and failover gates NOTRUN; needs a fresh format.** `raft-mkfs` commits the
joined membership in both groups; count and voters come from it on every
start; `EFS_MD_RAFT_N` removed; joins after formation refused (since
K33/W157 on its branch: added as committed revisions).

[Alpha 1.0 simple startup and node addition](alpha-1.0-node-addition-20261023.md):
**ACCEPTED; implementation OPEN, gates NOTRUN.** Minimal seed/join startup,
automatic identity and formation, safe live additions, always 2+1; removal
deferred. Supersedes mandatory cluster files and fixed-membership-only D39/D40.

[W158 ticketed live harnesses](w158-ticketed-harness-20261023.md):
**PASS, merged (`33c68a465`); intel expiry rerun passes every expiry check (fixture POSIX timeouts are W159).**
`gc_reclamation.py` and three wire harnesses send the K13 ticket header and
prove that an unticketed PUT is refused; a static `make test` check guards
it. nuc `make test-all` PASS.

[K13 / D31 dead and departing clients](k13-d31-drain-20261024.md):
**PASS (scoped, unit and in-process) merged into devel (`e6a21692d`; tested at `59a2308b8`); amd `make
test-all` FAILs (RDMA, one pool timeout) also fail on the base; live gates
NOTRUN.** After revocation the group leader drains a dead client's ticket
streams into the orphan ledger (anchored stream index, DRAIN refused while a
K14 stage is pending), and a clean unmount revokes its own session. The
unused RECLAIMING path is gone. New numbers: KV kind 250, Raft tag 250.

[K13 / D31 abandoned-upload collection](k13-d31-collection-20261023.md):
**PASS (scoped, unit and in-process) on branch `devel-k13d31`, not merged.**
The group leader deletes a settled abandoned body on every member that holds
a fragment, after the session's revocation is complete and behind each
member's storage floor, and retires the record only when every fragment is
acknowledged. Cluster gates are NOTRUN, and unsettled tickets of dead clients
are not collected yet.
[K29 append stall under load](k29-append-stall-20261023.md):
**cause established; two fixes on branch `claude/devel-k29-2185a1`, not
merged; `make test-all` PASS on xorinox; load verification NOTRUN (orcd-mini).**
Reservations and barrier retirements proposed one Raft entry per active
writer lane (up to 139 group-0 entries per 4 KiB record), now one TXN_BATCH
entry per group (11 per record); commit probes no longer overtake their
batches (`prev-beyond-log`).

[K13 / W86, W88, W137 ticketed PUT/REPORT default](k13-w86-ticket-default-20261023.md):
**PASS (scoped) on branch `devel-k13`, not merged.** Every data PUT and REPORT
record is bound to an admitted ticket; the W88 retry is refused by ticket
state; W137 uses the real reply shape. `make test` and a private three-daemon
gate pass on intel1. The D31 abandoned-upload collector is INCOMPLETE; RDMA,
orcd-mini and multi-host are NOTRUN.

[Full code-gate runtime review](make-test-runtime-review-20261023.md):
execution review complete; speedup NOTRUN. Fix the fault-daemon dependency,
then build harnesses incrementally and schedule isolated cases concurrently.
The fast/long split does not reduce full-gate work or qualify parallel safety.

[W155 per-request receipts and same-identity resolution](w155-ambiguous-completion-20261024.md):
**code fix PASS (scoped), branch merged into devel (`3e01ba95c`; tested at `b36710b65`); `make test-all` on nuc PASS (the one parallel-runner FAIL,
`test_k32_entry_count`, fails the same way on the base); nuc b1/b2 live reruns NOTRUN.** A REPORT runs under a
receipt keyed by session and publication id; a sync REPORT or identified
directory op whose reply was lost is resolved by identity before failing, and
EIO remains only for an outcome still unknown after the bound. Client and
server lines carry the request identity (G1–G6).

[W155 modify oracle and correlation tooling](w155-oracle-correlation-20261023.md):
**self-tests PASS on nuc; the W148 campaign rerun with it is NOTRUN** (branch
`devel-w155h`, merged Oct 23 `92886b8e2`). A failed modify records its syscall stages and
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
leak found. The W126 peek case it skipped now runs (see the W126 peek fix).
node9901 run NOTRUN.

[K30 intermittent tests](k30-intermittent-tests-20261023.md): **fixed on
`devel-k30`, merged Oct 23 `b64dbe934`; 0/25 FAIL per case on amd.** `test_reclaim` was a
product bug (same-second journal aside rename refused startup); the fork
oracle and `test_kv_lsm` were test races. The W126 peek limitation is fixed (W126 peek fix above).

[W156 client discovery](w156-client-discovery-20261023.md): **fixed on
`devel-w156` (`dca4dd544`), not merged; `make test-all` on nuc PASS without a
switch.** Mount owner from mountinfo, controller proven by the control
endpoint and `/dev/fuse`; foreign processes are never inspected;
`EFS_CLIENT_SKIP_FOREIGN` removed. Live foreign-client case NOTRUN.

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

[D39 admission implementation](d39-admission-20261023.md): **historical;
merged via `246bae7cf` and reverted on devel (`d7bca79d6`) by the user's
Oct 23 decision.** devel has no `--cluster`, no `cluster.conf` and no
admission layer; startup is seed/`--join`. The store lock and durable boot
counter returned with W153; formation and node addition are K33/W157. The
page records what the reverted branch did.

[D39 consolidated model](d39-consolidated-model-20261023.md): **MODEL PASS on
branch `devel-d39model`, merged Oct 23 `16d716308`.** Historical fixed-membership model; superseded for startup and membership growth
by the accepted node-addition amendment. `tests/d39_model.py` in `make test`
checks 101 traces from §§8–33, 11 negative controls under superseded rules and a
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

[Intel deploy of devel `a23d45540`](intel-devel-a23d45540-20261023.md): **fresh format; make test-all 428/0 FAIL; core 216/216 (both mounts), two-client 68/68, durability 27+27, extras 4/5 (mmap skip); expiry fixture: first run stopped at a stale harness step (W158, fixed); rerun passes every expiry check, closing fixture POSIX 214/216 with two timeouts (W159).** Eight live harnesses still expected protocol 4 (fixed `2791f211d`).

[Simple startup and node addition](alpha-1.0-node-addition-20261023.md): **user decision Oct 23: the `--cluster` file feature is removed; the D39 admission merge is reverted on devel; seed/`--join` is the only startup.** Alpha needs W153 (member count and voters from the committed membership, formation at three) and then node addition (K33/W157).

[K32 write regression](k32-write-regression-20261023.md): **OPEN; code fix on a branch ([fix evidence](k32-write-fix-20261023.md)).** orcd-mini (devel `757d76844`, fresh format) passes but ior easy write fell to ~0.36–0.5 GiB/s: K14 publishes every multi-chunk write as its own cross-group transaction (~20 Raft entries per 1 MiB write), K13 adds ~9.

[K31 fixed, K27 closed, orcd-mini on devel `24e58fb1d`](k31-k27-orcd-mini-20261023.md): **RDMA reply loss came from the K5 mount session starting before the daemon fork (poller thread lost); fixed in `24e58fb1d`. K27: no ior write regression on fresh formats. All orcd-mini suites PASS.**

[D40, alpha 1.0 scope decisions Oct 23](decisions.md#d40--alpha-10-scope-decisions-taken-oct-23-user-in-one-pass):
**D39 amended (simple startup, automatic identity, online addition; removal deferred; model first); W154 rebuild on return; K10 known limitation; K13 and K14 fixed in alpha; K23 stays deferred; orcd-mini reformat and the K27 A/B approved.**

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
  `/data1/efs/evidence/testsuite` were deleted at the user's direction.

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

**Raw evidence note (2026-10-23):** the bulk data in `testsuite` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
