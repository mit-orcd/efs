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

## Sep 1 2026 — six external review rounds

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
