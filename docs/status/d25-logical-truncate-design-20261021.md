# D25 logical truncate — single-entry design (proposal for review)

October 21, 2026. Branch `d25-logical-truncate` from devel `0e9df00a8`.
**Design only; not implemented; NOTRUN.** Linked items: D25 / W43 / W24
([contract](../backlog/decision-contracts.md#d25--logical-truncate),
[register](decisions.md)). Replaces the declined 256 MiB EBUSY refusal
(`fe2e13624`) and the 65-participant coordinated truncate on
`devel-w123-recovery` (`1e7835d8a`).

## Problem

`host_truncate` (`src/server/raft_host.c`) proposes one or more
`LANE_FENCE` entries per cross-group lane, each of which **deletes** chunk
rows past the new size, and only then the inode-group `TRUNCATE` entry. A
refusal of the final entry (BUSY with append reservations, NOMEM, leader
change) cannot roll the deletes back: EOF stays old, contents are gone
(reproduced: `git show devel-w123-recovery:docs/status/w123-refused-truncate-20261020.md`).
The client adds a second non-atomic step: it zero-fills and publishes the
retained tail chunk before SETATTR (`efs_client_truncate`, `src/client/ops.c`).

## Direction (user, Oct 21)

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

## 1. Entry encoding

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

## 2. Read-path validity

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

## 3. In-flight publishes and lane fences

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

## 4. GC and reclaim of superseded fragments

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

## 5. Crash and replay

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

## Format changes (fresh format only)

TRUNCATE entry layout; chunk-row per-part `valid_len`; lane stamp gains
`swept_epoch` (fenced → adopted); inode history keeps the existing
`CONTENT_FENCE` value under `EFS_META_FENCE_INODE`; new `TRUNC_SWEEP`,
`FENCE_RETIRE` commands and truncate-pending key kind; per-lane history
sidecars, `LANE_AUTHORITY` and `LANE_FENCE` removed. Unknown or removed
encodings fail closed.

## Gates (private node9901 cluster, ASan with `detect_leaks=0`)

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

## Open questions for review

1. Extend keeps the D25 rule (size only, no epoch). Alternative: always
   bump — simpler, but grows the history on repeated grow. Recommend keep.
2. Boundary rows are clipped in metadata (`valid_len`) instead of
   materialised. This holds one partial object per boundary chunk. OK?
3. History stays bounded at 32 with pruning and BUSY when full.
4. `architecture.md` §7.3 still says the epoch is "pushed by a bounded
   fence over the inode row plus the active lanes". It changes to lazy
   adoption with the inode as the single history authority; the D25
   contract row and register line will be updated with the implementation.


## Independent technical review — October 21

Reviewed proposal/source `06098fa98` on `d25-logical-truncate`. **Design review
only; all new gates NOTRUN.** I support one inode-group commit for the logical
size/fence and asynchronous physical reclamation. Fresh format only; no legacy
replay, migration or downgrade machinery is requested. The following are
correctness requirements to settle before calling this implementation-ready.

### 1. Request identity and size classification — blocking

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

### 2. Read authority must be a coherent image — blocking

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

### 3. Sweep completion and retirement evidence — blocking

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

### 4. Clipping and packed-object lifetime — blocking

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

### 5. Fail-closed encoding and bounded-history progress — blocking

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

### Scope and implementation order

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
