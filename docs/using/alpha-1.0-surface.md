# Alpha 1.0 — supported surface and effective settings (draft)

**Draft for user approval, October 24, 2026, revised October 25; checklist
items S1 and S4 of the [alpha 1.0 checklist](../status/alpha-1.0-2plus1-checklist.md#s-stage-2-functional-scope).
Checked against the code and the binaries at `ec3e09306`
([audit](../status/docs-audit-20261025.md)); `tests/test_cli_docs.py`
(`make test`) keeps this page and the CLI help in step.** "In" marks a surface the alpha
release supports and qualifies; "out" marks one the release does not
support, with the reason. Rows marked *proposed* are this draft's
recommendation and wait for the user; the others follow a recorded
decision. A surface marked out may still exist in the binaries; using it is
outside the alpha support claim.

[Alpha guide](alpha-1.0-guide.md) · [POSIX subset](alpha-1.0-posix.md) ·
[Release notes draft](../release-notes/alpha-1.0.md) ·
[Findings](../status/s-docs-alpha-1.0-20261024.md)

The alpha profile (A2) is: Linux FUSE client, RDMA or TCP, direct fragment
I/O, `.efs` commands on, background packing on, expiry governed by the
cluster expiry policy (off until an operator turns it on), containers
absent. Since A7 (D42) these are the only behaviors: fragment I/O is always
`O_DIRECT`, `.efs` follows the cluster commands-policy (on by default), and
expiry is in every build and follows the cluster expiry policy (D43; no
build switch, no environment variable; findings F7 and F8 closed).

## Binaries and scripts

| Surface | Alpha | Reason |
| --- | --- | --- |
| `efsd` | in | Storage and metadata daemon |
| `efs-fuse` | in | Linux FUSE client; the only client in alpha (decided Oct 22, W152) |
| `efs-mgmt` | in, the commands marked in below | Operator status and administration |
| `scripts/server.sh`, `scripts/client.sh` | in | Start, stop and mount wrappers with orderly-stop checks (`server_processes.py`, `client_processes.py`) |
| `efs-bench`, `blake3-bench` | out | Developer measurement tools; no user workflow depends on them |
| `tests/cluster.sh`, devops and portal deployment scripts | out | Test-cluster harnesses, not shipped product; the portal is not in this repository |
| NFS re-export | out | Decided out of alpha (user, Oct 22; beta backlog W152); the switch is removed (D42) |
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
| `--direct-io`, `--no-direct-io` | removed | Fragment I/O is always `O_DIRECT`; efsd refuses both flags and a root without `O_DIRECT` (D42) |
| `--meta-storage PATH` | in | Deployment input: Raft/KV on another root; it does not move an existing `mdraft/` |
| `--writers N` | out, *proposed* | Tuning switch (default 8); A8 has not justified it as operator configuration |
| `--no-persist` | removed | Membership is always persisted; efsd refuses the flag (D42, finding F6) |
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
| `--bench`, `--bench-time`, `--bench-mib` | removed | Refused: the in-process client benchmark left the product binary; nothing used it (finding F5). Use `efs-bench` |

## `efs-mgmt` commands

| Command | Alpha | Reason |
| --- | --- | --- |
| `status` | in | Members, reachability, quota/usage, usable 2+1 capacity, cluster state, degraded chunks and packed segments, `Protection:`, abandoned uploads, build identity of every member, metadata leader placement; exits 1 with a member down |
| `raft-status` | in | Per-group term, commit, applied, voters and follower bound |
| `version`, `io-stats`, `gc-status` | in | Read-only diagnostics |
| `sync-status` | in, *proposed* | Read-only durability state per root, and the phase and result of a verified recovery after an unclean stop (K36) |
| `mkfs` / `raft-mkfs` | in | Optional: forms at once before automatic formation; afterwards a no-op |
| `commands-policy` | in | `.efs` is in the alpha profile; the durable versioned mask |
| `pack-compact` | in, *proposed* | Packing control: summary (`0`) or on-demand compaction of shared segments |
| `raft-resync` | in, *proposed* | Recovery of a follower's group from the leader snapshot; daemon test `tests/test_operator_workflows.py` (finding F20) |
| `shrink-quota` | out, *proposed* | Reduces the running quota only; lost at restart; no migration. Use `--quota` |
| `raft-step-down` | out, *proposed* | Leader transfer used by the failover harness; no operator workflow |
| `data-commit` | out, *proposed* | Durability barrier; the orderly stop already runs it |
| `raft-change` | out | Manual voter change; K33 changes the voters itself, and a manual change leaves the default layout |
| `raft-dir`, `raft-session` and every `raft-<op>`: `raft-getattr`, `raft-lookup`, `raft-lookup-path`, `raft-create`, `raft-unlink`, `raft-rmdir`, `raft-link`, `raft-setattr`, `raft-rename`, `raft-readdir`, `raft-publish`, `raft-getchunks`, `raft-append`, `raft-hold`, `raft-flock`, `raft-fcntl` | out | Debug commands that bypass FUSE and read or change metadata directly; `raft-session fence` is also the way to resume a failed mount-session revoke |
| `add-node` | removed | Refused, naming `efsd --join`: one way to add a node (K33, finding F4) |
| `add-storage` | removed | Refused: a node's storage roots are fixed when its store is created; a restart with other roots is refused (finding F11) |
| `expiry-policy` | in | D43: `expiry-policy NODE:PORT cluster get` / `cluster set on\|off EXPECTED_VERSION` turns expiry on or off for the whole cluster at runtime (versioned like `commands-policy`, off by default); `get`/`set ROOT GENERATION ...` reads and sets a folder policy (set needs uid 0). Qualified by the `d5-expiry` gate |
| `expiry`, `expiry-status`, `expiry-clock`, `expiry-enable`, `expiry-id` | in | D43: path and deadline inspection, renewal, worker status, clock health and existing-file deadlines; refused (rc -16) while the cluster expiry policy is off |

`efs-mgmt` has no authentication: any host that reaches a daemon port can
run every command, including the mutating ones (finding F12; the user's
scope decision is still open, so this is the current state, not a
decision). Keep the cluster ports on a trusted network. A command the
build does not have (a misspelt name)
answers `Unknown command: NAME` and exits 1; the removed `add-node` and
`add-storage` are refused by name with the replacement and exit 2.

## `.efs` commands

| Command | Alpha | Reason |
| --- | --- | --- |
| `.efs/ls`, `.efs/find`, `.efs/stats`, `.efs/help` | in | D34/W91–W92; present on every mount while the cluster commands-policy allows it (mask 3, the default) and needs Python 3 on the client ([commands](efs-commands.md)); `EFS_COMMANDS` is removed (D42) |
| Expiry options of the commands | out | Rejected by the commands; the `.efs` commands neither set nor show expiry. Expiry itself ships through `efs-mgmt` (D43) |
| The former `.stats`/`.find` lookup-only files | removed | Replaced by `.efs/stats` and `.efs/find` (W92); `operations.md` still described `.stats` (finding F9, corrected) |

## Environment variables

Only the rows marked in are part of the alpha deployment surface. Everything
else is an internal tunable, an A/B selector, observability or a test hook,
and waits for the A6 inventory and the A7/A8 decisions.

| Variable | Side | Alpha | Reason |
| --- | --- | --- | --- |
| `EFS_TRANSPORT=auto\|tcp\|rdma` | both | in | Transport choice; default `auto` (logs the transport used); `rdma` refuses to start without a usable device |
| `EFS_RDMA_DEV`, `EFS_RDMA_GID_INDEX` | both | in | RDMA device and RoCE GID selection |
| `EFS_COMMANDS`, `EFS_COMMANDS_SERVER`, `EFS_EXPIRY`, `EFS_FUSE_EXPORT` | both | removed | Refused at startup (D42, D43): `.efs` follows `efs-mgmt commands-policy`; expiry follows `efs-mgmt expiry-policy NODE:PORT cluster` (the refusal names it); NFS re-export is not in alpha |
| `EFS_CLIENT_META_MB`, `EFS_DCACHE_HARD_BYTES`, `EFS_DCACHE_DRAIN_BYTES`, `EFS_DCACHE_BYTES`, `EFS_CLIENT_CONNS_PER_NODE`, `EFS_RDMA_BUFS`, `EFS_NUMA_NODE` | client | out, pending A8 | Capacity and placement tunables; defaults are the qualified values |
| `EFS_QUERY_DISK_MAX_MIB`, `EFS_QUERY_DISK_TOTAL_MIB`, `EFS_QUERY_DISK_RESERVE_MIB`, `EFS_QUERY_REFRESH_MS`, `EFS_QUERY_EXPORT_MAX_MS` | server | out, pending A8 | Query-cache sizing; a large namespace may need them, so A8 must decide |
| `EFS_PACK_*`, `EFS_REBUILD_PER_PASS` | server | out | Internal tunables; D37 allows no setting that disables the packer |
| `EFS_READ_PREFETCH`, `EFS_FUSE_SPLICE_READ`, `EFS_METADATA_BALANCE`, `EFS_QUERY_BACKEND`, `EFS_KV_BLOOM`, `EFS_RAFT_BATCH_NOOP`, `EFS_RAFT_BATCH_OVERLAP`, `EFS_UNMOUNT_REVOKE`, `EFS_RDMA_FIRST` | both | removed | Refused at startup (D42); one built-in behavior each. `EFS_RDMA_FIRST` is now `EFS_RDMA_TRACE=1` (diagnostics only) |
| `EFS_READ_VERIFY` | client | removed | The client re-hash is built in (D42 GET-path audit); a set value stops efs-fuse |
| `EFS_RAFT_APPLIED_INTERVAL_MS`, `EFS_GC_BACKLOG_IDLE_MS` | server | out, pending A8 | Numeric tunables |
| `EFS_FAULT_GC_DISABLE` | server | out | Test builds only (`EFS_FAULTS=1`); was `EFS_GC_DISABLE` |
| `*_DBG`, `*_TRACE`, `*_PROF`, `*_OBS`, `EFS_LOG_TS`, `EFS_PERF_*`, `EFS_STRACE_*` | both | out | Observability and profiling |
| `EFS_FAULT_*` | both | out | Test builds only; a normal build names them as ignored at startup (W149) |
| `EFS_FUSE_LOG`, `FUSE_THREAD_STACK`, `MALLOC_ARENA_MAX` | `client.sh` | in, wrapper defaults | The wrapper sets the last two when unset |

The [power-user page](power-user.md) describes most of these variables; its
descriptions are not a support statement.

## S4 — effective settings

**Status: structure only. The A6 inventory exists**
([settings inventory](../status/a6-settings-inventory-20261024.md): every
setting the binaries read, its kind, default and disposition) **and the
A7 removals are done; the user's decision whether W122 (an
effective-settings listing) is in alpha is still open.** Until then an
operator sees effective settings only through the channels below; no
command lists them.

| Setting | Where the operator sees the effective value today |
| --- | --- |
| Endpoint, node id, storage roots, quota (automatic or explicit, with the safe maximum), direct I/O, writer count | `efsd` startup lines in `log/efsd.log` (`Storage quota: …`, `node N listening on …`, `direct_io=…`); roots, quota and usage also in `efs-mgmt status` |
| Membership, member count, metadata voters, formation | `efs-mgmt status` (members), `efs-mgmt raft-status` (voters per group), log line `raft-host: formed: N members, g0 voters …, g2 voters …` |
| Build identity | `--version` on each binary, `efs-mgmt version NODE:PORT` |
| Durability window per root | `efs-mgmt sync-status NODE:PORT` (`window 200 ms / 256 MiB`, unsynced bytes, barriers) |
| `.efs` policy | `efs-mgmt commands-policy NODE:PORT get` (the only switch; `EFS_COMMANDS` is removed) |
| Recovery state after an unclean stop | `efs-mgmt sync-status NODE:PORT` (`recovery running\|verified\|FAILED`) |
| Transport | Daemon and client logs; not in any status command |
| Environment tunables and A/B selectors | Not shown. An unset variable gives the built-in default; a set one is visible only in the process environment. Some invalid values are logged (`query-config: …`) |

Structure for the S4 page once W122 is decided: one row per inventoried
setting (the A6 inventory already has the rows), with its disposition
(deployment input, justified tuning, observability, removed), its default,
whether its effective value is visible and where, and the A11 configuration
dimension it belongs to. If W122 is taken into alpha, its listing replaces
the last three rows of the table above; if not, F4 keeps "no
effective-settings listing" as limitation L24.

Removed settings (A6, `docs/status/a6-settings-inventory-20261024.md`) such as `EFS_FEATURES_TTL_MS`, `EFS_QUERY_CATALOG_MIB`, `EFS_GC_DISABLE` and `EFS_KV_COMPACT_DIE` are not settings any more: efsd and efs-fuse stop with a message naming one that is still set.
