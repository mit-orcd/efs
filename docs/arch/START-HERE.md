# Start here — what to work on, and what to read first

[Architecture](../architecture.md) · [Roadmap](../scaling-roadmap.md) ·
[Development](development.md) · [Verification](verification.md) ·
[Product gaps](../product-gaps.md)

This page exists because of the bar in [development.md](development.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---

## 1. The task right now

**Where the project is (Sep 20 2026).** [architecture.md §10](../architecture.md)
steps 0–12 are **all landed and gated**: simulator, KV, Raft, cross-shard
txns, sessions, directory spread (including populated leftover migrate as a
2-shard txn), delete-2PC, and FUSE A–D. LOCAL dirs auto-begin SPLITTING when
`nents > EFS_DIR_SPREAD_MIN` and the background migrator drains leftovers to
HASHED. Production `raft-change` wires `efs_raft_change` in `raft_host`
(operator desired file + learner attach with C_old, then joint/COLD).
InstallSnapshot is in the Raft SM, and `raft_host` snapshots after
`efs_kv_lsm_flush` when `applied - snap_idx ≥ 256` using the existing WAL
item payload (`kv_snap.c`), not a new dump format.

**So there is no next §10 step.** What is left is the work queue in
[§1a](#1a-the-work-queue) — measured gaps, in the order they should be taken.
**Every open measurement/bug-chasing item has a runbook + script in
[runbooks.md](runbooks.md) (`tests/measure/*.sh`, pinned to build
`b2184a5c7faf-dirty`); start there.** `tests/preflight.sh` is the deploy
rule's pre-flight as one command — run it before anything else.
**W1–W5 done; W6 correctness gate met Sep 20** (its three open sub-items
are performance and each needs a user decision — see W6). Take the
lowest-numbered item that is not marked done; correctness items come before
every performance item. Each item names what to change, how to measure it,
what proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4); several items below are blocked on
exactly that and say so.

**The one live cluster is port 19810** on fcstor003–006 (`/data1/01–06/efs`,
`--quota 36T --direct-io`, TCP), clients fcstor007–015 at `/tmp/efs-mount`.
19820 is retired. Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs`
without being asked — several items below run on the existing data. Before
touching the cluster, run the **pre-flight** in the fcstor deploy rule
(`.cursor/rules/efs-fcstor-deploy.mdc`).

### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**; it is mechanical and
the code is already unit-gated. Update or delete this block when done — an
"in flight" block older than the last commit is a bug in this page.

**Progress log (newest first — read this before the state below):**

- **12:25 (Sep 21)** — **W6 same-directory rate, full runbook**
  (`PERF=1 tests/measure/samedir_rate.sh`,
  `results/measure/20260921-161931-samedir-rate`). Storm PASS at 1×1,
  1×9, 4×9. Aggregate **138 / 134 / 159 ops/s** — flat, so adding procs
  does not add throughput. `busy_n=0` rules out the BUSY backoff.
  `checkout_us` ~15 ms/client rules out the conn pool. Fuse `recv_us`
  ≈ storm wall rules in server+wire wait. Leader log tail is the storm's
  own PREPARE/CREATE/UNLINK/RESOLVE/LEASE_CLOSE/REAP_DONE (GC_ACK 0.4 %,
  so the earlier 52 % GC_ACK was an idle-tail artifact of the small
  smoke). On-CPU profile: `memcmp` in `lsm_scan_prefix` under
  `reduces_pending`, `guards_conflict`, `efs_txn_resolve`. Next runbook
  in order: `ior_hard_scaling.sh` at `NPS="1 4 9 36" SEGS=3000`
  (open-cost §2 is already answered by the smoke).
- **11:30 (Sep 21)** — **Runbooks for every open measurement item**
  ([runbooks.md](runbooks.md), `tests/measure/*.sh`, `tests/preflight.sh`),
  all pinned to the running build `b2184a5c7faf-dirty`, each smoke-run
  once (dirs under `results/measure/20260921-*`). Two findings from the
  smokes that change the questions: **(1) 1 GiB cold open is 0.23 s idle,
  not 20 s** — exactly 128 GETCHUNKS × 1.6 ms; with 32 concurrent openers
  GETCHUNKS is **14.6 ms** (9.5×) and open 1.6–2.5 s, one map fetch per
  host — so the IOR 20 s is server-side GETCHUNKS serialization under 36
  ranks, not per-open cost. **(2) Same-parent storm is 171 ops/s at
  1 proc AND at 4 procs with `busy_n=0`** — a flat aggregate ceiling that
  is not the BUSY backoff (leader raft log tail: 52 % `GC_ACK`). Also:
  `EFS_RPC_PROF=1` counters only dump on an RPC, ≤ every 2 s — read them
  after a trivial RPC (`rpc_prof_last` in `tests/measure/lib.sh`), and they
  do **not** count REPORT (`rpc_send_recv_dual` unprofiled) — the REPORT
  number is the server's `report-split` line, which the write runbooks now
  collect. **(3)** 1-client 8 GiB dd+fsync today 380–418 MiB/s (Sep 18:
  639); its single REPORT was `pack 6.9 s + push 5.9 s + finish 1.0 s` =
  **13.9 of the 21.5 s wall** — pack+push, not the apply wait, is the tail.
  Next: run the runbooks at their default (full) settings, one at a time —
  order in runbooks.md §1–7; then bring the tables to the user.
- **08:45 (Sep 21)** — **Step E DONE as option (b), §7.2 end state:** the
  parent inode row, the dseq emptiness witness and the HASHED dir-lane
  stamp are **commutative reductions**, not EXCL full-image CASes. New txn
  kinds `EFS_TXN_REDUCE_INO` (signed `nlink`/`nents` delta, times MAX,
  `used_shards` OR, `parent` SET, `parent_version` delta) and
  `EFS_TXN_REDUCE_ADD` (u64 +1); `fold_reduce` in `txn.c` dispatches on
  the DATA key's kind (LANE / INODE / DSEQ) and bumps the folded key's
  version so a stale EXCL lands STALE. Soundness rules: a pending reduce
  and an EXCL/GUARD on one key are mutually BUSY (`reduces_pending`);
  the log path probes before an unversioned DEL (`dir_txn_busy` in
  `efs_meta_apply_rmdir`, `efs_txn_key_busy` in `efs_meta_apply_unlink`)
  and returns BUSY; dseq GUARDs compare the observed **value**
  (`efs_txn_dseq_observe`) — a log-path bump is unversioned but changes
  the value, so `RMDIR`'s guard goes STALE exactly when a child appeared.
  One shared PREPARE decoder `efs_txn_apply_prepare` (server + sim).
  All of `host_hashed_create_txn`, mkdir, rmdir, `host_unlink_txn`, link,
  `rename_at` converted (`host_prep_ino_delta` / `host_prep_dseq_bump` /
  `host_prep_lane_stamp`); the simulator's `dseq_prep`/`dseq_guard` too.
  Client retries STALE for UNLINK/LINK/RENAME_AT as well as CREATE
  (`stale_retryable`). Unit gates: `test_txn` +4
  (`test_ino_delta_commutes` is the Sep 20 lost update: log-path PUT
  between PREPARE and RESOLVE, fold lands on the log-path result, ver+1;
  vs-EXCL/GUARD BUSY both ways; lane 56-byte tail kept; wire decode) and
  `test_meta_apply::test_log_delete_busy_under_intent`; `test_sim`,
  `test_kv`, `test_raft` green. Deployed stop-all/start-all (new
  `tests/roll_efsd.sh --all`) as `b2184a5c7faf-dirty` + all 9 clients
  (`fuse-deploy`). **posix jobs=1 `results/posix/20260921-123904`:
  200/201 + mmap SKIP in 56 s** (unchanged signature). **Repro
  `tests/stress/same_parent_storm.sh` PASS
  `results/stress/same-parent-20260921-124141/`:** 9 hosts × 4 procs ×
  100 rounds of mkdir/create/rmdir/unlink in ONE parent = 14 400 ops,
  0 errors, parent ends `children=0 nlink=2`, `rmdir` OK. Measured while
  there: **~178 ms per op per proc under 36-way same-parent contention
  (~200 ops/s aggregate)** vs 7 ms idle — the same ceiling as
  mdtest-easy-write 0.238 kIOPS. Not a correctness item; it goes in the
  W6 residuals as "same-directory op rate" (BUSY/STALE retries are not
  logged at default verbosity, so first instrument, then decide).
  **Step 5 (9×4 IO-500 debug) PASS on this build:
  `results/io500/20260921-debug-9x4-reduce/`** — both `-R` reads 0
  errors, **0 `Unable to remove directory`**, the run tree is gone
  afterwards (the previous run left 3 undeletable dirs). Rates within
  noise of the previous build (mdtest-easy-write 0.189 vs 0.238 kIOPS,
  hard-write 0.240 vs 0.201, ior-easy-read 1.13, hard-read 4.06 GiB/s;
  NOTE.txt has the table). Step 6 = this commit.
  `efs-bg.sh` now runs jobs in a detached **GNU screen `efs-<name>`** on
  node9901 (user's suggestion): `screen -r efs-<name>` there shows the
  live job; log/rc bookkeeping unchanged.
- **23:10 (Sep 20)** — **Step D (9×4 IO-500 debug) DONE and correct:**
  `results/io500/20260921-debug-9x4-outbox/` (run id 2026.09.20-22.45.42,
  cluster on the outbox-fix build). Both `-R` reads **0 errors**, every
  ior file unlinked. vs the morning gate (`20260920-debug-9x4`):
  mdtest-easy-write **0.238 kIOPS (was 0.050, 4.7×)**, mdtest-hard-write
  0.201 (was 0.018, 11×), mdtest-easy-stat 0.88 (0.24), mdtest-hard-stat
  2.76 (0.79), mdtest-easy-delete 0.29, mdtest-hard-delete 0.61;
  ior-easy-write 0.776 GiB/s (0.814, unchanged — W4 wall), ior-hard-write
  0.046 (0.044, unchanged — 36-way sub-chunk CAS), ior-easy-read 1.16
  (1.80; 25 s of the 37 s is the 1 GiB `open`, see residuals),
  ior-hard-read 3.89 (3.55).
  **New correctness finding — wedged directory, needs a user decision
  (§4):** 7 mdtest `WARNING: Unable to remove directory
  …/mdtest-easy/test-dir.0-0/mdtest_tree.N.0`. Afterwards 4 of them rmdir
  fine (transient STALE), **3 return EIO forever**: `raft-rmdir` →
  `status=3` (ERROR) because `server_raft_host_rmdir` /
  `efs_meta_apply_rmdir` hit `prow.nlink < 3 → EFS_ERR_PROTO`. The parent
  `test-dir.0-0` (ino 824) has **nlink=2 with 3 live subdirectories**
  (true value 5): 36 ranks did `mkdir` then `rmdir` of one child each in
  the same parent; children with an even ino go the same-group **log
  path** (`efs_meta_apply_rmdir`: PUT of the parent row, no intent probe,
  no version bump) and odd-ino children go the **txn path** (EXCL on the
  parent row at `pver`, PUT of a full row image with `nlink-1` from its
  read snapshot). A txn that read the row before a log-path apply still
  PREPAREs at the old version, wins, and overwrites the log-path
  decrement/increment — the exact class `alloc_key_claim` fixed for the
  ALLOC key on Sep 20 (`meta_apply.c` comment above it), now on the parent
  inode row. Every log-path parent-row PUT is exposed: `create_file_batch`,
  `mkdir_batch`, `efs_meta_apply_unlink/link/rename/rmdir`. Consequence:
  parent `nlink` drifts low → the last children can never be rmdir'ed
  (EIO), `rm -rf` of an mdtest tree fails; drifts high → a directory
  claims children it does not have. This is the "transient mdtest rmdir
  ENOTEMPTY" residual — it is not transient.
  Leftovers on 19810: `/io500/2026.09.20-22.45.42/mdtest-easy/test-dir.0-0/
  mdtest_tree.{3,18,27}.0` (parent 824 nlink=2). Ignore or wipe.
  Options for the user: **(a)** generalize `alloc_key_claim` to the parent
  row: log-path PUT is BUSY under a pending intent and bumps the row's
  version so the txn's EXCL goes STALE — same rule as ALLOC, mechanical,
  plus the client must retry STALE for MKDIR/RMDIR/UNLINK/LINK/RENAME (today
  only `INODE_CREATE`, `inode_rpc.c:299`), and the 50 ms × 2ⁿ BUSY backoff
  becomes the same-parent latency; **(b)** the spec'd §7.2 end state:
  parent nlink / dseq / mtime as commutative REDUCE parts (the existing
  `efs_txn_reduce` carries only max_end/max_mtime/max_ctime; needs a
  signed nlink delta and a REDUCE resolve onto an inode row), which makes
  same-parent ops conflict-free instead of retried. Not started either.
- **22:45 (Sep 20)** — **The 100 ms metadata floor is gone: mkdir med
  103 → 7.2 ms, create+1B+close 60–107 → 6.7, append+close 160–180 → 9.0**
  (`results/perf/20260921-md-latency.txt`). Step C below is DONE; the
  reaper was only half of it. The other half was a wakeup bug in the
  raft_host outbox: ONE shared `outbox_cv` for all per-peer sender threads
  + `pthread_cond_signal`, so queuing a message for peer A usually woke
  peer B's sender (empty queue, back to sleep) and A's message left at the
  next signal for anyone — the next 50 ms heartbeat. Each AE and each AE
  reply lost 0–50 ms per hop → ~100 ms per commit while every fsync was
  0.3 ms. Found by strace on a FOLLOWER (003): it acked only every other
  heartbeat, two 85-byte `writev` back to back; on the leader the AE to
  one peer left 47 ms after the propose. Fix: per-peer `tx[].cv`
  (`struct host_outbox`), `host_send` signals exactly that one, shutdown
  broadcasts all. Rolled all four (`roll3`, 90 s, TCP peers,
  `EFSD_ENV=EFS_TRANSPORT=tcp`); posix jobs=1 running as `posix3`. Not
  committed yet (step A below now includes this).
- **21:55 (Sep 20)** — Steps 1–4 DONE again on the full in-flight tree
  (now also the `concurrent_appends` dcache fixes, the dir-rename dual-host
  bounce, and a reaper fix found tonight); step 5 (9×4 IO-500) NOT run;
  nothing committed yet. Everything long-running now runs **from node9901
  via `efs-bg.sh`** (see the `efs-test-ssh` skill; logs in `~/efs-runs/`)
  because the login-node shell died four times today — the fifth time was
  mid-doc-edit at 21:50, which is why this entry exists.
  - Posix jobs=1 twice: `results/posix/20260921-011518` (190 + mmap SKIP,
    but 8 clients were being rebuilt/remounted during it) and
    `results/posix/20260921-012432` (**191 both-pass + mmap SKIP**, fails
    = `dir_deep_nesting`, `dir_deep_nesting_beyond_64`, `names_crazy_dirs`
    15 s walks + `mtime_monotonic_many_writes` 15 s). `concurrent_appends`,
    `dir_rename_dir_with_contents`, `trunc_zero_then_high_pwrite` PASS.
    `mtime_monotonic_many_writes` is not a flake: 80 × (open O_APPEND,
    write 1 B, close) at **~200 ms each** > 15 s. Isolated timing on 007
    (idle cluster): mkdir med **103 ms** (min 5.5), create+1B+close 60–107,
    append+close 160–180, `report_ms=104` on every 1-rec REPORT,
    server `report-split ... finish_ms=103`.
  - **Root cause of the ~100 ms floor (fixed in tree, deployed, gate
    pending):** the reaper. `tests/tools/raft_log_tail.py` on 004's
    `raft.log` showed **92 % of both groups' entries were `LANE_SWEEP`**
    for the same 64 inodes, 362× each — ~33 entries/s per group with no
    client. `host_gc_propose` was leader-only, but a lane's shard is
    `ish + lane × odd stride`, so every ODD lane of an inode lives in the
    OTHER group: the anchor-group leader swept lane 0 (landed), got
    `NOT_PRIMARY` on lane 1, and retried the whole marker next second,
    forever — unless one node happened to lead both groups. Fix:
    `host_gc_propose` forwards a foreign-group command to that group's
    leader (`host_remote_cmd`, same path as a client op) and waits for the
    local replica if hosted. No unit test covers `raft_host`; the gate is
    the log histogram going quiet + the latency numbers above dropping.
  - Rolled all four (`tests/roll_efsd.sh 1 2 3 4` from node9901, 01:48–
    01:50 UTC, each caught up in < 10 s, all `d0fd0448adb6-dirty`). After
    the roll `REAP_DONE` finally lands (0.3 % → 40 % of entries): the
    reaper is **draining a backlog of dead inodes at ~15/s per group**
    (thousands from IO-500/mdtest/posix). Until it is idle every client
    commit queues behind it (mkdir med still 150 ms at 21:55). **Wait for
    both groups' `commit` to be flat for 30 s before measuring anything**,
    then redo the isolated latency probe — the min of 5.5 ms says what a
    raft commit costs; if the median stays > 20 ms on an idle cluster that
    is the next serialization point (each apply fsyncs the KV WAL inside
    the single pump thread, `raft_host.c:4245 EFS_KV_LSM_SYNC`) and needs
    a user decision, not a tweak.
  - Rolling restarts cause a **group-2 election storm** (term 199 → 264)
    lasting ~5 min after the last restart: the server peer pool is RDMA
    (`peer_pool.c` upgrades every peer conn; raft AE rides it) and a
    restarted node's QPs die with `transport retry counter exceeded` /
    `*** SOCKET CLOSED/REUSED BEHIND THIS CONN ***` on its peers, each
    blocking a sender for seconds → missed heartbeats. Pre-existing (same
    lines in every `efsd.log.prev`), converges by itself. Same class as
    the client pool identity bug (`test_conn_fd`) — not chased tonight.
  - New tooling, all committed with this batch: `efs-bg.sh` (detached
    runner on node9901), `tests/deploy_fuse_clients.sh` +
    `tests/fuse_client_remount.sh` (parallel client rebuild+remount, one
    status line per host), `tests/roll_efsd.sh` (rolling restart with
    build-ID check and per-group catch-up wait), `tests/tools/raft_log_tail.py`
    (what is the log made of). The deploy rule's kill command is now
    `pkill -9 -x efsd` only.
- **12:05** — Steps 1–4 DONE, step 5 not started; the agent's shell died
  again while probing the one new posix failure. Results:
  - Churn (step 1) = the GC reaper draining the fragment backlog the
    `lane-sweep rc=-5` bug left (no client process; `df` on every
    `/data1/0N` falls ~150 KB/s). Benign; let it run.
  - Rolling restart (step 2) done 11:33–11:45 in the order 004, 005, 006,
    003; each caught up within 9 s; all four run `d0fd0448adb6-dirty`.
  - **Step 3 PASSED:** raw IOR hard 36×3000 write **96.78 MiB/s** (was
    ~45), cold `-r -R` verify **0 errors**, `/tmp/hardcheck.py` from
    fcstor009 **bad records 0 of 108000** (was 6707). W1 `n1_shared_pwrite`
    **lost=0**. All in `results/io500/20260920-hardv-pubbatch/gate.txt`.
  - Step 4 posix jobs=1 `results/posix/20260920-155107`: **192 both-pass +
    mmap SKIP**, 3 EFS fails. Isolated re-run: `dir_deep_nesting` PASS,
    `mtime_monotonic_many_writes` PASS (load flakes, known), but
    **`dir_rename_dir_with_contents` FAILS ISOLATED** (deterministic
    `EIO` on `os.rename(d/src, d/dst)` where `src/sub/f` exists; 1.6 s).
    It was PASS in the Sep 17 `results/posix/20260917-190719` gate, so
    this is a regression in the in-flight tree or a state issue on this
    populated cluster. `ino_dup=0`, `lane-sweep rc=-5`=0 on all four
    servers. **Do this before step 5:** reproduce by hand on fcstor007
    (`mkdir -p $d/src/sub; echo x >$d/src/sub/f; mv $d/src $d/dst`), vary
    it (empty dir; dir with a file only; dir with an empty subdir) to see
    which shape EIOs, and read the RENAME path in
    `src/server/raft_host.c` (`server_raft_host_rename_at`, LOCAL same-dir
    directory rename GUARDs dest ancestry `parent_version`) plus the
    efsd.log of the parent's group leader for the txn status. Candidate:
    the `alloc_key_claim` version bump or `dentry_seq` GUARD racing the
    child's `nents` update — the test creates `sub` and `f` immediately
    before the rename.

**State as of Sep 20 10:40 (verified by probe, not memory; the agent's
shell died mid-rolling-restart, which is why this block exists):**

- **The bug being fixed: a REPORT that spans several Raft entries returned
  OK when only its LAST entry applied OK.** `host_pub_batch_wait`
  (`src/server/raft_host.c`) read the apply-verdict ring for `last_idx`
  only; a report larger than `HOST_PUB_BATCH_N` (256) pubs is proposed as
  several entries, and a pub that loses its N-1 CAS at apply time is
  `STALE` on the ring for *its* entry only. The client took the OK, marked
  those dirty ranges clean, and the CAS winner's generation (merged on an
  older base) became the file. Measured: raw IOR hard, 36 ranks × 3000
  segments of 47008 B (`SLOTS=4 bash tests/perf/io500/run.sh ior-hard-write
  3000`, then cold-remount `ior-hard-verify 3000`) lost **6707 of 108000
  records**, every one a client's whole sub-range of a record straddling a
  128 KiB chunk boundary, zeros in a cold verify (classifier:
  `/tmp/hardcheck.py <file> 36` on fcstor007 — recreate from the comment
  in that file if it is gone). The 9×4 debug run
  `results/io500/20260920-debug-9x4/` saw the same thing as 2 `-R` errors
  on ior-hard-read (easy-read 0). This is a **correctness** bug and blocks
  every other W6 item.
- Uncommitted code in the working tree (on top of `cc828d8` + the Sep 20
  docs/rules commit), all unit-gated on fcstor003 (`test_raft` /
  `test_meta_apply` / `test_kv_lsm` OK):
  - `src/server/raft_host.c` — `struct host_pub_batch` keeps every
    proposed index (`idxs[]`, `overflow`); `host_pub_batch_wait` waits for
    the last index then reads **every** entry's verdict; any `STALE`, any
    ring miss (`obs_arc_miss`), or `overflow` → the report is `STALE`
    (client re-pulls the map and replays only chunks whose generation
    moved). `host_pub_batch_reset` frees both arrays. No unit test covers
    `raft_host` (live-only); the gate is step 3 below.
  - `src/meta/meta_apply.c` — `alloc_key_claim` (log-path CREATE/MKDIR
    alloc is `BUSY` under a txn `EXCL` intent on the shard ALLOC key and
    bumps its version; closes the `ino_dup` 4746 race; gate
    `test_alloc_vs_txn_intent`); `sweep_cb` / `rsv_purge_cb` initialise
    `val=NULL vlen=0` on `EFS_KV_DEL` items (uninitialised `vlen` made
    `wal_encode` return `INVAL` → reaper `apply lane-sweep rc=-5` forever,
    124 inos re-swept ~9 entries/s).
  - `src/kv/kv_wal.c` — `wal_encode` validates `vlen` only for `PUT`;
    regression in `tests/test_kv_lsm.c` (DEL with garbage `val/vlen`).
  - `src/client/read.c`, `src/client/ops.c`, `src/client/client_internal.h`
    — a read whose layout pull fails **fails** instead of zero-filling;
    `pull_chunks_range` returns the first RPC error; `pull_layout_miss` has
    a 200 ms range cache, not a 1/s rate limit. Built into `efs-fuse` on
    fcstor007–015 (all nine were remounted from this tree before the last
    IO-500 run).
- **Servers:** `efsd` built from this tree at `/tmp/efs` on fcstor003–006
  (`d0fd0448adb6`-dirty, 4704656 B, 10:29). **fcstor004 (node 2) was
  restarted on it at ~10:33 and caught up** (its `applied` equalled the
  leaders' `commit` on both groups at every 3 s poll for 2 min). The HELLO
  gate accepted it, so the build ID is unchanged and a **rolling** restart
  is fine. **fcstor003 (node 1, g0 leader), fcstor005 (node 3), fcstor006
  (node 4, g2 leader) still run the PREVIOUS process** (started ~09:20,
  without the `host_pub_batch` fix). Nothing was wiped; no `--join`.
- **Unexplained, check first:** with no test running, both Raft groups
  were advancing ~33 entries/s (g0 commit 3259035 → 3333442 in ~7 min).
  Candidates: a leftover IOR/posix process on a client, or the reaper on
  the old-binary leaders. Do not restart anything until you know which.

**Steps (as of 22:45 Sep 20 — steps 1–4, B and C are DONE; what is left):**

- **A. DONE — commit `30c41ee`** (W6 correctness batch + the 100 ms
  floor). posix jobs=1 after the outbox fix: **200/201 both-pass + mmap
  SKIP in 60 s** (`results/posix/20260921-024204`; the four 15 s walk
  timeouts are gone). **The cluster still runs `d0fd0448adb6-dirty`**
  (byte-identical source to `30c41ee`, built before the commit); the next
  server restart must be stop-all-four / start-all-four, `roll_efsd.sh`
  will refuse. Clients likewise run the dirty build — fine until the next
  `deploy_fuse_clients.sh`.
- **B. DONE** — reaper drained, both groups flat; `raft_log_tail.py` on 004
  no longer dominated by `LANE_SWEEP`. Keep the check as a habit before
  any measurement.
- **C. DONE** — `results/perf/20260921-md-latency.txt`: medians 6–9 ms.
  The pump's per-apply KV WAL fsync is NOT a serialization point at this
  load (0.3 ms per fsync on NVMe); no `sync_mode` question for the user.
- **D. DONE** — `results/io500/20260921-debug-9x4-outbox/`, 0 read errors,
  all unlinks OK, mdtest 4.7–11× (see the 23:10 entry). Then step 6.
- **E. DONE (user chose (b), 08:45 Sep 21 entry)** — §7.2 reductions on
  the parent row / dseq / dir lane; repro is now a script,
  `tests/stress/same_parent_storm.sh` (PASS, 14 400 ops, parent clean).
  Any directory whose `stat` nlink ≠ 2 + subdir count after this build is
  a NEW bug, not this one. Step 5 (9×4 IO-500) PASS on it
  (`results/io500/20260921-debug-9x4-reduce/`); step 6 = the §7.2 commit
  (`git log -1 --grep="commutative reductions"`). **Nothing is in flight after that commit** — take the lowest
  open item in §1a (W6 residuals first). The cluster and the 9 clients run
  `b2184a5c7faf-dirty` = the same source as that commit; the next server
  restart is a build-ID change → `tests/roll_efsd.sh --all`.

Original steps (1–4 done twice, kept for the commands):

1. Pre-flight (deploy rule). Then the churn check: on fcstor007–015
   `pgrep -x io500; pgrep -x ior; pgrep -f posix_suite` (kill leftovers
   with `pkill -9 -x`, never `-f` on an ssh command line); on fcstor003 and
   fcstor006 `tail -c 400000 /tmp/efs/efsd.log | grep -oE 'raft-host: [a-z-]+ [a-z-]+' | sort | uniq -c | sort -rn | head`.
   If it is the reaper (`lane-sweep`), the restart below stops it; if it is
   a client, kill it and re-check `commit` is flat for 30 s.
2. Rolling restart: `efs-bg.sh start roll 'bash tests/roll_efsd.sh 1 2 3 4'`
   from the login node (runs on node9901; builds on each node, `pkill -9
   -x efsd`, starts, waits for per-group catch-up, refuses on a build-ID
   change). ~2 min for four nodes. Expect a few minutes of group-2
   elections afterwards (RDMA peer conns re-forming) — wait for
   `leader != -1` and a stable `term` on all voters.
3. **The gate for the fix:** on fcstor007
   `SLOTS=4 bash tests/perf/io500/run.sh ior-hard-write 3000`, wait for
   `last-run.log` to finish, remount every client (deploy rule "Restart one
   efs-fuse" — cold verify or it is dcache), then
   `SLOTS=4 bash tests/perf/io500/run.sh ior-hard-verify 3000`. Pass =
   **0** `-R` mismatches; also run `/tmp/hardcheck.py` and expect `bad
   records 0`. Expect hard-write MiB/s to DROP (the STALEs the old code
   swallowed are now retried) — record it, do not tune it here. Also the
   W1 gate: `tests/stress/n1_shared_pwrite.py` `prepare` on 007, then
   `write-a` on 007 ∥ `write-b` on 008, remount 009, `verify` on 009 →
   `lost=0` (shape and expected output: `results/stress/20260918-n1-w1/gate10c.txt`).
4. `EFS_TRANSPORT=tcp POSIX_JOBS=1 bash tests/run_tests.sh posix fcstor007.ib`.
   Gate: ≥ **195/201**, only the known signature (`dir_deep_nesting*` /
   `dir_many_files` / `names_crazy_dirs` 15 s walks, `mmap_write_read`
   SKIP, `concurrent_writes_disjoint` flake); no `ino_dup` in any
   efsd.log; no `apply lane-sweep rc=-5` on any leader after the restart.
5. 9×4 IO-500 debug once (`SLOTS=4 NP=36 bash tests/perf/io500/run.sh
   debug`, detached driver; poll `driver.log`). Gate: every phase
   finishes, ior-easy-read **and** ior-hard-read `-R` errors = 0, every
   mdtest unlink OK. Copy `result.txt` + ini + `driver.log` to
   `results/io500/<id>/`.
6. Commit code + rules + this file with the result directories cited, then
   delete this block (W6 residuals move to the W6 queue item).

---

### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency and
a severity: **correctness items come before performance items**, because a
parallel filesystem that returns wrong bytes has no throughput number worth
reporting. Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

**What the hardware allows.** Every performance item is measured against
this, not against last week's number. Cluster traffic rides `ibs1f0`
(200 Gb/s IPoIB, `ip route get 172.16.223.57` on a client); servers have six
NVMe each.

| ceiling | value | derivation |
| --- | --- | --- |
| one client, logical write | **~16.7 GB/s** | 25 GB/s line rate ÷ 1.5 (2+1 EC sends 3 fragments per 2 data) |
| one client, logical read | ~25 GB/s | line rate; a read fetches k=2 fragments |
| cluster, logical write | **~44–57 GB/s** | 4 hosts × 16.7–21.4 GB/s NVMe (`results/nvme/`) ÷ 1.5 |
| 1-client honest write today | 639–724 MiB/s | **3.8–4.1 %** of the client's ceiling (8 GiB dd+fsync after W3) |
| 4-client honest write today | 251 MiB/s | **0.46 %** of 44 GB/s (8 GiB dd+fsync, 4 own files; 0.41× one client) |
| 9-client honest write today | 202 MiB/s | **0.37 %** of 44 GB/s (8 GiB dd+fsync, 5 first-write clients; 0.32× one client) |
| 1-client honest read today | 4102 MiB/s | ~16 % of the client's ceiling (sr-1m, W4 morning) |

[architecture.md §1](../architecture.md) says: if a benchmark stops at a
mutex, one leader, one thread, FUSE serialization, one WAL or one
coordinator before a physical resource, *that is by definition an EFS bug*.
By that rule the write path is currently a bug, not a tuning task, and
"done" for W3–W5 is defined by the table above, not by beating the previous
run.

Baselines, all honest (flush in the clock, reads after remount, `findmnt`
verified `fuse.efs-fuse`):

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync` | **639 MiB/s** (13.5 s; best 724 / 11.86 s) | `results/perf/20260918-w3-split/gate.txt` |
| 1-client honest fio 9×2g sw-1m | **758 MiB/s** | `results/perf/20260918-w1-honest/` |
| 1-client honest fio 1-job 50g | **341 MiB/s** | `results/perf/20260918-w4-honest/` (W1 was 373; loaded reruns EIO on fsync) |
| prior 1-client 9×2g sw-1m | 924 MiB/s | `hot-sw-1m-9job-fcstor007.txt` |
| first honest matrix, 1 client | sw-1m 209 · … · sr-1m 3141 | `results/perf/20260917-honest/` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| IO-500 9×4 debug | easy-write **0.814 GiB/s**, hard-write 0.044, reads 0 errors | `results/io500/20260920-debug-9x4/` (W6) |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

---

#### W1 — Shared-file (N-1) writes from two clients silently lose data — DONE

**Done Sep 18 2026** (working tree on leftover-1 19810, TCP). I12 CAS is
live: `efs_chunk_rec.base_gen` + `chunk_generation` on the wire, leader
checks the writer's base before propose, apply STALE is audible
(`EFS_INODE_RPC_STALE=11` / `EFS_ERR_STALE=-14`) and does **not** stall
`last_applied`, client refetch+overlay+PUT retries. Fragments are
`{ci}.{fi}.{gen}`; GET uses `fragment_path_at` and mints the export shell.
Report identity comes from the last PUT (`putid`), not a GETCHUNKS stub.

`peer_shared_pwrite` is concurrent (`("a", a0), ("ab", (a, b)), ("a", a2)`).
STALE bound is **64** with 2–20 ms backoff — 8 loses to 16 fsyncs/chunk.
`EFS_CHUNK_BASE_UNCOND` (`UINT64_MAX`) is only for a full-chunk overwrite.

- **Gate:** `results/stress/20260918-n1-w1/` — n1-w1-gate10c
  `lost=0 decode_eio=0`; isolated `peer_shared_pwrite` **5/5**;
  `test_meta_apply` / `test_wire` OK (`test_publish_stale_then_retry`).
  Suite 2 one pair `results/posix2/20260918-122419/` **60/3** (was 58/4).
  Suite 1 jobs=1 `results/posix/20260918-123250/` 186/10 — every new FAIL
  isolated PASS except known `concurrent_appends`. Honest 1-client
  `results/perf/20260918-w1-honest/`: sw-1m **758** (not a regression vs
  209); sw-50g **373** (was NET). `FIO_ONLY=1` skips the 4/9 sweeps.
- **Forbidden to reopen:** a distributed chunk lock; per-record report
  status arrays; sending `UINT64_MAX` from any path that read a base;
  wipe / `raft-mkfs` / inventing chunked SNAP.

#### W2 — `write()` is specified as durable-and-visible; the code buffers — DONE

**Done Sep 18 2026**, option (i): the spec moved. [architecture.md §3](../architecture.md)
now lists three deviations; a returned `write()` is client-buffered;
durable + cross-client visible at `fsync` / last `close` / `O_SYNC`.
`O_SYNC`/`O_DSYNC`/`-o sync` is specified write-through and is **not
wired**. Do not implement publish-on-every-`write()` — that is the
rejected 10× throughput change.

Measured (`results/stress/20260918-w2/`): peer sees **0/10** of an
un-`fsync`ed 4 KiB `pwrite`; `kill -9` of `efs-fuse` loses 64 MiB of an
acknowledged `write()` (file exists, size=0).

- **Forbidden:** implementing option (ii) publish-on-write; editing §3
  back to "`write()` is durable"; wiring `O_SYNC` as a silent side-cut
  of a later item.

#### W3 — Split the single-client fsync tail, then remove the larger half — DONE

**Done Sep 18 2026** (leftover-1 19810, TCP). Measure said REPORT owned
73 % of the 8 GiB `fsync` (flush 5.7 s / report 15.0 s / wall 25.8 s).
Cuts, in order: `HOST_PUB_BATCH_N=2048` + skip `get_chunk` when
`base_gen==0`; raft-log `sync_hold` across the report's proposes;
client flush pipeline (`dcache_steal` + put pool); PUBLISH follower
forward is `host_propose` not `host_propose_wait`; O_APPEND fetches a
published mapping before sparse RMW; truncate/symlink report only that
ino; FUSE `flush` waits `report_dirty_ino(ino, 1)` (W2 close is
durable — kick-only left `i_size` 0 so the next O_APPEND wiped the
prefix). N=4096 did not win. AE 1 MiB is ~5168 pubs; deeper batching
is exhausted.

- **Gate:** `results/perf/20260918-w3-split/gate.txt`. 8 GiB
  `dd+fsync` **13.501 s / 639 MB/s** (best cut 11.862 s / 724 MB/s);
  remount `HEAD_OK` `TAIL_OK`. **3.8–4.1 %** of 16.7 GB/s (was 1.9 %
  at 317 MiB/s). posix jobs=1 `results/posix/20260918-w3f/` **195/201**
  (190 both-pass; W1 was 186/10). posix2 one pair
  `results/posix2/20260918-w3f/` **58/63** (W1 60/3). Remaining
  suite fails are load / known O_APPEND atomicity / 15 s walks, all
  isolated PASS except `concurrent_appends`. Remaining tail is REPORT
  pack+push (~6 s).
- **Forbidden to reopen:** a REPORT split into multiple RPCs;
  weakening `fsync`; async flush to inflate the number; raising
  `EFS_IO_TIMEOUT_MS`.

#### W4 — 4-client and 9-client honest fio and dd — DONE

**Done Sep 18 2026.** Writes share a ceiling and more clients make it
worse. Gate: `results/perf/20260918-w4-honest4/gate.txt`.

- 1-client morning matrix `results/perf/20260918-w4-honest/`: sw-1m
  **694**, sr-1m **4102**, sw-50g **341** (W5). All `FUSE_OK` `err=0`.
- 4-client fio (same dir, morning): sw-1m AGG **1589** (2.3×), ow-1m
  636, rw-1m 343. After bounce/grown table: sw-1m 281, ow-1m 240, then
  rw-1m **400 s ssh TIMEOUT** on all 4 (REPORT tail). Do not raise it.
  4/9-client fio reads and 9-client fio writes were not finished.
- 8 GiB `dd+fsync` own file, remount HEAD/TAIL `0x5a` OK: 4-client
  **251 MiB/s** (32 GiB / 130.5 s = 0.41× one client, 0.46 % of 44 GB/s);
  9-client first-write 011–015 **202 MiB/s** (40 GiB / 203 s = 0.32× one
  client). Do not quote 007–010's 13–31 s 9-client walls — those files
  kept the 4-client mtime.
- A 4-client fio storm can lose raft heartbeats (`report-split`
  `rc=-15` NOT_PRIMARY). Recovery is keep-storage efsd bounce, not wipe.

W3's leftover (REPORT pack+push) is the multi-client wall. The 1-client
limit was not per-client CPU.

- **Forbidden to reopen:** a REPORT split into multiple RPCs; quoting a
  run where any host failed the FUSE check; one shared file (W1);
  raising `EFS_IO_TIMEOUT_MS`.

#### W5 — Re-measure `sw-50g` after W3 — DONE

**Done Sep 18 2026** as the W4 morning 50g row:
`results/perf/20260918-w4-honest/` sw-50g **341** / sr-50g **2203**,
`FUSE_OK` `err=0`. W1 was 373. Later loaded reruns laid 50 GiB then
`end_fsync` EIO (the same REPORT tail). Completes when the path is
healthy; do not treat EIO as a reason to raise `EFS_IO_TIMEOUT_MS`.

- **Forbidden to reopen:** raising `EFS_IO_TIMEOUT_MS`; splitting REPORT
  without asking.

#### W6 — Run IO-500 (IOR easy, IOR hard, mdtest) — CORRECTNESS DONE, perf residuals open

**Correctness gate met Sep 20 2026** (commit `cc828d8`, servers
`708b350`+, TCP, 9 clients × 4 ranks): 9×4 debug
`results/io500/20260920-debug-9x4/` — **every phase finished, ior-easy-read
and ior-hard-read 0 verification errors, every unlink OK**. The path from
the Sep 18 numbers (hard-write DNF in 2 h 18 m, `-W` 4244 errors, 76108
easy-read errors, 27 undeletable files) to this is in
[../project-history.md](../project-history.md) "W6"; the fixes were: client
STALE retry cost + server partial-commit on STALE (hard-write livelock);
`dcache_image_current` / `snap_seq` ordering / forwarded-cmd reply index
(read coherency, `concurrent_appends`); Raft follower dedupe + leader AE
flow control (005 catch-up); **duplicate ino on concurrent CREATE**
(`alloc_hint_or_next` — apply is the allocator); reaper `lane-sweep`
batch-full misread as error (no file >8 MiB/lane was ever reclaimed).

| phase | Sep 20 9×4 | Sep 18 9×1 |
| --- | --- | --- |
| ior-easy-write | **0.814 GiB/s** | 0.263 |
| ior-hard-write | 0.044 GiB/s (495 s) | 0.025 (9×4: DNF) |
| ior-easy-read | 1.80 GiB/s, 0 errors | 0.60 (`-R` 512 errors) |
| ior-hard-read | 3.55 GiB/s, 0 errors | 1.26 (`-R` 15982 errors) |
| mdtest-easy-write | 0.050 kIOPS | 0.053 |

Not a list submission (stonewall 1 s). Do not quote the Sep 19 easy-read
4.0 GiB/s — it was zero-fill. Do not quote stonewall intra GiB/s.

**Open under this item (performance, not correctness):**

1. **ior-hard-write 45 MiB/s** — 36-way N-1 CAS on 47008 B records
   sharing 128 KiB chunks; every fsync replays the losers. Spec answer is
   [protocols/data.md](protocols/data.md) sub-chunk RMW = generation CAS;
   the small-write envelope ([architecture.md §9](../architecture.md))
   says immutable delta objects are the designed escape, **not built until
   benchmarks demand** — this benchmark demands it, so bring the measured
   number to the user before building anything.
2. **1 GiB open costs 20 s of a 22 s easy-read** — 128 sequential
   GETCHUNKS + a 64-lane stat per open. Spec §8 per-lane range fetch
   ([performance.md](performance.md)) is the fix; not implemented.
3. ~~mdtest `rmdir` ENOTEMPTY/EIO under load~~ — **FIXED Sep 21** (it was
   the parent-row lost update, not transient; §7.2 reductions). **Rate
   measured** `results/measure/20260921-161931-samedir-rate` (ROUNDS=100,
   storm PASS at every level): **138 / 134 / 159 ops/s aggregate at 1 / 9 /
   36 procs** — a flat ceiling, 7.3 → 226 ms/op/proc. `busy_n=0` (the
   50 ms BUSY backoff is not it). Each client's fuse daemon spends the
   wall in RPC recv (`recv_us` ≈ the storm wall, `checkout_us` ~15 ms).
   The group-0 leader's log tail during the storm is the storm itself
   (PREPARE 31 %, CREATE/UNLINK/RMDIR, RESOLVE, LEASE_CLOSE, REAP_DONE);
   GC_ACK is 0.4 %. On-CPU samples are LSM prefix scans inside
   `reduces_pending` / `guards_conflict` / `efs_txn_resolve`. Do not
   change the backoff or the txn protocol; the remaining question (LSM
   scan vs Raft fsync, which cpu-clock cannot separate) is the user's.

Harness (`tests/perf/io500/run.sh`): `SLOTS=4 NP=36 … debug` detaches
`prterun` and logs to `$IO500_DIR/last-run.log` (the ssh timeout used to
kill it); `ior-easy-write|verify <mb>` and `ior-hard-write|verify <segs>`
run IOR directly so a cold verify is possible (the io500 driver deletes
its data at the end of a run).

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check
  applies to every rank.
- **Gate (met):** 9×4 debug with 0 `-R` errors on both reads. For the
  three open sub-items the gate is the number in the table moving with
  the same harness and 0 errors kept.
- **Forbidden:** quoting a rank that fell back to local disk; tuning
  IOR's transfer size (47008 is the point); a chunk lock (W1); `pkill -f`
  (matches the agent). Kill hung `io500` with `pkill -9 -x io500` then
  remount FUSE (D-state `request_wait_answer` ignores SIGKILL until
  `efs-fuse` dies). Never gdb-attach an MPI rank through a timeout'd ssh
  (left a rank T-stopped, job unrecoverable).

#### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation

Suite 1 jobs=1 is **193 both-pass / 3 EFS** (`results/posix/20260918-030811`).
One is `mmap_write_read`, an expected SKIP (`MAP_SHARED` is ENODEV by spec —
a documented deviation, not a bug). The other two are latency, not
correctness, and fail isolated too:

- `concurrent_creates_same_dir` — 160 creates in one directory.
- `mtime_monotonic_many_writes` — 80 × 128 KiB close-publish + `stat`
  (~187 ms per iteration).

Both are per-op metadata round trips. Profile one of them against the same
publish/ReadIndex path W3 touches; they may move for free once W3 lands, so
re-run them after W3 before optimizing anything.

- **Gate:** the two tests pass isolated inside budget, and suite 1 jobs=1
  does not regress below 193.
- **Forbidden:** raising `POSIX_TEST_SEC` or the `@budget(...)` values. A
  timeout is a failure to be removed, not re-labeled.

#### W8 — 9-node POSIX suite 1 hits the harness wall

Nine hosts running suite 1 concurrently all hit the 385 s python cap at
131–144 of 201 (`results/posix/20260917-191430`). Only 5–7 were real
walk/name timeouts; the rest never ran and report `[None]`. So the number is a
harness artifact and cannot be read as 60 bugs. Four-node under load is
186–188 both-pass (`results/posix/20260917-190014`).

Make a 9-way run produce a complete TSV (per-test result even when the run is
cut) so the suite reports what it measured, then re-read the real failures.

- **Forbidden:** reporting `[None]` rows as failures, or as passes.

#### W9 — The client staging table is unbounded

`g_client.export` in `efs-fuse` keeps one row per inode this client has ever
touched and one entry per chunk it has written or pulled, and evicts nothing —
so a client that walks a large namespace holds the whole tree in RAM and the
server-side memory wall reappears per client. This is step 12 part A, the only
part of step 12 that never landed.

The plan, the pin rules that make eviction safe (a report builds its records
out of this table, so evicting a dirty row is data loss), and the gate are in
[../client-cache-design.md](../client-cache-design.md). Two design points in it
are unratified — bring them to the user before implementing.

- **Gate:** the walk-RSS gate in that doc, plus posix 1 + 2 and `leaks`.
- **Forbidden:** evicting a row that is dirty, publishing, a ghost with an open
  fd, or pinned by an in-flight op. Bounding it by dropping records instead of
  refetching them.

#### W10 — RDMA empty-table first `mkdir` — needs a fresh table, so ASK first

The client connection-pool lifecycle fix is in tree and unit-gated
(`test_conn_fd`): a pooled conn pins its socket identity, checkout evicts on
mismatch, destroy refuses to close a recycled fd. The **live** repro was never
re-run, because it only reproduces on a freshly `mkfs`'d / effectively empty
table, and 19810 is populated. A remount there is *not* this gate.

Running it means wiping and re-`mkfs`ing a cluster. **Ask before doing that.**
Default transport stays TCP for the items above until it is gated. Note that
every ceiling in the table above assumes RDMA eventually carries the data
path; TCP over IPoIB will not reach it.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping without being asked.

#### W11 — fcstor005 lags because its group's snapshot does not fit — BLOCKED, ASK

`raft_host` only compacts when the whole group's KV export fits in one
`EFS_WIRE_RAFT_MAX_CMD` command. On 19810 one group is over that, so the log
is never compacted, `snap_oversized` latches, and fcstor005 stays far behind
(it still serves; the gossip `DOWN` right after a bounce is the STATUS probe,
not a dead process — check `pgrep -x efsd`).

The fix is a chunked / multi-message InstallSnapshot, and **that protocol is
not specified anywhere**. Do not design it. Bring the measured symptom to the
user and ask for the decision. Until then this is a known, documented lag.

- **Forbidden:** inventing chunked SNAP. Killing 005 to "fix" the lag — a 2+1
  PUT needs every fragment ACK, so removing a node breaks writes.

#### W12 — Repo hygiene

`results/` holds only Raft-engine runs now (the pre-Sep-11 history was
removed). Commit a run directory when it is cited as a gate from this page or
from `.cursor/rules/`; otherwise delete it.

Run `make docs-check` after any doc edit: it regenerates
`architecture-full.md` **and** `architecture.html` from the markdown sources
and validates links plus every `I1..I25` reference. Never edit either
generated file. `make test` must be fully green — there is no
accepted-failure list.

---

**Decisions only the user can make — bring the evidence, do not start.**
These came out of reviewing efs as an HPC parallel filesystem. Each one
changes what efs *is*, so an agent must not pick a side; but each has a
cheap measurement an agent can produce first, named here:

- **Per-file layout control.** Chunk size and the EC profile are per export;
  there is no per-file or per-directory equivalent of Lustre's `lfs
  setstripe`. A 4 KiB-record checkpoint and a 1 GiB-per-rank dump therefore
  get the same 128 KiB geometry, and the declared 32× small-write
  amplification ([architecture.md §9](../architecture.md)) has no opt-out.
  Evidence to bring: W6's IOR-hard/IOR-easy ratio and rw-4k from W4.
- **An interface beyond FUSE.** FUSE is the only client. libfuse 3.10.2 cannot
  negotiate `FUSE_MAX_PAGES`, so every request is ≤128 KiB regardless of
  `max_write`; it cannot emit `FOPEN_PARALLEL_DIRECT_WRITES`; and Linux takes
  the inode lock exclusively for extending direct writes and the parent
  directory lock for `O_CREAT`, **per mount** (already in
  [architecture.md §9](../architecture.md) as an open kernel-interface item).
  So 64 ranks on one node writing one file serialize in the kernel before efs
  is called. Every production PFS has a kernel client, a user-space library,
  or an MPI-IO ADIO driver. Evidence to bring: W4's per-client scaling and a
  1-node 8-rank IOR-easy vs 8-node 1-rank IOR-easy comparison from W6.
- **Whether `write()` is durable** — W2 closed: spec says `fsync`/`close`/
  `O_SYNC`. Do not reopen as publish-on-write. `O_SYNC` wiring is a later
  item, not a silent side-cut.
- Already listed before this review: C1 relaxed coherence; a pressure-triggered
  directory-spread bound (unspecified); cutover of a 36T `efs-test`; any new
  REPORT or SNAP wire shape.

**Bigger than this queue.** [product-gaps.md](../product-gaps.md) inventories
what is missing before efs is a filesystem anyone could run — including the
things that contradict a guarantee the spec already makes (no fragment
repair, no protection-debt tracking, no session/fencing on the client, and
W1/W2 above). Those are not queue items beyond what W1–W2 say; each needs a
design decision first. Do not start one without asking, and do not treat the
queue above as the whole distance to a product.

Everything the §10 steps delivered (10.5c-1..35d, step 11's deletion of the old
engine, step 12 parts A–D) is landed and gated; the per-increment narrative is
in the commit history and in `.cursor/rules/efs-project-state.mdc`, not here.

**When the queue above is empty,** the next task comes from a measurement, not
from this page: run the gates in [testing.md](../testing.md), and take the
largest gap between what a gate reports and what the ceiling table at the top
of §1a says the hardware allows. If closing it needs a design decision the
spec does not contain, stop and ask (§4).

---

## 2. Routing: "I am changing X"

Read the row's **Read** column and nothing else first. The **Governs** column
is what your change must not break; the **Gate** column is what proves it.

| You are changing | Read | Governs | Gate |
| --- | --- | --- | --- |
| Any wire message | [architecture.md §7](../architecture.md) op matrix, `include/efs/protocol.h` | build-ID compat; restart all servers together | `make test`, solo posix |
| Path lookup / dentries | [protocols/directory.md](protocols/directory.md) | I5–I8 | posix, posix2 |
| create / unlink / rename / link | [protocols/directory.md](protocols/directory.md), [protocols/transactions.md](protocols/transactions.md) | I5–I9, I16, I17 | posix, posix2, posixstress |
| Anything about file size, mtime, ctime | [protocols/data.md](protocols/data.md) "lanes"/"times" | I21, I22 | posix (size-visibility tests), posix2 |
| Chunk write / publish / truncate / append | [protocols/data.md](protocols/data.md) | I11–I15, I20–I22, I24, I25 | posix, posixpersist, fio honest matrix |
| The read path, or read prefetch/caching | [protocols/data.md](protocols/data.md) "validated collect" | I24, I13 | posix, posix2 (cross-client visibility) |
| Client reconnect, leases, locks, open-unlinked | [protocols/sessions.md](protocols/sessions.md) | I19, I23, I16 | posix2, posixstress |
| Cross-shard anything | [protocols/transactions.md](protocols/transactions.md) | I16, I17, I9 | posix2, posixstress |
| Raft, KV, replication, membership | [architecture.md §7.1/§7.8](../architecture.md), [failure-tolerance.md](failure-tolerance.md) | I1–I4, I10, I18 | `tests/test_sim`, leaks |
| Production Raft host (`EFS_MD_RAFT`) | `src/server/raft_host.c`, [architecture.md §10](../architecture.md) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied; SNAP blob is the existing WAL item payload | `tests/test_kv_lsm`, `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
| Simulator / applied KV SM | [verification.md](verification.md), `include/efs/sim.h`, `include/efs/meta_apply.h`, `include/efs/raft.h` | I1–I4, I9, I10, I13–I16, I20–I23, I25 | `tests/test_sim`, `tests/test_meta_apply`, `tests/test_raft` |
| Op-ID / idempotency window | [architecture.md §7.9](../architecture.md), `include/efs/opid.h` | I16 | `tests/test_sim` |
| A hot path, for speed | [performance.md](performance.md) | P1–P4, §8 contract | fio honest matrix — **never** the stock `perf` write column |
| FUSE client behavior | [architecture.md §7.7](../architecture.md) | I24, kernel-cache rules | posix, posix2 |
| Module structure / file layout | [development.md](development.md) | ~1000-line file cap; header-only deps | `make test` + the suite for whatever moved |
| The spec itself | [development.md](development.md) "machine gate" | one home per normative table | regenerate `architecture-full.md`; links + `I1..I25` resolve |

Invariant texts live in [architecture.md](../architecture.md) §4. Where state
lives and which shards an operation touches live in §5 and §6 — those two
tables are the single source of truth; satellites explain them and never
restate them.

---

## 3. Done means

A change is finished when all of these hold. Do not stop early and do not
substitute one for another.

1. **It builds on a node** — never in the NFS home
   (see the fcstor deploy rule; a local `make` produces AVX-512 objects that
   SIGILL on the AMD test nodes).
2. **Unit tests pass:** `make test`, plus `test_meta_v6`, `test_dir_stats`,
   `test_ino_path`, `test_meta_slot` where metadata is involved.
3. **The gate from your routing row passes**, run with
   `tests/run_tests.sh <suite>`, and the result directory is recorded.
4. **A failure is a failure.** A timeout is not a skip; an empty TSV is not a
   pass; a suite that ran against a dead mount (`findmnt` not
   `fuse.efs-fuse`) did not run at all.
5. **The measurement is honest.** If you claim a speedup, it came from the
   documented method in [performance.md](performance.md), not from a cache.

---

## 4. Never, without asking

- Invent a design decision the spec does not contain (see §1).
- Restate a normative table in a satellite — link to its one home instead.
- Add a component as new monolith code; it lands inside the carved
  boundaries ([development.md](development.md)).
- Weaken an invariant to make a test pass.
- Widen a timeout instead of removing the work that made it slow.

---

## 5. If you are an AI agent — or briefing one

This page exists so that a **less advanced model can produce a correct
change**. That works only if the task arrives pre-chewed: the model's job is
execution inside hard edges, not exploration. The briefer (human or
orchestrator) owns: picking the step (§1), decomposing it until every
instruction is mechanical, naming the exact files to read (§2 — the Read
column and nothing else), and reviewing the diff. The agent owns: staying
inside the named files, and the checklist in §3 — all of it.

A briefing that works, paste-able:

```text
You are making ONE behavior-preserving change to the efs repo.
Read first, in order, and read nothing else:
  docs/arch/START-HERE.md, then only the files your routing row names.
Task: <one step, decomposed until mechanical: what moves, what does
not, what is forbidden>
Hard rules: no logic changes outside the task; no renames; no new
dependencies; no files outside the ones named; if anything seems to
need a design decision the spec does not contain, STOP and report —
do not decide.
Done means: builds on a test node (never in $HOME), make test passes,
the routing row's gate passes via tests/run_tests.sh, and you record
the result directory. A timeout is a failure. Verify the mount with
findmnt before trusting any suite result.
```

Two traps a pasted briefing must name because an outside agent cannot
rediscover them (the other recurring ones are already in §3):

- **EEXIST on a unique, never-used name is a bug, never benign.** Do not
  swallow it as a race and move on.
- **`pgrep -x`, never `pgrep -f`** on this project's processes — the `-f`
  pattern matches your own ssh command line and kills your own session.
