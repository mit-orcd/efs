# What is missing to be a usable product

[Architecture](../how-it-works/architecture.md) · [What to work on](../status/README.md) ·
[Parked ideas](ideas.md) · [Testing](../how-it-works/testing.md)

This page answers one question: **between here and a filesystem someone could
actually run, what does not exist?** It is a capability inventory, audited
against the source on Sep 18 2026 and re-checked Oct 1 2026 — not against
comments or spec prose. A
function that exists but is only reachable from tests counts as absent.

It is deliberately separate from [the status page §1a](../status/README.md), which is
the *near-term work queue* of active findings and their remaining gates.
Everything here is larger than a queue item and most of it needs a design
decision first. Nothing on this page is scheduled. Do not start an item here
without asking.

**Oct 7 review note:** the original Sep 18/Oct 1 inventory remains a dated
baseline, not a complete current source audit. Fault-harness availability and core durability/session/protection/integrity
gaps below have been checked against public paths; remaining surface claims
need re-audit against the
current build and public paths. Follow the [active queue](../status/README.md)
and [current handoff](../status/in-flight.md) for current implementation/gates.

---

## 1. Gaps that contradict a guarantee the spec already makes

These are the serious ones. In each case [architecture.md](../how-it-works/architecture.md)
states a property, and the code does not implement the mechanism that property
depends on. Until these close, the guarantee is a claim, not a behaviour.

### 1.1 There is no repair. Nothing regenerates a lost fragment.

[§2](../how-it-works/architecture.md) promises the cluster survives **f simultaneous permanent
node losses without data loss**, and assumes "a node that loses its local
storage rejoins empty and rebuilds from peers".
[protocols/data.md](../how-it-works/protocols/data.md) goes further: a degraded chunk
generation **consumes protection budget until repair completes**, and a
returning node "restores capacity, not the fragments it never held".

No production owner that scans under-protected live chunks and reconstructs
missing fragments was found. The GC reaper deletes obsolete fragments;
metadata transaction/snapshot recovery and reading from two survivors do not
restore the missing fragment. This accepted feature gap is indexed as
[W74](work-items.md#w74), with permanent-loss and protection-restoration gates.

The consequence is not "slow recovery", it is silent loss of the headline
guarantee. At f=1: node A dies, its fragments are gone; every chunk it held is
now 2-of-3 and reads still work, so the cluster looks healthy; A is replaced
with empty storage and still looks healthy; the next single failure takes those
chunks below k=2 and the data is gone. The system reports no degradation
between the first failure and the loss, because nothing is tracking protection
debt either.

Needs: a repair owner per chunk (the spec says deterministic, rate-limited,
priority below foreground I/O), a way to enumerate under-protected chunks
without a full-namespace walk, and a committed control-plane "unavailable"
state so `u` is a decision and not a client timeout.

### 1.2 Protection debt is not tracked, so degraded state is invisible

Related but separately absent: no `u` counter, no per-generation degraded flag,
no operator-visible "this cluster is one failure from loss". `efs-mgmt status`
reports node liveness and quota; the heal field is a stub that always returns
zeros (`EFS_MSG_HEAL_STATUS` in `src/server/handler.c`). A degraded publish
path is specified in [§2](../how-it-works/architecture.md) (publish with ≥ k+(f−u) fragments
when u domains are already unavailable) and is not implemented as that committed-debt protocol. The legacy PUT path
can return OK with two fragment ACKs after one quota failure; this concern is
tracked in [W42](work-items.md#w42--df--efs-mgmt-status-report-the-3-node-capacity-model-on-any-node-count-queue-row-2a).
Staged typed publication validates three distinct nodes but does not establish
durable physical ACKs or repair. Follow [W71](work-items.md#w71) and
[W74](work-items.md#w74); do not claim a full-ACK production guarantee here.

### 1.3 The client does not run the session protocol, so fencing is not real

Invariant **I23** and [protocols/sessions.md](../how-it-works/protocols/sessions.md)
define a session with an epoch and a revocation barrier, and make it the
fencing token: a partitioned-but-alive client is fenced at epoch+1 and can no
longer make an authoritative mutation. The server side of this exists
(`host_sess_gate`, the session applies in `raft_host.c`). The FUSE client does
not participate — when a request carries no session suffix the server
substitutes a stand-in UUID.

Production HOLD/FLOCK still omit that suffix. The mount's operation-ID UUID
at locally fixed epoch 1 provides retry identity, not a committed session
lifecycle. Append's owner suffix also does not implement that lifecycle.
Management can create/register/establish/fence sessions, and staged D25
publication admission enforces local session authority. Those primitives are
implemented; mount-wide establishment, touched-shard registration and all
public dependent paths remain [W72](work-items.md#w72). Avoid describing every
session mechanism as absent or treating the staged endpoint as full activation.

### 1.4 Fragment integrity is verified inconsistently

Invariant **I25** specifies immutable identity plus payload checksums and
repair of unavailable fragments. Current PUT hashes are payload-only and the
server stores the caller's digest. GET rejects a mismatch against a stored
checksum, but missing checksum evidence causes a fresh payload hash to be
returned rather than rejection. The parallel preferred-pair receive path
ignores returned digests; EFS_READ_VERIFY only affects the single-fragment
helper, not that common path. These trust/coverage gaps are
[W75](work-items.md#w75). Automatic repair remains [W74](work-items.md#w74).

### 1.5 Two clients writing disjoint ranges of one chunk — CLOSED

**I12** CAS is live (Sep 18 2026, leftover-1 19810). `efs_chunk_rec` carries
`base_gen` + `chunk_generation`; the leader CASes the writer's base; apply
STALE is audible and the client refetch+overlay+PUTs. Gate:
`results/stress/20260918-n1-w1/` lost=0; `peer_shared_pwrite` 5/5 concurrent.
Do not reopen with a distributed chunk lock. Details in [project-history.md](../archive/project-history.md) "START-HERE closed items", **W1**.

### 1.6 `write()` is specified as durable-and-visible; the client buffers — CLOSED

**Done Sep 18 2026**, option (i): the spec moved. [§3](../how-it-works/architecture.md) now
says a returned `write()` is client-buffered; durable + cross-client
visible at `fsync` / last `close` / `O_SYNC`. Measured
`results/stress/20260918-w2/`: peer 0/10, kill -9 loses 64 MiB.
`O_SYNC` is specified; explicit publication is unwired and actual kernel
sequencing needs the [W73](work-items.md#w73) trace gate. Independently,
[W71](work-items.md#w71) records that production PUT lacks the target
persistence barrier required by a successful durability operation. [project-history.md](../archive/project-history.md) "START-HERE closed items", **W2**.

---

## 1b. What an HPC site will ask for and not find

None of these contradicts the spec; each is a capability every deployed
parallel filesystem has and efs does not.

| ask | state | note |
| --- | --- | --- |
| IOR / mdtest / IO-500 numbers | **9×4 debug, every phase** | `results/io500/20261001-074905-rdma/` (fresh table, Oct 1): easy-write 5.17 GiB/s, hard-write 0.52, mdtest-easy-write 6.2 kIOPS, easy-stat 24.4 kIOPS, hard-read 0.82 with one read error (W38, open). Sep 30 `20260930-183504-rdma/`: 0 read errors, cold hardscan clean. No stonewall-compliant run yet (debug = 1 s stonewall, same-mount reads). |
| N-1 shared-file writes that are correct | **gated** | §1.5; `peer_shared_pwrite` concurrent 5/5. |
| per-file / per-directory layout (`lfs setstripe`-style chunk size, EC profile) | absent | export-wide only; the declared 32× small-write amplification has no opt-out. |
| a client other than FUSE (kernel module, user-space library, MPI-IO ADIO driver) | absent | FUSE-only. libfuse 3.10.2: ≤128 KiB per request, no `FOPEN_PARALLEL_DIRECT_WRITES`; Linux serializes extending direct writes and `O_CREAT` per inode/dir per mount, so many ranks on one node serialize above efs. Already an open item in [§9](../how-it-works/architecture.md). |
| the data path on the fast interconnect | **RDMA** (since Sep 28) | `EFS_TRANSPORT=rdma` on the test cluster; zero-copy sends (W39); zero-copy receive is still a design ask. |
| a hardware-relative throughput statement | 1 client: 1.3–1.5 GB/s write (~8–9 % of 16.7 GB/s), 3.6 GB/s cold read, 6.5 GB/s with four readers. 9 clients: 2.5–2.8 GB/s aggregate write (~6 % of 44 GB/s). | derivation in [performance.md](../how-it-works/performance.md#baselines-and-ceilings-current). |
| MPI-IO hints, collective-buffering guidance, Darshan/instrumentation hooks | absent | — |
| burst-buffer / tiering / HSM | absent, not designed | flash-only by decision (§1); no policy layer exists either way. |

---

## 2. Missing product surface

Product scope and acceptance limits, source-reviewed Oct 7. These rows do not
add approved features or certify a deployment. Previous overbroad wording and
the dated gateway symptom are [preserved](../archive/product-surface-before-round4-20261007.md).

| Capability | Status | Where it stands |
| --- | --- | --- |
| **Authentication** | absent | Plain TCP/RDMA. No TLS, shared secret, Kerberos, or capability tokens. `EFS_MSG_HELLO` gates *cluster join* on build ID; it authenticates nothing. Network reachability is not authentication; the configured listen port is not necessarily 19810. |
| **Authorization** | POSIX only | uid/gid/mode checked in the FUSE daemon (`check_access`). No ACLs. [§2](../how-it-works/architecture.md) is explicit that clients are assumed non-Byzantine and that hostile clients would make capability-based data authorization mandatory — so this is a scope boundary, not an oversight. |
| **Cluster identity (name / UUID)** | absent | HELLO checks build/version, not a minted cluster UUID or human name. Membership is persisted and mkfs creates a placement salt, but neither is authenticated cluster isolation. Do not confuse the salt with a checked network identity. |
| **fsck / offline consistency check** | incomplete | Targeted metadata probes, invariant tests and live reclamation fixtures exist, but there is no complete operator checker for dangling dentries, orphan inodes and missing chunk fragments on a real store. The proposed `fsck --verify-only` remains unbuilt. |
| **Extended attributes** | `user.*` only | One blob per inode (`EFS_KV_KIND_XATTR`), raft command `EFS_MD_CMD_XATTR`. set/get/list/remove for names starting with `user.`. `security.*` and `system.*` return EOPNOTSUPP and are not stored. No ACLs. `opt_xattr` passes (`results/posix/20260928-043918`). |
| **User-visible snapshots** | absent | No snapshot/clone FUSE surface. Do not confuse with Raft InstallSnapshot or `kv_snap.c`, which are internal replication machinery. |
| **Node removal (data plane)** | absent | `add-node`/`JOIN`/`NODE_LEFT` update the membership list in `cluster_nodes.bin`. No fragment evacuation, no rebalance onto a new node. `shrink-quota` refuses when `used > new_quota` rather than migrating. |
| **Multi-export** | single export by design | Legacy arrays and mount-name syntax do not establish multiple exports. `raft-mkfs` initializes one root/salt; its CLI ignores an extra name ([W79](work-items.md#w79)). The FUSE bootstrap uses a local compatibility name token. Multi-export creation/list/destruction is not a supported product path. |
| **Quota** | node-level only | Per-node byte quota enforced on new fragment creation (`EFS_ERR_QUOTA` → `ENOSPC`). No per-user, per-group, or per-directory quota, and no inode quota. |
| **Quota growth (`grow-quota`)** | absent | The opposite of `shrink-quota`. A node's quota is set once at startup (`efsd --quota`) and `shrink-quota` only subtracts from it (`src/server/handler.c`, refused when `used > new_quota`); nothing raises it at runtime. After a shrink — or after adding capacity with `add-storage` — the node stays capped until it is restarted with a larger `--quota`. Trivial compared to shrink (no migration, just raise the ceiling in `g_server->quota` and the local node record), but the wire op and the `efs-mgmt grow-quota` command do not exist. |
| **Metrics** | CLI / typed RPC counters | `status`, `raft-status`, `io-stats`, GC diagnostics and `.stats` provide targeted counters/queries; wire replies are structured, while operator output is mainly text. The old `efs-query` returns placeholder zeros ([W80](work-items.md#w80)); no integrated Prometheus endpoint or persistent time-series service was found. |
| **Logging** | text, subsystem debug switches | UTC timestamps are enabled by default (`EFS_LOG_TS=0` disables them). Some paths log inode/opid context, but there is no uniform request-correlation schema, global level policy or integrated application log rotation. Raft WAL compaction is separate from application logging. |
| **Non-FUSE access** | limited | FUSE is the filesystem mount path; management and benchmark clients also use userspace RPC paths. External NFS re-export is used with `EFS_FUSE_EXPORT=1`; a packaged general application API, native NFS/SMB server and MPI-IO client are not established. See the gateway lifetime limitation below. |
| **Stable NFS file handles across efs-fuse restarts** | acceptance missing | Oct 5 recorded a macOS NFS client keeping STATFS/READDIR/CREATE usable but cached-entry rename failing EIO until path re-resolution. Preserve that symptom; it does not prove every handle fails or identify the mechanism. The source enables `FUSE_CAP_EXPORT_SUPPORT`, returns EFS inode IDs through the low-level API and fixes entry generation at 1. Its comment attributes restart failures to a libfuse node table, but this review did not establish that explanation. Trace kernel export/decode, inode generation and gateway restart before choosing a fix; no current restart gate is claimed. |
| **ESTALE for unresolvable by-handle lookups** | unresolved gateway symptom | The Oct 5 cached-entry rename produced EIO. The by-handle failure stage and errno mapping need tracing; it is not established that this is the same cause as the earlier MKNOD mapping bug. Gate missing/stale handles and recovery by path, alongside gateway restart. |
| **Rolling upgrade** | same-build restart only | HELLO rejects build-ID/version mismatch. Same-build restart is distinct from a mixed-version upgrade; no wire-version negotiation was found. Membership quorum and public-write recovery gates still apply, so this row is not blanket restart acceptance. |

---

## 3. Verification gaps

Fault-injection infrastructure exists: [the fault suite](../../tests/faults/README.md)
provides simulator and isolated real-process/FUSE integration layers, including
server/client crashes and corruption cases. Its documentation distinguishes
`GAP` and `not_run` from passes; it is not part of the ordinary `make test`
green gate. [WITHHOLD unit coverage](../../tests/test_wb_fault.py) checks outgoing
publication omission, snapshot retention and fault-hook exclusion from
production builds. A [live GC reclamation fixture](../../tests/live/gc_reclamation.py)
is also present in the reviewed working tree.

These files establish harness availability, not execution or acceptance of a
current deployment. The remaining fault/recovery, protection/repair, fencing,
GC and deadline gates are tracked in the [queue](../status/README.md) and
[handoff](../status/in-flight.md). Inspect named raw results and build identity
before claiming a scenario passes. This review did not run those fixtures.

---

## 4. Performance distance

Not a capability gap, but it belongs in "usable product". Current references
(Oct 1 2026, RDMA, flush inside the clock; [performance.md](../how-it-works/performance.md#baselines-and-ceilings-current) has the table):
one client writes 8 GiB `dd bs=1M conv=fsync` at **1.3–1.5 GB/s** and reads
a cold 16 GiB file at **3.6 GB/s** (6.5 GB/s with four readers); nine
clients write **2.5–2.8 GB/s** aggregate. The binding ceilings are the
client's 200 Gb/s link (≈16.7 GB/s logical write after 2+1 EC) and the four
hosts' NVMe (≈44 GB/s logical) — so writes sit at roughly **6–9 %** of the
hardware and single-client reads at ~20 % of the link. [§1](../how-it-works/architecture.md)
is unambiguous that a benchmark stopping at a software serialization point
is by definition an EFS bug; the current levers are listed in [the status page](../status/README.md)
(read-side zero-copy receive, the per-client `report_mu`, IOR-hard's
shared-chunk publish rate).
