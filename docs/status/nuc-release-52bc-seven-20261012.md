# NUC frozen52 quiet seven-client diagnostic — 2026-10-12

Exact source `52bcfd3570b0a59076c225caad9157b0825954d8`, same previously
verified normal binaries. Additional diagnostic, separate from completed
passing group3 core/peer qualification.

Command: existing `tests/live/gc_reclamation.py --mode direct --transport tcp
--posix --posix-jobs 16` with seven clients plus private
root/mount/port arguments recorded in raw command JSON. FILES; flow, early
retirement, group drain and repair disabled. No build/profiling/loaders or
other daemons during this quiet measurement. Unique clean stores.
Assertions and deadlines unchanged.

**FAIL**, exit 1, 201.841 seconds. Every client has 221 final cases:

| Client | PASS | FAIL | SKIP | Seconds |
|---|---:|---:|---:|---:|
| 0 | 215 | 5 | 1 | 94.3 |
| 1 | 216 | 4 | 1 | 94.1 |
| 2 | 215 | 5 | 1 | 94.5 |
| 3 | 215 | 5 | 1 | 94.2 |
| 4 | 216 | 4 | 1 | 94.1 |
| 5 | 216 | 4 | 1 | 94.2 |
| 6 | 216 | 4 | 1 | 94.1 |

All seven: `dir_deep_nesting` exceeded its unchanged 30-second deadline;
`truncate_big_o_trunc_honest`, `err_chdir_into_file`, and
`chdir_dotdot_after_mkdir` failed with ENOSPC or EIO. Clients 0/2/3 also failed
`perm_dir_no_x_search` with ENOSPC. mmap unsupported was the one SKIP.

**Quota context differs from AMD/Intel comparator.** NUC omitted the explicit
4G quota and therefore ran default **1G per node**. Node1 logged GC admission
pressure: used 966,332,416 / quota 1,073,741,824 and busy counter 65,536; host
/data1 still had 207 GiB free. Later ENOSPC failures have real fixture quota/
admission context. This result is not a matched leader-hint A/B experiment,
not a claim that host disk was full, and not a hardware ceiling measurement.
The deep-directory deadline failure remains a recorded failure.

Helper then refused normal stop control and timed out after its existing
60 seconds while stopping extra client2. Later peer, cold-byte and physical
cleanup assertions were **NOT RUN**, not PASS. No final fixture JSON.
Its existing finally cleanup completed: both policy mounts unmounted and no
efsd processes. Original service was normally drained before this quiet run;
no additional old-binary/data preservation or backward compatibility effort.
No datasets outside identified EFS fixture paths touched.

Raw commands, environments, timings, logs, complete per-client TSVs and cleanup
record: `results/measure/20261012-nuc-52-seven/`. Manager informed of all failure
and quota context. Next centrally frozen2bf comparison explicitly uses4G to
match AMD/Intel; deadlines remain unchanged. No independent source fix or
candidate promotion.
