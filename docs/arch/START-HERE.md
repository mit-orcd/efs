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

**Where the project is (Oct 2 2026).** [architecture.md §10](../architecture.md)
steps 0–12 are landed and gated: simulator, KV, Raft, cross-shard txns,
sessions, directory spread, delete-2PC, FUSE A–D. The Raft+KV engine is
the only metadata engine (Step 11, Sep 11). There is no next §10 step.
What remains is the work queue in [§1a](#1a-the-work-queue): measured
gaps, in order. The live cluster state (build, leaders, recorders) is the
cluster fact in `.cursor/rules/efs-project-state.mdc`, not this page.

**How to pick work.** Finish [§1b](#1b-in-flight--finish-this-before-taking-a-queue-item)
first. Then take the lowest-numbered open item in §1a; correctness before
performance. Each item names what to change, how to measure it, what
proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4). Decisions already taken are the
rows marked **decided** in §1a "Decisions"; implement them, do not
re-ask. Open asks as of Oct 2 04:20Z: **D28** only (who owns
acknowledged bytes across client death; D27 was corrected 04:20Z and
is to be implemented as its row now reads). Everything else in
the Oct 2 plan is decided, asked, closed or deferred — see that table.

**What this page is not.** Approvals and decisions recorded here are
document claims about what the user said on the date given; they are
not authorization to mutate the cluster now. The long-form item texts
(sources, steps, forbidden lists for W6–W25) are in
[work-items.md](work-items.md); superseded handoffs, closed items and
the old ordering tables are in project-history.md — cite its anchors:
[handoff archive Oct 1 18:00–22:00Z](../project-history.md#ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba),
[handoff archive Sep 28 – Oct 1](../project-history.md#ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3),
[closed items moved Oct 2](../project-history.md#ph-start-here-closed-items--moved-oct-2-2026-ea8027),
[closed items moved Oct 1](../project-history.md#ph-start-here-closed-items--full-text-w1w5-w7-w11-w13-moved-oct-7d6df9).

**Before touching the cluster** run `tests/preflight.sh` (the deploy
rule's pre-flight as one command). Stop/start is
`tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]`;
a fresh table after a wipe is `cluster.sh start --fresh`. Runbooks for
the open measurement items are in [runbooks.md](runbooks.md)
(`tests/measure/*.sh`). **The one live cluster is port 19810** on
fcstor003–006 (`/data1/01–06/efs`, `--quota 36T --direct-io`, RDMA),
clients fcstor003–015 at `/tmp/efs-mount`, the user's own client on
fstor007 via `scripts/client.sh`. 19820 is retired. Do not
`wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs` without being asked.

### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**. Update or delete
this block when done — an "in flight" block older than the last commit
is a bug in this page. One block only; the previous one moves to
[project-history.md](../project-history.md) "START-HERE handoff archive"
when it is replaced.

**Oct 2 2026 05:00Z — review of the `r749` window (servers `--perf
--strace`, fstor007 `client.sh --perf --strace`), written for whoever
picks this up; the cluster is STOPPED (`stop751`, CLUSTER_OK 04:24Z).**
Results + reductions: `results/measure/20261002-040242-dd16x10g-review/`
(`SUMMARY.txt` is the review; raw traces stay in
`~/orcd/scratch/efs/perf/efs-mount/` and
`~/orcd/scratch/efs/perf/cluster-20261002-0423/`; `/tmp/efs-perf/
efsd.{data,strace}` are still on fcstor003–006 — `cluster.sh start`
deletes them). What it found, in order of weight:

1. **The run was 16 × 10 GiB dd on one client, not one dd.** The FUSE
   census (`client/ana-fuse-writers.txt`, nodeid from the
   `fuse_in_header` of every 1 MiB `/dev/fuse` read) shows 16 inodes
   × 10240 WRITEs from 04:02:43Z; 140 433 MiB in 222 s = **663 MB/s
   aggregate with recorders on both ends**, same as the untraced Oct 1
   20:56Z 16× run. The traced stream (dat16, `efs-mount/dd.trace.txt`)
   got 50 MB/s + a 74.6 s `close()` = 37.3 MB/s. Servers: 3700–4000
   fragment creates/s each = exactly that load. No regression, no PUT
   amplification, no `apply truncate`/`lane-fence` lines (fresh files).
   Four streams stopped at 3.6–5.2 GiB with a normal FLUSH and no error
   reply — **Q1 in SUMMARY: find their dd exit lines in the user's
   harness dir** (EIO/ENOSPC would be a W42 bug; a timeout is nothing).
2. **The publish path is slower than the PUT path, and BUSY-after-commit
   doubles its cost.** `report-split` on fcstor004/005: nrec 8 k → 30 k
   → 57 k → 68 k → 124 k → 322 k → 570 k; pack (one point get per record,
   100–133 µs each under `l0=100–170`) up to 42.9 s; push up to 77 s;
   every REPORT but the last ended `fail=wait/-13` (committed, then the
   apply-wait deadline → BUSY) and the client resent it byte-identical
   to the other dual host, which packed it again and found `skip=all`.
   Publish ≈ 2.7 k rec/s vs PUT ≈ 5 k chunks/s, so the dirty set grows
   for the whole write and the last close pays it. D24 bounds a close
   only when REPORTs drain faster than PUTs land. → W41 (decided) is
   necessary, not sufficient; **Q2 (ask): a REPORT verdict for
   "committed, apply pending"** instead of BUSY; and D26's L0 width is
   a REPORT-pack cost too, not only the GC's.
3. **W44 step a reading is in (D26's input):** idle leaders scan
   **1 026 406 keys of which 1 026 149 are tombstones** (group 0) /
   596 739 / 596 482 (group 2) to emit 257 records, every 1.6 s,
   430–740 ms per pass = 38 % of a core; ~90 % of each leader's `efsd`
   cycles under `host_gc_frag_pass`. It is tombstones under the GC
   prefix, not the L0 count → D26 option (i) (pending-GC watermark)
   is the answer to the empty pass. After the dd the tombstones were
   compacted away (12 k / 121 k) and grew back (171 k / 337 k, +1024
   per pass = the run's superseded objects draining at ~512 records
   per pass). A `gc-frag group=0 scans=1 records=126 ms=428` line
   repeating identically = a stuck set whose deletes fail every pass
   (**Q4**, `EFS_GC_DBG=1` for one pass). The raft log tail is 99.7 %
   GC_ACK at 5–6 commits/s per group — that is why `preflight.sh` says
   "not idle".
4. **Compaction pressure on the data-heavy followers** during the write:
   `kv-compact` l0 up to 170, 340 MB L0+L1 merges of 3–5 s,
   `apply-sleep` ×31 (fcstor004) / ×103 (fcstor005) = the `fail=wait`
   REPORTs in 2 and 400 ms BUSY on follower-served reads (D9/D10, ask).
5. Server syscalls per 5 min (each node): futex 14.7 M, eventfd `write`
   7.5 M (W47 saves nothing server-side: a conn thread is always armed),
   openat 1.85 M, writev 1.7 M, fsync 13.8 k (max 0.15 s). Fragment
   writes ~60 µs; the writer pool is not a bottleneck. Client: recv
   poller 6.4 M eventfd writes, 24 PUT threads 149 k poll+read each,
   blake3 19 % of 0.9 core — CPU is not the wall on either end.

The 01:20Z block (what landed, the W45 audit table, the gates) is in
project-history.md "START-HERE handoff archive Oct 2 2026 01:20Z".

**Next, in order (Oct 2 04:20Z; the same list as "Order of work from
here" in §1a — correctness first, no measurement before it):**

1. **D25** (logical truncation + fence history + background reclamation; normative in architecture.md §7.3) — W43 is done when
   `tests/stress/truncate_big.sh` exits 0 with gates t1–t9 and no `apply truncate rc=`
   / `apply lane-fence rc=` line appears during a 16× `O_TRUNC` dd.
2. **D27** — stalled publication, with 0a (a)–(c) and the gates g1–g9
   in the D27 row (`tests/stress/stalled_publish.sh`, `test_wb_err`).
3. Correctness gates owed: W36 posix2 `peer_rename_vs_unlink_src`
   20/20; W38 (plan row 4); W27 rerun (row 6); the W42 one-QUOTA-member
   PUT question (queue row 2a).
4. Measurements: ~~the W44 step a idle-hour reading~~ **done 05:00Z**
   (item 3 above: 1 M tombstones per pass, D26 (i)); W46/W47 counts per
   PUT are in item 5 above from the traced run (2 eventfd writes + 2
   writev per fragment server-side; one poll+read per fragment reply
   client-side) — an untraced `strace -e futex,write -p` attach is still
   owed for the W28 gate; the untraced 16× dd (row 11, servers `--perf`
   only, fresh names) — the traced one gave 663 MB/s aggregate and the
   REPORT shape in item 2; the untraced number is what replaces the
   baselines.
5. D23, W41, D17, D26 — W41 and D26 now carry item 2/3's evidence; bring
   **Q2** (REPORT "committed, apply pending" instead of BUSY) with W41.
6. `efsd --bench`, then `efs-fuse --bench`.
7. **D28** — the one open ask; bring it with 0a (a)'s answer and D27's
   gate result.

Handoff blocks before Oct 2 01:20Z (Sep 28 – Oct 1 22:00Z) are verbatim
in project-history.md ([Oct 1 18:00–22:00Z](../project-history.md#ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba),
[Sep 28 – Oct 1 14:00Z](../project-history.md#ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3)).
A `§1b <time> block` reference in the queue below points there.

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

[architecture.md §1](../architecture.md) says: if a benchmark stops at a
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
| 9 | **W46 · writer-pool wakeup — IN TREE Oct 2** | `writer_slot` has `cv_work`/`cv_done`/`cv_empty`; QUEUED and DONE are `pthread_cond_signal` to the one waiter of that class, only EMPTY broadcasts (several fallback waiters) | quick | W28 gate (8 GiB dd+fsync, remount, read) before/after; `futex` count per PUT in a 10 s `strace -e futex -p` attach |
| 10 | **W47 · recv_poller eventfd — IN TREE Oct 2 (variant)** | the poller writes the eventfd only when a waiter is armed (`efd_armed`, Dekker-paired with the waiter's post-arm ring re-check; `efs_rdma_reply_fd` arms, `reply_ready*` disarm). A spinning or still-checking waiter costs no `write`. True one-write-per-poll-batch needs a shared per-waiter fd (one thread polling three conns) — a protocol change, not taken | quick | W28 gate; `write` count per fragment |
| 11 | **re-measure the 16× dd** — a traced one ran 04:02Z (both ends under perf+strace): 663 MB/s aggregate, `push_ms > 0` now (W43 fixed), but `skip=all` on every resend after a `fail=wait/-13` (§1b item 2) | fresh names (or `rm` first), servers `--perf` only (or `--strace` with `EFS_STRACE_EXPR=trace=openat,writev,close`), client untraced | quick | `report-split` shows `push_ms > 0`, `skip` ≈ 0 and no `fail=wait`; the untraced numbers replace the 18:23Z run's; Q1 (four streams ended early) answered from the harness outputs |

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
| C | **W41 — decided yes** | remove `report_mu` (per-inode dirty-set extraction + multi-slot `pub_ino`) | medium | a `close()` on file A must not wait behind file B's REPORT retry. Gate: the D24 wedge gate plus a concurrent STALE-looping file not delaying other closes beyond their own REPORT. **Evidence Oct 2 (§1b item 2):** 16 writers, publish ≈ 2.7 k rec/s vs PUT ≈ 5 k chunks/s, dirty set 8 k → 570 k, last close 74.6 s; every REPORT packed twice because the host answered BUSY after committing (`fail=wait/-13`) — bring **Q2** (a "committed, apply pending" verdict, or an apply-wait tied to the commit) with this item; it is an ask, not in the spec |
| D | **D23 — decided** | clean dcache bodies have no budget; RSS tracks bytes written | quick | **drop a clean body once its PUT lands**; reads go to the rdcache, which has a budget. Gate: RSS flat across a 20 GiB dd; `cmp` after remount unchanged |
| E | **D17 — decided** | `st_blocks` = 0 for files this client did not write | medium (row/wire) | **per-lane present-chunk count in the lane stamp**, reduced at getattr like `max_end`, returned in the row image; the client sums it. Gate: `du` of a file written by another client ≈ size/512 (× EC not counted), ecrawl sparse heuristic 0 false positives |
| F | **D26 — decided (shape); W44 step a READ 05:00Z** | GC frag pass / L0 steady state | medium | **pending-GC watermark first** (one get per empty pass) — W44 a says the empty pass is ~1 M tombstones under the GC prefix, so (i) is the answer to it; the L0 width (`l0=100–170` during a 16× write, 50–54 idle) is a separate cost that shows in REPORT pack (100–133 µs per record get) — bring that number when D12/D13 come up. Gate: W44's |
| G | **`efsd --bench` — asked Oct 2** | the server storage bench | medium–long | implement the plan below ("Single-node storage bench"); its number decides I–K |
| H | **D15 / D16 — closed** | leader stickiness / peer transport class | — | closed; the motivating 30 s stall was the sender's channel bug (`85f5b31c`) |
| I | **fragment on-disk layout — deferred** | fewer path components per fragment (W34 residual) | long + **wipe** | not before G says the server PUT path is the wall (writers were 10 % busy in the 20:56Z trace) |
| J | **W40 — deferred** | FUSE write copy — own `/dev/fuse` receive loop | long | not before G; the write wall is RTT × in-flight depth, client at 1.35 cores |
| K | **RDMA zero-copy receive — deferred** | per-request posted receives | long | not before G; reads are in-flight bound at 3.6 / 6.5 GB/s |
| L | **`efs-fuse --bench` — asked Oct 2, after G** | the client-side ladder | medium | implement the plan below after `efsd --bench` |
| M | **recorders — decided** | for the re-measurement (row 11) | — | `--perf` only on the servers; the client untraced |

**Order of work from here (one order, correctness first — Oct 2
04:20Z):** (1) **D25** (logical truncation + background reclamation) completes W43 (`truncate_big.sh` exit 3 → 0 with t1–t9, `apply_max` flat);
(2) **D27** with 0a (a)–(c) and its gates; (3) the correctness gates
still owed — W36 20/20, W38 (row 4), W27 rerun (row 6), the W42
one-QUOTA-member PUT question; (4) only then the measurements — W44 a
idle-hour reading, W46/W47 counts (rows 9/10), the untraced 16× dd
re-measurement (row 11); (5) D23, W41, D17, D26 (after step 4's W44
reading); (6) `efsd --bench`, then `efs-fuse --bench`; (7) D28 is the
one open ask and goes to the user with 0a (a)'s answer and D27's gate
result. §1b "Next" is this list and nothing else.

#### Decisions — taken and pending (register D1–D28)

Each row is a choice the spec did not make. A row marked **decided**
was accepted by the user and is now part of the design; implement it
per the item it points at. A row marked **done** is history. Nothing
in an open row is implemented until the user asks. D14 was never
assigned. D15/D16 (closed Oct 2) and the Sep 28/29 ordering tables are
in project-history.md "START-HERE closed items".

**Decided Sep 28 2026 (user accepted the recommendations):**

| item | question | decision (Sep 28) | why, in one line |
| --- | --- | --- | --- |
| **D1 · W6.1 / W17** | what do N-1 shared-file writes do under conflict? | **A span publish commutes; no lock; no CAS on the base generation for spans.** A sub-range writer always publishes a span (even when it holds the base). `expected_gen` applies to full-image publishes only. The trailer keeps the folded spans' `candidate_gen`s (≤ `EFS_CHUNK_DELTA_MAX`) so a replay of a folded span is a no-op. The fold is done by the publisher that fills the last slot, or by a reader — never on another rank's `fsync`. | the code already has commuting non-overlapping spans; what collapses at nine ranks is the base-gen check *before* the span path (one fold STALEs every in-flight span) and the `have_base → full CAS` branch. This is §7.2's commutative reduction applied to the chunk map, and P1 (no serialization point). A distributed chunk lock stays forbidden. Gate in W17 |
| **D2 · W6.2** | may `open()` return before the chunk map is known; how big is the map window? | **Yes. `open()` adopts the inode row only.** Chunk maps are pulled per lane group, in parallel, by `pull_layout_miss` in a metadata window that runs one data window ahead of the prefetcher. Window size is internal and adaptive (start at the prefetch depth, grow while the read stays sequential). No mount option, no environment variable. | `performance.md` already says reads fetch chunk maps in per-lane windows; the code pulls the whole map sequentially at open and the server serializes 36 openers to 14.6 ms per GETCHUNKS. A row without a map is already the evictor's steady state (W9); a miss is pulled and an error is an error, never a zero-fill (I9) |
| **D3 · idle gate** | may the 5/s REAP_DONE gate be raised to remeasure ior-hard 1/9/36? | **No.** Measure after the reaper drains. If it does not drain, that is a bug to file. | the tail is the reaper clearing deleted IOR data, and a measurement started during it measures the reaper |

**Decided Sep 29 2026 (user accepted D4–D8 as written; source
`results/io500/20260929-023447-iorperf2/ana`). Implement per the item
each row points at; do not re-ask. D8 is a measurement whose result
comes back to the user before any budget changes.**

| item | question | decision (Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D4 · W22 step 1** | when does a group take a snapshot and truncate its log? | **By log bytes and follower reach, not every 256 entries.** Take a snapshot only when the log since the last one exceeds a byte budget (recommend 512 MiB — the export itself is 2.67 GB and takes 5–12 s, so anything smaller makes export the steady state), and keep a trailing window of log entries after the snapshot point so `send_ae` serves a follower that is behind by less than that window from the log. Send InstallSnapshot only when `next_index` is below the retained window | `HOST_SNAP_MIN` = 256 applied entries triggers a 2.67 GB export back to back on every node; `raft.c:672` sends the file to any peer more than 256 entries behind; three imports of 12–16 s happened in one 4-minute IOR and each one is a follower that answers nothing while its pump applies the diff |
| **D5 · W22 step 2** | may a follower answer heartbeats while its pump applies an InstallSnapshot diff? | **Yes — slice the diff.** Apply the import diff in bounded slices between pump cycles (one `HOST_TICK_US` worth of keys, then drain the inbox and answer the heartbeat), instead of one 12 s apply. The follower stays a follower; the leader keeps its term | this is W14 step 2's "heartbeat answered from a thread that is not importing", with the evidence: terms moved +45 / +105 across the run with `drop=0` on every outbox, so the outbox was not the trigger; the import-diff apply is the only multi-second pump hold left (`apply_max` 1.17 s on fcstor006, `pump_hold_max` 4–5 s on fcstor005 in the earlier run) |
| **D6 · W22 step 3** | may `mdraft/` (Raft log, KV WAL, segments) live on a root the fragment writers do not use? | **Yes, and measure first.** One `dd` on a quiet node: `fsync` of a 4 KiB file on `/data1/01` while the writer pool creates fragments there, versus on a root with no writers. If the shared root shows the 100 ms mode, give `efsd` a `--meta-storage <root>` (default: first root, today's layout) and point 19810 at a root the six `--storage` paths do not include (or a seventh partition) | the compactor's segment `fsync` was a flat 100 ms 97 times and `persist_max` reached 254 ms while twenty writer threads did `openat`/`write`/`close` on the same XFS; the Raft commit path and the data path share one journal |
| **D7 · W14 step 4** | may the client tell the server a PUT is the first write of `(ino, ci, fi, gen)` so the server skips the six-root probe? | **Yes, with a sentinel, not a guess.** `path_hint = 0xffffffff` means "the client has never PUT this fragment generation"; the server then creates on the writer's least-queue root with no `access()`. Any retry of the same generation (REPORT STALE replay, failed reply) sends the real hint or 0. A fragment name includes its generation, so a first write of a new generation cannot collide with an existing file | the hint from a previous PUT can only help a re-PUT; IOR-easy and a fresh `dd` write every chunk once, so 1.59M `access()` (192 s across six handler threads on fcstor004) survived W14.4 unchanged. The step-4 gate ("under 1 % on a single-client dd") cannot be met by the hint as specified |
| **D8 · W16 steps 2–3** | how does a 86 234-record REPORT reach the log? | **Measure the per-batch commit latency first, then bring the number.** `report-split` shows `push_ms=9869` for 337 publish batches — ~29 ms per batch through the one in-flight batch per peer — and `finish_ms=9227` waiting on the last apply. Add `pub_batch_ms` (p50/max) to `raft-obs`; if the per-batch commit is the ~2–6 ms a Raft round costs here, the 29 ms is queueing behind other clients' batches and the answer is fewer, larger entries per REPORT; if it is the 100 ms `fsync` mode, D6 is the fix. Pipelining past one in-flight batch stays forbidden | the client's fsync waited 19.4 s in one TCP `recvfrom` for a server that was still pushing; nine clients × ~80K records at the observed rate is minutes of leader time per stonewall |

**D9–D11 — DECIDED Sep 29 2026 and rolled 05:31Z (from the 04:07–04:27Z trace, `results/measure/20260929-040800-idle-trace/ana`; the numbers are in the handoff archive). D9 and D10 are the two halves of one problem — the KV cannot absorb writes as fast as the Raft log commits them, and the wait lands on the pump. D9's memory-bound sentence is retracted (see the row).**

| item | question | decided (D9–D11, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D9 · W13 step 2, W23** | may the apply path (the pump) block on L0 back-pressure? | **No. The pump never waits for the compactor.** When L0 is within `KV_LSM_RANGE_MAX` of the cap, the apply keeps writing the memtable and lets it grow past `memtable_max` (**the "bounded by the 512 MiB Raft window" claim written here on Sep 29 is wrong** — that figure is a snapshot trigger, it admits nothing and a follower acks on persist, not apply; the real mechanisms and the stalled-compactor measurement that must name the bound are the W23 correction in [work-items.md](work-items.md)); back-pressure moves to admission on the leader: `host_propose` for REPORT/publish batches returns BUSY while the local L0 is over the cap, so the client retries with its existing budget and the follower's pump is never the one that stalls. Heartbeats and AppendEntries replies do not depend on the KV | the pump waited 4.1 / 1.7 / **24.2** / 2.7 / 3.6 s in `kv_maybe_flush_locked` on fcstor004 in one 7-minute write, each wait one compaction long; every wait produced `apply-sleep` 400 ms timeouts → REPORT `rc=-13` → client fsync EIO, and the two term changes of the run. Same class as W13 (a lock hold the apply does not need) and W22.2 (a follower must answer while it imports) |
| **D10 · W23** | how does L0 reach L1 so that a 7-minute write does not rewrite the table thirty times? | **Compact by bytes, not by file count, and split range 0.** (a) Merge a range's L0 into its L1 only when that range's pending L0 bytes are at least a fraction of its L1 bytes (recommend 1/8: a 250 MB range waits for ~30 MB of L0, a 1.8 GB range for ~220 MB), otherwise let L0 files accumulate — the cap that matters is bytes in L0, not 64 files; reads already probe every L0 (`kv_seg_probe`), so make the L0 cap a byte budget (recommend 1 GiB) and drop the 64-file cap. (b) Partition on more than `key[0]` where a range is large: range 0 is 1.8 GB against 190–330 MB for the other fifteen; split it by the next key byte at flush and compaction time so no range exceeds ~256 MB. (c) The merge reads input blocks one `pread` at a time (3.7M of ~8 KB): read each input segment through a 1 MiB sequential buffer | 433 compactions wrote 159 GB to keep a 5.3 GB table current through one 7-minute write; the compactor is 48–55 % of two servers' samples; range 0 alone is 61 GB of the 159 and the 24 s pump hold. `inputs=` 5–11 files of ≤256 KB each per rewrite of 200–1800 MB is write amplification of several hundred. W13 step 5's partitioned flush bounded the rewrite to one range; it did not bound how often a range is rewritten |
| **D11 · W22 step 3 / D6** | is `mdraft/` sharing an XFS with the fragment writers the 100 ms `fsync` mode? | **No — close D6 as "not the sharing", skip the `dd` measurement, do not move `mdraft/`.** The pump's Raft-log `fsync` averaged 0.38–0.40 ms on all three servers measured (61 228 of 61 668 under 2 ms on 004) while twenty writers created 408K fragments; the 90–120 ms `fsync`s are the compactor's own 200–300 MB segments (299 of 695 on 004, 407 of 733 on 005). `--meta-storage` stays as an option; the compaction fix (D10) is what removes the 100 ms mode and the 130 MB/s the compactor puts on `/data1/01` | per-thread `fsync` histograms in `idle-detail2-fcstor00{3,4,5}.txt`; a segment written at ~2–3 GB/s and then `fsync`'d is ~100 ms by arithmetic, no journal needed |
| **D8 · answered** | is the per-batch commit ~3 ms or ~100 ms? | **~3 ms** (`pub_p50=3109us` in 17 of 23 samples with traffic); the 46 ms samples and `pub_max` 0.8–2.4 s are the pump holds above. No "fewer, larger entries per REPORT" wire change is indicated. Remeasure `pub_p50`/`pub_max` after D9/D10 in one 9-client IOR | the number the D8 row asked for, from the `raft-obs` line W16.2 added |

D9, D10, and D11 were rolled 05:31Z (`a53b253f2455-dirty`). The 12:02Z trace (§1b) is the measurement: D9 held (no pump wait, no `backpressure` line), D11 held (`fsync` max 91 ms), and D10's byte rule held (0.25 GB compacted, not 159 GB). D10's "drop the file cap" did not. That correction is D12.

**D12 was implemented and rolled 12:29Z** (`a53b253f2455-dirty`, no perf, no strace). On the 12:36Z IOR, fcstor004's compactor brought L0 from 7 files down to 3 (L1 stayed 397). The recommendation below is what was built. The 400 ms apply wait still returned BUSY (73 of 99 `report-split` lines on fcstor004).

| item | question | recommended (D12, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D12 · W23 step 4** | may L0's file count grow without a bound while L0 bytes stay under 1 GiB? | **No. Keep the 1/8 byte rule, and put the file cap back as a backstop.** When `n_l0` is over `KV_LSM_L0_DEFAULT` (4), compact the range with the most L0 files even if its bytes are under 1/8 of its L1, and repeat until `n_l0` is under the cap. The 1/8 rule still applies when the count is already under the cap, so a fat range is not rewritten for a few KB. The pump still does not wait. D9's 1 GiB admission stays; it did not fire here | fcstor004 went 26 → 310 L0 files (L1 353) in one IOR while 34 compactions wrote 0.254 GB at 10–14 ms each; `lookup` then probes every file, the publish apply falls behind, and `host_wait_applied` returns BUSY at 400 ms (`pack_ms=0 rc=-13`, 54 of 60 reports). D10 said to drop the file cap because "reads already probe every L0" — that probe is the abort. **The backstop as built rewrote L1 for the whole of the 13:17Z copy (D13)** |

**D13 · W23 step 5 (from the 13:08Z trace, `results/measure/20260929-130800-ddposix/ana`). In tree 14:20Z, not rolled, not gated.** The shape below is what was built. A file-cap compact writes one L0 file and does not open L1. A compact that already meets 1/8, or the 1 GiB byte cap, still rewrites L1.

| item | question | recommended (D13, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D13 · W23 step 5** | when the file cap forces a compact, may that compact read the range's L1? | **No. Merge that range's L0 files with each other into one L0 file, and do not read its L1.** Repeat until `n_l0` is under the cap. The 1/8 rule stays the only merge that rewrites L1. The pump still does not wait. D9's 1 GiB admission stays | D12 ran a full L0+L1 merge whenever the count was over 4, which was the entire 100 GiB copy: fcstor004 wrote 67.5 GB in 425 compacts (max 17 s, L0 peak 379) and `lookup` still binary-searched them (`kv_seg_probe` 8.3 % self). `pack_ms=0` on 172 of 184 BUSY reports; one `push_ms` was 206 s |

**From Sep 30 2026 07:10Z (`results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`). Both DECIDED: D18 Oct 1 (implemented), D17 Oct 2 01:45Z (user accepted the per-lane present-chunk count; implement per the row, queue order in "Plan after the Oct 1 22:00Z review").**

| item | question | decided (D17–D18) | why, in one line |
| --- | --- | --- | --- |
| **D17 · `st_blocks` — DECIDED Oct 2 2026 (user): per-lane present-chunk count in the lane stamp** | where does a client get the allocated-block count for a file it did not write? `inode_allocated_bytes` counts the client-local present-chunk table (W20), empty since D2 for unopened files, so `du` sums 0 for every regular file (52K for 3.8 TB) and ecrawl calls 21150 of 21503 files sparse. `efs_meta_row` has no count; W20 forbids `st_blocks` from size alone | **A per-lane present-chunk count in the lane stamp** (each publish adds the chunks it made present, a truncate subtracts), reduced at getattr like `max_end`/`max_mtime` and returned in the row image; the client sums it. One more u64 per lane read, no new RPC. Alternative: define `st_blocks = 0` as "unknown" for files this client did not write and tell tools so | sparse detection, `du`, quota tooling and ecrawl all read `st_blocks`; only the server sees every lane's publishes |
| **D18 · client staging-table floor — DECIDED Oct 1 2026 (user): evict whole cold tabs; implemented `f073e136`, see §1b** | the staging estimate is dominated by the per-shard-tab floor (one 256-row slab + 16 × 1232 B chunk entries + 512-slot indexes ≈ 90–170 KB per tab, ×4096 tabs ≈ 360–700 MB) and exceeds `EFS_CLIENT_META_MB` (256 MB) with a few thousand rows staged (412 MB at 10780 rows, RSS 330 MB). The evictor then drops 64 hot rows a second forever and never reaches the cap. Which bound do you want? | **Evict whole cold tabs** (`shard_tick` already tracks tab age; a tab with no dirty/pinned/open ino is freed and rebuilt on demand), so eviction frees the floor it cannot otherwise reach; keep the row LRU for the rest. Alternatives: a smaller first slab and lazily sized indexes (the floor shrinks ~10×), or apply the cap above the floor (RSS then ≈ floor + cap) | the 22 ms/s stall from this churn is fixed mechanically (targeted `efs_export_evict_ino`), but the cache still cannot hold a du's rows, and the RSS bound the cap promises is not the one delivered. Sep 30 21:10Z: with the floor above the cap the evictor's LRU scan is **31.6 % of a 62 min client profile and 47.6 % (≈ 0.6 core) during an ecopy**, re-armed by every staging op (`results/measure/20260930-205300-ecopy-perf-review`) |

**D25–D26 — DECIDED Oct 2 2026 01:45Z (user accepted the recommendations as written; from the review of the 20:56Z 16× dd run, §1b 22:00Z block; `~/orcd/scratch/efs/perf/efs-mount/server-2056/`). Implement: D25 with W43 (the drain inside the entry; the `truncate_big.sh` gate goes from exit 3 to exit 0), D26 after W44 step a's idle-hour number (watermark first).**

| item | question | decided (D25–D26, Oct 2) | why, in one line |
| --- | --- | --- | --- |
| **D25 · W43 / W24** | how does a truncate that drops more than 32 chunks per lane reach the KV? Today one Raft entry per cross-group lane (LANE_FENCE) plus one inode-group entry (TRUNCATE) each carry one `efs_kv_batch` capped at 32 chunk DELs per lane, and the apply gives up (NOMEM → logged → OK) on anything bigger | **REVISED Oct 2 05:15Z (user) — now normative in [architecture.md §7.3](../architecture.md) "Truncate is logical; reclamation is deferred"; the op-matrix TRUNCATE row, I22, L7 and the glossary were updated with it, so the spec no longer requires an in-entry range delete or tail rewrite.** Summary (the spec wins on any difference): the TRUNCATE / LANE_FENCE entry bumps the epoch, stamps `base_size`, appends `(epoch, size)` to the inode's **fence history** and pushes both to the active lanes — O(lanes) apply, measured as **total apply duration per entry** (`apply_max` on every replica during `truncate_big.sh`, single-digit ms); no chunk deletes, no tail rewrite (the Oct 2 01:45Z in-entry 64-chunk batches bounded each batch, not the entry). **Validity rule** for reads, publish merges and the sweep: `valid_end(r) = min{ size_j : epoch_j > e_r }` over the history; bytes past it read zero, a sub-chunk merge takes the masked base, a row wholly past it is dead; retained data keeps its old epoch and is never re-stamped — *offset first, then epoch*; "older epochs are never served" is wrong and must not be implemented. Extension stamps size only. **The history is the durable truncation record:** shrink to 100 B, extend to 1 MiB before the sweep, read → zeros past 100, because fence `e1`'s entry is still in the history; a single replaceable fence size would expose the discarded bytes, so entries are appended, never replaced, and only the sweep retires them. **Sweep** = the reaper's existing LANE_SWEEP (64 chunks per KV batch, its own entries): versioned DEL of dead rows (a new-epoch write that landed in between is never deleted), versioned CAS rewrite of a boundary row as the masked row at the fence epoch; a fence leaves the lane stamp when no older row remains and the inode history when every lane has dropped it; `FENCE_HISTORY_MAX` bounds the history and a TRUNCATE against a full history answers BUSY (client retries on its budget) — never an in-entry delete. **Crash replay:** history, lane stamps and rows are replicated; the sweep is re-derived from them and every step is idempotent. **Gates added to `truncate_big.sh` (each on a file > 32 chunks per lane, checked cold after remount and again after the sweep has finished):** (t1) **retained prefix survives** — `md5sum` of bytes `[0, new_size)` before and after equals, after the sweep too; (t2) **partial tail is zero after re-extension** — truncate to a non-chunk-aligned size, `truncate` back up (and separately `pwrite` past EOF): bytes `[old_tail_off, chunk_end)` read as zeros, not the old data; (t3) **no stale beyond the fence** — `raft-getchunks` on a chunk past the fence shows no served row until a new write; (t4) **the sweep cannot delete new-epoch writes** — truncate, immediately write new data into the swept range (several chunks, before and while the reaper runs, `EFS_FAULT_SWEEP_SLOW=1` to widen the window on the private cluster), cold `cmp` of the new data after the sweep finishes; (t5) **restart mid-sweep** — kill the anchor-group leader during the sweep, restart, the sweep completes (`gc-pass` shows the range drained) and t1/t4 still hold; (t6) **apply bound** — `apply_max` on every replica during the fence stays in the single-digit ms class. (t7) **durable truncation history** — shrink to 100 B, extend to 1 MiB *before* the sweep runs (`EFS_FAULT_SWEEP_SLOW=1`), read the whole file: bytes `[100, 1 MiB)` are zero; then the §7.3 sequence (shrink 100 @e1, extend, write `[500,600)` @e1, shrink 700 @e2, extend): `[0,100)` old data, `[100,500)` zero, `[500,600)` new, `[600,700)` zero, beyond 700 zero — checked cold before the sweep, after a leader restart mid-sweep, and after the sweep; (t8) **history bound** — `FENCE_HISTORY_MAX + 1` truncates faster than the sweep: the last is BUSY-retried by the client (eventually 0), none is applied as a delete, t7's reads hold throughout; (t9) **partial rewrites across fences** — sub-chunk writes into the boundary chunk before and after each shrink, cold `cmp` against a model file the test maintains. Space is reclaimed asynchronously; `df` may lag a big truncate by the sweep time (say so in the user-facing doc). No new opcode; no handler-side deletes; W43 step b's verdict rule stays (an apply that cannot fence answers an error, never OK) | 16 × `apply truncate rc=-2`, files kept 10 GiB, the rewrite's 34 REPORTs published nothing; W24's "open(O_TRUNC) did not return" is the same path under load |
| **D26 · W44 / W23** | may the leader's GC frag pass pay a full prefix merge over every L0 segment plus L1 every 1.2 s when there is nothing to collect — i.e. is the 50–54-file L0 steady state (D12/D13 leave `l0=54` for an hour, one L0 compacted per 7 min) the table shape we want, given that every prefix scan and every REPORT `chunk_holds` get pays it? | **Measure first (W44 step a), then one of:** (i) a per-anchor pending-GC watermark the apply maintains (one get per empty pass; the scan runs only when records exist) — smallest, no KV shape change; (ii) a D12 backstop that keeps L0 under ~8 files when the apply is idle (compact L0→L0 opportunistically, never L1) so the merge width is bounded; (iii) both | 80 % of each leader's `efsd` cycles for 84 min, 20–40 % of a core under the KV lock; the frag drain rate itself is only 140 records/s per group (78 min per 160 GiB) |

**D27 — DECIDED Oct 2 2026 03:50Z (user, verbatim intent; replaces the rejected spill and the rejected "drop after 16"). D28 — the architectural choice D27 leaves open; ask.** Source: 0a (d), fstor007 Oct 1 00:05 (`report-stale` ×2768 on one chunk, then `UNMOUNT DATA LOSS rc=-14 after 60s`). Framing: a successful buffered `write()` (returns the byte count) means the filesystem **owns** the pending bytes, not that they are durable; `fsync()` is the durability boundary; `close()` alone is not. `close()` runs the same publish and reports failure through `flush`, but is not a guarantee a program may rely on — the spec's "durable after last `close`" (architecture.md §3) is read that way (the one-paragraph statement is at the top of [work-items.md](work-items.md)). A delayed-writeback error is legitimate; reporting it does not recover the data, and a log line plus a counter are diagnostics, not a substitute for delivering the error ([write(2)](https://www.man7.org/linux/man-pages/man2/write.2.html), [errseq](https://www.kernel.org/doc/html/latest/core-api/errseq.html)).

| item | decided | implementation (binding where stated; the constants are proposals until the gate) |
| --- | --- | --- |
| **D27 · stalled publication** | **"Detect stalled publication, surface a persistent writeback failure, retain unresolved dirty state, and prohibit successful clean teardown while it remains."** Four parts: (1) **bound recovery attempts, not data retention** — on demonstrable non-progress stop the replay loop, keep the dirty bytes, record a writeback error that synchronization calls observe; (2) **contention is not breakage** — a cycle after which the observed server state moved is contention and keeps replaying; repeated cycles against the **same** observed state are a protocol/client defect and are what "stalled" means; a count of STALE replies alone decides nothing; (3) **the drain happens before unmount or daemon exit; ordinary teardown fails visibly while unresolved writes remain** and the daemon and mount stay up; forcing is an explicit operator action, and only a controlled forced teardown can report what it discards; (4) a dirty chunk is **never discarded automatically** — a discard-after-report policy, if ever chosen, is written down as an explicit unrecoverable-writeback policy (D28, not D27). **Corrected Oct 2 04:20Z (user):** the error is sticky, not consumed once; the drain precedes termination; SIGKILL promises nothing; the stall counter counts completed cycles with their identity | **Stall detection.** The unit is one completed *fetch → rebase → publish* cycle of a rec, keyed by (ino, ci, content epoch of the row the rebase used, the rec's operation identity — the client's publish op-id/seq). After each cycle record the observed server state (row generation, content epoch, span count). `STALL_CYCLES` (proposal 8) completed cycles in a row with the observed state unchanged = stalled; any change resets the count (that was contention; a contention stream is logged every 256 cycles as `report-contended`, never trips). On stall: the rec leaves the replay loop, its dcache entry stays dirty + pinned and is never a reclaim victim; one `publish-stalled ino= ci= off= len= gen= epoch= opid= cycles=` line + counter. **Memory admission (policy, corrected Oct 2 04:55Z — the earlier "separate stalled account" did not reduce memory and could be overshot by concurrent stalls):** pending and stalled bytes are budgeted **together under the one dirty cap**; a stalled byte was already under the cap when it stalled, so there is no overshoot and RSS stays bounded by the dirty cap + the clean caches. Stalled bytes are simply never reclaimable. **What is charged (Oct 2 05:05Z):** the cap is charged in **allocated chunk bodies**, not logical bytes — a 1-byte write that materialises a 128 KiB slot charges 128 KiB, a replay/rebase buffer (`stale_repull_replay`, the fetched base image) charges its full size while it exists, and a staged full image charges once. **Reservation is atomic across writers:** a writer reserves `need = (new slots × chunk) + replay buffers` under the cap's lock *before* allocating, and releases on free; two FUSE workers cannot both pass a check against the same headroom. Admission: (i) `write()` on an inode that has a stalled rec returns EIO at once — nothing queues behind a failed publish, so a stalled inode is bounded to the slots it held at detection (plus one replay buffer, released when the loop stops); (ii) `write()` on any other inode reserves and is admitted while `allocated_pending + allocated_stalled + need ≤ cap`, waiting for reclaim as today; (iii) reclaim can only free *pending* allocations, so when `cap − allocated_stalled` is below `need` the wait could never end — the client then returns ENOSPC instead of blocking, logs `stall-budget exhausted` once, and clears the condition on a resolution or an explicit forced discard. **Recovery workspace (Oct 2 05:15Z):** a recovery cycle needs capacity too — the fetched base image and the rebase buffer — and at a cap full of stalled bodies it could never reserve it, so recovery would deadlock. A fixed **recovery reserve** (`RECOVERY_RESERVE`, proposal 8 chunk bodies = 1 MiB) sits outside the dirty cap: ordinary writes can never reserve from it, only recovery cycles can, one at a time per reserve slot, released at cycle end; recovery concurrency is therefore bounded by the reserve, not by free cap. The RSS bound is `cap + RECOVERY_RESERVE + clean caches + the fixed pools`; g3 asserts it against `VmRSS`, not against a byte count. The dirty cap and the reserve are the two constants. **Error surfacing.** Per-inode `wb_err` (errseq model) **plus a sticky "unresolved" state**: while any stalled rec of the inode exists, **every** `fsync`/`fdatasync`/`flush`(close) on **any** description of that inode — including one opened after the stall, after close/reopen, from another process — returns EIO; observing the error never clears it. The unresolved state is **per inode and counts its stalled recs**: a landed publish removes one rec; the state clears only when the inode's stalled count reaches zero — one landed record never clears an inode that still holds other stalled records. A `fsync`/`fdatasync` on a stalled inode runs recovery cycles for its stalled recs, one per rec, from the recovery reserve, **under a whole-call budget** (`FSYNC_RECOVERY_MS`, proposal `EFS_IO_TIMEOUT_MS` = 30 s): cycles that land decrement the count and stay landed; when the budget expires with recs remaining the call returns EIO and the next call continues where it stopped — thousands of individually bounded cycles never turn one `fsync` into a hang. If every rec lands within the budget, the state clears and **that call returns 0** (it observed the resolution it caused). After resolution the errseq rule applies to the recorded failure: a description that already observed EIO during the unresolved window has sampled the sequence and sees 0; a description opened before the resolution that never observed it sees EIO **once**, then 0; a description opened after the resolution sees 0. Exact expected sequence, which g5 asserts: D1 opened before the stall → `fsync` ×3 = EIO, EIO, EIO; hook cleared; `fsync`(D1) = 0 (the resolving call), `fsync`(D1) = 0; D2 opened before the stall and never synced → `fsync` = EIO, then 0; D3 opened after → 0. (`write()` outcomes are the admission rules above: EIO on a stalled inode, ENOSPC when stalled bytes have exhausted the cap, otherwise admitted.) **Teardown.** The drain runs *before* the mount is detached and before the daemon exits: `efs_unmount_drain` with unresolved recs after its 60 s refuses — `scripts/client.sh stop` exits non-zero, prints the ino/ci list, the mount and the daemon stay up. `client.sh stop --force-discard` is the explicit action: it prints `UNMOUNT DATA LOSS ino= ci= off= len= epoch= opid= cause=` per rec and then unmounts. Where the kernel detaches without asking the daemon (`fusermount3 -uz`, a kernel umount of an idle mount): the daemon's session end runs the same drain and refuses to exit the same way where libfuse permits, else logs the per-rec lines before exiting — verify which during implementation and write the answer here. **SIGKILL / node death:** no logging, no per-record report, no promise; what survives is what the servers durably accepted — D28's question. **Gates (deterministic; the 0a (b) dd repro stays as a smoke test, it is not the gate).** The fault must reject **before** anything reaches the server, or the test cannot prove retention — a hook that relabels a successful reply as STALE would leave bytes already published. Primary hook, client side: `EFS_FAULT_WITHHOLD=<ino>:<ci>` (read at mount and re-read from `/tmp/efs/fault` per cycle, logged once) makes the REPORT packer **drop that rec from the REPORT before it is sent** and hand the classifier a synthetic verdict "STALE, observed state unchanged"; nothing of that chunk is published, so during the stall `efs-mgmt raft-getchunks <leader> <ino> <ci>` must show the row's generation unchanged (asserted in g1), and after a forced discard the chunk holds exactly the server's prior bytes (g6). The PUT of the fragment object may happen (it is unreferenced garbage the GC reaps) — the row is what retention is measured against. Secondary hook, server side, for the real wire path: `EFS_FAULT_REJECT_PUBLISH=<ino>:<ci>` on the leader makes `server_raft_host_report` answer STALE for that rec with the current row state and **propose nothing**; run on the private 3-node cluster (`tests/rdma_first_inode.sh` layout), never on 19810. Both hooks are compiled in only with `EFS_FAULTS=1`. `tests/stress/stalled_publish.sh` on one client: (g1) **repeated fsync** — write the chunk, `fsync` ×3 → EIO ×3, `raft-getchunks` row generation unchanged, `close`, `open`, `fsync` → EIO, second process `open`+`fsync` → EIO, `write()` on the stalled file → EIO; (g2) **isolation** — `fsync` on another file in the same mount → 0; (g3) **memory pressure** — with the stalled chunk pinned, write 4× the dirty cap to other files with `fsync` → all 0, the stalled entry still dirty + pinned afterwards (`EFS_DCACHE_TRACE`), RSS under the dirty cap + clean caches; (g3b) **cap exhausted by stalls** — withhold enough chunks (several inodes, written up to the cap first, then all stalled at once) that `cap − stalled` is under one chunk: `stall-budget exhausted` logged once, `write()` on an unrelated file → ENOSPC (not a hang), nothing discarded, RSS did not grow, `client.sh stop` refused; clear one hook → that file's `fsync` 0, writes elsewhere resume; (g4) **teardown** — `client.sh stop` → non-zero with the ino list, `findmnt` still `fuse.efs-fuse`, daemon alive; (g5) **recovery** — the exact D1/D2/D3 sequence above; (g5b) **partial recovery does not clear** — withhold two chunks of one file, clear the hook for one: `fsync` → EIO, `publish-stalled` count for the inode 2 → 1, `write()` still EIO; clear the second → `fsync` 0; (g5) clear the hook via `/tmp/efs/fault` (a remount is not recovery, it is teardown), `fsync`(D1) → 0 and again 0, `fsync`(D2) → EIO then 0, `fsync`(D3) → 0, the unresolved state gone, remount, `cmp` the chunk against the source; (g6) **forced discard** — repeat g1, `client.sh stop --force-discard` → one `UNMOUNT DATA LOSS` line per rec with ino/ci/off/len/epoch/opid/cause, remount, the chunk holds the server's bytes, nothing else of the file is lost; (g8) **recovery at a full cap** — fill the cap with stalled bodies (several inodes, g3b's setup) until `write()` elsewhere returns ENOSPC, then clear every hook: the next `fsync` on each file recovers using the reserve only (log shows `recovery-reserve` cycles), all return 0 within the per-call budget or after a bounded number of calls, `VmRSS` never exceeds the bound, and writes elsewhere resume; (g9) **whole-call bound** — withhold 2000 chunks of one file, clear the hook, `fsync` returns within `FSYNC_RECOVERY_MS` + one cycle (0, or EIO with progress logged), repeated calls converge to 0; (g7) **contention is not a stall** — two clients, one chunk, 1000 alternating writes with `fsync` → 0 and no `publish-stalled` line. Unit: `test_wb_err` for the sequence semantics (sticky while unresolved; once-per-description after resolution; a description opened after resolution sees 0). Forbidden: dropping a dirty chunk on a count; widening the drain; publishing a rec the server rejected; clearing the error on observation; a teardown path that discards without the explicit flag |
| **D28 · who owns the bytes across client death — ASK (restated Oct 2 04:55Z)** | What D27 leaves: a client that dies (SIGKILL, node loss) with *pending* or *failed* bytes loses them, and the original case (0a) is a process that **closed without `fsync`, whose `flush` failed or stalled, and whose client then died** — the application has already gone. Two things the user established: (1) an intent persisted only at `fsync` protects nothing in that case, because no `fsync` happened; a server-side intent can protect bytes only if it is durable **before the acknowledgment being protected** — for `write()`-returned bytes that acknowledgment is the `write()` itself, i.e. the rejected "every write publishes" cost class, or at the latest the `flush`; (2) under the visibility contract (§3) a durable intent **never** justifies a successful `fsync` — publication must complete; an intent can only make a later recovery possible. So the choice is: **(i) accept the loss on client death and forced teardown** as the written unrecoverable-writeback policy, with D27's controlled-teardown report as the whole contract and ENOSPC/EIO as the live signals; or **(ii) a server-side intent as best-effort salvage, scoped precisely** — one design, not the only one: at detection time (the client is alive, the servers reachable, only the publication rejected) the client persists an intent naming the inode, ranges, PUT objects and opid; a group-side recovery pass after client death can then complete or *name* the loss. Another: periodic server journaling of pending ranges, which can protect some pending writes too. **What (ii) protects, exactly: stalled writes whose intent was successfully persisted** — there is a death window between detection and persistence, and pending bytes are covered only if a journaling variant catches them; neither variant makes every acknowledged write survive death — only durable acceptance *before* the acknowledgment does that, which is the rejected cost class for `write()`. `fsync` still returns EIO until publication lands under either | not to be built either way until decided. (ii) is a protocol and metadata change (intent key kind, a recovery pass like `host_txn_recover_pass`, a client identity that survives restart, and the question of why an intent write succeeds where the publish did not); bring its cost after D27's gate exists and after 0a (a) says whether the Oct 1 stall was a defect or contention |

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

#### Long-form items (W6–W25) — in work-items.md

The full text of the open long-form items — source evidence, numbered
steps, status, and the binding Forbidden list — is in
[work-items.md](work-items.md): W6 (IO-500 perf residuals), W8 (9-host
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
| Production Raft host | `src/server/raft_host.c`, [architecture.md §10](../architecture.md) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied; SNAP blob is the existing WAL item payload | `tests/test_kv_lsm`, `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
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
2. **Unit tests pass:** `make test` (`test_sim`, `test_meta_apply`,
   `test_raft`, `test_kv_lsm`, `test_stage_evict`, … — no accepted-failure
   list).
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
