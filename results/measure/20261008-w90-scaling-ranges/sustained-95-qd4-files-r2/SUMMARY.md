# W90 storage churn results

Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.
Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.
Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.
each/batch times required body/directory sync. Deferred file batching still flushes each pending live body; legacy immediate scheduling is a separate reference. No durable index/recovery is modeled.

| Payload | Occupancy | Workers | I/O | Layout | File flush | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |
|---|---:|---:|---|---|---|---|---:|---:|---:|---:|---:|
| 64 KiB | 95% | 4 | direct | files | deferred | PASS | 2777 | 0.419 | 45.311 | 0.00 | 0 |
