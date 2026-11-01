# GC backlog CPU: query-catalog rebuilds — 2026-10-17

**Deployed to orcd-mini as `859b2b9eb320`. All live suites PASS. Goal met:
efsd CPU during a GC-only backlog fell from about 52% to 3–7% of a core per
node.** One part did not work: the cleanup deferral has no effect in the
daemon (see "Deferral defect"). The 60 s cap and the exact dependency set
carry the gain. GC passes, budgets, cadence and ordering are unchanged.

On orcd-mini, the daemons' ~40% (or more) CPU during a long GC backlog was
not GC work. It was the query-catalog publisher rebuilding an unchanged
catalog every ~2.3 s, because GC's cleanup of already-unlinked inodes counted
as namespace changes.

Linked item: the user's report that, after deleting about 1.4M profiling files,
each efsd spent about 40% CPU "on GC" for a long time.

## Live profile (orcd-mini, passive)

- **When:** 2026-10-17 01:34–01:36 UTC.
- **Build:** `4aff22c62f9f` (n1 1610193, n2 1610201, n3 1610212).
- **Cluster:** RDMA `mlx5_0`, direct I/O, no client workload running on
  node9900.
- **GC state:**
  - n2 led both groups; about 411k pending GC records per group, reap capped at
    32 markers per pass;
  - gc-pass 88–123 ms every ~1.1 s, mostly the whole-inode fence
    (`reap_fence` 62–90 ms).
- **Method:** non-root perf on the existing PIDs:
  - `perf record -F 499 -g` on all three for 15 s;
  - `perf record -e cs -g` on all three for 15 s;
  - `-e cs --call-graph dwarf` on the leader for 3 s.

| Measure | Value |
| --- | --- |
| efsd CPU over 19 s | 94 / 102 / 98% of a core |
| On-CPU in `host_query_thread` → `efs_query_catalog_build_disk_checked` | **90%** (pread/copy 33%, `link` 12%, `unlink` 8%, `pwrite` 7%) |
| On-CPU in the GC thread (`host_gc_thread`, `server_gc_inode`, fsync) | about 1–3% |
| `query-publish` cadence | every ~2.3 s, `rebuilt=128 build_ms≈1300` |
| `query-disk: incremental` | `nodes_ms≈1000 changes=0 dirs=0`; files stay at 7,140,812 |
| g0/g2 applied index per publish | +130 each, which matches the GC proposals (~28 reaps/s per group, each a LANE_SWEEP_BATCH plus a REAP_BATCH) |
| GC fsync (gc-status) | 2.69M directory fsyncs for 342.5k fragment unlinks per node, average 92 µs |

Raw captures are outside Git on node9901 in `/data1/efs/prof/gclive/`.
sha256 prefixes:
- `cpu.data` `a3ba769372945ca9`;
- `cs.data` `d15a614c2397ef56`;
- `dwarf.data` `1ecd8f22cd27aa18`.

### Why it rebuilt

1. **Trigger.** Every 100 ms the publisher compares per-shard
   `metadata_revision` counters with the last publish. It rebuilds when 256 or
   more changes are pending or the oldest is 2 s old, at most once per 2 s
   (start to start).
   - The counters advance on every INODE, DENTRY, LANE, transaction, PVER,
     DSEQ, EXPORT or APPEND_CUR write.
   - That includes LANE_SWEEP's deletion of a dead inode's lane row and
     REAP_DONE's deletion of its nlink=0 inode row and append cursor.
   - The user-visible change (dentry removed, nlink set to 0) had already been
     published at unlink time. Reaping therefore dirtied about 128 shards per
     publish for no namespace change.
2. **Cost.** The incremental builder summarized changed shards as a 64-bit
   mask (shard % 64). Each shard kept a 64-bit filter of its getattr inputs,
   and that filter included its own shard (lane 0 lives there).
   - With 128 changed shards about 87% of the bits are set. Nearly all 4096
     shards therefore took the slow path: a full walk of 7.1M nodes through
     `inputs_changed`, plus hard links of every reused file.

## Change

1. **Deferred dead-inode cleanup.**
   - New `efs_kv_lsm_cleanup_scope()`: a nestable scope on the calling thread.
   - Inside it, DELETEs of INODE, LANE and APPEND_CUR rows still advance the
     shard's `query_revision`, which the builder uses to prove a shard
     unchanged. They no longer advance `metadata_revision`, which only
     schedules a refresh.
   - PUTs, DENTRY deletes and every other family count as before.
   - `efs_meta_apply_lane_sweep{,_batch}` uses the scope for its final lane
     delete. `efs_meta_apply_reap_done` uses it for its batch, which runs
     only for an absent row or one with nlink 0 and no lease.
   - The next refresh caused by any other change rebuilds those shards
     exactly. A stale dead node in the catalog is invisible in the meantime:
     files are counted through dentry edges.
   - `query-publish` now logs `deferred_cleanup=` (process-local count).
2. **Exact dependency set.**
   - The builder records the changed shards as a 4096-bit set.
   - Each generation keeps, in memory, an exact 4096-bit row per shard of the
     other shards its inodes' getattr reads (2 MiB, charged to the catalog
     memory budget). An unchanged shard is rescanned only if one of those
     shards changed.
   - The inode's own shard is no longer recorded, in either the row or the
     persisted filter. A changed shard is always rescanned in full, and an
     unchanged one cannot invalidate itself.
   - A restored generation, or one whose budget cannot fit the rows, falls
     back to the persisted 64-bit filter until a shard is walked.
   - New counter `walked=`: unchanged shards rescanned, logged in
     `query-publish` and `query-disk: incremental`.
3. **`EFS_QUERY_REFRESH_MS`.**
   - Minimum time between refresh attempts and the reported overdue age.
     Range 200..3600000; invalid values keep the default and log
     `query-config`.
   - **The default is now 60000 (was 2000)**, at the user's direction: a
     catalog minutes behind is acceptable. A ~1.3 s rebuild at most once a
     minute is about 2% of a core instead of about 60%.
   - The live gates (`tests/live/gc_reclamation.py`,
     `tests/live/query_publication.py`) pin 2000 unless the caller sets it,
     because they assert freshness within seconds. `tests/live/efs_commands.py`
     checks `refresh_ms` against the configured value.

## Tests

The tests are built and run on node9901 (RHEL 9.7) in the test tree
`/data1/efs/evidence/testsuite/qcat1`. That tree is release `4aff22c62f9f` with this
change's files overlaid, built with `EFS_GIT_ID=qcat00000001`.

- `tests/test_kv_lsm.c` `test_cleanup_scope_hints`: scoped INODE, LANE and
  APPEND_CUR deletes leave `metadata_revision` unchanged and advance
  `query_revision`. DENTRY deletes and puts inside the scope, and deletes
  outside it, still schedule a refresh.
- `tests/test_meta_apply.c` `test_reap_cleanup_deferred`: lane sweep plus
  reap_done of an nlink=0 inode schedule no refresh, and the inode and lane
  shards' query revisions advance.
  - Negative: with `reap_done` writing outside the scope, 2 checks fail.
- `tests/test_query_disk.c` `test_dependency_rows`, checked against the RAM
  catalog:
  - a cross-shard lane edit rescans exactly one shard and refreshes the
    file's stat;
  - an inode in a shard that collides with that lane shard modulo 64 rescans
    nothing;
  - a restored generation, which has no exact rows, stays exact through the
    filter.
  - Negatives:
    - forcing the filter makes the colliding step walk (1 check fails);
    - recording no exact rows produces a stale stat (4 checks fail).
- `tests/test_query_refresh.c`: interval parsing, range checks, and 60 s
  coalescing under a saturated backlog.
- **`make test` on node9901.** Each recipe command was run separately by
  `run_all.sh`; this is a clean rerun after the negative builds.
  - Result: 765 commands, 4 failures, all environmental:
    - `test_readindex_pair`, `test_raft_ack_window` and `test_host_outbox` are
      the known LeakSanitizer failures on this host; all three PASS with
      `ASAN_OPTIONS=detect_leaks=0`.
    - `docs/check-architecture.py`: its `links` check fails because `results/`
      was not copied to the test tree, and its `regen-diff` check fails on the
      unmodified release too. In the repository checkout all 5 checks pass.
  - Build: efsd sha256 `d40c2d07fe593a656f9ae79e16b3110d2dbfeff02179a4d5e48f6992cd191e22`.
  - Repeated with the 60 s default and the live-gate pins: 765 commands, the
    same 4 environmental failures. efsd sha256
    `500d658f4b0eb84ef317b198994c1dd4d681322c0719d2ccb92421405e19210c`.
- **NOTRUN:** `tests/live/gc_reclamation.py`. It starts private daemons, and
  the user asked for no extra clusters. GC scheduling and GC deletion code
  are unchanged; only the KV revision accounting for reap writes changed.

## Deploy and live results (orcd-mini, 2026-10-17)

### Deploy

- **Build:** `859b2b9eb320` (`v0.3.2-pre-alpha-602-g859b2b9eb`), from a clean
  `git archive` without `results/`, built 2026-10-17T15:57:33Z with the
  private libfuse.
  - node9901 `/data1/efs/releases/859b2b9eb320`:
    - efsd `06878171369ba8c01b4aee6c4aa654dfd80d3525c566a07eb0f8c012f765506b`;
    - efs-fuse `c993131b910e7bef3beaf433e8f0ed4c7639c71eb9d76b187e17875da84b68c7`.
  - node9900 `/data1/efs/orcd-mini-client/releases/859b2b9eb320`:
    - efs-fuse `7d1d7b905e84af318e34f417ce5f09e5242d6a1440967176fc637f1f50e800af`.
  - The first node9900 build missed `PKG_CONFIG_PATH`, compiled against the
    system libfuse headers and failed. It was rebuilt with the private
    libfuse.
- **Before:** daemons `4aff22c62f9f`, clients `155695ccd`.
- **Order:**
  1. `fuser -m` on both mounts: no users.
  2. Synced devops `4de926a` (`start.sh`, `test_start_mode.py`).
  3. Stopped the node9900 client, then `EFS_CLUSTER=orcd-mini ./stop.sh`.
  4. Switched both `src` symlinks, then `start.sh` and `client.sh start`.
- **After start:** the catalog was restored from disk (`query-restore rc=0`),
  with no full rebuild.

### Suites

| Suite | Result |
| --- | --- |
| `smoke.sh` POSIX, one mount | **PASS 216/216** (15.3 s) |
| `scripts/orcd_mini_posix2.py`, node9901 + node9900 | **PASS 67/67** (450 s) |
| `smoke-persist.sh` prepare/remount/verify | **PASS 27/27** |
| `tests/live/gc_reclamation.py --mode direct --posix`, TCP, private, 2 `/tmp` roots | **PASS** (71 PASS lines, physical inventory empty after restart) |
| Same with `--transport rdma`, `EFS_RDMA_DEV=mlx5_0` | **PASS** (71 PASS lines) |

The `gc_reclamation` gate had failed at `offline-member` (fsync EIO) on
`09f8af09c` and `4d6530dd8`; it passed here in both transports.

### Catalog and CPU

Pre-deploy baseline on `4aff22c62f9f` (passive, 20 s):
- each efsd at about 52% CPU, 86% of it in `host_query_thread`, 1.6% in GC;
- `query-publish` every ~2.8 s, `rebuilt=13–14`, `build_ms≈500`, 8.99M
  files.

The pending-GC estimate showed 1.93M records. That estimate only
overshoots; the real remaining backlog was about 8k fragments, reclaimed in
the first ~9 minutes after the restart.

Live test on `859b2b9eb320`, run with `scripts/orcd_mini_gc_delete_live.sh`
on node9900 and `scripts/orcd_mini_efsd_sampler.sh` on node9901 (30 s
samples):
- **Create:** 200,000 × 256 B files by 16 writers in 89.7 s, 2,229 files/s.
- **Delete:** `rm -rf` by 16 threads in 27.1 s, 7,382 files/s. Unlink p50
  1.81 ms, p90 2.37 ms, p99 12.2 ms, max 101 ms.
- **CPU while creating and deleting:** n1 (leader of both groups) about 380%,
  n2/n3 about 185%.
  - A 20 s on-CPU sample during the deletes: 75% in `host_query_thread`, 4%
    in GC.
  - Each 60 s publish absorbed real changes (`changes` 52k–197k), taking
    0.8–4.5 s per node per minute.
- **GC-only backlog**, 16:45:30–17:10 UTC, reaping the 200k inodes at the
  unchanged ~30 inodes/s:
  - **efsd CPU 3–7% per node**;
  - `query-publish` once per 60 s, `build_ms` 287–312, `walked` 0–1.
- Draining all 200k inodes at the unchanged schedule takes about 1.9 h; the
  test observed 25 minutes of it.

### Deferral defect (found live, not fixed)

`deferred_cleanup` stayed 0 on all nodes, and `rebuilt` grew 26 → 64 shards
per publish while only reaps ran.

- **Cause:** Raft apply buffers each entry's KV writes
  (`efs_kv_lsm_apply_begin`/`apply_end`, `raft_host.c` around 2706/2803) and
  commits them after the apply function returns. The commit is where
  `metadata_dirty` runs, and the cleanup scope opened in
  `efs_meta_apply_lane_sweep*`/`reap_done` has already closed by then.
- **Why the tests missed it:** the unit tests write to the KV directly and
  never use the apply scope.
- **Impact:** small with the 60 s cap and exact dependencies. Reap-dirtied
  shards are rebuilt once a minute at about 300 ms.
- **Possible fix, not implemented** per the user's instruction: carry the
  cleanup mark into the buffered apply entries (a flag on the apply-scope
  memtable entry) and honor it at commit. Add a unit test that applies a
  reap through `efs_kv_lsm_apply_begin/end`.

### ior (node9900, 8 local ranks, OpenMPI 5.0.10)

Run with `scripts/orcd_mini_ior_node9900.sh` (geometry from
`tests/perf/io500/run.sh`):

| Test | Write | Read |
| --- | --- | --- |
| IOR easy: `-F -e -t 1m -b 2048m`, 16 GiB | 1,499 MiB/s | 5,784 MiB/s |
| IOR hard: `-e -t 47008 -b 47008 -s 2000`, one 717 MiB shared file | 41.1 MiB/s | 157 MiB/s |

- Single node, no `-C` reordering, so reads may hit the client cache.
- IOR hard matches the known ~45 MiB/s shared-file write wall (W6).
- No previous orcd-mini ior baseline exists. The io500 runbook targets
  fcstor007 with 9 clients.

Raw captures stay outside Git. sha256 prefixes:
- node9901 `/data1/efs/prof/qcat-live/`:
  - `pre/cpu.data` `f88bc7f006b53d77`;
  - `post/cpu.data` `44a81d4554334cad`;
  - `sampler.tsv` `986fa1d586fa90b7`;
  - `gcr/tcp.txt` `0e2226096ccb340f`;
  - `gcr/rdma.txt` `c05f62c24b0e0754`.
- node9900 `/data1/efs/orcd-mini-client/prof/qcat/`:
  - `ior/easy.txt` `424112214725fb5a`;
  - `ior/hard.txt` `e78839512e1b1f11`;
  - `summary.txt` `6b7f6ee3c05c25d4`.

All test directories on the mounts and the gate roots were removed.

## Reproduce

Build the tree as above. Then:

```bash
./tests/test_query_refresh
./tests/test_query_disk
EFS_TEST_KV_SNAPSHOT_ONLY=1 ./tests/test_kv_lsm
./tests/test_meta_apply
```

To profile live, sample the efsd PIDs while GC drains a backlog, as in the
method above. Compare `query-publish` lines (`walked=`, `deferred_cleanup=`,
cadence) before and after.

## Remaining

- **Deferral defect.** Reap writes still schedule refreshes (see above).
- **No larger-namespace test yet.** The user plans one at a larger scale
  (2^32 files) later.
- **User deletes still trigger refreshes.** A real `rm -rf` of many files is a
  real namespace change and still triggers refreshes. Each one is now cheaper
  (exact walks); `EFS_QUERY_REFRESH_MS` bounds how often they run.
- **Fence fsyncs unchanged.** The fence path costs about 8 directory/file
  fsyncs per reaped inode per node. A prepared, unapplied change cuts it to
  about 5 by skipping a duplicate inode-directory fsync and caching death-page
  ancestor syncs; it is held separately. It shortens `reap_fence` but is not
  where the CPU goes.

**Raw evidence note (2026-10-23):** the bulk data in `testsuite` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
