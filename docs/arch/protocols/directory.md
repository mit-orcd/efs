# Namespace placement and directory spreading

[Architecture](../../architecture.md) · [Transactions](transactions.md) ·
[Data protocol](data.md)

This is the placement decision everything else hangs off. The authoritative
operation→participant matrix derived from these rules is
[§6 of the spec](../../architecture.md); this document is the placement
protocol itself.

## Placement rules

- **The inode row lives on `inode_shard(ino)`** ([§5 of the
  spec](../../architecture.md) defines the function; this document never
  restates placement formulas, it explains them).
- **A dentry lives on its parent's shard**
  (`dentry_shard = inode_shard(parent_ino)`), with a **threshold-based
  spread** for huge/hot directories (below).

**Why parent-shard dentries.** It makes the two dominant namespace ops
single-shard: `create`/`unlink` inside a directory touch only
`inode_shard(parent)` for the dentry. readdir is one ordered range scan on
one shard. The cost — a hot directory funnels to one leader — is real but
bounded, and it is the *right* trade because directory locality is the common
case.

**The two-index problem, resolved.** The full mutable inode row is **not**
stored in both indexes. The dentry index stores a **projection**:

```text
(parent_ino, name) -> (ino, generation, type)        // dentry: small, lookup hint
(ino)              -> authoritative inode_row        // inode: single source of truth
```

The dentry value carries only what LOOKUP needs to route and to validate a
handle (`ino`, `generation`, `type`). All mutable attributes (mode, uid, gid,
nlink, …) live **only** in the inode row — with two deliberate exceptions
defined in [the data protocol](data.md): *write-generated* size, mtime and
ctime live in the file's bounded write lanes (otherwise every write would
serialize
on the inode shard), and `stat()` merges them under the double-collect
snapshot protocol. Explicit attribute changes (`utimens`, `chmod`, `chown`,
truncate) always update the inode row. This eliminates any
chmod/chown/mtime/size/nlink divergence between the two indexes, and it makes
hardlinks clean: many `(parent,name)` keys point at one authoritative inode.

**Cost we accept:** a LOOKUP that needs attributes does a dentry get (parent
shard) then an inode get (inode shard) — two point-gets, possibly two shards.
That is the price of not duplicating mutable state, and it is paid only on
the attribute path, not the name-exists path.

## The CREATE co-location rule (load-bearing) — files local, directories scatter

Parent-shard dentries only make CREATE single-shard if the new inode lands
on the *same* shard as the dentry. So inode allocation is constrained by
placement (P2). But a naive "child inherits the parent's shard" rule has a
fatal consequence: a directory's children inherit its shard, *their* children
inherit it again, and an entire subtree funnels into one Raft leader until
each directory individually grows hot enough to spread — "many independent
directories → near-linear" would be false for exactly the common case. So
the rule distinguishes files from directories:

```text
CREATE regular file:
    normal directory:  inode_shard(new_ino) = inode_shard(parent_ino)
    spread directory:  inode_shard(new_ino) = dentry_shard(parent_ino, name)
                                              (the 64-shard permutation below)

MKDIR (any directory, always):
    inode_shard(new_dir)  = hash(parent_ino, name, export_salt) & 0xFFF
```

**Files are local; directories scatter.** A new file shares its directory's
shard, so file CREATE writes dentry + inode row in **one Raft entry on one
shard** — the dominant namespace op stays a single log append. A new
*directory* is deliberately placed on its own hash shard: MKDIR pays one
two-shard transaction (dentry on the parent shard, inode row on the home
shard, via [transactions](transactions.md)) and in return buys **an
independently scalable subtree for the directory's entire lifetime** —
`/projectA/*` and `/projectB/*` live on different shards from birth, not
after a spread event. Directory creation is rare next to file creation, so
the transaction cost is amortized to nothing; the parallelism it buys is
permanent. The per-export `salt` (chosen at mkfs) prevents correlated names
from clustering across exports.

The allocator picks an ino whose low 12 bits equal the required shard (inos
are allocated per-shard — §5 of the spec — so this is free: the shard's
allocator simply hands out its own inos).

**The stranded-set bound still holds, now for files.** A normal directory's
*files* keep their inode rows on the directory's shard forever — their ino
identity encodes the shard (`ino & 0xFFF`) and cannot migrate.
`EFS_DIR_SPREAD_MIN` is therefore a **scalability bound, not just a space
threshold**: it caps how many file rows can be permanently co-located on one
shard before the directory spreads. It is kept low enough that the stranded
set is irrelevant at scale (thousands of rows, never millions).

## Hot-directory spread is a layout-epoch protocol, not a flag

A *normal* directory deliberately serializes its creates on one shard leader
— a locality choice, and the right one (directory locality is the common
case) — which means P1 is satisfied by the *spread mechanism*, not by
pretending the conflict doesn't exist. The spread trigger therefore responds
to **actual serialization pressure, not just namespace size**:

```text
spread when:  entries > EFS_DIR_SPREAD_MIN
           OR sustained op rate / queue depth / latency on the
              directory's shard crosses a pressure threshold
```

The LOCAL inode row carries `nents`, a count of immediate children
(create/mkdir/link dest increment it; unlink/rmdir/a name leaving a directory
decrement it). Crossing `nents > EFS_DIR_SPREAD_MIN` commits `SPLITTING` on
that same parent-row PUT — one Raft entry, no extra round. After spread the
count is frozen; HASHED emptiness is the per-lane `dentry_seq` read set, not
a distributed counter (the spec rejected that). Runtime override:
`EFS_DIR_SPREAD_MIN` (tests). Pressure-triggered spread is specified; the
bound is not a number in the spec and is not invented here. Draining
SPLITTING leftovers is the background migrator (in-memory queue, GC-thread
pass on the host, opportunistic drain in the sim after a size-trigger
flip). `raft-dir migrate` plus finish stay as the idempotent operator
path.

(a 100-entry directory with 100,000 clients creating and unlinking never
crosses the size threshold but melts its leader — pressure-triggered spread
catches it). Spread is one-way initially: once HASHED, a directory does not
collapse back. And crossing the trigger does not flip a bit — a 100M-entry
directory cannot be re-partitioned atomically, and
lookups/creates/unlinks/readdir must keep working *during* the move. The
directory carries a **layout epoch**:

```text
dir.layout = LOCAL            all dentries on the parent shard
    |
    v  (threshold crossed; owner Raft-commits SPLITTING)
dir.layout = SPLITTING(e)     new writes go to the directory's 64 dentry
                              shards; a migrator moves existing dentries
                              idempotently, in batches
    |
    v  (all dentries moved; owner Raft-commits HASHED)
dir.layout = HASHED(e)        all dentries on the 64 dentry shards; old
                              local range is GC'd
```

Read/write rules during `SPLITTING`: **writes go only to the hashed
location**; **reads check the hashed location first, then the not-yet-moved
local range**; the migrator's moves are idempotent (a dentry already moved is
a no-op), so a crash mid-split resumes cleanly; readdir merges both. The
layout epoch `e` lets a client cache the layout and detect a stale decision.

**Mutations of not-yet-migrated entries use a dominating tombstone** — this
is what enforces I8 (no resurrection) during the split. "Writes go to the
hashed location" is not enough on its own: if `unlink(foo)` only wrote the
hashed side while the old local `foo` still exists, the migrator could later
copy the stale local `foo` into hashed storage and resurrect it. So a
mutation of an entry that has not migrated yet writes an authoritative
hashed record first:

```text
unlink(foo) while foo is still LOCAL-only:
    write HASHED(foo) = TOMBSTONE(layout_epoch = e)

lookup(foo):
    hashed record or tombstone exists  ->  it decides (tombstone => ENOENT);
                                           LOCAL is never consulted
migrator:
    sees TOMBSTONE(epoch >= e) for foo ->  does not copy LOCAL foo
eventually:
    old LOCAL foo is GC'd with the rest of the local range
```

A rename/move out of a not-yet-migrated entry likewise writes the
authoritative hashed overlay record (new name, or tombstone for the old
name) *before* the old local copy is allowed to disappear. Rule: during
`SPLITTING`, **the hashed side always wins over the local side**, for
records and tombstones alike.

## Directory timestamps must spread with the directory

Spreading the dentries is not enough on its own, and this is easy to miss:
POSIX says creating, removing or renaming an entry updates the **containing
directory's** mtime and ctime. If a create that lands on hash shard 77 then
has to bump the directory inode's mtime on shard 12, **every create in the
spread directory still funnels through shard 12** — the spread has been
defeated by a timestamp, and the hot-directory scaling claim is false. This
is precisely the failure mode P1 exists to catch.

The answer is the same shape as file write lanes ([data protocol](data.md)):

```text
LOCAL directory:
    dentry mutation and dir mtime/ctime are on the same shard already
    -> one Raft entry, nothing to do

HASHED directory:
    each dentry shard carries a dir_lane:
        { lane_seq, max_mtime, max_ctime }

    create/unlink/rename on hash shard S:
        { dentry mutation                     <- exclusive key
        ; MAX(dir_lane[S].max_mtime, now)     <- commutative reduction
        ; MAX(dir_lane[S].max_ctime, now) }   <- commutative reduction
        ONE Raft entry on S. The parent shard is not involved.

    stat(directory):
        mtime = MAX( inode.base_mtime, dir_lanes' max_mtime )
        ctime = MAX( inode.base_ctime, dir_lanes' max_ctime )
```

**For that to be bounded, the directory's shard set must be bounded — so a
spread directory uses a fixed 64-shard permutation, exactly like file
lanes.** Hashing names freely over all 4096 shards would let one directory's
used set grow to 4096, and `stat(dir)` with it; "bounded like a file's
lanes" would be wishful. A directory therefore has at most 64 dentry shards,
chosen by the same construction data.md uses for lanes — a name hashes to
one of 64 lanes, and the lane maps to a shard by the odd-stride permutation
defined in [§5/§7.3 of the spec](../../architecture.md) (odd stride over a
power-of-two shard count ⇒ 64 DISTINCT shards; lane 0 is the directory's own
inode shard):

64 independent Raft leaders is the same throughput budget deemed sufficient
for the hottest single file, and it makes the used-shard set a **64-bit
bitmap on the directory's inode row** — a bounded `stat()` collect, by
construction rather than by hope. Global spread is unchanged: different
directories get different strides and different home shards, so a million
directories still cover all 4096 shards.

**First use of a dir lane touches the parent shard — the one exception.**
"The parent shard is not involved" holds for steady state, not for the first
mutation to land on a given lane: that one registers the lane in the
directory's used-shard bitmap, carried inside the transaction it is already
part of, exactly like `active_lanes` activation for files. It happens at
most 64 times in a directory's lifetime, so it is not a rate-proportional
cost. Without it `stat(dir)` could miss a shard that holds a newer mtime.

**Directory `utimens` needs the same generation fence as a file's.** An
explicit backwards mtime on a directory cannot stick while older
`dir_lane.max_mtime` values are still visible to the reduction, so the
directory inode row carries `dir_mtime_gen`, only `utimens` bumps it, and
the bump is distributed to the used lanes by the same bounded fence
(≤65 authorities) — the directory analogue of the inode fence in
[data.md](data.md). As with files, ctime needs no generation.

The reduction is validated by the same double-collect protocol as
file `stat()`, and the reductions are transaction payload rather than
exclusive keys ([transactions.md](transactions.md)) — two creates on
different hash shards must not conflict merely because both touch the
directory's time.

Explicit changes (`utimens`, `chmod`, `chown` on the directory itself) go to
the inode row's `base_*` values under `mtime_gen`, exactly as for files.

## RMDIR and RENAME are not uniformly cheap once a directory is HASHED

The operation→participant matrix in the spec gives the common case; the
spread layout changes two entries, and the difference is worth stating
explicitly because both are easy to get wrong.

**RMDIR must prove emptiness, and on a HASHED directory emptiness is
distributed.** For a LOCAL directory, "no entries" is a single range check on
one shard, so RMDIR is the ordinary two-shard transaction (parent dentry +
directory inode row). For a HASHED directory the entries are spread over the
directory's hash shards, and a naive per-shard check races: shard A can be
observed empty, then get a create, while shard B is being checked. So:

```text
RMDIR on LOCAL dir:   2 shards (parent dentry + inode row)
RMDIR on HASHED dir:  parent dentry + inode row
                      + a transactional read over the directory's dentry
                        shards, whose emptiness is part of the transaction's
                        read set (conditional PREPARE; any concurrent create
                        invalidates it and the RMDIR retries or fails
                        ENOTEMPTY)
```

The alternative — maintaining an exact distributed entry count — would
re-create a per-directory counter hotspot on the very directory that was
spread to avoid one. RMDIR is rare; paying an O(hash-shard-count)
transaction for it is the right trade, and the operation is bounded because
the used-shard set is recorded on the directory inode.

**Same-directory RENAME is not always single-shard.** In a LOCAL directory,
old and new name share the parent's shard, so a simple rename is one entry.
In a HASHED directory `hash(parent, old)` and `hash(parent, new)` are
independent, so the ordinary case is a **two-shard transaction**, widening
further if the destination exists and its inode row lives elsewhere.

## Directory rename must not create a cycle

POSIX forbids moving a directory beneath itself: the source may not be an
ancestor of the destination. Checking this by walking the tree at the moment
of the rename is **not sufficient under concurrency** — two concurrent
renames can each validate against a tree that was legal when they read it,
and together produce a detached cycle that no single operation ever
authorized:

```text
initially:   /a  and  /b  are siblings
rename(/a -> /b/a)   validates: b is not under a     OK
rename(/b -> /a/b)   validates: a is not under b     OK
both commit  ->  a and b are now each other's ancestor, unreachable
```

The fix is to make the **entire ancestry predicate part of the transaction's
read set**, so the two renames above are forced to conflict. Every directory
inode row carries an authoritative `parent_dir` pointer with a
`parent_version` that changes on every reparent:

```text
directory rename(src, dst):
  1. walk dst's ancestors to the root, collecting
     (dir_id, parent_version) for every link in the chain
  2. fail EINVAL if src appears anywhere in that chain
  3. include the whole collected chain in the conditional PREPARE as
     read-set entries at those versions
  4. any concurrent rename that reparents a directory in the chain bumps
     its parent_version -> our prepare fails its version check
  5. abort and retry
```

That makes the check a real concurrency-controlled predicate rather than an
advisory glance. The cost is an O(depth) multi-shard transaction — acceptable
precisely because directory rename is rare, and unavoidable if the invariant
"the namespace is a tree" (I8's structural half) is to hold under concurrent
renames. Renaming a *file* is unaffected: files have no children, so no
ancestry predicate exists.
