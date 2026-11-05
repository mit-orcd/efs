# K91: `efs-mgmt status` said `Protection: unknown` on a healthy cluster (Task DJ)

**Verdict: PASS, K91 FIXED, blocks alpha: no.** Under an 8-job gate on fstor007 the pre-fix tree failed `tests/test_put_quota_reclaim.py` in 1 of 160 copies with the gate's message (`Degraded segments: unknown (group leaders reported: 0)`, `Protection: unknown`, every node up, both group leaders elected). The fixed tree passed 160 of 160 under the same load. With the rebuild passes held from cluster start (new step 0 of `tests/test_k38_protection_now.py`), the pre-fix tree fails at once and the fixed tree passes. Status never said `full` for a degraded stripe. It said `unknown` for a healthy one, so alpha is not blocked.

Purpose: Task DJ, the intermittent `test_put_quota_reclaim.py` failure that waves 55 and 57 each hit once under 8-job gate load. Tested source: `6138995df` on devel `104eb5e10` (`git archive` builds, so the build id reads `unknown`). Later commits on the branch change docs, the K rows and the graph only.

Environment: fstor007 (96 cpus, private libfuse shim), private loopback clusters of three members (EFS_FAULTS `efsd`, TCP, buffered I/O, node 3 quota 16 MiB in the reclaim test) under `/tmp/efs-taskDJ-tmp`. Load: `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` (the full `test-all` gate) running alongside `scripts/evidence/dj/status_protection_load.sh TREE OUT 20 8`, which runs 20 rounds of 8 parallel copies of the reclaim test. Load average 7 to 11 during the runs. Other tasks (DH, DK) shared the host.

## Cause

- `efs-mgmt status` (`src/mgmt/efs_mgmt.c`) prints protection as `unknown` until both metadata groups have a node whose STATUS reply sets the group's `degraded_sampled` and `segment_sampled` bits.
- Before the fix, the STATUS handler (`src/server/handler.c`) copied those bits from server fields. Only the leader's W154 chunk rebuild pass (`host_rebuild_pass`, `src/server/raft_host.c`) and its K37 segment pass (`server_segment_rebuild_pass`) set them. The GC thread cleared them on a non-leader. K38 already counted the marks at the reply, but only for groups whose bit was set.
- The two passes are the last steps of each GC loop iteration, after expiry, reap, truncate, fragment and pack-retire work, and the loop idles up to 1 s (`GC_LOOP_MS`) between iterations. A newly elected leader answers status from its first moment, but it reports its groups only after its first full iteration. Under gate load that iteration ran seconds late, and the reclaim test asked for status about 5 s after start.
- The failing copy shows `Degraded chunks: 0 (group 0: 0, group 2: 0)`, so the chunk pass had run on both leaders, but `Degraded segments: unknown (group leaders reported: 0)`: group 2's leader had not yet reached its segment pass, which runs after the chunk pass in the same iteration.

## Semantics chosen

The task allowed three choices: compute at the request, run a first pass on election, or tell "first pass pending" apart from "unknown". The status answer now comes from the request. For each metadata group, the STATUS handler calls `server_raft_host_degraded_now`. If this node leads the group, that function does a ReadIndex (`host_read_index`, which waits until every entry committed at the read point is applied locally), then reads the chunk-mark count. It uses the apply-kept count (`efs_meta_degraded_count_peek`), or a prefix scan when the count has not been derived yet; the handler does not derive it, because the GC pass owns that. The handler then counts the segment marks (`server_segment_degraded_now`) and sets the reply's bits.

- Truthful: the counts include every mark committed before the request, from any earlier leader. Nothing depends on a pass having sampled. This also covers the K38 case (a mark committed after the last pass).
- `unknown` keeps one meaning: a group has no leader that could prove its leadership at this probe (election in progress, or a ReadIndex that failed). Neither of the other two choices would have shown `full` any sooner without a count behind it.
- Cost: one ReadIndex per led group per STATUS request. Status is an operator command. Data-path reads already use the same ReadIndex.
- Removed as dead: the server fields `degraded_chunks`, `degraded_sampled_mask`, `degraded_segments`, `unrecoverable_segments` and `segment_sampled_mask`, with their stores in the passes and the GC thread. The wire reply and the status text are unchanged.

## Results

| Check | Pre-fix (`104eb5e10`) | Fixed (`6138995df`) |
|---|---|---|
| `test_put_quota_reclaim.py`, 20 rounds × 8 copies alongside the 8-job gate, fstor007 | 1 of 160 failed with the gate's message (r10-c2, 4.4 s). One more copy (r1-c6) failed `PermissionError` on `efs-mgmt`, which the gate was relinking (an artifact of my setup, excluded) | 0 of 160 failed (7.9 to 10.4 s, median 9.1 s; pre-fix median 9.1 s) |
| Same, 8 then 32 copies, no gate (load 1 to 28, CPU only) | 0 of 40, 0 of 96 | not run |
| `test_k38_protection_now.py` step 0 (passes held from start) | FAIL at 3 s: `Protection: unknown`, `group leaders reported: none` | PASS, whole test 25.9 s |
| 8-job gate on the same host | `TOTAL 589 FAIL 1` (the new step 0 only; the updated test was copied into the pre-fix tree) | with the extra copies: `TOTAL 589 FAIL 0`, OVERBUDGET `test_k38_protection_now` 51.7 s of 30 s; plain gate: `TOTAL 589 FAIL 0 NOTRUN 3 (682s) OVERBUDGET 1`, `test_k38_protection_now` 22.4 s, `test_put_quota_reclaim` 7.9 s, OVERBUDGET `test_k36_recovery` 63.2 s of 55 s |

- The overbudget rows are load, not this change. With the gate plus the extra reclaim copies, `test_k38_protection_now` took 51.7 s. Solo on the quiet host, run side by side three times, the fixed test took 22.7, 24.4 and 23.7 s, and the original test on the pre-fix tree took 23.9, 23.8 and 23.8 s. The plain gate took 22.4 s. `test_k36_recovery` (two status calls) run side by side twice: pre-fix 53.2 and 38.1 s, fixed 43.1 and 51.0 s, so the same spread on a host that Tasks DH and DK shared. No budget was changed.
- CPU load alone did not reproduce it. What delays the first pass is slow disk I/O in the earlier GC stages, which the gate's I/O-heavy tests supply.
- TSan: `EFS_SAN_LIST` with `test_k38_protection_now.py` and `test_put_quota_reclaim.py` on a `-fsanitize=thread` build (`test-bins tests/faults/efsd`): `PASS k38_protection_now (32s)`, `PASS put_quota_reclaim (20s)`, `VERDICT: PASS`. The new path takes `h->mu`, then the ReadIndex wait, then `dg_wm_mu` from a handler thread; TSan reported no race.
- `make test`: rc 0 in 107 s on fstor007. `python3 tests/test_make_test_recipe.py`: PASS. `python3 scripts/check_k_register.py`: PASS, 91 rows.

## Raw data

Under `~/efs-artifacts/taskDJ-20261105/` on the Mac: per-copy `summary.tsv` files and the failing copies' logs, the gate summaries and the K38 logs. `SHA256SUMS` sha256: `6546f7d70e1534564f81c308abf704fe4e55879190994add28dc1bc72b20c961`. The `/tmp/efs-taskDJ*` trees on fstor007 were removed.

## Reproduction

```bash
make -j32 test-bins tests/faults/efsd
python3 tests/test_k38_protection_now.py
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh &
sh scripts/evidence/dj/status_protection_load.sh "$PWD" /tmp/dj-load 20 8
```

Expected: step 0 prints `0. leaders that never ran a rebuild pass: every member reports full protection PASS`, and `verdict.txt` reads `copies=160 failed=0`. Remove `/tmp/dj-load` afterwards. Remaining gates: none for K91. A real-host run is not owed, because the defect is the order of work in one daemon.
