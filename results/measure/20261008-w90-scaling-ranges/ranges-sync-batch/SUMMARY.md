# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| disjoint | 1 | direct | PASS | 5842 | 0.497 | 524.317 | 0.000 | 0 |
| hot-locked | 1 | direct | PASS | 24 | 258.047 | 563.540 | 0.001 | 0 |
| disjoint | 8 | direct | PASS | 44895 | 0.651 | 15.874 | 0.000 | 0 |
| hot-locked | 8 | direct | PASS | 15 | 4930.550 | 4930.550 | 4929.987 | 0 |
