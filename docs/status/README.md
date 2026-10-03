# Project status — the task right now and the work queue

[Architecture](../how-it-works/architecture.md) ·
[Backlog](../backlog/README.md) · [In flight](in-flight.md) ·
[Decisions](decisions.md) · [Operations](../operations/operations.md)

This page exists because of the bar in [developing.md](../how-it-works/developing.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---
## 1. The task right now

**Where the project is (Oct 2 2026).** [architecture.md §10](../how-it-works/architecture.md)
steps 0–12 are landed and gated: simulator, KV, Raft, cross-shard txns,
sessions, directory spread, delete-2PC, FUSE A–D. The Raft+KV engine is
the only metadata engine (Step 11, Sep 11). There is no next §10 step.
What remains is the work queue in [§1a](#1a-the-work-queue): measured
gaps, in order. The live cluster state (build, leaders, recorders) is the
cluster fact in `.cursor/rules/efs-project-state.mdc`, not this page.

**How to pick work.** Finish [§1b](in-flight.md)
first. Then take the lowest-numbered open item in §1a; correctness before
performance. Each item names what to change, how to measure it, what
proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4). Decisions already taken are the
rows marked **decided** in §1a "Decisions"; implement them, do not
re-ask. **Oct 2 19:05Z: two open correctness rows go first — W54 (row
0i, a fold's GC deletes the live base → read EIO, data loss) and row 0j
(the client's 50 ms lookup memo returns pre-mutation stats, posix
164/201).** Open asks as of Oct 2 13:45Z: **D28** (who owns acknowledged
bytes across client death), **D29** (a REPORT receipt for "committed,
apply pending"; its live symptom did not appear untraced — P0.2),
**D30** (the remedy for compaction-induced apply lag), **W53**
(keep or revert W41 given the create/close-storm p99 37 → 100–120 ms
under four concurrent 8 GiB writers), **W52**'s fix shape (serial
reservation resolves make an O_APPEND REPORT exceed 30 s). Open
**investigations** — evidence gathering, not approved implementations:
**W48** (four dd streams ended early), **W53**. **W50** and **W51**
closed as investigations (14:57Z / 14:38Z). D27 was
corrected 04:20Z and is to be implemented as its row now reads. The
performance plan's P0, P1, P2.1 and P2.4 are done (§1b); **P2.2 D26**
is next on that track (cursor in the tree, watermark not yet).
Everything else in the Oct 2 plan is decided, asked, closed or
deferred — see that table.

**What this page is not.** Approvals and decisions recorded here are
document claims about what the user said on the date given; they are
not authorization to mutate the cluster now. The long-form item texts
(sources, steps, forbidden lists for W6–W25) are in
[work-items.md](../backlog/work-items.md); superseded handoffs, closed items and
the old ordering tables are in project-history.md — cite its anchors:
[handoff archive Oct 1 18:00–22:00Z](../archive/project-history.md#ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba),
[handoff archive Sep 28 – Oct 1](../archive/project-history.md#ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3),
[closed items moved Oct 2](../archive/project-history.md#ph-start-here-closed-items--moved-oct-2-2026-ea8027),
[closed items moved Oct 1](../archive/project-history.md#ph-start-here-closed-items--full-text-w1w5-w7-w11-w13-moved-oct-7d6df9).

**Before touching the cluster** run `tests/preflight.sh` (the deploy
rule's pre-flight as one command). Stop/start is
`tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]`;
a fresh table after a wipe is `cluster.sh start --fresh`. Runbooks for
the open measurement items are in [runbooks.md](../operations/runbooks.md)
(`tests/measure/*.sh`). **The one live cluster is port 19810** on
fcstor003–006 (`/data1/01–06/efs`, `--quota 36T --direct-io`, RDMA),
clients fcstor003–015 at `/tmp/efs-mount`, the user's own client on
fstor007 via `scripts/client.sh`. 19820 is retired. Do not
`wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs` without being asked.

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
| 1-client honest write today (Oct 1) | ~1.3–1.5 GB/s | **~8–9 %** of the client's ceiling (8 GiB dd+fsync 1.3 GB/s; 16 GiB 1.5 GB/s) |
| 9-client honest write today (Sep 28) | 2551–2810 MiB/s aggregate | **~6 %** of 44 GB/s (8 GiB dd+fsync, 9 own files) |
| 1-client cold read today (Oct 1) | 3.6 GB/s; 4 readers 6.5 GB/s | ~14 % of the client's ceiling (16 GiB, remount before read) |

[architecture.md §1](../how-it-works/architecture.md) says: if a benchmark stops at a
mutex, one leader, one thread, FUSE serialization, one WAL or one
coordinator before a physical resource, *that is by definition an EFS bug*.
By that rule the write path is still a bug, not a tuning task.

Baselines, all honest (flush in the clock, reads after remount, `findmnt`
verified `fuse.efs-fuse`). The full history of these numbers is in
`.cursor/rules/efs-fio-honest.mdc`.

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync`, fcstor007 | **1.3 GB/s** (6.80 s) | §1b 13:20Z block (`dd539`), Oct 1 |
| 1-client 16 GiB write / cold read / 4 readers | **1.5 / 3.6 / 6.5 GB/s** | §1b 18:00Z block (`agent-rd-20261001-*`), Oct 1 |
| 1-client 8 GiB dd+fsync / cold read, fcstor007, Oct 2 (P0.1) | **1518 / 3297 MB/s** | `results/measure/20261002-054132-p0-x16/SUMMARY.txt`, `-053311-p0-gates/` |
| 1-client **16 × 10 GiB dd+fsync** aggregate, fcstor007, untraced (row 11) | **3815 MB/s** (every stream 44.7–45.0 s, no close tail) | `results/measure/20261002-054132-p0-x16/SUMMARY.txt` P0.2 (the traced 04:02Z 663 MB/s on fstor007 is not a baseline) |
| 1-client 8 GiB bs=1M / 4 GiB bs=64k dd+fsync, fcstor008, P1 tree (D23+W41) | **1678 / 737 MB/s** | `results/measure/20261002-060052-p1-d23-w41/SUMMARY.txt` §4 (v2–v5 spread 1363–1678 / 737–828) |
| 4 × 8 GiB dd + 300-file create/close storm, fcstor010, old → P1 tree | dd wall **9.70 → 8.30 s**; storm p50 4.9 → 3.8 ms, **p99 37 → 101 ms** | same SUMMARY §4 (W53) |
| 9-client 8 GiB dd+fsync, aggregate | **2551** MiB/s (best 2810) | `results/measure/20260928-134637-dd-prof-r5b`, `-131651-dd-prof-r2b` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| IO-500 9×4 debug, fresh table | easy-write **5.173 GiB/s**, hard-write 0.519, mdtest-easy-write 6.185 kIOPS, hard-read 1 error (W38) | `results/io500/20261001-074905-rdma/` |
| IO-500 9×4 debug, 0 errors | easy-write 4.563, hard-write 0.640, hard-read 1.034, cold hardscan bad=0 | `results/io500/20260930-183504-rdma/` |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

**Open correctness rows (Oct 1–2 2026; these go before every
performance row). Rows that are done (1j, 0f, 1i, 0d, 1a–1h) are in
project-history.md "START-HERE closed items".**

| # | item | follow-up steps | evidence and limits |
| --- | --- | --- | --- |
| 0g | **W43 · `truncate`/`O_TRUNC` of a file with > 32 chunks in a lane is a silent no-op; the apply answers OK on NOMEM** (Oct 1 22:00Z review, §1b). **Steps b and c IN TREE Oct 2** (the truncate now FAILS with EIO instead of lying; `tests/stress/truncate_big.sh`); step a is **D25, decided and revised Oct 2** (logical truncation + background reclamation), step d follows it | (a) **D25 (decided):** the fence entry sets epoch + size, the reaper reclaims; (b) mechanical regardless of D25: `apply_truncate_cmd` / `apply_lane_fence_cmd` put the apply's rc on the ring instead of `EFS_OK`, so SETATTR fails (EIO/EBUSY, W16 mapping) rather than lying; (c) repro + gate: `dd bs=1M count=10 conv=fsync` of a non-zero source onto an existing 1 GiB file, then `stat` (size 10 MiB), `md5sum` (the new bytes), `efs-mgmt raft-getchunks` on chunk 100 (gone); same with a 300 MiB file (every lane > 32 chunks) and with different content; add it to posix (`truncate_big_*`) and posix_persist; (d) then the apply drains per D25 and `apply truncate rc=` never appears in `efsd.log` during the 16× dd | servers `efsd.log` 20:56:41–43: `apply truncate rc=-2` ×16 inodes on every replica, `apply lane-fence rc=-2` ×661; client `slow-ok type=63 … status=0`; all 34 REPORTs `skip=8192 push_ms=0`. `TRUNC_IT_CAP` = 64 + 1 + 64×32×2 + 3, `efs_meta_apply_lane_fence` `it[1+32+32]`; `trunc_del_cb` → NOMEM at the 33rd chunk of a lane. Forbidden: raising the cap (a 1 TB file is 8192 chunks per lane); deleting chunk rows from the handler thread outside the entry; returning OK for an apply that wrote nothing |
| 0h | **W44 · the group leader's GC frag pass scans the whole prefix every 1.2 s and is 80 % of the leader's `efsd` cycles** (Oct 1 22:00Z review, §1b). **Step a IN TREE Oct 2** (`gc-pass … fsegs= fkeys= ftomb=`); the idle-hour reading on the live table is the next action, then D26 | (a) count what one `host_gc_frag_pass` scan visits (`EFS_GC_DBG`, plus a per-scan key/segment counter on the `gc-pass` line) on the live table while idle; (b) if the 205 ms empty scan is the 50–54 L0 segments, that is D12/D13's file count — bring the number to the user (**D26**); if it is tombstones under the GC prefix, the fix is a per-anchor "GC records pending" watermark the apply maintains so an empty pass costs one get; (c) gate: idle leaders show no `gc-pass` line (> 5 ms) for 10 min, `md_latency.py` medians unchanged, a 10 GiB `rm` still drains at ≥ today's 140 records/s per group | fcstor003 `perf-pid.txt`: tid 1056219 79.7 % of 780 K samples, flat `__memcmp_avx2_movbe` 24.9 % + `merge_scan` 16.7 % + `kv_seg_iter_next` 4.1 % + `kv_msrc_advance` 2.8 %; `gc-pass ms=205 frag=205 reap=0` every 1.2 s from 20:11 to 20:56 with nothing to collect; `kv-compact: end … l0=54 l1=149` once per 7 min. Forbidden: a longer `GC_LOOP_MS` to hide it (the rm drain rate is already 78 min per 160 GiB); scanning from a handler thread |
| 0a | **STALE replay that never converges** (no W number; the earlier `W26` label here collided with the `fallocate` item) | (a) identify ino 116202 on 19810 from a KV copy and compare its chunk row with what the classifier (`write.c:880–968`) would replay; (b) repro: eight `dd bs=1M` into one mount, SIGINT mid-write, client stop, `EFS_DCACHE_TRACE=1`; (c) mechanical: the unmount drain names the inos and rc it abandons, and a `report-stale` round that replays the same single chunk > 16 times logs ino/ci/row gen/verdict once; (d) **DECIDED Oct 2 03:50Z = D27** (decisions table): detect non-progress as repeated STALE against the *same* server generation (a gen advance is contention), stop the loop, keep the dirty bytes pinned, errseq-style EIO on `fsync`/`fdatasync`/`flush` of that inode, `client.sh stop` refused while a stalled rec exists, forced teardown reports ino/ci/off/len/cause per rec. Spill (Oct 1 23:00Z) and drop-after-N (Oct 2 01:45Z) were both rejected. **D28 (ask):** loss on forced teardown as written policy vs server-held write intents. Note: 2768 rounds prove a stalled operation, not a content mismatch — (a) decides which | fstor007 Oct 1 00:05: 2768 rounds of `report-stale: chunks=1 … committed=0 replayed=1` and then `UNMOUNT DATA LOSS … rc=-14 after 60s`. Acknowledged writes were discarded; the log cannot say whose. Forbidden: widening the 60 s drain, dropping the STALE check, or publishing a rec the server rejected |
| 0i | **W54 · a fold's GC deletes the live base: when the folded image's object is also a tombstoned span (same content hash → same generation), `efs_meta_apply_publish` queues that generation for GC and the reaper unlinks the fragments the row still names; the next cache-miss read is EIO (Oct 2 17:04Z, IO-500 9×4 ior-hard-read `MPI_ABORT`). MUST FIX before any performance row; data loss with no repair** | (a) **fix the apply (mechanical, L7 already says a fragment set the live row names is not an orphan):** in the fold branch of `efs_meta_apply_publish` (`meta_apply.c` ~3625, "A full image replaces the spans") skip a span whose `generation == stored.generation` or whose `(nodes, checksums)` alias `stored` (`chunk_aliases`); apply the same filter to the tombstone walk, which today would also GC the previous base when a new image aliases it; (b) `test_meta_apply`: span of object X, then full image with `candidate_gen == X` → the batch holds no GC key for X; (c) repro + gate: two peers write adjacent ranges of one chunk so both merge to the identical image (ior-hard shape; or a posix2 `peer_shared_chunk_fold_gc`), wait past the GC latency (≥ 2 s), remount, cold read; plus a cold `ior-hard-verify` + `hardscan` after every 9×4 run (this run surfaced it only because a same-mount read missed the cache); (d) **ask (not decided):** a GC-side guard — `host_gc_record` point-gets the chunk row before each delete and skips a generation it still references (one get per record on the GC thread = D26's cost; the GC key lacks the inode generation the chunk key needs); (e) stop-all/start-all roll of the four servers, then the gate in (c) | `results/measure/20261002-165956-redeploy-posix-ior/SUMMARY.txt`; fcstor015 `fuse.log` 17:04:40Z `fetch published ino=656804 ci=181944 rc=-9 then pull rc=0 rc=-9` (row unchanged across the pull — not a stale map), `efs-fuse read: decode error (efs_rc=-9) off=23847816512 len=47008`; `raft-getchunks 656804 181944`: `base_gen=15366554570668337119 spans=2` with tombstones `4218386339069290638 seq=1088` and `15366554570668337119 seq=3731` — the base IS the second tombstone; on disk under `…/0065/6804/177/` only `181944.{0,1,2}.3089159234672355549` (one per fcstor003/004/005), the live generation gone. `gc_queue` (`meta_apply.c:3154`) checks nothing; `host_gc_local_del` → `efs_store_del_if_sum` passes on the same object's sum. Server code unchanged since `2b5a25df`; the hit is probabilistic (same-gen collision, GC runs, cache miss) so the 15:21Z clean hard-read does not clear it. The file `/tmp/efs-mount/io500/2026.10.02-13.02.57/ior-hard/file` is unrecoverable (test data; delete it). Forbidden: zero-filling the read (I9); a longer GC latency to hide it; a client-side retry loop |
| 0j | **Client regression in `20745142`/`efc0f499` (not W54, same gate run): `lookup_memo_take` answers a GETATTR within 50 ms of the LOOKUP from the LOOKUP row — a stat right after write/chmod/link/utimens on the SAME client shows the pre-mutation row.** posix jobs=1 **164/201, 36 fail** (`results/posix/20261002-170119`: `basic_dd_rw` size 0, `attr_chmod` mode 420, `hardlink_basic` nlink 1, `attr_utimens_ns` old mtime …); posix2 63/63 (the peer holds no memo). The 15:19Z tree was 200/201 | mechanical: the memo must be invalidated by every local mutation of that ino (write/truncate/setattr/link/unlink/rename/utimens — the same set that already drops `g_lookup_memo` candidates on nothing today), or consumed only when no local op on the ino happened since `lookup_memo_put`; gate: posix jobs=1 back to 200/201 on one client and the Spark `du` number the commit cites not lost | `efs_fuse.c` `lookup_memo_put`/`lookup_memo_take` (`LOOKUP_MEMO_US` 50 ms, 32 slots, keyed by ino only). `attr_timeout` stays 0 (decided). Not a server change |
| 0e | **W38 · ior-hard: a client's full image folds its own published span without the span's bytes (4256 B of zeros, committed)** | the F3 block in §1b (Oct 1 08:05Z): one-client 4-rank IOR hard with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, `hardscan` cold, `raft-getchunks` on each bad chunk; fix on the client (the fold observation must come from a body that holds the span) | `results/io500/20261001-074905-rdma` (NOTE.txt, hardscan.txt, getchunks-118842-118844.txt): ino 10897 ci 118843 base 1774…2861 + len-0 tombstone 1838…0185 seq 1222; 1 of 747720 records; first IOR with W30 in the client. Data loss: goes before 0c |
| 0c | **W36 · rename-vs-unlink of one source both succeed, dangling dentry** (posix2 `peer_rename_vs_unlink_src`, 1 in 6) | the F1 block in §1b (Oct 1 07:45Z): trace the two txns with `APPLY_LOG`, decide between `apply_unlink_cmd`'s silent NOT_FOUND→OK and an EXCL DEL that passes on an absent key, fix that one | evidence `/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b` on 19810 (`-?????????`), `~/efs-runs/p2r422.log`, `results/posix2/20261001-073048`. Correctness: goes before 1a–1h |
| 2a | **W42 · `df` / `efs-mgmt status` report the 3-node capacity model on any node count** (Oct 1 2026, user). **IN TREE Oct 2:** `efs_capacity_logical` (placement.c, binary search on the Σ min(cᵢ, M) ≥ 3M bound), used by `efs_fuse_statfs` (total = quotas, avail = room, used = total − avail) and `efs-mgmt status`; `test_placement` covers 3 equal / 4 equal / 100/100/1000 → 200 / 6 equal / < 3 nodes → 0. Still to do: verify on 19810 (`df` vs `4 × 36T × 2/3`) and the one-QUOTA-member PUT question | mechanical: replace `total_logical = 2 × min_quota` (`efs_fuse_statfs`, `efs_fuse.c:3108–3120`) and `usable_cap = 2 × min_quota` / `usable_free = 2 × min_free` (`efs_mgmt.c:129–185`) with the 3-of-N placement bound: the largest `M` (chunks) with `Σ_i min(c_i, M) ≥ 3M`, `c_i` = node `i`'s quota (or free) in 64 KiB fragments, times 128 KiB; count only up nodes with a quota, as today. Reduces to `2 × min` on three nodes and to `Σ × 2/3` on N equal nodes. Also make `f_blocks` and the used figure come from the same model (statfs today derives used from `Σ phys × 2/3` and total from `2 × min`, so on four nodes used can exceed total and `avail` clamps to 0 while writes still succeed). Unit test with 3 equal, 4 equal, 3 unequal (100/100/1000 → 200, not 800). Then verify on 19810 (`df` vs `efs-mgmt status` vs `4 × 36T × 2/3`) | Both comments say "every chunk places one fragment on each node" — true for three nodes only. Four 500 GiB nodes show 1000 GiB instead of 1333. Not a data-path change; no decision needed. **Verify while there:** what a PUT does when exactly one stripe member answers `EFS_ERR_QUOTA` (`put_fragments_parallel_once`: `quota_errors >= 2` → QUOTA, `acks >= 2` → OK) — if the chunk publishes with two fragments, a full node creates protection debt silently (product-gaps §1.2); if `reroute_down_fragments` moves it, say so in the failure-tolerance table |
| 0b | **W27 · REPORT identity from the staging table** | (a) find the path that leaves a dirty chunk with neither a putid nor a dcache object (`write.c:1103`: putid table eviction, reclaim after `b6c1712d`'s pin release, or an irec-only threshold REPORT); one traced ecopy of a small-file tree on an idle cluster; (b) rerun on the current client first — if `putid miss` is 0 there, record and close; (c) only if (a) names the cause: a chunk with no PUT of ours is not ours to publish (keep dirty, replay from the row), as the `fragment_nodes[0] == 0` branch already does for span-only rows | ≥ 9016 recs (`n=8812…9016` in the last rate-limited second, all `ci=0`) reported with a mapping that "may be the server's row, not this client's PUT" — the Sep 30 (gen, off, len) / conflated-table class that lost ior-hard records. Forbidden: silencing the line, or "committing" such a rec client-side |

#### Plan after the Oct 1 22:00Z review — what runs without a decision, what is asked

Tie-break for every row, in this order: **no lost or misreported bytes
→ fewest surprises for a user → speed.** Effort: quick = hours, medium
= 1–2 days plus gate, long = days or a wipe. Source: the §1b 22:00Z
block and `~/orcd/scratch/efs/perf/efs-mount/server-2056/`.

**Runs without the user (take in this order; one cluster roll covers
the quick ones).** Each row is a queue item; the W number is binding.

| # | item | what | effort | gate |
| --- | --- | --- | --- | --- |
| 1 | **W43 step b — IN TREE Oct 2** | `apply_truncate_cmd` / `apply_lane_fence_cmd` return the apply's rc as the ring verdict (both on `host_apply`'s ring-only list, so a failed apply never halts the log); SETATTR surfaces EIO (W16 mapping) | quick | `tests/stress/truncate_big.sh` exits 3 (`TRUNC_ERR`, file unchanged) until D25, never 1 (a lie) |
| 2 | **W45 · apply-verdict audit — IN TREE Oct 2** (cases listed in §1b) | every `apply_*_cmd` in `raft_host.c` that logs `rc=` and then `return EFS_OK` (truncate, lane-fence, activate-lane, unlink/rmdir NOT_FOUND→OK, any other) either returns the rc as the ring verdict or is an explicitly documented idempotent replay with the op-id window as the witness; the list of cases goes to the user | quick–medium | `grep -n "rc=%d index" src/server/raft_host.c` — each hit has a verdict or a comment naming the replay rule; `test_meta_apply`, `test_sim`, posix 1/2 unchanged |
| 3 | **W43 step c — IN TREE Oct 2** (`tests/stress/truncate_big.sh`, standalone so the posix 200/201 gate stays meaningful; the posix `truncate_big_*` tests come with D25) | repro + gate for big-file truncate: `dd bs=1M count=10 conv=fsync` onto an existing 1 GiB and a 300 MiB file, same and different content, plus `truncate -s 0`; `stat`, `md5sum` through `iflag=direct`, `raft-getchunks` on chunk 100 | quick | exit 3 = truncate refused and file unchanged (today); exit 0 = all four PASS (after D25); exit 1 = a lie |
| 4 | **W38** | ior-hard fold tombstone — the fold observation must come from a body that holds the span (client) | medium | one-client 4-rank IOR hard with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, cold `hardscan bad=0` |
| 5 | **W36 — IN TREE Oct 2, gate pending** | the hole was one path: `efs_meta_apply_unlink_op` / `rmdir_op` probed the inode row for a pending intent (`efs_txn_key_busy`) but not the DENTRY they delete unversioned, so an unlink could drop the source name + row under a RENAME whose dentry EXCL had landed and whose inode-row REDUCE had not; the rename's RESOLVE then PUT the dest dentry over a dead row (`-?????????`). Both log paths now probe `k_loc`/`k_hash` and answer BUSY (client retries → ENOENT after the rename resolves). The silent NOT_FOUND→OK in `apply_unlink_cmd`/`apply_rmdir_cmd` is gone (W45) | medium | posix2 `peer_rename_vs_unlink_src` 20/20 |
| 6 | **W27 (0b)** | rerun the small-file ecopy on the current client; if `putid miss` is 0, record and close | quick | the row's gate |
| 7 | **W26 · `fallocate` — IN TREE Oct 2** | `ll_fallocate`: mode 0 past EOF extends like `ftruncate` (buffered writes published first so the extend never shrinks them), inside EOF 0; `KEEP_SIZE` inside EOF 0, past EOF `EOPNOTSUPP`; every other mode `EOPNOTSUPP`. posix `opt_fallocate` now calls raw `fallocate(2)` on an `O_DIRECT` fd (no glibc fallback) | quick | `ewrite` from `direct_rw` writes; posix 200/201 (W26 text: project-history "START-HERE closed items") |
| 8 | **W44 step a — IN TREE Oct 2; READ 05:00Z** | `efs_kv_lsm_scan_stats` (per thread): `gc-pass … fscans= fsegs= fkeys= femit= ftomb=` for the frag pass and `scans= segs= keys= emit= tomb=` for the whole pass; printed when the pass exceeds 5 ms or `EFS_GC_DBG` is set | quick | **read:** idle group-0 leader `fkeys=1 026 406 ftomb=1 026 149 femit=257` per 1.6 s pass (430–740 ms), group 2 `596 739 / 596 482 / 257` — tombstones under the GC prefix, not the L0 count (`results/measure/20261002-040242-dd16x10g-review/SUMMARY.txt` §4). D26 (i) first |
| 9 | **W46 · writer-pool wakeup — IN TREE Oct 2; GATED 05:33Z (P0.1)** | `writer_slot` has `cv_work`/`cv_done`/`cv_empty`; QUEUED and DONE are `pthread_cond_signal` to the one waiter of that class, only EMPTY broadcasts (several fallback waiters) | quick | **done:** W28 gate PASS (8 GiB dd+fsync 1518 MB/s, cold read 3297, CMP_OK); server `writev` 1.00/fragment (was 2), `futex` 8.1/fragment, client 0.5/PUT (`results/measure/20261002-053311-p0-gates/strace-c-*`) |
| 10 | **W47 · recv_poller eventfd — IN TREE Oct 2 (variant); GATED 05:33Z (P0.1)** | the poller writes the eventfd only when a waiter is armed (`efd_armed`, Dekker-paired with the waiter's post-arm ring re-check; `efs_rdma_reply_fd` arms, `reply_ready*` disarm). A spinning or still-checking waiter costs no `write`. True one-write-per-poll-batch needs a shared per-waiter fd (one thread polling three conns) — a protocol change, not taken | quick | **done:** server eventfd `write` 2.36/fragment (unchanged — "saves nothing server-side" stands); client 1 poll + 1 read per fragment reply |
| 11 | **re-measure the 16× dd — DONE 05:41Z (P0.2):** 16 × 10 GiB dd+fsync on fcstor007, untraced, servers `--perf`: **3815 MB/s aggregate**, every stream 44.7–45.0 s, no close tail; 169 `report-split`, all on fcstor004, `fail=wait` 0, `skip=all` 0, nrec ~8250, pack 3.4–7.6 µs/rec, push 9–36 ms; 144 `apply-sleep` of 20–34 ms, no 400 ms deadline hit | (`results/measure/20261002-054132-p0-x16/SUMMARY.txt`; the traced 04:02Z 663 MB/s is not a baseline) | — | the D29 symptom (BUSY-after-commit resend) did not occur untraced; W48 still needs the harness outputs (row 12) |
| 12 | **W48 · four of the 16 dd streams ended early — INVESTIGATE (Oct 2 05:30Z; not an implementation)** | nodeids 0x36f2e / 0x3df2e / 0x41f2e / 0x3cf2e stopped at 5234 / 4498 / 4151 / 3670 MiB between 04:04:17 and 04:04:48Z with a normal FLUSH and no error reply in the FUSE trace (`results/measure/20261002-040242-dd16x10g-review/client/ana-fuse-early-stop.txt`) | quick | the four dd's exit status, signal, stderr and byte counts from the user's harness dir; if EIO/ENOSPC, the server log at that second and the client `inode-rpc` lines name the cause — W42 is a candidate, not a conclusion; if a timeout/kill, close the item |
| 13 | **W50 · the repeating GC record set — INVESTIGATE (Oct 2 05:30Z)** | `gc-frag group=0 scans=1 records=126 ms=428` identical for minutes on the group-0 leader after the dd (`fcstor003/ana-log.txt`) | quick | one pass with `EFS_GC_DBG=1`: the 126 record identities, each fragment delete's verdict per node, and whether a GC_ACK for them committed; identical counts alone prove nothing — only a record seen in two passes with a non-OK delete verdict and no ack is "stuck" |
| 14 | **W51 · what the follower apply lag is made of — INVESTIGATE (Oct 2 05:30Z)** | `apply-sleep` 20–400 ms and `fail=wait/-13` REPORTs on fcstor004/005 during the 16× write while `kv-compact` ran 3–5 s merges | medium | on the private cluster or 19810 with `--perf --strace` on one follower: per `apply-sleep` episode, was the pump blocked on `l->mu` / the compactor (lock blocking), inside `efs_meta_apply_*` (slow application), or idle waiting for AppendEntries (transport)? One table, one episode class per row; D30 is decided only on that table. Raw material so far: `p0-x16/apply-sleep-compact-gc.txt` (144 episodes, 20–34 ms), fcstor006 `apply_max` 100–160 ms during the 8192-record REPORT applies 06:01–06:08Z, the 12:54Z `l0=143` compaction storm (P1 gate pass 4, dd fsync tails 5.4–6.4 s) |
| 15 | **W52 · a REPORT after thousands of O_APPEND writes answers after > 30 s — NEW Oct 2 13:45Z (both builds; fix shape is a question)** | `host_resolve_caught_up` (raft_host.c, after the `report-split` line, before `set_inode_rc`) runs one serial `host_propose_wait` per OPEN reservation at/below the published size — N appends = N proposals before the reply. Client: `inode-rpc: retry type=67 why=recv rc=-6 recv_ms=30312`, byte-identical resend, server `skip=all`, dd `fsync: I/O error`; the published size lags (completed prefix) then converges. ~5 ms per O_APPEND write. fcstor004 `report-split nrec=1133` 06:03:30Z then `skip=1133` 06:04:01Z (old build); `nrec=625` ×3 at 06:06:45/07:15/07:46Z (P1 build) — `results/measure/20261002-060052-p1-d23-w41/SUMMARY.txt` §5 | medium | bring the shape to the user (one proposal for all caught-up reservations? resolve after the reply?); gate = `tests/measure/append_gate` 20 000 × 4 KiB O_APPEND then fsync returns 0 and the size is exact. Forbidden: widening the client's REPORT timeout |
| 16 | **W53 · W41's create/close-storm tail under concurrent big writers — INVESTIGATE, then user decides keep/revert (Oct 2 13:45Z)** | same host fcstor010, old → P1 build: 4 × 8 GiB dd wall 9.70 → 8.30/8.37 s, storm p50 4.9 → 3.8/5.5 ms, **p99 37.2 → 100.7/121.6 ms, max 52.8 → 277/217 ms**; the P1.2 gate said p99 ≤ 51 ms. fcstor008 passes 2–5: p99 66–128 ms (`…p1-d23-w41/SUMMARY.txt` §4, `wedge-*-fcstor010*.txt`) | quick | per slow close: time in its own REPORT RPC vs in a client lock (`EFS_DCACHE_TRACE=1` `report` lines + a server `report-split` for the small inode); if the RPC is the whole wait, it is the small proposal queued behind the pool's 8192-record batch applies on the server (then the remedy is D30 territory or a smaller D24 batch — ask); if client-side, name the lock. Not a fix without the table |

**Decided Oct 2 2026 01:45Z — the user accepted every recommendation
below EXCEPT row B.** Rows A, C–F are design decisions, now part of the
spec: implement per the recommendation column, do not re-ask. G and L
are queue items now (the two bench plans below; `efs-fuse --bench`
follows `efsd --bench`). H is closed. I–K stay deferred until G's number. M is
the measurement setup. **B became D27 (decided Oct 2 03:50Z) and D28
(open)** — see the decisions table; until D27 is implemented an
unconverging STALE rec is still handled as today (logged, dropped at the
60 s unmount drain) and that remains a known data-loss case (0a).

| # | item | question | effort | decision (Oct 2) / recommendation |
| --- | --- | --- | --- | --- |
| A | **D25 — decided, REVISED Oct 2 04:55Z** | how a truncate of > 32 chunks per lane reaches the KV | medium | **logical truncation (fence entry sets epoch + size, O(lanes)) + background reclamation by the reaper**; the 01:45Z "drain inside the entry" bounded each batch, not the pump hold. Visibility and crash-replay conditions are in the D25 row. Implement with W43 b/c; `apply truncate rc=` must never appear and `apply_max` must stay flat during the 16× dd afterwards |
| B | **0a (d) — DECIDED Oct 2 03:50Z as D27; D28 OPEN** | what does a rec that STALEs on every replay do? | medium (D27); long (D28 ii) | **D27 (user):** detect stalled publication (same server generation repeated, not a count of gen advances), surface a persistent errseq-style writeback error on `fsync`/`fdatasync`/`flush`, retain the dirty bytes, refuse clean teardown while they remain, forced teardown reports ino/ci/off/len/cause per rec. Spill and drop-after-N both rejected. **D28 (ask, after 0a (a) and the D27 gate):** accept loss on forced client teardown as a written unrecoverable-writeback policy, or move recovery ownership to the servers (durable write intent referencing the PUT object, publication resolved server-side). Queue: D27 implements with 0a (a)–(c) |
| C | **W41 — decided yes; IN TREE + gated Oct 2 13:30Z (P1.2), W53 open** | remove `report_mu` (per-inode dirty sets, per-inode publish slots, 4-thread meta-flush pool for D24's landed REPORTs) | medium | **landed:** posix 200/201, posix2 63/63, md_latency unchanged; wedge dd wall 9.70 → 8.30 s but storm p99 37 → 101 ms (row 16, W53). Original text: a `close()` on file A must not wait behind file B's REPORT retry. Gate: the D24 wedge gate plus a concurrent STALE-looping file not delaying other closes beyond their own REPORT. **Evidence Oct 2 (§1b item 2):** 16 writers, publish ≈ 2.7 k rec/s vs PUT ≈ 5 k chunks/s, dirty set 8 k → 570 k, last close 74.6 s; every REPORT packed twice because the host answered BUSY after committing (`fail=wait/-13`) — bring **D29** (the REPORT receipt — row N) with this item; it is an ask, not in the spec |
| D | **D23 — decided; IN TREE + gated Oct 2 13:30Z (P1.1)** | clean dcache bodies have no budget; RSS tracks bytes written | quick | **landed as "hand the body to the rdcache when the REPORT commits the full-image object"** (`dcache_note_committed` → `dcache_body_to_rdcache`, put outside the slot lock) — not "when the PUT lands": a STALE replay needs the body and its ranges until the commit, and a span-only row (table gen 0) cannot enter the rdcache. Gate: 4 × 1 GiB bs=64k dd+fsync RSS 2.55 GB flat (old build 4.64 GB); `cmp` after remount OK (`…p1-d23-w41/SUMMARY.txt` §3). Original: drop a clean body once its PUT lands; reads go to the rdcache |
| E | **D17 — decided** | `st_blocks` = 0 for files this client did not write | medium (row/wire) | **per-lane present-chunk count in the lane stamp**, reduced at getattr like `max_end`, returned in the row image; the client sums it. Gate: `du` of a file written by another client ≈ size/512 (× EC not counted), ecrawl sparse heuristic 0 false positives |
| F | **D26 — decided (shape); W44 step a READ 05:00Z** | GC frag pass / L0 steady state | medium | **pending-GC watermark first** (one get per empty pass) — W44 a says a pass walks ~1 M tombstones under the GC prefix, **but it also emits 257 live records each time, so these are not empty passes and the watermark alone does not help while they remain** (user, 05:30Z): pair it with bounded scan progress (resume where the last pass stopped) and with W50 (row 13), and maintain the watermark atomically with insertion/removal and across recovery; the L0 width (`l0=100–170` during a 16× write, 50–54 idle) is a separate cost that shows in REPORT pack (100–133 µs per record get) — bring that number when D12/D13 come up. Gate: W44's |
| G | **`efsd --bench` — asked Oct 2** | the server storage bench | medium–long | implement the plan below ("Single-node storage bench"); its number decides I–K |
| H | **D15 / D16 — closed** | leader stickiness / peer transport class | — | closed; the motivating 30 s stall was the sender's channel bug (`85f5b31c`) |
| I | **fragment on-disk layout — deferred** | fewer path components per fragment (W34 residual) | long + **wipe** | not before G says the server PUT path is the wall (writers were 10 % busy in the 20:56Z trace) |
| J | **W40 — deferred** | FUSE write copy — own `/dev/fuse` receive loop | long | not before G; the write wall is RTT × in-flight depth, client at 1.35 cores |
| K | **RDMA zero-copy receive — deferred** | per-request posted receives | long | not before G; reads are in-flight bound at 3.6 / 6.5 GB/s |
| L | **`efs-fuse --bench` — asked Oct 2, after G** | the client-side ladder | medium | implement the plan below after `efsd --bench` |
| M | **recorders — decided** | for the re-measurement (row 11) | — | `--perf` only on the servers; the client untraced |
| N | **D29 — ASK (Oct 2 05:30Z)** | a REPORT the host committed is answered BUSY after the apply-wait deadline, and the client re-sends it whole (§1b 05:00Z item 2) | medium | a **receipt** ("committed, apply pending": group, index, term, op identity) the client uses to wait on or query the *same* operation, never to re-pack or re-propose. Not a successful publication; dirty bytes stay owned until the apply verdict (STALE and other failures remain possible). Tests: lost receipt (client re-queries, host answers from the verdict ring or NOT_FOUND → ordinary resend), leader change between receipt and verdict (`(index, term)` identity, I17 rule). Not to be built until decided |
| O | **D30 — ASK, blocked on W51 (Oct 2 05:30Z)** | remedy for compaction-induced apply lag on followers (row 14) | medium–long | open until W51's table says lock blocking vs slow application vs transport; the candidate remedies differ per class (compaction shape D9/D10; apply batching; sender pacing) and none is chosen now |

**Order of work from here (one order, correctness first — Oct 2
04:20Z):** (1) **D25** (logical truncation + background reclamation) completes W43 (`truncate_big.sh` exit 3 → 0 with t1–t9, `apply_max` flat);
(2) **D27** with 0a (a)–(c) and its gates; (3) the correctness gates
still owed — W36 20/20, W38 (row 4), W27 rerun (row 6), the W42
one-QUOTA-member PUT question; (4) only then the measurements — W44 a
idle-hour reading, W46/W47 counts (rows 9/10), the untraced 16× dd
re-measurement (row 11); (5) D23, W41, D17, D26 (after step 4's W44
reading); (6) `efsd --bench`, then `efs-fuse --bench`; (7) the open
asks go to the user — D28 with 0a (a)'s answer and D27's gate result,
D29 with W41, D30 with W51's table. The investigations W48 / W50 / W51
(rows 12–14) are quick evidence tasks that may run at any point the
cluster is up; they are not approvals to implement anything. §1b
"Next" is this list and nothing else.

#### Single-node storage bench `efsd --bench` — ASKED Oct 2 2026 (user). Queue position: after W41 / D23 / D17 / D26 in "Plan after the Oct 1 22:00Z review"; its number decides the fragment layout, W40 and zero-copy receive.

**Why this goes before the client tests (Oct 1 04:50Z).** The cluster write wall is ~2.5–2.8 GB/s logical with nine clients (`results/measure/20260928-134637-dd-prof-r5b`): ×1.5 for 2+1 EC ≈ 4.2 GB/s of 64 KiB fragments ≈ **16 K fragment writes/s per server**, each a file create plus an `O_DIRECT` write on XFS. That is an ordinary filesystem-metadata rate, not the 16.7–21.4 GB/s the `results/nvme/` fio shape measures, and only a bench that calls the fragment store the way the PUT handler does can say whether it is the number. Same for metadata: 4.6 kIOPS mdtest-easy-write across two groups against a 0.4 ms Raft-log `fsync` is an inference that the disk is not the limit; `--bench-meta` makes it a measurement. The bench needs no cluster, no clients, no quorum, no network, no wipe, and none of the things that have confused every cluster measurement (strace on the daemon, elections, a dead mount). **What it will not find:** the ~1 GB/s single-client wall — every client profile shows the server near idle; that one needs the client-side ladder (04:30Z §1b block: one dd, Little's law on one reclaim thread, then a `/dev/shm` private cluster), which comes after this.

**What.** One server, its six NVMe, the code `efsd` already uses, driven until the disk is the limit. Two workloads: data-fragment read and write, and metadata read and write (KV segments, the KV WAL, the Raft log — metadata is not an EC fragment). The same run records the hot path, so the next edit is the symbol that is on CPU while the disk is not full.

**What exists.** `efsd --bench <path>` (`src/server/bench_local.c`) `pwrite`s zeroed `EFS_FRAGMENT_SIZE` buffers into one `.efs-bench` file, rotating 1024 offsets, then deletes the dir. No `fsync`, no read, one path, and it never calls the fragment store, the writer pool, the KV, or the Raft log. That number is not a storage-engine result. **Replace it; do not keep it as a second line next to the real one.** `results/nvme/` is the per-host ceiling (16.7–21.4 GB/s) and is the bar.

**What to add.**

1. **Ceiling, same host, first.** The `results/nvme/` fio numbers for that host are the ceiling; re-run only if the host is not in that dir. Four numbers: sequential write with the flush in the clock, sequential read, and the metadata shape (small synchronous writes, random reads of a segment block). Print them in the bench output. The engine run is judged against them.
2. **Data, through the engine.** `efsd --bench <kind> --storage <p1>[,<p2>,…]` — the same comma list `efsd` takes, so the bench runs on **1, 2, 3, … 6 paths from one invocation set** and the output shows how the engine scales from one drive to many (one drive at the fio ceiling and six at 1.3× is a serialization point in the writer pool or the store, not the disks). Kind `data`: call the fragment store and the writer pool exactly as the PUT handler does — fragment name, the probe/`path_hint` logic as shipped, create, `O_DIRECT` write of a **non-zero** payload, whatever `fsync` policy PUT applies, and the publish/rename step if the store has one — then read those fragments back the GET handler's way (`O_DIRECT`, so it is not the page cache). No cleaner path than production: the gap to fio is only meaningful if it is the engine's own.
3. **Report latency at fixed queue depth, not only GiB/s.** For each path count and for QD 1, 16, 64, 256 per path: fragment ops/s, p50/p99 latency per op, and aggregate GiB/s. A client has 16–32 chunks in flight; what it sees is the latency at that depth, and `in-flight × 64 KiB ÷ latency` on the server side either matches the 2.8 GB/s cluster number or does not. A GiB/s-only number at unlimited depth hides the problem the system actually has.
4. **Metadata, through the engine.** Kind `meta`, on `--meta-storage`. Drive `efs_kv_lsm_*` and `efs_raft_disk_*` with sync on (the simulator's `EFS_KV_LSM_NOSYNC` / `EFS_RAFT_DISK_NOSYNC` is the wrong tool): puts that cross the memtable and compact, Raft appends that `fsync` at QD 1 and batched, then point gets and a log reopen. Report MB/s, ops/s and p50/p99 next to the small-IO fio lines; include the compactor segment `fsync` time (the 100 ms mode of D11). Flush is in the clock; bytes must show up in `du`.
5. **Hot path, same process.** `--perf` on that run (attach or the existing efsd flag; do not start `efsd` under strace). One line in the bench output per run: the top symbols and whether the wall was disk (`iostat` util per device) or CPU. A change that moves neither the GiB/s nor that line is not a result.
6. **Where.** One fcstor, no quorum, no clients, no port 19810, no wipe. Bench dirs under the `/data1/0N/efs`-style paths of a node that is not serving 19810 are off limits while it serves; use an unused directory on the same devices or a node the user names. Screen on that host (`EFS_RUNNER=fcstor0NN.ib`). Build in `/tmp/efs` on the node.

**Done when.** One log has the four fio ceilings, then for 1 and 6 paths (and the steps between) the engine's data write and read ops/s, p50/p99 at QD 1/16/64/256, aggregate GiB/s, and the meta read/write lines, each beside its fio ceiling, with the top symbols and `iostat` util. The 1-path → 6-path curve is part of the result. Anything short of the ceiling names the symbol to change next. Do not tune from the current `--bench` line, and do not add an IO path the server does not already have.

#### Client bench `efs-fuse --bench` — ASKED Oct 2 2026 (user); after `efsd --bench`.

**Why.** The single-client write wall (~1 GB/s whatever the transport, host, stream count or build; §1b 04:30Z) has never been located: no measurement separates the kernel FUSE path, the client pipeline, the per-fragment round trip and the client's own arithmetic. `ll_write` is a shim around `efs_fuse_write(NULL, buf, size, off, fi)`, and open/flush/fsync/release are the same kind of shim, so the whole client write path runs in-process with no kernel and no `/dev/fuse`. Four levels, one variable apart, replace the dd ladder:

| kind | what runs | what it isolates |
| --- | --- | --- |
| `cpu` | no network, no server: synthetic non-zero 128 KiB chunks → dcache store → blake3 + XOR parity + the RDMA bounce copy into a registered buffer, then drop | the client's arithmetic ceiling per thread and its scaling with threads (`memmove` + `blake3` + `xor_into` are ~11 % of today's profile; this says what they cost at full speed) |
| `put` | `efs_client_put_fragments_parallel` straight to the servers at a fixed queue depth; no FUSE, no dcache, no REPORT | the per-fragment round trip the client sees: fragments/s, p50/p99 at QD 1/16/64/256, GiB/s — the client side of Little's law. Against a `/dev/shm` private cluster it is network + handler + poller only; against NVMe it adds the store, which `efsd --bench` measured alone, so the difference is attributable |
| `write` | `efs_fuse_create` → `efs_fuse_write` 1 MiB at a time → `efs_fuse_fsync` → release, in-process, N files in parallel — the `ll_*` handlers minus the kernel | the full client pipeline (dcache, reclaim/put pool, dirty-byte throttle, REPORT) with the flush in the clock; the gap to `put` is the pipeline's own cost |
| dd through the mount | the number we have (fcstor007 1000 MiB/s, 32 GiB + fsync) | the gap to `write` is the kernel FUSE path: request dispatch, the `fuse_buf_copy`s, `max_write`, libfuse worker count — W15's splice/`max_write` questions become measurable |

Reading: `cpu` ≫ `put` ⇒ latency-bound (more in flight or a shorter RTT; not more poller threads); `put` ≈ `write` ≈ dd ⇒ the RTT is everything; `write` ≫ dd ⇒ the kernel FUSE path is the wall; `put` on `/dev/shm` ≈ `put` on NVMe ⇒ the store is not in the client's RTT.

**Rules, or it is another `--bench` that lies.**

1. **Production path only.** `efs_fuse_write` / `efs_fuse_fsync` and `efs_client_put_fragments_parallel` as they are: no bench-only shortcut, no skipped hash, no skipped REPORT in `write`. A hook the handlers do not have means the bench is measuring something else.
2. **Honest clock.** `write` counts bytes after fsync/REPORT returns; `put` counts a fragment on its reply. Non-zero payload (all-zero chunks skip the PUT).
3. **Fixed queue depth, latency reported** — ops/s, p50/p99, GiB/s per QD (1/16/64/256) and per thread count, like the server bench. The dd ceiling is `in-flight × 128 KiB ÷ latency`; print both factors.
4. **Never against 19810's export.** `put` makes fragments no row references; `write` makes real files. Target the private 3-node cluster on fcstor007 (`tests/rdma_first_inode.sh` layout, ports 19950–19952, storage on `/dev/shm` or a scratch dir) or a bench export the user names. No wipe, no roll.
5. **Untraced.** `perf record -g` on the bench process for the top symbols; never strace.
6. **One host, then two.** The same `put` / `write` from two hosts against the same servers separates the per-client wall from the cluster wall (4 clients = 578 MiB/s each, 9 = ~290 each today).

**Where in code.** A `--bench` entry in `efs_fuse.c`'s `main` before `fuse_session_new`; a driver loop per kind; a latency histogram; a non-zero chunk generator. The heavy lifting is existing functions.

**Done when.** One log per host count has the four levels side by side (dd from the honest-fio rule's run dir), each with ops/s, p50/p99 at the four depths, GiB/s, thread count, and the top symbols; the two largest gaps between adjacent levels name the next item. Forbidden: a cleaner code path than the handlers use, a GiB/s number without its queue depth and latency, zero payload, running against port 19810's export, tracing the bench process with strace.

#### Performance plan — PROPOSED Oct 2 2026 05:45Z, STARTED 05:33Z on the user's "implement performance plan"; P0 and P1 DONE 13:30Z, P2 next (status per row below)

**Scope.** Every item in this page and in work-items.md whose motivation
is throughput, latency, CPU or memory. Correctness items (D25, D27,
D28, W36, W38, W27, W48) are not here; they come first per §1a. Each row
below names its status class — **decided** (implement as its row
reads), **in tree** (gate owed), **investigate** (evidence only),
**ask** (no code until the user decides), **deferred** (no code until a
bench number says so) — so a reader cannot mistake a proposal for an
approval. Discipline for every row: one change per roll; before/after
on the same tree and the same files; servers `--perf` only, client
untraced (plan row M); flush in the clock; a results dir cited from
the row; `md_latency.py` medians and the posix/posix2 signature
unchanged or the change is reverted.

**Where the time goes today (from the Oct 2 review, `results/measure/20261002-040242-dd16x10g-review`):**
one client writes 663 MB/s with 16 streams (~4 % of its 16.7 GB/s
ceiling); the servers' writer pools are ~10 % busy; the client's CPU is
0.9 core; the walls are (1) the publish path — 2.7 k rec/s against 5 k
chunks/s of PUTs, every REPORT packed twice because of
BUSY-after-commit — (2) the group leaders burning 38 % of a core on a
GC pass that re-walks 1 M tombstones to find 257 records, (3) follower
apply lag under compaction (`apply-sleep`, 400 ms BUSY). Nothing below
is a tuning knob; each row removes a software serialization point or
measures where the next one is.

**P0 — close the gates owed on what is already in the tree (one roll, no recorders on the client).**

| row | item | status | what | gate / done when |
| --- | --- | --- | --- | --- |
| P0.1 | W46 + W47 | **DONE 05:33Z** | 10 s `strace -e futex,write -p` attach on one server and on the client during an 8 GiB dd, untraced otherwise | **W28 gate PASS** (1518 MB/s write, 3297 cold read, CMP_OK); server writev 1.00/frag (halved), eventfd write 2.36/frag (unchanged), futex 8.1/frag; client 1 poll + 1 read per reply, futex 0.5/PUT — `results/measure/20261002-053311-p0-gates/`, `-054132-p0-x16/SUMMARY.txt` |
| P0.2 | row 11 | **DONE 05:41Z** | the untraced 16× 10 GiB dd, fresh names, servers `--perf` only | **3815 MB/s aggregate**, no close tail, `fail=wait` 0, `skip=all` 0, pack 3.4–7.6 µs/rec — the D29 symptom did not occur untraced; baseline table updated |
| P0.3 | W19 | **CLOSED 05:45Z** | re-profile one group leader during P0.2 | memmove 1.6 %, `try_commit` < 0.2 % on both leaders; the leaders' top user-space cost is the GC frag pass's key scan (`__memcmp` 9 %, 6.6 % under `host_gc_thread`) = D26 (P2.2) |
| P0.4 | W49 | **CLOSED 05:47Z** | `strace -c -f -p efs-fuse` 10 s during an ecopy | fstat + getsockopt + recvfrom(MSG_PEEK) = 0.04 % of 534 894 syscalls |

**P1 — decided client items (no spec change; take in this order).**

| row | item | status | what changes | gate / done when | forbidden |
| --- | --- | --- | --- | --- | --- |
| P1.1 | **D23** clean dcache bodies | **DONE 13:30Z** — landed as a commit-time hand-off (`dcache_note_committed` → `dcache_body_to_rdcache`; decisions row D) after four gate passes found that a PUT-time drop breaks the STALE replay and span-only rows; RSS gate 2.55 GB flat vs 4.64 GB old (`…p1-d23-w41/SUMMARY.txt` §2–3). Original row: | `dcache_flush_keep` / `dcache_install_image`: a body whose PUT landed is dropped (full overwrites already are; append and partial paths still install); reads go to the rdcache, which has a budget. Keep the body-less node that names the object | RSS flat across a 20 GiB dd (`VmRSS` sampled per second); `cmp` after remount unchanged; posix `concurrent_appends` still passes (the Sep 30 reason the body was kept — the sparse/append replay source must stay: keep the body while the entry has ranges, drop only when the PUT image is the whole chunk) | a second clean-body cache without a budget |
| P1.2 | **W41** remove `report_mu` | **DONE 13:30Z, gate 1 of 3 NOT met** — posix/posix2/md_latency unchanged, dd wall 9.70 → 8.30 s, but the storm p99 is 101–122 ms against the row's ≤ 51 ms (W53, queue row 16; user decides keep/revert); the withheld-rec gate waits for D27's hook; the 16× dd had no close tail even before (P0.2). Original row: | `write.c`: the dirty set becomes per-inode (or per `EFS_DIR_LOCKS` stripe) so `dirty_snap_save_locked` detaches one inode's marks, not the process's; `pub_ino_keys` becomes a multi-slot table (one slot per in-flight REPORT); the flush thread becomes a small pool keyed by inode so one inode's STALE loop or 120 s `recvfrom` blocks only that inode; D24's landed counter kicks the inode(s) that landed; `close()`/`fsync` REPORT only their inode | the D24 wedge gate (4 × 8 GiB dd + 300-file create/close storm p99 ≤ 51 ms); a file looping on a withheld rec (`EFS_FAULT_WITHHOLD`, D27's hook) does not delay another file's `close()` beyond its own REPORT; the 16× dd's last `close()` drops from 74.6 s to its own tail (≤ 1 GiB of records) | splitting one REPORT into several RPCs; publish-on-every-write; a second global lock replacing `report_mu` |
| P1.3 | **D29** REPORT receipt | **ask** — bring with P1.2 | a "committed, apply pending" receipt (group, index, term, op identity) the client waits on / queries instead of re-sending; not a publication, frees no dirty bytes; with it the per-record `efs_meta_apply_chunk_holds` get in pack (one get per record, 100–133 µs under `l0=100–170`, added only to make byte-identical resends cheap) can be skipped on a first send — that skip is part of the ask, not a separate change | lost receipt and leader change tests (row N); pack_ms per record on the 16× dd drops from ~130 µs to the proposal cost | implementing any of it before the decision |

**P2 — decided server items.**

| row | item | status | what changes (server) | gate / done when | forbidden |
| --- | --- | --- | --- | --- | --- |
| P2.1 | **W50** | **CLOSED 14:57Z** — not stuck; `…w50-gcdbg/SUMMARY.txt` | one `EFS_GC_DBG=1` pass: identities, delete verdicts, ACK flush rc | table: all del/flush rc=0, 0 consecutive-pass repeats; `ex=(nil)` skip documented separately | naming a cause from counts |
| P2.2 | **D26** GC pass | decided (shape); **cursor in tree, rolled 19:22Z; watermark not yet.** 19:22–19:52Z profile (`results/measure/20261002-195156-servers-perf-idle`): with the cursor the tombstone walk is gone (`ftomb=2`) and the pass cost is the deletes — two 256-record scans per 200 ms budget, ~0.5 ms/record = 3 serial fragment deletes, ≤ ~400 records/s per group; a 100 GiB overwrite is ≥ 34 min of GC. The delete fan-out (parallel/remote-batched deletes) is a shape question for D26 (ii), not in the decided text | (a) per-anchor pending-GC watermark maintained in the apply (insert bumps, ack/removal lowers, re-derived at recovery from a prefix scan once) so a truly empty pass costs one get; (b) **bounded scan progress**: the frag pass resumes from a per-anchor cursor instead of restarting at the prefix head (`efs_kv_scan_from` + skip the inclusive start); (c) tombstone-aware emit already on the `gc-pass` line — prefix compact on `ftomb/fkeys` only if D26 (ii) is taken | idle leaders: no `gc-pass` line > 5 ms for 10 min and leader `efsd` CPU < 5 % idle; `md_latency.py` medians unchanged; a 10 GiB `rm` still drains at ≥ 512 records per pass; the raft tail of an idle cluster is no longer 99.7 % GC_ACK (preflight's "idle" becomes true) | a longer `GC_LOOP_MS`; scanning from a handler thread; a watermark that is not updated in the same apply as the record |
| P2.3 | **W23** stalled-compactor test | open (remaining action) | the test written in work-items.md W23 "correction": compactor stalled by a fault, measure memory and lag bound | numbers in `results/measure/`; feeds W51/D30 | — |
| P2.4 | **W51** | **DONE 14:38Z** — table only; `…w51/SUMMARY.txt` | 144 apply-sleep episodes from the P0.2 file | compact-overlap 47, small-gap 89, lag-gap 0; D30 still ask | a remedy before the table |
| P2.5 | **D30** apply-lag remedy | **ask**, blocked on P2.4 | candidate per class: compaction shape (D9/D10 revisit), apply batching, sender pacing | — | code before the decision |

**P3 — the benches (asked; they decide P4).** `efsd --bench` per its
plan above (ceiling first, 1→6 paths, QD 1/16/64/256, meta kind,
`--perf` in-process, one fcstor not serving 19810), then `efs-fuse
--bench` (`cpu` / `put` / `write` / dd ladder against the private
3-node cluster, never 19810). Done when each log has the four levels
beside the fio ceiling; the two largest adjacent gaps name P4's first
item.

**P4 — deferred until P3's numbers (long; one is a wipe).**

| row | item | taken only if | what |
| --- | --- | --- | --- |
| P4.1 | plan row I — fragment on-disk layout (W34 residual) | `efsd --bench` shows the server PUT path (create + O_DIRECT write per 64 KiB fragment, 16 K/s per server at the cluster wall) is the wall | fewer path components / larger containers per fragment; **wipe**; design row first |
| P4.2 | plan row J — **W40** FUSE write copy | `efs-fuse --bench` shows `write` ≫ dd (the kernel FUSE path is the wall) | own `/dev/fuse` receive loop into pool buffers; W15 step 5 (`FUSE_CAP_SPLICE_READ`, kernel prerequisite done Sep 29) rides with it |
| P4.3 | plan row K — RDMA zero-copy receive | `efs-fuse --bench` shows `put` ≈ `write` ≈ dd with RTT the whole wall and reads in-flight bound | per-request posted receives into chunk buffers; transport change, design row first |
| P4.4 | 9-client scaling | after P1/P2 | `dd_wall.sh` 1/4/9 and `fio_honest_matrix.sh`; if 9 clients still share one ceiling (Sep 28: 2.5–2.8 GB/s = 6 % of 44 GB/s), the shared point is on the servers — P3's `efsd --bench` meta line vs data line says which |

**P5 — re-baseline and close.** After each of P1.2, P2.2 and P4.x: 1-client
8/16 GiB dd+fsync, 16× dd, 4-reader cold read, 9-client dd, IO-500 9×4
debug; update the ceiling table and `efs-fio-honest.mdc`; commit the
results dirs; move this plan's finished rows to project-history.

**Dependencies, in one line.** P0 needs a cluster roll (no `--strace`).
P1.1 and P1.2 are independent of each other and of the servers; P1.2
wants P1.3 decided to realise its full effect but does not need it.
P2.2 waits for P2.1; P2.5 waits for P2.4. P3 needs no cluster. P4 needs
P3. Nothing here needs a wipe except P4.1.

#### Long-form items (W6–W25) — in work-items.md

The full text of the open long-form items — source evidence, numbered
steps, status, and the binding Forbidden list — is in
[work-items.md](../backlog/work-items.md): W6 (IO-500 perf residuals), W8 (9-host
posix), W9 (client staging table), W10 (RDMA), W12 (repo hygiene), W14
(server write path), W15 (client write CPU), W16 (BUSY surfaced as
ENOENT), W17 (N-1 STALE storm), W18 (dcache reclaim), W19 (Raft pump
copies), W20 (`st_blocks`), W21 (staging evictor), W22 (snapshot
cadence), W23 (server L0 back-pressure / compaction; its memory-bound
correction is open), W49 (client conn-liveness syscalls; was a second
"W23", renamed Oct 2), W24 (`open(O_TRUNC)`), W25 (`futimens`).
Each opens with current status / remaining action / governing decision
/ gate; the dated record follows.
Closed items (W1–W5, W7, W11, W13, W26, W28–W35) are in
project-history.md "START-HERE closed items". A plan row that names
one of these W numbers is binding; the work-items text says how.
