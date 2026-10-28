# User documentation audit, round 3 — October 27–28, 2026

Task Q · branch `agent/devel-task-q`, base `e7e72e2674e892e3667c3dbdb1a53eb8911b644c` (wave 22). Docs/static gates PASS; Mac build FAIL (Linux prerequisite); every live command/failure row below NOTRUN. No product changes, merge or push. K43/K48/W166 remain product gaps; stale resync CLI diagnostic reported. Evidence: this page.

## Verdict and method

**Documentation audit COMPLETE, runtime qualification NOTRUN on this Mac.**
This is the task's explicit fallback when the Linux tree does not build on
macOS. It is not a fresh `make test-all`, POSIX, expiry or recovery PASS.
The six requested pages and all L1–L29 limitation rows were checked against
merged waves 9–22, the decision register and current source. Seventeen docs
findings are corrected; four product/diagnostic findings remain reported,
without weakening the required behavior. L22 and L24 remain open user decisions.
The [round 2 audit](docs-audit-20261025.md) remains historical evidence.

No Linux host, deployed cluster, external devops repository, daemon, mount,
store or raw campaign evidence was changed. No compatibility path, migration,
format, feature flag, timeout or acceptance threshold was added.

Environment: macOS 26.6.2 (25G83), Darwin 25.6.0 arm64, Apple clang 21.0.0,
Python 3.9.6, pkg-config from Homebrew. Local fuse3 pkg-config reports 3.18.2;
libibverbs pkg-config is missing. The supported runtime remains **Linux
FUSE with O_DIRECT**, not a macOS client. `make -j4 all` fails (rc 2) in
`src/common/log_ts.c`: `cookie_io_functions_t` and `fopencookie` are glibc
APIs unavailable here. No EFS binary identity or loopback cluster exists
from this attempt. Installing more macOS packages would not qualify Linux
pidfd/FUSE/verbs behavior. No remote run was substituted for the task's
Mac-only scope.

Read-only source checks covered `include/efs/cli_help.h`,
`src/mgmt/efs_mgmt.c`, `src/server/raft_host.c`,
`src/common/session_revoke.c`, `src/client/inode_rpc.c`, the Makefile,
`tests/budgets.tsv` and summary runner, with the published source-specific
evidence below. Graphify's existing index located related pages; current
source and merged evidence, not inferred graph edges, decide the findings.
No external raw capture was downloaded or independently revalidated here;
remote results below remain **reported, scoped evidence** with their original
failures and limitations.

## Findings

U numbers in this table are local to **round 3**; earlier audit U numbers
retain their own meanings.

| ID | Kind | Finding and correction | Disposition / evidence |
| --- | --- | --- | --- |
| U1 | Docs | Four alpha pages still claimed October 25 binary checks at `ec3e09306`. Updated to the pinned wave-22 source/evidence review and explicit Mac runtime NOTRUN. Historical execution is not relabeled. | FIXED: guide, surface, POSIX, release notes |
| U2 | Docs | Guide's “make test, about a minute” obscured the split and full gate. Documented summary runner, D44 1,100 s target excluding build and INCOMPLETE when NOTRUN remains; testing page records the host-specific three-run refit. | FIXED: guide/testing; [Task J](gate-budgets-refit-20261026.md) |
| U3 | Docs | Two Expiry sections created duplicate anchors and conflicting activation examples; the first also said every expiry command is refused while off, which cannot include cluster-policy get/set. Retained one complete D43 section with cluster get/on/off, folder policy and coverage semantics. | FIXED: guide; CLI `cmd_expiry_cluster` / `cmd_expiry` and D43 |
| U4 | Docs | Formation text assigned all ids by endpoint order, contradicting the first node's fixed id 1. State that the cluster allocates joiner ids; no operator-assigned id. | FIXED: guide; shared CLI help and K33 |
| U5 | Docs | Stop runbook omitted K42's certified no-op waits, making a transient cleanup refusal look terminal. Added same-session bounded 20 s per-shard retries, refusal diagnostics, and explicit distinction from the still-unresolved all-member storage fence. | FIXED: guide; [K42](e2-clean-stop-busy-20261025.md), `session_revoke.c` |
| U6 | Docs | Addition paragraph said all orcd-mini addition gates NOTRUN and omitted retired-seed management behavior. Recorded wave-18 routing and scoped RDMA sweep; old “target a live member” workaround labeled historical, never a reason to reformat. | FIXED: guide/release notes; [K44](k44-retired-replica-routing-20261026.md) |
| U7 | Docs | Operators had no W130/W165 stored-decoder halt/recovery instructions. Added evidence preservation, per-group status, non-retired follower resync, asynchronous acceptance versus completion, exact readback and no format fallback. | FIXED: guide/surface/commands/testing; [Task F](w165-serve-path-damaged-row-20261026.md), [Task B](k22-k6-loopback-taskb-20261025.md) |
| U8 | Docs | POSIX P13 still called the W159 apply-wait fix unmerged. Recorded wave 9 progress-bounded wait and wave 22 K50 deadline-bounded retry fix, while retaining the broader loaded-case failures OPEN. | FIXED: POSIX/release notes/testing; [Task O](dir-ops-busy-under-load-20261027.md) |
| U9 | Docs | POSIX results stopped at October 24 and said every operation passed in “both” runs. Added source-specific Task K/M/O rows, explicit query skips and scope; no C1 distinct-host or persistence PASS inferred. | FIXED: POSIX; 218 core / 5 extras / 68 peer / 27 persistence source inventory unchanged |
| U10 | Docs | Release open list missed K43/K48 and recent merges, and still said all K15 live gates NOTRUN. Replaced with current blockers and scoped K15/K29 warm/peer/cold results; K38/K40 merges do not establish all W166 scopes. | FIXED: release notes; [Task N](k15-k29-append-gates-20261027.md), [Task G](k38-task-g-repair-20261027.md) |
| U11 | Docs | L27/L28 and POSIX P15 called F22/F23 “proposed” despite the user's D42 decision. Marked decided; implementation and scope limits retained. All L1–L29 rows reviewed; L22/L24 remain OPEN. | FIXED: release/POSIX; decision register D42 |
| U12 | Docs | Surface intro/S4 still implied shipped A/B selectors pending the A6/A7 decision; no expiry-policy effective-value row. Documented completed removal and retained numeric tunables separately; added `expiry-policy ... cluster get`. | FIXED: surface; A7/D42/D43 |
| U13 | Docs | Testing said a pool-only failure belongs in EXCLUSIVE or needs slots without first classifying it. A product race must be fixed, not serialized out of sight. Require investigation and measured resource justification; preserve failure. | FIXED: testing; no runner behavior changed |
| U14 | Docs | D44's current wording still directed W130 to removed `make test-w130`. Preserve the original decision's chronology and append wave-16 restoration to `make test-long`; target unchanged. | FIXED: decision register/testing; Makefile includes W130 and both K22 tests |
| U15 | Docs | Command guide did not connect cold-query limits and authoritative-row faults to operator actions. Added cold/refused/stale distinctions, current scoped query coverage, resync link and explicit separation from ordinary readdir's K48. | FIXED: commands; [Task K](k44-retired-replica-routing-20261026.md), [Task M](gate-fstor007-20261027.md) |
| U16 | Docs | Failed-stop prose generalized one successful retained readback into “no write is lost.” Restrict to the recorded run and require separate flush/settle results; a failed revoke is not clean-stop or durability PASS. | FIXED: guide and L29; round 2 U4 remains OPEN |
| U17 | Docs | Guide/L25 claimed no recorded cross-host RDMA runs. Task K records a native-RDMA remote client to co-located storage. Distinguish that scoped PASS from unqualified independent-storage-host topology and full RDMA acceptance. | FIXED: guide/L25; Task K's authorized orcd-mini section and Task M |
| U18 | Product diagnostic | `cmd_raft_resync` still prints “the next conflicting AppendEntries asks for a snapshot”; the implementation's explicit resync request also works when logs agree. Guide warns that this sentence is stale and that request rc 0 is not completion. | OPEN cosmetic CLI correction, not changed in this docs-only task; `src/mgmt/efs_mgmt.c:1154` and Task B/F |
| U19 | Product requirement | K43 writer BUSY after REPORT coordinator loss failed the churn soak at 7,195 s. Retained acknowledged payload readback did not make the write/soak succeed. | OPEN alpha blocker; [Task D](task-d-20261025.md); not an accepted limitation |
| U20 | Product requirement | K48 healthy-quorum ordinary listing omitted one of 96 names after replica halt; permanent payload loss not established. Later unchanged-code passes are not resolution or permission to retry the assertion. | OPEN alpha blocker; [Task N](k15-k29-append-gates-20261027.md); not snapshot lag |
| U21 | Product requirement / qualification | W166 responsive pressure repairs are merged; original full-process SIGSTOP / production-window and live RDMA/multi-host scenarios remain unqualified. No blanket healthy-cluster EIO waiver. | OPEN remaining gates; [W166](w166-close-eio-flush-deadline-20261026.md), [Task G](k38-task-g-repair-20261027.md) |

The guide's ordinary single-loss/healthy-quorum requirements remain intact.
The new caveat identifies evidence that has **not** met them; it does not
redefine BUSY, EIO, omitted names or failed durability as supported success.
No new K/W id duplicates an existing finding.

## Merged evidence reconciliation

Every wave-9-through-22 Recent-evidence entry in the pinned status README
was reconciled, including its linked branch report. A report's historical
“not merged” label does not override the later merge record.

| Wave | Current operator-relevant result and original limits |
| --- | --- |
| 9 | K38/K40 publication repairs, K39 ticket/degraded routing, D43 compiled expiry and policy create path, W159 apply-wait: merged. Responsive-path fixes do not prove later stalled-follower scenarios. [K39](k39-write-eio-one-member-down-20261025.md), [K38 origin](k38-stale-span-replay-20261025.md), [K40](k40-two-client-unit-busy-20261025.md) |
| 10 | [F1 follow-ups](f1-followups-20261025.md): K36 TSan and W77 ABORT fixes, allocator bounds; initial long rewrite plateau/packer failures retained. |
| 11 | [K33 interrupted startup](k33-join-retry-20261025.md): retry the same interrupted first-start command; no workaround formatting of initialized data. |
| 12 | [K41](k41-packer-placement-20261025.md): bounded parallel heartbeats, live-spare placement/probes, shortage EIO naming the segment. No second-loss protection implied. |
| 13 | [K42](e2-clean-stop-busy-20261025.md), [Task D](task-d-20261025.md), [RSS work](efsd-rss-plateau-20261025.md): certified waits merged, scoped plateau evidence distinct from D4/soak failures; K43 OPEN. |
| 14 | [D4 drain](d4-pack-drain-20261025.md), [reclaim](d4-reclaim-20261026.md): duplicate candidates, bounded resend and sweep/publication fixes; live-session abandoned uploads still await revocation. Does not promise synchronous `df` reclaim. |
| 15 | [Task B](k22-k6-loopback-taskb-20261025.md): covering snapshot heals halt, backoff on failed snapshot enqueue, K22 admission; original window EIO retained under W166. D44 target 1,100 s. |
| 16 | [W165](w165-serve-path-damaged-row-20261026.md): serve-first halt/quorum routing and W130 restoration; historical synthetic passes do not erase later K48. |
| 17 | [Task J](gate-budgets-refit-20261026.md): three clean calibration runs; K45 compacted-prefix refusal, K46 no campaigns while resync pending, K47 NET/NOT_FOUND re-probe; original ORCD/sanitizer failures preserved. |
| 18 | [K44](k44-retired-replica-routing-20261026.md): retired seed/current-voter management routing and whole-MKFS proxy merged; scoped orcd-mini addition/sweep PASS; cold query and host-limited gate coverage incomplete. |
| 19 | [W166](w166-close-eio-flush-deadline-20261026.md), [K38 continuation](k38-task-g-repair-20261027.md): authoritative ticket STATUS, shared pressure predicate, definite STALE/residual handling, bounded cohort reclaim; 30 s deadline unchanged, remaining fault scopes NOTRUN. |
| 20 | [Task M](gate-fstor007-20261027.md): libfuse >= 3.12 prerequisite, private current-CLI RDMA launcher; 221/223 POSIX, two explicit skips, peer 68/68 on co-located storage. |
| 21 | [Task N](k15-k29-append-gates-20261027.md): six append checks PASS scoped (59,992 acknowledged records); failed setup retained; K48 OPEN despite later zero-failure gates. |
| 22 | [Task O](dir-ops-busy-under-load-20261027.md): namespace retries use existing deadline, RMDIR 30 s; broader W159 latency failures remain. |

## Runtime execution inventory — NOTRUN

**Every row in this section is NOTRUN on the Mac at this base.** Source
presence/argument checking, historical evidence or a script's `--help` is
not live command execution. Tables enumerate the whole exposed surface,
including proposed/out-of-alpha/debug commands; listing one here does not
promote its support status. No privileged or mutating command was run.

### Management commands

Each command's forms are those in `include/efs/cli_help.h` and the surface
page: reads and mutations, normal/refused cases, version-CAS get/set,
policy on/off, snapshot request/completion, and repeat MKFS must be exercised
on a disposable Linux fixture, never a live workload store.

| ID | `efs-mgmt` command | Current Mac execution |
| --- | --- | --- |
| C01 | `commands-policy` | NOTRUN — Linux build unavailable |
| C02 | `data-commit` | NOTRUN — Linux build unavailable |
| C03 | `expiry` | NOTRUN — Linux build unavailable |
| C04 | `expiry-clock` | NOTRUN — Linux build unavailable |
| C05 | `expiry-enable` | NOTRUN — Linux build unavailable |
| C06 | `expiry-id` | NOTRUN — Linux build unavailable |
| C07 | `expiry-policy` | NOTRUN — Linux build unavailable |
| C08 | `expiry-status` | NOTRUN — Linux build unavailable |
| C09 | `gc-status` | NOTRUN — Linux build unavailable |
| C10 | `io-stats` | NOTRUN — Linux build unavailable |
| C11 | `mkfs` | NOTRUN — Linux build unavailable |
| C12 | `pack-compact` | NOTRUN — Linux build unavailable |
| C13 | `raft-append` | NOTRUN — Linux build unavailable |
| C14 | `raft-change` | NOTRUN — Linux build unavailable |
| C15 | `raft-create` | NOTRUN — Linux build unavailable |
| C16 | `raft-dir` | NOTRUN — Linux build unavailable |
| C17 | `raft-fcntl` | NOTRUN — Linux build unavailable |
| C18 | `raft-flock` | NOTRUN — Linux build unavailable |
| C19 | `raft-getattr` | NOTRUN — Linux build unavailable |
| C20 | `raft-getchunks` | NOTRUN — Linux build unavailable |
| C21 | `raft-hold` | NOTRUN — Linux build unavailable |
| C22 | `raft-link` | NOTRUN — Linux build unavailable |
| C23 | `raft-lookup` | NOTRUN — Linux build unavailable |
| C24 | `raft-lookup-path` | NOTRUN — Linux build unavailable |
| C25 | `raft-mkfs` | NOTRUN — Linux build unavailable |
| C26 | `raft-publish` | NOTRUN — Linux build unavailable |
| C27 | `raft-readdir` | NOTRUN — Linux build unavailable |
| C28 | `raft-rename` | NOTRUN — Linux build unavailable |
| C29 | `raft-resync` | NOTRUN — Linux build unavailable |
| C30 | `raft-rmdir` | NOTRUN — Linux build unavailable |
| C31 | `raft-session` | NOTRUN — Linux build unavailable |
| C32 | `raft-setattr` | NOTRUN — Linux build unavailable |
| C33 | `raft-status` | NOTRUN — Linux build unavailable |
| C34 | `raft-step-down` | NOTRUN — Linux build unavailable |
| C35 | `raft-unlink` | NOTRUN — Linux build unavailable |
| C36 | `shrink-quota` | NOTRUN — Linux build unavailable |
| C37 | `status` | NOTRUN — Linux build unavailable |
| C38 | `sync-status` | NOTRUN — Linux build unavailable |
| C39 | `version` | NOTRUN — Linux build unavailable |

### Daemon, mount, wrappers and `.efs` forms

| ID | Command / variants requiring a Linux fixture | Current Mac execution |
| --- | --- | --- |
| C40 | `efsd ADDR:PORT STORAGE`; fresh seed, persisted restart, 1–24 roots, automatic/explicit quota, `--meta-storage` | NOTRUN |
| C41 | `efsd --join [SEED] STORAGE`, explicit seed and UDP discovery; `--listen`, `--discover-on`, `--discovery-port` including off; online fourth/later addition and interruption | NOTRUN |
| C42 | `efsd --help`, `--version`, `--writers`, `--perf`, `--strace`; removed CLI/env refusals listed on the surface page, unsafe root/quota/endpoint refusals | NOTRUN; help names checked statically only |
| C43 | `efs-fuse NODE:PORT [NODE:PORT ...] MOUNTPOINT`, `-o`, `-f`, noexec Python command form; `--help`, `--help-fuse`, `--version`, `--perf`, `--strace`; removed benchmark/export-label refusal | NOTRUN |
| C44 | `efs-fuse --stop`, `--resume`, `--force-discard`; busy-user refusal, certified cleanup wait, genuine stall/failure, checked mount absence | NOTRUN; force-discard only on disposable unresolved data |
| C45 | `scripts/server.sh` first node/join/start/restart/stop and quota form; `scripts/client.sh` mount/stop; signal stop and exit records | NOTRUN |
| C46 | `.efs/ls`, `.efs/find`, `.efs/stats`, `.efs/help`; every documented sorting, filtering, output, display, timestamp/size/duration option and invalid combination | NOTRUN |
| C47 | `.efs` policy mask 0/1/2/3 transitions, noexec, permissions, real `.efs` precedence, cold/stale/error snapshot, bounds and rejected expiry options | NOTRUN |
| C48 | `df`, file I/O, exact warm/peer/remount bytes, POSIX core/extras/two-client/persistence/expiry, one-node-loss fsync/fdatasync | NOTRUN |
| C49 | Guide's `make`/`make test`/`test-all`/`make check`; testing page's leak/sanitizer/fault/soak/RDMA/POSIX/benchmark/profile/site-driver commands | Linux suite NOTRUN; `make all` attempted and FAIL; Python-only checks below PASS. External controllers, MPI, Valgrind and hardware profiles not exercised |

### Every guide failure row (plus lifecycle refusals)

| ID | Trigger / check | Current Mac execution |
| --- | --- | --- |
| R01 | One member down: reconstruct, place around or degraded durable mark; status and fsync/fdatasync | NOTRUN |
| R02 | Orderly member stop during writes; no acknowledgements from draining node | NOTRUN |
| R03 | Member hangs without exiting / RPC deadline | NOTRUN; existing W166 limits retained |
| R04 | Member returns; verified stripe and segment rebuild; protection restored | NOTRUN |
| R05 | Permanently lost member; no removal/replacement or re-protection elsewhere | NOTRUN |
| R06 | Stored decoder refusal on apply-first and serve-first paths, halt/quorum routing, asynchronous `raft-resync` healing; leader/retired refusal | NOTRUN; K48 remains OPEN |
| R07 | Client stop with member down; storage-fence failure and unresolved leases | NOTRUN; round-2 U4 / L29 decision remains OPEN |
| R08 | Two simultaneous losses; no quorum or insufficient fragments; explicit error | NOTRUN |
| R09 | Client death before publication; abandoned uploads and session cleanup | NOTRUN |
| R10 | Daemon crash/SIGKILL/forced exit; verified recovery and COMMIT fence; stop during recovery | NOTRUN |
| R11 | Packed segment loses two fragments; LOST/unrecoverable and explicit file EIO | NOTRUN |
| R12 | Power loss / kernel crash | NOTRUN; deferred power-loss scope, no process-restart equivalence |
| R13 | Healthy member set: busy mount, failed flush/settle/revoke, certified no-op BUSY, stalled record and operator resume | NOTRUN |
| R14 | Fresh-format refusal, mismatched peer/store, missing root, changed endpoint/root list, invalid/removed inputs and stale policy version | NOTRUN; no old binaries, migration or mixed-build test added |

All L1–L29 rows were reviewed in source/evidence; their live triggers also
remain NOTRUN here. L22 and L24 are decision rows, not tests that can decide
authentication scope or whether W122 ships. L25 retains the incomplete independent-storage-host RDMA topology matrix;
Task K's scoped cross-host client PASS is preserved, not overstated.

## Reproduction and local checks

From the task-Q worktree (raw outputs outside Git):

```sh
make -j4 all                                      # FAIL rc 2 on this Mac
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_cli_docs.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_make_test_recipe.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_env_inventory.py
python3 docs/gen-architecture-full.py
python3 docs/check-architecture.py
make docs-check
git diff --check
```

**Results:** shared-help/doc drift check PASS (39 management commands,
11 daemon options, 9 FUSE options, 10 user-facing pages); Makefile coverage/
budget recipe PASS; environment inventory PASS; architecture docs **5/5
PASS**, `make docs-check` PASS; whitespace check PASS. No full `make test`
PASS is claimed: it depends on the failed Linux build. AST inventory confirms
218 core and 68 peer decorators; extras/persistence counts are checked against
their suite selection and merged evidence, not decorator counting.

Raw local build logs, check outputs, environment/provenance and closed
`SHA256SUMS` are retained at:
`/Users/mike/Documents/Codex/artifacts/task-q-docs-20261027/`.
Closed manifest: **14 files**, SHA-256
`7bfce86b76bce982d7046d4756e9cc3ef8757d073d2159ab1ebc8c6ee6c5b9dd` (all file hashes verified locally). No runtime stores,
payloads or binaries were created. Ignored partial object files in the isolated
worktree are rebuildable, not qualification evidence. No cleanup of other
worktrees or any cluster is needed.
