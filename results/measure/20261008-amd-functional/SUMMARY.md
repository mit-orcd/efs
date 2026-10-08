# AMD functional package — Oct 8, 2026

Baseline devel `53be1da7` (v0.2.1-pre-alpha). Commits `fe2ce494`
(append identity/authority/overflow), `311c4e4e` (concurrent retry ownership),
`ec5e0387` (lazy RDMA send registration). Functional short cycles use
**direct I/O/TCP**. Targeted buffered/RDMA gates are explicitly identified.
Four daemon processes on one AMD host; RDMA is software RXE/rxe0.

| Check | Result |
| --- | --- |
| Initial fixed normal Linux units | Full make test passes; `full-units.log` |
| Initial direct/TCP service | POSIX 216 pass, 0 fail, 1 unsupported skip; peer 64/64; prepare/verify 26/26 each |
| Append duplicate allocation BEFORE | Real 32 simultaneous identical reserve requests: one incorrectly receives offset 8/end 16; retained failing fixture `append-replay-before` |
| Append duplicate allocation AFTER | All 32 receive offset 0/end 8; exact eight bytes after cold restart. Final nonblocking dispatcher also passes inside `rdma-pressure-lazy` (these raw control RPCs use TCP) |
| W87 buffered broad workload | Full POSIX 216/0/1 skip, peer 64/64, real server GC effects and exact 800 mixed-description append records after cold restart. Original NUC failure TSV/manifest retained; it lost W1S0002 and W0S0004 |
| W84 maximum namespace transaction | PREPARE and COMMIT coordinator deaths both record exactly 144 participants; cold atomic state survives. Depth 143 accepted, 144 refused in 8.005s without changing the source |
| RXE active-write leader stop | Initial 53 acknowledged files, 63.4ms exit; later fault-memory fixture 48 acknowledged files, 63.4ms exit (writer observes EIO). All acknowledged bytes cold-verify. Lazy-registration fixture exits in113.5ms and cold-verifies51 acknowledged files |
| RDMA transport | 67 QP frames plus one deliberate oversized TCP side-channel frame; no skip |
| RDMA production budget BEFORE, 1 GiB scope | OOM kill at 1 GiB while recovering retained publication; kernel/systemd proof in `rdma-oom-proof.log` |
| Diagnostic BEFORE, 2 GiB scope | Sampled peak 1214.3 MiB whole fixture and 675.0 MiB FUSE RSS. Sparse checkpoints exceed the old 512 MiB RSS gate. `rdma-memory-2g.jsonl` is a partial diagnostic trace, not continuous acceptance monitoring |
| RDMA lazy-registration AFTER, 1 GiB/no swap | Kernel peak 625795072 bytes (596.8 MiB), zero max/OOM/kill events. Same 256+64 MiB body/drain limits. 2037 accepted sparse writes retained, publication/scratch failures refuse finitely, same-mount admission recovers, cold bytes pass; eight sparse writers/256 writes also pass |
| RDMA real IOR + cold hardscan | 4 ranks ×3000 segments, 12000 records, 564096000 bytes; zero bad/short records. Full two-peer fold, physical deletion, unavailable-member retention, restart/quota and late-PUT fences pass |

The first IOR cleanup run failed because queue counters described a GC pass
before unlink. The gate now requires **physical absence and drained queues**
in one bounded wait; the original failure is retained. This is a harness
acceptance correction, not proof of a daemon reclamation failure.

Per-run folders contain source/test/binary hashes in manifests, runtime RDMA
FD proof and the named JSON/TSV outcomes. Large databases and successful
server debug logs remain on AMD under the `Evidence directory` paths in each
run log. Failed fixtures are preserved too. The auxiliary lazy-memory monitor
stopped during process retirement; its partial trace is excluded from claims.
The passing cgroup's own kernel peak and OOM counters are authoritative.

The harness borrows AMD's two host-policy-permitted empty mount paths while
the installed service is cleanly stopped. Every private run uses unique stores
and ports 28432–28435. Installed n1–n4 stores are untouched. Additional private
FUSE mounts require an administrator's host policy change; one client with
four IOR ranks is not nine clients or nine physical hosts.

Durable append receipts/ACK retirement across leader changes remain open.
W84 still needs a full-ancestry protocol or an explicit supported finite
transaction limit. The 44 open/eight awaiting grouped count is unchanged.
Final normal-unit and four-mode service matrix results are appended below.

## Final normal service matrix

Normal code `ec5e0387` with working gate/documentation changes; identical
production C source across all cells. The source-only final documentation
commit does not alter daemon/client C code. Full normal `make test` passes
(`final-units.log`), including the regenerated5/5 documentation gate.

| Storage / transport | Single-client POSIX | Peer POSIX | Cold persistence |
| --- | --- | --- | --- |
| direct / TCP |216 pass,0 fail,1 unsupported skip|64/64|26/26 prepare +26/26 verify|
| buffered / TCP |216 pass,0 fail,1 unsupported skip|64/64|26/26 prepare +26/26 verify|
| direct / RXE RDMA |216 pass,0 fail,1 unsupported skip|64/64|26/26 prepare +26/26 verify|
| buffered / RXE RDMA |216 pass,0 fail,1 unsupported skip|64/64|26/26 prepare +26/26 verify|

Each cell captures argv, explicit EFS_TRANSPORT and verbs-FD presence/absence
for all four daemons and both FUSE clients, before and after persistence.
The installed service is restored to direct-I/O/TCP after owned private tests.

## Final shutdown/churn acceptance

The final direct-I/O/RXE fixture holds16 idle TCP readers and16 partial TCP
frames while a separate connection-churn worker runs beside active RDMA writes.
The leader exits cleanly in 113.5ms; all 52 acknowledged files verify after
full restart/cold remount. Reader stalls are on the TCP framing side; the actual
data/client connections are RXE, confirmed by verbs-FD evidence. The complete
physical-GC/restart fixture passes (`rdma-shutdown-churn`).
