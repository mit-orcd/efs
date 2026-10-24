# EFS alpha 1.0 — release notes (draft)

**DRAFT, October 24, 2026, written against `a2326a369`. Not a release:
no candidate is frozen, no tag exists, and the alpha gates of the
[checklist](../status/alpha-1.0-2plus1-checklist.md) are open.** Checklist
item F4. Every limitation below names its trigger, what the user sees and
the decision or work item behind it. Rows marked *proposed* are not yet
decided by the user.

[Alpha guide](../using/alpha-1.0-guide.md) ·
[Supported surface](../using/alpha-1.0-surface.md) ·
[POSIX subset](../using/alpha-1.0-posix.md) ·
[Decision register](../status/decisions.md)

## What alpha 1.0 is

A distributed file system with 2+1 erasure-coded data on 3 to 16 storage
nodes, two three-voter Raft metadata groups, a Linux FUSE client over TCP or
RDMA, background packing of small files and the `.efs` directory commands.
A cluster starts with `efsd ADDR:PORT STORAGE` on the first node and
`efsd --join [SEED] STORAGE` on the others, forms by itself at three
members and takes further nodes online. A stripe acknowledged with only two
fragments is rebuilt when the missing node returns.

## Limitations

### Protection, topology and membership

| ID | Limitation | Trigger | What the user sees | Reference |
| --- | --- | --- | --- | --- |
| L1 | 2+1 only; no two-loss protection on any cluster size | Two nodes lost or down at once | Stripes with fragments on both are unreadable; a metadata group that loses two voters stops | D36 (final contract deferred), D38 |
| L2 | Fixed metadata group count and placement: two groups of three voters; both on nodes 1–3 at N = 3, nodes 1–3 and 2–4 at N ≥ 4 | Any cluster | Nodes 5 and up hold data only; metadata work and leadership stay on nodes 1–4, and leaders are not balanced (W66) | D38 as amended Oct 22 |
| L3 | Packed small files are data and use the data ring across all N nodes | Small files after close | Packed segments land on every node; a node that hosts no group reads the pack object from a voter. The placement audit (B4) is NOTRUN | D37, D38, K28 |
| L4 | Supported range 3 to 16 nodes on distinct hosts | Fewer than 3 members, more than 16, or several members per host | Fewer than 3: the cluster never forms. A 17th joiner is refused: it logs `the cluster has the maximum of 16 members` and exits 1, and the 16 members keep working (finding F14, `tests/test_member_limit.py`). Several daemons per host are not independent failure domains. Qualification on 3, 4 and more than 4 distinct hosts is NOTRUN (A3, A4) | `EFS_MAX_NODES` 16, K28, K33 |
| L5 | Online addition only; online removal deferred | A node is retired or lost for good | It stays a member, shown `DOWN`; its stripes stay degraded and `fsync` marks new ones degraded (L27); there is no remove or replace command | A13, D40 |
| L6 | No repair beyond rebuild on return | A node does not come back, or a fragment is lost or corrupt while all nodes are up | Nothing re-protects the stripe elsewhere; a second loss makes it unreadable. Rebuild runs only for stripes acknowledged with two fragments, when the missing node returns | K23, D40, W154; W74 deferred |
| L7 | No sudden power-loss qualification; crash recovery deferred | Host power-off or kernel crash | Not qualified: acknowledged writes may or may not survive | W71, W123 (K23) |
| L27 | `fsync` with a member down leaves stripes without tolerance, *proposed* | A storage node down or unreachable | `fsync` and `fdatasync` succeed with two fragments proven durable, after marking the stripe degraded; the stripe survives no further loss until the rebuild on return; with two of a stripe's nodes down they return `EIO` | K35 (D42), finding F22 |
| L28 | An unclean stop leaves a node's fragments unproven until a fresh format, *proposed* | A daemon crash, `SIGKILL`, forced exit (rc 3) or power loss, then restart | That node refuses COMMIT ("recovery uncertain"), so `fsync` marks every stripe with a fragment there degraded and returns `EIO` for a stripe that needs it; verified recovery is deferred (W71, W123, K36) and the node cannot be removed or replaced (L5) | `store.c` put sync; finding F23 |
| L8 | Degraded state is shown to the operator only | One node down, degraded stripes | `efs-mgmt status`: `Cluster state: DEGRADED`, `Degraded chunks: N`. The mount and applications get no indication | W154; finding F10 |

### Client and POSIX

| ID | Limitation | Trigger | What the user sees | Reference |
| --- | --- | --- | --- | --- |
| L9 | Linux FUSE client only | NFS re-export, macOS | Not supported; the `EFS_FUSE_EXPORT` switch is removed (D42) | W152 (user, Oct 22) |
| L10 | Read after fork (K10/W121) | A process forks while another of its threads has a read in flight on EFS, on a kernel without the FUSE direct-I/O page-pinning fix (upstream `738adade96b2`; the diagnosis host ran RHEL 9 `5.14.0-687.39.1.el9_8`) | The read succeeds but the caller's buffer keeps its previous bytes; a reread is correct. No EFS workaround in alpha; `posix_spawn` instead of `fork` avoids it | D40, [diagnosis](../status/orcd-w121-fork-read-20261016.md) |
| L11 | Shared mmap not supported | `mmap(MAP_SHARED)` of an EFS file | The mapping fails; `MAP_PRIVATE` works | W104 |
| L12 | `renameat2` flags not supported, *proposed* | `RENAME_EXCHANGE`, `RENAME_WHITEOUT`, `RENAME_NOREPLACE` onto a free name | `EOPNOTSUPP`, nothing renamed (`RENAME_NOREPLACE` onto an existing name gets `EEXIST`) | [POSIX P3](../using/alpha-1.0-posix.md#known-deviations) |
| L13 | `fallocate` limited, *proposed* | Any mode but 0 or `KEEP_SIZE` inside the file | `EOPNOTSUPP`; mode 0 extends with a hole and reserves no space | POSIX P4 |
| L14 | Extended attributes in `user.` only, *proposed* | ACLs, SELinux labels, `trusted.*` | `EOPNOTSUPP` | POSIX P5 |
| L15 | Special files, *proposed* | `mknod` of a FIFO, socket or device | `EPERM` | POSIX P6 |
| L16 | Unpublished writes die with the client | The client process dies before its writes are closed, synced or drained | Those writes are lost; an orderly `client.sh stop` drains or refuses | D28 |
| L17 | Bounded namespace operations | An operation with more than 144 metadata participants | Refused at once, atomically | D32 |
| L18 | `df` and `du` are logical | Any | `df` shows 2+1 logical capacity of the members up, lagging unlink cleanup; `du` counts server-side chunks | D17, POSIX P8–P9 |
| L19 | Concurrent `O_APPEND` record loss closed without resolution | Concurrent buffered appends under load | Never reproduced, no known symptom; reopens if seen | D41 (K9/W87) |

### Product scope and operations

| ID | Limitation | Trigger | What the user sees | Reference |
| --- | --- | --- | --- | --- |
| L20 | Expiry disabled, containers removed | `EFS_EXPIRY=1`, `efs-mgmt expiry*`, `.efs` expiry options | Outside the alpha surface; the commands reject expiry options | D33/W90, D37, A2 |
| L21 | One export per cluster | Any | No export label, no second export | `efs-fuse`, `mkfs` |
| L22 | No authentication on the cluster ports, *proposed* | Any host that reaches a daemon port | Can run every `efs-mgmt` command, including mutating ones; keep the network trusted | Finding F12 |
| L23 | Same build everywhere; no upgrade in place | A peer or client of another build; a format change | The connection is refused; a format change needs a fresh format, no migration and no downgrade | AGENTS.md (no backward compatibility) |
| L24 | No effective-settings listing, unless W122 enters alpha | An operator wants every effective value | Only the startup log and the status commands show settings | S4, W122 |
| L25 | Cross-host RDMA not qualified | RDMA between hosts | Recorded RDMA runs use one host's devices or software RXE | W106, A2 |
| L26 | The alpha profile is the only behavior (A7, D42) | — | Fragment I/O is always `O_DIRECT`, `.efs` follows the cluster commands-policy, expiry is compiled out; removed settings are refused at startup | Findings F7, F8 (closed by A7) |

## Open items that are not yet limitations

These K rows are open at `a2326a369`. Each must close (fixed or verified) or
be decided and added above before the release: K6 (fail-closed apply,
multi-host), K11 (W55 span without fragments, Linux FUSE), K12 (W58
`O_EXCL`), K15 (W52/W64 append failover; live gates NOTRUN), K17 (D27 STALE
replay), K18 (W54/W38 fold/GC, multi-host RDMA), K19 (W61–W63 GC drain),
K20 (W42 capacity reporting), K21 (W119/W65 RDMA failures and clean stop),
K22 (production-scale follower bound NOTRUN), K33 (live multi-host, cross-subnet,
RDMA and orcd-mini addition gates NOTRUN), and the NOTRUN cluster gates of
the FIXED rows K1, K2, K4, K5, K8, K13, K14, K16, K32. W159 (POSIX `rmdir`
`EBUSY` and timeouts on a one-host fixture) is open, cause not established.
Proposed K34 (found Oct 24, [finding F24](../status/s-docs-alpha-1.0-20261024.md)):
a chunk acknowledged during a member's orderly stop can hold its only
acknowledged fragments on that member and one other, so the file reads
`EIO` until the member returns.
