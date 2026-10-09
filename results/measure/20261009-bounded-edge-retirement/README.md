# Bounded staged lease-edge retirement

`efs_lease_guarded_retire` used to call whole-session lease deletion. It now
requires completed global revocation, local rejection floor, and no remaining
old-epoch lease/lock rows. Residual ownership returns BUSY; exact eight-row
cleanup is separate. Retirement respects prepared receipt ownership and deletes
one replay window, preserving durable epoch rejection.

AMD Linux full regression passes. Final session regression additionally proves
failed receipt deletion returns IO and retains the window for retry. Five fresh
child-process LSM opens prove eight-plus-one cleanup, retirement replay, and
old-epoch OPEN rejection after cold recovery. Prepared receipt ownership returns
BUSY. Previous session implementation fails the new relevant assertions (other
child failures in the recorded old run cascade from inherited failure counts).

This helper remains staged and assumes the KV includes session authority; a
cross-group production coordinator must check global authority independently.
Local FENCE_LOC already discards redundant edge replies after durable rejection;
this change prevents the explicit retirement helper from expanding into ownership
cleanup. It does not retire publication/append/PUT receipts or activate sessions.
No installed service or NUC changes; installed AMD mounts remain restored.
