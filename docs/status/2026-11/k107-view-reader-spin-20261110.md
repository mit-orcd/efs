# K107: group-view readers starve the publisher under TSan (Task FM)

**Merge state (Nov 10):** merged into devel in wave 73; K107 from OPEN to FIXED at the merge. The merge initialised `view_mu` in `tests/test_readindex_publish.py` (it was zero-initialised, which is not a valid mutex on macOS).

**Verdict: PASS on the targeted entry; K107 fix proposed, blocks alpha: no (the coordinator rules at the merge; the K row and the counts line are not edited here).**
`host_view_get` waits on a leaf mutex after 64 odd-sequence loads instead of spinning. `k96_status_slots_live` under TSan
failed 3 of 3 on devel `cff8eb424` on amd and passes 5 of 5 on the fixed tree; the full ASan lane passes (59 of 59), the full
TSan lane passes on its second run (the first run failed `k37_segment_rebuild`, a failure already seen on other trees, see below),
`make test` and `python3 scripts/tests_for.py server/raft_host --run` exit 0. On the batch F reference host fstor007 the entry
**passes** on devel (3 of 3), so on that host K107 did not show.

Task FM, Nov 10 2026. Purpose: fix K107 ([checklist](../alpha-1.0-2plus1-checklist.md), row K107; found by the
[wave 72 merge](ledger.md)) and show `bash tests/sanitizer_check.sh tsan` entry `k96_status_slots_live` passes again.

## Source and environment

| | |
|---|---|
| Before | devel `cff8eb424` (unchanged `src/server/raft_host.c`), `git archive` copy |
| After | task branch `claude/eager-thompson-cd7408`: code `e21ca9933`, test fix `d176f215f` (`raft_host.c` identical in both); `git archive` copy, so the daemons report build id `unknown` |
| amd | user maik, 32 cores, Ubuntu, kernel 7.0.0-42, gcc 15.2.0; work under `~/efs-taskFM/`, TCP loopback, free ports, no `/data1`, `/data2` or ports 17432 to 17435; runs in `screen` (see Not verified) |
| fstor007 | user erbmi1 through the shared ORCD socket, 96 cores, RHEL 9 kernel 5.14, gcc 11.5.0; `/tmp/efs-taskFM` and `TMPDIR=/tmp/efs-taskFM-tmp` only, libfuse shim via `EFS_FUSE_PREFIX`; removed afterwards |
| Entry | `tests/test_k96_status_slots_live.py` (three private `efsd`, 24 concurrent `efs-bench` copies), built with `-fsanitize=thread` by `tests/sanitizer_check.sh` |

## Cause

`host_publish_view` (under `h->mu`) makes the group's `v_seq` odd, stores about 15 atomic fields, makes it even.
`host_view_get` is the lock-free reader: copy, then retry with `goto retry` while `v_seq` is odd or changed. It has no
back-off and no bound. Under TSan every atomic access to a word takes that word's shadow lock in the runtime, and the
runtime's lock is not fair: with dozens of reader threads in a tight load loop on `v_seq`, the publisher's second increment
(`__atomic_add_fetch(..., __ATOMIC_RELEASE)`, `raft_host.c` line 4382 before the change) waits for the readers' shared
holds that never all drop. The publisher holds `h->mu`, so the pump and every connection thread queue behind it and the
member stops answering; the clients end with `revoke rc=-6` at unmount.

I reproduced it with one `gdb -p <pid> -batch -ex "thread apply all bt 14"` per silent `efsd` (a watcher script took it once the
member's log was 25 s old): 109 and 133 `host_view_get` frames at line 4409 (the `v_seq` load) and one thread in
`host_publish_view` at the second increment, inside `__tsan_atomic64_fetch_add`. The pump is that thread (`host_pump` ->
`host_pending_reads` -> `host_publish_group`). The same signature as the six dumps of the wave 72 merge.

Outside TSan the same loop is a bounded stall only as long as the publisher is scheduled; a publisher preempted inside its
window (the window contains no waits, but a thread can be descheduled) left every reader burning a core until it ran again.
Nothing else was wrong: the sequence protocol itself, the fields and the ordering are unchanged.

## Options and the choice

Callers: `host_view_get` is called from 18 functions in `raft_host.c`, 38 call lines (submit, read-index, wait-applied, cover pick, route, waiters
wake, local leader/view, halted hint, persist_applied, expiry and query authority, is_leader, publication committed); the
hot read paths among them must not get slower. `host_publish_view` is called from the pump, `attach_replica`,
`host_publish_group`, `host_read_start_requested` and `host_inbox_batch_handoff`, all with `h->mu` held (the function's contract). No other file calls either.

1. **After a fixed number of failed tries the reader waits on a mutex that the publisher holds across its window. Chosen.**
   New `h->view_mu`, a leaf lock (no other lock is taken inside it): the publisher locks it just before the first `v_seq`
   increment and unlocks it after the second, so it covers only the atomic stores; the getters (`efs_raft_*`, which may wait
   for the disk-log mutex) run before it. `h->mu` is not usable for this because the publisher holds it across those getters.
   A reader that has seen `v_seq` odd `HOST_VIEW_TRIES` (64) times locks and unlocks `view_mu` and retries. The uncontended
   read adds no lock and no system call: it is the same two `v_seq` loads. About 10 lines in `raft_host.c`.
2. Publisher writes a second copy and swaps one atomic index: rejected. It doubles the 15 fields, adds the index, and a
   reader that straddles two swaps still needs the sequence check and a retry, so the starvation of a hot reader loop is
   only made rarer, not bounded.
3. No other design was simpler. A per-group mutex instead of one `view_mu` buys nothing: both groups publish under `h->mu`
   anyway, so a single leaf lock is never contended by two publishers.

The comment above `struct host_view` now states the guarantee: one coherent publish, and what a reader does while a publisher
is in its window. No sleep, timed wait or new timeout was added.

## What bounds a reader's retries now (outside TSan too)

By the design: a reader loads `v_seq` at most 64 times while a publish is in progress, then blocks on `view_mu` without
spinning (no CPU, no `v_seq` access), and the publisher releases the mutex when it finishes its stores. A publisher
preempted inside its window therefore costs a reader 64 loads plus a sleep until the publisher runs again, not a spin.
By the unit test `tests/test_host_view_spin.py`: it runs the production `host_publish_view` and `host_view_get`
with stub getters, holds the publisher inside its window with a hook that blocks before its third field store (the fields are then half new),
starts 8 readers, waits (with `sched_yield`, no sleep) until all 8 are waiting on `view_mu`, and asserts that the readers made at most
8 x 64 `v_seq` loads, none has returned, and after the release every reader returns the finished view (all fields from one publish).
It also asserts that an uncontended read makes two loads and takes no lock, and runs a free-running publisher against 4 readers
for 30,000 publishes checking every returned view is one publish's (all fields equal the same number). Runtime 0.3 s.
On the unfixed `raft_host.c` (`EFS_RAFT_HOST_SOURCE=<devel copy>`) it fails with
`FAIL: readers spin on an odd v_seq without waiting for the publisher` (both on the Mac and on amd). Registered in `Makefile`
(`test:`) and `tests/budgets.tsv` (fast suite, 30 s). `tests/test_readindex_publish.py`, which splices the same functions,
got the `view_mu` member and the `HOST_VIEW_TRIES` define in its harness.

## Results (amd)

| Run | Before (devel) | After (fix) |
|---|---|---|
| `EFS_SAN_ONLY=k96_status_slots_live bash tests/sanitizer_check.sh tsan`, 3 runs before, 5 after | FAIL x3: 112 s, 260 s, 397 s; 1 to 2 of 24 copies `rc=1`, unclean unmount `revoke rc=-6`, silent member `n1` confirmed by gdb in runs 2 and 3 | PASS x5: 9, 10, 9, 11, 11 s |
| full `tsan` lane | not run | run 1: 30 of 31, FAIL `k37_segment_rebuild` ("packer moved 0 of 4" after node 3 returned, no TSan report, no `host_view_get` frame); run 2: PASS 31 of 31 (579 s) |
| full `asan` lane | not run | PASS 59 of 59 (350 s) |
| `make test` | not run | rc 0 (after fixing a gcc 15 `-Werror=misleading-indentation` in the new test, `d176f215f`; the first `make test` stopped there) |
| `python3 scripts/tests_for.py server/raft_host --run` | not run | rc 0 |

The first failing run was not in a `screen` and its siblings were lost with the ssh session (see Not verified); runs 2 and 3
and every "after" run were in `screen`. fstor007, devel `cff8eb424`, three runs: PASS in 12, 14 and 13 s each, so the entry
does not fail there (kernel 5.14, gcc 11.5, 96 cores; on amd the entry fails at gcc 15.2 / kernel 7.0 / 32 cores). That
answers the question that decided urgency: on the batch F reference host K107 did not occur in three runs; I did not run it
more often, so this is not proof it cannot occur there.

Docs gates on the final tree (Mac): `check_k_register.py` PASS (107 rows, counts unchanged), `check_doc_links.py` 0 dangling,
`docs/gen-architecture-full.py` no diff, `make docs-check` PASS, `tests/test_make_test_recipe.py` PASS.

`k37_segment_rebuild` alone under TSan on amd, three runs each, to see whether the lane's one failure belongs to this change:
devel PASS, FAIL, PASS (66 s, 153 s, 66 s); fixed tree FAIL, PASS, FAIL (155 s, 62 s, 156 s). All four failures carry the same
signature (`packer moved 0 of 4` after node 3 returned, no TSan report), also on unmodified devel, and
[K103's page](k103-read-probe-20261110.md), [K102's](k102-stuck-client-20261110.md) and the
[sequence-binaries page](sequence-binaries-sanitizers-20261109.md) record it on other trees and hosts. Three runs each cannot tell 1 of 3
from 2 of 3; the flake pre-exists this change and is left to its own row.

## Reproduction

```
git archive <commit> | ssh amd 'tar -x -C ~/efs-taskFM/src'
ssh amd 'cd ~/efs-taskFM/src && TMPDIR=~/efs-taskFM/tmp EFS_SAN_ONLY=k96_status_slots_live bash tests/sanitizer_check.sh tsan'
# silent member: gdb -p <efsd pid> -batch -ex "thread apply all bt 14"   (efsd whose log is older than 25 s)
python3 tests/test_host_view_spin.py        # the unit test; EFS_RAFT_HOST_SOURCE=<file> runs it against another raft_host.c
bash tests/sanitizer_check.sh tsan; bash tests/sanitizer_check.sh asan; make test; python3 scripts/tests_for.py server/raft_host --run
```
`<commit>` is `cff8eb424` for the failing runs and `d176f215f` for the passing ones. fstor007: the same command with `EFS_FUSE_PREFIX`
pointing at a directory with `lib64` and `include` symlinks to the libfuse deps. Clean up: remove `~/efs-taskFM` (amd) and
`/tmp/efs-taskFM*` (fstor007); `$TMPDIR/efs-tsan-check.*` directories stay after a FAIL.

## Not verified

- A cluster run outside TSan: the starvation needs TSan's runtime lock, so no non-TSan test reproduces the original symptom; the
  bound outside TSan rests on the design and the unit test.
- Whether other readers of lock-free data (the apply-result ring, `host_arc_read`) can starve a writer the same way under TSan:
  it already falls back to `h->mu` after 64 tries; not exercised here.
- The full TSan lane passed on the second run only; `k37_segment_rebuild` failed once (see above). It is not caused by this change as far as I can tell (no
  `host_view_get` frame, no TSan report), but it is a flake of the lane that this change does not remove.
- The first amd run was launched with `ssh amd 'bash -s'` instead of inside a `screen`; killing that ssh left only run 1 of its
  three, so runs 2 and 3 were repeated in `screen`.
- The sanitizer lanes were run once each, after the fix only; no ORCD gate (node9900, fstor007) on the fixed tree; the daemons
  were built from a `git archive` copy, so there is no build id.

## Raw output

Outside Git, on amd: `~maik/efs-artifacts/taskFM-20261110/` (before and after run logs, `gdb-silent-*.txt` dumps with
`gdb-watch.log`, full-lane logs, `make test` and `tests_for` logs, `fstor007/` run logs), with `SHA256SUMS` in that directory.
Hash of `SHA256SUMS` (first 16 hex digits): `c14b6282e3a90dec`.
