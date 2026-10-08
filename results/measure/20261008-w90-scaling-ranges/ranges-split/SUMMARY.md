# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| hot-locked | 4 | direct | PASS | 24 | 1003.519 | 1601.751 | 1003.519 | 0 |
| hot-locked | 4 | buffered | PASS | 6515 | 4.863 | 17.399 | 4.543 | 0 |
| hot-unlocked | 4 | direct | OBSERVATION | 50 | 481.892 | 481.892 | 0.000 | 9 |
| hot-unlocked | 4 | buffered | OBSERVATION | 36611 | 0.599 | 1.779 | 0.000 | 73951 |
