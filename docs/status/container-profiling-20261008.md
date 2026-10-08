# P4.1 · Container daemon tracing and five CPU profiling rounds

Branch `devel-storage-engine`; baseline `175fcf98`. User requested round 1
strace, then rounds 2–6 CPU perf, for both buffered and direct container I/O.
Use the same isolated three-node VM group/private gateway workload as the
[initial A/B](container-engine-20261008.md). Installed services stay running.

Each diagnostic round uses fresh roots, 256 × 512 KiB files, four gateway
workers, 15-second read and durable 128 KiB overwrite phases. Trace/profile
all three private daemons during create/read/overwrite/delete; verify hashes,
restart, and require complete physical reclamation. Profile cycles at 199 Hz
with frame-pointer call stacks, plus task clock/instructions/context switches.
Raw profiler data stays on the VMs; collect textual reports and source hashes.
Traced timings are diagnostic, not throughput comparisons. Finish with an
unprofiled repeated comparison. Do not weaken sync, checksum or lifetime-lock
requirements to improve a benchmark number.

## Round 1 · syscall trace

Both modes passed all gates. Representative direct node-1 create trace:
15,360 mkdir calls (14,738 EEXIST errors), 21,104 fsync calls. It synced the
storage root and six ancestors above it 1,025 times each. Buffered overwrite
trace showed 18,030 mkdir calls, all EEXIST. Futex wall time includes blocked
threads and is not evidence of CPU consumption or removable lock contention.

Change before round 2: attempt mapping creation before mkdir; create directories
only on ENOENT. Persist root ancestry once at startup; per-mapping publication
and deletion sync every ancestor through the storage root, without syncing
unrelated ancestors above it. Tests assert zero mkdir on overwrite, a root
barrier, and no barriers above root. Backend crash/corruption gates passed.

## Round 2 · cycles/call stacks

Both modes passed. Representative node-1 inclusive open cost: 8.38% of sampled
cycles in direct overwrite, 7.95% buffered; includes metadata/directory opens.
Container GET: 12.08% direct / 10.06% buffered. Kernel scheduling, interrupts,
PV spin locks and storage paths dominate individual symbols. Checksums and
lifetime locks are retained.

Change before round 3: keep positional-I/O descriptors for the first eight
container segments per root. Segment names are never replaced or removed while
serving; descriptors close after worker shutdown. Later segments retain per-op
opens. Mapping/directory descriptors are not cached. This bounds open descriptors
and preserves fsync/punch ordering. Tests cover descriptor reuse in both modes,
concurrent access, restart and injected failures. Gate passed.

## Round 3 · bounded descriptor cache

Representative direct node-1 read profile: inclusive libc open 1.93% (round 2:
4.26%); container GET 8.88% (12.08%). Mapping reads/path walks remain visible
(3.05% inclusive mapping_read). Percentages are sampled shares, not absolute
CPU savings or throughput claims. Bounded cache reuse is asserted by tests.

Change before round 4: retain a root directory descriptor; use relative openat
for mapping reads and unlinkat for deletion, and relative directory opens plus
fsync of the held root descriptor for runtime barriers. Names and all barriers
remain unchanged. Backend gate and round 4 passed both modes.

## Round 4 · root-relative lookup

Representative direct node-1 overwrite profile: mapping publication (`atomic_page`)
12.31% inclusive; temporary-name creation 3.70%. Create profile temp-name creation
5.29%. Relative mapping/directory operations remain checksummed and synced.
Percent shares fluctuate with VM/kernel work; no isolated speedup claimed yet.

Change before round 5: use the record's existing random nonce for a same-directory
O_EXCL temporary, opened relative to the held root; renameat/unlinkat also avoid
absolute ancestors. Markers retain mkstemp. Collision test preserves an existing
temporary and current map. Crash-after-rename injection now wraps renameat too.
Round 5 gate/run passed both modes.

## Round 5 · mapping publication

Representative direct node-1 overwrite: atomic_page 9.31% inclusive (round 4:
12.31%); server_container_put 17.18% (22.09%). Temp-name generator no longer
appears in runtime publication. These are diagnostic sampled shares.
Memory clearing remains visible (~0.58% libc memset self in this profile).

Change before round 6: avoid zeroing an entire aligned output buffer immediately
before copying the header and payload over it; zero only residual alignment
padding. New 17-byte payload test inspects the physical padding, then verifies
read/checksum/restart/deletion. A separate test forces a segment outside the
8-descriptor cache and exercises its per-operation I/O and recovery. Gate and round 6 passed both modes.

## Final measurement contract

After round 6, compare original daemon SHA256 `d5eb6ef0ec65d1efec63162d97e424715af82766457be5911e891da0eafd4153`
against final code with profiling disabled: original, optimized, optimized,
original; each batch tests both modes and later batches reverse mode order.
Thirty-second read and overwrite phases, two repeats per version/mode. Same
client and transport, fresh roots, complete integrity/restart/physical GC gates.
The installed VM cluster remains running, so shared-load variability is reported.





## Round 6 and final review

Both modes passed. No further container hot-path change justified by the remaining
kernel scheduling, network/Raft, required filesystem and checksum costs. Diagnostic
CPU/MiB shows little consistent improvement across rounds 2–6; sampled percentages
must not be presented as end-to-end gains. All perf phase reports have zero lost samples
(120 reports across five rounds × two modes × four phases × three nodes).

Final code review found overlapping container roots could make prefix routing
ambiguous. An additional startup-only guard canonicalizes the complete target set
and rejects overlap before any recovery/punching. A sentinel extent regression
checks both root orders and proves invalid startup leaves the first root untouched;
valid-root data is verified afterward. Files-engine nesting policy is unchanged.
The six diagnostic rounds and matched timing matrix use the pre-guard binary;
the final guard is outside the timed I/O path and passed separate final-build
functional qualification. Their source/binary identities are recorded separately.

## Matched unprofiled results

Two repeats per version/mode, ABBA version order, later batches reverse mode
order. Values below are min–max; both individual repeats and p99 latency are in
[matched-summary.json](../../results/measure/20261008-container-six-rounds/matched-summary.json).
Create/read/overwrite are logical client MiB/s; delete is unlink files/s, not
physical GC throughput. Each create/delete covers only 256 files, so its short
phase is particularly sensitive to metadata delays.

| Mode | Phase | Original range | Optimized range |
|---|---|---:|---:|
| direct | create MiB/s | 9.93–45.81 | 8.22–46.30 |
| direct | read MiB/s | 137.15–137.80 | 126.13–138.45 |
| direct | durable overwrite MiB/s | 22.68–22.69 | 22.50–22.53 |
| direct | delete files/s | 477.41–493.38 | 359.34–467.83 |
| buffered | create MiB/s | 13.69–46.45 | 12.35–13.18 |
| buffered | read MiB/s | 5.88–142.96 | 6.04–65.79 |
| buffered | durable overwrite MiB/s | 3.37–22.40 | 22.65–22.73 |
| buffered | delete files/s | 23.36–509.34 | 23.28–26.33 |

Direct durable overwrite median is 22.68 → 22.52 MiB/s (about −0.7%); no
material improvement. Original buffered repeat 1 had GETCHUNKS ReadIndex waits
around 400 ms and overwrite p99 852 ms; other write runs have p99 about 48–55 ms.
Buffered read p99 spans 20–1,054 ms originally and 534–1,253 ms optimized.
Buffered delete p99 spans 11–839 ms originally and 578–605 ms optimized.
These are real end-to-end stalls, retained in the evidence. This matrix cannot
attribute buffered differences to the container optimizations or establish a
read/delete speedup. Investigate Raft/metadata ReadIndex waits separately before
using these VMs to rank storage engines. No changes to Raft were made here.

All eight matched cases passed zero client errors, checksum verification,
acknowledged-data verification after restarting all three private daemons,
zero data mappings and zero allocated container blocks after GC on every node.
Installed-service PID/command snapshots are identical before and after each batch.
The small warm workload and four closed-loop workers do not prove drive saturation
or production performance. Physical VM backing storage is shared with the running
installed cluster. Kernel scheduling/network/Raft costs dominate remaining profiles;
removing mandatory sync/checksum/locks would change semantics and was rejected.

## Final qualification

Final startup guard regression and both final-build VM cases passed. Linux
`make -j4 all && make test` passed with zero compiler warnings. Total: 12 diagnostic,
8 matched unprofiled, and 2 final-build functional cases, all PASS. Final source and
binary identities are separate from the timed pre-guard build. Documentation gate
passes all five checks. Files remains default; containers remains experimental;
no migration or power-cut durability qualification is claimed. Installed cluster
services were not replaced, shared devel was not edited, and nothing was pushed.
Evidence contains compressed syscall traces, perf text/stats, per-run latency,
source and binary hashes; binaries and perf.data are excluded from Git.

## Diagnostic CPU normalization

Sum of three daemon task clocks per logical client MiB (includes profiling/phase
attachment overhead). No consistent reduction; these are diagnostic values.

| Perf round | Direct read ms/MiB | Buffered read ms/MiB | Direct overwrite ms/MiB | Buffered overwrite ms/MiB |
|---|---:|---:|---:|---:|
| 2 | 22.1 | 20.8 | 101.4 | 103.1 |
| 3 | 21.4 | 20.7 | 101.5 | 102.2 |
| 4 | 21.2 | 20.5 | 102.2 | 101.1 |
| 5 | 21.4 | 20.9 | 100.8 | 101.4 |
| 6 | 21.2 | 20.7 | 100.2 | 101.4 |
