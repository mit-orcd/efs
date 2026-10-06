# Ten-round continuation — NUC, Oct 6 2026

Requested: ten implementation rounds, commit logical portions, stop for a required
user design decision. Earlier lane-authority tests used Mac efs1, not NUC.

NUC initial state: four healthy nodes on loopback ports 17432–17435, two client
mounts; identical deployed f8bf47b4. Advertised logical capacity 500 GiB.
Configured storage: n1/n2 under /data1, n3/n4 under /data2. Actual /data2 resolves
onto the 70 GiB root filesystem (about 64 GiB free), unlike /data1's 745 GiB disk.
Configured per-node quotas are not proof that their backing filesystem has room.

## Rounds

1. Cold lane bootstrap wire operation (111/112): exact FileID and configured
   export geometry validated server-side, cold inode routing with dual-group
   forwarding, then a fresh lane-local view. Client redirects/BUSY/validation
   regression passes. FUSE admission remains staged.

2. Client admission view: ordinary requests read only the publication lane;
   only NOT_FOUND triggers one cold bootstrap call. BUSY, STALE and transport
   errors preserve the prior output and do not consult/adopt inode authority.

NUC private `/data1/efs/review/ten-rounds` full build and full `make test` passed
for the first cold-wire round. New export/geometry host and client admission
regressions pass locally. Live rollout/acceptance follows the implementation batch.

3. Lane/cold admission respects the inherited deadline before connection
   checkout, after receive and before committing output; BUSY backoff is clamped
   to remaining time. Fake-clock regressions verify short/expired budgets.
   Full socket/checkout whole-call bounds remain part of D27's remaining gate.

4. Durable LSM crash gate: COMMIT, ABORT and undecided PREPARE are reopened
   after child-process exit without store close. Unresolved lane authority is
   BUSY; resolution and inode active-lane registration survive another reopen.
   The new gate passed on NUC.

5. Snapshot alias guards reject partial body overlap and output overlap with
   state/plan before changing any storage. Real cache/state ASan/UBSan passes.

6. PUT binding rejects a same-mutation plan whose original ownership differs
   from accepted ranges, retaining bytes and leaving the publication unset.
   Older immutable snapshots remain allowed after concurrent admission.

7. Geometry-checked lane write boundary validates FileID and configured chunk
   size before allocation, ownership or copying. Invalid geometry/reserved fields
   preserve the body and sidecar.

8. Geometry-checked lane snapshot boundary combines lane authority with exact
   GETCHUNKS FileID/epoch. Mismatched base/geometry leaves snapshot outputs intact.

9. Caller-locked cache lane admission atomically records typed ownership and
   copies bytes, charging metadata before success, tracking dirty bytes and pins.
   Legacy accepted bytes cannot be adopted; both legacy flush paths return BUSY
   for typed ownership before GET/PUT. No FUSE caller activates this API yet.

10. Caller-locked cache lane snapshot captures byte ranges and body together,
    validating expected FileID and geometry. Pending publication blocks another
    snapshot. Real cache regressions cover rejection, retained ownership and
    successful immutable capture; ASan/UBSan passes.

NUC final build/test and live acceptance results follow below.

Final NUC private build `make -j8 -B all` and full `make test`: PASS.
Local real cache/state ASan/UBSan and legacy flush fence regressions: PASS.
Architecture documentation gate: 5/5. Live rollout results are recorded below.

Live NUC rollout: 79983128, four identical daemons, two remounted clients.
Both original mounts drained cleanly before server stop; no client force-discard.
The existing NUC stop script escalated node 1 to SIGKILL after its grace period;
recorded as an operational finding, not a clean daemon shutdown claim.
Build identity includes `-dirty` because Makefile counts untracked files in
`git status --porcelain`. Remote `git diff` and `git diff --cached` were empty;
only the six pre-existing untracked Mac test artifacts were listed.
Source revision is 79983128.

W36 `peer_rename_vs_unlink_src`: 20/20 PASS, first live gate after rollout.
TSVs saved under `tsv/w36-20261006-165927-*.tsv`. This NUC result does not close
the xorinox recurrence or substitute for its current-build gate.

Live full suites on 79983128:
- Single client: 216 PASS, 0 FAIL, 1 SKIP (`mmap_write_read`: unsupported).
- Two clients: 64/64 PASS.
- Persistence: 26/26 prepare and 26/26 verify PASS after clean unmount/remount.
- Final status: 4/4 healthy, identical versions, both mounts present.
TSVs retained alongside this checkpoint. No logical resize activation or
100/1000-client routing/load acceptance is claimed. D25 still needs FUSE entry
points, typed publication across both flush paths, retirement and live sweeps;
D27 still needs strict socket/checkout whole-call bounds and recorded load gates.

Commits: 34775d99 (rounds 1–2), fd3531b5 (3), f899fb6b (4–6),
79983128 (7–10). No required design decision arose during this batch.
