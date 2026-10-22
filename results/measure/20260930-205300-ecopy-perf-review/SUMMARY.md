# ecopy perf-dir review — client hot paths under a 400-thread ecopy (Sep 30 2026, 21:10Z)

## Purpose and linked item

Third review of the fstor007 client perf directory (`/home/erbmi1/orcd/scratch/efs/perf/efs-mount/`),
following `20260930-063500-perf-dir-review` and `20260930-080000-ecopy-times-review`. Documentation
only; no code changed. Feeds the pending **D18** decision (staging-table floor vs `EFS_CLIENT_META_MB`
cap) and names the close-path predicate and dcache-chain questions that later became plan items.
Added to the repo by commit `1137dbdac` ("perf-dir review 21:10Z: evictor LRU scan and per-close
chunk walk are the client hot paths under ecopy").

## Verdict

PASS as an analysis deliverable (review completed, findings recorded). No pass/fail gate was run;
the findings are inputs to D18 (still an ask at run time) and to next steps N1–N5, none implemented
in this run.

## Source commit and build identity

- Client (fstor007): `/tmp/efs/efs-fuse` built 19:53Z Sep 30, tree HEAD `44073b77` + the Sep 30
  evictor fix `5ea397fd`.
- Servers: `44073b77249b-dirty` (roll350, 19:43Z), run without `--perf`/`--strace`.

## Environment

Single client fstor007 mounting the fcstor003–006 cluster; transport not recorded in the run.
Workload: `ecopy --verify /data1/erbmi1/software/ /tmp/efs-mount/software/` (~400 threads).
Profiling: `perf record -F 499 -g` on efs-fuse for 3741 s (19:54Z–20:56Z, 13.05 M samples) plus
two `strace` captures of ecopy (86 s and 150 s). Buffered I/O via FUSE; fixture size not recorded.

## Method

Reductions only, from raw perf/strace captures kept outside the repo: `ecopy_syscalls.py` and
`ecopy_ops.py` (both committed in this directory) reduce the straces; `perf-slices.txt` splits the
62-min profile into first 5 % / middle 20 % / last 5 % and annotates `stage_evict_main`. No cluster
restart, no cleanup step; the review ends with next steps N1–N5 for later runs.

## Results

| Finding | Number |
| --- | --- |
| `stage_evict_main` share of client profile | 31.6 % whole run; 47.6 % in the last slice (evictor re-arms on every staging op — D18 floor > cap) |
| `dcache_steal_dirty` in last slice | 36.9 % (per-close walk of every chunk index of the file, dirty or not) |
| efs-side syscall latency (run 1 / run 2) | renameat 54.2 / 28.6 ms avg; openat 18.0 / 7.7 ms; vs idle references 0.3–6 ms (5–30×) |
| efs ops in flight | ≈ 27 (run 1), ≈ 5.6 (run 2) — not 400-way server queueing |
| Mount throughput | ≈ 2300 / ≈ 1050 efs syscalls/s |
| efs errors in either trace | none (no EIO/EBUSY/ESTALE; max syscall 1.686 s) |

Baseline: idle single-thread references from the Sep 30 postfix review (stat 0.33 ms, openat 0.60 ms,
rename 6.2 ms). ecopy's own threads sat on futexes (≈ 247–274 waiting) — its dispatch, not efs,
bounds its throughput.

## Limitations and remaining gates

- No server-side trace for the window (roll350 untraced); the middle 20 % of the profile had an
  untraced workload; the ≥ 8 GiB file triggering `dcache_flush_all_slots` was not identified.
- §3 (long dcache slot chains) is a hypothesis; the gdb chain-walk check (N3) was not yet run.
- Next steps recorded: N1 decide D18 (or measure with `EFS_CLIENT_META_MB=4096`), N2 close-path
  dirty predicate, N3 dcache chain measurement, N4 server trace of one ecopy, N5 ecopy's own dispatch.

## Evidence index

| file | sha256 (16) |
| --- | --- |
| SUMMARY.txt (full review, all sections) | f22133ecc7970a1d |
| by_thread.txt (whole-profile per-symbol shares) | cf7dc5d41c1dd26e |
| perf-slices.txt (time-sliced perf report + annotate) | 8c9186fe6569cbd5 |
| ops-by-fs.txt (per-syscall latency, efs vs local) | 7cc9392249fb6033 |
| syscalls.txt (per-syscall counts/max, ≥1 s calls) | 6826610cf18b394b |
| flat.txt (client perf flat profile) | 6daeed63048ac069 |
| perf-callers.txt (callers of hot symbols) | 2365f81efaf56b14 |
| ecopy_ops.py (reduction script for ops-by-fs) | a4b58d2f7126a929 |
| ecopy_syscalls.py (reduction script for syscalls) | ec6b430cc3d6ad34 |
