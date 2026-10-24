# Alpha 1.0 — supported surface and effective settings (draft)

**Draft for user approval, October 24, 2026; checklist items S1 and S4 of
the [alpha 1.0 checklist](../status/alpha-1.0-2plus1-checklist.md#s-stage-2-functional-scope).
Checked against the code at `a2326a369`.** "In" marks a surface the alpha
release supports and qualifies; "out" marks one the release does not
support, with the reason. Rows marked *proposed* are this draft's
recommendation and wait for the user; the others follow a recorded
decision. A surface marked out may still exist in the binaries; using it is
outside the alpha support claim.

[Alpha guide](alpha-1.0-guide.md) · [POSIX subset](alpha-1.0-posix.md) ·
[Release notes draft](../release-notes/alpha-1.0.md) ·
[Findings](../status/s-docs-alpha-1.0-20261024.md)

The alpha profile (A2) is: Linux FUSE client, RDMA or TCP, direct fragment
I/O, `.efs` commands on, background packing on, expiry off, containers
absent. The daemon defaults are not all the alpha profile: fragment I/O is
buffered unless `--direct-io` is given, and a mount has no `.efs` unless
`EFS_COMMANDS=1` is set (findings F7 and F8).

## Binaries and scripts

| Surface | Alpha | Reason |
| --- | --- | --- |
| `efsd` | in | Storage and metadata daemon |
| `efs-fuse` | in | Linux FUSE client; the only client in alpha (decided Oct 22, W152) |
| `efs-mgmt` | in, the commands marked in below | Operator status and administration |
| `scripts/server.sh`, `scripts/client.sh` | in | Start, stop and mount wrappers with orderly-stop checks (`server_processes.py`, `client_processes.py`) |
| `efs-bench`, `blake3-bench` | out | Developer measurement tools; no user workflow depends on them |
| `tests/cluster.sh`, devops and portal deployment scripts | out | Test-cluster harnesses, not shipped product; the portal is not in this repository |
| NFS re-export (`EFS_FUSE_EXPORT=1` and a gateway) | out | Decided out of alpha (user, Oct 22; beta backlog W152) |
| macOS client | out | Decided out of alpha (user, Oct 22; W152) |
| Containers | out | Removed from devel (D37) |

## `efsd` arguments and options

| Argument or option | Alpha | Reason |
| --- | --- | --- |
| `efsd ADDR:PORT STORAGE` | in | Creates a cluster in fresh storage, or restarts the node of that store (K33) |
| `efsd --join [SEED] STORAGE` | in | Joins through SEED or local-subnet discovery, or restarts a joined store |
| `STORAGE` (1–24 roots, comma-separated) | in | Deployment input |
| `--listen ADDR:PORT` | in | A joiner's own endpoint; needed when it cannot be derived (several nodes on one host) |
| `--discovery-port N`, `--discover-on IFACE` | in | Deployment input for discovery (default UDP 17430; 0 turns it off) |
| `--quota SIZE` | in | Deployment input; default is the automatic safe quota |
| `--direct-io` | in | The alpha profile's fragment I/O (A2) |
| `--no-direct-io` | out, *proposed* | Buffered I/O is the daemon default but not the alpha profile; A7 decides whether one mode stays (finding F7) |
| `--meta-storage PATH` | in | Deployment input: Raft/KV on another root; it does not move an existing `mdraft/` |
| `--writers N` | out, *proposed* | Tuning switch (default 8); A8 has not justified it as operator configuration |
| `--no-persist` | out, *proposed* | Turns off only the joined-node table (`meta/cluster_nodes.bin`); the committed membership is persisted regardless. No alpha workflow needs it (finding F6) |
| `--perf`, `--strace` | out | Profiling; needs `perf`/`strace` and `ptrace_scope` 0; not a supported operation |
| `--help`, `--version` | in | |
| `--node-id`, `--addr`, `--port`, `--storage`, `EFS_MD_RAFT_N` | removed | Refused with an error naming the replacement (K33, W153) |

## `efs-fuse` arguments and options

| Argument or option | Alpha | Reason |
| --- | --- | --- |
| `efs-fuse NODE:PORT [NODE:PORT ...] MOUNTPOINT` | in | One address is enough; the client discovers the rest. There is one export and no export label |
| `-o OPTIONS` and other libfuse options (`-f`) | in, passed to libfuse unchanged | No EFS-specific mount option exists; only the defaults are qualified |
| `--stop MOUNTPOINT`, `--resume MOUNTPOINT` | in | Drain and stop control used by `client.sh stop` |
| `--stop MOUNTPOINT --force-discard` | in, explicit loss | Discards unresolved writes; for a client that cannot drain |
| `--help`, `--help-fuse`, `--version` | in | |
| `--perf`, `--strace` | out | Profiling only |
| `--bench`, `--bench-time`, `--bench-mib` | out | Undocumented developer benchmark modes in the user binary (finding F5) |

## `efs-mgmt` commands

| Command | Alpha | Reason |
| --- | --- | --- |
| `status` | in | Members, reachability, quota/usage, usable 2+1 capacity, cluster state, degraded chunks, abandoned uploads |
| `raft-status` | in | Per-group term, commit, applied, voters and follower bound |
| `version`, `io-stats`, `gc-status` | in | Read-only diagnostics |
| `sync-status` | in, *proposed* | Read-only durability state per root; missing from `--help` (finding F3) |
| `mkfs` / `raft-mkfs` | in | Optional: forms at once before automatic formation; afterwards a no-op |
| `commands-policy` | in | `.efs` is in the alpha profile; the durable versioned mask |
| `add-storage` | in, limited | Attaches roots to a running node; not remembered at restart and untested (finding F11) |
| `pack-compact` | in, *proposed* | Packing control: summary (`0`) or on-demand compaction of shared segments; missing from `--help` (finding F3) |
| `raft-resync` | in, *proposed* | Recovery of a follower's group from the leader snapshot; its `--help` fails (finding F2); no `make test` case |
| `add-node` | out, *proposed* | Sends a legacy JOIN to a running node; the alpha way to add a node is `efsd --join` (K33). Untested (finding F4) |
| `shrink-quota` | out, *proposed* | Reduces the running quota only; lost at restart; no migration. Use `--quota` |
| `raft-step-down` | out, *proposed* | Leader transfer used by the failover harness; missing from `--help`; no operator workflow |
| `data-commit` | out, *proposed* | Durability barrier; the orderly stop already runs it; missing from `--help` |
| `raft-change` | out | Manual voter change; K33 changes the voters itself, and a manual change leaves the default layout |
| `raft-dir`, `raft-session` and every `raft-<op>` (`raft-create`, `raft-lookup`, `raft-rename`, `raft-publish`, `raft-flock`, …) | out | Debug commands that bypass FUSE and change metadata directly |
| `expiry`, `expiry-policy`, `expiry-status`, `expiry-clock`, `expiry-enable`, `expiry-id` | out | Expiry is disabled in the alpha profile (W90; A7 decides removal) |

`efs-mgmt` has no authentication: any host that reaches a daemon port can
run every command, including the mutating ones (finding F12). Keep the
cluster ports on a trusted network.

## `.efs` commands

| Command | Alpha | Reason |
| --- | --- | --- |
| `.efs/ls`, `.efs/find`, `.efs/stats`, `.efs/help` | in | D34/W91–W92; needs `EFS_COMMANDS=1` on the mount, server policy, and Python 3 on the client ([commands](efs-commands.md)) |
| Expiry options of the commands | out | Rejected by the commands; expiry is off |
| The former `.stats`/`.find` lookup-only files | removed | Replaced by `.efs/stats` and `.efs/find` (W92); `operations.md` still described `.stats` (finding F9, corrected) |

## Environment variables

Only the rows marked in are part of the alpha deployment surface. Everything
else is an internal tunable, an A/B selector, observability or a test hook,
and waits for the A6 inventory and the A7/A8 decisions.

| Variable | Side | Alpha | Reason |
| --- | --- | --- | --- |
| `EFS_TRANSPORT=auto\|tcp\|rdma` | both | in | Transport choice; default `auto` |
| `EFS_RDMA_DEV`, `EFS_RDMA_GID_INDEX` | both | in | RDMA device and RoCE GID selection |
| `EFS_COMMANDS=1` | client | in | Turns `.efs` on for this mount (required by the alpha profile) |
| `EFS_COMMANDS_SERVER=0\|1` | server | in | Default 1; 0 stops a node serving queries; set it the same on every node |
| `EFS_EXPIRY` | server | out | Expiry is off in alpha |
| `EFS_FUSE_EXPORT` | client | out | NFS re-export is out (W152) |
| `EFS_CLIENT_META_MB`, `EFS_DCACHE_HARD_BYTES`, `EFS_DCACHE_DRAIN_BYTES`, `EFS_DCACHE_BYTES`, `EFS_CLIENT_CONNS_PER_NODE`, `EFS_RDMA_BUFS`, `EFS_NUMA_NODE` | client | out, pending A8 | Capacity and placement tunables; defaults are the qualified values |
| `EFS_QUERY_DISK_MAX_MIB`, `EFS_QUERY_DISK_TOTAL_MIB`, `EFS_QUERY_DISK_RESERVE_MIB`, `EFS_QUERY_REFRESH_MS`, `EFS_QUERY_EXPORT_MAX_MS` | server | out, pending A8 | Query-cache sizing; a large namespace may need them, so A8 must decide |
| `EFS_PACK_*`, `EFS_REBUILD_PER_PASS` | server | out | Internal tunables; D37 allows no setting that disables the packer |
| `EFS_READ_VERIFY`, `EFS_READ_PREFETCH`, `EFS_FUSE_SPLICE_READ`, `EFS_METADATA_BALANCE`, `EFS_QUERY_BACKEND`, `EFS_RAFT_APPLIED_INTERVAL_MS`, `EFS_GC_BACKLOG_IDLE_MS`, `EFS_RDMA_FIRST`, `EFS_KV_BLOOM` | both | out | A/B selectors and experiments; A7 removes or decides them |
| `EFS_FAULT_GC_DISABLE` | server | out | Test builds only (`EFS_FAULTS=1`); was `EFS_GC_DISABLE` |
| `*_DBG`, `*_TRACE`, `*_PROF`, `*_OBS`, `EFS_LOG_TS`, `EFS_PERF_*`, `EFS_STRACE_*` | both | out | Observability and profiling |
| `EFS_FAULT_*` | both | out | Test builds only; a normal build names them as ignored at startup (W149) |
| `EFS_FUSE_LOG`, `FUSE_THREAD_STACK`, `MALLOC_ARENA_MAX` | `client.sh` | in, wrapper defaults | The wrapper sets the last two when unset |

The [power-user page](power-user.md) describes most of these variables; its
descriptions are not a support statement.

## S4 — effective settings

**Status: structure only; depends on the A6 inventory**
(`docs/status/a6-settings-inventory-20261024.md`, being written in another
session) **and on a user decision whether W122 is in alpha.** Until then
an operator sees effective settings only through the channels below.

| Setting | Where the operator sees the effective value today |
| --- | --- |
| Endpoint, node id, storage roots, quota (automatic or explicit, with the safe maximum), direct I/O, writer count | `efsd` startup lines in `log/efsd.log` (`Storage quota: …`, `node N listening on …`, `direct_io=…`); roots, quota and usage also in `efs-mgmt status` |
| Membership, member count, metadata voters, formation | `efs-mgmt status` (members), `efs-mgmt raft-status` (voters per group), log line `raft-host: formed: N members, g0 voters …, g2 voters …` |
| Build identity | `--version` on each binary, `efs-mgmt version NODE:PORT` |
| Durability mode per root | `efs-mgmt sync-status NODE:PORT` |
| `.efs` policy | `efs-mgmt commands-policy NODE:PORT get`; the mount's `EFS_COMMANDS` is visible only in the client's environment |
| Transport | Daemon and client logs; not in any status command |
| Environment tunables and A/B selectors | Not shown. An unset variable gives the built-in default; a set one is visible only in the process environment. Some invalid values are logged (`query-config: …`) |

Structure for the S4 page once A6 lands: one row per inventoried setting,
with its A6 disposition (deployment input, justified tuning, observability,
removed), its default, whether its effective value is visible and where,
and the A11 configuration dimension it belongs to. If W122 is taken into
alpha, its listing replaces the last three rows of the table above; if not,
F4 lists "no effective-settings listing" as a limitation.

Removed settings (A6, `docs/status/a6-settings-inventory-20261024.md`) such as `EFS_FEATURES_TTL_MS`, `EFS_QUERY_CATALOG_MIB`, `EFS_GC_DISABLE` and `EFS_KV_COMPACT_DIE` are not settings any more: efsd and efs-fuse stop with a message naming one that is still set.
