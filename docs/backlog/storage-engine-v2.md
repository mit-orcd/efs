# Storage engine v2 and XFS optimization proposal

[Backlog](README.md) · [Parked ideas](ideas.md) · [Current queue](../status/README.md) ·
[Architecture](../how-it-works/architecture.md) · [GC checkpoint](../status/gc-implementation-20261007.md)

Recorded Oct 7, 2026 following the AMD storage benchmark and design discussion.
The user wants these ideas preserved and clarifies that `efs-bench` should test
storage principles with workloads closely relevant to `efsd` I/O and payloads.
The benchmark experiments are independent of the production storage engine;
measured improvements are subsequently implemented and validated in `efsd`. This is a proposed
direction, not an accepted architecture, a new queue priority, or production
rollout authorization. Active correctness work and the normative contracts
continue to govern implementation.

The recommended sequence is to model the current I/O workload in independent
benchmark drivers, compare individual principles under controlled conditions,
and combine successful ideas into an experimental v2 design. Apply demonstrated
improvements to `efsd` separately, with production correctness and performance
validation. No shared storage-engine implementation is required. Consider a
raw-device experiment only if measurements show a remaining filesystem bottleneck.

## Existing engine and benchmark baseline

The current production backend stores fragment bodies in files beneath inode
directories. `src/server/store.c` implements physical reads and writes;
`src/server/store_nvme.c` binds them to `include/efs/store.h`. The writer pool
handles writes. Direct-format bodies carry an aligned checksum tail; buffered
bodies have a smaller checksum tail. The store vtable is a starting seam for a
second backend, not evidence that recovery, flush and inode-GC interfaces are
already sufficient.

Two current reclamation paths provide workload shapes for separate principle
experiments; reproducing their full production implementation is not required:

- Conditional fragment GC locks the object, opens/stats it, reads the stored
  checksum, preserves a mismatch, unlinks a matching object, updates accounting,
  and syncs the containing directory before successful acknowledgement.
- Whole dead-inode GC persists a death fence against late PUTs, walks fragment
  directories with bounded work, unlinks bodies, syncs directories, and removes
  empty scaffolding. The current per-call budget is 128 entries, including
  traversed child directories, not a guarantee of 128 removed fragments.

`src/server/raft_host.c` coordinates lane sweeps, local/remote deletion and
durable retirement using a background GC thread. The fragment pass has a
200 ms per-group work budget. Local physical deletion throughput does not
measure this coordinator, metadata consensus, network retries or open-file
retention. Keep those layers distinct.

W71 records that production fragment acknowledgements lack a persistence barrier.
The existing unflushed benchmark is useful for characterizing that behavior,
but is not a durable-write baseline. Compare equivalent persistence semantics
and label current-behavior versus durable cases explicitly. W75 integrity
findings and W87 buffered append acceptance remain relevant; do not assume
healthy throughput implies correctness.

### AMD measurement reference

The Oct 7 run copied the local working tree, including uncommitted changes, into
`amd_efs:/home/efs/git/efs-bench/` and built on that host. It did not use or replace
the separate `/home/efs/git/efs` services. Build ID: `ddc677ae4bf3-dirty`.
The local tree evolved afterward, so retained sources define the measured build.

Command:

```sh
./efs-bench.sh --binary /home/efs/git/efs-bench/efs-bench \
  --storage-root /data1/efs/bench \
  --output /home/efs/git/efs-bench/logs/amd-data1-20261007
```

All 201 workload cases and profile reports passed; runner exit status was zero.
Raw I/O was synchronous `pread`/`pwrite`, one file per worker, with 4 KiB,
64 KiB and 1 MiB blocks; 1/16/64/256 workers; a 256 MiB total window; and
3-second timed phases. QD here counts synchronous workers, not requests on one
asynchronous submission queue. Reads were populated first; writes compared
allocation and prepared overwrite. Final validation and flushing were outside
the main raw-I/O clock; throughput came from unprofiled baselines.

| Best observed direct I/O | Write GiB/s | Read GiB/s |
|---|---:|---:|
| Raw file I/O | 0.915 | 0.879 |
| Raw file I/O with BLAKE3 | 0.917 | 0.855 |
| Local EFS data engine | 0.805 | 0.687 |

These are selected peaks, not sustained or power-loss-durable guarantees.
Buffered results reflect a repeatedly accessed small cached window. Engine
QD increases beyond saturation raised latency; auto writers at QD256 had
read p99 about 307 ms. Standalone 1 MiB BLAKE3 measured about 9.67 GiB/s
with one thread and 159.73 GiB/s with 32 threads.

Full evidence, executable and `sources.zip` remain in the remote output path
above. Existing host services were running. The auxiliary raw ceiling fallback
included the sequential write flush and is a different workload; its random
read case completed zero operations and is unusable despite the main matrix
passing. Preserve raw evidence when importing these numbers elsewhere.

## XFS and device opportunities

Observed `/data1` configuration on Oct 7: XFS on `/dev/sda`, 4 KiB blocks,
32 allocation groups, internal journal, `reflink=1`, `rmapbt=1`, `inode64`,
`relatime`, `logbufs=8`, and `logbsize=32k`. There is no realtime device.
The body drive identifies as GIGABYTE `GP-ASM2NE6200TTTD`, a 2 TB AORUS
Gen4 SSD, behind a Realtek RTL9210B USB bridge using UAS at 10 Gbit/s.

The SSD's advertised native sequential speeds are up to 5,000 MB/s read and
4,400 MB/s write. The negotiated USB link limits this configuration: about
1,212 MB/s after line encoding and before protocol overhead. Measured raw
direct I/O was about 944 MB/s read and 982 MB/s write. A new layout cannot
remove that link limit.

Linux exposed zero discard maximum and granularity. `fstrim.timer` was enabled
and active, but this device cannot be trimmed through the currently exposed
stack. Investigate bridge/driver/device support; this observation does not
establish that the underlying SSD lacks TRIM. Do not apply discard changes or
firmware changes based solely on this proposal.

| Idea | Intended use and qualification |
|---|---|
| Internal noatime | Suppress host access-time writes on internal fragment/container files. EFS user metadata has separate semantics. Current relatime already limits updates. |
| Explicit preallocation | Reserve container extents with `fallocate`, without an application zero-fill. Measure initial unwritten-extent conversion separately from populated overwrites. |
| Extent-size hints | Apply per-file or inherited directory hints to containers. Test, for example, 1 GiB containers and 1–8 MiB hints; these sizes are hypotheses. Large hints on small fragment files can waste capacity. |
| Hole punching | Release aligned, sufficiently large dead ranges without copying live bodies. Small frequent punches can fragment extent maps and add metadata work. |
| Reflink | Avoid payload copies for actual local cloning use cases. Shared extents trigger later COW and delay physical reclamation; enabled reflink does not make every write COW. |
| Filestream allocator | Test selected container directories for allocation locality/concurrency. It is not NAND placement or a guaranteed SSD improvement. |
| Batched parent sync | Delete bounded groups sharing a directory, sync the directory, then acknowledge those completed deletes. Preserve retry and crash semantics. |
| Periodic TRIM | Prefer evaluating scheduled `fstrim` to continuous discard once supported. Trim marks unneeded logical blocks; it is not immediate physical erasure or proof of reduced wear. |
| Journal tuning | Benchmark larger valid `logbsize` values against 32 KiB on metadata-heavy loads. Eight buffers are already configured; no demonstrated endurance gain is claimed. |
| External XFS log | Separate filesystem journal traffic onto another suitable device if contention warrants it. This moves writes; it does not eliminate them. |
| XFS realtime device | Explore putting selected file bodies on a separate realtime device while inode/directory metadata remains on the main device. Requires provisioning, kernel/tool compatibility and recovery testing. |

Do not apply blanket large `allocsize`: it changes buffered EOF preallocation,
overrides XFS's adaptive behavior, and is not a generic direct-I/O tuning knob.
Do not infer physical NAND alignment from XFS extents or reported logical
sectors. Keep filesystem free-space headroom; any SSD-GC benefit also depends
on discard delivery or device provisioning. Preserve metadata checksums,
ordering and required persistence barriers rather than trading them for numbers.

## Proposed storage engine v2

One candidate experiment uses large XFS files containing immutable fragment
records and a small index mapping object identity to container, offset, length
and checksum. Compare this with a file-per-fragment workload model rather than
committing to containers before measuring. The eventual production design must
preserve export/FileID, inode generation, chunk generation, fragment index and
coding identity as required by the authoritative protocols. Checksums are
integrity evidence, not substitutes for ownership or liveness authority.

Reads resolve an indexed location and issue aligned I/O. Writes reserve space,
append records, and group persistence where the contract allows it. Separate
short-lived/unpublished objects, replacement-heavy data and long-lived data
when the workload provides reliable lifetime information. Physical packing
alone does not ensure the SSD places those groups in separate NAND blocks.

Deletion first durably retires a reference. Reclamation can then choose among
hole punching, reusing safe holes, deleting a fully dead container, and moving
survivors out of a partially live container. Compare fixed-slot containers
against append/cleaning layouts: random slot reuse can reduce copying but
changes locality, fragmentation and device-write behavior. Do not assume an
append-only design always reduces total write amplification.

Keep namespace metadata, Raft/KV files and storage-index checkpoints on XFS.
A later raw-device backend could store bodies in reserved segments on a
dedicated device or partition. Raw access still goes through a normal SSD's
FTL; it does not confer NAND erase-block control. FDP or ZNS requires suitable
hardware and software support, neither established for the AMD USB device.
SPDK Blobstore is an alternative foundation to evaluate, not a selected dependency.

For eventual integration into `efsd`, the backend contract must cover
reserve/put/get, durable completion, conditional
retirement, fencing, reclamation, accounting, recovery and shutdown. Whole-inode
GC currently reaches into the file layout, so swapping the get/put vtable alone
will not make a second backend production-ready. Keep storage modules small
and testable through explicit headers instead of embedding another engine in
the server monolith.

### Required recovery invariants

1. Persist bodies before publishing durable location references or successful
   durable completion. A crash between these steps can leave collectible
   garbage, never a published missing body.
2. Durably retire references before releasing or reusing their space. Preserve
   outstanding reads and snapshot/checkpoint references through reclamation.
3. Relocation persists the new body, durably switches the index, and only then
   retires the old location. Concurrent readers need stable locations/pins.
4. Recovery detects torn/incomplete records and reconciles body records with
   the index using epochs/generations and checksums. An XFS journal does not
   make metadata and raw-device writes a cross-device atomic transaction.
5. Preserve inode death fences, generation/conditional-delete checks, durable
   retry ledgers, quota reconciliation and all-required-member acknowledgement.
6. Unpublished objects on a live inode cannot be collected by age or a local
   fence. Preserve D31's authoritative revocation/ticket requirements; staged
   metadata is not an integrated production collection mechanism.

## Proposed efs bench extension

`efs-bench` is a laboratory for principles relevant to the production workload.
Independent benchmark implementations are appropriate: they should make the
hypothesis and I/O costs visible without pulling in the production engine,
Raft, sessions or GC authority machinery. Existing production-linked `data`
and `meta` modes may remain optional diagnostic references; extending them is
not a prerequisite for the proposed experiments.

Model what matters to the experiment: fragment/delta sizes, checksum work and
tails, file/directory geometry, allocation versus overwrite, access locality,
concurrency, lifetime distribution, persistence policy and device topology.
Publish those assumptions with every result and explain differences from
production. Match the relevant I/O semantics, not every internal implementation
choice or incidental source structure.

### Implemented opt in principle suite

`efs-bench.sh --storage-principles` selects independent benchmark drivers rather
than production storage/GC functions. The default suite uses 64 KiB payloads,
4,096 total objects per case, QD 1/16/64/256 and batch size 32. The existing
normal benchmark matrix remains the default without this option.

```sh
make efs-bench
./efs-bench.sh --storage-principles --storage-root /data1/efs/bench --dry-run
# Short functional pass; not a performance or endurance conclusion.
./efs-bench.sh --storage-principles --storage-root /data1/efs/bench \
  --principles-sizes 4K --principles-objects 32 --qds 1,2 --no-perf
# More representative payload/fixture, with separate profiling reruns.
./efs-bench.sh --storage-principles --storage-root /data1/efs/bench \
  --principles-sizes 64K --principles-objects 4096 --qds 1,16 \
  --principles-batch 32
```

Each QD/size combination has 46 cases: file-per-fragment versus one container
per worker; buffered/direct body formats; verified reads; allocating versus
preallocated/populated overwrite writes; and conditional file unlink, container
hole punching or whole-container release. `--persist none|each|batch` is selected
in the matrix for modifying operations. Checksum-tail lookup precedes unlink and
hole punching. Whole-container release models a known entirely dead container
and skips per-record checksum reads. It closes the unlinked container before
completion so its open descriptor does not retain allocation.

These are finite one-pass workloads: `--principles-objects` controls the fixture,
not `--time` or `--data-size`. Default object counts represent 256 MiB of payload
plus tails per case. Setup creates deterministic nonzero data, checksums and
per-worker directories outside timing. Buffered fragment files use 32-byte
checksum tails; direct fragment files and all containers use aligned 4 KiB
tails. Container alignment is required for whole-block hole punching and is
reported, not hidden in a claim of identical allocated-byte geometry. Delete
digest lookups use buffered reads regardless of the body format.

`none` measures acceptance; any write allocation flush is outside its clock.
`each` includes file/directory persistence per operation. `batch` includes the
necessary file flushes and directory sync at each worker's batch boundary;
it cannot turn separate fragment-file data flushes into one flush. Containers
can share a single file flush across records. Intermediate batch operations do
not individually imply durable completion; wall throughput includes the final
batch barrier. Reported operation latency includes sync at batch boundaries.
Whole-container release latency is per container, while `ops` counts represented
fragment objects; `latency_samples` records that distinction.

Delete payload GiB/s counts represented payload retired, not transferred bytes.
`allocated_before`, `allocated_after` and `reclaimed_allocated_bytes` use file
`st_blocks` across the fixture; they exclude directory/journal overhead and do
not measure NAND erasure or TRIM. All records are checked after the run; every
read checks its stored hash against independently prepared expected hashes.
A failed operation, missing object, zero-work worker or verification error
invalidates the case. The harness creates and removes only private children
of the supplied scratch parents. Baseline, perf and optional strace are separate
fixtures; perf-control excludes setup and final verification from CPU sampling.

Validation on an isolated `amd_efs` checkout at
`/home/efs/git/efs-principles-validation-20261007` passed all 46 cases with 4 KiB
payloads, eight objects and QD2, both unprofiled and with separate perf runs and
reports. Evidence is under `logs/smoke` and `logs/perf-smoke` in that checkout.
Linux tests cover both layouts, all policies/persistence modes, both record
formats, multi-root and uneven worker assignment, caller-data preservation,
sync reduction, and injected checksum corruption/worker sync failure. Eighteen
harness tests passed. These are functional/profile validations, not measured
production speedups or SSD endurance results.

The first implementation isolates layout, allocation, checksum-tail I/O and
synchronization/reclamation costs. It does not yet implement mixed churn,
extent-hint sweeps, selective live/dead container cleaning, production fencing,
authoritative GC or a deployable v2 backend. One container/directory per worker
is an explicit experimental geometry, not a claim to reproduce all `efsd`
directory layouts. The broader workload and integration gates below remain.

### Existing experimental workflow

The [Oct 6 allocation and multi-location checkpoint](../../results/measure/20261006-bench-allocation-multiroot/SUMMARY.md)
compares initially allocating writes with populated, flushed, preallocated
windows, and compares individual roots with combined roots at fixed total QD
and working-set size. The [performance guide](../how-it-works/performance.md#separating-io-from-checksum-cost)
also separates raw I/O, I/O plus BLAKE3, and CPU-only hashing. These isolate
principles without changing production format or policy.

The [Oct 7 hot-path review](../../results/measure/20261007-bench-hot-path-review/SUMMARY.md)
records the next step: measurements and profiles motivated a production KV
lookup improvement, with matched before/after evidence and correctness tests.
Writer-policy changes were withheld where combined profiles did not establish
the cause. Follow that experiment, evidence, integration, validation workflow.

### Measurement layers

| Layer | What it establishes |
|---|---|
| Current I/O workload model | Independent file-per-fragment read/write/delete drivers with EFS-relevant sizes, tails, layout and persistence; a documented reference shape. |
| Single-principle experiments | Matched variations of allocation, batching, checksum placement, queueing, directory synchronization, extent hints or hole punching. |
| Combined v2 experiment | A benchmark-only combination of successful ideas; measures interactions, cleaning cost and foreground latency without claiming production readiness. |
| Production integration validation | Separately apply selected improvements to `efsd`, then test correctness, recovery and actual read/write/reclamation performance. |

For example, compare checksum-tail read plus unlink plus per-file parent sync
against checksum-tail read plus unlink plus bounded per-directory sync batches.
Compare file-per-fragment deletion with aligned hole punching and whole-container
release. Use the same live/dead populations and persistence requirement; report
both delete acceptance and return of usable space. Production ownership/fencing
remains an integration requirement, not a requirement to embed its GC in every
microbenchmark.

### Workload and evidence requirements

- Populate allocated, nonzero, verified data outside the delete clock. Record
  extent/preallocation state and cache conditions. A newly truncated sparse
  file is not a populated deletion fixture.
- Cover 64 KiB fragments, smaller deltas, partial bodies and checksum tails;
  many small inodes versus large inodes; shared versus separate directories;
  one versus multiple actual devices; buffered versus direct; and per-device
  concurrency ladders. QD and writer count must be reported separately.
- Deletion consumes its fixture rather than wrapping indefinitely over already
  absent objects. Report genuinely removed objects separately from absent
  retries, checksum mismatches and errors.
- Validate experiment-local data integrity, absent-object handling, checksum
  mismatch behavior where modeled, and concurrent read/delete or write/reuse
  behavior. Add fault/recovery experiments where the principle depends on them.
  Full PUT/death-fence, open-unlinked, hardlink, distributed retirement and
  authoritative GC tests belong to subsequent production integration gates.
- Measure pure reads, writes and reclamation, then mixed sustained churn.
  Test explicit low/medium/high occupancy points, such as 50/80/95 percent of
  a bounded isolated store, with reserved reclamation capacity. Precondition
  the workload; fresh, mostly empty drive peaks are not steady-state results.
- Include required body/directory/index persistence inside the durable clock.
  Report unsynced acceptance separately. Perf and optional strace remain
  separate reruns; preserve commands, source hashes, device topology and errors.
- Report fragments/s, inodes/s, payload bytes retired/s, actual allocated bytes
  returned/s, durable completion time, backlog drain time, p50/p99/max, CPU,
  memory/index size, allocation fragmentation and recovery time. Namespace
  disappearance, reusable capacity, filesystem free space and SSD discard are
  different events. Quota counters alone do not prove physical reclamation.
- Count application GC-copy bytes and device host-write deltas, normalized to
  a defined payload workload. Standard host-write counters do not establish
  NAND write amplification; obtain media-write/endurance telemetry where the
  device supports it. Track GC's impact on foreground throughput and p99.

### Implementation order and promotion gate

1. Document an EFS-relevant workload model and add independent read/write/delete
   drivers. State payload, layout, checksumming, durability and completion semantics.
2. Isolate one principle per matched comparison: batching, allocation, queueing,
   directory-sync granularity, container geometry or space reclamation. Preserve
   verified baselines and repeat measurements on the same device/configuration.
3. Combine measured winners in a benchmark-only v2 experiment and test interactions
   under steady-state churn, high occupancy and foreground read/write load.
   Reject apparent gains bought by omitted syncs, unverifiable data, unbounded
   deferred reclamation, lost capacity or unlimited RAM.
4. Select improvements from evidence and implement them separately in `efsd`.
   Resolve affected W71/W75 and other correctness requirements when integrating;
   an isolated experiment need not implement the entire production system.
5. Validate the production change with restart/fault, fencing, retention, integrity,
   mixed-load and accounting gates, followed by matched production measurements.
   Format changes require migration/rollback planning. Raw-device, realtime-device,
   FDP and ZNS variants remain later experiments driven by evidence.

## References

- [XFS mount and layout documentation](https://docs.kernel.org/admin-guide/xfs.html)
- [XFS inode allocation flags](https://www.man7.org/linux/man-pages/man2/ioctl_xfs_fsgetxattr.2.html)
- [XFS inspection and file controls](https://www.man7.org/linux/man-pages/man8/xfs_io.8.html)
- [Preallocation and hole punching](https://man7.org/linux/man-pages/man2/fallocate.2.html)
- [Periodic filesystem trim](https://man7.org/linux/man-pages/man8/fstrim.8.html)
- [XFS realtime device structure](https://kernel.googlesource.com/pub/scm/fs/xfs/xfs-documentation/+/master/design/XFS_Filesystem_Structure/realtime.asciidoc)
- [Linux zoned storage interface](https://docs.kernel.org/filesystems/zonefs.html)
- [NVMe Flexible Data Placement](https://nvmexpress.org/nvmeflexible-data-placement-fdp-blog/)
- [SPDK Blobstore](https://spdk.io/doc/blob.html)
- [AORUS drive specifications](https://www.gigabyte.com/SSD/AORUS-NVMe-Gen4-SSD-2TB)

Design sizes and predicted benefits above are experiments, not measured results
or accepted NAND placement guarantees.
