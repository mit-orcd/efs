# NUC ReadIndex investigation, Oct 8

Read-only live investigation; no daemon restart, code deployment, leadership change or durability change. Installed build devel-find-stats befaee95, distinct from this branch. `probes.json` preserves 48 successful attribute probes (12 per group/endpoint), alternating group order, on existing inodes 11434383 (group 0) and 1946256 (group 2), plus before/after status. `probe.py` is the exact successful runner. One preliminary runner completed its probes but failed to serialize because its after-status dictionary was missing; it was corrected and rerun. Only the successful rerun is retained as the measured probe dataset.

`summary.json` and node*-window.log come from the earlier approximately 30-second all-process strace capture, not the later untraced probes. Full capture remains at /data1/efs/evidence/bench/tiny-strace-20261008T2100a and locally under outputs/tiny-strace-20261008. Source excerpts were read from the installed source tree; they are not a claim of reproducible source-to-binary identity. The before snapshot of the original capture retains executed binary hashes.

Probe times include launching the management CLI on the remote host, socket setup and request processing; they are end-to-end milliseconds, not isolated server handler times. Existing workload was active; group write pressure was unequal. One inode per group and 12 samples per cell do not establish general capacity or a controlled hardware comparison.

See ../../../docs/status/metadata-group-perf-followup-20261008.md for interpretation and next gate.

**Raw evidence note (2026-10-23):** the bulk data in `bench` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
