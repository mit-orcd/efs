# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| hot-locked | 1 | direct | PASS | 30 | 462.847 | 663.326 | 0.001 | 0 |
| hot-locked | 1 | direct | PASS | 25 | 463.690 | 463.690 | 0.001 | 0 |
