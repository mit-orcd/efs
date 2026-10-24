# Power user — mount options, environment variables, quotas, efs-mgmt

**Accepted deployment contract (D36, Oct 9):** three nodes are the minimum;
four nodes provide the maximum two-node-loss safety target; additional nodes
scale volume and performance across all dimensions. Today's fixed 2+1 data and
three-voter metadata groups do not implement that four-node guarantee. See
[binding requirement and implementation gates](../status/node-count-protection-20261009.md); preserve the requirement
rather than deriving the design from current code.

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
  an active supported verbs device is available, else TCP). `rdma` requires
  successful RDMA setup; it does not silently downgrade a failed upgrade.
  Oversized frames retain the transport's explicit TCP frame path.
  `EFS_RDMA_DEV=<ibdev>` pins the device. Native InfiniBand uses an active
  port/LID; RoCE uses an active Ethernet verbs device and nonzero GID.
  `EFS_RDMA_GID_INDEX=<index>` pins a valid local GID; otherwise discovery
  chooses the first nonzero GID. Set these variables for servers and clients.
- Splice measurement runs may need a larger `fs.pipe-max-size`; splice is
  disabled by default, so 8 MiB is not a default-mount prerequisite.
  Enable only for explicit measurement; the recorded profile found no gain.
- Supported FUSE surface (`chmod`/`chown`/`truncate`/`rename`/`utimens`,
  the lookup-only `.stats` virtual file):
  [operations.md § FUSE surface](../operations/operations.md#fuse-surface).

Example for an already configured RXE device (this does not create it):

```bash
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
```

The device/index must exist locally; use `rdma link` and inspect its GID table.
AMD's [four-mode acceptance](../status/v020-amd-release.md) used software RXE,
not hardware RoCE or cross-host offload. Server `--direct-io` / `--no-direct-io`
selects fragment I/O independently of transport; buffered is the daemon default.

## Environment variables (client, unless noted)

| Variable | Effect |
| --- | --- |
| `EFS_CLIENT_META_MB` | client staging-table cap in MB (default 256); the evictor drops whole cold tabs above it |
| `EFS_DCACHE_HARD_BYTES` | shared body-admission budget (default 256 MiB; not a process RSS cap) |
| `EFS_DCACHE_DRAIN_BYTES` | reserved drain/recovery body capacity (default 64 MiB) |
| `EFS_DCACHE_BYTES` | soft dirty-body reclaim target in bytes (default 128 MiB); not the hard allocation bound |
| `EFS_CLIENT_CONNS_PER_NODE` | pooled connections per server node |
| `EFS_READ_PREFETCH` | read prefetch depth override |
| `EFS_READ_VERIFY=1` | enable payload re-hashing on GET receive paths (off by default); returned digests are checked against the captured metadata view where available; identity-bound integrity/repair remain [W75](../backlog/work-items.md#w75) |
| `EFS_FUSE_SPLICE_READ` | opt into `FUSE_CAP_SPLICE_READ` for measurement (default off); the recorded profile found no gain, so copy savings are not an accepted result |
| `EFS_NUMA_NODE` | `=none` disables NUMA pinning at startup; `=N` forces node N |
| `EFS_MD_RAFT_N` | **removed (W153):** efsd stops when it is set. The member count N (3..16) is the membership `mkfs` commits; node IDs must be 1..N. Each metadata group has three voters (N = 3: all; N ≥ 4: nodes 1–3 and 2–4); nodes 5..N hold data only |
| `EFS_RDMA_DEV` / `EFS_RDMA_GID_INDEX` | **server/client:** verbs device and RoCE GID selection |
| `EFS_RDMA_BUFS` | RDMA buffer pool size |
| `EFS_LOG_TS` | UTC timestamps are enabled by default; `=0` disables the timestamp wrapper |
| `EFS_DCACHE_TRACE=1` | client dcache/publish trace (the `report` / `dcache` lines) |
| `EFS_REPORT_DBG=1` | client REPORT pack + verdict detail |
| `EFS_RPC_PROF=1` | client per-RPC profiling counters |
| `EFS_STAGE_DBG=1` | client staging-table debug |
| `EFS_FLOCK_DBG=1` | client POSIX-lock debug |
| `EFS_GC_DBG=1` | **server:** log every GC frag/reap pass (otherwise only passes over 5 ms print a `gc-pass` line) |
| `EFS_FAULT_GC_DISABLE=1` | **server, test only:** no GC reaper; `EFS_FAULTS=1` builds only (`tests/faults/efsd`; was `EFS_GC_DISABLE`) |
| `EFS_RAFT_OBS=1` | **server:** `raft-obs` observability lines (incl. `pub_p50`/`pub_max`) |
| `EFS_PERF_DIR` | recorder directory for `server.sh` / `client.sh` wrappers (default under `~/orcd/scratch/efs/perf`) |
| `EFS_PERF_PATH` / `EFS_STRACE_PATH` | output paths for recorders started by the binaries themselves |
| `EFS_STRACE_EXPR` | strace syscall filter |
| `EFS_FAULT_WITHHOLD=<ino>:<ci>` | **client:** implemented only in `EFS_FAULTS=1` builds; omits that REPORT record before serialization; `/tmp/efs/fault` containing `WITHHOLD <ino>:<ci>` overrides the environment, and `OFF` disables it |
| `EFS_FAULT_COMPACT_DIE=<n>` | **test only:** `_exit(99)` after the n-th finished compaction output segment, before the manifest rename; compiled only with `EFS_FAULTS=1` and exercised by `tests/faults/test_kv_compact_crash` (K24; was `EFS_KV_COMPACT_DIE`, read in every build). A normal `efsd` or `efs-fuse` prints `<prog>: <VAR> ignored: this build has no fault hooks` at startup for each `EFS_FAULT_*` and for `EFS_KV_COMPACT_DIE` in its environment (W149) |

Every setting the binaries read, with its kind and disposition, is in the
[A6 settings inventory](../status/a6-settings-inventory-20261024.md). A
removed setting that is still set (for example `EFS_MD_RAFT_N`,
`EFS_GC_DISABLE`, `EFS_FEATURES_TTL_MS`) stops efsd and efs-fuse with a
message naming it.

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

## Server query-cache capacity

The disk query backend is a local, rebuildable cache. Configure the server process
with `EFS_QUERY_DISK_MAX_MIB` (per generation, default 16384),
`EFS_QUERY_DISK_TOTAL_MIB` (all retained generations and new-build scratch in the
private cache root, default 32768), and `EFS_QUERY_DISK_RESERVE_MIB` (filesystem
free-space guard, default 64). Settings are read at process startup. These are
logical admission limits, not exclusive disk reservations. Shared links count
conservatively per generation; pinned exports can cause refresh admission refusal.
A refusal preserves the old checkpoint and reports a refresh error. Existing
valid cache reads remain available after a total-limit reduction.
See [aggregate admission and acceptance](../status/query-cache-closure-20261010.md).

The metadata directory also keeps an optional fixed-size `QUERY-REVISIONS` sidecar
for restart catch-up. It is derived state paired with the KV manifest; missing or
invalid hints force rebuilding rather than preventing metadata recovery. Partition
change counters continue while command serving is disabled, with no accumulated
per-operation queue. Disk catalog version 5 rebuilds older local cache generations.

For large streaming exports, set server `EFS_QUERY_EXPORT_MAX_MS` to the required
finite total lifetime (milliseconds, allowed 60000–604800000; default 3600000).
For example, `86400000` allows a 24-hour active export. The idle timeout stays
60 seconds; each host retains at most one paged export snapshot, and permission
checks still run on every page. Longer total lifetimes can retain old cache files
longer, so provision the aggregate query-cache budget accordingly. Apply the
setting consistently to potential publishers before starting the export.
