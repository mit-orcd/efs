# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| disjoint | 1 | direct | PASS | 3076 | 0.763 | 10.591 | 0.000 | 0 |
| hot-locked | 1 | direct | PASS | 3192 | 0.731 | 11.490 | 0.000 | 0 |
| disjoint | 8 | direct | PASS | 18399 | 1.135 | 15.241 | 0.000 | 0 |
| hot-locked | 8 | direct | PASS | 2696 | 26.367 | 58.237 | 25.727 | 0 |
