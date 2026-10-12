# ORCD v0.3.1 ordinary files / RDMA / direct I/O — 2026-10-12

Requested release: `release-v0.3.1-pre-alpha`, frozen at
`40f4509dff748aec6d66c73bd2da511b438f42b7`. This is a new ORCD execution;
AMD TCP results and previous ORCD candidates do not transfer their verdicts.

The live profile uses four physical storage hosts (fcstor003–006), six storage
roots each, seven client hosts (fcstor007–013), native RDMA on `mlx5_2`, and
explicit `--direct-io --storage-engine files`. Server commands, client
commands and packed writes are explicitly zero. Experimental repair stays
zero. The existing stores were preserved and previous source trees retained
at `/tmp/efs-before-profile-40f4509dff74`.

Small-file packing, containers, `.efs` commands/queries and dedicated
unplanned-event repair/recovery are excluded from mounted qualification.
Native bundled component checks do not certify those disabled features.
The historical TCP-only standalone repair/fault harness is not an RDMA
mounted test and is not reported as one here.

## Prerequisites and runtime proof

All eleven normal builds passed. The full normal native suite, early-ACK
Raft gate, separate fault build/full native suite, eight affected ASan/UBSan
extractors, and fully instrumented Raft native/early-ACK gates passed on
fcstor006 before the live switch. No builds or profilers overlap the quiet
mounted measurements.

All 777 committed code/test input files matched the frozen release on every
host before and after deployment. Runtime proof records the exact revision,
UID 246111 (`erbmi1`, nonroot), binary hashes, native verbs device descriptors,
server direct-I/O/files arguments and all disabled feature flags. Every
running executable hash matches its staged binary manifest. `.efs` is absent.
All seven mounts read the exact acknowledged pre-upgrade data after the
orderly drain/unmount/server-stop/redeploy sequence.

## Mounted results

The first seven-client concurrent core run, 16 jobs per client, passed
**1,512/1,512** cases with zero failures, skips or NOTRUN. The original
30-second minimum per-case budget and longer existing decorator budgets
were retained. Whole-suite times were approximately 31 seconds per client;
these are correctness-suite durations, not a storage-throughput benchmark.

The first cross-host peer run passed **67/67** cases. All seven clients
subsequently drained and unmounted without force/lazy detach; all four servers
stopped through the identity-checked SIGTERM path without SIGKILL. All seven
restarted mounts read identical fsynced ordinary bytes. Real payload/checksum
files existed before unlink and disappeared across all 24 storage roots.
This is ordinary-object physical GC, not shared packed GC.

The post-restart core repeat again passed **1,512/1,512**, with no failures,
skips or NOTRUN; whole-suite times were approximately 21 seconds per client.
The peer repeat also passed **67/67**.

**Verdict: PASS for ordinary files/native RDMA/direct I/O. No test failures
were observed in the selected scope.** Both rounds used the original budgets;
no failing case was replaced by a quiet single-client retry. The first peer
run took 113.2 seconds and the repeat 100.0 seconds, including cross-host SSH
coordination. All seven clients remain mounted on the tested source. Disabled
features and dedicated recovery remain unqualified.

During testing the release advanced to `4f8c25e0` with independent AMD
TCP/buffered documentation/evidence. Its product/test tree is identical to
the frozen `40f4509d` tested here. Those AMD results are not counted as ORCD
RDMA results. No product code or release ref is modified by
this qualification.

[Raw execution evidence](../../results/orcd-v031-rdma-direct-20261012-40f4509d/)
includes the exact controller scripts, source/binary manifests, native and
sanitizer logs, per-host TSVs, cold hashes and ordinary physical-GC observations.

All collected artifact SHA256 hashes were verified, alongside exact per-host
PASS counts and source/binary identity. Findings and evidence are saved on
the isolated ORCD branch; the requested release ref and product code were
not changed by this work.
