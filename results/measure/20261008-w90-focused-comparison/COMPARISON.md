# Matched layout comparison

Persistence: batch; batch: 32; seed: 42; hot set/access: 10/90%.

Simulator evidence only. CPU is measured worker user+system time including final required barriers; setup/verification excluded.
Ops/s sums reads and replacements. Replacements retire an old version. Target rates use the common process measurement interval.
Sample ranges describe observed variation, not confidence intervals. A single short run is functional evidence only.

| Payload | Occupancy | Workers | I/O | Layout | Pass/total | Median ops/s | Min–max ops/s | Median read/write p99 ms | Median CPU µs/op | Median allocated bytes |
|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|
| 65536 | 95% | 4 | direct | files | 4/4 | 12157 | 11917–12241 | 0.316/6.143 | 62.94 | 8160313344 |
| 65536 | 95% | 4 | direct | punch | 4/4 | 22123 | 21846–22209 | 0.315/2.591 | 49.13 | 8162666496 |

## Target samples

| Case | Target | Workers | Ops/s | Allocated bytes |
|---|---:|---:|---:|---:|
| 65536-95-qd4-direct-files-r1 | 0 | 2 | 6063.561 | 4080156672 |
| 65536-95-qd4-direct-files-r1 | 1 | 2 | 6071.583 | 4080156672 |
| 65536-95-qd4-direct-punch-r1 | 0 | 2 | 11113.995 | 4081307648 |
| 65536-95-qd4-direct-punch-r1 | 1 | 2 | 11094.768 | 4081364992 |
| 65536-95-qd4-direct-punch-r2 | 0 | 2 | 11054.070 | 4080840704 |
| 65536-95-qd4-direct-punch-r2 | 1 | 2 | 11140.675 | 4081094656 |
| 65536-95-qd4-direct-files-r2 | 0 | 2 | 6064.022 | 4080156672 |
| 65536-95-qd4-direct-files-r2 | 1 | 2 | 6176.894 | 4080156672 |
| 65536-95-qd4-direct-files-r3 | 0 | 2 | 6083.166 | 4080156672 |
| 65536-95-qd4-direct-files-r3 | 1 | 2 | 6095.149 | 4080156672 |
| 65536-95-qd4-direct-punch-r3 | 0 | 2 | 11019.855 | 4081360896 |
| 65536-95-qd4-direct-punch-r3 | 1 | 2 | 11030.422 | 4081332224 |
| 65536-95-qd4-direct-punch-r4 | 0 | 2 | 10915.727 | 4081340416 |
| 65536-95-qd4-direct-punch-r4 | 1 | 2 | 10930.064 | 4081319936 |
| 65536-95-qd4-direct-files-r4 | 0 | 2 | 5948.466 | 4080156672 |
| 65536-95-qd4-direct-files-r4 | 1 | 2 | 5968.426 | 4080156672 |
