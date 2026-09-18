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

**Where the project is (Sep 18 2026).** [architecture.md §10](../architecture.md)
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
Take the lowest-numbered item that is not marked done. Each item names what to
change, how to measure it, what proves it, and what is forbidden. If an item
needs a decision the spec does not contain, **stop and ask** (§4); several
items below are blocked on exactly that and say so.

**The one live cluster is port 19810** on fcstor003–006 (`/data1/01–06/efs`,
`--quota 36T --direct-io`, TCP), clients fcstor007–015 at `/tmp/efs-mount`.
19820 is retired. Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs`
without being asked — several items below run on the existing data.

---

### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency
(W2's numbers are meaningless until W1 moves, W3 is the same wall as W1 at
8× the size). Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

Baselines every performance item is measured against, all honest (flush in
the clock, reads after remount, `findmnt` verified `fuse.efs-fuse`):

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync` | **448 MiB/s** (18.3 s) | `results/perf/20260918-dd-1c/` |
| 1-client honest fio 9×2g sw-1m | **924 MiB/s** | `hot-sw-1m-9job-fcstor007.txt` |
| 1-client honest fio 1-job 512m | 247 MiB/s | same |
| first honest matrix, 1 client | sw-1m 209 · ow-1m 196 · rw-1m 196 · rw-128k 194 · rw-4k 89 · sr-1m 3141 · rr-1m 2276 · rr-128k 1492 · rr-4k 144 | `results/perf/20260917-honest/` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

---

#### W1 — Split the single-client fsync tail, then remove the larger half

The 8 GiB `dd+fsync` above spends ~6 s streaming at 1.3–1.4 GB/s and then
**~12 s inside `fsync`**. That tail, not bandwidth, is why one client reports
448 MiB/s. Both daemons are far from CPU-bound during it: `efs-fuse` ~1.1
cores (blake3 52% in `hash_write_fragments` / `dcache_flush_slot_inner`,
memmove 33%, `poll` 0.9%), `efsd` 0.20–0.40 CPUs (hottest is the dual-host
node at 49% `memcmp`/`lsm_get` under `server_raft_host_report` →
`efs_meta_apply_get_chunk`). So the tail is **off-CPU wait**, and the profile
already says where it is not.

`fsync` on this path is two serial phases in `efs_fuse_fsync_ino`
(`src/client/efs_fuse.c`): `efs_file_data_sync_fh` (flush remaining dirty
chunks: hash + EC + PUT) and then `efs_client_report_dirty_ino(ino, 1)`
(`src/client/write.c`), which publishes every dirty chunk rec. 8 GiB ÷ 128 KiB
= **65 536 chunk recs** in one report.

1. **Measure the split first.** Instrument the two phases (or time them from
   the client) and record milliseconds for each on an 8 GiB `dd+fsync`. Do
   not proceed until you know which phase owns the ~12 s. Everything below is
   conditional on that number.
2. **If REPORT owns it:** the server side batches `HOST_PUB_BATCH_N = 256`
   PUBLISH cmds per Raft proposal (`host_pub_batch_push` /
   `host_pub_batch_finish`, `src/server/raft_host.c`), so 65 536 recs is
   ~256 proposals. Check, with evidence, whether those proposals actually
   pipeline or still serialize a round trip each (256 × ~40 ms ≈ 10 s fits the
   observed tail exactly), and whether the per-rec `lsm_get` in
   `host_pub_pack` / skip-identical is paid once per rec. Fix the one the
   measurement blames: deeper batching, or removing the per-rec lookup.
3. **If flush owns it:** the remaining dirty set is hashed and PUT at close.
   That is the blake3/memmove path already visible in the profile — a client
   pipelining problem (hash while PUT is in flight), not a server problem.

- **Read:** [protocols/data.md](protocols/data.md) (publication, lanes),
  [performance.md](performance.md) (batching clause: no persistence boundary
  per chunk when one can cover many).
- **Gate:** the 8 GiB `dd+fsync` improves and stays byte-correct — remount,
  then verify head and tail bytes and the on-disk delta (≈ logical × 1.5 for
  2+1 EC). Plus posix suite 1 jobs=1 and posix suite 2 one pair, unchanged.
- **Forbidden:** inventing a REPORT split into multiple RPCs (that is an
  unmade wire decision — if the measurement says the batch itself is too big,
  **stop and ask**). Weakening `fsync` durability, dropping the flush, or
  reporting asynchronously to make the number look better.

#### W2 — 4-client and 9-client honest fio and dd

Leftover 1 is only ever gated at **1 client**. Nothing above 1 has been run on
the current tree, so "efs scales with clients" is unmeasured.

- **Do:** `bash tests/stress/fio_honest_matrix.sh results/perf/<id>-honest`
  (it already runs 1 / 4 / 9 hosts: 007, 007–010, 007–015) and the same
  8 GiB `dd+fsync` from 4 and then 9 clients, each to its **own** file.
- **Expect and report the shape, not just the total.** Writes have
  historically shared a ceiling (9 clients ≈ 1.5× one) while reads scaled
  6–8×. If 4-client aggregate ≈ 1-client, the ceiling is shared and W1's
  answer is the lever again. If it scales ~4×, then the 1-client limit was
  per-client CPU and that changes W1's conclusion — say so.
- **Read:** [performance.md](performance.md) §9 scaling envelope; the honest
  fio method in `.cursor/rules/efs-fio-honest.mdc`.
- **Gate:** every job logs `FUSE_OK`, no job's `Disk stats` names `md0`/`sda`
  (that is local disk = the run did not touch efs), `err=0`, and bytes written
  ≤ the on-disk `du` delta × EC factor.
- **Forbidden:** one shared file across clients (that is O_APPEND/RMW
  contention, not throughput). Quoting a run where any host failed the FUSE
  check.

#### W3 — `sw-50g` fails `end_fsync` with `EFS_ERR_NET`

A 50 GiB single-file write lays the data down and then times out in the final
flush: ~400 000 publications in one report versus `EFS_IO_TIMEOUT_MS`. This is
W1's wall at 6× the recs, so **do W1 first** — it may close this outright.
Re-run after W1 before touching anything.

- **Do:** re-run only the 50g row (`FIO_50G=50g`, 1 job,
  `--filename=big50`), and record whether it now completes.
- **Forbidden:** raising `EFS_IO_TIMEOUT_MS` to make it pass — that is
  widening a timeout instead of removing the work (§4). Splitting REPORT
  without asking.

#### W4 — fcstor005 lags because its group's snapshot does not fit — BLOCKED, ASK

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

#### W5 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation

Suite 1 jobs=1 is **193 both-pass / 3 EFS** (`results/posix/20260918-030811`).
One is `mmap_write_read`, an expected SKIP (`MAP_SHARED` is ENODEV by spec —
a documented deviation, not a bug). The other two are latency, not
correctness, and fail isolated too:

- `concurrent_creates_same_dir` — 160 creates in one directory.
- `mtime_monotonic_many_writes` — 80 × 128 KiB close-publish + `stat`
  (~187 ms per iteration).

Both are per-op metadata round trips. Profile one of them against the same
publish/ReadIndex path W1 touches; they may move for free once W1 lands, so
re-run them after W1 before optimizing anything.

- **Gate:** the two tests pass isolated inside budget, and suite 1 jobs=1
  does not regress below 193.
- **Forbidden:** raising `POSIX_TEST_SEC` or the `@budget(...)` values. A
  timeout is a failure to be removed, not re-labeled.

#### W6 — 9-node POSIX suite 1 hits the harness wall

Nine hosts running suite 1 concurrently all hit the 385 s python cap at
131–144 of 201 (`results/posix/20260917-191430`). Only 5–7 were real
walk/name timeouts; the rest never ran and report `[None]`. So the number is a
harness artifact and cannot be read as 60 bugs. Four-node under load is
186–188 both-pass (`results/posix/20260917-190014`).

Make a 9-way run produce a complete TSV (per-test result even when the run is
cut) so the suite reports what it measured, then re-read the real failures.

- **Forbidden:** reporting `[None]` rows as failures, or as passes.

#### W7 — RDMA empty-table first `mkdir` — needs a fresh table, so ASK first

The client connection-pool lifecycle fix is in tree and unit-gated
(`test_conn_fd`): a pooled conn pins its socket identity, checkout evicts on
mismatch, destroy refuses to close a recycled fd. The **live** repro was never
re-run, because it only reproduces on a freshly `mkfs`'d / effectively empty
table, and 19810 is populated. A remount there is *not* this gate.

Running it means wiping and re-`mkfs`ing a cluster. **Ask before doing that.**
Default transport stays TCP for the items above until it is gated.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping without being asked.

#### W8 — Sub-chunk overlapping writes are read-modify-write

Two clients writing overlapping byte ranges inside one 128 KiB chunk resolve
by generation CAS, so a loser's bytes can be lost even though both writes
returned. Suite 2 currently passes 63/63 including the straddle test
(`results/posix2/20260918-031332`), so this is not an open failure — it is an
**unstated contract**. Either write it into the POSIX contract as a deviation
alongside the other two, or bring it to the user as a decision.

- **Read:** [architecture.md §3](../architecture.md) (the deviation list is
  the one home — add there, do not restate elsewhere).
- **Forbidden:** claiming syscall-level multi-writer atomicity we do not
  implement.

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

#### W10 — Repo hygiene

`results/` holds ~45 untracked run directories. Decide per directory whether a
run is a gate worth committing (the ones cited in this file are) or noise. The
tracked `results/` history also still carries several hundred run directories
that measured the deleted snapshot/2PC engine — they benchmark code that no
longer exists and are recoverable from `git log`, so removing them is safe if
you want the tree small.

Run `make docs-check` after any doc edit: it regenerates
`architecture-full.md` and validates links plus every `I1..I25` reference.
`make test` must be fully green — there is no accepted-failure list.

---

**Out of scope** (do not start these; they are decisions, not tasks): C1
relaxed coherence; a pressure-triggered directory-spread bound (the bound is
unspecified); cutover of a 36T `efs-test`; any new REPORT or SNAP wire shape.

**Bigger than this queue.** [product-gaps.md](../product-gaps.md) inventories
what is missing before efs is a filesystem anyone could run — including three
things that contradict a guarantee the spec already makes (no fragment repair,
no protection-debt tracking, no session/fencing on the client). Those are not
queue items; each needs a design decision first. Do not start one without
asking, and do not treat the queue above as the whole distance to a product.

Everything the §10 steps delivered (10.5c-1..35d, step 11's deletion of the old
engine, step 12 parts A–D) is landed and gated; the per-increment narrative is
in the commit history and in `.cursor/rules/efs-project-state.mdc`, not here.

**When the queue above is empty,** the next task comes from a measurement, not
from this page: run the gates in [testing.md](../testing.md), and take the
largest gap between what a gate reports and what
[performance.md](performance.md) §9 says the hardware allows. If closing it
needs a design decision the spec does not contain, stop and ask (§4).

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
