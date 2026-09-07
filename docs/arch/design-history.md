# Design history

[Architecture](../architecture.md)

The normative specification is [../architecture.md](../architecture.md). It
states rules without narrating their discovery. This note records how it got
here — the review rounds and the mistakes they caught — so the reasoning
stays available without cluttering the spec.

## What was replaced

The previous metadata layer was a whole-table snapshot with copy-on-write
pages and a root two-phase commit. Its commit point was an
under-replicated coordinator/root generation, and the bug history of the
old system — a dual-writer root generation, peers pinned at an old committed
generation, roots adopted before their pages were local — was the system
repeatedly discovering that the commit point was not quorum-replicated.
That mechanism is deleted, not patched. The data-path mechanism
(client-direct RDMA, k+f EC) is unchanged; its commit semantics are what
the new architecture specifies.

## Sep 1 2026 — nine external review rounds

**Round 1 — specification blockers.** `shard_bits = 20` was an arithmetic
error (2³² objects / 4096 shards wants 12 bits). Inode IDs became 64-bit +
generation (ABA-safe handles). The dentry index became a small *projection*
`(ino, gen, type)` with the inode row as the only mutable copy. Cross-shard
rename/hardlink were shown to need a real Raft-backed transaction — the "no
2PC" claim was only ever about the old root-snapshot 2PC. I1–I4 were
restated as Raft's actual safety properties (election uniqueness, leader
completeness, term fencing, quorum acknowledgement) instead of "at most one
node believes it is leader." Reads became leader + ReadIndex with no clock
leases. Reconfiguration became joint consensus. Request idempotency via
op-IDs was added.

**Round 2 — POSIX composition.** Three (later four) governing principles
were written down (P1 never-serialize, P2 co-locate-what-commits-atomically,
P3 make-stale-harmless). The CREATE co-location rule was made explicit
(without it every CREATE is a distributed transaction). Hot-directory spread
became a layout-epoch protocol instead of a flag. Size became sharded
high-water lanes co-located with the chunk shard (I21). stat() got the
content-epoch double-check; truncate became a content-epoch bump (I22).
O_APPEND became reserve-then-distributed-write. Locking got one lock
authority per inode. File data became direct-I/O by decision. The simulator
was scoped to the *logical* data protocol. Data targets were made dumb.

**Round 3 — the hot-path contract.** EC publication was fixed to require
**all** k+f durable fragment ACKs before publication — the earlier "≥ 2 of
3 ACKs" rule had a real durability hole at f=1 (publish with 2 durable
fragments, lose 1 → 1 fragment < k=2 → acknowledged data lost). The FUSE
layer was named as an architectural serializer risk (parallel-direct-writes
et al. became requirements). Direct-I/O's loss of kernel readahead produced
the client prefetch requirement. Size lanes were bounded to L per file (a
4096-way fanout would have made stat() absurd). The multi-Raft runtime,
publication batching, the declared small-write envelope, QoS isolation, and
rate-limited distributed rebuild all entered the spec. "Durable ACK" was
defined as a persistent-NVMe operation, not RDMA completion.

**Round 4 — configurable failure tolerance.** f became a configured target
(1–3). The automatic guarantee was set at N ≥ max(2f+1, k+f). The user's
ratified 4-node/f=2 and 6-node/f=3 configurations survived only as an
explicitly labeled N=2f "durability-only mode" with an operator-gated
survivor-log-merge disaster-recovery protocol.

**Round 5 — structural fixes.** MKDIR was changed to scatter (files
co-locate with the parent directory; every new directory hashes onto its own
shard, buying an independently scalable subtree for one 2-shard transaction).
stat() became a double collect over monotonic `lane_seq` numbers (the epoch
check alone could assemble a size the file never had). Multi-chunk write
atomicity flipped: syscall-level atomic publication became the default
(I24), per-chunk visibility an explicit relaxed mode. O_APPEND gained the
serialized visible commit frontier. Client sessions became a real protocol
(I23).

**Round 6 — this revision.** The N=2f survivor-log merge was shown unsound:
selecting the highest-term entry independently per log index can synthesize
a command sequence no leader ever authorized and drop a *committed* entry
(commitment information dies with the lost majority). The mode was deleted
outright — N ≥ max(2f+1, k+f), RF = 2f+1, full stop. The FUSE
syscall-boundary problem (async-DIO splits a syscall without identifying
it) became an explicit unresolved contract with a MUST-NOT-claim, instead
of a claim that papered over the kernel interface. Write-publication
transactions got hash(txid) coordinator dispersion and parallel no-wait
PREPARE (they are on the hot path now). Directory spreading gained the
dominating hashed tombstone (no resurrection during SPLITTING, I8).
Session fencing was narrowed to metadata authorities — data targets do not
check sessions (orphan fragment PUTs are harmless by P3). Open-unlinked
gained the session-scoped open lease. Chunk identity gained
`coding_profile_id` for online f/k changes. The lane assignment became
round-robin within a file (`(chunk_index + hash(ino)) % L`) so sequential
read windows are one range request per lane. And the specification itself
was split: this file plus the satellites, leaving
[../architecture.md](../architecture.md) as the normative index.

**Round 7 — protocol closure.** The structure was accepted; this round only
closed remaining protocol holes, and every one of them was a place where an
earlier statement was true-sounding but did not compose.

*Session fencing was not actually a protocol.* "Leaders cache the session
table and re-validate on doubt" fences nothing: a shard that has not heard
about `epoch+1`, acting on its own cached view, can still commit for the
dead client — a direct I23 violation, and nothing ever tells that leader to
be in doubt. Replaced by a real revocation barrier: the session record
carries a touched-shard set, and `ACTIVE(E) → FENCING(E+1) → ACTIVE(E+1)`
only completes after every touched shard has durably acknowledged the fence.
Registration is once per shard per session, so the I/O path is untouched.

*ABA protection did not reach the data plane.* Handles were
`(ino, generation)` but chunk objects were keyed by bare `ino`, so a delayed
fragment PUT from the previous occupant of an ino could land in the new
object's key space. Every data-plane key is now scoped by
`FileID = (ino, inode_generation)`.

*Successor generation numbering was unsound.* Two concurrent writers reading
committed `G` would both mint "`G+1`" for different bytes and PUT different
content under one object identity — destroying the immutability everything
else rests on. Generations are now globally unique candidate identities, with
ordering supplied by the publication CAS instead of by the name.

*But the reviewer's companion suggestion — put `content_epoch` in the chunk
key — was wrong, and the spec already had that bug.* With the epoch in the
object identity, `truncate()` to a smaller non-zero size would strand every
surviving chunk under an epoch no reader consults (and the text said those
were GC'd), while re-tagging them instead turns one truncate into an
O(file-size) rewrite. POSIX requires the prefix to survive. The epoch is now
strictly a fence over lane state and in-flight publications; committed chunk
data survives a bump. That change exposed a second gap: with lanes
epoch-invalidated, `stat()` had nothing to read after a truncate, so the
inode row gained `base_size` alongside `base_mtime`.

*A derived `L` could never change.* `lane = f(chunk_index) % L` means
changing L relocates every chunk-map key of the file. Replaced by a fixed
64-lane space activated on use: an odd-stride permutation guarantees 64
*distinct* shards (independent hashing collides), lane 0 is the inode's own
shard so small files have no fan-out, and a monotonic `active_lanes` bitmap
bounds what `stat()` collects — at most 64 inode interactions over a file's
entire lifetime.

*The lane was about to become the next hotspot.* If a transaction's `MAX` on
lane size/mtime were an exclusive intent key, two writers publishing
*different* chunks that share a lane would conflict on nothing — the
per-file serializer moved down one level rather than removed. Transactions
now distinguish exclusive CAS keys from commutative reductions carried as
payload. Intent resolution also gained its fourth outcome: *cannot establish
authority* is a retryable error, never "absent" (I9 again).

*Timestamps had two hidden serializers.* A write updates ctime as well as
mtime, so routing write-ctime to the inode row would send every hot-file
writer back to the inode leader — write ctime moved into the lanes. Worse,
POSIX updates the **containing directory's** mtime/ctime on every entry
create/remove, so a spread directory would still funnel every create through
its home shard: hashed directories now keep per-dentry-shard `dir_lane`
times. And atime stopped being unspecified — `noatime` is the default,
`relatime` an option, strict per-read atime deliberately not offered.

*Two namespace cases were hiding behind "2 shards."* `rmdir` must prove
emptiness, which is distributed once a directory is hashed (now a
transactional read set, not a counter, which would rebuild the hotspot), and
same-directory `rename` is two-shard in a hashed directory because the two
names hash independently. Separately, directory rename's cycle check is
unsound if it merely walks the tree: two concurrent renames can each validate
a legal tree and together create an unreachable cycle. The destination's
whole ancestry now enters the transaction's versioned read set.

*Three hygiene items at scale.* Duplicate-suppression state became a bounded
window (watermark + completion bitmap + reply cache) instead of a permanent
row per operation, with a stated GC condition — as did transaction decision
records. Because targets are deliberately dumb, publication became the
validator of durability evidence against the *current* coding profile and
placement, so a stale client that durably wrote a full stripe to an obsolete
placement cannot publish it. And end-to-end fragment checksums were added
(I25): media corruption is not Byzantine behavior, and EC without integrity
checking will reconstruct confidently from a corrupt fragment.

*One performance claim was simply too strong.* "No physical NVMe sync per
logical operation on any hot path" contradicts "a returned `write()` is
durable" — some persistence boundary must be crossed. The contract now says
what was actually meant: boundaries are amortized to the largest batch the
externally visible semantics allow, never paid per chunk, per record, or per
Raft group when one could cover many.
### Round 8 — composition edge cases

Round 8 found no structural problem: the reviewer's verdict was that the
architecture is stable and the remaining work is where correct mechanisms
*compose*. Four of the findings were real bugs in the specification.

*Atomicity had only a write side.* I24 was satisfied by publishing every
chunk of a call under one decision — but a reader takes time too. Fetch
chunk A, let the writer's `{A,B}` decision commit, fetch chunk B, and the
read returns old A with new B: a state no serialization of those operations
produced, and POSIX makes `read` and `write` atomic *with respect to each
other*. Write-side atomicity cannot fix it. A multi-chunk read is now a
validated collect — the same shape as `stat()` — and, as a direct
consequence, the per-lane chunk-map prefetch windows are explicitly prefetch
rather than a cache: usable without revalidation only inside the read whose
linearization interval covers them.

*Committed reductions could briefly vanish.* Round 7 correctly made lane
`MAX`es commutative payload rather than exclusive keys. But it also said
they are applied "at resolve time", while a transaction becomes visible at
its *decision* — so between a durable COMMIT and the reducer running, a
`stat()` could read a size older than a write that had already returned.
"Not a lock" was quietly reading as "not visible". Pending reduction intents
are now discoverable under the lane's key prefix and fold into the
authoritative lane read; materialization is background compaction of state
that is already visible.

*Truncate had two holes, and the second was a data-resurrection bug.* First,
nothing distributed the new content epoch to the lane leaders that are
supposed to reject stale-epoch publications — and ordinary writes must never
consult the inode shard. Truncate now pays: a bounded fence over the inode
row plus the active lanes, ≤65 authorities, for an operation that is
inherently a file-wide serialization event. Second, "chunks beyond the new
size become unreferenced and are reclaimed lazily" meant the chunk-map
entries survived, so a later sub-chunk write into one of those chunks would
take the pre-truncate generation as its RMW base and bring back bytes a
successful `truncate()` had removed. Remembering the latest `base_size` does
not fix it either: across shrink→extend→shrink cycles, whether an old entry
survives depends on the smallest size imposed by any *newer* truncate, not
the most recent one.

The reviewer proposed logical range tombstones. The merged fix is simpler
and exact: the same fence performs a **per-lane range delete**. Because the
KV is ordered and lane `i` holds chunks `i, i+64, i+128, …`, the entries
beyond the new size are a contiguous key suffix — 64 range deletes, not 8M
key removals. The O(1)-ish shrink survives *and* nothing can be resurrected,
with no per-chunk epoch bookkeeping and no unbounded truncate history.

*`O_APPEND` was correct only against other appenders.* `append_eof` was an
independent counter on the inode shard, but an ordinary extending `pwrite`
publishes on a *lane* and never touches it — so after a write to offset
1 GiB, the next append reserved offset 0 and overwrote the file. EOF
reservation must serialize against everything that can move EOF. It now
validates against the active-lane EOF vector using the read-set machinery
`stat()` already has: an extending publication bumps its lane's `lane_seq`,
which aborts a racing reservation. Ordinary writers pay nothing; the
appender absorbs the retry, which is consistent with `O_APPEND` being the
one accepted same-file hotspot.

*Two generations, or one guard in the right place.* The reviewer showed that
filtering lane mtime *and* ctime through a single `mtime_gen` makes mtime
move backwards: a write stamps gen 7, a `chmod` bumps to gen 8, and the
write's mtime is filtered out. Their fix was to split `mtime_gen` from
`ctime_gen`. The merged fix removes the bug at its source instead: a
generation guard is only ever needed for a timestamp that can be set
*backwards*, and exactly one operation can do that — `utimens`. Every other
source of both times is a monotone "now", so plain `MAX` is already correct.
Only `utimens` bumps `mtime_gen`, only lane mtimes are guarded, and ctime
needs no generation at all — giving it one would have created a
backwards-moving ctime rather than prevented one. The same bounded fence as
truncate distributes `mtime_gen` to the lanes, so a write racing a `utimens`
is rejected and retried instead of having its timestamp silently dropped.

*Online `f`/`k` change could not prove its own result.* Re-stripe, verify,
flip `effective_f` lets a live writer publish a new old-profile generation
behind the scanner, after which the cluster claims a protection level the
current data does not have. The profile cutover is now committed **before**
the scan, and publication validation (round 7) enforces the current profile
from that moment — so the set of old-profile current generations can only
shrink, and the scan covers a set that cannot grow.

*Two ABA gaps and one ownership error.* Round 7 scoped every data object by
`FileID`; inode-scoped *ephemeral* state — open leases, byte-range locks,
append reservations — was still keyed by bare `ino`, so a delayed CLOSE or
UNLOCK could release a reused inode's state. All of it is `FileID`-keyed
now, and any operation acting through an inode handle validates the
generation first. Separately, a single `(client_uuid, session_epoch,
process_id)` owner models only classic `fcntl` locks: on Linux both `flock`
and OFD `fcntl` locks belong to the **open file description** and are shared
by duplicated and inherited descriptors, so ownership is now per namespace.

*Elapsed time cannot prove a straggler is gone.* Dedup GC was gated on a
"retention window", but the failure model allows unbounded message delay, so
a timer asserts something the network never promised. Reclamation now
follows a client **response-ack watermark**. This also fixed a subtler
requirement: a retried `O_APPEND` reservation must recover the *same offset*
from the reply cache — knowing that "something completed" would let the
retry allocate a second range and leave a permanent hole.

*Positioning.* With strict per-read atime deliberately unsupported and
syscall-level atomicity above the FUSE boundary still open, "a POSIX
filesystem" claims more than the contract delivers. The spec now describes
efs as a high-performance parallel filesystem *targeting Linux/POSIX
semantics with explicitly documented deviations*, and §3 lists them.
### Round 9 — protocol completion and kernel-interface reality

The verdict was that the architecture is stable and that what remained was
*protocol completion*, not redesign. Nothing about Raft sharding, fixed write
lanes, immutable generations, directory scatter/spread, session fencing or
the plane split was reopened.

*The normative matrix had gone stale against its own protocol section, and
§6 wins ties.* Round 8 made truncate a bounded fence over the inode row plus
active lanes, but §6 still described it as `content_epoch + base_size + times
on inode row, 1 shard, single Raft entry` — so by the spec's own precedence
rule the superseded design was the binding one. `SETATTR` had the same
problem in two ways: `utimens` now distributes `mtime_gen` through the lane
fence, and `SETATTR(size)` *is* truncate. This is the failure mode the
"never restate a normative table" rule exists to prevent, caught in the one
place a table is legitimately the single home for something.

*Read-set protocols were being used before they were defined.* Round 8 leaned
on read sets for `O_APPEND`, `RMDIR` emptiness, rename ancestry, and the
`stat`/read fallbacks, but §7.2 only had exclusive intents and commutative
reductions — so none of those had a stated mechanism. **Read/predicate
guards** are now the third transaction primitive: durable, shared, and held
*through the decision* rather than until the last check. `RMDIR` forced the
sharper form of the problem: emptiness is a claim about keys that do not
exist, and an insert into an observed-empty shard is a **phantom** that no
version check can detect. A per-shard `dentry_seq` bumped by every dentry
mutation gives the predicate a materialized witness — predicate isolation
without MVCC and without the distributed entry counter that would rebuild
the per-directory hotspot.

*Validated collects validated the wrong thing.* Checking key versions cannot
detect a transaction whose intents were already in place when the read began
and whose **decision** moved during it: resolve lane A as undecided (take the
old value), the transaction commits, resolve lane B as committed (take the
new value) — two states of one atomic transaction, with every version
unchanged. Collects now carry the set of txids they resolved as *undecided*
and re-check those. Because a decision is final, nothing already decided
needs re-checking, so the added cost is proportional to in-flight
transactions the read actually touched.

*`O_APPEND` was fixed only up to the reservation.* Round 8 closed the race
while determining EOF and left the interval after it open: an ordinary
extending `pwrite` could publish EOF 400 while an append's `[100,200)` was
still unresolved, a state no serialization explains. POSIX couples EOF
determination and the append write, so an **append barrier** now lives on the
active lanes while reservations are outstanding — taken once per append
burst, and constraining only publications that would push EOF past the
reservation watermark. Two smaller holes closed with it: the reserved length
is now defined as the length the client will attempt (reserving a *requested*
length only works if the append is length-faithful, or B appends after bytes
A never wrote), and a live client whose append fails resolves it itself as
`ABORTED_HOLE` — previously only a fenced client's reservation could resolve,
so a live failure blocked the frontier until someone killed the client.

*Truncate named its tail rewrite without composing it.* When the new size
falls inside a chunk, the bytes past it must be permanently forgotten and
must read as zeros if the file grows again. Publishing the tail after the
truncate commits leaves a window where the file is nominally shorter while
the old bytes remain readable. The zero-filled tail candidate is now written
first and **CAS-published inside the truncate transaction**; losing the CAS
retries the whole truncate.

*The profile cutover repeated the pre-round-7 fencing mistake.* A
control-plane Raft commit does not change what 4096 lane leaders believe, so
a leader still holding the old profile would keep accepting old-profile
publications after the "cutover" and the verification scan would be chasing a
growing set. The cutover is now a **barrier pushed to every publication
authority and durably ACKed**. Two related corrections: *lowering* f cannot
run the raise sequence backwards — the advertised guarantee must drop first,
or weaker data is written while the stronger guarantee is still promised —
and `u` is **protection debt, not a headcount**. A node returning restores
capacity, not the fragments it never received, so a degraded generation
consumes budget until repair actually rebuilds it; "unavailable" is likewise
a committed control-plane state, never a client's timeout.

*One factual Linux error.* Classic and OFD `fcntl` locks were described as
non-interacting namespaces. They are not: they differ in *ownership* and
release semantics but occupy **one record-lock conflict domain** and conflict
by byte range even within a single process on a single descriptor
(POSIX.1-2024 specifies this; Linux has behaved this way since 3.15).
`flock` is the genuinely separate domain. The round-8 ownership fix was
correct and is unchanged.

*The kernel is a serializer efs does not control.* Upstream Linux still takes
the inode lock exclusively for a direct write that extends `i_size` even with
`FOPEN_PARALLEL_DIRECT_WRITES`, forces it for `IOCB_APPEND`, and takes the
parent directory inode exclusively for `O_CREAT` — `FUSE_CAP_PARALLEL_DIROPS`
covers lookup and readdir only. These are *per-mount*, so 64 clients on 64
nodes are unaffected, but 64 ranks on one node sharing one mount serialize
before efs sees a request. It is now a second explicit unresolved
kernel-interface item next to syscall splitting, and §9 states the limit
rather than absorbing it into "efs scales".

*A hashed directory's fanout was not bounded.* Directory timestamp lanes were
claimed to be "bounded the same way a file's lanes are", but dentries hashed
over all 4096 shards, so a directory's used-shard set — and `stat(dir)` with
it — could reach 4096. A spread directory now uses the **same fixed 64-shard
permutation** as file lanes, which also makes its used-shard set a 64-bit
bitmap. Two consequences follow the file design exactly: first use of a lane
registers it on the parent shard (≤64 times per directory ever, so "the
parent shard is not involved" needed that exception), and directory
`utimens` needs its own `dir_mtime_gen` fence for the same backwards-time
reason a file's does.

*Smaller corrections.* "Only `utimens` can move a timestamp backwards"
assumed a monotone clock, which the failure model does not provide —
implicit updates are clamped by `MAX`, stated as a deliberate choice rather
than left as an assumption. Re-striping and repair must not touch
user-visible `size`/`mtime`/`ctime`; they change representation, not content.
The dedup ack watermark must be the highest **contiguous** acknowledged
reply, since out-of-order arrival would otherwise let reply 100 authorize
discarding 99's result — and a *fenced* session can never advance a
watermark at all, so its state is dropped wholesale after the revocation
barrier, with old-epoch requests rejected by epoch rather than by lookup.
L7 was broadened: truncate range-deletes and re-stripes create generations
that *were* published and are no longer reachable, which the old
"unpublished generations" wording did not cover.

## Sep 4 2026 — single-export decision

The old system supported multiple named exports (`efs-test` and `efs-s3`
coexisted on one cluster) because an export there was one slot in a static
array of in-memory tables. In the new architecture an export is a
replicated state-machine set (4096 Raft groups + a KV), so "create a
second export" is a real design decision, not an array index. The user
decided: **one export per cluster, hardcoded name `efs`**; the mount
target is `cluster:port:efs`; there is no create-export operation. The
escape hatch is preserved by construction: if a second filesystem is ever
required, it is one engine per export side by side — **never an
`export_id` in keys** — so the single-export key format is not a retrofit
trap. This closed the last open 10.5c design question. Production Raft
host in `efsd` (10.5c-9, env-gated `EFS_MD_RAFT`) is gated on a scratch
cluster; LOOKUP/GETATTR through ReadIndex + KV (10.5c-10) and file
CREATE as one Raft entry (10.5c-11) and MKDIR as a 2-shard txn
(10.5c-12) and last-link file UNLINK (10.5c-13) and mode/owner
SETATTR (10.5c-14) and empty LOCAL RMDIR (10.5c-15) and LINK
(10.5c-16) and nlink>1 UNLINK (10.5c-17) and utimens
(10.5c-18) and same-dir LOCAL file RENAME (10.5c-19) and
READDIR/LOOKUP_PATH (10.5c-20) and SETATTR SIZE / chunk-aligned
truncate (10.5c-21) and chunk publish + GETCHUNKS (10.5c-22) and
unaligned truncate tail CAS (10.5c-23) and cross-group propose
(10.5c-24, MKFS submit + inode bounce, no new opcode) and
O_APPEND reserve (10.5c-25, resolve-on-report) and SYMLINK as
CREATE S_IFLNK + publish (10.5c-26) and same-dir LOCAL directory
rename (10.5c-27) and HASHED dest CREATE (10.5c-28) and HOLD
open-unlinked leases (10.5c-29) are gated
on the same
smoke. What remains is cutover of the live table — not new
design. FLOCK and session fencing are later host items.
