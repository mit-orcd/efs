# Allocation, GC and fold acceptance — Oct 7, 2026

D17 closes after actual ecrawl acceptance on NUC direct and buffered four-node
TCP fixtures. D26/W44 and W54/W38 retain the unmet gates described below.

## D17: closed

The cold non-writing client reports correct blocks for dense, partial-tail,
truncated, empty, 1 GiB sparse and 16 GiB sparse files. Real ecrawl captures
report zero false sparse classifications in the dense tree, two true sparse
files in the sparse tree, and zero errors in both modes. Dense 1 MiB reports
2048 blocks; 131073 bytes reports 257; truncated 1000 bytes reports two;
sparse files report 256 blocks each. No allocation is invented from apparent
size alone. The first harness attempt failed to read ecrawl's manifest filename;
that attempt is retained and superseded by the successful repeats.

The ecrawl build uses clean ereport source `7308ed67c352ec3157e1ee3a8672a35cb45dbf25`.
It was built in a private NUC directory using the existing zstd runtime and
copied public headers; no system package or shared cluster was changed.
Cold stat timings were recorded under concurrent compilation/I/O and do not
establish the separate historical under-1-ms idle performance claim.

## D26/W44

The full live-table idle-hour gate attributes committed Raft commands by CRC-
validated log identities and commit bounds. It counts GC_ACK separately, without
counting replica copies or uncommitted/truncated entries as committed work.
The full 3600.000-second idle interval with 4096 live files passes: zero new
committed entries, zero GC_ACK, zero slow GC passes; daemon CPU is
0.679%, 1.096%, 1.101%, 0.682%. The complete fixture then passes physical
delete, restart, empty inventory, zero quota and late PUT fencing on all members.
The retained-table gate
does not close: its first settled sample still shows a GC-related transient.

The 10 GiB deletion reclaimed exactly 245760 fragments and 16106127360 payload
bytes (15 GiB), leaving zero physical usage. It took 574.374 seconds. The pass
trace reaches 1028 emitted rows; the historical minimum pass-work coverage is
represented, but metadata after deletion was measured during another nine-client
IOR job: mkdir 11.5 → 264.7 ms, create 3.2 → 402.5 ms, append 7.3 → 519.7 ms.
These overlapping timings cannot isolate a GC regression; a retained-table quiet
probe is required before acceptance. Full per-pass data and latency samples are
retained, rather than using an empty namespace as proof of reclamation.

## W54/W38

Real IOR hard geometry uses 47008-byte transfers, 3000 segments per rank and
fixed timestamp signature 271828. Both IOR cold verification and independent
hardscan are run after separate remounts. Direct four-rank and 36-rank scans
verify 12000 and 108000 records respectively, with zero bad or short records.
Buffered four-rank cold verification/hardscan also passes. Two-peer fold audits
and cold GC/restart controls accompany each fixture. The nine-client fixture completes with 108000 records, bad=0 and short=0,
including physical GC/restart and zero-quota controls. It uses nine independent
FUSE processes on one NUC, not nine physical hosts. The direct single-client
fixture exceeded its 600-second cleanup timer while GC was progressing;
a later exact-binary-guarded reopen confirms physical usage zero.
The documented nine-host/RDMA topology gate remains open.

## Retirement failure repaired

Initial buffered and bulk fixtures failed during final stop because procfs
reported PermissionError on a dying client's descriptor directory. Fix
`c47c6f63` treats inaccessible descriptors as unproven ownership and waits for
natural exit. It never signals a live inaccessible owner. Synthetic Linux tests
cover both natural exit and live refusal. The buffered fixture is rerun from a
fresh private source tree. Remaining retirements in the already-running direct
and hour fixtures use this helper overlay; their binaries and loaded harness
remain unchanged, and separate overlay manifests record that fact.

## Measurement exclusions

A reused scratch-directory copy retained older C inputs. Those cold-tool and
small-bulk attempts are excluded and superseded by uniquely named clean-tree
repeats. The retained-store binary guard rejected a mismatched probe before
opening data. An earlier IOR cleanup resume used the older scratch build;
its result is excluded, and the matching original binary independently
revalidated zero physical usage afterward. Original IOR and idle-hour C inputs
match the committed source.

Early retained-table samples also overlap startup transaction recovery, which
visits 64 of 4096 marked shards per pass. Their first two create medians were
about 46 ms; the third returned to 3.177 ms. They measure recovery contention,
not settled metadata performance. The settled repeat waits 180 seconds. Its first create/append medians are
44.342/60.650 ms; the next two are 3.073/6.530 and 2.946/5.574 ms. Startup
recovery completed during the wait, but an active pass during metadata work
scans 41271 fragment keys (41231 tombstones), emitting 40 rows, and takes
1393 ms (1377 ms reap, 12 ms frag). The time is dominated by reap, so the fragment tombstone count alone
does not establish the cause. This is a remaining retained-table
performance finding, not merely startup recovery. Final deletion of the 4096-file idle fixture also shows approximately
1.2-second passes dominated by reap, with no delete/sweep/reap/ack errors
while progressing. D26/W44 remain open;
tombstone compaction/delete fan-out require the existing D26(ii) design choice.

## Counts and evidence

| Category | Before | Current |
| --- | ---: | ---: |
| Unfinished correctness/investigations | 15 | 15 |
| Implemented/partial awaiting gates | 10 | 9 |
| Unresolved decisions | 3 | 3 |
| Future features | 18 | 18 |
| Total open packages | 46 | 45 |

[Evidence](../../results/measure/20261007-three-gates/README.md). No shared-service
rollout was performed. The full Linux unit suite passes; helper and production
encoding regression tests are also recorded.
