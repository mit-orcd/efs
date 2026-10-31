# Leftover agent branch disposition — October 30, 2026

**Purpose:** settle each unmerged agent branch so the user can delete or keep it (Task BJ, Stage 1 of
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md)). Read-only: nothing merged, deleted, checked out or
pushed, no checklist row edited. This page is the only change.
**Verdict:** **no branch holds an undecided product fix.** Four branches are DELETE
(`agent/devel-task-an`, `agent/devel-task-ao`, `agent/orcd-eio-stress-repair-20261028`, `agent/devel-taskad`),
`agent/devel-task-h` is DELETE as long as `agent/task-h-repair` stays, and `agent/task-h-repair` is KEEP PARKED
(F25 held, five evidence pages that exist nowhere else). `agent/devel-task-ae` is listed only; Task BI audits it.
No PORT CANDIDATE.

**Tested source:** devel `8b0d128ccb3ed8aae92a20bcc446a3df1b355cb7` (wave 43), branch heads as in the table, all read through git
objects only (`git log`, `git show`, `git diff`, `git ls-tree`). Mac, no cluster, no build; nothing here is a runtime result.

## Summary

| Branch | Head | Ahead of devel | Already-in | Ported | Dropped | Held | Undecided | Verdict |
|---|---|---|---|---|---|---|---|---|
| `agent/devel-task-an` | `127231eb3` | 77 | 0 | 40 (1 product + test, 38 evidence/tool commits whose tip content is on devel) | 37 (10 product, 27 support) | 0 | 0 | **DELETE** |
| `agent/devel-task-ao` | `6a825ad32` | 76 | 22 | 19 (7 product, 12 support) | 35 (7 product, 28 support) | 0 | 0 | **DELETE** |
| `agent/devel-taskad` | `474e3dcb3` | 4 | 0 | 0 | 4 (support) | 0 | 0 | **DELETE** |
| `agent/devel-task-h` | `bf143e486` | 3 | 0 | 0 | 3 (support) | 0 | 0 | **DELETE**, but only while `agent/task-h-repair` exists: all 3 commits are inside it |
| `agent/task-h-repair` | `cd822e2f7` | 258 (256 non-merge) | 33 | 37 (30 product, 7 support) | 184 (42 product, 142 support) | 2 (F25) | 0 | **KEEP PARKED** |
| `agent/orcd-eio-stress-repair-20261028` | `afb0e1ee3` | 15 | 0 | 8 | 7 (2 product, 5 docs/test) | 0 | 0 | **DELETE** |
| `agent/devel-task-ae` | `ae7867b82` | 28 | not classified | | | | | Task BI audits it; do not delete before BI reports |

"Ahead" is `git rev-list --count devel..<branch>`; every other count is over the non-merge commits (only `task-h-repair` has merges: 2).
"Product" means the commit touches `src/` or `include/`; "support" means tests, scripts, tools, docs, graph only.
Per-commit tables are in the [appendix](#appendix-per-commit-classification).

## Deleting

`git branch -d` refuses every branch below because none is merged into the checked-out branch (run from devel, none is an ancestor of devel: cherry-picked ports are new commits). `-D` also refuses while the branch is checked out in its worktree, so each block removes the worktree first. The worktrees were not inspected; `git worktree remove` refuses a dirty one, and the user may need `--force` after looking at what is uncommitted there.

## Method

```sh
# per commit: stable patch-id on the branch side and on devel since the oldest base
pids() { git log -p --no-merges --diff-merges=off --format='commit %H' "$@" | git patch-id --stable; }
pids devel..<branch>                                   # branch commits
pids $(git merge-base --octopus devel <all branches>)..devel   # devel side (base 394f2b791, 705 non-merge commits)
```

Classes: **ALREADY-IN** = same stable patch-id on devel. **PORTED** = a port report names the commit as ported
(or devel carries the same subject / a hand-conflicted copy), or the commit's files are byte-identical at devel's tip. **DROPPED** = a port report or the
branch's own reconciliation gives a reason, or it is graph / measurement / bookkeeping, or a Task H duplicate. **HELD** = F25.
**UNDECIDED** = product code not on devel and no report says why; the check found none.
Port reports used: [AO](ao-product-port-20261030.md), [AN](an-product-port-20261030.md), [EIO](eio-product-port-20261030.md),
[H K38/K40](h-k38-publication-20261029.md), [H K22](h-k22-allocator-20261029.md), [H K33](h-k33-routing-20261029.md),
[H K42](h-k42-session-20261029.md), [H K45/K51](h-k45-shutdown-20261029.md), [H recovery](h-recovery-20261029.md),
[Task H baseline](task-h-devel-baseline-20261029.md); the Task H F/R inventory is
`docs/status/task-h-wave33-reconciliation-20261029.md` on `agent/task-h-repair` (not on devel).

Limits of the method:

- Cherry-picks with conflict resolution change the patch-id, so most ports are PORTED by report or by subject, not ALREADY-IN. Where only a report supports the class it is named in the appendix.
- Support commits are classed by file content at the branch tip versus devel's tip (identical means carried; otherwise the generic reason in the appendix). That is a heuristic; the fix a test belongs to is classed by its product commit, and ports used Task H's own tests, adapted.
- No test was run, nothing was built. "On devel" claims for code are from reading devel's source and the port reports.
- `graphify query` could not be used: the worktree has no `graphify-out/graph.json`. Reads were of the git objects directly.

## `agent/devel-task-an` — DELETE

Head `127231eb3`, 77 ahead, base `9c4bfd171`. Worktree `/Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-an`.

- **Ported:** K65 store parent hint `871f9220d` with its test `545c538f8`, as devel `7514b2145` ([AN report](an-product-port-20261030.md)). The AN measurement scripts and evidence page are on devel through `05360d91d`, so 38 more commits have tip content identical to devel.
- **Dropped, reason in the AN report:** K48 `b5e60f229`, `bca5ac4a7` (devel's `host_dir_migrate_txn` already keeps the local row; no failing omission reproduction); K35 `3ddfced51`, `e965262c3`, `1ee28ae43`, `6a6fa2853`, `9b46b07a1` (devel's `test_slow_commit_proof` covers the scenario); `c3f3f313f` (Task H F08 duplicate).
- **Dropped by me:** `ef514fbd8`, `99262765b` (stderr diagnostics only, no behaviour); `a80065c60` and its revert `66daccd4d` (net zero).
- **Undecided: none.**
- **Only on this branch:** `tests/test_dir_migration_materialization.py` (the synthetic K48 harness the AN report's reproduction line names), `tests/test_getchunks_retry_record.py` (F08 duplicate), `tests/retained_tmp.py`. K48 stays OPEN on the checklist; if the 640-cut synthetic result matters later, save the file first:
  `git show agent/devel-task-an:tests/test_dir_migration_materialization.py > /tmp/test_dir_migration_materialization.py`.

```sh
git worktree remove /Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-an
git branch -D agent/devel-task-an
```

`-d` refuses because the branch is not an ancestor of devel (the ports are new commits). Even `-D` refuses while the branch is checked out in that worktree, hence the first line.

## `agent/devel-task-ao` — DELETE

Head `6a825ad32`, 76 ahead, base `9c4bfd171`. Worktree `/Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-ao`.

- **Already in (22):** same patch-id, including `bdc3f4185`, `dd0eb1ef0`, `37348f212` and the Task H F10–F16, F20, F31, F32 copies and the F28/F29 test.
- **Ported (19):** bounded directory pages `68ee39b3c`→`95fbfb05b`, `e46fdf321`→`6fa231182`; K74 `2080aadee`→`34fcf33f3`; K75 `4ebfb7754`→`536e034c9`; K76 `7bcf9e33a`→`fd1b98174`; K78 `6c8bdc72b`→`c84cbce43`; comment-only `abbdad868`; and 12 measurement/live-test/evidence commits brought as `6997b2672` ([AO report](ao-product-port-20261030.md)).
- **Dropped, reason in the AO report:** `93cdc8d00` (redundant with devel `hashed_side_claims`), `e565fb892`, `e8fd647ab` (harness updates devel already has), graph commits, checkpoint prose.
- **Dropped as Task H duplicates already on devel:** `f65961d4d` (F12), `7f3b11c14` (F19), `c1c546e58` (F30), `3cd700397` (F28), `c88420452` (F29).
- **`ccdaeee1e` (shutdown demotion)** — the AO report says "not examined for a unique repro". Examined here: its commit message reads "Port source and pump/core regression from 68f8c7fc0", and devel carries `68f8c7fc0` itself (F34, [H K33 report](h-k33-routing-20261029.md): "already on devel"). The 25-line `host_stop_leading` in `src/server/raft_host.c` is a second copy; DROPPED. Its extra `tests/test_shutdown_bounded.py` case is the same K33 section devel has.
- **Undecided: none. Only-on-branch files: none** (every AO file is on devel).

```sh
git worktree remove /Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-ao
git branch -D agent/devel-task-ao
```

`-d` refuses: not an ancestor of devel. `-D` also needs the worktree gone.

## `agent/devel-taskad` — DELETE

Head `474e3dcb3`, 4 ahead, base `486aa3a46`. Worktree `/Users/mike/git/efs-taskad`. Zero product diff: an early AD attempt
(node9901, docs plus `scripts/ad_measure.py`). Devel closes AD with a different, sealed node9900 run
(`a107516ec`, `docs/status/ad-counter-contention-20261028.md`, checklist AD is `[x]`: measured, no change), so this branch's page is a superseded draft of the same conclusion.
Only on the branch: `scripts/ad_measure.py` (118 lines, a superseded driver; devel's page names its own driver commits `82afb4d2c`, `ef0ddf653`, `14ba987c2`).
Undecided: none.

```sh
git worktree remove /Users/mike/git/efs-taskad
git branch -D agent/devel-taskad
```

`-d` refuses: not an ancestor of devel. `-D` needs the worktree removed first.

## `agent/devel-task-h` — DELETE (keep `agent/task-h-repair`)

Head `bf143e486`, 3 ahead, base `394f2b791`. Worktree `/Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-h`.
Two test-probe commits (`scripts/task_h_truncate_probe.py`) and the docs commit that adds
`docs/status/task-a-finding-b-rerun-20261026.md` (O_TRUNC/fsync recheck, NOT REPRODUCED). **All three commits are ancestors of `agent/task-h-repair`**
(`git merge-base --is-ancestor agent/devel-task-h agent/task-h-repair` is true), and the page and the probe are byte-identical there (blob `861ef2be`, `88a07478`).
Deleting this branch loses nothing while `task-h-repair` stays; if `task-h-repair` is ever deleted, that page and the probe go with it (see below). Undecided: none.

```sh
git worktree remove /Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-h
git branch -D agent/devel-task-h
```

`-d` refuses: not an ancestor of devel (it is an ancestor of `agent/task-h-repair`, which does not help `-d`). `-D` needs the worktree removed.

## `agent/task-h-repair` — KEEP PARKED

Head `cd822e2f7`, 258 ahead (256 non-merge), base `330c48aea`. The Task H source branch. Worktree `/Users/mike/Documents/Codex/2026-10-08/efs/work/task-h-repair`.

- **Already in (33)** by patch-id; **ported (37)**: F02–F04, F07, F08, F10–F24, F26, F28–F32, F35 by the K22/K38/K42/K45/K33/recovery port reports (wave 38). **No Task H F-fix is waiting**: F01, F05, F06, F33, F34 were already on devel or NOT-NEEDED, F27's code lost to devel's own rule (only its test was kept, `ceb1f9e72`).
- **Dropped (184):** the R01–R11 refactor/diagnostic rows (`bf3a4d7e9`, `b88b84459`, `45ad1da38`, `5db04aab3`, `2636d5a60`, `39965d653`, `b36444b29`, `ef896be58`, the R09 diagnostics, `a42933a72`, `c26286eab`, `3987609ff` …): the branch's own reconciliation calls them cut candidates with no retained failing reproduction, and the K22/K33/K42/K45 reports state none of them was ported. The K39/K55 overlap experiment (`31dcb3cbe`, `4c74077e7`, `0ffc905b2`, withdrawn by `15562d47d`) is REMOVED; devel's durability wins. `939503c75` (pack authority read on lagging hosted destination) is on devel as `4e73a01c4`, same six-line change. 142 support commits (forensic scripts, retained-evidence verifiers, docs).
- **Minor un-ported hunks of ported commits** (reports name them): the CREATE bootstrap hunk of `67350b59a` (no K42 reproduction), the `last_rc=`/`reason_scope=` text of `dbf87d011`, the epoch-bound session ACK was ported later with K42. Nothing here has a retained failing reproduction.
- **Held:** F25, below.
- **Undecided: none.**

### Why this branch stays

1. F25 is held on purpose (next section). Its origin commits `6412320f6`, `becc5729c` exist in the repository only here and in devel's reverted history.
2. **Five evidence pages exist on no other branch and not on devel:** `task-h-wave33-reconciliation-20261029.md` (the F01–F35 / R01–R11 inventory every H port report cites as its source, "on that branch"), `task-h-publication-repair-20261026.md`, `task-h-k38-b6-repair-20261028.md`, `task-h-devel-b2-integration-20261028.md`, `task-a-finding-b-rerun-20261026.md`. Devel's port reports and the baseline link to them. Per `AGENTS.md`, evidence must be migrated before its source is removed.
3. 20 forensic/verifier scripts (`scripts/k22_verify_retained.py`, `k33_verify_clone.py`, `k36_verify_retained.py`, `k42_verify_retained.py`, `qualify_publication_repair.py`, …), `tests/live/k22_*`, `tests/test_bootstrap_cohort.py`, `tests/test_storage_fence_authority.py`, `tests/test_verify_window.py`, `tools/fuse_error_trace.c` exist nowhere else. They were skipped on purpose; they are what the pages' reproduction text refers to.

**To make it DELETE:** a docs-only session that copies those five pages (as summaries per `AGENTS.md`, no raw captures) onto devel and decides whether the forensic scripts are worth keeping, plus the F25 decision. Then:

```sh
git worktree remove /Users/mike/Documents/Codex/2026-10-08/efs/work/task-h-repair
git branch -D agent/task-h-repair
```

`-d` refuses: not an ancestor of devel (256 non-merge commits, 222 of which have no patch-id match on devel). `-D` also needs the worktree removed.

## F25 — held on purpose

| Item | Value |
|---|---|
| Task H origin commits | `6412320f6` (`Bound PUT durability epochs to owned fragment and namespace barriers`), `becc5729c` (`Drain pending PUT namespace proofs before GC prunes ancestors`) on `agent/task-h-repair` |
| Devel commits (wave 38) | `b687e26bb`, `a1855bd1e`, and the test fix `c7bfd1218` (`honour TMPDIR in the F25 epoch-files test`), merged with `agent/h-k38-publication` in `8bded1a42` |
| Wave-38 revert | `8427c385f` (reverts `b687e26bb`), `c887132c5` (reverts `a1855bd1e`), `baa214e92` (reverts `c7bfd1218`); note `66ff59cb3` (`docs(status): F25 held back in wave 38`) |
| Branch it lived on | `agent/h-k38-publication`: no longer exists in this repository (no ref). The three commits stay reachable from devel history; `git cat-file -e b687e26bb` succeeds |
| What it changes | `src/server/store.c` K55 syncer: the root-wide `syncfs` per PUT epoch is replaced by per-file and per-name barriers owned by the epoch, through the new `src/server/put_epoch_files.h` (81 lines); second commit drains queued namespace proofs before ancestor GC. Tests: `tests/test_put_epoch_files.py`, `tests/test_store_durability.c` |
| Symptom it targets | Task H `attempt-32`: foreground 14.31 s, fsync 14.128 s behind a 9.355 s unrelated-filesystem barrier |
| Why held | Its only evidence is structural (the new header is absent on devel) plus loopback unit controls. The 14.3 s symptom was not reproduced on devel and no real-cluster rerun exists ([H K38/K40 report](h-k38-publication-20261029.md), wave-38 note) |
| Coordinator position | Stays parked until a demonstrated problem on devel justifies changing the durability barriers. If A11-B decides the PUT barrier is one root-wide `syncfs` per epoch, the revert stands |

If it is ever wanted back: cherry-pick `b687e26bb a1855bd1e` (and `c7bfd1218`) from devel history, or revert the three reverts; both need a behavioural failing reproduction first.

## `agent/orcd-eio-stress-repair-20261028` — DELETE

Head `afb0e1ee3`, 15 ahead, base `9c4bfd171`. Worktree `/Users/mike/Documents/Codex/2026-10-08/efs/work/orcd-eio-stress-repair`.
All 15 are covered by the [EIO port report](eio-product-port-20261030.md) (wave 39, devel `48e671bc8`, merge `db4e74cd0`):

- **Ported (8):** K69 `de0bb5dc7`, `0958c03dd`, `36e566175`; K70 `1b840457a`; K68 `897a79b64`; K67 `d30594aa6`; K71 `6cad51b02`; K66 `098333947`.
- **Dropped (7):** `e7f9a89a1` and `585411524` (test tooling and diagnostics), `d8a7df06c` (Task H F09, on devel as `5a820f91c`), four docs commits (`4837a054e`, `0bb93e9dd`, `b05ff3483`, `afb0e1ee3`, merged into the repair page on devel).
- Only on the branch: `tests/test_posix_failed_retention.py` (the report skipped it: it fails against devel's suite for unrelated harness reasons). Undecided: none.

```sh
git worktree remove /Users/mike/Documents/Codex/2026-10-08/efs/work/orcd-eio-stress-repair
git branch -D agent/orcd-eio-stress-repair-20261028
```

`-d` refuses: not an ancestor of devel (the K66–K71 port is new commits). `-D` needs the worktree removed.

## `agent/devel-task-ae` — listed only

Head `ae7867b82` (2026-10-29), 28 commits ahead of devel, base `714d7e6fc`, worktree `/Users/mike/Documents/Codex/2026-10-08/efs/work/agent-devel-task-ae`.
Not classified here. Devel already has [ae-product-port-20261030.md](ae-product-port-20261030.md) and the `ae-port` merge (K72, K73). Task BI audits this branch.

## Appendix: per-commit classification

Commits are in the order of the groups; `[support]` marks commits that touch no `src/` or `include/` file. ALREADY-IN rows give branch hash→devel hash.

### `agent/devel-task-an`

| Class | Reason / evidence | n | Commits |
|---|---|---|---|
| PORTED | support content identical on devel (carried by the port/merge) [support] | 38 | `2ec74818f`, `4350b5bbd`, `b219611d3`, `c23dd310c`, `9258fd89b`, `4fb5502b6`, `061b7e1fa`, `401785e9f`, `b36f9711b`, `df8fd6d20`, `f8d716dc1`, `8661b6a5f`, `a4715b7cf`, `a7cb7f7dc`, `49c99c84f`, `cbf193ad7`, `ec2d3aeb0`, `cda2b5428`, `96391b44d`, `92a9e829f`, `7e8d0e0f3`, `22bc49bb8`, `aab9b08a3`, `c936721a8`, `3bab2ba55`, `18dbcac21`, `f62df2fc1`, `97b6a585f`, `d4aa07160`, `31b36fe91`, `e39db67ce`, `7084ef525`, `ee6122c52`, `2e85944fd`, `bb83ae41f`, `5800d47a2`, `867817f1c`, `624b94e18` |
| PORTED | 7514b2145 [support] | 1 | `545c538f8` |
| PORTED | 7514b2145 | 1 | `871f9220d` |
| DROPPED | evidence prose / checkpoint (summary page carried or superseded) [support] | 21 | `06a9ac454`, `1ae02dd9b`, `6b60f5f47`, `20d251cdf`, `a13c442da`, `abe62204e`, `579f98a55`, `5e2dc376d`, `241349b19`, `d0386774d`, `7073c43a3`, `ed9b0bacf`, `402e212a2`, `4f2c3ed21`, `ccbc5b36d`, `07871417c`, `6527155b6`, `c658c4356`, `b569cb400`, `e5bbd1f74`, `deeb21cfd` |
| DROPPED | test/tool/measurement support, no product behaviour [support] | 4 | `00a725dbf`, `b38f5d278`, `f7b74f714`, `127231eb3` |
| DROPPED | K35 (an-product-port) | 2 | `e965262c3`, `1ee28ae43` |
| DROPPED | STATUS-proof prototype, reverted by 66daccd4d | 1 | `a80065c60` |
| DROPPED | revert of a80065c60 (net zero) | 1 | `66daccd4d` |
| DROPPED | K35 test conflicts with devel test_slow_commit_proof (an-product-port) [support] | 1 | `6a6fa2853` |
| DROPPED | K35: devel test_slow_commit_proof already covers it (an-product-port) | 1 | `3ddfced51` |
| DROPPED | K35 test (an-product-port) [support] | 1 | `9b46b07a1` |
| DROPPED | K48 migration visibility: devel host_dir_migrate_txn already keeps the local row; no failing omission repro (an-product-port) | 1 | `b5e60f229` |
| DROPPED | K48 lane pre-registration: no failing omission repro (an-product-port) | 1 | `bca5ac4a7` |
| DROPPED | diagnostic fprintf only (K38 refusal phases) | 1 | `ef514fbd8` |
| DROPPED | diagnostic fprintf only (GETCHUNKS refusal details) | 1 | `99262765b` |
| DROPPED | duplicates Task H F08, on devel (an-product-port) | 1 | `c3f3f313f` |

### `agent/devel-task-ao`

| Class | Reason / evidence | n | Commits |
|---|---|---|---|
| PORTED | 6997b2672 AO scripts, live test, evidence page [support] | 11 | `68ce3577a`, `4eea50723`, `bc7721f84`, `fd91807a2`, `bc66afac0`, `fe64b2322`, `9855f7386`, `d3f248a0c`, `15d805c15`, `32aa08e76`, `c1f468bc1` |
| PORTED | 95fbfb05b | 1 | `68ee39b3c` |
| PORTED | 6fa231182 | 1 | `e46fdf321` |
| PORTED | 95fbfb05b (comment-only; meta_apply text is on devel) | 1 | `abbdad868` |
| PORTED | 34fcf33f3 | 1 | `2080aadee` |
| PORTED | c84cbce43 | 1 | `6c8bdc72b` |
| PORTED | fd1b98174 | 1 | `7bcf9e33a` |
| PORTED | d7e783c36 (same subject) [support] | 1 | `6b2cc49f1` |
| PORTED | 536e034c9 | 1 | `4ebfb7754` |
| ALREADY-IN | same patch-id on devel | 22 | `bdc3f4185`→`30ed03aea`, `dd0eb1ef0`→`bf027bb17`, `37348f212`→`6cab1ca52`, `6b0f99e87`→`1e187677d`, `584affe87`→`4aac24509`, `ed3a2b4b8`→`b91d74923`, `4d35d10a7`→`bac5f6f72`, `c1e368721`→`fd6160c15`, `ee712719d`→`6681c5c1d`, `eff6e8537`→`02ba70ce7`, `68c2a5f1e`→`9dc317626`, `b2592b2e9`→`97670beec`, `8b979a1c2`→`264cb48d9`, `279f3d1e8`→`85bceedac`, `251e0f911`→`91238d981`, `bee87649c`→`25246866d`, `491705335`→`da562c23c`, `77aa36eb9`→`b6e809776`, `ba42a7968`→`2295e1a0b`, `6f3e1f1c3`→`850a4d121`, `434f3e488`→`4e28c8e62`, `2fb508759`→`dbf4f7c4b` |
| DROPPED | checkpoint prose not ported (ao-product-port) [support] | 13 | `9ded07bad`, `6da8adb96`, `b58b5cbb7`, `c175d04b1`, `6c2f7aa2c`, `16bf64ae0`, `d7e9e6ebf`, `591ef117c`, `b13b177d1`, `474162f77`, `79fdbd496`, `a57474508`, `6a825ad32` |
| DROPPED | graph bookkeeping (regenerated) [support] | 12 | `a2c6ccf4b`, `6f9f0a945`, `cd02727d8`, `dd31ef14c`, `7fe39669b`, `7d355d5c1`, `9f41e53ea`, `22d34f24e`, `88a4f1a8e`, `055e4c893`, `96cf53f46`, `2bcfc2d1b` |
| DROPPED | redundant with devel hashed_side_claims (ao-product-port) | 1 | `93cdc8d00` |
| DROPPED | Task H F12 duplicate, on devel as a6d66a6d9 | 1 | `f65961d4d` |
| DROPPED | harness updates for F15 stack devel already made (ao-product-port) [support] | 1 | `e565fb892` |
| DROPPED | same (ao-product-port) [support] | 1 | `e8fd647ab` |
| DROPPED | Task H F19 duplicate, on devel as ee9748b5b | 1 | `7f3b11c14` |
| DROPPED | Task H F30 duplicate, on devel as 9c54f7c89 | 1 | `c1c546e58` |
| DROPPED | copy of devel 68f8c7fc0 (F34 stop-leading): its message says "Port source ... from 68f8c7fc0"; h-k33-routing: F34 already on devel | 1 | `ccdaeee1e` |
| DROPPED | test budget bookkeeping for ported tests [support] | 1 | `af1a0c938` |
| DROPPED | Task H F28 duplicate, on devel as c0d374e5d | 1 | `3cd700397` |
| DROPPED | Task H F29 duplicate, on devel as f981cdf4e | 1 | `c88420452` |

### `agent/devel-taskad`

| Class | Reason / evidence | n | Commits |
|---|---|---|---|
| DROPPED | superseded AD attempt: devel carries its own AD evidence page; zero product diff [support] | 4 | `76a44a585`, `62bcece61`, `06ee968fb`, `474e3dcb3` |

### `agent/devel-task-h`

| Class | Reason / evidence | n | Commits |
|---|---|---|---|
| DROPPED | support-only; all 3 commits are inside agent/task-h-repair [support] | 3 | `f4d783e13`, `7b1bc9676`, `bf143e486` |

### `agent/task-h-repair`

| Class | Reason / evidence | n | Commits |
|---|---|---|---|
| PORTED | support content identical on devel (carried by the port/merge) [support] | 7 | `881d01dce`, `4662c7cce`, `a2e3d6212`, `c90a8e5ea`, `633aafc12`, `caf92a618`, `8db51c2c1` |
| PORTED | F15 via h-k22-allocator | 2 | `6c91140e0`, `6e2566cfe` |
| PORTED | F21 via h-k42-session | 2 | `e9a08cbbc`, `67350b59a` |
| PORTED | F12 via h-k22-allocator | 2 | `e32d1721a`, `f533e6054` |
| PORTED | F16 via h-k22-allocator | 2 | `1bf8d29f3`, `3a5ad979f` |
| PORTED | F04 via h-k38-publication | 2 | `45102379f`, `fcc44fe5c` |
| PORTED | F10 via h-k22-allocator | 2 | `5ecd9cefe`, `a10f96604` |
| PORTED | F26 via h-recovery | 2 | `e2e9030cd`, `ece63c2a6` |
| PORTED | F17 via h-k22-allocator | 2 | `db289451c`, `210a0ad2c` |
| PORTED | F30 via h-k33-routing | 2 | `ce5196205`, `254725fd9` |
| PORTED | F07 via h-k38-publication | 1 | `a10859489` |
| PORTED | F02 via h-k38-publication | 1 | `fdc0f0900` |
| PORTED | F11 via h-k22-allocator | 1 | `994c025e6` |
| PORTED | F24 via h-k45-shutdown | 1 | `602cccc0a` |
| PORTED | F19 via h-k22-allocator | 1 | `c4d2fba18` |
| PORTED | F18 via h-k22-allocator | 1 | `16728b167` |
| PORTED | F23 via h-k42-session | 1 | `b4331aef8` |
| PORTED | F14 via h-k22-allocator | 1 | `0b0502dd3` |
| PORTED | F13 via h-k22-allocator | 1 | `bd9540650` |
| PORTED | F35 via h-k45-shutdown | 1 | `16c0e10c0` |
| PORTED | F28 via h-recovery | 1 | `dde6564ed` |
| PORTED | F29 via h-recovery | 1 | `20c0a2ebe` |
| ALREADY-IN | same patch-id on devel | 33 | `58378d1da`→`ddff5fcfd`, `49583981f`→`878c00e96`, `5857ccc96`→`430cd676c`, `cd27c1d8a`→`cc511e41e`, `eaf2aca9f`→`73ceaebdf`, `0c513ab6b`→`a72adf70a`, `bc343ba44`→`ec7c639d7`, `084c88387`→`2b5b7d1ee`, `cf90a64e9`→`23ead82d1`, `84d6c351c`→`743028129`, `3b7ea7124`→`4bcd52ca0`, `4f8f25e7c`→`9f03d4c91`, `40856b152`→`b7ef01a76`, `5c81d6070`→`08d62d686`, `e41cd19ad`→`b5ef40197`, `e7b321502`→`66c78fbdb`, `81008234d`→`c5bd8b334`, `5e63df1a0`→`4556abd9b`, `1848fe5e0`→`7249fc81e`, `1dc4ffb45`→`d90f92287`, `c88c0324c`→`91238d981`, `84d3c8f3a`→`d3a5c7b73`, `6c1f9dd1e`→`4383cf1e1`, `977d10c7c`→`44cc4e3e2`, `121e5fb24`→`5ec231ed5`, `c92271a5f`→`955e5ecc1`, `662789008`→`5a820f91c`, `ea2b9a342`→`7a292ac53`, `f1ed2ccd8`→`c7a70201f`, `52d6f5890`→`8eb03ca72`, `06978bb31`→`54bb0ab43`, `b50f33258`→`25246866d`, `f11d00963`→`da562c23c` |
| HELD | F25 (reverted in wave 38) | 2 | `6412320f6`, `becc5729c` |
| DROPPED | evidence prose / checkpoint (summary page carried or superseded) [support] | 100 | `bf143e486`, `de585cb33`, `80cb2c7f1`, `b807294be`, `b8950164d`, `d04dcca85`, `bee20f99d`, `0c3037d92`, `1e3ec180f`, `84b77d5ef`, `2549585ef`, `69b4b35a7`, `43e6e0401`, `ef6711855`, `6816a420c`, `d8fde8ab0`, `de997c721`, `2bc5fc664`, `df19be8b7`, `38b74fb99`, `e0a41ac3d`, `31298eff7`, `2264afb3a`, `68e50ead1`, `8e850370f`, `85bd01b2f`, `76e87cf42`, `e2ae4ae9c`, `7912e69f3`, `38839d024`, `1c89d651e`, `3fc5c6af5`, `8abe33f23`, `17857ff94`, `b50b592e7`, `f58ce59db`, `e9994d908`, `5dd27b218`, `6725eb66c`, `436d1698c`, `23278a29d`, `7272693d5`, `32e8a1d2b`, `2ccc990c1`, `f79b0c99f`, `0c51bf6b9`, `936ff725e`, `ac311c195`, `ba1d8b3fc`, `3a6487537`, `9fcb535de`, `f9315b655`, `6827929a9`, `395eb8590`, `fdaa276da`, `889b1bdc4`, `e94f090c8`, `647b26e6b`, `851c154a8`, `55356ece1`, `161e3cb66`, `0039b3983`, `17f61c101`, `e4944ce5e`, `f5523ec9c`, `2429b79c2`, `35d3a7185`, `75f08b6ca`, `39e69cb53`, `de2121b66`, `e7b2b4ae9`, `c5acf58dd`, `5b861b338`, `d1030297c`, `4a92d3cdd`, `992487592`, `d67522a67`, `f7dfd60b9`, `784aa81d1`, `95d9704e7`, `761714602`, `a8ecc8e9b`, `484d7cad6`, `fdfda8cfa`, `b6479fcb3`, `776eeceb9`, `9e2bc6b4f`, `f25d98abd`, `2279b0597`, `ffab32b47`, `521812f0d`, `f4ee0778c`, `e9db20c35`, `8190caee1`, `f73784b53`, `d0c9b7717`, `cda23ccde`, `a3aacfee9`, `c6df0f915`, `cd822e2f7` |
| DROPPED | test/tool/measurement support, no product behaviour [support] | 42 | `f4d783e13`, `7b1bc9676`, `bfa6dfb57`, `6c1c82a93`, `f1ac9ef65`, `b7d7de9f7`, `ca1785f83`, `4d3a6c4a6`, `ec02fd774`, `b1118675c`, `f923dcb8c`, `83ddea77a`, `0349cd2c1`, `d4f72cc9b`, `a9b5f9c86`, `08c04bad5`, `e1857539f`, `eeef43d75`, `f617577dc`, `69196702d`, `2d6b48bee`, `fa69fc679`, `816894354`, `924035f2b`, `74077baae`, `b158403d3`, `dd8794804`, `c0eb2058e`, `d0e179a69`, `931f877b3`, `32696e72e`, `4d80689f7`, `5328bfbd0`, `619b898ea`, `b21a32614`, `0b66e1889`, `16e4845b5`, `f702c1e0c`, `db357867a`, `3bc3db7ad`, `826a33150`, `ca6a3edbd` |
| DROPPED | R09 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 9 | `077a59dc0`, `31ee8bee5`, `1aa4260fd`, `b618b13f7`, `8dd44c05b`, `d6a5d40ff`, `79d0811e5`, `dbf87d011`, `4c7eaaa67` |
| DROPPED | R03 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 5 | `45ad1da38`, `16f9e243f`, `3987609ff`, `a42933a72`, `c26286eab` |
| DROPPED | REMOVED row: devel durability wins | 3 | `31dcb3cbe`, `4c74077e7`, `0ffc905b2` |
| DROPPED | diagnostic (R09 class) | 2 | `537bc94e1`, `33ebc9f24` |
| DROPPED | R02 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 2 | `b88b84459`, `0d0552d30` |
| DROPPED | F27 code not landed: devel 35a6e41b0 rule wins; test guard ported as ceb1f9e72 (h-recovery) | 2 | `236a25cb5`, `9ceececc9` |
| DROPPED | F01 already on devel / NOT-NEEDED (h-k38-publication) | 1 | `0c1970678` |
| DROPPED | EFS_FAULTS test hook only | 1 | `da5eb7cbe` |
| DROPPED | devel carries the same fix independently as 4e73a01c4 (verified by diff) | 1 | `939503c75` |
| DROPPED | withdraws the overlap experiment (K39/K55 REMOVED rows) | 1 | `15562d47d` |
| DROPPED | F05 already on devel / NOT-NEEDED (h-k38-publication) | 1 | `5e55c8225` |
| DROPPED | EFS_FAULTS hook and log text only | 1 | `6e10b4d8d` |
| DROPPED | diagnostic log field | 1 | `6a63798a4` |
| DROPPED | R08 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 1 | `ef896be58` |
| DROPPED | one-line include for the branch bootstrap code | 1 | `aaf1db324` |
| DROPPED | R04 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 1 | `5db04aab3` |
| DROPPED | R07 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 1 | `b36444b29` |
| DROPPED | R01 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 1 | `bf3a4d7e9` |
| DROPPED | R05 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 1 | `2636d5a60` |
| DROPPED | R06 not ported by design (h-k22/k33/k45 "No R01-R11 refactor, forensic ...") | 1 | `39965d653` |
| DROPPED | comment only (R07) | 1 | `1a52cf4a1` |
| DROPPED | test-slice refactor of the R03 batching code | 1 | `d49fb47ca` |
| DROPPED | EFS_FAULTS hook only | 1 | `95b837784` |
| DROPPED | F33 already on devel (h-k33-routing) | 1 | `bddd7376c` |
| DROPPED | F34 already on devel (h-k33-routing) | 1 | `85962d8cd` |

### `agent/orcd-eio-stress-repair-20261028`

| Class | Reason / evidence | n | Commits |
|---|---|---|---|
| PORTED | 48e671bc8 (K66-K71 port) | 6 | `de0bb5dc7`, `1b840457a`, `897a79b64`, `d30594aa6`, `6cad51b02`, `098333947` |
| PORTED | 48e671bc8 (K66-K71 port) [support] | 2 | `0958c03dd`, `36e566175` |
| DROPPED | docs only [support] | 3 | `0bb93e9dd`, `b05ff3483`, `afb0e1ee3` |
| DROPPED | test tooling and diagnostics (eio-product-port) | 1 | `e7f9a89a1` |
| DROPPED | Task H F09 duplicate, on devel as 5a820f91c | 1 | `d8a7df06c` |
| DROPPED | docs only (eio-product-port) [support] | 1 | `4837a054e` |
| DROPPED | test-suite diagnostics only (eio-product-port) [support] | 1 | `585411524` |
