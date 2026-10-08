# W90 storage churn results

Independent workload with a volatile index and worker-local reclamation; not production GC or crash recovery.
Container size is a fixed logical file length; sparse and preallocated allocation are distinct. Occupancy is bounded data-slot capacity with one extra cleaning container per worker.
Buffered reads can be cached. No global cache drop, mount change or host-disk occupancy change is performed. Gross released blocks do not equal net free space or NAND erasure.
each/batch times required body/directory sync; files still require one body flush per replacement. No on-disk index/journal is modeled, so this is not a durable storage engine.

| Payload | Occupancy | Workers | I/O | Layout | Status | Ops/s | Read p99 ms | Write p99 ms | Copy / new record bytes | Cleanings |
|---|---:|---:|---|---|---|---:|---:|---:|---:|---:|
| 128 KiB | 50% | 1 | direct | files | PASS | 1034 | 0.291 | 1.599 | 0.00 | 0 |
| 128 KiB | 50% | 1 | direct | punch | PASS | 2333 | 0.287 | 1.207 | 0.00 | 0 |
| 128 KiB | 50% | 1 | direct | compact | PASS | 3692 | 0.311 | 0.707 | 0.24 | 1 |
| 128 KiB | 50% | 1 | buffered | files | PASS | 1327 | 0.070 | 1.999 | 0.00 | 0 |
| 128 KiB | 50% | 1 | buffered | punch | PASS | 2448 | 0.042 | 2.207 | 0.00 | 0 |
| 128 KiB | 50% | 1 | buffered | compact | PASS | 5950 | 0.049 | 1.431 | 0.24 | 1 |
| 128 KiB | 80% | 1 | direct | files | PASS | 1011 | 0.297 | 1.647 | 0.00 | 0 |
| 128 KiB | 80% | 1 | direct | punch | PASS | 2147 | 0.289 | 1.527 | 0.00 | 0 |
| 128 KiB | 80% | 1 | direct | compact | PASS | 1720 | 0.313 | 0.699 | 2.47 | 5 |
| 128 KiB | 80% | 1 | buffered | files | PASS | 1563 | 0.061 | 2.023 | 0.00 | 0 |
| 128 KiB | 80% | 1 | buffered | punch | PASS | 2564 | 0.039 | 2.463 | 0.00 | 0 |
| 128 KiB | 80% | 1 | buffered | compact | PASS | 3614 | 0.047 | 1.439 | 2.47 | 5 |
| 128 KiB | 95% | 1 | direct | files | PASS | 1119 | 0.301 | 1.679 | 0.00 | 0 |
| 128 KiB | 95% | 1 | direct | punch | PASS | 1523 | 0.289 | 1.239 | 0.00 | 0 |
| 128 KiB | 95% | 1 | direct | compact | PASS | 430 | 0.313 | 0.735 | 14.34 | 23 |
| 128 KiB | 95% | 1 | buffered | files | PASS | 1537 | 0.066 | 2.023 | 0.00 | 0 |
| 128 KiB | 95% | 1 | buffered | punch | PASS | 2640 | 0.041 | 2.751 | 0.00 | 0 |
| 128 KiB | 95% | 1 | buffered | compact | PASS | 1392 | 0.039 | 1.455 | 14.34 | 23 |
