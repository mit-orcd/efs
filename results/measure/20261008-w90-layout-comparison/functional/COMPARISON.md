# Matched layout comparison

Persistence: batch; batch: 32; seed: 42; hot set/access: 10/90%.

Simulator evidence only. CPU is measured worker user+system time including final required barriers; setup/verification excluded.
Ops/s sums reads and replacements. Replacements retire an old version. Target rates use the common process measurement interval.
Sample ranges describe observed variation, not confidence intervals. A single short run is functional evidence only.

| Payload | Occupancy | Workers | I/O | Layout | Pass/total | Median ops/s | Min–max ops/s | Median read/write p99 ms | Median CPU µs/op | Median allocated bytes |
|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|
| 65536 | 80% | 2 | direct | files | 2/2 | 10950 | 7533–14367 | 0.234/1.511 | 52.69 | 13369344 |
| 65536 | 80% | 2 | direct | punch | 2/2 | 16346 | 16320–16373 | 0.232/1.167 | 41.29 | 13369344 |
| 65536 | 80% | 2 | direct | compact | 2/2 | 3798 | 3783–3813 | 0.276/4.783 | 142.95 | 16502784 |
| 65536 | 95% | 2 | direct | files | 2/2 | 8839 | 8727–8950 | 0.245/2.055 | 55.76 | 15876096 |
| 65536 | 95% | 2 | direct | punch | 2/2 | 14539 | 14506–14572 | 0.236/1.131 | 50.49 | 15890432 |
| 65536 | 95% | 2 | direct | compact | 2/2 | 1319 | 1317–1320 | 0.290/5.903 | 365.38 | 16642048 |

## Target samples

| Case | Target | Workers | Ops/s | Allocated bytes |
|---|---:|---:|---:|---:|
| 65536-80-qd2-direct-files-r1 | 0 | 1 | 3766.364 | 6684672 |
| 65536-80-qd2-direct-files-r1 | 1 | 1 | 3766.364 | 6684672 |
| 65536-80-qd2-direct-punch-r1 | 0 | 1 | 8160.047 | 6684672 |
| 65536-80-qd2-direct-punch-r1 | 1 | 1 | 8160.047 | 6684672 |
| 65536-80-qd2-direct-compact-r1 | 0 | 1 | 1891.276 | 8286208 |
| 65536-80-qd2-direct-compact-r1 | 1 | 1 | 1891.276 | 8216576 |
| 65536-80-qd2-direct-compact-r2 | 0 | 1 | 1906.518 | 8286208 |
| 65536-80-qd2-direct-compact-r2 | 1 | 1 | 1906.518 | 8216576 |
| 65536-80-qd2-direct-punch-r2 | 0 | 1 | 8186.367 | 6684672 |
| 65536-80-qd2-direct-punch-r2 | 1 | 1 | 8186.367 | 6684672 |
| 65536-80-qd2-direct-files-r2 | 0 | 1 | 7183.503 | 6684672 |
| 65536-80-qd2-direct-files-r2 | 1 | 1 | 7183.503 | 6684672 |
| 65536-95-qd2-direct-files-r1 | 0 | 1 | 4363.495 | 7938048 |
| 65536-95-qd2-direct-files-r1 | 1 | 1 | 4363.495 | 7938048 |
| 65536-95-qd2-direct-punch-r1 | 0 | 1 | 7252.983 | 7946240 |
| 65536-95-qd2-direct-punch-r1 | 1 | 1 | 7252.983 | 7938048 |
| 65536-95-qd2-direct-compact-r1 | 0 | 1 | 660.063 | 8355840 |
| 65536-95-qd2-direct-compact-r1 | 1 | 1 | 660.063 | 8286208 |
| 65536-95-qd2-direct-compact-r2 | 0 | 1 | 658.698 | 8355840 |
| 65536-95-qd2-direct-compact-r2 | 1 | 1 | 658.698 | 8286208 |
| 65536-95-qd2-direct-punch-r2 | 0 | 1 | 7285.923 | 7954432 |
| 65536-95-qd2-direct-punch-r2 | 1 | 1 | 7285.923 | 7942144 |
| 65536-95-qd2-direct-files-r2 | 0 | 1 | 4475.152 | 7938048 |
| 65536-95-qd2-direct-files-r2 | 1 | 1 | 4475.152 | 7938048 |
