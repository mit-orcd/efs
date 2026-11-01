# ORCD fresh-store read/close baseline — October 16, 2026

Scope: W119 timeout attribution, continuing the
[AMD/NUC investigation](amd-async-durability-20261015.md). User requested a
controlled comparison, then specified **orcd-test with RDMA enabled**. ORCD
replaces AMD for this run. RDMA baseline is common to either interpretation;
the TCP leg awaits clarification whether a comparison or RDMA-only testing
is intended. No transport attribution or complete A/B verdict is claimed.

## Frozen source and isolation

Source `852caf1ae5c96482c5439c2ffb12968a8e70ddde`, branch
`devel-amd-durability-20261015`, identical product code to the passing NUC
TCP baseline. Later local commits are documentation only. Source was archived
with `git archive --format=tar.gz 852caf1ae Makefile src include deps scripts tests`
and built separately on fcstor003–007 at `/tmp/efs-transport-852caf1ae`.
Each build uses `make -j8 EFS_GIT_ID=852caf1ae EFS_GIT_DESCRIBE=852caf1ae
EFS_GIT_BRANCH=devel-amd-durability-20261015 efsd efs-fuse efs-mgmt`, with the
existing private fuse3 pkg-config prefix and runtime rpath under
`/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse/lib`.

Access is through the existing authenticated ORCD login control connection,
then `ssh -o BatchMode=yes fcstorNNN`; `orcd-test` itself is a cluster name,
not a resolvable SSH host. Use `~/git/cluster/clusters/orcd-test.env` for the
node/address map. All services run as erbmi1 without sudo.

Exclusivity preflight found six live `tiny_files_md5.py` crawlers on
fcstor007–012. Each received SIGINT; outputs were retained. All seven old
client mounts drained/unmounted normally. The four old daemons stopped with
SIGTERM and exited without force. Their six-root stores were preserved.
Subsequent all-eleven-host process inventory found no EFS workloads/builds;
remaining old screen sessions held idle shells, not active writers/crawlers.
No other agents were messaged or scheduled. AMD and NUC were untouched.

## Test configuration

Four physical storage hosts fcstor003–006, addresses172.16.223.57–60,
port17432, one fresh local root per host per round:
`/data1/01/efs-transport-852caf1ae-erbmi1-rdma-N`. Each starts with zero usage,
files storage,17G quota, direct I/O. Two mounts on fcstor007 use
`/tmp/efs-transport-rdma-N-mnt1` and `mnt2`. This is not a seven-client run.
The existing six-root namespace is not used for these tests.

Environment for daemons, management and clients:

```sh
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_DIRECT_IO=1
EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0
EFS_REPAIR_EXPERIMENT=0 EFS_MD_RAFT_N=4 EFS_METADATA_BALANCE=1
EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20
```

mlx5_2 is active native InfiniBand, unlike AMD Soft-RoCE. Runtime inspection
of all six processes verifies the explicit environment and native uverbs
file descriptors. No profiling variables, extra barriers, timeout overrides
or `.efs` features were enabled. One client/jobs16
preserves the workload concurrency of the preceding AMD/NUC core runs.
Hardware, topology and retained-store state still differ from historical AMD;
this is not a controlled AMD-versus-ORCD transport experiment.

## Reproduction

After verifying exclusivity, build the frozen source on each participating
host as above. For each round choose a **new** root suffix; require its path
not to exist. Start node IDs1–4 with their configured address, port17432,
`--storage ROOT --quota 17G --direct-io`; nodes2–4
also use `--join 172.16.223.57:17432`. Export the environment above. Record
PIDs and logs under the evidence directory, wait for listening, then run
`efs-mgmt raft-mkfs 172.16.223.57:17432`. On fcstor007 mount twice using
`scripts/client.sh 172.16.223.57:17432 MOUNT`, with distinct EFS_FUSE_LOG paths.
From the staged source on fcstor007:

```sh
python3 -u tests/posix/posix_suite.py /tmp/efs-transport-rdma-N-mnt1 --suite core --jobs 16 --tag orcd-rdma-baseline --results NEW_CORE.tsv
python3 -u tests/posix/posix_2client.py --local /tmp/efs-transport-rdma-N-mnt1 /tmp/efs-transport-rdma-N-mnt2 --parent orcd-rdma-peer --results NEW_PEER.tsv
```

Keep default case deadlines, including15s names/deep cases and existing
longer overrides. For each new-store round, normally stop both mounts and
verify they are detached before SIGTERM to the four exact recorded daemon
PIDs (check executable identity first). Wait for exit; abort on drain/stop
failure instead of force-discard or kill9. Never remove the original stores.

Raw build/run output stays outside Git under
`/home/erbmi1/orcd/scratch/efs/orcd-test/transport-852caf1ae/`, with per-round
`rdma-N/fcstorNNN` logs, result TSVs and exit markers. Old crawler outputs
remain in their original directories. Only summaries/instructions are committed.

## Initial results

Round1 core **PASS216/216,11.3s**, no skip/notrun, exit0. Round1 local two-mount
peer **PASS67/67,6.8s**, exit0. These results do not qualify cross-host peer
behavior. No code changes were made. W119's earlier failing AMD configuration
remains open; native RDMA passing here does not isolate Soft-RoCE, accumulated
metadata, hardware/storage latency or competing activity as its cause.

## Binary identity

Build time differs between build batches; matching source does not imply
identical executable hashes across those batches.

| Hosts | efsd SHA256 | efs-fuse SHA256 |
| --- | --- | --- |
| fcstor003–005 | `31da540bc9b1680ca7c15c6a8a65a89e34a5bfc00d26817531725a83c614b48b` | `d23a1686cfdf5531abfb5908769bc7ab05a7167c2892de26fd523afd6d135b7c` |
| fcstor006–007 | `749cbb47249751bcd133136567081b0eac63c404e1ded521235cb0dd1b4676bf` | `44bf47be3e5d1548d20cd2b4a49b804895958f583c9c5a86c021423614193eb5` |


## Repeated fresh-store outcome

| Round | Transport | Core jobs16 | Duration |
| --- | --- | --- | --- |
| 1 | Native RDMA/direct | PASS216/216 | 11.3s |
| 2 | Native RDMA/direct | PASS216/216 | 11.0s |
| 3 | Native RDMA/direct | PASS216/216 | 11.1s |

All have zero skips/notrun and exit0. Each used a new empty root per server,
unchanged binaries, quota, features and deadlines. Round1 local peer passed
67/67; peer was not repeated in rounds2/3. All transitions drained both clients
and stopped the four daemons normally. Final round3 daemons and two mounts are
left running with RDMA/direct and disabled features. Old production-shaped
stores are preserved but their old services/crawlers remain stopped.

**TCP leg NOTRUN, pending the transport-scope clarification.** These three
passing RDMA runs are not a completed alternating transport comparison. They
show the timeout is not universal to this code on RDMA; they do not prove the
AMD failure was caused by Soft-RoCE, hardware, competing activity or metadata
history. No optimization was made or justified by a failing ORCD profile.
No push, tag, branch merge or seven-client test.

Post-run verification: all951 archived source inputs match the frozen archive
on each of fcstor003–007. Architecture documentation gate5/5 PASS.
