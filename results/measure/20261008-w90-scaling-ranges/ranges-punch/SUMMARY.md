# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| hot-locked | 4 | direct | PASS | 21 | 1163.263 | 1401.896 | 1163.263 | 0 |
| hot-locked | 4 | buffered | PASS | 8030 | 4.031 | 24.753 | 3.711 | 0 |
| hot-unlocked | 4 | direct | OBSERVATION | 44 | 401.407 | 600.062 | 0.000 | 57 |
| hot-unlocked | 4 | buffered | OBSERVATION | 24552 | 0.747 | 9.928 | 0.000 | 20762 |
