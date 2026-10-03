# In flight — the current handoff block

**This is the current handoff block** (the "1b. In flight" block the work
queue refers to). Older blocks are archived in
[project-history.md](../archive/project-history.md) under "START-HERE
handoff archive".

[Work queue + status](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md)

---

### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**. Update or delete
this block when done — an "in flight" block older than the last commit
is a bug in this page. One block only; the previous one moves to
[project-history.md](../archive/project-history.md) "START-HERE handoff archive"
when it is replaced.

**Oct 2 2026 19:05Z — TWO CORRECTNESS ROWS OPENED BY THE 17:00Z GATE;
do these before anything on the performance track.** Servers ran
19:22–19:52Z on `efc0f499cbae-dirty` under `perf record -g` and were
**stopped at 19:52Z** (`cluster.sh stop`; storage kept, no wipe, next
start needs no `--join`). Clients fcstor003–015 are down. **The whole
cluster is DOWN.** Nothing here needs a wipe.
0. **Perf read of that window (20:05Z, no code change):**
   `results/measure/20261002-195156-servers-perf-idle/SUMMARY.txt`.
   Servers at 8–10 % of one core; load was one 10 GiB `dd` from the
   user's fstor007 client (O_TRUNC over an existing file, traced,
   337 MB/s — not a baseline). Hot paths: (a) PUT = kernel XFS
   create + DIO extent alloc per fragment file, 20–24 % on every
   node, efsd user code ~0.3 %; (b) followers compact 20×/s during
   the dd (`l0=270`, 1215 compactions in one minute, L0 stays
   ~260 idle) and the leader pump lags 35 ms mean / 118 ms max
   (`apply-sleep`) under the REPORT applies — D9/D10 shape;
   (c) **the leaders' GC frag pass is bounded by serial fragment
   deletes, not by the scan any more**: `GC_FRAG_BUDGET_US` ends
   every pass after two 256-record scans (~130 ms each = 3 serial
   deletes per record, 2 remote at ~170 µs) → ≤ ~400 records/s per
   group, 591 identical `gc-frag records=512` passes in 13 min after
   the dd. Whether that queue was draining or a stuck set needs a
   GC-prefix key count on a KV copy (safe now, servers down) — feeds
   D26/P2.2, not a decision; (d) the user's client profile has
   7.7 % memmove under `efs_rdma_send_frame` on the PUT send path
   (W39 zero-copy apparently not engaged on fstor007 — check
   `efs_rdma_zc_region_add` there before assuming it is everywhere).
1. **W54 (row 0i, server, data loss):** a fold GC'd the generation it
   kept as the base; ior-hard-read `MPI_ABORT` on a decode EIO. Fix in
   `efs_meta_apply_publish`'s fold branch, unit test, cold repro, then a
   stop-all/start-all roll. Evidence in the row and
   `results/measure/20261002-165956-redeploy-posix-ior/SUMMARY.txt`.
2. **Row 0j (client, `20745142`'s 50 ms lookup memo):** posix jobs=1
   164/201 — a same-client stat after a mutation returns the pre-mutation
   row. Invalidate the memo on local mutation of the ino.
Then re-run the gate (posix jobs=1 200/201, posix2 63/63, IO-500 9×4
debug every phase + cold `hardscan`) before touching P2.2.
Previous block (15:00Z: W50/W51 done, D26 cursor in tree, 15:19Z gate
clean on `111a07527093`) is below as history; W50:
`results/measure/20261002-134900-w50-gcdbg/SUMMARY.txt`, W51:
`results/measure/20261002-143815-w51/SUMMARY.txt`, gate
`results/measure/20261002-151920-posix-ior/SUMMARY.txt`.

1. **P0 (gates on what was already in the tree):** W28 gate PASS (8 GiB
   dd+fsync 1518 MB/s, cold read 3297 MB/s, CMP_OK, fcstor007); W46/W47
   syscall counts recorded (server: writev 1.00/frag — halved — eventfd
   write 2.36/frag unchanged, futex 8.1/frag; client: 1 poll + 1 read per
   reply, futex 0.5/PUT); **row 11 = untraced 16 × 10 GiB dd on one
   client: 3815 MB/s aggregate, every stream 44.7–45.0 s, no close
   tail, 0 `fail=wait`, 0 `skip=all`** — the BUSY-after-commit resend
   (D29's motivation) did not occur untraced; D29 stays an ask with no
   live symptom in this run. **W19 CLOSED** (leader profile: memmove
   1.6 %, `try_commit` < 0.2 %; the leader's top cost is the GC frag
   pass's key scan = D26). **W49 CLOSED** (fstat + getsockopt +
   recvfrom(MSG_PEEK) = 0.04 % of syscalls under an ecopy).
2. **P1.1 D23 and P1.2 W41 are in the tree (client only; `write.c`,
   `client_internal.h`, `node_cache.c`), gated on fcstor008/009:**
   posix 200/201, posix2 63/63, CMP_OK after remount, md_latency 3.2 /
   1.7 / 3.4 / 0.2 / 0.6 / 3.1 ms. Five gate passes; the first four each
   found a bug in the D23 placement (P1 SUMMARY §2): (a) the rdcache
   hand-off under the slot mutex deadlocked against `efs_client_set_chunk`
   (lock order is `idx_mu` → slot, never the reverse); (b) a span-only
   row (table gen 0) cannot enter the rdcache, and a body-less have_base
   node then publishes zeros as a "full image" → FOLD_LIST STALE loop;
   (c) `dcache_store`/`dcache_store_owned` used `dcache_find`, which
   skips body-less nodes, so a second node shadowed the first; (d) a
   STALE replay needs the body AND the ranges until the REPORT commits.
   Final form: the body goes to the rdcache in `dcache_note_committed`
   (REPORT OK, `object_gen == gen`, entry clean, base_gen ≠ UNCOND), put
   outside the slot lock; store paths look up with `dcache_find_meta`.
   **D23 RSS gate:** 4 × 1 GiB bs=64k dd+fsync → P1 build VmRSS 1.45 →
   2.52 → 2.55 → 2.55 GB; old build 1.45 → 2.52 → 4.64 GB (tracked the
   bytes). **W41 gate, same host fcstor010, old vs P1 build:** 4 × 8 GiB
   dd wall 9.70 → 8.30/8.37 s and storm p50 4.9 → 3.8/5.5 ms, **but the
   300-file create/close storm's p99 went 37 → 101/122 ms (max 53 →
   277/217 ms); the plan's gate was p99 ≤ 51 ms and is NOT met.** That
   is **W53 (investigate, user decides keep/revert):** the mechanism is
   not established; consistent with the evidence is that each small
   close now sends its own REPORT proposal and queues behind the pool's
   8192-record batch applies (`apply_max` 60–85 ms during them) where
   before its records rode in whichever client-wide REPORT was in
   flight. Not excluded: the commit-time rdcache put taking `idx_mu`
   (should not trigger — the storm's files are spans, the dd's are
   UNCOND), or the four pool threads' conns. The code stays in the tree
   because the item was decided and its other gates pass; the tail
   number is in the SUMMARY beside the old one.
3. **W52 (new, both builds, not P1):** a REPORT after thousands of
   O_APPEND writes is answered after > 30 s because
   `host_resolve_caught_up` (raft_host.c, after the `report-split` log
   line) runs one serial `host_propose_wait` per OPEN reservation at or
   below the published size — N appends = N proposals before the reply.
   The client's 30 s recv timeout fires (`inode-rpc: retry type=67
   why=recv rc=-6 recv_ms=30312`), it resends byte-identical, the server
   packs again (`skip=all`), dd prints `fsync: I/O error`; the published
   size lags (completed prefix) and converges later. ~5 ms per O_APPEND
   write. Fix shape is a question (batch the resolves into one
   proposal? resolve asynchronously after the reply?) — not decided.
4. **P2.1 W50 CLOSED 14:57Z** as not a stuck-delete set: with a RAM
   export, 30 s of `EFS_GC_DBG` on both leaders had every `gc del` rc=0,
   every `gc ack flush` rc=0, 0 apply-gc-ack failures, and 0
   `(ino,ci,frag)` repeats across consecutive passes (33792 / 36864
   unique identities = the del count). The repeating `records=256/512`
   is the scan batch walking a large queue. Separate finding: without a
   PUT/GET on that process, `ex=(nil)` and the pass emits the same head
   256 forever (no deletes). Do not name that as the morning 11/s
   GC_ACK cause — those `gc-frag` lines required an export.
5. **P2.4 W51 table written** from the existing 144 `apply-sleep` lines
   (`compact-overlap` 47, `small-gap-no-compact` 89, `lag-gap` 0). D30
   stays an ask; this file has no AE-wait class.
6. **Next:** finish P2.2 D26 — cursor (`scan_from` past the last
   emitted key) is in `raft_host.c` unrolled; the apply-batch watermark
   (insert +1 / retire −1, re-derive on GET miss) is still to land.
   Then P2.3 W23 stalled-compactor test on the private 3-node cluster.
   Then P3 benches. Asks unchanged: D28, D29, D30, W53, W52.
7. **Hint — the next performance package (20:15Z, from the 19:22–19:52Z
   profile; after 0i/0j):** run **P3 `efsd --bench` first** (no
   cluster, one day), because today's data already says what it will
   point at: the server PUT path is 98 % kernel XFS — one inode
   create + one DIO extent allocation per 128 KiB fragment file, efsd
   user code 0.3 % — so the 9-client shared ceiling (2.5–2.8 GB/s,
   6 % of NVMe) is the per-fragment-file store, **P4.1 (fragment
   on-disk layout: fewer path components, larger containers per
   fragment; wipe; design row first)**. That is the package with the
   most throughput behind it. The single-client number (1.5 GB/s) is
   client CPU in the put pool (blake3 21 %, copies 14 %, parity 11 %,
   FUSE kernel copy 6 %) → **P4.2 W40** is the client package, and
   the cheap check before it is whether W39's zero-copy send is
   engaged at all on fstor007 (7.7 % memmove under
   `efs_rdma_send_frame`). The GC delete fan-out (≤ 400 records/s per
   group) is background cost, not a throughput wall — fold it into
   D26 (ii), do not take it before P4.1.

The 05:00Z block (the r749 review: 16 streams not one, the publish
path, W44 a reading, compaction pressure, syscall counts) is in
project-history.md "START-HERE handoff archive Oct 2 2026 05:00Z";
the 01:20Z block (what landed, the W45 audit table, the gates) in
"START-HERE handoff archive Oct 2 2026 01:20Z".

**Next, in order (Oct 2 04:20Z; the same list as "Order of work from
here" in §1a — correctness first, no measurement before it). The
user started the performance track ahead of this list on Oct 2 05:45Z
("implement performance plan"); items 4 and 5 below are done or
rewritten as the P0/P1 rows of that plan, the rest stands:**

1. **D25** (logical truncation + fence history + background reclamation; normative in architecture.md §7.3) — W43 is done when
   `tests/stress/truncate_big.sh` exits 0 with gates t1–t9 and no `apply truncate rc=`
   / `apply lane-fence rc=` line appears during a 16× `O_TRUNC` dd.
2. **D27** — stalled publication, with 0a (a)–(c) and the gates g1–g9
   in the D27 row (`tests/stress/stalled_publish.sh`, `test_wb_err`).
3. Correctness gates owed: W36 posix2 `peer_rename_vs_unlink_src`
   20/20; W38 (plan row 4); W27 rerun (row 6); the W42 one-QUOTA-member
   PUT question (queue row 2a).
4. ~~Measurements~~ **done as P0 (05:33Z–05:47Z,
   `results/measure/20261002-054132-p0-x16`):** W44 a read, W46/W47
   untraced counts, W28 gate PASS, row 11 = 3815 MB/s untraced, W19 and
   W49 closed.
5. ~~D23, W41~~ **in tree, gated 13:30Z (P1; W53 tail question open)**;
   D17, D26 remain (D26 = P2.2 after W50). D29 stays an ask — its
   motivating symptom did not appear untraced (P0.2).
6. `efsd --bench`, then `efs-fuse --bench`.
7. **D28** — the one open ask; bring it with 0a (a)'s answer and D27's
   gate result.

Handoff blocks before Oct 2 01:20Z (Sep 28 – Oct 1 22:00Z) are verbatim
in project-history.md ([Oct 1 18:00–22:00Z](../archive/project-history.md#ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba),
[Sep 28 – Oct 1 14:00Z](../archive/project-history.md#ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3)).
A `§1b <time> block` reference in the queue below points there.
