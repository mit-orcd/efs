# Container profiling rounds 7–8

Follow-up from branch devel-storage-engine, baseline 73b402cd. Four diagnostic
cases passed; all 48 perf reports have zero lost samples. The same private
three-node xorinox VM group and gateway ran 256 × 512 KiB files, four workers,
and 128 KiB durable overwrites across 32 hot files. Each case verified hashes,
restarted all three daemons and reverified, then required zero data mappings
and allocated container blocks after deletion on all nodes. Installed-service
PID/command snapshots matched before/after each batch.

container-round7 and container-round8 contain raw workload JSON, inventories,
compressed server logs and perf reports/call stacks, and perf stat text.
The matching source directories record source and measured binary identities.
profile-summary.json normalizes three-daemon task clock/cycles/context switches
per client MiB. These diagnostic phases last 15 seconds and include profiling
overhead; no throughput comparison should use them.

The matched unprofiled matrix uses 30-second read/overwrite phases, ABBA version
order, reverse mode order in the later pair, and two repeats per mode/version.
All eight matched cases passed the same integrity/restart/reclamation/process
gates. matched-summary.json contains median, min/max and per-repeat p99;
matched-identities.json records daemon hashes and batch order. Original is the
final build from rounds 1–6, optimized is round 8. final-source records the
committed runtime (comment-only differences from measured sources), the final
atomic test counter and checksum-header build dependencies. A clean Linux
`make clean && make -j4 all && make test` passed with zero compiler warnings;
see linux-validation.log.gz. SHA256.json covers every other evidence file.
No binaries or raw perf.data are included; raw perf.data stays on the VMs.

See the [checkpoint](../../../docs/status/container-profiling-rounds7-8-20261008.md).
