# The data plane, specified precisely

**Implementation scope:** this page specifies the accepted design, not a
current deployment guarantee. The [public-path review](../../status/spec-implementation.md)
records persistence, session, fixed-profile/repair and integrity limitations;
consult those gates before claiming this contract is implemented.

[Architecture](../architecture.md) · [Transactions](transactions.md) ·
[Sessions](sessions.md) · [Failure tolerance](../failure-tolerance.md) ·
[Performance](../performance.md)

This is where "scales with raw hardware" is won or lost. The mechanism
(client-direct RDMA, EC) is unchanged from the current implementation; what
this protocol adds is a precise commit and concurrency protocol.

**Default coding follows D36's node-count contract.** Three nodes use 2+1;
four and larger clusters default to 2+2. The maximum protection target is two
node losses. Larger clusters add scale, not an automatic f=3 profile. Fragments
must occupy distinct node failure domains. Current production code is fixed
2+1; 2+2 and its repair/cutover gates remain W74 implementation work. Data coding
alone does not establish the required metadata preservation (§2 of the spec).
Any k fragments reconstruct the chunk, so reads survive any f
losses by construction. Writes place all k+f fragments; while nodes are
down, a write that cannot place its full stripe either blocks on repair or
commits a degraded stripe that is explicitly queued for re-striping — never
silently under-protected.

**Every persistent data identity is scoped by `FileID`, not by `ino`.** An
ino is reused after deletion, and the inode generation is what makes a
handle ABA-safe ([§5 of the spec](../architecture.md)) — so the data
plane must carry the same incarnation, or a delayed fragment PUT from the
*previous* occupant of ino 100 can collide with the new one:

```text
FileID = (ino, inode_generation)
```

`FileID` — never bare `ino` — keys fragment objects, chunk-map entries,
write lanes, append reservations, and publication intents.

**Immutable chunk generations.** Each logical chunk's fragments are keyed by

```text
(FileID, chunk_index, chunk_generation, fragment_index, coding_profile_id)
```

A generation is immutable once written. Reconstruction never mixes
generations (I13). Metadata publishes a generation only after it satisfies
EC durability (I14); an unpublished generation's fragments are orphans,
reclaimable without touching committed state (I15). This is what makes
stale-client writes safe: a stale client may write an orphan generation, but
it cannot make it visible without winning the metadata publication protocol
(I20).

**`chunk_generation` is a unique candidate identity, never `G+1`.** A
successor-numbering scheme is unsound with concurrent writers: two clients
that both read committed generation `G` would both mint "`G+1`" for
*different* bytes, and would then PUT **different content under the same
object identity** — destroying the immutability the whole design rests on.
So a candidate generation is globally unique by construction, and ordering
is supplied by the publication CAS rather than by the identifier:

```text
candidate_generation = H(client_uuid, session_epoch, op_id,
                         chunk_index, retry_attempt)      (or a 128-bit UUID)

publish:  CAS(expected = committed base generation,
              publish  = candidate_generation)
```

Two concurrent writers therefore build two *distinct* immutable objects;
exactly one wins the CAS, and the loser's object is an orphan it can reclaim
and retry over (P3). Successor notation (`G`, `G+1`) appears in this
document only as pseudocode for logical succession — it is never an
identifier.

**`content_epoch` is a fence, not part of the object identity.** It tags
*publication requests* and *lane state* so a truncate can invalidate both in
one single-shard commit (below), but it must **not** appear in a fragment
object's key. If it did, a `truncate()` to a smaller non-zero size — which
POSIX requires to preserve the surviving prefix — would strand every
surviving chunk under an epoch no reader consults, and re-tagging them
instead would turn one truncate into an O(file-size) rewrite (8M chunk-map
entries for a 1 TiB file). Uniqueness across epochs is already guaranteed by
`FileID` + the unique candidate generation, so the epoch has no identity
work left to do.

**`coding_profile_id` identifies the coding interpretation** — `{k, f,
stripe/coding epoch, placement epoch}` — because f and k change online
(see [failure-tolerance.md](../failure-tolerance.md)): the same
`(FileID, chunk_index, candidate_generation)` under profile 2+1 and under 2+2
would be ambiguous without it. Re-striping to a new profile is itself
generation-based (P3): reconstruct the chunk, write a **new immutable
generation** under the new profile, satisfy the new durability requirement,
publish it atomically, then GC the old generation. An existing generation's
coding interpretation is never mutated in place.

Legacy folded-span replay is separately limited by
[W88](../../backlog/work-items.md#w88): trailer tombstones can be evicted
before an ambiguous retry. Staged durable receipt/floor primitives are not
proof that the production REPORT path carries them.

**The publication state machine (a durability boundary).**

This runs at synchronization/publication, not at every ordinary buffered
`write()` return. Under the accepted consistency contract, `fsync`/last
`flush` and supported synchronous writes wait for this boundary. Current implementation and remaining
acceptance gates are W71/W73 in the linked scope review.

```text
1. allocate chunk generation G
2. encode + store fragments of G (client-direct RDMA to k+f nodes)
3. obtain ALL k+f durable fragment ACKs         (EC durability, see below)
4. Raft-commit the metadata publication of G  (chunk-map/size/mtime/ctime)
5. apply the publication
6. return successful synchronization/publication
```

Step 3 waits for **all** k+f fragments, not the fastest k: a generation
published with only D durable fragments survives f losses only if
D − f ≥ k, i.e. D = k+f. Publishing earlier would open a window where
killing the right f nodes loses an acknowledged write — a violation of I11,
not a performance trade-off. Write latency therefore tracks the slowest
fragment target, not the fastest k; that is the honest price of the
guarantee, and it is hidden by queue depth and pipelining
([performance.md](../performance.md)), never by weakening the durability
definition.

**"Durable ACK" is defined precisely, because a successful
durability boundary rests on it.** A durable fragment ACK
means the storage target has completed the required **persistent-NVMe
operation** for the fragment — an NVMe flush / FUA write, or a write into
power-loss-protected (PLP) media — *not* that the RDMA WRITE completed.
RDMA completion only reports on the RDMA operation; it says nothing about
the NVMe durability boundary. The same precision applies to Raft: a follower
ACK means the log entry has reached the persistence level efs defines as
durable (WAL on PLP media, or flushed/FUA), not that it sits in a volatile
write cache. If the deployment's SSDs have volatile write caches without
PLP, efs requires FUA/flush on the durability-critical writes; "returned
write == durable" is only as strong as this definition.

**Degraded publication.** There is one legitimate exception, and it is
precise: while the cluster is *already* operating with `u` unavailable
failure domains (`u ≤ f` failures already consumed), a generation may publish
with `D ≥ k + (f − u)` durable fragments — the invariant "survives the
remaining f−u further losses" is maintained throughout. At f=1 with one node
down that is D ≥ 2: the two surviving fragments suffice, because losing
another node would be a *second* simultaneous failure, outside the configured
model. Such a generation is marked **degraded** in its publication entry and
queued for repair; when capacity returns, the missing fragments are
re-striped and the degraded mark cleared (rebuild scheduling in
[performance.md](../performance.md)). Degraded publication is never used
just because a target is slow — only because it is unavailable.

A returned ordinary buffered `write()` is **not** durable or cross-client visible. Bytes
land in the client dcache; same-client read-your-writes hold. Publication
(the machine above) runs at `fsync`, each `flush`, or an `O_SYNC`/`O_DSYNC`
write callback. Mount `-o sync` remains specified with acceptance owed under W73. The stronger
"`write()` publishes" alternative was measured ([project-history.md](../../archive/project-history.md) "START-HERE closed items", W2) and
rejected: a peer sees none of an un-`fsync`ed 4 KiB write, and `kill -9`
of `efs-fuse` loses a 64 MiB acknowledged `write()`. `O_SYNC` is the
explicit write-through path, along with O_DSYNC; both callbacks publish before
replying. `fsync()` covers the client-side dirty set plus the publication quorum.
AMD direct/TCP callback, peer-before-close and process-crash gates pass; final
mode acceptance and hardware power-loss proof remain W73/W71 obligations.

## Partial-chunk and concurrent writers (same file)

This is the hot-file problem and it gets a real answer. The governing rule:
**never serialize work that the semantics and the hardware allow to run in
parallel** — the filesystem must not invent a conflict merely because two
writes share an inode.

- **Chunk metadata is itself sharded — into a bounded number of per-file
  write lanes.** This is the load-bearing detail, and it has two halves. If
  every chunk-map key for a file lived on `inode_shard(ino)`, then a million
  writers to disjoint chunks would still funnel through **one Raft group /
  one leader**, because Raft serializes the group's log — independent *keys*
  do not help when they share one *log*. But spreading one file's chunk
  metadata across all 4096 shards overshoots in the other direction: it would
  give a large file up to 4096 authoritative size lanes, and a linearizable
  `stat()` would need up to 4096 remote reads. The design point is the
  middle: **distribute enough to remove the bottleneck, not more** (P1+P2).

  **The lane count is fixed, and lanes are activated, not resized.** A
  tempting formulation — "derive L from the required publication rate" —
  does not survive contact with the key space: if `lane = f(chunk_index) % L`
  then changing L **relocates essentially every chunk-map key of the file**,
  so L could never adapt at runtime without a full file-layout migration
  protocol. Instead the lane space is a fixed maximum, and a file's *use* of
  it grows:

  The formulas themselves are defined once, in [§7.3 of the
  spec](../../architecture.md): a fixed `LMAX = 64` for every file, the lane
  is the chunk index modulo `LMAX`, and the lane's shard is the odd-stride
  permutation from the inode's shard. Two properties fall out of that
  formula, and both are load-bearing:

  - **The 64 lanes are guaranteed to land on 64 *distinct* shards.** Because
    the shard count is a power of two and `stride` is odd, `lane ↦ (base +
    lane·stride) mod 4096` is a permutation — so the mapping is injective
    over the 64 lanes. Independent hashing (`hash(ino, lane) & 0xFFF`) would
    not be: birthday collisions would silently give a hot file fewer
    effective leaders than it asked for.
  - **Lane 0 is the inode's own shard.** An ordinary small file's chunk map,
    size, times and inode row are therefore all on one shard: no fan-out at
    all for the overwhelmingly common case.

  Different files get different strides *and* different bases, so lane sets
  scatter across all 4096 shards and global balance is preserved.

  **The active-lane bitmap.** The inode row carries a 64-bit
  `active_lanes`. A lane counts only once it has been used:

  ```text
  first publication into a not-yet-active lane
      -> the publication transaction also sets that bit on the inode row
      -> monotonic; bits are never cleared
  stat() collects only the ACTIVE lanes
  ```

  So a one-chunk file has exactly one active lane and `stat()` is one read;
  a huge hot file ends up with 64 active lanes and 64 independent leaders.
  Activation costs at most **64 inode-row interactions over the entire
  lifetime of the file** — not one per write — and a multi-chunk write sets
  every bit it needs in the transaction it was already running, so a
  sequential writer activates the full set within its first few writes
  rather than in 64 separate round trips.

  A lane's shard carries, for every chunk assigned to it, the chunk-map
  entry **plus that lane's size, mtime and ctime high-water marks** — so
  chunk publication, size update and time updates for an extending write are
  **one Raft entry on one shard** (P2; this is what makes I21 hold with no
  cross-shard protocol):

  ```text
  { publish (FileID, epoch, chunk_index, candidate_generation)   <- CAS key
  ; MAX(lane.max_end,   end_offset)                              <- reduction
  ; MAX(lane.max_mtime, now)                                     <- reduction
  ; MAX(lane.max_ctime, now) }                                   <- reduction
  ```

  The distinction annotated above is not decoration — it is what keeps the
  lane from becoming the next bottleneck, and it is specified in
  [transactions.md](transactions.md): the chunk-map entry is an **exclusive
  CAS key**, while the three `MAX`es are **commutative reductions**. Two
  writers publishing *different* chunks that happen to share a lane must not
  conflict; if lane state were an exclusive intent key they would, and the
  inode hotspot would simply have moved down one level.

  Because lane i holds chunks i, i+64, i+128, …, a sequential window of a
  file is one contiguous range request per lane (read windows in
  [performance.md](../performance.md)), and the collect set for `stat()` is
  known from the bitmap without reading anything else. A disjoint full-chunk
  write publishes on its lane's shard — never on the inode's shard. The data
  is distributed *and* the metadata publication is distributed, up to 64
  leaders per file. This is what makes the hot-file claim structurally true
  rather than a slogan.
- **stat() is a distributed snapshot, not a bare MAX.** Reading `MAX` over
  several independently-linearizable shards is not automatically an atomic
  snapshot — and the content-epoch double-check alone only closes
  *truncate-vs-stat*, not *concurrent extending writes across lanes*: with
  lanes A=B=100, a stat that reads A=100, then sees writers commit A=1000
  and B=500, then reads B=500 would return 500 — a size the file never had.
  So each lane carries a monotonically increasing `lane_seq` (bumped by
  every lane mutation), and stat() is a **double collect**:

  ```text
  1. read inode row -> content_epoch E, mtime_gen M, active_lanes A,
                       base_size, base_mtime, base_ctime
  2. collect the lanes in A:  (lane_seq, max_end, max_mtime, max_ctime)
                              [entries tagged E], resolving any pending
                              reduction intents; record every txid that
                              resolved as UNDECIDED  -> the decision set D
  3. collect those lane_seq values again, and re-resolve D
  4. re-read inode row -> E, M, A unchanged?
  5. if every lane_seq is unchanged AND E/M/A unchanged AND every txid in D
     is still undecided:
         the lane vector existed simultaneously (all values held between the
         end of collect 1 and the start of collect 2) -> MAX is linearizable
     else: retry
  ```

  **Step 5's decision clause is load-bearing, and version checks alone
  cannot replace it.** A transaction's intents are written at PREPARE, so
  they are already present in the keys a reader inspects; what changes later
  is the *decision*, which lives somewhere else entirely. A reader that
  resolved lane A's intent as undecided (and so excluded it), and then —
  after the transaction commits — resolved lane B's intent as committed (and
  so included it), has mixed two different states of one atomic transaction,
  and every `lane_seq` it checks is unchanged, because nothing about the
  intents changed. Only the decisions moved. The saving grace is that a
  decision is **final**: undecided → {COMMIT, ABORT} happens once and never
  reverses, so the reader only has to re-check the txids it resolved as
  *undecided*; anything it saw already decided is stable by construction.
  That keeps the extra validation proportional to in-flight transactions
  the read actually touched, usually zero.

  The common path stays lock-free: `2·|A|+2` point reads, and `|A|` is 1 for
  an ordinary small file and at most 64 for the hottest. To keep a
  continuously-written file from starving the retry loop, the retry count is
  bounded (a few); on exhaustion stat() falls back to a **read-only
  multi-shard transaction** over the active lane set (the
  [transaction machinery](transactions.md): read intents on the lane set,
  one consistent read) — the correctness escape hatch, rare in practice.
  The same double collect validates the distributed times below. A collect
  that meets a prepared publication intent resolves it against the durable
  decision (below); it never treats an unreachable authority as "absent"
  (I9).
- **Timestamps have exact semantics — and none of them reintroduce the inode
  bottleneck.** POSIX has three, and a write touches two of them: it updates
  `mtime` *and* `ctime`. Routing write-generated `ctime` to the inode row
  would send every hot-file writer straight back to the inode leader and
  undo the lanes entirely, so **write-generated mtime and ctime both live in
  the lanes**; the lane leader stamps `max_mtime`/`max_ctime` on the
  publication entry and a million writers never touch `inode_shard(ino)`.

  **"Only `utimens` can move a timestamp backwards" is a property efs
  enforces, not one the clock provides.** POSIX says an implicitly updated
  timestamp takes the current time, and Linux `CLOCK_REALTIME` can step
  backwards (NTP correction, operator adjustment); the failure model does not
  assume synchronized or monotone clocks anywhere else, so it must not
  quietly assume one here. efs resolves this deliberately: implicit
  mtime/ctime updates are **clamped by `MAX`**, so a clock step cannot make a
  file's implicit timestamps go backwards, and only `utimens` — an explicit
  request to set a time — can. The cost is that after a backwards clock step
  an implicit timestamp may read slightly ahead of wall clock until the
  clock catches up. That is a documented semantic choice suited to HPC, not
  bit-exact wall-clock POSIX behavior, and it is why `mtime_gen` guards only
  the one operation that can genuinely regress a time.

  Explicit status changes remain inode-row events, with the POSIX
  distinction respected exactly — in particular **`chmod` updates ctime, not
  mtime**:

  ```text
  chmod / chown / link / unlink   -> ctime
  utimens                         -> the given times, plus ctime
  truncate                        -> mtime and ctime, plus content_epoch
  write                           -> mtime and ctime, in the lanes
  ```

  Each explicit change updates the inode row's `base_mtime`/`base_ctime`
  (and `base_size` for truncate). `stat()` computes, under the same
  double-collect validation:

  ```text
  size  = MAX( inode.base_size,
               active current-epoch lanes' effective max_end )
  mtime = MAX( inode.base_mtime,
               lanes' max_mtime where lane.mtime_gen == inode.mtime_gen )
  ctime = MAX( inode.base_ctime,
               lanes' max_ctime )                        <- no gen guard
  ```

  **Only `utimens` bumps `mtime_gen`, and nothing guards ctime.** Getting
  this wrong makes a timestamp move backwards. Suppose a generation counter
  guarded both times and every status change bumped it:

  ```text
  write at t=100     -> lane max_mtime = 100, stamped gen 7
  chmod at t=200     -> ctime := 200, counter := 8
  stat()             -> the lane's mtime=100 is now filtered out as stale
                     -> mtime falls back to base_mtime: it moved BACKWARDS
  ```

  A generation guard is only ever needed for a timestamp that can be set
  **backwards**, and exactly one operation can do that: `utimens`, on mtime
  (and atime). Every other source of both times is monotone "now" — `chmod`,
  `chown`, `link`, `unlink`, `truncate` and writes all move ctime forward —
  so plain `MAX` is already correct for them, and giving ctime a generation
  would create the bug rather than prevent one. Hence: `chmod`/`chown`/
  `link`/`unlink` bump no counter at all, `utimens` bumps `mtime_gen`, and
  ctime is an unguarded reduction.

  **`mtime_gen` reaches the lanes the same way `content_epoch` does** — by
  the bounded inode+active-lane fence (truncate, below). A lane stamps its
  `max_mtime` with the generation it has been fenced to, so a publication
  carrying a stale generation is rejected and retried rather than silently
  filtered out later; without that, a write racing a `utimens` could have
  its mtime dropped after returning success. `utimens` is rare, the fence is
  ≤65 authorities, and the write path still never reads the inode row.

  `base_size` is what makes truncate correct: after a shrink the lanes are
  epoch-invalidated and contribute nothing, so the inode's own value is the
  answer until new writes exceed it. So a `stat()` after a returned
  `write()` — from any client — sees mtime and ctime at least as new as that
  write, and an explicit `utimens` is honored exactly. This is defined efs
  behavior, not "where POSIX permits."
- **atime is an explicit policy choice, not an omission.** A strict POSIX
  access timestamp would turn every read into a metadata mutation — the
  precise opposite of the read path this architecture is built for, and the
  reason `relatime` is the Linux default and `noatime` is universal in HPC.
  efs therefore defines: **`noatime` semantics are the default** — reads do
  not update atime at all, exactly as Linux `noatime` means; the stored
  atime is whatever an explicit `utimens` (or the file's creation) last set,
  and it simply does not advance on access. `relatime` semantics — advance
  atime only when it precedes mtime/ctime, or
  is older than a coarse interval — are available as a mount option, paid for
  by a coalesced, best-effort inode update that is never on the read
  completion path. Strictly-conforming per-read atime is **not offered**; it
  is a scaling contradiction, and the deviation is documented in the POSIX
  contract rather than hidden.
- **Chunk publications are batched at the Raft layer.** Semantics stay
  per-chunk; physical persistence does not. At 128 KiB chunks, 100 GB/s of
  logical write bandwidth is ~800k chunk publications/s — one synchronous
  metadata transaction per chunk would make the metadata plane the ceiling
  long before the NICs or NVMe are. So the client partitions publication
  records by lane shard and the shard leader commits **many publications per
  Raft proposal**, with one durable group commit covering the batch (this
  composes with the multi-Raft runtime's group-committed WAL —
  [performance.md](../performance.md)). The per-chunk publication record,
  its CAS, and its lane updates are unchanged — only the physical commit is
  amortized.
- **One write syscall publishes atomically; the data movement does not
  serialize.** A `write()` spanning several chunks uses the
  [transaction machinery](transactions.md) for the *visibility decision
  only*:

  ```text
  chunk candidates (immutable generations)
       ↓  all written in parallel, client-direct RDMA
  per-lane publication INTENTS (prepared, not yet visible)
       ↓
  ONE durable write-txid commit decision (coordinator shard)
       ↓
  all intents become visible together
  ```

  Physical work — EC encode, fragment writes, even the per-lane intent
  prepares — stays parallel (P1); only the final visibility decision is
  atomic. A reader that meets an uncommitted intent resolves it against the
  durable decision record using the **four-state rule** of
  [transactions.md](transactions.md) — COMMIT → the new value, ABORT → the
  old value, NO-DECISION → the old value (the read linearizes before the
  eventual decision), and *cannot establish authority* → a distinct error,
  **never "absent"** (I9). Many writers to disjoint chunks of one file still
  run fully in parallel: their transactions touch disjoint keys and do not
  contend. The **relaxed mode** (§3 of the spec) skips the transaction and
  publishes per-chunk.

  **An atomic write decision is only half of I24 — the read side needs its
  own protocol.** Committing `{A, B}` as one decision does not stop a reader
  from seeing a mixture, because the *reader* takes time too:

  ```text
  reader fetches chunk A                     -> old A
       writer's {A,B} decision commits
  reader fetches chunk B                     -> new B
  reader returns old A + new B               -- a state no serialization
                                                of read and write produced
  ```

  POSIX requires `read`/`write` effects to be atomic *with respect to each
  other*, so this is a real violation, and no amount of write-side atomicity
  fixes it. A multi-chunk read is therefore a **validated collect**, exactly
  parallel in structure to `stat()`:

  ```text
  1. collect the chunk-map versions covering the requested range,
     resolving any publication intents; record the txids that resolved
     as UNDECIDED -> decision set D
  2. fetch the fragments in parallel (the expensive part)
  3. re-read those versions AND re-resolve D
  4. both unchanged -> return the data
     either moved   -> discard and retry (bounded)
     exhausted      -> read-only multi-shard transaction over the range
  ```

  Step 3 re-resolves `D` for exactly the reason `stat()` does: a writer's
  intents are in place before the read begins, so a read that spans a
  transaction's *decision* can splice old and new chunks while every version
  it checks stays put. Decisions are final, so only the undecided ones need
  re-checking.

  The revalidation is metadata-only and overlaps the data fetch, so the cost
  is one extra small round per read, not a serialization point; single-chunk
  reads (the overwhelming majority) need no validation at all, since one
  chunk-map read is already atomic.

  **This is also the rule that keeps read prefetch honest.** The per-lane
  chunk-map windows that make sequential reads fast
  ([performance.md](../performance.md)) are *prefetch*, not a cache: a
  prefetched chunk map may be used without revalidation only within the read
  operation whose linearization interval covers the prefetch. Carrying one
  across later reads would turn it into a stale authoritative cache and
  contradict immediate cross-client visibility (§3 of the spec).

  **The FUSE syscall boundary is an explicit unresolved contract.** POSIX
  requires syscall-level read/write atomicity, but with
  `FUSE_CAP_ASYNC_DIO` the kernel may split one large direct-I/O syscall
  into several smaller concurrent FUSE write requests, and FUSE does not
  tag requests with their originating syscall. So "POSIX-atomic at `write()`
  granularity" and "atomicity unit = one FUSE write request" cannot both be
  final claims. This is a **kernel-interface limitation, not an EFS protocol
  problem** — the machinery above publishes whatever unit the client hands
  it atomically. The contract:

  - The atomicity unit is **one FUSE write request**; `max_write`/
    `max_pages` are sized so the writes that matter arrive as one request.
  - **EFS MUST NOT claim full POSIX write atomicity above the FUSE
    request-size boundary until this is solved.**
  - Investigation paths, in order of preference: (1) kernel/FUSE support
    for operation grouping; (2) intentionally short-write at the supported
    atomic boundary so the kernel never splits a syscall; (3) a
    client/kernel extension carrying a syscall transaction ID;
    (4) disabling the relevant parallel FUSE behavior in a strict mode;
    (5) a clearly documented Linux/FUSE compatibility mode vs. strict
    POSIX mode.
- **Sub-chunk read-modify-write** uses generation CAS: a writer reads
  committed base generation `B`, builds a **unique candidate generation**
  from it (above — not `B+1`), and publishes with
  `CAS(expected_generation = B)`; on conflict it refetches the committed
  generation, reapplies its byte-range patch, mints a fresh candidate, and
  retries. The loser's candidate is a distinct immutable orphan, never a
  competing object under a shared name. Concurrent
  *disjoint* byte-range writes to the same chunk thus both land (I12), with
  no whole-file lock. The invariant is "every returned chunk is a valid
  serialization of committed byte-range writes," not "old whole chunk or one
  writer's whole chunk."
- **The small-write envelope is declared honestly.** A 4 KiB write inside a
  128 KiB chunk still stores a full chunk image (k+f fragments) — roughly
  **80× data-path amplification** for the logical 4 KiB. Disjoint writes
  do not share one generation CAS. A partial publish appends an
  **immutable span**: the object is a full chunk image, readers copy only
  `[off, len)`, and the base generation does not change, so concurrent
  disjoint spans do not STALE each other. Overlap, or a trailer already
  holding `EFS_CHUNK_DELTA_MAX` (8) spans, returns STALE. The client
  refetches and publishes one image that folds exactly the spans it
  read; `delta_base_n` and `delta_base_seq` must match, or that CAS
  STALEs. A fold that names a longer list than the image contains
  deletes a peer span. That client fold is the consolidation. There is
  no separate background span compactor. **efs stays optimized for
  HPC-sized, aligned I/O**; the span path removes the invented chunk
  conflict, and the bytes on disk stay a full chunk per span.
- **Truncate is a content-epoch bump, distributed by a bounded fence.**
  Truncate (or any wholesale content replacement) advances the inode's
  `content_epoch` and stamps the new authoritative `base_size`. But a bump
  recorded only on the inode row fences nothing: the lane leaders are the
  ones that must reject stale-epoch publications, and **ordinary writes must
  never consult the inode shard** — that is the whole point of the lanes. So
  the epoch has to be *pushed*, and truncate is the operation that pays:

  ```text
  truncate(FileID, S):
    transaction over { inode row } ∪ { active lanes }        (≤ 65 shards)
      inode:  content_epoch := E+1 ; base_size := S ; times := now
      lane:   accept E+1 (reject in-flight publications naming E)
              discard the lane's epoch-E size/time high-water marks
              RANGE-DELETE the lane's chunk-map entries beyond S
  ```

  The active-lane bitmap bounds the fence at 65 authorities, and truncate is
  inherently a file-wide serialization event, so it is the right operation to
  carry the cost. Rare ops pay; the hot path stays lane-local (P2).

  **The range delete is not an optimization — it is what stops truncated
  data from coming back.** Retaining chunk-map entries beyond the new size
  and reclaiming them lazily looks like an O(1) shrink, but it resurrects
  data on the next write:

  ```text
  chunk 100 = AAAA…               (published, 128 KiB)
  truncate(file, 0)               (lazy: chunk-map entry for 100 survives)
  pwrite(4 KiB @ chunk 100)       (sub-chunk RMW takes the OLD generation
                                   as its base)
  -> the 124 KiB of AAAA… around the new 4 KiB is back, after a truncate
     that returned success
  ```

  Remembering only the latest `base_size` does not fix it either: with
  shrink → extend → shrink cycles, whether an old entry survives depends on
  the *smallest* size any truncate newer than that entry imposed, not the
  most recent one. Deleting the entries is the rule that composes.

  It is also cheap, because the KV is **ordered** and a lane holds chunks
  `i, i+64, i+128, …` in chunk-index order: the entries beyond `S` are a
  contiguous key suffix, so each lane does **one range delete**, not one
  delete per chunk. Shrinking a 1 TiB file is 64 range deletes, not 8M key
  removals — the O(1)-ish shrink survives, and correctness with it. The
  fragments themselves are untouched: the orphaned generations are reclaimed
  lazily (L7), which is a space concern, not a correctness one.

  What the bump does **not** do is invalidate committed chunk data *below*
  the new size. Surviving entries are keyed by `FileID` and candidate
  generation, not by epoch (above), so the prefix POSIX requires to remain
  readable stays exactly where it was.

  **The straddling tail chunk is part of the truncate transaction, not a
  follow-up.** When `S` falls inside an existing chunk, the bytes from `S` to
  that chunk's end must be forgotten *permanently* — if the file later grows
  again, POSIX requires the extended region to read as zeros, not as
  whatever the old generation held there. A tail rewrite published after the
  truncate commits would leave a window where the file is nominally `S` bytes
  long but the tail chunk still carries the old bytes past `S`. So the
  rewrite is prepared first and committed *with* everything else:

  ```text
  truncate(FileID, S), S inside chunk C:
    1. read C's committed generation
    2. build a candidate: [0,S) preserved, [S, chunk_end) zeroed
    3. durably write its k+f fragments  (ordinary immutable PUT path)
    4. ONE transaction commits, atomically:
         inode:  content_epoch := E+1 ; base_size := S ; times
         lanes:  epoch fence + discard epoch-E marks
         lane(C): CAS-publish the new tail generation
                  (expected = the generation read in step 1)
         lanes:  RANGE-DELETE every chunk-map entry beyond C
  ```

  If the tail CAS loses to a concurrent writer, the whole truncate retries
  from step 1 — it never commits a size without the matching tail, and it
  never publishes a tail without the size. A chunk-aligned truncate skips
  steps 1–3 entirely. The rule for ordering against concurrent writes:

  ```text
  a write publishing under epoch E linearizes BEFORE the operation that
  advances the file from E to E+1;

  a write that begins after truncate has returned uses epoch E+1.
  ```

  A writer that read epoch 17 and publishes after the file moved to epoch 18
  has its publication rejected by the lane leader's epoch check, so no reader
  ever considers it — P3: the stale write is harmless, not prevented. Its
  candidate fragments are orphans (L7); old-epoch *lane* state is discarded
  with the bump.
- **Append replay results are durable.** The production ACK-bearing request
  carries UUID, epoch, sequence and a contiguous client acknowledgement.
  Its inode-shard receipt and reservation are one KV batch. Results bind the
  requested length and exact offset. At most sixteen unretired results occupy
  each inode/session window; exhaustion refuses admission before moving EOF.
  ACK retirement retains a durable replay floor. A retry whose exact result
  was retired fails STALE rather than fabricating offset zero. Receipt streams
  survive resize and are removed with the dead, unheld inode. Session-wide
  reclamation still depends on W72; a timer cannot authorize it. Older
  suffixes retain their legacy guarantees. See [the acceptance ledger](../../status/replay-readiness-20261008.md).
- **O_APPEND is a serialized reservation sequence feeding a parallel data
  path.** EOF allocation is the one same-file serialization the semantics
  truly require, and it lives on `inode_shard(ino)`:

  **A private append counter is not EOF, and using one is a correctness
  bug.** If the reservation authority kept its own watermark, an ordinary
  extending write — which publishes on a *lane*, not on the inode shard —
  would never advance it:

  ```text
  pwrite(offset = 1 GiB)      -> size is now 1 GiB+ (lane max_end)
                                 a private append_eof is still 0
  O_APPEND write: reserve()   -> returns offset 0
  -> the append overwrites the file's data instead of extending it
  ```

  So the reservation must be taken against the file's **real** EOF, and it
  must serialize against *everything* that can move EOF, not merely against
  other reservations. It does that with the read-set machinery `stat()`
  already uses — no fence, and **no new cost on the ordinary write path**:

  ```text
  append authority = inode_shard(ino)

  reserve_append(FileID, len):
    read set:  the active-lane EOF vector, validated by lane_seq
               (the stat() double collect, incl. pending committed
                reductions)
    eof     =  MAX(base_size, active lanes' effective max_end,
                   reservation_watermark)
    write:     reservation_watermark := eof + len
               record reservation (FileID, eof, len, session)
    commit iff no lane_seq in the read set changed  -> else retry
    return eof
  ```

  An extending publication bumps its lane's `lane_seq` — which it already
  does — so a concurrent reservation's validation fails and it retries: the
  reservation and the extending write serialize on one side or the other of
  EOF determination, which is exactly what `O_APPEND` requires. Ordinary
  writers pay nothing for this; the appender absorbs the retry, consistent
  with `O_APPEND` being the one accepted same-file hotspot. The watermark is
  a monotone allocation cursor derived from observed EOF, never an
  independent counter.

  A registered reservation whose inode, cursor, receipt, reservation and all
  active lane images belong to one participant shard may use
  `APPEND_ATOMIC` (command 53). The leader sends the exact captured images;
  This requires local leadership of that shard with multiple voters; follower
  coordinators retain forwarded PREPARE/DECIDE/RESOLVE to avoid redirect loops.
  Replicas validate their canonical receipt fold, established session floor,
  pending intents/shared guards and every old image before one atomic KV batch.
  No replica chooses its own EOF. Conflicts install nothing; storage failures
  retry the entry. Exact replay returns the original reservation without a
  new effect. Each input key’s transaction predicate version advances in
  the same batch, matching ordinary EXCL commit semantics; delayed pre-entry
  guards fail and exact replay advances neither images nor versions.
  Multi-shard reservations retain coordinated PREPARE/DECIDE/RESOLVE.
  All daemons must understand this command before it is admitted; older binaries
  cannot replay its entries. It does not change existing stored row formats.

  Published, registered append completion may use `APPEND_COMPLETE_ATOMIC`
  (command 54) when every active lane and completion dependency belongs to
  the inode shard. Its fixed body identifies inode, generation, reservation
  offset, reporting session UUID/epoch, content epoch and immutable reservation bytes. It allocates no offset. Serialized
  apply uses the existing completion collector, rechecks generation, local
  session floor and every intent/shared guard, then writes owned replacement
  images and predicate versions in one KV batch. Out-of-order completion
  still stops at the first unresolved range; DONE replay has no new effect.
  Missing active lanes refuse authority. A lane activation crossing shards
  between eligibility and apply returns BUSY with no effect; the unchanged
  REPORT retry uses normal coordinated completion. Local multi-voter
  leadership is required; follower coordinators retain the transaction path.
  All replicas must understand command 54 before admission.

  **Validating EOF once is not enough — the barrier must outlive the
  reservation.** POSIX couples EOF determination and the append write into
  one atomic step, so no intervening modification may fall between them.
  Validating at reservation time closes the race *while choosing* the
  offset, and leaves the interval afterwards open:

  ```text
  EOF = 100
  append A reserves [100,200)          -- A's data still in flight
  ordinary pwrite extends to 400       -- publishes lane.max_end = 400
  a reader now sees size 400 while A is unresolved
  -> no serialization explains it: pwrite-first means A should have
     taken 400; A-first means A's bytes must be visible before it
  ```

  So while a FileID has **unresolved reservations**, an *append barrier* is
  installed on its active lanes — the same bounded inode fence truncate uses
  (≤65 authorities), taken once when the reservation set becomes non-empty
  and released once when it drains, **per append burst, not per append**.
  Once the set drains, completion folds its frontier into inode `base_size`
  and removes the cursor. New-burst admission may retain fewer than eight
  completed reservation rows wholly below that captured inode floor. The
  next full page is retired in a bounded transaction before admission;
  records beyond the floor after shrink/reset still require immediate cleanup.
  An OPEN cursor retains its frontier dependencies. Exact inode/cursor guards
  reject concurrent completion or truncate races, and the independent durable
  receipt window and rejection floor continue to protect replay.

  Its rule at a lane leader is narrow, and it is a rule about EOF only:

  ```text
  publication that does not move max_end            -> unaffected
  publication within the reservation_watermark      -> unaffected
      (this includes the appenders' own reserved ranges)
  publication that would push EOF past the watermark
      -> must first advance the watermark on the inode shard,
         i.e. it orders against the outstanding reservations
  ```

  Disjoint and non-extending writes — nearly all concurrent traffic, and all
  of the data movement of the appenders themselves — continue in full
  parallel. Only the act of *extending past the outstanding append region*
  is ordered, which is precisely the operation `O_APPEND` cannot tolerate
  being reordered against.

  **Reservation length and short writes.** Reserving a requested length is
  only sound if the append is length-faithful, so efs fixes the length at the
  point where it is already known: the client holds the entire FUSE write
  request buffer before it reserves, and reserves exactly the length it will
  attempt. A shorter transfer therefore never arises from the usual cause
  (a partial device write); it can only arise from *failure*, and failure
  resolves as a committed zero hole rather than as a silent short append.
  efs does not hand back an offset for `len` and then write less than `len`
  while leaving the difference undefined — B's offset depends on A's length
  being real.

  The commit is one Raft mutation on the inode shard (the accepted
  hotspot). The client then writes its reserved `[eof, eof+len)` range
  through the **ordinary distributed chunk path** — chunk publications and
  size lanes spread across the file's lanes, so concurrent appenders
  serialize only on the *reservation*, never on the data movement:

  ```text
  reserve 10    reserve 11    reserve 12      (serial, inode shard)
  data 10  ─┐
  data 11  ─┼─ all run concurrently           (parallel, lane shards)
  data 12  ─┘
  visibility frontier advances over contiguous resolved reservations
  ```

  **The visible commit frontier is serialized — and must be.** O_APPEND is
  `(determine EOF; write)` as one atomic operation, so B's reserved offset
  only *means* anything because A's reservation was ordered before it. If A
  reserves `[0,100)` and B reserves `[100,200)`, B's data cannot become
  visible while A is unresolved: exposing B's bytes with A's range
  meaningless would expose a state no legal serialization produced. So the
  visible EOF is a **frontier that advances in reservation order** over the
  longest contiguous prefix of *resolved* reservations — B waits for A's
  resolution, not because of an implementation lock, but because append
  semantics order them. **Data movement is parallel; allocation order and
  the visible commit frontier are serialized.** That is serializing only
  what O_APPEND inherently requires.

  **Crash semantics, stated precisely.** The `reservation_watermark` is
  internal allocation state — it is *not* the visible EOF;
  the visible size advances only as the frontier crosses resolved
  reservations. Every reservation must eventually *resolve*, and there are
  three ways, not one — a live client that simply fails must not be able to
  block the frontier until somebody kills it:

  ```text
  COMPLETED       the reservation's publication commits
  ABORTED_HOLE    the owning client is alive and its append failed:
                  it commits the abort itself (for a lost reserve reply,
                  by abandoning the identity), and the reserved range
                  becomes a committed zero hole
  FENCED_HOLE     the owning session is fenced (sessions.md); recovery
                  commits the same zero hole on its behalf
  ```

  **Retirement after a lost reply is decided once (K15/W64).** A reserve
  whose reply never arrives leaves a committed OPEN reservation that its
  owner cannot complete, because the owner never learned the offset. Only
  two facts authorize retiring it; a timer is not one of them, because a
  live owner that did receive the reply may still be writing:

  ```text
  owner fenced     global revocation done and the shard's local rejection
                   floor is above the epoch -> cleanup-appends resolves its
                   OPEN rows as FENCED_HOLE (session cleanup)
  owner abandons   the live owner gives up on the write and sends an
                   abandon for that exact identity (uuid, epoch, seq, len)
                   under its established session -> the receipt result
                   becomes STALE and an OPEN row resolves as ABORTED_HOLE
  ```

  Both run as one exact-image transaction over the owner's session row,
  inode, receipt, reservation, cursor and lane barriers. The resolved row
  records its outcome (completed, aborted hole or fenced hole). The first
  resolution in the log wins. A retirement over a completed row is a no-op.
  A completion over a hole-retired row is refused STALE and never applied.
  A completion that is decided but not yet resolved holds its intents, so
  a concurrent retirement aborts and retries rather than overtaking it.
  Because the abandon retires the receipt, a delayed reserve or replay of
  that identity is refused STALE and can never allocate or learn the
  offset. A reserve that never applied is therefore never applied later.
  On the wire, the abandon is the ordinary APPEND request with
  `EFS_APPEND_ABANDON` set in `len`. If the abandon cannot be delivered,
  the client still releases the identity, and its later ACK forbids any
  new allocation under it. The stranded row then waits for session fencing
  or the last-close drain, which is the fail-closed backstop.

  The client is *required* to resolve a reservation it cannot complete;
  session fencing is the backstop for a client that stops participating
  altogether, not the ordinary path. Each resolution is a deliberate
  committed state, never an emergent one, and each unblocks the frontier for
  everyone queued behind it. The
  precise POSIX statement: a hole arises only from a write that never
  returned success, it reads as zeros (indistinguishable from a sparse
  region), and no returned write is ever lost. Reservations are never
  rolled back, because later appenders already received offsets past them.

### W52 serialized completion command — candidate implementation

The NUC candidate completes all reservations covered by an already committed
REPORT EOF in one inode-group `APPEND_CATCHUP` entry. It does not publish chunks
or recompute EOF from a replica's lane view. The host subsequently waits for a
guarded cross-shard transaction to retire lane barriers before replying to
REPORT. A partial publication retains the remaining barriers. Implementation
and outstanding live acceptance are recorded in the
[W52/NUC ledger](../../status/nuc-w52-w97-resume-20261011.md).

The internal Raft command uses big-endian integers:

| Byte offset | Field | Bytes |
| --- | --- | --- |
| 0 | Command tag, 52 | 1 |
| 1 | Inode | 8 |
| 9 | Inode generation | 8 |
| 17 | Content epoch | 8 |
| 25 | Previously published EOF | 8 |
| 33 | Captured outstanding-reservation allocation bound | 4 |
| 37 | Session present, exactly 0 or 1 | 1 |
| 38 | Client UUID, present only when the flag is 1 | 16 |
| 54 | Session epoch, present only when the flag is 1 | 4 |

Exactly 38 or 58 bytes are accepted, on the inode's Raft group only. Generation,
content epoch, session and intent checks precede mutation. Completion rows and
the cursor share one atomic KV apply scope with the applied marker. IO or
allocation failure discards the scope and retries the committed entry; logical
refusals advance the log with a failed verdict. A lost reply after completion
retries barrier retirement without another completion proposal.

The existing scope admits at most 32,766 outstanding reservations and scan rows.
An outstanding count above the captured bound returns BUSY for recapture; an
unsupported backlog returns OVERFLOW before mutation. There is no serial
per-reservation fallback or widened request timeout. All voters must understand
tag 52 before admission. No persistent row format changes, but an old binary
must not replay a log containing an uncheckpointed command it cannot decode.

## Packed small files (W125)

**The write path is unchanged; packing happens after close.** A file of at
most `EFS_PACK_THRESHOLD` (16 KiB) is written and published as per-file
chunk fragments like any file. Its chunk-zero publication also records the
file in its group's **candidate index** (KV kind 46, key epoch + FileID, the
epoch being leader time / 60 s, `EFS_PACK_EPOCH_S`), in the same batch.

**Packer (group leader).** For each led group the packer selects
candidates that are closed, quiet for the minimum age (60 s), linked,
single-chunk and not leased; groups them by (epoch, parent directory,
inode); reads their bytes (data fragment 0 of base and spans, or fragments
1 and 2 XORed); builds a segment (`packed_segment` v2: 96 B header, 72 B
per record, at most 512 records, records up to 16 KiB, per-record digests)
and encodes it 2+1.

**Admission, upload, switch:**
1. `PACK_ADMIT` (MD 73) stores the segment object (KV kind 45: ledger v2
   plus placement) with every record PENDING, before any fragment exists,
   and makes the store require reader capability PACK_V2 (older builds
   refuse the store).
   Placement is three distinct live members of the data ring keyed by the
   segment id (`efs_pack_destinations`), not the group's voters (D38): a
   segment may live entirely on nodes that host no metadata group.
2. `PUT_SEGMENT` (wire 157) writes each fragment to its node, which checks
   by a linearizable read that the object is admitted, ACTIVE and places
   this exact fragment there, and fsyncs file and directory before replying.
   A node that hosts the group reads after its own ReadIndex; any other node
   asks a voter with `PACK_OBJECT_READ` (wire 173/174: routed leader first,
   then each voter; the voter answers only if it hosts the group, after its
   ReadIndex). Any transport error, refusal or bad reply refuses the PUT;
   segment reclaim uses the same read and stays BUSY on failure.
3. `PACK_SWITCH` (MD 74) moves up to 128 files in one entry. Each member's
   source is the digest of its stored chunk-zero row; it switches only if
   the inode generation matches, the file is linked and unleased, the row
   still hashes to that digest, and the new reference carries the row's
   newest content epoch and a fresh record generation. Switched records
   become LIVE; the superseded per-file generation and spans become GC
   records; a replaced packed record is released. Other members are
   CANCELLED (dead bytes).
4. `PACK_CANCEL` (MD 75) abandons remaining PENDING records (failed copy,
   deposed packer).

**Ledger and reclaim.** Every path that drops or replaces a packed row
(overwrite, truncate, lane sweep, reap) releases its record in the same
batch, so live/dead bytes always match the mappings. A segment with no
live or pending record is RECLAIMING; the retirement pass hands it to every
member's reclaim journal (`GC_SEGMENT_BATCH`, wire 161; journal record kind
SEGMENT, fenced, deleted later) and `PACK_RETIRE` (MD 76) forgets it.
Superseded per-file fragments go to the journals in batches
(`GC_FRAG_BATCH`, wire 163; kind SUPERSEDED, checksum-guarded deletion
and removal of empty chunk and inode directories).

**Budget.** Packing runs at the idle rate (5,000 files/s per group) unless
the group is busy, meaning foreground commits exceed 100/s, measured over
windows of at least 1 s. While busy, it drops to a floor of 50 files/s and
ramps back towards the idle rate only under space pressure. Pressure is the
larger of two linear ramps:

- unpacked candidate bytes, estimated as backlog × 64 KiB, from 64 GiB to
  128 GiB;
- the highest member quota usage, from 50% to 80%.

The busy rate is `floor + (idle − floor) × pressure`. The backlog is
counted every 30 s. A pass reads its segments' files on 16 threads and
builds and switches its segments concurrently (`EFS_PACK_SEGMENTS_PER_PASS`, at most 32). Passes run back to
back while backlog and budget remain.

**Segment quota reserve.** `PUT_SEGMENT` may exceed a node's quota by
quota/64, clamped to 16–256 MiB. Packing frees more than it writes, since
each file's 69,632-byte fragments shrink to a share of a segment. Without
the reserve, a node at quota could never pack its way back under it.

**Compaction.** Compaction is scheduled maintenance, separate from packing
passes and from foreground I/O. It considers every ACTIVE segment that has
a hole, and underfilled segments (under 8 KiB live), which are merged
together. The whole segment index is scanned and the 64 highest-priority
candidates are kept. Priority is `0.5 × live fraction + 0.5 × coldness`:

- the live-fraction term clears tiny holes first;
- coldness is age over 24 h, capped at 1; compacted output counts as cold.

So a cluster left idle or read-only long enough ends with no holes. Live
records are read back and switched into cold segments, the same way
packing switches files. Compaction runs on three triggers:

- **Idle:** after 60 s without busy load, a batch of up to 256 output
  segments, capped at 10 s of work per batch, repeated every 60 s while
  idle. Reads are not commits, so a read-only cluster counts as idle.
- **Daily:** a UTC window (default 03:00 for 120 min) runs
  `segments_per_pass` per pass whenever the group is not busy.
- **Operator:** `efs-mgmt pack-compact <node:port> [segments]` (wire
  167/168) queues a request on every group the node leads and runs even
  while busy. With `0` it only prints the fragmentation summary.

Each leader logs a `packer: segments` fragmentation summary every 30 s:
segments, live, dead, holed, underfilled and compacted total.

**Backfill.** `PACK_INDEX` (MD 78) backfills the candidate index from a
leader scan of inode shards, for stores written before packing.

**Tunables** (efsd environment; defaults in parentheses):

| Variable | Meaning |
| --- | --- |
| `EFS_PACK_MIN_AGE_MS` (60000) | quiet time before a closed file is packed |
| `EFS_PACK_FILES_PER_S` (5000) | idle packing rate per group |
| `EFS_PACK_BUSY_FILES_PER_S` (50) | floor while busy |
| `EFS_PACK_BUSY_COMMITS_PER_S` (100) | foreground commit rate that counts as busy |
| `EFS_PACK_UNPACKED_GIB` (64) | unpacked bytes where pressure starts (full at 2×) |
| `EFS_PACK_SPACE_START_PCT` / `_FULL_PCT` (50 / 80) | quota-usage pressure ramp |
| `EFS_PACK_SEGMENTS_PER_PASS` (8) | segments per pass, and compaction per daily pass |
| `EFS_PACK_COMPACT_IDLE_S` (60) | idle time before idle compaction |
| `EFS_PACK_COMPACT_IDLE_SEGS` (256) | output segments per idle batch |
| `EFS_PACK_COMPACT_DAILY_START_MIN` (180) | daily window start, UTC minute of day |
| `EFS_PACK_BACKFILL_SHARDS` (64) | inode shards scanned per backfill step |

Built in since A6 (a set variable stops efsd): a 10 s work cap per idle
compaction batch, a 120 min daily window and full coldness at 86400 s
([inventory](../../status/a6-settings-inventory-20261024.md)).

**Reads.** GETCHUNKS returns a packed base with `EFS_CHUNK_REC_F_PACKED`
and the 96-byte reference; nodes and checksums are the segment's. The
client fetches any two segment fragments with `GET_SEGMENT` (wire 159),
hashes each, decodes, checks the record against the reference, zero-fills
past it and overlays spans. The scratch buffers come from the demand-read
allowance (below). Connections must first declare protocol 2
(`CLIENT_HELLO`, wire 165); the server answers GETCHUNKS and segment
messages only on declared connections, and a new client refuses to mount
daemons that do not accept the declaration.

**Read headroom.** Demand reads, including packed-segment scratch, may use
the client buffer pool's hard and drain budgets plus a read allowance
(`EFS_DCACHE_READ_BYTES`, 64 MiB). Dirty write data cannot hold that
allowance, so a heavy writer can no longer starve readers into
ENOMEM/EBUSY. A read that cannot get buffers waits until its deadline
(30 s when it has none) instead of failing at once.

**Modification.** Writes, appends and truncates of a packed file use the
ordinary write path: a full rewrite releases the record; spans over a
packed base are read together with it and re-index the file, which the
packer then repacks; a write that takes the file past the packing
threshold is never a span: the client folds chunk zero (packed base plus
the new bytes) into an ordinary per-file base, whose publication releases
the record, so no file above the threshold keeps a packed base under
spans; a truncate keeps a packed tail like a span chain (the size and
epoch fence hide bytes past EOF; extended by truncate, the file reads
zeros past the record). Hole punch is not supported for any file.

## Stale-target fencing — targets are dumb, metadata decides

Because clients write directly to data targets, Raft terms on the metadata
path do not fence the data path. But the fencing is *not* "target rejects an
older generation" — generations are immutable candidate objects, and two
concurrent clients may legitimately produce different candidate generations
for the same chunk with no global ordering between them. The correct model:

- A fragment object `(FileID, chunk_index, candidate_generation,
  fragment_index, coding_profile_id)` is **immutable and its PUT is
  idempotent**. A target stores it; storing the same object twice is a
  no-op. A target may hold several candidate generations of one chunk at
  once — that is fine, because they are distinct immutable objects.
- The target verifies only **placement epoch, target incarnation, and object
  identity** — i.e. "am I the current, legitimate home for this object?" It
  does **not** decide which candidate generation is logically newer, and it
  does **not** check client sessions (a fenced client's PUT just stores an
  orphan; publication is rejected on the metadata path — see
  [sessions.md](sessions.md)).
- **The metadata authority alone decides which generation is committed**
  (via the publication CAS). Unpublished candidates are orphans, reclaimed
  without touching committed state (I15).

This keeps the data nodes dumb — which fits the minimalist design — and it is
what makes I20 precise: a stale writer cannot make its generation *visible*,
because visibility is granted only by the metadata publication protocol, not
by anything a data target does.

### Publication validates the durability evidence

Dumb targets have a consequence that must be stated, or the model has a hole:
if nobody on the data path checks the coding and placement contract, then
**the metadata publication authority is the only place it can be checked**,
and it must actually do so. A stale client can hold an old placement view and
successfully write a full, durable stripe *to the wrong set of targets*.
Those PUTs are harmless in themselves (P3) — but the publication naming them
must be rejected, not accepted because "k+f ACKs arrived."

A publication therefore carries its evidence, and the lane leader validates
it before the entry is committed:

```text
publication = { FileID, content_epoch, chunk_index,
                candidate_generation,
                coding_profile_id, placement_epoch,
                durable_ack_set = [ { target incarnation,
                                      fragment role/index,
                                      object identity,
                                      fragment checksum } ... ] }

accept only if:
    coding_profile_id  == the file's current profile
    placement_epoch    == the current placement for those fragments
    the ack set covers exactly the k+f expected fragment roles
    those targets sit in the required number of distinct failure domains
    every ACK names THIS candidate object, not some other generation
    (degraded publication: the relaxed k+(f-u) rule above, and only then)
```

This is what lets targets stay dumb without weakening I11 or I14: the
contract is enforced once, by the authority that decides visibility, instead
of by every target on the fastest path.

### End-to-end integrity (I25)

The failure model excludes Byzantine *participants*, but silent media
corruption is not Byzantine behavior — it is ordinary SSD/DRAM/wire reality,
and erasure coding is specifically bad at it: EC can reconstruct from a
corrupt fragment and produce confidently wrong data, because the code has no
way to tell which input lied.

So every durable fragment carries a checksum over **its immutable identity
plus its payload** (identity included, so a correct fragment stored under the
wrong name cannot pass). The checksum is verified on read and before any use
as reconstruction input. A fragment that fails verification is treated as
**unavailable** — it is never fed to the decoder, the read is served by
reconstructing from other fragments, and the bad fragment is queued for
repair like any lost one. The immutable-generation design makes this cheap:
the checksum is computed once, at creation, and never has to be maintained.

This also gives the simulator ([verification.md](../verification.md)) a
natural corruption fault to inject, which is the only way the property is
ever actually tested.

#### Fragment record format 2 (implemented, W75)

Every fragment file a storage node writes, chunk fragments and packed-segment
fragments alike, is the payload followed by one fixed 128-byte **fragment
record** (`include/efs/frag_record.h`). Buffered roots append the record
directly after the payload. O_DIRECT roots append a 4 KiB tail page that
begins with the record and is zero-padded. There is no sidecar file. All
integers are big-endian.

```text
off  size  field
  0     8  magic "EFSFRAG\0"
  8     2  version = 2
 10     2  kind: 1 chunk fragment, 2 packed-segment fragment
 12     4  record bytes = 128
 16     4  space: export id (chunk) | publication group (segment)
 20     4  fragment index (0 = D1, 1 = D2, 2 = P)
 24     8  object: inode number (chunk) | segment id (segment)
 32     8  generation: chunk candidate generation | segment generation
 40     4  chunk index (0 for a segment)
 44     4  payload length
 48     4  coding profile = 1 (fixed 2+1)
 52     4  reserved, zero
 56    32  payload digest = BLAKE3(payload)
 88    32  bound digest   = BLAKE3(record bytes [0, 88))
120     8  reserved, zero
```

The bound digest covers the identity fields and the payload digest, so it is
a checksum over the immutable identity plus the payload, as I25 requires.
The payload digest is the value that travels on the wire and sits in the
chunk map, so clients, metadata and GC records are unchanged.

**Write.** The PUT handler and the ticketed upload rehash the received
payload before storage, as before. The store then builds the record from
the identity the request names and writes payload and record in one write.
An incomplete identity, such as generation 0, is refused, so no file is
ever written without a record.

**Read.** A GET names export, inode, chunk index, fragment index and
candidate generation. The storage node checks, in order:

1. The record is present and complete. A torn or missing record is
   `EFS_ERR_CHECKSUM`.
2. Magic, version, kind, record size and reserved fields are known. Anything
   else is `EFS_ERR_FRAG_FORMAT`.
3. The bound digest matches. A rotted record is `EFS_ERR_CHECKSUM`.
4. Every identity field equals the requested identity, including the export
   geometry's payload length. A mismatch is `EFS_ERR_FRAG_IDENTITY`.
5. For data fragments, BLAKE3 of the payload equals the payload digest. A
   mismatch is `EFS_ERR_CHECKSUM`. Metadata-table pages stop after step 4,
   because their payload integrity is the EFSR root checksums.

A refused fragment is never served. The node replies with GET status
`EFS_GET_CHUNK_INTEGRITY` and logs `store: I25 get refused fragment <path>:
<reason> field=<name>` (the first 256 refusals, then every 1024th). The
client logs `read: I25 node refused fragment` and treats the fragment as
unavailable. A read then decodes from the other two fragments, which pass the
same server-side checks and are compared with the chunk-map digests captured
by the reader. The node's check ends at its reply buffer, so the client
hashes every received fragment payload (BLAKE3) and compares it with the
reply's digest before the fragment is decoded: TCP checksums and RDMA's ICRC
do not cover host memory or the receive path. There is no switch
([D42 audit](../../status/read-verify-audit-20261024.md)). Two refused fragments fail the read with EIO. Nothing is
repaired automatically (W74).

**GC.** The checksum-conditional delete reads the record, verifies identity
and format, and compares the payload digest with the GC record's digest. A
file whose record has an unknown format, another identity or a bad bound
digest is neither deleted nor acknowledged. The GC record stays pending and
the refusal is logged. Whole-inode reclaim of a dead inode removes the
inode's directory without per-file checks, because it removes everything
under that FileID.

**Packed segments.** Segment fragment files use the same record with kind 2.
Segment GET, a retried segment PUT that finds an existing file, and segment
reads all verify the record against `(group, segment id, segment generation,
fragment index, length)` before comparing digests.

**Restart after an unclean stop.** Verified recovery (below) reads every
fragment file whole and proves its record and payload digest; it replaced the
report-only scan of recently modified files.

**Identity fields not in the record.** `content_epoch` is a fence, not
object identity (see above), so it is not bound. The inode generation and
the export salt are not bound either. Neither GET, the GC executor nor the
packer receives them today. Fragment GETs carry only `(export, ino, chunk,
fragment, candidate generation)`, and the client read path does not hold
the inode generation. FileID separation comes from the candidate generation:
it is BLAKE3 of a 128-bit per-process random object UUID, a sequence, and the
chunk index. A fragment of a previous inode incarnation or of an earlier
filesystem therefore carries a different generation and is refused as an
identity mismatch. Binding the inode generation and the salt directly needs
both fields on the GET and GC wire and in the client read path. That is a
separate follow-up.

**No earlier format is read.** The storage root marker (`.efs-engine`) now
records root format 1, meaning fragment record format 2. At startup a root
whose marker names format 0, the payload-only 32-byte digest tail, is
refused, and so is an unmarked root whose `data/` directory is not empty. The error
names the format, for example `storage: <root> has root format 0 (fragment
format 1: payload-only digest tail); this build reads only root format 1
...; refusing startup, reformat the root`. There is no migration. A single
old-format file found inside a format-1 root is refused per file as
`EFS_ERR_FRAG_FORMAT`, or as `EFS_ERR_CHECKSUM` when its 32-byte tail is too
short to hold a record.

### Verified recovery (K36)

A crash, `SIGKILL`, forced exit (rc 3) or power loss leaves every storage
root of the node "recovery uncertain": PUT acknowledgement is asynchronous,
so fragments acknowledged after the last durable barrier may be missing or
torn. The node fences COMMIT (`fsync` and DATA_COMMIT return `EIO`), across
orderly restarts, until a recovery thread (`src/server/recovery.c`) proves
the store; decision D42, finding F23. A restart, a sampled check or a forced
sync never clears the fence. The phases, each shown in
`efs-mgmt sync-status` and the log:

1. **identity**: every root's durability record names this boot (previous
   boot + 1) as running under this process's verifier; the member store
   advanced the incarnation before anything was served.
2. **journal**: the reclaim journal was verified and replayed at startup, or
   rebuilt by its own recovery thread after a failed verification.
3. **ledgers**: every storage session certificate (the K13 PUT floor) reads
   back intact; `*.tmp.*` leftovers of an interrupted seal are removed.
4. **fragments**: every chunk and segment fragment file is read whole, under
   the lock a PUT of that object holds. Its record must name exactly the
   object its file name names, and BLAKE3 of the payload must equal the
   record's digest. A torn, misnamed or corrupt file is dropped (unlinked,
   directory synced, quota credited). A name that is not a fragment name is
   counted and left alone.
5. **raft**: each metadata group the node hosts answers a ReadIndex from this
   node, which returns only once the local replica applied everything its
   leader had committed. A replica whose apply missed a committed row is
   resynced from the leader's snapshot first (as `raft-resync`).
6. **tickets**: every ADMITTED ticket (K13) placing a fragment here whose
   fragment is not proven here is remembered; it may still be published.
7. **metadata**: every published object (chunk base and live span) placing a
   fragment here is checked against the published digest. The inventory is
   read page by page (`EFS_RECOVERY_SCAN_PAGE` records) from the local KV
   after the ReadIndex, or with `EFS_MSG_RECOVERY_SCAN` (243/244) from a member
   hosting a group this node does not host. A missing or mismatching fragment
   is dropped and its stripe gets a W154 degraded mark through Raft
   (`EFS_MD_CMD_RECOVERY_MARK`, tag 248): the mark is written only while the
   chunk row still references exactly that object; an identical mark is
   left as it is; a mark for another fragment of the same object is never
   overwritten (a second loss the 2+1 code cannot rebuild, counted
   `unrecoverable`). Every ACTIVE packed segment holding switched files that
   places a fragment here is checked the same way; one not proven is dropped
   and counted `segments missing`: its two peers still serve it, and W154 has
   no segment rebuild.
8. **fence**: every root's fence clears and COMMIT works again.

After the fence the thread waits for each remembered ticket to leave
ADMITTED (a published one is marked as in 7) and counts the marked fragments
the group leaders rebuild here (`rebuilt`, `rebuild_waiting`). Nothing a
client was promised depends on that tail: the PUT verifier changed with the
unclean boot, so a client that held acknowledgements of the previous boot
keeps failing `fsync` (an error it saw never turns into success), and a
crash before the tail finishes is caught by the next recovery's inventory.

A transient obstacle (no leader yet, a peer unreachable) is retried and shown
as the reason. A failed verification (an unreadable certificate, a fragment
that cannot be removed, a row that does not decode) leaves the node fenced
with `recovery FAILED` and the reason until a restart verifies it. Every step
is idempotent, so a crash during recovery leaves the record unclean and the
next start recovers again. A single node that never formed a cluster cannot
prove its groups and stays fenced. Full power-loss qualification is a
separate scope decision.

