# D25 regular-file mtime coherence — Oct 6 2026

Code commit: `a43745b3`.

Regular-file MTIME SETATTR now prepares the inode first, then exact lane images,
with one durable transaction decision. The inode hold freezes the active-lane
set; lane holds block publications until resolution. Each lane clears only its
old mtime stamp, increments its sequence, and adopts the new mtime generation.
Size, ctime, block counts, append state and content history survive. A missing
active lane stamp is installed at the captured content epoch. Exact CAS conflicts
abort safely; an ambiguous COMMIT is never converted to ABORT.

Cold lane bootstrap now captures/validates and installs the inode mtime generation.
A legacy REPORT carrying an older generation no longer revives invalidated mtime.
Durable publication uses the serialized lane-local generation, without an inode
RPC on the ordinary publication path. Directory and atime-only handling retain
existing behavior. D25 typed FUSE publication/flush integration is still staged.

## Validation

- NUC Linux full build and full unit suite: PASS (`unit.log`).
- Publication/mtime races and durable recovery: PASS (`mtime-recovery.log`).
  Covers backwards mtime, bootstrap exclusion, stale legacy timestamps,
  incomplete plans, exact publication conflicts, lost COMMIT acknowledgement,
  and PREPARE/COMMIT/ABORT restart including partial shard resolution.
- Four-node normal deployment/start/smoke: PASS (`rollout.log`).
- POSIX: 216 pass / 0 fail / 1 unsupported mmap skip.
- Live multi-lane sparse-file gate: PASS (`live.log`): server mtime changes
  100 → 50 → 150 → 25 seconds preserve size and bytes; a subsequent fsynced
  write advances mtime. Only the test's disposable file is removed.
- Architecture documentation gate: all five checks PASS.

The rollout identifies `a43745b3` on all four servers. Its build ID has a dirty
suffix because this untracked evidence directory was being collected during
source sync; the deployed checkout had no tracked code modifications.

Next: automatic lane session admission and immutable publication/ACK ownership
through both FUSE flush paths, plus bounded abandoned-session receipt cleanup
and remaining live activation gates. D25 is not fully activated by this change.
