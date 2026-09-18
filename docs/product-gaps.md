# What is missing to be a usable product

[Architecture](architecture.md) · [What to work on](arch/START-HERE.md) ·
[Roadmap](scaling-roadmap.md) · [Testing](testing.md)

This page answers one question: **between here and a filesystem someone could
actually run, what does not exist?** It is a capability inventory, audited
against the source on Sep 18 2026 — not against comments or spec prose. A
function that exists but is only reachable from tests counts as absent.

It is deliberately separate from [START-HERE §1a](arch/START-HERE.md), which is
the *near-term work queue* (W1–W12: two correctness items, then measured performance and harness gaps).
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
client-side verification is off unless `EFS_READ_VERIFY` is set, PUT writes the
`.sum` sidecar without verifying the payload it just received against it, and
"never decoded, always repaired" cannot hold while §1.1 is open — a corrupt
fragment is detected and then nothing repairs it.

### 1.5 Two clients writing disjoint ranges of one chunk lose one of them

**I12** and [protocols/data.md](arch/protocols/data.md) specify sub-chunk
read-modify-write as a generation CAS against the base the *writer* read, with
refetch-and-retry on conflict, so that disjoint ranges from different clients
both land. The implementation cannot fail that CAS: `struct efs_chunk_rec`
carries no base generation, and the server fills `expected_gen` from the
generation it reads immediately before proposing (`server_raft_host_report`).
Two further facts make it silent end to end: the client never learns a chunk
generation at all (`GETCHUNKS` returns the same generation-less record), and
the batched publish apply discards per-record results (`(void)
apply_one_publish`, returns `EFS_OK`), so even a CAS that did fire would not
reach the report reply. Cross-client exclusion is process-local only
(`efs_wb_ino_lock`). The result is a silent lost update, and the write that
was lost returned success.

This is the N-1 shared-file pattern — IOR-hard, MPI-IO, one HDF5 file per
job — i.e. the workload an HPC parallel filesystem exists for. The suite-2
test named "IOR-hard write" runs the two writers sequentially and cannot see
it. Scheduled as START-HERE **W1**, with the deterministic repro and the fix
shape written out there.

### 1.6 `write()` is specified as durable-and-visible; the client buffers

[§3](architecture.md) states that a returned `write()` is durable and visible
to every client. `efs-fuse` patches the write into its dcache and publishes at
`fsync`, `close`, or reclaim. Consequences: an acknowledged write is lost if the
client node dies first, and a peer reads stale bytes until the writer syncs —
close-to-open coherence, which is not what §3 promises. Buffering is probably
the right choice for HPC; the contradiction is what has to go, one way or the
other. START-HERE **W2** (ASK).

---

## 1b. What an HPC site will ask for and not find

None of these contradicts the spec; each is a capability every deployed
parallel filesystem has and efs does not.

| ask | state | note |
| --- | --- | --- |
| IOR / mdtest / IO-500 numbers | **never run** | `tests/perf/io500/` exists since Aug 20 with no result; mdtest disabled in its ini. There is no metadata-rate number on this engine at all. START-HERE W6. |
| N-1 shared-file writes that are correct | **broken** | §1.5 above. |
| per-file / per-directory layout (`lfs setstripe`-style chunk size, EC profile) | absent | export-wide only; the declared 32× small-write amplification has no opt-out. |
| a client other than FUSE (kernel module, user-space library, MPI-IO ADIO driver) | absent | FUSE-only. libfuse 3.10.2: ≤128 KiB per request, no `FOPEN_PARALLEL_DIRECT_WRITES`; Linux serializes extending direct writes and `O_CREAT` per inode/dir per mount, so many ranks on one node serialize above efs. Already an open item in [§9](architecture.md). |
| the data path on the fast interconnect | TCP over IPoIB | RDMA is implemented but not the gated default; every ceiling in START-HERE assumes it (W10). |
| a hardware-relative throughput statement | 1.3–2.7 % of one client's 16.7 GB/s logical write ceiling; ~2 % of the cluster's; reads ~13 % | derivation in START-HERE §1a. |
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
| **Extended attributes** | absent | `efs_ll_ops` has no `getxattr`/`setxattr`/`listxattr`. Note this blocks the parked expiry feature, whose stated user surface is the xattr `user.efs.expire`. |
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

Not a capability gap, but it belongs in "usable product". The measured
single-client write path is **448 MiB/s** (8 GiB `dd bs=1M conv=fsync`) and
**924 MiB/s** (honest fio, 9×2g sw-1m). The binding ceilings are the client's
200 Gb/s IPoIB link (≈16.7 GB/s logical write after 2+1 EC) and the four
hosts' NVMe (≈44–57 GB/s logical) — so writes sit at roughly **2 %** of the
hardware. Reads reach 3141 MiB/s single-client, ~13 % of the link.

4- and 9-client throughput has never been measured on this engine, so no
scaling factor exists yet. [§1](architecture.md) is unambiguous that a
benchmark stopping at a software serialization point is by definition an EFS
bug, so this is a stated-goal gap, not merely tuning. It is the one item on
this page that *is* scheduled: START-HERE W3 (split the fsync tail) and W4
(multi-client).
