# K22 / W23: universal follower memory/lag bound (Oct 22, 2026)

**Purpose.** Alpha 1.0 checklist row K22 (backlog W23, decision D30): a
follower whose compactor stalls, or whose apply falls behind, must stay
within a memory and lag bound on every growth path, the healthy quorum
must keep committing, and the follower must catch up once the stall clears.

**Verdict.**

| Gate | Verdict |
| --- | --- |
| Unit test `tests/test_follower_bound`, normal and `EFS_FAULTS=1` builds | PASS |
| `make test`, normal build, efs3 | PASS except `test_reclaim`, which fails identically on pristine devel on efs3 (environment) |
| Private 3-daemon cluster, stalled-compactor follower, scaled fault-build caps | PASS (run 2) |
| Same cluster before the capacity gate (run 1) | FAIL: the stalled follower became group 2 leader; fixed in `ac3e838ee` |
| Production-scale plateau (1 GiB L0 cap, derived 121 MiB budget) | NOTRUN |
| InstallSnapshot import into a stalled follower | NOTRUN (no snapshot was sent in either run) |
| Multi-host, RDMA, crash/restart while at the bound | NOTRUN |

## The bound

The KV memory budget is derived, with no operator setting:
`mem_budget = max(4 × memtable_max, min(L0 cap / 8, RAM / 16))`. A
replica's state machine holds at most `mem_budget` of unflushed memtable
(the WAL tail) next to an L0 held at its cap. Its Raft log holds at most
`max_unapplied = max(256, mem_budget / 256)` entries past `last_applied`.

| Path that grows | Enforcement |
| --- | --- |
| KV memtable / WAL tail (flushes deferred at the L0 cap) | `efs_kv_lsm_apply_room` defers apply with `EFS_ERR_BUSY` at `mem_budget`; the same entry is retried, never skipped; it kicks the flush or compactor |
| Follower unapplied log | A batch that would exceed `max_unapplied` is refused with a busy reply. Nothing is appended or dropped; the hint is the proven prefix. Successful replies carry a busy advisory once a full batch no longer fits |
| Leader send path | A busy peer gets heartbeats and commit probes only, never entries, until a reply clears busy (or two heartbeats pass) |
| Leader unapplied log | `efs_raft_propose` refuses `EFS_ERR_BUSY` at `max_unapplied` |
| Leader admission (D30) | New proposals are refused `EFS_ERR_BUSY` at 3/4 of either bound. DECIDE, RESOLVE and DROP keep the last quarter |
| Leadership (D30) | A replica under KV pressure or at 3/4 of `max_unapplied` does not campaign. A leader that stays so for four election timeouts steps down |

`efs-mgmt raft-status` prints the node bound (`kv_mt_bytes`,
`kv_mem_budget`, `kv_l0_bytes`, `kv_l0_cap`, `kv_apply_deferred`,
`admit_busy`). Per group it prints `unapplied`, `max_unapplied`,
`busy_sent`, `admit_busy`, `low_capacity`, `campaigns_held` and
`capacity_stepdowns`. Per peer it prints `busy` and `busy_hold`.

## Source and builds

- Branch `devel-k22` on devel `7c510c60f`. Commits `5ca8142cc` (bound),
  `b21ee8d25` (cluster driver), `ac3e838ee` (capacity gate, memtable
  fault hook).
- Built on efs3 from the working tree of those commits with
  `make -j4` and `make -j4 EFS_FAULTS=1`. The build ID is `unknown`
  because the tree was copied without `.git`.
- Run 2 binaries match `ac3e838ee`. Run 1 binaries were `5ca8142cc` plus
  the `EFS_FAULT_MEMTABLE_BYTES` hook, without the capacity gate.

## Environment

- efs3: Fedora 42 aarch64 VM, 4 vCPU, 1.9 GiB RAM, data disk `/data1`.
- The VM was shared with another session's private cluster. Load
  average was 13–15 with about 73% iowait, so absolute rates are low and
  noisy.
- Three `efsd` on 127.0.0.1 ports 18501–18503, TCP, buffered I/O
  (`--no-direct-io`), `EFS_MD_RAFT_N=3`, quota 8G, stores under
  `/data1/k22/run`.
- Nodes 1 and 2 ran mkfs and lead both metadata groups. Node 3 joined as a
  follower of both groups.
- Node 3 ran with `EFS_FAULT_COMPACT_STALL=1`,
  `EFS_FAULT_L0_BYTES=1048576` and `EFS_FAULT_MEMTABLE_BYTES=262144`.
  Its derived bound was a 1 MiB memory budget and a 4096-entry cap.
- Nodes 1 and 2 ran production values: 1 GiB L0 cap, 121.7 MiB budget,
  498378-entry cap.

## Reproduction

Build the fault tree, then run the committed driver in a screen:

```bash
make -j4 EFS_FAULTS=1
WORKERS=4 bash tests/measure/k22_follower_bound.sh "$PWD" /data1/k22/run
```

The driver runs four phases:

1. **Warm**, 60 s with the compactor running.
2. **Stall**: the fault file parks node 3's compactor. The phase ends
   300 s after a leader first reports node 3 busy, with a 3600 s deadline.
3. **Release**, 120 s with the load still running.
4. **Drain**: the load stops, and node 3 must apply every commit on both
   groups within 600 s.

The load is 4 loops of `efs-bench --meta` mkdir+create rounds of 2000
files with 16 workers each, direct metadata RPCs with no FUSE. The driver
samples every 5 s per node and group. It writes `samples.tsv` and
`summary.txt` under the run directory and stops its daemons on exit.

Cleanup:

```bash
edelete --force --delete /data1/k22
```

On efs3 `/data1` is owned by `efs`, so the run directory was created and
its empty top level removed with `sudo`.

Retained outside Git on efs3 under `~/k22-evidence/`:

| File | sha256 |
| --- | --- |
| `run2/samples.tsv` | `ca532260b703eda5855923a5293789377134e1fbf750c248db89d82e971575d5` |
| `run2/summary.txt` | `8f98b661c9c66da9ba5b85b2d42ca540142d4d7e9d85e532ac3df2708064dcfc` |
| `run1-pre-capacity/samples.tsv` | `32ecec03b3b0c3fcdc0d8e18b80c543bedff7583e34bf57769fd744c8aff4182` |
| `run1-pre-capacity/summary.txt` | `d60eac6786ff52c5eae65fc87d978b5e9c44e87e96d9692c0bc97a7298cc21b1` |

The store directories were deleted.

## Results: run 2 (PASS)

Node 3, group 0. `t` is seconds from start. The stall began at t≈61 and
the release came at t≈572.

| t | phase | RSS KiB | unapplied / cap | busy refusals | memtable / budget | L0 / cap | applies deferred |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 40 | warm | 13748 | 0 / 4096 | 0 | 172326 / 1048576 | 101 / 1048576 | 0 |
| 205 | stall | 18976 | 24 | 0 | 62130 | 1062921 | 0 |
| 245 | stall | 21664 | 1487 | 0 | 1048610 | 1062921 | 5335 |
| 285 | stall | 22440 | 4096 | 363 | 1048610 | 1062921 | 18085 |
| 406 | stall | 22440 | 4078 | 2436 | 1048898 | 1062921 | 52128 |
| 568 | stall | 22440 | 4065 | 4852 | 1049268 | 1062921 | 92559 |
| 589 | release | 25640 | 0 | 4957 | 225596 | 321981 | 94911 |
| 695 | drain | 27652 | 1 | 4957 | 126824 | 293132 | 94911 |

**Plateau.** For 283 s (t=285–568), node 3's RSS held at 22440 KiB and
its unapplied log at 4065–4096 entries. Its memtable stayed within 1 KiB
of the budget, and its L0 at the cap plus one flush.

| Peak during the stall | Value | Bound |
| --- | --- | --- |
| Memtable | 1049268 | 1048576 budget |
| L0 | 1062921 | 1048576 cap |
| Unapplied entries | 4096 | 4096 cap |
| RSS (node 3) | 22440 KiB | — |
| RSS (leader nodes 1 and 2) | 50812 / 33228 KiB | — |

**Quorum progress.** Group 0's leader commit index moved at about the
same rate before and during the plateau. Nodes 1 and 2 admitted
everything: `admit_busy` stayed 0 on every node.

| Interval | Group 0 commits/s |
| --- | --- |
| Warm | about 58 |
| Stall, before the bound | about 51 |
| Plateau, t=285–568 | about 54 (17006 → 32350) |

**Leadership.** Node 3 hit its election timeout once, at 21:05:39, in
both groups. It logged "election held: low capacity" and stayed a
follower; no leadership changed in the run.

**Recovery.** After the fault file was removed, node 3 applied its 4065
entries and reached `unapplied=0` within 17 s with the load still
running. In drain it matched the leaders' commit on both groups
(38961 / 27484) 5 s after the load stopped.

**Client errors.** Some bench rounds failed with `EBUSY` after 16
BUSY/STALE retries: 15 rounds in stall and 4 in release. The leader's
400 ms apply wait fired at the same rate in warm as during the stall.

| Minute | Leader `apply-sleep` count | Phase |
| --- | --- | --- |
| 20:57 | 4902 | warm |
| 20:58–21:07 | 1383–8780 | stall and release |

The failures follow the overloaded VM's commit latency, not the bound,
which refused no admission. Warm was too short to show a baseline
failure count.

## Results: run 1 (FAIL, fixed)

The same run without the capacity gate reached the same plateau: 4095 of
4096 entries, memtable and L0 at their bounds, 270 busy refusals and
full catch-up 5 s into drain.

At 20:46:37, 11 s into the plateau, node 3's election timer fired. Its
inbox drop counter was rising on the overloaded VM; the exact cause of
the missed heartbeats was not isolated. Its log was up to date, so it
won group 2. With apply deferred it refused every proposal: 11206
`admit_busy`. Group 2's commit index stayed at 12796 until the stall
cleared 5 minutes later. This violated D30, which says leadership
requires local capacity. `ac3e838ee` fixed it, and run 2 shows the fix.

## Limitations and remaining gates

- **Scaled caps.** The cluster plateau used fault-build caps (1 MiB L0,
  256 KiB memtable). At this VM's metadata rate, about 40 entries/s per
  group, the production 1 GiB / 121 MiB / 498k-entry bound would take
  hours to fill. The unit test and the cluster use the same derivation
  code, but the production-scale plateau is NOTRUN.
- **InstallSnapshot.** Importing a snapshot into a stalled follower is not
  gated by the KV budget. No snapshot was sent in either run.
- **Bytes on disk.** The unapplied-log bound counts entries. Its RAM cost
  per entry is the resident index record; command bytes are on disk,
  bounded by the cap times the entry size and by the storage quota.
- **Not run.** Multi-host, RDMA, crash/restart while at the bound, and a
  leader whose own compactor stalls in a real cluster. The leader
  step-down is covered only by the unit test.
- **Pre-existing failure.** `test_reclaim` fails on efs3 with or without
  this change.
