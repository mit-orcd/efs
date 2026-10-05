# io-stats — always-on per-op-class data-plane counters

[Backlog](README.md) · [Status (the queue)](../status/README.md)

**Status:** landed (Oct 4 2026, dev cluster; see Verification).

## Problem

Performance questions on the data plane ("is the read slow, or the disk
write? how many PUTs is this node actually serving?") had no always-on
answer. `raft-obs:` covers the metadata pump; `read-prof` and
`EFS_RAFT_OBS` are opt-in env diagnostics. What was missing is a cheap,
always-on counter set for the two data-plane op classes — GET_CHUNK and
PUT_CHUNK — plus the disk-write half of a PUT, so an operator (or the
portal) can diff counters and see ops/s, bytes/s, error rate and latency
without restarting anything.

## Design

Three op classes, in `src/server/iostats.c`:

| class | where | what it times |
| --- | --- | --- |
| `get` | `EFS_MSG_GET_CHUNK` handler (conn thread) | whole request: export acquire, disk read, read-verify hash, reply send |
| `put` | `EFS_MSG_PUT_CHUNK` handler (conn thread) | whole request: length checks, `efs_store_put` (incl. writer-pool wait), reply send |
| `wdisk` | writer-pool `run_job` (writer.c) | the disk write itself, pooled and inline paths |

Per class: `ops`, `bytes`, `errors`, `us_sum`, `us_max`, and a 64-sample
latency ring for p50 (insertion-sort at read time, copied from the
`raft-obs` publish-wait ring in raft_host.c). Counters are plain `u64`s
updated with `__ATOMIC_RELAXED` `__atomic` builtins — the same diagnostic
style as `read_prof_add`; torn reads are acceptable. Counters are
cumulative since efsd start; consumers diff.

Two surfaces:

1. **`iostats:` stderr line**, printed from the raft host pump on the
   same ~5 s cadence as `host_obs_dump` (own throttle; *not* gated on
   `EFS_RAFT_OBS`), once any class is nonzero:

   ```
   iostats: get ops=N bytes=B errors=E avg_us=A p50_us=P max_us=M put ops=N ... wdisk ops=N ...
   ```

2. **Mgmt opcode pair** `EFS_MSG_IO_STATS` = 103 / `EFS_MSG_IO_STATS_REPLY`
   = 104 (next free in `enum efs_msg_type`; a new pair, so no existing
   strict `reply_len` check is touched). The reply is a fixed struct of
   u64s (`struct efs_msg_io_stats_reply`: uptime + one
   `struct efs_io_stats_class` per class). `efs-mgmt io-stats <ip:port>`
   prints one line per class with stable key=value columns:

   ```
   io-stats 192.168.2.4:17432 uptime_s=213
     get_chunk  ops=1234 bytes=... errors=0 avg_us=... p50_us=... max_us=...
     put_chunk  ops=...  bytes=... errors=0 avg_us=... p50_us=... max_us=...
     disk_write ops=...  bytes=... errors=0 avg_us=... p50_us=... max_us=...
   ```

## Overhead argument

Per instrumented op: two `clock_gettime(CLOCK_MONOTONIC)` calls (vDSO,
~20–40 ns) plus 4–6 relaxed atomic increments and one relaxed ring store.
No locks, no allocation, no printf on the hot path; the `iostats:` line
and the p50 sort run on the pump / mgmt path only. At the measured dev
rate (order 10^4 fragments/s) this is well under 0.1 % CPU; the
before/after bench below shows no throughput movement.

## Portal integration (landed)

`~/git/cluster/portal/portal.py` polls `efs-mgmt io-stats <node>:17432`
per node inside its existing per-node SSH probe, diffs `ops`/`bytes`/
`us_sum` between samples (2 s cadence) for rates, and renders live
`ops/s`, `MiB/s`, interval `avg_us` and cumulative `p50_us` on each node
card. Counters reset only on efsd restart; the portal detects this as a
negative delta and skips the interval (also visible via a dropping
`uptime_s`). The CLI prints raw `us_sum` alongside `avg_us` because
interval latency cannot be reconstructed by diffing rounded averages.

## Verification

Dev cluster (3 Tart VMs, TCP), `bench.sh 512` on efs1 before and after
the change, unit suite via `deploy.sh`:

- Unit tests (deploy run on efs1): all 16 suites PASS
  (test_wire, test_data, test_kv, test_kv_lsm, test_raft_store,
  test_meta_apply, test_raft, test_sim, test_txn, test_session,
  test_erasure, test_placement, test_lock, test_stage_evict,
  test_conn_fd, test_rdma_xprt). The docs gate also passed on efs1.
- After `./cluster.sh restart`, a 64 MiB write + fresh-file cold read:

  ```
  $ ./efs-mgmt io-stats 192.168.2.4:17432
  io-stats 192.168.2.4:17432 uptime_s=168
    get_chunk  ops=170 bytes=11141120 errors=0 avg_us=2954 p50_us=708 max_us=26905
    put_chunk  ops=768 bytes=50331648 errors=0 avg_us=983 p50_us=352 max_us=19079
    disk_write ops=768 bytes=50331648 errors=0 avg_us=169 p50_us=114 max_us=13934
  ```

  (GET counts only moved on the fresh file: an immediately-reread file is
  served by the client's read cache and never reaches the server — that
  is a client property, not a counter gap.)
- `iostats:` lines in `/efs/data/efsd.log` on all three nodes, on the
  5 s cadence once PUTs had landed:

  ```
  2026-10-04T07:47:31.331Z iostats: get ops=0 bytes=0 errors=0 avg_us=0 p50_us=0 max_us=0 put ops=512 bytes=33554432 errors=0 avg_us=761 p50_us=275 max_us=7164 wdisk ops=512 bytes=33554432 errors=0 avg_us=167 p50_us=128 max_us=3784
  ```

- `efs-mgmt status` output unchanged (no protocol regression).
- Bench (512 MiB dd write conv=fsync + cold read + `efs-bench --meta`):

  | run | write | read |
  | --- | --- | --- |
  | baseline | 213 MB/s (2.52 s) | 2.4 GB/s (0.221 s) |
  | with io-stats | 134 MB/s (4.00 s) | 1.1 GB/s (0.511 s) |
  | with io-stats, immediate reruns | 219 / 188 MB/s | 4.2 / 2.2 GB/s |

  The slow middle run did not reproduce: two immediate reruns of the same
  dd pair straddle the baseline, so the delta is VM noise, not the
  instrumentation. Meta op-rates moved up across the board (mkdir 11.5 →
  27.4, create 144.5 → 178.2, getattr 78.8k → 81.9k ops/s), same
  conclusion.

## Out of scope

- Per-export or per-path breakdown (the classes are process-global).
- GC_FRAGMENT and BENCH_PUT classes (GC is control-plane cadence;
  BENCH_PUT does no disk I/O).
- Client-side op stats (the client already has `EFS_RPC_PROF`).
