# K86: valid session refused `UNREGISTERED` on a first `mkdir` (Task CF)

**Verdict: PASS, K86 FIXED, blocks alpha: no.** Fixes the product defect Task CE filed
([bench-published-enospc-20261031.md](bench-published-enospc-20261031.md)). Acceptance: the loaded reproducer
below and `tests/test_bench_published.py` in F1.

## Mechanism

`rc=-28` is `EFS_ERR_UNREGISTERED`. The refusing step was not the owner lookup (CE's first guess) but the closing
`efs_session_accept` of the cross-group admission in `host_namespace_session_admit_covered`
(`src/server/raft_host.c`): the failure log carried a temporary tag naming the step, it was the final accept in all
10 reproduced failures, and the local floor row was absent at that moment and present 200 ms later.

Sequence (three `efs-bench` workers, one session, `dsh=1` for all three; the session shard's owner group differs from
the dentry shard's group, so admission is `REGISTER` on the owner, then `ESTABLISH` on the dentry group):

1. Workers A and B reach the admission on a node that is a follower for the dentry shard's Raft group (leaders are
   split between the two groups under load; the mkdir node need not lead both).
2. A's `ESTABLISH` is forwarded to the group leader and proposed. B's `ESTABLISH` reaches the leader after A's
   entry applied *there*.
3. The leader's submit path takes the reuse shortcut (`host_session_establish_reuse`,
   `src/server/publication_session_fast.h`): the floor is already established, so it answers OK **without proposing**
   and without a log index (`out->index` stays 0).
4. B's node treats OK as "settled" and runs `efs_session_accept` on its own replica, which has not yet applied A's
   entry: no floor row, so `UNREGISTERED`. `server_raft_host_mkdir` returns it at `stage=4`; the client maps it to
   EIO. The other callers of the same forwarded `ESTABLISH` (PUT ticket paths) already guard the zero index with
   `host_read_index`; the namespace admission did not.

## Fix

In `host_namespace_session_admit_covered`, a closing `efs_session_accept` that says `UNREGISTERED` after the
admission command succeeded takes one `host_read_index` on the group and accepts again. Still `UNREGISTERED` after
that read boundary stays `UNREGISTERED` (fail closed). Cost: one read round, only in the race. Chosen over a client
retry (the bench and FUSE create paths do not retry `UNREGISTERED`, and the refusal is a wrong answer of the host,
not a client concern) and over changing the reuse path to return an index (the follower would then read a stranger's
verdict from its apply ring). `tests/test_namespace_admission_rounds.py` now asserts the re-read succeeds once and
fails closed when the floor stays absent.

## Results (intel2, 8 CPU, XFS, 24 concurrent copies of `tests/test_bench_published.py` x 10 rounds)

| Tree | Runs | Fail | `rc=-28` | `rc=-4` (read-index timeout, separate class) |
|---|---|---|---|---|
| before (log tag only) | 240 | 4 | 3 | 2 |
| before (tag + debug re-read) | 240 | 6 | 3 | 5 |
| before (tag + debug re-read) | 240 | 4 | 4 | 0 |
| after (helper form of the fix) | 240 | 0 | 0 | 0 |
| after (helper form of the fix) | 240 | 1 | 0 | 1 |
| after, final shipped tree | 240 | 1 | 0 | 1 |

Before 10 of 720 `rc=-28`; after 0 of 720. The 2 `rc=-4` after the fix (7 before) are a different class
(read-index timeouts under load average about 38), not chased; follow-up. Unit test: `test_namespace_admission_rounds.py`
PASS. Budgeted gate on the final tree (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, intel2):
`TOTAL 583 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (851s) OVERBUDGET 1 BUILD 10s`; OVERBUDGET is
`test_pump_logging.py` 114.3 s against 95 (known, not touched); the 11 NOTRUN are RDMA without a device and the
Memcheck header. A first gate run on an earlier form of the fix had 2 FAIL (`test_namespace_admission_rounds.py`,
`test_report_owner_prepare.py`): they slice `raft_host.c` by function name and my first form added a helper outside
the slice; the final form is inline.

## Reproduction

```sh
git archive <commit> | ssh HOST 'mkdir -p ~/efs-taskCF/src && tar -x -C ~/efs-taskCF/src'
ssh HOST 'cd ~/efs-taskCF/src && TMPDIR=$HOME/efs-taskCF/tmp make -j8 efsd efs-fuse efs-bench efs-mgmt'
ssh HOST 'cd ~/efs-taskCF/src && TMPDIR=$HOME/efs-taskCF/tmp scripts/bench_published_par.sh OUTDIR 24 10'
```

`scripts/bench_published_par.sh` (added here) runs the copies with separate `TMPDIR`s, keeps failing logs and writes
`results.tsv` and `summary.txt`. Expect 3-4 `rc=-28` per 240 on the unfixed tree, 0 on the fixed. Prerequisites: FUSE;
expected about 3 minutes per 240 runs. Cleanup: delete `~/efs-taskCF/src` and `tmp`.

Limitations: no cheap deterministic reproducer (the race needs a follower node, a sibling's reused `ESTABLISH` and
an unapplied replica; the unit test covers the new branch with stubs only). Raw output outside Git: intel2
`~/efs-artifacts/taskCF-20261031/` (532 KiB, `SHA256SUMS` sha256 `e7db489c506d10735c6205b97a4e936e57a4fdbdc3a0174264cb68791829087f`).
