# Shared-container range observations

Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.

| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |
|---|---:|---|---|---:|---:|---:|---:|---:|
| disjoint | 1 | direct | PASS | 6474 | 0.501 | 112.380 | 0.000 | 0 |
| hot-read | 1 | direct | PASS | 8513 | 0.155 | 0.514 | 0.000 | 0 |
| hot-locked | 1 | direct | PASS | 26 | 448.511 | 563.971 | 0.001 | 0 |
| hot-unlocked | 1 | direct | OBSERVATION | 22 | 399.359 | 463.646 | 0.000 | 0 |
| disjoint | 4 | direct | PASS | 23582 | 0.543 | 444.906 | 0.000 | 0 |
| hot-read | 4 | direct | PASS | 17982 | 0.241 | 0.503 | 0.000 | 0 |
| hot-locked | 4 | direct | PASS | 16 | 1605.631 | 2133.565 | 1605.631 | 0 |
| hot-unlocked | 4 | direct | OBSERVATION | 51 | 561.151 | 562.821 | 0.000 | 0 |
| disjoint | 8 | direct | PASS | 45087 | 0.643 | 14.594 | 0.000 | 0 |
| hot-read | 8 | direct | PASS | 20213 | 0.485 | 0.729 | 0.000 | 0 |
| hot-locked | 8 | direct | PASS | 16 | 4112.383 | 4968.659 | 4112.383 | 0 |
| hot-unlocked | 8 | direct | OBSERVATION | 88 | 557.055 | 600.350 | 0.000 | 0 |
