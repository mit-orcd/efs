# W95 independent indexed-packing experiment

This specifies an experimental model for the existing storage-principles work.
It selects no production format, changes no EFS store and grants no permission
to wipe existing stores. Existing file/container measurements remain baselines;
performance comparisons must use equal durability and account for index writes.

The initial candidate uses immutable packed body segments and a bounded,
checksummed manifest snapshot. A manifest entry contains object identity,
generation, segment identity, offset, length, digest and live/tombstone state.
The object table has an explicit capacity. Tombstones retain generation identity;
recreation increments generation. Old conditional deletes cannot delete a later
version. Full manifest snapshots are intentionally a simple recovery baseline;
measure their write amplification before considering a journal optimization.

Publication order:

1. Reserve bounded body/index scratch capacity before creating a new segment.
2. Write immutable records containing object identity and generation as well as
   payload, and synchronize the segment and its parent directory.
3. Write a complete checksummed manifest replacement, synchronize it, atomically
   rename it over the old manifest, then synchronize the parent directory.
4. Acknowledge each cohort member only after that publication boundary.
5. Collect only segments unreferenced by the currently durable manifest, then
   synchronize directory deletion. No age-based deletion and no early reuse of
   a retired segment name.

A failure after publication starts makes the writer unusable until cold reopen;
it cannot guess whether the old or new manifest won. Recovery first validates
manifest geometry/checksum and every referenced live record's identity, range
and digest. Invalid or missing authority fails closed and preserves evidence.
Read-only recovery performs no repair or collection. A recovered writer first
synchronizes the validated manifest and directory: a process crash may have
exposed a rename before its publication directory barrier. Only then may a writer collect proven
unreferenced segments under exclusive ownership
of this private experimental root. This is not production session revocation.

Crash gates cover body write, body sync/directory durability, manifest write,
manifest sync, rename, publication directory sync, ACK and conditional GC.
Every acknowledged live generation must survive cold reopen. Unacknowledged
publication may resolve to either valid old or new state. A tombstoned version
must never reappear after acknowledged deletion. Corrupt manifest/body evidence,
wrong-generation deletion, quota exhaustion and repeated overwrite/delete/
recreate churn must preserve these properties and bound physical garbage.

The model must report logical live bytes, segment bytes, index bytes, allocated
blocks, garbage and actual reclaimed bytes separately. Snapshot/index bytes and
all required barriers belong in durable completion latency. Host-write savings
are not measured NAND endurance improvements.


The prototype exposes `--indexed` through the existing principles geometry,
worker barrier, latency and retained/cold harness. Initial timed support is
buffered packed allocation and read-only cold verification; unsupported direct,
unflushed, overwrite/delete harness modes are refused explicitly. Conditional
versioned overwrite/delete/recreate and GC are exercised by the independent
protocol gate and a separate comparative timed churn harness.

Admission budgets include both manifest copies and new body allocation, rounded
using the filesystem allocation unit (at least 4 KiB). Actual allocated blocks
are checked before manifest publication. This is an experimental admission
budget, not a kernel-enforced physical quota: an allocation anomaly may leave
one unpublished cohort requiring recovery/collection, but is never acknowledged
over budget. Live bytes, body bytes, index bytes and dead records inside a still
live shared segment are distinct. Snapshot write amplification is reported and
has not been selected as a production optimization.


Implemented independent model gates now cover 18 process-exit boundaries,
4,800 versioned churn operations, shared-segment live-neighbor protection,
stale deletes, checksum corruption, exclusive ownership, quota refusal and
read-only recovery without file or timestamp mutation. Directory inventory
errors fail closed rather than treating an incomplete scan as EOF. Geometry
integration covers 1, 17, 512 and 65,536-byte objects and exact index/body write
counters. Linux and Mac sanitizer gates pass.

The [36-sample comparison](../../results/measure/20261008-w93-w96-review/w95-indexed/README.md)
shows that individual indexed publications are slower, whereas cohorts of 32
are substantially faster for finite creation. This supports further experiments;
it does not establish sustained churn performance or choose a production format.


The [sustained churn gate](../../results/measure/20261008-w93-w96-review/w95-churn/README.md)
passed 24 AMD samples and 184,320 operations with read-only reopen generation
and byte verification. Mixed cases include 64-KiB objects among tiny objects.
GC and inventory cost belong to sustained throughput; publication latency ends
at its durability boundary. Individual indexed publications remain slower;
cohorts of 32 improve both tiny and mixed cases. This is a serial independent
model, not a multiclient EFS result. Peak allocation samples exclude transient
manifest replacement; pre-publication admission checks cover that separately.

Actual fault gates inject four publication fsync failures, ten partial-write/
ENOSPC boundaries, GC directory-sync failure and incomplete directory inventories.
A publication failure fences the writer until reopen; valid prior authority is
preserved. A scratch file lacking sufficient root identity is retained rather
than guessed safe to delete. Test syscall indirection exists only in a separate
test object; production and benchmark objects have no fault injection.
