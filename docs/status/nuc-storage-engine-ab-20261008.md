# P4.1 — NUC storage-engine A/B, 2026-10-08

This checkpoint repeats the experimental `devel-storage-engine` files/containers
comparison on the NUC. It does not close P4.1 or change the default engine.

## Deployment and topology

Source baseline: `73eb7fd5`, plus the dedicated
[`ab_storage_engines_nuc.py`](../../scripts/ab_storage_engines_nuc.py) runner.
The deployed tracked source and runner are verified by SHA-256. Native compilation
uses the NUC's CPU instruction set. Source, binaries and tests are deployed into
`/data1/efs/bench/nuc-engine-ab-20261008/src`; installed binaries and services
under `/data1/efs/src` are not replaced. The full test invocation rebuilt the
copied tree without `.git`, so the binaries report unknown Git metadata; the
source verification manifest and final binary hashes identify the tested build.
All four daemons use the same binary throughout the matrix. For subsequent
copies, pass `EFS_GIT_ID`, `EFS_GIT_DESCRIBE` and `EFS_GIT_BRANCH` to both build
and test invocations to preserve human-readable revision information.

Four private daemons share an Intel i5-1135G7 (4 cores / 8 threads), 32 GiB RAM,
and the same bare-metal Linux host. Nodes 1–2 use SATA XFS roots below `/data1`;
nodes 3–4 use NVMe XFS roots below `/data2/efs/bench`. `/data2/efs` resolves into
`/home/efs/additional-work-dir`, on `/dev/mapper/rl-home`; `/data2` alone is not
the relevant filesystem. The installed four-node group remains running, so this
is a shared-host measurement, not an idle-machine or multi-host network result.

Each start **and restart** verifies `/proc/PID/cmdline` against the exact command:

```text
EFS_MD_RAFT_N=4 EFS_TRANSPORT=tcp efsd
  --node-id ID --addr 127.0.0.1 --port PORT
  --storage PRIVATE_ROOT --quota 2G --writers 2
  --storage-engine files|containers --no-direct-io|--direct-io
  [--join 127.0.0.1:27432]
```

IDs are 1–4 and ports 27432–27435. Only nodes 2–4 receive `--join`.
The FUSE client uses the same Raft cardinality and TCP transport. Relevant inherited
`EFS_*` overrides are removed. Membership must show all four private endpoints
before formatting or mounting. The runner also waits for both Raft groups to
elect leaders: node membership alone is insufficient. Fresh roots and unused ports are required; recorded
private PID command lines are checked before signalling during cleanup.

## Workload and gates

The existing fixed-client workload is unchanged: 256 deterministic 512 KiB files
(128 MiB), four workers, full-file reads with SHA-256 verification, and durable
128 KiB overwrites of 32 worker-disjoint hot files. Each file creation and overwrite
is followed by `fsync`. Reads and overwrite churn each run for 30 seconds.

Two passes alternate the four engine/I/O combinations in reverse order. Every case
verifies data, stops and restarts all four daemons, verifies acknowledged contents
again, deletes all files, then requires zero data-fragment records and zero allocated
container bytes on every root within 120 seconds. Delete rate measures namespace
unlink operations; physical GC convergence is a separate gate. Metadata fragments
continue to use buffered files under both engine choices.

The full native `make test` suite gates the live matrix. Warm small-file results
measure this client/server workload, not raw device bandwidth, cold-cache behavior,
or power-loss durability. No tracing overhead is included in the timed matrix.

## Startup finding

After two fully passing cases, the first containers/buffered startup returned
`raft-mkfs rc=-15` (`EFS_ERR_NOT_PRIMARY`). All four nodes were listed, but at least one metadata Raft group was not ready
to accept the proposal. The reply does not identify which group lacked a primary. This was a test-runner
readiness gap; no engine or daemon implementation was changed. The failed
attempt and its roots are retained separately. The runner now waits for known
leaders on all hosted groups and exactly one leader in each of groups 0 and 2.
It retries only explicit election/BUSY replies during fresh private initialization;
MKFS preserves the already committed export salt on retries (W37).

Resume also handles closed TCP connections in TIME_WAIT without confusing them
with listeners. Completed cases are retained, installed process identities and
configuration must match, and existing failed roots must be preserved under new
names before retrying. The six remaining cases use the improved runner; all
cases use the identical daemon/client binaries.

For the first container repeat in each I/O mode, `/proc/PID/fdinfo` inspection
also confirms the segment descriptors' `O_DIRECT` flag on all four daemons:
set for direct, clear for buffered. Argument verification runs for every case
at both startup and restart.

## Results

**All eight cases pass**: checksums before/after restart, acknowledged overwrite
recovery, deletion, and physical GC to zero on all four roots. All 64 daemon
launches (four nodes × start/restart × eight cases) match the recorded engine,
I/O mode, ports, roots, quota, writers, Raft cardinality and transport. The full
native test suite passes with zero compiler warnings. Final binary hashes still
match the pre-matrix hashes. Installed daemon/client PIDs and command lines are
unchanged, and no private test listeners or mounts remain.

| Engine | I/O | Read MiB/s | Durable overwrite MiB/s | Delete files/s | Write p99 ms |
|---|---|---:|---:|---:|---:|
| files | buffered | 6.17–216.23 | 1.19–4.72 | 13.61–401.25 | 357.81–649.40 |
| files | direct | 6.25–120.34 | 1.28–4.00 | 14.82–829.61 | 404.43–507.43 |
| containers | buffered | 5.68–6.21 | 1.19–1.20 | 17.49–17.68 | 603.88–606.04 |
| containers | direct | 6.05–142.28 | 1.00–3.20 | 17.64–752.63 | 522.87–710.82 |

Ranges are min–max of two repeats, not confidence intervals.

These are complete client-operation rates. Durable overwrite uses 128 KiB
writes plus `fsync`; read verifies 512 KiB files; delete counts namespace unlinks.
The two-sample median would conceal the very large performance split and is not
a useful engine-selection statistic here.

### Metadata placement confounds the comparison

Every case creates the same 256 files under one `engine-ab` directory in a fresh
export. Native session logs record all 256 distinct file inode IDs per case.
All are even in each slow case and odd in each fast case. The
[inode-shard mapping](../../include/efs/kv_key.h) keeps the inode's low bits, and
the [Raft group mapping](../../include/efs/raft.h) sends odd shards to group 0
and even shards to group 2. The reproducible analysis reads those log entries;
it does not infer placement from throughput.

| Case | File inode metadata group | Read MiB/s | Durable overwrite MiB/s | Delete files/s |
|---|---:|---:|---:|---:|
| files-buffered-r1 | 2 | 6.17 | 1.19 | 13.61 |
| containers-direct-r1 | 2 | 6.05 | 1.00 | 17.64 |
| containers-buffered-r1 | 2 | 6.21 | 1.20 | 17.49 |
| files-direct-r1 | 2 | 6.25 | 1.28 | 14.82 |
| files-direct-r2 | 0 | 120.34 | 4.00 | 829.61 |
| containers-buffered-r2 | 2 | 5.68 | 1.19 | 17.68 |
| containers-direct-r2 | 0 | 142.28 | 3.20 | 752.63 |
| files-buffered-r2 | 0 | 216.23 | 4.72 | 401.25 |

Thus this matrix observes **group-2 file inodes: 5.68–6.25 MiB/s reads**, versus
**group-0 file inodes: 120.34–216.23 MiB/s**. Slow runs also show rate-limited
`getchunks slow` entries on node 2 with ReadIndex waits; fast runs have no such
entries in their initial daemon logs. The common metadata/routing path is a
strong investigation target. This is correlation across eight cases, not a
causally isolated diagnosis or proof that storage engines have equal performance.

Initial leader placement alone does not explain it: fast containers/direct and
files/buffered runs start with leaders split between nodes 1 and 2, as do slow
runs. The earlier files/direct pair also differs in leadership, so attributing
its speed change solely to co-located leaders would be incorrect.

The first pass supplies all four engine/I/O combinations on group 2; their
reads are similarly slow, buffered durable writes are similar, and direct
containers do not beat direct files on durable writes. The group-0 subset lacks
containers/buffered. Do not compare that row's group-2-only rates against the
other rows' fast group-0 samples as evidence of a buffered-container regression.

**Recommendation:** retain `files` as the default. Before another engine ranking,
control and record test-file metadata placement, balance both groups across
all four combinations, and profile the group-2 forwarding/ReadIndex path against
group 0 with the same client and payload. Keep required checksum and durability
operations enabled. No daemon optimization or engine-module refactor was made
in this NUC round. Power-loss durability, cold-cache saturation, multi-host
network behavior and multi-target-per-daemon acceptance remain outside this run.

Raw evidence and its checker are in the
[NUC evidence directory](../../results/measure/20261008-nuc-engine-ab/README.md).
Build/test and daemon logs are compressed; binaries and storage contents are
excluded. Initial startup failures are retained, rather than counted as passing
workload cases. P4.1 remains open.

The [metadata-group follow-up](metadata-group-perf-followup-20261008.md) records
the group definitions, Xorinox comparison and next balanced investigation under
P4.1 / W66. It remains open; this checkpoint does not establish its root cause.

## Reproduction

Deploy this branch into a **fresh** private base, build natively, run the test suite,
then run as the unprivileged `efs` user on `nuc_efs`:

```sh
python3 /data1/efs/bench/NEW_BASE/src/scripts/ab_storage_engines_nuc.py \
  --base /data1/efs/bench/NEW_BASE --seconds 30 --repeat 2
```

The script expects `efsd`, `efs-fuse` and `efs-mgmt` in that base's `src` directory,
plus the tracked `tests/live/storage_engine_{ab,inventory}.py` helpers. It creates
matching NVMe roots under `/data2/efs/bench/NEW_BASE`. It refuses to reuse an existing
results directory, mount directory or case root. Keep binary/source hashes and the
build/test logs with the results. No installed NUC launch scripts are invoked.
