# Acceptance definitions for nine K rows, October 31

**Purpose:** give K2, K4, K11, K16, K17, K19, K45, K46 and K47 a named, runnable acceptance each, or a written decision for the user, so the D46 campaign can schedule and close them. Planning only: no code ran on a cluster for this page, it records no PASS, FAIL or NOTRUN verdict, and no row changes status.

**Linked items:** [Stage 1 acceptance matrix](../stage1-acceptance-matrix-20261030.md) (Task BK found these rows undefined), [Section K register](../alpha-1.0-2plus1-checklist.md#k-stage-1-known-defects-in-the-alpha-scope), D45 item 5 and D46 in [decisions](../decisions.md), the project rule *No backward compatibility* in `AGENTS.md`.

**Source state:** devel at `909a07d71` (wave 45 merged). Every command below was read from the tree at that commit; none was run on Linux or on a cluster (this task ran on the Mac).

**Result in one table.** "New" means a new test or script; none was needed, so no `tests/budgets.tsv` row was added and the recipe bound (1,750 s) is untouched.

| K | Acceptance on the frozen candidate | Where | Duration | New? | Decision for the user |
| --- | --- | --- | --- | --- | --- |
| K2 | `test_decoder_inputs.py` + `test_decoder_refusal.py` in `make test-all` (F1); `make decoder-check` (new matrix run F5); `c5-protocol-live` tag refusals in every cell | loopback; every cell | F5 about 135 min | no | BT-1: no distinct-host or RDMA decoder run |
| K4 | `test_declaration_gate` + `test_declaration_gate_handler.py` (F1); `c5-protocol-live` `wire` checks in every cell; `w125_p3_compat.sh` superseded | loopback; every cell | seconds per member | no | none: contradiction resolved |
| K11 | `gc-reclamation` "failed server PUT barrier" case (F4); `e1-manifest`, `e4-scale`, `posix-durability` cold-read gates in every cell | loopback-5; every cell | F4 30 min; cells as-is | no | BT-2: historical NFS/macOS diagnosis |
| K16 | `test_frag_record`, `test_store_durability`, `test_get_integrity.py` (F1); `fragment-integrity` (F4); RDMA cell reads | loopback-5; RDMA cells | F4 30 min (gate 10 s) | no | BT-3: RDMA negative path |
| K17 | `scripts/orcd_mini_c1.sh 3600 ...` (O3, now 70 min) | orcd-mini | 70 min | no | BT-4: the three "remaining gates" |
| K19 | `test_reclaim`, `test_store_gc`, `test_meta_apply` (F1); `gc-reclamation` (F4) | loopback-5 | F4 30 min | no | BT-5: historical reconciliation, live fault |
| K45 | `./tests/test_raft` (F1); H15 snapshot import plus a `storage fenced` log scan | loopback; distinct-host | H15 as-is | no | none |
| K46 | `./tests/test_raft` (F1); H3 `test_w130_dir_fault.py` as confirmation | loopback; distinct-host | H3 as-is | no | BT-6: live is not gating |
| K47 | `tests/test_get_integrity.py` (F1); `test_k36_segment_recovery.py` (F1); H4/H5 member-return reads | loopback; distinct-host | H4, H5 as-is | no | BT-6 (same text) |

## Run changes this page makes to the matrix

- **O7 removed** (K4's `w125_p3_compat.sh` hardware run): superseded, see [K4](#k4).
- **O3 grows from 30 to 70 min** (K17): the reference C1 run is 3,600 s.
- **F5 added** (K2): `make decoder-check`, about 135 min, loopback, no campaign host.
- **F4 widened** (K11, K16, K19): `gc-reclamation` plus `fragment-integrity` on loopback-5, still 30 min.
- Log scans (K11, K45) are one-line `grep` commands run over the raw output of runs the matrix already schedules; they add no time. Command: `grep -rEl 'storage fenced|fetch published .* rc=-9' <run output dir>` must print nothing.

The matrix sequential total moves from 3,810 min to 3,955 min (65 h 55 min, about 2.7 days).

<a id="k2"></a>
## K2 — proposal RPC accepts tags this build cannot apply (W140)

**Observable.** A client or follower submitting a metadata command whose tag the build cannot apply got it replicated, and every replica then halted the group at apply; the fix refuses unknown tags at submit with PROTO, adds no log entry, and keeps the group serving.

**Acceptance.**

1. *F1, existing:* `python3 tests/test_decoder_inputs.py` (in `make test`) and `python3 tests/test_decoder_refusal.py` (in `make test-long`, budget 10 s). The second starts three private loopback daemons, submits unknown tags through leader and follower, checks no replica log holds the tag, no group halts, and a following CREATE commits.
2. *Overall decoder qualification, existing script, new matrix run F5:* `make -j4 all decoder-bins` then `CC=clang python3 scripts/decoder_check.py --jobs 8 --seconds 1200 --extended-seconds 7200 --out "$HOME/efs-artifacts/<run>"`. PASS is rc 0 for all 25 targets (24 record targets plus apply), the [Task C](task-c-20261025.md) result being 25/25 on `eb936c71f`. Prerequisites: a Linux host with clang and sanitizers (intel1 class). Raw output stays outside Git.
3. *Every cell, existing:* `c5-protocol-live` runs the `wire` helper (`scripts/stage2_helpers.py`, `wire_node`) on every member and checks each unsupported tag is refused `ERR_PROTO` through leader and follower (`...-tag-<tag>-group-<g>-refused`).

**Where / duration.** F1 and F5 on any Linux loopback host, F5 about 135 minutes (an estimate: the declaration and apply targets run at least 7,200 s each and are the floor; Task C used `--jobs 4`). The cell check is inside the 60-minute cell.

**New?** No.

**What dropped.** The matrix read Task C's "remaining named boundaries" as a list of uncovered validators. The sentence in the evidence page lists the pure validators the mutation gate *does* cover (store identity, HELLO, recovery envelopes, audit, degraded marks, D31 drain, PUT header, E2 probe); I found no named uncovered boundary. H3 therefore no longer carries the decoder half. Decision [BT-1](#bt-1) covers the rest.

<a id="k4"></a>
## K4 — client protocol declaration gate is fail-open (W133)

**Observable.** A connection that never declared the protocol, or declared a different version, was served (any request but CLIENT_HELLO and HEARTBEAT worked); after the fix an undeclared request is closed with no reply and a wrong declaration is answered `ERR_PROTO` and closed.

**The contradiction is already resolved in the tree.** The matrix, and the W133 item's first paragraph, say `scripts/w125_p3_compat.sh` expects an old client's writes to keep working. That is the *pre-fix* description. Commit `543255776` ("K4 / W133: fail-closed client protocol declaration gate, exact protocol 3") rewrote the script, and its header and checks now say the opposite: case 2 requires the old client to be *refused* (the mount fails, or every read and write fails, no file is written, and every daemon logs `handler: refused`), and case 3 requires the new client to refuse old daemons. The W133 item itself records "`w125_p3_compat.sh` now expects old clients to be refused". Nothing in the script asserts backward compatibility, so there is nothing to rewrite for the rule. The stale paragraph in `docs/backlog/work-items.md` is left as history (it is outside this task's files).

**Acceptance.**

1. *F1, existing, in `make test`:* `./tests/test_declaration_gate` (undeclared request refused with no reply, HEARTBEAT served, wrong, short and bad-magic declarations, an accepted declaration then a served request, client-side refusal) and `python3 tests/test_declaration_gate_handler.py` (gate precedes dispatch on every frame; every connection opener declares; no minimum-version range anywhere). It asserts protocol 5 today.
2. *Every cell, existing:* `c5-protocol-live`, whose `wire_node` checks on every member: `undeclared-heartbeat-served`, `undeclared-request-closed-without-reply`, `older-declaration-answered-proto` (declaration of protocol − 1 gets `ERR_PROTO`), `older-declaration-then-closed`, `bad-magic-declaration-refused`, `current-declaration-accepted`, `declared-heartbeat-served`. This is the fail-closed refusal the rule requires, on the real hosts, in all six cells.

**Superseded.** `scripts/w125_p3_compat.sh` is **superseded as the K4 acceptance** by items 1 and 2 and is not deleted. Reasons it cannot be scheduled: it needs a retained older release (`OLD_REL`) with a different protocol number, it hard-codes `EFS_TRANSPORT=rdma`, `mlx5_0` and node9901 addresses, and it needs a 1,000-file positive case that `e1-manifest` already covers. What it alone shows is the refusal over a real RDMA connection; the gate function (`efs_decl_gate_frame`) is the same on TCP and RDMA, and every RDMA cell shows the positive path (RDMA clients declare and are served). If the user wants the RDMA refusal observed, it is a one-off run with an `OLD_REL` that someone must retain; this page does not schedule it.

**Where / duration.** F1 anywhere; `wire` takes seconds per member inside `c5-protocol-live` (deadline 1,800 s for the whole gate, already counted in the cell).

**New?** No. **Matrix effect:** O7 removed.

<a id="k11"></a>
## K11 — span committed but fragment PUTs never landed (W55)

**Observable.** A span committed in Raft names generations whose fragment objects were never written; every later read of that chunk fails `EFS_ERR_DECODE` (-9), logged as `fetch published ino=… rc=-9`, and the data is lost.

**Acceptance.** The invariant to prove on Linux FUSE is "a committed span always has its fragments, so a cold reader gets the exact bytes", checked three ways:

1. *F4:* `python3 tests/live/gc_reclamation.py --put-barrier-fault` (run by the driver as the `gc-put-barrier` command of the `gc-reclamation` gate: `python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json --topology loopback-5 --gates gc-reclamation,fragment-integrity --allow all --out OUT`; the script has no `--mode`, direct I/O is its only mode). Its case "failed server PUT barrier: no publication ACK, retained bytes, recovery and exact cold bytes" fails the PUT leg with a test-only `syncfs(storage root)` refusal (the batched barrier, [page](k11-put-barrier-case-20261031.md)) on one, two and all members: one failing member still ACKs `fsync`; two or more get EIO, the bytes stay readable, and after an orderly restart a fresh mount reads them exactly. The span is published before `fsync` proves it (K35), so the case records its state instead of requiring absence.
2. *Every cell, existing:* `e1-manifest` (acknowledged-file manifest verified after orderly stop/start), `e4-scale` (every file exact on a cold reader), `posix-durability`.
3. *Log scan over those runs' client logs:* `grep -rE 'fetch published .* rc=-9' <run output>` must print nothing.

**Where / duration.** Item 1 on loopback-5, about 30 minutes (the gate measured 280 s in rehearsal; 30 minutes is the matrix's own allowance). Item 2 inside every cell. Item 3 free.

**New?** No. Decision [BT-2](#bt-2) closes the unreproducible part.

<a id="k16"></a>
## K16 — identity-bound fragment integrity format (W75)

**Observable.** A fragment record whose identity or payload digest does not match its place is refused as INTEGRITY on GET and by GC and segments, never served; a root in an older format refuses startup.

**Acceptance.**

1. *F1, existing:* `./tests/test_frag_record` (each identity field wrong is refused; flipped and truncated payloads refused), `./tests/test_store_durability` (`test_root_format`: a format-0 root with fragment data refuses startup), `./tests/test_store_gc` (both modes), `python3 tests/test_get_integrity.py`.
2. *F4, existing:* `python3 tests/live/fragment_integrity.py` (gate `fragment-integrity`, 10 s on amd): disposable three-member cluster, `O_DIRECT`, one and two roots: ticketed PUTs, then a GET of a damaged fragment record must answer INTEGRITY (reply status 3) and never serve the bytes. The cluster-level client behaviour (other fragments used, EIO when two are wrong) is recorded on the [K16 page](k16-w75-identity-format-20261022.md) from a one-off run, not from this gate.
3. *Fresh format:* every campaign cell is deployed fresh by the project rule, so each cell is a fresh-format run of the identity-checked path; RDMA cells read every fragment through it.

**Where / duration.** F1 anywhere; F4 on loopback-5, 30 minutes (the gate itself 10 s); RDMA is covered by the existing RDMA cells at no extra time. The performance A/B the page lists is not a gate here: the write-side figure is K32's O2 run.

**New?** No. Decision [BT-3](#bt-3) states the RDMA scope.

<a id="k17"></a>
## K17 — STALE replay convergence (0a / D27)

**Observable.** Under modify-under-load with two clients, a publish went STALE and never converged: `publish-stalled` or `flush-meta` lines, write-path EIO, and read mismatches (the unfixed orcd-mini run had 31 errors, 4 mismatches; the private unfixed run 2 stalled inodes and 11 EIO in 15 min).

**Acceptance.** The C1 workload named by [the fix page](w0a-lane-epoch-lag-20261021.md) as "the acceptance gate": `scripts/orcd_mini_c1.sh 3600 <WAIT_A> <WAIT_B>` (arms node9901 as client A with 4 writers, 4 readers, 2 deleters, 4 modifiers and a 60 s `pack-compact` loop, and node9900 as client B with 4 readers and 2 deleters). **PASS:** no `publish-stalled` or `flush-meta` line in either client's output, `errors=0` on both, no `pub-stale` refusal, and `crosscheck.txt` ends `rc=0` (every file A lost was deleted by B, none reappeared). Prerequisites: the frozen candidate deployed on orcd-mini, the user's go-ahead for orcd-mini (the same one as O1–O9), the two WAIT files so it follows the preceding run. Raw output stays on the nodes under `prof/c1-<tag>`; the summary records the tag and hashes.

**Where / duration.** orcd-mini RDMA, first in D45 item 5's order. **70 minutes** (3,600 s run plus arming and verify; the matrix said 30, an estimate with no basis in the script, whose reference run is one hour).

**New?** No. Decision [BT-4](#bt-4) drops the unnamed "strict timing, fault/recovery/contention and RSS" gates.

<a id="k19"></a>
## K19 — GC final drain and lost-ledger reconciliation (W61–W63)

**Observable.** Local GC discarded a failed lane-sweep apply verdict (W61) or lost delta records at batch boundaries (W62), so a reap marker was dropped and fragments were never physically reclaimed; backlog and progress were invisible (W63). The incident behind it, 40 GiB reclaimed on xorinox on Oct 5–7, was never given a cause.

**Acceptance.**

1. *F1, existing:* `./tests/test_reclaim` (reap and journal under I/O errors: a refused write leaves the inode, `BUSY` is retried later, the journal cap refuses instead of dropping records), `./tests/test_store_gc` and `./tests/test_store_gc unlimited` (injected I/O faults on delete, gc_inode), `./tests/test_meta_apply` (W62 cases: exact dead identity, sweep and truncate across batches).
2. *F4, existing:* `gc-reclamation` on the alpha profile (direct I/O, 2+1, four nodes): physical reclamation after delete, restart, and "offline fourth member retains exact durable reap ledger". This is the final-drain and ledger check.

**Where / duration.** F1 anywhere; F4 on loopback-5, 30 minutes (K42 already names it). The matrix's H6 90-minute distinct-host run stays for K18 only.

**New?** No. Decision [BT-5](#bt-5) states what no existing test observes.

**One thing to know first.** `gc-reclamation` was FAIL in Task A's runs because of the K42 clean-stop BUSY; the loopback-5 rerun after that fix is still NOTRUN per the [K42 page](e2-clean-stop-busy-20261025.md). F4 may fail for that reason, not for K19.

<a id="k45"></a>
## K45 — delayed AppendEntries below the snapshot (Raft)

**Observable.** After a follower installed a snapshot, a delayed AppendEntries whose previous index lay below it permanently fenced the follower's storage (`raft: group=… storage fenced at=on_ae_req`).

**Acceptance.**

1. *F1, existing, `make test`:* `./tests/test_raft`, case `test_snapshot_rejects_delayed_compacted_prefix` (the request is refused with the snapshot boundary as the rebase hint, nothing appended or applied, and a valid suffix still replicates).
2. *Live, existing run plus a check:* H15 (`python3 tests/test_k22_snapshot_import.py` and `scripts/k22_snapshot_import.sh`, a follower installing a snapshot on distinct hosts), and the same check over every campaign run: `grep -rl 'storage fenced' <run output>/**/efsd*.log` must print nothing.

**Why distinct-host is enough.** The refusal sits in the receive path above the transport; a delayed packet exists on TCP and RDMA alike and loopback already reproduced it. The live run adds real timing, not a new code path.

**Where / duration.** F1; H15 as scheduled (90 min × 2). **New?** No.

<a id="k46"></a>
## K46 — `raft-resync` replica could win an election

**Observable.** A replica with resync requested could become leader in the next term (`resync=1 role=2 term_before=1 term_after=2`) through a queued leadership transfer or delayed vote grant.

**Acceptance.** *F1, existing:* `./tests/test_raft` cases around `efs_raft_resync_pending` (no timer, pre-vote or transfer election while resync is pending; an in-flight candidacy and read proof cancelled; eligibility returns after install). *Live confirmation, existing:* H3's `python3 tests/test_w130_dir_fault.py`, which resyncs a replica with `raft-resync` and requires the group to keep serving and the next write to land. See [BT-6](#bt-6).

**Where / duration.** F1; H3 as scheduled (30 × 2). **New?** No.

<a id="k47"></a>
## K47 — packed read with a returned holder answering NET returned EIO

**Observable.** A packed read where one down-marked holder had returned, one fragment was valid and one was absent, returned `EIO` instead of the bytes, because the returned-holder re-probe ran only when the last verdict was NET.

**Acceptance.** *F1, existing:* `python3 tests/test_get_integrity.py` (all six holder/absence orders), and `python3 tests/test_k36_segment_recovery.py` (the readback after a compaction crash where Task J found it; budget 100 s). *Live, existing:* H4's member-return reads and H5's restart-and-read-back must show exact bytes and no `EIO` for every packed file; the cell gate `b6-member-down` does the same for non-packed files. See [BT-6](#bt-6).

**Where / duration.** F1; H4 (120 × 2) and H5 (60 × 2) as scheduled. **New?** No.

## Decisions for the user

Each is a recommendation; no row changes until the user answers. The rows keep their status words.

<a id="bt-1"></a>
**BT-1 (K2): decoder qualification stays loopback and sanitizer; no distinct-host or RDMA decoder run.** *Recommend yes.* Text: "K2 is accepted by F1 (`test_decoder_inputs.py`, `test_decoder_refusal.py`), the F5 decoder-check run (25 of 25 targets), and the `c5-protocol-live` tag refusals in every cell. Decoders are pure functions of the bytes and the submit refusal sits above the transport, so a distinct-host or RDMA decoder campaign is not required." Alternative: keep an H3 distinct-host run (+30 min × 2) for the submit gate only.

<a id="bt-2"></a>
**BT-2 (K11): the historical macOS NFS incident is not an alpha acceptance.** *Recommend yes.* Text: "W55's original trigger (macOS NFS re-export, xorinox, Oct 5) was never isolated and NFS/macOS is out of alpha (W152). K11 closes on the Linux FUSE invariant: F4's failed-PUT-barrier case passes, every cell's `e1-manifest`, `e4-scale` and `posix-durability` pass, and no `fetch published … rc=-9` appears in any client log. Any recurrence reopens K11." Alternative: leave K11 VERIFY until the NFS reproduction exists (beta).

<a id="bt-3"></a>
**BT-3 (K16): RDMA acceptance is the positive path.** *Recommend yes.* Text: "The identity and digest check runs in the store before any transport; the INTEGRITY refusal is exercised on TCP by `fragment-integrity` (F4) and the unit tests. RDMA cells prove the same checked GET serves every read. A separate RDMA corruption run is not required." Alternative: extend `tests/live/fragment_integrity.py` to RDMA (it is TCP-only today; the RDMA live harnesses are already a known gap in the matrix).

<a id="bt-4"></a>
**BT-4 (K17): C1 alone closes K17.** *Recommend yes.* Text: "0a's acceptance is the orcd-mini C1 rerun (`scripts/orcd_mini_c1.sh 3600`) with the PASS criteria above. The 'strict timing, fault/recovery/contention and RSS' gates in the work queue have no script and no owner; they are not alpha gates for K17 and stay with the 0a backlog item." Alternative: name each of the three gates and a script before Stage 1 closes.

<a id="bt-5"></a>
**BT-5 (K19): two parts of the row are not acceptance-testable.** *Recommend yes.* Text: "(a) 'Lost-ledger reconciliation' of the Oct 5–7 xorinox incident cannot be reproduced: its raw logs were never inspected here and the store predates the fresh format. K19 is accepted on the unit tests and F4's ledger-retention case, with the incident left as an unexplained historical observation. (b) No live test injects a BUSY or I/O error into a faulted sweep apply and checks the reap marker survives; `test_reclaim` and `test_meta_apply` cover it in process, and I did not trace whether verdict-ring eviction and term changes are among their cases. Accept the in-process coverage for alpha." Alternative: write a live fault-injection gate (a new test; 1 day of work, not done here).

<a id="bt-6"></a>
**BT-6 (K46, K47): the exact race is deterministic-only.** *Recommend yes.* Text: "K45 is also observed live by the `storage fenced` scan. K46's election race and K47's NET-plus-absent-fragment race need exact message ordering that no live run can force; they are accepted on `./tests/test_raft` and `tests/test_get_integrity.py` (all six orders), with H3, H4 and H5 as incidental live confirmation that no group stops serving and no packed file reads EIO."

## Limitations

No command here ran on Linux or a cluster; durations marked as estimates are estimates; F5's 135 minutes is an extrapolation from Task C's per-target times. I did not run `gc-reclamation` or `fragment-integrity` to check whether they pass today. The log-scan commands depend on each run keeping its daemon and client logs; the qualify driver keeps them under the run output, but I did not verify the file names. PASS on any acceptance here closes only the K row's owed acceptance; FIXED is not VERIFIED until the campaign runs on the frozen candidate.

## Reproduction

Regenerate the `Acceptance` references: `grep -n 'Task BT' docs/status/alpha-1.0-2plus1-checklist.md docs/status/stage1-acceptance-matrix-20261030.md`. Check the cited commands exist: `python3 scripts/qualify.py plan | grep -E 'c5-protocol-live|fragment-integrity|gc-reclamation'` and `grep -nE 'decoder-check|test_declaration_gate|test_decoder_refusal' Makefile`. Checks run on the Mac for this page: `python3 tests/test_make_test_recipe.py` PASS (101 budget rows, 1,732 s of the 1,750 s bound, unchanged: no test or budget row was added); `python3 docs/check-architecture.py` PASS after regenerating `docs/how-it-works/architecture-full.md` and `architecture.html` (the status README is an input). `make test` is NOTRUN here: it does not build on macOS (`src/common/log_ts.c` uses Linux `fopencookie`).
