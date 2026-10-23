# W92 private query publication smoke, Oct 8

Private TCP cluster under /data1/efs/evidence/w92-query-fixture-20261008c, ports
24800–24803. Production /data1/efs/src, stores and mounts unchanged; population
continues. Four owned daemons terminated cleanly. Stores/logs retained remotely.
The rsynced offline build has no Git metadata: --version says unknown; exact
binary SHA256 identities are retained. Source belongs to commit `220cf45a` (catalog component `ed2808f2`).
The version.o build timestamp is older than the final link; SHA256 is the exact
artifact identity here. This is not a clean deployed binary/build-ID claim.

`tests/live/query_publication.py` passes: dual-group voters n2/n3 publish an
initial catalog, actual Raft CREATEs converge to four files during quiet T
refresh, partial hosts n1/n4 never publish, and a full fixture restart rebuilds
all 4096 partitions from persisted metadata with four files. Empty initial build
7–17 ms in the earlier fixture; these are tiny-fixture timings, not scalability
measurements. Read final logs for exact current timings/watermarks. A negative
raft-mkfs reply during election is retried; it is retained, not counted as a
successful operation.

Existing shutdown gate also passes all six buffered/direct variants with the
new worker: idle, partial TCP framing, accept churn and a worker-targeted signal.

Host publication now refuses imports, non-voters/partial group coverage and
metadata changes observed across post-capture ReadIndex barriers. Unresolved
transaction records refuse catalog build. No snapshot scan/fold under host
apply lock; bounded memtable pin still holds that lock and requires loaded
latency measurement. N=256 metadata-key changes, T=2000 ms, 200 ms minimum
attempt spacing; failed attempts back off 2000 ms. Candidate budget 256 MiB,
memtable-copy budget 64 MiB, 10 s cooperative build deadline, one candidate plus
one published catalog. Old publication/source vectors survive failed refresh.
Continuous metadata activity can refuse a consistent cut and exceed T; stale
age/error must be exposed, not claimed as freshness or bounded acceptance.

Internal borrowed-view callback API exists, but no virtual query RPC consumer
is wired yet. Fresh per-user permissions/ancestry, output semantics, live stats,
large namespace, concurrent transactions and fault gates remain required.
