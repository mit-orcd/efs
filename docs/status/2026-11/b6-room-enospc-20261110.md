# B6 full mode: room freed on a full node went to a held chunk (Task FE, Nov 10)

**Status (Nov 10):** merged into devel in wave 72. The coordinator accepted the verdict (test defect, no K row) on the fixture trace and the K90 contract; the failure was never reproduced, and which client path re-sent the held chunk is not identified.

**Verdict: PASS for the fix (test only, no K row, blocks alpha: no). The product
admitted correctly; the gate failure is not reproduced.** In the wave 69 merge gate on fstor007,
`room_scenario` in `tests/test_b6_unavailable.py` failed with `room: ENOSPC then
ENOSPC, want ok then ENOSPC`. The one fragment of room that the test's unlink freed on
node 3 was taken by the retained chunk of the last, refused fill file (K90 hold),
not by `room-0`. Node 3's quota admission was right both times. The scenario now ends
that hold first (`give_up_held`), and a fast test pins both halves. What triggered the
held chunk's PUT at that moment is not identified, and the failure did not reproduce in
40 baseline runs (20 solo on fstor007, 20 under load on amd).

## Source and environment

- Source: branch `claude/focused-heisenberg-897cf2` on devel `2094fea52`; test commit
  `fd8fffe23`. Build identity: plain `make all` / `make tests/faults/efsd`, build id
  `unknown` (no release stamping); amd trees: `c` = devel, `a` = devel plus a
  backtrace and a `FE-PUTRUN` line on every client PUT run (`put_fragments_parallel_run`,
  `src/client/write.c`), `d` = devel plus this commit's tests, `e` = this commit.
- Gate failure host: fstor007 (user erbmi1), wave 69 merge gate, fixture
  `~erbmi1/efs-artifacts/merge-wave69-20261109-fstor007-b6fixture/` (tar.gz sha256
  `43e4d795dc0eb5007383b6d55560c0bf1bd4801321746090071f5d2ff1433286`, fail log sha256
  `3ec5808978f4678846966036d253b51292245a4c3ba196e7f2f3008421833a2f`). Load there was
  about 160.
- Reproduction host: amd (32 CPUs, 119 GB, Python 3.14.4, system libfuse3), under
  `$HOME/efs-taskFE`, `TMPDIR=$HOME/efs-taskFE/tmp`. **The before and after loops ran on
  amd, not on the host of the original failure.** The harness takes ports with `bind(0)`,
  which on amd are 32768-60999; none of 17432-17435, 18xxx or 27xxx can be handed out.
  The 8-job `make test` load also uses `bind(0)` ports.
- Cluster: three `tests/faults/efsd` members on loopback, node 3 at `--quota 16M`, two
  `efs-fuse` mounts, buffered I/O, discovery off, GC and packer on.

## The fixture sequence (fstor007, UTC, Nov 10 02:14-02:15)

- Node 3 at exit: `usage.bin` used 16,736,256 of 16,777,216; 255 fragment files, one
  more than the 254 the test's "unlink freed one fragment" state allows. Node 3 accepted
  exactly one 65,536-byte PUT after the unlink (iostats 5 s samples: 02:15:02 to 02:15:07
  86 PUTs, 85 refused, 1 accepted).
- Per-node fragment files: the accepted fragment is object `0.1.1476274851595866583` of
  ino 1032193, the last fill file that had been refused (client lines `dcache-flush-fail
  ino=1032193 stage=put rc=-11` at 02:14:55, tickets 264-266). Its PUT is ticket 267 at
  02:15:02 on n1, n2 and n3. `room-0` (ino 1036289, ticket 268) reached n1 and n2 only.
- Client log mnt-a: `put quota backpressure ino=1036289 ... ticket=268` 02:15:02.800,
  `fsync: quota exceeded` 02:15:09, `dcache-flush-fail` 02:15:15. Ticket 267 logs nothing
  because it succeeded.

## Mechanism

K90 ([unmount drain](k90-unmount-drain-20261104.md)): a write refused with ENOSPC leaves
its chunk dirty in the mount (`dcache_store`) and the mount PUTs it again once room
appears (`efs_dcache_flush_ino`, the reclaim pool `dcache_reclaim_main`, the pressure
drain `fuse_pressure_main`); an unmount with the member still full gives it up
(`efs_dcache_discard_dirty`, `force-discard ... cause=unmount-enospc`, exit 1). Node 3's
admission (`src/server/store.c` quota check, `server_phys_admit_locked` in
`src/server/reclaim.c`) accepts whatever fits the ledger. So the fragment of room belongs
to whichever PUT arrives first. The scenario kept mount A across the unlink, so A's held
chunk of the last fill file could arrive before `room-0`. The small-cluster experiments
(unlink, no new write, 30 s) show the chunk does not land by itself; a client event
flushes it. Which event did in the gate run is not shown.

## Fix

`tests/k33_daemons.py` `give_up_held(m)` unmounts and checks the exit code against the
`force-discard` lines (exit 0 and none, or exit 1 and some). `room_scenario` calls it on
mount A and remounts before the unlink, so no refused chunk is held when the room appears.
`ok then ENOSPC` is unchanged; no wait, bound or sleep was changed.
`tests/test_b6_room_claimant.py` (fast suite, FUSE; NOTRUN without it) pins both
halves at 4 MiB on node 3: A given up (exit 1, one chunk named), a fresh mount's `room-0`
lands in the first freed fragment, and B's held chunk lands in the second when flushed.
Budget 35 s (28, 26, 26 s solo on amd, 1.25 times the slowest rounded up to 5 s).

## Results

| Check | Result |
|---|---|
| New test on devel's helper module | rc 1, `ImportError: cannot import name 'give_up_held'` (0 s) |
| New test on this commit, solo, 3 runs | rc 0, 28 / 26 / 26 s |
| B6 full, devel test with backtrace, 20 runs, 8-job `make test` loop beside it | 0 failures, 172-186 s |
| B6 full, this commit, same time, same load | 0 failures, 185-193 s |
| Load loop (`EFS_TEST_JOBS=8 EFS_TEST_TARGET=test scripts/make_test_summary.sh`) | 124 rounds, 0 FAIL; rounds 7, 17, 102 `INCOMPLETE` (NOTRUN 3, 3, 10 against the usual 2 host limits), not looked into |
| `make test` on this commit | rc 0, 160 s |
| `python3 scripts/tests_for.py server/store --run` | rc 0, 39 s |

The load was low: load average 1.3-3.1 on 32 CPUs, against about 160 on fstor007. The
earlier fstor007 runs (20 solo on a clean devel build, plus three by the coordinator) also
passed. **So the failure rate before the fix is 0 of 40 here; the claim that the fix
removes the failure rests on the fixture trace and the K90 contract, not on a
before/after rate.**

## Reproduction

```
git archive fd8fffe23 | ssh amd 'mkdir -p ~/efs-taskFE/e && tar -x -C ~/efs-taskFE/e'
# on amd, in that tree, TMPDIR set to a private directory:
make -j16 all tests/faults/efsd
python3 tests/test_b6_room_claimant.py                  # fast, about 27 s
EFS_B6_MODES=full python3 tests/test_b6_unavailable.py  # about 185 s, 20 times
EFS_TEST_JOBS=8 EFS_TEST_TARGET=test sh scripts/make_test_summary.sh   # the load, in a copy
```

Cleanup: `rm -rf ~/efs-taskFE`.

## Raw output

amd `~/efs-artifacts/taskFE-20261110/` (plan, loop and load logs, step 7 output, the
three claimant runs); `SHA256SUMS` itself has sha256
`2c58dc3ff9008466801415357439f3467ede750bd4896e15c2308d8be7c4c889`. The fstor007 fixture
copy `/tmp/efs-taskFE-fx` is deleted at the end; the original archive above stays.
Findings notes: the session notes folder `taskFE-fixture-findings.md`.

## Not done

- The event that sent the held chunk's PUT in the gate run (reclaim pool, pressure drain
  or flush) is not identified; the baseline loop never failed, so the backtrace build
  captured nothing.
- No heavy-load reproduction (fstor007's load was about 160) and no run on fstor007: it
  was held for merge gates, and the proof moved to amd by the coordinator's instruction.
- Why load rounds 7, 17 and 102 of the amd `make test` loop reported extra NOTRUN rows.
- No live gate; docs and graph checks are listed in the commit that records this page.
