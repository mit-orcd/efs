# AMD three-hour non-RDMA round

Started 2026-10-11 03:38:46 UTC (Oct 10 EDT), with a 06:38:46 UTC deadline.
Branch `devel-amd` is based on devel `746ae9f0`; no push or shared-devel edit.
The user gives exclusive AMD access and requests quick closures first.

## Completed checkpoints

- W89 measurement defect: fresh 1 GiB follower-cap/recovery/cold-value gate,
  2240 acknowledged MiB writes and 276 exact TSV/raw observations.
- Symlink request/test mismatch: explicit follow, no-follow, dangling and peer
  unlink/link-count checks; production hard-link behavior unchanged.
- W73 and W95 documentation reconciled with existing accepted software and
  indexed-packing evidence. W95's production-layout decision remains open.
- W66 reproduced unhosted-coordinator LOOKUP error repaired; 64 mkdir races
  per I/O mode survive cold restart. Broader W66 obligations remain open.
- W94 combined first-descriptor adoption: focused/native, sanitizer/Memcheck,
  both TCP full POSIX/peer, synchronous writes, replay and physical-GC gates
  pass. Applied CRC-validated Raft logs show three versus two creation/close
  lease commands per file in 64-file controls. Repeated performance and
  additional fault gates are being completed.

The [status checkpoint](../../../docs/status/amd-non-rdma-20261010.md) records
exact scopes and limitations. The earlier buffered MKDIR failure and unsupported
native-ISA Valgrind attempt are preserved as failures, not passing gates.

## Test isolation

Four private `efsd` processes on AMD's one NVMe use ports 24190–24193, TCP,
file storage and separate fresh stores, with direct or buffered I/O. This is
functional software coverage, not four physical failure domains. Installed
stores, daemon binaries and ports 17432–17435 are preserved. Allowed FUSE
mounts are borrowed through the drain-safe client script and restored with
TCP and `.efs` commands disabled. No RDMA gates or D25 activation.

## Reproduction and interpretation

`w94-ab-run.sh` runs four alternating pairs in each I/O mode. Both frozen
builds include the same W66 routing repair and RPC implementation; the FUSE
adoption change is the intended variable. Eight workers create/write/close
8192 deterministic nonzero 512-byte files. Close durability remains enabled.
`tiny-mixed.json` times creation, write and close; every case then verifies
every file after private daemon crash/restart, unlinks it and requires physical
reclamation. The first baseline uses serial cold verification; subsequent
cases use eight independent oracle workers. Oracle/cleanup time is not part
of reported creation throughput.

`lease-proof.py --source SOURCE --root REMOTE_AB_ROOT` decodes stopped private
Raft stores with the existing production-log decoder, validates record CRCs and
truncation, and bounds entries by persisted applied indexes. It records flags
and immutable identities for all 64 control files; cold reads add the same
OPEN/CLOSE pair to both paths. Text logs omit successful deferred edges.

`analyze-w94.py LOCAL_AB_EVIDENCE [--partial]` verifies case markers, payload,
mode/transport, exact cold checks, independent physical-inventory proof and
restoration before summarizing rates and
latency. Percentiles are medians of per-case percentiles, not pooled samples.
Finite storms do not establish sustained-service or universal RSS bounds.

The candidate's portable ownership Memcheck succeeds. The native-ISA server
Memcheck attempt aborts at an unsupported AVX-512 instruction and is not a
passed server gate. Raw compact evidence is retained under `evidence/`; private
store binaries are kept on AMD and excluded from retrieval.

## Untimed oracle correction

The first long cases exposed a test omission: the early-exit tiny fixture
waited for sampled GC counters but skipped the full fixture's final physical
inventory check. Later fixtures require both empty counters and absent physical
objects/checksums before acceptance. `inventory-proof.py REMOTE_AB_ROOT` also
checks every completed stopped private store and retains manifest/result hashes,
so earlier cases receive independent physical verification too. This changes
only the untimed acceptance oracle.

A 20-second 49-Hz profile and bounded client syscall summary during the first
buffered baseline's untimed phase show background Raft pump/GC work while the
client is idle. Long case completion includes physical reclamation; it must not
be reported as slow creation or included in creation throughput. The strace
exit 124 is its intentional timeout/detach, not a client failure.

## Additional lifecycle gates queued

The fault-only runner rebuilds after A/B, then hides one consumed durable
OPEN|CLEAR_PENDING reply in the real FUSE client. The failed CREATE descriptor
must return EIO without losing its retained cleanup identity; unlink must
reclaim the original inode, and name reuse must preserve exact cold bytes.
This is a deterministic consumed-reply fault, not a physical network outage.
The raw pending-CREATE helper also crashes before and after combined adoption
on LOCAL and cross-group HASHED directories, then replays the same durable
identity before CLOSE. These additional variants are pending live results.

The perf capture has zero lost samples and its cycle report decodes. Header
inspection warns about unavailable event-description metadata; preserve that
limitation when reusing this diagnostic. It is not a calibrated attribution
of wall latency or an A/B CPU measurement.

A later review reproduces a refused-combined-edge bookkeeping leak in the
first candidate. Before/after actual-source regressions and sanitizer evidence
are retained. Cleanup now handles immediate negative proof and negative proof learned
after an unknown reply, preserves the original CREATE FileID, and
allocates pending-only cleanup before retiring the known negative body. Its
live invalid-FileID and server-batch tests remain queued. A/B binaries stay
frozen; performance results describe the earlier successful-path candidate.
