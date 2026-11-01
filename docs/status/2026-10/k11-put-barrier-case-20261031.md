# K11 failed-PUT-barrier case runs again (Task CG, Oct 31)

**Purpose.** Make the F4 acceptance case of K11 ([definition](acceptance-definitions-20261031.md#k11)), `gc_reclamation.py --put-barrier-fault`, fail the PUT leg on today's code, and have the driver run it. Found by Task CB (its page `f4-loopback5-20261031.md`, merging in wave 49), where the case failed because the fault never fired.

**Verdict.** **PASS**: three direct runs and one driver run on intel1. K11 stays VERIFY (its other gates, `e1-manifest`, `e4-scale`, `posix-durability`, and decision BT-2 are open).

## Why the shim stopped firing (CB's hypothesis holds)

The old preload refused `fsync()` on regular files under `/data/exports/`. Since the unstable-PUT/COMMIT design (`735b82562` removed the A/B selectors; the per-root syncer is documented in `store.c` above `put_sync_attach`), a production PUT on a root with a durability record is acknowledged after `write` and `close` (`finish_fragment_put`, `finish_shard_write(fd, bytes, 0, 0)`, then `put_sync_note`). Durability is the per-root syncer's `syncfs(root fd)` followed by an `fdatasync` of `.efs-durable-epoch`, run by the `efsd` syncer thread when the epoch is 200 ms old or a client COMMIT asks. No data fragment `fsync` remained for the shim to refuse. A failed `syncfs` is sticky until restart: COMMIT and `fsync` return EIO.

## What changed

- `tests/live/store_barrier_fault.c`: interposes `syncfs()` instead of `fsync()`. It refuses (EIO) only on a descriptor whose directory holds `.efs-durable-epoch` (a fragment root) and only on the nodes the marker names (`all`, or `,N,` tokens); it prints the `test-store-barrier: refusing fragment syncfs` line once per daemon. No new daemon hook was needed: LD_PRELOAD reaches `syncfs` (dynamic libc) and `efsd` has no `test-store-barrier` environment of its own.
- `tests/live/put_barrier_fault.py`: cases `quorum-files` (node 1 fails, `fsync` succeeds), `partial-files` (nodes 1 and 2, EIO), `all-files` (EIO), in widening order because the barrier error is sticky. The `all-directories` sub-case is deleted: the PUT path has no directory barrier left. The retry `fsync` after removing the fault is deleted, since it cannot succeed before a restart (the fragments' incarnation changes, K35); recovery is the harness's existing orderly restart, fresh mount, exact cold read and drain.
- Assertion change: the old case required the failed span never to be published. Today the span is published first and `fsync` proves it afterwards (K35, `src/client/data_commit.c`), so a failed proof leaves `live_spans=1`. The case now asserts EIO (no success ACK), the retained bytes readable, the exact bytes after restart and the drained cold state, and records the span state in `put-barrier-fault.json`.
- `scripts/qualify.py` `b_gc`: the `gc-reclamation` gate now runs a second command, `gc-put-barrier` (`--put-barrier-fault`, own roots `p3`, `p4`), because the case owns its cluster and preload. `gc_args` stays `--posix`.
- The definition's `--mode direct` command is gone (the script has no such option; direct I/O is its only mode).

## Environment and reproduction

Tree: devel `8e53122cd6c9bdd56ac020f1c1be4a8a5e4445af` plus the files above (this commit), shipped with `git archive` plus those files (no `.git`, build id `unknown`), `make -j8` on host. Host intel1: 8 CPU, Rocky Linux, XFS `/home`, TCP, direct fragment I/O; four-member private 2+1 cluster per run, 1 GiB quota, two roots; ports 18750 (direct), 18700-18799 (driver).

```bash
# prerequisites: built tree, python3, cc, FUSE; run from the tree root with roots that exist
mkdir -p $D/p1 $D/p2
python3 tests/live/gc_reclamation.py --put-barrier-fault --root $D/p1 --root $D/p2 --port 18750
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json --topology loopback-5 \
  --gates gc-reclamation,fragment-integrity --allow all --out OUT --port-base 18780 --private-port-base 18700
```

Expect `failed server PUT barrier: ... PASS`, one `test-store-barrier: refusing fragment syncfs` line in three of the four `n*.log`, and `put-barrier-fault.json` with `fsync_errno` null, 5 (EIO), 5. Cleanup: the harness stops its daemons and unmounts; delete `$D`.

## Results

| Run | Verdict | Notes |
| --- | --- | --- |
| direct `--put-barrier-fault` x3 | PASS, PASS, PASS (rc 0) | about 100 s each; refusal line on n1, n2, n3; fsync errno None / 5 / 5 |
| driver `gc-reclamation` (now `--posix` plus `gc-put-barrier`) | PASS | 3 min 50 s; the case line PASS in `10-gc-put-barrier.log` |
| driver `fragment-integrity` | PASS | 14 s |
| `tests/test_qualify_driver.py` on intel1 | PASS | the test needs `/proc`, so it does not run on macOS |

The driver's overall status is NOTRUN (exit 3) because the `identity` gate was not selected, as in Task CB. No C code changed, so the budgeted gate was not rerun.

## Finding for the register (not fixed here)

While the barrier is refused, the `efsd` syncer retried `syncfs` thousands of times per second per failing root (the first shim version logged every call: over 15 000 lines per second per node). A failed barrier keeps its epoch "due" and the loop has no back-off. Not part of K11; worth a K row and a pause after a failed barrier.

## Artifacts

`intel1:/home/maik/efs-artifacts/taskCG-20261031/` (2.2 MB, 80 files: `direct/run1-3.log`, `qualify/`, `driver.log`, `build.log`, `test_qualify_driver.log`). `SHA256SUMS` there has sha256 `ff30bda03196814f715cb5c1057916c5a2cf1c968724365c247bb6871d9df0ba`.

## Limitations

One host, TCP, loopback. K11 is not VERIFIED by this; it also needs the cell gates and BT-2. The case no longer proves "never published"; it proves "never acknowledged and never lost".
