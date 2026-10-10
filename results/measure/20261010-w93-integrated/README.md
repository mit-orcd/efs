# W93 final branch integration — Oct 10

The completed `5b2df4bd32be` acceptance is retained in the neighboring
`20261010-w93-closure` directory. Concurrent changes arrived during that run.
`merge-tips.json` freezes the final integration boundary, including the new
`devel-intel` branch. This revalidation uses engine `37ad6a8790c8-integration`,
separate build/fixture roots and fresh service workload directories.

Native, full default/candidate durability/POSIX/peer, maximum KV/marker EIO,
partition and per-group cohort gates precede service deployment. The original
ABBA method is repeated on this integrated binary, with preserved service stores
and default restoration/cold checks. Acceptance remains pending until complete.

Remote roots: `/data1/efs/w93-integrated-build` and
`/data1/efs/w93-integrated-20261010`. Source/controller hashes are retained.

The first merged engine cdc5c265 passed native/partition checks but returned
EAGAIN in concurrent_appends (211/212). Retained logs identify a saturated
16-receipt window and one-second admission while holding the append stripe.
Commit 37ad6a87 retries before reservation outside that stripe within the
inherited deadline. Actual-source regression gates catch the old code.
Original failures and source provenance are retained separately.
