# D25 logical truncate — single-entry protocol

October 21, 2026. Branch `d25-logical-truncate`, source devel `0e9df00a8`.
**Normative design for final review; not implemented; all gates NOTRUN.**
Linked items: D25 / W43 / W24
([contract](../backlog/decision-contracts.md#d25--logical-truncate),
[register](decisions.md)). This text replaces the declined 256 MiB EBUSY
refusal (`fe2e13624`) and the 65-participant coordinated truncate on
`devel-w123-recovery` (`1e7835d8a`). It folds in the original proposal
(`06098fa98`), two reviews (`7f8ffc6a9`, `58eea8360`) and two author
responses (`48c423434`, `a1b3e68c1`). Those are kept in the
[appendix](#appendix--review-history-superseded) and are **superseded**:
where they differ from §§0–7, this text governs. Fresh format only: no old
encoding is decoded, migrated or replayed (AGENTS.md).

## 0. Problem and direction

`host_truncate` (`src/server/raft_host.c`) deletes cross-group chunk rows in
`LANE_FENCE` entries before the inode-group `TRUNCATE` entry. If that last
entry is refused, the deletions remain: EOF keeps the old size but the
contents are gone (`git show
devel-w123-recovery:docs/status/w123-refused-truncate-20261020.md`). The
client adds a second non-atomic step by zero-filling and publishing the
retained tail before SETATTR (`efs_client_truncate`, `src/client/ops.c`).

The user's direction (Oct 21) is that truncate works like a delete. **One
inode-group Raft entry** commits the new size and, for a shrink, a new
content epoch and a fence-history entry. Nothing is deleted when the
truncate runs. Reads mask superseded mappings. A bounded asynchronous sweep
deletes or clips them and hands their fragments to the existing reclaim
paths. Truncate to 0 is the simplest case: one history entry `(E, 0)`, and
the sweep only deletes.

Reused from devel: per-part `content_epoch` on chunk rows; the masking
helpers `efs_fence_valid_end` / `efs_fence_view_build`
(`include/efs/fence_view.h`); GETCHUNKS per-part read views; the opid
window (`include/efs/opid.h`, as used by append receipts); the GC record →
`GC_FRAG_BATCH` → reclaim-journal `SUPERSEDED` pipeline; and packed-base
release through `packed_retirement` and the segment ledger.

## 1. State and encodings

All integers are big-endian unless stated. The opid suffix is the existing
`efs_opid_req_pack` form: 16 B UUID plus native-order session epoch (4 B),
sequence (8 B) and acknowledgement (8 B), 36 B in total.

### 1.1 Durable state

| record | shard | contents (new or changed fields in bold) |
| --- | --- | --- |
| inode row | inode | `content_epoch` `E`, `base_size`, `active_lanes`, times (unchanged layout) |
| fence history (`CONTENT_FENCE`, authority `EFS_META_FENCE_INODE`) | inode | **format 2:** `fmt:4=2`, `count:4`, **`revision:8`**, then `count × (epoch:8, size:8, done:8)`; at most 32 entries; epochs strictly increasing, sizes strictly increasing after pruning; `done` is a lane bitmap. Created with the inode (`count = 0`, `revision = 0`) and **never deleted** while the inode lives: an empty history keeps its revision, so `(E, revision)` never repeats |
| truncate receipt window (**new kind `TRUNCATE_RECEIPT`**) | inode | key `(ino, client_uuid, session_epoch)`, value an `efs_opid_window` (16 cached replies, 64-position bitmap). A completed truncate stores `rc = OK`, `extra = E_after`; nothing else is stored, and a replayed success answers with the inode's current attributes |
| truncate-pending (**new kind `TRUNC_PENDING`**) | inode's anchor shard | key `(ino)`, value `gen:8` |
| lane stamp | lane | `max_end`, `max_mtime`, `max_ctime`, `seq`, **`adopted_epoch`** (was `fenced_epoch`), `mtime_gen`, `append_bar`, `present`, **`sweep_target:8`, `sweep_next_ci:8`, `swept_epoch:8`**; 88 B |
| chunk row | lane | each part (base and each span) gains **`valid_len:4`**: visible bytes `[off, off + min(len, valid_len))`; a new part has `valid_len = len` |

Removed, and refused on decode: per-lane `CONTENT_FENCE` sidecars,
`LANE_AUTHORITY` records, fence-history format 1, and lane values of any
length other than 88 B.

### 1.2 Commands

| command | group | exact layout | bytes |
| --- | --- | --- | --- |
| `TRUNCATE` | inode | `tag:1 kind:1 ino:8 inode_gen:8 expect_epoch:8 expect_base_size:8 size:8 mtime_ns:8 opid:36` | **86** |
| `ACTIVATE` | inode | `tag:1 ino:8 inode_gen:8 lane:1 writer_epoch:8` | 26 |
| `BOOTSTRAP` | lane | `tag:1 ino:8 inode_gen:8 lane:1 epoch:8` | 26 |
| `TRUNC_SWEEP` | lane | `tag:1 ino:8 inode_gen:8 lane:1 target:8 count:1 count × (epoch:8 size:8)` | 27 + 16·count, count 1..32 |
| `SWEEP_DONE` | inode | `tag:1 ino:8 inode_gen:8 lane:1 target:8` | 26 |
| `RETIRE` | inode | `tag:1 ino:8 inode_gen:8 target:8` | 25 |

`TRUNCATE` offsets: kind at 1, ino 2, inode_gen 10, expect_epoch 18,
expect_base_size 26, size 34, mtime_ns 42, opid 50–85. `kind`: 0 = extend,
1 = shrink. `ACTIVATE` replaces `ACTIVATE_LANE` / `_MASK` / `_AT`.
`LANE_FENCE`, `LANE_SWEEP` used for truncate, and the old 63-byte
`TRUNCATE` with its tail candidate are removed.

### 1.3 Decoding (fail closed)

Each decoder checks, before any KV read or mutation: the exact length, or
for `TRUNC_SWEEP` `27 + 16·count` with `count` in 1..32; `kind` ≤ 1; lane
< 64; non-zero ino, generation and target; a valid opid
(`efs_opid_req_valid`); `size` with `size / EFS_MIN_CHUNK_SIZE ≤
UINT32_MAX`; and, for the snapshot, strictly increasing epochs and sizes
with the newest epoch equal to `target`. A failure is a **fatal apply
error**: the group's apply stops with a logged decode error. It is never
turned into a proposer verdict, and it is separate from BUSY, STALE and
INVAL. An inode with `content_epoch == UINT64_MAX` or a history with
`revision == UINT64_MAX` refuses a shrink (and `RETIRE`) with INVAL before
any change; that verdict is not fatal.

## 2. TRUNCATE

### 2.1 Handler (inode-group leader host, hosts both groups)

1. Under inode-group ReadIndex, read the row (`E`, `base_size`, gen) and run
   the getattr lane collect: lane stamps under lane-group ReadIndex, with
   the existing read-twice lane-sequence validation and the pending
   reductions, counting only lanes with `adopted_epoch == E` (§3.3).
   Result: `eof`.
2. `kind = extend` if `size ≥ eof`, otherwise `shrink`. A shrink uses the
   whole history as captured.
3. Propose one `TRUNCATE` carrying the client's opid, `expect_epoch = E`
   and `expect_base_size = base_size`, and wait for its apply verdict.
4. On STALE or BUSY, go back to step 1 with the **same opid**, within the
   request budget. On OK, return the inode. Nothing is proposed to any
   other group.

### 2.2 Apply (one KV batch)

1. Decode (§1.3). Load the row. A missing row or a different generation is
   STALE; a file that is not regular is INVAL.
2. **Receipt lookup**, on a private copy of the stored window with the
   request's acknowledgement applied. Three outcomes are kept distinct:
   - a **cached reply** for this seq → return it (OK and current
     attributes); write nothing;
   - a seq **at or below the acknowledged watermark** and not cached → the
     client already acknowledged it and must not retry it: answer STALE
     (acknowledged duplicate), write nothing. The zero-valued stub that
     `efs_opid_lookup` fills for a reclaimed seq is never read as a result;
     the implementation needs a lookup that reports "reclaimed" separately;
   - **new** → continue.
   This runs **before** the preconditions, so a retry of a committed truncate
   never applies twice.
3. **Receipt capacity.** Encode the completed receipt into the private
   window now (`efs_opid_fold`-equivalent on the private copy). If it does
   not fit (seq more than 64 above the watermark, or 16 cached replies),
   answer BUSY and write nothing: unlike the ordinary opid fallback, a
   truncate **never applies unrecorded**. The client's acknowledgements free
   capacity; the handler retries within its budget. A failed fold leaves no
   state behind: only the batch below is committed.
4. Preconditions. Any failure writes nothing and records nothing:
   `content_epoch == expect_epoch` and `base_size == expect_base_size`,
   else STALE; open append reservations give BUSY; for extend,
   `size ≥ base_size`, else INVAL (a handler bug).
5. **Extend:** `base_size = size`. Epoch and history are unchanged.
6. **Shrink:** `E' = E + 1`; `base_size = size`. Append `(E', size,
   done = 0)` after removing every entry with `size_j ≥ size`. If the
   history still holds 32 entries, answer BUSY and write nothing.
   `revision++`. Put `TRUNC_PENDING`.
7. Both kinds: `base_mtime` and `base_ctime` are raised to `mtime_ns`; the
   append cursor is deleted (existing); the receipt window encoded in step 3
   is written in the **same batch** as the inode and history changes.

Only a committed success is recorded. Under the same opid, STALE, BUSY and
INVAL stay retryable or final for the handler: they are never stored as a
receipt. A **definite** refusal (an apply verdict the handler received)
means the truncate did not commit. A timeout or leader change is
**ambiguous**: the handler, and then the client, resolve it only by retrying
the same opid, never by assuming the old file remains. The client keeps one opid until it gets a definite answer and
acknowledges it through the window's normal contiguous ack.

### 2.3 Why a stale extend is correct

Suppose an extend's collect missed a publication P that raises EOF above
`size` before the apply. The collect began after the truncate request
arrived and was coherent: lane sequences were validated, pending reductions
counted, and lanes on older epochs excluded. So P had not committed when the
request began, and P overlaps the truncate. `expect_epoch` excludes any
shrink in between, and `expect_base_size` excludes any extend or shrink
that changed the inode-owned size. The resulting state is `base_size =
size`, with P's bytes and lane EOF intact, so `EOF = max(size, P's end)`.
That is exactly the state of the valid order "truncate, then P". Append
reservations cannot interleave, because open ones refuse the truncate and
new ones validate against the same EOF vector (§3.3).

### 2.4 Client

`efs_client_truncate` drops the tail rewrite. It flushes the inode's dirty
records (existing), sends SETATTR with a fresh opid, then drops every
cached view of the inode whose authority is not the returned `(E,
revision)`.

## 3. Reads, writes and lanes

### 3.1 Validity rule

A part (base or span) with epoch `e`, chunk offset `off` and length `len`
shows file bytes `[c + off, c + off + min(len, valid_len, max(0,
valid_end(e) − c − off)))`, where `c` is the chunk start and `valid_end(e)
= min{size_j : epoch_j > e}` over the inode history (unbounded if no such
entry). Bytes it does not show read as whatever lies under it, or as zero
within EOF. Parts are applied base first, then spans in publication order,
each masked before it is overlaid (architecture §7.3). A part that shows
nothing is dead.

### 3.2 Coherent read view (GETCHUNKS, writer view)

On the host serving both groups:

1. Under inode-group ReadIndex, read `(E, history, revision)` twice and
   compare. On a mismatch, retry.
2. Under lane-group ReadIndex, build each chunk view as today
   (`efs_meta_get_chunk_view_tx`), with masks from the **inode** history
   and `valid_len`. The lane stamp is not an authority.
3. Re-read `(E, revision)`. If it changed, or any captured part has epoch
   `> E`, retry from step 1 (bounded tries, then BUSY).
4. The reply carries `authority_epoch = E` and `revision`. Each record
   carries its self-contained masked view.

A view captured this way stays valid after retirement: retirement happens
only after the sweep has deleted or clipped every part it would mask
(§4.4). Fragment loss during a read goes through the existing
refresh-and-retry path; zeros are never substituted for live data.

### 3.3 Lane adoption and publishing

**Adoption transition.** Every path that moves a lane to a newer epoch —
publish, `BOOTSTRAP`, `TRUNC_SWEEP`, and any reduction the audit permits —
uses one transition, `adopt(lane, e)`: if `e > adopted_epoch`, then
`adopted_epoch = e`, `max_end = 0` and `seq++`, atomically with the rest of
the entry; if `e ≤ adopted_epoch`, nothing changes, so current writes and
their maximum are kept. Resetting `max_end` matters: without it, a lane
whose old 1 MiB maximum is ignored after a shrink to 100 B would be
relabelled current by the raise and report 1 MiB again.

The writer publishes at the view's `authority_epoch = E`. This replaces
`read_view.fence_epoch`, which is taken from the lane stamp
(`src/client/write.c` lines 414, 3854 and 6708). Lane apply for a publish
at epoch `p`:

| lane stamp | result |
| --- | --- |
| absent | BUSY (activation and bootstrap first, §3.4) |
| `p < adopted_epoch` | STALE |
| `p == adopted_epoch` | apply as today |
| `p > adopted_epoch` | `adopt(lane, p)`, then apply |

A publish that keeps an existing part copies its `valid_len` unchanged, and
no publish ever raises a `valid_len`. A merge publish (sub-chunk RMW)
compares the exact source row value: part identities, sequences and clips.
A publish at `E` that lands on a lane not yet adopted after a shrink to
`E+1` is ordered before that shrink: the history masks it, the collect
ignores its EOF, and the sweep removes it.

**EOF summaries.** getattr, the size collect and the append EOF vector
count a lane's `max_end` and pending reductions only when `adopted_epoch
== E`. **Reductions (audit 1, §8.1):** no production path reduces an EOF
into any lane, so `max_end` is removed from the lane reduction triple:
`struct efs_txn_reduce` and its wire form lose `max_end`, `getattr`,
`collect_phys_eof` and `efs_meta_capture_append` stop reading
`red.max_end`, and a lane EOF can change only through the lane's own
entries, which go through `adopt`. Pending reductions then cannot carry an
untagged maximum at all. `present` and `st_blocks`
include dead rows until the sweep, so `df` lags a large truncate.

### 3.4 Activation and bootstrap

- `ACTIVATE` (inode group): STALE if `writer_epoch != E`. A lane that is
  already active gives OK and changes nothing. A newly active lane sets its
  bit and also sets its `done` bit in **every** history entry with epoch
  ≤ `E`: a lane registered at `E` cannot hold older rows (floor below).
- `BOOTSTRAP` (lane group): proposed only after an inode-group ReadIndex
  shows the lane active. It carries the inode's `E` at that moment, which
  is at least the activation epoch. Apply: absent stamp → create it with
  `adopted_epoch = epoch`, `max_end = 0`; present stamp → `adopt(lane,
  epoch)`. An equal or older epoch changes nothing. It never touches the
  sweep fields.
- **Floor:** a stamp is created only by `BOOTSTRAP` or `TRUNC_SWEEP`, never
  by a publish. With §3.3, a lane rejects every publish below the epoch
  its first stamp was created at.

### 3.5 Caches on every mount

Every cached chunk mapping and decoded chunk is keyed by `(ino, gen, E,
revision)`. It is served only while that key matches the inode authority
the client currently holds. That authority is refreshed under the same
rules that already govern size visibility (open, getattr, attribute
timeout), so a mount never shows discarded bytes once it can see the new
size. **Audit 2 (§8.2)** found the kernel caches nothing for regular
files, so the rule is entirely in the client: the decoded-chunk cache key
already includes the view's epoch, revision and masks, so it needs only the
§3.2 change to carry `E` and `revision`. The existing promise is kept
unchanged: a mount that opens the file after the truncate returns sees the
new size and data. An already-open descriptor keeps the size this mount
last had (`efs_fuse_getattr_ino`: "Open-fd size stays local", deliberate,
for unflushed writes and unlinked-open files) while a path stat shows the
new size; within `EFS_READ_MAP_TTL_MS` (default 200 ms) its reads show the
new data and zeros past the new EOF, never discarded bytes. That last part
needs the absent-entry fix recorded in §8.2.

## 4. Sweep, reclaim and retirement

### 4.1 Scheduling

The GC pass on the anchor's leader scans `TRUNC_PENDING`. For an inode, it
reads the history under ReadIndex. `T` is the newest entry, and `S` is the
snapshot of every entry ≤ `T`. For each active lane whose `done` bit for
`T` is clear, it proposes `TRUNC_SWEEP(lane, T, S)` until it observes
`swept_epoch ≥ T` under lane ReadIndex, then proposes `SWEEP_DONE(lane,
T)`. When every active lane is done for `T`, it proposes `RETIRE(T)`. An
inode whose last shrink answered BUSY because the history was full is
moved to the front of the next pass. That priority lives only in memory:
after a restart, the committed `TRUNC_PENDING` keys and histories are
enough to find all work again.

### 4.2 `TRUNC_SWEEP` apply (lane group)

Deterministic and bounded. It reads only this lane's shard prefix `(shard,
ino, gen, lane)` and the lane stamp. It never reads the inode, the other
group or wall time.

1. A missing stamp is created with `adopted_epoch = T`, `max_end = 0`,
   `sweep_target = T` and cursor 0. If `T < sweep_target`, the command is
   obsolete: OK, no change. If `T > sweep_target`: `sweep_target = T`,
   cursor 0, and `adopt(lane, T)`.
2. Scan chunk keys in order from `sweep_next_ci`. Stop at whichever comes
   first: 256 keys examined, 64 rows changed, the batch item cap, or a key
   under a pending transaction intent. The cursor stops **at** that key,
   and a later entry continues from it.
3. Each row is masked with `S` (§3.1), one part at a time:
   - A dead span is removed, and its fragments are queued as GC records.
   - A dead ordinary base is dropped (row base identity cleared) and its
     fragments queued. A dead packed base goes to `packed_retirement`
     exactly once.
   - A part that is only partly visible gets `valid_len` lowered to its
     visible length. Identity, checksums and stored length are unchanged,
     and a packed base keeps its ledger reference.
   - A row with no parts left is deleted and `present` lowered.
   - A row that changes nothing is left untouched.
4. Advance `sweep_next_ci`. If the scan reached the end of the prefix,
   `swept_epoch = max(swept_epoch, T)`.

S is valid for every remaining row even if it lacks entries that were
retired or pruned. A pruned entry could not lower any bound. A retired
entry `j < T` was retired only after every lane was swept for `j` or a
later target, so no row it masks is left. Because the snapshot needs no
revision check, retirement cannot stall the sweep.

### 4.3 The `swept_epoch ≥ T` property

The property: `adopted_epoch ≥ T`, and no part with epoch `< T` shows a
byte that `S` masks. It holds once the cursor reaches the end, because
step 1 adopted `T` before the scan, and from then on no publish below `T`
is accepted (§3.3). Rows behind the cursor change only through publishes at
epoch ≥ `T`, which leave older parts' clips alone; through sweeps at higher
targets, which only delete or clip; and through bootstrap, which only
raises `adopted_epoch`. No other update can occur, so the property stays
true.

### 4.4 `SWEEP_DONE` and `RETIRE` (inode group)

- `SWEEP_DONE(lane, T)`: OK with no change when the generation differs, `T`
  is not an entry of the history (retired or pruned), or the lane is not
  active. Otherwise it sets the lane's bit in `T.done`. The proposer
  observed the durable, monotonic `swept_epoch ≥ T` before proposing, so
  the apply reads no lane state.
- `RETIRE(T)`: OK with no change if `T` is not an entry. INVAL if `T.done ⊉
  active_lanes`. Otherwise it removes every entry with epoch ≤ `T` and
  increments `revision`. When the history becomes empty, the history record
  stays (count 0, revision kept) and `TRUNC_PENDING` is deleted in the same
  batch.

Retiring every entry ≤ `T` is sound because the sweep for `T` masked rows
with the whole snapshot `S`. A lane activated after `T` was marked done at
activation and is covered by the floor in §3.4.

### 4.5 Reclaim

Ordinary base and span fragments use the existing path: the GC record is
written in the same batch as the row change, then `GC_FRAG_BATCH`, then
`SUPERSEDED` records in each member's reclaim journal. Packed records are
released only through the segment ledger, never per file. A clipped part
keeps its whole object until the part is overwritten or dropped. An
`INODE` reclaim record cannot be used for truncate, because fragment files
under `(ino, ci)` are shared with new-epoch writes.

### 4.6 Packing and compaction

- A packing candidate is built from a coherent view (§3.2), with every
  history mask and `valid_len` applied, and the packed record is stamped
  with the **newest source-part epoch**, never the current inode epoch.
  `PACK_SWITCH` compares the full source row value, including identities,
  span sequences and clips.
- **A shrink between capture and SWITCH is safe in both cases.** If its
  entry is still in the history, the record is older than the entry and is
  masked by it. If the entry was retired first, the lane's sweep for that
  entry either changed or deleted the source row, so the CAS fails, or left
  it unchanged because it showed no bytes past the bound. In that case the
  masked image adds nothing outside the bounds that remain.
- Segment compaction copies only the visible range of each record and
  keeps its clip.

## 5. Crash, replay and progress

- **A truncate is one entry.** Internally the file is either wholly old
  (entry not committed) or wholly new (size, epoch, history and receipt all
  committed); there is no partial state. This closes the W123
  refused-truncate hole by construction. A *definite* refusal tells the
  caller the old file remains. A timeout or leader change does not: the
  outcome is resolved by retrying the same opid.
- **Lost reply:** the retry with the same opid hits the receipt before any
  precondition and returns the first result. A later write is never
  truncated again.
- **Every sweep-side step is idempotent:** adoption is a max; a sweep
  command is obsolete, restarts or continues according to its target; the
  cursor is durable; deletes and clips act on the current row inside the
  apply; `swept_epoch` is a max; `SWEEP_DONE` sets a bit; `RETIRE` is
  conditional. A crash between a row change and the reclaim acceptance is
  covered by the GC record committed with the change.
- **Snapshots** hold the history, receipts, pending keys and lane stamps;
  after a restart, the sweep is rediscovered from them.
- **Full history:** a shrink answers BUSY with nothing written. Progress
  needs no new truncate: the committed pending key drives the sweep, and
  `SWEEP_DONE` and `RETIRE` free entries. The client retries within its
  budget.

## 6. Gates

All run on the private node9901 cluster under `/tmp`, with ASan and
`ASAN_OPTIONS=detect_leaks=0`; orcd-mini and ORCD-test are not used.
Evidence summaries follow AGENTS.md. "Cold" means after unmount and remount
and a same-format orderly restart.

- **g0 refusal atomicity:** the W123 repro (300 MiB pattern, two mounts).
  Definite non-commits are injected after the handler's reads, at proposal,
  and as an apply refusal: O_TRUNC fails and the whole old file verifies
  from both mounts. Ambiguous outcomes (reply dropped after commit, leader
  killed around commit) are resolved by same-opid retry, and the file then
  verifies as wholly old or wholly new, consistent with the resolved
  verdict.
- **g1 speed:** sparse and allocated 100 GiB files, `dd if=/dev/null`
  (baseline 4.3 s at `4d6530dd8`). Report server commit latency and
  whole-syscall latency separately, plus `apply_max` on every replica.
  Single-digit milliseconds is a measured target, not a guarantee. The
  sweep then drains, and bytes verify cold.
- **g2 identity:** a lost reply, an internal recapture, an intervening
  pwrite, then a client retry returns the first result and keeps the
  pwrite. A distinct truncate to the same size truncates again. Also covers
  zero-size and same-size truncates. **Receipt capacity:** a full reply
  cache and a sparse seq beyond the 64-position bitmap answer BUSY with the
  whole truncate unchanged; an acknowledgement frees capacity and the retry
  commits; an acknowledged duplicate answers STALE, never a stub success;
  lost reply across a same-format restart at each boundary.
- **g3 interleavings:** delayed truncate requests and replies across
  another truncate, an extend, an append reservation and a pwrite; a stale
  extend against a concurrent publication (§2.3).
- **g4 lazy lanes:** a write that starts after truncate returns, on a lane
  not yet adopted, must survive; a delayed old-epoch publish across a
  shrink is invisible, leaves the size unchanged, and is later deleted;
  activation and bootstrap delayed across a shrink, a sweep and a
  retirement. **Bootstrap and EOF:** bootstrap of an existing lane after a
  shrink (old `max_end` must not reappear); duplicate bootstrap after a new
  write (its maximum kept); obsolete bootstrap after a newer epoch was
  adopted (no change).
- **g5 views and caches:** mount B reads before and after a truncate on
  mount A, both cold and warm; a read view captured before a retirement
  finishes after it.
- **g6 sweep:** restart of the anchor and lane leaders mid-sweep; a cursor
  blocked by a pending intent; new-epoch writes behind and ahead of the
  cursor; a stalled lane with more than 32 non-dominated fences: BUSY while
  blocked, completion once the lane resumes.
- **g7 clipping:** shrink, extend and partial rewrite with overlapping
  spans, before and after retirement, read cold.
- **g8 packing:** two files sharing a segment, one truncated or unlinked
  while the other is read; SWITCH delayed across a shrink, a sweep and a
  retirement, with mixed-epoch spans; truncate racing segment compaction.
- **D25 contract t1–t9**, with t3 read as "no served row" before the sweep
  and "row absent" after it.
- **Unit tests:** decoders (every length and field refusal is fatal),
  pruning, adoption table, getattr lane filter, clip monotonicity in
  publish and sweep, `SWEEP_DONE` and `RETIRE` evidence, receipt-before-
  precondition ordering.

## 7. Implementation order and open items

1. ~~The two audits~~ — **done, §8.** Their code consequences join step 2:
   remove `max_end` from the lane reduction (audit 1), and carry `E` and
   `revision` in the read view that keys the client caches (audit 2).
2. **Done — [step-2 evidence](d25-step2-gates-20261021.md), `3e133c257`;
   g4/g5 step-2 form PASS, full g4 waits for step 3.** One gated change:
   inode-authority read views (§3.2), the writer epoch
   source and `adopt` (§3.3), `ACTIVATE` / `BOOTSTRAP` and the floor
   (§3.4), and inode-keyed caches (§3.5). The client stamp is never switched
   without the matching server lane rules. Accepted on **g4**, and on
   **g5** for cache coherence.
3. **Done — [step-3 evidence](d25-step3-gates-20261021.md): g0–g4 PASS
   after the October 22 fix of a pre-existing stale-cache republish (UNCOND
   images now carry their formation epoch); lazy truncate stays off by
   default until step 4.** `TRUNCATE` with receipts (§2),
   behind the old path until **g0–g4**
   pass. Incremental private development is fine; no partly converted path
   is qualified.
4. `TRUNC_SWEEP`, `SWEEP_DONE`, `RETIRE`, reclaim and packing (§4), with
   g5–g8.
5. Remove `LANE_FENCE`, the destructive truncate path, tail stubs and the
   client tail rewrite. Update architecture §7.3 ("pushed by a bounded fence
   over the inode row plus the active lanes" becomes lazy adoption with the
   inode as the single history authority), the D25 contract row and the
   register.

Constants proposed here: history 32 entries, sweep 256 keys examined and 64
rows changed per entry. They stay proposals until g1 and g6 measure them.
W43 stays open until the gates pass.

## 8. Audit results — October 21

Source inspection of `d25-logical-truncate` at `bc1eff29e` (devel
`0e9df00a8` plus docs only). Read-only; no runtime run.

### 8.1 Audit 1 — reductions into lanes

| call site | target key | fields set | file lane? |
| --- | --- | --- | --- |
| `host_prep_lane_stamp` (`src/server/raft_host.c`), the **only** producer of `EFS_TXN_REDUCE`; called from 8 call sites, all in namespace transactions on a parent directory | lane `efs_kv_dir_lane(name)` of the **parent directory** | `max_mtime`, `max_ctime`, `mtime_gen`; `max_end = 0` (memset) | no |
| `host_prep_raw(... EFS_TXN_REDUCE_INO ...)` | inode row | nlink, entry count, times, used shards, parent, parent version | no; never touches `base_size` or `content_epoch` |
| `EFS_TXN_REDUCE_ADD` | `DSEQ` witness | counter | no |
| `EFS_TXN_REDUCE_OPID` | opid window | receipt | no |
| `efs_txn_prepare_reduce` decode path (`src/meta/txn.c`) | any key named in the PREPARE | the full triple from the wire | **not restricted today** |

Consumers of a reduction's `max_end`: `efs_meta_apply_getattr`,
`collect_phys_eof` and `efs_meta_capture_append` (all in
`src/meta/meta_apply.c`); the resolve fold folds the triple into the lane
record. `src/sim` also produces reductions and must follow the same change.

**Rule:** no production reduction carries an EOF, and the decoder accepts
one only because the field exists. Remove `max_end` from `struct
efs_txn_reduce` and from `EFS_TXN_REDUCE_WIRE` (32 → 24 B; fresh format),
and stop reading it in the three consumers and the fold. File-lane EOF then
moves only through lane entries that use `adopt` (§3.3), and no epoch tag
is needed on reductions.

### 8.2 Audit 2 — size and data visibility on a mount

| layer | call site | behaviour | effect on truncate |
| --- | --- | --- | --- |
| kernel page cache | `fuse_fi_direct_io` (`src/client/efs_fuse.c`), set on every regular-file open | `direct_io = 1`, `keep_cache = 0` | no kernel data cache to invalidate |
| kernel attributes and entries | `fill_entry`, `fuse_reply_attr(..., 0.0)` | `attr_timeout = entry_timeout = 0` | every stat reaches the daemon |
| daemon lookup memo | `LOOKUP_MEMO_*` (`efs_fuse.c`) | one-shot, 50 ms, invalidated by local mutations | at most 50 ms stale for a peer's change, as today |
| local truncate | `ll_setattr` → `ll_inval_inode` | drops pages at and past the new EOF on the truncating mount | unchanged |
| local inode row (size used by reads) | `efs_client_read` reads `efs_export_get_inode` | refreshed by open (`efs_client_stat_ino`), getattr and lookup — **except** a getattr on an inode open on this mount, which answers from the local row (`efs_client_stat_open`, only `nlink` refreshed) | a held descriptor keeps its size; a read without stat uses the last fetched size, as today |
| chunk maps | `efs_client_map_fresh` (`src/client/ops.c`) | a map is reused without GETCHUNKS only if pulled **after the last open** of that inode on this mount and within `EFS_READ_MAP_TTL_MS` (default 200 ms, capped at 10 s) | an open after the truncate always re-pulls |
| decoded chunks | `rdcache` (`src/client/read.c`), key `efs_chunk_read_key` (`include/efs/metadata.h`) | key covers base and span identities, packed reference, and `read_view.fence_epoch`, `revision` and every part's mask | a refreshed view with new masks never hits old bytes |

**Rule:** the kernel holds no state to invalidate. The protocol keeps
today's promise unchanged: open after the truncate returns → new size and
data; an already-open descriptor → its size stays local, and its reads
show new data (zeros past the new EOF) within the map TTL.

**Correction (measured on node9901, October 21):** this table first said
an open descriptor sees the new size at its next stat; the code and the
step-2 gate g5.1 show it does not (see the row above). The gate also found
a pre-existing client bug, present on the unchanged release `47ba6b590`:
`pull_chunks_range` (`src/client/ops.c`) never removed local entries for
chunks that a successful GETCHUNKS reported absent, yet marked the range
fresh, so a peer kept serving a truncated chunk's old objects until GC
deleted them (shrink, then extend: old bytes back from 1 MiB on mount B).
Logical truncate would hit the same path once the sweep deletes rows. The
fix drops such an entry if it is unchanged since before the RPC (step 2). The one required change is §3.2: the read view's
`fence_epoch` and `revision` must be the inode's `E` and history revision.
Today they come from the lane stamp, which lazy adoption no longer moves,
so without the change a cached map could stay keyed as current. g5 checks
both the open and the already-open cases.

## Final protocol review of `07011a9cc` — October 21

**Incorporated** into §§1–7 by the commit after `290c048cb`: the shared
`adopt` transition (§3.3, used by §3.4 and §4.2), mandatory receipt capacity
with distinct reclaimed-seq handling (§2.2), definite versus ambiguous
outcomes (§2.2, §5, g0), the persistent history revision and its overflow
refusal (§1.1, §1.3, §4.4), the audit outputs and kernel caches (§3.5, §7),
and g4/g5 in the acceptance order (§7). One correction found while
incorporating: `efs_opid_reply` holds one 64-bit `extra`, so a receipt
stores `E_after` only, and a replayed success answers with current
attributes (§1.1). The review text below is kept as written.

**Design review: changes required; implementation and all runtime gates NOTRUN.**
This review qualifies §§0–7 above; it is not part of the superseded appendix.
The single-entry direction, lane-local sweep and source-row CAS approach are
sound directions to implement, but the following details must be resolved first.
No code, cluster deployment or format migration is included in this review.

### Blocking: BOOTSTRAP must invalidate the old EOF summary

Section 3.4 raises a present stamp's `adopted_epoch` without resetting
`max_end` or advancing `seq`. Counterexample: a lane at epoch 0 has
`max_end = 1 MiB`; truncate commits epoch 1 and size 100. Its old EOF is
correctly ignored until BOOTSTRAP(1) relabels it as current. The next collect
then reports 1 MiB again, without any new publication. This follows directly
from the proposed epoch filter; current `collect_phys_eof` in
`src/meta/meta_apply.c` also confirms that the lane maximum contributes to EOF.

Use one adoption transition for publish, bootstrap, sweep and any permitted
reduction: when the epoch increases, reset the epoch-scoped EOF summary and
advance its validation sequence atomically. Equal or older bootstrap requests
must preserve current writes and their maximum. Bootstrap must still preserve
the sweep fields. The reduction audit must cover pending reductions too;
clearing only the stored maximum cannot neutralize an untagged pending maximum.
Add g4 cases for existing-lane bootstrap after shrink, duplicate bootstrap after
a new write, and an obsolete bootstrap after a newer epoch has been adopted.

### Required: receipt exhaustion must refuse the whole truncate

The proposed atomic receipt is right, but “same shape and bounds as
APPEND_RECEIPT” is not enough to specify its failure behavior.
`include/efs/opid.h` and `src/sim/opid.c` explicitly permit an ordinary operation
to apply unrecorded when `efs_opid_fold` cannot fit the receipt. The cache holds
16 replies and the completion bitmap spans 64 sequence positions.

Truncate must never inherit that fallback. Apply the acknowledgement to a
private window, check lookup/admission, and successfully encode the completed
receipt before committing any inode/history changes. Capacity exhaustion must
produce a bounded retryable refusal with the entire truncate batch unchanged.
Do not treat a partially modified in-memory window from a failed
`efs_opid_complete` as committed state. Test full reply cache, sparse sequences
beyond the bitmap, acknowledgement freeing capacity, and lost reply across a
restart at each boundary. A reclaimed lookup returns a zero-valued success stub;
it must not be interpreted as a real `(E_after=0, size=0)` authority result.
Keep acknowledged duplicates distinct from unacknowledged receipts whose exact
result must remain available. These are current-format correctness tests only.

### Gate ordering and two smaller clarifications

- The two audits may start immediately, but their outputs must identify every
  relevant call site and the resulting rule, not just record that code was read.
  The cache audit must include the kernel's attribute/page caches as well as
  userspace mappings, and ordinary read paths that do not reopen the file.
- g4 is missing from the explicit implementation-order gate lists. It is a
  prerequisite for accepting the joint authority/adoption change and the new
  truncate path, alongside g0–g3. Run g5 before accepting cache coherence;
  keep g6–g8 as sweep/packing gates. Private incremental development is fine;
  no partially converted production path should be qualified.
- Retain the history revision even when its entry count becomes zero; deleting
  the history record and recreating revision zero would defeat its use as an
  ABA-resistant cache/read validation token. Specify overflow refusal, just as
  for the content epoch.
- “Before commit” in §5 is an internal state boundary, not a conclusion a
  timed-out caller can draw. A lost reply or leadership change can leave an
  unknown outcome. g0 must inject a definite non-commit/refusal to demand the
  old payload; ambiguous outcomes require same-opid resolution, not an assertion
  that the old file necessarily remains.

**Recommendation:** incorporate these focused corrections, perform the two
call-site audits, then implement against the gates. The stale-extend argument
and conditional packing-CAS argument do not need another conceptual rewrite.
No backward-compatible encodings, retained-data migration or old-binary tests
are requested.

## Appendix — review history (superseded)

**Superseded by §§0–7 above.** Kept unchanged except for heading depth, so
the review trail stays readable: the original proposal (`06098fa98`, with the
42-byte correction from `7f8ffc6a9`), the two reviews and the two author
responses. Nothing here is normative; where it differs from §§0–7, §§0–7
govern. In particular, the 48/42-byte encodings, the lane-revision equality
check and the leader-selected sweep keys are withdrawn.

### Original proposal (`06098fa98`)

October 21, 2026. Branch `d25-logical-truncate` from devel `0e9df00a8`.
**Design only; not implemented; NOTRUN.** Linked items: D25 / W43 / W24
([contract](../backlog/decision-contracts.md#d25--logical-truncate),
[register](decisions.md)). Replaces the declined 256 MiB EBUSY refusal
(`fe2e13624`) and the 65-participant coordinated truncate on
`devel-w123-recovery` (`1e7835d8a`).

#### Problem

`host_truncate` (`src/server/raft_host.c`) proposes one or more
`LANE_FENCE` entries per cross-group lane, each of which **deletes** chunk
rows past the new size, and only then the inode-group `TRUNCATE` entry. A
refusal of the final entry (BUSY with append reservations, NOMEM, leader
change) cannot roll the deletes back: EOF stays old, contents are gone
(reproduced: `git show devel-w123-recovery:docs/status/w123-refused-truncate-20261020.md`).
The client adds a second non-atomic step: it zero-fills and publishes the
retained tail chunk before SETATTR (`efs_client_truncate`, `src/client/ops.c`).

#### Direction (user, Oct 21)

Truncate is a delete of the discarded content: **one Raft entry on the
inode's group** commits the new size and a new content epoch; nothing is
deleted at truncate time; readers ignore superseded mappings; GC reclaims
them afterwards. Truncate to 0 is the simplest case.

What devel already has and is reused: chunk rows carry `content_epoch` per
part (base and each span); stale-epoch publishes are refused against the
lane stamp (`p->content_epoch < ln.fenced_epoch`, `meta_apply.c`); the
masking rule `efs_fence_valid_end` / `efs_fence_view_build`
(`include/efs/fence_view.h`); GETCHUNKS already returns per-part masked
read views; the exact-key sweep select/apply path and the GC record →
`GC_FRAG_BATCH` → reclaim-journal `SUPERSEDED` pipeline.

What changes: the fence history moves to **one authority — the inode** —
and lanes adopt a new epoch **lazily** instead of being fenced by the
truncate. No transaction, no LANE_FENCE, no in-entry range delete.

#### 1. Entry encoding

`EFS_MD_CMD_TRUNCATE` is redefined in place (no old layout is decoded;
an old-format entry is refused with `EFS_ERR_PROTO`):

```
[tag:1][ino:8][inode_gen:8][expect_epoch:8][size:8][mtime_ns:8][kind:1]
kind: 0 = extend (size >= observed EOF), 1 = shrink
```

42 bytes for the fields listed above (corrected during review; no implicit
C-struct padding). Apply on the inode group, one KV batch, O(1) items:

1. Row exists, regular file, `inode_gen` matches, `content_epoch ==
   expect_epoch` — else STALE (handler re-reads and re-decides).
2. Open append reservations → BUSY (unchanged rule; nothing written).
3. **Extend:** `base_size = size`, mtime/ctime. No epoch bump, no history
   entry. The history already masks anything an earlier shrink discarded.
4. **Shrink:** `E = content_epoch + 1`; `base_size = size`; history append
   `(E, size)` on the inode authority key (`EFS_META_FENCE_INODE`) with
   pruning: every older entry with `size_j >= size` is dropped — it can no
   longer lower any `valid_end`. A shrink to 0 leaves the history `[(E,0)]`.
   History still full after pruning (32 strictly growing sizes, unswept) →
   BUSY, nothing written; the client retries on its budget.
5. Close the append cursor (existing DEL); `PUT` a **truncate-pending**
   key on the inode's anchor shard (ino, gen) so the GC pass schedules the
   sweep (§4).

The handler reads the row, history and the active-lane EOF collect under
ReadIndex, picks extend/shrink against the collected EOF, proposes the one
entry and returns its apply verdict. A shrink that races a concurrent
publish is linearised before or after it by the epoch rule below; an
extend computed against a collect that a concurrent publish then exceeds
is linearised before that publish (both were in flight).

Removed: `EFS_MD_CMD_LANE_FENCE` and `host_lane_fence_drain`, the
same-group NOMEM drain retry, the tail stub (`TAIL_EXT`, `KEEP_TAIL`,
cross-group tail CAS publish, zero-fragment stub digests),
`ACTIVE_SEEN`, `truncate_lane_range_del` / `trunc_del_cb` on the truncate
path, the per-lane history sidecars and `LANE_AUTHORITY` records, and the
client tail rewrite. The `efs_fence_*` helpers stay.

#### 2. Read-path validity

One rule, unchanged from architecture §7.3: a part with epoch `e_r` is
valid below `valid_end(e_r) = min{size_j : epoch_j > e_r}` over the inode
history; bytes past it read as zeros within EOF; a part with
`valid_end <= part start` is dead.

- **Same-group and cross-group lanes are treated identically.** GETCHUNKS
  is already served by a host of both groups: read inode row (`E`) and
  inode history under inode-group ReadIndex, then the lane rows under
  lane-group ReadIndex. A part with epoch `> E` means a shrink committed in
  between: re-read the inode (bounded tries, then BUSY). `fence_view_build`
  already refuses such parts. The reply's `authority_epoch = E`; the
  writer view's history comes from the inode, not from a lane.
- **Lane stamps become adoption markers.** `fenced_epoch` is renamed
  `adopted_epoch`; it is moved forward only by a publish at a newer epoch
  or by the sweep, never by truncate.
- **Lane EOF summaries.** `getattr`, the append EOF vector and the size
  collect count a lane's `max_end` (materialised or pending reduction) only
  when `adopted_epoch == E`. A lagging lane contributes nothing, so a late
  old-epoch REPORT cannot undo a shrink. File-lane reductions must carry
  the epoch they were made under or be refused on file lanes; which
  transactions reduce into file lanes is not yet checked.
- **Block counts** (`present`) include dead rows until the sweep removes
  them; `st_blocks` and `df` lag a large truncate by the sweep time
  (user-facing doc note, as the contract already says).
- **Client caches** are validated by `authority_epoch`; a changed epoch
  drops cached views for the inode. The client truncate becomes: flush the
  inode's dirty records (existing), SETATTR, invalidate.

#### 3. In-flight publishes and lane fences

At lane apply, a publish with epoch `p`:

| lane state | result |
| --- | --- |
| `p < adopted` | STALE (unchanged) |
| `p == adopted` | apply as today |
| `p > adopted` | adopt: `adopted = p`, `max_end = 0`, `seq++`, then apply |

`p` is taken from a GETCHUNKS/writer view whose authority is the inode
row, so a lane never adopts an epoch the inode has not committed. An
old-epoch publish that lands on a not-yet-adopted lane after the shrink
committed is ordered before the shrink: its row is masked by the
history, its EOF claim is ignored by the collect, and the sweep deletes
it. Sub-chunk writes at the new epoch merge on the **masked** base (the
existing writer-range code). A full overwrite replaces an old row by the
normal CAS and queues its fragments for GC.

Packing must use masked views: a candidate captured before a shrink is
re-validated against the inode epoch at SWITCH, and segment compaction
copies only retained ranges (the bug found on `devel-w123-recovery`,
`ae563ebed` area — port the test, not the branch).

#### 4. GC and reclaim of superseded fragments

The leader's GC pass picks up truncate-pending keys and runs a **truncate
sweep** per active lane, using the existing two-step exact-key path (lane
leader selects ≤64 keys, the selection is proposed as one entry):

`TRUNC_SWEEP [ino][gen][lane][history snapshot ≤32×16 B][n][chunk indices]`

Apply on the lane group:

1. Adopt: `adopted = max(adopted, newest history epoch)` (with
   `max_end = 0` if it moved). From here no older-epoch row can appear.
2. Per selected row, re-read and mask with the carried history: a wholly
   dead row is deleted and its base and span fragments go to `gc_queue` —
   the existing GC frag batch then turns them into reclaim-journal
   `SUPERSEDED` records, the same path as overwritten data.
3. A partly valid row (at most the boundary chunk of each history entry)
   is **clipped in metadata**: a new per-part `valid_len` field in the
   chunk row value (base and each span) is set to the masked length and the
   part keeps its epoch. No GET/PUT and no materialisation step. The clipped
   tail of that one object stays allocated until the chunk is overwritten
   or truncated below its start.
4. When the lane has no rows below the newest epoch past their bound,
   record `swept_epoch` on the lane stamp.

Retirement: when every active lane reports `swept_epoch >= epoch_j`, the
GC pass proposes `FENCE_RETIRE(ino, gen, epoch_j)` on the inode group. That
apply checks the lane evidence again (lanes are read under ReadIndex first;
a lane registered after `epoch_j` was registered at a newer epoch) and
drops entries `<= epoch_j`. An empty history deletes the truncate-pending
key. Unlink during a sweep: the reaper's whole-inode reclaim takes over and
deletes the pending key with the inode.

Readers holding a view of a deleted row keep their captured masks; if the
fragment is already reclaimed they refresh and retry (§7.3 rule, existing
fragment-loss retry). Truncate to 0 has no boundary rows: the sweep is
pure delete.

Not changed: fragment files are per `(ino, ci)` with the generation in the
name, shared with new-epoch writes, so a whole-inode `INODE` reclaim
record cannot be used for truncate. Per-fragment `SUPERSEDED` records are
the bounded path; an epoch-scoped directory is a possible later
optimisation, not part of this change.

#### 5. Crash and replay

- The truncate is one entry: either committed (new size, epoch, history)
  or not (nothing changed). A refusal or a lost leader before commit
  leaves the whole old file. This closes the W123 hole by construction.
- A lost reply followed by a client retry finds `expect_epoch` stale; the
  handler re-reads and, for the same size, answers OK without a second
  bump if `base_size == size` and the newest entry is `(E, size)`.
- Sweep entries are idempotent: adoption is a max, deletes are exact-key
  on the row version read, clips are CAS on the row value, `swept_epoch`
  is a max, retirement removes `<= epoch_j` only after the lane evidence.
- A crash between a row delete and the reclaim acceptance is covered by
  the existing GC record (written in the same batch as the DEL).
- Snapshots carry inode history, lane stamps and pending keys like any KV;
  the sweep is re-derived from them after restart.

#### Format changes (fresh format only)

TRUNCATE entry layout; chunk-row per-part `valid_len`; lane stamp gains
`swept_epoch` (fenced → adopted); inode history keeps the existing
`CONTENT_FENCE` value under `EFS_META_FENCE_INODE`; new `TRUNC_SWEEP`,
`FENCE_RETIRE` commands and truncate-pending key kind; per-lane history
sidecars, `LANE_AUTHORITY` and `LANE_FENCE` removed. Unknown or removed
encodings fail closed.

#### Gates (private node9901 cluster, ASan with `detect_leaks=0`)

Adapted from the D25 contract t1–t9; t3 now means "no served row" before
the sweep and "row absent" after it.

- **g0 refusal atomicity:** the W123 repro (300 MiB pattern, fault after
  the handler's reads and before propose, and a fault refusing the apply):
  O_TRUNC fails, the whole file verifies from two mounts.
- **g1 speed:** `dd if=/dev/null of=f` on a 100 GiB file (baseline 4.3 s
  after `4d6530dd8`); truncate latency and `apply_max` on every replica in
  the single-digit ms class; sweep drains (gc-pass) afterwards.
- **g2 stale cross-group publish:** delay one lane publish across a
  shrink: size and bytes past it unchanged, publish later deleted.
- t1, t2, t4, t5 (kill anchor leader mid-sweep), t7 (history sequence),
  t8 (history bound → BUSY retry), t9 (sub-chunk rewrites across fences),
  plus a packed-file shrink/regrow/repack case.
- Unit: history pruning, adoption table, getattr lane filter, clip CAS,
  retire evidence, refused old encodings.

#### Open questions for review

1. Extend keeps the D25 rule (size only, no epoch). Alternative: always
   bump — simpler, but grows the history on repeated grow. Recommend keep.
2. Boundary rows are clipped in metadata (`valid_len`) instead of
   materialised. This holds one partial object per boundary chunk. OK?
3. History stays bounded at 32 with pruning and BUSY when full.
4. `architecture.md` §7.3 still says the epoch is "pushed by a bounded
   fence over the inode row plus the active lanes". It changes to lazy
   adoption with the inode as the single history authority; the D25
   contract row and register line will be updated with the implementation.


### Independent technical review — October 21

Reviewed proposal/source `06098fa98` on `d25-logical-truncate`. **Design review
only; all new gates NOTRUN.** I support one inode-group commit for the logical
size/fence and asynchronous physical reclamation. Fresh format only; no legacy
replay, migration or downgrade machinery is requested. The following are
correctness requirements to settle before calling this implementation-ready.

#### 1. Request identity and size classification — blocking

The proposed entry has generation and expected content epoch but no operation
identity. Matching `base_size` and the latest fence is not proof that a retry is
of the same request. Two distinct truncates to the same size, separated by a
write, must not be confused; a lost-reply retry must not truncate later writes
again. Extension does not change the content epoch, so that epoch alone also
cannot reject a stale size collect after another extension/publication.

Specify a durable request identity/verdict and the rules for size classification
under concurrent EOF changes. A handler cannot simply assert that its stale
"extend" linearizes before a publish: the resulting EOF and masks must actually
match that order. Preserve the single-entry goal, but give a concrete proof or
protocol for the cross-group EOF collect, append reservations and subsequent
apply. Test delayed requests/replies across another truncate, extension, append
and pwrite, including identical requested sizes and zero/same-size operations.
No request-ID or format compatibility with older builds is needed.

#### 2. Read authority must be a coherent image — blocking

ReadIndex establishes an applied floor; separate inode/history reads following
it are not automatically one atomic image. Current chunk-view code in
`src/meta/meta_apply.c` rereads stamp/history and checks for movement. Preserve
an equivalent coherent capture for the new inode authority and chunk rows.
Checking only for a part with epoch greater than E is insufficient: a truncate
or history retirement can occur without any new-epoch part appearing.

Specify a revision that changes on history retirement as well as shrink, and
validate the capture or use an appropriate pinned snapshot. Self-contained
masks may survive retirement only if the source row and masks were captured
consistently. Include peer cache invalidation/revalidation: invalidating the
truncating client's cache alone does not establish correctness for a read
started on another mount after truncate returns.

#### 3. Sweep completion and retirement evidence — blocking

The stated `TRUNC_SWEEP` wire sketch carries indices but not the row versions
or history revision later claimed by the CAS/idempotence text. Choose one
precise model: inspect and transform the current row deterministically at
apply using validated immutable bounds, or encode the expected versions and
handle CAS misses without claiming coverage. Never let a missed/new row count
as swept merely because its key was selected earlier.

A `swept_epoch` acknowledgement needs a complete range-scan proof, a persistent
or safely restartable cursor, and adoption that excludes late older publishes.
History retirement on the inode group cannot depend on each replica reading
mutable remote lane state during apply. Encode durable, monotonic completion
evidence with inode generation, target epoch and lane-membership identity;
validate it deterministically against inode-owned state. Define how lane
registration races retirement. In particular, an old-view writer must not
register an omitted lane after history is retired and publish unmasked old
bytes. The assertion that every later registration uses a newer epoch needs
an enforced admission rule, not merely a handler convention.

#### 4. Clipping and packed-object lifetime — blocking

Metadata-only clipping is reasonable if every read, RMW, fold, packing and
compaction path respects it even after history retirement. Define `valid_len`
unambiguously for base and offset spans; preserve original object identity,
checksum and stored length while reducing visible bytes. A clip cannot be
undone by a delayed sweep or a mapping rewrite. Test shrink/extend/partial
rewrite before and after retirement, with overlapping spans and cold reads.

Do not send shared packed segment fragments to per-file SUPERSEDED reclamation
when one packed mapping dies. Use the packing mapping/ledger ownership path,
releasing the record exactly once and reclaiming the segment only when its
lifetime rules permit it. Partial clips retain their reference. A PACK_SWITCH
inode-epoch check also needs a serialized proof across groups; a handler-side
check followed by a later switch is a race. Test two files sharing a segment,
truncate/unlink one while reading the other, and race truncate against packing
and segment compaction. No automatic crash-recovery feature is implied.

#### 5. Fail-closed encoding and bounded-history progress — blocking

The listed entry is 42 bytes, now corrected above. Specify exact current-format
lengths, allowed kind values, size bounds and epoch-overflow refusal. Existing
`host_apply` normalizes TRUNCATE errors into proposer verdicts; a decoder's
`EFS_ERR_PROTO` alone does not establish fatal format refusal. Use the reviewed
fatal-encoding classification and abort/unlock rules before any mutation.

Bounded history with pruning is a sensible space bound, but BUSY needs a
progress mechanism: a full history must wake/reprioritize the sweep and retire
proven entries without requiring a successful new truncate. Persist the pending
work with the logical commit. Test a stalled lane and more than 32 non-dominated
fences; preserve bounded refusal while blocked and eventual completion after
progress resumes. Separate a legitimate BUSY verdict from malformed encoding.

#### Scope and implementation order

Keep the design's O(1) logical mutation and bounded asynchronous sweep. First
settle request identity/classification and authority/retirement proofs, then
implement coherent masking and lazy adoption, then sweep/packed ownership,
and finally remove the old destructive truncate path. Audit the noted file-lane
reductions before claiming the epoch invariant; it is not an optional follow-up.

For g1, report both server logical-commit latency and whole syscall latency:
client dirty-data flush, RPC queueing and quota pressure are outside the O(1)
metadata mutation. Single-digit milliseconds are a measured target, not an
algorithmic guarantee. Test large sparse and allocated files separately, and
verify all expected bytes before and after a same-format orderly restart.
The missing failure/interleaving gates above supplement t1–t9. No runtime
success, implementation approval or deployment is claimed by this review.

### Author response to the review — October 21

Response from the proposal's author to the review in `7f8ffc6a9`; checked
against devel `0e9df00a8`. Design text only; NOTRUN. Accepted points
change the design as stated below; the sections above stay as proposed and
will be rewritten once these points are settled.

**Encoding size.** Accepted: 1+8+8+8+8+8+1 = 42 bytes, not 48.

#### Additional gap found while checking the review

The client stamps its publications with the **lane** stamp, not the inode
epoch: `p->publish_epoch = obs->read_view.fence_epoch`
(`src/client/write.c:414`, also `:3854`, the merge check at `:6708`), and the
host sets `view.bytes.fence_epoch = stamp.fenced_epoch`
(`efs_meta_get_chunk_view_tx`). With lazy adoption the stamp does not move
at truncate, so a post-truncate writer would publish at the old epoch and its
data would be masked by the new fence: **data loss, not just staleness.**
Required: the read view carries the inode epoch `E` as its authority, the
writer publishes at `E`, and the merge/STALE checks compare against `E` plus
the history revision (below). §3 of the proposal assumed this already held.
It does not.

#### 1. Request identity and size classification — agree, with one claim kept

- **Identity: agree.** My retry rule ("`base_size == size` and newest entry
  `(E, size)` → OK") is wrong: a second truncate to the same size after a
  write would be answered OK without truncating. Use the existing opid
  session machinery (`efs_opid_req`, session verdicts in
  `include/efs/session.h`): the TRUNCATE entry carries the opid, and its
  apply records the verdict in the same batch. A retry returns the recorded
  verdict and never applies again. The entry grows by the opid. Exact layout
  to follow.
- **Classification: partly disagree.** The linearisation argument for a
  stale "extend" holds, but only if the EOF collect is coherent and starts
  after the request: a publish missing from a ReadIndex collect made after
  the request began was concurrent with it, so placing the truncate before
  that publish is a valid order. The resulting state (`EOF = max(s,
  publish end)`, publish bytes visible) is exactly that order. What the
  proposal missed is **inode-owned staleness**. Extend does not bump the
  epoch, so the apply must also refuse when the size it would set has
  already moved: the entry carries the `base_size` it observed, and the apply
  answers STALE if it differs (another extend or shrink came in between).
  Then `s < base_size` at apply cannot happen silently. Append reservations
  stay BUSY. The collect reuses getattr's lane-sequence double read, which
  is what makes it coherent. I will write this as a short proof in the
  rewritten §1 and test the delayed-request matrix the review lists.

#### 2. Coherent read image — agree on the mechanism, disagree on one reason

- **Agree:** add a **history revision** on the inode, bumped by both append
  and retirement. Capture `(E, revision, history)` with the same
  read-twice-and-compare pattern the current chunk view uses, read lanes,
  then recheck. This is cheap and matches existing code.
- **Disagree that "epoch > E only" is unsound for the order I specified.** The
  inode is read first and the lane rows after it. Retirement commits only
  after the sweep entries it depends on, so any lane ReadIndex taken after the
  inode read sees the swept rows. A shrink between the two reads with no
  new-epoch write leaves a view that is consistent with the state just before
  the shrink: the read is ordered before it. I still accept the revision
  recheck as defence in depth and to remove the cross-group timing argument.
- **Peer caches: agree. This is the more serious part.** The §7.3 rule
  ("validate against the lane's fence revision") stops working with lazy
  adoption, because the lane revision no longer changes at truncate.
  Validation moves to the inode `(E, history revision)`. Every cached
  mapping or decoded chunk is keyed by it, and a read on any mount
  revalidates it under the same lease or getattr rules that already govern
  size. Gate: on mount B, read before and after a truncate on mount A, both
  cold and warm.

#### 3. Sweep completion and retirement — agree; concrete model

- **One model: inspect and transform at apply.** History entries are
  immutable until retirement, and retirement waits for the sweep, so the
  carried history snapshot is a valid immutable bound. To be safe, the apply
  also checks the snapshot's revision against the one the lane recorded at
  adoption. The lane stamp keeps a **sweep cursor** `(target_epoch,
  next_ci)`. Each `TRUNC_SWEEP` entry scans at most 64 rows from the cursor
  **inside the apply**: the scan is deterministic because replicas hold
  identical KV. It deletes or clips them, then advances the cursor. The
  leader-side exact-key selection is dropped for this path, so a CAS miss
  cannot be counted as coverage. The first entry adopts `target_epoch`. After
  that, a lane publish below it is STALE, so rows behind the cursor cannot
  regain old epochs. When the cursor passes the end of the lane, the same
  apply sets `swept_epoch = target_epoch`. Restart resumes from the
  durable cursor.
- **Retirement evidence: agree that the inode apply must not read lanes.**
  The GC leader observes `swept_epoch` under lane ReadIndex. Because it is
  monotonic and durable, an observed value stays true. The leader then
  proposes `SWEEP_DONE(ino, gen, lane, target_epoch)` on the inode group,
  whose apply sets a per-target done bitmap in inode-owned state. Retirement
  applies only if `active_lanes ⊆ done` for that target, checked inside the
  apply.
- **Registration races: agree an admission rule is needed.** On devel, a
  lane stamp can be created by a plain publish (`ln.fenced_epoch =
  p->content_epoch` when absent, `meta_apply.c`), and that is the hole.
  Rule: lane activation is an inode-group entry that carries the writer's `E`
  and is refused if `E` is stale. The first stamp of a lane is created only
  by that activation's follow-up bootstrap or by a sweep, never by a
  publish. A publish to a lane with no stamp answers BUSY, which is the
  existing behaviour for the durable-result path. A lane activated after a
  fence at `E` cannot contain older rows, so its activation marks it done
  for every pending target.

#### 4. Clipping and packed objects — agree, plus one correction to the review

- **Packed bases: agree, and my note was wrong.** Devel already releases a
  dead packed base through `packed_retirement` and the segment ledger, not
  `gc_queue` (`trunc_del_cb`). The sweep keeps that split: ordinary
  fragments are reclaimed as `SUPERSEDED`, and a packed record is released
  exactly once through its ledger. A clipped packed base keeps its reference.
- **`valid_len`:** a visible-length clip on each part, at most the part's
  length and covering `[off, off + valid_len)` of that part. Object
  identity, checksum and stored length are unchanged. It only ever
  decreases: the clip CAS refuses a larger value, so a delayed sweep cannot
  undo it. A new overwrite replaces the part instead of growing it.
  Folding, packing, compaction and RMW all read through the same masking
  helper, which treats `valid_len` as an extra bound.
- **PACK_SWITCH race: disagree that cross-group serialisation is needed,
  provided the packed record keeps its source epoch.** The W123 bug came from
  stamping a raw image with the *new* epoch. If the packer builds the masked
  image from a coherent view (§2) and stamps it with the **newest source-part
  epoch**, a shrink that commits between capture and SWITCH still masks the
  packed record correctly, because the record is older than that fence. SWITCH
  then needs only its existing source-row CAS, plus a refusal if any source
  part has an epoch above the lane's adopted epoch. I'll add the two-file
  shared-segment and truncate-versus-pack/compaction races to the gates.

#### 5. Fail-closed decoding and full history — agree

- The current TRUNCATE path turns apply errors into proposer verdicts, so
  `EFS_ERR_PROTO` from a decoder would reach the client as an error while
  replicas carry on. Length, kind (0/1), the size bound (no chunk index past `UINT32_MAX`) and the
  epoch overflow check (`content_epoch == UINT64_MAX`) are validated before
  any mutation. A malformed entry stops the group's apply as fatal, which is
  distinct from the BUSY/STALE verdicts.
- **Progress when history is full: agree, and the pending key already lands
  with the commit (proposal step 4.5).** On top of that, a BUSY for a full
  history raises that inode's priority in the GC pass, and retirement runs
  from `SWEEP_DONE` evidence alone, with no new truncate needed. The
  stalled-lane test and the >32 non-dominated fences test are accepted.

#### Other points

- **File-lane reductions:** agree this is a prerequisite audit, not a
  follow-up. It runs before any epoch-invariant code.
- **g1:** agree. Report server commit latency and whole-syscall latency
  separately, and test sparse and allocated 100 GiB files separately.
  Single-digit milliseconds is a target to measure, not a claim.
- **Order of work:** agree with the review's order, with the publish-epoch
  source fix (above) placed first, because every later step depends on it.


### Follow-up review of the author response — October 21

Reviewed `48c423434`; source/design inspection only, **runtime gates NOTRUN**.
The response substantially improves the design. Rewrite §§1–5 into one
normative protocol next; leaving contradictory original and response text is
not a safe implementation handoff. No backward compatibility is required.

#### Disposition of the disputed points

- **Writer epoch: accepted and essential.** `src/client/write.c` copies
  `obs->read_view.fence_epoch` into the publication, and current chunk-view
  construction derives that field from the lane stamp. All writer-view,
  retained retry and merge paths must use the coherent inode authority under
  lazy adoption. Test a write begun after truncate returns on a lane that has
  not adopted yet. The described loss is a hazard if lazy adoption lands
  without this change, not a newly reproduced current-devel loss incident.
- **Stale extend: the proposed ordering can be valid.** A coherent collect
  performed during the request may order an omitted overlapping publication
  after the truncate, provided the result really remains
  `max(requested_size, same-epoch lane EOF)` and preserves those bytes. The
  expected-base-size check addresses an inode-owned change absent from the
  epoch-only precondition. I do not require an epoch bump on every extend.
  The proof must include coherent active-lane membership, pending reductions,
  shrink races (epoch check), and reservation admission at apply. Test those
  conditions; a generic claim about ReadIndex alone is insufficient.
- **PACK_SWITCH: withdraw a blanket cross-group-serialization requirement.**
  A coherently masked image at the newest source-part epoch can preserve
  shrink semantics without synchronously locking the inode group. This is
  valid only if the image incorporates every earlier mask and `valid_len`,
  and source-row CAS includes all part identities and clips. If a later
  shrink occurs before SWITCH, its retained fence masks the replacement.
  If its history was retired, the completed sweep must already have either
  changed/deleted any affected source row (making CAS fail), or proved that
  the replacement introduces no bytes outside the surviving bounds. Prove
  this two-case argument explicitly. Do not stamp the current inode epoch
  onto older captured bytes. Test SWITCH delayed through shrink, sweep and
  retirement, not only through shrink, including mixed-epoch spans.
- **Read ordering: accept the revision recheck as the chosen rule.** An
  overlapping read may legitimately linearize before a shrink; seeing a
  shrink during a read does not inherently make its earlier view invalid.
  My concern was an incoherent inode/history/row capture, particularly across
  retirement, not a requirement to return the newest concurrent state.
  With coherent capture, monotonic retirement evidence and the agreed recheck,
  there is no need to settle a weaker unchecked algorithm. Keep the actual
  rule in the normative text and cover warm peer caches.

#### Remaining protocol details to specify

1. **Opid results versus internal retries.** Persist a successful truncate's
   identity/effect/verdict atomically. Define how precondition STALE and
   history-full BUSY interact with that receipt. If every attempt records a
   terminal STALE for the same opid, rereading and retrying it cannot succeed;
   if a retry uses a new identity before resolving an ambiguous success, it
   can truncate again. Specify the existing session mechanism's applicable
   admission/receipt rules, recheck identity before preconditions, and gate
   lost reply plus internal recapture plus intervening writes. The wire size
   is now **TBD**, not 42: expected base size and the opid enlarge the earlier
   sketch. Publish an exact validated encoding in the rewrite.
2. **Deterministic bounded sweep.** Replicas do not have identical whole-KV
   images at every instant: different Raft groups apply independently.
   An apply-time scan is valid if it examines only the target lane/group's
   replicated state in deterministic key order, with explicit bounds and
   no dependence on remote inode reads, wall time or uncommitted overlays.
   Bound scanned work/bytes as well as changed-row count. Define restart of
   the cursor when the target epoch changes, handling of a stale sweep
   command, and preservation of clips when new-epoch publications retain
   older parts behind the cursor.
3. **Activation and SWEEP_DONE proof lifecycle.** Inode-group activation
   must commit before bootstrap can permit a publication. Duplicate or delayed
   bootstrap must never lower adoption or reset a newer sweep. Define what
   happens if a lane is already active: an old request must not be treated as
   a fresh empty activation and marked done. Bind SWEEP_DONE to inode
   generation, lane and target, ignore obsolete targets, and prove the
   acknowledged property survives all permitted lane updates. No handler-only
   claim that a lane is empty may authorize retirement. Marking a truly new
   lane done is safe only with an enforced bootstrap floor that excludes all
   older publishes, including delayed requests after retirement.
4. **Revision matching must allow retirement to make progress.** Inode history
   revision changes on retirement even when the target content epoch does not.
   Specify which adoption/target owns a carried history snapshot and how it
   remains valid or is retried when revision changes. An unexplained equality
   check against an old lane revision can permanently reject otherwise valid
   sweep work. A full-history priority hint may be in memory, but committed
   pending keys must suffice to rediscover work after restart.

The packed ledger correction and abort/fatal distinction are accepted. The
work order should implement the writer-epoch source, coherent views and lane
admission as one gated change before enabling lazy truncate; do not switch
only the client stamp while the server still enforces incompatible lane rules.
Then qualify retirement and packed compaction with the same-format restart
and interleaving tests above. This review changes no code or deployment and
claims no runtime acceptance.

### Author response to the follow-up review — October 21

Reviewed `58eea8360`; source inspection only, NOTRUN. I accept the
dispositions as stated, including the narrower PACK_SWITCH proof obligation
and the decision to recheck revisions as the read rule. I also agree the next step is one
normative rewrite of §§1–5; the rules below are what that rewrite will
contain. Two corrections to my previous response come first.

#### Corrections to my previous response

- **"Existing opid session machinery" was too broad.** Most of
  `include/efs/session.h` (lease edges, close steps, guarded probes) is
  staged, not production. The production replay path is the opid window in
  `include/efs/opid.h` (`efs_opid_lookup` / `efs_opid_complete` /
  `efs_opid_ack`), which CREATE uses (`HOST_CREATE_F_OPID`,
  `src/server/raft_host.c`). The truncate receipt should use that window,
  not the staged lease or session code.
- **"Replicas hold identical KV" was loose.** The KV on a node is shared by
  both groups, so the true statement is narrower: replicas of one group hold
  identical state *for that group's shards* at the same applied index. The
  sweep rule below depends only on that.

#### 1. Opid receipts versus internal retries

- Only a committed **success** is recorded. The TRUNCATE apply first runs
  `efs_opid_lookup` on the inode shard's window. A hit returns the stored
  result `(OK, E_after, size)` and changes nothing else. Then come the
  preconditions (generation, `expect_epoch`, `expect_base_size`, append
  reservations, history room). On success, the effect and
  `efs_opid_complete` go in the same KV batch.
- STALE and BUSY are **not terminal and not recorded**. The handler's
  internal retry reuses the same opid with freshly captured preconditions.
  This is safe: if an earlier attempt committed, its receipt answers the
  retry before any precondition is checked, so the effect is never applied
  twice. If no attempt committed, there is nothing to confuse.
- The client keeps the same opid across RPC retries until it gets a definite
  answer, and acks it with the window's normal contiguous ack. A new opid is
  used only after a definite answer.
- Malformed encodings stop the apply as fatal before the lookup. The window
  bound is the existing one.
- Gate: lost reply → handler recapture → intervening pwrite → client retry
  must return the first result and keep the pwrite's bytes. A second,
  distinct truncate to the same size with a new opid must truncate again.
- Encoding: the earlier 42 bytes plus `expect_base_size:8` plus the opid
  wire form (`EFS_OPID_WIRE_LEN` = 36): 86 bytes as sketched. The exact
  layout is set in the rewrite,
  validated with an exact-length check.

#### 2. Deterministic bounded sweep

- The scan reads only the target lane's shard prefix `(shard, ino, gen,
  lane)`, in key order from the cursor. It stops at whichever comes first:
  `SWEEP_SCAN_MAX` keys examined (proposed 256), 64 rows changed, or the
  batch item cap. It does not read the inode, wall time or the other group.
- A chunk key under a pending transaction intent stops the entry at that key
  with the cursor unchanged. A later entry retries, so a rewrite never runs
  over an uncommitted overlay.
- The cursor is `(target_epoch, next_ci)` on the lane stamp.
  - A command whose target is **below** the stamp's target is obsolete and
    is a no-op (OK).
  - A target **above** it restarts the cursor at 0 and adopts:
    `adopted = max(adopted, target)`.
  - An equal target continues from the cursor.
- **Behind the cursor, new-epoch publishes keep older parts' clips.** A
  span published onto a clipped base copies the base's `valid_len`
  unchanged, and a base replacement removes the old part outright. Publish
  apply never writes a larger `valid_len`, and a unit test asserts this.
- **Mixed rows:** each part is handled on its own. A fully dead base under
  live spans becomes an empty base: identity dropped, fragments sent to
  `SUPERSEDED` (packed records to the ledger). Dead spans are removed, and
  a span that crosses the boundary is clipped.

#### 3. Activation, bootstrap and SWEEP_DONE lifecycle

- **ACTIVATE** `(ino, gen, lane, writer_E)` runs on the inode group. Its
  apply refuses with STALE if `writer_E != content_epoch`. If the lane is
  already active it is a no-op OK that never marks anything done. A newly
  active lane records `activated_epoch = E` and is marked done for every
  pending entry with epoch ≤ E: a lane registered at E cannot hold older
  rows, provided the floor below holds.
- **BOOTSTRAP** creates the lane stamp on the lane group. It is proposed
  only after a ReadIndex read shows the lane active, and it carries the
  inode's current epoch at proposal time, which is ≥ `activated_epoch`.
  Apply creates the stamp with that `adopted` value, or raises an existing
  one with `max()`. It never lowers adoption and never touches the sweep
  cursor or target.
- **Floor:** a publish to a lane with no stamp answers BUSY; a publish
  below `adopted` answers STALE. A lane marked done at activation therefore
  rejects anything older than `activated_epoch`, which is ≥ the target.
- **Delayed bootstrap after retirement:** retirement of target T needs
  SWEEP_DONE (or done-at-activation) for every active lane. A sweep at
  target T creates or raises the stamp to ≥ T. A delayed bootstrap with an
  older epoch therefore hits an existing stamp and `max()` leaves it
  unchanged.
- **SWEEP_DONE** `(ino, gen, lane, T)` runs on the inode group. It is
  ignored (OK) if the generation differs, T is no longer a history entry
  (already retired or pruned), or the lane is not active. Otherwise it sets
  the lane's bit for T.
  - **The acknowledged property:** `adopted ≥ T`, and no part with
    epoch < T lies past its `valid_end`.
  - It survives every update a lane can receive: publishes at ≥ `adopted`
    add only parts at ≥ T; clip-preserving publishes keep the masks; higher
    sweeps only delete or clip more; bootstrap only raises `adopted`.
- **RETIRE(T)** removes every entry ≤ T once done ⊇ `active_lanes` for T.
  This is valid because the sweep for T carried all unretired entries ≤ T,
  so it masked them as well. Only committed apply state authorises
  retirement, never a handler's claim.

#### 4. Revision matching and progress

- I withdraw my earlier "the snapshot revision must equal the one recorded
  at adoption" rule. It would block progress after any retirement, which is
  the review's point.
- The carried snapshot belongs to its target T. It is valid for sweeping
  toward T if it contains T and every unretired entry below T. Pruning only
  drops entries that cannot lower a `valid_end`. Retiring an entry j < T
  happens only after every lane is swept for j, so no row that j would mask
  is left. Either way, a later snapshot that is missing j yields the same
  bounds for every remaining row, and no revision equality is needed.
- The lane apply checks only that the snapshot is well formed and that its
  newest entry is T.
- The full-history priority hint is in memory only. The committed
  truncate-pending key plus the inode history are enough to rediscover all
  pending sweeps after a restart.

#### Work order

Agreed. The writer-epoch source, coherent inode-authority views, the
ACTIVATE/BOOTSTRAP floor and the lane adoption table land together as one
gated change, before lazy truncate is enabled. Then TRUNCATE with opid
receipts, then sweep/SWEEP_DONE/RETIRE with packed-ledger release, and
finally removal of the destructive path. File-lane reduction audit first.
