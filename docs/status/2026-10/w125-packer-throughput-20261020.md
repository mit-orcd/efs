# W125 packer throughput and busy budget (`445b8b549`) — 2026-10-20

**Gates PASS; copy throughput fixed (idle drain ~1,020 → ~5,000 files/s);
the lag-controlled busy budget keeps a sustained writer's backlog bounded
but costs that writer 37% — foreground target FAIL, policy decision open.
orcd-mini runs `445b8b549c82`; ior on a fresh cluster is at or above the
W124 and pre-change builds.** Linked work: W125 landing follow-up
([devel page](w125-devel-20261019.md)); found by the 2026-10-19 live
profile of the user's tiny-file workload.

## Why

During the user's 16-worker `tiny_files.py` run on orcd-mini (devel
`27c20cd4b`), efsd profiling (180 s, `perf record -F 499 -g`, node9901)
showed the packer at about 1% of efsd cycles but packing only ~100
files/s in total while the writer ran at thousands: unpacked per-file
data had reached 125 GiB per node (17% of quota). efsd then: n1 605% CPU,
n2/n3 ~200%; 62% of cycles in the kernel (scheduler balancing, poll), 8%
xfs, 14% efsd user code (Raft pump/receive/send ~20%, apply 5%, lookups
4%, `kv_mtab_pos` 3.5%). Raw:
node9901 `/data1/efs/prof/w125-live-20261019T215816Z/` (`perf.data`, 947,458
samples; `pidstat.txt`, `top.txt`). The node9900 client capture failed
(perf could not attach to exiting threads).

## Changes (`445b8b549`, on devel)

- Candidate epochs 60 s (were 600 s): inside one epoch the 4,096-key scan
  window is ordered by inode, so a sustained writer's young files crowded
  out eligible ones (56–727 eligible per pass) and hid the real lag.
- Busy rate follows the lag (oldest eligible candidate's wait past the
  minimum age): 50 files/s floor up to `EFS_PACK_LAG_TARGET_MS` (60 s), then
  linear to the idle rate (5,000) over `EFS_PACK_LAG_SPAN_MS` (300 s).
- Copy path: a segment's files are read on 16 threads, the three fragments
  upload in parallel, a pass's segments are built and switched concurrently
  (unique segment ids), passes run back to back while backlog and budget
  remain; foreground commit rate measured over ≥ 1 s windows.
- Log line: `lag_s`, `rate`, `scan_ms`, `read_ms`, `seg_ms`, `pass_ms`.
- Tests and scripts: `tests/test_pack_budget.py` (in `make test`),
  `scripts/w125_pack_drain.sh`, `scripts/w125_budget_ab.sh`,
  `scripts/w125_budget_rates.py`.

## Environment

node9901 private fixtures (`/tmp`): 3-daemon RDMA (`mlx5_0`) clusters,
`--direct-io`, quota 200G per node, one efs-fuse mount, ports 17681–17683
(`w125_p0_cluster.sh`); suites' own 4-node TCP fixtures for GC/expiry.
Builds from `git archive 445b8b549` (normal and `EFS_FAULTS=1`), base
`d9cee1ffd` (`/tmp/w125-base2/src`). orcd-mini: 3 efsd on node9901 (RDMA,
direct I/O, 720 GiB quota, 2+1, `EFS_EXPIRY=0`), clients node9901/node9900,
release `445b8b549c82` (`v0.3.2-pre-alpha-681-g445b8b549`, branch devel).

## Copy throughput (idle drain)

`scripts/w125_pack_drain.sh REL OUT 17680 300000`: 300,000 files written
with packing held back, then a restart at minimum age 0; drain time
includes ~30 s of restart.

| Build | Drain | Rate | Pass (8 segments) |
| --- | --- | --- | --- |
| before (serial copy) | 294 s | ~1,020 files/s | read 1.1 s + segments 0.7 s + 1 s sleep |
| parallel reads only | 144 s | ~2,080 files/s | read 0.11 s + segments 0.75 s |
| `445b8b549` (all changes) | 92 s | ~5,000 files/s (idle-rate cap) | ~0.3 s |

## Gates on `445b8b549` (node9901)

| Gate | Result |
| --- | --- |
| `make test` | PASS except `docs/check-architecture.py` regen-diff (the generated architecture docs embed the status README, stale after the previous docs commit; regenerated in this commit) |
| crash injection, 5 points | **PASS** |
| two-client (packer age 0) | **67/67** |
| durability, remount after packing | **27/27**, 694 packed |
| GC reclamation (direct, POSIX) | **PASS** |
| expiry gate, suite + 5 runs | **6/6** |
| P6 landing L1–L7 (incl. backfill after upgrade) | **8/8** |
| P5 matrix (10 cases, 2,000 files) | **PASS**, 0 bad |

## Sustained-writer A/B

`scripts/w125_budget_ab.sh /tmp/w125-base2/src REL OUT 17680 2 1200000`:
16 workers write 1,200,000 files (`w125_p0_capture.py`, ≤ 512 B each) on
a fresh cluster per configuration, default minimum age 60 s; two
round-robin reps.

| Config | Writer mean files/s (rep 1 / 2) | vs base | Per-file fragments left at end of write (node 1) |
| --- | --- | --- | --- |
| B base `d9cee1ffd` | 2,060 / 2,025 | — | 1,200,000 (no packer) |
| F floor only (`EFS_PACK_LAG_TARGET_MS=86400000`) | 1,944 / 1,978 | −4.0% | 1,075,860 / 1,053,048 |
| A lag-controlled (defaults) | 1,304 / 1,290 | **−36.5%** | 154,170 / 88,919 |

Per minute (rep 1): B 2,318 → 1,827 files/s over the run; A follows B for
the first two minutes, then the lag passes 60 s and the packer ramps to
~900–2,200 files/s per group; the writer settles near 1,100 files/s. At
that equilibrium packing a file costs about as much cluster capacity as
creating one. F leaves the backlog to the idle catch-up: all 1.2M files
packed within the 1,800 s wait after the write. (The script's `drained`
time for A stops at its 1,800 s cap because each daemon's `packed_total`
counts only the groups it led; the fragment count shows A was fully packed.)

An earlier A/B on `27c20cd4b` + lag budget without the copy changes
(`/data1/efs/prof/w125-budget-ab.txt`) gave A −14%/−17% while still falling
behind (packing capped near 1,000 files/s); it motivated the copy changes.

## ior on orcd-mini (node9900, 8 ranks, `scripts/orcd_mini_ior_node9900.sh`)

| Build / state | Easy write / read MiB/s | Hard write / read MiB/s |
| --- | --- | --- |
| `445b8b549`, storage left by the user's workload, 2 runs | 709 / 3,392; 706 / 3,643 | 27.8 / 117; 23.8 / 106 |
| `445b8b549`, fresh, 4 runs | 1,565 / 4,620; 1,821 / 4,795; 1,913 / 3,761; 2,075 / 6,096 | fsync failure (1 run); 49.5 / 186; 47.7 / 171 |
| `27c20cd4b`, fresh | 1,384 / 4,023 | 40.8 / 162 |
| `de0c856b4` (W124), fresh | 1,593 / 4,625 | 41.3 / 160 |
| 2026-10-17 (`orcd-mini-gc-query-catalog` page) | 1,499 / 5,784 | 41.1 / 157 |

- On a fresh cluster the new build matches or exceeds the older ones. The
  two slow runs were on the storage left after the user's workload; the
  packer logged no work during them; the cause was not verified (likely
  reclaim of the deleted tiny files).
- The hard-test fsync failure (`report-stale … committed=0 replayed=2`
  rounds, then `flush-meta: stale generation (efs_rc=-14)` on the shared
  file) is open item **0a, STALE replay that never converges** (seen
  2026-10-01, before W125); no packer activity in that window. 1 of 6 hard
  runs on `445b8b549`; the two older builds ran hard once each.
- The ior bisect reformatted orcd-mini four times (allowed by the user);
  it now holds no user data.

## Decision needed: busy-time policy

Packing at the writer's pace costs the writer about a third of its
throughput; yielding keeps the writer within ~4% but lets unpacked data
grow (~70 KB per file per node here) until the cluster is idle, where it
now drains at ~5,000 files/s. Options:

1. **Floor while busy, idle catch-up** (`EFS_PACK_LAG_TARGET_MS` very high
   by default): −4%, unbounded backlog during sustained writes.
2. **Lag-controlled (current default)**: bounded backlog, −37% under a
   saturating writer.
3. **Space-pressure trigger** (not implemented): floor while busy until
   unpacked bytes or quota usage pass a threshold, then ramp — bounded
   space, foreground cost only when space is actually at risk.

orcd-mini runs option 2 (the defaults of `445b8b549`) until decided.

## Raw output (outside Git)

node9901 (sha256 prefixes): `/data1/efs/prof/w125-dv2/` `maketest.log`
`08cf99e7b1f5b9d9`, `crash.txt` `f16648907d48465c`, `suites.txt`
`d039b9a3af2f1336`, `landing.txt` `bdef4f0ef5726a0c`, `matrix.txt`
`d5471e9a407f6b4b`, `ab-run.txt` `d117fa006ba92085`, `ab/{B,F,A}-{1,2}/w.csv`
B-1 `bead11f772b9928e`, B-2 `1c39ae17a10425fa`, F-1 `05f0915a811e3d26`,
F-2 `5cf1ee44f6259b78`, A-1 `9cf35c6099d43b7c`, A-2 `08b553c81428e1c8`;
chain summary `/data1/efs/prof/w125-dv2-chain.txt` `08aeaaf9ae3278df`;
drains `/data1/efs/prof/w125-budget/drain1/packer.log` `0d87b66ab282ec5e`,
`drain3/packer.log` `15cf21792388cd7a`. node9900
`/data1/efs/orcd-mini-client/prof/`: `ior-445b8b549/easy.txt`
`f80fac9308d99b6d`, `ior-445b8b549-3/easy.txt` `547d574281aae0db`,
`ior-445b8b549-3/hard.txt` `33d47a80ab284ee8`, `ior-445b8b549-4/hard.txt`
`a045b82e5c095df0`, `ior-bisect-445b8b549c82/hard.txt` `a74b7779ad14a7f8`,
`ior-bisect-27c20cd4b018/easy.txt` `ee7dffbcc3dbec28`,
`ior-bisect-de0c856b4f4c/easy.txt` `3c4e454e8861dde6`.

## Reproduce

```sh
# node9901, REL/FAULT = 445b8b549 builds, BASE = d9cee1ffd build
cd REL && ASAN_OPTIONS=detect_leaks=0 make -i test
cd FAULT && scripts/w125_p7_crash.sh FAULT /tmp/w125-dv2-crash
cd REL && scripts/w125_p7_suites.sh REL OUT/suites
scripts/w125_p6_landing.sh REL BASE /tmp/w125-dv2-landing
scripts/w125_p5_matrix.sh REL /tmp/w125-dv2-p5
scripts/w125_pack_drain.sh REL OUT/drain 17680 300000
scripts/w125_budget_ab.sh BASE REL OUT/ab 17680 2 1200000
# node9900 against orcd-mini
scripts/orcd_mini_ior_node9900.sh
```

Cleanup: the scripts stop their fixtures; remove `/tmp/w125-dv2-*` and
`/tmp/w125-*-run`.
