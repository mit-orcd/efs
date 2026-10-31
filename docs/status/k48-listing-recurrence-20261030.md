# Task BG — K48 listing recurrence attribution

**Verdict: ATTRIBUTED (b), server page serving during a cross-group directory migration; fixed by devel `dabcf937a` and companions. K48 moves OPEN → VERIFY, blocks alpha: yes until the named rechecks below.** The Task AU incident itself was not traced name-by-name (its capture never recorded the missing name or any page); it is attributed by reproducing the same test, signature and source on a bounded trace. No product code changed in this task.

Item: K48 ([checklist](alpha-1.0-2plus1-checklist.md)), W130 one-replica halt (`tests/test_w130_dir_fault.py`). Earlier work: [Task S](k48-readdir-omission-20261028.md), [Task V](a4-lab-topology-dryrun-20261028.md), [Task AU](au-pump-kicks-20261028.md), [AN port](an-product-port-20261030.md).

## Source and builds

| Tree | Commit | Notes |
| --- | --- | --- |
| Devel under test | `8b0d128cc` + this branch's instrumentation (`f36362629`, `772e4cd91`, `9c07a8e2d`) | test-only trace plus the storm; the trace is off unless `EFS_TRACE_READDIR` is set |
| Task AU base | `9c4bfd171` (Oct 28 21:36) | the incident source; wave-28 layout revalidation `c0081d095` is included |
| Base + one fix | AU base + `dabcf937a` diff | single-fix A/B |

The same trace function was ported by hand onto the AU base (its wire structs differ). All trees were built with `EFS_FUSE_PREFIX=/usr make -j8 test-bins` on intel1 (`EFS_BUILD_ID` "unknown": builds are identified by commit and the tree names above, kept under `~/efs-taskBG/` on intel1).

## Environment

intel1 (8 CPUs, Linux 6.12 el10), private three-node loopback cluster per run, TCP, buffered I/O, the W130 fault build (`tests/faults/efsd`), 96 target files in one directory, ephemeral loopback ports and fresh per-run `TMPDIR` under `$HOME`. Nothing touched the live clusters, `/data1`, or any other host; intel2 was used only to read the retained case. "Under load" means a continuous `make test-all` at 8 jobs ran in parallel on the same host (1-min load average 6–16).

## What the retained Task AU case contains

Copied read-only from intel2 `~/efs-artifacts/taskAU-20261028/k48-w130-case/` and scanned with the Task S offline reader (`scripts/forensics/k48_store_scan.c`) against the AU source, on copies only.

- Directory inode **327** (odd, so metadata group 0). Node 3's group 0 halted at Raft index 411, a directory-migrate transaction, at 21:13:12.8Z; nodes 1 and 2 hold identical state: **96 dentries** (26 on the source shard, 36 on group-2 lane shards, 34 on group-0 lane shards), layout SPLITTING, 65 entries, so the directory was still migrating at shutdown. Node 3's group 0 is frozen at split-begin (64 local rows).
- **Not recoverable:** which name was missing, which voter served the listing, the page boundaries, and whether the missing dentry's commit preceded the listing. The case has no READDIR log lines and `mnt-a.log` has no RPC lines. This is the evidence gap Task S already named; it is why the attribution below uses reproduction.

## Method

1. `tests/test_w130_dir_fault.py` now sets `EFS_TRACE_READDIR=1` (client and server print one bounded stderr line per READDIR page: peer, cursor, count, next cursor and the names) and, on any short listing, writes `k48-partial.txt` into the kept work directory: missing names with lane, lane shard, group and source shard; stat inodes; relists after 0 s and 1 s; per-voter `efs-mgmt raft-readdir` paging; the page trace for the parent. The lane maths (FNV-1a mod 64, shard = `(ino&0xFFF + lane*(2*((ino*2654435761)&0x7FF)+1)) & 0xFFF`, group = shard parity) is a Python port of `src/meta/dir_layout.c`.
2. After the existing step-3 check the test now lists the directory repeatedly for `W130_STORM_SECS` (default 5 s) and fails on any count other than 96 ("storm listing"), so the transient migration window is sampled thousands of times instead of once.
3. `scripts/k48_attribution_loop.sh SRC OUTDIR RUNS` runs the test N times, one fresh cluster per run, keeping only failing cases.

Reproduce: build `SRC` as above, start a load (`EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 sh scripts/make_test_summary.sh` in another tree), then

```
W130_STORM_SECS=20 sh scripts/k48_attribution_loop.sh SRC $HOME/efs-artifacts/taskBG-20261030/NAME 20
```

(`W130_STORM_SECS=0` for the single-listing form). Expected on a fixed tree: `rc=0`, "N storm listings complete and exact". Cleanup: `~/bin/edelete --force --delete` on the OUTDIR scratch.

## Results

| Run set | Tree | Runs | Failures | Notes |
| --- | --- | --- | --- | --- |
| Single listing, load, 100 runs | devel | 100 | **0** | max 35 s |
| Single listing, load, 100 runs | AU base | 100 | **2** (runs 91, 100) | `partial listing 95 != 96`, same signature as AU |
| 20 s listing storm | AU base | 6 | **6** | failing listing at index 510, 172, 821, 1414, 1267, 593 |
| 20 s listing storm | base + `dabcf937a` | 6 | 0 | 102,650 listings |
| 20 s listing storm | devel | 6, then 20 | **0** | 288,313 listings in the 20-run set |

In all eight base failures the missing name belongs to a lane owned by the **other** Raft group than the parent directory's own shard group (for example `f0053`, lane 29, group 2, parent 2779 in group 0; `f0026`, lane 13, group 0, parent 1196 in group 2). The page trace shows one voter (node 1) serving every page; in a failing listing page 2 returns 31 of 32 names while page 1's cursor has shifted by one, and a relist immediately after returns all 96. So the name is not lost, not mis-sorted by the client, and not racing the test's own writes (all writes and the halt precede the listing).

Unit-level confirmation: `tests/test_dir_migrate_visibility.py` (in `make test`, added by `dabcf937a`) fails on the AU base (`efs_meta_dir_migrate_one` is not BUSY while the copy is unresolved) and with `--expect-gap` prints `first=source partial=95/96 REPRODUCED GAP`; it passes on devel.

## Attribution

- **(c) test wait condition: excluded.** The failing listings come after the halt and every write; the same state lists 96 a moment later.
- **(a) client overlay/cursor: not supported.** The client dedups by name in `readdir_collect_page` and carries an (src, name) cursor; the trace shows the server returning the short page. Page 1/2 straddle the moved entry, which is the server's view.
- **(b) server page serving: shown.** On the AU base `host_dir_migrate_txn` (`src/server/raft_host.c`, one 2PC that writes the hashed destination row and deletes the local source row) resolves its two groups independently. A reader that scans local rows and materialised lane rows only, between the two RESOLVEs, sees the name in neither. Devel closes this window in three places: `dabcf937a` keeps the source row until the cross-group copy resolves (`src/meta/dir_layout.c`, `host_dir_migrate_txn` at `raft_host.c:19838`); `35a6e41b0` keeps the local listing until the copied lane is enumerable; K74 (`34fcf33f3`, `37348f212`) makes the reader merge committed unresolved destination intents. Single-fix A/B: `dabcf937a` alone already clears the 20 s storm 6/6.
- Wave 28 (`c0081d095`, in the AU base) fixed a different, intra-call race (layout and bitmap revalidation between scan and publish). Its own note says the fix is "not a snapshot across separate page RPCs"; this task shows the residual cross-RPC/cross-group window it left, which is the AU recurrence.

## Limits and remaining gates

- The original Task N and Task V incidents and the retained AU case are **attributed by matching signature, source and mechanism, not traced individually**; their missing names were never captured. Treat the attribution as established for the mechanism, not proven for each incident.
- `EFS_READDIR_MAX` is 256 on devel (64 at the AU base), so devel lists 96 names in one RPC and the base in two. The fixed-tree result therefore combines the migration fix with the larger page; the `dabcf937a`-only A/B (same 64 page) isolates the fix.
- Loopback TCP on one host only; no RDMA and no multi-host run.
- **Remaining (K48 stays blocks alpha: yes until done):** (1) the W130 storm gate on the frozen candidate (`W130_STORM_SECS=20`, 20 runs under 8-job load, 0 FAIL); (2) the K74 live listing on ORCD with a halted metadata replica.

## Gate on this branch

Final tree `9c07a8e2d` (instrumentation, `tests/budgets.tsv` row for W130's 5 s storm, A6 inventory row for `EFS_TRACE_READDIR`, boundary-test stub). W130 ten serial runs: RESULT_W130. `make test-all`, 8 jobs: RESULT_GATE.

An earlier gate on the first instrumented tree failed two tests that this task caused and then fixed: `test_env_inventory.py` (no inventory row for `EFS_TRACE_READDIR`) and `test_readdir_reply_boundary.py` (its standalone compile lacked the trace hook); both pass on the final tree.

## Raw captures (outside Git)

On intel1: `~/efs-artifacts/taskBG-20261030/` (`loop100`, `base100`, `base-storm`, `devel-storm`, `devel-storm2`, `v1-storm`, `final-w130`, `final-gate.log`, `k48-w130-case` copy) with `SHA256SUMS`: HASHLINE. Retained original case: intel2 `~/efs-artifacts/taskAU-20261028/k48-w130-case/` (read only). Build trees: intel1 `~/efs-taskBG/{devel,au,v1,final}-src`.
