# SUMMARY — 20261008-w92-query-publication

## Purpose and linked item
Smoke gate for the first W92 server-owned query catalog publication: bounded
background catalog builds with source watermarks, voter-only publication, and
restart rebuild from persisted metadata. Linked item: **W92** (.efs/find and
.efs/stats server snapshots, `docs/backlog/work-items.md`). Added with commit
`220cf45a4` ("server: publish bounded background query catalogs with source
watermarks").

## Verdict
**PASS** (smoke only). `fixture.log` ends with
"quiet metadata refresh and partial-host refusal PASS",
"restart rebuild from persisted metadata PASS",
"query_publication: PASS (smoke only)". The separate shutdown gate also passes
all six buffered/direct variants (`shutdown.log`).

## Source commit and build identity
Source commit `220cf45a` (catalog component `ed2808f2`). Rsynced offline build
with no Git metadata; `--version` reports unknown. Exact artifact identity via
SHA256 in `binary-identity.txt` (efsd `c11bc95c…`, efs-mgmt `136651d8…`), built
2026-10-08T06:28:47Z on nuc. The README warns the version.o timestamp predates
final linking — SHA256 is the exact identity, not a clean build-ID claim.

## Environment
Private TCP cluster of four owned daemons under
`/data1/efs/w92-query-fixture-20261008c`, ports 24800–24803, on the NUC host.
Production `/data1/efs/src`, stores and mounts unchanged; population continues
during the test. Tiny fixture (initial build 7–17 ms in an earlier fixture) —
not a scalability measurement.

## Method
`tests/live/query_publication.py` (repo path) against the private fixture.
Checks: dual-group voters n2/n3 publish an initial catalog; actual Raft CREATEs
converge to four files during quiet T=2000 ms refresh (200 ms minimum attempt
spacing, 2000 ms backoff on failure); partial hosts n1/n4 never publish; full
fixture restart rebuilds all 4096 partitions from persisted metadata. Candidate
budget 256 MiB, memtable-copy budget 64 MiB, 10 s cooperative build deadline.
Cleanup: four owned daemons terminated cleanly; stores/logs retained remotely.
The shutdown gate exercises idle, partial TCP framing, accept churn and a
worker-targeted signal across six buffered/direct variants.

## Results
| Check | Result |
| --- | --- |
| Initial catalog by dual-group voters | PASS |
| Quiet refresh converges to 4 files | PASS |
| Partial hosts n1/n4 refusal | PASS |
| Restart rebuild of 4096 partitions | PASS |
| Shutdown gate, 6 variants | PASS (0.215–0.264 s each) |

## Limitations and remaining gates
Tiny-fixture smoke, not scale evidence. Host publication refuses imports,
non-voters/partial coverage and cross-barrier metadata changes; unresolved
transactions refuse catalog build. No snapshot scan/fold under host apply lock;
the bounded memtable pin still holds that lock and needs loaded latency
measurement. Continuous metadata activity can refuse a consistent cut and
exceed T — stale age/error must be exposed. No virtual query RPC consumer is
wired yet. Fresh per-user permissions/ancestry, output semantics, live stats,
large namespace, concurrent transactions and fault gates remain required.
W92 remains open.

## Evidence index
| File | sha256 (16) | Carries |
| --- | --- | --- |
| `README.md` | c41167b77a3e206a | Full scope, bounds, remaining gates |
| `binary-identity.txt` | 7467cefb22b8066a | Exact binary SHA256 identities |
| `fixture.log` | b9224ab44b45c970 | PASS verdicts, raft-create records |
| `shutdown.log` | 04fabda2c5d60070 | Six-variant shutdown gate PASS |
| `n1-restart.log` | 779c46e558bf9242 | Restart rebuild evidence |
