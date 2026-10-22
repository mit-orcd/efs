# 20261010-w97-three-drive — summary

## Purpose and linked item

W97: matched three-node drive comparison (NVMe-only vs SATA-only), requested
after the earlier four-node mixed-drive observation showed only the NVMe drive
slowing under a durable tiny-file workload. The NUC persistence-latency
investigation was later retagged W111. Full report:
`docs/status/w97-three-drive-20261010.md`.

## Verdict

PASS (requested comparison completed as specified); W97 itself remains OPEN —
the decline reproduces on both drives but the cause (e.g. NAND cache
exhaustion vs EFS barrier service) is not established. A retained preflight
failure (default four-voter bootstrap refusing the three-voter topology) and
an initial node-4 readiness failure with passing retry are preserved in
`failed-preflight/`; final `tests-exit.txt`, `restore-exit.txt`, `run-exit.txt`
are all zero.

## Source commit and build identity

Source `6b8f05acb484b52284a7ffdd6c1da7b0476f35f6`, build `6b8f05acb484-w97`,
branch `devel-persistence-pressure`, built 2026-10-10T11:47:05Z on nuc.
No production code or drive-setting changes. `identity.json` records efsd /
efs-fuse / efs-mgmt sha256 hashes and flags; `provenance.json` freezes
workload/controller/perf code hashes.

## Environment

- NUC cluster, one fresh private three-node cluster per drive (NVMe first,
  then SATA), 1200 s each; regular four-node service stopped throughout and
  restored after.
- NVMe: Samsung 970 EVO 1 TB, firmware 2B2QEXE7, XFS/LVM, write-back, also
  hosts root/home/swap. SATA: MK0800GEYKE, firmware HPG1, XFS, write-through.
- EFS_MD_RAFT_N=3, both metadata groups hosted on all three nodes (voters 0x7);
  FLOW=1, 10 ms applied interval, TCP transport, direct I/O; commands,
  overlap/no-op/group-drain/early-retire off; batch observation on.
- Private node quotas 80G; guards: >80 GiB physical free, >3 GiB available RAM.

## Method

Scripts in run dir: `drive-test.py` (per-cell driver), `drive-load.py`
(workload generator), `w93_perf.py` (perf tracepoints), `analyze.py`
(minute/five-minute windows, pooled sync means), `health-summary.py`,
`capture-nvme-health.sh` / `capture-sata-health.sh` (read-only telemetry).
Workload: 8 workers, 4 per verified inode-placement group; 1–512-byte
SHA-derived payloads, exclusive CREATE + one write + CLOSE; 2,048 directories
per group prepared (excluded from timing), 1,000 files/dir, 512k files/worker
cap; fixed 30 s settle after preparation; 5 s snapshot sampling. Cold-restart
verification: 64 exact payload checks per drive (128 total). Cleanup: private
clusters drained and stopped, regular four-node cluster and both mounts
restored on the same build, collectors stopped, no private daemon/loader left.

## Results

| Three-node storage | First-min files/s | Final-min files/s | First→final server Raft sync | 20-min files/s | Files |
| --- | ---: | ---: | ---: | ---: | ---: |
| Samsung 970 EVO NVMe | 31.83 | 10.80 | 2.465→6.993 ms | 12.91 | 15,506 |
| MK0800GEYKE SATA SSD | 562.73 | 299.40 | 0.150→0.319 ms | 300.76 | 361,060 |

- NVMe creation falls 2.95× while server sync grows 2.84×; SATA 1.88× / 2.13×.
- Device counters: NVMe flush mean 1.375→3.870 ms, request 1.186→3.403 ms,
  busy 91.4→93.6%; SATA request 0.179→0.409 ms, busy 92.2→95.5% (no SATA
  flush requests reported). Host writes 15.09→4.72 MiB/s (NVMe),
  239.58→134.23 MiB/s (SATA).
- Whole-run CREATE→CLOSE p50/p95/p99: 680.57/884.32/981.01 ms (NVMe) vs
  22.03/49.92/69.56 ms (SATA).
- Zero loader errors, zero Raft sync-error increments; inbox refusals NVMe
  n3=35, SATA n2=15/n3=109 (vs 75,109 in the earlier mixed-drive run).
- NVMe health (146 samples, started after slowdown began): composite
  34.85–45.85°C, no warnings or thermal transitions. No SATA telemetry
  (noninteractive sudo denied; attempt retained).

## Limitations and remaining gates

- One run per case, timed (not matched-inode-count); leader histories and
  dataset sizes differ between drives; NVMe volume carries OS traffic.
- Host bytes do not measure NAND write amplification or cache occupancy;
  busy% is not a hardware-ceiling proof. No SATA temperature data.
- Next discriminating test (still open): matched sustained durable writes
  outside EFS, preallocated overwrite vs growing XFS files, with device
  health and barrier latency recorded.

## Evidence index

- `README.md` — 6171abafb2c49481 — run self-description
- `identity.json` — 1744d0aec8fa1e12 — binary hashes, flags, cell config
- `comparison-summary.json` — 32695615dd70fc6c — pooled per-drive verdict numbers
- `analysis.json` — af7b63a904f6ce45 — minute/five-minute distributions
- `health-summary.json` — ebb1f10de9d4cf5e — NVMe temperature/counter coverage
- `provenance.json` — b1165330599e6e90 — launch code hashes
- `baseline-sata.json` — 5b0369927881e38c — earlier four-node mixed-drive context
- `tests-exit.txt` / `restore-exit.txt` / `run-exit.txt` — 9a271f2a916b0b6e — zero exits
- `nvme/` and `sata/` — raw snapshots, loader/batch logs, cold oracles
- `failed-preflight/` — retained four-voter refusal and readiness retry
