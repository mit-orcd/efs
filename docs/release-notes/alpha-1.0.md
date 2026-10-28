# EFS alpha 1.0 — release notes (draft)

**DRAFT, October 24, 2026, audited through `e7e72e267` (waves 9–22)
([round 3 audit](../status/docs-audit-20261027.md); source/evidence review,
Mac live execution NOTRUN). Not a release: no candidate is
frozen, no tag exists, and the alpha gates of the
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
RDMA, background packing of small files, the `.efs` directory commands and
scratch expiry under a cluster expiry policy (off until an operator turns it
on with `efs-mgmt expiry-policy NODE:PORT cluster set on VERSION`, D43).
A cluster starts with `efsd ADDR:PORT STORAGE` on the first node and
`efsd --join [SEED] STORAGE` on the others, forms by itself at three
members and takes further nodes online. A stripe or packed segment
acknowledged with only two fragments is rebuilt when the missing node
returns (W154, K37); a member back from an unclean stop verifies its store
before it proves data durable again (K36); `fsync` with a member down
succeeds once two fragments of every stripe are proven durable and the
third is marked for rebuild (K35); a member that stops in order
acknowledges no fragment it still receives (K34). Fragment I/O is
`O_DIRECT` only and `.efs` follows the cluster commands-policy (D42).
Scratch expiry ships too (D43): files under an `efs-mgmt expiry-policy`
get a deadline, can be renewed and are removed when it passes; the
cluster expiry policy is off until an operator turns it on (L20).

## Limitations

### Protection, topology and membership

| ID | Limitation | Trigger | What the user sees | Reference |
| --- | --- | --- | --- | --- |
| L1 | 2+1 only; no two-loss protection on any cluster size | Two nodes lost or down at once | Stripes with fragments on both are unreadable; a metadata group that loses two voters stops | D36 (final contract deferred), D38 |
| L2 | Fixed metadata group count and placement: two groups of three voters; both on nodes 1–3 at N = 3, nodes 1–3 and 2–4 at N ≥ 4 | Any cluster | Nodes 5 and up hold data only; metadata work and leadership stay on nodes 1–4, and leaders are not balanced (W66) | D38 as amended Oct 22 |
| L3 | Packed small files are data and use the data ring across all N nodes | Small files after close | Packed segments land on every node; a node that hosts no group reads the pack object from a voter. The placement audit (B4) is NOTRUN | D37, D38, K28 |
| L4 | Supported range 3 to 16 nodes on distinct hosts | Fewer than 3 members, more than 16, or several members per host | Fewer than 3: the cluster never forms. A 17th joiner is refused: it logs `the cluster has the maximum of 16 members` and exits 1, and the 16 members keep working (finding F14, `tests/test_member_limit.py`). Several daemons per host are not independent failure domains. Qualification on 3, 4 and more than 4 distinct hosts is NOTRUN (A3, A4) | `EFS_MAX_NODES` 16, K28, K33 |
| L5 | Online addition only; online removal deferred | A node is retired or lost for good | It stays a member, shown `DOWN`; its stripes stay degraded and `fsync` marks new ones degraded (L27); there is no remove or replace command | A13, D40 |
| L6 | No repair beyond rebuild on return | A node does not come back, or a fragment is lost or corrupt while all nodes are up | Nothing re-protects the stripe elsewhere; a second loss makes it unreadable. Rebuild runs for stripes and packed segments acknowledged with two fragments, and for fragments a returning member's verified recovery or the leader's audit found missing or torn, when that member is up (W154, K36, K37). A packed segment that lost two fragments is reported `unrecoverable` (`Protection: LOST`) and its files read `EIO` until rewritten or removed | K23, D40, W154, K37; W74 deferred |
| L7 | No sudden power-loss qualification | Host power-off or kernel crash | Not qualified: acknowledged writes may or may not survive. The restart runs the same verified recovery as after a crash (K36); what it cannot prove is dropped and rebuilt from the other members | W71, W123 (K23); D42 (F23) |
| L27 | `fsync` with a member down leaves stripes without tolerance (decided D42) | A storage node down or unreachable | `fsync` and `fdatasync` succeed with two fragments proven durable, after marking the stripe degraded (the client log names each stripe); the stripe survives no further loss until the rebuild on return; with two of a stripe's nodes down they return `EIO` | K35 (D42), finding F22 |
| L28 | A node back from an unclean stop proves nothing until its verified recovery is done (decided D42) | A daemon crash, `SIGKILL`, forced exit (rc 3) or power loss, then restart | Until `sync-status` shows `recovery verified`, that node refuses COMMIT: `fsync` marks every stripe with a fragment there degraded and returns `EIO` for a stripe that needs it. Recovery proves the store (K36, D42), drops what it cannot prove and has it rebuilt from the other members; a write the crashed run acknowledged but had not committed is treated as lost on that node. A stop during the recovery leaves the store fenced (`server.sh stop` rc 4) until the next start verifies it. A recovery that fails leaves the node fenced; there is no repair or replacement for it (L5, L6) | K36; finding F23 |
| L29 | A clean client stop does not complete while a member is down, — **not accepted as a limitation (D46): must be fixed before alpha 1.0 (K54)** (found Oct 25) | `client.sh stop` or a clean unmount with any storage node down | The October 25 run flushed and settled its writes; this is not a general durability proof for failed stops. The mount session's revoke must seal every configured member and fails after 15 s (`revoke rc=-13 phase=storage-fence`); `client.sh stop` exits 1, the client exits, the session's leases and locks stay until the member is back | [audit finding U4](../status/docs-audit-20261025.md); `session_revoke.c`, `host_storage_fence_members` |
| L8 | Degraded state is shown to the operator only | One node down, degraded stripes or packed segments | `efs-mgmt status`: `Cluster state: DEGRADED`, `Degraded chunks: N`, `Degraded segments: M`, `Protection: DEGRADED` or `LOST`, exit status 1 with a member down. The mount and applications get no indication | W154, K37; finding F10 |

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
| L20 | Containers removed; expiry only through `efs-mgmt` | `EFS_EXPIRY=1`, `.efs` expiry options | efsd refuses `EFS_EXPIRY` and names `efs-mgmt expiry-policy NODE:PORT cluster`; the `.efs` commands reject expiry options. Expiry ships (D43), off by default; a deadline acts only while the cluster policy is on and its source folder policy enabled | D33, D37, D43, A2 |
| L21 | One export per cluster | Any | No export label, no second export | `efs-fuse`, `mkfs` |
| L22 | No authentication on the cluster ports (D45, user, Oct 28: none in any alpha release) | Any host that reaches a daemon port | Can run every `efs-mgmt` command, including mutating ones; keep the network trusted | Finding F12 |
| L23 | Same build everywhere; no upgrade in place | A peer or client of another build; a format change | The connection is refused; a format change needs a fresh format, no migration and no downgrade | AGENTS.md (no backward compatibility) |
| L24 | No effective-settings listing yet (D46, user, Oct 28: W122 is required in alpha 1.0; this row goes when it ships) | An operator wants every effective value | Only the startup log and the status commands show settings; the [A6 inventory](../status/a6-settings-inventory-20261024.md) lists every setting and its default | S4, W122 |
| L25 | Full RDMA topology qualification across independent storage hosts remains open | RDMA with 3/4/>4 distinct storage hosts | Task K's node9900 client to four co-located node9901 daemons has scoped RDMA POSIX/management PASS; Task M has single-host native RDMA evidence. Neither qualifies independent storage failure domains or the complete RDMA release matrix | W106, A2; [Task K](../status/k44-retired-replica-routing-20261026.md), [Task M](../status/gate-fstor007-20261027.md) |
| L26 | The alpha profile is the only behavior (A7, D42) | — | Fragment I/O is always `O_DIRECT`, `.efs` follows the cluster commands-policy, expiry follows the cluster expiry policy (D43); removed settings are refused at startup | Findings F7, F8 (closed by A7) |

## Open items that are not yet limitations

The requirements below remain release gates, not accepted limitations. This
summary supersedes the October 25 open-item list; the
[checklist](../status/alpha-1.0-2plus1-checklist.md) retains the detailed matrix.

- **K43, OPEN:** the four-hour churn soak failed at 7,195 s after the REPORT
  coordinator was killed; a writer received BUSY. The successful retained
  readback does not convert that failed operation to PASS
  ([Task D](../status/task-d-20261025.md)).
- **K48, OPEN correctness candidate:** after one replica halted, an ordinary
  healthy-quorum directory listing returned 95 of 96 fsynced names. No
  permanent payload loss is established; later passing gates do not resolve
  the omission ([Task N](../status/k15-k29-append-gates-20261027.md)).
- **W166:** option B's responsive-node pressure paths and K38 shared-chunk
  repairs are merged in wave 19, retaining the 30 s flush deadline. Original
  full-process SIGSTOP / production snapshot-window, RDMA and multi-host
  acceptance remain NOTRUN ([W166](../status/w166-close-eio-flush-deadline-20261026.md),
  [K38 continuation](../status/k38-task-g-repair-20261027.md)). Healthy-cluster
  EIO is not an accepted limitation.
- **W159:** progress-bounded applied waits merged in wave 9; K50's
  deadline-bounded namespace retries merged in wave 22. Broader loaded POSIX
  latency failures remain OPEN; no longer describe either fix as merely
  parked on a branch ([Task O](../status/dir-ops-busy-under-load-20261027.md)).
- **K6/W130 and W165:** apply/serve-path fail-closed halts and snapshot
  resync healing are merged (waves 15–16); K45–K47 cover compacted log
  rejections, resync election exclusion and packed-read returned-holder
  reprobes (wave 17). Scoped loopback passes do not replace remaining
  real-host acceptance or resolve K48. Follow the
  [resync runbook](../using/alpha-1.0-guide.md#a-metadata-replica-halts-w130w165).
- **K44/W167:** retired-seed management routing is fixed and merged (wave
  18), with a scoped orcd-mini RDMA addition/management sweep. The old
  workaround of targeting another member is historical; no reformat is
  needed for it ([Task K](../status/k44-retired-replica-routing-20261026.md)).
- **K15/K29:** six Task N append gates pass on single-host TCP, including
  warm/peer/cold exact verification. Multi-host/RDMA and combined-fault
  acceptance remain NOTRUN; this is not the earlier blanket "live gates
  NOTRUN" state.
- **Remaining matrix:** K11, K12, K17–K21, K22's production-scale bound,
  full 3/4/>4 distinct-host topology/addition gates, and the NOTRUN scopes
  of fixed rows still need acceptance. Task M's RDMA POSIX/GC evidence
  does not qualify every K34–K37 scenario; cold-query skips remain
  incomplete coverage. Zero FAIL with NOTRUN is INCOMPLETE, not a full gate
  PASS. L22 is decided (D45: no authentication in any alpha release); W122 is
  required in alpha 1.0 (D46), so L24 goes when it ships.
