# devel-efs ordinary-file command qualification — October 12, 2026

Purpose: W91/W92/W108/W109 command qualification on ORCD, with expiry deferred.
Current verdict: **FAIL** for concurrent query qualification. Focused commands
pass; the retry candidate below is not yet deployed or qualified.

Tested source: `a54b42f3c3d7325977917ff384cb681f80e6a959`, based on
`release-v0.3.2-pre-alpha` (`3a414935`). All eleven hosts build this snapshot;
778 source/test/build-input hashes per host match its archive. Binary SHA-256
identities and runtime process flags are retained externally with the run.
Normal native, separate fault native, eight ASan/UBSan host/read/GC extractors,
and native/instrumented Raft early-ACK checks pass. No fault fixture replaces
normal running binaries. These prerequisites do not activate failure recovery.

Environment: ORCD physical storage fcstor003–006 (four metadata/storage nodes),
six direct-I/O roots per server, seven native FUSE clients fcstor007–013 at
`/tmp/efs-mount`. Native RDMA uses `mlx5_2`; processes run as site account
`erbmi1` (UID 246111). Servers explicitly select `--storage-engine files
--direct-io`, `EFS_COMMANDS_SERVER=1`, `EFS_PACKED_WRITES=0`; clients use
`EFS_COMMANDS=1`, `EFS_PACKED_WRITES=0`. Containers and packing are excluded.
The existing ordinary-file export is preserved; no new export/quota is created.
Default query disk budgets remain 16 GiB per generation / 32 GiB total,
with the default reserve. Actual catalog/namespace size varies with fixture
mutation; this is not a billion-entry or 400 GiB qualification.

Reproduction: install/build the exact snapshot with ORCD's libfuse pkg-config
and runtime paths, start the above profile, then create a second independent
RDMA FUSE mount on fcstor007. Run:

```sh
python3 scripts/orcd_efs_commands.py /tmp/efs-mount /tmp/efs-command-peer \
  --query-host 172.16.223.58 --query-port 17432 \
  --output /home/erbmi1/orcd/scratch/efs/orcd-test/fcstor007/NEW-command-results
```

The output directory must be new. The runner retains six named logs and a
summary; each underlying command uses its existing timeout/assertions.
It leaves dedicated fixture directories for cold oracles. Orderly stop the
extra mount with `bash scripts/client.sh stop /tmp/efs-command-peer`.
Do not recursively remove the mounted export; clean only recorded test roots.

For the loaded mixed gate, simultaneously on all seven clients run:

```sh
python3 tests/posix/posix_suite.py /tmp/efs-mount --suite core \
  --jobs 16 --timeout-s 30 --results /outside/repository/core.tsv &
python3 tests/posix/posix_suite.py /tmp/efs-mount --suite extras \
  --filter virt_ --jobs 3 --timeout-s 30 --results /outside/repository/query.tsv &
wait
```

Capture both exit statuses independently; `wait` alone is not an overall
success oracle. Physical-peer qualification uses the existing
`tests/posix/posix_2client.py --remote fcstor007 fcstor008` harness with forwarded
SSH credentials and a prepared fresh parent. Background builds/profiling must
finish before quiet acceptance. Expiry tests cover unavailable/refusal behavior
only; no deadline implementation or deletion is included.

Results on a54b42f3:

- Six focused suites PASS: commands, options, pages, virtual interfaces,
  loaded publication and large delta. Their wall times are 11.53, 22.84,
  6.82, 58.52, 23.69 and 10.98 seconds respectively.
- Virtual interface: all ten cases PASS. Loaded publication converges jointly
  to 7,979 files / 2,042,624 logical bytes and verifies every payload on the
  independent mounted client. The 1,025-file payload delta passes create,
  update, delete, rename and hard-link query/peer oracles.
- Seven-client mixed gate: 1,512/1,512 core PASS; query extras 7/21 PASS,
  14 FAIL. Each client retains the 30-second stats timeout and terminated
  find worker. This is a failed query gate even though core/peer pass.
- Physical peer: 67/67 PASS, 90.8 seconds.
- Orderly redeployment and a later diagnostic restart both drain/unmount
  normally and preserve acknowledged small/large marker bytes on all clients.
- A stack-dump reproduction retains query failure. A subsequent stage-traced
  run passes 21/21 query extras and core cases; it does not supersede the quiet
  failures or establish a repair.

Diagnosis: client stacks include query LOOKUP in `query_snapshot_rpc` retry
pause. Server stages show transaction-cut invalidation during fresh proof;
new query roots remain absent from the published catalog. The traced run also
records a pending-intent build refusal and successful multi-second incremental
builds. This attributes the retry/publication path, not a mutex deadlock or
permanent RDMA message loss.

Candidate: permit up to sixteen independently fenced pins in a burst capped
at 100 ms before starting another attempt, also respecting the original build
deadline. Failed pins are released; no authority/import/transaction checks are
removed. The production-body regression accepts a coherent tenth cut following
nine collisions (old source fails), bounds continuous collisions and expensive
captures, and retains the mixed A-only/B-delayed rejection and import/quorum/
error/deadline tests. Candidate deployment, quiet repetitions, enabled cold
query and owner-name rollout checks remain pending.

Raw evidence remains outside Git: local
`/Users/mike/Documents/Codex/2026-10-08/efs/operations/devel-efs/` and remote
`/home/erbmi1/orcd/scratch/efs/orcd-test/{fcstor003..fcstor013}/` with tag
`a54b42f3c3d7` (focused, mixed, diag1 and trace1). The source-controlled
reproduction above does not depend on those captures.
