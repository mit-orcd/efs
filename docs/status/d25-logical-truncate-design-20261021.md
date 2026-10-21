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

## Author response to the review — October 21

Response from the proposal's author to the review in `7f8ffc6a9`; checked
against devel `0e9df00a8`. Design text only; NOTRUN. Accepted points
change the design as stated below; the sections above stay as proposed and
will be rewritten once these points are settled.

**Encoding size.** Accepted: 1+8+8+8+8+8+1 = 42 bytes, not 48.

### Additional gap found while checking the review

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

### 1. Request identity and size classification — agree, with one claim kept

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

### 2. Coherent read image — agree on the mechanism, disagree on one reason

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

### 3. Sweep completion and retirement — agree; concrete model

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

### 4. Clipping and packed objects — agree, plus one correction to the review

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

### 5. Fail-closed decoding and full history — agree

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

### Other points

- **File-lane reductions:** agree this is a prerequisite audit, not a
  follow-up. It runs before any epoch-invariant code.
- **g1:** agree. Report server commit latency and whole-syscall latency
  separately, and test sparse and allocated 100 GiB files separately.
  Single-digit milliseconds is a target to measure, not a claim.
- **Order of work:** agree with the review's order, with the publish-epoch
  source fix (above) placed first, because every later step depends on it.


## Follow-up review of the author response — October 21

Reviewed `48c423434`; source/design inspection only, **runtime gates NOTRUN**.
The response substantially improves the design. Rewrite §§1–5 into one
normative protocol next; leaving contradictory original and response text is
not a safe implementation handoff. No backward compatibility is required.

### Disposition of the disputed points

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

### Remaining protocol details to specify

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
