# Container tracing and CPU profiling · six rounds

Branch: devel-storage-engine. Baseline source: 175fcf98.
12 diagnostic cases (round 1 strace; rounds 2–6 cycles perf), 8 unprofiled
matched original/optimized cases, and 2 final-build functional cases passed.
All cases verified content, restarted all private daemons, verified acknowledged
data again, and required zero data mappings/allocated container blocks on all
three nodes after deletion. Installed service PID/command snapshots are unchanged.
120 perf reports show zero lost samples. Profiling overhead excludes these timings
from throughput comparisons; use matched-summary.json and individual results.

Workload: 256 × 512 KiB random files (128 MiB), four closed-loop gateway workers,
128 KiB durable overwrites on 32 worker-disjoint hot files; complete-file SHA256
reads. TCP, private three-node VM Raft group, private gateway mount, port 27432.
Diagnostic read/overwrite phases 15 seconds; matched phases 30 seconds; final
functional cases have no timed loop. Warm small workload, not proof of saturation.
Deletion rates count unlinks, not physical GC work. Shared VM host and metadata
ReadIndex waits confound throughput, especially buffered reads/deletes.

round1..round6 contain compressed syscall traces, perf reports/statistics,
server logs, inventories and raw workload JSON. round2-source..round6-source
record incremental source/binary identities. matched-identities.json identifies
original/optimized daemon hashes and ABBA repeat order; matched-summary.json
contains median, range and per-repeat p99. container-final-guard-source records
the separately qualified final build: only startup overlap rejection and a test
assertion follow the timed round6 version. container-final-guard is its two-case
cluster evidence. No binaries or raw perf.data are included; perf.data remains
on each private VM under its case root.

Final Linux build/test output: linux-validation.log.gz. SHA256.json covers every
other evidence file. See the [checkpoint](../../../docs/status/container-profiling-20261008.md)
for findings and unresolved limitations.
