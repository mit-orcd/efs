# AMD integration and guarded-lease follow-up — Oct 11

## Scope and integration

`devel-amd` integrates every local branch, remote-tracking ref and detached
worktree tip captured at the start of this request. All 20 recorded tips are
ancestors of integration commit `3b3b0172`. No source conflict required choosing
one implementation over another; generated architecture HTML was regenerated
from merged sources. Shared branches and installed AMD data remain unchanged.
[Starting inventory](../../results/measure/20261011-amd-integration/starting-refs.json).

## W94 legacy guarded-lease fixture

The prior failed private fixture retains an actual CLOSE_STEP row for inode
45057, generation 1, original client UUID `39a7c4ddbdf87bb8962334ab0ad6da7c`
and sequence 12 on both inspected stopped replicas. The test used
`write_bytes()`, closing its real descriptor before attempting an absent-value
PREPARE for an artificial marker. Real CLOSE queues its own cleanup marker,
so that setup races bounded asynchronous GC rather than isolating the intended
prepared-marker guard. No marker was overwritten or deleted for this diagnosis.

The corrected fixture writes and fsyncs while retaining the real descriptor
through PREPARE, the expected legacy OPEN/BUSY, RESOLVE/ABORT and successful
legacy OPEN/CLOSE. It closes the descriptor afterward and retains the physical
reclamation oracle. The helper now reports exhaustion counts for connection,
transport, redirect, cleanup STALE/BUSY and session BUSY without extending or
weakening its existing retry budget. Actual live acceptance remains pending.

## Gates and remaining scope

The first merged normal run stopped at the documentation link gate because the
isolated deployment excluded committed `results/` evidence; this was a copy
omission, not a production compiler failure. The evidence is now copied and the
full normal/fault suites will be rerun. All five documentation checks pass in
the complete local worktree. Live validation uses private direct/buffered TCP
stores and the two permitted AMD mount paths, with restoration afterward.
RDMA, D25 activation and pushes remain outside this work.

W94 remains partial until the complete two-mode fault matrix, sustained-service
acceptance and full session/data-plane integration pass. W66's broader scaling
and D36 choices are unchanged.
