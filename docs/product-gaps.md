# What is missing to be a usable product

[Architecture](architecture.md) · [What to work on](arch/START-HERE.md) ·
[Roadmap](scaling-roadmap.md) · [Testing](testing.md)

This page answers one question: **between here and a filesystem someone could
actually run, what does not exist?** It is a capability inventory, audited
against the source on Sep 18 2026 and re-checked Oct 1 2026 — not against
comments or spec prose. A
function that exists but is only reachable from tests counts as absent.

It is deliberately separate from [START-HERE §1a](arch/START-HERE.md), which is
the *near-term work queue* (numbered W items, most of them closed).
Everything here is larger than a queue item and most of it needs a design
decision first. Nothing on this page is scheduled. Do not start an item here
without asking.

Re-audit before trusting this page: it will rot.

---

## 1. Gaps that contradict a guarantee the spec already makes

These are the serious ones. In each case [architecture.md](architecture.md)
states a property, and the code does not implement the mechanism that property
depends on. Until these close, the guarantee is a claim, not a behaviour.

### 1.1 There is no repair. Nothing regenerates a lost fragment.

[§2](architecture.md) promises the cluster survives **f simultaneous permanent
node losses without data loss**, and assumes "a node that loses its local
storage rejoins empty and rebuilds from peers".
[protocols/data.md](arch/protocols/data.md) goes further: a degraded chunk
generation **consumes protection budget until repair completes**, and a
returning node "restores capacity, not the fragments it never held".

No repair exists. The only background data-plane thread on a server is the GC
reaper (`host_gc_thread` in `src/server/raft_host.c`), which *deletes*
fragments orphaned by unlink and truncate. Nothing scans for a live chunk whose
fragment set is short, and nothing re-encodes a missing fragment onto a healthy
node.

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
path is specified in [§2](architecture.md) (publish with ≥ k+(f−u) fragments
when u domains are already unavailable) and is not implemented — publication
requires the full ACK set or fails.

### 1.3 The client does not run the session protocol, so fencing is not real

Invariant **I23** and [protocols/sessions.md](arch/protocols/sessions.md)
define a session with an epoch and a revocation barrier, and make it the
fencing token: a partitioned-but-alive client is fenced at epoch+1 and can no
longer make an authoritative mutation. The server side of this exists
(`host_sess_gate`, the session applies in `raft_host.c`). The FUSE client does
not participate — when a request carries no session suffix the server
substitutes a stand-in UUID.

So the mechanism that is supposed to stop a stale client from corrupting state,
reclaim its locks, resolve its append reservations, and drop its open-unlinked
leases is inert on the only production client. What exists instead is
connection-level retry (`rpc_send_recv_shard`: 16 attempts with backoff,
peer marked down for 30 s after four failures), which recovers from a dropped
socket but carries no state and provides no fencing.

### 1.4 Fragment integrity is verified inconsistently

Invariant **I25** says every durable fragment is checksummed over identity plus
payload, and a failing fragment is treated as unavailable and repaired, never
fed to the decoder. Partially true: the server re-hashes on GET and returns
NOT_FOUND on mismatch, which correctly keeps bad bytes out of the decoder. But
client-side verification is off unless `EFS_READ_VERIFY` is set, PUT stores
the digest the client sent (the 4 KiB tail of the fragment file) without
re-hashing the payload it just received, and
"never decoded, always repaired" cannot hold while §1.1 is open — a corrupt
fragment is detected and then nothing repairs it.

### 1.5 Two clients writing disjoint ranges of one chunk — CLOSED

**I12** CAS is live (Sep 18 2026, leftover-1 19810). `efs_chunk_rec` carries
`base_gen` + `chunk_generation`; the leader CASes the writer's base; apply
STALE is audible and the client refetch+overlay+PUTs. Gate:
`results/stress/20260918-n1-w1/` lost=0; `peer_shared_pwrite` 5/5 concurrent.
Do not reopen with a distributed chunk lock. Details in START-HERE **W1**.

### 1.6 `write()` is specified as durable-and-visible; the client buffers — CLOSED

**Done Sep 18 2026**, option (i): the spec moved. [§3](architecture.md) now
says a returned `write()` is client-buffered; durable + cross-client
visible at `fsync` / last `close` / `O_SYNC`. Measured
`results/stress/20260918-w2/`: peer 0/10, kill -9 loses 64 MiB.
`O_SYNC` is specified, not wired. START-HERE **W2**.

---

## 1b. What an HPC site will ask for and not find

None of these contradicts the spec; each is a capability every deployed
parallel filesystem has and efs does not.

| ask | state | note |
| --- | --- | --- |
| IOR / mdtest / IO-500 numbers | **9×4 debug, every phase** | `results/io500/20261001-074905-rdma/` (fresh table, Oct 1): easy-write 5.17 GiB/s, hard-write 0.52, mdtest-easy-write 6.2 kIOPS, easy-stat 24.4 kIOPS, hard-read 0.82 with one read error (W38, open). Sep 30 `20260930-183504-rdma/`: 0 read errors, cold hardscan clean. No stonewall-compliant run yet (debug = 1 s stonewall, same-mount reads). |
| N-1 shared-file writes that are correct | **gated** | §1.5; `peer_shared_pwrite` concurrent 5/5. |
| per-file / per-directory layout (`lfs setstripe`-style chunk size, EC profile) | absent | export-wide only; the declared 32× small-write amplification has no opt-out. |
| a client other than FUSE (kernel module, user-space library, MPI-IO ADIO driver) | absent | FUSE-only. libfuse 3.10.2: ≤128 KiB per request, no `FOPEN_PARALLEL_DIRECT_WRITES`; Linux serializes extending direct writes and `O_CREAT` per inode/dir per mount, so many ranks on one node serialize above efs. Already an open item in [§9](architecture.md). |
| the data path on the fast interconnect | **RDMA** (since Sep 28) | `EFS_TRANSPORT=rdma` on the test cluster; zero-copy sends (W39); zero-copy receive is still a design ask. |
| a hardware-relative throughput statement | 1 client: 1.3–1.5 GB/s write (~8–9 % of 16.7 GB/s), 3.6 GB/s cold read, 6.5 GB/s with four readers. 9 clients: 2.5–2.8 GB/s aggregate write (~6 % of 44 GB/s). | derivation in START-HERE §1a. |
| MPI-IO hints, collective-buffering guidance, Darshan/instrumentation hooks | absent | — |
| burst-buffer / tiering / HSM | absent, not designed | flash-only by decision (§1); no policy layer exists either way. |

---

## 2. Missing product surface

Real gaps, but they contradict nothing — the spec simply does not claim them
yet. Roughly in the order a user would notice.

| Capability | Status | Where it stands |
| --- | --- | --- |
| **Authentication** | absent | Plain TCP/RDMA. No TLS, shared secret, Kerberos, or capability tokens. `EFS_MSG_HELLO` gates *cluster join* on build ID; it authenticates nothing. Any host that can reach port 19810 is a peer. |
| **Authorization** | POSIX only | uid/gid/mode checked in the FUSE daemon (`check_access`). No ACLs. [§2](architecture.md) is explicit that clients are assumed non-Byzantine and that hostile clients would make capability-based data authorization mandatory — so this is a scope boundary, not an oversight. |
| **fsck / offline consistency check** | absent | Nothing validates dangling dentries, orphan inodes, or chunk maps pointing at absent fragments. The simulator checks invariants on simulated state; there is no tool for a real cluster. Listed in [roadmap "tests still to write"](scaling-roadmap.md) as `fsck --verify-only`, unbuilt. |
| **Extended attributes** | `user.*` only | One blob per inode (`EFS_KV_KIND_XATTR`), raft command `EFS_MD_CMD_XATTR`. set/get/list/remove for names starting with `user.`. `security.*` and `system.*` return EOPNOTSUPP and are not stored. No ACLs. `opt_xattr` passes (`results/posix/20260928-043918`). |
| **User-visible snapshots** | absent | No snapshot/clone FUSE surface. Do not confuse with Raft InstallSnapshot or `kv_snap.c`, which are internal replication machinery. |
| **Node removal (data plane)** | absent | `add-node`/`JOIN`/`NODE_LEFT` update the membership list in `cluster_nodes.bin`. No fragment evacuation, no rebalance onto a new node. `shrink-quota` refuses when `used > new_quota` rather than migrating. |
| **Multi-export** | partial | Server arrays hold `EFS_MAX_EXPORTS` (16) and `raft-mkfs` creates one, but list/create/destroy are retired opcodes and the client selects one export name at mount. |
| **Quota** | node-level only | Per-node byte quota enforced on new fragment creation (`EFS_ERR_QUOTA` → `ENOSPC`). No per-user, per-group, or per-directory quota, and no inode quota. |
| **Metrics** | human-readable only | `efs-mgmt status` / `raft-status` text, plus per-directory `.stats` through FUSE. No Prometheus endpoint, no structured stats, no time series. |
| **Logging** | unstructured | `fprintf(stderr, ...)` throughout, no levels, no rotation, no request IDs. (`raft_log_rotate_locked` is WAL compaction, not application logging.) Debug output is env-gated per subsystem, which is not the same as a log level. |
| **Non-FUSE access** | absent | FUSE is the only client. No NFS or SMB re-export, no library/DAOS-style direct API. Note [§9](architecture.md) already bounds intra-mount scaling because upstream Linux serializes extending direct writes, `IOCB_APPEND`, and `O_CREAT` per mount — so a second access path is a performance question, not only a compatibility one. |
| **Rolling upgrade** | works, narrowly | HELLO rejects a build-ID/version mismatch, so same-build restarts roll one at a time. There is no wire version negotiation, so an actual version *change* requires stopping every server together. |

---

## 3. Verification that does not exist

From [roadmap "tests still to write"](scaling-roadmap.md), because it belongs in
any honest readiness assessment: **there is no fault-injection harness at all.**
POSIX layers 1–3 (201 single-client tests, 63 two-client) cover syscalls, peer
visibility and same-file races on a healthy cluster. Layer 4 — crash after
fsync, kill between EC and publish, degraded read with a node down, silent
corruption repair, partition and fencing, lock-holder crash, kill mid-rename on
a spreading directory — has no harness.

Note the circularity with §1: several Layer 4 cases are the only way to test
repair and fencing, and repair and fencing are the things that do not exist.

---

## 4. Performance distance

Not a capability gap, but it belongs in "usable product". Current references
(Oct 1 2026, RDMA, flush inside the clock; START-HERE §1a has the table):
one client writes 8 GiB `dd bs=1M conv=fsync` at **1.3–1.5 GB/s** and reads
a cold 16 GiB file at **3.6 GB/s** (6.5 GB/s with four readers); nine
clients write **2.5–2.8 GB/s** aggregate. The binding ceilings are the
client's 200 Gb/s link (≈16.7 GB/s logical write after 2+1 EC) and the four
hosts' NVMe (≈44 GB/s logical) — so writes sit at roughly **6–9 %** of the
hardware and single-client reads at ~20 % of the link. [§1](architecture.md)
is unambiguous that a benchmark stopping at a software serialization point
is by definition an EFS bug; the current levers are listed in START-HERE
(read-side zero-copy receive, the per-client `report_mu`, IOR-hard's
shared-chunk publish rate).
