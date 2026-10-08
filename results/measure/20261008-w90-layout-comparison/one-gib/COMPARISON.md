# Matched layout comparison

Persistence: batch; batch: 32; seed: 42; hot set/access: 10/90%.

Simulator evidence only. CPU is measured worker user+system time including final required barriers; setup/verification excluded.
Ops/s sums reads and replacements. Replacements retire an old version. Target rates use the common process measurement interval.
Sample ranges describe observed variation, not confidence intervals. A single short run is functional evidence only.

| Payload | Occupancy | Workers | I/O | Layout | Pass/total | Median ops/s | Min–max ops/s | Median read/write p99 ms | Median CPU µs/op | Median allocated bytes |
|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|
| 65536 | 80% | 2 | direct | files | 2/2 | 6548 | 6162–6933 | 0.282/4.015 | 61.98 | 3435921408 |
| 65536 | 80% | 2 | direct | punch | 2/2 | 8145 | 7539–8752 | 0.284/2.207 | 54.13 | 3436189696 |

## Target samples

| Case | Target | Workers | Ops/s | Allocated bytes |
|---|---:|---:|---:|---:|
| 65536-80-qd2-direct-files-r1 | 0 | 1 | 3454.282 | 1717960704 |
| 65536-80-qd2-direct-files-r1 | 1 | 1 | 3479.075 | 1717960704 |
| 65536-80-qd2-direct-punch-r1 | 0 | 1 | 3766.086 | 1718091776 |
| 65536-80-qd2-direct-punch-r1 | 1 | 1 | 3772.485 | 1718079488 |
| 65536-80-qd2-direct-punch-r2 | 0 | 1 | 4362.866 | 1718112256 |
| 65536-80-qd2-direct-punch-r2 | 1 | 1 | 4388.678 | 1718095872 |
| 65536-80-qd2-direct-files-r2 | 0 | 1 | 3094.881 | 1717960704 |
| 65536-80-qd2-direct-files-r2 | 1 | 1 | 3066.884 | 1717960704 |
