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
Take the lowest-numbered item that is not marked done; correctness items
(W1–W2) come before every performance item. Each item names what to change,
how to measure it, what proves it, and what is forbidden. If an item
needs a decision the spec does not contain, **stop and ask** (§4); several
items below are blocked on exactly that and say so.

**The one live cluster is port 19810** on fcstor003–006 (`/data1/01–06/efs`,
`--quota 36T --direct-io`, TCP), clients fcstor007–015 at `/tmp/efs-mount`.
19820 is retired. Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs`
without being asked — several items below run on the existing data.

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
| 1-client honest write today | 209–448 MiB/s | **1.3–2.7 %** of the client's ceiling |
| 9-client honest write today | 924 MiB/s | **~2 %** of the cluster ceiling; 4.4× one client = 49 % scaling |
| 1-client honest read today | 3141 MiB/s | ~13 % of the client's ceiling |

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
| 1-client 8 GiB `dd bs=1M conv=fsync` | **448 MiB/s** (18.3 s) | `results/perf/20260918-dd-1c/` |
| 1-client honest fio 9×2g sw-1m | **924 MiB/s** | `hot-sw-1m-9job-fcstor007.txt` |
| 1-client honest fio 1-job 512m | 247 MiB/s | same |
| first honest matrix, 1 client | sw-1m 209 · ow-1m 196 · rw-1m 196 · rw-128k 194 · rw-4k 89 · sr-1m 3141 · rr-1m 2276 · rr-128k 1492 · rr-4k 144 | `results/perf/20260917-honest/` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| IO-500 IOR / mdtest | **never run** | harness exists: `tests/perf/io500/` |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

---

#### W1 — Shared-file (N-1) writes from two clients silently lose data

**This is the HPC pattern** — every rank writes its own region of one file
(IOR-hard, MPI-IO, HDF5 with one file per job) — and efs gets it wrong
without reporting an error. It violates **I12** and the protocol in
[protocols/data.md](protocols/data.md) ("Sub-chunk read-modify-write"),
which says: the writer reads committed base generation `B`, publishes with
`CAS(expected_generation = B)`, and on conflict refetches, re-applies its
byte-range patch and retries, so *disjoint ranges both land*.

What the code does instead, and why the CAS cannot fire:

- The client builds a partial-chunk write by fetching the 128 KiB chunk,
  patching its bytes in, and PUTting new fragments (`efs_fuse.c` writeback
  worker, `write.c` RMW base fetch). The only mutual exclusion is
  **process-local**: `efs_wb_ino_lock` + `wb_overlap_inflight` in
  `efs_fuse.c` ~2000–2030 serialize two RMWs of one chunk *inside one
  `efs-fuse`*. Two clients never see each other.
- The report record `struct efs_chunk_rec` (`include/efs/protocol.h`) is
  `{ino, chunk_index, nodes[], checksums[]}` — **it carries no base
  generation.** The client has no field in which to say "I patched
  generation B".
- So the server fills the CAS itself: `server_raft_host_report` →
  `p.expected_gen = got.generation` (`src/server/raft_host.c` ~8432), where
  `got` was read *moments before proposing*. The apply layer's
  `if (p->expected_gen != committed) return EFS_ERR_STALE` in
  `efs_meta_apply_publish` (`src/meta/meta_apply.c` ~2519) is real, but it
  is being handed the answer it checks against. It serializes two
  publications; it cannot notice that the client's 128 KiB was built on a
  chunk that has since moved on.

Timeline: A and B each hold chunk 0 at gen 5. A patches [0,4K), PUTs,
reports — committed becomes 6 (A's 128 KiB). B patches [4K,8K) into *its*
gen-5 copy, PUTs, reports — the host reads 6, CASes 6→7 with B's 128 KiB,
in which [0,4K) is still the gen-5 bytes. **A's write is gone and A was told
it succeeded.**

Why nobody has seen it as a failure: suite 2's `peer_shared_pwrite`
(docstring: "IOR-hard write") runs A **then** B sequentially, so B's base
already contains A. `peer_overlap_pwrite_partial` does run concurrently and
checks exclusive ranges, but two SSH-launched 4 KiB writes almost never
overlap in the ~ms RMW window — that is why it flips between PASS and the
"exclusive-range zeros" failure recorded in the project state. IOR-hard hits
the window continuously.

1. **Reproduce it deterministically first** (no MPI needed). On clients A
   and B, concurrently and with no barrier, loop 500 times: A
   `pwrite`s 4 KiB of `0xAA` at offset `i*8192`, B `pwrite`s 4 KiB of `0xBB`
   at `i*8192 + 4096`, each followed by `fsync`, all inside chunk-sized
   stretches of one pre-sized 64 MiB file. From a third client, remount,
   read the file, and count 4 KiB blocks whose content is not the owner's
   pattern. I12 says **0**. Record the count in `results/stress/<id>-n1/`.
   This becomes the gate and stays in `tests/posix/posix_2client.py` as a
   real concurrent test (replace `peer_shared_pwrite`'s sequential shape).
   Two facts that shape the fix (verified in the tree, do not re-derive):
   **(a) the client never learns a chunk generation today** — the
   `GETCHUNKS` reply is the same `struct efs_chunk_rec recs[]`, so one
   field added to that one struct carries the generation in *both*
   directions; **(b) the server drops per-record apply results on the
   floor** — the batched publish loop in `raft_host.c` (`apply_publish_cmd`,
   ~1339) does `(void)apply_one_publish(...)` and returns `EFS_OK` no matter
   what, so an apply-time `EFS_ERR_STALE` is invisible to
   `host_wait_applied` and the report replies OK. If you only do step 3
   without step 4 the CAS will fire and the test will still fail.

2. **Carry the generation on the wire.** Add `uint64_t base_gen` to
   `struct efs_chunk_rec` (`include/efs/protocol.h` ~575; `test_wire.c`
   round-trips it via `RT(struct efs_chunk_rec)` — keep it green). Then:
   - server `GETCHUNKS` fill, `server_raft_host_getchunks`
     (`raft_host.c` ~8818, right after `efs_meta_apply_get_chunk(... &ch)`):
     `out->recs[out->count].base_gen = ch.generation;`
   - client staging entry `struct efs_chunk_entry` (`include/efs/metadata.h`
     ~22): add `uint64_t generation`; `apply_chunk_recs` (`src/client/ops.c`,
     called from the `GETCHUNKS` loop ~226) stores it.
   - client dcache slot `struct dcache_ent` (`src/client/write.c` ~1300): add
     `uint64_t base_gen`. Set it where the RMW base is read
     (`write.c` ~1187: after `export_chunk_copy(ino, ci, &ce)` succeeds it is
     `ce.generation`; the `*from_zero_out = 1` branch is `0`). A **full
     chunk-aligned overwrite** (the `aligned` path in `efs_fuse.c` ~2022 reads
     no base) sets `UINT64_MAX` = "unconditional, last-writer-wins", which is
     legal for a whole-chunk write. Never use the sentinel for an RMW.
   - the slot must **keep its dirty ranges after the merge**: today
     `write.c` ~1891 does `e->nrange = 0; if (!have_base) dcache_add_range`.
     Track ranges for `have_base=1` too (if `DCACHE_NR` overflows, collapse
     to one range covering the whole chunk — then the retry is a full
     overwrite and needs no base). Without this the client cannot re-apply
     *its own bytes* onto a fresh base in step 5.
   - carry the slot's `base_gen` into the staging entry when the PUT lands
     (`dcache_put_now` → `efs_export_set_chunk`), and copy it into the report
     record where `crecs[cn]` is built (`write.c` ~505).
   `EFS_BUILD_ID` changes — restart all four `efsd` together, then remount.
3. **Check it on the leader, before proposing.** In `host_pub_pack`
   (`raft_host.c` ~8336; it fills `struct efs_meta_publish p` around ~8432):
   keep the existing idempotency short-circuit (identical `nodes`+`checksums`
   → `EFS_OK`) *first*, then
   `if (rec->base_gen != UINT64_MAX && rec->base_gen != got.generation) return EFS_ERR_STALE;`
   and `p.expected_gen = (rec->base_gen == UINT64_MAX) ? got.generation : rec->base_gen;`.
   This is a ReadIndex'd read, so the check is linearizable; the apply-time
   CAS remains the safety net for the propose→apply race.
4. **Make the safety net audible.** `apply_publish_cmd` returns the first
   non-OK `rc` from `apply_one_publish` instead of `EFS_OK` (that value is
   what lands in `arc_rc[]` and what `host_wait_applied` hands back).
   `server_raft_host_report` maps `EFS_ERR_STALE` to a new
   `#define EFS_INODE_RPC_STALE 11` reply status. The reply is a single
   status for the whole report (`struct efs_msg_inode_reply`) — **do not add
   per-record results to the wire**; a whole-report STALE is correct and
   cheap because the client can find the stale records itself in step 5.
5. **Retry on the client.** `efs_client_report_dirty` (`write.c` ~495)
   already merges the dirty set back on any failure. On `EFS_INODE_RPC_STALE`:
   for every ino in the failed report, re-run the `GETCHUNKS` pull (it now
   returns generations); for every dirty dcache slot whose `base_gen` is not
   `UINT64_MAX` and differs from the fresh generation, refetch the committed
   chunk, overlay the slot's kept ranges, PUT a new candidate
   (`host_pub_candidate_gen` is content-hashed, so new bytes mean a new
   candidate automatically), update `base_gen`, and report again. Bound it
   (8 tries); exhaustion is `EIO` on the `fsync`, never silent.
6. Run the step-1 repro again: **0** lost blocks under 500×2 concurrent
   writes. Then suite 2 one pair and 4 pairs, then `fio_honest_matrix.sh`
   1-client (the aligned sentinel path must not cost throughput).

- **Read:** [protocols/data.md](protocols/data.md) (sub-chunk RMW, candidate
  generations), [architecture.md §7.3](../architecture.md) and I12,
  [architecture.md §4](../architecture.md) I16 (retry idempotency).
- **Gate:** step-1 repro 0 lost; the new concurrent `posix_2client.py` test
  PASS 5/5; suite 1 jobs=1 and suite 2 unchanged; `test_meta_apply` and
  `test_wire` green (add a STALE-then-retry case to `test_meta_apply`).
- **Forbidden:** "fixing" it by locking the chunk across clients (a
  distributed lock on the hot-file path is the serialization point §0 P1
  forbids, and it is not what the spec says). Documenting it as a deviation
  — the spec already decided the opposite. Sending the `UINT64_MAX`
  sentinel from any path that read a base (it turns the CAS off and
  recreates the bug). Adding per-record status arrays to the report reply
  (a whole-report STALE + client-side refetch is the decided shape).

#### W2 — `write()` is specified as durable-and-visible; the code buffers — ASK

[architecture.md §3](../architecture.md) says four times that *a returned
`write()` is durable and visible to every client* (§3 "Data", §7.3, and
[performance.md](performance.md) builds its "no persistence boundary per
op" wording on it). The client does not do that: a `write()` lands in the
`efs-fuse` dcache (small writes are an in-place patch, larger ones go to the
writeback pool) and is published at `fsync`, `close`, or dcache reclaim.
That is why honest fio needs `end_fsync` at all, and why the fio rule
warns that a `time_based` run without it "measures memory bandwidth".

Two consequences, both currently undocumented:

- **Durability:** an acknowledged `write()` is lost if the *client* node
  dies before `fsync`/`close`. Storage-node failures are covered by §2; a
  client crash is not a §2 event, so this is a pure §3 contradiction.
- **Visibility:** a peer reading the range between A's `write()` and A's
  `fsync` gets the old bytes. This is close-to-open coherence — NFS
  semantics, not what §3 promises. MPI-IO codes that sync + barrier are
  fine; POSIX-coherence-dependent codes are not.

Buffering is almost certainly the *right* engineering choice for HPC (POSIX
does not require `write()` durability, and every production PFS buffers), so
the likely resolution is to **move the spec**, not the code — but that
changes a normative contract and is the user's call.

1. **Measure both halves.** (a) Visibility: A `pwrite`s 4 KiB and does *not*
   close; B `pread`s the same range 10× over 2 s — record whether B ever
   sees the new bytes before A's `fsync`. (b) Durability: A writes 64 MiB,
   `kill -9` A's `efs-fuse` before close; remount; read back — record how
   much is missing. Put both in `results/stress/<id>-w2/`.
2. Bring the numbers and the two options to the user: **(i)** spec says
   durable+visible at `fsync`/`close`, plus an `O_SYNC`/`-o sync` write-through
   mode for callers who need per-write durability; or **(ii)** code publishes
   on every `write()` (the fio numbers will drop, and W3's fsync tail becomes
   every write's tail). Do not choose.

- **Read:** [architecture.md §3](../architecture.md) (the contract and its
  deviation list — the one home), §7.7 FUSE, [protocols/data.md](protocols/data.md).
- **Forbidden:** editing §3 without the decision. Making `write()` durable
  "to match the spec" without being asked — that is a 10× throughput
  decision made by an agent.

#### W3 — Split the single-client fsync tail, then remove the larger half

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
  Report the result as a percentage of the 16.7 GB/s client ceiling.
- **Forbidden:** inventing a REPORT split into multiple RPCs (that is an
  unmade wire decision — if the measurement says the batch itself is too big,
  **stop and ask**). Weakening `fsync` durability, dropping the flush, or
  reporting asynchronously to make the number look better.

#### W4 — 4-client and 9-client honest fio and dd

Leftover 1 is only ever gated at **1 client**. Nothing above 1 has been run on
the current tree, so "efs scales with clients" is unmeasured.

- **Do:** `bash tests/stress/fio_honest_matrix.sh results/perf/<id>-honest`
  (it already runs 1 / 4 / 9 hosts: 007, 007–010, 007–015) and the same
  8 GiB `dd+fsync` from 4 and then 9 clients, each to its **own** file.
- **Expect and report the shape, not just the total.** Writes have
  historically shared a ceiling (9 clients ≈ 1.5× one) while reads scaled
  6–8×. If 4-client aggregate ≈ 1-client, the ceiling is shared and W3's
  answer is the lever again. If it scales ~4×, then the 1-client limit was
  per-client CPU and that changes W3's conclusion — say so. Report every
  aggregate as a percentage of the 44–57 GB/s cluster ceiling.
- **Read:** [performance.md](performance.md) §9 scaling envelope; the honest
  fio method in `.cursor/rules/efs-fio-honest.mdc`.
- **Gate:** every job logs `FUSE_OK`, no job's `Disk stats` names `md0`/`sda`
  (that is local disk = the run did not touch efs), `err=0`, and bytes written
  ≤ the on-disk `du` delta × EC factor.
- **Forbidden:** one shared file across clients (that is W1's territory, not
  throughput). Quoting a run where any host failed the FUSE check.

#### W5 — `sw-50g` fails `end_fsync` with `EFS_ERR_NET`

A 50 GiB single-file write lays the data down and then times out in the final
flush: ~400 000 publications in one report versus `EFS_IO_TIMEOUT_MS`. This is
W3's wall at 6× the recs, so **do W3 first** — it may close this outright.
Re-run after W3 before touching anything.

- **Do:** re-run only the 50g row (`FIO_50G=50g`, 1 job,
  `--filename=big50`), and record whether it now completes.
- **Forbidden:** raising `EFS_IO_TIMEOUT_MS` to make it pass — that is
  widening a timeout instead of removing the work (§4). Splitting REPORT
  without asking.

#### W6 — Run IO-500 (IOR easy, IOR hard, mdtest) for the first time

`tests/perf/io500/` has existed since Aug 20 and has **never produced a
result** (`results/` has no `io500/`). IOR and mdtest are the numbers an HPC
site asks for first, and mdtest is the only way this engine gets a metadata
rate at all — the 33k creates/s in old notes measured the deleted engine.
mdtest is currently turned off in `config-debug.ini`; turn it on.

Do this **after W1** (before it, IOR-hard with write-check would report the
lost updates — which is a valid way to demonstrate W1, but not a benchmark)
and **after W3** (before it, the write numbers only measure the fsync tail).

1. `bash tests/perf/io500/run.sh prereqs` on fcstor007; follow the README
   for the DOCA OpenMPI prefix (`mpi-env.sh`). No Slurm, no `yum install
   openmpi`.
2. Run `config-debug.ini` first (short stonewall) with 9 ranks × 1 per host,
   then 9 × 4. Enable IOR write-check (`-W`) on ior-hard for one run and
   record the mismatch count — it must be 0 after W1.
3. Record IOR-easy write/read, IOR-hard write/read (GiB/s), mdtest-easy and
   mdtest-hard create/stat/delete (kIOPS) in `results/io500/<id>/` with the
   ini and hostfile alongside. State stonewall time and rank count; these
   are not list submissions.
4. Compare IOR-easy write against the cluster ceiling table; compare
   IOR-hard against IOR-easy — the ratio is the shared-file RMW cost the
   spec accepts by design, and it should be reported as such, not hidden.

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check applies
  to every rank's mount (`findmnt` on all 9 before starting).
- **Gate:** a complete run on 9 hosts with every rank on `fuse.efs-fuse`;
  IOR-hard `-W` mismatches = 0.
- **Forbidden:** quoting any number from a run where a rank fell back to
  local disk; tuning IOR's transfer size to make IOR-hard look aligned
  (47008 is the point).

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
- **Whether `write()` is durable** — W2, above.
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
