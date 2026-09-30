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
**The order as of Sep 28 2026 is the numbered table under "Decisions —
taken and pending" in §1a** (W17.1, W16.1, W18, W19, W14.4, W15.3, the
residuals, then D1 spans and D2 open windows). Two design decisions are
taken there (D1, D2) and are to be implemented, not re-asked.
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
`--quota 36T --direct-io`, RDMA), clients fcstor007–015 at `/tmp/efs-mount`.
19820 is retired. Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs`
without being asked — several items below run on the existing data. Before
touching the cluster, run the **pre-flight** in the fcstor deploy rule
(`.cursor/rules/efs-fcstor-deploy.mdc`).

### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**. Update or delete
this block when done — an "in flight" block older than the last commit
is a bug in this page.

**Sep 30 08:40Z: the user's 07:17Z ecopy perf dir reviewed; four client bugs behind the 149 `--verify` mismatches fixed and deployed to fcstor007–015 (`results/measure/20260930-080000-ecopy-times-review/SUMMARY.txt`).** (1) 119 `(atime)`: `struct efs_inode_mem` had no `atime_nsec`/`ctime_nsec`, so every stat served from the staged row (the post-rename lookup) had nsec 0 — fields added in the struct padding (192 B unchanged), `efs_export_set_atime` takes nsec. (2) 29 `(mtime)` = close time, three holes in "utimens has nothing to flush": a wb job waiting in `wb_overlap_inflight` was invisible to `efs_wb_ino_pending_locked` (busy_ino set at the pop now); every flusher's clean-before-PUT / mark-after window (`put_win_open/close` per inode, `efs_dcache_flush_ino` waits for them); and the predicate itself — a threshold REPORT consumes the ino mark with an irec-only record while the bytes are still a dirty dcache entry, so `efs_client_ino_is_dirty` is not "has unpublished data" (`efs_utimens_flush_dirty` also checks `efs_dcache_ino_pinned`). Traced with `EFS_DCACHE_TRACE=1` (new `utimens`/`report`/`release` lines). (3) 1 `(size)` 32398 → 131072: `write_chunks_no_replicate` marked the chunk dirty before the size/ino mark, a threshold REPORT shipped a crec with no irec and the server's `sz == 0` fallback rounded to the chunk end; the ino mark and local size grow are in the chunk's locked block now, and the REPORT builder appends an irec for every crec without one. Gate: `size_gate.py` 48 threads × 120 files × 8 sizes, write→futimens(ns)→close→rename, 4 × 5760 files **0 mismatches** same client and from fcstor008 (was 2, then 1–2 per run); `atime_repro.py` 10/10; posix jobs=1 fcstor007 **200/201** 29.1 s (`results/posix/20260930-083309`). Perf (976K samples): memmove 21.9 % (6.4 % the `efs_rdma_send_frame` bounce copy, ~14 % FUSE→dcache copy), blake3 10 %, `xor_into` 7 %, `stage_evict_main` 5.9 % = the `5ea397fd` evictor fix, absent from fstor007's 06:16Z client. Zero-copy RDMA from registered dcache memory is a design ask. Servers unchanged (`44f397b4ca2e-dirty`). fstor007's `/tmp/efs` rebuilt on this tree for `client.sh`. Pre-existing and untouched: `test_raft_store` 4 W22.1 assertions, `check-architecture.py` regen/single-home. The 07:40Z block below still holds (IO-500 hard-write fsync failures, hard-read EIO, d50 ENOTEMPTY).

**Sep 30 07:40Z: gate of the 06:50Z servers (`44f397b4ca2e-dirty`) + the evictor-fix client, nine clients mounted RDMA 07:17Z. Posix passes; IO-500 hard-write fsyncs fail and hard-read aborts with EIO — one open regression, one open one-off.** Posix jobs=1 fcstor007 **200/201** in 16.8 s under the user's ecopy (`results/posix/20260930-072042`) and 7.1 s after it stopped (`20260930-072224`); posix2 fcstor007/008 **63/63** 47.6 s (`results/posix2/20260930-072241`, prior 47.2 s); 9-host (`results/posix/20260930-072501`) eight hosts **200/201** in 18.0–19.1 s, **fcstor012 199/201**: `dir_deep_nesting` `rmdir d50` → ENOTEMPTY (`[Errno 39]`) and still ENOTEMPTY from that client 1–2 min later (`d50-probes-012.txt`), yet the KV copy at 07:28Z shows d50 = ino 56024 `nlink=2 nents=0`, no dentries, no pending intent (`d50-kv-dump.txt`), and `rmdir` from fcstor007 succeeded at once (`d50-rmdir-from-007.txt`). Client-side, gdb on fcstor012 afterwards: a child vector for parent 56024 exists **on the root tab** with `count=0 cap=4` (`d50-client012-childvec.txt`) — `efs_client_unlink`'s local fast path (`ops.c:1303`) calls `efs_export_dir_empty(&g_client.export, ino)`, which reads only the root tab's `child_vecs`; a stale slot there would return ENOTEMPTY without an RPC, and nothing logs it. Not proven either way (the server pre-check `row.nlink > 2` / one-entry readdir reads the raw row, so a pending `REDUCE_INO` from d51's rmdir would also give a transient ENOTEMPTY; the client retries that 20 × 1 ms only, `efs_fuse_rmdir_at`). First occurrence in the results history; first 9-host run with the `5ea397fd` evictor (active during the suite — D18). Next: make the fast path log which tab/slot says non-empty, or drop the local emptiness check (the server is authoritative) — the second is a one-line decision, ask. **IO-500 debug 9×4** `results/io500/20260930-072824-rdma/NOTE.txt`: ior-easy-write **3.148 GiB/s** (09-28: 2.917), mdtest-easy-write **3.696 kIOPS** (3.418), ior-hard-write **0.261** in 133 s (0.291 in 73 s) with **16 `fsync failed`** (09-28: 0), mdtest-hard-write **2.126 kIOPS** (0.757), ior-easy-read **2.976** (3.092), mdtest-easy-stat **3.600** (3.248), **ior-hard-read ABORT**: `read(…, 47008) failed Input/output error` on ranks 22/23 → `MPI_ABORT` 07:32Z; no later phase. Every client's `fuse.log` shows the same two failures on the shared file ino 84857: `report-loop rounds=1 busy=1 ms≈10000 rc=-13` (fsync EBUSY, W17.1's first-BUSY bound) then `rounds=1 stale=1 ms≈65000 rc=-14` (fsync EIO). The unpublished spans are what hard-read hit (I9: fail, do not zero-fill). Servers: terms unchanged, `apply_max` 0, `pump_hold_max` ≤ 10 µs, but **`inbox_drop` 978 / 3102 / 223 on fcstor004/005/006** — `inbox full, dropped frame type=3 group=2 from=3 len=949540` = 950 KB AppendEntries from the group-2 leader dropped at a 256-frame `HOST_INBOX_MAX`; `wait_timeouts` 619 on fcstor004, `apply-sleep` 20–127 ms, `pub_p50` 544 ms on fcstor005 (3 ms at D8). L0 sits at 113–231 files per node and does not drain at idle: ~10 000 `kv-compact` lines per node, 50–270 KB each in 1–2 ms (D10's per-range trigger), while `gc-pass ms=244 frag=244` on both leaders keeps flushing memtables with `GC_ACK` entries. The F2 counter is new since 05:06Z, so the 09-28 run cannot say whether it dropped too; the hard-write fsync failures are the regression against 09-28 (0 warnings, pre-W17.1/D9 build). Nothing changed in code for this. Aborted-run data removed from the mount; two stale `posix-*` dirs (Sep 29) removed; nine clients still mounted; fstor007 client is the user's.

**Sep 30 07:10Z: the rest of the user's perf dir (du 52K, the 1 Hz stall, 5–10 ms big-file stat, 16 fragments/s GC) — `results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`; servers rolled `--all` 06:50Z on `44f397b4`, client fix in tree.** Four findings, three fixed: (1) `du -hs` = 52K for 3.8 TB is 103 symlinks × 512 B — every regular file reports `st_blocks = 0` because `inode_allocated_bytes` counts the client-local present-chunk table, which is empty for any file this mount did not write (D2 adopts the row only). `efs_meta_row` has no allocated/chunk count and W20 forbids size-based `st_blocks`. **D17 (ask):** a per-lane present-chunk stamp reduced at getattr like `max_end`, or accept `st_blocks = 0` for files this client did not write. Not implemented. (2) Every metadata op stalled 14–27 ms exactly once a second (du/find1/find2 straces; absent on the 01:10 fresh mount) — **client side**: `stage_evict_main` wakes every 1 s, the staging estimate (412 MB with 10780 rows) is over `EFS_CLIENT_META_MB` 256 MB, and each of its 64 `evict_one` calls ran `ino_has_chunks` + `drop_chunks_from` + `forget_ino` = three walks over every loaded shard tab (2400 of 4096 after one du), 250 µs each under the table lock, then `efs_export_compact` rebuilt every tab's hash index to the same size (the 25 % load-factor test always passes at 1–3 rows with a 512-slot floor). RPC threads queued on the lock the whole 22 ms (`fuse.strace` on fcstor007: 58 lock hand-offs on `g_client` per wake). Fixed in tree: `efs_export_evict_ino` visits the ≤ 3 tabs the row names plus the chunk-group tabs its size reaches (fan-out only for a missing row, a hard link, or a file spanning most shards; `test_stage_evict`), compact reindexes only when the rebuild shrinks, and slab bytes are booked at `sizeof(struct efs_inode_mem)` (192 B) instead of the 512 B on-disk row size. **Still true (D18, ask):** the estimate is dominated by the per-tab floor — one 256-row slab + 16 × 1232 B chunk entries + 512-slot indexes ≈ 90–170 KB per shard tab, ×4096 tabs > the 256 MB cap before any data is staged, RSS 330 MB with 10780 rows — so the evictor evicts 64 hot rows a second forever and cannot reach the cap. Options: evict whole cold tabs (`shard_tick` exists, unused), a smaller first slab / lazily sized indexes, or apply the cap above the tab floor. Not chosen. (3) `stat` of a 64-lane file was 5–10 ms vs 0.2–0.9 (servers ~2 % CPU): `host_read_inode_lanes` did a ReadIndex per active foreign lane (32 peer round trips on a follower) and every per-lane `efs_txn_reduce_read_ex` prefix scan loaded a 1 MiB readahead window per covering segment (22 × 1 MiB pread per RPC, ×2 for LOOKUP + GETATTR, fcstor004 strace). Rolled: one ReadIndex per group; `kv_seg` iterators read one block unless `kv_seg_iter_set_seq` (compaction, export). After: 4.7 ms per stat (was 9.8), 300 stats 1.65 s. The remaining cost is 64 × (scan + 2 gets) per collect, twice per stat — a fold or a cached collect is a design item. (4) GC reclaimed 16 fragments/s (32-record scan, one 16-ack entry per group per second; 70–86 unlink per server per 5 s = 21 h per 100 GiB file): now 256 records per scan, 128 acks per `GC_ACK` entry (`EFS_META_GC_ACK_MAX`), rescan while full within 200 ms per group per loop. `raft-host: gc-pass ms=` prints when a GC iteration exceeds 5 ms (fcstor005/006 leaders: `recover=70`, of which 64 ms is `HOST_REC_YIELD_US` sleeps). Also: the rsync `getcwd ENOENT` at 01:53 was the invoking shell's cwd (the 02:31 re-run from `/tmp/direct_copy` worked), dd bs=16k is 20–40 µs per FUSE write (known direct_io shape), `readlinkat` p50 0.85 ms. Servers were rolled twice (06:35Z, 06:50Z) under the user's live `client.sh --perf` on fstor007.

**Sep 30 06:17Z: the wedged mount after the user's 100 GiB dd was a pipelined-sender channel mix-up on the fcstor004↔fcstor005 peer lane; fixed `85f5b31c`, rolled `--all` 06:16Z (`85f5b31c…-dirty`, `~/efs-runs/roll126.log`).** `results/measure/20260930-060000-dd-wedge/SUMMARY.txt`. A frame over 72 KiB goes over the conn's TCP side-channel and is answered on TCP; `host_sender` sends up to three messages and collected the replies with `recv_chan` left at the last message's channel, so [AE_REP (RDMA), big AE (TCP)] read TCP only, left the RDMA reply in the ring, timed out at `HOST_SEND_IO_MS`, destroyed the conn, and Raft resent the same shape — forever (fail=2399 in 10 min, hi=2048, both directions). Only the lane between the two dual-group hosts mixes one group's AEs with the other group's replies, and only a big-entry stream (the dd's 2048-publish batches) makes the AE take TCP. The follower each lane fed sat ~700 entries behind; every follower-served read and every dual-group REPORT was BUSY (`apply-sleep` 400 ms); dd close EIO, `ls` EBUSY 16.7 s, sha256sum ENOENT 33 s, unmount `DATA LOSS` after 60 s. Before F1 the same shape waited 30 s per attempt — that was the "lost RAFT_REPLY" behind the 04:06Z election storms (D15/D16 lose their motivating case; still asks). Fix: `efs_conn.last_recv_chan`, and the sender counts expected replies per channel. The L0 gate was not involved (KV dir 0.33 GB). Not changed: the 819200-record close REPORT is one RPC and one BUSY at its end discards 16 s of server work (splitting is an ask); `kv-compact … l0only=1` folds after every flush (L0 ~150 files, 1–5 ms each, noise); `apply lane-fence rc=-2` ×32 during the dd, not chased. **Gate passed (`~/efs-runs/gate130.log`, 06:19Z):** 32 GiB non-zero dd+fsync from fcstor007 (`fuse.efs-fuse`, MOUNT_OK) in 32.85 s ≈ **1000 MiB/s**, `DD_RC=0`, size 34359738368, close REPORT `nrec=262144 pack_ms=2811 push_ms=4186 rc=0` (same 2048-publish batch shape that wedged), zero `exhausted|report-loop|DATA LOSS|conn pool` in `fuse.log`, `tx->N fail=0` on every lane of fcstor004 and fcstor005 before and after, terms unchanged (g0 106, g2 680), both groups commit==applied, `apply-sleep` only 23–107 ms waits one index behind. File removed, fcstor007 unmounted cleanly (rc=0, no `efs-fuse`). Do not run a dd inside an `efs-ssh.sh` call (gate128 died at the 15 s timeout before writing a byte; the dd itself belongs in a screen on the client).

**Sep 30 05:45Z: review of the 05:06Z roll under ecopy; two client fixes committed (`e002771e`) and rolled.** `results/measure/20260930-051000-review2/SUMMARY.txt` is the review. (1) `dcache_reclaim_main` was 39.8 % of efs-fuse cycles: 16 threads re-walked the 64 dirty lists nonstop while dirty_bytes sat over the 2 GiB limit with no `have_base` slot to pop (ecopy writes fresh files). A sweep that pops nothing now parks until the next kick after a 10 ms nap (`g_reclaim.kicks`/`empty_kicks`/`empty_until_ns`, write.c). (2) The 244 `inode-rpc: slow-ok ... attempts=1 saw_busy=0` at exactly 5.0 / 10 / 15 s were `efs_client_conn_get` timing out on a full 16-slot pool (`pthread_cond_timedwait` 5 s, silent), not the network; `put_fragments` and `fetch_fragment` then counted the NULL as a node failure and four of them marked a live node DOWN for 30 s (`EFS_NODE_DOWN_FAILS`=4, `EFS_NODE_DOWN_MS`=30000) — the shape of the two 28 s client-wide freezes in the ecopy strace and the 12 `no quorum` PUTs against four live servers. The timeout is logged (`efs: conn pool node=N exhausted for 5 s (pool=…)`, first ten then every 100th) and no longer counted; the default pool is 64 per node (server cap `EFS_SERVER_MAX_CONNS` 4096, 13 × 64 = 832; one RDMA conn pins ≈ 2.6 MB per end, slots connect lazily). Not changed: the 5 s checkout timeout, REPORT's one-at-a-time whole-dirty-set shape (splitting it is an ask), follower `apply-sleep` (D14). "ecopy + atime": no new defect — W25 item 2 landed in 2dd77dac and the strace has zero mismatch lines; the roll's gate re-checks the round trip. Servers `e002771e56e4-dirty` rolled `--all` at 05:43Z (`~/efs-runs/roll107.log`); fstor007's `/tmp/efs` rebuilt on the same tree for the user's `client.sh`. Gated on fcstor007 with the new client (`~/efs-runs/gate109.log`, `fuse.efs-fuse` + MOUNT_OK): `mtime_repro.py` rows `ecopy-order*`, `rsync-order`, `fsync-between` all end at 1380661863; `touch -a -d @1500000000.123456789` → `stat` `1500000000.123456789` (atime nanoseconds round-trip); posix jobs=1 **200/201**, 0 EFS BUGS, `mmap_write_read` SKIP only, ~40 s (`results/posix/20260930-054501`); fcstor007 unmounted cleanly afterwards, no `conn pool` line. Still to gate under load on the next ecopy: `grep -c "conn pool node" fuse.log` and no `no quorum` line while all four servers are up (2); `dcache_reclaim_main` out of the perf top (1).

**Sep 30 05:13Z (superseded above): F1, F2 and the mtime fix (A) are in tree and rolled; committed `a5e3d5fe`.** Servers `06916bc7e5c1-dirty` rolled `--all` at 05:06Z (`~/efs-runs/roll78.log`, ROLL_OK, no recorders, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`); the user mounted fstor007 with the same tree at 05:10:32Z (`client.sh`, `fcstor003.ib:19810`). **F1:** `struct efs_conn` has `recv_timeout_ms`; `efs_conn_set_recv_timeout()` sets it and the TCP `SO_RCVTIMEO`; `conn_rdma_frame` waits `recv_timeout_ms` instead of the 30 s `EFS_IO_TIMEOUT_MS`; `host_sender` calls it with `HOST_SEND_IO_MS` (250 ms) on every conn kind (the per-sender conn is private, `server_peer_conn_new`, so no pool restore). **F2:** `server_raft_host_inbox` counts the inbox-full drop in `h->inbox_drop` (logs the first ten as `raft-host: inbox full, dropped frame type= group= from= len=`) and the `raft-obs: wait_timeouts=` line ends with `inbox_drop=`. **Mtime:** `efs_fuse_utimens_ino` calls `efs_utimens_flush_dirty()` before the SETATTR when mtime is set — if the inode has writeback jobs pending or is in the dirty set it runs `efs_append_flush_report(NULL, ino)` (the close path's flush+REPORT), so the publish lands under the old `mtime_gen` and the utimens fences it; a clean inode pays one dirty-set lookup. `efs_client_mtime_pin` logs once when the 256-entry table is full instead of dropping silently. **Gate on the live mount (FUSE_OK, `~/efs-runs/rec-gate84.log`):** `write→futimens→close→rename`, `write→futimens→close`, `write→close→utimens`, `write→futimens→fsync→close` all end at 1380661863; `futimens→write→close` ends at the write time, which is POSIX. Unit tests on fcstor007: 14 of the 15 `make test` binaries pass, `test_raft` OK with the two new cases; **`test_raft_store` fails 4 assertions on clean HEAD too** (`snap index should report its term`, `compacted prefix differs`, `snapshot did not shrink the log`, `reopen after rotation` — W22.1's retained log window, test not updated; `~/efs-runs/rs77.log`), and `docs/check-architecture.py` fails on the regen-diff and the repeated §1 decision-table headers. After the roll, under the user's load: fcstor004 `tx->2 fail=5` (250 ms each now, not 30 s), one group-0 election (term 94→95 at 05:11:19Z), `inbox_drop=0` on every node, `wait_timeouts=5`, 256 `kv-compact: start` lines and 120–270 ms `apply-sleep` waits on group 2 in one window. The lost RAFT_REPLY itself is still unexplained (D15/D16 pending). Leftover: a plain-dir `/tmp/efs-mount/measure/mtime-repro2` under fstor007's mountpoint (my gate ran 30 s before the user's mount came up; those rows are local XFS and are not a result); remove it when the mount is down.

**Sep 30 04:45Z: ecopy's 153 "verification metadata mismatch" lines are all `(mtime)`, and they are an efs bug, not F1/F2** (`results/measure/20260930-044100-mtime-utimens-close/SUMMARY.txt`). ecopy does `write → futimens(fd) → close → rename` (POSIX-valid; XFS keeps the time). On efs the write is client-buffered, so its PUBLISH reaches the server inside the close REPORT, *after* the utimens; the publish carries `p.now` and the current `mtime_gen`, so the lane stamp wins the stat MAX and mtime becomes the close time. Reproduced from one process on an idle cluster (`mtime_repro.py`): `write→futimens→close` and `write→futimens→fsync→close` are wrong, `write→close→utimens` (rsync's order) is right. `EFS_INO_REC_F_TIMES` is set by the client and read by nothing on the server; `MTIME_PIN_MAX` 256 drops pins silently. Because of this every file ecopy had already copied failed `same_size_and_mtime` and was copied again (the 22.4 GiB). **Fix A written and gated 05:12Z (block above):** the FUSE setattr path flushes the inode's dirty dcache before a MTIME/ATIME SETATTR, so the publish lands under the old `mtime_gen` and the utimens fences it — no wire or server change, and ecopy's close REPORT becomes empty. Alternative B: make the REPORT apply honour `EFS_INO_REC_F_TIMES`. The repro's client on fstor007 was stopped again.

**Sep 30 04:20Z: the post-fix runs from fstor007 are reduced (`results/measure/20260930-040600-postfix-review/SUMMARY.txt`). The wedge fix holds; a second Raft problem is measured and needs two decisions.** With no server strace, one client, warm servers: `ls` 0.015 s, `find -ls` of the tree 6.5 s and `du` 5.8 s with no call over 0.2 s, and a re-sync of `~/git` at stat 0.33 ms, openat 0.60 ms, close 0.34 ms, utimensat 1.05 ms, chmod 0.49 ms, rename 6.2 ms (max 36 ms). Those are the per-op references for `06916bc7e5c1-dirty`. Then `ecopy --verify` (400 threads, 22.4 GiB) put both groups into an election storm: group 0 63 terms, group 2 426 (03:56→04:09:39Z, settled at term 669, leader fcstor005); 12 `newfstatat` took 55–59.5 s, 513 took ≥ 0.2 s; 21 `shard=0` LOOKUPs and 8 REPORTs exhausted 16 BUSY retries; fcstor004 `wait_timeouts` 0→1290. Cause, from `raft-obs tx->` and the code: `host_sender` blocks for `RAFT_REPLY` per batch, and on an RDMA peer conn that wait is `EFS_IO_TIMEOUT_MS` = **30 s** (`conn_rdma_frame`, protocol.c:380) — the 250 ms `HOST_SEND_IO_MS` bound is applied to TCP conns only (raft_host.c ~757). `sent` froze for 10–30 s on fcstor004→005, fcstor006→004 and 006→005 while `enq` grew, then `fail` +1. The peer processes the batch (fcstor006 won votes through a frozen lane) — the reply is what does not come back; which side loses it is the open measurement (stack-sample the sender and peer-conn threads in a storm; on-CPU perf will not show it). The unheard peer campaigns and `on_vote_req`→`maybe_step_down` deposes the live leader every time (no Pre-Vote, no leader stickiness). **Fixes, in SUMMARY.txt:** F1 bound the RDMA peer reply wait to `HOST_SEND_IO_MS` (mechanical, the decision is the raft_host.c:45 comment) and F2 count `server_raft_host_inbox` BUSY drops; **D15** leader stickiness / Pre-Vote and **D16** a separate credit class for peer frames are questions for the user. Servers still run `--perf`; fstor007's client is stopped (user ran `client.sh stop` at 04:05Z). Do not quote a rate from these runs.

**Sep 30 03:57Z: the group-0 wedge is fixed and rolled.** `tests/test_raft` and the two new cases pass on fcstor007 (`~/efs-runs/raftfix62.log`). `on_ae_req` reports `match = prev + nentries` (a stale tail is not a match) and the leader no-op, cold entry, and joint entry advance `send_idx` the way `efs_raft_propose` does. Rolled `--all` at 03:56Z with `--perf` only (`~/efs-runs/ready65.log`, same embedded id `06916bc7e5c1-dirty`, the tree is uncommitted): group 0 elected fcstor003 at term 43 and all three voters hold 437638; group 2 leader fcstor004 term 262. fstor007 remounted `client.sh --perf`: `ls -l` and `stat` of the root return at once, mkdir+rmdir OK, group 0 commit 437640 on every voter, zero `apply-sleep`. The 23:16 rsync and the 23:51 `ls` (EBUSY after 16.76 s, `~/logs/ls.strace.txt`) were this wedge; do not quote either. The no-strace rsync rerun is ready.

**Sep 30 03:10Z: the no-strace rsync rerun is set up, not run.** Rolled `--all` (`~/efs-runs/ready59.log`) to `06916bc7e5c1-dirty` with `EFSD_ARGS=--perf` only (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`). All four efsd are `--perf`, `strace=0`, no `efsd.strace`; `efsd.data` is `/tmp/efs-perf/efsd.data` and is unreadable until the recorder stops. Both groups commit==applied (leader fcstor005, terms 41 and 261). fstor007 only is mounted: `fuse.efs-fuse`, `MOUNT_OK`, `EFS_TRANSPORT=rdma`, `client.sh --perf`, `strace=0`. Client samples go to `~/orcd/scratch/efs/perf/rsync-perf/efs-mount/fuse.data` (reports on `scripts/client.sh stop /tmp/efs-mount` from `/tmp/efs` on fstor007). fcstor003–015 are not mounted. The user runs the 5 min rsync. Do not quote a rate from this setup.

**Sep 30 02:30Z: the user's traced rsync from fstor007 is reduced** (`results/measure/20260930-021317-fstor007-rsync/SUMMARY.txt`). 4961 files in 287 s, 17.3 files/s, zero errors and zero elections on either side. The receiver is in efs syscalls 99 % of the wall: rename 25 ms (44 %), three `openat` 23 %, utimensat 11.5 %. 3.75 RPCs per efs syscall; 68 % of all RPCs are LOOKUPs (entry_timeout 0 re-resolves every component). Every efsd was under strace, so absolute latencies are 3× the Sep 29 16:41 reference (peer RTT 400 µs vs 24 µs); the counts and shares hold. Do not quote these latencies as the system's.

**Sep 30 00:57Z: posix2 is 63/63. Next is the fstor007 dd.** At 01:45Z the four servers were restarted with `--perf` and `--strace` (`~/efs-runs/restart53.log`); recorders are `/tmp/efs-perf/efsd.data` and `efsd.strace` on fcstor003–006. Clients fcstor003–015 and fstor007 are stopped. `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, same 17:04Z table (restart needs no `--join`). Embedded id `bd57b7e74a91-dirty` (HEAD `bd57b7e7` plus this uncommitted tree). Do not widen the 400 ms apply wait. Do not retry the one-shot REPORT. D14 and folding a same-group rename into one command stay asks.

What this tree changes, and what the gates showed:

- `host_read_inode_lanes` returns NOT_PRIMARY with a dual-host hint when the lane's group is not local, and setattr / unlink / link / rename forward before propose. `nohint=0` on all four servers across the 9-host run.
- The create EIOs were an apply verdict of BUSY (`rc=-13`, alloc-key intent) that `host_wait_settled` discarded; the handler then looked the name up, missed, and the client got NOT_FOUND → EIO. `server_raft_host_create` now waits with `host_wait_verdict`, so BUSY is retried. A name clash stays EXIST (it was rewritten to OK, which made both `O_EXCL` creators win). Gate: 9-host **200/201** on fcstor007–015, 15.6–17.3 s, 0 bugs (`results/posix/20260930-000428`). `create-miss=0`. `apply create rc=-13` was 6/8/8/2 on fcstor003–006 and the suite still passed.
- Rename: a dest inode that vanishes between the dentry read and the row read is STALE. A NOT_FOUND after the reply's getattr is OK with a zero stat only when `host_txn_commit` itself returned OK. Treating a NOT_FOUND *from the commit* as success made both sides of `peer_rename_same_src_two_dst` return 0; the loser is `rename_at … rc=-3` again and that test passes. `peer_rename_vs_unlink_dst` and `peer_overlap_pwrite_partial` pass (`efs_dcache_overlay` takes the whole image only when the map has no deltas).
- The two holder failures were not a slow unlink. `rpc_note_slow_ok` never fired, and every posix2 step was under 2 s. The holder saw `go` and then `read` returned EIO (`after go: errno 5`); fuse.log was `layout pull rc=-3` because open adopts the row only and the last unlink, with no lease, retires the inode. First `open()` of an existing regular file now takes `HOLD` (`efs_client_rpc_hold` flags=1, owner `flock_token`); last close releases it. CREATE still does not: a hold per new file is the names_crazy wall. A flock unlock already closes that same owner, so release does not close twice.

posix2, fcstor007+008, **63/63**, 47.2 s (`results/posix2/20260930-005552`, `~/efs-runs/gate49.log`). `peer_unlink_while_b_has_fd`, `peer_open_unlink_nlink`, and `peer_create_unlink_stat_churn` all pass. The 00:13Z **60/63** (`results/posix2/20260930-000915`) is the run before this hold.

fstor007 8 GiB `dd bs=1M conv=fsync`, own file, `FUSE_OK`, on-disk 8589934592, wall **12.396 s → 661 MiB/s** (`results/measure/20260930-005700-dd-fstor007`, `~/efs-runs/dd50.log`). That is below fcstor007's Sep 28 **947** and above the ecopy plateau of 345–372, so the single stream is already short of the fcstor host and the 400-thread pattern is a second gap. This dd created a new file, so it did not pay the open hold.

The ecopy of `~/git` did not finish (`results/measure/20260930-012100-ecopy-git`, `~/efs-runs/ecopy51.log`). `~/git` now contains `linux-xfs`; the Sep 29 copy of that path was 3.8 GiB in 24 s. The ssh budget was 240 s and the copy was still running, so there is no rate. The flushed lines are 11 EBUSY, 4 EIO, 8 ENOENT. During that window group 0 (leader fcstor003, term 31) stayed `hi=1`, `pub_p50=0`, `apply_max` under 100 µs except one 7.3 ms sample, and the leader's on-CPU time was about 20 s across the four minutes (`eventfd` write 8 %, `memcmp` 7 %). Group 2 did not: terms ran 18→253, `tx->2 hi=2048`, and fcstor005 recorded `pub_p50=405 ms` once. It had a leader again at 01:30Z (leader 3, term 253, commit==applied). The partial tree `/tmp/efs-mount/measure/ecopy-git` on fstor007 is still there; `rm` hit the same 60 s clock. Do not ask about folding a rename or about D14 off this run: group 2 lost its leader, so the copy never measured a quiet metadata path.

**Sep 29 20:55Z: why rsync and ecopy are slow from fstor007 — the user's four copies of 16:41–16:44 EDT, reduced.** Source files: `~/orcd/scratch/efs/perf/efs-mount/` (`fuse.data` 44K samples, `fuse.strace` 630 MB over 16:40:57–16:44:50, `efs-fuse-efs-mount.log`, `ecopy.strace{,2,3}.txt`, `rsync.strace.txt`). Reduction: `results/measure/20260929-164057-fstor007-copy/` (`copier-syscalls.txt`, `fstrace1.log`, `nohint-addr2line.txt`, `flat.txt`, `callers-head.txt`, `fuse-log-last-mount.txt`). The client is today's build (`36 x 72 KiB bufs/conn`, `why=no-hint from=`); the servers are the 19:10Z roll; no server recorder was running, so the server side of this window is `raft-obs` and `efsd.log` only. All four copies were ^C'd.

What the copies did:

| run | source → dest | wall | bytes | shape |
| --- | --- | --- | --- | --- |
| ecopy2 | `/orcd/scratch/001` → `002` (large files, 396 threads) | 31 s | 8.9 GiB | 800–1600 MiB/s for 3 s (dcache fills), then flat **345–372 MiB/s** to the ^C |
| ecopy | `/data1/erbmi1/software` (396 threads) | 51 s | 5.0 GiB | first 5.5 s every syscall stalled (started 8 s after ecopy2's ^C left its dirty tail; `efs-fuse` did 160K `futex` calls in those 6 s); pwrite avg 94 ms, max 10.7 s |
| ecopy3 | `~/git` (526 threads) | 24 s | 3.8 GiB | 3.6 GiB of `results/` in 4 s, then a pure metadata phase at **160–215 files/s** |
| rsync `-aP` | `~/git` (3 processes, one op at a time) | 37 s | 377 MiB | **965 files, 26 files/s**, 10 MiB/s |

Why, in order of weight:

1. **rsync is a serial chain of metadata RPCs; each is a Raft round or three.** On efs in that run (rsync chdirs into the destination; every path in its trace is efs): `newfstatat` 0.87 ms, `openat(O_CREAT)` 1.22, `close` 0.48, `chmod` 1.22, `utimensat` 2.46, `mkdir` 4.88, **`rename` 9.21 ms** (1273 calls, 12.0 s of the 37). Seven such calls per file is 17–20 ms, plus the 50 ms below on 25 of the renames = 26 files/s. Nothing in rsync's trace waits on data. The 9 ms rename is the multi-shard txn (PREPARE parts, DECIDE, RESOLVE: three sequential commits); a same-directory file rename whose src dentry, dst dentry and parent row sit on one group could be one command — that is the "fold PREPARE parts into one command" protocol change §1a already marks **ask**. Do not start it without the decision. `utimensat` + `chmod` + `close` are three more rounds on the same inode that rsync issues back to back; there is nothing to fold there without changing what each call promises.

2. **The hintless NOT_PRIMARY has a source now, and it is not an election: `host_read_inode_lanes` on a single-group host.** Item 3's log line fired on fcstor003 32× in this window, `from=0x40f475` and `0x40f766` = `handler.c:662` and `:704`, the return addresses of `server_raft_host_setattr` and `server_raft_host_rename_at` (`set_inode_rc` is inlined). Both call `host_read_inode_lanes` after the op committed (`raft_host.c:9577`, `:10542`; 13 more callers: utimens `:9496`, unlink `:8582`, link `:9931`, hashed create `:7203`, getattr `:6699`, …). That function issues `host_read_index` on every active lane's group (`:4407`); lanes straddle both groups, so any inode with an odd lane active asks a single-group host for the group it does not host, and `host_read_index` returns NOT_PRIMARY with `hint = -1` on `!v.has` (`:2874`). fcstor003 is the client's first live voter for group 0 and hosts only group 0, so every SETATTR/RENAME of a multi-lane file that lands there pays it. Today's client (skip-the-voter) paid **50 ms × 64** in this 4-minute window (25 RENAME_AT, 24 SETATTR, 13 LOOKUP, 2 CREATE). Yesterday's client re-asked the same node 16× — that is the **810 `exhausted … -> EBUSY`**, the **177 `create .ecopy.tmp… failed`** and the `rename_at … rc=-13` lines in the older sections of the same log, and it is why the earlier ecopy runs were the slowest thing on this page. Fix (no decision needed): in `host_read_inode_lanes`, an active lane whose group this host does not serve returns NOT_PRIMARY with a **real hint** — a node that hosts both groups (`host_pick_peer(h, {SHARD, SHARD2}, 2, -1)`), never −1 — and the callers that already have the row in hand (setattr, utimens, rename, unlink, link) do that check **before** `host_propose`, so the op runs once on a dual host instead of committing here and being retried there. The retry after commit is safe today (setattr/utimens idempotent, rename/unlink/link/create answered from the op-id window) but it is a wasted commit. The remaining three lines (`handler.c:839`, fcstor004 ×2 and fcstor005 ×1) are `server_raft_host_report` on dual hosts — a different path; read it after this one. Gate: `grep -c 'NOT_PRIMARY without hint' efsd.log` stays 0 on all four across an rsync of `~/git`, and `why=no-hint` disappears from the client log.

3. **One client on fstor007 writes 360 MiB/s sustained; fcstor007 did 947 with `dd conv=fsync` yesterday.** ecopy2 is the clean measurement: 15 s flat at 345–372 MiB/s with ~400 writers, `pwrite` of 1 MiB averaging 53 ms. The client was ~19 % of one core (44K samples in 233 s): not CPU. In the profile the put path is 37 % inclusive (`dcache_put_now` from `put_pool_thread` 18 % and `dcache_flush_slot_inner` 18 %), and inside it `__lll_lock_wait` is 5.9 % under `put_fragments_parallel_once` / `put_recv_reply` / `efs_client_put_fragments_parallel`: that is `conn_lock[node]` in `efs_client_conn_get`/`_release`, six lock hand-offs per chunk across the put threads. `memmove` 13.4 % inclusive is the RDMA send copy plus the FUSE copy; blake3 7.4 %; `xor_into` 3.7 %. Measure before changing anything: **8 GiB `dd bs=1M conv=fsync` from fstor007** (the honest-fio rule's shape). ~360 → the host or its HCA (`mlx5_1`; check `ibstat` rate and an `ib_write_bw` to fcstor003 against the fcstor hosts' `mlx5_2`), and no client change moves it. ~900 → ecopy's pattern (400 threads × 1 MiB `pwrite` into one daemon) is what the put path cannot drain, and the pool mutex is first: pin one conn per node per put thread so the hot path takes no pool lock. Either way the number, not the guess, goes in this page.

4. **Small files saturate at ~2K metadata ops/s from one client, and latency inflates to match.** ecopy3's metadata phase: 24 584 `newfstatat`, 8 459 `openat`, 9 248 `close`, 3 154 `utimensat`, 1 021 `mkdir` in 24 s ≈ 1.9K ops/s, and at 526 threads the same ops that cost rsync 1–5 ms cost **14 ms (open), 60 ms (close; 21 of them ≥ 5 s), 70 ms (utimensat), 75 ms (mkdir)**. That is the Sep 27 same-parent rate (1 385/s at 9 procs) seen from the other side: throughput flat, queue depth ×500. Which side queues is not in this data (no server recorder). Next measurement: the same ecopy of `~/git` with `perf record` on the four `efsd` (no strace) and `raft-obs` every 5 s; read `pub_p50`, the leader lanes' `hi`, `apply-sleep` per group, and the top of the group-0 leader's profile. If the leader is idle and the client's handler threads sit in `futex`, it is the client (the `futex` storm at ecopy's start says look there first: `dcache` mutexes and `g_client.idx_mu` under 500 writers). If the leader is busy, it is one Raft round per op and the answer is batching proposals per client (already done for concurrent proposers) or D14.

5. **The staging table grows past 256 MB with nothing evictable** (`efs-fuse: staging table over EFS_CLIENT_META_MB … growing`, twice in this mount; `stage_evict_main` 4.5 % inclusive). Those are dirty rows waiting for their REPORT; with 400 writers the REPORT cadence is behind the create rate. Not a fault by itself — pinned data is real work — but it says the close-time REPORT is the client's backlog, and it is what the 5.5 s stall at ecopy's start was draining (ecopy2's tail). One `put_fragments … no quorum (efs_rc=-8) attempt 1/4` in the window: one PUT got fewer than 2 acks inside its wait and succeeded on retry.

Not the cause: `recvfrom` max 0.85 s, `poll` on the CQ channel 361K calls averaging 2.9 ms (that is the RPC wait, i.e. the Raft round), the 30 s `read(/dev/fuse)` lines are idle workers, `copy_file_range` EXDEV is ecopy's cross-mount fallback, exit 130 is the ^C.

The hint fix (item 2), the create / rename / overlay fixes, and the open hold are in the 00:57Z block above. posix2 is 63/63. Item 3's dd is **661 MiB/s**. Item 4's ecopy did not finish: group 2 elected through term 253 with `hi=2048`. That block has the numbers.

**Sep 29 20:21Z: items 1–7 of the 17:39Z list are in the running binaries. jobs=1 is 200/201. The 9-host suite is not.** Cluster is up, no recorders, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, fresh 17:04Z table (restart needs no `--join`). Rolled 19:10Z from `33c7e29c` plus this tree (`~/efs-runs/gate40.log`); the embedded id is `33c7e29cd161-dirty` and stays that until the next roll. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT. D14 (serve a leader read from a heartbeat lease) is still a question — do not write it. Item 8 (libfuse cannot cap total workers) is not done. Item 6's `.sum` sidecar stays (GC proves the fragment with it) and GET still has no root field: the process-wide cache is filled by the PUT that already returns the root.

What landed:

1. `overlay_chunk_deltas` skips `len == 0`. The apply already kept that tombstone and skipped it.
2. The sender drains up to `min(8, nrecv-1)` frames per wake (`HOST_SENDER_PIPE_MAX` in `raft_host.c`), then collects that many replies. `send_ae` on the flush path calls `send_commit_probe` when a caught-up peer's `ae_inflight_commit` is behind `commit_index` (one empty AE per peer per commit advance; the priority lane replaces an older heartbeat).
3. A hintless NOT_PRIMARY skips that voter on the next pick (`raft_voter_conn_skip`). `set_inode_rc` logs the return address at most once a second (`NOT_PRIMARY without hint`).
4. `recv_poller` blocks 1 s on the CQ channel after arming and draining.
5. `FUSE_CAP_SPLICE_READ` is cleared. The 8 MiB `pipe-max-size` sysctl is unused by efs.
6. `make_dir_up` creates the leaf and walks up only on ENOENT. `frag_loc_cache` is process-wide (65536 slots), published by a PUT and by a GET probe hit, so the next GET on any thread opens that root first.
7. `persist_applied` `pwrite`s 16 bytes (index and its complement) into one long-lived fd and `fdatasync`s. A torn or short record reads as 0. `clean_cluster.sh` treats mkfs `rc=-13` as success once `raft-status` prints `root=1`.

Gate (`~/efs-runs/gate41.log`):

- jobs=1, fcstor007: **200/201**, 0 fail, `mmap_write_read` SKIP, 28.2 s (`results/posix/20260929-191140`). The five folded-chunk EIOs and `dir_readdir_while_unlink` passed. Do not quote `results/posix/20260929-191224` (the `--parallel` call had no host list and ran on one host).
- 9-host, jobs=16, 13.5–14.7 s (`results/posix/20260929-202024`): **200/201** on fcstor010, 012, 013, 014, 015. Five EIO: fcstor007 `dir_many_files` `f0064`; fcstor008 `dir_readdir_listing` `f39`; fcstor009 `concurrent_create_unlink_two_proc` `c0-4`; fcstor011 `dir_many_files` `f0007` and `names_crazy_roundtrip` `back\slash`. fuse.log on 009 and 011 is `create <name> failed (EIO)`, which is `fuse_create_errno`: the create returned no inode, `last_err` was not BUSY (that path returns EBUSY), and the follow-up lookup missed the name. 007 and 008 were remounted for posix2, so those two logs are the later mount.
- posix2, fcstor007+008: **60/63**, 46.9 s (`results/posix2/20260929-202044`). `peer_unlink_while_b_has_fd` (holder `go timeout`), `peer_overlap_pwrite_partial` (`[8192,12288)` is not B's range), `peer_rename_vs_unlink_dst` (rename ENOENT; 007 logs `rename_at … rc=-3 after 1 tries`).

raft-obs at 20:21Z, last sample: `rtt_avg` 22–24 µs (was 440), leader lanes `hi=10` and `hi=6` (was 651), followers `hi=1`. `wait_timeouts=0`, `nohint=0`, `report-split rc=-13` count 0. `apply_max` 99–264 µs, `persist_max` 499–891 µs, `pump_hold_max` 109–610 µs. The 47 ms `apply_max` seen on fcstor004 after jobs=1 did not recur. `apply-sleep` since the 19:10Z start is 30 / 192 / 0 / 0 on fcstor003–006 (it was 2 / 2 / 0 / 0 after jobs=1), so the 9-host run added sleeps over 20 ms on the two group-0 voters while `apply_max` in that same tail stayed under 300 µs. `pub_p50` on fcstor004 is 829 µs.

What to do next, in order. None needs a wipe; each is a roll of the four servers (build-ID change → `roll_efsd.sh --all`) plus `tests/deploy_fuse_clients.sh`, then the gate named. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

1. **The five create EIOs are unattributed because both ends are silent. Make them speak, then rerun the 9-host suite.** Evidence: jobs=1 is 200/201 and jobs=16 on nine mounts lost 5 creates in ~3 000 (`dir_many_files` ×2, `dir_readdir_listing`, `names_crazy_roundtrip`, `concurrent_create_unlink_two_proc` — every one a create of a fresh name under 16 concurrent tests on one mount). The four server logs have no `raft-host: create … rc=`, no `opid-replay`, no `NOT_PRIMARY without hint`, no `txn-recover` in 20:20:24–20:21:37Z; `dirop_fail_on` (`raft_host.c:552`) prints only BUSY and STALE, so a create that returned IO, INVAL, PROTO, NOT_FOUND, or EXIST left nothing. The client printed `create f0007 failed (EIO)` (`efs_fuse.c:2849`), which is `fuse_create_errno` after `efs_client_create_ex` returned 0 with `last_err` not BUSY and not QUOTA and a follow-up LOOKUP of the name missing: the rc is not printed. No `inode-rpc: retry` line on 009 or 011, so it was not a send/recv drop and not a hintless NOT_PRIMARY; BUSY/STALE retries below exhaustion are silent, and `rpc_send_recv_shard` returns `EFS_ERR_PROTO` silently on `rtype != expect || plen < reply_len` (`inode_rpc.c:436`). Change: (a) `create %s failed` prints `g_client.last_err`; (b) the PROTO return prints `type`, `rtype`, `plen`, `reply_len`, `nid`; (c) `dirop_fail_on` returns 1 for every rc except OK, BUSY, STALE, EXIST, NOT_FOUND, NOT_EMPTY (those five are legitimate answers and EXIST is every `O_CREAT` open of an existing file); (d) in `server_raft_host_create`, when the propose+`host_wait_settled` returned OK and the post-commit `efs_meta_apply_lookup_tx` misses, print that separately — that is a committed create answered as failure, the I16 class. Gate: `EFS_TRANSPORT=rdma bash tests/run_tests.sh posix --parallel fcstor007.ib … fcstor015.ib` (3 min); read the new line, then fix what it names. Do not guess at the cause before that line exists.

2. **`peer_rename_vs_unlink_dst`: rename a→b returned ENOENT while a was alive. Two NOT_FOUND returns in `server_raft_host_rename_at` are the cause; make them STALE and OK.** fcstor007's fuse log: `rename_at 12265/a -> 12265/b rc=-3 after 1 tries`; the client retries STALE and BUSY for RENAME_AT (`stale_retryable`), never NOT_FOUND. In the handler: (a) the dest dentry is read at `raft_host.c:10103` (`xist=1`, b = OLD), then the dest row at `:10176` `efs_meta_apply_get_inode(h->kv, ndent.ino, &nrow)`; B's unlink of b committing between those two reads makes that get NOT_FOUND, which goes straight to `set_inode_rc` as ENOENT. A dest that was there a moment ago and is gone is the dest moving under the txn — return `EFS_ERR_STALE` so the client re-runs and the rename lands on an empty name. (b) After `host_txn_commit` succeeds (`:10538`), `host_read_inode_lanes(row.ino)` and `efs_meta_apply_getattr(row.ino)` (`:10542–10544`) run before the reply; B's unlink of the now-renamed b (NEW, last link → DEL) in that gap returns NOT_FOUND for a rename that committed. Answer OK with `out->inode.ino = row.ino` and a zero stat, the same shape `host_opid_reply_ino` already uses for "row gone"; the client's dual-apply must tolerate a zero stat there (check `efs_client_rename_at`, `ops.c:1263`). `peer_rename_same_src_two_dst`'s `rename_at 16898/x -> 16898/z rc=-3` on 008 is the loser of that race and is correct. Gate: `python3 tests/posix/posix_2client.py --remote fcstor007.ib fcstor008.ib --mnt /tmp/efs-mount --filter peer_rename_vs_unlink_dst` ×10, then the full posix2.

3. **`peer_overlap_pwrite_partial`: `efs_dcache_overlay` stamps the whole local image over a fetched chunk that has spans; `efs_dcache_copy` already refuses that case. Add the same guard.** The server state cannot be wrong for this test: A's [0,8192) and B's [4096,12288) are both span publishes (D1, commuting), and either fold order satisfies the assertion. The failing read is A's, on the mount that wrote the whole 12 288-byte chunk in step a0 (clean full image, `have_base`, `base_gen` = the table gen). A span PUT keeps the table gen as the base and adds a delta (`write.c:2323–2331`), so after A's own publish `export_chunk_gen_of` still equals `e->base_gen` and `dcache_image_current(e, tg)` is 1. `efs_dcache_copy` (`write.c:2541`) serves the image whole only `&& !spans` (the local map has no deltas) — correct, A's map has its own delta, so the read goes to the fetch path. That path decodes the published chunk, overlays the server's deltas (B's span included), then calls `efs_dcache_overlay` (`read.c:818/841/851`), and that function (`write.c:2612`) copies the whole local image over `dst` on `dcache_image_current` alone, no `spans` check. A's image is a0's zeros plus A's range; B's [8192,12288) is gone. Fix: in `efs_dcache_overlay`, take the whole-image branch only when the map has no deltas (same `export_chunk_copy(…).ndelta` test as `efs_dcache_copy`); with deltas, overlay only the unpublished ranges (the branch below it), which is also right for A's own range because it is already in the fetch. This is the Sep 27 morning "overlap pwrite" class, not the tombstone. Gate: `--filter peer_overlap_pwrite_partial` ×10, `peer_overlap_pwrite_same_range` and `_chunk_straddle` still pass, `test_chunk_deltas` (W17.3) still passes, full posix2.

4. **`peer_unlink_while_b_has_fd`: step a2 (A's `unlink` of a file B holds open, then `exists`) took 10–19 s and then succeeded.** The holder on B timed out its 20 s wait for `go` while B's `_holder_go` was still inside its own 8 s wait, so `go` was written 12–20 s after `ready`; only step a2 and two ssh hops sit between. The 16-step BUSY/STALE ladder is 10.35 s and prints nothing unless it exhausts, and the server printed no `raft-host: unlink … rc=` (rate-limited BUSY/STALE) in the window. Change first: `rpc_send_recv_shard`/`_dual` print one line when an op returns OK after more than 1 s of retries (`type`, attempts, `saw_busy`, last status). Then run the filter alone with `time` around step a2 and read that line. If it is BUSY from `host_wait_applied` on the unlink's group, the pair is `apply-sleep` on that leader at the same second (fcstor003/004 added 28 / 190 `apply-sleep` lines during the 9-host + posix2 window while `apply_max` in the tail stayed under 300 µs — those sleeps are the 20 ms-plus class, not the 400 ms deadline, and this is where to look at them). Gate: the filter ×10 under 2 s each, full posix2.

5. **Then, not before:** D14 (leader read lease) is a decision to ask for, and item 8 (libfuse thread growth) is a libfuse limit. The 9-host numbers to beat are 200/201 on all nine (Sep 27, `results/posix/20260927-123717`) and posix2 63/63 (`results/posix2/20260928-044304`).

**Sep 29 17:39Z: cluster and clients stopped; the 17:04Z run is reduced. The fix list below was implemented at 20:21Z (the block above); this paragraph is the evidence.** Stop was SIGTERM clients, then servers (`~/efs-runs/stop35.log`); fcstor006 exited on SIGTERM, fcstor003–005 were killed after their recorder children were signaled. Files, node-local: `/tmp/efs-perf/efsd.data` 10–13 MB + `efsd.strace` 0.9–1.1 GB on fcstor003–006; `fuse.data` 0.6–1.5 MB + `fuse.strace` 154–186 MB on every client. Reduction: `results/measure/20260929-170457-wipeposix/ana` (`SUMMARY.txt`, then `perf-<host>-<efsd|fuse>-<posix|idle|ecopy>.txt`, `strace-…-top.txt`, `efsd-<host>-raftobs.txt`, `fuse-fcstor007.log`). Windows (EDT): posix 13:05–13:07 (jobs=1 on fcstor007), idle 13:10–13:34, ecopy 13:35–13:37 (user's `ecopy --verify` of `/data1/erbmi1/software` from fstor007; that client's own profile is `~/orcd/scratch/efs/perf/efs-mount/`, 22K samples, 67 s of strace, 7.68 GiB at ~110 MiB/s, ^C'd). About 60 % of every server sample and 50 % of every client sample is `ptrace_*` from the attached strace; the shares below are of the rest. The next roll must not carry `--strace` if the number is a bandwidth.

What was found, in the order to fix it. Each item names the evidence and the change; none needs a wipe.

1. **Reads of a chunk that was folded once return EIO. Fix in `src/client/read.c` `overlay_chunk_deltas`: skip `ce.deltas[i].len == 0`.** The apply keeps a `len == 0` tombstone in the span trailer after a fold and skips it (`meta_apply.c:3410–3415`); the client's `overlay_one_delta` returns `EFS_ERR_INVAL` on `d->len == 0` (`read.c:623`). `fuse-fcstor007.log`: `read: invalid argument (efs_rc=-5) ino=6216 off=0 len=11` and `ino=5722 len=65537`. The write side hits the same check through `efs_client_fetch_published_chunk` → merge base fails → "do not PUT a zero base" → `flush-meta: I/O error` (inos 6079, 6693, 6724) and `unmount: data flush incomplete (dcache=-9)`. This is all five EIO tests, and the mapping is exact for the reads: Python's `read()` on a 10-byte file issues `read(11)` (`basic_overwrite_middle`: write 10, overwrite 3 in the middle, read back) and on a 65 536-byte file `read(65537)` (`content_random_overwrite_append`). The other three fail in the close flush (`efs_rc=-1` = `EFS_ERR_IO` from `write.c:3560`, "do not PUT a zero base"): ino 6079 ×4 is `concurrent_appends`' four workers, 6693 and 6724 are `concurrent_overlap_write` and `mtime_monotonic_many_writes` (each a sub-chunk write after a fold; the STALE replay path at `write.c:4225` fetches the same base). None of the six came from a BUSY REPORT: the report loop still retries BUSY 8× and prints `report-loop … rc=` when it gives up, and `fuse-fcstor007.log` has no such line and no `rc=-13`. Gate: those five, then posix jobs=1 back to 200/201.

2. **The leader's per-peer sender lane is the metadata latency. Fix in `raft_host.c` sender thread: drain the whole lane per eventfd wake with no syscall between frames, and drop the per-frame `fstat` + second `read`.** Evidence: `apply_max=0`, `pump_hold_max` 5–7 µs, pump thread 2 500 cycles/s with no gap over 15 ms, yet `apply-sleep` (a waiter 1–2 indexes behind `applied`) fired 477× on fcstor003 (g0 leader; 52 of them 100–149 ms), 652× on fcstor004 (6 at the 400 ms deadline = `wait_timeouts=7` = the four `report-split … finish_ms=430–1024 rc=-13`; the client retried those and they passed, so no test failed on them this time — under load they are the fsync EIO of the earlier runs), 511× on fcstor006 (g2 leader). `raft-obs`: leaders' lanes `tx->1 hi=651`, `tx->2 hi=244` (fcstor003), `hi=519`/`92` (fcstor006); every follower `hi=1`. Sender thread 982200 on fcstor003 (`strace`): per frame `poll` → `read(eventfd)=8` → `fstat(sock)` → `read(eventfd)=EAGAIN`, ~230–440 µs per frame, 14–18K syscalls/s for the whole posix burst, ~154K frames for ~5K group-0 commits. `rtt_avg=440us` is that loop's period on every host, not the wire. A 651-deep lane × ~230 µs is the 149 ms max. Successful one-record REPORTs took `finish_ms=133–139` for the same reason. This is also `dir_readdir_while_unlink` (15 s timeout: it creates 40 one-byte files serially, each a CREATE + a close REPORT at `finish_ms` ~135, then scans) and the 84 s posix wall (client `work_s=1.25` in 120 s: the client only waits). The same class showed at mkfs time: both `raft-mkfs` calls returned `rc=-13` (the first after proposing index 2 on a cluster that had been up for 3 s; the second, sent to the leader, `index=0`), yet `raft-status` showed `root=1` and the table served. `clean_cluster.sh` and `~/efs-runs/wipeposix.sh` treat that BUSY as a failure and retry mkfs; they should read `raft-status` for `root=1` instead. Do not widen the 400 ms budget for it.
   - Why the lane is that full: `efs_raft_read_begin` → `broadcast_ae` (`raft.c:1901`), one heartbeat AE to each peer per ReadIndex round, and every GETATTR/LOOKUP/READDIR on the leader is a round when none is pending. ~30 frames per commit. **Decision needed (D14, ask):** serve leader reads without a round while the leader's last heartbeat majority is younger than the election timeout (a lease), or space rounds at a minimum interval. Either is a protocol-level choice; the sender fix above is not and comes first.
   - Also cheap: a follower waiting for a forwarded command's index learns the commit only at the next heartbeat (50 ms). Send an empty AE when the leader's commit index advances and the lane is idle.

3. **A hintless NOT_PRIMARY retries the same node 16 times for 10.3 s. Fix in `inode_rpc.c`: on `why=no-hint` skip the node that answered when re-picking (`raft_voter_conn` is deterministic: first live voter).** fstor007's log: 4 160 `retry … why=no-hint`, 810 `exhausted 16 BUSY/STALE (10.3 s) -> EBUSY` across the day (SETATTR 295, CREATE 247, RENAME_AT 145, LOOKUP 108, GETATTR 47); 5 SETATTR exhaustions on the fresh table with both terms stable, so this is not an election. `set_inode_rc` puts `hint + 1` in `primary_id`; 0 means `hint < 0`, and the client also treats `primary_id == nid` as hintless. Server side, `host_read_index` returns before setting the hint when `!v.has`; find which path answered by logging `rc=-15 hint=-1` at `set_inode_rc` once.

4. **`recv_poller` is a 1 kHz timer. Fix in `rdma.c:614`: after `ibv_req_notify_cq` and the re-poll, block on the completion channel (`poll(…, -1)` or a long safety timeout), not 1 ms.** Idle window: fcstor008 `poll` 1 342 269 in 1 440 s (932/s), `recv_poller` 34 % of the idle client's samples; efsd 1.87 M `poll`/24 min, `recv_poller` 14 % (fcstor003) and 10.5 % (fcstor004) of idle server samples. It is why every idle client wrote 158 MB of strace in 35 min. The notify already guarantees the wakeup; the 1 ms was insurance against the ack-before-wait race that the comment at `rdma.c:563` describes, and the arm-drain-wait order already closes it.

5. **W15.5 `FUSE_CAP_SPLICE_READ` buys nothing; revert it (and the 8 MiB `pipe-max-size` sysctl is then unneeded).** fstor007 ecopy: 24 668 `splice(/dev/fuse → pipe)`, 17 072 of them header-sized; each 1 MiB write is then eight `read(pipe, 128 KiB)` (59 107 such reads) — the same one copy per byte as `read(/dev/fuse)` plus one syscall per request. Profile: `copyout` 5.2 % under `pipe_read`, and ~7 % in `do_anonymous_page` / `get_page_from_freelist` / `clear_page_erms` / `free_unref_page*`: the dcache destination buffers are fresh anonymous pages every time. Second change: a freelist of chunk buffers so a write does not fault + zero + free 32 pages per chunk.

6. **Fragment store syscall shape (server, `store.c`).** From the ecopy window on fcstor003 (120 s): `openat` 149 770 (51 130 ENOENT), `close` 98 638, `fstat` 92 353, `mkdir` 9 304 (8 176 EEXIST), `pread64` 845, `pwrite64` 2 360. Three separate fixes:
   - GET probes the six roots in order (`openat(/data1/01/…/0.1.<gen>) ENOENT` ×4, then `/data1/05` hit). The PUT reply already carries the root; put it in the GET request and open that root first.
   - PUT runs `mkdir("/data1")`, `mkdir("/data1/06")`, … to the leaf on every fragment (≈7 EEXIST per write). Create the inode dir once per `(root, ino)` and `mkdir` only on the create's ENOENT.
   - PUT writes a second file `<frag>.sum` (open + write + close) per fragment: two XFS inode creates per fragment (`xfs_btree_lookup`, `__d_lookup_rcu` in fcstor004's ecopy profile). Store the checksum in the fragment's xattr or a fixed-size header written in the same `pwrite`.
   - The `O_TRUNC|O_DIRECT` probe for an existing fragment (`openat … ENOENT` then `O_CREAT`) still runs for the fstor007 client; check that `EFS_PATH_HINT_NEW` (W14.4b) is sent by every write path.

7. **Pump: `applied.N.tmp` open + write 8 B + fsync + close + rename ~36×/s** (fcstor003 posix window: pump thread 982191 `openat` 5 329, `rename` 4 376, `fsync` 8 943 = 2.4 s of 120). `pwrite` the index in place into one long-lived fd and `fdatasync`, or persist at most every N ms; it is only the restart resume point.

8. **efs-fuse thread count is unbounded under load.** fstor007 ecopy client: 657 `clone3` in 67 s, 664 threads, 0 exits; 109 threads splice from `/dev/fuse`. libfuse 3.10 has `max_idle_threads` only; the put/get pools should be capped, and workers that finish should exit.

Not a finding: `rename_at 1306/a -> 1306/b rc=-12` is ENOTEMPTY for `dir_rename_dir_over_nonempty` (passes). The ecopy's 361 `EXDEV` are `copy_file_range` across mounts (it falls back), its 188 `ETIMEDOUT` are its progress thread's 200 ms `futex` timer, and exit 130 is the user's ^C.

Order: 1 (one line, gates the suite), 2's sender drain, 4, 3, then 5–8; D14 is a question for the user before any lease code.

**Sep 29 17:04Z: wiped and redeployed, posix jobs=1 on fcstor007.** `tests/wipe_cluster.sh` then a fresh `raft-mkfs` (`~/efs-runs/wipeposix.log`). Both mkfs RPCs returned `rc=-13` (BUSY); the table is up anyway. Build `2dd77dac881c-dirty`, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, servers and clients fcstor003–015 started with `--perf --strace`. Group 0 leader 0 term 4 commit==applied 6015; group 2 leader 3 term 1 commit==applied 4797. All 13 mounts `fuse.efs-fuse` + `MOUNT_OK`. Posix jobs=1 on fcstor007: **194/201**, 6 fail (5 EIO, `dir_readdir_while_unlink` 15 s timeout), `mmap_write_read` SKIP, 83.9 s (`results/posix/20260929-170457`). Recorders: `/tmp/efs-perf/efsd.data` + `efsd.strace` on fcstor003–006, `fuse.data` + `fuse.strace` on the clients. Background `edelete` of the renamed-aside trees is still running. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 16:09Z: servers restarted, clients not mounted.** `tests/roll_efsd.sh --all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1` and `--perf --strace` (`~/efs-runs/rollw34.log`). All four built `2dd77dac881c-dirty` (commit `2dd77da` plus the uncommitted W21.2 / W17.3 / W15.5 tree), efsd up, `perf=1`, `strace=1`. Group 0 leader 0 term 9454 commit==applied 13775244; group 2 leader 3 term 2925 commit==applied 12187894. `efs-fuse` is 0 on fcstor003–015. Recorders are `/tmp/efs-perf/efsd.data` and `efsd.strace` (the strace files were already 0.8–1.5 GB two minutes after start; stop them with SIGTERM). The 1 s start line printed `perf=0`; a later check is `perf=1`. Next is 10's one IOR, then 7 step 2 (c) (measure, then ask), then 11. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 15:40Z: W21 step 2 and W17 step 3's tests are in the tree; W15 step 5 is blocked on a sysctl.** `efs_export_staged_bytes` is one load of the root's `staged_total`, kept by `staged_refresh` at every capacity change (W21). `test_chunk_deltas` covers replay-of-folded-span, span-after-fold, overlap-after-fold, and a full image that must name the current base (W17.3); its old "trailer gone" assertion predated D1's tombstones and is replaced. W15.5 (`FUSE_CAP_SPLICE_READ`): `fs.pipe-max-size` was 1 MiB and libfuse needs `max_write` + 4 KiB = 4 MiB + 4 KiB to use the pipe; the user raised it to 8 MiB on all 15 hosts at 15:45Z (runtime sysctl, reverts on reboot), and `efs_fuse_init` now asks for the capability. The first client profile after the roll should show `fuse_copy_page` unchanged and the caller-less `memmove` gone. Committed through `2dd77da`; these four are on top, uncommitted.

**Sep 29 15:15Z: review of the last 24 h of changes (bb338f2..a53b253 plus the dirty tree). Six fixes in the tree, not rolled, not gated.** (1) `kv_flush_locked` still refused a memtable with more than 256 runs (`nneu >= 256`) after range 0 was split into 256 subranges; with 512 possible ranges that flush returned BUSY on every attempt, D9 kept the memtable in RAM, and a snapshot flush was BUSY too. Cap is `KV_RANGE_N`. (2) `kv_seg_data_bytes` walked every block index entry (8 KiB blocks: 230K for the old range-0 L1) and the compactor called it per L1 file and `kv_l0_bytes` once per range inside the 512-range loop, under `l->mu`; the byte count is now fixed at `kv_seg_open` and the L0 sum is taken once per compact. (3) The D13 fold's output was installed at the head of L0, ahead of a same-range file flushed during the merge; a lookup would have returned the folded, older value. The output now goes where the newest input was. (4) The compaction iterator's 1 MiB readahead returned `EFS_ERR_IO` for a block over 1 MiB (one value can be up to 16 MiB); that read is now whole. (5) `maybe_prefetch` called the layout-miss path for every prefetch; that path re-pulls its window every 200 ms even when the maps are local. It now asks only when the chunk's map is absent. (6) `dcache_flush_slot_inner` dereferenced its saved chain pointer after re-taking the lock; a concurrent drop frees chain nodes. It re-finds `(ino,ci)` after the GET and PUT and restarts the walk from the head if the node is gone. Also: W25's `lane_bits` is written by `pack_utimens_cmd` (was patched into the buffer after packing); two `test_kv_lsm` assertions that encoded the deleted file-count rule follow the byte rule. Nothing else in the 24 h diff needed a change: W23's pool generation and 1 s probe skip, W24's outer deadline, D2's parallel windows, D9/D10, 8h, W17.1, W22 are as START-HERE describes them.

**Sep 29 14:50Z: the shard-I/O lock is no longer held across a fragment GET. In tree, not rolled, not gated.** `dcache_flush_slot_inner` drops `shard_io` with the dcache mutex before `efs_client_fetch_published_chunk` and `dcache_put_now`, and takes it again to install the result. The entry is clean before the drop (body stolen, or `dirty` cleared), which is the same order `dcache_steal_dirty` already uses, so a second flush of that chunk does not read the same merge base. `ll_fsync` of another file on the shard no longer waits out the 30 s recv. Cluster is still stopped (13:53Z). Next is 10's one IOR, then 7 step 2 (c) (measure, then ask), then 11. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 14:20Z: D13 (8j) is in the tree, not rolled, not gated.** Over the file cap, and only when no range already meets the 1/8 rule or the 1 GiB byte cap, `kv_compact_locked` merges the range with the most L0 files (at least two) into one L0 file and does not open that range's L1. Tombstones stay in that file, because L1 is still there. The compactor loop already repeats while `n_l0` is over the cap. A range with a single L0 file is left alone. The 1 GiB byte cap still rewrites L1; that is how bytes leave L0. The pump does not wait. Cluster is still stopped (13:53Z). Next is the shard-I/O lock held across a fragment GET, which this change does not touch. Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.

**Sep 29 14:05Z: the 13:08Z perf+strace run is reduced. Next work is D13 (8j). Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT.** Cluster and clients were stopped at 13:53Z (`~/efs-runs/stop32.log`) so the recorders could finish. Clients exited on SIGTERM. The four `efsd` did not, and were killed after their recorder children were signaled; `perf report` still opens the files. Nothing is running. Files, node-local: `/tmp/efs-perf/` (`efsd.data` 183–299 MB and `efsd.strace` 13–20 GB on fcstor003–006; `fuse.data` 68–72 MB and `fuse.strace` 3.1–3.2 GB on fcstor009 and fcstor010, about 1–2 MB / 180–210 MB on the other clients). Reduction: `results/measure/20260929-130800-ddposix/ana`. The window is 09:17–09:50 EDT: jobs=1 posix on fcstor007 and fcstor008, and two clients each `dd bs=1M conv=fsync` of two 100 GiB files. About 30 % of every server sample is `ptrace_stop` because strace was attached. That is the tracer. The numbers below are the rest.

The copies each transferred 100 GiB and then `fsync` returned EIO. fcstor009's fuse log: two `report-loop`, both `rounds=1 busy=1 rc=-13`, the longer 39.4 s. fcstor010: three, max 39.1 s. posix under that load was 192/201 and 194/201, the fails EIO, `mmap_write_read` the only skip. The client is off-CPU. On fcstor009 the window's non-wait syscalls are 141 s; `recvfrom` max is 30.5 s; `write` is 4.9 M calls and 57 s. On-CPU, blake3 is 28.7 % self inside `dcache_flush_slot_inner` / `dcache_reclaim_main`. Do not spend a change on blake3. fcstor007's posix window is 1.1 s of non-wait work and `recv_poller` is the top of that profile. Do not put the yield loop back.

What the servers did:

- **The BUSY is still `host_read_index` before pack, and a few pushes block for much longer.** fcstor004 `report-split`: 184 `rc=-13`, 59 `rc=0`. `pack_ms=0` on 172 of the 184. `finish_ms` on those BUSY lines averages 656 ms and maxes at 5.5 s. `push_ms` averages 5.4 s, and 12 of them are ≥ 1 s, one **206 s**, inside `host_pub_batch_push`. The successful lines have `push_ms=0` and `finish_ms` ≤ 392. `backpressure=0` on every server, so L0 bytes never reached 1 GiB and D9's admission did not fire. Do not widen `HOST_READ_TRIES`.
- **D12's backstop rewrote L1 for the whole copy.** It compacts whichever range has the most L0 files as soon as `n_l0` is over 4, and the count stayed over 4 for the entire ingest, so the 1/8 rule never applied. fcstor004: 425 compacts, **67.5 GB**, max 17.2 s, L0 peaked at **379** and was still 36 at the stop (L1 448). fcstor003: 770 / 75.5 GB, max 7.1 s, L0 peak 95, ended at 3. fcstor005: 431 / 67.5 GB, max 25.4 s, L0 peak 360. fcstor006: 754 / 75.9 GB, max 9.0 s, L0 peak 114. An average compact is about 159 MB. That is a fat L1 rewrite, which is the amplification D10 had removed.
- **A point get still binary-searches the files that rewrite did not remove in time.** fcstor004 work-window self time: `kv_seg_probe` 8.3 %, `__memcmp_avx2_movbe` 10.2 %, `lookup` 3.4 % self / 23 % inclusive, `efs_meta_apply_get_chunk` 18 % inclusive, `host_pump` 15 % inclusive. Span skip only rejects a key outside a segment. Every L0 file of the same range is inside the span, and there were hundreds of them. `apply-sleep` hit 400 ms on 599 of 655 group-0 sleeps on fcstor003 (247 s summed) and on 547 of 611 group-2 sleeps on fcstor006 (232 s). fcstor006's CPU profile is `openat` / `writer_thread` (17 %) and XFS, not lookup: the same deadline, waiting on a disk that is also writing the 67 GB.
- **The fragment create is the other server CPU, and it is not the EIO.** fcstor003 in the window: `openat` 7.52 M / 387 s, `access` 85 k (D7 held), `fstat` 2.7 M, `pread64` over 1 MiB 143 k (the merge readahead is in use). `fsync` 37 488 calls on fcstor003 (115 s, average 3.1 ms) and 68 960 on fcstor004 (114 s); the longest on any server is 3.8 s. The 100 ms and the 3.6 s outliers are the compactor's segments under that 67 GB, not the Raft log. Do not move `mdraft/`. `raft-snap` fired 4 / 8 / 8 / 6 times. Do not go back to snapshotting every 256 entries.

**How to fix it.** D13 is in the tree (14:20Z), not rolled. When the file count is over the cap and no range meets the 1/8 rule, merge that range's L0 files with each other and do not read its L1. The 1/8 rule, and the 1 GiB byte cap, stay the paths that rewrite L1. That drops the count the probe walks, and it stops the 67 GB that makes the apply miss 400 ms. The shard-I/O lock held across a fragment GET (the 12:36Z IOR hang) did not show up in this run: these fsyncs returned. That drop is in the tree (14:50Z), not rolled.

**Sep 29 12:50Z: 8h and D12 are rolled (12:29Z, no perf, no strace) and the 9×4 debug IOR did not finish.** `results/io500/20260929-123635-rdma`. Stonewall is 1 s, same shape as the Sep 28 runs, so every number is `[INVALID]`. ior-easy-write **2.218 GiB/s** (19.720 s) and mdtest-easy-write **1.765 kIOPS** (2.739 s) completed. ior-hard-write printed 31 `fsync failed` and no bandwidth; rank 28 on fcstor014 called `abort`. No later phase, no SCORE. fcstor004's L0 file count ended at **3** (D12 held; it was 310 on the 05:31Z trace) with L1 still 397. `report-split` on that node was 23 `rc=0`, 73 `rc=-13`, 3 `rc=-14`, and there were 1582 `apply-sleep` lines, so the 400 ms apply wait is still the BUSY. Do not widen it, and do not retry the one-shot REPORT.

The hard-write hang is a different wait. On fcstor012, `ll_fsync` of the shared file (ino 2595379) blocked on the shard I/O lock while `dcache_reclaim_main` held that lock across `efs_client_fetch_published_chunk` (chunk 170279). The two fragment GETs sat in `efs_rdma_recv_wait` (`timeout_ms=30000`) against nodes 3 and 4. That client's four ranks went D (`request_wait_answer` / `fuse_flush`) and ignored SIGKILL until `efs-fuse` was killed; the client was remounted at 12:50Z (`FUSE_OK`). The earlier fsyncs on that inode were STALE (`rc=-14`) and then EIO, not the L0 admission. Cluster is up, `a53b253f2455-dirty`, RDMA, no recorders. Group 0 leader 0 term 9434 commit==applied 13756802; group 2 leader 1 term 2919 commit==applied 12169621 (at the 12:32Z roll).

**Sep 29 12:02Z: the 05:31Z trace is reduced. 8h and D12, named below, were the next work and are now in the 12:29Z binaries.** Do not widen the 400 ms apply wait, and do not retry the one-shot REPORT. Cluster and clients were stopped at 12:02Z so the recorders could finish (`~/efs-runs/stop31.log`). Clients exited on SIGTERM. The four `efsd` did not, and were killed after their recorders were signaled; `perf report` still opens the files (IOR slice on fcstor004: 155K samples, lost 0). Nothing is running. Files, node-local: `/tmp/efs-perf/` (`efsd.data` 89–150 MB and `efsd.strace` 7.2–7.4 GB on fcstor003–006; `fuse.data` 8–13 MB and `fuse.strace` 1.8–1.9 GB on fcstor003–015). Reduction: `results/measure/20260929-053100-w30trace/ana`. The recorders ran 01:31–08:02 EDT. The only write in that window is the 01:46 EDT IOR (05:46Z), which aborted; the profiles below are that 01:45:30–01:52:30 slice, not the six idle hours.

No bandwidth. Same abort as 00:40Z and 02:35Z: every client `report-loop rounds=1 busy=1 rc=-13` (one `stale=1 rc=-14` on fcstor008), walls 8–26 s, then `fsync`/`close` failed. fcstor007's strace is one `recvfrom` of 25.9 s. The client is off-CPU. On-CPU, blake3 is 20.8 % and `memmove` 6.6 %; `fstat` is 126 calls and `getsockopt` 2931 (W23's probe cut held). `recv_poller` is 5 % of the IOR samples and the top of the idle slice, which is ~2 % of one core. Do not put the yield loop back, and do not spend a change on blake3.

What the servers did, fcstor004 unless said (60 `report-split` lines: 54 `rc=-13`, 2 `rc=-15`, 4 `rc=0`):

- **The BUSY is `host_wait_applied`, before pack.** `pack_ms=0 push_ms=0` on the `rc=-13` lines. Each one is preceded by `apply-sleep us=400xxx group=2`: commit ran ahead of applied by about 30 entries (idx 12148239, applied 12148214) and the wait hit its 400 ms deadline. Group 2 hit that deadline on 53 of 60 sleeps; group 0 on 9 of 35 (median 53 ms). This is not D9. There is no `kv-compact: backpressure` line on any server, so L0 bytes never reached 1 GiB and `host_pub_batch_propose` did not refuse the batch. The four `rc=0` lines have `pack_ms` 393–612. The two `rc=-15` lines (`push_ms` 5551 and 5721) sit inside the one snapshot below.
- **D10's byte rule held, and the file cap it removed did not.** 34 compactions wrote **0.254 GB** (003: 14 / 0.123 GB, 005: 38 / 0.257 GB, 006: 12 / 0.075 GB). The longest is 1161 ms. `inputs=2` or 3, output 0.5–1 MB, 10–14 ms, ranges 322 and 503. L0 file count went **26 → 310** on 004 (24 → 149 on 003, 27 → 327 on 005) and L1 ended at 353. `kv_compact_locked` returns BUSY unless a range's L0 bytes are at least 1/8 of its L1, so the compactor only rewrites ranges whose L1 is still tiny, one range per turn, and the flush adds files faster than that removes them. `KV_LSM_L0_DEFAULT` is 4; waking at 4 and then returning BUSY is how the count reaches 310. The pump did not wait on any of these (D9 held). `fsync` in the IOR window: 3090 calls, 1.40 s total, max 0.091 s (D11 held). `pread64` over 1 MiB: 6304 (the merge readahead is in use).
- **A point get walks every one of those files, on the pump.** IOR-window inclusive profile: `host_pump` 16.0 %, `host_apply` 15.0 %, `efs_meta_apply_publish` 14.9 %, `efs_meta_apply_get_chunk` 9.0 %, `lookup` 17.9 %, `kv_seg_probe` 11.8 %, `__memcmp_avx2_movbe` 10.1 % self / 19.0 % inclusive. `lookup` probes `n_l0 + n_l1` segments (310 + 353 by the end) and `kv_seg_probe` binary-searches each. That is the apply the 400 ms wait is timing out on. `writer_thread` is the other 15 % (`openat` 855K calls / 108 s in the window — the fragment creates). `access()` is 10 237 against those 855K opens (D7 held for this write). `vx_sift_down` is 2.2 %; `efs_kv_lsm_view_export` is under 0.5 %.
- **W22.1 fired once, not every 256.** One `raft-snap` on group 2: 2.82 GB, 14.1 s. The 54 BUSYs start before it. Do not go back to snapshotting every 256 entries.

Compared with D10 as written: "let L0 files accumulate, the cap that matters is bytes, reads already probe every L0" is the sentence this trace falsifies. The bytes are fine. The probes are the abort. Fixes are 8h (skip a segment whose key span misses, no decision — the range split already exists) and 8i / D12 (when the file count is over the cap, compact the range with the most L0 files even under the 1/8 ratio, until the count is back under the cap). The 1/8 rule stays for the steady state, so a fat range is not rewritten to absorb a few KB. The pump still does not wait.

**Sep 29 04:08Z: W22.1, W22.2, W14.4b, the `--meta-storage` flag, and `pub_p50` are on the cluster.** Rolled `--all` (`~/efs-runs/rollw22.log`), build `54a500da9dc8-dirty`, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, servers and clients fcstor007–015 started with `--perf --strace`. Group 0 leader 1 term 9418 commit==applied 13687019; group 2 leader 1 term 2902 commit==applied 12107762. Clients fcstor007–015 remounted in the same screen, and fcstor003–006 (which also run `efs-fuse`) remounted at 04:13Z (`~/efs-runs/fuse36.log`), all `fuse.efs-fuse MOUNT_OK`. Recorders ran until 04:27Z and were stopped for the analysis below (the perf children are zombies until the daemon's `stop_recorder` reaps them; `pgrep -x perf` still counts them). Files, node-local and final: `/tmp/efs-perf/efsd.data` (28–42 MB) and `efsd.strace` (2.4–3.7 GB) on fcstor003–006; `/tmp/efs-perf/fuse.data` (0.4–0.6 MB) and `fuse.strace` (65–96 MB) on fcstor003–015. At 04:11Z every host had `perf=1` and `strace=1` (fcstor005's `perf=0` in the roll log was the 1 s `pgrep` racing the attach). No IOR this roll; a user `ecopy` ran 04:15–04:22Z and is what the next block analyzes.

What landed: a group snapshots when command bytes past `snap_idx` reach 512 MiB (`EFS_RAFT_SNAP_BYTES`) and keeps that much of the log (`log_base`); `send_ae` sends InstallSnapshot only when `next_index` is below the window. An import diff is applied 1024 keys per pump cycle (`HOST_SNAP_SLICE`) and the last-chunk retry stays BUSY until the cursor finishes. `path_hint = 0xffffffff` (`EFS_PATH_HINT_NEW`) is the first PUT of a fragment; the server skips `access()` and creates on the least-queued root; a retry sends 0. `efsd --meta-storage <root>` exists and defaults to the first `--storage` root — `mdraft/` was not moved. `raft-obs` prints `pub_p50` / `pub_max` (propose→apply of the publish batch). Not done: D2's parallel chunk-map windows, the D8 IOR that reads `pub_p50`, the D6 shared-vs-quiet `fsync` measurement (ask which device before any move).

**Sep 29 04:07–04:27Z: perf + un-narrowed strace on every daemon across a user `ecopy` (`results/measure/20260929-040800-idle-trace/ana`, per host `perf-<h>-{efsd,fuse}-{flat,threads,callers,children}.txt`, `strace-<h>-{efsd,fuse}-{summary,tids,tidcalls,persec,long}.txt`, `idle-detail-<h>.txt`, `idle-detail2-<h>.txt`, `strace-fcstor00N-efsd-{openat-paths,mkdir-paths,rename-unlink,top-tid-*}.txt`).** The recorders from the 04:08Z roll were stopped at 04:27Z for this analysis (perf must finalize the file); they are not running now. Something wrote through the cluster from 04:15 to 04:22Z (`.ecopy.tmp.*` names in `unlink-simple` lines; 408K fragment `O_CREAT` opens on fcstor003 and on fcstor004, 120K of them in the first minute); none of it went through fcstor007–015's FUSE (125 `read`, 3 `recvfrom` per client in 19 minutes), so the client profiles are idle-only. No IOR was run.

What the servers did, fcstor004 unless stated (voter in both groups, group-2 leader until 04:16:17Z):

- **W22.1 held:** zero `raft-snap: start` on all four nodes in 20 minutes (the 02:35Z run had one every 256 entries). No import, no `efs_kv_lsm_view_export` in any profile. `disk_log_new_bytes`, the byte counter W22.1 added, walked the whole retained log under `d->mu` on every pump tick: 1.0–1.25 % of every server. Fixed in tree (running `bytes_all`/`bytes_old` in `raft_disk_group`, O(1)); not rolled.
- **The compactor is the server.** 48 % of fcstor004's samples and 55 % of fcstor005's are one thread: `kv_compact_locked` (`cm_sift_up`, `__memcmp_avx2_movbe`, `cm_pop`, `kv_seg_probe`). 433 compactions on 004 in 20 minutes (441 / 648 / 656 on 005 / 003 / 006), `bytes=` sum **159 GB** (159 / 124 / 124 GB) against a 5.3 GB table (100 L1 + 3 L0 files at 04:44Z). Each compaction takes one `key[0]` range's L0 files (`inputs=` 5–11, 33 once) and rewrites that range's L1: ranges 1–15 average 190–330 MB per rewrite; **range 0 averages 1.8 GB** (34 rewrites = 61 GB of the 159). `kv-compact: backpressure n_l0=55..64` appears throughout the write window. The thread does 5.25M `write(…, 4096)` (stdio's default buffer; fixed in tree with `setvbuf` 1 MiB in `kv_seg_w_open`, not rolled) and 3.7M `pread64` of 7.5–8.2 KB (one block per call, the merge's input reads).
- **The pump blocks on the compactor. That is the election trigger now, and the REPORT BUSY.** Pump tid 881545's `futex` waits during the write window: **4.14 s, 1.68 s, 24.23 s, 2.70 s, 3.57 s** (04:16:13–04:16:54Z), each matching a `kv-compact: end ms=` of the same length (4139, 24233 — range 0, 1.77 GB — 2696, 3574). That is `kv_maybe_flush_locked`'s `while (n_l0 + KV_LSM_RANGE_MAX > KV_LSM_MAX_SEGS) pthread_cond_wait(&l->cv)`: the apply path cannot place a memtable while L0 is within 16 files of the 64 cap, so it waits for the compactor's publish, on the pump, under `h->mu`. `raft-obs`: `pump_hold_max=4438191us apply_max=4438134us applies_in_worst=5` on 004 (history 6.4 s, 5.8 s, 2.5 s, 2.0 s), 2.14 s on 005. While held: `raft-host: apply-sleep us=400xxx` (the 400 ms `host_wait_applied` deadline) → `report-split nrec=76757 … finish_ms=1881 rc=-13` (12 of 116 REPORTs on 004 non-zero rc; a client's `fsync` would have returned EIO under W17.1); `get_max=2018ms` on `tx->2`; and the terms: group 0 9418→9421 at 04:15:43Z (fcstor003 stepped down and was re-elected), group 2 2902→2904 at 04:16:17Z (fcstor004 stepped down to fcstor006), both inside the pump holds. W13 step 2 called this wait "the only stall left, and it means the compactor is 16× behind (log it)"; it is now the stall.
- **The 100 ms `fsync` is the compactor's own segment, not journal sharing.** Per thread on 004: the pump's 61 668 `fsync` (Raft log + `applied.N.tmp`) average **0.39 ms**, 61 228 under 2 ms, 2 over 90 ms; the compactor's 695 average **51 ms**, 299 in 90–120 ms. 003: pump 0.40 ms, compactor 36 ms (141 ≥ 90 ms). 005: pump 0.38 ms, compactor 62 ms (407 ≥ 90 ms). A 200–300 MB segment written a moment earlier and then `fsync`'d at 2–3 GB/s is ~100 ms; that is the mode. The Raft commit path's `fsync` is not slowed by the sharing D6 assumed. What the sharing does cost is the compactor's ~130 MB/s of continuous segment I/O on `/data1/01` while it is also one of six fragment roots.
- **D8's number:** `pub_p50=3109us` in 17 of the 5 s samples with traffic, `46511us` in 4, `8051–8665us` in 2; `pub_max` 0.80 s and 2.36 s in the election minute. The per-batch commit is a ~3 ms Raft round when the pump is free; the 29–46 ms and the seconds are the pump held by the flush wait above, not queueing behind other clients and not the log `fsync` (0.4 ms). No wire change is indicated; remeasure after the compaction fix.
- **D7 held:** 26 265 `access()` on 004 for 408K fragment creates (was 1.59M per IOR); the rest are re-PUTs and probes.
- Smaller: `persist_applied` is `openat`+`write`+`fsync`+`rename`+`close` per group per pump cycle with applies (32 769 `openat`, 31 809 `rename` on the pump; 25 459 `fsync` in minute 04:19 = 9.15 s of that minute on the pump). `mkdir_p` on a new chunk directory issues eleven `mkdir` from `/data1` down (39 680 calls for 3 248 new directories, 1.7 s total). Neither is a stall.
- **Clients (idle):** `recv_poller` is 930 `poll(…, 1 ms)`/s waiting on the CQ channel, 3.7K samples in 19 minutes = 0.3 % of one core. Nothing to fix; do not put the yield loop back.

Compared with what this page said before this trace: W13's back-pressure wait was described as a bound that means "16× behind" and would be logged — it fired five times in one 7-minute write on one node, once for 24 s, on the pump. W22's source said "the import-diff apply is the only multi-second pump hold left" — with the import gone, the flush wait is the multi-second pump hold, and the same client symptom (BUSY → `rc=-13` → fsync EIO) and the same term changes follow from it. D6's premise (the 100 ms `fsync` mode is the shared XFS journal slowing the Raft log) is not what the per-thread numbers show; the Raft log `fsync` is 0.4 ms. D8's "is it ~3 ms or ~100 ms" is answered: ~3 ms, with a tail that is the pump hold. The recommendations are D9–D11 in §1a's decision table and rows 8e–8f in the order table.

**Sep 29 02:35Z: IOR with perf and strace on the outbox / multi-chunk / post-and-return tree** (`results/io500/20260929-023447-iorperf2`). No bandwidth. Same abort as 00:40Z: `report-loop rounds=1 busy=1 rc=-13` (18–19 s on fcstor007), fsync EBUSY, `close` failed, rank 3 `MPI_ABORT`. Ranks were gone by 02:41Z. Recorders were SIGINT'd then; files are node-local `/tmp/efs-perf/` (`efsd.data` 28–33 MB and `efsd.strace` 1.9–2.4 GB on fcstor003–006; `fuse.data` 4.1–5.0 MB and `fuse.strace` 241–245 MB on fcstor007–015). The client recorder attached to the daemon (fuse.log has `perf recorder pid=`). The strace was not narrowed, which is why the server files are ~2 GB.

Analysis (`…/ana/`, per-host `perf-*-{flat,callers,children}.txt`, `strace-*-{summary,long,persec,tids}.txt`, `log-*.txt`, `leader-detail-fcstor004.txt`, `pump-detail-fcstor004.txt`): the client's 19.5 s REPORT was one TCP `recvfrom` of 19.36 s (tid 813056 at 22:38:58.52) — no client retry; the group-2 leader fcstor004 held it: `report-split nrec=86234 pack_ms=1431 push_ms=9869 finish_ms=9227 rc=-13`. fcstor004 answered 77 REPORTs: 43 OK, 18 BUSY, 15 NOT_PRIMARY; fcstor005 answered 32 as a transient leader (27 BUSY). Group 0 term 9370→9415 and group 2 2792→2897 across the run, with `drop=0` on every `raft-obs tx->` line on all four nodes (W14.2 (a) gate half met: no drops, terms still move). Every node exports a 2.67 GB snapshot every 256 applied entries (`HOST_SNAP_MIN`), 4.6–12 s each, back to back (`efs_kv_lsm_view_export` memcmp is the top user symbol on all four; 14 % of fcstor004's samples on the GC thread), and followers behind by more than that install it: fcstor006 imported group 2 twice (`diff n=130398 ms=16467`, `n=685534 ms=12132`), fcstor003 group 0 once (`n=380716 ms=12041`). The compactor is fcstor004's top thread (22 % of samples, `kv_compact_locked`), with its segment `fsync` at a flat 100 ms while 20 writer threads create fragments on the same XFS (`persist_max` 254 ms). Client: blake3 15.7 %, `memmove` 5.5 % of which 3.1 % is the RDMA send copy — the `fuse_buf_copy`/`ll_write_buf` copies are gone (W15.3 multi-chunk landed); `send_buf_pick`/vDSO spin gone (W15.4); 28K samples in ~4 min = mostly off-CPU. `access()` on fcstor004: 1.59M calls, 192 s across six handler threads (first PUT of every chunk misses the W14.4 hint). Both profiles carry ptrace overhead from the concurrent strace (`ptrace_do_notify` 4–5 %).

**Sep 29 00:40Z: first IOR on the W17–D2 tree
(`results/io500/20260929-002758-wimpl`, `bbcbcb5ad779-dirty`
rolled 00:28Z RDMA, clients remounted 00:39Z, perf on every daemon).**

No bandwidth. 9-client `run.sh ior` (30 s stonewall, mdtest off)
printed the usual `stonewall-time 30s != 300s` INVALID, then eight
`fsync` warnings, `close(20) failed`, rank 2 `MPI_ABORT`; the ranks
were gone by 00:41:33Z (no D-state this time — W17.1 did what it
was specified to do). Every client logged one `report-loop … rounds=1
stale=0 busy=1 rc=-13`, wall 8.2–26.1 s: the first sync REPORT came
back BUSY and the loop did not retry. The old loop retried BUSY up to
8 times, which is why the Sep 28 run's fsync succeeded after 328 s
and this one returned EIO in under 30 s. The 8 s is checked between
attempts; the extra is one in-flight RPC that the server holds (the
`report-split nrec=90016` lines on fcstor004: nine `rc=-13` with
`pack_ms=0`, then six `rc=0` with `pack_ms` 0.86–2.77 s). The
`dual type=67 exhausted 16 BUSY/STALE retries (10.3 s)` lines are
the close-kicked `report_dirty_ino(0)` path, which sets no deadline.
The server-side BUSY on a 90016-record REPORT is W16 steps 2–3
(attribute, then ask before touching the 400 ms / 10 s budgets);
`fsync` returning EBUSY-mapped EIO on it is the decided behaviour.

Profiles from that window (~100 s of writing; overlaps compaction
and a group-2 snapshot export; `cycles:P`, lost 0; trees under
`~/orcd/scratch/efs/perf/fcstor00N/`). Leaders were fcstor003
(group 0) and fcstor006 (group 2); fcstor004 was a follower in both.

| where | samples | now | was (Sep 28 run 3) | reads as |
| --- | --- | --- | --- | --- |
| fcstor003, group-0 leader both runs | 79K | `memmove` self 4.2%; `send_ae` 2.5% (the one log→frame copy); `host_send` under the 1.5% floor; `try_commit` self 0, 2.5% children all `host_apply`; `host_sender` 5.8% | `memmove` 18.5% (12.6% `host_send`←`send_ae`), `try_commit` 4.0% self / 10.5% | **W19 landed.** |
| fcstor007 client | 14K ≈ 29 CPU-s in ~100 s, so mostly off-CPU | `dcache_flush_slot_inner` self 0.1%, `pthread_once` absent; `dcache_reclaim_main` children 49.7%, all `dcache_put_now` (blake3 ~17%, RDMA send 4.7%) | `dcache_flush_slot_inner` self 43.8%, `pthread_once` 25%, mutex 12% | **W18 landed.** Reclaim now spends its time on the PUT itself, which is the work. |
| fcstor007 client | same | `memmove` in `ll_write_buf` 8.6% | 7.5% | W15 step 3, open |
| fcstor004, follower, REPORT target | 127K | `open` 11.9% + `access` 8.7% + `write` 6.9% on the fragment PUT (`open` callers are unresolved frames whose bytes decode to the six `/data1/0N/efs` roots); `memcmp` 9.2% self in `lsm_get` under `efs_meta_apply_get_chunk` (REPORT pre-check) and publish, in `kv_compact_locked`, and in `efs_kv_lsm_view_export`; `kv_seg_probe` 3.1%; `vx_sift_down` 2.2% all export | `memmove` 20.5%, `copyout` 15.4% | W14 step 4, open. The `memcmp`/`kv_seg_probe` share is the point-get and export merge over a large L1 (fcstor006 had ~1000 L1 files at the roll); check L1 file counts before calling it a regression. |

**Before the next run:** nothing is wedged. Clients fcstor007–015 are
mounted RDMA on this build; all four `efsd` are up; perf recorders
are stopped. Run `tests/preflight.sh` (idle gate) first.

---

**What the Sep 28 evening IOR runs established (kept for the
comparison column above).**

Two 9-client `run.sh ior` runs on `bbcbcb5` (and the same tree
uncommitted before it), 30 s stonewall, RDMA, perf attached to every
`efsd` and `efs-fuse`. Neither produced a score. Both finished
easy-write: **1.048 GiB/s** in 397 s and **0.769 GiB/s** in 393 s.
In each, the 30 s of writing is followed by a ~326–328 s `fsync`
(`fsync-split … flush_ms=327885 report_ms=0 rc=0`). IOR's wear-out
makes every rank write as many blocks as the fastest rank managed in
30 s, so the wall is that flush, not the stonewall. Then:

- run 1 (mdtest still on): `create file.mdtest.0.2236 failed (EIO)`
  → abort. `run.sh ior` no longer runs mdtest.
- run 2 (uncommitted tree, 22:00Z): `INODE_LOOKUP` on shard 3745 hit
  the 16-retry BUSY budget → IOR `stat` failed → abort. That is W16.
- run 3 (`bbcbcb5`, 22:14Z): ior-hard on the shared file. Nine ranks
  publish the same inode (1166063); fcstor004 logged
  `apply publish rc=-14` (STALE) for many chunk indexes; on the
  clients `REPORT_CHUNKS` (type 67) hit the 16-retry BUSY budget
  repeatedly, `fsync` returned EIO (`efs_rc=-13`), IOR `close` failed,
  rank 8 aborted, and **io500 on seven clients stayed in D-state
  `request_wait_answer`** after `MPI_ABORT` killed it. That is W17.

What the profiles say (all `cycles:P`, `perf record -F 499 -g`;
reports under `~/orcd/scratch/efs/perf/fcstor00N/efsd-19810/` and
`.../fcstor00N/efs-mount/`):

| where | run | share | what |
| --- | --- | --- | --- |
| client fcstor007 | 3 (917K samples, 40 min) | 44% + 25% + 12% | `dcache_flush_slot_inner` self, `pthread_once`, shard mutex — all on `dcache_reclaim_main` (73% of the client). Blake3 3%. **W18** |
| client fcstor007 | 2 (36K, 5 min; report files since overwritten by run 3) | 27% + 19% | blake3 in `hash_write_fragments`; `memmove` in `ll_write_buf` (7.5%) and the RDMA send (6.3%). W15 steps 1 and 3 |
| fcstor003 (group 0 leader) | 3 (1M) | 12.6% + 5.2% + 6.9% | `memmove` in `host_send` and `send_ae` on the **pump**, then `writev` copy-in on `host_sender` — every AppendEntries byte copied twice in user space and once by the kernel, over TCP. `try_commit` 10.5% (walks `last_i → commit_index` calling `log_term` per AE reply). **W19** |
| fcstor004 (group 0 follower, group 2 leader) | 3 (2M) | 15.4% + 6.3% | `copyout` in `server_handle_conn` → `recv` (the other end of those AEs); `memcmp` in LSM `lookup` under publish apply, view export, compaction |

Gone from the profiles: `send_snap` recopy, `kv_flush_locked` on the
pump (`7eecf1d` + `bbcbcb5`), `efs_rdma_reply_ready_us` + vDSO
(W15 step 2, done in `bbcbcb5`: vDSO 0.87%, the symbol under the
0.5% floor), `recv_poller` (1.2%).

| fstor007 `client.sh --perf` | 20:36–20:39 EDT (214K, 3 min) | 35% + 8.9% + 6.6% | `memmove` (14.6% dcache patch, ~17% `fuse_buf_copy` bounce with no frame pointer, 3.6% RDMA send); `ll_setattr` → `fill_stat_from_inode` walking every chunk of the file (**W20**); `efs_export_staged_bytes` on the evictor, 1024 passes per wake (**W21**). `send_buf_pick` spin (`EFS_RDMA_NSEND`=2) is 4.2% (W14 step 5). Details under W15's source. |
| fstor007 `client.sh --perf` | 22:11–22:14 EDT (32K, 3 min), binary built 22:06 **with** W15.3 / W20 / W21 / `NSEND=EFS_WRITE_PIPELINE`, seven parallel `dd bs=1M` to new files in one dir | 39.5% + 8.2% + 6.9% | `memmove` still two copies per byte (16% `ll_write_buf`, ~19% unattributed `fuse_buf_copy`, 4.4% RDMA send): W15.3 takes the single-copy path only when `size == chunk`, and a 1 MiB write is eight chunks. vDSO 6.9% is `now_us()` + `pthread_spin_lock` in `efs_rdma_send_frame`'s **synchronous send-CQE wait**, not `send_buf_pick`; more send buffers did not move it. W20 and W21 are gone from the profile (dd does no setattr; evictor under cap). One `dd` failed `open` with EBUSY: fuse log has **249** `exhausted 16 BUSY/STALE` lines (LOOKUP 98, SETATTR 50, GETATTR 47, RENAME 31, CREATE 23), every retry `why=no-hint` — elections, not apply lag. Group 0 term 9309→9368 and group 2 2763→2789 since 00:39Z; fcstor004 `raft-obs: tx->3 enq=50698 drop=72114 sent=50695 hi=2048`. Review and fixes under W15, W14 step 2, W16. |
| fstor007 `client.sh --perf` | 22:48–22:55 EDT (260K, 6.5 min, ~1.3 cores average), binary with the multi-chunk W15.3 and the W21 cursor; `dd bs=1M status=progress` to `/tmp/efs-mount/001/dat08`, killed at 1.7 GB | 29% + 12.8% + 9.9% | **W15 step 3 verified:** the `ll_write_buf` `memmove` is gone; `memmove` is 7.2% RDMA send copy + ~22% caller-less `fuse_buf_copy` — one user copy per byte now. vDSO is 0.8% (the send-CQE wait is below the floor on this run; keep W15 step 4's gate). **W21 not fixed:** `efs_export_staged_bytes` 7.4% self + `stage_evict_main` 5.4% self — the cursor limits the bands per wake, but every write over cap *kicks* a new wake, so the wake rate is the write rate. Fix under W21. blake3 9.9%, `xor_into` 5.3%, kernel `fuse_dev_read` 7% are the work. `~/orcd/scratch/efs/perf/dd.trace.txt`: 1667 × 1 MiB `write()` returned 1048576, the 1668th was in flight when `dd` got SIGKILL; no `-tt`/`-T`, so the trace does not say how long that write waited. |
| fstor007 `client.sh --perf --strace` | 00:18–00:22 EDT Sep 29 (75K samples; `fuse.strace` 1.4 GB, 17.5M lines, 374 threads; `ecopy --verify` of `/data1/erbmi1/knouse` plus a `dd` into an existing 1.7 GB file), servers on the 02:35Z roll | ptrace 43% | **The perf half is not usable for shares:** `ptrace_do_notify`/`ptrace_stop`/`do_notify_parent_cldstop` are 43% of samples; user symbols keep their order (memmove 7.2%, blake3 3.8%, `stage_evict_main` 3.1%, W21 again). **The strace half is the finding.** `strace-summary.txt` for 230 s: `fstat` 1 171 561, `getsockopt` 1 172 007 (586 035 `SO_ERROR` + 585 972 `TCP_INFO`), `recvfrom` 598 781 (`MSG_PEEK`), `poll` 1.12M, `read` 1.20M, `write` 584K (`recv_poller` eventfd) — five liveness syscalls per connection checkout plus an `fstat` per send, ~586K checkouts, ~15K syscalls/s of probing (**W23**). `clock_nanosleep` 794 calls / 488.7 s: 564 of them 0.8 s = ~47 RPCs that ran the full 16-attempt BUSY budget in four minutes. Fuse log: 424 `exhausted` (was 249 at 22:19): SETATTR 136, RENAME 106, LOOKUP 98, GETATTR 47, CREATE 37; 2 212 `why=no-hint`; `rename_at … rc=-13 after 9 tries` and `report-loop … busy=1 ms=8435..24182 rc=-13` → `flush-meta: resource busy` (ecopy's fsyncs). `dd.trace.txt`: `openat("/tmp/efs-mount/001/dat08", O_WRONLY\|O_CREAT\|O_TRUNC)` **never returned**; killed (**W24**). Terms since the 02:35Z roll (outbox coalesce in): group 0 9368 → 9421, group 2 2789 → **2904** — 115 elections in two hours with `drop=0`; that is W22's import trigger, not the outbox. `ecopy.strace.txt` traced the main thread only (no `-f`): it waits on its workers until SIGTERM/SIGKILL; nothing else in it. |

Those seven D-state `io500` processes were cleared by the 00:39Z
remount. The `-dirty` suffix on the servers' build id is the node
tree missing tracked `results/` (the deploy rsync excludes it); all
four IDs match, so the HELLO gate is satisfied.

**Sep 28 ~18:30 EDT: `df` on a fresh mount during the 9×4 IOR returned
ENOENT.** Root GETATTR (shard 0, group 2) got BUSY for 10.3 s and the
client maps that to ENOENT. Not data loss. Written up as **W16** in §1a
with the fix order (mapping first, then attribute the BUSY stream with
`raft-status` before touching the server). Start there.

**L1 cap and the snapshot pump (Sep 28 evening, committed `bbcbcb5`).**
The 64-slot L1 array is a growable list. The on-disk MANIFEST was
already one `1 <seq>` line per file. `efs_kv_lsm_flush_nowait` returns
BUSY without walking the memtable when L0 cannot take another full
set of ranges; `host_snap_open` uses that on the pump. A no-progress
InstallSnapshot ack waits one heartbeat before the next chunk, and
the empty AppendEntries still goes out. Abandoned `snap-*.kvx.tmp`
files were deleted while efsd was down. Roll `--all` of
`7eecf1da00cd-dirty` (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`) caught
every voter up inside the script's window: group 0 commit=applied
13616251, group 2 12041271. fcstor004 L0 49→2, fcstor005 58→3,
compaction rc=0, group 0 snapshot import diff n=15697. Unit tests
`test_kv_lsm` and `test_raft` passed on node9901. 9-client
`run.sh ior` (30 s stonewall, mdtest still `run=TRUE` in
`config-ior-only.ini`) finished easy-write at **1.048 GiB/s** in
397.381 s (one fsync flush 325.6 s, rc=0) and then aborted:
`create file.mdtest.0.2236 failed (EIO)`, rank 0 `MPI_ABORT`.
That easy-write is not comparable to the 1 s stonewall 2.917 GiB/s.
mdtest in `config-ior-only.ini` is now `run=FALSE`. A later
`client.sh --perf` IOR (22:00Z, daemons then stopped) produced no
RESULT: fsync `flush_ms=202803` `rc=0`, then `INODE_LOOKUP` on
shard 3745 exhausted 16 BUSY retries and IOR's `stat` aborted.
Committed as `bbcbcb5` and rolled 22:14Z (`bbcbcb5ad779-dirty`
on every server; `-dirty` is the excluded `results/` tree).
That IOR's easy-write is **0.769 GiB/s** in 393.014 s
(`flush_ms=327885`, `rc=0`), then `fsync`/`close` failed and
rank 8 aborted. Publish of ino 1166063 was returning STALE
(`rc=-14`). No score. io500 on seven clients stayed in
D-state `request_wait_answer`.
Profiles from the first run (`cycles:P`): fcstor003 top is kernel
dentry lookup under `nvme_put` (5.6%); fcstor004 top is LSM `memcmp`
in lookup (10.6%). `send_snap` / `kv_flush_locked` are not the stack.
efsd is up; fcstor007–015 are mounted RDMA (with the D-state io500
leftovers noted above).

**9-client dd profile cycle is done (Sep 28 afternoon).** Five
rounds on RDMA, 8 GiB `dd bs=1M conv=fsync`, own file, every file
8589934592. The change that moved the wall was probing for an
existing fragment *before* taking the writer-pool lock
(`results/measure/20260928-131651-dd-prof-r2b`, **2810.5** MiB/s,
walls 25.05–26.23 s). 64 KiB snapshot chunks, a thread-local
directory fd, and a 256-publish / 64 KiB AppendEntries cap each
made the slowest client worse; those three are reverted. Kept:
the probe stays outside the pool lock, and a snapshot chunk in
flight is not resent on every pump wake (`ae_inflight` end is
`UINT64_MAX` until the reply or a heartbeat). Live check after
that restore: **2551.5** MiB/s, walls 24.84–28.90 s
(`results/measure/20260928-134637-dd-prof-r5b`). Do not quote the
hung round (`20260928-102121-dd-prof-r2`). Prior morning RDMA
9-client was 1326.6.

**9×4 IO-500 debug, same binary, TCP and RDMA (Sep 28).**
`SLOTS=4 NP=36 tests/perf/io500/run.sh debug` (1 s stonewall,
IOR easy/hard + mdtest, find off). Servers
`db2b88c4802a-dirty`, preflight idle, every client
`fuse.efs-fuse` + `stat` OK. Reads are same-mount (client
dcache). Official score is INVALID (stonewall is not 300 s).
No data-check failures in either driver log. RDMA easy-write
**2.917 GiB/s**, hard-write **0.291**, easy-read **3.092**,
hard-read **1.525**; mdtest-easy-write **3.418 kIOPS**,
hard-write **0.757**, easy-stat **3.248**. TCP easy-write
**1.376 GiB/s**, hard-write **0.274**, easy-read **2.669**,
hard-read **1.162**; mdtest-easy-write **2.721 kIOPS**,
hard-write **0.910**, easy-stat **16.461**. Dirs
`results/io500/20260928-150609-rdma` and
`results/io500/20260928-151707-tcp`. An earlier RDMA attempt
aborted in easy-write when `INODE_LOOKUP` returned EBUSY
after 16 retries; that attempt is not a result.

**User xattrs are durable (Sep 28, same `db2b88c4802a-dirty`).**
One blob per inode (`EFS_KV_KIND_XATTR` 23), command
`EFS_MD_CMD_XATTR` 27. Only names starting with `user.` are stored;
`security.*` and `system.*` return EOPNOTSUPP with no RPC. The key
is deleted with the inode when it exists. `roll_efsd.sh --all`
exited FAIL while fcstor005 was still receiving the snapshot; both
groups then reached the leaders (group 0 commit 13387007, group 2
11813960). Clients fcstor007–015 were remounted RDMA. posix jobs=1:
**200/201**, 0 fail, `opt_xattr` PASS, `mmap_write_read` SKIP
(`MAP_SHARED` ENODEV; kernel 5.14 has no
`FOPEN_DIRECT_IO_ALLOW_MMAP`), 45.2 s
(`results/posix/20260928-043918`). posix2 alone **63/63** in 65.4 s
(`results/posix2/20260928-044304`). The overlapped posix2
(`results/posix2/20260928-043918`) failed
`peer_overlap_pwrite_chunk_straddle` and
`peer_rename_vs_unlink_src`; they did not reproduce alone. persist
26/26 (`results/posixpersist/20260928-043919`). Four suites at once
on fcstor007 (`POSIX_JOBS=1`, `posixstress 4`): each **200/201**,
0 fail, mmap SKIP only, 46–57 s
(`results/posix/20260928-044951`). The Sep 17 four-suite run
(`results/posix/20260917-120328`) was 165–168 timeouts at 15 s;
that was saturation on the old build, not this one. Do not clear
`direct_io` to make the mmap test pass. fcstor003–006 still run the
previous fuse.

**RDMA recv poller no longer spins (Sep 28).** During posix jobs=1
the shared CQ poller was 64% of `efs-fuse` and 31% of `efsd`
(`results/measure/20260928-050145-posix-prof-rdma`, 147775
cpu-clock samples in `recv_poller`). It paused and `sched_yield`'d
for the whole time a QP was up. TCP on the same suite
(`results/measure/20260928-050841-posix-prof-tcp`) has no such
thread; its top sample is blake3 at 355 hits, and the wall is
45.9 s / 59.0 s. The poller now spins about 100 µs and then waits
on the completion channel for at most 1 ms. Acking the event
before that wait disarmed the notify and added a millisecond to
every RPC (76 s). After the ack moved to after `poll`, jobs=1 is
**200/201** twice, 58.5 s and 57.8 s, 0 fail
(`results/measure/20260928-053033-posix-prof-rdma-fix2`).
`perf trace` cannot read tracefs here. Do not put the yield loop back.

Five more cpu-clock rounds, same morning, jobs=1 on fcstor007,
RDMA, all four servers restarted after each change
(`results/measure/20260928-093100-posix-prof-r1` through
`20260928-100138-posix-prof-r5`). Every suite was **200/201**,
mmap SKIP only. The client top was `clock_gettime` in a 200 µs
`efs_rdma_recv_wait` spin; metadata replies wait on a Raft
commit, so the spin always ran out. That wait is 64 pauses, then
`poll` on the eventfd. The server top was snapshot export: a
linear merge (`memcmp`), then three `write`s per key. The merge
is a heap (newest L0 still wins a tie) and the export buffers
64 KiB. Compaction uses the same heap. With the recv spin gone,
`recv_poller`'s 128 empty CQ polls were 16% of efs-fuse; that
spin is 32. A jobs=1 run after the last roll was **200/201** in
45.1 s. One pair on the recv-wait change was 29.0 s and 29.7 s;
the runs around it were 44–58 s. Do not quote 29 s as the wall.

**Shared-file IOR-hard uses immutable spans (Sep 27, TCP, clients
`8e62ff12b422-dirty`).** A partial publish appends a span object.
The base generation does not move, so disjoint writers do not STALE
each other. A full-chunk fold commits only when the live span list
is the one that image already folded; a longer list is left at seq 0
so the CAS STALEs and the replay refetches. NP=4, SEGS=3000, 47008 B,
cold remount, FUSE_OK: write **481.56 MiB/s** (1.12 s), read
**91.35 MiB/s** (5.89 s), pattern 12000 records bad 0. posix2 on
those clients: **63/63** (`results/posix2/20260927-190509`, 77.7 s).
The prior 4-rank bar was 33 MiB/s
(`results/measure/20260921-162514-ior-hard-scaling`). 1/9/36 were
not remeasured. Group 2 kept committing REAP_DONE of distinct dead
inodes at ~11–16/s, and `tests/preflight.sh` fails above 5 entries/s.
Do not raise that threshold. The dd above was measured on
`75321297f719-dirty` (servers rolled `--all` Sep 27 20:35Z). The
`pub-stale` fprintfs are out of the live efsd. 9-client 8 GiB
dd+fsync is valid: **1478** MiB/s, walls 49.76–49.89 s, every host
FUSE_OK and 8589934592 bytes, zero report `rc=-3`
(`results/measure/20260927-204907-dd-wall`). Preflight was idle.
1-client 977 and 4-client 1984 are build `4c6a5acefe03-dirty` and
were not remeasured here. The 9-client wall is report BUSY retries
(119 of 128 `report-split` lines), 3.4% of the 44 GB/s ceiling.
Servers are now `ab458efab95b-dirty` (`roll_efsd.sh --all` Sep 27
21:11Z, TCP, `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1`). Same-directory
creates no longer each broadcast their own AppendEntries: a leader
with an open fsync hold appends locally, one fsync covers the
burst, and the pump sends one batch. `samedir_rate.sh`
(`results/measure/20260927-211953-samedir-rate`, PREFLIGHT_OK,
storm PASS, parent `children=0 nlink=2`): **333 / 1385 / 1241**
ops/s at 1 / 9 / 36 procs (was 138 / 134 / 159). `busy_n` was 0, 0,
and 1. Idle mkdir median 3.1 ms. Next queue item is W6 residual 2
(1 GiB open).

**19810 is on RDMA (Sep 28, `db2b88c4802a-dirty`).**
`EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`, clients fcstor007–015
remounted, fuse log `RDMA transport up`. A full per-peer outbox dropped
the catch-up AppendEntries and still returned success, so
`ae_inflight` suppressed the retry and fcstor004 group 0 sat 704
entries behind until a snapshot install finished. `host_send` keeps
snapshot chunks and entry-carrying AppendEntries and returns
`EFS_ERR_AGAIN` when it cannot queue; `send_ae` does not mark that
batch in flight. Gate before user xattr: posix jobs=1 **199/201**,
mmap + xattr SKIP, 0 fail, 90.1 s
(`results/posix/20260928-033823`); 9-host **199/201**
on all nine, 56.7–59.1 s (`results/posix/20260928-034049`); posix2
**63/63** in 76.8 s (`results/posix2/20260928-034350`). 9-client
8 GiB dd+fsync that morning was **1326.6** MiB/s, slowest wall
55.574 s (walls 28.55–55.57 s), all nine files 8589934592, no
fsync EIO, zero `rc=-3`
(`results/measure/20260928-033420-dd-wall`). Afternoon cycle,
same shape, after the probe moved off the writer-pool lock:
best **2810.5** (`20260928-131651-dd-prof-r2b`), live restore
**2551.5** (`20260928-134637-dd-prof-r5b`). 99 of 108
report-split lines are BUSY. That is slower than the TCP 1478.
Earlier RDMA build `113823180b15-dirty`: 1-client **947**, 4-client
**2311** (`results/measure/20260928-015839-dd-wall`); that run's
9-client row is INVALID (do not quote ~238). IOR-hard NP=4 on that
earlier build: write **469.55**, read **107.03**, bad 0
(`results/measure/20260928-015806-ior-hard-rdma`); not remeasured
on this server roll. Posix and the 9-client write are not faster
than TCP. Do not roll 19810 back to TCP unless a suite fails.

**Posix suites clean (Sep 27, TCP).** 9-host
`results/posix/20260927-123717`: 200 pass + `mmap_write_read` SKIP
on all nine, 0 not-run (screen 12:37:17–12:37:52Z). posix2
`results/posix2/20260927-123946`: **63/63** in 58 s, and again
63/63 after the span client fix (above). The 9-host EBUSY before
the morning roll was group 2 electing: `send_snap` returns
`EFS_ERR_AGAIN` while the export file is not ready, and sending
nothing let that peer campaign and step the leader down (term +200,
mkdir EBUSY). `send_ae` now sends an empty AppendEntries in that
window. Do not remove it. Same tree, data path: `DCACHE_NR` 32,
truncate keeps `got.generation`, `dcache_note_committed` clears
`nrange` when the slot is clean.

**W13 done (Sep 26).** The user ratified the background-compactor row.
L1 compaction runs on a `kv_lsm` thread. The pump still flushes the
memtable to L0 and kicks the thread when `n_l0 >= l0max`; the merge
opens private segment fds, drops `l->mu` for the rewrite, and installs
under the lock (atomic manifest rename). A segment file is unlinked
only when its refcount hits 0 (`kv_seg_doom`), so `efs_kv_lsm_view_pin`
survives a concurrent flush and compact. `kv_compact_locked` remains
for `efs_kv_lsm_compact` and for the fallback if the thread did not
start. The write path waits on `l->cv` only when `n_l0` reaches
`KV_LSM_MAX_SEGS` (64) and logs `kv-compact: backpressure` — that wait
means the compactor is ~16× behind. `EFS_KV_COMPACT_DIE=N` exits after
the Nth finished output segment, before the manifest rename; reopen
serves the old manifest. Partitioned flush (one L0 file per `key[0]`)
is in the TCP build rolled Sep 27 05:07 UTC.

W13 gate, on the build that was rolled Sep 26 16:37 UTC
(`3210a3d63f73-dirty`, TCP). The cluster at that write-up was
`4c6a5acefe03-dirty`, TCP, after the Sep 27 RDMA rollback. It is
RDMA as of Sep 28 (§1b). Gate:

- Hammer `results/measure/20260926-163709-mkdir-hammer`: idle p50
  **4.68 ms**; 144-way **35166** mkdirs in 15 s, p50 **56.5 ms**.
  `apply_max` **68 ms** on fcstor005 while two compactions rewrote
  ~760 MiB in 1964 ms and 2067 ms (`l1=12`). The only errors are
  harness `rmdir-own ENOTEMPTY`.
- 9-host posix `results/posix/20260926-164123`: **200/201 on all
  nine**, mmap SKIP, **13.2–14.8 s**. Timeline
  `results/measure/20260926-124106-w8-stall-timeline`: group 0 stayed
  term 6882 leader 1, group 2 stayed term 1198 leader 3, for 139 s;
  no probe stat/mkdir/rmdir over 1 s. Four `kv-compact` cycles in
  that window; worst `apply_max` **67 ms**.
- Idle `md_latency.py` on fcstor007 after the suite: mkdir 2.9 /
  create+close 1.6 / append+close 2.0 / stat 0.3 / unlink 0.8 /
  rmdir 2.9 ms.

W11's chunked snapshot is running on this build (see the Sep 27 note
below). The live RDMA switch was tried Sep 27 and rolled back: 9-host
posix was 193–196/201 in 385 s
(`results/posix/20260927-044348`), against the TCP 200/201 in 31 s.
19810 is TCP again (`4c6a5acefe03-dirty`). Partitioned flush is
in this build (one L0 file per `key[0]`; compaction rewrites one
range). The RDMA mkdir gap is diagnosed on the private cluster and
fixed in tree, not rolled: the shared recv poller slept 100 ms after
acking a CQ event (11 of 100 mkdirs), and every SEND called
`ibv_query_qp` plus a sysfs read (~380 µs raft RTT vs ~15 µs TCP).
After both fixes, 100 mkdirs were 642 ms on RDMA vs 507 ms on TCP,
raft RTT ~50 µs (`~/efs-runs/rdmaprof7.log`). A later run's wall was
one 1.5 s mkdir plus three BUSY retries; the other 99 were 2–7 ms.
Do not switch 19810 on that private number.

Sep 27 dd (TCP, 8 GiB `conv=fsync`, FUSE_OK). Morning run
`results/measure/20260927-053506-dd-wall` on `4c6a5acefe03-dirty`:
1-client **977** MiB/s, 4-client **1984**. Its 9-client row is
INVALID (fcstor009 and fcstor013 fsync EIO, report `rc=-3`); do not
quote 1464 from that run. Evening rerun on `75321297f719-dirty`,
preflight idle: 9-client **1478** MiB/s, walls 49.76–49.89 s, all
nine files 8589934592, no fsync EIO, zero `rc=-3`
(`results/measure/20260927-204907-dd-wall`). 119 of 128 report-split
lines are BUSY. That is 3.4% of the 44 GB/s ceiling.
Do not raise `l0_max`, the memtable, or the election timeout. `EFSD_ENV` is
space-separated. Live servers are already `75321297f719-dirty`.

**Txn finisher (Sep 26, `src/server/raft_host.c` `host_txn_commit`).**
A compaction stall longer than the 400 ms apply wait turns an in-flight
DECIDE/RESOLVE into BUSY and leaves the EXCL intent until the 5 s
recovery scan; that intent makes every later create on the shard BUSY
and the client burns its 16 attempts into EIO. `host_txn_commit` hands
that txn to a finisher thread, which reads the decision record and
proposes RESOLVE (retrying BUSY until 10 s). The stall itself is W13,
now off the apply path (above). First 9-host suite on the finisher
build was **200/201 all nine, 42–44 s**
(`results/posix/20260926-0345-fin`, `fin_q=0`).

**Mkdir hammer (Sep 26, `6a60318`).** Propose was fsyncing the Raft
log while holding `h->mu`, then sending one AppendEntries.
`results/measure/20260926-042515-mkdir-hammer`: idle p50 8.4 ms,
144-way p50 **258 ms** / 470 mkdir/s. The fsync now runs outside the
lock (`68dfebb`). Proposers only raise the send ceiling; the pump
ships one batch after the threads queued on the lock have appended,
and it will not start a second batch while one is in flight
(`f47854b`, `6a60318`). A clean 144-way run
(`results/measure/20260926-052320-mkdir-hammer`): idle p50 11.3 ms,
144-way p50 **168 ms** / 754 mkdir/s, apply_max 61 ms, `fin_q=0`.
The repeat (`20260926-052437-mkdir-hammer`) hit a 766 ms apply of
31 entries and fell to p50 235 ms — that stall was the synchronous
compaction W13 has since moved off the apply path. Do not pipeline
past the one in-flight batch. Earlier
same-morning numbers, before this batching, are in the history
(`044248` 144 ms / 735 on a smaller table, `050609` 189 ms / 706).

**KV get (`5d3e603`).** A mkdir's negative lookup pread every LSM
segment under the KV lock and freed the block on a miss. The segment
is pinned, the pread runs outside the lock, and the block stays
cached (32 slots per segment). Measured on that tree (build string
`f93e7e6669b6-dirty`):
`results/measure/20260926-053753-mkdir-hammer` idle p50 **7.1 ms**,
144-way p50 **147 ms** / 887 mkdir/s, apply_max 59 ms, `fin_q=0`.
Three `rmdir-own ENOTEMPTY` lines, no mkdir errors. Do not pipeline
past the one in-flight batch. A multi-entry AppendEntries fsyncs
once at the end of the batch (catch-up used to fsync each entry
under the host lock). Do not give every proposer its own sync-hold
slot: idle mkdir waited out unrelated appends (p50 7.1 → 11.9 ms)
and the 144-way rate did not move.

**Apply path: where the 887/s ceiling was (Sep 26 midday).** A
thread-local fsync defer for one txn's PREPARE/RESOLVE burst was
built and measured first: no gain (`20260926-110435`, 12059 / p50
174 ms, clean apply) — the wall was never the leader fsync count. It
was the apply itself: `strace -c` on the dual-host follower during
the hammer showed 8 650 `pread64`/s and 875 `fsync`/s; `perf` showed
half of efsd on `memcmp` under `search_block` / `kv_msrc_advance`.
Every PREPARE apply runs two prefix scans (`guards_conflict`,
`reduces_pending`), every RESOLVE three (`txn_scan_kinds`), and
`merge_scan` opened an iterator on every LSM segment — one pread and
a linear walk of a 64 KiB block each — under `l->mu` on the pump.
Three changes, all LSM-internal / pump-internal, no protocol change:
(1) `merge_scan` skips a segment whose [first,last] key range cannot
hold the prefix (`kv_seg_excludes`; L1 is range-partitioned, so a
3-byte txn prefix is in one or two of them) and the iterator reuses
the segment's cached block; (2) the pump's KV WAL fsync and the
applied-index file write moved off `h->mu` and the file write is
throttled to 10 ms (three fsyncs per ~3 ms cycle were under the lock
proposers queue on; the saved index is only a restart lower bound);
(3) `KV_LSM_BLOCK_TARGET` 64 → 8 KiB (readers take any block size;
old segments stay valid until compaction rewrites them). Hammers,
build string still `194286c37a4f-dirty`: prune alone
`20260926-112916`: idle 9.5, 144-way **13715 / p50 137 ms**; + pump
tail `20260926-133934`: idle **6.97**, **14764 / 106 ms**; + 8 KiB
blocks `20260926-134640` idle **4.54**, **21990 / 73 ms** and
`20260926-134738` idle 5.59, **28072 / p50 60 ms** (~1 870 mkdir/s,
22 `rmdir-own ENOTEMPTY`, 0 mkdir errors). Every one of those runs
contained a 1.7–2.0 s `kv_compact_locked` stall; the clean-window
rate is higher still. **Cost of the speed: the L1 rewrite now
happens every ~7 s of hammer** (compaction trigger is 4 × 4 MiB L0
flushes and the table writes ~2 MB/s of records) and a 1.9 s stall is
past the election timeout — the 9-host suite on this build
(`results/posix/20260926-1350-blk`) ran in **20–38 s** but scored
195–199/201 with two elections, both at a 1.9 s compaction
(`link_across_dirs`, `last_link_unlink_other_dir`, `dir_many_files`,
`dir_deep_nesting*` — the BUSY-exhausted-10 s / election class).
That stall is what W13 (done, top of this section) removed from the
apply path. Do not tune `l0_max` or the memtable size around how
often compaction runs.

**Follow-on, same afternoon: memtable probes and the wakeup herd.**
After `610f4a8` the pump thread was still 61–67 % of efsd samples,
with `memcmp` 23 % — 12 % of it the memtable binary search inlined
into `lsm_batch` (one `lsm_put` per PREPARE part, ~15 probes, each
two dependent cache misses: `e[mid]` then its key) — and ~20 % of
efsd in `native_queued_spin_lock_slowpath` / `futex_wake` /
`futex_wait_setup`: the pump's `pthread_cond_broadcast(applied_cv)`
after every cycle woke every sleeping handler (~150 under the
hammer) to re-check a predicate that was false for all but a few, all
on one futex word. Three changes, no protocol change: (1) the
memtable entry's key is allocated inline with `struct kv_ent`; (2) a
parallel `uint64_t pfx[]` array (first 8 key bytes, big-endian) is
what the binary search probes — L2-resident, an entry is touched only
on a prefix tie; (3) waiters register the index they need (or "any
change of this group's view" for a read-index round) in one of 512
slots with its own condvar (`host_waiter_sleep`), and the pump
signals only the slots the fresh view satisfies
(`host_waiters_wake`); the view carries a `stamp` bumped by every
publish that changed a field; `applied_cv` remains as the overflow
path. `memcmp` 23 → 11–13 %, `kv_mtab_pos` itself 2.2 %. Hammer
(build `610f4a8…-dirty`): `20260926-141940` idle **5.23 ms**, 144-way
**30366 / p50 52 ms**, p99 224, 0 mkdir errors (67 `rmdir-own
ENOTEMPTY`); its twin `20260926-142052` hit a 14.7 s max with 30
`mkdir EIO` — the run-to-run spread is how many 2 s compaction stalls
and the elections they trigger land in the 15 s window (g0 term
6746 → 6751 during the first run), not the code. 9-host suite:
`results/posix/20260926-1430-wake2` **200/201 on eight hosts, 199 on
fcstor009** (`flock_shared_then_exclusive`, an intermittent seen on
Sep 22/25/26 builds) in **15.5–23 s per host** — the prior gate runs
were 39–44 s — with a 2.0 s `apply_max` and one election per group
at the start. Its predecessor `20260926-1425-wake` ran into a
four-term g0 election burst (`apply_max=851 ms` on 003 → LEADER→
FOLLOWER → CANDIDATE ×3, `arc_term_miss=37`) and scored 193–200 in
16–75 s with the EIO cluster on one host. Same W13 class; do not
bisect it. Remaining pump profile: `memmove` 7 % (pointer + prefix
array insert), txn-record scans ~6 %, `kv_compact_locked` (W13), and
the residual futex share is `l->mu` / `h->mu` handoffs, not the herd.
One PREPARE command = one part = one Raft entry = one `lsm_put`;
folding a txn's parts into one PREPARE is a protocol change (per-part
verdicts) and goes to the user, not into the tree. Staging WAL
records under the hold into one `write()` per cycle was tried and
reverted (no measurable change; history, Sep 26 afternoon). Cluster
is on `c044fb16e859-dirty`, servers and clients.

**9-host suite on that same tree** (`results/posix/20260926-054047`,
timeline `results/measure/20260926-014030-w8-stall-timeline`):
seven hosts **200/201**, fcstor007 199, fcstor013 198, 39–41 s,
0 NOTRUN. No term change. `names_crazy_dirs` passed on all nine.
Left: one `dir_many_files` EIO, one `names_crazy_roundtrip` EIO,
one `dir_deep_nesting` 15 s timeout. The previous suite on
`e8f3dc1` was 194–199/201 with a group-0 election
(`results/posix/20260926-050825`).



**I16 landed (Sep 23 02:47, `43bdf6a41f7d`) — gate runs owed.** Every
directory RPC (CREATE/MKDIR, UNLINK/RMDIR, LINK, RENAME_AT) now carries
an optional 36-byte op-id suffix `(client uuid, session epoch, seq,
contiguous ack)` (`EFS_OPID_WIRE_LEN`, `handler.c:dirop_opid`). The
window for an op lives on the **dentry shard of the (destination) name**
— deterministic from the request, always a participant, always in the
group that owns the shard (the coordinator is randomized, the parent
shard may be in the other group: both rejected). The leader probes it
(`host_opid_replay`) *before* the EEXIST/ENOENT pre-checks and answers a
replay from the recorded verdict (`host_opid_reply_ino`: current row if
the ino still exists, the stored ino/nlink otherwise). The verdict is
recorded **atomically with the op**: cross-group txns add a
`EFS_TXN_REDUCE_OPID` part (`host_prep_opid`; `fold_reduce` on
`EFS_KV_KIND_OPID` runs `efs_opid_fold` = ack + complete on whatever the
window holds at RESOLVE, so it commutes with the log path's
read-modify-write); the single-group log path appends a trailer to the
`CREATE` (flag bit `HOST_CREATE_F_OPID` in `cmd[1]`) / `UNLINK` /
`RMDIR` (by length) commands and `efs_meta_apply_*_op` writes the window
in the same `efs_kv_batch` (`opid_item`). The apply keeps
returning OK for a replay it sees (idempotent); the client-visible
EEXIST/ENOENT for a *genuine* second attempt still comes from the
leader's pre-check, which now runs after the probe. Client
(`inode_rpc.c`): one random uuid per mount (epoch 1), one seq space,
512-slot in-flight table; `ack = lowest in-flight − 1`, so a shard's
window advances past seqs it never served; the slot is held across every
BUSY/STALE/NOT_PRIMARY retry inside `rpc_send_recv_*`, so the retry is
byte-identical. Table full = op goes out with no identity (pre-I16
behaviour). `efs_opid_ack` now moves the watermark and shifts the
bitmap; `efs_txn_prepare_opid` rejects any key that is not exactly the
window key. Unit: `test_txn:test_opid_reduce` (commit records, abort does
not, commutes with a log-path fold, wire form), `test_meta_apply` I16
block (mkdir/create/unlink/rmdir replay = recorded verdict; the same op
without an id = EEXIST/ENOENT). `raft-obs` line gains `opid_replay=`.
**Bounded by design, and the two limits to know:** the reply cache is
16 entries per `(client, epoch, shard)` window (`EFS_OPID_REPLY_CACHE`;
`EFS_OPID_VAL_MAX` 512 → `txn.c VAL_MAX`); a 17th un-acked op on one
shard applies **unrecorded** (`efs_opid_fold` NOMEM → skip), i.e. falls
back to pre-I16. One client with > 16 concurrent dir ops on one shard
whose oldest is stalled hits that. If the 9-host gate still shows
EEXIST-on-fresh-name, check this first (count `opid_replay` and look for
>16 in-flight per shard) before suspecting the mechanism; raising the
cache is an implementation matter. Second limit: the dir op-id identity
is a per-mount random uuid (epoch 1), **not** the §7.5 session's
`(uuid, epoch)`, so a dead client's windows are never dropped by the
fence barrier §7.9 relies on ("a fenced session's records are dropped
wholesale") — a remount leaves its old windows behind. Follow-up, no
decision needed: seed `opid_uuid/epoch` from the client session and drop
`EFS_KV_KIND_OPID` keys for a fenced epoch in the barrier.
**Gate results on `43bdf6a` (Sep 23 02:52–03:07, rolled 02:49):**
`i17_leader_freeze.sh` run 1
(`results/measure/20260923-025259-i17-leader-freeze`): parent clean,
`arc_term_miss` 0→3, `opid_replay` 0→3, **0 worker errors** (the
`46d54e6` runs had 3 EEXIST + 1 ENOENT). Run 2 (`-025903-`): parent
clean, `opid_replay` 3→7, but **3 workers got `mkdir ENOENT`** at round 7
during the g0 freeze (fcstor007 p0, fcstor008 p3, fcstor015 p3); the
following `rmdir` of the same name succeeded, so the MKDIR committed and
only the *reply path* failed. Not an I16 miss: the replay path returns
OK, and `raft-host` logs no mkdir `rc=-3`. Root cause found in the
client: `rpc_send_recv_shard/_dual` returned `EFS_ERR_NOT_PRIMARY`
immediately on a **hintless** NOT_PRIMARY (`primary_id == 0` — a stale
leader that just stepped down and has not heard the new one), and
`efs_client_stat_ino` maps every RPC failure to NOT_FOUND, so
`ll_mkdir`'s post-mkdir `lookup_fill(new_ino)` (GETATTR) turned into
ENOENT for a dir that exists. Fix in tree (uncommitted, see §1b tail):
hintless NOT_PRIMARY backs off like BUSY (50 ms ×2^n, same 16-attempt
budget, EBUSY at the end) in both send paths; `ll_mkdir` logs
`fuse: mkdir ... ok ino=N but getattr rc=` when it happens. Server side:
`host_opid_reply_ino` logs `opid-replay ... (stub)` / `row gone`.
posix jobs=1: **200/201 + mmap SKIP** (`results/posix/20260923-030005`).
9-host: **189–191/201 per host, 0 not-run**
(`results/posix/20260923-030056`); fails = the six many-op timeouts
(item 4) + one-offs `names_crazy_roundtrip` EIO (014), `symlink_absolute`
EIO (012), `unlink_open_then_recreate` `b''` (012) — the same hintless-
NOT_PRIMARY class is the first suspect for the EIOs. `md_latency.py` 30 s
after the suite: mkdir 8.4 / create+close 19.3 / append+close 65.9 /
stat 0.5 / unlink 2.0 / rmdir 8.3 ms — post-suite churn, **not** a valid
comparison; remeasure on an idle cluster (commit flat 30 s).
**That client fix is `bb634d9`, rolled, and it closed the getattr
case.** On `ad292b9` (getattr reads a COMMITted-but-unresolved inode
row; `efs_meta_apply_get_inode_tx`) the freeze
(`results/measure/20260923-144129-i17-leader-freeze` and `-144243-`)
had no new `fuse: mkdir ... but getattr` line. Run 2 was clean
(opid_replay 1→5, parent removable). Run 1 left `d-fcstor011-1-22`:
three clients got `mkdir EBUSY` after the 10.3 s retry budget while
recovery of shard 1504 took 16.5 s, then `rmdir` returned ENOENT for a
name that existed. The dentry was a COMMITted EXCL intent, and lookup
used a bare kv get. Handler lookups and resolves now go through
`efs_meta_apply_lookup_tx` / `resolve_tx` (same rule as the inode row;
the apply path stays plain). `rmdir`/`unlink` map `EFS_ERR_BUSY` to
EBUSY — they were returning EIO. Do not widen the 16-attempt budget to
outrun recovery.
**Gated on `7e29943f28ef-dirty` (rolled Sep 23 18:21 UTC, servers and
clients 007–015):** `i17_leader_freeze.sh` ×2
(`results/measure/20260923-182559-i17-leader-freeze`, `-182709-`) — both
PASS, 0 worker errors on all 9 clients, parent removable, `opid_replay`
0→1→3, `arc_term_miss` 0→3→15, every `fuse.log` `getattr=0 exhausted=0`.
Idle `md_latency.py` before the freeze: mkdir 9.8 / create 4.4 / append
7.1 / stat 0.4 / unlink 2.0 / rmdir 10.5 ms (reference 6.1/4.0/6.2/0.3/
1.5/5.2; mkdir/rmdir high — remeasure once `commit` is flat 30 s; the
post-freeze samples were taken with commit still moving and are
invalid). Known limit, by design: a client's 16-attempt budget (~10.3 s)
is shorter than recovery of a stranded txn (16.5 s on shard 1504) → the
app sees EBUSY, never a wrong answer. Do not widen it.

**Gates on `7e29943f28ef-dirty` (Sep 23 20:26–20:46 UTC):**
9-host posix **193–196/201, 0 not-run, 79–94 s**
(`results/posix/20260923-202626`, timeline
`results/measure/20260923-162609-w8-stall-timeline`). Fails are the six
many-op timeouts (item 4; mkdir p50 46 ms / p90 230 ms under the suite)
plus one-offs at the one group-2 election in the run (term 535→537,
leader raft-id 2→3, one root stat of 1.6 s): `concurrent_create_unlink_two_proc`
EIO on 009 and 010, `content_random_roundtrip` EIO on 014,
`concurrent_write_and_readdir` EIO on 007, `unlink_open_then_recreate`
reading `b''` on 009, `concurrent_appends` timeout on 010. Group 0 did
not change term during the suite (5498; `leader=0` in the status line is
raft id 0 = node 1, not "no leader"). No 2.4 s apply stall in this run.
`same_parent_storm.sh` 9×4×100 on Sep 23: parent ended `children=0
nlink=2` and `rmdir` succeeded, but fcstor014 logged two `mkdir ENOENT`
at round 63 (`results/stress/same-parent-20260923-202846`). Reply path:
`ll_mkdir` committed, then `lookup_fill` (no open fh) refreshed, and
`stat_refresh` maps every GETATTR failure to NOT_FOUND. Fixed: a
committed mkdir/symlink/link replies the dual-applied local row when
that refresh fails (`lookup_fill_committed`). Re-gate on the new
clients (servers still `7e29943f28ef-dirty`): PASS 9×4×100, parent
`children=0 nlink=2`, `RMDIR_OK`
(`results/stress/same-parent-20260924-170945`). The fallback line did
not fire, so this run did not exercise the failed refresh; the Sep 23
failure is the case the reply now covers.
Idle `md_latency.py` twice, 20 min apart, term stable and
`commit == applied`, commit still +30 in 30 s (the 0–2/s reaper band,
not catch-up): mkdir 8.5 / create+close **56.8** / append+close **59.5**
/ stat 0.5 / unlink 2.2 / rmdir 7.8 ms
(`results/measure/20260923-162535-idle-mdlat/mdlat-idle2.txt`). The 50 ms
mode is on the two ops that write a byte and close; mkdir is near the
6.1 ms reference. Not the post-roll election (the roll was 18:21, this
sample is 20:46). A 4 MiB raft-log tail still shows the suite, not the
idle 1/s — do not blame a command from it.
**Strace (Sep 24, `results/measure/20260924-021832-close-strace`,
`20260924-053648-leader-strace`): the 50 ms is a `recvfrom`, not fsync.**
On the client every slow mkdir/create/append is one `recvfrom` of a
441-byte reply (0x1B9). On both leaders, at the same instant as the slow
creates, a thread blocks 60–70 ms in `recvfrom` of a 2-byte frame; the
group-2 leader also blocks 77–114 ms reading a ~64 KiB frame (0x10049).
A different pair of threads on each leader sits ~52 ms in `recvfrom` of
an 81-byte frame for the whole trace — the 50 ms heartbeat cadence,
present with no creates running. The follower's long calls were a
2.000 s `clock_nanosleep` and 2.000 s reads, not this op. One create in
the leader trace was 10 ms; the other three were 66 / 73 / 118 ms.
Do not treat this as the Sep 20 wrong-condvar bug until a send-side
trace shows the 81-byte frame leaving a follower late.

**9-host on the mkdir-reply clients (Sep 24 20:33 UTC,
`results/posix/20260924-203211`, servers still `7e29943f28ef-dirty`):**
193–196/201, skip=1 (`mmap_write_read`), 0 not-run, 74–80 s. The Sep 23
election one-offs (`content_random_roundtrip`, `unlink_open_then_recreate`
`b''`, `concurrent_create_unlink_two_proc`) did not recur. New one-offs:
fcstor008 `names_near_path_max` and `content_random_overwrite_append`
EEXIST, `rename_file_over_symlink` EIO; fcstor010 `dir_many_files` EIO
at `f0149`; fcstor015 `dir_deep_nesting` ENOENT. The rest are the six
many-op timeouts. The XFS baseline on node9901 did not run
(`/data1/efs` is not a mount there); score the `efs-*.tsv` summaries,
not the compare files.
**9-host after the ghost + close-unlock clients (Sep 25 00:02 UTC,
`results/posix/20260925-0002-ghost`, servers still
`abc6e913e760-dirty`):** 191–197/201, skip=1, 0 not-run, 66–74 s.
`unlink_open_then_recreate` and `flock_unlock_on_close` passed on
every host. The empty read was the sharded `keep_last` path returning
only when the dentry table differed from the parent, so the common
case deleted the open-fd ghost and getattr adopted size 0. Last close
now also sends `LOCK_UN` for that fd's owner. Non-timeout leftovers:
fcstor007 and 009 `dir_many_files` EIO (`f0200`, `f0253`), fcstor010
`trunc_grow_sparse` FileExistsError on the test directory, fcstor011
`names_dash_prefix_terminal` EBUSY on the test directory. The rest
are the many-op timeouts.
**9-host after NET-retry + visible-dir mkdir (Sep 25 00:54 UTC,
`results/posix/20260925-0048-net`):** 194–197/201, skip=1, 0 not-run,
77–101 s. `dir_many_files`, `trunc_grow_sparse`, and
`names_dash_prefix_terminal` passed on every host, as did
`unlink_open_then_recreate` and `flock_unlock_on_close`. A dropped
conn or recv used to return `EFS_ERR_NET` on the first attempt
(create → EIO). It now stays inside the existing 16-attempt loop.
MKDIR `EEXIST`/`EBUSY` returns success only when a lookup sees a
directory, and no longer reports EEXIST when the name is not visible
(`fuse_create_errno` used to short-circuit on `last_err`).
**9-host after local directory lookup (Sep 25 02:50 UTC,
`results/posix/20260925-0130-lookup`):** 195–198/201, skip=1, 0
not-run, 63–69 s. `dir_deep_nesting_beyond_64` passed on all nine
hosts. With `entry_timeout=0` a depth-100 mkdir was one LOOKUP RPC
per ancestor per level. A directory this client already has is
answered from the local table; files still go to the server, so a
peer unlink stays visible. `dir_deep_nesting` (the same walk plus
`rmtree`) still timed out on 7 of 9 hosts. `names_crazy_dirs` timed
out on every host. `names_crazy_roundtrip`,
`concurrent_creates_same_dir`, and `concurrent_write_and_readdir`
timed out on some hosts only.
**9-host after write() stopped reporting inline (Sep 25 04:30 UTC,
`results/posix/20260925-0412-write`, servers+clients
`d2e593244a10-dirty`):** 194–198/201, skip=1, 0 not-run, 60–66 s.
`write()` used to `report_dirty(0)` the whole set before returning;
flush/close already does `report_dirty_ino(ino, 1)`, so the write
report was a second round trip on every small file. Best hosts
(007, 008, 011) fail only `dir_deep_nesting` and `names_crazy_dirs`.
Those two still time out on every host. `dir_deep_nesting_beyond_64`
slipped back to a timeout on some hosts after the roll.
Alone on fcstor007 (Sep 25 04:34 UTC, same build, jobs=16) both
pairs pass: `dir_deep_nesting` + `beyond_64` in 3.6 s,
`names_crazy_dirs` + `names_crazy_roundtrip` in 3.2 s. The 15 s
failure is the 9-host queue, not a bug in those tests.
**9-host after an immutable-segment block cache (Sep 25 12:35 UTC,
`results/posix/20260925-0445-segcache`, `6ba3592b03c2-dirty`):**
198–199/201, skip=1, 0 not-run, 48–56 s. `dir_deep_nesting` and
`dir_deep_nesting_beyond_64` passed on every host checked.
`names_crazy_dirs` still timed out on every host;
`names_crazy_roundtrip` on some. Each KV get was malloc + pread of
the block under the LSM lock, including a miss that walks every
segment. The segment is immutable, so the last block stays cached.
A miss used to install that block and evict the hot one. Misses
leave the cache alone, and a key past the segment's last key does
not read the last block again.
**9-host after that, plus no open-lease and no empty close REPORT
(Sep 25 16:49 UTC, `results/posix/20260925-1240-misscache`,
`59b312f5904b-dirty`):** 199–200/201, skip=1 (`mmap_write_read`),
0 not-run, 44–49 s. Seven hosts failed nothing. fcstor008 and
fcstor013 failed only `names_crazy_dirs` (15 s). `dir_deep_nesting`,
`dir_deep_nesting_beyond_64`, and `names_crazy_roundtrip` passed.
`names_crazy_dirs` later passed on all nine
(`results/posix/20260926-164123`). W13 is done. W11 is done
(Sep 27): 9-host posix is 200/201 again
(`results/posix/20260927-033723`). W9's pin rules are on the nine
clients. Posix 1, posix 2 (59/63), the leak gate, and the
1M-file RSS walk are done (`results/measure/20260927-w9-walk`).
W10's private empty-mkdir passed 5/5. The Sep 27 live switch failed
the 9-host suite and was rolled back that hour. Sep 28 the cluster
is on RDMA again; see §1b.
The idle 50 ms create+close median did not hold: Sep 24 15:03 UTC on
the live mount, term stable, `md_latency.py` was mkdir 5.9 /
create+close 4.1 / append+close 6.1 / stat 0.3 / unlink 1.6 / rmdir
6.2 (`results/measure/20260924-150154-rpcprof-shapes` agrees). One
50–110 ms sample per 20-op batch remains. Not a code change until
that median is back.
The I16 follow-up's server half is in tree and unit-tested
(`efs_session_fence_local` deletes that shard's op-id window for the
fenced epoch; `test_session` OK on node9901) and is **not rolled**: the
FUSE client never creates an efs session, so nothing fences it. Seeding
the op-id from the session waits on that client session, which is the
product-gap item, not a one-line change.

The I17 story (index-only ring match; `46d54e6`; two gate runs
`results/measure/20260922-122517-i17-leader-freeze`, `-122629-`) is in
`docs/project-history.md` (Sep 22). posix jobs=1 on `46d54e6`:
**200/0/1 SKIP in 92 s** (`results/posix/20260922-133653`); 9-host
**185–197 / 201, 0 NOTRUN, 46–66 s** (`results/posix/20260922-134008`,
a leader change 12 s in explains the low end). The six old `posix-*`
leftovers on 19810 stay half-applied (pre-fix rows, no repair tool) —
ignore or wipe at the next agreed `raft-mkfs`.

Cluster runs `46d54e679e4f-dirty` → rolling to **`43bdf6a41f7d`** with
`EFS_RAFT_OBS=1` (5 s `raft-obs:` lines in
`efsd.log`; keep it on until W8 closes). W8's gate chain passes except
the 9-host row: `tests/measure/w8_root_lat.sh` root mkdir med 10 ms max
0.063, no 1 s mode; `w8_root_mkdir.sh` 9/9 root + 9/9 sub; posix jobs=1
**200/201 + mmap SKIP in 37 s** (`results/posix/20260922-015106`);
`same_parent_storm.sh` PASS 9×4×100 (`results/stress/same-parent-20260922-015426`);
9-host suite **191–195 / 201 on every host, 0 NOTRUN, all nine done in
~62 s** (`results/posix/20260922-020950`, timeline
`results/measure/20260921-220933-w8-stall-timeline`). Idle
`md_latency.py` reference is now mkdir 6.1 / create 4.0 / append 6.2 /
stat 0.3 / unlink 1.5 / rmdir 5.2 ms (a run 20 s after a roll shows a
50 ms mode — that is the post-roll RDMA election churn, wait 2 min).

What is left in the 9-host row, and what to do with each:
1. **Half-applied cross-shard txn (I17)** — fixed `46d54e6` (ring match
   by `(index, term)`), gated Sep 22 (two freeze runs).
2. **Retry of a committed non-idempotent op (I16)** — `link_of_symlink`
   EEXIST on a never-used name, `concurrent_create_unlink_two_proc` EIO,
   `unlink_open_then_recreate` reading `b''`: a client got BUSY from the
   400 ms apply-wait deadline during a stall, retried LINK/UNLINK
   (`stale_retryable`), and met its own result. **Landed `43bdf6a`**
   (block above); gated on `7e29943` (the 20:26 9-host run above). The
   client still retries BUSY and the 400 ms deadline is unchanged. The
   `b''` read and the EIO one-offs recurred once, at the group-2
   election in that run.
3. **Compaction stall → leader loss (needs a decision, §4).** One pump
   cycle held `h->mu` for **2.4 s** on both g0 replicas at once
   (`obs-fcstor004/005.txt`: `apply_max=2464250us applies_in_worst=54`),
   the leader missed its heartbeats and g0 went 5299→5302→5303; the
   client saw a 2 s `stat` and two failed mkdirs in that window. The KV
   is 1 GiB per node in 64 MiB L1 segments; an L0 segment spans every
   shard, so `kv_compact_locked` merges **all** of L1 every 4 flushes
   (≈ 16 MiB of writes) under `l->mu` (every read on the node stalls
   too) and `h->mu`. It is deterministic, so every replica does it at
   the same moment. Options the spec does not choose between: bounded
   (leveled / per-key-range) compaction; compaction off the apply path
   (background thread, readers merge an immutable memtable); a larger
   memtable as a stopgap (fewer, not shorter, stalls). **W13 landed
   the background compactor (Sep 26); see the top of §1b.** Do not
   raise the election timeout.
4. **Throughput at 144 concurrent jobs** — the six many-op tests
   (`dir_deep_nesting*`, `names_crazy_*`, `concurrent_write_and_readdir`,
   `concurrent_creates_same_dir`, `mtime_monotonic_many_writes`) time out
   on most hosts: mkdir p50 21 ms / p90 88 ms under the suite, so a
   200-op test needs > 15 s. Steady-state apply is still ~0.3 ms per
   entry with no fsync in it (`apply_max=10657us applies_in_worst=36`):
   that is the KV reads in the apply path — one `pread` per segment per
   lookup/scan, 17 segments, no block cache. Measure before choosing
   (perf on the pump thread under the suite); a block cache is an
   implementation matter, a different segment layout is a decision.
Do not raise the per-test 15 s budget or the suite cap.

**Progress log (newest first — read this before the state below):**

- **22:15 (Sep 21)** — **9-host posix: 191–195/201 on all nine in 62 s
  (was 66–95 with `[None]` at the 385 s cap). Three causes, three fixes.**
  (1) *Harness:* the parallel path stamped the per-test clock at
  **submit** — 196 tests into 16 workers, so anything still queued after
  15 s was recorded "timeout" without running (3-op tests "timing out",
  the same first timeout on every host, ~100/201). The worker now stamps
  its own start (`a683def`); the budget is unchanged.
  (2) *`h->mu` contention:* with `read_mu` gone, 30+ handlers per pump
  cycle took the pump's own mutex to read commit/applied/leader and to
  cond_wait; the pump lost its heartbeat cadence and both groups
  re-elected 4× in 50 s under load
  (`results/measure/20260921-211829-w8-stall-timeline`). Handlers now
  read a lock-free view the pump publishes (`host_publish_view`,
  atomics) and sleep on `applied_cv` under a separate `cv_mu`; a handler
  takes `h->mu` only to propose or begin a read round (`4eb1419`).
  (3) *KV WAL fsync per applied entry:* 0.5 ms per apply on every
  replica under `h->mu` (`EFS_RAFT_OBS`). One `efs_kv_lsm_sync_hold` per
  pump cycle, released before `persist_applied` (`84a2a55`); spec
  §"Raft log is the durability boundary" says the KV owes nothing on
  the critical path. Under the suite: stat p50 3 ms (was 9), mkdir p50
  21 ms (was 63); idle mkdir 6.1 / create 4.0 / append 6.2 ms. What
  remains (§1b): retry-of-committed-op (I16 dedup), the 2.4 s
  full-L1 compaction stall that still costs a term, and per-op cost at
  144 jobs. New tools: `tests/measure/w8_stall_timeline.sh` (1 Hz
  raft term/commit + client op latency around the 9-host suite),
  `EFS_RAFT_OBS=1` on the deployed efsd.

- **17:37 (Sep 21)** — **The sweep itself was a 100 ms regression; fixed
  in `3291c6d`.** posix jobs=1 on `9534e53` passed 200/201 but took 219 s
  (56 s in the morning); jobs=16 put 77 tests over the 15 s budget;
  `md_latency.py`: mkdir med 99.5, create+close 152, append+close 202 ms
  (reference 7.2/6.7/9.0). The GC thread on each leader ran 512 shards ×
  3 prefix scans per second; strace: one scan = 9 `pread64` (one block
  per segment) ≈ 0.3 ms under the LSM mutex the apply path needs; perf:
  38 % `rep_movs` + 21 % memcmp, process CPU only 3 % — lock hold, not
  CPU. Now every PREPARE apply marks its shard, a 5 s-old mark gets one
  scan (64 per pass, 1 ms yield), clean clears it, a fresh process marks
  all 4096 once. After the roll: create 6.1–6.7, append 8.5–9.3, stat
  0.4, unlink 2.1, rmdir 7–10 ms medians; mkdir 9.9 (bimodal 2 ms log
  path / ~50 ms cross-group txn, so its median is the mix).
- **17:15 (Sep 21)** — **Recovery gates 0–3 pass on `9534e53`.** Within
  ~1 min of the roll the two leaders logged 47 `txn-recover` lines —
  every one `COMMIT (resolved)`: the coordinators HAD written COMMIT,
  only their RESOLVEs were lost, so the recovered ops (dentries, inodes,
  ALLOC bumps) are now visible ~90 min after their callers were told
  EBUSY. One forwarded RESOLVE came back BUSY once and succeeded the
  next pass. `kv_intents` on a fresh copy: **0** INTENT/GUARD/REDUCE
  (`results/measure/20260921-w8-orphans/after-recovery.txt`). Parent
  burst 12 rounds × 9 hosts: **108/108, 0 BUSY lines, max 0.31 s**
  (`results/measure/20260921-210917-w8-parent-burst`). Root lat
  (`…-211258-w8-root-lat`): root mkdir 16 ms (log path) or 50–57 ms
  (cross-group txn: 5 PREPARE + DECIDE + 2 RESOLVE ≈ 8 commits), max
  0.119, **no 1 s mode**; the "≤ 15 ms" I wrote for this gate was the
  log-path number, the txn path is ~50 ms by construction. Concurrent
  root mkdtemp (`…-211320-w8-root`): root 9/9 ≤ 0.137 s, fresh parent
  9/9 ≤ 0.311 s, every rmdir OK, no ENOENT.
- **17:10 (Sep 21)** — **Stranded transactions were the 10.4 s EBUSY.**
  `tests/measure/w8_parent_burst.sh` (9 hosts × 6 rounds of fresh-parent
  `mkdtemp`, `results/measure/20260921-204358-w8-parent-burst`): 53 OK,
  1 EBUSY after 10.39 s, and the new server line shows 16 identical
  `mkdir … rc=-13 (BUSY)` for it — one shard, every retry. A copy of
  node 2's `mdraft/kv` through `tests/tools/kv_intents`
  (`results/measure/20260921-w8-orphans`): **105 INTENT, 1 GUARD, 28
  REDUCE records, all 4 500–5 000 s old**, 45 of the intents on ALLOC
  keys of 45 even shards — a log-path create/mkdir landing on one of
  those shards is BUSY on every attempt. They date from the 1 s-scan era:
  a DECIDE or RESOLVE whose `host_wait_applied` hit 400 ms made the
  coordinator skip the rest, and nothing ever came back for the records
  (spec L5 says recovery must; there was none). `9534e53`:
  `efs_txn_scan_pending` + `host_txn_recover_pass` on the GC thread.

- **16:30 (Sep 21)** — **Root cause of the 1 s root mkdir: full-shard
  scan in RESOLVE.** Server strace during 12 root pairs
  (`results/measure/20260921-202253-w8-root-srv`): on the dual host 3–5
  threads sit in one futex wait and release together 0.70 s / 1.05 s
  later; on fcstor005 a futex wait ends `ETIMEDOUT` at exactly 0.400 s
  (the `host_wait_applied` deadline → BUSY); no disk syscall ≥ 0.15 s.
  perf on fcstor003+004 (`results/measure/20260921-202715-w8-root-perf`):
  **85–90 % of efsd CPU** is `host_pump → apply_committed → host_apply →
  efs_txn_resolve → lsm_scan_prefix → merge_scan → memcmp`.
  `efs_txn_resolve` and `efs_txn_drop` used a 2-byte prefix = every key
  of the shard, on every replica, per participant shard, under the KV
  lock and `h->mu`. Root's shard 1 carries the most history, so a
  cross-group child (txn path) cost ~1 s while a same-group child (log
  path, no RESOLVE) cost 2 ms — the bimodality. The 400 ms BUSY then
  drove the 10.4 s client backoff, and a retried UNLINK whose first
  attempt had committed came back NOT_FOUND = the rmdir ENOENT. Fix
  `165e779`: scan `[shard][INTENT|GUARD|REDUCE]` (3-byte prefixes).
  Unit tests `test_txn`/`test_meta_apply`/`test_sim` OK on fcstor003.
- **20:02 (Sep 21)** — **Root mkdir is 2 ms or 1.05 s; BUSY burns 10.4 s.**
  Root has **410** names, listed in 10 ms
  (`results/measure/20260921-195541-w8-root-trace`). Not the 65536
  spread. A strace of `efs-fuse` shows the 1.12 s call is one
  `recvfrom`; there is no client sleep on that path. Twenty sequential
  root mkdir+rmdir pairs
  (`results/measure/20260921-195829-w8-root-lat`): 13 at 2 ms, 7 at
  1.03–1.09 s, and the rmdir matches the mkdir. Interleaved with a
  fresh directory
  (`results/measure/20260921-200108-w8-root-interleave`): 6/16 root
  calls still ~1.03 s while the fresh-directory call in the same
  second is 2–15 ms. Two fresh-directory creates returned `EBUSY`
  after 10.38 s, which is all 16 BUSY retries. Raft tail, last 64 KiB
  (`results/measure/20260921-200251-w8-root-log`): group 0 PREPARE 58
  vs DECIDE 10. The 9-host warmup dies inside that backoff.
- **19:44 (Sep 21)** — **W8 root burst vs a fresh parent.**
  `results/measure/20260921-194332-w8-root`. One root `mkdtemp` 1.192 s.
  Nine at once: six `MKDIR_OK` in 1.19–5.96 s, three still out at 8 s
  (one python in `request_wait_answer`), and fcstor012/014 then `rmdir`
  ENOENT on the directory that same call had just created. Nine at once
  in a fresh subdirectory: **9/9 in 0.008–0.197 s**, every `rmdir` OK.
  Group 0 `commit == applied`, +55 entries in ~9 s. Root `stat` right
  after: nlink=406, `.stats` rollups all zero
  (`results/measure/20260921-194815-w8-root-stats`). Not a 65536-entry
  spread. The mount root is what the 9-host suite cannot enter.
- **19:40 (Sep 21)** — **W7 closed. W8 harness proven, 9-host gate not run.**
  Isolated walks are 1.1–6.1 s (`results/measure/20260921-133437-posix-isolated`);
  suite 1 jobs=1 stays 200/201. A 20 s cut of suite 1 writes 201 TSV rows
  (`results/measure/w8-cut.tsv`): 1 PASS, 200 `NOTRUN` ("suite cut by
  signal 15"), `NONE=0`. `compare.py` reports `EFS BUGS : 0` and
  `not run : 200` (`results/measure/w8-compare.txt`). Immediately after,
  9 clients each doing one `mkdtemp` in the mount root: **0 MKDIR_OK**.
  Five ssh timed out at 20 s (D-state `request_wait_answer` ignores the
  inner timeout); four were killed at 12 s (`rc=124`) with no success
  line. A same-binary remount cleared them. One mkdir on one client
  still returns. Do not start `run_tests.sh posix --parallel` until the
  root burst in `w8_root_mkdir.sh` returns.
- **18:23 (Sep 21)** — **W11 measured, still unspecified.**
  `results/measure/20260921-182308-raft-snap-state`: raft logs
  **1.83 / 3.56 / 4.36 / 1.89 GB** on fcstor003–006, still growing
  388–801 B/s on an idle cluster. KV 0.54–1.08 GB. `snapshot skipped`
  is latched (group 0 applied≈4521152, group 2 ≈3256233). Both groups
  `commit == applied` on every voter — fcstor005 is not behind on this
  build. A follower restart is still a full replay. Chunked
  InstallSnapshot stays unspecified: ask, do not design.
- **16:30 (Sep 21)** — **dd wall, 1 and 4 clients. 9-client number does not exist.**
  `results/measure/20260921-163033-dd-wall`, 8 GiB `dd conv=fsync` of
  non-zero `/tmp/src8g`, FUSE_OK, file 8589934592. **1 client 499 MiB/s**
  (wall 16.4 s; Sep 18 was 639). That REPORT: pack 3554 + push 4848 +
  finish 1013 ms, `rc=0`. **4 clients 176 MiB/s** aggregate (slowest
  185.8 s; Sep 18 was 251). 21 `report-split` lines, all `nrec=65536`
  `rc=0`, one push 149 s. 9 clients: every ssh hit 400 s; mkdir EIO on
  007/009/010 (the 30.2 s WALL is a failed open) and fsync EIO on
  011–015. Row is `INVALID`. Do not quote a 9-client rate from this run.
- **12:30 (Sep 21)** — **IOR-hard scaling, full runbook**
  (`tests/measure/ior_hard_scaling.sh`,
  `results/measure/20260921-162514-ior-hard-scaling`). Write MiB/s
  **372 / 33 / 69 / 82 at NP 1 / 4 / 9 / 36** (47008 B × 3000 segs, one
  file). 1-rank matches the own-file dd wall. From 4 ranks, ~half of
  `report-split` lines are `rc=-14` STALE and `finish_ms` (apply wait)
  is the large phase (37 s summed vs a 59 s IOR wall at 36 ranks).
  Not a monotone CAS cliff. Next: `dd_wall.sh` at 1/4/9 with `PERF=1`.
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

#### Decisions — taken and pending (Sep 28 2026)

Each row is a choice the spec did not make. A row marked **decided**
was accepted by the user and is now part of the design; implement it
per the item it points at. A row marked **done** is history. Nothing
in an open row is implemented until the user asks. W13 is done; its
pinned segment view is the primitive W11's steps use.

**Decided Sep 28 2026 (user accepted the recommendations):**

| item | question | decision | why, in one line |
| --- | --- | --- | --- |
| **D1 · W6.1 / W17** | what do N-1 shared-file writes do under conflict? | **A span publish commutes; no lock; no CAS on the base generation for spans.** A sub-range writer always publishes a span (even when it holds the base). `expected_gen` applies to full-image publishes only. The trailer keeps the folded spans' `candidate_gen`s (≤ `EFS_CHUNK_DELTA_MAX`) so a replay of a folded span is a no-op. The fold is done by the publisher that fills the last slot, or by a reader — never on another rank's `fsync`. | the code already has commuting non-overlapping spans; what collapses at nine ranks is the base-gen check *before* the span path (one fold STALEs every in-flight span) and the `have_base → full CAS` branch. This is §7.2's commutative reduction applied to the chunk map, and P1 (no serialization point). A distributed chunk lock stays forbidden. Gate in W17 |
| **D2 · W6.2** | may `open()` return before the chunk map is known; how big is the map window? | **Yes. `open()` adopts the inode row only.** Chunk maps are pulled per lane group, in parallel, by `pull_layout_miss` in a metadata window that runs one data window ahead of the prefetcher. Window size is internal and adaptive (start at the prefetch depth, grow while the read stays sequential). No mount option, no environment variable. | `performance.md` already says reads fetch chunk maps in per-lane windows; the code pulls the whole map sequentially at open and the server serializes 36 openers to 14.6 ms per GETCHUNKS. A row without a map is already the evictor's steady state (W9); a miss is pulled and an error is an error, never a zero-fill (I9) |
| **D3 · idle gate** | may the 5/s REAP_DONE gate be raised to remeasure ior-hard 1/9/36? | **No.** Measure after the reaper drains. If it does not drain, that is a bug to file. | the tail is the reaper clearing deleted IOR data, and a measurement started during it measures the reaper |

**Decided Sep 29 2026 (user accepted D4–D8 as written; source
`results/io500/20260929-023447-iorperf2/ana`). Implement per the item
each row points at; do not re-ask. D8 is a measurement whose result
comes back to the user before any budget changes.**

| item | question | decision | why, in one line |
| --- | --- | --- | --- |
| **D4 · W22 step 1** | when does a group take a snapshot and truncate its log? | **By log bytes and follower reach, not every 256 entries.** Take a snapshot only when the log since the last one exceeds a byte budget (recommend 512 MiB — the export itself is 2.67 GB and takes 5–12 s, so anything smaller makes export the steady state), and keep a trailing window of log entries after the snapshot point so `send_ae` serves a follower that is behind by less than that window from the log. Send InstallSnapshot only when `next_index` is below the retained window | `HOST_SNAP_MIN` = 256 applied entries triggers a 2.67 GB export back to back on every node; `raft.c:672` sends the file to any peer more than 256 entries behind; three imports of 12–16 s happened in one 4-minute IOR and each one is a follower that answers nothing while its pump applies the diff |
| **D5 · W22 step 2** | may a follower answer heartbeats while its pump applies an InstallSnapshot diff? | **Yes — slice the diff.** Apply the import diff in bounded slices between pump cycles (one `HOST_TICK_US` worth of keys, then drain the inbox and answer the heartbeat), instead of one 12 s apply. The follower stays a follower; the leader keeps its term | this is W14 step 2's "heartbeat answered from a thread that is not importing", with the evidence: terms moved +45 / +105 across the run with `drop=0` on every outbox, so the outbox was not the trigger; the import-diff apply is the only multi-second pump hold left (`apply_max` 1.17 s on fcstor006, `pump_hold_max` 4–5 s on fcstor005 in the earlier run) |
| **D6 · W22 step 3** | may `mdraft/` (Raft log, KV WAL, segments) live on a root the fragment writers do not use? | **Yes, and measure first.** One `dd` on a quiet node: `fsync` of a 4 KiB file on `/data1/01` while the writer pool creates fragments there, versus on a root with no writers. If the shared root shows the 100 ms mode, give `efsd` a `--meta-storage <root>` (default: first root, today's layout) and point 19810 at a root the six `--storage` paths do not include (or a seventh partition) | the compactor's segment `fsync` was a flat 100 ms 97 times and `persist_max` reached 254 ms while twenty writer threads did `openat`/`write`/`close` on the same XFS; the Raft commit path and the data path share one journal |
| **D7 · W14 step 4** | may the client tell the server a PUT is the first write of `(ino, ci, fi, gen)` so the server skips the six-root probe? | **Yes, with a sentinel, not a guess.** `path_hint = 0xffffffff` means "the client has never PUT this fragment generation"; the server then creates on the writer's least-queue root with no `access()`. Any retry of the same generation (REPORT STALE replay, failed reply) sends the real hint or 0. A fragment name includes its generation, so a first write of a new generation cannot collide with an existing file | the hint from a previous PUT can only help a re-PUT; IOR-easy and a fresh `dd` write every chunk once, so 1.59M `access()` (192 s across six handler threads on fcstor004) survived W14.4 unchanged. The step-4 gate ("under 1 % on a single-client dd") cannot be met by the hint as specified |
| **D8 · W16 steps 2–3** | how does a 86 234-record REPORT reach the log? | **Measure the per-batch commit latency first, then bring the number.** `report-split` shows `push_ms=9869` for 337 publish batches — ~29 ms per batch through the one in-flight batch per peer — and `finish_ms=9227` waiting on the last apply. Add `pub_batch_ms` (p50/max) to `raft-obs`; if the per-batch commit is the ~2–6 ms a Raft round costs here, the 29 ms is queueing behind other clients' batches and the answer is fewer, larger entries per REPORT; if it is the 100 ms `fsync` mode, D6 is the fix. Pipelining past one in-flight batch stays forbidden | the client's fsync waited 19.4 s in one TCP `recvfrom` for a server that was still pushing; nine clients × ~80K records at the observed rate is minutes of leader time per stonewall |

**Pending Sep 29 2026 (from the 04:07–04:27Z trace, `results/measure/20260929-040800-idle-trace/ana`; §1b has the numbers). Recommendations, not decisions: bring them to the user. D9 and D10 are the two halves of one problem — the KV cannot absorb writes as fast as the Raft log commits them, and the wait lands on the pump.**

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| **D9 · W13 step 2, W23** | may the apply path (the pump) block on L0 back-pressure? | **No. The pump never waits for the compactor.** When L0 is within `KV_LSM_RANGE_MAX` of the cap, the apply keeps writing the memtable and lets it grow past `memtable_max` (memory, bounded by what the Raft window can commit — 512 MiB of log is the ceiling since W22.1); back-pressure moves to admission on the leader: `host_propose` for REPORT/publish batches returns BUSY while the local L0 is over the cap, so the client retries with its existing budget and the follower's pump is never the one that stalls. Heartbeats and AppendEntries replies do not depend on the KV | the pump waited 4.1 / 1.7 / **24.2** / 2.7 / 3.6 s in `kv_maybe_flush_locked` on fcstor004 in one 7-minute write, each wait one compaction long; every wait produced `apply-sleep` 400 ms timeouts → REPORT `rc=-13` → client fsync EIO, and the two term changes of the run. Same class as W13 (a lock hold the apply does not need) and W22.2 (a follower must answer while it imports) |
| **D10 · W23** | how does L0 reach L1 so that a 7-minute write does not rewrite the table thirty times? | **Compact by bytes, not by file count, and split range 0.** (a) Merge a range's L0 into its L1 only when that range's pending L0 bytes are at least a fraction of its L1 bytes (recommend 1/8: a 250 MB range waits for ~30 MB of L0, a 1.8 GB range for ~220 MB), otherwise let L0 files accumulate — the cap that matters is bytes in L0, not 64 files; reads already probe every L0 (`kv_seg_probe`), so make the L0 cap a byte budget (recommend 1 GiB) and drop the 64-file cap. (b) Partition on more than `key[0]` where a range is large: range 0 is 1.8 GB against 190–330 MB for the other fifteen; split it by the next key byte at flush and compaction time so no range exceeds ~256 MB. (c) The merge reads input blocks one `pread` at a time (3.7M of ~8 KB): read each input segment through a 1 MiB sequential buffer | 433 compactions wrote 159 GB to keep a 5.3 GB table current through one 7-minute write; the compactor is 48–55 % of two servers' samples; range 0 alone is 61 GB of the 159 and the 24 s pump hold. `inputs=` 5–11 files of ≤256 KB each per rewrite of 200–1800 MB is write amplification of several hundred. W13 step 5's partitioned flush bounded the rewrite to one range; it did not bound how often a range is rewritten |
| **D11 · W22 step 3 / D6** | is `mdraft/` sharing an XFS with the fragment writers the 100 ms `fsync` mode? | **No — close D6 as "not the sharing", skip the `dd` measurement, do not move `mdraft/`.** The pump's Raft-log `fsync` averaged 0.38–0.40 ms on all three servers measured (61 228 of 61 668 under 2 ms on 004) while twenty writers created 408K fragments; the 90–120 ms `fsync`s are the compactor's own 200–300 MB segments (299 of 695 on 004, 407 of 733 on 005). `--meta-storage` stays as an option; the compaction fix (D10) is what removes the 100 ms mode and the 130 MB/s the compactor puts on `/data1/01` | per-thread `fsync` histograms in `idle-detail2-fcstor00{3,4,5}.txt`; a segment written at ~2–3 GB/s and then `fsync`'d is ~100 ms by arithmetic, no journal needed |
| **D8 · answered** | is the per-batch commit ~3 ms or ~100 ms? | **~3 ms** (`pub_p50=3109us` in 17 of 23 samples with traffic); the 46 ms samples and `pub_max` 0.8–2.4 s are the pump holds above. No "fewer, larger entries per REPORT" wire change is indicated. Remeasure `pub_p50`/`pub_max` after D9/D10 in one 9-client IOR | the number the D8 row asked for, from the `raft-obs` line W16.2 added |

D9, D10, and D11 were rolled 05:31Z (`a53b253f2455-dirty`). The 12:02Z trace (§1b) is the measurement: D9 held (no pump wait, no `backpressure` line), D11 held (`fsync` max 91 ms), and D10's byte rule held (0.25 GB compacted, not 159 GB). D10's "drop the file cap" did not. That correction is D12.

**D12 was implemented and rolled 12:29Z** (`a53b253f2455-dirty`, no perf, no strace). On the 12:36Z IOR, fcstor004's compactor brought L0 from 7 files down to 3 (L1 stayed 397). The recommendation below is what was built. The 400 ms apply wait still returned BUSY (73 of 99 `report-split` lines on fcstor004).

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| **D12 · W23 step 4** | may L0's file count grow without a bound while L0 bytes stay under 1 GiB? | **No. Keep the 1/8 byte rule, and put the file cap back as a backstop.** When `n_l0` is over `KV_LSM_L0_DEFAULT` (4), compact the range with the most L0 files even if its bytes are under 1/8 of its L1, and repeat until `n_l0` is under the cap. The 1/8 rule still applies when the count is already under the cap, so a fat range is not rewritten for a few KB. The pump still does not wait. D9's 1 GiB admission stays; it did not fire here | fcstor004 went 26 → 310 L0 files (L1 353) in one IOR while 34 compactions wrote 0.254 GB at 10–14 ms each; `lookup` then probes every file, the publish apply falls behind, and `host_wait_applied` returns BUSY at 400 ms (`pack_ms=0 rc=-13`, 54 of 60 reports). D10 said to drop the file cap because "reads already probe every L0" — that probe is the abort. **The backstop as built rewrote L1 for the whole of the 13:17Z copy (D13)** |

**D13 · W23 step 5 (from the 13:08Z trace, `results/measure/20260929-130800-ddposix/ana`). In tree 14:20Z, not rolled, not gated.** The shape below is what was built. A file-cap compact writes one L0 file and does not open L1. A compact that already meets 1/8, or the 1 GiB byte cap, still rewrites L1.

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| **D13 · W23 step 5** | when the file cap forces a compact, may that compact read the range's L1? | **No. Merge that range's L0 files with each other into one L0 file, and do not read its L1.** Repeat until `n_l0` is under the cap. The 1/8 rule stays the only merge that rewrites L1. The pump still does not wait. D9's 1 GiB admission stays | D12 ran a full L0+L1 merge whenever the count was over 4, which was the entire 100 GiB copy: fcstor004 wrote 67.5 GB in 425 compacts (max 17 s, L0 peak 379) and `lookup` still binary-searched them (`kv_seg_probe` 8.3 % self). `pack_ms=0` on 172 of 184 BUSY reports; one `push_ms` was 206 s |

**Pending Sep 30 2026 04:20Z (from the post-fix ecopy, `results/measure/20260930-040600-postfix-review/SUMMARY.txt`). The two mechanical fixes (F1, F2 there) are in tree and rolled 05:06Z; these two still need the user. Ask; do not write either.**

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| **D15 · W14 step 2 / raft.c `on_vote_req`** | may a voter that heard from the current leader within the last election timeout refuse a higher-term VOTE_REQ and keep its leader? | **Yes — leader stickiness (Raft §4.2.3 / §9.6; Pre-Vote or CheckQuorum are the two standard shapes).** Recommend Pre-Vote: a candidate first asks "would you vote for me at term+1?" without bumping anyone's term; a voter answers no while its leader heartbeat is younger than the election timeout; only a majority of yes starts the real election. `maybe_step_down` then never fires from a peer whose only problem is that its own lane to the leader is stalled | 426 group-2 terms in 13 minutes: fcstor006, unable to get a reply from fcstor004/005 for 30 s at a time, campaigned every 0.5–0.9 s and each VOTE_REQ deposed the live leader (`LEADER->FOLLOWER leader=-1`, re-elected 0.6 s later); every cycle is 400 ms of BUSY reads for every client. F1 shortens each stall from 30 s to 250 ms; it does not stop the deposition |
| **D16 · peer transport** | should Raft peer frames share the RDMA device's one shared recv CQ / poller and the per-conn 36-credit scheme with ~400 client conns, or get their own class with reserved credits and their own poller? | **Measure first (stack-sample the sender and peer-conn threads in a storm), then decide.** If the lost RAFT_REPLY is the shared `recv_poller` behind client completions, peer conns need their own CQ/poller; if it is the peer's reply send waiting on a credit, reserved credits. Do not pick before the sample | the frames of a blocked batch reach the peer and are processed (fcstor006 won votes through a frozen lane); only the reply is late by up to 30 s, and only while ecopy drives hundreds of client conns per server. **Sep 30 06:17Z: the 30 s late reply was the sender reading the wrong channel (`85f5b31c`), not the transport; D15/D16 keep no motivating case** |

**Pending Sep 30 2026 07:10Z (from the user's perf dir, `results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`). Ask; do not write either.**

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| **D17 · `st_blocks`** | where does a client get the allocated-block count for a file it did not write? `inode_allocated_bytes` counts the client-local present-chunk table (W20), empty since D2 for unopened files, so `du` sums 0 for every regular file (52K for 3.8 TB) and ecrawl calls 21150 of 21503 files sparse. `efs_meta_row` has no count; W20 forbids `st_blocks` from size alone | **A per-lane present-chunk count in the lane stamp** (each publish adds the chunks it made present, a truncate subtracts), reduced at getattr like `max_end`/`max_mtime` and returned in the row image; the client sums it. One more u64 per lane read, no new RPC. Alternative: define `st_blocks = 0` as "unknown" for files this client did not write and tell tools so | sparse detection, `du`, quota tooling and ecrawl all read `st_blocks`; only the server sees every lane's publishes |
| **D18 · client staging-table floor** | the staging estimate is dominated by the per-shard-tab floor (one 256-row slab + 16 × 1232 B chunk entries + 512-slot indexes ≈ 90–170 KB per tab, ×4096 tabs ≈ 360–700 MB) and exceeds `EFS_CLIENT_META_MB` (256 MB) with a few thousand rows staged (412 MB at 10780 rows, RSS 330 MB). The evictor then drops 64 hot rows a second forever and never reaches the cap. Which bound do you want? | **Evict whole cold tabs** (`shard_tick` already tracks tab age; a tab with no dirty/pinned/open ino is freed and rebuilt on demand), so eviction frees the floor it cannot otherwise reach; keep the row LRU for the rest. Alternatives: a smaller first slab and lazily sized indexes (the floor shrinks ~10×), or apply the cap above the floor (RSS then ≈ floor + cap) | the 22 ms/s stall from this churn is fixed mechanically (targeted `efs_export_evict_ino`), but the cache still cannot hold a du's rows, and the RSS bound the cap promises is not the one delivered |

**Order for continued implementation (decided Sep 28 2026).** Correctness
and truthfulness first because they are small and they are what "easiest
to use" means; then the CPU items that need no decision and are most of
both profiles; then the two design items above; W12 whenever a run dir is
cited.

| # | item | what | why now |
| --- | --- | --- | --- |
| 1 | **W17 step 1** | `fsync`/`release` return in bounded time; a killed writer never leaves a D-state FUSE request | seven clients wedged for 20 min after `kill -9` |
| 2 | **W16 step 1** | RPC BUSY/NET/IO surface as EBUSY/EIO, never ENOENT | a busy filesystem must not claim a file is missing |
| 3 | **W18** | dcache reclaim from a dirty list; init once | 73 % of the client's CPU walks a table looking for dirty chunks |
| 4 | **W19** | encode each AppendEntries once; `try_commit` from the replying peer's match | 35 % of the leader is copying AEs on the pump |
| 5 | **W14 step 4** | fragment `path_index` hint on PUT | six path walks per PUT |
| 6 | **W15 step 3** | one `fuse_buf_copy` per whole chunk, for multi-chunk writes too | in tree only for `size == chunk`; `dd bs=1M` (eight chunks) still copies twice: 16 % + ~19 % in the 22:11 profile |
| 7' | **W14 step 2 (a)–(b)** | outbox coalesces replies/heartbeats instead of dropping them | `drop=72114 > sent=50695` to one peer; terms +59 / +26 in two hours; every client BUSY is `no-hint` |
| 7'' | **W15 step 4** | post the RDMA SEND and return; reap CQEs in `send_buf_pick` and the reply wait | 6.9 % of the client waits synchronously for its own send CQE; more send buffers did not change it |
| 6a | **W20** | `st_blocks` from a per-inode counter; `ll_setattr` reads `size` from the row | a `stat` walks every chunk under two locks; 8.9 % of the client under `ll_setattr` |
| 6b | **W21** | a kick does not wake a scan that cannot evict (1 s idle after a no-evict wake, cleared on unpin); cross-tab running total | in-tree cursor did not help: 12.8 % of the client on the 22:48 profile because every write over cap kicks a wake |
| 6c | **W15 step 5** | `FUSE_CAP_SPLICE_READ` so the write payload is read from the pipe straight into the dcache buffer | after step 3 one user copy per byte remains (~22 % `fuse_buf_copy`) plus the kernel copy into libfuse's buffer (~9.5 %) |
| 6d | **W23** | probe a pooled connection's liveness once per idle period, not per checkout; pool generation instead of `fstat` per send | 1.17M `fstat` + 1.17M `getsockopt` + 0.6M `MSG_PEEK` in 230 s (~15K syscalls/s) for 586K checkouts; ~2.5 % of the untraced client |
| 6e | **W24** | `open(O_TRUNC)` / `truncate` of a large file returns in bounded time; SETATTR under BUSY is EBUSY, not a hang | a `dd` onto an existing 1.7 GB file never got past `openat`; SETATTR is the most-exhausted RPC (136 of 424) |
| 2' | **W25** | `futimens` fences cross-group lanes (today EINVAL on every multi-lane file); atime keeps nanoseconds; BUSY → EBUSY everywhere | `ecopy --verify` fails `futimens` on every large file **and then deletes its copy**; the atime check fails on every small one; correctness, goes with row 2 |
| 7 | **W14 steps 2, 3, 5; W15 step 4** | election trigger under import; reap `snap-*.kvx.tmp`; `send_buf_pick` | smaller residuals from the same profiles |
| 8 | **D1 → W17 steps 2–3, W6.1** | commuting span publish | shared-file write rate must rise with ranks; the headline N-1 number |
| 9 | **D2 → W6.2** | open adopts the row; maps in windows | 1 GiB open 1.6–2.5 s under 36 openers |
| 10 | **W16 steps 2–3 (D8)** | `pub_batch_ms` in `raft-obs`, one 9-client IOR, bring the number; no budget change without the user | `push_ms=9869` for 337 batches is either queueing or the 100 ms `fsync` mode, and the two have different fixes |
| 11 | **W8, W10** | 9-host posix 201/201; RDMA faster than TCP | gates re-run once 3–9 land |

**Revised order (Sep 29 2026, after D4–D8).** The items below go
**before 9 and 10**: they are the cause of the two IOR aborts and
nothing downstream can be measured until an ior-easy-write completes.
9 (D2 windows) and 10 (D8) follow; 11 last.

| # | item | what | why now |
| --- | --- | --- | --- |
| 8a | **W22 step 1 (D4)** | snapshot by log bytes (512 MiB) with a retained log window; InstallSnapshot only below the window | a 2.67 GB export every 256 entries is the top user symbol on all four servers and turns any 257-entry lag into a 12–16 s import |
| 8b | **W22 step 2 (D5)** | the follower applies an import diff in per-tick slices and keeps answering heartbeats | terms +45 / +105 across one IOR with `drop=0`: the import is the election trigger left |
| 8c | **W14 step 4 (b) (D7)** | `path_hint = 0xffffffff` = first write of this fragment generation; server creates on the least-queue root with no probe | 1.59M `access()` / 192 s on one server per IOR; the re-PUT hint cannot reach a fresh chunk |
| 8d | **W22 step 3 (D6)** | measure `fsync` on the shared vs a quiet root; if the 100 ms mode is the sharing, `--meta-storage <root>` and redeploy 19810 with `mdraft/` off the fragment roots | compactor `fsync` flat 100 ms ×97, `persist_max` 254 ms; a redeploy is a cluster change, so it is last of the four and its measurement decides it. **Superseded by D11 (Sep 29 04:27Z trace): the Raft-log `fsync` is 0.4 ms; the 100 ms is the compactor's own segment. Flag is in tree; no move** |
| 8e | **W23 step 1 (D9)** | the pump never waits for the compactor; back-pressure is BUSY at `host_propose` on the leader while L0 bytes are over 1 GiB | **rolled 05:31Z, held on the 12:02Z trace:** no pump wait, no `backpressure` line. The BUSY in that IOR is not this admission |
| 8f | **W23 step 2 (D10)** | compact a range when its L0 bytes reach 1/8 of its L1, L0 cap in bytes, split range 0, 1 MiB merge reads | **rolled 05:31Z. Byte half held** (0.25 GB compacted, not 159 GB; `pread` over 1 MiB is in use). **File-cap half did not** — see 8i |
| 8g | **rolled 05:31Z** | `disk_log_new_bytes` O(1); `kv_seg_w_open` `setvbuf` 1 MiB | in the same roll as 8e |
| 8h | **W23 step 3** | `lookup` skips a segment whose first/last key span does not contain the key, before `block_for` | **rolled 12:29Z.** Last key is cached in `kv_seg_open`; `kv_seg_probe` returns NOT_FOUND when the key is before `idx[0]` or past the cached last key. Not separately profiled (this IOR had no perf). L1 was still 397 files and `report-split` was still mostly `rc=-13` |
| 8i | **W23 step 4 (D12)** | when `n_l0` is over the file cap, compact the range with the most L0 files even under the 1/8 ratio, and repeat until the count is under the cap | **rolled 12:29Z.** On the short IOR, fcstor004 ended at L0=3. On the 100 GiB copy the same rule rewrote L1 the whole time (67 GB, L0 peak 379). That shape is D13 |
| 8j | **W23 step 5 (D13)** | over the file cap, merge the range's L0 files with each other and do not read its L1; the 1/8 rule stays the only L1 rewrite | **in tree 14:20Z, not rolled, not gated.** The probe walks every in-span L0 file, and the full-L1 backstop both failed to keep the count down during the copy and put 67 GB on the same disk as the apply |

In tree and rolled (Sep 29 04:08Z, `54a500da9dc8-dirty`, RDMA, `--perf --strace`; the 02:35Z roll was `bbcbcb5ad779-dirty`), not gated by a suite: 1 (sync report loop: 8 s checked between REPORT attempts, plus whatever one in-flight RPC takes; dirty set merged back; fsync returns EIO), 2 (`fuse_stat_errno`), 3 (dirty list + `dcache_ensure` so `pthread_once` is not on the lock), 4 (`send_ae` reads the log into the wire buffer; `try_commit` starts at the quorum match), 5 (PUT reply carries the storage root), 6 (a 1 MiB write walks chunk-aligned pieces; each whole chunk is one `fuse_buf_copy` into a dcache-owned buffer; a partial head or tail bounces its own length), 6a, 6b, 7's snap-tmp reap and `send_buf_pick`, 7' (an AE reply replaces an older queued reply to that peer, a heartbeat replaces an older heartbeat, a vote is not evicted, and the sender drains that lane before entry AppendEntries; a full entry lane returns AGAIN and does not increment `st_drop`), 7'' (`efs_rdma_send_frame` posts and returns; the reply wait reaps send CQEs and treats a send still busy after 5 s as a dead QP), 8, 9 step (a), 8a (snapshot by 512 MiB with a retained window), 8b (import diff in 1024-key slices), 8c (`EFS_PATH_HINT_NEW`), and the `pub_p50`/`pub_max` line of 10. 8d's `--meta-storage` flag is in and defaults to the first `--storage` root; `mdraft/` was not moved and the shared-vs-quiet `fsync` measurement was not run. 8h and 8i / D12 were rolled 12:29Z. The 12:36Z IOR (`results/io500/20260929-123635-rdma`) finished ior-easy-write at 2.218 GiB/s and mdtest-easy-write at 1.765 kIOPS, then ior-hard-write aborted on fsync (31 failures, no bandwidth) and fcstor012's ranks stuck in D behind a reclaimer holding the shard I/O lock across a fragment GET. The 13:17Z copy failed a different way: `fsync` returned EIO because D12 rewrote L1 for the whole ingest (D13). 8j / D13 is in the tree (14:20Z), not rolled, not gated. The shard-lock drop is in the tree (14:50Z), not rolled. Next is 10's one IOR, 7 step 2 (c) (measure, then ask), 11. 8e, 8f, and 8g were rolled 05:31Z, including the 1 GiB L0 byte cap; the 12:02Z trace is their measurement (§1b). In tree and rolled with that same build: W23 client (pool generation instead of `fstat`; liveness probe only after 1 s idle), W24 step 2 (one 8 s deadline around truncate's flush, REPORT, and SETATTR), W25 (`futimens` proposes the utimens command on the other group for cross-group lanes before the inode-group entry — not `LANE_FENCE`, which would delete chunks; `atime_nsec` on the SETATTR wire; `EFS_ERR_BUSY` is EBUSY). The three `test_meta_apply` cases named under W17 step 3 are in `test_chunk_deltas` (Sep 29 15:40Z): replay of a folded span is a no-op, a span after the fold attaches to the new base without naming it, overlap with a live span is STALE (a tombstone's old range is free), and a full image after the fold must name the current base. The 04:08Z roll and the 02:35Z IOR analysis are in §1b.

Prior recommended answers, for the record:

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| W13 | full-L1 compaction holds the apply path 2.4 s and costs a term | **done Sep 26**, partitioned flush rolled Sep 27 on TCP | background compactor kept `apply_max` under 70 ms. Flush now writes one L0 file per `key[0]`; one compact leaves the other range's L1 file in place (`test_kv_lsm`) |
| W11 | the KV export is larger than one 4 MiB SNAP command, so the log never truncates | **done Sep 27** — chunked InstallSnapshot of a file; import is a sorted diff | logs under 5 KB; fcstor005 rejoined in 510 ms; `apply_max` 0 on a 386 MB export. 9-host posix 200/201 (`results/posix/20260927-123717`) |
| W9 | bound the client staging table | **done Sep 27** — in-flight pin, append-reservation pin, chunk maps before the row, and the evictor walks past a pinned oldest window | a cold stat of 1M files leveled at 233 MB RSS (`results/measure/20260927-w9-walk`). posix 2 is 63/63 (`results/posix2/20260927-123946`). Leaks are clean (`results/leaks/20260927-035622`) |
| W10 | RDMA needs an empty table to gate | **live Sep 28**, suites pass, not faster than TCP on posix or the 9-client write | 19810 is RDMA (`db2b88c4802a-dirty`). jobs=1 after user xattr is 200/201 (`results/posix/20260928-043918`); 9-host before that was 199/201 in 57–59 s; posix2 63/63; 9-client dd **1326.6** vs TCP 1478 |

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
- **Amended Sep 28 2026 (D1):** the per-chunk CAS remains for
  full-image publishes; span publishes no longer CAS on the base
  generation (W17 step 3). The N-1 contract is: non-overlapping
  concurrent writes to one chunk never conflict; overlapping ones
  resolve in Raft order.
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

1. **ior-hard-write** — **spans are the data path (Sep 27).**
   NP=4, SEGS=3000, 47008 B, one file, cold remount: write
   **481.56 MiB/s** (1.12 s), read **91.35 MiB/s** (5.89 s), pattern
   12000 records bad 0. posix2 **63/63**
   (`results/posix2/20260927-190509`). Prior curve
   `results/measure/20260921-162514-ior-hard-scaling`: **372 / 33 /
   69 / 82 MiB/s at 1 / 4 / 9 / 36**, about half of REPORTs
   `EFS_ERR_STALE`. The 4-rank point moved 33 → 482. 1/9/36 were not
   remeasured: group 2's REAP_DONE tail stayed ~11–16 entries/s
   (distinct inodes) and preflight fails above 5/s. Do not raise it
   (**D3**). Do not add a chunk lock. Do not tune 47008.
   **Sep 28, NP=9 via `run.sh ior`: STALE storm, fsync EIO, D-state
   ranks — W17.** **Decision D1 (Sep 28): the span publish commutes
   (no base-gen CAS for spans; sub-range writers always publish spans;
   fold by the chain-filler or a reader). Implement as W17 step 3.**
2. **1 GiB open costs 20 s of a 22 s easy-read** — 128 sequential
   GETCHUNKS + a 64-lane stat per open. Spec §8 per-lane range fetch
   ([performance.md](performance.md)) is the fix. **Decision D2 (Sep
   28): `open()` adopts the inode row only; `pull_layout_miss` is the
   one pull path, one GETCHUNKS per lane group in parallel, a metadata
   window one data window ahead of the prefetcher, size adaptive and
   internal (no knob).** Steps: (a) remove `pull_file_layout` from
   adopt for regular files; (b) make `pull_layout_miss` issue the
   window's lane-group GETCHUNKS concurrently and record the covered
   range per ino; (c) have the read prefetcher request the next
   metadata window when the data window advances; (d) keep the 64-lane
   size stat at open (size authority is not the map). Gate: 1 GiB cold
   open under 10 ms with 36 concurrent openers
   (`tests/measure/` open-cost script); easy-read not worse than
   `results/io500/20260928-150609-rdma`; `-R` errors 0 on both reads;
   posix 1 jobs=1 200/201. Forbidden: a whole-map pull anywhere;
   zero-fill on a miss; a mount option or env var for the window.
3. ~~mdtest `rmdir` ENOTEMPTY/EIO under load~~ — **FIXED Sep 21** (it was
   the parent-row lost update, not transient; §7.2 reductions). **Rate
   fixed Sep 27.** The flat 138 / 134 / 159 ops/s
   (`results/measure/20260921-161931-samedir-rate`) was one AppendEntries
   per create: a proposer that arrived while a fsync hold was open
   broadcast that single entry under the raft lock. Those proposers now
   append locally; the hold owner fsyncs the burst and marks every
   covered index durable; the pump sends one batch.
   `results/measure/20260927-211953-samedir-rate` (ROUNDS=100, storm
   PASS, parent `children=0 nlink=2`): **333 / 1385 / 1241 ops/s at
   1 / 9 / 36 procs**, `busy_n` 0 / 0 / 1. Idle mkdir median 3.1 ms.
   Do not change the backoff or the txn protocol. Do not lower
   `EFS_DIR_SPREAD_MIN`.

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

#### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation — DONE

**Done Sep 21 2026.** The 15 s failures were the metadata wakeup floor
(fixed Sep 20), not a remaining per-test bug. Isolated on an idle cluster,
`results/measure/20260921-133437-posix-isolated` (budget 15 s, jobs=1):

| test | result | seconds |
| --- | --- | --- |
| `concurrent_creates_same_dir` | PASS | 4.9 |
| `mtime_monotonic_many_writes` | PASS | 1.1 |
| `dir_deep_nesting` | PASS | 6.1 |
| `dir_deep_nesting_beyond_64` | PASS | 4.8 |
| `names_crazy_dirs` | PASS | 3.3 |
| `dir_many_files` | PASS | 5.2 |

Suite 1 jobs=1 on the same build is **200/201** plus `mmap_write_read`
SKIP (`results/posix/20260921-123904`), above the 193 floor. If any of
these walks exceed 15 s again, the cluster was not idle or a wakeup
regressed — do not raise the budget.

- **Forbidden:** raising `POSIX_TEST_SEC` or the `@budget(...)` values. A
  timeout is a failure to be removed, not re-labeled.

#### W8 — 9-node POSIX suite 1 (gate: 201 rows, 0 NOTRUN, every host)

**State (Sep 21 22:15): 191–195 / 201 on every host, 0 NOTRUN, all nine
finish in ~62 s** (`results/posix/20260922-020950`). History and the
three fixes that got here are in the §1b progress log (harness clock at
submit; `h->mu` contention after `read_mu`; KV WAL fsync per apply).
Earlier symptoms — nine hosts at the 385 s cap with `[None]` rows
(`results/posix/20260917-191430`), the 1.2 s / 1.03–1.08 s root mkdir,
`rmdir` ENOENT on a just-created name, `EBUSY` after the 10.4 s backoff —
are closed: whole-shard txn scans (`165e779`), stranded txn records
(`9534e53`, `3291c6d`), `read_mu` (`223da15`), peer-pool starvation
(`f10fec0`), harness (`a683def`), view (`4eb1419`), WAL hold (`84a2a55`).

What still fails, in order (details and instructions in §1b):
0. Half-applied cross-shard txns (I17) — **fixed `46d54e6` and gated**
   (Sep 22): freeze both leaders during `same_parent_storm`. The parent
   row stayed consistent (`nlink=5 nents=3` with three real children on
   the run that left names behind; the other run removed the parent).
   `arc_term_miss` moved. Details in §1b.
1. Retry of a committed non-idempotent op after a BUSY (EEXIST on a
   fresh LINK name, EIO, empty read) → I16 op-id dedup for
   LINK/UNLINK/MKDIR/RENAME. Mechanical, spec §7.9.
2. The synchronous full-L1 compaction: 2.4 s under `h->mu`+`l->mu` on
   every replica at once, costs the leader its term. **Decision.**
3. Six many-op tests at 144 concurrent jobs (mkdir p50 21 ms under
   load; apply 0.3 ms/entry of KV reads). Measure, then decide.

Run it as `bash tests/measure/w8_stall_timeline.sh` (runs the suite,
gives the raft/latency timeline) and read `raft-obs:` from the four
`efsd.log`s. Do not raise the 70 s warmup, the 15 s per-test budget,
or the 400 s suite timeout; do not point the suite at a subdirectory;
do not raise the election timeout.

- **Forbidden:** reporting `[None]` rows as failures, or as passes;
  the clock-at-submit bug coming back (a queued test cannot time out).

#### W9 — The client staging table is unbounded

`g_client.export` in `efs-fuse` keeps one row per inode this client has ever
touched and one entry per chunk it has written or pulled, and evicts nothing —
so a client that walks a large namespace holds the whole tree in RAM and the
server-side memory wall reappears per client. This is step 12 part A.
The evictor and the pin rules are in the client as of Sep 27.

The plan, the pin rules that make eviction safe (a report builds its records
out of this table, so evicting a dirty row is data loss), and the gate are in
[../client-cache-design.md](../client-cache-design.md). The Sep 23
recommendation below is in the client as of Sep 27. Posix 1 is
200/201, posix 2 is 59/63, the leak gate is clean, and a cold stat
of 1M files leveled at 233 MB RSS
(`results/measure/20260927-w9-walk`). Done.

**Recommendation (Sep 23), implemented Sep 27:** (1) The pin rules —
dirty / publishing / dirty dcache, ghost with an open fd, in-flight op pin,
live append reservation or lock — are exactly the set of rows whose absence
a report or an open fd could observe; anything narrower loses a write,
anything wider is not a bound. (2) `EFS_CLIENT_META_MB` = 256 MB default,
soft: at ~200–300 B per row plus chunk maps that is on the order of a
million rows, above any FUSE working set we have measured (IO-500 9×4,
the posix suites, `find` over the 410-name root), and a table where
everything is pinned grows and logs once rather than evicting work. Pick
a different number only if a client RSS measurement says so; the cap is
an env var, not a protocol.

Steps, once ratified:
1. Pin bookkeeping: a per-row pin count set by the dirty/publishing sets,
   `efs_open_note`/`close_note` for ghosts, and a scoped pin in every
   dual-apply window (create, rename, link, unlink). Unit test: a report
   built while eviction runs never skips a dirty row.
2. LRU by last-touch tick over unpinned rows; evict chunk maps of clean
   closed files first, then rows. Ghost reclaim at last close.
3. `statfs` from server numbers; `efs_export_fits_page_cap` becomes a
   best-effort early-out (the doc's §4).
4. Gate: `make test`; posix 1 (jobs=1 and 9-host) and posix 2 with no new
   failures; the walk-RSS gate — one client walks a multi-million-file
   tree and RSS stays near the cap where it grew linearly before; the
   valgrind leak gate (eviction is a new free path).

- **Gate:** the walk-RSS gate in that doc, plus posix 1 + 2 and `leaks`.
- **Forbidden:** evicting a row that is dirty, publishing, a ghost with an open
  fd, or pinned by an in-flight op. Bounding it by dropping records instead of
  refetching them.

#### W10 — RDMA — live on 19810 Sep 28, not faster than TCP

Sep 28 gate on `db2b88c4802a-dirty` (`EFS_TRANSPORT=rdma`,
`EFS_RAFT_OBS=1`). posix jobs=1 199/201 in 90.1 s
(`results/posix/20260928-033823`); 9-host 199/201 on all nine,
56.7–59.1 s, 0 fail, mmap + xattr SKIP
(`results/posix/20260928-034049`); posix2 63/63 in 76.8 s
(`results/posix2/20260928-034350`). After user xattr, jobs=1 is
**200/201** (`mmap_write_read` SKIP only,
`results/posix/20260928-043918`); 9-host was not remeasured.
9-client 8 GiB dd+fsync
**1326.6** MiB/s, walls 28.55–55.57 s, every file 8589934592
(`results/measure/20260928-033420-dd-wall`). TCP bars are posix
9-host ~13–15 s and 9-client dd 1478. The outbox fix that let
fcstor004 catch up is in this build. The speed bar in step 3
below is still open. Do not roll back to TCP unless a suite fails.

The Sep 27 live switch below was rolled back the same hour. It is
history, not the current cluster.

Private gate passed 5/5 (`tests/rdma_first_inode.sh` on fcstor007).
The live switch (`EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`,
`roll_efsd.sh --all`, clients remounted `EFS_TRANSPORT=rdma`) did not
match TCP. 9-host posix jobs=1 was 193–196/201, skip
`mmap_write_read`, 0 not-run, duration 385 s on every host
(`results/posix/20260927-044348`). The TCP bar on this tree is
200/201 in 30.4–31.3 s. What failed that passes on TCP: the many-op
tests (`dir_deep_nesting`, `dir_deep_nesting_beyond_64`,
`names_crazy_dirs`, and on some hosts `concurrent_creates_same_dir`
and `mtime_monotonic_many_writes`) hit the 15 s cap, plus a few
EIO/EEXIST one-offs. During that run `apply_max` on fcstor003 stayed
under 1 ms. Step 5 rolled 19810 back to
`EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` the same hour; clients
fcstor007–015 are TCP and a mkdir/rmdir on fcstor007 returned
immediately. Freeze, idle `md_latency`, and the dd rebaseline were
not run on RDMA. Do not debug this on the live cluster.

The private-cluster gap (100 mkdirs, fcstor007, ports 19950–19952)
was two bugs in `src/common/rdma.c`, both fixed in tree and not
rolled. The shared recv poller acked the completion-channel event
and then slept 100 ms, so a work completion already in the CQ
stalled every conn (11 of 100 mkdirs; 9-host posix then hit the
15 s cap). It now harvests again before a 1 ms backstop. Every
SEND also called `ibv_query_qp` and read a port counter, which made
a raft AppendEntries ~380 µs against ~15 µs on TCP; those reads
happen only after a send has already failed. With both fixes, one
clean run was 642 ms RDMA vs 507 ms TCP and raft RTT ~50 µs
(`~/efs-runs/rdmaprof7.log`). The Sep 28 live gate is the paragraph
above this section. 19810 is RDMA. The "same numbers as TCP or
better" speed bar is not met.

The steps below are the procedure that was followed, kept so the
gate stays findable.

The client connection-pool lifecycle fix is in tree and unit-gated
(`test_conn_fd`): a pooled conn pins its socket identity, checkout evicts on
mismatch, destroy refuses to close a recycled fd. The **live** repro was never
re-run, because it only reproduces on a freshly `mkfs`'d / effectively empty
table, and 19810 is populated. A remount there is *not* this gate.

19810's transport is RDMA as of the Sep 28 gate in §1b. Suites pass.
Posix and the 9-client write are still slower than TCP, so the speed
bar in step 3 is open. Every ceiling in the table above assumes the
data path can use the fabric; TCP over IPoIB will not reach it.

**Recommendation (Sep 23): do not wipe 19810 for this.** The repro needs an
*empty* table, not *the* table: `tests/rdma_first_inode.sh` stands up a
private 3-node efsd on one host (fcstor007, ports 19950–19952, storage
under `/tmp/efs-rdma-first`, `EFS_TRANSPORT=auto`) and runs the first
`mkdir`. That is the gate, and it touches nothing on 19810. The live
switch of 19810 to RDMA then needs no fresh table either — the project
state rule records that a populated table drove millions of creates over
RDMA with 0 errors; the empty-table hang was the only open defect. A wipe
buys nothing here that the private cluster does not; keep it for W11
(below), whose gate is easier on a table that grows from zero.

Steps:
1. `efs-bg.sh start w10-first 'bash tests/rdma_first_inode.sh'` with
   `EFS_RUNNER=fcstor007.ib`, ×5 (the tree must be built on 007 first:
   `efsd`, `efs-mgmt`, `efs-fuse`). Pass = every run prints
   `MKDIR_RC=0 WRITE_RC=0` and exits 0 (the first `mkdir` and a write
   inside it each have an 8 s `timeout`). Fail = any `MKDIR_RC=124`, and
   the fix is not in; stop and bring `/tmp/efs-rdma-first/fuse.log`'s
   `RDMA transport|WAIT TIMEOUT|SOCKET CLOSED` lines.
2. Switch the live cluster: `tests/roll_efsd.sh --all` with
   `EFSD_ENV="EFS_TRANSPORT=rdma EFS_RAFT_OBS=1"`, then remount the
   clients with `EFS_TRANSPORT=rdma`. Wait for `commit` flat on both
   groups (post-roll election churn is ~2–5 min).
3. Gate on RDMA, same numbers as TCP or better: posix jobs=1 (200/201),
   9-host posix (≥185/201, 0 not-run), `i17_leader_freeze.sh` ×2 with
   0 worker errors, idle `md_latency.py` within the TCP reference.
4. Re-baseline the write wall (`efs-fio-honest`: 8 GiB dd+fsync 1/4/9
   clients) and record it in §1a's ceiling table. Then TCP is no longer
   the default in the deploy rule and START-HERE.
5. If step 3 fails on anything that passes on TCP, roll back to TCP with
   the same `roll_efsd.sh --all` and bring the failure; do not debug
   RDMA on the live cluster with the suites down.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping 19810 without being asked
  (this item no longer needs it).

#### W11 — chunked InstallSnapshot — DONE Sep 27

Running build `612ee9ba9202-dirty`, TCP. The log truncates. A follower
install is a sorted diff (`src/kv/kv_snap.c`). fcstor005 rejoined in
510 ms. 9-host posix is 200/201, skip `mmap_write_read`, 30.4–31.3 s
(`results/posix/20260927-033723`). The earlier 199–200 run is
`results/posix/20260927-015719`. Gate numbers are in
`results/measure/20260927-w11-gate` and `docs/project-history.md`
(Sep 27). The original recommendation, kept so the steps stay
findable:

`raft_host` only compacts when the whole group's KV export fits in one
`EFS_WIRE_RAFT_MAX_CMD` command. On 19810 one group is over that, so the log
is never compacted and `snap_oversized` latches. Measured Sep 21
(`results/measure/20260921-182308-raft-snap-state`): logs 1.83–4.36 GB
and still growing ~400–800 B/s idle, both groups `commit == applied`
(fcstor005 is not behind on this build; the gossip `DOWN` right after a
bounce is the STATUS probe, not a dead process — check `pgrep -x efsd`).
A follower restart is a full replay of that log.

The fix is a chunked / multi-message InstallSnapshot, and **that protocol is
not specified anywhere**. Do not implement it before the recommendation
below is ratified. Until then this is a known, documented lag.

**Recommendation (Sep 23): adopt the Raft paper's InstallSnapshot chunking
(§7 of the paper: `lastIncludedIndex`, `lastIncludedTerm`, `offset`,
`data[]`, `done`), and take the snapshot as a pinned segment view, not a
RAM blob.** Two facts drive it:

- The wire cap is the smaller problem. Today `efs_raft_snapshot` calls
  `snap_get` at `last_applied` **under the SM lock** and keeps the whole
  KV export in `r->snap_blob` in RAM; `host_snap_get` is a full
  `efs_kv_scan` of the group. Removing the 4 MiB cap alone would turn every
  `HOST_SNAP_MIN`-entry snapshot into a multi-hundred-MB scan under `h->mu`
  on the leader — the same stall class as W13, on every snapshot instead of
  every fourth flush. The spec already says snapshots exist only for log
  truncation and the KV is durable, so the snapshot *point* should cost a
  memtable flush and a `save_snap(idx, term)`; the *export* happens only when
  a follower actually needs one, off the pump thread.
- The protocol is not an invention. Chunked InstallSnapshot with
  `offset`/`done` is the one the Raft paper specifies; the code base already
  has the "regenerate at the current applied index on demand" precedent
  (`send_snap` after a leader restart). The follower side stays
  `efs_kv_group_import` on the assembled bytes, unchanged.

Steps, once ratified (after W13's step 1, which builds the pinned view):
1. Spec: add to [architecture.md](../architecture.md) §KV the snapshot
   rule — snapshot point = memtable flushed + `save_snap`; export is lazy,
   from a pinned immutable segment view taken in one pump cycle at applied
   index N (so it is exactly the state at N); shipped as ≤ 4 MiB `SNAP_REQ`
   chunks `(incl_index, incl_term, offset, done)`; the follower stages
   chunks in `<storage>/mdraft/snap-<group>-<incl>.part`, restarts from
   `offset 0` on any `(incl_index, incl_term)` change, and on `done` imports
   the file, sets `last_applied = incl`, and truncates. Under the existing
   rule: no snapshot past the KV's durable point.
2. `raft.c`: drop `snap_blob`; `snap_get` becomes `snap_open(incl) →
   (handle, total_len)` + `snap_read(handle, offset, buf, n)` +
   `snap_close`; `send_snap` keeps one chunk in flight per peer (like
   `ae_inflight`) and advances on the `SNAP_REP` ack carrying `offset`.
   The 8-byte `app_old/app_new` prefix stays on chunk 0.
3. `raft_host.c`: `host_snap_open` pins the view in the pump cycle, exports
   it to `mdraft/snap-<g>-<incl>.kvx` on the GC thread (the one that runs
   `host_txn_recover_pass`), unpins, and serves `pread`s from the file;
   one snapshot file per group, replaced when a newer one is opened.
   `snap_oversized` and the `snapshot skipped` path go away.
4. `raft_sim` test: a follower behind a truncated log catches up from a
   snapshot that spans ≥ 3 chunks; a leader change mid-transfer restarts
   the transfer from offset 0; a follower crash mid-transfer leaves no
   `.part` applied.
5. Live gate on 19810: both groups' `raft.log` shrink and stay bounded
   (`results/measure/…-raft-snap-state` re-run: today 1.83–4.36 GB); kill
   and restart fcstor005's efsd — it rejoins by snapshot in seconds, not by
   replaying GBs; `apply_max` in `raft-obs` does not spike at a snapshot;
   `md_latency.py` unchanged. 9-host posix and the freeze script as usual.

- **Forbidden:** implementing before the row is ratified. Killing 005 to
  "fix" the lag — a 2+1 PUT needs every fragment ACK, so removing a node
  breaks writes.

#### W13 — Synchronous full-L1 compaction is the remaining election trigger — DONE Sep 26 2026

Gate is at the top of §1b. Steps 1–4 landed: pinned view, background
compactor, flush no longer compacts, and the 9-host suite kept
`apply_max` under 100 ms with no term change. Step 5 (partitioned
flush) is in the TCP build rolled Sep 27 05:07 UTC. A flush writes
one L0 file per `key[0]` (at most 16 on efs keys, shard in the first
two bytes). Compaction merges one of those ranges plus the L1 files
that overlap it, and cuts an output file when the range changes, so
the next cycle does not rewrite the other ranges. `make test` on
fcstor014 passed, including `test_partitioned_flush`. The live table's
existing L1 files are still wide until the next compaction rewrites
them; the byte bound is the steady state after that.

Measured Sep 21 (`results/measure/20260921-220933-w8-stall-timeline/obs-*.txt`):
`kv_compact_locked` runs inside the apply path under `l->mu` + `h->mu`,
rewrites all of L1 (~16 MiB; 64 MiB L1 segments) every 4 memtable flushes
because one L0 segment spans every shard and so overlaps every L1 segment.
`apply_max=2464250us` on 004 and 005 at once, group 0 term 5299→5302→5303,
client `stat` 2 s. It is the last non-hardware cause of a term change under
the 9-host suite, and it scales with table size, so it gets worse. It was
listed in §1b as "unspecified → decision"; the decision request is here.

**Recommendation: run compaction on a background thread; then, as a
follow-on, bound its per-cycle I/O with a partitioned flush. Do not raise
the memtable or the election timeout.**

- *Why background first.* The stall is a lock hold, not CPU: the apply path
  waits on `l->mu` for a 2.4 s file rewrite it does not need to observe.
  Segment files are immutable, so a compactor can read the old L0/L1 files
  and write new L1 files with no lock at all; only the manifest swap needs
  `l->mu`, and that is microseconds. The logical content of the KV is the
  same on every replica regardless of when compaction runs, so it does not
  touch determinism, the applied index, or the durability rule (the WAL and
  the manifest swap are what persist). This removes the stall at any table
  size.
- *Why not per-key-range compaction alone.* Leveled compaction bounds the
  bytes per compaction only when L0 segments have narrow key ranges. Our
  memtable holds every shard, so every L0 segment overlaps every L1 segment
  and a range compaction still rewrites all of L1. It needs a partitioned
  flush (one L0 file per key range) first — that is the follow-on, and it
  is worth doing for read cost (one `pread` per overlapping segment per
  KV read), not for the stall.
- *Why not a bigger memtable.* It makes the same stall rarer, and one stall
  is one election. Same reason the election timeout stays: a longer timeout
  hides a 2.4 s apply stall today and a 5 s one at twice the table size.

Steps:
1. **Pinned segment view** (shared with W11): `kv_lsm_view_pin()` returns a
   refcounted copy of `{l0[], l1[]}`; a segment file is unlinked only when
   the manifest no longer lists it *and* its pin count is 0. Unit test:
   a scan over a pinned view is unaffected by a concurrent flush and
   compaction.
2. **Compactor thread** in `kv_lsm`: the write path flushes memtable → L0
   as today and signals when `n_l0 >= l0max`; the compactor pins the L0 set
   plus overlapping L1, merges to new L1 files with no lock held, then
   under `l->mu` swaps the manifest (atomic rename — verify
   `kv_manifest_write` is), drops its pin, unlinks unreferenced files.
   `kv_compact_locked` remains only for `efs_kv_lsm_compact` (tests,
   `--compact` tools). Back-pressure: when `n_l0` reaches `KV_LSM_MAX_SEGS`
   the write path waits on `l->cv` for the compactor — the only stall left,
   and it means the compactor is 16× behind (log it). **Sep 29 04:27Z:
   that stall is the pump, it fired five times in one 7-minute write on
   fcstor004 (24 s once), and it is the remaining election trigger and
   REPORT BUSY — see W23 / D9.** Crash test: kill
   after every file write in a compaction, reopen, verify every key.
3. Remove the compaction call from `efs_kv_lsm_flush` (the snapshot path)
   — it only needs the memtable flushed.
4. **Gate:** `raft-obs` `apply_max` < 100 ms on every replica across ≥ 3
   compactions during the 9-host suite (log `kv-compact: start/end` with
   bytes and ms); `tests/measure/w8_stall_timeline.sh` shows no term change
   on either group through the run; `md_latency.py` within the idle
   reference; `make test` green with the two new tests; 9-host posix
   ≥ the current 185–197/201.
5. **Follow-on (after gate):** partitioned flush — write the memtable as
   one L0 file per shard range so compaction touches only overlapping L1
   segments; gate = bytes rewritten per compaction bounded by
   `KV_LSM_L1_TARGET` × ranges touched, read `pread` count per KV get ≤ 3
   on a 1 GiB table.

- **Read:** `src/kv/kv_compact.c`, `src/kv/kv_lsm.c` (`efs_kv_lsm_flush`,
  `kv_maybe_flush_locked`), the "One KV WAL fsync per pump cycle" and
  "Synchronous full-L1 compaction" learnings in the project state rule.
- **Forbidden:** raising the election timeout; raising `KV_LSM_MEM_DEFAULT`
  as the fix; any compaction step that holds `h->mu`.

#### W14 — Server: snapshot install and the fragment probe are on the write path

**Status (Sep 28 evening).** Step 1 is done in `7eecf1d` (the diff is
prepared on the GC thread; the pump applies it on the ack) and
`bbcbcb5` (the pump's `host_snap_open` no longer flushes a memtable it
cannot place; a no-progress snapshot ack waits one heartbeat). The
`send_snap` and `kv_flush_locked` stacks are gone from the run-3
profiles. Step 3's reap runs at `efsd` start. Step 4 carries
`path_hint` on the next PUT and probes the other roots only on a
miss. Step 5 sizes the send-buffer pool to `EFS_WRITE_PIPELINE`.
Step 2 (the election trigger) is still open. The
L1 cap that kept fcstor004/005 from compacting is removed in
`bbcbcb5` (growable L1 list; L0 still 64).

**Source (Sep 28 2026, 13:30–13:45 EDT).** `perf record -F 499 -g` attached
to all four `efsd` on 19810 (RDMA, `2f086f5a0ef8-dirty`) while fstor007 ran
`ecopy` and then a `dd` that did not start. Reports:
`~/orcd/scratch/efs/perf/efsd-19810-fcstor00{3,4,5,6}/{flat,by_thread,callers}.txt`.
The recorded binary was replaced on disk by the roll, so user frames are
raw addresses in the reports; resolve them with
`addr2line -f -C -e /tmp/efs/efsd <addr>` on that host (the file there is the
same build). fcstor005, 265K samples:

| share | stack | what |
| --- | --- | --- |
| 16% | pump → `drain_inbox` → `on_snap_req` → `host_snap_chunk` → `efs_kv_group_import` → `efs_kv_scan` (`lsm_scan_from`), `memcmp` 12% self | a follower installing a snapshot: full local KV scan under `l->mu`, then sort + diff of every key, **on the pump thread under `h->mu`** |
| 12% | `writer_thread` → `run_job` → `server_write_fragment_with_sum_sync` → `pwrite` | fragment writes (the useful work) |
| 10.6% | `server_handle_conn` → `recv` (`copyout`) | PUT payloads over TCP |
| 10.4% | `server_handle_conn` → `server_find_fragment_root` → `access()` ×6 (`__d_lookup_rcu`, `link_path_walk`) | six path walks per PUT to find which of the six roots has the fragment |
| 8.6% | `host_gc_thread` → `host_snap_export_pass` → `vx_pop` / `msort` | a leader exporting a snapshot |
| 7.5% | `compactor_main` → `kv_compact_locked` → `cm_push` | L1 compaction |

`raft-obs` in the same window: group 0 changed term at 13:31, 13:37, 13:40,
13:43–13:44; group 2 at 13:39–13:40. Every leader change re-exports and
re-ships a snapshot. Every server holds 5–12 abandoned
`/data1/01/efs/mdraft/snap-*.kvx.tmp` files (34 MB–960 MB each, from 00:41
through 11:24 that day); nothing removes them.

**Why the `dd` did not start.** `create` is a group-0 commit that waits in
`host_wait_settled` for the pump. The pump was inside
`efs_kv_group_import` under `h->mu`, and group 0 was re-electing. The
client's 16 BUSY/NOT_PRIMARY retries back off to 800 ms each, so the
`open()` sat for over ten seconds with nothing printed.

Steps, in this order; each is its own change with its own gate:

1. **Take `efs_kv_group_import` off the pump thread.** The scan and the sort
   run on the GC thread (the export already does). Only the resulting diff
   batch is applied under `h->mu`. `on_snap_req` hands the finished `.part`
   file to that thread and answers the leader when the diff has been
   applied. Gate: `perf` on a follower during a forced InstallSnapshot
   (`tests/measure/i17_leader_freeze.sh` or a bounce of one voter) shows
   `efs_kv_scan` off the pump, and `raft-obs` `apply_max` stays under
   70 ms while the install runs.
2. **Find the election trigger and stop it.** Run
   `tests/tools/raft_log_tail.py` on fcstor004's `raft.log` for the
   13:37 and 13:43 windows. If step 1 removes the term changes under the
   same load, this is done. If not, the follower that cannot answer
   AppendEntries during an import needs the heartbeat answered from a
   thread that is not importing. Do not raise the election timeout.

   **Measured cause candidate (Sep 28 22:19 EDT, after step 1 landed in
   `bbcbcb5`).** Terms still move: group 0 9309 → 9368 and group 2
   2763 → 2789 between 00:39Z and 02:19Z with no roll in between.
   fcstor004 (`raft_id` 1; follower in group 2 whose leader is
   fcstor006 = `raft_id` 3) prints
   `raft-obs: tx->3 enq=50698 drop=72114 sent=50695 hi=2048`: the
   per-peer outbox to the group 2 leader hit `HOST_OUTBOX_MAX` (2048)
   and **dropped more frames than it sent**. `outbox_must_keep` protects
   only `SNAP_REQ` and entry-carrying `AE_REQ`; the victims are
   AppendEntries **replies**, heartbeats, and vote traffic. A leader
   whose follower's replies are dropped re-sends after the `ae_inflight`
   timeout and never sees the match advance; a follower whose heartbeats
   are dropped on the way in campaigns. `rtt_avg=239us` on that link and
   a synchronous request/reply `host_sender` bound one peer link to
   roughly 4 000 frames/s, which the leader's AE rate under 36 writers
   exceeds; the excess is dropped, not queued. `wait_timeouts=366`
   on the same node are the client-visible BUSYs (W16).

   Fixed implementation, in this order:

   **(a)–(b) are in tree and rolled 02:35Z Sep 29.** Gate result: `drop=0`
   on every `raft-obs tx->` line on all four nodes over a 9-client
   `run.sh ior` (was `drop=72114 > sent`), but terms still moved
   (group 0 +45, group 2 +105). The drops were not the trigger; the
   follower's InstallSnapshot import is — see **W22** / D5. `hi` still
   reaches 2048 on fcstor003→1 and fcstor004→2 (entry lane full → AGAIN
   to `send_ae`, no drop), which is (c)'s measurement.

   a. **Coalesce instead of drop.** An AE reply to peer P supersedes any
      older AE reply to P still queued (only the latest `match`/`term`
      matters); a heartbeat (empty `AE_REQ`) supersedes an older queued
      heartbeat to P; a vote/pre-vote message is never dropped. When the
      outbox is full, replace the superseded item in place; only if
      nothing is replaceable return `EFS_ERR_AGAIN` to the caller (as
      `send_ae` already handles for entry AEs). Gate: `drop=0` in every
      `raft-obs tx->` line over a 9-client `run.sh ior`; both groups'
      terms unchanged across the run.
   b. **Do not let one peer's backlog hide a heartbeat.** If (a) leaves
      `hi` at the cap, the sender needs the heartbeat ahead of queued
      entry AEs: dequeue heartbeats and replies before entry AEs
      (two-lane outbox), keeping entry AEs in order among themselves.
   c. **The per-link rate is a design question.** `host_sender` waits
      for `RAFT_REPLY` per frame. Letting more than the one in-flight
      batch ride a link is pipelining and is forbidden below; batching
      several reply frames into one RPC is not, but it changes the wire.
      Measure after (a)+(b): if `hi` still reaches the cap with
      `drop=0`, bring the frames/s and RTT numbers to the user.
3. **Reap dead `snap-*.kvx.tmp`.** At `efsd` start, and whenever an export
   is abandoned (`send_snap` BUSY path, leader step-down), unlink every
   `snap-<group>-*.kvx.tmp` that is not the one in progress. Gate: after
   a roll, `ls /data1/01/efs/mdraft/*.tmp` is empty on all four.
4. **Drop the six `access()` calls per PUT.** **Review after the Sep 29
   02:35Z run:** the hint as specified only helps a re-PUT of the same
   fragment; a first write has no previous PUT to carry a hint from, so
   IOR-easy and a fresh `dd` still walk all six roots (fcstor004: 1.59M
   `access()`, 192 s across six handler threads). **Step 4 (b), decided
   D7 (Sep 29):** the client sends `path_hint = 0xffffffff` when its
   dcache slot has never PUT this fragment generation (no recorded hint,
   not a STALE replay, not a retry of a failed reply); the server then
   skips `server_find_fragment_root` and creates on the writer's
   least-queue root. A fragment name is `{ci}.{fi}.{gen}`, so a first
   write of a new generation cannot collide with an existing file; the
   quota charge stays on the create path. Any re-PUT sends the recorded
   root or 0. Gate: the step-4 gate below (under 1 % on a single-client
   dd) plus `peer_shared_pwrite` / `concurrent_appends` (re-PUT paths
   still probe). Order-table row 8c. A global fd cache hung the
   9-client dd; a thread-local fd cache removed the sample and made the
   slowest client worse (both reverted, see `docs/project-history.md`
   Sep 28). Do not retry either. The PUT reply already tells the client
   which storage path took the fragment; carry that `path_index` back on
   the next PUT of the same `(ino, ci)` as a hint and probe only on a
   miss. Gate: `server_find_fragment_root` under 1% in a single-client
   8 GiB dd profile, and the 9-client dd slowest wall not worse than
   `results/measure/20260928-134637-dd-prof-r5b` (2551.5 MiB/s).
5. **`send_buf_pick` spin.** `pthread_spin_lock` inside
   `efs_rdma_send_frame` is 2.3% of the client and has a server
   counterpart. Size the send-buffer pool per QP so a pick does not
   contend, or hand each sender its own ring. Gate: the spin is gone from
   `flat.txt` on both sides.

- **Gate:** items above, plus posix 1 jobs=1 and 9-host, posix 2, and a
  1-client and 9-client 8 GiB dd with the flush in the clock, none worse
  than the Sep 28 numbers in `.cursor/rules/efs-fio-honest.mdc`.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`;
  chunking InstallSnapshot differently (W11 is done); the global or
  thread-local fd cache; changing `EFS_RAFT_SNAP_CHUNK`, `HOST_PUB_BATCH_N`
  or `EFS_RAFT_AE_BYTES` (all three were measured worse on Sep 28).

#### W15 — Client: copies and busy-waits are the write CPU

**Status (Sep 29).** Step 2 is done in `bbcbcb5`
(`efs_rdma_reply_ready_us` pauses at most 16 times, then the caller
blocks on the CQ fd); on the run-3 profile the vDSO is 0.87% and the
symbol is under the 0.5% floor. Step 1's fresh profile is run 2 /
run 3 in §1b: `ll_write_buf` `memmove` 7.5%, RDMA send `memmove`
6.3% (step 3 stands), and no `memmove` caller under
`dcache_flush_slot_inner` above the 0.5% floor (the body-drop path is
taken). Run 2's client report files were overwritten by run 3 (same
`efs-mount` directory); those three numbers survive only here. The
larger client item was the reclaim walk, W18, which is in tree.
Step 3 hands a whole chunk to the dcache after one `fuse_buf_copy`
(the request buffer does not outlive the reply). Step 4 is the
same send-buffer pool as W14 step 5.

**Source.** `client.sh --perf` on fstor007 during the same `ecopy`
(`~/orcd/scratch/efs/perf/efs-mount/{flat,by_thread,callers}.txt`,
371K samples). This profile is of the binary **before** the Sep 28
dcache changes (full-chunk body drop, no snapshot copy, running
`staged_bytes`); take a fresh one first and strike whatever those already
removed.

| share | where |
| --- | --- |
| 36% | `memmove`: 15.4% in `ll_write_buf` (FUSE copy-in + dcache patch), 3.5% into the RDMA send buffer, the rest under `dcache_flush_slot_inner` (snapshot copy + `dcache_install_image` copy-back) |
| 7.5% + 5.8% | vDSO `clock_gettime` and `efs_rdma_reply_ready_us` from `efs_conn_reply_watch_us`: busy-waiting for the PUT reply |
| 7.1% | `blake3_hash_many_avx512` |
| 5.2% | `efs_export_staged_bytes` (the evictor walk; replaced by a running total Sep 28) |
| 4.6% | `xor_into` (parity) |
| 2.3% | `pthread_spin_lock` in `efs_rdma_send_frame` (`send_buf_pick`) |
| 2.2% | `__lll_lock_wait` from `dcache_put_now` |

Kernel futex + schedule under 3%: the client is not lock-bound. It is
copying and spinning. `ecopy` and `dd` on the same mount were not blocked
by each other; the `dd` was blocked on the server (W14).

**Re-profile, Sep 28 20:36–20:39 EDT (fstor007 `client.sh --perf`,
214K samples, same report dir, binary with the body-drop and
`dcache_ensure` changes).** `memmove` is still 35%. Resolved by call
address (`perf report --no-children --symbol-filter=memmove -g
caller,address` on fstor007): 14.6% under `ll_write_buf`
efs_fuse.c:2569 (the `efs_dcache_try_patch` copy bounce → dcache),
3.6% into the RDMA send buffer, and **~17% with no caller at all**.
That last share is `fuse_buf_copy` in `efs_fuse_write_buf` (libfuse
is built without frame pointers, so the chain stops at `memmove`).
Every written byte is copied kernel → libfuse buffer (`memcpy_erms`
1.6%), libfuse buffer → bounce (`fuse_buf_copy`, ~17%), bounce →
dcache (14.6%), dcache → RDMA send buffer (3.6%). Step 3's gate counts
the bounce copy too, not only the `ll_write_buf` line. `memmove` under
`dcache_flush_slot_inner` is gone (body drop landed). The vDSO share is
back at 5.7%, but from a different place: 2.5% `now_us()` in
`send_buf_pick` (inlined into `efs_rdma_send_frame`) plus 1.7%
`pthread_spin_lock` there — waiting for one of `EFS_RDMA_NSEND` = **2**
pool send buffers per QP; a third 64 KiB PUT on the same conn spins.
That is step 4 / W14 step 5, with its cause named. Two things in this
profile are not on any item and are now **W20** and **W21** below:
`ll_setattr` → `efs_fuse_getattr_ino` → `fill_stat_from_inode` is 8.9%
(a stat walks every chunk of the file), and `efs_export_staged_bytes`
is 6.6% self on the evictor thread (it was not replaced by a running
total; the per-call loop over `shard_tabs` and up to 1024 passes per
wake are still there).

Steps:

1. **Re-profile on the current binary** (one client, 8 GiB `dd bs=1M
   conv=fsync`, non-zero source, `client.sh --perf`, `stop` writes the
   reports). Record the three shares above again. If `memmove` under
   `dcache_flush_slot_inner` is still above 5%, the body-drop path is not
   being taken for the sequential write; find out why before anything
   else.
2. **Replace the reply busy-wait.** `efs_conn_reply_watch_us` +
   `efs_rdma_reply_ready_us` spin on `clock_gettime`. A PUT reply is
   behind a disk write, tens of milliseconds. Spin for at most ~50 µs,
   then block on the CQ event fd (`rc->efd`) the way `efs_rdma_recv_wait`
   already does. Gate: the two symbols together under 2% on the dd
   profile; the 1-client dd wall not worse.
3. **Cut the FUSE copy-in for whole chunks.** `ll_write_buf` copies the
   libfuse buffer into the dcache. When one write covers a whole 128 KiB
   chunk, hand the libfuse buffer to the slot as the body (the body-drop
   path then sends it and frees it). Check `fuse_buf_copy` ownership in
   libfuse 3.10.2 before assuming the buffer outlives the reply. Gate:
   `ll_write_buf` `memmove` under 5%.

   **Review of the in-tree version (Sep 28 22:00, `efs_fuse_write_buf`
   "W15.3").** It copies once only when `size == cs && offset % cs == 0`,
   i.e. a write of exactly one chunk. `max_write` is
   `EFS_WRITE_PIPELINE × cs` (≥ 1 MiB), so `dd bs=1M`, IOR `-t 1m`, and
   every `cp` deliver 1 MiB writes = eight chunks, and those all fall
   through to the bounce + `efs_dcache_try_patch` path. The 22:11
   profile of this binary shows it: `fuse_buf_copy` ~19% + `ll_write_buf`
   16%. Fixed implementation: walk the write in chunk-aligned pieces.
   For every whole chunk inside `[offset, offset+size)` allocate the
   dcache-owned buffer (`efs_buf_alloc(cs)`), set `dst` to that one
   buffer and call `fuse_buf_copy(&dst, buf, FUSE_BUF_NO_SPLICE)`;
   `fuse_bufvec` keeps the source position (`buf->idx`, `buf->off`), so
   successive calls consume the request buffer in order without a
   bounce; store each with `efs_dcache_store_full_owned`. Only a partial
   head or tail chunk goes through a bounce of its own length into
   `efs_dcache_try_patch`. Keep the O_APPEND reservation and the
   `stage_unpin` exactly where they are. If a mid-write store fails,
   fall back to the old path for the rest of the request (no partial
   success visible to the caller). Gate: on a 1-client `dd bs=1M`
   profile, `memmove` under `ll_write_buf` under 2% and total `memmove`
   with no caller under 5% (that is the single remaining copy); posix
   2 `peer_shared_pwrite` / `concurrent_appends` unchanged.
4. **`send_buf_pick` spin** — same as W14 step 5, one change for both
   sides.

   **Review (22:11 profile, `NSEND = EFS_WRITE_PIPELINE` in the binary).**
   The vDSO + `pthread_spin_lock` share did not move (6.9%). The
   callers are `now_us()` and the spin at `efs_rdma_send_frame`
   lines ~1257–1301: after `post_send` the sender **waits for its own
   send CQE** (spin 200 µs, then `sched_yield` until `send_busy[idx]`
   clears). More pool buffers cannot help; each PUT fragment still costs
   the thread one HCA round trip before it can post the next. The
   comment says the wait exists so a failed first SEND after upgrade
   surfaces as EIO instead of hanging in recv. On the 22:48 single-`dd`
   profile the vDSO share was 0.8%, so the wait costs CPU mainly when
   several writers share one QP; the gate below still applies to the
   seven-`dd` shape. Fixed implementation:
   post and return; reap send CQEs where they are already reaped
   (`send_buf_pick` on the next send, and in the reply wait —
   `efs_rdma_recv_wait` / `efs_rdma_reply_ready*` call `reap_sends`
   before blocking, and a send CQE with an error status marks
   `rc->broken` and makes the pending reply wait return `EFS_ERR_NET`).
   That preserves the "failed send becomes EIO on the reply path"
   property without a synchronous wait. Keep the 5 s
   `EFS_RDMA_SEND_WAIT_US` as the reply-wait's dead-QP bound. Gate:
   vDSO + `pthread_spin_lock` under 1% on the dd profile;
   `tests/test_rdma_xprt` and `tests/rdma_first_inode.sh` pass
   (the first-SEND-after-upgrade case is the one the wait was added
   for); 9-client dd not worse than 2551.5.
5. **The last user-space copy: let the kernel fill the dcache buffer.**

   **Checked Sep 29 15:33Z on fstor007 (`~/efs-runs/rec-splice1.log`):
   cannot take effect on this cluster as configured.** libfuse
   3.10.2's `fuse_session_receive_buf_int` uses the pipe only if it
   can grow it to `se->bufsize` = `max_write` + 4 KiB; `efs_fuse_init`
   sets `max_write` to `EFS_WRITE_PIPELINE × cs` = 4 MiB, and
   `/proc/sys/fs/pipe-max-size` is 1048576, so `F_SETPIPE_SZ` fails
   for a non-root process and libfuse falls back to the buffer path
   for good (`can_grow = 0`). **Resolved 15:45Z: the user set
   `fs.pipe-max-size=8388608` on node9901, fstor007, fcstor003–015
   (`sysctl -w`, runtime only — it reverts on reboot like
   `ptrace_scope`; ask the user to re-apply, do not edit
   `/etc/sysctl.d`).** `efs_fuse_init` now sets `FUSE_CAP_SPLICE_READ`
   when libfuse offers it. Rolled 16:09Z on the servers; clients are
   not mounted, so this is not gated. The gate
   below stands; the `fuse_copy_page` share is the number to compare.
   Lowering `max_write` under 1 MiB was the other route and was not
   taken (every `dd bs=1M` write would become two requests).

   After step 3 (verified 22:48: `ll_write_buf` `memmove` gone) every
   written byte is still copied twice: kernel → libfuse request buffer
   (`fuse_copy_page`/`memcpy_erms`, ~7% + 2.5% kernel) and libfuse
   buffer → dcache (`fuse_buf_copy`, ~22% user, caller-less in perf
   because libfuse has no frame pointers). libfuse 3 can receive a
   write's payload through a pipe instead of its buffer
   (`FUSE_CAP_SPLICE_READ`; `fuse_session_receive_buf` marks the
   payload `FUSE_BUF_IS_FD`), and `fuse_buf_copy` from an fd buffer
   into a memory buffer is one `read()` from the pipe into the
   destination — the dcache buffer step 3 already allocates. Net: one
   kernel copy per byte, none in user space. Do this as its own
   change: in `efs_fuse_init` set `conn->want |= FUSE_CAP_SPLICE_READ`
   when `conn->capable` has it; keep `FUSE_BUF_NO_SPLICE` on the
   `fuse_buf_copy` calls (it only forbids `splice()` for fd→fd, which
   we never do). Verify first, on fstor007, three things libfuse
   3.10.2 decides at runtime: that a 1 MiB write is actually delivered
   as an fd buffer (libfuse copies small requests to memory; check the
   threshold in `fuse_session_receive_buf_int`), that the pipe can hold
   `max_write` + header (`F_SETPIPE_SZ` vs `/proc/sys/fs/pipe-max-size`,
   1 MiB default — if the cap is below `max_write` + 4 KiB the request
   falls back to the buffer path and nothing changes), and that the
   `write_buf` path is the only consumer (`efs_fuse_write` with a plain
   pointer must not be reachable for fd buffers). Gate: on the 1-client
   `dd bs=1M` profile, caller-less `memmove` under 3% and total
   `memmove` under 12%; `fuse_copy_page` unchanged or lower; posix 1
   jobs=1 200/201, posix 2 63/63 (`peer_shared_pwrite`, appends,
   O_DIRECT tests); 1-client dd wall not worse than 947 RDMA.

- **Gate:** posix 1 jobs=1 (200/201, `mmap_write_read` SKIP only), posix 2
  63/63, 1-client 8 GiB dd ≥ 977 MiB/s TCP / 947 RDMA (the Sep 27–28
  numbers), 9-client dd not worse than 2551.5.
- **Forbidden:** clearing `FOPEN_DIRECT_IO`; touching `entry/attr_timeout`;
  replacing blake3 (the hash is not the wall; W3); putting the
  `sched_yield` loop back in `recv_poller` or the 200 µs clock spin back
  in `efs_rdma_recv_wait`.

#### W16 — Under a write flood, a metadata read fails after 10 s and surfaces as ENOENT

**Source (Sep 28 2026, ~18:30 EDT).** fstor007-mgmt mounted 19810 with
`client.sh --perf` while the 9×4 IOR was writing. Mount succeeded
(`fuse serving`, RDMA up). `df -h /tmp/efs-mount/` printed
`No such file or directory`. The fuse log has one line for it:
`inode-rpc: shard=0 type=47 exhausted 16 BUSY/STALE retries (10.3 s) -> EBUSY`.
Type 47 is `EFS_MSG_INODE_GETATTR`; shard 0 is the root's shard (even →
group 2). The mount was alive; `client.sh stop` unmounted cleanly. Same
class as the `INODE_LOOKUP` on shard 3745 that aborted IOR's `stat` in the
22:00Z run (§1b). Not data loss, not a dead mount, not RDMA.

Two defects, one visible and one underneath:

1. **Error mapping.** `efs_client_stat_ino` and `efs_client_stat_refresh`
   (`src/client/ops.c`) return `EFS_ERR_NOT_FOUND` for *any* RPC failure;
   `ll_getattr` (`src/client/efs_fuse.c`) maps every non-ACCES failure to
   `-ENOENT`. An overloaded cluster therefore looks like a missing
   directory to `df`, `ls`, and every path walk. The comment above the
   `no-hint` retry in `rpc_send_recv_shard` already names this.
2. **The read starves.** A GETATTR is a linearizable read:
   `host_read_index` records `commit` on arrival and waits for a read
   round and `applied ≥ read_index`; `host_wait_applied` has the same
   budget. Both give up after `HOST_READ_TRIES × HOST_TICK_US` = 400 ms
   with BUSY (`obs_wait_timeouts` counts it). The client
   (`rpc_send_recv_shard`) retries 16 times with `50 ms << min(attempt,4)`
   backoff, ~10.35 s total, then returns `EFS_ERR_BUSY`. So the observed
   line means group 2 could not satisfy a read for ten seconds straight:
   either `applied` trailed `commit` by more than 400 ms the whole time
   (36 ranks publishing; REPORT/publish applies on the pump; L1 pressure
   from this same IOR is recorded in §1b), or group 2 was re-electing
   (a hintless NOT_PRIMARY lands in the same BUSY/STALE bucket). A fresh
   client is the victim because its first op is the root GETATTR and it
   has no cached row; the IOR ranks mostly write and read their own
   dcache.

Steps, in this order:

1. **Fix the mapping first (client, small).** Carry the RPC rc out of
   `efs_client_stat_ino` / `efs_client_stat_refresh` (return it, do not
   fold to NOT_FOUND). In `ll_getattr` and the `lookup_fill` path map
   `EFS_ERR_BUSY` and `EFS_ERR_NOT_PRIMARY` to `-EBUSY` (rmdir/unlink
   already use EBUSY for `EFS_ERR_BUSY`), `EFS_ERR_NET`/`EFS_ERR_IO` to
   `-EIO`, and only a real `EFS_INODE_RPC_NOT_FOUND` to `-ENOENT`. Grep
   every caller of both functions (there are ~10 in `efs_fuse.c`) —
   some legitimately treat "cannot read parent" as ENOENT for a *child*
   lookup; those need the same split. Gate: posix 1 jobs=1 200/201,
   posix 2 63/63, and a forced 10 s BUSY (step 2's repro) makes `stat`
   return EBUSY, never ENOENT. If the user prefers EIO over EBUSY for
   stat, that is a one-line choice; ask before choosing EIO.
2. **Reproduce and attribute, then decide which server fix applies.**
   Script under `tests/measure/` (runbook in runbooks.md): start the 9×4
   IOR (`run.sh debug`), mount a 10th client on node9901 or fstor007
   (TCP or RDMA, whichever the cluster runs), and once a second run
   `stat /tmp/efs-mount/` recording errno + wall, while sampling
   `efs-mgmt raft-status` (`commit − applied`, `leader`, `term` per
   group) and grepping the leaders' `efsd.log` for
   `raft-host: read-sleep` / `apply-sleep us=` and `raft-obs`
   `obs_wait_timeouts`. Outcome A: `commit − applied` on group 2 stays
   above what 400 ms of apply covers for the whole IOR → the read is the
   messenger and the fix is apply throughput (W14 step 1 plus the cost of
   publish/REPORT apply under load; measure `apply_max` and per-cycle
   apply count first). Outcome B: term changes line up with the BUSY
   window → it is the W14 step 2 election trigger. Outcome C: lag is
   bursty (compaction/flush stalls of ≥ 400 ms) → it is L1/L0 pressure
   (§1b, W13's follow-on), not the read path. Record which; do not
   guess.
3. **Only after step 2:** if the lag is steady-state and apply throughput
   cannot close it, bring the trade-off to the user: readers currently
   wait for `applied ≥ commit-at-arrival`, which is the linearizable
   contract; a read that gives up after 400 ms and a client that gives
   up after 10 s are policy numbers the spec does not set. Do not change
   either number on your own.

**Step 2 outcome, first sample (Sep 28 22:11–22:19 EDT, no script yet).**
Seven parallel `dd bs=1M` from fstor007 into new files in one directory;
one `dd` got `open: Device or resource busy` (step 1's mapping is in
the binary, so this is the same failure that printed ENOENT before).
The fuse log for that mount holds **249** `exhausted 16 BUSY/STALE`
lines: LOOKUP 98, SETATTR 50, GETATTR 47, RENAME 31, CREATE 23. Every
`inode-rpc: retry … why=no-hint` line is a NOT_PRIMARY with no leader
hint. `raft-status`: group 0 term 9368 (was 9309 at 00:39Z), group 2
term 2789 (was 2763), both `commit == applied` when sampled;
fcstor004 `raft-obs`: `wait_timeouts=366`, `apply_max=30us`,
`apply-sleep` up to 57 ms on single entries, and the outbox
`drop=72114 > sent=50695` to the group 2 leader. **That is Outcome B:
elections, not apply lag.** The fix is W14 step 2 (a)–(c); W16 step 3's
policy question does not arise until W14 step 2 is done and this is
re-sampled with the script. Still write the script: it is the gate.

**Sep 29 02:35Z re-sample, and D8 (decided).** With W14 step 2 (a)–(b)
rolled, `drop=0` on every outbox and the terms still moved (+45 /
+105) — the trigger is the InstallSnapshot import (W22, D5). The
write-side wait now has a number: `report-split nrec=86234
pack_ms=1431 push_ms=9869 finish_ms=9227` — 337 publish batches at
~29 ms each through the one in-flight batch. **D8:** before any
read/wait policy or budget question, add `pub_batch_ms` (p50 / max
per group, the time from a batch's propose to its apply) to
`raft-obs`, run one 9-client `run.sh ior` after W22 steps 1–2 are
rolled, and bring the number: ~2–6 ms per batch means the 29 ms is
queueing behind the other eight clients' batches and the lever is
fewer, larger entries per REPORT (a wire question — ask); ~100 ms
means the `fsync` mode and D6 is the lever. Pipelining past one
in-flight batch stays forbidden. Order-table row 10.

- **Gate:** step 1's mapping gate; step 2's script committed with one
  attributed run in `results/measure/`; §1b updated with the outcome.
- **Forbidden:** raising `HOST_READ_TRIES`, `HOST_TICK_US`, or the
  16-attempt client budget to make the symptom go away; serving a
  GETATTR from the follower's or client's local state without the read
  round (that is the linearizability I2/I10 guarantee); touching
  `entry/attr_timeout`; clearing `FOPEN_DIRECT_IO`.

#### W17 — Nine writers on one file: publish STALE storm, fsync EIO, and a FUSE request that outlives its process

**Source (Sep 28 2026, 22:14–22:56Z, `bbcbcb5`).** ior-hard, 9 ranks,
one file (ino 1166063), 47008-byte records, `run.sh ior`. Evidence:

- fcstor004 `efsd.log`: a run of `raft-host: apply publish rc=-14
  index=… ino=1166063 ci=…` with distinct chunk indexes (512867,
  617329, 566269, 856797, …). `-14` is `EFS_ERR_STALE`: the CAS base
  the client reported is no longer the published generation because
  another rank published that chunk in between.
- Every client `fuse.log`: `inode-rpc: retry type=67 … why=recv rc=-6`
  then `inode-rpc: dual type=67 exhausted 16 BUSY/STALE retries
  (10.3 s) -> EBUSY`, several times per client. Type 67 is
  `EFS_MSG_REPORT_CHUNKS`. The BUSY is the server's
  `host_wait_applied` 400 ms budget inside `host_pub_batch_wait`
  (group 2 apply lagging under nine clients' publish batches), and the
  STALE is `host_pub_batch_wait` reporting any STALE verdict for the
  batch.
- fcstor013: `efs-fuse fsync: resource busy (efs_rc=-13) ino=1166063`
  → `efs_fuse_fsync_ino` returns `-EIO`. IOR: `WARNING: fsync(19)
  failed` ×8, `ERROR: close(20) failed`, rank 8 `MPI_ABORT`.
- After the abort, `io500` on fcstor007–010 and 012–014 sat in
  D-state `request_wait_answer` for 20+ min (SIGKILLed, still waiting
  on a FUSE reply). The reply they wait for is a `flush`/`release`/
  `fsync` whose `efs_client_report_dirty_ino(ino, sync=1)` is still in
  its loop: up to 64 STALE rounds, each a `stale_repull_replay` (GET +
  PUT of every moved chunk) plus a REPORT that can itself take the full
  16-retry 10.3 s, and up to 8 BUSY rounds on top. The loop is bounded
  in code and unbounded in practice.

This is the W1 N-1 CAS shape (36-way sub-chunk CAS on Sep 21 gave
hard-write 0.046 GiB/s) with nine whole ranks instead of sub-chunk
writers. The 4-rank shared-file IOR passed on Sep 27 (481.56 MiB/s,
`results/posix2/20260927-190509`); nine did not.

What to implement, in this order:

1. **A FUSE request returns.** `efs_client_report_dirty_ino` with
   `sync=1` must have a wall-clock bound that is shorter than what the
   kernel will wait, and the bound must cover the whole loop (STALE
   rounds × REPORT retries × BUSY retries), not one leg. When the bound
   is hit, `fsync` returns EIO with the dirty set merged back (the data
   is not lost; the next fsync retries). Measure first: add a counter
   line at loop exit (`report-loop ino= rounds= stale= busy= ms= rc=`)
   and run ior-hard NP=9 once to record the distribution. Then bring the
   number to the user — the 64-round and 16-retry budgets are policy
   the spec does not set. Gate: after `pkill -9 -x io500` mid-ior-hard,
   `pgrep -x io500` is empty within that bound on all nine clients
   and `efs-fuse` is still serving (`stat` OK).
2. **Attribute the STALE per chunk.** Log, on the server, how many
   publishes of one `(ino, ci)` lost the CAS within one fsync window
   (`raft-obs` counter `pub_stale` per group is enough), and on the
   client how many bytes each `stale_repull_replay` re-PUT versus how
   many bytes it owned. If a 47 KB record costs a 128 KiB GET + PUT +
   re-REPORT per losing rank, that ratio is the number to show the user.
   The `delta_off`/`delta_len` fields in `efs_chunk_rec` exist for a
   sub-chunk span publish; check whether the replay path fills them or
   always re-publishes the whole chunk, and record which.
3. **Decided (D1, Sep 28): make the span publish commute.** Three
   changes, in `efs_meta_apply_publish` and `dcache_flush_slot_inner`:
   - **Server:** for `delta_len > 0`, drop the `expected_gen != committed
     → STALE` check; a span attaches to whatever base is current. Keep
     the check for full-image publishes (`delta_len == 0`), which still
     require the live delta list to match exactly. When a fold replaces
     the base, write the folded spans' `candidate_gen`s into the new
     trailer (≤ `EFS_CHUNK_DELTA_MAX` entries); a span whose
     `candidate_gen` is in that list is a replay → OK, no-op. Overlap
     with a live span stays STALE (the client re-pulls that span's
     bytes only). A full chain still returns STALE to the publisher,
     who folds — that publisher, not the others.
   - **Client:** a slot whose dirty ranges cover less than the chunk
     publishes a span **even when `have_base` is set**
     (`span_of` regardless of `have_base`); the full-image path is for
     `dcache_full_overwrite` and for the fold. `stale_repull_replay`
     on an overlap STALE re-pulls and re-publishes the overlapping span,
     not the chunk.
   - **Reader:** `efs_meta_apply_get_chunk_deltas` and the client's
     base+spans overlay are unchanged; a reader that sees a full chain
     may fold as a background PUT, off the read.
   Gate: `test_meta_apply` gains "span after fold is OK",
   "replay of folded span is no-op", "overlapping span is STALE";
   `peer_shared_pwrite` and `concurrent_appends` pass; ior-hard NP=9
   and NP=36 SEGS=3000 cold verify bad 0 with `pub_stale` per fsync
   at 0 in steady state and the write rate rising from NP=4's 481.56
   MiB/s.

- **Gate:** step 1's bound + kill test; step 2's numbers in
  `results/measure/` with the script under `tests/measure/`; step 3's
  gate; ior-hard NP=4 SEGS=3000 cold verify still 12000 records bad 0;
  posix 1 jobs=1 200/201; posix 2 63/63.
- **Forbidden:** a distributed chunk lock; splitting REPORT into
  several RPCs; widening the 16-attempt RPC budget or `HOST_READ_TRIES`;
  making `fsync` return success on a merged-back (unpublished) dirty
  set; `hard_remove`; raising `EFS_CHUNK_DELTA_MAX` to avoid the fold;
  folding on a rank's `fsync` other than the one that filled the chain.

#### W18 — Client: the dcache reclaim is a table walk, and it is most of the client's CPU under a long write

**Source (Sep 28 2026, run 3, fcstor007 `efs-fuse`, 917K samples over
~40 min).** `by_thread.txt`: `dcache_flush_slot_inner` 43.8% **self**,
`pthread_once@GLIBC` 25.0% self, `pthread_mutex_lock` 8.3% +
`unlock` 3.5%, blake3 3.3%, `memmove` 2.4%. `callers.txt`:
`dcache_reclaim_main` is 73.5% of the process. The 5-minute profile
from run 2 (36K samples) had blake3 at 27% and this function's self
time under the floor; the long capture is the one that shows the
steady state.

Why it looks like this (`src/client/write.c`):

- `DCACHE_SLOTS` is 65536 hash buckets of chained `dcache_ent`;
  `DCACHE_RECLAIM_THREADS` is 16; each wake walks
  `DCACHE_RECLAIM_SCAN` = 64 slots via a shared cursor and, in
  `dcache_flush_slot_inner`, walks each slot's whole chain under the
  shard mutex skipping clean entries (`!e->dirty || !e->data`). With
  the staging table "over EFS_CLIENT_META_MB (256 MB) with nothing
  evictable; growing" (every client logged this), the chains are long
  and mostly clean. The self time is the chain walk.
- `dcache_mu()` and `dcache_io_mu()` call
  `pthread_once(&g_dcache_once, dcache_init)` on every call; the walk
  calls them per slot. 25% of the client is that check.
- 64 shard mutexes for 65536 slots, 16 walkers plus the FUSE write
  threads (`efs_dcache_try_patch`) on the same locks: the 12% in
  lock/unlock.

What to implement:

1. **Reclaim from a dirty list, not a table scan.** When an entry
   becomes dirty (`dcache_note_dirty_bytes(+len)` call sites), put it on
   a per-shard dirty list; reclaim pops from that list. A popped entry
   that is no longer dirty is skipped in O(1). The chain walk in
   `dcache_flush_slot_inner` stays only for `have_only=1` (close/fsync
   of one inode), which can also use a per-inode index if the profile
   still shows it. Gate: `dcache_flush_slot_inner` self under 5% and
   `dcache_reclaim_main` under 15% of a 30 s-stonewall easy-write
   profile; `dirty_bytes` accounting unchanged (posix 2 63/63;
   `concurrent_appends` and `peer_shared_pwrite` pass).
2. **Initialize the dcache once.** Call `dcache_init` from client
   startup (the export mount path) and drop the `pthread_once` from
   `dcache_mu` / `dcache_io_mu`. Gate: `pthread_once` gone from
   `flat.txt`.
3. **Re-measure W15 steps 1 and 3** on the same profile: `memmove` in
   `ll_write_buf` (7.5% in run 2) and in the RDMA send (6.3%). Those
   items stand; this one goes first because it is the larger share.

- **Gate:** the two profile gates; 9-client 8 GiB dd not worse than
  2551.5 MiB/s; 1-client dd not worse than 947 RDMA; posix 1 jobs=1
  200/201, posix 2 63/63.
- **Forbidden:** changing the 2 GiB reclaim limit or the 2× inline-help
  cap to hide the walk; dropping the `have_base` guard for reclaim
  (the `concurrent_appends` NULs); clearing `FOPEN_DIRECT_IO`.

#### W19 — Server: the Raft pump copies every AppendEntries twice, and `try_commit` re-walks the log per reply

**Source (Sep 28 2026, run 3, fcstor003 = group 0 leader, 1M
samples).** `flat.txt`: `memmove` 18.5%, kernel `rep_movs_alternative`
17.0%, `try_commit` 4.0% self, `__d_lookup_rcu` 2.6%. `callers.txt`:

| share | stack |
| --- | --- |
| 12.6% | `host_pump` → `efs_raft_recv`/`efs_raft_flush` → `send_ae` → `host_send` → `memmove` |
| 5.2% | same → `send_ae` → `memmove` (the arena fill) |
| 6.9% | `host_sender` → `efs_conn_send_msg` → `writev` → `tcp_sendmsg` → `copyin` |
| 10.5% | `host_pump` → `efs_raft_recv` → `try_commit` (4.0% self, `log_term` → `disk_get` → mutex) |

On fcstor004 the receiving side is `server_handle_conn` → `recv` →
`copyout` 15.4%. The peer connections are TCP for frames above the
72 KiB RDMA buffer (`conn_pick_send_chan`), which is every catch-up
AppendEntries batch.

What the code does (`src/raft/raft.c`, `src/server/raft_host.c`):

- `send_ae` mallocs an `EFS_RAFT_AE_BYTES` arena and `store->get`s each
  entry into it (copy 1, `disk_get`). `host_send` encodes the message
  into a stack buffer or a heap buffer (copy 2) and, when the stack
  buffer sufficed, `malloc`+`memcpy`s it again (copy 3) before queueing
  it for `host_sender`, which `writev`s it (kernel copy 4). All but the
  last run on the pump thread, under whatever the pump holds.
- `try_commit` runs on every AE reply and walks `n = last_i` down to
  `commit_index`, calling `log_term` (a `disk_get` under the store
  mutex) for each `n`, until it finds a quorum. When a follower is
  behind, `last_i − commit_index` is large and the walk repeats per
  reply.

What to implement:

1. **One user-space copy per AppendEntries.** Encode the wire frame
   directly into the buffer `host_sender` will write: `send_ae` asks
   `host_send` (or a new `net->alloc`) for a frame buffer sized from
   the entries' `clen`, fills header + entries in place, and hands
   ownership to the outbox. Drop the arena and the stack-then-heap
   re-copy. Gate: `memmove` under `host_send` + `send_ae` under 3% of
   the leader's profile during a 9-client write; `test_raft` OK.
2. **`try_commit` from the reply's match index.** The only index whose
   match changed is the replying peer's; a commit can only advance to
   an `n` in `(commit_index, match_index[from]]` whose term is current.
   Start the walk at `min(last_i, match_index[from])` and stop at the
   first quorum; cache the term of the highest entry so the common case
   is one `log_term`. Gate: `try_commit` self under 0.5%; `test_raft`
   commit/term tests OK; no change in `commit_index` sequence on a
   replayed unit log.
3. **Measure, do not change, the transport choice.** Record how many
   AE frames per second exceed 72 KiB and their size distribution
   (`raft-obs` line). `EFS_RAFT_AE_BYTES` and the RDMA buffer size are
   not to be tuned here; if the numbers say the peer path should ride
   RDMA with a different framing, that is a design question for the
   user.

- **Gate:** the two profile gates; both groups commit==applied and no
  term change during a 9-client 8 GiB dd; posix 1 9-host not worse
  than `results/posix/20260928-034049`.
- **Forbidden:** changing `EFS_RAFT_AE_BYTES`, `EFS_RAFT_AE_MAX`,
  `HOST_PUB_BATCH_N`, `HOST_TICK_US`, or the heartbeat/election
  timeouts; taking `h->mu` in `host_sender`; pipelining more than the
  one in-flight batch per behind peer.

#### W20 — Client: `stat` is O(chunks) — `st_blocks` walks every chunk of the file under two locks

**Status (Sep 29, in tree, not measured).** `efs_inode_mem` keeps
`present_chunks` (table inserts and removals) and `present_extra`
(a dirty dcache slot that is not in the table). `st_blocks` reads
that sum and probes only a partial tail. `ll_setattr` reads `size`
from the row.

**Source (Sep 28 2026, 20:36–20:39 EDT, fstor007 `client.sh --perf`,
214K samples, `~/orcd/scratch/efs/perf/efs-mount/callers.txt`).**
`ll_setattr` 8.9% of the process, all of it
`efs_fuse_getattr_ino` → `fill_stat_from_inode` → `inode_allocated_bytes`
(`src/client/efs_fuse.c`). Inside that: `efs_dcache_has` 3.7% self,
`pthread_mutex_lock`/`unlock` 2.5%, `__lll_lock_wait` 2.0% (the futex
sleeps under `fill_stat_from_inode` are 1.4% of the kernel time; the
flush path holds `idx_mu` and the dcache shard mutexes).

Why: `inode_allocated_bytes` computes `st_blocks` as "present chunks,
not holes" by looping `ci = 0 .. size/128 KiB` and, per chunk, taking
`efs_client_lock_dir` + `idx_mu` for `efs_export_get_chunk`, then the
dcache shard mutex for `efs_dcache_has`. A 16 GiB IOR file is 131072
chunks, so one `stat` is 262144 lock/unlock pairs and hash probes, and
`ll_setattr` calls `efs_fuse_getattr_ino` **twice** (before, for
`old_size`; after, for the reply). `ls -l`, `du`, `cp -p`/`ecopy`
(utimes + chmod after each file), IOR's `stat`, and every `getattr`
the kernel issues with `attr_timeout=0` pay this per file. It was ~0%
when added (project-history: small files) and is now a scaling bug by
the §1 rule: `stat` cost grows with file size.

What to implement:

1. **Make `st_blocks` O(1).** Keep a per-inode "present bytes" (or
   present-chunk count) in the staged inode row, maintained where chunks
   are inserted/removed in the client table (`efs_export_*chunk*` add /
   remove / truncate) and where the dcache admits a dirty chunk that is
   not yet in the table (`dcache_note_dirty_bytes` call sites already
   count bytes per entry). `inode_allocated_bytes` reads that counter.
   Holes stay holes: a chunk that is neither in the table nor dirty in
   the dcache is not counted, same as today.
2. **`ll_setattr` asks for `old_size` from the row, not a full
   `getattr`.** Only `FUSE_SET_ATTR_SIZE` needs the old size, and it
   needs `size`, not `st_blocks`.
3. **Gate:** a `stat` of a 16 GiB file on an idle mount under 1 ms
   (measure with `tests/measure/md_latency.py`'s stat row on a large
   file, add that row); `ll_setattr`/`fill_stat_from_inode` under 0.5%
   in the W15 re-profile; `st_blocks` unchanged for the posix suite
   (`size_and_mtime`, sparse and truncate tests) — posix 1 jobs=1
   200/201, posix 2 63/63.

- **Forbidden:** reporting `st_blocks` from `size` alone (sparse files
  and truncate stubs must still show holes); touching
  `entry/attr_timeout`; caching a stale `st_blocks` across a peer's
  REPORT (the counter is updated when the table adopts chunks, not
  time-based).

#### W21 — Client: the staging evictor recomputes the table size up to 1024 times per second and evicts nothing

**Status (Sep 29 16:09Z, steps 1 and 2 rolled with the servers,
not measured; clients are not mounted).** Step 1: a wake that evicts nothing while over the cap
sets a 1 s idle deadline that `stage_evict_kick` honours; an unpin
clears it. Step 2: `efs_export_staged_bytes` returns the root's
`staged_total`, a running sum of every table's `staged_est`;
`staged_refresh` re-derives one table's estimate wherever a capacity
or arena changes (slab and name arenas via `staged_slab_add`, the
slab directory, chunk array growth and shrink, the ino/name/chunk/icnt
index rebuilds, the child index and vectors, `compact_one_tab`, tab
attach and free). `test_stage_evict` checks the total across a
compaction. Step 3 stays behind a measurement. The earlier in-tree
state, for the record: one `efs_export_staged_bytes` per wake, a
wake walks at most four oldest bands, a fully pinned band advances a
cursor for the next wake instead of looping 1024 times.

**Re-review on the 22:48–22:55 profile (single `dd bs=1M`, 260K
samples).** `efs_export_staged_bytes` **7.4% self** and
`stage_evict_main` **5.4% self** (the inlined LRU scan in
`evict_pass`) — 12.8% of the client, more than before. The per-wake
cap works; the wake *rate* is the problem. `efs_client_stage_evict_kick`
is called from the staging paths on every write and is gated only on
`g_stage_bytes_seen > cap`. Once the table is over the cap with every
entry pinned (the "growing" state every writer reaches), that gate is
permanently true, so each 1 MiB write kicks the evictor, which wakes,
walks up to 4096 `shard_tabs` in `efs_export_staged_bytes`, scans the
LRU array up to four times, evicts nothing, goes back to
`pthread_cond_timedwait`, and is kicked again by the next write. The
1 s timer never gets to run because the kicks arrive faster.

Fixed implementation (replaces steps 1–2 above):

1. **A kick must not wake a scan that cannot evict.** Keep a
   `g_evict_idle_until` (monotonic) set to now + 1 s whenever a wake
   ends with `bytes > cap && evicted_total == 0`; `stage_evict_kick`
   returns without signalling while now < that deadline. Clear the
   deadline (set 0) where an entry becomes evictable — the unpin paths
   (`efs_client_stage_unpin`, last close, dcache clean-and-unpinned
   transitions) — so a real opportunity wakes the evictor at once.
   Gate: with one writer over cap, the evictor wakes at most once per
   second (count wakes in `EFS_STAGE_DBG` output).
2. **`efs_export_staged_bytes` in O(1).** Maintain one cross-tab
   running total in the root export, updated by `staged_slab_add` and
   the child/name-arena accounting where each tab's counters change
   (they already update per-tab totals); the shard-tab loop goes.
   `efs_fuse.c:5047` (the mount log line) reads the same total.
   Gate: `efs_export_staged_bytes` under 0.2% on any profile.
3. **The LRU scan is the remaining cost per real wake** (5.4% here
   because of the wake rate; with step 1 it is 1 scan/s). If it still
   shows above 0.5% after step 1, keep the LRU in tick order (a
   min-heap or a circular array indexed by tick) so "the 256 oldest
   above a cursor" is a slice, not a pass over `g_lru_mask + 1` slots.
   Do not do this before measuring after step 1.
4. **Gate:** as below, plus `stage_evict_main` self + children under
   0.5% on a 1-client `dd bs=1M` profile with the table over cap.

**Source (same profile as W20).** `efs_export_staged_bytes` 6.6%
**self**, caller `stage_evict_main` (`src/client/stage_evict.c`).
`stage_evict_main` itself 2.4%. W15's table said this was "replaced by a
running total Sep 28"; it was not. `export_staged_bytes_one` reads
running totals for one tab, but `efs_export_staged_bytes` still loops
`s = 1 .. shard_tab_cap` over `shard_tabs` (up to 4096) per call, and
`evict_pass` calls it once per pass.

Why the call count: when `bytes > cap` and the SCAN_BATCH (256) oldest
LRU entries are all pinned, `evict_pass` returns `evicted=0,
band_full=1`; `stage_evict_main` sets `min_tick = band_max` and loops,
up to 1024 rounds per wake, each round recomputing staged bytes and
scanning the **whole** LRU array (`for i in 0..g_lru_mask`) to find the
next 256 candidates. Every client logged "staging table over
EFS_CLIENT_META_MB (256 MB) with nothing evictable; growing" during the
IOR, so this is the steady state under a write flood: O(rounds × (tabs
+ LRU)) per second, no eviction, on a core the writers wanted.

What to implement:

1. **One `staged_bytes` per wake.** Compute it once in
   `stage_evict_main` before the round loop and adjust by what each
   round actually freed (`evict_pass` knows the bytes it dropped), or
   keep a true running total across tabs (`staged_slab_add` already
   exists per tab; add the cross-tab sum there). Gate:
   `efs_export_staged_bytes` under 0.5% in the W15 re-profile.
2. **Stop the pinned-band loop from rescanning the LRU.** Walk the LRU
   once per wake in tick order (or keep it ordered) so advancing past a
   pinned band is a cursor move, not a fresh O(N) scan. If the whole
   table is pinned, log the "growing" line once (already done) and
   sleep until the next kick or 1 s — do not spend 1024 rounds proving
   it. Gate: `stage_evict_main` + children under 1% while a client is
   over cap with all entries pinned (the IOR steady state).
3. **Gate:** W9's RSS result still holds (cold stat of 1M files levels
   near 233 MB, `results/measure/20260927-w9-walk`); posix 1 jobs=1
   200/201; posix 2 63/63.

- **Forbidden:** raising `EFS_CLIENT_META_MB` to hide it; evicting
  pinned (dirty or open) inodes; changing the 1 s wake period to
  something longer so the walk is merely rarer.

#### W23 — Client: five liveness syscalls per connection checkout, plus an `fstat` per send

**Source (Sep 29 2026 00:18–00:22 EDT, `client.sh --perf --strace` on
fstor007, `~/orcd/scratch/efs/perf/efs-mount/strace-summary.txt`,
230 s window, `ecopy --verify` + one `dd`).** Syscall counts:
`fstat` 1 171 561, `getsockopt` 1 172 007 (586 035 `SO_ERROR`,
585 972 `TCP_INFO`), `recvfrom` 598 781 (all `MSG_PEEK`), `poll`
1 121 932, `read` 1 195 618 (297K on the FUSE fd, the rest eventfds and
sockets), `write` 583 579 (`recv_poller` eventfd wakes). In the
untraced 22:11 profile the same path was `__fstat64` 1.2% +
`__getsockopt` 1.2% + `efs_client_conn_get` 0.6%; under ptrace it is
`efs_client_conn_get` 5% and `raft_voter_conn` 8%.

Why: `efs_client_conn_get` (`src/client/node_cache.c`) calls
`conn_fd_is_dead` on **every** checkout: `efs_conn_fd_matches` (an
`fstat`), `getsockopt(TCP_INFO)`, `poll(0)`, `recv(MSG_PEEK)`,
`getsockopt(SO_ERROR)`. Then `efs_conn_send_msg_parts`
(`src/common/protocol.c`) does `efs_conn_fd_matches` again before the
RDMA send. Six syscalls per checkout, three checkouts per chunk PUT,
one per metadata RPC — ~586K checkouts in this window, ~15K
syscalls/s. The probe exists for a real reason (the comment: a pooled
fd in CLOSE-WAIT after a peer FIN looked like a live checkout and every
PUT then failed; IPoIB CLOSE-WAIT sometimes reports no POLLIN/HUP), and
`fd_matches` exists because a recycled fd number once sent into a dead
QP. Neither reason needs a probe per checkout.

What to implement:

1. **Pool generation instead of `fstat`.** The pool is the only closer
   of pool fds. Give each slot a `gen` that `efs_client_conn_drop` /
   destroy bumps, store it in `struct efs_conn` at checkout, and make
   `efs_conn_fd_matches` compare integers. Keep the `fstat` form only
   for conns not owned by the pool (server side, tests) behind the
   existing `fd_id_ok` flag. Gate: `fstat` count in an
   `EFS_STRACE_EXPR='trace=fstat'` summary under 1% of the PUT count.
2. **Probe once per idle period.** Record `last_ok_ms` on a conn at
   every successful send/recv. In `efs_client_conn_get`, run
   `conn_fd_is_dead` only when `now - last_ok_ms > 1000` (the CLOSE-WAIT
   case is a conn that sat idle while the peer went away; a conn that
   completed an RPC a few milliseconds ago is not in CLOSE-WAIT). A
   checkout that skipped the probe and then fails in send/recv already
   drops the conn and marks the node (`efs_client_conn_drop`,
   `note_fail`); that path stays. Gate: `getsockopt` + `recvfrom(MSG_PEEK)`
   under 1% of the PUT count on the same summary; the Sep 20 CLOSE-WAIT
   repro (bounce one `efsd` while a client is idle for 30 s, then write)
   still recovers without an EIO.
3. **Gate:** posix 1 jobs=1 200/201, posix 2 63/63, 9-client dd not
   worse than 2551.5; `efs_client_conn_get` + `__fstat64` +
   `__getsockopt` together under 0.5% of an untraced dd profile.

- **Forbidden:** removing the dead-conn detection (the probe's reasons
  are real); probing on a timer thread (it would need the pool lock the
  hot path uses); changing `EFS_NODE_DOWN_FAILS` / `EFS_NODE_DOWN_MS`.

#### W24 — Client: `open(O_TRUNC)` of a large existing file did not return; SETATTR is the most-exhausted RPC

**Source (same window).** `dd.trace.txt`:
`openat(AT_FDCWD, "/tmp/efs-mount/001/dat08", O_WRONLY|O_CREAT|O_TRUNC, 0666) = ?`
followed by `+++ killed by SIGKILL +++` — the create-with-truncate of
the 1.7 GB file the previous `dd` had left never returned before the
user killed it. The fuse log's `exhausted 16 BUSY/STALE` count by type
went from 50 → **136** for SETATTR (type 63) between 22:19 and 00:27,
the largest of any type (RENAME 106, LOOKUP 98, GETATTR 47, CREATE
37). `clock_nanosleep`: 564 sleeps of 0.8 s in 230 s — the tail of
the BUSY backoff — plus `report-loop … busy=1 ms=8435..24182 rc=-13`
lines.

Why an `open` can outlive every budget in the tree:

- Kernel `open(O_TRUNC)` on an existing inode becomes FUSE `setattr(size)`
  → `efs_fuse_truncate_ino` (`src/client/efs_fuse.c`) →
  `efs_dcache_flush_ino` (flush every dirty chunk of the ino; each PUT
  waits on the writer pool), then `efs_client_truncate`
  (`src/client/ops.c`) → `efs_client_report_dirty_ino` (a REPORT loop
  — `report-loop … ms=24182` is one of those), then the SETATTR RPC,
  then `ll_setattr`'s getattr. Three to four RPC chains in sequence.
- Each chain is 16 attempts, but an attempt is bounded by the socket
  `EFS_IO_TIMEOUT_MS` = **30 s**, not by the 400 ms server read
  deadline: when the server holds the RPC (a REPORT was measured
  holding 20.5 s in W22's source; `host_truncate` in `raft_host.c`
  does a `host_read_index`, then proposes a LANE_FENCE per cross-group
  lane, then the truncate entry, each a commit wait), one chain can be
  minutes. The 10.3 s figure in the log line is the sum of the sleeps,
  not the wall.
- With W16 step 1 the final answer is EBUSY; the caller still waits
  through all of it first. The seven D-state `io500` processes in W17
  are the same shape seen from the other end.

What to implement:

1. **Measure one truncate, idle and loaded.** `truncate -s 0` of a
   2 GiB file on an idle cluster and under a 9-client write, with
   `strace -f -tt -T` on the fuse daemon filtered to that worker, and
   the `inode-rpc: slow-recv` / `report-loop` lines. Record per-RPC wall
   (`REPORT`, `SETATTR`, `GETATTR`) and the server-side
   `raft-host: setattr … rc=` pair. This says whether the time is the
   REPORT loop, the SETATTR's commit chain, or the retry sleeps.
   `results/measure/<stamp>-truncate-wall/`.
2. **Bound the whole operation, not each RPC.** `efs_fuse_truncate_ino`
   gets one deadline (the same 8 s W17 step 1 gave `fsync`, measured
   from entry); `efs_dcache_flush_ino`, `efs_client_report_dirty_ino`,
   and the SETATTR RPC each receive the remaining budget and return
   `EFS_ERR_BUSY` when it is gone. `fuse_stat_errno` maps that to EBUSY
   (already). Do not retry a SETATTR whose reply was lost: the server's
   op-id window (I16) answers the replay. Gate: under a 9-client write,
   `truncate -s 0` of a 2 GiB file returns (EBUSY allowed) within 10 s;
   idle it completes in under 1 s; posix 1 `truncate*` and
   `peer_o_trunc_visible` unchanged.
3. **`host_truncate` should not need a read round for the size path.**
   The `host_read_index` at the top is what pays the 400 ms BUSY under
   election churn before the proposal even starts; the proposal itself
   is the linearization point. Check whether the pre-read is only
   fetching `row` for lane splitting; if so, read the row from the
   local KV without a read round and let the truncate entry's apply
   re-validate (it already CASes the tail). This is a server change; if
   the pre-read is load-bearing for a fence ordering, stop and say so.

- **Gate:** step 1's measurement committed; step 2's bounds; posix 1
  jobs=1 200/201 (the truncate tests), posix 2 63/63,
  `posix_persist` shrink/remount cases once they exist.
- **Forbidden:** lowering `EFS_IO_TIMEOUT_MS` to make the chain shorter
  (a slow server reply is not a dead peer); returning success for a
  truncate whose SETATTR did not commit; touching the 16-attempt budget
  (W16 forbids it).

#### W25 — `futimens` fails with EINVAL on any file with a lane in the other Raft group; atime loses its nanoseconds; `ftruncate` says EAGAIN where `stat` says EBUSY

**Source (Sep 29 2026 00:1x EDT, `ecopy --verify /data1/erbmi1/knouse/
→ /tmp/efs-mount/knouse/` and the `software/` copy, operator's
terminal).** Three error strings, thousands of times:

- `<path>: futimens: Invalid argument` — only on large files (R and boost
  tarballs, cuDNN `.a`/`.so`, a MATLAB tar).
- `ecopy: verification metadata mismatch: <path> (atime)` — only on small
  files (conda-meta `.json`, headers).
- `ftruncate: Resource temporarily unavailable` — during the same window
  in which the fuse log's SETATTR `exhausted` count went 50 → 136.

**Confirmed from `ecopy.strace.txt` (00:35 EDT, `strace -f`, 585
threads, 255 931 lines, the `software/` copy, ended by Ctrl-C).**
Non-ENOENT failures: `utimensat` EINVAL **40** (516 succeeded),
`ftruncate` EAGAIN **19** (43 succeeded), `fallocate` EOPNOTSUPP 62,
`copy_file_range` EXDEV 423; `pwrite64` 29 905 and `pread64` 4 635 with
**no short or failed call**, no `close` errors, no `fsync` at all
(ecopy does not call it). Every EINVAL `utimensat` is on an fd that had
just had `fallocate` → `ftruncate` to tens or hundreds of MB (e.g. fd 74:
`ftruncate(74, 104082560)` then `utimensat(74, …) = EINVAL`); every
successful `utimensat` is on a file that went through the small-file
`copy_file_range` fallback. That is the lane split below, seen from
userspace. **Consequence:** after `futimens: Invalid argument` and after
`ftruncate: Resource temporarily unavailable`, ecopy `close`s and
**`unlinkat`s the `.ecopy.tmp.*` file** (56 unlinks in the trace): each
of those files was *not copied*. On this tree that is every file larger
than a few MB. The `ftruncate` that failed for `fd 47`
(923 946 888 bytes) was issued at line 6 123 and returned at line
137 107 of the trace — it blocked for most of the run before failing
(W24's chain; no `-tt` in this trace, so no wall figure). The 303
`verification metadata mismatch` lines are all `(atime)` and all on
files whose `utimensat` succeeded, i.e. item 2. `fallocate`
EOPNOTSUPP and `copy_file_range` EXDEV are the kernel's answers for a
FUSE mount without those ops and a cross-device copy; ecopy falls back
correctly and they cost one syscall each — not defects, noted so nobody
chases them.

Each is a code path, read from the tree, not from a profile:

1. **EINVAL on `futimens` = cross-group lanes.** `host_utimens`
   (`src/server/raft_host.c` ~9100): when the mask has MTIME it walks
   `row.active_lanes` and, for every lane whose shard is in the other
   Raft group, sets `rc = EFS_ERR_INVAL` with the comment
   `/* cross-group lane fence later */`. Lane shards straddle both
   groups (every odd lane of an inode is in the other group, project
   state), so any file that has written into two or more lanes — any
   file past the first few chunks — cannot have its mtime set.
   `efs_rc_to_errno` turns that into EINVAL. Small files (one lane, or
   all lanes in the row's group) work, which is exactly the split in the
   ecopy output. `host_truncate` already handles the same case (it
   proposes a LANE_FENCE per cross-group lane, then the entry);
   `host_utimens` was left as a stub.
2. **atime mismatch = seconds only.** `efs_fuse_utimens_ino`
   (`src/client/efs_fuse.c` 3228) keeps `tv[0].tv_sec` and drops
   `tv[0].tv_nsec`; `efs_client_set_atime` / `efs_client_utimens_both`
   carry `atime` as seconds; `host_utimens` stores
   `u.atime = atime * 1000000000`. mtime keeps its nanoseconds
   (`msec`, `nsec`), atime does not. ecopy compares full timespecs, so
   every file whose `futimens` *succeeded* then fails the atime check.
   Reads do not touch atime anywhere in the tree, so this is the whole
   cause.
3. **`ftruncate` EAGAIN = the SETATTR BUSY budget (W24), mapped
   differently.** `efs_fuse_truncate_ino` returns
   `efs_rc_to_errno(EFS_ERR_BUSY)` = **EAGAIN**; W16 step 1's
   `fuse_stat_errno` maps the same BUSY to **EBUSY** for
   getattr/lookup/create. Two errnos for one condition; ecopy treats
   EAGAIN as a failure and moves on. The cause is W24/W22; the mapping
   is this item.

What to implement:

1. **`host_utimens` fences cross-group lanes the way `host_truncate`
   does** (propose the LANE_FENCE entries on the other group first, then
   the UTIMENS entry; the same helper). If the fence is unnecessary for
   a pure mtime set — the fence exists so an in-flight REPORT on another
   lane cannot re-stamp "now" over the set time — then the UTIMENS entry
   needs the `mtime_gen` bump the apply already checks, per lane group;
   decide by reading `efs_meta_apply` UTIMENS/REPORT ordering, and say
   which. Gate: `futimens` on a 2 GiB file returns 0 and `stat` shows
   the set mtime; posix 1 `utimens*` and posix 2 `peer_utimens_visible`
   unchanged; `cp -p` / `ecopy` of a 1 GiB file reports no `futimens`
   error.
2. **Carry `atime_nsec` end to end**: `efs_fuse_utimens_ino` →
   `efs_client_set_atime` / `_utimens_both` → `efs_msg_inode_setattr`
   (add the field; wire change, bump nothing else) → `host_utimens`
   `u.atime = atime*1e9 + atime_nsec` → `efs_meta_setattr` → row. Gate:
   `touch -a -d '@1500000000.123456789'` then `stat` shows
   `.123456789`; ecopy's atime check passes on the small-file tree.
3. **One mapping for BUSY.** `efs_rc_to_errno` returns `-EBUSY` for
   `EFS_ERR_BUSY` (keep EAGAIN for `EFS_ERR_AGAIN` only), so truncate,
   utimens, chmod, and the stat path agree. Gate: posix 1 jobs=1
   200/201; grep the suite for tests that assert EAGAIN on BUSY (there
   should be none; if one exists, that test is wrong about the
   contract, not this change).

- **Gate:** the three above plus posix 1 9-host not worse than the
  last run.
- **Forbidden:** setting mtime without fencing or bumping `mtime_gen`
  (a later REPORT would restore "now" — the exact bug
  `setattr_rpc_dual_apply`'s comment describes); rounding atime on the
  server to hide step 2; mapping BUSY to EIO.

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

#### W22 — Server: the snapshot cadence makes InstallSnapshot the steady state, and a follower inside an import campaigns

**Status (Sep 29 2026, 04:27Z).** Steps 1 and 2 are in tree and rolled
(04:08Z, `54a500da9dc8-dirty`): a group snapshots at 512 MiB of log
commands past `snap_idx` (`EFS_RAFT_SNAP_BYTES`), `raft_group_snap`
keeps that window (`log_base`, recorded in the SNAP record so a rotated
log replays it), `send_ae` InstallSnapshots only below
`efs_raft_log_floor`; the import diff is applied 1024 keys per pump
cycle (`HOST_SNAP_SLICE`, state `SNAP_IMP_APPLY`) and the last-chunk
retry stays BUSY until the cursor finishes. First 20 minutes on the
cluster, with a 7-minute write in them: zero `raft-snap: start` on any
node, no import. The byte counter's first version walked the log on
every tick (1–1.25 % of every server); the O(1) version is in tree, not
rolled. Step 3's flag `--meta-storage` is in tree with the default
layout; its measurement is **closed by D11** — the per-thread `fsync`
histograms from that trace put the Raft log at 0.4 ms and the 100 ms
mode on the compactor's own segments. What the trace found instead is
W23. Order-table rows 8a, 8b done; 8d closed.

**Source (Sep 29 2026 02:35–02:41Z, `results/io500/20260929-023447-iorperf2/ana`,
9-client `run.sh ior`, perf + `strace -f -tt -T` on every daemon).**

- `host_maybe_snapshot` (`raft_host.c`, `HOST_SNAP_MIN` = 256) starts an
  export as soon as 256 entries have applied since the last one and the
  previous export has finished. The export is the whole KV: 2.67 GB,
  4.6–12 s each, so every node exports back to back for the whole run
  (`raft-snap: start/end` pairs 256 entries apart; fcstor005 twelve of
  them in six minutes). `efs_kv_lsm_view_export` `memcmp` +
  `vx_sift_down` is the top user symbol on all four servers (14 % of
  fcstor004's samples on the GC thread).
- `send_ae` (`raft.c:672`) sends InstallSnapshot to a peer whose
  `next_index ≤ snap_idx`; with the snap point moving every 256
  entries, a follower 257 entries behind gets the 2.67 GB file instead
  of 257 log entries. fcstor006 imported group 2 twice (`import diff
  n=685534 ms=12132`, `n=130398 ms=16467`), fcstor003 imported group 0
  once (`n=380716 ms=12041`).
- The import diff is applied on the follower's pump on the ack (W14
  step 1 moved the scan and sort off the pump; the apply stayed). For
  those 12–16 s the follower answers no heartbeat. Group 0 term moved
  9370 → 9415 and group 2 2792 → 2897 across the run with `drop=0` on
  every `raft-obs tx->` line on all four nodes — the outbox coalesce
  (W14.2 (a)–(b)) removed the drops and the terms still moved. The
  group-2 leader answered 15 REPORTs NOT_PRIMARY and fcstor005 answered
  32 as a transient leader.
- fcstor004's busiest thread (22 % of samples) is the compactor:
  sequential 8050-byte `pread` of one segment, 4 KiB `write`s of the
  new one, and a segment `fsync` at a flat 100 ms, 97 times.
  `persist_max` reached 254 ms. The twenty writer-pool threads create
  fragments on the same XFS (`/data1/01`, ~14 s of `openat`/`write`/
  `close` each). The Raft log, the KV WAL, the segments, and the data
  share one journal.
- Net effect on a client: one REPORT of 86 234 records held the RPC for
  20.5 s (`pack_ms=1431 push_ms=9869 finish_ms=9227 rc=-13`), which is
  the client's `recvfrom` of 19.36 s and the `rounds=1 rc=-13` fsync EIO
  that aborted IOR. The pump itself was not held: `pump_hold_max` 59 ms,
  `apply_max` 69 µs on the leader in this run.

**Steps, each its own change with its own gate (D4–D6 decided):**

1. **Snapshot by log bytes; keep a log window (D4).** In
   `host_maybe_snapshot`: replace `applied ≥ snap + HOST_SNAP_MIN` with
   "bytes appended to this group's log since `snap_idx` ≥ 512 MiB"
   (the store knows its size; count entry `clen` at append if it does
   not). In the store's `save_snap`: keep the log entries after the
   snapshot point instead of truncating at it — retain a window of the
   last `W` entries (start with the same 512 MiB worth; the window is
   internal, no knob) and drop only what falls out of it. In `send_ae`
   (`raft.c:672`): send InstallSnapshot only when `next_index` is below
   the retained window's first index, not when it is `≤ snap_idx`.
   `snap_ensure` / `send_snap` keep re-exporting on demand for a peer
   that is genuinely below the window (fresh node, wiped `mdraft/`).
   Gate: over a 9-client `run.sh ior`, `raft-snap: start` per group at
   most once per 512 MiB of log; `import start` count 0 on every node
   unless one was restarted; `efs_kv_lsm_view_export` under 2 % of
   every server's samples; `test_raft` OK; fcstor005 bounce-and-rejoin
   still catches up (from the log now, not a file).
2. **Slice the import-diff apply (D5).** Today the pump applies the whole
   diff batch on the ack. Make the import state machine resumable: the
   pump applies at most one `HOST_TICK_US` of diff keys per cycle
   (measure keys/µs once; a fixed count per cycle is fine), releases,
   drains the inbox and answers AppendEntries as a follower, and
   continues on the next cycle; the SNAP_REP that acks the last chunk
   goes out when the final slice is in (until then the retry of that
   chunk keeps getting BUSY, as it does now). Gate: force an
   InstallSnapshot under load (bounce one voter after wiping its
   `mdraft/`, with a 9-client write running); `apply_max` under 70 ms
   throughout, no term change on the importing group during the
   import, `tests/measure/i17_leader_freeze.sh` still passes; the
   follower's `commit == applied` catches up to the leader after the
   import.
3. **Metadata root off the fragment root (D6).** Measure first, on one
   server, with the cluster serving a 9-client write: `dd if=/dev/urandom
   of=<root>/fsync-probe bs=4k count=1 conv=fsync` 200 times on
   `/data1/01` (shared) and on a root with no writers (stop one path
   from `--storage` on a scratch efsd, or use a directory on `/` for the
   comparison only), p50/p99 of each into
   `results/measure/<stamp>-fsync-root/`. If the shared root shows the
   100 ms mode and the quiet one does not: add `--meta-storage <root>`
   to `efsd` (default the first `--storage` root — today's layout, no
   migration of existing `mdraft/`), then redeploy 19810 with `mdraft/`
   on a root none of the six `--storage` paths use (ask which device;
   this is a cluster layout change). Gate: compactor and pump `fsync`
   p99 under 10 ms in an `EFS_STRACE_EXPR='trace=fsync,fdatasync'`
   trace of a 9-client write; `persist_max` under 20 ms. If the quiet
   root shows the same 100 ms mode, this step is closed as "not the
   sharing" and the number goes to the user.

- **Gate:** the three above; posix 1 jobs=1 200/201 and 9-host, posix 2
  63/63; a 9-client `run.sh ior` that completes ior-easy-write with zero
  `report-loop … rc=-13` lines; 9-client 8 GiB dd not worse than 2551.5.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`; widening
  the 400 ms apply budget or the 16-attempt RPC budget to hide the wait;
  putting the import scan/sort back on the pump (W14 step 1); changing
  `EFS_RAFT_SNAP_CHUNK`; a global or thread-local fd cache (W14 step 4).

**Two measurement notes from this run.** Both perf profiles carry the
concurrent strace (`ptrace_do_notify` 4–5 % on every host; ~40 % of the
client's `children` view is ptrace stops), so shares are inflated toward
syscall-heavy threads — profile with one recorder at a time when a share
matters. And an un-narrowed server `--strace` is ~2 GB per 6 minutes;
use `EFS_STRACE_EXPR`.

#### W23 — Server: the apply path blocks on L0 back-pressure, and compaction rewrites the table to absorb a few MiB

**Status (Sep 29 2026, 05:11Z).** In tree and rolled
(`a53b253f2455-dirty`, RDMA, `--perf --strace`). Step 1: the apply path
does not wait in `kv_maybe_flush_locked`; a publish batch returns BUSY
from `host_pub_batch_propose` while L0 is within one flush of the cap.
Step 2: a range is compacted when its L0 bytes are at least 1/8 of its
L1 bytes (or when L0 is at the file cap, whichever comes first);
`key[0]==0` is 16 subranges (`key[1]`'s top nibble) at flush and at
compaction output; compaction iterators read 1 MiB ahead. Not gated.
The 1 GiB L0 byte cap in place of the 64-file array is not in this
change — the file cap still forces a compaction, but of one subrange.

**Source (Sep 29 2026 04:07–04:27Z, `results/measure/20260929-040800-idle-trace/ana`,
perf + `strace -f -tt -T` on every daemon, a user `ecopy` 04:15–04:22Z
writing 408K fragments per server; §1b has the full list).**

- `kv_maybe_flush_locked` (`kv_compact.c:711`) waits on `l->cv` while
  `n_l0 + KV_LSM_RANGE_MAX > KV_LSM_MAX_SEGS` (i.e. L0 ≥ 48 files). The
  caller is the apply path on the pump under `h->mu`. On fcstor004 the
  pump's `futex` waits were 4.14, 1.68, **24.23**, 2.70, 3.57 s, each the
  length of one `kv-compact: end ms=`; `pump_hold_max` 4.4 s (history
  6.4 s), `apply_max` 4.4 s. fcstor005 2.14 s. W13 step 2 documented
  this wait as "the only stall left" and logged it (`kv-compact:
  backpressure n_l0=`); the log shows it continuously through the write.
- While the pump is held: `host_wait_applied` hits its 400 ms deadline
  (`raft-host: apply-sleep us=400xxx`, dozens per hold) → the REPORT
  answers BUSY (`report-split nrec=76757 … finish_ms=1881 rc=-13`; 12 of
  116 on 004) → the client's `fsync` returns EIO (W17.1) — the same
  abort the last two IORs died of. No heartbeat leaves either: group 0
  9418→9421 (04:15:43Z), group 2 2902→2904 (04:16:17Z), inside the
  holds. With W22.1–2 rolled and zero snapshot activity, this is the
  multi-second pump hold that is left.
- Why L0 is always near the cap: a flush writes one L0 file per
  `key[0]` range (W13 step 5, ≤ 16 files of ~256 KB from a 4 MiB
  memtable); compaction takes one range's L0 files (`inputs=` 5–11) and
  rewrites that range's whole L1 — 190–330 MB for ranges 1–15, **1.8 GB
  for range 0**. 433 compactions in 20 minutes on 004 (441 / 648 / 656
  on 005 / 003 / 006) summed to **159 GB** of `bytes=` against a 5.3 GB
  table; range 0's 34 rewrites are 61 GB of that and the 24 s hold. The
  compactor thread is 48 % / 55 % of fcstor004 / fcstor005's samples
  (`cm_sift_up`, `memcmp`, `cm_pop`, `kv_seg_probe`), with 5.25M 4 KiB
  `write`s (stdio default buffer) and 3.7M ~8 KB `pread`s (one per
  input block).
- The compactor's segment `fsync` is the 100 ms mode (299 of 695 on 004
  in 90–120 ms, avg 51 ms; 407 of 733 on 005); the pump's Raft-log
  `fsync` averages 0.38–0.40 ms. That closes D6 (D11): not the sharing.

**Steps (D9–D10 pending; write them as decided once the user answers):**

1. **The pump never waits for the compactor (D9).** In
   `kv_maybe_flush_locked`, when the caller is the apply path (pass a
   flag, or make the apply's flush a separate entry point), do not wait:
   flush what can be placed, or let the memtable grow past
   `memtable_max` and return OK; the memtable is bounded by what the log
   can commit ahead of the KV (512 MiB since W22.1). Back-pressure moves
   to admission: `host_propose` (or the REPORT handler before it)
   returns BUSY when the local L0 bytes are over the cap, so the client
   retries on its existing 16-attempt / 10 s budget and no follower's
   pump ever stalls. `kv_compact_locked` for tests and `--compact` keeps
   the synchronous path. Gate: a 9-client `run.sh ior` with `pump_hold_max`
   under 100 ms on every node throughout (`raft-obs`), zero
   `apply-sleep` lines above 100 ms, zero `report-split … rc=-13`, no
   term change on either group; `make test`.
2. **Compaction by bytes, and a bounded range (D10).** (a) A range is
   compacted when its pending L0 bytes reach 1/8 of its L1 bytes
   (`kv-compact: start` logs both); the L0 cap becomes a byte budget
   (1 GiB) and the 64-file cap goes — the read path already probes every
   L0 file, so the file count is not the constraint. (b) A range whose
   L1 exceeds ~256 MB is split on the next key byte at flush and at
   compaction output, so no single rewrite exceeds that. (c) The merge
   reads each input segment through a 1 MiB sequential buffer instead of
   one `pread` per block. Gate: over the same IOR, `kv-compact: end`
   `bytes=` sum under 10× the KV's growth for the run; no compaction over
   1 s; compactor under 15 % of any server's samples; `test_kv_lsm` and
   `test_partitioned_flush` pass; a KV copy from a live node reopens and
   verifies every key.
3. **Already in tree (8g), roll with the next build:** `disk_log_new_bytes`
   O(1); `kv_seg_w_open` `setvbuf` 1 MiB.

- **Read:** `src/kv/kv_compact.c` (`kv_maybe_flush_locked`, `compactor_main`,
  `kv_compact_locked`), `src/kv/kv_lsm.c` (`kv_flush_locked`), `src/kv/kv_seg.c`,
  W13 above, the "L1 compaction is a background thread" learning.
- **Forbidden:** raising `KV_LSM_MEM_DEFAULT` or `KV_LSM_L0_DEFAULT` as the
  fix (W13); raising the election timeout, `HOST_TICK_US`, or the 400 ms
  apply budget; any compaction step under `h->mu`; making `fsync` succeed
  on a merged-back dirty set; moving `mdraft/` to another device to hide
  the compactor's I/O (D11 says it is not the sharing).

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
