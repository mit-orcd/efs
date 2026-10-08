# W90 storage churn results

Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.
Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.
Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.
each/batch times required body/directory sync; files still require one body flush per replacement. No on-disk index/journal is modeled, so this is not a durable storage engine.

| Payload | Occupancy | Workers | I/O | Layout | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |
|---|---:|---:|---|---|---|---:|---:|---:|---:|---:|
| 128 KiB | 95% | 1 | direct | files | PASS | 984 | 0.309 | 1.711 | 0.00 | 0 |
| 128 KiB | 95% | 1 | direct | punch | PASS | 1645 | 0.285 | 1.231 | 0.00 | 0 |
| 128 KiB | 95% | 1 | direct | compact | PASS | 432 | 0.307 | 0.735 | 14.34 | 23 |
