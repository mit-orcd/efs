# orcd-mini deletes, GC and crawl on `5d673efa8` — 2026-10-18

**Profiling only (no code change). Three findings:**
1. **GC reclaim is throttled by its schedule, not by resources.**
   - Each node reclaims 0.18–0.21 GiB/min (48–57 fragments/s), whether or
     not deletes run.
   - The GC thread is busy about a third of the time: one pass per second
     of 120–336 ms, capped at 32 reap markers per group.
   - Directory fsyncs use 3–4% of one thread, and the disk is 54–81%
     utilised by everything combined.
   - A delete of ~60 GiB takes hours to come back as quota.
2. **A read-only open/close costs about two Raft commits.**
   - These are the per-inode lease edges, OPEN at first open and CLOSE at
     last close.
   - A small-file read workload is therefore also a Raft write workload:
     the md5 reader alone kept group 2 at about 6,000 commits/s.
3. **Crawl (stat) throughput is now 49.0 K ops/s,** against the 10.6 K
   ops/s in the user's earlier `ecrawl` report. The namespace differs
   between the two runs.
   - LOOKUP's KV read (`lsm_get`, `kv_mtab_pos`) is the largest server CPU
     path.
   - ReadIndex admission is a quarter of the leader's (small) lock waits.

Follows the [`5d673efa8` profile](orcd-mini-profile-5d673efa8-20261018.md)
and the [2026-10-17 live GC notes](orcd-mini-gc-query-catalog-20261017.md).

## Environment and workload

- **Build:** orcd-mini on `5d673efa8fca`, daemons and both clients. Three
  efsd on node9901, RDMA `mlx5_0`, direct I/O, `.efs` on, `EFS_EXPIRY=0`.
- **Leadership:** node 3 led both groups.
- **Capacity:** quota was at 99.4–99.8% (716–718 of 720 GiB per node)
  after the user's two writers hit ENOSPC at about 16:17Z.
- **User workload, on node9901:**
  - `edelete --delete --force tiny-files-evict-20261017-0635` and
    `…-0635-b/`. Both started about 16:32Z and had finished by 16:41Z.
  - `tiny_files_md5.py /data1/efs/mnt/`, running throughout.
- **Added for the tests:**
  - an 8-thread read-only reader for 12 s (`readtest.sh`);
  - `ecrawl --no-write` on node9900 for 150 s (16:45:57Z).
  - Neither writes data.

**Method:** `gcround.sh` on node9901, one round per window. It records:
- `efs-mgmt gc-status` on all three nodes, quota and Raft commits, before
  and after (41–51 s windows);
- 15 s on-CPU samples of all efsd (`perf -F 499 -g`);
- 10 s of context switches on all efsd;
- 3 s of DWARF context switches on the leader;
- 10 s of `iostat -x` on `/data1` (`xi_dev00`);
- 10 s of client and efs-fuse on-CPU samples.

`gcsum.py` turns the snapshots into rates and averages the `gc-pass` log
lines inside each window.
- Rounds r1 and r2 ran during the deletes, r3 after them.
- One extra round ran under the crawl.

## 1. GC reclaim

| Round | Raft commits/s (g0 / g2) | Fragments removed/s per node | Reclaimed per node | GC passes/s | Mean pass (ms): reap / sweep / fence / done | Directory fsyncs/s (avg µs) | Disk util |
| --- | --- | --- | --- | --- | --- | --- | --- |
| r1 (deletes) | 5,650 / 5,162 | 48 | 0.18 GiB/min | 1.01 | 336: 325 / 112 / 93 / 114 | 383 (83–86) | 54% |
| r2 (deletes) | 4,789 / 5,968 | 48 | 0.18 GiB/min | 1.01 | 312: 303 / 96 / 98 / 102 | 386 (98) | 63% |
| r3 (md5 only) | 58 / 5,843 | 57 | 0.21 GiB/min | 0.99 | 120: 115 / 21 / 71 / 20 | 457 (65) | 81% |
| crawl | 58 / 4,882 | 57 | 0.21 GiB/min | 1.01 | — | — | — |

- **Constant rate:** reclaim is the same with and without the deletes, and
  matches the 0.23 GiB/min seen all day after this morning's test filled
  the quota. Quota fell from 717.82 to 715.38 GiB per node between 16:34Z
  and 16:48Z.
- **Pass is capped:** every pass reports `capped=1` with `reap_seen=32`
  per group, `REAP_SCAN_MAX` (`src/server/raft_host.c`).
- **Stale estimate:** `pending_estimate` stays at 4,626 for group 0
  (sampled once, it is not a live count).
- **Schedule:** the pass repeats after `GC_LOOP_MS` = 1000 ms. The opt-in
  `EFS_GC_BACKLOG_IDLE_MS` (20–1000 ms) shortens that wait only when a
  capped pass made progress. orcd-mini leaves it unset, and `start.sh`
  does not pass it through.
- **Cumulative cost per node since start:** 2.41 M fragment unlinks
  (27 µs average) and 19.2 M directory fsyncs (47 µs average), about 8
  fsyncs per removed fragment and about 913 s of fsync per node. The
  unapplied fence-fsync patch from the 2026-10-17 notes targets this; it
  would shorten `reap_fence`.

**Headroom:**
- **Shorter idle:** with a 20 ms idle, passes would run back to back
  while a backlog exists. At 120–336 ms per pass that is about 3–8 passes
  per second instead of 1, so roughly 3–8 times the reclaim rate, with
  the same per-pass budgets.
- **Larger cap:** raising `REAP_SCAN_MAX` would raise it further, at the
  cost of longer passes.
- **Next step:** a controlled A/B, `orcd_mini_gc_delete_ab.sh` from the
  2026-10-17 notes with `EFS_GC_BACKLOG_IDLE_MS` unset versus 20. Enabling
  it on orcd-mini needs a restart and a `start.sh` change.

## 2. Deletes

**CPU, all three efsd, r1 (deletes running), children:**

| Path | Share |
| --- | --- |
| `host_query_thread` → `efs_query_catalog_build_disk_checked` (the 60 s catalog refresh) | 14.0% |
| `host_pump` | 10.0% |
| `server_raft_host_unlink` (8.3%), of which `unlink_batch` | 4.0% |
| `lsm_get` / `kv_mtab_pos` | 7.3% / 4.9% |
| `host_apply` | 7.2% |
| `kv_compact_locked` (KV compaction) | 6.2% |

**Leader lock waits** (DWARF, r1) were 14.9% of its switches:
- `host_propose` via `host_propose_mode`, the unlink proposals: 44.0%;
- the submit path: 9.8%;
- `efs_raft_disk_sync_depth` and `efs_raft_disk_sync_release` under
  propose: 11.3%.

After the deletes (r3), lock waits fell to 6.6% of switches.

The catalog refresh is the largest single CPU consumer during a large
delete. It is bounded to once per 60 s, but each rebuild has real work
because the namespace changes. The deletes' file rate could not be
measured: `edelete` reports to the user's terminal, and syscall
tracepoints need root.

## 3. Read-only opens commit

- **Mechanism:** efs-fuse takes a sequenced server lease per (session,
  inode). It proposes an OPEN edge at the first open and a CLOSE edge at
  the last close (`struct efs_open_ref`, `src/client/efs_fuse.c`). That
  includes `O_RDONLY` opens of files that are only read.
- **Measurement** (`readtest.sh`): an 8-thread reader opened and read
  27,363 existing files in `tiny-files8` in 12 s (2,280 files/s).
  - Group 0 (those files' group) went from 59 to 4,592 commits/s.
  - That is about 2.0 commits per file.
- **Background:** with only `tiny_files_md5.py` running (700–970
  files/s), group 2 committed 5,600–6,600 entries/s.
- **Why it exists:** the lease keeps an open file alive if it is unlinked,
  and it carries server-side locks.
- **Implication:** with tiny files, every read pays two Raft commits plus
  the proposal path on `h->mu`, which is 44% of leader lock waits under
  load. Avoiding it for read-only opens would be a design change, for
  example taking the lease lazily or only for writable or locking opens.
  It needs a decision on open-unlink semantics; it was not attempted here.

## 4. Crawl (stat) path

`ecrawl --no-write --progress /data1/efs/orcd-mini-mnt/` on node9900 (16
crawl threads), 150 s cap, alongside the md5 reader:
- 4,608,063 entries (2,044 directories) in 94.05 s;
- `avg_ops_per_sec=49.0K`, `queue_lock_waits=157`, `crawl_idle_ns=79.0 s`.

The user's earlier run on the same mount: 6,346,070 entries (3,164
directories) in 601.3 s, `avg_ops_per_sec=10.6K`. Its build and namespace
differ, so this is not a controlled comparison.

**Server CPU during the crawl** (all efsd, children):
- `server_raft_host_lookup` 14.1%;
- `lsm_get` 9.5% and `lookup.constprop.0` 8.9%;
- `efs_txn_read_ex` 7.2%;
- `efs_meta_apply_getattr` 6.7%;
- `kv_mtab_pos` 5.5% (3.7% self, the KV memtable search);
- `kv_seg_probe` 2.5%.

**Leader lock waits** were 7.4% of its switches:
- `host_propose_impl` via submit (the md5 reader's lease edges): 30.2%;
- `host_pump`: 19.3%;
- ReadIndex admission (`host_read_index_admitted` from LOOKUP): 14.6%;
- its condvar wake: 10.3%.

The stat path is CPU-bound in the KV read, not lock-bound. The memtable
search (`kv_mtab_pos`) is the clearest single candidate.

## Reproduce

On node9901, with the scripts in `/data1/efs/prof/gc-prof/`:
- `gcround.sh DIR` (sha256 prefix `ae21208732e60862`) for one round, or
  `rounds.sh` for three rounds 100 s apart;
- `gcsum.py DIR [efsd logs]` (`8190cb1653a7486c`) for rates and gc-pass
  means;
- `readtest.sh` (`3704475d17ac5ab9`) for commit rates with and without an
  extra read-only reader. Point its `root` at a populated tree.

Run `cs_dwarf_locks.py` and `cs_lock.py` from `scripts/`. Run the crawl on
node9900 with `timeout 150 ~/git/ereport/ecrawl --no-write --progress
/data1/efs/orcd-mini-mnt/`.

Prerequisites: `perf_event_paranoid <= 1`. These are read-only and
profiling-only steps, but check `fuser -m` so the user's workload is known.

Raw output (outside Git):
- node9901 `/data1/efs/prof/gc-prof/`:
  - `r{1,2,3}/`, `crawl/` (`perf.data` files, snapshots, `iostat`;
    4.4 GB);
  - snapshot hashes: `r1/snapA.txt` `bd43a262fa165a6b`, `r1/snapB.txt`
    `064d4c0a7afa16ab`, `r2/snapA.txt` `1df2f9a1c1429074`, `r3/snapB.txt`
    `1ee107c64e577db2`, `crawl/snapB.txt` `ac90b6be8842d8c4`.
- node9900 `/data1/efs/orcd-mini-client/prof/crawl-5d67/crawl.log`.
- The 16:04–16:11Z user-workload lock profile is in
  `/data1/efs/prof/mu-prof2/`.

## Limitations

- The workload was uncontrolled. The deletes ran only during r1–r2, and
  their file rate is unknown.
- The quota was full, so GC ran with its real backlog. The reclaim rate
  is a schedule limit and should not depend on fullness, but that was not
  measured separately.
- These are 3–15 s snapshots of one leader; followers were included only
  in the all-efsd CPU and context-switch captures.
- The headroom estimates for `EFS_GC_BACKLOG_IDLE_MS` and `REAP_SCAN_MAX`
  are arithmetic from pass timings, not measurements.
