# Reserved writer admission — Oct 7

Fallback waiters now form a FIFO per slot with individual conditions. An empty
slot is reserved for its oldest waiter; newer fast-path callers cannot bypass
it. Timeout removes the waiter and signals the next eligible head. Shutdown
signals all fallback waiters. This does not introduce a global FIFO or move
callers between slots. I/O, writer-count and durability defaults are unchanged.

Deterministic ordered-admission regression and sustained contention pass locally
with ASan/UBSan and on Linux. Five simulated-service controls (16 callers,
12 writers, 8192 operations) reduce admission maximum from 83.2–83.5 ms to
0.197–0.224 ms, with mean service unchanged at ~152 microseconds.

40 randomized matched xorinox runs: QD16, auto/12 writers, 64 KiB fragments,
64 MiB overwrite window, five repeats, buffered/direct, each storage root.
Both binaries use the committed buffered fix and unchanged direct I/O; only
writer admission differs. All runs complete without errors. VMs remain off.

| Mode/root | Before GiB/s | Fair GiB/s | Mean p99 before/fair µs | Worst max before/fair ms |
|---|---:|---:|---:|---:|
| Buffered/root1 | 27.301 | 25.952 | 60 / 92 | 68.25 / 3.91 |
| Buffered/root2 | 27.553 | 26.230 | 60 / 90 | 23.50 / 51.75 |
| Direct/root1 | 4.666 | 4.311 | 714 / 1043 | 1217.97 / 604.14 |
| Direct/root2 | 3.273 | 3.393 | 517 / 598 | 297.19 / 50.06 |

Fairness has a cost: buffered throughput falls ~5% and p99 rises. Worst
observed direct waits shrink but root1 throughput remains variable (CV 38%
before, 44% after). Root2 buffered maximum worsens in one run; do not claim
universal latency improvement. These are warm-window diagnostics, not sustained
media or FUSE performance. Fair admission removes demonstrated starvation;
it does not resolve root1's device/filesystem variability.
