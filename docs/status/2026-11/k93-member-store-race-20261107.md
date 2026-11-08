# K93: member-store identity race (Task DV)

Verdict: **PASS** (K93 FIXED, blocks alpha: no). Linked item: K93 in
[the checklist](../alpha-1.0-2plus1-checklist.md). Purpose: fix the TSan race
Task DH found on `tests/test_member_limit.py`
([finding 4](overbudget-paths-20261105.md)) and put that test on the TSan list.

## Race and fix

`ms_persist()` (`src/server/member_store.c`) adopts a new identity by copying
the whole `struct member_store` (`*ms = *next`). Once the Raft host runs, its
callers (`host_learn_membership`, `host_seed_propose`) hold the host's `h->mu`,
and every other runtime reader in `raft_host.c` reads the store under `h->mu`
too. The one exception was the rejoin thread: `server_join_cluster`
(`src/server/cluster.c`) read `s->mstore.token` for the HELLO, two log lines and
the local node row (`:176`, under `s->lock`, a different lock), and
`s->mstore.cluster` for the HELLO, with no lock. TSan reported the write at
`member_store.c:442` against the read at `cluster.c:176`. The token's value
never changes after open, but cluster and formed state change in the same copy,
so one HELLO could carry a torn token/cluster/formed triple.

Lock rule (stated at `struct member_store` in `member_store.h`, at
`ms_persist`, and in `src/server/README.md`): once the Raft host runs, the
member-store identity is written and read only under `h->mu`; threads outside
`raft_host.c` read it through `server_member_identity`. Before the host starts
(`efsd.c` up to `server_raft_host_start`, including `host_membership_reconcile`
before `g_host` is published) the daemon is single-threaded for the store, and
the accessor reads it unlocked, as `server_membership_get` does.

Change (`e7b0fcb28`, 33 lines added, 13 removed over 7 files):

- `raft_host.c`: new `server_member_identity(&cluster, &formed)` next to
  `server_membership_get`, the same `g_host` acquire-load and `h->mu` pattern;
  returns token, cluster and formed from one locked read (+13).
- `cluster.c` `server_join_cluster`: takes that snapshot once before the HELLO
  is built and uses it for `h.token`, `h.cluster`, `h.flags`, the two log lines
  and `local->token`; the `server_membership_get` call there is gone
  (`mstore.cluster` equals the committed record's cluster once learned, and
  the `formed` flag is unchanged: every `mview_publish` site in `raft_host.c`
  follows a successful `member_store_learn`, or derives from a formed store).
- `server_internal.h` declaration, `member_store.h`/`.c` rule comments,
  `src/server/README.md` lock line.
- `tests/sanitizer_check.sh`: `member_limit|tests/test_member_limit.py` in the
  TSan daemon list.

No second copy of the struct, no atomics, no timeout, budget or sleep changed.
The fix adds one function to `raft_host.c`, which Task DU (K94) also edits; the
hunk is additive and sits next to `server_membership_get`.

## Tested

- Base devel `826d0ef22` (before tree, with the after tree's
  `tests/sanitizer_check.sh` so `member_limit` is on its list); tested source
  `e7b0fcb28`, the fix alone on that base. Trees shipped with `git archive`.
- Host node9900 (192 CPUs, gcc 11.5.0, libtsan), private libfuse
  (`EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, `PKG_CONFIG_PATH` and
  `LD_LIBRARY_PATH` to its `lib64`). TCP loopback, buffered I/O,
  harness-chosen ports, `TMPDIR` under `/tmp/efs-taskDV-tmp`. F14: 16
  production `efsd` and a refused 17th joiner, one mount.
- Commands, from each tree's root:
  - Rep 1 builds and keeps the TSan tree:
    `EFS_SAN_ONLY=member_limit EFS_SAN_LOGDIR=<L>/r01 bash tests/sanitizer_check.sh tsan`.
  - Reps 2..n reuse it:
    `EFS_SAN_BUILD=<L>/r01/build EFS_SAN_ONLY=member_limit EFS_SAN_LOGDIR=<L>/rNN bash tests/sanitizer_check.sh tsan`.
    Each rep counts `WARNING: ThreadSanitizer` lines and `SUMMARY` lines naming
    `ms_persist` or `server_join_cluster` in its `.tsan.*` files.
  - After tree, plain build: `make -j32 test`; three solo
    `python3 tests/test_member_limit.py`; one gate
    `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`.
  - Cleanup: remove both trees and `TMPDIR`.

## Results

| Tree | TSan runs of `member_limit` | Runs reporting this race | Other reports |
| --- | --- | --- | --- |
| before `826d0ef22` | 12 | **11 of 12** (1 report each: write `member_store.c:442` in `ms_persist`, read `cluster.c:176` in `server_join_cluster`) | none |
| after `e7b0fcb28` | 20 | **0 of 20**, `VERDICT: PASS` in all 20 | none |

At the before rate (11 of 12), 20 clean after-runs without a fix would have a
chance of about (1/12)^20, below 1e-21.

- `make test` on the after tree: rc 0.
- `tests/test_member_limit.py` solo on the after tree: 10.9, 11.2, 12.6 s, PASS
  (budget row 15 s, unchanged).
- 8-job gate on the after tree: `TOTAL 593 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (628s)`, no over-budget row, F14 10.2 s in the pool; the four NOTRUN are the host's (no `~/git/cluster` scripts, no valgrind twice, no RDMA live run), as in the wave 61 node9900 gate.
- Locally: `python3 tests/csrc.py` and `python3 scripts/check_include_boundary.py`
  pass.

## Limitations

- Only node9900, only `member_limit` under TSan; the rest of the TSan list was
  not rerun (no other entry touches `server_join_cluster`'s HELLO path more
  than F14 does).
- The TSan reps reuse one build per tree (rep 1's). `make test`, the solo runs
  and the gate used one plain build of the same after tree.
- Lock-order inversions are out of the gate's scope (`detect_deadlocks=0`);
  `server_member_identity` takes only `h->mu` and holds nothing else.
- Raw output: node9900 `~/efs-artifacts/taskDV-20261107/` (`taskDV-raw.tar.gz`
  holds every rep's gate log and TSan reports, the `make test`, solo and gate
  logs, `summary.txt`, `timings.tsv` and the two driver scripts; the TSan build
  trees were not kept). `SHA256SUMS` there has sha256
  `a716650044c16cc5b73cd02d7567f87f4f3abf0879d277bec50617d6b549a3f1`, the tarball's sha256 is `c1936fbaeca73da08c6d914272365812cc78f00a30903c965c22b907a833457a`.
