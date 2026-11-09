# Blind reader: two changes measured by an unfamiliar reader (Task EI)

**Verdict: PASS (Nov 9), with the reference corrected by the reader in
question 2.** Two cold `general-purpose` subagents made the two changes
from the question alone; both started with `graphify explain` on the right
component. Question 1 (a diagnostic, as the brief prescribes when neither
a pass nor a report is owed): the reader's decision matches the reference
(the session revoke collects retained leases; the report is diagnostic,
the write-back count is taken before `efs_wb_stop`), diff in `main.c`
only, committed unchanged as `e75daf090`; 888 lines opened, 817 needed,
10 source files, 195 s. Question 2 (the measured real fix): the reader
found the cause on both sides (`gc.c:103` derive, the reaper's follower
`continue` and its derive in `host_gc_frag_pass`) and a pre-commit race
the reference missed: the twin's rule, the reference's expected fix, can
drop the insert of an apply in flight and under-count. Its fix keeps the
latest apply's inserts; committed unchanged as `6e7f838aa` (six files: two
beyond the allowances, named below); 1,627 lines opened, 1,401 needed, 8
source files, 919 s. node9900: both trees `make test` rc 0;
`tests_for.py client/efs_fuse.main --run` and `client/efs_fuse.open
--run` rc 0; `./tests/test_meta_apply gc_pending` rc 0 and rc 1 with the
fix reverted in either direction; `tests_for.py meta/meta_apply --run`,
`test_gc_fragment_backlog.py`, `test_apply_unsupported.py`,
`test_txn_apply_exact.py` rc 0. No K row for question 2's defect (an
over-count; blocks alpha: no). The same race in the k37 twin
(`efs_meta_degraded_count_begin`) is a candidate, not fixed here (see
Findings). Branch `claude/task-ei-blind-reader-7195d0` on devel
`33cd0a839`. Merged into devel in wave 66 (Nov 9).

- Purpose: [Task EE item 4](review-feedback-nov9-20261109.md#4-unfamiliar-reader)
  and its brief EI. Every reading set so far (Tasks
  [EB](taskEB-review-followup-20261108.md), [EC](find-res-ownership-20261108.md))
  was the implementing agent's own. Here a cold subagent that gets only the
  question makes two real changes and records what it opened; the recorder
  (this session) writes the reference answers first and never writes code.
  [Structure plan](structure-plan-20261102.md) phase 3.
- Source: devel `33cd0a839` (wave 65 graph commit), branch
  `claude/task-ei-blind-reader-7195d0`: `e75daf090` (question 1),
  `6e7f838aa` (question 2), then this page and the ledger, then the graph.
  The node9900 trees are `git archive` of `e75daf090` (tar sha256
  `03f72ade6578f701ac055eee183bd47da6050d40498750fa6c08711aaf0d5c84`) and `6e7f838aa` (`a6481bba8bfdd3bd63d6a10aaf6b87545be51aba0a0462634b644a972f65f07e`), so `EFS_GIT_ID`
  is unknown in the build.
- Environment: node9900 (192 cpus, gcc, libfuse from
  `/data1/efs/deps/libfuse` via `PKG_CONFIG_PATH`; the first question 1
  build picked up the system `fuse3` headers and failed on
  `fuse_loop_cfg_create`, rerun with the deps copy forced), builds under
  `/tmp/efs-taskEI`, `TMPDIR=/tmp/efs-taskEI-tmp`, runs in `screen`, load
  0.2–2. The readers ran on the Mac (no libfuse, no `fopencookie`: neither
  could build there).
- Raw output: `~erbmi1/efs-artifacts/taskEI-20261109/` on node9900 (18
  logs and env files, `SHA256SUMS` sha256 `b9601c35918b94518d0261c8b148083db38ebd61ec7071c65b643f79844da6a2`). The two
  readers' full transcripts are not kept (Agent tool output files in the
  session scratchpad).
- Blinding: each reader is a fresh `general-purpose` subagent started with
  the Agent tool (not a fork): it gets the prompt below and the tracked
  instructions (`AGENTS.md`), no session-start memory injection. The prompt
  forbids `docs/status/`, every `docs/` file outside `docs/how-it-works/`,
  the claude-mem tools and git history (`git log` names k37, K101, EB and
  EC). The reference answers were written to this page before each reader
  started and are not shown to it: the page was written at 19:34:56Z
  (sha256 `908a07c6ce5ec3eeb12524a9b70b55c2fb6c0ac8b3682e94322525e99e62393c`) and kept outside the worktree until both
  readers had returned, so no grep could reach it. The two readers ran one
  after the other; question 1's diff was committed before question 2
  started.
- Blinding limits: the question 1 reader called its advisor tool once
  (step 16, "pin down that revoke releases leases"); the advisor is a
  stronger model that sees only that reader's own transcript, not this
  session, but it is help an unaided reader would not have. The question 2
  log lists no advisor call. The graph includes `docs/status` nodes; a
  reader could see their names in graph output, though neither log shows
  one opened.
- Counting: as on the EB page. A line is "opened" when it was read or
  printed to the reader, "needed" when its answer depends on it. Grep and
  graph output count as navigation. The numbers are the reader's own log,
  checked by the recorder against the files (line ranges exist, categories
  as defined).

## Question 1: unmount with a retained release

### Reference answer (recorder, written before the reader started)

Read: `open.h` (lifetime and operations), `writeback.h` (lifetime,
`efs_wb_pending`, `efs_wb_stop`), `release.c` `efs_control_drain`,
`main.c` `efs_fuse_destroy`, `node_cache.c` `efs_client_shutdown`,
`inode_rpc.c` `efs_client_session_end`, `src/meta/session.c`
`efs_session_cleanup_select` and `cleanup_key`.

- **No retry pass is owed.** A retained row is a HOLD lease whose CLOSE
  edge was not proven. `efs_client_shutdown` always (D42) ends with
  `efs_client_session_end`, which revokes the mount's session; the revoke's
  cleanup selects every `EFS_KV_KIND_LEASE` and `EFS_KV_KIND_LOCK` key of
  the session's uuid and epoch on every touched shard (`session.c`
  `cleanup_key`, `efs_session_cleanup_select`), and a shard is touched
  before the first mutation there, the lease OPEN included. A pending
  CREATE's owed cleanup is a lease release too. So whatever a destroy-time
  pass could prove, the revoke collects. A failed revoke already makes the
  stop unclean (`unmount_unclean(efs_client_shutdown_reason())`, exit 1,
  stop record naming the shard), resumable by `efs-mgmt raft-session fence`;
  nothing is lost silently. Fenced mount: `efs_client_session_end` returns
  `EFS_OK` without revoking because the epoch is already revoked by the
  fencer, whose cleanup collects the keys; a pass could not run there
  anyway (mutations under a fenced epoch fail locally).
- **No report is owed for correctness.** The stop path refuses because it
  keeps the mount; unmount does not keep it, and the revoke settles the
  retained rows. `wb-pending` at unmount is 0 by contract after
  `efs_wb_stop` (writeback.h: "after the stop the pool is unstarted again:
  `efs_wb_pending` says 0"), and `efs_wb_sync` before it waits until the
  pool is idle, failing only past an RPC deadline.
- Hence, as the brief prescribes for this outcome, **question 1 is the
  report line alone, a diagnostic like EB's item 5**: unmount names
  `retained-releases` and `wb-pending` as the stop path does. Placement is
  part of correctness: `retained-releases` is meaningful after the drain
  loop and before `efs_open_retry_stop` / `efs_client_shutdown` (table
  alive); `wb-pending` is meaningful only before `efs_wb_stop` (after it, it
  is always 0). A line that samples `efs_wb_pending(0)` after `efs_wb_stop`
  is a correctness miss (it prints `wb-pending=0` unconditionally).
  Acceptable: a line printed only when something is retained, or always;
  it must not mark the stop unclean (the revoke decides that).
- Expected diff: `src/client/efs_fuse/main.c` only.
- Test: no existing test drives a failed CLOSE edge into `efs_fuse_destroy`
  (`grep` for `retained-releases`, `efs_open_retained`, `efs_fuse_destroy`
  over `tests/` finds only `test_open_interface.c`, the owner's test of
  `open.h`, and `test_stop_dirty_drain.py`, which splices
  `efs_control_drain`). Under the main.c-only rule the reader cannot add
  one; the right answer is "none proves the line; the unmount rows of
  `tests_for.py client/efs_fuse.main` and `make test` show no regression".
  A case in `tests/test_open_interface.c` would be the wrong place.

### Prompt (verbatim)

```text
You are an engineer new to this codebase. Answer one question by reading the code, make the change, and keep an exact log of what you read. The log is being measured, so it matters as much as the change.

Worktree: /Users/mike/git/efs/.claude/worktrees/task-ei-blind-reader-7195d0 (EFS, a distributed file system in C: Raft-replicated metadata stores, a FUSE client). Work only in this directory.

Navigation rule: before your first grep or source read, run `graphify explain "<component tag>"` when you know the component (tags are the `scripts/tests_for.py` subsystems, for example `client/efs_fuse.main` or `meta/meta_apply.gc`), or `graphify query "<question>"` only to locate an owner you do not know. Then read. `python3 scripts/tests_for.py <tag>` lists the tests routed to a component.

Forbidden: any file under docs/status/; any file under docs/ other than docs/how-it-works/; restrict greps to src/, include/, tests/, scripts/ and docs/how-it-works/ so they do not print docs/status lines; the claude-mem tools (mem-search, search, get_observations, timeline, smart_search and every other mcp__plugin_claude-mem tool); git history (git log, git show, git blame, git diff against other commits); ssh or any other host; committing. Do not start other agents.

Question: At unmount (`efs_fuse_destroy`) a last close whose CLOSE edge failed is still retained in the open table, and the write-back pool may still hold jobs. The explicit stop path refuses with a line naming both counts; unmount says nothing and stops the retry thread. Decide from the contracts what unmount owes (a bounded retry pass, a report, or neither, and why), implement it, and name the test that proves it.

Deliverable rules for this question: edit only the file that holds `efs_fuse_destroy`. If you decide that neither a pass nor a report is owed for correctness, still implement the report as a diagnostic (unmount names the two counts as the stop path does) and say so. If no test can be added under that rule, name the existing test or tests that cover the path and say plainly what they do and do not prove. You cannot reach the test hosts; you may try a local build or test if it works on this Mac and report what happened, but do not spend long on it.

Recording rule. Keep this log as you go, not from memory at the end. A line is "opened" when it was read or printed to you; "needed" when your answer depends on it.

Reply in exactly this format:

FIRST CALL: the exact first command you ran and a two-line summary of what it returned.

READING LOG (in order, one row per file range or command):
step | tool (graphify / grep / read / tests_for / other) | path or command | lines opened (e.g. 120-180, or "N hit lines") | category (guide, interface, implementation, callers, tests, navigation) | needed or not needed | why, in a few words (for not needed: why you went there)

TESTS LOOKED FOR AND NOT FOUND: each test you searched for and did not find, with the search.

DECISION: pass, report, or neither (with the diagnostic), and why, citing the contract lines (file:line) it rests on.

DIFF: the full `git diff` of your change.

VALIDATE: the exact commands that build and validate it, and the test you name as proof, with what it proves.

LOCAL RUN: whether you built or ran anything and the result.
```

### Reading log

Reader's log as returned (reformatted into a table; wording shortened, ranges
and verdicts unchanged). Wall time 195 s (Agent call 19:35:01Z to the
hand-back; 23 tool calls).

First call: `graphify explain "client/efs_fuse.main"`. It returned the
component node: `implemented_by main.c`, `guided_by README.md`, and 10
`tested_by` rows (k42_peer_stop_open, k90_unmount_enospc, stop_progress,
k54_member2_unmount, fuse_workers, fuse_worker_options, client_allocator,
client_large_alloc, address_parser, rdma_fork_poller). The graph rule
held: this was the reader's first call.

| Step | Tool | Path or command | Lines | Category | Needed | Why |
|---|---|---|---|---|---|---|
| 1 | graphify | `graphify explain "client/efs_fuse.main"` | 12 | navigation | yes | owner file and routed tests |
| 2 | grep | `efs_fuse_destroy` over src, include; `retain` in the README | 14 hits | navigation | yes | destroy at `main.c:29`; README names the owner |
| 3 | read | `src/client/efs_fuse/README.md` | 40–100 | guide | yes | the K90 unmount paragraph, header map |
| 4 | read | `src/client/efs_fuse/main.c` | 1–120 | implementation | yes | `efs_fuse_destroy` |
| 5 | grep | `efs_open_retained`, `efs_wb_pending`, retry pass/stop | 17 hits | callers | yes | the stop path at `release.c:71-96` |
| 6 | read | `src/client/efs_fuse/open.h` | 1–117 | interface | yes | retained-row lifetime, `efs_open_retry_pass`, `efs_open_retained` |
| 6b | read | `src/client/efs_fuse/release.h` | 1–40 | interface | yes | `efs_control_drain` contract |
| 6c | read | `src/client/efs_fuse/release.c` | 40–115 | implementation | yes | the `stop-refused` line |
| 6d | read | `src/client/efs_fuse/writeback.h` | 1–62 | interface | yes | `efs_wb_stop` finishes every job; pending is 0 after |
| 7 | grep | `efs_client_shutdown`, `revoke` in src/client, include | 20 hits | navigation | yes | shutdown and the D31 revoke |
| 8 | read | `client_internal.h` 286–298, 814–830; `node_cache.c` 920–990 | 30 + 71 | interface + implementation | yes | shutdown always revokes; a failed revoke is unclean |
| 9 | grep | `revok` over `docs/how-it-works/` filtered by lease/hold/open | ~3 preview lines of 524 KB | guide | no | too broad |
| 10 | grep | `lease` in `inode_rpc.c` | 8 hits | navigation | no | looked for a lease release inside `efs_client_session_end` |
| 11 | grep | `session_end` definition, lease lines 2100–2400 | 16 hits | navigation | no | lease resume code, not the revoke |
| 12 | read | `src/client/inode_rpc.c` | 3920–3960 | implementation | yes | the D31 comment on `efs_client_session_end` |
| 13 | grep, tests_for | `efs_open_retry_stop` in `open.c` (-A25); `retained-releases\|stop-refused` in tests, scripts; `tests_for.py client/efs_fuse.main` | 26 + 2 + 10 | implementation + tests | yes | retry stop only joins; no test asserts the line |
| 14 | read, grep | `tests/test_k42_peer_stop_open.py` 1–30; `retained` over tests | 30 + 10 hits | tests | yes | what K42 proves; `test_open_interface.c` counts retained rows |
| 15 | grep | retained/retry in `test_open_interface.c`; `main.c` in `tests/csrc.py` | 11 hits | tests | yes | unit coverage of the count |
| 16 | other | advisor tool | — | navigation | yes | "pin down that revoke releases leases" (see Blinding limits) |
| 17 | grep, read | revoke+lease in src/server, src/meta; `tests/csrc.py` 30–45 | 0 + 16 | tests | no | looked for a destroy harness |
| 18 | read, grep | `tests/csrc.py` 1–28; `session_revoke` over src | 28 + 5 hits | tests / navigation | no / yes | no harness for destroy; found `session_revoke.c` |
| 19 | grep | lease/hold/open in `session_revoke.c`, README | 6 hits | navigation | yes | lease cleanup is in the revoke |
| 20 | read | `src/common/session_revoke.c` | 60–80, 296–310 | implementation | yes | the revoke deletes the session's leases and locks on every touched shard |
| 21 | other | `git diff`; `cc -fsyntax-only` (no libfuse on the Mac) | — | other | — | local build not possible |

Tests looked for and not found: a test asserting the `stop-refused` or
`retained-releases` line (`grep -rln "retained-releases\|stop-refused"
tests/ scripts/`: only `scripts/evidence/{intel,orcd}` observers); a unit
harness that runs `efs_fuse_destroy` (`main.c` appears in `tests/csrc.py`
only in the splice source list); a test with a retained CLOSE at unmount
(only `test_open_interface.c`, which never reaches unmount).

Decision as returned: "neither a retry pass nor a report is owed for
correctness. Unmount now prints the report as a diagnostic." Grounds:
`open.h:22-33`, `node_cache.c:940-958`, `inode_rpc.c:3936-3943`,
`session_revoke.c:63, 301-303` (the revoke's cleanup makes exact lease
and lock deletions on every touched shard), `writeback.h:19-24, 47-51`,
`release.c:94-96`. The write-back count is taken before `efs_wb_stop`,
the retained count after `efs_open_retry_stop` joined.

Test named as proof: none can be added under the main.c-only rule. The
reader named `tests/test_open_interface.c:186-200` (the retained count,
not unmount), `tests/test_k42_peer_stop_open.py` (the revoke's cleanup at
a clean unmount) and `tests/test_stop_progress.py` (a failed revoke is
unclean), each with what it does not prove, and described the missing
test (a fault hook that fails the CLOSE edge, an unmount, the line names
`retained-releases=1`, exit 0, the lease gone on the server). It did not
add a case to `test_open_interface.c`; it did not have to be pointed
elsewhere.

### Diff

```diff
diff --git a/src/client/efs_fuse/main.c b/src/client/efs_fuse/main.c
index 0f375a0c5..dd9cf547a 100644
--- a/src/client/efs_fuse/main.c
+++ b/src/client/efs_fuse/main.c
@@ -39,6 +39,8 @@ static void efs_fuse_destroy(void *userdata)
         snprintf(why, sizeof(why), "data flush writeback=%d", wrc);
         unmount_unclean(why);
     }
+    /* Counted before the stop: after it the pool says 0 (writeback.h). */
+    int wb_left = efs_wb_pending(0);
     efs_wb_stop();
     /* Drain the dirty set before efs_client_shutdown frees the table.
      *
@@ -112,6 +114,14 @@ static void efs_fuse_destroy(void *userdata)
     }
     find_res_clear();
     efs_open_retry_stop();
+    /* Diagnostic only, not unclean: efs_wb_stop finished those jobs (their
+     * failures are the sticky error above) and the session revoke in
+     * efs_client_shutdown deletes this mount's leases, retained CLOSEs
+     * included; its rc decides the stop. Same counts as stop-refused. */
+    unsigned retained = efs_open_retained();
+    if (retained || wb_left)
+        fprintf(stderr, "efs-fuse: unmount: left to the stop (retained-releases=%u "
+                "wb-pending=%d)\n", retained, wb_left);
     efs_client_view_stop();
     if (efs_client_shutdown() != EFS_OK)
         unmount_unclean(efs_client_shutdown_reason());
```

## Question 2: GC pending watermark after a leader change

### Reference answer (recorder, written before the reader started)

Read: `graphify explain meta/meta_apply.gc`, `gc.h`, `gc.c` 50–130 and
200–260, `src/server/raft_host.c` `host_gc_frag_pass` 11119–11160 and the
degraded twin at 12320–12340, `include/efs/meta_apply.h` 1244–1258,
`tests/test_meta_apply.c` `test_gc_watermark` and the degraded case at
5730–5762, `tests/test_gc_fragment_backlog.py` 1–60.

- Cause: `efs_meta_gc_pending_derived` (`gc.c`,
  `gc_wm_pending[anchor] = (int64_t)n + gc_wm_pos[anchor]`) adds the
  inserts noted while the count was unknown (`efs_meta_gc_pending_note`
  from `sweep.c`, a follower or a fresh leader before its first pass) to
  the scan count `n`, although the scan already counted every record
  applied before it. The reaper derives in `host_gc_frag_pass`
  (`raft_host.c`, the one call of `efs_meta_gc_pending_derived`). A leader
  change invalidates the count (`efs_meta_gc_pending_invalidate`), so every
  record the new leader applied as a follower is counted twice; the extra
  bounded scans run until `efs_meta_gc_pending_zero_if` clamps. The `gc.c`
  comment accepts this ("a folded insert may double (upward only)").
- Fixed twin: `efs_meta_degraded_count_begin` (k37, Task DY), which the
  degraded reaper calls just before its scan and which drops `dg_wm_pos`
  while unknown, so only inserts applied during the scan are kept.
- Expected fix: `efs_meta_gc_pending_begin(anchor)` in `gc.c` (drop
  `gc_wm_pos` while unknown), declared in `include/efs/meta_apply.h` beside
  its twin, called in `host_gc_frag_pass` just before the count scan, and a
  `test_meta_apply.c` case whose name contains `gc_pending` that shows the
  double count without the call and the exact count with it, plus an insert
  during the scan kept (upward only: no record missed).
- Upward-only safety: an insert applied before the scan is in the scan; one
  applied during it is in `gc_wm_pos` (and perhaps also in the scan); none
  is in neither. A record can be over-counted, never missed.
- Equivalent fixes graded correct: `efs_meta_gc_pending_invalidate(anchor)`
  just before the scan (at that point the count is unknown, so it only
  clears `gc_wm_pos`; no header change); clearing `gc_wm_pos` in
  `efs_meta_gc_pending_peek` when it answers unknown (gc.c only, but a peek
  with a side effect). Not correct: subtracting in `derived`, or dropping
  `gc_wm_pos` in `derived` (loses the inserts during the scan: a record
  can be missed until the next derive).
- Allowances decided before the reader started (outside the brief's
  three files): one declaration line in `include/efs/meta_apply.h` (the
  twin is declared there); the one call line in `raft_host.c`; and one stub
  line in `tests/test_gc_fragment_backlog.py`, which splices
  `host_gc_frag_pass` from `raft_host.c` and stubs its callees, so a new
  call breaks its compile.
- Validate: `./tests/test_meta_apply gc_pending` (Task EG's filter; the
  case name must contain `gc_pending`, no existing case does),
  `python3 tests/test_gc_fragment_backlog.py`, `make test`.
- K row: no (an over-count costs bounded scans, no record is missed, the
  code comment accepted it); blocks alpha: no.

### Prompt (verbatim)

```text
You are an engineer new to this codebase. Answer one question by reading the code, make the change, and keep an exact log of what you read. The log is being measured, so it matters as much as the change.

Worktree: /Users/mike/git/efs/.claude/worktrees/task-ei-blind-reader-7195d0 (EFS, a distributed file system in C: Raft-replicated metadata stores, a FUSE client). Work only in this directory.

Navigation rule: before your first grep or source read, run `graphify explain "<component tag>"` when you know the component (tags are the `scripts/tests_for.py` subsystems, for example `client/efs_fuse.main` or `meta/meta_apply.gc`), or `graphify query "<question>"` only to locate an owner you do not know. Then read. `python3 scripts/tests_for.py <tag>` lists the tests routed to a component.

Forbidden: any file under docs/status/; any file under docs/ other than docs/how-it-works/; restrict greps to src/, include/, tests/, scripts/ and docs/how-it-works/ so they do not print docs/status lines; the claude-mem tools (mem-search, search, get_observations, timeline, smart_search and every other mcp__plugin_claude-mem tool); git history (git log, git show, git blame, git diff against other commits); ssh or any other host; committing. Do not start other agents.

Question: After a leader change the GC reaper's pending watermark for an anchor exceeds the number of live GC records and the reaper runs extra bounded scans until the count clamps. Find the cause across the reaper and the counter, fix it, and pin it with a case in tests/test_meta_apply.c.

Deliverable rules for this question: keep the change to the files the fix needs and list every file you change and why. The fix must never let the count miss a live record (an over-count is tolerable, an under-count is not); say why yours cannot. The case in tests/test_meta_apply.c should show the wrong count without your fix and the right one with it; give it a name containing `gc_pending` so `./tests/test_meta_apply gc_pending` selects it. If your change would break another test's compile or stubs, find and fix that too and say so. You cannot reach the test hosts; you may try a local build or test if it works on this Mac and report what happened, but do not spend long on it.

Recording rule. Keep this log as you go, not from memory at the end. A line is "opened" when it was read or printed to you; "needed" when your answer depends on it.

Reply in exactly this format:

FIRST CALL: the exact first command you ran and a two-line summary of what it returned.

READING LOG (in order, one row per file range or command):
step | tool (graphify / grep / read / tests_for / other) | path or command | lines opened (e.g. 120-180, or "N hit lines") | category (guide, interface, implementation, callers, tests, navigation) | needed or not needed | why, in a few words (for not needed: why you went there)

TESTS LOOKED FOR AND NOT FOUND: each test you searched for and did not find, with the search.

CAUSE: the cause, naming each file and the exact line(s) (file:line) where it happens, and why it is upward-only safe after your fix.

DIFF: the full `git diff` of your change.

VALIDATE: the exact commands that build and validate it, and what each proves.

LOCAL RUN: whether you built or ran anything and the result.
```

### Reading log

Reader's log as returned (reformatted into a table; wording shortened, ranges
and verdicts unchanged). Wall time 919 s (46 tool calls).

First call: `graphify explain "meta/meta_apply.gc"`. It returned the
component node (degree 3): `implemented_by gc.c`, `contract gc.h`,
`guided_by README.md`, and no `tested_by` edge. The graph rule held.

| Step | Tool | Path or command | Lines | Category | Needed | Why |
|---|---|---|---|---|---|---|
| 1 | graphify | `graphify explain "meta/meta_apply.gc"` | 13 | navigation | yes | gc.c and gc.h own it |
| 2 | grep | `pending` in gc.h, gc.c | 20 hits | navigation | yes | the watermark functions |
| 3 | read | `src/meta/meta_apply/gc.h` | 1–52 | interface | yes | while unknown, inserts fold in, lowers dropped |
| 4 | read | `src/meta/meta_apply/gc.c` | 50–175 | implementation | yes | note, derive, `gc_wm_pos`; `gc_queue` notes at queue time |
| 5 | grep | `efs_meta_gc_pending_*` over src, include, tests | 31 hits | callers | yes | reaper call sites, stubs, the existing case |
| 6 | read | `src/server/raft_host.c` | 3845–3965 | callers | no | checked whether the import invalidations are the leader change (they are not) |
| 7 | read | `src/server/raft_host.c` | 11100–11270 | implementation | yes | the derive at 11140–11150, the `zero_if` clamp |
| 8 | grep | `host_gc_frag_pass\|gc_running` in raft_host.c | 40 hits | navigation | yes | the reaper loop at 12748 |
| 9 | read | `src/server/raft_host.c` | 12690–12752 | callers | yes | a follower `continue`s: it never derives |
| 10 | read | `src/meta/meta_apply/gc.c` | 380–440 | implementation | yes | the degraded clear notes before `meta_write_batch` |
| 11 | read | `src/meta/meta_apply/sweep.c` | 655–685 | implementation | yes | the lower comes after the batch |
| 12 | grep | `gc_queue(` over src/meta | 8 hits | callers | yes | every insert note precedes its commit |
| 13 | grep | an apply mutex in raft_host.c | 0 | navigation | no | looked for an apply lock |
| 14 | grep | `efs_meta_apply_cmd\|applying` | 7 hits | navigation | no | looked for the apply entry |
| 15 | grep, read | `host_apply(`; raft_host.c 270–300 | 4 + 31 | navigation + implementation | yes | the GC scan is a pinned view without the LSM lock |
| 16 | read | `src/server/raft_host.c` | 3551–3600 | implementation | yes | `host_apply` opens the apply scope |
| 17 | grep, read | `efs_kv_lsm_apply_begin`; `src/kv/kv_lsm.c` 1883–1908 | 4 + 26 | navigation + implementation | yes | the scope holds the writers lock from begin to end |
| 18 | read | `src/kv/kv_lsm.c` | 1238–1252, 1728–1737 | implementation | 4 of 25 | a batch outside a scope takes the read lock |
| 19 | grep, read | `&l->writers`; `include/efs/kv_lsm.h` 125–145 | 16 + 21 | navigation + interface | yes | other readers see the old committed table until the scope ends |
| 20 | grep | `^int efs_kv_batch` | 0 | navigation | no | an empty-batch barrier idea |
| 21 | read | `include/efs/kv.h` | 129–141 | interface | no | same idea, dropped |
| 22 | read | `src/kv/kv_lsm.c` | 1215–1232, 20–32 | implementation | no | same idea; the scope is per thread |
| 23 | read | `src/kv/kv_lsm.c` | 1110–1120 | implementation | no | an empty batch is not clearly a no-op |
| 24 | read | `tests/test_gc_fragment_backlog.py` | 1–80 | tests | yes | the reaper's stubs |
| 25 | read | `tests/test_meta_apply.c` | 5405–5500 | tests | yes | `test_gc_watermark`, `wm_count_anchor` |
| 26 | grep | `RUN`, `strstr`, `argv` in test_meta_apply.c | 20 hits | tests | yes | the case filter |
| 27 | grep, read | `RUN(test_gc_watermark)`; `include/efs/meta_apply.h` 1236–1260 | 1 + 25 | tests + interface | yes | the public API; found `efs_meta_degraded_count_begin` |
| 28 | read, grep | `gc.c` 175–270; `degraded_count_begin` in src/server | 96 + 2 | implementation + navigation | yes | the house pattern, its comment names the follower bug |
| 29 | grep, read | `efs_kv_lsm_apply_begin` in src; raft_host.c 12322–12340 | 1 + 19 | callers | yes | one scope entry; the degraded derive order |
| 30 | grep | `host_apply_coord\|host_apply_dispatch(` | 6 hits | callers | yes | dispatch only from `host_apply` |
| 31 | grep | harnesses compiling `host_apply` or the counter | 10 files | tests | yes | which harnesses splice the changed code |
| 32 | grep | those harness files | 21 hits | tests | yes | which take the whole `host_apply` |
| 33 | read | `test_session_combined_group.py` 1–40; `test_apply_unsupported.py` 60–100, 180–190 | 92 | tests | yes | `test_apply_unsupported.py` has no `meta_apply.h` and builds with `-Werror` |
| 34 | grep | includes and build lines of 3 harnesses | ~50 hits | tests | yes | `test_txn_apply_exact.py` links `libefs.a` |
| 35 | grep | includes of gc.c, apply_internal.h | 22 hits | implementation | no | `memset` available |
| 36 | grep | test helpers in test_meta_apply.c | 4 hits | tests | yes | helper locations |
| 37 | read | `tests/test_meta_apply.c` | 794–812, 1983–2015, 4165–4185 | tests | yes | helper signatures |
| 38 | grep | counter and header in `test_txn_apply_exact.py` | 2 hits | tests | yes | it uses the real header |
| 39 | grep | the GC-noting apply entries in src/server | 21 hits | callers | yes | every GC-noting entry is an apply helper |
| 40 | other | enclosing functions of raft_host.c:3179, 23719; `meta_apply.h` 1370–1376 | 10 | callers | yes | 3179 is in `apply_one_publish`; `chunk_holds` is a read |
| 41–43 | other | `make tests/test_meta_apply` (fails on the Mac: `fopencookie`); `cc -fsyntax-only`; `python3 tests/test_gc_fragment_backlog.py` PASS | 27 | other | — | local checks |

Tests looked for and not found: a test that derives after a follower
tenure or calls a begin before the GC derive (`grep -rn
"efs_meta_gc_pending" src include tests`: only `test_gc_watermark` and the
stubs in `test_gc_fragment_backlog.py`); any case named `gc_pending` (none
in the RUN list).

Cause as returned: a follower never derives (`raft_host.c:12709-12719`, the
`!lead` `continue`), so while the count is unknown every insert note folds
into `gc_wm_pos` (`gc.c:77-81`) and `gc.c:103` (`gc_wm_pending = n +
gc_wm_pos`) adds the whole tenure's inserts on top of a scan that counts
them; the reaper scans until `zero_if` clamps (`raft_host.c:11201-11202`).
The degraded counter's `efs_meta_degraded_count_begin` is the fixed twin.

Fix as returned, and where it goes beyond the reference: the reader noticed
that the insert note precedes its batch's commit (`gc.c` `gc_queue`, "the
bump is at queue time, not batch commit"; the degraded clear at `gc.c`
435), and that an apply scope commits only at `efs_kv_lsm_apply_end`
while the reaper's scan reads the old committed table
(`include/efs/kv_lsm.h:131-136`). A `begin` that drops all of `gc_wm_pos`
(the twin's rule, the reference's expected fix) can therefore drop the
insert of an apply in flight that the scan does not see: an under-count.
The reader's `begin` keeps the inserts noted since the latest apply began
(`gc_wm_cur`, zeroed by `efs_meta_gc_pending_apply_start` in `host_apply`
under the KV writers lock, which serializes applies). Its premise, stated
by the reader: every GC-noting write runs inside `host_apply`'s scope (not
traced helper by helper).

### Diff

```diff
diff --git a/include/efs/meta_apply.h b/include/efs/meta_apply.h
index 6d5c98f12..d26aaf7d6 100644
--- a/include/efs/meta_apply.h
+++ b/include/efs/meta_apply.h
@@ -1245,6 +1245,11 @@ void efs_meta_gc_pending_note(uint32_t anchor, int delta);
 int64_t efs_meta_gc_pending_peek(uint32_t anchor);
 void efs_meta_gc_pending_derived(uint32_t anchor, uint64_t n);
 void efs_meta_gc_pending_invalidate(uint32_t anchor);
+/* begin: just before the derive scan, drop inserts the scan counts itself
+ * (all but the latest apply's). apply_start: each apply, under the KV
+ * writers lock. */
+void efs_meta_gc_pending_begin(uint32_t anchor);
+void efs_meta_gc_pending_apply_start(void);
 /* W154: degraded-mark count per anchor, same rules as the GC watermark
  * (-1 unknown; overshoots only; zero_if clamps after an empty scan). */
 int64_t efs_meta_degraded_count_peek(uint32_t anchor);
diff --git a/src/meta/meta_apply/gc.c b/src/meta/meta_apply/gc.c
index 877c676b8..7f66a1454 100644
--- a/src/meta/meta_apply/gc.c
+++ b/src/meta/meta_apply/gc.c
@@ -60,12 +60,15 @@ int efs_meta_unpack_reap(const uint8_t *p, uint32_t n, uint64_t *generation,
  * import); while unknown, inserts accumulate in gc_wm_pos and lowers are
  * dropped — the derive recounts live keys, so a dropped lower lands in
  * the count or not at all, never twice, and a folded insert may double
- * (upward only: an extra bounded scan, never a missed record). */
+ * (upward only: an extra bounded scan, never a missed record).
+ * gc_wm_cur holds the inserts noted since the latest apply began: the
+ * bump precedes its batch's commit, so begin keeps those, not zero. */
 #define GC_WM_ANCHOR_MAX 2
 
 static pthread_mutex_t gc_wm_mu = PTHREAD_MUTEX_INITIALIZER;
 static int64_t gc_wm_pending[GC_WM_ANCHOR_MAX + 1];
 static int64_t gc_wm_pos[GC_WM_ANCHOR_MAX + 1];
+static int64_t gc_wm_cur[GC_WM_ANCHOR_MAX + 1];
 static int gc_wm_known[GC_WM_ANCHOR_MAX + 1];
 
 void efs_meta_gc_pending_note(uint32_t anchor, int delta)
@@ -79,10 +82,36 @@ void efs_meta_gc_pending_note(uint32_t anchor, int delta)
             gc_wm_pending[anchor] = 0;
     } else if (delta > 0) {
         gc_wm_pos[anchor] += delta;
+        gc_wm_cur[anchor] += delta;
     }
     pthread_mutex_unlock(&gc_wm_mu);
 }
 
+/* host_apply calls this once the apply scope holds the KV writers lock:
+ * applies are serialized there, so every insert noted earlier belongs to
+ * an apply that already committed. */
+void efs_meta_gc_pending_apply_start(void)
+{
+    pthread_mutex_lock(&gc_wm_mu);
+    memset(gc_wm_cur, 0, sizeof(gc_wm_cur));
+    pthread_mutex_unlock(&gc_wm_mu);
+}
+
+/* Call just before the derive scan. An insert noted before the latest
+ * apply began is committed, so the scan counts it if it is still live;
+ * only the latest apply's inserts (maybe not committed yet) and those
+ * noted during the scan are folded on top. A follower never derives, so
+ * without this a leader elected later added every record it ever applied. */
+void efs_meta_gc_pending_begin(uint32_t anchor)
+{
+    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
+        return;
+    pthread_mutex_lock(&gc_wm_mu);
+    if (!gc_wm_known[anchor])
+        gc_wm_pos[anchor] = gc_wm_cur[anchor];
+    pthread_mutex_unlock(&gc_wm_mu);
+}
+
 int64_t efs_meta_gc_pending_peek(uint32_t anchor)
 {
     int64_t r;
diff --git a/src/server/raft_host.c b/src/server/raft_host.c
index cea8a19a5..5e62e6f2f 100644
--- a/src/server/raft_host.c
+++ b/src/server/raft_host.c
@@ -3598,6 +3598,7 @@ static int host_apply(void *app, uint64_t index, uint64_t term,
     int scope_rc=efs_kv_lsm_apply_begin(h->kv);
     if (scope_rc!=EFS_OK) rc=scope_rc;
     else {
+    efs_meta_gc_pending_apply_start(); /* D26: under the writers lock */
 #if EFS_FAULTS
     if ((cmd[0]==EFS_MD_CMD_APPEND_CATCHUP || cmd[0]==EFS_MD_CMD_PREPARE || cmd[0]==EFS_MD_CMD_DECIDE ||
          cmd[0]==EFS_MD_CMD_RESOLVE || cmd[0]==EFS_MD_CMD_DROP || cmd[0]==EFS_MD_CMD_TXN_BATCH) &&
@@ -11141,6 +11142,7 @@ static int host_gc_frag_pass(struct efs_raft_host *h, uint8_t group,
     if (pend < 0) {
         uint64_t n = 0;
 
+        efs_meta_gc_pending_begin(anchor);
         src = efs_kv_scan_prefix(h->kv, prefix, plen, gc_count_cb, &n);
         if (src < 0) {
             __atomic_fetch_add(&h->s->gc_scan_errors, 1, __ATOMIC_RELAXED);
diff --git a/tests/test_apply_unsupported.py b/tests/test_apply_unsupported.py
index f8d6ecd8c..ddf72c956 100644
--- a/tests/test_apply_unsupported.py
+++ b/tests/test_apply_unsupported.py
@@ -73,6 +73,7 @@ static void efs_raft_step_down(struct efs_raft *r){r->stepped++;}
 static void wr64be(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
 static void host_mark_incomplete(struct host_group *g){g->kv_incomplete=1;}
 static void host_clear_incomplete(struct host_group *g){g->kv_incomplete=0;}
+static void efs_meta_gc_pending_apply_start(void){}
 static int verdict,dispatched;
 static int host_apply_dispatch(struct efs_raft_host *h,uint64_t index,uint64_t term,uint8_t group,
                                const uint8_t *cmd,uint32_t clen){
diff --git a/tests/test_gc_fragment_backlog.py b/tests/test_gc_fragment_backlog.py
index 8890770db..997550229 100644
--- a/tests/test_gc_fragment_backlog.py
+++ b/tests/test_gc_fragment_backlog.py
@@ -39,6 +39,7 @@ static int gc_gi(uint8_t group){return group==0?0:1;}
 static int efs_kv_key_gc_prefix(uint32_t anchor,uint8_t *out,uint32_t *n){(void)out;*n=3;return anchor?0:EFS_ERR_INVAL;}
 static int64_t efs_meta_gc_pending_peek(uint32_t a){(void)a;return pending;}
 static void efs_meta_gc_pending_derived(uint32_t a,uint64_t n){(void)a;pending=n;}
+static void efs_meta_gc_pending_begin(uint32_t a){(void)a;}
 static int efs_meta_gc_pending_zero_if(uint32_t a,int64_t n){(void)a;if(pending==n){pending=0;return 1;}return 0;}
 static int gc_count_cb(void *ctx,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn){(void)ctx;(void)k;(void)kn;(void)v;(void)vn;return 0;}
 static int gc_scan_cb(void *ctx,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn){(void)ctx;(void)k;(void)kn;(void)v;(void)vn;return 0;}
diff --git a/tests/test_meta_apply.c b/tests/test_meta_apply.c
index 51867e0a6..064e68e67 100644
--- a/tests/test_meta_apply.c
+++ b/tests/test_meta_apply.c
@@ -5493,6 +5493,64 @@ static void test_gc_watermark(void)
     efs_kv_mem_free(kv);
 }
 
+/* D26: a follower applies GC records for a whole tenure while its
+ * watermark is unknown (it never derives). The derive on election must
+ * not add those inserts on top of a scan that already counts them: only
+ * the latest apply's inserts (perhaps uncommitted) are folded in. */
+static void test_gc_pending_follower_derive(void)
+{
+    struct efs_kv *kv = efs_kv_mem_create();
+    struct efs_meta_chunk newc;
+    efs_ino_t ino = 0;
+    uint32_t anch;
+    int i, f;
+
+    CHECK(kv != NULL, "kv");
+    CHECK(efs_meta_apply_init(kv, T0) == EFS_OK, "init");
+    CHECK(efs_meta_apply_create_file(kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
+                                     "gcp", &ino) == EFS_OK && ino, "create");
+    anch = efs_kv_anchor_shard(efs_kv_lane_shard(ino, 0));
+    efs_meta_gc_pending_invalidate(anch);
+
+    /* Follower tenure: three superseding publishes queue three records;
+     * the first is retired (its acks' lowers are dropped while unknown). */
+    efs_meta_gc_pending_apply_start();
+    pub(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xA1, 0, T0 + 1, "gcp0");
+    for (i = 0; i < 3; i++) {
+        efs_meta_gc_pending_apply_start();
+        fill_ch(&newc);
+        for (f = 0; f < EFS_NUM_FRAGMENTS; f++)
+            newc.checksums[f][0] = (uint8_t)(0x80 + 0x10 * i + f);
+        pub_ch(kv, ino, 0, EFS_MIN_CHUNK_SIZE, (uint64_t)0xA2 + i,
+               (uint64_t)0xA1 + i, T0 + 2 + i, &newc, "gcp re-pub");
+    }
+    efs_meta_gc_pending_apply_start();
+    gc_ack_all(kv, ino, 0xA1, 0, 0, "gcp ack");
+    CHECK(wm_count_anchor(kv, anch) == 2, "two records live");
+
+    /* Elected: begin, scan, derive. All inserts were before the latest
+     * apply began, so the scan's count stands (was 2 + 3 = 5). */
+    efs_meta_gc_pending_begin(anch);
+    efs_meta_gc_pending_derived(anch, wm_count_anchor(kv, anch));
+    CHECK(efs_meta_gc_pending_peek(anch) == 2, "derive equals live records");
+
+    /* An insert in the latest apply may be uncommitted at scan time:
+     * begin keeps it (upward only), never drops it. */
+    efs_meta_gc_pending_invalidate(anch);
+    efs_meta_gc_pending_apply_start();
+    fill_ch(&newc);
+    for (f = 0; f < EFS_NUM_FRAGMENTS; f++)
+        newc.checksums[f][0] = (uint8_t)(0xE0 + f);
+    pub_ch(kv, ino, 0, EFS_MIN_CHUNK_SIZE, 0xA5, 0xA4, T0 + 9, &newc,
+           "gcp in-flight");
+    efs_meta_gc_pending_begin(anch);
+    efs_meta_gc_pending_derived(anch, 2); /* scan missed the new record */
+    CHECK(efs_meta_gc_pending_peek(anch) == 3, "in-flight insert kept");
+
+    CHECK(efs_meta_apply_check(kv) == EFS_OK, "check");
+    efs_kv_mem_free(kv);
+}
+
 /* ---- W154 degraded marks ---- */
 struct dg_collect {
     uint64_t n;
@@ -7373,6 +7431,7 @@ int main(int argc, char **argv)
     RUN(test_gc_delta_batch_boundary);
     RUN(test_gc_tail_alias);
     RUN(test_gc_watermark);
+    RUN(test_gc_pending_follower_derive);
     RUN(test_degraded_marks);
     RUN(test_recovery_marks);
     RUN(test_degraded_mark_after_publish);
```

## Correctness against the references

| Check | Question 1 | Question 2 |
|---|---|---|
| Decision or cause matches the reference | yes: neither owed; diagnostic report; both counts placed where meaningful | yes, and more: both files and the derive line (`gc.c:103`, `raft_host.c:11140-11150`), the follower `continue` (`raft_host.c:12709-12719`), the twin; plus the pre-commit race the reference missed |
| Diff confined | `main.c` only (as required) | six files: `gc.c`, the test case, and the pre-decided allowances (`include/efs/meta_apply.h` declarations, the reaper call in `raft_host.c`, the stub in `test_gc_fragment_backlog.py`); beyond them: a second `raft_host.c` line (`efs_meta_gc_pending_apply_start()` in `host_apply`) and one stub line in `tests/test_apply_unsupported.py`, both needed by the race fix |
| Upward-only safe (no record missed) | n/a | yes, on the stated premise that every GC-noting write runs inside `host_apply`'s scope |
| Test | none can prove the line under the main.c-only rule; the reader said so and named what covers the path; not `test_open_interface.c` | `test_gc_pending_follower_derive`: fails with `begin` a no-op (`5535: derive equals live records`) and with the twin's rule (`5548: in-flight insert kept`), passes with the fix |
| Recorder had to fix | nothing | nothing |
| `graphify explain` first | yes, `client/efs_fuse.main` | yes, `meta/meta_apply.gc` |
| Wall time | 195 s | 919 s |

Reference correction (question 2). The reference above was written before
the reader started and is left as written. Its "upward-only safety"
paragraph is wrong: the insert note runs at queue time (`gc.c`
`gc_queue`: "The bump is at queue time, not batch commit"), the apply
scope commits only at `efs_kv_lsm_apply_end`, and the reaper's scan reads
the old committed table (`include/efs/kv_lsm.h`: "other readers see the
old committed table. Writers are excluded until end"). So an insert noted
by the apply in flight is in neither the scan nor, after the twin's
`begin`, `gc_wm_pos`. The recorder checked these three lines; the twin
variant of the case on node9900 fails at the in-flight check, as the
reader predicted.

## node9900

| Tree | Command | Result | Wall |
|---|---|---|---|
| `e75daf090` | `make -j64 all` | rc 0 | — |
| `e75daf090` | `make -j64 test` | rc 0 | 125 s |
| `e75daf090` | `python3 scripts/tests_for.py client/efs_fuse.main --run` | rc 0 (K42, K90, stop progress, K54 member2 unmount and the rest) | 73 s |
| `e75daf090` | `python3 scripts/tests_for.py client/efs_fuse.open --run` | rc 0 | 10 s |
| `6e7f838aa` | `make -j64 all` | rc 0 | — |
| `6e7f838aa` | `./tests/test_meta_apply gc_pending`, `./tests/test_meta_apply gc_watermark` | rc 0, rc 0 | — |
| `6e7f838aa` | `python3 tests/test_gc_fragment_backlog.py`, `test_apply_unsupported.py`, `test_txn_apply_exact.py` | rc 0 each | — |
| `6e7f838aa` | `make -j64 test` | rc 0 | 121 s |
| `6e7f838aa` | `python3 scripts/tests_for.py meta/meta_apply --run` | rc 0 | 4 s |
| `6e7f838aa`, `begin` body removed (old fold) | `./tests/test_meta_apply gc_pending` | rc 1, `FAIL tests/test_meta_apply.c:5535: derive equals live records` | — |
| `6e7f838aa`, `begin` sets `gc_wm_pos = 0` (twin rule) | `./tests/test_meta_apply gc_pending` | rc 1, `FAIL tests/test_meta_apply.c:5548: in-flight insert kept` | — |

No TSan entry is owed: `test_meta_apply` is on no sanitizer list and
question 1 changes only a `fprintf` path in `efs_fuse_destroy`. The
variants are `sed` edits of the archived tree, stated under Reproduce.

Reproduce: `git archive` each commit, build with
`PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig` and
`LD_LIBRARY_PATH` set to its `lib64`, then run the commands in the table;
for the variants replace `gc_wm_pos[anchor] = gc_wm_cur[anchor];` in
`src/meta/meta_apply/gc.c` with `;` or `gc_wm_pos[anchor] = 0;` and run
`make tests/test_meta_apply && ./tests/test_meta_apply gc_pending`.

## Reading numbers and comparison

Counted from the logs above. "Understand" is every category but tests,
"validate" is tests, as on the EB and EC pages. The 524 KB grep in
question 1 step 9 counts as the ~3 lines the reader saw.

| Category (opened / needed) | Q1 | Q2 |
|---|---|---|
| guide | 64 / 61 | 0 / 0 |
| interface | 249 / 249 | 111 / 98 |
| implementation | 370 / 370 | 681 / 596 |
| callers | 17 / 17 | 280 / 159 |
| navigation | 81 / 57 | 106 / 99 |
| tests (validate) | 107 / 63 | 449 / 449 |
| total | 888 / 817 | 1,627 / 1,401 |

| Measurement | EB item 1 after | EB item 5 | EC after | EI Q1 (blind) | EI Q2 (blind) |
|---|---|---|---|---|---|
| Reader | implementer | implementer | implementer | cold subagent | cold subagent |
| Kind | ownership question | caller-side line | ownership question | caller-side line + decision | cross-component fix |
| Understand opened / needed | 683 / 543 | 29 / 29 (all) | 445 / 428 | 781 / 754 | 1,178 / 952 |
| Validate opened / needed | 903 / 427 | (in the 29) | 395 / 395 | 107 / 63 | 449 / 449 |
| Source files opened | 4 | 3 | 3 | 10 | 8 |
| Guide lines opened | 98 | 0 | 101 | 64 | 0 |
| Dead ends (ranges or searches not needed) | 2 | 0 | 0 | 6 (docs grep, two lease greps in `inode_rpc.c`, two `csrc.py` reads, a revoke grep) | 9 (the import invalidations, 121 lines; an empty-batch barrier idea across `kv.h` and `kv_lsm.c`, 55 lines; two apply-lock greps; the gc.c includes) |
| Stopped at guide + interface + one implementation + routed tests | yes | yes (no implementation) | yes | no: the decision needs the revoke (`node_cache.c`, `inode_rpc.c`, `session_revoke.c`) | no: the fix needs the KV apply scope (`kv_lsm.c`, `kv_lsm.h`) and the harnesses |
| Wall time | not recorded | not recorded | not recorded | 195 s | 919 s |

Wrong turns, with why the reader went there: Q1 looked for the lease
release inside `efs_client_session_end` in `inode_rpc.c` (it is in
`session_revoke.c`), grepped `docs/how-it-works` for the revoke (too
broad), and read `tests/csrc.py` looking for a harness that runs
`efs_fuse_destroy` (none exists). Q2 read the import invalidations in
`raft_host.c` 3845–3965 to see whether they were the leader change (they
are the snapshot paths), and spent 55 lines on an empty-batch barrier it
dropped. `graphify explain` on `meta/meta_apply.gc` returned no
`tested_by` edge, so the reader found `test_gc_watermark` and the two
harnesses by grep.

What the comparison establishes: a reader with no session memory, given
only the question, reached the reference's decision for question 1 and a
better fix than the reference for question 2, starting from `graphify
explain` both times; the component node named the right owner files
immediately (step 1 in both logs). The ownership pilots' "stops at one
guide, one interface, one implementation file" held for single-owner
questions read by their implementer; it did not hold here, because both
questions cross owners (the client's unmount and the session revoke; the
counter, the reaper and the KV apply scope), and a blind reader opens
more: 10 and 8 source files against 3 and 4.

What it does not establish: one reader per question, two questions; the
counts are the reader's own log (the recorder checked that the ranges
exist and the categories follow the definitions, not that nothing else
was opened); the question 1 reader used its advisor once; question 1's
prompt told the reader to deliver the diagnostic if it decided "neither",
which the EB and EC questions did not; the questions differ in kind from
EB's and EC's, so the rows are not before/after pairs; the graph and
selector are as on devel `33cd0a839` (no `tested_by` edge for
`meta/meta_apply.gc`).

## Findings

- Question 2's defect: the double count after a follower tenure, fixed in
  `6e7f838aa`. An over-count: extra bounded reap scans, no record missed.
  No K row; blocks alpha: no.
- Candidate, not fixed here (outside this task's files): the k37 twin
  `efs_meta_degraded_count_begin` drops all of `dg_wm_pos` while unknown,
  but `degraded_note` runs after `meta_write_batch` inside the apply scope,
  before `efs_kv_lsm_apply_end` commits. A mark of the apply in flight can
  then be in neither the derive scan nor the count: an under-count by at
  most one apply's marks until the next derive, during which the rebuild
  reaper may treat the anchor as clean. Not reproduced; no K row until it
  is shown reachable (a follow-up should pin it with a case like
  `test_gc_pending_follower_derive`'s in-flight check, and fix it with the
  same `apply_start` mark).

## Not done

- No live cluster, no `make test-all`; TSan not run (none owed).
- The degraded-count candidate above is recorded, not fixed or reproduced.
- Question 1's line is proved by no test: no harness drives a failed CLOSE
  edge into `efs_fuse_destroy`, and adding one was outside the main.c-only
  rule.
- The reader's premise for question 2 (every GC-noting write runs inside
  `host_apply`'s apply scope) was checked by the reader at the call sites,
  not traced helper by helper, and not by the recorder.
- `gc.h` still says "A count moves only after its batch committed", which
  the GC insert note does not do; neither reader nor recorder changed it.
