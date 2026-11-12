# K123: TSan `k37_segment_rebuild` failures were test races (Task GF)

Verdict: **PASS** (K123 cause found and fixed in the test; blocks alpha: no).
Linked item: K123 in [the checklist](../alpha-1.0-2plus1-checklist.md).
Current state: both signatures are test defects, not product defects. (1)
`packer moved 0 of 4` / `1 of 4`: step 5 of `tests/test_k37_segment_rebuild.py`
took its packed-files counter *after* `restart(3)` and `up(c, 3)`, but the
packer moves the refused files within milliseconds of node 3 listening, before
`up()` returns, so the counter already included them and the wait for "+4"
could never be met. (2) `Protection: full` and `Cluster state: OK` not both
shown: step 3 waited for the degraded-segment count to reach 0 and then read
the status once, while one degraded chunk was still being rebuilt. The test
now keeps the earlier counter and waits on the status it asserts. 60 of 60
runs pass on the fixed tree under the conditions in which 8 of 60 failed
before. Signature 2 was not reproduced in 120 runs; its cause rests on the
wave 80 status text. No product change.

## Run table (TSan, entry alone, host amd, 1 to 3 loops in parallel)

| Tree | Condition | Runs | Pass | Fail | Signature of the failures |
|---|---|---|---|---|---|
| devel `60c9fcdad` | one loop, host otherwise quiet | 8 | 8 | 0 | |
| devel `60c9fcdad` | three loops in parallel | 52 | 44 | 8 | signature 1 only |
| fixed | three loops in parallel | 60 | 60 | 0 | |

The 8 failures: 6 `moved 0 of 4`, 2 `moved 1 of 4`. Signature 2 did not occur.

## Diagnosis

Signature 1 (`tests/test_k37_segment_rebuild.py`, step 5, `wait_packed`).
In a failing run (a9) node 3 restarted at 04:53:33.316; the packer on node 1
logged `written degraded ... packed_total=56` (52 before, 4 files of the
batch `one`) at 04:53:33.309, 33 ms before node 3's `raft-host: up` line
(04:53:33.342), and no packer pass logged anything afterwards for the 120 s of
the wait. The test then read `base = packed_total(c)` after `up()`, so
`base` was already 56 and "base + 4" was unreachable. The same order holds in b4 (`written degraded` 04:58:41.675, node 3 up
04:58:41.712) and c1 (04:55:22.725, node 3 up 04:55:22.760): the pass that
moved the files sits before node 3's raft-up line. The
packer is not late: it runs a pass every 200 ms (`EFS_FAULT_PACK_INTERVAL_MS`,
`pk_main` in `src/server/packer.c`), and nothing in it waits for an event
from membership; the 2 s failure backoff (`pk_group`, `backoff_until_ns`) had
expired during the stop. Under TSan the restart and `up()` take longer than
the packer's 200 ms pass, which is why only the sanitized lane showed it.
Test defect.

Signature 2 (step 3, last `assert 'Protection: full' ... in st`). The wave 80
failure text shows `Degraded segments: 0 ... rebuilt=3`, `Degraded chunks: 1
(group 0: 1)` and `Cluster state: DEGRADED (3 up, 0 down)`: `wait_segs`
returns when the segment count is 0, the status is read once, and the chunk
rebuild had not finished. The wait looks at the segments only; the assertion
reads chunks too. Test defect.

## Change

`tests/test_k37_segment_rebuild.py`: step 5 waits with the `base` taken while
the files were refused (no new count after the restart); step 3 waits for
`Protection: full` and `Cluster state: OK` with `wait_status` (the 60 s bound
of `wait_up_status`, which now calls it). No timeout, bound or sleep was
raised or added.

## Verification

Commands, on amd with `TMPDIR=~/efs-taskGF/tmp`, from a `git archive` copy:
`EFS_SAN_ONLY=k37_segment_rebuild EFS_SAN_LOGDIR=<dir> bash tests/sanitizer_check.sh tsan`
once to build, then `EFS_SAN_BUILD=<dir>/build` with the same `EFS_SAN_ONLY`
in loops (`gfloop.sh`, three loops of 20 in parallel for the fixed tree).
Results of `make`, `make test`, the test itself, `scripts/tests_for.py
server/packer --run`, `tests/test_make_test_recipe.py` and the whole ASan and
TSan lists: all returned 0 on amd. Raw run output (tables, the eight failing
runs, daemon logs of three of them, loop scripts) is in
`~maik/efs-artifacts/taskGF-20261111/` on amd, with a `SHA256SUMS` file; the
scratch tree `~/efs-taskGF` was deleted.

## Not verified / limits

- Signature 2 was never reproduced; the fix follows from the status text of
  the wave 80 lane.
- Step 5 now also accepts the four files being packed (degraded, node 3 down)
  right after the refusal ends and before node 3 returns; step 6's
  `wait_segs` still requires the rebuild.
- Runs were on one host (amd) with the coordinator's lanes possible beside
  them; durations are not absolute.
