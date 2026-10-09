# AMD RXE W73/W64 gate evidence

Run October 8, 2026 New York / October 9 UTC. Source commit
`f6233987b9d1243d5dc95167e9aaabc77e32b44d`, branch `devel-gate-closure`, build ID
`f6233987b9d1`. [Status report](../../../docs/status/amd-rxe-gate-closure-20261009.md).

| Mode | Core POSIX | Peer | Mount sync | Exact cold append/GC |
| --- | --- | --- | --- | --- |
| Direct RXE | 212/212, 19.0 s | 64/64, 14.4 s | PASS | 800 records; PASS |
| Buffered RXE | 212/212, 18.6 s | 64/64, 14.2 s | PASS | 800 records; PASS |

- `direct-full/`, `buffered-full/`: full gate logs, exact fixture paths,
  source/binary manifests, sync/fault/recovery/replay verdicts, core and peer
  TSVs, cold append audit, final GC status and verbs runtime snapshots.
- `direct-mount-sync/`, `buffered-mount-sync/`: actual sync mount flag,
  ordinary and append peer-before-close writes, cold crash bytes and deletion.
- `runner/`: exact matrix command, build and non-skipped transport probe,
  restoration and zero runner exit. A wrong-target build invocation is retained
  separately; its corrected build succeeded before live acceptance.
- `deployment/`: exact binaries moved into installed source path, original
  installation backup path, graceful deployment/start logs, all six processes'
  RDMA/device/GID configuration and verbs handles, all five versions/hashes,
  installed synchronous peer/truncate-append smoke and optional suite TSV/log.
- `source-identity.json`: local committed production-source hash, verified
  against every gate manifest and installed source; all three gate binary
  hashes match the installed exact binaries.

The core suite excludes optional mmap/.efs checks in this integrated revision.
The installed optional suite has 1 pass / 0 fail / 4 skips: shared mmap
unsupported and commands explicitly disabled. No core tests were skipped.
Each full RXE fixture records fourteen verbs snapshots; each mount-sync fixture
three. Transport-up excerpts are retained; full daemon/FUSE logs and private
stores remain on AMD. Paths in gate commands/manifests name the predeployment
candidate directory, subsequently moved to `/data1/efs/src` without rebuilding.

This is software RXE on one AMD host. W72 production session fencing/unknown
outcomes and W71 hardware power-loss remain separate open acceptance work.
