# D4 drain after a rewrite load and packer put errors at a high rewrite rate (Oct 25)

**Purpose.** Two packer progress findings from the fixed-working-set D4 load
in the [F1 follow-ups](f1-followups-20261025.md) (Open 5 and Open 2), for
release check D4 (bounded progress with packing, indexing and KV maintenance
active) in the [alpha 1.0 checklist](alpha-1.0-2plus1-checklist.md):

1. After a rewrite load, pack candidates stayed unpacked for minutes (lag up
   to 346 s, drains of 391–415 s against 3.5 s after the create load). The
   75 s plateau test missed its 150 s drain bound, so `tests/test_d4_plateau.py`
   had been taken out of `make test-long`.
2. The flake-sweep resend of a fragment refused `BUSY`, `NOT_PRIMARY` or
   `AGAIN` (4 × 50 ms) lost to a follower's ReadIndex refusing for longer:
   5 packer put errors in a 60-minute run.

**Verdict.** Branch `claude/interesting-mclean-65362a` (devel `1fc526f66`
with `claude/devel-f1-followups-ae2deb` merged, `ad0b65761`). Code commits
`dd0abe363` (packer) and `b7bf18bdd` (truncate sweep); tested tree
`9670b895b`.

| Item | Result |
| --- | --- |
| 1a. Lease hypothesis | **Refuted.** The lag the driver reports counts only candidates that already passed the lease and minimum-age checks |
| 1b. Cause 1: one inode twice in a segment | **FIXED** (`dd0abe363`). A file republished in a later pack epoch before it packed had two candidate keys; a segment holding both was refused by `efs_pack_build`, silently, every pass. New test `test_pack_dup_candidate.py`: red 4 of 4 on the unfixed tree, green 6 of 6 |
| 1c. Cause 2: truncate-sweep backlog | **FIXED** (`b7bf18bdd`). With cause 1 fixed the 75 s plateau test still missed 150 s (4 of 4): the D25 truncate sweep retired about 5 inodes a second, and each sweep was a metadata change the index drain waited for. Now drain 22–25 s alone, 54.5 s after the 60-minute load |
| 2. Packer put errors | **FIXED** (`dd0abe363`). The resend is bounded by 2 s, not 4 tries. K37 case 1c (800 ms refusal): green; with the old 4-try loop: FAIL. 60-minute runs: **0 put errors** (amd 9 resends, intel1 1, all accepted on the first resend) |
| Plateau test back in `make test-long` | Done: budget row 180 s / 2 slots, `NOT_WIRED` entry removed; dup-candidate test 105 s / 1 slot; budgeted gate 1439 s of 1440 |
| 60-minute run (intel1, `9670b895b`) | **Drain 54.5 s ≤ 150, 0 put errors, every file exact, 193 642 rewrites, 0 errors.** The run's overall verdict is FAIL on one RSS criterion: the load mount's efs-fuse grew 5.4 % over the last third (bound 5 %), the open F1 RSS item. All three efsd plateau (+1.0 to +1.7 %) |
| `make test-all`, 8-job pool (xorinox, `9670b895b`) | **PASS with host limits:** 500 commands, 0 FAIL, 13 NOTRUN (RDMA device, `~/git/cluster` scripts, an `EFS_FAULTS=1`-only case, valgrind/sanitizer runtime markers), 760 s, exit 3 (INCOMPLETE); no OVERBUDGET. Plateau 158 s of 180, dup candidate 71 s of 105, K37 199 s of 335 |
| New finding: reclaim lags under the rewrite load | **FIXED Oct 26** ([evidence](d4-reclaim-20261026.md); residual open). Was **OPEN.** Quota use climbs linearly through the load (intel1 52 % of 8 GiB at 60 min, F1's xorinox runs 31 %); amd's higher rate reached ENOSPC at about 45 minutes. Old generations are reclaimed after the load (64 MB left per store). See [Open](#open) |

## Item 1: the drain

### Leases were not the cause

The packer's selection skips a file while any lease on it exists. But the
lag it reports (`lag_s`, the driver's `pack_lag_ms`) is the oldest wait over
the files that passed that check and the minimum age (`src/server/packer.c`,
`oldest` is taken after `efs_lease_any`). A lag of 346 s therefore meant
lease-free, quiet files left unpacked.

### Cause 1: one inode twice in a segment

The F1 diagnostic run A's leader log (raw at xorinox
`~/efs-artifacts/f1-close-20261025/d4p-diag-A/`) shows the stall pass by
pass, with the cluster idle:

```
packer: group=0 ... lag_s=330 ... candidates=28 eligible=28 segments=1 unindexed=0 packed_total=35198 ... seg_ms=0
```

Every 200 ms pass found 28 eligible files, built one segment job and packed
none, with no put, read or admit error counted. `pk_segment` has silent
exits before ADMIT, and `efs_pack_build` refuses a segment holding one inode
twice. The candidate index key is (group anchor, epoch, inode), epoch =
publish time / 60 s (`EFS_PACK_EPOCH_S`). A small file republished in a
later epoch before it packed gets a second key with the same inode
generation, and both validate against the inode row. When the backlog fits
in one segment, as after a load, both keys go into the same segment and it
fails every pass until a rewrite changes the file.

**Fix** (`dd0abe363`): selection keeps one entry per inode, its oldest. Once
the file is packed its other keys are no longer candidates and are
unindexed. A refused build is logged (`packer: segment gN of N files not
built (rc=...)`).

**Test** `tests/test_pack_dup_candidate.py` (`make test-long`): three efsd
and one efs-fuse on loopback, packer minimum age 10 s. 24 small files are
rewritten in place every 2 s until a round lies wholly in a later epoch
than an earlier round, so each file has two candidate keys. The test
requires the backlog to reach 0 within 20 s of the files becoming quiet,
every file packed once, no put or read error, and the last contents. On the
unfixed tree the backlog stays at 48 (`eligible=48 segments=1
packed_total=0 seg_ms=0`, the D4 signature).

| Tree | Result |
| --- | --- |
| `ad0b65761` (unfixed) | **4 of 4 FAIL** (3 with the first version of the test, 1 with the committed one): backlog 48, 0 packed |
| `dd0abe363`, first version | 3 of 3 PASS, backlog 0 1.0 s after quiet |
| `b7bf18bdd` + committed test | 3 of 3 PASS (41–60 s), backlog 0 1.0 s after quiet |

### Cause 2: the truncate-sweep backlog

With cause 1 fixed, the 75 s plateau test still missed its drain on
xorinox (2 of 2, then 2 more with logs kept). Its pack backlog reached 0
within 5 s (lag at most 0.9 s), but the index's pending changes stayed at
4–39 at every 2 s refresh, with no client writes, no packing and no
compaction. The index counts KV metadata revisions per shard.

A diagnostic build (not committed; `diag/diag*.py` in the raw evidence)
counted revision bumps by KV kind, then tagged each LANE write with the
Raft command being applied. After the last client write, each member's only
metadata writes were LANE puts from `EFS_MD_CMD_TRUNC_SWEEP`, about 735 per
member at ~6 a second, on mostly distinct inodes (560 once, 54 two to five
times). That is the D25 truncate sweep (`host_gc_trunc_pass`) working off
the load's truncates (`O_TRUNC` overwrites and truncate-then-rewrite). The
pass took 16 `TRUNC_PENDING` inodes and needed three GC passes per inode:
TRUNC_SWEEP, then SWEEP_DONE once the lane stamp showed it, then RETIRE.
That came to about 5 inodes a second.

**Fix** (`b7bf18bdd`): `host_bg_propose` returns once the local replica
applied the entry, so the pass reads the lane stamp right after its
TRUNC_SWEEP and records SWEEP_DONE and RETIRE in the same pass. RETIRE
still refuses unless every active lane is done for the target. The pass
takes 32 inodes, 128 proposals.

| Tree (xorinox, 75 s plateau test) | Drain |
| --- | --- |
| `dd0abe363` | not drained in 150 s, 4 of 4 (pack lag ≤ 0.95 s) |
| + one-pass sweep, 16 inodes | 52 s, 59 s |
| + 32 inodes, 128 proposals (`b7bf18bdd`) | 22 s, 25 s; test PASS in 142 and 144 s |
| `9670b895b` in the 8-job gate | PASS, 158 s |

The [F1 drain table](f1-followups-20261025.md#drain-after-a-long-rewrite-load)
attributed the long drains partly to index changes from segment
compaction. Here compaction ran only briefly in the drain (`compacted`
flat); the index changes were the truncate sweep's.

## Item 2: time-bounded fragment resend

`fc79fe086` (flake sweep, on devel since wave 9) resends a fragment refused
`BUSY`, `NOT_PRIMARY` or `AGAIN` 4 times 50 ms apart before the segment is
written degraded. **Fix** (`dd0abe363`): the resend repeats 50 ms apart
until 2 s after the first round, one deadline for the segment's three
fragments, the client's per-fragment bound. The log now gives the tries and
the time (`resent after rc=-15: rc=0 tries=16 ms=821`).

`EFS_FAULT_SEGMENT_PUT_TRANSIENT` (EFS_FAULTS builds) takes a millisecond
count in its flag file: the member refuses every segment PUT that long.
K37 case 1c refuses node 3's PUTs for 800 ms and requires a resend accepted
after at least 600 ms, no degraded write and no put error.

| Tree | `test_k37_segment_rebuild.py` |
| --- | --- |
| `dd0abe363` | PASS (case 1c: 16 tries, 821 ms), 227 s |
| `dd0abe363` with the 4-try loop restored | **FAIL at case 1c**: no resend accepted after the 800 ms refusal |
| `9670b895b` in the 8-job gate | PASS, 199 s |

60-minute runs: amd (`dd0abe363`) counted 0 put errors with 9
`NOT_PRIMARY` resends, each accepted on its first resend after 54–315 ms;
intel1 (`9670b895b`) 0 put errors, 1 resend (285 ms). F1's run 6 had 5.

## The 60-minute fixed-working-set runs

`EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_QUOTA=8G EFS_D4_KEEP=1
EFS_D4_ARTIFACTS=... python3 tests/test_d4_plateau.py`, default drain bound
150 s.

| Host, tree | Rewrites (errors) | Drain | Put errors | RSS (last-third growth) | Verdict |
| --- | --- | --- | --- | --- | --- |
| amd, `dd0abe363` (alone) | ENOSPC from ~45 min, 4107 errors | 1.1 s (after the failures) | 0 | — | **FAIL**: quota full (reclaim lag, Open 1); 13 `.efs/stats` over 10 s |
| amd, `9670b895b` (shared) | — | — | — | — | **NOTRUN**: amd went down about 40 min in (also running another session's gates and an e2busy loop); cause unknown |
| intel1, `9670b895b` (alone) | 193 642 (0) | **54.5 s** | **0** | efs-fuse load **+5.4 %**, fg +0.0 %; efsd +1.7 / +1.4 / +1.0 % | **FAIL on the RSS criterion only** |

intel1 details: 19 639 foreground operations, max 1.28 s (write+fsync
1 MiB), `.efs/stats` max 0.94 s; every one of 2080 files exact; pack lag up
to 64 s in the first 15 minutes (the initial pack of the set at the busy
rate), at most 1.0 s after that; index pending age at most 2.3 s; peak RSS
efs-fuse 300 MiB of 1024, efsd 91 MiB of 2048.

## Open

1. **Reclaim lags under the rewrite load (new, D4).** Quota use on the
   fixed working set rises linearly through the load: intel1 0.1 → 52.1 %
   of 8 GiB over 60 minutes at 193 642 rewrites. F1's xorinox runs show
   the same, 31 % at 60 minutes. On amd it reached ENOSPC at about 45
   minutes. Mid-run on intel1 one node held 14 010 fragment files (916
   MB) against about 70 MB live: chunks kept up to 29 superseded
   generations, some over 10 minutes old. After the drain, verification
   and shutdown the store held 64 MB. So old generations are reclaimed,
   but slower than a high rewrite rate produces them. Not attributed yet:
   the next measurement is the reclaim path's rate (journal, GC reap) per
   rewrite. Owner: D4/GC. **Cause found and fixed (Oct 26,
   [evidence](d4-reclaim-20261026.md)):** not GC throughput. Abandoned
   uploads wait in the D31 orphan ledger until their mount's session
   ends, and each resizing rewrite abandoned its whole first upload: the
   REPORT host passed the client's stale span list with an unconditional
   tail image, and the fold check refused the unit (`66bfc2786`). The
   60-minute run now keeps stored data at 118–278 MiB per node. About
   3.5 % of `O_TRUNC` overwrites still abandon an upload (open there).
2. **efs-fuse RSS on the load mount** grew 5.4 % over the last third in
   the intel1 run (criterion 5 %). This is the F1 plateau item in the
   [F1 follow-ups](f1-followups-20261025.md); efsd plateaued in this run.
   Owner: F1.
3. amd went down during the second 60-minute run; nothing here attributes
   it.

## Environment

- **xorinox**: AMD Ryzen 9 7940HS (16 CPUs), 60 GB RAM, Rocky Linux 10, gcc
  14.3.1. Red/green test trees, the diagnostic build, the 75 s plateau runs
  and the gates; shared at times with another user's K22 probe
  (`efs-taskB`) and, early on, the efsd RSS session.
- **intel1**: Intel Core Ultra 7 256V (8 CPUs), 15 GB RAM, kernel
  6.12.0-211, the 60-minute run alone. The host's live cluster on 17xxx
  ports was not touched.
- **amd**: 32 CPUs, 119 GB RAM, the first 60-minute run alone; the second
  shared with another session's gates and an e2busy loop.

Every tree was a `git archive` of a commit, built with `make test-bins`;
loopback ports chosen by the tests; stores under `$HOME/efs-tmp` (XFS on
xorinox and intel1, ext4 on amd); buffered I/O as in the D4 tests; packer
1 s minimum age and 200 ms passes, `.efs` refresh 2 s (the D4 test
configuration). nuc ran an 8-job gate for three minutes, stopped at the
efsd RSS session's request.

## Reproduction

```bash
git archive 9670b895b | ssh HOST 'mkdir -p ~/efs-x ~/efs-tmp && tar -xm -C ~/efs-x && cd ~/efs-x && find . -type f -exec touch {} + && make -j"$(nproc)" test-bins'
```

Regression tests, the 75 s plateau test and the gate (about 15 minutes at
8 jobs; `summary.txt` line 1 is the verdict):

```bash
ssh HOST 'cd ~/efs-x && export TMPDIR=$HOME/efs-tmp && python3 tests/test_pack_dup_candidate.py && python3 tests/test_k37_segment_rebuild.py && python3 tests/test_d4_plateau.py && EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh; head -1 summary.txt'
```

The 60-minute run (about 65 minutes; needs the host alone):

```bash
ssh HOST 'cd ~/efs-x && TMPDIR=$HOME/efs-tmp EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_QUOTA=8G EFS_D4_ARTIFACTS=$HOME/d4p-3600 python3 tests/test_d4_plateau.py'
```

Expected: `pack duplicate candidate: PASS`, K37 `1c. ... resent (tries,
ms) ... PASS`, the plateau test PASS with a drain under 150 s, the gate
`FAIL 0`. In the 60-minute run, drain under 150 s and `put_errors=0` in
`d4-samples.json`. The RSS table may still read GROWING for the load
mount's efs-fuse (Open 2).

Red trees: the unfixed packer is `ad0b65761`. For K37 1c, replace
`pk_now_ns() < put_deadline` with `tries < 4` in `src/server/packer.c`.

Cleanup: delete `~/efs-x`, `~/efs-tmp` and the artifacts directory.

Raw outputs:

- intel1 `~/efs-artifacts/d4-pack-drain-20261025/` (64 MB;
  `SHA256SUMS` sha256 `3746e150630774ed3d0741915866c9695ec1554d6e1e2783eb6adf6a31e85714`):
  the 60-minute run's stdout, driver JSON and daemon/mount logs. Its
  runtime stores and build tree were pruned (`PRUNED.txt`).
- xorinox `~/efs-artifacts/d4-pack-drain-20261025/` (96 files, 14 MB;
  `SHA256SUMS` sha256 `fc98ef2ff42935f7ebec783d82efd83afa09e0ae91747f9e572f0d21d39bb629`):
  `runs/` (every red/green and plateau run's output and JSON, the gate's
  `summary.txt` and `timings.tsv`), `kept-logs/` (daemon logs of the kept
  plateau and dup-candidate runs, including the diagnostic runs) and
  `diag/` (the diagnostic patches and build logs).
- amd `~/efs-artifacts/d4-pack-drain-20261025/` (the ENOSPC run): not
  reachable while amd is down. Its numbers above were read before the
  outage.

The xorinox private trees (`~/efs-drain-*`, about 3.4 GB) and `~/efs-tmp`
(457 MB of kept runtime stores) are still there: the session could not
delete them. They are bulk to prune; nothing above depends on them.

## Limitations

- Loopback with the D4 test configuration (256 KiB memtables, 2 s index
  refresh); production-scale checks belong to K22 and qualification.
- One 60-minute run passed the drain and put-error checks (intel1). The
  amd repeat at the fix is NOTRUN.
- The new gate rows leave the budgeted gate 1 s under its 1440 s bound.
