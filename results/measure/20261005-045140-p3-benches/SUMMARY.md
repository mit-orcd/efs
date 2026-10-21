# P3 benches — first `efsd --bench` + `efs-fuse --bench` run (20261005-045140)

## Purpose and linked item

First run of the two P3 bench tools: `efsd --bench data|meta` ("Single-node
storage bench", `docs/backlog/work-items.md`, asked Oct 2 2026) and
`efs-fuse --bench cpu|put|write` ("Client bench", same file). Both items
record this run's `SUMMARY.txt` as their Oct 5 status. Landed by commit
`6a4ed80e6 bench: P3 — efsd --bench data|meta + efs-fuse --bench cpu|put|write`.

## Verdict

PASS (tools landed, unit suites gated, first numbers recorded) — with an
explicit scope caveat: the host is a dev VM, not an fcstor node, so the
1→6 real-NVMe path-scaling question and the two-host client ladder remain
owed. Not a release gate.

## Source commit and build identity

Not recorded in the run (no deployed-version/sha256 files). The run is tied
to commit `6a4ed80e6` by the adding commit; fio 3.37 is the only tool
version recorded.

## Environment

- Host: `efs1` of the dev Tart cluster — 2 vCPU, 2.8 GiB RAM, one virtio
  disk (vda, host-cached); explicitly NOT an fcstor node.
- All six bench storage roots (p1..p6) are directories on the same vda3
  device, so the path-count curve runs on one physical device.
- Server: dev-cluster efsd at 192.168.2.4:17432; client benches over TCP
  from the same host.
- Bench used `--direct-io` for the data store; 5 s data rounds, 10 s meta
  phases.

## Method

Sequential benches on efs1 (server benches first, then client benches):

```
efsd --bench data --storage p1..p6 --time 5 --writers 8 --direct-io
efsd --bench meta --meta-storage /var/tmp/efs-bench-meta --time 10
efs-fuse 192.168.2.4:17432 fs /tmp/bench-mnt --bench cpu|put|write
efsd --bench data ... --time 2 --perf        # top-symbols line
```

Ladders: paths 1–6 × QD 1/16/64/256; exact p50/p99 per round; fio ceilings
(FIO_CEIL lines) in the same log; iostats/diskstats cross-checks. Bench
roots are scratch; the bench export tree is removed between path counts and
afterwards.

## Results

fio ceilings on this host (FIO_CEIL lines):

| workload | result |
|---|---|
| wseq64k O_DIRECT+end_fsync | 51.6 MiB/s, 825 IOPS, clat 1209 µs |
| rseq64k O_DIRECT | 49.6 MiB/s, 793 IOPS |
| wsync4k write+fsync (WAL bar) | 27 ops/s (~36.5 ms/write+fsync) |
| rrand4k O_DIRECT | 487–2025 IOPS (host cache varies) |

Key engine numbers:

| bench | result |
|---|---|
| data write QD16 paths=1 | 680 ops/s = ~82 % of the 825-IOPS disk ceiling; flat-to-degrading at higher QD (disk saturated, diskutil 82–99 %) |
| data read QD256 | 12.1–12.6 k ops/s regardless of path count |
| meta kv-write QD1 | 27.0 ops/s, p50 36.1 ms — exactly the fsync ceiling |
| meta raft-append QD1 / batch32 | 26.9 ops/s / 950 entries/s (fsync amortized ×32) |
| fuse cpu | 1 thread 1.44 GiB/s; ≥2 threads 2.85 GiB/s (2 vCPU saturate) |
| fuse put QD1 | 1416 chunks/s, p50 458 µs, 0.26 GiB/s stored |
| fuse write | 51 → 71 MiB/s at 1 → 16 files; 64 files OOM-killed on the 2.8 GiB VM (resource limit, not a bench defect) |
| --perf top symbol | 42.69 % `blake3_hash_many_neon` (read-verify hash); wall was the disk |

Interpretation (per the plan's reading key): cpu ≫ put ⇒ latency-bound;
put ≈ write ⇒ the pipeline adds nothing over the per-fragment round trip on
this host. A few EFS_ERR_BUSY slot-handoff errors at QD256 (1–7/round)
recorded under multi-second disk stalls.

## Limitations and remaining gates

- Single shared virtio disk: the 1→6 path curve is noise; owed: a run on a
  named 6-NVMe fcstor host.
- Two-host client ladder (plan rule 6) not run.
- dd level not re-measured; fcstor baseline (1.3–1.5 GB/s) stands.
- 64-file fuse write level blocked by VM RAM, not re-run on bigger host.

## Evidence index

- `SUMMARY.txt` (a080ea95d6d00c6e) — original full run summary with all ladders
- `efsd-bench-data.log` (35c6667017429b40) — data write/read ladders, fio ceilings, diskstats
- `efsd-bench-meta.log` (a306ab6f87b3666a) — kv/raft meta benches
- `efsd-bench-data-perf.log` (1f72f103f241af84) — --perf top-symbols run
- `fuse-bench-cpu.log` (9dce622f8542c504) — client arithmetic ceiling
- `fuse-bench-put.log` (7566aaad8b397209) — put round-trip ladder
- `fuse-bench-write.log` (e77b8e01557a2121) — write pipeline levels incl. OOM-kill round
