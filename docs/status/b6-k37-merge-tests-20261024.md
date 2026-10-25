# B6 under K37, `test_get_integrity.py`, and a packer unindex race (Oct 24, 2026)

**Purpose.** On `merge-wave6` (`646a43041`) two tests failed:
`tests/test_b6_unavailable.py`, where mode `refuse` asserted that nothing is
packed while node 3 refuses every PUT, and `tests/test_get_integrity.py`,
whose generated stub redefined `read_reprobe_down` (`-Werror`). Decide which
side is wrong, fix the tests, and confirm them on a bare-metal host. Items:
B6 ([b6-d4-unavailable-progress-20261024.md](b6-d4-unavailable-progress-20261024.md)),
K37 ([k37-segment-rebuild-20261024.md](k37-segment-rebuild-20261024.md)).

**Verdict.**

- `test_get_integrity.py`: **PASS**, 3 of 3 runs at `335bfb5a0` (intel1),
  and 3 of 3 at each earlier commit below.
- `test_b6_unavailable.py` at `335bfb5a0`: **PASS**, 3 of 3 runs on intel1
  (all four modes). Mode `full` stays a **known intermittent FAIL**, not
  counted as PASS: a packer race, below, also present on devel
  `9ab6ad74f`. It did not hit in these three runs. Not fixed here: the
  fix is production metadata format work and needs its own branch and gate.
- `make test-all` on intel2 at `335bfb5a0`: `TOTAL 467 FAIL 1 NOTRUN 15 STATUS FAIL (2104s)`. B6 PASS (339.2 s) and
  `test_get_integrity.py` PASS in it. The one FAIL, `tests/test_k33_payload.py`
  (EIO reading a file while a member is down), is a test this branch does
  not touch, on devel's code, and is a known intermittent. **No new FAIL.**
- Branch `claude/admiring-babbage-a2387d` (devel `51bf8ed25` + 4 test
  commits + this page), not pushed. The code under test is devel's.

## Which side was wrong

B6 was written and gated before K37 merged. Its evidence says "packer needs
all three": with B6's tested code the packer cancelled any segment copy
without three acknowledgements. K37 changed that on purpose, and the user
decided on Oct 24 to keep it: a segment copied with two acknowledged
fragments gets a `PACK_DEGRADED` mark before any file is switched in, and
the mark is cleared after a rebuild once the member is back. So B6's
expectation was stale, not the packer. `test_get_integrity.py` extracts
`read_reprobe_down` from `src/client/read.c` itself since the merge, so its
own stub was a duplicate.

Both minimal fixes were already on devel in `12d2a7957` (drop the stub;
accept packing during the fault if `Degraded segments` is above 0, and wait
for 0 after the restore). This branch makes the B6 check exact. The net
change against devel (commits `6dcadfe38`, `8dbe24b69`, `f0644c80c`,
`335bfb5a0`):

| Check (step 5, during the fault) | Modes |
| --- | --- |
| Every new `packer: segment … written degraded: fragment f for node n` line names node 3 | all |
| A file packed during the fault implies at least one segment written degraded (devel's check accepted any mark, even one older than the fault) | down, refuse, full |
| Every Q* file (written, possibly packed, during the fault) reads back exact through a fresh mount | down, refuse, slow |
| After the restore: `Degraded segments: 0` (from devel); recorded: how many of those segments have their missing fragment's file back on node 3 | all |

`slow` may pack on three acknowledgements: the packer waits for node 3,
which answers after 8 s.

**Two stricter checks were tried and dropped**, because K37 does not
promise them:

- *`Degraded segments` ≥ the number of segments written degraded while node
  3 is unavailable*, and *each such segment's fragment back on node 3 after
  the restore*. A mark also ends when its segment is retired. Compaction
  merges small segments, during the fault too. Its switch releases the old
  segment, and `PACK_RETIRE` (or the rebuild pass, `segment no longer live,
  mark retired`) drops the mark. Seen on intel1 at `f0644c80c`, mode `down`:
  `packer: compact group=0 sources=2 moved=5/5`, then `segment-rebuild: g0
  id=e1abb5ba39c29801 gen=1 mask=4: segment no longer live, mark retired`
  (`Degraded segments: 1` for 2 written degraded). On intel2 at
  `f0644c80c`, mode `refuse`, 2 of 3 runs: a mark was gone with no fragment
  on node 3 and no retire line. The K37 page documents this ("the old
  segments' records are released by the switch and their marks retire with
  them").
- *A `segment-rebuild: rebuilt …` line per segment* (`6dcadfe38`): the leader
  logs it only after its CLEAR proposal returns OK. A segment retired as
  above has none (intel2 `make test-all` at `8dbe24b69`, mode `down`).
- `8dbe24b69` also waited up to 180 s, not 30 s, for the count during the
  fault. The count is what the group leaders' rebuild passes last reported,
  and under `make test-all` load it lagged (intel1 at `6dcadfe38`, `refuse`:
  the status printed just after the assertion showed 2).

## Runs

| Commit, host | get_integrity | B6 (all four modes per run) |
| --- | --- | --- |
| `6dcadfe38`, intel1 | 3/3 PASS | 1/3 PASS; runs 1–2 FAIL in `full` (the race below) |
| `6dcadfe38`, intel2, `full` alone | — | 3/4 PASS, 1 FAIL (the race) |
| `8dbe24b69`, intel2 | 3/3 PASS | 3/3 PASS |
| `f0644c80c`, intel2 | 3/3 PASS | 1/3 PASS (the dropped per-segment check) |
| `f0644c80c`, intel1, `down,refuse` | — | 1/2 PASS (the dropped count check) |
| **`335bfb5a0`, intel1** | **3/3 PASS** | **3/3 PASS** (349.7, 347.8, 352.6 s) |

At `335bfb5a0`: segments written degraded during the fault were seen in
`refuse` (runs 1, 2: 3 and 2 files packed, 1 segment each, fragment back on
node 3) and `down` (run 2: 4 files, 1 segment, fragment back). `slow` packed
7–9 files on three acknowledgements. W154 restore 2.2–3.7 s (down), 21.0–21.5 s
(refuse, including the 10 s node backoff after refused rebuilds), 3.7–4.2 s
(slow).

## Known intermittent FAIL: B6 `full`, a lost pack candidate

**Symptom.** `AssertionError: full: the packer after the restore: packed 11
of 12`: one of the four Q* files rewritten after the restore is never
packed (B6 waits 120 s). No data loss: the file reads back exact, it only
stays unpacked. Counts: intel1 2 of 3 runs at `6dcadfe38`; intel2 1 of 4
runs of `EFS_B6_MODES=full` alone at `6dcadfe38`. 0 of 6 runs at later
commits (3 at `8dbe24b69`, 3 at `335bfb5a0`). B6 changes nothing in `full`
before that step, and devel `9ab6ad74f` differs from this branch's base
`51bf8ed25` only in documentation, so devel has the same exposure.

**Cause.** In `full` the Q* files are refused `ENOSPC` during the fault and
rewritten with `O_TRUNC` after the restore. A pack candidate is the KV row
`(anchor, epoch, ino) → inode generation`, written with the chunk
publication (`src/meta/meta_apply.c`, W125). The packer scans a Q* file's
candidate while the rewrite has truncated it to size 0, so the candidate
counts as "never" and is dropped. With `EFS_PACK_DBG=1`:
`packer: drop ino=208897 rc=0 mode=100644 nlink=1 size=0 profile=1 ndelta=1 lanes=1`
(and `ino=196609` in another run). The drop goes out as
`EFS_MD_CMD_PACK_UNINDEX`, and `efs_meta_apply_pack_unindex` deletes the key
without a condition. If the rewrite's publication puts the same key again
(same epoch, same inode generation) before the UNINDEX commits, the fresh
candidate is deleted. Nothing indexes the file again (backfill runs only
on a new leader), so it stays unpacked. In the failing intel2 run, the pass
after the restore logged `candidates=3 eligible=0 … unindexed=1`, and only 3
of the 4 Q* files were ever packed. In a passing debug run the drop happened
too, but the publication landed after the delete and the file was packed.

**Proposed fix (follow-up, not done here).** A compare-and-delete UNINDEX.
The candidate value becomes `[inode generation][publication stamp]` (the
publication's proposal time, deterministic on every replica). UNINDEX carries
the scanned value per item and deletes only on an exact match, so a candidate
re-put after the scan survives. That touches the candidate writer and the
backfill index (`meta_apply.c`), the UNINDEX encoding and its decoder
(`packer.c`, `raft_host.c`), and `tests/test_pack_switch.c`. It needs a
fresh format and a regression test that interleaves a re-put between scan
and unindex.

## Gate: `make test-all`

**Final, intel2 at `335bfb5a0`** (`scripts/make_test_summary.sh`, with
`EFS_GIT_ID=335bfb5a0`; the intel2 build of the same commit):
`TOTAL 467 FAIL 1 NOTRUN 15 STATUS FAIL (2104s) BUILD 1s`.

- `tests/test_b6_unavailable.py` PASS (339.2 s), `tests/test_get_integrity.py`
  PASS, `tests/test_k36_recovery.py` PASS.
- FAIL: `tests/test_k33_payload.py` (38.2 s): `OSError: [Errno 5]` reading a
  file through a fresh mount while one member is down (`verify(a, 'node k
  down')`). This branch does not touch it and the code is devel's. The K37
  page records the same EIO as an intermittent of this test. Not
  investigated here.
- NOTRUN, all host limits: `test_cluster_stop.py` (no `~/git/cluster`),
  `test_rdma_memcheck.py` and `test_leak_gate.py` (no valgrind), the RDMA
  path of `test_declaration_gate`, `test_rdma_xprt`, seven
  `test_rdma_segmented` modes (no RDMA device), `test_follower_bound`'s
  compactor case (needs an `EFS_FAULTS=1` build), `test_query_first_catalog.py`'s
  UNFORMED race case (did not occur), and the runner self-test's
  by-design NOTRUN line.

**Earlier gates (superseded), each with the FAILs in tests this branch
does not touch shown:**

- intel1 at `6dcadfe38`: `TOTAL 467 FAIL 1 NOTRUN 15 STATUS FAIL (2265s)`;
  the FAIL was B6 `refuse` at the 30 s count wait (fixed in `8dbe24b69`,
  later dropped).
- intel2 at `8dbe24b69`, stopped at 454/467: 2 FAIL, B6 `down` (the
  per-segment rebuild line, dropped) and `tests/test_k36_recovery.py` step
  11, which wants packed segments in both metadata groups and got
  `segments: (6, 0)`. That depends on placement, so it is a flaky
  expectation in the K36 test. It passed in the other two gates.
- intel2 at `f0644c80c`, stopped at 415/467 before B6: 0 FAIL.

## Environment and reproduction

- **Source:** each commit copied with `git archive` into a private
  `~/efs-b6k37/src`. Build `EFS_GIT_ID=<commit>`, gcc 14.3.1, x86_64,
  Makefile defaults (`-O3 -march=native`), `EFS_BUILD_JOBS=8`. sha256 at
  `335bfb5a0` (intel1): efsd
  `ce87e340f876f77bfd13c221da1a18347e1fc410dec264f17619fbb57b23cca5`, efs-fuse
  `888fc6b031c3912cea6a34996074fd89cb25f535d9c642d843fe5487dcd93291`, efs-mgmt
  `bdf5a8412d2ffff3de6e2946c0aeb9b57ca1ed0acdcbccc6e1a264579eaadc20`,
  `tests/faults/efsd`
  `bb37242b029b2f4a00aeab240177a4403069a78185f5487df9f3e8083b0c3a0e`.
- **Hosts:** intel1 and intel2 (8 CPUs, 15 GB, Rocky Linux 10.2, kernel
  6.12.0-211.67.1.el10_2), loopback TCP only, three daemons each on
  test-chosen ports, stores and `TMPDIR` under `$HOME`, direct fragment I/O,
  `EFS_FAULTS` build for B6. The user's cluster on 17432–17435 and
  `/data1/efs` were not touched.
- **Commands** (tree root, after `make -j8 all tests/faults/efsd`):

  ```bash
  python3 tests/test_get_integrity.py
  EFS_B6_ARTIFACTS=<dir> python3 tests/test_b6_unavailable.py      # ~6 min, needs /dev/fuse
  EFS_PACK_DBG=1 EFS_B6_KEEP=1 EFS_B6_MODES=full python3 tests/test_b6_unavailable.py   # the race, ~1 min; delete the kept dir
  scripts/make_test_summary.sh                                      # make test-all
  ```

## Limitations

- One host per run, loopback TCP: regression evidence, not qualification.
- B6 `full` passes only when the race does not hit.
- The race was identified from logs (`EFS_PACK_DBG` drop lines, pass
  counters) and the unconditional delete in the code. No forced-interleaving
  test was written.
- B6 no longer checks how each degraded segment's mark ends. K37's own
  daemon test (`tests/test_k37_segment_rebuild.py`) covers the rebuild per
  segment, with compaction moved out of its run.

## Raw outputs

Not in Git. Each directory has `SHA256SUMS` (sha256 of that file given):

- intel1 `~/efs-artifacts/b6-k37-335bfb5a0-20261024/` (`2d0eda66…e769`,
  160 KB): final build log, binaries, three get_integrity and three B6 logs
  with `b6-result.json`.
- intel2 `~/efs-artifacts/b6-k37-335bfb5a0-20261024/` (same name, other host): `a115365d…c825`, 168 KB:
  that gate's `summary.txt`, `progress.txt`, `timings.tsv`, `cmds.txt`,
  `build.log` and the `test_k33_payload.py` failure log.
- intel1 `~/efs-artifacts/b6-k37-20261024/` (`42b4c5ae…6dd0`, 128 KB):
  `6dcadfe38` runs and its `make test-all` (summary, progress, timings,
  commands, the failure log).
- intel2 `~/efs-artifacts/b6-k37-20261024-intel2/` (`4354df41…38dc`, 64 KB):
  the four `full` runs, the debug run, and the packer, leader and K37 lines
  of the failing run's daemons with its client log.
- intel2 `~/efs-artifacts/b6-k37-8dbe24b69-20261024/` (`6267a6d2…b9`) and
  `~/efs-artifacts/b6-k37-f0644c80c-20261024/` (`7f6e1cdf…4253`): those
  commits' runs and the failure logs of their stopped `make test-all` runs;
  intel1 `~/efs-artifacts/b6-k37-f0644c80c-20261024-intel1/`
  (`3298f623…6f89`): the `down,refuse` runs and the compaction-retire lines.
- Daemon stores and private trees were deleted.
