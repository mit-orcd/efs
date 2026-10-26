# K41: packer placement on a member already known down (Oct 25)

**Verdict: PASS (scoped, loopback).** Branch `k41-packer-placement` on
devel `1fc526f66`, not merged, not pushed. On intel2 at `531c7c783`:
`make test-all` at 8 jobs, 499 commands, 0 FAIL, 14 NOTRUN host limits
(656 s); the new `tests/test_k41_packer_placement.py` passes (90 s in the
gate) and fails on the unpatched base; `tests/test_k33_growth.py`
10 of 10 runs in a row PASS (leader placement 2 spread, 8 colocated). Live, RDMA and orcd-mini runs are NOTRUN (the parent
session's gates).

- **Purpose:** alpha 1.0 single-loss durability: every acknowledged write
  survives any single-member loss once protection is restored. K41 in the
  [checklist](alpha-1.0-2plus1-checklist.md) (section K), W163 in the
  [work queue](work-queue.md). Found by the wave-9 merge gate: on an
  8-member loopback cluster (`test_k33_growth.py`, group leaders spread)
  the group-0 leader packed files 27 s after node 1 had stopped, placed
  fragment 0 on node 1, wrote the segment degraded (K37), and the loss of
  the second holder seven seconds later left one fragment: a fresh mount
  read the file `EIO`.
- **Base:** devel `1fc526f66` (wave 9 merged).
- **Tested source:** `531c7c783`; commits on the base:
  - `0ec653792` bounded parallel heartbeats with per-node-id marks; probed
    packer placement with live spares; `EIO` for a short packed read;
    `efs-mgmt pack-compact` reports `spares=` and `degraded=`; the new test
    and unit cases;
  - `43108ad33` `test_get_integrity` extracts the new read helper; budget
    row measured in the gate;
  - `531c7c783` integrity and decode verdicts of a packed read keep their
    code (gate run 2 finding, below).

  Copied with `git archive`, built by `scripts/make_test_summary.sh`
  (`EFS_BUILD_ID` unknown, no git metadata in the copy); sha256 `efsd`
  `5e4aa0fd…46dc`, `efs-fuse` `eb2a6e68…4a84`, `efs-mgmt` `0fb5ae4e…edfa8`,
  `tests/faults/efsd` `21e5c79f…1ee17`. Base binaries (`1fc526f66`,
  `make all tests/faults/efsd`): `efsd` `d35ec0d6…31c7`,
  `tests/faults/efsd` `3ca34174…48ec3`.
- **Environment:** intel2 (Intel Core Ultra 7 256V, 8 CPUs, 15 GiB, one
  NVMe, Rocky 10, kernel 6.12, Python 3.12), user `maik`, trees under
  `$HOME`, loopback TCP (no RDMA device), buffered I/O, private free ports
  (never 17xxx/27xxx), stores under the tree's `tmp/`, quota 1 GiB per
  member. The host was otherwise idle during gate run 3.

## Cause

Not the suspected streak reset. The heartbeat thread
(`src/server/cluster.c`) probed members **one after another** on
connections with the 30 s I/O timeout. A committed member that listens but
does not answer (a joiner still in its join loop: its listen socket exists
before it accepts; or a frozen daemon) held each probe for 30 s, so a
cycle took a minute and a stopped member never reached three missed
heartbeats on the members that had that joiner in their table. In the
failing run (intel2 `~/efs-artifacts/k41-20261025`) members 7 and 8 were
committed at 19:48:45-46 but answered only from 19:49:16; nodes 2 and 3
(both group leaders) never marked node 1 (stopped 19:48:42.9) down, the
late joiners 4, 5 and 6 did within 4-6 s, and the packer on node 2 placed
on node 1 at 19:49:09. The packer trusted the heartbeat mark alone and
wrote the third fragment's failure as a K37 degraded segment although five
live members could have taken it.

## Change

- **Heartbeat (`cluster.c`):** every probe of a cycle runs in its own
  thread under a network deadline of `EFS_HEARTBEAT_MS` (2 s), so a cycle
  lasts at most 4 s whatever a member does. The fail streak and down mark
  are kept per node id and written into every node-table row after each
  cycle and by every path that rebuilds or extends the table (join adopt,
  membership revision, HELLO, persisted load:
  `server_heartbeat_apply_locked`), so no rebuild resets them or imports
  another process's monotonic deadline. Three missed heartbeats mark a
  member down on every member within 12 s.
- **Packer placement (`packer.c`, `placement.c`):** a member counts as down
  for a pass when the heartbeat marks it, the peer pool's connect cooldown
  refuses it (`server_peer_is_down`), or, as a voter of the group this node
  leads, it acknowledged no AppendEntries for 2 s
  (`server_raft_host_unresponsive_voters`). Each destination answers a
  heartbeat on a new connection within 1 s before the segment is admitted
  (as K35's `spare_reprobe` probes a client PUT spare); one that fails is
  replaced by the next live ring member (`efs_pack_spare`: ascending ids
  after the failed one, wrapping, not in the stripe). A fragment refused at
  write time cancels the admission (its fragments are reclaimed as any
  cancelled segment's) and the files are placed again under a new segment
  id without the failed members, at most three attempts. The holders are
  recorded in the pack object as before; reads, reclaim and rebuild use
  that record (B1), never recomputed placement, so the contract is
  unchanged. The K37 degraded write (two fragments plus its mark) remains
  only when no live spare exists; with spares left after three attempts
  the segment is cancelled, not written degraded.
- **Client (`read.c`):** a packed read whose fragments are missing
  (`NET`/`NOT_FOUND`) while at least one holder answered returns `EIO` and
  logs `efs: packed read ino=… segment gG id=… gen=…: N of 3 fragments read
  (node a rc=…, …; need 2): EIO` (once per segment per 10 s). Integrity,
  decode and record verdicts keep their codes; all three unreachable stays
  `NET`.
- No new `EFS_FAULT_*`, KV kind, Raft tag or wire message; `struct
  efs_packer_status` gained `spares` and `degraded` (no compatibility: not
  in production). A6 rows for `EFS_FAULT_PACK_PLACE_DOWN` and
  `EFS_FAULT_SEGMENT_PUT_REFUSE` updated; `scripts/env_inventory.py
  --check` PASS.

## Reproduction

Prerequisites: a Linux host with gcc, fuse3 and `fusermount3`, Python 3;
no root.

```bash
git archive <commit> | ssh <host> 'mkdir -p ~/k41 && tar -x -C ~/k41'
cd ~/k41 && make -j8 all tests/faults/efsd && mkdir -p tmp
TMPDIR=$PWD/tmp python3 tests/test_k41_packer_placement.py
EFS_TEST_JOBS=8 scripts/make_test_summary.sh      # make test-all; summary.txt, timings.tsv
TMPDIR=$PWD/tmp EFS_K33_KEEP=1 python3 tests/test_k33_growth.py   # repeated 10 times
```

`test_k41_packer_placement.py` (test-long, budget 110 s, 1 slot): five
members, one mount, `EFS_PACK_MIN_AGE_MS=20000`, `EFS_FAULT_PACK_PLACE_DOWN=6`,
`EFS_FAULT_SEGMENT_PUT_REFUSE=<dir>`. (1) A batch of 60 files (300 B to
16 KiB) is written; node 4 stops; node 6 is added, then frozen with
`SIGSTOP` (it holds no data). Nodes 1, 2, 3, 5 must each log `heartbeat:
node 4 … marked down` and `node 6 … marked down` within 30 s; once the batch
is packed every new segment has three fragments on running members (none
on 4 or 6), node 6 failed the placement probe and was replaced, nothing was
written degraded, `Degraded segments: 0`. (2) Node 6 resumes refusing
segment PUTs, node 4 returns, a second batch is packed: admissions whose
fragment node 6 refused are cancelled and placed again (`spares=` grows),
every segment has three fragments, none on node 6, nothing degraded. (3)
A holder of a step-1 segment stops; a fresh mount reads all 120 files
exactly. Cleanup: the test removes its `tmp/efs-k41-*` tree
(`EFS_K41_KEEP=1` keeps it). `tests/test_placement.c` adds `k41_pack_spare`
(skip mask, wrap, order independence, three-member edge).

## Results

| Run | Source | Result |
|---|---|---|
| K41 test, alone | base `1fc526f66` | **FAIL**: `node 1 never marked node 4 down within 30 s` |
| K41 test without that assertion | base `1fc526f66` | no member marked 4 or 6 within 30 s; the packer wrote 3 segments `written degraded: fragment … for node 6` while live spares existed |
| K41 test, alone | fix | PASS, 72 s (step 1 marks both down 12-14 s after the stop; 5-6 failed probes replaced; 9-16 refused admissions placed again) |
| `make test-all`, 8 jobs, run 1 | `0ec653792` | 499 commands, 3 FAIL: `test_get_integrity` (did not extract the new helper: this change), `test_decoder_refusal` (pre-existing: base 2 of 8 FAIL alone, fix 8 of 8 PASS), `test_k38_shared_chunk_pressure` (load: 4 of 4 PASS alone on base and fix); summary not retained (tree removed before copying) |
| `make test-all`, 8 jobs, run 2 | `43108ad33` | 499 commands, 1 FAIL: `test_get_integrity` packed integrity case returned `EIO` for `CHECKSUM` (fixed in `531c7c783`) |
| `make test-all`, 8 jobs, run 3 | `531c7c783` | **499 commands, 0 FAIL, 14 NOTRUN** (RDMA x9, valgrind x2, sanitizer runtime, cluster scripts, `EFS_FAULTS` follower bound), 656 s, no OVERBUDGET; K41 test 90.0 s |
| `test_k33_growth.py` x16 | base `1fc526f66` | 16 PASS (8 spread placements): the failing window also needs node 1 down when the packer fires, which this host did not hit alone |
| `test_k33_growth.py` x10 in a row | fix `531c7c783` | **10 PASS** (runs 2 and 6 spread: group 0 on node 2, group 2 on node 3; 8 colocated on node 3) |

The growth test's EIO itself did not recur on the unpatched base in 16
runs here (the parent saw 1 of 3 on intel2 with other work running); the
mechanism is reproduced deterministically by the new test, which fails on
the base and passes on the fix.

Raw outputs (not in Git): intel2 `~/efs-artifacts/k41-fix-20261025/`
(`baseline-devel-1fc526f66/`: base test outputs, growth tally, marks and
degraded lines of the variant run, base binary hashes;
`gate-run2-43108ad33/`, `gate-run3-531c7c783/`: `summary.txt`,
`timings.tsv`, `build.log`, binary hashes; `growth10/`: per-run output and
tally), `SHA256SUMS` in that directory (sha256 `58baba8e…e8ff`). Work trees
removed with `edelete`. The failing wave-9 run: intel2 `~/efs-artifacts/k41-20261025`.

## Limitations and remaining gates

- Loopback only; live multi-host, RDMA and orcd-mini runs NOTRUN.
- A frozen member still costs `efs-mgmt status`/`pack-compact` 30 s per
  query (unchanged; the test reads packer counters from the logs while a
  member is frozen).
- The heartbeat now declares a member that does not answer within 2 s for
  three cycles down (it used to wait 30 s per probe); placement and rebuild
  skip it, client I/O is unaffected. A member that is that slow under
  sanitizer builds would be skipped by the packer, not lost.
- The packer probe adds one connection per remote destination per segment.
