# AU — Raft pump kicks: fewer eventfd writes, never a lost wake (2026-10-28)

Coordinator report: branch `agent/devel-au`, head `9c4bfd171ccc5450ae852a2597351150e543730a`
(docs-only branch; measurement-only task, no product change). Gate on intel2:
`TOTAL 546 FAIL 2 NOTRUN 11 STATUS FAIL (821s) OVERBUDGET 4` — both FAILs are
untouched-base failures: K55 (b6 slow fsync EIO) and a **K48 recurrence**
(`test_w130_dir_fault.py` `partial listing 95 != 96`, case retained).
Evidence: this page; raw data intel2 `~/efs-artifacts/taskAU-20261028/`. NOTRUN: none beyond the gate's usual host
limits (no RDMA device, no valgrind, no cluster scripts); `perf sched` is
unavailable on intel2 (tracepoints inaccessible), so scheduler wake-ups were
read from `/proc/<pid>/task/<tid>/status` voluntary_ctxt_switches instead.
Product defects found: none from this task's mechanism; the gate's base failure
is K55 (already registered). No product change.

**Verdict: PASS — measured, no change.** The stop rule is "kicks per drain
under 2, OR the syscall share of the pump thread under 2 %". The pump thread's
own eventfd syscalls (poll + drain-read per cycle) cost at most **~1.1 % of one
CPU** at the highest measured cycle rate, and the *entire* candidate saving —
every redundant kick write on every producer thread — is bounded by **~0.8 % of
one CPU per daemon** at the worst run. Step-down runs, the failover workload
this optimization would target, show kicks/drain of **1.12–1.42**: no redundancy
to remove. The pending-flag protocol is not built.

## Purpose and checklist item

Task AU of the Stage 1 wake-ups/polling block (AS–AU):
`src/server/raft_host.c::host_pump_kick` does a non-blocking eventfd write on
every call from proposal/read/inbox/control paths; the pump polls and drains.
The kernel counter already coalesces notifications; the only candidate saving
is the write syscall while work is already signalled. Checkbox: AU.

## Tested commit and build identity

- Source: devel `9c4bfd171ccc5450ae852a2597351150e543730a`, branch
  `agent/devel-au` (no product commit; this page plus checklist/README).
- Measured build: instrumented `tests/faults/efsd` of the same commit built
  2026-10-29 on intel2 (see Method). Gate build: `efsd
  9c4bfd171ccc5450ae852a2597351150e543730a (branch agent/devel-au, built
  2026-10-29T21:04:22Z on intel2)`.

## Host and configuration

intel2 (8 CPUs), quiet before each stage. D4 runs: fresh 3-member loopback TCP
cluster per run (`tests/k33_daemons.py`, instrumented faults daemon,
`EFS_RAFT_OBS=1`, packing on a 1 s age, quota 4G), one FUSE mount, 8 or 32
writers creating 4 KiB files with fsync for 60 s. Step-down runs:
`python3 tests/test_report_stepdown.py` with `EFS_RSD_KEEP=1` (rc=0 on all
three; logs harvested, stores pruned).

## Method (measurement)

Temporary, **uncommitted** counters in `raft_host.c` (transformer and drivers
retained in the artifacts): `host_pump_kick` counts every eventfd write and its
total duration; `host_pump` records its tid and counts cycles, eventful drains
(poll returned POLLIN) and timer wake-ups; cumulative values print on the
existing ~5 s `raft-obs` cadence as `au-prof` lines. The pump thread's CPU and
voluntary context switches (scheduler wake-ups) come from
`/proc/<pid>/task/<tid>` sampled around the load.

Three cost readings, so the stop rule's "syscall share" is covered under any
interpretation:

- pump thread's own eventfd syscalls: (drains + timeouts) cycles/s × 2 syscalls
  × ~1 µs;
- total kick-write time across all producer threads (`kick_us`), and the
  savable part of it, (1 − 1/(kicks per drain)) × `kick_us`.

## Results

D4 load (60 s runs, per daemon; medians and ranges across 3 reps):

| config | node | kicks/drain | kick write total (per 60 s) | savable share of one CPU | pump CPU | pump wake-ups/s |
|---|---|---|---|---|---|---|
| 8 writers | n1 | 2.46–2.80 | 0.55–0.73 s | 0.6–0.8 % | 8.5–9.6 s | ~1,500–2,000 |
| 8 writers | n2 | 3.84–4.47 | 0.18–0.41 s | 0.2–0.6 % | 6.2–7.5 s | ~1,100 |
| 8 writers | n3 | 4.02–4.88 | 0.18–0.49 s | 0.2–0.7 % | 6.2–7.7 s | ~1,100–1,300 |
| 32 writers | n1 | 1.58–3.95 | 0.52–1.26 s | 0.5–0.8 % | 9.2–14.5 s | ~1,400–4,100 |
| 32 writers | n2 | 4.66–34.50 | 0.15–0.33 s | 0.2–0.5 % | 6.2–7.7 s | ~1,000 |
| 32 writers | n3 | 5.48–33.15 | 0.17–0.39 s | 0.2–0.5 % | 5.9–7.7 s | ~950 |

Step-down (`test_report_stepdown.py`, per daemon, 3 reps): kicks/drain
**1.12 / 1.13 / 1.14 / 1.15 / 1.16 / 1.18 / 1.20 / 1.35 / 1.42** — near 1:1,
no redundancy. Pump cycles there are dominated by real failover work (timeouts
of 33–64k per run are election/tick activity, not empty polling).

Reading of the numbers:

- Where kicks/drain is high (17–34 on some D4 nodes), the redundancy is real
  but the *total* kick-write cost is only 0.15–0.39 s per 60 s, so even
  eliminating 95 % of it saves ~0.2–0.5 % of one CPU. Where the total is
  highest (1.26 s), the ratio is 1.58 and the savable part is ~0.8 %.
- The pump thread's own eventfd syscall cost peaks at ~5.6k cycles/s × 2
  syscalls × ~1 µs ≈ 1.1 % of one CPU (d4-w32-r3 n1); its CPU is 10–24 % of one
  CPU of real raft work — the eventfd path is a small slice of it.
- Both clauses of the stop rule are therefore satisfied in substance: the
  syscall share is under 2 % on every reading, and step-down shows kicks/drain
  near 1. No pending flag, no interleaving model test, no code change: nothing
  measured justifies them.

## Gate

`make test-all` on intel2 via `scripts/make_test_summary.sh`, 8 jobs, clean
(uninstrumented) tree of the same commit:

`TOTAL 546 FAIL 2 NOTRUN 11 STATUS FAIL (821s) OVERBUDGET 4 BUILD 31s`

## Limitations

- `perf sched` NOTRUN on intel2 (tracepoints inaccessible); scheduler wake-ups
  were measured as voluntary_ctxt_switches/s from /proc — a superset of
  eventfd-driven wake-ups (it also includes mutex waits), used here only as
  context, not as the decision metric.
- Loopback TCP on intel2 only; RDMA hosts could raise message rates, but the
  saving is bounded by the measured kick-write cost, which scales with the same
  rates — the step-down ratio near 1 is the structural answer.
- No product change, so no new regression test, no budgets row, no settings
  inventory row; every wake-up path and the timer cadence are untouched.

## Artifacts

intel2 `~/efs-artifacts/taskAU-20261028/`: `measure/*.json` (one per run),
`measure/*.log`, per-daemon `*-n*.log` (au-prof and raft-obs lines),
`matrix.log`, `au_prof_patch.py`, `au_measure.py`, `au_run.sh` (temporary
tooling), `gate.log`, `summary.txt`, `timings.tsv`, `SHA256SUMS` (manifest
sha256: `69ed3e6c3125f8201788cc7749e26267fa3808dbd5482dfff9878c92387245ac`). Scratch trees deleted after the gate; the
measurement is reproduced from devel `9c4bfd171` plus the retained patch
script.
