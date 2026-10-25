# Packer UNINDEX race: an O_TRUNC rewrite left unpacked (Oct 24, 2026)

**Purpose.** Fix the packer race found by the B6 work (checklist item
**B6**, [alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md);
[B6/D4 evidence](b6-d4-unavailable-progress-20261024.md)):
`EFS_B6_MODES=full python3 tests/test_b6_unavailable.py` failed
intermittently with `full: the packer after the restore: packed 11 of 12`
(intel1 2 of 2, intel2 1 of 4, on the B6 branch `claude/admiring-babbage-a2387d`).
No data was lost: the file read exactly, but stayed unpacked for good.

**Verdict: FIXED, PASS (scoped).** Branch `claude/interesting-clarke-cfb30b`
on devel `9ab6ad74f`, not merged, not pushed. Code commit `6ef6e6037`
(tree `a24ddce9`, identical to the tested WIP commit `40dd2a6ab`). The new
unit test fails without the comparison and passes with it. B6 full passes 15
of 15 with the fix. The live race did not fire on devel during this work
(31 of 31 baseline runs passed), so the B6 runs show no regression rather
than a before/after. `make test-all`: no new FAIL against devel on either host (xorinox: 467
commands, **0 FAIL**, 14 NOTRUN host limits; amd: 9 FAIL, all RDMA on its
`rxe0` device and all failing on devel there too).

## Cause

A file rewritten with `O_TRUNC` publishes size 0, then its new bytes. The
packer scanned the candidate `(anchor, epoch, ino)` while the size was 0
and dropped it (correct: an empty file is never packed). Its
`PACK_UNINDEX` (MD 77) carried only `(epoch, ino)`, and
`efs_meta_apply_pack_unindex` deleted that key unconditionally. When the
rewrite's publication re-added the same key (same epoch, same inode
generation) before the UNINDEX committed, the new candidate was deleted
with it and no later publication re-added it. The same exposure applied to
the "done" list of switched files: a span published on a just-packed file
re-adds its key, which the post-switch UNINDEX could also remove.

## Fix (fresh format)

- Candidate value: `[inode generation:8][publish stamp:8]`
  (`EFS_PACK_CANDIDATE_VAL_BYTES` 16). The stamp is the file's lane-0
  sequence after that publication, which every publication increments, so
  a publication that re-adds an existing key always writes a different
  value. Backfill (`PACK_INDEX`, MD 78) writes stamp 0; publications never
  do (the sequence is at least 1).
- `PACK_UNINDEX` members are `[epoch:8][ino:8][inode generation:8][stamp:8]`
  (`EFS_PACK_UNINDEX_ITEM_BYTES` 32), the key and the value the packer read
  (both for dropped and for switched files). The apply deletes only on an
  exact match; a key that is gone or holds another value is left alone.
- Fail closed: a stored candidate value of another length is
  `EFS_ERR_PROTO` in the apply, and the packer stops its pass with
  `packer: group=G candidate value of N bytes is not this format`. An
  UNINDEX of another length is `EFS_ERR_UNSUPPORTED` (`raft_host.c`), and a
  member naming the backfill cursor key is `EFS_ERR_INVAL`. No KV kind,
  Raft tag or wire type is added; existing stores need a fresh format.
- Files: `include/efs/pack_mapping.h`, `src/meta/meta_apply.c`,
  `src/server/raft_host.c`, `src/server/packer.c`,
  `tests/test_pack_switch.c`; docs `docs/how-it-works/protocols/data.md`
  (regenerated `architecture-full.md` and `architecture.html`).

## Evidence

Environment: intel1, intel2 (Rocky Linux 10.2, 8 CPU, gcc 14.3.1),
xorinox (Rocky Linux 10.2, 16 CPU, gcc 14.3.1), amd (Ubuntu 26.04.1, 32 CPU,
gcc 15.2.0); default build (`make`), TCP loopback, buffered I/O, private
trees under `$HOME`, stores under a private `TMPDIR`, free ports (never
17432–17435). B6 full: three loopback `tests/faults/efsd` members, packer
minimum age 500 ms, node 3 started with a 16 MiB quota and filled, then
restarted with 1 GiB (see the B6 page for the workload).

**Unit test (deterministic reproducer).** `tests/test_pack_switch.c` gains
the race: publish a small file, read its candidate as the packer would,
publish size 0 and then new bytes (the key comes back with a higher stamp),
UNINDEX with the stale value (the candidate stays), with another inode
generation (stays), with the exact value (deleted; a repeat is a no-op);
values of 8 and 24 bytes are PROTO; the cursor key is INVAL. On amd, the
same tree with the comparison line removed fails at the race assertion
(`cand_value(kv, 0, rx, &other) == EFS_OK && other.stamp == cur.stamp`,
abort); with the fix it passes.

**B6 full, `scripts/b6_full_loop.sh TREE OUTDIR RUNS`.**

| Tree | Host | Runs | PASS | Notes | Raw (`SHA256SUMS` sha256) |
|---|---|---|---|---|---|
| fix `40dd2a6ab` | intel1 | 5 | 5 | `EFS_PACK_DBG=1` | intel1 `~/efs-artifacts/pkrace-b6-fix-20261024/` (`6f314819…51cc08`) |
| fix `40dd2a6ab` | intel2 | 10 | 10 | `EFS_PACK_DBG=1` | intel2 `~/efs-artifacts/pkrace-b6-fix-intel2-20261024/` (`ace11a03…88918f`) |
| devel `9ab6ad74f` | intel2 | 5 | 5 | `EFS_PACK_DBG=1` | intel2 `~/efs-artifacts/pkrace-b6-base-20261024/` (`a3e707cf…f0ca08`) |
| devel `9ab6ad74f` | intel1 | 10 | 10 | `EFS_PACK_DBG=1` | intel1 `~/efs-artifacts/pkrace-b6-base-intel1-20261024/` (`61e92b1d…fe002b`) |
| devel `9ab6ad74f` | intel1 | 2 × 8 | 16 | two loops at once, no trace | intel1 `~/efs-artifacts/pkrace-b6-base-load{a,b}-20261024/` (`d3b530a7…3ea9dc`, `42751e8b…cdb95d`) |

Every run took 46–82 s (`full` mode only). The race needs the packer's
scan to land between the truncate's and the rewrite's publications and the
UNINDEX to commit after the rewrite; on devel it did not occur in these 31
runs. The unit test, not the B6 rate, is the before/after evidence.

**`make test-all`** (`scripts/make_test_summary.sh`, `TMPDIR` private):

| Tree | Host | Commands | FAIL | NOTRUN | Time | Raw (`SHA256SUMS` sha256) |
|---|---|---|---|---|---|---|
| fix `6ef6e6037` | xorinox | 467 | 0 | 14 | 2,356 s | xorinox `~/efs-artifacts/pkrace-testall-src-20261024/` (`d4cd3367…12cbfd`) |
| devel `9ab6ad74f` | xorinox | 467 | 1 | 14 | 2,368 s | xorinox `~/efs-artifacts/pkrace-testall-base-20261024/` (`26592902…c6cb8a`) |
| fix `40dd2a6ab` | amd | 467 | 9 | 6 | 2,250 s | amd `~/efs-artifacts/pkrace-testall-src-20261024/` (`e3692ab7…541356`) |
| devel `9ab6ad74f` | amd | 467 | 10 | 6 | 2,434 s | amd `~/efs-artifacts/pkrace-testall-base-20261024/` (`85aacf17…916e1e`) |

xorinox devel FAIL: `tests/test_d4_progress.py` (`packer errors: ...
put_errors: 2`), passing with the fix; one run each, so this is not
claimed as fixed. amd FAIL on both trees: `test_declaration_gate` (its RDMA
frame path) and the eight `test_rdma_xprt` / `test_rdma_segmented` cases
(`CQ moderation off failed`, `segmented RDMA setup failed`: the soft-RoCE
device); devel's tenth is `tests/test_k36_recovery.py`, passing with the fix.
NOTRUN: no RDMA device (xorinox), `~/git/cluster` absent, no valgrind, no
EFS_FAULTS build of `test_follower_bound`, TMPDIR not XFS (amd), the
UNFORMED race not occurring, and the runner's self-test marker.

## Reproduce

```bash
git archive 6ef6e6037 | ssh HOST 'mkdir -p ~/efs-x && tar -x -C ~/efs-x'
ssh HOST 'cd ~/efs-x && make -j8 tests/test_pack_switch && ./tests/test_pack_switch'
ssh HOST 'cd ~/efs-x && scripts/b6_full_loop.sh ~/efs-x ~/efs-artifacts/b6-full-$(date +%Y%m%d) 5'
ssh HOST 'cd ~/efs-x && TMPDIR=$HOME/efs-x-tmp scripts/make_test_summary.sh'
```

Expected: `pack switch: ... compare-and-delete UNINDEX ... PASS`; every B6
run `rc=0 ... PASS`; `make test-all` failures limited to the host limits
listed above. Clean up with `rm -rf ~/efs-x ~/efs-x-tmp`.

## Limitations and remaining gates

- Loopback regression evidence only; multi-host and RDMA runs NOTRUN.
- The live race was not reproduced on devel during this work; the B6
  failure rate before the fix rests on the B6 session's runs (3 of 6).
- Backfill entries all carry stamp 0: two backfills of one key write the
  same value, which is harmless (the packer re-validates every entry).
