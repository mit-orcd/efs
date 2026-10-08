# W90 storage churn results

Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.
Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.
Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.
each/batch times required body/directory sync; files still require one body flush per replacement. No on-disk index/journal is modeled, so this is not a durable storage engine.

| Payload | Occupancy | Workers | I/O | Layout | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |
|---|---:|---:|---|---|---|---:|---:|---:|---:|---:|
| 64 KiB | 95% | 8 | direct | files | PASS | 329 | 0.351 | 561.151 | 0.00 | 0 |
