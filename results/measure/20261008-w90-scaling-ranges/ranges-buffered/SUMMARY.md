# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| disjoint | 8 | buffered | PASS | 71635 | 2.863 | 20.902 | 0.000 | 0 |
| hot-read | 8 | buffered | PASS | 736888 | 0.013 | 0.098 | 0.000 | 0 |
| hot-locked | 8 | buffered | PASS | 11202 | 6.687 | 31.507 | 6.463 | 0 |
| hot-unlocked | 8 | buffered | OBSERVATION | 131658 | 0.483 | 12.250 | 0.000 | 0 |
