# W92 disk backend evidence — Oct 9, 2026

Branch: `devel-find-stats`, baseline `da653c33`, implementation `9eb3dd51`. Xorinox Linux VMs; files engine,
direct I/O, TCP. Installed stores were retained. Private fixture source:
`/home/efs/query-disk-src`; private metadata clone:
`/data1/w92-diagnostic-kv` on xefs1. No private metadata or binaries are included.

## Functional gates

`make all`, `tests/test_query_disk`, and `tests/test_query_catalog` pass.
Disk/RAM differential runs use 3000 and 12000 files with the same <4 MiB
accounted builder budget. This is allocation accounting, not a complete RSS
scale gate. Free-space refusal uses `EFS_QUERY_DISK_RESERVE_MIB=1048576` and
`EFS_QUERY_TEST_NOSPACE=1`; it passes. The optional `/usr/bin/time` attempt failed
because the utility is absent; the 12000-file test was rerun directly and passed.

The four-daemon private fixture invocation was:

```sh
EFS_QUERY_RPC=1 EFS_QUERY_FUSE=1 EFS_QUERY_POSIX=1 \
  python3 tests/live/query_publication.py /home/efs/query-disk-final-live
```

It passed command/RPC versions, permissions, streaming/pinned queries, POSIX,
peer and restart oracles. See `query-disk-final-live.log`. This is smoke acceptance.

`query-disk-release-live.log` records the old fixture's false failure when a
valid cold restore reported rebuilt=0. Test commit `e4d29d16` permits verified
restore or rebuild; `query-disk-release-live3.log` passes both restart publication
and independent post-restart filename checks with the stamped committed binaries.

The private copy excludes results archives referenced by existing docs, so the
architecture link gate fails. Code-gate invocations use a temporary Makefile with
only the architecture checker command omitted. A concurrent rerun hit the
one-second/QD256 benchmark scheduling deadline; `query-disk-bench-retry.log`
records its subsequent isolated PASS. The final complete code-gate rerun passed (`query-disk-complete-gates.log`).
The transient lifecycle test race also passed on an unchanged focused rerun
(`query-disk-tail-gates.log`).

## Retained namespace blocker

Installed `.efs/stats --json` returned EIO. The isolated clone reproduces the
publication refusal: directory inode 4113, parent 9876, nlink 2; incoming
`level_2` entry lives in shard 0, while the LOCAL parent's authoritative shard
is 1684. Direct `efs_meta_apply_lookup` returns NOT_FOUND. See
`query-disk-retained-probe2.log` and `query-disk-orphan-probe2.log`.

Both backends refuse a live directory lacking an authoritative incoming edge.
`query-disk-placement-test.log` records the focused regression. No metadata
repair/relocation was performed; provenance and repair remain a separate step.
Normal servers, gateway and client mounts were restored. All three servers ran
as `efs`, with direct I/O, and matched their installed executable; all three
reported `9eb3dd51` and the same efsd SHA-256:
`ce5e32424ef3957353f42e957242c83a2ce81d316b5b439290ad29c8f7c168fe`.
Gateway and test-client root stats passed; `.efs` remained opted out on ordinary
mounts. `final-version.txt`, `final-binary-sha256.txt`, `final-runtime.txt` and
`final-query-runtime.log` record the final installation. The installed-cluster
query gate is not passed.

`catalog_probe.c` and `orphan_probe.c` are offline diagnostic tools run only on
the stopped-state private clone. `deployed-sha256.txt` and `runtime.txt` record
the deployment at collection time.

[Implementation status](../../../docs/status/query-disk-backend-20261009.md).
