# Intel tiny-file performance rounds — 2026-10-23

**Three rounds on branch `devel-performance`; not merged. Round 3 is the one
measured win; round 2 cut client admission waits; durability-level fixes
(rounds 4–5) need design decisions.** User request: fix the tiny-file issue
found by [the Intel profile](intel-profile-20261023.md) on a
`devel-performance` branch, then redeploy, rerun the workload and profile,
for up to five rounds. Related: W125 (packing/GC), the identity admission
window (`EFS_OPID_REPLY_CACHE`), W126 (KV maintenance).

## Environment and method

- **Servers:** Intel cluster, four efsd: intel1 (10.0.0.207) holds n1/n2 and
  intel2 (10.0.0.209) holds n3/n4. TCP at 5 Gb/s; direct I/O; 400 GiB quota
  per node; `.efs` on.
- **Disks:** consumer NVMe ("EXT X200E"). Stores n2 and n4 are on the
  `/home` LVM of the second NVMe.
- **Client:** efs-fuse on xorinox, `/data1/efs/intel-mnt`, built from the
  same commit as the servers.
- **Workload:** `tiny_files.py --workers 32 --max-depth 2`, files of
  1–512 B, one 120 s run per measurement, without perf attached. The
  metric is files created divided by 120 s.
- **Rounds:** `scripts/intel_perf_round.sh OUT` deploys HEAD (devops
  `stop.sh`, `deploy.sh`, then `start.sh` with `EFS_COMMANDS=1`, storage
  kept), builds the client and runs the measurement. Since round 3 it also
  records 60 s of mid-run `iostat` on both servers.
- **Off-CPU:** `scripts/intel_offcpu_profile.sh` and
  `scripts/offcpu_summary.py` (`sched_switch` with DWARF stacks).

## Results

| Run | Build | files/s | Note |
| --- | --- | ---: | --- |
| baseline | `163241584` (deployed for hours) | 197 | single 120 s run |
| round 1 | `cd9264469`: `EFS_OPID_STATS` diagnostics | 423 | first run after restart |
| round 2 | `ad8f75eca`: admission retries wait on `opid_cv` | 313, then 202, then 132 | consecutive runs; restarting only the client did not recover |
| round 3 | `5d63d7bc6`: GC superseded deletes defer directory syncs | 395, then 424 | consecutive runs; no slowdown |

Throughput fell across consecutive runs on the same build before round 3,
and a restart reset it. That drift is larger than the effect of rounds 1
and 2, so neither has an isolated files/s gain. Each comparison is one run;
treat the numbers as indicative.

**Round 1, diagnostics.** With `EFS_OPID_STATS=1` the client logs refused
admissions by cause and the oldest unresolved identity.
- **Cause:** every refusal was the sequence window (`refused_window`); none
  was a slot shortage. This confirms the review's head-of-line reading.
- **Oldest holder:** the oldest identity was usually 1–60 ms old (maximum
  about 150 ms). In order of frequency it was held by a retained create
  (`efs_client_create_begin`), the create RPC, the open lease acquire and a
  release.
- **Window not widened:** a commit raising `EFS_OPID_REPLY_CACHE` to 64
  (`293d8e28c`) is reverted (`9fdbdf61e`). The review advised against it,
  and the data does not call for it.

**Round 2, event-driven admission retries.**
- **Change:** three loops retried a local admission refusal with a fixed
  1 ms `usleep`: create begin, open lease acquire and the close spin. They
  now call `efs_client_opid_wait(1)`, which waits on `opid_cv` (broadcast
  by `opid_end`) for at most 1 ms.
- **Effect:** in off-CPU captures at 32 workers, admission waits fell from
  about 42% of FUSE worker time (1 ms sleeps in close and create, plus
  `opid_begin` waits) to about 11%.

**Round 3, deferred GC directory syncs.**
- **The disks:** under the sustained load both servers' data disks ran at
  90–98% utilisation, with about 1,000 flushes per second.
- **The largest flush source on intel2** was GC: an `fsync` of the parent
  directory after every superseded-fragment unlink, and again after each
  emptied-directory `rmdir`. That was 10,705 in 8 s at 2.6 ms each.
- **Change:** superseded deletes now take the deferred path that whole-inode
  deletes already use. The reclaim journal keeps the obligation, and
  `retire_some` runs one `syncfs` per filesystem before any journal segment
  retires; replay redoes an undurable delete.
- **Effect:** the GC directory `fsync` disappeared from the blocking stacks.
  intel2's disk-sync time fell from 51.7 to 32.7 thread-seconds, and
  throughput held across consecutive runs.
- **Disk stats after round 3:** about 730 flushes/s on intel1 data and n3,
  about 930/s on n2, and about 13–31/s on n4.

## Remaining limits (rounds 4–5 not done)

After round 3 the client waits mostly on server round-trips. Mean waits per
RPC: REPORT 16 ms, PUT fragments 4.5 ms, lease edges 6–7 ms, create 3.7 ms
and lookup 1.4 ms. On the servers, handlers wait 3.6–4.6 ms per Raft apply
(`host_wait_applied`) and 2–4 ms for a proposal slot. The remaining flushes
are the key-value WAL's `fdatasync` (about 700–1,200/s), the Raft log's
`fdatasync` on the leader and followers, direct-I/O fragment writes, the
reclaim journal's `fsync` on accept, and the PUT barrier's `syncfs`.
Reducing these changes the durability design (two synced logs per
metadata write; a filesystem-wide `syncfs` barrier). That needs a design
decision, not a performance round.

A `wb_lock_budget` 1 ms polling loop (about 3.5% of worker time) was
considered for round 4 and skipped: ten tests compile that function with a
mocked clock that only `usleep` advances.

## Correctness on the round-3 build

| Gate | Result |
| --- | --- |
| `make test` (`scripts/make_test_summary.sh` on intel2, libtsan installed) | 1,039 commands, 0 FAIL, 14 NOTRUN (RDMA and environment) |
| Core POSIX (devops `smoke.sh`) | 216/216 (run twice) |
| Two-host POSIX (`smoke2.sh`) | 68/68 |
| Durability across remount (`smoke-persist.sh`) | 27/27 + 27/27 (run twice) |

Not run: crash or power-loss injection during GC deferral, and an A/B for
each round on a freshly reset cluster.

## Raw data (outside Git)

- **Round outputs:** files/s, `opid-stats` lines and `iostat` are on the
  driver Mac, in this session's scratchpad under `rounds/`.
- **Off-CPU captures:**
  - intel1/intel2 `/data1/efs/prof/offcpu-20261023/` and `/data1/efs/prof/offcpu-r3/`;
  - xorinox `/data1/efs/intel-client/prof/offcpu-20261023/` and `.../offcpu-r3/`.
  
  The hashes are in each run's `hashes.txt`.
- **Degraded-state CPU captures:** intel1/intel2 `/data1/efs/prof/degraded-*`.
