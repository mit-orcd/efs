# W90 storage churn results

Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.
Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.
Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.
each/batch times required body/directory sync. Deferred file batching still flushes each pending live body; legacy immediate scheduling is a separate reference. No durable index/recovery is modeled.

| Payload | Occupancy | Workers | I/O | Layout | File flush | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |
|---|---:|---:|---|---|---|---|---:|---:|---:|---:|---:|
| 64 KiB | 80% | 2 | direct | files | deferred | PASS | 7533 | 0.237 | 1.527 | 0.00 | 0 |
| 64 KiB | 80% | 2 | direct | punch | — | PASS | 16320 | 0.236 | 1.199 | 0.00 | 0 |
| 64 KiB | 80% | 2 | direct | compact | — | PASS | 3783 | 0.275 | 4.831 | 2.09 | 404 |
| 64 KiB | 80% | 2 | direct | compact | — | PASS | 3813 | 0.277 | 4.735 | 2.09 | 404 |
| 64 KiB | 80% | 2 | direct | punch | — | PASS | 16373 | 0.227 | 1.135 | 0.00 | 0 |
| 64 KiB | 80% | 2 | direct | files | deferred | PASS | 14367 | 0.231 | 1.495 | 0.00 | 0 |
| 64 KiB | 95% | 2 | direct | files | deferred | PASS | 8727 | 0.243 | 1.967 | 0.00 | 0 |
| 64 KiB | 95% | 2 | direct | punch | — | PASS | 14506 | 0.237 | 1.119 | 0.00 | 0 |
| 64 KiB | 95% | 2 | direct | compact | — | PASS | 1320 | 0.289 | 5.983 | 8.10 | 1189 |
| 64 KiB | 95% | 2 | direct | compact | — | PASS | 1317 | 0.291 | 5.823 | 8.10 | 1189 |
| 64 KiB | 95% | 2 | direct | punch | — | PASS | 14572 | 0.236 | 1.143 | 0.00 | 0 |
| 64 KiB | 95% | 2 | direct | files | deferred | PASS | 8950 | 0.248 | 2.143 | 0.00 | 0 |
