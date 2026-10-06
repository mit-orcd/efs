# Local benchmark CLI relocation — Oct 6 2026

Moved local data/meta engine benchmarks from `efsd --bench` to
`efs-bench --bench`. The local harness now lives under `src/bench` and links
sectioned copies of the unchanged production store, NVMe adapter, writer pool
and iostats implementation. Unused daemon/network entry points are discarded.
The existing 1 MiB thread creation helper is shared from `src/server/thread.c`.
No daemon startup, sockets or quorum participate in local runs.

Existing seed-based efs-bench modes and server BENCH_PUT RPC remain: the latter
is required to measure remote network throughput. Daemon perf/strace diagnostics
also remain. Historical measurement labels were preserved; current usage and
status were updated.

Validation on the NUC host, private `/data1/efs/review/ten-rounds` source:

- Build of efsd and efs-bench PASS; full `make -j8 test` PASS.
- CLI regression: help/ownership, invalid duration/writer options, incompatible
  local/remote modes, missing roots and nonempty-root sentinel preservation PASS.
- Real local metadata and data smoke, 0.03 s per engine round, writer pool of two
  for data, disposable empty scratch roots: PASS, reported errors zero.
- `nm efsd` contains no local benchmark runner symbols.
- Documentation gate 5/5 and `git diff --check` PASS.

An initial remote full-test attempt stopped because generated documentation had
not yet been synced; regeneration/sync followed by the full rerun passed.

[Smoke output](smoke.log), [full test output](test.log).
These short smoke runs validate execution, not storage performance. The live
four-node cluster was not redeployed or exercised by this change.
