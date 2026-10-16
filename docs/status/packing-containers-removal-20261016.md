# Packing and container engine removed from devel — 2026-10-16

**Decision [D37](decisions.md); removal PASS (build and suite), deployment
NOTRUN.** At the user's request, all packing code and the experimental container
storage engine are removed from `devel`, so they can be resumed later without
weighing on current work.

## Where the code is

- Tag **`archive/packing-containers-20261016`** and branch **`packing-containers`**
  point at `6c274f47f`, the last devel commit with both features, their tests,
  scripts and docs.
- The removal is the single commit `5ded11179`. To resume: branch from
  current `devel` and `git revert 5ded11179` (or cherry-pick from the
  archive), resolve against whatever changed since, then re-qualify. Do not develop on the
  archive branch in parallel with `devel`; it would drift.

## What was removed

- **Client:** `src/client/pack_write.c` (`EFS_PACKED_WRITES`), the packed read path
  (`packed_reader`), packed chunk-cache references, the publication-batch RPC,
  and the logic acting on the legacy EFSM v5 `pack_ino`/`pack_off`/`pack_len`
  inode fields. Those fields stay in the struct and wire layouts as reserved
  zeros.
- **Common/metadata:** `packed_segment`, `packed_reader`, `pack_cohort`,
  `pack_ledger`, `pack_mapping`, `pack_admission`; the PACK coding profile, the
  packed chunk extension and `packed_retirement` in metadata apply (truncate,
  lane fence, sweep and fold now use plain batches); packed publications.
- **Server:** `packed_upload.c`, `packed_fence.h`, `packed_gc.h`,
  `pack_authority.h`; the packed PUT/GET/GETCHUNKS/GC, capability, authority
  and publication-batch messages; packed GC passes; packed storage objects.
- **Container engine:** `store_containers.c`; the server `storage_engine` switch
  and every container-only path. That covers parallel reap fences, batched GC
  records, empty-lane GC, the inbox read fold, deferred held flush, 32
  read-pair helpers, lock admission status and the cleanup fsync cohort.
- **Kept:** the files-engine parts of the old module (root locks, engine marker,
  pinned-ancestry directory barriers, metadata-path detection) now live in
  `src/server/storage_roots.c`.
- **Tests and scripts:** 46 packing/container test files, two stress tests, one
  live diagnostic and three container A/B/qualification scripts. Historical
  harnesses that set `EFS_PACKED_WRITES=0` remain; the variable is now inert.

## Fail-closed on old data

Nothing is silently skipped or reinterpreted:

| Old state | Behaviour now |
| --- | --- |
| KV MANIFEST `KVLSM2 … 1` (packing reader requirement, set before any packed row is written) | open refuses: `kv-lsm: … requires reader capability 1 (packing)` |
| Root formatted by the container engine (`.efs-engine` byte 8 = 1) | startup refuses: `storage-engine: … formatted by the removed container engine` |
| `--storage-engine containers` | refused at argument parsing |
| Chunk row with the retired PACK profile | `unpack_chunk` → `EFS_ERR_PROTO` |
| Snapshot import carrying packed kinds/rows | import refused |
| Retired Raft packing commands (45–50) in a log | apply halts with `raft-host: packing command …`; never skipped |
| Publication with the retired packed flag | invalid command |
| Retired wire messages 131–144, KV kinds 36–39, chunk flag 8 | numbers reserved, never reused |

orcd-mini (node9901 n1–n3) was checked before this change: `KVLSM1`
manifests, engine marker 0, no `.efs-containers`. It opens unchanged.

## Verification

- Build: `make all tests` on node9901 (RHEL 9.7, gcc, private libfuse 3.16.2):
  efsd, efs-fuse, efs-mgmt, efs-query and efs-bench without new warnings.
- Full `make test` (1,219 commands after removing 127 packing/container
  commands; `ASAN_OPTIONS=detect_leaks=0` because ORCD sets
  `ptrace_scope=2`).
- `tests/test_kv_reader_gate` now checks that `KVLSM2`/packed and malformed
  MANIFESTs are refused, and that the plain store reopens and stays `KVLSM1`.

Result: **1,219 commands, 1 failure (413 s)**. The failure is
`docs/check-architecture.py` on the uploaded tree, which omits `results/`;
it passes 5/5 on the full checkout. Twenty-two tests that compiled source
slices or stubs for removed fields/messages were updated: their packing or
container branches were removed and the files-only assertions kept. The
retained `test_container_*` tests (`pending_reads`, `read_admission`,
`read_probe`, `ack_window`) cover Raft-host paths that run on every engine
and pass.

## Limitations

- Mixed-version clusters are not supported: the build-ID HELLO gate already
  requires daemons and clients from one build.
- Not deployed anywhere yet. orcd-mini still runs `4d6530dd8`; the RDMA fix
  release `bbebedea8` is staged separately.
- Historical status pages still describe packing and containers as they were;
  they are evidence, not current behaviour.
