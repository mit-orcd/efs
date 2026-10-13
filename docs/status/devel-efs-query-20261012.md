# devel-efs ordinary-file command qualification — October 12, 2026

Purpose: W91/W92/W108/W109 command qualification on ORCD, with expiry deferred.
Current verdict: **PASS** for the ordinary-file ORCD functional and concurrent
command gates. The original failing baseline remains documented below. Broader
large-namespace/platform qualification remains NOTRUN.

Final tested source: `0dd2a7c7912b4fec4b2ee6f6237075f2a7d3e5b5`, based on
`release-v0.3.2-pre-alpha` (`3a414935`). All eleven hosts build this snapshot;
778 source/test/build-input hashes per host match its archive. Binary SHA-256
identities and runtime process flags are retained externally with the run.
Normal native, separate fault native, nine ASan/UBSan production-body extractors,
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

The output directory must be new, and the options harness requires its fixed
`/tmp/efs-mount/efs-options-gates` fixture to be absent. If repeating, remove
only that recorded test-owned fixture after its previous run has stopped.
The runner retains six named logs and a
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
error/deadline tests. Candidate deployment, three quiet repetitions, enabled cold query and
owner-name rollout checks now pass. The 100 ms limit stops starting another
retry; an already-running proof remains bounded by the original build deadline.

Raw evidence remains outside Git: local
`/Users/mike/Documents/Codex/2026-10-08/efs/operations/devel-efs/` and remote
`/home/erbmi1/orcd/scratch/efs/orcd-test/{fcstor003..fcstor013}/` with tag
`a54b42f3c3d7` (focused, mixed, diag1 and trace1). The source-controlled
reproduction above does not depend on those captures.

## Final-source results

All eleven hosts execute the candidate with query-stage tracing/work diagnostics
off. Installed input hashes match the 778 archived inputs on every host; running
binary hashes match each host's staged build. Host-local builds have distinct
binary identities; they are not claimed byte-identical across every machine.
Server SHA-256: fcstor003–005
`e77a30e7d4d36b4a3a389f62f5b606d22df762253e21392c694cdb9c7fde5d01`,
fcstor006 `0426c1b8c7732376509d024f332e69a7571cb39ac99d47817010dee261430626`.
Client SHA-256: fcstor007–008
`63a3973061f68b3af15c2ab0653a77328ac31bd288ae7f4a83c9c267e89cddde`,
fcstor009–011 `af9e058df1079220e3158fbb0df01f6cfef28cf0688a0ee4d59c1d0b65aab0a7`,
fcstor012–013 `5496b9bb133bb9c3883c72fab816a7080206f9a90dd6544fd2d2d37ed0a988c7`.

- Full normal/fault native checks, early-ACK Raft, nine ASan/UBSan extractors,
  and fully instrumented Raft checks PASS. The tenth-cut regression fails
  against frozen baseline source and passes against the candidate.
- Six fresh focused suites PASS: commands/options/pages/virtual/load/large
  delta, in 10.68/25.49/6.32/55.83/26.55/11.08 seconds. Original deadlines and
  assertions remain. An earlier repeat failed options setup with EEXIST due
  to its retained baseline fixture; cleanup of that known fixture and a fresh
  full repeat pass. That setup failure remains in external evidence.
- Three quiet mixed rounds PASS: 4,536/4,536 ordinary POSIX cases plus
  63/63 concurrent command cases. Each round uses all seven physical clients,
  core jobs16 and query jobs3, with unchanged 30-second case deadlines.
- Physical-peer cases PASS 67/67 (86.6 seconds).
- W108 rebuilt embedded ls/find long format resolves `erbmi1:erbmi1` on all
  seven clients both before and after restart. Numeric mode retains
  `246111:246111`; JSON remains numeric. All 29 native formatter cases PASS.
- Durable RDMA command policy masks0/1/2/3, stale-CAS rejection and
  ordinary writes while disabled/catchup PASS. Mask3/version4 survives restart;
  two fsynced files/19 bytes and their exact payloads pass on all seven clients.
- Seven client drains/unmounts and four orderly server stops PASS, without
  force/lazy unmount. Three acknowledged marker payload hashes survive and
  enabled cold stats/find return exact counts, logical bytes and paths on all
  seven clients. Ordinary fragment/checksum objects then disappear across
  all 24 roots after only those marker files are unlinked (0.72 seconds).
- Real capacity fixture: 4,096 fsynced ordinary files, 16 directories,
  256 KiB each (1 GiB), eight workers. Write PASS 11.61 seconds. After the
  full restart, fcstor008 verifies every payload and every complete paged
  query path, without omissions/duplicates: PASS 5.39 seconds, refresh_error0.
- Cold whole-export stats PASS on all seven clients: 54,418 files,
  13,192 directories, 2,411,055,624 logical bytes at this checkpoint.

For the real cold fixture, use a fresh named root/output directory:

```sh
python3 tests/live/efs_query_capacity.py write /tmp/efs-mount NEW-root /outside/repository/NEW-output --files 4096 --bytes 262144 --workers 8
# Orderly stop every client, stop/restart servers using the same profile,
# remount clients, then on a different physical client:
python3 tests/live/efs_query_capacity.py verify /tmp/efs-mount NEW-root /outside/repository/NEW-output --files 4096 --bytes 262144 --workers 8
```

Retain the harness manifest across restart; do not remove the fixture before
verification. After both phases finish, remove only that named fixture if desired.
For owner checks run `<fixture>/.efs/ls -l`, `-n`, `--json` and
`<fixture>/.efs/find proof --match exact` in those same modes; assert names,
IDs and exact paths. For policy use `EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2
./efs-mgmt commands-policy 172.16.223.58:17432 get`, and `set MASK VERSION`
with its returned version for CAS. Verify each bit, rejected stale CAS,
fsynced disabled writes and post-enable/cold catchup; always restore mask3.

Final raw evidence uses tag `0dd2a7c7912b` in the same external locations above;
local `focused-fresh.log`, `concurrent-candidate.log`, `repeat1.log`, `repeat2.log`,
`restart-final.log`, `capacity-verify.log`, `cold-policy.log`, runtime/source
manifests and prerequisite logs retain the complete run. These are functional
ordinary-file results: no packing/container/expiry implementation, unplanned
recovery, NFS platform, 400 GiB or billion-entry qualification is asserted.
W108 rollout and the reproduced ORCD W109 gate are satisfied; broader W91/W92,
W97/W98/W110 acceptance retains its independently documented scope.
