# NUC integrated-code evidence — 2026-10-11

Branch: `devel-nuc`; initial all-ref capture integrated at `c3118143b3e1`.
Core fixes validated as `108afa3e-nuc`; later changes affect tests/docs only.

`initial/` retains baseline failures, copied-store causal controls and old-source
regression failures. `collected/` retains final native/live outputs, fourteen
owned fixture manifests, source/binary hashes, ABBA append/drive controls,
CPU/strace summaries, enabled commands and restored-service checks.
`collected/acceptance-checks.json` is the checked final acceptance summary.

Full POSIX and peer TSVs are under `collected/fixtures/`, with exact binary/source
identity in each manifest. Compressed text uses gzip. Binary perf data remains
on NUC under `/data1/efs/devel-nuc-20261011/` and is referenced by size and SHA-256;
it is not duplicated into Git. Interrupted 20k runs and verifier failures are
retained and are not accepted as passing tests. Copied KV inventories never
open the retained original fixture store; original hashes are checked.

Final native full-suite snapshot uses test/docs/Makefile overlay `658e42a2`.
A later focused real-process identity regression records its separate source
hashes. Engine C/H matches the final branch; the only later Makefile change adds
the cleanup regression to two test targets. Installed NUC images stay unchanged,
and all prior 16 sample payloads are verified on both restored mounts.

The session status ledger explains scope, timing and remaining W52/W72/W87/W97
limits. No firmware/cache diagnosis, hardware ceiling, full-tree preservation
inventory, expiry deletion, NFS or billions-of-files acceptance is claimed.
