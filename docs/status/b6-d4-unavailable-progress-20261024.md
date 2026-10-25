# B6 · D4 · Unavailable fragment destination and bounded progress (Oct 24, 2026)

**Purpose.** Turn Stage 2 checklist items **B6** (a deliberately
unavailable fragment destination) and **D4** (bounded progress with
packing, indexing and KV maintenance active) of
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md) into
bounded, deterministic gates, and fix what they find.

**Verdict: PASS (scoped, loopback regression evidence); real-host gates
NOTRUN (parent).** Branch `claude/devel-b6-d4-4f65ac` on `merge-wave5`
(`376369aa8`), not merged, not pushed. Code commits `74e03592a`, `c5624116a`, `834f0b7ec`.
xorinox `make -k test-all` at `834f0b7ec`: **exit 0, 0 FAIL** (454
`python3`/`./` test commands, 14 NOTRUN lines, all host limits). Two defects found and fixed
(a packed read waited out a slow member; the first `.efs` catalog of a
just-formed member waited a whole refresh interval and every query on it
failed EIO), one broken base test repaired. No repair beyond W154 is
claimed. Single-host loopback daemons are regression evidence, not
qualification (A4).

## B6: one destination unavailable

`tests/test_b6_unavailable.py` (make test-long). Three loopback members
(tests/faults/efsd), so every stripe and every packed segment has a fragment
on node 3, the member made unavailable. Ordinary files 300000 B (three
128 KiB chunks), packed files 1 B to 16 KiB (packer minimum age 500 ms),
an in-flight file of 2 MiB + 5 B. Writes through a mount on node 1; every
read check is a fresh mount.

| Mode | How node 3 is made unavailable |
|---|---|
| down | orderly stop (SIGTERM) |
| refuse | up; new hook `EFS_FAULT_DEST_FILE` (`src/server/dest_fault.h`, EFS_FAULTS builds only) answers fragment PUT/GET, COMMIT, segment PUT/GET and rebuild PUT with an error |
| slow | up; the same hook delays those operations 8 s (past the 2 s read fragment, 5 s read pair and 2 s PUT drain deadlines); one fsync step delays COMMIT 65 s (past the client's 60 s COMMIT deadline); one read step delays GETs 45 s (past the whole read deadline) |
| full | started with a 16 MiB quota and filled by real writes (B5's quota refusal, no hook) |

Each mode: files written with all up (ordinary R, packed P, unsynced F);
fault on while I0 is being written; read every R and P; fsync every F;
write and fsync new W (ordinary) and Q (small); orderly restore (fault off,
node 3 stopped and started; full: restarted with a 1 GiB quota); W154 must
clear the degraded state; every acknowledged file exact; then the whole
cluster stopped in order and started, every acknowledged file exact again.

Measured in the development runs `b6-<mode>-4` (this test, code of
`74e03592a`); the gate run's counts are under Gate.

| Check (bound) | down | refuse | slow | full |
|---|---|---|---|---|
| Reads during the fault: exact, never wrong/short (≤ 30 s each) | exact, max 0.16 s | exact, max 0.08 s | exact, max 5.02 s | exact, max 0.02 s |
| Reads with GETs 45 s slow (≤ 12 s each) | — | — | exact, max 5.02 s | — |
| In-flight write I0 (≤ 90 s) | OK, 0.65 s | OK, 0.48 s | OK, 12.5 s | — |
| fsync of F: success only with proof (≤ 75 s) | OK, 13 stripes marked | OK, 13 marked | OK, 3 marked (the 60 s COMMIT step), others proven after 8 s | OK, 0 marked (node 3 durable) |
| Writes W, Q during the fault (≤ 90 s each) | OK, max 0.06 s | OK, max 0.26 s | OK, max 4.13 s | ENOSPC (all 7), no degraded acknowledgement |
| Degraded acknowledgements (all name node 3) | 30 | 23 | 30 | 0 |
| `Degraded chunks` during the fault, `Protection: DEGRADED` | 17 | 34 | 10 (and W154 already rebuilding onto the slow member: 7, 9, 17 rebuilds during the fault in three runs) | 0 (`full`) |
| Files packed during the fault (packer needs all three, before K37; since K37 a segment may be written on two fragments and marked, see [b6-k37-merge-tests-20261024.md](b6-k37-merge-tests-20261024.md)) | 0 | 0 | 4 (waited 8 s per segment fragment) | 0 |
| W154 after the restore: count 0, `Protection: full` (≤ 180 s) | 4.1 s, 43 rebuilt | 22.3 s, 43 rebuilt (10 s node backoff after refused rebuilds) | 2.1 s, 32 rebuilt | 1.1 s |
| Packer resumes, Q packed (≤ 120 s) | 9.8 s | 6.0 s | 0 s (packed during) | 30.7 s (30 s backoff after the refused segment) |
| Acknowledged files exact after restore and after whole-cluster restart | 22 | 22 | 22 | 48 |

A slow member is up and answers late: W154 rebuilds its missing fragments
during the fault (its rebuild PUTs wait 8 s and succeed), so its count can
be back at 0 before a sample; the test then accepts the rebuilds the
leaders report as the degraded state shown and cleared (gate run 2 failed
on the stricter check; see Gate).

`full` writes fill node 3 with 29 files of 1 MiB before the 30th is refused
ENOSPC; afterwards every write is ENOSPC (three members: no member to place
around, B5) until the restart with a larger quota.

**Fixed: packed read waited out a slow member.** With node 3's segment GETs
delayed past the read deadline, a packed file's read took the whole 30 s
budget and failed `EBUSY` (`efs-fuse read: resource busy (efs_rc=-13)`)
although the other two segment fragments were available
(`src/common/packed_reader.c` reads the fragments in order and stops at
BUSY to keep the caller's deadline; `packed_fetch` had no per-fragment
bound). Chunk reads already bounded each fragment (2 s). Now
`packed_fetch` (`src/client/read.c`) gives each segment fragment the same
2 s, and a fetch past its own bound, not the read's, is that member
failing (NET): the reader takes the next fragment. Red run (before the
fix, same test): `P0` 30.0 s, EBUSY; after: `P0` 2.01 s exact.

**Real hosts:** `scripts/qualify.py` gate **`b6-member-down`** (needs
`--allow restart` and `control.stop_node/start_node`): the down mode on a
described cluster (writes before, packing wait on the pack backlog,
orderly stop of the last non-seed member, reads from the second mount,
fsync, writes, `Protection: DEGRADED`, start, W154 back to `full`, every
acknowledged file exact through both mounts). Refusing, slow and full
members need the EFS_FAULTS daemon or a quota change and stay in the
loopback test. Checked on a driver-owned loopback cluster (`qualify.py run
--loopback 3 --allow restart`): PASS, 22 stripes marked, W154 cleared in
10.0 s, 19 files exact.

## D4: bounded progress under background work

Driver `scripts/d4_progress.py` (pluggable into `scripts/qualify.py` as
**`d4-progress`**, default plugin). Load: N writers create small files (1 B
to 8 KiB) in their own directories; every eighth create also unlinks an
older file (segment holes, GC work). Foreground at the same time, through
the second mount: write+fsync 1 MiB, read an earlier file exactly, stat and
list a load directory, `.efs/stats --json`; after the load only reads and
queries, so the drain measures background work alone. Sampled every
second: efs-fuse RSS per mount, efsd RSS per member, `raft-status`
(kv_mt_bytes/kv_mem_budget, kv_l0_bytes/kv_l0_cap, new
kv_flushes/kv_compactions, unapplied), `pack-compact … 0` (new per-group
backlog line), the `.efs` snapshot (generation, pending changes, the age of
the oldest unindexed change). A foreground operation over its deadline, or
no packer/index progress for `--stall-s`, saves raft-status and the
efs-fuse task stacks to `stall-N.txt` and fails the run. A `.efs/stats`
answer of "not indexed yet" (ENODATA, the answering member's first catalog
not yet published; each member builds its own) is counted and tolerated
only within 60 s of the start (`--index-ready-s`, initial catalog creation,
D3); later it is an error, as is any other failure (EIO among them).

Loopback version `tests/test_d4_progress.py` (make test-long): three
members (tests/faults/efsd), 8 writers for 45 s, packer minimum age 1 s,
index refresh every 2 s, 256 KiB memtables (`EFS_FAULT_MEMTABLE_BYTES`; the
fault daemon now links fault-built `kv_lsm.o`/`kv_compact.o` so the KV hooks
apply). Measured in the gate run at `834f0b7ec` (two earlier runs of the
same test in brackets):

| Bound | Stated | Measured |
|---|---|---|
| Every foreground op within its deadline | 10 s | 291 ops, 0 over; max write+fsync 0.87 s, read 0.18 s, stat+readdir 0.03 s, `.efs/stats` 0.23 s [max 1.05 s] |
| Client RSS (each efs-fuse) | ≤ 1024 MiB | 147.4 MiB [144.0, 145.1] |
| Server RSS (each efsd) | ≤ 2048 MiB | 73.8 MiB [60.8, 65.6] |
| KV memtable | kv_mt_bytes ≤ kv_mem_budget (design bound) | 182,771 ≤ 134,217,728 |
| KV L0 | kv_l0_bytes ≤ kv_l0_cap | 219,242 ≤ 1,073,741,824 |
| Pack backlog (candidate-index entries) | ≤ 50,000 | max 117 [94, 110] |
| Index: age of the oldest unindexed change | ≤ 60 s | max 1.8 s [0.8, 2.2] |
| Progress during the load | packing, index, flush, compaction > 0 | 3179 files packed, 22 index generations, 31 flushes and 372–378 compactions per member |
| Drain after the load (pack backlog and index pending 0) | ≤ 150 s | 4.4 s [3.4, 4.5] |
| Every acknowledged file exact | all | 2935 |

Through `qualify.py run --loopback 3` (product `./efsd`, default 4 MiB
memtables, 60 s index interval, 16 writers for 90 s, packer minimum age
1 s; run `qual-3`): PASS; 851 foreground ops, max 0.91 s; client RSS
146.0 MiB, server RSS 99.3 MiB; kv_mt max 4,114,196 ≤ 134,217,728;
L0 max 2,990,970 ≤ 1,073,741,824; pack backlog max 4071, oldest
eligible candidate waited at most 80.5 s (the packer yields to foreground
load by design); 9334 files packed, 1 index generation (the 60 s
interval), 5 flushes and 77 compactions per member; index pending age max
59.8 s; drained in 31.1 s; 13,268 load files and 152 foreground files exact
(the verification read 13,420 files in 108.6 s).

**Fixed: first `.efs` catalog of a just-formed member, and EIO meanwhile.**
The D4 gate on a fresh qualify loopback cluster failed 104 of 104
`.efs/stats` in its first 60 s with `[Errno 5]`. Daemon log: node 1's first
catalog capture ran just before the member counted itself formed (the
commands-policy row lands first) and was refused `EFS_ERR_UNFORMED`
(`query-refresh: rc=-29 … initial=1 … stage=capture`); only BUSY and
NOT_PRIMARY got the 1 s initial retry, so the next attempt waited the whole
refresh interval (60 s), and `server_raft_host_query_read` returned the
recorded -29 (client: EIO) instead of "not indexed yet" (ENODATA, which the
command waits on). UNFORMED is now an unsettled authority like BUSY and
NOT_PRIMARY (`efs_query_refresh_unsettled`, `src/server/query_refresh.h`):
prompt retry, ordinary failure count, NODATA to queries. Tests:
`tests/test_query_refresh.c`, `tests/test_query_target_route.py` (unit) and
`tests/test_query_first_catalog.py` (daemons, production 60 s interval; the
refusal is a race at formation, so it forms up to 12 clusters until a member
logs it). Red before the fix: formation 6 hit the race and node 1 had no
catalog 15 s after formation. After: 2 of 5 runs hit the race, every first
catalog within 2.2 s, no EIO; the qualify run `qual-3` hit it too and
passed with 0 query errors.

No other repeated stall was seen; `stalls_dumped` 0 in every run.

**Finding, not changed (Stage 3, P):** cold reads of packed small files
are slow: the verification read of 13,420 files took 108.6 s (8 ms each)
in `qual-3` and of 11,872 files 393.9 s (33 ms each) in `qual-1`. Each read
fetches two segment fragments of `geometry / 2` bytes, the whole segment
image (`src/common/packed_reader.c`); not profiled further. Bounded and
correct; recorded for Stage 3.

## Base defect repaired

`tests/test_get_integrity.py` (READ_VERIFY branch) did not compile on
`merge-wave5`: K34 added `read_reprobe_down` to the decode path the test
extracts from `read.c`, and the test neither extracted nor stubbed it
(`make test` fails on the base). The test now extracts it (and the new
`packed_fetch_run`) and stubs `efs_client_node_reprobe`.

## Interfaces changed (no compatibility, AGENTS.md)

- `efs_msg_raft_status_reply` gains `kv_flushes`, `kv_compactions`;
  `efs-mgmt raft-status` prints them on the `bound` line.
- `efs_msg_pack_compact_reply` gains per-group `backlog`, `lag_ms`,
  `backlog_age_ms`; `efs-mgmt pack-compact` prints
  `  backlog group=G candidates=N lag_ms=L counted_ms_ago=A`.
- Both are length-checked replies: an older peer's reply is refused
  ("no current-format reply"), never misread.
- New fault hook `EFS_FAULT_DEST_FILE` (A6 inventory row). No new KV kind,
  Raft tag or wire message type.

## Gate

- **Tree:** `git archive 834f0b7ec` under `~/efs-b6d4/gate` on xorinox,
  `make -j16 EFS_GIT_ID=834f0b7ec all`, then `make -k EFS_GIT_ID=834f0b7ec
  test-all` in `screen`, `TMPDIR=~/efs-b6d4/gtmp` (trees deleted after).
- **Environment:** xorinox, Rocky Linux 10.2, kernel
  6.12.0-211.67.1.el10_2 x86_64, 16 CPUs, gcc 14.3.1, Python 3.12.15, FUSE
  (`/dev/fuse`, `fusermount3`), TCP only, direct fragment I/O (A7),
  loopback daemons on free ports (never 17xxx), 1 GiB quotas (16 MiB for
  the full member), no other load on the host.
- **Result:** `EXIT 0` (21:13–21:47 UTC, 34 min). `test_b6_unavailable.py`
  PASS (334.8 s: down 31 → 0 in 4.6 s, refuse 9 → 0 in 23.4 s, slow 15 → 0
  in 2.6 s, full ENOSPC with 0 degraded, every acknowledged file exact after
  the restore and a whole-cluster restart); `test_d4_progress.py` PASS
  (94.9 s, table above); `test_query_first_catalog.py` PASS (12 formations
  without the race in this run, every first catalog within 2.0 s);
  `test_d4_driver.py`, `test_query_refresh`, `test_query_target_route.py`,
  `test_get_integrity.py`, `test_qualify_driver.py` PASS. NOTRUN lines: RDMA
  ×9 (no device), `~/git/cluster` absent, valgrind absent,
  `test_stalled_compactor` (needs `make EFS_FAULTS=1`), and the race case
  of `test_query_first_catalog.py`.
- **Earlier gate runs (kept):** run 1 at `74e03592a` failed only
  `test_d4_progress.py`: two `.efs/stats` answers of "not indexed yet" at
  the start counted as errors; after the UNFORMED fix that answer replaces
  EIO before a member's first catalog, so the driver now tolerates it within
  the initial 60 s window only (`c5624116a`). make stops `test-long` at its
  first failing line, so run 1 did not reach the later tests. Run 2 at
  `c5624116a` failed only `test_b6_unavailable.py` slow mode: node 3 (the
  slow member) led both groups and W154 rebuilt its marks before the count
  was sampled (`rebuilt=23`); the slow-mode check now accepts those
  rebuilds (`834f0b7ec`).
- **Raw output** (outside Git): xorinox `~/efs-artifacts/b6-d4-20261024/`
  (283 files, 16 MiB: `env.txt`, `build.log`, `test-all.log`, `run1-*`,
  `run2-*`, `dev-runs/` with every development run's logs and JSON
  results, qualify runs `qual-1` to `qual-3`, red runs `b6-slow-2.log` and
  `qfirst-base.log`; `PRUNED.txt`; `SHA256SUMS` sha256
  `f5e519ff95fb445ea88bd1c37e1a3c7a00ce666c692e12dc05ce5987a6ef8840`).
- **Docs gate:** `cd docs && python3 gen-architecture-full.py`, then
  `python3 docs/check-architecture.py`: all 5 checks pass.

## Reproduce

On a Linux host with FUSE, from this commit:

```bash
make -j16 all tests/faults/efsd && make test-all
```

```bash
python3 tests/test_b6_unavailable.py
```

```bash
EFS_D4_ARTIFACTS=/tmp/d4 python3 tests/test_d4_progress.py
```

```bash
EFS_PACK_MIN_AGE_MS=1000 python3 scripts/qualify.py run --loopback 3 --out /tmp/q --work /tmp/qw --allow restart --gates identity,d4-progress,b6-member-down
```

Real cluster (parent): add `--allow restart` and `control.stop_node` /
`start_node` to the description for `b6-member-down`; `d4-progress` runs on
a mount host (fixture keys `d4_load_s`, `d4_threads`, `d4_fg_deadline_s`,
`d4_drain_s`, `d4_client_rss_mib`, `d4_server_rss_mib`, `d4_pack_backlog`,
`d4_index_age_s`; defaults 600 s, 16, 10 s, 900 s, 1024 MiB, 4096 MiB,
200,000, 300 s). Clean up loopback runs by removing `--out`/`--work`.

## NOTRUN and limits

- **Real-host B6 (`b6-member-down`) and D4 (`d4-progress`) runs: NOTRUN**
  (parent session), including long soaks, RDMA and separate hosts.
- Refusing, slow and full members are loopback-only (EFS_FAULTS daemon,
  quota restart). A member that never returns is not repaired: W154 only
  rebuilds on return; no repair beyond W154 is claimed.
- Three members cannot place around an unavailable member: writes are
  acknowledged degraded (down, refuse, slow) or refused ENOSPC (full).
- The index pending age assumes synchronized clocks on real hosts (server
  `CLOCK_REALTIME` sample time).
- Packing under a slow member is not refused: the packer waits for the slow
  member's durable segment fragment (all three required).
