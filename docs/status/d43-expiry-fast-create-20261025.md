# D43 follow-up: creates keep the ordinary path until a folder policy is enabled (Oct 25)

**Verdict: PASS (scoped, one shared host), not merged, not pushed.** With
expiry on and no folder policy enabled, creates cost the same as with expiry
off (0.93x and 1.01x on amd, against 2.20x and 2.16x for `412c779ba` in the
same window); with any folder policy enabled, cost is unchanged. amd
`make test-all` at 8 jobs: 484 commands, 11 FAIL, every one also failing in
the same gate on the baseline `412c779ba` there (13 FAIL): amd host limits
and one pool intermittent. **Not met:** "0 FAIL" on a host without those
limits; nuc (D43's 0-FAIL host) was running another session's soak.

- **Purpose:** the follow-up proposed in
  [D43 expiry](d43-expiry-20261025.md#interpretation-and-limits): with the
  cluster expiry policy on, every regular-file create walked its ancestors
  and took the cross-group expiry transaction even with no folder policy
  anywhere (nuc: 77 ms off, 146 ms on, 155 ms on with a folder policy).
  Linked items: [D43](decisions.md#d43--efs-and-expiry-ship-in-alpha-10-user-oct-25),
  checklist A2 (expiry qualified on and off).
- **Tested source:** `b5b9ffc03` on branch `claude/funny-blackburn-010f41`
  (code `05efb3bcf`, then `b5b9ffc03`: the tag rename and two source-shape
  tests),
  based on the D43 tip `412c779ba` (`claude/ecstatic-mccarthy-911369`; not on
  devel or main). Baseline for comparison: `412c779ba` itself. Trees copied to
  amd with `git archive` (no `.git`). `efsd` sha256: measurement trees
  `f301f569…53ab` (change, `05efb3bcf` code) and `fb02f42b…1ca2` (baseline);
  gate trees `be604c5e…7006` (`b5b9ffc03`, `EFS_GIT_ID=b5b9ffc03000`) and
  `f399b6cc…c2bd` (`412c779ba`, `EFS_GIT_ID=412c779ba000`). The next commit
  adds this page and the docs.
- **Host:** amd (32 CPUs, 119 GB, Ubuntu, NVMe; Python 3.14), private trees
  under `~/efs-d43-fast`, `TMPDIR` inside them, loopback ports the tests pick.
  Shared during every run: another session's two loopback acceptance clusters
  (`~/efs-taskA`) and the W123 campaign; load averages are recorded per run
  below. No live cluster, `/data1/efs`, `efs` user or 17432–17435/27432–27433
  port was touched.
- **Raw outputs:** `amd:~/efs-artifacts/d43-fast-20261025/` with `SHA256SUMS`
  (33 files, 164 KB; `SHA256SUMS` sha256 `37bf20b4…9035`): the four
  create-cost JSON files and the driver log and scripts, gate run 2's
  summary, timings and failure logs, the baseline gate's summary and
  timings, gate run 1's summary as printed (its tree was replaced before its
  files were copied), the `efsd` hashes and the instrumented run's log line.
  Private trees deleted with `edelete`.

## Design

| Piece | Now |
| --- | --- |
| State | New KV kind 57 `EXPIRY_FOLDERS`: the number of enabled folder policies in the cluster, one row on each group anchor (shards 1 and 2; absent = 0). The cluster switch row (kind 56) is unchanged, so `EXPIRY_SWITCH` applies never conflict with a count change |
| Maintenance | A folder policy write (tag 58, renamed `EFS_MD_REQ_EXPIRY_POLICY_SET`: the request `efs-mgmt` sends, no longer a log command) is one cross-group transaction: a guard on the folder's `parent_version`, the policy row as an exact-value CAS and, when its enabled flag flips, both count rows (exact-value CAS, +1 or -1). `efs_expiry_policy_plan` computes it from local state; `efs_expiry_policy_set` is the same plan in one store (unit tests). Unequal anchors, or a decrement at 0, are refused as corrupt (`PROTO`) |
| Cache | Each node keeps "some folder policy enabled" next to `expiry_enabled`: refreshed with the cluster policy every GC pass, set by its own policy commits and by every fence refusal. It is a hint and never decides correctness |
| Fast path | Expiry on and the cache at 0: a regular-file create takes the ordinary path. The `CREATE` command carries `HOST_CREATE_F_FENCE`; its apply in the dentry group reads that group's anchor row and answers `AGAIN` when the count is non-zero or a transaction holds the row. The check runs before op-id replay and lease ownership, so a refusal records nothing and the host retries the same op-id as a birth (ancestor walk and expiry transaction, unchanged). A create that goes through the hashed-directory transaction takes a shared `GUARD` on the row's version instead; a changed count makes it stale and it retries as a birth |
| Why no window | Within a group the Raft log orders the fenced `CREATE` against the count row's `PREPARE`. Applied before it, the create precedes the policy's commit decision (which follows every `PREPARE`), so a create without a deadline is correct for it. Applied after it, the create sees the intent or the new count and is refused. No node's cache has to be current, so enabling a folder policy waits for nothing |
| Surface | `efs-mgmt expiry-policy NODE:PORT cluster get` adds `folder_policies=N`. No new command, setting or environment variable |

Unchanged: the D33/D43 birth rules (nearest folder policy, fixed deadline,
coverage at deletion), the cluster switch and its per-pass propagation, the
existing-file enable, renewal and deletion transactions.

## Tests

| Test | Group | What it checks |
| --- | --- | --- |
| `tests/test_expiry` (`folder_count_gates`, new) | `make test` | A first row written disabled counts nothing; enable +1 on both anchors; another value of an enabled policy does not count again; two policies count 2; disables back to 0; the fence (`AGAIN` while counted, OK at 0, invalid anchor refused); an intent on one anchor's row fences that group only and holds off another change, its COMMIT writes the count and bumps the row version; unequal anchors and a decrement at 0 are `PROTO`; a create `GUARD` at an old version is stale, a current one holds off a change until it resolves; a short row is `PROTO` |
| `tests/test_expiry_policy_live.py` (extended) | `make test-long` | Three `./efsd` and efs-fuse. With expiry on and no folder policy: `folder_policies=0` and a new file has no record (ordinary path). A folder policy enabled through node 3, then at once one file through each node's mount: count 1 and every file has a deadline. Count 0 after a disable, 1 after the re-enable, and 1 after an orderly restart of every daemon. Every earlier D43 step unchanged |
| `tests/test_pending_create_command.py` (extended) | `make test` | `apply_create_cmd` with the fence bit: `AGAIN` before any create call while the group's anchor counts a policy (group 2 reads anchor 2), admitted at 0 (group 0, anchor 1), never consulted without the bit |
| `tests/test_kv_kind_unique.py`, `tests/test_submit_unknown_tag.py`, `tests/test_meta_cmd_namespace.py`, `tests/test_retained_create_authority.py` | `make test` | Kind 57 unique; the applied-command table, the dispatcher and the `EFS_MD_CMD_*` names agree after tag 58 left the log; the retained-create proof rules hold in `host_create_ex` |

**Fence exercised:** in a one-off instrumented build (a log line in the
`AGAIN` retry, not committed) the extended live test logged
`raft-host: DEBUG fence refused parent=3796 name=fenced-1 hashed=0`: node 1
still cached no folder policy after node 3 enabled one, its ordinary create
was refused by apply, retried as a birth, and the file got its deadline. The
hashed-directory guard is covered by the unit test only (the live test's
directories are local layout).

## Create cost

`tests/measure/expiry_create_cost.py --files 300 --rounds 3`: three `./efsd`
and one efs-fuse mount on amd's loopback; each sample is O_CREAT|O_EXCL,
4 KiB write, fsync, close; modes interleaved per round. The script now
disables each folder policy after its sample (so every "on" sample has no
enabled folder policy in the cluster) and adds `on+elsewhere` (an enabled
policy on a sibling directory only). Baseline and change alternated run by
run in one window: base, change, base, change (driver
`~/efs-d43-fast/measure.sh`, log `measure.log`).

Mean / p50 / p99 ms, 900 creates per cell:

| Mode | Base run 1 | Change run 1 | Base run 2 | Change run 2 |
| --- | --- | --- | --- | --- |
| off | 63.1 / 57.4 / 179.7 | 62.2 / 57.4 / 132.6 | 76.4 / 62.8 / 490.4 | 69.2 / 60.8 / 349.4 |
| **on, no folder policy** | **138.8** / 117.9 / 909.4 | **58.1** / 56.0 / 138.9 | **165.3** / 134.1 / 730.6 | **69.9** / 62.2 / 419.1 |
| on, folder policy on the directory | 74.4 / 64.9 / 205.1 | 73.2 / 65.0 / 248.4 | 97.7 / 81.5 / 542.1 | 99.9 / 79.2 / 540.1 |
| on, folder policy elsewhere | 99.2 / 67.3 / 343.5 | 96.5 / 67.8 / 333.1 | 132.7 / 100.9 / 583.1 | 133.5 / 116.1 / 656.9 |
| 1-min load at start | 21.6 | 9.6 | 10.2 | 26.5 |

- **On with no folder policy is now the ordinary path:** 0.93x and 1.01x of
  "off" in the same run (baseline: 2.20x and 2.16x). This was the goal.
- **With an enabled folder policy anywhere, cost is as before:** the change
  matches the baseline within run noise in both policy modes. The birth path
  is unchanged; its cost depends on how far the walk goes: a policy on the
  directory itself stops the walk at the parent (1.2–1.4x of off), one
  elsewhere walks to the root with an absence guard per ancestor (1.6–1.9x).
  The baseline's "on, no policy" (2.2x) walked to the root the same way.
- amd's disk is much faster than nuc's (fdatasync about 0.25 ms against
  7 ms), so the ratios matter, not the absolute times; the D43 nuc numbers
  were 77 / 146 / 155 ms. Not re-measured on nuc (it was running another
  session's soak).

## Code gate

`scripts/make_test_summary.sh` (`make test-all`, `EFS_TEST_JOBS=8`,
`EFS_TEST_CMD_TIMEOUT` default 900 s) on amd, driver `gate.sh`/`gate-base.sh`
in the raw folder:

| Run | Tree | Commands | FAIL | NOTRUN | Time | 1-min load at start |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `05efb3bcf` | 484 | 15 | 6 | 619 s | 5.3 |
| 2 | `b5b9ffc03` | 484 | **11** | 6 | 537 s | 2.1 |
| baseline | `412c779ba` | 484 | 13 | 6 | 532 s | 0.3 |

- **Run 1** found three source-shape tests that read the code this change
  edits: `test_meta_cmd_namespace.py` (every `EFS_MD_CMD_*` name must be a
  log command: tag 58 renamed to a request), `test_pending_create_command.py`
  (`apply_create_cmd` takes the group; now also tests the fence) and
  `test_retained_create_authority.py` (reads `host_create_ex`). Fixed in
  `b5b9ffc03`; each passes alone on amd.
- **Run 2's 11 FAIL all fail on the baseline too:** `test_rdma_xprt`,
  seven `test_rdma_segmented` modes and `test_declaration_gate` (RDMA
  `server_accept`/client upgrade on amd's soft-RoCE device),
  `test_sanitizer_gate.py` (its LeakSanitizer fixture reports no leak on
  amd) and `test_create_retry_fault.py` (a client-only fault-hook test, 30 s
  bound exceeded in the pool; it passes alone on this tree, and the baseline
  gate failed it the same way). The first three were also run alone on the
  baseline tree with the same result.
- The baseline additionally failed `test_k33_payload.py` (`EIO`, the known
  payload degraded-read intermittent) and `test_stop_progress.py` (the gvfs
  automounter held a mount); run 1 also failed `test_stop_progress.py`.
- NOTRUN (both trees): no `~/git/cluster`, `test_follower_bound` fault build,
  the query-first race did not occur, no valgrind, `TMPDIR` not XFS, the
  summary runner's self-test. Every expiry test passed, including
  `test_expiry_policy_live.py` and `posix_expiry.py --private`.

## Limits and remaining gates

- The count includes the policy of a folder that was removed while enabled:
  the row is not deleted with the folder (as before D43), and a removed
  folder's policy can no longer be written, so from then on every
  regular-file create takes the birth path. Coverage still ignores that
  policy. The guide says to disable a folder policy before removing the
  folder. A cleanup path for such rows (rmdir, or an operator disable by
  FileID) is not built.
- Enabling a folder policy waits while hashed-directory creates hold their
  shared guard on a count row: the policy transaction retries `BUSY` for up
  to one second, then answers `BUSY` to `efs-mgmt`.
- Not re-run: the `d5-expiry` qualify gate and `posix_expiry.py --private`
  outside `make test-all` (it is inside it), real-host and RDMA runs, and a
  nuc re-measurement.
- No compatibility: tag 58 is no longer a log command and kind 57 is new; a
  log written by `412c779ba` that contains a tag-58 entry is refused at apply
  (fresh format, as for the rest of D43).
