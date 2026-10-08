# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| hot-unlocked | 8 | direct | OBSERVATION | 62 | 599.874 | 599.874 | 0.000 | 0 |
| hot-unlocked | 8 | buffered | OBSERVATION | 151794 | 0.101 | 2.472 | 0.000 | 0 |
