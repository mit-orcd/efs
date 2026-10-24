# B1–B5 · geometry, placement and usable capacity (Oct 24, 2026)

**Purpose.** Section B of
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md) (Stage 2,
"Verify geometry, placement and usable capacity"), items B1 to B5: turn
each item into tests (deterministic unit tests where the logic allows,
loopback daemon tests otherwise), fix every defect they find, and give the
parent session one driver that runs the B checks on any running cluster.

**Verdict: PASS (scoped, loopback regression evidence).** Branch
`claude/devel-b-geometry-fbcf5b` on `merge-wave3` (`a2326a369`), not
merged, not pushed. `make test-all` on xorinox at `17aac881c`: 453
commands, **0 FAIL**, 13 NOTRUN host limits (runner status INCOMPLETE
only because of those NOTRUNs). Two defects found and **FIXED** (a full
stripe member was acknowledged as a degraded stripe; the reader could
decode from recomputed placement). The real-host run of
`scripts/b_checks.py` is **NOTRUN** (parent session; A4 needs distinct
hosts). Single-host loopback daemons are regression evidence, not
qualification.

## Item map

| Item | Deterministic (`make test`) | Real daemons (`make test-long`) and driver | Result |
|---|---|---|---|
| B1 D1/D2/P on three distinct members, from records | `tests/test_b_geometry.c` `b1_stripes` (114,688 stripes, 3..16 members, sparse ids; segments skip down members; role bound in the record); `tests/test_read_recorded_nodes.py` (reader takes nodes only from the row / pack object) | `tests/test_b_geometry.py` runs `scripts/b_checks.py --store …` at 3..8 members: every chunk row, span and pack object of the test files names three distinct current members, and each store's fragment records (kind, object, chunk, generation, role) sit on the row's node and on no other (510 to 888 records checked per run) | PASS; **FIXED** reader gate |
| B2 partial/empty/boundary, pairs, corruption | `b2_payloads`: every two-fragment pair exact for 0 B to 1 chunk at 128 KiB, 256 KiB and 1 MiB chunks, last chunks of 7 file sizes, 8 segment bodies; corrupt payload/record and 5 identity mismatches refused | driver: 19 boundary sizes exact through every mount, chunk-row shape; daemon test: on the stores D1/D2 are the halves, P = D1 ^ D2, every pair rebuilds the chunk, the last chunk is 17 B + zero padding; through fresh mounts a corrupt D1 and a D1 replaced by another chunk's fragment both read exactly (from D2 + P), two corrupt fragments are EIO, restored reads exactly | PASS |
| B3 3 / 4 / 5–8 members, growth, histograms | `b3_spread`: three use all three every chunk; four omit each member exactly once per four consecutive chunks; 3..8 grown one at a time (dense and gapped ids) give every member every role within 10 % of the mean; 24 files after an addition reach the new member | driver at each size after each addition 4..8: histogram node × D1/D2/P (below); four: the two chunks of a file omit different members; five or more: every member holds every role | PASS |
| B4 packing audit | `b4_packing`: segment destinations over the ring at 3..8, every role within 10 %; voters' share 3/N; ring vs voters-only usable capacity | driver: small files switched into segments, pack objects' nodes recorded, group voters from the leader; segment fragments on every member at every size; the daemon test asserts group 2 votes on {2,3,4} from four members and a voter share below 1 | PASS |
| B5 logical vs physical, headroom, ENOSPC | `b5_capacity`: 3-of-N bound vs brute force (2,000 random cases), +2Q/3 per addition, one full member, 1.5x for whole chunks only, startup headroom budget; `tests/test_put_retry_budget.py` and `tests/test_put_reply_deadline.py` (full member named per slot, placed around or ENOSPC, never degraded) | driver: status on every member = 3-of-N bound over the members' capacities, df on every mount = status, cost from the fragment records; daemon test: full member at three → ENOSPC, 0 free in status and df, no degraded chunks; addition → capacity up, writes placed off the full member; automatic quota = headroom budget | PASS; **FIXED** full member |

## Defects found and fixed

1. **B5: a full stripe member made silent protection debt.** With one
   member refusing a fragment as full (`EFS_ERR_QUOTA`), the PUT counted two
   acknowledgements and the write was acknowledged as a degraded
   two-fragment stripe (W154 "degraded for rebuild on return"). No rebuild
   can pay that debt while the member stays full, and at three members every
   later write was unprotected while `df` reported 0 free. This was the open
   "one-QUOTA-member PUT" question of W42 / K20. Negative control on the
   untouched base `a2326a369` (`EFS_B_PHASES=full`): **200 MiB acknowledged
   with a 16 MiB member full, never ENOSPC**, 13 degraded acknowledgements
   logged in the excerpt kept. Fix (`src/client/write.c`):
   `put_fragments_parallel_once` names each member that refused as full
   (also a refusal that arrives after the quorum, in the drain);
   `put_fragments_parallel_run` returns QUOTA instead of a degraded OK when
   the missing fragment is a full member's; `efs_client_put_fragments_parallel`
   gives the object up, marks the member full for 10 s and places a new
   object with that slot on a spare that is neither down nor full
   (`reroute_untried_fragments` skips full members); with no such spare the
   write fails ENOSPC. After the fix: three members with node 3 at 16 MiB
   take 32 × 1 MiB files, then ENOSPC; node 4 joins and 8 more files are
   written on three members with none on node 3 (8 re-placements logged), no
   degraded acknowledgement. Documented in
   [failure-tolerance](../how-it-works/failure-tolerance.md#degraded-operation).
2. **B1: the reader could decode from placement recomputed now.**
   `efs_client_decode_placed_chunk_attempts` started from
   `efs_place_fragments()` under the current membership and only replaced
   it by the row's nodes when they were non-zero; a row without nodes (or no
   row) was fetched from recomputed placement, which after an addition names
   other members than those holding the fragments. Now a missing row or a
   row without a valid D1/D2/P stripe (`efs_fragment_nodes_valid`: three
   distinct ids in 1..16) is refused (`EFS_ERR_PROTO`, EIO) before any GET;
   `read.c` has no placement call left. No such row was seen in the runs
   (publication already refuses zero or duplicate nodes, `evidence_ok`), so
   this closes a path rather than a reproduced loss.

Also: `efs-mgmt status` prints exact per-member bytes
(`bytes: used= quota= capacity= available= valid=`) under each member
line; the rounded line stays. No new KV kind, Raft tag or wire message
type; no format change.

## Results (xorinox, `17aac881c`, `tests/test_b_geometry.py`, 310 s)

Driver checks per run (B1 / B2 / B3 / B4 / B5), fragment records checked
against rows, the B3 histogram over the spread files (node: D1/D2/P, two
chunks per file), packed segments with fragments per node, group-2 voters
and the voters' share of segment fragments, status capacity (1 GiB quota
per member; node 8 automatic), and the B5 costs (payload ratio / allocated
ratio for whole-chunk files, payload ratio of packed small files):

| N | checks | records | B3 histogram | segments: fragments per node | g2 voters | voters' share | capacity (B) | whole chunks | packed |
|---:|---|---:|---|---|---|---:|---:|---|---:|
| 3 | 665/1570/99/124/44 | 510 | 1:25/33/38 2:38/25/33 3:33/38/25 | 21: 21 21 21 | 1,2,3 | 1.00 | 2147483648 | 1.5 / 1.594 | 1.73 |
| 4 | 716/1310/56/166/48 | 561 | 1:24/21/24 2:27/24/21 3:24/27/24 4:21/24/27 | 37: 24 28 29 30 | 2,3,4 | 0.73 | 2863311530 | 1.5 / 1.594 | 1.81 |
| 5 | 797/1610/20/207/54 | 618 | 1:21/23/28 2:23/21/23 3:25/23/21 4:28/25/23 5:23/28/25 | 33: 21 21 19 19 19 | 2,3,4 | 0.60 | 3579139412 | 1.5 / 1.594 | 1.68 |
| 6 | 908/1910/24/248/60 | 705 | 1:28/22/22 2:33/21/18 3:22/28/22 4:18/33/21 5:22/22/28 6:21/18/33 | 38: 10 28 10 28 10 28 | 2,3,4 | 0.58 | 4294967296 | 1.5 / 1.594 | 1.68 |
| 7 | 1022/2210/28/289/66 | 795 | 1:25/26/20 2:26/24/24 3:23/25/26 4:20/26/24 5:24/23/25 6:26/20/26 7:24/24/23 | 43: 23 15 21 13 23 17 17 | 2,3,4 | 0.38 | 5010795178 | 1.5 / 1.594 | 1.67 |
| 8 | 1139/2510/32/330/72 | 888 | 1:25/27/23 2:23/25/21 3:21/25/27 4:27/23/25 5:23/21/25 6:21/27/23 7:27/23/21 8:25/21/27 | 50: 16 23 21 22 18 19 11 20 | 2,3,4 | 0.44 | 7516192768 | 1.5 / 1.594 | 1.68 |

- Capacity: 2 GiB at three, +715,827,882 B (2/3 GiB, floor) per 1 GiB
  member; node 8's automatic quota (593,352,617,984 B on a 914 GB
  filesystem, ceiling 640,002,377,728 B) makes the bound 7,516,192,768 B:
  the 3-of-N bound with one large member, not 2/3 of the sum. Status on
  every member and df on every mount agreed at every size.
- Costs (B5), from the fragment records: a whole chunk costs exactly 1.5x
  in payload (what quotas charge) and 1.594x allocated (each 64 KiB
  fragment file is 68 KiB with its 4 KiB direct-I/O record page). A
  partial chunk is a span object with a whole 2+1 stripe: 1 B over a chunk
  boundary costs 192 KiB payload (131,073 B → 3.0x; 16,385 B → 12x;
  1 MiB + 3 → 1.69x). Packed small files: 1.64x for 4 KiB and 16 KiB files,
  about 1.7x over the mixed 1 B..16 KiB batch, payload shared by length;
  files below about 1 KiB cost about 24x even packed (8 KiB segment
  geometry shared by few files). Before packing every small file costs a
  whole stripe. The 1.5x figure holds only for whole chunks; status
  `used` counts fragment payload, not record pages or filesystem blocks.
- Full member (separate cluster, node 3 `--quota 16M`): capacity 32 MiB;
  32 files of 1 MiB, then ENOSPC twice; status free 0, `Degraded chunks: 0`,
  df free 0 and total = status capacity. Node 4 (1 GiB) joins: capacity
  2,158,668,458 B, free 2,098,724,864 B; 8 more files, every chunk on three
  members other than node 3; all 40 files read back exactly.
- Six members: ordinary and packed stripes are {1,3,5} or {2,4,6}
  (stride N/3 = 2), so the 38 segments split 10 : 28 between the two sets.
  Expected behaviour of the stride placement (N/3 disjoint stripe sets when
  3 divides N); balance holds over many stripes (unit test, ±10 %).

## Negative controls

- Base `a2326a369`, full-member phase: FAIL, `200 MiB written on a 16 MiB
  member without ENOSPC` (raw `negative/base-a2326a369-full-phase.log`).
- `tests/test_read_recorded_nodes.py` on the base `read.c`: the placement
  call is present (would FAIL).
- `tests/test_b_checks_driver.py`: negative controls for duplicate or
  non-member nodes, missing/extra chunk rows, wrong packed length, stub
  without span, span on a duplicate node, missing fragment, stray copy,
  wrong generation, missing span fragment, retired voter view and junk files.
- Development run 1 (WIP commit): the driver FAILed on single-chunk files
  whose base row is a stub with the data in a span; the driver then learned
  spans (`row_objects`). Gate run at `9bc1d4512`: 453 commands, 1 FAIL
  (`tests/test_k33_payload.py`, see below); gate at `941e7e601`: 1 FAIL, the
  new voter assertion raced the group-2 joint change (fixed by waiting for
  settled voters, `17aac881c`). Those two gate trees were deleted before
  their summaries were copied; the counts are from the session record.

## Code gate

Host xorinox (16 CPUs, Rocky Linux, gcc, Python 3.12), private trees under
`~/b-geometry-gate/` and `~/b-geometry-base/` (removed after the runs),
`TMPDIR` under `$HOME`, loopback ports chosen by the tests (never
17xxx/27xxx; no running cluster, mount or the `efs` user touched).
Daemon topology: up to eight efsd on 127.0.0.1, TCP, default efsd I/O
settings (fragment files carry a 4 KiB record page), 2+1, one store each, packer
`EFS_PACK_MIN_AGE_MS=500`, GC on. Builds from `git archive` (build id
given by `EFS_GIT_ID`).

```sh
H=17aac881c764fa213d2fca42bd909d2e50daa36a
git archive $H | ssh xorinox 'mkdir -p ~/b-geometry-gate/src ~/b-geometry-gate/tmp && tar x -C ~/b-geometry-gate/src'
# on xorinox, in screen:
cd ~/b-geometry-gate/src && TMPDIR=~/b-geometry-gate/tmp EFS_GIT_ID=$H \
  EFS_GIT_BRANCH=claude/devel-b-geometry-fbcf5b EFS_GIT_DESCRIBE=$H \
  EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
# the B daemon test alone, keeping the driver results:
TMPDIR=<dir> EFS_B_ARTIFACTS=<dir> python3 tests/test_b_geometry.py
# negative control: the full-member phase on a base tree with the new test files
EFS_B_PHASES=full python3 tests/test_b_geometry.py
```

Result: `TOTAL 453 FAIL 0 NOTRUN 13 STATUS INCOMPLETE (475s) BUILD 19s`
(`summary.txt` sha256 `d5b42d28…9009`). NOTRUN: RDMA ×9
(`test_declaration_gate` RDMA case, `test_rdma_xprt`,
`test_rdma_segmented` ×7: no IB device), `test_cluster_stop.py` (no
`~/git/cluster`), `test_follower_bound` stalled compactor (needs
`EFS_FAULTS=1`), `test_leak_gate.py` (no valgrind), runner self-test line.
New tests: `test_b_geometry.py` 414 s in the gate, `./tests/test_b_geometry`
0.1 s, `test_b_checks_driver.py`, `test_read_recorded_nodes.py` under 0.1 s,
`test_put_retry_budget.py` 1.2 s.

**Pre-existing flake found (not fixed here, B6 scope):**
`tests/test_k33_payload.py` FAILs on xorinox with `EIO` while one member is
down (`verify(a, 'node k down')`): base 2 of 3 runs, this branch 1 of 3
(and once in the first gate), passing in the final gate. Every failure is
the same: `efs-fuse read: network error (efs_rc=-6) ino=4097 off=0 len=2`,
a packed small file of the first batch read with node 3 down; the packed
read does not fall back to the two remaining segment fragments. Logs:
`k33-payload-flake/`. Follow-up for the parent (B6: unavailable fragment
under a bounded test).

## The driver for real hosts

```sh
scripts/b_checks.py --mount /mnt/efs --seed HOST:PORT \
  [--mount /mnt/efs-via-other-member ...] [--store ID=/path/to/store ...] \
  [--expect-members N] [--pack-wait 300] --out b-checks.json
```

Read-only apart from its test directory under the first mount (removed at
the end unless `--keep`); it uses `efs-mgmt status`, `raft-status`,
`raft-getchunks` and `version`, never changes membership, quotas or
configuration. `--store` (a member store readable on the host, e.g. the
local one) adds the persisted-inventory part of B1 and the record-based B5
cost; without it those parts are NOTRUN in the result. Real clusters pack
after `EFS_PACK_MIN_AGE_MS` (60 s default): keep `--pack-wait` above it.
`--strict-used` additionally requires the status `used` delta of the large
files to be 1.5x (quiet cluster only). Full member, ENOSPC and capacity
after an addition change the cluster and stay NOTRUN in the driver; they
are covered by the daemon test. Result JSON: `verdict`, per item `verdict`,
`checks`, `failures`, `notrun`, histograms, voters, usable capacity, df,
costs, versions of every member. Exit 0 PASS, 1 FAIL, 2 cluster not usable
(member down, wrong member count).

## Limitations and remaining gates

- **NOTRUN:** the driver on real hosts (A4 distinct hosts; parent),
  RDMA, orcd-mini, the named 19810 capacity acceptance (W42).
- Loopback only: one machine, one filesystem for every store; capacity
  values come from quotas, not distinct disks.
- `df` keeps a member's last STATUS values when it cannot reach it, while
  status reports `degraded` without a number; with every member up they
  agree (checked). Not changed here.
- The full mark is per client, 10 s; another client still tries the full
  member once before it marks it.
- Raw evidence: xorinox `~/efs-artifacts/b-geometry-20261024/`
  (`SHA256SUMS` sha256 `a12d99ae…9b57`, 83 files, 4.3 MB: gate summary
  and logs, driver JSON per member count, daemon logs of the flake, the
  negative-control log). Runtime stores and work trees were deleted.
