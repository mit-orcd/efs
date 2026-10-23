# 20261009-w97-pump-phases — W97 pump-phase diagnostic reproduction

## Purpose and linked item
Work item W97 (replication / persistence throughput collapse investigation).
Linked status page: `docs/status/w97-solution-investigation-20261009.md`, which
references this run's README as "Raw phase evidence and method". The run
reproduces the W97 throughput collapse with opt-in pump-phase timing
instrumentation (queue residence, pump-lock, inbox-drain, KV WAL release,
applied-index persistence histograms) to attribute where time goes.

## Verdict
PASS (diagnostic run completed successfully and reproduced the collapse). Core
POSIX suite 212/212 in 70.8 s, loader exited 0, clean restart, 32 sampled cold
payloads verified on both mounts. The run's *finding* is a failure of
throughput: rate decayed 113.40 → 50.55 → 55.80 files/s over three 100 s
windows, with 94,175 dropped replication frames on node 3. Peer/fault suites
were not repeated in this run.

## Source commit and build identity
- efsd 2cca7cfc, branch `devel-persistence-pressure`, built 2026-10-09T15:55:05Z
  on host `nuc` (`deployed-version.txt`).
- Binary sha256 (`deployed-sha256.txt`): efsd `744e9811…`, efs-fuse `c8643f41…`.
- Same production replication policy as a770290a; observation-only build, timings
  do not control admission, ACKs or durability.

## Environment
- Single host `nuc`; 4-node efsd cluster on loopback (127.0.0.1 ports
  17432–17435), node 1 leader, nodes 2–4 joined.
- Storage: nodes 1/2/4 under `/data1/efs/n*`, node 3 under `/data2/efs/n3`;
  per-node quota 187.5G; `--direct-io` on all nodes.
- Transport: TCP loopback. Feature: opt-in W97 pump-phase observation
  histograms enabled; no compilation or functional tests overlapped the
  measured workload.

## Method
- Deploy via `deploy.sh`; workload driven by `loader.py` /
  `/data1/efs/evidence/w97-replication-loader.py --max-depth 8 --max-files-per-folder
  1000 --max-files 2000000 --workers 8
  /data1/efs/mnt/tiny-w97-pump-phases-20261009` (seed 970001, tiny-file
  distributions, 8 workers, 300 s window, cooperative SIGUSR1 drain).
- Per-node baselines captured before start (`baseline-n*.json`); observations
  sampled per node (`n*-observations.jsonl`, `n*-window.log.gz`); analysis by
  `capture.py` + `analyze.py`; teardown/verification by `finish.sh`
  (core POSIX suite, restart, cold-payload sha256 oracle, process-environment
  verification with observations disabled).

## Results
| Window (s) | files/s |
|---|---|
| 0–100 | 113.40 |
| 100–200 | 50.55 |
| 200–300 | 55.80 |

- Loader: 22,181 files created in 300.5 s (exit 0).
- Node 3 (SATA spindle path): inbox_drop 94,175 (34,276 data / 59,899 empty),
  sync_us p50 ≤ 8192 µs, inbox_age_us p50 ≤ 262 ms, pump_drain_us p50 ≤ 8 ms.
- Node 4: zero drops; node 1/2 normal sub-millisecond pump phases.
- Core POSIX: 212/212 pass in 70.8 s.

## Limitations and remaining gates
- Queue age includes shared-inbox wait and earlier frames in the same drain;
  drain timing includes decode + Raft persistence + metadata apply (not pure
  disk time). Histograms include idle pump cycles; timing overhead is
  uncalibrated.
- Dropped frames are refused replication messages at the bounded receiver
  inbox (Raft retransmits); counters alone do not establish per-message
  retries, data-loss absence, or recovery latency.
- Peer/fault suites not repeated; see the W97 status page for attribution and
  remaining work.

## Evidence index (sha256 first 16 chars)
- `README.md` `d1cd7a946cd7b59e` — run method/verdict write-up
- `analysis.txt` `cf64e73b58245303` — per-node counters and phase histograms
- `analysis.json` `0ee69a4dc2a8f481` — machine-readable histogram windows
- `rates.json` `db0a107a61e584a7` — three-window files/s rates
- `posix.tsv` `11e3f30528c9dd8c` — core POSIX 212/212 result
- `complete.json` `bc072ad99f0d57da` — completion status and server table
- `loader-exit.json` `14a4bc75b27d855a` — loader exit 0, 300.5 s
- `identity.json` `7891cb84d3a56ca3` — 4-node topology/commands
- `deployed-version.txt` `7a7d66975bd0f10d` — build identity 2cca7cfc
- `cold-payload-oracle.json` `ed61e902e2de3c74` — 32-file post-restart sha256 oracle
