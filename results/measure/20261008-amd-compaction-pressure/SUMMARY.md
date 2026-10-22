# SUMMARY — results/measure/20261008-amd-compaction-pressure

AMD compaction-pressure campaign: finite unchanged-cap fault experiment with
sampled peaks, ack latency percentiles, snapshot import and cold-value oracles.

## Purpose and linked item

- Compaction under memory/back-pressure (branch devel-compaction-pressure;
  deploy acceptance in `20261008-amd-compaction-deploy`).

## Verdict

**PASS** (experiment completed): 2240 acknowledged 1 MiB writes, p50 10.7 ms /
p99 21.1 ms / max 1568 ms, 0 max retries; cap held 35.17 s, sampled recovery
4.23 s; snapshot import observed; 3 cold-value oracles passed; fuse 32 files ×
256 KiB, max fsync 10.6 ms. Peaks (node1): RSS 1.67 GB, L0 8.4 MB, memtable
3.1 MB, kv physical 25.2 MB, log physical 2.43 GB, replication lag 1.

## Source commit and build identity

`source-sha256.txt` pins efsd/efs-fuse/efs-mgmt (efsd 3d182c5a…, efs-fuse
2ed425cb…, efs-mgmt 25f8b2ff…). Branch devel-compaction-pressure.

## Environment

AMD host, same-host nodes (per summary.json node1/node2/node3), capped memory
pressure experiment; full unit run in `full-tests-final.log` (incl.
test_store_containers).

## Method

Finite unchanged-cap fault experiment: pressure applied, cap held, recovery
sampled; publication-pending and mount-borrow logs; `cap-05/` sub-run.

## Results

| metric | value |
|---|---|
| acknowledged writes | 2240 × 1 MiB (p50 10.7 ms, p99 21.1 ms, max 1.57 s, 0 retries) |
| held cap | 35.17 s |
| sampled recovery | 4.23 s |
| cold-value oracles | 3 pass |
| snapshot import | observed |

Scope: peaks are samples, not universal/permanent-stall throughput guarantees.

## Limitations and remaining gates

Single-cap finite experiment; see scope note above (from summary.json).

## Evidence index

- `summary.json` — campaign record (run `shasum -a 256 summary.json |
  cut -c1-16` here)
- `source-sha256.txt`, `full-tests-final.log`, `host-pressure-policy.log`,
  `publication-pending.log`, `mount-borrow.log`, `restored-health-final.log`,
  `build-final.log`, `cap-05/`
