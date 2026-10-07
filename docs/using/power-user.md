# Power user — mount options, environment variables, quotas, efs-mgmt

[Docs index](../README.md) · [Quick start](quickstart.md) ·
[Operations](../operations/operations.md) ·
[Architecture](../how-it-works/architecture.md)

One page of pointers to the knobs, with the one-line version of what each
does. The normative text is the linked section, never this page.

## Mounting and transport

- `scripts/client.sh <addr:port> <mountpoint>` mounts; `scripts/client.sh
  stop <mountpoint>` unmounts — [quickstart](quickstart.md) has the minimal
  sequence, [operations.md § Scripts](../operations/operations.md#scripts)
  the full usage including `--perf` / `--strace` recorders.
- `EFS_TRANSPORT=auto|tcp|rdma` — transport; default `auto` (RDMA if
  InfiniBand is up, else TCP). `rdma` is strict (no TCP fallback).
  `EFS_RDMA_DEV=<ibdev>` pins the device.
- Splice measurement runs may need a larger `fs.pipe-max-size`; splice is
  disabled by default, so 8 MiB is not a default-mount prerequisite. See the repo
  [README](../../README.md#what-you-need).
- Supported FUSE surface (`chmod`/`chown`/`truncate`/`rename`/`utimens`,
  the lookup-only `.stats` virtual file):
  [operations.md § FUSE surface](../operations/operations.md#fuse-surface).

## Environment variables (client, unless noted)

| Variable | Effect |
| --- | --- |
| `EFS_CLIENT_META_MB` | client staging-table cap in MB (default 256); the evictor drops whole cold tabs above it |
| `EFS_DCACHE_HARD_BYTES` | shared body-admission budget (default 256 MiB; not a process RSS cap) |
| `EFS_DCACHE_DRAIN_BYTES` | reserved drain/recovery body capacity (default 64 MiB) |
| `EFS_DCACHE_BYTES` | soft dirty-body reclaim target in bytes (default 128 MiB); not the hard allocation bound |
| `EFS_CLIENT_CONNS_PER_NODE` | pooled connections per server node |
| `EFS_READ_PREFETCH` | read prefetch depth override |
| `EFS_READ_VERIFY=1` | enable payload re-hashing in the single-fragment helper (off by default); the common parallel pair bypasses this switch ([W75](../backlog/work-items.md#w75)); server GET checks stored digests where present |
| `EFS_FUSE_SPLICE_READ` | opt into `FUSE_CAP_SPLICE_READ` for measurement (default off); the recorded profile found no gain, so copy savings are not an accepted result |
| `EFS_STATS_TTL_MS` | `.stats` rollup cache TTL (default 1000) |
| `EFS_FEATURES_TTL_MS` | exported-features cache TTL (default 2000) |
| `EFS_NUMA_NODE` | `=none` disables NUMA pinning at startup; `=N` forces node N |
| `EFS_RDMA_SPIN_US` | RDMA completion poll spin before sleeping |
| `EFS_RDMA_BUFS` | RDMA buffer pool size |
| `EFS_LOG_TS` | UTC timestamps are enabled by default; `=0` disables the timestamp wrapper |
| `EFS_DCACHE_TRACE=1` | client dcache/publish trace (the `report` / `dcache` lines) |
| `EFS_REPORT_DBG=1` | client REPORT pack + verdict detail |
| `EFS_RPC_PROF=1` | client per-RPC profiling counters |
| `EFS_STAGE_DBG=1` | client staging-table debug |
| `EFS_FLOCK_DBG=1` | client POSIX-lock debug |
| `EFS_GC_DBG=1` | **server:** log every GC frag/reap pass (otherwise only passes over 5 ms print a `gc-pass` line) |
| `EFS_GC_DISABLE=1` | **server:** disable the GC loops (debug only) |
| `EFS_RAFT_OBS=1` | **server:** `raft-obs` observability lines (incl. `pub_p50`/`pub_max`) |
| `EFS_PERF_DIR` | recorder directory for `server.sh` / `client.sh` wrappers (default under `~/orcd/scratch/efs/perf`) |
| `EFS_PERF_PATH` / `EFS_STRACE_PATH` | output paths for recorders started by the binaries themselves |
| `EFS_STRACE_EXPR` | strace syscall filter |
| `EFS_FAULT_WITHHOLD=<ino>:<ci>` | **client:** implemented only in `EFS_FAULTS=1` builds; omits that REPORT record before serialization; `/tmp/efs/fault` containing `WITHHOLD <ino>:<ci>` overrides the environment, and `OFF` disables it |
| `EFS_FAULT_REJECT_PUBLISH` | proposed server hook in the D27 decision; no implementation found in the reviewed source ([handoff](../status/in-flight.md)) |

## Quotas and capacity

- Per-node quota across its storage paths (`--quota`, suffixes `T/G/M/K`);
  a PUT returns QUOTA when at least two stripe replies are quota failures: [operations.md § Quotas](../operations/operations.md#quotas).
- Storage layout, auto-rejoin, writer threads and `--direct-io`:
  [operations.md § Storage layout](../operations/operations.md#storage-layout),
  [§ Auto-rejoin](../operations/operations.md#auto-rejoin),
  [§ Writer threads and direct I/O](../operations/operations.md#writer-threads-and-direct-io).

## efs-mgmt

`status`, `mkfs`, `add-node`, `add-storage`, `shrink-quota`, `raft-status`,
`raft-change`, `raft-dir`, and a `raft-<op>` for every metadata operation:
[operations.md § Cluster management](../operations/operations.md#cluster-management).

## `df` / `du` semantics

- `df` total/avail comes from the 3-of-N placement bound over node quotas
  (Σ min(cᵢ, M) ≥ 3M), not physical bytes; usage can lag unlink cleanup; public logical truncate remains staged,
  and physical `/data1` free space is a separate measurement — [architecture.md §7.3](../how-it-works/architecture.md),
  [operations.md § Quotas](../operations/operations.md#quotas).
- `du` (`st_blocks`) counts chunks present per lane; a file this client did
  not write reports its server-side count, not size/512 — decision D17 in
  [decisions.md](../status/decisions.md).
