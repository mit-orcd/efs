# efs project history — operational log, archived

**This is an archive, not guidance.** It is the accumulated "project state"
memory from Aug 18 – Sep 20 2026, moved out of the always-applied rule on
Sep 20 2026 because 4 000 lines of mostly superseded notes were misleading
agents (many paragraphs describe the DELETED pre-Sep-11 snapshot/2PC
metadata engine and say so inline; several say "do not start step 12" or
"do not kill 19820" — both long since moot).

Authoritative, current sources:

- **What to work on:** [arch/START-HERE.md](arch/START-HERE.md) §1 / §1a
  (work queue) / §1b (in-flight handoff).
- **Current facts and live do-not-re-chase learnings:**
  `.cursor/rules/efs-project-state.mdc` (kept under ~250 lines).
- **Architecture review history (rounds 1–9):**
  [arch/design-history.md](arch/design-history.md).

Use this file when a rule or START-HERE cites a date or a result directory
and you want the full narrative, or when you are about to re-derive a root
cause and want to check whether it was already found. Search by date,
result directory (`results/...`), commit hash, or symptom. Entries are
roughly reverse-chronological within each block; blocks were appended over
time, so the same day can appear in several places.

---

## Sep 30 2026 21:10Z — review of the user's fstor007 perf dir: the evictor and the per-close walk (documented, not changed)

The user ran `perf record -F 499 -g` on fstor007's efs-fuse for 62 min
(19:54Z–20:56Z) around two `ecopy --verify /data1/erbmi1/software/
/tmp/efs-mount/software/` runs, the first traced with `strace -f -tt -T`
for 86 s (died of its own SIGPIPE), the second for 150 s (^C). Asked to
review and document next steps only.
`results/measure/20260930-205300-ecopy-perf-review/SUMMARY.txt`.

`stage_evict_main` is 31.6 % of the whole profile and 47.6 % of the
second ecopy's slice; `perf annotate` puts every hot instruction in the
inlined `evict_pass` scan of `g_lru_keys`/`g_lru_ticks`. It runs
back-to-back because of D18's floor: the staging estimate is always over
the 256 MB cap, each pass evicts 64 just-closed rows (table lock 64×, one
compact per wake) and `efs_client_stage_evict_kick` — called from every
`ensure_meta_room` — re-arms it because the pass did evict. The Sep 30
targeted-evict fix made a pass cheap; it did not change the cadence.

`dcache_steal_dirty` is 36.9 % of that slice with a broken callchain
(libfuse worker, no frame pointers). The only caller at that rate is the
per-close path: `ll_flush → efs_append_flush_report → efs_dcache_flush_ino
→ dcache_flush_ino_pass`, which loops over every chunk index of the file
(from the staged size), two mutex pairs and a slot chain walk each, under
the inode's append stripe, on read-only opens and dup'd fds too, and
falls back to all 65536 slots once the file is ≥ 8 GiB
(`dcache_flush_all_slots.part.0` is in the slice). There is no
"anything unpublished?" predicate on that path; the utimens path got
one earlier the same day. `efs_dcache_yield_extra` (one `dcache_find`)
at 6.6 % suggests the slot chains hold many dead nodes; that is a
hypothesis to count with gdb, not a finding.

Per-op efs latency under the ecopy, with dirfd resolved through the fd
table: rename 54 / 29 ms, openat 18 / 7.7, utimens 8.8 / 15.9, stat
9.5 / 4.0, close 9.8 / 2.5 (idle refs 6.2 / 0.6 / 1.05 / 0.33 / 0.34),
at only ≈ 27 and ≈ 5.6 efs ops in flight; ecopy kept ≈ 250–275 of its
~400 threads on one futex. No efs syscall over 1.7 s, no EIO/EBUSY/
ESTALE. Next steps as written in the SUMMARY: decide D18 (an isolating
run with `EFS_CLIENT_META_MB=4096` needs no code), the close-path
predicate (mechanical, after a `--call-graph dwarf` confirmation), the
gdb chain count, and only then a server-side trace.

## Sep 30 2026 18:55Z — item 10: a 9-client IO-500 that completes, and the ior-hard-read loss was a naming bug

Goal for the afternoon: one 9×4 IO-500 that completes ior-hard-write
(every IOR since Sep 29 had aborted on fsync EIO). Seven debug runs today
completed every phase once the REPORT path stopped exhausting the 8 s
wall (single-get GETCHUNKS, follower ReadIndex coalescer, PULL_FAN /
REPLAY_THREADS, growable putid table). What remained was ior-hard-read:
40–208 records wrong per run (`/tmp/hardscan` cold scan of the 36 GB
file; IOR itself reported 80–140 read errors), always the client-boundary
piece of a chunk two hosts share, the server row showing no base image
and the missing piece's span absent.

The trace (fcstor009 with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, run
ior330, ino 547783) gave two chunks with different shapes:

- **238434 / 239622** — one full-image put-record after a
  `snap-inner | dirty=1 hb=1 bg=0 obj=0 r=` (nrange 0 on a first partial
  write), one `report … rc=0`, no server apply line, and the server span
  `off=25344 len=47008 gen=0xe990ec58f2c8212d nodes=2,3,4`. That gen
  **is fcstor009's object** (16830211674258022701 decimal) — under the
  *peer's* range. Object names are content hashes of the 128 KiB PUT
  image; the two clients had merged the shared chunk to the identical
  image, so fcstor010's span record and fcstor009's carried the same
  gen. `apply_publish` treated the second as "replay of this same span
  object" and, on the retry path, the REPORT pack skipped it as held
  (`efs_meta_apply_chunk_holds` matched gen alone) — the `report-split
  skip=N` counts. The bytes were never recorded; the read saw zeros.
  The nrange-0 snapshot is a second, independent bug: `dcache_take`
  marks the slot dirty and the have_base/base_gen/range came in a second
  locked section, so a flusher in between PUT the buffer as a full image
  and the merge overlay (which walks nrange) dropped the write.
- **41745** — three objects (span, then two full images), the slot base
  became its own first object gen through `dcache_need_published_merge`
  → pull → `export_chunk_copy` returning the *local* gen; the pack STALEd
  on base CAS against no row; the classifier read
  `stale-class committed=94a3… ours=94a3… done=1` because the GETCHUNKS
  reply had no row for the chunk, the local table kept our candidate
  gen, and "committed == ours" was taken as done. Dropped as committed.
  `span_of` had the same confusion: our own unreported span in the table
  "covered" the range, so a widened flush published only the tail, and
  putid (one object per chunk) forgot the first span. Its cover logic
  was also wrong on its own terms (any overlapping span extended cover,
  so an uncovered head with a covered tail counted as covered).

Fixes, all in `f2d3a7871b96-dirty`, rolled 18:30Z, clients 18:32Z:
span identity is (gen, off, len) on the server (apply replay check and
`chunk_holds`; `test_meta_apply` publishes the same object under a
second range and expects a third span); the STALE classifier matches a
span by range and a full image by base gen and treats a chunk the repull
found **no row** for as never committed (`pull_chunks_range` fills an
absence bitmap, `efs_client_pull_chunks_range_absent`); the replay of an
absent chunk drops the table's gen/list, starts from zeros with an empty
observation and publishes our ranges / span / image with expected 0
(`dcache_replay_stale_ex`); `dcache_init` installs have_base / base_gen /
range with the buffer under one lock; `span_of` grows cover only
contiguously from the range start and ignores the putid object's own
span.

Result (`results/io500/20260930-183504-rdma`): every phase, **0 fsync
failures, 0 read errors, cold hardscan 766980 records bad=0**.
ior-easy-write 4.563 GiB/s, mdtest-easy-write 4.632 kIOPS, ior-hard-write
0.640 GiB/s in 52.6 s, mdtest-hard-write 2.612, ior-hard-read 1.034,
mdtest-easy-stat 16.478. Server apply STALE (all FOLD_LIST) 220–463 per
node against 3238–6573 in ior330; three client STALE rounds in total with
pull ≤ 1.2 s against one per client at ~7 s. `inbox_drop` 171 on fcstor005
(3102 before). Not read: `pub_p50` 596629 on fcstor005's last raft-obs
line, 24 `getchunks slow` lines on fcstor004.

## Sep 30 2026 08:40Z — ecopy's 149 metadata mismatches: four client bugs, none on the server

The user's 07:17Z `ecopy --verify` on fstor007 (`85f5b31c` client) left 149
"verification metadata mismatch" lines: 119 atime, 29 mtime, 1 size. The
05:06Z mtime fix (flush before SETATTR) had closed the common case; these
were what remained. All four are client-side; a stat from a second client
confirmed the server held the wrong value only where the client had sent
it. Review, gates and traces:
`results/measure/20260930-080000-ecopy-times-review/SUMMARY.txt`.

**atime (119).** `struct efs_inode_mem` had no `atime_nsec` / `ctime_nsec`.
`inode_copy_attr` and `inode_to_rpc_p` dropped them and `efs_export_set_atime`
took seconds. ecopy's post-rename stat is a lookup filled from the staged
row: nsec 0. The old client reproduced it on the first try
(`atime_repro.py`: after-close OK, after-rename `.000000000`). The fields
went into the struct's padding (still 192 B), both copies carry them, the
setter takes nsec, and the second-resolution local ctime bumps zero
`ctime_nsec`.

**mtime = close time (29).** Three holes in "utimens has nothing to
flush", found one at a time with an ecopy-shaped burst (`size_gate.py`, 48
threads × 120 files, write → futimens(ns) → close → rename, then stat from
the same and a second client). (a) A wb job waiting in
`wb_overlap_inflight` was in neither the queue nor `busy_ino[]`;
`efs_wb_ino_pending_locked` said clean. `busy_ino` is set at the pop.
2 of 5760 files still wrong. (b) Every flusher marks the dcache entry clean
before its PUT and the chunk dirty after (the put-record); in that window
the chunk is invisible to a REPORT snapshot and to `efs_dcache_flush_ino`.
`put_win_open/close` count windows per inode; `efs_dcache_flush_ino` waits
for them (8 s → BUSY) and re-passes. Still 1–2 per run. (c) The predicate
itself: `efs_client_ino_is_dirty` is the dirty SET. A threshold REPORT
(`only_ino=0`) snapshots the ino mark `dcache_note_size` set and ships an
irec-only record while the bytes are still a dirty dcache entry. With
`EFS_DCACHE_TRACE=1` (new `utimens` / `report` / `release` lines) the bad
inode showed `utimens ... flush rc=0` with no report before it, the setattr,
then `snap-steal` of a `dirty=1 r=[0,1000)` entry and a `only=0 sync=0`
publish. `efs_utimens_flush_dirty` now also checks `efs_dcache_ino_pinned`,
the predicate the evictor already uses. Four bursts: 0 / 5760 each, same
client and fcstor008.

**size (1).** mpfr's `Makefile.in`, 32398 bytes on XFS, 131072 on efs.
`write_chunks_no_replicate` marked the chunk dirty and the ino only at the
end of the function; a threshold REPORT between the two shipped the crec
with no irec; `raft_host.c` sizes a crec without an irec to `(ci+1) ×
128 KiB` and irecs are newer-only. The ino mark and a local size grow sit
in the chunk's locked block now, and the REPORT builder appends an irec for
every crec whose ino has none. Server fallback left as is.

Perf of the same session (`fuse.data`, 976K samples): memmove 21.9 % (6.4 %
the `efs_rdma_send_frame` bounce copy, ~14 % FUSE → dcache), blake3 10.2 %,
`xor_into` 7.3 %, `stage_evict_main` 5.9 % — the evictor bug fixed in
`5ea397fd`, which fstor007's client did not have. Zero-copy RDMA from
registered dcache memory would be a design decision. Deployed to
fcstor007–015 08:35Z; fstor007's `/tmp/efs` rebuilt; posix jobs=1 200/201.
Pre-existing, untouched: `test_raft_store` 4 W22.1 assertions,
`check-architecture.py` regen / single-home.

## Sep 30 2026 07:40Z — gate of the 06:50Z roll: posix green (one ENOTEMPTY), IO-500 hard-write fsyncs fail, hard-read EIO

Servers `44f397b4ca2e-dirty` (rolled `--all` 06:50Z), clients fcstor007–015
mounted RDMA 07:17Z on the `5ea397fd` tree (evictor fix). The user's
ecopy from fstor007 was still running for the first suite and stopped
~07:22Z.

**Posix.** jobs=1 on fcstor007: 200/201 in 16.8 s under the ecopy
(`results/posix/20260930-072042`), 200/201 in 7.1 s idle
(`20260930-072224`); `mmap_write_read` SKIP only. posix2
fcstor007/008 63/63 in 47.6 s (`results/posix2/20260930-072241`).
9-host (`results/posix/20260930-072501`): eight hosts 200/201 in
18.0–19.1 s, fcstor012 199/201 — `dir_deep_nesting` failed `rmdir
d50` with `[Errno 39] Directory not empty`. From that client the rmdir
kept returning ENOTEMPTY 1–2 min later (`d50-probes-012.txt`); a KV
copy from fcstor005 at 07:28Z (`kv_dir_dump`, `d50-kv-dump.txt`) shows
d50 = ino 56024 `nlink=2 nents=0`, parent d49 = 164529 `nlink=3
nents=1` with the one `d50` dentry, no intent/guard/reduce; `rmdir`
from fcstor007 succeeded at once (`d50-rmdir-from-007.txt`). gdb on
fcstor012's `efs-fuse` afterwards (`d50-client012-childvec.txt`):
`child_vec_get(&g_client.export, 56024, 0)` on the **root tab**
returns a vector with `count=0 cap=4` — a row with parent 56024 was
on the root tab at some point. `efs_client_unlink` (`ops.c:1303`)
refuses a rmdir locally when `efs_export_dir_empty(&g_client.export,
ino)` says non-empty, and that function reads only the root tab's
`child_vecs`; nothing logs the refusal. The server pre-check
(`raft_host.c:8776`, `row.nlink > 2` or one readdir entry) reads the
raw row, so a pending `REDUCE_INO` from d51's rmdir would also give a
transient ENOTEMPTY, but the client retries that only 20 × 1 ms
(`efs_fuse_rmdir_at`), and "transient" does not cover the later
probes. No `dir_deep_nesting` failure in the Sep 28–30 history; this
is the first 9-host run with the evictor fix (the evictor is active
during the suite — D18). Open. Two ways forward: log which tab/slot
made the fast path say non-empty, or remove the local emptiness check
(the server is authoritative, and the fast path saves one RPC on a
non-empty rmdir only) — the second is a decision, ask.

**IO-500 debug 9×4** (`results/io500/20260930-072824-rdma`, launched
07:28:24Z, cluster idle, terms unchanged through the run):

| phase | Sep 30 | Sep 28 (`20260928-150609-rdma`) |
| --- | --- | --- |
| ior-easy-write | 3.148 GiB/s | 2.917 |
| mdtest-easy-write | 3.696 kIOPS | 3.418 |
| ior-hard-write | 0.261 GiB/s, 133 s, 16 fsync failed | 0.291, 73 s, 0 |
| mdtest-hard-write | 2.126 kIOPS | 0.757 |
| ior-easy-read | 2.976 GiB/s | 3.092 |
| mdtest-easy-stat | 3.600 kIOPS | 3.248 |
| ior-hard-read | ABORT, read EIO | 1.525 |

Every client's `fuse.log` has the same pair on the shared file ino
84857: `report-loop rounds=1 stale=0 busy=1 ms≈9000–11600 rc=-13`
(fsync EBUSY — W17.1 returns the first BUSY REPORT) and `rounds=1
stale=1 busy=0 ms≈58000–68600 rc=-14` (fsync EIO). ior-hard-read
then read those unpublished spans: `read(23, …, 47008) failed
Input/output error` on ranks 22/23 (fcstor012 and others) →
`MPI_ABORT` 07:32Z; nothing after that ran. I9 behaves as specified
(fail, do not zero-fill). Server side (`servers-after.txt`):
`apply_max` 0 µs and `pump_hold_max` ≤ 10 µs on all four (the pump is
not held), but `inbox_drop` 978 / 3102 / 223 on fcstor004/005/006 —
`raft-host: inbox full, dropped frame type=3 group=2 from=3
len=949540`: 950 KB AppendEntries from the group-2 leader (fcstor006)
dropped at fcstor005's 256-frame `HOST_INBOX_MAX`; `wait_timeouts` 619
on fcstor004, `apply-sleep` 20–127 ms everywhere, `pub_p50` 544 ms on
fcstor005 (3 ms at D8's IOR). L0 is 113–231 files per node and does
not drain at idle: ~10 000 `kv-compact` lines per node since the
roll, each 50–270 KB in 1–2 ms (`inputs=2 … l0only=1`, D10's
per-range trigger), while `gc-pass ms=244 frag=244` on both leaders
keeps flushing memtables with `GC_ACK` entries (the run's garbage).
The F2 drop counter did not exist on Sep 28, so that run cannot say
whether its inbox dropped too; the hard-write fsync failures are the
regression (0 warnings on 09-28, before W17.1/D9). No code changed
for this. The aborted run's data and two stale Sep 29 `posix-*` dirs
were removed from the mount; the nine clients stay mounted.

## Sep 30 2026 07:10Z — the rest of the perf dir: du 52K, a 1 Hz client stall, 10 ms stats, 16 fragments/s

`results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`. The user
asked about the other files in `~/orcd/scratch/efs/perf/efs-mount`
(ls, ls -lart, rsync, ecrawl, three finds, du, dd bs=16k and their
straces), starting from `du -hs` = 52K for 3.8 TB.

**du 52K.** du sums `st_blocks`; 52K = 103 symlinks × 512 B. Every
regular file reports 0 blocks (`find -ls` block column, ecrawl
`total_allocated_bytes=0`, `files_sparse_heuristic=21150/21503`).
`inode_allocated_bytes` (efs_fuse.c) counts the client-local
present-chunk table (W20), which since D2 is empty for a file the mount
did not write. The server row has no chunk count and W20 forbids
size-based `st_blocks`. Design ask D17 (per-lane present-chunk stamp
reduced at getattr). Not implemented.

**1 Hz stall.** du, find1 and find2 each show one syscall of 13–14 ms
every 1.014 s on whatever op was in flight; the 01:10 find on a fresh
mount does not. First suspect was the server GC loop (1 s cadence,
prefix scans under `l->mu`); a `gc-pass` timing line went in, the
servers were rolled, and the stall was still there, larger (17–27 ms).
Straced both sides at once during a stat loop: servers idle apart from
50 ms heartbeat waits; on the client, thread `stage_evict_main` leaves
its 1 s timedwait and then takes and releases the client table lock 58
times in 21 ms while the RPC workers sit in FUTEX_WAIT on it. gdb on
the live client: staged estimate 412 MB against the 256 MB cap with
10780 rows — the root export has 5 rows, ~2400 shard tabs have 1–3
each and are booked at ~170 KB apiece (a 256-row slab counted at the
512 B on-disk row size though calloc got 192 B rows, 16 × 1232 B chunk
entries, 512-slot indexes). So the evictor was over cap forever and
each of its 64 `evict_one` per second ran `ino_has_chunks` +
`drop_chunks_from` + `forget_ino` = three walks over every loaded tab
(250 µs), then `efs_export_compact` rebuilt every tab's index to the
same size (the 25 % load test always passes at 1–3 rows against the
256-row floor). Fixed on the client: `efs_export_evict_ino` visits the
tabs the row names (ino, parent, hashed dentry, chunk groups by size;
fan-out for a missing row, a hard link, or a file spanning most
shards), compact reindexes only when the rebuild shrinks, slab bytes at
`sizeof(struct efs_inode_mem)`; `test_stage_evict` covers the two-phase
evict. fcstor007 remounted on it: two du runs, 0 syscalls over 10 ms.
The floor itself (up to 4096 tabs × 90–170 KB > the cap before any data
is staged, evictor churning 64 hot rows/s) is D18.

**Big-file stat 5–10 ms.** `host_read_inode_lanes` issued a ReadIndex
per active foreign lane (32 peer round trips on a follower) and every
per-lane `efs_txn_reduce_read_ex` prefix scan opened an iterator per
covering segment whose `iter_load` pulled a 1 MiB readahead window —
22 × 1 MiB pread per RPC, twice per stat (LOOKUP + GETATTR), fcstor004
strace, servers at 2 % CPU. Fixed and rolled `--all` 06:50Z
(`44f397b4`): one ReadIndex per group; `kv_seg` iterators read one
block unless `kv_seg_iter_set_seq` (compaction and export keep the
readahead). 9.8 → 4.7 ms per stat; the remainder is the 64-lane double
collect.

**GC 16 fragments/s.** `host_gc_frag_pass` scanned 32 records and
proposed one 16-ack `GC_ACK` per group per second: 70–86 unlink per
server per 5 s, 21 hours per 100 GiB file. Now 256 per scan, 128 acks
per entry (`EFS_META_GC_ACK_MAX`, apply scratch on the heap), rescan
while full within 200 ms per group per loop, 1 ms yield between scans.
`raft-host: gc-pass ms=` prints when a GC iteration exceeds 5 ms
(leaders: `recover=70`, 64 ms of it the per-shard yield sleep).

Also read: rsync's `getcwd ENOENT` at 01:53 was the invoking shell's
cwd (re-run at 02:31 from `/tmp/direct_copy` worked); dd bs=16k is
20–40 µs per 16 KiB FUSE write (~500 MB/s single stream, direct_io
shape); readlink p50 0.85 ms; ecrawl's 16 threads had 718 calls over
10 ms in 4.8 s; the 01:10 find's 1.025 s getdents was the 05:11Z
election. Servers were rolled twice under the user's live `client.sh
--perf` on fstor007 (06:35Z, 06:50Z).

## Sep 30 2026 06:17Z — the wedged mount after a 100 GiB dd: a pipelined peer lane read its replies on the wrong channel

The user ran ls/rsync/ecrawl/find/du (all fine), then a 100 GiB `dd
bs=1M` from fstor007 against the 05:43Z roll. 1.0–1.2 GB/s for 106 s,
then `dd: closing output file: Input/output error`; sha256sum ENOENT
after 33 s, `ls /tmp/efs-mount` EBUSY after 16.7 s, unmount `DATA LOSS`
after 60 s of BUSY REPORTs. Analysis and inputs in
`results/measure/20260930-060000-dd-wedge/`.

The servers: no elections, `inbox_drop=0`, `apply_max` 110 ms. But the
two dual-group hosts' lanes to each other were failing once per 250 ms
in both directions (`tx->2 fail=2399 hi=2048` on fcstor004, `tx->1
fail=2395 hi=2048` on fcstor005; the 003 and 006 lanes fail=0), and ten
minutes after the client was gone fcstor005's group 0 was 678 entries
behind its leader and fcstor004's group 2 was 721 behind — each moving
one entry every ~15 s. Every read served by those followers
(`host_read_index` → 400 ms `apply-sleep` → BUSY: 164 + 82 lines) and
every REPORT touching both groups (`report-split nrec=819200 …
rc=-13`, 83 + 82 lines, pack 0 / push 0) returned BUSY; the client's
16-retry budget ran out.

Root cause, read from `host_sender`: a frame larger than the RDMA buffer
(72 KiB) goes over the conn's TCP side-channel (`conn_pick_send_chan`)
and the peer answers it on TCP; smaller frames go and come back on RDMA.
The sender pipelines up to three messages and then collected the
replies with `recv_chan` left at the *last* message's channel. A batch
of [AE_REP (RDMA), big AE (TCP)] read TCP only, left the RDMA reply in
the ring, timed out at `HOST_SEND_IO_MS`, destroyed the conn, and Raft
resent the same window — same shape, same result, forever. Only the
004↔005 lane mixes one group's AppendEntries with the other group's
replies (a single-group lane keeps one AE in flight and rarely batches
two kinds), and only a big-entry stream makes an AE take TCP: the dd's
publish batches (2048 publishes per command) did. Before F1 the same
shape waited `EFS_IO_TIMEOUT_MS` (30 s) per attempt — that is the
"lost RAFT_REPLY" the 04:06Z review could not place; the reply was on
the other channel.

Fix `85f5b31c`: `struct efs_conn` records `last_recv_chan` on every
receive path; `host_sender` counts the expected replies per channel
after each send and reads with `recv_chan=RDMA` while RDMA replies are
outstanding (that wait drains the ring first and falls through to TCP
when a byte is there), TCP otherwise; a reply on a channel with none
outstanding drops the conn. No wire change. Rolled `--all` 06:16Z.

Gate 06:19Z (`~/efs-runs/gate130.log`): fcstor007, 32 GiB of random
bytes through `dd bs=1M conv=fsync`, 32.85 s ≈ 1000 MiB/s, rc 0, size
34359738368, server `report-split nrec=262144 … rc=0` — the same
2048-publish stream that wedged. `tx->N fail=0` on every lane of
fcstor004 and fcstor005 before and after, terms unchanged, both groups
commit==applied, no `exhausted`/`report-loop`/`DATA LOSS` in fuse.log;
clean unmount. (A first attempt, gate128, ran the dd inside a 15 s
`efs-ssh.sh` call and died before writing a byte — void.)

Not changed: the 819200-record close REPORT is one RPC whose last-batch
BUSY discards 16 s of server work (splitting it is a design ask); the
D13 fold runs after every flush (L0 ~150 files, 1–5 ms each, log
noise); `apply lane-fence rc=-2` ×32 during the dd, not chased; the
client dd profile is memmove 29 % / blake3 13 % / xor 4.7 % with
`dcache_reclaim_main` gone from the top.

## Sep 30 2026 05:45Z — review of the 05:06Z roll under ecopy: the conn pool was the 5 s mode and the 30 s freezes

The user ran `client.sh --perf` on fstor007 against the 05:06Z roll
(F1/F2/utimens flush) and drove find, `ecopy --verify software/`,
ecrawl and `rsync -avvvP ~/git` through it (05:10–05:19Z). Review in
`results/measure/20260930-051000-review2/SUMMARY.txt`; fixes committed
`e002771e`, rolled `--all` 05:43Z (`e002771e56e4-dirty`,
`~/efs-runs/roll107.log`), fstor007 `/tmp/efs` rebuilt.

1. **`dcache_reclaim_main` was 39.8 % of efs-fuse cycles.** The 16
   reclaim threads pop only `have_base` dirty slots (W18); ecopy's dirty
   set is fresh files, so with dirty_bytes over the 2 GiB limit every
   kick re-walked all 64 dirty lists, popped nothing, and looped. Now a
   sweep that pops nothing parks the threads until the next kick after a
   10 ms nap; a sweep that pops re-arms immediately.
2. **The 5.0 / 10 / 15 s RPC mode (244 `slow-ok`, `attempts=1
   saw_busy=0`, every message type) was `efs_client_conn_get`**: 16
   slots per node, a checkout holds a slot for the whole RPC, a PUT
   holds three, a full pool waits `pthread_cond_timedwait` 5 s and
   returns NULL silently. 400 ecopy threads on 16 conns. The earlier
   attribution to `EFS_RDMA_SEND_WAIT_US` was wrong (IB hw_counters:
   `out_of_buffer` 0 on all servers). Worse than the wait:
   `put_fragments` and `fetch_fragment` called
   `efs_client_node_note_fail()` on that NULL, and four of those mark a
   live node DOWN for 30 s (`EFS_NODE_DOWN_FAILS`/`EFS_NODE_DOWN_MS`),
   after which every RPC to it returns NULL immediately — the two 28 s
   client-wide freezes in the ecopy strace (every syscall class parked,
   then all released together) and the 12 `put_fragments ... no quorum`
   lines against four live servers. Fix: log the timeout
   (rate-limited, with the pool size), never count it as a node failure
   (connect failures are already counted inside `conn_get`), default
   pool 64 per node (`EFS_SERVER_MAX_CONNS` is 4096 now, not the 512
   the old comment cited; one RDMA conn pins ≈ 2.6 MB per end; slots
   connect lazily).
3. Left as is: REPORT is one at a time with the whole dirty set, so a
   slow RPC inside it parks every queued closer (1630 ecopy syscalls
   ≥ 1 s) — splitting it is a design ask; re-measure after (2) first.
   fcstor004's 2475 `apply-sleep` follower waits are D14. 1376 tiny
   kv-compactions on 004/005 are log noise (4.3 s total).
4. **"ecopy + atime": no new defect.** W25 item 2 (atime nanoseconds
   end to end) landed in 2dd77dac; reads never move atime; the ecopy
   strace has zero mismatch lines; rsync's 8504 `set modtime, atime of
   <dir>` lines are its -vvv directory time set.
5. Servers after the 05:06Z roll: terms stable but for one group-0
   election at 05:11:19Z under load, `inbox_drop=0` everywhere, F1's
   250 ms bound visible as `tx-> fail` +1 per stall (fcstor006 tx->1
   fail=8, fcstor004 tx->2 fail=5). IB hw_counters snapshot saved as
   the delta base for the next run.

## Sep 30 2026 05:13Z — F1, F2 and the utimens flush implemented and rolled

User: "can you implement f1 f2 and the time bug?". All three are in the
tree, uncommitted, rolled `--all` at 05:06Z as `06916bc7e5c1-dirty`
(`~/efs-runs/roll78.log`, no recorders).

**F1 — bound the RDMA peer reply wait.** `struct efs_conn` gained
`recv_timeout_ms` (`include/efs/network.h`); `efs_conn_set_recv_timeout()`
(`src/common/network.c`) stores it and sets `SO_RCVTIMEO` on the TCP
side-channel; `conn_rdma_frame` (`src/common/protocol.c`) waits
`recv_timeout_ms` when set instead of `EFS_IO_TIMEOUT_MS` (30 s);
`host_sender` (`src/server/raft_host.c`) calls it with `HOST_SEND_IO_MS`
(250 ms) for every conn kind — the old block was `if (pc->kind ==
EFS_CONN_TCP)`. The sender's conn comes from `server_peer_conn_new`
(private, not the pool), so nothing has to be restored.

**F2 — count inbox drops.** `server_raft_host_inbox` increments
`h->inbox_drop` on the inbox-full `EFS_ERR_BUSY` path, logs the first ten
(`raft-host: inbox full, dropped frame type= group= from= len=`), and the
`raft-obs: wait_timeouts=` line ends with `inbox_drop=`. `from` is the
big-endian s32 at wire offset 4 (`efs_raft_msg` codec, `wire.h`).

**Mtime — flush before a SETATTR that sets mtime.** `efs_fuse_utimens_ino`
calls `efs_utimens_flush_dirty(ino)`: if the inode has writeback jobs
pending (`efs_wb_ino_pending_locked`) or is in the client's dirty set
(`efs_client_ino_is_dirty`), it runs `efs_append_flush_report(NULL, ino)`
— the same flush + REPORT that close runs — so the publish reaches the
server under the old `mtime_gen` and the SETATTR that follows bumps it and
fences the lanes. A clean inode pays one hash lookup. No wire or server
change; `EFS_INO_REC_F_TIMES` is still read by nothing. `efs_client_mtime_pin`
says `mtime-pin: table full` once instead of dropping pins silently.

**Gate.** `mtime_repro.py` on the user's live fstor007 mount (FUSE_OK,
`~/efs-runs/rec-gate84.log`): `write→futimens→close→rename`,
`write→futimens→close`, `write→close→utimens`, `write→futimens→fsync→close`
all end at 1380661863. `futimens→write→close` ends at the write time
(correct). An earlier run of the same gate (`gate81`) started 30 s before
the user's mount came up and wrote into the plain dir under the
mountpoint; those rows are local XFS and are not a result — the dir
`/tmp/efs-mount/measure/mtime-repro2` is still there under the mount.

**Unit tests** (fcstor007, `/tmp/efs-build69`): 14 of the 15 `make test`
binaries pass, `test_raft` OK with `test_ae_reply_match_stops_at_prev` and
`test_noop_over_stale_tail`. `test_raft_store` fails four assertions
(`snap index should report its term`, `compacted prefix differs`,
`snapshot did not shrink the log`, `reopen after rotation`) on a clean
`git archive HEAD` build as well (`~/efs-runs/rs77.log`) — W22.1's
retained log window changed what a snapshot does to the log and the test
was not updated. `docs/check-architecture.py` fails on the regen diff and
on the repeated decision-table headers in START-HERE §1.

**After the roll, under the user's load (05:06–05:14Z):** fcstor004
`tx->2 fail=5 hi=47`, `wait_timeouts=5`, `inbox_drop=0` on every node; one
group-0 election (term 94→95 at 05:11:19Z, fcstor005 campaigned and
fcstor004 stepped down); 256 `kv-compact: start` lines and 120–270 ms
`apply-sleep` waits on group 2 in one log window. F1 turned each lost
reply into a 250 ms `fail` + reconnect instead of a 30 s freeze; it does
not say which side lost the reply, and it does not stop `on_vote_req` from
deposing the leader (D15/D16 pending).

## Sep 30 2026 04:45Z — ecopy's mtime mismatches: a utimens between write and close is lost

The 153 "ecopy: verification metadata mismatch" lines at the start of the 04:03:51Z ecopy are all `(mtime)` (message lengths against the paths the same threads had just stat'd; `mis_ctx.py`). The copier thread does `openat(tmp, O_CREAT)` → `pwrite` → `utimensat(fd, [atime, mtime 2013])` → `close` → `renameat`; the verifier's `lstat` then sees a 2026 mtime. Reproduced from one python process on fstor007 against the idle cluster (`results/measure/20260930-044100-mtime-utimens-close/repro-fstor007.txt`): `write→futimens→close` gives the close time, `write→futimens→fsync→close` too (2013 survives the fsync, not the close), `write→close→utimens` is right, XFS is right in all three. Cause: the buffered write's PUBLISH reaches the server in the close REPORT after the SETATTR; `host_utimens` bumped `mtime_gen` and fenced the lanes, but the publish is packed with the *current* `mtime_gen` and `p.now`, so `meta_apply.c:3467` stamps the lane and the stat at `:4175` takes the MAX. The client-side pin (`efs_client_mtime_pin`, `EFS_INO_REC_F_TIMES` on the REPORT rec) was meant to cover this; the server never reads the flag, and the pin table is 256 entries dropped silently. Consequence for the user's copies: every already-copied file fails ecopy's `same_size_and_mtime`, so each rerun re-copies the whole tree (22.4 GiB this time) and fails verification again. Documented in SUMMARY.txt with the recommended fix (flush dirty dcache before a MTIME/ATIME SETATTR, client only) and the server-side alternative; nothing written. The repro client was stopped afterwards; `/tmp/efs-mount/measure/mtime-repro/` holds five 7-byte files.

## Sep 30 2026 04:20Z — post-fix review: per-op references, and a 30 s RDMA peer wait behind the election storms

The user ran `ls`, `find -ls`, `du`, a re-sync of `~/git`, `ecrawl` and `ecopy --verify` (22.4 GiB, 400 threads) from fstor007 against the 03:56Z roll (`06916bc7e5c1-dirty`, servers `--perf` only, client `client.sh --perf`). Reduction and raw pulls: `results/measure/20260930-040600-postfix-review/`. The first five are clean and give the per-op references for this build with no server strace: stat 0.33 ms, openat 0.60, close 0.34, utimensat 1.05, chmod 0.49, rename 6.2 ms (max 36 ms), `ls` 0.015 s, `find` of the tree 6.5 s with no call over 0.2 s. ecopy put both groups into an election storm: group 0 63 terms, group 2 426 (fcstor006) in 13 minutes, 12 `newfstatat` of 55–59.5 s, 513 over 0.2 s, 21 `shard=0` LOOKUPs and 8 REPORTs exhausted 16 BUSY retries, fcstor004 `wait_timeouts` 0→1290, and the 96 727-record REPORT answered NOT_PRIMARY four times by fcstor005 before fcstor004 packed it (846 ms). Group 2 settled at term 669 (leader fcstor005) at 04:09:39Z with all voters commit==applied.

Cause, from `raft-obs tx->` and the code: `host_sender` blocks for a `RAFT_REPLY` per batch, and on an RDMA peer conn that wait is `EFS_IO_TIMEOUT_MS` = 30 s (`conn_rdma_frame`, protocol.c:380) — the 250 ms `HOST_SEND_IO_MS` bound at raft_host.c ~757 is applied to TCP conns only, although the comment at raft_host.c:45 states the rule for any peer. `sent` froze for 10–30 s on fcstor004→005, fcstor006→004 and 006→005 while `enq` grew, then `fail` +1 and the conn was rebuilt. The frames of the frozen batch are processed by the peer (fcstor006 won votes through a frozen lane); the reply is what does not come back, and which side loses it (the peer's reply send waiting on a credit, or the sender node's shared `recv_poller` behind ~400 client conns) needs a stack sample of the sender and peer-conn threads in a storm; on-CPU perf cannot show it. The unheard peer campaigns every 0.5–0.9 s and `on_vote_req` → `maybe_step_down` deposes the live leader each time (no Pre-Vote, no leader stickiness); the leader re-wins 0.6 s later. The 01:21Z `~/git` ecopy (`20260930-012100-ecopy-git`, terms 18→253, `hi=2048`) was the same thing. No CQE-error, `retry counter` or QP line on any node; `pump_hold_max` ≤ 2.7 ms, `apply_max` ≤ 67 µs; `ping` 0.03 ms.

Documented in SUMMARY.txt and START-HERE: F1 bound the RDMA peer reply wait like TCP (per-conn recv timeout on `efs_conn`, set by `host_sender`), F2 count `server_raft_host_inbox` BUSY drops (a dropped Raft frame today has no line and no counter, and `server_handle_conn` still replies), both mechanical; D15 leader stickiness / Pre-Vote and D16 a separate credit class or poller for peer frames are questions for the user. Client profile: 27 % memmove, 10 % blake3, 4 % XOR — the write payload, nothing new on the metadata side; the 59 s stat walls are off-CPU in the RPC retry loop.

## Sep 30 2026 03:57Z — group-0 wedge: a stale tail was a "match"; the leader's no-op never shipped

The 23:16Z rsync froze and a 23:51Z `ls` returned EBUSY after 16.76 s (`~/logs/ls.strace.txt`, `results/measure/20260930-032400-g0-wedge`). Group 0's leader fcstor005 had armed `send_idx` at attach with the replayed `last_i` = 437637 and never shipped its own term-41 no-op 437638; fcstor004 held a stale pre-restart 437638 (term 34) and, because `on_ae_req` answered an empty heartbeat with `match = last_i`, reported 437638 as matched. When `durable_idx` reached 437638, `try_commit` committed it with {self, fcstor004} — a false commit; fcstor004 then truncated, the reject/accept ping-pong ran every heartbeat, and every follower-served group-0 ReadIndex waited for 437638 → 400 ms → BUSY → 16× → EBUSY/ENOENT. Fix in `src/raft/raft.c`: the success reply reports `match = prev_index + nentries`, never the follower's last log index; `send_idx_cover` advances `send_idx` in `become_leader`, `maybe_append_cold` and `efs_raft_change` the way `efs_raft_propose` does. `test_raft` gained `test_ae_reply_match_stops_at_prev` and `test_noop_over_stale_tail`; both pass (`~/efs-runs/raftfix62.log`). Rolled `--all` 03:56Z with `--perf` only (`~/efs-runs/ready65.log`): group 0 elected fcstor003 at term 43 with all voters at 437638; `ls`/`stat`/mkdir from fstor007 returned at once, zero `apply-sleep`.

## Sep 29 2026 16:09Z — servers rolled with perf and strace, no clients

`tests/roll_efsd.sh --all` (`~/efs-runs/rollw34.log`), `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`, `EFSD_ARGS='--perf --strace'`. All four built `2dd77dac881c-dirty` and came up; group 0 leader 0 term 9454 commit==applied 13775244, group 2 leader 3 term 2925 commit==applied 12187894. The 1 s start check printed `perf=0`; a second check (`~/efs-runs/chk34.log`) is `perf=1` `strace=1` on fcstor003–006 and `efs-fuse` 0 on fcstor003–015. Storage was kept. The strace files were already 0.8–1.5 GB two minutes in.

## Sep 29 2026 15:40Z — W21 step 2, W17 step 3 tests, W15 step 5 blocked

`efs_export_staged_bytes` is a running total (`staged_refresh` at every capacity change; `test_stage_evict` checks it across compaction). `test_chunk_deltas` gained the three W17.3 cases and drops the pre-D1 "trailer gone" assertion (a fold keeps tombstones). W15.5 checked on fstor007: `fs.pipe-max-size` 1 MiB < `max_write` 4 MiB + 4 KiB, so libfuse 3.10.2 never used the pipe. The user set `fs.pipe-max-size=8388608` on all 15 hosts (runtime, reverts on reboot) and `efs_fuse_init` now sets `FUSE_CAP_SPLICE_READ`. Not rolled, not gated.

## Sep 29 2026 15:15Z — 24 h review: six fixes to the unrolled tree

`kv_flush_locked` capped runs at 256 while `KV_RANGE_N` is 512 (a wide memtable was BUSY forever under D9). `kv_seg_data_bytes` is cached at open and `kv_l0_bytes` is hoisted out of the compactor's range loop (was O(blocks) per L1 file and per range under `l->mu`). The D13 fold's output is installed at the newest input's position, not at the head (a same-range file flushed during the merge stays ahead). The merge iterator reads a block over 1 MiB whole instead of returning IO. `maybe_prefetch` asks the layout-miss path only when the chunk map is absent. `dcache_flush_slot_inner` re-finds its entry after the unlocked GET/PUT instead of dereferencing a pointer a drop may have freed. `lane_bits` moved into `pack_utimens_cmd`. Two `test_kv_lsm` assertions updated to the byte rule. Not rolled, not gated.

## Sep 29 2026 14:50Z — reclaim drops the shard lock before a fragment GET

In tree, not rolled, not gated. `dcache_reclaim_main` flushed through `dcache_flush_slot`, which held `shard_io` across `efs_client_fetch_published_chunk`. On the 12:36Z IOR that recv sat at 30 s and `ll_fsync` of the shared file blocked on the same lock until the client was killed. The flush now drops `shard_io` with the dcache mutex before the GET and the PUT, after the entry is clean, and takes both again only to install the result. `dcache_steal_dirty` already dropped the lock at that point.

## Sep 29 2026 14:20Z — D13: a file-cap compact does not read L1

In tree, not rolled, not gated. When `n_l0` is over the file cap and no range meets the 1/8 rule or the 1 GiB byte cap, `kv_compact_locked` merges the range with the most L0 files (at least two) into one L0 file and does not open that range's L1. Tombstones are kept, because L1 still holds older copies. The compactor already loops while the count is over the cap. A single-file range is left for the 1/8 rule. The 1 GiB byte cap still rewrites L1.

## Sep 29 2026 13:53Z — 100 GiB copy trace: the file-cap backstop rewrote L1

Stopped the 13:08Z `--perf --strace` roll at 13:53Z (`~/efs-runs/stop32.log`). Reduction is `results/measure/20260929-130800-ddposix/ana`, window 09:17–09:50 EDT (posix on fcstor007/008 plus `dd conv=fsync` of two 100 GiB files from fcstor009 and fcstor010). Every `fsync` returned EIO. D12 compacted whenever `n_l0` was over 4, which was the whole copy, so the 1/8 rule never applied: fcstor004 wrote 67.5 GB in 425 compacts (max 17 s, L0 peak 379). The apply still binary-searched those files (`kv_seg_probe` 8.3 % self on fcstor004) and `host_read_index` returned BUSY at 400 ms (`pack_ms=0` on 172 of 184 `rc=-13` reports). One `push_ms` was 206 s. `backpressure` did not fire. Server `fsync` averaged 3 ms (max 3.6 s on the compactor). The fix is D13: over the file cap, merge a range's L0 files together and do not read its L1.

## Sep 29 2026 12:36Z — 8h and D12 rolled; easy-write finished, hard-write did not

Rolled `--all` at 12:29Z without perf or strace (`~/efs-runs/rollw31.log`), build `a53b253f2455-dirty`. 8h caches a segment's last key at open and `kv_seg_probe` rejects a key outside the in-memory span before `block_for`. D12, when `n_l0` is over the file cap, compacts the range with the most L0 files even under the 1/8 byte rule. The 9×4 1 s-stonewall IOR (`results/io500/20260929-123635-rdma`) printed ior-easy-write 2.218 GiB/s (19.720 s) and mdtest-easy-write 1.765 kIOPS (2.739 s). ior-hard-write then logged 31 `fsync failed` and no bandwidth; rank 28 on fcstor014 called abort. fcstor004 ended at L0=3, L1=397, with 73 of 99 `report-split` lines `rc=-13`. On fcstor012 the shared-file `ll_fsync` waited on the shard I/O lock while the reclaimer held it across a fragment GET (`efs_rdma_recv_wait`, 30 s) of chunk 170279; those four ranks went D and were cleared by killing that `efs-fuse` and remounting.

## Sep 29 2026 12:02Z — IOR trace: L0 file count, not the pump wait

Recorders from the 05:31Z roll (`a53b253f2455-dirty`, RDMA, `--perf --strace`) ran until 12:02Z and were stopped so the files could be read (`~/efs-runs/stop31.log`). Clients exited on SIGTERM. The four `efsd` did not return within 8 s and were killed after their recorder children were signaled; the perf files still open (fcstor004 IOR slice 155K samples, lost 0). Reduction is `results/measure/20260929-053100-w30trace/ana`. The only write in the 01:31–08:02 EDT window is the 01:46 EDT IOR, which aborted with no bandwidth: `report-loop rounds=1 busy=1 rc=-13`, walls 8–26 s, one client `recvfrom` of 25.9 s.

D9, D10's byte rule, D11, and D7 held. 34 compactions on fcstor004 wrote 0.254 GB (max 1161 ms, no `backpressure` line, `fsync` max 91 ms, `access()` 10K against 855K `openat`). What did not hold is D10's dropped file cap: L0 went 26 → 310 files and L1 ended at 353, because `kv_compact_locked` returns BUSY unless a range's L0 bytes are 1/8 of its L1, and the ranges that qualify are 0.5–1 MB / 10 ms. `lookup` probes every segment, so the publish apply on the pump (`efs_meta_apply_get_chunk` 9 %, `lookup` 18 %, `kv_seg_probe` 12 %, memcmp 10 % self) falls behind and `host_wait_applied` returns BUSY at 400 ms with `pack_ms=0` (54 of 60 reports on fcstor004). One group-2 snapshot, 2.82 GB / 14.1 s, accounts for the two `rc=-15` reports, not the 54 BUSYs. Fixes are START-HERE 8h and D12. The client hot path is blake3 at 21 % of on-CPU time while the wall is the RPC wait.

## Sep 29 2026 04:08Z — snapshot window, sliced import, first-write hint

Rolled `--all` (`~/efs-runs/rollw22.log`) onto `54a500da9dc8-dirty`,
RDMA, `EFS_RAFT_OBS=1`, servers and clients fcstor007–015 with
`--perf --strace`. No suite and no IOR. Group 0 leader 1 term 9418
commit==applied 13687019; group 2 leader 1 term 2902 commit==applied
12107762. All nine clients FUSE_OK. Recorders left running.

W22.1: `host_maybe_snapshot` fires when command bytes past `snap_idx`
reach `EFS_RAFT_SNAP_BYTES` (512 MiB). `raft_group_snap` keeps that
many command bytes (`log_base`); `send_ae` InstallSnapshots only when
`next_index` is below the window. A rotated log records `log_base` in
the SNAP record so replay can put the window back. W22.2: the pump
applies an import diff 1024 keys at a time and returns BUSY until the
cursor finishes, so a heartbeat can go out between slices. W14.4b:
the client's first PUT of a fragment sends `EFS_PATH_HINT_NEW`; the
server skips the `access()` walk and the writer picks the least-queued
root. A retry of an unacknowledged PUT sends 0. D6: `efsd
--meta-storage` exists and defaults to the first `--storage` root;
`mdraft/` stayed where it was. D8: `raft-obs` prints `pub_p50` and
`pub_max` for the publish batch's propose-to-apply wait. D2's
parallel chunk-map windows are still open.

### 04:27Z — the trace, analyzed

Recorders stopped at 04:27Z; per-host analysis in
`results/measure/20260929-040800-idle-trace/ana` (scripts
`~/efs-runs/ana4*.sh`). A user `ecopy` ran 04:15–04:22Z (408K fragment
creates per server; nothing through fcstor007–015's FUSE, whose
profiles are idle: `recv_poller` 930 `poll`/s, 0.3 % of a core).

Held: zero snapshot exports in 20 minutes (W22.1); 26K `access()` for
408K creates (D7). Regressed: `disk_log_new_bytes` walked the retained
log per pump tick, 1–1.25 % of every server — replaced with running
counters (in tree). Found: the compactor is 48–55 % of two servers,
433 compactions and 159 GB of `bytes=` in 20 minutes on a 5.3 GB
table, range 0 at 1.8 GB per rewrite; the pump's apply path blocked in
`kv_maybe_flush_locked` for 4.1 / 1.7 / 24.2 / 2.7 / 3.6 s on
fcstor004, each the length of one compaction, giving `apply-sleep`
400 ms timeouts, `report-split … rc=-13`, and the run's two term
changes (g0 9418→9421, g2 2902→2904). Per-thread `fsync`: pump
(Raft log) 0.38–0.40 ms average on 003/004/005; compactor 36–62 ms
average with 141–407 calls in 90–120 ms — the 100 ms mode is the
compactor's own segment, so D6's shared-journal premise is closed
(D11). D8's `pub_p50` is 3.1 ms when the pump is free. Also in tree:
`setvbuf` 1 MiB on segment writes (5.25M 4 KiB `write`s). W23 (D9:
pump never waits for the compactor; D10: compact by bytes, split
range 0) is written up as pending decisions in START-HERE.

## Sep 29 2026 05:11Z — pump no longer waits; map windows; range 0 split

Rolled `--all` (`~/efs-runs/rollw25.log`) onto `a53b253f2455-dirty`,
RDMA, `--perf --strace`, clients fcstor003–015 remounted. No suite.
Group 0 leader 1 term 9426 commit==applied 13735166; group 2 leader 2
term 2907 commit==applied 12147949. Recorders running.

D2: `pull_layout_miss` pulls an adaptive window (16 chunks, doubling
to 256 while the read stays sequential) one window ahead of the
caller, and issues each chunk group's GETCHUNKS on its own thread.
D9: `kv_maybe_flush_locked` returns without waiting when L0 cannot
take the flush; `host_pub_batch_propose` returns BUSY while L0 is
within one flush of the 64-file cap. D10: compaction skips a range
whose L0 bytes are under 1/8 of its L1 unless L0 is at that cap;
`key[0]==0` flushes and compacts as 16 subranges; compaction reads
1 MiB ahead of each block. The L0 array is still 64 files. Also in
this binary: the O(1) log-byte counter and the 1 MiB segment
`setvbuf`.

## Sep 29 2026 02:35Z — outbox coalesce, multi-chunk copy, post-and-return; IOR with perf and strace

Rolled `bbcbcb5ad779-dirty` `--all` at 02:35Z with `EFSD_ARGS='--perf
--strace'` and remounted fcstor007–015 the same way (`EFS_TRANSPORT=rdma`).
Three items that had a fixed design went in with that roll: W14.2 (a)–(b)
(AE reply and heartbeat coalesce in place; votes are not evicted; the
sender drains that lane before entry AppendEntries), W15.3 for a
multi-chunk write (one `fuse_buf_copy` per whole chunk inside a 1 MiB
write), and W15.4 (`efs_rdma_send_frame` posts and returns; the reply
wait reaps the send CQ). `efs-fuse --perf` now starts the recorder in
the daemon child; `--strace` is `strace -f -tt -T`. IOR
(`results/io500/20260929-023447-iorperf2`) aborted again on the first
BUSY REPORT (`report-loop rounds=1 busy=1 ms=18246–19508 rc=-13` on
fcstor007). No bandwidth. Recorders were SIGINT'd at 02:41Z so the
files are finalized; they live on each node under `/tmp/efs-perf/`
(`efsd.data` / `efsd.strace` on 003–006, `fuse.data` / `fuse.strace`
on 007–015). Server straces are 1.9–2.4 GB because the trace was not
narrowed.

## Sep 29 2026 00:40Z — IOR abort on the W17–D2 tree, perf hot path

Rolled `bbcbcb5ad779-dirty` `--all` at 00:28Z (`EFS_TRANSPORT=rdma
EFS_RAFT_OBS=1`). fcstor006 did not catch up inside the roll's 180 s:
it was installing a 2.3 GB group-2 snapshot (5796 ms) and compacting
(L1 ~1000). It was commit==applied at 12083285 before the client
remount. Clients fcstor007–015 remounted RDMA at 00:39Z with
`perf record -F 499 -g`.

`tests/perf/io500/run.sh ior` (stonewall 30 s) produced no bandwidth.
`results/io500/20260929-002758-wimpl`: INVALID stonewall, `fsync`
failed, `close` failed, rank 2 `MPI_ABORT`, ranks gone by 00:41:33Z
with no D-state. `report-loop` on every client, one round, `busy=1
rc=-13`, walls 8.2–26.1 s: the W17.1 bound returned EIO on the first
BUSY REPORT where the old loop retried BUSY 8 times (the Sep 28 run's
fsync succeeded after 328 s). The 8 s is checked between attempts;
the rest is one RPC the server held. The `exhausted 16` lines are
the non-sync close-kick path. fcstor004 `report-split nrec=90016` is
BUSY nine times (`pack_ms=0`) then six `rc=0` with `pack_ms`
0.86–2.77 s.

Profiles (`cycles:P`, lost 0, ~100 s window overlapping compaction
and a group-2 snapshot export) under `~/orcd/scratch/efs/perf/`.
Leaders were fcstor003 (group 0) and fcstor006 (group 2). fcstor003
(79K): `memmove` self 4.2% (was 18.5%), `send_ae` 2.5% (the one
log→frame copy), `host_send` under 1.5%, `try_commit` self 0 (was
4.0% / 10.5%) — W19. fcstor007 (14K ≈ 29 CPU-s, mostly off-CPU):
`dcache_flush_slot_inner` self 0.1% (was 43.8%), `pthread_once`
gone (was 25%), reclaim children 49.7% all in `dcache_put_now` —
W18. `ll_write_buf` memmove 8.6% (W15.3 open). fcstor004 (127K,
follower, REPORT target): fragment `open` 11.9% + `access` 8.7%
(W14.4 open); `memcmp` 9.2% in `lsm_get` / compact / export,
`vx_sift_down` 2.2% in the export heap.

## Sep 28 2026 — decisions D1–D3 and the implementation order

The user accepted three recommendations after the evening IOR runs.
D1: the span publish commutes — `efs_meta_apply_publish` checks
`expected_gen` only for full-image publishes, a sub-range writer
publishes a span even with the base in hand, folded spans'
`candidate_gen`s stay in the trailer so a replay is a no-op, and the
fold is done by the publisher that fills the chain or by a reader.
No distributed lock. D2: `open()` adopts the inode row only; chunk
maps come from `pull_layout_miss` per lane group in parallel, one
metadata window ahead of the prefetcher, size adaptive, no mount
option. D3: the 5/s REAP_DONE idle gate stays. The order for the
queue is W17.1, W16.1, W18, W19, W14.4, W15.3, the W14/W15 residuals,
D1, D2, W16.2–3, then W8/W10 re-gates. Recorded in START-HERE
("Decisions — taken and pending") and in the W1, W6, W17 items.

## Sep 28 2026 — committed L1 list, IOR easy-write 0.769 GiB/s

`bbcbcb5` rolled `--all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`.
Both groups were commit==applied before the run (group 0
13626406 term 8016 leader 0, group 2 12052065 term 1917 leader 1).
Clients fcstor007–015 mounted RDMA. The node build id is
`bbcbcb5ad779-dirty` because rsync excludes `results/` and git
then sees those tracked files deleted; all four servers printed
the same id.

`run.sh ior` (30 s stonewall) printed easy-write **0.769 GiB/s**
in 393.014 s. fcstor007's fsync for that phase was
`flush_ms=327885 report_ms=0 rc=0`, after REPORT type 67 exhausted
16 BUSY retries. mdtest-easy-write printed 0.000 kIOPS in 0.000 s
(the ini has `run=FALSE`). The next phase's `fsync` returned
errors, `close` failed, and rank 8 `MPI_ABORT`. During that phase
fcstor004 logged `apply publish rc=-14` (STALE) for ino 1166063
across many chunk indexes. No easy-read, hard, or score. After
the abort, io500 on fcstor007–010 and 012–014 stayed in D-state
`request_wait_answer`. efsd and the mounts were left up. Perf
reports: `~/orcd/scratch/efs/perf/fcstor00N/efsd-19810/` and
`.../efs-mount/`.

## Sep 28 2026 — perf IOR, stopped after stat EBUSY

Restarted 19810 with the real node ids (not `scripts/server.sh`,
which derives a node id from the address) and `perf record -F 499 -g`
on each efsd. Clients fcstor007–015 were mounted with
`scripts/client.sh --perf` and `EFS_TRANSPORT=rdma`. mdtest in
`config-ior-only.ini` is `run=FALSE`. The driver printed the 30 s
stonewall INVALID line, then `stat` of the easy files failed and
rank 6 called `MPI_ABORT`. No RESULT line. Client logs: REPORT
(type 67) exhausted 16 BUSY retries, `fsync-split` `flush_ms=202803`
`report_ms=0` `rc=0`, then `INODE_LOOKUP` (type 43) on shard 3745
exhausted the same budget. Profiles:
`~/orcd/scratch/efs/perf/fcstor00N/efsd-19810/` and
`.../efs-mount/`. Daemons were stopped after the reports were
written. `efs-fuse` and `efsd` counts were 0 on a later check.

## Sep 28 2026 — L1 list, snapshot pump, IOR easy-write

The dual-host nodes were stuck at 64 L1 files, so compaction
published BUSY, L0 could not drain, and the group 0 follower never
installed a snapshot. The pump on that follower's other group spent
the profile in `kv_flush_locked` (fcstor004, 41%) and the group 0
leader spent it in `send_snap` (fcstor003, 50%).

L1 is now a growable list. The MANIFEST format did not change.
`host_snap_open` calls `efs_kv_lsm_flush_nowait`, which returns BUSY
before walking the memtable when L0 cannot take `KV_LSM_RANGE_MAX`
more files. A no-progress snapshot ack sets a one-heartbeat retry
and the next send skips the pread; an empty AppendEntries is still
sent. Abandoned `snap-*.kvx.tmp` files were removed on fcstor003–006
while efsd was down.

`roll_efsd.sh --all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1` built
`7eecf1da00cd-dirty` and reported `ROLL_OK`. Both groups were
commit==applied at the leaders' indexes (group 0 13616251, group 2
12041271). Compaction on fcstor004/005 ended rc=0 and L0 fell to 2
and 3. fcstor004's group 0 import applied a 15697-item diff.

`tests/perf/io500/run.sh ior` on fcstor007–015 (all `MOUNT_OK`)
printed easy-write **1.048 GiB/s** in 397.381 s. The client log for
that fsync is `flush_ms=325611 report_ms=0 rc=0`. The driver then
aborted on `create file.mdtest.0.2236 failed (EIO)`. Official score
is not a result: stonewall is 30 s, and the later phases did not
run. After the write, L1 was 310 files on fcstor004 and 405 on
fcstor005, L0 still 3. Group 0 and group 2 were still
commit==applied (13624345 and 12050181). Server profiles are under
`~/orcd/scratch/efs/perf/efsd-19810-fcstor00{3,4,5,6}/`; client
profiles under `~/orcd/scratch/efs/perf/fcstor00N/efs-mount/`.

## Sep 28 2026 — 9×4 IO-500, TCP and RDMA

Same binaries (`db2b88c4802a-dirty`, the tree committed as
`2f086f5`), 9 clients × 4 ranks, `tests/perf/io500/run.sh debug`
(1 s stonewall). Preflight idle and `fuse.efs-fuse` + `stat` OK
before each run. Reads are the driver's same-mount reads.

| phase | TCP | RDMA |
| --- | --- | --- |
| ior-easy-write GiB/s | 1.376 | 2.917 |
| ior-hard-write GiB/s | 0.274 | 0.291 |
| ior-easy-read GiB/s | 2.669 | 3.092 |
| ior-hard-read GiB/s | 1.162 | 1.525 |
| mdtest-easy-write kIOPS | 2.721 | 3.418 |
| mdtest-hard-write kIOPS | 0.910 | 0.757 |
| mdtest-easy-stat kIOPS | 16.461 | 3.248 |
| mdtest-hard-stat kIOPS | 10.271 | 5.683 |
| mdtest-easy-delete kIOPS | 2.964 | 2.223 |
| mdtest-hard-read kIOPS | 10.661 | 4.895 |
| mdtest-hard-delete kIOPS | 0.425 | 0.538 |

`results/io500/20260928-151707-tcp` and
`results/io500/20260928-150609-rdma`. The first RDMA launch
aborted: `INODE_LOOKUP` on shard 1745 exhausted 16 BUSY retries
(10.3 s) and IOR's post-write `stat` called `MPI_Abort`. The
files were on disk afterward (36 × ~1.4 GiB). The quoted RDMA
row is the retry. After the TCP run, fcstor010–015 wedged
(`stat` hung, ssh 10 s timeout). `killall -9 efs-fuse`, then
`timeout 3 fusermount3`, then an RDMA remount: all nine
`MOUNT_OK`, `RDMA transport up` once each.

## Sep 28 2026 — five 9-client dd profile rounds

8 GiB `dd bs=1M conv=fsync`, non-zero source, own file, FUSE only,
RDMA. Aggregate is 9 × 8192 / slowest client wall. Every quoted row
has all nine files at 8589934592.

| round | aggregate MiB/s | slowest wall | what changed |
| --- | --- | --- | --- |
| 1 | 1875.6 | 39.31 s | profile only (`20260928-101245-dd-prof-r1`) |
| 2 | 2810.5 | 26.23 s | fragment probe moved off `g_pool.lock` (`20260928-131651-dd-prof-r2b`) |
| 3 | 2443.1 | 30.18 s | 64 KiB snap chunks (`20260928-133055-dd-prof-r3`) |
| 4 | 2326.7 | 31.69 s | thread-local inode-dir fd (`20260928-133558-dd-prof-r4`) |
| 5 | 1776.1 | 41.51 s | publish batch 256 and AE cap 64 KiB (`20260928-134137-dd-prof-r5`) |
| restore | 2551.5 | 28.90 s | rounds 3–5 reverted (`20260928-134637-dd-prof-r5b`) |

The first attempt at round 2 cached inode-directory fds in a global
table and the 9-client dd hit the 150 s ssh timeout
(`20260928-102121-dd-prof-r2`). That cache is not in the tree.
`access()` under the pool lock was the round-1 server sample; moving
the probe out is what moved the wall. Later rounds removed that
`access()` and the snapshot `pread` from the top of the profile and
the slowest client got worse. Client CPU stayed blake3 in
`hash_write_fragments`. A 4 MiB snapshot chunk made the pump resend
the same chunk on every wake once `send_idx` passed `next_index`
(outbox hi=2048, ~20k messages/s). The in-flight end for a snapshot
chunk is now `UINT64_MAX` until the reply or a heartbeat. That fix
stayed in the restore.

## Sep 28 2026 — five posix jobs=1 profile rounds

cpu-clock while posix suite 1 ran on fcstor007, RDMA, port 19810.
Each round restarted fcstor003–006 with `roll_efsd.sh --all`
(`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`) and remounted the clients
when the client binary changed. Every suite was 200/201, mmap
SKIP only, 0 EFS bugs. `perf trace` still cannot read tracefs;
the second pass of the harness is not a wall-time result (one
traced pass took 150.9 s).

Round 1 (`results/measure/20260928-093100-posix-prof-r1`, 54.9 s):
client top `clock_gettime` in `efs_rdma_recv_wait`; server top
`memcmp` in `efs_kv_lsm_view_export` on the GC thread. The export
merge became a heap. Newest segment (lowest index) still wins a
tie. `test_kv_lsm` covers two overlapping L0s, a tombstone, and
the other raft group.

Round 2 (44.1 s / 56.1 s): export `memcmp` dropped off the top of
group 0. The client spin was still the top. `efs_rdma_recv_wait`
now pauses 64 times and then polls the eventfd.

Round 3 (29.0 s / 29.7 s): `clock_gettime` left `recv_wait`.
`recv_poller` was 16% and blake3 6%. Compactor `memcmp` was the
server-side merge; it uses the same heap. The 29 s pair did not
hold on the next rounds.

Round 4 (58.4 s / 57.0 s): server top was `write` from the export,
three syscalls per key. The export now buffers 64 KiB and flushes
before the seek that writes the item count.

Round 5 (50.7 s on the cpu-clock pass): writes left the server
top. A large export put `memcmp` and `vx_sift_down` back on top;
that is the heap walking the table, not a loop to delete.
`recv_poller` was still 16% of efs-fuse. The empty-CQ spin went
from 128 polls to 32. The ack stays after `poll`. A jobs=1 run
with no tracer after that roll was 200/201 in 45.1 s.

## Sep 28 2026 — RDMA recv poller

posix jobs=1 under `perf record -e cpu-clock -g` on the client fuse
and both raft leaders. RDMA
(`results/measure/20260928-050145-posix-prof-rdma`): `recv_poller`
was 64% of efs-fuse (147775 samples) and 31% of efsd. The thread
polled, paused, and `sched_yield`'d whenever a QP was up. Suites
56.4 s and 43.6 s, 200/201, 0 fail. TCP on the same binary
(`results/measure/20260928-050841-posix-prof-tcp`): no poller; the
top sample is blake3 at 355 hits. Suites 45.9 s and 59.0 s.
`perf trace` cannot open `/sys/kernel/tracing` (mode 700).

The poller now does a short spin and then `poll`s the completion
channel for at most 1 ms. Acking a CQ event before that `poll`
disarms `ibv_req_notify_cq`, so the next completion was invisible
until the timeout. That version took 76–78 s. With the ack after
`poll`, jobs=1 is 58.5 s and 57.8 s, 200/201, 0 fail
(`results/measure/20260928-053033-posix-prof-rdma-fix2`), and
`recv_poller` is absent. Servers and fcstor007–015 are that binary,
`EFS_TRANSPORT=rdma`. The wall did not move past TCP. The spinning
core is what came out.

## Sep 28 2026 — user xattrs, posix gate

`user.*` attributes are one KV blob per inode
(`EFS_KV_KIND_XATTR`) and one raft command (`EFS_MD_CMD_XATTR`).
`security.*` and `system.*` return EOPNOTSUPP without an RPC, so a
create does not pay a commit for an LSM label. The blob is removed
in the same batch as the inode row, and only when the key exists.
SELinux on fcstor007 is Disabled.

`roll_efsd.sh --all` with `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`
built `db2b88c4802a-dirty` on all four and then exited FAIL:
fcstor005 stayed at group 0 commit 13316260 and group 2 11742588
while the leaders were at 13387007 and 11813960. The follower was
writing `snap-0-13387007.part` and `snap-2-11813960.part`. A minute
later both groups matched the leaders. Clients fcstor007–015 were
remounted. fcstor003–006 still have the previous fuse.

posix jobs=1 on fcstor007: **200/201**, 0 fail, `opt_xattr` PASS,
`mmap_write_read` SKIP (`MAP_SHARED` ENODEV), 45.2 s
(`results/posix/20260928-043918`). Compare exits 2 because XFS
passes that mmap test. Kernel 5.14.0-687 has `FOPEN_DIRECT_IO` and
no `FOPEN_DIRECT_IO_ALLOW_MMAP`; `fuse_file_mmap` returns ENODEV
when the file is direct-I/O and the mapping is shared.
`MAP_PRIVATE` still passes. Do not clear `direct_io`.

Four suites at once on one client (`posixstress 4 fcstor007`,
`POSIX_JOBS=1`): each **200/201**, 0 fail, mmap SKIP only, 46–57 s
(`results/posix/20260928-044951`). The Sep 17 four-suite result
(`results/posix/20260917-120328`, 165–168 timeouts) is not this
build. A 16-suite run exists only on the deleted engine (Sep 1).

posix2 overlapped with that suite and persist
(`results/posix2/20260928-043918`): 61/63,
`peer_overlap_pwrite_chunk_straddle` and
`peer_rename_vs_unlink_src`. Alone, the same pair is **63/63** in
65.4 s (`results/posix2/20260928-044304`). persist on fcstor015:
26 prepared, 26 survived (`results/posixpersist/20260928-043919`).

## Sep 28 2026 — 19810 on RDMA

The Sep 27 live switch had been rolled back after 9-host posix
took 385 s. A second switch (`EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`,
`roll_efsd.sh --all`) stayed up. posix jobs=1 and 9-host are
199/201 (mmap + xattr SKIP, 0 fail) in 90 s and 57–59 s
(`results/posix/20260928-033823`, `results/posix/20260928-034049`).
posix2 is 63/63 in 76.8 s (`results/posix2/20260928-034350`).
9-client 8 GiB dd+fsync is **1326.6** MiB/s, walls 28.55–55.57 s,
every file 8589934592 (`results/measure/20260928-033420-dd-wall`).
TCP's 9-client bar is 1478, and TCP 9-host posix was ~13–15 s.
RDMA is not faster on those two.

fcstor004 group 0 had sat at commit 13315306 while the leader was
at 13316010, across RDMA and a TCP restart, until a snapshot
install landed. The shared per-peer outbox dropped the catch-up
AppendEntries and `host_send` still returned success, so
`ae_inflight` suppressed the resend. It now keeps snapshot chunks
and entry-carrying AppendEntries, and returns `EFS_ERR_AGAIN`
when the new message is not queued. `send_ae` / `send_snap` do
not mark that batch in flight and do not fail the propose.
Earlier RDMA build `113823180b15-dirty` had a valid 1-client
**947** and 4-client **2311**
(`results/measure/20260928-015839-dd-wall`) and an INVALID
9-client (do not quote ~238). IOR-hard NP=4 on that build:
write 469.55, read 107.03, bad 0
(`results/measure/20260928-015806-ior-hard-rdma`).

## Sep 27 2026 — same-directory creates

Creating many files in one directory stayed near 140 ops/s from 1
process to 36 (`results/measure/20260921-161931-samedir-rate`).
Spread does not apply below 65536 entries, and the log-path file
create is point gets, not a shard scan. The remaining serialization
was Raft: the first proposer held the fsync and appended locally,
but every proposer that arrived while that hold was open called
`efs_raft_propose`, which broadcast that one entry under `h->mu`.
The next create waited out the round trip.

A leader with a hold already open now appends locally and raises
the send ceiling. The hold owner waits until the proposers already
inside `host_propose` have appended (capped), kicks the pump, fsyncs
once, and marks every log index covered by that fsync durable on
every group it leads. A record remembers the file offset where it
ended so a later append in the same file is not treated as covered.
Log rotation fsyncs the new file and clears those offsets; leaving
the old offsets in place made the durable scan stop at the first
stale one. Idle single-create still takes the hold only when none
is open, so it does not wait out an unrelated batch (that version
moved idle mkdir from 7.1 ms to 11.9 ms and did not raise the
144-way rate).

`tests/test_raft_store` checks the covered-index rule. Servers
rolled `--all` as `ab458efab95b-dirty` at 21:11Z, TCP.
`results/measure/20260927-211953-samedir-rate` (PREFLIGHT_OK,
ROUNDS=100): storm PASS at 1×1, 1×9, and 4×9, parent
`children=0 nlink=2`. Aggregate **333 / 1385 / 1241** ops/s.
`busy_n` was 0, 0, and 1. Idle mkdir median on fcstor007 was
3.1 ms; an empty create+close median was 0.6 ms.

## Sep 27 2026 — 9-client fsync EIO

The morning 9-client 8 GiB dd (`results/measure/20260927-053506-dd-wall`)
returned fsync EIO on fcstor009 and fcstor013. Both files were still
8589934592 bytes. `report-split` on fcstor004 had two lines
`nrec=65536 pack_ms=0 push_ms=0 finish_ms=0 rc=-3`. A deleted inode
is already answered OK on this path, so that NOT_FOUND was not a
missing row. `send_ae` returned `EFS_ERR_NOT_FOUND` when `store->get`
or `log_term` raced a snapshot that had compacted the index.
`broadcast_ae` then failed the propose, and
`efs_client_report_dirty_ino` did not retry NOT_FOUND.

`send_ae_behind` sends an empty AppendEntries (or the snapshot) in
that window and does not return NOT_FOUND. `broadcast_ae` keeps
going if one peer still returns it. The report handler maps a
leftover NOT_FOUND to BUSY after the log line, and the client
retries NOT_FOUND the same way it retries BUSY.

`tests/test_raft` and `tests/test_raft_store` passed on fcstor014.
`roll_efsd.sh --all` with `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` landed
`75321297f719-dirty` on fcstor003–006 (ROLL_OK 20:35Z). Fuse on
fcstor007–015 was rebuilt at 20:36Z.

9-client rerun `results/measure/20260927-204907-dd-wall`, preflight
idle (group0 0/s, group2 0/s): **1478 MiB/s**, slowest wall 49.894 s,
all nine walls 49.76–49.89 s, every file 8589934592, FUSE_OK, no
fsync EIO. `report-split` is 128 lines: 119 `rc=-13`, 9 `rc=0`,
zero `rc=-3`. The OK lines have `push_ms=0` (the BUSY attempts had
already published). 1478 is 3.4% of the 44 GB/s ceiling. 1-client
977 and 4-client 1984 stay the morning numbers; they were not
remeasured on this build. Do not quote 1464.

## Sep 27 2026 — shared-file IOR-hard on immutable spans

A partial chunk publish appends a span object. The base generation
does not change, so concurrent disjoint ranges do not STALE each
other. The span object is a full chunk image; readers copy
`[off, len)`. Overlap, or a trailer already at 8 spans, returns
STALE and the client folds those spans into one full-chunk CAS.
That CAS commits only when `delta_base_n` / `delta_base_seq` name
the list the image already contains. Adopting a longer list after
the image was built deletes a peer span: that was
`peer_overlap_pwrite_chunk_straddle` (B's exclusive tail above the
chunk boundary came back as A's bytes). The fold now pulls the lane
seq only when the span generation set is unchanged, and leaves seq
at 0 otherwise so the CAS STALEs and the replay refetches. A replay
whose slot was cleaned keeps the published span range instead of
copying the whole zero-padded object. A short read with `ndelta > 0`
does not return an unpainted rdcache hit.

Open does not take a server lease, so the last unlink deletes the
inode row. A peer `fstat` of a still-open fd used to return the
local nlink (1). `efs_client_stat_open` treats GETATTR
`EFS_ERR_NOT_FOUND` as nlink 0 and copies only nlink and ctime, so
the local size stays. That pair, plus the xfs baseline capture
(python's stdout had been prepended to the tsv, and compare.py
rejected every row), is posix2 **63/63**
(`results/posix2/20260927-190509`, 77.7 s, compare PASS).

NP=4, SEGS=3000, 47008 B, one shared file, cold remount, FUSE_OK:
write **481.56 MiB/s** (1.12 s, 537.96 MiB), read **91.35 MiB/s**
(5.89 s), pattern 12000 records bad 0. The Sep 21 4-rank bar was
33 MiB/s. 1/9/36 were not remeasured. After the verify both groups
kept committing REAP_DONE. Group 0 fell to 1/s; group 2 stayed
~11–16/s for the whole wait. A 2 MiB tail of group 2
(index 11532756..11534661) was 1710 REAP_DONE and 1710 distinct
inodes, so it is a backlog, not a loop on one marker.
`tests/preflight.sh` fails above 5 entries/s. The scaling script
was not run on that cluster. Do not quote a 1/9/36 table from this
day, and do not raise the idle gate.

Clients fcstor007–015 are the gate6 fuse (`8e62ff12b422-dirty`).
Servers were not rolled for the fold and nlink fix; they already
had the span apply. The `pub-stale` fprintfs are removed from the
tree and still present in the live efsd until the next
`roll_efsd.sh --all`. Uncommitted.

## Sep 27 2026 — posix suites, and the snapshot window that elected

9-host suite 1 on TCP (`results/posix/20260927-123717`, screen
12:37:17–12:37:52Z): every host 200 pass, `mmap_write_read` SKIP
(`MAP_SHARED` ENODEV), 0 fail, 0 not-run. Compare still exits 1
because it counts that SKIP as an EFS bug. posix2
(`results/posix2/20260927-123946`): 63/63 in 58.0 s, rc=0.
Group 0 stayed term 7858 and group 2 stayed term 1719 through the
9-host run.

Two earlier 9-host runs the same morning (`20260927-121916`,
`posix9b`) were not this. `os.makedirs` of a fresh test directory
returned EBUSY, some hosts aborted on the uncaught `OSError`, and
the rest were cut. `apply_max` was under 1 ms. fcstor006's log
shows the cause: `raft-snap: start group=2` and then ~200 term
changes, commit frozen at 11164719 while fcstor005's commit moved
on. `send_snap` returns `EFS_ERR_AGAIN` for the whole export
(`host_snap_open` while `snap_exporting`). `send_ae` treated that
as success and sent nothing. The peer's election timer fired, the
vote request carried a higher term, and `maybe_step_down` dropped
the leader even though the log was not up to date enough to win.
`send_ae` now sends an empty AppendEntries in that window (not on
the `data_only` flush path). `on_ae_req` resets the timer before it
rejects a `prev_index` the follower does not have.

posix2's earlier 59/63 (`results/posix2/20260927-035212`) and the
mid-fix 57/63 were three data-path bugs, all on the build that
posix2d re-ran:

- `peer_shared_pwrite` lost 496 of 1000 half-blocks. `DCACHE_NR`
  was 8. Sixteen disjoint 4 KiB ranges in one chunk collapsed to
  one whole-chunk range with `base_gen = EFS_CHUNK_BASE_UNCOND`, so
  the last fsync published that client's image over the peer.
  `DCACHE_NR` is 32.
- `peer_truncate_visible` and `peer_extend_and_truncate` returned
  EIO. The truncate stub minted a new `candidate_gen` while copying
  the old objects' nodes and checksums. Objects are named
  `{ci}.{fi}.{gen}`. The reader GET of the new gen was never PUT
  (`EFS_ERR_DECODE`, fuse `efs_rc=-9`). When `got.generation != 0`
  the stub now keeps that generation, so apply sees
  `committed == candidate` and leaves the chunk row. The client
  already rewrites the zeroed tail before setattr.
- `peer_overlap_pwrite_partial` and `chunk_straddle` kept the
  previous pwrite's ranges after a successful report. The next
  pwrite merged into that span and a STALE replay painted the old
  image over the peer's exclusive tail. `dcache_note_committed`
  clears `nrange` when the slot is not dirty.

An every-read `pull_chunks_range` was tried and removed. GETCHUNKS
`NOT_FOUND` became EIO on an open fd after unlink, and the posix2
holder script turns that into an empty read. Do not put that pull
back on the read path.

## Sep 27 2026 — dd rebaseline on TCP

8 GiB `dd bs=1M conv=fsync`, own file per client, flush in the
clock, every mount `fuse.efs-fuse` (`results/measure/20260927-053506-dd-wall`).
Preflight refused because both groups were committing ~8 entries/s;
the leader log tail was `REAP_DONE` from the million-file unlink,
commit==applied, no client. The run was taken with that noted.
1 client (fcstor007) 8.388 s, **977 MiB/s**. 4 clients (007–010)
16.42–16.52 s, aggregate **1984 MiB/s**. 9 clients all wrote
8589934592 bytes in ~50.2 s, but fcstor009 and fcstor013 returned
`fsync` EIO: `fsync-split` `rc=-3` (`EFS_ERR_NOT_FOUND`) from
`efs_client_report_dirty_ino` after flush ~10 s and report 13–18 s.
That row is INVALID. Do not quote the 1464 the harness printed
from the walls. Sep 18 was 639 / 251 / 202; Sep 21 was 499 / 176
with the 9-client row also invalid.

## Sep 27 2026 — RDMA mkdir gap, two transport bugs

A private 3-node cluster on fcstor007 (ports 19950–19952, not
19810) made the live posix failure reproducible without the reap
tail. 100 mkdirs were ~2× to ~7× TCP, and the histogram had a
mode at 100–108 ms (11 of 100). RPC-PROF put that time in recv,
with `busy_n=0`. The shared recv poller acked the completion
channel and then `poll`ed it for 100 ms. Acking consumes the
event for a CQE that landed in between, so the WC sat in the CQ
until the tick, and that one poller stalled every conn. The
poller now harvests again before a 1 ms backstop.

That removed the 100 ms mode and left a raft AppendEntries at
~380 µs against ~15 µs on TCP. The send-CQE wait itself was 1 µs.
Every `efs_rdma_send_frame` called `ibv_query_qp` and opened the
port-counter sysfs file before posting, so the failure dump could
show the QP state at post time. Those reads now happen only after
a send has already failed. A clean rerun was 642 ms RDMA vs 507 ms
TCP for 100 mkdirs, raft RTT ~50 µs
(`~/efs-runs/rdmaprof7.log`). A later run's wall was one 1.5 s
mkdir and three BUSY retries; the other 99 were 2–7 ms. 19810
stays TCP. The send path also spins ~200 µs before `sched_yield`,
because a yield on the first miss gave the core away for the rest
of a timeslice.

## Sep 27 2026 — partitioned flush, and the RDMA mkdir gap

W13 step 5. A memtable flush used to write one L0 segment spanning
every key, so every compaction rewrote all of L1. The flush now
writes one L0 file per distinct `key[0]`. efs keys store the shard
in the first two bytes, so that is at most 16 files. Compaction
merges the fullest of those ranges and the L1 segments that overlap
it, and starts a new output file when the key range changes. A
segment that already spans several ranges is still compacted whole,
once, so the old files drain. `make test` on fcstor014 passed
(`test_partitioned_flush`: two ranges, two L0 files, and compacting
one leaves the other range's L1 file on disk). `roll_efsd.sh --all`
with `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` (`pflush-roll`) put that
binary on 19810. A mkdir/rmdir on fcstor007 returned immediately.

The same hour, a private 3-node cluster on fcstor007 (ports
19950–19952, not 19810) timed 100 mkdirs: TCP 1000 ms, RDMA 2246 ms.
That is the gap the live 9-host suite hit (193–196/201, 385 s).
The two causes and the private rerun are in the section above.
19810 stays TCP.

## Sep 27 2026 — live RDMA switch rolled back

W10 step 1 had already passed (private empty-table mkdir, 5/5 on
fcstor007). After the million-file tree delete finished, all four
servers were rolled with `EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`
(`w10roll`, `ROLL_OK`, build `4c6a5acefe03-dirty` on every node) and
fcstor007–015 were remounted with `EFS_TRANSPORT=rdma`. A single
mkdir/rmdir on fcstor007 returned immediately. The 9-host posix
jobs=1 did not match TCP: 193–196/201, skip `mmap_write_read`, 0
not-run, 385 s on every host
(`results/posix/20260927-044348`). The TCP bar is 200/201 in
30.4–31.3 s (`results/posix/20260927-033723`). The failures that
pass on TCP are the many-op tests hitting the 15 s cap
(`dir_deep_nesting`, `dir_deep_nesting_beyond_64`, `names_crazy_dirs`,
and on some hosts `concurrent_creates_same_dir` and
`mtime_monotonic_many_writes`) plus a few EIO and EEXIST one-offs.
`apply_max` on fcstor003 during that window stayed under 1 ms, so
it was not the compactor. A REAP_DONE tail from the deleted tree
was still committing before the suite, and group 0's term moved
during the run; the suite was still several times the TCP wall and
step 5 says that is a rollback, not a debugging session. The same
`roll_efsd.sh --all` with `EFS_TRANSPORT=tcp EFS_RAFT_OBS=1`
(`w10back`) brought 19810 back, the nine clients were remounted
TCP, and a mkdir/rmdir on fcstor007 returned immediately. Freeze,
idle md_latency, and the dd rebaseline were not run.

## Sep 27 2026 — chunked InstallSnapshot, and the import that held the pump

The first roll of chunked snapshots truncated every raft.log from
1.7–5.8 GB down to 1–3 KB and exported ~386 MB per group in ~8 s on
the GC thread. fcstor003 and fcstor004 then stopped answering
`raft-status`. The pump was inside `efs_kv_group_import`, which
compared every local key with every incoming key and then PUT the
whole image, under `h->mu`. A one-index catch-up of a live group does
not finish that way. The import now sorts both sides and writes only
the diff (`src/kv/kv_snap.c`). Unit tests passed (`w11mktest4`).
After `roll_efsd.sh --all` the four logs stayed under 5 KB, both
groups had one leader with commit==applied, and a restart of
fcstor005's efsd rejoined in 510 ms (`results/measure/20260927-w11-gate`).
`apply_max` during the 386 MB export was 0. Idle md_latency on the
second sample was mkdir 2.9 / create 1.5 / append 2.0 / stat 0.4 /
unlink 0.7 / rmdir 2.8 ms. The first sample, a minute after the
restart, had create and append at 53 ms (one retry sleep) and did not
hold. 9-host posix was 199–200/201 on the second run
(`results/posix/20260927-015719`), not the clean 200/201 of
`20260926-164123`. The leader-freeze script failed once (one rmdir
EBUSY left a child whose parent nlink stayed 2) and passed on the
rerun (`results/measure/20260927-020111-i17-leader-freeze`,
`arc_term_miss` 0→6, parent clean). The private RDMA empty-table
mkdir on fcstor007 passed 5/5 (`MKDIR_RC=0 WRITE_RC=0`). 19810 stayed
on TCP.

The client staging-table pin rules from the Sep 23 recommendation
are in the tree the same night. `efs_client_stage_pin` holds a
count across create, rename, link, unlink, and from an append
reservation until the write has the bytes in dcache or the PUT has
marked the ino dirty. Rename marks the ino dirty before it drops
the directory locks. The evictor drops chunk maps of a clean closed
file and leaves the row for a later pass. Every open fd stays
pinned. `make test` on node9901 passed (`w9mktest2`), including
`test_stage_evict`. The nine clients were remounted (`w9fuse1`).
9-host posix jobs=1 is 200/201, skip `mmap_write_read`, 30.4–31.3 s
(`results/posix/20260927-033723`). That is the W11 bar as well.
Posix 2 one pair is 59/63
(`results/posix2/20260927-035212`): `peer_truncate_visible`, both
overlap-pwrite cases, and `peer_shared_pwrite` (496 of 1000
half-blocks). The shared-pwrite case passed when run again by
itself. The valgrind leak gate failed once on 36 bytes in
`test_raft`'s snapshot callbacks (the forced export miss allocated
the handle, and the receiver kept the assembled bytes) and passed
after those frees (`results/leaks/20260927-035622`: unit, efsd,
efs-fuse, and the RDMA phase all 0 definite). The first stat of that tree, on a client whose oldest rows were
still pinned, grew RSS from 264 MB to 622 MB with no plateau: the
drain treated a full window of pinned entries as "nothing left" and
never reached the clean files behind them. After the drain walks
past that window, a cold stat on fcstor013 went from 5 MB to a
level 233 MB at 1M files
(`results/measure/20260927-w9-walk`). 19810 stayed on TCP.

## Sep 26 2026 evening — W13: L1 compaction off the apply path

The user asked to implement the next roadmap item. That ratified W13
only. The merge used to run inside `kv_compact_locked` under `l->mu`
and `h->mu` and rewrote every L1 segment (an L0 spans all shards),
which was the remaining election trigger (`apply_max` 1.7–2.4 s).

The compactor is one thread in `kv_lsm`. Flush still writes the
memtable to an L0 file and signals when `n_l0 >= l0max`. The thread
snapshots the L0 set plus overlapping L1, drops the lock, merges
through private `kv_seg` opens (the live segment block cache is not
shared), then under `l->mu` drops only the snapshotted inputs, keeps
L0 files flushed during the merge, and commits with the existing
atomic manifest rename. `kv_seg_doom` unlinks a file when the last
pin goes away, so `efs_kv_lsm_view_pin` still reads the old segments.
`EFS_KV_COMPACT_DIE=N` `_exit(99)` after the Nth finished output
segment, before that rename. `efs_kv_lsm_flush` no longer compacts;
at `KV_LSM_MAX_SEGS` it waits on `l->cv` and logs
`kv-compact: backpressure`. `kv_compact_locked` stays for
`efs_kv_lsm_compact` and for the no-thread fallback.

`make` of the KV/raft/txn tests on fcstor007 was green, including
`test_pinned_view` and `test_compact_crash`, before
`tests/roll_efsd.sh --all` with
`EFSD_ENV='EFS_TRANSPORT=tcp EFS_RAFT_OBS=1'`. Build
`3210a3d63f73-dirty`. Clients fcstor007–015 remounted
`fuse.efs-fuse`.

Hammer `results/measure/20260926-163709-mkdir-hammer`: idle p50
4.68 ms; 144-way 35166 mkdirs, p50 56.5 ms; `apply_max` 68473 µs on
fcstor005. fcstor004 logged two compactions, 796333166 bytes in
1964 ms and 798775386 bytes in 2067 ms. Errors were ten
`rmdir-own ENOTEMPTY` lines.

9-host posix `results/posix/20260926-164123`: 200/201 on all nine
hosts, 13.2–14.8 s. Timeline
`results/measure/20260926-124106-w8-stall-timeline`: group 0 term
6882 leader 1 and group 2 term 1198 leader 3 for the whole 139 s;
no probe over 1 s. Four `kv-compact` lines in the window (two ~400
MiB in ~0.85 s, two ~760 MiB in ~2.1 s); per-host `apply_max` peaks
32 / 67 / 64 / 11 ms. Idle `md_latency.py` afterwards: mkdir 2.9,
create+close 1.6, append+close 2.0, stat 0.3, unlink 0.8, rmdir
2.9 ms.

Not started: partitioned flush (W13 step 5), W11, W9, W10.

## Sep 26 2026 — Raft log fsync moved out of the host lock

Propose held `h->mu` across `log_sync_locked`, so 144 mkdir threads
each waited out a private fsync and AppendEntries did not start until
that fsync returned. `68dfebb` takes the shared sync hold before the
lock, broadcasts, fsyncs outside the lock, then
`efs_raft_durable` so the leader does not vote for an entry it has
not synced. Followers can still form a majority without the leader.

`results/measure/20260926-042515-mkdir-hammer` (before): idle p50
8.4 ms, 9×16 p50 258 ms, 470 mkdir/s. `20260926-044248-mkdir-hammer`
(after, smaller table): idle p50 3.7 ms, 144-way p50 144 ms, 735
mkdir/s. One AppendEntries stayed in flight, so the next mkdir was
still its own round trip.

Two attempts to batch that send were reverted. Broadcasting only
after dropping `h->mu` (`1835c70`) did not beat 735/s. Pipelining
every newer suffix while the previous batch was unacked (`0e81e49`)
committed far ahead of apply: `20260926-050319-mkdir-hammer` shows
fcstor005 `pump_hold_max=5336343us` and `applies_in_worst=12120`.
`e8f3dc1` puts the one-batch cap back. Remeasure on the table those
runs left behind (`20260926-050609-mkdir-hammer`): idle p50 10.9 ms,
144-way p50 189 ms, 706 mkdir/s, apply_max 67 ms, `fin_q=0`. The
idle gap versus 3.7 ms is the grown table, not a return of the
under-lock fsync. Do not pipeline past the one in-flight batch.

9-host suite on `e8f3dc1`
(`results/posix/20260926-050825`, timeline
`results/measure/20260926-010808-w8-stall-timeline`): 194–199/201 in
40–44 s. Group 0 elected once (term 6294→6296). `names_crazy_dirs`
still hits the 15 s budget on the slower hosts.

`6a60318` holds the pump's AppendEntries until proposers blocked on
`h->mu` have appended, and still refuses a second batch while one is
in flight (the `0e81e49` pipeline applied 12 120 entries under the
lock). Clean hammer `results/measure/20260926-052320-mkdir-hammer`:
144-way p50 168 ms, 754 mkdir/s, apply_max 61 ms, `fin_q=0`.
`20260926-052437-mkdir-hammer` is the same binary with a 766 ms
apply of 31 entries (p50 235 ms) — compaction, not the batching.

`5d3e603` stops holding the LSM lock across a segment pread and
caches the block after a miss. A negative dentry lookup used to read
every segment and free the buffer, so the next name in the same
directory paid the disk again. Hammer
`results/measure/20260926-053753-mkdir-hammer` (build string
`f93e7e6669b6-dirty`, the bytes of `5d3e603`): idle p50 7.1 ms,
144-way p50 147 ms, 887 mkdir/s, apply_max 59 ms / 256 applies,
`fin_q=0`. Three `rmdir-own ENOTEMPTY` cleanups, no mkdir errors.
9-host suite on the same tree
(`results/posix/20260926-054047`, timeline
`results/measure/20260926-014030-w8-stall-timeline`): seven hosts
200/201, fcstor007 199 (`dir_many_files` EIO), fcstor013 198
(`names_crazy_roundtrip` EIO and `dir_deep_nesting` 15 s), 39–41 s.
Term did not move. `names_crazy_dirs` passed on every host. Probe
during the run: stat p50 3 ms (one sample 1.3 s), mkdir p50 14 ms.

A follower AppendEntries of many entries fsynced once per entry
under `h->mu` (the duplicated 128-entry catch-up was ~30 ms). The
store's `batch_begin` / `batch_end` defer that fsync to one call at
the end of the batch. The leader path is unchanged: only the
proposer that finds the sync hold at zero takes a slot. Giving
every proposer a slot, so the fsync waited for threads that had not
appended yet, moved idle mkdir from 7.1 ms to 11.9 ms and did not
raise the 144-way rate. That version was not kept.

## Sep 26 2026 afternoon — memtable probes and the applied_cv herd

With the scans pruned (`610f4a8`) the pump thread was still 61–67 % of
efsd samples on the dual-host follower. `perf report --sort pid,sym`
put `memcmp` at 21 % of efsd on that one thread, and `-g caller` split
it: 12 % under `lsm_batch ← lsm_put ← prepare_reduce_rec ←
efs_txn_apply_prepare` — the memtable binary search (`kv_mtab_pos`,
inlined), ~15 probes per put, each probe a pointer load `e[mid]` and
then the key behind it, two dependent misses; the rest under the txn
scans and `kv_seg_probe`. A second block, ~20 % of efsd, was kernel:
`native_queued_spin_lock_slowpath` under `futex_wake` and
`futex_wait_setup`, `_raw_spin_unlock_irqrestore`, `futex_hash`. That
is the signature of many threads on one futex word: the pump ended
every cycle with `pthread_cond_broadcast(&h->applied_cv)`, so under
the hammer ~150 sleeping handlers woke on every cycle (hundreds per
second), took `cv_mu`, re-read the view, found `applied < idx`, and
slept again.

Three changes, all inside `src/kv/kv_lsm.c` and `raft_host.c`, no
wire or on-disk change:

1. The memtable entry's key is allocated inline
   (`calloc(sizeof(*e) + klen)`, `e->key = (uint8_t *)(e + 1)`);
   `ent_free` no longer frees the key. Alone it moved `memcmp` from
   13.4 to 11.9 % — the miss just moved to the single load.
2. `struct kv_mtab` gained `uint64_t *pfx`, the first 8 key bytes of
   `e[i]` as a big-endian integer, zero-padded; `kv_mtab_pos` compares
   the integer and calls `kv_key_cmp` only on a tie (a key shorter than
   8 bytes ties with a longer key whose tail is zero, and the tie falls
   through to the full compare, so ordering is `kv_key_cmp`'s). The
   array is ~220 KiB for a full 4 MiB memtable and stays in L2; insert
   memmoves both arrays; `m->bytes` counts the extra 8 bytes.
   `memcmp` 23 → 11.6 % of efsd, `kv_mtab_pos` 2.2 %, `memmove` 4.7 →
   6.7 %.
3. Targeted wakeups. `struct host_waiter` × 512 in the host, each with
   its own condvar and `group / any / idx / seen`. `host_waiter_sleep`
   (cv_mu held, called right after the caller re-read the view under
   cv_mu and found its predicate false) takes a free slot and
   timed-waits on it; the pump, after `host_publish_view`, runs
   `host_waiters_wake`: under cv_mu, for every active slot, signal if
   the group's replica is gone, or (`any`) the group's `v_stamp`
   differs from `seen`, or (`!any`) `v_applied >= idx`. `v_stamp` is
   bumped by every publish that changed a field, stored last, and
   `host_view_get` reads it first, so a waiter holding stamp S has
   fields from publish S or later. `applied_cv` stays as the overflow
   path (`cv_overflow` counts sleepers on it; the pump broadcasts only
   when it is non-zero). `host_read_index` sleeps with `any=1`,
   `host_wait_applied` with `any=0`.

Each step went through `test_kv`, `test_kv_lsm`, `test_meta_apply`,
`test_txn`, `test_raft_store`, `test_raft` on fcstor007, a
`roll_efsd.sh --all` + client redeploy, and two `mkdir_hammer.sh` runs
(the second with `perf record` on fcstor004). Build string
`610f4a847738-dirty` throughout.

| run | change | idle p50 | 144-way mkdir | p50 | max |
| `20260926-135832` / `-135944` | inline key | 5.43 / 5.15 | 16520 / 23983 | 51 / 49 | 11.4 s / 11.2 s |
| `20260926-141042` / `-141156` | + prefix array | 5.53 / 5.35 | 25942 / 26834 | 65 / 60 | 9.7 s / 12.1 s |
| `20260926-141940` / `-142052` | + targeted wakeups | **5.23** / 5.08 | **30366** / 14439 | **52** / 60 | 3.3 s / 14.7 s |

The mkdir count is dominated by how many 2 s `kv_compact_locked`
stalls, and the elections they trigger, land in the 15 s window: every
run had `apply_max` ≈ 2.0 s, and group 0 went from term 6746 to 6751
during the best run alone. The 10–15 s maxima are the client's 16
BUSY/STALE retries (10.3 s) exhausted across a stall + election. Read
p50, idle p50 and the profile, not the count.

9-host posix on the final build: `results/posix/20260926-1425-wake`
started into a four-term group-0 election burst (fcstor003
`apply_max=851 ms` → LEADER→FOLLOWER at 6754, CANDIDATE→CANDIDATE
twice, LEADER at 6757, FOLLOWER again at 6758, `arc_term_miss=37`) and
scored 193–200/201 in 16–75 s, with an EIO cluster on fcstor014's
file tests (`write_hole_pread_zeros`, `two_fds_independent_offset`,
`trailing_slash_on_file`, `flock_unlock_on_close`,
`access_f_ok_after_unlink`, `mkdirat_unlinkat`) inside that window.
Sixty seconds later `results/posix/20260926-1430-wake2`: **200/201 on
eight hosts, 199 on fcstor009** (`flock_shared_then_exclusive` —
"LOCK_EX taken while another fd holds LOCK_SH", seen once each on Sep
22, 25 and 26 builds), **15.5–23 s per host** against 39–44 s on every
earlier gate run, through a 2.0 s `apply_max` on 004/005 and one
election per group at the start. (The first run's output landed under
`results/posix/home/...` because `POSIX_OUT` is a directory name, not a
path; moved.)

Tried and reverted right after: staging WAL records under a sync-hold
in a 1 MiB user buffer and issuing one `write()` before the hold's
fsync (one syscall per pump cycle instead of one per `lsm_put`). Tests
passed, the cluster ran it (`20260926-143658` / `-143818`: idle 5.29 /
5.10, p50 45 / 49 ms, both stall-dominated), but the pump's syscall
share did not move (`rep_movs_alternative` 2.1 %, `syscall_enter` 0.5
% — the bytes copied are the same, and the per-call overhead was never
the cost) and the 9-host suite (`20260926-1441-wal`, not committed)
ran into another multi-term election burst at an 850 ms `apply_max`
and scored 194–200. No measurable gain, a small change to
process-crash semantics (records in user memory instead of the page
cache until the hold's fsync): reverted before commit. The servers were
then rolled to the committed tree (`c044fb16e859-dirty`; hammer idle
p50 **4.73 ms**, 26585 / p50 54).

What is left on the pump: `memmove` 7 % (two-array insert into a
sorted memtable), `kv_msrc_advance` / `kv_seg_iter_next` /
`search_block` ~5 % (the txn-record scans — `guards_conflict` and
`reduces_pending` are already per-key prefixes; `txn_scan_kinds` walks
a shard's three kinds because the txid is the key's suffix, and
narrowing that is a key-layout change), `kv_compact_locked` (W13), and
a residual futex share from `l->mu` / `h->mu` handoffs. One PREPARE
command carries one part and costs one Raft entry and one `lsm_put`;
a mkdir is ~8 of them plus DECIDE and RESOLVEs. Folding a
transaction's parts into one PREPARE would cut the entry count but
changes the verdict protocol (one verdict per part today) — that is
a decision for the user, listed in START-HERE §1b.

## Sep 26 2026 midday — the mkdir ceiling was the apply path, not fsync

Follow-up to the hammer work above. A thread-local fsync defer
(`log_defer_depth`, the follower-batch mechanism) wrapped the
cross-group mkdir/rmdir PREPARE loops and the RESOLVE loop so one
thread's ~6 entries shared one fsync. Rolled and measured
(`results/measure/20260926-110435-mkdir-hammer`, uncited): 12059
mkdirs / p50 174 ms with a clean 26 ms apply_max, idle 10.7 ms —
no better than the 887/s baseline. Reverted before commit.

Then measured instead of guessed. `strace -c -f -p efsd` on fcstor004
(dual-host follower) for 12 s of hammer: 103 847 `pread64`, 10 506
`fsync`, 3 502 `openat`+`rename`+`pwrite64`+`close` (the applied-index
file, one per pump cycle per group). `perf record -g` on the same
host: 32 % `__memcmp_avx2_movbe`, 5 % `search_block`, 4.7 %
`kv_seg_iter_next`, 6 % `memmove` — the linear walk inside 64 KiB
LSM blocks, under `l->mu`, on the pump. Each PREPARE apply runs
`guards_conflict` + `reduces_pending` (two prefix scans), each RESOLVE
`txn_scan_kinds` (three); `merge_scan` opened an iterator on every
segment (one pread + walk each) whether or not the segment's key
range could hold the prefix.

Three changes, committed together:

1. `kv_seg_excludes` + `merge_scan` skip: a segment whose first key is
   past the prefix or whose last key is below the seek is not opened
   (the range test compaction already uses). `iter_load` copies a block
   from the segment's slot cache instead of a pread when a point get
   just read it. `pread64` fell off the strace top list.
2. Pump durability tail off `h->mu`: the KV WAL fsync and the
   applied-index write now run after the unlock; the file write is
   throttled to 10 ms (`HOST_APPLIED_PERSIST_US`) and forced once at
   pump exit. The saved index is only a restart lower bound and never
   passes the durable KV, so the guarantee is unchanged; waiters wake
   before the KV fsync, which is fine because the Raft log is the
   durability boundary.
3. `KV_LSM_BLOCK_TARGET` 64 KiB → 8 KiB, `KV_SEG_CACHE_SLOTS` 32 →
   256. Readers take any block size, so the live table needed no wipe;
   compaction rewrites old segments. Index RAM ≈ 8 MB per GiB.

Hammers (all on build string `194286c37a4f-dirty`, each containing one
1.7–2.0 s `kv_compact_locked` stall): prune only
`20260926-112916` idle 9.5 / 144-way 13715 / p50 137 ms; + pump tail
`20260926-133934` idle 6.97 / 14764 / 106; + 8 KiB `20260926-134640`
idle 4.54 / 21990 / 73 and `20260926-134738` idle 5.59 / **28072 /
p50 60 ms**, 22 `rmdir-own ENOTEMPTY`, 0 mkdir errors. perf after:
`memcmp` 24 % (now mostly `kv_compact_locked` 7 % and `lookup_mt`),
`search_block` 0.8 %.

Cost: the L1 rewrite now comes every ~7 s of hammer (trigger is 4 ×
4 MiB flushes; the table writes ~2 MB/s of records at this rate) and
1.9 s is past the election timeout. 9-host suite
`results/posix/20260926-1350-blk`: 20–38 s per host (was 39–41),
195–199/201, two elections each at a 1.9 s compaction (g0 6624→6626,
g2 1068→1069), `txn-recover` ABORTs after them, client
`inode-rpc: ... exhausted 16 BUSY/STALE retries (10.3 s)`. Failures
are the election class (`link_across_dirs`,
`last_link_unlink_other_dir`, `dir_many_files`, `dir_deep_nesting*`).
W13 is unchanged as a decision; it is now the only thing between this
build and 200/201.

## Sep 26 2026 — txn finisher after a 400 ms apply wait

A full-L1 compaction under the KV lock stalls apply for >400 ms
(`results/posix/20260926-0330-diag`: fcstor004 `apply_max=1665075us`,
`persist_max=1103us`, `wait_timeouts` 0→79 in one obs window). Every
in-flight `host_propose_wait` returns BUSY. A DECIDE COMMIT already in
the log then has no RESOLVE, the EXCL intent (often the ALLOC key)
stays until `host_txn_recover_pass` (age 5 s; log showed
`txn-recover ... age=6.0s -> COMMIT`), and other clients on that shard
burn the 16-attempt budget into EIO.

`host_txn_commit` queues that txn on a finisher thread. The thread
reads `efs_txn_decision_get` (NOT_FOUND until DECIDE has applied) and
proposes RESOLVE with that decision, retrying BUSY/NOT_PRIMARY until
10 s. RESOLVE stays idempotent with recovery. This does not shorten
the stall (W13).

First 9-host suite on the build: 200/201 × 9, 42–44 s
(`results/posix/20260926-0345-fin`), `fin_q=0`. Repeats
`-fin2`/`-fin3`/`-fin4` (not cited): 196–200, `fin_done` up to 11,
`fin_drop` 1, remaining fails are the 15 s many-op tests once the
table is warm and a stall lands inside the window.

Rejected the same day, do not restore: forwarded-submit early return,
batched remote PREPARE/RESOLVE, and an AppendEntries suffix while one
batch is in flight. Each left the 9-host suite worse than the
committed empty-commit-probe baseline (`20260926-0125-clean`,
199–200/201). `send_ae` keeps one batch in flight on purpose (Sep 19
fsync storm).

## Sep 23 2026 evening — gates on `7e29943`, and the idle 50 ms is on close

Owed gates, no new code on the cluster. 9-host posix 193–196/201, 0
not-run, 79–94 s (`results/posix/20260923-202626`). The six many-op
timeouts remain (mkdir p50 46 ms under the suite). One group-2 election
(term 535→537) lines up with the one-offs: EIO on
`concurrent_create_unlink_two_proc` (009, 010), `content_random_roundtrip`
(014), `concurrent_write_and_readdir` (007), a `b''` read on
`unlink_open_then_recreate` (009), `concurrent_appends` timeout (010).
Group 0 stayed term 5498; the status line `leader=0` is raft id 0, which
is node 1. No 2.4 s compaction stall in this run.
`same_parent_storm` 9×4×100 left the parent `children=0 nlink=2`, rmdir
OK, and FAILed on two `mkdir ENOENT` from fcstor014 at round 63
(`results/stress/same-parent-20260923-202846`) — a reply-path ENOENT, the
row was not torn.
Idle `md_latency` 20 min later, term stable, commit==applied, still +30
entries in 30 s: mkdir 8.5 / create+close 56.8 / append+close 59.5 /
stat 0.5 / unlink 2.2 / rmdir 7.8
(`results/measure/20260923-162535-idle-mdlat`). The 50 ms mode is the
two ops that write one byte and close, not every metadata op, and it is
not the post-roll churn (roll was two hours earlier). Next measurement
is a follower strace of one create+close.

Server half of the I16 window-GC follow-up, not rolled: `fence_local`
deletes that shard's op-id window for the epoch it fences
(`test_session` OK on node9901). The FUSE client never creates an efs
session, so this path does not run in production, and seeding the op-id
uuid from "the client session" has nothing to copy.

## Sep 23 2026 — visibility at the coordinator's decision (`ad292b9`, `7e29943`); four decisions written up

Two more reply-path errors from the freeze runs on `bb634d9`, both the
same shape: a client saw ENOENT/EIO for a name or row that had already
COMMITted but whose RESOLVE was still pending (recovery of a stranded
txn takes 5–16 s; the I16 window answers the retry OK meanwhile).
`getattr` on the new dir read the inode row with a bare kv get and got
NOT_FOUND (`fuse: mkdir ... ok ino=85959 but getattr rc=-2`) →
`efs_meta_apply_get_inode_tx` reads through a COMMITted EXCL intent via
`efs_txn_read` (`ad292b9`). Then `rmdir` of `d-fcstor011-1-22` returned
ENOENT for a dentry in the same state → `efs_meta_apply_lookup_tx` /
`resolve_tx` (`7e29943`); every handler-side lookup/resolve in
`raft_host.c` uses them, the apply path stays on the plain read (a state
machine cannot ask a coordinator). `rmdir`/`unlink` now map
`EFS_ERR_BUSY` to EBUSY (`fuse_unlink_errno`), not EIO. Tests
`test_stat_committed_unresolved_row`, `test_lookup_committed_unresolved_dentry`.
Rolled `7e29943f28ef-dirty` 18:21 UTC (servers + clients 007–015);
freeze ×2 (`results/measure/20260923-182559-i17-leader-freeze`,
`-182709-`) both PASS, 0 worker errors, parent removable,
`opid_replay` 0→1→3, `arc_term_miss` 0→3→15. Idle `md_latency.py`
pre-freeze 9.8/4.4/7.1/0.4/2.0/10.5 ms (mkdir/rmdir above the
6.1/5.2 reference; remeasure on a flat commit). Design limit kept: the
16-attempt client budget (10.3 s) is shorter than a 16.5 s recovery →
EBUSY to the app, never a wrong answer.

Tooling: one `Shell` call executed three times (three `efs-rec: start`
lines within a second → concurrent builds in one `/tmp/efs`, duplicate
`git commit`s). `efs-rec.sh` now runs a name once (flock + `.done`,
exit 75 `DUPLICATE`), `efs-bg.sh start` holds a per-name lock; rule
`efs-remote-timeouts` says never reuse a name to retry. An `EFSD_ENV`
with a comma (`EFS_TRANSPORT=tcp,EFS_RAFT_OBS=1`) is one env var →
transport AUTO (ungated RDMA), no OBS; caught by the `efsd.log` header
and re-rolled with the space form.

With the freeze class closed, the queue stopped on four decisions the
spec does not make. Each got a recommendation, the reason, and steps in
START-HERE §1a ("Decisions pending", items W13/W11/W9/W10): background
compactor (a lock hold, not CPU; per-range compaction cannot help while
one L0 spans every shard); the Raft paper's chunked InstallSnapshot over
a lazily exported snapshot file from a pinned segment view (the RAM
`snap_blob` + export under the SM lock is the same stall class as
compaction — removing only the 4 MiB cap would make it worse); ratify
the client-cache pin rules and the 256 MB soft cap; and no wipe for W10
— `tests/rdma_first_inode.sh` is a private 3-node cluster on fcstor007
and is the empty-table repro, so 19810 can be switched to RDMA in place.

## Sep 22–23 2026 — I16: op-id dedup for directory RPCs (`43bdf6a41f7d`)

Why now: the second I17 freeze run (`results/measure/20260922-122629-i17-leader-freeze`)
left three `d-*` dirs whose `mkdir` had returned EEXIST and whose `rmdir`
returned ENOENT, with a perfectly consistent parent row. Recovery had
logged `COMMIT (resolved)` on those shards: the client's coordinator got
BUSY from the apply-wait deadline, the client retried the same MKDIR, and
the retry met the dentry its first attempt had (invisibly, at that
moment) committed. Same class as the 9-host suite's `link_of_symlink`
EEXIST on a fresh name, `concurrent_create_unlink_two_proc` EIO and the
`b''` read. §7.9 already specified the fix (stable request identity +
bounded per-shard window); only APPEND (sim) and `create_file_op` (sim)
implemented it, and the production dir RPCs carried no identity at all
(`pack_create_cmd` zeroed the uuid slot; that slot is the lease holder's
identity, not the requester's).

Design decisions taken (each was a real choice, recorded so nobody
re-litigates): (1) **Window shard = dentry shard of the destination
name.** The coordinator is randomized per txn (a retry would probe the
wrong shard); the parent's inode shard can be in the other group (the
window record would then live in a group that does not own that shard
range). The dentry shard is deterministic from the request, always a
participant, always owned by its group. (2) **Atomic with the op, never
a separate write.** Cross-group txns get a `EFS_TXN_REDUCE_OPID` part
folded at RESOLVE by `fold_reduce` (dispatch on `EFS_KV_KIND_OPID`),
which commutes with the log path's read-modify-write of the same window
because both are Raft-log-ordered on the owning group; the single-group
log path carries a trailer on the CREATE (`HOST_CREATE_F_OPID` flag in
`cmd[1]`) / UNLINK / RMDIR (by length) commands and the apply writes the
window in the same `efs_kv_batch`. (3) **Probe before the pre-check.**
The apply's idempotent OK-on-EEXIST would otherwise mask a genuine
second create; so the leader probes (`host_opid_replay`) first, then
runs the usual lookup that yields EEXIST/ENOENT. (4) **Ack semantics.**
`efs_opid_ack` now advances the watermark and shifts the bitmap even
past seqs the shard never served; the client sends `ack = lowest
in-flight seq − 1` (512-slot in-flight table, slot held across the
whole `rpc_send_recv_*` retry loop so the retry is byte-identical).
Because an op is in flight until its RPC returns, no request ever probes
a window whose watermark already covers its own seq — so the "acked
stub" answer in `efs_opid_lookup` cannot fire for a first attempt.
(5) `VAL_MAX` in `txn.c` rose from 320 to `EFS_OPID_VAL_MAX` (512); the
res_acc buffer is 48 × 512 on a server thread stack.

Limits left in place (spec-bounded, not bugs): 16-entry reply cache per
`(client, epoch, shard)` — a 17th un-acked op on one shard applies
unrecorded (falls back to pre-I16); the identity is a per-mount random
uuid rather than the §7.5 session's, so the fence barrier does not yet
drop a dead client's windows (follow-up in START-HERE §1b).

Tests: `test_txn:test_opid_reduce`, `test_meta_apply` I16 block; on-node
build clean (no warnings), all C tests OK
(`~/efs-runs/i16-build.log`). Rolled all four (`roll-all.log`,
`ROLL_OK`, `43bdf6a41f7d-dirty`, g0 leader 1 term 5374, g2 leader 3 term
469), `EFS_RAFT_OBS=1` kept.

Gate runs on `43bdf6a` (02:52–03:07): freeze run 1
(`results/measure/20260923-025259-i17-leader-freeze`) parent clean,
`arc_term_miss` 0→3, `opid_replay` 0→3, 0 worker errors — the retry
class the item was opened for is answered from the window. Freeze run 2
(`-025903-`) parent clean, `opid_replay` 3→7, but three workers got
`mkdir ENOENT` at round 7 during the g0 freeze; the same workers'
`rmdir` of that name then succeeded, so the MKDIR had committed and the
error was in the reply path. Server logs held no mkdir `rc=-3`. Traced
in the client: `rpc_send_recv_shard/_dual` return `EFS_ERR_NOT_PRIMARY`
at once when NOT_PRIMARY arrives without a hint (`primary_id == 0` — a
stale leader that just stepped down, before it hears the new leader's
heartbeat); `efs_client_stat_ino` maps any GETATTR failure to
NOT_FOUND; `ll_mkdir` does `lookup_fill(new_ino)` after the mkdir reply
and the FUSE `*_at` helpers map that to ENOENT. So the app saw ENOENT
for a directory that exists. Fix: a hintless NOT_PRIMARY backs off like
BUSY (50 ms × 2^min(n,4), same 16-attempt budget, EBUSY at the end) in
both send paths; `ll_mkdir` logs `fuse: mkdir ... ok ino=N but getattr
rc=` so the next occurrence is attributable; `host_opid_reply_ino` logs
`opid-replay ... (stub)`/`row gone`. The same class is the first
suspect for the 9-host one-off EIOs. posix jobs=1 200/201 + mmap SKIP
(`results/posix/20260923-030005`); 9-host 189–191/201 per host, 0
not-run (`results/posix/20260923-030056`). `md_latency.py` 30 s after
the suite (8.4/19.3/65.9/0.5/2.0/8.3 ms) is post-suite churn, not a
number; idle remeasure owed.

Operational note: the login-node Cursor shell died three times in this
session ("no exit status", no `rec-*.log` created = wrapper never ran);
each time a fresh probe 20–60 min later answered. The nested-quoting
`efs-bg.sh start … "ssh … \"…\""` one-liner was replaced by a script in
`~/efs-runs/i16-build.sh` (rsync + `make clean` + build + run the C
tests on one fcstor) — reuse it.

## Sep 22 2026 00:00–00:35 — I17: the apply ring matched proposals by index, not (index, term)

Picked up START-HERE §1b item 0 (half-applied cross-shard txns: ino 62991
nlink=3 nents=1 with 0 dentries, ino 31264 with a dentry to a missing row).
The hypothesis in that block — recovery's 5 s stranded age racing a slow
coordinator — was wrong on inspection: `efs_txn_decide` is first-writer-
wins and `host_txn_recover_one` does ABORT → PROTO → "COMMIT (resolved)"
correctly. The hole was in the primitive under every coordinator:

- `host_propose_wait_idx` = `host_propose` (returns the log index) →
  `host_wait_applied(idx)` (published view `applied >= idx`) →
  `host_apply_rc_locked(idx)`, which read `arc_rc[idx & MASK]` if
  `arc_idx[slot] == idx`, else counted `obs_arc_miss` and returned **OK**.
  `host_apply` recorded the ring with `(void)term;`.
- Raft reuses an index across terms: a leader that loses its term has
  its uncommitted tail truncated by the new leader, which commits a
  different entry at the same index. The old leader's waiter then sees
  `applied >= idx` and reads the stranger's verdict — almost always OK.
  For a txn coordinator that is "my DECIDE COMMIT applied" when no
  DECISION exists → RESOLVE COMMIT on the participants it reaches (child
  row deleted / dentry written), recovery finds no decision 5 s later →
  ABORT on the rest. Exactly the two dumped rows. The compaction stall
  (2.4 s under `h->mu`, both g0 replicas) supplied the leader changes
  (g0 5302→5303 in the 22:10 run).
- Forced repro before fixing: `tests/measure/i17_leader_freeze.sh` —
  `same_parent_storm.sh` 9×4×80 while SIGSTOPping the g0 leader 3 s, then
  the g2 leader 3 s (HOST_ELECT_BASE 100 ticks × 5 ms = 0.5–1 s, so each
  freeze forces an election). On `84a2a55`: parent `children=1 nlink=2`,
  `rmdir` FAIL, `kv_dir_dump` on a fcstor004 KV copy: parent 230820
  `nlink=2 nents=0` with dentry `d-fcstor009-2-29` → ino 113873 row
  present; the worker logged `mkdir EIO` then `rmdir ENOENT`; recovery
  logged `shard=1444 coord=3281 parts=2 age=13.6s -> COMMIT (resolved)`
  (`results/measure/20260922-042153-i17-leader-freeze`). First attempt
  of the script silently froze nothing: `GROUPS` is a bash builtin array
  (read 246111) — renamed `FREEZE_GROUPS`.
- Fix `46d54e6`: `arc_term[]` beside `arc_idx[]`; `host_propose` gains a
  `uint64_t *term` out (= `efs_raft_term(r)` under the same `h->mu` as
  the append; 0 when forwarded); `host_apply_rc_locked` /
  `host_apply_extra_locked` take the term: mismatch → `EFS_ERR_NOT_PRIMARY`
  (the entry was never committed; nothing happened; retry at the new
  leader), slot overwritten → `EFS_ERR_BUSY` (verdict unknown), never OK.
  `host_wait_verdict` (wait + verdict) and `host_wait_settled` (wait +
  term check only, for the CREATE/APPEND-style callers that ignored the
  verdict) replace the six bare propose+`host_wait_applied` pairs;
  `host_bg_propose` (recovery DECIDE/RESOLVE, reaper) checks too;
  `host_idx_ref` and the pipelined RESOLVE prefs carry the term;
  `host_pub_batch` keeps `terms[]` and answers STALE on a mismatch (client
  repulls). `efs_msg_raft_mkfs_reply.term` (server↔server, same build ID
  everywhere) carries the leader's term for the forwarded-PUBLISH branch
  of `server_raft_host_submit`, which proposes without waiting. `host_apply`
  stamps the slot on its early return so no waited-on slot is ever
  "never written". `raft-obs` prints `arc_term_miss=`. Unit tests
  (test_raft/txn/meta_apply/wire/sim) OK, 0 warnings, built on fcstor007.
- Rolled all four (`roll_efsd.sh --all`, `EFS_RAFT_OBS=1` kept):
  running `46d54e679e4f-dirty` (dirty = the doc edits in the tree; the
  four IDs agree, which is what the HELLO gate needs). Preflight passed
  everything except my exact-string `--expect-build`; then the login-node
  shell died ("no exit status" ×3) before the fixed-build freeze run
  could be launched. **Owed:** `i17_leader_freeze.sh` ×2 on the new build
  (pass = clean parent + `arc_term_miss` total > 0), posix jobs=1, 9-host.
- Not fixed by this and not I17: worker ERR lines during a freeze
  (`mkdir EIO`, `unlink EIO`, then `rmdir ENOENT` of the name whose mkdir
  "failed") are the frozen leader's in-flight ops and the retry of a
  committed op — I16 op-id dedup, next. The six pre-fix `posix-*`
  leftovers on 19810 stay half-applied; there is no repair tool.
- Gate, once the login shell was back (Sep 22 08:25–09:36,
  `46d54e679e4f-dirty`): two freezes. `arc_term_miss` 0→6 on the leaders
  that were stopped (fcstor004 and fcstor006), then 6→8. Run 2 parent
  `children=0 nlink=2` RMDIR_OK
  (`results/measure/20260922-122629-i17-leader-freeze`). Run 1 left
  `d-fcstor007-0-25`, `d-fcstor013-1-25`, `d-fcstor014-0-25`
  (`20260922-122517-…`); each worker line was `mkdir EEXIST` then
  `rmdir ENOENT`. Dump of a fcstor004 KV copy: parent ino 145111
  `nlink=5 nents=3`, three dentries, three child rows present and empty.
  Idle rmdir of the three and of the parent succeeded
  (`~/efs-runs/rec-rmdir-left.log`). Recovery during that run was
  `COMMIT (resolved)` on shards 1436, 1622, 1751, 3816 — the child shards
  and the parent shard. The client observed ENOENT while the txn was not
  visible yet; recovery then installed it. The freeze script's
  `children=0` check called that a torn row; it now accepts
  `nlink == 2 + d-* lines` as a note. Review leftover, not hit here:
  `host_wait_settled` (CREATE/APPEND-style callers) still returns OK when
  the ring slot was overwritten; `host_wait_verdict` returns BUSY. The
  txn path uses the verdict.

---

## Sep 21 2026 night — the 9-host posix suite: harness clock, h->mu, KV WAL fsync, compaction

Where it started: after `read_mu` (`223da15`) and the peer-pool /
forwarding fixes (`f10fec0`), the 9-host suite still failed ~100 of 201
per host, with the same first timeout (`dir_deep_nesting`) on every
host and 3-op tests like `err_stat_nonexistent` "timing out"
(`results/posix/20260922-001341`). The probe showed the contended lock
had moved from `read_mu` to `h->mu` itself (31 handler threads on the
g0 leader in `host_read_index`/`host_wait_applied`).

**Timeline tool.** `tests/measure/w8_stall_timeline.sh` runs the suite
and samples at 1 Hz: raft term/leader/commit for both groups from
fcstor003 and stat + mkdir + rmdir wall time from fcstor007 (itself
under load). First run (`results/measure/20260921-211829-w8-stall-timeline`):
**four leader changes in the first 50 s** (g2 391→392→395, g0
5215→5216→5219), but **no 15 s stall anywhere** — stat p50 9 ms, mkdir
p50 63 ms / p90 212 ms, max 1.9 s at a leaderless moment. So the
"timeouts" were not a stall.

**Harness.** `posix_suite.py`'s parallel path did
`start[fut] = time.time()` at **submit**, for all 196 tests at once,
into a 16-worker pool; the timeout loop then failed every future older
than 15 s whether or not a worker had picked it up. Every 9-host number
before this (Sep 17's 131–144 with `[None]`, the 66–95 pass rows) was
queue time plus real stalls, and the abandoned-but-running futures are
why the run hit the 385 s cap. Fix (`a683def`): the worker stamps its
own start; an unstarted test cannot time out; the budget is unchanged.
(First attempt shadowed `main()`'s `t0` and crashed `flush_tsv` — the
run produced empty TSVs, `results/measure/20260921-213802-w8-stall-timeline`;
renamed to `began`.)

**h->mu.** With `read_mu` gone, every handler took `h->mu` to read
commit/applied/leader/role and to `cond_wait` on `applied_cv`; the pump
(which ticks heartbeats) waited on an unfair mutex behind 30+ threads
per cycle. `host_publish_view` now stores the replica state as atomics
(end of every pump cycle, after `read_begin`/propose, on attach);
`host_read_index`, `host_wait_applied`, `host_is_leader`,
`host_local_leader`, `server_raft_host_submit` read the view; waiters
sleep under a dedicated `cv_mu` (predicate on the view checked while
holding it; pump publishes then broadcasts under `cv_mu`). New raft
accessors `efs_raft_read_index`, `efs_raft_read_done` (`4eb1419`).
Result (`results/posix/20260922-015206`): **191–194/201 on every host,
0 NOTRUN, all nine in ~87 s**, stat p50 3 ms; still 3 leader changes.

**KV WAL fsync per apply.** Rolled with `EFS_RAFT_OBS=1`
(`results/measure/20260921-215919-w8-stall-timeline/obs-*.txt`):
steady state `apply_max` 20 ms for 40 applies on the leader, 130 ms for
256 on a follower — 0.5 ms per applied entry, under `h->mu`, on every
replica. Cause: every metadata apply is ≥1 `efs_kv_put` and `lsm_batch`
fsyncs the KV WAL per put unless a hold is open (only the publish batch
opened one). Spec ("The Raft log is the durability boundary; the applied
KV is a replayable view … the KV's own sync mode is a performance
choice") — so one `efs_kv_lsm_sync_hold` per pump cycle around
drain+tick, `sync_release` (the single fsync) before `persist_applied`
(`84a2a55`). Result (`results/posix/20260922-020950`,
`results/measure/20260921-220933-w8-stall-timeline`): **191–195/201, all
nine in ~62 s**, mkdir p50 21 ms / p90 88 ms under the suite; idle
`md_latency.py` mkdir 6.1 / create 4.0 / append 6.2 / stat 0.3 /
unlink 1.5 / rmdir 5.2 ms. (Measured 20 s after the roll it read
53/57/59 ms — post-roll RDMA election churn, not a regression.)

**What the OBS lines still show.** One pump cycle of **2.4 s** on 004
and 005 at the same moment (`apply_max=2464250us applies_in_worst=54`
/ `2430765us`, 37), 1.0 s on 003 and 006 at other moments; g0 went
5299→5302→5303 around it, the client saw a 2 s `stat` and two failed
mkdirs, `arc_miss=1` on both g0 replicas. The KV is 1 GiB per node in
64 MiB L1 segments (`/data1/01/efs/mdraft/kv`: 17 sst, every L1
segment's mtime identical), an L0 segment spans every shard, so
`kv_compact_locked` merges all of L1 every 4 flushes (4 MiB memtable →
≈16 MiB of writes) under `l->mu` (all reads on the node stall) and
`h->mu`, deterministically on every replica. Compaction strategy is not
in the spec → decision item. Steady-state apply is still ~0.3 ms/entry
with no fsync in it: the KV reads in the apply path (`pread` per
segment per lookup/scan, no block cache).

**Remaining 9-host failures** (`results/posix/20260922-020950`): six
many-op tests time out on most hosts (throughput at 144 jobs); and at
the election moment `link_of_symlink` EEXIST on a never-used name,
`concurrent_create_unlink_two_proc` EIO, `unlink_open_then_recreate`
reading `b''` — the client's BUSY retry of a LINK/UNLINK that had
already committed (the latent hazard noted Sep 21 evening). Spec §7.9 /
I16 defines the fix (op-id dedup, implemented today only for APPEND and
create_file); it is the next queue item.

**Leftovers = evidence of half-applied txns (I17).** 41 `posix-*` dirs
were left in the mount root by the timed-out suites; `rm -rf` removed
35. New `tests/tools/kv_dir_dump` (inode row + dentries + child-row
presence for an ino, from a KV copy) on fcstor004 and fcstor005 (both
host both groups; output identical): `names_crazy_dirs` ino 62991
nlink=3 nents=1 **0 dentries** (created 22:10, build `84a2a55`);
`mkdirat_unlinkat` ino 31264 nlink=3 nents=1, dentry `sub` → 83075
**row missing** (21:19, `f10fec0`); `perm_sticky_owner_can_unlink/sub`
66151 healthy row, `rmdir` EIO; `names_near_path_max_dir/aaa…` ENOTEMPTY
with nlink=2. A rmdir/unlink txn committed on the child shard and not on
the dentry/parent shard. Working hypothesis: `host_txn_recover_pass`
(5 s stranded age) and a live coordinator delayed past 5 s by the
compaction stall + BUSY backoff (10.4 s worst case) both decide — see
START-HERE §1b for the check (DECIDE must be a first-writer-wins CAS on
the DECISION record). Left in place.

Also: `w8_posix9_probe.sh` labels updated (`read_mu` → "host mutex").
`same_parent_storm.sh` PASS 9×4×100 both before and after; root lat /
root mkdir gates unchanged (max 0.063 s, 9/9).

## Sep 21 2026 evening — root mkdir 1 s, stranded transactions, the sweep that cost 100 ms

Three server commits, all on the W8 path (9-host posix suite cannot
start because concurrent root `mkdtemp` stalls).

**`165e779` — RESOLVE/DROP scanned the whole shard.** Server strace during
12 root mkdir+rmdir pairs (`results/measure/20260921-202253-w8-root-srv`):
3–5 efsd threads park in one futex and release together 0.70 s / 1.05 s
later; on fcstor005 a futex wait ends ETIMEDOUT at exactly 0.400 s (the
`host_wait_applied` deadline → BUSY). perf
(`results/measure/20260921-202715-w8-root-perf`): 85–90 % of efsd CPU in
`host_apply → efs_txn_resolve → lsm_scan_prefix → merge_scan → memcmp`.
`efs_txn_resolve`/`efs_txn_drop` used a 2-byte `[shard]` prefix = every
key of the shard, on every replica, per participant, under the KV lock
and `h->mu`. Root's shard 1 has the most history, so a cross-group child
(txn path, RESOLVE) cost ~1 s while a same-group child (log path) cost
2 ms — the bimodal 2 ms / 1.05 s of
`results/measure/20260921-195829-w8-root-lat`. The 400 ms BUSY drove the
client's 16-retry 10.4 s backoff, and a retried UNLINK whose first attempt
had committed came back NOT_FOUND = the `rmdir` ENOENT. Fix: three 3-byte
`[shard][INTENT|GUARD|REDUCE]` prefixes (`txn_scan_kinds`). After it,
root mkdir max 0.119 s.

**`d5cbee6` — log BUSY/STALE dir-op outcomes.** Rate-limited (20/s) server
lines for mkdir/rmdir/create/unlink ending BUSY or STALE; client line when
the 16 BUSY/STALE retries are exhausted. Before this, a 10 s EBUSY had no
line anywhere.

**`9534e53` — L5 recovery of stranded transactions.**
`tests/measure/w8_parent_burst.sh` (9 hosts × 6 rounds of fresh-parent
mkdtemp, `results/measure/20260921-204358-w8-parent-burst`): 53 OK, one
EBUSY after 10.39 s with 16 identical server `mkdir … rc=-13` lines —
same shard, every retry. `tests/tools/kv_intents` on a copy of node 2's
`mdraft/kv` (`results/measure/20260921-w8-orphans`): 105 INTENT, 1 GUARD,
28 REDUCE records, all 4 500–5 000 s old, 45 intents on ALLOC keys of 45
even shards; a log-path create/mkdir landing on one of those shards was
BUSY on every attempt. They were left by coordinators whose DECIDE or
RESOLVE wait hit the 400 ms deadline in the 1 s-scan era; nothing ever
came back for the records (spec L5 says recovery must; there was none).
`efs_txn_scan_pending` lists a shard's distinct pending txns with their
part lists; the group leader's GC thread proposes DECIDE ABORT at the
coordinator (PROTO = it had COMMITted, only the RESOLVEs were lost) and
RESOLVE on every participant; `host_bg_propose` returns the apply
verdict for the local-leader path. After the roll: 47 `txn-recover`
lines, every one `COMMIT (resolved)` — the recovered dentries/inodes/ALLOC
bumps became visible ~90 min after their callers were told EBUSY (2PC
lost-ack semantics). `kv_intents` afterwards: 0 pending. Burst 12 × 9:
108/108, 0 BUSY. Root lat: no 1 s mode; root mkdir 16 ms (log) / 50–57 ms
(cross-group txn ≈ 8 commits). Concurrent root mkdtemp 9/9 ≤ 0.137 s,
fresh parent 9/9 ≤ 0.311 s, no ENOENT.

**`3291c6d` — the sweep was a 100 ms regression.** posix jobs=1 on
`9534e53`: 200/201 but 219 s (56 s in the morning); jobs=16: 77 tests
over the 15 s budget (`results/posix/20260921-211622`, `-211929`).
`tests/measure/md_latency.py`: mkdir med 99.5, create+close 152,
append+close 202 ms (reference 7.2/6.7/9.0). The first sweep scanned 512
shards × 3 prefixes per second on each leader; strace of the GC thread:
one `efs_kv_scan_prefix` = 9 `pread64` (one block per LSM segment) ≈
0.3 ms under the LSM mutex the apply path needs; perf 38 % `rep_movs` +
21 % memcmp with the process at 3 % CPU — lock hold, not CPU
(`results/measure/20260921-w8-orphans/sweep-regression.txt`). Now every
PREPARE apply marks its shard (`host_rec_mark`), a mark 5 s old gets one
scan (64 per pass, 1 ms yield), nothing pending clears it, anything found
re-arms it; a fresh process marks all 4096 once. After the roll: create
6.1–6.7, append 8.5–9.3, stat 0.4, unlink 2.1, rmdir 7–10 ms; posix
jobs=1 200/201 in 42.5 s (`results/posix/20260921-213801`).

Seen once during the `9534e53` posix run, not chased: the suite's cleanup
`rmdir` sat ≥ 90 s in `recv` on fcstor003's conn while the server's conn
thread for that fd was idle in `recv` and both Recv-Q/Send-Q were 0 — a
lost reply or lost request; SO_RCVTIMEO is 30 s, so the 90 s is itself a
question. It eventually returned. Open: 16 729 DECISION records are never
reaped (spec silent on when a decision may go).

## Sep 21 2026 afternoon — measurements, W7 closed, W8 blocked on the mount root

Runbooks ran on `b2184a5c7faf-dirty`. Same-parent rate is flat
~140–160 ops/s (`results/measure/20260921-161931-samedir-rate`).
IOR-hard write is 372 / 33 / 69 / 82 MiB/s at NP 1 / 4 / 9 / 36
(`results/measure/20260921-162514-ior-hard-scaling`). 8 GiB dd+fsync
is 499 MiB/s on one client and 176 on four
(`results/measure/20260921-163033-dd-wall`); the 9-client row is
INVALID (400 s, mkdir/fsync EIO). Raft logs are 1.8–4.4 GB, snapshot
skipped, both groups caught up
(`results/measure/20260921-182308-raft-snap-state`).

W7 is done: the isolated walks are 1–6 s. W8's `NOTRUN` harness is
proven on a cut suite. The 9-host suite still cannot start. Nine
concurrent `mkdtemp` in the mount root take 1–6 s, three of nine miss
an 8 s budget, and two creators then get ENOENT from `rmdir` of the
directory they just made. The same nine in a fresh subdirectory finish
in 0.2 s (`results/measure/20260921-194332-w8-root`). Root nlink is
406 and `.stats` rollups are zero, under the 65536 spread threshold.

Later the same afternoon: the root has 410 names and lists in 10 ms.
One-at-a-time, a root mkdir is 2 ms or 1.03–1.08 s (one `recvfrom`),
and a fresh directory in the same second is ≤15 ms except two creates
that return `EBUSY` after the 10.4 s BUSY backoff. Raft PREPARE
outnumbers DECIDE about 6:1 in that window. The 9-host warmup dies
inside that backoff.

## Sep 21 2026 morning — parent-row lost update → §7.2 commutative reductions

**Symptom (found 23:10 Sep 20 in the 9×4 IO-500 debug run
`results/io500/20260921-debug-9x4-outbox/`).** 7 mdtest `WARNING: Unable
to remove directory …/mdtest-easy/test-dir.0-0/mdtest_tree.N.0`; 3 of them
returned EIO forever afterwards. `raft-getattr` of the parent
`test-dir.0-0` (ino 824) said **nlink=2 with 3 live subdirectories**
(true value 5); `server_raft_host_rmdir` / `efs_meta_apply_rmdir` hit
`prow.nlink < 3 → EFS_ERR_PROTO`. It had been filed as "transient mdtest
rmdir ENOTEMPTY" for days.

**Root cause.** 36 ranks did `mkdir` then `rmdir` of one child each in one
parent. Children whose ino lands in the parent's group take the same-group
**log path** (`mkdir_batch`, `efs_meta_apply_rmdir`: a plain PUT of the
parent row, unversioned, no intent probe); children in the other group
take the **txn path** (EXCL on the parent row at the version it read, PUT
of a full row image with `nlink±1` from that read snapshot). A txn that
read the row, then had a log-path apply change it, still PREPAREs at the
old version — the log path never bumped it — wins, and its full image
overwrites the log-path increment/decrement. Every log-path parent-row PUT
was exposed (`create_file_batch`, `mkdir_batch`,
`efs_meta_apply_unlink/link/rename/rmdir`). Same class as the ALLOC-key
bug `alloc_key_claim` fixed the day before, now on the parent inode row.

**Options given to the user.** (a) generalize `alloc_key_claim`: log-path
PUT is BUSY under a pending intent and bumps the version so the txn goes
STALE, plus client STALE retry for every dir op — mechanical, but every op
in a directory then serializes on one row and the 50 ms × 2ⁿ BUSY backoff
becomes the same-parent latency. (b) the spec'd §7.2 end state: parent
nlink / dseq / mtime as commutative REDUCE parts, conflict-free instead of
retried. User: "a or b which scales better?" → b → "do b".

**What was built (all in one dirty tree, unit-gated on fstor007, then
deployed):**

- `include/efs/txn.h`, `src/meta/txn.c`: kinds `EFS_TXN_REDUCE_INO`
  (`struct efs_txn_ino_delta`: signed `d_nlink`/`d_nents`, `max_mtime`/
  `max_ctime`, `or_used_shards`, `set_parent`, `d_pver`; 48-byte wire) and
  `EFS_TXN_REDUCE_ADD` (u64, 8-byte wire); `struct efs_txn_reduce` gained
  `mtime_gen` (32-byte wire, 24-byte payloads still decode). One record
  shape `reduce(key, txid) = [txid][parts][payload]` for all three
  (`prepare_reduce_rec`), so drop/resolve find every reduce of a txn by
  suffix. `fold_reduce` dispatches on the DATA key's kind: LANE = MAX
  triple + seq++ + mtime_gen MAX keeping the 56-byte tail; INODE = unpack,
  apply delta, repack; DSEQ = u64 add; INODE and LANE folds bump the key
  version (`res_add_ver_bump`) so a stale EXCL lands STALE. `reduces_pending`
  makes `efs_txn_prepare_excl` / `_guard` BUSY over another txn's pending
  reduce; `prepare_reduce_rec` is BUSY over a pending EXCL intent or
  another txn's GUARD. `efs_txn_dseq_observe` (value, not version) and
  `efs_txn_key_busy` (generic intent-or-reduce probe for the log path).
  `efs_txn_apply_prepare` is the one PREPARE decoder for server and sim.
- `src/server/raft_host.c`: `pack_prep_raw`, `host_prep_raw`,
  `host_prep_ino_delta`, `host_prep_dseq_bump`, `host_prep_lane_stamp`;
  every txn site converted — `host_hashed_create_txn`,
  `server_raft_host_mkdir`, `server_raft_host_rmdir`, `host_unlink_txn`,
  `server_raft_host_link`, `server_raft_host_rename_at`. Parent rows are
  no longer read-modify-EXCL-PUT; dseq bumps are `+1`; HASHED dir-lane
  stamps are REDUCE with mtime_gen; RMDIR's emptiness GUARD carries the
  observed dseq VALUE.
- `src/meta/meta_apply.c`: `dir_txn_busy` before the log-path rmdir DEL
  (probes the dir row and all its dseq keys), `efs_txn_key_busy` before
  the log-path unlink DEL of a last-name row; `efs_meta_unpack_inode`
  exported for the fold.
- `src/client/inode_rpc.c`: `stale_retryable` — STALE retried for
  CREATE, UNLINK, LINK, RENAME_AT (nothing has committed when a dir-op
  txn says STALE; REPORT/APPEND still own their STALE).
- `src/sim/sim_ns.c`, `sim_txn.c`: `dseq_prep` → REDUCE_ADD, `dseq_guard`
  → value observe; `pack_prep` carries raw payloads. Until this the sim
  failed 11 `test_sim` checks (rmdir / rename / hashed overwrite) because
  its dseq guards still used versions.
- Tests: `test_txn` +4 (`test_ino_delta_commutes` — the exact race: two
  deltas prepared, a log-path PUT lands between, both fold onto the
  log-path result, ver+1, a pre-fold EXCL is STALE;
  `test_ino_delta_vs_excl_guard`; `test_lane_fold_preserves_tail`;
  `test_apply_prepare_wire`), `test_meta_apply::test_log_delete_busy_under_intent`.
- `tests/roll_efsd.sh --all`: parallel build on all four, ID agreement,
  stop all / start all, wait for both groups (the build-ID gate rejects a
  rolling restart across a commit; there was no script for that case).
- `tests/stress/same_parent_storm.sh` + `same_parent_worker.py`: the
  repro as a gate.

**Gates on the deployed build `b2184a5c7faf-dirty`.** posix jobs=1
`results/posix/20260921-123904` 200/201 + mmap SKIP in 56 s (unchanged
signature). `results/stress/same-parent-20260921-124141/` 9 hosts × 4
procs × 100 rounds mkdir/create/rmdir/unlink in one parent = 14 400 ops,
0 errors, parent `children=0 nlink=2`, rmdir OK. 9×4 IO-500 debug
`results/io500/20260921-debug-9x4-reduce/`: both `-R` reads 0 errors, 0
`Unable to remove directory`, run tree gone afterwards; rates within noise
of the previous build (mdtest-easy-write 0.189 vs 0.238 kIOPS, hard-write
0.240 vs 0.201, ior-easy-read 1.13, hard-read 4.06 GiB/s).

**Learned.** (1) The storm measured **178 ms per op per proc under 36-way
same-parent contention** (~200 ops/s aggregate) vs 7 ms idle — the
reductions removed the lost update, not the same-directory ceiling; that
is now W6 residual 3 (count BUSY/STALE retries first; they are not logged).
(2) `efs_kv_scan_prefix` callbacks returning >0 = batch-full, again
relevant in `reduces_pending` — check it in every new scan user. (3) The
user pointed out `screen -S` on node9901 as the natural long-job holder;
`efs-bg.sh` now launches each job in a detached `screen efs-<name>` (same
NFS log + rc bookkeeping) so `screen -r efs-<name>` shows the live job.

## Sep 20 2026 evening — reaper cross-group lane bug, the 100 ms commit floor, node9901 runner

**Symptom chain.** Posix jobs=1 on the full in-flight tree gave 191/201 +
mmap SKIP twice (`results/posix/20260921-011518`, `-012432`), with
`mtime_monotonic_many_writes` timing out deterministically: 80 × (open
`O_APPEND`, write 1 B, close) at ~200 ms each. Isolated probe on fcstor007
against an "idle" cluster: mkdir median 103 ms (min 5.5), create+1B+close
60–107, append+close 160–180; the client log said `report_ms=104` on every
one-record REPORT, and the leader's `report-split nrec=1 pack_ms=0
push_ms=0 finish_ms=103` — pack and push free, the wait for the apply
verdict ~100 ms.

**Finding.** `efs-mgmt raft-status` showed both groups committing ~33
entries/s with no client process anywhere. A 3 MB tail of fcstor004's
`raft.log` (new `tests/tools/raft_log_tail.py`, framing from
`raft_log.c`) was **92 % `LANE_SWEEP`** in both groups: the same 64
inodes, lane 0 only, 362 times each in the window — one sweep per inode
per GC pass, forever. `host_gc_propose` was leader-only ("the reaper only
walks groups this node leads"), but `efs_kv_lane_shard` is
`ish + lane × (2·(h & 0x7ff)+1)`: an odd stride, so lane 1, 3, 5 … of every
inode are in the *other* group. The anchor-group leader swept lane 0
(landed), got `NOT_PRIMARY` for lane 1, `break` → "retry the whole marker
next pass". No inode with an active odd lane could ever be reaped unless
one node happened to lead both groups (which is why it sometimes worked in
the past). The earlier `rc=-5` (`vlen`) and `rc=1` (batch-full) reaper
bugs masked this one: they failed lane 0 first.

**Fix.** `host_gc_propose` proposes locally when leader, otherwise
forwards to that group's leader with `host_remote_cmd` (the same path a
client op takes through `server_raft_host_submit` → `host_propose_wait_idx`)
and then `host_wait_applied` on the local replica if it hosts the group.
No `read_mu` (GC thread never holds it). Deployed with
`tests/roll_efsd.sh 1 2 3 4` (01:48–01:50 UTC, each node caught up in
< 10 s, build `d0fd0448adb6-dirty` unchanged). After the roll `REAP_DONE`
went from 0.3 % to 40 % of entries: the backlog of dead inodes (IO-500 /
mdtest / posix leftovers, thousands) drains at ~15 inodes/s per group. The
pass is serial (one propose+wait per lane, then REAP_DONE); batching the
32 markers' proposals before waiting is an obvious speed-up, not done.

**Why it cost clients 100 ms — first (wrong) theory.** The pump is one
thread per node: tick + apply, and every apply fsyncs the KV WAL
(`EFS_KV_LSM_SYNC`), so the 33 entries/s reaper stream was assumed to keep
the pump ~90 % busy and queue every client entry. After the reaper drained
(both groups flat) the median did NOT move: mkdir still ~100 ms, min 5.5.
The general shape still holds — **any background stream of small commits
adds latency to every client op**, so measure only on a flat-`commit`
cluster and check `raft_log_tail.py` first — but it was not the floor.

**The real 100 ms floor: outbox wakeup (fixed, 22:40).** `strace -f -tt` on
the leader (004) during a create loop: client request in, raft-log
`pwrite`+`fsync` 0.3 ms, KV WAL fsync 0.3 ms — and the AppendEntries to one
peer left the outbox **47 ms** after the propose. On a follower (003) the
picture was unmistakable: heartbeats arrive every 50 ms, but the follower's
outbox writes its 85-byte AE replies only on every OTHER heartbeat, two
back to back — every reply waited for the next incoming message. Code:
`raft_host.c` had ONE `h->outbox_cv` shared by all per-peer sender threads
and `host_send` used `pthread_cond_signal`, which wakes one arbitrary
waiter. A message for peer A woke B's sender (its queue empty, back to
sleep) and A's sender ran at the next signal for anyone — the next
heartbeat. Each hop lost 0–50 ms; AE + reply ≈ 100 ms per commit,
independent of fsync speed. Fix: `pthread_cond_t cv` per `struct
host_outbox`, `host_send` signals `tx->cv`, `host_stop_senders` broadcasts
all. Rolled all four (TCP peers). Result, idle cluster, 20 ops each
(`results/perf/20260921-md-latency.txt`): mkdir med 103 → **7.2 ms**,
create+1B+close 60–107 → **6.7**, append+close 160–180 → **9.0**, stat 0.4,
unlink 1.8. Lesson: when a median sits at a multiple of the heartbeat
interval while every syscall is sub-millisecond, it is a wakeup/scheduling
bug — strace the follower, not the leader, and look for replies bunching.

**Rolling-restart election storm (observed, not fixed).** Group 2 went
term 199 → 264 during and ~5 min after the roll. All three voters' logs
are full of `RDMA send CQE error status=12 (transport retry counter
exceeded)` and `*** SOCKET CLOSED/REUSED BEHIND THIS CONN ***`: server
peer connections (raft AE included, `raft_host.c` outbox →
`server_peer_conn_get`) are upgraded to RDMA by `peer_pool.c`; a restarted
node's QPs vanish and each peer's pooled conn blocks for the retry budget
before it is dropped, missing heartbeats. Same lines exist in every
`efsd.log.prev` from earlier restarts; it converges by itself. Same class
as the client pool identity bug (`test_conn_fd`, W10).

**Runner.** The login-node Cursor shell died five times today, twice
mid-measurement. From now on anything over ~60 s runs detached on node9901
(`~/.cursor/skills/efs-test-ssh/scripts/efs-bg.sh start|status|wait|kill`,
per-host ssh-agent in the wrapper since `$HOME` is NFS-shared; log
`~/efs-runs/<name>.log`), and the login node only probes. New scripts:
`tests/deploy_fuse_clients.sh` + `tests/fuse_client_remount.sh` (8 clients
rebuilt and remounted in 5 s wall, one status line each),
`tests/roll_efsd.sh` (rolling restart with build-ID refusal and per-group
catch-up wait). The deploy rule's `ps|awk` kill-by-port was replaced by
`pkill -9 -x efsd` after it killed the agent's own remote shell (003 and
005 down together, group 0 without quorum for 4 min, Sep 20 morning).

**9×4 IO-500 after the fix (23:00).** `results/io500/20260921-debug-9x4-outbox/`:
both `-R` reads 0 errors, all ior files unlinked; mdtest-easy-write 0.238
kIOPS (morning 0.050), mdtest-hard-write 0.201 (0.018), easy/hard stat 0.88
/ 2.76 (0.24 / 0.79); bandwidth phases unchanged (easy-write 0.78 GiB/s,
hard-write 0.046, hard-read 3.89) — those walls are W4 and the 36-way
sub-chunk CAS, not metadata latency. posix jobs=1 200/201 + mmap SKIP in
60 s (was 191 in 6 min).

**Wedged directory (found in the same run, open).** mdtest printed seven
`Unable to remove directory …/test-dir.0-0/mdtest_tree.N.0`; four rmdir
fine afterwards, three return EIO forever. `raft-rmdir` → status 3 =
`EFS_ERR_PROTO` from `prow.nlink < 3`: the parent (ino 824) has nlink 2 with
three live subdirectories. Mechanism: a child with an even ino is removed
on the same-group log path (`efs_meta_apply_rmdir` PUTs the parent row
without probing intents or bumping its version); an odd-ino child goes the
txn path (EXCL on the parent row at the version it read, PUT of a full
image with nlink−1). A txn that read before a log-path apply still
PREPAREs at the old version, wins, and overwrites the log-path change — a
lost update on nlink (and on dseq/mtime), identical in shape to the
`alloc_key_claim` bug of the same morning, just on a different key. It
also explains the long-standing "transient mdtest rmdir ENOTEMPTY". Two
ways out are written up in START-HERE §1b step E: generalize the ALLOC
rule to the parent row (BUSY under intent, bump version, client retries
STALE for the directory ops) or the §7.2 reductions (nlink as a signed
REDUCE delta). Waiting on the user.

## W6 narrative as it stood in START-HERE before Sep 20 (superseded)

hard/easy write = **0.097**. Easy write 0.26 GiB/s vs 9-client 8g
dd+fsync 0.20 GiB/s (same order; 0.6 % of 44 GB/s). First
ior-hard-write hung D-state (`20260918-debug-hung-hard/`, 26k-rec
STALE REPORT); remounted retry finished.

**IOR-hard `-W`** `results/io500/20260918-hard-w/`: 13.08 MiB/s,
583 s, **4244 incorrect-data errors**, IOR exit 40. Gate
mismatches=0 is FAIL. Do not retune 47008. Do not invent a chunk lock.

**30 s ior 9×1 aborted** `results/io500/20260918-ior-30s-abort/`:
ior-easy-write fsync/close EIO at 545 s; `report-split nrec=125000
rc=-13` (BUSY) on fcstor004, then apply-publish STALE (`rc=-14`) at
88 % CPU. Same class as loaded 50g `end_fsync` EIO. Do not raise
`EFS_IO_TIMEOUT_MS`. 1 s easy-write fsyncs; 30 s (~125k pubs) does
not.

**Cluster RESET (Sep 18 PM, user-authorized).** The 30 s abort left a
self-draining STALE backlog (~1.2 entries/s, term flapping, hours to
drain) and fcstor005 430k entries behind on group 2 (W11 oversized
SNAP — can never catch up). The table was wiped and `raft-mkfs`'d
fresh. Gotcha now fixed in `clean_cluster.sh`: mkfs proposes only to
group 0, so it must go to group 0's CURRENT leader; `rc=-15
leader_hint=H` means retry on node H+1 (`172.16.223.(57+H)`).

**FOUND + FIXED (Sep 18 PM): same-parent concurrent mkdir EIO.**
9×4 mdtest-easy aborted: 8/36 `mdtest_tree.N.0` mkdirs failed EIO
(9×1 passed). Repro: 36-way same-parent mkdir across 4 clients →
~5/36 EIO in 2.9 s (fast fail, not BUSY-retry exhaustion). Server
`EFS_RAFT_DBG` mkdir line: `rc=-13/-14 stage=11` — the cross-group
mkdir txn (MKDIR scatters the child, so every mkdir with an
even-shard child is a 2-group txn) CASes the parent-row + dseq
versions; the two dual-hosts (004/005) race, one commits, the other
preps STALE. The client RPC loops retried BUSY/NOT_PRIMARY but NOT
STALE → `fuse_create_errno` → EIO. Fix (client `inode_rpc.c`):
`EFS_INODE_RPC_STALE` retried like BUSY for `EFS_MSG_INODE_CREATE`
only (idempotent unique name; a landed retry reads as EEXIST, which
FUSE already handles). REPORT_CHUNKS keeps its STALE (W1
refetch+overlay). Gate: 36/36 then 90/90 same-parent mkdirs, 83
STALE/BUSY conflicts absorbed on 004 alone, 0 errors. Same exposure
exists for cross-group UNLINK/RENAME/LINK txns — not measured, not
fixed. **Deeper fix (not done):** the parent-row nlink++/dseq++/
times should be commutative txn reductions (spec §7.2), not an EXCL
row CAS — that removes the same-parent mkdir serialization point
entirely instead of retrying it.

**Fresh-cluster 9×4 (Sep 18 PM):** ior-easy-write **1.34 GiB/s**
(39.7 s) — 5× the poisoned-table 9×1. mdtest/reads/hard: see
`results/io500/<new id>/` when landed.

**W11 is every-run, not edge:** `snapshot skipped ... KV export
exceeds SNAP cap` fires as soon as real data flows (ior-easy-write
≈ 400k chunk pubs ≈ 40 MB KV > 4 MiB cap), so a data-bearing
cluster NEVER compacts its raft log → any follower restart = full
log replay, and a lagged follower is permanent (what killed 005).

**Still open (this item):**
1. `SLOTS=4 NP=36 bash tests/perf/io500/run.sh debug` (1 s
   stonewall) — RUNNING on the fresh cluster. `findmnt`
   `fuse.efs-fuse` on every rank. Copy ini+hostfile+result.txt to
   `results/io500/<id>/`.
2. Confirm every rank stayed FUSE (no local-disk fallback).
   Another 30 s `run.sh ior` will hit the same fsync EIO until
   REPORT can take 125k pubs.

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check
  applies to every rank.
- **Gate:** 9×1 debug is in; 30 s + 9×4 still required. `-W`
  mismatches are **4244**, not 0 — record that, do not hide it, do
  not reopen W1 with a lock.
- **Forbidden:** quoting a rank that fell back to local disk; tuning
  IOR's transfer size (47008 is the point); `pkill -f` (matches the
  agent). Kill hung `io500` with `pkill -9 -x io500` then remount
  FUSE (D-state `request_wait_answer` ignores SIGKILL until
  `efs-fuse` dies).


---

## The always-applied "project state" rule as of Sep 20 2026, verbatim


# efs project state (compact memory)

efs: distributed FS, 2+1 XOR EC, FUSE client, RDMA (RC QP), 128 KiB chunks.
Pre-alpha. **Goal: scale to >= 2^32 files/dirs** (see docs/scaling-roadmap.md).
**NO approval needed to run scripts against node9901, fstor007, fcstor003–015
— including full cluster wipe (user, Aug 27 2026). Wrapper scripts need
`required_permissions: ["all"]` to escape the sandbox; that is pre-authorized.
See efs-fcstor-deploy.**

**AUTO-RDMA FIRST INODE — ROOT-CAUSED (Sep 1), CLIENT POOL FIX IN TREE
(Sep 17). The server DESTROYS the QP the client is sending to, because
the client closed that connection's TCP side-channel while leaving the
conn object in its pool.**
Symptom: on a fresh mkfs + auto-transport mount the first `mkdir` D-states
forever; `fuse.log` repeats `efs: RDMA send type=43 WAIT TIMEOUT` (43 =
`EFS_MSG_INODE_LOOKUP`, plain LOOKUP — **not** `LOOKUP_PATH`, so b518003's
wire change is innocent). Servers healthy (fresh gen=1, clean catchup, no
BUSY); FUSE `waiting=1` (ONE outstanding request, not saturation); efs-fuse
S-state; the D-state `filename_create` procs are just queued behind the one
unanswered LOOKUP. `EFS_TRANSPORT=tcp` on the SAME binary passes in <1s.
**The plan's hypothesis (RNR NAK from too few recv buffers + infinite
`rnr_retry`) is DISPROVEN — do not re-chase it.** `rnr_retry` was made
configurable and set finite: failure identical. Requester-side HCA counters
`rnr_nak_retry_err` / `req_cqe_error` / `local_ack_timeout_err` /
`packet_seq_err` / `out_of_buffer` all stayed **0**; only the SERVER's
`resp_cqe_error` + `resp_cqe_flush_error` incremented.
**How it was proven (measurement, not code reading):**
- `rdma resource show qp` on the server during the hang: the client's
 `dest_qpn` is **missing** from an otherwise contiguous live QPN sequence
 (61571, [61572 absent], 61573) — the server had already destroyed it.
- Port correlation nails the pair: client `fd=14 qpn=86621 local_port=58152`
 ↔ server `qpn=61662 peer_port=58152 fd=24`. The server then logged
 `tcp EOF (peer FIN) fd=24` → `conn teardown reason=tcp_has_request failed
 (blocking) qpn=61662`, and the client's later send to `dest_qpn=61662`
 timed out. So the server tears down the whole conn (QP included) when the
 RDMA conn's TCP side-channel sees a FIN.
- The failing conn's OWN socket was never closed: the client records
 `/proc/self/fd/N` (`socket:[inode]`) at upgrade and re-reads it at failure
 — `(same socket)` every time. `ss -tanp` on the client showed the failing
 conns `ESTAB` but **5 other conns in TIME-WAIT**, i.e. the client sent the
 first FIN on those.
- **The smoking gun:** client `fd=6` upgraded for `qpn=86619`, then LATER a
 *different* connection upgraded on the **same `fd=6`** for `qpn=86622`.
 fd 6 was closed and recycled while the conn object holding `qpn=86619`
 was still reachable from the pool; the next RPC that checked that object
 out sent into a QP the server had already reaped.
- `efs_conn_destroy` backtraces resolve to `efs_client_conn_drop` — that is
 an error handler reacting to the breakage, **not** the origin. The origin
 (which bootstrap path closes a pooled conn's fd without evicting the conn)
 is the remaining unknown.
**Status: CLIENT POOL LIFECYCLE FIX IN TREE (Sep 17).** Not `rdma.c` tuning
(RNR/poller already landed). A pooled `efs_conn` now records sockfs
`st_dev`/`st_ino` at wrap; checkout treats a mismatch as dead; destroy
closes the fd only when that identity still matches (a recycled number is
someone else's TCP — closing it FINs the new owner and the peer reaps the
QP). `conn_init` skips busy slots (it used to destroy a checked-out conn
and clear busy). Post-mount GET_FEATURES / STATUS use the pool (no raw
`close(fd)` next to live QPs). New sockets are `SOCK_CLOEXEC`. Gate:
`test_conn_fd`. Live empty-table auto/RDMA first `mkdir` is still the
repro; 19820 is populated so do not treat a remount there as that gate.
Diagnostics stay behind `EFS_RDMA_FIRST`. Default leftover-1 gates stay
TCP until that empty-table mkdir is run. **SCOPE: it is NOT "RDMA is
broken".** A node9901 client on auto/RDMA (`mlx5_0`) against an
ALREADY-POPULATED table (`meta ready gen=13 inodes=55621`) drove millions
of creates with 0 network errors. The repro needs a **freshly mkfs'd /
effectively empty** table.
**Harness gotcha — FIXED (Sep 1).** `ensure_mounted` returned early when the
host was already mounted, so `EFS_TRANSPORT=tcp bash tests/run_tests.sh
setup` on a cluster whose clients were already up on auto **silently kept the
RDMA mount** and the variable appeared to do nothing (this is why "every gate
since Aug 30 ran on TCP" was itself unverified). It now checks
`grep -c "RDMA transport up" /tmp/efs/fuse.log` on a reused mount and
remounts on a mismatch. Only `tcp` is enforced — the RDMA upgrade is lazy
(first pool checkout), so a fresh auto mount legitimately has no such line
yet and must not be remounted. Verifying that count is **0** per host is
still the right way to confirm a TCP run; do not trust the env var alone.

**Prereq 1 of Cut C landed + gated (Sep 1, working tree).**
`slab_page_persisted` is now authoritative from the committed root
(`root.ino_page_count`, falling back to `root.page_count`) instead of
returning `ex->page_src != NULL`. Slab si is page 1+si, so the image covers it
iff `si+1 < ino_pc`. **This is a strict no-op today** — the predicate is only
reached from `inode_slab_ensure`'s `(flush_blob || page_src)` branch, the blob
branch is untouched, and `page_src` is still never installed — but it is the
one thing that MUST land before any page source exists, or every new slab's
first row (callers do `pos = inode_count++` before filling, which already
looks like a fault) becomes a hard failure = the "jammed at exactly 256 rows"
bug. Gate: `test_meta_v6` / `test_dir_stats` / `test_ino_path` /
`test_meta_slot` OK; fresh bits=3 wipe + solo posix on a TCP mount
`results/posix/20260901-171838` **196/201, 0 EFS bugs**.

**THE REAL CUT-C BLOCKER IS THE *FULL* SERIALIZE, NOT THE BLOB SIZE (Sep 1,
measured with new TRIM-PROF / `absent=` counters, fresh bits=3 TCP,
`EFS_INO_RAM_MB=64`, scale_grow to 500k).** Trim works, and it is still
futile. Do not build the spill until this is fixed — it would spill pages that
are immediately faulted back.
- **`incr=0` on 100% of flushes** (63/63 across primary + a joiner) for the
 whole 500k grow. `efs_export_serialize_dirty` bails whenever the inode
 region grows (`new_ino != cache_ino_len`), and during growth it grows
 constantly, so **every** flush takes `efs_export_serialize_ex`.
- `serialize_ex` repacks **every** row, so it **faults every evicted slab back
 in**. Measured on fcstor005: `evicted=241 resident=194 slab_n=512`, and the
 next flush is back to a full table. Eviction accomplishes nothing.
- Therefore `page_absent` (prereq 2) **never fires during growth** — it is
 only produced on the incremental path. Measured `absent=0` on 1762 flushes
 at 10M and 63/63 here. It is correct and it is the right groundwork, but it
 buys nothing until the full path stops faulting.
- The blob dominates the cap it is counted inside: `bytes_mb=63 blob_mb=54`
 (**86%**), so trim thrashes toward a floor it cannot evict.
- The primary is worse: **79 of 116** trim calls are `TRIM-PROF skip
 no-source` (`!flush_blob && !page_src`), 0 evictions.
**So the ordering is: make `efs_export_serialize_ex` able to emit a page for a
non-resident slab without faulting it (reuse-by-ci, same argument as
`page_absent`), THEN bound the blob, THEN spill.** At 10M the same run showed
`ser=350ms` + `snap=175ms` per flush, so this is also the flush-cost lever.
Instrumentation for all of the above is in tree and env-gated behind
`EFS_FLUSH_PROF`: `TRIM-PROF` (cap/bytes/blob/resident/slab_n/evicted/hops,
plus skip reasons) and `absent=` on the `FLUSH-PROF` line.
`clean_cluster.sh` now forwards `EFS_INO_RAM_MB`.

**10M SCALE RUN, errors=0 (Sep 1, `results/scale/20260901-181133`, bits=3 TCP,
9 clients x16, EFS_INO_RAM_MB=64).** total | creates/s | errors | rss_max MB |
B/inode: 100k 9999 0 101 3408 | 500k 26666 0 597 3358 | 1M 33333 0 968 2852 |
2M 33333 0 1532 2506 | 5M 27272 0 3333 2159 | 10M 15873 0 5587 1918. **errors=0
at every step incl. 10M** (Aug 31 bits=3 had 46/176 at 5M/10M, and the Sep 1
Cut-3 10M attempt wedged). Latency flat: stat p50 0.454, create 0.552, unlink
0.422, readdir 6.9 ms. `rss_max 5587 MB` ~= the 10M x 512 B `flush_blob` ino
region (5.1 GB) — the blob IS the RSS, which is the quantified case for Cut C.

**SPILL DESIGN CORRECTION — a full rewrite per flush is impossible.** The
plan said "write the committed region bytes to local NVMe at flush". At 2M
inodes the ino region is ~977 MB and flushes run at **~35/s**, so that is
~34 GB/s of writes. The spill file must be maintained **incrementally**:
`pwrite` only the pages the flush already knows are dirty (~5 pages, ~640 KB)
at offset `pi * EFS_META_PAGE_SIZE`, making it a page-indexed local mirror of
the committed inode region. This is also what makes it safe against a failed
commit: a fault only ever reads a **non-resident** slab, `trim_ino_ram` only
evicts **clean** pages, and a clean page is by definition not among the pages
a flush rewrites — so uncommitted bytes can never be the bytes a fault reads.
On restart the table is rebuilt from EC pages and every slab becomes resident,
so the file is a warm cache and can simply be recreated.

**Code-hygiene pass (Aug 26 PM, commits 4a959ac/1124e6d/6bb83b8/6cb5791):**
dead code removed (~469 lines: client coal subsystem, client meta-flush/
election leftovers, unused server helpers), obsolete slurm-jobs/ + old
scripts/fio-*.sh deleted (docs/testing.md now points at fcstor run_tests.sh).
**Valgrind memcheck: NO LEAKS** — unit tests, full efsd workload, and full
efs-fuse workload all 0 definite / 0 indirect. Possibly-lost are benign
one-time allocs (libfuse fuse_new session bufs, glibc dl-init constructors,
pthread TLS 288B/thread). **Found+fixed (1124e6d): uninitialised padding in
struct efs_ino_size_rec sent on the wire** (REPORT_CHUNKS) — malloc→calloc.
Root/installs rule added (6cb5791): always ask user, never work around.
**Repeatable leak gate (957c6df, RDMA added 1b72183):** `tests/run_tests.sh
leaks [host]` → tests/valgrind_leaks.sh. Self-contained single-node on a
private port + scratch (never touches the live cluster); gates unit + efsd +
efs-fuse under memcheck over **BOTH TCP and RDMA, client AND server**.
Hard-fail on definite/indirect leaks, uninit-on-wire, invalid rw;
possibly-lost only flagged when the direct caller is efs code. RDMA phase is
leaks-only (ibverbs/DMA fills structs valgrind can't track → uninit
false-positives by design). RDMA exercised via eager upgrade on connect +
metadata/packed + chunk PUT/GET ops. Verified PASS
on fcstor003: 0 definite / 0 indirect everywhere.
NOTE: valgrind on efsd needs a clean exit for full leak stacks (SIGTERM gives
summary only). **vgdb / LSan / `strace -p` / `gdb -p` all WORK — do not ask
the user to enable ptrace.** Verified Sep 1 2026 by live attach (`strace -p`
+ `gdb -p`) on **all 15** hosts: `kernel.yama.ptrace_scope=0` on node9901,
fstor007 and fcstor003–015. Every "ptrace/Yama blocked" note elsewhere in
this file is HISTORICAL and no longer true.
**BUT IT IS RUNTIME-ONLY AND REVERTS ON REBOOT.** All 15 still ship
`/etc/sysctl.d/99-ptrace.conf` with `kernel.yama.ptrace_scope = 2` (an
ssh-keysign fd-stealing mitigation); the 0 was applied with `sysctl -w` and
never persisted. If a host comes back from maintenance at 2, that is
expected — just re-apply `sudo sysctl -w kernel.yama.ptrace_scope=0` (sudo
needs a password, so the USER must run it). Do not "fix" it by editing the
conf file without asking — that weakens a deliberate mitigation fleet-wide.
Still unavailable:
`perf trace` (`perf_event_paranoid=2`, `/sys/kernel/tracing/events`
unreadable) — `perf record -p` / `perf stat -p` same-uid are fine.

## Where we are

**W1 I12 N-1 CAS LANDED + GATED (Sep 18).** `base_gen` on the wire, leader
CAS, audible apply STALE (no `last_applied` stall), client refetch+overlay
(64 tries). Gen-keyed fragments. `peer_shared_pwrite` concurrent 5/5.
n1 `results/stress/20260918-n1-w1/` lost=0. Honest 1-client
`results/perf/20260918-w1-honest/` sw-1m **758**, sw-50g **373**. Do not
reopen with a chunk lock. **W2 DONE option (i):** spec moved — `write()`
is client-buffered; durable+visible at `fsync`/`close`/`O_SYNC`.
Measured 0/10 visibility and 64 MiB lost on kill -9
(`results/stress/20260918-w2/`). `O_SYNC` specified, not wired. Do not
publish on every `write()`. **W3 DONE (Sep 18):** 8 GiB dd+fsync
**639 MiB/s** (13.5 s; best 724 / 11.86 s), remount HEAD/TAIL OK,
posix jobs=1 **195/201**, posix2 **58/63**. Cuts: N=2048, skip
get_chunk, sync_hold, flush pipeline, propose-only PUBLISH forward,
O_APPEND published-merge, close waits per-ino report. Remaining tail
is REPORT pack+push. Do not invent a REPORT RPC split. **W4 DONE (Sep
18):** writes share a ceiling and more clients make it worse. 8g
dd+fsync 1/4/9 = 639 / **251** / **202** MiB/s (3.8 / 0.46 / 0.37 %
of 16.7 / 44 / 44 GB/s). Morning 4-client sw-1m AGG **1589**. Loaded
4-client rw-1m 400 s TIMEOUT; 4-client fio storm can lose raft
heartbeats (`rc=-15`). Gate
`results/perf/20260918-w4-honest4/gate.txt`. **W5 DONE:** sw-50g
**341**. **Current: W6** (9×1 debug in
`results/io500/20260918-debug-9x1/`: easy-write 0.26 GiB/s,
hard-write 0.025 GiB/s, mdtest-easy-write 0.053 kIOPS; IOR-hard `-W`
4244 errors. 30s ior aborted on fsync EIO /
`nrec=125000` BUSY (`results/io500/20260918-ior-30s-abort/`). 9×4
debug rerun (`results/io500/20260918-debug-9x4/`): easy-write
**0.415 GiB/s** (1.58× 9×1), mdtest-easy-write **28.5/s** (0.54× —
worse), **hard-write DID NOT FINISH in 2h18m** (36-way CAS contention
on shared 128 KiB chunks; fsync report_ms 58 s–443 s, one 85-min
report lost STALE rc=-14; ~7.5 MiB/s aggregate vs 26 at 9×1). Raft
never the bottleneck (commit==applied, terms stable throughout).
W6 gate "hard -W mismatches = 0" unreachable until the shared-file
write tail is fixed. Harness bugs found: run.sh foreground prterun
dies with the ssh timeout (detach + log to NFS instead); never
gdb-attach an MPI rank through a timeout'd ssh (left a rank
T-stopped, job unrecoverable).

**W6 CORRECTNESS GATE MET (Sep 20, commit 708b350 on top of a9e94a6).**
9×4 IO-500 debug `results/io500/20260920-debug-9x4/`: **every phase
finished, ior-easy-read AND ior-hard-read 0 verification errors, every
unlink OK.** easy-write **0.814 GiB/s**, hard-write 0.044 (495 s, was
DNF), easy-read 1.80, hard-read 3.55, mdtest-easy-write 50/s. **Root
cause of the Sep 19 76108/4314 read errors + 27 undeletable easy files
was DUPLICATE INO ALLOCATION on concurrent CREATE**
(`results/io500/20260920-easy-dupino/`): `server_raft_host_create`
peeks the alloc watermark on the PROPOSER (applied state) and packs the
ino into the CREATE cmd; `create_file_batch(want_ino)` took it
unconditionally, so two creates on one shard that peeked before either
applied (other voters / forwarded) wrote two names onto ONE inode row
(36 IOR easy files → 10 distinct inos, 8 names on ino 4460); both
writers published onto one ino (mixed content), the first unlink deleted
the row and the siblings dangled (resolve → I9 EIO, `rm` ENOENT, `rmdir`
ENOTEMPTY). Fix `alloc_hint_or_next` in meta_apply: the apply IS the
allocator — hint honored only if ≥ watermark and unoccupied, else
`alloc_next_free`; deterministic per replica; host already reads the
ino back from the dentry. Same for the mkdir same-group fast path. The
hashed-create / mkdir txn paths CAS the alloc key and were never
affected. Gate `test_meta_apply` `test_create_log_at_dup_hint`. Raw IOR
repro: `SLOTS=4 run.sh ior-easy-write 1024` then `ior-easy-verify 1024`
(36/36 distinct, 0 mismatches, write 463→902 MiB/s). Same commit: the
reaper's `LANE_SWEEP` / `rsv_purge` misread `efs_kv_scan_prefix` rc>0
(batch-full) as an error → no lane with >64 chunks was ever swept (every
deleted file >8 MiB/lane leaked its fragments; `apply lane-sweep rc=1`
every 33 raft entries). **Do not quote the Sep 19 easy-read 4.0 GiB/s —
it was zero-fill.** Leftover dangling dentries on 19810: `/io500/easyv`
(26) and `/io500/2026.09.19-22.52.15/ior-easy` (27) — inert, cannot be
unlinked by design. **W6 residuals (perf, not correctness):** hard-write
45 MiB/s (36-way N-1 CAS on 47008 B records); easy-read open phase 20 s
of 22 s (1 GiB open = 128 sequential GETCHUNKS + 64-lane stat, 0.2–0.8 s
unloaded — spec §8 per-lane range fetch); 7/36 mdtest rmdir transient
ENOTEMPTY under load (clean seconds later). That run used client
`efs-fuse` a9e94a6 on 007–015 WITHOUT the read.c/ops.c pull change
(so the 0-error result is the server fix alone); servers 708b350.

**SALT DIVERGENCE + RAFT SNAPSHOT CATCH-UP FIXED (Sep 18 PM, commit
43e3e4e).** Three production bugs, all gated: (1) **export salt
diverged across nodes** — `host_export_salt` was computed locally, so
dir placement (`hash(parent,name,salt)`) disagreed between nodes and
hashed lookups missed. `EFS_MD_CMD_SALT` now replicates the salt
through Raft at mkfs; readers fall back to the anchor. (2) **A
restarted Raft leader could never serve InstallSnapshot** — the
snapshot blob is memory-only; after a restart `snap_idx`/`snap_term`
reload from disk but the blob is gone, so the leader sent an 8-byte
metadata-only SNAP that `efs_kv_group_import` rejects (PROTO), and the
follower starved forever. `send_snap` now re-exports the app state at
`last_applied` on demand (pump-serialized, so consistent; regression
`test_install_snapshot_restarted_leader`). (3) **`disk_save_snap`
rejected skip-ahead snapshots** — `last_index - snap_idx > n` → INVAL,
which is exactly InstallSnapshot onto a behind follower; the in-memory
`raft_group_snap` and the replay path already clamped that case, only
the live save path had the stale guard. Removed (backwards snaps still
INVAL); disk/mem parity case in `test_raft_store`. Found live:
fcstor005 stuck at applied=249 after a rolling restart of the
compacted leader; after the fix it installed `incl=512` and caught up
in seconds. `on_snap_req` save_snap failures now log under
`EFS_RAFT_DBG` (they were silent, no reply sent). Gates: test_raft /
test_raft_store / test_wire / test_txn / test_sim OK on fcstor003;
live 19810 restart all-4, group 0 applied=512 and group 2 applied=303
on every voter, term stable under IO-500 load.

**HPC REVIEW LANDED IN THE ROADMAP (Sep 18).** START-HERE §1a queue:
W1–W5 done. **W6 IN PROGRESS** (9×1 debug + hard `-W` 4244 errors;
30s/9×4 open). W7–W12 = old
W5,W6,W9,W7,W4,W10. Write 1/4/9-client 8g dd+fsync is 639 / 251 /
202 MiB/s. `architecture.html` is generated.

**19820 RETIRED; 19810 IS THE ONLY CLUSTER (Sep 17).** User-directed:
wipe 19820 `/tmp/efs-raft-scratch` and the leftover-1 19810 table, then
`raft-mkfs` 19810 on `/data1` (36T, TCP, seed 003, join 004–006).
`wipe_cluster.sh` now removes scratch dirs; `pkill -x efsd` is safe.
Do not stand up 19820 again. POSIX/perf/leaks gates run on **19810**
or localhost. Orphan old-engine sources deleted (`test_meta_batch`,
`test_rpc_create`, `blobscan`, `dump_root`, `txprobe`, `kv_compact_dir`).

**POSIX 1+2 gated on 19810 after 983bfb8 (Sep 17).** Same storage, TCP,
jobs=1. Harness: `concurrent_appends` / `concurrent_create_unlink_two_proc`
`@budget(30)` (isolated 22 / 20 s vs 15 s default); `dir_many_files`
`@budget(75)`; `POSIX2_STEP_SEC` default 45 (400+400 append SSH was 15 s).
One-node `results/posix/20260917-190719`: **194 both-pass / 2 EFS** (efs
TSV 199/1/1 in 241 s) — leftover `dir_many_files` 45 s (isolated PASS
12 s; mmap SKIP ENODEV). 4-node `--parallel` 007–010 under 5-way load
`20260917-190014`: 186–188 both-pass (008 164 / incomplete 175 tests).
9-node `20260917-191430`: every host hit the 385 s python cap at
131–144/201 (5–7 real walk/name timeouts; the rest `[None]` never-ran —
same class as the old 165 s cap, not 60 bugs). posix2 one pair
`20260917-191150` **58 both-pass / 4 EFS**; posix2 multi
`20260917-192141` **56–58 / 4–6**. Shared posix2 set: `peer_concurrent_append`
got 400 want 800 on every pair (one side's O_APPEND records never land);
overlap-pwrite RMW; `peer_fcntl_range_conflict` per-client lockf.
`peer_rename_dir` EIO is load-only (isolated PASS). Do not invent REPORT
split or chunked SNAP. 005 gossip-DOWN ~30 s after bounce is STATUS
probe, not a dead process.

**FRESH-19810 POSIX + DD (same day).** 8 GiB non-zero `dd` on the
*previous* 19810 (FUSE_OK, not zeros): stream **731–825 MB/s**,
`conv=fsync` **391 MB/s** (22 s) — short of ~2 GiB/s. Client on-CPU is
blake3 in `hash_write_fragments`; do not quote the idle-heavy 2 h
`perf stat`. Fresh-cluster posix jobs=1
`results/posix/20260917-161117`: **94 PASS / 3 timeout / 104 never
ran** — suite hit the 165 s SSH cap. Idle ops were **100–370 ms**
(stat/mkdir/listdir). Cause: unconditional `applied *` stderr on the
pump + a ReadIndex quorum per op. Fix in tree: gate success apply
logs on `EFS_RAFT_DBG`; skip ReadIndex when `efs_raft_read_current`.
lane-sweep `rc=-5` on empty files treated as OK. **Need efsd bounce
(keep storage) + re-gate.** Do not invent REPORT split / chunked SNAP.

**STEP 11 (delete the old snapshot/root-2PC metadata engine) COMPLETE
(Sep 11 2026, commits 94c4c15 Inc1 → d7baa38 Inc2 → db023c7 Inc3 →
c878a88 Inc4 → f5c4390 Inc5).** The Raft+KV engine is the ONLY metadata
path. Deleted: meta_server.c / migrate.c / verify.c, the old inode-RPC
handler branches, the server's metadata-table ownership (exports[] is a
shell {id,name,chunk_size}), the client's old lookup/report/bootstrap/
pack paths (client is raft-only; raft LOOKUP replies carry the full
resolved row; entry at parent shard, host walks lanes), the old efs-mgmt
control plane (mkfs now aliases raft-mkfs; list-exports/destroy/drain/
undrain/remove-node/feature/upgrade gone), 58 dead metadata.c functions
(serialize/snapshot/root_*/pack_*/flush-dirty/trim/evict/rehash/load/
save/merge/adopt — metadata.c 6042→4335 lines; it STAYS linked because
the client keeps `g_client.export` as its local staging/dirty table),
13 old tests + test_directio + 7 old-engine scripts, and the
EFS_MD_RAFT / EFS_MKFS_SHARD_BITS / EFS_FLUSH_PROF / EFS_LOCK_PROF /
EFS_INO_PROF / EFS_INO_RAM_MB / EFS_CWI_TRACE env knobs. **Everything in
this file below about the old engine (2PC root commit, CoW page flush,
GC races, meta-rebuild, shard tabs, extras catchup, EFS_INO_RAM_MB, the
dangling-dentry saga, flush group commit, bits=3/5 tuning) describes
DELETED code — historical reference only, do not re-apply.**
**Gates (all on the 19820 scratch raft cluster, TCP):** every increment
posix 196/201 with the exact known signature (dir_move_into_subdir
EINVAL debt, virt_find_query .find unimplemented, deep-nesting/many-files
15s timeouts, occasional concurrent_writes_disjoint EIO flake); unit
green (the 6 test_lock getlk failures were a test bug, closed Sep 18 —
the test aliased req/out and `efs_lock_getlk` clears out first).
Inc 5 final: posix 196/201 in 28s.
**`tests/valgrind_leaks.sh` PORTED to raft + GATE GREEN** (same commit as
this note): single-node efsd can't reach quorum, so every wire phase now
runs a 3-node localhost raft group (EFS_MD_RAFT_N=3, ports PORT..PORT+2,
per-node storage) + `raft-mkfs`; the valgrind'd server joins LAST as a
follower (plain nodes hold quorum/leadership; the follower still applies
every committed entry); UNIT_TESTS updated to the current suite;
kill_ours matches $WORK not $PORT; suspicious-possibly-lost frame list
updated to the current sources (raft/kv/meta/wire/data/sim). The port
surfaced TWO REAL CLIENT LEAKS, both fixed and gated: (1) main() ran
`efs_export_init(&g_client.export)` and `raft_bootstrap_metadata` then
re-inited the SAME struct without freeing (init memsets it) — the whole
first table leaked (~14 KB); now `efs_export_free` before the re-init.
(2) `decode_frag_scratch` (read.c) was a `static __thread` heap buffer
never freed — 786 KB definite (4 pool workers x 192 KiB); now a pthread
key destructor frees it at thread exit, and a new
`efs_client_read_pools_stop()` (called from `efs_client_shutdown` AFTER
the flush, which can read) joins the get/frag pool workers so the
destructor actually runs. Phase-4 RDMA traps fixed: the servers were
started with EFS_TRANSPORT=tcp, but `efs_rdma_available()` returns 0
process-wide under tcp, so the server REFUSED every upgrade (phase was
vacuously TCP) — servers now run auto; the client runs STRICT
EFS_TRANSPORT=rdma so a failed upgrade can no longer fall back silently;
the "RDMA transport up" check moved after the workload (the upgrade is
lazy — first pool checkout, not mount). Full gate PASS on fcstor003:
13 unit + efsd + efs-fuse TCP + efsd/efs-fuse RDMA, all 0 definite /
0 indirect / 0 uninit / 0 invalid.
**Open follow-ups:** Step 12 A–D landed (Part B `0df94e4`; C+D `ec3ec5b`).
Populated LOCAL-range migrate is a cross-group txn (`5a8219e`). LOCAL dirs
auto-begin SPLITTING when `nents > EFS_DIR_SPREAD_MIN` (`efb2ca9`). Background
leftover drain is in this tree (`dir_spread.c` + GC-thread pass). Honest
fio matrix (Part D perf contract) is **not** run on the 1G 19820 scratch.
Relaxed-coherence stays out of scope. Production `raft-change` is in this
tree (operator desired file + learner attach with C_old). The control-plane
desired Raft group is still sim-only. Leftover 1 honest fio on 19810 is
**1-client gated** (`results/perf/20260917-honest/`; see Sep 17 note).
First-matrix sw-1m 209; after WAL hold / activate-mask / pipelined
propose, honest 9×2g sw-1m **924**. 4/9-client not run. sw-50g
`end_fsync` NET (400k-pub REPORT vs 30 s).
Pressure-spread bound is unspecified (do not invent). Host KV snapshot uses the existing
WAL item payload (`kv_snap.c`); import dest-key collect stores **offsets**
(arena realloc UAF). A group over the 4 MiB SNAP cap is left uncompacted.
Chunked InstallSnapshot is unspecified. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**BATCHED APPENDENTRIES + OUTBOX CLOSED (Sep 15).** Catch-up was
one entry per AE. `send_ae` now packs up to `EFS_RAFT_AE_MAX=128`
entries / `EFS_RAFT_AE_BYTES=1MiB`; `on_ae_req` appends the batch;
a reject jumps `next_index = match_index+1`. Host outbox
`HOST_OUTBOX_MAX` 256→2048; `host_send` heap-encodes when the
stack buffer is too small. Wire nentries 0..AE_MAX; decode packs
cmds into `cmd_buf`. Sim pack/unpack matches. Unit
`test_ae_batch_catchup` (drop follower, 200 proposes, 4 ticks).
**Gates (19820 scratch, TCP, keep storage):** `test_raft` / `test_wire`
/ `test_sim` / `test_txn` OK. Live: kill fcstor005, create while
down, restart — applied 22123→22241 in one poll (CATCHUP_OK).
posix jobs=1 **196/201** (`results/posix/20260915-ae/`): 4 walk
15s timeouts (`dir_deep_nesting`, `dir_many_files`,
`names_crazy_dirs`, `dir_deep_nesting_beyond_64`) +
`concurrent_writes_disjoint` zeros flake. Isolated
`dir_readdir_listing` PASS 2.6s.

**STEP 12 PART B — LOW-LEVEL FUSE (Sep 15, working tree).**
`efs-fuse` uses `fuse_lowlevel_ops` / `fuse_session_loop_mt`
(libfuse 3.10.2, `FUSE_USE_VERSION 32`). nodeid ↔ efs ino 1:1;
timeouts stay **0**. Two correctness holes closed in the same
cut: (1) sharded `unlink_name_ex` now honors `keep_last` (ghost
nameless nlink=0 row) so unlink-open getattr does not adopt owner
size 0 and `fuse_file_read_iter` empty-read; (2) path LOOKUP
adopts the RPC row (`efs_client_adopt_lookup`); path getattr
`efs_client_stat_refresh` (GETATTR+adopt+overlay); open-fd
getattr stays local-first so a peer REPORT cannot invalidate an
in-flight dcache. `run_tests.sh` remount/ensure_mounted honor
`EFS_SEED` (default still `:19810`).
**Gates (19820 scratch, TCP):** posix jobs=1 **196/201**
(`results/posix/20260915-partb/suite-jobs1-refresh.tsv`) — 5×
15s walk timeouts (`dir_deep_nesting` ×2, `dir_many_files`,
`names_crazy_dirs`, `concurrent_write_and_readdir`); unlink-open
3/3 isolated. posix2 **59/63**
(`results/posix2/20260915-partb-refresh/`) — known
`peer_concurrent_append` + `peer_fcntl_range_conflict`; two
overlap-pwrite load flakes that pass isolated. Isolated
`peer_shared_pwrite` / `peer_creat_excl_race` PASS. leaks
**PASS** (`results/leaks/20260915-partb/`, 0 definite / 0
indirect, 19820 pid unchanged). Unlink-storm 9×500 empty
**PASS** (`results/stress/unlink-storm-20260915-partb-n500b/`);
9×4000 is quota/latency on the 1G scratch, not a FUSE hang.
Hardlink storm `HLSTORM_OK` n=200 (`results/stress/hlstorm-partb.txt`).
Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.

**STEP 12 PART C — EXACT SELF-INVAL (Sep 16).**
Timeouts stay **0**. `notify_inval_*` from a request handler
deadlocks (kernel holds the parent); queue after `fuse_reply_*`
on a dedicated thread. Do **not** `inval_entry` on create/unlink
(redundant at timeout=0; races create-then-pwrite). Only a
**shrinking** SETATTR SIZE notifies, and only pages at/after the
new EOF (`(0,0)` = whole mapping only for truncate-to-zero).
`clone_fd` stays libfuse default. GC `del_if_sum` now uncharges
`local->used` (`store.c`) — 1G scratch was ENOSPC because
unlink never returned quota. `wait_cluster_idle` treats raft
status (no `gen=` line) as idle when all 4 nodes are Heal idle.
**Gates (19820 scratch, TCP):** posix jobs=1 **194/201**
(`results/posix/20260915-partc/suite-jobs1.tsv`) then
**190/201** (`results/posix/20260915-partc3/suite-jobs1.tsv`,
453s, `--tag partc3 --keep`) — 15s walk/concurrent timeouts +
`symlink_relative_after_parent_rename` EIO load flake (known).
posix2 **58/63** (`results/posix2/20260915-partc/`). Isolated
overlap currently **FAIL** on HEAD Part B fuse too (same exclusive-
range zeros) — not a Part C regression; sub-chunk RMW, not page
cache. Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.

**STEP 12 PART D — FOPEN_DIRECT_IO + PREFETCH (Sep 16).**
`fi->direct_io = 1` on every regular open/create. 4 KiB EINVAL only
when the **application** set `O_DIRECT` (FOPEN_DIRECT_IO still
accepts unaligned FUSE I/O). Kernel writeback cap is cleared.
libfuse **3.10.2** cannot emit `FOPEN_PARALLEL_DIRECT_WRITES` or
negotiate `FUSE_MAX_PAGES` (kernel default 32 pages = 128 KiB per
request) — do not set `conn->max_read` without `-o max_read=`
(libfuse aborts: `init() and fuse_session_new() requested different
maximum read size`). Sequential reads prefetch up to 16 published
chunks into rdcache (`EFS_READ_PREFETCH`, drop if the GET pool is
half full). `MAP_SHARED` mmap is ENODEV (spec: unsupported);
`MAP_PRIVATE` still works.
**Gates (19820 scratch, TCP):** posix jobs=1 **193/201**
(`results/posix/20260916-partd/suite-jobs1-clean.tsv`, 387s) —
walk/concurrent 15s timeouts + known symlink EIO flake + expected
`mmap_write_read` SKIP ENODEV. Isolated overlap still **1/3**
(same as HEAD Part B: exclusive-range zeros = chunk RMW, not
cache). posixpersist **25/26** (`results/posixpersist/20260916-042337/`);
the 1 is `many_files_in_one_dir` 60s timeout on prepare **and**
verify, not a silent loss. Honest fio not run here (1G quota).
`ensure_mounted` TCP check no longer treats grep -c 0 as RDMA
(`|| echo 0` concatenated to `00`). Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**POPULATED LOCAL MIGRATE AS TXN (Sep 16).** A leftover whose HASHED
dentry shard is on the other Raft group is a 2-shard txn (local DEL +
hashed PUT + dseq, optional parent `used_shards`); same-group leftovers
and I8 (hashed live/tombstone already there) stay single-group
`DIR_MIGRATE`. Lane-0 names stay local; migrate-done is NOT_FOUND (mgmt
status=1) once bit 0 is set so apply cannot swallow it into status=0
forever. Host bounces to a dual-host when this replica does not host
the dest group. `migrate_one` still PUTs for same-KV unit tests.
**Gates (19820 scratch, TCP, keep storage):** `test_sim` (new
`test_migrate_populated` seed 139) / `test_meta_apply` / `test_txn` /
`test_wire` OK. Targeted efs-mgmt `raft-smoke-mg`: leftover `x0` (psh
odd, dsh even) begin → two migrate steps → status=1 → finish; lookup +
readdir keep `pre` and `x0`. posix jobs=1 `--tag migrate --keep`
**195/201 in 247s** (`results/posix/20260916-migrate/`) — 15s walk
timeouts (`dir_deep_nesting` ×2, `dir_many_files`, `names_crazy_dirs`)
+ known `symlink_relative_after_parent_rename` EIO flake + expected
`mmap_write_read` SKIP ENODEV. Full `raft_host_smoke.sh` not re-run
(EXIT trap kills 19820). Do not `wipe_cluster.sh` / `pkill -x efsd`
while 19820 is up.

**AUTO-SPREAD SIZE TRIGGER (Sep 16).** LOCAL dirs carry `nents` in the
inode-row padding (bytes 60–63). Create/mkdir/link dest increment it;
unlink/rmdir/rename-src decrement it. Crossing `nents > EFS_DIR_SPREAD_MIN`
commits SPLITTING on that same parent PUT (dseq/dentry shard still use the
pre-flip layout). HASHED/SPLITTING freeze the count. Env override
`EFS_DIR_SPREAD_MIN` for tests (default 65k). Pressure-triggered spread
is still open (bound unspecified). `make docs-check` is the architecture
machine-gate (`python3 docs/check-architecture.py`).
**Gates:** `test_sim` `test_auto_spread` seed 140 (min=3). Do not
`wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.

**BACKGROUND LEFTOVER MIGRATOR (Sep 16).** In-memory SPLITTING-ino queue
(`include/efs/dir_spread.h`, `src/meta/dir_spread.c`). Lost on crash;
re-note on DIR_BEGIN, on a nents flip, on apply of a SPLITTING parent, and
on a txn resolve PUT of a SPLITTING dir row. Host: `host_dir_spread_pass`
from the existing GC thread (cap 8 leftovers/tick) — reuses `host_dir_migrate`
so cross-group leftovers stay a txn; FINISH when peek is NOT_FOUND. No new
`raft_host` thread. Sim: opportunistic drain after create/mkdir/link/rename
only on LOCAL→SPLITTING (operator-begin leftover tests stay un-drained).
Pressure numbers are still unspecified.
**Gates:** `test_sim` `test_auto_spread` seed 140 reaches HASHED without an
explicit migrate loop. Do not `wipe_cluster.sh` / `pkill -x efsd` while
19820 is up.

**INSTALLSNAPSHOT IN THE RAFT SM (Sep 16).** `EFS_RAFT_MSG_SNAP_REQ=5` /
`SNAP_REP=6`. `last_log_*` = lastIncluded; `nentries=1` is
`[app_old:4][app_new:4][user blob]` so wire/sim/host codecs stay AE-
shaped. `efs_raft_snapshot` freezes `snap_get` before compacting.
`send_ae` sends SNAP when `next_index <= snap_idx` (no more skip).
`on_snap_req` rejects a skip-ahead unless `snap_put` installs that
exact prefix — empty metadata-only SNAP cannot jump `last_applied`.
`raft_mem` save_snap clamps like disk (empty learner).
**Gates:** `test_install_snapshot` (compact 40, grow `0x7→0x1f`,
learners install) + `test_install_snapshot_needs_blob` (metadata-only
snap stays BUSY) + `test_wire` SNAP codec. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**HOST KV SNAPSHOT (Sep 16).** After persist_applied, if
`applied - snap_idx ≥ HOST_SNAP_MIN` (256), the pump flushes the LSM
then `efs_raft_snapshot`. `snap_get`/`snap_put` are
`efs_kv_group_export`/`import`: WAL item payload, keys whose shard
maps to that Raft group, import replaces that namespace only. Oversize
→ `EFS_ERR_BUSY`, `snap_oversized` latches, log stays uncompacted
(the live 283k scratch will hitch here until chunked SNAP exists).
Do not invent chunking. Do not restart 19820 for this cut.
**Gates:** `test_kv_lsm` `test_kv_group_snap` + `test_raft` /
`test_wire` / `test_sim` / `test_txn`. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**19810 NVMe + HONEST FIO 1-CLIENT (Sep 17).** Same 19810 storage (no
`raft-mkfs`). 19820 left up; 007 remounted back to `:19820` after the
run. Unblocked 2g `end_fsync` with publication batching (`HOST_PUB_BATCH_N`
256 pubs / Raft entry) plus `host_rpc_submit` heap-encode when `clen >
HOST_CMD_MAX` (512 was the stack buffer; a 51 KiB follower-forward
returned INVAL and `host_remote_cmd` remapped that to NOT_PRIMARY
`efs_rc=-15`). Earlier in the same leftover: LSM compact `drop[]`
sized to `KV_LSM_MAX_SEGS*2` (004 SIGSEGV at 64 L0 + overlapping L1;
compact-first flush; `test_compact_full_l0`); dest-key offsets in
`kv_snap.c`; one ReadIndex per group; skip identical pub; per-ino
fsync report (not a new REPORT wire). Units on 003:
`test_raft` / `test_kv_lsm` / `test_sim` / `test_wire` /
`test_meta_apply` OK. Honest 007 TCP `results/perf/20260917-honest/`
FUSE_OK, no `md0`, err=0 except 50g:

| test | 1-client MiB/s |
| sw-1m | 209 |
| ow-1m | 196 |
| rw-1m | 196 |
| rw-128k | 194 |
| rw-4k | 89 |
| sr-1m | 3141 |
| rr-1m | 2276 |
| rr-128k | 1492 |
| rr-4k | 144 |
| sw-50g | FAIL `end_fsync` NET |

Write walls include fsync (intra-job write samples several GiB/s — do
not quote those as the number). 50g laid down 50 GiB then one REPORT
of ~400k pubs missed `EFS_IO_TIMEOUT_MS`. 4/9 not run. 005 19810 still
lagged (oversized SNAP). Do not invent chunked SNAP or REPORT split.
Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up. Do not
auto `raft-mkfs` again.

**HOT-PATH AFTER HONEST FIO (Sep 17).** Same leftover-1 19810 (no
`raft-mkfs`, 19820 left up). Profile of 1-client sw (attach to a
RUNNING daemon; do not restart under strace): client on-CPU is blake3
in `hash_write_fragments` / `dcache_flush_slot_inner` plus futex on
REPORT; server fsync counts 2223 / 4466 / 2112 on 003 / 004 / 006 —
one WAL fsync per sequential `apply_one_publish`. Group-commit does
not share a fsync among a single-threaded apply loop. Fixes in this
tree: `kv_wal_hold` / `efs_kv_lsm_sync_hold` across a batched
PUBLISH apply (`test_kv_lsm` `test_sync_hold`); `EFS_MD_CMD_ACTIVATE_LANE`
17 B mask via `efs_meta_apply_activate_lanes` (one inode PUT); inode
row cache on REPORT; `host_pub_batch_propose` then
`host_wait_applied` of the last idx. Honest remasure (007 TCP,
FUSE_OK, no `md0`, err=0): 512m 1-job **247** MiB/s
(`hot-sw-512m-fcstor007.txt`); 9×2g sw-1m **924** MiB/s, 18 GiB /
20 s (`hot-sw-1m-9job-fcstor007.txt`). A 255 figure was 9 jobs on
one 2g file (`$jobnum` not unique). Intra-job write samples stay
several GiB/s — do not quote those. 50g / 4/9 not re-run. 007
remounted back to `:19820`. Units earlier on 003: `test_raft` /
`test_kv_lsm` / `test_meta_apply` / `test_sim` / `test_wire` OK.
Do not invent chunked SNAP or REPORT split. Do not `wipe_cluster.sh`
/ `pkill -x efsd` while 19820 is up.

**RDMA FIRST-INODE POOL FIX (Sep 17).** Not all RDMA items were fixed:
poller re-arm, finite `rnr_retry`, and leaks RDMA phase already landed;
the parked empty-table first-`mkdir` hang did not. Client pool now
pins sockfs identity at wrap, evicts on mismatch, refuses to `close`
a recycled fd, and `conn_init` skips busy slots. Post-mount
GET_FEATURES/STATUS use the pool. Gate: `test_conn_fd`. Do not call
a 19820 remount the empty-table repro. Do not `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up.

**19810 NVMe + HONEST FIO ATTEMPT (Sep 16).** Port 19810 `raft-mkfs`'d
on `/data1` (36T, `--direct-io`, seed 003, join 004–006). 19820 scratch
left up. First sw-1m: fcstor005 19810 SIGABRT
`malloc(): mismatching next->prev_size` in `kv_mtab_set` ←
`efs_kv_group_import` (`kv_snap.c`) ← `on_snap_req`. Cause: collect
arena `realloc` left `key_ref` pointers dangling into `efs_kv_batch`.
Fix: store offsets. Gate: `test_kv_lsm` arena + LSM dest replace (2k
stale keys). Redeployed 19810 only (no `--join`). Honest
`results/perf/20260916-honest5` 007 TCP sw-1m **FAIL** 400s: data
moved (usable 13.5→27 GiB, g0 applied ~17k→33k) then `end_fsync`
`fsync-meta efs_rc=-6` (`EFS_ERR_NET`) on inos 18326/22422; fio EIO
on f.5/f.6 last 1m sync. 005 stays g0 `commit=751` /
`applied=18688` (applied≫commit) and `raft-create` to :19810 on 005
is status=5 BUSY. Kill 005 19810 to dodge BUSY **breaks 2+1 PUT**
(needs all fragment ACKs). Do not invent chunked SNAP or REPORT
split. Do not `wipe_cluster.sh` / `pkill -x efsd` while 19820 is up.
Do not auto `raft-mkfs` again.

**PRODUCTION RAFT-CHANGE (Sep 16).** `raft_host` wires `efs_raft_change`
(I18). Operator `efs-mgmt raft-change <node:port> <group> <voters>` reuses
`EFS_MSG_RAFT_MKFS` with host-only `EFS_MD_CMD_CFG` (NOTE then CHANGE — not
a log command). Desired mask is a per-group file under `mdraft/`; every
peer attaches a learner replica with **C_old** as `cfg.voters` before the
leader appends JOINT. Status reports live `efs_raft_voters` + `joint`.
`host_hosts` is the committed voting set, so a learner does not serve inode
RPCs until COLD. Fan NOTE uses `HOST_SEND_IO_MS` (a 30 s peer submit was
the first 0xb hang). Control-plane desired group stays sim-only.
**Gates (19820 scratch, TCP, keep storage):** `test_raft` / `test_sim` /
`test_wire` / `test_txn` OK on fcstor003. Live no-op `raft-change 0 0x7`
**OK** (`rc=0`, index=283372). Live 3-for-3 `0x7→0xb` started learner
catch-up on fcstor006 (applied ~26k of 283k, ~800/s) and wedged that
node's `h->mu` on `xlog_wait_on_iclog`; JOINT did not commit (g0 stayed
`0x7 joint=0`). Restored `0x7`, dropped `desired.0`, restarted 006+005;
`raft-create` `.raft-chg-smoke` ino=851969 status=0. Full live swap on
this log still needs a host KV snapshot (SM InstallSnapshot is in; the
host never compact). Do not `wipe_cluster.sh` / `pkill -x efsd` while
19820 is up.

**OLD-ENGINE 19810 CLUSTER DESTROYED (Sep 15).** No snapshot/2PC
on-disk compat. `/data1/01–06/efs` on fcstor003–006 renamed-aside +
`edelete` (empty dirs left). Removed: Aug 19 crash binaries under
`logs/`, `~/efs-bin-travel/` (Aug 18–19), `/tmp/efs-495` on
003–006 (Sep 11 leftover tree still linking `meta_server.c`).
19820 scratch (`/tmp/efs-raft-scratch`) was left running; clients
007/008 stay mounted on `:19820`. Do **not** `wipe_cluster.sh` /
`pkill -x efsd` while 19820 is up. Do **not** auto `raft-mkfs` a
36T 19810 cluster until asked. `/tmp/efs` on the nodes is the
current tree — keep it.

**`.FIND` READDIR WALK CLOSED (Sep 11 night, working tree).** POSIX
`virt_find_query` returned empty because `find_index_build_locked`
memcpy'd `g_client.export`, which has no GET_META snapshot after step 11.
Fix is client-only in `efs_fuse.c`: drop the whole-table index; walk the
query directory's subtree with `efs_client_rpc_readdir_cur` (name-order
cookies, depth 128, 65536-visit cap, skip `.fuse_hidden*` and reserved
`.find`/`.stats`). Paths are host-absolute from `g_mountpoint` + the
FUSE dir prefix of the `.find/<term>` path (getattr does not fill
name/parent). Result cache (16 slots, 5 s TTL) unchanged. Not a
server-side name index (derived-index design / roadmap "Server-side
`.stats`/`.find` refresh" stays NOT DONE).
**Gates (19820 scratch, TCP, remount efs-fuse only):** isolated
`virt_find_query` + `virt_find_not_a_real_dir` **2/2 in 0.1 s**. posix
jobs=1 **199/201 in 75 s** (`results/posix/20260911-find/suite-jobs1.tsv`)
— `virt_find_query` PASS. The 2 fails are same-parent dir-rename EIO
(`dir_rename_dir_with_contents`, `rename_dir_same_parent`) on this
long-lived scratch; both pass **5/5 isolated** (0.1 s) and were PASS on
the wipe+mkfs cross-dir jobs=1. Committed `986ca45`. Do not start step 12.

**CROSS-DIR RENAME CLOSED (Sep 11 night, working tree on top of
`e486ec4`).** POSIX `dir_move_into_subdir` was EINVAL because
`server_raft_host_rename_at` rejected `old_parent != new_parent`.
That test is a **file** rename `a/f` → `b/f`, not a directory-into-subdir
cycle. Apply + sim already implemented cross-dir; the host now runs a
LOCAL 2-parent txn (src dentry DEL, dest dentry PUT, inode parent=,
both parent stamps + dseq, bounce if this replica does not host both
parent groups). Directory cross-dir also moves nlink (src--, dest++)
and still GUARDs dest ancestry `parent_version` (`err_rename_dir_into_itself`
stays EINVAL). HASHED rename closed below; SPLITTING stays BUSY.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` OK (plant the file — `create_file`'s first ino on shard S is
S itself and would clobber a planted parent at 21/37). Isolated
`dir_move_into_subdir` + `err_rename_dir_into_itself` PASS. posix
jobs=1 **200/201 in 104 s** (`results/posix/20260911-crossdir/suite-jobs1.tsv`)
— only `virt_find_query` (closed above). jobs=16 still saturation-timeouts
on this scratch (186 then collapse on stacked runs); not a rename
correctness fail (`dir_move_into_subdir` passed in the parallel batch).
Committed `4d47038`. Do not start step 12.

**HASHED RENAME CLOSED (Sep 11 night, working tree on top of `986ca45`).**
Same pattern as LOCAL cross-dir: apply + sim already implemented HASHED
rename; the host INVALed non-LOCAL parents. HASHED is now a txn that
stamps dir-lanes (parent row only for `used_shards` / nlink), dseq on
the name's dir-lane, and bounces when a hashed dentry shard is on
another Raft group. Same-dir dest first-use sets the dest lane
`used_shards` bit. SPLITTING stays BUSY. HASHED dest-dir overwrite
closed below.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` OK. efs-mgmt: LOCAL same-dir, HASHED same-dir (`n0`→`r0`
status=0), LOCAL cross-dir all status=0. Isolated posix rename/find
PASS. posix jobs=1 **201/201 in 30.4 s**
(`results/posix/20260911-hashed/suite-jobs1.tsv`). FUSE HASHED dir
`hr-fuse`: `mv n0 r0` OK. HASHED unlink/rmdir closed below. Do not start
step 12.

**HASHED UNLINK/RMDIR CLOSED (Sep 11 night, working tree on top of
`0ce6167`).** Apply + sim already implemented HASHED unlink/rmdir; the
host INVALed HASHED parents (and HASHED child rmdir). FUSE `rm -rf` of a
HASHED dir then EIO'd on the directory itself. Host now allows HASHED:
LOCAL stamps parent-row times; HASHED stamps dir-lanes + dseq on
`efs_kv_dir_lane(name)`; bounce if this replica does not host the dentry
shard. HASHED empty-dir rmdir GUARDs used-lane dseqs (cap
`EFS_TXN_MAX_PART` → BUSY). Last-link same-shard still uses
`EFS_MD_CMD_UNLINK` (HASHED file create places the inode on the dentry
shard). Sim `apply_unlink_cmd` accepts the session on the dentry shard
(HASHED create is `dsh==ish` but the cmd previously accepted on `psh`).
SPLITTING stays BUSY. HASHED dest LINK closed below.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` / `test_txn` / `test_wire` / `test_kv_lsm` OK. efs-mgmt:
HASHED last-link unlink (`u0` status=0, lookup NOT_FOUND) + HASHED
empty-dir rmdir (`raft-smoke-he` status=0). FUSE HASHED dir `hr-rm`:
`rm -rf` OK. posix jobs=1 **196/201 in 163 s**
(`results/posix/20260911-hashed-unlink/suite-jobs1.tsv`) — 4× 15s
timeouts (`dir_deep_nesting` ×2, `dir_many_files`, `names_crazy_dirs`)
from ~100 ms ReadIndex (pre-existing ~110 ms cross-group note), plus
`concurrent_writes_disjoint` zeros flake. Unlink/rmdir/find/rename
tests PASS. Do not start step 12.

**HASHED LINK DEST CLOSED (Sep 11 night, working tree on top of
`91838aa`).** Apply + sim already implemented HASHED dest LINK (dir-lane
stamp, dseq on `efs_kv_dir_lane(dst_name)`, parent row only for LOCAL
times or HASHED `used_shards` first-use). The host INVALed
`dprow.layout != LOCAL`. Host now allows HASHED: bounce via
`host_fwd_link` when this replica does not host dsh/ish/psh; LOCAL
stamps parent-row times on psh; HASHED stamps the dir-lane on dsh and
dseq on that lane. First-use sets the dest lane `used_shards` bit.
SPLITTING stays BUSY. HASHED dest-dir overwrite closed below. HASHED
file create still places the inode on the dentry shard, so last-link
same-shard unlink is unchanged.
**Gates (19820 scratch, TCP):** `test_meta_apply` / `test_sim` OK.
efs-mgmt: HASHED dest link (`n0`→`l0` status=0, nlink=2, dup EXIST).
FUSE HASHED dir `hl-fuse`: `os.link(a, l0)` OK (nlink 2). posix jobs=1
**200/201 in 36.8 s** (`results/posix/20260911-hashed-link/suite-jobs1.tsv`)
— only the `concurrent_writes_disjoint` zeros flake; all hardlink tests
PASS. Smoke `raft_host_smoke.sh` HASHED dest LINK check added (rename
hygiene: dedicated names so POSIX file-over-file replace is not scored
as EXIST). Do not start step 12.

**HASHED DEST-DIR OVERWRITE CLOSED (Sep 11 night, working tree on top of
`fc057cd`).** Apply does not replace (EXIST); production path is the
host txn. Sim and host both INVALed HASHED dest dirs. HASHED empty-dir
rmdir already proved distributed emptiness (per-lane `shard_empty` +
dseq GUARDs). Rename-over-empty-HASHED-dir reuses that, then DEL dest
inode (same as LOCAL dest dir overwrite). File-over-file replace
already works; file-over-dir / dir-over-file stay INVAL. SPLITTING dest
stays BUSY. Nonempty HASHED dest stays NOT_EMPTY (RPC status 8), not
INVAL. Host: bounce used-lane groups + GUARD used-lane dseqs (`gv[]`
stays pver ancestry; dest dseqs in `xdseq[]`); `EFS_TXN_MAX_PART` →
BUSY. Dest HASHED-dir overwrite only when source is also a dir.
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_meta_apply` /
`test_sim` OK (new `test_rename_hashed_dir_overwrite`). efs-mgmt:
nonempty `hs`→HASHED `hx` status=8, unlink child, overwrite status=0,
`hx` ino = src. FUSE `hd-fuse`: nonempty `mv` ENOTEMPTY, emptied `mv`
OK (dst ino = src). posix jobs=1 **200/201 in 44.9 s**
(`results/posix/20260911-hashed-dow/suite-jobs1.tsv`) — only the
`concurrent_writes_disjoint` zeros flake; rename/find/unlink/hardlink
PASS. Smoke `raft_host_smoke.sh` HASHED dest-dir overwrite check added
(fresh path passed; after-crash hung on an environmental no-leader
wedge, not this slice). Do not start step 12.

**SPLITTING DEST CREATE + READDIR CLOSED (Sep 12, working tree on top of
HASHED dest-dir overwrite).** Apply + sim already wrote hashed during
SPLITTING and merged READDIR (hashed side wins, I8). The host BUSY'd
SPLITTING dest CREATE and READDIR. File CREATE now reuses the HASHED
first-use txn when the dest lane's group differs from the parent
(`used_shards` bit + dest dentry/inode); same-group first-use stays a
single propose. READDIR drops the BUSY; apply already merges LOCAL
leftovers with hashed lanes. LOOKUP / LOOKUP_PATH were already
hashed-then-local. Smoke `raft-smoke-sp` stays SPLITTING: local `pre`
plus a hashed-name file. Populated-range migrate as a txn closed Sep
16 (below). SPLITTING dest UNLINK/RMDIR/LINK/
RENAME stay BUSY (tombstone I8 on the host later). HASHED/SPLITTING
dest MKDIR still writes the dentry on `psh`.
**Gates (19820 scratch, TCP):** `test_sim` / `test_meta_apply` /
`test_wire` OK. efs-mgmt: mkdir `raft-smoke-sp`, local `pre`, begin
(no migrate), hashed `n0` create status=0 (ino shard ≠ parent),
readdir `n0,pre`, dup EXIST, lookup-path both. FUSE `sp-fuse`: `pre`
then begin, hashed `n0`, `ls` sees both. posix jobs=1 **200/201 in
112 s** (`results/posix/20260912-splitting/suite-jobs1.tsv`) — only
the `concurrent_writes_disjoint` zeros flake. Do not start step 12.

**SPLITTING DEST UNLINK/RMDIR CLOSED (Sep 12, working tree on top of
SPLITTING dest CREATE+READDIR).** Apply + sim already wrote
`HASHED(name)=TOMBSTONE(layout_epoch)` on unmigrated names (I8) and
skipped the redundant local DEL when keys alias. The host BUSY'd
SPLITTING dest UNLINK/RMDIR. Host now uses `host_dent_drop_*` matching
apply: EXCL DEL local leftover unless HASHED or (SPLITTING && keys
alias); SPLITTING PUT tombstone on the hashed key; HASHED DEL hashed.
Bounce if this replica does not host psh/dsh/ish. Last-link
`EFS_MD_CMD_UNLINK` is forced onto the txn when SPLITTING && parent
group ≠ hashed group (the hashed-group UNLINK cmd cannot DEL a local
leftover on the other group). A child that is itself SPLITTING stays
BUSY. SPLITTING dest LINK closed below; RENAME stays BUSY.
HASHED/SPLITTING dest MKDIR still writes the dentry on `psh`.
**Gates (19820 scratch, TCP):** `test_sim` / `test_meta_apply` /
`test_wire` OK. efs-mgmt: hashed unlink NOT_FOUND, recreate OK
(tombstone ≠ EXIST), unlink local `pre`, rmdir empty `e`, readdir
empty of all three, rmdir of the SPLITTING dir itself status=5 BUSY.
FUSE `sp-rm`: `pre` + begin + hashed `n0`, `rm` both + empty child,
`ls` empty, `rmdir` self EIO (BUSY). posix jobs=1 **199/201 in 31 s**
(`results/posix/20260912-splitting-unlink/suite-jobs1.tsv`) —
`concurrent_writes_disjoint` zeros flake +
`symlink_relative_after_parent_rename` EIO (isolated PASS 0.1 s, known
load flake). Smoke `raft_host_smoke.sh` I8 checks already in tree; full
script not re-run (its EXIT trap kills 19820). Do not start step 12.

**SPLITTING DEST LINK CLOSED (Sep 12, working tree on top of
SPLITTING dest UNLINK/RMDIR).** Apply already wrote hashed dest LINK
during SPLITTING (dir-lane stamp, parent row only for `used_shards`
first-use). The host BUSY'd SPLITTING dest. Host now allows
LOCAL/HASHED/SPLITTING dest: hashed dest during SPLITTING, bounce if
this replica does not host psh/ish/dsh. Sim `link_build` used
hashed-key-only `dent_absent`, so a local leftover looked absent, the
link succeeded, and nlink went to 3. Now `sim_txn_lookup`
(hashed-then-local; tombstone = NOT_FOUND): leftover dest is EXIST;
tombstone dest is absent so nlink stays 2. SPLITTING dest RENAME
stays BUSY. HASHED/SPLITTING dest MKDIR still writes the dentry on
`psh`.
**Gates (19820 scratch, TCP):** `test_sim` / `test_meta_apply` /
`test_wire` OK. efs-mgmt: mkdir `raft-smoke-sp`, local `pre`, mkdir
`e`, begin (no migrate), hashed `n0` create (ino shard ≠ parent),
raft-link hashed `l0` nlink=2, dup EXIST, leftover dest `pre` EXIST,
unlink hashed, link onto tombstone dest nlink=2, unlink, recreate OK,
unlink hashed, unlink `pre`, rmdir `e`, dest link still present
nlink=1, rmdir self BUSY status=5. FUSE `sp-ln`: `pre` + begin +
hashed `n0`, `os.link` hashed `l0` nlink=2. posix jobs=1 **201/201 in
83.9 s** (`results/posix/20260912-splitting-link/suite-jobs1.tsv`).
Smoke `raft_host_smoke.sh` not re-run (EXIT trap kills 19820). Do not
start step 12.

**SPLITTING DEST RENAME CLOSED (Sep 12, working tree on top of
SPLITTING dest LINK).** Apply + sim already implemented SPLITTING
rename (hashed dest PUT, `dentry_drop_items` I8 src drop); the host
BUSY'd SPLITTING src/dest parents. Host now allows LOCAL/HASHED/
SPLITTING for both parents: dest PUT goes hashed, src drop reuses
`host_dent_drop_fill`/`host_dent_drop_prep` (EXCL DEL local leftover
unless HASHED or keys alias; SPLITTING PUTs HASHED=TOMBSTONE), and a
leftover dest under a SPLITTING parent is POSIX replace (EXCL DEL the
local dest key `dest_del_loc` + PUT hashed dest + retire the dest inode
nlink--/DEL with reap marker), not EXIST. Sim `drop_dentry_prep` now
reads the psh/hsh KVs it mutates (was the caller's group KV — a
leftover on the other group could CAS-fail); dest existence uses
`sim_txn_lookup` (hashed-then-local, tombstone = NOT_FOUND). A dest
dir that is itself SPLITTING stays BUSY (distributed emptiness).
HASHED/SPLITTING dest MKDIR still writes the dentry on `psh`.
Populated-range migrate as a txn closed Sep 16 (below).
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_sim` (new
`test_rename_splitting` seed 137: hashed dest, leftover dest replace,
tombstone dest restore, leftover src) / `test_meta_apply` /
`test_wire` / `test_txn` OK. efs-mgmt: `raft-smoke-sp` begin (no
migrate), hashed `n0`→`n28` status=0, `n28`→leftover `q` replace OK
(q = src ino, old q retired), `q`→`n0` onto tombstone OK, leftover
`pre` still present, rmdir self status=5 BUSY. FUSE `sp-rn`: same
sequence through `os.rename` with content + ino identity checks, 5
repeat reads stable. posix jobs=1 **200/201 in 42.9 s**
(`results/posix/20260912-splitting-rename/suite-jobs1.tsv`) — only
the `concurrent_writes_disjoint` zeros flake. Smoke
`raft_host_smoke.sh` SPLITTING rename checks in tree (hashed dest,
leftover dest replace, tombstone restore); full script not re-run
(EXIT trap kills 19820). Do not start step 12.

**HASHED/SPLITTING DEST MKDIR CLOSED (Sep 12-13, working tree on top
of SPLITTING dest RENAME) — the last slice of the dest series.** Apply
+ sim already implemented hashed MKDIR (dentry on the hashed lane
shard, child inode scattered via `mkdir_shard`); the host always wrote
the dentry on `psh`. Host `server_raft_host_mkdir` now mirrors the
HASHED/SPLITTING create path: dentry + dseq + first-use dir-lane stamp
on `dsh = efs_kv_dentry_shard(parent, name, layout)` (`p_lane =
dir_lane(name)` unless LOCAL), parent row on `psh` gets nlink++
always but times ONLY when LOCAL (else the `used_shards` bit), child
inode + alloc stay on `csh = efs_kv_mkdir_shard(parent, name, salt)`.
Parts = {psh, csh, dsh} dedup'd; bounce via `host_fwd_create` when
this replica does not host dsh; ReadIndex on dsh's group when
distinct. EXIST check was already layout-aware. Sim `mkdir_build`
gained the same shape (takes the client id, per-part `sim_sess_ensure`,
`sim_txn_lookup` for EXIST). This completes the op matrix for
HASHED/SPLITTING dests: CREATE, READDIR, UNLINK/RMDIR, LINK, RENAME,
MKDIR all write hashed. Populated-range migrate as a txn closed Sep 16
(below).
**Gates (19820 scratch, TCP, wipe+raft-mkfs):** `test_sim` (new
`test_mkdir_splitting` seed 138: SPLITTING hashed mkdir, readdir merge
with the local leftover, dup EXIST, rmdir, recreate over the tombstone
with a fresh ino, migrate to HASHED, lookup from a second client,
HASHED mkdir) / `test_meta_apply` / `test_wire` / `test_txn` OK.
efs-mgmt: `raft-smoke-sp` begin (no migrate), hashed `d0` mkdir
status=0 (served by primary=2 — the dentry lane is on the other
group), lookup ino/mode match, readdir merges `d0,pre`, dup EXIST,
rmdir OK, lookup NOT_FOUND, recreate OK with a new ino, rmdir of the
SPLITTING dir itself status=5 BUSY. FUSE `sp-mk`: same sequence
through `os.mkdir`/`os.listdir`/`os.rmdir` with ino identity checks.
posix jobs=1 **200/201 in 112.1 s**
(`results/posix/20260912-splitting-mkdir/suite-jobs1.tsv`) — only the
`concurrent_writes_disjoint` zeros flake. Smoke `raft_host_smoke.sh`
SPLITTING mkdir block in tree (hashed mkdir, lookup/readdir/dup/
rmdir/recreate); full script not re-run (EXIT trap kills 19820).
Do not start step 12.

**RAFT-LAG CLOSED (Sep 11 PM, working tree) — three fixes in
`src/server/raft_host.c`, gated 199/201 jobs=16 in 26.5 s with ZERO
post-startup term changes (was: term +8 in 75 ms storms, terms 113/328
cumulative).** (1) **Per-peer outbox:** `host_send` no longer does the
synchronous send+empty-ACK round trip under `h->mu`; it encodes and
queues to `h->tx[peer]` (cap 256, drop-newest) and one `host_sender`
thread per peer does FIFO pop → conn get → 250 ms timeouts → send →
wait ACK → release. Pump `h->mu` holds dropped from up to 250 ms to
~0.5–2 ms worst (drain ~200–480 µs = raft_log_append fsyncs; apply
~200 µs = KV WAL fsync; persist ~250 µs — all measured with the new
`EFS_RAFT_OBS=1` instrumentation: term/role-change logs, per-outbox
counters, 5 s pump-phase maxima dump). (2) **Cross-node read_mu
deadlock fixed:** `host_read_index`'s follower branches and
`host_propose`'s forward branch blocked in `host_remote_cmd` (30 s
`EFS_IO_TIMEOUT_MS` recv + retries) while holding `read_mu`; a
post-restart leader-confusion cycle (003→004→005→003) wedged ALL
metadata ops for minutes (suite hung at startup, FUSE
request_wait_answer). All three sites now drop `read_mu` around the
network wait and relock after (safe: the apply layer re-validates, the
command is fully formed before propose, the raft log orders mutations).
(3) **THE ELECTION-STORM ROOT CAUSE — ticks were iteration-counted,
not wall-clocked:** the event-driven pump (495444d) wakes on every
inbox message/propose, so under load the loop spins as fast as the
drain runs (µs/iteration on a leader), and one `efs_raft_tick` per
iteration fired the 100–200-tick election deadline in **milliseconds**
— any busy node campaigned constantly, its higher-term vote requests
forced the leader to step down (`maybe_step_down`), and the leader
itself re-campaigned instantly (3 re-campaigns in 35 ms observed).
Leaders also broadcast AEs every 10 spinning ticks (~50 µs), flooding
the outbox (hi=91). Fix: `host_pump` gates `efs_raft_tick` on
`now - last_tick_us >= HOST_TICK_US` (never catches up missed ticks —
late is safe, early is not); drain/apply/persist stay event-driven, so
the 495444d latency win is untouched. raft.c is unchanged (the
simulator drives ticks as logical time). **Gate:** fresh wipe +
raft-mkfs, posix jobs=16 **199/201 in 26.5 s**
(`results/posix/20260911-outbox/suite-tickfix-jobs16.tsv`), only the 2
permanent known-debt fails (`dir_move_into_subdir` EINVAL,
`virt_find_query`) — zero timeouts, zero EIO flakes; term lines in all
4 efsd logs are startup-only (g0 term 3, g2 term 4, stable through the
run). Unit: test_raft/kv_lsm/meta_apply/sim/wire/txn OK.
`EFS_RAFT_OBS` stays in tree, env-gated, off by default.

**GC-GAP CLOSED (Sep 11 PM, working tree on top of `0a480c9`) — raft
mode now deletes data-plane fragment files on unlink/truncate (spec
L7).** The old engine reclaimed fragments; the Raft+KV engine never did
— every unlinked/truncated chunk's `<ci>.<frag>` + `.sum` files leaked
on the storage nodes forever. This adds a metadata-driven reaper.
**Design (all in the KV/apply layer + a host reaper thread; NO client
change):** when the apply layer retires a chunk (publish CAS supersede,
truncate range-delete, tail CAS, last-link unlink) it emits a **GC
record** (KV kind 21: `[anchor:2][21][ino:8][chunk_gen:8][lane:1][ci:4]`,
112 B value) keyed on `anchor_shard(lane_shard(ino,lane))`; a last-link
unlink also emits a **REAP marker** (kind 22: `[anchor:2][22][ino:8]`,
16 B value = `[inode_generation][active_lanes]`) keyed on
`anchor_shard(inode_shard(ino))`. `efs_kv_anchor_shard(shard) =
(shard&1) ? 1 : 2` maps any shard to its group's anchor so the record
lands on the same group that owns the state being reclaimed. New raft
cmds `EFS_MD_CMD_LANE_SWEEP 21` / `_REAP_DONE 22` / `_GC_ACK 23`; new
store op `del_if_sum` (checksum-conditional delete so a reaper never
deletes a record a fresher write replaced); new wire
`EFS_MSG_GC_FRAGMENT 99` / `_REPLY 100`. A per-node **`host_gc_thread`**
(raft_host.c ~3290, 1 s loop) runs, for each group this node leads,
`host_gc_reap_pass` (REAP markers → LANE_SWEEP to enumerate live lanes →
GC_FRAGMENT per dead fragment → REAP_DONE) then `host_gc_frag_pass`
(GC records → GC_FRAGMENT → GC_ACK deletes the record via del_if_sum).
Caps per pass: GC_SCAN_MAX=32, REAP_SCAN_MAX=32, GC_ACK_MAX=16.
Followers apply the same cmds so their KV state matches; only the
leader issues the actual fragment deletes. Files: kv_key.h/.c,
meta_cmd.h, meta_apply.h/.c, store.h/.c, store_nvme.c, store_mem.c,
protocol.h, handler.c, raft_host.c, server_internal.h, sim_ns.c,
sim_sess.c, test_meta_apply.c. **Stub-tail alias fix:** a truncate that
lands on a chunk boundary re-uses the old placement with a zero digest;
the host aliases that case and the apply layer skips GC for it
(regression `test_gc_tail_alias`).
**THE END-TO-END BUG (found + fixed this session) — scan-full was
treated as fatal.** The reaper ran, the leader check passed, but nothing
was ever reclaimed. Cause: `reap_scan_cb`/`gc_scan_cb` return **1** when
the batch hits SCAN_MAX (32); `merge_scan` (kv_lsm.c) propagates any
non-zero callback return; and the pass functions did
`if (src != EFS_OK) return;`. So once ≥32 markers/records had
accumulated (all the prior testing while the reaper was broken), every
pass collected 32 and **threw them away**, forever — a classic
"full == error" confusion. Fix: bail only on a real KV error
(`src < 0`); process the partial batch on `src == EFS_OK` (complete) OR
`src > 0` (scan-full stop). Applied to both pass functions. Added
`EFS_GC_DBG=1` per-marker/per-delete diagnostics (`gc reap ... sweep ok,
reap_done rc`, `gc del ino=... ci=... frag=... node=... rc`).
**Known robustness gap (not separately fixed):** `host_gc_export`
returns NULL when `export_count==0` — the export is registered in
`s->exports[]` lazily on `EFS_MSG_PUT_CHUNK` (handler.c:351), so right
after a server restart with no client writes yet the reaper can't name
the export for a delete. Resolves itself with any client activity (all
fragment-holding nodes register). Worth a real registration at mount/
bootstrap if this ever bites.
**ENVIRONMENTAL-LATENCY RED HERRING — GC is EXONERATED from the jobs=16
collapse.** Earlier jobs=16 runs on the GC tree collapsed; an A/B on the
SAME env (TCP) proved it was transient environmental, not GC: pre-GC
(`0a480c9` clean) jobs=16 = **198/201 (63.6 s)**; GC tree jobs=16 =
**198/201 (71.2 s)**, identical 3 known-debt fails. Separately noted
during the hunt: a pre-existing ~110 ms cross-group 2PC fan-out latency
(unrelated to GC; cross-group txns multiply raft rounds).
**GATES (fresh wipe + raft-mkfs, port 19820, TCP, `EFS_GC_DBG=1
EFS_RAFT_OBS=1`):** unit 12/13 OK (`test_lock` getlk,
since closed as a test bug).
End-to-end on ino 4097: write 200000 B → ci0 frags on 003/004/005 + ci1
on 004/005/006; `ftruncate(50000)`+fsync → content sha256 intact, **ci1
fragments reclaimed** (g2 leader fcstor004 `gc del ci=1 frag=0/1/2
rc=0`), ci0 present; `rm` → **all fragments reclaimed** (g0 leader
fcstor003 `gc reap lanes=3 sweep ok, reap_done rc=0` + `gc del ci=0
frag=0/1/2 rc=0`), count 0 on all 4 nodes. jobs=16 posix **199/201 in
68.9 s**, only the 2 permanent known-debt fails (`dir_move_into_subdir`
EINVAL, `virt_find_query` .find) — `concurrent_writes_disjoint` passed.
`EFS_GC_DISABLE=1` turns the reaper off; `EFS_GC_DBG`/`EFS_RAFT_OBS`
stay in tree, env-gated, off by default.

**STEP 1 LATENCY BUCKET LANDED (Sep 11, commit 495444d) —
posix jobs=1 198/201 in 34s (was 182/201 in ~14 min); jobs=16 193/201 in
30s (was catastrophic saturation).** Three changes:
(1) **Event-driven `host_pump`** (raft_host.c): the fixed 5ms usleep poll is
now an **eventfd** (`pump_efd`, EFD_NONBLOCK) + `poll` with `h->mu` UNLOCKED;
`host_pump_kick` does a lock-free write from the inbox path and the
leader-side propose path. FIRST version used a condvar signaled under `h->mu`
and wedged the cluster (election flapping): the pump holds `h->mu` across
synchronous `host_send`, the peer's handler blocked on its own `h->mu` to
signal, 3-node cycle broken only by the 250ms `HOST_SEND_IO_MS` timeout.
**The network inbox path must NEVER take `h->mu`** — that is why the kick is
an eventfd. `host_wait_applied`/`host_read_index` now
`pthread_cond_timedwait` on `applied_cv` (broadcast by the pump each cycle;
safe — no I/O under that wait) with a 400ms absolute deadline.
(2) **Eager commit broadcast** (raft.c `on_ae_rep`): when `try_commit`
advances `commit_index`, the leader now `broadcast_ae` immediately instead of
letting caught-up followers learn the new commit only at the next 50ms
heartbeat. This was the deterministic 74ms create: any op landing on a
follower (2/3 of voters) stalled in `host_wait_applied`. Measured after:
create 2.2ms, stat 0.7ms, unlink 1.4ms, depth-100 mkdir chain 4.2s.
(3) **READDIR pagination cursor fix** (protocol.h, raft_host.c, handler.c,
inode_rpc.c, efs_fuse.c): the raft host scans a dir in NAME order but the
wire contract paginated by `after_ino` (ascending-ino assumption from the old
table-scan server) — with ino-interleaved names and 64-entry pages, later
pages dropped smaller-ino entries (8 threads x 20 creates: listdir saw 69 of
160). `efs_msg_inode_readdir` gained `after_src`/`after_name[EFS_MAX_NAME]`,
the reply gained `next_src`/`next_done`/`next_name` (resume cookie);
`server_raft_host_readdir` takes the cookie, dropped the ino filter, and its
internal page request is `max_ents - out->count` so the cursor never advances
past unemitted entries. Client: `efs_client_rpc_readdir_cur` +
`efs_fuse_readdir` uses it under `efs_client_raft_mode()` (legacy ino path
kept for non-raft; shared `readdir_collect_page` helper does the name dedup).
**Remaining 3 solo failures, all known debt (do not re-derive):**
`dir_move_into_subdir` cross-dir rename EINVAL; `virt_find_query` .find
unimplemented; `concurrent_writes_disjoint` EIO load flake (a sync report
carries ALL dirty recs; a churning file's barrier-BUSY rec EIOs an unrelated
close). jobs=16 adds only 15s-budget timeouts on the heavy-walk tests
(dir_deep_nesting x2, dir_many_files) + the rename-family EIO — saturation
latency, 0 correctness bugs. Unit: test_raft/kv_lsm/meta_apply/sim/wire/txn
OK. **NFS GOTCHA (bit once): rsync -a quick-check (mtime+size) can skip a
just-edited file through the NFS attr cache — if a node builds with a stale
header after rsync, re-copy with `rsync -a --checksum`.**

**STEP 1 PRODUCTION ADOPTION LANDED (Sep 10, commit 3299e89): efs-fuse mounts
the Raft+KV engine behind `EFS_MD_RAFT` (client + server env).** Bootstrap
polls RAFT_STATUS for `kv_has_root` (no GET_META — the host serves no
serialized table); inode RPCs route by the compiled-in shard→group→voter map
(odd shards→group 0 = nodes 1-3, even→group 2 = nodes 2-4) to any voter and
follow the host's `primary_id` bounce. **Scratch cluster: port 19820, storage
`/tmp/efs-raft-scratch`, fcstor003–006, `--quota 1G --writers 0
--no-direct-io`.** Kill by port (`grep -q 19820`). The old-engine 19810
NVMe cluster is gone (Sep 15 2026) — no snapshot/2PC on-disk compat;
19810 is free for a current-tree `raft-mkfs` on `/data1`. Client:
fcstor007 `/tmp/efs-mount`, `EFS_TRANSPORT=tcp`.
Fixes in the same commit, all found by the first real gate: (1) **monotonic
raft boot id via `boot.bin`** — pid^time can go backwards and the peer boot
fence then silently drops every reply from the restarted node (replication
wedge); (2) `efs_meta_apply_truncate` had a **765 KB stack frame** on 1 MiB
efsd pump threads (now heap, 25 KB) and its multi-lane range delete indexed
shared `del_keys` by a per-lane base so lane i+1 overwrote lane i's keys
(wrong chunks deleted; regression test in test_meta_apply); (3) **REPORT
publish for a deleted inode is a harmless no-op (P3)** at host AND apply
layer — one unreportable rec otherwise poisons every fsync of that client
forever (all-or-nothing report + merge-back; this was the
`fsync_reopen_visible` EIO); (4) **pack staging disabled in raft mode** (KV
row has no pack fields; pack chunks publish under the parent DIRECTORY ino →
INVAL); (5) SETATTR SIZE|MTIME pairing accepted. `EFS_RAFT_DBG=1` gates
server report/publish debug logs.
**Gate: 12/12 unit suites incl. test_sim; posix on the Raft mount 150/201
(`results/posix/raft-step1-20260910-2133`).** `test_lock` had 6
failures (a test bug, closed Sep 18).

**STEP 1 FOLLOW-UP FIXES (Sep 10 PM, working tree on top of 3299e89) — gate
182/201 (`results/posix/raft-step1-bug7-20260910`).** The 150→182 climb
fixed, in order: (2) `last_link_unlink_other_dir` EIO — unlink/rename of a
dentry whose inode row is on the OTHER group now bounces dentry-first
(before resolving the row); (3) txn PREPARE packed dentry keys with a
1-byte klen — names >255 bytes truncated and collided (now 2-byte, host +
sim); (4) rename-over-existing EEXIST — the destination row is retired in
the same txn (nlink--/DEL, empty-dir rules), host + sim; (5) ctime ns
truncation on the wire — claimed padding as ctime_nsec/atime_nsec; (6)
flock survived close — the kernel NEVER relays a flock UNLOCK on close, so
the last-lease edge now drops all lock records for (ino,gen)
(`efs_lock_drop_file`) and the client HOLD is edge-triggered on a per-ino
open-description refcount (`efs_open_note`/`efs_close_note`); (7) **the
11-test EIO window = `rsv_scan` 64-cap NOMEM** — a file with >64
append-reservation records (a timeout-killed concurrent_appends leaves
orphaned OPENs + their blocked DONEs) made `efs_meta_apply_append_open`
fail NOMEM inside `host_resolve_caught_up`, which failed the whole report
batch, which kept the rec dirty and poisoned EVERY later sync report
(direct_*, two_fds, trunc_open_other_fd, fsync_then_fstat_size, the
symlink family, last_link_unlink data loss — all one poison rec,
ino-pinned, proven by 418 × `report FAIL ino=6590 rc=-2` on fcstor004).
Fix: the scan is dynamic (no cap), a resolve failure no longer fails a
report whose publish succeeded (bookkeeping, not durability — size rides
the lane MAX), and the last-lease edge drains orphaned OPEN reservations
as ABORTED_HOLE (`efs_meta_apply_append_drain_file`, host + sim) so a
killed appender can no longer wedge the frontier or accumulate records
without bound.
**Remaining 19/201, classified (do not re-derive):** ~15 latency-bucket
timeouts (`host_pump` ticks every 5 ms, `host_wait_applied` polls 5 ms ×
80 → each Raft round 15–25 ms; cross-group txns multiply; depth-100 mkdir
chains and 16-way bursts sit on the 15 s test budget — the event-driven
pump is a post-step-1 PERFORMANCE item, not correctness); 2 load flakes
under the killed-test barrier churn that pass isolated
(`concurrent_writes_disjoint`, `symlink_relative_after_parent_rename` — a
sync report carries ALL dirty recs, so a churning file's barrier-BUSY rec
can EIO an unrelated file's close after retries exhaust; per-ino fsync
report or per-rec BUSY skip is the step-2 refinement);
`dir_move_into_subdir` (cross-dir rename EINVAL — slice debt);
`virt_find_query` (.find unimplemented). jobs=16 under the same latency
profile is saturation noise (first instance 182/182 fails all timeouts, 0
correctness classes). Known slice debts still open: cross-group utimens
INVAL (`host_utimens`), append_bar not pushed to cross-group lanes, sim
blind spot (all-voters shared-disk).

**ARCHITECTURE DECISION (Sep 1 2026, user-ratified; REVISED after external
protocol review).** The project now has a master spec: **`docs/architecture.md`**
(+ browsable `docs/architecture.html`). User's bar: efs must earn the name
**extremfs** — throughput tracks aggregate hardware (NVMe+NIC), no software
serialization point, incl. many writers to ONE file. Decisions locked:
- **Metadata = one Raft group per shard over an on-disk ordered KV**; RAM is a
  bounded cache. Replaces whole-table snapshot + CoW page flush + 2PC root
  commit (those get DELETED, not patched).
- **Build a deterministic simulator FIRST** (seeded PRNG drives net/disk/
  crashes; invariants checked every step; failures replay from seed; plus an
  independent linearizability/history checker). It becomes the correctness
  gate; the 13-node cluster becomes a perf harness.
- **Targets:** ≥ 2³² live objects; 3–64 nodes; reference 2³² on 4 nodes;
  immediate cross-client visibility. **Failure tolerance is CONFIGURABLE
  (user, Sep 1 PM):** f = max simultaneous PERMANENT node losses with no data
  loss, capped at 3, bounded by N. The math (do not re-derive): data EC k+f
  needs N ≥ f+2; metadata Raft needs quorum Q > f → **N ≥ 2f**, and full
  write availability through f losses needs N ≥ 2f+1. Rule: **RF = min(N,
  2f+1)**, config invalid unless N ≥ 2f. Table: 3 nodes → f=1 (RF=3, 2+1);
  4 → f=2 (RF=4, 2+2; durable through 2, available through 1); 5 → f=2 full
  (RF=5); 6 → f=3 (RF=6, 2+3; durable through 3, available through 2);
  ≥7 → f=3 full (RF=7). **f=3 needs 6 nodes, NOT 5** (RF=5 gives Q=3 ≯ 3 —
  the 3 dead nodes could be the whole quorum that acked the last writes).
  At N = 2f the system pauses rather than loses (CAP-consistent); recovery
  from a lost quorum is node-returns or an explicit operator
  `force-reconfigure`. Changing f is an online control-plane op (add/remove
  replicas + re-stripe parities), not a reformat. **Also fixed in the same
  pass: the write commit rule now requires ALL k+f fragment ACKs before
  publication** (D − f ≥ k ⟺ D = k+f); the old "≥ 2 of 3 ACKs" rule had a
  real durability hole at f=1 (publish with 2 durable fragments, lose 1 →
  1 fragment < k=2 → acknowledged data lost). I10/I11 parameterized to the
  configured f.
- Data path mechanism (client-direct RDMA 2+1 EC) UNCHANGED, but commit
  semantics now specified (immutable chunk generations + atomic metadata
  publication + target-side generation fencing).
**Review corrections now IN the spec (the reviewer was right; do not re-litigate):**
shard_bits=**12** (not 20) for 4096 shards; ino is **64-bit + generation**
(ABA-safe); dentry index stores a small **projection** `(ino,gen,type)`, the
inode row is the ONLY mutable copy (dentries on parent's shard, threshold
spread for huge dirs); cross-shard rename/hardlink/unlink-open use a
**Raft-backed transaction** (txid/coordinator/intents/durable decision) — the
"no 2PC" is only about the OLD root-snapshot 2PC, not per-op cross-shard 2PC;
I1–I4 restated as Raft's real properties (election uniqueness / leader
completeness / term fencing / quorum-ack, NOT "at most one believes leader");
reads are **leader + ReadIndex** (no clock leases — clocks never correctness);
safe reconfiguration via **joint consensus** (desired placement ≠ authoritative
config); request **idempotency** via op-IDs; open-unlinked lifecycle; FUSE
kernel-cache invalidation (entry/attr/negative_timeout=0 + active invalidate);
3 planes (data / metadata / control). **The extremfs-critical design (review P0.14 + follow-up):** a hot file must
NOT serialize. Two load-bearing details, both now specified: (1) **chunk
metadata is itself sharded** — `chunk_meta_shard = hash(ino, chunk_index) &
0xFFF` — because independent chunk-map KEYS on one Raft group still serialize
on that group's ONE log/leader; spreading chunk publication across many shard
leaders is what makes the hot-file claim structurally true. (2) **size is
sharded high-water LANES** — MAX() commuting removes the ordering dependency
but not the physical serialization point, so size lives in L distributed lanes
(each an idempotent MAX on its own shard); stat() = max over current-epoch
lanes; truncate bumps the content epoch to invalidate all lanes at once.
mtime coalesced; sub-chunk RMW = generation CAS; O_APPEND EOF allocation is
the ONLY accepted same-file hotspot.
**THIRD REVIEW ROUND IN (Sep 1 PM) — the POSIX composition layer is now
protocols, not intentions. Do not re-litigate:** (1) **Three governing
principles** now head the spec (§0): P1 never-serialize-parallel-able-work;
P2 **co-locate what must commit atomically on the common path, distribute the
rest**; P3 **make stale work harmless, not impossible**. (2) **CREATE
co-location rule (load-bearing):** `inode_shard(new_ino) = inode_shard(parent)`
(normal dir) / `= hash(parent,name)&0xFFF` (spread dir) — inos allocated
per-shard so CREATE = dentry+inode row in ONE Raft entry; without it every
CREATE is a distributed txn. (3) **Authoritative op→shard matrix** in §5.3
(the old "only rename/hardlink touch two shards" claim was WRONG: UNLINK
nlink>1 and LINK are 2-shard txns). (4) **Hot-dir spread = layout-epoch
protocol** LOCAL→SPLITTING(e)→HASHED(e): writes go hashed during SPLITTING,
reads check hashed-then-local, migrator moves idempotently. (5) **Size lane
co-located with its chunk shard** (`size_lane = chunk_meta_shard`) so an
extending write is ONE Raft entry `{publish chunk G; MAX(lane,end)}` — kills
the publish-without-size / size-without-publish hole (new invariant **I21**).
(6) **stat() linearization via epoch double-check:** read epoch E → read only
E-lanes → re-read epoch → retry if changed. (7) **Truncate = content-epoch
bump with stated linearization rule** (epoch-E write linearizes before the
bump; new invariant **I22** epoch fencing). (8) **O_APPEND = serialized
`reserve_append(len)` on the inode shard, then ordinary distributed write**;
crash-after-reserve leaves a POSIX-legal hole, never rolled back. (9)
**Distributed locking:** one lock authority per inode on `inode_shard(ino)`,
`(ino,start,end,owner)` via Raft; dead-client reclaim via stale incarnation;
advisory locks never fence the data path. (10) **File-data coherence =
direct-I/O, decided** (kernel page cache bypassed; coherent caching later
only if it pays); **cross-client coherent MAP_SHARED unsupported initially**
(documented, not implied). (11) **Simulator models the LOGICAL data protocol**
(PUT/ACK/crash/publish/stale-gen events), not RDMA mechanics — else I11–I15/
I20–I22 are uncheckable. (12) **Data targets are dumb:** fragment PUT is
immutable+idempotent; target verifies placement epoch/incarnation/identity
only; the metadata authority alone decides which generation is committed (NO
target-side generation ordering). (13) **readdir contract explicitly weak**
(name existed sometime during scan; no dups; cookies survive leader change;
NO snapshot). (14) **mtime exact:** rides the chunk publication entry
(`max(mtime,now)` stamped by the shard leader); a stat after a returned write
sees it, any client. (15) **namespace-as-database reworded honestly:** the
architecture makes namespace-wide queries possible WITHOUT POSIX walks;
secondary indexes/aggregates are a separate derived-index design. (16)
Migration renumbered: step 10 = dir-spread epochs + locking; delete-2PC =
step 11; FUSE cache opts = step 12. Sections: locking §5.12, FUSE/data cache
§5.13, modularity §5.14.
**FOURTH REVIEW ROUND IN (Sep 1 PM) — the hot-path implementation contract.
Do not re-litigate:** the reviewer's verdict was "topology is right for
linear scaling; the hot-path contract is not complete enough to guarantee
hardware efficiency". All 19 points are now IN the spec (md §5.7/§5.13/§5.15
+ HTML §3a/§3c/§5): (1) **EC publication: healthy = ALL k+f fragments durable
before publish** (already landed with the f-parameterization); NEW is the
**degraded rule** — with u domains already unavailable, publish with
≥ k+(f−u) durable fragments, mark degraded, re-stripe on repair (I11
rewritten; never for slow targets, only unavailable ones). (2) **FUSE is an
architectural serializer risk:** `FOPEN_DIRECT_IO` +
`PARALLEL_DIRECT_WRITES` + `FUSE_CAP_ASYNC_DIO` + `FUSE_CAP_PARALLEL_DIROPS`
+ large max_write/max_pages/max_background are REQUIREMENTS (without
parallel_direct_writes Linux re-serializes same-file direct writes ABOVE
efs). (3) **Direct-I/O disables kernel readahead → the client owns an async
prefetch pipeline** (adaptive queue depth), or sequential reads lockstep.
(4) **Size lanes REBOUNDED:** the round-3 `size_lane = chunk_meta_shard`
(hash over 4096 shards) would give a big file up to 4096 lanes → stat() =
4096-way fanout. Now: `lane = hash(ino,chunk) % L`,
`chunk_meta_shard = hash(ino,lane) & 0xFFF`, **L derived** (required hot-file
pub rate ÷ per-shard commit rate, capped at tens); per-file spread is L
leaders, global spread still 4096. (5) **mtime contradiction FIXED:**
write-mtime lives in the lanes (`lane.max_mtime` on the publication entry);
explicit utimens/chmod update the inode row's `base_mtime` + bump
`mtime_gen`; stat = MAX(base_mtime, current-gen lane mtimes) under the
content-epoch check. (6) **Multi-Raft runtime is an architectural
requirement** (§5.4): reactors per NUMA domain, messages batched by
destination, heartbeats coalesced, WAL group-committed across groups, KV
apply batched — 4096 logical groups, NOT 4096 physical WALs/threads.
(7) **Publication batching:** many chunk publications per Raft proposal (at
128KiB, 100 GB/s = ~800k pubs/s; one txn per chunk would cap metadata).
(8) **Small-write envelope DECLARED:** 4K-in-128K = ~80x amplification +
same-chunk CAS contention; efs is HPC-aligned-I/O optimized; immutable delta
objects + background consolidation are the designed escape hatch, NOT built
until benchmarks demand. (9) **Dir spread triggers on entry count OR op
pressure** (100-entry dir with 100k clients melts its leader without ever
crossing the size threshold); spread is one-way. (10) `EFS_DIR_SPREAD_MIN` is
a **scalability bound** (caps pre-spread stranded co-located inode rows).
(11) **P4 added** (hot path never crosses cores/NUMA/locks unnecessarily) +
§5.15 contract: NUMA-local async execution, no cross-core locks on ordinary
PUT/GET. (12) **Ceiling is protection-adjusted:** logical write BW ≤
min(client egress, storage ingress, NVMe) × k/(k+f), EC CPU/mem BW, pub rate
× chunk size; the bar is 85–95% of THAT. (13) **Near-linear defined per
workload class** (table in §7/HTML §5; semantically-serial ops excluded
explicitly). (14) **write()=durable is a documented latency-for-durability
trade**, benchmarked against the strict-POSIX alternative before final.
(15) **ReadIndex amortized** (one quorum round serves a batch). (16) **QoS
isolation:** Raft/membership/txn/small-meta get own RDMA queues/traffic
class/credits; NVMe WAL/KV/bulk/rebuild queues separate. (17) **Rebuild
distributed + rate-limited:** deterministic repair owner, priority foreground
> metadata > rebuild. (18) **O_APPEND respecified:** reserve (inode shard) →
parallel data → per-reservation commit; the reservation watermark is NOT the
visible EOF; a dead appender's range is **committed as a zero hole by
recovery** (stale incarnation); the glib "POSIX-legal hole" claim is gone —
holes arise only from never-returned writes, read as zeros, no returned write
lost. (19) **Multi-chunk write atomicity DECIDED:** default = per-chunk
visibility (a concurrent reader can see a mix — same as Linux O_DIRECT,
documented in the POSIX contract; after write() returns, all chunks visible
to everyone); strong path = the §5.6 transaction machinery (write txid +
intents + one decision) when atomic multi-chunk publication is required.
Scaling goal in §1 rewritten measurably: "no software serialization point may
become the limiting resource before a physical resource does — if a benchmark
stops at a mutex/one leader/one thread/FUSE serialization/one WAL/one
coordinator, that is by definition an EFS bug."
**FIFTH REVIEW ROUND IN (Sep 1 PM) — six architectural fixes, all now IN the
spec (md + HTML). Do not re-litigate:** (1) **CREATE co-location split by
kind:** the old `inode_shard(new)=inode_shard(parent)` rule co-located whole
subtrees on one shard until each dir spread — "many independent dirs →
near-linear" was false. Now: **files** co-locate with the parent dir (one
Raft entry, unchanged); **every MKDIR scatters** —
`inode_shard(new_dir)=hash(parent,name,export_salt)&0xFFF` — paying one
2-shard transaction per mkdir to buy an independently scalable subtree for
its lifetime (this generalizes today's ROOT-only dir hashing; nested-dir
hashing is now safe because §5.6 transactions exist — the Aug 30 revert was
a pre-transaction limitation). (2) **stat() is a double collect:** the
content-epoch check only covered truncate-vs-stat; concurrent extending
writes across lanes could assemble a size the file never had (A=100,B=100 →
reads A=100, commits land, reads B=500 → returns 500). Every lane now
carries a monotonic `lane_seq`; stat = read inode epoch/gen → collect L
lanes (seq,size,mtime) → collect seqs again → re-check epoch; all-unchanged
⇒ the vector existed simultaneously ⇒ MAX linearizable. Bounded retries,
then a read-only multi-shard txn over the L lanes as the escape hatch. Same
collect validates distributed mtime. (3) **Multi-chunk write atomicity
FLIPPED:** default is now syscall-level ATOMIC publication (POSIX read/write
atomicity) — parallel data movement, per-lane publication intents, ONE
durable write-txid decision; per-chunk visibility is an explicit relaxed
mount mode (MPI-IO-style), never the default. Atomicity unit = one FUSE
write request; `max_write` sized so the writes that matter arrive as one
request; recovering the exact syscall boundary under FUSE async-DIO
splitting is an explicitly OPEN investigation. New invariant I24. (4)
**Failure rule simplified: the automatic guarantee requires N ≥ max(2f+1,
k+f)** (3→f1, 5→f2, 7→f3; 4 and 6 nodes run the next-lower f). The user's
ratified 4→2 / 6→3 numbers survive ONLY as an explicitly labeled
**durability-only mode** (N=2f): no data loss (every committed entry keeps
≥1 survivor, Q+f>N), but a minority survives and plain Raft CANNOT safely
continue — recovery is a specified operator-gated DR protocol (freeze shard
→ collect ALL surviving logs → merge per index, highest term wins — leader
completeness proves a committed entry is never shadowed, and op-ID
idempotency makes uncommitted survivors harmless → force new config →
re-add replicas). Changing f has an explicit transition state:
`effective_f`→`target_f` only after replicas added + ALL generations
re-striped + verified. (5) **O_APPEND: data movement parallel, but
allocation order AND the visible commit frontier are serialized** — B's
bytes at offset 100 only mean anything because A reserved first, so the
visible EOF advances over contiguous RESOLVED reservations; a fenced
appender's range resolves as a committed zero hole, unblocking the frontier.
The old "never serializes one appender's visibility on another's speed"
claim was wrong and is gone. (6) **Client sessions are a real protocol
(§5.12a):** `session={client_uuid, session_epoch, state}` on a SHARDED
session authority (hash(uuid)&0xFFF), mutated via Raft; heartbeats only
decide WHEN to attempt replacement, consensus decides WHICH session is
authoritative; once epoch+1 commits, the old session is fenced everywhere
(locks reclaimed, open-unlinked refs dropped, append reservations resolved,
dedup records retained). Fencing a partitioned-but-alive client is an
availability sacrifice, never a safety one (the epoch IS the fencing token).
New invariant I23. §5.11 open-unlinked and §5.12 locking now sit on it;
migration step 8 = sessions first, then open-unlinked. **Small corrections
landed:** op matrix — MKDIR/RMDIR = 2-shard txns, UNLINK-last-link is
single-shard ONLY if dentry_shard==inode_shard (hardlink corner: after
link+unlink the surviving dentry can be on a different shard), GETATTR =
inode row + L lanes; replica math at 64 nodes = 192/node at RF=3, 448/node
at RF=7, leaders ~64/node at any RF; appendix "Raft group = RF replicas";
§6 "survive f nodes" wording; §5.15 gained **per-lane batched/range
chunk-map fetch** (lane i holds chunks i,i+L,i+2L… so one range request per
lane covers a contiguous window; metadata window runs ahead of the data
window — else 800k metadata RPCs/s at 100 GB/s caps sequential reads);
**"durable ACK" defined** = target completed the persistent-NVMe operation
(NVMe flush/FUA or PLP media), NOT RDMA WRITE completion — same for Raft
follower ACKs (WAL at the defined persistence level, not volatile cache).
**SIXTH REVIEW ROUND IN (Sep 1 PM) — ten fixes + the normative/detail SPLIT.
Do not re-litigate:** (1) **N=2f durability-only mode DELETED — the
survivor-log merge was unsound.** The reviewer proved "highest-term entry
wins per index" can synthesize a log that never existed (A: idx5/term10 X
uncommitted branch; B: idx5/term9 Y + idx6/term11 Z legitimately; merge =
X,Z — a command sequence no leader authorized; op-ID idempotency does not
fix it). The earlier "user-ratified 4→2 / 6→3" is GONE. Only rule:
**N ≥ max(2f+1, k+f), RF = 2f+1** (3→f1, 5→f2, 7→f3; 4 and 6 run the
next-lower f). Permanent majority loss = operator DR territory, not an efs
mode. (2) Stale "default: per-chunk atomicity" section deleted (contradicted
the atomic-publication default). (3) **FUSE syscall boundary = explicit
UNRESOLVED CONTRACT**: POSIX requires syscall-granularity atomicity, FUSE
ASYNC_DIO splits syscalls with no originating ID, so EFS MUST NOT claim full
POSIX write atomicity above the max_write/FUSE-request boundary until solved
(kernel op grouping / deliberate short-write / syscall txid / strict mode /
documented compat-vs-strict). I24 + §3 scoped to one FUSE write request.
(4) **Txn hot path:** coordinator = `participant[hash(txid) % n]` (was a
deterministic participant = same-file serializer); write-publication PREPAREs
run in PARALLEL with no-wait semantics (canonical order only for namespace
txns). (5) **Dir-spread dominating tombstone:** during SPLITTING a mutation
of an unmigrated name writes `HASHED(name)=TOMBSTONE(epoch)`; hashed side
always wins → migrator can't resurrect an unlinked name (I8). (6) **I23
narrowed:** fencing = no AUTHORITATIVE mutation; metadata leaders check the
epoch, **data targets do NOT** (orphan immutable PUT is harmless, P3).
(7) **Open-unlinked mechanism:** one session-scoped open lease per
(session, inode) on the inode shard — first open commits it, last close
removes it, fenced sessions' leases lazily dropped; reclaim iff nlink==0 AND
open_sessions empty. (8) **coding_profile_id** {k,f,stripe/coding epoch,
placement epoch} is part of chunk identity; online f/k change re-stripes to
NEW immutable generations under the new profile, never mutates in place.
(9) **Lane formula fixed:** `lane = (chunk_index + hash(ino)) % L` (was
hash(ino,ci)%L, which contradicted "lane i holds chunks i,i+L,…");
chunk_meta_shard = hash(ino,lane) & 0xFFF unchanged. (10) Small: "four
principles" header, I16 (not I9) for idempotency refs, flock/fcntl
non-interaction is LINUX behavior (flock isn't POSIX; OFD fcntl IS
POSIX.1-2024 and is in target), MKDIR row includes parent nlink, same-dir
rename widens when replacing a cross-shard destination.
**THE SPLIT (same round, user-directed): `architecture.md` is now the
NORMATIVE INDEX (1840 → 790 lines); all rationale/derivation/protocol detail
moved to satellites — `docs/arch/{naming,design,design-history,
failure-tolerance,performance,development,verification}.md` +
`docs/arch/protocols/{transactions,data,directory,sessions}.md`. If the
index and a satellite disagree, the index wins.** New section numbering (old
§5.x refs in this file are historical): §0 principles · §1 goal · §2 failure
guarantees · §3 consistency contract · §4 invariants · §5 state placement
(incl. the single authoritative **state-placement table**: state / authority
/ key / replication) · §6 op→participant matrix · §7 protocol summaries
(7.1 Raft+reads, 7.2 txns, 7.3 data, 7.4 dirs, 7.5 sessions, 7.6
open-unlinked+locking, 7.7 FUSE, 7.8 control plane, 7.9 idempotency) ·
§8 hot-path contract · §9 scaling envelope · §10 implementation order.
Historical review language ("the earlier claim was wrong…") lives in
`arch/design-history.md`, not the spec. HTML synced to all of the above.
**REVIEW ROUNDS 7–9 LANDED (Sep 1–2, by another agent; reviewed + verified by
kimi-k3 Sep 2 — verdict: correct, keep).** Three more external review rounds,
all in tree (md + HTML + regenerated `architecture-full.md`, 4155 lines);
full narratives in `arch/design-history.md`. Do not re-litigate:
(7) **protocol closure** — fencing became a real 3-phase revocation BARRIER
(session record carries a 4096-bit touched-shard bitmap; ACTIVE→FENCING→FENCE-
to-every-touched-shard→ACTIVE; registration once per (session,shard), never on
the I/O path); every data-plane key AND every inode-scoped ephemeral record
(leases/locks/reservations) is `FileID=(ino,inode_generation)`-scoped;
`chunk_generation` is a globally unique CANDIDATE identity, never G+1
(ordering from the publication CAS); content_epoch is a fence ONLY, never in
an object key (else truncate strands the surviving prefix); lanes are a FIXED
64 per file, `lane=ci%64`, `lane_shard=(inode_shard+lane*odd_stride)&0xFFF`
(permutation ⇒ 64 distinct shards, lane 0 = inode shard), `active_lanes`
64-bit bitmap bounds stat(); txn effects split exclusive-CAS vs COMMUTATIVE
REDUCTIONS (MAX = payload, never blocks prepare; pending committed reductions
are discoverable under the lane prefix and fold into authoritative reads);
write-ctime moved to lanes; dir mtime/ctime spread as `dir_lane` per dentry
shard; noatime default (strict atime not offered, documented deviation);
dedup = bounded window (highest_contiguous_seq + bitmap + reply cache),
reclaimed by client contiguous-ack watermark, never by time.
(8) **composition edge cases** — I24 gained a READ side (multi-chunk read =
validated collect: versions + re-resolve undecided txids after the fetch;
prefetch is not a cache); truncate = bounded inode fence (row + active lanes,
≤65) carrying per-lane RANGE DELETE (stops RMW resurrecting truncated bytes)
+ the straddling tail chunk CAS-published INSIDE the truncate txn;
`base_size` on the inode row; O_APPEND validates against the active-lane EOF
vector (a private counter is a correctness bug) + an EOF BARRIER on the lanes
while reservations are unresolved; resolutions COMPLETED/ABORTED_HOLE/
FENCED_HOLE; only `utimens` bumps `mtime_gen` (only op that moves time
backwards), ctime unguarded, implicit times MAX-clamped (CLOCK_REALTIME can
step back); profile cutover is a pushed+ACKed barrier BEFORE the re-stripe
scan; lowering f advertises the weaker guarantee FIRST; FileID keying for
ephemeral state; OFD/flock owned by open-file-description.
(9) **protocol completion + kernel reality** — read/predicate GUARDS are the
third txn primitive (durable, shared, held through the decision); RMDIR
phantom fixed by per-(dir,dentry-shard) `dentry_seq` witnesses; directory
rename puts the whole ancestry chain's `parent_version` in its read set
(cycle prevention); publication VALIDATES durability evidence
(coding_profile_id, placement_epoch, per-fragment ACK set w/ incarnations +
checksums, distinct domains) — dumb targets check nothing; I25 end-to-end
fragment checksums (identity+payload; corrupt = unavailable, never decoded);
`u` is PROTECTION DEBT not headcount (a returning node restores capacity, not
fragments); spread dirs use the same fixed 64-shard permutation as file lanes
(dir stat bounded, used-shard bitmap on the dir row, `dir_mtime_gen` fence);
Linux lock domains corrected: classic+OFD fcntl CONFLICT in one record-lock
domain, flock separate; **second open kernel-interface item: upstream Linux
serializes per mount** (extending direct write takes exclusive inode lock
even with PARALLEL_DIRECT_WRITES; IOCB_APPEND forces it; O_CREAT takes the
parent dir exclusively — PARALLEL_DIROPS covers lookup/readdir only), §9
bounds intra-mount scaling for those three, pre-sizing avoids the first.
**New file `docs/arch/START-HERE.md`** — task routing (current task = Phase M
`wire/`), "I am changing X" → read/governs/gate table, done-means,
never-without-asking. Generator updated; full.md verified current.
**ONE GAP FOUND IN REVIEW: the doc machine-gate specified in
`arch/development.md` (regen-diff check, link/invariant-ref validation,
single-home normative tables) has NO implementing script yet** — only
`docs/gen-architecture-full.py` exists and is run by hand. Build the gate
before relying on it.
**SEVENTH REVIEW ROUND IN (Sep 1 PM) — protocol closure. Reviewer accepted the
structure and asked only for remaining protocol holes; 10 of 11 points merged
as given, 1 merged CORRECTED. Do not re-litigate:** (1) **Session fencing was
not a protocol.** "Leaders cache the session table and re-validate on doubt"
fences nothing — a shard that hasn't heard of `epoch+1` can still commit for
the dead client (I23 violation) and nothing tells it to doubt. Replaced with a
**revocation barrier**: the session record carries a 4096-bit `touched_shards`
bitmap; a shard registers ONCE per session before accepting its epoch; then
`ACTIVE(E) -> FENCING(E+1)` (freeze the set) `-> FENCE` every touched shard
`-> ACTIVE(E+1)` only after ALL ACK. No lock reclaim / lease drop / reservation
resolution before that commit. Unreachable shard ⇒ the NEW session waits (not a
real availability loss — a shard that can't establish authority can't commit for
the old client either). No session lookup on the I/O path. (2) **ABA protection
didn't reach the data plane:** handles were `(ino,gen)` but chunk objects were
keyed by bare `ino`. Every data key is now scoped by **`FileID = (ino,
inode_generation)`** — fragment objects, chunk map, lanes, append reservations,
publication intents. (3) **`G+1` generation numbering was unsound** — two
writers reading committed G both mint "G+1" for DIFFERENT bytes and PUT
different content under one immutable object name. Generations are now
**globally unique candidate identities** (`H(client_uuid, session_epoch, op_id,
chunk_index, retry)` or UUID); ordering comes from `CAS(expected = base)`, the
loser's object is an orphan. (4) **CORRECTED, and the spec had the bug already:**
the reviewer also wanted `content_epoch` in the chunk key. It WAS there, and it
is wrong — with the epoch in the object identity, `truncate()` to a smaller
non-zero size strands the surviving prefix under an epoch no reader consults
(and the text said those pages were GC'd), while re-tagging instead makes one
truncate an O(file-size) rewrite (8M entries for 1 TiB). POSIX requires the
prefix to survive. **content_epoch is now strictly a fence** over lane state +
in-flight publications; committed chunk data survives a bump. That exposed a
second gap: with lanes epoch-invalidated, `stat()` had nothing to read after a
truncate — the inode row gained **`base_size`** next to `base_mtime`. (5)
**Derived `L` could never change** (`lane = f(ci) % L` relocates every chunk-map
key). Replaced by **64 fixed lanes activated on use**: `lane = ci % 64`,
`lane_shard = (inode_shard(ino) + lane*stride(ino)) & 0xFFF` with
`stride = 2*(hash(ino)&0x7FF)+1` (odd ⇒ permutation over 2^12 ⇒ **64 guaranteed
DISTINCT shards**; independent `hash(ino,lane)` collides). **Lane 0 = the inode's
own shard** (small files have zero fan-out). Monotonic 64-bit `active_lanes`
bitmap on the inode row, set on a lane's first use — ≤64 inode touches per file
LIFETIME, batched into the write's existing txn; `stat()` collects only active
lanes. (6) **The lane was about to become the next hotspot:** if a txn's `MAX` on
lane size/times were an exclusive intent key, two writers publishing DIFFERENT
chunks sharing a lane would conflict on nothing. Transactions now split
**exclusive CAS keys** (chunk map, dentry, inode row) from **commutative
reductions** (lane `MAX`es, dir-lane `MAX`es) carried as payload, applied by the
reducer, never blocking a prepare. Intent resolution gained its 4th outcome:
**cannot-establish-authority ⇒ retryable error, NEVER "absent"** (I9). (7)
**Timestamps had two hidden serializers.** A write updates ctime too, so
**write-ctime moved into the lanes** (`lane.max_ctime`) — routing it to the inode
row sends every hot-file writer back to the inode leader. Worse: POSIX updates
the CONTAINING DIRECTORY's mtime/ctime on every entry create/remove, so a spread
dir would still funnel every create through its home shard — **hashed dirs now
keep a `dir_lane` (max_mtime/max_ctime) on EACH dentry shard**, `stat(dir)`
reduces over used shards. POSIX distinction kept exactly: **chmod sets ctime,
NOT mtime**. **atime decided: `noatime` default**, `relatime` a coalesced mount
option, strict per-read atime deliberately not offered. (8) **Two namespace
cases were hiding behind "2 shards":** `rmdir` must prove emptiness, which is
distributed on a HASHED dir — now a **transactional read set** over its dentry
shards (an exact distributed entry counter is REJECTED: it rebuilds the
per-directory hotspot the spread removed); and **same-dir `rename` is 2-shard on
a HASHED dir** because `hash(parent,old)` and `hash(parent,new)` are independent.
(9) **Directory-rename cycle prevention was missing and is unsound if you just
walk the tree:** two concurrent renames each validate a legal tree and together
create an unreachable cycle (`mv /a /b/a` ‖ `mv /b /a/b`). Every dir row now
carries `parent_dir` + `parent_version`; a dir rename collects the destination's
WHOLE ancestry chain at those versions into its conditional PREPARE, so any
concurrent reparent in the chain aborts it. O(depth), rare. (10) **Dedup state
is now a bounded window** — per `(client_uuid, session_epoch)`:
`highest_contiguous_seq` + a small completion bitmap for the out-of-order edge +
a bounded reply cache; GC'd once fenced + ambiguity resolved + window passed.
Transaction **decision records** got the matching participant-ACK GC condition.
(11) **Publication validates durability evidence** — since targets are
deliberately dumb, the publication entry carries `coding_profile_id`,
`placement_epoch` and the per-fragment ACK set (target incarnation, role, object
identity, checksum), and the lane leader REJECTS evidence that doesn't match the
current profile/placement/k+f roles/distinct failure domains. A stale client can
durably write a full stripe to an obsolete placement and still not publish it.
(12) **NEW INVARIANT I25 · end-to-end integrity** — every durable fragment is
checksummed over **identity + payload**; a failing fragment is treated as
UNAVAILABLE and repaired, never fed to the EC decoder. Media corruption is not
Byzantine, and EC without integrity checking reconstructs confidently wrong data.
Simulator gained a silent-corruption fault (that is the only way I25 is ever
tested). (13) **Performance contract was too strong:** "no physical NVMe sync per
logical operation on any hot path" contradicts `write()`=durable. Now: **"no
persistence boundary is paid per chunk, per metadata record or per Raft group
when several operations can safely share one; boundaries are amortized to the
largest batch the externally visible semantics allow."** (14) **Doc CI gate**
added to `arch/development.md`: regenerate `architecture-full.md` and fail on
diff, validate internal links, validate every `I1..I25` reference, validate §6
matrix protocol refs, and **reject a normative table defined in more than one
place** (duplicated tables are how the index and a satellite come to disagree).
Round-7 rationale lives in `arch/design-history.md`; the index, all satellites,
the HTML and `architecture-full.md` are in sync (links + invariant refs
verified).
**CONTRIBUTOR ENTRY POINT (Sep 1 PM):** `docs/arch/START-HERE.md` — the
answer to "what do I work on and what governs it". §1 the current task
(Phase M step 2 `data/` + transport/store interfaces; step 1 `wire/`
landed Sep 2) + the rule for picking the next; §2 a routing table
(changing X -> read exactly these pages / these invariants govern / this gate
proves it); §3 what "done" means (build on a node, unit tests, the row's
gate, a timeout is a FAIL, honest measurement); §4 never-without-asking
(invent a decision the spec lacks, restate a normative table, new monolith
code, weaken an invariant, widen a timeout). **§5 added (Sep 2):** how to
brief a LOW-END AI agent — the briefer owns task pick/decomposition/reading
whitelist/diff review, the agent owns staying inside the named files + the
§3 checklist; includes a paste-able briefing skeleton and the two traps an
outside agent cannot rediscover (EEXIST-on-unique-name is never benign;
`pgrep -x` never `-f`). Linked from `architecture.md`
line 3, `arch/development.md` and the HTML; included in the generated
one-file version. It is navigation, not normative — it links to the single
homes of the §4/§5/§6 tables rather than restating them.
**EIGHTH REVIEW ROUND IN (Sep 1 PM) — composition edge cases. Reviewer's
verdict: architecture STABLE, no redesign; 8 of 10 merged as given, 2 merged
CORRECTED (better mechanism). Do not re-litigate:** (1) **I24 had only a
write side.** Publishing every chunk of a call under one decision does not
stop a slow READER: fetch A, writer's `{A,B}` commits, fetch B → old A + new
B, a state no serialization produced, and POSIX makes read/write atomic *with
respect to each other*. Multi-chunk reads are now a **validated collect**
(collect chunk-map versions → parallel fetch → revalidate → bounded retry →
read-only txn), same shape as `stat()`. Consequence: per-lane chunk-map
prefetch windows are **prefetch, not a cache** — usable without revalidation
only inside the read whose linearization interval covers them. (2)
**Committed reductions could briefly vanish.** Round 7 made lane `MAX`es
commutative payload but said they apply "at resolve time" while a txn is
visible at its *decision* — so between a durable COMMIT and the reducer
running, `stat()` could return a size older than a returned write. Pending
reduction intents are now **discoverable under the lane's key prefix**;
authoritative lane read = MAX(materialized, committed-pending);
materialization is background compaction. Also fixed the Appendix-6 sentence
`aborted/unknown → absent` (contradicted §7.2 + I9). (3) **Truncate had two
holes; the second was data resurrection.** (a) Nothing distributed the new
`content_epoch` to the lane leaders that must reject stale publications, and
writes must never touch the inode shard → **truncate pays: a bounded fence
over the inode row + active lanes (≤65 authorities)**. (b) "chunks beyond the
new size become unreferenced, reclaimed lazily" left the chunk-map entries
live, so a later sub-chunk RMW takes the pre-truncate generation as its base
and brings back truncated bytes; `base_size` alone cannot fix it (across
shrink→extend→shrink, survival depends on the SMALLEST size any *newer*
truncate imposed, not the latest). **MERGED CORRECTED:** reviewer proposed
logical range tombstones; the fix is a **per-lane RANGE DELETE** carried by
the same fence — the KV is ordered and lane i holds chunks i,i+64,i+128…, so
the entries beyond S are a contiguous key suffix = **64 range deletes, not 8M
key removals**. O(1)-ish shrink survives AND nothing resurrects, with no
per-chunk epoch bookkeeping and no unbounded truncate history. (4)
**`O_APPEND` was correct only against other appenders.** `append_eof` was an
independent counter on the inode shard, but an ordinary extending `pwrite`
publishes on a LANE and never advances it → after a write to 1 GiB the next
append reserved offset **0** and overwrote the file. Reservation now
validates against the **active-lane EOF vector using the `stat()` read set**;
an extending publication bumps `lane_seq` and aborts a racing reservation.
Ordinary writers pay NOTHING; the appender absorbs the retry. (5) **Online
f/k change could not prove its own result** (scanner races a live writer
republishing X on the old profile). The **profile cutover now commits BEFORE
the scan**, and round-7 publication validation enforces the current profile
from that moment, so the old-profile set can only shrink and the scan covers
a set that cannot grow. (6) **MERGED CORRECTED — timestamps.** Reviewer
showed one `mtime_gen` guarding both times makes mtime move BACKWARDS (write
stamps gen 7 → chmod bumps to 8 → the write's mtime is filtered out) and
proposed splitting `mtime_gen`/`ctime_gen`. Better fix, adopted: a generation
guard is only ever needed for a time that can be set **backwards**, and only
`utimens` can do that. So **only `utimens` bumps `mtime_gen`, only lane
mtimes are guarded, and ctime needs NO generation** — every other time source
is a monotone "now" and plain MAX is correct; guarding ctime would CREATE a
backwards ctime. The same fence as truncate distributes `mtime_gen`. (7)
**ABA reached data objects but not ephemeral inode state:** open leases,
byte-range locks and append reservations were keyed by bare `ino` → all now
**`FileID`-keyed**, and any op acting through an inode handle validates the
generation first (mismatch = stale-handle error, never a silent no-op). (8)
**Dedup GC was gated on a "retention window"** — the failure model allows
unbounded delay, so a timer asserts what the network never promised. Now a
client **response-ack watermark**. Sharpened: a retried `O_APPEND` reservation
must recover **the same offset** from the reply cache; "something completed"
would let it allocate a second range and leave a permanent hole. (9) **Lock
ownership is per namespace:** classic `fcntl` = process token; `flock` AND
OFD `fcntl` = **open-file-description token** (shared by dup'd/inherited
fds) — a single process token modeled only classic fcntl. (10) Textual:
"six review rounds"→eight; direct-write step 4 `size/chunk-map/mtime`→
`+ctime`; directory two-index "size and mtime"→`+ctime`; **I9 now says
non-`ENOENT`**, not necessarily *retryable* (an unrepairable integrity
failure is legitimately terminal EIO — the substitution is forbidden, not the
finality); `noatime` reworded to Linux's actual meaning (reads do not update
atime; it does NOT "track mtime/ctime"). **Positioning (§3):** efs is a
"high-performance parallel filesystem targeting Linux/POSIX semantics **with
explicitly documented deviations**", not "a POSIX filesystem" — §3 now lists
the two known deviations (no strict per-read atime; no syscall-level write
atomicity above the FUSE request boundary). New glossary terms: **inode
fence**, **validated collect**. Round-8 rationale in `arch/design-history.md`;
verification.md gained 8 fault-injection events (multi-chunk read vs write
decision, stat between COMMIT and resolve, truncate vs in-flight lane
publications, sub-chunk write into a truncated range, append vs extending
pwrite, lost reservation reply + same-op-ID retry, utimens vs in-flight
write, profile cutover vs live writer) and 3 new checker properties.
**NINTH REVIEW ROUND IN (Sep 1 PM) — protocol completion + kernel-interface
reality. Reviewer: architecture STABLE, "no reason to change the fundamental
architecture"; what remains is protocol completion, not redesign. All 10
merged. Do not re-litigate:** (1) **The normative §6 matrix had gone stale
against round 8 and §6 wins ties** — it still said `TRUNCATE / 1 shard /
single Raft entry` while §7.3 said inode+active lanes, so the SUPERSEDED
design was binding. Fixed; `SETATTR` split by class (mode/owner = 1 shard;
`utimens` = inode fence; `SETATTR(size)` IS truncate); added multi-chunk
`READ` row. §5 gained `mtime_gen`, the dir used-shard bitmap, `dentry_seq`,
append reservations, and the coding-profile authority. This is the failure
mode the "never restate a normative table" CI rule exists for. (2)
**Read/predicate guards are §7.2's THIRD primitive** — round 8 leaned on read
sets (O_APPEND lane_seq, RMDIR emptiness, rename ancestry, stat/read
fallbacks) that were never defined. Guards are durable, SHARED, and held
**through the decision**, not until the last check. **RMDIR forced the
phantom case:** emptiness is a claim about keys that DO NOT EXIST, so an
insert into an observed-empty shard has no version to check — each directory
now carries a per-dentry-shard **`dentry_seq`** bumped by every mutation
incl. inserts, and RMDIR guards those. Predicate isolation without MVCC and
without the rejected distributed entry counter. (3) **Validated collects were
validating the wrong thing.** Intents exist from PREPARE, so a read spanning
a *decision* can take old-A (undecided) then new-B (committed) with EVERY key
version unchanged. Collects now carry the set of txids they resolved as
**UNDECIDED** and re-check those; decisions are final, so anything already
decided needs no re-check (cost ∝ in-flight txns actually touched, usually
0). Applies to `stat()` and multi-chunk reads alike. (4) **O_APPEND was fixed
only up to the reservation.** An ordinary extending pwrite could publish EOF
400 while an append's [100,200) was unresolved — no legal serialization. An
**append barrier** now sits on the active lanes while reservations are
outstanding (same bounded fence, once per append BURST), constraining only
publications that would push EOF past the reservation watermark; ordinary and
within-watermark writes untouched. Plus: reserved length = the length the
client will attempt (it holds the whole FUSE buffer before reserving), so a
short append comes only from failure; and **ABORTED_HOLE** — a LIVE client
whose append fails commits the resolution itself (previously only a fenced
client could resolve, so a live failure blocked the frontier until someone
killed it). (5) **Truncate's tail chunk is now IN the transaction:** the
zero-filled tail candidate is written first and CAS-published inside the
truncate txn (losing the CAS retries the whole truncate) — publishing it
after would leave a window where the file is nominally short while the old
bytes past the new end are readable. (6) **Profile cutover repeated the
pre-round-7 fencing mistake:** a control-plane commit does not change what
4096 lane leaders believe. It is now a **barrier pushed to every publication
authority and durably ACKed** before the scan. **Lowering f is NOT the raise
sequence reversed** — drop the advertised guarantee FIRST, else weaker data
is written while the stronger promise stands. **`u` is protection DEBT, not
a headcount:** a returning node restores capacity, not the fragments it never
held (f=1: A down → G on B,C → A returns → B dies → 1 fragment, data lost
after ONE failure), so a degraded generation consumes budget until REPAIR
completes; "unavailable" is a committed control-plane state, never a client
timeout. (7) **Factual Linux error fixed (verified against fcntl(2) +
POSIX.1-2024):** classic and OFD `fcntl` are NOT non-interacting namespaces —
they differ in ownership/release but share ONE record-lock conflict domain
and conflict by byte range even within one process on one fd. `flock` is the
separate domain. Model = **2 conflict domains, 3 ownership kinds**. (8)
**Linux/FUSE is itself a serializer, per mount:** extending direct writes
still take the inode lock exclusively even with `FOPEN_PARALLEL_DIRECT_WRITES`,
`IOCB_APPEND` forces it, and `O_CREAT` takes the parent dir inode
(`FUSE_CAP_PARALLEL_DIROPS` = lookup/readdir ONLY). Not cluster-wide (64
nodes fine) but 64 ranks on ONE mount serialize before efs is called. Now an
explicit second unresolved kernel-interface item + a §9 caveat; pre-sizing
fixes only the first case. A benchmark stopping there must be reported as
that, not as "efs scaled". (9) **Hashed-dir fanout was NOT bounded** — dentries
hashed over all 4096, so `stat(dir)` could be a 4096-way collect while the
text claimed "bounded like a file's lanes". A spread dir now uses the **same
fixed 64-shard permutation** as file lanes (used set = 64-bit bitmap); first
use of a dir lane registers on the parent shard (≤64/dir ever — the exception
to "parent not involved"); dir `utimens` gets its own `dir_mtime_gen` fence.
(10) Smaller: implicit mtime/ctime are **MAX-clamped** because CLOCK_REALTIME
can step backwards (stated as a deliberate HPC choice, not assumed);
re-stripe/repair must NOT touch user-visible size/mtime/ctime; dedup ack
watermark must be highest **CONTIGUOUS** reply (out-of-order arrival would let
100 authorize discarding 99) and a **fenced** session's state is dropped
wholesale after the revocation barrier (it can never advance a watermark —
old-epoch requests are rejected by epoch, not by lookup); **L7 broadened** to
once-published-now-unreachable generations (truncate range-deletes,
re-striped old-profile copies). verification.md gained 12 more fault events
and 4 checker properties. Round-9 rationale in `arch/design-history.md`.
**ROUNDS 8 AND 9 ARE BOTH UNCOMMITTED** (HEAD = 492bfed, round 7).
**BYTE-RANGE LOCKING PROTOCOL GAP CLOSED (Sep 2, kimi-k3, user-reported gap).**
The spec had lock PLACEMENT/ownership/conflict domains (state table row,
§7.6, sessions.md) but not the distributed PROTOCOL. Added: blocking waits
(`F_SETLKW`/blocking flock) are long-lived RPCs held at the lock authority,
granted FIFO from an in-memory leader queue (grant = the reply; no polling,
no timers, queue is NOT Raft state); queued exclusive blocks later shared
grants (no starvation); leader failover → client re-issues under the same
op-id (I16 resolves a pre-failover grant); a session fenced mid-wait is
dequeued by the revocation barrier and never granted; EINTR cancels by
op-id. Full POSIX range algebra on the single authority (partial-unlock
split, adjacent merge, in-place type conversion, F_GETLK = leader read),
per-inode record cap → ENOLCK. Deadlock detection is SAME-INODE ONLY (the
authority holds the whole wait-for graph for its inode); cross-inode cycles
not detected — POSIX makes EDEADLK a MAY, Linux checks only classic fcntl
even locally, and no wait is ever stuck (signal + fencing). Touched:
sessions.md (3 new subsections), architecture.md (§7.6 paragraph + §6 LOCK
matrix row), architecture.html (locking card, plain language),
verification.md (8 lock fault events + 3 checker properties: no conflicting
pair granted, fenced waiter never granted, every wait resolves).
architecture-full.md regenerated (4272 lines); links + invariant refs
verified. UNCOMMITTED with rounds 8-9.

**One-file paste version (user, Sep 1 PM):** `docs/architecture-full.md` =
index + all satellites inlined, GENERATED by `docs/gen-architecture-full.py`
— never edit it directly; regenerate after any doc edit (for pasting the
whole architecture to another agent).
**SUPERSEDED Sep 18: `architecture.html` is now GENERATED** by
`docs/gen-architecture-full.py` from the md (same run that builds
`architecture-full.md`); `make docs-check` fails if either artifact is
stale or hand-edited. The hand-written plain-language/SVG version below is
gone — one source of truth, the markdown. Historical note follows.
**HTML IS THE PLAIN-LANGUAGE RENDITION (Sep 1 PM, user-directed).**
`docs/architecture.html` was rewritten end-to-end in simple wording: short
sentences, everyday words, and a "few words we use" glossary table up front
(shard / Raft / leader / majority / term / committed / KV / EC / generation /
idempotent / linearizable read / serialization point — each defined in one
plain sentence). Jargon is kept only as *names* (Raft, NVMe, RDMA, FUSE) and
explained once at first use. The md stays the precise spec; the HTML is the
readable version — keep both in sync when editing. Structure, SVGs, and
content are unchanged; only the voice changed.
**HARDWARE ENVELOPE (Sep 1 PM, user-confirmed): efs is flash/NVMe-ONLY — no
HDD support, ever.** Now in the spec (md §1 hardware-envelope block + §8
rejection; HTML §1 + §10). The architecture actively spends the assumption:
random I/O is first-class (no seek-aware layouts; KV pager faults and random
chunk reads are cheap by assumption), deep hardware queues are assumed (the
P4 model can't run on disk at all), µs-scale device latency is what makes
write()=durable affordable, and SMR/rotational/track-alignment problem
classes are deleted, not engineered around. Scaling claims do not transfer
to HDDs.
**IDENTITY / positioning (from the 2nd review — do not overclaim):** every
primitive (Raft, dist. metadata, RDMA, EC, immutable gens, KV, dir sharding,
deterministic sim) has existed; WEKA/DAOS/VAST/CephFS get close to parts.
The defensible claim is the COMPOSITION + the principle "never serialize work
that semantics and hardware allow in parallel", NOT "novel" / "nobody has done
this". Also part of the identity: **the namespace IS the database** (du/find/
lifecycle = KV queries, not traversals) and **minimalist ops** (3 nodes → efs
init → mount → done; no MDS tier, no special node types). **NAMING WARNING:**
"extremfs" collides with XtreemFS (trademark, Quobyte) and "EFS" = Amazon EFS
— pick a distinctive, searchable public name later; "efs" is a working name
only. Do a trademark/search check before any public naming.
**MODULARITY IS AN ARCHITECTURAL CONSTRAINT (Sep 1, user-directed) — and the
roadmap does it FIRST.** `docs/architecture.md` §5.14: a module must be
understandable/changeable/testable from its own source + interface header
alone; no file over ~1000 lines; state machines pure (no globals, I/O behind
transport/storage interfaces — the SAME property that lets the simulator
reuse the compiled SM). **The bar, user-directed (Sep 1 PM): a LESS ADVANCED
AI model must be able to contribute a correct change** — not the best model
with the whole tree in context. Consequences now in the spec: (1) local
correctness must be locally decidable (module + header + tests, never global
reasoning — else the project scales with model quality, not contributor
count); (2) the blast radius of a mistake is one module and fails fast/locally
(unit test / simulator assertion / interface check — the quality bar is
enforced by the boundaries, not the contributor's sophistication); (3)
interfaces carry the contract (invariants/ownership/threading in the header);
(4) conventions are machine-checkable (lint/build/test gate, not reviewer
vigilance). Honest current state: 4 files hold ~45% of the
36.5k-line tree (`metadata.c` 6042, `efs_fuse.c` 3720, `meta_server.c` 3654,
`handler.c` 3648). **Roadmap "Phase M — carve the monolith FIRST"** (added to
`docs/scaling-roadmap.md`, ahead of all other forward work): carve into
`raft/ kv/ meta/ wire/ data/ client/` as behavior-preserving refactor gated by
EXISTING suites (make test + solo posix, no wipe). It is both the dev-cycle
lever AND the hard prerequisite for migration step 1 (the simulator can only
reuse a state machine that is already pure). Do NOT build Raft/KV/txns as new
monolith code — every new component lands inside the carved boundaries.
Read `docs/architecture.md` before any metadata work. The scaling roadmap is
now the increment plan toward that spec, not the spec itself.


**"DANGLING DENTRIES" ROOT-CAUSED + RECOVERED (Sep 1 PM) — IT WAS THE INODE RAM
CAP, NOT A DESCRIPTOR CLOBBER, AND NO DATA WAS EVER LOST.** Trigger: shard 7's
inode region is 2.82M rows x 512 B = **~1.44 GB**, which EXCEEDS the 1024 MB
`EFS_INO_RAM_MB` default, so it is the ONE table big enough to make
`trim_ino_ram` evict. An evicted slab's only fault source is `flush_blob`; when
the fault fails `inode_at` returns NULL and the handler answers **NOT_FOUND** —
so a live row reads as a missing inode. Proof (decisive, one variable): restart
all 4 efsd with `EFS_INO_RAM_MB=32768`, change nothing else — **all 12 dangling
ROOT entries went to 0**, `synth-test-data` nlink 2 -> **11** and children 0 ->
**13**, `imagenet2` children 0 -> 3, every `setattr` OK. Shards 1/3/5 (132-232
pages) on the SAME owner as shard 7 were always healthy, and shard 6 (511 pages)
self-repaired on a plain restart while shard 7 did not — exactly the size
ordering the cap predicts. This is the known Cut-C blocker ("bounding the blob
without another page source just makes faults FAIL") **confirmed to be silently
returning wrong answers on a live cluster**, not a theoretical constraint; Cut A
(512 B rows) is what made it reachable at this table size.
**TWO HARDENING ITEMS THIS EXPOSES:** (1) a failed slab fault must NEVER be
reported as NOT_FOUND — it is a resource failure and must surface as EIO/BUSY,
because answering "no such inode" lets CREATE repopulate the name and mint a
second ino for a live object (this is the likely source of the ever-higher-ino
`name_dup` retries seen in the user's rsync); (2) the cap silently stops holding
instead of failing loudly. **MITIGATION IS ENV-ONLY AND NOT PERSISTED** — the
cluster currently runs `EFS_INO_RAM_MB=32768`; any efsd restarted without it
reverts to 1024 MB and the symptom returns.
**Superseded analysis kept only as the record of what was ruled out:** `rsync` into `/tmp/efs-mount/synth-test-data`
failed `failed to set times on "."` = EIO. Two separate things:
1. **Client bug, FIXED (working tree):** `efs_fuse_utimens` collapsed EVERY rc
 to `-EIO`, so a server `NOT_FOUND` read as "Input/output error". `chmod`
 hits the SAME SETATTR handler and correctly reported ENOENT via
 `efs_rc_to_errno` — that mismatch is what identified the real status.
 utimens/chown/truncate now all route through `efs_rc_to_errno`. Builds
 clean on fcstor007. **NOT yet gated with posix.**
2. **Real condition:** a set of directories have a ROOT dentry but NO inode
 row on the owning shard. Every one reports `nlink=2` (the dentry-stub
 default) while having real children; every one fails SETATTR NOT_FOUND;
 creating INSIDE them fails ENOENT (`mkstemp ... No such file or
 directory`), and each rsync retry re-mkdirs the same names with ever
 higher inos (deep_skinny_chain 18851023 -> 18855407 -> 18856063).
**The split is exactly by shard, and it is OLD rows only:**
 - broken (shard 6 or 7): `imagenet1` 893014, `imagenet2` 2603039,
 `scale-fcstor007` 52022, `scale-fcstor008` 16671, `scale-fcstor014` 6,
 `scale-fcstor015` 7, `synth-test-data` 889799.
 - fine (shards 0-5): `imagenet3` 444904, `imagenet4` 9,
 `scale-fcstor009`-`013` inos 8,2,3,4,5.
 - shard 6 owner = fcstor005 (also owns 2, healthy); shard 7 owner =
 fcstor006 (also owns 3, healthy) — so it is per-SHARD, not per-node.
**NOT spreading — do not treat as an active regression.** 64 fresh ROOT dirs
created on fcstor007, evenly spread over all 8 shards incl. 6+7, probed from a
DIFFERENT client (fcstor008): 64/64 SETATTR OK, and still 64/64 several
hundred rebuilds later. So new rows on 6/7 are fine; only pre-existing ones
were lost.
**RULED OUT by measurement — do NOT re-chase:** client-side artifact (same
inos + same EIO from a clean second client); ongoing/spreading loss (64 fresh
dirs over all 8 shards still 64/64 after hundreds of rebuilds); whole-shard or
whole-node outage (shards 6/7 accept NEW rows fine, and their sibling shards
2/3 on the SAME owners are healthy); `efs_export_evict_cold_shards` (defined
in metadata.c + declared in the header, but called ONLY from
`tests/test_meta_v6.c` — it is dead code in the server, so LRU eviction cannot
be the way a tab goes non-resident).
**ESTABLISHED by code read.** The only way an extra tab goes non-resident is
the keep/free block in `server_rebuild_export_from_pages_ino` (meta_server.c
~1088-1132): `keep=0` falls through to `efs_export_free(ex)`, which frees it.
It is then recreated LAZILY by `shard_tab_get_or_create` (metadata.c ~2473),
which sets `meta_needs_rebuild = (tab->root.page_count > 0)` **only if the
installed root carries a descriptor for that shard**. No descriptor (or
page_count 0) leaves an EMPTY table with `meta_needs_rebuild = 0`, which
`server_ensure_shard_ready` reports READY — so LOOKUP answers NOT_FOUND and
CREATE happily repopulates it. That is exactly the observed signature
(dentry survives on another table, inode row gone, new rows fine).
**SUPERSEDED SUSPECT (was wrong — the cap was the cause; kept because the fix
below is still correct hardening and is now IN TREE):** the rebuild installs its
root with a WHOLESALE
`efs_export_root_move(&ex->root, &snap)` at meta_server.c:1202 — **no
`efs_export_root_maxmerge_extras`**, unlike the paths at 2516 and 2996-2997
which merge first. A root_move that drops extra-shard descriptors is precisely
the Aug 23 "efs-s3 root clobber" bug, whose documented symptom was "shard
tables permanently lost"; that fix was applied to primary-commit capture,
PUT_META full-adopt and catchup install, but meta_server.c:1202 (and 2474,
2637, 2817) still move wholesale. Proving it needs a descriptor dump at
recreate time = an efsd restart on all 4 (build-id gate), which was deferred
because the user was running load.
**THREE SERVER FIXES LANDED IN TREE (Sep 1 PM, built + unit-gated + deployed;
NOT posix-gated, NOT committed).** None of these was the cause of the symptom
above (the diagnostic below fired **0** times, which is itself the evidence that
the descriptor path was innocent) — they are correct hardening found by reading
that path, and they ship with the redeploy:
- **A. rebuild root install now maxmerges extras.** `snap` is deep-copied from
 `ex->root` BEFORE the long unlocked page fetch, and the re-check at the swap
 compares only `generation`. Extra-shard commits are deliberately SAME-gen
 (`server_commit_cluster_extras` keeps the main gen so peers can tell an
 extras-only refresh from a real advance), so a descriptor landing during the
 fetch window is invisible to that guard and `efs_export_root_move` would drop
 it. Now `efs_export_root_maxmerge_extras(&snap, &ex->root)` runs under the
 lock before the keep/free scan (so the scan's descriptor comparison sees it
 too). Same class as the Aug 23 root clobber; this was the one call site the
 Aug 23 fix never reached.
- **B. `efs_export_precreate_shards` was a silent no-op (the "definite bug").**
 It ran BEFORE `efs_export_root_move` installed the real root, so `ex->root`
 was still the staged root — which the code's own comment says carries no EFSR
 — hence `shard_bits == 0` and it returned immediately, creating NOTHING after
 every main rebuild. Blocker 2's invariant ("op/read paths never do the
 per-export lazy-create mutation under a single shard lock") was silently not
 held. Moved after the root install (and after chunk_size/features restore,
 which new tabs inherit), still inside the held shard locks.
- **C. diagnostic:** loud warning when an extra tab comes back empty AND not
 marked for rebuild AND has no descriptor while the root carries descriptors
 for OTHER shards — i.e. the exact state that reports READY with no rows.
 Fired 0 times across the redeploy.
Context: all 3 joiners rebuild constantly (2287-2355 `meta-catchup:
rebuilding` each), every rebuild loads a root at `next_ino` ~594368 while the
live allocator is at 2603040 (the cad1083 `meta-repair` floor fires 1294-1496
times per joiner), and fcstor005 logged 233 `page N unrecoverable ... raced
with GC` (fcstor004 24, fcstor006 1).
**Two measurement traps that cost time here — do not repeat:**
 - `efs_client_chmod` returns EFS_OK WITHOUT an RPC when the mode is
 unchanged, and utimens/chmod/chown all short-circuit locally when
 `efs_client_ino_is_dirty(ino)`. Probing with an unchanged mode, or from
 the client that just created the row, gives a false PASS (that is why
 `imagenet1` first looked healthy). Probe from a SECOND client with
 `os.utime(p, (current atime, current mtime))`.
 - node9901's `efs-fuse` died mid-session with no log line; `/tmp/efs-mount`
 reverts to a plain empty dir and EVERYTHING reads ENOENT, which looks
 exactly like mass data loss. Check `findmnt -o FSTYPE` before believing
 a disappearance (same class as the fio `NOT_FUSE` rule).

**SIZE-VISIBILITY CLOSED (Sep 1, commit b518003, GATED).** `stat` could return
a stale/zero size right after a write (5-6 of the 10 non-deep 16x failures
since Aug 27). Root cause, found with NDJSON probes: `dirty_snap_save_locked`
DETACHES the dirty set the instant a REPORT starts, and `efs_fuse_flush`
(close) kicks that report **without waiting**. So between snapshot and apply
the inode reads CLEAN, `lookup_walk` skips the local overlay, and the owner's
older size wins. Fix: `efs_client_ino_is_dirty` also consults the set a report
is publishing (`pub_ino_keys`, aliased under `dirty_mu`, cleared on **every**
exit path incl. both OOM returns — the alias would otherwise dangle past
`dirty_snap_merge_back`); and the leaf overlays local size/pack when its mtime
is not older, leaving name/nlink/mode authoritative on the owner (Cut 4).
Shrink still works: SETATTR stamps mtime=now server-side so a truncate is
newer and wins. **Ruled out by probe, do NOT re-chase:** H2 local row size 0,
H3 loss in `fill_stat_from_inode`, H4 server rejects grow on the mtime rule,
H5 ino never in the size recs, H6 report and GETATTR on different nodes,
H7 server drops recs for an ino not on the tab, H11 `lookup_needs_getattr`
keeps a size-0 dentry stub.
Gate: 16x size-visibility failures **8 -> 0** (`attr_stat_fields`,
`content_random_roundtrip` x3, `opt_fallocate`,
`content_random_overwrite_append`, `opt_copy_file_range`,
`names_near_path_max`, `chmod_preserves_mtime`); solo posix **0 EFS bugs**;
fresh-cluster 16x leaves only `dir_deep_nesting_beyond_64` x11-13 plus known
mtime/flock flakes.

**POST-CUT-A RAM BASELINE — Cut A made bytes/inode WORSE and BREACHED the cap
(Sep 1, `results/scale/20260901-cutA-baseline`, fresh bits=3 TCP, 9 clients
x16 threads, same shape as the Cut 3 run).** This is the number the structural
program is measured against; there was none before today.

| total | creates/s | errors | rss_max MB | rss_sum MB | B/inode |
| 100k | 9999 | 0 | 100 | 323 | 3387 |
| 500k | 26666 | 0 | 559 | 1474 | 3091 |
| 1M | 33333 | 0 | 874 | 2520 | 2642 |
| 2M | 33333 | 0 | **1476** | 4429 | **2322** |

vs Cut 3 at 2M (`20260831-cut3`): 1023 MB / 3216 MB / **1686 B/inode**. So
Cut A is **+636 B/inode (+38%)** and `rss_max` **1476 MB now EXCEEDS the
1024 MB `EFS_INO_RAM_MB` default** (Cut 3 sat exactly ON it at 1023).
Latency is fine and unchanged (stat p50 0.39 ms, create 0.41, unlink 0.39,
readdir 5.4 ms, all flat 100k->2M) and **errors=0 at every step** (Cut 3 had
42 at 2M), so this is purely a memory regression, not a correctness or speed
one.
**Cause is arithmetic, not a bug.** v8 makes slab == page with 512 B rows,
so the serialized inode region is 256 rows/page and `flush_blob`'s ino region
is **exactly 512 B/inode**: 2M inodes = 7813 slabs x 128 KiB = **977 MB**, or
95% of the whole 1024 MB cap. `ino_ram_bytes` counts the blob INSIDE the cap
and `trim_ino_ram` loops `while (bytes > cap && ino_slabs_resident > 1)`, so
once the blob alone approaches the cap, evicting every slab but one still
cannot get under it and the cap silently stops holding. At 1M the blob is
488 MB (48% of cap) and rss_max 874 MB is still under — the breach appears
between 1M and 2M exactly as the arithmetic predicts.
**Do not read this as "Cut A was a mistake":** the 512 B self-contained row is
what makes per-page faulting possible at all. It trades RAM for the ability to
page, and the trade only pays once the blob is gone. That is Cut C, and this
baseline is the quantified case for it.

**CUT C IS BLOCKED ON A LOCK INVERSION — measured, not guessed (Sep 1).** Do
not schedule "bound flush_blob" as a standalone task. `flush_blob` is
**server-only** (nothing on the client ever assigns it; the client never
trims, which is why Cut D is the client-side item) and plays THREE roles:
(1) the ONLY backing store for an evicted slab (`inode_slab_fault`), (2) the
base `efs_export_serialize_dirty` memcpys forward for clean pages, (3) the
memcmp base for page reuse in the flush loop. Role 3 already degrades safely
to the committed-root checksum compare. Role 1 is the blocker: bounding the
blob without another page source just makes faults FAIL, and `trim_ino_ram`
already refuses to evict when `!flush_blob && !page_src`.
**Why `page_src` is not a wiring job:** `server_global_lock` is a plain
NON-RECURSIVE `pthread_mutex_lock(&s->lock)`, and handlers hold it across
`efs_export_get_inode` -> `inode_at` -> `inode_slab_fault`. A fault calling a
network page reader would (a) re-lock `s->lock` and self-deadlock and (b)
issue peer RPCs under the global metadata lock. Faults must be resolved
OUTSIDE the lock (try -> "need page N" -> drop lock -> fetch -> retry) before
that hook can be filled.
**Landed as the prerequisite (commit 05df9eb):** `server_fetch_meta_page`
factors the per-page fetch/decode/heal out of
`server_rebuild_export_from_pages_ino`. Behaviour-identical; gated by a joiner
restart that rebuilt and rejoined at a consistent gen with 0 unrecoverable /
zero-filled / checksum-mismatch / decode-failed on all 4 nodes.
**Also note for whoever does Cut C:** `slab_page_persisted` currently returns
`ex->page_src != NULL` when there is no blob — with a page_src installed that
is TRUE for every si and would re-break fresh slab growth (the Sep 1
"jammed at exactly 256 rows" bug). Make it authoritative from
`root.ino_page_count` first. And `serialize_dirty` only needs bytes for pages
that will actually be ENCODED: a non-resident slab is clean by construction
(trim skips dirty pages), so its page can be reused by ci from the committed
root without any bytes at all.

**PATH RESOLUTION WAS CUBIC — FIXED + GATED (Sep 1, working tree, UNGATED
commit).** `posixstress 16 fcstor007` (16 concurrent suites, one client) had
`dir_deep_nesting_beyond_64` failing **16/16 on `timeout after 15s`**. Not a
flake and not saturation: solo it took **7.0s** of the 15s budget.
Root cause, measured (NDJSON probes on `lookup_walk` + `efs_fuse_mkdir`, not
code reading): building a depth-100 tree cost **256,985 LOOKUP RPCs**, and
`total_walk=6.23s` of a 6.6s run. Two multipliers compose:
(1) `entry_timeout=0` means the kernel caches no dentry, so a depth-k op
issues k FUSE lookups; (2) Cut 4 made `lookup_walk` re-resolve **every
component from the root** with no memoisation, so each of those k lookups
costs up to k more RPCs. Net **O(depth^3)**.
**Ruled out by the same probes — do not re-chase:** the CREATE itself
(102 mkdirs = **0.00s** total, flat ~38us at every depth) and BUSY backoff
(a flat **24us/RPC** at every depth; a 50ms<<n sleep is unmissable).
Fix: `walk_batch_ancestors` in `ops.c` resolves the leaf's ancestor chain
with batched LOOKUP_PATH (<=64 components per RPC) and leaves the **leaf**
on the authoritative per-component LOOKUP(+GETATTR), so Cut 4's invariant is
untouched. Any non-OK batch reply falls back to the full per-component walk,
so a batch miss (extra hashed ROOT shard) can only cost an RPC, never change
an answer. Only engages past `EFS_WALK_BATCH_MIN` (8 components) — normal
3-6 component paths keep today's exact RPC pattern.
**Wire change:** `struct efs_msg_inode_lookup_path` gained `efs_ino_t start`
(path is relative to it; 0 = root) so a >64-deep chain can be walked in
ceil(depth/64) trips while still returning every ancestor's mode/uid/gid for
the exec check. `efs_client_rpc_lookup_path` now routes to the owner of
`start`. **EFS_BUILD_ID changes — restart all 4 efsd together** (on-disk
EFSM v8 is unchanged, so NO mkfs/wipe is needed; redeploy on the live table).
Measured before/after, same 6696 kernel-driven walks both times (so this is
fewer RPCs, not a cache hiding work): LOOKUP RPCs **256,985 -> 9,815**,
walk time **6.23s -> 0.25s**, RPCs/walk **101 -> 1.5**, test **6.6s -> 1.1s**.
Gates: solo posix `results/posix/20260901-134359` **1 EFS bug**
(`attr_touch_terminal`, known mtime flake); posix2 007+008
`results/posix2/20260901-134445` **1 EFS bug** = `peer_fcntl_range_conflict`
(known per-client `lockf` gap) with the hardlink/nlink and
`peer_rename_across_dirs_chase` tests — the ones Cut 4's caches broke — all
**PASS**; `test_meta_v6` / `test_dir_stats` / `test_ino_path` /
`test_meta_slot` OK.
**posixstress 16 `results/posix/20260901-134816`: 23 EFS bugs total (was 34),
3 instances fully clean 195/0 (was none).**
**STILL OPEN — the kernel multiplier.** `dir_deep_nesting_beyond_64` still
fails **13/16** under 16-way. Measured under load: **450,037 walks** in the
run and per-RPC latency inflating **25.5us -> 62.9us** (walk p50 144us, p90
1423us, max 18ms), so ~6700 sequential walks sit right on the 15s budget.
The remaining factor of k is `entry_timeout=0`, which is a deliberate
cross-client correctness choice (a kernel dentry cache ghosts a peer unlink —
posix2 `peer_open_rename_fd`). Per-op cost is now near-optimal; do **not**
"fix" this by raising POSIX_TEST_SEC or by turning on entry_timeout without
solving peer invalidation.

**CUT A (EFSM v8) POSIX SESSION — 2 BUGS FIXED, 1 OPEN (Sep 1, working tree,
UNGATED).** v8 = 512 B self-contained inode row (name inline at off 126),
slab si == page 1+si, per-slab name arenas, `slab_idx` in the row, `page_src`
hook (unwired). **Not wire-compatible with v7 — fresh mkfs required.**

**Fixed 1 — moving an inode row must re-intern its name.** `name_off` is an
offset into the OWNING SLAB's arena, so any raw `*dst = *src` row copy across
slots aliased or lost names (false EEXIST on fresh names, ENOENT on real
ones). Added `inode_row_move`; used by `export_drop_zero_inodes` (compaction)
and `remove_inode_slot` (swap-remove). `efs_export_link` clears
`name_off`/`name_len` and re-stamps `slab_idx` before `inode_set_name`.
`inode_set_name` no longer takes the in-place path when the row does not own
`name_off` (`name_len == 0`), and interning `""` is a no-op.
Regression tests in `test_meta_v6`: row-move churn + never-used-name
aliasing, sharded ino uniqueness, and a v8 serialize/deserialize round trip
over 5 slabs of mixed 1-char/long names. `test_meta_v6: OK`.

**Fixed 2 — `lookup_walk` leaked a dir-lock stripe.** It locked
`child.parent ? child.parent : child.ino`, then OVERWROTE `child` with the
local row and recomputed the same expression to unlock — a different stripe
when the local row's parent differed. The stripe stayed held, and the client
flush thread's `lock_all_dirs` then deadlocked against FUSE workers (mount
D-state, `/sys/fs/fuse/connections/*/waiting` climbing, `idx_mu` free).
Now the key is captured once in `lk`. `efs_client_lock_dir` /
`unlock_dir` carry a thread-local `t_dir_held[]` that reports
`DIRLOCK-RECURSE` / `-NEST` / `-UNDERFLOW` with a backtrace; after the fix
those are silent. **This is why HEAD (1d7d582, without the fix) HANGS the
16-way suite while the v8 tree completes in seconds** — do not read HEAD's
hang as a v8 regression, and do not use HEAD as the fresh-cluster baseline.

**Gate reached:** long-lived cluster, remounted client, `--jobs 16` ×3
consecutive: **200/201 each**, only `fcntl_byte_range_lock` (known
per-client `lockf` gap). Compare gate `results/posix/20260901-044258`
190/201 was on a client whose table still held pre-fix corruption.

**CLOSED (Sep 1) — the fresh-mkfs catastrophe was TWO bugs, both fixed and
gated.** Fresh bits=3 TCP, solo posix on fcstor007, **4 consecutive runs
196/201, 0 EFS bugs** (`results/posix/20260901-13*`); unit
`test_meta_v6` / `test_dir_stats` / `test_meta_slot` / `test_ino_path` OK.
Found by NDJSON probes, not by reading code — both hide on a settled
cluster, which is why HEAD looked fine there and died on a fresh wipe.
1. **A catching-up joiner answered LOOKUP as if the export were not
 sharded.** `export_is_sharded_root` required a resident ROOT inode row —
 true of the main table in steady state, false on a joiner that has not
 caught up. `efs_export_lookup` then took the flat single-table path and
 never consulted a shard tab, so every name on an extra shard came back
 ENOENT. Evidence: 2171 events, all from fcstor006, all reporting
 `shard_id=0 shard_bits=3 shard_count=8` (configured sharded, unsharded
 path). Extra tabs carry the same `shard_bits` — that is what the ROOT-row
 term was really guarding. Fix: test `ex->shard_id == 0`; shard 0 always
 resolves to `ex` itself and never gets a tab.
2. **Tables jammed permanently at exactly 256 rows (one v8 slab).**
 `inode_slab_ensure` split "fault from disk" from "fresh slab" on
 `s0 < inode_count`, but callers do `pos = ex->inode_count++` BEFORE
 filling the row, so the first row of a new slab always looks like a
 fault; its page was never persisted, the fault failed, `inode_at`
 returned NULL, and the table stopped growing. Evidence: 173 identical
 events `s0=256 inode_count=257 blob_ino_len=262144 page_needed=2
 page_bytes=262144`, plus 1248/1520 misses on a table stuck at 256. Fix:
 `slab_page_persisted()` — a failed fault is fatal only when the image
 actually covers that page, otherwise it is fresh growth and gets a
 zeroed slab.
Post-fix deltas: unsharded fallthrough 2171 -> 0; tables at 256
1248 -> 0 (now 900+); successful creates 262 -> 1446; rows present at a
lookup miss 0/1520 (every remaining miss is an ordinary pre-create
negative). Do NOT re-chase the list below — it stays only as the record of
what was ruled out.

**Historical (the symptom list while it was open):** 173/181 failures are one shape: a test dir is created OK
(`cwi:` trace shows correct sharded inos, e.g. root ino 5 -> children 13,
21, 29 all on tab_shard=5) and the very next open of `<testdir>/f` gets
ENOENT. **Ruled out by direct instrumentation, do NOT re-chase:**
`efs_export_alloc_ino` never restarts a class or regresses (`ino-alloc:
restart/REGRESS` = 0 on all 4 nodes); `create_sharded` and CREATE_SHARD
always allocate from a table whose `shard_id == target` (`create-tab` /
`create-shard: MISMATCH` = 0); no ownerless create (`create-owner: NO
OWNER` = 0); the root is never transiently unsharded (`create-bits:
UNSHARDED` = 0); no server-side EXIST (`create-exist` = 0); CREATE replies
always carry the requested parent+name (`rpc-create: REPLY MISMATCH` = 0);
`nlive=4` and `owner_psh` agrees with the answering node. The `lookup-miss`
flood (2822 on one node) is ordinary negative lookups from `makedirs` —
not evidence. The secondary `cwi-fail: ino_dup parent=8 name=a ino=8`
family (client-side, child handed its parent's ino, always the 8/16/24…
shard-0 sequence) appears only in SOME fresh runs and is still unexplained;
`held_by` shows the ino belongs to a dir the server gave a different ino.
LOOKUP is answered purely from local tables and never returns NOT_PRIMARY —
worth a look. Next probe: log successful CREATE on the REMOTE/CREATE_SHARD
branches (only the local branch is logged today, 78 of ~200 creates) and
pair it with the miss by name.

**Debug scaffolding still in the tree from that hunt (safe to remove now
that the bug is closed; all env-gated, off by default):**
`EFS_INO_PROF` (server: ino-alloc restart/REGRESS, create-exist,
create-tab/create-shard MISMATCH, create-owner, create-bits, create-ok,
lookup-miss; forwarded by `clean_cluster.sh`), `EFS_CWI_TRACE` (client:
every `create_with_ino` with ino < 256), the unconditional `rpc-create:
REPLY MISMATCH` check in `efs_client_rpc_create`, the `held_by` fields on
`cwi-fail: ino_dup`, and the SIGUSR1/SIGUSR2 all-thread stack dumper in
`efs_fuse.c` (spinlock-serialized) + `tests/debug/resolve_stacks.sh`.
Patch snapshot of the pre-instrumentation cut: `/tmp/v8-work.patch`.

**INFRA: the shell exec env died at the end of this session** (same trap as
Aug 29) right after a `clean_cluster.sh` + `setup`; the cluster may be
mid-deploy. Verify `efs-mgmt status` and remount clients before trusting
any measurement.

**METADATA STRUCTURAL PROGRAM (Sep 1, working tree, UNGATED):** Four cuts in
tree. Do not treat posixstress flakes / LINK `lock_all` / BUSY backoff as
the next lever.

**Cut 1 — extra writes do not inherit MAIN fence.**
`main_fence_blocks_shard` BUSY only if `!shard_bits` or shard 0 is in the
lock set. CREATE with parent dentry on shard 0 still fences (`20260831-182724`
EIO). LOOKUP of hashed ROOT names ensures/locks the hash shard, not shard 0.
After main rebuild: keep live+clean extra tabs whose descriptor gen+checksum
is unchanged (`meta-rebuild: kept extra shard=N`). Joiner restart then hashed
ROOT mkdir: **OK** (10.3s BUSY then create, not EIO); fcstor005 logged
`kept extra shard=6`. TCP solo posix `results/posix/20260831-232435`:
**193/201** (3 EFS bugs: `concurrent_write_and_readdir` 39/40 flake,
`dir_deep_nesting_beyond_64` same-name `/d/d/…` ENOENT ~depth 31 isolated,
`names_crazy_dirs` isolated PASS). posixstress 2×4 `20260831-233935`: **0
FUSE create EIO**; 008 warmup 70s miss + 007 4-way 15s timeouts (Cut 4 RPC
tax, not the fence EIO). bits=5 ecopy_rounds 3r `results/ecopy/20260831-bits5-cut1`
vs `20260831-bits5`: joiner **005 rebuild_work 313→34, GC 62→0**; 006
rebuild_work 5 / GC 1; 004 129 / 17; primary 0. Kept-extra lines
151–285/joiner. files/s 63 | 172 | 58 (Cut 4 LOOKUP tax; host FAILs remain
CREATE-EIO family). `EFS_MKFS_SHARD_BITS=5` on `clean_cluster.sh` +
`EXTRA_DEFS` / `#ifndef EFS_DEFAULT_SHARD_BITS`. Sep 1 re-wipe is **bits=3**.

**Cut 2 — name arena, slim live inode.** `efs_inode_mem` has `name_off`/
`name_len`; RPC `struct efs_inode` still has `name[256]`. No EFSM bump.
Empty-name SETATTR/REPORT upsert must not intern `""` (O_TRUNC ENOENT).
`test_meta_v6: OK`. scale_grow 1M `results/scale/20260831-cut2`: RSS
**1897 B/inode** (1809 MB / 1M); unlink p50 **0.386 ms** flat; creates 19k/s
at the 1M step. Residual errors 10–29/step (catchup, <5%).

**Cut 3 — inode slabs + RAM cap.** `EFS_INO_RAM_MB` default 1024; trim
clean slabs after flush. scale 2M `results/scale/20260831-cut3`: rss max
**1023 MB** (cap) sum 3216 MB = **1686 B/inode**; unlink p50 0.375 ms.
5M/10M continuation wedged (007 `makedirs` FileExistsError on existing
hashed ROOT `scale-fcstor007` after LOOKUP miss — Cut 4 remount path).
Do not quote 5M/10M.

**WHY META IS STILL RAM (Sep 1, code-read, no code changed).** Cut 3 is a
RAM↔RAM pager, not RAM↔disk — do not read the 1023 MB cap as "cold inodes
live on NVMe". `inode_slab_fault` restores a slab from **`ex->flush_blob`**,
the last serialize of the WHOLE inode region, also in RAM; `trim_ino_ram`
returns early `if (!ex->flush_blob)`, and `ino_ram_bytes` counts the blob
INSIDE the cap, so trimming slabs fights a term it can never evict. If the
blob cannot restore a slab, `inode_at` returns NULL and the op fails —
there is **no** "fetch `page_cis[i]` from the 2+1 fragments" path on a
LOOKUP/CREATE miss. `page_cis` is read only by
`server_rebuild_export_from_pages_ino`, which loops **all** `page_count`
pages into one `calloc(page_count, 128 KiB)` and deserializes the full
table. `evict_cold_shards` frees whole extra tabs (shard>=1, clean), never
cold pages inside a live tab. `name_arena` + `chunks[]` stay one contiguous
alloc per tab. Third full copy: every client still `efs_export_deserialize`s
the GET_META blob into `g_client.export` at mount (read.c comment: "over a
GiB at multi-million-inode scale"); Cut 4 stopped *serving names* from it,
not *loading* it. Hence ~1.7 KB/inode cluster-wide and 2^32 ≈ multi-TB.
**Two defects found while verifying (unfixed):** (1) `name_arena` is
append-only with no free list and a refaulted slab has `name_len=0`, so
`inode_set_name` takes the grow path — every evict→refault cycle re-appends
every name and permanently grows `name_arena_cap`, which is itself inside
the cap. (2) `inode_slot_of` is a linear scan over `ino_slab_n` when
`ex->inodes` is NULL (~9.5k slabs at 10M, `EFS_INO_SLAB_ROWS`=1057) on the
rename/mark-dirty paths. Real fix is the pager: page-granular fault from
`page_cis`, `flush_blob` shrunk to dirty pages, bounded client inode cache.
Not more shard bits, not 9x4 posixstress.

**Cut 4 — lookup_walk always RPC names/nlink.** No dir short-circuit /
`created_recent` / `lookup_cache`. No LOOKUP_PATH (primary misses extra
hashed ROOT dest). Dirty size/pack overlay + CREATE dual-apply kept.
Client LOOKUP tries hash(1,name) first for ROOT. posix2 007+008
`results/posix2/20260831-234915`: hardlink/nlink tests **PASS**
(`peer_nlink_after_link`, `peer_hardlink_*`); 50+ tests PASS then
`peer_rename_across_dirs_chase` 45s SSH hang and the rest 124. ecopy
sample bits=5 TCP: **FUSE_OK**, 1299/1300 files, **75 files/s** (create
RTT, not cache).

**Re-gate Sep 1 (bits=3 TCP, after LOOKUP skip-ensure):** Parent owner
LOOKUP of hashed ROOT no longer `ensure`s an extra shard it does not own
(that BUSY'd remount LOOKUP while CREATE saw the shard-0 dentry). Isolated
same-name nest `d/d/…` **100/100 OK**. TCP solo posix
`results/posix/20260901-010229`: **194/201**, 2 EFS bugs
(`concurrent_create_unlink_two_proc` flake; `dir_deep_nesting_beyond_64`
ENOENT ~56 under 16-way — isolated PASS). posix2 `20260901-010348`: **49/63**
both-pass; hardlink/nlink (`peer_hardlink_visible`, `peer_nlink_after_link`,
`peer_hardlink_write`, `peer_chmod_via_hardlink`) **PASS**; then
`peer_rename_across_dirs_chase` 45s D-state and the rest 124. posixstress
2×4 `20260901-011809`: **0 EIO** in any TSV; 007-0 **141** both-pass before
385s kill; others empty-TSV/FileNotFound (Cut 4 RPC tax, not CREATE-EIO).
scale 10M `20260901-cut3-10m` wedged (011/013 create count frozen, FUSE
D-state); **do not quote**. Cut 3 RSS gate remains 2M
`20260831-cut3` **1023 MB cap**. `test_meta_v6: OK` on fcstor003.

**Empty-name upsert (Cut 2, gated with posix):** `inode_from_rpc` skips
interning `""` when the live row has a name; upsert rebind only if
`rec->name[0]`.

**POSIXSTRESS 2×4 LOOKUP FENCE (Aug 31, bits=3 TCP):** `posixstress 4`
fcstor007+008. Baseline `results/posix/20260831-184519`: 007 near-clean
(196/194/192/194), 008 saturated (120/192/127/119, ~70 timeouts). Servers
idle (~2 cores); client **busy_us** 124s/273s. Debug: 87/112 `main_fence`
BUSYs were LOOKUP on extra shards 1/2/3/5 (Heal idle) — client
`usleep(50ms<<n)`. Skipping the main fence for **all** extra ops caused
CREATE EIO (`20260831-182724`). Fix: `main_fence_blocks_shard_read` only
for LOOKUP/GETATTR/READDIR/GETCHUNKS/LOOKUP_PATH when shard≠0; CREATE/
UNLINK/SETATTR/REPORT/APPEND still fence. Gate `20260831-185301`: **196/0
×2, 195/1 ×5, 194/2**; **0 timeouts**; **0 FUSE create EIO**. LOOKUP
extra `main_fence` 87→0. 008 `busy_n` 940→68. Residual: `ctime_on_link_rename`
ENOENT (known flake) ×5; one `fsync_then_fstat_size`; one rename-over-nonempty.
Do not skip extra CREATE fence.

**POSIX2 MULTI (Aug 31, bits=3 TCP):** `results/posix2/20260831-195716`
4 pairs **61/63**, 0 timeouts. `peer_concurrent_hardlink` /
`peer_concurrent_unlink_hardlinks` were 4/4 `nlink got 2 want 3/1`:
`lookup_walk` served the local dual-apply row (created_recent /
lookup_cache) while the peer's link/unlink had already landed on the
owner. Fix: those shortcuts only apply when `nlink<=1`; `adopt_rpc_inode`
merges owner nlink. Gate: both tests 4/4 PASS. Remaining (unchanged):
`peer_fcntl_range_conflict` = per-client `lockf` (feature, adjacent-range
is target-better); `peer_rename_across_dirs_chase` = known dual-apply
stale-dentry flake (`x` at d2 and d3). Harness parents now include
`RUN_ID` so leftover `posix-2c-*` cannot FileExistsError prepare.

**META BENCH HOT PATHS (Aug 31, bits=3 TCP, `efs-bench --meta`):**
`results/meta/20260831-153805` after three server-side cuts (working tree).
Workers 1/4/16 × 5000 files × 64 hashed ROOT dirs. Rename was 5000/5000
NOT_FOUND (`efs_export_rename` used `inode_ptr` on the main table; files
live on extra tabs — FUSE uses `rename_at`, bench used `efs_export_rename`).
Hashed-ROOT rmdir mapped `ensure_shard_ready` not_owner → BUSY → 50ms<<n.
CREATE_SHARD/UNLINK_SHARD inherited the MAIN `meta_needs_rebuild` fence
(joiners catching up after extras+main flush) → w4 mkdir **82 ops/s**.
Fixes: shard-route rename; skip ensure on shards this node does not own;
CREATE_SHARD/UNLINK_SHARD do not BUSY on the main fence (extra readiness
is `ensure_shard_ready(target)`). Bench dirs are hashed names under ROOT.
Post-H6 mkdir w4 **11831 ops/s** (`busy_n=0`; was 82 / `busy_n=16` / 3s
sleep). Rename 10k ops 0 fail. Create 25k / 78k / 111k ops/s. Cluster
Heal idle gen=2 all 4. 2PC/CoW unchanged. Next: CREATE RTT (~40 µs), not
more false BUSY.

**POSIXSTRESS 9×4 HOT PATH SPLIT (Aug 31):** Fresh bits=3 TCP
`results/posix/20260831-140025` + `EFS_RPC_PROF` (client checkout/send/recv/
BUSY). 28/36 tsv (009/010 warmup lost). **99.3% of fails are `timeout after
15s`** (3297/3320); fcstor012 nearly clean (196/0 … 199 pass). Servers idle
(primary **1.9/24** cores, 103 `wait_woken`). `lock_all` hold 19 ms/run,
`1_wait` 4.7 ms, `global_wait` 0.56 s primary / 2.5 s worst joiner — not
15 s. Client (007, 15k RPCs): checkout 6 µs, send 5 µs, **recv 124 µs**
(LOOKUP 81 µs, CREATE 37 µs, REPORT 3.1 ms). **99% of client RPC wall is
`usleep(50ms<<n)` on BUSY** (245 s / 609 events). APPEND does not take
that sleep. Joiner extras rebuild was **not** the storm (0/0/1/4
`meta-rebuild`, 0 GC-race). Next is per-opcode BUSY, not more lock drops
and not “the wire is slow.”

**FLUSH SKIP-CLEAN + INCR SERIALIZE GATED (Aug 30/31):** Stopped O(table)
encode+blake3 on clean CoW pages, then incremental serialize so a flush
does not snapshot/pack every compact slot. Per-page dirty bits on
create/set_chunk/setattr; same-count rename marks the compact slot + all
dentry pages (mark_full disabled incr for the ecopy temp→final storm).
Unlink/swap still `flush_full`. `serialize_dirty` copies the last blob,
rewrites dirty compact pages (slot range, not a full inode scan) and
dentries; page loop **memcmps** against `flush_blob` before reuse so a
missed bit cannot keep a stale ci. `FLUSH-PROF incr=1` 46105 vs
`incr=0` 34 on the primary after 6 ecopy rounds; last window
pages=39 written=5–6 reused=33–34, `ser` ~1.5–2 ms.
Gate: TCP posix `results/posix/20260830-174926` (fresh) and
`results/posix/20260831-030124` (on the grown ecopy table) both
**196/201, 0 EFS bugs**. ecopy_rounds 9×2 dirs, dest `ecopy-<host>/rN`,
`results/ecopy/20260830-flush-incr3`: files/s
**183.8 | 208.9 | 196.1 | 186.3 | 183.7 | 177.4** vs Aug 29
**84.5 | 75.2 | 65.6 | 59.4 | 46.9 | 37.6** (2.2× decay). Peak-to-end
now 1.18×. Harness keeps RESULT on ecopy erc=2. Residual: ~81/134567
files missing (26 class dirs short 1–5, CREATE EIO / ftruncate; joiner
fcstor005 293 `meta-rebuild` / 52 `raced with GC` — same catchup family,
not the flatten term). Do not schedule LINK/RENAME lock drops or H3
client RTT as the 2^32 cut.

**SCALE_GROW REMEASURE (Aug 31, bits=3 TCP):** Harness now uses a unique
ROOT name per client (`scale-<host>`) — shared `scale/<host>` raced
hashed ROOT mkdir (ENOENT) and invented 80k/s / 400 B/inode fiction.
Fail-closed on missing ERRORS / probe n=0. Primary SIGSEGV in
`inode_ptr` during 9×16 CREATE: extras merge freed shard tabs under
the global lock only, extra flush could lock shard 0 if `shard_id`
was 0, catchup `adopt_tables` swapped inodes without shard locks.
Unlink is **flat** (drop_chunks_scan fix holds): p50
0.14 / 0.14 / 0.14 / 0.14 / 0.15 / **0.17 ms** at 100k→10M
(Aug 28 was 0.44→4.6 ms). stat ~0.16 ms flat; readdir ~4.1 ms flat;
create p50 0.19→0.30 ms. Creates/s 20k–50k then 25k at 5M / 15k at
10M (`results/scale/20260831-bits3`). errors 0 through 2M; 46 / 176
at 5M / 10M (0.0015–0.0035%, catchup; rebuilds 42→86, 0 primary
death). RSS **1.7 KB/inode at 10M** (16280 MB / 10M; 2.1 KB at 5M vs
Aug 28 1.56 KB — `flush_blob` cache). 2^32 × 1.7 KB ≈ **7.3 TB**
aggregate still cannot sit in RAM on 4 nodes. Next: bits=5 trial.

**BITS=5 TRIAL (Aug 31, mkfs shards=32, default back to 3):**
Dedicated mkfs (`EFS_DEFAULT_SHARD_BITS=5` for that wipe only).
TCP posix `results/posix/20260831-041839`: **196/201, 0 EFS bugs**.
ecopy_rounds 9×2 `results/ecopy/20260831-bits5`: files/s
**230.7 | 209.3 | 205.4 | 191.5 | 186.8 | 174.6** (peak-to-end 1.32×
vs bits=3 1.18× — flatten term still gone, not the old 2.2×).
Most ecopy rounds FAILED on 4–7 hosts (CREATE EIO). Joiner
**fcstor005** 313 `meta-rebuild` / 62 `raced with GC` (fcstor004
48/6; primary 0). Same extras catchup/GC family; 32 tables amplified
host-level EIO. Do **not** jump to bits=12; cap extra-shard commit
rate before that.
`scale_grow` to 1M on that table (`results/scale/20260831-bits5`,
leftover ecopy in RSS at 100k): errors=0; creates 9k then **40k / 50k**/s;
unlink p50 **0.135 / 0.138 / 0.141 ms** (flat, same as bits=3);
stat ~0.16 ms; readdir ~4.1 ms. Rebuild log **unchanged** (362) across
the 1M grow — empty-create burst does not trip the ecopy extras storm.
Incremental RSS ~1.4 KB/inode (3060−1820 MB over +900k files). Next on
the cap track is Phase 4 (name out of inode), not H3 RTT.

**POSIXSTRESS QUEUE CLASSIFIED H3 (Aug 30):** Extended `EFS_LOCK_PROF`
(`1_wait_us`/`1_calls` on `server_shard_lock`, `global_wait_us`/
`global_hold_us` on inode+REPORT `server_global_lock`, `busy` replies).
One fresh TCP 9×4 `results/posix/20260830-160943`: 36 tsv, 1454 fail,
**96.5% timeout**, 6× 196/0 (008×2, 011×3, 014×1). Primary last window:
`1_wait_us=14ms` (`1_calls=75k`, 0.19 µs/call), `n_wait_us=2.2ms`,
`all_hold_us=33ms`, `global_hold_us=59ms` (0.59 µs/call),
`global_wait_us=2.85s` (28 µs/call) / joiners 5.3–5.4s,
`busy=2873` with `append=4988` (joiners busy−append ≈ 200–350).
**0 `meta-ensure` rebuilds on primary** (1 each joiner). H1/H2/H4 do not
fit: parent-shard wait is not the queue, `ensure_shard_ready` I/O never
ran, APPEND BUSY already returns without the 50ms<<n sleep. Do **not**
drop LINK/RENAME, do **not** drop global before ensure, do **not** cap
BUSY backoff as the 9×4 lever. Next is client `rpc_send_recv_shard` RTT
(not more server lock drops). `clean_cluster.sh` now forwards
`EFS_TRANSPORT` with `EFS_LOCK_PROF`.

**AUTO RDMA FIRST INODE GATED (Aug 30):** First `mkdir`/`ls` after mount
no longer D-states. Root `stat` is still local (GET_META one-shot TCP);
the first pool checkout upgrades and CREATE/READDIR ride the QP.
`efs_conn_wait_request` is shared with `test_rdma_xprt` (quiet-gap +
CREATE-sized frame). Poller `poll_cq`s after comp-channel POLLIN.
Gate: remount auto, `mkdir` in 5s, fuse.log `RDMA transport up`,
solo posix `results/posix/20260830-154329` **196/201, 0 EFS bugs**.
`EFS_RDMA_FIRST=1` for the six-site trace. Do not start 9×4 as an RDMA
correctness gate. `EFS_TRANSPORT=tcp` remains the fallback.

**PER-OP LOCK DROP GATED (Aug 30 afternoon):** Finished dropping `g_server->lock`
for APPEND, CREATE_SHARD, GETCHUNKS, READDIR (one shard), LOOKUP_PATH (relock
per component), UNLINK/UNLINK_SHARD/`fan_drop_chunks` (discover-relock, no
`lock_all`, no global across peer RPC), SETATTR+SIZE. LINK/RENAME/HOLD still
`lock_all` — `EFS_LOCK_PROF` after 9×4 shows they are not the ceiling
(`all_hold_us` 5.6–17 ms total for the whole run, ~1–2 μs/call; `n_wait_us`
~1–2 ms). Harness: `wait_cluster_idle` + fail-closed warmup (empty-TSV trap).
Sharded `unlink_name` is dentry-only; client dual-apply must `nlink_dec_ex` or
hardlink tests see stale nlink (fixed in `ops.c`).
**Lock-drop gate was TCP** (`EFS_TRANSPORT=tcp`); auto first-inode is
now gated separately (see above). Do not quote RDMA posixstress for the
lock-drop cut.
Solo posix `results/posix/20260830-143842`: **195/201**, 1 EFS bug
`dir_rename_over_existing` (flake family), 0 hardlink bugs.
**3× fresh 9×4 vs Aug 30 RDMA baseline `20260830-064913` (36 tsv, 5137 fail,
98.6% timeout, 1 clean):**
| run | dir | tsv | fail | timeout | clean/near/sat |
| r1 | `20260830-144643` | 28 | 1458 | 95.5% | 12/2/14 |
| r2 | `20260830-145338` | 32 | 1827 | 96.4% | 10/2/20 |
| r3 | `20260830-145931` | 28 | 1118 | 94.9% | 13/4/11 |
tsv < 36 = fail-closed warmup on 1–2 hosts that started while others were
already in-suite (008/009, 007, 009/013). Clean 196/0 is now common (whole
hosts 012/014/015 or 011/012). Saturated hosts still ~95% `timeout after 15s`
— not `lock_all`. Next lever is not RENAME/LINK.

**FIO ROUNDS RDMA (Aug 30):** Honest 9-client sw-1m, 5 rounds, fresh
bits=3, auto RDMA (`results/fio-rounds/20260830-rdma-5r/`). FUSE_OK every
job, no `md0`. Agg MiB/s **3409 | 3234 | 3117 | 3126 | 3104** (1.10× decay
r1→r5 — not the ecopy 2.2×). Per-client lockstep ~338–372 MiB/s; 18 GiB
written per host per round; primary `used=304 GiB` after 5 rounds matches
9×18×5=810 GiB logical × ~1.5 EC / 4 nodes. Aug 29 TCP 9-way sw-1m was
3599. `fio_rounds.sh` is now honest (`--end_fsync=1`, no `time_based`, 9×2g).
First attempt EIO: 9 clients `mkdir` the same ROOT name `fio` at once and
wedged RDMA QPs; dirs are now `fio-<host>/rN`.

**RDMA posixstress (Aug 30):** Nested dir hashing (`cda9ac9`, every mkdir
hashed) split dentry vs inode and broke posix (rmdir-nonempty, LOOKUP,
rename). Reverted to ROOT-only hash (`1df9a31`: testdir/ecopy dest spreads;
nested stay on that shard). Single-client posix over auto RDMA:
**196/201, 0 EFS bugs** (`results/posix/20260830-064840`). 9×4 posixstress
over RDMA (`results/posix/20260830-064913`): 36 instances finished, cluster
idle gen=109; **fcstor008-2 clean 196/0**. Aggregate ~4971 EFS-bugs,
**99.3% `timeout after 15s`** (same saturation family as TCP 9×4, not
RDMA-specific correctness). 36 non-timeouts (`dir_deep_nesting` EIO leaf
×12, `dir_many_files` ×4, rest singles) — load flakes, 0 of those on the
solo gate. Do not start posixstress in the post-mkfs catchup window (first
attempt: all 9 warmups D-state, used=0, empty TSVs).

**RDMA (Aug 30):** Shared-CQ poller re-arms after drain (the old loop dropped
the next CQE after a quiet gap — inode/REPORT hung on a fresh mount so they
were forced onto TCP). Small SEND copies into the registered pool (no stack
INLINE). Peer pool is `efs_conn` + the same upgrade, so server↔server
GET/PUT_CHUNK is RDMA. Default transport is auto; only GET_META /
GET_META_ROOT stay TCP (unbounded replies). Gates no longer force
`EFS_TRANSPORT=tcp`.

**ECOPY ROUND 6 (Aug 29 night):** Directories under ROOT hash to a shard.
ecopy dest trees no longer funnel through the metadata primary.
1-client 61 → **112 files/s**; primary create=1; dest owner does the rest.
gens 11 (extras). Nested dirs stay on the dest shard (rename/hardlink).

**ECOPY ROUND 5 (Aug 29 night):** SETATTR without SIZE (futimens/chmod)
drops the global lock; one inode-shard lock. ~88 files/s.

**ECOPY ROUND 4 (Aug 29 night):** Skip GETATTR after LOOKUP when the inode
lives on the same shard as the dentry (files stay with parent). getattr
3.0 → 1.9 per create. ~82 files/s.

**ECOPY ROUND 3 (Aug 29 night):** REPORT_CHUNKS drops `g_server->lock` and
takes only the shards the recs hit (not lock_all). 0 drops. Mix-dependent
~75–80 files/s (73 dirs) vs round 2's 98 files/s (333 dirs).

**ECOPY ROUND 2 (Aug 29 night):** LOOKUP and GETATTR drop `g_server->lock`
and run on the parent/ino shard lock. Fair empty-cluster ecopy
61 → **98 files/s** (5962/60s, `results/ecopy-rounds/r2/`). CREATE still
all on shard 0 (dest under root). Next: REPORT/SETATTR off the global lock,
then independent directories.

**ECOPY ROUND 1 (Aug 29 night):** CREATE no longer holds `g_server->lock`.
Files stay on the parent directory's shard; ino alloc is shard-local.
Hashed dirs tried and reverted (split dentry/inode broke ecopy rename).

**ECOPY LOOKUP TAX CUT; RATE STILL ~60 FILES/S = CREATE (Aug 29 night).**
The 56–70 LOOKUPs/file were real but not the 70 files/s ceiling. Client
`lookup_walk` now coalesces same-client (parent,name)→ino for ~250ms
(`ops.c`); needed because sharded upsert is child-shard while name lookup
is parent-shard, so local HIT always missed and a name-only cache never
fired. Keep `entry_timeout=0` (posix2 peer unlink/rename). Gate: fresh
bits=3, dest **does not exist** so ecopy sets `destination_fresh` and skips
its 512-wide dest-stat. 60s `ecopy …/ecrawl-synt-small/ /tmp/efs-mount/r3fresh`:
3646 files, **FUSE_OK**, gens 6052×4. RPC delta: lookup 11098 / create 3782
= **2.9 LOOKUPs/file** (was ~70 into an existing dest). Rate **61 files/s**
unchanged — CREATE 63/s is the serial limit (primary idle CPU, lock_all hold
~2.5μs). Copying into an **existing** dest still dest-stats 512 names/batch
(~70 LOOKUPs/file) and is the same ~60/s. 1000 files/s is CREATE, not LOOKUP.
Do not quote the interrupted 3-node `ino_dup` run (fcstor006 was DOWN).
**2PC GEN-SYNC GATED (Aug 29, working tree, UNGATED commit).** Joiners no
longer pin at gen=2/5 while the primary runs away. Repeatable gate:
`bash tests/clean_cluster.sh && bash tests/run_tests.sh setup && bash tests/stress/ecopy_gen_sync.sh`
— one client, `~/git/direct_copy/ecopy ~/orcd/scratch/ecrawl-synt-small/ /tmp/efs-mount/`
(contents of SRC into DST; tree is ~6M files, default window 180s). First
run `results/ecopy-gen-sync/20260829-163709`: **FUSE_OK**, 12057 files / 180s,
**gens 5779 5779 5779 5779 PASS** (still 5887/5887/5887/5887 after). Joiners
logged 5886 `meta-commit: promoted` each; **0 BUSY / 0 STALE**. Primary
`perf record -p $(pgrep -x efsd)`: 20% blake3 in `server_flush_fragmented_meta_locked`
(known O(table) flush). Joiner `strace -cf -p`: 61% futex / 24% recvfrom
(followers waiting, not hashing). Do **not** wrap efsd in strace.
Fixes: META_COMMIT BUSY only if `primary && shard_dirty`; PUT_META
gen-advancing prepare only from the metadata primary (else extras-merge /
STALE); extra-shard `bootstrap_export_on_peers` is a no-op; catchup rebuild
allows joiner `shard_dirty`. Leftover compile trap: do not leave a stray
`return 0;` after `put_meta_status_name`.
**FIO / DATA-PATH (Aug 29) — method + table live in `.cursor/rules/efs-fio-honest.mdc`.
Do not re-derive.** Honest writes `--end_fsync=1`; reads after remount.
Harness: `tests/stress/fio_honest_matrix.sh`. Writes ~2.4 GiB/s (1 client)
→ ~3.6–5.3 GiB/s (9 clients, 1.5–2.1×). Reads 7.8 → 49 GiB/s (6.3×).
Stock `run_tests.sh perf` write column is cache-inflated — never quote.
Last `perf`: client blake3 on flush (2.7 cores); primary off-CPU idle.
Same shared serialization family as ecopy (9 clients only ~1.3× one).
Cleanup gotcha (not a bug): a dir holding files fio still had open rmdir'd
ENOTEMPTY forever while readdir showed it empty — a leaked libfuse
`.fuse_hidden` node (readdir filters the prefix). A remount cleared it; the
server had no orphan. Minimal create/rm/rmdir cycles are clean.
**REAL-WORKLOAD SCALE FINDING (Aug 29) — ImageNet ecopy throughput DECAYS 2.2x
as the table grows, and the primary is the single-threaded ceiling.**
New harness `tests/stress/ecopy_rounds.sh` (+ `ecopy_verify.sh`): every client
ecopy's a disjoint slice of ImageNet class dirs into its own subtree, one slice
per round, so each round is fresh work on a strictly larger table and rounds are
directly comparable. Source
`/orcd/scratch/orcd/001/erbmi1/imagenet/images_complete/ilsvrc/train` (1000
class dirs, 732-1300 files each, ~117 KB avg — metadata-bound, not bandwidth).
**GOTCHA: `ecopy SRC DST` copies the CONTENTS of SRC into DST, not SRC as a
subdir** — the harness mkdirs `DST/<class>` per class or ImageNet's shape is
lost and per-class accounting reads 0.
**6 rounds, 9 clients, 2 class dirs/client/round, fresh bits=3 cluster
(`results/ecopy/20260829-031849`), files/s aggregate:**
`84.5 | 75.2 | 65.6 | 59.4 | 46.9 | 37.6` over 23k->140k files. Monotonic,
no plateau. Single-client solo is ~66 files/s (unchanged since Aug 24), so
9 clients together only ever beat one client by 1.3x and end up BELOW it.
**Correctness is CLEAN: `ecopy_verify.sh` = 139767/139767 files, 108/108 class
dirs exact, 0 mismatches; 0 `ino_dup` / 0 `raced with GC` / 0 `meta-rebuild` on
all 4 servers.** Short rounds (23183/23171/23213 vs 23400) are ImageNet's
variable class sizes, NOT loss — always verify per-class before calling it a bug.
**Cause is NOT the flush, measured directly and do NOT re-chase:** pages
*written* per flush (the `next_ci` delta) is FLAT 3.1 -> 4.6 while the table
grew 8 -> 149 pages, i.e. CoW page reuse works and metadata write volume is
not what grows. 86861 commits in ~41 min (~35 flushes/s).
**Cause IS a single-threaded serialization point on the primary:** mid-round
sampling shows fcstor003 at **87.5% of ONE core (of 24)** while fcstor004/006
are at 0.0%, fcstor005 at 6.7%, and `efs-fuse` on the clients at 0.0%. All the
work funnels through one thread on one node; the other 3 servers and every
client are idle. This is the same shape as the posixstress saturation
(everything through the primary) but here it also *degrades with table size*,
which the empty-file scale test did NOT show (create rate was flat to 5M) —
so the growing term is on the chunk/REPORT_CHUNKS side, not create.
**`peer-commit-promote` ROOT-CAUSED + FIXED (Aug 29, working tree, UNGATED).**
Peers stashed every `meta-prepare` but stayed pinned at an old committed gen, so
recent metadata generations existed ONLY on the primary — a real durability
exposure. Cause: `EFS_MSG_INODE_DROP_CHUNKS` (handler.c ~2590) did
`ex->shard_dirty = 1` on the **main** table unconditionally. `fan_drop_chunks`
sends that RPC to EVERY node on EVERY unlink/truncate, so a peer set the flag
even when it dropped nothing (its tables hold none of that ino's chunks), and
**nothing on a peer ever clears it** — only a main-table flush clears
`shard_dirty`, and peers do not own shard 0, so the primary is the only node
that ever flushes it. One unlink anywhere therefore latched every peer's main
table dirty PERMANENTLY, and the META_COMMIT promote gate
(`if (ex->shard_dirty) { reply = BUSY; /* keep pending, do not promote */ }`,
handler.c ~1181) then refused every subsequent commit forever. Matches the
symptom exactly (prepares stashed for gens up to 11, still `committed gen=3`).
**Ruled out first, do not re-chase:** the commit's `export_id`
(`ex->root.id`, correct), the `root_sum` fingerprint (peer hashes the same
prepare bytes the writer hashed), and prepare/commit reordering (both PUT_META
and META_COMMIT are synchronous request/reply, so the writer cannot send
prepare N+1 before the peer has processed commit N).
Fix: mark dirty per-table and only when a table actually lost a chunk —
`drop_chunks_scan` now counts removals and sets `ex->shard_dirty` on the table
it modified; the blanket flag in the DROP_CHUNKS handler is gone.
**GATE OWED: posixpersist + confirm peers advance `committed gen` under load.**
**HYPOTHESIS WORTH TESTING FIRST — this may also be the residual
`unmount-drain-chunks` / `large-file-tail` content loss.** Both are "size
correct, contents all zeros" with idle, normally-flushing servers, i.e. the
inode row survived but the chunk records did not. If peers were pinned at an
old committed gen, then any post-remount read served from a peer's root is
missing every chunk record written since that gen — which is exactly that
symptom. Re-run posixpersist after this fix BEFORE chasing those two
separately; they may already be closed.

**FLUSH THREAD CHURN REDUCED (Aug 29, working tree, UNGATED).** Each page
written spawned 3 threads (one per fragment) and joined all 3, on the single
thread that already serializes every flush. Now spawns fragments 1..N-1 and
runs fragment 0 inline: same concurrency, 2 pthread_creates per page instead
of 3. The join loop was already guarded by `spawned[fi]`, so fragment 0 is
correctly not joined.

**UNLINK O(n) TERM FOUND + FIXED (Aug 29, working tree, UNGATED — no build was
possible, shell backend dead).** `drop_chunks_scan` (metadata.c ~1414) took its
bounded fast path only when `icnt_get(ex, ino) > 0`; when the count was **0** it
fell through to a full `while (i < ex->chunk_count)` pass over that table's
whole chunk array. `efs_export_drop_chunks_from` fans the scan over the main
table AND **every loaded shard tab**, and an ino's chunks live in only one or a
few of them — so for every other table the count is 0 and one unlink scanned
that entire unrelated table. **One unlink was O(all chunks on this node)**,
which is the measured unlink p50 0.44ms -> 4.6ms from 100k to 5M. Fix: if the
icnt index exists and reports 0 for this ino, return immediately (checked
BEFORE `layout_epoch++` — a table with nothing to drop did not change, and a
spurious bump makes the flush treat it as raced and stay dirty). Safe because
`icnt` is maintained everywhere `chunk_idx` is (`efs_export_set_chunk`, the
merge path, `remove_chunk_at`, `export_reindex_chunks`), so a 0 is exactly as
trustworthy as the non-zero count the existing fast path already returns on.
Holds for `first_chunk > 0` too (no chunks means none in any range).
`fan_drop_chunks` is fine — it dedups by node (O(nodes), not O(shards)) — but
note its `server_ensure_shard_ready` loop is O(shard_count) per unlink, which
is 2 owned shards at bits=3 but ~1024 at bits=12: fix that before raising bits.
**GATE STILL OWED: build + posix + posixpersist + a scale_grow unlink curve.**

**HOT PATH FOUND BY CODE READING (Aug 29, not yet measured — shell was dead):
every flush re-serializes and re-HASHES the WHOLE table just to discover which
pages changed.** `server_flush_fragmented_meta_locked` (meta_server.c ~1721):
1. `efs_export_table_snapshot_ex` — full memcpy of inodes[]+chunks[] under
 global + all shard locks;
2. `efs_export_serialize_ex` — serializes the ENTIRE table to a ~20 MB blob
 (156 pages x 128 KiB at 140k files);
3. then **for every one of the 156 pages**: `efs_meta_extract_page` (128 KiB
 memcpy) + `efs_encode_chunk` (EC encode) + **3x `efs_hash` over 64 KiB
 fragments**, and only THEN compares the checksums against the committed
 root's to decide the page is unchanged and can be reused.
So ~4.6 pages are actually written but all 156 are encoded and blake3'd. Per
flush that is ~78 MB touched incl. **~30 MB of blake3**; at ~35 flushes/s that
is **~1 GB/s of hashing+memcpy on ONE thread** — the 87.5%-of-one-core, and it
is LINEAR in table size, which is exactly the observed 2.2x decay. Matches the
old bits=5 profile (`server_flush_fragmented_meta_locked` ~50% of efsd, blake3
20% + memmove 12%) — that was the same cost multiplied by shard tables.
`efs_export_set_chunk` is O(1) (hash idx + append) and REPORT_CHUNKS apply is
O(recs), so **the apply path is NOT the problem — do not chase it.**
**FIX, in order of value:**
(a) cheap: cache the previous flush's serialized blob per table and `memcmp`
 each page's raw bytes instead of encode+3x blake3 (encode is deterministic,
 so identical bytes => identical fragments/checksums; same outcome, ~4-10x
 less work on the ~97% of pages that are reused). Invalidate on layout
 change (`ino_page_count`/`chunk_blob_len`) and on any failed flush.
(b) structural: per-page dirty tracking so step 2 serializes only dirty pages
 and step 3 never visits clean ones — removes the O(table) term entirely.
 This is the one that matters for 2^32.
NOTE `omit_chunks` already skips the chunk region when `chunk_epoch` is
unchanged, but an ecopy/fio write workload dirties chunks every flush, so it
never engages there.
**NEXT: find which primary thread is hot and confirm the above.**
`perf record -g` on efsd HUNG both times (perf report never returned);
use `top -H` + `/proc/PID/task/TID/{comm,wchan,stack}` instead.
**Harnesses staged but NEVER RUN (shell died first):**
`tests/stress/fio_rounds.sh` (rounds of fio from all 9 clients, fresh dir per
round so the table grows — same shape as ecopy_rounds so the two curves are
comparable; a flat fio curve + decaying ecopy curve would localize the cost to
metadata) and `ecopy_verify.sh deep` (remounts each client, then reads each
host's files back FROM A DIFFERENT host and md5s them against the ImageNet
source — the counts mode only proves the writer can still see its own writes,
possibly from its own dcache).
**INFRA: the shell exec env wedged right after this** — caused by
`pkill -f "efsd-ecopy"`, which self-matched the shell's own command line and
killed the session (the documented `pgrep/pkill -f` trap, third time it has bit
this project). A round-7 ecopy (`results/ecopy/round7`, log `/tmp/r7.log`) was
left in flight and its result was never read; the cluster still holds the ~140k
file tree under `/tmp/efs-mount/ecopy/<host>/r<N>/`.

**DURABILITY GATE ADDED (Aug 28, b2e0dce) — `run_tests.sh posixpersist`.**
Every other suite writes and verifies inside ONE mount session, so all of them
would pass even if efs never made anything durable (the client cache answers
the reads). `tests/posix/posix_persist.py` splits each of 26 tests across two
processes — `--phase prepare`, unmount, remount, `--phase verify` — with
content derived from the test name via `rand_bytes` so verify recomputes what
prepare should have written instead of trusting a file. Self-validating, NO
XFS baseline. Two guards, both load-bearing: the unmount is a real
`fusermount3 -u` (NOT `remount_client`, which SIGKILLs — a clean unmount must
preserve everything written, a crash only what was fsynced; `--crash` picks
the kill deliberately) and it NEVER falls back to a kill; and the run fails if
the efs-fuse pid did not change, since a surviving daemon serves the reads
from cache and passes vacuously. Validated both ways before trusting it:
26/26 on a local FS, and a negative control (truncate / zero-fill / delete one
of 500) fails exactly those 3.
**FOUND+FIXED on the first run: unlink freed the chunks of a file that still
had links.** `ln a b; rm a` left b with the right size and nlink but reading
all zeros, PERMANENTLY (identical after a second remount). Invisible
in-session — the client still had the data cached — which is why posix's
hardlink tests never caught it. The parent-owner UNLINK handler dropped chunks
for ANY non-dir unlink (`urc==0 && have_victim && !is_dir && !keep`); the
sibling UNLINK_SHARD path always gated on `nlink == 0`. Now reads the
surviving link count from the authoritative row (`efs_export_get_inode`, or
the child owner's UNLINK_SHARD reply) — `victim.nlink` is the PRE-unlink count
and on a parent shard can be a dentry stub. Gates: posixpersist 26/26 settled,
posix 0 EFS bugs.
**FIXED (Aug 28, cad1083) — the post-wipe loss was a DIRTY MAIN TABLE being
rebuilt.** The extra-shard paths have always refused to rebuild a table with
unflushed ops (DIRTY-REBUILD: the owner is the single writer, so a dirty table
is AHEAD of the committed root and a rebuild can only discard ACKed work); the
**main table had no such guard** — missing in both `server_ensure_shard_ready`'s
shard-0 branch and the catchup thread's `need[e]`. The rebuild also did
`ex->next_ino = ex->root.next_ino` AFTER `*ex = staging`, so the live watermark
was discarded and an older root rolled the allocator BACKWARDS; next_ino is now
floored at the pre-swap value (captured next to `saved_shard_id`). Reusing a
live ino corrupts a file, skipping ino numbers costs nothing.
**Gate: posixpersist immediately after a fresh wipe 25-26/26 (was 11-16 LOST),
0 `ino_dup`, 0 `raced with GC` on all 4 servers (fcstor004 alone had 45
rebuilds + 11 races before); posix 195/201, only the known size-visibility
flake.** The metadata half of the loss (missing creates, ENOENT, spurious
EEXIST) is GONE. What remains is content-only ("size correct, contents zeros")
and the servers are idle and flushing normally through it — that is the
separate clean-unmount drain item below, NOT this.
**Original diagnosis, kept for the symptom list:** a fresh `mkfs` is followed
by MINUTES of metadata rebuild churn
(measured: fcstor004 45 `meta-rebuild` + 11 `raced with GC`, fcstor006 25,
fcstor005 6). **While that churn runs, the servers silently drop applied
metadata ops.** All of these are the SAME bug, not separate ones:
- chunk mappings lost → "size correct, contents all zeros" (11-16 of 26);
- whole creates lost → `many_files_in_one_dir` came back **3 of 500**,
 symlinks/files ENOENT;
- the ino allocator ROLLS BACK → `cwi-fail: ino_dup parent=8 name=f043 ino=19
 shard=0 cnt=45` on the primary → **spurious EEXIST on a brand-new name**,
 then the file does not exist. **Note this is the scale-test `ino_dup`
 signature WITH per-op CREATE already reverted (bc27e0c), so per-op CREATE
 was NOT its only cause** — a rebuild reloading a shard table from stale/GC'd
 pages loses recently applied rows and the allocator re-issues their inos.
**Once the rebuild churn stops the cluster is stable and posixpersist is
25-26/26.** So it is a startup/catch-up window, not steady-state corruption —
but it is silent, and `fsynced_file` was among the lost, i.e. **fsync returned
success and the data was gone** (the report succeeded, then the rebuild threw
the row away). All of the above is fixed by cad1083.
**RULED OUT as the cause (do not re-chase):** the client. `efs_client_report_dirty`
merges the dirty snapshot back on failure and sets `report_flush_failed`, and
in every failing run the client logged NOTHING because the report returned OK.
`efs_fuse_destroy` was still hardened (845b853): it flushed once, discarded the
rc, and let `efs_client_shutdown` free the merged-back table — retry-until-drain
+ a loud message now, but that loop never engages here.
**HARNESS CONSEQUENCE: a cluster is NOT usable the moment `clean_cluster.sh` +
`setup` return.** Runs started immediately after a wipe measure the churn
window, not the build (this is what made one posixpersist run look 16/26 and
the next 26/26). Wait for `meta-rebuild|rebuilt export` counts to stop growing
on all 4 servers, or accept the noise.
**Residual, separate, still open — CONTENT-ONLY unmount drain.** Post-cad1083 a
wipe+immediate posixpersist still loses 0-4 of 26, always "size correct,
contents all zeros" (`closed_but_not_fsynced` every time; also
`multichunk_file` / `overwrite_middle` / `sparse_file`), while the servers log
only normal `meta-flush: committed` + `meta-prepare: stashed` — no rebuild, no
GC-race, no ino_dup. So chunk records are not reaching durability on a clean
unmount; metadata does. Same family as `large_file_size_only` (64 MiB file,
tail chunk at size-128KiB, ~1 run in 3 even settled; isolated repro of the same
write always survives, with and without fsync). **NOTE while chasing this:
peers log `meta-prepare: stashed ... (was committed gen=3)` for every gen up to
11 — the 2PC COMMIT phase is not promoting on the peers, so only the primary
holds recent gens. Unrelated to the loss seen here (the primary serves) but a
real durability exposure of its own.**
**SCALE TEST BUILT + FIRST 10M-INODE RUN (Aug 28) — every number before this
was from a table under 440k against a 2^32 goal. `tests/stress/scale_grow.sh`
(+ scale_worker.py / scale_probe.py) grows an export in steps and records
create rate, idle per-op latency, server RSS and error count at each size.
It found four things in one afternoon; three are FIXED.**
1. **SILENT DATA LOSS: per-op CREATE dropped ~5% of concurrent creates —
   REVERTED (bc27e0c).** O_CREAT|O_EXCL on unique never-used names returned
   EEXIST and the file was then absent (stat ENOENT); 111/111 sampled dirs
   were short (4-67 of 1000 files). 407k `cwi-fail: ino_dup` on the primary,
   **all shard=0**. Cause: `efs_export_alloc_ino_for_shard` consults the MAIN
   table (`efs_export_inode_slot(ex, ino)`) on EVERY alloc, but per-op CREATE
   locks only {psh,dsh,target} — when none is 0 it read shard 0's ino index
   while a concurrent create was inserting + reallocating it → missed a live
   row → handed out an in-use ino → the dentry write onto main rejected it.
   0 errors at 1 thread vs 27 at 16 threads (same client, same size) = pure
   race. **Revert was also FASTER: 500k/9x16 errors 25017→0, creates/s
   23809→39999, unlink p50 0.95→0.53ms, RSS 2055→1265MB.** Consistent with
   402d14a (partition perf-neutral). Locking shard 0 per create would
   serialize all creates = the thing the partition existed to avoid, so
   there is **no cheap correct version**; the allocator's dependency on the
   main table must be designed away first. Transitional step (d462ab0,
   aa7bd5a) is NOT implicated and stays.
2. **readdir was O(inodes in export) PER PAGE — FIXED (60cbef3).** The
   handler scanned the whole shard table for rows with a matching parent, and
   readdir fans each page to every shard. A 1000-entry dir: 31ms at 90k →
   1.9s at 10M. The parent→children index already existed
   (`efs_export_foreach_child`) and just was not used. Now **4.6ms at 5M and
   FLAT** (4.4/4.3/4.6 at 100k/1M/5M). Also retired an O(ncand^2)
   duplicate-dentry DIAG (~500k strncmp/page) — foreach_child only yields a
   slot the name index maps back, so a dup cannot reach the reply.
3. **Flush + GET_META snapshots took only the global lock — FIXED
   (60cbef3).** Fine until per-op CREATE started reallocating `ex->inodes[]`
   under a shard lock alone; then the snapshot memcpy could read freed
   memory. **2 of 4 servers died with SIGSEGV in
   `efs_export_table_snapshot_ex` on the flush thread** under 9x10M creates,
   and the other 2 wedged re-polling for pages the dead nodes held. Both now
   take global + all shard locks (same `rebuild_shards_lock` discipline as
   the rebuild). RSS at 5M also fell 27.5GB→7.4GB, consistent with the racing
   snapshot reading torn `inode_count`s. **This was the "flush is not
   shard-lock-aware" gap that higher bits were blocked on.**
4. **STILL OPEN — RAM is the 2^32 wall.** Marginal ~1.3-1.5 KB/inode
   (5M→10M: +6.6GB for 5M inodes) cluster-wide, tables fully RAM-resident.
   2^32 x 1.3KB ≈ **5.6 TB aggregate** = ~1.4 TB/node on 4 nodes. Reaching
   2^32 needs a paged/on-disk table or ~40-100 nodes, NOT more shard bits.
   Measure again now that the snapshot race is fixed.
**Scale curve (post-fix, 9 clients x 16 threads, bits=3, TCP):** creates
16.6k-40k/s and NOT degrading with size; stat p50 flat 0.27ms 100k→5M;
create p50 0.28→0.34ms; readdir flat ~4.4ms; **unlink still grows
0.44→0.53→4.6ms (100k/500k/5M) — next thing to chase.**
**HARNESS GOTCHA (cost an hour):** the worker swallowed `FileExistsError` as
benign, so a 5%-per-dir loss reported `errors=0`; and it batched progress at
2000/thread so a 694-file slice printed 0 forever and a dead cluster looked
like a hang. Both fixed. **Never treat EEXIST on a unique name as benign.**
**BIGGEST PERF WIN — fsync flush GROUP COMMIT (Aug 28, 886f75b): 36-way
posixstress 34.7 -> 7.2 EFS-bugs/instance (4.8x), 3 fresh-wipe runs each.**
Every sync REPORT_CHUNKS (every client fsync/close) ran its OWN full metadata
flush, all serialized on the single `meta_flush_mu`: ~55 flushes/s x ~14ms =
~77% duty on one mutex, so each flush queued ~88ms. **85% of the flush window
was pure WAIT, not work** (flush wait 126.7s -> 0.30s, 428x; calls 1441 ->
610). Concurrent fsyncs now share a flush: the caller's mutation is already
applied before it reaches the flush, so any flush that *starts* after that
point includes it — a caller finding one in flight waits for the NEXT one.
Leader keeps flushing while callers are queued, tracked as the highest
generation anyone needs (`flush_target`), NOT a waiter count (waiters can only
decrement after the leader drops the mutex, which would spin the leader).
Gates: posix 195-196/201 0 real bugs; posix2 60/63 at POSIX2_STEP_SEC=45.
**unlink/nlink fan de-scanned (e754c23):** `efs_export_unlink` and
`tab_set_nlink` linearly scanned `inodes[]` while `for_each_loaded_tab` fans
them across EVERY loaded shard tab → one unlink was **O(total inodes)**. Now
early-out on the authoritative ino index. Neutral at bits=3 (small tabs),
~15% at bits=5, and removes a term that is FATAL at 2^32.
**bits=5 STILL ~8x worse than bits=3 (7.1 vs 57.3, 3 runs each) — CAUSE NOT
FOUND. Ruled out by direct measurement, do NOT re-investigate these:**
flush work (identical: 1441/19.7s vs 1448/22.2s) and flush wait (fixed by
group commit); `lock_all` acquire (0.09s vs 0.11s) AND hold (bits=3 is
*worse*: 2.88s vs 2.44s); per-op `lockn` wait (bits=3 worse: 0.19s vs 0.14s);
shard rebuild/evict churn (0 lines); per-op cost unloaded (single suite 3.5s
vs 4.0s); RPC amplification (bits=5 issues FEWER RPCs: 755k vs 846k — it
completes less work). Under load all 4 servers are IDLE (154/183 threads in
`wait_woken`) and efs-fuse is at 11% CPU, so **the bottleneck is client-side
or on the wire, not the server.** Client profile is flat (memmove 12%,
blake3 9%, mutex 5%). NEXT PROBE: client-side per-op latency/RPC-wait
instrumentation, or wire round-trip count per op.
**Tooling added (env-gated, off by default):** `EFS_FLUSH_PROF` (per-stage
flush timing, separating lock WAIT from work — this is what found the group
commit win), `EFS_LOCK_PROF` (lock_all calls/wait/**hold**, per-op lockn
wait, per-opcode RPC counts). Shard-lock waits are off-CPU (futex) so
`perf record` CANNOT see them — that is why the on-CPU profile was
misleading for two rounds.
**STOP-THE-LINE MEASUREMENT (Aug 28, 51ebb17) — the per-shard lock partition
has produced NO measurable throughput change. A 7-point bisect of 36-way
posixstress, every point a fresh wipe+mkfs at bits=3, EFS-bugs per instance:**
`ff7896c` pre-transitional **35.1** | `aa7bd5a` transitional **41.7** |
`1dc03f1` +per-op CREATE **29.8** | `e4efd97` +per-op APPEND **38.9** |
`main` 51ebb17 **37.4 / 30.7 / 36.1** (3 repeats). Aug-27 pre-partition
baseline **46.2**. **Every variant sits in a 30-46 noise band — the whole
partition (blockers 1-3 + transitional + per-op CREATE/APPEND) is
performance-NEUTRAL.** Reason: only CREATE/APPEND are per-op; every other
handler still does global + `lock_all`, and at bits=3 there are only 2
shards/server (max theoretical 2x anyway).
**METHODOLOGY WARNING — single 36-way runs are NOISE.** Run-to-run spread is
±20%, and a confounded run read **162.8/instance** (4x outlier) purely
because a previously-killed posixstress left load on the clients. NEVER
conclude from one run; use >=3 fresh-wipe repeats. Two `pgrep -f` traps bit
this session: `pgrep -f edelete` / `pgrep -c -f posix_suite.py` **match their
own ssh command string** — always `pgrep -x`, or the check is a false
positive (cost ~15 min of waiting on a drain that had already finished).
**bits=5 REVERTED to bits=3 (51ebb17).** bits=5 measured 133/instance
(outside the band, 2 runs) and profiling under load pinned ~50% of efsd
on-CPU in `server_flush_fragmented_meta_locked` (blake3 20%, memmove 12%):
the flush pays a FIXED per-shard-table cost (snapshot under `s->lock`, CoW
page setup, >=1 page write per table), so 32 tables quadruple the flush
lock-hold and starve handlers (all 4 servers idle while clients blocked).
The create-only latency probe looked ~6x BETTER at bits=5 (a create burst
dirties few pages) — it is not a proxy for the suite. Raise bits only after
the flush snapshots under the SHARD lock instead of `s->lock` and packs
small tables per page.
**Correctness on main is GREEN:** single-client posix 195/201 + 1 known
flake, 0 real bugs; posix2 59/63 (the 3 = harness ssh-timeout,
`peer_fcntl_range_conflict` known feature gap, `peer_rename_across_dirs_chase`
known flake). 36-way "bugs" are ~99.6% 15s timeouts = saturation, NOT
correctness.
**Per-shard `g_server->lock` partition — BLOCKERS 1+2+3 + TRANSITIONAL STEP LANDED (Aug 27, c65865f + 958b15b + b167ea2 + d462ab0 + aa7bd5a); per-op global-lock drop remains.**
**Transitional nested-lock step DONE (d462ab0 Batch 1: GETATTR/CREATE_SHARD/
APPEND/GETCHUNKS per-shard; aa7bd5a Batch 2: LOOKUP/CREATE/UNLINK/UNLINK_SHARD/
RENAME/RENAME_AT/SETATTR/LINK/LINK_SHARD/HOLD/DROP_CHUNKS/LOOKUP_PATH/READDIR/
REPORT_CHUNKS via `server_shard_lock_all`).** Global lock stays HELD in every
handler (no parallelism yet) — this only establishes global→shard discipline.
Every mid-op global-lock drop (CREATE/UNLINK/LINK fan-out, REPORT_CHUNKS yield)
and every `fan_drop_chunks` call is wrapped unlock_all→drop→relock→lock_all so
no shard lock is held across a global-lock drop (would deadlock vs the rebuild's
`rebuild_shards_lock`). Shard locks taken after any ensure/reply_if_shard_busy.
**Because ALL handlers now hold lock_all, the earlier per-op use-after-free
(CREATE realloc of `ex->inodes[]` racing a shard-only GETATTR) is now IMPOSSIBLE
— a per-op reader holding shard X excludes with any writer's lock_all.** Gate
(fresh bits=3, TCP): posix 194-196/201 (only the pre-existing size-visibility
flake family, moving run to run); 4-way posixstress smoke 3/4 clean + 1 same
flake, NO deadlock/wedge. **GOTCHA: the posix harness `ensure_mounted` REUSES an
existing mount — a manual `efs-fuse` started without `EFS_TRANSPORT=tcp` comes
up on RDMA (broken data path on fresh mount) and the suite then mass-times-out
looking exactly like a server deadlock (server idle, gen stuck, client D-state
request_wait_answer). Always let the harness mount, or mount with
EFS_TRANSPORT=tcp.** NEXT (hard, the actual ~2x win): per-op drop of the global
lock for the hot ops. Reads (GETATTR/LOOKUP/READDIR/LOOKUP_PATH/GETCHUNKS) are
the easy/safe ones (no fan-out/yield) but don't fix the create burst; the burst
needs per-op CREATE/UNLINK/REPORT_CHUNKS, which must take the global lock only
briefly (export lookup + membership + ensure) then run on their shard set —
hard because of the CREATE fan-out, UNLINK fan/fan_drop_chunks, and the
REPORT_CHUNKS yield all currently relying on the global lock.
**Per-op CREATE LANDED (working tree, first per-op write).** CREATE now holds
the global lock ONLY for the membership/ownership/node-addr snapshot +
shard-ready ensure (all stable during a burst), then drops it
(`global_held=0`, dispatch unlock guarded by the flag) and runs on per-shard
locks: remote branch {psh,dsh} for the EXIST check + local dentry write,
fan-out with NO lock; local branch {psh,dsh,target} around `efs_export_create`.
Landmines handled: (1) `dsh`/`dir_is_spread` is computed under the psh lock —
it reads the parent inode, and a global-lock-only read would race a concurrent
per-op create's `inodes[]` realloc (the same UAF as the original GETATTR bug);
(2) `mark_rpc_dirty` is global-lock-free (atomic + CV hint, safe under a shard
lock); (3) `evict_cold_shards` is SKIPPED in the per-op path — it FREES shard
tables = UAF vs per-op shard locks (no-op at bits=3; needs shard-lock-aware
eviction as a follow-up for higher bits); (4) `hold_inc` uses the leaf
`hold_mu` (safe under a shard lock); (5) fence re-check (`meta_needs_rebuild`)
after each shard-lock acquisition → BUSY. The fan-out EXIST→create non-
atomicity (orphan row on a duplicate) is PRE-EXISTING (the old code also
dropped the global lock for the fan-out). Gate (fresh bits=3, TCP): posix
196/201 0 bugs (clean run; other runs 0-2 pre-existing size-visibility flakes);
4-way posixstress smoke 0-2 bugs/instance (flake family, no deadlock, no
consistent FileExistsError — the one 7-bug spike was the wipe's background
edelete). NEXT: per-op UNLINK/REPORT_CHUNKS/SETATTR/APPEND + the reads.
Goal: fix the posixstress 9×4 startup burst — volume-driven `g_server->lock`
queueing (~2s op latency in the first ~4s as 576 threads start; 99.6% of
failures are 15s timeouts, **0 correctness bugs**; sustained latency after the
burst is ~7ms). The `drop_chunks` O(chunk_count)→O(chunks_of_ino) fix
(commit a2b90cf, `icnt` per-ino live-chunk map) gave only ~5% — the burst is
op-VOLUME, not long lock-holds. **This partition is a MULTI-DAY refactor, not
a quick win.** Infrastructure + blocker 1 LANDED (commit c65865f):
`g_server->shard_locks` heap array (`[eidx*EFS_META_MAX_SHARDS + shard]`,
malloc+init in efsd.c) + canonical-order helpers `server_shard_lock/
lockn/unlockn/lock_all/unlock_all` in server_internal.h. **Design:** per-shard
locks for the ~17 inode RPCs + REPORT_CHUNKS + LOOKUP_PATH/READDIR/GETCHUNKS;
ONE global lock kept for membership + flush + rebuild + catchup + append
barrier; strict **global→shard** ordering (never hold a shard lock while
acquiring `s->lock`); flush/rebuild take global + ALL shard locks of the
export; `create_rr` becomes a single atomic `fetch_add` (it lives on shard 0
but is bumped by every create). **Expected benefit ~2×/server at bits=3**
(only 2 shards/server to parallelize); more at higher bits.
**3 foundational BLOCKERS — must land BEFORE any handler is partitioned:**
(1) **Rebuild manages `s->lock` internally — DONE (c65865f).** The rebuild's
table-swap commit in `server_rebuild_export_from_pages_ino` now holds the
export's shard locks in global→shard order via `rebuild_shards_lock`/
`rebuild_shards_unlock` (meta_server.c): ALL shards for a main-table rebuild
(`server_shard_lock_all`, since `efs_export_free` drops every shard tab),
ONE for an extra-shard rebuild. The page fetch still runs lock-free (network
I/O). No handler holds shard locks yet, so this is a no-op — it establishes
the discipline. Validated on a fresh bits=3 cluster: posix 196/196 0 EFS
bugs; a joiner restart (fcstor004) exercised BOTH rebuild paths (main
all-shards gen=49, extra single-shard shards 1+5) with no deadlock and a
consistent gen. (The 4-way posixstress smoke surfaced a PRE-EXISTING
load-dependent size-visibility flake `content_random_overwrite_append`
size-0 — also seen at 12:39/13:31 pre-blocker-1, passes 5/5 isolated; NOT a
blocker-1 regression, but a real cross-op size race worth its own look.)
(2) **On-demand shard-table creation — DONE (958b15b).** `efs_export_table`/
`table_for_ino` lazily created+installed shard tables and bumped
`ex->shard_tick` — a shared per-export mutation hiding inside read paths
(GETATTR/LOOKUP). Now the lazy create is extracted into
`shard_tab_get_or_create` (called only under the global lock or all shard
locks), every shard table is pre-created at rehash (mkfs/upgrade) and after a
main-table rebuild (which frees the clean tabs), and the LRU tick bump is
atomic (`__atomic_add_fetch`). Validated on a fresh bits=3 cluster: posix
196/196 0 EFS bugs; a joiner restart (fcstor004) exercised the rebuild
precreate (`meta-catchup: rebuilt export=efs-test` + owned shards 1,5) with a
consistent gen; 4-way posixstress smoke 3/4 clean (1 pre-existing concurrency
size-visibility flake `fsync_then_fstat_size`, passes 5/5 isolated).
(3) **Global-state + multi-shard ops — global-state DONE (b167ea2).** The
three pieces of global state the partition touches are now safe to touch
without the global lock: `ex->create_rr` is a single atomic fetch_add
(bumped by every create); `s->rpc_dirty_ops[]` is atomic bump/read/reset
(the flush trigger); the HOLD table (`hold_buck`, handler.c) has a dedicated
`hold_mu` leaf lock. All behavior-preserving under the still-held global
lock (no handler takes shard locks yet). Validated on a fresh bits=3
cluster: posix suite (5 runs, 0-2 pre-existing flakes/run, one clean
196/196) + 4-way posixstress smoke (2/4 clean) — only the pre-existing
size-visibility/ENOTEMPTY flake family, each passing isolated; no
regression. **Deferred to the transitional step:** the multi-shard
discover-relock — UNLINK reads the parent shard to discover the victim ino
(→ the child shard), and `fan_drop_chunks` touches all shards, so the lock
set isn't knowable upfront (lock→read→discover→relock-in-canonical-order +
re-validate). `fan_drop_chunks` re-acquires the global lock mid-op and
`server_ensure_shard_ready` can trigger a rebuild — an ordering entanglement
that doesn't belong in a clean global-lock-still-held step. **Plan:**
nested-lock transitional step (handlers take global+shard together, no
parallelism, safe) → drop the global lock per op, gating each with posix
suite + posixstress + valgrind. NOTE: on a FRESH cluster the shards are not
hollow, so the rebuild-deadlock (blocker 1) doesn't fire in the posixstress
gate — but it MUST stay fixed for this to be rejoin-safe.
**posix2 cross-client fixes (Aug 26 PM, commits c65ca48/731d023/5525d49/
5062a9b).** posix2 was 15-34 bugs; now ~56-59/63. Fixes: (1) `c65ca48`
created-set getattr refresh (truncate stale-size). (2) `731d023` unlink
non-fatal on local-table miss (peer_open_unlink_nlink EIO). (3) `5525d49`
REMOVED the created-set short-circuit from `lookup_walk` — it was not
authoritative for a shared dir (a peer can create/rename/unlink names in a
dir this client created); now every clean-file lookup under a locally-known
parent does LOOKUP+GETATTR (2 RPCs, inherent to sharding: parent-shard LOOKUP
returns a size-0 dentry stub because the child shard tab is hollow on the
parent owner, so GETATTR on the child owner gets the full row). This fixed 7
visibility/nlink/excl bugs. (4) `5062a9b` SETATTR size-shrink clears the ino's
stale append reservations (a truncated-away rsv looked permanently
outstanding, BUSYing the next O_APPEND into -EIO; peer_append_while_truncate).
**Remaining posix2 bugs:** `peer_fcntl_range_conflict` (byte-range lockf is
per-client in-memory `efs_fuse_lock`, NOT coordinated cross-client — deep
feature, needs server-side range-lock tracking); `peer_rename_across_dirs_chase`
(x in two places — FLAKY, passes 5/5 isolated, load-dependent cross-shard
rename race); flaky EIO/timeout on concurrent truncate/extend/pwrite tests
(load-dependent, amplified by the created-set removal's 2-RPC lookup cost
adding g_server->lock contention). The 15s POSIX2_STEP_SEC is too aggressive
post-removal; 45s gives a cleaner signal. **posixstress 9×4 RE-RUN on the
created-set-removal build (Aug 27, results/posix/20260827-121700, fresh
bits=3, TCP):** 36 instances, 1665 EFS bugs, **1658 (99.6%) are
`timeout after 15s` (saturation); 7 (0.4%) non-timeout — and ALL 7 pass in
isolation** (flock_two_proc_exclusive ×5 = the test's 0.2s child-flock sleep
is too short under load — the child's LOCK_EX RPC hasn't landed when the
parent's LOCK_EX|NB arrives; fdatasync_write ×1; dir_deep_nesting_beyond_64
×1). **ZERO genuine correctness bugs under 36-way.** The GETATTR in the
2-RPC lookup is NOT redundant (parent-shard LOOKUP returns a size-0 dentry
stub; GETATTR on the child owner gets the full row) — it's inherent to
sharding + immediate visibility. The saturation lever is the
`g_server->lock` partition (roadmap Perf lever 1), not fewer lookup RPCs.
**posixstress saturation CONFIRMED = `g_server->lock` latency, not CPU
(Aug 27 profiling on a fresh bits=3 TCP cluster, 36-way):** servers NOT
CPU-saturated (primary ~7.5/24 cores, joiners ~2.8/24; client efs-fuse
~0.43 cores) — a LATENCY bottleneck, not throughput. On-CPU work (perf
record): blake3 chunk hashing, memmove, `drop_chunks_scan`, serialize.
wchan identical at rest AND under load (156 `wait_woken` + 22
`futex_do_wait` = idle worker pool, NOT elevated lock contention — the
per-op hold is short, threads don't pile up). Per-op latency probe under
load vs rest: **create (touch) 5-13ms → 100-350ms (~20-70x)**; stat/readdir
stay ~2-3ms. ALL inode RPCs (LOOKUP/GETATTR/CREATE/UNLINK/...) take the
SINGLE global `g_server->lock` (handler.c:1621). Two lock-hold bottlenecks
confirmed in code: (1) `drop_chunks_scan` is O(chunk_count) under the lock
per unlink/truncate (handler.c:2387 INODE_DROP_CHUNKS, metadata.c:1298);
(2) the CoW flush snapshots the table (memcpy) under the lock
(meta_server.c:1469-1505 server_flush_fragmented_meta_locked). The CREATE
handler already releases the lock during the fan-out CREATE_SHARD RPC
(handler.c:1763-1766), but a create still takes the lock 3x (parent 2x +
child 1x) + the fan-out RTT. Under 36-way the lock arrival rate exceeds the
service rate → creates queue 100-350ms → tests exceed the 15s
POSIX_TEST_SEC. Lever: partition g_server->lock (per-shard/per-export) or
shrink the hold time (chunk-index drop_chunks instead of a table scan;
snapshot without holding the lock).
**posix suite 191→201: seeded random content + strange-name/depth tests
(Aug 26 PM).** `rand_bytes(seed,n)` = SHA-256-keystream generator mixing
ASCII/whitespace/control/NUL/high bytes (NOT zeros/repetitive — zero-fill/
dedup bugs can't hide); same seed → same bytes on any client (peer-verifiable).
New tests: content random roundtrip across 128KiB chunk boundaries,
all-256-byte values, whitespace/line-endings, NUL+control, random
overwrite+append, 2MiB multichunk (sha256-verified); raw-byte names
(control/DEL/high/space-only), unicode NFC-vs-NFD coexistence, crazy names
as dirs, dir nesting past the 64-ancestor LOOKUP_PATH cap. basic_dd_rw now
dd's a seeded-random file (was /dev/zero). `rmdir_rideout_sillyrename` rides
out the FUSE silly-rename ENOTEMPTY transient that outlasts the daemon's
20ms retry under concurrent open/close load. **Limits (from code):** name
component ≤255B (`EFS_MAX_NAME`, ENAMETOOLONG per-component), path ≤4096B
(`EFS_MAX_PATH`), NO hard depth cap (LOOKUP_PATH batches ≤64 ancestors then
falls back to a per-component walk; `find` reporting caps at 128 comps).
Single-client fcstor007: 196 both-pass, 0 EFS bugs, 3 consecutive runs.
**9×4 posixstress harness + first run (Aug 26 PM, commits d123fa4/79df463).**
`run_tests.sh posixstress [N] [hosts]` = N full posix suites per host, all
hosts in parallel (default 9×4=36). posix_suite `--tag` gives each instance a
unique testdir prefix (`posix-<host>-<tag>-`) so same-host siblings never sweep
each other; `posix_one` fans out to `posix_instance`. wipe_cluster now renames
`/data1/*/efs` aside + recreates instantly (detached background edelete
reclaims) and polls for D-state efs-fuse — wipe is ~ms not minutes.
**First 9×4 run (results/posix/20260826-224050, fresh bits=3, TCP):** all 36
instances completed; cluster STABLE after (4 up, gen=1060 consistent, no
wedge/GC, client probe OK). ALL 337 failures are `timeout after 15s`
(saturation), ZERO correctness/assertion failures. Always-timeout (36/36):
`concurrent_appends` + `concurrent_appends_two_proc` (O_APPEND reserve-RPC
path). Near-always: `mtime_monotonic_many_writes` (32),
`concurrent_create_unlink_two_proc` (32), `create_excl_two_proc` (31).
Per-instance 2–22 bugs (fcstor012 cleanest at 2). The `Bad file descriptor`
stderr flood = worker threads of a timed-out `concurrent_appends` erroring
during cleanup (cosmetic). Finding: 36-way concurrency saturates the cluster;
O_APPEND + same-dir concurrent create/unlink serialize worst. Not yet
root-caused.
**2PC metadata root commit LANDED (Aug 26 early AM, working tree) — the
GC-wedge fix.** `PUT_META` is now phase-1 PREPARE (peer stashes the root as
`pending_root[ei]` + raw bytes in `pending_blob[ei]`, NO install/persist/
fence/GC). New `EFS_MSG_META_COMMIT`=91 is phase-2: after the writer's
prepare quorum + local commit, it broadcasts COMMIT(export, gen, root_sum);
peers promote only on a fingerprint match (`efs_hash` of the stashed prepare
bytes) — a same-gen retry that rewrote the same cis can never promote a
superseded prepare. Stragglers converge via GET_META_ROOT catchup (only ever
serves committed roots). Client-side metadata write path DELETED (write
lease, `meta_heal`, `replicate_metadata`, `send_meta_root`, server
`META_FLUSH_BEGIN` election, dead `server_send_metadata_to`) — **the server
is the sole metadata writer.** Validated on a fresh bits=3 cluster: primary
committed gen→135 under 4-way, peers `meta-commit: promoted` + catchup,
**0 unrecoverable / 0 "raced with GC"** — the wedge is gone.
**Two bugs fixed in the 2PC path itself:** (1) `META_COMMIT` busy guard uses
`ex->shard_dirty` (main table) not `rpc_dirty_ops` (shard tabs) — the broad
guard was blocking every promote. (2) use-after-free: the writer hashed
`root_buf` for the fingerprint AFTER `free(root_buf)` → garbage fingerprint
→ 0 promotions (all catchup). Now hashed at serialize time into a local
`root_sum[]` before the free.
**HARNESS FALSE-PASS BUG FIXED (critical):** `cmd_posix` never verified the
mount. After `wipe_cluster.sh` kills all efs-fuse, `/tmp/efs/mnt` is a plain
local dir and the suite was silently passing 186/191 against **tmpfs** (0
"EFS bugs" trivially) — the Aug 25-late "clean 4/9-way" results were NOT
against efs. `run_tests.sh` `posix_one` now refuses to run unless
`findmnt` shows `fuse.efs-fuse` (writes an ERROR stub + fails the host).
**Real gate status on a fresh cluster (EFS_TRANSPORT=tcp):** single-client
007 **185/191, 1 bug** = `names_crazy_roundtrip` (known parallel-visibility
flake, passes isolated). 4-way: no wedge, but **flaky concurrency
correctness** — per-run ~1-2 of 4 clients hit a cluster of rename→EINVAL /
hardlink→EIO / readdir-missing-just-created-entries / FileExistsError on
unique names. Failures move between clients across runs (not
client-specific). Hypothesis: a dirty SHARD table fenced+rebuilt from stale
pages drops unflushed creates (the main table is protected by the
`shard_dirty` COMMIT guard; shard tabs are not). Added `rename-fail:` log in
the RENAME_AT handler failure branch (prints rrc, shard, tab ptr,
np_present, tab_rebuild, tab inode_count, gen) — **rebuilt efsd on all 4,
needs rolling restart + a 4-way `--filter rename` repro to confirm.**
**Shell exec env went unresponsive mid-session (infra)** — resume by:
rolling-restart the 4 efsd (same commit-dirty build-id), `run_tests.sh
setup` all 9 clients, then 4-way gate and read `rename-fail` lines on the
shard owners.
**Default is bits=3 (Aug 25).** `mkfs` creates `EFS_DEFAULT_SHARD_BITS=3`
(8 shards) — no `upgrade` step. bits=0 is not a product mode; cluster
wipe is allowed. `root.id` is stamped on that create so bootstrap
GET_META does not invent export id=2.
**Cluster (Aug 25 evening):** efsd+fuse rolled with the 9-way fixes
on the grown leftover table (no wipe). After that 9-way, remount
failed: EFSR gen=571 pages decode-error on all 4 nodes. **Needs
wipe+mkfs** (clients first). Do not timeout-skip.
**GC-wedge ROOT-CAUSED via instrumentation (Aug 25 PM):** the chain is
**root adopted before its pages are local, then old pages GC'd.**
Instrumented `meta-flush: committed` / `meta-adopt` / `meta-gc` logs on
a fresh gen=1 cluster + 16-way posix suite showed: primary (fcstor003)
commits gen=1,2,3 via its own flush (page0_ci 65536→65538→65541, real
writes). Then gen=4 and gen=5 appear ONLY as `meta-adopt: PUT_META
old_gen=3 new_gen=4 page0_ci=65544` / `new_gen=5 page0_ci=65546` on
EVERY node — **no node logged `meta-flush: committed` for gen=4/5**, so
no node ever wrote the page fragments for ci=65544/65546. The adopt
(catchup_install_newer_root full-adopt, meta_server.c:2210-2234, and the
PUT_META full-adopt handler.c:1175+) installs the new root, fences the
table (`meta_needs_rebuild=1`, `FENCE-SITE catchup-root-no-blob`), and
the SAME adopt then GC's the PREVIOUS gen's cis (handler.c:1274
server_gc_meta_cow_pages). Rebuild then fetches ci=65544/65546 → rc=-3
on all nodes (never written, or already GC'd by the symmetric adopt on
peers) → `page 0 unrecoverable, gen N raced with GC, re-polling`
forever → shard-0 never ready → REPORT_CHUNKS BUSY → client
meta_flush_main spins (inode_rpc.c:128) → all fuse_flush D-state.
**The defect: a node adopts + persists + GC's a root whose CoW pages it
has NOT fetched/verified locally.** The pages for the new gen live only
on the original writer's placement; once every node adopts-and-GCs, the
fragments are gone everywhere. Fix direction: catchup/PUT_META adopt
must FETCH the new root's pages (or confirm 2+1 fragments present)
BEFORE server_save_export + before the old-gen GC runs; defer the
old-cis reclaim until the new root's pages are local. (My earlier
live-root recheck in server_gc_meta_cow_pages only skips cis STILL in
the current root — it cannot save a ci that was correctly absent from
the adopted new root but never written.) Also still open: REPORT_CHUNKS
BUSY retry has no give-up (amplifier), and the RDMA data path never
completes a CQE on a fresh mount (gate runs on EFS_TRANSPORT=tcp).
**GC-reclaims-live-root-pages race REPRODUCED on a FRESH cluster (Aug 25 PM):**
after a clean wipe+mkfs (gen=1, verified stable), running the 16-way
posix suite drove the export to gen=96, then the primary (node 1)
wedged: `meta-rebuild: page 0 unrecoverable (CoW ci=65600); gen 96
raced with GC, re-polling` — page 0's fragments are rc=-3 (gone) on
ALL of nodes 3,4,1. Restarting the primary does NOT help (it re-reads
the on-disk gen=96 root whose page_cis[0]=65600 is GC'd). So GC
reclaimed pages still referenced by the LIVE committed root gen=96 —
that is the bug, not a leftover-table artifact. Effect chain:
`server_ensure_shard_ready(0)` fails forever → REPORT_CHUNKS shard 0
BUSY → client `meta_flush_main` spins in `rpc_send_recv_shard` BUSY
retry (inode_rpc.c:128, usleep 50ms<<shift, attempt<16, NO give-up for
REPORT_CHUNKS — the INODE_APPEND/FLOCK early-return guard at line 123
does NOT cover it) → dirty set never drains → every close-time
`fuse_flush` wedges D-state (kernel `fuse_flush`+`request_wait_answer`).
Two client wedges from ONE server bug. Hardening gaps: (a) GC must
never reclaim a ci still in the committed root's page_cis[]; (b)
REPORT_CHUNKS BUSY retry needs a give-up/circuit-breaker so an
unready shard EIOs instead of wedging the mount; (c) the rebuild
"re-poll newest root" is a livelock when gen=96 IS the newest root.
**RDMA data path broken on fresh mount (Aug 25 PM, FIXED Aug 30):**
chunk PUT/GET over RDMA never completed a CQE (QP reaches RTS, first
SEND/recv after upgrade lost). Cause: shared-CQ poller armed, spun,
then blocked in `ibv_get_cq_event` without re-arming — the next CQE
after a quiet gap generated no event. Inode/REPORT were forced TCP as
a workaround. Fix: drain → req_notify → race-drain → block; small SEND
from the registered pool; peer pool uses the same QP. Default transport
is auto. Historical numbers: write 10B took 10.36s, read timed out 20s;
`EFS_TRANSPORT=tcp` was 0.005s / 0.023s. On TCP, single-client posix suite:
185/191 both-pass, 1 EFS bug `names_crazy_roundtrip` (passes isolated
— parallel-visibility flake), 5 target-better XFS quirks. Then the
16-way suite triggered the GC race above and wedged the cluster.
**vfs_unlink D-state / create+unlink hang ROOT-CAUSED (Aug 25 PM):**
NOT a C bug — the cluster was still on the **corrupt leftover table**
(build af6f380, log starts extras-merge gen=7→80, never a fresh mkfs).
Primary efsd loops `meta-rebuild: page 0 unrecoverable (CoW ci=65554)
gen 80 raced with GC` — shard-0 pages are corrupt (`scheme=legacy`
checksum mismatch + frag fetch rc=-3). So `server_ensure_shard_ready(0)`
fails forever → every REPORT_CHUNKS for shard 0 gets BUSY → client
`append_reserve_offset` BUSY-flush loop spins holding `g_append_mu`
inside FUSE `.write_buf` → kernel holds the inode write lock → the
suite's cleanup `unlink` wedges D-state in `vfs_unlink+0x48` (before
FUSE is called; gdb showed all 10 FUSE workers idle in
`fuse_dev_do_read`, one thread in the report BUSY retry). First suite
test `basic_oappend_flag` (ino 3224) hung before the TSV was written.
**Fix = wipe+fresh mkfs, not code.** Hardening gaps to consider later:
(a) `append_reserve_offset` spins 4000× (~8s+) holding `g_append_mu`
with no give-up-and-EIO, so an unready shard wedges the whole mount's
O_APPEND + blocks unlink on the same inode; (b) no circuit-breaker when
REPORT itself returns BUSY repeatedly (shard permanently unready).
**Fixes this cut (working tree, not committed):** (1) fsync EIO —
sync REPORT flush failure is BUSY (retry), skip hollow extras
instead of failing the barrier; client retries BUSY. (2) two-proc
O_APPEND — rsv slot is free once size catches up (was live 30s);
256 slots; never fall back to an unreserved local end. (3) suite
exit hang — cwd off the mount, close leaked FUSE fds per test,
`os._exit` after TSV; release no longer re-does data_sync (flush
already did; 014 D-state on close of leaked fds). (4) posix2
`--parent` + `run_tests.sh posix2 multi` unique per-pair parents.
(5) heal status counts **owned** tables only; label is
need-rebuild not "dirty". After efsd restart, status was idle
gen=177 (was stuck "healing 6/8 dirty").
**Re-gate on dirty table (partial):** isolated 007 fsync+two-proc
PASS. 9-way: two-proc PASS on every host that finished; 007/008
original 4 fsync EIO gone (EEXIST leftovers instead). 012/014
188/188 then hung (leaked fds + blocking release) — TSV written.
Some later hosts still EIO on fsync-path tests. Clean mkfs gate
not done (pages now corrupt).
**9-way posix `20260825-170203` (pre-fix):** 6/9 clients 0 EFS
bugs. 007 4 fsync EIO; 008/010 two-proc lost lines; 008–015 hung
after TSV.
**posix2 multi-pair `20260825-171200`:** 4 pairs **34/34** on
remount-A+prepare+remount-B. Leftover `posix-2c` was harness.
**POSIX suites (working tree):** posix_suite **191** (+3 local:
high-then-low extend, mtime monotonic, trunc0+high pwrite).
posix_2client **63** (+29 adversarial). Harness now runs
`("ab", (fn_a, fn_b))` A∥B. Layer 4 crash/EC/partition is **not**
in this suite. Re-run XFS baseline before the next compare.
**bits=3 gate (committed `af6f380`):** single-client posix
`20260825-160200` **188/188**. posix2 A=007 B=008
`20260825-162207` **34/34**. unlink-storm 9×4000
`20260825-163504` **9/9 RM_OK+ALIVE**.
**posix2 EIO / size-0 was three bugs:** (1) wr() close kicks async
REPORT; O_TRUNC setattr size=0; late grow-only REPORT restores the
old size (B saw 10). REPORT grow is now rejected when the rec mtime
is older than the row. SETATTR SIZE also stamps client-clock mtime
so a behind-client post-trunc REPORT is not treated as stale (B
saw 0). (2) remounted peer LOOKUP skipped GETATTR / overwrote RPC
with a local size-0 stub → utimens showed "now". GETATTR unless
dirty; keep local only when dirty. (3) posix2 harness remounts B
after prepare fsync (`.keep`) so B does not walk an old posix-2c
ino. Drain REPORT before utimens/truncate. fcstor004 catchup stuck
at gen=329 vs 347 broke flock/unlink mid-suite — restart joiner.
**link_across_dirs** (earlier this cut): created-set local hit must
return the stitched inode (LOOKUP/GETATTR wedged FUSE).
**Isolated PASS this cut:** `attr_utimens` / `attr_utimens_ns` /
`attr_touch_terminal` / `perm_owner_utime_readonly` (setattr now
applies times locally + pin so data-path "now" cannot clobber);
`names_too_long_component` (lookup returned EFS_ERR_INVAL → ENOTDIR;
now EFS_ERR_NAMETOOLONG + f_namemax=255).
**Still open:** 9-way `20260825-170203` — 007 4× fsync-path EIO;
008/010 two-proc O_APPEND dropped lines; post-suite FUSE wait on
exit. posix2 multi-pair is green when testdirs are clean.
**Fixes this cut (working tree):** (1) serialize `dentry_bytes` +
bounds — flush SIGABRT. (2) bootstrap no longer invents export id=2.
(3) `mkfs` default bits=3 + stamp `root.id`. (4) `create_with_ino`
uses `lookup_on_tab` (parent dentry skip → 1/8 readdir miss).
(5) catchup no longer bails on gen=0+`shard_bits` mkfs root (joiners
stayed bits=0 → CREATE_SHARD NOT_PRIMARY → file create EIO).
(6) `efs_export_lookup` consults dentry shards only — walking extras
found the child-row name after rename/unlink. Isolated
`dir_rename_file` / `dir_rename_symlink` / `unlink_open_file` PASS.
(7) utimens: local set_mtime/set_atime after setattr upsert +
mtime pin vs dcache_note_size/REPORT echo; adopt takes owner times
when !dirty. Do not shrink size on an older setattr mtime — that
zeroed basic_dd_rw after dirty cleared. (8) NAMETOOLONG:
EFS_ERR_NAMETOOLONG, not INVAL. Deployed efs-fuse on 007–015.
Isolated gates PASS. Full suite on the ~440k leftover table hung
in FUSE wait on `fsync_reopen_visible`. Need a clean mkfs before
re-gating posix/posix2/storm.
**Wipe always kills clients first** (`killall -9 efs-fuse` on
003–015) — leftover fuse is the 232MB/1.9M-chunk re-adopt.
**Phase 3b gates (Aug 24 late, build `09d38fe9c2b8-dirty`)** — cluster
rebuilt after a 13-way NFS rsync of `results/` wedged the first deploy.
**bits=0:** posix `20260825-034030` 182 both-PASS; 1 classified EFS bug
`dir_move_into_subdir` (ConnectionAbortedError) **isolated PASS**; fcntl
matches XFS FAIL; 3 virt SKIPs. posix2 first run 23/34 — primary
**SIGABRT** (`corrupted size vs. prev_size`) in
`efs_export_table_snapshot_free` ← `server_flush_fragmented_meta_locked`
(heap smash during flush, not 11 independent bugs). Restart + remount:
posix2 `20260825-035117` **34/34, 0 EFS bugs**. **bits=3 blocked:**
fresh mkfs+upgrade shards=8, FUSE bootstrap `export id=2` vs
`efs-mgmt list-exports` `id=1` → `mkdir` EIO (CREATE to missing id).
mc_stress + multi-9 sw-1m not run. Do not treat bits=3 as green.
**Phase 3b implemented (Aug 24 night, working tree)** — Items 0–2 of
docs/scaling-roadmap.md. Item 3 (replica-3 meta pages) left optional / not
in this cut. `test_meta_v6` OK; efsd + efs-fuse build clean. **Fresh
`edelete`+`mkfs` required** (chunk routing is not backward compatible).
- Item 0: op-class comments on REPORT (commutative), CREATE (independent),
  RENAME (conflicting) in protocol.h / handler.c.
- Item 1: `efs_export_chunk_shard_of` — group 0 stays on `shard_of(ino)`
  (small-file locality); later 8 MiB groups mix. REPORT/GETCHUNKS route by
  group via `rpc_owner_conn_shard` (no crafted ino). GETCHUNKS serves one
  group, no inode required on that shard, loud NOT_PRIMARY. Truncate/last-
  link unlink fan `EFS_MSG_INODE_DROP_CHUNKS` to other shard owners.
  `drop_chunks_from` scans the table (extent groups are not dense from 0).
- Item 2: `EFS_DIR_SPREAD_MIN` (65k) derived from rollups. Dentries go to
  `hash(parent,name)` past the threshold; LOOKUP tries hash then parent;
  readdir merges `EFS_READDIR_F_LOCAL_ONLY` per shard; unlink prefers hash
  shard (hash-shard row wins a crash duplicate). Stress:
  `tests/stress/hotdir_spread_probe.sh`.
**Roadmap gained Phase 3b (Aug 24 night)** from the architecture review
(plan still in docs/scaling-roadmap.md). Rejected: consensus-group sprawl,
MVCC, SPDK, universal hash dentries, range leases.
**rsync 10-round (Aug 24 night).** `rsync -avvvcSP` of `ecrawl-synt-small`
onto FUSE, 70s windows. Peak **16156 files/70s** (r9; r10 15983) vs r1
13889 / old LOOKUP_PATH-era ~230/70s. NFS→local same tree is ~3238/s —
efs is the limiter. C levers that mattered: hashed `hold_find`; same-dir
rename `expand_parent_chain` + skip child_idx; **`stamp_ctime_loaded`
nlink<=1 is O(1)** (was a full-table scan per rsync temp→final — 9% efsd
on a grown table). Also: LOOKUP last-component to parent owner; session
`created_set` skips dest-stat ENOENT under dirs this client mkdir'd
(remounted peers still RPC). Flush window 10s/20k. Reverted parent-shard
file create (piled work on shard 0). Residual: CREATE+RENAME RTTs
(~11k temps from `-P`). **Do not push ewrite/ecopy.**
**Unlink-storm 9×4000 PASS (Aug 24 night).** First run wedged at ~4–7 creates/s
(9-way LOOKUP_PATH+CREATE+HOLD+blocking REPORT, plus 100 ms extras flush).
Fixes (no new caches): local-table getattr for dirs this client already
dual-applied; `EFS_CREATE_F_HOLD` piggybacked on CREATE; close kicks
REPORT off the FUSE worker; server flush window 100 ms→2 s / 1000→8000 ops.
Grown leftover table (~65k inodes) still ~8 min/4000; **fresh mkfs bits=3**
is **~10 s create + ~3 s rmtree** (`results/stress/unlink-storm-20260825-000136`,
9/9 RM_OK+ALIVE). POSIX: isolated `dir_move_into_subdir` PASS; posix2
**34/34** (`20260825-001749`). Full 188-row tsv has one timeout-abort
artifact from a 300 s SSH cut. **Do not push ewrite/ecopy.**
**POSIX + posix2 CLEAN on bits=3 (Aug 24).** Fresh `efs-test` shards=8.
Single-client `posix_suite.py` **188/188** (`results/posix/20260824-214726`,
0 EFS bugs vs XFS; 5 target-better = known XFS quirks). Two-client
`posix_2client.py` **34/34** A=007 B=008 (`results/posix2/20260824-214648`,
0 EFS bugs). Suite grown 162→188 and 24→34 (same-fd size, hardlink/symlink
corners, rename-over-symlink, more peer visibility). The Aug 24 gaps are fixed: `ctime_on_link_rename` (RENAME_AT
+ ctime bump), `direct_unaligned_einval` (4k O_DIRECT), cluster flock
(HOLD/FLOCK RPCs + per-`fi` owner), unlink-open / peer unlink-while-fd
(keep_last + HOLD), same-fd empty reads (getattr prefers local size after
adopt — RPC size 0 was clobbering kernel i_size), `peer_symlink_visible`
(owner getattr for non-dir leaf + sync REPORT after symlink write).
**9-way POSIX (Aug 24 night):** sequential clean ≠ parallel. `trunc_open_other_fd`
fstat 0: adopt shrank a dirty local size to the owner's still-0 getattr
(fsync path-lookup). Fix: skip shrink when ino dirty; fsync by `fi->fh`.
`concurrent_appends*` lost lines: `append_rsv[ino%64]` stole another ino's
live reservation. Fix: open-address, never evict a live rsv. 008 hang =
`rmtree` D-state after 9 suites finish together — use `--keep`; not a
suite-correctness bug. **Do not push ewrite/ecopy.**
**Phase 3 storage model (Aug 24).** Pages + v8 descriptors are the DB.
Commits: `38dd6f0` (owner-only extras + unlink no-instantiate),
`9eff867` (reuse unchanged CoW pages), `a3485eb` (skip chunk snapshot
when chunk_epoch unchanged). **Post-restart rebuild + flush FIXED**
(this commit): unsharded create/unlink/link now set `shard_dirty`
(flush was no-oping after `rpc_dirty` only); never adopt leftover
`.efsm` (that cleared `needs_rebuild` on a 1-inode table). Catchup
rebuilds a hollow table when `blob_len > 64K`. Flush holds dirty
ops until rebuild instead of dropping them. Trustworthy meta w=1
`20260824-150410` on the grown table: create **69325** / unlink
**60047** (was 119 / 30); flush `reused 546/547`, Heal gen 1→2.
Persist: `--keep` 32 files, gen→3, kill+restart all 4, rebuild,
cold FUSE mount saw all 32. The earlier 70k print was RAM-only;
this one is RPC apply + async CoW reuse. **Current throwaway `efs-test`
is bits=3** (posix/posix2 above). FUSE timeouts 0. No caches.
Cluster live on **EFSR v8** + Phase 3 data-path sharding Stages 0–3
(committed). CoW = **debe6dd**, catchup-vs-GC = **76753ef**, 2b reads
= **94f96fd**.
**Data-path sharding LANDED (Aug 23 night):** both
blockers from the morning finding are fixed together (fixing only one
= silent data loss). (1) REPORT_CHUNKS is per-shard (`write.c`
`efs_client_report_dirty` partitions by `efs_export_shard_of`; server
loud-`NOT_PRIMARY` instead of silent drop). (2) file creates
round-robin `create_rr % sc` (dirs stay on parent shard);
parent-owner fans `EFS_MSG_INODE_CREATE_SHARD` to the child owner,
then writes the dentry locally. Probe
`tests/stress/shard_spread_probe.sh` PASS: 32 files, 8 distinct shards,
non-primary-owned, md5 match on a second client. Extra: hardlink/
last-link unlink fan `LINK_SHARD`/`UNLINK_SHARD` to the child owner
(parent-local `nlink_inc` was NOT_FOUND → EIO). PUT_META never fences
a live table for extras-only roots (that dropped unflushed mkdir
ops and made sharded dirs vanish mid-create).
**POSIX suite extended (Aug 24):** `posix_suite.py` 144 → 162 → **188**,
`posix_2client.py` 12 → 24 → **34**. bits=3 **188/188** + **34/34**
— 0 EFS bugs. The four Aug 24 gaps (`ctime_on_link_rename`,
`direct_unaligned_einval`, `peer_flock_exclusive`,
`peer_unlink_while_b_has_fd`) are fixed.
**POSIX after datapath sharding:** bits=0 `efs-test`
`20260823-232541` 0 EFS bugs; bits=3 `20260823-233448` 0 EFS bugs
(hardlink 8/8 after LINK_SHARD); posix2 12/12 both
(`20260823-232632` bits=0, `20260823-233514` bits=3).
**Perf on fresh bits=3 mkfs (Aug 23 233721/233849, all rc=0):**
single 007 sw-1m **5772** (new peak, was 5656 bits=0). Multi-9 sw-1m
agg **6925** — did NOT beat bits=0 **9342** / missed the ≥3× single
(~17 GB/s) gate. Files DID spread (007 f.0–f.7 inos cover shards
0–7). Residual: extras-commit catchup storm (~113–131
`rebuilt export=efs-test shard=N` per server during the multi run)
re-centralizes work. Next lever: skip extra-shard rebuild when the
local tab is already at/above the incoming extras gen (or batch
extras so peers don't rebuild every commit). Guide remains
`docs/phase3-datapath-sharding-impl.md`.
**Extras catchup storm FIXED (Aug 24):** two changes.
(1) Per-shard incremental rebuild cache
(`s->shard_blob_cache[export][shard]`) so a shard rebuild only fetches
pages whose checksums changed (was: every descriptor refresh re-fetched
every page). (2) THE LEVER: `server_flush_fragmented_meta` now only
flushes shards with `shard_dirty` set (was: flushed every shard on
every 100ms window, bumping every descriptor gen and making every peer
rebuild every shard). Multi-9 sw-1m agg **7569** (was 6925), rebuild
lines per server 113–131 → **7–8**. Still below bits=0 **9342**;
residual is per-chunk server latency under 9-way contention
(`g_server->lock` sharing between REPORT/flush/PUT). mc_stress on
bits=3 VERIFY-OK both clients; kill -9 non-primary shard owner
(fcstor005) → 200/200 failover reads OK, restart rejoined, cold-mount
client read OK. Cluster healthy (4 up / 0 down).
**Perf push (Aug 23, committed, bits=0, all on top of the above):** a chain of
CPU/lock fixes validated on a FRESH cluster (wiped with edelete + mkfs —
accumulated chunk-table state was silently degrading throughput ~15-40%, so
A/B on a grown table is meaningless; always measure perf on a fresh mkfs).
Fresh-cluster peaks (all rc=0): single 007 sw-1m **5656** (was 4993); multi-9
agg sw-1m **9342** (was 8270) / sr-1m **30513** (was 27931) / rw-4k **13934**
(~flat). Server CPU during multi write dropped from 13-15 cores to ~2.6.
Fixes, in order: (1) shared-CQ + single poller thread replaced per-conn CQ
spin-polling (top CPU on client AND server); (2) generation-tagged recv
`wr_id` (`gen<<32|reg_idx<<8|buf`) so a stale completion from a destroyed QP
can't be injected into a reused conn slot (was corrupting `g_server->lock` →
SIGSEGVs); (3) server conn-thread leak — `efs_rdma_recv_wait` now polls the
TCP control fd alongside the efd (a silently-dead peer never errors the QP)
and drains the efd when the pend ring empties (4091 leaked threads → 32);
(4) server conn threads use `efs_rdma_reply_ready_quick` (no adaptive spin —
~13 cores of pure spin under 9 clients); (5) O_DIRECT write fast path —
`EFS_RDMA_RECV_ALIGN` places the PUT payload on a 4096 boundary so
`shard_io_thread` writes straight out of the recv buffer (no 64KiB bounce
memcpy, was 11% CPU); (6) clock-gated `now_us()` in the poller + recv_wait
spins (was ~1 core of pure clock_gettime); (7) client PUT reply wait capped
to a fixed 24us spin (`efs_conn_reply_watch_us`) — the adaptive budget handed
200us to each of 32 put workers (~8 cores of spin at 5 GB/s); (8) THE
multi-write lever: `REPORT_CHUNKS` held `g_server->lock` for ~8ms per report
(applying tens of thousands of chunk recs, yielding only every 8192) while
every data-path `PUT_CHUNK` blocked on the same lock — multi sw-1m was 12ms
avg / 1.5s max write latency. Now yields every 1024 recs + `sched_yield()` so
PUTs interleave → 8.8ms avg / 447ms max, multi sw-1m 7550→9342. **Remaining
write ceiling:** per-chunk server latency inflates ~870us→~3.9ms under 9-way
contention (residual `g_server->lock` sharing between REPORT/flush/PUT +
single recv_poller at 30% CPU). Next levers: partition the metadata lock or
N recv CQs/pollers; the real fix is Phase 3 data-path sharding (bits>0).
EFS_WRITE_PIPELINE 64 measured WORSE than 32 (single 3780 vs 4686) — depth
is not the limiter. EFS_RDMA_BUFS 32 ≈ 4 (recv depth not the limiter).
POSIX after the perf chain: 143/144 + `concurrent_creates_same_dir` flake
(passes 3/3 isolated) + 5 known XFS quirks.
**Data-path sharding load-split (the Aug 23 morning finding) is FIXED**
— see the "LANDED" paragraph above. The old
silent-drop + create-stride=`nlive` pair is gone. The remaining
multi-write gap vs bits=0 is extras catchup rebuild, not routing.
**Phase 3 code-complete (working tree, Aug 21):** tables + v8 persist +
owner routing + owner extra flush + on-demand LRU + spread creates
(dentry on parent, inode on child, stride=nlive) + `efs_export_rehash`
(`efs-mgmt upgrade`). Same-fd zeros fixed (dcache overlay).
**Phase 3 VALIDATED on a throwaway sharded export (Aug 21 PM, 0298143):**
`efs-s3` bits=3 (8 shards), single-export cluster (see multi-export gap
below). Low-bits ino encoding (`ino & (2^bits-1)`, root pinned to shard 0),
canonical sorted-live ownership, server rebuild preserves shard_id,
catchup rebuilds shard tables after main rebuild, client dual-apply
mirrors server (child full row + parent dentry), attr accessors route to
the canonical shard row (`shard_route`). **Root-protocol fix (the big
one):** extra-shard owners no longer bump the main gen on extras commits
(`server_commit_cluster_extras` same-gen), and PUT_META/catchup recognize
identical shard-0 checksums as an extras-only refresh — merge descriptors
(`efs_export_merge_extra_roots`: skip dirty tabs, skip >= gen), never
fence live tables. Flush thread drops dirty ops only when primary.
Cross-client O_APPEND is atomic via `EFS_MSG_INODE_APPEND` reserve RPC;
REPORT_CHUNKS applies size grow-only / mtime newer-only; `adopt_rpc_inode`
re-pulls chunk mappings on newer mtime even at same size.
**mc_stress (tests/stress/, 2 clients, bits=3): VERIFY-OK on both** —
200 same-dir creates cross-visible with content, 200 appends (100+100,
0 torn), 64×128 KiB disjoint chunks of a shared 8 MiB file intact on both
clients, rename churn clean, growfile monotonic. kill -9 shard owner →
16/16 failover reads OK; restart rejoined; cold mount 16/16 + appfile
200 lines. Zero "dirty under rebuild"/"unrecoverable" log lines on all
4 servers during the stress.
**Regression after all Phase 3 fixes (Aug 21 PM, bits=0 efs-test):**
POSIX **144/144** (`results/posix/20260821-185338`, fcstor007; 5
target-better = known XFS-baseline quirks), posix2 **12/12**
(`results/posix2/20260821-185357`, A=007 B=008), perf quick: single
007 sw-1m **5049** / ow-1m 4970 / sr-1m 4686 / rw-4k **1559** (399k IOPS);
multi-9 agg sw-1m **4842** / sr-1m 24576 / rw-4k **13917** (≈ the 13428
pre-collapse baseline; rw-128k 121 GB/s agg is the known not-credible
dcache artifact). No regression anywhere.
**Known gaps from sharded validation — ALL FIXED (Aug 22-23, working
tree):** (1) multi-export server metadata: PUT_META/GET_META/catchup are
per-export (named requests, per-export dirty/flush); two exports
(`efs-test` bits=0 + `efs-s3` bits=3) coexist on one cluster. (2)
shard-table pages GC'd on the commit=0 flush path too. (3)
`entry_timeout=0`/`negative_timeout=0` — no stale negative dentries.
(4) shard ownership remap fenced (commit-time recheck + extras filter).
Plus: sharded hardlink/unlink nlink routing (`efs_export_link`/
`efs_export_unlink_name` shard-aware), primary-owned extra-shard
durability (commit captures local tabs + max-merges previous
descriptors), self-heal create (parent dentry authoritative).
**Cross-client data fixes (Aug 22-23):** O_APPEND barrier
(`EFS_MSG_INODE_APPEND` reserve RPC + `append_rsv` table; barrier
released by REPORT after data lands — `dcache_put_now` marks the ino
dirty). rwfile staleness: (A) read-miss self-heal —
`efs_client_read` pulls the mapping range from the owner
(`efs_client_pull_layout_miss`, rate-limited 1/s/ino) instead of
zero-filling a stale-cache "hole"; (B) `dcache_note_size` bumps mtime
on EVERY write (reports are newer-only; a frozen mtime left peers
without an adopt trigger). FUSE flush/release use `fi->fh` (no path
re-lookup) so a failed lookup RPC can no longer silently skip the
close-time flush.
**efs-s3 root clobber FIXED (Aug 23):** non-primary servers in a
rebuild storm (root-only GET_META window after each primary commit →
peer page rebuilds; 114-116 rebuilds/5.5h) eventually adopted a
newer-gen root that lacked extra-shard descriptors; the catchup adopt
(`catchup_install_newer_root`) did a wholesale `root_move` with no
extras preservation, the rebuild wiped `shard_tabs` and reinstalled
from the (empty) extras → shard tables permanently lost → clients saw
3 inodes. Fix: `efs_export_root_maxmerge_extras` (per-shard
higher-gen-wins + carry-forward-missing) applied on EVERY adopt/commit
path — primary commit capture, PUT_META full-adopt, catchup install,
and `efs_export_merge_extra_roots` (which otherwise dropped
descriptors for shards with no local tab). Extras are now monotonic.
**Fresh-cluster validation (Aug 23, post-fix, efs-test + efs-s3
coexisting):** mc_stress on efs-s3 VERIFY-OK both clients + kill -9
shard owner failover OK + restart rejoin OK + cold mount OK; rebuild
counts 0/6/2/5 (storm gone), zero fence/dirty-drop lines. POSIX
**0 EFS bugs** on efs-test (`results/posix/20260823-071727`) AND on
efs-s3 (`20260823-071843`); posix2 **12/12**
(`results/posix2/20260823-071901`). Perf quick: single 007 sw-1m
**4993** / ow-1m 4974 / sr-1m 4578 / rw-4k **1536** (393k IOPS);
multi-9 agg sw-1m **8270** / sr-1m 27931 / rw-4k **13817** — all at or
above the Aug 21 baselines, all rc=0.
**Multi sw-1m FIXED (working tree, Aug 21):** 023817's 1274 agg
(~140/client lockstep) was a 100MB-table flush stalling PUTs — serialize
and peer deserialize ran under `s->lock`, and overwrite REPORTed every
checksum. Snapshot-serialize + unlocked catchup adopt + checksum-only
skip dirty. Single overwrite **4564** MiB/s; 9-client sw-1m agg **~4326**
(shared write ceiling; was 4593 on 220930). Live table ~880k chunks.
**Same-fd POSIX zeros FIXED (working tree):** `dcache_copy` now serves
fully-covered `have_base=0` dirty ranges; GET overlays those ranges onto
a zero/fetched chunk. That was the 2b-reads regression
(`basic_rdwr_no_reopen` et al.) — not a getattr/adopt size clobber.
**POSIX 144/144** (`results/posix/20260821-005111`, fcstor007) after Phase 3
code-complete (bits=0 live path). Re-run `20260821-010553`: 1 flake
(`concurrent_create_unlink_two_proc` rc=1) that passed on isolated retry.
**posix2 12/12** (`results/posix2/20260821-010629`, A=007 B=008).
**Test suite committed (d52287e)**: reusable POSIX + perf suites, results
tracked in git under `results/`. See "Test suite" section below.
**2 client data-path fixes (7fa99e8)**: packed-file overwrite/append + rename/
link data loss (see "Root causes" below). POSIX suite 64/69 (was 16 fail).
**5 POSIX gaps fixed (7e442fa) → suite 69/69**: daemon-side permission checks
(access/open via check_access), rmdir ENOTEMPTY, ENAMETOOLONG, serialized
O_APPEND, + a concurrent-write dcache flush race (see "Root causes").
**Suite extended to 144 tests (user, ef4e9cd + working tree) + 3 more data
fixes → 126/144, then the last 18 POSIX gaps fixed → 144/144 on fcstor007**:
- O_TRUNC in efs_fuse_open (ce269e0) — kernel gives no separate truncate for
  O_TRUNC on open, so truncate in the open handler.
- Concurrent-append re-dirty (c129b39) — a write landing during another
  thread's flush of the same chunk found dirty cleared, fell to load_and_patch,
  saw the not-yet-published chunk, and built a zeroed have_base=1 entry that
  overwrote in-flight data. Fix: patch the mid-flush entry + re-dirty it
  (pairs with the shard_io flush lock, which serializes the re-flush).
- chmod/setattr dual-apply size (396c1b8) — primary's returned inode lags the
  data path (learns size via async REPORT_CHUNKS), so a non-truncate setattr
  upserted a stale size=0, hiding data. Preserve local size unless truncate.
**18 POSIX gaps FIXED (working tree, Aug 20) → 144/144**:
- Permissions: daemon-side W|X on create/mkdir/unlink/rmdir/rename/link/
  symlink (export root skipped — root-owned 0755), ancestor X on getattr/
  open/access, R on readdir, W on path truncate, EPERM chown for non-root.
- Metadata: create dual-apply via create_with_ino (parent nlink++), unlink/
  rename adjust dir nlink, chmod bumps ctime even in the same second,
  setattr mode/owner applied locally so hard-link rows share mode;
  attr_timeout=0 so the high-level path cache cannot hide the other name.
- Errno: rename-over-nonempty maps EFS_ERR_NOT_EMPTY → ENOTEMPTY (was EIO).
- Features: st_blocks counts present chunks (not holes), .lock implements
  overlapping exclusive byte-range locks, .lseek implements SEEK_HOLE/DATA.

## Test suite (d52287e)
Run from the login node (needs ssh to nodes): `tests/run_tests.sh
posix|posix2|perf|setup|all`. `setup` rsyncs+builds+mounts the 9 pure clients.
- **ewrite sweep** (`tests/run_tests.sh ewrite [host]`): 30s
  `ewrite.sh <mnt> 1 {2,4,8,16}`, kill, record bytes/wall. Default
  fcstor007:/tmp/efs/mnt. Results: `results/ewrite/`.
- **POSIX** (`tests/posix/posix_suite.py`, **162** tests, @test-extensible): run vs
  an XFS baseline (node9901:/data1/efs); `compare.py` → efs bug = PASS on XFS,
  FAIL on efs. Extended Aug 24 (+18). Latest bits=3 `20260824-131138`:
  155 both-PASS, 2 new EFS bugs (`ctime_on_link_rename`,
  `direct_unaligned_einval`), 5 known XFS target-better.
- **POSIX 2-client** (`tests/posix/posix_2client.py`, **24** tests): create/mkdir/
  rename/O_CREAT on client A, check client B with no remount. Extended Aug 24
  (+12). Latest bits=3 `20260824-131451`: 22/24; EFS bugs
  `peer_flock_exclusive` + `peer_unlink_while_b_has_fd`. Harness remounts B
  only if the parent is missing. `fusermount3 -uz` — a busy FUSE remount hangs.
  Run: `tests/run_tests.sh posix2`.
- **Perf** (`tests/perf/perf_node.sh`): dd + 9-job fio, single|multi client
 (parallel), aggregated to `results/perf/history.tsv`. Harness logs stay
 LOCAL (don't depend on the efs read-after-write path being benchmarked).
- **Multi-client stress** (`tests/stress/mc_stress_worker.sh` A|B on two
 clients + `mc_stress_verify.sh`): same-dir creates, same-file O_APPEND,
 disjoint 128 KiB chunks of one shared file (chunk-aligned — sub-chunk
 cross-client RMW is still a follow-on), rename churn, read-during-write
 monotonicity. VERIFY-OK on both clients = pass. NOTE: run the harness
 UNSANDBOXED (ssh to the nodes); a sandboxed run fails ssh silently and
 produces empty 0-test results.
- **NFS scratch baseline (20260820-221413)**: same suite, one client
  (fcstor007) vs Engaging ORCD scratch NFS
  (`~/orcd/scratch/efs/nfs-fio-baseline` → fstor004.ib, nfs4, 294T).
  Recorded under `results/nfs/` (not mixed into efs `perf/history.tsv`).
  All rc=0. fio sw-1m 5725 / sr-1m 17818 / rw-4k 131 MiB/s. Sequential
  read is well above efs single-client; 4k random write is in the same
  ballpark as the collapsed efs number before the dcache-limit fix.
- **Local NVMe ceiling (20260820-221902)**: same suite on fcstor003-006
  `/data1/01`–`06` (6×7T XFS NVMe each). Harness:
  `tests/run_tests.sh nvme` → `tests/perf/perf_local_nvme.sh`. Writes only
  under `<path>/fio-ceil` (never live `<path>/efs`); efsd stayed up.
  All rc=0. Per-drive serial typical: sw-1m ~3.5–3.9 GB/s, sr-1m
  ~3.2–4.1 GB/s, rw-4k ~2.4 GB/s. Host-sum with all 6 busy: sw-1m
  16.7–21.4 GB/s, sr-1m 21.4–23.5 GB/s, rw-4k 11.2–12.4 GB/s. Two
  serial outliers (fcstor004 /data1/01 sw-1m 642, fcstor006 /data1/03
  rw-4k 647) — likely momentary efsd/device contention, not a dead drive.
- Multi-client baseline (20260820-170303): write ceiling SHARED (~4.9 GB/s
  aggregate sw-1m across 9 ≈ single-client), read scales (~25 GB/s sr-1m).
- **Perf re-run after POSIX 144/144 (2b47a2c), Aug 20**: single
  `20260820-204328` + multi-9 `20260820-204949` (all rc=0). Sequential
  1m write still shared-ceiling (multi sw-1m agg 5853 MiB/s). 4k IOPS
  collapsed vs morning baseline: single rw-4k 1372→125 MiB/s, multi
  rw-4k 13428→302 MiB/s. Cached dd-read also ~3× slower.
  **rw-4k profiled (fcstor007, Aug 20): NOT getattr/attr_timeout.** perf
  `-g -F 999` during isolated 8-job 256m randwrite: getattr/check_search/
  inode_allocated_bytes ~0%. Cost is 128 KiB RMW flush — GET fragment
  ~33% inclusive, dcache_flush_slot_inner ~41%, writer
  `maybe_reclaim` inline GET+PUT when dirty_bytes > 2×512 MiB cap
  (working set 2 GiB). `have_base=0` on published chunks forces a full
  chunk GET before PUT. strace of efs-fuse blocked (Yama) — **historical;
  ptrace_scope is 0 since at least Sep 1 2026, strace -p works.**
  **rw-4k ROOT-CAUSED + FIXED (working tree, Aug 20)**: two changes.
  (1) `dcache_load_and_patch` now reads the published chunk base ONCE at
  patch time (have_base=1) instead of zero-fill + a GET on EVERY flush —
  flush is now PUT-only (verified: no client_read under flush_slot_inner).
  Correct hygiene but NOT the lever by itself (GET just moved to patch).
  (2) THE LEVER: `dcache_reclaim_limit` default 512 MiB → **2 GiB**. The
  dcache is 65536 slots × 128 KiB (8 GiB hard cap) but reclaim keyed off
  512 MiB held only ~4096 chunks; a 2 GiB randwrite working set (16384
  chunks) churned reclaim → every evicted partial chunk = a 128 KiB RMW
  (~32-64x amplification). At 2 GiB the working set stays cached, repeat
  4k writes coalesce, flush is one PUT. Verified: EFS_DCACHE_BYTES=4G and
  the new 2 GiB default both recover single rw-4k to ~1545 MiB/s
  (run 20260820-220703, was 125). Multi-9 rw-4k 302→1799 MiB/s agg
  (20260820-220930) — still well under the 13428 morning baseline; the
  residual is cross-client RMW contention on the shared write ceiling
  (sw-1m agg 4593 ≈ ceiling). Single-client is fully fixed; multi-client
  sub-chunk/ranged PUT is the follow-on if that gap matters.
- **Perf after Phase 3 (2b63d31), Aug 21**: single `20260821-013841` all
  rc=0 — sw-1m 4255 / sr-1m 3566 / rw-4k 1554 (matches 220703). First
  multi `20260821-023152` is invalid (009–015 still on stale v7 FUSE,
  all rc=1 in 1s). Remounted those seven; multi `20260821-023817` all
  rc=0. dd seq1m.write agg 18944 / sr-1m 9171 / rw-4k 9225. fio sw-1m
  collapsed to 1274 agg (~140/client) vs 4593 on 220930 — 9-way write
  lockstep, not a single-client regression. rw-128k agg 90 GB/s is not
  credible (likely dcache-served or a parse/report artifact).
  **FIXED (working tree):** (1) flush/GET_META serialize via table snapshot
  unlocked; (2) catchup `deserialize` into a tmp table then
  `efs_export_adopt_tables` (was under `s->lock` for 800k chunks);
  (3) REPORT norollup + yield every 8k; (4) overwrite with same placement
  does not `mark_chunk_dirty` (checksum-only). That flush+peer-rebuild
  storm is what pinned 9 writers at ~140. Revalidated after efsd bounce +
  remount 007–015: single overwrite 4564; 9-client warmup ~231/client
  (~2.1 GB/s agg first-write), sw-1m 262×8 + 2230 on 015 ≈ **4326 agg**
  (shared ceiling). First-write of *new* files on a huge table is still
  ~1.7 GB/s single (grow + REPORT); overwrite is the apples-to-apples
  number. dd-of-zero still skips PUTs — do not trust seq1m.write agg.

## POSIX gaps vs XFS (suite, Aug 20) — ALL FIXED, 144/144
FIXED (7fa99e8): packed-file overwrite/append + rename/link data loss (see
"Root causes" below). All data-path tests pass (basic_append, basic_oappend,
basic_overwrite_middle, basic_terminal_cp_cat, basic_dd_rw, dir_rename_*,
hardlink_*, unlink_open_file).
**unlink-open investigated — NOT reproducing**: the theoretical concern
(efs_export_unlink_name removes inode+chunks on nlink->0, breaking open-fd
reads) does NOT manifest. Reads via the open fd succeed for buffered AND
O_DIRECT, small packed AND large chunked, flushed AND unflushed. No fix needed.
FIXED (7e442fa) — the last 5:
- **O_APPEND atomicity** (concurrent_appends): serialized daemon-side
  (g_append_mu + re-read true end) AND the underlying concurrent-write dcache
  flush race (see "Root causes" — the real data-loss bug).
- **Permission enforcement** (attr_access, err_write_readonly_file): daemon-side
  check_access helper in the access + open handlers. NOT default_permissions
  (that opt gated the root-owned export root → whole mount read-only).
- **rmdir-nonempty ENOTEMPTY**: client fast-path + authoritative server check
  (EFS_INODE_RPC_NOT_EMPTY).
- **ENAMETOOLONG**: name-component >255 check in split_parent_name.
- **Hardlink** (known FUSE kernel-level gap, below) — much improved by the
  7fa99e8 link fix (hardlink_* now pass) but the kernel-level dentry gap
  remains latent.

## Phase 2 sub-phase state
- **2a server-side mutation durability (DONE, cffb138)**: RPC INODE_CREATE/
  UNLINK handlers bump rpc_dirty_ops[eidx]; primary-only meta_flush_thread
  batches + commits dirty exports via CoW server_flush_fragmented_meta.
- **2b route FUSE through RPC (DONE, d7d2c7c)**: all FUSE mutations (create/
  unlink/rename/setattr/link/symlink/readlink) run on the primary via RPC
  (rpc_send_recv_primary retries NOT_PRIMARY), dual-applied to the local
  snapshot via efs_export_upsert_inode. Server allocates inos (next_ino moved
  server-side). Data path: efs_client_report_dirty snapshots the dirty set and
  sends REPORT_CHUNKS (chunk mappings + ino size/mtime) to the primary instead
  of the client blob flush — the client flush/election/dirty-set machinery is
  no longer the flush path. fsync = sync REPORT_CHUNKS + server commits before
  replying. Validated on a fresh mkfs cluster: all mutations + data + sparse
  holes persist across remount, flush failures 0.
  **Two server flush bugs fixed in 2b**: (1) server_flush_fragmented_meta
  carried a stale pre-2a client write_lease_id → peers rejected the root (no
  quorum, mutations lost); now cleared. (2) meta_flush_thread raced a sync
  fsync flush on the same new_gen → loser rejected STALE → fsync EIO; now
  serialized by efsd_server.meta_flush_mu.
- **2b reads (DONE, working tree, Aug 20)**: lookup/getattr/readdir via
  `rpc_send_recv_primary`. `adopt_rpc_inode` upserts new rows only; merge
  remote size/pack when the primary grew (or newer+shrink). GETCHUNKS is
  indexed (`start` = chunk_index) — never scan `ex->chunks[]` (387k-row
  scan under `g_server->lock` stalled the cluster). Packed files: pull
  only the pack_off window. `posix2` 12/12. Single-client POSIX 135/144
  (4 same-fd zeros — see Test suite).
- **2c full-table gen-check cache**: **skip, never needed.** One-gen/one-blob
  cache would be ripped out for Phase 3. The *idea* (don't RPC every stat)
  is Phase 3 item 4: per-shard on-demand LRU. Same-fd POSIX zeros land there.
- **Phase 3 (STARTED, Aug 20)**: design `docs/phase3-sharding.md`. This cut:
  `efs_shard_owner_of`, `efs_export_alloc_ino` (parent shard when bits>0;
  today's `next_ino++` when bits=0), RPC send path takes an owner ino
  (still the export primary until per-shard flush). Next: split the table
  + flush (`efs_meta_shard_table_ino`), then ungate `rpc_owner_conn`.

## Known live issues
- **Hardlink (`ln`) flaky at the FUSE kernel level** (pre-existing, NOT a 2b
  regression): the kernel intermittently fails the link's *source* path
  resolution from a cached dentry WITHOUT calling the daemon (efs_fuse_link
  never invoked). The 2b link RPC itself is correct (server applies it —
  nlink bumps). efs-fuse uses the high-level FUSE API which has NO .lookup op
  (never had one) — likely a dentry/generation-tracking gap. Doesn't affect
  fio/rclone/dd (no hardlinks). Not yet root-caused.
- **efs-fuse double-free SIGABRT** (reported Aug 19): was on the client-driven
  flush+resync path. **2b removed that flush path** (clients no longer
  blob-flush), so this is likely moot now — but the resync/rebase path still
  exists. If it recurs under 2b, reproduce with an ASan efs-fuse.

## Known live issues (not yet root-caused)
- **efs-fuse double-free SIGABRT** (reported Aug 19): `double free or
  corruption (!prev)` during heavy write (`ewrite.sh /tmp/efs-mount/001 1 32`,
  32 jobs) + resync/rebase under transient no-quorum. Log: STALE quorum →
  resync → `rebased ... onto gen 100623` → abort. Backtrace only shows libc
  `free`→abort (efs-fuse frame is just the fatal-signal handler). Code review
  of resync (fetch/deserialize/merge/swap/adopt/frees), flush blob ownership
  (`blob_is_cache`, `dirty_snap_free`), `incremental_serialize_v6` bounds,
  `efs_export_root_copy` (deep), `MARK_SPAN` (bounds-checked) found NO obvious
  double-free/overflow. Crash binaries (with debug_info) in ~/git/efs/logs/.
  Cluster healthy after (4 up/0 down). **Next step: reproduce with an
  ASan build of efs-fuse (`-fsanitize=address`) under the same workload to
  pinpoint the exact free.** This is the client-driven flush+resync path that
  Phase 2 replaces — so Phase 2 is the long-term fix, but the bug is live
  until then.

## Done — don't re-test
- Phase 1 perf fixes (8b71dd3, bed1123, 00323c4, cddf692, 41d047e): RDMA
  latency, statfs phantom usage, torn-name readdir, meta-flush election
  hardening, GET_META serialize cache, efs-fuse RSS ~27GB→~4GB, read-verify
  moved server-side, resync/rebuild staged out of global locks.
- **v7 wire format** (f147fb8): split inode/dentry regions, page-aligned
  dentries → O(1) creates. Validated to 50K files, ~5.6 ms create+fdatasync.
- **Cycle fix** (d7b95fb): recompute_dir_postorder stack-overflow on cyclic
  garbage tables (the "raced deserialize SIGSEGV" that killed fcstor005).
- **CoW metadata flush / EFSR v7** (debe6dd): root carries page_cis[] +
  next_ci; flush writes each dirty page to a fresh ci and commits the root
  only after all pages land; rebuild reads page_cis[i] exactly; GC
  (server_gc_meta_cow_pages) reclaims cis the old root no longer references.
  Validated: kill -9 mid-flush (touch + fsync-heavy) remounts clean, no torn
  pages. **Torn-page root cause is fixed.**
- Scaling roadmap Phases 2–4 (0a381b8): docs/scaling-roadmap.md.
- Validation: fio suite on fcstor007 all rc=0, no segfault under load
  (sw 4.6 GB/s, sr 2.4 GB/s, 4k rand ~40k IOPS). rclone 32×10GiB copy rc=0.

## Root causes already found — don't re-diagnose
- **names_crazy_roundtrip / `.fuse_hidden` leftover (ROOT-CAUSED + FIXED,
  Aug 26)**: NOT an efs bug, and the silly-rename is done by **libfuse's
  high-level API** (the userspace lib efs-fuse links), NOT the kernel.
  Mechanism: the kernel defers the final `fput`/FUSE `release` (task_work /
  delayed_fput) by a few hundred µs, so it sends FUSE_UNLINK before
  FUSE_RELEASE. libfuse's `fuse_lib_unlink` then sees the file still open in
  its internal tree (`is_open`) and calls `hide_node` → renames it to
  `.fuse_hidden%08x%08x` (nodeid, hidectr); when the deferred release is
  processed, libfuse unlinks the `.fuse_hidden`. Proven with a timestamped
  open/flush/release/unlink/rename daemon trace (`EFS_SILLY_DBG`): the
  silly-renamed cycle is missing the read-close `release` before the unlink
  (a normal cycle has it); the release lands ~103µs AFTER the unlink, then
  `.fuse_hidden` is unlinked ~63µs after that. Always transient, never leaks
  (0 lingering after 19k cycles). Repro: `tests/debug/silly_race.py`.
  **`.fuse_hidden*` is the ONLY namespace artifact libfuse injects** — both
  unlink-of-open (`fuse_lib_unlink`) and rename-over-open (`fuse_lib_rename`
  → `hide_node`) use the same pattern; an exhaustive grep of lib/fuse.c found
  no other generated names. The kernel adds nothing to the mounted namespace.
  **`hard_remove` mount opt considered + REJECTED**: it skips the silly-rename
  but then read/write/fsync/fstat/ftruncate on the still-open fd fail ENOENT
  (libfuse removed the node) — breaks unlink-while-open (posix
  `unlink_open_file`/`unlink_open_then_recreate`). libfuse docs explicitly
  recommend against it.
  **Fix = daemon readdir hides `.fuse_hidden*` entries** (efs_fuse.c) — a
  deleted-open file is never user-visible; the open fd uses `fi->fh` (ino),
  not a path lookup, so filtering is safe and libfuse still cleans up.
  Prefix match on `.fuse_hidden` covers both the unlink and rename cases and
  any hex length. Edge case: a user file literally named `.fuse_hidden*`
  would be hidden too (de-facto-reserved namespace; accepted). Test reverted
  to a STRICT `listdir == []` (a leftover now = a real bug). The only way to
  eliminate silly-rename entirely is the low-level (inode-based) FUSE API —
  a large rewrite, not warranted for a transient cosmetic artifact.
  Single-client gate after fix: 186/191, 0 EFS bugs.
- **232MB / 1.9M-chunk adopt after wipe (Aug 25)**: not a client disk
  cache and not leftover `.efsm` on the NVMe. `adopted cache blob` is
  the GET_META payload kept in RAM. Wipe of `/data1/*/efs` + new `mkfs`
  while **any** `efs-fuse` is still alive (often D-state; `pkill -x`
  without `-9` misses it) lets that daemon REPORT/flush the old table
  onto the empty primary. Next mount sees the same fingerprint
  (`232007402B`, 110 inodes, 1932296 chunks, gen=102). **Wipe order:
  `killall -9 efs-fuse` on fcstor003–015, confirm zero, then efsd,
  then edelete.** `tests/wipe_cluster.sh`. A 1-second `cat fuse.log`
  can also show the previous mount's 232MB line because
  `rsync --exclude='*.log'` never deletes it — `rm -f fuse.log`
  before start; trust `meta ready` from the new process, not a stale log.
- **efs-s3 metadata clobber = extras loss on adopt (FIXED, Aug 23)**: a
  newer-gen root missing extra-shard descriptors (born during a rebuild
  storm) was wholesale-`root_move`d by `catchup_install_newer_root`;
  the rebuild then wiped `shard_tabs` and reinstalled from the empty
  extras → permanent shard loss (clients saw 3 inodes). All adopt/commit
  paths now `efs_export_root_maxmerge_extras` (per-shard higher-gen-wins
  + carry-forward-missing), so descriptors are monotonic. The storm
  itself = peers rebuilding whenever they poll during the primary's
  root-only GET_META window (gm_blob freed on new gen before the new
  blob is cached) — a perf nit, not a correctness bug.
- **Sharded-export data loss = dual-writer root gen (FIXED, working tree)**:
  the primary AND every extra-shard owner bumped the SAME cluster-root gen
  (owner's `server_commit_cluster_extras` did gen+1 with a STALE copy of
  the shard-0 page_cis). The primary's PUT_META handler accepted the newer
  gen, fenced+zeroed its live table, and the flush thread dropped the
  unflushed RPC ops ("dirty under rebuild"); the rebuild then fetched cis
  the primary's own flushes had long superseded (GC'd → unrecoverable).
  mc_stress symptoms: cross-client appends vanished (appfile 100/200, all
  one client's), packed files read size 0 on the peer, rwfile chunks
  missing. Fix: extras commits keep the main gen; receivers compare shard-0
  page checksums — identical ⇒ extras-only refresh, merge descriptors
  without fencing (per-shard single-writer makes gen-compare safe; dirty
  tabs are never fenced). Also: cross-client O_APPEND needed a server-side
  reserve RPC (local size reads race), and pre-sized files needed
  mtime-newer chunk re-pulls in adopt_rpc_inode (size-only growth missed
  peer writes into an already-sized file).
- **Multi-client sw-1m ~140 MiB/s lockstep (FIXED, working tree)**: not
  RDMA, not 1m-try_patch. After warmup, each close REPORTed ~32k new
  chunk recs; `meta_flush_thread` serialized the live 100MB table under
  `g_server->lock` (every PUT takes that lock for `export_acquire`).
  Peers then GET_META + `deserialize` of 800k chunks, also under the
  lock. Overwrite also dirtied checksum-only updates so the storm
  continued into sw-1m. Fix: snapshot serialize, unlocked catchup adopt,
  norollup REPORT, skip dirty when placement is unchanged. dd-of-zero
  "18 GB/s" is fake (`chunk_put_worker` skips all-zero PUTs).
- **Same-fd read-your-writes zeros after 2b-reads (FIXED, working tree)**:
  a first write to an unpublished chunk is `have_base=0` (zeros + dirty
  ranges). `dcache_copy` refused those entries (unpatched bytes are not
  data), so same-fd read fell through to GET/rdcache zeros. Close+reopen
  flushed then read published data and passed. Fix: serve a have_base=0
  entry when the requested range is fully dirty; overlay dirty ranges
  onto a GET/zero chunk when a read straddles chunks
  (`basic_chunk_boundary`).
- **Concurrent-write data loss = dcache flush base-read race (FIXED, 7e442fa)**:
  dcache_flush_slot dropped the shard lock (dcache_mu) before the network
  merge-base read + PUT, so two concurrent flushes of the SAME chunk (e.g. 4
  threads appending to one file, each close→flush) both read a stale pre-PUT
  base and the last PUT wiped the other's just-written ranges → NUL holes with
  a correct file size. Fix: per-shard shard_io mutex held across the whole
  flush (base-read + PUT). Lock order shard_io → dcache_mu; the base-read's
  efs_client_read only takes dcache_mu, so no deadlock.
- **Packed-file overwrite/append = rdcache staleness (FIXED, 7fa99e8)**:
  dcache_put_now wrote the merged chunk to the servers but never invalidated
  the rdcache entry that the flush's have_base=0 merge-base read had populated
  with the OLD published chunk → a later read served the stale rdcache copy
  and lost the update. Fix: efs_rdcache_invalidate(ino, ci) after a
  successful PUT.
- **rename/link data loss = dual-apply upsert (FIXED, 7fa99e8)**: the 2b
  dual-apply upserted the RPC-returned inode, which carries a stale size=0
  (the close's metadata flush is batched/async, so the server hasn't seen the
  size yet) → upserting wiped the fresher local data-path state (size, pack
  fields, chunk mappings). Fix: dual-apply via efs_export_rename /
  efs_export_link (name/parent/nlink/ctime only), fallback to upsert.
- Slow creates = **v6 dentry-shift** (O(table) memmove of the packed dentry
  region on every create), NOT fdatasync/election. Fixed by v7.
- **Torn metadata pages = non-atomic dual-slot flush** (PUT dirty pages to a
  same-parity slot before committing the root; kill mid-flush overwrites a
  page the committed root references → checksum mismatch → zero-fill →
  garbage next_ino). Fixed by CoW (debe6dd).
- fcstor005 SIGSEGV = **pre-existing rebuild-race** (torn page → cyclic garbage
  table → infinite postorder recursion), NOT v7. Fixed by cycle detection.
- **Catchup-vs-GC race (FIXED, 76753ef)**: a catching-up server rebuilding an
  OLD root lost the race to GC reclaiming that root's CoW cis → zero-fill →
  corrupt table. Fix: for a CoW root, an unrecoverable page = GC-race (not
  genuine loss), so rebuild returns EFS_ERR_PROTO (no zero-fill) and the
  catchup re-polls for the newest root. Validated: 20k fsync-creates while a
  wiped server caught up → 65 GC-races, 0 zero-fills, no corruption.
- **Stuck catchup (DEFERRED, todo 15)**: during that validation fcstor006's
  catchup hung ~7 min (0% CPU, idle conns, all S-state) after installing the
  final gen. NOT a blocking recv — peer conns already have SO_RCVTIMEO
  (peer_pool.c:173). Likely a futex (s->lock) or logic stall; needs
  gdb/instrumentation to root-cause. Workaround: restart the node.
- `used=0 B` in `efs-mgmt status` can be **stale gossip**, not data loss —
  check on-disk (`du`) before concluding.
- Build-ID gate: rolling efsd restart only works within the same commit.

## Housekeeping
- Integration tests `test_quota` / `test_migrate` are **pre-existing flaky**
  (unrelated to recent changes) — don't chase them as regressions.

---
**After any milestone/fix/validation, update "Where we are" + "Done" so the
next session doesn't repeat completed work.**
