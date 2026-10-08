# P4.1 · Experimental daemon container backend

Implementation branch: `devel-storage-engine`; baseline merge `a42a5be3`.
User authorized implementation and four-way files/containers × buffered/direct
A/B on the xorinox VM cluster. This is a new on-disk format with fresh roots;
there is no conversion of installed data.

## Initial implementation contract

`--storage-engine files|containers`, default files. Persist engine/root identity
and hold an exclusive root lock; reject incompatible engine startup. Legacy
unmarked roots may be adopted by files; containers requires empty data roots.
Metadata table fragments retain their existing file format and buffered I/O.

The first container implementation retains one checksummed 4 KiB mapping file
per data fragment at the existing fragment path. Payloads live in variable-sized,
4 KiB-aligned extents inside sparse 1 GiB containers per target. This preserves
existing path-based placement, orphan traversal and GC authority while providing
an actual packed/reusable data backend. It does not eliminate namespace metadata,
and simulator gains are not a performance promise for this implementation.

Write a fresh extent, sync its bytes, atomically replace and durably sync the
mapping, then retire the old extent. Readers hold the existing fragment/inode
locks through physical reads. Deletion durably removes the mapping before punch
and reuse. Failed/uncertain operations retain allocation rather than risk reuse.
Startup reconstructs bounded allocation bitmaps from validated mappings and
reclaims unreachable ranges before serving. A corrupt mapping/header fails closed.
Payload checksums detect wrong/stale ranges. Root UUIDs bind mappings to targets;
reordering configured roots does not reinterpret offsets. Fixed startup targets
only for containers initially; dynamic target addition is rejected explicitly.

## Acceptance and A/B plan

Test actual backend read/write/replacement, checksum-conditional deletion, inode
GC, quota/restart accounting, concurrent same-fragment access, interrupted writes,
corruption and engine mismatch in both I/O modes. Build/test on Linux.

Use an isolated three-daemon group in the existing xorinox VMs, alternate test
ports and private `/data1/efs/bench` roots, plus a private gateway mount. Run all
four combinations with identical client workloads, fresh roots and repeated
measurements; verify data before/after daemon restart and deletion. Record guest
XFS/virtio and host qcow2/cache settings. Guest O_DIRECT bypasses guest data cache;
it does not necessarily bypass QEMU/host caches or establish physical NVMe limits.
Keep client I/O settings fixed so only server engine/mode varies. Preserve the
installed services and user data; report errors and rare stalls alongside rates.

## Implemented and first cluster gate

`efsd` now dispatches actual data PUT/GET, conditional deletion, inode GC and
restart usage accounting through the selected backend. Files remains default.
Multi-target startup, root identity/locking, bounded allocation recovery and
full payload checksum validation are implemented. The metadata path detector
covers every reserved metadata shard inode, including restart accounting.

Sixteen cluster cases passed: eight short functional and eight sustained cases,
two repeats per engine/I/O combination. Every case verified complete file hashes
before and after a full three-daemon restart. Deletion converged to zero data
records and zero allocated container blocks on all three nodes. Installed daemon
and gateway PID/command snapshots matched before/after each matrix.

Sustained phases ran 30 seconds each, four client workers, 256 × 512 KiB files
and durable 128 KiB overwrites across 32 hot files. TCP and client I/O settings
were fixed. Rates include end-to-end EFS and hash verification costs.

| Engine | Server I/O | Read MiB/s range | Overwrite MiB/s median (range) | Overwrite p99 ms range |
| --- | --- | ---: | ---: | ---: |
| files | buffered | 5.4–5.9 | 25.1 (25.1–25.1) | 39.5–39.6 |
| files | direct | 78.2–139.2 | 25.4 (25.4–25.4) | 39.2–40.2 |
| containers | buffered | 136.3–141.7 | 22.6 (22.5–22.6) | 47.9–50.3 |
| containers | direct | 136.1–137.9 | 22.5 (22.4–22.6) | 49.1–49.5 |

Containers were approximately 10–11% slower for durable overwrites here, so keep
files as default. Read rates do not isolate storage performance: buffered-files
logs contain slow GETCHUNKS metadata read-index waits and busy retries around
400 ms; files/direct varies substantially between repeats. Investigate the shared
metadata/read-index path before attributing the read gap to the physical backend.
Two repeats are preliminary evidence, not a confidence or capacity claim.

The storage VMs have two vCPUs and XFS/virtio qcow2 data disks with QEMU
`cache=none,io=native,iothread=1`, sharing the host XFS NVMe. Installed production
load was left running. No bare-metal or cold-disk performance claim is made.

The inventory observer hit an unlink race during fifth sustained-case GC.
The workload/restart/delete had succeeded; its measurements were preserved.
Inventory now tolerates concurrent unlink (20 local stress sweeps passed), the
case's reclamation/log collection/clean shutdown completed, and remaining cases
resumed against the original installed-process baseline.

Raw [results and reproduction evidence](../../results/measure/20261008-container-engine-ab/README.md)
include JSON phases, compressed logs, inventories, runtime source hashes and
before/after process snapshots. Measured daemon SHA256:
`d5eb6ef0ec65d1efec63162d97e424715af82766457be5911e891da0eafd4153`.

The production entry-point harness covers buffered/direct I/O, two targets,
versions, logical quota, same-fragment concurrency, conditional and inode GC,
interrupted writes, sync/punch failure, corrupt mapping/header/payload, large
fragments and format mismatch. Linux `make -j4 all` and full `make test` passed against the final runtime sources; zero compiler warnings. The full suite includes the new production-entry-point backend harness and existing storage/metadata/client/benchmark gates. Final build/test logs are preserved with the evidence.

See [operator usage and reproduction](../operations/storage-engines.md).
No production acceptance claimed; files remains the recommended default.

## Follow-up · six container profiling rounds

See [P4.1 profiling checkpoint](container-profiling-20261008.md) for syscall
reductions, five CPU profiling rounds and repeated original/optimized measurements.
Direct durable overwrite remains about 22.5 MiB/s. Buffered read/delete metadata
stalls persist; historical A/B numbers above do not establish a storage speedup.
