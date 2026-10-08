# W90 storage churn results

Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.
Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.
Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.
each/batch times required body/directory sync; files still require one body flush per replacement. No on-disk index/journal is modeled, so this is not a durable storage engine.

| Payload | Occupancy | Workers | I/O | Layout | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |
|---|---:|---:|---|---|---|---:|---:|---:|---:|---:|
| 64 KiB | 80% | 1 | direct | files | PASS | 716 | 0.279 | 1.703 | 0.00 | 0 |
| 64 KiB | 80% | 1 | direct | punch | PASS | 1948 | 0.243 | 1.599 | 0.00 | 0 |
| 64 KiB | 80% | 1 | direct | compact | PASS | 2315 | 0.252 | 0.571 | 2.39 | 5 |
| 64 KiB | 95% | 1 | direct | files | PASS | 534 | 0.281 | 2.911 | 0.00 | 0 |
| 64 KiB | 95% | 1 | direct | punch | PASS | 620 | 0.257 | 1.727 | 0.00 | 0 |
| 64 KiB | 95% | 1 | direct | compact | PASS | 580 | 0.259 | 0.599 | 13.88 | 23 |
