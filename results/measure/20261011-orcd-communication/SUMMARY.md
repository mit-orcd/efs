# Summary — 20261011-orcd-communication

## Purpose and linked item

Derived analysis of the ORCD communication capture
`communication-20261011T045504Z` for work items W109 (metadata/query
contention) and W110 (EFS/IB/TCP communication diagnosis). Linked status
pages: `docs/status/orcd-communication-20261011.md` and
`docs/status/orcd-communication-hypotheses-20261011.md`.

## Verdict

NOTRUN (no gate executed). This is a diagnostic capture/analysis only: an open
investigation with no code, configuration or deployment change. The status
page states explicitly that this capture "did not rerun or close" the loaded
POSIX gates; the earlier repeated-load verdict (1,485 pass, 27 fail, 7 skip,
28 NOTRUN) remains owned by `orcd-integration-20261011.md`. W109/W110 remain
open.

## Source commit and build identity

Deployed runtime `52bc8edb` (recorded in the linked status page). No version
or binary-identity file is present in this run directory itself — the three
JSON files contain counters, logs-derived timings and perf summaries only.

## Environment

- ORCD cluster (`orcd-test`), eleven hosts fcstor003–fcstor013, native
  InfiniBand/RDMA (mlx5_2), direct I/O; four storage daemons and seven FUSE
  mounts.
- Live workload during capture: existing 16-worker `tiny_files.py` benchmark
  on fcstor007 (~1.03–1.06M files in the query catalog at capture time).
- Capture windows: five 30-second C CPU perf captures (four daemons + one FUSE
  client); IB/TCP counter snapshots ~40.6 s around perf and ~91.5 s around the
  initial traces on all eleven hosts; separate ~4 s decoded traces.

## Method

No rerun: all data derives from one capture. `analysis.json` aggregates
per-host IB error-counter deltas (perf and whole windows), TCP socket
deltas/retransmissions, perf self-time breakdowns, apply-wait statistics and
global `/proc/net/netstat` deltas. `historical-failure-window.json` counts
raft-host lane-sweep `rc=-13` events per inode and kv-compact activity in a
60 s historical failure window (04:33:30–04:34:30 UTC).
`hypothesis-evidence.json` records query-publication timings (generation,
build_ms, refolded rows, file deltas), apply waits, compaction intervals and
TCP recovery deltas feeding the repair-hypothesis document. All tracers were
detached afterwards; final status showed four nodes up, heal idle.

## Results

| Finding | Value |
| --- | --- |
| EFS-attributable TCP retransmissions | 2 (fcstor004→007, fcstor006→007); 35 host-wide |
| mlx5_2 error/drop/retry counters (25 selected, 11 hosts) | All unchanged (zero deltas) |
| fcstor004 Raft apply waits | 15 waits, 21.668–50.117 ms, median 23.895 ms |
| Query-build thread inclusive sampled CPU | 19.87% (fcstor004), 42.96% (fcstor005) |
| fcstor007 FUSE client self overhead | receive-wait 8.42%, BLAKE3 AVX2 21.84%, buffer-copy 9.14%, allocator 8.63% |
| fcstor007 TCP side-channel receive | ~1.33 GiB over 40.6 s (compiled RDMA pool is 84 KiB; larger frames use TCP) |
| Query publication build time | ~7.1–7.5 s typical, one 20.5 s outlier (generation 310); ~2.8–3.0k files/generation delta |
| Historical failure window | Repeated lane-sweep `rc=-13` on four inodes (128 events fcstor003, 256 fcstor004) plus kv-compact activity |

Interpretation per the status page: CPU samples are not request latency;
zero counter growth does not exclude transient RNR starvation; correlation
with compaction/lane-sweep does not establish causality.

## Limitations and remaining gates

- No per-op lost-message rate available; message types of the TCP
  side-channel bytes were not decoded.
- No pure-TCP cluster-mode comparison or switch congestion capture performed.
- W109 closure still requires reproduction of the original timeouts with
  unchanged deadlines/oracles and timing correlation; W110 requires
  timestamped request identities with endpoint/device mapping during a
  reproduced stall.
- Historical ACK/CQ error totals predate the baseline and have no timestamp
  or process attribution.

## Evidence index

| File | sha256 (16) |
| --- | --- |
| analysis.json | 6e32a49be5eb93fc |
| historical-failure-window.json | ca83db480bccc984 |
| hypothesis-evidence.json | b0d959c7527ddbcd |

Bulk profiles, perf reports, traces and raw snapshots are outside this
directory: `~/orcd/scratch/efs/orcd-test/communication-20261011T045504Z/HOST/`
on ORCD (recorded in the linked status page).
