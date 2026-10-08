# W58/W55 Xorinox evidence, Oct 8

Branch devel-known-bugs from 3e1d4c6e. Isolated four-node files/direct/TCP fixtures
inside xefs1; installed three-VM services were preserved, not redeployed.

- `combined/`: manifest, lost-reply identity record, PUT-loss/collision record,
  POSIX result tables and other compact enclosing GC evidence.
- `unique/`: manifest and 1,024-file/16-worker cold acceptance record.
- `barrier/`: manifest and actual server fragment-fsync refusal/recovery record.
- Corresponding `live.log`, `unique-final.log`, `barrier.log`: complete harness
  phase results. `final-unit.log` records final focused regressions.
- `unique.log` and `unique-rerun.log`: excluded harness-placement/readiness
  failures. First phase accidentally ran before FUSE launch, creating ordinary
  local files; corrected phase runs after confirmed FUSE readiness in fresh paths.
  These failures are not W58 or EFS data-loss reproductions.

Remote raw logs, binaries, fixtures and source remain beneath
`/data1/efs-known-bugs` on xefs1. Compact JSON manifests record actual binary
hashes. Preload fsync failures are test-only and attached only to owned servers.
No power-loss or independent host-failure claim follows from process crashes.

Reproduction (create empty, unmounted mount paths and two existing root parents):

```sh
make -j2 EFS_FAULTS=1 all
python3 tests/live/gc_reclamation.py --mode direct \
  --root ROOT1 --root ROOT2 --mount MOUNT --peer-mount PEER --port 28320 \
  --opid --opid-restart --create-lease --put-reply-fault \
  --client-binary "$PWD/efs-fuse" --report-audit --posix
python3 tests/live/gc_reclamation.py --mode direct \
  --root ROOT1 --root ROOT2 --mount MOUNT --peer-mount PEER --port 28320 \
  --unique-create
python3 tests/live/gc_reclamation.py --mode direct \
  --root ROOT1 --root ROOT2 --mount MOUNT --peer-mount PEER --port 28330 \
  --put-barrier-fault
make test-create-errors
```

Use an isolated build tree; do not reuse stale objects when toggling EFS_FAULTS.
Actual remote build used explicit EFS_GIT_ID=3e1d4c6e-known-bugs and
EFS_GIT_BRANCH=devel-known-bugs because rsync omitted Git metadata.

The macOS installed-NFS baseline did not pass: plain cp failed extended-attribute
copying with EPERM, then the export was no longer mounted and its local root-owned
mountpoint refused mkdir. No completed 20/20 cp gate or branch NFS acceptance is
claimed. See ../../../docs/status/known-bugs-validation-20261008.md for scope and
remaining original-incident gates.
