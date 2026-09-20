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

**State as of Sep 20 08:00 (verified by probe, not memory):**

- Uncommitted code in the working tree (on top of `cc828d8` + the Sep 20
  docs/rules split commit):
  - `src/meta/meta_apply.c` — `alloc_key_claim`: a log-path CREATE / MKDIR
    alloc is `BUSY` while a txn holds an `EXCL` intent on the shard ALLOC
    key, and bumps that key's version so an outdated txn PREPARE is
    `STALE`. Closes the second allocator race (log path vs cross-group
    mkdir / hashed create; posix `names_*` `cafeé` + `aaaa…` → one ino
    4746, `cwi-fail: ino_dup`). Gate `tests/test_meta_apply.c`
    `test_alloc_vs_txn_intent`. `test_meta_apply` / `test_sim` /
    `test_txn` / `test_wire` **OK on fcstor003**.
  - `src/client/read.c`, `src/client/ops.c`, `src/client/client_internal.h`
    — a read whose layout pull fails now **fails** instead of zero-filling;
    `pull_chunks_range` returns the first RPC error; `pull_layout_miss` has
    a 200 ms range cache instead of a 1/s rate limit. Built into `efs-fuse`
    on **fcstor007 only**; 008–015 run the pre-change client. The Sep 20
    0-error IO-500 result did NOT include this change (server fix alone).
- `efsd` + `efs-mgmt` built at `/tmp/efs` on fcstor003–006 from this tree
  (`cc828d8-dirty`) but **not started**; all four servers are still running
  `a9e94a63f880-dirty`. Because the build ID changed, a rolling restart is
  rejected by the HELLO gate — **stop all four, then start all four**
  (unwiped storage: no `--join`).

**Steps:**

1. Pre-flight (deploy rule). Expect `pgrep -x efsd` = 1 on each of 003–006
   and `build=a9e94a63f880-dirty` in `/tmp/efs/efsd.log`.
2. `pkill -9 -x efsd` on all four; confirm 0 each. Then start all four with
   the exact command in the deploy rule ("Restart one efsd"), node-id 1–4 =
   fcstor003–006, no `--join`. `./efs-mgmt raft-status 172.16.223.57:19810`
   from fcstor003 until group 0 and group 2 each show one leader and
   `commit == applied` on every voter.
3. On fcstor007: `EFS_TRANSPORT=tcp bash tests/run_tests.sh posix fcstor007.ib`
   (jobs=1). Gate: no `ino_dup` line in any efsd.log, no `names_*` dangling
   dentry, and the suite at or above **195/201** with only the known
   signature (`dir_deep_nesting*` / `dir_many_files` / `names_crazy_dirs`
   15 s walks, `mmap_write_read` SKIP, `concurrent_writes_disjoint` flake).
4. Build + remount `efs-fuse` on 008–015 (deploy rule "Restart one
   efs-fuse") so every client carries the read.c change; re-run the 9×4
   IO-500 debug once (`SLOTS=4 NP=36 bash tests/perf/io500/run.sh debug`)
   and confirm 0 `-R` errors still. Copy `result.txt` + ini to
   `results/io500/<id>/`.
5. Commit code + rules + this file with the result directories cited.

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
3. 7/36 mdtest `rmdir` transient ENOTEMPTY under load (clean seconds
   later) — same-parent txn STALE family; the client retries STALE only
   for CREATE (see history "same-parent concurrent mkdir EIO").

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
