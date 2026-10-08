AMD controlled production Raft-core/disk request cost, not an end-to-end filesystem throughput claim. Three serial/batch pairs; 2560 one-KiB records, 16 already-ready requests per wave. Timer starts for every request at wave readiness and ends in its post-durability ACK callback, including queue wait. Same binary, serial recv versus recv_batch, -O2 -std=gnu11 -D_GNU_SOURCE, linked libefs.a -pthread -lm. Callback checks covered durable index; cold reopen verifies every acknowledged record. No wait to fill batches. One host, process-crash evidence only. Full live fixture gc-direct-qu6dh4p6 uses direct-I/O/TCP and restores installed service. Mixed-group sustained and idle-latency performance gates remain to be expanded before closing W93.

Follow-up: three alternating-order same/mixed-group samples, 2560 acknowledged
records each, width 1 (single-request path) and 16 (already-queued requests).
Mixed-group width16 serial 6443–6706 entries/s vs batched 44329–49288;
p99 readiness-to-ACK 2584–2750 vs 420–485 us. Single-request mixed-group
p99 serial 279–296 us vs batch API 276–292 us; no fill delay. Every cold record
verified. Peak RSS reported by the microbenchmark is 11956 KiB in all samples;
this short protocol fixture is not a universal host memory/lag bound. See
mixed-results.jsonl and raft_recv_mixed_cost.c. Production mixed large-writer/
tiny-close (W53) stress and long-lag observation remain separate acceptance work.
