# AMD functional acceptance — Oct 8, 2026

Working branch: devel, following v0.2.1-pre-alpha. Functional fixes use
**direct I/O/TCP**; buffered and RXE gates are targeted exceptions. No
benchmark code, benchmark harness or writer/storage optimization is changed.
AMD is one physical host with four daemon processes and software RXE on rxe0.
Hardware RoCE, native IB and nine physical hosts remain separate obligations.

## Fixes and gates

| Package | Evidence / remaining work |
| --- | --- |
| W64/W87 append | Complete UUID/epoch/length cache identity, fresh authority and cursor-overflow refusal (`fe2ce494`). Real 32-identical-RPC gate reproduces offset 8/end 16 on the former dispatcher; nonblocking per-inode reserve/reply ownership fixes it. Full direct/TCP POSIX 216 pass/one unsupported skip, peer 64/64 and durability 26/26 each phase pass. Buffered broad suites and mixed-close append during GC verify 800 exact records cold. Restart/leader-change durable reserve receipts and retirement remain open; passing repeats do not prove the original failure's sole cause. |
| W65 shutdown | Active RXE writes while stopping a leader exit cleanly in 63–114 ms; acknowledged files verify after a complete cold restart. The final run also includes 16 idle readers, 16 partial TCP frames and connection churn beside active RDMA data. This adds RXE evidence to previously closed supported-TCP acceptance. Hardware/native-IB obligations remain separate. |
| W59/W67 RDMA memory | Eager registration pinned the entire 32-slot send arena even for serial metadata connections. The 1 GiB fixture OOMed; a 2 GiB diagnostic peaked at 1214.3 MiB with 675 MiB sampled FUSE RSS. Lazy per-slot registration/reuse preserves 32 in-flight slots and bounded waits. The same production 256+64 MiB budgets pass under 1 GiB/no swap, 597 MiB kernel peak, zero OOM events; 2,037 accepted sparse writes survive failed publication, scratch exhaustion and same-mount recovery. |
| W84 namespace | Exact 144-participant PREPARE/COMMIT coordinator deaths both preserve a cold atomic namespace. In this fixture depth 143 succeeds and 144 refuses in 8.005 s without changing the source. This does not approve a universal 143-depth contract: directory lanes/fixed participants can consume the transaction envelope too. Full-ancestry protocol versus an explicit finite product limit remains a decision. |
| W54/W38 fold/GC | Real four-rank IOR and independent cold hardscan verify 12,000 records. The first run exposes a gate race: sampled-zero queue counters predate unlink; the gate now requires physical absence together with drained queues. Final physical-GC/restart acceptance is recorded in the raw run summary. Nine separate FUSE mounts are blocked by AMD's host mount policy; nine physical hosts remain owed. |

## Test isolation

AMD's fusermount policy allows only `/data1/efs/mnt` and `mnt2`. The installed
service is cleanly stopped during private-store gates, which borrow those
unmounted empty paths and never modify installed n1–n4 stores. The service
is restored in direct-I/O/TCP afterward. The harness accepts explicit mount
paths, refuses mounted/nonempty paths, defaults to TCP, and requires verbs
device descriptors for every RDMA server/client, including cold remounts.

The RDMA transport probe reports 67 QP frames plus one deliberate oversized
TCP side-channel frame. Production-function allocation tests cover serial
reuse, caller-reserved slots, full 32-slot capacity, CQ timeout/error and failed
registration. Send MRs are deregistered after QP teardown, before freeing the
shared arena. Per-connection virtual capacity and protocol sizes are unchanged.

[Raw evidence and final run summary](../../results/measure/20261008-amd-functional/SUMMARY.md).

The grouped 44-open/eight-awaiting count is unchanged: these are narrower
fixes/gates within packages with remaining obligations, not five closures.
