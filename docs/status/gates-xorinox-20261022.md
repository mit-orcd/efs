# Live gates on xorinox: K28 five-node, K22 follower bound, K9 append oracle (Oct 22–23, 2026)

Alpha 1.0 Stage 1 rows [K28](alpha-1.0-2plus1-checklist.md) (W151),
[K22](alpha-1.0-2plus1-checklist.md) (W23) and
[K9](alpha-1.0-2plus1-checklist.md) (W87), run on the host xorinox from
branch `devel-gates-xor`. Not merged.

| Gate | Verdict |
| --- | --- |
| K28 five-node live gate | PASS, with finding F1 (no re-protection of a stripe acknowledged with two fragments; deferred repair scope) |
| K22 plateau and stalled follower | PASS (scoped): plateau at fault-build caps, SIGSTOP stall and catch-up. NOTRUN: production-scale plateau, snapshot import, live leader admission at 3/4 |
| K9 / W87 append oracle under load | NOT REPRODUCED: 110 rounds, 84,480 records, all exact; W87 stays OPEN |

## Common

- **Tested source:** devel `163241584` (`163241584306`). No product code
  change. The gate harness added on this branch
  (`tests/live/gate_k28_five_node.sh`, `tests/live/gate_k9_append_load.sh`,
  `tests/measure/k22_stalled_follower.sh`) only starts, signals and samples
  private daemons. It does not touch the daemon or client source.
- **Builds:** `git archive` of the branch onto the host, gcc 14, x86_64,
  `-O3 -march=native`.
  - Normal build: `make -j16 EFS_GIT_ID=163241584306`. Build id
    `163241584306`. sha256 efsd `960b9c19…acc30a`, efs-fuse
    `e7063dce…31cf5f`, efs-mgmt `e3e373ff…29c8`.
  - Fault build, used for the K22 plateau only:
    `make -j8 EFS_FAULTS=1 EFS_GIT_ID=163241584306-faults`. sha256 efsd
    `95e6f380…4f0`.
- **Host:** xorinox, Rocky Linux 10, 16 CPU, 60 GB RAM. Stores were on
  `/home` (XFS on LVM). All daemons ran on 127.0.0.1 with
  `EFS_TRANSPORT=tcp` and buffered fragment I/O (`--no-direct-io`). The
  host also ran an unrelated user FUSE client, which was not touched. K9
  and K22 ran at the same time, so load average was 25–30 and absolute
  rates are lower than on an idle host.
- **Raw outputs:** run logs, `samples.tsv` and round JSON were kept under
  `~/efs-gates-xor/` on xorinox until the end of the session. They were
  then deleted with the rest of that directory, by the host rules. This
  summary is the retained record.

## K28 / W151: five-node cluster

**Purpose.** This is the live gate that K28 left NOTRUN
([evidence](k28-w151-cluster-cap-20261022.md)). A cluster of more than four
nodes must work for membership, data placement and clients. Metadata keeps
two three-voter groups on nodes 1–4. Packed segments go on the data ring
across all nodes, and a node that hosts no group reads the pack object
remotely for PUT_SEGMENT admission and segment reclaim.

**Verdict: PASS, with finding F1 (outside K28: no re-protection of degraded
writes, automatic repair is deferred).**

**Setup.** Five efsd on ports 19001–19005 with `EFS_MD_RAFT_N=5`, quota 8G
each, stores `~/efs-gates-xor/k28/n1..n5`. Node 1 was the seed and the
others ran with `--join`. Then `efs-mgmt raft-mkfs` and one efs-fuse mount.
`EFS_PACK_MIN_AGE_MS=2000` (a documented packer tunable, default 60 s)
let the packer take files within the run.

```bash
tests/live/gate_k28_five_node.sh start  REL /abs/k28   # 5 daemons, mkfs, mount
tests/live/gate_k28_five_node.sh status REL /abs/k28   # status, raft-status, packer
tests/live/gate_k28_five_node.sh counts REL /abs/k28   # fragment / segment files per store
tests/live/gate_k28_five_node.sh node-stop  REL /abs/k28 5
tests/live/gate_k28_five_node.sh node-start REL /abs/k28 5
tests/live/gate_k28_five_node.sh stop   REL /abs/k28
```

`DIR` must be absolute: `client.sh stop` matches the mount by its absolute
path. The first run passed a relative path, so the final unmount said "not
mounted", and the mount was removed by hand with `fusermount3 -u`. The
workload was `tests/live/gate_k28_workload.py`. It writes files from a
seeded PRNG, records sha256 per file, and reads every file back for
verification. It does `large` (N files × M MiB, 1 MiB writes) and `small`
(N files of 1 B to 16 KiB, in one directory per writer). The phases ran
these commands, with REL as the build tree, D the store dir, M the mount
and W the script:

```bash
python3 W large M D/man-large.txt 10 128                      # 10 × 128 MiB
for w in a b c d; do python3 W small M D/man-small-$w.txt 1500 $w & done; wait
rm -rf M/small/a M/small/b; # then unlink 3 of every 4 files in M/small/c
efs-mgmt pack-compact 127.0.0.1:19001 256                      # queue compaction
python3 W verify M D/man-all.txt                              # concatenated manifests
gate_k28_five_node.sh node-stop REL D 5                        # then 3 × 64 MiB + 1,000 small, verify
gate_k28_five_node.sh node-start REL D 5; gate_k28_five_node.sh node-stop REL D 1
python3 W large M D/man-n1down.txt 3 64 _n1; python3 W small M D/man-n1down.txt 1000 f; verify all
gate_k28_five_node.sh node-start REL D 1; python3 W verify M D/man-all.txt
```

During the node-5 phase, the 64 MiB files reused the names
`large/f000..f002` (harness slip; the script now takes a directory
suffix). Their 128 MiB manifest lines were replaced with the new ones
before verification.

**Results.**

| Check | Result |
| --- | --- |
| Membership | `efs-mgmt status`: 5 nodes up, `Cluster state: OK`, 5/5 identical build |
| Voter layout | `raft-status`: group 0 `voters=0x7` (nodes 1–3, leader node 1), group 2 `voters=0xe` (nodes 2–4, leader node 2), node 5 `hosted=0` for both groups |
| Large files | 10 × 128 MiB in 9 s. Data fragments (64 KiB + header) per store were n1 6145, n2 6142, n3 6142, n4 6145, n5 6146, after the packer removed the small-file fragments. That is 30,720 = 1280 MiB × 1.5 / 64 KiB, split evenly across all five |
| Small files, packer | 4 writers × 1,500 files in 50 s. Group 0's packer built 399 segments, `files_packed=6000`, `put_errors=0`, `admit_errors=0`, `read_errors=0`. Segment fragment files per store: n1 135, n2 137, n3 129, n4 135, n5 133 |
| PUT_SEGMENT on node 5 | Node 5 hosts no group, so its 133 accepted segment fragments passed admission only through the remote pack-object read (`server_raft_host_pack_object` → `host_pack_object_remote`). No `refused` lines in its log |
| Segment reclaim on node 5 | The test deleted 4,125 small files and queued compaction (`pack-compact 256`); `compacted_total` went to 613. On node 5, 189 retired segment fragments were deleted and 19 new compacted ones were admitted. Node 5 `gc-status`: `reclaim_state=ok`, `reclaim_busy=0`, `delete_errors=0`. A segment is deleted only after `segment_acceptable` proves it not ACTIVE through that same remote read |
| Readback | 1,885 surviving files, 0 mismatches |
| Node 5 stopped | `DEGRADED (4 up, 1 down)`. 3 × 64 MiB + 1,000 small files were written in 21 s, and all 2,885 files read back correctly |
| Node 5 restarted | It rejoined ("Joined cluster with 5 nodes") and the cluster was OK within 10 s |
| Node 1 stopped (seed, group 0 leader) | Node 2 became group 0 leader (term 2) and led both groups. The client kept serving through nodes 2–5. 3 × 64 MiB + 1,000 small files were written in 14 s and read back correctly. Node 2's packer packed those 1,000 files, and node 5 admitted 50 more segment fragments through a remote read served by the new leader. The older files: 2,884 of 2,885 read correctly, and one returned **EIO** (F1) |
| Node 1 restarted | Rejoined as group 0 follower at `applied=commit=65558`, cluster OK. **All 3,888 files read back correctly, including the F1 file.** Its 308 stale tail fragments from the truncated rewrite were collected within 30 s |
| Daemon logs | No `fatal`, `panic`, `abort` or `assert` lines on any node |

**F1 (finding, no fix): a stripe written in the window before the client
marks a node down stays at two fragments after that node returns.** The
test rewrote `large/f000` (64 MiB, 512 chunks) right after it stopped node
5. Of its 512 chunks, 508 were placed on nodes 1–4 with three fragments
each. Four chunks (1, 2, 3, 6) had been written before the client marked
node 5 down. Each was acknowledged with two fragments; the third was meant
for node 5. Node 5 rejoined about 70 s later and nothing rebuilt those
stripes. When node 1 was stopped, chunks 2 and 3 had lost two of three
fragments. The client logged `fetch published ino=5275 ci=3 rc=-9` and
`decode error (efs_rc=-9)`, and read(2) returned EIO. The data came back
when node 1 returned. The cluster was never more than one node down at a
time, yet the file was unreadable for one single-node outage. The write
path gave no warning when it acknowledged a two-fragment stripe. This is
the deferred automatic repair/rebuild scope in the checklist's Stage 1
introduction. It is not a K28 placement defect, but the window and the
missing re-protection should be tracked against that deferral.

**Observation.** In the first phase, only group 0's packer found
candidates (all 6,000 small files hashed to group 0's shard). So group 2's
segment path was exercised only when node 2 led both groups.

**Limitations.** One host on loopback TCP, so five daemons are not five
machines. No RDMA, no direct I/O, no POSIX or load suite on the five-node
mount. Nodes 2–4 (metadata voters other than node 1) were not stopped.
Fragment counts are from the store tree, not from a placement audit of
every chunk.

## K22 / W23: follower memory and lag bound

**Purpose.** These are the live items that K22 left NOTRUN
([summary](../../results/measure/20261022-k22-follower-bound/SUMMARY.md)):
the plateau on a real cluster, and a stalled follower that catches up,
ideally through snapshot import.

**Verdict: PASS (scoped) for the bound plateau at fault-build caps and for
SIGSTOP stall and catch-up. NOTRUN: the production-scale plateau, snapshot
import into a stalled follower, and live leader admission at 3/4.**

### Plateau (fault build, compactor parked on node 3)

The committed driver ran as-is, on ports 19011–19013
(`PORT=19010`), stores under `~/efs-gates-xor/k22/plateau2`:

```bash
PORT=19010 L0=33554432 MEMTABLE=2097152 WORKERS=8 BENCH_FILES=5000 WARM=60 STALL=3000 \
  PLATEAU=300 RELEASE=120 DRAIN=600 bash tests/measure/k22_follower_bound.sh REL-faults /abs/k22/plateau2
```

The load was 8 loops of `efs-bench --meta` (16 workers each, rounds of
5,000 mkdir+create) for 285,000 files in total. Node 3 had
`EFS_FAULT_COMPACT_STALL=1`, L0 cap 32 MiB and memtable 2 MiB. Its derived
bound was `kv_mem_budget=8388608` and `max_unapplied=32768`, 8× the efs3
run. Nodes 1 and 2 used production values: 1 GiB L0 cap, 128 MiB budget,
524,288 entries.

| Item | Result |
| --- | --- |
| Stall to bound | The compactor parked at 04:08:37. The L0 cap was reached and apply deferral started at 04:20:18. The leader first saw node 3 busy at 04:23:33 (`AE busy: unapplied=32767 max=32768`) |
| Plateau, 302 s | Node 3 RSS 99,912–100,316 KiB, unapplied 32,101–32,767 of 32,768, memtable 8,388,770–8,389,246 B (budget 8,388,608), L0 33,936,039 (cap 33,554,432 plus one flush) |
| Peaks, whole run | Node 3 RSS 116,856 KiB, unapplied 32,768, `busy_sent=17977`, `apply_deferred=319696`. Leaders' RSS 94,408 / 66,812 KiB, leader unapplied ≤ 158 |
| Quorum progress | Leader commit rate in group 0 / group 2: warm 141/138/s, stall before the bound 170/153/s, plateau 189/172/s, release 228/223/s. The bound did not slow the healthy quorum. `admit_busy=0` on every node |
| Leadership | Node 3 logged `election held: low capacity` once per group and stayed a follower. No leadership change |
| Release | Unapplied went from 32,747 to under 1,000 within 5 s under load. At drain start, node 3 had applied the leaders' commits on both groups (246,107 / 225,495) |
| Client errors | 31 bench rounds failed with `EBUSY`, all with `inode-rpc: dual type=45 exhausted 16 BUSY/STALE retries`: 22 during the stall, the first at t=333 s, before the bound engaged at t≈952 s; 1 in release and 8 in drain; none in the 60 s warm. This matches the efs3 observation. Admission refused nothing |

**Production caps (partial, stopped).** The first run used production caps
on node 3 (L0 1 GiB, budget 128 MiB, 524,288 entries) with the compactor
parked. In 574 s its L0 grew from 0 to 35.3 MB, about 61 KB/s, at about
170 entries/s per group, with K9 sharing the host. Reaching the bound
needs about 4.8 h for L0, then about 128 MiB of memtable, then about
51 min for 524,288 entries. That is roughly 6 h, which does not fit this
session, so the run was stopped. Its partial figures (RSS 79.5 MB, node 3
unapplied ≤ 63) show only that nothing grew without bound before the cap.
**The production-scale plateau stays NOTRUN.**

### Stalled follower (normal build, SIGSTOP)

`tests/measure/k22_stalled_follower.sh` (new on this branch) ran on ports
19014–19016 with stores under `~/efs-gates-xor/k22/stall2`:

```bash
PORT=19013 STOP=600 HOLD=30 WORKERS=8 bash tests/measure/k22_stalled_follower.sh REL /abs/k22/stall2
```

The load was the same bench loops (195,000 files). Node 3 was stopped with
`SIGSTOP` (its recorded PID) 60 s into the load. It was resumed after 630 s
with `SIGCONT`.

| Item | Result |
| --- | --- |
| While stopped (630 s) | Group 0 / group 2 leader commits went 8,630 → 108,904 and 8,260 → 103,050, at 152–160/s against 137–143/s in warm. Leader unapplied ≤ 142, `admit_busy=0`, leader RSS 87.8 MB |
| Leader bound | **Not engaged.** A stopped follower sends no busy reply, and leader admission counts the leader's own `last − applied`, which the healthy quorum kept below 170. This matches D30: one slow follower does not throttle the quorum |
| Snapshot | None taken. `EFS_RAFT_SNAP_BYTES` is 512 MiB with a 512 MiB retained window. The raft log grew about 35 KB/s here (about 125 B/entry across both groups), so the stopped follower was always inside the window |
| Catch-up | After SIGCONT, node 3 caught up through AppendEntries at about 440 entries/s per group. Its unapplied stayed ≤ 114 and it sent no busy replies. It was within 2,000 entries of both leaders' commit after 384 s under load, and applied every commit 76 s after the load stopped. RSS went from 29.7 to 59.5 MB. No election attempts; only the usual `prev-beyond-log` AE rejects |
| Client errors | 25 bench rounds failed with the same 16-retry `EBUSY`: 15 while node 3 was stopped (the first 117 s in), 7 after resume and 3 in drain; none in the 60 s warm. The healthy quorum's commit rate did not drop. Whether a stopped voter raises this client retry exhaustion was not isolated |

**NOTRUN.**
- Snapshot import into a stalled follower. It needs more than 1 GiB of
  command log beyond the follower's match, about 8 h at this rate. It is
  not an operator knob, and `raft-resync` only repairs a conflicting log.
- Leader admission at 3/4. Neither run drove a leader's own unapplied log
  near its bound.
- Multi-host, RDMA, and crash or restart while at the bound.

## K9 / W87: concurrent O_APPEND oracle

**Purpose.** Try to reproduce W87: buffered concurrent `O_APPEND` loses
records under load. It was never reproduced, and the last attempt was 23
rounds on a small VM ([attempt](w87-append-oracle-efs2-20261022.md)).

**Verdict: NOT REPRODUCED, 110 of 110 rounds exact. W87 stays OPEN**
(its gate is the original-load acceptance and a cause, not passing
rounds).

**Setup.** `tests/live/append_oracle_cluster.sh` started three efsd on
ports 19021–19023 (`EFS_MD_RAFT_N=3`, TCP, `--no-direct-io`, quota 4G,
stores `~/efs-gates-xor/k9/n1..n3`) and one efs-fuse mount with a 4 MiB
client cache (`EFS_DCACHE_BYTES=4194304`). Physical GC used its default
setting (on). `tests/live/gate_k9_append_load.sh` (new on this branch)
commits the background load that the efs2 attempt ran from scratch
scripts:

- **Churn:** create and rewrite eight files of 128 KiB–1 MiB per
  iteration in 32 names on the mount, and delete four. This drives
  physical GC. It ran without errors.
- **POSIX:** `posix_suite.py --filter concurrent_appends` in a loop, with
  every fifth pass the full core suite at `--jobs 4`.
- **Cold step:** both loops are paused at an iteration boundary for every
  cold step. Rounds 4, 8, … restart all three daemons; the others remount.

```bash
tests/live/gate_k9_append_load.sh run  REL /abs/k9 19020 90   # 90 minutes of rounds
tests/live/gate_k9_append_load.sh stop REL /abs/k9 19020
```

The oracle shapes rotated, 22 rounds each:

| Shape | Rounds | Acked records |
| --- | --- | --- |
| 8 writers × 40, open/close per record | 22 | 7,040 |
| 12 × 60, mixed, fsync every 7 | 22 | 15,840 |
| 16 × 60, persistent fd | 22 | 21,120 |
| 4 × 60, persistent fd, records up to 3,000 B | 22 | 5,280 |
| 16 × 100, mixed, 200 µs gap | 22 | 35,200 |

**Results.** It ran from 03:55 to 05:25 UTC (90 minutes; 4,050 s of
writing). There were **110 rounds and 84,480 acknowledged records**, 27
of them with a full three-daemon restart as the cold step. Every warm and
every cold read had every record exactly once: no missing, duplicate,
foreign, torn or reordered record, and the byte count matched. No
`write(2)` failed (the efs2 run had 3 `EBUSY`). The client log had no
decode errors or EIO. The POSIX loop made 262 passes: 210
`concurrent_appends` passes at 2/2, and 52 full core passes, 51 at
216/216 and 1 at 215/216. The failure was `mtime_monotonic_many_writes`
`timeout after 15s` at 04:04 UTC, when both K22 clusters also ran
(load average 26–29). It was not investigated further and passed in the
other 51 runs. Daemon logs had no fatal errors; the only abort lines were
`txn-finish ... abort decided (coordinator lost its group)` around
restart cold steps.

**Harness notes.** On this run the driver recorded the POSIX exit code
after `$(date)` had reset `$?`. Its `posix.rc` showed 0 for every pass,
so the counts above come from the suite's own summary lines. Its `stop`
also waited for loops that had already ended. Both are fixed in the
committed script.

**Limitations.** One host, loopback TCP, three daemons, one mount. No
two-mount peer appends, no NUC four-node GC fixture, no RDMA, no direct
I/O. The host was shared with K22, so load average was 15–29 for most of
the run, but there was no disk saturation like efs2's. Passing rounds do
not close W87.
