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
- Client hosts need `fs.pipe-max-size` ≥ 8 MiB — see the repo
  [README](../../README.md#what-you-need).
- Supported FUSE surface (`chmod`/`chown`/`truncate`/`rename`/`utimens`,
  the lookup-only `.stats` virtual file):
  [operations.md § FUSE surface](../operations/operations.md#fuse-surface).

## Environment variables (client, unless noted)

| Variable | Effect |
| --- | --- |
| `EFS_CLIENT_META_MB` | client staging-table cap in MB (default 256); the evictor drops whole cold tabs above it |
| `EFS_DCACHE_BYTES` | client dirty-cache byte budget (default 2 GiB) |
| `EFS_CLIENT_CONNS_PER_NODE` | pooled connections per server node |
| `EFS_READ_PREFETCH` | read prefetch depth override |
| `EFS_READ_VERIFY=1` | re-hash fragments on read (integrity is otherwise server-side: write-time verify + scrubber) |
| `EFS_FUSE_SPLICE_READ` | toggle `FUSE_CAP_SPLICE_READ` (saves one user copy per written byte) |
| `EFS_STATS_TTL_MS` | `.stats` rollup cache TTL (default 1000) |
| `EFS_FEATURES_TTL_MS` | exported-features cache TTL (default 2000) |
| `EFS_NUMA_NODE` | `=none` disables NUMA pinning at startup; `=N` forces node N |
| `EFS_RDMA_SPIN_US` | RDMA completion poll spin before sleeping |
| `EFS_RDMA_BUFS` | RDMA buffer pool size |
| `EFS_LOG_TS` | stamp every daemon log line with UTC timestamps |
| `EFS_DCACHE_TRACE=1` | client dcache/publish trace (the `report` / `dcache` lines) |
| `EFS_REPORT_DBG=1` | client REPORT pack + verdict detail |
| `EFS_RPC_PROF=1` | client per-RPC profiling counters |
| `EFS_STAGE_DBG=1` | client staging-table debug |
| `EFS_FLOCK_DBG=1` | client POSIX-lock debug |
| `EFS_GC_DBG=1` | **server:** log every GC frag/reap pass (otherwise only passes over 5 ms print a `gc-pass` line) |
| `EFS_GC_DISABLE=1` | **server:** disable the GC loops (debug only) |
| `EFS_RAFT_OBS=1` | **server:** `raft-obs` observability lines (incl. `pub_p50`/`pub_max`) |
| `EFS_PERF_PATH` / `EFS_STRACE_PATH` / `EFS_STRACE_EXPR` | recorder output paths / strace filter for the `--perf` / `--strace` wrappers |
| `EFS_FAULT_WITHHOLD` / `EFS_FAULT_REJECT_PUBLISH` | **not implemented** — proposed D27 fault hooks; would compile only with `EFS_FAULTS=1` ([decisions.md](../status/decisions.md)) |

## Quotas and capacity

- Per-storage-path quotas (`--quota`, suffixes `T/G/M/K`), `ENOSPC` when two
  or more nodes are full: [operations.md § Quotas](../operations/operations.md#quotas).
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
  (Σ min(cᵢ, M) ≥ 3M), not physical bytes; `df` also lags a large truncate
  by the background sweep — [architecture.md §7.3](../how-it-works/architecture.md),
  [operations.md § Quotas](../operations/operations.md#quotas).
- `du` (`st_blocks`) counts chunks present per lane; a file this client did
  not write reports its server-side count, not size/512 — decision D17 in
  [decisions.md](../status/decisions.md).
