# Product surface — superseded pre-round-4 inventory

Historical wording retained for evidence; it is not current guidance. The
[active inventory](../backlog/product-gaps.md) corrects overbroad absence claims
and distinguishes the Oct 5 gateway symptom from an unverified mechanism.

## 2. Missing product surface

Real gaps, but they contradict nothing — the spec simply does not claim them
yet. Roughly in the order a user would notice.

| Capability | Status | Where it stands |
| --- | --- | --- |
| **Authentication** | absent | Plain TCP/RDMA. No TLS, shared secret, Kerberos, or capability tokens. `EFS_MSG_HELLO` gates *cluster join* on build ID; it authenticates nothing. Any host that can reach port 19810 is a peer. |
| **Authorization** | POSIX only | uid/gid/mode checked in the FUSE daemon (`check_access`). No ACLs. [§2](../how-it-works/architecture.md) is explicit that clients are assumed non-Byzantine and that hostile clients would make capability-based data authorization mandatory — so this is a scope boundary, not an oversight. |
| **Cluster identity (name / UUID)** | absent | `raft-mkfs` writes no cluster UUID or name; `cluster_nodes.bin` records membership only. Two clusters on one network are indistinguishable: nothing keeps a peer or client of cluster A from talking to cluster B (HELLO gates on build ID, not identity), and `efs-mgmt status` shows no cluster name. A mkfs-minted UUID carried in HELLO + a name in status output is the shape; no wire field or metadata key exists for either. |
| **fsck / offline consistency check** | absent | Nothing validates dangling dentries, orphan inodes, or chunk maps pointing at absent fragments. The simulator checks invariants on simulated state; there is no tool for a real cluster. Listed in [roadmap "tests still to write"](../backlog/ideas.md) as `fsck --verify-only`, unbuilt. |
| **Extended attributes** | `user.*` only | One blob per inode (`EFS_KV_KIND_XATTR`), raft command `EFS_MD_CMD_XATTR`. set/get/list/remove for names starting with `user.`. `security.*` and `system.*` return EOPNOTSUPP and are not stored. No ACLs. `opt_xattr` passes (`results/posix/20260928-043918`). |
| **User-visible snapshots** | absent | No snapshot/clone FUSE surface. Do not confuse with Raft InstallSnapshot or `kv_snap.c`, which are internal replication machinery. |
| **Node removal (data plane)** | absent | `add-node`/`JOIN`/`NODE_LEFT` update the membership list in `cluster_nodes.bin`. No fragment evacuation, no rebalance onto a new node. `shrink-quota` refuses when `used > new_quota` rather than migrating. |
| **Multi-export** | partial | Server arrays hold `EFS_MAX_EXPORTS` (16) and `raft-mkfs` creates one, but list/create/destroy are retired opcodes and the client selects one export name at mount. |
| **Quota** | node-level only | Per-node byte quota enforced on new fragment creation (`EFS_ERR_QUOTA` → `ENOSPC`). No per-user, per-group, or per-directory quota, and no inode quota. |
| **Quota growth (`grow-quota`)** | absent | The opposite of `shrink-quota`. A node's quota is set once at startup (`efsd --quota`) and `shrink-quota` only subtracts from it (`src/server/handler.c`, refused when `used > new_quota`); nothing raises it at runtime. After a shrink — or after adding capacity with `add-storage` — the node stays capped until it is restarted with a larger `--quota`. Trivial compared to shrink (no migration, just raise the ceiling in `g_server->quota` and the local node record), but the wire op and the `efs-mgmt grow-quota` command do not exist. |
| **Metrics** | human-readable only | `efs-mgmt status` / `raft-status` text, plus per-directory `.stats` through FUSE. No Prometheus endpoint, no structured stats, no time series. |
| **Logging** | unstructured | `fprintf(stderr, ...)` throughout, no levels, no rotation, no request IDs. (`raft_log_rotate_locked` is WAL compaction, not application logging.) Debug output is env-gated per subsystem, which is not the same as a log level. |
| **Non-FUSE access** | absent | FUSE is the only client. No NFS or SMB re-export, no library/DAOS-style direct API. Note [§9](../how-it-works/architecture.md) already bounds intra-mount scaling because upstream Linux serializes extending direct writes, `IOCB_APPEND`, and `O_CREAT` per mount — so a second access path is a performance question, not only a compatibility one. |
| **Stable NFS file handles across efs-fuse restarts** | absent | When efs-fuse is used as an NFS re-export (`EFS_FUSE_EXPORT=1` on a gateway), file handles are built by the kernel's generic fuse export from libfuse's per-process nodeid→(parent,name) table (`src/client/efs_fuse.c`). An efs-fuse restart empties that table, so every previously issued NFS file handle becomes unresolvable and clients must remount or re-resolve by path (observed Oct 5 2026: macOS client kept a mount working for STATFS/READDIR/CREATE but rename of a cached entry failed EIO until the path was re-walked). efs inodes/generations are cluster-persistent, so handles could instead encode (export id, efs inode, generation) and survive a gateway restart — that needs efs-fuse's own export logic instead of the libfuse default. |
| **ESTALE for unresolvable by-handle lookups** | absent | When a by-handle lookup fails after an efs-fuse restart, the NFS client sees EIO, not ESTALE — same unmapped-errno class as the FUSE_MKNOD case (`ll_mknod`, dca87c71). ESTALE would let clients recover by path re-resolution instead of hard-failing. |
| **Rolling upgrade** | works, narrowly | HELLO rejects a build-ID/version mismatch, so same-build restarts roll one at a time. There is no wire version negotiation, so an actual version *change* requires stopping every server together. |

---
