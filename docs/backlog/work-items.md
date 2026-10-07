# Work items — long-form text for the open W items

Companion to [the status page](../status/README.md). The status page holds the queue,
the plan and the decisions; this page holds the full text of the open
long-form items that the plan rows point at: the source evidence, the
numbered steps with their status, and the binding **Forbidden** list.
Nothing here is a queue position — the order is the status page §1a. A step
marked done here is a document claim about the date given; the gate
result directory is the evidence. Closed items (W1–W5, W7, W11, W13,
W26, W28–W35) are in [project-history.md](../archive/project-history.md)
"START-HERE closed items". `§1b <time> block` references point at the
"START-HERE handoff archive" there. Every item below opens with its
current status, remaining action, governing decision and gate; what
follows that block is the dated record. W23 is the server compaction
item; the client connection-liveness item is W49 (renamed Oct 2).

**Durability and visibility.** The normative statement is
[architecture.md §3](../how-it-works/architecture.md) (the three "Data —" bullets) and
is not repeated here — earlier copies drifted. Vocabulary used by the
items below, defined there: a buffered byte is *pending* (not yet
published, in flight, or retrying on contention), *transiently failed*
(one drain hit a transport/budget failure: that `fsync`/`flush` returns
EIO, the byte stays pending) or *stalled* (D27: sticky EIO on every
description until it lands, bytes retained). Publication happens at
`fsync`, at every `flush`, and at the D24 landed-PUT REPORTs; `release`
publishes nothing; `fsync` waits for this client's writes only.

---

#### W6 — Run IO-500 (IOR easy, IOR hard, mdtest) — CORRECTNESS DONE, perf residuals open

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** correctness DONE (Sep 20; every IO-500 phase, 0 read errors). Of the three perf residuals: (1) ior-hard-write rate is W17/D1 (span publish in the tree, Sep 30 36-rank hard-write 0.640 GiB/s, 0 errors) with W38 (fold tombstone, 1 read error Oct 1) the open residual; (2) 1 GiB open is D2, implemented (`open()` adopts the row only; see the project-state rule); (3) rmdir rate fixed Sep 27.
>
> **Remaining action:** none under this number. W38 is plan row 4 in [the status page](../status/README.md) §1a; hard-write scaling beyond that is measured by the 9×4 debug run after each roll.
>
> **Governing decision:** D1 (span publish commutes), D2 (open adopts the row), D3 (no raised REAP gate). A chunk lock stays forbidden.
>
> **Gate:** IO-500 9×4 debug: every phase finishes, 0 `-R` errors on both reads, cold `hardscan bad=0` (`results/io500/20261001-074905-rdma` has the one W38 error; `20260930-183504-rdma` is the 0-error reference).

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

**Correctness gate met Sep 20 2026** (commit `cc828d8`, servers
`708b350`+, TCP, 9 clients × 4 ranks): 9×4 debug
`results/io500/20260920-debug-9x4/` — **every phase finished, ior-easy-read
and ior-hard-read 0 verification errors, every unlink OK**. The path from
the Sep 18 numbers (hard-write DNF in 2 h 18 m, `-W` 4244 errors, 76108
easy-read errors, 27 undeletable files) to this is in
[../project-history.md](../archive/project-history.md) "W6"; the fixes were: client
STALE retry cost + server partial-commit on STALE (hard-write livelock);
`dcache_image_current` / `snap_seq` ordering / forwarded-cmd reply index
(read coherency, `concurrent_appends`); Raft follower dedupe + leader AE
flow control (005 catch-up); **duplicate ino on concurrent CREATE**
(`alloc_hint_or_next` — apply is the allocator); reaper `lane-sweep`
batch-full misread as error (no file >8 MiB/lane was ever reclaimed).

| phase | Sep 30 18:35Z 9×4 RDMA | Sep 20 9×4 | Sep 18 9×1 |
| --- | --- | --- | --- |
| ior-easy-write | **4.563 GiB/s** | 0.814 | 0.263 |
| ior-hard-write | **0.640 GiB/s** (52.6 s, 0 fsync fail) | 0.044 (495 s) | 0.025 (9×4: DNF) |
| ior-easy-read | 16.8 GiB/s same-mount, 0 errors | 1.80, 0 errors | 0.60 (`-R` 512 errors) |
| ior-hard-read | **1.034 GiB/s, 0 errors; cold hardscan bad=0** | 3.55, 0 errors | 1.26 (`-R` 15982 errors) |
| mdtest-easy-write | **4.632 kIOPS** | 0.050 | 0.053 |
| mdtest-hard-write | 2.612 kIOPS | — | — |
| mdtest-easy-stat | 16.478 kIOPS | — | — |

Sep 30 row: `results/io500/20260930-183504-rdma/NOTE.txt` (handoff archive, [../project-history.md](../archive/project-history.md)).
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
   ([performance.md](../how-it-works/performance.md)) is the fix. **Decision D2 (Sep
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
  (matches the agent). Kill hung `io500` with `pkill -9 -x io500`. **Do not remount as the
  next step:** a remount is a client teardown and the killed ranks may
  hold unpublished bytes. Follow D27's procedure in
  [decisions.md](../status/decisions.md) (decisions table): `scripts/client.sh stop` must
  complete its drain; if it refuses, the stalled recs it lists are part
  of the run's result, and a forced teardown is the explicit
  `--force-discard` only. The old pre-D27 drain behavior is historical. The current controlled
  stop implementation must refuse unresolved work; its remaining fault/RSS
  gates are in the handoff. Retain every forced-discard report with the run. (D-state `request_wait_answer`
  ignores SIGKILL until `efs-fuse` answers or dies.) Never gdb-attach an MPI rank through a timeout'd ssh
  (left a rank T-stopped, job unrecoverable).

#### W8 — 9-node POSIX suite 1 (gate: 201 rows, 0 NOTRUN, every host)

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** gate MET (Oct 1: 9-host **200/201 on all nine**, `results/posix/20261001-160049`; the one skip is `mmap_write_read` by spec). Sub-items 0–2 closed (I17 `46d54e6`, I16 op-id window `43bdf6a`, W13 background compaction); sub-item 3's many-op floor was the per-peer sender wakeup bug (Sep 20).
>
> **Remaining action:** none; this is the standing 9-host regression gate. Anything below 200/201 on any host is a regression, not noise.
>
> **Governing decision:** none open.
>
> **Gate:** `tests/run_tests.sh posix` on nine hosts: 200/201 each, 0 NOTRUN, no `[None]` rows, under the 385 s cap.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

**State (Sep 21 22:15): 191–195 / 201 on every host, 0 NOTRUN, all nine
finish in ~62 s** (`results/posix/20260922-020950`). History and the
three fixes that got here are in the handoff archive ([../project-history.md](../archive/project-history.md)) (harness clock at
submit; `h->mu` contention after `read_mu`; KV WAL fsync per apply).
Earlier symptoms — nine hosts at the 385 s cap with `[None]` rows
(`results/posix/20260917-191430`), the 1.2 s / 1.03–1.08 s root mkdir,
`rmdir` ENOENT on a just-created name, `EBUSY` after the 10.4 s backoff —
are closed: whole-shard txn scans (`165e779`), stranded txn records
(`9534e53`, `3291c6d`), `read_mu` (`223da15`), peer-pool starvation
(`f10fec0`), harness (`a683def`), view (`4eb1419`), WAL hold (`84a2a55`).

What still fails, in order (details in the handoff archive, [../project-history.md](../archive/project-history.md)):
0. Half-applied cross-shard txns (I17) — **fixed `46d54e6` and gated**
   (Sep 22): freeze both leaders during `same_parent_storm`. The parent
   row stayed consistent (`nlink=5 nents=3` with three real children on
   the run that left names behind; the other run removed the parent).
   `arc_term_miss` moved. Details in the handoff archive ([../project-history.md](../archive/project-history.md)).
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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE (Sep 27: pin rules + LRU evictor, walk of 1M files levels at 233 MB RSS, `results/measure/20260927-w9-walk`). The per-tab floor residual was D18, implemented Oct 1 (`evict_cold_tabs`, `test_stage_evict`). The "Steps, once ratified" list below is **implemented; superseded as instructions**.
>
> **Remaining action:** none.
>
> **Governing decision:** D18 (evict whole cold tabs).
>
> **Gate:** the walk-RSS gate in [client-cache-design.md](../archive/landed/client-cache-design.md); `test_stage_evict`; posix 1 + 2; `leaks`.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

`g_client.export` in `efs-fuse` keeps one row per inode this client has ever
touched and one entry per chunk it has written or pulled, and evicts nothing —
so a client that walks a large namespace holds the whole tree in RAM and the
server-side memory wall reappears per client. This is step 12 part A.
The evictor and the pin rules are in the client as of Sep 27.

The plan, the pin rules that make eviction safe (a report builds its records
out of this table, so evicting a dirty row is data loss), and the gate are in
[client-cache-design.md](../archive/landed/client-cache-design.md). The Sep 23
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

Steps as planned Sep 23 — **all implemented Sep 27; superseded as instructions:**
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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE as a transport switch: 19810 and every client run RDMA (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`) since Sep 28, suites at the standing signature, 9-client dd 2551–2810 MiB/s (Sep 28) against the TCP bar of 1478–1984 — the step-3/4 speed bar is met by document claim. The Oct 1 RDMA fixes (`getifaddrs` cache, zero-copy send W39) are in the project-state rule.
>
> **Remaining action:** none. Do not roll back to TCP unless a suite fails; do not debug RDMA on 19810 — `tests/rdma_first_inode.sh` is the private gate.
>
> **Governing decision:** none open (W39 done; zero-copy receive is deferred, plan row K).
>
> **Gate:** posix jobs=1 200/201 and 9-host 200/201 on RDMA; `tests/rdma_first_inode.sh` 5/5 on an empty private table.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

19810's transport is RDMA as of the Sep 28 gate (the handoff archive in [../project-history.md](../archive/project-history.md)). Suites pass.
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
   clients) and record it in the ceiling table of [performance.md](../how-it-works/performance.md). Then TCP is no longer
   the default in the deploy rule and the status page.
5. If step 3 fails on anything that passes on TCP, roll back to TCP with
   the same `roll_efsd.sh --all` and bring the failure; do not debug
   RDMA on the live cluster with the suites down.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping 19810 without being asked
  (this item no longer needs it).

#### W14 — Server: snapshot install and the fragment probe are on the write path

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE by document claim: steps 1 (`7eecf1d`, `bbcbcb5`), 2 (a)–(b) (rolled Sep 29 02:35Z, `drop=0`), 3, 4 (D7 sentinel hint) and 5 landed; the remaining election triggers moved to W22 (D4/D5, done) and to the Sep 30 sender-channel fix (`85f5b31c`).
>
> **Remaining action:** none under this number.
>
> **Governing decision:** D7 (`path_hint = 0xffffffff` sentinel).
>
> **Gate:** no term change on either group during a 9-host posix or a 9×4 IOR; `access()` under 1 % of a single-client dd's server syscalls.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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
  than the Sep 28 numbers in `docs/how-it-works/performance.md`.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`;
  chunking InstallSnapshot differently (W11 is done; [../project-history.md](../archive/project-history.md)); the global or
  thread-local fd cache; changing `EFS_RAFT_SNAP_CHUNK`, `HOST_PUB_BATCH_N`
  or `EFS_RAFT_AE_BYTES` (all three were measured worse on Sep 28).

#### W15 — Client: copies and busy-waits are the write CPU

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** PARTIAL. Steps 1–2 done (`bbcbcb5`); step 3 in tree for `size == chunk` only; step 4 superseded by W39 (two-SGE zero-copy send, Oct 1); step 5 (`FUSE_CAP_SPLICE_READ`) has its kernel prerequisite (`fs.pipe-max-size`, Sep 29) but no roll record names it as landed — treat as not done. The remaining FUSE write copy is **W40**, deferred until `efsd --bench` (plan row J).
>
> **Remaining action:** none until W40 is taken; then W40's own text. Do not re-profile for this item before the bench.
>
> **Governing decision:** plan row J (W40 deferred); W39 done.
>
> **Gate:** the W28 gate (8 GiB dd+fsync, remount, read) and client CPU per GiB.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

**Status (Sep 29).** Step 2 is done in `bbcbcb5`
(`efs_rdma_reply_ready_us` pauses at most 16 times, then the caller
blocks on the CQ fd); on the run-3 profile the vDSO is 0.87% and the
symbol is under the 0.5% floor. Step 1's fresh profile is run 2 /
run 3 (handoff archive, [../project-history.md](../archive/project-history.md)): `ll_write_buf` `memmove` 7.5%, RDMA send `memmove`
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
profile are not on any item and are now **W20** and **W21** (their own sections in this file):
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
4. **`send_buf_pick` spin** (superseded by W39) — same as W14 step 5, one change for both
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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE by document claim: step 1 (RPC BUSY/NET/IO → EBUSY/EIO, never ENOENT — "the W16 mapping" that W43/W45 rely on) landed; step 2's D8 was answered (~3 ms per batch); step 3 did not arise. The Sep 23/29 hintless-NOT_PRIMARY backoff is the other half.
>
> **Remaining action:** none.
>
> **Governing decision:** D8 (answered).
>
> **Gate:** a FUSE op on an existing object never returns ENOENT because of an RPC failure: `grep 'but getattr\|inode-rpc:' fuse.log` empty across posix 1/2 and a 9×4 IOR.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

**Source (Sep 28 2026, ~18:30 EDT).** fstor007-mgmt mounted 19810 with
`client.sh --perf` while the 9×4 IOR was writing. Mount succeeded
(`fuse serving`, RDMA up). `df -h /tmp/efs-mount/` printed
`No such file or directory`. The fuse log has one line for it:
`inode-rpc: shard=0 type=47 exhausted 16 BUSY/STALE retries (10.3 s) -> EBUSY`.
Type 47 is `EFS_MSG_INODE_GETATTR`; shard 0 is the root's shard (even →
group 2). The mount was alive; `client.sh stop` unmounted cleanly. Same
class as the `INODE_LOOKUP` on shard 3745 that aborted IOR's `stat` in the
22:00Z run (handoff archive, [../project-history.md](../archive/project-history.md)). Not data loss, not a dead mount, not RDMA.

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
   from this same IOR is recorded in the handoff archive, [../project-history.md](../archive/project-history.md)), or group 2 was re-electing
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
   (W13's follow-on, closed), not the read path. Record which; do not
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
  attributed run in `results/measure/`; START-HERE §1b updated with the outcome.
- **Forbidden:** raising `HOST_READ_TRIES`, `HOST_TICK_US`, or the
  16-attempt client budget to make the symptom go away; serving a
  GETATTR from the follower's or client's local state without the read
  round (that is the linearizability I2/I10 guarantee); touching
  `entry/attr_timeout`; clearing `FOPEN_DIRECT_IO`.

#### W17 — Nine writers on one file: publish STALE storm, fsync EIO, and a FUSE request that outlives its process

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** PARTIAL. Step 1 (a FUSE request returns): D24's landed-PUT REPORTs and the record-sized REPORT wait removed the 20-minute class; the unconverging STALE loop itself is now **D27** (stalled publication: stop, retain, sticky EIO, refuse clean teardown). Step 3 (D1 span publish) is in the tree per the Sep 30 IO-500 (36-rank hard-write 0.640 GiB/s, 0 errors). Open residuals: **W38** (a fold tombstone without the span's bytes, plan row 4) and **W41** (`report_mu`, decided, plan row C). Step 2's per-chunk attribution counters are not recorded as landed.
>
> **Remaining action:** W38, then W41, in the status page order; implement D27 under 0a.
>
> **Governing decision:** D1, D24, D27, W41 (decided). **Fold actors, precisely:** a fold may be performed only by (a) the publisher whose span fills the last delta slot, inside that publish, or (b) a reader that observes a full chain, as a background PUT off the read path that never blocks the read. No other rank's `fsync`/`close` folds; no fold on a chain that is not full; a fold's observation must come from a body that holds every span it folds (W38).
>
> **Gate:** ior-hard NP=36 SEGS=3000 cold `hardscan bad=0`; `pkill -9 -x io500` mid-run leaves no D-state rank once D27 is in; D27's `stalled_publish.sh` g1–g9 including **g8 recovery at a full cap** and **g9 whole-call bound**; posix 1 200/201, posix 2 63/63.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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
     may fold as a background PUT, off the read — the second of the
     two allowed fold actors (status block); it never delays the read.
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
  a fold by any actor other than the two named in the status block
  (the chain-filling publisher inside its publish; a reader as a
  background PUT off the read path); a fold that blocks a read; a
  fold whose observation does not hold every span it folds (W38).

#### W18 — Client: the dcache reclaim is a table walk, and it is most of the client's CPU under a long write

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE by document claim: reclaim pops per-shard dirty lists, `dcache_init` once, `dirty ⟹ on_dirty` (`b6c1712d`, Sep 30/Oct 1); the `g_reclaim` herd fix (Oct 1) is the follow-on.
>
> **Remaining action:** none.
>
> **Governing decision:** none.
>
> **Gate:** `dcache_flush_slot_inner` self under 5 % and no `dcache_reclaim_main` scan at the top of a client profile during an 8 GiB dd.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

> **Current status (Oct 2 2026 05:45Z): CLOSED by measurement (P0.3).** Both group leaders profiled 20 s during the untraced 16 × 10 GiB dd (3.8 GB/s): libc `__memmove_avx_unaligned_erms` 1.59 % / 1.39 %, `try_commit` not in the top 60 (< 0.2 %); the leaders' top user-space cost is `__memcmp_avx2_movbe` 9 % of which 6.6 % is the GC frag pass's key scan (D26) (`results/measure/20261002-054132-p0-x16/perf-w19-*`). Steps 1–2 below are not taken.
>
> **Remaining action:** none.
>
> **Governing decision:** none needed (mechanical).
>
> **Gate:** the two profile shares above; both groups `commit == applied` with no term change during the measurement.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** steps 1–2 DONE (Sep 29, `present_chunks`/`present_extra`, `ll_setattr` reads `size` from the row). The residual — `st_blocks` is 0 for files this client did not write — is **D17, decided Oct 2** (per-lane present-chunk count in the lane stamp), plan row E.
>
> **Remaining action:** implementation landed; the ecrawl false-positive gate remains owed. D17 landed Oct 4 (dev cluster): lane stamps carry a present-chunk count, getattr sums it into the row image, the client takes the max with its local table; `du` on a non-writing client = size/512 (16 unit suites + posix2 two-client PASS; the ecrawl false-positive check is owed to 19810, which is down).
>
> **Governing decision:** D17.
>
> **Gate:** `stat` of a 16 GiB file under 1 ms idle; after D17, `du` of a file written by another client ≈ size/512.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE by document claim: steps 1–2 rolled Sep 29; the Sep 30 targeted-evict fix; D18 (cold-tab eviction) Oct 1.
>
> **Remaining action:** none.
>
> **Governing decision:** D18.
>
> **Gate:** two consecutive `du` runs with 0 syscalls over 10 ms in a client strace; `test_stage_evict`.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

#### W49 — Client: five liveness syscalls per connection checkout, plus an `fstat` per send (was "W23"; renamed Oct 2 — W23 is the server compaction item)

> **Current status (Oct 2 2026 05:47Z): CLOSED by measurement (P0.4).** `strace -c -f -p efs-fuse` 10 s during an ecopy of 18 705 files: 534 894 syscalls, fstat 0, getsockopt 156, recvfrom 78 (all EAGAIN) = 0.04 % < 1 % (`results/measure/20261002-054132-p0-x16/w49-strace-c.txt`). Steps 1–2 below are not taken.
>
> **Remaining action:** none.
>
> **Governing decision:** none needed (mechanical).
>
> **Gate:** the syscall counts above; posix 1 jobs=1 200/201.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** SUPERSEDED by W43 / D25 (plan rows 1, 3, A): the truncate path's silent NOMEM was the idle-cluster half of this symptom; W43 b made it an honest EIO, D25 (decided) makes it complete. The BUSY→EBUSY mapping is W16's.
>
> **Remaining action:** none under this number; D25 under W43.
>
> **Governing decision:** D25.
>
> **Gate:** `tests/stress/truncate_big.sh` exit 0 with gates t1–t9 (retained prefix, zero tail after re-extension, no stale beyond the fence, sweep vs new-epoch writes, restart mid-sweep, apply bound, **durable truncation history across repeated shrink/extend/partial-rewrite**, history bound, boundary rewrites); a loaded `open(O_TRUNC)` of a 10 GiB file returns within the SETATTR budget.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE by document claim: `futimens` on multi-lane files no longer EINVALs and `atime_nsec`/`ctime_nsec` ride the staged row (Sep 30 ecopy review); the mtime-lost-at-close case was fixed Sep 30 (utimens flushes pending writeback first); BUSY mapping is W16's.
>
> **Remaining action:** none.
>
> **Governing decision:** none open.
>
> **Gate:** `ecopy --verify` on a multi-lane tree: 0 `futimens: Invalid argument`, 0 atime mismatches; `mtime_repro.py` rows print the set time.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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
chases them. **`posix_fallocate` returning `EINVAL` is the same gap,
seen through glibc (Oct 1).** `efs_ll_ops` has no `fallocate`; libfuse
replies `ENOSYS` and the kernel turns that into `EOPNOTSUPP` for the
`fallocate` syscall. glibc's `posix_fallocate` does not return that: on
`EOPNOTSUPP` it writes zeros itself, and it does **not** emulate
`EINVAL`. On an `O_DIRECT` fd the fallback `pwrite` is not 4 KiB-aligned,
so it returns `EINVAL` (`fuse_odirect_unaligned` does the same for an
application `O_DIRECT` write). A normal fd's `posix_fallocate(0, 4096)`
succeeds only because of that zero-fill — `opt_fallocate` PASS is not
the filesystem reserving blocks. There is no preallocation. A tool that
treats the `EINVAL` as fatal writes nothing. **Implement the handler:
W26.**

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

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** STANDING RULE, not a queue item. Pruned to the rule Oct 1.
>
> **Remaining action:** apply it whenever a run directory is cited or stops being cited.
>
> **Governing decision:** none.
>
> **Gate:** `results/` contains only directories a live document cites.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

`results/` holds only runs that a live document cites (this page, the
rules, `docs/`, `tests/`; `project-history.md` and `design-history.md` are
archives and do not count). Commit a run directory when it is cited as a
gate; otherwise delete it. Pruned to that rule on Oct 1 2026; `git log --
results/` keeps the rest.

Run `python3 docs/gen-architecture-full.py` (or `make docs-check`) after any doc edit: it regenerates
`architecture-full.md` **and** `architecture.html` from the markdown sources
and validates links plus every `I1..I25` reference. Never edit either
generated file. `make test` must be fully green — there is no
accepted-failure list.

---

**Idea checked Oct 1 2026 — synchronous encryption. Not a decision. Do not implement.**

The user asked how transport and at-rest encryption could work, and whether a key created with the cluster would let only the Linux user who mounts with that key see the contents. Conclusions, for a later design pass:

- Nothing encrypts today. `EFS_MSG_HELLO` checks the build id. Any host that can reach 19810 is a peer. Modes are enforced in the FUSE process; the client tells the server its uid (`docs/backlog/product-gaps.md`).
- One key, created at `raft-mkfs` and shown once. The cluster stores a verifier, not the key. A mount proves it knows the key. The key file is mode `0600` and the mount is not `allow_other`, so other uids on that machine see nothing at the mount point. Root on that machine can still read the key (ptrace is open here). Any other host that has the key can mount. Every holder of the key sees every byte it encrypts. Per-user secrecy is a different key per user.
- Fragments, on the client, after parity. Split the chunk, XOR the parity, then encrypt each of the three fragments with its own IV stored beside the fragment. `PUT` and the disk see only ciphertext. A read decrypts, then XORs if it must rebuild one fragment. XOR of ciphertext does not reconstruct, so encryption sits outside the parity. The object name stays a hash of the plaintext, so two clients still agree on one object; the IV is not part of that name. The server stores opaque fragments and does not need the key to hold them.
- Metadata is the open choice. Names and directory updates are applied by the servers. Without the key they cannot apply encrypted names, so either the namespace stays plaintext on disk (a pulled NVMe reveals names, not file bytes) or `efsd` is unlocked with the key at start and the KV and Raft log are encrypted on the way to disk. The key must not live in the Raft log, or the disk contains the key that decrypts the disk. A server that was not unlocked cannot serve.
- Transport is the same idea on the frame: encrypt before send, decrypt before the handler, on the RDMA path and on the TCP side channel large frames already use. A traffic key derived from the cluster key at connect. Ciphertext fragments on the wire still show sizes and which object was touched; the frame hides names and the access pattern.
- "Synchronous" means the `pwrite` and the wire only ever see ciphertext. There is no later encryption pass.

**Decisions only the user can make — bring the evidence, do not start.**
These came out of reviewing efs as an HPC parallel filesystem. Each one
changes what efs *is*, so an agent must not pick a side; but each has a
cheap measurement an agent can produce first, named here:

- **Per-file layout control.** Chunk size and the EC profile are per export;
  there is no per-file or per-directory equivalent of Lustre's `lfs
  setstripe`. A 4 KiB-record checkpoint and a 1 GiB-per-rank dump therefore
  get the same 128 KiB geometry, and the declared 32× small-write
  amplification ([architecture.md §9](../how-it-works/architecture.md)) has no opt-out.
  Evidence to bring: W6's IOR-hard/IOR-easy ratio and rw-4k from W4.
- **An interface beyond FUSE.** FUSE is the only client. libfuse 3.10.2 cannot
  negotiate `FUSE_MAX_PAGES`, so every request is ≤128 KiB regardless of
  `max_write`; it cannot emit `FOPEN_PARALLEL_DIRECT_WRITES`; and Linux takes
  the inode lock exclusively for extending direct writes and the parent
  directory lock for `O_CREAT`, **per mount** (already in
  [architecture.md §9](../how-it-works/architecture.md) as an open kernel-interface item).
  So 64 ranks on one node writing one file serialize in the kernel before efs
  is called. Every production PFS has a kernel client, a user-space library,
  or an MPI-IO ADIO driver. Evidence to bring: W4's per-client scaling and a
  1-node 8-rank IOR-easy vs 8-node 1-rank IOR-easy comparison from W6.
- **Whether `write()` is durable** — W2 closed: spec says `fsync`/`close`/
  `O_SYNC`. Do not reopen as publish-on-write. `O_SYNC` wiring is a later
  item, not a silent side-cut.
- Already listed before this review: C1 relaxed coherence; a pressure-triggered
  directory-spread bound (unspecified; now indexed as [W81](#w81)); cutover of a 36T `efs-test`; any new
  REPORT or SNAP wire shape.

**Bigger than this queue.** [product-gaps.md](product-gaps.md) inventories
what is missing before efs is a filesystem anyone could run — including the
things that contradict a guarantee the spec already makes (no fragment
repair, no protection-debt tracking, no session/fencing on the client, and
W1/W2 in [../project-history.md](../archive/project-history.md) "START-HERE closed items"). That was the historical review's scope, not the current index: W71–W77 and
W81 now give these source gaps canonical queue identities and gates. Accepted
contracts remain binding; unresolved design choices stay in the decision
register. Do not treat the queue as the whole distance to a product.

#### W22 — Server: the snapshot cadence makes InstallSnapshot the steady state, and a follower inside an import campaigns

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** DONE by document claim: D4 (snapshot by log bytes, retained window) and D5 (sliced import) rolled Sep 29; D6 closed by D11 (not the sharing; `--meta-storage` exists, default unchanged); the rotated-SNAP-record replay bug was fixed Oct 1 (`test_rotation`).
>
> **Remaining action:** none.
>
> **Governing decision:** D4, D5, D11.
>
> **Gate:** `import start` count 0 on every node across a 9×4 IOR; no term change; `make test` (`test_raft_store`).

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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

> **Current status (reviewed Oct 7, round 6):** D9–D13 implementation and Sep 29 rollout are historical acceptance. The follower memory/lag bound remains unproven. The first [completed Oct 5 measurement](../../results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt) wrote 4352 MiB and stopped after 27 seconds at the RSS threshold. The summary's zero-L0/no-effect interpretation is invalid: [W89](#w89) finds four KV values packed into one TSV field. Re-expanding the retained samples shows node3 reached 22 L0 files / 3,367,253 bytes, versus node1 4 / 105,281. Raw server logs confirm L0=22 during the park. The run still does not validate the pressure bound. The earlier [setup failure](../../results/measure/20261005-014440-w23-stalled-compactor/SUMMARY.txt) is superseded, not the only run.
>
> **Remaining action:** repair TSV serialization/validation and derived summaries, then force enough L0 pressure to test the bound, compare follower RSS with healthy leaders, and record lag/catch-up under the finite stop conditions below. D26 watermark/cursor acceptance is separate; it does not prove W23.
>
> **Current source distinction:** `kv_maybe_flush_locked` keeps the memtable when L0 bytes hit the cap or no file slots fit; with the production compactor started it kicks compaction and returns, rather than waiting. A `kv-compact: backpressure` line marks deferred flushing, not proof that the pump blocked. `host_pub_batch_propose` gates legacy REPORT batches on local L0 bytes; that is not cluster-wide follower-lag admission or a universal metadata proposal gate. Snapshot bytes are a trigger, not a memory budget.
>
> **Governing decisions/gates:** D9–D13 and D26; real pressure measurements, unchanged metadata latency and bounded recovery. Preserve the historical correction below, but do not use its old “64-file pump stall” description as current source behavior.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

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
writing 408K fragments per server; the handoff archive in [../project-history.md](../archive/project-history.md) has the full list).**

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

**Steps as written Sep 29 (D9–D10 were DECIDED the same day and rolled 05:31Z; D12/D13 followed — historical):**

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

**Correction (Oct 2 2026) — the bound in step 1 is unproven.** Step 1
says the memtable "is bounded by what the log can commit ahead of the
KV (512 MiB since W22.1)". It is not: the 512 MiB figure is a
*snapshot trigger*; it neither admits nor refuses writes, and a
follower acknowledges AppendEntries on log persist, not on apply, so
a follower whose compactor is stalled accumulates unapplied log (on
disk) without the leader noticing except through follower-served
`host_wait_applied` timeouts. The mechanisms that actually exist:
(a) `memtable_max` flushes the memtable to an L0 file; (b) the 64-file
L0 cap, at which the apply path **blocks** (`kv-compact: backpressure`)
— a pump stall, which is the thing D9 forbids, so today the bound is a
stall; (c) D9's admission BUSY on local L0 bytes over 1 GiB applies to
the leader's own KV only; (d) D12/D13 keep `n_l0` under the cap by
L0→L0 merges while the compactor runs. **Remaining action (no
decision needed to measure).** Fault hook `EFS_FAULT_COMPACT_STALL=1`
(compiled only with `EFS_FAULTS=1`), **fault location:** `compactor_main`
parks at the top of its loop, *between* iterations, holding neither
`l->mu` nor `h->mu` and owning no pinned view — i.e. the compactor is
alive but never starts a merge. Memtable flushes to L0 (`kv_flush_locked`,
apply-path) continue; only L0→L1 and the D12/D13 L0→L0 merges stop.
Parking while holding `l->mu` would test a lock-hold, a different
failure, and is not this experiment. **Run:** private 3-node cluster
(`tests/rdma_first_inode.sh` layout on one fcstor, `/dev/shm` or a scratch
dir), one client, `dd bs=1M` of a non-zero source to fresh files, the
hook set on **one follower only** (the leader keeps compacting, so the
leader-side admission (c) is not what fires). **Sample every 5 s per
node:** memtable bytes, `n_l0`, `pump_hold_max`, `apply-sleep` count,
`kv-compact: backpressure` count, `commit − applied`, RSS. **Bounded
stopping condition — stop at the first of:** the follower logs
`kv-compact: backpressure` (bound = the stall at 64 L0 files, name the
bytes written at that point); follower RSS exceeds 2× its pre-run RSS;
follower `commit − applied` exceeds 10 000 entries for 30 s; 10 GiB
written; 10 minutes. **Then** clear the hook (`/tmp/efs/fault`) and
record how long the follower takes to reach `commit == applied` (must be
under 2 min, else that is a second finding). **Result:** the dir under
`results/measure/` names which of (a)–(d) stopped the run and at what
size. A finite run can show a bound that fired, or lag that grew for the
whole run with no bound observed; it cannot prove unbounded growth —
write whichever it is. If the answer is "a pump stall at 64 L0 files" or
"no bound observed within 10 GiB / 10 min", that is a design ask (a
follower-lag admission rule) — bring the numbers, do not pick.

- **Read:** `src/kv/kv_compact.c` (`kv_maybe_flush_locked`, `compactor_main`,
  `kv_compact_locked`), `src/kv/kv_lsm.c` (`kv_flush_locked`), `src/kv/kv_seg.c`,
  W13 (closed; [../project-history.md](../archive/project-history.md) "START-HERE closed items") and the
  "L1 compaction is a background thread" learning in the project-state rule.
- **Forbidden:** raising `KV_LSM_MEM_DEFAULT` or `KV_LSM_L0_DEFAULT` as the
  fix (W13, closed); raising the election timeout, `HOST_TICK_US`, or the 400 ms
  apply budget; any compaction step under `h->mu`; making `fsync` succeed
  on a merged-back dirty set; moving `mdraft/` to another device to hide
  the compactor's I/O (D11 says it is not the sharing).

**When the status page's queue is empty,** the next task comes from a measurement,
not from this page: run the gates in [testing.md](../how-it-works/testing.md), and take
the largest gap between what a gate reports and what the ceiling table in
[performance.md](../how-it-works/performance.md) says the hardware allows. If closing it needs a design
decision the spec does not contain, stop and ask ([developing.md](../how-it-works/developing.md) §4).

---

---

## Queue rows and plan texts moved from the status page (Oct 3 2026)

The sections below are the full texts of the status page's queue rows and
plan rows, moved here verbatim so [the status page](../status/README.md)
can be a one-line-per-item index (table cells reflowed to sections; the
text is unchanged). The queue order and the one-line rows live in
[../status/README.md](../status/README.md) §1a; references of the form
"§1a", "§1b", "row N" and "plan row N" below refer to that page's tables
and its archived handoff blocks unless they are linked. D-numbered
decisions cited here are in [../status/decisions.md](../status/decisions.md).

**Open correctness rows (Oct 1–2 2026; these go before every
performance row). Rows that are done (1j, 0f, 1i, 0d, 1a–1h) are in
project-history.md "START-HERE closed items".**

## W54 · a fold's GC deletes the live base (queue row 0i)

**Oct 5 implementation checkpoint — IN TREE, local gates pass; uncommitted,
cluster gate pending.** `efs_meta_apply_publish` now skips GC for a superseded
base or span (including replay tombstones) whose generation matches the new
base or whose nodes/checksums alias it. Distinct superseded objects still emit
GC records. `test_gc_fold_live_alias` covers live-span and tombstone aliases,
same-generation and different-generation cases, fold replay, and a non-alias
reclamation control. The regression produced 12 failed assertions before the
fix; the complete `test_meta_apply` suite passes after the fix, including under
ASan/UBSan. No GC-side guard was added; that remains undecided below. Remaining:
roll and the two-peer fold / wait-for-GC / remount cold-read gate in (c), plus
cold IOR-hard verification and hardscan. No cluster rollout performed here.

**W54 · a fold's GC deletes the live base: when the folded image's object is also a tombstoned span (same content hash → same generation), `efs_meta_apply_publish` queues that generation for GC and the reaper unlinks the fragments the row still names; the next cache-miss read is EIO (Oct 2 17:04Z, IO-500 9×4 ior-hard-read `MPI_ABORT`). MUST FIX before any performance row; data loss with no repair**

**Follow-up steps.** (a) **fix the apply (mechanical, L7 already says a fragment set the live row names is not an orphan):** in the fold branch of `efs_meta_apply_publish` (`meta_apply.c` ~3625, "A full image replaces the spans") skip a span whose `generation == stored.generation` or whose `(nodes, checksums)` alias `stored` (`chunk_aliases`); apply the same filter to the tombstone walk, which today would also GC the previous base when a new image aliases it; (b) `test_meta_apply`: span of object X, then full image with `candidate_gen == X` → the batch holds no GC key for X; (c) repro + gate: two peers write adjacent ranges of one chunk so both merge to the identical image (ior-hard shape; or a posix2 `peer_shared_chunk_fold_gc`), wait past the GC latency (≥ 2 s), remount, cold read; plus a cold `ior-hard-verify` + `hardscan` after every 9×4 run (this run surfaced it only because a same-mount read missed the cache); (d) **ask (not decided):** a GC-side guard — `host_gc_record` point-gets the chunk row before each delete and skips a generation it still references (one get per record on the GC thread = D26's cost; the GC key lacks the inode generation the chunk key needs); (e) stop-all/start-all roll of the four servers, then the gate in (c)

**Evidence and limits.** `results/measure/20261002-165956-redeploy-posix-ior/SUMMARY.txt`; fcstor015 `fuse.log` 17:04:40Z `fetch published ino=656804 ci=181944 rc=-9 then pull rc=0 rc=-9` (row unchanged across the pull — not a stale map), `efs-fuse read: decode error (efs_rc=-9) off=23847816512 len=47008`; `raft-getchunks 656804 181944`: `base_gen=15366554570668337119 spans=2` with tombstones `4218386339069290638 seq=1088` and `15366554570668337119 seq=3731` — the base IS the second tombstone; on disk under `…/0065/6804/177/` only `181944.{0,1,2}.3089159234672355549` (one per fcstor003/004/005), the live generation gone. `gc_queue` (`meta_apply.c:3154`) checks nothing; `host_gc_local_del` → `efs_store_del_if_sum` passes on the same object's sum. Server code unchanged since `2b5a25df`; the hit is probabilistic (same-gen collision, GC runs, cache miss) so the 15:21Z clean hard-read does not clear it. The file `/tmp/efs-mount/io500/2026.10.02-13.02.57/ior-hard/file` is unrecoverable (test data; delete it). Forbidden: zero-filling the read (I9); a longer GC latency to hide it; a client-side retry loop


## W55 · span committed to raft, fragment PUTs never landed → read EIO, data loss (queue row 0k)

**W55 · a span is committed to raft naming generations/node sets whose fragment objects do not exist on any node — nothing was ever PUT, nothing was reaped — and every subsequent read of the chunk fails with `EFS_ERR_DECODE` (-9) → EIO (Oct 5 2026, xorinox test cluster: 3-node libvirt, 2+1, gateway nfsd re-export mounted by a macOS client). Data loss with no repair; same user-visible signature as W54 but a different mechanism — W54's reaper deletes objects that exist, here the objects never exist.**

**Follow-up steps.** (a) **trace the span write path stage → PUT fragments → report span:** a committed span whose PUTs never ran must be impossible; find where the PUT leg is skipped or its failure swallowed while the REPORT still lands (start at the `putid miss` fallback — "identity from staging table" — in the client report path, cf. W27); (b) add two log lines first, then repro: the span REPORT (putid/gen + node set) and per-fragment PUT completion/failure keyed by that gen — one repro then shows which leg vanishes; (c) repro loop on the test cluster: `cp <file> /mnt/nfs-export/` from the Mac, then `efs-mgmt raft-getchunks <seed> <ino>` + `find /data1 -path '*<ino>*'` on each node — span-without-objects = hit, objects present + read back = clean; (d) decide the relation to W27 (REPORT identity / `putid miss`) — possibly one root; (e) gate: the repro loop clean 20/20 plus a posix suite run on the nfsd re-export mount

**Evidence and limits.** xorinox cluster, Oct 5 2026 (efsd built 20:59Z; both hits on gateway xefsgw, fuse log `/mnt/efs-fuse-efs.log`, server logs `/data1/efsd.log`). **Hit 1** — ino 8193, written 21:38:06Z (pre-`all_squash` export, macOS EXCLUSIVE4 mode-0 create, uid 501): read 21:39:45Z → `fetch published ino=8193 ci=0 rc=-9 then pull rc=0 rc=-9`, row identical before/after the pull (`gen=0 nd=1 seq=5`), empty `{…}/8193/0/` fragment dirs on all nodes; raft applied only lease open/close (kind 8/9) for the ino; the staged span's **seq advanced 5→9 across pure read attempts** (22:00:04 → 22:00:25Z, no writes — reads mutating the staged record). **Hit 2** — ino 32769 (`/tiny_files.py`, 2922 B), written 22:00:38Z *after* the export fix, cluster fully up (no efsd restarts 21:14 → 22:05Z): `raft-getchunks 32769` = `ci=0 nodes=0,0,0 base_gen=0 ck0=00000000 spans=1 seq=1; span off=0 len=2922 gen=5683377705060155088 nodes=3,1,2`; `find /data1/data/exports/1 -path '*32769*'` on all three nodes: **zero fragment objects**; reads EIO persistently from Mac NFS and FUSE-direct on the gateway. **Excluded:** GC/reaper (no delete records near either hit; W54's mechanism needs objects that exist); the export uid/squash (hit 2 post-dates the `all_squash` fix by 20 min); a stale client map (row identical across pull). **Controls, same window, same mount, all landed + read back fine:** rsync of a git tree (20:44–21:08Z), fresh 2922 B random file (21:48Z), same content to a new name (21:49Z), create-then-overwrite (21:52Z), FUSE-direct write on the gateway. Both hits were `cp` of the same source file from the same Mac client; the trigger is not isolated (a third cp onto the broken name at 22:05Z also failed but is contaminated — it raced an efsd roll). Client log in the same window shows `efs: report rec … identity from staging table (putid miss, n=1)` for two other inos (29918, 31158) whose files read fine. Both hit files unrecoverable (objects never existed; test data — delete). Forbidden: zero-filling the read (I9); treating this as NFS-export configuration (FUSE-direct reads fail identically); a read-side retry that papers over the missing objects


## W56 · root-level rename leaves a ghost name in the renaming client's local lookup (queue row 0l)

**Oct 6 fix:** `f8fef814` drops matching old-directory-name cache rows from both lookup tabs after authoritative rename. It preserves other names/chunks and ignores a replacement inode. NUC full POSIX jobs=4/jobs=1 and full posix2 PASS; the root directory regression now passes. Xorinox roll remains owed.

**W56 · after `mv /export/A /export/B` with the parent the export root (or any spread directory), the renaming FUSE client keeps resolving the old name in LOOKUP — stat/open on the old path still succeed and return the renamed inode — while the server metadata and every other view (parent READDIR, other clients, fresh mounts) are correct. The ghost lasts until remount (Oct 5 2026, xorinox test cluster, gateway FUSE mount). User-visible trigger: a tool that stats the output dir before creating refuses to run against the ghost of a just-renamed directory.**

**Follow-up steps.** (a) fix: the rename local-apply must resolve the old dentry with the same tab order as `efs_export_lookup` (dentry-hash tab first for root/spread parents) and delete the name-index entry there — today `efs_export_rename_at` (`metadata.c`) probes only the parent's shard tab, so for a root-level entry `name_idx_get` misses, the by-ino fallback `efs_export_rename` upserts the row under the new name, and nothing removes `(root, old_name)` from the hash tab; (b) audit `efs_client_unlink`'s local apply for the same tab asymmetry (rmdir tested clean Oct 5 at both levels, but confirm the code uses the lookup tab order rather than the parent shard only); (c) gate: IN TREE Oct 5 — `tests/posix/posix_suite.py` `@root` group (`root_rename_dir_old_name_gone` FAILS on the Oct 5 build, reproducing the ghost within the suite; the sibling root-level rename/create/unlink/mkdir tests pass) — then full posix jobs=1 + posix2 on the dev cluster after the fix

**Evidence and limits.** Repro on xefsgw (xorinox 3-node libvirt, bits=12 export, efsd built Oct 5 22:05Z): root-level `mkdir rt; mv rt rt2; stat rt` → still resolves ≥ 65 s later (bug); nested `nt/sub → nt/sub2` → ENOENT (clean); root-level and nested `rm -rf` → ENOENT (clean). Narrowed Oct 5 by the new posix `@root` group on xefsct1: only **directory** renames with a root parent ghost — root-level **file** renames and root→nested dir moves are clean, i.e. the missed tab holds directory dentries, not file dentries. Ghost is not the 0j 50 ms lookup memo (old dir name still resolves ≥ 2 s later). Mechanism chain: `efs_fuse_lookup_at` answers directory LOOKUPs from the local staged table with no RPC (by design, the mkdir-walk O(n²) note in `efs_fuse.c`); on a sharded export, dentries of root/spread parents live on the `hash(parent,name)` dentry shard and `efs_export_lookup` (`metadata.c`, the "Dentry shards only" fix) checks that tab first — the rename local-apply never does. Not server-side: parent READDIR, other clients and fresh mounts are all correct; it is not the kernel dentry cache either (the VFS moves the old dentry on a same-mount rename — the stale answer comes from efs-fuse). Forbidden: routing directory LOOKUPs via RPC as the "fix" (that path exists for files and was deliberately not taken for dirs); a time-based expiry that papers over the missed removal


## 0j · the client's 50 ms lookup memo returns pre-mutation stats (queue row 0j)

**Oct 5 implementation checkpoint — IN TREE, uncommitted; local gates pass,
Linux integration gates pending.** FUSE mutation requests clear the 32-slot
memo at entry and exit, and a balanced active-mutation count suppresses memo
use while operations overlap. LOOKUP captures a mutation serial before reading
its row; a reply spanning a mutation cannot reinsert stale attributes after
invalidation. Coverage includes write/write_buf, setattr (chmod/chown/size/times),
namespace mutations and replacement targets, O_TRUNC, fallocate, xattrs,
flush/fsync/release publication, and asynchronous writeback. Clearing all slots
is conservative: unrelated mutations also cause misses; no inode-resolution
RPC is added solely for invalidation. No-mutation LOOKUP → GETATTR still gets
its one-shot 50 ms hit, and attr_timeout remains zero.

`make test-lookup-memo` extracts the production memo and representative FUSE
callbacks with RPC stubs: writes, setattr reply attributes, link/unlink/rename,
O_TRUNC, failures, fallocate early exits, overlapping mutations, stale RPC
completion, TTL expiry and read-only/fresh hits pass. ASan/UBSan pass; earlier
client-memory tests pass again. An adapted-copy Mac FUSE syntax check passes;
this is not a production Linux build. Remaining gates: Linux posix jobs=1 back
to the recorded 200/201 baseline and Spark du performance measurement. No roll
performed. Next code item in the agreed sequence is W38.

**Client regression in `20745142`/`efc0f499` (not W54, same gate run): `lookup_memo_take` answers a GETATTR within 50 ms of the LOOKUP from the LOOKUP row — a stat right after write/chmod/link/utimens on the SAME client shows the pre-mutation row.** posix jobs=1 **164/201, 36 fail** (`results/posix/20261002-170119`: `basic_dd_rw` size 0, `attr_chmod` mode 420, `hardlink_basic` nlink 1, `attr_utimens_ns` old mtime …); posix2 63/63 (the peer holds no memo). The 15:19Z tree was 200/201

**Follow-up steps.** mechanical: the memo must be invalidated by every local mutation of that ino (write/truncate/setattr/link/unlink/rename/utimens — the same set that already drops `g_lookup_memo` candidates on nothing today), or consumed only when no local op on the ino happened since `lookup_memo_put`; gate: posix jobs=1 back to 200/201 on one client and the Spark `du` number the commit cites not lost

**Evidence and limits.** `efs_fuse.c` `lookup_memo_put`/`lookup_memo_take` (`LOOKUP_MEMO_US` 50 ms, 32 slots, keyed by ino only). `attr_timeout` stays 0 (decided). Not a server change


## W38 · ior-hard fold tombstone without the span's bytes (queue row 0e)

**Oct 5 implementation checkpoint — IN TREE, uncommitted; deterministic
local regressions pass, historical IOR gate pending.** Two unsafe paths were
identified in the current client. STALE replay fetched a base plus live spans,
then copied the older local image over it when the base still matched this
client's object (including the no-range branch); the PUT named the fetched
span list despite lacking its bytes. Replay now preserves fetched live spans
and overlays only owned dirty ranges; tombstones do not count as live bytes.
A full image without a byte-backed observation now names an empty list unless
it is a true whole-chunk overwrite, forcing a server STALE/refetch if spans
exist. Metadata-only list/sequence learning is removed. A span whose local
chain fills during PUT can become a full image only with the captured byte
observation; otherwise it returns STALE without replacing the staged mapping
or PUT identity. The existing flush failure path retains/re-dirties its body
for a later retry; this race can surface an error on the current flush rather
than silently publish an incomplete fold.

`make test-fold-observation` compiles production replay and PUT functions with
separate byte images and metadata plus RPC stubs. Restoring the old replay
conditions in a temporary copy reproduces loss of the acknowledged 4256-byte
span. Tests cover owned ranges, cleared ranges, tombstones, table changes during
PUT, missing observations, chain-full fallback, whole overwrites, failed PUTs
and no failed-object publication. Local regression and ASan/UBSan pass; write.c
syntax, earlier client-memory/0j/D25-helper tests and metadata suite pass. This
proves the identified paths, not that the cited historical IOR run has been
reproduced on the cluster. Remaining: traced one-client four-rank IOR-hard,
cold hardscan, inspect bad rows, then the 9×4 cold verification gate below.
Next code item in the agreed sequence is W43/D25 production wiring.

**W38 · ior-hard: a client's full image folds its own published span without the span's bytes (4256 B of zeros, committed)**

**Follow-up steps.** the F3 block in §1b (Oct 1 08:05Z): one-client 4-rank IOR hard with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, `hardscan` cold, `raft-getchunks` on each bad chunk; fix on the client (the fold observation must come from a body that holds the span)

**Evidence and limits.** `results/io500/20261001-074905-rdma` (NOTE.txt, hardscan.txt, getchunks-118842-118844.txt): ino 10897 ci 118843 base 1774…2861 + len-0 tombstone 1838…0185 seq 1222; 1 of 747720 records; first IOR with W30 in the client. Data loss: goes before 0c


## W36 · rename-vs-unlink of one source both succeed, dangling dentry (queue row 0c)

**Current acceptance, reviewed Oct 7.** The exact-source/value PREPARE and
UNLINK verdict changes passed the named `peer_rename_vs_unlink_src` gate
20/20 on xorinox `b4a75492` and again on final NUC `128f6b7d`, with broad
peer acceptance. W36 is accepted for those recorded builds; repeating the
gate on a new rollout is a release obligation, not evidence that the old
implementation remains unaccepted. The historical recurrence/next-rollout
instructions below are superseded by the
[round-2 ledger](../archive/queue-review-20261007-round2.md).


**Oct 6 reply-path fix:** `75b06624` makes simple UNLINK wait for the actual apply verdict. Previously an apply NOT_FOUND/BUSY was hidden by `host_wait_settled`, allowing both operations to report success even when unlink lost. This is distinct from the earlier exact-source PREP protection against dangling entries. NUC race gate 20/20 and full posix2 PASS; repeat on the current xorinox build before closing its recurrence.

**W36 · rename-vs-unlink of one source both succeed, dangling dentry** (posix2 `peer_rename_vs_unlink_src`, 1 in 6)

**Plan row 5 (in tree).** the hole was one path: `efs_meta_apply_unlink_op` / `rmdir_op` probed the inode row for a pending intent (`efs_txn_key_busy`) but not the DENTRY they delete unversioned, so an unlink could drop the source name + row under a RENAME whose dentry EXCL had landed and whose inode-row REDUCE had not; the rename's RESOLVE then PUT the dest dentry over a dead row (`-?????????`). Both log paths now probe `k_loc`/`k_hash` and answer BUSY (client retries → ENOENT after the rename resolves). The silent NOT_FOUND→OK in `apply_unlink_cmd`/`apply_rmdir_cmd` is gone (W45) Gate: posix2 `peer_rename_vs_unlink_src` 20/20.

**Follow-up steps.** the F1 block in §1b (Oct 1 07:45Z): trace the two txns with `APPLY_LOG`, decide between `apply_unlink_cmd`'s silent NOT_FOUND→OK and an EXCL DEL that passes on an absent key, fix that one

**Evidence and limits.** evidence `/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b` on 19810 (`-?????????`), `~/efs-runs/p2r422.log`, `results/posix2/20261001-073048`. Correctness: goes before 1a–1h

**Recurrence (Oct 6 2026, xorinox cluster).** The same dangling dentry appeared at `/mnt/efs/posix-2c/peer_rename_vs_unlink_src/b` (found by the user's `find -ls`: readdir lists `b`, stat → ENOENT), created ~00:41Z by a posix2 run on a fresh (mkfs Oct 5 19:31Z) 3-node cluster running `v0.1.0-pre-alpha-12-g3d3f17c2-dirty` — a build that **contains** this fix (`2b5a25df`) plus the uncommitted Oct 5 D25 transaction/`meta_apply.c` work. KV-level proof, no client cache involved: `raft-readdir 3492` lists `b`, `raft-lookup 3492 b` → `ino=0 mode=00 nlink=0`. So either the fix's BUSY-probe does not cover the path this run took, or the dirty tree's txn changes reopened the hole — the owed 20/20 gate would have caught this; run it before anything else on the next cluster. The dangling name is still in the KV for inspection (cleanup: `efs-mgmt raft-unlink <node> 3492 b` — itself a probe of the fixed path).

**Guarded cleanup (Oct 6, b4a75492).** The retained `b` was still present after
xorinox deployed d0e8dce4; its parent mtime/ctime still matched 00:41:38 UTC.
This is persisted damage, not evidence of a new occurrence on d0e8dce4.
Unlink now removes a dangling regular-file name only when its missing inode and
LOCAL parent are in the same shard, with parent/dentry/inode/dseq intent guards
and atomic directory/opid updates. No inode or object is fabricated or erased.
Directory and foreign-shard corruption still fails closed.
[Repair and live acceptance checkpoint](../../results/measure/20261006-xorinox-orphan-unlink/SUMMARY.md).


**Local follow-up (Oct 6 2026, uncommitted).** Reproduced the complementary
race before rename's first source PREPARE: log-path unlink deletes the source
and its last-link inode without bumping the dentry's transaction version, so
version-only EXCL DEL still accepts the absent source. The original BUSY probes
protect already prepared names, not that earlier window. The committed code
already has this mechanism; the dirty build does not prove D25 introduced it.
`efs_meta_capture_dentry_drop` now checks the original dentry identity and
captures exact local/hashed bytes or absence. The shared server source-drop
helper for rename/unlink/rmdir prepares those comparisons through EXCL_VALUE;
unlink or name reuse before capture/PREPARE answers STALE, and prepared keys
continue to reject log deletion with BUSY. Split tombstones mask local copies
and cannot satisfy a live source. Regression tests reproduce old unsafe
acceptance and cover both race orders, ABA, split/hashed captures and a live
resolved destination. Full metadata/transaction tests pass normally and under
ASan/UBSan, simulator and strict local server syntax pass. Still owed: Linux
server/FUSE build and cluster `peer_rename_vs_unlink_src` 20/20, first on the
next rollout. No cluster rollout or artifact cleanup in this follow-up.



## W42 · df / efs-mgmt status report the 3-node capacity model on any node count (queue row 2a)

**Current state, reviewed Oct 7.** Both FUSE statfs and management status
use `efs_capacity_logical`. Final four-node NUC evidence reports 500 GiB
capacity from four 187.5 GiB quotas, with the FUSE total agreeing. The old
mechanical replacement steps below describe the original defect, not missing
implementation. The named fcstor capacity gate has not been established by
this review. The full-stripe protection question remains open: current PUT
returns OK with two ACKs and fewer than two quota failures, and its outer
retry loop returns immediately on OK, without rerouting a quota-rejected
third fragment. Trace and validate protection debt/repair before closing W42.
[Evidence and limits](../archive/queue-review-20261007-round2.md).


**W42 · `df` / `efs-mgmt status` report the 3-node capacity model on any node count** (Oct 1 2026, user). **IN TREE Oct 2:** `efs_capacity_logical` (placement.c, binary search on the Σ min(cᵢ, M) ≥ 3M bound), used by `efs_fuse_statfs` (total = quotas, avail = room, used = total − avail) and `efs-mgmt status`; `test_placement` covers 3 equal / 4 equal / 100/100/1000 → 200 / 6 equal / < 3 nodes → 0. Still to do: verify on 19810 (`df` vs `4 × 36T × 2/3`) and the one-QUOTA-member PUT question

**Follow-up steps.** mechanical: replace `total_logical = 2 × min_quota` (`efs_fuse_statfs`, `efs_fuse.c:3108–3120`) and `usable_cap = 2 × min_quota` / `usable_free = 2 × min_free` (`efs_mgmt.c:129–185`) with the 3-of-N placement bound: the largest `M` (chunks) with `Σ_i min(c_i, M) ≥ 3M`, `c_i` = node `i`'s quota (or free) in 64 KiB fragments, times 128 KiB; count only up nodes with a quota, as today. Reduces to `2 × min` on three nodes and to `Σ × 2/3` on N equal nodes. Also make `f_blocks` and the used figure come from the same model (statfs today derives used from `Σ phys × 2/3` and total from `2 × min`, so on four nodes used can exceed total and `avail` clamps to 0 while writes still succeed). Unit test with 3 equal, 4 equal, 3 unequal (100/100/1000 → 200, not 800). Then verify on 19810 (`df` vs `efs-mgmt status` vs `4 × 36T × 2/3`)

**Evidence and limits.** Both comments say "every chunk places one fragment on each node" — true for three nodes only. Four 500 GiB nodes show 1000 GiB instead of 1333. Not a data-path change; no decision needed. **Verify while there:** what a PUT does when exactly one stripe member answers `EFS_ERR_QUOTA` (`put_fragments_parallel_once`: `quota_errors >= 2` → QUOTA, `acks >= 2` → OK) — if the chunk publishes with two fragments, a full node creates protection debt silently ([product-gaps](product-gaps.md) §1.2); if `reroute_down_fragments` moves it, say so in [the failure-tolerance table](../how-it-works/failure-tolerance.md)


## W27 · REPORT identity from the staging table (queue row 0b)

**Oct 6 drain follow-up:** `febc55e5` distinguishes a phantom span-only
staging-row mark from actual local ownership before requeuing a missing PUT
identity. The old zero-node branch requeued indefinitely and blocked clean
stop even after all data tests passed. Both report construction paths now
retain dirty/stalled/pinned/unreported local cache work, but drop an ownership-
free mark. NUC full posix2 64/64 and both fresh client clean stops PASS. The
nonzero-node staging-identity fallback remains; this is not a complete W27 close.

**W27 · REPORT identity from the staging table**

**Follow-up steps.** (a) find the path that leaves a dirty chunk with neither a putid nor a dcache object (`write.c:1103`: putid table eviction, reclaim after `b6c1712d`'s pin release, or an irec-only threshold REPORT); one traced ecopy of a small-file tree on an idle cluster; (b) rerun on the current client first — if `putid miss` is 0 there, record and close; (c) only if (a) names the cause: a chunk with no PUT of ours is not ours to publish (keep dirty, replay from the row), as the `fragment_nodes[0] == 0` branch already does for span-only rows

**Evidence and limits.** ≥ 9016 recs (`n=8812…9016` in the last rate-limited second, all `ci=0`) reported with a mapping that "may be the server's row, not this client's PUT" — the Sep 30 (gen, off, len) / conflated-table class that lost ior-hard records. Forbidden: silencing the line, or "committing" such a rec client-side


## W43 · truncate/O_TRUNC of a file with > 32 chunks in a lane is a silent no-op (queue row 0g)

**Current checkpoint (Oct 7):** the original silent-success bug is historical.
Explicit failure is implemented; `3a1b4a52` also makes oversized legacy truncate
reject atomically, preserving all fragment references on capacity failure
(`tests/test_meta_apply.c` covers the refusal). D25 logical resize/public FUSE
activation and the named live acceptance gates remain open. The heading is
retained for existing links; the following dated observations describe earlier
builds, not a new finding that current truncate silently succeeds.

**W43 · `truncate`/`O_TRUNC` of a file with > 32 chunks in a lane is a silent no-op; the apply answers OK on NOMEM** (Oct 1 22:00Z review, §1b). **Steps b and c IN TREE Oct 2** (the truncate now FAILS with EIO instead of lying; `tests/stress/truncate_big.sh`); step a is **D25, decided and revised Oct 2** (logical truncation + background reclamation), step d follows it

**Follow-up steps.** (a) **D25 (decided):** the fence entry sets epoch + size, the reaper reclaims; (b) mechanical regardless of D25: `apply_truncate_cmd` / `apply_lane_fence_cmd` put the apply's rc on the ring instead of `EFS_OK`, so SETATTR fails (EIO/EBUSY, W16 mapping) rather than lying; (c) repro + gate: `dd bs=1M count=10 conv=fsync` of a non-zero source onto an existing 1 GiB file, then `stat` (size 10 MiB), `md5sum` (the new bytes), `efs-mgmt raft-getchunks` on chunk 100 (gone); same with a 300 MiB file (every lane > 32 chunks) and with different content; add it to posix (`truncate_big_*`) and posix_persist; (d) then the apply drains per D25 and `apply truncate rc=` never appears in `efsd.log` during the 16× dd

**Evidence and limits.** servers `efsd.log` 20:56:41–43: `apply truncate rc=-2` ×16 inodes on every replica, `apply lane-fence rc=-2` ×661; client `slow-ok type=63 … status=0`; all 34 REPORTs `skip=8192 push_ms=0`. `TRUNC_IT_CAP` = 64 + 1 + 64×32×2 + 3, `efs_meta_apply_lane_fence` `it[1+32+32]`; `trunc_del_cb` → NOMEM at the 33rd chunk of a lane. Forbidden: raising the cap (a 1 TB file is 8192 chunks per lane); deleting chunk rows from the handler thread outside the entry; returning OK for an apply that wrote nothing



**Plan row 1 (in tree).** `apply_truncate_cmd` / `apply_lane_fence_cmd` return the apply's rc as the ring verdict (both on `host_apply`'s ring-only list, so a failed apply never halts the log); SETATTR surfaces EIO (W16 mapping) Gate: `tests/stress/truncate_big.sh` exits 3 (`TRUNC_ERR`, file unchanged) until D25, never 1 (a lie).

**Plan row 3 (in tree).** repro + gate for big-file truncate: `dd bs=1M count=10 conv=fsync` onto an existing 1 GiB and a 300 MiB file, same and different content, plus `truncate -s 0`; `stat`, `md5sum` through `iflag=direct`, `raft-getchunks` on chunk 100 Gate: exit 3 = truncate refused and file unchanged (today); exit 0 = all four PASS (after D25); exit 1 = a lie.

**Oct 6 note (nuc bare-metal cluster, posix `truncate_big_ftruncate_honest` / `truncate_big_o_trunc_honest`).** Two lane-spanning truncates in flight at once (the two tests at jobs ≥ 2) make the REFUSED file's subsequent reads fail with EIO for ~1 s while the lane settles; size and bytes stay intact and reads recover (solo runs are clean, verified 3/3 rounds). The tests are now `@serial` and `_verify_big_unchanged` retries reads through that window, so the lie gate is deterministic again. Whether the transient EIO is acceptable (vs EAGAIN/queued behind the in-flight truncate) is open — no bytes at risk, but an honest EIO on an intact file can still spook a reader that races a refused truncate.

## 0a · STALE replay that never converges (queue row 0a)

**STALE replay that never converges** (no W number; the earlier `W26` label here collided with the `fallocate` item)

**Follow-up steps.** (a) identify ino 116202 on 19810 from a KV copy and compare its chunk row with what the classifier (`write.c:880–968`) would replay; (b) repro: eight `dd bs=1M` into one mount, SIGINT mid-write, client stop, `EFS_DCACHE_TRACE=1`; (c) mechanical: the unmount drain names the inos and rc it abandons, and a `report-stale` round that replays the same single chunk > 16 times logs ino/ci/row gen/verdict once; (d) **DECIDED Oct 2 03:50Z = D27** ([decisions table](../status/decisions.md)): detect non-progress as repeated STALE against the *same* server generation (a gen advance is contention), stop the loop, keep the dirty bytes pinned, errseq-style EIO on `fsync`/`fdatasync`/`flush` of that inode, `client.sh stop` refused while a stalled rec exists, forced teardown reports ino/ci/off/len/cause per rec. Spill (Oct 1 23:00Z) and drop-after-N (Oct 2 01:45Z) were both rejected. **D28 (ask):** loss on forced teardown as written policy vs server-held write intents. Note: 2768 rounds prove a stalled operation, not a content mismatch — (a) decides which

**Evidence and limits.** fstor007 Oct 1 00:05: 2768 rounds of `report-stale: chunks=1 … committed=0 replayed=1` and then `UNMOUNT DATA LOSS … rc=-14 after 60s`. Acknowledged writes were discarded; the log cannot say whose. Forbidden: widening the 60 s drain, dropping the STALE check, or publishing a rec the server rejected


## W44 · the group leader's GC frag pass scans the whole prefix every 1.2 s (queue row 0h)

**W44 · the group leader's GC frag pass scans the whole prefix every 1.2 s and is 80 % of the leader's `efsd` cycles** (Oct 1 22:00Z review, §1b). **Step a IN TREE Oct 2** (`gc-pass … fsegs= fkeys= ftomb=`); **D26 implemented Oct 4 (dev cluster)** — the per-anchor pending-GC watermark gates the scan (a 5120-record `rm` drained at ~514 records/pass, then no `gc-pass` line for 10 idle min; the recovery derive consumed the table's 19664 GC-prefix tombstones once at startup); the idle-hour reading on the live table is owed to 19810 (down), as is the raft-tail 99.7 % GC_ACK check

**Follow-up steps.** (a) count what one `host_gc_frag_pass` scan visits (`EFS_GC_DBG`, plus a per-scan key/segment counter on the `gc-pass` line) on the live table while idle; (b) if the 205 ms empty scan is the 50–54 L0 segments, that is D12/D13's file count — bring the number to the user (**D26**); if it is tombstones under the GC prefix, the fix is a per-anchor "GC records pending" watermark the apply maintains so an empty pass costs one get; (c) gate: idle leaders show no `gc-pass` line (> 5 ms) for 10 min, `md_latency.py` medians unchanged, a 10 GiB `rm` still drains at ≥ today's 140 records/s per group

**Evidence and limits.** fcstor003 `perf-pid.txt`: tid 1056219 79.7 % of 780 K samples, flat `__memcmp_avx2_movbe` 24.9 % + `merge_scan` 16.7 % + `kv_seg_iter_next` 4.1 % + `kv_msrc_advance` 2.8 %; `gc-pass ms=205 frag=205 reap=0` every 1.2 s from 20:11 to 20:56 with nothing to collect; `kv-compact: end … l0=54 l1=149` once per 7 min. Forbidden: a longer `GC_LOOP_MS` to hide it (the rm drain rate is already 78 min per 160 GiB); scanning from a handler thread


## 0m · parent directory mtime/ctime must bump on entry create/unlink/rename/link (queue row 0m)

**Acceptance update, Oct 7.** Final NUC raw TSVs show all seven directory-time
tests and `peer_dir_mtime_bump_visible` passing on `128f6b7d`; this supersedes
the 6/7 single-client and peer-gate-pending statements below. The row is archived
as accepted on that build. [Raw evidence](../archive/queue-review-20261007-round2.md).


**Parent directory mtime/ctime must bump on entry create/unlink/rename/link/mkdir/rmdir — POSIX, and ruled a bug if missing (user, Oct 6 2026: "EFS is as much as possible POSIX compliant").** The open "bug vs. intended" question is closed: intended = POSIX.

**Status (Oct 6).** Code audit: the apply implements the bump for all six entry ops — create (`efs_meta_apply_create_file_op`), mkdir, unlink, link, rename (src and dst parents via `stamp_dir_items`), rmdir. A LOCAL directory's times ride the parent row in the same atomic batch; a HASHED directory's live in the dentry shard's dir lane (`dir_lane_stamp`, §7.4); `efs_meta_apply_getattr` reduces the `used_shards` lanes for a spread directory. **Unverified end-to-end:** client-side visibility (the getattr path, dcache, the 0j memo window) — the tests below are the arbiter, and a failure is a bug. **Gate ran Oct 6 (nuc bare-metal 3-node loopback cluster):** create/unlink/mkdir/rmdir/link and same-dir rename all bump same-client (posix `dir_times_*` 6/7 after the test-wait fix below); the only failure is `dir_times_rename`'s cross-dir dst parent — localized to the renaming client's attr invalidation and filed as **W57**. Note the Oct 5 session's `dir_times_bump_on_child_mutation` (posix) was added as a *failing* gate on the Oct 5 build — since the apply is verified correct, a same-client failure localizes the bug to the client's directory attr path (0j-adjacent).

**Tests in tree Oct 6.** posix: `dir_times_create`, `dir_times_unlink`, `dir_times_mkdir_rmdir`, `dir_times_rename` (same-dir and cross-dir, both parents), `dir_times_link`, `dir_times_write_no_bump` (the negative: content writes never touch the directory). posix2: `peer_dir_mtime_bump_visible` (A creates, B sees the directory's mtime+ctime advance). Every re-stat waits a full wall-clock second (the original 0.06 s — meant only to sit outside the 50 ms lookup-memo window, 0j — could not see a legitimate bump: creation rows show whole-second granularity, and on a fast loopback cluster the mutation lands in the same second as the baseline; 1 s crosses a boundary under any client/server clock offset).

**Follow-up steps.** run both suites on a live cluster; if a `dir_times_*` test fails, the failing op's stamp path (above) is the suspect; if only the posix2 test fails, look at the peer's getattr reduction or the client's directory attr caching. The spread-directory case (≥ `EFS_DIR_SPREAD_MIN` = 65536 entries) is not covered by these tests — add it when a spread-dir fixture exists.

**Forbidden.** declaring the bump "intended to be absent" to avoid the cross-shard stamp — §7.4 already solved that with dir lanes.


## W57 · cross-directory rename never refreshes the dst parent's attrs on the renaming client (queue row 0n)

**Oct 6 fix:** `f8fef814` refreshes both parents after committed rename, updates cached directory attrs from authoritative rows, and prevents dirty namespace state from overlaying stale directory attrs. The existing directory LOOKUP shortcut remains. NUC full POSIX jobs=4/jobs=1 and full posix2 PASS, including cross-directory parent-time phases.

**W57 · after any cross-directory rename (`mv a/f b/f`), the renaming FUSE client keeps serving the DST parent directory's pre-rename attributes indefinitely — mtime/ctime still show the pre-rename value ≥ 25 s later, a readdir of the dst parent does not refresh them, only another mount shows the truth — while the server stamps BOTH parents correctly (verified from a second mount on the same cluster: ns-resolution bumps). With a SIBLING layout (`a/f → b/f`, no shared ancestors) the SRC parent goes stale as well; with a parent→child layout (`d/f → d/sub/f`) the src parent survives because it is an ancestor of the dst path and gets refreshed along it (Oct 6 2026, nuc bare-metal 3-node loopback cluster, build `4e4c10ff-dirty`). This is the same-client failure 0m predicted would localize to the client's directory attr path.**

**Follow-up steps.** (a) fix: the rename reply/local-apply path must invalidate (or restamp) the client's attr state for BOTH parent inos, including when the dst parent is not on the source path — today only the components along the two rename paths are refreshed and the dst parent's dir-lane-reduced GETATTR row is never re-fetched; (b) audit `link(2)` into an already-statted directory for the same gap (`dir_times_link` passes, so likely clean — confirm in code); (c) gate: IN TREE Oct 6 — `tests/posix/posix_suite.py` `dir_times_rename` cross-dir phases (parent→child dst-parent check and sibling src+dst checks, each with a 0.1 s post-rename wait to sit outside the 50 ms rename-reply memo; both FAIL on the Oct 6 build) — then full posix jobs=4 and jobs=1 after the fix

**Evidence and limits.** nuc bare-metal cluster (3× efsd on loopback, efs-fuse mount): `dir_times_rename` fails on the dst-parent check with identical before/after ns values; a second mount on the same cluster shows the server bumping BOTH parents at ns resolution (e.g. `:55.641246728`) while the renaming mount still shows the pre-rename whole-second row 25 s later; a sibling-layout probe shows the src parent ALSO stale at +0.15 s and +2 s. Not the 0j 50 ms memo (persists ≥ 25 s); not `attr_timeout` (0 per 0j). Same-dir rename and the create/unlink/mkdir/rmdir/link bumps are all visible same-client (`dir_times_*` pass once the test waits cross a wall-clock second — creation rows were observed at whole-second granularity, so the old 0.06 s re-stat wait could not see a legitimate bump and failed spuriously on a fast cluster), i.e. the invalidation gap is specific to the rename's implicit dst parent (and a non-ancestor src parent). Forbidden: a time-based expiry that papers over the missed invalidation; routing every dir GETATTR through an extra RPC as the "fix" (0m's dir-lane reduction already makes the fresh answer cheap — the client just has to ask)


## W58 · open(O_EXCL) create answered EEXIST for a name the same client's own create just landed (queue row 0o)

**`open("xb")` on a never-before-used name raised `FileExistsError`** (user, Oct 6 2026, xorinox cluster, build `3d3f17c2-dirty`): `tiny_files.py --depth 4 --files-per-folder 100 --total 1000000 --workers 16 /mnt/efs/tiny_files7/` died at `folder_000231/…/file_000049.txt`.

**Analysis (Oct 6, evidence on the cluster).** The script is innocent by construction: every leaf path is namespaced by a unique `folder_index`, each leaf is written by exactly one task, each file index once — no duplicate path is generatable, and `open("xb")` failed at file 50 of 100 in that leaf. The filesystem state contradicts the verdict: `file_000049.txt` **exists** (ino 623812, created 05:17:31.319Z, **size 0** — the create landed, the write never happened because the caller got an error). Server logs: xefs1 `05:17:31.733Z raft-host: create parent=419012 name=file_000049.txt rc=-13` (BUSY) — after the successful apply; **no server ever logged rc=-17 (EEXIST) for anything in the run**. Client log: `05:17:32.459Z inode-rpc: slow-ok type=67 attempts=2 saw_busy=1 status=0 us=1173252` ×4 — concurrent creates each taking >1 s through BUSY. Chain: the create applied (05:17:31.319), a retry saw BUSY (.733), and the application ultimately received EEXIST — a verdict the server never logged, so it was either fabricated client-side or returned by a retry whose opid no longer matched its own recorded verdict (I16: a replay in the window returns the recorded verdict). BUSY on *unique-name* creates is itself new behavior on this build — the uncommitted D25 intent probes make plain creates contend.

**Follow-up steps.** (a) in the client create path, check that every retry of one logical create carries the *same* opid and that a post-BUSY retry re-probes the opid window before falling through to the name-exists check; (b) decide where the EEXIST was born — server name check on a new opid, or a client-side lookup fallback after an ambiguous verdict; (c) repro is cheap: rerun the same tiny_files command into a fresh dir under 16 workers — recurred within 23k files on Oct 6; (d) a posix2 or stress gate: parallel `open(O_CREAT|O_EXCL)` of unique names must never yield EEXIST.

**Forbidden.** "Fixing" it by having the client swallow EEXIST on create retries (that hides real EEXIST for genuinely existing names); treating BUSY as terminal.


## W59 · write(2) via FUSE fails ENOSPC with 156 GiB free — client cache-admission mapped to ENOSPC; the 8 MiB metadata budget never drains (queue row 0p)

**W59 · `dd bs=1M count=1024 conv=fsync` on a FUSE mount died on the FIRST write with `No space left on device` (0 bytes) while the export showed 156 GiB free and every node disk 85 GiB free (Oct 6 2026, xorinox cluster, xefsct1). Not a capacity problem: the client maps its internal write-cache budget exhaustion to ENOSPC, and one of the two budgets — the fixed 8 MiB dcache metadata pool — never drains, so once it pins at its cap every subsequent write on that mount fails ENOSPC until remount.**

**Local implementation (Oct 6, uncommitted; no deployment).** Admission now logs metadata live/reserved/cap/request plus the failing budget leg before reclaim. Under metadata pressure, scan linked heap entries under their shard locks and free only body-less, published entries eligible for reuse; stalled records, uncommitted object/sequence identities, pins, dirty-list members, reclaim claims and present-extra accounting remain protected. The existing 8 MiB cap stays enforced. Retry after read-cache and metadata trim, after each successful REPORT drain, and with eight 100 ms backoff waits for concurrent reservations/REPORTs to release capacity. Exhausted local admission returns EAGAIN; allocation failure returns ENOMEM. Local allocator admission now returns BUSY rather than QUOTA, separating it from backend verdicts; genuine backend QUOTA, including during pressure drain, remains ENOSPC. Code review corrects the original suggestion to remap every flush-returned QUOTA: the drain does not call request reservation, and its QUOTA originates from backend PUT/inode RPC verdicts. This bounds the additional admission backoff, not the duration of a blocking REPORT RPC.

**Validation.** The actual allocator/cache/FUSE admission harness saturates metadata at 8 MiB with zero live body bytes, reproduces rejected admission, then proves admission succeeds and metadata falls to precisely the two protected unresolved/stalled entries. Removing their protection releases the remaining charge. It also checks local congestion returns EAGAIN and genuine pressure-drain QUOTA remains ENOSPC without discarding accepted bytes. Local memory gates and ASan/UBSan pass, as do D27 runtime/fault/STALE, controlled-stop, fold, fence-view and REPORT-pressure regressions. Live gates remain open: rebuilt client process/remount, multi-GiB sequential writes followed by more writes on the same mount, and posix jobs=1. Do not close W59 from local tests alone.

**Follow-up ENOMEM (Oct 6, xefsct1; local fix uncommitted).** User deployed the first fix and `dd bs=1M count=10024 conv=fsync` failed after 251 MiB. Read-only inspection confirms the new `546f8647-dirty` client process, ~2 GiB available host RAM and `write_buf` failures without a preceding admission-pressure log; subsequent close/flush succeeds. The actual allocator regression reproduces a reservation violation: reserve the normal budget, allocate flush scratch from the drain reserve, then the admitted writer's fully credited allocation fails because allocation rechecks total live+reserved against the normal cap. Fully credited allocations now use the already enforced combined hard+drain bound; uncredited/partially credited ordinary allocations still use the normal bound. Neither configured bound is raised. The regression fills both budgets, proves the reserved allocations succeed and the next unreserved allocation fails, then verifies complete release. It fails before the fix and passes afterward; local memory and D27 recovery suites plus allocator/cache ASan/UBSan pass. `write_buf` failure logging now uses POSIX strerror/errno, correcting its former interpretation of `-ENOMEM` as EFS `NOTEMPTY`. Live causation is consistent with this reproduced race, but the same deployed dd and subsequent writes remain required to close the gate.

**Follow-up steps.** (a) **log first, then fix:** the `dcache-pressure` line (`efs_fuse.c:2799`) prints only body counters (live/reserved/backing/limit/request) — add `g_metadata`/`g_meta_reserved` and which admission leg failed, so the exhausted resource is visible on the next occurrence; (b) **errno semantics:** a server QUOTA verdict (cluster genuinely full) → ENOSPC; a client-local admission failure → block with bounded backoff, worst case ENOMEM/EAGAIN — never ENOSPC (the two sites: `efs_fuse.c:2815` flush-returned-QUOTA and `:2824` drains-exhausted); POSIX apps (dd, rsync, git) treat ENOSPC as fatal-full and abort a transfer that could have proceeded; (c) **metadata reclaim:** body-less dcache entries are kept per published chunk for report/CAS base (`write.c` `dcache_find_meta`; `dcache_keep_on_drop` blocks dropping the unreported) and chain nodes stay charged when reused ("Heap nodes remain charged when reused", `bufpool.c`), so `g_metadata` grows to a peak and never shrinks — release or evict body-less entries once their report has landed (cap per-slot chains; reclaim reported-clean entries in the pressure path), so the 8 MiB cap cannot pin; (d) **relation to mem1** (sparse writes bypass reclaim; admission hard bound — in tree): mem1 bounds the body side; W59's metadata cap is the remaining leg — confirm the mem1 implementation does not already reclaim these entries; (e) **gate:** repro loop — a multi-GiB sequential dd through one FUSE mount (thousands of 128 KiB chunk entries), then keep writing while `df` shows free space: after the fix no ENOSPC; plus posix jobs=1 regression

**Evidence and limits.** xorinox 3-node libvirt cluster, xefsct1 FUSE mount, Oct 6 2026. Symptom: `dd if=/dev/urandom of=/mnt/efs/002.dat bs=1M count=1024 status=progress conv=fsync` → `No space left on device`, 0+0 records. Capacity checks all green: `df -h /mnt/efs` = 200G total / 45G used / 156G avail; `/data1` on xefs1-3 = 112G with 85G avail each, inodes 2%; no `nospc` in any efsd log. Client log `/mnt/efs-fuse-efs.log` shows two regimes. **(1) 06:16–06:18Z, genuine transient pressure:** `dcache-pressure live=266993664 reserved=0 backing=301989888 limit=335544320 request=1572864` in a minutes-long storm — live pinned at 254.6 MiB so `g_live + g_reserved > g_hard − bytes` (266862592), failing the byte leg (`bufpool.c:93`) — while `inode-rpc: slow-ok type=67` (= `EFS_MSG_REPORT_CHUNKS`) took 5.2 s with `saw_busy=1`: the server answered REPORT with BUSY, drains could not keep up, the 16 pressure-drain rounds were insufficient → `-ENOSPC`. Real congestion, wrong errno. **(2) 06:26Z, the dd, permanent failure:** `dcache-pressure live=0 reserved=0 backing=301989888 limit=335544320 request=5242880` — cache EMPTY, yet a 5 MiB admission fails. The byte leg provably passes (0 ≤ 256 MiB − 5 MiB); per-thread credits are clean (reserved=0); the only remaining leg is metadata (`g_metadata + g_meta_reserved ≤ 8 MiB − metadata`, `bufpool.c:94-95`) → `g_metadata` pinned at the 8 MiB cap. backing=301989888 is NOT a leak: 9 warm slabs × SLAB_BYTES (256 × 128 KiB = 32 MiB) that stay registered for process lifetime by design, and slab bytes do not count against admission. Arithmetic checks out: chunk size 128 KiB → a 1 MiB dd write = 8 chunks + 2 guard = 10 → request `10 × 4 × 128 KiB = 5242880`, metadata `10 × 1024`; the drain loop finds no dirty ino (live=0, `efs_dcache_pressure_ino` → 0, break) → `-ENOSPC` at `efs_fuse.c:2824`. Persistence: identical failures minutes apart on an idle cluster — a pinned counter, not congestion. The mount had accumulated entries over its lifetime (≈45 GiB written into the export earlier; posix 2client runs on xefsct1/2). Red herring: the user's first `dd bs=1m` failed on coreutils argument parsing (lowercase `m`), unrelated. Forbidden: returning ENOSPC for any client-local budget condition; raising the 8 MiB cap (or `EFS_DCACHE_HARD_BYTES`) as "the fix" without metadata reclaim; a time-based expiry that papers over the missing release

**Confirmation (Oct 6, nuc bare-metal cluster — second independent site, user-reported).** posix suite `/data1/efs/logs/posix-20261006-023609.tsv` (mnt=/data1/efs/mnt): **183/217 FAIL, 180 of them `Errno 28`**, starting with the very first write test (`basic_write_read`); `basic_empty_file` (no write) passes. The export is essentially EMPTY (`df -h /data1/efs/mnt` = 200G total, 4.2M used) and the data disk has 731 GiB free. Fuse log `/data1/efs/efs-fuse-mnt.log` (413 `dcache-pressure` lines) replays both regimes: 04:06–04:10Z genuine byte pressure (live 247–263 MiB during heavy write tests); the 05:03Z and 06:05Z suites still pass 214/217 with zero ENOSPC; from **06:18:15Z** the signature flips to `live=2883584 reserved=0 request=1572864` — 2.75 MiB live, the byte leg trivially passes, only the metadata leg can fail — and the 06:18Z suite fails 172 tests ENOSPC, the 06:36Z suite 180. Once pinned it never recovers (two suites 18 min apart, idle cluster). Rules out anything specific to the libvirt VMs or to a filled export: the budget accumulates over the mount's lifetime across suite runs, then bricks writes permanently.


## W60 · a concurrent sequential reader's prefetch queue can park the entire read-side body budget; demand reads then fail NOMEM, surfaced as EIO (queue row 0q)

**W60 · `rg --hidden --no-ignore --stats 'search text' /mnt/efs/` on xefsct1 (Oct 6 2026, xorinox cluster, build `b4a75492-dirty`) emitted thousands of `Input/output error (os error 5)` on tiny files (`tiny_files*/folder_*/level_4/*.txt`) while all three efsd logged zero errors. The client log holds 19,112 `efs-fuse read: out of memory (efs_rc=-2)` lines between 21:58:16Z and 22:57:01Z (per-10-min: 950, 6433, 4879, 2085, 1665, 1704, 1396 — onset ~40 min into the scan, decay as rg moved off the large files). The EIO is `efs_fuse_read_ino` mapping any `efs_client_read` error to -EIO while the log keeps the real code (`efs_fuse.c:2166-2169`). Root cause established by controlled A/B reproduction (below): queued prefetch buffers can hold the full 256 MiB read-side body budget, and the demand-read scratch allocation has no trim/wait/retry — best-effort readahead starves real reads.**

**Mechanism (code).** The shared body pool (`bufpool.c`) admits demand reads only up to `EFS_DCACHE_HARD_BYTES` (default 256 MiB; the 64 MiB drain reserve is write/drain-only — `efs_buf_alloc` uses `g_hard` without a full reservation or drain context) and charges every buffer a minimum of one chunk (`buf_charge`, 128 KiB — a 3-byte file costs a full slot). A sequential reader arms prefetch after two in-order reads (`t_seq_run >= 2`, `read.c:1725-1732`); `prefetch_ahead` (`read.c:1508-1553`) allocates one pool buffer per queued chunk (`:1536`) into the GET pool, which queues up to `GET_POOL_QDEPTH=32` × `GET_POOL_N=64` (`:1194-1204`) = 2048 jobs ≈ 256 MiB — 100% of the read budget. Prefetch's own alloc failure is silent (`:1537-1539`). Any non-chunk-aligned read (every sub-chunk tiny file, every tail) needs a scratch buffer at `read.c:1797` and fails `EFS_ERR_NOMEM` (`:1802`) with no recovery — unlike the write path, which trims the read cache, drains and retries (`fuse_write_admit`, `efs_fuse.c:2900-2978`, the only `efs_rdcache_trim` caller). The log line blames the victim; the culprit is invisible.

**Reproduction (controlled A/B, Oct 6, xefsct1).** Throwaway second mount of the same cluster with `EFS_DCACHE_HARD_BYTES=33554432 EFS_DCACHE_DRAIN_BYTES=33554432` (64 MiB total = 512 chunk buffers): tiny-file loop alone (`head -c 4` × 3000 files of `tiny_files9`) → **0/3000 failed**; same loop with `cat 001.dat > /dev/null` (10 GiB sequential) concurrently → **2000/2000 failed**, first failure on file #1, 2000 NOMEM lines. Ruled out by measurement: kernel OOM (none in the window), staging-table cap (no `EFS_CLIENT_META_MB` line), rdcache body accumulation (reads leave the pool empty when no sequential reader runs), chunk-size geometry (128 KiB, confirmed by the `request=5242880` admission arithmetic). Full write-up with evidence: [fuse-memory.md](../status/fuse-memory.md) §"Read-path ENOMEM storm".

**Follow-up steps.** (a) read-path backpressure: on scratch-alloc failure, trim clean unpinned rdcache bodies and wait briefly (bounded), mirroring `fuse_write_admit`, instead of failing the read; (b) prefetch admission watermark: best-effort prefetch must never consume the last of the budget — skip submission when live+reserved exceeds a fraction of `g_hard`, and/or cap queued prefetch bytes well below the demand budget; (c) distinct log lines for prefetch-drop vs demand-read failure (the single `read: out of memory` line hides the pressure direction); (d) consider sub-chunk body charging so tiny-file reads do not pay a full 128 KiB slot; (e) gate: the 64 MiB A/B must flip to 0/2000 with the sequential reader running, and a full-root `rg` over the xorinox tree (four ~10 GiB `00*.dat` + `tiny_files2..9`) must complete with zero `efs_rc=-2` lines. Cousin of W59's errno semantics: W59 maps client-local write admission to ENOSPC, W60 maps read admission to EIO — both should be backpressure, not terminal errors.

**Forbidden.** Raising `EFS_DCACHE_HARD_BYTES` (or shrinking the GET queue) as "the fix" — that moves the onset, the starvation remains; disabling or serializing prefetch (it carries sequential throughput — DIO arrives as 128 KiB requests, see the Oct 1 read review); an unbounded retry spin on the demand path.


**Implementation and NUC gate (Oct 6).** `73ce8aaa` adds atomic half-budget
speculative/cache-body admission and clean-cache trim plus bounded demand
retry. Same four-node NUC cluster, same 32 MiB normal + 32 MiB drain budget,
same 512 MiB sequential fixture and 2000 tiny files: pre-fix read/allocator
code **2000/2000 failed**, fixed code **0/2000 failed**, zero read-NOMEM log
lines. Concurrent admission, credit/drain protection and pending/pin ownership
regressions pass. Xorinox full-tree acceptance and optional diagnostics/
sub-chunk charging remain follow-ups.

#### Plan after the Oct 1 22:00Z review — what runs without a decision, what is asked

Tie-break for every row, in this order: **no lost or misreported bytes
→ fewest surprises for a user → speed.** Effort: quick = hours, medium
= 1–2 days plus gate, long = days or a wipe. Source: the §1b 22:00Z
block and `~/orcd/scratch/efs/perf/efs-mount/server-2056/`.

**Runs without the user (take in this order; one cluster roll covers
the quick ones).** Each row is a queue item; the W number is binding.

## W48 · four of the 16 dd streams ended early — INVESTIGATE (plan row 12)

**W48 · four of the 16 dd streams ended early — INVESTIGATE (Oct 2 05:30Z; not an implementation)**

**What.** nodeids 0x36f2e / 0x3df2e / 0x41f2e / 0x3cf2e stopped at 5234 / 4498 / 4151 / 3670 MiB between 04:04:17 and 04:04:48Z with a normal FLUSH and no error reply in the FUSE trace (`results/measure/20261002-040242-dd16x10g-review/client/ana-fuse-early-stop.txt`)

**Effort.** quick

**Gate.** the four dd's exit status, signal, stderr and byte counts from the user's harness dir; if EIO/ENOSPC, the server log at that second and the client `inode-rpc` lines name the cause — W42 is a candidate, not a conclusion; if a timeout/kill, close the item


## W52 · a REPORT after thousands of O_APPEND writes answers after > 30 s (plan row 15)

**Implementation check, Oct 7.** Approved shape A is still pending:
`host_resolve_caught_up()` loops over each eligible reservation and calls
`host_propose_wait()` separately. The ordinary append POSIX/concurrency
passes do not prove the agreed one-batch proposal gate. No batching
implementation or new acceptance is claimed by this review.


**W52 · a REPORT after thousands of O_APPEND writes answers after > 30 s — NEW Oct 2 13:45Z (both builds); DECIDED Oct 5 2026 (user): fix shape A — one batched proposal for all caught-up reservations before the reply**

**What.** `host_resolve_caught_up` (raft_host.c, after the `report-split` line, before `set_inode_rc`) runs one serial `host_propose_wait` per OPEN reservation at/below the published size — N appends = N proposals before the reply. Client: `inode-rpc: retry type=67 why=recv rc=-6 recv_ms=30312`, byte-identical resend, server `skip=all`, dd `fsync: I/O error`; the published size lags (completed prefix) then converges. ~5 ms per O_APPEND write. fcstor004 `report-split nrec=1133` 06:03:30Z then `skip=1133` 06:04:01Z (old build); `nrec=625` ×3 at 06:06:45/07:15/07:46Z (P1 build) — `results/measure/20261002-060052-p1-d23-w41/SUMMARY.txt` §5

**Effort.** medium

**Gate.** the shape question was brought to the user Oct 5 2026 — decided: **shape A, batch all caught-up reservations into one raft proposal before the REPORT reply** (not per-reservation serial `host_propose_wait`, not resolve-after-reply); gate = `tests/measure/append_gate` 20 000 × 4 KiB O_APPEND then fsync returns 0 and the size is exact. Forbidden: widening the client's REPORT timeout


## W53 · W41's create/close-storm tail under concurrent big writers — INVESTIGATE (plan row 16)

**W53 · W41's create/close-storm tail under concurrent big writers — INVESTIGATE, then user decides keep/revert (Oct 2 13:45Z)**

**What.** same host fcstor010, old → P1 build: 4 × 8 GiB dd wall 9.70 → 8.30/8.37 s, storm p50 4.9 → 3.8/5.5 ms, **p99 37.2 → 100.7/121.6 ms, max 52.8 → 277/217 ms**; the P1.2 gate said p99 ≤ 51 ms. fcstor008 passes 2–5: p99 66–128 ms (`…p1-d23-w41/SUMMARY.txt` §4, `wedge-*-fcstor010*.txt`)

**Effort.** quick

**Gate.** per slow close: time in its own REPORT RPC vs in a client lock (`EFS_DCACHE_TRACE=1` `report` lines + a server `report-split` for the small inode); if the RPC is the whole wait, it is the small proposal queued behind the pool's 8192-record batch applies on the server (then the remedy is D30 territory or a smaller D24 batch — ask); if client-side, name the lock. Not a fix without the table


## W50 · the repeating GC record set — CLOSED (plan row 13)

**Performance-plan row P2.1 — CLOSED.** Status: **CLOSED 14:57Z** — not stuck; `…w50-gcdbg/SUMMARY.txt` What (as run): one `EFS_GC_DBG=1` pass: identities, delete verdicts, ACK flush rc Result: table: all del/flush rc=0, 0 consecutive-pass repeats; `ex=(nil)` skip documented separately Forbidden: naming a cause from counts.

**W50 · the repeating GC record set — INVESTIGATE (Oct 2 05:30Z)**

**What.** `gc-frag group=0 scans=1 records=126 ms=428` identical for minutes on the group-0 leader after the dd (`fcstor003/ana-log.txt`)

**Effort.** quick

**Gate.** one pass with `EFS_GC_DBG=1`: the 126 record identities, each fragment delete's verdict per node, and whether a GC_ACK for them committed; identical counts alone prove nothing — only a record seen in two passes with a non-OK delete verdict and no ack is "stuck"


## W51 · what the follower apply lag is made of — CLOSED (plan row 14)

**Performance-plan row P2.4 — CLOSED.** Status: **DONE 14:38Z** — table only; `…w51/SUMMARY.txt` What (as run): 144 apply-sleep episodes from the P0.2 file Result: compact-overlap 47, small-gap 89, lag-gap 0; D30 still ask Forbidden: a remedy before the table.

**W51 · what the follower apply lag is made of — INVESTIGATE (Oct 2 05:30Z)**

**What.** `apply-sleep` 20–400 ms and `fail=wait/-13` REPORTs on fcstor004/005 during the 16× write while `kv-compact` ran 3–5 s merges

**Effort.** medium

**Gate.** on the private cluster or 19810 with `--perf --strace` on one follower: per `apply-sleep` episode, was the pump blocked on `l->mu` / the compactor (lock blocking), inside `efs_meta_apply_*` (slow application), or idle waiting for AppendEntries (transport)? One table, one episode class per row; D30 is decided only on that table. Raw material so far: `p0-x16/apply-sleep-compact-gc.txt` (144 episodes, 20–34 ms), fcstor006 `apply_max` 100–160 ms during the 8192-record REPORT applies 06:01–06:08Z, the 12:54Z `l0=143` compaction storm (P1 gate pass 4, dd fsync tails 5.4–6.4 s)


## Single-node storage bench `efsd --bench` — ASKED Oct 2 2026 (user). Queue position: after W41 / D23 / D17 / D26 in "Plan after the Oct 1 22:00Z review"; its number decides the fragment layout, W40 and zero-copy receive.

**CLI relocation (Oct 6 2026).** Local `data|meta` modes moved to
`efs-bench --bench`, reusing the same storage backends without daemon/network
startup. `efsd --bench` is removed. The original Oct 5 results below keep their
historical command labels; future measurements use `efs-bench`. See
[local usage](../how-it-works/performance.md#local-storage-benchmarks).

**Status (Oct 5 2026).** **Tool in tree + gated (dev cluster): 16 unit suites PASS.** `efsd --bench data|meta` as specified below (fio ceilings in the same log, store + writer pool exactly as the handlers drive them, paths × QD 1/16/64/256 ladders with exact p50/p99, iostats cross-check, diskstats util, `--perf` top symbols; the old pwrite loop is deleted). First numbers measured on efs1 (dev VM, one virtio disk — not an fcstor node): `results/measure/20261005-045140-p3-benches/SUMMARY.txt`. Write ≈ 82 % of the single-disk fio ceiling at QD16 with the disk at 82–99 % util (the engine reaches the disk's limit on this host); meta sits exactly on the fsync wall (27 puts/s QD1 = fio wsync4k, batch32 ×32). **Owed:** the 1→6 real-NVMe curve on a named fcstor host — the dev VM's six roots share one device, so the path-scaling question the plan asks is not answered by this run.

**Current checkpoint (Oct 7):** production-backend tools are implemented;
corrected latency/window and metadata validation checkpoints are indexed in
[queue row P3](../status/README.md#1a-the-work-queue). The named fcstor six-NVMe
curve and two-host client ladder remain owed. The original design/evidence below
is retained as history: its old “what exists” pwrite-loop description and
“what to add” instructions are not a claim that the current tool is missing.
Use `efs-bench --bench` for local storage measurements.

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

## Client bench `efs-fuse --bench` — ASKED Oct 2 2026 (user); after `efsd --bench`.

**Status (Oct 5 2026).** **Tool in tree + gated (dev cluster).** `--bench cpu|put|write` as specified below, entry in `efs_fuse.c`'s main before `fuse_session_new`, production path only (per-chunk hashes, REPORT on fsync, non-zero payloads). First numbers on efs1 → dev cluster (TCP): `results/measure/20261005-045140-p3-benches/SUMMARY.txt`. cpu 2.85 GiB/s (2 vCPU saturate) ≫ put 0.26 GiB/s stored at QD1 (p50 458 µs) ≈ write — on this host the per-fragment round trip is everything and the pipeline adds nothing, per the reading key below. 64-file write level OOM-killed on the 2.8 GiB VM (resource limit, recorded). **Owed:** the two-host ladder (rule 6) and any fcstor-backed run; the dd level stays the fcstor number we have.

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

## Performance plan — PROPOSED Oct 2 2026 05:45Z, STARTED 05:33Z on the user's "implement performance plan"; P0 and P1 DONE 13:30Z, P2 next (status per row below)

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

Rows P0.1–P0.4 are DONE; one-line records are in [../archive/README.md](../archive/README.md).

**P1 — decided client items (no spec change; take in this order).**

Rows P1.1 and P1.2 are DONE (archive index); P1.3 (D29) is an ask — its row text is in [../status/decisions.md](../status/decisions.md).

**P2 — decided server items.**

Rows P2.1 and P2.4 are DONE (archive index); P2.2 and P2.3 follow as sections below; P2.5 (D30) is an ask — its row text is in [../status/decisions.md](../status/decisions.md).

## P2.2 · D26 — the GC pass (performance plan row)

**Item.** **D26** GC pass

**Status.** decided (shape); **cursor in tree, rolled 19:22Z; watermark landed Oct 4 (dev cluster).** Oct 4 gate: 16 unit suites PASS (`test_gc_watermark`: bump at `gc_queue`, lower on retire, replay no-op, derive folds inserts-during-unknown upward, `zero_if` rejects a stale expect); live on the dev cluster — 40 × 16 MiB written then deleted drained 5120 records at ~514 records per pass (2 scans × ~257), then both leaders printed no `gc-pass` line for 10 idle min (an empty pass is one peek; the startup derive scanned the table's 19664 GC-prefix tombstones once). Owed to 19810 (down): the idle-hour reading and the raft-tail GC_ACK share. 19:22–19:52Z profile (`results/measure/20261002-195156-servers-perf-idle`): with the cursor the tombstone walk is gone (`ftomb=2`) and the pass cost is the deletes — two 256-record scans per 200 ms budget, ~0.5 ms/record = 3 serial fragment deletes, ≤ ~400 records/s per group; a 100 GiB overwrite is ≥ 34 min of GC. The delete fan-out (parallel/remote-batched deletes) is a shape question for D26 (ii), not in the decided text

**What changes (server).** (a) per-anchor pending-GC watermark maintained in the apply (insert bumps, ack/removal lowers, re-derived at recovery from a prefix scan once) so a truly empty pass costs one get; (b) **bounded scan progress**: the frag pass resumes from a per-anchor cursor instead of restarting at the prefix head (`efs_kv_scan_from` + skip the inclusive start); (c) tombstone-aware emit already on the `gc-pass` line — prefix compact on `ftomb/fkeys` only if D26 (ii) is taken

**Gate / done when.** idle leaders: no `gc-pass` line > 5 ms for 10 min and leader `efsd` CPU < 5 % idle; `md_latency.py` medians unchanged; a 10 GiB `rm` still drains at ≥ 512 records per pass; the raft tail of an idle cluster is no longer 99.7 % GC_ACK (preflight's "idle" becomes true)

**Forbidden.** a longer `GC_LOOP_MS`; scanning from a handler thread; a watermark that is not updated in the same apply as the record


## P2.3 · W23 — the stalled-compactor test (performance plan row)

**Item.** **W23** stalled-compactor test

**Status.** **Test/hook in tree; valid stalled-compactor bound still owed.** The [completed Oct 5 run](../../results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt) supersedes the earlier setup failure: it recorded 4352 MiB in 27 seconds, but its zero-L0 interpretation comes from shifted TSV columns ([W89](#w89)). Corrected samples show real L0 accumulation; the cap/lag bound remains untested. Use the [current W23 checkpoint](#w23--server-the-apply-path-blocks-on-l0-back-pressure-and-compaction-rewrites-the-table-to-absorb-a-few-mib) for the source distinction and next measurement. This private-cluster gate does not depend on the historical 19810 endpoint.

**What changes (server).** the test written in [W23](#w23--server-the-apply-path-blocks-on-l0-back-pressure-and-compaction-rewrites-the-table-to-absorb-a-few-mib) "correction": compactor stalled by a fault, measure memory and lag bound

**Gate / done when.** numbers in `results/measure/`; feeds W51/D30

**Forbidden.** —



## P3 — the benches (asked; they decide P4)

`efs-bench --bench` per its
plan above (ceiling first, 1→6 paths, QD 1/16/64/256, meta kind,
`--perf` in-process, one fcstor not serving 19810), then `efs-fuse
--bench` (`cpu` / `put` / `write` / dd ladder against the private
3-node cluster, never 19810). Done when each log has the four levels
beside the fio ceiling; the two largest adjacent gaps name P4's first
item.

## P4 — deferred until P3's numbers (long; one is a wipe)

| row | item | taken only if | what |
| --- | --- | --- | --- |
| P4.1 | plan row I — fragment on-disk layout (W34 residual) | `efs-bench --bench` shows the server PUT path (create + O_DIRECT write per 64 KiB fragment, 16 K/s per server at the cluster wall) is the wall | fewer path components / larger containers per fragment; **wipe**; design row first |
| P4.2 | plan row J — **W40** FUSE write copy | `efs-fuse --bench` shows `write` ≫ dd (the kernel FUSE path is the wall) | own `/dev/fuse` receive loop into pool buffers; W15 step 5 (`FUSE_CAP_SPLICE_READ`, kernel prerequisite done Sep 29) rides with it |
| P4.3 | plan row K — RDMA zero-copy receive | `efs-fuse --bench` shows `put` ≈ `write` ≈ dd with RTT the whole wall and reads in-flight bound | per-request posted receives into chunk buffers; transport change, design row first |
| P4.4 | 9-client scaling | after P1/P2 | `dd_wall.sh` 1/4/9 and `fio_honest_matrix.sh`; if 9 clients still share one ceiling (Sep 28: 2.5–2.8 GB/s = 6 % of 44 GB/s), the shared point is on the servers — P3's `efs-bench --bench` meta line vs data line says which |

## P5 — re-baseline and close

After each of P1.2, P2.2 and P4.x: 1-client
8/16 GiB dd+fsync, 16× dd, 4-reader cold read, 9-client dd, IO-500 9×4
debug; update the ceiling table in `docs/how-it-works/performance.md`; commit the
results dirs; move this plan's finished rows to project-history.


<a id="w61"></a>

## W61 · Local GC discards failed lane-sweep apply verdicts (queue row 0r)

**Class:** correctness bug. **Status:** repaired in `3a1b4a52`; private-store acceptance is recorded in the [GC checkpoint](../status/gc-implementation-20261007.md). Existing-service rollout and reconciliation of the incident remain open; this review did not inspect the remote raw fixture logs.

**Evidence and required change:** [GC reclamation review](../status/gc-reclamation-review.md#w61--local-gc-proposals-discard-the-apply-verdict). The reported xorinox 40 GiB deletion remains an investigation; these entries do not claim its cause is established.

**Gate:** Fault local and forwarded sweeps with BUSY/I/O errors; retain the reap marker on failure or unknown verdict; retry to complete fragment cleanup. Include verdict-ring eviction and term changes.

**Forbidden:** Removing a reap marker based solely on an applied index; hiding errors with longer retries.


<a id="w62"></a>

## W62 · Sweep/truncate batch boundaries lose delta GC records (queue row 0s)

**Class:** correctness bug. **Status:** repaired in `3a1b4a52`; private-store acceptance is recorded in the [GC checkpoint](../status/gc-implementation-20261007.md). Existing-service rollout and reconciliation of the incident remain open; this review did not inspect the remote raw fixture logs.

**Evidence and required change:** [GC reclamation review](../status/gc-reclamation-review.md#w62--batch-capacity-can-drop-delta-gc-records). The reported xorinox 40 GiB deletion remains an investigation; these entries do not claim its cause is established.

**Gate:** Sweep and truncate across batch boundaries with eight live deltas per chunk; durably queue every generation before deleting its references; verify all fragments are removed, including retry/crash cases.

**Forbidden:** Deleting chunk references while omitting live delta GC records; silently clipping delta counts.


<a id="w63"></a>

## W63 · GC progress and backlog visibility (queue row gc-obs)

**Class:** observability enhancement. **Status:** reporting implemented in `3a1b4a52`; [GC checkpoint](../status/gc-implementation-20261007.md) records private-store testing. Operational rollout, reliable interpretation and the original incident remain open; remote raw logs were not inspected in this review.

**Evidence and required change:** [GC reclamation review](../status/gc-reclamation-review.md#w63--missing-operational-visibility). The reported xorinox 40 GiB deletion remains an investigation; these entries do not claim its cause is established.

**Gate:** Expose bounded periodic backlog, actual fragment/byte reclamation, stage/error retries, oldest marker and last-progress times; distinguish idle, blocked and failing cleanup without per-fragment debug flood.

**Forbidden:** Calling scan counts freed bytes; claiming a root cause from quiet logs or unchanged record counts.


<a id="w64"></a>

## W64 · Durable append reservation replay across leader changes (queue row 0t)

**Class:** correctness / recovery. **Status:** open; indexed Oct 7, 2026.

**Evidence/current state:** The host-local replay race is fixed in ec1500ec, but reservation replay/failover is not a durable protocol. The round-20 first-record-loss trace remains unresolved evidence.

**Home:** [current detail](../status/in-flight.md).

**Gate:** Leader churn, lost reservation replies, identical-op replay, server restart and exact offsets/bytes; demonstrate a durable identity and fail-closed behavior for unknown outcomes.

**Forbidden:** Treating host-local cache state or clean concurrent repeats as durable failover proof.


<a id="w65"></a>

## W65 · Daemon exceeds graceful shutdown wait (queue row 0u)

**Class:** liveness investigation. **Status:** open; indexed Oct 7, 2026.

**Evidence/current state:** The final NUC rollout observed n1 exceeding a ten-second SIGTERM wait twice after clean client drains; deployment tooling used SIGKILL. The blocking path is not identified.

**Home:** [current detail](../status/in-flight.md).

**Gate:** Capture userspace stacks during a controlled stop; remove the actual blocker and show bounded graceful shutdown without discarding accepted data.

**Forbidden:** Raising a timeout or sending SIGKILL as the correctness fix; claiming graceful shutdown from client drain alone.


<a id="w66"></a>

## W66 · Metadata leadership distribution and routing (queue row meta-scale)

**Class:** scalability enhancement. **Status:** open; indexed Oct 7, 2026.

**Evidence/current state:** Two fixed parity Raft groups and dual-group routing remain implemented; logical shard count is not independent leadership capacity.

**Home:** [current detail](../status/metadata-scaling.md).

**Gate:** Follow the staged topology/routing plan and measured hotspot, transaction, restart and capacity gates in the linked home.

**Forbidden:** Claiming 1000 active-client capacity from connection counts; enabling an unvalidated topology.


<a id="w67"></a>

## W67 · Sparse dirty writes and bounded body admission (queue row mem1)

**Class:** correctness / resource exhaustion. **Status:** open; indexed Oct 7, 2026.

**Evidence/current state:** Committed foundation 033a842a and later regressions implement body admission and retained-byte ownership. Targeted pressure/failure/RSS acceptance remains open.

**Home:** [current detail](../status/fuse-memory.md#sparse-dirty-writes-bypass-reclaim-and-cache-admission-has-no-hard-bound).

**Gate:** Sparse/append concurrent pressure, failed publication, cold-byte checks and small-host recovery-reserve/RSS gates.

**Forbidden:** Calling the body allocator budget a process RSS ceiling; discarding retained dirty data on pressure.


<a id="w68"></a>

## W68 · Reply-buffer lifetime on FUSE worker retirement (queue row mem2)

**Class:** resource lifetime bug. **Status:** open; indexed Oct 7, 2026.

**Evidence/current state:** Committed foundation 033a842a implements pthread-key reply owners; local retirement/failure tests pass.

**Home:** [current detail](../status/fuse-memory.md#readreaddir-reply-buffers-leak-when-fuse-workers-exit).

**Gate:** Repeated real Linux FUSE worker creation/retirement and RSS with TLS/allocation-failure coverage.

**Forbidden:** Closing from mocked retirement tests alone.


<a id="w69"></a>

## W69 · Lookup memo returns pre-mutation stats (queue row 0j)

**Class:** correctness bug. **Status:** open; indexed Oct 7, 2026.

**Evidence/current state:** Existing queue row 0j now has a W identity. Mutation serial/invalidation code exists; historical dates do not establish current targeted rollout acceptance.

**Home:** [current detail](../backlog/work-items.md#0j--the-clients-50-ms-lookup-memo-returns-pre-mutation-stats-queue-row-0j).

**Gate:** Follow the original 0j POSIX/Spark du and stale-reply concurrency gates; record the build and current result.

**Forbidden:** Serving a pre-mutation reply as a fresh memo or closing from unrelated tests.


<a id="w70"></a>

## W70 · shrink-quota falsely reports background migration (queue row op1)

**Class:** operator observability bug. **Status:** open, found during the
Oct 7 documentation review. Documentation corrected; source behavior unchanged.

**Evidence:** `src/server/handler.c` handles SHRINK_QUOTA by subtracting the
requested amount from the running node quota, refusing a result below usage.
It starts no migration worker, but returns the legacy IN_PROGRESS status.
`cmd_shrink_quota()` in `src/mgmt/efs_mgmt.c` then prints "background migration
started". The capability inventory already says evacuation/migration is absent.

**Change/gate:** make the successful CLI response describe the actual quota
reduction and keep the wire response compatible or explicitly version it.
Exercise accepted and rejected reductions, verify the updated running quota,
and assert no migration is advertised. Do not promise restart persistence
without checking the startup quota source.

**Forbidden:** implementing an unreviewed evacuation protocol as a wording
fix; claiming migration/repair from an IN_PROGRESS enum name alone.


<a id="w71"></a>

## W71 · Fragment PUT success is not a durable persistence acknowledgement (queue row spec1)

**Class:** durability correctness. **Status:** open, source finding Oct 7;
no power-loss reproduction or deployment change in this review.

**Evidence:** `src/server/store.c:server_write_fragment_to_path` zero-initializes
`shard_io_arg`; `sync_write` is assigned only under `EFS_BENCH_BUILD`.
`shard_io_thread` adds O_SYNC only in that benchmark build. `finish_shard_write`
flushes only when `sync_write` is set, so the production PUT path can return OK
after write/length/close without a persistence barrier. Buffered PUT explicitly
omits fsync. `handler.c` then ACKs a successful store. `efs_fuse_fsync_ino`
drains/REPORTs but issues no separate fragment-store flush. Direct I/O alone
does not establish the spec's durable-ACK boundary; process restart tests do
not simulate losing the OS/device's volatile state. This is not proof of the
cause of the 40 GiB reclamation incident or W55.

**Change/gate:** establish the required persistence boundary for fragment
payload, stored checksum, final length and necessary directory entries before
acknowledged durable publication, with batching allowed if the acknowledgement
ordering remains correct. Inject write/flush/close failures; reject publication
on a failed barrier; test buffered/direct paths, new and existing objects,
crash and power-loss behavior on the supported hardware assumptions. Check
metadata/data ordering and retain raw build/configuration evidence.

**Forbidden:** equating RDMA completion, O_DIRECT, successful close or a
benchmark --sync result with production persistence; silently weakening the
accepted durability contract or fixing only FUSE flag handling.

<a id="w72"></a>

## W72 · Production mount session lifecycle is not connected to I23 (queue row spec2)

**Class:** fencing integration. **Status:** open; metadata/coordinator and
staged publication gates exist, full production integration absent.

**Evidence:** `inode_rpc.c:opid_seed_locked` creates a local random UUID at
fixed epoch 1. HOLD sends only its base request; FLOCK sends its range but
no session suffix. The server's `host_sess_gate` accepts an absent suffix via
the legacy stand-in path. Append has a fixed-epoch owner suffix, which does
not establish the mount-wide session/revocation lifecycle. `efs-mgmt
raft-session` can create/register/establish/fence, and D25 typed publication
checks shard-local session state: those are implemented primitives, not proof
that all mounted production mutations participate. See the D25 routing page
for staged I23 admission and remaining abandoned-stream cleanup.

**Change/gate:** establish real mount sessions, register every touched authority
and connect the relevant production requests to that authority. Exercise a
partitioned old client, coordinator restart and leader changes; old mutations
must be rejected after the completed barrier, and lease/lock/append cleanup
must not precede it. Record public-path results, not only metadata unit passes.

**Forbidden:** treating opid epoch fields, connection timeouts or manually
fencing an unrelated UUID as mount fencing; duplicating the existing D25
publication primitives or reclaiming state before revocation completes.

<a id="w73"></a>

## W73 · Synchronous application-write publication is not explicitly wired (queue row spec3)

**Class:** synchronous-write integration. **Status:** specified, no explicit
O_SYNC/O_DSYNC publication in the inspected FUSE write callbacks.

**Evidence:** both `efs_fuse_write_admitted` and
`efs_fuse_write_buf_admitted` can accept dirty data/queued jobs and return bytes;
they check O_APPEND/O_DIRECT but do not dispatch publication on O_SYNC/O_DSYNC.
The spec already marks O_SYNC unwired. Engine benchmark O_SYNC is a separate
build/option and does not implement the application's flags. A kernel may
issue follow-up FUSE fsync requests: this review did not trace the real kernel
sequence, so the finding does not prove every O_SYNC syscall returns early.

**Change/gate:** trace both write callbacks under O_SYNC, O_DSYNC and the
supported mount-sync behavior. Either demonstrate correct kernel-driven
publication for each supported path or wire it explicitly. A successful
synchronous syscall must satisfy publication plus W71's target durability;
exercise stalled publication, PUT failure, append and buffered/direct paths.

**Forbidden:** claiming support from flag retention or engine --sync; requiring
ordinary buffered writes to publish; closing without real syscall evidence.

<a id="w74"></a>

## W74 · Configurable protection profiles, debt and automatic repair remain absent (queue row spec4)

**Class:** protection/recovery feature gap. **Status:** accepted design, absent
public control-plane/data-plane integration; no new design decision here.

**Evidence:** `EFS_NUM_FRAGMENTS` is 3 and `erasure.h` specifies fixed 2+1.
`efs_publication_from_rec` stamps K2F1; `meta_apply.c:evidence_ok` rejects other
profiles. HEAL_STATUS returns a zero-filled structure. No production owner
that reconstructs missing live fragments, effective_f/target_f transition or
profile-cutover controller was found. Namespace/metadata recovery and reading
from two survivors are not data repair. W42 separately records the two-ACK
legacy PUT success/protection-debt concern; staged evidence validation is not
a repair implementation. Five or seven nodes alone do not activate f=2/f=3.

**Change/gate:** implement the accepted profile/debt/repair control plane in
stages; advertise only supported and validated guarantees. Demonstrate missing
fragment reconstruction onto a healthy target, protection restored before a
subsequent loss, durable debt across restart and authority changes, and safe
profile cutover while writers remain active. Retain actual configured RF/EC
and permanent-loss evidence. Coordinate with W66 metadata topology.

**Forbidden:** counting returning/empty nodes as restored fragments, calling
GC or metadata snapshot recovery repair, or advertising the spec's f=2/f=3
matrix as current production capability.

<a id="w75"></a>

## W75 · Integrity evidence is payload-only and missing evidence is accepted (queue row spec5)

**Class:** integrity correctness. **Status:** open, inspected source gap to I25;
no corruption experiment was run in this documentation review.

**Evidence:** `write.c` hashes each fragment payload using `efs_hash` without
its immutable identity. PUT passes the supplied digest to the store without
an independent nonzero-payload rehash. GET verifies a stored checksum when
`sum_ok` is true, but when it is false computes a new checksum from the bytes
and serves them. Optional client verification compares against returned
checksum; it does not restore a lost independent trust anchor. A readable
object with corrupted bytes and missing integrity evidence can therefore be
returned with a freshly matching digest. The ordinary two-fragment path
`get_one_reply` also discards the returned digest and does not consult
EFS_READ_VERIFY; the switch only affects `efs_client_get_fragment`, so it is
not comprehensive client read verification. I25 specifies identity plus
payload; automatic repair is separately absent under W74.

**Continuation repair (Oct 7):** data GET now treats missing/truncated digest
evidence as an unavailable fragment; it never substitutes a newly computed hash
as proof. Recorded-digest mismatch rejection remains. NUC real-RPC corruption
tests pass for payload flips, missing/truncated digests and digest flips in
buffered/direct mode with one/two roots, preserving valid-object reads. The
baseline serves missing-digest data successfully and fails this same gate.
The existing metadata-table root-integrity path is separate and unchanged.
The client verification option now checks ordinary parallel GET replies as well
as fallback GETs, hashes zero payloads rather than trusting a zero digest, and
initializes its shared option with pthread_once. NUC production-code reply tests
reject corrupted zero/nonzero replies and accept valid ones. Identity-bound on-disk hashing remains below. Data PUT also rehashes the received payload before storage
or quota mutation; NUC tests reject wrong caller digests without damaging an
existing valid object. Metadata-table integrity remains on its separate protocol.
Mapped base and span reads now compare returned digests with the captured
metadata checksums on preferred and fallback paths, even when optional payload
rehashing is disabled. NUC FUSE object-swap tests recover one bad fragment using
parity, reject two bad fragments with EIO and verify restored bytes on cold reads.

**Change/gate:** define a compatible, trusted identity-bound integrity format
and fail-closed behavior for missing/corrupt integrity evidence; preserve
explicit legacy-format handling rather than silently reclassifying bytes as
verified. Exercise payload flips, digest removal/truncation, object swaps,
incorrect caller digests, and valid legacy/new objects on TCP/RDMA. A verified
alternate fragment or explicit error is acceptable; silent corrupted bytes
are not. Check metadata-page integrity separately.

**Forbidden:** using freshly computed checksums as evidence that existing
bytes are correct; assuming client rehash proves immutable identity; making
an unreviewed on-disk format change or claiming repair from a decode fallback.


<a id="w76"></a>

## W76 · Namespace opid capacity exhaustion silently removes retry identity (queue row spec6)

**Class:** conditional retry/idempotency correctness. **Status:** open source
finding; reaching this limit on an ordinary mounted workload is not established.

**Evidence:** `EFS_OPID_INFLIGHT` is 512 in `client_internal.h`.
`inode_rpc.c:opid_begin` leaves its request identity zero and returns -1 when
all slots are occupied. `opid_suffix` then omits the suffix, and CREATE,
UNLINK, RENAME_AT and LINK callers still send/retry the mutation. Normal
identified retries preserve their bytes, but the exhaustion fallback does
not satisfy I16's stable identity. Healthy POSIX acceptance does not exercise
an ambiguous reply at that limit. Session lifecycle is separately W72.

**Change/gate:** bounded admission/backpressure or a visible error when no
identity slot is available, preserving the caller's whole-call deadline;
never issue an unprotected mutation as success admission. Occupy every slot,
exercise all affected callers, inject a lost successful reply and retry,
then release slots. Verify single namespace effect, bounded timing and no
watermark advancement past outstanding requests. Check actual FUSE concurrency
limits before claiming this is reachable in the default mount.

**Forbidden:** dropping identity to avoid waiting, treating an arbitrarily
larger finite table as a correctness fix, or closing from unrelated POSIX tests.


<a id="w77"></a>

## W77 · Multi-chunk public publication/observation lacks an established I24 gate (queue row spec7)

**Class:** atomic-publication/observation integration gap. **Status:** open,
source inspection; no torn-read reproduction or current I24 acceptance found
in this review.

**Evidence:** the accepted atomicity unit is one FUSE write request, not an
arbitrarily larger kernel syscall. Legacy REPORT accumulates publications in
separate `host_pub_batch` buffers per Raft group and may split each buffer into
several proposals. It does not encode one original FUSE request's cross-lane
transaction identity. The staged PUBLICATION endpoint is per chunk; namespace
and resize transaction primitives do not activate whole-write publication.
`efs_client_read_refs` pulls a range and then pins cache entries one chunk at
a time without a final cross-lane version collect/revalidation. This is a
public-path gap to the specified contract, not proof that every multi-chunk
operation tears. Other read paths and actual request boundaries must be
included in the gate.

**Change/gate:** preserve the supported request's mutation identity/range,
connect all participating lanes to one atomic visibility decision, and validate
a whole read's collected mapping versions. Deterministically pause between
cross-lane proposals and between chunk fetches; a peer must observe all-old
or all-new for one accepted atomic request, never a splice. Cover partial
chunks, both FUSE write callbacks, read/ref paths, restart and leader change.
Keep the above-max_write kernel-interface limitation explicit. Coordinate
with D25 integration, W66 topology and W71 persistence rather than claiming
those primitive gates already establish I24.

**Forbidden:** equating a REPORT batch or successful per-chunk CAS with one
request-wide transaction; extending the atomicity claim beyond observable
FUSE request boundaries; closing from healthy non-racing POSIX tests.


<a id="w78"></a>

## W78 · Server wrapper trusts stale PID-file ownership (queue row op2)

**Class/state:** conditional operator correctness; open, source-reviewed Oct 7.
**Evidence:** `scripts/server.sh:kill_from_pidfile` reads the stored PID and
passes it to `kill_pid_graceful`. That helper checks only `kill -0`, sends
SIGTERM, then SIGKILL after about three seconds. Startup and path-based stop
both use this cleanup. No executable, start-time or storage/port identity is
checked before signaling. A stale PID reused by another process can therefore
cause an unrelated process to be terminated. This is conditional source
reachability, not a reproduced incident; no processes were signaled in review.

**Next:** validate numeric PID and process identity before signaling, bind the
record to a process start identity, and handle stale records without killing
an unrelated process. Coordinate the shutdown bound with W65 rather than
assuming SIGTERM completion means data was drained.

**Gate:** isolated wrapper fixtures for absent/dead/nonnumeric/reused PID,
matching daemon and unrelated live process; unrelated processes survive start
and stop. Verify identity again around escalation to address PID reuse races.
No cleanup test may operate on an operator's real storage/processes.

<a id="w79"></a>

## W79 · mkfs CLI advertises a name it silently ignores (queue row op3)

**Class/state:** operator observability bug; open, source-reviewed Oct 7.
**Evidence:** `src/mgmt/efs_mgmt.c` general help advertises
`mkfs <node:port> <export-name>`, but `cmd_raft_mkfs` only parses `argv[0]`
and ignores extra arguments. Both mkfs aliases use that handler. Metadata
`efs_meta_apply_mkfs` returns OK for an existing root; the host reads back the
committed placement salt before distributing it. The command does not create
a second named export or fail because a supplied name exists. FUSE still
accepts a name token and builds a local shell with it; this is not named-export
creation. No cluster initialization was run in review.

**Next:** align help, argument validation and success text with the accepted
single-export contract. If legacy extra arguments remain accepted, explicitly
report their compatibility meaning; do not imply namespace creation. Check the
FUSE failure hints for retired `list-exports`/old metadata-table guidance too.

**Gate:** both aliases' help and invocation tests; extra/malformed arguments
have an explicit outcome. Repeated initialization against the same intended
cluster preserves root, data and committed salt, including uncertain-reply
retry. This work does not authorize multi-export design or destructive reset.


<a id="w80"></a>

## W80 · efs-query presents retired placeholder statistics as real totals (queue row op4)

**Class/state:** operator observability bug / missing query integration; open,
source-reviewed Oct 7. `src/server/handler.c:EFS_MSG_QUERY_STATS` explicitly
notes that the whole-table query was retired, zeroes a reply and sends it.
`src/query/efs_query.c` accepts that reply and prints total files/bytes and
users as an ordinary successful result (also under `--raw`). The command
therefore has no current source path to report real filesystem totals. This
was source inspection, not execution against a live nonempty cluster.

**Next:** explicitly report the unsupported/placeholder state or implement a
verified metadata-backed query, with a defined consistency and accounting
scope. Do not substitute physical node fragment usage for logical file/user
statistics or claim `.stats` is an equivalent cluster-wide per-user query.

**Gate:** a nonempty isolated export must not produce a successful empty-store
claim in either output mode. If implemented, test create/write/unlink,
open-unlinked files, users, directory rollups, restart and query races against
an independent oracle. If retired, gate an explicit unsupported outcome and
update command help. Placeholder zero is not a GC completion signal and does
not diagnose the current reclamation incident.


<a id="w81"></a>

## W81 · Automatic directory spread responds to size, not pressure

**Class/state:** accepted scalability requirement / feature gap; source-reviewed
Oct 7, awaiting triage. The directory protocol requires size OR sustained
serialization pressure to start spread, with the numeric pressure bound still
unspecified. `src/meta/dir_layout.c:efs_meta_dir_note_entry` starts SPLITTING
only when an added entry makes nents exceed `efs_dir_spread_min`.
`host_dir_spread_pass` drains directories already marked SPLITTING; it is not
a queue/latency/rate-based admission trigger. No automatic pressure trigger
was found in reviewed metadata/server paths. Explicit DIR commands and
size-triggered migration exist. No new hotspot measurement was performed.

**Next:** define and review the bounded pressure signal/admission policy,
coordinate it with W66 metadata scaling, and use the existing committed layout
transition/migration machinery. Do not invent a numeric trigger as documentation
or mistake old same-directory rate improvements for this feature's completion.

**Gate:** a hot directory kept below the size threshold can admit spread under
the agreed sustained-pressure policy, while cold/small directories do not
churn. Validate bounded migration, lookup/create/unlink/readdir consistency,
restart/leader change and fair progress alongside unrelated directories;
report offered load, achieved rates, tails and memory on a named build.


<a id="w82"></a>

## W82 · Completed read authority is reused by an isolated former leader

**Class/state:** linearizable-read correctness; core-model reproduction Oct 7.
`host_read_index` captures the local commit index and returns immediately when
`host_view_covers` sees a completed read index covering it. `try_commit` can
populate that coverage from a prior write quorum. `efs_raft_read_covers` has no
request freshness criterion; leader ticks send heartbeats but do not revoke
completed coverage merely because the leader has lost contact with a majority.
A higher-term message steps it down, but an isolated node cannot receive one.

**Reproduction:** an isolated copy of the existing in-memory Raft test elects
node 0 in term 1, commits A, and partitions it bidirectionally from nodes 1/2.
The majority elects node 1 in term 2 and commits B. After further old-leader
ticks, node 0 retains A and `read_covers(local_commit)` returns 1. The assertion
that later reads must not reuse that authority fails (exit 1). Retained
[patch](../archive/round5-stale-read-repro.patch) and
[output](../archive/round5-stale-read-output.txt) establish this core path;
no live FUSE/server stale-read experiment was run. The host's covering-view
fast path is source inspection, not a model of its full networking stack.

This violates the purpose of quorum-backed authoritative reads if the cached
result is used for a later request. The
[Raft paper, §8](https://raft.github.io/raft.pdf) requires a majority leadership
check for read-only requests; previously committed writes are not perpetual
read authority. The architecture forbids clock-based leader leases.

**Implementation (Oct 7, follow-up round 1):** each new host request requires
`read_round+1`, so neither a completed round nor probes sent before admission
can cover it. AppendEntries echoes the round context in its existing fixed wire
slot; only successful matching-context voter replies count. Write commits no
longer create read authority. Completion requires a committed current-term entry,
application and unchanged configuration. Published host predicates use a coherent
sequence-checked snapshot. The pump credits election no-ops only through the
already-fsynced disk watermark, avoiding a fresh-read/first-write deadlock when
only the new leader and one follower survive.

NUC unit regressions reject delayed replies from completed rounds and revoke
read authority on higher terms. The private real-daemon
`tests/live/raft_read_freshness.py` cuts peer traffic in both directions while
keeping client RPC reachable: majority changes root mode, eight concurrent old
leader GETATTRs fail, then healed reads return the new mode. The continuation also tests majority-side
rename followed by stale old-leader LOOKUP, verifies explicit BUSY/NOT_PRIMARY
replies and that the old daemon remains alive and believes it is leader, then
checks the renamed lookup after healing. Transaction/session/publication views
and configuration-change partition scenarios remain owed.

**Gate:** old leader serves A, majority commits B after isolation, then a new
read at the old leader must fail/retry, never return A as authoritative.
Cover delayed heartbeat replies, multiple concurrent readers, leadership and
configuration changes, current-term no-op and follower-forwarded reads. Repeat
through actual LOOKUP/GETATTR, transaction-decision and session/publication-view
RPC paths. No healthy POSIX result closes this partition gate.

<a id="w83"></a>

## W83 · Transaction decisions have no inspected retirement protocol

**Class/state:** metadata lifecycle / bounded-state feature gap; source-reviewed
Oct 7. `efs_txn_decide` stores a decision keyed by coordinator shard and txid.
`efs_txn_resolve` and `efs_txn_drop` scan/delete INTENT, GUARD and REDUCE kinds;
the coordinator decision is outside those scans. Reviewed decision-key callers
only write/read it. No durable participant-acknowledgement or safe decision-GC
path was found. Transaction recovery resolves pending intents, not old decisions.
Thus the documented bound by active/recovering transactions is not established;
retained decision keys can grow with completed transaction history. No growth
benchmark or measured disk-cost claim was made.

**Next:** design the accepted safe retirement condition: every relevant
participant has durably acknowledged resolution and no eligible recovery/retry
can still require the decision. Track that proof explicitly. Do not delete
decisions by wall age, lack of locally visible intents, or an in-memory ACK.
A missing decision must not reinterpret a committed effect as undecided.

**Gate:** sustained completed transactions reach a defined retained-state bound;
crash between decision, resolve and acknowledgement; lagging/offline participant,
lost replies, old retry and coordinator restart all recover the same verdict.
Demonstrate safe crash-restart reclamation without depending on GC thread timing.

<a id="w84"></a>

## W84 · Namespace guard bounds reject supported deep/spread-directory work

**Class/state:** namespace completeness / admission gap; source-reviewed Oct 7.
`EFS_TXN_MAX_PART` is 64 for general transaction envelopes, but
`EFS_TXN_NAMESPACE_MAX_PART` remains 8. `host_pver_guard_chain` refuses once it
has eight ancestry guard records, even if another ancestor shares a shard;
its separate 64-hop walk bound is not the effective supported depth. Namespace
`host_parts_add` can hit its eight-distinct-participant limit earlier.

`host_rmdir` also returns BUSY after eight used-lane dseq guards on a HASHED
child. An empty directory whose historical used-lane set exceeds eight can
therefore be refused even after all children are removed. Equivalent hashed
replacement checks exist in the directory rename path. Repeating without a
layout/input change does not remove these static bounds. No live deep-rename
or emptied-spread-directory reproduction was run.

**Next:** reconcile bounded work with the accepted full-ancestry and all-used-
lane predicates. Document any approved product limit explicitly; otherwise
support the required predicate set without dropping guards. A bounded memory
allocation is necessary, but BUSY on permanently supported work is not a
complete overload policy. Coordinate envelope/buffer sizing and recovery gates.

**Gate:** rename below/above eight ancestry records (including same-shard
ancestors), concurrent cycle attempts, and empty HASHED directories with more
than eight used lanes; rmdir and directory replacement must preserve phantom
protection and make progress. Validate restart/leader changes, wire bounds,
status/errno and finite retry behavior. Do not weaken guards to pass the tests.


<a id="w85"></a>

## W85 · Path-hint eviction can relabel an ambiguous PUT retry as first-send

**Round-6 current source:** a concurrent repair, now committed as `6d6056c3`, owns first-send
state in the PUT attempt and locks the root cache; cache misses on retry probe.
`tests/test_put_hint.py` is present. The GC checkpoint now reports
helper/concurrency and two-root accounting tests; this review did not rerun
them or independently establish release/restart acceptance. The
reproduction below retains the round-5 defect-bearing baseline.

**Class/state:** conditional storage/accounting correctness; hint-helper
reproduction Oct 7. D7 permits NEW only for a generation never PUT before;
an ambiguous/lost-reply retry must use a real root hint or probe (0).
`write.c:path_hint_get` remembers tried/valid in a 4096-entry direct-mapped
cache. A collision overwrites the tried record. Retrying the earlier PUT then
returns NEW again. The source comment says a collision only costs an access,
but this case suppresses the server's probe instead.

An extracted helper fixture uses node 1, chunk/fragment 0, inodes 1 and 4097,
which both map to slot 1959. First A, colliding B, then lost-reply retry A all
return 0xffffffff. Retained [fixture](../archive/round5-path-hint-repro.c) and
[output](../archive/round5-path-hint-output.txt) reproduce the hint violation;
no end-to-end multi-root/store/quota experiment was performed. The helper's
existing lack of synchronization also requires a concurrency audit; the
sequential collision already suffices for this finding.

Server source treats NEW as skip-search, selects a writer root, and treats
that write as newly chargeable when quota accounting is enabled. An earlier
successful PUT with a lost ACK can therefore be charged again on the same
root or recreated on another root. These are downstream source concerns,
not measured quota drift or a diagnosis of the 40 GiB reclamation incident.

**Next:** own first-send versus retry state with the actual PUT generation/
request lifetime, or use a safe probe when history is unavailable. An evictable
performance hint must not assert nonexistence after an ambiguous outcome.
Preserve bounded state and audit races; do not recover certainty by guessing.

**Gate:** lost ACK, colliding cache entry, retry of the same object generation,
same-root and multi-root routing, concurrent PUTs and restarted clients.
One logical fragment has correct node usage, no stray duplicate roots and
readable identical bytes; retirement removes every legitimate object. Verify
first-send optimization still respects D7 and retries never falsely assert NEW.


<a id="w86"></a>

## W86 · Durable PUT tickets — staged ownership and revocation integration

**Class/state:** feature / safe reclamation; metadata implementation committed in `b3a11877`, staged for production integration. The [GC checkpoint](../status/gc-implementation-20261007.md#approved-unpublished-body-policy) records user approval of the narrow abandoned-upload policy; [D31](../status/decisions.md#d31--recorded-abandoned-upload-policy) indexes that record. This review creates no new approval and does not resolve D28's broader salvage question.

**Evidence:** `include/efs/put_ticket.h`, `src/meta/put_ticket.c`, metadata publication hooks and `tests/test_put_ticket.c` exist. Admission is bounded at 128 tickets per lane-shard/session; PUBLISHED joins the mapping batch; reclamation requires authoritative global and shard revocation state, and retirement preserves a sequence floor. The helper explicitly requires colocated authoritative session/shard views. Production host encoding and mount/session/data-plane coordination do not yet carry the complete lifecycle. Source inspection is not a rerun of the checkpoint's restart tests.

**Next/gate:** connect W72 mount sessions, versioned admission before every PUT, immutable object/digest validation, shard-local rejection floors, authoritative completed global revocation, durable storage fences, stable membership identities for captured member masks and all-configured-member deletion ACKs. Retain the ledger when any member is offline. Exercise lost admission/publication/deletion replies, coordinator/lane leader change, restart, late PUT and body revival; no live mapping can be collected. Gate integration with W43, W71 and W82 before activation.

**Forbidden:** deletion from age, disconnect, missing receipt, a local fence or another group's asynchronously applied cached state; treating staged metadata tests as production activation; promising salvage of all write-returned bytes.


<a id="w87"></a>

## W87 · Buffered concurrent append loses records under broader load

**Class/state:** correctness investigation; failure recorded in the Oct 7 [GC checkpoint](../status/gc-implementation-20261007.md#tests-and-limits). Fixture `/data1/efs/gc-buffered-3ohdte4d` reportedly passed focused GC checks but failed both broad POSIX concurrent-append cases with missing records. Ten isolated repeats subsequently passed; they do not erase the original failure. Raw remote fixture logs were not inspected in this round.

**Next:** retain failing results, process/source hashes and reservation/publication traces; record the traced full buffered rerun's outcome. Distinguish append admission offsets, identity replay, byte ownership and publication before assigning a cause. W64 concerns durable failover replay; it is related coverage, not a proven explanation of this loaded buffered failure.

**Gate:** full buffered single/peer POSIX plus repeated concurrent append under the original broader load, exact expected records/offsets/bytes, no missing/duplicate records, and a focused regression for the isolated cause. Retain a failing run even if focused repeats pass; record fixed-build identity and cold verification. Do not label buffered production acceptance complete from isolated repeats.


<a id="w88"></a>

## W88 · Legacy span replay loses idempotency after folded-history eviction

**Class/state:** publication/retry correctness; isolated metadata reproduction Oct 7, round 6. D1 requires a folded span retry to be a no-op; I16 requires byte-identical retries not to repeat effects. This finding concerns the active legacy path, not the staged durable-result receipt/floor design.

**Evidence:** `meta_apply.c:publish_inner` suppresses a legacy span replay while its generation/range or a folded len-zero tombstone remains in the eight-entry trailer. New spans evict oldest tombstones to make room. After eviction, the old same-epoch span can append again to the current base. `raft_host.c:host_pub_pack` checks `chunk_holds`, but a first span with `base_gen == 0` has no later base-generation check; `pack_publish_cmd` carries neither a durable publication receipt identity nor a retirement floor for this legacy path.

The [retained fixture](../archive/round6-span-replay-repro.c) uses the actual metadata apply code over in-memory KV: publish eight one-byte spans, fold them, publish a later full image, append a disjoint new span (evicting the oldest folded identity), then retry the original span byte-for-byte. Result: OK and one live delta naming the old object over the later base. [Output](../archive/round6-span-replay-output.txt) fails the no-reintroduction assertion. This is a metadata-state failure, not a byte-level/FUSE/network reproduction; physical object availability and host packet acceptance require the next gate.

**Next/gate:** use the staged durable identity/verdict/retirement primitives where appropriate, and carry their authority through production REPORT before claiming D1 replay safety. Distinguish a genuinely new write from an ambiguous retry after later writes. Test lost original ACK, history eviction, later overlapping full/span writes, old objects retained and GC/re-PUT, restart/leader change and exact bytes through the actual host/FUSE path. Retired identities must fail closed rather than resurrect effects; bounds must apply to admission and retained identity state.

**Forbidden:** treating an evictable eight-entry trailer as durable replay authority; raising its size as a safety proof; introducing a chunk lock or silently weakening D1/I16. No new design approval or execution priority is created here.


<a id="w89"></a>

## W89 · W23 TSV field packing corrupts derived acceptance metrics

**Class/state:** verification correctness; raw-evidence/source confirmation Oct 7, round 6. `tests/measure/w23_stalled_compactor.sh:sampler` inserts `${ko}` as one tab field even though it contains four space-separated KV values. Rows have nine fields under a twelve-field header; the peak reducer assumes twelve, shifting L0/RSS/lag/pump/error columns. The retained Oct 5 SUMMARY falsely reports n_l0 zero and assigns RSS values to l0_bytes. Its “stall never bit” interpretation is withdrawn.

**Evidence:** [original samples](../../results/measure/20261005-040810-w23-stalled-compactor/samples.tsv), [node3 log](../../results/measure/20261005-040810-w23-stalled-compactor/s3.log), [corrected expansion](../archive/round6-w23-samples-corrected.tsv) and [recomputed peaks](../archive/round6-w23-peaks-corrected.txt). Node3 logs L0=22 / 3,367,253 bytes during the 04:08:29–04:08:56 park; corrected samples confirm these peaks. Node1 peaks at 4 / 105,281, node2 at 13 / 1,366,435. Recorded RSS peaks are 30,316 / 25,936 / 25,624 KiB. These are samples, not guaranteed continuous maxima. All sample lag peaks are zero; the 27-second RSS-threshold run does not prove a follower pressure bound.

**Implementation (Oct 7 continuation):** the sampler serializes twelve explicit
fields through `tests/measure/w23_samples.py`. The reducer validates the header,
field counts and numeric types, fails on malformed rows and preserves unavailable
observations as `NA`. Counter peaks survive resets. NUC synthetic tests cover
all distinct fields, the historical nine-field shape, bad values, missing values
and counter resets; driver shell syntax passes.

**Remaining gate:** rerun W23 under meaningful cap pressure and compare samples
with raw logs. This format repair does not establish a follower pressure bound;
the original record and withdrawn interpretation remain retained.

**Forbidden:** accepting shifted/missing fields as zero, claiming no effect or a bound from the old summary, or silently replacing original evidence.
