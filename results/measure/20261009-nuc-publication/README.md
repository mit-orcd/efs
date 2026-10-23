# NUC publication and bounded cleanup evidence

Private source and stores: `/data1/efs/evidence/agent4-publication-20261008` on `nuc_efs`.
Baseline `863054ca` plus the bounded sweep change and deterministic manifest-test
corruption fix. Build uses `EFS_FAULTS=1 EXTRA_DEFS=-Werror`; test policy is
`EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=1 EFS_TRANSPORT=tcp`.

- `build-integrated.log`, `full-tests-integrated-2.log`: merged baseline build and
  complete passing suite. The earlier `full-tests-integrated.log` retains the
  manifest test's random-byte no-op failure, fixed by XORing the persisted byte.
- `client-session-fenced.log`: real production helper, committed/unapplied receipt,
  four-daemon crash, exact locator, retirement floor and fenced epoch refusal.
- `replay-same.log`, `replay-cross.log` and matching JSON directories: initial real
  immutable-byte, physical-GC, history-eviction, old re-PUT and cold FUSE gates.
- `build-bounded-sweep.log`, `bounded-sweep.log`: strict build and metadata suite,
  including eight exact keys with eight deltas each, failure and duplicate retry.
- `bounded-sweep-cold-2.log`: ungraceful LSM WAL recovery and exact-key retry PASS.
  The earlier cold log retains a fixture assertion corrected to inspect an explicit
  FileID key: ordinary get_chunk cannot resolve an already unlinked inode.
- `replay-bounded-same.log`, `replay-bounded-cross.log` and matching JSON directories:
  repeated real-byte/cold gates with bounded cleanup. Their manifests record binary
  hashes; these precede the final preservation of the legacy recovery allowlist.
- `full-tests-bounded.log`: first suite after bounded cleanup stopped because the
  extracted GC decoder harness needed a stub for the new function. The harness now
  exercises the bounded command and its malformed lengths/verdicts explicitly.
- `full-tests-bounded-2.log`: complete integrated suite PASS after that harness update.
- `final-sha256.txt`: final tested source and binary identities.
- `build-bounded-final.log`: final server build and GC decoder regression after
  preserving legacy recovery admission for old logs and remote selection.
- `restore-bounded-mnt2.log`, `restored-bounded-mnt2.stat`: installed mount restoration.

Installed four daemons and both mounts remain owned by `efs`. Installed binaries,
stores and pressure policy are unchanged. No package closure or default activation
is claimed; see the [status note](../../../docs/status/publication-nuc-20261009.md).

**Raw evidence note (2026-10-23):** the bulk data in `agent4-publication-20261008` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
